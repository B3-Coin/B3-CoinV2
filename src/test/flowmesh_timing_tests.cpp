// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <node/flowmesh_timing.h>
#include <boost/test/unit_test.hpp>
#include <stdexcept>

BOOST_AUTO_TEST_SUITE(flowmesh_timing_tests)
BOOST_AUTO_TEST_CASE(default_off_nested_frozen_bounded_capture)
{
    using namespace node;
    BOOST_CHECK(!FlowMeshTimingRecording());
    { FlowMeshTimingSpan ignored{"disabled"}; }
    BOOST_CHECK_EQUAL(FlowMeshTimingControl("read")["events"].size(), 0U);
    FlowMeshTimingControl("start");
    BOOST_CHECK_THROW(FlowMeshTimingControl("start"), std::runtime_error);
    BOOST_CHECK_THROW(FlowMeshTimingControl("read"), std::runtime_error);
    {
        FlowMeshTimingSpan outer{"outer"};
        outer.Mark("lock_requested_us");
        { FlowMeshTimingSpan inner{"inner"}; inner.Field("count", uint64_t{3}); }
    }
    auto capture = FlowMeshTimingControl("stop");
    BOOST_CHECK(!FlowMeshTimingRecording());
    BOOST_REQUIRE_EQUAL(capture["events"].size(), 2U);
    const auto& inner = capture["events"][0]["event"];
    const auto& outer = capture["events"][1]["event"];
    BOOST_CHECK_EQUAL(inner["parent_span_id"].getInt<uint64_t>(), outer["span_id"].getInt<uint64_t>());
    BOOST_CHECK_LE(outer["started_us"].getInt<uint64_t>(), inner["started_us"].getInt<uint64_t>());
    BOOST_CHECK_LE(inner["monotonic_us"].getInt<uint64_t>(), outer["monotonic_us"].getInt<uint64_t>());
    const auto frozen = FlowMeshTimingControl("read").write();
    { FlowMeshTimingSpan ignored{"after_stop"}; }
    BOOST_CHECK_EQUAL(FlowMeshTimingControl("read").write(), frozen);
    FlowMeshTimingControl("start");
    UniValue row{UniValue::VOBJ}; row.pushKV("synthetic", true);
    for (size_t i{0}; i < 32769; ++i) FlowMeshTimingEmit("test", row);
    capture = FlowMeshTimingControl("stop");
    BOOST_CHECK_EQUAL(capture["events"].size(), 32768U);
    BOOST_CHECK_EQUAL(capture["dropped"].getInt<uint64_t>(), 1U);
    FlowMeshTimingControl("start");
    row.pushKV("synthetic_payload", std::string(1024 * 1024, 'x'));
    for (size_t i{0}; i < 17; ++i) FlowMeshTimingEmit("test", row);
    capture = FlowMeshTimingControl("stop");
    BOOST_CHECK_LE(capture["bytes"].getInt<uint64_t>(), capture["max_bytes"].getInt<uint64_t>());
    BOOST_CHECK_GT(capture["dropped"].getInt<uint64_t>(), 0U);
    BOOST_CHECK_THROW(FlowMeshTimingControl("erase"), std::runtime_error);
}
BOOST_AUTO_TEST_SUITE_END()
