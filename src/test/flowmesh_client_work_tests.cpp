// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <node/flowmesh_client_work.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using Gate = node::FlowMeshClientWorkGate;
using Priority = node::FlowMeshClientWorkPriority;
using Scope = node::FlowMeshClientWorkScope;
using namespace std::chrono_literals;

struct Threads {
    std::vector<std::thread> values;
    void Join() { for (auto& thread : values) if (thread.joinable()) thread.join(); }
    ~Threads() { Join(); }
};

// The initial owner prevents all queued callers from running until their
// ordering is established through a CV observation, not sleeps or luck.
struct OrderedRun {
    Gate gate;
    std::mutex record_mutex;
    std::vector<int> order;
    std::atomic<unsigned> active{0}, maximum_active{0};
    Threads threads;
    // Destroy before threads: a failed test assertion must release the owner
    // before joining, without destroying data still referenced by the workers.
    std::unique_lock<Gate> initial{gate};

    bool Add(Priority priority, int id, size_t foreground, size_t passive)
    {
        threads.values.emplace_back([&, priority, id] {
            Scope scope{priority};
            std::lock_guard lock{gate};
            const auto owners{active.fetch_add(1) + 1};
            auto previous{maximum_active.load()};
            while (previous < owners && !maximum_active.compare_exchange_weak(previous, owners)) {}
            {
                std::lock_guard record{record_mutex};
                order.push_back(id);
            }
            --active;
        });
        return gate.WaitForQueuedForTest(foreground, passive, 5s);
    }

    void Finish()
    {
        initial.unlock();
        threads.Join();
    }
};

// Linger cases hold the gate directly. A failed assertion still releases the
// owner and ends any window before joining, so no case waits out a long window.
// Declare state that queued work uses BEFORE the run: locals are destroyed in
// reverse order, and the run's destructor joins the workers that use them.
struct LingerRun {
    Gate gate;
    Threads threads;
    std::unique_lock<Gate> owner{gate, std::defer_lock};

    explicit LingerRun(std::chrono::milliseconds linger) : gate{linger} {}
    ~LingerRun()
    {
        if (owner.owns_lock()) owner.unlock();
        gate.ExpireLingerForTest();
        threads.Join();
    }

    void Queue(Priority priority, std::function<void()> work)
    {
        threads.values.emplace_back([this, priority, work = std::move(work)] {
            Scope scope{priority};
            std::lock_guard lock{gate};
            work();
        });
    }
};
} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_client_work_tests)

BOOST_AUTO_TEST_CASE(scope_defaults_foreground_and_restores_nested_context)
{
    BOOST_CHECK(Scope::Current() == Priority::FOREGROUND);
    {
        Scope passive{Priority::PASSIVE};
        BOOST_CHECK(Scope::Current() == Priority::PASSIVE);
        {
            Scope foreground{Priority::FOREGROUND};
            BOOST_CHECK(Scope::Current() == Priority::FOREGROUND);
            {
                Scope nested{Priority::PASSIVE};
                BOOST_CHECK(Scope::Current() == Priority::PASSIVE);
            }
            BOOST_CHECK(Scope::Current() == Priority::FOREGROUND);
        }
        BOOST_CHECK(Scope::Current() == Priority::PASSIVE);
    }
    BOOST_CHECK(Scope::Current() == Priority::FOREGROUND);
}

BOOST_AUTO_TEST_CASE(scope_is_thread_local_and_not_inherited_by_a_worker)
{
    Scope passive{Priority::PASSIVE};
    std::atomic<bool> child_default{false}, child_restore{false};
    std::thread worker{[&] {
        child_default = Scope::Current() == Priority::FOREGROUND;
        { Scope own_passive{Priority::PASSIVE}; }
        child_restore = Scope::Current() == Priority::FOREGROUND;
    }};
    worker.join();
    BOOST_CHECK(child_default.load());
    BOOST_CHECK(child_restore.load());
    BOOST_CHECK(Scope::Current() == Priority::PASSIVE);
}

BOOST_AUTO_TEST_CASE(exception_releases_ownership_and_restores_request_context)
{
    Gate gate;
    const auto fail = [&] {
        Scope passive{Priority::PASSIVE};
        std::lock_guard lock{gate};
        throw std::runtime_error{"synthetic request failure"};
    };
    BOOST_CHECK_THROW(fail(), std::runtime_error);
    BOOST_CHECK(Scope::Current() == Priority::FOREGROUND);
    const bool acquired{gate.try_lock()};
    BOOST_CHECK(acquired);
    if (acquired) gate.unlock();
    const auto observation{gate.Inspect()};
    BOOST_CHECK(!observation.active);
    BOOST_CHECK_EQUAL(observation.foreground, 0U);
    BOOST_CHECK_EQUAL(observation.passive, 0U);
}

