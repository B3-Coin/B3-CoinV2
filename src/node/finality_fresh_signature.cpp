// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/finality_fresh_signature.h>

#include <chain.h>
#include <crypto/bls.h>
#include <logging.h>
#include <modern/chain_domain.h>
#include <modern/finality_schedule.h>
#include <modern/mpa.h>
#include <node/finality_fresh_lineage.h>
#include <primitives/transaction.h>
#include <tinyformat.h>
#include <util/strencodings.h>

#include <algorithm>
#include <limits>
#include <span>
#include <string>

namespace node {

const char* FreshLineageSignaturePool::AcceptName(const Accept a)
{
    switch (a) {
    case Accept::ACCEPTED: return "accepted";
    case Accept::DUPLICATE: return "duplicate";
    case Accept::STALE: return "stale";
    case Accept::UNCONFIGURED: return "unconfigured";
    case Accept::LINEAGE_MISMATCH: return "lineage-mismatch";
    case Accept::UNKNOWN_EPOCH: return "unknown-epoch";
    case Accept::NOT_CHECKPOINT: return "not-checkpoint";
    case Accept::TOO_SHALLOW: return "too-shallow";
    case Accept::BAD_INDEX: return "bad-index";
    case Accept::POOL_FULL: return "pool-full";
    case Accept::VERIFICATION_DEFERRED: return "verification-deferred";
    case Accept::BAD_SIGNATURE: return "bad-signature";
    }
    return "unknown";
}

namespace {

std::optional<uint256> ChainDomain(const Consensus::Params& params)
{
    if (!params.legacy_final_hash) return std::nullopt;
    return modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash);
}

//! hash_at over the active chain (the pool always works on the active tip).
modern::FreshHashAtFn ActiveChainHashAt(const CChain& chain)
{
    return [&chain](const int h) -> std::optional<uint256> {
        const CBlockIndex* index{chain[h]};
        if (!index) return std::nullopt;
        return index->GetBlockHash();
    };
}

//! The usable fresh state: configured, bootstrapped and not broken.
const FreshLineageState* UsableFreshState(const FinalityTracker& tracker, const Consensus::Params& params)
{
    if (!params.fresh_lineage || !params.modern_pos) return nullptr;
    const auto& fresh{tracker.Current().fresh};
    if (!fresh || !fresh->bootstrapped) return nullptr;
    return &*fresh;
}

//! The digest this chain expects for slot (epoch, height), if derivable.
std::optional<uint256> ExpectedDigest(const uint64_t epoch, const uint64_t height, const FreshLineageState& state,
                                      const modern::FreshHashAtFn& hash_at, const Consensus::Params& params,
                                      const BridgeStateIndex* bridge_index, const uint256& domain)
{
    if (height > static_cast<uint64_t>(std::numeric_limits<int>::max())) return std::nullopt;
    const auto fb{ExpectedFinalizedBlockV2(epoch, static_cast<int>(height), state, hash_at, params, bridge_index)};
    if (!fb) return std::nullopt;
    return modern::FinalityDigestV2(domain, *fb);
}

} // namespace

