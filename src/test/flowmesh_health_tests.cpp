// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <node/flowmesh_health.h>

#include <univalue.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
using node::FlowMeshGateReason;
using node::FlowMeshNotReady;
using Clock = flowmesh::WireClock;

const Clock::time_point T0{std::chrono::seconds{1000}};

uint256 Id(const uint8_t value) { return uint256{value}; }

std::vector<std::string> Names(const std::vector<FlowMeshNotReady>& reasons)
{
    std::vector<std::string> out;
    for (const auto reason : reasons) out.emplace_back(node::FlowMeshNotReadyName(reason));
    return out;
}

std::vector<std::string> Names(const UniValue& array)
{
    std::vector<std::string> out;
    for (const auto& value : array.getValues()) out.push_back(value.get_str());
    return out;
}

std::set<std::string> Keys(const UniValue& object)
{
    const auto keys{object.getKeys()};
    return {keys.begin(), keys.end()};
}

//! No value anywhere in the tree is null: absent values are omitted.
void CheckNoNull(const UniValue& value)
{
    BOOST_CHECK(!value.isNull());
    if (!value.isObject() && !value.isArray()) return;
    for (const auto& child : value.getValues()) CheckNoNull(child);
}

node::FlowMeshMarketHealthObservation Market(const uint8_t id)
{
    node::FlowMeshMarketHealthObservation out;
    out.market_id = Id(id);
    out.epoch = 3;
    out.next_sequence = 9;
    out.last_hash = Id(0x99);
    out.active_seats = 4;
    out.quorum_required = 3;
    return out;
}

std::shared_ptr<node::FlowMeshRuntimeHealthSnapshot> Snapshot(Clock::time_point published = T0)
{
    auto out{std::make_shared<node::FlowMeshRuntimeHealthSnapshot>()};
    out->published_at = published;
    out->connected_peers = 3;
    return out;
}

node::FlowMeshGateObservation Open() { return {}; }
node::FlowMeshGateObservation Closed(const FlowMeshGateReason reason, const Clock::time_point since)
{
    return {true, since, reason};
}
} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_health_tests)

BOOST_AUTO_TEST_CASE(closed_vocabulary_names_are_stable)
{
    // Clients and status pages match these exact strings.
    const std::vector<std::string> not_ready{
        "service_stopped", "initial_block_download", "below_activation", "reconciling_prolonged",
        "worker_stalled", "no_flowmesh_peers", "market_not_installed", "market_halted", "catching_up",
        "stalled_with_pending_work"};
    for (size_t i{0}; i < not_ready.size(); ++i) {
        BOOST_CHECK_EQUAL(node::FlowMeshNotReadyName(static_cast<FlowMeshNotReady>(i)), not_ready[i]);
    }
    const std::vector<std::string> gate{
        "open", "starting", "initial_block_download", "below_activation", "reconciling", "idle_wait_timeout",
        "store_reconciliation_failed", "market_refresh_failed", "checkpoint_reconciliation_failed", "tip_changed"};
    for (size_t i{0}; i < gate.size(); ++i) {
        BOOST_CHECK_EQUAL(node::FlowMeshGateReasonName(static_cast<FlowMeshGateReason>(i)), gate[i]);
    }
    BOOST_CHECK_EQUAL(node::FlowMeshServiceStateName(node::FlowMeshServiceState::DORMANT), "dormant");
    BOOST_CHECK_EQUAL(node::FlowMeshServiceStateName(node::FlowMeshServiceState::STOPPED), "stopped");
    BOOST_CHECK_EQUAL(node::FlowMeshServiceStateName(node::FlowMeshServiceState::RUNNING), "running");
    BOOST_CHECK_EQUAL(node::FLOWMESH_HEALTH_VERSION, 1);
}

