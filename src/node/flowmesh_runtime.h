// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_RUNTIME_H
#define B3COIN_NODE_FLOWMESH_RUNTIME_H

#include <flowmesh/client_evidence.h>
#include <flowmesh/p2p.h>
#include <flowmesh/production_wire.h>
#include <node/flowmesh_delivery.h>
#include <node/flowmesh_agreement.h>
#include <node/flowmesh_health.h>
#include <node/flowmesh_local_admission.h>
#include <node/flowmesh_production_store.h>
#include <sync.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace node {

class FlowMeshTimingSpan;
class FlowMeshObservedTimeLog;

/** Immutable historical identity used to authorize a seat-reward claim. */
struct FlowMeshHistoricalRewardSeat {
    flowmesh::ActiveFnBlsSeatSet seats;
    flowmesh::ActiveFnBlsSeat member;
};

/** Canonical handoff core plus its exact B3 inclusion block. */
struct FlowMeshRuntimeConnectedHandoff {
    modern::FlowMeshCheckpointCoreV1 core;
    ProductionB3Connection connection;
};

enum class FlowMeshSeatTransitionKind : uint8_t {
    CONTINUE = 0,
    HANDOFF,
    PAUSED,
};

/**
 * Explicit newest-anchor decision. PAUSED is distinct from CONTINUE so an
 * observed next snapshot with fewer than four seats can never accidentally
 * authorize the outgoing committee to keep executing.
 */
struct FlowMeshSeatTransition {
    FlowMeshSeatTransitionKind kind{FlowMeshSeatTransitionKind::CONTINUE};
    std::optional<flowmesh::ActiveFnBlsSeatSet> next_seats;
    // Diagnostic only. Never contributes to seat membership or signed data.
    std::string pause_reason{};
};

/**
 * Chain/index dependency of the production runtime.
 *
 * Implementations take their snapshots under the chain/index locks and return
 * values. The runtime never holds cs_main while decoding, executing, signing,
 * aggregating, or writing its production store.
 */
class FlowMeshRuntimeChain : public flowmesh::AnchorPolicy
{
public:
    virtual int32_t TipHeight() const = 0;
    /** Local chain-reconciliation generation, never serialized or certified.
     * A change detects a gate that closed and reopened during one handler. */
    virtual uint64_t DeliveryGeneration() const { return 0; }

    /** Resolve one exact anchored epoch/set identity. */
    virtual std::optional<flowmesh::ActiveFnBlsSeatSet> SeatSet(
        const uint256& domain, const flowmesh::MarketId& market_id,
        uint64_t epoch, const uint256& seat_set_hash) const = 0;

    /** Resolve the set that certified one already-durable log sequence. */
    virtual std::optional<flowmesh::ActiveFnBlsSeatSet> SeatSetForSequence(
        const uint256& domain, const flowmesh::MarketId& market_id,
        uint64_t sequence) const = 0;

    /**
     * Return the next anchored set when an epoch handoff is required. No value
     * means the current epoch may continue. This is chain fact, not timeout
     * policy and not a caller-selected committee.
     */
    virtual FlowMeshSeatTransition SeatTransition(
        const uint256& domain, const flowmesh::MarketId& market_id,
        const flowmesh::ActiveFnBlsSeatSet& current) const
    {
        return {};
    }

    /**
     * Return an already-connected B3 handoff checkpoint for this exact
     * certified log entry. The runtime independently waits for the returned
     * inclusion block to reach the FlowMesh anchor depth before advancing the
     * production store. This lets bounded catch-up cross a mature handoff that
     * connected before the lagging node received the off-chain entry.
     */
    virtual std::optional<FlowMeshRuntimeConnectedHandoff>
    ConnectedHandoffCheckpoint(
        const uint256& domain, const flowmesh::MarketId& market_id,
        const uint256& microblock_hash) const
    {
        return std::nullopt;
    }

    /** Historical lookup for ActionType::CLAIM_SEAT_REWARD. */
    virtual std::optional<FlowMeshHistoricalRewardSeat> HistoricalRewardSeat(
        const uint256& domain, const flowmesh::MarketId& market_id,
        const flowmesh::AccountId& reward_account) const
    {
        return std::nullopt;
    }
};

/** Wallet/key-provider boundary. Raw keys never enter P2P or RPC objects. */
class FlowMeshRuntimeKeyProvider
{
public:
    virtual ~FlowMeshRuntimeKeyProvider() = default;
    virtual std::vector<bls::SecretKey> LocalSeatKeys(
        const flowmesh::MarketId& market_id,
        const flowmesh::ActiveFnBlsSeatSet& active_seats) const = 0;
};

/** Injectable monotonic clock; round timeout is local node policy. */
class FlowMeshRuntimeClock
{
public:
    virtual ~FlowMeshRuntimeClock() = default;
    virtual flowmesh::WireClock::time_point Now() const = 0;
};

class SteadyFlowMeshRuntimeClock final : public FlowMeshRuntimeClock
{
public:
    flowmesh::WireClock::time_point Now() const override
    {
        return flowmesh::WireClock::now();
    }
};

enum class FlowMeshRuntimeHalt : uint8_t {
    NONE = 0,
    INVALID_CONFIG,
    STORE_FAILURE,
    SIGNING_CONFLICT,
    CERTIFICATE_CONFLICT,
    ANCHOR_INVALIDATED,
};

enum class FlowMeshRuntimeMarketReadiness : uint8_t {
    READY = 0,
    INSUFFICIENT_SEATS,
};

const char* FlowMeshRuntimeHaltName(FlowMeshRuntimeHalt halt);

/** `peer` set means a direct reply; otherwise relay to subscribed peers. */
struct FlowMeshRuntimeRelay {
    std::optional<flowmesh::WirePeerId> peer;
    std::optional<flowmesh::WirePeerId> exclude_peer;
    flowmesh::WireMessage message;
    // Process-local delivery identity; never part of the signed wire object.
    uint64_t delivery_id{0};
};

using FlowMeshRuntimeRelayFn =
    std::function<FlowMeshRelayResult(FlowMeshRuntimeRelay relay)>;

