// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <node/flowmesh_client_settlement.h>

#include <chain.h>
#include <coins.h>
#include <consensus/era.h>
#include <modern/flowmesh_checkpoint.h>
#include <modern/flowmesh_vault_proof.h>
#include <modern/mpa.h>
#include <node/flowmesh_checkpoint_index.h>
#include <node/flowmesh_vault_index.h>
#include <node/fn_seat_index.h>
#include <sync.h>
#include <util/int128.h>
#include <validation.h>

#include <limits>
#include <utility>
#include <variant>
#include <vector>

namespace node {
namespace {

//! The mandatory B3 indexes remain available with the optional engine off.
//! Keep their dependency order identical to chain validation and the local
//! service. A missing/pruned history fails closed, never to endpoint state.
bool SyncSettlementIndexes(ChainstateManager& chainman, Chainstate& chainstate,
                           const CBlockIndex& tip, std::string& error)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    const auto& params{chainman.GetConsensus()};
    FnSeatTracker& seats{chainstate.ModernFnSeats()};
    if (!seats.Sync(chainstate.m_chain, chainstate.m_blockman, params, tip)) {
        error = "FlowMesh FN-seat history is unavailable";
        return false;
    }
    FlowMeshVaultTracker& vaults{chainstate.ModernFlowMeshVaults()};
    if (!vaults.Sync(chainstate.m_chain, chainstate.m_blockman, params, tip)) {
        error = "FlowMesh vault history is unavailable";
        return false;
    }
    if (!chainstate.ModernFlowMeshCheckpoints().Sync(
            chainstate.m_chain, chainstate.m_blockman, params, seats.Index(),
            vaults.Index(), tip)) {
        error = "FlowMesh checkpoint history is unavailable";
        return false;
    }
    return true;
}

bool AppendLiveInput(Chainstate& chainstate, const FlowMeshVaultRecord& record,
                     std::vector<interfaces::FlowMeshVaultInput>& inputs)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    const Coin& coin{chainstate.CoinsTip().AccessCoin(record.outpoint)};
    if (coin.IsSpent()) return false;
    inputs.push_back({record.outpoint, coin.out});
    return true;
}

uint256 EffectId(const modern::FlowMeshEffectV1& effect)
{
    if (const auto* deposit{std::get_if<modern::FlowMeshDepositAcceptanceV1>(&effect)}) {
        return deposit->acceptance_id;
    }
    return std::get<modern::FlowMeshWithdrawalReceiptV1>(effect).receipt_id;
}

} // namespace

std::optional<interfaces::FlowMeshPendingCheckpoint> VerifyClientCheckpoint(
    ChainstateManager& chainman, const uint256& expected_market,
    const std::span<const unsigned char> payload, std::string& error)
{
    error.clear();
    const auto envelope{modern::DecodeFlowMeshCheckpointEnvelopeV1(payload)};
    if (expected_market.IsNull() || !envelope ||
        envelope->core.market_id != expected_market) {
        error = "FlowMesh checkpoint payload is malformed or names a different market";
        return std::nullopt;
    }
    const auto checkpoint_id{modern::FlowMeshCheckpointIdV1(envelope->core)};
    if (!checkpoint_id) {
        error = "FlowMesh checkpoint identity is invalid";
        return std::nullopt;
    }
    CMpaRecord record{modern::MPA_TYPE_FLOWMESH_CHECKPOINT, modern::MPA_VERSION_V1,
                      std::vector<unsigned char>{payload.begin(), payload.end()}};
    {
        LOCK(::cs_main);
        Chainstate& chainstate{chainman.ActiveChainstate()};
        const CBlockIndex* tip{chainstate.m_chain.Tip()};
        if (!tip || tip->nHeight == std::numeric_limits<int>::max() ||
            !SyncSettlementIndexes(chainman, chainstate, *tip, error)) {
            if (error.empty()) error = "FlowMesh B3 chain is unavailable";
            return std::nullopt;
        }
        // The scratch transaction is never submitted. VerifyTransaction uses
        // copies of the checkpoint/head/nullifier maps and performs the same
        // anchored membership, bitmap, quorum, signature, transition, domain
        // and market checks as candidate B3 transactions.
        CMutableTransaction scratch;
        scratch.version = 2;
        scratch.mpa.push_back(record);
        if (!chainstate.ModernFlowMeshCheckpoints().Index().VerifyTransaction(
                CTransaction{scratch}, tip->nHeight + 1, chainstate.m_chain,
                chainman.GetConsensus(), chainstate.ModernFnSeats().Index(),
                chainstate.ModernFlowMeshVaults().Index(), error)) {
            return std::nullopt;
        }
    }
    return interfaces::FlowMeshPendingCheckpoint{
        std::move(record), *checkpoint_id, envelope->core.sequence,
        envelope->core.effect_count};
}

