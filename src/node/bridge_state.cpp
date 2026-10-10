// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/bridge_state.h>

#include <bridge/admission_v2.h>
#include <bridge/burn_gate_v2.h>
#include <bridge/deposit.h>
#include <bridge/exec_chain.h>
#include <bridge/mpt.h>
#include <bridge/proof.h>
#include <bridge/reserve_v2.h>
#include <bridge/rlp.h>
#include <bridge/state_observation.h>
#include <chain.h>
#include <consensus/era.h>
#include <logging.h>
#include <modern/asset_output.h>
#include <modern/bridge_asset.h>
#include <modern/bridge_binding.h>
#include <modern/bridge_forwarder.h>
#include <modern/bridge_v2.h>
#include <modern/chain_domain.h>
#include <modern/finality_certificate.h>
#include <modern/finality_schedule.h>
#include <modern/finality_v2.h>
#include <node/blockstorage.h>
#include <node/finality_fresh_lineage.h>
#include <node/finality_tracker.h>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

namespace node {
namespace {

using AnchorMap = std::map<uint256, BridgeExecutionAnchor>;
using AnchorHeightMap = std::map<uint64_t, uint256>;
using NullifierSet = std::set<bridge::BridgeDepositKey>;
using EpochMintMap =
    std::map<std::pair<modern::AssetId, uint64_t>, CAmount>;
using WithdrawalMap =
    std::map<BridgeWithdrawalId, BridgeManagedWithdrawalRequest>;
using DecentralizedWithdrawalMap =
    std::map<uint64_t, BridgeDecentralizedWithdrawalRequest>;
using DecentralizedWithdrawalSourceMap =
    std::map<BridgeWithdrawalId, uint64_t>;
using WithdrawalV2Map = std::map<uint64_t, BridgeWithdrawalV2Request>;
using WithdrawalV2SourceMap = std::map<BridgeWithdrawalId, uint64_t>;

bool SameBeaconHeader(const bridge::ssz::BeaconBlockHeader& a,
                      const bridge::ssz::BeaconBlockHeader& b)
{
    return a.slot == b.slot && a.proposer_index == b.proposer_index &&
           a.parent_root == b.parent_root && a.state_root == b.state_root &&
           a.body_root == b.body_root;
}

bool SameExecutionHeader(const bridge::ssz::ExecutionPayloadHeader& a,
                         const bridge::ssz::ExecutionPayloadHeader& b)
{
    return a.parent_hash == b.parent_hash &&
           a.fee_recipient == b.fee_recipient &&
           a.state_root == b.state_root &&
           a.receipts_root == b.receipts_root &&
           a.logs_bloom == b.logs_bloom && a.prev_randao == b.prev_randao &&
           a.block_number == b.block_number && a.gas_limit == b.gas_limit &&
           a.gas_used == b.gas_used && a.timestamp == b.timestamp &&
           a.extra_data == b.extra_data &&
           a.base_fee_per_gas == b.base_fee_per_gas &&
           a.block_hash == b.block_hash &&
           a.transactions_root == b.transactions_root &&
           a.withdrawals_root == b.withdrawals_root &&
           a.blob_gas_used == b.blob_gas_used &&
           a.excess_blob_gas == b.excess_blob_gas;
}

bool SameLightClientHeader(const bridge::LightClientHeader& a,
                           const bridge::LightClientHeader& b)
{
    return SameBeaconHeader(a.beacon, b.beacon) &&
           SameExecutionHeader(a.execution, b.execution) &&
           a.execution_branch == b.execution_branch;
}

bool SameCommittee(const bridge::ssz::SyncCommittee& a,
                   const bridge::ssz::SyncCommittee& b)
{
    return a.pubkeys == b.pubkeys &&
           a.aggregate_pubkey == b.aggregate_pubkey;
}

bool SameLightClientStore(const bridge::LightClientStore& a,
                          const bridge::LightClientStore& b)
{
    if (!SameLightClientHeader(a.finalized_header, b.finalized_header) ||
        a.period != b.period || !SameCommittee(a.current, b.current) ||
        a.next.has_value() != b.next.has_value()) {
        return false;
    }
    return !a.next || SameCommittee(*a.next, *b.next);
}

bool SameOptionalStore(const std::optional<bridge::LightClientStore>& a,
                       const std::optional<bridge::LightClientStore>& b)
{
    if (a.has_value() != b.has_value()) return false;
    return !a || SameLightClientStore(*a, *b);
}

bridge::LightClientConfig LightClientConfigFor(
    const Consensus::EthereumLightClientPins& pins)
{
    bridge::LightClientConfig config;
    config.genesis_validators_root = pins.genesis_validators_root;
    config.electra_epoch = pins.electra_epoch;
    config.min_participants = pins.min_sync_committee_participants;
    config.forks.reserve(pins.fork_schedule.size());
    for (const Consensus::EthereumForkVersionPin& fork :
         pins.fork_schedule) {
        config.forks.push_back(
            bridge::ForkVersion{fork.activation_epoch, fork.fork_version});
    }
    return config;
}

bool SlotUsesKnownFork(const uint64_t slot,
                       const Consensus::EthereumLightClientPins& pins)
{
    return bridge::EpochAtSlot(slot) <=
           pins.fork_schedule_valid_through_epoch;
}

bool UpdateUsesKnownFork(const bridge::LightClientUpdate& update,
                         const Consensus::EthereumLightClientPins& pins)
{
    if (update.signature_slot == 0) return false;
    // The sync-committee signing domain uses the epoch immediately before
    // signature_slot when the slot is an epoch boundary.
    const uint64_t signing_epoch{
        bridge::EpochAtSlot(update.signature_slot - 1)};
    return signing_epoch <= pins.fork_schedule_valid_through_epoch &&
           SlotUsesKnownFork(update.attested.beacon.slot, pins) &&
           SlotUsesKnownFork(update.finalized.beacon.slot, pins);
}

bool FinalizedHeadFresh(const bridge::LightClientStore& store,
                        const int64_t candidate_time,
                        const Consensus::EthereumLightClientPins& pins)
{
    if (candidate_time < 0) return false;
    const uint64_t candidate{static_cast<uint64_t>(candidate_time)};
    const uint64_t finalized{store.finalized_header.execution.timestamp};
    if (candidate <= finalized) return true;
    if (pins.max_sync_lag_slots >
        std::numeric_limits<uint64_t>::max() / 12) {
        return false;
    }
    return candidate - finalized <= pins.max_sync_lag_slots * 12;
}

BridgeExecutionAnchor FinalizedAnchor(
    const bridge::LightClientHeader& header, const int height,
    const uint256& block_hash)
{
    return BridgeExecutionAnchor{
        header.execution.block_number,
        header.execution.block_hash,
        header.execution.receipts_root,
        header.beacon.slot,
        header.execution.block_number,
        header.execution.timestamp,
        height,
        block_hash,
        header.execution.state_root,
    };
}

bool HasValidBackfillOrigin(const BridgeExecutionAnchor& anchor)
{
    return anchor.source_finalized_execution_block >= anchor.block_number &&
           anchor.source_finalized_execution_block - anchor.block_number <=
               bridge::MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS;
}

bool ExecutionRangeReachable(const uint64_t newer_block,
                             const uint64_t older_block)
{
    return newer_block >= older_block &&
           newer_block - older_block <=
               bridge::MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS;
}

bool CheckCumulativeBackfillTarget(const BridgeExecutionAnchor& source,
                                   const uint64_t target_block_number,
                                   std::string& error)
{
    if (!HasValidBackfillOrigin(source) ||
        target_block_number > source.source_finalized_execution_block ||
        source.source_finalized_execution_block - target_block_number >
            bridge::MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS) {
        error = "bridge execution proof exceeds the cumulative backfill limit";
        return false;
    }
    return true;
}

std::optional<uint64_t> ExecutionHeaderTimestamp(
    const std::vector<unsigned char>& encoded)
{
    const auto top{bridge::RlpDecode(encoded)};
    if (!top || !top->is_list) return std::nullopt;
    const auto fields{bridge::RlpChildren(*top)};
    if (!fields || fields->size() < 16) return std::nullopt;
    return bridge::deposit_detail::RlpUint64((*fields)[11]);
}

uint64_t BridgeEpoch(const int height,
                     const Consensus::BridgeAssetParams& params)
{
    return static_cast<uint64_t>(height - *params.activation_height) /
           params.mint_caps->epoch_length_blocks;
}

bool IsConfiguredAndActive(const Consensus::Params& params, const int height,
                           std::string& error)
{
    if (!params.busd_bridge ||
        !Consensus::BridgeMintParamsReady(*params.busd_bridge)) {
        error = "bridge consensus parameters are incomplete";
        return false;
    }
    if (height < *params.busd_bridge->activation_height) {
        error = "bridge consensus rules are not active";
        return false;
    }
    return true;
}

/**
 * Candidate-local overlays over one connected state. The light-client and
 * anchor references name the store the candidate height selects (V1 below
 * the bridge V2 height V, V2 from V); every other base is shared.
 */
struct ScratchState {
    ScratchState(
        const std::optional<bridge::LightClientStore>& light_client,
        const AnchorMap& anchors, const AnchorHeightMap& anchor_by_height,
        const NullifierSet& nullifiers, const EpochMintMap& epoch_minted,
        const WithdrawalMap& withdrawals,
        const DecentralizedWithdrawalMap& decentralized_withdrawals,
        const DecentralizedWithdrawalSourceMap& decentralized_sources,
        const modern::WithdrawalTreeState& withdrawal_tree,
        const WithdrawalV2Map& withdrawals_v2,
        const WithdrawalV2SourceMap& withdrawal_v2_sources,
        const modern::WithdrawalTreeState& withdrawal_tree_v2,
        const BridgeObservationSlotsV2& observations_v2,
        const bridge::ReserveLedgerV2& reserve_v2,
        const bridge::ProvenDepositMapV2& deposits_v2,
        const CAmount deposits_v2_total,
        const std::optional<BridgeV2Conflict>& conflict_v2,
        const bridge::ReserveLedgerV2& reserve_v2_successor,
        const bridge::ProvenDepositMapV2& deposits_v2_successor,
        const CAmount deposits_v2_successor_total)
        : light_client_base{light_client},
          anchors_base{anchors},
          anchor_by_height_base{anchor_by_height},
          nullifiers_base{nullifiers},
          epoch_minted_base{epoch_minted},
          withdrawals_base{withdrawals},
          decentralized_withdrawals_base{decentralized_withdrawals},
          decentralized_sources_base{decentralized_sources},
          withdrawals_v2_base{withdrawals_v2},
          withdrawal_v2_sources_base{withdrawal_v2_sources},
          deposits_v2_base{deposits_v2},
          deposits_v2_base_total{deposits_v2_total},
          deposits_v2_successor_base{deposits_v2_successor},
          deposits_v2_successor_base_total{deposits_v2_successor_total},
          withdrawal_tree{withdrawal_tree},
          withdrawal_tree_v2{withdrawal_tree_v2},
          observations_v2{observations_v2},
          reserve_v2{reserve_v2},
          conflict_v2{conflict_v2},
          reserve_v2_successor{reserve_v2_successor}
    {
    }

    const std::optional<bridge::LightClientStore>& LightClientState() const
    {
        return light_client_changed ? light_client_overlay
                                    : light_client_base;
    }

    const bridge::LightClientStore* LightClient() const
    {
        const auto& state{LightClientState()};
        return state ? &*state : nullptr;
    }

    void SetLightClient(bridge::LightClientStore state)
    {
        light_client_overlay = std::move(state);
        light_client_changed = true;
    }

    const BridgeExecutionAnchor* Anchor(const uint256& hash) const
    {
        const auto overlay{anchors_overlay.find(hash)};
        if (overlay != anchors_overlay.end()) return &overlay->second;
        const auto base{anchors_base.find(hash)};
        return base == anchors_base.end() ? nullptr : &base->second;
    }

    //! An anchor connected by an earlier block (the base state only): kind 9
    //! observations never read an anchor added by their own block.
    const BridgeExecutionAnchor* BaseAnchor(const uint256& hash) const
    {
        const auto base{anchors_base.find(hash)};
        return base == anchors_base.end() ? nullptr : &base->second;
    }

    const uint256* AnchorHashAtHeight(const uint64_t height) const
    {
        const auto overlay{anchor_by_height_overlay.find(height)};
        if (overlay != anchor_by_height_overlay.end()) {
            return &overlay->second;
        }
        const auto base{anchor_by_height_base.find(height)};
        return base == anchor_by_height_base.end() ? nullptr : &base->second;
    }

    void AddAnchor(const BridgeExecutionAnchor& anchor)
    {
        anchors_overlay.emplace(anchor.block_hash, anchor);
        anchor_by_height_overlay.emplace(anchor.block_number,
                                         anchor.block_hash);
    }

    bool IsNullified(const bridge::BridgeDepositKey& key) const
    {
        return nullifiers_overlay.contains(key) ||
               nullifiers_base.contains(key);
    }

    bool AddNullifier(const bridge::BridgeDepositKey& key)
    {
        return nullifiers_overlay.insert(key).second;
    }

    CAmount EpochMinted(
        const std::pair<modern::AssetId, uint64_t>& key) const
    {
        const auto overlay{epoch_minted_overlay.find(key)};
        if (overlay != epoch_minted_overlay.end()) return overlay->second;
        const auto base{epoch_minted_base.find(key)};
        return base == epoch_minted_base.end() ? 0 : base->second;
    }

    void SetEpochMinted(const std::pair<modern::AssetId, uint64_t>& key,
                        const CAmount amount)
    {
        epoch_minted_overlay[key] = amount;
    }

    bool AddWithdrawal(const BridgeWithdrawalId& id)
    {
        if (withdrawals_base.contains(id)) return false;
        return withdrawals_overlay.insert(id).second;
    }

    bool AddDecentralizedWithdrawal(
        const BridgeWithdrawalId& source,
        const modern::BridgeWithdrawalV1& withdrawal, uint256& leaf)
    {
        if (decentralized_sources_base.contains(source) ||
            decentralized_sources_overlay.contains(source) ||
            decentralized_withdrawals_base.contains(
                withdrawal.withdrawal_id) ||
            decentralized_withdrawals_overlay.contains(
                withdrawal.withdrawal_id)) {
            return false;
        }
        const auto appended{modern::AppendBridgeWithdrawal(
            withdrawal_tree, withdrawal)};
        if (!appended) return false;
        leaf = *appended;
        decentralized_sources_overlay.insert(source);
        decentralized_withdrawals_overlay.insert(withdrawal.withdrawal_id);
        return true;
    }

    bool AddWithdrawalV2(const BridgeWithdrawalId& source,
                         const modern::BridgeWithdrawalV2& withdrawal,
                         uint256& leaf)
    {
        if (withdrawal_v2_sources_base.contains(source) ||
            withdrawal_v2_sources_overlay.contains(source) ||
            withdrawals_v2_base.contains(withdrawal.withdrawal_id) ||
            withdrawals_v2_overlay.contains(withdrawal.withdrawal_id)) {
            return false;
        }
        const auto appended{modern::AppendBridgeWithdrawalV2(
            withdrawal_tree_v2, withdrawal)};
        if (!appended) return false;
        leaf = *appended;
        withdrawal_v2_sources_overlay.insert(source);
        withdrawals_v2_overlay.insert(withdrawal.withdrawal_id);
        return true;
    }

    const std::optional<bridge::LightClientStore>& light_client_base;
    const AnchorMap& anchors_base;
    const AnchorHeightMap& anchor_by_height_base;
    const NullifierSet& nullifiers_base;
    const EpochMintMap& epoch_minted_base;
    const WithdrawalMap& withdrawals_base;
    const DecentralizedWithdrawalMap& decentralized_withdrawals_base;
    const DecentralizedWithdrawalSourceMap& decentralized_sources_base;
    const WithdrawalV2Map& withdrawals_v2_base;
    const WithdrawalV2SourceMap& withdrawal_v2_sources_base;
    const bridge::ProvenDepositMapV2& deposits_v2_base;
    //! The sum of deposits_v2_base (the index's running total).
    CAmount deposits_v2_base_total{0};
    //! The B-23 successor's connected deposits and their running total.
    const bridge::ProvenDepositMapV2& deposits_v2_successor_base;
    CAmount deposits_v2_successor_base_total{0};

    std::optional<bridge::LightClientStore> light_client_overlay{};
    bool light_client_changed{false};
    AnchorMap anchors_overlay{};
    AnchorHeightMap anchor_by_height_overlay{};
    NullifierSet nullifiers_overlay{};
    EpochMintMap epoch_minted_overlay{};
    std::set<BridgeWithdrawalId> withdrawals_overlay{};
    std::set<uint64_t> decentralized_withdrawals_overlay{};
    std::set<BridgeWithdrawalId> decentralized_sources_overlay{};
    std::set<uint64_t> withdrawals_v2_overlay{};
    std::set<BridgeWithdrawalId> withdrawal_v2_sources_overlay{};
    modern::WithdrawalTreeState withdrawal_tree{};
    modern::WithdrawalTreeState withdrawal_tree_v2{};
    CAmount minted_this_block{0};
    // Bridge V2 accounting, each initialised from the connected state and
    // updated record by record (the delta keeps the connected values).
    bridge::ProvenDepositMapV2 deposits_v2_overlay{};
    //! The sum of deposits_v2_overlay.
    CAmount deposits_v2_overlay_total{0};
    BridgeObservationSlotsV2 observations_v2{};
    bridge::ReserveLedgerV2 reserve_v2{};
    std::optional<BridgeV2Conflict> conflict_v2{};
    //! Set once an earlier record of this block stored a kind 9 observation
    //! or set the sticky conflict: the burn gate's inputs then differ from
    //! the connected state the caller's block-level gate read.
    bool gate_inputs_changed{false};
    //! The B-23 successor v''s ledger and the deposits this block proved
    //! into v' (node changes section 7.6), as for the launch entry above.
    bridge::ReserveLedgerV2 reserve_v2_successor{};
    bridge::ProvenDepositMapV2 deposits_v2_successor_overlay{};
    CAmount deposits_v2_successor_overlay_total{0};

    static bool IsSuccessor(const Consensus::BridgeV2EntryRole role)
    {
        return role == Consensus::BridgeV2EntryRole::SUCCESSOR;
    }

    //! The ledger of vault entry `role`.
    bridge::ReserveLedgerV2& Ledger(const Consensus::BridgeV2EntryRole role)
    {
        return IsSuccessor(role) ? reserve_v2_successor : reserve_v2;
    }

    bool HasProvenDepositV2(const Consensus::BridgeV2EntryRole role,
                            const uint64_t deposit_id) const
    {
        return IsSuccessor(role)
                   ? deposits_v2_successor_base.contains(deposit_id) ||
                         deposits_v2_successor_overlay.contains(deposit_id)
                   : deposits_v2_base.contains(deposit_id) ||
                         deposits_v2_overlay.contains(deposit_id);
    }

    //! Record a deposit this block proved into entry `role`. False (state
    //! unchanged) for a non-positive amount, an id this block already
    //! recorded for the entry or a total that would leave the CAmount range.
    bool AddProvenDepositV2(const Consensus::BridgeV2EntryRole role,
                            const uint64_t deposit_id, const CAmount amount)
    {
        bridge::ProvenDepositMapV2& overlay{
            IsSuccessor(role) ? deposits_v2_successor_overlay
                              : deposits_v2_overlay};
        CAmount& overlay_total{IsSuccessor(role)
                                   ? deposits_v2_successor_overlay_total
                                   : deposits_v2_overlay_total};
        const auto total{CheckedAdd(overlay_total, amount)};
        if (amount <= 0 || !total ||
            !overlay.emplace(deposit_id, amount).second) {
            return false;
        }
        overlay_total = *total;
        return true;
    }

