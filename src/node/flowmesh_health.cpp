// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_health.h>

#include <univalue.h>

#include <algorithm>
#include <limits>

namespace node {
namespace {

int64_t ElapsedMs(const flowmesh::WireClock::time_point since, const flowmesh::WireClock::time_point now)
{
    if (now <= since) return 0;
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - since).count();
}

bool Contains(const std::vector<FlowMeshNotReady>& reasons, const FlowMeshNotReady reason)
{
    return std::find(reasons.begin(), reasons.end(), reason) != reasons.end();
}

void Add(std::vector<FlowMeshNotReady>& reasons, const FlowMeshNotReady reason)
{
    if (!Contains(reasons, reason)) reasons.push_back(reason);
}

UniValue Codes(const std::vector<FlowMeshNotReady>& reasons)
{
    UniValue out{UniValue::VARR};
    for (const auto reason : reasons) out.push_back(FlowMeshNotReadyName(reason));
    return out;
}

// The normal per-block pause while the chain gate is closed; the gate's own
// observation reports a closure that lasts.
constexpr const char* B3_RECONCILIATION_PAUSE{"b3_reconciliation_pending"};

} // namespace

const char* FlowMeshGateReasonName(const FlowMeshGateReason reason)
{
    switch (reason) {
    case FlowMeshGateReason::OPEN: return "open";
    case FlowMeshGateReason::STARTING: return "starting";
    case FlowMeshGateReason::INITIAL_BLOCK_DOWNLOAD: return "initial_block_download";
    case FlowMeshGateReason::BELOW_ACTIVATION: return "below_activation";
    case FlowMeshGateReason::RECONCILING: return "reconciling";
    case FlowMeshGateReason::IDLE_WAIT_TIMEOUT: return "idle_wait_timeout";
    case FlowMeshGateReason::STORE_RECONCILIATION_FAILED: return "store_reconciliation_failed";
    case FlowMeshGateReason::MARKET_REFRESH_FAILED: return "market_refresh_failed";
    case FlowMeshGateReason::CHECKPOINT_RECONCILIATION_FAILED: return "checkpoint_reconciliation_failed";
    case FlowMeshGateReason::TIP_CHANGED: return "tip_changed";
    }
    return "unknown";
}

const char* FlowMeshNotReadyName(const FlowMeshNotReady reason)
{
    switch (reason) {
    case FlowMeshNotReady::SERVICE_STOPPED: return "service_stopped";
    case FlowMeshNotReady::INITIAL_BLOCK_DOWNLOAD: return "initial_block_download";
    case FlowMeshNotReady::BELOW_ACTIVATION: return "below_activation";
    case FlowMeshNotReady::RECONCILING_PROLONGED: return "reconciling_prolonged";
    case FlowMeshNotReady::WORKER_STALLED: return "worker_stalled";
    case FlowMeshNotReady::NO_FLOWMESH_PEERS: return "no_flowmesh_peers";
    case FlowMeshNotReady::MARKET_NOT_INSTALLED: return "market_not_installed";
    case FlowMeshNotReady::MARKET_HALTED: return "market_halted";
    case FlowMeshNotReady::CATCHING_UP: return "catching_up";
    case FlowMeshNotReady::STALLED_WITH_PENDING_WORK: return "stalled_with_pending_work";
    }
    return "unknown";
}

const char* FlowMeshServiceStateName(const FlowMeshServiceState state)
{
    switch (state) {
    case FlowMeshServiceState::DORMANT: return "dormant";
    case FlowMeshServiceState::STOPPED: return "stopped";
    case FlowMeshServiceState::RUNNING: return "running";
    }
    return "unknown";
}

void FlowMeshGateObserver::Observe(const bool closed, const uint64_t opens,
                                   const flowmesh::WireClock::time_point now,
                                   const std::optional<FlowMeshGateReason> reason)
{
    // A count read before a concurrent writer's increment is older, never a
    // new opening; a count that moved means the gate opened in between.
    const bool reopened{opens > m_opens};
    m_opens = std::max(m_opens, opens);
    if (!closed) {
        m_observation = {};
        return;
    }
    // "open" is never a reason for a closed gate (a writer that opened it
    // may observe it after another writer closed it again).
    const auto refined{reason && *reason != FlowMeshGateReason::OPEN ? reason : std::nullopt};
    if (!m_observation.closed || reopened || !m_observation.closed_since) {
        m_observation.closed = true;
        m_observation.closed_since = now;
        m_observation.reason = refined.value_or(FlowMeshGateReason::RECONCILING);
    } else if (refined) {
        m_observation.reason = *refined;
    }
}

