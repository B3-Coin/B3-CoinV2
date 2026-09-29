// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_api_admission.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <map>
#include <string>

namespace {

using node::FlowMeshApiAdmission;
using node::FlowMeshApiRequestClass;
using Clock = std::chrono::steady_clock;
constexpr auto READ{FlowMeshApiRequestClass::READ};
constexpr auto WRITE{FlowMeshApiRequestClass::TRADE_WRITE};

//! The f8a3432 Admit, kept to show the lockout it had.
struct OldAdmit {
    struct Bucket { Clock::time_point time{}; unsigned count{0}; };
    std::map<std::string, Bucket> clients;
    unsigned global_count{0};
    Clock::time_point global_time{};
    bool operator()(const std::string& peer, const Clock::time_point now)
    {
        if (now - global_time >= std::chrono::seconds{1}) { global_time = now; global_count = 0; }
        if (++global_count > 128) return false;
        auto& bucket{clients[peer]};
        if (now - bucket.time >= std::chrono::seconds{1}) { bucket.time = now; bucket.count = 0; }
        return ++bucket.count <= 32;
    }
};

unsigned Admitted(FlowMeshApiAdmission& admission, const std::string& peer, FlowMeshApiRequestClass request_class,
                  unsigned attempts, Clock::time_point now)
{
    unsigned admitted{0};
    for (unsigned i{0}; i < attempts; ++i) {
        admitted += admission.Admit(peer, request_class, now) == FlowMeshApiAdmission::Result::ADMITTED;
    }
    return admitted;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_api_admission_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(api_admission_single_identity_cannot_exhaust_global)
{
    const auto now{Clock::now()};
    // The old code counted refused requests toward the global budget: one
    // flooder at 1,000 requests locked every other client out.
    OldAdmit old;
    for (int i{0}; i < 1000; ++i) old("198.51.100.7", now);
    BOOST_CHECK(!old("203.0.113.9", now));
    // Now a refusal charges nothing: others keep their budgets.
    FlowMeshApiAdmission admission;
    BOOST_CHECK_EQUAL(Admitted(admission, "198.51.100.7", READ, 1000, now), 28U);
    BOOST_CHECK_EQUAL(Admitted(admission, "203.0.113.9", READ, 28, now), 28U);
    BOOST_CHECK_EQUAL(Admitted(admission, "203.0.113.9", WRITE, 4, now), 4U);
}

BOOST_AUTO_TEST_CASE(api_admission_read_flood_leaves_write_floor)
{
    const auto now{Clock::now()};
    FlowMeshApiAdmission admission;
    BOOST_CHECK_EQUAL(Admitted(admission, "192.0.2.1", READ, 100, now), 28U);
    BOOST_CHECK_EQUAL(Admitted(admission, "192.0.2.1", WRITE, 100, now), 4U); // per-identity write floor
    // Globally: reads stop at 112, leaving at least 16 writes.
    FlowMeshApiAdmission global;
    for (int n{0}; n < 8; ++n) Admitted(global, "192.0.2." + std::to_string(10 + n), READ, 28, now);
    BOOST_CHECK_EQUAL(Admitted(global, "192.0.2.100", READ, 28, now), 0U);
    unsigned writes{0};
    for (int n{0}; n < 4; ++n) writes += Admitted(global, "198.51.100." + std::to_string(n), WRITE, 16, now);
    BOOST_CHECK_EQUAL(writes, 16U);
}

BOOST_AUTO_TEST_CASE(api_admission_write_flood_leaves_read_floor)
{
    const auto now{Clock::now()};
    FlowMeshApiAdmission admission;
    BOOST_CHECK_EQUAL(Admitted(admission, "192.0.2.1", WRITE, 100, now), 16U);
    BOOST_CHECK_EQUAL(Admitted(admission, "192.0.2.1", READ, 100, now), 16U); // per-identity read floor
    FlowMeshApiAdmission global;
    for (int n{0}; n < 8; ++n) Admitted(global, "192.0.2." + std::to_string(10 + n), WRITE, 16, now);
    BOOST_CHECK_EQUAL(Admitted(global, "192.0.2.100", WRITE, 16, now), 0U);
    unsigned reads{0};
    for (int n{0}; n < 4; ++n) reads += Admitted(global, "198.51.100." + std::to_string(n), READ, 28, now);
    BOOST_CHECK_EQUAL(reads, 64U);
}

BOOST_AUTO_TEST_CASE(api_admission_totals_never_exceed_old_limits)
{
    const auto now{Clock::now()};
    FlowMeshApiAdmission admission;
    unsigned total{0};
    for (int n{0}; n < 20; ++n) {
        const std::string peer{"10.0.0." + std::to_string(n)};
        unsigned per_peer{Admitted(admission, peer, WRITE, 40, now)};
        per_peer += Admitted(admission, peer, READ, 40, now);
        BOOST_CHECK_LE(per_peer, 32U);
        total += per_peer;
    }
    BOOST_CHECK_EQUAL(total, 128U);
    // The next window starts afresh.
    BOOST_CHECK_EQUAL(Admitted(admission, "10.0.0.0", READ, 1, now + std::chrono::seconds{1}), 1U);
}

BOOST_AUTO_TEST_CASE(api_admission_refusal_charges_nothing)
{
    const auto now{Clock::now()};
    node::FlowMeshApiAdmissionLimits limits;
    limits.identities = 2;
    FlowMeshApiAdmission admission{limits};
    BOOST_CHECK_EQUAL(Admitted(admission, "192.0.2.1", READ, 28, now), 28U);
    BOOST_CHECK(!admission.ChargeExtra("192.0.2.1", READ, now)); // extra tokens obey the same budget
    BOOST_CHECK(admission.ChargeExtra("192.0.2.1", WRITE, now));
    BOOST_CHECK_EQUAL(Admitted(admission, "192.0.2.2", READ, 1, now), 1U);
    // A third identity is refused while both are recent; refused, it charges
    // nothing, and after a minute idle identities make room.
    BOOST_CHECK(admission.Admit("192.0.2.3", READ, now) == FlowMeshApiAdmission::Result::IDENTITIES);
    BOOST_CHECK(admission.Admit("192.0.2.3", READ, now + std::chrono::minutes{2}) ==
                FlowMeshApiAdmission::Result::ADMITTED);
    // Order sub-budget: 12 per identity, 48 globally, nothing on refusal.
    FlowMeshApiAdmission orders;
    unsigned charged{0};
    for (int i{0}; i < 20; ++i) charged += orders.ChargeOrder("192.0.2.9", now);
    BOOST_CHECK_EQUAL(charged, 12U);
    for (int n{0}; n < 10; ++n) {
        for (int i{0}; i < 12; ++i) charged += orders.ChargeOrder("198.51.100." + std::to_string(n), now);
    }
    BOOST_CHECK_EQUAL(charged, 48U);
}

BOOST_AUTO_TEST_CASE(api_admission_classifies_exact_submit_prefix_only)
{
    BOOST_CHECK(node::ClassifyFlowMeshApiRequest(R"({"method":"submit","params":{}})") == WRITE);
    BOOST_CHECK(node::ClassifyFlowMeshApiRequest(R"({"method":"submit")") == WRITE);
    BOOST_CHECK(node::ClassifyFlowMeshApiRequest(R"({"method":"action","params":{}})") == READ);
    BOOST_CHECK(node::ClassifyFlowMeshApiRequest(R"({ "method":"submit"})") == READ);
    BOOST_CHECK(node::ClassifyFlowMeshApiRequest(R"({"params":{},"method":"submit"})") == READ);
    BOOST_CHECK(node::ClassifyFlowMeshApiRequest(R"({"method":"submitx"})") == READ);
    BOOST_CHECK(node::ClassifyFlowMeshApiRequest("") == READ);
}

BOOST_AUTO_TEST_CASE(api_admission_ipv6_slash64_identity)
{
    const auto now{Clock::now()};
    FlowMeshApiAdmission admission;
    // Two addresses in one /64 share one identity; another /64 does not.
    BOOST_CHECK_EQUAL(Admitted(admission, "2001:db8:1:2::1", READ, 20, now), 20U);
    BOOST_CHECK_EQUAL(Admitted(admission, "2001:db8:1:2::ffff", READ, 20, now), 8U);
    BOOST_CHECK_EQUAL(Admitted(admission, "2001:db8:1:3::1", READ, 20, now), 20U);
    // An IPv4-mapped address is its IPv4 identity.
    BOOST_CHECK_EQUAL(Admitted(admission, "192.0.2.44", READ, 20, now), 20U);
    BOOST_CHECK_EQUAL(Admitted(admission, "::ffff:192.0.2.44", READ, 20, now), 8U);
}

BOOST_AUTO_TEST_CASE(trading_api_429_body_byte_identical)
{
    BOOST_CHECK_EQUAL(std::string{node::FLOWMESH_API_BUDGET_EXHAUSTED_BODY},
                      "{\"ok\":false,\"error\":\"Public trading request budget exhausted before admission\"}");
}

BOOST_AUTO_TEST_SUITE_END()
