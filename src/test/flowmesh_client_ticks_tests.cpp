// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <node/flowmesh_client_ticks.h>

#include <univalue.h>

#include <boost/test/unit_test.hpp>

#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace {
using Tick = flowmesh::ClearingTick;
using Side = flowmesh::ImbalanceSide;

uint256 TickHash(const uint64_t sequence)
{
    uint256 out;
    *out.begin() = static_cast<uint8_t>(sequence + 1);
    *(out.begin() + 1) = static_cast<uint8_t>((sequence + 1) >> 8);
    return out;
}

//! A matched clearing at even sequences, none at odd ones.
Tick At(const uint64_t sequence)
{
    Tick tick;
    tick.sequence = sequence;
    tick.microblock_hash = TickHash(sequence);
    if (sequence % 2 == 0) {
        tick.cleared = true;
        tick.price = 100 + static_cast<CAmount>(sequence);
        tick.volume = 5;
        tick.imbalance = 3;
        tick.imbalance_side = Side::BID;
    }
    return tick;
}

UniValue Row(const Tick& tick)
{
    UniValue row{UniValue::VOBJ};
    row.pushKV("sequence", tick.sequence);
    row.pushKV("microblock_hash", tick.microblock_hash.GetHex());
    row.pushKV("kind", flowmesh::ClearingTickKindName(tick.kind));
    row.pushKV("cleared", tick.cleared);
    row.pushKV("price", tick.price);
    row.pushKV("volume", tick.volume);
    row.pushKV("imbalance", tick.imbalance);
    row.pushKV("imbalance_side", flowmesh::ImbalanceSideName(tick.imbalance_side));
    return row;
}

UniValue Page(const std::vector<Tick>& ticks, const bool gap = false)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("available", true);
    out.pushKV("source", "local_engine");
    out.pushKV("gap", gap);
    out.pushKV("more", false);
    UniValue rows{UniValue::VARR};
    for (const auto& tick : ticks) rows.push_back(Row(tick));
    out.pushKV("ticks", std::move(rows));
    return out;
}

std::vector<Tick> Range(const uint64_t first, const uint64_t last)
{
    std::vector<Tick> out;
    for (uint64_t sequence{first}; sequence <= last; ++sequence) out.push_back(At(sequence));
    return out;
}

std::vector<uint64_t> Sequences(const std::deque<Tick>& ring)
{
    std::vector<uint64_t> out;
    for (const auto& tick : ring) out.push_back(tick.sequence);
    return out;
}

size_t Advertised(const std::string& json)
{
    UniValue status;
    BOOST_REQUIRE(status.read(json));
    return node::FlowMeshAdvertisedClearingTicks(status);
}

constexpr size_t LIMIT{256};
} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_client_ticks_tests)

BOOST_AUTO_TEST_CASE(advertised_clearing_ticks_are_capped_and_malformed_hints_disable_them)
{
    BOOST_CHECK_EQUAL(Advertised(R"({"running":true})"), 0U); // Older endpoints.
    BOOST_CHECK_EQUAL(Advertised(R"({"clearing_ticks_max":0})"), 0U);
    BOOST_CHECK_EQUAL(Advertised(R"({"clearing_ticks_max":16})"), 16U);
    BOOST_CHECK_EQUAL(Advertised(R"({"clearing_ticks_max":100000})"), flowmesh::MARKET_DATA_MAX_TICKS);
    for (const char* bad : {R"({"clearing_ticks_max":-1})", R"({"clearing_ticks_max":1.5})", R"({"clearing_ticks_max":"16"})",
                            R"({"clearing_ticks_max":true})", R"({"clearing_ticks_max":18446744073709551616})", "[]"}) {
        BOOST_CHECK_EQUAL(Advertised(bad), 0U);
    }
}

BOOST_AUTO_TEST_CASE(ticks_merge_incrementally_and_bind_the_verified_head)
{
    std::deque<Tick> ring;
    // The newest page, ending at the verified head.
    BOOST_CHECK_EQUAL(node::FlowMeshMergeClearingTicks(ring, Page(Range(5, 9)), std::nullopt, LIMIT, 9, TickHash(9)), "");
    BOOST_CHECK((Sequences(ring) == std::vector<uint64_t>{5, 6, 7, 8, 9}));
    BOOST_CHECK(ring.back() == At(9));
    // The ticks after the last one held.
    BOOST_CHECK_EQUAL(node::FlowMeshMergeClearingTicks(ring, Page(Range(10, 11)), 9, LIMIT, 11, TickHash(11)), "");
    BOOST_CHECK_EQUAL(ring.back().sequence, 11U);
    // A page reaching past the verified head: the rest cannot be bound yet.
    BOOST_CHECK_EQUAL(node::FlowMeshMergeClearingTicks(ring, Page(Range(12, 14)), 11, LIMIT, 12, TickHash(12)), "");
    BOOST_CHECK_EQUAL(ring.back().sequence, 12U);
    // Overlap with ticks already held is accepted when identical.
    BOOST_CHECK_EQUAL(node::FlowMeshMergeClearingTicks(ring, Page(Range(10, 13)), std::nullopt, LIMIT, 13, TickHash(13)), "");
    BOOST_CHECK((Sequences(ring) == std::vector<uint64_t>{5, 6, 7, 8, 9, 10, 11, 12, 13}));
    // An empty page at an unchanged head changes nothing.
    BOOST_CHECK_EQUAL(node::FlowMeshMergeClearingTicks(ring, Page({}), 13, LIMIT, 13, TickHash(13)), "");
    BOOST_CHECK_EQUAL(ring.size(), 9U);
    // An endpoint's own unavailability leaves what is held.
    UniValue unavailable{UniValue::VOBJ};
    unavailable.pushKV("available", false);
    unavailable.pushKV("unavailable_reason", "history_not_ready");
    BOOST_CHECK_EQUAL(node::FlowMeshMergeClearingTicks(ring, unavailable, 13, LIMIT, 13, TickHash(13)), "endpoint_unavailable: history_not_ready");
    BOOST_CHECK_EQUAL(ring.size(), 9U);
    // An evicted stretch, flagged as a gap by the endpoint, starts again.
    BOOST_CHECK_EQUAL(node::FlowMeshMergeClearingTicks(ring, Page(Range(20, 21), true), 13, LIMIT, 21, TickHash(21)), "");
    BOOST_CHECK((Sequences(ring) == std::vector<uint64_t>{20, 21}));
}