BOOST_AUTO_TEST_CASE(gate_observer_follows_the_real_gate_and_its_continuous_closure)
{
    node::FlowMeshGateObserver gate;
    BOOST_CHECK(!gate.Get().closed);
    gate.Observe(true, 0, T0, FlowMeshGateReason::STARTING);
    BOOST_CHECK(gate.Get().closed);
    BOOST_CHECK(gate.Get().closed_since == T0);
    BOOST_CHECK(gate.Get().reason == FlowMeshGateReason::STARTING);
    // A still-closed gate refines its reason and keeps its start.
    gate.Observe(true, 0, T0 + 1s, FlowMeshGateReason::IDLE_WAIT_TIMEOUT);
    BOOST_CHECK(gate.Get().closed_since == T0);
    BOOST_CHECK(gate.Get().reason == FlowMeshGateReason::IDLE_WAIT_TIMEOUT);
    // A closure first seen by a reader has a generic reason.
    gate.Observe(true, 0, T0 + 2s);
    BOOST_CHECK(gate.Get().reason == FlowMeshGateReason::IDLE_WAIT_TIMEOUT);
    gate.Observe(false, 1, T0 + 3s);
    BOOST_CHECK(!gate.Get().closed);
    BOOST_CHECK(!gate.Get().closed_since);
    BOOST_CHECK(gate.Get().reason == FlowMeshGateReason::OPEN);
    gate.Observe(true, 1, T0 + 4s);
    BOOST_CHECK(gate.Get().closed_since == T0 + 4s);
    BOOST_CHECK(gate.Get().reason == FlowMeshGateReason::RECONCILING);

    // Two writers: A opens the gate (opens 1 -> 2), B closes it again, then
    // B observes before A. The opening between them is not folded into one
    // continuous closure, and A's late observation of the closed gate never
    // records it as open or with the reason "open".
    gate.Observe(true, 2, T0 + 5s, FlowMeshGateReason::RECONCILING); // B
    BOOST_CHECK(gate.Get().closed);
    BOOST_CHECK(gate.Get().closed_since == T0 + 5s);
    gate.Observe(true, 2, T0 + 6s, FlowMeshGateReason::OPEN); // A
    BOOST_CHECK(gate.Get().closed);
    BOOST_CHECK(gate.Get().closed_since == T0 + 5s);
    BOOST_CHECK(gate.Get().reason == FlowMeshGateReason::RECONCILING);
    // A stale count (read before a concurrent increment) is not an opening.
    gate.Observe(true, 1, T0 + 7s, FlowMeshGateReason::TIP_CHANGED);
    BOOST_CHECK(gate.Get().closed_since == T0 + 5s);
    BOOST_CHECK(gate.Get().reason == FlowMeshGateReason::TIP_CHANGED);
    // The reverse order: the opener observes last, while the gate is open.
    gate.Observe(false, 3, T0 + 8s);
    BOOST_CHECK(!gate.Get().closed);
}

BOOST_AUTO_TEST_CASE(lag_tracker_hints_never_start_an_episode)
{
    node::FlowMeshLagTracker lag{5};
    for (flowmesh::WirePeerId peer{0}; peer < 100; ++peer) lag.Hello(peer, 1'000'000, T0);
    BOOST_CHECK(!lag.Behind());
    const auto summary{lag.Summary()};
    BOOST_CHECK(!summary.behind_since);
    BOOST_CHECK(!summary.proven_target);
    BOOST_CHECK(!summary.speculative);
    // Bounded under a flood of hints; shown, never used.
    BOOST_CHECK_EQUAL(summary.peers_hinting_ahead, node::FlowMeshLagTracker::MAX_HINTS);
    BOOST_CHECK_EQUAL(*summary.peer_hint_next_sequence, 1'000'000U);
    lag.Expire(T0 + node::FlowMeshLagTracker::HINT_TTL + 1s);
    BOOST_CHECK_EQUAL(lag.Summary().peers_hinting_ahead, 0U);
    BOOST_CHECK(!lag.Summary().peer_hint_next_sequence);
}

BOOST_AUTO_TEST_CASE(lag_tracker_proven_lag_survives_commits_announcements_and_time)
{
    node::FlowMeshLagTracker lag{5};
    lag.Proven(9, T0); // A verified certificate for sequence 9.
    BOOST_CHECK(lag.Behind());
    BOOST_CHECK(lag.ProvenBehind());
    BOOST_CHECK(lag.Summary().behind_since == T0);
    BOOST_CHECK(lag.Summary().proven_since == T0);
    BOOST_CHECK_EQUAL(*lag.Summary().proven_target, 10U);
    // Page-by-page progress, a deferred catch-up certificate retried through
    // the live path, or an old certificate relayed for our next sequence:
    // all of them only advance the local head, which never restarts the
    // episode and never ends it before the proven target.
    for (uint64_t next{6}; next <= 9; ++next) {
        lag.LocalAdvanced(next, T0 + std::chrono::seconds{next});
        BOOST_CHECK(lag.Behind());
        BOOST_CHECK(lag.Summary().behind_since == T0);
        BOOST_CHECK(lag.Summary().proven_since == T0);
    }
    // No announcement from any peer, and no timeout, clears proven lag.
    for (flowmesh::WirePeerId peer{0}; peer < 4; ++peer) lag.Hello(peer, 9, T0 + 20s);
    lag.Expire(T0 + 10min);
    BOOST_CHECK(lag.Behind());
    BOOST_CHECK(lag.Summary().behind_since == T0);
    // A later certificate raises the target; an older one never lowers it.
    lag.Proven(12, T0 + 11min);
    lag.Proven(10, T0 + 11min);
    BOOST_CHECK_EQUAL(*lag.Summary().proven_target, 13U);
    lag.LocalAdvanced(13, T0 + 12min);
    BOOST_CHECK(!lag.Behind());
    BOOST_CHECK(!lag.Summary().behind_since);
    BOOST_CHECK(!lag.Summary().proven_since);
    BOOST_CHECK(!lag.Summary().proven_target);
    // A certificate below the local head proves nothing.
    lag.Proven(3, T0 + 13min);
    BOOST_CHECK(!lag.Behind());
}

