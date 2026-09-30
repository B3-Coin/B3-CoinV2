// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! Operator-observed time: liveness classification and UTC candle rollups.
//! Display only; nothing here reads a clock or touches certified state.

#include <flowmesh/observed_time.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace {

using flowmesh::CandleInterval;
using flowmesh::ClearingTick;
using flowmesh::ObservedCandle;
using flowmesh::ObservedDelay;
using flowmesh::ObservedMode;
using flowmesh::ObservedRollup;
using flowmesh::ObservedStamp;

// 2026-09-30T12:00:00Z
constexpr int64_t T0{1790769600000};

uint256 EntryHash(const uint64_t sequence)
{
    uint256 out;
    out.data()[0] = static_cast<unsigned char>(sequence & 0xff);
    out.data()[1] = static_cast<unsigned char>((sequence >> 8) & 0xff);
    out.data()[31] = 1;
    return out;
}

ObservedStamp Stamp(const uint64_t sequence, const int64_t ms, const ObservedMode mode = ObservedMode::LIVE, const uint8_t flags = 0)
{
    ObservedStamp out;
    out.sequence = sequence;
    out.hash = EntryHash(sequence);
    out.mode = mode;
    out.ms = mode == ObservedMode::UNTIMED ? 0 : ms;
    out.flags = flags;
    out.delay = mode == ObservedMode::LIVE ? ObservedDelay::NONE : ObservedDelay::CATCHUP;
    return out;
}

ClearingTick Tick(const uint64_t sequence, const CAmount price, const CAmount volume)
{
    ClearingTick out;
    out.sequence = sequence;
    out.microblock_hash = EntryHash(sequence);
    out.cleared = volume > 0;
    out.price = volume > 0 ? price : 0;
    out.volume = volume;
    return out;
}

