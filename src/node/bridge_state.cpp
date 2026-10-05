// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/bridge_state.h>

#include <bridge/admission_v2.h>
#include <bridge/deposit.h>
#include <bridge/exec_chain.h>
#include <bridge/mpt.h>
#include <bridge/proof.h>
#include <bridge/rlp.h>
#include <chain.h>
#include <consensus/era.h>
#include <logging.h>
#include <modern/asset_output.h>
#include <modern/bridge_asset.h>
#include <modern/bridge_binding.h>
#include <modern/bridge_v2.h>
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
        const modern::WithdrawalTreeState& withdrawal_tree_v2)
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
          withdrawal_tree{withdrawal_tree},
          withdrawal_tree_v2{withdrawal_tree_v2}
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
};

bool AddAnchor(const BridgeExecutionAnchor& anchor, ScratchState& state,
               std::vector<BridgeExecutionAnchor>& added,
               std::string& error)
{
    if (anchor.block_hash.IsNull() || anchor.receipts_root.IsNull() ||
        anchor.connected_height < 0 || anchor.connected_block.IsNull() ||
        !HasValidBackfillOrigin(anchor)) {
        error = "bridge execution anchor is malformed";
        return false;
    }
    if (const auto* same_hash{state.Anchor(anchor.block_hash)}) {
        if (same_hash->block_number != anchor.block_number ||
            same_hash->receipts_root != anchor.receipts_root) {
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
                     state, delta.anchors_added, error);
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
                     state, delta.anchors_added, error);
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
        block_hash};
    const size_t before{delta.anchors_added.size()};
    if (!AddAnchor(anchor, state, delta.anchors_added, error)) {
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
 */
bool VerifyMintV2(const CTransaction& tx, const bridge::BridgeMintV1& proof,
                  const int height, const int64_t candidate_time,
                  const Consensus::Params& params, ScratchState& state,
                  BridgeBlockDelta& delta, BridgeTxAuthorization& tx_result,
                  std::string& error)
{
    const Consensus::BridgeV2Params& v2{*params.bridge_v2};
    const auto entry_id{modern::ConfiguredBridgeV2EntryId(params)};
    if (!entry_id || proof.registry_id != *entry_id) {
        error = "bridge mint registry id does not match the V2 vault entry";
        return false;
    }
    const auto event{ProveMintDeposit(
        proof, v2.light_client, v2.vault.origin_deployment_block,
        v2.vault.vault_address, bridge::RECEIPT_MAX_TYPE_V2, candidate_time,
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
        v2, *asset,
        bridge::ProvenBridgeDeposit{v2.vault.origin_chain_id,
                                    proof.target_block_number,
                                    v2.vault.vault_address, *event},
        height, bridge::BridgeMintBudget{state.minted_this_block, epoch_used},
        authorization)};
    if (admitted != bridge::BridgeAdmissionResult::OK) {
        error = "bridge mint deposit fails registry, recipient, amount, or cap admission";
        return false;
    }
    return ConnectMintAuthorization(tx, proof.output_index, authorization,
                                    epoch_key, epoch_used, state, delta,
                                    tx_result, error);
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

/**
 * Kind 6 from V (node changes section 7.2, checks 1 and 4 of this slice):
 * valid only from W2, naming the V2 vault entry id, a non-null Ethereum
 * recipient and the exact bUSD V2 BURN output. The leaf V2 takes the next id
 * of the V2 tree, the V2 entry's chain, vault and token, and this height.
 * The reserve bound (check 2) is implied by asset conservation while one
 * entry exists and nothing is credited by migration; the wind-down bound
 * (check 3) and the full gate (check 4 beyond the caller's temporary
 * lineage gate) need kind 9 observations and come with them.
 */
bool VerifyBridgeBurnV2(
    const CTransaction& tx, const bridge::BridgeBurnV1& record,
    const int height, const uint256& block_hash,
    const Consensus::Params& params, ScratchState& state,
    BridgeBlockDelta& delta, BridgeTxAuthorization& tx_result,
    std::string& error)
{
    if (!params.BridgeV2BurnRulesActive(height)) {
        error = "bridge V2 burns are not active";
        return false;
    }
    const Consensus::BridgeV2VaultEntry& entry{params.bridge_v2->vault};
    const auto entry_id{modern::ConfiguredBridgeV2EntryId(params)};
    const auto asset{modern::ConfiguredBridgeV2AssetId(params)};
    if (!entry_id || !asset || record.registry_id != *entry_id) {
        error = "bridge burn does not name the V2 vault entry";
        return false;
    }
    if (bridge::EthAddressIsNull(record.ethereum_recipient)) {
        error = "bridge V2 burn Ethereum recipient is null";
        return false;
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
    delta.withdrawal_tree_v2_after = state.withdrawal_tree_v2;
    BridgeWithdrawalV2Request request{
        tx.GetHash(), record.burn_output_index, withdrawal, leaf, height,
        block_hash};
    delta.withdrawals_v2_added.push_back(request);
    tx_result.withdrawal_v2 = std::move(request);
    return true;
}

bool VerifyBridgeTransaction(
    const CTransaction& tx, const int height, const int64_t candidate_time,
    const uint256& block_hash, const Consensus::Params& params,
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
                                              block_hash, params, state,
                                              delta, tx_result, error);
                }
                return VerifyBridgeBurn(tx, payload, height, block_hash,
                                        params, state, delta, tx_result,
                                        error);
            }
            error = "bridge record kind is unsupported";
            return false;
        },
        decoded->payload);
}

} // namespace

