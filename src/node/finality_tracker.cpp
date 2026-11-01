// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/finality_tracker.h>

#include <chain.h>
#include <consensus/era.h>
#include <crypto/bls.h>
#include <hash.h>
#include <logging.h>
#include <modern/chain_domain.h>
#include <modern/finality_certificate.h>
#include <modern/pos_v1.h>
#include <node/blockstorage.h>
#include <node/bridge_state.h>
#include <serialize.h>
#include <streams.h>
#include <util/check.h>

#include <algorithm>
#include <array>
#include <functional>
#include <ios>
#include <limits>
#include <map>

namespace node {

// ----------------------------------------------------------------- State

modern::FinalityEpochView FinalityTracker::State::View() const
{
    modern::FinalityEpochView v;
    v.current_epoch = epoch;
    v.epoch_starts = epoch_starts;
    v.lineage_broken = lineage_broken;
    if (finalized) v.finalized_height = static_cast<uint64_t>(finalized->height);
    if (current) {
        v.current_set = &current->View();
        v.current_set_hash = current->SetHash();
    }
    if (next) v.next_set_hash = next->SetHash();
    if (previous) v.previous_set = &previous->View();
    return v;
}

std::optional<uint32_t> FinalityTracker::State::SetSize(const uint64_t e) const
{
    if (!bootstrapped) return std::nullopt;
    if (e == epoch && current) return static_cast<uint32_t>(current->Size());
    if (epoch >= 1 && e == epoch - 1 && previous) return static_cast<uint32_t>(previous->Size());
    return std::nullopt;
}

bool BridgeWithdrawalValidatorSetsReady(
    const FinalityTracker::State& projected,
    const Consensus::BridgeDecentralizedWithdrawalPins& pins)
{
    if (!pins.Valid() || !projected.bootstrapped ||
        projected.lineage_broken || !projected.current || !projected.next ||
        projected.current->Epoch() != projected.epoch ||
        projected.epoch == std::numeric_limits<uint64_t>::max() ||
        projected.next->Epoch() != projected.epoch + 1) {
        return false;
    }
    const auto eligible{[&pins](const ValidatorSetSnapshot& set) {
        return set.Size() >= pins.min_bridge_validators &&
               set.Size() <= pins.max_bridge_validators &&
               set.TotalWeight() >= pins.min_bridge_total_weight;
    }};
    return eligible(*projected.current) && eligible(*projected.next);
}

// --------------------------------------------------------------- helpers

std::optional<ValidatorSetSnapshot> FinalityTracker::SnapshotAt(const uint64_t epoch, const int height) const
{
    return ValidatorSetSnapshot::BuildAt(epoch, m_stakes, height, m_bindings.Index());
}

FreshCommittee FinalityTracker::FreshCommitteeAt(const FreshCommitteeRequest& request) const
{
    // Projected() asks only for Snapshot(height - 1) == the synced height,
    // and Sync / BlockConnected keep the fresh key tracker at the synced tip
    // in fresh-key mode; anything else is a bug, reported as UNAVAILABLE
    // (the projection then fails), never as a smaller committee.
    if (!Assume(request.snapshot_height == m_synced_height) || !Assume(m_fresh_keys.Synced(m_synced_tip))) {
        LogError("FinalityTracker: B-37 committee for fresh epoch %u at snapshot %d unavailable (synced height %d, "
                 "fresh key index %s)",
                 request.target_epoch, request.snapshot_height, m_synced_height,
                 m_fresh_keys.Synced(m_synced_tip) ? "in step" : "out of step");
        return FreshCommittee::Unavailable();
    }
    CAmount total{0};
    return FreshCommittee{BuildFreshCommittee(m_stakes.ActiveWeights(request.snapshot_height, total),
                                              m_bindings.Index(), m_fresh_keys.Index(), request)};
}

std::shared_ptr<const ValidatorSetSnapshot> FinalityTracker::SuccessorSet(const State& s, const int boundary_height,
                                                                          const Consensus::ModernPosParams& pos) const
{
    // s is already rotated: s.current = Set_e (e = s.epoch); we need Set_{e+1}.
    const uint64_t successor_epoch{s.epoch + 1};
    const auto snap{SnapshotAt(successor_epoch, boundary_height)};
    if (snap && snap->Size() >= static_cast<size_t>(pos.min_finality_set)) {
        return std::make_shared<const ValidatorSetSnapshot>(*snap);
    }
    // Carry-over: Set_{e+1} = Set_e re-stamped with epoch e+1.
    return std::make_shared<const ValidatorSetSnapshot>(s.current->WithEpoch(successor_epoch));
}

std::shared_ptr<const ValidatorSetSnapshot> FinalityTracker::SyncedTipSnapshot() const
{
    if (!m_tip_snapshot.filled || m_tip_snapshot.tip != m_synced_tip) {
        const auto snap{SnapshotAt(0, m_synced_height)};
        m_tip_snapshot.filled = true;
        m_tip_snapshot.tip = m_synced_tip;
        m_tip_snapshot.snapshot = snap ? std::make_shared<const ValidatorSetSnapshot>(*snap) : nullptr;
    }
    return m_tip_snapshot.snapshot;
}

std::shared_ptr<const ValidatorSetSnapshot> FinalityTracker::ProducerSetOrCarry(
    const uint64_t epoch, const std::shared_ptr<const ValidatorSetSnapshot>& fallback,
    const size_t min_size) const
{
    // V1's Snapshot rule (ACTIVE stake joined with a non-revoked
    // FINALITY_KEY binding); below min_size the fallback is carried over
    // (V1's carry-over when min_size is MIN_FINALITY_SET).
    const auto snap{SyncedTipSnapshot()};
    if (snap && snap->Size() >= std::max<size_t>(min_size, 1)) {
        return std::make_shared<const ValidatorSetSnapshot>(snap->WithEpoch(epoch));
    }
    if (fallback) return std::make_shared<const ValidatorSetSnapshot>(fallback->WithEpoch(epoch));
    return nullptr;
}

void FinalityTracker::Reset()
{
    m_tip_snapshot = TipSnapshotMemo{};
    m_state = State{};
    m_undo.clear();
    m_stakes.MarkDirty();
    m_bindings.MarkDirty();
    m_fresh_keys.MarkDirty();
    m_synced_tip.SetNull();
    m_synced_height = -1;
    m_dirty = true;
}

// ------------------------------------------------------------ projection

FinalityTracker::State FinalityTracker::Projected(const int height, const Consensus::Params& params,
                                                 const std::optional<int64_t> round) const
{
    State s{ProjectedV1(height, params)};
    // H1 (regtest knob only): the producer schedule, from the same private
    // trackers; it never touches a V1 field. Below R_h `producers` stays
    // m_state's, which is empty there.
    if (params.H1RulesActive(height)) s.producers = ProjectProducers(height, params, round);
    // Fresh lineage (regtest knob only): projected after V1 and from the same
    // private trackers (Snapshot(height - 1)); it never touches V1 fields.
    // In fresh-key mode (R_key) the committees follow the B-37 seat rules.
    if (params.fresh_lineage && m_synced_height == height - 1) {
        FreshCommitteeAtFn committee_at;
        if (FreshKeyModeConfigured(params)) {
            committee_at = [this](const FreshCommitteeRequest& request) { return FreshCommitteeAt(request); };
        }
        if (!ProjectFreshLineage(s.fresh, height, m_synced_tip, params,
                                 [this](const int h) { return SnapshotAt(0, h); }, committee_at)) {
            s.fresh_unavailable = true;
        }
    }
    return s;
}

FinalityTracker::State FinalityTracker::ProjectedV1(const int height, const Consensus::Params& params) const
{
    State s{m_state};
    const std::optional<int> modern_start{Consensus::ModernPosStartHeight(params)};
    if (!modern_start || !params.modern_pos || height < *modern_start) return s;
    // The private trackers hold the state exactly as of height - 1.
    if (!Assume(m_synced_height == height - 1)) return s;
    const Consensus::ModernPosParams& pos{*params.modern_pos};
    const int E{pos.finality_epoch_blocks};

    if (height == *modern_start) {
        // Bootstrap: epoch 0 starts at M; Set_0 = Set_1 = Snapshot(M-1).
        s = State{};
        s.epoch = 0;
        s.epoch_starts = {*modern_start};
        const auto snap{SnapshotAt(0, height - 1)};
        if (snap && snap->Size() >= static_cast<size_t>(pos.min_finality_set)) {
            s.current = std::make_shared<const ValidatorSetSnapshot>(*snap);
            s.bootstrap = s.current;
            s.next = std::make_shared<const ValidatorSetSnapshot>(snap->WithEpoch(1));
            s.bootstrapped = true;
        }
        return s;
    }
    if (!s.bootstrapped || s.epoch_starts.empty()) return s;

    // Handover-gated rotation: first h >= epoch_start + E with the epoch-e
    // certificate already included below h.
    if (s.handover_certified && height >= s.epoch_starts.back() + E) {
        s.previous = s.current;
        s.current = s.next;
        s.epoch += 1;
        s.epoch_starts.push_back(height);
        s.handover_certified = false;
        s.next = SuccessorSet(s, height - 1, pos);
    }
    // Extension exhausted: the lineage is broken (no further certificate valid).
    if (!s.handover_certified && !s.lineage_broken &&
        height - s.epoch_starts.back() >= E + pos.max_epoch_extension) {
        s.lineage_broken = true;
    }
    return s;
}

bool FinalityTracker::ProducerInputsReady(const int height, const Consensus::Params& params) const
{
    if (!params.H1RulesActive(height) || !params.modern_pos) return false;
    // Chain parameters keep R_h above the first modern-PoS height M (the
    // V1 Set_0 bootstrap at M stays V1); anything else fails closed.
    const std::optional<int> modern_start{Consensus::ModernPosStartHeight(params)};
    const int activation{params.h1->activation_height};
    if (!modern_start || activation <= *modern_start) return false;
    // The private trackers (and m_state) hold the chain exactly as of height - 1.
    if (m_synced_height != height - 1) return false;
    // Above R_h the schedule stored for height - 1 is the input.
    return height == activation || m_state.producers.has_value();
}

std::optional<ProducerSchedule> FinalityTracker::ProjectProducers(const int height, const Consensus::Params& params,
                                                                 const std::optional<int64_t> round) const
{
    if (!ProducerInputsReady(height, params)) return std::nullopt;
    const Consensus::ModernPosParams& pos{*params.modern_pos};
    const int E{pos.finality_epoch_blocks};
    const size_t min_set{static_cast<size_t>(pos.min_finality_set)};

    ProducerSchedule p;
    if (height == params.h1->activation_height) {
        // Bootstrap: ProducerSet_0 = ProducerSet_1 = Snapshot(R_h - 1), or the
        // V1 set in force at R_h - 1 when that snapshot is below the floor.
        p.epoch = 0;
        p.epoch_start = height;
        p.current = ProducerSetOrCarry(0, m_state.current, min_set);
        p.next = p.current ? std::make_shared<const ValidatorSetSnapshot>(p.current->WithEpoch(1)) : nullptr;
    } else {
        p = *m_state.producers;
        // Fixed rotation, no certificate needed: ProducerSet_(k+1) takes
        // force and ProducerSet_(k+2) = Snapshot(height - 1) (or carry-over).
        if (height >= p.epoch_start + E) {
            p.epoch += 1;
            p.epoch_start = height;
            p.current = p.next;
            p.next = ProducerSetOrCarry(p.epoch + 1, p.current, min_set);
        }
    }
    // Escape (FD-2): a block in round >= R_escape takes its weights from
    // Snapshot(parent), whatever its size, and restarts the schedule at its
    // own height with both sets equal to it. No minimum-set floor: the
    // escape exists for liveness when the set in force is offline, and the
    // snapshot already holds every bound, ACTIVE staker. Only an empty
    // snapshot carries the set otherwise in force over.
    if (params.H1EscapeRound(height, round)) {
        auto set{ProducerSetOrCarry(p.epoch + 1, p.current, /*min_size=*/1)};
        p.epoch += 1;
        p.epoch_start = height;
        p.next = set ? std::make_shared<const ValidatorSetSnapshot>(set->WithEpoch(p.epoch + 1)) : nullptr;
        p.current = std::move(set);
        p.last_escape_height = height;
        ++p.escapes;
    }
    return p;
}

bool FinalityTracker::ProducerScheduleAvailable(const int height, const Consensus::Params& params) const
{
    // Exactly ProjectProducers' preconditions (ProducerInputsReady).
    return !params.H1RulesActive(height) || ProducerInputsReady(height, params);
}

std::shared_ptr<const ValidatorSetSnapshot> FinalityTracker::SetInForceAt(const int height,
                                                                          const Consensus::Params& params,
                                                                          const std::optional<int64_t> round) const
{
    // H1 (regtest knob only): from R_h block producers follow the producer
    // schedule; `round` selects the escape set. Below R_h, V1 unchanged.
    if (params.H1RulesActive(height)) {
        const auto producers{ProjectProducers(height, params, round)};
        return producers ? producers->current : nullptr;
    }
    // Cheap form of Projected(): the set governing `height` is Set_{e+1}
    // when the rotation fires there, else Set_e (Set_{e+2} need not be built).
    const std::optional<int> modern_start{Consensus::ModernPosStartHeight(params)};
    if (!modern_start || !params.modern_pos || height < *modern_start) return nullptr;
    if (height == *modern_start) {
        const State s{Projected(height, params)};
        return s.current;
    }
    const State& s{m_state};
    if (!s.bootstrapped || s.epoch_starts.empty()) return nullptr;
    if (s.handover_certified && height >= s.epoch_starts.back() + params.modern_pos->finality_epoch_blocks) {
        return s.next;
    }
    return s.current;
}

// ----------------------------------------------------------- certificate

namespace {

bool JudgeCoinbaseCertificateV1(const CBlock& block, const CBlockIndex& index, const Consensus::Params& params,
                                const FinalityTracker::State& projected,
                                std::optional<modern::FinalityCertificatePair>& pair,
                                std::string& error,
                                const BridgeStateIndex* bridge_index)
{
    pair.reset();
    if (block.vtx.empty()) { error = "finality-cert-no-coinbase"; return false; }
    if (!modern::MatchFinalityCertificateForEpoch(
            *block.vtx[0], [&](uint64_t e) { return projected.SetSize(e); }, pair, error)) {
        return false;
    }
    if (!pair) return true;
    if (!params.modern_pos) { error = "no-modern-pos-rules"; return false; }
    const auto domain{params.legacy_final_hash
                          ? modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)
                          : std::nullopt};
    if (!domain) { error = "finality-domain-unpinned"; return false; }
    const modern::FinalityEpochView view{projected.View()};
    const auto hash_at{[&index](const int h) -> std::optional<uint256> {
        const CBlockIndex* anc{index.GetAncestor(h)};
        if (!anc) return std::nullopt;
        return anc->GetBlockHash();
    }};
    const auto withdrawal_root_at{
        [&params, bridge_index](const int h) -> std::optional<uint256> {
            return FinalityWithdrawalRoot(h, params, bridge_index);
        }};
    return modern::JudgeFinalityCertificate(*domain, pair->finalized_block, pair->certificate, index.nHeight, view,
                                            *params.modern_pos, hash_at, error,
                                            withdrawal_root_at);
}

//! The V1 record first (unchanged), then the fresh-lineage record (type 11).
bool JudgeCoinbaseCertificate(const CBlock& block, const CBlockIndex& index, const Consensus::Params& params,
                              const FinalityTracker::State& projected,
                              std::optional<modern::FinalityCertificatePair>& pair,
                              std::optional<modern::FinalityCertificateV2Record>& fresh_record,
                              std::string& error,
                              const BridgeStateIndex* bridge_index)
{
    fresh_record.reset();
    if (!JudgeCoinbaseCertificateV1(block, index, params, projected, pair, error, bridge_index)) return false;
    const auto hash_at{[&index](const int h) -> std::optional<uint256> {
        const CBlockIndex* anc{index.GetAncestor(h)};
        if (!anc) return std::nullopt;
        return anc->GetBlockHash();
    }};
    return JudgeFreshCoinbaseCertificate(block, index, params, projected.fresh, hash_at, bridge_index,
                                         fresh_record, error);
}

} // namespace