BOOST_AUTO_TEST_CASE(lag_tracker_speculative_lag_ends_only_by_its_peer_tail_slack_or_ttl)
{
    node::FlowMeshLagTracker lag{5};
    lag.Speculative(7, T0);
    BOOST_CHECK(lag.Behind());
    BOOST_CHECK(lag.Summary().speculative);
    // Speculative lag is shown, never proven.
    BOOST_CHECK(!lag.ProvenBehind());
    BOOST_CHECK(!lag.Summary().proven_since);
    BOOST_CHECK(!lag.Summary().proven_target);
    // Page-by-page progress keeps the episode's start.
    lag.LocalAdvanced(8, T0 + 1s);
    lag.Speculative(7, T0 + 2s);
    BOOST_CHECK(lag.Summary().behind_since == T0);
    // Another peer announcing a head at or below ours cannot hide it.
    lag.Hello(8, 8, T0 + 3s);
    BOOST_CHECK(lag.Behind());
    // The serving peer can.
    lag.Hello(7, 8, T0 + 4s);
    BOOST_CHECK(!lag.Behind());
    BOOST_CHECK(!lag.Summary().behind_since);

    lag.Speculative(7, T0 + 5s);
    BOOST_CHECK(lag.Summary().behind_since == T0 + 5s);
    lag.SpeculativeEnd(T0 + 6s); // A page with tail slack.
    BOOST_CHECK(!lag.Behind());

    lag.Speculative(7, T0 + 7s);
    lag.Expire(T0 + 7s + node::FlowMeshLagTracker::SPECULATIVE_TTL);
    BOOST_CHECK(lag.Behind());
    lag.Expire(T0 + 8s + node::FlowMeshLagTracker::SPECULATIVE_TTL);
    BOOST_CHECK(!lag.Behind());

    // A removed peer can no longer end the episode; the TTL still does.
    lag.Speculative(7, T0 + 60s);
    lag.RemovePeer(7);
    lag.Hello(7, 8, T0 + 61s);
    BOOST_CHECK(lag.Behind());
    // Speculative peers are bounded; unrecorded peers cannot end it either.
    for (flowmesh::WirePeerId peer{100}; peer < 120; ++peer) lag.Speculative(peer, T0 + 62s);
    lag.Hello(119, 8, T0 + 63s);
    BOOST_CHECK(lag.Behind());
    lag.Hello(100, 8, T0 + 64s);
    BOOST_CHECK(!lag.Behind());
}

BOOST_AUTO_TEST_CASE(lag_tracker_speculative_end_keeps_proven_lag)
{
    node::FlowMeshLagTracker lag{0};
    lag.Proven(4, T0);
    lag.Speculative(1, T0 + 1s);
    lag.SpeculativeEnd(T0 + 2s);
    lag.Hello(1, 0, T0 + 3s);
    BOOST_CHECK(lag.Behind());
    BOOST_CHECK(lag.Summary().behind_since == T0);
    BOOST_CHECK(lag.Summary().proven_since == T0);
    BOOST_CHECK(!lag.Summary().speculative);
    lag.LocalAdvanced(5, T0 + 4s);
    BOOST_CHECK(!lag.Behind());
}

