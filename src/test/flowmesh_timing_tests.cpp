// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <node/flowmesh_timing.h>
#include <boost/test/unit_test.hpp>
#include <array>
#include <mutex>
#include <stdexcept>

namespace {
struct ObservedHold {
    const char* stage{nullptr};
    kernel::LockHoldTimes times{};
};
struct TimingTestState {
    bool enabled{true};
    bool held{false};
    bool emitted_while_held{false};
    size_t enabled_calls{0}, emits{0};
    std::array<ObservedHold, 4> observations{};
};
thread_local TimingTestState* timing_test_state{nullptr};

class TimingTestScope {
    TimingTestState* m_previous;
public:
    explicit TimingTestScope(TimingTestState& state) : m_previous{timing_test_state} { timing_test_state = &state; }
    ~TimingTestScope() { timing_test_state = m_previous; }
};
bool TestTimingEnabled() noexcept
{
    ++timing_test_state->enabled_calls;
    return timing_test_state->enabled;
}
void TestTimingEmit(const char* stage, const kernel::LockHoldTimes& times) noexcept
{
    auto& state{*timing_test_state};
    state.emitted_while_held |= state.held;
    if (state.emits < state.observations.size()) state.observations[state.emits] = {stage, times};
    ++state.emits;
}
const kernel::LockTimingCallbacks TEST_CALLBACKS{TestTimingEnabled, TestTimingEmit};

// The callback can directly observe if it ran before this real mutex unlocked.
class ObservedMutex {
    std::mutex m_mutex;
    TimingTestState& m_state;
public:
    explicit ObservedMutex(TimingTestState& state) : m_state{state} {}
    void lock() { m_mutex.lock(); m_state.held = true; }
    void unlock() { m_mutex.unlock(); m_state.held = false; }
};

void CheckHoldTimes(const kernel::LockHoldTimes& times)
{
    BOOST_CHECK_LE(times.requested_us, times.acquired_us);
    BOOST_CHECK_LE(times.acquired_us, times.releasing_us);
}
void CheckNodeHoldEvent(const UniValue& capture)
{
    BOOST_REQUIRE_EQUAL(capture["events"].size(), 1U);
    const auto& event{capture["events"][0]["event"]};
    BOOST_CHECK_LE(event["lock_requested_us"].getInt<uint64_t>(), event["lock_acquired_us"].getInt<uint64_t>());
    BOOST_CHECK_LE(event["lock_acquired_us"].getInt<uint64_t>(), event["lock_releasing_us"].getInt<uint64_t>());
    BOOST_CHECK_LE(event["lock_releasing_us"].getInt<uint64_t>(), event["monotonic_us"].getInt<uint64_t>());
}
} // namespace

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
BOOST_AUTO_TEST_CASE(lock_hold_release_is_observed_before_deferred_emission)
{
    using namespace node;
    FlowMeshTimingControl("start");
    std::mutex mutex;
    {
        FlowMeshTimingSpan span{"synthetic_lock_hold"};
        span.Mark("lock_requested_us");
        {
            std::lock_guard lock{mutex};
            FlowMeshLockHoldObservation held{span};
        }
        span.Mark("after_unlock_us");
        // Active reads are deliberately refused; timestamp ordering below
        // checks the resulting record. Kernel callback tests separately check
        // that emission happens only after the actual guard has unlocked.
        BOOST_CHECK_THROW(FlowMeshTimingControl("read"), std::runtime_error);
    }
    const auto capture{FlowMeshTimingControl("stop")};
    BOOST_REQUIRE_EQUAL(capture["events"].size(), 1U);
    const auto& event{capture["events"][0]["event"]};
    BOOST_CHECK_LE(event["lock_requested_us"].getInt<uint64_t>(), event["lock_acquired_us"].getInt<uint64_t>());
    BOOST_CHECK_LE(event["lock_acquired_us"].getInt<uint64_t>(), event["lock_releasing_us"].getInt<uint64_t>());
    BOOST_CHECK_LE(event["lock_releasing_us"].getInt<uint64_t>(), event["after_unlock_us"].getInt<uint64_t>());
    BOOST_CHECK_LE(event["after_unlock_us"].getInt<uint64_t>(), event["monotonic_us"].getInt<uint64_t>());
}