void FlowMeshLagTracker::Proven(const uint64_t sequence, const Clock::time_point now)
{
    if (sequence >= m_local_next) {
        const uint64_t target{sequence == std::numeric_limits<uint64_t>::max() ? sequence : sequence + 1};
        m_proven_next = std::max(m_proven_next.value_or(0), target);
    }
    Update(now);
}

void FlowMeshLagTracker::Speculative(const flowmesh::WirePeerId peer, const Clock::time_point now)
{
    m_speculative = true;
    m_speculative_seen = now;
    if (m_speculative_peers.size() < MAX_SPECULATIVE_PEERS) m_speculative_peers.insert(peer);
    Update(now);
}

void FlowMeshLagTracker::SpeculativeEnd(const Clock::time_point now)
{
    EndSpeculative();
    Update(now);
}

void FlowMeshLagTracker::Hello(const flowmesh::WirePeerId peer, const uint64_t announced_next,
                               const Clock::time_point now)
{
    if (announced_next <= m_local_next) {
        m_hints.erase(peer);
        // Only a peer that served the speculative pages can end them this
        // way; an arbitrary peer cannot hide a lag. Proven lag is unaffected.
        if (m_speculative && m_speculative_peers.contains(peer)) EndSpeculative();
    } else if (const auto it{m_hints.find(peer)}; it != m_hints.end()) {
        it->second = {announced_next, now};
    } else if (m_hints.size() < MAX_HINTS) {
        m_hints.emplace(peer, Hint{announced_next, now});
    }
    Update(now);
}

void FlowMeshLagTracker::LocalAdvanced(const uint64_t local_next, const Clock::time_point now)
{
    m_local_next = local_next;
    if (m_proven_next && *m_proven_next <= m_local_next) m_proven_next.reset();
    std::erase_if(m_hints, [&](const auto& item) { return item.second.target <= m_local_next; });
    Update(now);
}

void FlowMeshLagTracker::RemovePeer(const flowmesh::WirePeerId peer)
{
    m_hints.erase(peer);
    m_speculative_peers.erase(peer);
}

void FlowMeshLagTracker::Expire(const Clock::time_point now)
{
    std::erase_if(m_hints, [&](const auto& item) { return now - item.second.seen > HINT_TTL; });
    if (m_speculative && now - m_speculative_seen > SPECULATIVE_TTL) EndSpeculative();
    Update(now);
}

bool FlowMeshLagTracker::ProvenBehind() const
{
    return m_proven_next && *m_proven_next > m_local_next;
}

bool FlowMeshLagTracker::Behind() const
{
    return ProvenBehind() || m_speculative;
}

FlowMeshLagSummary FlowMeshLagTracker::Summary() const
{
    FlowMeshLagSummary out;
    out.behind_since = m_behind_since;
    out.proven_since = m_proven_since;
    if (ProvenBehind()) out.proven_target = m_proven_next;
    out.speculative = m_speculative;
    for (const auto& [peer, hint] : m_hints) {
        (void)peer;
        if (hint.target <= m_local_next) continue;
        ++out.peers_hinting_ahead;
        out.peer_hint_next_sequence = std::max(out.peer_hint_next_sequence.value_or(0), hint.target);
    }
    return out;
}

void FlowMeshLagTracker::EndSpeculative()
{
    m_speculative = false;
    m_speculative_peers.clear();
}

void FlowMeshLagTracker::Update(const Clock::time_point now)
{
    const auto edge = [&](const bool behind, std::optional<Clock::time_point>& since) {
        if (!behind) {
            since.reset();
        } else if (!since) {
            since = now;
        }
    };
    edge(Behind(), m_behind_since);
    edge(ProvenBehind(), m_proven_since);
}

bool FlowMeshServiceHealth::Ready() const
{
    return ServiceReady() && std::all_of(rows.begin(), rows.end(), [](const Row& row) { return row.readiness.ready; });
}

const FlowMeshServiceHealth::Row* FlowMeshServiceHealth::Find(const flowmesh::MarketId& market) const
{
    const auto it{std::find_if(rows.begin(), rows.end(), [&](const Row& row) { return row.market_id == market; })};
    return it == rows.end() ? nullptr : &*it;
}

