// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_NODE_FLOWMESH_CLIENT_H
#define BITCOIN_NODE_FLOWMESH_CLIENT_H

#include <interfaces/chain.h>
#include <node/flowmesh_asset_metadata.h>
#include <node/flowmesh_https.h>
#include <univalue.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

class ChainstateManager;
namespace node {
class FlowMeshService;

//! Trading-only boundary. No wallet secrets, validator arming, RPC dispatcher,
//! execution worker or signing-history mutation is part of this interface.
class FlowMeshTradingBackend {
public:
    virtual ~FlowMeshTradingBackend() = default;
    //! Bounded in-memory lookup; safe under a wallet lock, never waits for HTTP.
    virtual std::optional<modern::AssetDisplayMetadata> Metadata(const uint256& asset) const { return std::nullopt; }
    virtual uint64_t MetadataGeneration() const { return 0; }
    virtual std::vector<interfaces::FlowMeshMarketStatus> Markets(const std::optional<uint256>& account) = 0;
    virtual std::optional<interfaces::FlowMeshMarketStatus> Market(const uint256& market, const std::optional<uint256>& account) = 0;
    virtual std::optional<flowmesh::MarketData> Data(const uint256& market, const std::optional<uint256>& account,
                                                   const flowmesh::MarketDataQuery& query, std::string& error) = 0;
    virtual interfaces::FlowMeshActionReceipt Submit(const uint256& market, const flowmesh::Action& action) = 0;
    virtual interfaces::FlowMeshActionReceipt ActionStatus(const uint256& market, const uint256& action, bool retry) = 0;
    virtual std::vector<interfaces::FlowMeshSavedAction> SavedActions(
        const uint256& account, const std::optional<uint256>& market) { return {}; }
    virtual interfaces::FlowMeshClientStatus Status() const = 0;
    virtual std::optional<interfaces::FlowMeshPendingCheckpoint> Checkpoint(const uint256& market, std::string& error) = 0;
    virtual std::vector<interfaces::FlowMeshVaultOperation> VaultOperations(const std::optional<uint256>& market, std::string& error) = 0;
    virtual std::optional<interfaces::FlowMeshVaultOperation> VaultOperation(const uint256& effect, std::string& error) = 0;
};

std::unique_ptr<FlowMeshTradingBackend> MakeLocalFlowMeshBackend(
    FlowMeshService& service, FlowMeshAssetMetadataCatalog metadata = {});
std::unique_ptr<FlowMeshTradingBackend> MakeRemoteFlowMeshBackend(
    ChainstateManager& chainman, std::vector<HttpsEndpoint> endpoints,
    const fs::path& client_datadir, std::string& error);

//! Restricted HTTPS adapter. It holds no wallet and never invokes the RPC table.
std::unique_ptr<FlowMeshHttpsServer> MakeFlowMeshTradingApi(
    FlowMeshService& service, FlowMeshHttpsServer::Options options,
    FlowMeshAssetMetadataCatalog metadata = {});

//! Shared public projection used by the wallet RPC and restricted endpoint.
UniValue FlowMeshClientMarketDataJson(const flowmesh::MarketData& data);
UniValue FlowMeshClientReceiptJson(const interfaces::FlowMeshActionReceipt& receipt);
UniValue FlowMeshClientStatusJson(const interfaces::FlowMeshClientStatus& status);
} // namespace node
#endif