BOOST_AUTO_TEST_CASE(service_readiness_rules_and_graces)
{
    node::FlowMeshHealthPolicy policy;
    const auto snapshot{Snapshot()};
    using node::FlowMeshServiceState;
    const auto evaluate = [&](FlowMeshServiceState state, const node::FlowMeshGateObservation& gate,
                              const node::FlowMeshRuntimeHealthSnapshot* observed, Clock::time_point now) {
        return Names(node::EvaluateFlowMeshServiceReadiness(state, gate, observed, now, policy));
    };
    using Names_t = std::vector<std::string>;
    BOOST_CHECK(evaluate(FlowMeshServiceState::RUNNING, Open(), snapshot.get(), T0 + 1s) == Names_t{});
    // A stopped or dormant service reports only that, never a closed gate.
    BOOST_CHECK(evaluate(FlowMeshServiceState::STOPPED, Closed(FlowMeshGateReason::RECONCILING, T0), nullptr, T0 + 1h) ==
                Names_t{"service_stopped"});
    BOOST_CHECK(evaluate(FlowMeshServiceState::DORMANT, Open(), nullptr, T0) == Names_t{"service_stopped"});
    // Inactive gates are reported at once; a reconciliation only after its
    // continuous closure outlasts the grace.
    BOOST_CHECK(evaluate(FlowMeshServiceState::RUNNING, Closed(FlowMeshGateReason::INITIAL_BLOCK_DOWNLOAD, T0), snapshot.get(), T0) ==
                Names_t{"initial_block_download"});
    BOOST_CHECK(evaluate(FlowMeshServiceState::RUNNING, Closed(FlowMeshGateReason::BELOW_ACTIVATION, T0), snapshot.get(), T0) ==
                Names_t{"below_activation"});
    for (const auto reason : {FlowMeshGateReason::RECONCILING, FlowMeshGateReason::STARTING,
                              FlowMeshGateReason::STORE_RECONCILIATION_FAILED, FlowMeshGateReason::TIP_CHANGED}) {
        const auto gate{Closed(reason, T0)};
        const auto fresh{Snapshot(T0 + 10s)};
        BOOST_CHECK(evaluate(FlowMeshServiceState::RUNNING, gate, fresh.get(), T0 + 9999ms) == Names_t{});
        BOOST_CHECK(evaluate(FlowMeshServiceState::RUNNING, gate, fresh.get(), T0 + 10s) == Names_t{"reconciling_prolonged"});
    }
    BOOST_CHECK(evaluate(FlowMeshServiceState::RUNNING, {true, std::nullopt, FlowMeshGateReason::RECONCILING}, snapshot.get(), T0) ==
                Names_t{"reconciling_prolonged"});
    // A worker that has not published for the stall bound, or never did.
    BOOST_CHECK(evaluate(FlowMeshServiceState::RUNNING, Open(), snapshot.get(), T0 + 4999ms) == Names_t{});
    BOOST_CHECK(evaluate(FlowMeshServiceState::RUNNING, Open(), snapshot.get(), T0 + 5s) == Names_t{"worker_stalled"});
    BOOST_CHECK(evaluate(FlowMeshServiceState::RUNNING, Open(), nullptr, T0) == Names_t{"worker_stalled"});
    // No FlowMesh peers for the whole grace.
    auto isolated{Snapshot(T0 + 30s)};
    isolated->connected_peers = 0;
    isolated->no_peers_since = T0;
    BOOST_CHECK(evaluate(FlowMeshServiceState::RUNNING, Open(), isolated.get(), T0 + 29s) == Names_t{});
    BOOST_CHECK(evaluate(FlowMeshServiceState::RUNNING, Open(), isolated.get(), T0 + 30s) == Names_t{"no_flowmesh_peers"});
    // Fixed order, no duplicates.
    BOOST_CHECK(evaluate(FlowMeshServiceState::RUNNING, Closed(FlowMeshGateReason::IDLE_WAIT_TIMEOUT, T0), isolated.get(), T0 + 40s) ==
                (Names_t{"reconciling_prolonged", "worker_stalled", "no_flowmesh_peers"}));
}

