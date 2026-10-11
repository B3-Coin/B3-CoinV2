// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/finality_signature.h>

#include <chain.h>
#include <consensus/era.h>
#include <interfaces/chain.h>
#include <logging.h>
#include <modern/chain_domain.h>
#include <modern/finality_schedule.h>
#include <node/bridge_state.h>
#include <node/validator_set.h>
#include <util/strencodings.h>

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
    case Accept::VERIFICATION_DEFERRED: return "verification-deferred";
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
                                                            const BridgeStateIndex* bridge_index,
                                                            const std::function<bool()>& consume_verification_budget)
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
    if (slot_it != m_slots.end()) {
        const auto known{slot_it->second.sigs.find(sig.index)};
        if (known != slot_it->second.sigs.end()) {
            // Coordinates alone are not proof of receipt of verified bytes.
            // Canonical BLS encoding is unique for this member and digest, so
            // another byte string is not a valid duplicate and cannot replace
            // the verified record or acquire its relay/acceptance status.
            return known->second == sig.signature ? Accept::DUPLICATE : Accept::BAD_SIGNATURE;
        }
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
    if (consume_verification_budget && !consume_verification_budget()) {
        return Accept::VERIFICATION_DEFERRED;
    }
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
                                       const BridgeStateIndex* bridge_index,
                                       const CBlockIndex* candidate_parent) const
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
        if (candidate_parent) {
            std::string error;
            if (!tracker.JudgeCandidateCertificate(*fb, cert, *candidate_parent,
                                                    params, error, bridge_index)) {
                // A retained quorum can become temporarily too shallow after
                // rollback. It must not mask an older usable certificate or
                // be erased to make progress. At most eight slots are tried.
                continue;
            }
        }
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

std::vector<FinalitySignaturePool::CheckpointStatus>
FinalitySignaturePool::VerifiedCheckpoints(
    const FinalityTracker& tracker, const CChain& chain,
    const Consensus::Params& params, const BridgeStateIndex* bridge_index) const
{
    std::vector<CheckpointStatus> out;
    const auto* tip{chain.Tip()};
    const auto start{Consensus::ModernPosStartHeight(params)};
    const auto domain{params.legacy_final_hash
                          ? modern::ModernChainDomain(params.hashGenesisBlock,
                                                      *params.legacy_final_hash)
                          : std::nullopt};
    if (!params.legacy_b3coin || !params.modern_pos || !tip || !start || !domain) return out;
    const auto& state{tracker.Current()};
    for (auto it{m_slots.rbegin()}; it != m_slots.rend(); ++it) {
        const auto [epoch, height]{it->first};
        if (height > static_cast<uint64_t>(tip->nHeight) ||
            (state.finalized && height <= static_cast<uint64_t>(state.finalized->height))) continue;
        const int h{static_cast<int>(height)};
        const auto* set{SetForEpoch(state, epoch)};
        const auto checkpoint_epoch{modern::EpochOfHeight(state.epoch_starts, h)};
        if (!set || !checkpoint_epoch || *checkpoint_epoch != epoch ||
            !modern::IsCheckpointHeight(h, *start, params.modern_pos->checkpoint_interval) ||
            !modern::CheckpointDepthSatisfied(h, tip->nHeight, params.modern_pos->checkpoint_depth)) continue;
        const auto checkpoint{ExpectedFinalizedBlock(epoch, height, state, chain, params, bridge_index)};
        if (!checkpoint || modern::FinalityDigest(*domain, *checkpoint) != it->second.digest) continue;

        CheckpointStatus status;
        status.checkpoint = *checkpoint;
        status.signing_set_hash = set->SetHash();
        status.validator_count = static_cast<uint32_t>(set->Size());
        status.quorum_count = modern::FinalityHeadcountQuorum(status.validator_count);
        status.total_weight = set->TotalWeight();
        status.quorum_weight = set->QuorumWeight();
        for (const auto& [index, signature] : it->second.sigs) {
            if (index >= set->Size()) continue;
            status.signer_indices.push_back(index);
            status.signed_weight += set->Members()[index].weight;
        }
        if (!status.signer_indices.empty()) out.push_back(std::move(status));
    }
    return out;
}

std::vector<FinalitySig> FinalitySignaturePool::RelayableSignatures(
    const FinalityTracker& tracker, const CChain& chain,
    const Consensus::Params& params, const BridgeStateIndex* bridge_index) const
{
    std::vector<FinalitySig> out;
    for (const auto& status : VerifiedCheckpoints(tracker, chain, params, bridge_index)) {
        const auto& checkpoint{status.checkpoint};
        const auto& slot{m_slots.at({checkpoint.epoch, checkpoint.height})};
        for (const auto index : status.signer_indices) {
            FinalitySig sig;
            sig.epoch = checkpoint.epoch;
            sig.height = checkpoint.height;
            sig.index = index;
            sig.signature = slot.sigs.at(index);
            out.push_back(std::move(sig));
        }
    }
    return out;
}

bool FinalitySigner::SetKeys(
    const std::vector<bls::SecretKey>& keys,
    const modern::ValidatorKeyBytes& validator_key, std::string& error)
{
    if (keys.empty() || keys.size() > MAX_KEYS) {
        error = strprintf("finality signer requires between 1 and %u snapshot keys", MAX_KEYS);
        return false;
    }
    std::vector<SigningKey> selected;
    for (const auto& key : keys) {
        const auto pubkey{key.GetPublicKey().Compressed()};
        if (std::none_of(selected.begin(), selected.end(), [&](const SigningKey& entry) {
                return entry.pubkey == pubkey;
            })) {
            selected.push_back({key, pubkey});
        }
    }
    m_keys = std::move(selected);
    m_validator_key = validator_key;
    m_store = FinalitySignerStore{};
    m_operator_trusted_recovery.reset();
    m_last_signed = -1;
    m_error.clear();
    m_key_error.clear();
    m_permanent_error = false;
    // An offline signer has no journal directory for the fresh lineage either.
    m_store_directory.clear();
    m_fresh_scope.SetNull();
    m_fresh_store = FinalitySignerStore{};
    m_fresh_last_signed = -1;
    m_fresh_error.clear();
    m_fresh_permanent_error = false;
    ClearFreshKeys();
    error.clear();
    return true;
}

void FinalitySigner::ClearFreshKeys()
{
    m_fresh_keys.clear();
    m_fresh_key_store.reset();
    m_fresh_key_status = FreshSignerKeyStatus{};
    m_fresh_key_error.clear();
    m_fresh_alerts.clear();
}

bool FinalitySigner::SetFreshKeys(const std::vector<bls::SecretKey>& keys, std::string& error)
{
    if (!m_store_directory.empty()) {
        error = "a persistent finality signer reads its fresh keys only from its fresh key store";
        return false;
    }
    if (m_keys.empty()) {
        error = "set the validator identity (SetKeys) before its fresh keys";
        return false;
    }
    if (keys.size() > MAX_FRESH_KEYS) {
        error = strprintf("finality signer takes at most %u fresh keys", MAX_FRESH_KEYS);
        return false;
    }
    std::vector<SigningKey> selected;
    for (const auto& key : keys) {
        const auto pubkey{key.GetPublicKey().Compressed()};
        if (std::none_of(selected.begin(), selected.end(), [&](const SigningKey& entry) {
                return entry.pubkey == pubkey;
            })) {
            selected.push_back({key, pubkey});
        }
    }
    m_fresh_keys = std::move(selected);
    error.clear();
    return true;
}

