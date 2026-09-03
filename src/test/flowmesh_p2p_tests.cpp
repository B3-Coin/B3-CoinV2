// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/p2p.h>
#include <protocol.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

namespace {

flowmesh::WireMessage Message(const flowmesh::WireMessageKind kind,
                              const size_t payload_size = 1)
{
    flowmesh::WireMessage message;
    message.kind = kind;
    message.header.market_id = uint256::ONE;
    message.header.epoch = 7;
    message.header.sequence = 9;
    message.payload.assign(payload_size, 0x42);
    if (kind == flowmesh::WireMessageKind::ATTESTATION) {
        message.payload.assign(flowmesh::FLOWMESH_ATTESTATION_BYTES, 0x42);
    } else if (kind == flowmesh::WireMessageKind::GET) {
        message.payload = *flowmesh::EncodeCatchupRequest(64, 4 * 1024 * 1024);
    } else if (kind == flowmesh::WireMessageKind::ENTRIES) {
        std::vector<std::vector<unsigned char>> entries{{0x42}};
        message.payload = *flowmesh::EncodeCatchupEntries(entries);
    }
    return message;
}

} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_p2p_tests)

BOOST_AUTO_TEST_CASE(commands_header_and_strict_shapes_round_trip)
{
    using namespace flowmesh;
    const std::vector kinds{
        WireMessageKind::HELLO, WireMessageKind::ACTION,
        WireMessageKind::PROPOSAL, WireMessageKind::ATTESTATION,
        WireMessageKind::CERTIFICATE, WireMessageKind::GET,
        WireMessageKind::ENTRIES};
    for (const auto kind : kinds) {
        BOOST_REQUIRE(WireKindForCommand(WireCommand(kind)).has_value());
        BOOST_CHECK(*WireKindForCommand(WireCommand(kind)) == kind);
        WireCheck check;
        const auto encoded{EncodeWireMessage(Message(kind), check)};
        BOOST_REQUIRE(encoded.has_value());
        BOOST_CHECK(check == WireCheck::OK);
        BOOST_CHECK_EQUAL(encoded->size(), FLOWMESH_WIRE_HEADER_SIZE +
                                             Message(kind).payload.size());
        const auto decoded{DecodeWireMessage(kind, *encoded, check)};
        BOOST_REQUIRE(decoded.has_value());
        BOOST_CHECK(*decoded == Message(kind));
    }

    BOOST_CHECK(!WireKindForCommand("fmunknown"));
    BOOST_CHECK_EQUAL(NetMessageQueueRank(NetMsgType::BLOCK), 0U);
    BOOST_CHECK_EQUAL(NetMessageQueueRank(NetMsgType::FMCERT), 1U);
    BOOST_CHECK_EQUAL(NetMessageQueueRank(NetMsgType::FMATTEST), 1U);
    BOOST_CHECK_EQUAL(NetMessageQueueRank(NetMsgType::FMPROP), 2U);
    BOOST_CHECK_EQUAL(NetMessageQueueRank(NetMsgType::FMACTION), 3U);
    BOOST_CHECK_EQUAL(NetMessageQueueRank(NetMsgType::FMENTRIES), 4U);
    WireCheck check;
    auto bad{Message(WireMessageKind::ACTION)};
    bad.header.version++;
    BOOST_CHECK(!EncodeWireMessage(bad, check));
    BOOST_CHECK(check == WireCheck::BAD_VERSION);
    bad = Message(WireMessageKind::ACTION);
    bad.header.market_id.SetNull();
    BOOST_CHECK(!EncodeWireMessage(bad, check));
    BOOST_CHECK(check == WireCheck::NULL_MARKET);

    bad = Message(WireMessageKind::ATTESTATION);
    bad.payload.pop_back();
    BOOST_CHECK(!EncodeWireMessage(bad, check));
    BOOST_CHECK(check == WireCheck::WRONG_LENGTH);
    bad = Message(WireMessageKind::ACTION,
                  FLOWMESH_ACTION_MAX_BYTES + 1);
    BOOST_CHECK(!EncodeWireMessage(bad, check));
    BOOST_CHECK(check == WireCheck::TOO_LARGE);
}