BOOST_AUTO_TEST_CASE(market_readiness_is_market_scoped_and_labels_one_state)
{
    node::FlowMeshHealthPolicy policy;
    using Names_t = std::vector<std::string>;
    const auto evaluate = [&](const std::vector<FlowMeshNotReady>& service,
                              const node::FlowMeshMarketHealthObservation* market, Clock::time_point now) {
        return node::EvaluateFlowMeshMarketReadiness(service, market, now, policy);
    };
    auto market{Market(1)};
    market.head_advanced_at = T0;
    auto readiness{evaluate({}, &market, T0 + 60s)};
    BOOST_CHECK(readiness.ready);
    BOOST_CHECK_EQUAL(readiness.state, "live");
    // An idle head ages legitimately: head age alone never decides readiness.
    readiness = evaluate({}, &market, T0 + 1h);
    BOOST_CHECK(readiness.ready);
    BOOST_CHECK_EQUAL(readiness.state, "idle");
    market.head_advanced_at.reset(); // Head applied before this process started.
    BOOST_CHECK_EQUAL(evaluate({}, &market, T0).state, "idle");

    readiness = evaluate({}, nullptr, T0);
    BOOST_CHECK(Names(readiness.not_ready) == Names_t{"market_not_installed"});
    BOOST_CHECK_EQUAL(readiness.state, "not_installed");

    auto halted{Market(2)};
    halted.halted = true;
    halted.halt = "store-failure";
    readiness = evaluate({}, &halted, T0);
    BOOST_CHECK(Names(readiness.not_ready) == Names_t{"market_halted"});
    BOOST_CHECK_EQUAL(readiness.state, "halted");

    // Only locally proven lag decides readiness. Speculative lag (a page
    // without tail slack) is labelled but never makes a market not ready.
    auto speculative{Market(3)};
    speculative.lag.behind_since = T0;
    speculative.lag.speculative = true;
    readiness = evaluate({}, &speculative, T0 + 1h);
    BOOST_CHECK(readiness.ready);
    BOOST_CHECK_EQUAL(readiness.state, "catching_up");
    auto behind{Market(3)};
    behind.lag.behind_since = behind.lag.proven_since = T0;
    behind.lag.proven_target = 20;
    readiness = evaluate({}, &behind, T0 + 9s);
    BOOST_CHECK(readiness.ready);
    BOOST_CHECK_EQUAL(readiness.state, "catching_up");
    readiness = evaluate({}, &behind, T0 + 10s);
    BOOST_CHECK(Names(readiness.not_ready) == Names_t{"catching_up"});

    auto shell{Market(4)};
    shell.installed = false;
    shell.paused = true;
    BOOST_CHECK_EQUAL(evaluate({}, &shell, T0).state, "insufficient_seats");
    BOOST_CHECK(evaluate({}, &shell, T0).ready);
    auto handoff{Market(5)};
    handoff.pending_handoff = true;
    BOOST_CHECK_EQUAL(evaluate({}, &handoff, T0).state, "handoff_pending");
    auto paused{Market(6)};
    paused.paused = true;
    paused.pause_reason = "handoff_checkpoints_pending";
    BOOST_CHECK_EQUAL(evaluate({}, &paused, T0).state, "paused");
    BOOST_CHECK(evaluate({}, &paused, T0).ready);
    // The normal per-block pause is not a separate state.
    paused.pause_reason = "b3_reconciliation_pending";
    BOOST_CHECK_EQUAL(evaluate({}, &paused, T0).state, "idle");

    // Service reasons apply to every market and label its state first.
    readiness = evaluate({FlowMeshNotReady::SERVICE_STOPPED}, &halted, T0);
    BOOST_CHECK(Names(readiness.not_ready) == (Names_t{"service_stopped", "market_halted"}));
    BOOST_CHECK_EQUAL(readiness.state, "stopped");
    BOOST_CHECK_EQUAL(evaluate({FlowMeshNotReady::BELOW_ACTIVATION}, &market, T0).state, "inactive");
    BOOST_CHECK_EQUAL(evaluate({FlowMeshNotReady::RECONCILING_PROLONGED}, &market, T0).state, "reconciling");
    BOOST_CHECK_EQUAL(evaluate({FlowMeshNotReady::WORKER_STALLED}, &market, T0).state, "worker_stalled");
    BOOST_CHECK_EQUAL(evaluate({FlowMeshNotReady::NO_FLOWMESH_PEERS}, &market, T0).state, "isolated");
}

BOOST_AUTO_TEST_CASE(service_health_rows_union_filter_and_bound)
{
    auto snapshot{Snapshot()};
    snapshot->markets = {Market(1), Market(3)};
    snapshot->markets[1].halted = true;
    const std::set<flowmesh::MarketId> service{Id(1), Id(2)};
    node::FlowMeshHealthPolicy policy;
    const auto health{node::BuildFlowMeshServiceHealth(node::FlowMeshServiceState::RUNNING, Open(), policy, T0,
                                                       snapshot, service)};
    BOOST_REQUIRE_EQUAL(health.rows.size(), 3U);
    BOOST_CHECK(health.ServiceReady());
    BOOST_CHECK(!health.Ready()); // A label only: one market is not ready.
    BOOST_CHECK(health.Find(Id(1))->readiness.ready);
    BOOST_CHECK(!health.Find(Id(2))->observation);
    BOOST_CHECK(Names(health.Find(Id(2))->readiness.not_ready) == std::vector<std::string>{"market_not_installed"});
    BOOST_CHECK(Names(health.Find(Id(3))->readiness.not_ready) == std::vector<std::string>{"market_halted"});
    BOOST_CHECK(!health.Find(Id(4)));
    BOOST_CHECK(!health.truncated);

    const auto one{node::BuildFlowMeshServiceHealth(node::FlowMeshServiceState::RUNNING, Open(), policy, T0,
                                                    snapshot, service, Id(3))};
    BOOST_REQUIRE_EQUAL(one.rows.size(), 1U);
    BOOST_CHECK(one.rows[0].market_id == Id(3));
    BOOST_CHECK(node::BuildFlowMeshServiceHealth(node::FlowMeshServiceState::RUNNING, Open(), policy, T0,
                                                 snapshot, service, Id(9)).rows.empty());

    std::set<flowmesh::MarketId> many;
    for (unsigned i{0}; i < node::FLOWMESH_HEALTH_MAX_MARKETS + 5; ++i) {
        uint256 id;
        id.data()[0] = static_cast<unsigned char>(i);
        id.data()[1] = static_cast<unsigned char>(i >> 8);
        many.insert(id);
    }
    const auto bounded{node::BuildFlowMeshServiceHealth(node::FlowMeshServiceState::RUNNING, Open(), policy, T0,
                                                        snapshot, many)};
    BOOST_CHECK_EQUAL(bounded.rows.size(), node::FLOWMESH_HEALTH_MAX_MARKETS);
    BOOST_CHECK(bounded.truncated);
}

