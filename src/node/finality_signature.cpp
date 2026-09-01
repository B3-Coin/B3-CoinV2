// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/finality_signature.h>

#include <chain.h>
#include <consensus/era.h>
#include <logging.h>
#include <modern/chain_domain.h>
#include <modern/finality_schedule.h>
#include <node/bridge_state.h>
#include <node/validator_set.h>

#include <algorithm>

namespace node {

const char* FinalitySignaturePool::AcceptName(const Accept a)
{
    switch (a) {
    case Accept::ACCEPTED: return "accepted";
    case Accept::DUPLICATE: return "duplicate";
    case Accept::STALE: return "stale";
    case Accept::UNKNOWN_EPOCH: return "unknown-epoch";
    case Accept::NOT_CHECKPOINT: return "not-checkpoint";
    case Accept::TOO_SHALLOW: return "too-shallow";
    case Accept::BAD_INDEX: return "bad-index";
    case Accept::POOL_FULL: return "pool-full";
    case Accept::BAD_SIGNATURE: return "bad-signature";
    }
    return "unknown";
}

namespace {

//! The signing set for `epoch` in `state` (current or previous), else null.
const ValidatorSetSnapshot* SetForEpoch(const FinalityTracker::State& state, const uint64_t epoch)
{
    if (!state.bootstrapped || state.lineage_broken) return nullptr;
    if (epoch == state.epoch) return state.current.get();
    if (state.epoch >= 1 && epoch == state.epoch - 1) return state.previous.get();
    return nullptr;
}

//! hash(Set_{epoch+1}) as derived on this chain.
std::optional<uint256> SuccessorHashForEpoch(const FinalityTracker::State& state, const uint64_t epoch)
{
    if (epoch == state.epoch && state.next) return state.next->SetHash();
    if (state.epoch >= 1 && epoch == state.epoch - 1 && state.current) return state.current->SetHash();
    return std::nullopt;
}

} // namespace

std::optional<modern::FinalizedBlock> FinalitySignaturePool::ExpectedFinalizedBlock(const uint64_t epoch,
                                                                                    const uint64_t height,
                                                                                    const FinalityTracker::State& state,
                                                                                    const CChain& chain,
                                                                                    const Consensus::Params& params,
                                                                                    const BridgeStateIndex* bridge_index)
{
    const auto successor{SuccessorHashForEpoch(state, epoch)};
    const CBlockIndex* index{height <= static_cast<uint64_t>(std::numeric_limits<int>::max())
                                 ? chain[static_cast<int>(height)]
                                 : nullptr};
    if (!successor || !index) return std::nullopt;
    modern::FinalizedBlock fb;
    fb.height = height;
    fb.block_hash = index->GetBlockHash();
    const auto withdrawal_root{FinalityWithdrawalRoot(
        static_cast<int>(height), params, bridge_index)};
    if (!withdrawal_root) return std::nullopt;
    fb.withdrawal_root = *withdrawal_root;
    fb.validator_set_hash = *successor;
    fb.epoch = epoch;
    return fb;
}

FinalitySignaturePool::Accept FinalitySignaturePool::Submit(const FinalitySig& sig, const FinalityTracker& tracker,
                                                            const CChain& chain, const Consensus::Params& params,
                                                            const BridgeStateIndex* bridge_index)
{
    // Cheap, state-free checks first.
    if (!params.legacy_b3coin || !params.modern_pos) return Accept::STALE;
    const std::optional<int> modern_start{Consensus::ModernPosStartHeight(params)};
    const CBlockIndex* tip{chain.Tip()};
    if (!modern_start || !tip) return Accept::STALE;
    if (sig.height > static_cast<uint64_t>(tip->nHeight)) return Accept::NOT_CHECKPOINT;
    const int h{static_cast<int>(sig.height)};
    const Consensus::ModernPosParams& pos{*params.modern_pos};
    if (!modern::IsCheckpointHeight(h, *modern_start, pos.checkpoint_interval)) return Accept::NOT_CHECKPOINT;
    if (!modern::CheckpointDepthSatisfied(h, tip->nHeight, pos.checkpoint_depth)) return Accept::TOO_SHALLOW;

    const FinalityTracker::State& state{tracker.Current()};
    if (state.finalized && sig.height <= static_cast<uint64_t>(state.finalized->height)) {
        Prune(state.finalized->height);
        return Accept::STALE;
    }
    const ValidatorSetSnapshot* set{SetForEpoch(state, sig.epoch)};
    if (!set) return Accept::UNKNOWN_EPOCH;
    // The checkpoint must lie in the span of the claimed epoch.
    const auto epoch_of_h{modern::EpochOfHeight(state.epoch_starts, h)};
    if (!epoch_of_h || *epoch_of_h != sig.epoch) return Accept::NOT_CHECKPOINT;
    if (sig.index >= set->Size()) return Accept::BAD_INDEX;

    const auto key{std::make_pair(sig.epoch, sig.height)};
    auto slot_it{m_slots.find(key)};
    if (slot_it != m_slots.end() && slot_it->second.sigs.count(sig.index)) return Accept::DUPLICATE;
    if (slot_it == m_slots.end() && m_slots.size() >= MAX_TRACKED_CHECKPOINTS) return Accept::POOL_FULL;

    // Expensive part last: reconstruct the digest and verify one signature.
    const auto fb{ExpectedFinalizedBlock(sig.epoch, sig.height, state, chain,
                                         params, bridge_index)};
    const auto domain{params.legacy_final_hash
                          ? modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)
                          : std::nullopt};
    if (!fb || !domain) return Accept::STALE;
    const auto decoded{bls::Signature::Decode(sig.signature)};
    if (!decoded) return Accept::BAD_SIGNATURE;
    const uint256 digest{modern::FinalityDigest(*domain, *fb)};
    // Provenance: the member key passed its PoP in consensus at binding time.
    const auto& member_key{set->View().keys[sig.index]};
    if (!bls::Verify(member_key.Key(), std::span<const unsigned char>(digest.begin(), 32), *decoded)) {
        return Accept::BAD_SIGNATURE;
    }
    m_slots[key].sigs[sig.index] = sig.signature;
    return Accept::ACCEPTED;
}