FreshLineageSignaturePool::Accept FreshLineageSignaturePool::Submit(const FreshLineageSig& sig,
                                                                    const FinalityTracker& tracker,
                                                                    const CChain& chain,
                                                                    const Consensus::Params& params,
                                                                    const BridgeStateIndex* bridge_index,
                                                                    const std::function<bool()>& consume_verification_budget)
{
    const FreshLineageState* state{UsableFreshState(tracker, params)};
    const CBlockIndex* tip{chain.Tip()};
    const auto domain{ChainDomain(params)};
    if (!state || !tip || !domain) return Accept::UNCONFIGURED;
    // Slots never outlive the lineage they were verified for: a reorg across
    // GENESIS_HEIGHT yields another lineage id and another set of objects.
    if (m_lineage_id != state->lineage_id) {
        m_slots.clear();
        m_lineage_id = state->lineage_id;
    }
    // Reclaim finalized slots on every submission, as the V1 pool does.
    if (state->finalized) Prune(state->finalized->height);

    if (sig.lineage_id != state->lineage_id) return Accept::LINEAGE_MISMATCH;
    if (state->lineage_broken) return Accept::STALE; // nothing is certifiable any more (FD-8)
    if (sig.height > static_cast<uint64_t>(tip->nHeight)) return Accept::NOT_CHECKPOINT;
    const int h{static_cast<int>(sig.height)};
    const Consensus::ModernPosParams& pos{*params.modern_pos};
    const auto checkpoint_index{modern::FinalityCheckpointIndexV2(h, state->genesis_height, pos.checkpoint_interval)};
    // GENESIS_HEIGHT itself is history leaf 0 and never signed.
    if (!checkpoint_index || *checkpoint_index == 0) return Accept::NOT_CHECKPOINT;
    if (!modern::CheckpointDepthSatisfied(h, tip->nHeight, pos.checkpoint_depth)) return Accept::TOO_SHALLOW;
    if (state->finalized && h <= state->finalized->height) return Accept::STALE;
    const auto set{state->SetForEpoch(sig.epoch)};
    if (!set || set->Size() == 0) return Accept::UNKNOWN_EPOCH;
    const auto epoch_of_h{modern::EpochOfHeight(state->epoch_starts, h)};
    if (!epoch_of_h || *epoch_of_h != sig.epoch) return Accept::NOT_CHECKPOINT;
    if (sig.index >= set->Size()) return Accept::BAD_INDEX;

    // Reconstruct the exact signed object before deduplication: a permitted
    // pre-finality reorg can change the block hash (and so the history root
    // of every later checkpoint) at unchanged coordinates.
    const auto hash_at{ActiveChainHashAt(chain)};
    const auto digest{ExpectedDigest(sig.epoch, sig.height, *state, hash_at, params, bridge_index, *domain)};
    if (!digest) return Accept::STALE;
    for (auto it{m_slots.begin()}; it != m_slots.end();) {
        const auto expected{ExpectedDigest(it->first.first, it->first.second, *state, hash_at, params, bridge_index,
                                           *domain)};
        if (!expected || *expected != it->second.digest) {
            it = m_slots.erase(it);
        } else {
            ++it;
        }
    }

    const auto key{std::make_pair(sig.epoch, sig.height)};
    const auto slot_it{m_slots.find(key)};
    if (slot_it != m_slots.end()) {
        const auto known{slot_it->second.sigs.find(sig.index)};
        if (known != slot_it->second.sigs.end()) {
            // Canonical BLS encoding is unique for one member and digest:
            // other bytes are not a duplicate and never replace the record.
            return known->second == sig.signature ? Accept::DUPLICATE : Accept::BAD_SIGNATURE;
        }
    }
    // A newer valid checkpoint replaces the oldest bounded slot after
    // verification; an old replay cannot evict newer work.
    const bool evict_oldest{slot_it == m_slots.end() && m_slots.size() >= MAX_TRACKED_CHECKPOINTS};
    if (evict_oldest && key <= m_slots.begin()->first) return Accept::POOL_FULL;

    // Expensive BLS verification remains last, charged to the caller's
    // budget only now (never for a cheap reject or an exact duplicate).
    if (consume_verification_budget && !consume_verification_budget()) return Accept::VERIFICATION_DEFERRED;
    const auto decoded{bls::Signature::Decode(sig.signature)};
    if (!decoded) return Accept::BAD_SIGNATURE;
    const auto& member_key{set->View().keys[sig.index]};
    if (!bls::Verify(member_key.Key(), std::span<const unsigned char>(digest->begin(), 32), *decoded)) {
        return Accept::BAD_SIGNATURE;
    }
    if (evict_oldest) m_slots.erase(m_slots.begin());
    Slot& slot{m_slots[key]};
    if (slot.sigs.empty()) slot.digest = *digest;
    if (slot.digest != *digest) return Accept::STALE; // defensive; evicted above
    slot.sigs[sig.index] = sig.signature;
    return Accept::ACCEPTED;
}

