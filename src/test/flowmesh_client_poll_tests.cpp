// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <node/flowmesh_client_poll.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <chrono>
#include <cstdint>

namespace {
using Poll = node::FlowMeshClientPollScheduler;
using namespace std::chrono_literals;

uint256 Id(uint64_t value)
{
    uint256 out;
    for (unsigned byte{0}; byte < sizeof(value); ++byte) out.begin()[byte] = static_cast<unsigned char>(value >> (8 * byte));
    return out;
}

Poll::Key Key(uint64_t market, uint64_t action)
{
    return {Id(market), Id(action)};
}
} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_client_poll_tests)

BOOST_AUTO_TEST_CASE(first_query_is_demand_not_a_cached_observation)
{
    Poll poll;
    const auto key{Key(1, 2)};
    const Poll::TimePoint now{};
    BOOST_CHECK(!poll.HasObservation(key));
    BOOST_REQUIRE(poll.Demand(key));
    const auto selected{poll.Take(now)};
    BOOST_REQUIRE(selected);
    BOOST_CHECK(*selected == key);
    BOOST_CHECK(!poll.HasObservation(key));
    BOOST_CHECK_EQUAL(poll.AttemptsInWindow(now), 0U);
    BOOST_REQUIRE(poll.TryChargeAttempt(now));
    BOOST_CHECK(!poll.HasObservation(key));
    BOOST_REQUIRE(poll.MarkObserved(key));
    BOOST_CHECK(poll.HasObservation(key));
    // No automatic requeue or network charge merely because a receipt exists.
    BOOST_CHECK(!poll.Take(now));
    BOOST_CHECK_EQUAL(poll.AttemptsInWindow(now), 1U);
}

BOOST_AUTO_TEST_CASE(fifo_fairness_across_exact_market_action_keys)
{
    Poll poll;
    const std::array keys{Key(1, 7), Key(2, 7), Key(1, 8), Key(2, 8)};
    const Poll::TimePoint start{};
    for (const auto& key : keys) BOOST_REQUIRE(poll.Demand(key));
    for (unsigned repeat{0}; repeat < 1000; ++repeat) BOOST_REQUIRE(poll.Demand(keys.front()));
    BOOST_CHECK_EQUAL(poll.DemandCount(), keys.size());
    BOOST_CHECK_EQUAL(poll.KeyCount(), keys.size());
    for (size_t i{0}; i < keys.size(); ++i) {
        const auto now{start + Poll::REFILL_INTERVAL * i};
        const auto selected{poll.Take(now)};
        BOOST_REQUIRE(selected);
        BOOST_CHECK(*selected == keys[i]);
        BOOST_REQUIRE(poll.TryChargeAttempt(now));
        BOOST_REQUIRE(poll.MarkObserved(*selected));
        // Even an immediately repeated hot caller rejoins at the back.
        BOOST_REQUIRE(poll.Demand(keys.front()));
    }
    const auto selected{poll.Take(start + Poll::REFILL_INTERVAL * keys.size())};
    BOOST_REQUIRE(selected);
    BOOST_CHECK(*selected == keys.front());
    // Same bare ActionId in a different market has independent observations.
    poll.Remove(keys[1]);
    BOOST_CHECK(!poll.HasObservation(keys[1]));
    BOOST_CHECK(poll.HasObservation(keys[0]));
}

BOOST_AUTO_TEST_CASE(each_failed_or_successful_endpoint_attempt_is_charged)
{
    Poll poll;
    const Poll::TimePoint start{};
    BOOST_REQUIRE(poll.Demand(Key(1, 1)));
    BOOST_REQUIRE(poll.Take(start));
    // Model an unsuccessful first endpoint and a successful second endpoint.
    BOOST_CHECK(poll.TryChargeAttempt(start));
    BOOST_CHECK(poll.TryChargeAttempt(start));
    BOOST_CHECK(!poll.TryChargeAttempt(start));
    BOOST_CHECK_EQUAL(poll.AttemptsInWindow(start), 2U);
    BOOST_CHECK(!poll.CanAttempt(start + Poll::REFILL_INTERVAL - 1us));
    BOOST_CHECK(poll.CanAttempt(start + Poll::REFILL_INTERVAL));
    BOOST_CHECK(poll.TryChargeAttempt(start + Poll::REFILL_INTERVAL));
    BOOST_CHECK(!poll.TryChargeAttempt(start + Poll::REFILL_INTERVAL));
    // Take itself has not reserved or charged an additional attempt.
    BOOST_CHECK_EQUAL(poll.AttemptsInWindow(start + Poll::REFILL_INTERVAL), 3U);
}

