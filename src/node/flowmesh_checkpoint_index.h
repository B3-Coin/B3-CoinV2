// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_CHECKPOINT_INDEX_H
#define B3COIN_NODE_FLOWMESH_CHECKPOINT_INDEX_H

#include <consensus/params.h>
#include <coins.h>
#include <flowmesh/market.h>
#include <flowmesh/settlement.h>
#include <modern/flowmesh_checkpoint.h>
#include <modern/flowmesh_vault_proof.h>
#include <primitives/block.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

class CBlockIndex;
class CChain;

namespace node {

class BlockManager;
class FnSeatIndex;
class FlowMeshVaultIndex;

/** One certificate-verified checkpoint connected on B3. */
struct FlowMeshConnectedCheckpoint {
    modern::FlowMeshCheckpointId checkpoint_id;
    modern::FlowMeshCheckpointCoreV1 core;
    int connected_height{-1};
    uint256 connected_block;

    friend bool operator==(const FlowMeshConnectedCheckpoint& a,
                           const FlowMeshConnectedCheckpoint& b) = default;
};

enum class FlowMeshNullifierKind : uint8_t {
    DEPOSIT_ACCEPTANCE = 1,
    WITHDRAWAL_RECEIPT = 2,
};

/** A typed effect identity consumed by one connected type-9 proof. */
struct FlowMeshEffectNullifier {
    FlowMeshNullifierKind kind{FlowMeshNullifierKind::DEPOSIT_ACCEPTANCE};
    uint256 effect_id;

    friend bool operator==(const FlowMeshEffectNullifier& a,
                           const FlowMeshEffectNullifier& b) = default;
    friend bool operator<(const FlowMeshEffectNullifier& a,
                          const FlowMeshEffectNullifier& b)
    {
        if (a.kind != b.kind) {
            return static_cast<uint8_t>(a.kind) < static_cast<uint8_t>(b.kind);
        }
        return a.effect_id < b.effect_id;
    }
};

/** Exact state appended by one A3+ block, including empty blocks. */
struct FlowMeshCheckpointBlockDelta {
    int height{-1};
    uint256 block_hash;
    std::vector<FlowMeshConnectedCheckpoint> checkpoints; // block/MPA order
    std::vector<FlowMeshEffectNullifier> nullifiers;       // block/MPA order
    std::vector<flowmesh::WithdrawalSettlementFactV1>
        withdrawal_settlements; // block/transaction order

    friend bool operator==(const FlowMeshCheckpointBlockDelta& a,
                           const FlowMeshCheckpointBlockDelta& b) = default;
};

/** Per-input script exemption produced only by a valid type-9 operation. */
struct FlowMeshVaultAuthorization {
    std::vector<bool> authorized_inputs;
    std::optional<FlowMeshEffectNullifier> nullifier;
};

/**
 * Rebuildable chain authorization state for type 8 and the proof-replay part
 * of type 9. Candidate verification operates on scratch maps and produces a
 * complete block delta; Connect/Disconnect then apply or undo that exact
 * already-verified delta.
 *
 * Vault input/output conservation is intentionally a transaction-context
 * rule and is wired by validation with the UTXO view. This index owns only
 * checkpoint ancestry, BLS authorization, effect inclusion, and durable
 * deposit/receipt nullifiers.
 */
class FlowMeshCheckpointIndex
{
public:
    std::optional<FlowMeshConnectedCheckpoint> Get(
        const modern::FlowMeshCheckpointId& checkpoint_id) const;
    std::optional<FlowMeshConnectedCheckpoint> Head(
        const flowmesh::MarketId& market_id) const;
    bool IsNullified(const FlowMeshEffectNullifier& nullifier) const;
    bool VerifyVaultProof(const modern::FlowMeshVaultProofV1& proof,
                          std::string& error) const;

    bool VerifyBlock(const CBlock& block, int height, const uint256& block_hash,
                     const CChain& chain, const Consensus::Params& params,
                     const FnSeatIndex& seats,
                     const FlowMeshVaultIndex& vaults,
                     FlowMeshCheckpointBlockDelta& out,
                     std::string& error) const;

    /** Mempool/miner preview against the currently connected index. */
    bool VerifyTransaction(const CTransaction& tx, int candidate_height,
                           const CChain& chain,
                           const Consensus::Params& params,
                           const FnSeatIndex& seats,
                           const FlowMeshVaultIndex& vaults,
                           std::string& error) const;

    bool ConnectBlock(const FlowMeshCheckpointBlockDelta& delta,
                      std::string& error);
    bool DisconnectBlock(int height, const uint256& block_hash,
                         std::string& error);
    void Clear();