BOOST_AUTO_TEST_CASE(active_passive_request_is_not_preempted)
{
    Scope passive{Priority::PASSIVE};
    OrderedRun run;
    BOOST_REQUIRE(run.Add(Priority::FOREGROUND, 1, 1, 0));
    BOOST_REQUIRE(run.Add(Priority::PASSIVE, 2, 1, 1));
    const auto held{run.gate.Inspect()};
    BOOST_CHECK(held.active);
    { std::lock_guard lock{run.record_mutex}; BOOST_CHECK(run.order.empty()); }
    BOOST_CHECK_EQUAL(run.maximum_active.load(), 0U);
    run.Finish();
    const std::vector<int> expected{1, 2};
    BOOST_CHECK_EQUAL_COLLECTIONS(run.order.begin(), run.order.end(), expected.begin(), expected.end());
    BOOST_CHECK_EQUAL(run.maximum_active.load(), 1U);
}

BOOST_AUTO_TEST_CASE(queued_foreground_precedes_older_passive_requests)
{
    OrderedRun run;
    BOOST_REQUIRE(run.Add(Priority::PASSIVE, 10, 0, 1));
    BOOST_REQUIRE(run.Add(Priority::PASSIVE, 11, 0, 2));
    BOOST_REQUIRE(run.Add(Priority::FOREGROUND, 20, 1, 2));
    BOOST_REQUIRE(run.Add(Priority::FOREGROUND, 21, 2, 2));
    run.Finish();
    const std::vector<int> expected{20, 21, 10, 11};
    BOOST_CHECK_EQUAL_COLLECTIONS(run.order.begin(), run.order.end(), expected.begin(), expected.end());
    BOOST_CHECK_EQUAL(run.maximum_active.load(), 1U);
}

BOOST_AUTO_TEST_CASE(foreground_callers_are_fifo)
{
    OrderedRun run;
    for (int i{0}; i < 12; ++i) BOOST_REQUIRE(run.Add(Priority::FOREGROUND, i, i + 1, 0));
    run.Finish();
    BOOST_REQUIRE_EQUAL(run.order.size(), 12U);
    for (int i{0}; i < 12; ++i) BOOST_CHECK_EQUAL(run.order[i], i);
    BOOST_CHECK_EQUAL(run.maximum_active.load(), 1U);
}

BOOST_AUTO_TEST_CASE(passive_callers_are_fifo)
{
    OrderedRun run;
    for (int i{0}; i < 12; ++i) BOOST_REQUIRE(run.Add(Priority::PASSIVE, i, 0, i + 1));
    run.Finish();
    BOOST_REQUIRE_EQUAL(run.order.size(), 12U);
    for (int i{0}; i < 12; ++i) BOOST_CHECK_EQUAL(run.order[i], i);
    BOOST_CHECK_EQUAL(run.maximum_active.load(), 1U);
}

BOOST_AUTO_TEST_CASE(waiting_passive_runs_after_bounded_foreground_burst)
{
    OrderedRun run;
    BOOST_REQUIRE(run.Add(Priority::PASSIVE, 100, 0, 1));
    BOOST_REQUIRE(run.Add(Priority::PASSIVE, 101, 0, 2));
    constexpr size_t FOREGROUND{2 * Gate::MAX_FOREGROUND_BURST + 1};
    for (size_t i{0}; i < FOREGROUND; ++i) BOOST_REQUIRE(run.Add(Priority::FOREGROUND, i, i + 1, 2));
    run.Finish();
    std::vector<int> expected;
    for (size_t i{0}; i < FOREGROUND; ++i) {
        expected.push_back(i);
        if (i + 1 == Gate::MAX_FOREGROUND_BURST) expected.push_back(100);
        if (i + 1 == 2 * Gate::MAX_FOREGROUND_BURST) expected.push_back(101);
    }
    BOOST_CHECK_EQUAL_COLLECTIONS(run.order.begin(), run.order.end(), expected.begin(), expected.end());
    BOOST_CHECK_EQUAL(run.maximum_active.load(), 1U);
    const auto drained{run.gate.Inspect()};
    BOOST_CHECK(!drained.active);
    BOOST_CHECK_EQUAL(drained.foreground, 0U);
    BOOST_CHECK_EQUAL(drained.passive, 0U);
}

