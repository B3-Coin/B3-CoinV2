// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/finality_fresh_lineage.h>

#include <chain.h>
#include <consensus/era.h>
#include <logging.h>
#include <modern/chain_domain.h>
#include <modern/finality_schedule.h>
#include <node/bridge_state.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace node {

namespace {

std::optional<uint256> ChainDomain(const Consensus::Params& params)
{
    if (!params.legacy_final_hash) return std::nullopt;
    return modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash);
}

//! Checkpoint height of history leaf i (G + i * interval), if it fits an int.
std::optional<int> LeafHeight(const int genesis_height, const uint64_t i, const int interval)
{
    if (genesis_height < 0 || interval <= 0) return std::nullopt;
    const uint64_t max_steps{static_cast<uint64_t>(std::numeric_limits<int>::max() - genesis_height) /
                             static_cast<uint64_t>(interval)};
    if (i > max_steps) return std::nullopt;
    return genesis_height + static_cast<int>(i) * interval;
}

//! Append leaves [state.count, end) of the chain `hash_at` describes.
bool AppendHistoryLeaves(modern::FinalityHistoryState& tree, const uint64_t end, const int genesis_height,
                         const int interval, const FreshHashAtFn& hash_at)
{
    for (uint64_t i{tree.count}; i < end; ++i) {
        const auto h{LeafHeight(genesis_height, i, interval)};
        if (!h) return false;
        const auto hash{hash_at(*h)};
        if (!hash) return false;
        if (!modern::AppendFinalityHistoryLeaf(tree, modern::FinalityHistoryLeaf(static_cast<uint64_t>(*h), *hash))) {
            return false;
        }
    }
    return true;
}

} // namespace

// ------------------------------------------------------------------ sets

std::shared_ptr<const FreshLineageSet> MakeFreshLineageSet(std::shared_ptr<const ValidatorSetSnapshot> members,
                                                           const uint64_t epoch)
{
    if (!members) return nullptr;
    auto set{std::make_shared<FreshLineageSet>()};
    set->header = modern::RulesetV2Header(members->Header(), epoch);
    set->set_hash = modern::ValidatorSetHash(set->header);
    set->members = std::move(members);
    return set;
}

// ----------------------------------------------------------------- state

modern::FreshLineageEpochView FreshLineageState::View() const
{
    modern::FreshLineageEpochView v;
    v.lineage_id = lineage_id;
    v.genesis_height = genesis_height;
    v.current_epoch = epoch;
    v.epoch_starts = epoch_starts;
    v.lineage_broken = lineage_broken;
    if (finalized) v.finalized_height = static_cast<uint64_t>(finalized->height);
    if (!bootstrapped) return v; // no committee: NO_FINALITY_SET
    if (current) {
        v.current_set = &current->View();
        v.current_set_hash = current->set_hash;
    }
    if (next) v.next_set_hash = next->set_hash;
    if (previous) v.previous_set = &previous->View();
    return v;
}

std::shared_ptr<const FreshLineageSet> FreshLineageState::SetForEpoch(const uint64_t e) const
{
    if (!bootstrapped) return nullptr;
    if (e == epoch) return current;
    if (epoch >= 1 && e == epoch - 1) return previous;
    return nullptr;
}

std::optional<uint32_t> FreshLineageState::SetSize(const uint64_t e) const
{
    const auto set{SetForEpoch(e)};
    if (!set) return std::nullopt;
    return static_cast<uint32_t>(set->Size());
}

static_assert(FRESH_LINEAGE_RETAINED_PAST_EPOCHS == 2, "the state retains previous (e-1) and previous2 (e-2)");

std::optional<uint256> FreshLineageState::SetHashForEpoch(const uint64_t k) const
{
    if (!bootstrapped) return std::nullopt;
    // Written without epoch + 1 / epoch - 2, which could wrap.
    std::shared_ptr<const FreshLineageSet> set;
    if (k == epoch) {
        set = current;
    } else if (k > epoch && k - epoch == 1) {
        set = next;
    } else if (k < epoch && epoch - k == 1) {
        set = previous;
    } else if (k < epoch && epoch - k == 2) {
        set = previous2;
    }
    if (!set) return std::nullopt;
    return set->set_hash;
}

std::optional<uint256> FreshLineageState::SuccessorHashForEpoch(const uint64_t e) const
{
    if (!bootstrapped) return std::nullopt;
    if (e == epoch && next) return next->set_hash;
    if (epoch >= 1 && e == epoch - 1 && previous && current) return current->set_hash;
    if (epoch >= 2 && e == epoch - 2 && previous2 && previous) return previous->set_hash;
    return std::nullopt;
}