struct FlowMeshRuntimeEvent {
    uint64_t monotonic_us{0};
    std::string stage;
    flowmesh::WireMessageKind kind{flowmesh::WireMessageKind::ACTION};
    uint64_t sequence{0};
    uint256 object_id;
    std::optional<flowmesh::WirePeerId> peer;
    std::string reason;
    // Per-market/runtime append cursor, not a protocol sequence or receipt
    // ACK. Timestamps can precede earlier appends for concurrent admissions.
    uint64_t event_id{0};
    uint64_t epoch{0};
    // Observed set unless a decoded/certified entry supplies its signed set.
    uint256 seat_set_hash;
    std::optional<uint32_t> round;
    std::optional<uint32_t> seat_index;
    std::optional<uint256> related_object_id;
    // SHA256d of compressed public bytes/signature; no private material.
    std::optional<uint256> bls_key_hash;
    std::optional<uint256> signature_hash;
    // SHA256d of EncodeWireMessage bytes, populated only for BENCH tracing.
    std::optional<uint256> wire_hash;
    std::optional<uint64_t> delivery_id;
};

/** Local process observations, never proof of peer receipt or a remote tip.
 * Admission includes legacy queue acceptance, which has no socket completion.
 * catchup_completed counts applied pages with local tail slack, not a tip ACK.
 * event_queue_overflows is node-global and repeated in each market snapshot. */
/** ACTION frame header policy observations (local relay policy, never
 * consensus). A frame's header only claims its sender's certified head. */
struct FlowMeshRuntimeActionAdmission {
    //! Remote frames for another version, market or epoch.
    uint64_t header_refused_identity{0};
    //! Remote frames more than two slots ahead or behind this node's head.
    uint64_t header_refused_far{0};
    //! Remote frames behind this head whose interval this node could not
    //! prove excludes the action (not retained, epoch change, included, or
    //! no longer pending).
    uint64_t header_refused_unproven{0};
    //! Frames admitted from a sender one or two slots ahead (a lagging node).
    uint64_t header_accepted_ahead{0};
    //! Frames admitted from a sender one or two slots behind, relayed with
    //! this node's head (the payload is never touched).
    uint64_t header_restamped_behind{0};
};

struct FlowMeshRuntimeDeliverySnapshot {
    flowmesh::MarketId market_id;
    uint64_t created{0}, admitted{0}, refused{0}, retried{0};
    uint64_t socket_written{0}, verified{0}, certificate_formed{0};
    uint64_t durably_applied{0}, catchup_started{0}, catchup_completed{0};
    uint64_t catchup_timeouts{0}, catchup_replies_refused{0}, catchup_partial_pages{0};
    uint64_t completion_timeouts{0}, retention_refused{0}, cancelled{0};
    uint64_t event_queue_overflows{0};
    uint64_t receive_deferred{0}, receive_refused{0};
    // Exact repeats of already verified AGREEMENT bytes consumed at admission
    // without a committee token, queue slot or second verification.
    uint64_t agreement_duplicates_coalesced{0};
    // ATTESTATION/AGREEMENT frames for slots this node has already durably
    // appended, and exact entry copies of its recent appends, consumed at
    // admission without a committee token, queue slot or handler pass.
    uint64_t certified_slot_frames_consumed{0};
    // Frames of either kind above refused RATE_LIMITED because their sender
    // spent one of its separate budgets for consumed frames (per market, or
    // across all markets). This includes a frame refused before admission
    // hashed it, because its sender had no settled and no committee token
    // left. A peer is one connection identity (legacy NodeId or FMN2 session
    // id). In steady state the per-peer budget binds when one sender's summed
    // sustained settled rate exceeds 128/s (about 11 markets at the 12/s
    // average measured with four validators); a non-zero value points at a
    // flooding peer, or at a larger committee or market count whose honest
    // relays outgrew the sizing.
    uint64_t settled_frames_rate_limited{0};
    size_t pending_objects{0}, pending_bytes{0};
    size_t deferred_objects{0}, deferred_bytes{0};
    std::string current_reason;
    uint256 last_target_hash;
    uint64_t sampled_monotonic_us{0};
    uint64_t last_event_id{0}, events_dropped{0};
    // The opt-in BENCH mirror is capped at 16 MiB per market/process.
    uint64_t trace_bytes{0}, trace_events_dropped{0};
    // Process-lifetime 64 MiB BENCH cap; repeated across market snapshots.
    uint64_t trace_global_bytes{0}, trace_global_events_dropped{0};
    // At most 128 events. Clocks are monotonic and local to this process.
    std::vector<FlowMeshRuntimeEvent> events;
    FlowMeshRuntimeActionAdmission action_admission;
};

struct FlowMeshRuntimeMarketConfig {
    uint256 domain;
    flowmesh::MarketId market_id;
    uint256 treasury_owner_commitment;
    flowmesh::ActiveFnBlsSeatSet active_seats;
    flowmesh::FlowMeshState state;
    uint64_t next_sequence{0};
    uint64_t next_effect_index{0};
    uint256 last_microblock_hash;
    FlowMeshProductionStore* store{nullptr};
    const flowmesh::DepositVerifier* deposits{nullptr};
    // Immutable fresh-market-only operator mode, protected by store format v4.
    bool preagreement{false};
    fs::path agreement_path;
    uint256 agreement_identity;
    bool agreement_allow_create{false};
    /** A discovered live market may exist before a valid k>=4 set exists. */
    FlowMeshRuntimeMarketReadiness readiness{
        FlowMeshRuntimeMarketReadiness::READY};
    //! Display-only operator-observed time (flowmesh_observed_time_log.h),
    //! recorded after each durable commit. Nothing in the runtime reads it.
    std::shared_ptr<FlowMeshObservedTimeLog> observed{};
};

/** Sets `out.observed` from `log` (null: not recorded by this node): the
 * stamps of the head and of exactly the reply's history and tick entries
 * whose hash matches; unavailable while the log's history loads
 * (FlowMeshObservedTimeLog::LOADING_REASON). Display only; call outside the
 * runtime's locks. */
void FillObservedTimes(flowmesh::MarketData& out, const FlowMeshObservedTimeLog* log);

struct FlowMeshRuntimeConfig {
    FlowMeshRuntimeChain* chain{nullptr};
    FlowMeshRuntimeKeyProvider* keys{nullptr};
    FlowMeshRuntimeClock* clock{nullptr};
    FlowMeshRuntimeRelayFn relay;

