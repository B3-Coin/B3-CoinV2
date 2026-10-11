// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_HEALTH_H
#define B3COIN_NODE_FLOWMESH_HEALTH_H

#include <flowmesh/market.h>
#include <flowmesh/p2p.h>
#include <flowmesh/seat_evidence.h>
#include <uint256.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

class UniValue;

namespace node {

/**
 * Local, advisory operations vocabulary for one node's FlowMesh service.
 *
 * Nothing here is consensus, a signed or certified field, evidence, or an
 * input to admission, relay, signing, verification, durability or a client's
 * acceptance of data. It describes what this node has locally observed so an
 * operator, a status page or a client can tell a lagging or isolated endpoint
 * from a healthy one. Readers must treat a not-ready code they do not know as
 * not ready.
 */
inline constexpr int FLOWMESH_HEALTH_VERSION{1};

//! Why the service's chain gate (its B3 reconciliation gate) is in its
//! current state. Dormant and stopped services are reported through the
//! service state, never as a gate reason.
enum class FlowMeshGateReason : uint8_t {
    OPEN = 0,
    STARTING,
    INITIAL_BLOCK_DOWNLOAD,
    BELOW_ACTIVATION,
    RECONCILING,
    IDLE_WAIT_TIMEOUT,
    STORE_RECONCILIATION_FAILED,
    MARKET_REFRESH_FAILED,
    CHECKPOINT_RECONCILIATION_FAILED,
    TIP_CHANGED,
};
const char* FlowMeshGateReasonName(FlowMeshGateReason reason);

//! Closed vocabulary of reasons an endpoint reports itself not ready.
enum class FlowMeshNotReady : uint8_t {
    // Service level: they apply to every market this node serves.
    SERVICE_IS_STOPPED = 0,
    INITIAL_BLOCK_DOWNLOAD,
    BELOW_ACTIVATION,
    RECONCILING_PROLONGED,
    WORKER_STALLED,
    NO_FLOWMESH_PEERS,
    // Market level.
    MARKET_NOT_INSTALLED,
    MARKET_HALTED,
    CATCHING_UP,
    //! A signed action has been includable for longer than stall_grace with
    //! no head advance, while the service and the market are otherwise
    //! healthy: certification is not progressing (e.g. no quorum).
    STALLED_WITH_PENDING_WORK,
};
const char* FlowMeshNotReadyName(FlowMeshNotReady reason);

/** Node-local thresholds, never consensus. Only the regtest-only
 * -flowmeshhealthtestgracems changes them. Everything is measured on the
 * local steady clock. */
struct FlowMeshHealthPolicy {
    //! Longer than UpdatedBlockTip's 5 s idle wait plus an ordinary per-block
    //! reconciliation: a gate closed for this long failed or is stuck.
    std::chrono::milliseconds reconcile_grace{10'000};
    //! How long proven or speculative catch-up may last before it is reported.
    std::chrono::milliseconds lag_grace{10'000};
    //! Twenty 250 ms service ticks without a worker observation.
    std::chrono::milliseconds worker_stall{5'000};
    std::chrono::milliseconds no_peers_grace{30'000};
    //! Includable work this long without a head advance is a stall. Far above
    //! a healthy certification (a 2 s round timeout and its view changes).
    std::chrono::milliseconds stall_grace{30'000};
    //! Label only: a head applied within this window is "live", else "idle".
    std::chrono::milliseconds live_window{60'000};

    //! Every grace (not the worker stall or the label window) set to `grace`.
    static FlowMeshHealthPolicy WithGrace(std::chrono::milliseconds grace)
    {
        FlowMeshHealthPolicy out;
        out.reconcile_grace = out.lag_grace = out.no_peers_grace = out.stall_grace = grace;
        return out;
    }
};

//! What the service observed of its chain gate. The gate itself (an atomic
//! flag read by admission) stays the only authority; this is its observation.
struct FlowMeshGateObservation {
    bool closed{false};
    //! Set on an observed open->closed transition; the closure has been
    //! continuous since then as far as this observer can tell.
    std::optional<flowmesh::WireClock::time_point> closed_since;
    FlowMeshGateReason reason{FlowMeshGateReason::OPEN};
};

/**
 * Tracks the chain gate from its authoritative value. Each Observe() call
 * passes the gate's current value and how many times it has opened so far
 * (read after the gate's own stores), so concurrent writers converge to the
 * real gate: the last observation always reflects the gate it read, and a
 * reopening that happened between two observations is never folded into one
 * continuous closure.
 */
class FlowMeshGateObserver
{
public:
    //! `reason` refines the reason while the gate is closed; nullopt keeps
    //! the current one (or RECONCILING for a closure first seen here).
    void Observe(bool closed, uint64_t opens, flowmesh::WireClock::time_point now,
                 std::optional<FlowMeshGateReason> reason = std::nullopt);
    const FlowMeshGateObservation& Get() const { return m_observation; }

private:
    FlowMeshGateObservation m_observation;
    uint64_t m_opens{0};
};

struct FlowMeshLagSummary {
    //! Behind (proven or speculative) continuously since. A label only.
    std::optional<flowmesh::WireClock::time_point> behind_since;
    //! Provably behind continuously since: the only lag that decides
    //! readiness (catching_up).
    std::optional<flowmesh::WireClock::time_point> proven_since;
    //! One past the highest BLS-verified certificate this node holds for a
    //! sequence it has not applied yet. Cannot be forged or hidden: it clears
    //! only when this node's own head passes it.
    std::optional<uint64_t> proven_target;
    //! A verified catch-up page arrived without tail slack, so the serving
    //! peer may hold more. Not a proof and never a readiness input: it ends
    //! on a tail-slack page, on that peer announcing a head at or below ours,
    //! or after SPECULATIVE_TTL.
    bool speculative{false};
    //! Unverified HELLO hints, display only; they never decide readiness.
    std::optional<uint64_t> peer_hint_next_sequence;
    size_t peers_hinting_ahead{0};
};

/**
 * Per-market catch-up observation owned by the runtime worker. No field here
 * changes a relay, catch-up, verification or signing decision.
 *
 * Proven lag starts only from a BLS-verified certificate for a future
 * sequence and ends only when the local head passes it; no commit path,
 * announcement or timeout clears it. Speculative lag starts from an applied
 * catch-up page without tail slack. A live commit never ends either kind:
 * deferred catch-up certificates are retried through the live path, and an
 * old certificate relayed for our next sequence must not hide a lag. Only
 * proven lag decides readiness; speculative lag and hints are shown. Start
 * times are kept per tracker and recomputed after every change, so
 * page-by-page progress never restarts them. Bounded: at most MAX_HINTS
 * hints and MAX_SPECULATIVE_PEERS peers.
 */
class FlowMeshLagTracker
{
public:
    using Clock = flowmesh::WireClock;
    static constexpr size_t MAX_HINTS{8};
    static constexpr size_t MAX_SPECULATIVE_PEERS{8};
    static constexpr std::chrono::seconds HINT_TTL{30};
    static constexpr std::chrono::seconds SPECULATIVE_TTL{30};

    explicit FlowMeshLagTracker(uint64_t local_next = 0) : m_local_next{local_next} {}

    //! A BLS-verified certificate for `sequence`, which is at or above the
    //! local next sequence.
    void Proven(uint64_t sequence, Clock::time_point now);
    //! A catch-up page from `peer` was applied without tail slack.
    void Speculative(flowmesh::WirePeerId peer, Clock::time_point now);
    //! A catch-up page was applied with tail slack.
    void SpeculativeEnd(Clock::time_point now);
    //! An unverified HELLO: `peer` announces `announced_next`.
    void Hello(flowmesh::WirePeerId peer, uint64_t announced_next, Clock::time_point now);
    //! The local head now ends just before `local_next`.
    void LocalAdvanced(uint64_t local_next, Clock::time_point now);
    void RemovePeer(flowmesh::WirePeerId peer);
    void Expire(Clock::time_point now);

    bool Behind() const;
    bool ProvenBehind() const;
    FlowMeshLagSummary Summary() const;

private:
    struct Hint {
        uint64_t target{0};
        Clock::time_point seen{};
    };
    void EndSpeculative();
    void Update(Clock::time_point now);

    uint64_t m_local_next;
    std::optional<uint64_t> m_proven_next;
    bool m_speculative{false};
    Clock::time_point m_speculative_seen{};
    std::set<flowmesh::WirePeerId> m_speculative_peers;
    std::map<flowmesh::WirePeerId, Hint> m_hints;
    std::optional<Clock::time_point> m_behind_since;
    std::optional<Clock::time_point> m_proven_since;
};

//! One market as the runtime worker last published it. Values only.
struct FlowMeshMarketHealthObservation {
    flowmesh::MarketId market_id;
    //! False for an explicit insufficient-seats shell.
    bool installed{true};
    uint64_t epoch{0};
    uint64_t next_sequence{0};
    uint256 last_hash;
    size_t active_seats{0};
    size_t quorum_required{0};
    size_t pending_actions{0};
    bool paused{false};
    bool pending_handoff{false};
    //! Fixed diagnostic literal, or empty.
    std::string pause_reason;
    bool halted{false};
    std::string halt{"none"};
    //! Operator detail only; bounded.
    std::string error;
    std::string proposal_wait_reason;
    //! When this process last applied a head (local steady clock).
    std::optional<flowmesh::WireClock::time_point> head_advanced_at;
    //! Since when the pool has continuously held an includable signed action
    //! (local steady clock); empty while it holds none. Observation only.
    std::optional<flowmesh::WireClock::time_point> eligible_work_since;
    //! Existing local wall-clock observation of the head, display only.
    std::optional<int64_t> local_observed_at;
    FlowMeshLagSummary lag;
    //! Seat-misbehaviour evidence this node recorded for the market; an
    //! observation like everything here, never an input to anything. Records
    //! ever inserted into the market's store, and those it retains (newest
    //! first; each self-verifiable with flowmesh::CheckSeatEvidence).
    uint64_t seat_evidence_recorded{0};
    std::shared_ptr<const std::vector<flowmesh::SeatEvidence>> seat_evidence;
    //! Detections this process could not record: not verifiable against the
    //! local chain's seat sets, over the per-minute check pace (applied
    //! before any verification), or a failed write.
    uint64_t seat_evidence_unrecorded{0};
};

struct FlowMeshRuntimeHealthSnapshot {
    flowmesh::WireClock::time_point published_at{};
    size_t connected_peers{0};
    std::optional<flowmesh::WireClock::time_point> no_peers_since;
    //! MarketId order.
    std::vector<FlowMeshMarketHealthObservation> markets;
};

struct FlowMeshMarketReadiness {
    bool ready{false};
    std::vector<FlowMeshNotReady> not_ready;
    const char* state{"unknown"};
};

enum class FlowMeshServiceState : uint8_t {
    //! The A2/A3 schedule is incomplete; the service never starts.
    DORMANT = 0,
    STOPPED,
    RUNNING,
};
const char* FlowMeshServiceStateName(FlowMeshServiceState state);

//! Upper bound of the regtest-only -flowmeshhealthtestgracems.
inline constexpr std::chrono::milliseconds FLOWMESH_TEST_HEALTH_GRACE_MAX{60'000};

//! At most this many market rows are evaluated and reported.
inline constexpr size_t FLOWMESH_HEALTH_MAX_MARKETS{256};
//! Seat-evidence records per market in the public object (newest first);
//! operator detail lists every retained record.
inline constexpr size_t FLOWMESH_HEALTH_PUBLIC_SEAT_EVIDENCE{4};

/** One evaluated health observation of a service and its markets. */
struct FlowMeshServiceHealth {
    FlowMeshServiceState state{FlowMeshServiceState::STOPPED};
    FlowMeshGateObservation gate;
    FlowMeshHealthPolicy policy;
    flowmesh::WireClock::time_point now{};
    //! Keeps every row's observation alive.
    std::shared_ptr<const FlowMeshRuntimeHealthSnapshot> snapshot;
    std::vector<FlowMeshNotReady> service_not_ready;
    struct Row {
        flowmesh::MarketId market_id;
        //! Null when the runtime has not installed this service market.
        const FlowMeshMarketHealthObservation* observation{nullptr};
        FlowMeshMarketReadiness readiness;
    };
    std::vector<Row> rows;
    bool truncated{false};

    bool ServiceReady() const { return service_not_ready.empty(); }
    //! Service ready and every reported market ready (a label only: market
    //! rows decide per-market routing).
    bool Ready() const;
    const Row* Find(const flowmesh::MarketId& market) const;
};

std::vector<FlowMeshNotReady> EvaluateFlowMeshServiceReadiness(
    FlowMeshServiceState state, const FlowMeshGateObservation& gate,
    const FlowMeshRuntimeHealthSnapshot* snapshot,
    flowmesh::WireClock::time_point now, const FlowMeshHealthPolicy& policy);

FlowMeshMarketReadiness EvaluateFlowMeshMarketReadiness(
    const std::vector<FlowMeshNotReady>& service_not_ready,
    const FlowMeshMarketHealthObservation* market,
    flowmesh::WireClock::time_point now, const FlowMeshHealthPolicy& policy);

/** Evaluate `snapshot` against the service's own view. Rows are the union of
 * the service's market ids and the snapshot's markets, in MarketId order and
 * at most FLOWMESH_HEALTH_MAX_MARKETS; `only` limits them to one market. */
FlowMeshServiceHealth BuildFlowMeshServiceHealth(
    FlowMeshServiceState state, const FlowMeshGateObservation& gate,
    const FlowMeshHealthPolicy& policy, flowmesh::WireClock::time_point now,
    std::shared_ptr<const FlowMeshRuntimeHealthSnapshot> snapshot,
    const std::set<flowmesh::MarketId>& service_markets,
    const std::optional<flowmesh::MarketId>& only = std::nullopt);

/** The versioned health object. Absent values are omitted, never null. The
 * public object carries no free text, key material or path; operator detail
 * (local RPC only) adds bounded error text, the proposer wait reason, peer
 * counts, unverified peer head hints, every retained seat-evidence record
 * and the count of detections not recorded. */
UniValue FlowMeshServiceHealthJson(const FlowMeshServiceHealth& health, bool operator_detail);

//! One seat-evidence record with every field needed to check it.
UniValue FlowMeshSeatEvidenceJson(const flowmesh::SeatEvidence& evidence);

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_HEALTH_H