bool FinalityTracker::CheckBlockCertificate(const CBlock& block, const CBlockIndex& index,
                                            const Consensus::Params& params,
                                            std::string& error,
                                            const BridgeStateIndex* bridge_index) const
{
    if (!index.pprev || !Synced(index.pprev->GetBlockHash())) {
        error = "finality-state-unavailable";
        return false;
    }
    const State projected{Projected(index.nHeight, params)};
    if (projected.fresh_unavailable) {
        error = "finality-state-unavailable";
        return false;
    }
    std::optional<modern::FinalityCertificatePair> pair;
    std::optional<modern::FinalityCertificateV2Record> fresh_record;
    return JudgeCoinbaseCertificate(block, index, params, projected, pair, fresh_record,
                                    error, bridge_index);
}

bool FinalityTracker::JudgeCandidateCertificate(const modern::FinalizedBlock& fb,
                                                const modern::FinalityCertificate& cert, const CBlockIndex& parent,
                                                const Consensus::Params& params,
                                                std::string& error,
                                                const BridgeStateIndex* bridge_index) const
{
    if (!Synced(parent.GetBlockHash())) {
        error = "finality-state-unavailable";
        return false;
    }
    if (!params.modern_pos) {
        error = "no-modern-pos-rules";
        return false;
    }
    const auto domain{params.legacy_final_hash
                          ? modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)
                          : std::nullopt};
    if (!domain) {
        error = "finality-domain-unpinned";
        return false;
    }
    const State projected{Projected(parent.nHeight + 1, params)};
    const modern::FinalityEpochView view{projected.View()};
    const auto hash_at{[&parent](const int h) -> std::optional<uint256> {
        const CBlockIndex* anc{parent.GetAncestor(h)};
        if (!anc) return std::nullopt;
        return anc->GetBlockHash();
    }};
    const auto withdrawal_root_at{
        [&params, bridge_index](const int h) -> std::optional<uint256> {
            return FinalityWithdrawalRoot(h, params, bridge_index);
        }};
    return modern::JudgeFinalityCertificate(*domain, fb, cert, parent.nHeight + 1, view, *params.modern_pos, hash_at,
                                            error, withdrawal_root_at);
}

