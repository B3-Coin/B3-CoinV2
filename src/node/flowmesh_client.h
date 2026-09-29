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
namespace flowmesh {
struct ClientEventPage;
} // namespace flowmesh
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
    //! One bounded live-update read of a market: at most `wait` spent waiting
    //! for a change the endpoint can report, then the same verified data
    //! Data() returns. Never signs, sends or resends an action. A zero wait
    //! is an ordinary read that also releases a remote client's watch lane.
    virtual std::optional<flowmesh::MarketData> Watch(const uint256& market, const std::optional<uint256>& account,
                                                    const flowmesh::MarketDataQuery& query, std::chrono::milliseconds wait,
                                                    interfaces::FlowMeshMarketWatch& watch, std::string& error)
    {
        watch = {};
        watch.status = "unsupported";
        return Data(market, account, query, error);
    }
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
//! Action-wait and watch slots together: each adds a sleeping worker, and the
//! HTTPS server's worker bound is never raised for them.
inline constexpr size_t FLOWMESH_API_MAX_WAIT_SLOTS{FLOWMESH_HTTPS_MAX_WORKER_THREADS - FLOWMESH_API_ACTIVE_PERMITS};
static_assert(FLOWMESH_API_ACTION_WAITERS_MAX <= FLOWMESH_API_MAX_WAIT_SLOTS);

//! Node-local latency policy for the restricted API's bounded 'updates' watch,
//! never consensus. A zero max or zero watchers disables it: no capability is
//! advertised and 'updates' keeps its exact previous request and reply
//! contract. A watched 'updates' read waits, holding a separate slot and no
//! HTTPS permit, until an event after the client's cursor exists for its
//! market and account filter, the head differs from its known head, or its
//! deadline; its reply is the ordinary event page and status object with no
//! reported_data.
struct FlowMeshTradingApiWatch {
    std::chrono::milliseconds max{0};
    size_t watchers{0};
};
inline constexpr std::chrono::milliseconds FLOWMESH_API_UPDATES_WATCH_MAX{2500};
//! Off until the pending-checkpoint memo has been reviewed and the cost of
//! concurrent watchers measured; 2000 ms is the intended enabled value.
inline constexpr std::chrono::milliseconds FLOWMESH_API_UPDATES_WATCH_DEFAULT{0};
inline constexpr size_t FLOWMESH_API_UPDATES_WATCHERS_DEFAULT{8};
inline constexpr size_t FLOWMESH_API_UPDATES_WATCHERS_MAX{10};
//! A watched 'updates' reply: at most CLIENT_EVENT_PAGE_MAX events and one
//! status object, never reported_data. The server refuses a larger one.
inline constexpr size_t FLOWMESH_API_WATCH_MAX_REPLY{512 * 1024};
/** Watch slots for `action_slots` action-wait slots: an explicitly requested
 * count (0..FLOWMESH_API_UPDATES_WATCHERS_MAX), refused when watches are
 * enabled and it exceeds the room the action waits leave; otherwise the
 * default, reduced to that room, so a configuration valid before watches
 * existed never fails to start. nullopt and `error` when refused. */
std::optional<size_t> FlowMeshApiWatchSlots(size_t action_slots, bool watch_enabled,
                                            std::optional<int64_t> requested, std::string& error);

//! Node-local policy for the restricted API's public 'health' method and the
//! advisory readiness keys (health_api_version, endpoint_ready,
//! endpoint_not_ready) in its status objects; never consensus or evidence.
//! Disabled keeps the exact previous request and reply contract: 'health'
//! is not exposed and status objects carry no readiness keys.
struct FlowMeshTradingApiHealth {
    bool enabled{false};
};
//! Per client IPv4 address or IPv6 /64, charged after the API's ordinary
//! admission budget; tighter than it.
inline constexpr unsigned FLOWMESH_API_HEALTH_PER_CLIENT_PER_SECOND{4};
//! The all-markets 'health' reply is rebuilt at most this often.
inline constexpr std::chrono::milliseconds FLOWMESH_API_HEALTH_CACHE{250};
inline constexpr const char* FLOWMESH_API_HEALTH_BUDGET_ERROR{"Public health request budget exhausted"};

//! Node-local policy for opt-in, verifiable action outcomes on the restricted
//! API: the "action_outcome_v1" advert in status objects, the 'action'
//! parameter "outcome" and the 'outcome_evidence' method. Disabled keeps the
//! exact previous request and reply contract. Evidence comes only from this
//! node's in-memory recent window, never from the store.
struct FlowMeshTradingApiOutcome {
    bool enabled{false};
};
inline constexpr const char* FLOWMESH_API_OUTCOME_BUDGET_ERROR{
    "Public trading request budget exhausted before outcome evidence"};

//! Restricted HTTPS adapter. It holds no wallet and never invokes the RPC table.
std::unique_ptr<FlowMeshHttpsServer> MakeFlowMeshTradingApi(
    FlowMeshService& service, FlowMeshHttpsServer::Options options,
    FlowMeshAssetMetadataCatalog metadata = {}, FlowMeshTradingApiWait wait = {},
    FlowMeshTradingApiHealth health = {}, FlowMeshTradingApiOutcome outcome = {},
    FlowMeshTradingApiWatch watch = {});

//! Shared public projection used by the wallet RPC and restricted endpoint.
UniValue FlowMeshClientMarketDataJson(const flowmesh::MarketData& data);
//! The event page part of an 'updates' reply (cursor, gap, more, event ids
//! and events), ordinary or watched.
UniValue FlowMeshClientUpdatesPageJson(const flowmesh::ClientEventPage& page);
//! PUBLIC_V1 writes exactly the previous receipt keys (updates, submit, a v1
//! 'action' reply, the outbox row). PUBLIC_OUTCOME_V1 (an opt-in 'action'
//! reply) adds reason_code when known; LOCAL_RPC adds it, the display stage
//! and the verified outcome.
enum class FlowMeshReceiptJsonScope : uint8_t { PUBLIC_V1, PUBLIC_OUTCOME_V1, LOCAL_RPC };
UniValue FlowMeshClientReceiptJson(const interfaces::FlowMeshActionReceipt& receipt,
                                   FlowMeshReceiptJsonScope scope = FlowMeshReceiptJsonScope::PUBLIC_V1);
//! The verified outcome object of a LOCAL_RPC receipt.
UniValue FlowMeshActionOutcomeJson(const interfaces::FlowMeshActionOutcome& outcome);
UniValue FlowMeshClientStatusJson(const interfaces::FlowMeshClientStatus& status);
} // namespace node
#endif