    //! P_pre and P_post at `cut` over entry `role`'s connected and this
    //! block's deposits, walking only the ids at or above the cut.
    std::optional<bridge::ProvenDepositSumsV2> ProvenDepositSums(
        const Consensus::BridgeV2EntryRole role, const uint64_t cut) const
    {
        return IsSuccessor(role)
                   ? bridge::SplitProvenDepositsAtCut(
                         deposits_v2_successor_base,
                         deposits_v2_successor_base_total,
                         deposits_v2_successor_overlay,
                         deposits_v2_successor_overlay_total, cut)
                   : bridge::SplitProvenDepositsAtCut(
                         deposits_v2_base, deposits_v2_base_total,
                         deposits_v2_overlay, deposits_v2_overlay_total, cut);
    }
};

//! The RESERVE_E and TOKEN_FLAGS subjects of vault entry `role` (node
//! changes sections 7.6 and 8.2): 2 and 3 for the launch entry, 0x12 and
//! 0x13 for the B-23 successor.
uint8_t ReserveSubjectFor(const Consensus::BridgeV2EntryRole role)
{
    return role == Consensus::BridgeV2EntryRole::SUCCESSOR
               ? bridge::ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR
               : bridge::ETH_OBSERVATION_SUBJECT_RESERVE;
}

uint8_t TokenFlagsSubjectFor(const Consensus::BridgeV2EntryRole role)
{
    return role == Consensus::BridgeV2EntryRole::SUCCESSOR
               ? bridge::ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS_SUCCESSOR
               : bridge::ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS;
}

//! The latest observation of `subject` in `observations`; a reference to an
//! empty slot for an unknown subject (callers pass known subjects only).
const std::optional<BridgeObservationRecordV2>& ObservationSlot(
    const BridgeObservationSlotsV2& observations, const uint8_t subject)
{
    static const std::optional<BridgeObservationRecordV2> none{};
    const auto slot{BridgeObservationSlotV2(subject)};
    return slot ? observations[*slot] : none;
}

/**
 * Retain `anchor` in the selected store, or accept it as already retained
 * when the store holds the same hash with the same number and receipts root.
 * On the V2 store (`v2_store`) a retained anchor must also carry the same
 * stateRoot, the root kind 9 observations read. The V1 store never compares
 * stateRoot, so V1 acceptance stays exactly as it was before the root was
 * recorded.
 */
bool AddAnchor(const BridgeExecutionAnchor& anchor, ScratchState& state,
               std::vector<BridgeExecutionAnchor>& added,
               const bool v2_store, std::string& error)
{
    if (anchor.block_hash.IsNull() || anchor.receipts_root.IsNull() ||
        anchor.connected_height < 0 || anchor.connected_block.IsNull() ||
        !HasValidBackfillOrigin(anchor)) {
        error = "bridge execution anchor is malformed";
        return false;
    }
    if (const auto* same_hash{state.Anchor(anchor.block_hash)}) {
        if (same_hash->block_number != anchor.block_number ||
            same_hash->receipts_root != anchor.receipts_root ||
            (v2_store && same_hash->state_root != anchor.state_root)) {
            error = "bridge execution anchor hash conflicts with retained state";
            return false;
        }
        return true;
    }
    if (const auto* same_height{
            state.AnchorHashAtHeight(anchor.block_number)};
        same_height && *same_height != anchor.block_hash) {
        error = "bridge execution anchor height conflicts with retained state";
        return false;
    }
    state.AddAnchor(anchor);
    added.push_back(anchor);
    return true;
}

/**
 * Kind 1 against the store the height selects: the V1 pins and vault
 * deployment block below V, the V2 entry's from V. The guard below is
 * therefore per store, so the V2 store can be bootstrapped once although the
 * V1 store already was (node changes section 3.4).
 */
bool VerifyBootstrap(const bridge::BridgeBootstrapV1& bootstrap,
                     const int height, const uint256& block_hash,
                     const Consensus::EthereumLightClientPins& pins,
                     const uint64_t origin_deployment_block,
                     ScratchState& state, BridgeBlockDelta& delta,
                     std::string& error)
{
    if (state.LightClient()) {
        error = "bridge light client is already bootstrapped";
        return false;
    }
    if (bootstrap.header.beacon.slot != pins.trusted_checkpoint_slot ||
        bootstrap.header.beacon.HashTreeRoot() != pins.trusted_checkpoint_root) {
        error = "bridge bootstrap does not match the pinned slot and root";
        return false;
    }
    if (!SlotUsesKnownFork(bootstrap.header.beacon.slot, pins)) {
        error = "bridge bootstrap is beyond the pinned Ethereum fork schedule";
        return false;
    }
    if (!ExecutionRangeReachable(
            bootstrap.header.execution.block_number,
            origin_deployment_block)) {
        error =
            "bridge bootstrap cannot reach the configured vault deployment block";
        return false;
    }
    bridge::LightClientStore store;
    const bridge::LcResult result{bridge::InitStore(
        store, LightClientConfigFor(pins), pins.trusted_checkpoint_root,
        bootstrap.header, bootstrap.current_committee,
        bootstrap.current_committee_branch)};
    if (result != bridge::LcResult::OK) {
        error = "bridge bootstrap light-client proof is invalid";
        return false;
    }
    state.SetLightClient(std::move(store));
    return AddAnchor(FinalizedAnchor(state.LightClient()->finalized_header,
                                     height, block_hash),
                     state, delta.anchors_added, delta.v2_store, error);
}

bool VerifyUpdate(const bridge::BridgeUpdateV1& record, const int height,
                  const uint256& block_hash,
                  const Consensus::EthereumLightClientPins& pins,
                  ScratchState& state, BridgeBlockDelta& delta,
                  std::string& error)
{
    const bridge::LightClientStore* current{state.LightClient()};
    if (current == nullptr) {
        error = "bridge light-client update precedes bootstrap";
        return false;
    }
    const bridge::LightClientUpdate& update{record.update};
    if (!UpdateUsesKnownFork(update, pins)) {
        error = "bridge light-client update uses an unpinned Ethereum fork";
        return false;
    }
    const bridge::LightClientHeader& old{current->finalized_header};
    if (update.finalized.beacon.slot == old.beacon.slot &&
        !SameLightClientHeader(update.finalized, old)) {
        error = "bridge light-client update conflicts at a finalized slot";
        return false;
    }
    bridge::LightClientStore next{*current};
    const bridge::LcResult result{
        bridge::ProcessUpdate(next, LightClientConfigFor(pins), update)};
    if (result != bridge::LcResult::OK) {
        error = "bridge light-client update is invalid";
        return false;
    }
    if (SameLightClientStore(next, *current)) {
        error = "bridge light-client update is a no-op";
        return false;
    }
    const bool finalized_changed{
        !SameLightClientHeader(next.finalized_header,
                               current->finalized_header)};
    if (finalized_changed &&
        !ExecutionRangeReachable(
            next.finalized_header.execution.block_number,
            current->finalized_header.execution.block_number)) {
        error =
            "bridge light-client update exceeds the execution backfill window";
        return false;
    }
    state.SetLightClient(std::move(next));
    if (!finalized_changed) return true;
    return AddAnchor(FinalizedAnchor(state.LightClient()->finalized_header,
                                     height, block_hash),
                     state, delta.anchors_added, delta.v2_store, error);
}

bool VerifyBackfill(const bridge::BridgeExecutionBackfillV1& proof,
                    const int height, const uint256& block_hash,
                    ScratchState& state, BridgeBlockDelta& delta,
                    std::string& error)
{
    const BridgeExecutionAnchor* source{
        state.Anchor(proof.finalized_anchor_hash)};
    if (source == nullptr) {
        error = "bridge execution backfill names an unretained anchor";
        return false;
    }
    const auto target{bridge::VerifyExecAncestry(
        source->block_hash, proof.target_block_number,
        proof.ancestry_headers)};
    if (!target) {
        error = "bridge execution backfill ancestry is invalid";
        return false;
    }
    if (!CheckCumulativeBackfillTarget(*source, target->block_number,
                                       error)) {
        return false;
    }
    const auto timestamp{
        ExecutionHeaderTimestamp(proof.ancestry_headers.back())};
    if (!timestamp) {
        error = "bridge execution backfill target timestamp is invalid";
        return false;
    }
    const BridgeExecutionAnchor anchor{
        target->block_number, target->block_hash, target->receipts_root,
        source->source_finalized_beacon_slot,
        source->source_finalized_execution_block, *timestamp, height,
        block_hash, target->state_root};
    const size_t before{delta.anchors_added.size()};
    if (!AddAnchor(anchor, state, delta.anchors_added, delta.v2_store,
                   error)) {
        return false;
    }
    if (delta.anchors_added.size() == before) {
        error = "bridge execution backfill is a no-op";
        return false;
    }
    return true;
}

/**
 * The receipt-proof half of a kind 3 MINT, shared by V1 and V2: the target
 * block is at or after the vault deployment, the light-client head is inside
 * the pinned fork schedule and fresh, the named anchor is retained in the
 * selected store, the execution ancestry and cumulative backfill bound hold,
 * the receipt is in the proven root, decodes with EIP-2718 types up to
 * `max_receipt_type`, and names one exact Deposit log of `vault`.
 */
std::optional<bridge::DepositEvent> ProveMintDeposit(
    const bridge::BridgeMintV1& proof,
    const Consensus::EthereumLightClientPins& pins,
    const uint64_t origin_deployment_block,
    const bridge::EthAddress& vault, const uint8_t max_receipt_type,
    const int64_t candidate_time, const ScratchState& state,
    std::string& error)
{
    if (proof.target_block_number < origin_deployment_block) {
        error =
            "bridge mint target precedes the configured origin deployment block";
        return std::nullopt;
    }
    const bridge::LightClientStore* light_client{state.LightClient()};
    if (light_client == nullptr) {
        error = "bridge mint precedes light-client bootstrap";
        return std::nullopt;
    }
    if (!SlotUsesKnownFork(
            light_client->finalized_header.beacon.slot, pins)) {
        error = "bridge mint uses a light-client head beyond the pinned fork schedule";
        return std::nullopt;
    }
    if (!FinalizedHeadFresh(*light_client, candidate_time, pins)) {
        error = "bridge mint light-client head is stale";
        return std::nullopt;
    }
    const BridgeExecutionAnchor* anchor{
        state.Anchor(proof.finalized_anchor_hash)};
    if (anchor == nullptr) {
        error = "bridge mint names an unretained execution anchor";
        return std::nullopt;
    }
    const auto target{bridge::VerifyExecAncestry(
        anchor->block_hash, proof.target_block_number,
        proof.ancestry_headers)};
    if (!target) {
        error = "bridge mint execution ancestry is invalid";
        return std::nullopt;
    }
    if (!CheckCumulativeBackfillTarget(*anchor, target->block_number,
                                       error)) {
        return std::nullopt;
    }
    const std::vector<unsigned char> trie_key{
        bridge::RlpEncodeUint64(proof.tx_index)};
    const auto receipt_value{bridge::VerifyMptProof(
        target->receipts_root, trie_key, proof.mpt_nodes)};
    if (!receipt_value) {
        error = "bridge mint receipt is not included in the proven root";
        return std::nullopt;
    }
    const auto receipt{bridge::DecodeReceipt(*receipt_value, max_receipt_type)};
    if (!receipt) {
        error = "bridge mint receipt is malformed";
        return std::nullopt;
    }
    const auto event{bridge::ExtractDepositAt(
        *receipt, vault, proof.receipt_log_index)};
    if (!event) {
        error = "bridge mint does not name one exact configured-vault deposit log";
        return std::nullopt;
    }
    return event;
}

/**
 * The state half of a kind 3 MINT, shared by V1 and V2 once admission has
 * produced `authorization`: the nullifier is unused, the named output pays
 * exactly the authorized OWNER, the ptxid is unique in the block, and the
 * nullifier, per-block total and epoch counter are recorded.
 */
bool ConnectMintAuthorization(
    const CTransaction& tx, const uint32_t output_index,
    const bridge::BridgeMintAuthorization& authorization,
    const std::pair<modern::AssetId, uint64_t>& epoch_key,
    const CAmount epoch_used, ScratchState& state, BridgeBlockDelta& delta,
    BridgeTxAuthorization& tx_result, std::string& error)
{
    if (state.IsNullified(authorization.nullifier)) {
        error = "bridge deposit is already nullified";
        return false;
    }
    if (output_index >= tx.vout.size()) {
        error = "bridge mint output index is out of range";
        return false;
    }
    const auto expected{modern::MakeAssetOwnerOutput(
        authorization.asset, authorization.amount,
        authorization.recipient_script)};
    if (!expected || tx.vout[output_index] != *expected) {
        error = "bridge mint is not paid to the exact authorized OWNER output";
        return false;
    }

    const BridgeTxMintAuthorization connected{
        tx.GetPtxid(), output_index, authorization};
    if (std::any_of(delta.mint_authorizations.begin(),
                    delta.mint_authorizations.end(),
                    [&](const auto& prior) {
                        return prior.transaction_id == connected.transaction_id;
                    })) {
        error = "bridge block duplicates a mint ptxid";
        return false;
    }
    if (!state.AddNullifier(authorization.nullifier)) {
        error = "bridge deposit is already nullified";
        return false;
    }
    state.minted_this_block += authorization.amount;
    state.SetEpochMinted(epoch_key, epoch_used + authorization.amount);
    delta.nullifiers_added.push_back(authorization.nullifier);
    delta.mint_authorizations.push_back(connected);
    tx_result.mint = connected;
    return true;
}

bool VerifyMint(const CTransaction& tx, const bridge::BridgeMintV1& proof,
                const int height, const int64_t candidate_time,
                const uint256& block_hash, const Consensus::Params& params,
                ScratchState& state, BridgeBlockDelta& delta,
                BridgeTxAuthorization& tx_result, std::string& error)
{
    const Consensus::BridgeAssetParams& configured{*params.busd_bridge};
    const auto registry_id{modern::ConfiguredBridgeRegistryId(params)};
    if (!registry_id || proof.registry_id != *registry_id) {
        error = "bridge mint registry id does not match the configured registry";
        return false;
    }
    const auto event{ProveMintDeposit(
        proof, *configured.light_client, *configured.origin_deployment_block,
        configured.asset.vault_address, bridge::RECEIPT_MAX_TYPE_V1,
        candidate_time, state, error)};
    if (!event) return false;

    const uint64_t epoch{BridgeEpoch(height, configured)};
    const auto asset{modern::ConfiguredBridgeAssetId(params)};
    if (!asset) {
        error = "bridge mint asset identity is unavailable";
        return false;
    }
    const auto epoch_key{std::make_pair(*asset, epoch)};
    const CAmount epoch_used{state.EpochMinted(epoch_key)};
    bridge::BridgeMintAuthorization authorization;
    const bridge::BridgeAdmissionResult admitted{
        bridge::AdmitConfiguredDeposit(
            params,
            bridge::ProvenBridgeDeposit{configured.asset.origin_chain_id,
                                        proof.target_block_number,
                                        configured.asset.vault_address, *event},
            height,
            bridge::BridgeMintBudget{state.minted_this_block, epoch_used},
            authorization)};
    if (admitted != bridge::BridgeAdmissionResult::OK) {
        error = "bridge mint deposit fails registry, recipient, amount, or cap admission";
        return false;
    }
    return ConnectMintAuthorization(tx, proof.output_index, authorization,
                                    epoch_key, epoch_used, state, delta,
                                    tx_result, error);
}

/**
 * Kind 3 from V (node changes section 4): the record names the V2 vault
 * entry id, the receipt is proven against the V2 store with the V2 receipt
 * limits (decoded by the caller) and EIP-2718 types 1..0x7f, and admission is
 * the V2 entry's, minting bUSD V2 under nullifier namespace 2. The epoch is
 * (height - V) / epoch length, keyed by the bUSD V2 AssetId; the per-block
 * total stays shared (V1 mints are invalid from V).
 *
 * The mint also enters the reserve ledger (node changes section 7.1):
 * minted += amount, and the deposit id -> amount pair joins the proven
 * deposits that invariant R and the wind-down bound sum.
 *
 * With a B-23 successor (section 7.6) the record names either entry
 * (modern::ResolveBridgeV2Entry): the launch entry v at every height from V
 * (a retired v still takes deposits it cannot refund, which B3 mints and
 * sweeps to v'), the successor v' only from h_m. The receipt must show a
 * Deposit log of the named entry's vault at or after its deployment block,
 * admission is that entry's, and the mint enters that entry's ledger and
 * proven deposits. Both mint the one bUSD V2 AssetId, and the epoch
 * counter, keyed by it, is shared (the caps are equal, SuccessorValid).
 */
bool VerifyMintV2(const CTransaction& tx, const bridge::BridgeMintV1& proof,
                  const int height, const int64_t candidate_time,
                  const Consensus::Params& params, ScratchState& state,
                  BridgeBlockDelta& delta, BridgeTxAuthorization& tx_result,
                  std::string& error)
{
    const Consensus::BridgeV2Params& v2{*params.bridge_v2};
    const auto role{
        modern::ResolveBridgeV2Entry(params, proof.registry_id, height)};
    const Consensus::BridgeV2VaultEntry* const entry{
        role ? v2.Entry(*role) : nullptr};
    if (!role || entry == nullptr) {
        error = "bridge mint registry id does not match the V2 vault entry";
        return false;
    }
    const auto event{ProveMintDeposit(
        proof, v2.light_client, entry->origin_deployment_block,
        entry->vault_address, bridge::RECEIPT_MAX_TYPE_V2, candidate_time,
        state, error)};
    if (!event) return false;

    const auto asset{modern::ConfiguredBridgeV2AssetId(params)};
    if (!asset || height < v2.activation_height) {
        error = "bridge mint asset identity is unavailable";
        return false;
    }
    const uint64_t epoch{
        static_cast<uint64_t>(height - v2.activation_height) /
        v2.vault.mint_caps.epoch_length_blocks};
    const auto epoch_key{std::make_pair(*asset, epoch)};
    const CAmount epoch_used{state.EpochMinted(epoch_key)};
    bridge::BridgeMintAuthorization authorization;
    const bridge::BridgeAdmissionResult admitted{bridge::AdmitDepositV2(
        v2, *entry, *asset,
        bridge::ProvenBridgeDeposit{entry->origin_chain_id,
                                    proof.target_block_number,
                                    entry->vault_address, *event},
        height, bridge::BridgeMintBudget{state.minted_this_block, epoch_used},
        authorization)};
    if (admitted != bridge::BridgeAdmissionResult::OK) {
        error = "bridge mint deposit fails registry, recipient, amount, or cap admission";
        return false;
    }
    if (!ConnectMintAuthorization(tx, proof.output_index, authorization,
                                  epoch_key, epoch_used, state, delta,
                                  tx_result, error)) {
        return false;
    }
    // The nullifier set already forbids a second mint of this deposit and
    // each entry's proven-deposit index is keyed the same way (the
    // nullifier names the entry's vault), so both refusals below are
    // defensive. A failure fails the whole block.
    if (state.HasProvenDepositV2(*role, event->deposit_id)) {
        error = "bridge V2 deposit id is already proven";
        return false;
    }
    bridge::ReserveLedgerV2 ledger{state.Ledger(*role)};
    if (!bridge::RecordMintV2(ledger, authorization.amount)) {
        error = "bridge V2 reserve ledger cannot record the mint";
        return false;
    }
    if (!state.AddProvenDepositV2(*role, event->deposit_id,
                                  authorization.amount)) {
        error = "bridge V2 proven deposits cannot record the mint";
        return false;
    }
    state.Ledger(*role) = ledger;
    (*role == Consensus::BridgeV2EntryRole::SUCCESSOR
         ? delta.deposits_v2_successor_added
         : delta.deposits_v2_added)
        .push_back(
            BridgeProvenDepositV2{event->deposit_id, authorization.amount});
    delta.reserve_v2_after = state.reserve_v2;
    delta.reserve_v2_successor_after = state.reserve_v2_successor;
    return true;
}

bool VerifyWithdrawal(
    const CTransaction& tx,
    const bridge::BridgeManagedWithdrawalV1& proof, const int height,
    const uint256& block_hash, const Consensus::Params& params,
    ScratchState& state, BridgeBlockDelta& delta,
    BridgeTxAuthorization& tx_result, std::string& error)
{
    if (!Consensus::BridgeWithdrawalRulesActive(height, params)) {
        error = "bridge withdrawal rules are not active";
        return false;
    }
    const Consensus::BridgeAssetParams& configured{*params.busd_bridge};
    if (*configured.withdrawal_mode !=
        Consensus::BridgeWithdrawalMode::MANAGED_V1) {
        error = "managed bridge withdrawal proof is disabled";
        return false;
    }
    const auto registry_id{modern::ConfiguredBridgeRegistryId(params)};
    const auto asset{modern::ConfiguredBridgeAssetId(params)};
    if (!registry_id || !asset || proof.registry_id != *registry_id) {
        error = "managed bridge withdrawal registry id does not match";
        return false;
    }
    if (bridge::EthAddressIsNull(proof.ethereum_recipient)) {
        error = "managed bridge withdrawal Ethereum recipient is null";
        return false;
    }
    if (proof.raw_amount == 0 ||
        proof.raw_amount > static_cast<uint64_t>(MAX_MONEY)) {
        error = "managed bridge withdrawal amount is invalid";
        return false;
    }
    const CAmount amount{static_cast<CAmount>(proof.raw_amount)};
    if (proof.burn_output_index >= tx.vout.size()) {
        error = "managed bridge withdrawal burn output index is out of range";
        return false;
    }
    const auto expected{modern::MakeAssetBurnOutput(*asset, amount)};
    if (!expected || tx.vout[proof.burn_output_index] != *expected) {
        error = "managed bridge withdrawal does not name the exact bUSD BURN output";
        return false;
    }
    const BridgeWithdrawalId id{tx.GetHash(), proof.burn_output_index};
    if (!state.AddWithdrawal(id)) {
        error = "managed bridge withdrawal request is duplicated";
        return false;
    }
    BridgeManagedWithdrawalRequest request{
        tx.GetHash(), proof.burn_output_index, *asset, amount,
        proof.ethereum_recipient, height, block_hash};
    delta.withdrawals_added.push_back(request);
    tx_result.withdrawal = std::move(request);
    return true;
}

bool VerifyBridgeBurn(
    const CTransaction& tx, const bridge::BridgeBurnV1& record,
    const int height, const uint256& block_hash,
    const Consensus::Params& params, ScratchState& state,
    BridgeBlockDelta& delta, BridgeTxAuthorization& tx_result,
    std::string& error)
{
    if (!Consensus::BridgeWithdrawalRulesActive(height, params)) {
        error = "bridge withdrawal rules are not active";
        return false;
    }
    const Consensus::BridgeAssetParams& configured{*params.busd_bridge};
    if (*configured.withdrawal_mode !=
        Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1) {
        error = "decentralized bridge burn is disabled";
        return false;
    }
    const auto registry_id{modern::ConfiguredBridgeRegistryId(params)};
    const auto asset{modern::ConfiguredBridgeAssetId(params)};
    if (!registry_id || !asset || record.registry_id != *registry_id) {
        error = "decentralized bridge burn registry id does not match";
        return false;
    }
    if (bridge::EthAddressIsNull(record.ethereum_recipient)) {
        error = "decentralized bridge burn Ethereum recipient is null";
        return false;
    }
    if (record.raw_amount == 0 ||
        record.raw_amount > static_cast<uint64_t>(MAX_MONEY)) {
        error = "decentralized bridge burn amount is invalid";
        return false;
    }
    const CAmount amount{static_cast<CAmount>(record.raw_amount)};
    if (record.burn_output_index >= tx.vout.size()) {
        error = "decentralized bridge burn output index is out of range";
        return false;
    }
    const auto expected{modern::MakeAssetBurnOutput(*asset, amount)};
    if (!expected || tx.vout[record.burn_output_index] != *expected) {
        error = "decentralized bridge record does not name the exact bUSD BURN output";
        return false;
    }
    if (height < 0) {
        error = "decentralized bridge burn height is invalid";
        return false;
    }

    modern::BridgeWithdrawalV1 withdrawal;
    withdrawal.withdrawal_id = state.withdrawal_tree.count;
    withdrawal.origin_chain_id = configured.asset.origin_chain_id;
    withdrawal.asset_id = *asset;
    withdrawal.origin_token = configured.asset.token_address;
    withdrawal.recipient = record.ethereum_recipient;
    withdrawal.amount = amount;
    withdrawal.b3_height = static_cast<uint64_t>(height);

    uint256 leaf;
    const BridgeWithdrawalId source{tx.GetHash(), record.burn_output_index};
    if (!state.AddDecentralizedWithdrawal(source, withdrawal, leaf)) {
        error = "decentralized bridge withdrawal id or burn source is duplicated";
        return false;
    }
    delta.withdrawal_tree_after = state.withdrawal_tree;
    BridgeDecentralizedWithdrawalRequest request{
        tx.GetHash(), record.burn_output_index, withdrawal, leaf, height,
        block_hash};
    delta.decentralized_withdrawals_added.push_back(request);
    tx_result.decentralized_withdrawal = std::move(request);
    return true;
}

template <typename State>
std::optional<bridge::BurnGateObservationV2<State>> GateObservation(
    const BridgeObservationSlotsV2& observations, const uint8_t subject)
{
    const auto& record{ObservationSlot(observations, subject)};
    if (!record || !std::holds_alternative<State>(record->state)) {
        return std::nullopt;
    }
    return bridge::BurnGateObservationV2<State>{
        record->t_obs, std::get<State>(record->state), record->verdict};
}

/**
 * The burn gate rows (bridge::EvaluateBurnGateV2) of vault entry `role` on a
 * full lineage context and the given kind 9 observations and sticky
 * conflict, at now_eff = max(candidate_time, `light_client`'s finalized
 * execution timestamp). EvaluateBridgeV2BurnGate passes the connected
 * index's state; VerifyBridgeBurnV2 passes the block's state after its
 * earlier records and the connected light client, so both read the same
 * now_eff. Never UNAVAILABLE: the callers check availability.
 *
 * The entry's rows read VERIFIER_E (the verifier is shared) and the
 * entry's own TOKEN_FLAGS and RESERVE_E (subjects 3 and 2 for the launch
 * entry, 0x13 and 0x12 for the B-23 successor). Retirement is no row: the
 * burn gate passes the entry that takes burns at the height, the kind 16
 * sweep gate the retired launch entry (its gate "but for retirement",
 * section 7.6), and a burn naming the retired entry is refused by its
 * record check before any gate.
 */
BridgeV2GateResult EvaluateBurnGateOn(
    const BridgeV2LineageContext& lineage,
    const BridgeObservationSlotsV2& observations,
    const std::optional<BridgeV2Conflict>& conflict,
    const std::optional<bridge::LightClientStore>& light_client,
    const Consensus::Params& params, const int height,
    const int64_t candidate_time, const Consensus::BridgeV2EntryRole role)
{
    BridgeV2GateResult out;
    std::optional<uint64_t> light_client_time;
    if (light_client) {
        light_client_time = light_client->finalized_header.execution.timestamp;
    }
    out.now_eff = bridge::BurnGateNowEffV2(candidate_time, light_client_time);
    out.readiness = BridgeBurnReadiness::NOT_READY;

    const Consensus::BridgeV2Params& v2{*params.bridge_v2};
    if (!v2.Valid()) {
        out.row = bridge::BurnGateRowV2::C1;
        out.reason = "bridge V2 parameters are incomplete";
        return out;
    }
    bridge::BurnGateInputsV2 in;
    in.height = height;
    in.burn_height = v2.burn_height;
    in.now_eff = out.now_eff;
    const LineageV2View view{MakeLineageV2View(params, lineage.fresh)};
    in.lineage.configured = view.configured;
    in.lineage.bootstrapped = view.bootstrapped;
    in.lineage.broken = view.broken;
    in.lineage.epoch = view.epoch;
    in.lineage.current_count = view.current_count;
    in.lineage.current_weight = view.current_weight;
    in.lineage.next_count = view.next_count;
    in.lineage.next_weight = view.next_weight;
    in.lineage.finalized_height = view.finalized_height;
    if (lineage.fresh && lineage.fresh->finalized) {
        in.lineage.finalized_epoch = lineage.fresh->finalized->epoch;
    }
    in.bounds = bridge::BurnGateBoundsFromPins(v2.verifier);
    in.verifier = GateObservation<bridge::VerifierStateV2>(
        observations, bridge::ETH_OBSERVATION_SUBJECT_VERIFIER);
    in.token = GateObservation<bridge::TokenFlagsV2>(
        observations, TokenFlagsSubjectFor(role));
    in.reserve = GateObservation<bridge::ReserveStateV2>(
        observations, ReserveSubjectFor(role));
    if (conflict) in.conflict_reason = conflict->reason;
    const bridge::BurnGateDecisionV2 decision{
        bridge::EvaluateBurnGateV2(in, v2.gate)};
    out.readiness = decision.ready ? BridgeBurnReadiness::READY
                                   : BridgeBurnReadiness::NOT_READY;
    out.row = decision.row;
    out.reason = decision.reason;
    out.closes_at = decision.closes_at;
    return out;
}

/**
 * Kind 6 from V (node changes section 7.2): valid only from W2, naming the
 * V2 vault entry id, a non-null Ethereum recipient and the exact bUSD V2
 * BURN output (check 1); the burn gate READY at this record's position
 * (check 4, below); at most the entry's reserve after the block's earlier
 * burns (check 2); within the wind-down bound of the latest RESERVE_E
 * observation, which must exist, earlier burns of the block included
 * (check 3). The leaf V2 takes the next id of the V2 tree, the V2 entry's
 * chain, vault and token, and this height, and burned_v grows by the amount.
 *
 * Check 4 per record: with a full lineage context the gate is evaluated on
 * that context, the block-level now_eff (the connected light client) and
 * the observations and sticky conflict after the block's earlier records.
 * So a RESERVE_E failing invariant R, a CERT_CONFLICT VERIFIER_E, an
 * impaired verifier or a paused, deprecated or blacklisting token earlier in
 * the same block refuses every later burn. The caller also evaluates the
 * gate once per block on the connected state (EvaluateBridgeV2BurnGate),
 * which this equals while no earlier record changed an observation.
 * REPLAY_LINEAGE skips it (judged when the block first connected). Without
 * a context the caller's evaluation stands unless an earlier record changed
 * an observation or the conflict, and then the burn fails closed.
 *
 * With a B-23 successor (section 7.6) a burn names the entry that takes
 * burns at its height: v below h_m, v' from h_m. A burn naming the retired v
 * from h_m is refused before the gate, deterministically (so under
 * REPLAY_LINEAGE and without a context too). Checks 2-4 then read that
 * entry: its ledger, its RESERVE_E (subject 2 or 0x12) and proven deposits,
 * and its gate rows; the leaf names its vault and token, and its ledger
 * counts the amount in burned_v.
 *
 * A burn with a route (d3) exit block (check 5, section 7.2 item 5) also
 * needs the pinned exit forwarder, parameters valid against it
 * (modern::ExitForwarderParamsValidV2), a refund EOA that holds no bridge V2
 * role (modern::BridgeV2ReservedEthAddressReason, OI-6: none of those
 * contracts can call withdrawOnEthereum, so F_x would lose its escape) and a
 * recipient equal to their F_x (modern::ExitForwarderAddressV2). The leaf is
 * unchanged: it names F_x as its recipient like any other burn. The
 * parameters are stored with the leaf (BridgeWithdrawalV2Request::exit).
 *
 * The exit forwarder is one height-less pin, valid only while it never
 * changes; on regtest the pin file is frozen from A3. Spec section 8's
 * coordinated re-pin (an adapter, OFT or endpoint change) must first make it
 * an append-only, height-versioned list checked against the pin active at
 * each burn's height, or a replay would refuse every earlier route (d3)
 * burn.
 */
bool VerifyBridgeBurnV2(
    const CTransaction& tx, const bridge::BridgeBurnV1& record,
    const int height, const int64_t candidate_time, const uint256& block_hash,
    const Consensus::Params& params, const BridgeV2LineageContext* lineage,
    ScratchState& state, BridgeBlockDelta& delta,
    BridgeTxAuthorization& tx_result, std::string& error)
{
    if (!params.BridgeV2BurnRulesActive(height)) {
        error = "bridge V2 burns are not active";
        return false;
    }
    const auto asset{modern::ConfiguredBridgeV2AssetId(params)};
    const auto role{
        modern::ResolveBridgeV2Entry(params, record.registry_id, height)};
    const Consensus::BridgeV2VaultEntry* const resolved{
        role ? params.bridge_v2->Entry(*role) : nullptr};
    if (!asset || !role || resolved == nullptr) {
        error = "bridge burn does not name the V2 vault entry";
        return false;
    }
    if (*role == Consensus::BridgeV2EntryRole::LAUNCH &&
        params.BridgeV2MigrationActive(height)) {
        error = "bridge V2 burn names the retired vault entry";
        return false;
    }
    const Consensus::BridgeV2VaultEntry& entry{*resolved};
    if (bridge::EthAddressIsNull(record.ethereum_recipient)) {
        error = "bridge V2 burn Ethereum recipient is null";
        return false;
    }
    // Check 5, the optional route (d3) exit block (section 7.2 item 5): the
    // recipient is the exit forwarder F_x of the block's parameters under
    // the pinned exit factory. Deterministic and before the gate, so it
    // holds under REPLAY_LINEAGE and without a context too.
    if (record.exit) {
        const auto& exit_pins{params.bridge_v2->exit_forwarder};
        if (!exit_pins) {
            error = "bridge V2 burn exit block needs a pinned exit forwarder";
            return false;
        }
        if (!modern::ExitForwarderParamsValidV2(*record.exit,
                                                exit_pins->dst_eid)) {
            error = "bridge V2 burn exit parameters are invalid";
            return false;
        }
        if (modern::BridgeV2ReservedEthAddressReason(*params.bridge_v2,
                                                     record.exit->refund_eoa)) {
            error = "bridge V2 burn exit refund EOA holds a bridge V2 role";
            return false;
        }
        if (modern::ExitForwarderAddressV2(*exit_pins, *record.exit) !=
            record.ethereum_recipient) {
            error = "bridge V2 burn recipient is not the exit forwarder of its exit parameters";
            return false;
        }
    }
    if (record.raw_amount == 0 ||
        record.raw_amount > static_cast<uint64_t>(MAX_MONEY)) {
        error = "bridge V2 burn amount is invalid";
        return false;
    }
    const CAmount amount{static_cast<CAmount>(record.raw_amount)};
    if (record.burn_output_index >= tx.vout.size()) {
        error = "bridge V2 burn output index is out of range";
        return false;
    }
    const auto expected{modern::MakeAssetBurnOutput(*asset, amount)};
    if (!expected || tx.vout[record.burn_output_index] != *expected) {
        error = "bridge V2 burn record does not name the exact bUSD V2 BURN output";
        return false;
    }
    // Check 4 at this record's position.
    if (lineage == nullptr) {
        if (state.gate_inputs_changed) {
            error = "bridge V2 burn after a kind 9 observation of the same block needs the fresh lineage context";
            return false;
        }
    } else if (!lineage->trust_recorded_verdicts) {
        const BridgeV2GateResult gate{EvaluateBurnGateOn(
            *lineage, state.observations_v2, state.conflict_v2,
            state.light_client_base, params, height, candidate_time, *role)};
        if (gate.readiness != BridgeBurnReadiness::READY) {
            error = "bridge V2 burn gate is not ready at the burn: " +
                    gate.Describe();
            return false;
        }
    }
    // Check 2: the entry ledger's burned already holds the block's earlier
    // burns.
    bridge::ReserveLedgerV2 ledger{state.Ledger(*role)};
    if (!bridge::RecordBurnV2(ledger, amount)) {
        error = "bridge V2 burn exceeds the vault entry's reserve";
        return false;
    }
    // Check 3, against the ledger before this burn.
    const auto& reserve_observation{
        ObservationSlot(state.observations_v2, ReserveSubjectFor(*role))};
    if (!reserve_observation ||
        !std::holds_alternative<bridge::ReserveStateV2>(
            reserve_observation->state)) {
        error = "bridge V2 burn has no reserve observation";
        return false;
    }
    const auto& reserve{
        std::get<bridge::ReserveStateV2>(reserve_observation->state)};
    const auto sums{state.ProvenDepositSums(*role, reserve.next_deposit_id)};
    if (!sums) {
        error = "bridge V2 proven deposits cannot be summed";
        return false;
    }
    if (!bridge::WindDownAllows(amount, reserve, *sums, state.Ledger(*role),
                                params.bridge_v2->gate.wind_down_margin_ppm)) {
        error = "bridge V2 burn exceeds the wind-down bound of the latest reserve observation";
        return false;
    }

    modern::BridgeWithdrawalV2 withdrawal;
    withdrawal.withdrawal_id = state.withdrawal_tree_v2.count;
    withdrawal.destination_chain_id = entry.origin_chain_id;
    withdrawal.asset_id = *asset;
    withdrawal.vault = entry.vault_address;
    withdrawal.token = entry.token_address;
    withdrawal.recipient = record.ethereum_recipient;
    withdrawal.amount = amount;
    withdrawal.b3_height = static_cast<uint64_t>(height);

    uint256 leaf;
    const BridgeWithdrawalId source{tx.GetHash(), record.burn_output_index};
    if (!state.AddWithdrawalV2(source, withdrawal, leaf)) {
        error = "bridge V2 withdrawal id or burn source is duplicated";
        return false;
    }
    state.Ledger(*role) = ledger;
    delta.reserve_v2_after = state.reserve_v2;
    delta.reserve_v2_successor_after = state.reserve_v2_successor;
    delta.withdrawal_tree_v2_after = state.withdrawal_tree_v2;
    BridgeWithdrawalV2Request request{
        tx.GetHash(), record.burn_output_index, withdrawal, leaf, height,
        block_hash, static_cast<uint8_t>(*role), /*sweep=*/false, record.exit};
    delta.withdrawals_v2_added.push_back(request);
    tx_result.withdrawal_v2 = std::move(request);
    return true;
}

/**
 * Kind 16 VAULT_SWEEP (node changes section 7.6, B-23): from the migration
 * height h_m, append the leaf that moves the retired launch vault v's
 * reserve to the successor v'. The first sweep is the migration leaf;
 * later ones move what remains, the deposits minted into v since and any
 * part an earlier sweep's wind-down bound held back, because a retired v
 * takes no burns and no credit. Checks, in order:
 *  1. the V2 rules are active (the V2 store);
 *  2. the migration is active (BridgeV2MigrationActive);
 *  3. the record names the launch entry, the one that is retired;
 *  4. v's gate "but for retirement" is READY at this record's position
 *     (EvaluateBurnGateOn for the launch entry, on the block's state after
 *     its earlier records). It needs a full context; REPLAY_LINEAGE skips
 *     it (judged when the block first connected), and without a context
 *     the record is refused;
 *  5. reserve_v after the block's earlier records is positive;
 *  6. a launch RESERVE_E exists, and the amount is reserve_v capped by its
 *     wind-down headroom (bridge::VaultSweepAmountV2, check 3 of section
 *     7.2), which must be positive. After a partial seizure the sweep thus
 *     moves what v can still pay and strands only the shortfall.
 * The leaf (MakeVaultSweepWithdrawalV2) names v's chain, vault and token,
 * pays v''s vault and takes the next V2 id and this height; its source is
 * (txid, `binding_vout`), the carrier's binding output. v's burned_v counts
 * the amount and v''s transit_v' receives it, credited to C_v' only by
 * RESERVE_E_SUCCESSOR observations showing absorbedTotal.
 */
bool VerifyVaultSweepV2(
    const CTransaction& tx, const bridge::BridgeVaultSweepV2& record,
    const uint32_t binding_vout, const int height,
    const int64_t candidate_time, const uint256& block_hash,
    const Consensus::Params& params, const BridgeV2LineageContext* lineage,
    ScratchState& state, BridgeBlockDelta& delta,
    BridgeTxAuthorization& tx_result, std::string& error)
{
    using Consensus::BridgeV2EntryRole;
    if (!params.BridgeV2RulesActive(height) || !delta.v2_store) {
        error = "bridge V2 vault sweep records are not active";
        return false;
    }
    if (!params.BridgeV2MigrationActive(height)) {
        error = "bridge V2 vault sweeps are not active";
        return false;
    }
    const Consensus::BridgeV2Params& v2{*params.bridge_v2};
    const auto launch_id{
        modern::ConfiguredBridgeV2EntryId(params, BridgeV2EntryRole::LAUNCH)};
    const auto asset{modern::ConfiguredBridgeV2AssetId(params)};
    if (!launch_id || !asset || !v2.successor ||
        record.registry_id != *launch_id) {
        error = "bridge V2 vault sweep does not name the retired vault entry";
        return false;
    }
    if (lineage == nullptr) {
        error = "bridge V2 vault sweep cannot be judged here";
        return false;
    }
    if (!lineage->trust_recorded_verdicts) {
        const BridgeV2GateResult gate{EvaluateBurnGateOn(
            *lineage, state.observations_v2, state.conflict_v2,
            state.light_client_base, params, height, candidate_time,
            BridgeV2EntryRole::LAUNCH)};
        if (gate.readiness != BridgeBurnReadiness::READY) {
            error = "bridge V2 vault sweep gate is not ready: " +
                    gate.Describe();
            return false;
        }
    }
    const auto reserve_v{state.reserve_v2.Reserve()};
    if (!reserve_v || *reserve_v <= 0) {
        error = "bridge V2 vault sweep has nothing to sweep";
        return false;
    }
    const auto& reserve_observation{ObservationSlot(
        state.observations_v2, bridge::ETH_OBSERVATION_SUBJECT_RESERVE)};
    const auto* reserve{
        reserve_observation ? std::get_if<bridge::ReserveStateV2>(
                                  &reserve_observation->state)
                            : nullptr};
    if (reserve == nullptr) {
        error = "bridge V2 vault sweep has no reserve observation";
        return false;
    }
    const auto sums{state.ProvenDepositSums(BridgeV2EntryRole::LAUNCH,
                                            reserve->next_deposit_id)};
    if (!sums) {
        error = "bridge V2 proven deposits cannot be summed";
        return false;
    }
    const auto amount{bridge::VaultSweepAmountV2(
        state.reserve_v2, *reserve, *sums, v2.gate.wind_down_margin_ppm)};
    if (!amount) {
        error = "bridge V2 vault sweep has no wind-down headroom at the latest reserve observation";
        return false;
    }
    bridge::ReserveLedgerV2 launch{state.reserve_v2};
    bridge::ReserveLedgerV2 successor{state.reserve_v2_successor};
    if (!bridge::RecordSweepV2(launch, successor, *amount)) {
        error = "bridge V2 reserve ledgers cannot record the vault sweep";
        return false;
    }
    if (height < 0 || binding_vout >= tx.vout.size()) {
        error = "bridge V2 vault sweep binding output is invalid";
        return false;
    }
    const modern::BridgeWithdrawalV2 withdrawal{
        modern::MakeVaultSweepWithdrawalV2(
            v2.vault, *asset, v2.successor->entry.vault_address,
            state.withdrawal_tree_v2.count, *amount,
            static_cast<uint64_t>(height))};
    uint256 leaf;
    const BridgeWithdrawalId source{tx.GetHash(), binding_vout};
    if (!state.AddWithdrawalV2(source, withdrawal, leaf)) {
        error = "bridge V2 withdrawal id or burn source is duplicated";
        return false;
    }
    state.reserve_v2 = launch;
    state.reserve_v2_successor = successor;
    delta.reserve_v2_after = state.reserve_v2;
    delta.reserve_v2_successor_after = state.reserve_v2_successor;
    delta.withdrawal_tree_v2_after = state.withdrawal_tree_v2;
    BridgeWithdrawalV2Request request{
        tx.GetHash(), binding_vout, withdrawal, leaf, height, block_hash,
        static_cast<uint8_t>(BridgeV2EntryRole::LAUNCH), /*sweep=*/true};
    delta.withdrawals_v2_added.push_back(request);
    tx_result.withdrawal_v2 = std::move(request);
    return true;
}

/**
 * Kind 9 ETH_STATE_OBSERVATION from V (node changes section 8.2):
 * - the V2 light client is bootstrapped, inside the pinned fork schedule
 *   and fresh at the candidate time (the mint rules);
 * - the named anchor is a V2 anchor connected by an earlier block (never one
 *   added by this block) and carries a stateRoot;
 * - its execution block is strictly above the subject's latest observation,
 *   earlier records of this block included;
 * - the EIP-1186 proofs verify against that stateRoot with the pinned code
 *   hashes and decode strictly (bridge::VerifyEthStateObservationV2).
 * Per subject:
 * - VERIFIER_E: a verifier that is not LIVE needs verdict 0. A LIVE one
 *   needs a lineage context; unless it trusts recorded verdicts (replay) the
 *   verdict must equal VerifierVerdictV2. A CERT_CONFLICT verdict sets the
 *   sticky E5 conflict when none is set.
 * - RESERVE_E: credits transit on proven arrival (section 7.6), then judges
 *   invariant R; a failure sets the sticky E8 conflict when none is set. The
 *   record stays valid either way.
 * - TOKEN_FLAGS: stored.
 * The record becomes the subject's latest observation (its t_obs is the
 * anchor's execution timestamp).
 *
 * RESERVE_E_SUCCESSOR (0x12) and TOKEN_FLAGS_SUCCESSOR (0x13) are RESERVE_E
 * and TOKEN_FLAGS of the B-23 successor v' (section 7.6), valid only from
 * the migration height and read with v''s pins. A RESERVE_E_SUCCESSOR
 * credits v''s transit from v''s absorbedTotal and judges invariant R on
 * v''s ledger and proven deposits; its failure sets the same sticky E8
 * conflict.
 */
bool VerifyEthStateObservation(
    const bridge::BridgeEthStateObservationV2& record, const int height,
    const int64_t candidate_time, const uint256& block_hash,
    const Consensus::Params& params, const BridgeV2LineageContext* lineage,
    const BridgeStateIndex& index, ScratchState& state,
    BridgeBlockDelta& delta, std::string& error)
{
    if (!params.BridgeV2RulesActive(height) || !delta.v2_store) {
        error = "bridge V2 observation records are not active";
        return false;
    }
    const Consensus::BridgeV2Params& v2{*params.bridge_v2};
    const uint8_t subject{record.subject};
    const auto slot{BridgeObservationSlotV2(subject)};
    const uint8_t base_subject{bridge::EthObservationBaseSubject(subject)};
    if (!slot || base_subject == 0) {
        error = "bridge observation subject is unknown";
        return false;
    }
    const Consensus::BridgeV2EntryRole role{
        bridge::EthObservationEntryRole(subject)};
    if (role == Consensus::BridgeV2EntryRole::SUCCESSOR &&
        !params.BridgeV2MigrationActive(height)) {
        error = "bridge V2 successor observations are not active";
        return false;
    }
    const bridge::LightClientStore* light_client{state.LightClient()};
    if (light_client == nullptr) {
        error = "bridge observation precedes light-client bootstrap";
        return false;
    }
    if (!SlotUsesKnownFork(light_client->finalized_header.beacon.slot,
                           v2.light_client)) {
        error = "bridge observation uses a light-client head beyond the pinned fork schedule";
        return false;
    }
    if (!FinalizedHeadFresh(*light_client, candidate_time, v2.light_client)) {
        error = "bridge observation light-client head is stale";
        return false;
    }
    const BridgeExecutionAnchor* anchor{state.BaseAnchor(record.anchor_hash)};
    if (anchor == nullptr) {
        error = state.Anchor(record.anchor_hash) != nullptr
                    ? "bridge observation names an anchor connected by the same block"
                    : "bridge observation names an unretained execution anchor";
        return false;
    }
    if (anchor->state_root.IsNull()) {
        error = "bridge observation anchor has no state root";
        return false;
    }
    if (anchor->execution_timestamp >
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        error = "bridge observation anchor timestamp is out of range";
        return false;
    }
    const auto& latest{state.observations_v2[*slot]};
    if (latest && anchor->block_number <= latest->anchor_block) {
        error = "bridge observation anchor is not newer than the subject's latest observation";
        return false;
    }
    std::string detail;
    auto observed{bridge::VerifyEthStateObservationV2(
        record, anchor->state_root,
        bridge::ObservationPinsFromParams(v2, role), detail)};
    if (!observed) {
        error = "bridge observation is invalid: " + detail;
        return false;
    }

    if (base_subject == bridge::ETH_OBSERVATION_SUBJECT_VERIFIER) {
        const auto& verifier{std::get<bridge::VerifierStateV2>(*observed)};
        if (verifier.status != bridge::VERIFIER_STATUS_LIVE) {
            if (record.verdict != 0) {
                error = "bridge observation verdict must be zero for a verifier that is not LIVE";
                return false;
            }
        } else {
            if (lineage == nullptr) {
                error = "bridge observation verdict cannot be judged here";
                return false;
            }
            if (!lineage->trust_recorded_verdicts) {
                const auto expected{VerifierVerdictV2(verifier, *lineage, params,
                                                      index, height, error)};
                if (!expected) return false;
                if (*expected != record.verdict) {
                    error = "bridge observation verdict does not match B3's lineage";
                    return false;
                }
            }
            if ((record.verdict & bridge::ETH_OBSERVATION_VERDICT_CERT_CONFLICT) &&
                !state.conflict_v2) {
                state.conflict_v2 = BridgeV2Conflict{
                    bridge::BRIDGE_V2_CONFLICT_E5, height, block_hash,
                    anchor->block_number};
            }
        }
    } else if (base_subject == bridge::ETH_OBSERVATION_SUBJECT_RESERVE) {
        const auto& reserve{std::get<bridge::ReserveStateV2>(*observed)};
        bridge::ReserveLedgerV2 ledger{state.Ledger(role)};
        if (!bridge::CreditTransit(ledger, reserve.absorbed_total)) {
            error = "bridge V2 reserve ledger cannot credit transit";
            return false;
        }
        const auto sums{state.ProvenDepositSums(role, reserve.next_deposit_id)};
        if (!sums) {
            error = "bridge V2 proven deposits cannot be summed";
            return false;
        }
        state.Ledger(role) = ledger;
        if (!bridge::InvariantRHolds(reserve.locked, sums->below_cut,
                                     state.Ledger(role)) &&
            !state.conflict_v2) {
            state.conflict_v2 = BridgeV2Conflict{
                bridge::BRIDGE_V2_CONFLICT_E8, height, block_hash,
                anchor->block_number};
        }
    }

    BridgeObservationRecordV2 stored{
        subject,
        record.anchor_hash,
        anchor->block_number,
        static_cast<int64_t>(anchor->execution_timestamp),
        record.verdict,
        std::move(*observed),
        height,
        block_hash};
    state.observations_v2[*slot] = stored;
    state.gate_inputs_changed = true;
    delta.observations_v2_added.push_back(std::move(stored));
    delta.reserve_v2_after = state.reserve_v2;
    delta.reserve_v2_successor_after = state.reserve_v2_successor;
    delta.conflict_v2_after = state.conflict_v2;
    return true;
}

/**
 * Kind 13 FINALITY_CONFLICT from V (node changes section 8.5): valid only
 * while no sticky conflict is set (the first conflict wins, whether E5, E8
 * or kind 13, so a block accepts at most one kind 13 record), and only with
 * a lineage context. A full context judges it (JudgeFinalityConflictV2:
 * current or next committee, a valid quorum certificate, a statement other
 * than B3's). REPLAY_LINEAGE trusts it, as it trusts recorded verdicts: the
 * block was judged with a full context when it first connected.
 *
 * The record sets the sticky conflict (reason kind 13, anchor_block 0, the
 * evidence certId, checkpoint index and epoch), so the burn gate refuses
 * every later burn of the same block at C4 and every burn after it.
 */
bool VerifyFinalityConflictV2(
    const bridge::BridgeFinalityConflictV2& record, const int height,
    const uint256& block_hash, const Consensus::Params& params,
    const BridgeV2LineageContext* lineage, const BridgeStateIndex& index,
    ScratchState& state, BridgeBlockDelta& delta, std::string& error)
{
    if (!params.BridgeV2RulesActive(height) || !delta.v2_store) {
        error = "bridge V2 conflict records are not active";
        return false;
    }
    if (state.conflict_v2) {
        error = "bridge V2 sticky conflict is already set";
        return false;
    }
    if (lineage == nullptr) {
        error = "bridge finality conflict cannot be judged here";
        return false;
    }
    std::optional<modern::FinalizedBlockV2> evidence;
    if (lineage->trust_recorded_verdicts) {
        const auto decoded{record.Decode()};
        if (!decoded) {
            error = "bridge finality conflict certificate is malformed";
            return false;
        }
        evidence = decoded->first;
    } else {
        evidence = JudgeFinalityConflictV2(record, *lineage, params, index,
                                           height, error);
        if (!evidence) return false;
    }
    const uint256 cert_id{modern::FinalityCertIdV2(*evidence)};
    state.conflict_v2 = BridgeV2Conflict{
        bridge::BRIDGE_V2_CONFLICT_KIND13, height, block_hash,
        /*anchor_block=*/0, cert_id, evidence->checkpoint_index,
        evidence->epoch};
    state.gate_inputs_changed = true;
    delta.conflict_v2_after = state.conflict_v2;
    delta.finality_conflicts_v2_added.push_back(cert_id);
    return true;
}

bool VerifyBridgeTransaction(
    const CTransaction& tx, const int height, const int64_t candidate_time,
    const uint256& block_hash, const Consensus::Params& params,
    const BridgeV2LineageContext* lineage, const BridgeStateIndex& index,
    ScratchState& state, BridgeBlockDelta& delta,
    BridgeTxAuthorization& tx_result, std::string& error)
{
    tx_result = {};
    if (!params.busd_bridge || !params.busd_bridge->light_client) {
        error = "bridge consensus parameters are incomplete";
        return false;
    }
    // From V every record kind runs against the V2 regime (node changes
    // section 3.4): the caller's ScratchState already names the V2 store.
    const bool v2{params.BridgeV2RulesActive(height)};
    if (v2 != delta.v2_store) {
        error = "bridge candidate store does not match its height";
        return false;
    }
    const Consensus::EthereumLightClientPins& pins{
        v2 ? params.bridge_v2->light_client
           : *params.busd_bridge->light_client};
    const CMpaRecord* bridge_record{nullptr};
    for (const CMpaRecord& record : tx.mpa) {
        if (record.payload_type != bridge::BRIDGE_MPA_TYPE) continue;
        if (bridge_record != nullptr) {
            error = "transaction has more than one bridge record";
            return false;
        }
        bridge_record = &record;
    }
    if (bridge_record == nullptr) {
        if (modern::BridgeBindingOutputCount(tx) != 0) {
            error = "transaction has an orphan bridge binding output";
            return false;
        }
        return true;
    }
    if (tx.IsCoinBase()) {
        error = "coinbase transaction cannot carry a bridge record";
        return false;
    }
    if (!modern::CheckBridgeRecordBinding(tx, *bridge_record, error)) {
        return false;
    }
    // The V2 receipt limits (8,192-byte terminal node, 16 KiB proof) apply
    // only from V; below it a record that needs them is malformed.
    const auto decoded{bridge::DecodeBridgeMpaRecordV1(
        *bridge_record,
        v2 ? bridge::BRIDGE_MPT_LIMITS_V2 : bridge::BRIDGE_MPT_LIMITS_V1)};
    if (!decoded) {
        error = "bridge record is malformed";
        return false;
    }

    return std::visit(
        [&](const auto& payload) -> bool {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, bridge::BridgeBootstrapV1>) {
                return VerifyBootstrap(
                    payload, height, block_hash, pins,
                    v2 ? params.bridge_v2->vault.origin_deployment_block
                       : *params.busd_bridge->origin_deployment_block,
                    state, delta, error);
            } else if constexpr (std::is_same_v<T, bridge::BridgeUpdateV1>) {
                return VerifyUpdate(payload, height, block_hash, pins, state,
                                    delta, error);
            } else if constexpr (std::is_same_v<T, bridge::BridgeMintV1>) {
                if (v2) {
                    return VerifyMintV2(tx, payload, height, candidate_time,
                                        params, state, delta, tx_result,
                                        error);
                }
                return VerifyMint(tx, payload, height, candidate_time,
                                  block_hash, params, state, delta, tx_result,
                                  error);
            } else if constexpr (
                std::is_same_v<T, bridge::BridgeExecutionBackfillV1>) {
                return VerifyBackfill(payload, height, block_hash, state,
                                      delta, error);
            } else if constexpr (
                std::is_same_v<T, bridge::BridgeManagedWithdrawalV1>) {
                if (v2) {
                    error = "managed bridge withdrawals are closed from the bridge V2 activation height";
                    return false;
                }
                return VerifyWithdrawal(tx, payload, height, block_hash,
                                        params, state, delta, tx_result,
                                        error);
            } else if constexpr (
                std::is_same_v<T, bridge::BridgeBurnV1>) {
                if (v2) {
                    return VerifyBridgeBurnV2(tx, payload, height,
                                              candidate_time, block_hash,
                                              params, lineage, state, delta,
                                              tx_result, error);
                }
                return VerifyBridgeBurn(tx, payload, height, block_hash,
                                        params, state, delta, tx_result,
                                        error);
            } else if constexpr (
                std::is_same_v<T, bridge::BridgeEthStateObservationV2>) {
                return VerifyEthStateObservation(
                    payload, height, candidate_time, block_hash, params,
                    lineage, index, state, delta, error);
            } else if constexpr (
                std::is_same_v<T, bridge::BridgeFinalityConflictV2>) {
                return VerifyFinalityConflictV2(payload, height, block_hash,
                                                params, lineage, index, state,
                                                delta, error);
            } else if constexpr (
                std::is_same_v<T, bridge::BridgeVaultSweepV2>) {
                // The sweep leaf's source is the carrier's one binding
                // output (CheckBridgeRecordBinding above made it exact).
                const auto binding{
                    modern::MakeBridgeBindingOutput(*bridge_record)};
                const auto found{
                    binding ? std::find(tx.vout.begin(), tx.vout.end(),
                                        *binding)
                            : tx.vout.end()};
                if (found == tx.vout.end()) {
                    error = "bridge V2 vault sweep binding output is invalid";
                    return false;
                }
                return VerifyVaultSweepV2(
                    tx, payload,
                    static_cast<uint32_t>(found - tx.vout.begin()), height,
                    candidate_time, block_hash, params, lineage, state, delta,
                    tx_result, error);
            }
            error = "bridge record kind is unsupported";
            return false;
        },
        decoded->payload);
}

} // namespace