std::optional<std::pair<int, uint256>> FinalityTracker::PinnedCheckpoint() const
{
    std::optional<std::pair<int, uint256>> pin;
    if (m_state.finalized) pin = std::make_pair(m_state.finalized->height, m_state.finalized->block_hash);
    if (m_state.fresh && m_state.fresh->finalized && (!pin || m_state.fresh->finalized->height > pin->first)) {
        pin = std::make_pair(m_state.fresh->finalized->height, m_state.fresh->finalized->block_hash);
    }
    return pin;
}

bool FinalityTracker::JudgeCandidateCertificateV2(const modern::FinalizedBlockV2& fb,
                                                  const modern::FinalityCertificate& cert,
                                                  const CBlockIndex& parent, const Consensus::Params& params,
                                                  std::string& error,
                                                  const BridgeStateIndex* bridge_index) const
{
    if (!Synced(parent.GetBlockHash())) {
        error = "finality-state-unavailable";
        return false;
    }
    const State projected{Projected(parent.nHeight + 1, params)};
    if (projected.fresh_unavailable) {
        error = "finality-state-unavailable";
        return false;
    }
    const auto hash_at{[&parent](const int h) -> std::optional<uint256> {
        const CBlockIndex* anc{parent.GetAncestor(h)};
        if (!anc) return std::nullopt;
        return anc->GetBlockHash();
    }};
    return JudgeFreshCertificate(fb, cert, parent.nHeight + 1, params, projected.fresh, hash_at, bridge_index,
                                 error);
}