std::optional<std::pair<modern::FinalizedBlockV2, modern::FinalityCertificate>>
FreshLineageSignaturePool::BestCertificate(const FinalityTracker& tracker, const CChain& chain,
                                           const Consensus::Params& params, const BridgeStateIndex* bridge_index,
                                           const CBlockIndex* candidate_parent) const
{
    const FreshLineageState* state{UsableFreshState(tracker, params)};
    const auto domain{ChainDomain(params)};
    if (!state || !domain || state->lineage_broken || m_lineage_id != state->lineage_id) return std::nullopt;
    const auto hash_at{ActiveChainHashAt(chain)};
    // Highest height first; prefer the newest epoch at equal height.
    for (auto it{m_slots.rbegin()}; it != m_slots.rend(); ++it) {
        const auto& [epoch, height]{it->first};
        if (height > static_cast<uint64_t>(std::numeric_limits<int>::max())) continue;
        if (state->finalized && height <= static_cast<uint64_t>(state->finalized->height)) continue;
        const auto set{state->SetForEpoch(epoch)};
        if (!set || set->Size() == 0) continue;
        uint64_t weight{0};
        uint32_t signer_count{0};
        for (const auto& [index, sig] : it->second.sigs) {
            if (index < set->Size()) {
                weight += set->members->Members()[index].weight;
                ++signer_count;
            }
        }
        if (weight < set->members->QuorumWeight() ||
            signer_count < modern::FinalityHeadcountQuorum(static_cast<uint32_t>(set->Size()))) {
            continue;
        }
        const auto fb{ExpectedFinalizedBlockV2(epoch, static_cast<int>(height), *state, hash_at, params,
                                               bridge_index)};
        if (!fb || modern::FinalityDigestV2(*domain, *fb) != it->second.digest) continue;
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
        if (candidate_parent) {
            std::string error;
            if (!tracker.JudgeCandidateCertificateV2(*fb, cert, *candidate_parent, params, error, bridge_index)) {
                LogDebug(BCLog::VALIDATION, "fresh-lineage pool: checkpoint %d (epoch %d) not includable (%s)\n",
                         height, epoch, error);
                continue;
            }
        }
        return std::make_pair(*fb, std::move(cert));
    }
    return std::nullopt;
}

std::vector<FreshLineageSig> FreshLineageSignaturePool::VerifiedSignatures(const FinalityTracker& tracker,
                                                                           const CChain& chain,
                                                                           const Consensus::Params& params,
                                                                           const BridgeStateIndex* bridge_index) const
{
    std::vector<FreshLineageSig> out;
    const FreshLineageState* state{UsableFreshState(tracker, params)};
    const CBlockIndex* tip{chain.Tip()};
    const auto domain{ChainDomain(params)};
    if (!state || !tip || !domain || state->lineage_broken || m_lineage_id != state->lineage_id) return out;
    const auto hash_at{ActiveChainHashAt(chain)};
    const Consensus::ModernPosParams& pos{*params.modern_pos};
    for (auto it{m_slots.rbegin()}; it != m_slots.rend(); ++it) {
        const auto [epoch, height]{it->first};
        if (height > static_cast<uint64_t>(tip->nHeight) ||
            (state->finalized && height <= static_cast<uint64_t>(state->finalized->height))) {
            continue;
        }
        const int h{static_cast<int>(height)};
        const auto set{state->SetForEpoch(epoch)};
        const auto epoch_of_h{modern::EpochOfHeight(state->epoch_starts, h)};
        if (!set || !epoch_of_h || *epoch_of_h != epoch ||
            !modern::CheckpointDepthSatisfied(h, tip->nHeight, pos.checkpoint_depth)) {
            continue;
        }
        const auto digest{ExpectedDigest(epoch, height, *state, hash_at, params, bridge_index, *domain)};
        if (!digest || *digest != it->second.digest) continue;
        for (const auto& [index, signature] : it->second.sigs) {
            if (index >= set->Size()) continue;
            FreshLineageSig sig;
            sig.lineage_id = m_lineage_id;
            sig.epoch = epoch;
            sig.height = height;
            sig.index = index;
            sig.signature = signature;
            out.push_back(sig);
        }
    }
    return out;
}