bool BridgeV2SponsoredSighashAllowed(const BridgeWithdrawalV2Request& withdrawal,
                                     const Consensus::Params& params,
                                     const int height)
{
    return !withdrawal.sweep && params.BridgeV2SponsoredSighashActive(height);
}

bool BridgeV2SponsoredSighashAllowed(const BridgeTxAuthorization& authorization,
                                     const Consensus::Params& params,
                                     const int height)
{
    if (!authorization.withdrawal_v2 || authorization.mint ||
        authorization.withdrawal || authorization.decentralized_withdrawal) {
        return false;
    }
    return BridgeV2SponsoredSighashAllowed(*authorization.withdrawal_v2,
                                           params, height);
}

bool HasDecentralizedBridgeBurn(const CTransaction& tx)
{
    for (const CMpaRecord& record : tx.mpa) {
        if (record.payload_type != bridge::BRIDGE_MPA_TYPE) continue;
        // The V2 record set, so a burn with a route (d3) exit block counts
        // for the gate paths and the closing-block eviction. For kind 6 it
        // differs from the V1 set only by that block, which is malformed
        // below V, so such a transaction is refused there either way.
        const auto decoded{bridge::DecodeBridgeMpaRecordV1(
            record, bridge::BRIDGE_MPT_LIMITS_V2)};
        if (decoded &&
            std::holds_alternative<bridge::BridgeBurnV1>(decoded->payload)) {
            return true;
        }
    }
    return false;
}

