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
    const FinalityTracker::State& state{tracker.Current()};
    // Reclaim finalized slots on every submission, including malformed or
    // stale messages. A valid next-checkpoint signature must never encounter
    // capacity retained solely by normal finality progress.
    if (state.finalized) Prune(state.finalized->height);

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

    if (state.finalized && sig.height <= static_cast<uint64_t>(state.finalized->height)) {
        return Accept::STALE;
    }
    const ValidatorSetSnapshot* set{SetForEpoch(state, sig.epoch)};
    if (!set) return Accept::UNKNOWN_EPOCH;
    // The checkpoint must lie in the span of the claimed epoch.
    const auto epoch_of_h{modern::EpochOfHeight(state.epoch_starts, h)};
    if (!epoch_of_h || *epoch_of_h != sig.epoch) return Accept::NOT_CHECKPOINT;
    if (sig.index >= set->Size()) return Accept::BAD_INDEX;

    // Reconstruct the exact signed object before deduplication. Coordinates
    // alone do not identify a branch: a permitted pre-finality reorg can change
    // the checkpoint hash (or withdrawal/successor root) at the same
    // (epoch,height).
    const auto fb{ExpectedFinalizedBlock(sig.epoch, sig.height, state, chain,
                                         params, bridge_index)};
    const auto domain{params.legacy_final_hash
                          ? modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)
                          : std::nullopt};
    if (!fb || !domain) return Accept::STALE;
    const uint256 digest{modern::FinalityDigest(*domain, *fb)};

    // Remove every bounded slot whose exact signed object is no longer
    // derivable on this branch. Besides fixing the incoming same-coordinate
    // case, this prevents a series of reorgs from consuming all eight slots.
    for (auto it{m_slots.begin()}; it != m_slots.end();) {
        const auto expected{ExpectedFinalizedBlock(
            it->first.first, it->first.second, state, chain, params,
            bridge_index)};
        if (!expected ||
            modern::FinalityDigest(*domain, *expected) != it->second.digest) {
            it = m_slots.erase(it);
        } else {
            ++it;
        }
    }

    const auto key{std::make_pair(sig.epoch, sig.height)};
    auto slot_it{m_slots.find(key)};
    if (slot_it != m_slots.end() &&
        slot_it->second.sigs.count(sig.index)) {
        return Accept::DUPLICATE;
    }
    // A prolonged quorum outage must not freeze the pool on its first eight
    // checkpoints forever. A newer valid checkpoint replaces the oldest
    // bounded slot; an old replay cannot evict newer work. Defer the actual
    // eviction until after BLS verification so garbage cannot churn the pool.
    const bool evict_oldest{
        slot_it == m_slots.end() &&
        m_slots.size() >= MAX_TRACKED_CHECKPOINTS};
    if (evict_oldest && key <= m_slots.begin()->first) {
        return Accept::POOL_FULL;
    }

    // Expensive BLS verification remains last.
    const auto decoded{bls::Signature::Decode(sig.signature)};
    if (!decoded) return Accept::BAD_SIGNATURE;
    // Provenance: the member key passed its PoP in consensus at binding time.
    const auto& member_key{set->View().keys[sig.index]};
    if (!bls::Verify(member_key.Key(), std::span<const unsigned char>(digest.begin(), 32), *decoded)) {
        return Accept::BAD_SIGNATURE;
    }
    if (evict_oldest) m_slots.erase(m_slots.begin());
    Slot& slot{m_slots[key]};
    if (slot.sigs.empty()) slot.digest = digest;
    if (slot.digest != digest) return Accept::STALE; // defensive; reset above
    slot.sigs[sig.index] = sig.signature;
    return Accept::ACCEPTED;
}