bool FinalityTracker::ApplyModern(const CBlock& block, const CBlockIndex& index,
                                  const Consensus::Params& params,
                                  const BridgeStateIndex* bridge_index)
{
    // H1: the block's own consensus-checked round decides whether it is an
    // escape block (block data only: the exact timestamps of the block and
    // its parent). A connected block always has an exact round.
    const bool h1_active{params.H1RulesActive(index.nHeight)};
    std::optional<int64_t> round;
    if (h1_active) {
        if (index.pprev && params.modern_pos) {
            round = modern::DecodeModernPosRound(index.pprev->GetBlockTime(), block.GetBlockTime(),
                                                 *params.modern_pos);
        }
        if (!round) {
            LogWarning("FinalityTracker: connected block %s (height %d) has no exact modern-PoS round",
                       index.GetBlockHash().ToString(), index.nHeight);
            return false;
        }
    }
    State s{Projected(index.nHeight, params, round)};
    if (h1_active && !s.producers) {
        // Never store a guessed producer schedule: rebuild through Sync().
        LogWarning("FinalityTracker: connected block %s (height %d): H1 producer schedule unavailable",
                   index.GetBlockHash().ToString(), index.nHeight);
        return false;
    }
    if (s.fresh_unavailable) {
        // Never store a guessed fresh state: the caller marks the tracker
        // dirty and the next Sync() rebuilds it.
        LogWarning("FinalityTracker: connected block %s (height %d): fresh-lineage committee unavailable",
                   index.GetBlockHash().ToString(), index.nHeight);
        return false;
    }
    std::optional<modern::FinalityCertificatePair> pair;
    std::optional<modern::FinalityCertificateV2Record> fresh_record;
    std::string error;
    if (!JudgeCoinbaseCertificate(block, index, params, s, pair, fresh_record, error,
                                  bridge_index)) {
        LogWarning("FinalityTracker: connected block %s (height %d) carries an invalid certificate (%s)",
                   index.GetBlockHash().ToString(), index.nHeight, error);
        return false;
    }
    if (pair) {
        const auto& fb{pair->finalized_block};
        s.finalized = FinalizedCheckpoint{static_cast<int>(fb.height), fb.block_hash, fb.epoch, index.nHeight};
        if (fb.epoch == s.epoch) s.handover_certified = true;
    }
    if (fresh_record) {
        const auto hash_at{[&index](const int h) -> std::optional<uint256> {
            const CBlockIndex* anc{index.GetAncestor(h)};
            if (!anc) return std::nullopt;
            return anc->GetBlockHash();
        }};
        if (!s.fresh || !params.modern_pos ||
            !ApplyFreshCertificate(*s.fresh, *fresh_record, index.nHeight, hash_at,
                                   params.modern_pos->checkpoint_interval)) {
            LogWarning("FinalityTracker: connected block %s (height %d): fresh-lineage history unavailable",
                       index.GetBlockHash().ToString(), index.nHeight);
            return false;
        }
    }
    m_state = std::move(s);
    return true;
}

// ---------------------------------------------------------------- sync

bool FinalityTracker::StepTrackers(const CBlock& block, const CBlockIndex& index, const CChain* chain,
                                   const BlockManager* blockman, const Consensus::Params& params)
{
    const uint256 parent{index.pprev ? index.pprev->GetBlockHash() : uint256{}};
    if (index.pprev && m_stakes.Synced(parent) && m_bindings.Synced(parent)) {
        m_stakes.BlockConnected(block, index, params);
        m_bindings.BlockConnected(block, index, params);
    } else {
        if (!chain || !blockman) return false;
        if (!m_stakes.Sync(*chain, *blockman, params, index) || !m_bindings.Sync(*chain, *blockman, params, index)) {
            return false;
        }
    }
    // B-37 fresh keys (regtest -b3freshkeyheight only): the same in-step /
    // rebuild discipline; never stepped without the knob.
    if (FreshKeyModeConfigured(params)) {
        if (index.pprev && m_fresh_keys.Synced(parent)) {
            m_fresh_keys.BlockConnected(block, index, params);
        } else if (!chain || !blockman || !m_fresh_keys.Sync(*chain, *blockman, params, index)) {
            return false;
        }
        if (!m_fresh_keys.Synced(index.GetBlockHash())) return false;
    }
    return m_stakes.Synced(index.GetBlockHash()) && m_bindings.Synced(index.GetBlockHash());
}

const FreshKeyBindingIndex* FinalityTracker::FreshKeys(const Consensus::Params& params) const
{
    if (!FreshKeyModeConfigured(params) || m_dirty || !m_fresh_keys.Synced(m_synced_tip)) return nullptr;
    return &m_fresh_keys.Index();
}

bool FinalityTracker::Sync(const CChain& chain, const BlockManager& blockman, const Consensus::Params& params,
                           const CBlockIndex& target,
                           const BridgeStateIndex* bridge_index)
{
    const bool fresh_keys{FreshKeyModeConfigured(params)};
    if (Synced(target.GetBlockHash()) && (!fresh_keys || m_fresh_keys.Synced(target.GetBlockHash()))) return true;
    if (chain[target.nHeight] != &target) return false; // Target must lie on the chain.

    const std::optional<int> final_height{Consensus::LegacyFinalHeight(params)};
    if (!final_height) return false; // No modern era configured: nothing to track.
    const std::optional<int> modern_start{Consensus::ModernPosStartHeight(params)};

    int start{*final_height + 1};
    if (!m_dirty && m_synced_height >= *final_height && m_synced_height <= target.nHeight &&
        chain[m_synced_height] != nullptr && chain[m_synced_height]->GetBlockHash() == m_synced_tip) {
        start = m_synced_height + 1;
        // Fresh-key mode: the B-37 committees of the next projection read the
        // fresh key index at the resumed tip, so bring it there first (a knob
        // configured after the last walk leaves it behind the other trackers).
        if (fresh_keys && !m_fresh_keys.Synced(m_synced_tip) &&
            !m_fresh_keys.Sync(chain, blockman, params, *chain[m_synced_height])) {
            LogWarning("FinalityTracker: fresh key state unavailable at height %d", m_synced_height);
            Reset();
            return false;
        }
    } else {
        Reset();
        ++m_rebuilds;
        // Empty state "as of H": the first modern-era block extends it.
        m_synced_height = *final_height;
        if (const CBlockIndex* h{chain[*final_height]}) m_synced_tip = h->GetBlockHash();
    }

    for (int height{start}; height <= target.nHeight; ++height) {
        const CBlockIndex* pindex{chain[height]};
        CBlock block;
        if (pindex == nullptr || !blockman.ReadBlock(block, *pindex)) {
            LogWarning("FinalityTracker: cannot read modern block at height %d; finality state unavailable", height);
            Reset();
            return false;
        }
        // Only the blocks the undo history can still hold keep their prior State.
        std::optional<State> prior;
        if (static_cast<size_t>(target.nHeight - height) < StakeTracker::UndoDepth(params)) prior = m_state;
        if (modern_start && height >= *modern_start &&
            !ApplyModern(block, *pindex, params, bridge_index)) {
            Reset();
            return false;
        }
        if (!StepTrackers(block, *pindex, &chain, &blockman, params)) {
            LogWarning("FinalityTracker: stake/binding state unavailable at height %d", height);
            Reset();
            return false;
        }
        m_synced_tip = pindex->GetBlockHash();
        m_synced_height = height;
        if (prior) PushUndo(m_synced_tip, std::move(*prior), params);
    }
    // Fresh-key mode: an empty span (target == H) steps nothing, and a knob
    // configured after the last walk leaves the fresh key index behind the
    // other trackers; bring it to the target on its own.
    if (fresh_keys && !m_fresh_keys.Synced(target.GetBlockHash()) &&
        !m_fresh_keys.Sync(chain, blockman, params, target)) {
        LogWarning("FinalityTracker: fresh key state unavailable at height %d", target.nHeight);
        Reset();
        return false;
    }
    m_synced_tip = target.GetBlockHash();
    m_synced_height = target.nHeight;
    m_dirty = false;
    return true;
}