    //! Policy only. It selects the next proposer round but is never serialized
    //! into a consensus parameter. Fully validated proposals are processed
    //! independently of timer skew; only an adjacent round can nudge the
    //! local timer. Permanent candidate/signature locks remain authoritative.
    std::chrono::milliseconds round_timeout{std::chrono::seconds{2}};
    //! Repeat a completed legacy peer/market discovery sweep after this delay.
    //! At least one minute; global probe pacing remains one request per second.
    std::chrono::milliseconds legacy_probe_interval{std::chrono::seconds{60}};
    std::function<void(uint64_t)> cancel_delivery;
    //! Memory-only retention of recent certified entries per market. The
    //! defaults are the production bounds; tests may only lower them.
    FlowMeshRecentWindowLimits recent_window{};
};

struct FlowMeshRuntimeMarketStatus {
    uint64_t epoch{0};
    uint64_t next_sequence{0};
    uint64_t next_effect_index{0};
    uint32_t round{0};
    uint256 last_microblock_hash;
    uint256 state_root;
    size_t pending_actions{0};
    bool observer_only{true};
    bool paused{false};
    bool pending_handoff{false};
    FlowMeshRuntimeHalt halt{FlowMeshRuntimeHalt::NONE};
    std::string error;
    //! Writer-sampled current seat context (not the last entry's old set).
    //! Internal advisory cache pins only; never grants signing authority.
    uint256 current_seat_set_hash;
    std::optional<flowmesh::AnchorRef> current_seat_anchor;
};

/** Local scheduling only: a fully applied page could fit any further entry,
 * so a greedy honest sender would have included it. This is not a tip proof. */
inline bool FlowMeshCatchupPageHasTailSlack(
    const size_t requested_count, const size_t requested_bytes,
    const size_t received_count, const size_t received_bytes,
    const size_t applied_count)
{
    constexpr size_t MAX_FRAMED_ENTRY{4 + flowmesh::FLOWMESH_CERTIFICATE_MAX_BYTES};
    return received_count != 0 && applied_count == received_count &&
           received_count < requested_count && received_bytes <= requested_bytes &&
           requested_bytes - received_bytes >= MAX_FRAMED_ENTRY;
}

/** Memory-only relay policy, independent of consensus and signing. */
class FlowMeshEvidenceRetryBudget
{
public:
    static constexpr size_t MAX_MESSAGES{8};
    static constexpr size_t MAX_BYTES{32 * 1024};
    static constexpr size_t MAX_MARKETS_SCANNED{16};
    static constexpr auto INTERVAL{std::chrono::milliseconds{250}};
    static constexpr auto SWEEP_DELAY{std::chrono::seconds{1}};

    bool Begin(flowmesh::WireClock::time_point now)
    {
        if (now < m_next) return false;
        m_next = now + INTERVAL;
        m_messages = MAX_MESSAGES;
        m_bytes = MAX_BYTES;
        return true;
    }
    bool Consume(size_t framed_bytes)
    {
        if (m_messages == 0 || framed_bytes > m_bytes) return false;
        --m_messages;
        m_bytes -= framed_bytes;
        return true;
    }
    bool Full() const { return m_messages == 0 || m_bytes == 0; }

private:
    flowmesh::WireClock::time_point m_next{};
    size_t m_messages{0};
    size_t m_bytes{0};
};

/** Verified committee gossip and cached local vote retries; before fan-out. */
class FlowMeshCommitteeRelayBudget
{
public:
    static constexpr size_t GLOBAL_MESSAGES{32};
    static constexpr size_t GLOBAL_BYTES{4 * 1024 * 1024};
    static constexpr size_t MARKET_MESSAGES{8};
    static constexpr size_t MARKET_BYTES{2 * 1024 * 1024 + 4 * 1024};
    static constexpr auto INTERVAL{std::chrono::milliseconds{250}};
    static constexpr auto REPEAT_DELAY{std::chrono::seconds{1}};

    FlowMeshCommitteeRelayBudget(size_t messages, size_t bytes)
        : m_max_messages{messages}, m_max_bytes{bytes} {}

    bool Available(flowmesh::WireClock::time_point now, size_t bytes)
    {
        if (now >= m_next) {
            m_next = now + INTERVAL;
            m_messages = m_max_messages;
            m_bytes = m_max_bytes;
        }
        return m_messages != 0 && bytes <= m_bytes;
    }
    void Charge(size_t bytes)
    {
        --m_messages;
        m_bytes -= bytes;
    }
    void Reset()
    {
        m_next = {};
        m_messages = 0;
        m_bytes = 0;
    }

private:
    const size_t m_max_messages;
    const size_t m_max_bytes;
    flowmesh::WireClock::time_point m_next{};
    size_t m_messages{0};
    size_t m_bytes{0};
};

/** Additional exact ACTION copies; not the retained-candidate retry budget. */
class FlowMeshDuplicateActionRelayBudget : public FlowMeshCommitteeRelayBudget
{
public:
    static constexpr size_t MAX_MESSAGES{4};
    static constexpr size_t MAX_BYTES{16 * 1024};
    static constexpr size_t MAX_MARKETS_SCANNED{16};
    static constexpr size_t MAX_ACTIONS_SCANNED{16};
    static constexpr auto REPEAT_DELAY{std::chrono::milliseconds{250}};

    FlowMeshDuplicateActionRelayBudget()
        : FlowMeshCommitteeRelayBudget{MAX_MESSAGES, MAX_BYTES} {}
};

/** Locally admitted pending ACTION retries; no new payload or authorization. */
class FlowMeshLocalActionRelayBudget : public FlowMeshCommitteeRelayBudget
{
public:
    // Together with retained-evidence (32/s) and duplicate forwarding (16/s),
    // this additional 16/s stays within the existing 64 ACTION/s peer budget.
    static constexpr size_t GLOBAL_MESSAGES{4};
    static constexpr size_t GLOBAL_BYTES{16 * 1024};
    static constexpr size_t MARKET_MESSAGES{2};
    static constexpr size_t MARKET_BYTES{8 * 1024};
    static constexpr size_t MAX_MARKETS_SCANNED{16};
    static constexpr size_t MAX_ACTIONS_SCANNED{16};
    static constexpr auto REPEAT_DELAY{std::chrono::seconds{1}};

    FlowMeshLocalActionRelayBudget(size_t messages = GLOBAL_MESSAGES,
                                  size_t bytes = GLOBAL_BYTES)
        : FlowMeshCommitteeRelayBudget{messages, bytes} {}
};