LineageV2View MakeLineageV2View(const Consensus::Params& params, const std::optional<FreshLineageState>& state)
{
    LineageV2View v;
    v.configured = params.fresh_lineage.has_value();
    if (params.fresh_lineage) v.genesis_height = params.fresh_lineage->GenesisHeight();
    if (!v.configured || !state) return v;
    v.bootstrapped = state->bootstrapped;
    v.broken = state->lineage_broken;
    v.lineage_id = state->lineage_id;
    v.genesis_height = state->genesis_height;
    v.epoch = state->epoch;
    if (state->current) {
        v.current_set_hash = state->current->set_hash;
        v.current_count = static_cast<uint32_t>(state->current->Size());
        v.current_weight = state->current->TotalWeight();
    }
    if (state->next) {
        v.next_set_hash = state->next->set_hash;
        v.next_count = static_cast<uint32_t>(state->next->Size());
        v.next_weight = state->next->TotalWeight();
    }
    if (state->finalized) v.finalized_height = state->finalized->height;
    v.handover_certified = state->handover_certified;
    return v;
}

// ------------------------------------------------------------ bridge hooks

std::optional<uint256> FreshLineageWithdrawalRoot(const int height, const Consensus::Params& params,
                                                  const BridgeStateIndex* bridge_index)
{
    // Zero below V (and wherever bridge_v2 is unset), the indexed V2 tree
    // root from V; nullopt when the index cannot answer (header comment).
    return FinalityWithdrawalRootV2(height, params, bridge_index);
}

std::optional<uint8_t> FreshLineageBridgeFlags(const int height, const Consensus::Params& params,
                                               const BridgeStateIndex* bridge_index)
{
    // 0 below V (and wherever bridge_v2 is unset), bit 0 from the first
    // checkpoint at or after the sticky conflict; nullopt when the index
    // cannot answer (header comment).
    return FinalityBridgeFlagsV2(height, params, bridge_index);
}

// --------------------------------------------------------------- history

std::optional<uint256> FreshHistoryRootAt(const FreshLineageState& state, const uint64_t k,
                                          const FreshHashAtFn& hash_at, const int interval)
{
    if (!state.bootstrapped || k >= modern::MAX_FINALITY_CHECKPOINT_INDEX) return std::nullopt;
    modern::FinalityHistoryState tree;
    if (k >= state.history.count) tree = state.history; // extend the accumulator
    if (!AppendHistoryLeaves(tree, k, state.genesis_height, interval, hash_at)) return std::nullopt;
    return tree.root;
}

// ------------------------------------------------------------ projection