void FinalityTracker::BlockConnected(const CBlock& block,
                                     const CBlockIndex& index,
                                     const Consensus::Params& params,
                                     const BridgeStateIndex* bridge_index)
{
    if (Consensus::GetB3Era(index.nHeight, params) != Consensus::B3Era::MODERN) return;
    if (m_dirty || index.pprev == nullptr || m_synced_tip != index.pprev->GetBlockHash()) {
        m_dirty = true;
        return;
    }
    // Fresh-key mode: the B-37 committees projected for this block read the
    // fresh key index at the parent; out of step, rebuild through Sync().
    if (FreshKeyModeConfigured(params) && !m_fresh_keys.Synced(m_synced_tip)) {
        m_dirty = true;
        return;
    }
    const std::optional<int> modern_start{Consensus::ModernPosStartHeight(params)};
    State prior{m_state};
    if (modern_start && index.nHeight >= *modern_start &&
        !ApplyModern(block, index, params, bridge_index)) {
        m_dirty = true;
        return;
    }
    if (!StepTrackers(block, index, nullptr, nullptr, params)) {
        m_dirty = true;
        return;
    }
    m_synced_tip = index.GetBlockHash();
    m_synced_height = index.nHeight;
    PushUndo(m_synced_tip, std::move(prior), params);
}

void FinalityTracker::PushUndo(const uint256& hash, State prior, const Consensus::Params& params)
{
    if (FreshKeyModeConfigured(params)) {
        m_undo.clear();
        return;
    }
    const size_t depth{StakeTracker::UndoDepth(params)};
    if (depth == 0) return;
    m_undo.push_back(BlockUndo{.hash = hash, .prior = std::move(prior)});
    while (m_undo.size() > depth) m_undo.pop_front();
}

void FinalityTracker::BlockDisconnected(const CBlockIndex& index, const Consensus::Params& params)
{
    if (m_dirty || index.pprev == nullptr || m_synced_tip != index.GetBlockHash() ||
        FreshKeyModeConfigured(params) || m_undo.empty() || m_undo.back().hash != m_synced_tip) {
        // Not synced to this block, its record is no longer kept, or the
        // fresh key index (no undo trail) is in use: rebuild on next Sync().
        m_dirty = true;
        return;
    }
    const uint256 parent{index.pprev->GetBlockHash()};
    m_stakes.BlockDisconnected(index);
    m_bindings.BlockDisconnected(index);
    if (!m_stakes.Synced(parent) || !m_bindings.Synced(parent)) {
        m_dirty = true;
        return;
    }
    m_state = std::move(m_undo.back().prior);
    m_undo.pop_back();
    m_tip_snapshot = TipSnapshotMemo{};
    m_synced_tip = parent;
    m_synced_height = index.pprev->nHeight;
}

// ------------------------------------------------------------ B0b record