/** Shared, immutable parts of one retained recent certified entry. Encoding
 * happens outside every runtime lock. */
struct FlowMeshClientOutcomeParts {
    //! The exact (sequence, hash) is retained in the recent window.
    bool found{false};
    //! The sequence is retained, whatever its hash.
    bool sequence_retained{false};
    std::shared_ptr<const flowmesh::ProductionCertifiedEnvelope> envelope;
    size_t seat_count{0};
    //! The store's own re-execution record, when retained.
    std::shared_ptr<const ProductionOutcomeRecord> outcome;
};

/** The seat-misbehaviour recorder's verification work (observation only). */
struct FlowMeshSeatEvidenceWork {
    //! Detections checked: past the dedup and the per-market pace.
    uint64_t checks{0};
    //! Chain seat-set reads for a set other than the market's installed one.
    uint64_t chain_lookups{0};
    //! Seat-set validations and BLS verifications (flowmesh::SeatEvidenceWork).
    uint64_t seat_set_checks{0};
    uint64_t signature_checks{0};
};

/** One local API/RPC submission: the certified-state decision and, only when
 * that decision is ENQUEUE, the bounded queue's verdict. */
struct FlowMeshLocalSubmitResult {
    std::optional<flowmesh::QueueResult> queued;
    FlowMeshLocalAdmission admission;
    //! The client event log position just before the enqueue. A refusal
    //! event at or before it belongs to an earlier attempt, not this one.
    std::optional<flowmesh::ClientEventCursor> cursor_before;
};

/** Certificate, state, status and reported history captured at one head. */
struct FlowMeshClientSnapshotView {
    flowmesh::ClientStateEvidence evidence;
    flowmesh::MarketData reported;
};

/**
 * Production FlowMesh orchestration core.
 *
 * P2P threads perform only bounded wire framing and call EnqueueWireMessage.
 * One owned worker serializes all market state transitions outside cs_main.
 * The relay callback keeps this class independent from CConnman and lets the
 * node integration choose peer subscriptions and outbound scheduling.
 *
 * Discovering anchor-final USER_DEPOSIT outputs and constructing each market
 * config belongs to NodeContext integration; AddMarket installs that config
 * without dropping existing queues or durable market state.
 */
class FlowMeshRuntime final : public flowmesh::WireMessageSink
{
public:
    FlowMeshRuntime(FlowMeshRuntimeConfig config,
                    std::vector<FlowMeshRuntimeMarketConfig> markets);
    ~FlowMeshRuntime() override;

    FlowMeshRuntime(const FlowMeshRuntime&) = delete;
    FlowMeshRuntime& operator=(const FlowMeshRuntime&) = delete;

    bool Start(std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex, !m_health_mutex);
    void Stop();

    flowmesh::QueueResult EnqueueWireMessage(
        flowmesh::WirePeerId peer,
        flowmesh::WireMessage message) override;
    void FlowMeshPeerConnected(flowmesh::WirePeerId peer) override;
    void FlowMeshPeerDisconnected(flowmesh::WirePeerId peer) override;

    /** Wake the worker to evaluate timeout/anchor/set policy. */
    void NotifyTick();

    /** Begin one bounded, requested-only catch-up from `peer`. */
    bool RequestCatchup(flowmesh::WirePeerId peer,
                        const flowmesh::MarketId& market_id);

    /**
     * Add an anchor-final discovered market without restarting the runtime.
     * Initialization/replay is serialized on the worker, outside cs_main;
     * callers must invoke this synchronous boundary without holding cs_main
     * and not from the runtime relay callback.
     * A ready config may replace the same market's insufficient-seat shell.
     */
    bool AddMarket(FlowMeshRuntimeMarketConfig market, std::string& error);

    std::vector<flowmesh::MarketId> MarketIds() const;

    /**
     * Bounded wallet/RPC admission path. The signed action uses the same
     * production codec, queue, authentication, and relay path as a peer
     * action; local callers cannot inject directly into execution state.
     */
    flowmesh::QueueResult SubmitLocalAction(
        const flowmesh::MarketId& market_id, const flowmesh::Action& action);
    /**
     * The same admission, first decided against this node's certified state
     * in the same market-lock scope (FlowMeshDecideLocalAdmission). Only an
     * ENQUEUE decision reaches the queue. Any other decision appends no v1
     * event; an authenticated attempt (pure Schnorr, outside the market
     * lock; a deposit is its outpoint fact) gets a local notice. Never
     * applied to remote frames, proposals or certified entries. `origin`
     * picks the notice ring of this submission's notices, including the
     * worker's recheck and pool-drop notices for it: NODE only for this
     * node's own wallet RPC and GUI, never for public API traffic. A PUBLIC
     * deposit gets no notice from this call, whether refused for missing
     * facts or answered as already credited (it has no credential). `submitter`, for PUBLIC only, is the requesting client's
     * identity (IPv4, or IPv6 /64): its notices count against that
     * identity's share of the PUBLIC ring (CLIENT_NOTICE_PER_SUBMITTER).
     */
    FlowMeshLocalSubmitResult SubmitLocalActionChecked(
        const flowmesh::MarketId& market_id, const flowmesh::Action& action,
        flowmesh::ClientNoticeOrigin origin = flowmesh::ClientNoticeOrigin::PUBLIC,
        const std::string& submitter = {});
    //! Current position of the client event log (a cheap leaf-lock read).
    flowmesh::ClientEventCursor ClientEventCursor() const;
    //! The newest local notice for this exact action, if retained.
    std::optional<flowmesh::ClientActionNotice> NewestClientNotice(
        const flowmesh::MarketId& market_id, const uint256& action_id) const;
    //! The newest local notice only while it is newer than every retained
    //! event of this exact action.
    std::optional<flowmesh::ClientActionNotice> NewestClientNoticeIfNewer(
        const flowmesh::MarketId& market_id, const uint256& action_id) const;