bool ProjectFreshLineage(std::optional<FreshLineageState>& fresh, const int height, const uint256& parent_hash,
                         const Consensus::Params& params, const FreshSnapshotAtFn& snapshot_at,
                         const FreshCommitteeAtFn& committee_at)
{
    if (!params.fresh_lineage || !params.modern_pos) {
        fresh.reset();
        return true;
    }
    const Consensus::FreshLineageParams& fl{*params.fresh_lineage};
    const Consensus::ModernPosParams& pos{*params.modern_pos};
    if (height < fl.activation_height) {
        fresh.reset();
        return true;
    }
    const int E{pos.finality_epoch_blocks};
    // B-37 seat rules (header comment) replace the V1-key snapshots only
    // when the caller supplies them and params set R_key.
    const bool seat_rules{committee_at && fl.key_height.has_value()};
    // One committee for `request`: the seat rules' (an unavailable answer
    // ends the projection, never a guessed committee) or the V1 snapshot.
    const auto committee{[&](const FreshCommitteeRequest& request) -> FreshCommittee {
        if (!seat_rules) return FreshCommittee{snapshot_at(request.snapshot_height)};
        return committee_at(request);
    }};

    if (height == fl.activation_height) {
        // Genesis: the release itself authorizes Set'_0 (no old-set vote),
        // exactly as V1's bootstrap at M does.
        FreshLineageState s;
        s.genesis_height = fl.GenesisHeight();
        s.genesis_block_hash = parent_hash;
        s.epoch = 0;
        s.epoch_starts = {height};
        const auto domain{ChainDomain(params)};
        // Rule (3): one genesis binding (naming 0, included in [R_key,
        // R_f - 1]) serves Set'_0 and Set'_1. Below the floor there is no
        // fallback to the V1 keys: the lineage stays unbootstrapped.
        const FreshCommittee genesis{committee(FreshCommitteeRequest{height - 1, 0, fl.key_height.value_or(-1)})};
        if (!genesis.available) {
            fresh.reset();
            return false;
        }
        const auto& snap{genesis.members};
        if (domain && snap && snap->Size() >= static_cast<size_t>(pos.min_finality_set) && !parent_hash.IsNull()) {
            const auto members{std::make_shared<const ValidatorSetSnapshot>(*snap)};
            s.current = MakeFreshLineageSet(members, 0);
            s.genesis = s.current;
            s.next = MakeFreshLineageSet(members, 1);
            s.lineage_id = modern::FinalityLineageIdV2(*domain, static_cast<uint64_t>(s.genesis_height),
                                                       parent_hash, s.current->set_hash);
            // History leaf 0 is GENESIS_HEIGHT itself (never signed as an index).
            s.bootstrapped = modern::AppendFinalityHistoryLeaf(
                s.history, modern::FinalityHistoryLeaf(static_cast<uint64_t>(s.genesis_height), parent_hash));
        }
        fresh = std::move(s);
        return true;
    }
    if (!fresh || !fresh->bootstrapped || fresh->epoch_starts.empty()) return true;
    FreshLineageState& s{*fresh};

    // Handover-gated rotation (V1's rule, V1's E).
    if (s.handover_certified && height >= s.epoch_starts.back() + E) {
        // start_{e-1} for the epoch e entered here: the B-37 window of the
        // bindings that fix Set'_{e+1} opens there.
        const int window_lo{s.epoch_starts.back()};
        s.previous2 = s.previous; // Set'_{e-2}, for the bridge V2 verifier judgement
        s.previous = s.current;
        s.current = s.next;
        s.epoch += 1;
        s.epoch_starts.push_back(height);
        s.handover_certified = false;
        const uint64_t successor_epoch{s.epoch + 1};
        // Rule (2): bindings naming e+1 included in [start_{e-1}, start_e - 1].
        const FreshCommittee successor{committee(FreshCommitteeRequest{height - 1, successor_epoch, window_lo})};
        if (!successor.available) {
            fresh.reset();
            return false;
        }
        const auto& snap{successor.members};
        if (snap && snap->Size() >= static_cast<size_t>(pos.min_finality_set)) {
            s.next = MakeFreshLineageSet(std::make_shared<const ValidatorSetSnapshot>(*snap), successor_epoch);
        } else {
            // Carry-over (rule (4) under B-37): the committee in force
            // serves another epoch with its members and keys.
            s.next = MakeFreshLineageSet(s.current->members, successor_epoch);
        }
    }
    // The fresh lineage's own extension (FD-7) exhausted: broken for good.
    if (!s.handover_certified && !s.lineage_broken &&
        static_cast<int64_t>(height) - s.epoch_starts.back() >= int64_t{E} + fl.max_epoch_extension) {
        s.lineage_broken = true;
    }
    return true;
}

// ------------------------------------------------------------- judgement

bool JudgeFreshCertificate(const modern::FinalizedBlockV2& fb, const modern::FinalityCertificate& cert,
                           const int including_height, const Consensus::Params& params,
                           const std::optional<FreshLineageState>& projected, const FreshHashAtFn& hash_at,
                           const BridgeStateIndex* bridge_index, std::string& error)
{
    if (!params.fresh_lineage || !projected || !projected->bootstrapped) {
        error = "fresh-cert-inactive";
        return false;
    }
    if (!params.modern_pos) {
        error = "no-modern-pos-rules";
        return false;
    }
    const auto domain{ChainDomain(params)};
    if (!domain) {
        error = "finality-domain-unpinned";
        return false;
    }
    const int interval{params.modern_pos->checkpoint_interval};
    const FreshLineageState& state{*projected};
    const modern::FreshLineageEpochView view{state.View()};
    const auto history_root_at{[&state, &hash_at, interval](const uint64_t k) {
        return FreshHistoryRootAt(state, k, hash_at, interval);
    }};
    const auto withdrawal_root_at{[&params, bridge_index](const int h) {
        return FreshLineageWithdrawalRoot(h, params, bridge_index);
    }};
    const auto bridge_flags_at{[&params, bridge_index](const int h) {
        return FreshLineageBridgeFlags(h, params, bridge_index);
    }};
    return modern::JudgeFinalityCertificateV2(*domain, fb, cert, including_height, view, *params.modern_pos, hash_at,
                                              history_root_at, withdrawal_root_at, bridge_flags_at, error);
}

