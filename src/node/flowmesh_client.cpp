// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <node/flowmesh_timing.h>
#include <node/flowmesh_client.h>
#include <node/flowmesh_action_wait.h>
#include <node/flowmesh_client_action_wait.h>
#include <node/flowmesh_client_join.h>
#include <node/flowmesh_client_poll.h>
#include <node/flowmesh_client_work.h>
#include <node/flowmesh_client_settlement.h>

#include <chain.h>
#include <consensus/flowmesh_params.h>
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <dbwrapper.h>
#include <flowmesh/auth.h>
#include <flowmesh/client_evidence.h>
#include <flowmesh/production_wire.h>
#include <hash.h>
#include <modern/chain_domain.h>
#include <modern/mpa.h>
#include <modern/flowmesh_vault_proof.h>
#include <node/blockstorage.h>
#include <node/flowmesh_checkpoint_index.h>
#include <node/flowmesh_service.h>
#include <node/flowmesh_vault_index.h>
#include <node/fn_seat_index.h>
#include <streams.h>
#include <sync.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <util/int128.h>
#include <util/log.h>
#include <validation.h>

#ifdef FLOWMESH_CLIENT_CRASH_TEST_HOOKS
#include <test/flowmesh_client_crash_hook.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace node {
namespace {
using Receipt = interfaces::FlowMeshActionReceipt;
using MarketStatus = interfaces::FlowMeshMarketStatus;
constexpr size_t CLIENT_MAX_REPLY{24 * 1024 * 1024};
constexpr size_t CLIENT_MAX_MARKETS{256};
constexpr size_t CLIENT_MAX_CACHED_MARKETS{8};
constexpr size_t CLIENT_MAX_ACTIONS{512};
constexpr size_t CLIENT_MAX_ENDPOINTS{8};
constexpr auto CLIENT_REQUEST_TIMEOUT{std::chrono::seconds{5}};
// A waited 'action' reply carries at most one hex certified entry.
constexpr size_t CLIENT_ACTION_WAIT_MAX_REPLY{2 * (flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_BYTES + 1024) + 64 * 1024};
static_assert(CLIENT_ACTION_WAIT_MAX_REPLY <= CLIENT_MAX_REPLY);
// A waited 'action' read ends at least this long before its HTTPS deadline,
// leaving time to fetch the certified payload and write the reply.
constexpr auto API_WAIT_REPLY_RESERVE{std::chrono::milliseconds{1000}};

[[noreturn]] void Fail(const std::string& reason) { throw std::runtime_error(reason); }
void Keys(const UniValue& value, std::initializer_list<const char*> allowed)
{
    if (!value.isObject()) Fail("Expected a bounded object");
    std::set<std::string> seen;
    for (const auto& key : value.getKeys()) {
        if (!seen.insert(key).second || std::none_of(allowed.begin(), allowed.end(), [&](const char* name) { return key == name; }))
            Fail("Unknown or duplicate client API field");
    }
}
std::string Text(const UniValue& object, const char* key, size_t maximum = 1024)
{
    const auto& value{object[key]};
    if (!value.isStr() || value.get_str().size() > maximum) Fail(std::string{"Invalid text: "} + key);
    return value.get_str();
}
uint256 Id(const UniValue& object, const char* key, bool zero = false)
{
    const auto text{Text(object, key, 64)};
    const auto hash{uint256::FromHex(text)};
    if (text.size() != 64 || !hash || (!zero && hash->IsNull())) Fail(std::string{"Invalid identifier: "} + key);
    return *hash;
}
uint64_t Number(const UniValue& object, const char* key, uint64_t maximum = INT64_MAX)
{
    if (!object[key].isNum()) Fail(std::string{"Expected integer: "} + key);
    const auto value{object[key].getInt<uint64_t>()};
    if (value > maximum) Fail(std::string{"Integer exceeds client bound: "} + key);
    return value;
}
bool Flag(const UniValue& object, const char* key)
{
    if (!object[key].isBool()) Fail(std::string{"Expected boolean: "} + key);
    return object[key].get_bool();
}
std::vector<unsigned char> Bytes(const UniValue& object, const char* key, size_t maximum)
{
    const auto text{Text(object, key, maximum * 2)};
    if (text.empty() || text.size() % 2 || !IsHex(text)) Fail(std::string{"Invalid bounded bytes: "} + key);
    return ParseHex(text);
}
UniValue CursorJson(const flowmesh::ClientEventCursor& cursor)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("instance_id", cursor.instance_id.GetHex());
    out.pushKV("event_id", cursor.event_id);
    return out;
}
flowmesh::ClientEventCursor ParseCursor(const UniValue& row)
{
    Keys(row, {"instance_id", "event_id"});
    return {Id(row, "instance_id"), Number(row, "event_id")};
}
Receipt EventReceipt(const flowmesh::ClientEvent& event)
{
    Receipt out;
    out.action_id = event.action_id;
    if (!event.account_id.IsNull()) out.account_id = event.account_id;
    out.reason = event.reason;
    switch (event.kind) {
    case flowmesh::ClientEventKind::QUEUE_ADMITTED: out.state = "queued"; break;
    case flowmesh::ClientEventKind::POOL_ADMITTED: out.state = "admitted"; break;
    case flowmesh::ClientEventKind::POOL_REFUSED: out.state = "rejected"; break;
    case flowmesh::ClientEventKind::CERTIFIED_INCLUDED:
        out.state = "certified_inclusion";
        out.microblock_hash = event.microblock_hash;
        out.microblock_sequence = event.microblock_sequence;
        break;
    case flowmesh::ClientEventKind::CERTIFIED_HEAD: break;
    }
    return out;
}
Receipt LocalActionReceipt(const std::optional<flowmesh::ClientEvent>& status, const uint256& action)
{
    Receipt out; out.action_id = action;
    if (status) out = EventReceipt(*status);
    if (out.state == "certified_inclusion") out.certificate_verified = true;
    return out;
}

class LocalBackend final : public FlowMeshTradingBackend {
    FlowMeshService& m_service;
    const FlowMeshAssetMetadataCatalog m_metadata;
public:
    explicit LocalBackend(FlowMeshService& service, FlowMeshAssetMetadataCatalog metadata = {})
        : m_service(service), m_metadata(std::move(metadata)) {}
    std::optional<modern::AssetDisplayMetadata> Metadata(const uint256& asset) const override
    {
        const auto it{m_metadata.find(asset)};
        if (it == m_metadata.end()) return std::nullopt;
        auto out{it->second};
        out.source = "operator-public-catalog";
        return out;
    }
    uint64_t MetadataGeneration() const override { return m_metadata.empty() ? 0 : 1; }
    std::vector<MarketStatus> Markets(const std::optional<uint256>& account) override
    {
        std::vector<MarketStatus> out;
        for (const auto& market : m_service.Markets()) {
            if (out.size() == CLIENT_MAX_MARKETS) break;
            if (auto status{Market(market.market_id, account)}) out.push_back(std::move(*status));
        }
        return out;
    }
    std::optional<MarketStatus> Market(const uint256& id, const std::optional<uint256>& account) override
    {
        const auto market{m_service.Market(id)};
        if (!market) return std::nullopt;
        MarketStatus out;
        out.available = m_service.Enabled(); out.running = m_service.Running();
        out.domain = market->domain; out.market_id = id; out.vault_id = market->vault_id;
        out.base_asset = market->base_asset; out.quote_asset = market->quote_asset;
        out.execution_config_id = market->execution_config_id;
        if (const auto runtime{m_service.MarketStatus(id)}) {
            out.epoch = runtime->epoch; out.next_microblock_sequence = runtime->next_sequence;
            out.next_effect_index = runtime->next_effect_index; out.round = runtime->round;
            out.last_microblock_hash = runtime->last_microblock_hash; out.state_root = runtime->state_root;
            out.pending_actions = runtime->pending_actions; out.observer_only = runtime->observer_only;
            out.paused = runtime->paused; out.pending_handoff = runtime->pending_handoff;
            out.halt = FlowMeshRuntimeHaltName(runtime->halt); out.error = runtime->error;
            out.certificate_verified = runtime->next_sequence != 0;
        }
        std::string error;
        if (const auto checkpoint{m_service.NextCheckpointMpa(id, error)}) {
            out.checkpoint_pending = true; out.pending_checkpoint_id = checkpoint->checkpoint_id;
            out.pending_checkpoint_sequence = checkpoint->sequence;
            out.pending_checkpoint_effect_count = checkpoint->effect_count;
        }
        if (account) {
            out.account_id = account;
            if (const auto state{m_service.StateSnapshot(id)}) {
                out.next_account_sequence = state->NextSequence(*account); out.slot = state->Slot();
                out.base_available = state->LedgerView().Available(*account, market->base_asset);
                out.base_reserved = state->LedgerView().Reserved(*account, market->base_asset);
                out.b3_available = state->LedgerView().Available(*account, modern::NativeAsset());
                out.b3_reserved = state->LedgerView().Reserved(*account, modern::NativeAsset());
                out.account_state_verified = out.certificate_verified;
            }
        }
        return out;
    }
    std::optional<flowmesh::MarketData> Data(const uint256& id, const std::optional<uint256>& account,
                                          const flowmesh::MarketDataQuery& query, std::string& error) override
    {
        auto out{m_service.MarketData(id, account, query, error)};
        if (out) { out->certificate_verified = out->snapshot.certified; out->account_state_verified = out->snapshot.certified; out->execution_result_verified = out->snapshot.certified; }
        return out;
    }
    Receipt Submit(const uint256& id, const flowmesh::Action& action) override
    {
        Receipt out; out.action_id = action.Id();
        if (!m_service.SubmitLocalAction(id, action, out.reason)) { out.state = "rejected"; return out; }
        out.state = "queued";
        if (const auto status{m_service.ClientActionStatus(id, action.Id())}) {
            const auto exact{flowmesh::EncodeProductionActionPayload(action)};
            // A refusal belongs to one exact credential-bearing attempt.
            // Do not attribute an older attempt's refusal to a new queue item.
            if (status->kind != flowmesh::ClientEventKind::POOL_REFUSED ||
                (exact && status->signed_action_hash == Hash(*exact))) out = EventReceipt(*status);
        }
        if (out.state == "certified_inclusion") out.certificate_verified = true;
        return out;
    }
    // The local engine's status is already current: there is no transport to
    // hide, so a requested wait is ignored and the read returns immediately.
    Receipt ActionStatus(const uint256& id, const uint256& action, bool retry,
                         std::chrono::milliseconds wait = std::chrono::milliseconds{0}) override
    {
        auto out{LocalActionReceipt(m_service.ClientActionStatus(id, action), action)};
        if (retry) out.reason += "; local retry requires the retained original signed action";
        return out;
    }
    interfaces::FlowMeshClientStatus Status() const override
    {
        interfaces::FlowMeshClientStatus out; out.engine_enabled = true; return out;
    }
    std::optional<interfaces::FlowMeshPendingCheckpoint> Checkpoint(const uint256& id, std::string& error) override
    {
        const auto value{m_service.NextCheckpointMpa(id, error)};
        if (!value) return std::nullopt;
        return interfaces::FlowMeshPendingCheckpoint{value->record, value->checkpoint_id, value->sequence, value->effect_count};
    }
    static interfaces::FlowMeshVaultOperation Convert(const node::FlowMeshVaultOperation& value)
    {
        interfaces::FlowMeshVaultOperation out;
        out.market_id = value.market_id; out.checkpoint_id = value.checkpoint_id;
        out.record = value.record; out.effect = value.effect;
        for (const auto& input : value.inputs) out.inputs.push_back({input.record.outpoint, input.txout});
        return out;
    }
    std::vector<interfaces::FlowMeshVaultOperation> VaultOperations(const std::optional<uint256>& id, std::string& error) override
    {
        std::vector<interfaces::FlowMeshVaultOperation> out;
        for (const auto& value : m_service.VaultOperations(id, error)) {
            if (out.size() >= 128) { error = "Vault-operation page exceeds the client bound"; return {}; }
            out.push_back(Convert(value));
        }
        return out;
    }
    std::optional<interfaces::FlowMeshVaultOperation> VaultOperation(const uint256& id, std::string& error) override
    {
        const auto value{m_service.VaultOperation(id, error)};
        return value ? std::optional{Convert(*value)} : std::nullopt;
    }
};

UniValue MarketJson(const MarketStatus& status)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("market_id", status.market_id.GetHex()); out.pushKV("domain", status.domain.GetHex());
    out.pushKV("base_asset_id", status.base_asset.GetHex()); out.pushKV("vault_id", status.vault_id.GetHex());
    out.pushKV("execution_config_id", status.execution_config_id.GetHex()); out.pushKV("quote_asset", "B3");
    out.pushKV("available", status.available); out.pushKV("running", status.running);
    out.pushKV("paused", status.paused); out.pushKV("pending_handoff", status.pending_handoff);
    out.pushKV("halt", status.halt); out.pushKV("error", status.error);
    out.pushKV("next_microblock_sequence", status.next_microblock_sequence);
    out.pushKV("last_microblock_hash", status.last_microblock_hash.GetHex());
    out.pushKV("epoch", status.epoch); out.pushKV("round", status.round);
    out.pushKV("pending_actions", uint64_t{status.pending_actions});
    out.pushKV("checkpoint_pending", status.checkpoint_pending);
    if (status.checkpoint_pending) {
        out.pushKV("pending_checkpoint_id", status.pending_checkpoint_id.GetHex());
        out.pushKV("pending_checkpoint_sequence", status.pending_checkpoint_sequence);
        out.pushKV("pending_checkpoint_effect_count", status.pending_checkpoint_effect_count);
    }
    return out;
}
UniValue VaultJson(const interfaces::FlowMeshVaultOperation& op)
{
    // The remote client derives the effect from this existing authenticated
    // proof and selects its inputs from local B3 UTXOs, not this server's rows.
    UniValue out{UniValue::VOBJ};
    out.pushKV("market_id", op.market_id.GetHex()); out.pushKV("checkpoint_id", op.checkpoint_id.GetHex());
    out.pushKV("proof", HexStr(op.record.payload));
    return out;
}