BOOST_AUTO_TEST_CASE(uncontended_foreground_does_not_spend_future_refresh_allowance)
{
    OrderedRun run;
    // No passive waiter existed for these prior acquisitions. Once contention
    // starts, the trade still gets its bounded foreground preference.
    run.initial.unlock();
    for (size_t i{0}; i < 2 * Gate::MAX_FOREGROUND_BURST; ++i) {
        std::lock_guard lock{run.gate};
    }
    run.initial.lock();
    BOOST_REQUIRE(run.Add(Priority::PASSIVE, 1, 0, 1));
    BOOST_REQUIRE(run.Add(Priority::FOREGROUND, 2, 1, 1));
    run.Finish();
    const std::vector<int> expected{2, 1};
    BOOST_CHECK_EQUAL_COLLECTIONS(run.order.begin(), run.order.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(try_lock_is_prompt_and_does_not_barge_a_waiter)
{
    Gate gate;
    std::mutex mutex;
    std::condition_variable condition;
    bool acquired{false}, release{false};
    gate.lock();
    BOOST_CHECK(!gate.try_lock());
    std::thread passive{[&] {
        Scope scope{Priority::PASSIVE};
        std::lock_guard work{gate};
        std::unique_lock lock{mutex};
        acquired = true;
        condition.notify_all();
        condition.wait(lock, [&] { return release; });
    }};
    const bool queued{gate.WaitForQueuedForTest(0, 1, 5s)};
    gate.unlock();
    // The waiter is either queued or already owns the gate. Keep it held until
    // after this attempt, so successful try_lock cannot be an ordinary idle gap.
    const bool barged{gate.try_lock()};
    if (barged) gate.unlock();
    bool observed{false};
    {
        std::unique_lock lock{mutex};
        observed = condition.wait_for(lock, 5s, [&] { return acquired; });
        release = true;
    }
    condition.notify_all();
    passive.join();
    BOOST_CHECK(queued);
    BOOST_CHECK(observed);
    BOOST_CHECK(!barged);
    const bool idle{gate.try_lock()};
    BOOST_CHECK(idle);
    if (idle) gate.unlock();
}

BOOST_AUTO_TEST_CASE(foreground_release_reserves_gate_for_next_foreground)
{
    // A long window proves the reservation; the test ends it explicitly.
    std::atomic<bool> passive_owned{false};
    LingerRun run{10s};
    run.owner.lock();
    run.Queue(Priority::PASSIVE, [&] { passive_owned = true; });
    BOOST_REQUIRE(run.gate.WaitForQueuedForTest(0, 1, 5s));
    run.owner.unlock();
    BOOST_CHECK(run.gate.Inspect().lingering);
    // The trade's next step is served before the queued refresh.
    run.owner.lock();
    const auto captured{run.gate.Inspect()};
    BOOST_CHECK(captured.active);
    BOOST_CHECK_EQUAL(captured.passive, 1U);
    BOOST_CHECK(captured.owner_foreground);
    BOOST_CHECK(captured.owner_linger_window);
    BOOST_CHECK_EQUAL(captured.linger_captures, 1U);
    BOOST_CHECK(!passive_owned.load());
    run.owner.unlock();
    run.gate.ExpireLingerForTest();
    run.threads.Join();
    BOOST_CHECK(passive_owned.load());
    const auto drained{run.gate.Inspect()};
    BOOST_CHECK(!drained.active);
    BOOST_CHECK_EQUAL(drained.passive, 0U);
    BOOST_CHECK_EQUAL(drained.linger_captures, 1U);
    BOOST_CHECK_EQUAL(drained.linger_expired_with_passive_waiting, 1U);
    BOOST_CHECK_EQUAL(drained.burst_forced_passive_turns, 0U);
}

BOOST_AUTO_TEST_CASE(passive_runs_when_foreground_linger_expires)
{
    using Clock = std::chrono::steady_clock;
    Clock::time_point acquired{};
    LingerRun run{Gate::FOREGROUND_LINGER};
    run.owner.lock();
    run.Queue(Priority::PASSIVE, [&] { acquired = Clock::now(); });
    BOOST_REQUIRE(run.gate.WaitForQueuedForTest(0, 1, 5s));
    const auto released{Clock::now()};
    run.owner.unlock();
    run.threads.Join();
    // No notification marks expiry: the waiter's own deadline releases it.
    BOOST_CHECK(acquired - released >= Gate::FOREGROUND_LINGER);
    BOOST_CHECK(acquired - released < 5s);
    const auto observed{run.gate.Inspect()};
    BOOST_CHECK(!observed.lingering);
    BOOST_CHECK_EQUAL(observed.linger_captures, 0U);
    BOOST_CHECK_EQUAL(observed.linger_expired_with_passive_waiting, 1U);
    BOOST_CHECK_EQUAL(observed.burst_forced_passive_turns, 0U);
}

BOOST_AUTO_TEST_CASE(burst_limit_overrides_foreground_linger)
{
    std::mutex record_mutex;
    std::vector<int> order;
    const auto record = [&](int id) {
        std::lock_guard lock{record_mutex};
        order.push_back(id);
    };
    LingerRun run{10s};
    run.owner.lock();
    run.Queue(Priority::PASSIVE, [&] { record(100); });
    BOOST_REQUIRE(run.gate.WaitForQueuedForTest(0, 1, 5s));
    constexpr int BURST{static_cast<int>(Gate::MAX_FOREGROUND_BURST)};
    for (int i{1}; i <= BURST; ++i) {
        run.owner.unlock();
        run.owner.lock();
        record(i);
    }
    const auto spent{run.gate.Inspect()};
    BOOST_CHECK_EQUAL(spent.passive, 1U);
    BOOST_CHECK_EQUAL(spent.linger_captures, Gate::MAX_FOREGROUND_BURST);
    // The spent allowance ends the window: the refresh runs before the next
    // foreground step, even though that step is already asking.
    run.owner.unlock();
    run.owner.lock();
    record(BURST + 1);
    run.owner.unlock();
    run.threads.Join();
    std::vector<int> expected;
    for (int i{1}; i <= BURST; ++i) expected.push_back(i);
    expected.push_back(100);
    expected.push_back(BURST + 1);
    BOOST_CHECK_EQUAL_COLLECTIONS(order.begin(), order.end(), expected.begin(), expected.end());
    const auto drained{run.gate.Inspect()};
    BOOST_CHECK_EQUAL(drained.burst_forced_passive_turns, 1U);
    BOOST_CHECK_EQUAL(drained.linger_expired_with_passive_waiting, 0U);
    BOOST_CHECK_EQUAL(drained.linger_captures, Gate::MAX_FOREGROUND_BURST);
}

BOOST_AUTO_TEST_CASE(passive_release_does_not_linger)
{
    using Clock = std::chrono::steady_clock;
    std::atomic<bool> owned{false};
    LingerRun run{10s};
    {
        Scope passive{Priority::PASSIVE};
        run.owner.lock();
    }
    run.Queue(Priority::PASSIVE, [&] { owned = true; });
    BOOST_REQUIRE(run.gate.WaitForQueuedForTest(0, 1, 5s));
    const auto released{Clock::now()};
    run.owner.unlock();
    BOOST_CHECK(!run.gate.Inspect().lingering);
    run.threads.Join();
    BOOST_CHECK(owned.load());
    BOOST_CHECK(Clock::now() - released < 5s);
    const auto observed{run.gate.Inspect()};
    BOOST_CHECK_EQUAL(observed.linger_expired_with_passive_waiting, 0U);
    BOOST_CHECK_EQUAL(observed.burst_forced_passive_turns, 0U);
}

BOOST_AUTO_TEST_CASE(try_lock_during_linger)
{
    std::atomic<bool> owned{false};
    LingerRun run{10s};
    run.owner.lock();
    run.owner.unlock();
    BOOST_CHECK(run.gate.Inspect().lingering);
    // With no queued caller, a probe is still prompt inside the window.
    BOOST_REQUIRE(run.owner.try_lock());
    BOOST_CHECK(run.gate.Inspect().owner_linger_window);
    run.owner.unlock();
    run.Queue(Priority::PASSIVE, [&] { owned = true; });
    BOOST_REQUIRE(run.gate.WaitForQueuedForTest(0, 1, 5s));
    // The window holds the queued refresh, and the probe does not barge it.
    const bool barged{run.owner.try_lock()};
    BOOST_CHECK(!barged);
    if (barged) run.owner.unlock();
    BOOST_CHECK(!owned.load());
    run.gate.ExpireLingerForTest();
    run.threads.Join();
    BOOST_CHECK(owned.load());
    const bool idle{run.owner.try_lock()};
    BOOST_CHECK(idle);
    if (idle) run.owner.unlock();
}

BOOST_AUTO_TEST_CASE(zero_linger_reproduces_prior_handoff)
{
    std::mutex mutex;
    std::condition_variable condition;
    bool owned{false}, release{false};
    LingerRun run{0ms};
    run.owner.lock();
    run.Queue(Priority::PASSIVE, [&] {
        std::unique_lock lock{mutex};
        owned = true;
        condition.notify_all();
        // Bounded: after a failed assertion nothing sets release, and the
        // run's destructor joins this worker.
        condition.wait_for(lock, 5s, [&] { return release; });
    });
    BOOST_REQUIRE(run.gate.WaitForQueuedForTest(0, 1, 5s));
    run.owner.unlock();
    // No reservation: the queued refresh takes the gate at the release.
    BOOST_CHECK(!run.gate.Inspect().lingering);
    bool observed{false};
    {
        std::unique_lock lock{mutex};
        observed = condition.wait_for(lock, 5s, [&] { return owned; });
        release = true;
    }
    condition.notify_all();
    run.threads.Join();
    BOOST_CHECK(observed);
    const auto drained{run.gate.Inspect()};
    BOOST_CHECK_EQUAL(drained.linger_captures, 0U);
    BOOST_CHECK_EQUAL(drained.linger_expired_with_passive_waiting, 0U);
}

BOOST_AUTO_TEST_SUITE_END()