    std::optional<FlowMeshRuntimeMarketStatus> MarketStatus(
        const flowmesh::MarketId& market_id) const EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex);
    /** Synchronous, advisory key-change observation. Call after releasing the
     * key provider's mutex, without cs_main or a runtime relay callback.
     * Updates published observer metadata only: never signs, schedules work
     * or grants authority. Before Start/from Stop it does nothing. Concurrent
     * replacements are sampled from the live provider, not a caller's keys. */
    void RefreshSeatKeyObservation() EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex);
    std::optional<flowmesh::FlowMeshState> StateSnapshot(
        const flowmesh::MarketId& market_id) const EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex);
    //! Same committed state, shared and immutable; no market/store/chain lock.
    std::shared_ptr<const flowmesh::FlowMeshState> StateView(
        const flowmesh::MarketId& market_id) const EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex);

    /** One bounded snapshot of certified curves, account state and history.
     * No whole-state copy, state hashing, log scanning or execution on reads. */
    std::optional<flowmesh::MarketData> MarketData(
        const flowmesh::MarketId& market_id,
        const std::optional<flowmesh::AccountId>& account,
        const flowmesh::MarketDataQuery& query, std::string& error) const EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex);

    /** Exact current certificate and immutable state at one published head.
     * Serialization stops at the client cap, outside runtime locks. */
    std::optional<flowmesh::ClientStateEvidence> ClientSnapshot(
        const flowmesh::MarketId& market_id, std::string& error) const EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex);
    /** Certificate/state and bounded reported history from ONE immutable view.
     * A later publication cannot mix two heads into this response.
     * `reported_query` shapes only the reported (unauthenticated) part. */
    std::optional<FlowMeshClientSnapshotView> ClientSnapshotView(
        const flowmesh::MarketId& market_id,
        const std::optional<flowmesh::AccountId>& account, std::string& error,
        const flowmesh::MarketDataQuery& reported_query = {}) const EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex);
    /** One certified entry as a client payload. Entries in the recent window
     * (the head included) are encoded from memory outside the market lock;
     * only older sequences read the store. allow_history=false never takes
     * a market/chain/store lock; absent old evidence is history_busy, not
     * expired or proof of nonexecution. */
    std::optional<std::vector<unsigned char>> ClientCertifiedEntry(
        const flowmesh::MarketId& market_id, uint64_t sequence, std::string& error,
        bool allow_history = true) const EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex);
    /** Memory-only: the retained certified entry that includes this exact
     * action (a decisive disposition first, else the newest), if any. */
    std::optional<std::pair<uint64_t, uint256>> ClientRecentInclusion(
        const flowmesh::MarketId& market_id, const uint256& action_id) const EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex);
    /** Memory-only: shared parts of one retained (sequence, hash). Never
     * reads the store, encodes or verifies under the lock. */
    FlowMeshClientOutcomeParts ClientOutcomeParts(
        const flowmesh::MarketId& market_id, uint64_t sequence, const uint256& hash,
        bool want_payload) const EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex);
    /** Test-only: ClientCertifiedEntry calls that read the store. */
    uint64_t ClientEntryStoreReadsForTest() const;
    //! A display view's curve copy unused for this long is dropped by the
    //! next market-data read.
    static constexpr std::chrono::seconds BOOK_CAPTURE_IDLE{30};
    /** Test-only: display-view curve copies retained (one per market at most). */
    size_t BookCapturesForTest() const EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex);
    flowmesh::ClientEventPage ClientEvents(
        const std::optional<flowmesh::ClientEventCursor>& after,
        const std::optional<flowmesh::MarketId>& market,
        const std::optional<flowmesh::AccountId>& account,
        size_t limit = flowmesh::CLIENT_EVENT_PAGE_MAX) const;
    std::optional<flowmesh::ClientEvent> ClientActionStatus(
        const flowmesh::MarketId& market_id, const uint256& action_id) const;
    /** Bounded long-poll on the client event log (see ClientEventLog::
     * WaitActionStatus). Holds no queue or market lock while it sleeps. Before
     * Start and from Stop on, every current and later wait is INTERRUPTED. */
    flowmesh::ClientActionWait WaitClientActionStatus(
        const flowmesh::MarketId& market_id, const uint256& action_id,
        std::chrono::steady_clock::time_point deadline,
        const std::function<bool()>& interrupted, bool include_notices = false) const;
    /** Ends every current client-action wait and event watch (shutdown,
     * never a result). */
    void WakeClientWaiters() const;
    //! Nonblocking: would ClientEvents(after, market, account) now return an
    //! event (READY) or report a gap (GAP)? Otherwise TIMEOUT.
    flowmesh::ClientWatchResult ClientEventsPending(
        const flowmesh::ClientEventCursor& after, const flowmesh::MarketId& market,
        const std::optional<flowmesh::AccountId>& account) const;
    /** Bounded watch on the client event log (see ClientEventLog::WaitEvents).
     * Holds no queue or market lock while it sleeps. Before Start and from
     * Stop on, every current and later watch that would sleep is INTERRUPTED. */
    flowmesh::ClientWatchResult WaitClientEvents(
        const flowmesh::ClientEventCursor& after, const flowmesh::MarketId& market,
        const std::optional<flowmesh::AccountId>& account,
        std::chrono::steady_clock::time_point deadline,
        const std::function<bool()>& interrupted) const;

    /** Test/shutdown aid: waits only for this runtime's current work queue. */
    bool WaitForIdle(std::chrono::milliseconds timeout);

    /** Nonblocking bounded network-completion handoff; false means refused. */
    bool NotifyDeliveryEvent(const FlowMeshDeliveryEvent& event);
    std::vector<FlowMeshRuntimeDeliverySnapshot> DeliverySnapshots(
        std::optional<flowmesh::MarketId> market = std::nullopt) const;

    /** Latest worker-published health observation: published once by Start
     * and at most every 250 ms from the worker's tick. Takes only a leaf
     * mutex (never the market or queue mutex, cs_main or the store), so it
     * answers while the worker is busy or wedged; the snapshot's age is then
     * itself the signal. Null before the first Start. Never evidence. */
    std::shared_ptr<const FlowMeshRuntimeHealthSnapshot> HealthSnapshot() const
        EXCLUSIVE_LOCKS_REQUIRED(!m_health_mutex);

    /** Test-only: how many frames' bytes admission has hashed so far. */
    uint64_t AdmissionHashCountForTest() const;
    /** Test-only: the seat-misbehaviour recorder's work, summed over markets. */
    FlowMeshSeatEvidenceWork SeatEvidenceWorkForTest() const;
    /** Test-only: public actions whose submitter the market remembers. */
    size_t PublicSubmittersForTest(const flowmesh::MarketId& market_id) const;
    /** Test-only: the identity the market remembers as `action_id`'s public
     * submitter, if any. */
    std::optional<std::string> PublicSubmitterForTest(const flowmesh::MarketId& market_id, const uint256& action_id) const;

