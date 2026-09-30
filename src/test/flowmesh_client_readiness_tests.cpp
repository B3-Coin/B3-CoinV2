// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <node/flowmesh_client_readiness.h>

#include <univalue.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

namespace {
using State = node::FlowMeshAdvertisedReadiness::State;

node::FlowMeshAdvertisedReadiness Parse(const std::string& json)
{
    UniValue status;
    BOOST_REQUIRE(status.read(json));
    return node::FlowMeshAdvertisedReadinessOf(status);
}
} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_client_readiness_tests)

BOOST_AUTO_TEST_CASE(advertised_readiness_parses_only_bounded_consistent_hints)
{
    // An older endpoint advertises nothing.
    BOOST_CHECK(Parse(R"({"running":true})").state == State::UNKNOWN);
    BOOST_CHECK(Parse(R"([])").state == State::UNKNOWN);
    const auto ready{Parse(R"({"health_api_version":1,"endpoint_ready":true,"endpoint_not_ready":[]})")};
    BOOST_CHECK(ready.state == State::READY);
    BOOST_CHECK(ready.not_ready.empty());
    const auto behind{Parse(R"({"health_api_version":1,"endpoint_ready":false,"endpoint_not_ready":["catching_up","no_flowmesh_peers"]})")};
    BOOST_CHECK(behind.state == State::NOT_READY);
    BOOST_CHECK((behind.not_ready == std::vector<std::string>{"catching_up", "no_flowmesh_peers"}));
    // A later version keeps the keys' meaning; an unknown code still means
    // not ready.
    BOOST_CHECK(Parse(R"({"health_api_version":2,"endpoint_ready":false,"endpoint_not_ready":["future_code_7"]})").state == State::NOT_READY);
    for (const std::string bad : {
             R"({"endpoint_ready":true,"endpoint_not_ready":[]})",
             R"({"health_api_version":0,"endpoint_ready":true,"endpoint_not_ready":[]})",
             R"({"health_api_version":1.5,"endpoint_ready":true,"endpoint_not_ready":[]})",
             R"({"health_api_version":"1","endpoint_ready":true,"endpoint_not_ready":[]})",
             R"({"health_api_version":1,"endpoint_ready":"yes","endpoint_not_ready":[]})",
             R"({"health_api_version":1,"endpoint_ready":true})",
             R"({"health_api_version":1,"endpoint_ready":true,"endpoint_not_ready":{}})",
             // Self-contradictory hints.
             R"({"health_api_version":1,"endpoint_ready":true,"endpoint_not_ready":["catching_up"]})",
             R"({"health_api_version":1,"endpoint_ready":false,"endpoint_not_ready":[]})",
             // Codes: text only, bounded, lower-case snake_case.
             R"({"health_api_version":1,"endpoint_ready":false,"endpoint_not_ready":[7]})",
             R"({"health_api_version":1,"endpoint_ready":false,"endpoint_not_ready":[""]})",
             R"({"health_api_version":1,"endpoint_ready":false,"endpoint_not_ready":["Catching Up"]})",
             R"({"health_api_version":1,"endpoint_ready":false,"endpoint_not_ready":["<script>"]})",
         }) {
        BOOST_TEST_CONTEXT(bad) { BOOST_CHECK(Parse(bad).state == State::UNKNOWN); }
    }
    std::string long_code(node::FLOWMESH_CLIENT_READINESS_MAX_CODE_SIZE + 1, 'a');
    BOOST_CHECK(Parse(R"({"health_api_version":1,"endpoint_ready":false,"endpoint_not_ready":[")" + long_code + R"("]})").state == State::UNKNOWN);
    long_code.pop_back();
    BOOST_CHECK(Parse(R"({"health_api_version":1,"endpoint_ready":false,"endpoint_not_ready":[")" + long_code + R"("]})").state == State::NOT_READY);
    std::string many{R"({"health_api_version":1,"endpoint_ready":false,"endpoint_not_ready":[)"};
    for (size_t i{0}; i <= node::FLOWMESH_CLIENT_READINESS_MAX_CODES; ++i) many += std::string{i ? "," : ""} + "\"c\"";
    BOOST_CHECK(Parse(many + "]}").state == State::UNKNOWN);
    BOOST_CHECK_EQUAL(node::FlowMeshAdvertisedReadinessName(State::UNKNOWN), "unknown");
    BOOST_CHECK_EQUAL(node::FlowMeshAdvertisedReadinessName(State::READY), "ready");
    BOOST_CHECK_EQUAL(node::FlowMeshAdvertisedReadinessName(State::NOT_READY), "not_ready");
}

BOOST_AUTO_TEST_SUITE_END()