namespace {

//! Sets compared by value: the header, the members and each member's
//! binding_seq, which operator== leaves out.
bool SameSet(const ValidatorSetSnapshot& a, const ValidatorSetSnapshot& b)
{
    if (!(a == b)) return false;
    for (size_t i{0}; i < a.Size(); ++i) {
        if (a.Members()[i].binding_seq != b.Members()[i].binding_seq) return false;
    }
    return true;
}

bool SameSetRef(const std::shared_ptr<const ValidatorSetSnapshot>& a,
                const std::shared_ptr<const ValidatorSetSnapshot>& b)
{
    if (!a || !b) return !a && !b;
    return a == b || SameSet(*a, *b);
}

//! States compared field by field, sets by value. No persisted state has a
//! fresh lineage, so a state with one is never the same as another.
bool SameFinalityState(const FinalityTracker::State& a, const FinalityTracker::State& b)
{
    if (a.fresh || b.fresh || a.fresh_unavailable != b.fresh_unavailable ||
        a.bootstrapped != b.bootstrapped || a.epoch != b.epoch || a.epoch_starts != b.epoch_starts ||
        !SameSetRef(a.bootstrap, b.bootstrap) || !SameSetRef(a.previous, b.previous) ||
        !SameSetRef(a.current, b.current) || !SameSetRef(a.next, b.next) ||
        a.handover_certified != b.handover_certified || a.lineage_broken != b.lineage_broken ||
        a.finalized != b.finalized || a.producers.has_value() != b.producers.has_value()) {
        return false;
    }
    if (!a.producers) return true;
    const ProducerSchedule& p{*a.producers};
    const ProducerSchedule& q{*b.producers};
    return p.epoch == q.epoch && p.epoch_start == q.epoch_start && SameSetRef(p.current, q.current) &&
           SameSetRef(p.next, q.next) && p.last_escape_height == q.last_escape_height && p.escapes == q.escapes;
}

//! A set as read: what FromMembers takes, and the set hash it must give.
struct StoredSet {
    uint64_t epoch{0};
    uint256 hash;
    std::vector<ValidatorSetMember> members;
};

//! A state as read, its sets still numbers into the table (0: no set).
struct StoredState {
    FinalityTracker::State state;
    std::array<uint64_t, 4> sets{}; // bootstrap, previous, current, next
    uint64_t producer_current{0};
    uint64_t producer_next{0};
};

/**
 * The invariants the rules keep for a state as of the block `at` (design
 * section 6, item 4), checked on the numbers of its sets before any key is
 * verified: below M the empty state; from M one epoch start per epoch, the
 * first at M, rising, none above `at`; without Set_0 no set, handover,
 * break or checkpoint; with it the sets of epochs 0, e and e + 1, and of
 * e - 1 from epoch 1; a finalized checkpoint on this chain, in the epoch of
 * its height, certified at or above it and at or below `at`; and the H1
 * producer schedule exactly where H1 is active, started at or above R_h and
 * at or below `at`, its sets of epochs k and k + 1.
 */
bool StateInvariantsHold(const StoredState& stored, const std::vector<StoredSet>& sets, const CBlockIndex& at,
                         const int modern_start, const Consensus::Params& params)
{
    const FinalityTracker::State& s{stored.state};
    const auto epoch_of{[&sets](const uint64_t set) -> std::optional<uint64_t> {
        if (set == 0) return std::nullopt;
        return sets[set - 1].epoch;
    }};
    const bool no_set{std::ranges::all_of(stored.sets, [](const uint64_t set) { return set == 0; })};
    if (at.nHeight < modern_start) {
        return !s.bootstrapped && s.epoch == 0 && s.epoch_starts.empty() && no_set && !s.handover_certified &&
               !s.lineage_broken && !s.finalized && !s.producers;
    }
    if (s.epoch_starts.empty() || s.epoch_starts.size() - 1 != s.epoch || s.epoch_starts.front() != modern_start ||
        s.epoch_starts.back() > at.nHeight ||
        std::ranges::adjacent_find(s.epoch_starts, std::greater_equal<>{}) != s.epoch_starts.end()) {
        return false;
    }
    const auto [bootstrap, previous, current, next]{stored.sets};
    if (!s.bootstrapped) {
        if (s.epoch != 0 || !no_set || s.handover_certified || s.lineage_broken || s.finalized) return false;
    } else if (epoch_of(bootstrap) != 0 || epoch_of(current) != s.epoch || epoch_of(next) != s.epoch + 1 ||
               (s.epoch == 0 ? previous != 0 : epoch_of(previous) != s.epoch - 1)) {
        return false;
    }
    if (s.finalized) {
        const FinalizedCheckpoint& f{*s.finalized};
        const CBlockIndex* checkpoint{f.height >= modern_start ? at.GetAncestor(f.height) : nullptr};
        if (checkpoint == nullptr || checkpoint->GetBlockHash() != f.block_hash || f.certified_at < f.height ||
            f.certified_at > at.nHeight || modern::EpochOfHeight(s.epoch_starts, f.height) != f.epoch) {
            return false;
        }
    }
    if (s.producers.has_value() != params.H1RulesActive(at.nHeight)) return false;
    if (s.producers) {
        const ProducerSchedule& p{*s.producers};
        if (p.epoch_start < params.h1->activation_height || p.epoch_start > at.nHeight ||
            (stored.producer_current != 0 && epoch_of(stored.producer_current) != p.epoch) ||
            (stored.producer_next != 0 && epoch_of(stored.producer_next) != p.epoch + 1)) {
            return false;
        }
    }
    return true;
}

} // namespace

uint256 FinalityTracker::ParamsDigest(const Consensus::Params& params)
{
    // Keep in step with Sync, BlockConnected, ApplyModern and the
    // projections; with the private trackers (the binding tracker's digest,
    // and reorg_horizon for the undo depths); and with the bridge index
    // behind FinalityWithdrawalRoot, whose roots the certificate checks
    // read. Every busd_bridge pin, the bridge withdrawal activation and the
    // A1, A2 and A3 heights are in, read today or not: a superset costs one
    // rebuild after a change, a missing field a stale state.
    HashWriter h{};
    const auto height{[&h](const std::optional<int>& value) {
        h << value.has_value() << int32_t{value.value_or(0)};
    }};
    const auto optional_field{[&h](const auto& value) {
        h << value.has_value();
        if (value) h << *value;
    }};
    h << std::string{"B3/derived-state/finality-group"} << FinalityBindingTracker::ParamsDigest(params)
      << int32_t{params.transition_pow_length} << params.modern_pos.has_value();
    if (params.modern_pos) {
        const Consensus::ModernPosParams& p{*params.modern_pos};
        h << p.block_interval_seconds << p.round_seconds << p.f0_num << p.f0_den << p.sentinel_bits
          << p.max_future_seconds << p.reward << p.halving_interval << p.treasury_percent << p.treasury_script;
        height(p.reorg_horizon);
        h << int32_t{p.finality_epoch_blocks} << int32_t{p.checkpoint_interval} << int32_t{p.checkpoint_depth}
          << int32_t{p.max_epoch_extension} << int32_t{p.min_finality_set};
    }
    h << params.h1.has_value();
    if (params.h1) h << int32_t{params.h1->activation_height} << params.h1->escape_rounds;
    // Never written under these regtest knobs (Encode), so presence is all.
    h << params.fresh_lineage.has_value() << params.bridge_v2.has_value();
    height(params.fn_pod_activation_height);
    height(params.asset_activation_height);
    height(params.flowmesh_activation_height);
    height(params.bridge_withdrawal_activation_height);
    h << params.busd_bridge.has_value();
    if (params.busd_bridge) {
        const Consensus::BridgeAssetParams& b{*params.busd_bridge};
        h << b.asset.version << b.asset.origin_chain_id << b.asset.vault_address << b.asset.token_address
          << b.asset.origin_decimals << b.asset.asset_decimals;
        optional_field(b.origin_deployment_block);
        optional_field(b.vault_runtime_code_hash);
        optional_field(b.implementation_or_adapter);
        optional_field(b.adapter_version);
        optional_field(b.recipient_encoding_version);
        optional_field(b.activation_height);
        optional_field(b.approval_last_height);
        h << b.mint_caps.has_value();
        if (b.mint_caps) {
            h << b.mint_caps->max_per_block << b.mint_caps->max_per_epoch << b.mint_caps->epoch_length_blocks;
        }
        h << b.light_client.has_value();
        if (b.light_client) {
            const Consensus::EthereumLightClientPins& l{*b.light_client};
            h << l.trusted_checkpoint_root << l.trusted_checkpoint_slot << l.genesis_validators_root;
            WriteCompactSize(h, l.fork_schedule.size());
            for (const Consensus::EthereumForkVersionPin& fork : l.fork_schedule) {
                h << fork.activation_epoch << fork.fork_version;
            }
            h << l.fork_schedule_valid_through_epoch << l.electra_epoch
              << uint32_t{l.min_sync_committee_participants} << l.max_sync_lag_slots;
        }
        h << b.withdrawal_mode.has_value();
        if (b.withdrawal_mode) h << static_cast<uint8_t>(*b.withdrawal_mode);
        h << b.managed_withdrawal.has_value();
        if (b.managed_withdrawal) {
            const Consensus::BridgeManagedWithdrawalPins& m{*b.managed_withdrawal};
            h << m.authority_address << m.vault_runtime_code_hash << m.withdrawal_rules_version
              << m.withdrawal_rules_commitment;
        }
        h << b.decentralized_withdrawal.has_value();
        if (b.decentralized_withdrawal) {
            const Consensus::BridgeDecentralizedWithdrawalPins& d{*b.decentralized_withdrawal};
            h << d.ethereum_verifier_address << d.ethereum_verifier_code_hash << d.bootstrap_validator_set_hash
              << d.withdrawal_rules_version << d.withdrawal_rules_commitment << d.min_bridge_validators
              << d.max_bridge_validators << d.min_bridge_total_weight << d.max_epoch_lag;
        }
    }
    return h.GetSHA256();
}