BOOST_AUTO_TEST_CASE(waited_fast_path_yields_to_older_ordinary_demand)
{
    Poll poll;
    // Equal bare ActionIds in different markets must remain distinct.
    const auto hot{Key(1, 7)}, older{Key(2, 7)}, next{Key(2, 8)};
    const Poll::TimePoint start{};
    BOOST_REQUIRE(poll.TryChargeUnqueuedAttempt(start));
    BOOST_REQUIRE(poll.TryChargeUnqueuedAttempt(start));
    BOOST_REQUIRE(poll.Demand(older));
    BOOST_REQUIRE(poll.Demand(next));
    BOOST_CHECK(!poll.Take(start));

    const std::array expected{older, next, hot};
    for (size_t i{0}; i < expected.size(); ++i) {
        const auto now{start + Poll::REFILL_INTERVAL * (i + 1)};
        // A hot waited caller arrives first at every token refill. Before
        // the queue guard, it spends that token on itself and starves Take.
        BOOST_CHECK(!poll.TryChargeUnqueuedAttempt(now));
        BOOST_CHECK_EQUAL(poll.AttemptsInWindow(now), i + 2);
        // Model ActionStatus's same-call ordinary fallback: enqueue only
        // its own key, then service the oldest demand, not necessarily itself.
        BOOST_REQUIRE(poll.Demand(hot));
        const auto selected{poll.Take(now)};
        BOOST_REQUIRE(selected);
        BOOST_CHECK(*selected == expected[i]);
        BOOST_REQUIRE(poll.TryChargeAttempt(now));
        BOOST_REQUIRE(poll.MarkObserved(*selected));
        BOOST_CHECK_EQUAL(poll.HasObservation(hot), i == expected.size() - 1);
        BOOST_CHECK(!poll.Take(now));
    }
    BOOST_CHECK_EQUAL(poll.DemandCount(), 0U);
    BOOST_CHECK(poll.TryChargeUnqueuedAttempt(start + 4 * Poll::REFILL_INTERVAL));
    BOOST_CHECK_EQUAL(poll.AttemptsInWindow(start + 4 * Poll::REFILL_INTERVAL), 6U);
}

BOOST_AUTO_TEST_CASE(waited_fast_path_yields_even_when_only_its_own_key_is_queued)
{
    Poll poll;
    const auto key{Key(1, 7)};
    const Poll::TimePoint now{};
    BOOST_REQUIRE(poll.Demand(key));
    BOOST_CHECK(!poll.TryChargeUnqueuedAttempt(now));
    BOOST_CHECK_EQUAL(poll.AttemptsInWindow(now), 0U);
    BOOST_REQUIRE(poll.Demand(key));
    BOOST_CHECK_EQUAL(poll.DemandCount(), 1U);
    const auto selected{poll.Take(now)};
    BOOST_REQUIRE(selected);
    BOOST_CHECK(*selected == key);
    BOOST_REQUIRE(poll.TryChargeAttempt(now));
    BOOST_CHECK_EQUAL(poll.DemandCount(), 0U);
    BOOST_CHECK(!poll.HasObservation(key));
    BOOST_CHECK(poll.TryChargeUnqueuedAttempt(now));
    BOOST_CHECK(!poll.TryChargeUnqueuedAttempt(now));
    BOOST_CHECK_EQUAL(poll.AttemptsInWindow(now), 2U);
}

BOOST_AUTO_TEST_CASE(waited_and_ordinary_attempts_share_one_rolling_budget)
{
    Poll poll;
    const Poll::TimePoint start{};
    BOOST_REQUIRE(poll.TryChargeUnqueuedAttempt(start));
    BOOST_REQUIRE(poll.TryChargeAttempt(start));
    for (size_t i{1}; i <= 14; ++i) {
        const auto now{start + Poll::REFILL_INTERVAL * i};
        BOOST_REQUIRE(i % 2 ? poll.TryChargeUnqueuedAttempt(now) : poll.TryChargeAttempt(now));
    }
    BOOST_CHECK(!poll.TryChargeUnqueuedAttempt(start + Poll::REFILL_INTERVAL * 15));
    BOOST_CHECK(!poll.TryChargeAttempt(start + Poll::WINDOW - 1us));
    BOOST_CHECK_EQUAL(poll.AttemptsInWindow(start + Poll::WINDOW - 1us), 16U);
    BOOST_CHECK(poll.TryChargeUnqueuedAttempt(start + Poll::WINDOW));
    BOOST_CHECK(poll.TryChargeAttempt(start + Poll::WINDOW));
    BOOST_CHECK(!poll.TryChargeUnqueuedAttempt(start + Poll::WINDOW));
    BOOST_CHECK_EQUAL(poll.AttemptsInWindow(start + Poll::WINDOW), 16U);
}