std::optional<std::pair<modern::FinalizedBlock, modern::FinalityCertificate>>
FinalitySignaturePool::BestCertificate(const FinalityTracker& tracker, const CChain& chain,
                                       const Consensus::Params& params,
                                       const BridgeStateIndex* bridge_index) const
{
    const FinalityTracker::State& state{tracker.Current()};
    const auto domain{params.legacy_final_hash
                          ? modern::ModernChainDomain(params.hashGenesisBlock,
                                                      *params.legacy_final_hash)
                          : std::nullopt};
    if (!domain) return std::nullopt;
    // Highest height first; prefer the newest epoch at equal height.
    for (auto it{m_slots.rbegin()}; it != m_slots.rend(); ++it) {
        const auto& [epoch, height]{it->first};
        if (state.finalized && height <= static_cast<uint64_t>(state.finalized->height)) continue;
        const ValidatorSetSnapshot* set{SetForEpoch(state, epoch)};
        if (!set) continue;
        uint64_t weight{0};
        uint32_t signer_count{0};
        for (const auto& [index, sig] : it->second.sigs) {
            if (index < set->Size()) {
                weight += set->Members()[index].weight;
                ++signer_count;
            }
        }
        if (weight < set->QuorumWeight() ||
            signer_count <
                modern::FinalityHeadcountQuorum(
                    static_cast<uint32_t>(set->Size()))) {
            continue;
        }
        const auto fb{ExpectedFinalizedBlock(epoch, height, state, chain,
                                             params, bridge_index)};
        if (!fb || modern::FinalityDigest(*domain, *fb) != it->second.digest) {
            continue;
        }
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

bool FinalitySigner::SetKeyPersistent(
    const bls::SecretKey& key,
    const modern::ValidatorKeyBytes& validator_key,
    const uint256& chain_domain, const fs::path& store_directory,
    std::string& error)
{
    FinalitySignerStore store;
    if (!store.Open(store_directory, chain_domain, validator_key, error)) {
        return false;
    }
    m_key = key;
    m_validator_key = validator_key;
    m_store = std::move(store);
    m_last_signed = m_store.State()
                        ? m_store.State()->last_signed_height
                        : -1;
    m_error.clear();
    m_permanent_error = false;
    return true;
}

void FinalitySigner::Fail(std::string error, const bool permanent)
{
    if (m_error != error) {
        LogError("finality signer: disabled safely: %s", error);
    }
    m_error = std::move(error);
    m_permanent_error = m_permanent_error || permanent;
}

bool FinalitySigner::EnsurePersistentSafety(
    const FinalityTracker& tracker, const CChain& chain,
    const Consensus::Params& params,
    const BridgeStateIndex* bridge_index)
{
    if (!m_store.IsOpen()) return true; // unit-test/offline signer
    if (m_permanent_error) return false;

    const auto configured_domain{
        params.legacy_final_hash
            ? modern::ModernChainDomain(params.hashGenesisBlock,
                                        *params.legacy_final_hash)
            : std::nullopt};
    if (!configured_domain ||
        *configured_domain != m_store.ChainDomain()) {
        Fail("configured chain domain no longer matches the signer journal");
        return false;
    }

    const CBlockIndex* tip{chain.Tip()};
    const std::optional<int> modern_start{
        Consensus::ModernPosStartHeight(params)};
    if (!tip || !modern_start || !params.modern_pos) {
        Fail("finality schedule is not configured");
        return false;
    }
    const Consensus::ModernPosParams& pos{*params.modern_pos};
    const FinalityTracker::State& state{tracker.Current()};

    // An absent file is ambiguous whenever this validator could still have a
    // live unrecorded vote: it might be a fresh install, or a deleted
    // anti-equivocation record. Before the first included finality pin, any
    // globally signable checkpoint is ambiguous because a competing corridor
    // fork may have a disjoint Set0. After that pin, a genuine newcomer which
    // belongs to neither accepted signing set can safely establish its empty
    // marker before entering a future set.
    if (m_store.IsAbsent()) {
        const int signable_to{tip->nHeight - pos.checkpoint_depth};
        int highest_signable{-1};
        if (signable_to >= *modern_start) {
            highest_signable =
                signable_to - ((signable_to - *modern_start) %
                               pos.checkpoint_interval);
        }
        bool may_hide_live_vote{false};
        if (highest_signable >= *modern_start) {
            if (!state.finalized) {
                // Before the first included certificate, a missing journal
                // could hide a vote made by this validator on a competing
                // corridor-derived Set_0, even when it is not a member of the
                // Set_0 visible on this branch.
                may_hide_live_vote = true;
            } else {
                // Once B3 has an included finality pin, only unfinalized
                // checkpoints in the current/current-1 acceptance window can
                // still use this validator. This lets a genuine newcomer arm
                // safely before entering a future set, without treating every
                // post-M validator as though it had deleted an old journal.
                int h{std::max(*modern_start,
                               state.finalized->height + 1)};
                const int rem{(h - *modern_start) % pos.checkpoint_interval};
                if (rem != 0) h += pos.checkpoint_interval - rem;
                for (; h <= highest_signable;
                     h += pos.checkpoint_interval) {
                    const auto epoch{
                        modern::EpochOfHeight(state.epoch_starts, h)};
                    const ValidatorSetSnapshot* set{
                        epoch ? SetForEpoch(state, *epoch) : nullptr};
                    if (set && set->IndexOf(m_validator_key)) {
                        may_hide_live_vote = true;
                        break;
                    }
                }
            }
        }
        std::string error;
        if (!may_hide_live_vote) {
            if (!m_store.InitializeEmpty(error)) {
                Fail(strprintf("cannot initialize durable signer state: %s",
                               error));
                return false;
            }
        } else {
            Fail(strprintf(
                "durable signer state is absent after checkpoint %d became signable; refusing to recreate a possibly deleted anti-equivocation record",
                highest_signable),
                /*permanent=*/false);
            return false;
        }
    }

    const FinalitySignerState& persisted{*m_store.State()};
    m_last_signed = persisted.last_signed_height;
    if (persisted.lock_height < 0) {
        m_error.clear();
        return true;
    }

    const CBlockIndex* locked{chain[persisted.lock_height]};
    if (locked && locked->GetBlockHash() == persisted.lock_block_hash) {
        m_error.clear();
        return true;
    }

    // The voted branch disappeared. A timeout, a deeper tip, or an operator
    // action is not an unlock proof. The sole V1 lock-change proof is a
    // strictly newer quorum certificate which consensus already validated and
    // included on this active chain. Quorum intersection then makes a second
    // conflicting certificate impossible under the protocol fault bound.
    if (!state.finalized ||
        state.finalized->height <= persisted.lock_height ||
        state.finalized->certified_at <= state.finalized->height ||
        !chain[state.finalized->certified_at]) {
        Fail(strprintf(
            "active chain does not descend from signed checkpoint %d %s and has no newer included finality certificate; refusing to vote on this fork",
            persisted.lock_height,
            persisted.lock_block_hash.ToString()),
            /*permanent=*/false);
        return false;
    }
    const auto finalized{FinalitySignaturePool::ExpectedFinalizedBlock(
        state.finalized->epoch,
        static_cast<uint64_t>(state.finalized->height), state, chain,
        params, bridge_index)};
    if (!finalized || finalized->block_hash != state.finalized->block_hash) {
        Fail("cannot reconstruct the included certificate used to move the ancestry lock",
             /*permanent=*/false);
        return false;
    }
    const ValidatorSetSnapshot* finalized_set{
        SetForEpoch(state, state.finalized->epoch)};
    if (!finalized_set) {
        Fail("cannot reconstruct the signing set used by the included certificate",
             /*permanent=*/false);
        return false;
    }
    // Quorum intersection is only guaranteed within the exact same signing
    // set. The epoch-e handover certificate may predate this orphaned vote,
    // and Set_{e+1} may be disjoint, so a successor-epoch certificate is not
    // an unlock proof even when the committed lineage hashes match.
    if (state.finalized->epoch != persisted.lock_epoch ||
        finalized_set->SetHash() != persisted.lock_signing_set_hash) {
        Fail("included certificate does not use the exact epoch and validator set of the orphaned signer lock",
             /*permanent=*/false);
        return false;
    }
    const uint256 digest{
        modern::FinalityDigest(*configured_domain, *finalized)};
    std::string error;
    if (!m_store.CommitCertifiedAnchor(
            state.finalized->height, state.finalized->block_hash, digest,
            state.finalized->epoch, finalized_set->SetHash(),
            finalized->validator_set_hash, error)) {
        Fail(strprintf("cannot persist the certified ancestry lock: %s",
                       error));
        return false;
    }
    m_error.clear();
    return true;
}

std::vector<FinalitySig> FinalitySigner::MaybeSign(const FinalityTracker& tracker, const CChain& chain,
                                                   const Consensus::Params& params, FinalitySignaturePool& pool,
                                                   const BridgeStateIndex* bridge_index)
{
    std::vector<FinalitySig> out;
    if (!m_key || !params.legacy_b3coin || !params.modern_pos) return out;
    if (!EnsurePersistentSafety(tracker, chain, params, bridge_index)) {
        return out;
    }
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
        // Crash safety and fork safety come before every externally usable
        // signature. In production, atomically persist the exact vote and its
        // ancestry lock before adding it even to the node-local pool. A pool
        // certificate can be mined or relayed independently of this return
        // value, so persisting after Submit() would be too late.
        if (m_store.IsOpen()) {
            std::string error;
            if (!m_store.CommitSignedCheckpoint(
                    h, fb->block_hash, digest, *epoch, set->SetHash(),
                    fb->validator_set_hash, error)) {
                Fail(strprintf("cannot persist checkpoint %d before signing relay: %s",
                               h, error));
                break;
            }
            m_last_signed = h;
        }
        // One signature per checkpoint, strictly increasing heights.
        const FinalitySignaturePool::Accept accepted{
            pool.Submit(sig, tracker, chain, params, bridge_index)};
        if (accepted == FinalitySignaturePool::Accept::POOL_FULL) {
            // This locally constructed signature passed every state check and
            // is merely older than all retained slots. Record that we signed
            // it so a validator which has since left the current set does not
            // repeat the same BLS work on every staking-loop iteration. Do not
            // relay it: peers enforcing the same bounded window have no use
            // for it.
            m_last_signed = h;
            continue;
        }
        if (accepted != FinalitySignaturePool::Accept::ACCEPTED &&
            accepted != FinalitySignaturePool::Accept::DUPLICATE) {
            continue;
        }
        m_last_signed = h;
        out.push_back(std::move(sig));
    }
    return out;
}

} // namespace node