const ObservedCandle& Only(const ObservedRollup& rollup, CandleInterval interval)
{
    BOOST_REQUIRE_EQUAL(rollup.Candles(interval).size(), 1U);
    return rollup.Candles(interval).front();
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_observed_time_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(utc_alignment_all_intervals)
{
    // 12:34:56.789 UTC on 2026-09-30.
    const int64_t ms{T0 + ((34 * 60) + 56) * 1000 + 789};
    BOOST_CHECK_EQUAL(flowmesh::CandleOpenMs(ms, CandleInterval::M1), T0 + 34 * 60'000);
    BOOST_CHECK_EQUAL(flowmesh::CandleOpenMs(ms, CandleInterval::M5), T0 + 30 * 60'000);
    BOOST_CHECK_EQUAL(flowmesh::CandleOpenMs(ms, CandleInterval::M15), T0 + 30 * 60'000);
    BOOST_CHECK_EQUAL(flowmesh::CandleOpenMs(ms, CandleInterval::H1), T0);
    // 4h buckets start at 00, 04, 08, 12, 16 and 20 UTC.
    BOOST_CHECK_EQUAL(flowmesh::CandleOpenMs(ms, CandleInterval::H4), T0);
    BOOST_CHECK_EQUAL(flowmesh::CandleOpenMs(ms - 1, CandleInterval::H4) % 14'400'000, 0);
    // Days start at 00:00 UTC.
    BOOST_CHECK_EQUAL(flowmesh::CandleOpenMs(ms, CandleInterval::D1), T0 - 12 * 3'600'000);
    BOOST_CHECK_EQUAL(flowmesh::CandleOpenMs(-5, CandleInterval::M1), 0);
    for (const auto interval : flowmesh::CANDLE_INTERVALS) {
        BOOST_CHECK(flowmesh::ParseCandleInterval(flowmesh::CandleIntervalName(interval)) == interval);
        BOOST_CHECK_GE(flowmesh::CandleMemoryWindow(interval), flowmesh::OBSERVED_CANDLE_PAGE_MAX);
    }
    BOOST_CHECK(!flowmesh::ParseCandleInterval("2m"));
}

BOOST_AUTO_TEST_CASE(ohlc_and_128bit_volumes_exact)
{
    ObservedRollup rollup;
    const CAmount big{MAX_MONEY};
    rollup.Add(Stamp(10, T0 + 1'000), Tick(10, 500, 7));
    rollup.Add(Stamp(11, T0 + 2'000), Tick(11, 900, 3));
    rollup.Add(Stamp(12, T0 + 3'000), Tick(12, 100, 5));
    rollup.Add(Stamp(13, T0 + 4'000), Tick(13, big, big));
    const auto& candle{Only(rollup, CandleInterval::M1)};
    BOOST_CHECK_EQUAL(candle.open_ms, T0);
    BOOST_CHECK_EQUAL(candle.first_sequence, 10U);
    BOOST_CHECK_EQUAL(candle.last_sequence, 13U);
    BOOST_CHECK_EQUAL(candle.open, 500);
    BOOST_CHECK_EQUAL(candle.high, big);
    BOOST_CHECK_EQUAL(candle.low, 100);
    BOOST_CHECK_EQUAL(candle.close, big);
    BOOST_CHECK_EQUAL(candle.clearings, 4U);
    BOOST_CHECK_EQUAL(candle.entries, 4U);
    const util::Unsigned128 base{util::Unsigned128{7 + 3 + 5} + static_cast<uint64_t>(big)};
    const util::Unsigned128 quote{util::Unsigned128{500 * 7 + 900 * 3 + 100 * 5} +
                                  util::Unsigned128{static_cast<uint64_t>(big)} * static_cast<uint64_t>(big)};
    BOOST_CHECK(candle.base_volume == base);
    BOOST_CHECK(candle.quote_volume == quote);
    BOOST_CHECK(candle.flags == 0);
    // Every interval saw the same entries.
    for (const auto interval : flowmesh::CANDLE_INTERVALS) BOOST_CHECK(Only(rollup, interval).quote_volume == quote);
}

BOOST_AUTO_TEST_CASE(non_clearing_entries_counted_without_prices)
{
    ObservedRollup rollup;
    rollup.Add(Stamp(1, T0 + 100), Tick(1, 0, 0));
    rollup.Add(Stamp(2, T0 + 200), std::nullopt);
    // A tick of another entry never prices this one.
    rollup.Add(Stamp(3, T0 + 300), Tick(4, 77, 9));
    const auto& candle{Only(rollup, CandleInterval::M5)};
    BOOST_CHECK_EQUAL(candle.entries, 3U);
    BOOST_CHECK_EQUAL(candle.clearings, 0U);
    BOOST_CHECK(candle.base_volume == 0);
    rollup.Add(Stamp(4, T0 + 400), Tick(4, 77, 9));
    BOOST_CHECK_EQUAL(Only(rollup, CandleInterval::M5).open, 77);
    BOOST_CHECK_EQUAL(Only(rollup, CandleInterval::M5).clearings, 1U);
}

BOOST_AUTO_TEST_CASE(delayed_entries_placed_only_inside_their_bound)
{
    ObservedRollup rollup;
    rollup.Add(Stamp(1, T0 + 10'000), Tick(1, 50, 1));
    // Applied late, but between the last LIVE time and now nothing crosses a
    // minute: counted in that minute, marked late.
    rollup.Add(Stamp(2, T0 + 12'000, ObservedMode::DELAYED), Tick(2, 60, 2));
    {
        const auto& candle{Only(rollup, CandleInterval::M1)};
        BOOST_CHECK_EQUAL(candle.entries, 2U);
        BOOST_CHECK_EQUAL(candle.close, 60);
        BOOST_CHECK(candle.flags & flowmesh::CANDLE_LATE);
        BOOST_CHECK(!(candle.flags & flowmesh::CANDLE_PARTIAL));
    }
    // A bound from 12:00:10 to 12:01:30 crosses a minute: placed nowhere,
    // both minutes are partial (the second when it forms), and the hour holds it.
    rollup.Add(Stamp(3, T0 + 90'000, ObservedMode::DELAYED), Tick(3, 70, 3));
    BOOST_CHECK(Only(rollup, CandleInterval::M1).flags & flowmesh::CANDLE_PARTIAL);
    BOOST_CHECK_EQUAL(Only(rollup, CandleInterval::M1).entries, 2U);
    BOOST_CHECK_EQUAL(Only(rollup, CandleInterval::H1).entries, 3U);
    BOOST_CHECK(Only(rollup, CandleInterval::H1).flags & flowmesh::CANDLE_LATE);
    rollup.Add(Stamp(4, T0 + 95'000), Tick(4, 80, 4));
    const auto& minutes{rollup.Candles(CandleInterval::M1)};
    BOOST_REQUIRE_EQUAL(minutes.size(), 2U);
    BOOST_CHECK(minutes.back().flags & flowmesh::CANDLE_PARTIAL);
    // Not contiguous: sequence 3 is between them, so no quiet fill.
    BOOST_CHECK_EQUAL(minutes.front().last_sequence + 1, 3U);
    BOOST_CHECK_EQUAL(minutes.back().first_sequence, 4U);
}

BOOST_AUTO_TEST_CASE(delayed_without_a_live_bound_is_never_placed)
{
    ObservedRollup rollup;
    // Nothing before it: an upper bound alone places nothing.
    rollup.Add(Stamp(5, T0 + 1'000, ObservedMode::DELAYED), Tick(5, 10, 1));
    for (const auto interval : flowmesh::CANDLE_INTERVALS) BOOST_CHECK(rollup.Candles(interval).empty());
    rollup.Add(Stamp(6, T0 + 2'000), Tick(6, 20, 1));
    const auto& candle{Only(rollup, CandleInterval::M1)};
    BOOST_CHECK_EQUAL(candle.first_sequence, 6U);
    BOOST_CHECK(candle.flags & flowmesh::CANDLE_PARTIAL);
    // A sequence gap also ends the bound: the LIVE time before it bounds nothing.
    rollup.Add(Stamp(9, T0 + 3'000, ObservedMode::DELAYED), Tick(9, 30, 1));
    BOOST_CHECK_EQUAL(Only(rollup, CandleInterval::M1).entries, 1U);
}

BOOST_AUTO_TEST_CASE(contiguity_and_seams)
{
    ObservedRollup rollup;
    rollup.Add(Stamp(1, T0 + 1'000), Tick(1, 5, 1));
    rollup.Add(Stamp(2, T0 + 5 * 60'000), Tick(2, 6, 1));
    auto minutes{rollup.Candles(CandleInterval::M1)};
    BOOST_REQUIRE_EQUAL(minutes.size(), 2U);
    // Consecutive sequences and no seam: the empty minutes between are proven quiet.
    BOOST_CHECK_EQUAL(minutes[1].first_sequence, minutes[0].last_sequence + 1);
    BOOST_CHECK(!(minutes[1].flags & flowmesh::CANDLE_SEAM_BEFORE));
    // A clock step at the first entry of a candle breaks that proof.
    rollup.Add(Stamp(3, T0 + 65 * 60'000, ObservedMode::LIVE, flowmesh::OBSERVED_CLOCK_STEP), Tick(3, 7, 1));
    minutes = rollup.Candles(CandleInterval::M1);
    BOOST_CHECK(minutes.back().flags & flowmesh::CANDLE_SEAM_BEFORE);
    BOOST_CHECK(minutes.back().flags & flowmesh::CANDLE_CLOCK);
    // So does a restart.
    rollup.Add(Stamp(4, T0 + 70 * 60'000, ObservedMode::LIVE, flowmesh::OBSERVED_RESTART), Tick(4, 8, 1));
    BOOST_CHECK(rollup.Candles(CandleInterval::M1).back().flags & flowmesh::CANDLE_SEAM_BEFORE);
    // A clamped stamp inside a candle marks the candle, not a seam.
    rollup.Add(Stamp(5, T0 + 70 * 60'000, ObservedMode::LIVE, flowmesh::OBSERVED_CLOCK_CLAMPED), Tick(5, 9, 1));
    BOOST_CHECK(rollup.Candles(CandleInterval::M1).back().flags & flowmesh::CANDLE_CLOCK);
    BOOST_CHECK_EQUAL(rollup.Candles(CandleInterval::M1).back().entries, 2U);
}

BOOST_AUTO_TEST_CASE(untimed_entries_mark_the_next_candle_partial)
{
    ObservedRollup rollup;
    rollup.Add(Stamp(1, T0 + 1'000), Tick(1, 5, 1));
    rollup.Add(Stamp(2, 0, ObservedMode::UNTIMED), Tick(2, 6, 1));
    BOOST_CHECK(Only(rollup, CandleInterval::M1).flags & flowmesh::CANDLE_PARTIAL);
    rollup.Add(Stamp(3, T0 + 3 * 60'000), Tick(3, 7, 1));
    const auto& minutes{rollup.Candles(CandleInterval::M1)};
    BOOST_REQUIRE_EQUAL(minutes.size(), 2U);
    BOOST_CHECK(minutes.back().flags & flowmesh::CANDLE_PARTIAL);
    BOOST_CHECK_EQUAL(minutes.back().first_sequence, 3U);
    // Earlier-sequence stamps are ignored.
    rollup.Add(Stamp(2, T0 + 4 * 60'000), Tick(2, 6, 1));
    BOOST_CHECK_EQUAL(rollup.Candles(CandleInterval::M1).size(), 2U);
}

BOOST_AUTO_TEST_CASE(liveness_classifier_matrix)
{
    flowmesh::ObservedLivenessInputs base;
    base.sequence = 100;
    base.warm = true;
    BOOST_CHECK(flowmesh::ClassifyObservedLiveness(base) == ObservedDelay::NONE);
    auto in{base};
    in.from_catchup = true;
    BOOST_CHECK(flowmesh::ClassifyObservedLiveness(in) == ObservedDelay::CATCHUP);
    in = base;
    in.deferred_watermark = 100;
    BOOST_CHECK(flowmesh::ClassifyObservedLiveness(in) == ObservedDelay::DEFERRED);
    in.deferred_watermark = 99;
    BOOST_CHECK(flowmesh::ClassifyObservedLiveness(in) == ObservedDelay::NONE);
    in = base;
    // A verified certificate for this entry itself is not a lag.
    in.proven_target = 101;
    BOOST_CHECK(flowmesh::ClassifyObservedLiveness(in) == ObservedDelay::NONE);
    in.proven_target = 102;
    BOOST_CHECK(flowmesh::ClassifyObservedLiveness(in) == ObservedDelay::BEHIND);
    in = base;
    in.anchor_stale = true;
    BOOST_CHECK(flowmesh::ClassifyObservedLiveness(in) == ObservedDelay::STALE_ANCHOR);
    in = base;
    in.warm = false;
    BOOST_CHECK(flowmesh::ClassifyObservedLiveness(in) == ObservedDelay::WARMUP);

    flowmesh::ObservedWarmup warmup;
    const auto start{std::chrono::steady_clock::time_point{} + std::chrono::hours{1}};
    const auto period{flowmesh::ObservedWarmupFor(std::chrono::milliseconds{2000})};
    BOOST_CHECK_EQUAL(period.count(), 4000);
    BOOST_CHECK_EQUAL(flowmesh::ObservedWarmupFor(std::chrono::milliseconds{200}).count(), 2000);
    BOOST_CHECK(!warmup.Warm(start, period));
    warmup.Observe(true, start);
    BOOST_CHECK(!warmup.Warm(start + std::chrono::milliseconds{3999}, period));
    warmup.Observe(true, start + std::chrono::seconds{3});
    BOOST_CHECK(warmup.Warm(start + std::chrono::seconds{4}, period));
    warmup.Observe(false, start + std::chrono::seconds{5});
    warmup.Observe(true, start + std::chrono::seconds{6});
    BOOST_CHECK(!warmup.Warm(start + std::chrono::seconds{9}, period));
    BOOST_CHECK(warmup.Warm(start + std::chrono::seconds{10}, period));
    // An interruption within the tolerance keeps the warm-up; a longer one
    // restarts it.
    const std::chrono::milliseconds tolerance{2000};
    warmup.Observe(false, start + std::chrono::seconds{11}, tolerance);
    warmup.Observe(false, start + std::chrono::milliseconds{12500}, tolerance);
    warmup.Observe(true, start + std::chrono::seconds{13}, tolerance);
    BOOST_CHECK(warmup.Warm(start + std::chrono::seconds{13}, period));
    warmup.Observe(false, start + std::chrono::seconds{14}, tolerance);
    warmup.Observe(false, start + std::chrono::seconds{16}, tolerance);
    warmup.Observe(true, start + std::chrono::seconds{17}, tolerance);
    BOOST_CHECK(!warmup.Warm(start + std::chrono::seconds{20}, period));
    BOOST_CHECK(warmup.Warm(start + std::chrono::seconds{21}, period));
}

BOOST_AUTO_TEST_CASE(page_before_ms_limit_more)
{
    ObservedRollup rollup;
    for (uint64_t i{0}; i < 10; ++i) rollup.Add(Stamp(i, T0 + static_cast<int64_t>(i) * 60'000 + 1), Tick(i, 10 + i, 1));
    auto page{rollup.Page(CandleInterval::M1, std::nullopt, 4)};
    BOOST_CHECK(page.available);
    BOOST_REQUIRE_EQUAL(page.candles.size(), 4U);
    BOOST_CHECK_EQUAL(page.candles.front().first_sequence, 6U);
    BOOST_CHECK_EQUAL(page.candles.back().first_sequence, 9U);
    BOOST_CHECK_EQUAL(page.before_ms, T0 + 10 * 60'000);
    BOOST_CHECK_EQUAL(page.from_ms, T0 + 6 * 60'000);
    BOOST_CHECK(page.more);
    page = rollup.Page(CandleInterval::M1, page.from_ms, 4);
    BOOST_REQUIRE_EQUAL(page.candles.size(), 4U);
    BOOST_CHECK_EQUAL(page.candles.front().first_sequence, 2U);
    page = rollup.Page(CandleInterval::M1, page.from_ms, 4);
    BOOST_CHECK_EQUAL(page.candles.size(), 2U);
    BOOST_CHECK(!page.more);
    // The limit is bounded.
    page = rollup.Page(CandleInterval::M1, std::nullopt, 1'000'000);
    BOOST_CHECK_EQUAL(page.before_ms - page.from_ms, int64_t{500} * 60'000);
    BOOST_CHECK_EQUAL(rollup.Page(CandleInterval::M1, std::nullopt, 0).candles.size(), 1U);
}

BOOST_AUTO_TEST_CASE(retention_evicts_oldest_per_interval)
{
    ObservedRollup rollup;
    // Two days of one trade an hour: the minute window keeps one day.
    for (uint64_t i{0}; i < 48; ++i) rollup.Add(Stamp(i, T0 + static_cast<int64_t>(i) * 3'600'000 + 5), Tick(i, 1, 1));
    const auto& minutes{rollup.Candles(CandleInterval::M1)};
    BOOST_CHECK_LE(minutes.size(), 25U);
    BOOST_CHECK_GE(minutes.front().open_ms, minutes.back().open_ms - int64_t{1440} * 60'000);
    BOOST_CHECK_EQUAL(rollup.Candles(CandleInterval::H1).size(), 48U);
    BOOST_CHECK_EQUAL(rollup.Candles(CandleInterval::D1).size(), 3U);
}

BOOST_AUTO_TEST_CASE(extend_served_candles_with_live_ticks)
{
    ObservedRollup node;
    for (uint64_t i{0}; i < 5; ++i) node.Add(Stamp(i, T0 + static_cast<int64_t>(i) * 10'000 + 1), Tick(i, 100 + i, 2));
    const auto served{node.Page(CandleInterval::M1, std::nullopt, 10).candles};
    BOOST_REQUIRE_EQUAL(served.size(), 1U);
    std::vector<std::pair<ObservedStamp, ClearingTick>> newer;
    for (uint64_t i{3}; i < 9; ++i) {
        const auto stamp{Stamp(i, T0 + static_cast<int64_t>(i) * 10'000 + 1)};
        newer.emplace_back(stamp, Tick(i, 100 + i, 2));
        if (i >= 5) node.Add(stamp, Tick(i, 100 + i, 2));
    }
    bool incomplete{true};
    const auto extended{flowmesh::ExtendObservedCandles(served, CandleInterval::M1, newer, &incomplete)};
    BOOST_CHECK(!incomplete);
    // Exactly the node's own bucketing, with no entry counted twice.
    const auto& expected{node.Candles(CandleInterval::M1)};
    BOOST_REQUIRE_EQUAL(extended.size(), expected.size());
    for (size_t i{0}; i < extended.size(); ++i) BOOST_CHECK(extended[i] == expected[i]);
    // A ring that no longer reaches the served candle asks for a refetch.
    std::vector<std::pair<ObservedStamp, ClearingTick>> skipped{{Stamp(7, T0 + 70'001), Tick(7, 1, 1)}};
    flowmesh::ExtendObservedCandles(served, CandleInterval::M1, skipped, &incomplete);
    BOOST_CHECK(incomplete);
    // A late stamp without a known bound is placed nowhere: the candle it
    // may belong to is partial, and nothing is refetched in a loop for it.
    std::vector<std::pair<ObservedStamp, ClearingTick>> late{{Stamp(5, T0 + 50'001, ObservedMode::DELAYED), Tick(5, 1, 1)}};
    const auto flagged{flowmesh::ExtendObservedCandles(served, CandleInterval::M1, late, &incomplete)};
    BOOST_CHECK(!incomplete);
    BOOST_REQUIRE_EQUAL(flagged.size(), 1U);
    BOOST_CHECK(flagged.back().flags & flowmesh::CANDLE_PARTIAL);
    BOOST_CHECK_EQUAL(flagged.back().entries, served.back().entries);
}

BOOST_AUTO_TEST_SUITE_END()