bool HasDecentralizedBridgeBurn(const CBlock& block)
{
    return std::any_of(
        block.vtx.begin(), block.vtx.end(), [](const CTransactionRef& tx) {
            return tx && HasDecentralizedBridgeBurn(*tx);
        });
}

bool CheckBridgeBurnReadiness(const CTransaction& tx,
                              const BridgeBurnReadiness readiness,
                              std::string& error)
{
    if (!HasDecentralizedBridgeBurn(tx) ||
        readiness == BridgeBurnReadiness::READY) {
        return true;
    }
    error = readiness == BridgeBurnReadiness::UNAVAILABLE
                ? "decentralized bridge burn readiness is unavailable"
                : "decentralized bridge burn requires bridge-ready current and next validator sets";
    return false;
}

bool CheckBridgeBurnReadiness(const CBlock& block,
                              const BridgeBurnReadiness readiness,
                              std::string& error)
{
    if (!HasDecentralizedBridgeBurn(block) ||
        readiness == BridgeBurnReadiness::READY) {
        return true;
    }
    error = readiness == BridgeBurnReadiness::UNAVAILABLE
                ? "decentralized bridge burn readiness is unavailable"
                : "decentralized bridge burn requires bridge-ready current and next validator sets";
    return false;
}

//! Whether `tx` carries a bridge record, decoded under the bridge V2 record
//! set, of one of the payload types `Kinds`.
template <typename... Kinds>
static bool HasBridgeV2Record(const CTransaction& tx)
{
    for (const CMpaRecord& record : tx.mpa) {
        if (record.payload_type != bridge::BRIDGE_MPA_TYPE) continue;
        const auto decoded{bridge::DecodeBridgeMpaRecordV1(
            record, bridge::BRIDGE_MPT_LIMITS_V2)};
        if (decoded &&
            (std::holds_alternative<Kinds>(decoded->payload) || ...)) {
            return true;
        }
    }
    return false;
}

bool HasBridgeV2LineageRecord(const CTransaction& tx)
{
    return HasBridgeV2Record<bridge::BridgeEthStateObservationV2,
                             bridge::BridgeFinalityConflictV2,
                             bridge::BridgeVaultSweepV2>(tx);
}

bool HasBridgeV2VaultSweep(const CTransaction& tx)
{
    return HasBridgeV2Record<bridge::BridgeVaultSweepV2>(tx);
}

bool HasBridgeV2RetiredEntryBurn(const CTransaction& tx,
                                 const Consensus::Params& params,
                                 const int height)
{
    if (!params.BridgeV2MigrationActive(height)) return false;
    const auto launch_id{modern::ConfiguredBridgeV2EntryId(
        params, Consensus::BridgeV2EntryRole::LAUNCH)};
    if (!launch_id) return false;
    for (const CMpaRecord& record : tx.mpa) {
        if (record.payload_type != bridge::BRIDGE_MPA_TYPE) continue;
        // The V2 record set: a burn with an exit block is a burn too.
        const auto decoded{bridge::DecodeBridgeMpaRecordV1(
            record, bridge::BRIDGE_MPT_LIMITS_V2)};
        if (!decoded) continue;
        const auto* burn{std::get_if<bridge::BridgeBurnV1>(&decoded->payload)};
        if (burn != nullptr && burn->registry_id == *launch_id) return true;
    }
    return false;
}

