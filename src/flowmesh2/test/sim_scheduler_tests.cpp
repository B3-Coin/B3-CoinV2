// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/result.h>
#include <flowmesh2/sim/clock.h>
#include <flowmesh2/sim/faults.h>
#include <flowmesh2/sim/scheduler.h>
#include <flowmesh2/test/util/require.h>
#include <flowmesh2/test/util/slot_fixtures.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::sim;
using namespace flowmesh2::test;

namespace {

//! The labels of `count` events queued at time 0, in the order they ran.
std::vector<int> TieOrder(uint64_t seed, int count)
{
    Scheduler scheduler{seed};
    std::vector<int> order;
    for (int i{0}; i < count; ++i) {
        RequireOk(scheduler.At(0, [&order, i] { order.push_back(i); }), "At");
    }
    const Result<uint64_t> ran{scheduler.RunUntilIdle()};
    RequireOk(ran, "RunUntilIdle");
    BOOST_CHECK_EQUAL(*ran, static_cast<uint64_t>(count));
    return order;
}

//! A mixed schedule: events at several times, some queueing more events, and jitter draws.
std::vector<std::string> MixedRun(uint64_t seed)
{
    Scheduler scheduler{seed};
    std::vector<std::string> log;
    for (int i{0}; i < 12; ++i) {
        const uint64_t jitter{scheduler.Jitter(4).value()};
        RequireOk(scheduler.At(jitter, [&scheduler, &log, i] {
                      log.push_back("e" + std::to_string(i) + "@" + std::to_string(scheduler.NowMs()));
                      if (i % 3 == 0) {
                          RequireOk(scheduler.After(1, [&scheduler, &log, i] {
                                        log.push_back("f" + std::to_string(i) + "@" + std::to_string(scheduler.NowMs()));
                                    }),
                                    "After");
                      }
                  }),
                  "At");
    }
    RequireOk(scheduler.RunUntilIdle(), "RunUntilIdle");
    return log;
}

} // namespace

BOOST_AUTO_TEST_SUITE(sim_scheduler_tests)

BOOST_AUTO_TEST_CASE(seed_key_golden)
{
    // evidence R14/golden.py: SHA256("fm2/sim/seed/" || u64le(seed)), printed as uint256::GetHex.
    BOOST_CHECK_EQUAL(SeedKey(1).GetHex(), "210d45cdfe2c0ce5b9a543e254a496c9f9bb46857a0253cea14d005e4f75ca76");
    BOOST_CHECK_EQUAL(SeedKey(2).GetHex(), "6695be3da668901d4b88c0485e692f3d2187f115c0f358f5829149da26ea2d80");
    BOOST_CHECK(SeedKey(1) != SeedKey(2));
}

BOOST_AUTO_TEST_CASE(tie_orders_for_seeds_1_and_2_match_goldens)
{
    // evidence R14/golden.py: ties are the first eight rand64() of
    // ChaCha20(SeedKey(seed)); events run in ascending (tie, seq).
    BOOST_CHECK(TieOrder(1, 8) == (std::vector<int>{1, 4, 3, 0, 2, 6, 5, 7}));
    BOOST_CHECK(TieOrder(2, 8) == (std::vector<int>{1, 6, 5, 2, 0, 4, 3, 7}));
}

BOOST_AUTO_TEST_CASE(same_seed_same_order_other_seed_other_order)
{
    for (uint64_t seed{1}; seed <= 8; ++seed) {
        BOOST_CHECK(TieOrder(seed, 16) == TieOrder(seed, 16));
        BOOST_CHECK(MixedRun(seed) == MixedRun(seed));
    }
    // 16 tied events: a collision of two seeds' permutations has probability 1/16!.
    BOOST_CHECK(TieOrder(1, 16) != TieOrder(2, 16));
    size_t distinct{0};
    std::vector<std::vector<std::string>> seen;
    for (uint64_t seed{1}; seed <= 8; ++seed) {
        const std::vector<std::string> run{MixedRun(seed)};
        bool fresh{true};
        for (const auto& other : seen) fresh = fresh && other != run;
        if (fresh) ++distinct;
        seen.push_back(run);
    }
    BOOST_CHECK_GT(distinct, 1U);
}

