// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_checkpoint_index.h>

#include <chain.h>
#include <consensus/era.h>
#include <flowmesh/bls_certificate.h>
#include <flowmesh/production_engine.h>
#include <logging.h>
#include <modern/asset_output.h>
#include <modern/asset_validation.h>
#include <modern/chain_domain.h>
#include <modern/mpa.h>
#include <node/blockstorage.h>
#include <node/flowmesh_vault_index.h>
#include <node/fn_seat_index.h>
#include <script/script.h>

#include <algorithm>
#include <map>
#include <limits>
#include <span>
#include <utility>

namespace node {
namespace {

const CBlockIndex* CanonicalDeepAnchor(
    const modern::FlowMeshCheckpointAnchorV1& anchor,
    const int candidate_height, const CChain& chain, const char* name,
    std::string& error)
{
    if (anchor.height > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        error = std::string{"FlowMesh checkpoint "} + name +
                " anchor height exceeds int range";
        return nullptr;
    }
    const int anchor_height{static_cast<int>(anchor.height)};
    const CBlockIndex* anchor_index{chain[anchor_height]};
    if (anchor_index == nullptr ||
        anchor_index->GetBlockHash() != anchor.block_hash) {
        error = std::string{"FlowMesh checkpoint "} + name +
                " anchor is not canonical";
        return nullptr;
    }
    if (static_cast<int64_t>(candidate_height) - anchor_height <
        Consensus::FLOWMESH_ANCHOR_DEPTH) {
        error = std::string{"FlowMesh checkpoint "} + name +
                " anchor is not deep enough";
        return nullptr;
    }
    return anchor_index;
}

std::optional<flowmesh::ActiveFnBlsSeatSet> AnchoredSeatSet(
    const modern::FlowMeshCheckpointAnchorV1& anchor,
    const flowmesh::MarketId& market_id, const uint64_t epoch,
    const int candidate_height, const CChain& chain,
    const Consensus::Params& params, const FnSeatIndex& seats,
    std::string& error)
{
    const CBlockIndex* anchor_index{CanonicalDeepAnchor(
        anchor, candidate_height, chain, "seat-set", error)};
    if (anchor_index == nullptr) return std::nullopt;
    const auto snapshot{
        seats.AnchoredSnapshot(chain, *anchor_index, candidate_height, params,
                               error)};
    if (!snapshot) return std::nullopt;
    if (!snapshot->FlowMeshReady()) {
        error = "FlowMesh checkpoint seat set has fewer than four seats";
        return std::nullopt;
    }

    const auto domain{params.legacy_final_hash
                          ? modern::ModernChainDomain(params.hashGenesisBlock,
                                                      *params.legacy_final_hash)
                          : std::nullopt};
    if (!domain) {
        error = "FlowMesh checkpoint chain domain is unavailable";
        return std::nullopt;
    }
    std::vector<flowmesh::BlsSeatBinding> bindings;
    bindings.reserve(snapshot->members.size());
    for (const FnSeatRecord& member : snapshot->members) {
        bindings.push_back(flowmesh::BlsSeatBinding{
            member.outpoint, member.bls_pubkey, member.proof_of_possession});
    }
    flowmesh::BlsSeatSetCheck seat_check{flowmesh::BlsSeatSetCheck::BAD_SET_HASH};
    auto active{flowmesh::BuildActiveFnBlsSeatSet(
        *domain, market_id, epoch, anchor.height, anchor.block_hash, bindings,
        seat_check)};
    if (!active) {
        error = std::string{"FlowMesh checkpoint seat set is invalid: "} +
                flowmesh::BlsSeatSetCheckName(seat_check);
        return std::nullopt;
    }
    return active;
}

bool SameAnchor(const modern::FlowMeshCheckpointAnchorV1& a,
                const modern::FlowMeshCheckpointAnchorV1& b)
{
    return a.height == b.height && a.block_hash == b.block_hash;
}

std::optional<FnSeatSnapshot> LatestSignableSeatSnapshot(
    const int candidate_height, const CChain& chain,
    const Consensus::Params& params, const FnSeatIndex& seats,
    std::string& error)
{
    // A certificate is assembled against the parent tip and then published in
    // this candidate. Keep that one-block publication boundary while refusing
    // any older committee view.
    const int64_t anchor_height{
        static_cast<int64_t>(candidate_height) -
        Consensus::FLOWMESH_ANCHOR_DEPTH - 1};
    if (anchor_height < 0 ||
        anchor_height > std::numeric_limits<int>::max()) {
        error = "FlowMesh checkpoint latest signable anchor is unavailable";
        return std::nullopt;
    }
    const CBlockIndex* anchor{chain[static_cast<int>(anchor_height)]};
    if (anchor == nullptr) {
        error = "FlowMesh checkpoint latest signable anchor is not canonical";
        return std::nullopt;
    }
    auto snapshot{seats.AnchoredSnapshot(chain, *anchor, candidate_height,
                                         params, error)};
    if (!snapshot) return std::nullopt;
    if (!snapshot->FlowMeshReady()) {
        error = "FlowMesh checkpoint latest seat set has fewer than four seats";
        return std::nullopt;
    }
    return snapshot;
}

bool SameSeatMembership(const flowmesh::ActiveFnBlsSeatSet& active,
                        const FnSeatSnapshot& snapshot)
{
    if (active.members.size() != snapshot.members.size()) return false;
    for (size_t i{0}; i < active.members.size(); ++i) {
        const auto& certified{active.members[i]};
        const auto& current{snapshot.members[i]};
        if (certified.seat_id != current.seat_id ||
            certified.outpoint != current.outpoint ||
            certified.key.Key().Compressed() != current.bls_pubkey) {
            return false;
        }
    }
    return true;
}

class CheckpointChainAnchorPolicy final : public flowmesh::AnchorPolicy
{
public:
    explicit CheckpointChainAnchorPolicy(const CChain& chain) : m_chain{chain} {}

    bool Acceptable(const flowmesh::AnchorRef& anchor) const override
    {
        return StillCanonical(anchor);
    }

    bool StillCanonical(const flowmesh::AnchorRef& anchor) const override
    {
        if (anchor.height < 0) return false;
        const CBlockIndex* index{m_chain[anchor.height]};
        return index != nullptr && index->GetBlockHash() == anchor.hash;
    }