BOOST_AUTO_TEST_CASE(tick_pages_that_contradict_the_head_or_themselves_clear_the_ring)
{
    const auto rejected = [](const UniValue& page, const std::optional<uint64_t> after, const uint64_t head,
                             const uint256& hash, const size_t limit = LIMIT) {
        std::deque<Tick> ring{At(3), At(4)};
        const auto reason{node::FlowMeshMergeClearingTicks(ring, page, after, limit, head, hash)};
        BOOST_CHECK_MESSAGE(reason.starts_with("endpoint_clearing_ticks_rejected"), reason);
        BOOST_CHECK(ring.empty());
    };
    // The tick at the verified head must be that head.
    rejected(Page(Range(5, 6)), 4, 6, TickHash(99));
    // Ticks already held never change.
    auto changed{At(4)};
    changed.price = 1;
    rejected(Page({changed, At(5)}), std::nullopt, 5, TickHash(5));
    // Order, position and bounds.
    rejected(Page({At(5), At(7)}), 4, 7, TickHash(7));
    rejected(Page({At(6), At(5)}), 4, 6, TickHash(6));
    rejected(Page(Range(4, 5)), 4, 5, TickHash(5));
    rejected(Page(Range(6, 7)), 4, 7, TickHash(7)); // A skip without the endpoint's gap flag.
    rejected(Page(Range(5, 7)), 4, 7, TickHash(7), 2);
    // Values the engine cannot have produced.
    const auto malformed = [&](const auto& change) {
        auto tick{At(6)};
        change(tick);
        rejected(Page({At(5), tick}), 4, 6, tick.microblock_hash);
    };
    malformed([](Tick& tick) { tick.kind = Tick::Kind::HANDOFF; });
    malformed([](Tick& tick) { tick.volume = 0; });
    malformed([](Tick& tick) { tick.imbalance_side = Side::NONE; });
    malformed([](Tick& tick) { tick.imbalance = 0; });
    malformed([](Tick& tick) { tick.cleared = false; });
    auto quiet{At(5)};
    quiet.imbalance_side = Side::UNKNOWN;
    rejected(Page({quiet}), 4, 5, TickHash(5));
    auto row{Row(At(5))};
    row.pushKV("kind", "trade");
    UniValue rows{UniValue::VARR};
    rows.push_back(row);
    auto page{Page({})};
    page.pushKV("ticks", rows);
    rejected(page, 4, 5, TickHash(5));
    rejected(UniValue{}, 4, 5, TickHash(5));
    rejected(UniValue{UniValue::VARR}, 4, 5, TickHash(5));
    // A clearing whose side was not observed is kept as unknown.
    std::deque<Tick> ring;
    auto unknown{At(6)};
    unknown.imbalance_side = Side::UNKNOWN;
    BOOST_CHECK_EQUAL(node::FlowMeshMergeClearingTicks(ring, Page({unknown}), std::nullopt, LIMIT, 6, TickHash(6)), "");
    BOOST_CHECK(ring.front().imbalance_side == Side::UNKNOWN);
    // A handoff or settlement tick is side "none" without a clearing.
    Tick handoff{At(7)};
    handoff.kind = Tick::Kind::HANDOFF;
    BOOST_CHECK_EQUAL(node::FlowMeshMergeClearingTicks(ring, Page({handoff}), 6, LIMIT, 7, TickHash(7)), "");
    BOOST_CHECK(ring.back().kind == Tick::Kind::HANDOFF);
}

BOOST_AUTO_TEST_CASE(held_ticks_are_bounded)
{
    std::deque<Tick> ring;
    uint64_t next{0};
    for (int page{0}; page < 5; ++page) {
        const auto ticks{Range(next, next + 99)};
        next += 100;
        BOOST_REQUIRE_EQUAL(node::FlowMeshMergeClearingTicks(ring, Page(ticks), page ? std::optional{next - 101} : std::nullopt,
                                                             LIMIT, next - 1, TickHash(next - 1)), "");
    }
    BOOST_CHECK_EQUAL(ring.size(), flowmesh::MARKET_DATA_MAX_TICKS);
    BOOST_CHECK_EQUAL(ring.front().sequence, next - flowmesh::MARKET_DATA_MAX_TICKS);
    BOOST_CHECK_EQUAL(ring.back().sequence, next - 1);
}

BOOST_AUTO_TEST_SUITE_END()
