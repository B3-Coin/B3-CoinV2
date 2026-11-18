// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** Service lanes (slice R13; net/lanes.h).
 *
 * Strict lane priority CRITICAL > PROPOSAL > ACTION > BULK and FIFO inside
 * a lane; the reserved 10 % share: CANCEL, STATUS and HEALTH are accepted
 * while the ordinary share of a lane is full (by messages and by bytes);
 * every refusal (BUSY, OVERSIZE, FENCED, INVALID_INPUT) leaves no queued
 * copy and the counters unchanged; Close; concurrent pushers and poppers
 * never let the ordinary share exceed its bound and lose nothing.
 */

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/net/lanes.h>
#include <flowmesh2/test/util/api_fixtures.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <uint256.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;
using net::Lane;
using net::LaneLimits;
using net::LaneSet;
using net::LaneSetLimits;
using net::LaneStats;
using net::TrafficClass;

namespace {

using Buffer = std::vector<unsigned char>;

//! A request id with `n` in its first 8 bytes.
uint256 IdOf(uint64_t n)
{
    uint256 id;
    for (size_t i{0}; i < 8; ++i) id.data()[i] = static_cast<unsigned char>(n >> (8 * i));
    return id;
}

uint64_t NumberOf(const uint256& id)
{
    uint64_t n{0};
    for (size_t i{0}; i < 8; ++i) n |= static_cast<uint64_t>(id.data()[i]) << (8 * i);
    return n;
}

Buffer PayloadOf(size_t size, unsigned char fill = 0xab) { return Buffer(size, fill); }

//! Every lane with `limits`.
LaneSetLimits Uniform(const LaneLimits& limits)
{
    LaneSetLimits all;
    all.lanes.fill(limits);
    return all;
}

void RequireRefused(const Result<void>& result, ErrorCode code, std::string_view detail, std::string_view what)
{
    if (result.has_value()) BOOST_FAIL(std::string{what} + ": queued, want a refusal");
    BOOST_CHECK_MESSAGE(result.error().code == code, std::string{what} + ": code " + std::string{ToString(result.error().code)});
    BOOST_CHECK_MESSAGE(result.error().detail == detail, std::string{what} + ": detail " + std::string{result.error().detail});
}

void RequireQueued(const Result<void>& result, std::string_view what)
{
    if (!result.has_value()) {
        BOOST_FAIL(std::string{what} + ": refused " + std::string{ToString(result.error().code)} + " " +
                   std::string{result.error().detail});
    }
}

//! Stats of `lane` agree with what its C1 mailbox holds (each message carries one class byte).
void RequireMailboxAgrees(const LaneSet& lanes, Lane lane)
{
    const LaneStats stats{lanes.Stats(lane)};
    const MailboxStats mailbox{lanes.MailboxStatsOf(lane)};
    BOOST_CHECK_EQUAL(mailbox.messages, stats.messages);
    BOOST_CHECK_EQUAL(mailbox.payload_bytes, stats.payload_bytes + stats.messages);
}

} // namespace

BOOST_AUTO_TEST_SUITE(lanes_tests)