class TradingApi {
    FlowMeshService& m_service;
    LocalBackend m_local;
    // Zero when the bounded 'action' wait is disabled.
    const std::chrono::milliseconds m_wait_max;
    FlowMeshActionWaitSlots m_waits;
    std::mutex m_rate_mutex;
    struct Bucket { std::chrono::steady_clock::time_point time{}; unsigned count{0}; };
    std::map<std::string, Bucket> m_clients;
    unsigned m_global_count{0};
    std::chrono::steady_clock::time_point m_global_time{};
    UniValue MarketResponse(const MarketStatus& status) const
    {
        auto out{MarketJson(status)};
        if (const auto metadata{m_local.Metadata(status.base_asset)}) {
            out.pushKV("asset_display", FlowMeshAssetMetadataJson(status.domain, status.base_asset, *metadata));
        }
        // Latency capability, never evidence: clients send wait_ms only to an
        // endpoint that advertises it, since older servers reject the field.
        if (m_wait_max.count()) out.pushKV("action_wait_ms_max", uint64_t(m_wait_max.count()));
        return out;
    }
    UniValue MarketResponse(const flowmesh::MarketData& data) const
    {
        // Match all head/runtime fields to the captured projection. Looking
        // up Market() here would race a newly committed microblock and mix
        // its newer status with the old certified payload/history.
        MarketStatus status;
        status.available = m_service.Enabled();
        status.domain = data.domain; status.market_id = data.market_id;
        const auto vault{flowmesh::ComputeFlowMeshVaultId(data.domain, data.market_id)};
        if (!vault) Fail("Invalid public market identity");
        status.vault_id = *vault; status.base_asset = data.base_asset_id;
        status.quote_asset = modern::NativeAsset(); status.execution_config_id = data.execution_config_id;
        const auto& snapshot{data.snapshot};
        status.running = snapshot.running; status.paused = snapshot.paused;
        status.pending_handoff = snapshot.pending_handoff; status.halt = snapshot.halt;
        status.error = snapshot.error; status.epoch = snapshot.epoch;
        status.next_microblock_sequence = snapshot.next_microblock_sequence;
        status.last_microblock_hash = snapshot.last_microblock_hash;
        status.state_root = snapshot.state_root; status.round = snapshot.runtime.round;
        status.pending_actions = snapshot.pending_actions;
        // Pending checkpoint is a separate service observation, never used
        // as a substitute for this exact certified snapshot.
        std::string error;
        if (const auto checkpoint{m_service.NextCheckpointMpa(data.market_id, error)}) {
            status.checkpoint_pending = true; status.pending_checkpoint_id = checkpoint->checkpoint_id;
            status.pending_checkpoint_sequence = checkpoint->sequence;
            status.pending_checkpoint_effect_count = checkpoint->effect_count;
        }
        auto out{MarketResponse(status)};
        // Keep this transient service observation with the captured response.
        // It is not part of the certified entry or authenticated account state.
        out.pushKV("chain_reconciling", snapshot.chain_reconciling);
        return out;
    }
    bool Admit(const std::string& peer)
    {
        // Charged before decoding/authentication and before the operator's
        // local-action queue. Bounded identities, requests and global rate.
        std::lock_guard lock(m_rate_mutex);
        const auto now{std::chrono::steady_clock::now()};
        if (now - m_global_time >= std::chrono::seconds{1}) { m_global_time = now; m_global_count = 0; }
        if (++m_global_count > 128) return false;
        if (!m_clients.contains(peer) && m_clients.size() >= 1024) {
            std::erase_if(m_clients, [&](const auto& pair) { return now - pair.second.time > std::chrono::minutes{1}; });
            if (m_clients.size() >= 1024) return false;
        }
        auto& bucket{m_clients[peer]};
        if (now - bucket.time >= std::chrono::seconds{1}) { bucket.time = now; bucket.count = 0; }
        return ++bucket.count <= 32;
    }
    //! Bounded wait for the exact action's terminal status on this node.
    //! Returns the wait_status label; replaces receipt only after a wait.
    std::string WaitForAction(const FlowMeshHttpsServer::Request& http, const uint256& market,
                              const uint256& action_id, const bool tracked, const std::chrono::milliseconds wait,
                              Receipt& receipt, FlowMeshTimingSpan& timing)
    {
        // Only for an action this node already recorded ('submit' appends its
        // queue event before it replies), so made-up ActionIds cannot hold
        // slots. Public ones can: 'updates' lists every retained event's
        // action_id, so any client may wait on other clients' non-terminal
        // actions and a few addresses can hold every slot. That only turns
        // other waited reads into an immediate 'busy' reply (as without
        // waits); it never delays or changes any reply's receipt.
        if (!tracked) return "untracked";
        const auto now{std::chrono::steady_clock::now()};
        const auto until{std::min(now + wait, http.deadline - API_WAIT_REPLY_RESERVE)};
        if (until <= now) return "deadline";
        auto slot{m_waits.TryAcquire(http.remote_address)};
        if (!slot) return "busy";
        timing.Mark("wait_started_us");
        // Asleep, this request holds no HTTPS permit, so handshake, read and
        // write concurrency stays at its bound. The server also resumes a
        // handler that leaves this wait by an exception.
        if (http.suspend) http.suspend();
        const auto result{m_service.WaitClientActionStatus(market, action_id, until,
            [&] { return m_waits.Interrupted(*slot); })};
        if (http.resume) http.resume();
        timing.Mark("wait_completed_us");
        receipt = LocalActionReceipt(result.status, action_id);
        return flowmesh::ClientWaitResultName(result.result);
    }
public:
    explicit TradingApi(FlowMeshService& service, FlowMeshAssetMetadataCatalog metadata, const FlowMeshTradingApiWait wait)
        : m_service(service), m_local(service, std::move(metadata)),
          m_wait_max{wait.waiters == 0 ? std::chrono::milliseconds{0}
                                       : std::clamp(wait.max, std::chrono::milliseconds{0}, FLOWMESH_API_ACTION_WAIT_MAX)},
          m_waits{m_wait_max.count() ? std::min(wait.waiters, FLOWMESH_API_ACTION_WAITERS_MAX) : 0} {}
    size_t WaitSlots() const { return m_waits.Capacity(); }
    //! Server stop hook: refuse new waits and end every current one.
    void Interrupt()
    {
        m_waits.Close();
        m_service.WakeClientWaiters();
    }
    //! Server start hook.
    void Reopen() { m_waits.Open(); }
    UniValue Call(const UniValue& request, const FlowMeshHttpsServer::Request& http)
    {
        FlowMeshTimingSpan timing{"https_server_api"};
        Keys(request, {"method", "params"});
        const std::string method{Text(request, "method", 32)};
        const auto& params{request["params"]};
        timing.Field("method", method);
        for (const char* key : {"market_id", "action_id", "account_id"})
            if (timing.Enabled() && params[key].isStr()) timing.Field(key, params[key].get_str());
        if (method == "markets") {
            Keys(params, {});
            UniValue out{UniValue::VARR};
            for (const auto& value : m_local.Markets(std::nullopt)) out.push_back(MarketResponse(value));
            return out;
        }
        if (method == "snapshot") {
            Keys(params, {"market_id", "account_id"});
            const auto id{Id(params, "market_id")};
            const std::optional<uint256> account{params.exists("account_id") ? std::optional{Id(params, "account_id")} : std::nullopt};
            std::string error;
            const auto view{m_service.ClientSnapshotView(id, account, error)};
            if (!view) Fail(error.empty() ? "No certified state snapshot is available" : error);
            const auto& evidence{view->evidence};
            UniValue out{UniValue::VOBJ};
            out.pushKV("status", MarketResponse(view->reported));
            out.pushKV("certified_payload", HexStr(evidence.certified_payload));
            out.pushKV("state_bytes", HexStr(evidence.state_bytes));
            out.pushKV("cursor", CursorJson(evidence.cursor));
            // Unauthenticated projections are explicitly separate. The client
            // derives its balance/book from state_bytes instead of these rows.
            out.pushKV("reported_data", FlowMeshClientMarketDataJson(view->reported));
            return out;
        }
        if (method == "updates") {
            Keys(params, {"market_id", "account_id", "cursor", "known_head", "before_sequence", "limit"});
            const auto id{Id(params, "market_id")};
            const std::optional<uint256> account{params.exists("account_id") ? std::optional{Id(params, "account_id")} : std::nullopt};
            const std::optional<flowmesh::ClientEventCursor> after{params.exists("cursor") ? std::optional{ParseCursor(params["cursor"])} : std::nullopt};
            const auto page{m_service.ClientEvents(after, id, account, flowmesh::CLIENT_EVENT_PAGE_MAX)};
            UniValue out{UniValue::VOBJ};
            out.pushKV("cursor", CursorJson(page.cursor));
            out.pushKV("gap", page.gap); out.pushKV("more", page.more);
            out.pushKV("oldest_event_id", page.oldest_event_id); out.pushKV("latest_event_id", page.latest_event_id);
            UniValue events{UniValue::VARR};
            for (const auto& event : page.events) {
                UniValue item{FlowMeshClientReceiptJson(EventReceipt(event))};
                item.pushKV("event_id", event.event_id); item.pushKV("kind", flowmesh::ClientEventKindName(event.kind));
                item.pushKV("market_id", event.market_id.GetHex());
                item.pushKV("signed_action_hash", event.signed_action_hash.GetHex());
                events.push_back(std::move(item));
            }
            out.pushKV("events", std::move(events));
            flowmesh::MarketDataQuery query;
            if (params.exists("known_head")) query.known_head = Id(params, "known_head", true);
            if (params.exists("before_sequence")) { query.before_sequence = Number(params, "before_sequence"); query.known_head.reset(); }
            if (params.exists("limit")) query.limit = Number(params, "limit", flowmesh::MARKET_DATA_MAX_HISTORY);
            std::string error;
            const auto data{m_local.Data(id, account, query, error)};
            if (!data) Fail(error.empty() ? "No market-data view is available" : error);
            out.pushKV("status", MarketResponse(*data));
            out.pushKV("reported_data", FlowMeshClientMarketDataJson(*data));
            return out;
        }
        if (method == "submit") {
            Keys(params, {"market_id", "action_id", "action_hex"});
            const auto id{Id(params, "market_id")};
            const auto action_id{Id(params, "action_id")};
            const auto payload{Bytes(params, "action_hex", flowmesh::FLOWMESH_ACTION_MAX_BYTES)};
            const auto action{flowmesh::DecodeProductionActionPayload(payload)};
            if (!action || action->Id() != action_id) Fail("Invalid exact signed action or ActionId");
            if (action->type == static_cast<uint8_t>(flowmesh::ActionType::CLAIM_SEAT_REWARD)) Fail("Operator reward claims are not a public trading operation");
            const auto market{m_service.Market(id)};
            if (!market) Fail("Unknown market");
            if (!action->IsDeposit() && !flowmesh::SchnorrActionAuthenticator(market->domain, market->execution_config_id).Authenticate(*action))
                Fail("Action authentication failed for this chain/domain/market configuration");
            return FlowMeshClientReceiptJson(m_local.Submit(id, *action));
        }
        if (method == "action") {
            // Disabled, the contract is exactly the previous one, in which
            // wait_ms is an unknown field.
            if (m_wait_max.count() == 0) Keys(params, {"market_id", "action_id"});
            else Keys(params, {"market_id", "action_id", "wait_ms"});
            const auto id{Id(params, "market_id")};
            const auto action_id{Id(params, "action_id")};
            const bool wait_requested{params.exists("wait_ms")};
            // Any unsigned integer is clamped to the advertised maximum.
            const std::chrono::milliseconds wait{wait_requested
                ? std::min<uint64_t>(Number(params, "wait_ms", std::numeric_limits<uint64_t>::max()), m_wait_max.count())
                : 0};
            // Same receipt as the local backend's ActionStatus(id, action, false).
            const auto status{m_service.ClientActionStatus(id, action_id)};
            auto receipt{LocalActionReceipt(status, action_id)};
            std::string wait_status{"none"};
            if (wait.count() > 0 && receipt.state != "certified_inclusion") {
                wait_status = WaitForAction(http, id, action_id, status.has_value(), wait, receipt, timing);
            }
            UniValue out{FlowMeshClientReceiptJson(receipt)};
            if (receipt.state == "certified_inclusion") {
                // Never built from the event: this waits for the commit's
                // market lock and reads the durable certified entry.
                std::string error;
                if (const auto payload{m_service.ClientCertifiedEntry(id, receipt.microblock_sequence, error)}) out.pushKV("certified_payload", HexStr(*payload));
                else out.pushKV("evidence_error", error);
            }
            if (wait_requested) {
                // A latency hint only, never evidence; absent unless requested.
                out.pushKV("wait_status", wait_status);
                timing.Field("wait_status", wait_status);
            }
            return out;
        }
        if (method == "checkpoint") {
            Keys(params, {"market_id"});
            std::string error;
            const auto value{m_local.Checkpoint(Id(params, "market_id"), error)};
            if (!value) Fail(error.empty() ? "No pending checkpoint" : error);
            UniValue out{UniValue::VOBJ};
            out.pushKV("payload", HexStr(value->record.payload));
            return out;
        }
        if (method == "vault_operations") {
            Keys(params, {"market_id"});
            std::string error;
            const auto values{m_local.VaultOperations(params.exists("market_id") ? std::optional{Id(params, "market_id")} : std::nullopt, error)};
            if (!error.empty()) Fail(error);
            UniValue out{UniValue::VARR};
            for (const auto& value : values) out.push_back(VaultJson(value));
            return out;
        }
        if (method == "vault_operation") {
            Keys(params, {"effect_id"});
            std::string error;
            const auto value{m_local.VaultOperation(Id(params, "effect_id"), error)};
            if (!value) Fail(error.empty() ? "No connected unconsumed effect" : error);
            return VaultJson(*value);
        }
        Fail("This method is not exposed by the public trading API");
    }
    FlowMeshHttpsServer::Response Handle(const FlowMeshHttpsServer::Request& request)
    {
        try {
            if (!Admit(request.remote_address)) return {429, "{\"ok\":false,\"error\":\"Public trading request budget exhausted before admission\"}"};
            UniValue parsed;
            if (!parsed.read(request.body)) Fail("Malformed public trading request");
            UniValue out{UniValue::VOBJ};
            out.pushKV("ok", true); out.pushKV("result", Call(parsed, request));
            auto body{out.write()};
            if (body.size() > CLIENT_MAX_REPLY) Fail("Public trading response exceeds snapshot bound");
            return {200, std::move(body)};
        } catch (const std::exception& e) {
            UniValue out{UniValue::VOBJ}; out.pushKV("ok", false); out.pushKV("error", std::string(e.what()).substr(0, 1024));
            return {400, out.write()};
        }
    }
};