bool HasBridgeV2LineageRecord(const CBlock& block)
{
    return std::any_of(
        block.vtx.begin(), block.vtx.end(), [](const CTransactionRef& tx) {
            return tx && HasBridgeV2LineageRecord(*tx);
        });
}

bool HasBridgeV2FinalityConflict(const CTransaction& tx)
{
    return HasBridgeV2Record<bridge::BridgeFinalityConflictV2>(tx);
}

const BridgeV2LineageContext REPLAY_LINEAGE{
    /*trust_recorded_verdicts=*/true, /*fresh=*/std::nullopt, /*hash_at=*/{}};

std::optional<BridgeV2LineageContext> MakeBridgeV2LineageContext(
    FinalityTracker& finality, const CChain& chain,
    const BlockManager& blockman, const Consensus::Params& params,
    const CBlockIndex& parent, const BridgeStateIndex& bridge_index,
    std::string& reason)
{
    reason.clear();
    if (!params.bridge_v2) {
        reason = "bridge V2 is not configured";
        return std::nullopt;
    }
    if (!finality.Sync(chain, blockman, params, parent, &bridge_index)) {
        reason = "the fresh finality lineage cannot be reconstructed at the parent block";
        return std::nullopt;
    }
    BridgeV2LineageContext context;
    context.fresh = finality.Projected(parent.nHeight + 1, params).fresh;
    // Block index entries live as long as the node; the context only ever
    // reads ancestors of the parent.
    const CBlockIndex* const parent_index{&parent};
    context.hash_at = [parent_index](const int h) -> std::optional<uint256> {
        const CBlockIndex* ancestor{parent_index->GetAncestor(h)};
        if (ancestor == nullptr) return std::nullopt;
        return ancestor->GetBlockHash();
    };
    return context;
}

std::optional<uint8_t> VerifierVerdictV2(
    const bridge::VerifierStateV2& observed,
    const BridgeV2LineageContext& lineage, const Consensus::Params& params,
    const BridgeStateIndex& index, const int height, std::string& error)
{
    const auto refuse{[&error](std::string why) -> std::optional<uint8_t> {
        error = std::move(why);
        return std::nullopt;
    }};
    if (!params.modern_pos || params.modern_pos->checkpoint_interval <= 0) {
        return refuse("bridge observation verdict needs modern PoS checkpoint parameters");
    }
    if (!lineage.fresh || !lineage.fresh->bootstrapped || !lineage.hash_at) {
        return refuse("bridge observation verdict needs a bootstrapped fresh lineage");
    }
    const FreshLineageState& fresh{*lineage.fresh};
    if (fresh.lineage_broken) {
        return refuse("bridge observation verdict cannot be judged on a broken fresh lineage");
    }
    // E3's window (node changes sections 8.4 and 8.6): the lineage retains
    // Set'_{e-2} .. Set'_{e+1}, so a verifier up to two epochs behind is
    // judged. A wider max_epoch_gap does not widen it.
    const uint64_t e_b3{fresh.epoch};
    const uint64_t e_e{observed.current_epoch};
    if (e_e > e_b3 || e_b3 - e_e > FRESH_LINEAGE_RETAINED_PAST_EPOCHS) {
        return refuse("bridge observation verifier epoch is outside the lineage's retained epochs (e_B3 - 2 .. e_B3)");
    }
    if (height < 1 ||
        observed.latest_height > static_cast<uint64_t>(height - 1)) {
        return refuse("bridge observation verifier checkpoint is above the parent block");
    }
    // e_e + 1 cannot wrap: e_e <= e_b3, a count of rotations.
    const auto set_hash{fresh.SetHashForEpoch(e_e)};
    const auto successor_hash{fresh.SetHashForEpoch(e_e + 1)};
    if (!set_hash || !successor_hash) {
        return refuse("bridge observation verdict lacks the lineage sets of the verifier epoch");
    }

    uint8_t verdict{0};
    // E4: the verifier's pinned pair must be Set'_{e_E} and Set'_{e_E+1}.
    if (observed.current_set_hash != *set_hash ||
        observed.next_set_hash != *successor_hash) {
        verdict |= bridge::ETH_OBSERVATION_VERDICT_SETS_MISMATCH;
    }
    // E5: Latest must sit on the checkpoint grid at its index, and its
    // certId must be CertIdAt(index), B3's own certId there. The verifier
    // refuses index 0 (initialize and submitCertificate), so index 0 while
    // LIVE is a conflict too. After any accepted statement the verifier's
    // currentEpoch is that statement's epoch, so e_E is its epoch. B3 signs
    // a checkpoint only in the epoch of its height (the signer and
    // JudgeFinalityConflictV2 check 9 apply the same rule), so a Latest whose
    // epoch e_E is not the epoch of Latest.height is not B3's certificate,
    // whatever its other fields.
    const uint64_t interval{
        static_cast<uint64_t>(params.modern_pos->checkpoint_interval)};
    const bool on_grid{
        observed.latest_index != 0 && fresh.genesis_height >= 0 &&
        observed.latest_height >= static_cast<uint64_t>(fresh.genesis_height) &&
        (observed.latest_height - static_cast<uint64_t>(fresh.genesis_height)) %
                interval == 0 &&
        (observed.latest_height - static_cast<uint64_t>(fresh.genesis_height)) /
                interval == observed.latest_index};
    if (!on_grid) {
        verdict |= bridge::ETH_OBSERVATION_VERDICT_CERT_CONFLICT;
        return verdict;
    }
    // Latest.height <= height - 1 fits an int (checked above).
    const int latest_height{static_cast<int>(observed.latest_height)};
    const auto checkpoint_epoch{
        modern::EpochOfHeight(fresh.epoch_starts, latest_height)};
    if (!checkpoint_epoch) {
        return refuse("bridge observation verdict: B3 cannot form its statement at the verifier's latest checkpoint");
    }
    if (*checkpoint_epoch != e_e) {
        verdict |= bridge::ETH_OBSERVATION_VERDICT_CERT_CONFLICT;
        return verdict;
    }
    const auto canonical{ExpectedFinalizedBlockV2(
        e_e, latest_height, fresh, lineage.hash_at, params, &index)};
    if (!canonical) {
        return refuse("bridge observation verdict: B3 cannot form its statement at the verifier's latest checkpoint");
    }
    if (modern::FinalityCertIdV2(*canonical) != observed.latest_cert_id) {
        verdict |= bridge::ETH_OBSERVATION_VERDICT_CERT_CONFLICT;
    }
    return verdict;
}

std::optional<modern::FinalizedBlockV2> JudgeFinalityConflictV2(
    const bridge::BridgeFinalityConflictV2& record,
    const BridgeV2LineageContext& lineage, const Consensus::Params& params,
    const BridgeStateIndex& index, const int height, std::string& error)
{
    const auto refuse{
        [&error](std::string why) -> std::optional<modern::FinalizedBlockV2> {
            error = std::move(why);
            return std::nullopt;
        }};
    // 1. Parameters and the chain domain the certificate is signed under.
    if (!params.modern_pos || params.modern_pos->checkpoint_interval <= 0) {
        return refuse("bridge finality conflict needs modern PoS checkpoint parameters");
    }
    const auto domain{
        params.legacy_final_hash
            ? modern::ModernChainDomain(params.hashGenesisBlock,
                                        *params.legacy_final_hash)
            : std::nullopt};
    if (!domain) {
        return refuse("bridge finality conflict needs a pinned chain domain");
    }
    // 2. A lineage that can still be judged.
    if (!lineage.fresh || !lineage.fresh->bootstrapped || !lineage.hash_at) {
        return refuse("bridge finality conflict needs a bootstrapped fresh lineage");
    }
    const FreshLineageState& fresh{*lineage.fresh};
    if (fresh.lineage_broken) {
        return refuse("bridge finality conflict cannot be judged on a broken fresh lineage");
    }
    // 3. The certificate (the codec already checked its shape).
    const auto decoded{record.Decode()};
    if (!decoded) {
        return refuse("bridge finality conflict certificate is malformed");
    }
    const modern::FinalizedBlockV2& fb{decoded->first};
    const modern::FinalityCertificate& cert{decoded->second};
    // 4. This lineage.
    if (fb.lineage_id != fresh.lineage_id) {
        return refuse("bridge finality conflict names another lineage");
    }
    // 5. The current or next committee only (written without epoch + 1,
    // which could wrap); Set'_{e-1} and older are refused, as Verifier V2's
    // reportConflict refuses rotated-out sets.
    std::shared_ptr<const FreshLineageSet> committee;
    if (fb.epoch == fresh.epoch) {
        committee = fresh.current;
    } else if (fb.epoch > fresh.epoch && fb.epoch - fresh.epoch == 1) {
        committee = fresh.next;
    }
    if (!committee || !committee->members) {
        return refuse("bridge finality conflict is not signed by the lineage's current or next committee");
    }
    // 6. The bitmap is sized for that committee.
    if (record.validator_count != committee->Size()) {
        return refuse("bridge finality conflict bitmap does not match the signing committee");
    }
    // 7. The checkpoint the index names, at or below the parent block.
    const int interval{params.modern_pos->checkpoint_interval};
    if (fb.checkpoint_index == 0 ||
        fb.checkpoint_index >= modern::MAX_FINALITY_CHECKPOINT_INDEX ||
        fresh.genesis_height < 0 ||
        fb.checkpoint_index >
            static_cast<uint64_t>(std::numeric_limits<int>::max() -
                                  fresh.genesis_height) /
                static_cast<uint64_t>(interval)) {
        return refuse("bridge finality conflict checkpoint index is out of range");
    }
    const int checkpoint{fresh.genesis_height +
                         static_cast<int>(fb.checkpoint_index) * interval};
    if (height < 1 || checkpoint > height - 1) {
        return refuse("bridge finality conflict checkpoint is above the parent block");
    }
    // 8. A quorum of that committee signed it. The statement's own successor
    // hash is passed, so the successor field is not checked: a wrong one is
    // just another way to differ from B3's statement.
    const modern::CertificateCheck check{modern::VerifyFinalityCertificateV2(
        *domain, fb, cert, committee->View(), fb.validator_set_hash)};
    if (check != modern::CertificateCheck::OK) {
        return refuse(std::string{"bridge finality conflict certificate does not verify: "} +
                      modern::CertificateCheckName(check));
    }
    // 9. It differs from B3's canonical statement at that checkpoint
    // (ExpectedFinalizedBlockV2, field for field): B3 signs index k only in
    // the epoch of its height, so another epoch differs by construction.
    const auto cannot_form{[&refuse] {
        return refuse("bridge finality conflict: B3 cannot form its statement at the evidence checkpoint");
    }};
    const auto checkpoint_epoch{
        modern::EpochOfHeight(fresh.epoch_starts, checkpoint)};
    if (!checkpoint_epoch) return cannot_form();
    if (fb.epoch != *checkpoint_epoch) return fb;
    // Every field but the history root is cheap to form, and any difference
    // proves the conflict. Only a statement equal in all of them (such as a
    // canonical certificate wrapped as kind 13) reads B3's history root: the
    // finalized checkpoint's recorded root at its index, else
    // FreshHistoryRootAt (an O(index) rebuild below the lineage's
    // accumulator; header cost note).
    const auto block_hash{lineage.hash_at(checkpoint)};
    const auto successor{fresh.SuccessorHashForEpoch(fb.epoch)};
    const auto withdrawal_root{
        FreshLineageWithdrawalRoot(checkpoint, params, &index)};
    const auto bridge_flags{FreshLineageBridgeFlags(checkpoint, params, &index)};
    if (!block_hash || !successor || !withdrawal_root || !bridge_flags) {
        return cannot_form();
    }
    if (fb.height != static_cast<uint64_t>(checkpoint) ||
        fb.block_hash != *block_hash ||
        fb.withdrawal_root != *withdrawal_root ||
        fb.validator_set_hash != *successor ||
        fb.bridge_flags != *bridge_flags) {
        return fb;
    }
    const auto history_root{
        fresh.finalized &&
                fresh.finalized->checkpoint_index == fb.checkpoint_index
            ? std::optional<uint256>{fresh.finalized->history_root}
            : FreshHistoryRootAt(fresh, fb.checkpoint_index, lineage.hash_at,
                                 interval)};
    if (!history_root) return cannot_form();
    if (fb.checkpoint_history_root == *history_root) {
        return refuse("bridge finality conflict equals B3's canonical statement");
    }
    return fb;
}

std::string BridgeV2GateResult::Describe() const
{
    switch (readiness) {
    case BridgeBurnReadiness::READY:
        return "ready";
    case BridgeBurnReadiness::NOT_READY:
        return std::string{bridge::BurnGateRowName(row)} + ": " + reason;
    case BridgeBurnReadiness::UNAVAILABLE:
        break;
    }
    return "unavailable: " + reason;
}

BridgeV2GateResult EvaluateBridgeV2BurnGate(
    const BridgeV2LineageContext* lineage, const BridgeStateIndex& index,
    const Consensus::Params& params, const int height,
    const int64_t candidate_time)
{
    BridgeV2GateResult out;
    out.now_eff = candidate_time;
    if (!params.bridge_v2) {
        out.reason = "bridge V2 is not configured";
        return out;
    }
    if (lineage == nullptr || lineage->trust_recorded_verdicts) {
        out.reason = "the fresh finality lineage is unavailable";
        return out;
    }
    if (index.ConnectedHeight() != height - 1) {
        out.reason = "the bridge state is not connected at the candidate's parent";
        return out;
    }
    return EvaluateBurnGateOn(*lineage, index.ObservationsV2(),
                              index.ConflictV2(), index.LightClientV2(),
                              params, height, candidate_time,
                              modern::ActiveBridgeV2Role(params, height));
}

BridgeV2GateResult EvaluateBridgeV2SweepGate(
    const BridgeV2LineageContext* lineage, const BridgeStateIndex& index,
    const Consensus::Params& params, const int height,
    const int64_t candidate_time)
{
    BridgeV2GateResult out;
    out.now_eff = candidate_time;
    if (!params.bridge_v2) {
        out.reason = "bridge V2 is not configured";
        return out;
    }
    if (!params.BridgeV2MigrationActive(height)) {
        out.reason = "bridge V2 vault sweeps are not active at this height";
        return out;
    }
    if (lineage == nullptr || lineage->trust_recorded_verdicts) {
        out.reason = "the fresh finality lineage is unavailable";
        return out;
    }
    if (index.ConnectedHeight() != height - 1) {
        out.reason = "the bridge state is not connected at the candidate's parent";
        return out;
    }
    return EvaluateBurnGateOn(*lineage, index.ObservationsV2(),
                              index.ConflictV2(), index.LightClientV2(),
                              params, height, candidate_time,
                              Consensus::BridgeV2EntryRole::LAUNCH);
}

std::optional<std::string> BridgeV2DepositsClosedReason(
    const BridgeStateIndex& index, const BridgeV2LineageContext* lineage,
    const Consensus::Params& params)
{
    if (!params.bridge_v2) return std::nullopt;
    // C4: the sticky conflict, first wins.
    if (const auto& conflict{index.ConflictV2()}) {
        const char* kind{"unknown"};
        switch (conflict->reason) {
        case bridge::BRIDGE_V2_CONFLICT_E5: kind = "E5"; break;
        case bridge::BRIDGE_V2_CONFLICT_E8: kind = "E8"; break;
        case bridge::BRIDGE_V2_CONFLICT_KIND13: kind = "kind 13"; break;
        }
        return std::string{"sticky "} + kind + " conflict at height " +
               std::to_string(conflict->height);
    }
    // C2's permanent case: the fresh lineage's extension is exhausted.
    if (lineage != nullptr && lineage->fresh && lineage->fresh->lineage_broken) {
        return std::string{"the fresh finality lineage is broken"};
    }
    // C1, and Ethereum's own close, from the latest VERIFIER_E at any age.
    const auto observed{
        index.LatestObservationV2(bridge::ETH_OBSERVATION_SUBJECT_VERIFIER)};
    const auto* verifier{
        observed ? std::get_if<bridge::VerifierStateV2>(&observed->state)
                 : nullptr};
    if (verifier != nullptr) {
        const std::string seen{"the latest VERIFIER_E (anchor block " +
                               std::to_string(observed->anchor_block) +
                               ") shows "};
        if (const auto impairment{bridge::VerifierImpairmentV2(
                {observed->t_obs, *verifier, observed->verdict},
                params.bridge_v2->gate)}) {
            return seen + "the " + *impairment;
        }
        if (verifier->deposits_closed) {
            return seen + "Verifier V2 depositsClosed";
        }
    }
    return std::nullopt;
}

namespace {

std::string BridgeV2BurnReadinessError(const BridgeBurnReadiness readiness)
{
    return readiness == BridgeBurnReadiness::UNAVAILABLE
               ? "bridge V2 burn readiness is unavailable"
               : "bridge V2 burn gate is not ready";
}

} // namespace

bool CheckBridgeV2BurnReadiness(const CTransaction& tx,
                                const BridgeBurnReadiness readiness,
                                std::string& error)
{
    if (!HasDecentralizedBridgeBurn(tx) ||
        readiness == BridgeBurnReadiness::READY) {
        return true;
    }
    error = BridgeV2BurnReadinessError(readiness);
    return false;
}

bool CheckBridgeV2BurnReadiness(const CBlock& block,
                                const BridgeBurnReadiness readiness,
                                std::string& error)
{
    if (!HasDecentralizedBridgeBurn(block) ||
        readiness == BridgeBurnReadiness::READY) {
        return true;
    }
    error = BridgeV2BurnReadinessError(readiness);
    return false;
}

struct BridgeBlockPreview::Impl {
    Impl(const int candidate_height, const int64_t candidate_time,
         const uint256& preview_block_id, const Consensus::Params& params,
         const bool v2_store,
         const std::optional<bridge::LightClientStore>& light_client,
         const AnchorMap& anchors, const AnchorHeightMap& anchor_by_height,
         const NullifierSet& nullifiers, const EpochMintMap& epoch_minted,
         const WithdrawalMap& withdrawals,
         const DecentralizedWithdrawalMap& decentralized_withdrawals,
         const DecentralizedWithdrawalSourceMap& decentralized_sources,
         const modern::WithdrawalTreeState& withdrawal_tree,
         const WithdrawalV2Map& withdrawals_v2,
         const WithdrawalV2SourceMap& withdrawal_v2_sources,
         const modern::WithdrawalTreeState& withdrawal_tree_v2,
         const BridgeObservationSlotsV2& observations_v2,
         const bridge::ReserveLedgerV2& reserve_v2,
         const bridge::ProvenDepositMapV2& deposits_v2,
         const CAmount deposits_v2_total,
         const std::optional<BridgeV2Conflict>& conflict_v2,
         const bridge::ReserveLedgerV2& reserve_v2_successor,
         const bridge::ProvenDepositMapV2& deposits_v2_successor,
         const CAmount deposits_v2_successor_total,
         const int connected_height, const uint256& connected_hash,
         const BridgeStateIndex& base_index,
         const BridgeV2LineageContext* lineage_context)
        : height{candidate_height},
          time{candidate_time},
          block_id{preview_block_id},
          consensus{params},
          index{base_index},
          lineage{lineage_context
                      ? std::optional<BridgeV2LineageContext>{*lineage_context}
                      : std::nullopt},
          state{std::make_unique<ScratchState>(
              light_client, anchors, anchor_by_height, nullifiers,
              epoch_minted, withdrawals, decentralized_withdrawals,
              decentralized_sources, withdrawal_tree, withdrawals_v2,
              withdrawal_v2_sources, withdrawal_tree_v2, observations_v2,
              reserve_v2, deposits_v2, deposits_v2_total, conflict_v2,
              reserve_v2_successor, deposits_v2_successor,
              deposits_v2_successor_total)}
    {
        delta.height = height;
        delta.block_hash = block_id;
        delta.previous_height = connected_height;
        delta.previous_block_hash = connected_hash;
        delta.withdrawal_tree_before = withdrawal_tree;
        delta.withdrawal_tree_after = withdrawal_tree;
        delta.v2_store = v2_store;
        delta.withdrawal_tree_v2_before = withdrawal_tree_v2;
        delta.withdrawal_tree_v2_after = withdrawal_tree_v2;
        delta.observations_v2_before = observations_v2;
        delta.reserve_v2_before = reserve_v2;
        delta.reserve_v2_after = reserve_v2;
        delta.conflict_v2_before = conflict_v2;
        delta.conflict_v2_after = conflict_v2;
        delta.reserve_v2_successor_before = reserve_v2_successor;
        delta.reserve_v2_successor_after = reserve_v2_successor;
    }

