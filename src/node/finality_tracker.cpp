// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/finality_tracker.h>

#include <chain.h>
#include <consensus/era.h>
#include <logging.h>
#include <modern/chain_domain.h>
#include <modern/finality_certificate.h>
#include <modern/pos_v1.h>
#include <node/blockstorage.h>
#include <node/bridge_state.h>
#include <util/check.h>

#include <algorithm>
#include <limits>

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
}

} // namespace node