std::optional<std::vector<unsigned char>> FinalityTracker::Encode(const Consensus::Params& params) const
{
    // Not persisted (section 4.3): the fresh lineage and bridge V2, regtest
    // knobs whose state this record does not hold; a chain without
    // modern-PoS rules; a tracker or private tracker out of step.
    if (params.fresh_lineage || params.bridge_v2 || !params.modern_pos) return std::nullopt;
    if (m_dirty || !m_stakes.Synced(m_synced_tip) || !m_bindings.Synced(m_synced_tip)) return std::nullopt;
    const auto pops{m_bindings.Index().EncodePops()};
    if (!pops) return std::nullopt;

    // The states first, numbering each distinct set as they first name it
    // (0: no set). The table that precedes them lists the sets in that order.
    std::vector<const ValidatorSetSnapshot*> table;
    std::map<const ValidatorSetSnapshot*, uint64_t> numbers;
    const auto number{[&](const std::shared_ptr<const ValidatorSetSnapshot>& set) -> uint64_t {
        if (!set) return 0;
        if (const auto it{numbers.find(set.get())}; it != numbers.end()) return it->second;
        // A set copied rather than shared is still one set.
        size_t n{0};
        while (n < table.size() && !SameSet(*table[n], *set)) ++n;
        if (n == table.size()) table.push_back(set.get());
        numbers.emplace(set.get(), n + 1);
        return n + 1;
    }};
    std::vector<unsigned char> states;
    VectorWriter w{states, 0};
    const auto write_state{[&](const State& s, const bool prior) {
        if (s.fresh || s.fresh_unavailable) return false;
        w << uint8_t{s.bootstrapped} << s.epoch;
        WriteCompactSize(w, s.epoch_starts.size());
        if (prior) {
            // A prior state is an ancestor's: its epoch starts begin the state's.
            if (s.epoch_starts.size() > m_state.epoch_starts.size() ||
                !std::equal(s.epoch_starts.begin(), s.epoch_starts.end(), m_state.epoch_starts.begin())) {
                return false;
            }
        } else {
            for (const int start : s.epoch_starts) w << int32_t{start};
        }
        for (const auto* set : {&s.bootstrap, &s.previous, &s.current, &s.next}) WriteCompactSize(w, number(*set));
        w << uint8_t{s.handover_certified} << uint8_t{s.lineage_broken} << uint8_t{s.finalized.has_value()};
        if (s.finalized) {
            w << int32_t{s.finalized->height} << s.finalized->block_hash << s.finalized->epoch
              << int32_t{s.finalized->certified_at};
        }
        w << uint8_t{s.producers.has_value()};
        if (s.producers) {
            const ProducerSchedule& p{*s.producers};
            w << p.epoch << int32_t{p.epoch_start};
            WriteCompactSize(w, number(p.current));
            WriteCompactSize(w, number(p.next));
            w << int32_t{p.last_escape_height} << p.escapes;
        }
        return true;
    }};
    if (!write_state(m_state, /*prior=*/false)) return std::nullopt;
    WriteCompactSize(w, m_undo.size());
    for (const BlockUndo& undo : m_undo) {
        w << undo.hash;
        if (!write_state(undo.prior, /*prior=*/true)) return std::nullopt;
    }

    std::vector<unsigned char> out;
    VectorWriter o{out, 0};
    o << m_bindings.Index().Encode() << *pops << m_stakes.Encode();
    WriteCompactSize(o, table.size());
    for (const ValidatorSetSnapshot* set : table) {
        o << set->Epoch() << set->SetHash();
        WriteCompactSize(o, set->Size());
        for (const ValidatorSetMember& m : set->Members()) {
            o << m.validator_key << m.bls_pubkey << m.weight << m.binding_seq;
        }
    }
    out.insert(out.end(), states.begin(), states.end());
    return out;
}

