// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_ANCHOR_H
#define B3COIN_NODE_FLOWMESH_ANCHOR_H

#include <consensus/flowmesh_params.h>
#include <flowmesh/deposit.h>

#include <optional>

class ChainstateManager;

namespace node {

/**
 * B3-chain-backed anchor policy: an anchor is acceptable iff its hash is
 * a block ON THE ACTIVE CHAIN at its claimed height, buried at least
 * `min_depth` blocks under the tip. `Current()` returns exactly the
 * newest acceptable position.
 *
 * `min_depth` is the OD-6 finality-depth OWNER DECISION carried as an
 * explicit constructor input — there is no default. A base-chain reorg
 * deeper than `min_depth` can invalidate previously acceptable anchors;
 * that residual risk is precisely what the owner's depth choice prices.
 * FlowMesh consulting this policy can stall when B3 stalls, never the
 * reverse: nothing here is reachable from B3 validation.
 */
/**
 * FLOWMESH_ANCHOR_DEPTH — RATIFIED (owner ruling 2026-08-22): 30 blocks,
 * ~30 minutes at the 60-second Modern-PoS interval. The SAME depth governs
 * recognizing B3 deposits, accepting microblock anchors, and making
 * certified withdrawal receipts redeemable on B3. Deliberately distinct
 * from MODERN_REORG_HORIZON (1440): this depth protects FlowMesh custody
 * against ordinary recent B3 reorgs; the horizon prevents deep Modern-PoS
 * chain replacement. Production wiring passes this constant to
 * ChainAnchorPolicy; fixtures may still pass small scaffolding depths.
 */
inline constexpr int FLOWMESH_ANCHOR_DEPTH{Consensus::FLOWMESH_ANCHOR_DEPTH};
static_assert(FLOWMESH_ANCHOR_DEPTH == 30);

class ChainAnchorPolicy final : public flowmesh::AnchorPolicy
{
public:
    ChainAnchorPolicy(const ChainstateManager& chainman, int min_depth);

    bool Acceptable(const flowmesh::AnchorRef& anchor) const override;
    bool StillCanonical(const flowmesh::AnchorRef& anchor) const override;
    flowmesh::AnchorRef Current() const override;

private:
    const ChainstateManager& m_chainman;
    const int m_min_depth;
};

/**
 * Fail-closed deposit verifier: production deposit recognition requires
 * canonical DEX_VAULT outputs on the B3 chain, which do not exist until
 * the modern policy/asset layer is activated (base-chain critical path)
 * and the vault mechanism is owner-ratified. Until then every deposit is
 * refused — custody facts are never improvised.
 */
class UnavailableDepositVerifier final : public flowmesh::DepositVerifier
{
public:
    std::optional<flowmesh::DepositInfo> GetDeposit(const COutPoint&,
                                                    const flowmesh::AnchorRef&) const override
    {
        return std::nullopt;
    }

    std::optional<CAmount> GetWithdrawalCapacity(
        const modern::AssetId&, const flowmesh::AnchorRef&) const override
    {
        return std::nullopt;
    }

    std::optional<std::vector<flowmesh::WithdrawalSettlementFactV1>>
    GetWithdrawalSettlements(
        const std::optional<flowmesh::AnchorRef>&,
        const flowmesh::AnchorRef&) const override
    {
        return std::vector<flowmesh::WithdrawalSettlementFactV1>{};
    }
};

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_ANCHOR_H
