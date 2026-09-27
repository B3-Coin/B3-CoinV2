// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_NODE_FLOWMESH_CLIENT_H
#define BITCOIN_NODE_FLOWMESH_CLIENT_H

#include <interfaces/chain.h>
#include <node/flowmesh_asset_metadata.h>
#include <node/flowmesh_client_join.h>
#include <node/flowmesh_https.h>
#include <univalue.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ChainstateManager;
namespace node {
class FlowMeshService;

struct FlowMeshClientReconnectResult {
    std::string status{"not_remote"};
    bool endpoint_available{false};
    std::string endpoint;
    size_t attempted_endpoints{0};
    std::string error{"The local trading backend does not use remote HTTPS endpoints"};
};

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
    //! A nonzero wait (only without retry) lets a remote backend hold one read
    //! of an action it just delivered open at the endpoint, bounded, until the
    //! action is certified there. It never resends or signs, and the receipt
    //! is never less conservative than an immediate read would return.
    virtual interfaces::FlowMeshActionReceipt ActionStatus(const uint256& market, const uint256& action, bool retry,
                                                           std::chrono::milliseconds wait = std::chrono::milliseconds{0}) = 0;
    virtual std::vector<interfaces::FlowMeshSavedAction> SavedActions(
        const uint256& account, const std::optional<uint256>& market) { return {}; }
    virtual interfaces::FlowMeshClientStatus Status() const = 0;
    //! Add/select a public HTTPS URL and probe with a read only request. A true
    //! result means configured; Status reports transport and API availability.
    virtual bool Connect(const std::string& url, std::string& error)
    {
        error = "Endpoint connections require the remote trading client; the local validator engine is enabled";
        return false;
    }
    //! Fresh read-only HTTPS availability probe. Never signs, retries an action,
    //! resets an outbox/cursor or changes configured trust. Busy fails promptly.
    virtual FlowMeshClientReconnectResult Reconnect() { return {}; }
    virtual std::optional<interfaces::FlowMeshPendingCheckpoint> Checkpoint(const uint256& market, std::string& error) = 0;
    virtual std::vector<interfaces::FlowMeshVaultOperation> VaultOperations(const std::optional<uint256>& market, std::string& error) = 0;
    virtual std::optional<interfaces::FlowMeshVaultOperation> VaultOperation(const uint256& effect, std::string& error) = 0;
};

std::unique_ptr<FlowMeshTradingBackend> MakeLocalFlowMeshBackend(
    FlowMeshService& service, FlowMeshAssetMetadataCatalog metadata = {});
//! join_windows are the wallet signing preflight's join windows; only the
//! regtest-only -flowmeshtestjoinwindowms passes anything but the defaults.
std::unique_ptr<FlowMeshTradingBackend> MakeRemoteFlowMeshBackend(
    ChainstateManager& chainman, std::vector<HttpsEndpoint> endpoints,
    const fs::path& client_datadir, std::string& error, const FlowMeshJoinWindows& join_windows = {});

//! Node-local latency policy for the restricted API's bounded 'action' wait,
//! never consensus. A zero max or zero waiters disables it: no capability is
//! advertised and 'action' keeps its exact previous request contract.
struct FlowMeshTradingApiWait {
    std::chrono::milliseconds max{0};
    size_t waiters{0};
};
inline constexpr std::chrono::milliseconds FLOWMESH_API_ACTION_WAIT_MAX{2500};
inline constexpr std::chrono::milliseconds FLOWMESH_API_ACTION_WAIT_DEFAULT{2000};
inline constexpr size_t FLOWMESH_API_ACTION_WAITERS_DEFAULT{4};
inline constexpr size_t FLOWMESH_API_ACTION_WAITERS_MAX{12};
//! Connections doing HTTPS work at once, as with the API's two workers before
//! waits existed; each wait slot adds a worker that only sleeps in a wait.
inline constexpr size_t FLOWMESH_API_ACTIVE_PERMITS{2};

//! Restricted HTTPS adapter. It holds no wallet and never invokes the RPC table.
std::unique_ptr<FlowMeshHttpsServer> MakeFlowMeshTradingApi(
    FlowMeshService& service, FlowMeshHttpsServer::Options options,
    FlowMeshAssetMetadataCatalog metadata = {}, FlowMeshTradingApiWait wait = {});

//! Shared public projection used by the wallet RPC and restricted endpoint.
UniValue FlowMeshClientMarketDataJson(const flowmesh::MarketData& data);
UniValue FlowMeshClientReceiptJson(const interfaces::FlowMeshActionReceipt& receipt);
UniValue FlowMeshClientStatusJson(const interfaces::FlowMeshClientStatus& status);
} // namespace node
#endif
