// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <node/flowmesh_client_action_wait.h>

#include <univalue.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <string>

namespace {
using namespace std::chrono_literals;
using Request = node::FlowMeshActionWaitRequest;

std::chrono::milliseconds Advertised(const std::string& json)
{
    UniValue status;
    BOOST_REQUIRE(status.read(json));
    return node::FlowMeshAdvertisedActionWait(status);
}

//! A just delivered, unresolved action at an endpoint advertising 2000 ms.
Request Eligible(std::chrono::milliseconds requested = 2000ms)
{
    return {.requested = requested, .may_have_been_sent = true, .delivery_endpoint_known = true, .advertised = 2000ms};
}
} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_client_action_wait_tests)

BOOST_AUTO_TEST_CASE(advertised_wait_is_capped_and_malformed_hints_disable_it)
{
    BOOST_CHECK(Advertised(R"({"running":true})") == 0ms); // Older endpoints.
    BOOST_CHECK(Advertised(R"({"action_wait_ms_max":0})") == 0ms);
    BOOST_CHECK(Advertised(R"({"action_wait_ms_max":1})") == 1ms);
    BOOST_CHECK(Advertised(R"({"action_wait_ms_max":2000})") == 2000ms);
    BOOST_CHECK(Advertised(R"({"action_wait_ms_max":2500})") == 2500ms);
    BOOST_CHECK(Advertised(R"({"action_wait_ms_max":60000})") == node::FLOWMESH_CLIENT_ACTION_WAIT_MAX);
    BOOST_CHECK(Advertised(R"({"action_wait_ms_max":18446744073709551615})") == node::FLOWMESH_CLIENT_ACTION_WAIT_MAX);
    for (const std::string bad : {"-1", "1.5", "\"2000\"", "true", "null", "[2000]", "{}", "18446744073709551616"}) {
        BOOST_TEST_CONTEXT("action_wait_ms_max=" << bad) {
            BOOST_CHECK(Advertised(R"({"action_wait_ms_max":)" + bad + "}") == 0ms);
        }
    }
    BOOST_CHECK(node::FlowMeshAdvertisedActionWait(UniValue{}) == 0ms);
    BOOST_CHECK(Advertised(R"([{"action_wait_ms_max":2000}])") == 0ms);
}

BOOST_AUTO_TEST_CASE(wait_is_the_smallest_of_request_advertisement_and_client_cap)
{
    BOOST_CHECK(node::FlowMeshActionWaitFor(Eligible()) == 2000ms);
    BOOST_CHECK(node::FlowMeshActionWaitFor(Eligible(1ms)) == 1ms);
    BOOST_CHECK(node::FlowMeshActionWaitFor(Eligible(1500ms)) == 1500ms);
    BOOST_CHECK(node::FlowMeshActionWaitFor(Eligible(10s)) == 2000ms);
    auto uncapped{Eligible(10s)};
    uncapped.advertised = 1h; // Never trusted beyond the client's own cap.
    BOOST_CHECK(node::FlowMeshActionWaitFor(uncapped) == node::FLOWMESH_CLIENT_ACTION_WAIT_MAX);
}

BOOST_AUTO_TEST_CASE(only_a_delivered_unresolved_action_at_an_advertising_endpoint_waits)
{
    const auto check = [](const char* name, auto change) {
        BOOST_TEST_CONTEXT(name) {
            auto request{Eligible()};
            change(request);
            BOOST_CHECK(node::FlowMeshActionWaitFor(request) == 0ms);
        }
    };
    check("no wait requested", [](Request& r) { r.requested = 0ms; });
    check("negative wait", [](Request& r) { r.requested = -5ms; });
    // An explicit retry keeps its fresh-query-then-resend path.
    check("retry", [](Request& r) { r.retry = true; });
    check("never delivered", [](Request& r) { r.may_have_been_sent = false; });
    check("already verified", [](Request& r) { r.certificate_verified = true; });
    // Recovery of a previously certified action never resends and keeps its
    // own proof-renewal path.
    check("previously certified", [](Request& r) { r.previously_certified = true; });
    check("definitely refused", [](Request& r) { r.rejected = true; });
    // For example after a restart, which loses the volatile delivery endpoint.
    check("no acknowledging endpoint", [](Request& r) { r.delivery_endpoint_known = false; });
    // Older endpoints reject wait_ms as an unknown field.
    check("not advertised", [](Request& r) { r.advertised = 0ms; });
    check("negative advertisement", [](Request& r) { r.advertised = -1ms; });
}