// Client-local durable public objects only. This is neither the validator
// journal nor an execution-history store. A corrupt outbox fails closed.
struct ClientJournalBlob {
    std::string json;
    SERIALIZE_METHODS(ClientJournalBlob, obj) { READWRITE(LIMITED_STRING(obj.json, 8 * 1024 * 1024)); }
};

// Kept separately from signed instructions and verified high-water marks.
// Only user-added public origins are stored; trust material stays in config.
struct ClientEndpointBlob {
    std::string json;
    SERIALIZE_METHODS(ClientEndpointBlob, obj) { READWRITE(LIMITED_STRING(obj.json, 32 * 1024)); }
};

class RemoteBackend final : public FlowMeshTradingBackend {
    ChainstateManager& m_chainman;
    std::vector<HttpsEndpoint> m_endpoints;
    std::vector<std::string> m_saved_endpoints;
    std::vector<std::chrono::steady_clock::time_point> m_retry_after;
    const fs::path m_path;
    // Network waits never hold cs_main, a wallet lock or an operator lock.
    // Retain one owner across network/cache/outbox work. Explicit requests
    // take priority over queued passive refreshes, never preempt an owner;
    // after an explicit request the idle gate briefly waits for its next step.
    // Status remains nonblocking; metadata uses its separate lock below.
    FlowMeshClientWorkGate m_work;
    // One TLS connection, owned exclusively under m_work. Reuse changes only
    // transport setup, not economic retry, state ownership or durability.
    FlowMeshHttpsClient m_https;
    // Waited status reads use their own TLS connection so that m_work is not
    // held across a server-side wait. m_wait_lane is only ever try-locked, and
    // never while holding m_work; a busy lane means an ordinary status read.
    std::mutex m_wait_lane;
    FlowMeshHttpsClient m_wait_https;
    // Set where m_https is reset; the lane revalidates trust before its next use.
    std::atomic<bool> m_wait_reset{false};
    // Per endpoint (index-stable, like m_retry_after), under m_work: the bounded
    // 'action' wait each endpoint last advertised. Zero never sends wait_ms.
    std::vector<std::chrono::milliseconds> m_action_wait;
    // Per endpoint, under m_work: a lane request to it got no HTTP response.
    // The lane skips it until EndpointResult records a successful response
    // from it (an ordinary read or probe), so a silent endpoint costs a
    // waited call its lane time once, not on every call.
    std::vector<bool> m_wait_unanswered;
    // Never acquire m_work from a wallet metadata lookup: it covers HTTPS.
    mutable std::mutex m_metadata_mutex;
    FlowMeshAssetMetadataCatalog m_metadata;
    std::atomic<uint64_t> m_metadata_generation{0};
    mutable std::mutex m_status_mutex;
    interfaces::FlowMeshClientStatus m_status;
    size_t m_selected{0};
    // Read preference survives successful failover, so a recovered preferred
    // service is tried again after its transport cooldown expires.
    size_t m_preferred{0};
    std::unique_ptr<CDBWrapper> m_db;
    struct Pending {
        uint256 market, domain, config;
        flowmesh::Action action;
        std::vector<unsigned char> bytes;
        int64_t initial_submission_ms{0};
        bool may_have_been_sent{false};
        Receipt receipt;
        // Durable replay prohibition survives restart even when an endpoint
        // has expired its event history. Fresh proof is required to renew the
        // verification label, never to justify sending this instruction again.
        bool previously_certified{false};
        std::vector<unsigned char> inclusion_proof;
        uint256 owner_account;
        // Volatile read failover cursor; never part of the signed instruction.
        std::optional<size_t> automatic_endpoint{};
        // Derived from the immutable bytes for saved-action rows; not durable.
        std::string signed_bytes_sha256{};
        // Volatile: endpoint that acknowledged this process's latest delivery.
        // Only a waited status read uses it; lost on restart.
        std::optional<size_t> delivery_endpoint{};
    };
    std::map<std::pair<uint256, uint256>, Pending> m_pending;
    FlowMeshClientPollScheduler m_action_polls;
    std::map<uint256, std::pair<uint64_t, uint256>> m_highwater;
    // RestartDigest() of the last successful synchronous outbox write. Empty
    // after startup and whenever a write attempt failed (durable state then
    // unknown), so the next conditional save always writes.
    std::optional<uint256> m_durable_restart_digest;
    struct Cache {
        flowmesh::ClientEvidencePins pins;
        flowmesh::VerifiedClientState verified;
        flowmesh::ClientEventCursor cursor;
        UniValue status, reported;
        size_t endpoint{0};
        bool event_gap{false};
        // A filtered cursor belongs to this exact account scope, not merely
        // to the market. Another wallet must not inherit skipped events.
        std::optional<uint256> account_scope;
        // {request started, validated} of the last Refresh that completed
        // for this cache. Volatile; cleared when a Refresh starts or an own
        // inclusion newer than this entry is verified. Only a signing
        // preflight's join reads it.
        std::optional<std::pair<std::chrono::steady_clock::time_point, std::chrono::steady_clock::time_point>> fresh{};
    };
    std::map<uint256, Cache> m_cache;
    // Microblock high-water of verified own inclusions per market. Survives
    // eviction of certified actions from m_pending; only restricts joins.
    FlowMeshOwnCertifiedThrough m_own_certified_through{CLIENT_MAX_MARKETS};
    // Local policy; only the regtest-only -flowmeshtestjoinwindowms changes it.
    const FlowMeshJoinWindows m_join_windows;
    // Immutable copy of the retained actions as of the most recent m_work
    // release. Leaf lock, written only by the m_work owner; saved-action
    // reads never wait for network work. Null only if publication failed.
    mutable std::mutex m_saved_mutex;
    std::shared_ptr<const std::vector<interfaces::FlowMeshSavedAction>> m_saved_view;

    // Every m_work owner goes through this. Publication runs in the
    // destructor body, before the member unlocks, so the view always matches
    // a completed method and never exposes a mid-method state (for example
    // Send's write-ahead 'unknown' before the network call).
    class WorkLock {
        RemoteBackend& m_backend;
        std::unique_lock<FlowMeshClientWorkGate> m_lock;
    public:
        explicit WorkLock(RemoteBackend& backend) : m_backend{backend}, m_lock{backend.m_work} {}
        WorkLock(RemoteBackend& backend, std::try_to_lock_t) : m_backend{backend}, m_lock{backend.m_work, std::try_to_lock} {}
        WorkLock(const WorkLock&) = delete;
        WorkLock& operator=(const WorkLock&) = delete;
        ~WorkLock() { if (m_lock.owns_lock()) m_backend.PublishSavedView(); }
        bool owns_lock() const noexcept { return m_lock.owns_lock(); }
    };

    static std::string BytesSha256Hex(const std::vector<unsigned char>& bytes)
    {
        unsigned char digest[CSHA256::OUTPUT_SIZE];
        CSHA256().Write(bytes.data(), bytes.size()).Finalize(digest);
        return HexStr(digest);
    }
    interfaces::FlowMeshSavedAction SavedRow(const Pending& p) const
    {
        interfaces::FlowMeshSavedAction row;
        row.market_id = p.market; row.domain = p.domain; row.execution_config_id = p.config;
        row.account_id = p.owner_account; row.receipt = p.receipt;
        if (!p.action.IsDeposit()) row.sequence = p.action.sequence;
        row.action_type = p.action.type;
        switch (static_cast<flowmesh::ActionType>(p.action.type)) {
        case flowmesh::ActionType::SUBMIT_BID:
        case flowmesh::ActionType::CANCEL_BID: row.canonical_side = "bid"; break;
        case flowmesh::ActionType::SUBMIT_ASK:
        case flowmesh::ActionType::CANCEL_ASK: row.canonical_side = "ask"; break;
        default: break;
        }
        if (!row.canonical_side.empty()) row.canonical_points = p.action.curve;
        row.signed_bytes_sha256 = p.signed_bytes_sha256.empty() ? BytesSha256Hex(p.bytes) : p.signed_bytes_sha256;
        row.signed_bytes_size = p.bytes.size();
        row.initial_submission_ms = p.initial_submission_ms;
        row.may_have_been_sent = p.may_have_been_sent; row.previously_certified = p.previously_certified;
        return row;
    }
    std::vector<interfaces::FlowMeshSavedAction> SavedActionsLocked(const uint256& account, const std::optional<uint256>& market) const
    {
        std::vector<interfaces::FlowMeshSavedAction> out;
        for (const auto& [key, p] : m_pending) {
            if (p.owner_account == account && (!market || p.market == *market)) out.push_back(SavedRow(p));
        }
        return out;
    }
    void PublishSavedView() noexcept
    {
        // m_pending is bounded by CLIENT_MAX_ACTIONS. Memory only: no network,
        // chain-index synchronization or durable write.
        std::shared_ptr<const std::vector<interfaces::FlowMeshSavedAction>> view;
        try {
            auto rows{std::make_shared<std::vector<interfaces::FlowMeshSavedAction>>()};
            rows->reserve(m_pending.size());
            for (const auto& [key, p] : m_pending) rows->push_back(SavedRow(p));
            view = std::move(rows);
        } catch (...) {
            // Null makes readers fall back to m_work, never to stale rows.
        }
        {
            std::lock_guard lock{m_saved_mutex};
            std::swap(m_saved_view, view);
        }
        // The previous view is released here, outside the leaf lock.
    }

    void LearnMetadata(const UniValue& status, const flowmesh::ClientEvidencePins& pins, size_t endpoint)
    {
        if (!status.exists("asset_display")) return; // Older operators remain compatible.
        std::string error;
        auto metadata{ParseFlowMeshAssetMetadata(status["asset_display"], pins.domain, pins.base_asset, error)};
        if (!metadata) {
            // Cosmetic failure must not alter admission, balances or signing.
            LogDebug(BCLog::NET, "Ignoring invalid FlowMesh display metadata for asset=%s: %s\n", pins.base_asset.GetHex(), error);
            return;
        }
        metadata->source = "endpoint-label: " + m_endpoints.at(endpoint).url;
        std::lock_guard lock{m_metadata_mutex};
        const auto old{m_metadata.find(pins.base_asset)};
        if (old == m_metadata.end() && m_metadata.size() >= CLIENT_MAX_MARKETS) return;
        if (old != m_metadata.end() && old->second.name == metadata->name &&
            old->second.ticker == metadata->ticker && old->second.source == metadata->source) return;
        m_metadata.insert_or_assign(pins.base_asset, std::move(*metadata));
        m_metadata_generation.fetch_add(1, std::memory_order_release);
    }
    // Latency hint from an authenticated status object, never evidence.
    void LearnActionWait(const UniValue& status, size_t endpoint)
    {
        m_action_wait.at(endpoint) = FlowMeshAdvertisedActionWait(status);
    }

