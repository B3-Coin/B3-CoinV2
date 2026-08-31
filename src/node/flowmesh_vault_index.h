// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_VAULT_INDEX_H
#define B3COIN_NODE_FLOWMESH_VAULT_INDEX_H

#include <consensus/params.h>
#include <flowmesh/deposit.h>
#include <flowmesh/market.h>
#include <modern/policy.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

class CBlockIndex;
class CChain;
class Chainstate;
namespace node { class BlockManager; }

namespace node {

/**
 * An anchor interval ending before A3 cannot contain a connected FlowMesh
 * withdrawal: type-9 records are consensus-invalid there. This is a positive
 * absence proof only when the complete A3 schedule is configured; callers
 * must still establish that both anchors are on one canonical chain.
 */
inline bool FlowMeshIntervalProvablyHasNoWithdrawals(
    const int after_exclusive, const int through_inclusive,
    const Consensus::Params& params)
{
    return params.flowmesh_activation_height && after_exclusive >= 0 &&
           through_inclusive > after_exclusive &&
           through_inclusive < *params.flowmesh_activation_height;
}

/** One canonical A2+ DEX_VAULT-v2 output and all chain-derived semantics. */
struct FlowMeshVaultRecord {
    COutPoint outpoint;
    modern::AssetId asset;
    CAmount amount{0};
    flowmesh::VaultId vault_id;
    uint8_t kind{0};
    uint16_t shard{0};
    std::optional<flowmesh::AccountId> account;
    int created_height{-1};
    uint256 created_block;

    friend bool operator==(const FlowMeshVaultRecord& a,
                           const FlowMeshVaultRecord& b) = default;
};

/**
 * Immutable on-chain creation fact for one simple-v1 asset/B3 market.
 * VaultId is one-way, so the first surviving USER_DEPOSIT must carry the
 * colored base asset and thereby prove this mapping before native B3 may
 * enter the vault.
 */
struct FlowMeshMarketRecord {
    modern::AssetId base_asset;
    flowmesh::MarketId market_id;
    flowmesh::VaultId vault_id;
    COutPoint establishing_deposit;
    int created_height{-1};
    uint256 created_block;

    friend bool operator==(const FlowMeshMarketRecord& a,
                           const FlowMeshMarketRecord& b) = default;
};

/** Net live-vault change at one exact A2+ block, including empty deltas. */
struct FlowMeshVaultBlockDelta {
    int height{-1};
    uint256 block_hash;
    std::vector<FlowMeshVaultRecord> removed; // input order
    std::vector<FlowMeshVaultRecord> added;   // transaction/vout order
    std::vector<FlowMeshMarketRecord> markets_added; // first colored deposits

    friend bool operator==(const FlowMeshVaultBlockDelta& a,
                           const FlowMeshVaultBlockDelta& b) = default;
};

/**
 * Derived vault history. Candidate verification is scratch-only; Connect and
 * Disconnect atomically apply exact net deltas. History is keyed by both
 * height and block hash and can reconstruct live state at any retained active
 * anchor.
 */
class FlowMeshVaultIndex
{
public:
    std::optional<FlowMeshVaultRecord> Get(const COutPoint& outpoint) const;
    std::optional<FlowMeshMarketRecord> Market(
        const flowmesh::MarketId& market_id) const;
    //! Market is returned only if established on the active chain at `anchor`.
    std::optional<FlowMeshMarketRecord> MarketAt(
        const flowmesh::MarketId& market_id,
        const CBlockIndex& anchor) const;
    bool VerifyBlock(const CBlock& block, int height, const uint256& block_hash,
                     const Consensus::Params& params,
                     FlowMeshVaultBlockDelta& out, std::string& error) const;
    bool ConnectBlock(const FlowMeshVaultBlockDelta& delta, std::string& error);
    bool DisconnectBlock(int height, const uint256& block_hash,
                         std::string& error);

    //! Record is returned only if live immediately after `anchor`.
    std::optional<FlowMeshVaultRecord> LookupAt(
        const COutPoint& outpoint, const CBlockIndex& anchor) const;

    /**
     * Resolve the complete chain-derived deposit fact for one configured v1
     * market. `expected_market` must itself equal MarketId(domain, base).
     */
    std::optional<flowmesh::DepositInfo> ResolveDepositAt(
        const COutPoint& outpoint, const CBlockIndex& anchor,
        const uint256& domain, const modern::AssetId& base_asset,
        const flowmesh::MarketId& expected_market,
        int vault_preparation_height,
        int flowmesh_activation_height) const;