BOOST_AUTO_TEST_CASE(registries_are_pinned)
{
    BOOST_CHECK_EQUAL(static_cast<int>(Lane::CRITICAL), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(Lane::PROPOSAL), 2);
    BOOST_CHECK_EQUAL(static_cast<int>(Lane::ACTION), 3);
    BOOST_CHECK_EQUAL(static_cast<int>(Lane::BULK), 4);
    BOOST_CHECK_EQUAL(static_cast<int>(TrafficClass::ORDINARY), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(TrafficClass::CANCEL), 2);
    BOOST_CHECK_EQUAL(static_cast<int>(TrafficClass::STATUS), 3);
    BOOST_CHECK_EQUAL(static_cast<int>(TrafficClass::HEALTH), 4);
    BOOST_CHECK_EQUAL(net::ToString(Lane::CRITICAL), "CRITICAL");
    BOOST_CHECK_EQUAL(net::ToString(Lane::BULK), "BULK");
    BOOST_CHECK_EQUAL(net::ToString(static_cast<Lane>(0)), "UNKNOWN");
    BOOST_CHECK_EQUAL(net::ToString(static_cast<Lane>(5)), "UNKNOWN");
    BOOST_CHECK_EQUAL(net::ToString(TrafficClass::HEALTH), "HEALTH");
    BOOST_CHECK_EQUAL(net::ToString(static_cast<TrafficClass>(5)), "UNKNOWN");
    BOOST_CHECK(!net::IsKnown(static_cast<Lane>(0)));
    BOOST_CHECK(!net::IsKnown(static_cast<TrafficClass>(0)));
    BOOST_CHECK(!net::IsReserved(TrafficClass::ORDINARY));
    BOOST_CHECK(net::IsReserved(TrafficClass::CANCEL));
    BOOST_CHECK(net::IsReserved(TrafficClass::STATUS));
    BOOST_CHECK(net::IsReserved(TrafficClass::HEALTH));
    BOOST_CHECK(!net::IsReserved(static_cast<TrafficClass>(5)));
    const std::array<Lane, 4> order{Lane::CRITICAL, Lane::PROPOSAL, Lane::ACTION, Lane::BULK};
    BOOST_CHECK(net::LANES_BY_PRIORITY == order);
}