    void SyncIndexes() EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        auto& chain{m_chainman.ActiveChainstate()};
        const auto* tip{chain.m_chain.Tip()};
        const auto& params{m_chainman.GetConsensus()};
        if (!tip) Fail("B3 chain is not available");
        auto& seats{chain.ModernFnSeats()}; auto& vaults{chain.ModernFlowMeshVaults()}; auto& checkpoints{chain.ModernFlowMeshCheckpoints()};
        if (!seats.Sync(chain.m_chain, chain.m_blockman, params, *tip) ||
            !vaults.Sync(chain.m_chain, chain.m_blockman, params, *tip) ||
            !checkpoints.Sync(chain.m_chain, chain.m_blockman, params, seats.Index(), vaults.Index(), *tip))
            Fail("Mandatory B3 FlowMesh indexes are not ready");
    }
    flowmesh::ClientEvidencePins Pins(const uint256& market)
    {
        LOCK(::cs_main);
        SyncIndexes();
        const auto& params{m_chainman.GetConsensus()};
        const auto domain{params.legacy_final_hash ? modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash) : std::nullopt};
        const auto record{m_chainman.ActiveChainstate().ModernFlowMeshVaults().Index().Market(market)};
        if (!domain || !record) Fail("Market is not established on the local B3 chain");
        flowmesh::ClientEvidencePins out{*domain, market, record->base_asset, record->vault_id,
            flowmesh::ComputeExecutionConfigId(record->vault_id, record->base_asset, modern::NativeAsset(), flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS)};
        if (!flowmesh::CheckClientEvidencePins(out)) Fail("Local market configuration is not canonical");
        return out;
    }
    bool CanonicalAnchor(const flowmesh::AnchorRef& anchor)
    {
        LOCK(::cs_main);
        const auto& chain{m_chainman.ActiveChain()};
        const auto* block{anchor.height >= 0 ? chain[anchor.height] : nullptr};
        return block && block->GetBlockHash() == anchor.hash && chain.Height() - anchor.height >= Consensus::FLOWMESH_ANCHOR_DEPTH;
    }
    flowmesh::ProductionEntryCore EntryPrefix(const std::vector<unsigned char>& payload)
    {
        if (payload.size() < 4) Fail("Missing certified entry");
        const size_t size{ReadBE32(payload.data())};
        if (size == 0 || size > flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_BYTES || size > payload.size() - 4) Fail("Invalid certified entry length");
        const auto entry{flowmesh::DecodeProductionEntry(std::span{payload}.subspan(4, size))};
        if (!entry) Fail("Malformed canonical certified entry");
        return *entry;
    }
    flowmesh::ProductionCertifiedEnvelope VerifyEntry(const flowmesh::ClientEvidencePins& pins,
                                                       const std::vector<unsigned char>& payload)
    {
        const auto entry{EntryPrefix(payload)};
        std::string error;
        const auto seats{ResolveFlowMeshClientSeats(m_chainman, pins, entry, error)};
        if (!seats) Fail(error);
        const auto certified{flowmesh::DecodeProductionCertifiedPayload(payload, seats->Size())};
        if (!certified || !CanonicalAnchor(entry.anchor) ||
            flowmesh::CheckProductionEntryCertificate(certified->entry, *seats, certified->certificate) != flowmesh::BlsCertificateCheck::OK)
            Fail("Action inclusion certificate is not valid under local B3 authority");
        return *certified;
    }
    void CheckStatus(const UniValue& value, const flowmesh::ClientEvidencePins& pins)
    {
        if (Id(value, "domain") != pins.domain || Id(value, "market_id") != pins.market_id ||
            Id(value, "base_asset_id") != pins.base_asset || Id(value, "vault_id") != pins.vault_id ||
            Id(value, "execution_config_id") != pins.execution_config_id || Text(value, "quote_asset") != "B3")
            Fail("Endpoint market/domain/configuration differs from local B3 pins");
        (void)Flag(value, "running"); (void)Flag(value, "paused"); (void)Flag(value, "pending_handoff");
        if (value.exists("chain_reconciling")) (void)Flag(value, "chain_reconciling");
        (void)Text(value, "halt"); (void)Text(value, "error");
        (void)Number(value, "next_microblock_sequence"); (void)Id(value, "last_microblock_hash", true);
    }
    // names_active is false only for the wait lane, whose endpoint is not the
    // one ordinary reads and submits use (m_selected): its successful reply
    // updates that endpoint's row but never the client's active endpoint.
    void EndpointResult(size_t endpoint, bool transport_available, const std::string& error, bool names_active = true)
    {
        std::lock_guard lock{m_status_mutex};
        auto& row{m_status.endpoints.at(endpoint)};
        row.available = error.empty(); row.last_error = error.substr(0, 1024);
        row.transport_available = transport_available;
        row.last_attempt_ms = TicksSinceEpoch<std::chrono::milliseconds>(SystemClock::now());
        if (transport_available) {
            row.consecutive_failures = 0; row.retry_after_ms = 0;
            m_retry_after[endpoint] = {};
        } else {
            row.consecutive_failures = std::min(row.consecutive_failures + 1, uint32_t{16});
            const auto delay{std::chrono::seconds{std::min(uint32_t{1} << row.consecutive_failures, uint32_t{60})}};
            row.retry_after_ms = row.last_attempt_ms + std::chrono::duration_cast<std::chrono::milliseconds>(delay).count();
            m_retry_after[endpoint] = std::chrono::steady_clock::now() + delay;
        }
        if (error.empty()) {
            if (names_active) m_status.active_endpoint = row.url;
            m_wait_unanswered.at(endpoint) = false;
        }
    }
    // Called by the m_work owner right after acquisition. Diagnostics only:
    // shows whether a trade's next step was captured by the gate's linger.
    void MarkWorkAcquired(FlowMeshTimingSpan& timing) const
    {
        timing.Mark("lock_acquired_us");
        if (!timing.Enabled()) return;
        const auto gate{m_work.Inspect()};
        timing.Field("priority", std::string{gate.owner_foreground ? "foreground" : "passive"});
        timing.Field("linger_window", uint64_t{gate.owner_linger_window});
        timing.Field("linger_after_release", uint64_t{gate.owner_lingers});
        timing.Field("linger_captures", gate.linger_captures);
        timing.Field("linger_expired_with_passive_waiting", gate.linger_expired_with_passive_waiting);
        timing.Field("burst_forced_passive_turns", gate.burst_forced_passive_turns);
    }
    UniValue Call(const std::string& method, const UniValue& params,
                  const std::function<void(const UniValue&, size_t)>& validate,
                  bool* possibly_sent = nullptr, bool* earlier_possible = nullptr,
                  size_t* attempted_endpoints = nullptr, Pending* automatic_action = nullptr)
    {
        FlowMeshTimingSpan timing{"client_call"};
        timing.Field("method", method);
        for (const char* key : {"market_id", "action_id", "account_id"})
            if (timing.Enabled() && params[key].isStr()) timing.Field(key, params[key].get_str());
        if (m_endpoints.empty()) Fail("No FlowMesh HTTPS trading endpoint configured; use flowmeshclientconnect or -flowmeshendpoint");
        UniValue request{UniValue::VOBJ}; request.pushKV("method", method); request.pushKV("params", params);
        const std::string body{request.write()};
        std::string error;
        // An explicit reconnect counts fresh probes and bypasses automatic
        // read cooldowns; exact-action submission/recovery keeps its full cycle.
        const bool automatic_read{!possibly_sent && !attempted_endpoints && method != "action"};
        const size_t first{automatic_action ? automatic_action->automatic_endpoint.value_or(m_selected) :
            (automatic_read ? m_preferred : m_selected)};
        for (size_t attempt{0}; attempt < m_endpoints.size(); ++attempt) {
            const size_t endpoint{(first + attempt) % m_endpoints.size()};
            if (automatic_read && std::chrono::steady_clock::now() < m_retry_after[endpoint]) continue;
            if (automatic_action && !m_action_polls.TryChargeAttempt(std::chrono::steady_clock::now())) {
                automatic_action->automatic_endpoint = endpoint;
                Fail("Automatic status refresh coalesced; retained observation is not a fresh endpoint response");
            }
            if (automatic_action) automatic_action->automatic_endpoint = (endpoint + 1) % m_endpoints.size();
            bool transport_available{false};
            try {
                if (attempted_endpoints) ++*attempted_endpoints;
                if (earlier_possible && possibly_sent) *earlier_possible = *possibly_sent;
                const auto reply{m_https.Request(m_endpoints[endpoint], "/flowmesh/v1", body, CLIENT_REQUEST_TIMEOUT, CLIENT_MAX_REPLY)};
                if (possibly_sent && reply.request_may_have_been_sent) *possibly_sent = true;
                if (!reply.response_received) Fail(reply.error.empty() ? "No HTTPS response; outcome unknown" : reply.error);
                transport_available = true;
                UniValue parsed;
                if (!parsed.read(reply.body)) Fail("Malformed endpoint JSON response");
                Keys(parsed, {"ok", "result", "error"});
                if (!Flag(parsed, "ok")) Fail(Text(parsed, "error"));
                if (reply.status != 200) Fail("Unexpected HTTPS response status");
                {
                    FlowMeshTimingSpan verification{"client_verify_response"};
                    verification.Field("method", method);
                    validate(parsed["result"], endpoint);
                }
                m_selected = endpoint; EndpointResult(endpoint, true, {});
                if (automatic_action) automatic_action->automatic_endpoint.reset();
                return parsed["result"];
            } catch (const std::exception& e) {
                error = e.what(); EndpointResult(endpoint, transport_available, error);
            }
        }
        if (automatic_action) automatic_action->automatic_endpoint.reset();
        if (error.empty()) {
            std::lock_guard lock{m_status_mutex};
            error = "Trading endpoints are waiting to retry";
            if (!m_status.endpoints[first].last_error.empty()) error += ": " + m_status.endpoints[first].last_error;
        }
        Fail(error);
    }
    void OpenJournal()
    {
        if (!m_db) m_db = std::make_unique<CDBWrapper>(DBParams{.path=m_path, .cache_bytes=1 << 20});
    }
    void SaveEndpoints(const std::vector<std::string>& urls, const std::string& selected)
    {
        UniValue root{UniValue::VOBJ}, entries{UniValue::VARR};
        root.pushKV("version", 1);
        for (const auto& url : urls) entries.push_back(url);
        root.pushKV("urls", std::move(entries)); root.pushKV("selected_url", selected);
        OpenJournal();
        m_db->Write(std::string{"public-client-endpoints-v1"}, ClientEndpointBlob{root.write()}, true);
    }
    void RestoreEndpoints()
    {
        if (!fs::exists(m_path)) return;
        OpenJournal();
        if (!m_db->Exists(std::string{"public-client-endpoints-v1"})) return;
        ClientEndpointBlob blob;
        if (!m_db->Read(std::string{"public-client-endpoints-v1"}, blob)) Fail("Saved trading endpoints are unreadable; preserved without reset");
        UniValue root;
        if (!root.read(blob.json)) Fail("Malformed saved trading endpoints; preserved without reset");
        Keys(root, {"version", "urls", "selected_url"});
        if (Number(root, "version", 1) != 1 || !root["urls"].isArray() || root["urls"].size() > CLIENT_MAX_ENDPOINTS)
            Fail("Malformed saved trading endpoint list; preserved without reset");
        const std::string selected{Text(root, "selected_url", 2048)};
        for (const auto& value : root["urls"].getValues()) {
            if (!value.isStr()) Fail("Saved trading endpoint is not a public HTTPS URL");
            HttpsEndpoint endpoint{value.get_str(), {}, {}};
            std::string error;
            if (!NormalizeFlowMeshHttpsEndpoint(endpoint, error) || endpoint.url != value.get_str())
                Fail("Invalid saved trading endpoint; preserved without reset");
            if (std::find(m_saved_endpoints.begin(), m_saved_endpoints.end(), endpoint.url) != m_saved_endpoints.end())
                Fail("Duplicate saved trading endpoint; preserved without reset");
            m_saved_endpoints.push_back(endpoint.url);
            // Explicit startup trust always wins for an existing origin.
            if (std::any_of(m_endpoints.begin(), m_endpoints.end(), [&](const auto& existing) { return existing.url == endpoint.url; })) continue;
            if (m_endpoints.size() == CLIENT_MAX_ENDPOINTS) Fail("Configured and saved trading endpoints exceed eight; preserved without reset");
            if (!ValidateFlowMeshHttpsTrust(endpoint, error)) Fail(error);
            m_endpoints.push_back(std::move(endpoint));
        }
        const auto it{std::find_if(m_endpoints.begin(), m_endpoints.end(), [&](const auto& endpoint) { return endpoint.url == selected; })};
        if (it != m_endpoints.end()) m_selected = std::distance(m_endpoints.begin(), it);
    }
    // Exactly what Restore reads back: each retained action's market, domain,
    // config, exact signed bytes (and so its ActionId), submission time,
    // delivery and certification flags and owner, plus every market's
    // high-water head. Receipts are written for inspection but never
    // restored: restart always requires fresh status evidence. A row's
    // signed bytes never change, so their SHA-256 (cached when the row is
    // created in Submit or Restore) stands for them without re-hashing up to
    // CLIENT_MAX_ACTIONS instructions on every conditional save.
    uint256 RestartDigest() const
    {
        HashWriter hasher{};
        hasher << uint64_t{m_highwater.size()};
        for (const auto& [market, head] : m_highwater) hasher << market << head.first << head.second;
        hasher << uint64_t{m_pending.size()};
        for (const auto& [key, p] : m_pending) {
            hasher << p.market << p.domain << p.config
                   << (p.signed_bytes_sha256.empty() ? BytesSha256Hex(p.bytes) : p.signed_bytes_sha256)
                   << p.initial_submission_ms << p.may_have_been_sent << p.previously_certified << p.owner_account;
        }
        return hasher.GetSHA256();
    }
    void PublishPendingCount()
    {
        std::lock_guard lock{m_status_mutex};
        m_status.pending_actions = std::count_if(m_pending.begin(), m_pending.end(), [](const auto& item) {
            return item.second.receipt.state != "certified_inclusion" && item.second.receipt.state != "rejected";
        });
    }
    // For writes after a network observation only. Skips the synchronous
    // write when nothing Restore reads changed since the last successful
    // write: such a write would persist only receipt text, which restart
    // discards. Writes that must precede a send always call Save().
    void SaveRestartState()
    {
        if (m_durable_restart_digest && *m_durable_restart_digest == RestartDigest()) {
            FlowMeshTimingSpan timing{"client_outbox_save_skipped"};
            PublishPendingCount();
            return;
        }
        Save();
    }
    void Save()
    {
        FlowMeshTimingSpan timing{"client_outbox_save"};
        m_durable_restart_digest.reset();
        UniValue root{UniValue::VOBJ}, actions{UniValue::VARR}, heads{UniValue::VARR};
        root.pushKV("version", 1);
        for (const auto& [key, p] : m_pending) {
            UniValue row{UniValue::VOBJ};
            row.pushKV("market_id", p.market.GetHex()); row.pushKV("domain", p.domain.GetHex()); row.pushKV("config", p.config.GetHex());
            row.pushKV("action_hex", HexStr(p.bytes)); row.pushKV("action_id", p.action.Id().GetHex());
            row.pushKV("initial_submission_ms", p.initial_submission_ms); row.pushKV("may_have_been_sent", p.may_have_been_sent);
            row.pushKV("previously_certified", p.previously_certified);
            row.pushKV("owner_account", p.owner_account.GetHex());
            row.pushKV("receipt", FlowMeshClientReceiptJson(p.receipt)); actions.push_back(std::move(row));
        }
        for (const auto& [market, head] : m_highwater) {
            UniValue row{UniValue::VOBJ}; row.pushKV("market_id", market.GetHex()); row.pushKV("sequence", head.first); row.pushKV("hash", head.second.GetHex());
            heads.push_back(std::move(row));
        }
        root.pushKV("actions", std::move(actions)); root.pushKV("heads", std::move(heads));
        ClientJournalBlob blob{root.write()};
        if (blob.json.size() > 8 * 1024 * 1024) Fail("Client outbox exceeds its durable bound");
        OpenJournal();
        timing.Mark("sync_started_us");
        m_db->Write(std::string{"public-client-v1"}, blob, true); // throws on failed synchronous write
        timing.Mark("sync_completed_us");
        m_durable_restart_digest = RestartDigest();
#ifdef FLOWMESH_CLIENT_CRASH_TEST_HOOKS
        test::ClientCrashRecord(m_path, "synchronous_journal_save_returned", root);
#endif
        PublishPendingCount();
    }
    Receipt ParseReceipt(const UniValue& value, const uint256& action, size_t endpoint)
    {
        if (Id(value, "action_id") != action) Fail("Endpoint action identity mismatch");
        Receipt out; out.action_id = action; out.state = Text(value, "receipt_state", 32);
        if (out.state != "queued" && out.state != "admitted" && out.state != "rejected" && out.state != "unknown" && out.state != "certified_inclusion")
            Fail("Unknown receipt state");
        out.reason = Text(value, "reason"); out.endpoint = m_endpoints.at(endpoint).url;
        if (out.state == "certified_inclusion") {
            out.microblock_hash = Id(value, "microblock_hash"); out.microblock_sequence = Number(value, "microblock_sequence");
        }
        // Endpoint booleans never establish a verification claim.
        return out;
    }
    void Restore()
    {
        if (!fs::exists(m_path)) return;
        OpenJournal();
        ClientJournalBlob blob;
        if (!m_db->Exists(std::string{"public-client-v1"})) return;
        if (!m_db->Read(std::string{"public-client-v1"}, blob)) Fail("Client outbox is unreadable; preserved without reset");
        UniValue root;
        if (!root.read(blob.json) || Number(root, "version", 1) != 1 || !root["actions"].isArray() || !root["heads"].isArray() ||
            root["actions"].size() > CLIENT_MAX_ACTIONS || root["heads"].size() > CLIENT_MAX_MARKETS) Fail("Malformed client outbox; preserved without reset");
        for (const auto& row : root["heads"].getValues())
            if (!m_highwater.emplace(Id(row, "market_id"), std::pair{Number(row, "sequence"), Id(row, "hash")}).second) Fail("Duplicate retained market head");
        for (const auto& row : root["actions"].getValues()) {
            Pending p; p.market = Id(row, "market_id"); p.domain = Id(row, "domain"); p.config = Id(row, "config");
            p.bytes = Bytes(row, "action_hex", flowmesh::FLOWMESH_ACTION_MAX_BYTES);
            p.signed_bytes_sha256 = BytesSha256Hex(p.bytes);
            const auto action{flowmesh::DecodeProductionActionPayload(p.bytes)};
            if (!action || action->Id() != Id(row, "action_id")) Fail("Invalid retained signed action");
            p.action = *action; p.initial_submission_ms = Number(row, "initial_submission_ms");
            p.may_have_been_sent = Flag(row, "may_have_been_sent"); p.receipt.action_id = action->Id();
            p.previously_certified = row.exists("previously_certified") && Flag(row, "previously_certified");
            p.owner_account = row.exists("owner_account") ? Id(row, "owner_account", true) : action->signer;
            if (!p.owner_account.IsNull()) p.receipt.account_id = p.owner_account;
            // Restart requires fresh evidence, never trusts a saved endpoint's
            // certification claim. It also never changes the signed bytes.
            p.receipt.reason = "Retained original action; fresh status evidence required after restart";
            if (!m_pending.emplace(std::pair{p.market, action->Id()}, std::move(p)).second) Fail("Duplicate retained action");
        }
#ifdef FLOWMESH_CLIENT_CRASH_TEST_HOOKS
        test::ClientCrashRecord(m_path, "journal_restore_completed", root);
#endif
        std::lock_guard lock{m_status_mutex}; m_status.pending_actions = m_pending.size();
    }
    void Inclusion(Pending& p, const flowmesh::ProductionCertifiedEnvelope& certified, size_t endpoint,
                   const std::vector<unsigned char>& proof)
    {
        const auto& entry{certified.entry};
        if (std::none_of(entry.actions.begin(), entry.actions.end(), [&](const auto& action) { return action.Id() == p.action.Id(); }))
            Fail("Certified entry does not include the requested semantic action");
        p.receipt = {}; p.receipt.action_id = p.action.Id(); p.receipt.state = "certified_inclusion";
        p.receipt.microblock_hash = entry.GetHash(); p.receipt.microblock_sequence = entry.sequence;
        p.receipt.certificate_verified = true; p.receipt.endpoint = m_endpoints[endpoint].url;
        p.previously_certified = true;
        m_action_polls.Remove({p.market, p.action.Id()});
        size_t retained_bytes{proof.size()};
        for (const auto& [key, item] : m_pending) if (&item != &p) retained_bytes += item.inclusion_proof.size();
        if (retained_bytes > 8 * 1024 * 1024) {
            // Only evict re-fetchable proof copies. The durable no-replay
            // marker and the exact signed instruction remain intact.
            for (auto& [key, item] : m_pending) if (&item != &p) {
                item.inclusion_proof.clear(); item.inclusion_proof.shrink_to_fit();
            }
        }
        p.inclusion_proof = proof;
        if (!p.owner_account.IsNull()) p.receipt.account_id = p.owner_account;
        p.receipt.reason = "Canonical action inclusion verified; execution outcome is not proved by inclusion alone";
        m_own_certified_through.Record(p.market, entry.sequence);
        // A cached state older than this inclusion must not feed a join.
        if (const auto cache{m_cache.find(p.market)}; cache != m_cache.end() &&
            entry.sequence > cache->second.verified.certified.entry.sequence) cache->second.fresh.reset();
    }
    void Snapshot(const uint256& id, const std::optional<uint256>& account)
    {
        const auto pins{Pins(id)};
        UniValue params{UniValue::VOBJ}; params.pushKV("market_id", id.GetHex());
        if (account) params.pushKV("account_id", account->GetHex());
        Call("snapshot", params, [&](const UniValue& value, size_t endpoint) {
            CheckStatus(value["status"], pins);
            flowmesh::ClientStateEvidence evidence{Bytes(value, "certified_payload", flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_BYTES + 1024),
                Bytes(value, "state_bytes", flowmesh::CLIENT_STATE_MAX_BYTES), ParseCursor(value["cursor"])};
            const auto entry{EntryPrefix(evidence.certified_payload)};
            std::string error;
            const auto seats{ResolveFlowMeshClientSeats(m_chainman, pins, entry, error)};
            if (!seats) Fail(error);
            auto verified{flowmesh::VerifyClientStateEvidence(pins, evidence, *seats,
                [&](const auto& anchor) { return CanonicalAnchor(anchor); }, error)};
            if (!verified) Fail(error);
            const auto previous{m_highwater.find(id)};
            if (previous != m_highwater.end() && (entry.sequence < previous->second.first ||
                (entry.sequence == previous->second.first && entry.GetHash() != previous->second.second)))
                Fail("Stale or conflicting certified state; retained client head is not reset");
            {
                LOCK(::cs_main); SyncIndexes();
                const auto connected{m_chainman.ActiveChainstate().ModernFlowMeshCheckpoints().Index().Head(id)};
                if (connected && (entry.sequence < connected->core.sequence ||
                    (entry.sequence == connected->core.sequence && entry.GetHash() != connected->core.microblock_hash)))
                    Fail("Endpoint snapshot predates or conflicts with the connected B3 checkpoint");
            }
            if (!CanonicalAnchor(entry.anchor)) Fail("B3 anchor changed during snapshot verification");
            if (m_highwater.size() >= CLIENT_MAX_MARKETS && previous == m_highwater.end()) Fail("Client retained market limit reached");
            const bool lost_cursor{previous != m_highwater.end() && !m_cache.contains(id)};
            m_highwater[id] = {entry.sequence, entry.GetHash()};
            // Preserve rollback detection before exposing this state. An
            // unchanged head is already durable and needs no new write.
            SaveRestartState();
            // Heads/actions are durable, event cursors are not. Restart or
            // bounded cache eviction requires an explicit snapshot gap even
            // when the certified head itself has not changed.
            const bool gap{lost_cursor || (m_cache.contains(id) && m_cache.at(id).event_gap)};
            if (m_cache.size() >= CLIENT_MAX_CACHED_MARKETS && !m_cache.contains(id)) m_cache.erase(m_cache.begin());
            m_cache.insert_or_assign(id, Cache{pins, std::move(*verified), evidence.cursor, value["status"], value["reported_data"], endpoint, gap, account});
            if (lost_cursor) { std::lock_guard lock{m_status_mutex}; ++m_status.event_gaps; }
            for (auto& [key, pending] : m_pending) {
                if (pending.market != id) continue;
                if (std::any_of(entry.actions.begin(), entry.actions.end(), [&](const auto& action) { return action.Id() == pending.action.Id(); }))
                    Inclusion(pending, m_cache.at(id).verified.certified, endpoint, evidence.certified_payload);
            }
            SaveRestartState();
            LearnMetadata(value["status"], pins, endpoint);
            LearnActionWait(value["status"], endpoint);
        });
    }
    enum class CacheCheck { CURRENT, SCOPE, CHECKPOINT, AUTHORITY, ANCHOR };
    // Local checks that a cached verified state still applies to this account
    // under the local B3 chain. No network use.
    CacheCheck CheckCache(const uint256& id, const Cache& cache, const std::optional<uint256>& account)
    {
        // Switching wallets changes the event filter. A current verified
        // snapshot is safe, but it is not replay of the other account's
        // omitted events; surface the gap before advancing its cursor.
        if (cache.account_scope != account) return CacheCheck::SCOPE;
        {
            LOCK(::cs_main); SyncIndexes();
            const auto checkpoint{m_chainman.ActiveChainstate().ModernFlowMeshCheckpoints().Index().Head(id)};
            const auto& entry{cache.verified.certified.entry};
            if (checkpoint && (entry.sequence < checkpoint->core.sequence ||
                (entry.sequence == checkpoint->core.sequence && entry.GetHash() != checkpoint->core.microblock_hash)))
                return CacheCheck::CHECKPOINT;
        }
        std::string authority_error;
        if (!ResolveFlowMeshClientSeats(m_chainman, cache.pins, cache.verified.certified.entry, authority_error)) return CacheCheck::AUTHORITY;
        // Retain the durable high-water mark. Only fresh ordinary evidence
        // can replace the cache; a reorg never rewinds signing history.
        if (!CanonicalAnchor(cache.verified.certified.entry.anchor)) return CacheCheck::ANCHOR;
        return CacheCheck::CURRENT;
    }
    Cache& Refresh(const uint256& id, const std::optional<uint256>& account, const flowmesh::MarketDataQuery& query)
    {
        const auto started{std::chrono::steady_clock::now()};
        if (const auto it{m_cache.find(id)}; it != m_cache.end()) it->second.fresh.reset();
        // Stamped only on success; an exception leaves the cache unstamped.
        const auto done = [&]() -> Cache& {
            auto& cache{m_cache.at(id)};
            cache.fresh = std::pair{started, std::chrono::steady_clock::now()};
            return cache;
        };
        if (!m_cache.contains(id)) { Snapshot(id, account); return done(); }
        auto& cache{m_cache.at(id)};
        if (CheckCache(id, cache, account) != CacheCheck::CURRENT) {
            cache.event_gap = true;
            { std::lock_guard lock{m_status_mutex}; ++m_status.event_gaps; }
            Snapshot(id, account); return done();
        }
        bool snapshot_needed{false};
        UniValue params{UniValue::VOBJ}; params.pushKV("market_id", id.GetHex());
        if (account) params.pushKV("account_id", account->GetHex());
        params.pushKV("cursor", CursorJson(cache.cursor));
        const bool same_reported_account{!account || (cache.reported["account"].isObject() &&
            cache.reported["account"]["account_id"].isStr() && cache.reported["account"]["account_id"].get_str() == account->GetHex())};
        if (same_reported_account) params.pushKV("known_head", cache.verified.certified.entry.GetHash().GetHex());
        params.pushKV("limit", static_cast<uint64_t>(query.limit));
        if (query.before_sequence) params.pushKV("before_sequence", *query.before_sequence);
        Call("updates", params, [&](const UniValue& value, size_t endpoint) {
            CheckStatus(value["status"], cache.pins);
            const auto cursor{ParseCursor(value["cursor"])};
            const bool gap{Flag(value, "gap")};
            const bool changed_instance{cursor.instance_id != cache.cursor.instance_id};
            if (changed_instance && !gap) Fail("Endpoint changed cursor instance without reporting an event gap");
            if (!changed_instance && cursor.event_id < cache.cursor.event_id) Fail("Out-of-order event cursor");
            if (!value["events"].isArray() || value["events"].size() > flowmesh::CLIENT_EVENT_PAGE_MAX) Fail("Event page exceeds bound");
            uint64_t last{changed_instance ? 0 : cache.cursor.event_id};
            for (const auto& event : value["events"].getValues()) {
                const auto position{Number(event, "event_id")};
                if (position <= last || position > cursor.event_id || Id(event, "market_id") != id) Fail("Invalid event order or market");
                last = position;
                // Events are hints only. A separate canonical entry request
                // establishes inclusion, never an endpoint's receipt boolean.
            }
            if (gap) { cache.event_gap = true; std::lock_guard lock{m_status_mutex}; ++m_status.event_gaps; }
            const auto reported_sequence{Number(value["status"], "next_microblock_sequence")};
            const auto reported_head{Id(value["status"], "last_microblock_hash", true)};
            if (reported_sequence < cache.verified.certified.entry.sequence + 1) Fail("Endpoint reported a stale microblock head");
            if (reported_sequence == cache.verified.certified.entry.sequence + 1 && reported_head != cache.verified.certified.entry.GetHash())
                Fail("Endpoint reported a conflicting equal-height head");
            snapshot_needed = gap || reported_head != cache.verified.certified.entry.GetHash();
            if (snapshot_needed && !gap) {
                // The atomically exported snapshot can advance past events
                // produced after this page. Explicit resynchronization is
                // reported instead of silently treating those events as seen.
                cache.event_gap = true;
                std::lock_guard lock{m_status_mutex}; ++m_status.event_gaps;
            }
            cache.cursor = cursor; cache.endpoint = endpoint; cache.status = value["status"];
            if (value.exists("reported_data") && !value["reported_data"]["unchanged"].isTrue()) cache.reported = value["reported_data"];
            // A 'more' page is not skipped: cursor remains at the returned
            // page boundary, and the next refresh resumes there.
            (void)Flag(value, "more");
            LearnMetadata(value["status"], cache.pins, endpoint);
            LearnActionWait(value["status"], endpoint);
        });
        if (snapshot_needed) Snapshot(id, account);
        return done();
    }
    // A foreground wallet signing preflight may reuse the refresh that
    // completed just before it (typically the refresh it queued behind, or
    // the balance read that preceded an order) instead of another 'updates'
    // round trip. Every local chain/authority check and Project's checkpoint
    // check still run; any doubt falls back to Refresh.
    Cache* JoinFresh(const uint256& id, const std::optional<uint256>& account)
    {
        if (!account || !FlowMeshSigningPreflightScope::Active() ||
            FlowMeshClientWorkScope::Current() != FlowMeshClientWorkPriority::FOREGROUND ||
            !m_own_certified_through.Complete()) return nullptr;
        const auto it{m_cache.find(id)};
        if (it == m_cache.end() || !it->second.fresh) return nullptr;
        auto& cache{it->second};
        try {
            std::vector<FlowMeshJoinOwnAction> own;
            for (const auto& [key, p] : m_pending) {
                if (p.market != id || p.action.IsDeposit() || p.action.signer != *account) continue;
                using Kind = FlowMeshJoinOwnAction::Kind;
                if (p.receipt.certificate_verified && p.receipt.state == "certified_inclusion") {
                    own.push_back({Kind::CERTIFIED, p.receipt.microblock_sequence, p.action.sequence});
                } else if (p.previously_certified) {
                    // Certified before a restart, or its proof is being
                    // renewed: where it was included is not known here. It
                    // may join only if the cached state already consumed its
                    // sequence.
                    own.push_back({Kind::CERTIFIED, std::nullopt, p.action.sequence});
                } else if (p.receipt.state == "rejected") {
                    // Send and QueryAction keep a refusal after any possible
                    // earlier delivery as unknown, so this is definite.
                    own.push_back({Kind::DEFINITE_REJECTED, std::nullopt, p.action.sequence});
                } else {
                    own.push_back({Kind::UNRESOLVED, std::nullopt, p.action.sequence});
                }
            }
            const auto next_account_sequence{cache.verified.state.NextSequence(*account)};
            const auto allowed = [&] {
                return FlowMeshCanJoinFreshPreflight(std::chrono::steady_clock::now(), cache.fresh->first, cache.fresh->second,
                                                     cache.verified.certified.entry.sequence, next_account_sequence,
                                                     m_own_certified_through.Through(id), own, m_join_windows);
            };
            // Local refusals first (usually a stamp outside the window): a
            // refused join is followed by Refresh, which runs CheckCache.
            if (!allowed()) return nullptr;
            if (CheckCache(id, cache, account) != CacheCheck::CURRENT) return nullptr;
            // CheckCache can wait for cs_main; the windows bound the reused
            // observation's age when the join is taken, so check them again.
            if (!allowed()) return nullptr;
        } catch (const std::exception&) {
            return nullptr;
        }
        return &cache;
    }
    void ReportedHistory(flowmesh::MarketData& out, const UniValue& reported)
    {
        if (!reported.isObject() || !reported["history"].isObject()) return;
        if (Id(reported, "market_id") != out.market_id || Id(reported, "domain") != out.domain ||
            Id(reported, "execution_config_id") != out.execution_config_id) Fail("Reported history has wrong identity");
        const auto& history{reported["history"]}; const auto& entries{history["entries"]};
        if (!entries.isArray() || entries.size() > flowmesh::MARKET_DATA_MAX_HISTORY) Fail("Reported history page exceeds bound");
        out.history.available = Flag(history, "available"); out.history.truncated = Flag(history, "truncated");
        if (history.exists("oldest_retained_sequence")) out.history.oldest_retained_sequence = Number(history, "oldest_retained_sequence");
        if (history.exists("next_before_sequence")) out.history.next_before_sequence = Number(history, "next_before_sequence");
        uint64_t previous{UINT64_MAX};
        for (const auto& row : entries.getValues()) {
            flowmesh::MarketHistoryEntry entry;
            entry.sequence = Number(row, "sequence");
            if (entry.sequence >= out.snapshot.next_microblock_sequence || entry.sequence >= previous) Fail("Reported history is stale/out of order");
            previous = entry.sequence; entry.microblock_hash = Id(row, "microblock_hash"); entry.epoch = Number(row, "epoch");
            entry.anchor_height = Number(row, "anchor_height", INT32_MAX); entry.anchor_hash = Id(row, "anchor_hash");
            entry.handoff = Text(row, "kind") == "handoff"; entry.cleared = Flag(row, "cleared");
            entry.price = Number(row, "price"); entry.quantity = Number(row, "quantity");
            entry.notional_atoms = Number(row, "notional_atoms"); entry.fee_atoms = Number(row, "fee_atoms");
            entry.account_fills_complete = false;
            const bool matching_account{out.account && reported["account"].isObject() &&
                Id(reported["account"], "account_id") == out.account->account_id};
            if (matching_account && Flag(row, "account_fills_known")) entry.account_fills.push_back({out.account->account_id,
                static_cast<CAmount>(Number(row, "account_bid_fill")), static_cast<CAmount>(Number(row, "account_ask_fill"))});
            out.history.entries.push_back(std::move(entry));
        }
        // The caller labels this entire history endpoint-reported. In
        // particular its account rows NEVER replace the authenticated ledger.
    }
    flowmesh::MarketData Project(Cache& cache, const std::optional<uint256>& account, const flowmesh::MarketDataQuery& query)
    {
        std::string error;
        auto out{flowmesh::ClientMarketData(cache.pins, cache.verified, account, query, error)};
        if (!out) Fail(error);
        out->remote = true; out->endpoint = m_endpoints[cache.endpoint].url;
        out->certificate_verified = true; out->account_state_verified = true;
        out->event_gap = cache.event_gap;
        out->snapshot.running = Flag(cache.status, "running"); out->snapshot.paused = Flag(cache.status, "paused");
        // Older endpoints omit the reason; their paused/error gates still
        // apply. Never infer reconciliation from an endpoint's error text.
        out->snapshot.chain_reconciling = cache.status.exists("chain_reconciling") && Flag(cache.status, "chain_reconciling");
        out->snapshot.pending_handoff = Flag(cache.status, "pending_handoff"); out->snapshot.halt = Text(cache.status, "halt");
        out->snapshot.error = Text(cache.status, "error"); out->snapshot.runtime.round = Number(cache.status, "round", UINT32_MAX);
        out->snapshot.pending_actions = Number(cache.status, "pending_actions", 65536);
        {
            LOCK(::cs_main); SyncIndexes();
            const auto checkpoint{m_chainman.ActiveChainstate().ModernFlowMeshCheckpoints().Index().Head(cache.pins.market_id)};
            if (checkpoint && (out->snapshot.next_microblock_sequence <= checkpoint->core.sequence ||
                (out->snapshot.next_microblock_sequence == checkpoint->core.sequence + 1 &&
                 out->snapshot.last_microblock_hash != checkpoint->core.microblock_hash)))
                Fail("B3 checkpoint advanced while reading account state; refresh required before signing");
            out->b3_checkpoint_confirmed = checkpoint && checkpoint->core.microblock_hash == out->snapshot.last_microblock_hash &&
                checkpoint->core.state_root == out->snapshot.state_root;
        }
        if (!out->unchanged) ReportedHistory(*out, cache.reported);
        return std::move(*out);
    }
    flowmesh::ClientEvidencePins RecheckActionAuthority(Pending& p)
    {
        const auto pins{Pins(p.market)};
        if (pins.domain != p.domain || pins.execution_config_id != p.config) Fail("Retained action domain/config differs from current local market");
        if (p.receipt.certificate_verified && !p.inclusion_proof.empty()) {
            try { (void)VerifyEntry(pins, p.inclusion_proof); return pins; }
            catch (const std::exception& e) {
                p.receipt.certificate_verified = false; p.receipt.state = "unknown";
                p.receipt.reason = std::string{"Prior inclusion authority no longer verified: "} + e.what();
            }
        }
        if (p.receipt.certificate_verified && p.inclusion_proof.empty()) {
            p.receipt.certificate_verified = false; p.receipt.state = "unknown";
            p.receipt.reason = "Previously verified inclusion retained; fresh authority evidence required";
        }
        return pins;
    }
    // Applies one endpoint's 'action' result to a retained action whose
    // receipt is not verified. Certified inclusion requires a certificate
    // valid under local B3 authority that agrees with the receipt; any other
    // observation keeps a possibly delivered action unresolved. Throws before
    // changing p when the result is unusable.
    void ApplyActionResult(Pending& p, const flowmesh::ClientEvidencePins& pins, const UniValue& value, size_t endpoint)
    {
        auto receipt{ParseReceipt(value, p.action.Id(), endpoint)};
        if (receipt.state == "certified_inclusion") {
            const auto payload{Bytes(value, "certified_payload", flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_BYTES + 1024)};
            const auto proof{VerifyEntry(pins, payload)};
            if (proof.entry.GetHash() != receipt.microblock_hash || proof.entry.sequence != receipt.microblock_sequence)
                Fail("Action receipt and its certificate disagree");
            Inclusion(p, proof, endpoint, payload);
        } else if ((receipt.state == "rejected" || receipt.state == "unknown") && p.may_have_been_sent) {
            // One node's rejection/absence cannot undo an earlier possible
            // delivery. Preserve uncertainty, not a fresh nonce/order.
            if (p.receipt.state != "queued" && p.receipt.state != "admitted") p.receipt.state = "unknown";
            p.receipt.reason = "No verified inclusion yet; endpoint observation: " + receipt.state + ": " + receipt.reason;
        } else {
            p.receipt = std::move(receipt);
        }
        if (!p.owner_account.IsNull()) p.receipt.account_id = p.owner_account;
    }
    Receipt QueryAction(Pending& p, bool automatic = false)
    {
        const auto pins{RecheckActionAuthority(p)};
        if (p.receipt.certificate_verified) return p.receipt;
        if (!automatic) p.automatic_endpoint.reset();
        UniValue params{UniValue::VOBJ}; params.pushKV("market_id", p.market.GetHex()); params.pushKV("action_id", p.action.Id().GetHex());
        Call("action", params, [&](const UniValue& value, size_t endpoint) {
            ApplyActionResult(p, pins, value, endpoint);
        }, nullptr, nullptr, nullptr, automatic ? &p : nullptr);
        if (!p.receipt.certificate_verified) m_action_polls.MarkObserved({p.market, p.action.Id()});
        // A non-certifying observation changes only the receipt, which
        // restart never reads.
        if (p.receipt.certificate_verified) Save();
        else SaveRestartState();
        return p.receipt;
    }
    Receipt Send(Pending& p)
    {
        if (p.previously_certified) {
            try { QueryAction(p); }
            catch (const std::exception& e) {
                p.receipt.certificate_verified = false; p.receipt.state = "unknown"; p.receipt.reason = e.what();
            }
            if (!p.receipt.certificate_verified) p.receipt.reason = "Previously verified inclusion is retained; fresh proof required for current authority, and this action will not be resubmitted";
            return p.receipt;
        }
        const auto pins{Pins(p.market)};
        if (pins.domain != p.domain || pins.execution_config_id != p.config) Fail("Cannot retry retained action under different domain/config");
        const bool prior_possible{p.may_have_been_sent};
        // Write-ahead uncertainty covers a crash immediately after the first
        // socket write. No signature is generated by this backend.
        p.may_have_been_sent = true; p.receipt.state = "unknown"; Save();
        UniValue params{UniValue::VOBJ}; params.pushKV("market_id", p.market.GetHex());
        params.pushKV("action_id", p.action.Id().GetHex()); params.pushKV("action_hex", HexStr(p.bytes));
        bool possible{false};
        bool earlier_possible{false};
        try {
            Call("submit", params, [&](const UniValue& value, size_t endpoint) {
                auto receipt{ParseReceipt(value, p.action.Id(), endpoint)};
                if (receipt.state == "certified_inclusion") {
                    // Submit acknowledgement itself carries no inclusion proof.
                    receipt.state = "unknown"; receipt.reason = "Endpoint reports inclusion; canonical certificate must still be retrieved";
                }
                if (receipt.state == "rejected" && (prior_possible || earlier_possible)) receipt.state = "unknown";
                p.receipt = std::move(receipt);
                if (!p.owner_account.IsNull()) p.receipt.account_id = p.owner_account;
                p.delivery_endpoint = endpoint;
            }, &possible, &earlier_possible);
        } catch (const std::exception& e) {
            p.receipt.state = "unknown"; p.receipt.reason = e.what();
        }
        // Even a TLS failure is conservatively retryable; only an explicit
        // rejection with no earlier possibly-delivered attempt is definite.
        // The write-ahead Save above already made the delivery flag durable;
        // this observation changes only the receipt.
        SaveRestartState(); return p.receipt;
    }
    // One waited 'action' read of an action this process delivered, sent on
    // the lane connection to the endpoint that acknowledged the delivery and
    // only if that endpoint advertises the wait. m_work is held to select the
    // target and charge one automatic attempt, and again to apply the reply by
    // QueryAction's rules, never across the network wait. It never sends or
    // signs, and a receipt verified meanwhile is returned unchanged. nullopt
    // means nothing was applied; the caller then takes the ordinary path. A
    // reply applied whose outbox save then failed is returned as the ordinary
    // refresh would return it: unverified, with the failure as its reason.
    //
    // Cost: a lane request adds at most wait + CLIENT_REQUEST_TIMEOUT (7.5 s
    // at the 2.5 s cap) before the ordinary read that may follow it, which
    // keeps its own c10c952 bound (CLIENT_REQUEST_TIMEOUT per endpoint
    // attempt). The lane never starts while its endpoint is in transport
    // cooldown, and a lane request that got no HTTP response keeps the lane
    // off that endpoint until a successful response from it is recorded, so
    // an endpoint that goes silent costs that extra time once, not per call.
    std::optional<Receipt> WaitedActionStatus(const uint256& market, const uint256& action, std::chrono::milliseconds requested)
    {
        FlowMeshTimingSpan timing{"client_action_wait"};
        timing.Field("market_id", market);
        if (timing.Enabled()) timing.Field("action_id", action);
        const auto outcome = [&](const char* result) { timing.Field("result", std::string{result}); };
        std::unique_lock lane{m_wait_lane, std::try_to_lock};
        if (!lane.owns_lock()) { outcome("lane_busy"); return std::nullopt; }
        const std::pair key{market, action};
        HttpsEndpoint target;
        size_t endpoint{0};
        std::chrono::milliseconds wait{0};
        timing.Mark("select_lock_requested_us");
        {
            WorkLock lock{*this};
            timing.Mark("select_lock_acquired_us");
            const auto it{m_pending.find(key)};
            if (it == m_pending.end()) { outcome("not_retained"); return std::nullopt; }
            const auto& p{it->second};
            wait = FlowMeshActionWaitFor({.requested = requested, .may_have_been_sent = p.may_have_been_sent,
                .certificate_verified = p.receipt.certificate_verified, .previously_certified = p.previously_certified,
                .rejected = p.receipt.state == "rejected", .delivery_endpoint_known = p.delivery_endpoint.has_value(),
                .advertised = p.delivery_endpoint ? m_action_wait.at(*p.delivery_endpoint) : std::chrono::milliseconds{0}});
            if (wait.count() == 0) { outcome("not_eligible"); return std::nullopt; }
            try {
                // QueryAction's local authority check, before any network use.
                const auto pins{Pins(p.market)};
                if (pins.domain != p.domain || pins.execution_config_id != p.config) { outcome("not_eligible"); return std::nullopt; }
            } catch (const std::exception&) { outcome("not_eligible"); return std::nullopt; }
            endpoint = *p.delivery_endpoint;
            // Both are only read here: a lane failure starts no transport
            // cooldown, and only a successful response clears 'unanswered'.
            if (std::chrono::steady_clock::now() < m_retry_after.at(endpoint)) { outcome("cooldown"); return std::nullopt; }
            if (m_wait_unanswered.at(endpoint)) { outcome("unanswered"); return std::nullopt; }
            // Exactly one automatic attempt, charged before transport starts.
            if (!m_action_polls.TryChargeAttempt(std::chrono::steady_clock::now())) { outcome("coalesced"); return std::nullopt; }
            target = m_endpoints.at(endpoint);
        }
        timing.Field("wait_ms", uint64_t(wait.count()));
        timing.Field("endpoint", target.url);
        if (m_wait_reset.exchange(false)) m_wait_https.Reset();
        UniValue params{UniValue::VOBJ};
        params.pushKV("market_id", market.GetHex()); params.pushKV("action_id", action.GetHex());
        params.pushKV("wait_ms", uint64_t(wait.count()));
        UniValue request{UniValue::VOBJ}; request.pushKV("method", "action"); request.pushKV("params", std::move(params));
        timing.Mark("request_started_us");
        // The endpoint answers before its own wait ends; the transport bound
        // is the ordinary one on top of the wait.
        const auto reply{m_wait_https.Request(target, "/flowmesh/v1", request.write(), wait + CLIENT_REQUEST_TIMEOUT,
                                              CLIENT_ACTION_WAIT_MAX_REPLY)};
        timing.Mark("request_completed_us");
        lane.unlock();
        timing.Field("connection_reused", uint64_t{reply.connection_reused});
        timing.Field("tls_handshake_performed", uint64_t{reply.tls_handshake_performed});
        UniValue result;
        std::string error;
        bool unsupported{false};
        try {
            if (!reply.response_received) Fail(reply.error.empty() ? "No HTTPS response" : reply.error);
            UniValue parsed;
            if (!parsed.read(reply.body)) Fail("Malformed endpoint JSON response");
            Keys(parsed, {"ok", "result", "error"});
            if (!Flag(parsed, "ok")) {
                const auto text{Text(parsed, "error")};
                unsupported = text == FLOWMESH_CLIENT_UNKNOWN_FIELD_ERROR;
                Fail(text);
            }
            if (reply.status != 200) Fail("Unexpected HTTPS response status");
            result = parsed["result"];
            // A latency hint only; never evidence.
            if (result.isObject() && result["wait_status"].isStr()) timing.Field("wait_status", result["wait_status"].get_str());
        } catch (const std::exception& e) {
            error = e.what();
        }
        timing.Mark("apply_lock_requested_us");
        WorkLock lock{*this};
        timing.Mark("apply_lock_acquired_us");
        if (!reply.response_received) m_wait_unanswered.at(endpoint) = true;
        if (unsupported) {
            // The endpoint no longer offers the wait (for example after a
            // downgrade). Nothing was applied or resent; wait_ms is sent there
            // again only after it advertises the capability again.
            m_action_wait.at(endpoint) = {};
            outcome("unsupported");
            return std::nullopt;
        }
        if (!error.empty()) {
            outcome("failed"); timing.Field("error", error);
            return std::nullopt;
        }
        const auto it{m_pending.find(key)};
        if (it == m_pending.end()) { outcome("not_retained"); return std::nullopt; }
        auto& p{it->second};
        bool applied{false};
        try {
            const auto pins{RecheckActionAuthority(p)};
            if (p.receipt.certificate_verified) {
                // Proved by another read meanwhile: never replaced by this one.
                m_action_polls.Remove(key);
                outcome("already_verified");
                return p.receipt;
            }
            ApplyActionResult(p, pins, result, endpoint);
            applied = true;
            EndpointResult(endpoint, true, {}, /*names_active=*/false);
            if (p.receipt.certificate_verified) {
                Save();
            } else {
                m_action_polls.MarkObserved(key);
                // A non-certifying observation changes only the receipt.
                SaveRestartState();
            }
        } catch (const std::exception& e) {
            timing.Field("error", std::string{e.what()});
            // A verified label here was set by this apply (the receipt was not
            // verified before it). Without one, and with nothing applied,
            // nothing changed: the ordinary read runs next, in the same call.
            if (!applied && !p.receipt.certificate_verified) { outcome("failed"); return std::nullopt; }
            // The reply was applied but the outbox write failed, so the new
            // state is not durable. Exactly like the ordinary refresh: record
            // why, and never return (nor leave for the ordinary path to
            // return) a verified label that was not saved. previously_certified
            // stays set in memory, so this action is still never resent.
            p.receipt.reason = e.what();
            if (p.previously_certified || p.receipt.certificate_verified) {
                p.receipt.certificate_verified = false; p.receipt.state = "unknown";
            }
            outcome("save_failed");
            return p.receipt;
        }
        outcome(p.receipt.certificate_verified ? "certified" : "observed");
        return p.receipt;
    }