std::optional<bls::SecretKey> FinalitySigner::FreshKeyFor(
    const std::array<unsigned char, bls::PUBKEY_SIZE>& pubkey) const
{
    if (!m_store_directory.empty()) {
        if (!m_fresh_key_store || !m_fresh_key_store->IsOpen()) return std::nullopt;
        return m_fresh_key_store->SecretFor(pubkey);
    }
    const auto found{std::find_if(m_fresh_keys.begin(), m_fresh_keys.end(), [&](const SigningKey& entry) {
        return entry.pubkey == pubkey;
    })};
    if (found == m_fresh_keys.end()) return std::nullopt;
    return found->secret;
}

const bls::SecretKey* FinalitySigner::KeyFor(
    const std::array<unsigned char, bls::PUBKEY_SIZE>& pubkey) const
{
    const auto found{std::find_if(m_keys.begin(), m_keys.end(), [&](const SigningKey& entry) {
        return entry.pubkey == pubkey;
    })};
    return found == m_keys.end() ? nullptr : &found->secret;
}

bool FinalitySigner::SetKeyPersistent(
    const bls::SecretKey& key,
    const modern::ValidatorKeyBytes& validator_key,
    const uint256& chain_domain, const fs::path& store_directory,
    std::string& error)
{
    return SetKeysPersistent({key}, validator_key, chain_domain, store_directory, error);
}

bool FinalitySigner::SetKeysPersistent(
    const std::vector<bls::SecretKey>& keys,
    const modern::ValidatorKeyBytes& validator_key,
    const uint256& chain_domain, const fs::path& store_directory,
    std::string& error)
{
    // Prepare keys without touching the live signer or its journal. Invalid
    // input or an unsafe existing record must leave both unchanged.
    FinalitySigner candidate;
    if (!candidate.SetKeys(keys, validator_key, error)) return false;
    FinalitySignerStore store;
    if (!store.Open(store_directory, chain_domain, validator_key, error)) {
        return false;
    }
    m_keys = std::move(candidate.m_keys);
    m_validator_key = validator_key;
    m_store = std::move(store);
    m_operator_trusted_recovery.reset();
    m_last_signed = m_store.State()
                        ? m_store.State()->last_signed_height
                        : -1;
    m_error.clear();
    m_key_error.clear();
    m_permanent_error = false;
    // The fresh-lineage journal lives in the same directory under its own
    // namespace; it is opened lazily once the fresh lineage id is known.
    m_store_directory = store_directory;
    m_fresh_scope.SetNull();
    m_fresh_store = FinalitySignerStore{};
    m_fresh_last_signed = -1;
    m_fresh_error.clear();
    m_fresh_permanent_error = false;
    // Fresh keys (B-37 mode) come from the fresh key store in the same
    // directory, reloaded on every pass.
    ClearFreshKeys();
    return true;
}