    const int height;
    const int64_t time;
    const uint256 block_id;
    const Consensus::Params& consensus;
    const BridgeStateIndex& index;
    const std::optional<BridgeV2LineageContext> lineage;
    std::unique_ptr<ScratchState> state;
    BridgeBlockDelta delta{};
};

BridgeBlockPreview::BridgeBlockPreview(std::unique_ptr<Impl> impl)
    : m_impl{std::move(impl)}
{
}

BridgeBlockPreview::~BridgeBlockPreview() = default;

bool BridgeBlockPreview::TryAppend(
    const std::span<const CTransactionRef> transactions,
    std::string& error)
{
    error.clear();

    // Ordinary chunks cannot affect the bridge overlay. Avoid even copying
    // the small candidate-local state unless a type-10 record or its policy-9
    // binding is present. Null transaction references are never valid input.
    bool has_bridge_material{false};
    for (const CTransactionRef& tx : transactions) {
        if (!tx) {
            error = "bridge candidate chunk contains a null transaction";
            return false;
        }
        has_bridge_material |= modern::BridgeBindingOutputCount(*tx) != 0;
        has_bridge_material |= std::any_of(
            tx->mpa.begin(), tx->mpa.end(), [](const CMpaRecord& record) {
                return record.payload_type == bridge::BRIDGE_MPA_TYPE;
            });
    }
    if (!has_bridge_material) return true;

    // Copy only this candidate block's overlay and accumulated effects. The
    // unbounded connected maps remain borrowed by reference and are never
    // copied or mutated during miner preview.
    auto trial_state{std::make_unique<ScratchState>(*m_impl->state)};
    BridgeBlockDelta trial_delta{m_impl->delta};
    for (const CTransactionRef& tx : transactions) {
        BridgeTxAuthorization ignored;
        if (!VerifyBridgeTransaction(
                *tx, m_impl->height, m_impl->time, m_impl->block_id,
                m_impl->consensus,
                m_impl->lineage ? &*m_impl->lineage : nullptr, m_impl->index,
                *trial_state, trial_delta, ignored, error)) {
            return false;
        }
    }

    m_impl->state = std::move(trial_state);
    m_impl->delta = std::move(trial_delta);
    return true;
}

namespace {

std::optional<BridgeExecutionAnchor> FindAnchor(const AnchorMap& anchors,
                                                const uint256& block_hash)
{
    const auto it{anchors.find(block_hash)};
    return it == anchors.end()
               ? std::nullopt
               : std::optional<BridgeExecutionAnchor>{it->second};
}

std::optional<BridgeExecutionAnchor> FindAnchorForTarget(
    const AnchorMap& anchors, const AnchorHeightMap& anchor_by_height,
    const uint64_t target_block_number, const int finalized_b3_height)
{
    if (finalized_b3_height < 0) return std::nullopt;

    for (auto it{anchor_by_height.lower_bound(target_block_number)};
         it != anchor_by_height.end(); ++it) {
        const auto anchor_it{anchors.find(it->second)};
        if (anchor_it == anchors.end()) continue;
        const BridgeExecutionAnchor& anchor{anchor_it->second};
        if (anchor.block_number != it->first ||
            anchor.connected_height < 0 ||
            anchor.connected_height > finalized_b3_height ||
            anchor.source_finalized_execution_block < target_block_number ||
            anchor.source_finalized_execution_block - target_block_number >
                bridge::MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS) {
            continue;
        }
        return anchor;
    }
    return std::nullopt;
}

} // namespace

std::optional<BridgeExecutionAnchor> BridgeStateIndex::Anchor(
    const uint256& block_hash) const
{
    return FindAnchor(m_anchors, block_hash);
}

std::optional<BridgeExecutionAnchor> BridgeStateIndex::AnchorV2(
    const uint256& block_hash) const
{
    return FindAnchor(m_anchors_v2, block_hash);
}

std::optional<BridgeExecutionAnchor> BridgeStateIndex::AnchorForTarget(
    const uint64_t target_block_number, const int finalized_b3_height) const
{
    return FindAnchorForTarget(m_anchors, m_anchor_by_height,
                               target_block_number, finalized_b3_height);
}

std::optional<BridgeExecutionAnchor> BridgeStateIndex::AnchorForTargetV2(
    const uint64_t target_block_number, const int finalized_b3_height) const
{
    return FindAnchorForTarget(m_anchors_v2, m_anchor_by_height_v2,
                               target_block_number, finalized_b3_height);
}

size_t BridgeStateIndex::NullifierCount(
    const uint8_t nullifier_namespace) const
{
    return static_cast<size_t>(std::count_if(
        m_nullifiers.begin(), m_nullifiers.end(),
        [nullifier_namespace](const bridge::BridgeDepositKey& key) {
            return key.nullifier_namespace == nullifier_namespace;
        }));
}

std::optional<BridgeStateConnection> BridgeStateIndex::NullifierConnection(
    const bridge::BridgeDepositKey& key) const
{
    const auto it{m_nullifier_connections.find(key)};
    return it == m_nullifier_connections.end()
               ? std::nullopt
               : std::optional<BridgeStateConnection>{it->second};
}

std::optional<BridgeLightClientSnapshot>
BridgeStateIndex::FinalizedLightClientSnapshot(
    const int finalized_b3_height) const
{
    return FinalizedSnapshot(finalized_b3_height, /*v2_store=*/false);
}

std::optional<BridgeLightClientSnapshot>
BridgeStateIndex::FinalizedLightClientSnapshotV2(
    const int finalized_b3_height) const
{
    return FinalizedSnapshot(finalized_b3_height, /*v2_store=*/true);
}

std::optional<BridgeLightClientSnapshot> BridgeStateIndex::FinalizedSnapshot(
    const int finalized_b3_height, const bool v2_store) const
{
    if (finalized_b3_height < 0) return std::nullopt;
    const auto& light_client{v2_store ? m_light_client_v2 : m_light_client};
    const auto& light_client_connection{
        v2_store ? m_light_client_connection_v2 : m_light_client_connection};
    const auto& light_client_floor{
        v2_store ? m_light_client_floor_v2 : m_light_client_floor};

    const auto eligible{[finalized_b3_height](
                            const std::optional<bridge::LightClientStore>& store,
                            const std::optional<BridgeStateConnection>& connection)
                            -> std::optional<BridgeLightClientSnapshot> {
        if (!store || !connection || connection->height < 0 ||
            connection->height > finalized_b3_height) {
            return std::nullopt;
        }
        return BridgeLightClientSnapshot{*store, *connection};
    }};

    // If the current store is final, it is necessarily the newest candidate.
    if (const auto current{eligible(light_client,
                                    light_client_connection)}) {
        return current;
    }

    // Every retained changing delta carries both sides of the transition.
    // Checking newest-to-oldest therefore also recovers the exact store just
    // before the oldest retained update without keeping another large copy.
    // A delta's light-client fields belong to the store its v2_store names.
    for (auto it{m_history.rbegin()}; it != m_history.rend(); ++it) {
        if (it->v2_store != v2_store) continue;
        if (const auto after{eligible(it->light_client_after,
                                     it->light_client_connection_after)}) {
            return after;
        }
        if (const auto before{eligible(it->light_client_before,
                                      it->light_client_connection_before)}) {
            return before;
        }
    }

    if (light_client_floor &&
        light_client_floor->connection.height >= 0 &&
        light_client_floor->connection.height <= finalized_b3_height) {
        return light_client_floor;
    }
    return std::nullopt;
}

std::optional<BridgeManagedWithdrawalRequest> BridgeStateIndex::Withdrawal(
    const BridgeWithdrawalId& id) const
{
    const auto it{m_withdrawals.find(id)};
    return it == m_withdrawals.end()
               ? std::nullopt
               : std::optional<BridgeManagedWithdrawalRequest>{it->second};
}

std::optional<BridgeDecentralizedWithdrawalRequest>
BridgeStateIndex::DecentralizedWithdrawal(
    const uint64_t withdrawal_id) const
{
    const auto it{m_decentralized_withdrawals.find(withdrawal_id)};
    return it == m_decentralized_withdrawals.end()
               ? std::nullopt
               : std::optional<BridgeDecentralizedWithdrawalRequest>{
                     it->second};
}

std::optional<BridgeDecentralizedWithdrawalRequest>
BridgeStateIndex::DecentralizedWithdrawal(
    const BridgeWithdrawalId& source) const
{
    const auto source_it{m_decentralized_withdrawal_sources.find(source)};
    if (source_it == m_decentralized_withdrawal_sources.end()) {
        return std::nullopt;
    }
    const auto withdrawal_it{
        m_decentralized_withdrawals.find(source_it->second)};
    if (withdrawal_it == m_decentralized_withdrawals.end() ||
        withdrawal_it->second.transaction_id != source.transaction_id ||
        withdrawal_it->second.burn_output_index !=
            source.burn_output_index ||
        withdrawal_it->second.withdrawal.withdrawal_id !=
            source_it->second) {
        return std::nullopt;
    }
    return withdrawal_it->second;
}

std::optional<std::vector<BridgeDecentralizedWithdrawalRequest>>
BridgeStateIndex::DecentralizedWithdrawalsThrough(const int height) const
{
    if (height < 0 || height > m_connected_height ||
        !m_withdrawal_roots.contains(height)) {
        return std::nullopt;
    }
    std::vector<BridgeDecentralizedWithdrawalRequest> out;
    for (const auto& [id, request] : m_decentralized_withdrawals) {
        if (request.connected_height > height) break;
        if (id != out.size() || request.withdrawal.withdrawal_id != id ||
            request.withdrawal.b3_height !=
                static_cast<uint64_t>(request.connected_height)) {
            return std::nullopt;
        }
        out.push_back(request);
    }
    return out;
}

std::optional<uint256> BridgeStateIndex::WithdrawalRootAtHeight(
    const int height) const
{
    const auto it{m_withdrawal_roots.find(height)};
    return it == m_withdrawal_roots.end()
               ? std::nullopt
               : std::optional<uint256>{it->second};
}

std::optional<uint256> FinalityWithdrawalRoot(
    const int height, const Consensus::Params& params,
    const BridgeStateIndex* bridge_index)
{
    if (!Consensus::BridgeRulesActive(height, params) ||
        !params.busd_bridge || !params.busd_bridge->withdrawal_mode ||
        *params.busd_bridge->withdrawal_mode !=
            Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1) {
        return uint256{};
    }
    if (bridge_index == nullptr) return std::nullopt;
    return bridge_index->WithdrawalRootAtHeight(height);
}

std::optional<BridgeWithdrawalV2Request> BridgeStateIndex::WithdrawalV2(
    const uint64_t withdrawal_id) const
{
    const auto it{m_withdrawals_v2.find(withdrawal_id)};
    return it == m_withdrawals_v2.end()
               ? std::nullopt
               : std::optional<BridgeWithdrawalV2Request>{it->second};
}

std::optional<BridgeWithdrawalV2Request> BridgeStateIndex::WithdrawalV2(
    const BridgeWithdrawalId& source) const
{
    const auto source_it{m_withdrawal_v2_sources.find(source)};
    if (source_it == m_withdrawal_v2_sources.end()) return std::nullopt;
    const auto withdrawal_it{m_withdrawals_v2.find(source_it->second)};
    if (withdrawal_it == m_withdrawals_v2.end() ||
        withdrawal_it->second.transaction_id != source.transaction_id ||
        withdrawal_it->second.burn_output_index !=
            source.burn_output_index ||
        withdrawal_it->second.withdrawal.withdrawal_id !=
            source_it->second) {
        return std::nullopt;
    }
    return withdrawal_it->second;
}

std::optional<std::vector<BridgeWithdrawalV2Request>>
BridgeStateIndex::WithdrawalsV2Through(const int height) const
{
    if (height < 0 || height > m_connected_height ||
        !m_withdrawal_v2_roots.contains(height)) {
        return std::nullopt;
    }
    std::vector<BridgeWithdrawalV2Request> out;
    for (const auto& [id, request] : m_withdrawals_v2) {
        if (request.connected_height > height) break;
        if (id != out.size() || request.withdrawal.withdrawal_id != id ||
            request.withdrawal.b3_height !=
                static_cast<uint64_t>(request.connected_height)) {
            return std::nullopt;
        }
        out.push_back(request);
    }
    return out;
}

std::optional<uint256> BridgeStateIndex::WithdrawalRootV2AtHeight(
    const int height) const
{
    const auto it{m_withdrawal_v2_roots.find(height)};
    return it == m_withdrawal_v2_roots.end()
               ? std::nullopt
               : std::optional<uint256>{it->second};
}

std::optional<uint256> FinalityWithdrawalRootV2(
    const int height, const Consensus::Params& params,
    const BridgeStateIndex* bridge_index)
{
    if (!params.BridgeV2RulesActive(height)) return uint256{};
    if (bridge_index == nullptr) return std::nullopt;
    return bridge_index->WithdrawalRootV2AtHeight(height);
}

uint8_t BridgeStateIndex::BridgeFlagsV2AtHeight(const int height) const
{
    return m_conflict_v2 && m_conflict_v2->height <= height
               ? modern::FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED
               : uint8_t{0};
}

std::optional<uint8_t> FinalityBridgeFlagsV2(
    const int height, const Consensus::Params& params,
    const BridgeStateIndex* bridge_index)
{
    if (!params.BridgeV2RulesActive(height)) return uint8_t{0};
    // A V2 root marks a height the index has connected; fail closed
    // otherwise, exactly as FinalityWithdrawalRootV2.
    if (bridge_index == nullptr ||
        !bridge_index->WithdrawalRootV2AtHeight(height)) {
        return std::nullopt;
    }
    return bridge_index->BridgeFlagsV2AtHeight(height);
}

CAmount BridgeStateIndex::EpochMinted(const modern::AssetId& asset,
                                      const uint64_t epoch) const
{
    const auto it{m_epoch_minted.find({asset, epoch})};
    return it == m_epoch_minted.end() ? 0 : it->second;
}

std::optional<BridgeObservationRecordV2> BridgeStateIndex::LatestObservationV2(
    const uint8_t subject) const
{
    const auto slot{BridgeObservationSlotV2(subject)};
    if (!slot) return std::nullopt;
    return m_observations_v2[*slot];
}

std::optional<BridgeExecutionAnchor> BridgeStateIndex::NewestStateAnchorV2()
    const
{
    for (auto it{m_anchor_by_height_v2.rbegin()};
         it != m_anchor_by_height_v2.rend(); ++it) {
        const auto anchor{m_anchors_v2.find(it->second)};
        if (anchor != m_anchors_v2.end() &&
            !anchor->second.state_root.IsNull()) {
            return anchor->second;
        }
    }
    return std::nullopt;
}

std::unique_ptr<BridgeBlockPreview> BridgeStateIndex::BeginBlockPreview(
    const int candidate_height, const int64_t candidate_time,
    const uint256& preview_block_id, const Consensus::Params& params,
    std::string& error, const BridgeV2LineageContext* lineage) const
{
    error.clear();
    if (!IsConfiguredAndActive(params, candidate_height, error)) {
        return nullptr;
    }
    if (candidate_time < 0 ||
        candidate_time > std::numeric_limits<uint32_t>::max()) {
        error = "bridge candidate block time is out of range";
        return nullptr;
    }
    if (preview_block_id.IsNull()) {
        error = "bridge preview block id is null";
        return nullptr;
    }

    const bool v2_store{params.BridgeV2RulesActive(candidate_height)};
    auto impl{std::make_unique<BridgeBlockPreview::Impl>(
        candidate_height, candidate_time, preview_block_id, params, v2_store,
        v2_store ? m_light_client_v2 : m_light_client,
        v2_store ? m_anchors_v2 : m_anchors,
        v2_store ? m_anchor_by_height_v2 : m_anchor_by_height, m_nullifiers,
        m_epoch_minted, m_withdrawals, m_decentralized_withdrawals,
        m_decentralized_withdrawal_sources, m_withdrawal_tree,
        m_withdrawals_v2, m_withdrawal_v2_sources, m_withdrawal_tree_v2,
        m_observations_v2, m_reserve_v2, m_deposits_v2, m_deposits_v2_total,
        m_conflict_v2, m_reserve_v2_successor, m_deposits_v2_successor,
        m_deposits_v2_successor_total, m_connected_height, m_connected_hash,
        *this, lineage)};
    return std::unique_ptr<BridgeBlockPreview>{
        new BridgeBlockPreview{std::move(impl)}};
}

bool BridgeStateIndex::VerifyBlock(
    const CBlock& block, const int height, const uint256& block_hash,
    const Consensus::Params& params, BridgeBlockDelta& out,
    std::string& error, const BridgeV2LineageContext* lineage) const
{
    out = {};
    if (!IsConfiguredAndActive(params, height, error)) return false;
    if (block_hash.IsNull()) {
        error = "bridge block hash is null";
        return false;
    }
    out.height = height;
    out.block_hash = block_hash;
    out.previous_height = m_connected_height;
    out.previous_block_hash = m_connected_hash;
    out.withdrawal_tree_before = m_withdrawal_tree;
    out.withdrawal_tree_after = m_withdrawal_tree;
    out.v2_store = params.BridgeV2RulesActive(height);
    out.withdrawal_tree_v2_before = m_withdrawal_tree_v2;
    out.withdrawal_tree_v2_after = m_withdrawal_tree_v2;
    out.observations_v2_before = m_observations_v2;
    out.reserve_v2_before = m_reserve_v2;
    out.reserve_v2_after = m_reserve_v2;
    out.conflict_v2_before = m_conflict_v2;
    out.conflict_v2_after = m_conflict_v2;
    out.reserve_v2_successor_before = m_reserve_v2_successor;
    out.reserve_v2_successor_after = m_reserve_v2_successor;

    const auto& light_client{out.v2_store ? m_light_client_v2
                                          : m_light_client};
    const auto& light_client_connection{
        out.v2_store ? m_light_client_connection_v2
                     : m_light_client_connection};
    ScratchState state{light_client,
                       out.v2_store ? m_anchors_v2 : m_anchors,
                       out.v2_store ? m_anchor_by_height_v2
                                    : m_anchor_by_height,
                       m_nullifiers, m_epoch_minted, m_withdrawals,
                       m_decentralized_withdrawals,
                       m_decentralized_withdrawal_sources,
                       m_withdrawal_tree, m_withdrawals_v2,
                       m_withdrawal_v2_sources, m_withdrawal_tree_v2,
                       m_observations_v2, m_reserve_v2, m_deposits_v2,
                       m_deposits_v2_total, m_conflict_v2,
                       m_reserve_v2_successor, m_deposits_v2_successor,
                       m_deposits_v2_successor_total};
    for (const CTransactionRef& tx : block.vtx) {
        BridgeTxAuthorization ignored;
        if (!VerifyBridgeTransaction(*tx, height, block.GetBlockTime(),
                                     block_hash, params, lineage, *this,
                                     state, out, ignored, error)) {
            return false;
        }
    }
    if (state.light_client_changed) {
        out.light_client_before = light_client;
        out.light_client_after = state.LightClientState();
        out.light_client_connection_before = light_client_connection;
        out.light_client_connection_after =
            BridgeStateConnection{height, block_hash};
    }
    for (const auto& [key, after] : state.epoch_minted_overlay) {
        const auto prior{m_epoch_minted.find(key)};
        const CAmount before{
            prior == m_epoch_minted.end() ? 0 : prior->second};
        out.epoch_mint_changes.push_back(
            BridgeEpochMintChange{key.first, key.second, before, after});
    }
    out.withdrawal_tree_after = state.withdrawal_tree;
    out.withdrawal_tree_v2_after = state.withdrawal_tree_v2;
    out.reserve_v2_after = state.reserve_v2;
    out.conflict_v2_after = state.conflict_v2;
    out.reserve_v2_successor_after = state.reserve_v2_successor;
    return true;
}