void FreshLineageSignaturePool::Prune(const int finalized_height)
{
    for (auto it{m_slots.begin()}; it != m_slots.end();) {
        if (finalized_height >= 0 && it->first.second <= static_cast<uint64_t>(finalized_height)) {
            it = m_slots.erase(it);
        } else {
            ++it;
        }
    }
}

size_t FreshLineageSignaturePool::SignatureCount(const uint64_t epoch, const uint64_t height) const
{
    const auto it{m_slots.find(std::make_pair(epoch, height))};
    return it == m_slots.end() ? 0 : it->second.sigs.size();
}

bool AppendFreshLineageCertificate(const FinalityTracker& tracker, const FreshLineageSignaturePool& pool,
                                   const CChain& chain, const Consensus::Params& params, const CBlockIndex& parent,
                                   const BridgeStateIndex* bridge_index, CMutableTransaction& coinbase)
{
    if (!params.fresh_lineage) return false;
    const auto best{pool.BestCertificate(tracker, chain, params, bridge_index, &parent)};
    if (!best) return false;
    const auto& [fb, cert]{*best};
    CMpaRecord record;
    record.payload_type = modern::MPA_TYPE_FINALITY_CERTIFICATE_V2;
    record.payload_version = modern::MPA_VERSION_V1;
    record.payload = modern::EncodeCertificatePayloadV2(fb, cert);
    coinbase.mpa.push_back(std::move(record));
    std::sort(coinbase.mpa.begin(), coinbase.mpa.end(), modern::MpaRecordLess);
    LogInfo("CreateNewBlock(): including fresh-lineage certificate for checkpoint %d (epoch %d, index %d, lineage %s)\n",
            fb.height, fb.epoch, fb.checkpoint_index, fb.lineage_id.ToString());
    return true;
}