std::optional<interfaces::FlowMeshVaultOperation> VerifyClientVaultOperation(
    ChainstateManager& chainman, const uint256& expected_market,
    const std::optional<uint256>& expected_effect,
    const std::span<const unsigned char> proof_bytes, std::string& error)
{
    error.clear();
    const auto proof{modern::DecodeFlowMeshVaultProofV1(proof_bytes)};
    if (expected_market.IsNull() || !proof ||
        std::visit([](const auto& effect) { return effect.market_id; }, proof->effect) != expected_market ||
        (expected_effect && (expected_effect->IsNull() || EffectId(proof->effect) != *expected_effect))) {
        error = "FlowMesh vault proof is malformed or names a different market/effect";
        return std::nullopt;
    }

    std::vector<interfaces::FlowMeshVaultInput> inputs;
    {
        LOCK(::cs_main);
        Chainstate& chainstate{chainman.ActiveChainstate()};
        const CBlockIndex* tip{chainstate.m_chain.Tip()};
        if (!tip || !SyncSettlementIndexes(chainman, chainstate, *tip, error)) {
            if (error.empty()) error = "FlowMesh B3 chain is unavailable";
            return std::nullopt;
        }
        const auto& checkpoints{chainstate.ModernFlowMeshCheckpoints().Index()};
        const auto checkpoint{checkpoints.Get(proof->checkpoint_id)};
        if (!checkpoint || checkpoint->core.market_id != expected_market ||
            !checkpoints.VerifyVaultProof(*proof, error)) {
            if (error.empty()) error = "FlowMesh vault proof checkpoint is not connected for this market";
            return std::nullopt;
        }
        const auto nullifier{FlowMeshNullifierForProof(*proof)};
        if (!nullifier || checkpoints.IsNullified(*nullifier)) {
            error = "FlowMesh vault effect is already nullified";
            return std::nullopt;
        }
        const auto& vaults{chainstate.ModernFlowMeshVaults().Index()};
        if (const auto* deposit{std::get_if<modern::FlowMeshDepositAcceptanceV1>(&proof->effect)}) {
            const auto input{vaults.Get(deposit->deposit_outpoint)};
            if (!input || input->vault_id != deposit->vault_id ||
                input->kind != modern::VAULT_KIND_USER_DEPOSIT ||
                !input->account || *input->account != deposit->account ||
                input->asset != deposit->asset || input->amount != deposit->amount ||
                input->shard != deposit->shard || !AppendLiveInput(chainstate, *input, inputs)) {
                error = "FlowMesh accepted deposit is no longer a live exact vault input";
                return std::nullopt;
            }
        } else {
            const auto& receipt{std::get<modern::FlowMeshWithdrawalReceiptV1>(proof->effect)};
            const auto candidates{vaults.LargestWithdrawalInputsAt(receipt.vault_id, receipt.asset, *tip)};
            if (!candidates) {
                error = "FlowMesh withdrawal capacity index is unavailable";
                return std::nullopt;
            }
            util::Signed128 selected{0};
            for (const auto& input : *candidates) {
                if (!AppendLiveInput(chainstate, input, inputs)) {
                    error = "FlowMesh selected vault input is not live";
                    return std::nullopt;
                }
                selected += input.amount;
                if (selected >= receipt.amount) break;
            }
            if (selected < receipt.amount) {
                error = "FlowMesh vault has insufficient live liquidity for the receipt";
                return std::nullopt;
            }
        }
    }
    return interfaces::FlowMeshVaultOperation{
        expected_market, proof->checkpoint_id,
        CMpaRecord{modern::MPA_TYPE_FLOWMESH_VAULT_PROOF, modern::MPA_VERSION_V1,
                   std::vector<unsigned char>{proof_bytes.begin(), proof_bytes.end()}},
        proof->effect, std::move(inputs)};
}

} // namespace node