BOOST_AUTO_TEST_CASE(time_order_clock_and_refusals)
{
    Scheduler scheduler{3};
    std::vector<std::string> log;
    RequireOk(scheduler.At(5, [&] { log.push_back("t5@" + std::to_string(scheduler.NowMs())); }), "At 5");
    RequireOk(scheduler.At(1, [&] {
                  log.push_back("t1@" + std::to_string(scheduler.NowMs()));
                  // A task may queue at the current time (it runs later in this
                  // drain) but never in the past.
                  RequireOk(scheduler.At(1, [&] { log.push_back("t1b@" + std::to_string(scheduler.NowMs())); }), "At now");
                  RequireError(scheduler.At(0, [] {}), ErrorCode::INVALID_INPUT, "At in the past");
                  BOOST_REQUIRE(scheduler.Current().has_value());
                  BOOST_CHECK_EQUAL(scheduler.Current()->time_ms, 1U);
              }),
              "At 1");
    RequireOk(scheduler.At(3, [&] { log.push_back("t3@" + std::to_string(scheduler.NowMs())); }), "At 3");
    BOOST_CHECK_EQUAL(scheduler.Pending(), 3U);
    BOOST_CHECK(!scheduler.Current().has_value());
    const Result<uint64_t> ran{scheduler.RunUntilIdle()};
    RequireOk(ran, "RunUntilIdle");
    BOOST_CHECK_EQUAL(*ran, 4U);
    BOOST_CHECK(log == (std::vector<std::string>{"t1@1", "t1b@1", "t3@3", "t5@5"}));
    BOOST_CHECK_EQUAL(scheduler.NowMs(), 5U);
    BOOST_CHECK_EQUAL(scheduler.Pending(), 0U);
    BOOST_CHECK(!scheduler.Current().has_value());

    // Refusals change nothing.
    const Result<EventKey> past{scheduler.At(4, [] {})};
    BOOST_REQUIRE(!past.has_value());
    BOOST_CHECK(past.error() == (Error{ErrorCode::INVALID_INPUT, SCHED_IN_PAST}));
    const Result<EventKey> overflow{scheduler.After(std::numeric_limits<uint64_t>::max(), [] {})};
    BOOST_REQUIRE(!overflow.has_value());
    BOOST_CHECK(overflow.error() == (Error{ErrorCode::INVALID_INPUT, SCHED_TIME_OVERFLOW}));
    const Result<EventKey> empty{scheduler.At(9, Scheduler::Task{})};
    BOOST_REQUIRE(!empty.has_value());
    BOOST_CHECK(empty.error() == (Error{ErrorCode::INVALID_INPUT, SCHED_EMPTY_TASK}));
    BOOST_CHECK_EQUAL(scheduler.Pending(), 0U);
    // seq counts queued events only: the next key's seq follows the four queued above.
    const Result<EventKey> next{scheduler.After(0, [] {})};
    RequireOk(next, "After 0");
    BOOST_CHECK_EQUAL(next->seq, 4U);
    BOOST_CHECK_EQUAL(next->time_ms, 5U);
}

BOOST_AUTO_TEST_CASE(run_until_idle_stops_at_its_limit)
{
    Scheduler scheduler{4};
    uint64_t runs{0};
    std::function<void()> again;
    again = [&] {
        ++runs;
        RequireOk(scheduler.After(1, again), "After");
    };
    RequireOk(scheduler.At(0, again), "At");
    const Result<uint64_t> ran{scheduler.RunUntilIdle(100)};
    BOOST_REQUIRE(!ran.has_value());
    BOOST_CHECK(ran.error() == (Error{ErrorCode::INVARIANT, SCHED_EVENT_LIMIT}));
    BOOST_CHECK_EQUAL(runs, 100U);
    BOOST_CHECK_EQUAL(scheduler.Pending(), 1U);
}

BOOST_AUTO_TEST_CASE(jitter_is_seeded_and_bounded)
{
    Scheduler a{7};
    Scheduler b{7};
    Scheduler c{8};
    std::vector<uint64_t> va, vb, vc;
    for (int i{0}; i < 64; ++i) {
        va.push_back(a.Jitter(10).value());
        vb.push_back(b.Jitter(10).value());
        vc.push_back(c.Jitter(10).value());
    }
    BOOST_CHECK(va == vb);
    BOOST_CHECK(va != vc);
    for (const uint64_t v : va) BOOST_CHECK_LT(v, 10U);
    RequireError(a.Jitter(0), ErrorCode::INVALID_INPUT, "Jitter(0)");
    // Ties and jitter share one stream: a jitter draw shifts every later tie.
    BOOST_CHECK_EQUAL(Scheduler{9}.Jitter(1).value(), 0U);
}

BOOST_AUTO_TEST_CASE(fake_clock_moves_forward_only)
{
    FakeClock clock;
    BOOST_CHECK_EQUAL(clock.NowMs(), 0U);
    RequireOk(clock.AdvanceTo(10), "AdvanceTo 10");
    RequireOk(clock.AdvanceTo(10), "AdvanceTo 10 again");
    const Result<void> back{clock.AdvanceTo(9)};
    BOOST_REQUIRE(!back.has_value());
    BOOST_CHECK(back.error() == (Error{ErrorCode::INVALID_INPUT, CLOCK_BACKWARDS}));
    BOOST_CHECK_EQUAL(clock.NowMs(), 10U);
}

BOOST_AUTO_TEST_CASE(fault_plan_real_faults_pass_and_stub_faults_are_typed)
{
    FaultPlan plan;
    RequireOk(CheckFaultPlan(plan), "empty plan");
    plan.withhold_from_proposals.insert({KeyAccount("alice"), 5});
    plan.fail_commit_barrier_at = 3;
    plan.bump_authority_at = 3;
    RequireOk(CheckFaultPlan(plan), "the three real faults");

    std::vector<FaultPlan> stubs(5, plan);
    stubs[0].crash_before_slot = 2;
    stubs[1].drop.push_back(DropRule{0, 1, 500});
    stubs[2].delay.push_back(DelayRule{0, 1, 20});
    stubs[3].partition.push_back({0, 1});
    stubs[4].skew.push_back(SkewRule{1, -5});
    for (const FaultPlan& stub : stubs) RequireStub(CheckFaultPlan(stub), "M4-SIM-02");
}

BOOST_AUTO_TEST_SUITE_END()