BOOST_AUTO_TEST_CASE(catchup_is_count_and_byte_bounded_before_allocation)
{
    using namespace flowmesh;
    BOOST_CHECK(!EncodeCatchupRequest(0, 1));
    BOOST_CHECK(!EncodeCatchupRequest(65, 1));
    BOOST_CHECK(!EncodeCatchupRequest(1, FLOWMESH_CATCHUP_MAX_BYTES + 1));

    const auto request{EncodeCatchupRequest(64, FLOWMESH_CATCHUP_MAX_BYTES)};
    BOOST_REQUIRE(request);
    uint16_t count{0};
    uint32_t bytes{0};
    BOOST_CHECK(DecodeCatchupRequest(*request, count, bytes));
    BOOST_CHECK_EQUAL(count, 64);
    BOOST_CHECK_EQUAL(bytes, FLOWMESH_CATCHUP_MAX_BYTES);

    std::vector<std::vector<unsigned char>> entries{{1, 2, 3}, {4, 5}};
    const auto encoded{EncodeCatchupEntries(entries)};
    BOOST_REQUIRE(encoded);
    BOOST_REQUIRE_EQUAL(encoded->size(), 2U + 4U + 3U + 4U + 2U);
    // One exact big-endian count followed immediately by the first u32
    // entry length. This guards the frozen wire framing byte-for-byte.
    BOOST_CHECK_EQUAL((*encoded)[0], 0);
    BOOST_CHECK_EQUAL((*encoded)[1], 2);
    BOOST_CHECK_EQUAL((*encoded)[2], 0);
    BOOST_CHECK_EQUAL((*encoded)[3], 0);
    BOOST_CHECK_EQUAL((*encoded)[4], 0);
    BOOST_CHECK_EQUAL((*encoded)[5], 3);
    const auto decoded{DecodeCatchupEntries(*encoded)};
    BOOST_REQUIRE(decoded);
    BOOST_CHECK(*decoded == entries);

    auto trailing{*encoded};
    trailing.push_back(0);
    BOOST_CHECK(!DecodeCatchupEntries(trailing));
    auto bad_count{*encoded};
    bad_count[0] = 0;
    bad_count[1] = 65;
    BOOST_CHECK(!DecodeCatchupEntries(bad_count));
    std::vector<std::vector<unsigned char>> too_many(65, std::vector<unsigned char>{1});
    BOOST_CHECK(!EncodeCatchupEntries(too_many));
}

BOOST_AUTO_TEST_CASE(priority_lanes_protect_certificates_and_enforce_rate_limits)
{
    using namespace flowmesh;
    BoundedWireQueue queue;
    const auto now{WireClock::time_point{std::chrono::seconds{100}}};

    BOOST_CHECK(queue.Push(1, Message(WireMessageKind::ACTION), now) ==
                QueueResult::ACCEPTED);
    BOOST_CHECK(queue.Push(1, Message(WireMessageKind::PROPOSAL), now) ==
                QueueResult::ACCEPTED);
    BOOST_CHECK(queue.Push(1, Message(WireMessageKind::ATTESTATION), now) ==
                QueueResult::ACCEPTED);
    BOOST_CHECK(queue.Push(1, Message(WireMessageKind::ENTRIES), now) ==
                QueueResult::ACCEPTED);

    BOOST_REQUIRE(queue.Pop());
    BOOST_CHECK(queue.Pop()->message.kind == WireMessageKind::PROPOSAL);
    BOOST_CHECK(queue.Pop()->message.kind == WireMessageKind::ACTION);
    BOOST_CHECK(queue.Pop()->message.kind == WireMessageKind::ENTRIES);
    BOOST_CHECK(queue.Empty());

    BoundedWireQueue actions;
    for (size_t i{0}; i < 256; ++i) {
        BOOST_REQUIRE(actions.Push(2, Message(WireMessageKind::ACTION), now) ==
                      QueueResult::ACCEPTED);
    }
    BOOST_CHECK(actions.Push(2, Message(WireMessageKind::ACTION), now) ==
                QueueResult::RATE_LIMITED);
    BOOST_REQUIRE(actions.Pop());
    BOOST_CHECK(actions.Push(2, Message(WireMessageKind::ACTION),
                             now + std::chrono::seconds{1}) ==
                QueueResult::ACCEPTED);
}

