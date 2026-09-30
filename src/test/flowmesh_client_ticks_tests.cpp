// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <node/flowmesh_client_ticks.h>

#include <flowmesh/observed_time.h>
#include <node/flowmesh_client.h>

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

namespace {
UniValue Stamp(const uint64_t sequence, const uint256& hash, const char* mode, const int64_t ms,
               const std::vector<std::string>& flags = {})
{
    UniValue row{UniValue::VOBJ};
    row.pushKV("sequence", sequence);
    row.pushKV("microblock_hash", hash.GetHex());
    row.pushKV("mode", mode);
    if (std::string{mode} == "live") row.pushKV("observed_ms", ms);
    if (std::string{mode} == "delayed") row.pushKV("applied_here_ms", ms);
    UniValue names{UniValue::VARR};
    for (const auto& flag : flags) names.push_back(flag);
    row.pushKV("flags", names);
    return row;
}
UniValue Section(const std::vector<UniValue>& rows, const bool suspect = false)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("available", true);
    out.pushKV("source", "endpoint_operator");
    out.pushKV("source_kind", "operator_observed");
    out.pushKV("certified", false);
    out.pushKV("clock_suspect", suspect);
    UniValue array{UniValue::VARR};
    for (const auto& row : rows) array.push_back(row);
    out.pushKV("rows", array);
    return out;
}
constexpr int64_t NOW{1790769600000};
} // namespace

BOOST_AUTO_TEST_CASE(observed_adverts_are_read_leniently)
{
    UniValue status;
    BOOST_REQUIRE(status.read(R"({"observed_time_v1":true,"observed_candles_max":100000})"));
    BOOST_CHECK(node::FlowMeshAdvertisedObservedTime(status));
    BOOST_CHECK_EQUAL(node::FlowMeshAdvertisedObservedCandles(status), flowmesh::OBSERVED_CANDLE_PAGE_MAX);
    for (const char* bad : {R"({"observed_time_v1":1,"observed_candles_max":-1})", R"({"observed_time_v1":"true","observed_candles_max":"5"})", "[]", "{}"}) {
        BOOST_REQUIRE(status.read(bad));
        BOOST_CHECK(!node::FlowMeshAdvertisedObservedTime(status));
        BOOST_CHECK_EQUAL(node::FlowMeshAdvertisedObservedCandles(status), 0U);
    }
}