bool HasDecentralizedBridgeBurn(const CTransaction& tx)
{
    for (const CMpaRecord& record : tx.mpa) {
        if (record.payload_type != bridge::BRIDGE_MPA_TYPE) continue;
        const auto decoded{bridge::DecodeBridgeMpaRecordV1(record)};
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

BridgeBurnReadiness BridgeV2BurnReadiness(
    const LineageV2View& lineage, const Consensus::BridgeV2Params& params,
    std::string& reason)
{
    reason.clear();
    const auto not_ready{[&reason](std::string why) {
        reason = std::move(why);
        return BridgeBurnReadiness::NOT_READY;
    }};
    if (!params.Valid()) {
        return not_ready("bridge V2 parameters are incomplete");
    }
    if (!lineage.configured) {
        return not_ready("fresh finality lineage is not configured");
    }
    if (!lineage.bootstrapped) {
        return not_ready("fresh finality lineage is not bootstrapped");
    }
    if (lineage.broken) {
        return not_ready("fresh finality lineage is broken");
    }
    const Consensus::BridgeV2VerifierPins& verifier{params.verifier};
    const auto authorizes{[&verifier](const uint32_t count,
                                      const uint64_t weight) {
        return count >= verifier.min_bridge_validators &&
               count <= verifier.max_bridge_validators &&
               weight >= verifier.min_bridge_total_weight;
    }};
    if (!authorizes(lineage.current_count, lineage.current_weight)) {
        return not_ready(
            "current fresh-lineage committee cannot authorize a Verifier V2 root");
    }
    if (!authorizes(lineage.next_count, lineage.next_weight)) {
        return not_ready(
            "next fresh-lineage committee cannot authorize a Verifier V2 root");
    }
    return BridgeBurnReadiness::READY;
}

namespace {

std::string BridgeV2BurnReadinessError(const BridgeBurnReadiness readiness)
{
    return readiness == BridgeBurnReadiness::UNAVAILABLE
               ? "bridge V2 burn readiness is unavailable"
               : "bridge V2 burn requires a live fresh finality lineage whose current and next committees can authorize a Verifier V2 root (temporary gate; the kind 9 liveness gate is pending)";
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

BridgeBurnReadiness ProjectedBridgeV2BurnReadiness(
    FinalityTracker& finality, const CChain& chain,
    const BlockManager& blockman, const Consensus::Params& params,
    const CBlockIndex& parent, const BridgeStateIndex& bridge_index,
    std::string& reason)
{
    reason.clear();
    if (!params.bridge_v2) {
        reason = "bridge V2 is not configured";
        return BridgeBurnReadiness::UNAVAILABLE;
    }
    if (!finality.Sync(chain, blockman, params, parent, &bridge_index)) {
        reason = "the fresh finality lineage cannot be reconstructed at the parent block";
        return BridgeBurnReadiness::UNAVAILABLE;
    }
    const int height{parent.nHeight + 1};
    return BridgeV2BurnReadiness(
        MakeLineageV2View(params, finality.Projected(height, params).fresh),
        *params.bridge_v2, reason);
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
         const int connected_height, const uint256& connected_hash)
        : height{candidate_height},
          time{candidate_time},
          block_id{preview_block_id},
          consensus{params},
          state{std::make_unique<ScratchState>(
              light_client, anchors, anchor_by_height, nullifiers,
              epoch_minted, withdrawals, decentralized_withdrawals,
              decentralized_sources, withdrawal_tree, withdrawals_v2,
              withdrawal_v2_sources, withdrawal_tree_v2)}
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
    }

    const int height;
    const int64_t time;
    const uint256 block_id;
    const Consensus::Params& consensus;
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
        if (!VerifyBridgeTransaction(*tx, m_impl->height, m_impl->time,
                                     m_impl->block_id, m_impl->consensus,
                                     *trial_state, trial_delta, ignored,
                                     error)) {
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

CAmount BridgeStateIndex::EpochMinted(const modern::AssetId& asset,
                                      const uint64_t epoch) const
{
    const auto it{m_epoch_minted.find({asset, epoch})};
    return it == m_epoch_minted.end() ? 0 : it->second;
}

std::unique_ptr<BridgeBlockPreview> BridgeStateIndex::BeginBlockPreview(
    const int candidate_height, const int64_t candidate_time,
    const uint256& preview_block_id, const Consensus::Params& params,
    std::string& error) const
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
        m_connected_height, m_connected_hash)};
    return std::unique_ptr<BridgeBlockPreview>{
        new BridgeBlockPreview{std::move(impl)}};
}

bool BridgeStateIndex::VerifyBlock(
    const CBlock& block, const int height, const uint256& block_hash,
    const Consensus::Params& params, BridgeBlockDelta& out,
    std::string& error) const
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
                       m_withdrawal_v2_sources, m_withdrawal_tree_v2};
    for (const CTransactionRef& tx : block.vtx) {
        BridgeTxAuthorization ignored;
        if (!VerifyBridgeTransaction(*tx, height, block.GetBlockTime(),
                                     block_hash, params, state, out, ignored,
                                     error)) {
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
    return true;
}

bool BridgeStateIndex::VerifyTransaction(
    const CTransaction& tx, const int candidate_height,
    const int64_t candidate_time, const Consensus::Params& params,
    BridgeTxAuthorization& out, std::string& error) const
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
                     error)) {
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
    if (!m_index.VerifyBlock(block, index.nHeight, index.GetBlockHash(),
                             params, delta, error) ||
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
