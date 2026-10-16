// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <node/flowmesh_timing.h>
#include <node/flowmesh_client.h>
#include <node/flowmesh_action_wait.h>
#include <node/flowmesh_api_admission.h>
#include <node/flowmesh_client_action_wait.h>
#include <node/flowmesh_client_join.h>
#include <node/flowmesh_client_outcome.h>
#include <node/flowmesh_client_poll.h>
#include <node/flowmesh_client_readiness.h>
#include <node/flowmesh_client_read_admission.h>
#include <node/flowmesh_client_work.h>
#include <node/flowmesh_client_settlement.h>
#include <node/flowmesh_client_ticks.h>
#include <node/flowmesh_client_watch.h>
#include <node/flowmesh_health.h>

#include <chain.h>
#include <consensus/flowmesh_params.h>
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <dbwrapper.h>
#include <flowmesh/auth.h>
#include <flowmesh/action_reason.h>
#include <flowmesh/client_evidence.h>
#include <flowmesh/market_view.h>
#include <flowmesh/observed_time.h>
#include <flowmesh/outcome_evidence.h>
#include <flowmesh/production_wire.h>
#include <hash.h>
#include <logging.h>
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
#include <tinyformat.h>
#include <util/check.h>
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
#include <functional>
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
static_assert(FLOWMESH_API_CANDLES_DEFAULT == flowmesh::OBSERVED_CANDLE_PAGE_MAX);
// An opt-in reply adds at most the consumer's hex entry and two hex evidences.
constexpr size_t CLIENT_ACTION_OUTCOME_MAX_REPLY{2 * CLIENT_ACTION_WAIT_MAX_REPLY +
                                                 4 * flowmesh::FLOWMESH_OUTCOME_EVIDENCE_MAX_BYTES + 64 * 1024};
static_assert(CLIENT_ACTION_OUTCOME_MAX_REPLY <= CLIENT_MAX_REPLY);
constexpr size_t CLIENT_OUTCOME_EVIDENCE_MAX_REPLY{2 * flowmesh::FLOWMESH_OUTCOME_EVIDENCE_MAX_BYTES + 4096};
// A 'candles' page: at most 500 candles of a few hundred bytes.
constexpr size_t CLIENT_CANDLES_MAX_REPLY{1024 * 1024};
// One held newest candle per cached market and interval.
constexpr size_t CLIENT_MAX_HELD_CANDLES{CLIENT_MAX_CACHED_MARKETS * flowmesh::CANDLE_INTERVALS.size()};
// A waited submission's reply: its acknowledgement plus one (opt-in) 'action' reply.
constexpr size_t CLIENT_SUBMIT_WAIT_MAX_REPLY{CLIENT_ACTION_OUTCOME_MAX_REPLY + 64 * 1024};
static_assert(CLIENT_SUBMIT_WAIT_MAX_REPLY <= CLIENT_MAX_REPLY);
// A retained not-executed marker whose proof this process has not renewed.
constexpr const char* CLIENT_NOT_EXECUTED_RETAINED{
    "Previously proven not executed; fresh proof required, and this action will not be resubmitted"};
// A waited 'action' read ends at least this long before its HTTPS deadline,
// leaving time to fetch the certified payload and write the reply.
constexpr auto API_WAIT_REPLY_RESERVE{std::chrono::milliseconds{1000}};

[[noreturn]] void Fail(const std::string& reason) { throw std::runtime_error(reason); }
//! A spent public 'health' budget: HTTP 429, like the admission budget.
struct HealthBudgetExhausted : std::runtime_error {
    HealthBudgetExhausted() : std::runtime_error{FLOWMESH_API_HEALTH_BUDGET_ERROR} {}
};
//! A spent public 'candles' budget: HTTP 429 as well.
struct CandlesBudgetExhausted : std::runtime_error {
    CandlesBudgetExhausted() : std::runtime_error{FLOWMESH_API_CANDLES_BUDGET_ERROR} {}
};
void Keys(const UniValue& value, std::initializer_list<const char*> allowed)
{
    if (!value.isObject()) Fail("Expected a bounded object");
    std::set<std::string> seen;
    for (const auto& key : value.getKeys()) {
        if (!seen.insert(key).second || std::none_of(allowed.begin(), allowed.end(), [&](const char* name) { return key == name; }))
            Fail("Unknown or duplicate client API field");
    }
}
void Keys(const UniValue& value, const std::vector<const char*>& allowed)
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
    // Memory-only annotation: written only by the opt-in and local scopes.
    out.reason_code = std::string{flowmesh::ActionReasonName(event.reason_code)};
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
    //! The public trading API's backend: its 'submit' reply never states an
    //! outcome, so none is derived for it (anyone may replay public certified
    //! actions, and a derivation hashes a whole retained entry).
    const bool m_public_api;
    // Local RPC callers never take every event-log watch registration.
    static constexpr size_t LOCAL_WATCHERS_MAX{4};
    std::atomic<size_t> m_local_watchers{0};
    // Derived local outcomes, FIFO-bounded: a status poll never derives the
    // same outcome twice and never reads the store.
    static constexpr size_t OUTCOME_CACHE_ENTRIES{1024};
    std::mutex m_outcome_mutex;
    std::map<std::pair<uint256, uint256>, interfaces::FlowMeshActionOutcome> m_outcomes;
    std::deque<std::pair<uint256, uint256>> m_outcome_order;

    static void ApplyOutcome(Receipt& receipt, const interfaces::FlowMeshActionOutcome& outcome)
    {
        receipt.outcome = outcome;
        receipt.outcome_verified = true;
        receipt.reason_code = outcome.reason_code;
        if (outcome.status == "not_executed") {
            receipt.state = "rejected";
            receipt.certificate_verified = false;
            receipt.microblock_hash.SetNull();
            receipt.microblock_sequence = 0;
            receipt.reason = "Never executes: its account sequence was used by another certified action in microblock #" +
                             std::to_string(outcome.microblock_sequence);
        }
    }

    //! Verified outcome from this engine's own retained re-execution: for a
    //! certified inclusion, the derivation over its entry; otherwise, when a
    //! newer local notice names a consumer, the proof that the action never
    //! executes. Memory only; encodes and verifies outside runtime locks.
    void AttachOutcome(const uint256& market, Receipt& receipt)
    {
        const std::pair key{market, receipt.action_id};
        const bool included{receipt.state == "certified_inclusion" && receipt.certificate_verified};
        {
            std::lock_guard lock{m_outcome_mutex};
            if (const auto it{m_outcomes.find(key)}; it != m_outcomes.end()) {
                const bool usable{it->second.status == "not_executed"
                    ? !included : included && it->second.microblock_hash == receipt.microblock_hash};
                if (usable) {
                    ApplyOutcome(receipt, it->second);
                    return;
                }
            }
        }
        std::optional<interfaces::FlowMeshActionOutcome> derived;
        if (included) {
            const auto parts{m_service.ClientOutcomeParts(market, receipt.microblock_sequence, receipt.microblock_hash, true)};
            if (!parts.found || !parts.envelope || !parts.outcome || parts.outcome->too_large) return;
            const auto& entry{parts.envelope->entry};
            const auto action{std::find_if(entry.actions.begin(), entry.actions.end(),
                                           [&](const auto& item) { return item.Id() == receipt.action_id; })};
            if (action == entry.actions.end()) return;
            const auto evidence{FlowMeshRecordEvidence(*parts.outcome)};
            if (flowmesh::VerifyExecutionOutcomeEvidence(entry, evidence) != flowmesh::OutcomeEvidenceCheck::OK) return;
            const auto outcome{flowmesh::DeriveActionOutcome(entry, evidence, *action)};
            if (!outcome) return;
            derived = FlowMeshReceiptOutcome(*outcome, receipt.microblock_sequence, receipt.microblock_hash,
                                             "local_engine_execution");
        } else {
            const auto notice{m_service.NewestClientNoticeIfNewer(market, receipt.action_id)};
            if (!notice || !notice->consumed_by ||
                (notice->code != flowmesh::ActionReasonCode::STALE_SEQUENCE &&
                 notice->code != flowmesh::ActionReasonCode::DROPPED_SEQUENCE_CONSUMED)) return;
            const auto& consumer{*notice->consumed_by};
            const auto parts{m_service.ClientOutcomeParts(market, consumer.microblock_sequence, consumer.microblock_hash, true)};
            if (!parts.found || !parts.envelope || !parts.outcome || parts.outcome->too_large) return;
            const auto evidence{FlowMeshRecordEvidence(*parts.outcome)};
            if (flowmesh::VerifyExecutionOutcomeEvidence(parts.envelope->entry, evidence) != flowmesh::OutcomeEvidenceCheck::OK ||
                !flowmesh::ProvesSequenceConsumedByOther(parts.envelope->entry, evidence, receipt.action_id,
                                                         notice->account_id, notice->account_sequence, consumer.action_id)) {
                return;
            }
            derived = FlowMeshNotExecutedOutcome(consumer.action_id, consumer.microblock_sequence, consumer.microblock_hash);
        }
        {
            std::lock_guard lock{m_outcome_mutex};
            if (m_outcomes.insert_or_assign(key, *derived).second) {
                m_outcome_order.push_back(key);
                if (m_outcome_order.size() > OUTCOME_CACHE_ENTRIES) {
                    m_outcomes.erase(m_outcome_order.front());
                    m_outcome_order.pop_front();
                }
            }
        }
        ApplyOutcome(receipt, *derived);
    }