public:
    RemoteBackend(ChainstateManager& chainman, std::vector<HttpsEndpoint> endpoints, const fs::path& path,
                  const FlowMeshJoinWindows& join_windows = {})
        : m_chainman{chainman}, m_path{path}, m_join_windows{join_windows}
    {
        if (endpoints.size() > CLIENT_MAX_ENDPOINTS) Fail("At most eight independent trading endpoints may be configured");
        for (auto& endpoint : endpoints) {
            std::string error;
            if (!NormalizeFlowMeshHttpsEndpoint(endpoint, error) || !ValidateFlowMeshHttpsTrust(endpoint, error)) Fail(error);
            const auto existing{std::find_if(m_endpoints.begin(), m_endpoints.end(), [&](const auto& candidate) { return candidate.url == endpoint.url; })};
            if (existing != m_endpoints.end()) {
                if (existing->ca_file != endpoint.ca_file || existing->certificate_sha256 != endpoint.certificate_sha256)
                    Fail("Duplicate trading endpoint has conflicting CA or certificate-pin settings");
                continue;
            }
            m_endpoints.push_back(std::move(endpoint));
        }
        m_status.backend = "remote"; m_status.engine_enabled = false;
        Restore();
        RestoreEndpoints();
        m_preferred = m_selected;
        m_retry_after.resize(m_endpoints.size());
        m_action_wait.resize(m_endpoints.size());
        m_wait_unanswered.resize(m_endpoints.size());
        for (const auto& endpoint : m_endpoints) m_status.endpoints.push_back({endpoint.url, false, {}});
        if (!m_endpoints.empty()) m_status.selected_endpoint = m_endpoints[m_selected].url;
        PublishSavedView();
    }
    bool Connect(const std::string& url, std::string& error) override
    {
        error.clear();
        HttpsEndpoint endpoint{url, {}, {}};
        if (!NormalizeFlowMeshHttpsEndpoint(endpoint, error)) return false;
        FlowMeshTimingSpan timing_lock_1145{__func__};
        timing_lock_1145.Field("lock_name", std::string{"client_work"});
        timing_lock_1145.Mark("lock_requested_us");
        WorkLock lock{*this};
        MarkWorkAcquired(timing_lock_1145);
        try {
            const auto existing{std::find_if(m_endpoints.begin(), m_endpoints.end(), [&](const auto& candidate) { return candidate.url == endpoint.url; })};
            const bool added{existing == m_endpoints.end()};
            if (added && m_endpoints.size() == CLIENT_MAX_ENDPOINTS) Fail("At most eight independent trading endpoints may be configured");
            if (added && !ValidateFlowMeshHttpsTrust(endpoint, error)) return false;
            const size_t selected{added ? m_endpoints.size() : static_cast<size_t>(std::distance(m_endpoints.begin(), existing))};
            auto saved{m_saved_endpoints};
            if (added) saved.push_back(endpoint.url);
            // A configured endpoint is selected as-is, never rewritten with
            // default trust. Persist before publishing an in-memory change.
            SaveEndpoints(saved, endpoint.url);
            if (added) {
                m_endpoints.push_back(endpoint); m_saved_endpoints = std::move(saved);
                m_retry_after.emplace_back();
                m_action_wait.emplace_back();
                m_wait_unanswered.push_back(false);
            }
            m_selected = selected; m_preferred = selected; m_retry_after[selected] = {};
            m_https.Reset(); // Explicit connect revalidates existing trust.
            m_wait_reset = true;
            {
                std::lock_guard status_lock{m_status_mutex};
                if (added) m_status.endpoints.push_back({endpoint.url, false, {}});
                m_status.selected_endpoint = endpoint.url;
                m_status.endpoints[selected].retry_after_ms = 0;
            }
        } catch (const std::exception& e) { error = e.what(); return false; }
        // Reachability is an observation, not configuration failure. Only
        // market discovery is sent; no retained action is inspected or sent.
        try { (void)ReadMarkets(); } catch (const std::exception&) {}
        return true;
    }
    std::optional<modern::AssetDisplayMetadata> Metadata(const uint256& asset) const override
    {
        std::lock_guard lock{m_metadata_mutex};
        const auto it{m_metadata.find(asset)};
        return it == m_metadata.end() ? std::nullopt : std::optional{it->second};
    }
    uint64_t MetadataGeneration() const override { return m_metadata_generation.load(std::memory_order_acquire); }