BOOST_AUTO_TEST_CASE(smooth_refill_still_obeys_rolling_sixteen_attempt_cap)
{
    Poll poll;
    const Poll::TimePoint start{};
    BOOST_REQUIRE(poll.TryChargeAttempt(start));
    BOOST_REQUIRE(poll.TryChargeAttempt(start));
    for (size_t i{1}; i <= 14; ++i) {
        BOOST_REQUIRE(poll.TryChargeAttempt(start + Poll::REFILL_INTERVAL * i));
    }
    // A replenished token is insufficient while sixteen attempts remain live.
    BOOST_CHECK(!poll.TryChargeAttempt(start + Poll::REFILL_INTERVAL * 15));
    BOOST_CHECK(!poll.TryChargeAttempt(start + Poll::WINDOW - 1us));
    BOOST_CHECK_EQUAL(poll.AttemptsInWindow(start + Poll::WINDOW - 1us), 16U);
    BOOST_CHECK(poll.TryChargeAttempt(start + Poll::WINDOW));
    BOOST_CHECK(poll.TryChargeAttempt(start + Poll::WINDOW));
    BOOST_CHECK(!poll.TryChargeAttempt(start + Poll::WINDOW));
    BOOST_CHECK_EQUAL(poll.AttemptsInWindow(start + Poll::WINDOW), 16U);
}

BOOST_AUTO_TEST_CASE(removal_does_not_refund_attempts_or_keep_a_cached_observation)
{
    Poll poll;
    const auto key{Key(1, 1)};
    const Poll::TimePoint now{};
    // The retained instruction belongs to the backend, not to this scheduler.
    // Backend no-resubmission requires its own regression; this helper cannot
    // erase an instruction or manufacture a receipt/permission to send one.
    BOOST_REQUIRE(poll.Demand(key));
    BOOST_REQUIRE(poll.MarkObserved(key));
    BOOST_REQUIRE(poll.TryChargeAttempt(now));
    BOOST_REQUIRE(poll.TryChargeAttempt(now));
    poll.Remove(key);
    BOOST_CHECK_EQUAL(poll.KeyCount(), 0U);
    BOOST_CHECK_EQUAL(poll.DemandCount(), 0U);
    BOOST_CHECK(!poll.HasObservation(key));
    BOOST_CHECK_EQUAL(poll.AttemptsInWindow(now), 2U);
    BOOST_CHECK(!poll.TryChargeAttempt(now));
    BOOST_REQUIRE(poll.Demand(key));
    BOOST_CHECK(!poll.HasObservation(key));
    BOOST_CHECK(!poll.Take(now));
}

BOOST_AUTO_TEST_CASE(duplicate_demand_and_observation_storage_are_bounded)
{
    Poll poll;
    for (size_t i{0}; i < Poll::MAX_KEYS; ++i) {
        BOOST_REQUIRE(poll.Demand(Key(1, i + 1)));
        BOOST_REQUIRE(poll.Demand(Key(1, i + 1)));
        BOOST_REQUIRE(poll.MarkObserved(Key(1, i + 1)));
    }
    BOOST_CHECK_EQUAL(poll.KeyCount(), Poll::MAX_KEYS);
    BOOST_CHECK_EQUAL(poll.DemandCount(), Poll::MAX_KEYS);
    BOOST_CHECK(!poll.Demand(Key(2, 1)));
    BOOST_CHECK(!poll.MarkObserved(Key(2, 1)));
    poll.Remove(Key(1, 1));
    BOOST_CHECK(poll.Demand(Key(2, 1)));
    BOOST_CHECK(!poll.HasObservation(Key(2, 1)));
    BOOST_CHECK_EQUAL(poll.KeyCount(), Poll::MAX_KEYS);
    BOOST_CHECK_EQUAL(poll.DemandCount(), Poll::MAX_KEYS);
}

BOOST_AUTO_TEST_CASE(clock_rollback_does_not_refill_or_reset_automatic_allowance)
{
    Poll poll;
    const Poll::TimePoint start{10s};
    BOOST_REQUIRE(poll.TryChargeAttempt(start));
    BOOST_REQUIRE(poll.TryChargeAttempt(start));
    BOOST_CHECK(!poll.TryChargeAttempt(start - 10s));
    BOOST_CHECK(!poll.CanAttempt(start + Poll::REFILL_INTERVAL - 1us));
    BOOST_CHECK_EQUAL(poll.AttemptsInWindow(start - 10s), 2U);
    BOOST_CHECK(poll.TryChargeAttempt(start + Poll::REFILL_INTERVAL));
}

BOOST_AUTO_TEST_CASE(restart_loses_only_volatile_polling_state)
{
    const auto key{Key(1, 1)};
    const Poll::TimePoint now{};
    Poll before;
    BOOST_REQUIRE(before.Demand(key));
    BOOST_REQUIRE(before.MarkObserved(key));
    BOOST_REQUIRE(before.TryChargeAttempt(now));
    Poll after;
    BOOST_CHECK_EQUAL(after.KeyCount(), 0U);
    BOOST_CHECK_EQUAL(after.DemandCount(), 0U);
    BOOST_CHECK_EQUAL(after.AttemptsInWindow(now), 0U);
    BOOST_CHECK(!after.HasObservation(key));
    BOOST_REQUIRE(after.Demand(key));
    BOOST_REQUIRE(after.Take(now));
    BOOST_CHECK(!after.HasObservation(key));
}

BOOST_AUTO_TEST_SUITE_END()