    void Clear();
    int ConnectedHeight() const
    {
        return m_history.empty() ? -1 : m_history.back().height;
    }
    uint256 ConnectedHash() const
    {
        return m_history.empty() ? uint256{} : m_history.back().block_hash;
    }
    size_t Size() const { return m_live.size(); }
    const std::map<COutPoint, FlowMeshVaultRecord>& All() const { return m_live; }
    const std::map<flowmesh::VaultId, FlowMeshMarketRecord>& Markets() const
    {
        return m_markets;
    }
    std::vector<FlowMeshMarketRecord> MarketsAt(
        const CBlockIndex& anchor) const;

    /** Largest live pool outputs at an exact retained anchor, ordered by
     * amount descending and then outpoint ascending. The result is bounded by
     * the frozen type-9 input limit. */
    std::optional<std::vector<FlowMeshVaultRecord>>
    LargestWithdrawalInputsAt(const flowmesh::VaultId& vault_id,
                              const modern::AssetId& asset,
                              const CBlockIndex& anchor) const;

    //! Sum of LargestWithdrawalInputsAt(), or null when history is unavailable.
    std::optional<CAmount> WithdrawalCapacityAt(
        const flowmesh::VaultId& vault_id, const modern::AssetId& asset,
        const CBlockIndex& anchor) const;
    const std::vector<FlowMeshVaultBlockDelta>& History() const { return m_history; }

private:
    std::map<COutPoint, FlowMeshVaultRecord> m_live;
    std::map<flowmesh::VaultId, FlowMeshMarketRecord> m_markets;
    std::vector<FlowMeshVaultBlockDelta> m_history;
};

/** Active-chain A3 replay/undo driver. */
class FlowMeshVaultTracker
{
public:
    bool Sync(const CChain& chain, const BlockManager& blockman,
              const Consensus::Params& params, const CBlockIndex& target);
    void BlockConnected(const CBlock& block, const CBlockIndex& index,
                        const Consensus::Params& params);
    void BlockDisconnected(const CBlockIndex& index,
                           const Consensus::Params& params);
    void MarkDirty() { m_dirty = true; }
    bool Synced(const uint256& tip_hash) const
    {
        return !m_dirty && m_synced_tip == tip_hash;
    }
    const FlowMeshVaultIndex& Index() const { return m_index; }

private:
    bool ApplyBlock(const CBlock& block, const CBlockIndex& index,
                    const Consensus::Params& params);

    FlowMeshVaultIndex m_index;
    uint256 m_synced_tip;
    int m_synced_height{-1};
    bool m_dirty{true};
};

/**
 * Production DepositVerifier bound to one simple-v1 base/B3 market. The
 * action supplies only `(outpoint, anchor)`; every other fact is recovered
 * from the active-chain vault history.
 */
class ChainDepositVerifier final : public flowmesh::DepositVerifier
{
public:
    ChainDepositVerifier(Chainstate& chainstate,
                         const modern::AssetId& base_asset,
                         const flowmesh::MarketId& market_id)
        : m_chainstate{chainstate}, m_base_asset{base_asset},
          m_market_id{market_id}
    {
    }

    std::optional<flowmesh::DepositInfo> GetDeposit(
        const COutPoint& outpoint,
        const flowmesh::AnchorRef& anchor) const override;

    std::optional<CAmount> GetWithdrawalCapacity(
        const modern::AssetId& asset,
        const flowmesh::AnchorRef& anchor) const override;

    std::optional<std::vector<flowmesh::WithdrawalSettlementFactV1>>
    GetWithdrawalSettlements(
        const std::optional<flowmesh::AnchorRef>& after_exclusive,
        const flowmesh::AnchorRef& through_inclusive) const override;

    std::optional<flowmesh::WithdrawalSettlementPlan>
    PlanWithdrawalSettlements(
        const std::optional<flowmesh::AnchorRef>& after_exclusive,
        const flowmesh::AnchorRef& through_inclusive) const override;

private:
    Chainstate& m_chainstate;
    const modern::AssetId m_base_asset;
    const flowmesh::MarketId m_market_id;
};

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_VAULT_INDEX_H