private:
    UniValue ReadMarkets()
    {
        UniValue params{UniValue::VOBJ};
        return Call("markets", params, [&](const UniValue& value, size_t endpoint) {
            if (!value.isArray() || value.size() > CLIENT_MAX_MARKETS) Fail("Market discovery exceeds bound");
            std::set<uint256> seen;
            for (const auto& row : value.getValues()) {
                const auto id{Id(row, "market_id")}; if (!seen.insert(id).second) Fail("Duplicate market identity");
                CheckStatus(row, Pins(id));
                LearnActionWait(row, endpoint);
            }
        });
    }
public:
    std::vector<MarketStatus> Markets(const std::optional<uint256>& account) override
    {
        FlowMeshTimingSpan timing_lock_1197{__func__};
        timing_lock_1197.Field("lock_name", std::string{"client_work"});
        timing_lock_1197.Mark("lock_requested_us");
        WorkLock lock{*this};
        MarkWorkAcquired(timing_lock_1197);
        std::vector<MarketStatus> out;
        const auto rows{ReadMarkets()};
        // Discovery is bounded metadata; selected market gets full proof on
        // demand, not every market's snapshot on each Qt refresh.
        for (const auto& row : rows.getValues()) {
            MarketStatus status; const auto pins{Pins(Id(row, "market_id"))};
            LearnMetadata(row, pins, m_selected);
            status.market_id = pins.market_id; status.domain = pins.domain; status.base_asset = pins.base_asset;
            status.vault_id = pins.vault_id; status.execution_config_id = pins.execution_config_id;
            status.remote = true; status.endpoint = m_endpoints[m_selected].url;
            status.available = true; status.running = false; status.paused = true;
            status.error = "Select market to verify its certified state"; out.push_back(std::move(status));
        }
        return out;
    }
    std::optional<MarketStatus> Market(const uint256& id, const std::optional<uint256>& account) override
    {
        FlowMeshTimingSpan timing_lock_1215{__func__};
        timing_lock_1215.Field("lock_name", std::string{"client_work"});
        timing_lock_1215.Mark("lock_requested_us");
        WorkLock lock{*this};
        MarkWorkAcquired(timing_lock_1215);
        MarketStatus status; status.market_id = id; status.remote = true;
        try {
            // Only a signing preflight may join. Explicit reads and Data()
            // always refresh; a join never re-stamps the cache, so joins
            // cannot chain.
            Cache* const joined{JoinFresh(id, account)};
            if (FlowMeshSigningPreflightScope::Active()) timing_lock_1215.Field("preflight", std::string{joined ? "joined" : "refreshed"});
            auto& cache{joined ? *joined : Refresh(id, account, {})}; const auto data{Project(cache, account, {})};
            status.domain = cache.pins.domain; status.base_asset = cache.pins.base_asset; status.vault_id = cache.pins.vault_id;
            status.execution_config_id = cache.pins.execution_config_id; status.endpoint = data.endpoint;
            status.available = true; status.running = data.snapshot.running; status.paused = data.snapshot.paused;
            status.halt = data.snapshot.halt; status.error = data.snapshot.error; status.pending_handoff = data.snapshot.pending_handoff;
            status.certificate_verified = true; status.account_state_verified = true; status.b3_checkpoint_confirmed = data.b3_checkpoint_confirmed;
            status.epoch = data.snapshot.epoch; status.next_microblock_sequence = data.snapshot.next_microblock_sequence;
            status.last_microblock_hash = data.snapshot.last_microblock_hash; status.state_root = data.snapshot.state_root;
            status.next_effect_index = cache.verified.certified.entry.effect_start + cache.verified.certified.entry.effect_count;
            status.slot = cache.verified.state.Slot(); status.round = data.snapshot.runtime.round; status.pending_actions = data.snapshot.pending_actions;
            if (data.account) {
                status.account_id = account; status.next_account_sequence = data.account->next_sequence;
                status.base_available = data.account->base_available; status.base_reserved = data.account->base_reserved;
                status.b3_available = data.account->b3_available_atoms; status.b3_reserved = data.account->b3_reserved_atoms;
            }
            status.checkpoint_pending = Flag(cache.status, "checkpoint_pending");
            if (status.checkpoint_pending) {
                status.pending_checkpoint_id = Id(cache.status, "pending_checkpoint_id");
                status.pending_checkpoint_sequence = Number(cache.status, "pending_checkpoint_sequence");
                status.pending_checkpoint_effect_count = Number(cache.status, "pending_checkpoint_effect_count", UINT32_MAX);
            }
        } catch (const std::exception& e) { status.error = e.what(); status.paused = true; status.running = false; }
        return status;
    }
    std::optional<flowmesh::MarketData> Data(const uint256& id, const std::optional<uint256>& account,
                                          const flowmesh::MarketDataQuery& query, std::string& error) override
    {
        FlowMeshTimingSpan timing_lock_1245{__func__};
        timing_lock_1245.Field("lock_name", std::string{"client_work"});
        timing_lock_1245.Mark("lock_requested_us");
        WorkLock lock{*this};
        MarkWorkAcquired(timing_lock_1245);
        try { return Project(Refresh(id, account, query), account, query); }
        catch (const std::exception& e) { error = e.what(); return std::nullopt; }
    }
    Receipt Submit(const uint256& market, const flowmesh::Action& action) override
    {
        FlowMeshTimingSpan timing_lock_1251{__func__};
        timing_lock_1251.Field("market_id", market);
        if (timing_lock_1251.Enabled()) timing_lock_1251.Field("action_id", action.Id());
        timing_lock_1251.Field("lock_name", std::string{"client_work"});
        timing_lock_1251.Mark("lock_requested_us");
        WorkLock lock{*this};
        MarkWorkAcquired(timing_lock_1251);
        Receipt out; out.action_id = action.Id();
        try {
            const auto pins{Pins(market)};
            if (!action.IsDeposit() && !flowmesh::SchnorrActionAuthenticator(pins.domain, pins.execution_config_id).Authenticate(action))
                Fail("Local signed action authentication failed");
            const auto bytes{flowmesh::EncodeProductionActionPayload(action)};
            if (!bytes || bytes->size() > flowmesh::FLOWMESH_ACTION_MAX_BYTES) Fail("Action exceeds bounded public codec");
            auto existing{m_pending.find({market, action.Id()})};
            if (existing != m_pending.end()) return Send(existing->second);
            for (const auto& [key, prior] : m_pending) {
                if (prior.market == market && !action.IsDeposit() && prior.action.signer == action.signer && prior.action.sequence == action.sequence &&
                    prior.receipt.state != "rejected" && !prior.receipt.certificate_verified)
                    Fail("A different retained action already uses this account sequence; resolve its outcome before signing another instruction");
            }
            if (m_pending.size() >= CLIENT_MAX_ACTIONS) {
                const auto completed{std::find_if(m_pending.begin(), m_pending.end(), [](const auto& value) { return value.second.receipt.certificate_verified; })};
                if (completed == m_pending.end()) Fail("Pending action limit reached; unresolved signed objects are not evicted");
                m_action_polls.Remove(completed->first);
                m_pending.erase(completed);
            }
            Pending pending{market, pins.domain, pins.execution_config_id, action, *bytes,
                TicksSinceEpoch<std::chrono::milliseconds>(SystemClock::now()), false, out, false, {}, action.signer};
            pending.signed_bytes_sha256 = BytesSha256Hex(pending.bytes);
            if (action.IsDeposit()) {
                LOCK(::cs_main); SyncIndexes();
                const auto deposit{m_chainman.ActiveChainstate().ModernFlowMeshVaults().Index().Get(action.outpoint)};
                if (!deposit || !deposit->account || deposit->vault_id != pins.vault_id) Fail("Deposit ownership is not available from the local B3 vault index");
                pending.owner_account = *deposit->account;
            }
            pending.receipt.account_id = pending.owner_account;
            auto [it, inserted]{m_pending.emplace(std::pair{market, action.Id()}, std::move(pending))};
            Save();
#ifdef FLOWMESH_CLIENT_CRASH_TEST_HOOKS
            test::ClientCrashBeforeFirstSend(m_path, action.Id(), action.IsDeposit());
#endif
            return Send(it->second);
        } catch (const std::exception& e) {
            out.state = m_pending.contains({market, action.Id()}) ? "unknown" : "rejected"; out.reason = e.what(); return out;
        }
    }
    Receipt ActionStatus(const uint256& market, const uint256& action, bool retry,
                         std::chrono::milliseconds wait = std::chrono::milliseconds{0}) override
    {
        // A waited read answers this call only if it applied a usable reply;
        // otherwise the ordinary status read below runs in the same call.
        if (!retry && wait.count() > 0) {
            if (auto waited{WaitedActionStatus(market, action, wait)}) return std::move(*waited);
        }
        FlowMeshTimingSpan timing_lock_1293{__func__};
        timing_lock_1293.Field("market_id", market);
        timing_lock_1293.Field("action_id", action);
        timing_lock_1293.Field("lock_name", std::string{"client_work"});
        timing_lock_1293.Mark("lock_requested_us");
        WorkLock lock{*this};
        MarkWorkAcquired(timing_lock_1293);
        Receipt out; out.action_id = action;
        const auto it{m_pending.find({market, action})};
        if (it == m_pending.end()) { out.reason = "No retained local signed object; action outcome is unknown"; return out; }
        out.account_id = it->second.owner_account;
        try {
            // Never use polling coalescence to preserve a stale authority label.
            (void)RecheckActionAuthority(it->second);
            if (it->second.receipt.certificate_verified) {
                m_action_polls.Remove(it->first);
                return it->second.receipt;
            }
            auto refresh = [&](Pending& pending, bool automatic) {
                try { QueryAction(pending, automatic); } catch (const std::exception& e) {
                    pending.receipt.reason = e.what();
                    if (pending.previously_certified) {
                        pending.receipt.certificate_verified = false; pending.receipt.state = "unknown";
                    }
                }
            };
            if (retry) {
                refresh(it->second, false);
                // A fresh status lookup already occurred. Do not repeat it in
                // Send's no-resubmit path for a previously certified action.
                if (!it->second.receipt.certificate_verified && !it->second.previously_certified) return Send(it->second);
                if (!it->second.receipt.certificate_verified && it->second.previously_certified)
                    it->second.receipt.reason = "Previously verified inclusion is retained; fresh proof required for current authority, and this action will not be resubmitted";
                return it->second.receipt;
            }
            if (!m_action_polls.Demand(it->first)) Fail("Automatic status demand bound reached; retained instruction preserved");
            bool own_refresh{false};
            if (const auto key{m_action_polls.Take(std::chrono::steady_clock::now())}) {
                // Fair bounded work across callers/markets. Only the caller's
                // own receipt below is returned, never another wallet's card.
                const auto work{m_pending.find(*key)};
                if (work != m_pending.end()) {
                    own_refresh = *key == it->first;
                    refresh(work->second, true);
                    if (work->second.receipt.certificate_verified) m_action_polls.Remove(*key);
                } else m_action_polls.Remove(*key);
            }
            auto receipt{it->second.receipt};
            if (!receipt.certificate_verified && !m_action_polls.HasObservation(it->first))
                receipt.reason = "Status read pending/coalesced; retained submission observation only. " + receipt.reason;
            else if (!receipt.certificate_verified && !own_refresh)
                receipt.reason = "Status read coalesced; retained endpoint observation, not a fresh response. " + receipt.reason;
            return receipt;
        } catch (const std::exception& e) {
            if (it->second.previously_certified) {
                it->second.receipt.certificate_verified = false;
                it->second.receipt.state = "unknown";
            }
            it->second.receipt.reason = e.what();
            out.reason = e.what(); return out;
        }
    }
    std::vector<interfaces::FlowMeshSavedAction> SavedActions(
        const uint256& account, const std::optional<uint256>& market) override
    {
        FlowMeshTimingSpan timing_lock_1352{__func__};
        timing_lock_1352.Field("lock_name", std::string{"client_saved_view"});
        timing_lock_1352.Mark("lock_requested_us");
        std::shared_ptr<const std::vector<interfaces::FlowMeshSavedAction>> view;
        {
            std::lock_guard lock{m_saved_mutex};
            view = m_saved_view;
        }
        timing_lock_1352.Mark("lock_acquired_us");
        timing_lock_1352.Field("view_published", uint64_t{view != nullptr});
        if (account.IsNull()) return {};
        // The view reflects every backend method that already returned. A
        // method still running (for example a Submit whose network call is
        // in flight) is not reflected yet, so its action reads as not
        // retained until that Submit returns, instead of waiting for it.
        if (!view) {
            WorkLock lock{*this};
            return SavedActionsLocked(account, market);
        }
        std::vector<interfaces::FlowMeshSavedAction> out;
        for (const auto& row : *view) {
            if (row.account_id == account && (!market || row.market_id == *market)) out.push_back(row);
        }
        return out;
    }
    interfaces::FlowMeshClientStatus Status() const override
    {
        std::lock_guard lock{m_status_mutex}; return m_status;
    }
    FlowMeshClientReconnectResult Reconnect() override
    {
        FlowMeshClientReconnectResult out;
        out.status = "busy";
        out.error = "Another FlowMesh client request is running; retry this read-only command after it completes";
        WorkLock work{*this, std::try_to_lock};
        if (!work.owns_lock()) return out;
        if (m_endpoints.empty()) {
            out.status = "not_configured";
            out.error = "No FlowMesh HTTPS trading endpoint configured; use flowmeshclientconnect or -flowmeshendpoint";
            return out;
        }
        try {
            // An explicit probe opens fresh TLS with the existing trust and
            // bounded failover policy. This is availability, not certification.
            m_https.Reset();
            m_wait_reset = true;
            // Do not call Markets/Refresh/QueryAction/Send: a connection probe
            // must not touch market caches, account cursors or durable history.
            Call("markets", UniValue{UniValue::VOBJ}, [&](const UniValue& value, size_t) {
                if (!value.isArray() || value.size() > CLIENT_MAX_MARKETS) Fail("Market discovery exceeds bound");
                std::set<uint256> seen;
                for (const auto& row : value.getValues()) {
                    if (!row.isObject() || !seen.insert(Id(row, "market_id")).second)
                        Fail("Invalid or duplicate market identity in availability response");
                }
            }, nullptr, nullptr, &out.attempted_endpoints);
            out.status = "reachable";
            out.endpoint_available = true;
            out.endpoint = m_endpoints[m_selected].url;
            out.error.clear();
        } catch (const std::exception& e) {
            out.status = "unavailable";
            out.error = std::string{e.what()}.substr(0, 1024);
            // Every configured endpoint just failed its probe. Do not display
            // an old successful URL as the result of this reconnect attempt.
            std::lock_guard status{m_status_mutex};
            m_status.active_endpoint.clear();
        }
        return out;
    }
    std::optional<interfaces::FlowMeshPendingCheckpoint> Checkpoint(const uint256& id, std::string& error) override;
    std::vector<interfaces::FlowMeshVaultOperation> VaultOperations(const std::optional<uint256>& id, std::string& error) override;
    std::optional<interfaces::FlowMeshVaultOperation> VaultOperation(const uint256& id, std::string& error) override;
};

