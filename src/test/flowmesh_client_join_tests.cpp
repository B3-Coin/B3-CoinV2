// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <node/flowmesh_client_join.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Own = node::FlowMeshJoinOwnAction;
using Kind = node::FlowMeshJoinOwnAction::Kind;
using Windows = node::FlowMeshJoinWindows;
using namespace std::chrono_literals;

// A refresh started at T0 and validated 20 ms later; the cached entry is 10
// and the cached state's next sequence for the signing account is 7.
const Clock::time_point T0{Clock::time_point{} + 1h};
const Clock::time_point VALIDATED{T0 + 20ms};
constexpr uint64_t ENTRY{10};
constexpr uint64_t NEXT{7};

bool Join(Clock::time_point now, std::vector<Own> own = {}, std::optional<uint64_t> through = std::nullopt,
          Clock::time_point started = T0, Clock::time_point validated = VALIDATED, const Windows& windows = {},
          uint64_t next = NEXT)
{
    return node::FlowMeshCanJoinFreshPreflight(now, started, validated, ENTRY, next, through, own, windows);
}

uint256 Market(unsigned char value)
{
    uint256 out;
    out.begin()[0] = value;
    return out;
}
} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_client_join_tests)

BOOST_AUTO_TEST_CASE(join_requires_completion_within_window)
{
    const Windows defaults;
    BOOST_CHECK(defaults.completion == 50ms);
    BOOST_CHECK(defaults.max_observation_age == 1000ms);
    BOOST_CHECK(Join(VALIDATED));
    BOOST_CHECK(Join(VALIDATED + 50ms));
    BOOST_CHECK(!Join(VALIDATED + 51ms));
    // Injected windows move the edge exactly.
    const Windows narrow{5ms, 1000ms};
    BOOST_CHECK(Join(VALIDATED + 5ms, {}, std::nullopt, T0, VALIDATED, narrow));
    BOOST_CHECK(!Join(VALIDATED + 6ms, {}, std::nullopt, T0, VALIDATED, narrow));
}

BOOST_AUTO_TEST_CASE(join_rejects_observation_older_than_cap)
{
    // A slow reused call: its request started long before it completed.
    const auto now{T0 + 1000ms};
    BOOST_CHECK(Join(now, {}, std::nullopt, T0, now - 10ms));
    BOOST_CHECK(!Join(now + 1ms, {}, std::nullopt, T0, now - 9ms));
    const Windows short_age{50ms, 100ms};
    BOOST_CHECK(Join(T0 + 100ms, {}, std::nullopt, T0, T0 + 90ms, short_age));
    BOOST_CHECK(!Join(T0 + 101ms, {}, std::nullopt, T0, T0 + 90ms, short_age));
}

BOOST_AUTO_TEST_CASE(join_rejects_clock_inversion)
{
    BOOST_CHECK(!Join(VALIDATED, {}, std::nullopt, VALIDATED + 1ms, VALIDATED));
    BOOST_CHECK(!Join(VALIDATED - 1ms));
}

BOOST_AUTO_TEST_CASE(join_rejects_unresolved_own_action)
{
    BOOST_CHECK(!Join(VALIDATED, {Own{Kind::UNRESOLVED, std::nullopt}}));
    // A microblock hint does not make an unresolved action joinable.
    BOOST_CHECK(!Join(VALIDATED, {Own{Kind::UNRESOLVED, ENTRY - 1}}));
    BOOST_CHECK(!Join(VALIDATED, {Own{Kind::DEFINITE_REJECTED, std::nullopt}, Own{Kind::UNRESOLVED, std::nullopt}}));
}

BOOST_AUTO_TEST_CASE(join_rejects_certified_own_action_not_reflected)
{
    BOOST_CHECK(!Join(VALIDATED, {Own{Kind::CERTIFIED, ENTRY + 1}}));
    BOOST_CHECK(Join(VALIDATED, {Own{Kind::CERTIFIED, ENTRY}}));
    BOOST_CHECK(Join(VALIDATED, {Own{Kind::CERTIFIED, ENTRY - 1}}));
}

BOOST_AUTO_TEST_CASE(join_rejects_previously_certified_not_yet_consumed)
{
    // Certified before a restart: where it was included is not known here.
    // A sequence the cached state has not consumed may take effect later.
    BOOST_CHECK(!Join(VALIDATED, {Own{Kind::CERTIFIED, std::nullopt, NEXT}}));
    BOOST_CHECK(!Join(VALIDATED, {Own{Kind::CERTIFIED, std::nullopt, NEXT + 1}}));
    // An account the cached state has never seen has consumed nothing.
    BOOST_CHECK(!Join(VALIDATED, {Own{Kind::CERTIFIED, std::nullopt, 0}}, std::nullopt, T0, VALIDATED, {}, 0));
}