BOOST_AUTO_TEST_CASE(observed_times_bind_to_held_ticks_and_are_flagged_never_corrected)
{
    std::deque<Tick> ring;
    for (uint64_t i{10}; i < 16; ++i) ring.push_back(At(i));
    std::map<uint64_t, flowmesh::ObservedStamp> held;
    bool suspect{false};
    const auto reason{node::FlowMeshMergeObservedTimes(held, Section({
        Stamp(10, TickHash(10), "live", NOW - 5000),
        // Another entry's hash at a held sequence, and a sequence not held: dropped.
        Stamp(11, TickHash(99), "live", NOW - 4000),
        Stamp(99, TickHash(99), "live", NOW - 4000),
        // Backwards: flagged, kept.
        Stamp(12, TickHash(12), "live", NOW - 6000),
        Stamp(13, TickHash(13), "delayed", NOW - 1000, {"restart", "signed_here", "clock_step"}),
        Stamp(14, TickHash(14), "untimed", 0),
        // Far in the future of its receipt: flagged.
        Stamp(15, TickHash(15), "live", NOW + 60'000)}, true), ring, NOW, suspect)};
    BOOST_CHECK_EQUAL(reason, "");
    BOOST_CHECK(suspect);
    BOOST_REQUIRE_EQUAL(held.size(), 5U);
    BOOST_CHECK(!held.contains(11));
    BOOST_CHECK(!(held.at(10).flags & flowmesh::OBSERVED_IMPLAUSIBLE));
    BOOST_CHECK(held.at(12).flags & flowmesh::OBSERVED_IMPLAUSIBLE);
    BOOST_CHECK_EQUAL(held.at(12).ms, NOW - 6000);
    // Only public flags are taken from an endpoint.
    BOOST_CHECK_EQUAL(held.at(13).flags, flowmesh::OBSERVED_RESTART | flowmesh::OBSERVED_CLOCK_STEP);
    BOOST_CHECK(held.at(13).mode == flowmesh::ObservedMode::DELAYED);
    BOOST_CHECK(held.at(14).mode == flowmesh::ObservedMode::UNTIMED);
    BOOST_CHECK(held.at(15).flags & flowmesh::OBSERVED_IMPLAUSIBLE);
    // The ring moves on: stamps it no longer holds are forgotten.
    ring.pop_front();
    ring.pop_front();
    BOOST_CHECK_EQUAL(node::FlowMeshMergeObservedTimes(held, Section({}), ring, NOW, suspect), "");
    BOOST_CHECK_EQUAL(held.size(), 4U);
    BOOST_CHECK(!held.contains(10));
    BOOST_CHECK(!suspect);
    // The endpoint's own "unavailable" keeps what is held; a malformed
    // section clears it.
    UniValue unavailable{UniValue::VOBJ};
    unavailable.pushKV("available", false);
    unavailable.pushKV("unavailable_reason", "not_recorded_by_this_node");
    BOOST_CHECK_EQUAL(node::FlowMeshMergeObservedTimes(held, unavailable, ring, NOW, suspect), "endpoint_unavailable: not_recorded_by_this_node");
    BOOST_CHECK_EQUAL(held.size(), 4U);
    const auto rejected{node::FlowMeshMergeObservedTimes(held, Section({Stamp(13, TickHash(13), "sometimes", NOW)}), ring, NOW, suspect)};
    BOOST_CHECK(rejected.starts_with("endpoint_observed_time_rejected"));
    BOOST_CHECK(held.empty());
}

namespace {
UniValue CandleReply(const std::vector<UniValue>& candles, const char* interval = "1m", const int64_t before = NOW)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("available", true);
    out.pushKV("source", "endpoint_operator");
    out.pushKV("source_kind", "operator_observed");
    out.pushKV("certified", false);
    out.pushKV("interval", interval);
    out.pushKV("interval_ms", 60'000);
    out.pushKV("from_ms", before - 10 * 60'000);
    out.pushKV("before_ms", before);
    out.pushKV("more", false);
    UniValue array{UniValue::VARR};
    for (const auto& candle : candles) array.push_back(candle);
    out.pushKV("candles", array);
    return out;
}
UniValue Candle(const int64_t open_ms, const uint64_t first, const uint64_t last, const bool priced = true)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("open_ms", open_ms);
    out.pushKV("first_sequence", first);
    out.pushKV("last_sequence", last);
    out.pushKV("entries", last - first + 1);
    out.pushKV("clearings", priced ? uint64_t{1} : uint64_t{0});
    if (priced) {
        out.pushKV("open", 5);
        out.pushKV("high", 9);
        out.pushKV("low", 4);
        out.pushKV("close", 8);
    }
    out.pushKV("base_volume", priced ? "340282366920938463463374607431768211455" : "0");
    out.pushKV("quote_volume", priced ? "7" : "0");
    UniValue flags{UniValue::VARR};
    flags.push_back("partial");
    flags.push_back("future_flag");
    out.pushKV("flags", flags);
    return out;
}
} // namespace

BOOST_AUTO_TEST_CASE(endpoint_candles_are_structure_checked)
{
    std::string error;
    const auto page{node::FlowMeshParseObservedCandles(CandleReply({Candle(NOW - 5 * 60'000, 3, 4), Candle(NOW - 60'000, 7, 7, false)}),
                                                       flowmesh::CandleInterval::M1, 7, error)};
    BOOST_REQUIRE_MESSAGE(page, error);
    BOOST_CHECK(page->remote);
    BOOST_REQUIRE_EQUAL(page->candles.size(), 2U);
    BOOST_CHECK(page->candles[0].base_volume == ~util::Unsigned128{0});
    BOOST_CHECK_EQUAL(page->candles[0].flags, flowmesh::CANDLE_PARTIAL);
    BOOST_CHECK_EQUAL(page->candles[1].clearings, 0U);
    // Refused: beyond the verified head, misaligned, overlapping, another
    // interval, inconsistent prices, values without a clearing, a volume
    // past 128 bits.
    auto high_low{Candle(NOW - 60'000, 3, 3)};
    high_low.pushKV("low", 10);
    auto phantom{Candle(NOW - 60'000, 3, 3, false)};
    phantom.pushKV("base_volume", "1");
    auto huge{Candle(NOW - 60'000, 3, 3)};
    huge.pushKV("base_volume", "340282366920938463463374607431768211456");
    for (const auto& [reply, head] : std::vector<std::pair<UniValue, uint64_t>>{
             {CandleReply({Candle(NOW - 60'000, 3, 8)}), 7},
             {CandleReply({Candle(NOW - 60'001, 3, 3)}), 7},
             {CandleReply({Candle(NOW - 3 * 60'000, 3, 5), Candle(NOW - 60'000, 5, 6)}), 7},
             {CandleReply({Candle(NOW - 60'000, 3, 3)}, "5m"), 7},
             {CandleReply({high_low}), 7},
             {CandleReply({phantom}), 7},
             {CandleReply({huge}), 7},
             {CandleReply({Candle(NOW, 3, 3)}), 7}}) {
        BOOST_CHECK(!node::FlowMeshParseObservedCandles(reply, flowmesh::CandleInterval::M1, head, error));
        BOOST_CHECK(error.starts_with("endpoint_candles_rejected"));
    }
    // What the node writes, the client reads back exactly.
    flowmesh::ObservedCandlePage written;
    written.available = true;
    written.interval = flowmesh::CandleInterval::M5;
    written.before_ms = NOW;
    written.from_ms = NOW - 3 * 300'000;
    flowmesh::ObservedCandle candle;
    candle.open_ms = NOW - 300'000;
    candle.first_sequence = candle.last_sequence = 2;
    candle.entries = candle.clearings = 1;
    candle.open = candle.high = candle.low = candle.close = 42;
    candle.base_volume = 3;
    candle.quote_volume = 126;
    candle.flags = flowmesh::CANDLE_LATE | flowmesh::CANDLE_SEAM_BEFORE;
    written.candles.push_back(candle);
    const auto read{node::FlowMeshParseObservedCandles(node::FlowMeshObservedCandlePageJson(written), flowmesh::CandleInterval::M5, 2, error)};
    BOOST_REQUIRE_MESSAGE(read, error);
    BOOST_REQUIRE_EQUAL(read->candles.size(), 1U);
    BOOST_CHECK(read->candles[0] == candle);
    BOOST_CHECK_EQUAL(read->from_ms, written.from_ms);
}

BOOST_AUTO_TEST_SUITE_END()
