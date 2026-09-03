// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_SERVICE_H
#define B3COIN_NODE_FLOWMESH_SERVICE_H

#include <crypto/bls.h>
#include <flowmesh/production_engine.h>
#include <modern/flowmesh_vault_proof.h>
#include <node/flowmesh_runtime.h>
#include <node/flowmesh_vault_index.h>
#include <primitives/transaction.h>
#include <uint256.h>
#include <util/fs.h>
#include <validationinterface.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

class CBlockIndex;
class ChainstateManager;
class PeerManager;

namespace node {

/** Public, chain-derived identity of one production FlowMesh v1 market. */
struct FlowMeshServiceMarket {
    uint256 domain;
    flowmesh::MarketId market_id;
    flowmesh::VaultId vault_id;
    modern::AssetId base_asset;
    modern::AssetId quote_asset;
    uint256 execution_config_id;

    friend bool operator==(const FlowMeshServiceMarket&,
                           const FlowMeshServiceMarket&) = default;
};

/**
 * A wallet-ready type-8 publication. The service, rather than the wallet,
 * resolves the historical seat count and performs the exact bitmap encoding.
 */
struct FlowMeshPendingCheckpoint {
    CMpaRecord record;
    modern::FlowMeshCheckpointId checkpoint_id;
    uint64_t sequence{0};
    uint32_t effect_count{0};
};

/** One exact live keyless input selected for a type-9 vault operation. */
struct FlowMeshVaultInput {
    FlowMeshVaultRecord record;
    CTxOut txout;
};

/**
 * Complete chain-derived material for a wallet to fund and relay one type-9
 * transaction. The wallet supplies only an ordinary native fee input and the
 * committed OWNER payout/change outputs; it never chooses a vault input or
 * rewrites the certified proof.
 */
struct FlowMeshVaultOperation {
    flowmesh::MarketId market_id;
    modern::FlowMeshCheckpointId checkpoint_id;
    CMpaRecord record;
    modern::FlowMeshEffectV1 effect;
    std::vector<FlowMeshVaultInput> inputs;
};

/**
 * One production FlowMesh service exists on every node. It remains dormant
 * unless the complete A2/A3 schedule is pinned. When enabled it uses only the
 * existing B3 connection and PeerManager's prioritized FlowMesh messages;
 * there is no second listener, port, or transport.
 *
 * The service starts as an observer. Supplying wallet-owned BLS seat keys is
 * an explicit, reversible operation and raw keys never reach P2P objects.
 */
class FlowMeshService final : public flowmesh::WireMessageSink,
                              public CValidationInterface
{
public:
    FlowMeshService(ChainstateManager& chainman, fs::path datadir);
    ~FlowMeshService();

    FlowMeshService(const FlowMeshService&) = delete;
    FlowMeshService& operator=(const FlowMeshService&) = delete;

    /** Start only when the complete A2/A3 schedule exists. */
    bool Start(PeerManager& peerman, std::string& error);
    /** Idempotent; joins all owned workers before returning. */
    void Stop();

    /**
     * Reconcile the current tip after block import caused initial download to
     * finish without a final non-IBD UpdatedBlockTip notification. The caller
     * must first wait for the import's validation-interface callbacks.
     */
    void ReconcileAfterInitialBlockDownload();

    bool Enabled() const;
    bool Running() const;

    std::vector<FlowMeshServiceMarket> Markets() const;
    std::optional<FlowMeshServiceMarket> Market(
        const flowmesh::MarketId& market_id) const;
    std::optional<FlowMeshRuntimeMarketStatus> MarketStatus(
        const flowmesh::MarketId& market_id) const;
    std::optional<flowmesh::FlowMeshState> StateSnapshot(
        const flowmesh::MarketId& market_id) const;

    bool SubmitLocalAction(const flowmesh::MarketId& market_id,
                           const flowmesh::Action& action,
                           std::string& error);
    bool ArmSeatKeys(std::vector<bls::SecretKey> keys, std::string& error);
    void DisarmSeatKeys();

    /** Earliest unconnected effect-bearing entry, fully encoded for MPA. */
    std::optional<FlowMeshPendingCheckpoint> NextCheckpointMpa(
        const flowmesh::MarketId& market_id, std::string& error) const;

    /** Resolve one connected, unconsumed effect into its exact type-9 proof. */
    std::optional<FlowMeshVaultOperation> VaultOperation(
        const uint256& effect_id, std::string& error) const;

    /**
     * Discover every currently actionable connected type-9 operation. This
     * is a chain-tip snapshot: withdrawals for the same market/asset may
     * select overlapping pool inputs, so publishers must confirm one and
     * refresh this list before constructing the next.
     */
    std::vector<FlowMeshVaultOperation> VaultOperations(
        std::optional<flowmesh::MarketId> market_id,
        std::string& error) const;

    flowmesh::QueueResult EnqueueWireMessage(
        flowmesh::WirePeerId peer,
        flowmesh::WireMessage message) override;
    void FlowMeshPeerDisconnected(flowmesh::WirePeerId peer) override;

private:
    void BlockDisconnected(const std::shared_ptr<const CBlock>& block,
                           const CBlockIndex* index) override;
    void UpdatedBlockTip(const CBlockIndex* new_tip,
                         const CBlockIndex* fork,
                         bool initial_download) override;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_SERVICE_H
