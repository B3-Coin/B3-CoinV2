// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_RUNTIME_H
#define B3COIN_NODE_FLOWMESH_RUNTIME_H

#include <flowmesh/p2p.h>
#include <flowmesh/production_wire.h>
#include <node/flowmesh_production_store.h>

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
};

using FlowMeshRuntimeRelayFn =
    std::function<void(FlowMeshRuntimeRelay relay)>;

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
    /** A discovered live market may exist before a valid k>=4 set exists. */
    FlowMeshRuntimeMarketReadiness readiness{
        FlowMeshRuntimeMarketReadiness::READY};
};

struct FlowMeshRuntimeConfig {
    FlowMeshRuntimeChain* chain{nullptr};
    FlowMeshRuntimeKeyProvider* keys{nullptr};
    FlowMeshRuntimeClock* clock{nullptr};
    FlowMeshRuntimeRelayFn relay;

    //! Policy only. It selects the next proposer round but is never serialized
    //! into a consensus parameter. A fully validated signed proposal may pull
    //! a receiver forward by one round to reconcile local timer skew.
    std::chrono::milliseconds round_timeout{std::chrono::seconds{2}};
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

    bool Start(std::string& error);
    void Stop();

    flowmesh::QueueResult EnqueueWireMessage(
        flowmesh::WirePeerId peer,
        flowmesh::WireMessage message) override;
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

    std::optional<FlowMeshRuntimeMarketStatus> MarketStatus(
        const flowmesh::MarketId& market_id) const;
    std::optional<flowmesh::FlowMeshState> StateSnapshot(
        const flowmesh::MarketId& market_id) const;

    /** Test/shutdown aid: waits only for this runtime's current work queue. */
    bool WaitForIdle(std::chrono::milliseconds timeout);

private:
    struct Market;
    struct CatchupCommand {
        flowmesh::WirePeerId peer{0};
        flowmesh::MarketId market_id;
    };
    struct AddMarketCommand {
        FlowMeshRuntimeMarketConfig market;
        std::shared_ptr<std::promise<std::pair<bool, std::string>>> completion;
    };

    bool InitializeMarkets(std::string& error);
    bool InitializeMarket(const FlowMeshRuntimeMarketConfig& config,
                          std::string& error);
    void WorkerLoop();
    void ProcessMessage(const flowmesh::QueuedWireMessage& queued);
    void ProcessTick();
    void ProcessCatchupCommand(const CatchupCommand& command);
    void ProcessAddMarketCommand(AddMarketCommand command);
    void RemovePeerOnWorker(flowmesh::WirePeerId peer);

    void HandleAction(Market& market, flowmesh::WirePeerId peer,
                      const flowmesh::WireMessage& message);
    void HandleProposal(Market& market, flowmesh::WirePeerId peer,
                        const flowmesh::WireMessage& message);
    void HandleAttestation(Market& market, flowmesh::WirePeerId peer,
                           const flowmesh::WireMessage& message);
    void HandleCertificate(Market& market, flowmesh::WirePeerId peer,
                           const flowmesh::WireMessage& message,
                           bool from_catchup = false);
    void HandleGet(Market& market, flowmesh::WirePeerId peer,
                   const flowmesh::WireMessage& message);
    void HandleEntries(Market& market, flowmesh::WirePeerId peer,
                       const flowmesh::WireMessage& message);
    void MaybePropose(Market& market);
    void MaybeCertify(Market& market, const uint256& candidate_hash);

    FlowMeshRuntimeConfig m_config;
    std::vector<FlowMeshRuntimeMarketConfig> m_market_configs;

    mutable std::mutex m_market_mutex;
    std::map<flowmesh::MarketId, std::unique_ptr<Market>> m_markets;

    std::mutex m_queue_mutex;
    std::condition_variable m_work_cv;
    std::condition_variable m_idle_cv;
    flowmesh::BoundedWireQueue m_queue;
    //! Ready market ids admitted to m_queue; guarded by m_queue_mutex.
    std::set<flowmesh::MarketId> m_admitted_markets;
    std::deque<flowmesh::WirePeerId> m_removed_peers;
    std::deque<CatchupCommand> m_catchup_commands;
    std::deque<AddMarketCommand> m_add_market_commands;
    bool m_tick_pending{false};
    bool m_started{false};
    bool m_stopping{false};
    bool m_processing{false};
    std::thread m_worker;

    //! Worker-owned after Start().
    flowmesh::CatchupRequestTracker m_catchup_tracker;
    struct PendingCatchup {
        uint64_t epoch{0};
        uint64_t from_sequence{0};
        uint16_t max_entries{0};
        uint32_t max_bytes{0};
    };
    std::map<std::pair<flowmesh::WirePeerId, flowmesh::MarketId>,
             PendingCatchup> m_pending_catchup;
};

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_RUNTIME_H