private:
    friend class FlowMeshRuntimeTestAccess;
    struct Market;
    struct ReadView;
    std::shared_ptr<const ReadView> LoadReadView(const flowmesh::MarketId& market_id) const
        EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex);
    //! Writer-owned calls only. No authority decision reads the published view.
    void PublishReadLocked(Market& market, const flowmesh::ProductionEntryCore* committed = nullptr,
                          std::vector<std::shared_ptr<const ReadView>>* local_retired = nullptr)
        EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex);
    void PublishReadViewsLocked() EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex);
    //! Called outside m_market_mutex; old states can be expensive to destroy.
    void ReleaseRetiredReadViews() EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex);
    struct CatchupCommand {
        flowmesh::WirePeerId peer{0};
        flowmesh::MarketId market_id;
    };
    struct AddMarketCommand {
        FlowMeshRuntimeMarketConfig market;
        std::shared_ptr<std::promise<std::pair<bool, std::string>>> completion;
    };

    bool InitializeMarkets(std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex);
    bool InitializeMarket(const FlowMeshRuntimeMarketConfig& config,
                          std::string& error,
                          std::deque<flowmesh::ClientEvent>* startup_events = nullptr)
        EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex);
    void WorkerLoop() EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex, !m_health_mutex);
    //! EnqueueWireMessage, with the frame marked counted (QueuedWireMessage).
    flowmesh::QueueResult Enqueue(flowmesh::WirePeerId peer, flowmesh::WireMessage message, bool counted,
                                 uint64_t local_retention_id = 0);
    //! Producer stages, also inspected through the narrow friend test seam.
    uint64_t ReserveLocalAction(const flowmesh::WireMessage& message, const uint256& domain,
                               const uint256& config, flowmesh::ClientNoticeOrigin origin,
                               const std::string& submitter, flowmesh::WireClock::time_point deadline);
    flowmesh::QueueResult EnqueueReservedLocalAction(uint64_t id);
    flowmesh::QueueResult EnqueueReservedLocalActionLocked(uint64_t id);
    void ReleaseLocalAction(uint64_t id);
    void DeferLocalAction(uint64_t id, bool observed_chain_gate_closed = false);
    size_t RetryLocalActionAdmissions(bool open, size_t budget, bool reopened);
    void ProcessMessage(const flowmesh::QueuedWireMessage& queued, uint64_t dequeued_us);
    void ProcessTick(uint64_t requested_us, uint64_t dequeued_us) EXCLUSIVE_LOCKS_REQUIRED(!m_health_mutex);
    struct PendingDelivery;
    void RelayMessage(Market& market, flowmesh::WireMessage message,
                      std::optional<flowmesh::WirePeerId> peer,
                      std::optional<flowmesh::WirePeerId> exclude,
                      bool replay_budget_charged = false);
    void AttemptDelivery(PendingDelivery& pending, bool retry);
    void RestartDelivery(uint64_t id);
    bool RecheckDelivery(uint64_t id);
    void RetryDeliveries();
    void RegenerateDeliveries();
    void DeferMessage(Market& market, const flowmesh::QueuedWireMessage& queued);
    void RetryDeferredMessages();
    void ProcessDeliveryEvent(const FlowMeshDeliveryEvent& event);
    void EraseDelivery(uint64_t id, const char* reason);
    void RetryRetainedEvidence();
    void RetryLocalActions();
    void ForwardPendingDuplicateActions();
    void AnnounceMarkets(bool refresh);
    void ProbeLegacyPeers(const std::vector<flowmesh::WirePeerId>& peers);
    bool TryRequestCatchup(Market& market, flowmesh::WirePeerId peer);
    void ExpireCatchup(flowmesh::WirePeerId peer, const flowmesh::MarketId& market_id);
    void ProcessCatchupCommand(const CatchupCommand& command);
    void ProcessAddMarketCommand(AddMarketCommand command) EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex, !m_health_mutex);
    void RemovePeerOnWorker(flowmesh::WirePeerId peer);
    //! Caller holds m_market_mutex. Display-only observed-time upkeep of
    //! every market that records it; decides nothing.
    void ObserveTimeMaintenance(size_t connected_peers);
    //! Caller holds m_market_mutex. Copies values the worker already owns;
    //! never reads the store, signs, executes or calls LocalSeatKeys.
    void PublishHealth(flowmesh::WireClock::time_point now, size_t connected_peers,
                       std::optional<flowmesh::WireClock::time_point> no_peers_since, bool force)
        EXCLUSIVE_LOCKS_REQUIRED(!m_health_mutex);

    void HandleAction(Market& market, flowmesh::WirePeerId peer,
                      const flowmesh::WireMessage& message, bool* authority_deferred = nullptr);
    void HandleHello(Market& market, flowmesh::WirePeerId peer,
                     const flowmesh::WireMessage& message);
    void HandleProposal(Market& market, flowmesh::WirePeerId peer,
                        const flowmesh::WireMessage& message);
    void HandleAttestation(Market& market, flowmesh::WirePeerId peer,
                           const flowmesh::WireMessage& message);
    void HandleCertificate(Market& market, flowmesh::WirePeerId peer,
                           const flowmesh::WireMessage& message,
                           bool from_catchup = false,
                           bool* reconciliation_deferred = nullptr);
    void HandleGet(Market& market, flowmesh::WirePeerId peer,
                   const flowmesh::WireMessage& message);
    void HandleEntries(Market& market, flowmesh::WirePeerId peer,
                       const flowmesh::WireMessage& message);
    void MaybePropose(Market& market);
    bool InitializeAgreement(Market& market, const FlowMeshRuntimeMarketConfig& config,
                             std::string& error);
    bool RefreshAgreement(Market& market);
    bool PublishAgreement(Market& market, const flowmesh::AgreementMessage& message,
                          std::optional<flowmesh::WirePeerId> exclude = std::nullopt);
    void HandleAgreement(Market& market, flowmesh::WirePeerId peer,
                         const flowmesh::WireMessage& message);
    void FinalizeAgreement(Market& market);
    void MaybeCertify(Market& market, const uint256& candidate_hash);
    /** False means fresh safety policy failed, not merely a paced no-send. */
    bool ForwardCommitteeMessage(
        Market& market, const flowmesh::ProductionEntryCore& entry,
        flowmesh::WirePeerId peer, const flowmesh::WireMessage& message,
        std::optional<flowmesh::WireClock::time_point>& last_attempt);

    FlowMeshRuntimeConfig m_config;
    flowmesh::ClientEventLog m_client_events{uint256{}};
    //! Lock-free, read under the event log's mutex by client-action waits.
    std::atomic<bool> m_client_waits_closed{true};
    std::vector<FlowMeshRuntimeMarketConfig> m_market_configs;

    mutable std::mutex m_market_mutex;
    std::optional<flowmesh::MarketData> ProjectReadView(
        const ReadView& view, const std::optional<flowmesh::AccountId>& account,
        const flowmesh::MarketDataQuery& query, std::string& error) const;
    std::optional<flowmesh::ClientStateEvidence> SnapshotReadView(
        const ReadView& view, const flowmesh::ClientEventCursor& cursor, std::string& error) const;
    //! Caller holds m_market_mutex: other markets' counted recent-window bytes.
    std::pair<size_t, size_t> RecentWindowOthers(const Market& self) const;
    std::shared_ptr<const flowmesh::BookCurves> BookCopy(
        const std::shared_ptr<const ReadView>& view, FlowMeshTimingSpan& timing) const;
    void PruneBookCaptures() const EXCLUSIVE_LOCKS_REQUIRED(!m_read_mutex);
    //! Leaf publication lock. Never held for projection, encoding or destruction.
    mutable Mutex m_read_mutex;
    std::map<flowmesh::MarketId, std::shared_ptr<const ReadView>> m_read_views GUARDED_BY(m_read_mutex);
    std::vector<std::shared_ptr<const ReadView>> m_retired_read_views GUARDED_BY(m_read_mutex);
    std::map<flowmesh::MarketId, std::unique_ptr<Market>> m_markets;
    //! ClientCertifiedEntry store reads (test observation only).
    mutable std::atomic<uint64_t> m_client_entry_store_reads{0};

    mutable std::mutex m_queue_mutex;
    std::condition_variable m_work_cv;
    std::condition_variable m_idle_cv;
    flowmesh::BoundedWireQueue m_queue;
    //! Ready market ids admitted to m_queue; guarded by m_queue_mutex.
    std::set<flowmesh::MarketId> m_admitted_markets;
    //! Exact AGREEMENT payloads the agreement engine has already verified and
    //! retained, including the validated wire header. Recovery copies of the
    //! active slot are paced; certified older slots stay coalesced until FIFO
    //! eviction. Bounded per market; guarded by m_queue_mutex.
    struct AgreementQuiet {
        static constexpr size_t MAX_PAYLOADS{4096};
        struct Payload {
            flowmesh::WireHeader header;
            flowmesh::WireClock::time_point until;
        };
        // Advanced only from the worker's certified production history.
        uint64_t sequence{0};
        std::map<uint256, Payload> payloads;
        std::deque<uint256> order;
        uint64_t coalesced{0};
    };
    std::map<flowmesh::MarketId, AgreementQuiet> m_agreement_quiet;
    void QuietAgreementPayload(const flowmesh::WireHeader& header, const uint256& payload_hash,
                               flowmesh::WireClock::time_point until);
    //! What this worker has durably appended for a market since the market
    //! was last (re)initialized, as seen by admission: committee frames for
    //! those slots are consumed without a committee token (EnqueueWireMessage).
    //! Written only after a durable append and erased whenever the market is
    //! (re)initialized. Bounded per admitted market; guarded by m_queue_mutex.
    struct CertifiedAdmission {
        static constexpr size_t MAX_ENTRIES{8};
        //! One past the last durably appended sequence.
        uint64_t next_sequence{0};
        //! (exact wire header, SHA256d of the exact entry bytes) of the last
        //! few durable appends, oldest first.
        std::deque<std::pair<flowmesh::WireHeader, uint256>> entries;
        uint64_t consumed{0};
    };
    std::map<flowmesh::MarketId, CertifiedAdmission> m_certified_admission;
    //! Admitted markets only; guarded by m_queue_mutex. Diagnostic count of
    //! frames refused because their sender's settled-frame budget was spent.
    std::map<flowmesh::MarketId, uint64_t> m_settled_rate_limited;
    //! Guarded by m_queue_mutex. Frames whose bytes admission hashed (an
    //! agreement payload, a certificate's entry bytes); test observation.
    uint64_t m_admission_hashes{0};
    void NoteCertifiedAdmission(const flowmesh::WireHeader& header, const uint256& entry_bytes_hash,
                                uint64_t next_sequence);
    //! Negotiated connections only, capped independently of incoming hints.
    std::set<flowmesh::WirePeerId> m_discovery_peers;
    //! Health observation only, never a discovery or relay input: the
    //! currently connected FlowMesh peers (uncapped by the discovery bound;
    //! bounded by the transports' own connection limits) and since when there
    //! have been none. Guarded by m_queue_mutex.
    std::set<flowmesh::WirePeerId> m_connected_peers;
    std::optional<flowmesh::WireClock::time_point> m_no_peers_since;
    std::deque<flowmesh::WirePeerId> m_removed_peers;
    std::deque<CatchupCommand> m_catchup_commands;
    std::deque<AddMarketCommand> m_add_market_commands;
    std::deque<FlowMeshDeliveryEvent> m_delivery_events;
    std::atomic<uint64_t> m_delivery_event_overflows{0};
    bool m_tick_pending{false};
    //! Observation of the first coalesced tick request; never a timer input.
    uint64_t m_tick_requested_us{0};
    bool m_discovery_refresh{false};
    bool m_started{false};
    bool m_stopping{false};
    bool m_processing{false};
    //! Diagnostic state only, guarded by m_queue_mutex; never a wake predicate.
    bool m_worker_waiting{false};
    std::thread m_worker;
    //! Leaf: acquired after m_market_mutex, never before it.
    mutable Mutex m_health_mutex;
    std::shared_ptr<const FlowMeshRuntimeHealthSnapshot> m_health GUARDED_BY(m_health_mutex);
    //! Worker-owned (under m_market_mutex): the next tick that publishes.
    flowmesh::WireClock::time_point m_next_health_publish{};
    //! Observed-time upkeep (display only) samples an open chain gate at
    //! most once per OBSERVED_SAMPLE_INTERVAL (a closed one on every tick);
    //! the markets' warm-up is observed on every tick against the latest
    //! sample.
    static constexpr std::chrono::seconds OBSERVED_SAMPLE_INTERVAL{1};
    flowmesh::WireClock::time_point m_next_observed_sample{};
    struct ObservedGateSample {
        int32_t anchor_height{-1};
        bool open{false};
    };
    std::optional<ObservedGateSample> m_observed_gate;

    // Exact signed critical objects: <=8192 / 64 MiB globally, <=16 MiB per
    // market. Retry at >=1s, or at once when a pause cancelled the admitted
    // attempt; lost completion expires at 5s; paused copies
    // retire after 60s and halted/disarmed copies retire immediately.
    // Compact proof/entry
    // sources survive retention rotation; no retry creates a signature.
    std::map<uint64_t, std::unique_ptr<PendingDelivery>> m_deliveries;
    std::map<uint256, uint64_t> m_delivery_keys;
    uint64_t m_next_delivery_id{1}, m_delivery_cursor{0};
    size_t m_delivery_bytes{0};
    bool m_delivery_retention_waiting{false};
    uint256 m_delivery_regeneration_cursor;
    flowmesh::WireClock::time_point m_next_delivery_regeneration{};
    // Unvalidated reconciliation-race input, <=128 / 16 MiB, per-market16,
    // per-peer8, expires after60s with an explicit bounded catch-up request.
    std::map<uint256, flowmesh::QueuedWireMessage> m_deferred_messages;
    std::map<uint256, flowmesh::WireClock::time_point> m_deferred_deadlines;
    size_t m_deferred_bytes{0};
    uint256 m_deferred_cursor;
    flowmesh::WireClock::time_point m_next_deferred_retry{};
    //! Local scheduling only: the chain gate and reconciliation generation
    //! the deferred retry last observed. A change retries at once.
    bool m_deferred_gate_open{false};
    uint64_t m_deferred_gate_generation{0};
    /** Exact accepted local sources. At most the existing per-peer deferred
     * bound of eight, also charged against shared128/16MiB and16/market.
     * Reserve BEFORE acknowledgement; no remote source can allocate one.
     * Bytes count encoded frame plus attribution, not total C++ heap usage.
     * Memory only: expiry/Stop require exact-client-outbox reconciliation. */
    struct LocalActionSource {
        flowmesh::QueuedWireMessage queued;
        uint256 domain, config, action_id;
        flowmesh::ClientNoticeOrigin origin{flowmesh::ClientNoticeOrigin::PUBLIC};
        std::string submitter;
        uint64_t public_serial{0};
        flowmesh::WireClock::time_point deadline{}, next_queue_attempt{};
        size_t bytes{0};
        bool admitted{false}, deferred{false};
    };
    std::map<uint64_t, LocalActionSource> m_local_action_sources;
    size_t m_local_action_source_bytes{0};
    //! Never reset across Stop/Start; zero/overflow refuses before acceptance.
    uint64_t m_next_local_action_source{1}, m_local_action_source_cursor{0};
    std::set<std::pair<flowmesh::WirePeerId, flowmesh::MarketId>> m_receive_recovery;
    std::optional<std::pair<flowmesh::WirePeerId, flowmesh::MarketId>> m_receive_recovery_cursor;
    FlowMeshCommitteeRelayBudget m_delivery_retry_budget{
        FlowMeshCommitteeRelayBudget::GLOBAL_MESSAGES,
        FlowMeshCommitteeRelayBudget::GLOBAL_BYTES};

    //! Worker-owned after Start().
    flowmesh::CatchupRequestTracker m_catchup_tracker;
    struct PendingCatchup {
        uint64_t epoch{0};
        uint64_t from_sequence{0};
        uint16_t max_entries{0};
        uint32_t max_bytes{0};
        flowmesh::WireClock::time_point started{};
        flowmesh::WireClock::time_point deadline{};
    };
    std::map<std::pair<flowmesh::WirePeerId, flowmesh::MarketId>,
             PendingCatchup> m_pending_catchup;
    std::map<std::pair<flowmesh::WirePeerId, flowmesh::MarketId>,
             flowmesh::WireClock::time_point> m_catchup_cooldowns;
    // Local delivery policy only: <=256 profiles, not committee/chain state.
    // Timeout reduces the page count, never the size of a permitted entry.
    struct CatchupProfile {
        uint16_t max_entries{static_cast<uint16_t>(flowmesh::FLOWMESH_CATCHUP_MAX_ENTRIES)};
        flowmesh::WireClock::time_point last_used{};
    };
    std::map<std::pair<flowmesh::WirePeerId, flowmesh::MarketId>,
             CatchupProfile> m_catchup_profiles;
    flowmesh::MarketId m_announcement_cursor;
    flowmesh::WireClock::time_point m_next_announcement_batch{};
    //! One bounded cursor/backoff per peer. Each known market gets a probe
    //! per sweep; newly admitted markets extend an in-progress or idle scan.
    std::vector<flowmesh::MarketId> m_probe_markets;
    struct LegacyPeerProbe {
        size_t market_cursor{0};
        flowmesh::WireClock::time_point next_sweep{};
    };
    std::map<flowmesh::WirePeerId, LegacyPeerProbe> m_peer_probe_cursors;
    flowmesh::WirePeerId m_probe_peer_cursor{0};
    flowmesh::WireClock::time_point m_next_legacy_probe{};
    FlowMeshEvidenceRetryBudget m_evidence_retry_budget;
    flowmesh::MarketId m_evidence_retry_cursor;
    FlowMeshCommitteeRelayBudget m_committee_relay_budget{
        FlowMeshCommitteeRelayBudget::GLOBAL_MESSAGES,
        FlowMeshCommitteeRelayBudget::GLOBAL_BYTES};
    FlowMeshCommitteeRelayBudget m_agreement_relay_budget{
        256, 2 * (flowmesh::AGREEMENT_MAX_BYTES + flowmesh::FLOWMESH_WIRE_HEADER_SIZE)};
    FlowMeshDuplicateActionRelayBudget m_duplicate_action_relay_budget;
    flowmesh::MarketId m_duplicate_action_relay_cursor;
    FlowMeshLocalActionRelayBudget m_local_action_relay_budget;
    flowmesh::MarketId m_local_action_relay_cursor;
};

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_RUNTIME_H