bool FinalitySigner::SetOperatorTrustedRecovery(
    const Consensus::FinalitySignerRecovery& recovery, std::string& error)
{
    if (!recovery.Valid() || !recovery.validator_key) {
        error = "operator-trusted recovery requires a valid exact incident and a mandatory validator target";
        return false;
    }
    if (!HasKey() || !m_store.IsOpen() || !m_store.State() ||
        m_permanent_error) {
        error = "operator-trusted recovery requires an already loaded, intact persistent signer journal without a permanent safety failure";
        return false;
    }
    const FinalitySignerState& persisted{*m_store.State()};
    if (*recovery.validator_key != m_validator_key ||
        persisted.validator_key != m_validator_key ||
        recovery.chain_domain != m_store.ChainDomain() ||
        persisted.chain_domain != recovery.chain_domain) {
        error = "operator-trusted recovery does not match the loaded validator identity and chain domain";
        return false;
    }
    if (!FinalitySignerStore::MatchesOperatorRecoveryIncident(persisted, recovery, error)) return false;
    // Approval is local and in memory only. The active chain is not supplied
    // here: application checks it afresh, and the store rechecks the on-disk
    // predecessor before any durable lock move. Never clear an existing
    // safety error merely because an operator supplied a recovery plan.
    m_operator_trusted_recovery = recovery;
    error.clear();
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

interfaces::FinalityRecoveryStatus FinalitySigner::RecoveryStatus(
    const FinalityTracker& tracker, const CChain& chain,
    const Consensus::Params& params, const BridgeStateIndex* bridge_index) const
{
    interfaces::FinalityRecoveryStatus result;
    if (const auto* tip{chain.Tip()}) {
        result.observed_tip_height = tip->nHeight;
        result.observed_tip_hash = tip->GetBlockHash();
    }
    result.journal_open = m_store.IsOpen();
    result.journal_present = m_store.State().has_value();
    result.permanent_error = m_permanent_error;
    const auto& state{tracker.Current()};
    if (state.finalized) {
        const auto& finalized{*state.finalized};
        result.finalized_height = finalized.height;
        result.finalized_hash = finalized.block_hash;
        result.finalized_epoch = finalized.epoch;
        result.finalized_certified_at = finalized.certified_at;
        result.certificate_included = finalized.certified_at > finalized.height &&
                                      chain[finalized.certified_at] != nullptr;
        if (const auto* set{SetForEpoch(state, finalized.epoch)}) {
            result.finalized_signing_set_hash = set->SetHash();
        }
        const auto expected{FinalitySignaturePool::ExpectedFinalizedBlock(
            finalized.epoch, finalized.height, state, chain, params, bridge_index)};
        result.certificate_reconstructible = expected && expected->block_hash == finalized.block_hash;
    }
    if (!result.journal_open) {
        result.state = "journal_unavailable";
        return result;
    }
    if (!result.journal_present) {
        result.state = result.permanent_error ? "signer_error" : "journal_missing";
        return result;
    }
    const auto& persisted{*m_store.State()};
    result.last_signed_height = persisted.last_signed_height;
    result.last_signed_hash = persisted.last_signed_block_hash;
    result.last_signed_digest = persisted.last_signed_digest;
    if (persisted.lock_height < 0) {
        result.state = result.permanent_error ? "signer_error" : "no_ancestry_lock";
        return result;
    }
    result.lock_height = persisted.lock_height;
    result.lock_hash = persisted.lock_block_hash;
    result.lock_digest = persisted.lock_digest;
    result.lock_epoch = persisted.lock_epoch;
    result.lock_signing_set_hash = persisted.lock_signing_set_hash;
    result.lock_successor_set_hash = persisted.lock_successor_set_hash;
    if (const auto* locked{chain[persisted.lock_height]}) {
        result.current_chain_hash = locked->GetBlockHash();
        result.blocked_on_orphan_vote = *result.current_chain_hash != persisted.lock_block_hash;
    }
    if (state.finalized) {
        result.certificate_strictly_newer = state.finalized->height > persisted.lock_height;
        result.certificate_same_epoch = state.finalized->epoch == persisted.lock_epoch;
        result.certificate_same_set = result.finalized_signing_set_hash &&
                                      *result.finalized_signing_set_hash == persisted.lock_signing_set_hash;
    }
    if (result.permanent_error) {
        result.state = "signer_error";
    } else if (!result.current_chain_hash) {
        // Being behind this height is not evidence that the vote was orphaned.
        result.state = "waiting_for_locked_height";
    } else if (result.blocked_on_orphan_vote) {
        result.state = "blocked_on_orphan_vote";
    } else {
        result.state = "lock_matches_chain";
    }
    return result;
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
    // The chain-pinned one-time recovery is consulted next. A separately
    // opted-in operator-trusted incident may be consulted last; unlike the
    // protocol proof or a hardened checkpoint, that choice relies on explicit
    // external trust and cannot revoke the old signature. Both incident
    // paths leave the journal untouched unless every specified fact holds.
    std::string protocol_refusal;
    const bool newer_certificate{
        state.finalized.has_value() &&
        state.finalized->height > persisted.lock_height &&
        state.finalized->certified_at > state.finalized->height &&
        chain[state.finalized->certified_at] != nullptr};
    if (!newer_certificate) {
        protocol_refusal = strprintf(
            "active chain does not descend from signed checkpoint %d %s and has no newer included finality certificate; refusing to vote on this fork",
            persisted.lock_height,
            persisted.lock_block_hash.ToString());
    } else {
        const auto finalized{FinalitySignaturePool::ExpectedFinalizedBlock(
            state.finalized->epoch,
            static_cast<uint64_t>(state.finalized->height), state, chain,
            params, bridge_index)};
        const ValidatorSetSnapshot* finalized_set{
            SetForEpoch(state, state.finalized->epoch)};
        if (!finalized ||
            finalized->block_hash != state.finalized->block_hash) {
            protocol_refusal =
                "cannot reconstruct the included certificate used to move the ancestry lock";
        } else if (!finalized_set) {
            protocol_refusal =
                "cannot reconstruct the signing set used by the included certificate";
        } else if (state.finalized->epoch != persisted.lock_epoch ||
                   finalized_set->SetHash() !=
                       persisted.lock_signing_set_hash) {
            // Quorum intersection is only guaranteed within the exact same
            // signing set. The epoch-e handover certificate may predate this
            // orphaned vote, and Set_{e+1} may be disjoint, so a
            // successor-epoch certificate is not an unlock proof even when
            // the committed lineage hashes match.
            protocol_refusal =
                "included certificate does not use the exact epoch and validator set of the orphaned signer lock";
        } else {
            const uint256 digest{
                modern::FinalityDigest(*configured_domain, *finalized)};
            std::string error;
            if (!m_store.CommitCertifiedAnchor(
                    state.finalized->height, state.finalized->block_hash,
                    digest, state.finalized->epoch,
                    finalized_set->SetHash(), finalized->validator_set_hash,
                    error)) {
                Fail(strprintf("cannot persist the certified ancestry lock: %s",
                               error));
                return false;
            }
            m_operator_trusted_recovery.reset();
            m_error.clear();
            return true;
        }
    }
    std::string recovery_note;
    for (const Consensus::FinalitySignerRecovery& pin :
         params.finality_signer_recoveries) {
        std::string candidate_note;
        switch (TryPinnedRecovery(pin, persisted, state, chain, params,
                                  *configured_domain, bridge_index,
                                  candidate_note)) {
        case PinnedRecovery::APPLIED:
            m_operator_trusted_recovery.reset();
            m_error.clear();
            return true;
        case PinnedRecovery::FAILED:
            return false;
        case PinnedRecovery::NOT_APPLICABLE:
            if (!candidate_note.empty()) {
                if (!recovery_note.empty()) recovery_note += "; ";
                recovery_note += candidate_note;
            }
            break;
        }
    }
    std::string operator_note;
    switch (TryOperatorTrustedRecovery(persisted, state, chain, params,
                                       *configured_domain, bridge_index,
                                       operator_note)) {
    case PinnedRecovery::APPLIED:
        m_error.clear();
        return true;
    case PinnedRecovery::FAILED:
        return false;
    case PinnedRecovery::NOT_APPLICABLE:
        if (!operator_note.empty()) {
            if (!recovery_note.empty()) recovery_note += "; ";
            recovery_note += operator_note;
        }
        break;
    }
    Fail(protocol_refusal +
             (recovery_note.empty() ? "" : " (" + recovery_note + ")"),
         /*permanent=*/false);
    return false;
}

FinalitySigner::PinnedRecovery FinalitySigner::TryPinnedRecovery(
    const Consensus::FinalitySignerRecovery& pin,
    const FinalitySignerState& persisted,
    const FinalityTracker::State& state, const CChain& chain,
    const Consensus::Params& params, const uint256& chain_domain,
    const BridgeStateIndex* bridge_index, std::string& reason)
{
    return TryRecoveryAnchor(pin, persisted, state, chain, params, chain_domain,
                             bridge_index, RecoveryTrust::HARDENED_CHECKPOINT,
                             reason);
}

FinalitySigner::PinnedRecovery FinalitySigner::TryOperatorTrustedRecovery(
    const FinalitySignerState& persisted,
    const FinalityTracker::State& state, const CChain& chain,
    const Consensus::Params& params, const uint256& chain_domain,
    const BridgeStateIndex* bridge_index, std::string& reason)
{
    reason.clear();
    if (!m_operator_trusted_recovery) return PinnedRecovery::NOT_APPLICABLE;
    const PinnedRecovery result{TryRecoveryAnchor(
        *m_operator_trusted_recovery, persisted, state, chain, params,
        chain_domain, bridge_index, RecoveryTrust::OPERATOR_TRUSTED, reason)};
    if (result == PinnedRecovery::APPLIED) m_operator_trusted_recovery.reset();
    return result;
}

FinalitySigner::PinnedRecovery FinalitySigner::TryRecoveryAnchor(
    const Consensus::FinalitySignerRecovery& pin,
    const FinalitySignerState& persisted,
    const FinalityTracker::State& state, const CChain& chain,
    const Consensus::Params& params, const uint256& chain_domain,
    const BridgeStateIndex* bridge_index, const RecoveryTrust trust,
    std::string& reason)
{
    reason.clear();
    const bool operator_trusted{trust == RecoveryTrust::OPERATOR_TRUSTED};
    const std::string label{operator_trusted ? "operator-trusted recovery" : "pinned recovery"};
    if (!pin.Valid() || !params.modern_pos) {
        return PinnedRecovery::NOT_APPLICABLE;
    }
    if (operator_trusted && (!state.bootstrapped || state.lineage_broken)) {
        reason = label + " not applicable: the epoch state is not bootstrapped with intact lineage";
        return PinnedRecovery::NOT_APPLICABLE;
    }
    const Consensus::ModernPosParams& pos{*params.modern_pos};
    const std::optional<int> modern_start{
        Consensus::ModernPosStartHeight(params)};
    const CBlockIndex* tip{chain.Tip()};
    if (!modern_start || !tip) return PinnedRecovery::NOT_APPLICABLE;

    // 1. Network: the pin, the configured chain and the journal must all
    //    name the same modern chain domain.
    if (pin.chain_domain != chain_domain ||
        m_store.ChainDomain() != chain_domain) {
        return PinnedRecovery::NOT_APPLICABLE;
    }
    if (pin.validator_key && *pin.validator_key != m_validator_key) {
        return PinnedRecovery::NOT_APPLICABLE;
    }
    if (operator_trusted &&
        (!pin.validator_key || persisted.validator_key != m_validator_key ||
         persisted.chain_domain != chain_domain ||
         persisted.last_signed_digest.IsNull() ||
         pin.anchor_height <= persisted.last_signed_height ||
         pin.anchor_height <= persisted.lock_height)) {
        reason = label + " not applicable: the intact targeted incident journal is required and the anchor must advance both recorded heights";
        return PinnedRecovery::NOT_APPLICABLE;
    }
    // 2. Journal: exactly the pinned incident as both the last vote and the
    //    ancestry lock (written together, with one digest), under the pinned
    //    epoch and exact validator sets, with no newer signing record. The
    //    store re-checks every one of these before writing. A journal that
    //    is not the incident is silently left alone.
    if (persisted.last_signed_height != pin.incident_height ||
        persisted.last_signed_block_hash != pin.incident_block_hash ||
        persisted.lock_height != pin.incident_height ||
        persisted.lock_block_hash != pin.incident_block_hash ||
        persisted.lock_digest != persisted.last_signed_digest ||
        persisted.lock_epoch != pin.incident_epoch ||
        persisted.lock_signing_set_hash != pin.incident_signing_set_hash ||
        persisted.lock_successor_set_hash !=
            pin.incident_successor_set_hash) {
        if (operator_trusted) {
            reason = label + " not applicable: the journal no longer exactly matches the approved incident; a later vote or recovery lock cannot be bypassed";
        }
        return PinnedRecovery::NOT_APPLICABLE;
    }
    // 3. The existing chain-pinned mode still requires the exact hardened
    //    modern checkpoint. Its block-validity rule is unchanged, including
    //    during reindex; only the separately opted-in mode uses local trust.
    if (!operator_trusted) {
        const auto hardened_anchor{
            params.modern_checkpoints.find(pin.anchor_height)};
        if (hardened_anchor == params.modern_checkpoints.end() ||
            hardened_anchor->second != pin.anchor_block_hash) {
            reason = "pinned recovery not applicable: the recovery anchor is not a hardened modern checkpoint";
            return PinnedRecovery::NOT_APPLICABLE;
        }
    }
    // Operator trust does not add or require a hardened block checkpoint.
    // It is a local exceptional choice, NOT an alternative quorum proof.
    // From here on this journal IS the pinned incident, so every refusal is
    // named: an operator must be able to tell a recovery that is still
    // pending from one that does not apply to this chain.
    //
    // 4. Active chain: the incident block must really be orphaned here (the
    //    caller only reaches this path when it is; kept as a guard), and the
    //    agreed anchor must be this chain's block at its height and buried
    //    to the required depth. ONLY hardened mode also has header admission
    //    and ConnectBlock enforcement. An operator-trusted anchor can itself
    //    be orphaned later; the normal ancestry guard must then refuse again.
    const CBlockIndex* at_incident{chain[pin.incident_height]};
    const CBlockIndex* anchor{chain[pin.anchor_height]};
    if (!at_incident) {
        reason = label + " pending: the active chain has not reached the incident height";
        return PinnedRecovery::NOT_APPLICABLE;
    }
    if (at_incident->GetBlockHash() == pin.incident_block_hash) {
        reason = label + " not applicable: the incident block is on this chain";
        return PinnedRecovery::NOT_APPLICABLE;
    }
    if (!anchor || anchor->GetBlockHash() != pin.anchor_block_hash) {
        reason = label + " not applicable: this chain does not carry the pinned anchor block";
        return PinnedRecovery::NOT_APPLICABLE;
    }
    const int recovery_depth{operator_trusted ? std::max(20, pos.checkpoint_depth)
                                             : pos.checkpoint_depth};
    if (!modern::CheckpointDepthSatisfied(pin.anchor_height, tip->nHeight,
                                          recovery_depth)) {
        reason = operator_trusted
                     ? label + " pending: the trusted anchor is not yet buried by at least 20 blocks and the consensus signing depth"
                     : "pinned recovery pending: the hardened anchor is not yet buried to finality-signing depth";
        return PinnedRecovery::NOT_APPLICABLE;
    }
    // 5. Lineage: the incident's epoch must still be inside this chain's
    //    certificate window ({current, current-1}, exactly as the signature
    //    pool resolves it), with the exact signing set and the exact
    //    committed successor set of the orphaned vote, and the anchor must be
    //    one of that epoch's scheduled checkpoints. This is deliberately not
    //    "the tracker is still in the incident epoch": a certified handover
    //    rotates the tracker on schedule whether or not the deadlock is
    //    resolved, and the previous epoch stays signable until the lineage
    //    breaks.
    const ValidatorSetSnapshot* incident_set{
        SetForEpoch(state, pin.incident_epoch)};
    const auto incident_successor{
        SuccessorHashForEpoch(state, pin.incident_epoch)};
    if (!incident_set ||
        incident_set->SetHash() != pin.incident_signing_set_hash ||
        !incident_successor ||
        *incident_successor != pin.incident_successor_set_hash) {
        reason = label + " not applicable: this chain's epoch state does not match the incident lineage";
        return PinnedRecovery::NOT_APPLICABLE;
    }
    const auto anchor_epoch{
        modern::EpochOfHeight(state.epoch_starts, pin.anchor_height)};
    if (!modern::IsCheckpointHeight(pin.anchor_height, *modern_start,
                                    pos.checkpoint_interval) ||
        !anchor_epoch || *anchor_epoch != pin.incident_epoch) {
        reason = label + " not applicable: the anchor is not a scheduled checkpoint of the incident epoch";
        return PinnedRecovery::NOT_APPLICABLE;
    }
    // 6. The exact object this node would sign at the anchor must be
    //    derivable on this chain and commit to the same successor set.
    const auto fb{FinalitySignaturePool::ExpectedFinalizedBlock(
        pin.incident_epoch, static_cast<uint64_t>(pin.anchor_height),
        state, chain, params, bridge_index)};
    if (!fb) {
        reason = label + " pending: the anchor checkpoint object is not derivable on this node yet (bridge state not synced?)";
        return PinnedRecovery::NOT_APPLICABLE;
    }
    if (fb->block_hash != pin.anchor_block_hash ||
        fb->validator_set_hash != pin.incident_successor_set_hash) {
        reason = label + " not applicable: the anchor checkpoint object differs from the pinned incident lineage";
        return PinnedRecovery::NOT_APPLICABLE;
    }
    const uint256 digest{modern::FinalityDigest(chain_domain, *fb)};
    std::string error;
    if (!m_store.CommitPinnedRecoveryAnchor(pin, digest, error)) {
        Fail(strprintf("cannot persist the %s anchor: %s", label, error));
        return PinnedRecovery::FAILED;
    }
    if (operator_trusted) {
        LogWarning(
            "finality signer: applied explicitly operator-trusted one-time recovery, NOT a quorum proof or hardened checkpoint: ancestry lock moved from orphaned checkpoint %d %s to the trusted anchor %d %s; the recorded vote at %d remains unchanged and is NOT revoked, and the next signature must be above %d",
            pin.incident_height, pin.incident_block_hash.ToString(),
            pin.anchor_height, pin.anchor_block_hash.ToString(),
            pin.incident_height, pin.anchor_height);
    } else {
        LogWarning(
            "finality signer: applied the pinned one-time recovery: ancestry lock moved from orphaned checkpoint %d %s to the agreed anchor %d %s; the recorded vote at %d is retained and the next signature must be above %d",
            pin.incident_height, pin.incident_block_hash.ToString(),
            pin.anchor_height, pin.anchor_block_hash.ToString(),
            pin.incident_height, pin.anchor_height);
    }
    return PinnedRecovery::APPLIED;
}

std::vector<FinalitySig> FinalitySigner::MaybeSign(const FinalityTracker& tracker, const CChain& chain,
                                                   const Consensus::Params& params, FinalitySignaturePool& pool,
                                                   const BridgeStateIndex* bridge_index)
{
    std::vector<FinalitySig> out{MaybeSignV1(tracker, chain, params, pool, bridge_index)};
    // Regtest staging signer only (no-op unless params.fresh_lineage has
    // staging_signer). Runs even when the V1 pass refused (orphan lock,
    // broken V1 lineage): the fresh journal is independent (RT4 item 2).
    MaybeSignFresh(tracker, chain, params, pool.Fresh(), bridge_index);
    return out;
}

std::vector<FinalitySig> FinalitySigner::MaybeSignV1(const FinalityTracker& tracker, const CChain& chain,
                                                     const Consensus::Params& params, FinalitySignaturePool& pool,
                                                     const BridgeStateIndex* bridge_index)
{
    std::vector<FinalitySig> out;
    if (m_keys.empty() || !params.legacy_b3coin || !params.modern_pos) return out;
    if (!EnsurePersistentSafety(tracker, chain, params, bridge_index)) {
        return out;
    }
    const std::optional<int> modern_start{Consensus::ModernPosStartHeight(params)};
    const CBlockIndex* tip{chain.Tip()};
    if (!modern_start || !tip) return out;
    const Consensus::ModernPosParams& pos{*params.modern_pos};
    const FinalityTracker::State& state{tracker.Current()};
    if (!state.bootstrapped || state.lineage_broken) return out;

    // A loaded rotation key does not mean this validator can currently sign.
    // Report the exact missing active key, but still permit any available
    // previous-epoch key below. Re-evaluate every pass so a future key becomes
    // usable automatically when its snapshot takes effect.
    std::string key_error;
    if (state.current) {
        if (const auto index{state.current->IndexOf(m_validator_key)}) {
            const auto& expected{state.current->Members()[*index].bls_pubkey};
            if (!KeyFor(expected)) {
                key_error = strprintf(
                    "missing finality private key for current epoch %u snapshot BLS public key %s; unlock the wallet containing this original key and restart staking, preserving the existing finality_signer journal",
                    state.epoch, HexStr(expected));
            }
        }
    }
    if (!key_error.empty() && key_error != m_key_error) {
        LogError("finality signer: %s", key_error);
    }
    m_key_error = std::move(key_error);

    int deepest{m_last_signed};
    if (state.finalized) deepest = std::max(deepest, state.finalized->height);
    // The durable ancestry lock may lie above the last vote (a certified
    // anchor, or the pinned one-time recovery). Never propose a checkpoint
    // at or below it: the journal would refuse it, and that refusal is a
    // permanent halt.
    if (m_store.IsOpen() && m_store.State()) {
        deepest = std::max(deepest, m_store.State()->lock_height);
    }
    // A local delay restricts only new signatures. Journal recovery and the
    // pool's consensus minimum remain unchanged, including during an upgrade.
    const auto signing_window{m_signing_policy.Checkpoints(
        tip->nHeight, pos.checkpoint_depth, *modern_start,
        pos.checkpoint_interval, deepest)};
    if (!signing_window) return out;
    for (int64_t candidate{signing_window->first_checkpoint};
         candidate <= signing_window->last_checkpoint;
         candidate += pos.checkpoint_interval) {
        const int h{static_cast<int>(candidate)};
        const auto epoch{modern::EpochOfHeight(state.epoch_starts, h)};
        if (!epoch) continue;
        const ValidatorSetSnapshot* set{SetForEpoch(state, *epoch)};
        if (!set) continue;
        const auto index{set->IndexOf(m_validator_key)};
        if (!index) continue;
        const bls::SecretKey* key{KeyFor(set->Members()[*index].bls_pubkey)};
        if (!key) continue;
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
        sig.signature = key->Sign(std::span<const unsigned char>(digest.begin(), 32)).Compressed();
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

// ------------------------------- fresh lineage (bridge V2 stage S3, regtest staging)

void FinalitySigner::FailFresh(std::string error, const bool permanent)
{
    if (m_fresh_error != error) {
        LogError("finality signer (fresh lineage, regtest staging): disabled safely: %s", error);
    }
    m_fresh_error = std::move(error);
    m_fresh_permanent_error = m_fresh_permanent_error || permanent;
}

bool FinalitySigner::EnsureFreshPersistentSafety(const FreshLineageState& state, const CChain& chain,
                                                 const Consensus::Params& params,
                                                 const BridgeStateIndex* bridge_index)
{
    if (m_fresh_scope != state.lineage_id) {
        // Another fresh lineage (first use, or a reorg across GENESIS_HEIGHT)
        // has its own journal, watermark and error state; this holds for an
        // offline signer's in-memory watermark too. A journal of an earlier
        // lineage is left untouched on disk and never consulted.
        m_fresh_scope = state.lineage_id;
        m_fresh_store = FinalitySignerStore{};
        m_fresh_last_signed = -1;
        m_fresh_error.clear();
        m_fresh_permanent_error = false;
    }
    if (m_store_directory.empty()) return true; // unit-test/offline signer
    if (m_fresh_permanent_error) return false;
    if (!m_fresh_store.IsOpen()) {
        // Never the V1 journal: its own file name and magic, so a V1 orphan
        // lock neither blocks nor is touched by fresh-lineage votes.
        FinalitySignerStore store;
        std::string error;
        if (!store.Open(m_store_directory, state.lineage_id, m_validator_key, error,
                        FinalitySignerNamespace::FRESH_LINEAGE_V2)) {
            FailFresh(strprintf("cannot open the fresh-lineage signer journal: %s", error));
            return false;
        }
        m_fresh_store = std::move(store);
    }

    const CBlockIndex* tip{chain.Tip()};
    const auto domain{params.legacy_final_hash
                          ? modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)
                          : std::nullopt};
    if (!tip || !params.modern_pos || !domain || state.genesis_height < 0) {
        FailFresh("fresh-lineage schedule is not configured", /*permanent=*/false);
        return false;
    }
    const Consensus::ModernPosParams& pos{*params.modern_pos};
    const int genesis{state.genesis_height};
    // Shared checks for retained, consensus-validated handovers. These are
    // derived internal records, not peer-supplied recovery instructions.
    // Both the checkpoint and its exact carrier must remain canonical.
    const auto in_epoch{[&state](const uint64_t e, const int h) {
        return e < state.epoch_starts.size() && h >= state.epoch_starts[e] &&
               (e + 1 == state.epoch_starts.size() || h < state.epoch_starts[e + 1]);
    }};
    const auto valid_evidence{[&](const FreshHandoverEvidence* evidence, const uint64_t e) {
        if (!evidence) return false;
        const auto& fb{evidence->finalized_block};
        if (fb.epoch != e || fb.lineage_id != state.lineage_id ||
            fb.height > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
        const int h{static_cast<int>(fb.height)};
        if (h <= genesis || pos.checkpoint_interval <= 0 ||
            (h - genesis) % pos.checkpoint_interval != 0 || !in_epoch(e, h) ||
            fb.checkpoint_index != static_cast<uint64_t>((h - genesis) / pos.checkpoint_interval) ||
            evidence->certified_at <= h) return false;
        auto successor{state.SetHashForEpoch(e + 1)};
        if (!successor) {
            const auto* next{state.HandoverForEpoch(e + 1)};
            if (next) successor = next->signing_set_hash;
        }
        if (!successor || *successor != fb.validator_set_hash) return false;
        const CBlockIndex* checkpoint{chain[h]};
        const CBlockIndex* carrier{chain[evidence->certified_at]};
        return checkpoint && checkpoint->GetBlockHash() == fb.block_hash && carrier &&
               carrier->GetBlockHash() == evidence->certified_at_hash &&
               !evidence->signing_set_hash.IsNull() && !fb.validator_set_hash.IsNull();
    }};

    // An absent journal is ambiguous once this validator could hold a live,
    // unrecorded fresh vote: V1's rule, unchanged, on the fresh state. Before
    // the first included fresh certificate every fresh-signable checkpoint
    // counts, member or not: a same-lineage fork diverging after R_f - 1 may
    // have rotated and seated this validator in a later committee. Once a
    // fresh certificate is included, only unfinalized fresh-signable
    // checkpoints in the {current, current-1} window where this validator is
    // a member count, so a genuine newcomer can arm before its first seat.
    if (m_fresh_store.IsAbsent()) {
        const int signable_to{tip->nHeight - pos.checkpoint_depth};
        int highest_signable{-1};
        if (signable_to > genesis) {
            highest_signable = signable_to - ((signable_to - genesis) % pos.checkpoint_interval);
        }
        bool may_hide_live_vote{false};
        if (highest_signable > genesis) {
            if (!state.finalized) {
                may_hide_live_vote = true;
            } else {
                int h{std::max(genesis + pos.checkpoint_interval, state.finalized->height + 1)};
                const int rem{(h - genesis) % pos.checkpoint_interval};
                if (rem != 0) h += pos.checkpoint_interval - rem;
                for (; h <= highest_signable; h += pos.checkpoint_interval) {
                    const auto epoch{modern::EpochOfHeight(state.epoch_starts, h)};
                    const auto set{epoch ? state.SetForEpoch(*epoch) : nullptr};
                    if (set && set->IndexOf(m_validator_key)) {
                        may_hide_live_vote = true;
                        break;
                    }
                }
            }
        }
        std::string error;
        if (may_hide_live_vote) {
            FailFresh(strprintf("fresh-lineage signer journal is absent after fresh checkpoint %d became signable; refusing to recreate a possibly deleted anti-equivocation record",
                                highest_signable),
                      /*permanent=*/false);
            return false;
        }
        if (!m_fresh_store.InitializeEmpty(error)) {
            FailFresh(strprintf("cannot initialize the fresh-lineage signer journal: %s", error));
            return false;
        }
    }

    const FinalitySignerState& persisted{*m_fresh_store.State()};
    m_fresh_last_signed = persisted.last_signed_height;
    if (persisted.lock_height < 0) {
        m_fresh_error.clear();
        return true;
    }
    const CBlockIndex* locked{chain[persisted.lock_height]};
    if (locked && locked->GetBlockHash() == persisted.lock_block_hash) {
        // The lock is still an ancestor, but the signer may have missed
        // multiple certified epochs (offline, or no fresh-key seat). Follow
        // each exact, consensus-validated handover, never an arbitrary epoch
        // jump. This is local regtest recovery, not a new certificate rule.
        const auto& finalized{state.finalized};
        if (finalized && finalized->epoch > persisted.lock_epoch) {
            const auto* predecessor{state.HandoverForEpoch(persisted.lock_epoch)};
            if (!valid_evidence(predecessor, persisted.lock_epoch) ||
                !in_epoch(persisted.lock_epoch, persisted.lock_height) ||
                persisted.lock_height <= genesis || (persisted.lock_height - genesis) % pos.checkpoint_interval != 0 ||
                predecessor->signing_set_hash != persisted.lock_signing_set_hash ||
                predecessor->finalized_block.validator_set_hash != persisted.lock_successor_set_hash) {
                FailFresh("canonical fresh-lineage signer lock lacks matching validated historical handover evidence; refusing catch-up",
                          /*permanent=*/false);
                return false;
            }
            for (size_t step{0}; step < MAX_FRESH_CATCH_UP_STEPS; ++step) {
                // Copy: Commit replaces the store state. Never keep a stale
                // reference as the predecessor of the next durable write.
                const FinalitySignerState before{*m_fresh_store.State()};
                if (before.lock_epoch >= finalized->epoch) break;
                const uint64_t next_epoch{before.lock_epoch + 1};
                // A delayed previous-epoch certificate can be newer than
                // that epoch's archived rotation handover. The final step
                // uses the exact newest included statement, not a guessed
                // reconstruction or the older rotation statement.
                const auto* evidence{next_epoch == finalized->epoch
                                         ? (state.finalized_evidence ? &*state.finalized_evidence : nullptr)
                                         : state.HandoverForEpoch(next_epoch)};
                if (!valid_evidence(evidence, next_epoch) ||
                    evidence->finalized_block.height <= static_cast<uint64_t>(before.lock_height) ||
                    evidence->signing_set_hash != before.lock_successor_set_hash ||
                    (next_epoch == finalized->epoch &&
                     (modern::FinalityCertIdV2(evidence->finalized_block) != finalized->cert_id ||
                      evidence->finalized_block.block_hash != finalized->block_hash ||
                      evidence->finalized_block.height != static_cast<uint64_t>(finalized->height) ||
                      evidence->certified_at != finalized->certified_at))) {
                    FailFresh("missing or noncanonical fresh-lineage handover evidence for the next durable catch-up step; refusing to vote",
                              /*permanent=*/false);
                    return false;
                }
                const auto& fb{evidence->finalized_block};
                std::string error;
                if (!m_fresh_store.CommitLineageCatchUp(static_cast<int>(fb.height), fb.block_hash,
                                                        modern::FinalityDigestV2(*domain, fb), next_epoch,
                                                        evidence->signing_set_hash, fb.validator_set_hash, error)) {
                    FailFresh(strprintf("cannot persist the sequential fresh-lineage ancestry lock catch-up: %s", error));
                    return false;
                }
            }
            if (m_fresh_store.State()->lock_epoch < finalized->epoch) {
                FailFresh("fresh-lineage ancestry lock catch-up is incomplete; continuing on the next normal signer pass without voting",
                          /*permanent=*/false);
                return false;
            }
        }
        m_fresh_error.clear();
        return true;
    }

    // The voted branch disappeared. As in V1, the sole lock-change proof is a
    // strictly newer fresh-lineage certificate that consensus validated and
    // included on this active chain, signed by the exact same fresh epoch and
    // ruleset-2 committee. There are no pinned or operator recoveries for the
    // fresh lineage.
    std::string refusal;
    const auto& finalized{state.finalized};
    const bool newer_certificate{finalized.has_value() && finalized->height > persisted.lock_height &&
                                 finalized->certified_at > finalized->height &&
                                 chain[finalized->certified_at] != nullptr};
    if (!newer_certificate) {
        refusal = strprintf(
            "active chain does not descend from signed fresh-lineage checkpoint %d %s and has no newer included fresh-lineage certificate; refusing to vote on this fork",
            persisted.lock_height, persisted.lock_block_hash.ToString());
    } else {
        const auto hash_at{[&chain](const int h) -> std::optional<uint256> {
            const CBlockIndex* index{chain[h]};
            if (!index) return std::nullopt;
            return index->GetBlockHash();
        }};
        const auto fb{ExpectedFinalizedBlockV2(finalized->epoch, finalized->height, state, hash_at, params,
                                               bridge_index)};
        const auto finalized_set{state.SetForEpoch(finalized->epoch)};
        if (!fb || modern::FinalityCertIdV2(*fb) != finalized->cert_id) {
            refusal = "cannot reconstruct the included fresh-lineage certificate used to move the ancestry lock";
        } else if (!finalized_set) {
            refusal = "cannot reconstruct the fresh committee that signed the included certificate";
        } else if (finalized->epoch != persisted.lock_epoch ||
                   finalized_set->set_hash != persisted.lock_signing_set_hash) {
            refusal = "included fresh-lineage certificate does not use the exact fresh epoch and committee of the orphaned signer lock";
        } else {
            std::string error;
            if (!m_fresh_store.CommitCertifiedAnchor(finalized->height, finalized->block_hash,
                                                     modern::FinalityDigestV2(*domain, *fb), finalized->epoch,
                                                     finalized_set->set_hash, fb->validator_set_hash, error)) {
                FailFresh(strprintf("cannot persist the certified fresh-lineage ancestry lock: %s", error));
                return false;
            }
            m_fresh_error.clear();
            return true;
        }
    }
    // The newest certificate may already be in a later epoch. A retained
    // rotation handover can still supply the *existing* same-epoch recovery
    // authority; do not let newer finality hide it. This is one direct lookup,
    // not a scan or permission to substitute a successor committee's vote.
    if (finalized && finalized->epoch > persisted.lock_epoch) {
        const auto* historical{state.HandoverForEpoch(persisted.lock_epoch)};
        if (valid_evidence(historical, persisted.lock_epoch) &&
            historical->signing_set_hash == persisted.lock_signing_set_hash &&
            historical->finalized_block.height > static_cast<uint64_t>(persisted.lock_height)) {
            const auto& fb{historical->finalized_block};
            std::string error;
            if (!m_fresh_store.CommitCertifiedAnchor(static_cast<int>(fb.height), fb.block_hash,
                                                     modern::FinalityDigestV2(*domain, fb), fb.epoch,
                                                     historical->signing_set_hash, fb.validator_set_hash, error)) {
                FailFresh(strprintf("cannot persist the historical certified fresh-lineage ancestry lock: %s", error));
                return false;
            }
            // Commit replaces the store state: do not reuse `persisted` or
            // authorize a multi-epoch vote from its old context. The next
            // normal pass follows canonical handovers, with the usual bound.
            FailFresh("historical same-epoch fresh-lineage certificate restored the ancestry lock; continuing catch-up on the next normal signer pass without voting",
                      /*permanent=*/false);
            return false;
        }
    }
    FailFresh(refusal, /*permanent=*/false);
    return false;
}

std::vector<FreshLineageSig> FinalitySigner::MaybeSignFresh(const FinalityTracker& tracker, const CChain& chain,
                                                            const Consensus::Params& params,
                                                            FreshLineageSignaturePool& pool,
                                                            const BridgeStateIndex* bridge_index)
{
    // Regtest params are the only producer of staging_signer = true
    // (-b3freshlineageheight); mainnet and testnet never reach past here.
    if (m_keys.empty() || !params.fresh_lineage || !params.fresh_lineage->staging_signer ||
        !params.legacy_b3coin || !params.modern_pos) {
        return {};
    }
    std::vector<FreshLineageSig> out{SignFresh(tracker, chain, params, pool, bridge_index)};
    // The V1 staking status shows V1 only: leave the fresh watermark,
    // refusal and (fresh-key mode) key status where getfinalitylineage
    // reads them (node-local).
    FreshLineageSignaturePool::LocalSignerStatus status;
    status.last_signed_height = m_fresh_last_signed;
    status.last_error = FreshLastError();
    status.fresh_keys = m_fresh_key_status.fresh_keys;
    status.missing_seats = m_fresh_key_status.missing_seats;
    status.alerts = m_fresh_key_status.alerts;
    pool.ReportLocalSigner(std::move(status));
    return out;
}

void FinalitySigner::RefreshFreshKeys(const FinalityTracker& tracker, const Consensus::Params& params,
                                      const uint256& chain_domain, const FreshLineageState* state,
                                      const int tip_height)
{
    std::string key_error;
    std::vector<std::string> store_alerts;
    std::vector<FreshSignerKeyEntry> keys;
    if (!m_store_directory.empty()) {
        // Reloaded on every pass: keys generated meanwhile (wallet RPC) are
        // picked up, and a store changed on disk is never written from a
        // stale view (FreshKeyStore re-verifies before every write).
        if (!m_fresh_key_store) m_fresh_key_store = std::make_unique<FreshKeyStore>();
        std::string error;
        if (!m_fresh_key_store->Open(m_store_directory, chain_domain, m_validator_key, error)) {
            key_error = strprintf("cannot open the fresh key store: %s", error);
        } else {
            // Erasure policy (finality dependency section 6.4): a key whose
            // committee is fixed, that neither the current nor the next
            // committee seats, goes once the start of the epoch that retired
            // it is `horizon` deep (FreshKeyRetiredSince).
            const int horizon{FreshKeyErasureHorizon(params)};
            for (const FreshKeyRecord& record : m_fresh_key_store->Records()) {
                if (!state || !record.Held() ||
                    !FreshKeyErasable(*state, record.bls_pubkey, record.target_epoch, tip_height, horizon)) {
                    continue;
                }
                if (!m_fresh_key_store->Erase(record.bls_pubkey, error)) {
                    // Signing does not depend on erasure; the next pass reopens and retries.
                    store_alerts.push_back(strprintf("cannot erase fresh key %s of fresh epoch %u: %s",
                                                     HexStr(record.bls_pubkey), record.target_epoch, error));
                    break;
                }
                const int since{FreshKeyRetiredSince(*state, record.bls_pubkey, record.target_epoch).value_or(tip_height)};
                LogInfo("finality signer (fresh lineage): erased fresh key %s of fresh epoch %u at height %d: in fresh epoch %u neither the current nor the next committee seats it, and it was retired at height %d, %d blocks ago (horizon %d)",
                        HexStr(record.bls_pubkey), record.target_epoch, tip_height, state->epoch, since,
                        tip_height - since, horizon);
            }
            for (const FreshKeyRecord& record : m_fresh_key_store->Records()) {
                FreshSignerKeyEntry entry;
                entry.epoch = record.target_epoch;
                entry.bls_pubkey = record.bls_pubkey;
                entry.held = record.Held();
                keys.push_back(std::move(entry));
            }
        }
    } else {
        for (const SigningKey& key : m_fresh_keys) {
            FreshSignerKeyEntry entry;
            entry.bls_pubkey = key.pubkey;
            entry.held = true;
            keys.push_back(std::move(entry));
        }
    }
    m_fresh_key_status = ComputeFreshSignerKeyStatus(m_validator_key, std::move(keys), tracker.FreshKeys(params),
                                                     state, params, tip_height);
    m_fresh_key_status.alerts.insert(m_fresh_key_status.alerts.end(), store_alerts.begin(), store_alerts.end());

    // A seat in the current committee without a held secret is V1's
    // "missing key" case: recoverable, re-evaluated every pass.
    if (key_error.empty() && state && state->bootstrapped) {
        for (const FreshMissingSeat& seat : m_fresh_key_status.missing_seats) {
            if (seat.epoch != state->epoch) continue;
            key_error = strprintf(
                "missing fresh-lineage key for current fresh epoch %u committee BLS public key %s; this signer's fresh key store does not hold it (it was bound elsewhere, or the store was lost or erased)",
                seat.epoch, HexStr(seat.bls_pubkey));
            break;
        }
    }
    if (!key_error.empty() && key_error != m_fresh_key_error) {
        LogError("finality signer (fresh lineage, regtest staging): %s", key_error);
    }
    m_fresh_key_error = std::move(key_error);

    // Each alert is logged once while it persists (and again if it returns).
    std::set<std::string> alerts{m_fresh_key_status.alerts.begin(), m_fresh_key_status.alerts.end()};
    for (const std::string& alert : alerts) {
        if (!m_fresh_alerts.contains(alert)) {
            LogWarning("finality signer (fresh lineage, regtest staging): ALERT: %s", alert);
        }
    }
    m_fresh_alerts = std::move(alerts);
}

std::vector<FreshLineageSig> FinalitySigner::SignFresh(const FinalityTracker& tracker, const CChain& chain,
                                                       const Consensus::Params& params,
                                                       FreshLineageSignaturePool& pool,
                                                       const BridgeStateIndex* bridge_index)
{
    std::vector<FreshLineageSig> out;
    const CBlockIndex* tip{chain.Tip()};
    const auto domain{params.legacy_final_hash
                          ? modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)
                          : std::nullopt};
    if (!tip || !domain) return out;
    const Consensus::ModernPosParams& pos{*params.modern_pos};

    // The fresh state of the tip. One block early (tip == R_f - 1) the
    // projection for R_f already names the lineage, so the journal can be
    // established before any fresh checkpoint becomes signable.
    std::optional<FreshLineageState> projected;
    const FreshLineageState* state{nullptr};
    if (const auto& current{tracker.Current().fresh}; current && current->bootstrapped) {
        state = &*current;
    } else if (tip->nHeight + 1 == params.fresh_lineage->activation_height) {
        projected = tracker.Projected(tip->nHeight + 1, params).fresh;
        if (projected && projected->bootstrapped) state = &*projected;
    }
    // B-37 fresh-key mode: keys, erasure and status come first, so they are
    // current (and reported) whatever the lineage or the journal says.
    const bool key_mode{FreshKeyModeConfigured(params)};
    if (key_mode) RefreshFreshKeys(tracker, params, *domain, state, tip->nHeight);
    if (!state || state->lineage_broken) return out;
    if (!EnsureFreshPersistentSafety(*state, chain, params, bridge_index)) return out;

    int deepest{std::max(m_fresh_last_signed, state->genesis_height)};
    if (state->finalized) deepest = std::max(deepest, state->finalized->height);
    // Never propose at or below the durable fresh ancestry lock.
    if (m_fresh_store.IsOpen() && m_fresh_store.State()) {
        deepest = std::max(deepest, m_fresh_store.State()->lock_height);
    }
    // The fresh grid is anchored at GENESIS_HEIGHT (on the V1 grid by the
    // knob's own check); the local signing delay applies as in V1.
    const auto signing_window{m_signing_policy.Checkpoints(tip->nHeight, pos.checkpoint_depth,
                                                           state->genesis_height, pos.checkpoint_interval,
                                                           deepest)};
    if (!signing_window) return out;
    const auto hash_at{[&chain](const int h) -> std::optional<uint256> {
        const CBlockIndex* index{chain[h]};
        if (!index) return std::nullopt;
        return index->GetBlockHash();
    }};
    for (int64_t candidate{signing_window->first_checkpoint}; candidate <= signing_window->last_checkpoint;
         candidate += pos.checkpoint_interval) {
        const int h{static_cast<int>(candidate)};
        const auto epoch{modern::EpochOfHeight(state->epoch_starts, h)};
        if (!epoch) continue;
        const auto set{state->SetForEpoch(*epoch)};
        if (!set) continue;
        const auto index{set->IndexOf(m_validator_key)};
        if (!index) continue;
        // Without fresh-key mode the committee records the V1 FINALITY_KEY
        // binding key, so the V1 key signs. In fresh-key mode (B-37) only a
        // fresh key of this validator can match its seat: never fall back
        // to a V1 key.
        const auto& seat_key{set->members->Members()[*index].bls_pubkey};
        std::optional<bls::SecretKey> fresh_key;
        const bls::SecretKey* key{nullptr};
        if (key_mode) {
            fresh_key = FreshKeyFor(seat_key);
            if (fresh_key) key = &*fresh_key;
        } else {
            key = KeyFor(seat_key);
        }
        if (!key) continue;
        const auto fb{ExpectedFinalizedBlockV2(*epoch, h, *state, hash_at, params, bridge_index)};
        if (!fb) continue;
        const uint256 digest{modern::FinalityDigestV2(*domain, *fb)};
        FreshLineageSig sig;
        sig.lineage_id = state->lineage_id;
        sig.epoch = *epoch;
        sig.height = static_cast<uint64_t>(h);
        sig.index = *index;
        sig.signature = key->Sign(std::span<const unsigned char>(digest.begin(), 32)).Compressed();
        // Persist the exact vote and its ancestry lock in the fresh journal
        // before the vote reaches even the node-local pool.
        if (m_fresh_store.IsOpen()) {
            std::string error;
            if (!m_fresh_store.CommitSignedCheckpoint(h, fb->block_hash, digest, *epoch, set->set_hash,
                                                      fb->validator_set_hash, error)) {
                FailFresh(strprintf("cannot persist fresh-lineage checkpoint %d before signing relay: %s", h, error));
                break;
            }
            m_fresh_last_signed = h;
        }
        const FreshLineageSignaturePool::Accept accepted{pool.Submit(sig, tracker, chain, params, bridge_index)};
        if (accepted == FreshLineageSignaturePool::Accept::POOL_FULL) {
            // Older than every retained slot: recorded as signed, not relayed.
            m_fresh_last_signed = h;
            continue;
        }
        if (accepted != FreshLineageSignaturePool::Accept::ACCEPTED &&
            accepted != FreshLineageSignaturePool::Accept::DUPLICATE) {
            LogDebug(BCLog::VALIDATION, "finality signer (fresh lineage): own vote for %d not pooled (%s)\n", h,
                     FreshLineageSignaturePool::AcceptName(accepted));
            continue;
        }
        m_fresh_last_signed = h;
        out.push_back(std::move(sig));
    }
    return out;
}

} // namespace node