BOOST_AUTO_TEST_CASE(node_hold_early_return)
{
    using namespace node;
    FlowMeshTimingControl("start");
    std::mutex mutex;
    const auto operation = [&] {
        FlowMeshTimingSpan span{"early_return_lock_hold"};
        span.Mark("lock_requested_us");
        std::lock_guard lock{mutex};
        FlowMeshLockHoldObservation held{span};
        return 7;
    };
    BOOST_CHECK_EQUAL(operation(), 7);
    CheckNodeHoldEvent(FlowMeshTimingControl("stop"));
    BOOST_REQUIRE(mutex.try_lock());
    mutex.unlock();
}

BOOST_AUTO_TEST_CASE(node_hold_exception_unwind)
{
    using namespace node;
    FlowMeshTimingControl("start");
    std::mutex mutex;
    const auto operation = [&] {
        FlowMeshTimingSpan span{"exception_lock_hold"};
        span.Mark("lock_requested_us");
        std::lock_guard lock{mutex};
        FlowMeshLockHoldObservation held{span};
        throw std::runtime_error("synthetic unwind");
    };
    BOOST_CHECK_THROW(operation(), std::runtime_error);
    CheckNodeHoldEvent(FlowMeshTimingControl("stop"));
    BOOST_REQUIRE(mutex.try_lock());
    mutex.unlock();
}

BOOST_AUTO_TEST_CASE(node_hold_default_off)
{
    using namespace node;
    BOOST_CHECK(!FlowMeshTimingRecording());
    const auto before{FlowMeshTimingControl("read").write()};
    std::mutex mutex;
    {
        FlowMeshTimingSpan span{"disabled_lock_hold"};
        std::lock_guard lock{mutex};
        FlowMeshLockHoldObservation held{span};
    }
    BOOST_CHECK_EQUAL(FlowMeshTimingControl("read").write(), before);
}

BOOST_AUTO_TEST_CASE(kernel_hold_callback_runs_after_unlock)
{
    TimingTestState state;
    TimingTestScope test_scope{state};
    ObservedMutex mutex{state};
    {
        kernel::LockTimingSpan span{TEST_CALLBACKS, "kernel_normal"};
        {
            std::lock_guard lock{mutex};
            kernel::LockHoldObservation held{span};
            BOOST_CHECK_EQUAL(state.emits, 0U);
        }
        BOOST_CHECK_EQUAL(state.emits, 0U);
    }
    BOOST_REQUIRE_EQUAL(state.emits, 1U);
    BOOST_CHECK_EQUAL(state.enabled_calls, 1U);
    BOOST_CHECK(!state.emitted_while_held);
    CheckHoldTimes(state.observations[0].times);
}

BOOST_AUTO_TEST_CASE(kernel_hold_early_return_and_exception)
{
    TimingTestState state;
    TimingTestScope test_scope{state};
    ObservedMutex mutex{state};
    const auto return_early = [&] {
        kernel::LockTimingSpan span{TEST_CALLBACKS, "kernel_return"};
        std::lock_guard lock{mutex};
        kernel::LockHoldObservation held{span};
        return 9;
    };
    const auto throw_early = [&] {
        kernel::LockTimingSpan span{TEST_CALLBACKS, "kernel_throw"};
        std::lock_guard lock{mutex};
        kernel::LockHoldObservation held{span};
        throw std::runtime_error("synthetic unwind");
    };
    BOOST_CHECK_EQUAL(return_early(), 9);
    BOOST_CHECK_THROW(throw_early(), std::runtime_error);
    BOOST_REQUIRE_EQUAL(state.emits, 2U);
    BOOST_CHECK(!state.emitted_while_held);
    BOOST_CHECK(!state.held);
    CheckHoldTimes(state.observations[0].times);
    CheckHoldTimes(state.observations[1].times);
}