public:
    explicit LocalBackend(FlowMeshService& service, FlowMeshAssetMetadataCatalog metadata = {}, const bool public_api = false)
        : m_service(service), m_metadata(std::move(metadata)), m_public_api{public_api} {}
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
        if (const auto checkpoint{m_service.PendingCheckpointStatus(id, error)}) {
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
    //! This node's own observed-time candles. Only a local caller pages the
    //! disk beyond the memory window; the public API serves memory only.
    std::optional<flowmesh::ObservedCandlePage> Candles(const uint256& id, const flowmesh::CandleInterval interval,
                                                        const std::optional<int64_t> before_ms, const size_t limit,
                                                        std::string& error) override
    {
        return m_service.ObservedCandles(id, interval, before_ms, limit, /*allow_disk=*/!m_public_api, error);
    }
    std::optional<flowmesh::MarketData> Data(const uint256& id, const std::optional<uint256>& account,
                                          const flowmesh::MarketDataQuery& query, std::string& error) override
    {
        auto out{m_service.MarketData(id, account, query, error)};
        if (out) { out->certificate_verified = out->snapshot.certified; out->account_state_verified = out->snapshot.certified; out->execution_result_verified = out->snapshot.certified; }
        return out;
    }
    //! The local engine's own event log: no network. Each call holds its
    //! caller (an RPC worker) for at most the wait.
    std::optional<flowmesh::MarketData> Watch(const uint256& id, const std::optional<uint256>& account,
                                             const flowmesh::MarketDataQuery& query, const std::chrono::milliseconds requested,
                                             interfaces::FlowMeshMarketWatch& watch, std::string& error) override
    {
        watch = {};
        const auto wait{std::clamp(requested, std::chrono::milliseconds{0}, FLOWMESH_CLIENT_UPDATES_WATCH_MAX)};
        // The position is captured before the head is read, so a commit
        // after that read appends its head event after this position.
        const auto cursor{m_service.ClientEventCursor()};
        auto out{Data(id, account, query, error)};
        if (!out) {
            watch.status = "failed";
            return out;
        }
        // Only a caller that names the head it holds can be told when it
        // changes; anything else answers at once, so a script cannot spin.
        if (!cursor || wait.count() == 0 || !query.known_head || query.before_sequence || query.curve_cursor ||
            query.expected_head) {
            watch.status = "not_eligible";
            return out;
        }
        watch.supported = true;
        if (!out->unchanged) {
            watch.status = "head_changed";
            watch.waited = true;
            return out;
        }
        if (m_local_watchers.fetch_add(1) >= LOCAL_WATCHERS_MAX) {
            --m_local_watchers;
            watch.status = "busy";
            return out;
        }
        const auto result{m_service.WaitClientEvents(*cursor, id, account, std::chrono::steady_clock::now() + wait, {})};
        --m_local_watchers;
        watch.status = flowmesh::ClientWatchResultName(result);
        watch.waited = FlowMeshWatchWaited(watch.status);
        watch.gap = result == flowmesh::ClientWatchResult::GAP || result == flowmesh::ClientWatchResult::RESTARTED;
        return Data(id, account, query, error);
    }
    // The local engine's status is already current: a requested wait is
    // ignored, as by ActionStatus.
    Receipt Submit(const uint256& id, const flowmesh::Action& action,
                   std::chrono::milliseconds wait = std::chrono::milliseconds{0}) override
    {
        return SubmitFrom(id, action, {});
    }
    /** Submit on behalf of `submitter`, the public request's client identity
     * (FlowMeshActionWaitSlots::PeerKey), which bounds that client's share
     * of the public notice ring; unused for this node's own submissions. */
    Receipt SubmitFrom(const uint256& id, const flowmesh::Action& action, const std::string& submitter)
    {
        Receipt out; out.action_id = action.Id();
        // Public API traffic only ever fills the public notice ring.
        const auto submitted{m_public_api
            ? m_service.SubmitLocalActionChecked(id, action, flowmesh::ClientNoticeOrigin::PUBLIC, submitter)
            : m_service.SubmitLocalActionChecked(id, action, flowmesh::ClientNoticeOrigin::NODE)};
        if (submitted.refused) { out.state = "rejected"; out.reason = submitted.error; return out; }
        switch (submitted.admission.kind) {
        case FlowMeshLocalAdmission::Kind::ENQUEUE: break;
        case FlowMeshLocalAdmission::Kind::ALREADY_INCLUDED:
            // Answered from this engine's certified data: never "rejected".
            out.reason_code = std::string{flowmesh::ActionReasonName(submitted.admission.code)};
            out.state = "certified_inclusion";
            out.microblock_sequence = submitted.admission.included->first;
            out.microblock_hash = submitted.admission.included->second;
            out.certificate_verified = true;
            out.reason = FlowMeshLocalAdmissionText(submitted.admission);
            if (!m_public_api) AttachOutcome(id, out);
            return out;
        case FlowMeshLocalAdmission::Kind::REFUSE:
            out.state = "rejected";
            out.reason = FlowMeshLocalAdmissionText(submitted.admission);
            out.reason_code = std::string{flowmesh::ActionReasonName(submitted.admission.code)};
            return out;
        case FlowMeshLocalAdmission::Kind::UNKNOWN_OUTCOME:
            out.state = "unknown";
            out.reason = FlowMeshLocalAdmissionText(submitted.admission);
            out.reason_code = std::string{flowmesh::ActionReasonName(submitted.admission.code)};
            return out;
        }
        out.state = "queued";
        if (const auto status{m_service.ClientActionStatus(id, action.Id())}) {
            const auto exact{flowmesh::EncodeProductionActionPayload(action)};
            // A refusal belongs to one exact credential-bearing attempt, and
            // to one appended after this attempt was queued: an older
            // attempt's refusal (or anyone's for the same credential-free
            // deposit) is never attributed to this queue item.
            const auto& before{submitted.cursor_before};
            const auto now{m_service.ClientEventCursor()};
            const bool this_attempt{exact && status->signed_action_hash == Hash(*exact) && before && now &&
                                    now->instance_id == before->instance_id &&
                                    status->event_id > before->event_id};
            if (status->kind != flowmesh::ClientEventKind::POOL_REFUSED || this_attempt) out = EventReceipt(*status);
        }
        if (out.state == "certified_inclusion") out.certificate_verified = true;
        if (!m_public_api) AttachOutcome(id, out);
        return out;
    }
    // The local engine's status is already current: there is no transport to
    // hide, so a requested wait is ignored and the read returns immediately.
    Receipt ActionStatus(const uint256& id, const uint256& action, bool retry,
                         std::chrono::milliseconds wait = std::chrono::milliseconds{0}) override
    {
        auto out{LocalActionReceipt(m_service.ClientActionStatus(id, action), action)};
        if (out.state != "certified_inclusion") {
            // This engine's own recent certified window still knows an
            // inclusion whose event left the ring.
            if (const auto included{m_service.ClientRecentInclusion(id, action)}) {
                out = LocalActionReceipt(std::nullopt, action);
                out.state = "certified_inclusion";
                out.microblock_sequence = included->first;
                out.microblock_hash = included->second;
                out.certificate_verified = true;
                out.reason = "certified inclusion retained by this engine";
            }
        }
        AttachOutcome(id, out);
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
    // Zero unless that wait is also offered on 'submit'.
    const std::chrono::milliseconds m_submit_wait_max;
    FlowMeshActionWaitSlots m_waits;
    // Zero when the bounded 'updates' watch is disabled. A separate slot
    // pool: a crowd of watchers never takes an action wait's slot.
    const std::chrono::milliseconds m_watch_max;
    FlowMeshActionWaitSlots m_watches;
    //! Pre-parse trade-write and read budgets (see flowmesh_api_admission.h).
    FlowMeshApiAdmission m_admission;
    struct Bucket { std::chrono::steady_clock::time_point time{}; unsigned count{0}; };
    // Public 'health' method; disabled keeps the exact previous contract.
    const bool m_health;
    // Opt-in verifiable outcomes; disabled keeps the exact previous contract.
    const bool m_outcome;
    // Largest clearing-tick page; zero keeps the exact previous contract.
    const size_t m_ticks_max;
    // Operator-observed time; disabled keeps the exact previous contract.
    const bool m_observed_time;
    // Largest 'candles' page; zero keeps the exact previous contract.
    const size_t m_candles_max;
    std::mutex m_candles_mutex;
    //! Per PeerKey, at most 1,024 identities; guarded by m_candles_mutex.
    std::map<std::string, std::pair<std::chrono::steady_clock::time_point, unsigned>> m_candle_clients;
    //! Caller holds m_candles_mutex. Charged after Admit, per client address.
    bool ChargeCandles(const std::string& remote_address)
    {
        const auto now{std::chrono::steady_clock::now()};
        auto key{FlowMeshActionWaitSlots::PeerKey(remote_address)};
        if (!m_candle_clients.contains(key) && m_candle_clients.size() >= 1024) {
            std::erase_if(m_candle_clients, [&](const auto& pair) { return now - pair.second.first > std::chrono::minutes{1}; });
            if (m_candle_clients.size() >= 1024) return false;
        }
        auto& bucket{m_candle_clients[std::move(key)]};
        if (now - bucket.first >= std::chrono::seconds{1}) bucket = {now, 0};
        return ++bucket.second <= FLOWMESH_API_CANDLES_PER_CLIENT_PER_SECOND;
    }
    //! A public reply's observed times: labelled as this endpoint's, with
    //! only the clock flags (never a delay reason or anything seat-related)
    //! and without this node's storage detail.
    static void PublicObservedTimes(flowmesh::MarketData& data)
    {
        if (!data.observed) return;
        data.observed->source = "endpoint_operator";
        data.observed->storage.reset();
        for (auto& row : data.observed->rows) {
            row.flags &= flowmesh::OBSERVED_PUBLIC_FLAGS;
            row.delay = flowmesh::ObservedDelay::NONE;
        }
    }
    //! A "ticks" parameter: its limit is clamped to the advertised maximum.
    flowmesh::ClearingTickQuery ParseTicks(const UniValue& value) const
    {
        Keys(value, {"after_sequence", "limit"});
        flowmesh::ClearingTickQuery out;
        if (value.exists("after_sequence")) out.after_sequence = Number(value, "after_sequence", std::numeric_limits<uint64_t>::max());
        const uint64_t limit{value.exists("limit") ? Number(value, "limit", std::numeric_limits<uint64_t>::max())
                                                   : uint64_t{flowmesh::MARKET_DATA_DEFAULT_TICKS}};
        if (limit == 0) Fail("Invalid clearing-tick limit");
        out.limit = static_cast<size_t>(std::min<uint64_t>(limit, m_ticks_max));
        return out;
    }
    std::mutex m_health_mutex;
    //! Per PeerKey, at most 1,024 identities; guarded by m_health_mutex.
    std::map<std::string, Bucket> m_health_clients;
    //! Guarded by m_health_mutex.
    std::optional<std::pair<std::chrono::steady_clock::time_point, UniValue>> m_health_cache;
    //! Caller holds m_health_mutex. Charged after Admit, per client address.
    bool ChargeHealth(const std::string& remote_address)
    {
        const auto now{std::chrono::steady_clock::now()};
        auto key{FlowMeshActionWaitSlots::PeerKey(remote_address)};
        if (!m_health_clients.contains(key) && m_health_clients.size() >= 1024) {
            std::erase_if(m_health_clients, [&](const auto& pair) { return now - pair.second.time > std::chrono::minutes{1}; });
            if (m_health_clients.size() >= 1024) return false;
        }
        auto& bucket{m_health_clients[std::move(key)]};
        if (now - bucket.time >= std::chrono::seconds{1}) { bucket.time = now; bucket.count = 0; }
        return ++bucket.count <= FLOWMESH_API_HEALTH_PER_CLIENT_PER_SECOND;
    }
    //! Advisory routing hint in a status object, never evidence (the same
    //! class as action_wait_ms_max). Absent when health is disabled.
    void AddReadiness(UniValue& out, const uint256& market, const FlowMeshServiceHealth* health) const
    {
        if (!m_health) return;
        std::optional<FlowMeshServiceHealth> own;
        if (!health) health = &own.emplace(m_service.Health(market));
        const auto* row{health->Find(market)};
        const auto readiness{row ? row->readiness
            : EvaluateFlowMeshMarketReadiness(health->service_not_ready, nullptr, health->now, health->policy)};
        UniValue reasons{UniValue::VARR};
        for (const auto reason : readiness.not_ready) reasons.push_back(FlowMeshNotReadyName(reason));
        out.pushKV("health_api_version", FLOWMESH_HEALTH_VERSION);
        out.pushKV("endpoint_ready", readiness.ready);
        out.pushKV("endpoint_not_ready", std::move(reasons));
    }
    UniValue MarketResponse(const MarketStatus& status, const FlowMeshServiceHealth* health = nullptr) const
    {
        auto out{MarketJson(status)};
        if (const auto metadata{m_local.Metadata(status.base_asset)}) {
            out.pushKV("asset_display", FlowMeshAssetMetadataJson(status.domain, status.base_asset, *metadata));
        }
        // Latency capability, never evidence: clients send wait_ms only to an
        // endpoint that advertises it, since older servers reject the field.
        if (m_wait_max.count()) out.pushKV("action_wait_ms_max", uint64_t(m_wait_max.count()));
        // The same for wait_ms on 'submit'.
        if (m_submit_wait_max.count()) out.pushKV("submit_wait_ms_max", uint64_t(m_submit_wait_max.count()));
        // The same for watch_ms on 'updates'.
        if (m_watch_max.count()) out.pushKV("updates_watch_ms_max", uint64_t(m_watch_max.count()));
        // Capability, never evidence: clients send "outcome" only here.
        if (m_outcome) out.pushKV("action_outcome_v1", true);
        // The same for "ticks" on 'snapshot' and unwatched 'updates'.
        if (m_ticks_max) out.pushKV("clearing_ticks_max", uint64_t{m_ticks_max});
        // And for "observed_time" on the same reads.
        if (m_observed_time) out.pushKV("observed_time_v1", true);
        // The 'candles' method and its largest page.
        if (m_candles_max) out.pushKV("observed_candles_max", uint64_t{m_candles_max});
        AddReadiness(out, status.market_id, health);
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
        if (const auto checkpoint{m_service.PendingCheckpointStatus(data.market_id, error)}) {
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
    //! Bounded wait for the exact action's terminal status on this node.
    //! Returns the wait_status label; replaces receipt only after a wait.
    std::string WaitForAction(const FlowMeshHttpsServer::Request& http, const uint256& market,
                              const uint256& action_id, const bool tracked, const std::chrono::milliseconds wait,
                              Receipt& receipt, FlowMeshTimingSpan& timing, const bool include_notices = false)
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
            [&] { return m_waits.Interrupted(*slot); }, include_notices)};
        if (http.resume) http.resume();
        timing.Mark("wait_completed_us");
        receipt = LocalActionReceipt(result.status, action_id);
        return flowmesh::ClientWaitResultName(result.result);
    }
    //! Bounded wait until the page after `after` would hold an event for this
    //! filter (or a gap). Returns the watch_status label. An already
    //! answerable watch never takes a slot or sleeps.
    std::string WaitForUpdates(const FlowMeshHttpsServer::Request& http, const uint256& market,
                               const std::optional<uint256>& account, const flowmesh::ClientEventCursor& after,
                               const std::chrono::milliseconds wait, FlowMeshTimingSpan& timing)
    {
        if (const auto now{m_service.ClientEventsPending(after, market, account)}; now != flowmesh::ClientWatchResult::TIMEOUT) {
            return flowmesh::ClientWatchResultName(now);
        }
        const auto start{std::chrono::steady_clock::now()};
        const auto until{std::min(start + wait, http.deadline - API_WAIT_REPLY_RESERVE)};
        if (until <= start) return "deadline";
        auto slot{m_watches.TryAcquire(http.remote_address)};
        if (!slot) return "busy";
        timing.Mark("watch_started_us");
        // Asleep, this request holds no HTTPS permit and no market, store or
        // event-log lock; the server also resumes a handler that leaves by
        // an exception.
        if (http.suspend) http.suspend();
        const auto result{m_service.WaitClientEvents(after, market, account, until,
            [&] { return m_watches.Interrupted(*slot); })};
        if (http.resume) http.resume();
        timing.Mark("watch_completed_us");
        return flowmesh::ClientWatchResultName(result);
    }
    //! A live page, never history paging: the ordinary event page and status
    //! object, read after the wait in the unwaited order, plus the
    //! watch_status latency hint. It carries no reported_data.
    UniValue WatchedUpdates(const FlowMeshHttpsServer::Request& http, const UniValue& params, const uint256& id,
                            const std::optional<uint256>& account,
                            const std::optional<flowmesh::ClientEventCursor>& after, FlowMeshTimingSpan& timing)
    {
        if (params.exists("before_sequence") || params.exists("limit")) Fail("watch_ms cannot be combined with history paging");
        // Any unsigned integer is clamped to the advertised maximum.
        const std::chrono::milliseconds wait{
            std::min<uint64_t>(Number(params, "watch_ms", std::numeric_limits<uint64_t>::max()), m_watch_max.count())};
        const std::optional<uint256> known{params.exists("known_head") ? std::optional{Id(params, "known_head", true)} : std::nullopt};
        flowmesh::MarketDataQuery status_only;
        status_only.status_only = true;
        std::string error;
        std::string watch_status{"none"};
        if (wait.count() > 0) {
            if (!after) {
                // No position: the page is the whole retained log, at once.
                watch_status = "no_cursor";
            } else {
                // Also refuses an unknown market before any slot is taken.
                const auto current{m_local.Data(id, account, status_only, error)};
                if (!current) Fail(error.empty() ? "No market-data view is available" : error);
                watch_status = known && *known != current->snapshot.last_microblock_hash
                    ? "head_changed" : WaitForUpdates(http, id, account, *after, wait, timing);
            }
        }
        const auto page{m_service.ClientEvents(after, id, account, flowmesh::CLIENT_EVENT_PAGE_MAX)};
        const auto data{m_local.Data(id, account, status_only, error)};
        if (!data) Fail(error.empty() ? "No market-data view is available" : error);
        UniValue out{FlowMeshClientUpdatesPageJson(page)};
        out.pushKV("status", MarketResponse(*data));
        out.pushKV("watch_status", watch_status);
        timing.Field("watch_status", watch_status);
        return out;
    }
    //! Encodes one retained record outside every runtime lock.
    static std::optional<std::string> EvidenceHex(const ProductionOutcomeRecord& record)
    {
        if (record.too_large) return std::nullopt;
        const auto bytes{flowmesh::EncodeExecutionOutcomeEvidenceV1(record.sequence, record.entry_hash,
                                                                     record.preimage, record.effects)};
        return bytes ? std::optional{HexStr(*bytes)} : std::nullopt;
    }
    //! The opt-in outcome parts of an 'action' reply. Every part comes from
    //! the runtime's in-memory window (never the store) and costs one more
    //! admission token; a refused token degrades to *_error "BUDGET".
    void AppendOutcomeFields(UniValue& out, const FlowMeshHttpsServer::Request& http, const uint256& market,
                             const uint256& action_id, const Receipt& receipt)
    {
        const auto charge = [&] {
            return m_admission.ChargeExtra(http.remote_address, FlowMeshApiRequestClass::READ,
                                           std::chrono::steady_clock::now());
        };
        if (receipt.state == "certified_inclusion") {
            if (!charge()) {
                out.pushKV("outcome_evidence_error", "BUDGET");
            } else {
                const auto parts{m_service.ClientOutcomeParts(market, receipt.microblock_sequence, receipt.microblock_hash, false)};
                if (!parts.found || !parts.outcome) {
                    out.pushKV("outcome_evidence_error", "EXPIRED");
                } else if (const auto hex{EvidenceHex(*parts.outcome)}) {
                    out.pushKV("outcome_evidence", *hex);
                } else {
                    out.pushKV("outcome_evidence_error", "TOO_LARGE");
                }
            }
        }
        const auto notice{m_service.NewestClientNoticeIfNewer(market, action_id)};
        if (!notice) return;
        UniValue pool{UniValue::VOBJ};
        pool.pushKV("reason_code", std::string{flowmesh::ActionReasonName(notice->code)});
        pool.pushKV("account_sequence", notice->account_sequence);
        pool.pushKV("next_account_sequence", notice->next_account_sequence);
        if (notice->consumed_by) {
            UniValue consumer{UniValue::VOBJ};
            consumer.pushKV("action_id", notice->consumed_by->action_id.GetHex());
            consumer.pushKV("microblock_sequence", notice->consumed_by->microblock_sequence);
            consumer.pushKV("microblock_hash", notice->consumed_by->microblock_hash.GetHex());
            pool.pushKV("consumed_by", std::move(consumer));
        }
        out.pushKV("pool_notice", std::move(pool));
        if (!notice->consumed_by) return;
        // The consumer's certified entry and its outcome evidence, so a client
        // can prove this action never executes.
        if (!charge()) {
            out.pushKV("consumer_evidence_error", "BUDGET");
            return;
        }
        const auto parts{m_service.ClientOutcomeParts(market, notice->consumed_by->microblock_sequence,
                                                      notice->consumed_by->microblock_hash, true)};
        if (!parts.found || !parts.envelope || !parts.outcome) {
            out.pushKV("consumer_evidence_error", "EXPIRED");
            return;
        }
        const auto payload{flowmesh::EncodeProductionCertifiedPayload(*parts.envelope, parts.seat_count)};
        const auto evidence{EvidenceHex(*parts.outcome)};
        if (!payload || !evidence) {
            out.pushKV("consumer_evidence_error", "TOO_LARGE");
            return;
        }
        out.pushKV("consumer_certified_payload", HexStr(*payload));
        out.pushKV("consumer_outcome_evidence", *evidence);
    }
    //! The 'action' reply for one ActionId: its receipt after an optional
    //! bounded wait, the durable certified entry (or why it is unavailable)
    //! for a certified inclusion, the opt-in outcome parts, and wait_status
    //! only when a wait was requested.
    UniValue ActionReply(const FlowMeshHttpsServer::Request& http, const uint256& id, const uint256& action_id,
                         const std::optional<std::chrono::milliseconds> wait, const bool outcome, FlowMeshTimingSpan& timing)
    {
        // Same receipt as the local backend's ActionStatus(id, action, false).
        const auto status{m_service.ClientActionStatus(id, action_id)};
        auto receipt{LocalActionReceipt(status, action_id)};
        // Opt-in only: an executed action whose inclusion event left the
        // ring is still answered from the in-memory certified window.
        const auto recent_inclusion = [&] {
            if (!outcome || receipt.state == "certified_inclusion") return;
            if (const auto included{m_service.ClientRecentInclusion(id, action_id)}) {
                receipt = LocalActionReceipt(std::nullopt, action_id);
                receipt.state = "certified_inclusion";
                receipt.microblock_sequence = included->first;
                receipt.microblock_hash = included->second;
                receipt.certificate_verified = true;
                receipt.reason = "certified inclusion retained by this endpoint; execution outcome established only by outcome evidence";
            }
        };
        recent_inclusion();
        std::string wait_status{"none"};
        if (wait && wait->count() > 0 && receipt.state != "certified_inclusion") {
            // An opt-in wait also ends on a newer refusal or drop notice.
            const bool tracked{status.has_value() || (outcome && m_service.NewestClientNoticeIfNewer(id, action_id))};
            wait_status = WaitForAction(http, id, action_id, tracked, *wait, receipt, timing, outcome);
            recent_inclusion();
        }
        UniValue out{FlowMeshClientReceiptJson(receipt, outcome ? FlowMeshReceiptJsonScope::PUBLIC_OUTCOME_V1
                                                                : FlowMeshReceiptJsonScope::PUBLIC_V1)};
        if (receipt.state == "certified_inclusion") {
            // Never built from the event: this waits for the commit's
            // market lock and reads the durable certified entry.
            std::string error;
            if (const auto payload{m_service.ClientCertifiedEntry(id, receipt.microblock_sequence, error)}) out.pushKV("certified_payload", HexStr(*payload));
            else out.pushKV("evidence_error", error);
        }
        if (outcome) AppendOutcomeFields(out, http, id, action_id, receipt);
        if (wait) {
            // A latency hint only, never evidence; absent unless requested.
            out.pushKV("wait_status", wait_status);
            timing.Field("wait_status", wait_status);
        }
        return out;
    }
    //! A waited 'submit', after its submission receipt was built: adds the
    //! 'action' reply for the same ActionId after the same bounded wait, in
    //! the same slots ("action"), and "wait_status" (a latency hint, never
    //! evidence: none for a refused submission, budget, failed, or the
    //! embedded reply's). A refused submission never waits. The embedded
    //! read costs one more request token, like the separate 'action' read
    //! it replaces; a spent budget omits it. Never fails the request and
    //! never changes the submission receipt, so a client always learns what
    //! this request admitted.
    void SubmitWait(const FlowMeshHttpsServer::Request& http, const uint256& id, const uint256& action_id,
                    const Receipt& submitted, const std::chrono::milliseconds wait, const bool outcome,
                    FlowMeshTimingSpan& timing, UniValue& out)
    {
        std::string wait_status{"none"};
        if (submitted.state == "rejected") {
            // Nothing admitted to wait for.
        } else if (!m_admission.ChargeExtra(http.remote_address, FlowMeshApiRequestClass::READ, std::chrono::steady_clock::now())) {
            wait_status = "budget";
        } else {
            try {
                auto action{ActionReply(http, id, action_id, wait, outcome, timing)};
                wait_status = action["wait_status"].get_str();
                out.pushKV("action", std::move(action));
            } catch (const std::exception&) {
                // Never build more of the reply without the permit a wait
                // released; resuming is idempotent.
                if (http.resume) http.resume();
                wait_status = "failed";
            }
        }
        out.pushKV("wait_status", wait_status);
        timing.Field("submit_wait_status", wait_status);
    }
    UniValue HealthReply(const UniValue& params, const FlowMeshHttpsServer::Request& http)
    {
        // A local, advisory observation, never evidence. Cheap: leaf
        // mutexes only, never cs_main, a market lock, the store, a wait
        // slot or a suspend.
        Keys(params, {"market_id"});
        const std::optional<uint256> market{params.exists("market_id") ? std::optional{Id(params, "market_id")} : std::nullopt};
        std::lock_guard lock{m_health_mutex};
        if (!ChargeHealth(http.remote_address)) throw HealthBudgetExhausted{};
        if (market) {
            const auto health{m_service.Health(*market)};
            if (!health.Find(*market)) Fail("Unknown market");
            return FlowMeshServiceHealthJson(health, /*operator_detail=*/false);
        }
        const auto now{std::chrono::steady_clock::now()};
        if (!m_health_cache || now - m_health_cache->first >= FLOWMESH_API_HEALTH_CACHE) {
            m_health_cache.emplace(now, FlowMeshServiceHealthJson(m_service.Health(), /*operator_detail=*/false));
        }
        return m_health_cache->second;
    }
public:
    explicit TradingApi(FlowMeshService& service, FlowMeshAssetMetadataCatalog metadata, const FlowMeshTradingApiWait wait,
                        const FlowMeshTradingApiHealth health, const FlowMeshTradingApiOutcome outcome,
                        const FlowMeshTradingApiWatch watch, const FlowMeshTradingApiTicks ticks,
                        const FlowMeshTradingApiObservedTime observed, const FlowMeshTradingApiCandles candles)
        : m_service(service), m_local(service, std::move(metadata), /*public_api=*/true),
          m_wait_max{wait.waiters == 0 ? std::chrono::milliseconds{0}
                                       : std::clamp(wait.max, std::chrono::milliseconds{0}, FLOWMESH_API_ACTION_WAIT_MAX)},
          m_submit_wait_max{wait.submit ? m_wait_max : std::chrono::milliseconds{0}},
          m_waits{m_wait_max.count() ? std::min(wait.waiters, FLOWMESH_API_ACTION_WAITERS_MAX) : 0},
          // Whatever the caller asks, both pools together never exceed the
          // sleeping workers the HTTPS server's bound leaves.
          m_watch_max{std::min({watch.watchers, FLOWMESH_API_UPDATES_WATCHERS_MAX, FLOWMESH_API_MAX_WAIT_SLOTS - m_waits.Capacity()}) == 0
                          ? std::chrono::milliseconds{0}
                          : std::clamp(watch.max, std::chrono::milliseconds{0}, FLOWMESH_API_UPDATES_WATCH_MAX)},
          m_watches{m_watch_max.count() ? std::min({watch.watchers, FLOWMESH_API_UPDATES_WATCHERS_MAX,
                                                    FLOWMESH_API_MAX_WAIT_SLOTS - m_waits.Capacity()}) : 0},
          m_health{health.enabled}, m_outcome{outcome.enabled},
          m_ticks_max{std::min(ticks.max, flowmesh::MARKET_DATA_MAX_TICKS)}, m_observed_time{observed.enabled},
          m_candles_max{std::min(candles.max, flowmesh::OBSERVED_CANDLE_PAGE_MAX)} {}
    size_t WaitSlots() const { return m_waits.Capacity(); }
    size_t WatchSlots() const { return m_watches.Capacity(); }
    //! Server stop hook: refuse new waits and watches and end every current one.
    void Interrupt()
    {
        m_waits.Close();
        m_watches.Close();
        m_service.WakeClientWaiters();
    }
    //! Server start hook.
    void Reopen()
    {
        m_waits.Open();
        m_watches.Open();
    }
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
            // One health observation for every row.
            const auto health{m_health ? std::optional{m_service.Health()} : std::nullopt};
            UniValue out{UniValue::VARR};
            for (const auto& value : m_local.Markets(std::nullopt)) out.push_back(MarketResponse(value, health ? &*health : nullptr));
            return out;
        }
        if (method == "snapshot") {
            // Disabled, the contract is exactly the previous one, in which
            // ticks (or observed_time) is an unknown field.
            std::vector<const char*> allowed{"market_id", "account_id"};
            if (m_ticks_max) allowed.push_back("ticks");
            if (m_observed_time) allowed.push_back("observed_time");
            Keys(params, allowed);
            const auto id{Id(params, "market_id")};
            const std::optional<uint256> account{params.exists("account_id") ? std::optional{Id(params, "account_id")} : std::nullopt};
            flowmesh::MarketDataQuery reported;
            if (params.exists("ticks")) reported.ticks = ParseTicks(params["ticks"]);
            if (params.exists("observed_time")) reported.observed_time = Flag(params, "observed_time");
            std::string error;
            auto view{m_service.ClientSnapshotView(id, account, error, reported)};
            if (!view) Fail(error.empty() ? "No certified state snapshot is available" : error);
            PublicObservedTimes(view->reported);
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
            // Disabled, the contract is exactly the previous one, in which
            // watch_ms is an unknown field.
            // Likewise ticks.
            // Likewise observed_time.
            std::vector<const char*> allowed{"market_id", "account_id", "cursor", "known_head", "before_sequence", "limit"};
            if (m_watch_max.count()) allowed.push_back("watch_ms");
            if (m_ticks_max) allowed.push_back("ticks");
            if (m_observed_time) allowed.push_back("observed_time");
            Keys(params, allowed);
            const auto id{Id(params, "market_id")};
            const std::optional<uint256> account{params.exists("account_id") ? std::optional{Id(params, "account_id")} : std::nullopt};
            const std::optional<flowmesh::ClientEventCursor> after{params.exists("cursor") ? std::optional{ParseCursor(params["cursor"])} : std::nullopt};
            if (params.exists("watch_ms")) {
                // A watched page has no reported_data to carry them.
                if (params.exists("ticks")) Fail("ticks cannot be combined with watch_ms");
                if (params.exists("observed_time")) Fail("observed_time cannot be combined with watch_ms");
                return WatchedUpdates(http, params, id, account, after, timing);
            }
            const auto page{m_service.ClientEvents(after, id, account, flowmesh::CLIENT_EVENT_PAGE_MAX)};
            UniValue out{FlowMeshClientUpdatesPageJson(page)};
            flowmesh::MarketDataQuery query;
            if (params.exists("known_head")) query.known_head = Id(params, "known_head", true);
            if (params.exists("before_sequence")) { query.before_sequence = Number(params, "before_sequence"); query.known_head.reset(); }
            if (params.exists("limit")) query.limit = Number(params, "limit", flowmesh::MARKET_DATA_MAX_HISTORY);
            if (params.exists("ticks")) query.ticks = ParseTicks(params["ticks"]);
            if (params.exists("observed_time")) query.observed_time = Flag(params, "observed_time");
            std::string error;
            auto data{m_local.Data(id, account, query, error)};
            if (!data) Fail(error.empty() ? "No market-data view is available" : error);
            PublicObservedTimes(*data);
            out.pushKV("status", MarketResponse(*data));
            out.pushKV("reported_data", FlowMeshClientMarketDataJson(*data));
            return out;
        }
        if (method == "submit") {
            // Not offered: exactly the previous contract, in which wait_ms and
            // outcome are unknown fields.
            if (m_submit_wait_max.count() == 0) Keys(params, {"market_id", "action_id", "action_hex"});
            else if (!m_outcome) Keys(params, {"market_id", "action_id", "action_hex", "wait_ms"});
            else Keys(params, {"market_id", "action_id", "action_hex", "wait_ms", "outcome"});
            // Parsed before the action, like every other check: an error
            // reply always means this request admitted nothing.
            const bool outcome{params.exists("outcome") && Flag(params, "outcome")};
            if (params.exists("outcome") && !params.exists("wait_ms")) Fail("outcome on 'submit' applies only with wait_ms");
            // Any unsigned integer is clamped to the advertised maximum.
            const std::optional<std::chrono::milliseconds> wait{params.exists("wait_ms")
                ? std::optional{std::chrono::milliseconds{
                      std::min<uint64_t>(Number(params, "wait_ms", std::numeric_limits<uint64_t>::max()), m_submit_wait_max.count())}}
                : std::nullopt};
            const auto id{Id(params, "market_id")};
            const auto action_id{Id(params, "action_id")};
            // The public submit reply keeps its v1 meaning: an endpoint never
            // states a verified outcome there (clients ignore endpoint
            // booleans), and m_local derives none for it. Without wait_ms it
            // is exactly the previous reply.
            const auto reply = [&](Receipt receipt) {
                receipt.outcome.reset();
                receipt.outcome_verified = false;
                UniValue out{FlowMeshClientReceiptJson(receipt)};
                if (wait) SubmitWait(http, id, action_id, receipt, *wait, outcome, timing, out);
                return out;
            };
            const auto payload{Bytes(params, "action_hex", flowmesh::FLOWMESH_ACTION_MAX_BYTES)};
            const auto action{flowmesh::DecodeProductionActionPayload(payload)};
            if (!action || action->Id() != action_id) Fail("Invalid exact signed action or ActionId");
            if (action->type == static_cast<uint8_t>(flowmesh::ActionType::CLAIM_SEAT_REWARD)) Fail("Operator reward claims are not a public trading operation");
            // Orders and (unauthenticated, chain-authorized) deposits spend a
            // sub-budget of the write class, before any Schnorr work, so an
            // order flood leaves room for cancels and withdrawals.
            const auto type{static_cast<flowmesh::ActionType>(action->type)};
            if ((type == flowmesh::ActionType::SUBMIT_BID || type == flowmesh::ActionType::SUBMIT_ASK || action->IsDeposit()) &&
                !m_admission.ChargeOrder(http.remote_address, std::chrono::steady_clock::now())) {
                // A lost-reply resend of an already-certified action still
                // gets its certified inclusion rather than the budget error.
                const auto status{m_service.ClientActionStatus(id, action_id)};
                if (status && status->kind == flowmesh::ClientEventKind::CERTIFIED_INCLUDED) {
                    return reply(LocalActionReceipt(status, action_id));
                }
                Fail("Public trading order budget exhausted; cancels and withdrawals are still accepted");
            }
            const auto market{m_service.Market(id)};
            if (!market) Fail("Unknown market");
            if (!action->IsDeposit() && !flowmesh::SchnorrActionAuthenticator(market->domain, market->execution_config_id).Authenticate(*action))
                Fail("Action authentication failed for this chain/domain/market configuration");
            // The client's identity bounds its share of the public notices.
            return reply(m_local.SubmitFrom(id, *action, FlowMeshActionWaitSlots::PeerKey(http.remote_address)));
        }
        if (method == "action") {
            // Disabled, the contract is exactly the previous one, in which
            // wait_ms and outcome are unknown fields.
            if (m_wait_max.count() == 0 && !m_outcome) Keys(params, {"market_id", "action_id"});
            else if (!m_outcome) Keys(params, {"market_id", "action_id", "wait_ms"});
            else if (m_wait_max.count() == 0) Keys(params, {"market_id", "action_id", "outcome"});
            else Keys(params, {"market_id", "action_id", "wait_ms", "outcome"});
            const auto id{Id(params, "market_id")};
            const auto action_id{Id(params, "action_id")};
            // "outcome": false is exactly the v1 request.
            const bool outcome{params.exists("outcome") && Flag(params, "outcome")};
            // Any unsigned integer is clamped to the advertised maximum.
            const std::optional<std::chrono::milliseconds> wait{params.exists("wait_ms")
                ? std::optional{std::chrono::milliseconds{
                      std::min<uint64_t>(Number(params, "wait_ms", std::numeric_limits<uint64_t>::max()), m_wait_max.count())}}
                : std::nullopt};
            return ActionReply(http, id, action_id, wait, outcome, timing);
        }
        if (method == "outcome_evidence" && m_outcome) {
            // Memory only: the runtime's retained re-execution record, never
            // a payload and never a store read. One extra admission token.
            Keys(params, {"market_id", "microblock_sequence", "microblock_hash"});
            const auto id{Id(params, "market_id")};
            const auto sequence{Number(params, "microblock_sequence")};
            const auto hash{Id(params, "microblock_hash")};
            if (!m_admission.ChargeExtra(http.remote_address, FlowMeshApiRequestClass::READ, std::chrono::steady_clock::now())) {
                Fail(FLOWMESH_API_OUTCOME_BUDGET_ERROR);
            }
            const auto parts{m_service.ClientOutcomeParts(id, sequence, hash, false)};
            if (!parts.found) {
                Fail(std::string{"FlowMesh outcome evidence is unavailable: "} +
                     (parts.sequence_retained ? "HASH_MISMATCH" : "EXPIRED"));
            }
            if (!parts.outcome) Fail("FlowMesh outcome evidence is unavailable: EXPIRED");
            const auto hex{EvidenceHex(*parts.outcome)};
            if (!hex) Fail("FlowMesh outcome evidence is unavailable: TOO_LARGE");
            UniValue out{UniValue::VOBJ};
            out.pushKV("outcome_evidence", *hex);
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
        if (method == "candles" && m_candles_max) {
            // Memory only, never the disk: the observed-time log keeps the
            // candles up to date at each commit, so a page is only a copy.
            Keys(params, {"market_id", "interval", "before_ms", "limit"});
            const auto id{Id(params, "market_id")};
            const auto interval{flowmesh::ParseCandleInterval(Text(params, "interval", 8))};
            if (!interval) Fail("Invalid candle interval");
            const std::optional<int64_t> before{params.exists("before_ms")
                ? std::optional{static_cast<int64_t>(Number(params, "before_ms", std::numeric_limits<int64_t>::max()))}
                : std::nullopt};
            // Any unsigned integer is clamped to the advertised maximum.
            const uint64_t limit{params.exists("limit") ? Number(params, "limit", std::numeric_limits<uint64_t>::max()) : m_candles_max};
            if (limit == 0) Fail("Invalid candle limit");
            {
                std::lock_guard lock{m_candles_mutex};
                if (!ChargeCandles(http.remote_address)) throw CandlesBudgetExhausted{};
            }
            if (!m_admission.ChargeExtra(http.remote_address, FlowMeshApiRequestClass::READ, std::chrono::steady_clock::now())) {
                throw CandlesBudgetExhausted{};
            }
            std::string error;
            auto page{m_local.Candles(id, *interval, before, static_cast<size_t>(std::min<uint64_t>(limit, m_candles_max)), error)};
            if (!page) Fail(error.empty() ? "No observed-time candles are available" : error);
            page->source = "endpoint_operator";
            return FlowMeshObservedCandlePageJson(*page);
        }
        if (method == "health" && m_health) return HealthReply(params, http);
        Fail("This method is not exposed by the public trading API");
    }
    FlowMeshHttpsServer::Response HandleReservedHealth(const FlowMeshHttpsServer::Request& request)
    {
        const auto busy = [] { return FlowMeshHttpsServer::Response{
            503, std::string{FLOWMESH_HTTPS_RESERVED_BUSY_BODY}, true}; };
        if (!m_health) return busy();
        try {
            // Same API instance/budget as ordinary Handle; one pre-parse
            // charge, including malformed/nonhealth requests refused here.
            if (m_admission.Admit(request.remote_address, ClassifyFlowMeshApiRequest(request.body),
                                  std::chrono::steady_clock::now()) != FlowMeshApiAdmission::Result::ADMITTED)
                return {429, std::string{FLOWMESH_API_BUDGET_EXHAUSTED_BODY}, true};
            UniValue parsed;
            if (!parsed.read(request.body)) return busy();
            Keys(parsed, {"method", "params"});
            if (Text(parsed, "method", 32) != "health") return busy();
            // Decoded values, not a prefix: reordered keys are fine, unknown
            // or duplicate fields and nonempty/null/array params are not.
            Keys(parsed["params"], {});
            UniValue out{UniValue::VOBJ};
            out.pushKV("ok", true);
            out.pushKV("result", HealthReply(parsed["params"], request));
            auto body{out.write()};
            if (body.size() > CLIENT_MAX_REPLY) return busy();
            return {200, std::move(body)};
        } catch (const HealthBudgetExhausted& e) {
            UniValue out{UniValue::VOBJ}; out.pushKV("ok", false); out.pushKV("error", e.what());
            return {429, out.write(), true};
        } catch (...) {
            // No general Call/Handle, action authentication/admission or wait
            // is ever entered from the reserve, including parse failures.
            return busy();
        }
    }
    FlowMeshHttpsServer::Response Handle(const FlowMeshHttpsServer::Request& request)
    {
        try {
            // Charged before decoding/authentication and before the operator's
            // local-action queue: bounded identities, per-class and total rates.
            if (m_admission.Admit(request.remote_address, ClassifyFlowMeshApiRequest(request.body),
                                  std::chrono::steady_clock::now()) != FlowMeshApiAdmission::Result::ADMITTED) {
                return {429, std::string{FLOWMESH_API_BUDGET_EXHAUSTED_BODY}};
            }
            UniValue parsed;
            if (!parsed.read(request.body)) Fail("Malformed public trading request");
            UniValue out{UniValue::VOBJ};
            out.pushKV("ok", true); out.pushKV("result", Call(parsed, request));
            auto body{out.write()};
            if (body.size() > CLIENT_MAX_REPLY) Fail("Public trading response exceeds snapshot bound");
            // A watched page is small by construction; never send more.
            if (body.size() > FLOWMESH_API_WATCH_MAX_REPLY && parsed["method"].isStr() &&
                parsed["method"].get_str() == "updates" && parsed["params"].exists("watch_ms")) {
                Fail("Public trading watch response exceeds its bound");
            }
            return {200, std::move(body)};
        } catch (const HealthBudgetExhausted& e) {
            UniValue out{UniValue::VOBJ}; out.pushKV("ok", false); out.pushKV("error", e.what());
            return {429, out.write()};
        } catch (const CandlesBudgetExhausted& e) {
            UniValue out{UniValue::VOBJ}; out.pushKV("ok", false); out.pushKV("error", e.what());
            return {429, out.write()};
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
    // A waited read whose own connection is cold may carry this verified one
    // on the wait lane (swapped under m_work with the lane held) and hands it
    // back under m_work; meanwhile m_https holds the lane's previous session
    // and opens its own connection if ordinary work needs one.
    FlowMeshHttpsClient m_https;
    // Ordinary reads retain one owner through body application, not just
    // socket completion. Only direct authenticated FOREGROUND Market/Data
    // may wait: eight stack waiters, original-entry 5s QUEUE-only deadline.
    // Passive, null-account and Watch paths remain immediate; no body queue.
    FlowMeshClientReadAdmission m_read_admission;
    std::mutex m_read_lane;
    FlowMeshHttpsClient m_read_https;
    std::atomic<bool> m_read_reset{false};
    // Under m_work. Explicit trust/configuration reset fences late replies;
    // endpoint observation revisions fence stale cooldown/capability writes.
    uint64_t m_read_epoch{0}, m_endpoint_revision{0}, m_cache_revision{0};
    bool m_read_context_exhausted{false};
    uint64_t AdvanceRevision(uint64_t& revision)
    {
        if (revision == std::numeric_limits<uint64_t>::max()) {
            m_read_context_exhausted = true;
            Fail("Client observation revision exhausted; signing history is preserved");
        }
        return ++revision;
    }
    void EndpointChanged() { AdvanceRevision(m_endpoint_revision); }
    void ResetReadTrust()
    {
        AdvanceRevision(m_read_epoch);
        m_read_reset = true;
        // Explicit connect/reconnect invalidates joins under the old trust
        // context. At most eight volatile caches; no head/history reset.
        for (auto& [id, cache] : m_cache) {
            if (!cache.fresh) continue;
            cache.fresh.reset();
            cache.revision = AdvanceRevision(m_cache_revision);
        }
    }
    struct ReadRequest {
        FlowMeshReadAdmission policy;
        std::chrono::steady_clock::time_point started;
    };
    static ReadRequest DirectReadRequest(const std::optional<uint256>& account,
                                         const std::chrono::steady_clock::time_point started)
    {
        return {account && FlowMeshClientWorkScope::Current() == FlowMeshClientWorkPriority::FOREGROUND
                    ? FlowMeshReadAdmission::FOREGROUND_ACCOUNT : FlowMeshReadAdmission::IMMEDIATE,
                started};
    }
    // Waited status reads use their own TLS connection so that m_work is not
    // held across a server-side wait. m_wait_lane is only ever try-locked, and
    // never while holding m_work; a busy lane means an ordinary status read.
    std::mutex m_wait_lane;
    FlowMeshHttpsClient m_wait_https;
    // Set where m_https is reset, under m_work; the lane consumes it under
    // m_work and revalidates trust before its next use.
    std::atomic<bool> m_wait_reset{false};
    // Per endpoint (index-stable, like m_retry_after), under m_work: the bounded
    // 'action' wait each endpoint last advertised. Zero never sends wait_ms.
    std::vector<std::chrono::milliseconds> m_action_wait;
    // Per endpoint (index-stable), under m_work: the same wait on 'submit'
    // each endpoint last advertised. Zero never sends wait_ms (or "outcome")
    // with a submission.
    std::vector<std::chrono::milliseconds> m_submit_wait;
    // Per endpoint (index-stable), under m_work: it advertised verifiable
    // outcomes ("action_outcome_v1": true). False never sends "outcome".
    std::vector<bool> m_action_outcome;
    // Per endpoint, under m_work: a lane request to it got no HTTP response.
    // The lane skips it until EndpointResult records a successful response
    // from it (an ordinary read or probe), so a silent endpoint costs a
    // waited call its lane time once, not on every call.
    std::vector<bool> m_wait_unanswered;
    // Live-update lane: its own TLS connection, so m_work is never held across
    // a server-side watch. Only try-locked, and never while holding m_work
    // (lane, then m_work, as for m_wait_lane); a busy lane means an ordinary
    // read.
    std::mutex m_watch_lane;
    FlowMeshHttpsClient m_watch_https;
    // Set where m_https is reset or a watched reply failed verification: the
    // lane drops its connection before its next request.
    std::atomic<bool> m_watch_reset{false};
    // Per endpoint (index-stable), under m_work: the 'updates' watch each
    // endpoint last advertised. Zero never sends watch_ms.
    std::vector<std::chrono::milliseconds> m_updates_watch;
    // Per endpoint, under m_work: a watch to it got no HTTP response. Cleared
    // like m_wait_unanswered, by a successful response from that endpoint.
    std::vector<bool> m_watch_unanswered;
    // Under m_work: smoothed time a quiet watch spent outside the endpoint's
    // wait (transport and both ends' work), which the next wait leaves room for.
    std::chrono::milliseconds m_watch_overhead{0};
    // Per endpoint (index-stable), under m_work: the largest clearing-tick page
    // each endpoint last advertised. Zero never sends "ticks".
    std::vector<size_t> m_clearing_ticks;
    // Per endpoint, under m_work: its advertisement is ignored until then,
    // after it refused "ticks" as an unknown field.
    std::vector<std::chrono::steady_clock::time_point> m_clearing_ticks_withdrawn;
    // Per endpoint (index-stable), under m_work: it advertised
    // observed_time_v1 (withdrawn with the ticks), and the largest
    // 'candles' page it advertised (zero: never called).
    std::vector<bool> m_observed_time;
    std::vector<size_t> m_observed_candles;
    // Display-only candles lane: its own TLS connection, so m_work is never
    // held across a candles request. Only try-locked, never while holding
    // m_work; a busy lane means the caller tries again later.
    std::mutex m_display_lane;
    FlowMeshHttpsClient m_display_https;
    std::atomic<bool> m_display_reset{false};
    // Under m_display_lane only, never under m_work alone: per (market,
    // interval), the newest candle of the latest page read from that
    // endpoint, the only bucket that can still grow. It stands in for a later
    // page's first held-back bucket once a verified head reaches it. At most
    // CLIENT_MAX_HELD_CANDLES; cleared with the lane's connection.
    struct HeldCandle {
        size_t endpoint;
        flowmesh::ObservedCandle candle;
    };
    std::map<std::pair<uint256, flowmesh::CandleInterval>, HeldCandle> m_held_candles;
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
    //! Withdraws every verified label together: an unverified inclusion
    //! never keeps an outcome, nor a reason code this client derived from
    //! verifying it (a remote receipt has no other reason code).
    static void WithdrawVerifiedLabels(Receipt& receipt)
    {
        receipt.certificate_verified = false;
        receipt.state = "unknown";
        receipt.outcome.reset();
        receipt.outcome_verified = false;
        receipt.reason_code.clear();
    }
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
        // Volatile: endpoints already asked for this inclusion's outcome
        // evidence by the outcome-only upgrade (at most once each).
        std::set<size_t> outcome_upgrade_asked{};
        // Durable: a DIFFERENT certified action used this action's account
        // sequence, proven once under local B3 authority, so this action can
        // never execute. Like previously_certified it forbids resending the
        // instruction after a restart; fresh proof renews only the label.
        std::optional<flowmesh::ClientConsumerRef> not_executed{};
        // Volatile, re-fetchable: the consumer's certified payload, re-verified
        // by RecheckActionAuthority and evictable like an inclusion proof.
        std::vector<unsigned char> consumer_proof{};
        // Volatile: the marker's one re-proof per process was attempted.
        bool not_executed_reproof_asked{false};
        // Volatile, under m_work: this new action's waited submission is on
        // the wait lane without m_work. Never evicted or resent meanwhile,
        // and absent from the saved view until that Submit returns, so the
        // view still shows only completed methods.
        bool lane_in_flight{false};
    };
    std::map<std::pair<uint256, uint256>, Pending> m_pending;
    static bool ProvenNotExecuted(const Pending& p) { return FlowMeshProvenNotExecuted(p.receipt, p.not_executed); }
    //! Endpoint admission tokens an opt-in 'action' reply cost beyond the
    //! request itself: TradingApi charges one per evidence part it adds (or
    //! tried to add), so the automatic status budget is charged for them too.
    static size_t OutcomeExtraCost(const UniValue& value)
    {
        if (!value.isObject()) return 0;
        return size_t{value.exists("outcome_evidence") || value.exists("outcome_evidence_error")} +
               size_t{value.exists("consumer_certified_payload") || value.exists("consumer_evidence_error")};
    }
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
        // False only between an applied page that asked for a snapshot and
        // that snapshot, which rebuilds the cache: no watch meanwhile.
        bool watch_ready{true};
        // The verified state's standing curves, copied at most once for this
        // cache (and so for its head) when a display view asks; see CachedBook.
        std::shared_ptr<const flowmesh::BookCurves> book{};
        // Global monotone identity: eviction/reinsert cannot create an ABA.
        uint64_t revision{0};
    };
    std::map<uint256, Cache> m_cache;
    //! Endpoint-reported clearing ticks of a cached market, never evidence.
    //! Kept apart from Cache, which each new head replaces, so a read asks
    //! only for the ticks after the last one held. Under m_work; an entry
    //! exists only for a cached market and is erased with it.
    struct TickRing {
        //! Consecutive, oldest first, none beyond the verified head; see
        //! FlowMeshMergeClearingTicks.
        std::deque<flowmesh::ClearingTick> ticks;
        //! Why the ring does not reach the verified head, if known.
        std::string reason;
        //! The head a live-update read last gave up its watch for, to let an
        //! ordinary read fetch the missing ticks: at most once per head.
        uint256 catchup_head;
        //! Endpoint-reported observed times of held ticks (never evidence),
        //! from one endpoint only (times_source); see
        //! FlowMeshMergeObservedTimes.
        std::map<uint64_t, flowmesh::ObservedStamp> times;
        std::string times_source;
        std::string times_reason;
        bool times_clock_suspect{false};
    };
    std::map<uint256, TickRing> m_ticks;
    //! One market-data read's clearing-tick request. Only Data() and Watch()
    //! make one: a signing preflight never sends "ticks".
    struct TickRequest {
        std::optional<uint64_t> after;
        //! The verified head's sequence when the request was made, if cached.
        std::optional<uint64_t> head;
        //! Endpoint (and page limit) of the attempt that carries "ticks".
        std::optional<size_t> sent;
        size_t limit{0};
        //! The page of the reply that last set or confirmed the verified head.
        std::optional<UniValue> page;
        //! The caller asked for observed times; sent with "ticks" only to an
        //! endpoint that advertises them.
        bool observed{false};
        bool observed_sent{false};
        //! That reply's observed-time section, and when it was received.
        std::optional<UniValue> observed_page;
        int64_t received_ms{0};
    };
    //! Under m_work: the request for a market's missing ticks.
    TickRequest TicksAfterRing(const uint256& id) const
    {
        TickRequest out;
        if (const auto it{m_ticks.find(id)}; it != m_ticks.end() && !it->second.ticks.empty()) {
            out.after = it->second.ticks.back().sequence;
        }
        if (const auto it{m_cache.find(id)}; it != m_cache.end()) out.head = it->second.verified.certified.entry.sequence;
        return out;
    }
    //! Adds "ticks" for an endpoint that advertises them.
    std::function<void(UniValue&, size_t)> TickDecorator(TickRequest* ticks)
    {
        if (!ticks) return {};
        return [this, ticks](UniValue& params, const size_t endpoint) {
            ticks->sent.reset();
            ticks->observed_sent = false;
            const size_t advertised{m_clearing_ticks.at(endpoint)};
            if (advertised == 0) return;
            // Held ticks more than a page behind the verified head could only
            // creep forward one page per read: the newest page replaces them.
            if (ticks->after && ticks->head && *ticks->head > *ticks->after && *ticks->head - *ticks->after > advertised) {
                ticks->after.reset();
            }
            UniValue value{UniValue::VOBJ};
            if (ticks->after) value.pushKV("after_sequence", *ticks->after);
            value.pushKV("limit", uint64_t{advertised});
            params.pushKV("ticks", std::move(value));
            ticks->sent = endpoint;
            ticks->limit = advertised;
            ticks->observed_sent = ticks->observed && m_observed_time.at(endpoint);
            if (ticks->observed_sent) params.pushKV("observed_time", true);
        };
    }
    //! Called from a reply's validation: keeps the page of an attempt that
    //! carried "ticks". Checked only later, by MergeTicks, never here.
    static void KeepTickPage(TickRequest* ticks, const UniValue& value, const size_t endpoint)
    {
        if (!ticks || ticks->sent != endpoint) return;
        ticks->page = value["reported_data"]["clearing_ticks"];
        if (ticks->observed_sent) {
            ticks->observed_page = value["reported_data"]["observed_time"];
            ticks->received_ms = TicksSinceEpoch<std::chrono::milliseconds>(SystemClock::now());
        } else {
            ticks->observed_page.reset();
        }
    }
    //! After the read's Refresh: merges its page against the verified head.
    //! A bad page costs only the ticks, never the read.
    void MergeTicks(const uint256& id, const Cache& cache, const TickRequest& ticks)
    {
        if (!m_cache.contains(id)) return;
        auto& ring{m_ticks[id]};
        if (!ticks.page) {
            if (m_clearing_ticks.at(cache.endpoint) == 0) ring.reason = "endpoint_does_not_advertise_clearing_ticks";
            return;
        }
        const auto& head{cache.verified.certified.entry};
        ring.reason = FlowMeshMergeClearingTicks(ring.ticks, *ticks.page, ticks.after, ticks.limit, head.sequence, head.GetHash());
        if (!ring.reason.empty()) LogDebug(BCLog::NET, "FlowMesh clearing ticks for market=%s: %s\n", id.GetHex(), ring.reason);
        // Observed times: one endpoint's series only, bound to held ticks.
        const auto& source{m_endpoints.at(cache.endpoint).url};
        if (ring.times_source != source) {
            ring.times.clear();
            ring.times_source.clear();
        }
        if (ticks.observed_page) {
            ring.times_reason = FlowMeshMergeObservedTimes(ring.times, *ticks.observed_page, ring.ticks, ticks.received_ms,
                                                           ring.times_clock_suspect);
            if (ring.times_reason.empty() || !ring.times.empty()) ring.times_source = source;
        } else {
            std::erase_if(ring.times, [&](const auto& item) {
                return ring.ticks.empty() || item.first < ring.ticks.front().sequence || item.first > ring.ticks.back().sequence;
            });
            if (ticks.observed && !m_observed_time.at(cache.endpoint)) ring.times_reason = "endpoint_does_not_advertise_observed_time";
        }
    }
    //! The caller's page of held ticks: available only when they reach the
    //! verified head (whose hash the tick there carries).
    flowmesh::ClearingTickPage TicksView(const Cache& cache, const flowmesh::ClearingTickQuery& query) const
    {
        flowmesh::ClearingTickPage out;
        const auto it{m_ticks.find(cache.pins.market_id)};
        if (it == m_ticks.end() || it->second.ticks.empty() ||
            it->second.ticks.back().sequence != cache.verified.certified.entry.sequence) {
            out.unavailable_reason = it != m_ticks.end() && !it->second.reason.empty() ? it->second.reason
                : m_clearing_ticks.at(cache.endpoint) == 0 ? "endpoint_does_not_advertise_clearing_ticks"
                                                           : "clearing_ticks_not_yet_received";
            return out;
        }
        return flowmesh::PageClearingTicks(it->second.ticks, query);
    }
    //! The endpoint's observed times for the reply's head, history and
    //! ticks, labelled with that endpoint. Endpoint-reported display data:
    //! flagged, never corrected; noexcept, so it can never fail a read.
    flowmesh::ObservedTimes ObservedView(const Cache& cache, const flowmesh::MarketData& out,
                                         const std::optional<int64_t> head_floor_ms) const noexcept
    {
        flowmesh::ObservedTimes view;
        try {
            view.source = m_endpoints.at(cache.endpoint).url;
            const auto it{m_ticks.find(cache.pins.market_id)};
            if (it == m_ticks.end() || it->second.times_source != view.source) {
                view.unavailable_reason = !m_observed_time.at(cache.endpoint) ? "endpoint_does_not_advertise_observed_time"
                    : it != m_ticks.end() && !it->second.times_reason.empty() ? it->second.times_reason
                                                                                : "observed_time_not_yet_received";
                return view;
            }
            const auto& ring{it->second};
            view.available = true;
            view.clock_suspect = ring.times_clock_suspect;
            std::map<uint64_t, uint256> wanted;
            const auto& head{cache.verified.certified.entry};
            wanted.emplace(head.sequence, head.GetHash());
            for (const auto& entry : out.history.entries) wanted.emplace(entry.sequence, entry.microblock_hash);
            if (out.ticks) for (const auto& tick : out.ticks->ticks) wanted.emplace(tick.sequence, tick.microblock_hash);
            for (const auto& [sequence, hash] : wanted) {
                const auto stamp{ring.times.find(sequence)};
                if (stamp == ring.times.end() || stamp->second.hash != hash) continue;
                auto row{stamp->second};
                if (sequence == head.sequence && head_floor_ms && row.mode == flowmesh::ObservedMode::LIVE && row.ms < *head_floor_ms) {
                    row.flags |= flowmesh::OBSERVED_IMPLAUSIBLE;
                }
                view.rows.push_back(row);
            }
        } catch (...) {
            view = {};
            view.unavailable_reason = "malformed";
        }
        return view;
    }
    //! Under m_work: the cache's verified curves for display views, which are
    //! computed from this copy only after m_work is released. The copy is
    //! made at most once per cache (so per verified head), from the state
    //! verified against the certified root, never from reported data; its
    //! time under m_work is recorded as book_copy_us.
    static std::shared_ptr<const flowmesh::BookCurves> CachedBook(Cache& cache, FlowMeshTimingSpan& timing)
    {
        if (cache.book) {
            timing.Field("book_copy", std::string{"reused"});
            return cache.book;
        }
        const auto started{std::chrono::steady_clock::now()};
        const auto head{cache.verified.certified.entry.GetHash()};
        // A book over the views' bound is never copied (under m_work).
        if (cache.verified.state.ReadCurves(0).total_curves > flowmesh::MARKET_VIEW_MAX_BOOK_CURVES) {
            cache.book = std::make_shared<const flowmesh::BookCurves>(flowmesh::BookCurves{.head = head, .curves = {}, .unavailable_reason = "work_budget"});
        } else {
            cache.book = std::make_shared<const flowmesh::BookCurves>(flowmesh::BookCurves{
                .head = head, .curves = cache.verified.state.ReadCurves(flowmesh::MARKET_VIEW_MAX_BOOK_CURVES).curves});
        }
        timing.Field("book_copy", cache.book->unavailable_reason.empty() ? std::string{"copied"} : cache.book->unavailable_reason);
        timing.Field("book_copy_curves", uint64_t(cache.book->curves.size()));
        timing.Field("book_copy_us", uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count()));
        return cache.book;
    }
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
            if (!p.lane_in_flight && p.owner_account == account && (!market || p.market == *market)) out.push_back(SavedRow(p));
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
            for (const auto& [key, p] : m_pending) if (!p.lane_in_flight) rows->push_back(SavedRow(p));
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
        EndpointChanged();
        m_action_wait.at(endpoint) = FlowMeshAdvertisedActionWait(status);
        m_submit_wait.at(endpoint) = FlowMeshAdvertisedSubmitWait(status);
        m_action_outcome.at(endpoint) = FlowMeshAdvertisedActionOutcome(status);
        LearnUpdatesWatch(FlowMeshAdvertisedUpdatesWatch(status), endpoint);
        const bool withdrawn{std::chrono::steady_clock::now() < m_clearing_ticks_withdrawn.at(endpoint)};
        m_clearing_ticks.at(endpoint) = withdrawn ? 0 : FlowMeshAdvertisedClearingTicks(status);
        m_observed_time.at(endpoint) = !withdrawn && FlowMeshAdvertisedObservedTime(status);
        m_observed_candles.at(endpoint) = FlowMeshAdvertisedObservedCandles(status);
    }
    void LearnUpdatesWatch(const std::chrono::milliseconds advertised, size_t endpoint)
    {
        EndpointChanged();
        m_updates_watch.at(endpoint) = advertised;
        std::lock_guard lock{m_status_mutex};
        m_status.endpoints.at(endpoint).live_updates_ms = static_cast<uint32_t>(advertised.count());
    }
    // Display only, never evidence and never an input to endpoint selection:
    // what the endpoint last advertised about its own readiness.
    void LearnReadiness(FlowMeshAdvertisedReadiness readiness, size_t endpoint, const uint256& market)
    {
        EndpointChanged();
        std::lock_guard lock{m_status_mutex};
        auto& row{m_status.endpoints.at(endpoint)};
        row.readiness = FlowMeshAdvertisedReadinessName(readiness.state);
        if (readiness.state == FlowMeshAdvertisedReadiness::State::UNKNOWN) {
            row.readiness_market.clear();
            row.not_ready.clear();
            row.readiness_observed_ms = 0;
            return;
        }
        row.readiness_market = market.GetHex();
        row.not_ready = std::move(readiness.not_ready);
        row.readiness_observed_ms = TicksSinceEpoch<std::chrono::milliseconds>(SystemClock::now());
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
        EndpointChanged();
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
            m_watch_unanswered.at(endpoint) = false;
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
    //! `undecorated_retry`, for reads only: when a decorated request is refused
    //! as carrying an unknown field and it returns true, the same endpoint is
    //! read once more without the decoration (never a failover).
    UniValue Call(const std::string& method, const UniValue& params,
                  const std::function<void(const UniValue&, size_t)>& validate,
                  bool* possibly_sent = nullptr, bool* earlier_possible = nullptr,
                  size_t* attempted_endpoints = nullptr, Pending* automatic_action = nullptr,
                  const std::function<void(UniValue&, size_t)>& decorate = {},
                  const std::optional<size_t> skip = std::nullopt,
                  const std::function<bool(size_t)>& undecorated_retry = {})
    {
        Assume(!undecorated_retry || (decorate && !possibly_sent && !earlier_possible && !attempted_endpoints && !automatic_action));
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
            if (skip && endpoint == *skip) continue;
            if (automatic_read && std::chrono::steady_clock::now() < m_retry_after[endpoint]) continue;
            if (automatic_action && !m_action_polls.TryChargeAttempt(std::chrono::steady_clock::now())) {
                automatic_action->automatic_endpoint = endpoint;
                Fail("Automatic status refresh coalesced; retained observation is not a fresh endpoint response");
            }
            if (automatic_action) automatic_action->automatic_endpoint = (endpoint + 1) % m_endpoints.size();
            bool transport_available{false};
            bool decorating{static_cast<bool>(decorate)};
            // At most two passes: the second only as undecorated_retry allows.
            for (;;) {
            try {
                if (attempted_endpoints) ++*attempted_endpoints;
                if (earlier_possible && possibly_sent) *earlier_possible = *possibly_sent;
                // An optional per-endpoint parameter (a capability only that
                // endpoint advertised) rebuilds the body; "method" stays first.
                std::string decorated;
                if (decorating) {
                    UniValue endpoint_params{params};
                    decorate(endpoint_params, endpoint);
                    UniValue endpoint_request{UniValue::VOBJ};
                    endpoint_request.pushKV("method", method);
                    endpoint_request.pushKV("params", std::move(endpoint_params));
                    decorated = endpoint_request.write();
                }
                const auto reply{m_https.Request(m_endpoints[endpoint], "/flowmesh/v1", decorating ? decorated : body,
                                                 CLIENT_REQUEST_TIMEOUT, CLIENT_MAX_REPLY)};
                if (possibly_sent && reply.request_may_have_been_sent) *possibly_sent = true;
                if (!reply.response_received) Fail(reply.error.empty() ? "No HTTPS response; outcome unknown" : reply.error);
                transport_available = true;
                UniValue parsed;
                if (!parsed.read(reply.body)) Fail("Malformed endpoint JSON response");
                Keys(parsed, {"ok", "result", "error"});
                if (!Flag(parsed, "ok")) {
                    const auto text{Text(parsed, "error")};
                    // A decorated request to an endpoint that no longer takes
                    // the field: both advertised capabilities are relearned
                    // from its next status object.
                    if (decorating && text == FLOWMESH_CLIENT_UNKNOWN_FIELD_ERROR) {
                        if (undecorated_retry) {
                            if (undecorated_retry(endpoint)) {
                                decorating = false;
                                continue;
                            }
                        } else {
                            EndpointChanged();
                            m_action_wait.at(endpoint) = {};
                            m_action_outcome.at(endpoint) = false;
                        }
                    }
                    Fail(text);
                }
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
            break;
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
            hasher << p.not_executed.has_value();
            if (p.not_executed) {
                hasher << p.not_executed->action_id << p.not_executed->microblock_sequence << p.not_executed->microblock_hash;
            }
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
    // A different read may have verified inclusion in memory before its
    // no-resubmit marker could be saved. No early return may expose that
    // verification until restart-relevant state is durable. Pure outcome
    // text changes no digest and causes no additional synchronous write.
    void SaveVerifiedRestartState(Pending& p)
    {
        if (!p.receipt.certificate_verified && !ProvenNotExecuted(p)) return;
        try {
            SaveRestartState();
        } catch (...) {
            WithdrawVerifiedLabels(p.receipt);
            // Exact bytes and previously_certified stay binding in memory.
            throw;
        }
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
            if (p.not_executed) {
                // Additive: older binaries restore rows by named keys and
                // ignore it (they may then resend an action that can never
                // execute, which is harmless).
                UniValue consumer{UniValue::VOBJ};
                consumer.pushKV("action_id", p.not_executed->action_id.GetHex());
                consumer.pushKV("microblock_sequence", p.not_executed->microblock_sequence);
                consumer.pushKV("microblock_hash", p.not_executed->microblock_hash.GetHex());
                row.pushKV("not_executed_consumer", std::move(consumer));
            }
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
#ifdef FLOWMESH_CLIENT_CRASH_TEST_HOOKS
        test::ClientCrashBeforeJournalSave(m_path, root);
#endif
        m_db->Write(std::string{"public-client-v1"}, blob, true); // throws on failed synchronous write
#ifdef FLOWMESH_CLIENT_CRASH_TEST_HOOKS
        test::ClientCrashAfterJournalSyncBeforeDigest(m_path, root);
#endif
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
            if (row.exists("not_executed_consumer")) {
                const auto& consumer{row["not_executed_consumer"]};
                p.not_executed = flowmesh::ClientConsumerRef{Id(consumer, "action_id"), Number(consumer, "microblock_sequence"),
                                                             Id(consumer, "microblock_hash")};
                if (action->IsDeposit() || p.not_executed->action_id == action->Id()) Fail("Invalid retained not-executed marker");
                p.receipt.reason = CLIENT_NOT_EXECUTED_RETAINED;
            }
            if (!m_pending.emplace(std::pair{p.market, action->Id()}, std::move(p)).second) Fail("Duplicate retained action");
        }
#ifdef FLOWMESH_CLIENT_CRASH_TEST_HOOKS
        test::ClientCrashRecord(m_path, "journal_restore_completed", root);
#endif
        std::lock_guard lock{m_status_mutex}; m_status.pending_actions = m_pending.size();
    }
    //! Keeps one re-fetchable proof copy of p (its inclusion or consumer
    //! proof, `slot`) within the 8 MiB budget of all retained proof copies.
    void RetainProof(Pending& p, std::vector<unsigned char>& slot, const std::vector<unsigned char>& proof)
    {
        size_t retained_bytes{proof.size()};
        for (const auto& [key, item] : m_pending) retained_bytes += item.inclusion_proof.size() + item.consumer_proof.size();
        retained_bytes -= slot.size();
        if (retained_bytes > 8 * 1024 * 1024) {
            // Only evict re-fetchable proof copies. The durable no-replay
            // markers and the exact signed instructions remain intact.
            for (auto& [key, item] : m_pending) if (&item != &p) {
                item.inclusion_proof.clear(); item.inclusion_proof.shrink_to_fit();
                item.consumer_proof.clear(); item.consumer_proof.shrink_to_fit();
            }
        }
        slot = proof;
    }
    void Inclusion(Pending& p, const flowmesh::ProductionCertifiedEnvelope& certified, size_t endpoint,
                   const std::vector<unsigned char>& proof)
    {
        const auto& entry{certified.entry};
        if (std::none_of(entry.actions.begin(), entry.actions.end(), [&](const auto& action) { return action.Id() == p.action.Id(); }))
            Fail("Certified entry does not include the requested semantic action");
        // An outcome this client already verified for this exact inclusion is
        // kept: verifying the same certified entry again (a later snapshot of
        // that head, or another reply's proof) never discards it. The outcome
        // is a pure function of that entry, and a withdrawn inclusion label
        // has already withdrawn it (WithdrawVerifiedLabels).
        std::optional<interfaces::FlowMeshActionOutcome> verified_outcome;
        if (p.receipt.certificate_verified && p.receipt.outcome_verified && p.receipt.outcome &&
            p.receipt.state == "certified_inclusion" && p.receipt.microblock_hash == entry.GetHash()) {
            verified_outcome = std::move(p.receipt.outcome);
        }
        p.receipt = {}; p.receipt.action_id = p.action.Id(); p.receipt.state = "certified_inclusion";
        p.receipt.microblock_hash = entry.GetHash(); p.receipt.microblock_sequence = entry.sequence;
        p.receipt.certificate_verified = true; p.receipt.endpoint = m_endpoints[endpoint].url;
        p.previously_certified = true;
        m_action_polls.Remove({p.market, p.action.Id()});
        RetainProof(p, p.inclusion_proof, proof);
        if (!p.owner_account.IsNull()) p.receipt.account_id = p.owner_account;
        p.receipt.reason = "Canonical action inclusion verified; execution outcome is not proved by inclusion alone";
        if (verified_outcome) {
            p.receipt.reason_code = verified_outcome->reason_code;
            p.receipt.outcome = std::move(verified_outcome);
            p.receipt.outcome_verified = true;
        }
        m_own_certified_through.Record(p.market, entry.sequence);
        // A cached state older than this inclusion must not feed a join.
        if (const auto cache{m_cache.find(p.market)}; cache != m_cache.end() &&
            entry.sequence > cache->second.verified.certified.entry.sequence) {
            cache->second.fresh.reset();
            cache->second.revision = AdvanceRevision(m_cache_revision);
        }
    }
    //! `page` is the cursor of the 'updates' page that asked for this
    //! snapshot, if any: events from there on are still delivered, so none
    //! is skipped unless the snapshot comes from another event log.
    void InstallSnapshot(const uint256& id, const std::optional<uint256>& account,
                         const flowmesh::ClientEvidencePins& pins, const UniValue& value, const size_t endpoint,
                         const std::optional<flowmesh::ClientEventCursor>& page, TickRequest* ticks,
                         const bool forced_gap, const uint64_t prior_gaps, const std::map<size_t, uint64_t>& withdrawn,
                         const bool learn_endpoint)
    {
        // Only this snapshot's page belongs to the head it sets.
        if (ticks) ticks->page.reset();
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
            const auto revision{AdvanceRevision(m_cache_revision)};
            const auto head_hash{entry.GetHash()};
            // The snapshot's cursor is its endpoint's log position when the
            // state was captured, at or after the page that asked for it.
            // Resume from the page within the same log, so the events in
            // between are still delivered as hints and none is skipped;
            // another log (another endpoint, or a restart) skipped them.
            flowmesh::ClientEventCursor cursor{evidence.cursor};
            bool skipped{false};
            if (page && page->instance_id == evidence.cursor.instance_id) {
                cursor.event_id = std::min(page->event_id, evidence.cursor.event_id);
            } else if (page) {
                skipped = true;
            }
            // Heads/actions are durable, event cursors are not. Restart or
            // bounded cache eviction requires an explicit snapshot gap even
            // when the certified head itself has not changed.
            const bool gap{forced_gap || lost_cursor || skipped || (m_cache.contains(id) && m_cache.at(id).event_gap)};
            std::vector<Pending*> included;
            included.reserve(m_pending.size());
            for (auto& [key, pending] : m_pending) {
                if (pending.market == id &&
                    std::any_of(entry.actions.begin(), entry.actions.end(), [&](const auto& action) { return action.Id() == pending.action.Id(); }))
                    included.push_back(&pending);
            }
            // Prepare a newly observed market's map node privately, before
            // binding any live obligation. Its later insertion cannot allocate.
            decltype(m_highwater) prepared_head;
            if (previous == m_highwater.end()) prepared_head.emplace(id, std::pair{entry.sequence, head_hash});
            auto head_node{prepared_head.extract(id)};
            try {
                // Bind EVERY matched original before receipt/proof work can
                // throw. Record is noexcept: allocation failure marks its
                // volatile floor incomplete, which disables all joins.
                for (Pending* pending : included) pending->previously_certified = true;
                if (!included.empty()) m_own_certified_through.Record(id, entry.sequence);
                if (head_node) {
                    m_highwater.insert(std::move(head_node));
                } else {
                    previous->second = {entry.sequence, head_hash};
                }
                // Candidate construction/copies may throw too: binding flags,
                // floor and head already cover every match before that work.
                Cache candidate{pins, std::move(*verified), cursor, value["status"], value["reported_data"], endpoint, gap, account};
                candidate.revision = revision;
                for (Pending* pending : included)
                    Inclusion(*pending, candidate.verified.certified, endpoint, evidence.certified_payload);
                // One aggregate blob contains both the head and all restart
                // no-resubmit markers. There is no head-only durable prefix.
                // Cache, labels and endpoint metadata remain private until
                // this existing synchronous barrier successfully returns.
                SaveRestartState();
                if (m_cache.size() >= CLIENT_MAX_CACHED_MARKETS && !m_cache.contains(id)) {
                    m_ticks.erase(m_cache.begin()->first);
                    m_cache.erase(m_cache.begin());
                }
                m_cache.insert_or_assign(id, std::move(candidate));
                if (prior_gaps || lost_cursor || skipped) {
                    std::lock_guard lock{m_status_mutex};
                    m_status.event_gaps += prior_gaps + uint64_t{lost_cursor || skipped};
                }
                if (learn_endpoint) {
                    ApplyTickWithdrawals(withdrawn);
                    LearnMetadata(value["status"], pins, endpoint);
                    LearnActionWait(value["status"], endpoint);
                    LearnReadiness(FlowMeshAdvertisedReadinessOf(value["status"]), endpoint, id);
                }
                KeepTickPage(ticks, value, endpoint);
            } catch (...) {
                // Binding flags/floors/head never roll back, including those
                // of matched originals Inclusion had not yet processed.
                // An ambiguous write must not earn a skipped later save.
                m_durable_restart_digest.reset();
                for (Pending* pending : included) WithdrawVerifiedLabels(pending->receipt);
                if (const auto old{m_cache.find(id)}; old != m_cache.end()) {
                    old->second.fresh.reset();
                    old->second.revision = AdvanceRevision(m_cache_revision);
                }
                // Also fence a same-head failure with no old cache/floor
                // change: OrdinaryReadCall must not walk another endpoint
                // over these already-bound in-memory obligations.
                AdvanceRevision(m_read_epoch);
                throw;
            }
    }
    enum class CacheCheck { CURRENT, SCOPE, CHECKPOINT, AUTHORITY, ANCHOR, RETAINED };
    // Local checks that a cached verified state still applies to this account
    // under the local B3 chain. No network use.
    CacheCheck CheckCache(const uint256& id, const Cache& cache, const std::optional<uint256>& account)
    {
        // Switching wallets changes the event filter. A current verified
        // snapshot is safe, but it is not replay of the other account's
        // omitted events; surface the gap before advancing its cursor.
        if (cache.account_scope != account) return CacheCheck::SCOPE;
        if (const auto high{m_highwater.find(id)}; high != m_highwater.end() &&
            (cache.verified.certified.entry.sequence < high->second.first ||
             (cache.verified.certified.entry.sequence == high->second.first && cache.verified.certified.entry.GetHash() != high->second.second)))
            return CacheCheck::RETAINED;
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
    struct UpdatesPage {
        flowmesh::ClientEventCursor cursor;
        bool gap{false}, snapshot_needed{false};
        UniValue status;
        std::optional<UniValue> reported;
    };
    //! Pure validation against the current cache. A page that needs a
    //! snapshot never advances a cursor/capability before its durable head.
    UpdatesPage ParseUpdatesPage(const Cache& cache, const uint256& id, const UniValue& value)
    {
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
        const auto reported_sequence{Number(value["status"], "next_microblock_sequence")};
        const auto reported_head{Id(value["status"], "last_microblock_hash", true)};
        if (reported_sequence < cache.verified.certified.entry.sequence + 1) Fail("Endpoint reported a stale microblock head");
        if (reported_sequence == cache.verified.certified.entry.sequence + 1 && reported_head != cache.verified.certified.entry.GetHash())
            Fail("Endpoint reported a conflicting equal-height head");
        // The snapshot this asks for resumes from this page's cursor, so the
        // events after it are still delivered (see Snapshot).
        UpdatesPage page{cursor, gap, gap || reported_head != cache.verified.certified.entry.GetHash(), value["status"], std::nullopt};
        if (value.exists("reported_data") && !value["reported_data"]["unchanged"].isTrue()) page.reported = value["reported_data"];
        // A 'more' page is not skipped: cursor remains at the returned
        // page boundary, and the next refresh resumes there.
        (void)Flag(value, "more");
        return page;
    }
    void InstallUpdatesPage(Cache& cache, const uint256& id, UpdatesPage page, const size_t endpoint, const bool learn_endpoint)
    {
        Assume(!page.snapshot_needed);
        cache.revision = AdvanceRevision(m_cache_revision);
        cache.watch_ready = true;
        cache.cursor = page.cursor;
        cache.endpoint = endpoint;
        cache.status = std::move(page.status);
        if (page.reported) cache.reported = std::move(*page.reported);
        if (learn_endpoint) {
            LearnMetadata(cache.status, cache.pins, endpoint);
            LearnActionWait(cache.status, endpoint);
            LearnReadiness(FlowMeshAdvertisedReadinessOf(cache.status), endpoint, id);
        }
    }
    // A foreground wallet signing preflight may reuse the refresh that
    // completed just before it (typically the refresh it queued behind, or
    // the balance read that preceded an order) instead of another 'updates'
    // round trip. Every local chain/authority check and Project's checkpoint
    // check still run; any doubt falls back to Refresh.
    Cache* JoinFresh(const uint256& id, const std::optional<uint256>& account)
    {
        if (m_read_context_exhausted || !account || !FlowMeshSigningPreflightScope::Active() ||
            FlowMeshClientWorkScope::Current() != FlowMeshClientWorkPriority::FOREGROUND ||
            !m_own_certified_through.Complete()) return nullptr;
        const auto it{m_cache.find(id)};
        if (it == m_cache.end() || !it->second.fresh) return nullptr;
        auto& cache{it->second};
        try {
            std::vector<FlowMeshJoinOwnAction> own;
            for (const auto& [key, p] : m_pending) {
                if (p.market != id || p.action.IsDeposit() || p.action.signer != *account) continue;
                own.push_back(FlowMeshJoinOwnActionFor(p.receipt, p.previously_certified, p.not_executed, p.action.sequence));
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
        // Display only: the earliest a stamp of the head can plausibly be (the
        // block that made its anchor final, less the block future bound).
        std::optional<int64_t> head_floor_ms;
        {
            LOCK(::cs_main); SyncIndexes();
            const auto checkpoint{m_chainman.ActiveChainstate().ModernFlowMeshCheckpoints().Index().Head(cache.pins.market_id)};
            if (checkpoint && (out->snapshot.next_microblock_sequence <= checkpoint->core.sequence ||
                (out->snapshot.next_microblock_sequence == checkpoint->core.sequence + 1 &&
                 out->snapshot.last_microblock_hash != checkpoint->core.microblock_hash)))
                Fail("B3 checkpoint advanced while reading account state; refresh required before signing");
            out->b3_checkpoint_confirmed = checkpoint && checkpoint->core.microblock_hash == out->snapshot.last_microblock_hash &&
                checkpoint->core.state_root == out->snapshot.state_root;
            if (query.observed_time) {
                const auto final_height{static_cast<int64_t>(cache.verified.certified.entry.anchor.height) + Consensus::FLOWMESH_ANCHOR_DEPTH};
                const auto& chain{m_chainman.ActiveChain()};
                if (final_height >= 0 && final_height <= chain.Height()) {
                    head_floor_ms = chain[static_cast<int>(final_height)]->GetBlockTime() * 1000 - 120'000;
                }
            }
        }
        if (!out->unchanged) ReportedHistory(*out, cache.reported);
        if (query.ticks) out->ticks = TicksView(cache, *query.ticks);
        // Never on a signing preflight (it never asks), never a failure.
        if (query.observed_time) out->observed = ObservedView(cache, *out, head_floor_ms);
        return std::move(*out);
    }
    struct ReadContext {
        uint256 market;
        std::optional<uint256> account;
        flowmesh::ClientEvidencePins pins;
        uint64_t epoch{0};
        std::optional<uint64_t> cache_revision;
        std::optional<std::pair<uint64_t, uint256>> highwater;
        std::optional<uint64_t> own_through;
        bool own_complete{false};
    };
    struct ReadSuperseded : std::runtime_error {
        ReadSuperseded() : std::runtime_error{"Market read was superseded; retained data is not a fresh response"} {}
    };
    ReadContext CaptureReadContext(const uint256& id, const std::optional<uint256>& account)
    {
        if (m_read_context_exhausted) throw ReadSuperseded{};
        ReadContext context{id, account, Pins(id), m_read_epoch, std::nullopt, std::nullopt,
                            m_own_certified_through.Through(id), m_own_certified_through.Complete()};
        if (const auto it{m_cache.find(id)}; it != m_cache.end()) context.cache_revision = it->second.revision;
        if (const auto it{m_highwater.find(id)}; it != m_highwater.end()) context.highwater = it->second;
        return context;
    }
    bool SameReadContext(const ReadContext& context) const
    {
        if (m_read_context_exhausted || context.epoch != m_read_epoch || context.own_through != m_own_certified_through.Through(context.market) ||
            context.own_complete != m_own_certified_through.Complete()) return false;
        const auto cache{m_cache.find(context.market)};
        if (context.cache_revision ? cache == m_cache.end() || cache->second.revision != *context.cache_revision : cache != m_cache.end()) return false;
        const auto high{m_highwater.find(context.market)};
        return context.highwater ? high != m_highwater.end() && high->second == *context.highwater : high == m_highwater.end();
    }
    void CheckReadContext(const ReadContext& context)
    {
        if (!SameReadContext(context)) throw ReadSuperseded{};
        const auto pins{Pins(context.market)};
        if (pins.domain != context.pins.domain || pins.market_id != context.pins.market_id ||
            pins.base_asset != context.pins.base_asset || pins.vault_id != context.pins.vault_id ||
            pins.execution_config_id != context.pins.execution_config_id) throw ReadSuperseded{};
    }
    void ApplyTickWithdrawals(const std::map<size_t, uint64_t>& withdrawn)
    {
        // Compare every staged withdrawal with the same pre-apply revision;
        // our own first EndpointChanged must not hide another current entry.
        const auto observations{m_endpoint_revision};
        for (const auto& [endpoint, origin] : withdrawn) {
            if (origin != observations) continue;
            EndpointChanged();
            m_clearing_ticks.at(endpoint) = 0;
            m_observed_time.at(endpoint) = false;
            m_clearing_ticks_withdrawn.at(endpoint) = std::chrono::steady_clock::now() + FLOWMESH_CLIENT_TICKS_WITHDRAWN_FOR;
        }
    }
    //! Only ordinary market reads use this split. No economic request, poll
    //! scheduler, caller-owned Pending, or general Call callback is unlocked.
    //! apply runs under m_work; false stages a page for a following snapshot.
    void OrdinaryReadCall(const std::string& method, const UniValue& params, const ReadContext& context,
                          TickRequest* ticks, std::map<size_t, uint64_t>& withdrawn,
                          const std::function<bool(const UniValue&, size_t, bool)>& apply)
    {
        size_t first{0}, count{0};
        {
            WorkLock lock{*this};
            CheckReadContext(context);
            count = m_endpoints.size();
            if (!count) Fail("No FlowMesh HTTPS trading endpoint configured; use flowmeshclientconnect or -flowmeshendpoint");
            first = m_preferred;
        }
        std::string error;
        for (size_t attempt{0}; attempt < count; ++attempt) {
            const size_t endpoint{(first + attempt) % count};
            bool decorating{ticks != nullptr};
            for (;;) { // At most the existing one undecorated retry.
                HttpsEndpoint target;
                std::string body;
                uint64_t observations{0};
                {
                    WorkLock lock{*this};
                    CheckReadContext(context);
                    if (std::chrono::steady_clock::now() < m_retry_after.at(endpoint)) break;
                    target = m_endpoints.at(endpoint);
                    observations = m_endpoint_revision;
                    UniValue endpoint_params{params};
                    if (ticks) {
                        ticks->sent.reset();
                        ticks->observed_sent = false;
                        if (decorating && !withdrawn.contains(endpoint)) TickDecorator(ticks)(endpoint_params, endpoint);
                    }
                    UniValue request{UniValue::VOBJ};
                    request.pushKV("method", method);
                    request.pushKV("params", std::move(endpoint_params));
                    body = request.write();
                }
                UniValue result;
                bool response_received{false}, transport_available{false}, unknown_field{false};
                std::string failure;
                {
                    // No lane mutex is retained when taking m_work to apply.
                    // The outer admission lease still bounds the parsed body.
                    std::unique_lock lane{m_read_lane, std::try_to_lock};
                    if (!lane.owns_lock()) Fail("Ordinary market read lane is busy");
                    if (m_read_reset.exchange(false)) m_read_https.Reset();
                    const auto started{std::chrono::steady_clock::now()};
                    const auto reply{m_read_https.Request(target, "/flowmesh/v1", body, CLIENT_REQUEST_TIMEOUT, CLIENT_MAX_REPLY)};
                    response_received = reply.response_received;
                    try {
                        if (!reply.response_received) Fail(reply.error.empty() ? "No HTTPS response; outcome unknown" : reply.error);
                        const auto remaining{std::chrono::duration_cast<std::chrono::milliseconds>(started + CLIENT_REQUEST_TIMEOUT - std::chrono::steady_clock::now())};
                        std::string trust_error;
                        if (!FlowMeshHttpsTrustCurrent(target, reply.trust, remaining, trust_error)) Fail(trust_error);
                        transport_available = true;
                        UniValue parsed;
                        if (!parsed.read(reply.body)) Fail("Malformed endpoint JSON response");
                        Keys(parsed, {"ok", "result", "error"});
                        if (!Flag(parsed, "ok")) {
                            const auto text{Text(parsed, "error")};
                            unknown_field = text == FLOWMESH_CLIENT_UNKNOWN_FIELD_ERROR;
                            Fail(text);
                        }
                        if (reply.status != 200) Fail("Unexpected HTTPS response status");
                        result = parsed["result"];
                    } catch (const std::exception& e) {
                        failure = e.what();
                    }
                    if (!failure.empty()) m_read_https.Reset();
                }
                WorkLock lock{*this};
                CheckReadContext(context);
                const bool current_observation{observations == m_endpoint_revision};
                if (unknown_field && decorating && ticks) {
                    // A capability rejection is not authoritative state.
                    // Commit the withdrawal only with this read's final head.
                    // Keep the local undecorated retry, but never let that
                    // later attempt launder a stale advertisement withdrawal.
                    withdrawn.insert_or_assign(endpoint, observations);
                    ticks->sent.reset();
                    ticks->observed_sent = false;
                    decorating = false;
                    continue;
                }
                if (failure.empty()) {
                    try {
                        const bool published{apply(result, endpoint, current_observation)};
                        if (published && current_observation) {
                            m_selected = endpoint;
                            EndpointResult(endpoint, true, {});
                        }
                        return;
                    } catch (const std::exception& e) {
                        // A durable save may already have advanced irreversible
                        // obligations. Never walk another endpoint over them.
                        if (!SameReadContext(context)) throw;
                        failure = e.what();
                    }
                }
                error = failure;
                // A concurrent trade/status observation may supersede only
                // transport bookkeeping, not this body's valid certificate.
                if (current_observation) EndpointResult(endpoint, transport_available || response_received, failure);
                break;
            }
        }
        if (error.empty()) error = "Trading endpoints are waiting to retry";
        Fail(error);
    }
    struct SnapshotContinuation {
        ReadContext context;
        flowmesh::ClientEventCursor page;
        bool gap{false};
        std::chrono::steady_clock::time_point started;
    };
    //! Entire ordinary read, including updates -> snapshot, owns one lease.
    //! finish is called only under m_work and must return values, not refs.
    void RefreshRead(const uint256& id, const std::optional<uint256>& account, const flowmesh::MarketDataQuery& query,
                     const ReadRequest& request, const std::function<void(Cache&)>& finish,
                     const bool join_after_admission = false,
                     const std::optional<SnapshotContinuation>& continuation = std::nullopt)
    {
        // Default-off bounded capture for direct foreground admission only.
        // Watch's IMMEDIATE path can retain its lane and emits no new span.
        // BENCH must remain disabled throughout the declared capture profile.
        // Declaration before lease keeps emission after lease/all local locks.
        std::optional<FlowMeshTimingSpan> admission_timing;
        if (request.policy == FlowMeshReadAdmission::FOREGROUND_ACCOUNT &&
            FlowMeshTimingRecording() && !LogAcceptCategory(BCLog::BENCH, BCLog::Level::Debug)) {
            admission_timing.emplace("client_read_admission");
            admission_timing->Field("market_id", id);
            if (account) admission_timing->Field("account_id", *account);
            admission_timing->Field("has_account", uint64_t{account.has_value()});
            admission_timing->Field("admission_policy", static_cast<uint64_t>(request.policy));
            admission_timing->Field("join_after_admission", uint64_t{join_after_admission});
            // Same steady-clock/microsecond epoch as FlowMeshTimingSpan::Mark.
            admission_timing->Field("request_started_us", static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(request.started.time_since_epoch()).count()));
            admission_timing->Mark("admission_requested_us");
        }
        // No m_work/lane lock, captured authority, params or body before this
        // reservation. Its wait is queue-only: HTTPS keeps its existing cap.
        FlowMeshClientReadAdmission::Lease lease{m_read_admission, request.policy,
            request.started + FlowMeshClientReadAdmission::QUEUE_TIMEOUT};
        if (admission_timing) admission_timing->Mark("admission_acquired_us");
        const auto started{continuation ? continuation->started : request.started};
        ReadContext context;
        bool context_captured{false};
        TickRequest ticks;
        bool snapshot{false}, gap{false};
        uint64_t gaps{0};
        std::optional<flowmesh::ClientEventCursor> page;
        UniValue params{UniValue::VOBJ};
        params.pushKV("market_id", id.GetHex());
        if (account) params.pushKV("account_id", account->GetHex());
        try {
        {
            WorkLock lock{*this};
            if (join_after_admission) {
                // A current new observation may have completed while queued.
                // Keep its ORIGINAL stamp; do not renew freshness or recurse
                // into another admission reservation when a join is refused.
                if (auto* cache{JoinFresh(id, account)}) { finish(*cache); return; }
            }
            if (continuation) CheckReadContext(continuation->context);
            const auto it{m_cache.find(id)};
            // Pending I/O does not erase a still-valid ORIGINAL join stamp.
            // JoinFresh rechecks its unchanged age and every local obligation;
            // explicit Data still requires this read's own observation.
            context = CaptureReadContext(id, account);
            context_captured = true;
            ticks = TicksAfterRing(id);
            ticks.observed = query.observed_time;
            if (continuation) {
                snapshot = true;
                page = continuation->page;
                gap = continuation->gap;
                gaps = uint64_t{gap};
            } else if (it == m_cache.end()) {
                snapshot = true;
            } else if (CheckCache(id, it->second, account) != CacheCheck::CURRENT) {
                snapshot = true;
                gap = true;
                gaps = 1;
            } else {
                const auto& cache{it->second};
                params.pushKV("cursor", CursorJson(cache.cursor));
                const bool same_account{!account || (cache.reported["account"].isObject() &&
                    cache.reported["account"]["account_id"].isStr() && cache.reported["account"]["account_id"].get_str() == account->GetHex())};
                if (same_account) params.pushKV("known_head", cache.verified.certified.entry.GetHash().GetHex());
                params.pushKV("limit", static_cast<uint64_t>(query.limit));
                if (query.before_sequence) params.pushKV("before_sequence", *query.before_sequence);
            }
        }
        std::map<size_t, uint64_t> withdrawn;
        const auto complete = [&](Cache& cache) {
            if (query.ticks) MergeTicks(id, cache, ticks);
            cache.fresh = std::pair{started, std::chrono::steady_clock::now()};
            finish(cache);
        };
        if (!snapshot) {
            OrdinaryReadCall("updates", params, context, query.ticks ? &ticks : nullptr, withdrawn,
                [&](const UniValue& value, const size_t endpoint, const bool learn_endpoint) {
                    auto& cache{m_cache.at(id)};
                    if (CheckCache(id, cache, account) != CacheCheck::CURRENT) throw ReadSuperseded{};
                    auto updates{ParseUpdatesPage(cache, id, value)};
                    KeepTickPage(query.ticks ? &ticks : nullptr, value, endpoint);
                    snapshot = updates.snapshot_needed;
                    if (snapshot) {
                        page = updates.cursor;
                        gap = updates.gap;
                        gaps = uint64_t{gap};
                        return false;
                    }
                    if (learn_endpoint) ApplyTickWithdrawals(withdrawn);
                    InstallUpdatesPage(cache, id, std::move(updates), endpoint, learn_endpoint);
                    complete(cache);
                    return true;
                });
        }
        if (snapshot) {
            UniValue request{UniValue::VOBJ};
            request.pushKV("market_id", id.GetHex());
            if (account) request.pushKV("account_id", account->GetHex());
            OrdinaryReadCall("snapshot", request, context, query.ticks ? &ticks : nullptr, withdrawn,
                [&](const UniValue& value, const size_t endpoint, const bool learn_endpoint) {
                    InstallSnapshot(id, account, context.pins, value, endpoint, page, query.ticks ? &ticks : nullptr, gap, gaps, withdrawn, learn_endpoint);
                    complete(m_cache.at(id));
                    return true;
                });
        }
        } catch (...) {
            // A failed current read withdraws its old join permission. A late
            // completion must not clear a newer watch/read/trust context.
            WorkLock lock{*this};
            if (context_captured && SameReadContext(context)) {
                if (const auto it{m_cache.find(id)}; it != m_cache.end() && it->second.fresh) {
                    it->second.fresh.reset();
                    it->second.revision = AdvanceRevision(m_cache_revision);
                }
            }
            throw;
        }
    }
    flowmesh::ClientEvidencePins RecheckActionAuthority(Pending& p)
    {
        const auto pins{Pins(p.market)};
        if (pins.domain != p.domain || pins.execution_config_id != p.config) Fail("Retained action domain/config differs from current local market");
        if (ProvenNotExecuted(p)) {
            // The consumer's certificate must still verify under current
            // authority. On failure only the label is withdrawn; the durable
            // marker stays, so the action is still never resent.
            std::string failure;
            if (p.consumer_proof.empty()) {
                failure = "Previously proven not executed; fresh authority evidence required, and this action will not be resubmitted";
            } else {
                try {
                    if (VerifyEntry(pins, p.consumer_proof).entry.GetHash() != p.not_executed->microblock_hash) {
                        failure = "Prior not-executed proof no longer matches its consumer";
                    }
                } catch (const std::exception& e) {
                    failure = std::string{"Prior not-executed proof no longer verified: "} + e.what();
                }
            }
            if (!failure.empty()) {
                WithdrawVerifiedLabels(p.receipt);
                p.receipt.reason = failure;
            }
        }
        if (p.receipt.certificate_verified && !p.inclusion_proof.empty()) {
            try { (void)VerifyEntry(pins, p.inclusion_proof); return pins; }
            catch (const std::exception& e) {
                WithdrawVerifiedLabels(p.receipt);
                p.receipt.reason = std::string{"Prior inclusion authority no longer verified: "} + e.what();
            }
        }
        if (p.receipt.certificate_verified && p.inclusion_proof.empty()) {
            WithdrawVerifiedLabels(p.receipt);
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
            ApplyOutcomeEvidence(p, proof.entry, value, endpoint);
        } else if ((receipt.state == "rejected" || receipt.state == "unknown") && p.may_have_been_sent) {
            // One node's rejection/absence cannot undo an earlier possible
            // delivery. Preserve uncertainty, not a fresh nonce/order.
            if (p.receipt.state != "queued" && p.receipt.state != "admitted") p.receipt.state = "unknown";
            p.receipt.reason = "No verified inclusion yet; endpoint observation: " + receipt.state + ": " + receipt.reason;
        } else {
            p.receipt = std::move(receipt);
        }
        if (!p.owner_account.IsNull()) p.receipt.account_id = p.owner_account;
        if (!p.receipt.certificate_verified) TryProveNotExecuted(p, pins, value, endpoint);
    }
    /** An opt-in 'action' reply may name the certified action X that used
     * this action's account sequence (pool_notice.consumed_by {X, M, hash})
     * and carry X's certified entry and its outcome evidence. The client
     * accepts "not executed" only from its own checks: the entry's
     * certificate under local B3 authority, exactly (M, hash), evidence that
     * hashes to its result root, and ProvesSequenceConsumedByOther. Anything
     * else leaves the receipt as it was; the endpoint's words are only an
     * observation. Never throws. Actions certified before (their inclusion
     * answers them) and deposits (no sequence) are never marked. */
    void TryProveNotExecuted(Pending& p, const flowmesh::ClientEvidencePins& pins, const UniValue& value,
                             const size_t endpoint) noexcept
    {
        if (p.previously_certified || p.action.IsDeposit() || ProvenNotExecuted(p)) return;
        try {
            if (!value.isObject() || !value["pool_notice"].isObject() || !value["pool_notice"].exists("consumed_by") ||
                !value.exists("consumer_certified_payload") || !value.exists("consumer_outcome_evidence")) return;
            const auto& named{value["pool_notice"]["consumed_by"]};
            const flowmesh::ClientConsumerRef consumer{Id(named, "action_id"), Number(named, "microblock_sequence"),
                                                       Id(named, "microblock_hash")};
            // A different marker, restored or proven, is never replaced.
            if (p.not_executed && !(*p.not_executed == consumer)) return;
            auto payload{Bytes(value, "consumer_certified_payload", flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_BYTES + 1024)};
            const auto evidence{Bytes(value, "consumer_outcome_evidence", flowmesh::FLOWMESH_OUTCOME_EVIDENCE_MAX_BYTES)};
            const auto proof{VerifyEntry(pins, payload)};
            if (!FlowMeshProvesNotExecuted(proof.entry, evidence, p.action, consumer)) return;
            MarkNotExecuted(p, consumer, std::move(payload), endpoint);
        } catch (...) {
        }
    }
    /** Records a proof just checked by TryProveNotExecuted or
     * ReproveNotExecuted, then makes its marker durable with a synchronous
     * outbox write. If that write fails, every label is withdrawn and a
     * marker this call added is dropped (the action is then only unknown, as
     * before; resending it is harmless because it can never execute). */
    void MarkNotExecuted(Pending& p, const flowmesh::ClientConsumerRef& consumer, std::vector<unsigned char> payload,
                         const size_t endpoint)
    {
        const bool had_marker{p.not_executed.has_value()};
        p.receipt = {};
        p.receipt.action_id = p.action.Id();
        p.receipt.state = "rejected";
        p.receipt.endpoint = m_endpoints.at(endpoint).url;
        p.receipt.outcome = FlowMeshNotExecutedOutcome(consumer.action_id, consumer.microblock_sequence, consumer.microblock_hash);
        p.receipt.outcome_verified = true;
        p.receipt.reason_code = p.receipt.outcome->reason_code;
        p.receipt.reason = "Proven not executed: certified action " + consumer.action_id.GetHex() +
                           " used this account sequence in microblock #" + std::to_string(consumer.microblock_sequence) +
                           "; this action can never execute and will not be resubmitted";
        if (!p.owner_account.IsNull()) p.receipt.account_id = p.owner_account;
        p.not_executed = consumer;
        RetainProof(p, p.consumer_proof, payload);
        m_action_polls.Remove({p.market, p.action.Id()});
        // The consumer changed this account at M: a join needs state at M.
        m_own_certified_through.Record(p.market, consumer.microblock_sequence);
        if (const auto cache{m_cache.find(p.market)}; cache != m_cache.end() &&
            consumer.microblock_sequence > cache->second.verified.certified.entry.sequence) {
            cache->second.fresh.reset();
            cache->second.revision = AdvanceRevision(m_cache_revision);
        }
        try {
            Save();
        } catch (const std::exception& e) {
            WithdrawVerifiedLabels(p.receipt);
            if (!had_marker) {
                p.not_executed.reset();
                p.consumer_proof.clear();
            }
            p.receipt.reason = std::string{"Not-executed proof could not be saved; outcome unknown: "} + e.what();
        }
    }
    /** After a restart a retained marker forbids resending but is not a
     * verified outcome. At most one re-proof per process: one bounded opt-in
     * 'action' read of the consumer X from one endpoint that advertises
     * outcomes, charged to the automatic status budget, never failed over.
     * X's certified inclusion at exactly (M, hash) plus its outcome evidence
     * proves again that X used this action's sequence. */
    void ReproveNotExecuted(Pending& p, const flowmesh::ClientEvidencePins& pins)
    {
        if (!p.not_executed || ProvenNotExecuted(p) || p.not_executed_reproof_asked || m_endpoints.empty()) return;
        const auto now{std::chrono::steady_clock::now()};
        std::optional<size_t> endpoint;
        for (size_t attempt{0}; attempt < m_endpoints.size() && !endpoint; ++attempt) {
            const size_t candidate{(m_selected + attempt) % m_endpoints.size()};
            if (m_action_outcome.at(candidate) && now >= m_retry_after.at(candidate)) endpoint = candidate;
        }
        if (!endpoint || !m_action_polls.TryChargeAttempt(now)) return;
        p.not_executed_reproof_asked = true;
        const auto consumer{*p.not_executed};
        try {
            UniValue params{UniValue::VOBJ};
            params.pushKV("market_id", p.market.GetHex());
            params.pushKV("action_id", consumer.action_id.GetHex());
            params.pushKV("outcome", true);
            UniValue request{UniValue::VOBJ};
            request.pushKV("method", "action");
            request.pushKV("params", std::move(params));
            const auto reply{m_https.Request(m_endpoints[*endpoint], "/flowmesh/v1", request.write(), CLIENT_REQUEST_TIMEOUT,
                                             CLIENT_ACTION_OUTCOME_MAX_REPLY)};
            if (!reply.response_received || reply.status != 200) return;
            UniValue parsed;
            if (!parsed.read(reply.body) || !parsed.isObject() || !parsed["ok"].isTrue()) return;
            const auto& result{parsed["result"]};
            m_action_polls.RecordExtraCost(std::chrono::steady_clock::now(), OutcomeExtraCost(result));
            if (Id(result, "action_id") != consumer.action_id || Text(result, "receipt_state", 32) != "certified_inclusion" ||
                Id(result, "microblock_hash") != consumer.microblock_hash ||
                Number(result, "microblock_sequence") != consumer.microblock_sequence) return;
            auto payload{Bytes(result, "certified_payload", flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_BYTES + 1024)};
            const auto evidence{Bytes(result, "outcome_evidence", flowmesh::FLOWMESH_OUTCOME_EVIDENCE_MAX_BYTES)};
            const auto proof{VerifyEntry(pins, payload)};
            if (!FlowMeshProvesNotExecuted(proof.entry, evidence, p.action, consumer)) return;
            MarkNotExecuted(p, consumer, std::move(payload), *endpoint);
        } catch (const std::exception&) {
            // The marker stays; the receipt stays unknown.
        }
    }
    //! Verifies an endpoint's outcome evidence against an entry whose
    //! certificate was just verified under local B3 authority, and derives
    //! this action's outcome from it. Never throws and never touches the
    //! inclusion fields: any failure sets only an evidence reason code, and
    //! the evidence bytes are never retained.
    void ApplyOutcomeEvidence(Pending& p, const flowmesh::ProductionEntryCore& entry, const UniValue& value,
                              const size_t endpoint) noexcept
    {
        using Code = flowmesh::ActionReasonCode;
        const auto code = [&](Code c) { p.receipt.reason_code = std::string{flowmesh::ActionReasonName(c)}; };
        try {
            if (!p.receipt.certificate_verified || p.receipt.state != "certified_inclusion" ||
                entry.GetHash() != p.receipt.microblock_hash) return;
            // Already verified for this inclusion (kept by Inclusion): a later
            // reply's missing or unusable evidence never withdraws it.
            if (p.receipt.outcome_verified) return;
            if (!value.isObject() || !value.exists("outcome_evidence")) {
                const std::string error{value.isObject() && value["outcome_evidence_error"].isStr()
                                            ? value["outcome_evidence_error"].get_str() : std::string{}};
                code(error == "EXPIRED" ? Code::OUTCOME_EVIDENCE_EXPIRED
                     : error == "TOO_LARGE" ? Code::OUTCOME_EVIDENCE_TOO_LARGE
                     : error == "BUDGET" ? Code::OUTCOME_EVIDENCE_BUDGET
                     : m_action_outcome.at(endpoint) ? Code::OUTCOME_EVIDENCE_INVALID : Code::OUTCOME_NOT_OFFERED);
                return;
            }
            const auto bytes{Bytes(value, "outcome_evidence", flowmesh::FLOWMESH_OUTCOME_EVIDENCE_MAX_BYTES)};
            const auto evidence{flowmesh::DecodeExecutionOutcomeEvidenceV1(bytes)};
            const auto derived{evidence && flowmesh::VerifyExecutionOutcomeEvidence(entry, *evidence) == flowmesh::OutcomeEvidenceCheck::OK
                                   ? flowmesh::DeriveActionOutcome(entry, *evidence, p.action) : std::nullopt};
            if (!derived) {
                code(Code::OUTCOME_EVIDENCE_INVALID);
                return;
            }
            p.receipt.outcome = FlowMeshReceiptOutcome(*derived, entry.sequence, p.receipt.microblock_hash,
                                                       "certified_result_preimage");
            p.receipt.outcome_verified = true;
            p.receipt.reason_code = p.receipt.outcome->reason_code;
        } catch (...) {
            code(Code::OUTCOME_EVIDENCE_INVALID);
        }
    }
    //! Outcome-only upgrade of a verified inclusion learned without evidence
    //! (for example from a snapshot): at most one 'outcome_evidence' request
    //! per (action, endpoint) per process, only to the advertising endpoint
    //! that served the inclusion, charged to the automatic status budget.
    //! One endpoint and one bounded request: no failover loop. The entry is
    //! the retained proof RecheckActionAuthority just re-verified.
    void UpgradeOutcome(Pending& p)
    {
        if (!p.receipt.certificate_verified || p.receipt.outcome_verified || p.inclusion_proof.empty()) return;
        const auto found{std::find_if(m_endpoints.begin(), m_endpoints.end(),
                                      [&](const auto& endpoint) { return endpoint.url == p.receipt.endpoint; })};
        if (found == m_endpoints.end()) return;
        const size_t endpoint{static_cast<size_t>(std::distance(m_endpoints.begin(), found))};
        if (!m_action_outcome.at(endpoint)) {
            if (p.receipt.reason_code.empty()) {
                p.receipt.reason_code = std::string{flowmesh::ActionReasonName(flowmesh::ActionReasonCode::OUTCOME_NOT_OFFERED)};
            }
            return;
        }
        if (p.outcome_upgrade_asked.contains(endpoint) ||
            std::chrono::steady_clock::now() < m_retry_after.at(endpoint) ||
            !m_action_polls.TryChargeAttempt(std::chrono::steady_clock::now())) return;
        p.outcome_upgrade_asked.insert(endpoint);
        try {
            const auto entry{EntryPrefix(p.inclusion_proof)};
            if (entry.GetHash() != p.receipt.microblock_hash) return;
            UniValue params{UniValue::VOBJ};
            params.pushKV("market_id", p.market.GetHex());
            params.pushKV("microblock_sequence", entry.sequence);
            params.pushKV("microblock_hash", entry.GetHash().GetHex());
            UniValue request{UniValue::VOBJ};
            request.pushKV("method", "outcome_evidence");
            request.pushKV("params", std::move(params));
            const auto reply{m_https.Request(m_endpoints[endpoint], "/flowmesh/v1", request.write(), CLIENT_REQUEST_TIMEOUT,
                                             CLIENT_OUTCOME_EVIDENCE_MAX_REPLY)};
            if (!reply.response_received) return;
            // The method spends one extra endpoint token for the evidence.
            m_action_polls.RecordExtraCost(std::chrono::steady_clock::now(), 1);
            UniValue parsed;
            if (!parsed.read(reply.body) || !parsed.isObject() || !parsed["ok"].isBool()) return;
            if (!parsed["ok"].get_bool()) {
                // Availability only; the inclusion stays verified.
                using Code = flowmesh::ActionReasonCode;
                const std::string text{parsed["error"].isStr() ? parsed["error"].get_str() : std::string{}};
                std::optional<Code> code;
                if (text == "FlowMesh outcome evidence is unavailable: EXPIRED") code = Code::OUTCOME_EVIDENCE_EXPIRED;
                if (text == "FlowMesh outcome evidence is unavailable: TOO_LARGE") code = Code::OUTCOME_EVIDENCE_TOO_LARGE;
                if (text == "FlowMesh outcome evidence is unavailable: HASH_MISMATCH") code = Code::OUTCOME_EVIDENCE_INVALID;
                if (text == FLOWMESH_API_OUTCOME_BUDGET_ERROR) code = Code::OUTCOME_EVIDENCE_BUDGET;
                if (code) p.receipt.reason_code = std::string{flowmesh::ActionReasonName(*code)};
                return;
            }
            ApplyOutcomeEvidence(p, entry, parsed["result"], endpoint);
        } catch (const std::exception&) {
            // Inclusion stays verified; the outcome stays unverified.
        }
    }
    Receipt QueryAction(Pending& p, bool automatic = false)
    {
        const auto pins{RecheckActionAuthority(p)};
        if (p.receipt.certificate_verified || ProvenNotExecuted(p)) {
            SaveVerifiedRestartState(p);
            return p.receipt;
        }
        if (p.not_executed) {
            ReproveNotExecuted(p, pins);
            if (ProvenNotExecuted(p)) return p.receipt;
        }
        if (!automatic) p.automatic_endpoint.reset();
        UniValue params{UniValue::VOBJ}; params.pushKV("market_id", p.market.GetHex()); params.pushKV("action_id", p.action.Id().GetHex());
        Call("action", params, [&](const UniValue& value, size_t endpoint) {
            m_action_polls.RecordExtraCost(std::chrono::steady_clock::now(), OutcomeExtraCost(value));
            ApplyActionResult(p, pins, value, endpoint);
        }, nullptr, nullptr, nullptr, automatic ? &p : nullptr, [&](UniValue& endpoint_params, size_t endpoint) {
            // Only an endpoint that advertised it receives "outcome".
            if (m_action_outcome.at(endpoint)) endpoint_params.pushKV("outcome", true);
        });
        // A proof saved its own marker (or withdrew it) in MarkNotExecuted.
        if (ProvenNotExecuted(p)) return p.receipt;
        if (p.not_executed && !p.receipt.certificate_verified) {
            p.receipt.reason = std::string{CLIENT_NOT_EXECUTED_RETAINED} + ". " + p.receipt.reason;
        }
        if (!p.receipt.certificate_verified) m_action_polls.MarkObserved({p.market, p.action.Id()});
        // A non-certifying observation changes only the receipt, which
        // restart never reads.
        if (p.receipt.certificate_verified) Save();
        else SaveRestartState();
        return p.receipt;
    }
    //! `skip`, when set, is an endpoint this delivery must not use (a lane
    //! submission already tried it).
    Receipt Send(Pending& p, const std::optional<size_t> skip = std::nullopt)
    {
        if (p.lane_in_flight) {
            // Another call's waited submission of these exact bytes is in
            // flight: nothing is sent again until its outcome is applied.
            SaveVerifiedRestartState(p);
            Receipt out{p.receipt};
            out.reason = "A waited submission of this exact action is in flight; its outcome is not known yet";
            return out;
        }
        if (p.previously_certified) {
            try { QueryAction(p); }
            catch (const std::exception& e) {
                WithdrawVerifiedLabels(p.receipt); p.receipt.reason = e.what();
            }
            if (!p.receipt.certificate_verified) p.receipt.reason = "Previously verified inclusion is retained; fresh proof required for current authority, and this action will not be resubmitted";
            return p.receipt;
        }
        if (p.not_executed) {
            // Proven never to execute: never resent, exactly like a
            // previously certified action.
            try { QueryAction(p); }
            catch (const std::exception& e) {
                if (!ProvenNotExecuted(p)) p.receipt.reason = e.what();
            }
            if (!ProvenNotExecuted(p) && !p.receipt.certificate_verified) p.receipt.reason = CLIENT_NOT_EXECUTED_RETAINED;
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
            }, &possible, &earlier_possible, nullptr, nullptr, {}, skip);
        } catch (const std::exception& e) {
            p.receipt.state = "unknown"; p.receipt.reason = e.what();
        }
        // Even a TLS failure is conservatively retryable; only an explicit
        // rejection with no earlier possibly-delivered attempt is definite.
        // The write-ahead Save above already made the delivery flag durable;
        // this observation changes only the receipt.
        SaveRestartState(); return p.receipt;
    }
    //! Under m_work while holding the wait lane, right before a lane request
    //! to `target`: consumes a pending trust reset (set with m_work held by
    //! Connect and Reconnect), then carries m_https's connection on the lane
    //! when only that one would be reused for `target`. True if it did.
    bool BorrowForLane(const HttpsEndpoint& target)
    {
        if (m_wait_reset.exchange(false)) m_wait_https.Reset();
        return !m_wait_https.Warm(target) && m_https.Warm(target) && m_wait_https.Swap(m_https);
    }
    //! Under m_work, still holding the wait lane, after that request: hands a
    //! carried connection back if it is still reusable, no explicit Connect
    //! or Reconnect reset trust meanwhile (the lane then drops it before its
    //! next use), and ordinary work has not meanwhile opened a connection to
    //! the selected endpoint (the lane then keeps the carried one). True if
    //! it did.
    bool ReturnLaneBorrow(const bool borrowed, const HttpsEndpoint& target)
    {
        if (!borrowed || m_wait_reset.load() || !m_wait_https.Warm(target) ||
            m_https.Warm(m_endpoints.at(m_selected))) return false;
        return m_https.Swap(m_wait_https);
    }
    //! Applies an endpoint's 'action' reply that arrived on the wait lane (a
    //! waited read's, or the one a waited submission embeds) to p, under
    //! m_work, by QueryAction's rules. A receipt verified, or an action proven
    //! not executed, meanwhile is returned unchanged (evidence for the same
    //! entry may still verify its outcome). An applied certification is saved
    //! before it is returned. nullopt when nothing was applied and no verified
    //! label changed: the caller falls back to its ordinary path. A reply
    //! applied whose outbox save then failed, or a verified label the current
    //! authority no longer accepts, is returned unverified with the failure as
    //! its reason. `label` names the result for diagnostics.
    std::optional<Receipt> ApplyLaneResult(Pending& p, const UniValue& result, const size_t endpoint,
                                           FlowMeshTimingSpan& timing, std::string& label)
    {
        const std::pair key{p.market, p.action.Id()};
        const bool verified_before{p.receipt.certificate_verified || ProvenNotExecuted(p)};
        bool applied{false};
        try {
            const auto pins{RecheckActionAuthority(p)};
            if (p.receipt.certificate_verified) {
                // Proved by another read meanwhile: never replaced by this one.
                // Evidence in this reply for the same entry may still verify
                // its outcome; the inclusion fields are never touched.
                m_action_polls.Remove(key);
                if (!p.receipt.outcome_verified && result.isObject() && result["certified_payload"].isStr() &&
                    !p.inclusion_proof.empty()) {
                    try {
                        const auto entry{EntryPrefix(p.inclusion_proof)};
                        if (entry.GetHash() == p.receipt.microblock_hash &&
                            Id(result, "microblock_hash") == p.receipt.microblock_hash) {
                            ApplyOutcomeEvidence(p, entry, result, endpoint);
                        }
                    } catch (const std::exception&) {}
                }
                applied = true; // The verified return also requires durability.
                SaveVerifiedRestartState(p);
                label = "already_verified";
                return p.receipt;
            }
            if (ProvenNotExecuted(p)) {
                // Proven not executed meanwhile: final, never replaced.
                m_action_polls.Remove(key);
                applied = true;
                SaveVerifiedRestartState(p);
                label = "already_verified";
                return p.receipt;
            }
            ApplyActionResult(p, pins, result, endpoint);
            applied = true;
            EndpointResult(endpoint, true, {}, /*names_active=*/false);
            if (p.receipt.certificate_verified) {
                Save();
            } else if (ProvenNotExecuted(p)) {
                // MarkNotExecuted already wrote its marker synchronously.
            } else {
                m_action_polls.MarkObserved(key);
                // A non-certifying observation changes only the receipt.
                SaveRestartState();
            }
        } catch (const std::exception& e) {
            timing.Field("error", std::string{e.what()});
            // Nothing applied and no verified label: nothing changed, and the
            // caller's ordinary path runs next, in the same call.
            if (!applied && !verified_before) { label = "failed"; return std::nullopt; }
            // Either the reply was applied but the outbox write failed (the new
            // state is not durable), or the current authority check rejected a
            // label another read had verified. Exactly like the ordinary path:
            // record why, and never return (nor leave for the ordinary path to
            // return) a verified label that is unsaved or no longer current.
            // previously_certified stays set in memory, so this action is still
            // never resent.
            p.receipt.reason = e.what();
            if (p.previously_certified || p.receipt.certificate_verified || p.not_executed) WithdrawVerifiedLabels(p.receipt);
            label = applied ? "save_failed" : "authority_failed";
            return p.receipt;
        }
        label = p.receipt.certificate_verified ? "certified" : "observed";
        return p.receipt;
    }
    static bool Settled(const Pending& p) { return p.receipt.certificate_verified || ProvenNotExecuted(p); }
    //! A new exact signed action's submission on the wait lane: its first
    //! delivery, with a bounded wait at the endpoint for its inclusion.
    struct LaneSubmission {
        std::pair<uint256, uint256> key;
        HttpsEndpoint target;
        size_t endpoint{0};
        std::chrono::milliseconds wait{0};
        bool with_outcome{false};
        bool borrowed{false};
        std::string body;
    };
    //! The answer of a lane submission, and whether the endpoint did not wait
    //! (so one waited status read may still follow in the same call).
    struct LaneSubmissionResult {
        Receipt receipt;
        bool chain{false};
    };
    /** Under m_work while holding the wait lane, for an exact signed action
     * this Submit call just retained (its row Saved) and never delivered:
     * when the ordinary submission endpoint offers a waited submission and
     * one automatic status attempt is available, makes the delivery record
     * durable before any byte can leave, exactly as Send's write-ahead does,
     * carries a warm ordinary connection if the lane's is cold, and marks the
     * row in flight. nullopt, having changed nothing, when Send must deliver
     * it instead. It throws only up to its write-ahead Save (the row is then
     * simply retained, as when Send throws there), never after it. */
    std::optional<LaneSubmission> PrepareLaneSubmission(Pending& p, const std::chrono::milliseconds requested)
    {
        if (m_endpoints.empty() || p.lane_in_flight || p.previously_certified || p.not_executed) return std::nullopt;
        const size_t endpoint{m_selected};
        const auto now{std::chrono::steady_clock::now()};
        const auto wait{FlowMeshSubmitWaitFor({.requested = requested, .new_instruction = !p.may_have_been_sent,
            .advertised = m_submit_wait.at(endpoint), .endpoint_cooling = now < m_retry_after.at(endpoint),
            .endpoint_unanswered = m_wait_unanswered.at(endpoint)})};
        if (wait.count() == 0) return std::nullopt;
        // Send's local authority check, before any network use.
        const auto pins{Pins(p.market)};
        if (pins.domain != p.domain || pins.execution_config_id != p.config) Fail("Cannot retry retained action under different domain/config");
        // Its optional embedded status read must yield to ordinary queued
        // demand, just like WaitedActionStatus. Send still delivers the exact
        // authorized action through the ordinary path when the wait yields.
        if (!m_action_polls.TryChargeUnqueuedAttempt(now)) return std::nullopt;
        LaneSubmission s;
        s.key = {p.market, p.action.Id()};
        s.target = m_endpoints.at(endpoint);
        s.endpoint = endpoint;
        s.wait = wait;
        s.with_outcome = m_action_outcome.at(endpoint);
        UniValue params{UniValue::VOBJ};
        params.pushKV("market_id", p.market.GetHex());
        params.pushKV("action_id", p.action.Id().GetHex());
        params.pushKV("action_hex", HexStr(p.bytes));
        params.pushKV("wait_ms", uint64_t(wait.count()));
        // Only an endpoint that advertised verifiable outcomes receives "outcome".
        if (s.with_outcome) params.pushKV("outcome", true);
        UniValue request{UniValue::VOBJ};
        request.pushKV("method", "submit");
        request.pushKV("params", std::move(params));
        s.body = request.write();
        // Write-ahead uncertainty, exactly Send's, covers a crash immediately
        // after the first socket write. No signature is generated here.
        p.may_have_been_sent = true; p.receipt.state = "unknown"; Save();
        // Nothing below throws: an in-flight mark is never left behind here.
        s.borrowed = BorrowForLane(s.target);
        p.lane_in_flight = true;
        return s;
    }
    /** The network half of a lane submission, then its application under
     * m_work. The flight is released under m_work on every path, including
     * an unwind: the row is published again, a carried connection handed
     * back and the lane unlocked. Only the retained exact bytes are ever
     * resent, and only where Send would send them: without wait_ms to the
     * same endpoint after it refused the field, or to the other endpoints
     * after it did not answer usably. A reply that cannot be used leaves the
     * action unknown, never rejected; a verified or proven receipt (from a
     * concurrent read) is never replaced; an acknowledgement never downgrades
     * a queued or admitted observation. Worst case on a single silent
     * endpoint: wait + CLIENT_REQUEST_TIMEOUT, then unknown with no resend. */
    LaneSubmissionResult FinishLaneSubmission(const LaneSubmission& s, std::unique_lock<std::mutex>& lane)
    {
        FlowMeshTimingSpan timing{"client_submit_wait"};
        timing.Field("market_id", s.key.first);
        if (timing.Enabled()) timing.Field("action_id", s.key.second);
        timing.Field("wait_ms", uint64_t(s.wait.count()));
        timing.Field("endpoint", s.target.url);
        timing.Field("borrowed", uint64_t{s.borrowed});
        const auto outcome = [&](const char* result) { timing.Field("result", std::string{result}); };
        struct Flight {
            RemoteBackend& backend;
            const LaneSubmission& s;
            std::unique_lock<std::mutex>& lane;
            FlowMeshTimingSpan& timing;
            bool open{true};
            //! The caller holds m_work.
            void Release() noexcept
            {
                if (!open) return;
                open = false;
                if (const auto it{backend.m_pending.find(s.key)}; it != backend.m_pending.end()) it->second.lane_in_flight = false;
                bool handed_back{false};
                try {
                    handed_back = backend.ReturnLaneBorrow(s.borrowed, s.target);
                } catch (...) {
                }
                timing.Field("handed_back", uint64_t{handed_back});
                if (lane.owns_lock()) lane.unlock();
            }
            ~Flight()
            {
                if (!open) return;
                try {
                    WorkLock lock{backend};
                    Release();
                } catch (...) {
                }
            }
        } flight{*this, s, lane, timing};

        timing.Mark("request_started_us");
        HttpsRequestResult reply;
        try {
            // The endpoint answers before its own wait ends; the transport
            // bound is the ordinary one on top of the wait.
            reply = m_wait_https.Request(s.target, "/flowmesh/v1", s.body, s.wait + CLIENT_REQUEST_TIMEOUT,
                                         CLIENT_SUBMIT_WAIT_MAX_REPLY);
        } catch (const std::exception& e) {
            // The exact bytes may have left: an unanswered request.
            reply = {};
            reply.request_may_have_been_sent = true;
            reply.error = e.what();
        }
        timing.Mark("request_completed_us");
        timing.Field("connection_reused", uint64_t{reply.connection_reused});
        timing.Field("tls_handshake_performed", uint64_t{reply.tls_handshake_performed});
        UniValue result;
        std::string error;
        bool unsupported{false};
        try {
            if (!reply.response_received) {
                Fail(reply.error.empty() ? "No HTTPS response to the waited submission; outcome unknown" : reply.error);
            }
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
        } catch (const std::exception& e) {
            error = e.what();
        }
        // A latency hint only; never evidence.
        const std::string wait_status{result.isObject() && result["wait_status"].isStr() ? result["wait_status"].get_str() : ""};
        if (!wait_status.empty()) timing.Field("wait_status", wait_status);

        // Test only: an unwind here must still release the flight (above).
        if (m_lane_submission_fault_for_test) m_lane_submission_fault_for_test();
#ifdef FLOWMESH_CLIENT_CRASH_TEST_HOOKS
        test::ClientCrashBeforeLaneApply(m_path, s.key.second);
#endif
        timing.Mark("apply_lock_requested_us");
        WorkLock lock{*this};
        timing.Mark("apply_lock_acquired_us");
        flight.Release();
        // Pinned while in flight: eviction skips it and nothing else erases.
        auto& p{m_pending.at(s.key)};
        if (unsupported) {
            // A downgraded endpoint (or a stale advertisement) refused the
            // field before admission. Withdrawn until it advertises again;
            // the exact bytes go by the ordinary path, where any refusal now
            // stays unknown because this delivery was possible.
            EndpointChanged();
            m_submit_wait.at(s.endpoint) = {};
            if (s.with_outcome) m_action_outcome.at(s.endpoint) = false;
            outcome("unsupported");
            return {Send(p), true};
        }
        std::optional<Receipt> acknowledged;
        if (error.empty()) {
            try {
                acknowledged = ParseReceipt(result, p.action.Id(), s.endpoint);
            } catch (const std::exception& e) {
                error = e.what();
            }
        }
        if (!acknowledged) {
            timing.Field("error", error);
            if (!reply.response_received) {
                // A lane request left unanswered: no transport cooldown, and
                // the lane skips this endpoint until it answers again.
                EndpointChanged();
                m_wait_unanswered.at(s.endpoint) = true;
            } else {
                EndpointResult(s.endpoint, true, error);
            }
            if (m_endpoints.size() > 1) {
                // Exactly the ordinary failover: the same bytes to the other
                // endpoints; a refusal there stays unknown.
                outcome(reply.response_received ? "refused_failover" : "unanswered_failover");
                return {Send(p, s.endpoint), true};
            }
            outcome(reply.response_received ? "refused" : "unanswered");
            if (!Settled(p)) {
                // As Send after a failed delivery: retained, possibly
                // delivered, never rejected on this evidence.
                if (p.receipt.state != "queued" && p.receipt.state != "admitted") p.receipt.state = "unknown";
                p.receipt.reason = error;
                SaveRestartState();
            }
            return {p.receipt, false};
        }
        auto receipt{std::move(*acknowledged)};
        if (receipt.state == "certified_inclusion") {
            // A submission acknowledgement itself carries no inclusion proof.
            receipt.state = "unknown"; receipt.reason = "Endpoint reports inclusion; canonical certificate must still be retrieved";
        }
        EndpointResult(s.endpoint, true, {}, /*names_active=*/s.endpoint == m_selected);
        p.delivery_endpoint = s.endpoint;
        if (!Settled(p)) {
            if ((p.receipt.state == "queued" || p.receipt.state == "admitted") &&
                (receipt.state == "rejected" || receipt.state == "unknown")) {
                // A concurrent read already observed this action queued or
                // admitted; one acknowledgement never undoes that.
                p.receipt.reason = "No verified inclusion yet; endpoint observation: " + receipt.state + ": " + receipt.reason;
            } else {
                // Otherwise this first and only delivery's answer, applied as
                // Send applies it: a refusal of it is definite.
                p.receipt = std::move(receipt);
                if (!p.owner_account.IsNull()) p.receipt.account_id = p.owner_account;
            }
        }
        // The endpoint did not wait (no slot, deadline, budget or failure):
        // one waited read of the acknowledged delivery may still follow.
        const bool not_waited{wait_status == "busy" || wait_status == "deadline" || wait_status == "budget" ||
                              wait_status == "failed"};
        if (result["action"].isObject()) {
            m_action_polls.RecordExtraCost(std::chrono::steady_clock::now(), OutcomeExtraCost(result["action"]));
            std::string label;
            if (auto applied{ApplyLaneResult(p, result["action"], s.endpoint, timing, label)}) {
                outcome(label.c_str());
                return {std::move(*applied), not_waited};
            }
            timing.Field("action_error", label);
        }
        // Only the receipt changed: the delivery record is already durable.
        SaveRestartState();
        outcome("acknowledged");
        return {p.receipt, not_waited};
    }
    // One waited 'action' read of an action this process delivered, sent on
    // the lane connection to the endpoint that acknowledged the delivery and
    // only if that endpoint advertises the wait. m_work is held to select the
    // target and charge one automatic attempt, and again to apply the reply by
    // QueryAction's rules, never across the network wait. It never sends or
    // signs, and a receipt verified meanwhile is returned unchanged. nullopt
    // means nothing was applied; ActionStatus then takes the ordinary path,
    // while Submit's chained read just returns its submission receipt. A
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
    //
    // Transport: the lane's own connection goes cold after 25 s without a
    // waited read, which is every trade of a sporadic trader, while m_https
    // to the same endpoint is usually warm (the preflight, the submission
    // and passive refreshes just used it). Then the request carries that
    // verified connection instead of a new TCP + TLS setup, and it is handed
    // back when the reply has been applied. Transport only: the same bytes
    // are sent and Request re-checks origin, pin and CA content first.
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
        bool with_outcome{false};
        bool borrowed{false};
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
            // Do not let this opportunistic lane bypass queued status work.
            // From ActionStatus, refusal falls through to the ordinary FIFO
            // path in the same call, which still returns only this action's
            // receipt. Submit's chained read has no such fallback.
            if (!m_action_polls.TryChargeUnqueuedAttempt(std::chrono::steady_clock::now())) { outcome("coalesced"); return std::nullopt; }
            target = m_endpoints.at(endpoint);
            with_outcome = m_action_outcome.at(endpoint);
            borrowed = BorrowForLane(target);
        }
        timing.Field("wait_ms", uint64_t(wait.count()));
        timing.Field("endpoint", target.url);
        timing.Field("borrowed", uint64_t{borrowed});
        UniValue params{UniValue::VOBJ};
        params.pushKV("market_id", market.GetHex()); params.pushKV("action_id", action.GetHex());
        params.pushKV("wait_ms", uint64_t(wait.count()));
        if (with_outcome) params.pushKV("outcome", true);
        UniValue request{UniValue::VOBJ}; request.pushKV("method", "action"); request.pushKV("params", std::move(params));
        timing.Mark("request_started_us");
        // The endpoint answers before its own wait ends; the transport bound
        // is the ordinary one on top of the wait.
        const auto reply{m_wait_https.Request(target, "/flowmesh/v1", request.write(), wait + CLIENT_REQUEST_TIMEOUT,
                                              with_outcome ? CLIENT_ACTION_OUTCOME_MAX_REPLY : CLIENT_ACTION_WAIT_MAX_REPLY)};
        timing.Mark("request_completed_us");
        // The lane stays held until a carried connection is handed back.
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
        timing.Field("handed_back", uint64_t{ReturnLaneBorrow(borrowed, target)});
        lane.unlock();
        if (!reply.response_received) { EndpointChanged(); m_wait_unanswered.at(endpoint) = true; }
        m_action_polls.RecordExtraCost(std::chrono::steady_clock::now(), OutcomeExtraCost(result));
        if (unsupported) {
            // The endpoint no longer offers the wait (for example after a
            // downgrade). Nothing was applied or resent; wait_ms (and
            // "outcome") are sent there again only after it advertises the
            // capabilities again.
            EndpointChanged();
            m_action_wait.at(endpoint) = {};
            m_action_outcome.at(endpoint) = false;
            outcome("unsupported");
            return std::nullopt;
        }
        if (!error.empty()) {
            outcome("failed"); timing.Field("error", error);
            return std::nullopt;
        }
        const auto it{m_pending.find(key)};
        if (it == m_pending.end()) { outcome("not_retained"); return std::nullopt; }
        std::string label;
        auto applied{ApplyLaneResult(it->second, result, endpoint, timing, label)};
        outcome(label.c_str());
        return applied;
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
        m_submit_wait.resize(m_endpoints.size());
        m_action_outcome.resize(m_endpoints.size());
        m_wait_unanswered.resize(m_endpoints.size());
        m_updates_watch.resize(m_endpoints.size());
        m_watch_unanswered.resize(m_endpoints.size());
        m_clearing_ticks.resize(m_endpoints.size());
        m_clearing_ticks_withdrawn.resize(m_endpoints.size());
        m_observed_time.resize(m_endpoints.size());
        m_observed_candles.resize(m_endpoints.size());
        for (const auto& endpoint : m_endpoints) m_status.endpoints.push_back({.url = endpoint.url, .available = false, .last_error = {}});
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
                m_submit_wait.emplace_back();
                m_action_outcome.emplace_back(false);
                m_wait_unanswered.push_back(false);
                m_updates_watch.emplace_back();
                m_watch_unanswered.push_back(false);
                m_clearing_ticks.push_back(0);
                m_clearing_ticks_withdrawn.emplace_back();
                m_observed_time.push_back(false);
                m_observed_candles.push_back(0);
            }
            ResetReadTrust();
            EndpointChanged();
            m_selected = selected; m_preferred = selected; m_retry_after[selected] = {};
            m_https.Reset(); // Explicit connect revalidates existing trust.
            m_wait_reset = true;
            m_watch_reset = true;
            m_display_reset = true;
            {
                std::lock_guard status_lock{m_status_mutex};
                if (added) m_status.endpoints.push_back({.url = endpoint.url, .available = false, .last_error = {}});
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
    //! See SetFlowMeshLaneSubmissionFaultForTest. Set before the calls it covers.
    void SetLaneSubmissionFaultForTest(std::function<void()> fault) { m_lane_submission_fault_for_test = std::move(fault); }
private:
    //! Test only (never set by the node): called by a lane submission after
    //! its request, before it takes m_work to apply the reply.
    std::function<void()> m_lane_submission_fault_for_test;
    UniValue ReadMarkets()
    {
        UniValue params{UniValue::VOBJ};
        return Call("markets", params, [&](const UniValue& value, size_t endpoint) {
            if (!value.isArray() || value.size() > CLIENT_MAX_MARKETS) Fail("Market discovery exceeds bound");
            std::set<uint256> seen;
            // One row per endpoint is shown: the first market it reports not
            // ready, otherwise the last one.
            std::optional<std::pair<FlowMeshAdvertisedReadiness, uint256>> shown;
            for (const auto& row : value.getValues()) {
                const auto id{Id(row, "market_id")}; if (!seen.insert(id).second) Fail("Duplicate market identity");
                CheckStatus(row, Pins(id));
                LearnActionWait(row, endpoint);
                if (!shown || shown->first.state != FlowMeshAdvertisedReadiness::State::NOT_READY) {
                    shown.emplace(FlowMeshAdvertisedReadinessOf(row), id);
                }
            }
            if (shown) LearnReadiness(std::move(shown->first), endpoint, shown->second);
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
        const auto request{DirectReadRequest(account, std::chrono::steady_clock::now())};
        FlowMeshTimingSpan timing_lock_1215{__func__};
        timing_lock_1215.Field("lock_name", std::string{"client_work"});
        timing_lock_1215.Mark("lock_requested_us");
        MarketStatus status; status.market_id = id; status.remote = true;
        try {
            const auto project = [&](Cache& cache) {
            const auto data{Project(cache, account, {})};
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
            };
            bool joined{false};
            if (FlowMeshSigningPreflightScope::Active()) {
                // A current no-network join needs no reservation. Admission
                // FIFO applies only to callers that need an ordinary owner.
                WorkLock lock{*this, std::try_to_lock};
                if (lock.owns_lock()) {
                    MarkWorkAcquired(timing_lock_1215);
                    if (auto* cache{JoinFresh(id, account)}) { project(*cache); joined = true; }
                }
            }
            if (FlowMeshSigningPreflightScope::Active()) timing_lock_1215.Field("preflight", std::string{joined ? "joined" : "refreshed"});
            if (!joined) RefreshRead(id, account, {}, request, project, FlowMeshSigningPreflightScope::Active());
        } catch (const std::exception& e) { status.error = e.what(); status.paused = true; status.running = false; }
        return status;
    }
private:
    std::optional<flowmesh::MarketData> DataImpl(const uint256& id, const std::optional<uint256>& account,
                                              const flowmesh::MarketDataQuery& query, std::string& error,
                                              const ReadRequest& request)
    {
        FlowMeshTimingSpan timing_lock_1245{"Data"}; // Preserve the public read's diagnostic label.
        timing_lock_1245.Field("read_lane", std::string{"ordinary"});
        std::optional<flowmesh::MarketData> out;
        try {
            RefreshRead(id, account, query, request, [&](Cache& cache) {
                out = Project(cache, account, query);
                if (query.indicative || flowmesh::LevelsWanted(*out, query)) out->book = CachedBook(cache, timing_lock_1245);
            });
        } catch (const std::exception& e) {
            error = e.what();
            return std::nullopt;
        }
        // Display views of the verified curves: after the work gate is
        // released, so no signing preflight or action waits for them.
        flowmesh::ComputeMarketViews(*out, query);
        return out;
    }
public:
    std::optional<flowmesh::MarketData> Data(const uint256& id, const std::optional<uint256>& account,
                                          const flowmesh::MarketDataQuery& query, std::string& error) override
    {
        const auto request{DirectReadRequest(account, std::chrono::steady_clock::now())};
        return DataImpl(id, account, query, error, request);
    }
    /** The endpoint serving this market's data, asked for one page of its
     * observed-time candles, only when it advertises them. Endpoint-reported
     * display data: only the page's structure is checked (a malformed page
     * is refused), and the page is bounded to a head this client verified:
     * candles ending past it are held back, never refused (below). Display
     * work: the work gate is taken only briefly and passively, without I/O,
     * to read the endpoint and the verified head before the request and to
     * withdraw a stale candles advertisement after it, never across the
     * request (it has its own lane and connection; a busy lane fails at
     * once). Nothing here refreshes the head, verifies certificates or
     * writes durable state. So an order never waits for a candles page
     * beyond those reads.
     *
     * The endpoint's forming candle ends at its own head, which on an active
     * market is nearly always past the head this client verified before the
     * request. That head bounds the page: candles ending past it are held
     * back and before_ms then ends before them. The first held-back bucket
     * may be this client's previous view of it (the newest candle of the
     * latest page from that endpoint, held per market and interval) once that
     * view ends within the head; Qt continues it with the held ticks. The
     * next market-data read moves the head. */
    std::optional<flowmesh::ObservedCandlePage> Candles(const uint256& id, const flowmesh::CandleInterval interval,
                                                        const std::optional<int64_t> before_ms, const size_t limit,
                                                        std::string& error) override
    {
        FlowMeshTimingSpan timing{"client_candles"};
        // Display work, whoever asks: never ahead of queued trading work and
        // never reserving the gate after it (no linger window).
        const FlowMeshClientWorkScope display{FlowMeshClientWorkPriority::PASSIVE, FlowMeshClientWorkLinger::NONE};
        std::unique_lock lane{m_display_lane, std::try_to_lock};
        if (!lane.owns_lock()) {
            error = "A candles read is already in progress";
            return std::nullopt;
        }
        HttpsEndpoint target;
        size_t endpoint{0}, advertised{0};
        uint64_t head_sequence{0};
        {
            WorkLock lock{*this};
            const auto it{m_cache.find(id)};
            if (it == m_cache.end()) {
                error = "Read this market's data before its candles";
                return std::nullopt;
            }
            endpoint = it->second.endpoint;
            head_sequence = it->second.verified.certified.entry.sequence;
            advertised = m_observed_candles.at(endpoint);
            target = m_endpoints.at(endpoint);
        }
        flowmesh::ObservedCandlePage out;
        out.interval = interval;
        out.remote = true;
        out.source = target.url;
        if (advertised == 0) {
            out.unavailable_reason = "endpoint_does_not_advertise_candles";
            return out;
        }
        UniValue params{UniValue::VOBJ};
        params.pushKV("market_id", id.GetHex());
        params.pushKV("interval", flowmesh::CandleIntervalName(interval));
        // Never a window past the display bound: an endpoint echoing one
        // back would be refused.
        if (before_ms) params.pushKV("before_ms", std::min(*before_ms, flowmesh::OBSERVED_MS_LIMIT));
        params.pushKV("limit", uint64_t{std::clamp<size_t>(limit, 1, advertised)});
        UniValue request{UniValue::VOBJ};
        request.pushKV("method", "candles");
        request.pushKV("params", std::move(params));
        if (m_display_reset.exchange(false)) {
            m_display_https.Reset();
            m_held_candles.clear();
        }
        const auto reply{m_display_https.Request(target, "/flowmesh/v1", request.write(), CLIENT_REQUEST_TIMEOUT, CLIENT_CANDLES_MAX_REPLY)};
        try {
            if (!reply.response_received) Fail(reply.error.empty() ? "No HTTPS response" : reply.error);
            UniValue parsed;
            if (!parsed.read(reply.body)) Fail("Malformed endpoint JSON response");
            Keys(parsed, {"ok", "result", "error"});
            if (!Flag(parsed, "ok")) {
                const auto text{Text(parsed, "error")};
                if (text == "This method is not exposed by the public trading API") {
                    // A stale advertisement: withdrawn until relearned.
                    WorkLock lock{*this};
                    EndpointChanged();
                    m_observed_candles.at(endpoint) = 0;
                }
                Fail(text);
            }
            if (reply.status != 200) Fail("Unexpected HTTPS response status");
            // The structure first; the head bounds it below.
            auto page{FlowMeshParseObservedCandles(parsed["result"], interval, std::numeric_limits<uint64_t>::max(), error)};
            if (!page) {
                m_display_https.Reset();
                return std::nullopt;
            }
            page->remote = true;
            page->source = target.url;
            const std::optional<flowmesh::ObservedCandle> served_newest{
                page->candles.empty() ? std::nullopt : std::optional{page->candles.back()}};
            const std::pair key{id, interval};
            const auto stored{m_held_candles.find(key)};
            const bool same_endpoint{stored != m_held_candles.end() && stored->second.endpoint == endpoint};
            const auto hold{FlowMeshHoldBackObservedCandles(*page, head_sequence,
                                                            same_endpoint ? std::optional{stored->second.candle} : std::nullopt)};
            if (hold.held_back > 0) {
                timing.Field("held_back", uint64_t{hold.held_back});
                timing.Field("substituted", uint64_t{hold.substituted});
            }
            // The page's own newest view, as served: an older page (with
            // before_ms) never replaces a newer one of the same endpoint.
            if (served_newest && (!same_endpoint || !before_ms || served_newest->open_ms >= stored->second.candle.open_ms)) {
                if (stored == m_held_candles.end() && m_held_candles.size() >= CLIENT_MAX_HELD_CANDLES) m_held_candles.erase(m_held_candles.begin());
                m_held_candles.insert_or_assign(key, HeldCandle{endpoint, *served_newest});
            }
            return page;
        } catch (const std::exception& e) {
            m_display_https.Reset();
            error = e.what();
            return std::nullopt;
        }
    }
    std::optional<flowmesh::MarketData> Watch(const uint256& id, const std::optional<uint256>& account,
                                             const flowmesh::MarketDataQuery& query, const std::chrono::milliseconds requested,
                                             interfaces::FlowMeshMarketWatch& watch, std::string& error) override
    {
        FlowMeshTimingSpan timing{"client_market_watch"};
        timing.Field("market_id", id);
        watch = {};
        const auto label = [&](const std::string_view status) {
            watch.status = std::string{status};
            timing.Field("watch_status", watch.status);
        };
        // The same verified ordinary data, but Watch explicitly remains
        // immediate even for an authenticated foreground caller.
        const auto ordinary = [&](const std::string_view status) {
            label(status);
            return DataImpl(id, account, query, error,
                {FlowMeshReadAdmission::IMMEDIATE, std::chrono::steady_clock::now()});
        };
        std::unique_lock lane{m_watch_lane, std::try_to_lock};
        if (!lane.owns_lock()) return ordinary("lane_busy");
        HttpsEndpoint target;
        size_t endpoint{0};
        std::chrono::milliseconds wait{0};
        bool ticks_catchup{false};
        flowmesh::ClientEventCursor sent_cursor;
        ReadContext sent_context;
        uint64_t sent_observations{0};
        std::string body;
        timing.Mark("select_lock_requested_us");
        {
            // Phase 1, under m_work without network: the cache's endpoint and
            // position, and whether a watch may be sent there at all.
            WorkLock lock{*this};
            timing.Mark("select_lock_acquired_us");
            const auto it{m_cache.find(id)};
            if (it != m_cache.end()) {
                auto& cache{it->second};
                endpoint = cache.endpoint;
                watch.endpoint = m_endpoints.at(endpoint).url;
                watch.endpoint_live_ms = static_cast<uint32_t>(m_updates_watch.at(endpoint).count());
                bool current{false};
                try {
                    current = CheckCache(id, cache, account) == CacheCheck::CURRENT;
                } catch (const std::exception&) {
                }
                // As Refresh: the cached reported data belongs to this account.
                const bool same_reported_account{!account || (cache.reported["account"].isObject() &&
                    cache.reported["account"]["account_id"].isStr() && cache.reported["account"]["account_id"].get_str() == account->GetHex())};
                wait = FlowMeshUpdatesWatchFor({.requested = requested, .advertised = m_updates_watch.at(endpoint),
                    .lane_overhead = m_watch_overhead, .cache_current = current, .watch_ready = cache.watch_ready,
                    .scope_matches = cache.account_scope == account && same_reported_account,
                    .cooldown = std::chrono::steady_clock::now() < m_retry_after.at(endpoint),
                    .unanswered = m_watch_unanswered.at(endpoint),
                    .paging = query.before_sequence || query.curve_cursor || query.expected_head});
                // Clearing ticks the ring lacks (a signing preflight's snapshot
                // never asks for them) come with an ordinary read instead: a
                // watched page carries none. At most once per head.
                if (wait.count() && query.ticks && m_clearing_ticks.at(endpoint) > 0) {
                    auto& ring{m_ticks[id]};
                    const auto& head{cache.verified.certified.entry};
                    if ((ring.ticks.empty() || ring.ticks.back().sequence < head.sequence) && ring.catchup_head != head.GetHash()) {
                        ring.catchup_head = head.GetHash();
                        wait = {};
                        ticks_catchup = true;
                    }
                }
                if (wait.count()) {
                    UniValue params{UniValue::VOBJ};
                    params.pushKV("market_id", id.GetHex());
                    if (account) params.pushKV("account_id", account->GetHex());
                    params.pushKV("cursor", CursorJson(cache.cursor));
                    params.pushKV("known_head", cache.verified.certified.entry.GetHash().GetHex());
                    params.pushKV("watch_ms", uint64_t(wait.count()));
                    UniValue request{UniValue::VOBJ};
                    request.pushKV("method", "updates");
                    request.pushKV("params", std::move(params));
                    body = request.write();
                    sent_cursor = cache.cursor;
                    target = m_endpoints.at(endpoint);
                    sent_context = CaptureReadContext(id, account);
                    sent_observations = m_endpoint_revision;
                }
            }
        }
        if (ticks_catchup) {
            // The watch was eligible: this read only fetches the ticks. The
            // lane keeps its connection for the watch that follows at once
            // (the next one at this head is sent).
            lane.unlock();
            auto out{ordinary("ticks_catchup")};
            watch.waited = true;
            return out;
        }
        if (!wait.count()) {
            // Not watching now: the lane's kept-alive connection would only
            // count against the endpoint's connection limit.
            m_watch_https.Reset();
            lane.unlock();
            return ordinary("not_eligible");
        }
        timing.Field("wait_ms", uint64_t(wait.count()));
        timing.Field("endpoint", target.url);
        if (m_watch_reset.exchange(false)) m_watch_https.Reset();
        // Phase 2, lane only: a read-only request, never an action.
        const auto sent{std::chrono::steady_clock::now()};
        timing.Mark("request_started_us");
        const auto reply{m_watch_https.Request(target, "/flowmesh/v1", body, wait + CLIENT_REQUEST_TIMEOUT,
                                               FLOWMESH_CLIENT_WATCH_MAX_REPLY)};
        const auto received{std::chrono::steady_clock::now()};
        timing.Mark("request_completed_us");
        timing.Field("connection_reused", uint64_t{reply.connection_reused});
        UniValue result;
        std::string failure;
        bool unsupported{false};
        std::string_view server_status;
        try {
            if (!reply.response_received) Fail(reply.error.empty() ? "No HTTPS response" : reply.error);
            std::string trust_error;
            const auto remaining{std::chrono::duration_cast<std::chrono::milliseconds>(sent + wait + CLIENT_REQUEST_TIMEOUT - std::chrono::steady_clock::now())};
            if (!FlowMeshHttpsTrustCurrent(target, reply.trust, remaining, trust_error)) Fail(trust_error);
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
            if (!result.isObject() || !result["watch_status"].isStr()) Fail("Watched page has no watch status");
            server_status = FlowMeshWatchStatusLabel(result["watch_status"].get_str());
        } catch (const std::exception& e) {
            failure = e.what();
        }
        // Release the endpoint's connection after an outcome that ends this
        // lane's use for a while.
        if (!failure.empty() || FlowMeshWatchReleasesLane(server_status)) m_watch_https.Reset();
        lane.unlock();

        std::optional<flowmesh::MarketData> applied;
        std::optional<SnapshotContinuation> continuation;
        uint64_t snapshot_gaps{0};
        std::string_view fallback;
        {
            // Phase 3, under m_work: the same checks and state as a Refresh.
            timing.Mark("apply_lock_requested_us");
            WorkLock lock{*this};
            timing.Mark("apply_lock_acquired_us");
            // No transport cooldown, as for the wait lane; only a successful
            // response from this endpoint clears 'unanswered'.
            if (sent_context.epoch != m_read_epoch) {
                error = "Watched response trust/configuration was superseded";
                label("superseded");
                return std::nullopt;
            }
            if ((!failure.empty() || unsupported) && sent_observations != m_endpoint_revision) {
                error = "Watched endpoint observation was superseded";
                label("superseded");
                return std::nullopt;
            }
            if (!reply.response_received) { EndpointChanged(); m_watch_unanswered.at(endpoint) = true; }
            if (unsupported) {
                // Downgraded, or a stale advertisement: withdrawn until the
                // endpoint advertises it again. The same call reads ordinarily.
                LearnUpdatesWatch({}, endpoint);
                fallback = "unsupported";
            } else if (!failure.empty()) {
                // Never an automatic failover walk: the caller's ordinary
                // reads own endpoint failover.
                label("failed");
                timing.Field("error", failure);
                error = failure;
                return std::nullopt;
            } else {
                const auto it{m_cache.find(id)};
                if (!SameReadContext(sent_context) || it == m_cache.end() || it->second.cursor.instance_id != sent_cursor.instance_id ||
                    it->second.cursor.event_id != sent_cursor.event_id || it->second.account_scope != account) {
                    // Another read moved this cache meanwhile: an older page
                    // is never applied on top.
                    label("superseded");
                    error = "Watched response was superseded; retained data is not a fresh response";
                    return std::nullopt;
                } else {
                    auto& cache{it->second};
                    bool current{false};
                    try {
                        current = CheckCache(id, cache, account) == CacheCheck::CURRENT;
                    } catch (const std::exception&) {
                    }
                    if (!current) {
                        // Local B3 authority moved while the watch slept: the
                        // next ordinary scheduled read can re-verify. This
                        // completed watch never starts another network walk.
                        label("revalidate");
                        error = "Local B3 authority changed during watch; a new verified read is required";
                        return std::nullopt;
                    } else {
                        try {
                            const auto event_gaps = [&] {
                                std::lock_guard status{m_status_mutex};
                                return m_status.event_gaps;
                            };
                            const auto gaps{event_gaps()};
                            auto ticks{TicksAfterRing(id)};
                            ticks.observed = query.observed_time;
                            auto updates{ParseUpdatesPage(cache, id, result)};
                            if (server_status == "timeout" && sent_observations == m_endpoint_revision) {
                                const auto outside{std::chrono::duration_cast<std::chrono::milliseconds>(received - sent) - wait};
                                m_watch_overhead = (3 * m_watch_overhead + std::max(outside, std::chrono::milliseconds{0})) / 4;
                            }
                            if (updates.snapshot_needed) {
                                // Leave both cursor and capabilities untouched;
                                // the ordinary lane fetches/verifies/saves the
                                // new head after this work scope ends.
                                continuation = SnapshotContinuation{CaptureReadContext(id, account), updates.cursor, updates.gap, sent};
                                snapshot_gaps = gaps;
                            } else {
                            const bool learn_endpoint{sent_observations == m_endpoint_revision};
                            InstallUpdatesPage(cache, id, std::move(updates), endpoint, learn_endpoint);
                            if (query.ticks) MergeTicks(id, cache, ticks);
                            // Observed from the send: never more permissive
                            // than a Refresh started at the same moment.
                            cache.fresh = std::pair{sent, std::chrono::steady_clock::now()};
                            if (learn_endpoint) EndpointResult(endpoint, true, {}, /*names_active=*/false);
                            watch.supported = true;
                            watch.waited = FlowMeshWatchWaited(server_status);
                            watch.gap = event_gaps() > gaps;
                            watch.endpoint_live_ms = static_cast<uint32_t>(m_updates_watch.at(endpoint).count());
                            label(server_status);
                            applied = Project(cache, account, query);
                            if (query.indicative || flowmesh::LevelsWanted(*applied, query)) applied->book = CachedBook(cache, timing);
                            }
                        } catch (const std::exception& e) {
                            label("invalid");
                            timing.Field("error", std::string{e.what()});
                            error = e.what();
                            m_watch_reset = true;
                            return std::nullopt;
                        }
                    }
                }
            }
        }
        if (continuation) {
            try {
                RefreshRead(id, account, query, {FlowMeshReadAdmission::IMMEDIATE, continuation->started}, [&](Cache& cache) {
                    watch.supported = true;
                    watch.waited = FlowMeshWatchWaited(server_status);
                    { std::lock_guard status{m_status_mutex}; watch.gap = m_status.event_gaps > snapshot_gaps; }
                    watch.endpoint_live_ms = static_cast<uint32_t>(m_updates_watch.at(endpoint).count());
                    label(server_status);
                    applied = Project(cache, account, query);
                    if (query.indicative || flowmesh::LevelsWanted(*applied, query)) applied->book = CachedBook(cache, timing);
                }, false, continuation);
            } catch (const std::exception& e) {
                label("invalid");
                error = e.what();
                m_watch_reset = true;
                return std::nullopt;
            }
        }
        if (applied) {
            // As Data(): outside the work gate.
            flowmesh::ComputeMarketViews(*applied, query);
            return applied;
        }
        return ordinary(fallback);
    }
    Receipt Submit(const uint256& market, const flowmesh::Action& action,
                   std::chrono::milliseconds wait = std::chrono::milliseconds{0}) override
    {
        FlowMeshTimingSpan timing_lock_1251{__func__};
        timing_lock_1251.Field("market_id", market);
        if (timing_lock_1251.Enabled()) timing_lock_1251.Field("action_id", action.Id());
        timing_lock_1251.Field("lock_name", std::string{"client_work"});
        timing_lock_1251.Mark("lock_requested_us");
        // A waited submission of a new action may travel on the wait lane,
        // taken before m_work exactly as a waited read takes it: only ever
        // try-locked, never while holding m_work. A busy lane means the
        // ordinary submission.
        std::unique_lock lane{m_wait_lane, std::defer_lock};
        if (wait.count() > 0) (void)lane.try_lock();
        std::optional<LaneSubmission> flight;
        Receipt out{SubmitOrdinary(market, action, timing_lock_1251, lane.owns_lock() ? wait : std::chrono::milliseconds{0}, flight)};
        bool chain{wait.count() > 0};
        if (!flight) {
            if (lane.owns_lock()) lane.unlock();
        } else {
            try {
                auto finished{FinishLaneSubmission(*flight, lane)};
                out = std::move(finished.receipt);
                chain = chain && finished.chain;
            } catch (const std::exception& e) {
                // The flight was released; the exact retained action may have
                // been delivered, so its outcome is unknown, never refused.
                out = {};
                out.action_id = action.Id();
                out.state = "unknown";
                out.reason = e.what();
                chain = false;
            }
        }
        // At most one bounded waited read of the action just delivered: what
        // a caller's getflowmeshactionstatus(wait_ms) right after this call
        // would send, without that caller's turnaround. It never resends.
        // Unlike ActionStatus, there is no ordinary-path fallback here: when
        // the read yields (queued status demand, a busy lane, cooldown, no
        // advertised wait) or applies nothing, the submission receipt above
        // is returned as it is, no status demand is queued and no queued
        // demand is served. Its status is left to the caller's next
        // ActionStatus read.
        if (chain && out.state != "rejected" && !out.certificate_verified) {
            if (auto waited{WaitedActionStatus(market, action.Id(), wait)}) return std::move(*waited);
        }
        return out;
    }
private:
    //! Retains a new exact signed action (Save before its first send) and
    //! delivers it, or redelivers a retained one, by Send; all under m_work.
    //! With a nonzero lane_wait (the caller holds the wait lane), a new
    //! action whose endpoint offers a waited submission is instead prepared
    //! for it into `flight`, which the caller must finish.
    Receipt SubmitOrdinary(const uint256& market, const flowmesh::Action& action, FlowMeshTimingSpan& timing_lock_1251,
                           const std::chrono::milliseconds lane_wait, std::optional<LaneSubmission>& flight)
    {
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
                // Evictable: a verified inclusion, or an action proven never to
                // execute (its marker needs no row once nothing can resend it),
                // but never one whose waited submission is still in flight.
                const auto completed{std::find_if(m_pending.begin(), m_pending.end(), [](const auto& value) {
                    return !value.second.lane_in_flight &&
                           (value.second.receipt.certificate_verified || value.second.not_executed.has_value());
                })};
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
            if (lane_wait.count() > 0 && (flight = PrepareLaneSubmission(it->second, lane_wait))) return it->second.receipt;
            return Send(it->second);
        } catch (const std::exception& e) {
            out.state = m_pending.contains({market, action.Id()}) ? "unknown" : "rejected"; out.reason = e.what(); return out;
        }
    }
public:
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
                UpgradeOutcome(it->second);
                SaveVerifiedRestartState(it->second);
                return it->second.receipt;
            }
            if (ProvenNotExecuted(it->second)) {
                m_action_polls.Remove(it->first);
                SaveVerifiedRestartState(it->second);
                return it->second.receipt;
            }
            auto refresh = [&](Pending& pending, bool automatic) {
                try { QueryAction(pending, automatic); } catch (const std::exception& e) {
                    pending.receipt.reason = e.what();
                    if (pending.previously_certified || pending.not_executed) WithdrawVerifiedLabels(pending.receipt);
                }
            };
            if (retry) {
                refresh(it->second, false);
                // A fresh status lookup already occurred. Do not repeat it in
                // Send's no-resubmit path for a previously certified or
                // not-executed action.
                auto& p{it->second};
                if (!p.receipt.certificate_verified && !p.previously_certified && !p.not_executed) return Send(p);
                if (!p.receipt.certificate_verified && p.previously_certified)
                    p.receipt.reason = "Previously verified inclusion is retained; fresh proof required for current authority, and this action will not be resubmitted";
                else if (!p.receipt.certificate_verified && p.not_executed && !ProvenNotExecuted(p))
                    p.receipt.reason = CLIENT_NOT_EXECUTED_RETAINED;
                return p.receipt;
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
                    if (work->second.receipt.certificate_verified || ProvenNotExecuted(work->second)) m_action_polls.Remove(*key);
                } else m_action_polls.Remove(*key);
            }
            auto receipt{it->second.receipt};
            const bool settled{receipt.certificate_verified || ProvenNotExecuted(it->second)};
            if (!settled && !m_action_polls.HasObservation(it->first))
                receipt.reason = "Status read pending/coalesced; retained submission observation only. " + receipt.reason;
            else if (!settled && !own_refresh)
                receipt.reason = "Status read coalesced; retained endpoint observation, not a fresh response. " + receipt.reason;
            return receipt;
        } catch (const std::exception& e) {
            if (it->second.previously_certified || it->second.not_executed) WithdrawVerifiedLabels(it->second.receipt);
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
            ResetReadTrust();
            m_https.Reset();
            m_wait_reset = true;
            m_watch_reset = true;
            m_display_reset = true;
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
bool SetFlowMeshLaneSubmissionFaultForTest(FlowMeshTradingBackend& backend, std::function<void()> fault)
{
    auto* const remote{dynamic_cast<RemoteBackend*>(&backend)};
    if (!remote) return false;
    remote->SetLaneSubmissionFaultForTest(std::move(fault));
    return true;
}
UniValue FlowMeshClientUpdatesPageJson(const flowmesh::ClientEventPage& page)
{
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
    return out;
}

std::optional<size_t> FlowMeshApiWatchSlots(const size_t action_slots, const bool watch_enabled,
                                            const std::optional<int64_t> requested, std::string& error)
{
    const size_t room{action_slots >= FLOWMESH_API_MAX_WAIT_SLOTS ? 0 : FLOWMESH_API_MAX_WAIT_SLOTS - action_slots};
    if (!requested) return std::min(FLOWMESH_API_UPDATES_WATCHERS_DEFAULT, room);
    if (*requested < 0 || *requested > int64_t(FLOWMESH_API_UPDATES_WATCHERS_MAX)) {
        error = strprintf("-flowmeshapiwatchers must be between 0 and %d", FLOWMESH_API_UPDATES_WATCHERS_MAX);
        return std::nullopt;
    }
    if (watch_enabled && size_t(*requested) > room) {
        error = strprintf("-flowmeshapiwatchers=%d with %d action-wait slots exceeds %d wait slots in total "
                          "(each takes a worker thread); lower -flowmeshapiwatchers or -flowmeshapiactionwaiters",
                          *requested, action_slots, FLOWMESH_API_MAX_WAIT_SLOTS);
        return std::nullopt;
    }
    return size_t(*requested);
}

std::unique_ptr<FlowMeshHttpsServer> MakeFlowMeshTradingApi(FlowMeshService& service, FlowMeshHttpsServer::Options options,
                                                        FlowMeshAssetMetadataCatalog metadata, const FlowMeshTradingApiWait wait,
                                                        const FlowMeshTradingApiHealth health, const FlowMeshTradingApiOutcome outcome,
                                                        const FlowMeshTradingApiWatch watch, const FlowMeshTradingApiTicks ticks,
                                                        const FlowMeshTradingApiObservedTime observed,
                                                        const FlowMeshTradingApiCandles candles)
{
    options.max_reply_bytes = CLIENT_MAX_REPLY;
    auto api{std::make_shared<TradingApi>(service, std::move(metadata), wait, health, outcome, watch, ticks, observed, candles)};
    // Handshake, read, handler and write concurrency stays at the two workers
    // the API always had. Each wait or watch slot adds a worker that only
    // sleeps in a suspended wait. Disabled waits and watches leave exactly
    // two workers and permits; the server's worker bound is never raised.
    options.active_permits = FLOWMESH_API_ACTIVE_PERMITS;
    options.worker_threads = FLOWMESH_API_ACTIVE_PERMITS + api->WaitSlots() + api->WatchSlots();
    Assert(options.worker_threads <= FLOWMESH_HTTPS_MAX_WORKER_THREADS);
    options.on_start = [api] { api->Reopen(); };
    options.on_stop = [api] { api->Interrupt(); };
    // An explicitly restricted health handler alone enables the separate
    // one-worker transport reserve (16 ordinary + 1 reserved maximum).
    FlowMeshHttpsServer::Handler restricted_health;
    if (health.enabled) restricted_health = [api](const auto& request) { return api->HandleReservedHealth(request); };
    return std::make_unique<FlowMeshHttpsServer>(std::move(options),
        [api](const auto& request) { return api->Handle(request); }, std::move(restricted_health));
}
} // namespace node