std::optional<interfaces::FlowMeshPendingCheckpoint> RemoteBackend::Checkpoint(const uint256& id, std::string& error)
{
    FlowMeshTimingSpan timing_lock_1431{__func__};
    timing_lock_1431.Field("lock_name", std::string{"client_work"});
    timing_lock_1431.Mark("lock_requested_us");
    WorkLock lock{*this};
    MarkWorkAcquired(timing_lock_1431);
    try {
        UniValue params{UniValue::VOBJ}; params.pushKV("market_id", id.GetHex());
        std::optional<interfaces::FlowMeshPendingCheckpoint> result;
        Call("checkpoint", params, [&](const UniValue& value, size_t) {
            const auto payload{Bytes(value, "payload", 4096)};
            result = VerifyClientCheckpoint(m_chainman, id, payload, error);
            if (!result) Fail(error);
        });
        return result;
    } catch (const std::exception& e) { error = e.what(); return std::nullopt; }
}
std::vector<interfaces::FlowMeshVaultOperation> RemoteBackend::VaultOperations(const std::optional<uint256>& id, std::string& error)
{
    FlowMeshTimingSpan timing_lock_1445{__func__};
    timing_lock_1445.Field("lock_name", std::string{"client_work"});
    timing_lock_1445.Mark("lock_requested_us");
    WorkLock lock{*this};
    MarkWorkAcquired(timing_lock_1445);
    try {
        UniValue params{UniValue::VOBJ}; if (id) params.pushKV("market_id", id->GetHex());
        std::vector<interfaces::FlowMeshVaultOperation> result;
        Call("vault_operations", params, [&](const UniValue& value, size_t) {
            if (!value.isArray() || value.size() > 128) Fail("Vault proof page exceeds bound");
            std::vector<interfaces::FlowMeshVaultOperation> candidate;
            for (const auto& row : value.getValues()) {
                const auto market{Id(row, "market_id")};
                if (id && market != *id) Fail("Wrong vault proof market");
                const auto payload{Bytes(row, "proof", modern::FLOWMESH_VAULT_PROOF_RECORD_MAX_SIZE)};
                auto operation{VerifyClientVaultOperation(m_chainman, market, std::nullopt, payload, error)};
                if (!operation) Fail(error);
                if (operation->checkpoint_id != Id(row, "checkpoint_id")) Fail("Vault proof checkpoint mismatch");
                candidate.push_back(std::move(*operation));
            }
            result = std::move(candidate);
        });
        return result;
    } catch (const std::exception& e) { error = e.what(); return {}; }
}
std::optional<interfaces::FlowMeshVaultOperation> RemoteBackend::VaultOperation(const uint256& id, std::string& error)
{
    FlowMeshTimingSpan timing_lock_1468{__func__};
    timing_lock_1468.Field("lock_name", std::string{"client_work"});
    timing_lock_1468.Mark("lock_requested_us");
    WorkLock lock{*this};
    MarkWorkAcquired(timing_lock_1468);
    try {
        UniValue params{UniValue::VOBJ}; params.pushKV("effect_id", id.GetHex());
        std::optional<interfaces::FlowMeshVaultOperation> result;
        Call("vault_operation", params, [&](const UniValue& value, size_t) {
            const auto payload{Bytes(value, "proof", modern::FLOWMESH_VAULT_PROOF_RECORD_MAX_SIZE)};
            result = VerifyClientVaultOperation(m_chainman, Id(value, "market_id"), id, payload, error);
            if (!result) Fail(error);
            if (result->checkpoint_id != Id(value, "checkpoint_id")) Fail("Vault proof checkpoint mismatch");
        });
        return result;
    } catch (const std::exception& e) { error = e.what(); return std::nullopt; }
}

} // namespace