std::vector<FlowMeshNotReady> EvaluateFlowMeshServiceReadiness(
    const FlowMeshServiceState state, const FlowMeshGateObservation& gate,
    const FlowMeshRuntimeHealthSnapshot* snapshot,
    const flowmesh::WireClock::time_point now, const FlowMeshHealthPolicy& policy)
{
    std::vector<FlowMeshNotReady> out;
    if (state != FlowMeshServiceState::RUNNING) {
        Add(out, FlowMeshNotReady::SERVICE_STOPPED);
        return out;
    }
    if (gate.closed) {
        switch (gate.reason) {
        case FlowMeshGateReason::INITIAL_BLOCK_DOWNLOAD:
            Add(out, FlowMeshNotReady::INITIAL_BLOCK_DOWNLOAD);
            break;
        case FlowMeshGateReason::BELOW_ACTIVATION:
            Add(out, FlowMeshNotReady::BELOW_ACTIVATION);
            break;
        default:
            // A closure of unknown start cannot be called brief.
            if (!gate.closed_since || now - *gate.closed_since >= policy.reconcile_grace) {
                Add(out, FlowMeshNotReady::RECONCILING_PROLONGED);
            }
            break;
        }
    }
    if (!snapshot || now - snapshot->published_at >= policy.worker_stall) {
        Add(out, FlowMeshNotReady::WORKER_STALLED);
    }
    if (snapshot && snapshot->connected_peers == 0 &&
        (!snapshot->no_peers_since || now - *snapshot->no_peers_since >= policy.no_peers_grace)) {
        Add(out, FlowMeshNotReady::NO_FLOWMESH_PEERS);
    }
    return out;
}

FlowMeshMarketReadiness EvaluateFlowMeshMarketReadiness(
    const std::vector<FlowMeshNotReady>& service_not_ready,
    const FlowMeshMarketHealthObservation* market,
    const flowmesh::WireClock::time_point now, const FlowMeshHealthPolicy& policy)
{
    FlowMeshMarketReadiness out;
    out.not_ready = service_not_ready;
    if (!market) {
        Add(out.not_ready, FlowMeshNotReady::MARKET_NOT_INSTALLED);
    } else {
        if (market->halted) Add(out.not_ready, FlowMeshNotReady::MARKET_HALTED);
        // Only locally proven lag: never a speculative page or a hint.
        if (market->lag.proven_since && now - *market->lag.proven_since >= policy.lag_grace) {
            Add(out.not_ready, FlowMeshNotReady::CATCHING_UP);
        }
    }
    out.ready = out.not_ready.empty();
    const auto has = [&](const FlowMeshNotReady reason) { return Contains(out.not_ready, reason); };
    if (has(FlowMeshNotReady::SERVICE_STOPPED)) {
        out.state = "stopped";
    } else if (!market) {
        out.state = "not_installed";
    } else if (market->halted) {
        out.state = "halted";
    } else if (has(FlowMeshNotReady::INITIAL_BLOCK_DOWNLOAD) || has(FlowMeshNotReady::BELOW_ACTIVATION)) {
        out.state = "inactive";
    } else if (has(FlowMeshNotReady::RECONCILING_PROLONGED)) {
        out.state = "reconciling";
    } else if (has(FlowMeshNotReady::WORKER_STALLED)) {
        out.state = "worker_stalled";
    } else if (has(FlowMeshNotReady::NO_FLOWMESH_PEERS)) {
        out.state = "isolated";
    } else if (!market->installed) {
        out.state = "insufficient_seats";
    } else if (market->pending_handoff) {
        out.state = "handoff_pending";
    } else if (market->paused && market->pause_reason != B3_RECONCILIATION_PAUSE) {
        out.state = "paused";
    } else if (has(FlowMeshNotReady::STALLED_WITH_PENDING_WORK)) {
        out.state = "stalled";
    } else if (market->lag.behind_since) {
        out.state = "catching_up";
    } else if (market->head_advanced_at && now - *market->head_advanced_at <= policy.live_window) {
        out.state = "live";
    } else {
        out.state = "idle";
    }
    return out;
}

FlowMeshServiceHealth BuildFlowMeshServiceHealth(
    const FlowMeshServiceState state, const FlowMeshGateObservation& gate,
    const FlowMeshHealthPolicy& policy, const flowmesh::WireClock::time_point now,
    std::shared_ptr<const FlowMeshRuntimeHealthSnapshot> snapshot,
    const std::set<flowmesh::MarketId>& service_markets,
    const std::optional<flowmesh::MarketId>& only)
{
    FlowMeshServiceHealth out;
    out.state = state;
    out.gate = gate;
    out.policy = policy;
    out.now = now;
    out.snapshot = std::move(snapshot);
    out.service_not_ready = EvaluateFlowMeshServiceReadiness(state, gate, out.snapshot.get(), now, policy);
    std::map<flowmesh::MarketId, const FlowMeshMarketHealthObservation*> markets;
    for (const auto& id : service_markets) markets.emplace(id, nullptr);
    if (out.snapshot) {
        for (const auto& market : out.snapshot->markets) markets.insert_or_assign(market.market_id, &market);
    }
    for (const auto& [id, observation] : markets) {
        if (only && id != *only) continue;
        if (out.rows.size() == FLOWMESH_HEALTH_MAX_MARKETS) {
            out.truncated = true;
            break;
        }
        out.rows.push_back({id, observation,
                            EvaluateFlowMeshMarketReadiness(out.service_not_ready, observation, now, policy)});
    }
    return out;
}

