// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <node/flowmesh_client_watch.h>

#include <univalue.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <set>
#include <string>

namespace {
using namespace std::chrono_literals;
using Eligibility = node::FlowMeshWatchEligibility;

std::chrono::milliseconds Advertised(const std::string& json)
{
    UniValue status;
    BOOST_REQUIRE(status.read(json));
    return node::FlowMeshAdvertisedUpdatesWatch(status);
}

//! A current, scoped cache at an endpoint advertising 2000 ms.
Eligibility Eligible(std::chrono::milliseconds requested = 1500ms)
{
    return {.requested = requested, .advertised = 2000ms, .cache_current = true, .watch_ready = true, .scope_matches = true};
}
} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_client_watch_tests)

BOOST_AUTO_TEST_CASE(advertised_updates_watch_is_capped_and_malformed_hints_disable_it)
{
    BOOST_CHECK(Advertised(R"({"running":true})") == 0ms); // Older endpoints.
    BOOST_CHECK(Advertised(R"({"updates_watch_ms_max":0})") == 0ms);
    BOOST_CHECK(Advertised(R"({"updates_watch_ms_max":1})") == 1ms);
    BOOST_CHECK(Advertised(R"({"updates_watch_ms_max":2000})") == 2000ms);
    BOOST_CHECK(Advertised(R"({"updates_watch_ms_max":60000})") == node::FLOWMESH_CLIENT_ADVERTISED_WATCH_MAX);
    BOOST_CHECK(Advertised(R"({"updates_watch_ms_max":18446744073709551615})") == node::FLOWMESH_CLIENT_ADVERTISED_WATCH_MAX);
    for (const char* malformed : {R"({"updates_watch_ms_max":-1})", R"({"updates_watch_ms_max":1.5})",
                                  R"({"updates_watch_ms_max":"2000"})", R"({"updates_watch_ms_max":true})",
                                  R"({"updates_watch_ms_max":null})", R"({"updates_watch_ms_max":18446744073709551616})"}) {
        BOOST_CHECK_MESSAGE(Advertised(malformed) == 0ms, malformed);
    }
    BOOST_CHECK(node::FlowMeshAdvertisedUpdatesWatch(UniValue{UniValue::VARR}) == 0ms);
}

BOOST_AUTO_TEST_CASE(watch_needs_current_ready_scoped_cache_and_advertisement)
{
    BOOST_CHECK(node::FlowMeshUpdatesWatchFor(Eligible()) == 1500ms);
    // Capped by the caller, the endpoint and the client maximum.
    BOOST_CHECK(node::FlowMeshUpdatesWatchFor(Eligible(400ms)) == 400ms);
    auto advertised{Eligible()};
    advertised.advertised = 700ms;
    BOOST_CHECK(node::FlowMeshUpdatesWatchFor(advertised) == 700ms);
    BOOST_CHECK(node::FlowMeshUpdatesWatchFor(Eligible(60s)) == node::FLOWMESH_CLIENT_UPDATES_WATCH_MAX);
    BOOST_CHECK(node::FLOWMESH_CLIENT_UPDATES_WATCH_MAX == 1500ms);
    // Every other condition sends nothing: the ordinary read runs instead.
    for (int condition{0}; condition < 9; ++condition) {
        auto e{Eligible()};
        switch (condition) {
        case 0: e.requested = 0ms; break;
        case 1: e.requested = -5ms; break;
        case 2: e.advertised = 0ms; break;
        case 3: e.cache_current = false; break;
        case 4: e.watch_ready = false; break;
        case 5: e.scope_matches = false; break;
        case 6: e.cooldown = true; break;
        case 7: e.unanswered = true; break;
        case 8: e.paging = true; break;
        }
        BOOST_CHECK_MESSAGE(node::FlowMeshUpdatesWatchFor(e) == 0ms, "condition " << condition);
    }
}

BOOST_AUTO_TEST_CASE(watch_leaves_room_for_measured_lane_overhead)
{
    // Wait plus twice the overhead stays within the cycle budget ...
    for (const auto overhead : {0ms, 200ms, 450ms, 600ms, 900ms}) {
        auto e{Eligible()};
        e.lane_overhead = overhead;
        const auto wait{node::FlowMeshUpdatesWatchFor(e)};
        BOOST_CHECK_MESSAGE(wait + 2 * overhead <= node::FLOWMESH_CLIENT_WATCH_CYCLE_BUDGET || wait == node::FLOWMESH_CLIENT_UPDATES_WATCH_MIN,
                            "overhead " << overhead.count() << " wait " << wait.count());
        BOOST_CHECK(wait >= node::FLOWMESH_CLIENT_UPDATES_WATCH_MIN);
    }
    auto slow{Eligible()};
    slow.lane_overhead = 600ms;
    BOOST_CHECK(node::FlowMeshUpdatesWatchFor(slow) == 1200ms);
    // ... but never below its minimum, however slow the link.
    slow.lane_overhead = 10s;
    BOOST_CHECK(node::FlowMeshUpdatesWatchFor(slow) == node::FLOWMESH_CLIENT_UPDATES_WATCH_MIN);
    slow.lane_overhead = -1s;
    BOOST_CHECK(node::FlowMeshUpdatesWatchFor(slow) == 1500ms);
}

BOOST_AUTO_TEST_CASE(watch_status_labels_are_a_closed_set)
{
    const std::set<std::string_view> labels{node::FLOWMESH_CLIENT_WATCH_LABELS.begin(), node::FLOWMESH_CLIENT_WATCH_LABELS.end()};
    BOOST_CHECK_EQUAL(labels.size(), node::FLOWMESH_CLIENT_WATCH_LABELS.size());
    // The endpoint's own labels map to themselves ...
    for (const char* server : {"none", "no_cursor", "head_changed", "ready", "gap", "timeout", "interrupted", "restarted", "busy", "deadline"}) {
        BOOST_CHECK_EQUAL(node::FlowMeshWatchStatusLabel(server), server);
    }
    // ... anything else, including this client's own labels, is "other".
    for (const char* unexpected : {"", "READY", "failed", "unsupported", "superseded", "ticks_catchup", "x"}) {
        BOOST_CHECK_EQUAL(node::FlowMeshWatchStatusLabel(unexpected), "other");
    }
    // Only a reply after which the caller may watch again at once is "waited".
    const std::set<std::string_view> waited{"timeout", "ready", "head_changed", "gap", "restarted"};
    for (const auto label : labels) {
        BOOST_CHECK_EQUAL(node::FlowMeshWatchWaited(label), waited.contains(label));
        BOOST_CHECK(!(node::FlowMeshWatchWaited(label) && node::FlowMeshWatchReleasesLane(label)));
    }
    BOOST_CHECK(node::FlowMeshWatchReleasesLane("busy"));
    BOOST_CHECK(node::FlowMeshWatchReleasesLane("deadline"));
}

BOOST_AUTO_TEST_SUITE_END()
