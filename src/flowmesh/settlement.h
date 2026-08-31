// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_SETTLEMENT_H
#define B3COIN_FLOWMESH_SETTLEMENT_H

#include <modern/flowmesh_checkpoint.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <cstdint>
#include <cstddef>

namespace flowmesh {

//! Frozen decode/execution bound for one mandatory settlement transition.
//! Exceeding it fails closed; no connected settlement may be silently skipped.
inline constexpr size_t FLOWMESH_MAX_WITHDRAWAL_SETTLEMENTS_PER_ENTRY{4096};

//! Consensus authorization accepts at most this many keyless pool inputs in
//! one type-9 withdrawal transaction.
inline constexpr size_t FLOWMESH_MAX_WITHDRAWAL_VAULT_INPUTS{64};

/**
 * One exact type-9 withdrawal that connected on B3.
 *
 * Production derives these facts from the active-chain checkpoint index over
 * an anchor interval. They are never supplied by a user or proposer. Keeping
 * the complete certified receipt and its B3 connection identity makes the
 * later FlowMesh liability retirement independently replayable and auditable.
 */
struct WithdrawalSettlementFactV1 {
    modern::FlowMeshWithdrawalReceiptV1 receipt;
    modern::FlowMeshCheckpointId checkpoint_id;
    Txid transaction_id;
    int32_t connected_height{-1};
    uint256 connected_block;

    friend bool operator==(const WithdrawalSettlementFactV1& a,
                           const WithdrawalSettlementFactV1& b) = default;
};

inline bool WithdrawalSettlementFactIsCanonical(
    const WithdrawalSettlementFactV1& fact)
{
    return !fact.checkpoint_id.IsNull() && !fact.transaction_id.IsNull() &&
           fact.connected_height >= 0 && !fact.connected_block.IsNull() &&
           modern::EncodeFlowMeshEffectV1(
               modern::FlowMeshEffectV1{fact.receipt})
               .has_value();
}

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_SETTLEMENT_H