BOOST_AUTO_TEST_CASE(reserved_share_rounds_up_without_overflow)
{
    BOOST_CHECK_EQUAL(net::ReservedShare(0), 0U);
    BOOST_CHECK_EQUAL(net::ReservedShare(1), 1U);
    BOOST_CHECK_EQUAL(net::ReservedShare(9), 1U);
    BOOST_CHECK_EQUAL(net::ReservedShare(10), 1U);
    BOOST_CHECK_EQUAL(net::ReservedShare(11), 2U);
    BOOST_CHECK_EQUAL(net::ReservedShare(20), 2U);
    BOOST_CHECK_EQUAL(net::ReservedShare(4'096), 410U);
    const size_t max{std::numeric_limits<size_t>::max()};
    BOOST_CHECK_EQUAL(net::ReservedShare(max), max / 10 + 1);
    for (size_t n{1}; n <= 1'000; ++n) {
        BOOST_CHECK(net::ReservedShare(n) * 10 >= n);
        BOOST_CHECK((net::ReservedShare(n) - 1) * 10 < n);
    }
}

BOOST_AUTO_TEST_CASE(create_refuses_limits_without_both_shares)
{
    // max_messages 1: the reserved share takes the only message.
    BOOST_CHECK(!net::LimitsValid(LaneLimits{1, 1'000, 10}));
    BOOST_CHECK(net::LimitsValid(LaneLimits{2, 1'000, 10}));
    // The reserved byte share must hold one message: ceil(99 / 10) = 10 < 11.
    BOOST_CHECK(!net::LimitsValid(LaneLimits{10, 99, 11}));
    BOOST_CHECK(net::LimitsValid(LaneLimits{10, 110, 11}));
    // The ordinary byte share must hold one message too.
    BOOST_CHECK(!net::LimitsValid(LaneLimits{10, 10, 5}));
    BOOST_CHECK(!net::LimitsValid(LaneLimits{10, 1'000, 0}));
    BOOST_CHECK(!net::LimitsValid(LaneLimits{0, 1'000, 10}));

    const LaneLimits good{10, 1'000, 10};
    LaneSetLimits limits{Uniform(good)};
    RequireOk(LaneSet::Create(limits), "valid limits");
    for (const Lane lane : net::LANES_BY_PRIORITY) {
        LaneSetLimits bad{limits};
        bad.Of(lane) = LaneLimits{1, 1'000, 10};
        const Result<std::unique_ptr<LaneSet>> refused{LaneSet::Create(bad)};
        BOOST_REQUIRE(!refused.has_value());
        BOOST_CHECK(refused.error().code == ErrorCode::INVALID_INPUT);
        BOOST_CHECK_EQUAL(refused.error().detail, net::LANE_LIMITS_INVALID);
    }

    // The provisional limits are valid, and the ACTION lane carries the largest action frame plus the request-kind byte.
    const ProfileParams profile{RegtestProfile()};
    const LaneSetLimits provisional{net::ProvisionalLaneLimits(profile)};
    for (const LaneLimits& lane : provisional.lanes) BOOST_CHECK(net::LimitsValid(lane));
    BOOST_CHECK_EQUAL(provisional.Of(Lane::ACTION).max_message_bytes, ActionV2::MaxEncodedSize(profile) + 1);
    BOOST_CHECK_EQUAL(provisional.Of(Lane::ACTION).max_messages, profile.pool_capacity);
    RequireOk(LaneSet::Create(provisional), "provisional limits");
}

BOOST_AUTO_TEST_CASE(lane_priority_order)
{
    auto lanes{NewLaneSet(Uniform(LaneLimits{20, 2'000, 10}))};
    // Fill the lanes lowest priority first, three messages each.
    uint64_t next{1};
    std::map<uint64_t, Lane> lane_of;
    const std::array<Lane, 4> fill_order{Lane::BULK, Lane::ACTION, Lane::PROPOSAL, Lane::CRITICAL};
    for (const Lane lane : fill_order) {
        for (int i{0}; i < 3; ++i) {
            lane_of[next] = lane;
            RequireQueued(lanes->TryPush(lane, TrafficClass::ORDINARY, IdOf(next), PayloadOf(1, static_cast<unsigned char>(next))), "fill");
            ++next;
        }
    }
    // PopNext serves CRITICAL, PROPOSAL, ACTION, BULK, each in FIFO order.
    std::vector<uint64_t> served;
    for (int i{0}; i < 4; ++i) {
        const std::optional<net::LaneMessage> message{lanes->PopNext()};
        BOOST_REQUIRE(message.has_value());
        served.push_back(NumberOf(message->request_id));
        BOOST_CHECK(message->lane == lane_of[served.back()]);
        BOOST_CHECK(message->payload == PayloadOf(1, static_cast<unsigned char>(served.back())));
    }
    BOOST_CHECK((served == std::vector<uint64_t>{10, 11, 12, 7}));
    // A CRITICAL message that arrives later is served before the rest of PROPOSAL.
    RequireQueued(lanes->TryPush(Lane::CRITICAL, TrafficClass::ORDINARY, IdOf(100), PayloadOf(1)), "late critical");
    std::vector<uint64_t> rest;
    while (const std::optional<net::LaneMessage> message{lanes->PopNext()}) rest.push_back(NumberOf(message->request_id));
    BOOST_CHECK((rest == std::vector<uint64_t>{100, 8, 9, 4, 5, 6, 1, 2, 3}));
    for (const Lane lane : net::LANES_BY_PRIORITY) {
        BOOST_CHECK(lanes->Stats(lane) == LaneStats{});
        RequireMailboxAgrees(*lanes, lane);
    }
    BOOST_CHECK(!lanes->PopNext().has_value());
    // TryPop serves one lane only, in FIFO order, and an unregistered lane is empty.
    RequireQueued(lanes->TryPush(Lane::BULK, TrafficClass::ORDINARY, IdOf(201), PayloadOf(1)), "bulk 1");
    RequireQueued(lanes->TryPush(Lane::CRITICAL, TrafficClass::ORDINARY, IdOf(202), PayloadOf(1)), "critical");
    RequireQueued(lanes->TryPush(Lane::BULK, TrafficClass::ORDINARY, IdOf(203), PayloadOf(1)), "bulk 2");
    BOOST_CHECK_EQUAL(NumberOf(lanes->TryPop(Lane::BULK)->request_id), 201U);
    BOOST_CHECK_EQUAL(NumberOf(lanes->TryPop(Lane::BULK)->request_id), 203U);
    BOOST_CHECK(!lanes->TryPop(Lane::BULK).has_value());
    BOOST_CHECK(!lanes->TryPop(static_cast<Lane>(0)).has_value());
    BOOST_CHECK_EQUAL(NumberOf(lanes->TryPop(Lane::CRITICAL)->request_id), 202U);
}

BOOST_AUTO_TEST_CASE(cancel_status_and_health_are_accepted_while_the_ordinary_share_is_full)
{
    // By messages: 20 messages, reserved share 2, ordinary share 18.
    auto lanes{NewLaneSet(Uniform(LaneLimits{20, 20'000, 100}))};
    for (uint64_t i{1}; i <= 18; ++i) {
        RequireQueued(lanes->TryPush(Lane::ACTION, TrafficClass::ORDINARY, IdOf(i), PayloadOf(10)), "ordinary " + std::to_string(i));
    }
    RequireRefused(lanes->TryPush(Lane::ACTION, TrafficClass::ORDINARY, IdOf(19), PayloadOf(10)), ErrorCode::BUSY, net::LANE_ORDINARY_FULL,
                   "19th ordinary");
    RequireQueued(lanes->TryPush(Lane::ACTION, TrafficClass::CANCEL, IdOf(20), PayloadOf(10)), "cancel");
    RequireQueued(lanes->TryPush(Lane::ACTION, TrafficClass::STATUS, IdOf(21), PayloadOf(10)), "status");
    BOOST_CHECK((lanes->Stats(Lane::ACTION) == LaneStats{20, 200, 18, 180, false}));
    // Now the lane itself is full: even the reserved classes are BUSY.
    RequireRefused(lanes->TryPush(Lane::ACTION, TrafficClass::HEALTH, IdOf(22), PayloadOf(10)), ErrorCode::BUSY, net::LANE_FULL,
                   "health in a full lane");
    // The other lanes are independent.
    RequireQueued(lanes->TryPush(Lane::BULK, TrafficClass::ORDINARY, IdOf(23), PayloadOf(10)), "bulk");
    // Popping an ordinary message frees one ordinary place; popping a reserved one frees no ordinary place.
    BOOST_CHECK_EQUAL(NumberOf(lanes->TryPop(Lane::ACTION)->request_id), 1U);
    RequireQueued(lanes->TryPush(Lane::ACTION, TrafficClass::ORDINARY, IdOf(24), PayloadOf(10)), "ordinary after a pop");
    RequireRefused(lanes->TryPush(Lane::ACTION, TrafficClass::ORDINARY, IdOf(25), PayloadOf(10)), ErrorCode::BUSY, net::LANE_FULL,
                   "lane full again");
    std::vector<uint64_t> drained;
    std::vector<TrafficClass> classes;
    while (const std::optional<net::LaneMessage> message{lanes->TryPop(Lane::ACTION)}) {
        drained.push_back(NumberOf(message->request_id));
        classes.push_back(message->traffic);
    }
    BOOST_REQUIRE_EQUAL(drained.size(), 20U);
    BOOST_CHECK_EQUAL(drained[16], 18U);
    BOOST_CHECK_EQUAL(drained[17], 20U);
    BOOST_CHECK(classes[17] == TrafficClass::CANCEL);
    BOOST_CHECK_EQUAL(drained[18], 21U);
    BOOST_CHECK(classes[18] == TrafficClass::STATUS);
    BOOST_CHECK_EQUAL(drained[19], 24U);
    BOOST_CHECK(lanes->Stats(Lane::ACTION) == LaneStats{});

    // By bytes: 1,000 bytes, reserved share 100, messages of at most 100 bytes.
    auto by_bytes{NewLaneSet(Uniform(LaneLimits{100, 1'000, 100}))};
    for (uint64_t i{1}; i <= 9; ++i) {
        RequireQueued(by_bytes->TryPush(Lane::ACTION, TrafficClass::ORDINARY, IdOf(i), PayloadOf(100)), "ordinary bytes");
    }
    RequireRefused(by_bytes->TryPush(Lane::ACTION, TrafficClass::ORDINARY, IdOf(10), PayloadOf(1)), ErrorCode::BUSY,
                   net::LANE_ORDINARY_FULL, "one more ordinary byte");
    RequireQueued(by_bytes->TryPush(Lane::ACTION, TrafficClass::CANCEL, IdOf(11), PayloadOf(100)), "a full-size cancel");
    RequireRefused(by_bytes->TryPush(Lane::ACTION, TrafficClass::STATUS, IdOf(12), PayloadOf(1)), ErrorCode::BUSY, net::LANE_FULL,
                   "no byte left");
    BOOST_CHECK((by_bytes->Stats(Lane::ACTION) == LaneStats{10, 1'000, 9, 900, false}));
    RequireMailboxAgrees(*by_bytes, Lane::ACTION);

    // Reserved traffic may use the whole lane when no ordinary traffic is queued.
    auto reserved_only{NewLaneSet(Uniform(LaneLimits{20, 20'000, 100}))};
    for (uint64_t i{1}; i <= 20; ++i) {
        RequireQueued(reserved_only->TryPush(Lane::ACTION, i % 2 == 0 ? TrafficClass::STATUS : TrafficClass::CANCEL, IdOf(i), PayloadOf(1)),
                      "reserved " + std::to_string(i));
    }
    RequireRefused(reserved_only->TryPush(Lane::ACTION, TrafficClass::ORDINARY, IdOf(21), PayloadOf(1)), ErrorCode::BUSY, net::LANE_FULL,
                   "ordinary in a full lane");
}

BOOST_AUTO_TEST_CASE(busy_leaves_no_queued_copy)
{
    auto lanes{NewLaneSet(Uniform(LaneLimits{10, 1'000, 100}))};
    std::set<uint64_t> accepted;
    std::set<uint64_t> refused;
    uint64_t next{1};
    // Push until every kind of refusal has happened at least once.
    for (int round{0}; round < 30; ++round) {
        const TrafficClass traffic{round % 3 == 0 ? TrafficClass::CANCEL : TrafficClass::ORDINARY};
        const LaneStats before{lanes->Stats(Lane::ACTION)};
        const Result<void> pushed{lanes->TryPush(Lane::ACTION, traffic, IdOf(next), PayloadOf(10))};
        if (pushed.has_value()) {
            accepted.insert(next);
        } else {
            BOOST_CHECK(pushed.error().code == ErrorCode::BUSY);
            refused.insert(next);
            BOOST_CHECK(lanes->Stats(Lane::ACTION) == before);
        }
        ++next;
    }
    // Other refusals change nothing either.
    const LaneStats before{lanes->Stats(Lane::ACTION)};
    RequireRefused(lanes->TryPush(Lane::ACTION, TrafficClass::CANCEL, IdOf(next), PayloadOf(101)), ErrorCode::OVERSIZE,
                   net::LANE_MESSAGE_OVERSIZE, "oversize");
    refused.insert(next++);
    RequireRefused(lanes->TryPush(static_cast<Lane>(9), TrafficClass::CANCEL, IdOf(next), PayloadOf(1)), ErrorCode::INVALID_INPUT,
                   net::LANE_UNKNOWN, "unregistered lane");
    refused.insert(next++);
    RequireRefused(lanes->TryPush(Lane::ACTION, static_cast<TrafficClass>(0), IdOf(next), PayloadOf(1)), ErrorCode::INVALID_INPUT,
                   net::LANE_CLASS_UNKNOWN, "unregistered class");
    refused.insert(next++);
    BOOST_CHECK(lanes->Stats(Lane::ACTION) == before);
    RequireMailboxAgrees(*lanes, Lane::ACTION);
    BOOST_CHECK_EQUAL(accepted.size(), 10U);
    BOOST_CHECK(!refused.empty());
    // Drain: exactly the accepted ids, each once, and no refused one.
    std::multiset<uint64_t> drained;
    for (const Lane lane : net::LANES_BY_PRIORITY) {
        while (const std::optional<net::LaneMessage> message{lanes->TryPop(lane)}) drained.insert(NumberOf(message->request_id));
    }
    BOOST_CHECK((std::set<uint64_t>{drained.begin(), drained.end()} == accepted));
    BOOST_CHECK_EQUAL(drained.size(), accepted.size());
    for (const uint64_t id : refused) BOOST_CHECK_EQUAL(drained.count(id), 0U);
    RequireMailboxAgrees(*lanes, Lane::ACTION);
}

BOOST_AUTO_TEST_CASE(refusal_precedence)
{
    auto lanes{NewLaneSet(Uniform(LaneLimits{2, 1'000, 100}))};
    RequireQueued(lanes->TryPush(Lane::ACTION, TrafficClass::ORDINARY, IdOf(1), PayloadOf(1)), "ordinary");
    RequireQueued(lanes->TryPush(Lane::ACTION, TrafficClass::CANCEL, IdOf(2), PayloadOf(1)), "cancel");
    // Full, oversize and closed together: unregistered values first, then closed, then oversize, then full.
    RequireRefused(lanes->TryPush(Lane::ACTION, TrafficClass::ORDINARY, IdOf(3), PayloadOf(1)), ErrorCode::BUSY, net::LANE_FULL, "full");
    RequireRefused(lanes->TryPush(Lane::ACTION, TrafficClass::ORDINARY, IdOf(4), PayloadOf(101)), ErrorCode::OVERSIZE,
                   net::LANE_MESSAGE_OVERSIZE, "oversize before full");
    lanes->Close();
    RequireRefused(lanes->TryPush(Lane::ACTION, TrafficClass::ORDINARY, IdOf(5), PayloadOf(101)), ErrorCode::FENCED, net::LANE_CLOSED,
                   "closed before oversize");
    RequireRefused(lanes->TryPush(static_cast<Lane>(0), TrafficClass::ORDINARY, IdOf(6), PayloadOf(1)), ErrorCode::INVALID_INPUT,
                   net::LANE_UNKNOWN, "unregistered lane before closed");
}

BOOST_AUTO_TEST_CASE(close_refuses_new_messages_and_keeps_queued_ones)
{
    auto lanes{NewLaneSet(Uniform(LaneLimits{10, 1'000, 100}))};
    RequireQueued(lanes->TryPush(Lane::PROPOSAL, TrafficClass::ORDINARY, IdOf(1), PayloadOf(3)), "before close");
    Buffer maximum(100);
    for (size_t i{0}; i < maximum.size(); ++i) maximum[i] = static_cast<unsigned char>(i);
    RequireQueued(lanes->TryPush(Lane::PROPOSAL, TrafficClass::STATUS, IdOf(2), maximum), "maximum payload");
    RequireQueued(lanes->TryPush(Lane::PROPOSAL, TrafficClass::CANCEL, IdOf(3), {}), "empty payload");
    RequireMailboxAgrees(*lanes, Lane::PROPOSAL);
    lanes->Close();
    lanes->Close(); // idempotent
    BOOST_CHECK(lanes->Closed());
    for (const Lane lane : net::LANES_BY_PRIORITY) {
        RequireRefused(lanes->TryPush(lane, TrafficClass::HEALTH, IdOf(2), PayloadOf(1)), ErrorCode::FENCED, net::LANE_CLOSED, "after close");
        BOOST_CHECK(lanes->Stats(lane).closed);
        BOOST_CHECK(lanes->MailboxStatsOf(lane).closed);
    }
    const std::optional<net::LaneMessage> kept{lanes->PopNext()};
    BOOST_REQUIRE(kept.has_value());
    BOOST_CHECK_EQUAL(NumberOf(kept->request_id), 1U);
    BOOST_CHECK(kept->payload == PayloadOf(3));
    const std::optional<net::LaneMessage> full_size{lanes->PopNext()};
    BOOST_REQUIRE(full_size.has_value());
    BOOST_CHECK_EQUAL(NumberOf(full_size->request_id), 2U);
    BOOST_CHECK(full_size->traffic == TrafficClass::STATUS);
    BOOST_CHECK(full_size->payload == maximum);
    const std::optional<net::LaneMessage> empty{lanes->PopNext()};
    BOOST_REQUIRE(empty.has_value());
    BOOST_CHECK_EQUAL(NumberOf(empty->request_id), 3U);
    BOOST_CHECK(empty->traffic == TrafficClass::CANCEL);
    BOOST_CHECK(empty->payload.empty());
    BOOST_CHECK(!lanes->PopNext().has_value());
    BOOST_CHECK((lanes->Stats(Lane::PROPOSAL) == LaneStats{0, 0, 0, 0, true}));
    RequireMailboxAgrees(*lanes, Lane::PROPOSAL);
}

BOOST_AUTO_TEST_CASE(concurrent_pushers_and_poppers_keep_the_shares)
{
    const LaneLimits limits{20, 20'000, 64};
    auto lanes{NewLaneSet(Uniform(limits))};
    const size_t ordinary_share{limits.max_messages - net::ReservedShare(limits.max_messages)};
    constexpr int PUSHERS{4};
    constexpr uint64_t PER_PUSHER{2'000};
    std::atomic<bool> done{false};
    std::atomic<bool> share_exceeded{false};
    std::atomic<bool> total_exceeded{false};
    std::vector<std::vector<uint64_t>> accepted(PUSHERS);
    std::vector<uint64_t> popped;
    std::vector<std::thread> threads;
    for (int t{0}; t < PUSHERS; ++t) {
        threads.emplace_back([&, t] {
            for (uint64_t i{0}; i < PER_PUSHER; ++i) {
                const uint64_t id{static_cast<uint64_t>(t) * PER_PUSHER + i + 1};
                const TrafficClass traffic{i % 4 == 0 ? TrafficClass::CANCEL : TrafficClass::ORDINARY};
                const Buffer payload(1 + id % 64, static_cast<unsigned char>(id));
                if (lanes->TryPush(i % 2 == 0 ? Lane::ACTION : Lane::CRITICAL, traffic, IdOf(id), payload).has_value()) {
                    accepted[t].push_back(id);
                }
            }
        });
    }
    threads.emplace_back([&] {
        while (true) {
            const bool finished{done.load()};
            for (const Lane lane : {Lane::ACTION, Lane::CRITICAL}) {
                const LaneStats stats{lanes->Stats(lane)};
                if (stats.ordinary_messages > ordinary_share) share_exceeded = true;
                if (stats.messages > limits.max_messages || stats.payload_bytes > limits.max_payload_bytes) total_exceeded = true;
            }
            const std::optional<net::LaneMessage> message{lanes->PopNext()};
            if (message) {
                popped.push_back(NumberOf(message->request_id));
                if (message->payload != Buffer(1 + popped.back() % 64, static_cast<unsigned char>(popped.back()))) total_exceeded = true;
            } else if (finished) {
                break;
            } else {
                std::this_thread::yield();
            }
        }
    });
    for (int t{0}; t < PUSHERS; ++t) threads[t].join();
    done = true;
    threads.back().join();
    BOOST_CHECK(!share_exceeded.load());
    BOOST_CHECK(!total_exceeded.load());
    std::multiset<uint64_t> want;
    for (const auto& ids : accepted) want.insert(ids.begin(), ids.end());
    const std::multiset<uint64_t> got{popped.begin(), popped.end()};
    BOOST_CHECK(got == want);
    BOOST_CHECK_GT(want.size(), 0U);
    for (const Lane lane : net::LANES_BY_PRIORITY) {
        BOOST_CHECK(lanes->Stats(lane) == LaneStats{});
        RequireMailboxAgrees(*lanes, lane);
    }
}

BOOST_AUTO_TEST_SUITE_END()