std::optional<FinalityTracker> FinalityTracker::Decode(const std::span<const unsigned char> bytes,
                                                       const CBlockIndex& block, const Consensus::Params& params)
{
    if (params.fresh_lineage || params.bridge_v2 || !params.modern_pos) return std::nullopt;
    const std::optional<int> final_height{Consensus::LegacyFinalHeight(params)};
    const std::optional<int> modern_start{Consensus::ModernPosStartHeight(params)};
    if (!final_height || !modern_start || block.nHeight < *final_height) return std::nullopt;

    std::optional<FinalityBindingIndex> bindings;
    std::optional<StakeTracker> stakes;
    std::vector<StoredSet> sets;
    StoredState tip;
    std::vector<std::pair<uint256, StoredState>> priors;
    try {
        SpanReader r{bytes};
        std::vector<unsigned char> part;
        r >> part;
        bindings = FinalityBindingIndex::Decode(part, *final_height + 1, block.nHeight);
        if (!bindings) return std::nullopt;
        r >> part;
        if (!bindings->DecodePops(part)) return std::nullopt;
        r >> part;
        stakes = StakeTracker::Decode(part, block, params);
        if (!stakes) return std::nullopt;

        const uint64_t count{ReadCompactSize(r)};
        for (uint64_t i{0}; i < count; ++i) {
            StoredSet set;
            r >> set.epoch >> set.hash;
            const uint64_t members{ReadCompactSize(r)};
            if (members == 0 || members > modern::MAX_FINALITY_SET) return std::nullopt;
            for (uint64_t j{0}; j < members; ++j) {
                ValidatorSetMember m;
                r >> m.validator_key >> m.bls_pubkey >> m.weight >> m.binding_seq;
                set.members.push_back(m);
            }
            sets.push_back(std::move(set));
        }
        std::vector<bool> named(sets.size());
        const auto read_set{[&](uint64_t& set) {
            set = ReadCompactSize(r);
            if (set > sets.size()) return false;
            if (set != 0) named[set - 1] = true;
            return true;
        }};
        const auto read_flag{[&](bool& flag) {
            uint8_t byte;
            r >> byte;
            flag = byte == 1;
            return byte <= 1;
        }};
        // `starts`: the state's epoch starts, of which a prior state's are a prefix.
        const auto read_state{[&](StoredState& stored, const std::vector<int>* starts) {
            State& s{stored.state};
            bool present;
            if (!read_flag(s.bootstrapped)) return false;
            r >> s.epoch;
            const uint64_t n{ReadCompactSize(r)};
            if (starts) {
                if (n > starts->size()) return false;
                s.epoch_starts.assign(starts->begin(), starts->begin() + n);
            } else {
                for (uint64_t i{0}; i < n; ++i) {
                    int32_t start;
                    r >> start;
                    s.epoch_starts.push_back(start);
                }
            }
            for (uint64_t& set : stored.sets) {
                if (!read_set(set)) return false;
            }
            if (!read_flag(s.handover_certified) || !read_flag(s.lineage_broken) || !read_flag(present)) return false;
            if (present) {
                FinalizedCheckpoint f;
                int32_t height;
                int32_t certified_at;
                r >> height >> f.block_hash >> f.epoch >> certified_at;
                f.height = height;
                f.certified_at = certified_at;
                s.finalized = f;
            }
            if (!read_flag(present)) return false;
            if (present) {
                ProducerSchedule p;
                int32_t start;
                int32_t escape;
                r >> p.epoch >> start;
                if (!read_set(stored.producer_current) || !read_set(stored.producer_next)) return false;
                r >> escape >> p.escapes;
                p.epoch_start = start;
                p.last_escape_height = escape;
                s.producers = std::move(p);
            }
            return true;
        }};
        if (!read_state(tip, nullptr)) return std::nullopt;
        const uint64_t records{ReadCompactSize(r)};
        if (records > StakeTracker::UndoDepth(params)) return std::nullopt;
        for (uint64_t i{0}; i < records; ++i) {
            uint256 hash;
            r >> hash;
            StoredState prior;
            if (!read_state(prior, &tip.state.epoch_starts)) return std::nullopt;
            priors.emplace_back(hash, std::move(prior));
        }
        // Every set is named by some state.
        if (!r.empty() || std::ranges::find(named, false) != named.end()) return std::nullopt;
    } catch (const std::ios_base::failure&) {
        return std::nullopt;
    }

    // The undo records name the chain's blocks ending at `block`, newest
    // last, all above H; each prior state is that of its block's parent.
    std::vector<const CBlockIndex*> parents(priors.size());
    const CBlockIndex* index{&block};
    for (size_t i{priors.size()}; i-- > 0;) {
        if (index == nullptr || index->pprev == nullptr || index->nHeight <= *final_height ||
            index->GetBlockHash() != priors[i].first) {
            return std::nullopt;
        }
        parents[i] = index->pprev;
        index = index->pprev;
    }
    if (!StateInvariantsHold(tip, sets, block, *modern_start, params)) return std::nullopt;
    for (size_t i{0}; i < priors.size(); ++i) {
        if (!StateInvariantsHold(priors[i].second, sets, *parents[i], *modern_start, params)) return std::nullopt;
    }

    // P1 (section 5): every key the binding index holds passes its proof of
    // possession, and the sets take their keys from these alone.
    const auto pops{bindings->KeyPops()};
    if (!pops) return std::nullopt;
    std::map<modern::BlsPubkeyBytes, bls::VerifiedPublicKey> keys;
    for (const auto& [key, pop] : *pops) {
        const auto pk{bls::PublicKey::Decode(key)};
        const auto sig{bls::Signature::Decode(pop)};
        if (!pk || !sig) return std::nullopt;
        const auto verified{bls::VerifiedPublicKey::FromPoP(*pk, *sig)};
        if (!verified) return std::nullopt;
        keys.emplace(key, *verified);
    }
    std::vector<std::shared_ptr<const ValidatorSetSnapshot>> snapshots;
    snapshots.reserve(sets.size());
    for (StoredSet& set : sets) {
        auto snapshot{ValidatorSetSnapshot::FromMembers(set.epoch, std::move(set.members), keys)};
        if (!snapshot || snapshot->SetHash() != set.hash) return std::nullopt;
        snapshots.push_back(std::make_shared<const ValidatorSetSnapshot>(std::move(*snapshot)));
    }

    const auto set_of{[&snapshots](const uint64_t set) -> std::shared_ptr<const ValidatorSetSnapshot> {
        return set == 0 ? nullptr : snapshots[set - 1];
    }};
    const auto wire{[&set_of](StoredState& stored) {
        State& s{stored.state};
        s.bootstrap = set_of(stored.sets[0]);
        s.previous = set_of(stored.sets[1]);
        s.current = set_of(stored.sets[2]);
        s.next = set_of(stored.sets[3]);
        if (s.producers) {
            s.producers->current = set_of(stored.producer_current);
            s.producers->next = set_of(stored.producer_next);
        }
        return std::move(s);
    }};
    FinalityTracker tracker;
    tracker.m_state = wire(tip);
    for (auto& [hash, prior] : priors) tracker.m_undo.push_back(BlockUndo{.hash = hash, .prior = wire(prior)});
    tracker.m_stakes = std::move(*stakes);
    tracker.m_bindings.Install(std::move(*bindings), block);
    tracker.m_synced_tip = block.GetBlockHash();
    tracker.m_synced_height = block.nHeight;
    tracker.m_dirty = false;
    return tracker;
}

bool FinalityTracker::SameState(const FinalityTracker& rebuilt) const
{
    if (m_dirty || rebuilt.m_dirty || m_synced_tip != rebuilt.m_synced_tip ||
        m_synced_height != rebuilt.m_synced_height || !SameFinalityState(m_state, rebuilt.m_state) ||
        m_undo.size() > rebuilt.m_undo.size()) {
        return false;
    }
    const size_t skip{rebuilt.m_undo.size() - m_undo.size()};
    for (size_t i{0}; i < m_undo.size(); ++i) {
        const BlockUndo& ours{m_undo[i]};
        const BlockUndo& theirs{rebuilt.m_undo[skip + i]};
        if (ours.hash != theirs.hash || !SameFinalityState(ours.prior, theirs.prior)) return false;
    }
    return m_stakes.SameState(rebuilt.m_stakes) && m_bindings.Synced(m_synced_tip) &&
           rebuilt.m_bindings.Synced(m_synced_tip) && m_bindings.Index().SameState(rebuilt.m_bindings.Index()) &&
           m_bindings.Index().KeyPops() == rebuilt.m_bindings.Index().KeyPops();
}

} // namespace node
