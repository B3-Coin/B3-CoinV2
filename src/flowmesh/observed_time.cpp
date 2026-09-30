// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/observed_time.h>

#include <consensus/amount.h>

#include <algorithm>
#include <limits>

namespace flowmesh {

size_t CandleMemoryWindow(const CandleInterval interval)
{
    switch (interval) {
    case CandleInterval::M1: return 1440;
    case CandleInterval::M5: return 864;
    case CandleInterval::M15: return 672;
    case CandleInterval::H1: return 720;
    case CandleInterval::H4: return 540;
    case CandleInterval::D1: return 500;
    }
    return 1440;
}

int64_t CandleOpenMs(const int64_t ms, const CandleInterval interval)
{
    if (ms <= 0) return 0;
    const int64_t step{CandleIntervalMs(interval)};
    return ms - ms % step;
}

std::vector<std::string> ObservedFlagNames(const uint8_t flags, const bool public_only)
{
    const uint8_t shown{public_only ? static_cast<uint8_t>(flags & OBSERVED_PUBLIC_FLAGS) : flags};
    std::vector<std::string> out;
    if (shown & OBSERVED_CLOCK_CLAMPED) out.emplace_back("clock_clamped");
    if (shown & OBSERVED_CLOCK_STEP) out.emplace_back("clock_step");
    if (shown & OBSERVED_CLOCK_SUSPECT) out.emplace_back("clock_suspect");
    if (shown & OBSERVED_RESTART) out.emplace_back("restart");
    if (shown & OBSERVED_NO_CLEARING) out.emplace_back("no_clearing_data");
    if (shown & OBSERVED_IMPLAUSIBLE) out.emplace_back("implausible");
    return out;
}

std::vector<std::string> ObservedCandleFlagNames(const uint8_t flags)
{
    std::vector<std::string> out;
    if (flags & CANDLE_PARTIAL) out.emplace_back("partial");
    if (flags & CANDLE_LATE) out.emplace_back("late");
    if (flags & CANDLE_SEAM_BEFORE) out.emplace_back("seam_before");
    if (flags & CANDLE_CLOCK) out.emplace_back("clock");
    return out;
}

ObservedDelay ClassifyObservedLiveness(const ObservedLivenessInputs& in) noexcept
{
    const uint64_t next{in.sequence == std::numeric_limits<uint64_t>::max() ? in.sequence : in.sequence + 1};
    if (in.from_catchup) return ObservedDelay::CATCHUP;
    if (in.deferred_watermark && in.sequence <= *in.deferred_watermark) return ObservedDelay::DEFERRED;
    // A certificate held for a later sequence: this entry was certified at
    // least a round before this node applied it.
    if (in.proven_target && *in.proven_target > next) return ObservedDelay::BEHIND;
    if (in.hint_next && *in.hint_next > next) return ObservedDelay::HINT;
    if (in.anchor_stale) return ObservedDelay::STALE_ANCHOR;
    if (!in.warm) return ObservedDelay::WARMUP;
    return ObservedDelay::NONE;
}

void ObservedRollup::Add(const ObservedStamp& stamp, const std::optional<ClearingTick>& tick, Changes* changed)
{
    if (m_last_sequence && stamp.sequence <= *m_last_sequence) return;
    const bool contiguous{m_last_sequence && stamp.sequence == *m_last_sequence + 1};
    // A bound from before a missing sequence bounds nothing after it.
    if (!contiguous) m_last_live_ms.reset();
    m_last_sequence = stamp.sequence;
    const bool priced{tick && tick->sequence == stamp.sequence && tick->microblock_hash == stamp.hash};
    const bool cleared{priced && tick->cleared && tick->volume > 0 && MoneyRange(tick->price) && MoneyRange(tick->volume)};
    const std::optional<int64_t> after{m_last_live_ms};
    for (const auto interval : CANDLE_INTERVALS) {
        if (!Tracked(interval)) continue;
        auto& series{m_series[Index(interval)]};
        const auto mark = [&](ObservedCandle& candle, const uint8_t flag) {
            if ((candle.flags & flag) == flag) return;
            candle.flags |= flag;
            if (changed) changed->emplace_back(interval, candle);
        };
        std::optional<int64_t> place;
        bool late{false};
        if (stamp.mode == ObservedMode::LIVE && stamp.ms > 0) {
            place = CandleOpenMs(stamp.ms, interval);
        } else if (stamp.mode == ObservedMode::DELAYED && stamp.ms > 0 && after && *after <= stamp.ms &&
                   CandleOpenMs(*after, interval) == CandleOpenMs(stamp.ms, interval)) {
            place = CandleOpenMs(stamp.ms, interval);
            late = true;
        }
        // Stamps are monotonic; a bucket behind the newest one is never
        // reopened, and such an entry is placed nowhere.
        if (place && !series.candles.empty() && *place < series.candles.back().open_ms) place.reset();
        if (!place) {
            if (after) {
                const int64_t low{CandleOpenMs(*after, interval)};
                for (auto it{series.candles.rbegin()}; it != series.candles.rend() && it->open_ms >= low; ++it) mark(*it, CANDLE_PARTIAL);
            }
            if (stamp.mode == ObservedMode::DELAYED && stamp.ms > 0) {
                const int64_t high{CandleOpenMs(stamp.ms, interval)};
                if (!series.candles.empty() && series.candles.back().open_ms == high) {
                    mark(series.candles.back(), CANDLE_PARTIAL);
                } else if (series.candles.empty() || series.candles.back().open_ms < high) {
                    series.pending_partial_open = high;
                }
            } else {
                // Its time is unknown beyond "before the next stamp".
                series.partial_next = true;
            }
            continue;
        }
        if (series.candles.empty() || series.candles.back().open_ms != *place) {
            ObservedCandle fresh;
            fresh.open_ms = *place;
            fresh.first_sequence = stamp.sequence;
            if (stamp.flags & (OBSERVED_CLOCK_STEP | OBSERVED_RESTART)) fresh.flags |= CANDLE_SEAM_BEFORE;
            if (series.partial_next || (series.pending_partial_open && *series.pending_partial_open == *place)) {
                fresh.flags |= CANDLE_PARTIAL;
            }
            series.partial_next = false;
            if (series.pending_partial_open && *series.pending_partial_open <= *place) series.pending_partial_open.reset();
            series.candles.push_back(fresh);
            const int64_t window{static_cast<int64_t>(CandleMemoryWindow(interval)) * CandleIntervalMs(interval)};
            while (series.candles.front().open_ms < series.candles.back().open_ms - window) series.candles.pop_front();
        }
        auto& candle{series.candles.back()};
        candle.last_sequence = stamp.sequence;
        if (candle.entries < std::numeric_limits<uint32_t>::max()) ++candle.entries;
        if (late) candle.flags |= CANDLE_LATE;
        if (stamp.flags & (OBSERVED_CLOCK_CLAMPED | OBSERVED_CLOCK_STEP | OBSERVED_CLOCK_SUSPECT)) candle.flags |= CANDLE_CLOCK;
        if (cleared) {
            if (candle.clearings == 0) {
                candle.open = candle.high = candle.low = candle.close = tick->price;
            } else {
                candle.high = std::max(candle.high, tick->price);
                candle.low = std::min(candle.low, tick->price);
                candle.close = tick->price;
            }
            if (candle.clearings < std::numeric_limits<uint32_t>::max()) ++candle.clearings;
            candle.base_volume += static_cast<uint64_t>(tick->volume);
            // Exact: the quote of a clearing is price x volume (clearing.h).
            candle.quote_volume += util::Unsigned128{static_cast<uint64_t>(tick->price)} * static_cast<uint64_t>(tick->volume);
        }
        if (changed) changed->emplace_back(interval, candle);
    }
    if (stamp.mode == ObservedMode::LIVE && stamp.ms > 0) m_last_live_ms = stamp.ms;
}

void ObservedRollup::Restore(const CandleInterval interval, std::vector<ObservedCandle> candles)
{
    if (!Tracked(interval)) return;
    auto& series{m_series[Index(interval)]};
    series = {};
    std::sort(candles.begin(), candles.end(), [](const auto& a, const auto& b) { return a.open_ms < b.open_ms; });
    for (auto& candle : candles) {
        if (!series.candles.empty() && series.candles.back().open_ms == candle.open_ms) continue;
        series.candles.push_back(std::move(candle));
    }
    if (series.candles.empty()) return;
    const int64_t window{static_cast<int64_t>(CandleMemoryWindow(interval)) * CandleIntervalMs(interval)};
    while (series.candles.front().open_ms < series.candles.back().open_ms - window) series.candles.pop_front();
}

void ObservedRollup::RestoreLast(const std::optional<uint64_t> last_sequence, const std::optional<int64_t> last_live_ms)
{
    m_last_sequence = last_sequence;
    m_last_live_ms = last_sequence ? last_live_ms : std::nullopt;
}

ObservedCandlePage ObservedRollup::Page(const CandleInterval interval, const std::optional<int64_t> before_ms, size_t limit) const
{
    ObservedCandlePage out;
    out.available = true;
    out.interval = interval;
    limit = std::clamp<size_t>(limit, 1, OBSERVED_CANDLE_PAGE_MAX);
    const auto& candles{Candles(interval)};
    const int64_t step{CandleIntervalMs(interval)};
    int64_t before{before_ms ? std::max<int64_t>(0, *before_ms)
                             : candles.empty() ? 0 : candles.back().open_ms + step};
    const int64_t span{static_cast<int64_t>(limit) * step};
    out.before_ms = before;
    out.from_ms = before > span ? before - span : 0;
    for (const auto& candle : candles) {
        if (candle.open_ms >= out.from_ms && candle.open_ms < out.before_ms) out.candles.push_back(candle);
    }
    out.more = !candles.empty() && candles.front().open_ms < out.from_ms;
    return out;
}

const std::deque<ObservedCandle>& ObservedRollup::Candles(const CandleInterval interval) const
{
    return m_series[Index(interval)].candles;
}

std::vector<ObservedCandle> ExtendObservedCandles(std::vector<ObservedCandle> candles, const CandleInterval interval,
                                                  const std::vector<std::pair<ObservedStamp, ClearingTick>>& newer,
                                                  bool* incomplete)
{
    if (incomplete) *incomplete = false;
    if (candles.empty() || newer.empty()) return candles;
    const uint64_t served_last{candles.back().last_sequence};
    ObservedRollup rollup{interval};
    rollup.Restore(interval, {candles.back()});
    rollup.RestoreLast(served_last, std::nullopt);
    bool first{true};
    for (const auto& [stamp, tick] : newer) {
        if (stamp.sequence <= served_last) continue;
        if (first && stamp.sequence != served_last + 1 && incomplete) *incomplete = true;
        first = false;
        rollup.Add(stamp, tick);
    }
    candles.pop_back();
    const auto& extended{rollup.Candles(interval)};
    candles.insert(candles.end(), extended.begin(), extended.end());
    return candles;
}

} // namespace flowmesh