bool JudgeFreshCoinbaseCertificate(const CBlock& block, const CBlockIndex& index, const Consensus::Params& params,
                                   const std::optional<FreshLineageState>& projected, const FreshHashAtFn& hash_at,
                                   const BridgeStateIndex* bridge_index,
                                   std::optional<modern::FinalityCertificateV2Record>& out, std::string& error)
{
    out.reset();
    if (block.vtx.empty()) {
        error = "finality-cert-no-coinbase";
        return false;
    }
    const CTransaction& coinbase{*block.vtx[0]};
    const bool carries{std::any_of(coinbase.mpa.begin(), coinbase.mpa.end(), [](const CMpaRecord& rec) {
        return rec.payload_type == modern::MPA_TYPE_FINALITY_CERTIFICATE_V2;
    })};
    if (!carries) return true;
    if (!params.fresh_lineage || !projected || !projected->bootstrapped) {
        error = "fresh-cert-inactive";
        return false;
    }
    const FreshLineageState& state{*projected};
    std::optional<modern::FinalityCertificateV2Record> record;
    if (!modern::MatchFinalityCertificateV2ForEpoch(
            coinbase, [&state](const uint64_t e) { return state.SetSize(e); }, record, error)) {
        return false;
    }
    if (!record) return true; // unreachable: `carries` found one
    if (!JudgeFreshCertificate(record->finalized_block, record->certificate, index.nHeight, params, projected,
                               hash_at, bridge_index, error)) {
        return false;
    }
    out = std::move(record);
    return true;
}

bool ApplyFreshCertificate(FreshLineageState& state, const modern::FinalityCertificateV2Record& record,
                           const int including_height, const FreshHashAtFn& hash_at, const int interval)
{
    const modern::FinalizedBlockV2& fb{record.finalized_block};
    if (!state.bootstrapped || fb.checkpoint_index >= modern::MAX_FINALITY_CHECKPOINT_INDEX) return false;
    // Extend the accumulator through leaf k (the certified checkpoint itself).
    modern::FinalityHistoryState tree{state.history};
    if (fb.checkpoint_index + 1 > tree.count &&
        !AppendHistoryLeaves(tree, fb.checkpoint_index + 1, state.genesis_height, interval, hash_at)) {
        return false;
    }
    state.history = tree;
    state.finalized = FreshFinalizedCheckpoint{
        .height = static_cast<int>(fb.height),
        .block_hash = fb.block_hash,
        .epoch = fb.epoch,
        .checkpoint_index = fb.checkpoint_index,
        .history_root = fb.checkpoint_history_root,
        .cert_id = modern::FinalityCertIdV2(fb),
        .certified_at = including_height,
    };
    if (fb.epoch == state.epoch) state.handover_certified = true;
    return true;
}

std::optional<modern::FinalizedBlockV2> ExpectedFinalizedBlockV2(const uint64_t epoch, const int height,
                                                                 const FreshLineageState& state,
                                                                 const FreshHashAtFn& hash_at,
                                                                 const Consensus::Params& params,
                                                                 const BridgeStateIndex* bridge_index)
{
    if (!params.modern_pos || !state.bootstrapped || state.lineage_broken) return std::nullopt;
    const int interval{params.modern_pos->checkpoint_interval};
    const auto index{modern::FinalityCheckpointIndexV2(height, state.genesis_height, interval)};
    if (!index || *index == 0) return std::nullopt;
    const auto successor{state.SuccessorHashForEpoch(epoch)};
    if (!successor) return std::nullopt;
    const auto block_hash{hash_at(height)};
    if (!block_hash) return std::nullopt;
    const auto history_root{FreshHistoryRootAt(state, *index, hash_at, interval)};
    if (!history_root) return std::nullopt;
    const auto withdrawal_root{FreshLineageWithdrawalRoot(height, params, bridge_index)};
    if (!withdrawal_root) return std::nullopt;
    const auto flags{FreshLineageBridgeFlags(height, params, bridge_index)};
    if (!flags) return std::nullopt;
    modern::FinalizedBlockV2 fb;
    fb.lineage_id = state.lineage_id;
    fb.height = static_cast<uint64_t>(height);
    fb.block_hash = *block_hash;
    fb.withdrawal_root = *withdrawal_root;
    fb.validator_set_hash = *successor;
    fb.epoch = epoch;
    fb.checkpoint_index = *index;
    fb.checkpoint_history_root = *history_root;
    fb.bridge_flags = *flags;
    return fb;
}

} // namespace node