BOOST_AUTO_TEST_CASE(advertised_submit_wait_is_capped_and_malformed_hints_disable_it)
{
    const auto advertised = [](const std::string& json) {
        UniValue status;
        BOOST_REQUIRE(status.read(json));
        return node::FlowMeshAdvertisedSubmitWait(status);
    };
    // Only its own key: an action wait alone never enables waited submissions.
    BOOST_CHECK(advertised(R"({"action_wait_ms_max":2000})") == 0ms);
    BOOST_CHECK(advertised(R"({"action_wait_ms_max":2000,"submit_wait_ms_max":2000})") == 2000ms);
    BOOST_CHECK(advertised(R"({"submit_wait_ms_max":1})") == 1ms);
    BOOST_CHECK(advertised(R"({"submit_wait_ms_max":60000})") == node::FLOWMESH_CLIENT_ACTION_WAIT_MAX);
    for (const std::string bad : {"-1", "1.5", "\"2000\"", "true", "null", "[2000]", "{}", "18446744073709551616"}) {
        BOOST_TEST_CONTEXT("submit_wait_ms_max=" << bad) {
            BOOST_CHECK(advertised(R"({"submit_wait_ms_max":)" + bad + "}") == 0ms);
        }
    }
    BOOST_CHECK(node::FlowMeshAdvertisedSubmitWait(UniValue{}) == 0ms);
    // The action wait reads only its own key.
    BOOST_CHECK(Advertised(R"({"submit_wait_ms_max":2000})") == 0ms);
}

BOOST_AUTO_TEST_CASE(submit_wait_needs_a_request_an_advertisement_a_new_instruction_and_a_usable_endpoint)
{
    using SubmitRequest = node::FlowMeshSubmitWaitRequest;
    const SubmitRequest eligible{.requested = 2000ms, .new_instruction = true, .advertised = 2000ms};
    BOOST_CHECK(node::FlowMeshSubmitWaitFor(eligible) == 2000ms);
    auto shorter{eligible};
    shorter.requested = 700ms;
    BOOST_CHECK(node::FlowMeshSubmitWaitFor(shorter) == 700ms);
    auto longer{eligible};
    longer.requested = 10s;
    longer.advertised = 1h; // Never trusted beyond the client's own cap.
    BOOST_CHECK(node::FlowMeshSubmitWaitFor(longer) == node::FLOWMESH_CLIENT_ACTION_WAIT_MAX);
    const auto check = [&](const char* name, auto change) {
        BOOST_TEST_CONTEXT(name) {
            auto request{eligible};
            change(request);
            BOOST_CHECK(node::FlowMeshSubmitWaitFor(request) == 0ms);
        }
    };
    check("no wait requested", [](SubmitRequest& r) { r.requested = 0ms; });
    check("negative wait", [](SubmitRequest& r) { r.requested = -5ms; });
    // A resend of an instruction that may have been delivered, or that is
    // certified or proven not executed, keeps its ordinary path.
    check("not a new instruction", [](SubmitRequest& r) { r.new_instruction = false; });
    // Older endpoints reject wait_ms on 'submit' as an unknown field.
    check("not advertised", [](SubmitRequest& r) { r.advertised = 0ms; });
    check("negative advertisement", [](SubmitRequest& r) { r.advertised = -1ms; });
    check("transport cooldown", [](SubmitRequest& r) { r.endpoint_cooling = true; });
    check("lane request unanswered", [](SubmitRequest& r) { r.endpoint_unanswered = true; });
}

BOOST_AUTO_TEST_SUITE_END()