bool BridgeStateIndex::VerifyTransaction(
    const CTransaction& tx, const int candidate_height,
    const int64_t candidate_time, const Consensus::Params& params,
    BridgeTxAuthorization& out, std::string& error,
    const BridgeV2LineageContext* lineage) const
{
    out = {};
    if (candidate_time < 0 ||
        candidate_time > std::numeric_limits<uint32_t>::max()) {
        error = "bridge candidate transaction time is out of range";
        return false;
    }
    CBlock candidate;
    candidate.nTime = static_cast<uint32_t>(candidate_time);
    candidate.vtx.push_back(MakeTransactionRef(tx));
    uint256 preview_hash;
    preview_hash.begin()[0] = 1;
    BridgeBlockDelta delta;
    if (!VerifyBlock(candidate, candidate_height, preview_hash, params, delta,
                     error, lineage)) {
        return false;
    }
    if (!delta.mint_authorizations.empty()) {
        if (delta.mint_authorizations.size() != 1) {
            error = "bridge candidate transaction produced multiple mint authorizations";
            return false;
        }
        out.mint = delta.mint_authorizations.front();
    }
    if (!delta.withdrawals_added.empty()) {
        if (delta.withdrawals_added.size() != 1) {
            error = "bridge candidate transaction produced multiple withdrawal requests";
            return false;
        }
        out.withdrawal = delta.withdrawals_added.front();
    }
    if (!delta.decentralized_withdrawals_added.empty()) {
        if (delta.decentralized_withdrawals_added.size() != 1) {
            error = "bridge candidate transaction produced multiple decentralized withdrawals";
            return false;
        }
        out.decentralized_withdrawal =
            delta.decentralized_withdrawals_added.front();
    }
    if (!delta.withdrawals_v2_added.empty()) {
        if (delta.withdrawals_v2_added.size() != 1) {
            error = "bridge candidate transaction produced multiple V2 withdrawals";
            return false;
        }
        out.withdrawal_v2 = delta.withdrawals_v2_added.front();
    }
    return true;
}

bool BridgeStateIndex::ConnectBlock(const BridgeBlockDelta& delta,
                                    std::string& error)
{
    if (delta.height < 0 || delta.block_hash.IsNull()) {
        error = "invalid bridge block delta identity";
        return false;
    }
    if (delta.previous_height != m_connected_height ||
        delta.previous_block_hash != m_connected_hash ||
        (m_connected_height >= 0 &&
         delta.height != m_connected_height + 1)) {
        error = "bridge block delta does not extend the connected tip";
        return false;
    }

    // The light-client fields and anchors of a delta belong to the store its
    // v2_store flag names (node changes section 3.4).
    std::optional<bridge::LightClientStore>& light_client{
        delta.v2_store ? m_light_client_v2 : m_light_client};
    std::optional<BridgeStateConnection>& light_client_connection{
        delta.v2_store ? m_light_client_connection_v2
                       : m_light_client_connection};
    std::optional<BridgeLightClientSnapshot>& light_client_floor{
        delta.v2_store ? m_light_client_floor_v2 : m_light_client_floor};
    AnchorMap& anchors{delta.v2_store ? m_anchors_v2 : m_anchors};
    AnchorHeightMap& anchor_by_height{
        delta.v2_store ? m_anchor_by_height_v2 : m_anchor_by_height};

    // Validate every effect against the live base state and against earlier
    // entries in this delta before mutating any live container.
    const bool light_client_changed{
        delta.light_client_before.has_value() ||
        delta.light_client_after.has_value()};
    const bool light_client_connection_changed{
        delta.light_client_connection_before.has_value() ||
        delta.light_client_connection_after.has_value()};
    if (light_client_changed != light_client_connection_changed) {
        error =
            "bridge delta light-client connection metadata does not match store change";
        return false;
    }
    if (light_client_changed) {
        const BridgeStateConnection expected{delta.height, delta.block_hash};
        if (!delta.light_client_after ||
            !SameOptionalStore(light_client, delta.light_client_before) ||
            SameOptionalStore(delta.light_client_before,
                              delta.light_client_after) ||
            delta.light_client_connection_before !=
                light_client_connection ||
            delta.light_client_connection_after != expected) {
            error = "bridge delta light-client before-state does not match";
            return false;
        }
        if (delta.light_client_before &&
            !SameLightClientHeader(
                delta.light_client_before->finalized_header,
                delta.light_client_after->finalized_header) &&
            !ExecutionRangeReachable(
                delta.light_client_after->finalized_header.execution.block_number,
                delta.light_client_before->finalized_header.execution.block_number)) {
            error =
                "bridge delta light-client execution advance exceeds the backfill window";
            return false;
        }
    }

    std::set<uint256> added_anchor_hashes;
    std::set<uint64_t> added_anchor_heights;
    for (const BridgeExecutionAnchor& anchor : delta.anchors_added) {
        if (anchor.block_hash.IsNull() || anchor.receipts_root.IsNull() ||
            !HasValidBackfillOrigin(anchor) ||
            anchor.connected_height != delta.height ||
            anchor.connected_block != delta.block_hash ||
            anchors.contains(anchor.block_hash) ||
            anchor_by_height.contains(anchor.block_number) ||
            !added_anchor_hashes.insert(anchor.block_hash).second ||
            !added_anchor_heights.insert(anchor.block_number).second) {
            error = "bridge delta contains a duplicate or mismatched execution anchor";
            return false;
        }
    }

    std::set<bridge::BridgeDepositKey> authorized_nullifiers;
    std::set<Ptxid> authorized_transactions;
    CAmount authorized_amount{0};
    for (const BridgeTxMintAuthorization& authorization :
         delta.mint_authorizations) {
        if (authorization.transaction_id.IsNull() ||
            authorization.authorization.asset.IsNull() ||
            authorization.authorization.amount <= 0 ||
            authorization.authorization.amount > MAX_MONEY ||
            authorization.authorization.recipient_script.empty() ||
            !authorized_transactions.insert(authorization.transaction_id).second ||
            !authorized_nullifiers
                 .insert(authorization.authorization.nullifier)
                 .second ||
            authorized_amount >
                MAX_MONEY - authorization.authorization.amount) {
            error = "bridge delta contains a malformed mint authorization";
            return false;
        }
        authorized_amount += authorization.authorization.amount;
    }
    // V1 mints use namespace 1 below V; V2 mints namespace 2 from V. The V1
    // nullifier set is therefore frozen at V.
    const uint8_t expected_namespace{
        delta.v2_store ? bridge::BRIDGE_NULLIFIER_NAMESPACE_V2
                       : bridge::BRIDGE_NULLIFIER_NAMESPACE_V1};
    std::set<bridge::BridgeDepositKey> listed_nullifiers;
    for (const bridge::BridgeDepositKey& nullifier :
         delta.nullifiers_added) {
        if (nullifier.nullifier_namespace != expected_namespace) {
            error = "bridge delta nullifier namespace does not match its store";
            return false;
        }
        if (!listed_nullifiers.insert(nullifier).second ||
            m_nullifiers.contains(nullifier) ||
            m_nullifier_connections.contains(nullifier)) {
            error = "bridge delta duplicates a deposit nullifier";
            return false;
        }
    }
    if (listed_nullifiers != authorized_nullifiers) {
        error = "bridge delta mint authorizations do not match nullifiers";
        return false;
    }

    CAmount changed_amount{0};
    std::set<std::pair<modern::AssetId, uint64_t>> changed_epochs;
    for (const BridgeEpochMintChange& change : delta.epoch_mint_changes) {
        const auto key{std::make_pair(change.asset, change.epoch)};
        const auto current{m_epoch_minted.find(key)};
        const CAmount before{
            current == m_epoch_minted.end() ? 0 : current->second};
        if (change.asset.IsNull() || change.before != before ||
            change.after <= change.before || change.after > MAX_MONEY ||
            !changed_epochs.insert(key).second ||
            changed_amount > MAX_MONEY - (change.after - change.before)) {
            error = "bridge delta contains an invalid epoch mint change";
            return false;
        }
        changed_amount += change.after - change.before;
    }
    if (changed_amount != authorized_amount) {
        error = "bridge delta mint amount does not match epoch accounting";
        return false;
    }

    // The V1 withdrawal requests and tree freeze at V; V2 leaves exist only
    // from V.
    if (delta.v2_store
            ? (!delta.withdrawals_added.empty() ||
               !delta.decentralized_withdrawals_added.empty())
            : !delta.withdrawals_v2_added.empty()) {
        error = "bridge delta withdrawals do not match its store";
        return false;
    }

    std::set<Txid> withdrawal_transactions;
    for (const BridgeManagedWithdrawalRequest& request :
         delta.withdrawals_added) {
        const BridgeWithdrawalId id{request.transaction_id,
                                    request.burn_output_index};
        if (request.transaction_id.IsNull() || request.asset.IsNull() ||
            request.amount <= 0 || request.amount > MAX_MONEY ||
            bridge::EthAddressIsNull(request.ethereum_recipient) ||
            request.connected_height != delta.height ||
            request.connected_block != delta.block_hash ||
            !withdrawal_transactions.insert(request.transaction_id).second ||
            m_withdrawals.contains(id)) {
            error = "bridge delta contains an invalid withdrawal request";
            return false;
        }
    }

    if (!(delta.withdrawal_tree_before == m_withdrawal_tree)) {
        error = "bridge delta withdrawal-tree before-state does not match";
        return false;
    }
    modern::WithdrawalTreeState expected_tree{m_withdrawal_tree};
    std::set<BridgeWithdrawalId> decentralized_sources;
    for (const BridgeDecentralizedWithdrawalRequest& request :
         delta.decentralized_withdrawals_added) {
        const BridgeWithdrawalId source{request.transaction_id,
                                        request.burn_output_index};
        const auto expected_leaf{
            modern::BridgeWithdrawalLeafV1(request.withdrawal)};
        if (request.transaction_id.IsNull() ||
            request.withdrawal.withdrawal_id != expected_tree.count ||
            request.withdrawal.b3_height !=
                static_cast<uint64_t>(delta.height) ||
            request.connected_height != delta.height ||
            request.connected_block != delta.block_hash || !expected_leaf ||
            *expected_leaf != request.leaf ||
            !withdrawal_transactions.insert(request.transaction_id).second ||
            !decentralized_sources.insert(source).second ||
            m_decentralized_withdrawal_sources.contains(source) ||
            m_decentralized_withdrawals.contains(
                request.withdrawal.withdrawal_id) ||
            !modern::AppendWithdrawalLeaf(expected_tree, request.leaf)) {
            error = "bridge delta contains an invalid decentralized withdrawal";
            return false;
        }
    }
    if (!(delta.withdrawal_tree_after == expected_tree)) {
        error = "bridge delta withdrawal-tree after-state does not match";
        return false;
    }
    if (m_withdrawal_roots.contains(delta.height)) {
        error = "bridge delta duplicates a withdrawal-root height";
        return false;
    }

    if (!(delta.withdrawal_tree_v2_before == m_withdrawal_tree_v2)) {
        error = "bridge delta V2 withdrawal-tree before-state does not match";
        return false;
    }
    modern::WithdrawalTreeState expected_tree_v2{m_withdrawal_tree_v2};
    std::set<BridgeWithdrawalId> v2_sources;
    for (const BridgeWithdrawalV2Request& request :
         delta.withdrawals_v2_added) {
        const BridgeWithdrawalId source{request.transaction_id,
                                        request.burn_output_index};
        const auto expected_leaf{
            modern::BridgeWithdrawalLeafV2(request.withdrawal)};
        if (request.transaction_id.IsNull() ||
            request.withdrawal.withdrawal_id != expected_tree_v2.count ||
            request.withdrawal.b3_height !=
                static_cast<uint64_t>(delta.height) ||
            request.connected_height != delta.height ||
            request.connected_block != delta.block_hash || !expected_leaf ||
            *expected_leaf != request.leaf ||
            !withdrawal_transactions.insert(request.transaction_id).second ||
            !v2_sources.insert(source).second ||
            m_withdrawal_v2_sources.contains(source) ||
            m_withdrawals_v2.contains(request.withdrawal.withdrawal_id) ||
            !modern::AppendWithdrawalLeaf(expected_tree_v2, request.leaf)) {
            error = "bridge delta contains an invalid V2 withdrawal";
            return false;
        }
    }
    if (!(delta.withdrawal_tree_v2_after == expected_tree_v2)) {
        error = "bridge delta V2 withdrawal-tree after-state does not match";
        return false;
    }
    // V2 roots exist only from V: below it (and on every chain without a
    // bridge V2 envelope) the V2 tree stays empty and FinalityWithdrawalRootV2
    // is zero without consulting the index, so nothing is recorded there.
    if (delta.v2_store && m_withdrawal_v2_roots.contains(delta.height)) {
        error = "bridge delta duplicates a V2 withdrawal-root height";
        return false;
    }

    // Bridge V2 accounting (node changes sections 7.1, 7.6, 8.2 and 8.4):
    // it changes only from V; below it every field passes through.
    if (!(delta.observations_v2_before == m_observations_v2) ||
        !(delta.reserve_v2_before == m_reserve_v2) ||
        delta.conflict_v2_before != m_conflict_v2 ||
        !(delta.reserve_v2_successor_before == m_reserve_v2_successor)) {
        error = "bridge delta V2 accounting before-state does not match";
        return false;
    }
    BridgeObservationSlotsV2 expected_observations{m_observations_v2};
    CAmount deposits_v2_total_after{m_deposits_v2_total};
    CAmount deposits_v2_successor_total_after{m_deposits_v2_successor_total};
    if (!delta.v2_store) {
        if (!delta.observations_v2_added.empty() ||
            !delta.deposits_v2_added.empty() ||
            !delta.deposits_v2_successor_added.empty() ||
            !delta.finality_conflicts_v2_added.empty() ||
            !(delta.reserve_v2_after == delta.reserve_v2_before) ||
            !(delta.reserve_v2_successor_after ==
              delta.reserve_v2_successor_before) ||
            delta.conflict_v2_after != delta.conflict_v2_before) {
            error = "bridge delta V2 accounting does not match its store";
            return false;
        }
    } else {
        // Kind 13 evidence: at most one accepted record per block (the first
        // sets the sticky conflict and refuses every later one), never null.
        if (delta.finality_conflicts_v2_added.size() > 1 ||
            std::any_of(delta.finality_conflicts_v2_added.begin(),
                        delta.finality_conflicts_v2_added.end(),
                        [](const uint256& id) { return id.IsNull(); })) {
            error = "bridge delta contains a malformed finality conflict";
            return false;
        }
        bool cert_conflict_added{false};
        for (const BridgeObservationRecordV2& record :
             delta.observations_v2_added) {
            const auto anchor{anchors.find(record.anchor_hash)};
            // The decoded state follows the base subject (RESERVE_E and
            // RESERVE_E_SUCCESSOR both decode a ReserveStateV2).
            const auto slot{BridgeObservationSlotV2(record.subject)};
            const uint8_t base_subject{
                bridge::EthObservationBaseSubject(record.subject)};
            if (!slot || base_subject == 0 ||
                record.state.index() != size_t{base_subject} - 1 ||
                (record.verdict & ~bridge::ETH_OBSERVATION_VERDICT_MASK) != 0 ||
                (record.subject != bridge::ETH_OBSERVATION_SUBJECT_VERIFIER &&
                 record.verdict != 0) ||
                record.connected_height != delta.height ||
                record.connected_block != delta.block_hash ||
                anchor == anchors.end() ||
                anchor->second.block_number != record.anchor_block ||
                anchor->second.state_root.IsNull() ||
                anchor->second.execution_timestamp >
                    static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
                static_cast<int64_t>(anchor->second.execution_timestamp) !=
                    record.t_obs) {
                error = "bridge delta contains a malformed observation";
                return false;
            }
            auto& latest{expected_observations[*slot]};
            if (latest && record.anchor_block <= latest->anchor_block) {
                error = "bridge delta observation anchors are not increasing";
                return false;
            }
            if (record.subject == bridge::ETH_OBSERVATION_SUBJECT_VERIFIER) {
                const auto& verifier{
                    std::get<bridge::VerifierStateV2>(record.state)};
                if (verifier.status != bridge::VERIFIER_STATUS_LIVE &&
                    record.verdict != 0) {
                    error = "bridge delta contains a malformed observation";
                    return false;
                }
                cert_conflict_added |=
                    (record.verdict &
                     bridge::ETH_OBSERVATION_VERDICT_CERT_CONFLICT) != 0;
            }
            latest = record;
        }

        // Each entry's ledger moves only by this block's V2 mints into its
        // vault, the leaves naming it, the sweep leaves paying it and transit
        // credited on proven arrival (sections 7.1 and 7.6).
        const auto sum_deposits{
            [](const std::vector<BridgeProvenDepositV2>& deposits,
               const bridge::ProvenDepositMapV2& connected,
               CAmount& deposited) {
                deposited = 0;
                std::set<uint64_t> ids;
                for (const BridgeProvenDepositV2& deposit : deposits) {
                    if (deposit.amount <= 0 || deposit.amount > MAX_MONEY ||
                        !ids.insert(deposit.deposit_id).second ||
                        connected.contains(deposit.deposit_id) ||
                        deposited > MAX_MONEY - deposit.amount) {
                        return false;
                    }
                    deposited += deposit.amount;
                }
                return true;
            }};
        CAmount deposited{0};
        CAmount deposited_successor{0};
        if (!sum_deposits(delta.deposits_v2_added, m_deposits_v2, deposited) ||
            !sum_deposits(delta.deposits_v2_successor_added,
                          m_deposits_v2_successor, deposited_successor) ||
            deposited > MAX_MONEY - deposited_successor) {
            error = "bridge delta contains an invalid proven deposit";
            return false;
        }
        const auto total_after{CheckedAdd(m_deposits_v2_total, deposited)};
        const auto successor_total_after{
            CheckedAdd(m_deposits_v2_successor_total, deposited_successor)};
        if (!total_after || !successor_total_after) {
            error = "bridge delta contains an invalid proven deposit";
            return false;
        }
        deposits_v2_total_after = *total_after;
        deposits_v2_successor_total_after = *successor_total_after;
        // burned_v sums the leaves naming each entry (sweeps name the
        // launch entry v); transit_v' receives the sweep amounts.
        CAmount burned{0};
        CAmount burned_successor{0};
        CAmount swept{0};
        for (const BridgeWithdrawalV2Request& request :
             delta.withdrawals_v2_added) {
            const CAmount amount{request.withdrawal.amount};
            CAmount& total{request.entry ==
                                   static_cast<uint8_t>(
                                       Consensus::BridgeV2EntryRole::SUCCESSOR)
                               ? burned_successor
                               : burned};
            if (amount <= 0 ||
                request.entry > static_cast<uint8_t>(
                                    Consensus::BridgeV2EntryRole::SUCCESSOR) ||
                (request.sweep &&
                 (request.entry != static_cast<uint8_t>(
                                       Consensus::BridgeV2EntryRole::LAUNCH) ||
                  request.exit)) ||
                total > MAX_MONEY - amount ||
                (request.sweep && swept > MAX_MONEY - amount)) {
                error = "bridge delta contains an invalid V2 withdrawal";
                return false;
            }
            total += amount;
            if (request.sweep) swept += amount;
        }
        const bridge::ReserveLedgerV2& before{delta.reserve_v2_before};
        const bridge::ReserveLedgerV2& after{delta.reserve_v2_after};
        const bridge::ReserveLedgerV2& next_before{
            delta.reserve_v2_successor_before};
        const bridge::ReserveLedgerV2& next_after{
            delta.reserve_v2_successor_after};
        // The successor's transit before the block plus the swept amounts is
        // what its credits and remaining transit share; both are in range.
        const auto next_inflow{CheckedAdd(next_before.transit, swept)};
        if (delta.deposits_v2_added.size() +
                    delta.deposits_v2_successor_added.size() !=
                delta.mint_authorizations.size() ||
            deposited + deposited_successor != authorized_amount ||
            !after.Valid() ||
            after.minted < before.minted ||
            after.minted - before.minted != deposited ||
            after.burned < before.burned ||
            after.burned - before.burned != burned ||
            after.credited < before.credited ||
            after.transit > before.transit ||
            after.credited - before.credited !=
                before.transit - after.transit ||
            !next_after.Valid() || !next_inflow ||
            next_after.minted < next_before.minted ||
            next_after.minted - next_before.minted != deposited_successor ||
            next_after.burned < next_before.burned ||
            next_after.burned - next_before.burned != burned_successor ||
            next_after.credited < next_before.credited ||
            next_after.transit > *next_inflow ||
            next_after.credited - next_before.credited !=
                *next_inflow - next_after.transit) {
            error = "bridge delta reserve ledger does not match its effects";
            return false;
        }

        // The sticky conflict is set at most once, by this block's E5
        // verdict, E8 observation or kind 13 record, and never cleared by a
        // later block. Kind 13 evidence is accepted only when it sets it.
        if (delta.conflict_v2_before) {
            if (delta.conflict_v2_after != delta.conflict_v2_before) {
                error = "bridge delta clears or replaces the sticky conflict";
                return false;
            }
            if (!delta.finality_conflicts_v2_added.empty()) {
                error = "bridge delta contains a malformed finality conflict";
                return false;
            }
        } else if (delta.conflict_v2_after) {
            const BridgeV2Conflict& conflict{*delta.conflict_v2_after};
            if (conflict.height != delta.height ||
                conflict.block_hash != delta.block_hash) {
                error = "bridge delta contains a malformed sticky conflict";
                return false;
            }
            if (conflict.reason == bridge::BRIDGE_V2_CONFLICT_KIND13) {
                // Set by the block's one accepted kind 13 record.
                if (conflict.anchor_block != 0 ||
                    conflict.evidence_cert_id.IsNull() ||
                    conflict.evidence_checkpoint_index == 0 ||
                    conflict.evidence_checkpoint_index >=
                        modern::MAX_FINALITY_CHECKPOINT_INDEX ||
                    delta.finality_conflicts_v2_added !=
                        std::vector<uint256>{conflict.evidence_cert_id}) {
                    error = "bridge delta contains a malformed sticky conflict";
                    return false;
                }
            } else {
                // E5 or E8: set by one of the block's observations, carrying
                // no kind 13 evidence.
                const auto sets_conflict{[&](const BridgeObservationRecordV2& r) {
                    if (r.anchor_block != conflict.anchor_block) return false;
                    if (conflict.reason == bridge::BRIDGE_V2_CONFLICT_E5) {
                        return r.subject == bridge::ETH_OBSERVATION_SUBJECT_VERIFIER &&
                               (r.verdict &
                                bridge::ETH_OBSERVATION_VERDICT_CERT_CONFLICT) != 0;
                    }
                    return conflict.reason == bridge::BRIDGE_V2_CONFLICT_E8 &&
                           bridge::EthObservationBaseSubject(r.subject) ==
                               bridge::ETH_OBSERVATION_SUBJECT_RESERVE;
                }};
                if (!conflict.evidence_cert_id.IsNull() ||
                    conflict.evidence_checkpoint_index != 0 ||
                    conflict.evidence_epoch != 0 ||
                    std::none_of(delta.observations_v2_added.begin(),
                                 delta.observations_v2_added.end(),
                                 sets_conflict)) {
                    error = "bridge delta contains a malformed sticky conflict";
                    return false;
                }
                if (!delta.finality_conflicts_v2_added.empty()) {
                    error = "bridge delta contains a malformed finality conflict";
                    return false;
                }
            }
        } else {
            if (!delta.finality_conflicts_v2_added.empty()) {
                error = "bridge delta contains a malformed finality conflict";
                return false;
            }
            if (cert_conflict_added) {
                error = "bridge delta omits the sticky conflict of a CERT_CONFLICT verdict";
                return false;
            }
        }
    }

    if (light_client_changed) {
        light_client = delta.light_client_after;
        light_client_connection = delta.light_client_connection_after;
        if (!light_client_floor) {
            light_client_floor = BridgeLightClientSnapshot{
                *delta.light_client_after,
                *delta.light_client_connection_after};
        }
    }
    for (const BridgeExecutionAnchor& anchor : delta.anchors_added) {
        anchors.emplace(anchor.block_hash, anchor);
        anchor_by_height.emplace(anchor.block_number, anchor.block_hash);
    }
    for (const bridge::BridgeDepositKey& nullifier :
         delta.nullifiers_added) {
        m_nullifiers.insert(nullifier);
        m_nullifier_connections.emplace(
            nullifier,
            BridgeStateConnection{delta.height, delta.block_hash});
    }
    for (const BridgeEpochMintChange& change : delta.epoch_mint_changes) {
        m_epoch_minted[{change.asset, change.epoch}] = change.after;
    }
    for (const BridgeManagedWithdrawalRequest& request :
         delta.withdrawals_added) {
        m_withdrawals.emplace(
            BridgeWithdrawalId{request.transaction_id,
                               request.burn_output_index},
            request);
    }
    for (const BridgeDecentralizedWithdrawalRequest& request :
         delta.decentralized_withdrawals_added) {
        m_decentralized_withdrawals.emplace(
            request.withdrawal.withdrawal_id, request);
        m_decentralized_withdrawal_sources.emplace(
            BridgeWithdrawalId{request.transaction_id,
                               request.burn_output_index},
            request.withdrawal.withdrawal_id);
    }
    for (const BridgeWithdrawalV2Request& request :
         delta.withdrawals_v2_added) {
        m_withdrawals_v2.emplace(request.withdrawal.withdrawal_id, request);
        m_withdrawal_v2_sources.emplace(
            BridgeWithdrawalId{request.transaction_id,
                               request.burn_output_index},
            request.withdrawal.withdrawal_id);
    }
    m_withdrawal_tree = delta.withdrawal_tree_after;
    m_withdrawal_roots.emplace(delta.height, m_withdrawal_tree.root);
    m_withdrawal_tree_v2 = delta.withdrawal_tree_v2_after;
    if (delta.v2_store) {
        m_withdrawal_v2_roots.emplace(delta.height, m_withdrawal_tree_v2.root);
    }
    m_observations_v2 = expected_observations;
    m_reserve_v2 = delta.reserve_v2_after;
    for (const BridgeProvenDepositV2& deposit : delta.deposits_v2_added) {
        m_deposits_v2.emplace(deposit.deposit_id, deposit.amount);
    }
    m_deposits_v2_total = deposits_v2_total_after;
    m_conflict_v2 = delta.conflict_v2_after;
    m_reserve_v2_successor = delta.reserve_v2_successor_after;
    for (const BridgeProvenDepositV2& deposit :
         delta.deposits_v2_successor_added) {
        m_deposits_v2_successor.emplace(deposit.deposit_id, deposit.amount);
    }
    m_deposits_v2_successor_total = deposits_v2_successor_total_after;
    m_connected_height = delta.height;
    m_connected_hash = delta.block_hash;
    m_history.push_back(delta);
    if (m_history.size() > BRIDGE_STATE_UNDO_BLOCKS) {
        m_history.pop_front();
    }
    return true;
}