std::optional<std::pair<modern::FinalizedBlock, modern::FinalityCertificate>>
FinalitySignaturePool::BestCertificate(const FinalityTracker& tracker, const CChain& chain,
                                       const Consensus::Params& params,
                                       const BridgeStateIndex* bridge_index) const
{
    const FinalityTracker::State& state{tracker.Current()};
    // Highest height first; prefer the newest epoch at equal height.
    for (auto it{m_slots.rbegin()}; it != m_slots.rend(); ++it) {
        const auto& [epoch, height]{it->first};
        if (state.finalized && height <= static_cast<uint64_t>(state.finalized->height)) continue;
        const ValidatorSetSnapshot* set{SetForEpoch(state, epoch)};
        if (!set) continue;
        uint64_t weight{0};
        for (const auto& [index, sig] : it->second.sigs) {
            if (index < set->Size()) weight += set->Members()[index].weight;
        }
        if (weight < set->QuorumWeight()) continue;
        const auto fb{ExpectedFinalizedBlock(epoch, height, state, chain,
                                             params, bridge_index)};
        if (!fb) continue;
        modern::FinalityCertificate cert;
        cert.signer_bitmap.assign(modern::SignerBitmapBytes(set->Size()), 0);
        std::vector<bls::Signature> sigs;
        for (const auto& [index, sig_bytes] : it->second.sigs) {
            if (index >= set->Size()) continue;
            const auto decoded{bls::Signature::Decode(sig_bytes)};
            if (!decoded) continue; // cannot happen: verified on submit
            cert.signer_bitmap[index / 8] |= static_cast<unsigned char>(1u << (index % 8));
            sigs.push_back(*decoded);
        }
        const auto aggregate{bls::AggregateSignatures(sigs)};
        if (!aggregate) continue;
        cert.aggregate_sig = aggregate->Compressed();
        return std::make_pair(*fb, std::move(cert));
    }
    return std::nullopt;
}

void FinalitySignaturePool::Prune(const int finalized_height)
{
    for (auto it{m_slots.begin()}; it != m_slots.end();) {
        if (it->first.second <= static_cast<uint64_t>(finalized_height)) {
            it = m_slots.erase(it);
        } else {
            ++it;
        }
    }
}

size_t FinalitySignaturePool::SignatureCount(const uint64_t epoch, const uint64_t height) const
{
    const auto it{m_slots.find(std::make_pair(epoch, height))};
    return it == m_slots.end() ? 0 : it->second.sigs.size();
}

std::vector<FinalitySig> FinalitySigner::MaybeSign(const FinalityTracker& tracker, const CChain& chain,
                                                   const Consensus::Params& params, FinalitySignaturePool& pool,
                                                   const BridgeStateIndex* bridge_index)
{
    std::vector<FinalitySig> out;
    if (!m_key || !params.legacy_b3coin || !params.modern_pos) return out;
    const std::optional<int> modern_start{Consensus::ModernPosStartHeight(params)};
    const CBlockIndex* tip{chain.Tip()};
    if (!modern_start || !tip) return out;
    const Consensus::ModernPosParams& pos{*params.modern_pos};
    const FinalityTracker::State& state{tracker.Current()};
    if (!state.bootstrapped || state.lineage_broken) return out;

    const int deepest{state.finalized ? std::max(state.finalized->height, m_last_signed) : m_last_signed};
    const int signable_to{tip->nHeight - pos.checkpoint_depth};
    // First scheduled checkpoint strictly above everything signed/final.
    int h{*modern_start};
    if (deepest >= *modern_start) {
        h = deepest + 1;
        const int rem{(h - *modern_start) % pos.checkpoint_interval};
        if (rem != 0) h += pos.checkpoint_interval - rem;
    }
    const auto pk{m_key->GetPublicKey().Compressed()};
    for (; h <= signable_to; h += pos.checkpoint_interval) {
        const auto epoch{modern::EpochOfHeight(state.epoch_starts, h)};
        if (!epoch) continue;
        const ValidatorSetSnapshot* set{SetForEpoch(state, *epoch)};
        if (!set) continue;
        const auto index{set->IndexOf(m_validator_key)};
        // Not a member of the set in force, or the snapshot records a
        // different (pre-rotation) BLS key than ours: do not sign.
        if (!index || set->Members()[*index].bls_pubkey != pk) continue;
        const auto fb{FinalitySignaturePool::ExpectedFinalizedBlock(
            *epoch, static_cast<uint64_t>(h), state, chain, params,
            bridge_index)};
        const auto domain{params.legacy_final_hash
                              ? modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)
                              : std::nullopt};
        if (!fb || !domain) continue;
        const uint256 digest{modern::FinalityDigest(*domain, *fb)};
        FinalitySig sig;
        sig.epoch = *epoch;
        sig.height = static_cast<uint64_t>(h);
        sig.index = *index;
        sig.signature = m_key->Sign(std::span<const unsigned char>(digest.begin(), 32)).Compressed();
        // One signature per checkpoint, strictly increasing heights.
        m_last_signed = h;
        pool.Submit(sig, tracker, chain, params, bridge_index);
        out.push_back(std::move(sig));
    }
    return out;
}

} // namespace node