    int ConnectedHeight() const
    {
        return m_history.empty() ? -1 : m_history.back().height;
    }
    uint256 ConnectedHash() const
    {
        return m_history.empty() ? uint256{} : m_history.back().block_hash;
    }
    size_t CheckpointCount() const { return m_checkpoints.size(); }
    size_t NullifierCount() const { return m_nullifiers.size(); }
    const std::vector<FlowMeshCheckpointBlockDelta>& History() const
    {
        return m_history;
    }

    /**
     * Return every connected withdrawal for `market_id` in the exact active
     * indexed interval `(after, through]`, sorted by receipt id. Both anchors
     * must name this index's retained canonical history. Intervals above the
     * frozen per-entry bound fail before the result can grow beyond it.
     */
    bool WithdrawalSettlementsBetween(
        const flowmesh::MarketId& market_id,
        const CBlockIndex& after_exclusive,
        const CBlockIndex& through_inclusive,
        std::vector<flowmesh::WithdrawalSettlementFactV1>& out,
        std::string& error) const;

    /**
     * Choose the furthest block boundary in `(after, through]` containing no
     * more than `max_count` withdrawals for this market. This scans counts in
     * retained block history and never materializes the complete backlog.
     * A single block over the bound is unsplittable and fails closed.
     */
    bool WithdrawalSettlementCatchupHeight(
        const flowmesh::MarketId& market_id,
        const CBlockIndex& after_exclusive,
        const CBlockIndex& through_inclusive, size_t max_count,
        int& selected_height, size_t& selected_count,
        std::string& error) const;

private:
    bool VerifyRecords(const CTransaction& tx, int candidate_height,
                       const uint256& candidate_block, const CChain& chain,
                       const Consensus::Params& params,
                       const FnSeatIndex& seats,
                       const FlowMeshVaultIndex& vaults,
                       std::map<modern::FlowMeshCheckpointId,
                                FlowMeshConnectedCheckpoint>& checkpoints,
                       std::map<flowmesh::MarketId,
                                modern::FlowMeshCheckpointId>& heads,
                       std::set<FlowMeshEffectNullifier>& nullifiers,
                       FlowMeshCheckpointBlockDelta& delta,
                       std::string& error) const;

    std::map<modern::FlowMeshCheckpointId, FlowMeshConnectedCheckpoint>
        m_checkpoints;
    std::map<flowmesh::MarketId, modern::FlowMeshCheckpointId> m_heads;
    std::set<FlowMeshEffectNullifier> m_nullifiers;
    std::vector<FlowMeshCheckpointBlockDelta> m_history;
};

/** Active-chain replay/undo driver. */
class FlowMeshCheckpointTracker
{
public:
    bool Sync(const CChain& chain, const BlockManager& blockman,
              const Consensus::Params& params, const FnSeatIndex& seats,
              const FlowMeshVaultIndex& vaults,
              const CBlockIndex& target);
    void BlockConnected(const CBlock& block, const CBlockIndex& index,
                        const CChain& chain, const Consensus::Params& params,
                        const FnSeatIndex& seats,
                        const FlowMeshVaultIndex& vaults);
    void BlockDisconnected(const CBlockIndex& index,
                           const Consensus::Params& params);
    void MarkDirty() { m_dirty = true; }
    bool Synced(const uint256& tip_hash) const
    {
        return !m_dirty && m_synced_tip == tip_hash;
    }
    const FlowMeshCheckpointIndex& Index() const { return m_index; }

private:
    bool ApplyBlock(const CBlock& block, const CBlockIndex& index,
                    const CChain& chain, const Consensus::Params& params,
                    const FnSeatIndex& seats,
                    const FlowMeshVaultIndex& vaults);

    FlowMeshCheckpointIndex m_index;
    uint256 m_synced_tip;
    int m_synced_height{-1};
    bool m_dirty{true};
};

std::optional<FlowMeshEffectNullifier> FlowMeshNullifierForProof(
    const modern::FlowMeshVaultProofV1& proof);

/**
 * Exact keyless DEX_VAULT spend authorization. `prev_coins` and `tx_fee`
 * are the already-checked UTXO projection from CheckTxInputs. The returned
 * bitmap authorizes script bypass for matching DEX_VAULT inputs only.
 */
bool CheckFlowMeshVaultTransaction(
    const CTransaction& tx, const std::vector<Coin>& prev_coins,
    CAmount tx_fee, int height, const Consensus::Params& params,
    const FlowMeshCheckpointIndex& checkpoints,
    FlowMeshVaultAuthorization& authorization, std::string& error);

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_CHECKPOINT_INDEX_H