bool BridgeStateIndex::DisconnectBlock(const int height,
                                       const uint256& block_hash,
                                       std::string& error)
{
    if (m_connected_height != height || m_connected_hash != block_hash) {
        error = "bridge disconnect does not match the index tip";
        return false;
    }
    if (m_history.empty()) {
        error = "bridge disconnect undo is no longer retained";
        return false;
    }
    if (m_history.back().height != height ||
        m_history.back().block_hash != block_hash) {
        error = "bridge retained undo does not match the connected tip";
        return false;
    }
    const BridgeBlockDelta& delta{m_history.back()};
    std::optional<bridge::LightClientStore>& light_client{
        delta.v2_store ? m_light_client_v2 : m_light_client};
    std::optional<BridgeStateConnection>& light_client_connection{
        delta.v2_store ? m_light_client_connection_v2
                       : m_light_client_connection};
    std::optional<BridgeLightClientSnapshot>& light_client_floor{
        delta.v2_store ? m_light_client_floor_v2 : m_light_client_floor};
    AnchorMap& anchors{delta.v2_store ? m_anchors_v2 : m_anchors};
    AnchorHeightMap& anchor_by_height{
        delta.v2_store ? m_anchor_by_height_v2 : m_anchor_by_height};

    // Validate the complete undo against live state before removing or
    // rewriting anything. A failed deep or corrupt undo leaves state intact.
    const auto root_at_height{m_withdrawal_roots.find(height)};
    if (!(m_withdrawal_tree == delta.withdrawal_tree_after) ||
        root_at_height == m_withdrawal_roots.end() ||
        root_at_height->second != delta.withdrawal_tree_after.root) {
        error = "bridge undo withdrawal-tree after-state does not match";
        return false;
    }
    // A V2 root was recorded for this height exactly when it is V2-store.
    const auto root_v2_at_height{m_withdrawal_v2_roots.find(height)};
    if (!(m_withdrawal_tree_v2 == delta.withdrawal_tree_v2_after) ||
        (delta.v2_store
             ? (root_v2_at_height == m_withdrawal_v2_roots.end() ||
                root_v2_at_height->second !=
                    delta.withdrawal_tree_v2_after.root)
             : root_v2_at_height != m_withdrawal_v2_roots.end())) {
        error = "bridge undo V2 withdrawal-tree after-state does not match";
        return false;
    }
    BridgeObservationSlotsV2 observations_after{delta.observations_v2_before};
    for (const BridgeObservationRecordV2& record :
         delta.observations_v2_added) {
        const auto slot{BridgeObservationSlotV2(record.subject)};
        if (!slot) {
            error = "bridge undo V2 accounting after-state does not match";
            return false;
        }
        observations_after[*slot] = record;
    }
    const auto deposits_missing{
        [](const std::vector<BridgeProvenDepositV2>& deposits,
           const bridge::ProvenDepositMapV2& connected) {
            return std::any_of(deposits.begin(), deposits.end(),
                               [&](const BridgeProvenDepositV2& deposit) {
                                   const auto found{
                                       connected.find(deposit.deposit_id)};
                                   return found == connected.end() ||
                                          found->second != deposit.amount;
                               });
        }};
    if (!(m_observations_v2 == observations_after) ||
        !(m_reserve_v2 == delta.reserve_v2_after) ||
        m_conflict_v2 != delta.conflict_v2_after ||
        !(m_reserve_v2_successor == delta.reserve_v2_successor_after) ||
        deposits_missing(delta.deposits_v2_added, m_deposits_v2) ||
        deposits_missing(delta.deposits_v2_successor_added,
                         m_deposits_v2_successor)) {
        error = "bridge undo V2 accounting after-state does not match";
        return false;
    }
    for (auto it{delta.withdrawals_v2_added.rbegin()};
         it != delta.withdrawals_v2_added.rend(); ++it) {
        const auto found{m_withdrawals_v2.find(it->withdrawal.withdrawal_id)};
        const auto source_found{m_withdrawal_v2_sources.find(
            BridgeWithdrawalId{it->transaction_id, it->burn_output_index})};
        if (found == m_withdrawals_v2.end() || !(found->second == *it) ||
            source_found == m_withdrawal_v2_sources.end() ||
            source_found->second != it->withdrawal.withdrawal_id) {
            error = "bridge undo cannot remove its V2 withdrawal";
            return false;
        }
    }
    for (auto it{delta.decentralized_withdrawals_added.rbegin()};
         it != delta.decentralized_withdrawals_added.rend(); ++it) {
        const auto found{m_decentralized_withdrawals.find(
            it->withdrawal.withdrawal_id)};
        const BridgeWithdrawalId source{it->transaction_id,
                                        it->burn_output_index};
        const auto source_found{
            m_decentralized_withdrawal_sources.find(source)};
        if (found == m_decentralized_withdrawals.end() ||
            !(found->second == *it) ||
            source_found == m_decentralized_withdrawal_sources.end() ||
            source_found->second != it->withdrawal.withdrawal_id) {
            error = "bridge undo cannot remove its decentralized withdrawal";
            return false;
        }
    }
    for (auto it{delta.withdrawals_added.rbegin()};
         it != delta.withdrawals_added.rend(); ++it) {
        const BridgeWithdrawalId id{it->transaction_id,
                                    it->burn_output_index};
        const auto found{m_withdrawals.find(id)};
        if (found == m_withdrawals.end() || !(found->second == *it)) {
            error = "bridge undo cannot remove its withdrawal request";
            return false;
        }
    }
    for (auto it{delta.epoch_mint_changes.rbegin()};
         it != delta.epoch_mint_changes.rend(); ++it) {
        const auto key{std::make_pair(it->asset, it->epoch)};
        const auto found{m_epoch_minted.find(key)};
        if (found == m_epoch_minted.end() || found->second != it->after) {
            error = "bridge undo epoch accounting does not match";
            return false;
        }
    }
    for (auto it{delta.nullifiers_added.rbegin()};
         it != delta.nullifiers_added.rend(); ++it) {
        const auto connection{m_nullifier_connections.find(*it)};
        if (!m_nullifiers.contains(*it) ||
            connection == m_nullifier_connections.end() ||
            connection->second != BridgeStateConnection{height, block_hash}) {
            error = "bridge undo cannot remove its deposit nullifier";
            return false;
        }
    }
    for (auto it{delta.anchors_added.rbegin()};
         it != delta.anchors_added.rend(); ++it) {
        const auto found{anchors.find(it->block_hash)};
        const auto by_height{anchor_by_height.find(it->block_number)};
        if (found == anchors.end() || !(found->second == *it) ||
            by_height == anchor_by_height.end() ||
            by_height->second != it->block_hash) {
            error = "bridge undo cannot remove its execution anchor";
            return false;
        }
    }
    const bool light_client_changed{
        delta.light_client_before.has_value() ||
        delta.light_client_after.has_value()};
    if (light_client_changed) {
        if (!delta.light_client_after ||
            !delta.light_client_connection_after ||
            !SameOptionalStore(light_client, delta.light_client_after) ||
            light_client_connection !=
                delta.light_client_connection_after) {
            error = "bridge undo light-client after-state does not match";
            return false;
        }
    }

    for (auto it{delta.withdrawals_added.rbegin()};
         it != delta.withdrawals_added.rend(); ++it) {
        m_withdrawals.erase(
            BridgeWithdrawalId{it->transaction_id,
                               it->burn_output_index});
    }
    for (auto it{delta.decentralized_withdrawals_added.rbegin()};
         it != delta.decentralized_withdrawals_added.rend(); ++it) {
        m_decentralized_withdrawals.erase(
            it->withdrawal.withdrawal_id);
        m_decentralized_withdrawal_sources.erase(
            BridgeWithdrawalId{it->transaction_id,
                               it->burn_output_index});
    }
    for (auto it{delta.withdrawals_v2_added.rbegin()};
         it != delta.withdrawals_v2_added.rend(); ++it) {
        m_withdrawals_v2.erase(it->withdrawal.withdrawal_id);
        m_withdrawal_v2_sources.erase(
            BridgeWithdrawalId{it->transaction_id, it->burn_output_index});
    }
    m_withdrawal_roots.erase(height);
    m_withdrawal_tree = delta.withdrawal_tree_before;
    m_withdrawal_v2_roots.erase(height);
    m_withdrawal_tree_v2 = delta.withdrawal_tree_v2_before;
    m_observations_v2 = delta.observations_v2_before;
    m_reserve_v2 = delta.reserve_v2_before;
    for (const BridgeProvenDepositV2& deposit : delta.deposits_v2_added) {
        m_deposits_v2.erase(deposit.deposit_id);
        // Each amount was added on connect, so the total stays >= 0.
        m_deposits_v2_total -= deposit.amount;
    }
    m_conflict_v2 = delta.conflict_v2_before;
    m_reserve_v2_successor = delta.reserve_v2_successor_before;
    for (const BridgeProvenDepositV2& deposit :
         delta.deposits_v2_successor_added) {
        m_deposits_v2_successor.erase(deposit.deposit_id);
        m_deposits_v2_successor_total -= deposit.amount;
    }
    for (auto it{delta.epoch_mint_changes.rbegin()};
         it != delta.epoch_mint_changes.rend(); ++it) {
        const auto key{std::make_pair(it->asset, it->epoch)};
        if (it->before == 0) {
            m_epoch_minted.erase(key);
        } else {
            m_epoch_minted[key] = it->before;
        }
    }
    for (auto it{delta.nullifiers_added.rbegin()};
         it != delta.nullifiers_added.rend(); ++it) {
        m_nullifiers.erase(*it);
        m_nullifier_connections.erase(*it);
    }
    for (auto it{delta.anchors_added.rbegin()};
         it != delta.anchors_added.rend(); ++it) {
        anchors.erase(it->block_hash);
        anchor_by_height.erase(it->block_number);
    }
    if (light_client_changed) {
        if (light_client_floor &&
            light_client_floor->connection ==
                *delta.light_client_connection_after) {
            light_client_floor.reset();
        }
        light_client = delta.light_client_before;
        light_client_connection = delta.light_client_connection_before;
    }
    m_connected_height = delta.previous_height;
    m_connected_hash = delta.previous_block_hash;
    m_history.pop_back();
    return true;
}

void BridgeStateIndex::Clear()
{
    m_light_client.reset();
    m_light_client_connection.reset();
    m_light_client_floor.reset();
    m_anchors.clear();
    m_anchor_by_height.clear();
    m_nullifiers.clear();
    m_nullifier_connections.clear();
    m_epoch_minted.clear();
    m_withdrawals.clear();
    m_decentralized_withdrawals.clear();
    m_decentralized_withdrawal_sources.clear();
    m_withdrawal_tree = modern::WithdrawalTreeState{};
    m_withdrawal_roots.clear();
    m_light_client_v2.reset();
    m_light_client_connection_v2.reset();
    m_light_client_floor_v2.reset();
    m_anchors_v2.clear();
    m_anchor_by_height_v2.clear();
    m_withdrawals_v2.clear();
    m_withdrawal_v2_sources.clear();
    m_withdrawal_tree_v2 = modern::WithdrawalTreeState{};
    m_withdrawal_v2_roots.clear();
    m_observations_v2 = {};
    m_reserve_v2 = {};
    m_deposits_v2.clear();
    m_deposits_v2_total = 0;
    m_conflict_v2.reset();
    m_reserve_v2_successor = {};
    m_deposits_v2_successor.clear();
    m_deposits_v2_successor_total = 0;
    m_connected_height = -1;
    m_connected_hash.SetNull();
    m_history.clear();
}

bool BridgeStateTracker::ApplyBlock(const CBlock& block,
                                    const CBlockIndex& index,
                                    const Consensus::Params& params)
{
    BridgeBlockDelta delta;
    std::string error;
    // The block was connected once with a full lineage context, so its
    // recorded VERIFIER_E verdicts are replayed as recorded.
    if (!m_index.VerifyBlock(block, index.nHeight, index.GetBlockHash(),
                             params, delta, error, &REPLAY_LINEAGE) ||
        !m_index.ConnectBlock(delta, error)) {
        LogWarning(
            "BridgeStateTracker: block at height %d failed bridge verification (%s); index unavailable",
            index.nHeight, error);
        m_index.Clear();
        m_dirty = true;
        return false;
    }
    return true;
}

bool BridgeStateTracker::Sync(const CChain& chain,
                              const BlockManager& blockman,
                              const Consensus::Params& params,
                              const CBlockIndex& target)
{
    if (Synced(target.GetBlockHash())) return true;
    if (chain[target.nHeight] != &target || !params.busd_bridge ||
        !Consensus::BridgeMintParamsReady(*params.busd_bridge)) {
        return false;
    }
    const int activation{*params.busd_bridge->activation_height};
    if (target.nHeight < activation) {
        m_index.Clear();
        m_synced_tip = target.GetBlockHash();
        m_synced_height = target.nHeight;
        m_dirty = false;
        return true;
    }

    int start{activation};
    if (!m_dirty && m_synced_height >= activation - 1 &&
        m_synced_height <= target.nHeight &&
        chain[m_synced_height] != nullptr &&
        chain[m_synced_height]->GetBlockHash() == m_synced_tip) {
        start = std::max(activation, m_synced_height + 1);
    } else {
        // TODO: production may replace this full replay with a dedicated
        // LevelDB sidecar only when config digest, tip marker, state and
        // per-block undo are committed in one synchronous batch. Never load a
        // partial or mismatched cache.
        m_index.Clear();
        m_dirty = true;
    }
    for (int height{start}; height <= target.nHeight; ++height) {
        const CBlockIndex* index{chain[height]};
        CBlock block;
        if (index == nullptr || !blockman.ReadBlock(block, *index)) {
            LogWarning(
                "BridgeStateTracker: cannot read bridge-active block at height %d; index unavailable",
                height);
            m_index.Clear();
            m_dirty = true;
            return false;
        }
        if (!ApplyBlock(block, *index, params)) return false;
    }
    m_synced_tip = target.GetBlockHash();
    m_synced_height = target.nHeight;
    m_dirty = false;
    return true;
}

void BridgeStateTracker::BlockConnected(const CBlock& block,
                                        const CBlockIndex& index,
                                        const Consensus::Params& params)
{
    if (!params.busd_bridge ||
        !Consensus::BridgeMintParamsReady(*params.busd_bridge)) {
        return;
    }
    if (m_dirty || index.pprev == nullptr ||
        m_synced_tip != index.pprev->GetBlockHash()) {
        m_dirty = true;
        return;
    }
    if (index.nHeight >= *params.busd_bridge->activation_height &&
        !ApplyBlock(block, index, params)) {
        return;
    }
    m_synced_tip = index.GetBlockHash();
    m_synced_height = index.nHeight;
}

void BridgeStateTracker::BlockDisconnected(
    const CBlockIndex& index, const Consensus::Params& params)
{
    if (m_dirty || m_synced_tip != index.GetBlockHash() ||
        index.pprev == nullptr) {
        m_dirty = true;
        return;
    }
    if (params.busd_bridge &&
        Consensus::BridgeMintParamsReady(*params.busd_bridge) &&
        index.nHeight >= *params.busd_bridge->activation_height) {
        std::string error;
        if (!m_index.DisconnectBlock(index.nHeight, index.GetBlockHash(),
                                     error)) {
            LogWarning(
                "BridgeStateTracker: failed to disconnect height %d (%s); index unavailable",
                index.nHeight, error);
            m_index.Clear();
            m_dirty = true;
            return;
        }
    }
    m_synced_tip = index.pprev->GetBlockHash();
    m_synced_height = index.pprev->nHeight;
}

} // namespace node