BOOST_AUTO_TEST_CASE(join_accepts_previously_certified_already_consumed)
{
    // Its sequence is below the cached next sequence: it took effect at or
    // before the cached entry, or is refused as stale wherever it landed.
    BOOST_CHECK(Join(VALIDATED, {Own{Kind::CERTIFIED, std::nullopt, NEXT - 1}}));
    BOOST_CHECK(Join(VALIDATED, {Own{Kind::CERTIFIED, std::nullopt, 0}}));
    BOOST_CHECK(Join(VALIDATED, {Own{Kind::CERTIFIED, std::nullopt, 0}, Own{Kind::CERTIFIED, std::nullopt, NEXT - 1},
                                 Own{Kind::CERTIFIED, ENTRY, NEXT - 1}, Own{Kind::DEFINITE_REJECTED, std::nullopt, NEXT}}));
    // Edge: one below versus equal to the next sequence.
    BOOST_CHECK(Join(VALIDATED, {Own{Kind::CERTIFIED, std::nullopt, 41}}, std::nullopt, T0, VALIDATED, {}, 42));
    BOOST_CHECK(!Join(VALIDATED, {Own{Kind::CERTIFIED, std::nullopt, 42}}, std::nullopt, T0, VALIDATED, {}, 42));
    // One not-yet-consumed row still refuses the whole join.
    BOOST_CHECK(!Join(VALIDATED, {Own{Kind::CERTIFIED, std::nullopt, 0}, Own{Kind::CERTIFIED, std::nullopt, NEXT}}));
}

BOOST_AUTO_TEST_CASE(join_sequence_rule_applies_only_to_unknown_microblock)
{
    // A known microblock after the cached entry refuses even with a consumed
    // sequence, and a known one at or before it joins even when not consumed.
    BOOST_CHECK(!Join(VALIDATED, {Own{Kind::CERTIFIED, ENTRY + 1, 0}}));
    BOOST_CHECK(Join(VALIDATED, {Own{Kind::CERTIFIED, ENTRY, NEXT + 5}}));
    // Unresolved actions refuse whatever their sequence.
    BOOST_CHECK(!Join(VALIDATED, {Own{Kind::UNRESOLVED, std::nullopt, 0}}));
    BOOST_CHECK(!Join(VALIDATED, {Own{Kind::CERTIFIED, std::nullopt, 0}, Own{Kind::UNRESOLVED, std::nullopt, 0}}));
    // The windows and the own high-water still apply to a consumed row.
    BOOST_CHECK(!Join(VALIDATED + 51ms, {Own{Kind::CERTIFIED, std::nullopt, 0}}));
    BOOST_CHECK(!Join(VALIDATED, {Own{Kind::CERTIFIED, std::nullopt, 0}}, ENTRY + 1));
}

BOOST_AUTO_TEST_CASE(join_accepts_definite_rejected_and_reflected_certified)
{
    BOOST_CHECK(Join(VALIDATED, {Own{Kind::DEFINITE_REJECTED, std::nullopt}, Own{Kind::CERTIFIED, ENTRY}}, ENTRY));
}

BOOST_AUTO_TEST_CASE(join_refuses_after_certified_own_action_was_evicted)
{
    // Own action certified at microblock 12, then evicted from the retained
    // map by a later Submit: it is no longer in the own-action list.
    node::FlowMeshOwnCertifiedThrough through{8};
    through.Record(Market(1), 12);
    through.Record(Market(1), 11); // never lowers the mark
    BOOST_REQUIRE(through.Complete());
    BOOST_REQUIRE(through.Through(Market(1)));
    BOOST_CHECK_EQUAL(*through.Through(Market(1)), 12U);
    BOOST_CHECK(!through.Through(Market(2)));
    // A refresh stamped before that inclusion (entry 10) is not joinable.
    BOOST_CHECK(!Join(VALIDATED, {}, through.Through(Market(1))));
    BOOST_CHECK(node::FlowMeshCanJoinFreshPreflight(VALIDATED, T0, VALIDATED, 12, NEXT, through.Through(Market(1)), {}));
    // Another market is unaffected.
    BOOST_CHECK(Join(VALIDATED, {}, through.Through(Market(2))));
}

BOOST_AUTO_TEST_CASE(own_certified_record_overflow_is_incomplete)
{
    node::FlowMeshOwnCertifiedThrough through{2};
    through.Record(Market(1), 1);
    through.Record(Market(2), 1);
    BOOST_CHECK(through.Complete());
    through.Record(Market(2), 5);
    BOOST_CHECK(through.Complete());
    through.Record(Market(3), 1);
    BOOST_CHECK(!through.Complete());
    BOOST_CHECK(!through.Through(Market(3)));
    BOOST_CHECK_EQUAL(*through.Through(Market(2)), 5U);
}

BOOST_AUTO_TEST_CASE(signing_preflight_scope_is_nested_and_thread_local)
{
    using Scope = node::FlowMeshSigningPreflightScope;
    BOOST_CHECK(!Scope::Active());
    {
        Scope outer;
        BOOST_CHECK(Scope::Active());
        {
            Scope inner;
            BOOST_CHECK(Scope::Active());
        }
        BOOST_CHECK(Scope::Active());
        bool worker_active{true};
        std::thread worker{[&] { worker_active = Scope::Active(); }};
        worker.join();
        BOOST_CHECK(!worker_active);
    }
    BOOST_CHECK(!Scope::Active());
}

BOOST_AUTO_TEST_SUITE_END()