FreshSignerKeyStatus ComputeFreshSignerKeyStatus(const modern::ValidatorKeyBytes& validator_key,
                                                 std::vector<FreshSignerKeyEntry> keys,
                                                 const FreshKeyBindingIndex* index, const FreshLineageState* state,
                                                 const Consensus::Params& params, const int tip_height)
{
    FreshSignerKeyStatus out;
    const bool bootstrapped{state != nullptr && state->bootstrapped &&
                            state->epoch <= std::numeric_limits<uint64_t>::max() - 2};
    const uint64_t e{bootstrapped ? state->epoch : 0};

    // This validator's counted fresh bindings (target order, then chain order).
    std::vector<FreshKeyBindingEntry> own;
    if (index) {
        for (FreshKeyBindingEntry& entry : index->BindingsFrom(0)) {
            if (entry.validator_key == validator_key) own.push_back(std::move(entry));
        }
    }
    // The retained committees by fresh epoch: Set'_{e-2} .. Set'_{e+1}.
    std::vector<std::pair<uint64_t, const FreshLineageSet*>> retained;
    if (bootstrapped) {
        if (e >= 2 && state->previous2) retained.emplace_back(e - 2, state->previous2.get());
        if (e >= 1 && state->previous) retained.emplace_back(e - 1, state->previous.get());
        if (state->current) retained.emplace_back(e, state->current.get());
        if (state->next) retained.emplace_back(e + 1, state->next.get());
    }
    const auto seat_key{[&validator_key](const FreshLineageSet& set) -> std::optional<modern::BlsPubkeyBytes> {
        const auto i{set.IndexOf(validator_key)};
        if (!i) return std::nullopt;
        return set.members->Members()[*i].bls_pubkey;
    }};

    for (FreshSignerKeyEntry& key : keys) {
        if (!key.epoch) {
            for (const FreshKeyBindingEntry& entry : own) {
                if (entry.binding.bls_pubkey == key.bls_pubkey) {
                    key.epoch = entry.target_epoch;
                    break;
                }
            }
        }
        key.seated_epochs.clear();
        for (const auto& [k, set] : retained) {
            if (seat_key(*set) == key.bls_pubkey) key.seated_epochs.push_back(k);
        }
    }
    const auto find_key{[&keys](const modern::BlsPubkeyBytes& pk) -> const FreshSignerKeyEntry* {
        const auto it{std::find_if(keys.begin(), keys.end(),
                                   [&pk](const FreshSignerKeyEntry& key) { return key.bls_pubkey == pk; })};
        return it == keys.end() ? nullptr : &*it;
    }};

    // Seats this signer must be able to sign for: Set'_{e-1} (delayed
    // checkpoints of the previous epoch), Set'_e and Set'_{e+1}.
    for (const auto& [k, set] : retained) {
        if (k + 1 < e) continue; // Set'_{e-2}: kept only for the verifier judgement
        const auto pk{seat_key(*set)};
        if (!pk) continue;
        const FreshSignerKeyEntry* held{find_key(*pk)};
        if (held && held->held) continue;
        if (held && k + 1 == e) continue; // erased by policy after the handover into e
        out.missing_seats.push_back(FreshMissingSeat{k, *pk});
    }

    // Alerts. Bindings and seats older than the retained committees (e-2)
    // are no longer reported.
    const uint64_t min_target{bootstrapped && e >= 2 ? e - 2 : 0};
    for (const FreshKeyBindingEntry& entry : own) {
        if (entry.target_epoch < min_target || find_key(entry.binding.bls_pubkey)) continue;
        out.alerts.push_back(strprintf(
            "fresh key %s bound for fresh epoch %u at height %d under this validator's identity is not in this signer's fresh key store: an old wallet backup or a leaked identity key is binding keys (consider restaking under a new identity)",
            HexStr(entry.binding.bls_pubkey), entry.target_epoch, entry.binding.height));
    }
    // A held key of this signer that the chain recorded under another first
    // claim: its own binding (if any) is inert and seats nothing. With
    // identity-bound proofs of possession only a holder of the secret can
    // make that claim, so the key has leaked (or was bound twice by hand).
    for (const FreshSignerKeyEntry& key : keys) {
        if (!index || !key.held) continue;
        const auto first{index->FirstBoundHeight(key.bls_pubkey)};
        if (!first || index->OwnFreshEpoch(validator_key, key.bls_pubkey)) continue;
        out.alerts.push_back(strprintf(
            "fresh key %s%s held by this signer was claimed first by another binding at height %d (another validator's, or a V1 FINALITY_KEY binding): this validator's binding of it is inert and seats nothing, and only a holder of the key's secret could have made that claim; bindfreshfinalitykey replaces it while that epoch's binding window is open",
            HexStr(key.bls_pubkey), key.epoch ? strprintf(" of fresh epoch %u", *key.epoch) : std::string{}, *first));
    }
    const uint64_t max_target{bootstrapped ? e + 2 : 0};
    for (uint64_t t{min_target}; index && t <= max_target; ++t) {
        const auto window{FreshSeatWindow(t, params, state, tip_height)};
        if (!window) continue;
        const FreshSeat seat{FreshSeatFor(*index, validator_key, t, window->first, window->second)};
        if (!seat.voided) continue;
        std::string keys_text;
        for (const auto& pk : seat.pubkeys) keys_text += (keys_text.empty() ? "" : ", ") + HexStr(pk);
        out.alerts.push_back(strprintf(
            "fresh-lineage seat for fresh epoch %u is void: %u fresh bindings of this validator name it (%s); an old wallet backup or a leaked identity key may be binding keys",
            t, seat.pubkeys.size(), keys_text));
    }
    out.fresh_keys = std::move(keys);
    return out;
}

} // namespace node