UniValue FlowMeshServiceHealthJson(const FlowMeshServiceHealth& health, const bool operator_detail)
{
    const auto now{health.now};
    UniValue out{UniValue::VOBJ};
    out.pushKV("health_version", FLOWMESH_HEALTH_VERSION);
    // Never certified evidence or a proof of the network's freshest head.
    out.pushKV("observation", "local-node");
    out.pushKV("ready", health.Ready());
    out.pushKV("service_ready", health.ServiceReady());
    out.pushKV("not_ready", Codes(health.service_not_ready));

    UniValue service{UniValue::VOBJ};
    service.pushKV("state", FlowMeshServiceStateName(health.state));
    if (health.state == FlowMeshServiceState::RUNNING) {
        service.pushKV("chain_gate", health.gate.closed ? "closed" : "open");
        service.pushKV("chain_gate_reason", FlowMeshGateReasonName(health.gate.reason));
        if (health.gate.closed && health.gate.closed_since) {
            service.pushKV("chain_gate_closed_ms", ElapsedMs(*health.gate.closed_since, now));
        }
        if (health.snapshot) {
            service.pushKV("worker_observation_age_ms", ElapsedMs(health.snapshot->published_at, now));
            if (operator_detail) {
                service.pushKV("flowmesh_peers", uint64_t{health.snapshot->connected_peers});
                if (health.snapshot->connected_peers == 0 && health.snapshot->no_peers_since) {
                    service.pushKV("no_peers_ms", ElapsedMs(*health.snapshot->no_peers_since, now));
                }
            }
        }
    }
    out.pushKV("service", std::move(service));

    const auto& p{health.policy};
    UniValue policy{UniValue::VOBJ};
    policy.pushKV("reconcile_grace_ms", static_cast<int64_t>(p.reconcile_grace.count()));
    policy.pushKV("lag_grace_ms", static_cast<int64_t>(p.lag_grace.count()));
    policy.pushKV("worker_stall_ms", static_cast<int64_t>(p.worker_stall.count()));
    policy.pushKV("no_peers_grace_ms", static_cast<int64_t>(p.no_peers_grace.count()));
    policy.pushKV("live_window_ms", static_cast<int64_t>(p.live_window.count()));
    out.pushKV("policy", std::move(policy));

    UniValue markets{UniValue::VARR};
    for (const auto& row : health.rows) {
        UniValue item{UniValue::VOBJ};
        item.pushKV("market_id", row.market_id.GetHex());
        item.pushKV("ready", row.readiness.ready);
        item.pushKV("not_ready", Codes(row.readiness.not_ready));
        item.pushKV("state", row.readiness.state);
        if (const auto* market{row.observation}) {
            item.pushKV("halt", market->halt);
            item.pushKV("paused", market->paused);
            item.pushKV("pause_reason", market->pause_reason);
            item.pushKV("pending_handoff", market->pending_handoff);
            item.pushKV("epoch", market->epoch);
            item.pushKV("active_seats", uint64_t{market->active_seats});
            item.pushKV("quorum_required", uint64_t{market->quorum_required});
            item.pushKV("pending_actions", uint64_t{market->pending_actions});
            item.pushKV("next_microblock_sequence", market->next_sequence);
            item.pushKV("last_microblock_hash", market->last_hash.GetHex());
            if (market->head_advanced_at) item.pushKV("head_age_ms", ElapsedMs(*market->head_advanced_at, now));
            if (market->local_observed_at) item.pushKV("head_local_observed_unix", *market->local_observed_at);
            UniValue catchup{UniValue::VOBJ};
            catchup.pushKV("behind", market->lag.behind_since.has_value());
            if (market->lag.behind_since) catchup.pushKV("behind_ms", ElapsedMs(*market->lag.behind_since, now));
            if (market->lag.proven_target) catchup.pushKV("proven_target", *market->lag.proven_target);
            if (market->lag.proven_since) catchup.pushKV("proven_behind_ms", ElapsedMs(*market->lag.proven_since, now));
            catchup.pushKV("speculative", market->lag.speculative);
            if (operator_detail) {
                if (market->lag.peer_hint_next_sequence) {
                    catchup.pushKV("peer_hint_next_sequence", *market->lag.peer_hint_next_sequence);
                }
                catchup.pushKV("peers_hinting_ahead", uint64_t{market->lag.peers_hinting_ahead});
            }
            item.pushKV("catchup", std::move(catchup));
            if (operator_detail) {
                item.pushKV("error", market->error);
                item.pushKV("proposal_wait_reason", market->proposal_wait_reason);
            }
        }
        markets.push_back(std::move(item));
    }
    out.pushKV("markets", std::move(markets));
    out.pushKV("markets_truncated", health.truncated);
    return out;
}

} // namespace node