std::unique_ptr<FlowMeshTradingBackend> MakeLocalFlowMeshBackend(FlowMeshService& service, FlowMeshAssetMetadataCatalog metadata)
{
    return std::make_unique<LocalBackend>(service, std::move(metadata));
}
std::unique_ptr<FlowMeshTradingBackend> MakeRemoteFlowMeshBackend(
    ChainstateManager& chainman, std::vector<HttpsEndpoint> endpoints, const fs::path& client_datadir, std::string& error,
    const FlowMeshJoinWindows& join_windows)
{
    try { return std::make_unique<RemoteBackend>(chainman, std::move(endpoints), client_datadir, join_windows); }
    catch (const std::exception& e) { error = e.what(); return {}; }
}
std::unique_ptr<FlowMeshHttpsServer> MakeFlowMeshTradingApi(FlowMeshService& service, FlowMeshHttpsServer::Options options,
                                                        FlowMeshAssetMetadataCatalog metadata, const FlowMeshTradingApiWait wait)
{
    options.max_reply_bytes = CLIENT_MAX_REPLY;
    auto api{std::make_shared<TradingApi>(service, std::move(metadata), wait)};
    // Handshake, read, handler and write concurrency stays at the two workers
    // the API always had. Each wait slot adds a worker that only sleeps in a
    // suspended wait. Disabled waits leave exactly two workers and permits.
    options.active_permits = FLOWMESH_API_ACTIVE_PERMITS;
    options.worker_threads = FLOWMESH_API_ACTIVE_PERMITS + api->WaitSlots();
    options.on_start = [api] { api->Reopen(); };
    options.on_stop = [api] { api->Interrupt(); };
    return std::make_unique<FlowMeshHttpsServer>(std::move(options), [api](const auto& request) { return api->Handle(request); });
}
} // namespace node