BOOST_AUTO_TEST_CASE(health_json_is_versioned_omits_absent_values_and_operator_text)
{
    auto snapshot{Snapshot(T0 - 120ms)};
    snapshot->connected_peers = 0;
    snapshot->no_peers_since = T0 - 2s;
    auto live{Market(1)};
    live.pause_reason = "";
    live.error = "operator-only text /path";
    live.proposal_wait_reason = "proposer_not_local";
    live.head_advanced_at = T0 - 850ms;
    live.local_observed_at = 1'790'000'000;
    live.lag.behind_since = live.lag.proven_since = T0 - 3s;
    live.lag.proven_target = 12;
    live.lag.peer_hint_next_sequence = 40;
    live.lag.peers_hinting_ahead = 2;
    auto fresh{Market(2)}; // No head in this process, no lag, no hints.
    snapshot->markets = {live, fresh};
    const auto health{node::BuildFlowMeshServiceHealth(
        node::FlowMeshServiceState::RUNNING, Closed(FlowMeshGateReason::TIP_CHANGED, T0 - 1500ms),
        node::FlowMeshHealthPolicy{}, T0, snapshot, {Id(1), Id(2), Id(3)})};

    const auto json{node::FlowMeshServiceHealthJson(health, /*operator_detail=*/false)};
    CheckNoNull(json);
    BOOST_CHECK(Keys(json) == (std::set<std::string>{"health_version", "observation", "ready", "service_ready",
                                                     "not_ready", "service", "policy", "markets", "markets_truncated"}));
    BOOST_CHECK_EQUAL(json["health_version"].getInt<int>(), 1);
    BOOST_CHECK_EQUAL(json["observation"].get_str(), "local-node");
    BOOST_CHECK(!json["ready"].get_bool());
    BOOST_CHECK(json["service_ready"].get_bool());
    BOOST_CHECK(json["not_ready"].empty());
    const auto& service{json["service"]};
    BOOST_CHECK(Keys(service) == (std::set<std::string>{"state", "chain_gate", "chain_gate_reason", "chain_gate_closed_ms",
                                                        "worker_observation_age_ms"}));
    BOOST_CHECK_EQUAL(service["state"].get_str(), "running");
    BOOST_CHECK_EQUAL(service["chain_gate"].get_str(), "closed");
    BOOST_CHECK_EQUAL(service["chain_gate_reason"].get_str(), "tip_changed");
    BOOST_CHECK_EQUAL(service["chain_gate_closed_ms"].getInt<int64_t>(), 1500);
    BOOST_CHECK_EQUAL(service["worker_observation_age_ms"].getInt<int64_t>(), 120);
    BOOST_CHECK(Keys(json["policy"]) == (std::set<std::string>{"reconcile_grace_ms", "lag_grace_ms", "worker_stall_ms",
                                                               "no_peers_grace_ms", "live_window_ms"}));
    const auto& markets{json["markets"].getValues()};
    BOOST_REQUIRE_EQUAL(markets.size(), 3U);
    const std::set<std::string> row_keys{"market_id", "ready", "not_ready", "state", "halt", "paused", "pause_reason",
                                         "pending_handoff", "epoch", "active_seats", "quorum_required", "pending_actions",
                                         "next_microblock_sequence", "last_microblock_hash", "catchup", "seat_evidence"};
    auto live_keys{row_keys};
    live_keys.insert({"head_age_ms", "head_local_observed_unix"});
    BOOST_CHECK(Keys(markets[0]) == live_keys);
    BOOST_CHECK(Keys(markets[1]) == row_keys);
    BOOST_CHECK(Keys(markets[2]) == (std::set<std::string>{"market_id", "ready", "not_ready", "state"}));
    BOOST_CHECK_EQUAL(markets[0]["head_age_ms"].getInt<int64_t>(), 850);
    BOOST_CHECK_EQUAL(markets[0]["head_local_observed_unix"].getInt<int64_t>(), 1'790'000'000);
    BOOST_CHECK_EQUAL(markets[0]["state"].get_str(), "catching_up");
    BOOST_CHECK(Keys(markets[0]["catchup"]) == (std::set<std::string>{"behind", "behind_ms", "proven_target", "proven_behind_ms",
                                                                       "speculative"}));
    BOOST_CHECK_EQUAL(markets[0]["catchup"]["proven_behind_ms"].getInt<int64_t>(), 3000);
    BOOST_CHECK_EQUAL(markets[0]["catchup"]["behind_ms"].getInt<int64_t>(), 3000);
    BOOST_CHECK_EQUAL(markets[0]["catchup"]["proven_target"].getInt<uint64_t>(), 12U);
    BOOST_CHECK(Keys(markets[1]["catchup"]) == (std::set<std::string>{"behind", "speculative"}));
    BOOST_CHECK(Names(markets[2]["not_ready"]) == std::vector<std::string>{"market_not_installed"});
    BOOST_CHECK(json.write().find("operator-only") == std::string::npos);
    BOOST_CHECK(json.write().find("proposer_not_local") == std::string::npos);

    const auto detail{node::FlowMeshServiceHealthJson(health, /*operator_detail=*/true)};
    CheckNoNull(detail);
    BOOST_CHECK(Keys(detail["service"]) == (std::set<std::string>{"state", "chain_gate", "chain_gate_reason", "chain_gate_closed_ms",
                                                                  "worker_observation_age_ms", "flowmesh_peers", "no_peers_ms"}));
    BOOST_CHECK_EQUAL(detail["service"]["no_peers_ms"].getInt<int64_t>(), 2000);
    auto detail_keys{live_keys};
    detail_keys.insert({"error", "proposal_wait_reason"});
    BOOST_CHECK(Keys(detail["markets"][0]) == detail_keys);
    BOOST_CHECK_EQUAL(detail["markets"][0]["error"].get_str(), "operator-only text /path");
    BOOST_CHECK_EQUAL(detail["markets"][0]["catchup"]["peer_hint_next_sequence"].getInt<uint64_t>(), 40U);
    BOOST_CHECK_EQUAL(detail["markets"][0]["catchup"]["peers_hinting_ahead"].getInt<uint64_t>(), 2U);
    BOOST_CHECK(Keys(detail["markets"][1]["catchup"]) == (std::set<std::string>{"behind", "speculative", "peers_hinting_ahead"}));

    // A service that is not running reports no gate and no worker fields.
    const auto stopped{node::FlowMeshServiceHealthJson(node::BuildFlowMeshServiceHealth(
        node::FlowMeshServiceState::DORMANT, Closed(FlowMeshGateReason::RECONCILING, T0), node::FlowMeshHealthPolicy{},
        T0, nullptr, {}), true)};
    CheckNoNull(stopped);
    BOOST_CHECK(Keys(stopped["service"]) == std::set<std::string>{"state"});
    BOOST_CHECK_EQUAL(stopped["service"]["state"].get_str(), "dormant");
    BOOST_CHECK(Names(stopped["not_ready"]) == std::vector<std::string>{"service_stopped"});
    BOOST_CHECK(stopped["markets"].empty());
}

BOOST_AUTO_TEST_CASE(health_json_lists_seat_evidence_as_information_only)
{
    // Shape only: the runtime keeps a record only after checking it.
    const auto evidence = [](const flowmesh::SeatEvidenceKind kind, const uint8_t sequence) {
        flowmesh::SeatEvidence out;
        out.kind = kind;
        out.domain = Id(0x11);
        out.market_id = Id(1);
        for (auto* statement : {&out.first, &out.second}) {
            statement->seat_epoch = 3;
            statement->seat_set_hash = Id(0x33);
            statement->sequence = sequence;
            if (kind == flowmesh::SeatEvidenceKind::ATTESTATION_EQUIVOCATION) {
                statement->seat_index = 2;
            }
            statement->signature.fill(0xab);
        }
        if (kind == flowmesh::SeatEvidenceKind::CERTIFICATE_CONFLICT) {
            out.first.signer_bitmap = {0x07};
            out.second.signer_bitmap = {0x0e};
        } else {
            out.seat_public_key.emplace();
            out.seat_public_key->fill(0xcd);
        }
        out.first.microblock_hash = Id(0xa1);
        out.second.microblock_hash = Id(0xb2);
        return out;
    };
    auto retained{std::make_shared<std::vector<flowmesh::SeatEvidence>>()};
    retained->push_back(evidence(flowmesh::SeatEvidenceKind::CERTIFICATE_CONFLICT, 9));
    for (uint8_t sequence{8}; sequence >= 4; --sequence) {
        retained->push_back(evidence(flowmesh::SeatEvidenceKind::ATTESTATION_EQUIVOCATION, sequence));
    }
    auto snapshot{Snapshot()};
    auto row{Market(1)};
    row.seat_evidence_recorded = 70;
    row.seat_evidence = retained;
    row.seat_evidence_unrecorded = 2;
    snapshot->markets = {row};
    const auto health{node::BuildFlowMeshServiceHealth(node::FlowMeshServiceState::RUNNING, Open(),
                                                       node::FlowMeshHealthPolicy{}, T0, snapshot, {Id(1)})};

    const auto json{node::FlowMeshServiceHealthJson(health, /*operator_detail=*/false)};
    CheckNoNull(json);
    // Recorded evidence never changes readiness.
    BOOST_CHECK(json["markets"][0]["ready"].get_bool());
    const auto& public_evidence{json["markets"][0]["seat_evidence"]};
    BOOST_CHECK(Keys(public_evidence) == (std::set<std::string>{"recorded", "retained", "latest"}));
    BOOST_CHECK_EQUAL(public_evidence["recorded"].getInt<uint64_t>(), 70U);
    BOOST_CHECK_EQUAL(public_evidence["retained"].getInt<uint64_t>(), 6U);
    BOOST_REQUIRE_EQUAL(public_evidence["latest"].size(), node::FLOWMESH_HEALTH_PUBLIC_SEAT_EVIDENCE);
    const auto& conflict{public_evidence["latest"][0]};
    BOOST_CHECK(Keys(conflict) == (std::set<std::string>{"evidence_id", "kind", "domain", "market_id", "first", "second",
                                                         "double_signers"}));
    BOOST_CHECK_EQUAL(conflict["kind"].get_str(), "certificate_conflict");
    BOOST_CHECK_EQUAL(conflict["evidence_id"].get_str(), retained->front().Id().GetHex());
    BOOST_CHECK_EQUAL(conflict["domain"].get_str(), Id(0x11).GetHex());
    BOOST_CHECK(Keys(conflict["first"]) == (std::set<std::string>{"seat_epoch", "seat_set_hash", "sequence",
                                                                  "microblock_hash", "signer_bitmap", "signature"}));
    BOOST_CHECK_EQUAL(conflict["first"]["signer_bitmap"].get_str(), "07");
    BOOST_CHECK_EQUAL(conflict["second"]["microblock_hash"].get_str(), Id(0xb2).GetHex());
    std::string signature_hex;
    for (int i{0}; i < 96; ++i) signature_hex += "ab";
    BOOST_CHECK_EQUAL(conflict["second"]["signature"].get_str(), signature_hex);
    BOOST_CHECK_EQUAL(conflict["double_signers"].write(), "[1,2]");
    const auto& equivocation{public_evidence["latest"][1]};
    BOOST_CHECK_EQUAL(equivocation["kind"].get_str(), "attestation_equivocation");
    BOOST_CHECK(!equivocation.exists("double_signers"));
    BOOST_CHECK(Keys(equivocation) == (std::set<std::string>{"evidence_id", "kind", "domain", "market_id", "first",
                                                             "second", "seat_public_key"}));
    std::string key_hex;
    for (int i{0}; i < 48; ++i) key_hex += "cd";
    BOOST_CHECK_EQUAL(equivocation["seat_public_key"].get_str(), key_hex);
    BOOST_CHECK(Keys(equivocation["second"]) == (std::set<std::string>{"seat_epoch", "seat_set_hash", "sequence",
                                                                       "microblock_hash", "seat_index", "signature"}));
    BOOST_CHECK_EQUAL(equivocation["second"]["seat_index"].getInt<int>(), 2);
    BOOST_CHECK_EQUAL(equivocation["first"]["sequence"].getInt<int>(), 8);

    // Operator detail lists every retained record and what was not recorded.
    const auto detail{node::FlowMeshServiceHealthJson(health, /*operator_detail=*/true)};
    CheckNoNull(detail);
    const auto& operator_evidence{detail["markets"][0]["seat_evidence"]};
    BOOST_CHECK_EQUAL(operator_evidence["unrecorded"].getInt<uint64_t>(), 2U);
    BOOST_CHECK_EQUAL(operator_evidence["latest"].size(), 6U);
    BOOST_CHECK_EQUAL(operator_evidence["latest"][5]["first"]["sequence"].getInt<int>(), 4);
}

BOOST_AUTO_TEST_SUITE_END()