BOOST_AUTO_TEST_CASE(lower_priority_work_is_evicted_but_b3_has_no_dependency)
{
    using namespace flowmesh;
    BoundedWireQueue queue;
    const auto now{WireClock::time_point{std::chrono::seconds{200}}};
    // Sixty-three maximum hello frames leave less than one proposal's room in
    // this peer's isolated FlowMesh budget.
    for (size_t i{0}; i < 63; ++i) {
        BOOST_REQUIRE(queue.Push(5, Message(WireMessageKind::HELLO,
                                            FLOWMESH_HELLO_MAX_BYTES), now) ==
                      QueueResult::ACCEPTED);
    }
    const size_t before{queue.Size()};
    BOOST_REQUIRE(queue.Push(5, Message(WireMessageKind::PROPOSAL, 128 * 1024), now) ==
                  QueueResult::ACCEPTED);
    BOOST_CHECK(queue.Size() < before + 1); // one or more hello frames evicted
    BOOST_CHECK(queue.PeerBytes(5) <= FLOWMESH_QUEUE_PER_PEER_BYTES);
    BOOST_REQUIRE(queue.Pop());
    BOOST_CHECK(queue.Pop()->message.kind == WireMessageKind::HELLO);

    queue.RemovePeer(5);
    BOOST_CHECK(queue.Empty());
    BOOST_CHECK_EQUAL(queue.PeerBytes(5), 0U);
    BOOST_CHECK_EQUAL(queue.PeerCount(5), 0U);
}

BOOST_AUTO_TEST_CASE(control_frames_are_rate_limited_per_peer_not_market)
{
    using namespace flowmesh;
    BoundedWireQueue queue;
    const auto now{WireClock::time_point{std::chrono::seconds{300}}};
    for (size_t i{0}; i < 64; ++i) {
        auto hello{Message(WireMessageKind::HELLO)};
        hello.header.market_id = uint256::ONE;
        hello.header.market_id.begin()[1] = static_cast<unsigned char>(i + 1);
        BOOST_REQUIRE(queue.Push(9, std::move(hello), now) ==
                      QueueResult::ACCEPTED);
    }
    auto another_market{Message(WireMessageKind::HELLO)};
    another_market.header.market_id = uint256::ONE;
    another_market.header.market_id.begin()[1] = 100;
    BOOST_CHECK(queue.Push(9, std::move(another_market), now) ==
                QueueResult::RATE_LIMITED);
    BOOST_CHECK_EQUAL(queue.PeerCount(9), 64U);

    auto refilled{Message(WireMessageKind::HELLO)};
    refilled.header.market_id = uint256::ONE;
    refilled.header.market_id.begin()[1] = 101;
    BOOST_CHECK(queue.Push(9, std::move(refilled),
                           now + std::chrono::seconds{1}) ==
                QueueResult::ACCEPTED);
}

BOOST_AUTO_TEST_CASE(unsolicited_or_oversized_catchup_responses_are_rejected)
{
    using namespace flowmesh;
    CatchupRequestTracker tracker;
    BOOST_CHECK(!tracker.AcceptResponse(7, uint256::ONE, 10, 1, 100));
    BOOST_REQUIRE(tracker.Begin(7, uint256::ONE, 10, 4, 1000));
    BOOST_CHECK(!tracker.Begin(7, uint256::ONE, 10, 4, 1000));
    BOOST_CHECK(!tracker.AcceptResponse(7, uint256::ONE, 11, 1, 100));
    BOOST_CHECK(!tracker.AcceptResponse(7, uint256::ONE, 10, 5, 100));
    BOOST_CHECK(!tracker.AcceptResponse(7, uint256::ONE, 10, 4, 1001));
    BOOST_CHECK(tracker.AcceptResponse(7, uint256::ONE, 10, 4, 1000));
    BOOST_CHECK_EQUAL(tracker.Size(), 0U);

    BOOST_REQUIRE(tracker.Begin(8, uint256::ONE, 0, 1, 1));
    tracker.RemovePeer(8);
    BOOST_CHECK_EQUAL(tracker.Size(), 0U);
}

BOOST_AUTO_TEST_SUITE_END()