    flowmesh::AnchorRef Current() const override
    {
        const CBlockIndex* tip{m_chain.Tip()};
        return tip == nullptr
                   ? flowmesh::AnchorRef{}
                   : flowmesh::AnchorRef{tip->nHeight, tip->GetBlockHash()};
    }

private:
    const CChain& m_chain;
};

bool CheckDeterministicGenesis(
    const modern::FlowMeshCheckpointCoreV1& core,
    const FlowMeshMarketRecord& market,
    const flowmesh::ActiveFnBlsSeatSet& active,
    const int candidate_height, const CChain& chain,
    const Consensus::Params& params, std::string& error)
{
    if (!params.modern_pos || params.modern_pos->treasury_script.empty() ||
        candidate_height < 0 ||
        candidate_height > std::numeric_limits<int32_t>::max() ||
        core.production_anchor.height >
            static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        error = "FlowMesh deterministic genesis parameters are unavailable";
        return false;
    }
    const CScript treasury_script{
        params.modern_pos->treasury_script.begin(),
        params.modern_pos->treasury_script.end()};
    const uint256 treasury_owner_commitment{
        modern::AssetOwnerCommitment(treasury_script)};
    if (treasury_owner_commitment.IsNull()) {
        error = "FlowMesh deterministic genesis treasury is invalid";
        return false;
    }

    const flowmesh::FlowMeshState initial_state{
        market.vault_id, market.base_asset, modern::NativeAsset(),
        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    const flowmesh::ProductionEpochGate epoch_gate{
        core.domain, core.market_id, active};
    const flowmesh::AnchorRef production_anchor{
        static_cast<int32_t>(core.production_anchor.height),
        core.production_anchor.block_hash};
    const CheckpointChainAnchorPolicy anchor_policy{chain};
    const flowmesh::ProductionAnchorContext anchor_context{
        static_cast<int32_t>(candidate_height), std::nullopt, &anchor_policy};
    const std::vector<flowmesh::Action> no_actions;
    flowmesh::ProductionEntryCheck check;
    const auto expected{flowmesh::BuildProductionExecutionEntry(
        initial_state, core.domain, core.market_id, active, epoch_gate,
        /*sequence=*/0, /*effect_start=*/0, uint256{}, production_anchor,
        anchor_context, treasury_owner_commitment, no_actions,
        /*deposits=*/nullptr, check)};
    const auto actual_commitment{
        modern::FlowMeshCheckpointProductionCommitmentV1(core)};
    const auto expected_commitment{
        expected ? expected->entry.Commitment() : std::nullopt};
    if (!expected || !actual_commitment || !expected_commitment ||
        *actual_commitment != *expected_commitment ||
        core.microblock_hash != expected->entry.GetHash()) {
        error = "first FlowMesh checkpoint is not the deterministic empty genesis";
        return false;
    }
    return true;
}

bool CheckCheckpointTransition(
    const modern::FlowMeshCheckpointCoreV1& core,
    const std::optional<FlowMeshConnectedCheckpoint>& head,
    std::string& error)
{
    if (!head) {
        if (!core.previous_checkpoint_id.IsNull()) {
            error = "first FlowMesh checkpoint does not name the null head";
            return false;
        }
        if (core.kind != modern::FlowMeshCheckpointKind::EXECUTION) {
            error = "first FlowMesh checkpoint must be an execution checkpoint";
            return false;
        }
        if (core.epoch != 0) {
            error = "first FlowMesh checkpoint must use epoch zero";
            return false;
        }
        if (core.sequence != 0) {
            error = "first FlowMesh checkpoint must use sequence zero";
            return false;
        }
        if (!SameAnchor(core.anchor, core.production_anchor)) {
            error = "first FlowMesh checkpoint must use one seat/production anchor";
            return false;
        }
        if (core.effect_start != 0) {
            error = "first FlowMesh checkpoint effect range does not start at zero";
            return false;
        }
        return true;
    }

    if (core.previous_checkpoint_id != head->checkpoint_id) {
        error = "FlowMesh checkpoint does not extend the current market head";
        return false;
    }
    if (core.sequence <= head->core.sequence) {
        error = "FlowMesh checkpoint sequence is not strictly increasing";
        return false;
    }
    const uint64_t expected_effect_start{
        head->core.effect_start + head->core.effect_count};
    if (core.effect_start != expected_effect_start) {
        error = "FlowMesh checkpoint effect range is not consecutive";
        return false;
    }
    if (core.production_anchor.height < head->core.production_anchor.height ||
        (core.production_anchor.height ==
             head->core.production_anchor.height &&
         core.production_anchor.block_hash !=
             head->core.production_anchor.block_hash)) {
        error = "FlowMesh checkpoint production anchor is not monotonic";
        return false;
    }

    if (head->core.kind == modern::FlowMeshCheckpointKind::EPOCH_HANDOFF) {
        const auto& handoff{*head->core.handoff};
        if (core.kind != modern::FlowMeshCheckpointKind::EXECUTION ||
            core.epoch != handoff.next_epoch ||
            !SameAnchor(core.anchor, handoff.next_anchor) ||
            core.seat_set_hash != handoff.next_seat_set_hash) {
            error = "FlowMesh execution does not activate the connected handoff";
            return false;
        }
        return true;
    }

    if (core.epoch != head->core.epoch ||
        !SameAnchor(core.anchor, head->core.anchor) ||
        core.seat_set_hash != head->core.seat_set_hash) {
        error = "FlowMesh epoch/anchor/set changed without a connected handoff";
        return false;
    }
    return true;
}

} // namespace

std::optional<FlowMeshEffectNullifier> FlowMeshNullifierForProof(
    const modern::FlowMeshVaultProofV1& proof)
{
    if (!modern::FlowMeshVaultProofEffectKindMatches(proof)) return std::nullopt;
    if (const auto* deposit{
            std::get_if<modern::FlowMeshDepositAcceptanceV1>(&proof.effect)}) {
        if (deposit->acceptance_id.IsNull()) return std::nullopt;
        return FlowMeshEffectNullifier{
            FlowMeshNullifierKind::DEPOSIT_ACCEPTANCE,
            deposit->acceptance_id};
    }
    const auto* receipt{
        std::get_if<modern::FlowMeshWithdrawalReceiptV1>(&proof.effect)};
    if (receipt == nullptr || receipt->receipt_id.IsNull()) return std::nullopt;
    return FlowMeshEffectNullifier{FlowMeshNullifierKind::WITHDRAWAL_RECEIPT,
                                   receipt->receipt_id};
}

std::optional<FlowMeshConnectedCheckpoint> FlowMeshCheckpointIndex::Get(
    const modern::FlowMeshCheckpointId& checkpoint_id) const
{
    const auto it{m_checkpoints.find(checkpoint_id)};
    return it == m_checkpoints.end()
               ? std::nullopt
               : std::optional<FlowMeshConnectedCheckpoint>{it->second};
}

std::optional<FlowMeshConnectedCheckpoint> FlowMeshCheckpointIndex::Head(
    const flowmesh::MarketId& market_id) const
{
    const auto head{m_heads.find(market_id)};
    if (head == m_heads.end()) return std::nullopt;
    return Get(head->second);
}

bool FlowMeshCheckpointIndex::IsNullified(
    const FlowMeshEffectNullifier& nullifier) const
{
    return m_nullifiers.contains(nullifier);
}

bool FlowMeshCheckpointIndex::VerifyVaultProof(
    const modern::FlowMeshVaultProofV1& proof, std::string& error) const
{
    const auto checkpoint{m_checkpoints.find(proof.checkpoint_id)};
    if (checkpoint == m_checkpoints.end()) {
        error = "FlowMesh vault proof checkpoint is not connected";
        return false;
    }
    if (!modern::VerifyFlowMeshVaultProofV1(proof, checkpoint->second.core)) {
        error = "FlowMesh vault proof is not included in its checkpoint";
        return false;
    }
    const auto nullifier{FlowMeshNullifierForProof(proof)};
    if (!nullifier || m_nullifiers.contains(*nullifier)) {
        error = "FlowMesh vault effect is already nullified";
        return false;
    }
    return true;
}

bool FlowMeshCheckpointIndex::WithdrawalSettlementsBetween(
    const flowmesh::MarketId& market_id,
    const CBlockIndex& after_exclusive,
    const CBlockIndex& through_inclusive,
    std::vector<flowmesh::WithdrawalSettlementFactV1>& out,
    std::string& error) const
{
    out.clear();
    if (market_id.IsNull() || after_exclusive.nHeight < 0 ||
        through_inclusive.nHeight <= after_exclusive.nHeight ||
        through_inclusive.GetAncestor(after_exclusive.nHeight) !=
            &after_exclusive) {
        error = "FlowMesh settlement interval is not one forward chain range";
        return false;
    }
    const auto indexed_anchor = [&](const CBlockIndex& index) {
        const auto it{std::find_if(
            m_history.begin(), m_history.end(), [&](const auto& delta) {
                return delta.height == index.nHeight;
            })};
        return it != m_history.end() &&
               it->block_hash == index.GetBlockHash();
    };
    // The complete checkpoint history starts at A3. A caller scanning a
    // mixed A2/A3 interval clamps its known-empty pre-A3 prefix to the exact
    // A3-1 predecessor. That predecessor has no checkpoint delta by design,
    // but it is a valid exclusive boundary for scanning every retained A3+
    // delta. Any wider/missing interior boundary still fails closed.
    const bool retained_history_predecessor{
        !m_history.empty() &&
        after_exclusive.nHeight + 1 == m_history.front().height &&
        through_inclusive.GetAncestor(m_history.front().height) != nullptr &&
        through_inclusive.GetAncestor(m_history.front().height)
                ->GetBlockHash() == m_history.front().block_hash};
    if ((!indexed_anchor(after_exclusive) &&
         !retained_history_predecessor) ||
        !indexed_anchor(through_inclusive)) {
        error = "FlowMesh settlement interval is outside retained indexed history";
        return false;
    }

    for (const FlowMeshCheckpointBlockDelta& delta : m_history) {
        if (delta.height <= after_exclusive.nHeight) continue;
        if (delta.height > through_inclusive.nHeight) break;
        const CBlockIndex* canonical{
            through_inclusive.GetAncestor(delta.height)};
        if (canonical == nullptr ||
            canonical->GetBlockHash() != delta.block_hash) {
            out.clear();
            error = "FlowMesh settlement interval does not match indexed history";
            return false;
        }
        for (const auto& fact : delta.withdrawal_settlements) {
            if (!flowmesh::WithdrawalSettlementFactIsCanonical(fact) ||
                fact.connected_height != delta.height ||
                fact.connected_block != delta.block_hash) {
                out.clear();
                error = "FlowMesh settlement history contains a malformed fact";
                return false;
            }
            if (fact.receipt.market_id == market_id) {
                if (out.size() ==
                    flowmesh::FLOWMESH_MAX_WITHDRAWAL_SETTLEMENTS_PER_ENTRY) {
                    out.clear();
                    error = "FlowMesh settlement interval exceeds the per-entry bound";
                    return false;
                }
                out.push_back(fact);
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.receipt.receipt_id < b.receipt.receipt_id;
    });
    if (std::adjacent_find(out.begin(), out.end(), [](const auto& a,
                                                       const auto& b) {
            return a.receipt.receipt_id == b.receipt.receipt_id;
        }) != out.end()) {
        out.clear();
        error = "FlowMesh settlement interval contains a duplicate receipt";
        return false;
    }
    return true;
}

bool FlowMeshCheckpointIndex::WithdrawalSettlementCatchupHeight(
    const flowmesh::MarketId& market_id,
    const CBlockIndex& after_exclusive,
    const CBlockIndex& through_inclusive, const size_t max_count,
    int& selected_height, size_t& selected_count,
    std::string& error) const
{
    selected_height = -1;
    selected_count = 0;
    if (market_id.IsNull() || max_count == 0 ||
        after_exclusive.nHeight < 0 ||
        through_inclusive.nHeight <= after_exclusive.nHeight ||
        through_inclusive.GetAncestor(after_exclusive.nHeight) !=
            &after_exclusive) {
        error = "FlowMesh settlement catch-up interval is not one forward chain range";
        return false;
    }
    const auto indexed_anchor = [&](const CBlockIndex& index) {
        const auto it{std::find_if(
            m_history.begin(), m_history.end(), [&](const auto& delta) {
                return delta.height == index.nHeight;
            })};
        return it != m_history.end() &&
               it->block_hash == index.GetBlockHash();
    };
    const bool retained_history_predecessor{
        !m_history.empty() &&
        after_exclusive.nHeight + 1 == m_history.front().height &&
        through_inclusive.GetAncestor(m_history.front().height) != nullptr &&
        through_inclusive.GetAncestor(m_history.front().height)
                ->GetBlockHash() == m_history.front().block_hash};
    if ((!indexed_anchor(after_exclusive) &&
         !retained_history_predecessor) ||
        !indexed_anchor(through_inclusive)) {
        error = "FlowMesh settlement catch-up interval is outside retained indexed history";
        return false;
    }

    for (const FlowMeshCheckpointBlockDelta& delta : m_history) {
        if (delta.height <= after_exclusive.nHeight) continue;
        if (delta.height > through_inclusive.nHeight) break;
        const CBlockIndex* canonical{
            through_inclusive.GetAncestor(delta.height)};
        if (canonical == nullptr ||
            canonical->GetBlockHash() != delta.block_hash) {
            error = "FlowMesh settlement catch-up interval does not match indexed history";
            return false;
        }

        size_t block_count{0};
        for (const auto& fact : delta.withdrawal_settlements) {
            if (!flowmesh::WithdrawalSettlementFactIsCanonical(fact) ||
                fact.connected_height != delta.height ||
                fact.connected_block != delta.block_hash) {
                error = "FlowMesh settlement history contains a malformed fact";
                return false;
            }
            if (fact.receipt.market_id == market_id) ++block_count;
        }
        if (block_count > max_count) {
            error = "one B3 block exceeds the FlowMesh settlement per-entry bound";
            return false;
        }
        if (selected_count > max_count - block_count) {
            // B3 blocks are indivisible settlement boundaries. Stop at the
            // exact predecessor of the first block that would exceed the
            // entry bound; empty blocks are retained, so this always names a
            // canonical indexed anchor strictly after `after_exclusive`.
            selected_height = delta.height - 1;
            if (selected_height <= after_exclusive.nHeight) {
                error = "FlowMesh settlement catch-up cannot advance safely";
                return false;
            }
            return true;
        }
        selected_count += block_count;
    }
    selected_height = through_inclusive.nHeight;
    return true;
}

bool CheckFlowMeshVaultTransaction(
    const CTransaction& tx, const std::vector<Coin>& prev_coins,
    const CAmount tx_fee, const int height, const Consensus::Params& params,
    const FlowMeshCheckpointIndex& checkpoints,
    FlowMeshVaultAuthorization& authorization, std::string& error)
{
    authorization = FlowMeshVaultAuthorization{};
    if (tx.IsCoinBase() || prev_coins.size() != tx.vin.size() || tx_fee < 0) {
        error = "FlowMesh vault authorization has an invalid transaction context";
        return false;
    }

    const CMpaRecord* proof_record{nullptr};
    for (const CMpaRecord& record : tx.mpa) {
        if (record.payload_type != modern::MPA_TYPE_FLOWMESH_VAULT_PROOF) {
            continue;
        }
        if (proof_record != nullptr) {
            error = "FlowMesh vault transaction has multiple type-9 proofs";
            return false;
        }
        proof_record = &record;
    }

    struct VaultCoin {
        size_t input_index{0};
        modern::ModernOutput output;
        modern::VaultParams params;
    };
    std::vector<VaultCoin> vault_inputs;
    __int128 owner_native_inputs{0};
    bool has_non_native_fee_input{false};
    for (size_t i{0}; i < tx.vin.size(); ++i) {
        std::string view_error;
        const auto view{modern::ViewAssetAwareCoin(prev_coins[i], height,
                                                   params, view_error)};
        if (!view) {
            error = "FlowMesh vault input " + std::to_string(i) + ": " +
                    view_error;
            return false;
        }
        if (view->policy_type ==
            static_cast<uint16_t>(modern::PolicyType::DEX_VAULT)) {
            const auto vault_params{modern::ParseVaultParams(view->policy_params)};
            if (!vault_params ||
                !modern::CheckVaultParams(view->policy_commitment,
                                          view->policy_params)) {
                error = "FlowMesh vault input has malformed policy params";
                return false;
            }
            vault_inputs.push_back(VaultCoin{i, *view, *vault_params});
            continue;
        }
        // A vault operation is deliberately narrow: every non-vault input is
        // an ordinarily authorized native-B3 coin funding the miner fee.
        if (view->asset != modern::NativeAsset() ||
            (view->policy_type !=
                 static_cast<uint16_t>(modern::PolicyType::OWNER) &&
             view->policy_type !=
                 static_cast<uint16_t>(modern::PolicyType::LEGACY_LOCK))) {
            has_non_native_fee_input = true;
            continue;
        }
        owner_native_inputs += view->amount;
    }

    if (vault_inputs.empty()) {
        if (proof_record != nullptr) {
            error = "FlowMesh type-9 proof does not authorize a vault input";
            return false;
        }
        return true;
    }
    if (proof_record == nullptr) {
        error = "DEX_VAULT input requires one matching type-9 proof";
        return false;
    }
    if (has_non_native_fee_input) {
        error = "FlowMesh vault transaction has a non-native fee input";
        return false;
    }
    // No unrelated MPA action may be bundled into a keyless custody spend.
    // This keeps fee/disintegration accounting and the one-effect operation
    // independently auditable.
    if (tx.mpa.size() != 1) {
        error = "FlowMesh vault transaction must contain only its type-9 proof";
        return false;
    }
    const auto proof{modern::DecodeFlowMeshVaultProofV1(proof_record->payload)};
    if (!proof || !checkpoints.VerifyVaultProof(*proof, error)) return false;
    const auto nullifier{FlowMeshNullifierForProof(*proof)};
    if (!nullifier) {
        error = "FlowMesh vault proof has no canonical nullifier";
        return false;
    }
    authorization.nullifier = *nullifier;
    authorization.authorized_inputs.assign(tx.vin.size(), false);

    for (const VaultCoin& input : vault_inputs) {
        if (!tx.vin[input.input_index].scriptSig.empty() ||
            !tx.vin[input.input_index].scriptWitness.IsNull()) {
            error = "DEX_VAULT input scriptSig and witness must be empty";
            return false;
        }
        authorization.authorized_inputs[input.input_index] = true;
    }
    if (owner_native_inputs <= 0) {
        error = "FlowMesh vault miner fee requires a separate native owner input";
        return false;
    }

    struct ViewedOutput {
        size_t index{0};
        modern::ModernOutput output;
        std::optional<modern::VaultParams> vault_params;
    };
    std::vector<ViewedOutput> outputs;
    outputs.reserve(tx.vout.size());
    for (size_t i{0}; i < tx.vout.size(); ++i) {
        std::string view_error;
        const auto view{modern::ViewAssetAwareOutput(tx.vout[i], height,
                                                     params, view_error)};
        if (!view) {
            error = "FlowMesh vault output " + std::to_string(i) + ": " +
                    view_error;
            return false;
        }
        std::optional<modern::VaultParams> vault_params;
        if (view->policy_type ==
            static_cast<uint16_t>(modern::PolicyType::DEX_VAULT)) {
            vault_params = modern::ParseVaultParams(view->policy_params);
            if (!vault_params ||
                !modern::CheckVaultParams(view->policy_commitment,
                                          view->policy_params)) {
                error = "FlowMesh vault output has malformed policy params";
                return false;
            }
        }
        outputs.push_back(ViewedOutput{i, *view, vault_params});
    }

    __int128 owner_native_outputs{0};
    if (proof->kind == modern::FlowMeshVaultProofKind::DEPOSIT_SWEEP) {
        const auto* effect{
            std::get_if<modern::FlowMeshDepositAcceptanceV1>(&proof->effect)};
        if (effect == nullptr || vault_inputs.size() != 1) {
            error = "FlowMesh deposit sweep must spend exactly one vault input";
            return false;
        }
        const VaultCoin& input{vault_inputs.front()};
        if (tx.vin[input.input_index].prevout != effect->deposit_outpoint ||
            input.params.kind != modern::VAULT_KIND_USER_DEPOSIT ||
            !input.params.account || *input.params.account != effect->account ||
            input.output.asset != effect->asset ||
            input.output.amount != effect->amount ||
            input.output.policy_commitment != effect->vault_id ||
            input.params.shard != effect->shard) {
            error = "FlowMesh deposit sweep input does not match its acceptance";
            return false;
        }

        size_t matching_pool_outputs{0};
        for (const ViewedOutput& output : outputs) {
            if (output.vault_params) {
                if (output.vault_params->kind !=
                        modern::VAULT_KIND_POOL_CHANGE ||
                    output.output.policy_commitment != effect->vault_id ||
                    output.vault_params->shard != effect->shard ||
                    output.output.asset != effect->asset ||
                    output.output.amount != effect->amount) {
                    error = "FlowMesh deposit sweep does not recreate the exact pool output";
                    return false;
                }
                ++matching_pool_outputs;
                continue;
            }
            if (output.output.asset != modern::NativeAsset() ||
                output.output.policy_type !=
                    static_cast<uint16_t>(modern::PolicyType::OWNER)) {
                error = "FlowMesh deposit sweep has a non-native payout";
                return false;
            }
            owner_native_outputs += output.output.amount;
        }
        if (matching_pool_outputs != 1) {
            error = "FlowMesh deposit sweep requires exactly one pool output";
            return false;
        }
    } else if (proof->kind == modern::FlowMeshVaultProofKind::WITHDRAWAL) {
        const auto* receipt{
            std::get_if<modern::FlowMeshWithdrawalReceiptV1>(&proof->effect)};
        if (receipt == nullptr || vault_inputs.empty() ||
            vault_inputs.size() >
                flowmesh::FLOWMESH_MAX_WITHDRAWAL_VAULT_INPUTS) {
            error = "FlowMesh withdrawal must spend between one and 64 vault inputs";
            return false;
        }
        std::map<modern::AssetId, __int128> vault_in;
        for (const VaultCoin& input : vault_inputs) {
            if (input.params.kind != modern::VAULT_KIND_POOL_CHANGE ||
                input.params.account ||
                input.output.policy_commitment != receipt->vault_id) {
                error = "FlowMesh withdrawal input is not matching pool change";
                return false;
            }
            vault_in[input.output.asset] += input.output.amount;
        }

        std::map<modern::AssetId, __int128> vault_change;
        std::set<modern::AssetId> change_assets;
        size_t payout_count{0};
        for (const ViewedOutput& output : outputs) {
            if (output.vault_params) {
                if (output.vault_params->kind !=
                        modern::VAULT_KIND_POOL_CHANGE ||
                    output.vault_params->account ||
                    output.output.policy_commitment != receipt->vault_id ||
                    output.vault_params->shard !=
                        receipt->deterministic_change_shard ||
                    !change_assets.insert(output.output.asset).second) {
                    error = "FlowMesh withdrawal has invalid or duplicate vault change";
                    return false;
                }
                vault_change[output.output.asset] += output.output.amount;
                continue;
            }

            // Colored B3A1 outputs and ordinary post-H native outputs both
            // project as OWNER and commit to the exact destination script
            // hash. LEGACY_LOCK is reserved for spent pre-H coins.
            const uint16_t payout_policy{
                static_cast<uint16_t>(modern::PolicyType::OWNER)};
            const bool payout{
                output.output.asset == receipt->asset &&
                output.output.policy_type == payout_policy &&
                output.output.policy_commitment ==
                    receipt->destination_owner_commitment};
            if (payout) {
                if (++payout_count != 1 ||
                    output.output.amount != receipt->amount) {
                    error = "FlowMesh withdrawal payout is not one exact OWNER output";
                    return false;
                }
                // A native payout is funded by the vault and is therefore
                // excluded from owner-funded native fee/change accounting.
                continue;
            }
            if (output.output.asset != modern::NativeAsset() ||
                output.output.policy_type !=
                    static_cast<uint16_t>(modern::PolicyType::OWNER)) {
                error = "FlowMesh withdrawal redirects vault value";
                return false;
            }
            owner_native_outputs += output.output.amount;
        }
        if (payout_count != 1) {
            error = "FlowMesh withdrawal is missing its committed OWNER payout";
            return false;
        }

        for (const auto& [asset, amount_in] : vault_in) {
            const __int128 payout{
                asset == receipt->asset ? static_cast<__int128>(receipt->amount)
                                        : 0};
            const __int128 change{vault_change[asset]};
            if (amount_in != payout + change) {
                error = "FlowMesh withdrawal vault inputs do not equal payout plus change";
                return false;
            }
        }
        for (const auto& [asset, change] : vault_change) {
            if (!vault_in.contains(asset) || change <= 0) {
                error = "FlowMesh withdrawal creates unmatched vault change";
                return false;
            }
        }
        if (!vault_in.contains(receipt->asset)) {
            error = "FlowMesh withdrawal has no input for the receipt asset";
            return false;
        }
    } else {
        error = "unknown FlowMesh vault proof kind";
        return false;
    }

    if (owner_native_inputs < owner_native_outputs ||
        owner_native_inputs - owner_native_outputs !=
            static_cast<__int128>(tx_fee)) {
        error = "FlowMesh vault miner fee is not funded only by owner-native value";
        return false;
    }
    return true;
}

bool FlowMeshCheckpointIndex::VerifyRecords(
    const CTransaction& tx, const int candidate_height,
    const uint256& candidate_block, const CChain& chain,
    const Consensus::Params& params, const FnSeatIndex& seats,
    const FlowMeshVaultIndex& vaults,
    std::map<modern::FlowMeshCheckpointId, FlowMeshConnectedCheckpoint>&
        checkpoints,
    std::map<flowmesh::MarketId, modern::FlowMeshCheckpointId>& heads,
    std::set<FlowMeshEffectNullifier>& nullifiers,
    FlowMeshCheckpointBlockDelta& delta, std::string& error) const
{
    const auto domain{params.legacy_final_hash
                          ? modern::ModernChainDomain(params.hashGenesisBlock,
                                                      *params.legacy_final_hash)
                          : std::nullopt};
    if (!domain) {
        error = "FlowMesh checkpoint chain domain is unavailable";
        return false;
    }

    for (const CMpaRecord& mpa : tx.mpa) {
        if (mpa.payload_type == modern::MPA_TYPE_FLOWMESH_CHECKPOINT) {
            const auto envelope{
                modern::DecodeFlowMeshCheckpointEnvelopeV1(mpa.payload)};
            if (!envelope) {
                error = "malformed FlowMesh checkpoint envelope";
                return false;
            }
            const auto& core{envelope->core};
            if (core.domain != *domain) {
                error = "FlowMesh checkpoint names the wrong chain domain";
                return false;
            }
            const CBlockIndex* production_anchor{CanonicalDeepAnchor(
                core.production_anchor, candidate_height, chain,
                "production", error)};
            if (production_anchor == nullptr) {
                return false;
            }
            const auto market{vaults.MarketAt(core.market_id,
                                              *production_anchor)};
            if (!market) {
                error = "FlowMesh checkpoint market is not registered at its production anchor";
                return false;
            }
            const auto expected_market{flowmesh::ComputeFlowMeshMarketId(
                *domain, market->base_asset)};
            const auto expected_vault{
                expected_market
                    ? flowmesh::ComputeFlowMeshVaultId(*domain,
                                                       *expected_market)
                    : std::nullopt};
            if (!expected_market || !expected_vault ||
                *expected_market != core.market_id ||
                market->market_id != core.market_id ||
                market->vault_id != *expected_vault) {
                error = "FlowMesh checkpoint market registry identity is invalid";
                return false;
            }

            const auto head_it{heads.find(core.market_id)};
            std::optional<FlowMeshConnectedCheckpoint> head;
            if (head_it != heads.end()) {
                const auto existing{checkpoints.find(head_it->second)};
                if (existing == checkpoints.end()) {
                    error = "FlowMesh checkpoint head index is inconsistent";
                    return false;
                }
                head = existing->second;
            }
            if (!head) {
                const auto bootstrap{
                    seats.EarliestFlowMeshReadySnapshot(
                        chain, market->created_height, candidate_height,
                        params, error)};
                if (!bootstrap) return false;
                if (core.anchor.height !=
                        static_cast<uint64_t>(bootstrap->anchor_height) ||
                    core.anchor.block_hash != bootstrap->anchor_hash) {
                    error = "first FlowMesh checkpoint does not use the unique bootstrap anchor";
                    return false;
                }
            }

            auto active{AnchoredSeatSet(core.anchor, core.market_id, core.epoch,
                                        candidate_height, chain, params, seats,
                                        error)};
            if (!active) return false;
            if (core.seat_set_hash != active->set_hash) {
                error = "FlowMesh checkpoint names the wrong anchored seat set";
                return false;
            }
            const auto record{modern::DecodeFlowMeshCheckpointRecordV1(
                mpa.payload, active->Size())};
            if (!record) {
                error = "FlowMesh checkpoint has a non-canonical signer bitmap";
                return false;
            }

            if (!CheckCheckpointTransition(core, head, error)) {
                return false;
            }
            if (!head &&
                !CheckDeterministicGenesis(core, *market, *active,
                                           candidate_height, chain, params,
                                           error)) {
                return false;
            }
            // Once a market has a connected head, that checkpoint chain is
            // authoritative until it publishes an exact epoch handoff. A
            // newly signable membership must not strand execution entries
            // that the connected committee certified before observing the
            // rotation. CheckCheckpointTransition above still requires every
            // such execution to extend the same epoch, anchor, and set.

            if (core.kind == modern::FlowMeshCheckpointKind::EPOCH_HANDOFF) {
                const auto& handoff{*core.handoff};
                const auto latest{LatestSignableSeatSnapshot(
                    candidate_height, chain, params, seats, error)};
                if (!latest) return false;
                // Publication may be delayed by unrelated B3 blocks. Compare
                // membership rather than exact anchor height after both
                // named anchors were proved canonical and deep.
                const bool active_is_latest{
                    SameSeatMembership(*active, *latest)};
                if (active_is_latest) {
                    error = "FlowMesh handoff does not change the active seat membership";
                    return false;
                }
                auto next{AnchoredSeatSet(handoff.next_anchor, core.market_id,
                                          handoff.next_epoch, candidate_height,
                                          chain, params, seats, error)};
                if (!next) return false;
                if (handoff.next_seat_set_hash != next->set_hash) {
                    error = "FlowMesh handoff names the wrong next anchored seat set";
                    return false;
                }
                if (!SameSeatMembership(*next, *latest)) {
                    error = "FlowMesh handoff names obsolete next seat membership";
                    return false;
                }
            }

            const auto cert_check{flowmesh::CheckBlsMicroblockCertificate(
                record->certificate,
                modern::FlowMeshCheckpointBlsContextV1(core), *active)};
            if (cert_check != flowmesh::BlsCertificateCheck::OK) {
                error = std::string{"FlowMesh checkpoint certificate is invalid: "} +
                        flowmesh::BlsCertificateCheckName(cert_check);
                return false;
            }
            const auto checkpoint_id{modern::FlowMeshCheckpointIdV1(core)};
            if (!checkpoint_id || checkpoints.contains(*checkpoint_id)) {
                error = "duplicate or invalid FlowMesh checkpoint id";
                return false;
            }
            FlowMeshConnectedCheckpoint connected{
                *checkpoint_id, core, candidate_height, candidate_block};
            checkpoints.emplace(*checkpoint_id, connected);
            heads.insert_or_assign(core.market_id, *checkpoint_id);
            delta.checkpoints.push_back(std::move(connected));
            continue;
        }

        if (mpa.payload_type != modern::MPA_TYPE_FLOWMESH_VAULT_PROOF) {
            continue;
        }
        if (tx.IsCoinBase()) {
            // A proof is an authorization for one exact vault spend. Letting
            // coinbase carry it would consume the one-time effect without
            // executing that operation or passing the UTXO conservation
            // checker below.
            error = "FlowMesh vault proof is not allowed in coinbase";
            return false;
        }
        const auto proof{modern::DecodeFlowMeshVaultProofV1(mpa.payload)};
        if (!proof) {
            error = "malformed FlowMesh vault proof";
            return false;
        }
        // A type-9 operation may consume only a checkpoint already connected
        // below this candidate block. A sibling type-8 record is atomic with,
        // not prior to, this block and therefore cannot authorize the spend.
        const auto checkpoint{m_checkpoints.find(proof->checkpoint_id)};
        if (checkpoint == m_checkpoints.end()) {
            error = "FlowMesh vault proof checkpoint is not connected";
            return false;
        }
        if (!modern::VerifyFlowMeshVaultProofV1(*proof,
                                                checkpoint->second.core)) {
            error = "FlowMesh vault proof is not included in its checkpoint";
            return false;
        }
        const auto nullifier{FlowMeshNullifierForProof(*proof)};
        if (!nullifier || !nullifiers.insert(*nullifier).second) {
            error = "FlowMesh vault effect is already nullified";
            return false;
        }
        delta.nullifiers.push_back(*nullifier);
        if (!candidate_block.IsNull()) {
            if (const auto* receipt{
                    std::get_if<modern::FlowMeshWithdrawalReceiptV1>(
                        &proof->effect)}) {
                flowmesh::WithdrawalSettlementFactV1 fact{
                    *receipt, proof->checkpoint_id, tx.GetHash(),
                    candidate_height, candidate_block};
                if (!flowmesh::WithdrawalSettlementFactIsCanonical(fact)) {
                    error = "FlowMesh withdrawal settlement fact is malformed";
                    return false;
                }
                delta.withdrawal_settlements.push_back(std::move(fact));
            }
        }
    }
    return true;
}

bool FlowMeshCheckpointIndex::VerifyBlock(
    const CBlock& block, const int height, const uint256& block_hash,
    const CChain& chain, const Consensus::Params& params,
    const FnSeatIndex& seats, const FlowMeshVaultIndex& vaults,
    FlowMeshCheckpointBlockDelta& out, std::string& error) const
{
    out = FlowMeshCheckpointBlockDelta{};
    if (!Consensus::FlowMeshRulesActive(height, params)) {
        error = "FlowMesh checkpoint index is not active";
        return false;
    }
    if (block_hash.IsNull()) {
        error = "FlowMesh checkpoint block hash is null";
        return false;
    }
    out.height = height;
    out.block_hash = block_hash;
    auto checkpoints{m_checkpoints};
    auto heads{m_heads};
    auto nullifiers{m_nullifiers};
    for (const CTransactionRef& tx : block.vtx) {
        if (!VerifyRecords(*tx, height, block_hash, chain, params, seats, vaults,
                           checkpoints, heads, nullifiers, out, error)) {
            return false;
        }
    }
    return true;
}

bool FlowMeshCheckpointIndex::VerifyTransaction(
    const CTransaction& tx, const int candidate_height, const CChain& chain,
    const Consensus::Params& params, const FnSeatIndex& seats,
    const FlowMeshVaultIndex& vaults, std::string& error) const
{
    if (!Consensus::FlowMeshRulesActive(candidate_height, params)) {
        error = "FlowMesh checkpoint index is not active";
        return false;
    }
    auto checkpoints{m_checkpoints};
    auto heads{m_heads};
    auto nullifiers{m_nullifiers};
    FlowMeshCheckpointBlockDelta ignored;
    ignored.height = candidate_height;
    return VerifyRecords(tx, candidate_height, uint256{}, chain, params, seats,
                         vaults, checkpoints, heads, nullifiers, ignored,
                         error);
}

bool FlowMeshCheckpointIndex::ConnectBlock(
    const FlowMeshCheckpointBlockDelta& delta, std::string& error)
{
    if (delta.height < 0 || delta.block_hash.IsNull()) {
        error = "invalid FlowMesh checkpoint block delta identity";
        return false;
    }
    if (!m_history.empty() && delta.height != m_history.back().height + 1) {
        error = "non-contiguous FlowMesh checkpoint block delta";
        return false;
    }

    auto checkpoints{m_checkpoints};
    auto heads{m_heads};
    auto nullifiers{m_nullifiers};
    std::set<uint256> withdrawal_nullifiers;
    for (const FlowMeshEffectNullifier& nullifier : delta.nullifiers) {
        if (nullifier.kind == FlowMeshNullifierKind::WITHDRAWAL_RECEIPT) {
            withdrawal_nullifiers.insert(nullifier.effect_id);
        }
    }
    std::set<uint256> settlement_receipts;
    std::set<Txid> settlement_transactions;
    for (const auto& fact : delta.withdrawal_settlements) {
        if (!flowmesh::WithdrawalSettlementFactIsCanonical(fact) ||
            fact.connected_height != delta.height ||
            fact.connected_block != delta.block_hash ||
            !m_checkpoints.contains(fact.checkpoint_id) ||
            !withdrawal_nullifiers.contains(fact.receipt.receipt_id) ||
            !settlement_receipts.insert(fact.receipt.receipt_id).second ||
            !settlement_transactions.insert(fact.transaction_id).second) {
            error = "FlowMesh checkpoint delta has an invalid withdrawal settlement";
            return false;
        }
    }
    if (settlement_receipts != withdrawal_nullifiers) {
        error = "FlowMesh checkpoint delta withdrawal settlements do not match nullifiers";
        return false;
    }
    for (const FlowMeshConnectedCheckpoint& checkpoint : delta.checkpoints) {
        if (checkpoint.connected_height != delta.height ||
            checkpoint.connected_block != delta.block_hash) {
            error = "FlowMesh checkpoint delta has mismatched block identity";
            return false;
        }
        const auto checkpoint_id{
            modern::FlowMeshCheckpointIdV1(checkpoint.core)};
        if (!checkpoint_id || *checkpoint_id != checkpoint.checkpoint_id) {
            error = "FlowMesh checkpoint delta has a mismatched checkpoint id";
            return false;
        }

        const auto head_it{heads.find(checkpoint.core.market_id)};
        std::optional<FlowMeshConnectedCheckpoint> head;
        if (head_it != heads.end()) {
            const auto prior{checkpoints.find(head_it->second)};
            if (prior == checkpoints.end()) {
                error = "FlowMesh checkpoint delta head index is inconsistent";
                return false;
            }
            head = prior->second;
        }
        if (!CheckCheckpointTransition(checkpoint.core, head, error)) {
            return false;
        }
        if (!checkpoints.emplace(checkpoint.checkpoint_id, checkpoint).second) {
            error = "FlowMesh checkpoint delta duplicates a checkpoint";
            return false;
        }
        heads.insert_or_assign(checkpoint.core.market_id,
                               checkpoint.checkpoint_id);
    }
    for (const FlowMeshEffectNullifier& nullifier : delta.nullifiers) {
        if ((nullifier.kind != FlowMeshNullifierKind::DEPOSIT_ACCEPTANCE &&
             nullifier.kind != FlowMeshNullifierKind::WITHDRAWAL_RECEIPT) ||
            nullifier.effect_id.IsNull()) {
            error = "FlowMesh checkpoint delta has an invalid nullifier";
            return false;
        }
        if (!nullifiers.insert(nullifier).second) {
            error = "FlowMesh checkpoint delta duplicates a nullifier";
            return false;
        }
    }
    m_checkpoints = std::move(checkpoints);
    m_heads = std::move(heads);
    m_nullifiers = std::move(nullifiers);
    m_history.push_back(delta);
    return true;
}

bool FlowMeshCheckpointIndex::DisconnectBlock(const int height,
                                               const uint256& block_hash,
                                               std::string& error)
{
    if (m_history.empty() || m_history.back().height != height ||
        m_history.back().block_hash != block_hash) {
        error = "FlowMesh checkpoint disconnect does not match the index tip";
        return false;
    }
    const FlowMeshCheckpointBlockDelta& delta{m_history.back()};
    auto checkpoints{m_checkpoints};
    auto heads{m_heads};
    auto nullifiers{m_nullifiers};
    for (auto it{delta.nullifiers.rbegin()}; it != delta.nullifiers.rend();
         ++it) {
        if (nullifiers.erase(*it) != 1) {
            error = "FlowMesh checkpoint undo cannot remove its nullifier";
            return false;
        }
    }
    for (auto it{delta.checkpoints.rbegin()}; it != delta.checkpoints.rend();
         ++it) {
        const auto head{heads.find(it->core.market_id)};
        const auto checkpoint{checkpoints.find(it->checkpoint_id)};
        if (head == heads.end() || head->second != it->checkpoint_id ||
            checkpoint == checkpoints.end() || !(checkpoint->second == *it)) {
            error = "FlowMesh checkpoint undo does not match its market head";
            return false;
        }
        checkpoints.erase(checkpoint);
        if (it->core.previous_checkpoint_id.IsNull()) {
            heads.erase(head);
        } else {
            if (!checkpoints.contains(it->core.previous_checkpoint_id)) {
                error = "FlowMesh checkpoint undo cannot restore its prior head";
                return false;
            }
            head->second = it->core.previous_checkpoint_id;
        }
    }
    m_checkpoints = std::move(checkpoints);
    m_heads = std::move(heads);
    m_nullifiers = std::move(nullifiers);
    m_history.pop_back();
    return true;
}

void FlowMeshCheckpointIndex::Clear()
{
    m_checkpoints.clear();
    m_heads.clear();
    m_nullifiers.clear();
    m_history.clear();
}

bool FlowMeshCheckpointTracker::ApplyBlock(
    const CBlock& block, const CBlockIndex& index, const CChain& chain,
    const Consensus::Params& params, const FnSeatIndex& seats,
    const FlowMeshVaultIndex& vaults)
{
    FlowMeshCheckpointBlockDelta delta;
    std::string error;
    if (!m_index.VerifyBlock(block, index.nHeight, index.GetBlockHash(), chain,
                             params, seats, vaults, delta, error) ||
        !m_index.ConnectBlock(delta, error)) {
        LogWarning(
            "FlowMeshCheckpointTracker: block at height %d failed checkpoint verification (%s); index unavailable",
            index.nHeight, error);
        m_index.Clear();
        m_dirty = true;
        return false;
    }
    return true;
}

bool FlowMeshCheckpointTracker::Sync(
    const CChain& chain, const BlockManager& blockman,
    const Consensus::Params& params, const FnSeatIndex& seats,
    const FlowMeshVaultIndex& vaults,
    const CBlockIndex& target)
{
    if (Synced(target.GetBlockHash())) return true;
    if (chain[target.nHeight] != &target ||
        !Consensus::FlowMeshSeatBindingScheduleConfigured(params)) {
        return false;
    }
    const int activation{*params.flowmesh_activation_height};
    if (target.nHeight < activation) {
        m_index.Clear();
        m_synced_tip = target.GetBlockHash();
        m_synced_height = target.nHeight;
        m_dirty = false;
        return true;
    }

    int start{activation};
    if (!m_dirty && m_synced_height >= activation - 1 &&
        m_synced_height <= target.nHeight && chain[m_synced_height] != nullptr &&
        chain[m_synced_height]->GetBlockHash() == m_synced_tip) {
        start = std::max(activation, m_synced_height + 1);
    } else {
        m_index.Clear();
        m_dirty = true;
    }
    for (int height{start}; height <= target.nHeight; ++height) {
        const CBlockIndex* index{chain[height]};
        CBlock block;
        if (index == nullptr || !blockman.ReadBlock(block, *index)) {
            LogWarning(
                "FlowMeshCheckpointTracker: cannot read A3+ block at height %d; index unavailable",
                height);
            m_index.Clear();
            m_dirty = true;
            return false;
        }
        if (!ApplyBlock(block, *index, chain, params, seats, vaults)) return false;
    }
    m_synced_tip = target.GetBlockHash();
    m_synced_height = target.nHeight;
    m_dirty = false;
    return true;
}

void FlowMeshCheckpointTracker::BlockConnected(
    const CBlock& block, const CBlockIndex& index, const CChain& chain,
    const Consensus::Params& params, const FnSeatIndex& seats,
    const FlowMeshVaultIndex& vaults)
{
    if (!Consensus::FlowMeshSeatBindingScheduleConfigured(params)) return;
    if (m_dirty || index.pprev == nullptr ||
        m_synced_tip != index.pprev->GetBlockHash()) {
        m_dirty = true;
        return;
    }
    if (index.nHeight >= *params.flowmesh_activation_height &&
        !ApplyBlock(block, index, chain, params, seats, vaults)) {
        return;
    }
    m_synced_tip = index.GetBlockHash();
    m_synced_height = index.nHeight;
}

void FlowMeshCheckpointTracker::BlockDisconnected(
    const CBlockIndex& index, const Consensus::Params& params)
{
    if (m_dirty || m_synced_tip != index.GetBlockHash() ||
        index.pprev == nullptr) {
        m_dirty = true;
        return;
    }
    if (Consensus::FlowMeshSeatBindingScheduleConfigured(params) &&
        index.nHeight >= *params.flowmesh_activation_height) {
        std::string error;
        if (!m_index.DisconnectBlock(index.nHeight, index.GetBlockHash(),
                                     error)) {
            LogWarning(
                "FlowMeshCheckpointTracker: failed to disconnect height %d (%s); index unavailable",
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