BOOST_AUTO_TEST_CASE(kernel_hold_null_and_disabled_callbacks)
{
    TimingTestState state;
    TimingTestScope test_scope{state};
    ObservedMutex mutex{state};
    for (const auto callbacks : {kernel::LockTimingCallbacks{},
                                 kernel::LockTimingCallbacks{TestTimingEnabled, nullptr},
                                 kernel::LockTimingCallbacks{nullptr, TestTimingEmit}}) {
        kernel::LockTimingSpan span{callbacks, "kernel_null"};
        std::lock_guard lock{mutex};
        kernel::LockHoldObservation held{span};
    }
    BOOST_CHECK_EQUAL(state.enabled_calls, 0U);
    BOOST_CHECK_EQUAL(state.emits, 0U);
    state.enabled = false;
    {
        kernel::LockTimingSpan span{TEST_CALLBACKS, "kernel_disabled"};
        std::lock_guard lock{mutex};
        kernel::LockHoldObservation held{span};
    }
    BOOST_CHECK_EQUAL(state.enabled_calls, 1U);
    BOOST_CHECK_EQUAL(state.emits, 0U);
}

BOOST_AUTO_TEST_CASE(kernel_hold_missing_acquisition_does_not_emit)
{
    TimingTestState state;
    TimingTestScope test_scope{state};
    { kernel::LockTimingSpan span{TEST_CALLBACKS, "kernel_not_acquired"}; }
    BOOST_CHECK_EQUAL(state.enabled_calls, 1U);
    BOOST_CHECK_EQUAL(state.emits, 0U);
}

BOOST_AUTO_TEST_CASE(kernel_activation_and_flush_are_separate_holds)
{
    TimingTestState state;
    TimingTestScope test_scope{state};
    ObservedMutex mutex{state};
    {
        kernel::LockTimingSpan span{TEST_CALLBACKS, "b3_chain_activation_lock"};
        std::lock_guard lock{mutex};
        kernel::LockHoldObservation held{span};
    }
    {
        kernel::LockTimingSpan span{TEST_CALLBACKS, "b3_chain_periodic_flush_lock"};
        std::lock_guard lock{mutex};
        kernel::LockHoldObservation held{span};
    }
    BOOST_REQUIRE_EQUAL(state.emits, 2U);
    BOOST_CHECK_EQUAL(state.observations[0].stage, "b3_chain_activation_lock");
    BOOST_CHECK_EQUAL(state.observations[1].stage, "b3_chain_periodic_flush_lock");
    CheckHoldTimes(state.observations[0].times);
    CheckHoldTimes(state.observations[1].times);
    BOOST_CHECK_LE(state.observations[0].times.releasing_us, state.observations[1].times.requested_us);
    BOOST_CHECK(!state.emitted_while_held);
}

BOOST_AUTO_TEST_CASE(kernel_callback_connects_to_regtest_capture)
{
    using namespace node;
    FlowMeshTimingControl("start");
    std::mutex mutex;
    {
        kernel::LockTimingSpan span{FlowMeshChainLockTimingCallbacks(), "kernel_capture"};
        std::lock_guard lock{mutex};
        kernel::LockHoldObservation held{span};
    }
    const auto capture{FlowMeshTimingControl("stop")};
    CheckNodeHoldEvent(capture);
    BOOST_CHECK_EQUAL(capture["events"][0]["event"]["stage"].get_str(), "kernel_capture");
    BOOST_CHECK_EQUAL(capture["events"][0]["event"]["lock_name"].get_str(), "cs_main");
}
BOOST_AUTO_TEST_SUITE_END()
