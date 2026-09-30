// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_OBSERVED_TIME_H
#define B3COIN_FLOWMESH_OBSERVED_TIME_H

#include <flowmesh/market_data.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace flowmesh {

/** Operator-observed time: classification and UTC candle rollups.
 *
 * Display only (doc/design/flowmesh-chart-time.md). Nothing here is read by
 * signing, admission, execution, verification, a state root, a commitment or
 * any certified object, and nothing here reads a clock: the caller supplies
 * every time. The node and the Qt client bucket with this same code. */

//! A backward wall-clock step up to this is held at the previous stamp
//! (flagged); a larger one records entries without a time until the clock
//! passes the previous stamp again. Also the wall/steady disagreement that
//! flags a clock step.
inline constexpr int64_t OBSERVED_CLOCK_SLEW_MS{1000};

//! The newest candles each interval keeps in memory, by time: 1m one day,
//! 5m three days, 15m one week, 1h 30 days, 4h 90 days, 1d 500 days. Public
//! reads are served from this window only; every window holds at least the
//! largest public page.
size_t CandleMemoryWindow(CandleInterval interval);
inline constexpr size_t OBSERVED_CANDLE_PAGE_MAX{500};

//! floor(ms / interval) * interval for ms >= 0; zero for a negative time.
int64_t CandleOpenMs(int64_t ms, CandleInterval interval);

//! Names of the set stamp flags; `public_only` keeps OBSERVED_PUBLIC_FLAGS.
std::vector<std::string> ObservedFlagNames(uint8_t flags, bool public_only);
std::vector<std::string> ObservedCandleFlagNames(uint8_t flags);

/** What decides whether a committed entry was applied as it was certified. */
struct ObservedLivenessInputs {
    uint64_t sequence{0};
    //! The entry came from a catch-up page.
    bool from_catchup{false};
    //! The highest sequence of a certificate, proposal or attestation that
    //! waited for B3 reconciliation (deferred), if any.
    std::optional<uint64_t> deferred_watermark;
    //! One past the highest BLS-verified certificate held for a sequence not
    //! applied yet (FlowMeshLagSummary::proven_target).
    std::optional<uint64_t> proven_target;
    //! The highest next sequence an (unverified) peer hint announced.
    std::optional<uint64_t> hint_next;
    //! The entry's anchor is more than a small margin behind the current
    //! acceptable anchor: it was proposed well before now.
    bool anchor_stale{false};
    //! The node has been caught up for at least the warm-up (ObservedWarmup).
    bool warm{false};
};

/** NONE (LIVE) only when nothing says the entry is late. A peer holding
 * exactly the entry being committed (announcing sequence + 1) is level with
 * this node, not ahead of it. */
ObservedDelay ClassifyObservedLiveness(const ObservedLivenessInputs& inputs) noexcept;

/** Since when the market has been caught up, continuously. Observed by the
 * runtime's maintenance tick; restarts after an interruption longer than
 * the tolerance (so a proven lag that resolves within a round, or a brief B3
 * reconciliation, does not turn the next entries into a gap). */
class ObservedWarmup
{
public:
    using Clock = std::chrono::steady_clock;
    void Observe(bool caught_up, Clock::time_point now,
                 std::chrono::milliseconds tolerance = std::chrono::milliseconds{0}) noexcept
    {
        if (caught_up) {
            m_interrupted.reset();
            if (!m_since) m_since = now;
            return;
        }
        if (!m_interrupted) m_interrupted = now;
        if (now - *m_interrupted >= tolerance) m_since.reset();
    }
    bool Warm(Clock::time_point now, std::chrono::milliseconds warmup) const noexcept
    {
        return m_since && now - *m_since >= warmup;
    }

private:
    std::optional<Clock::time_point> m_since, m_interrupted;
};

//! max(2 s, 2 x round_timeout): an old certificate relayed through the live
//! path right after a restart or reconnect must not be stamped LIVE.
inline std::chrono::milliseconds ObservedWarmupFor(const std::chrono::milliseconds round_timeout)
{
    return std::max(std::chrono::milliseconds{2000}, 2 * round_timeout);
}

/** UTC candles built from stamps in sequence order.
 *
 * A LIVE entry goes into the bucket of its time. A DELAYED entry goes into a
 * bucket only when its whole bound, from about the last LIVE time before it
 * to when it was applied, lies inside that bucket (CANDLE_LATE); otherwise,
 * like an UNTIMED entry, it is placed nowhere and every held candle its bound
 * overlaps (and the candle at its upper bound, when it forms) is marked
 * CANDLE_PARTIAL. A candle whose first entry carries a clock step or a
 * restart gets CANDLE_SEAM_BEFORE. Nothing is interpolated: empty buckets
 * are never created. */
class ObservedRollup
{
public:
    //! `only`: keep that one interval (a client extending one page).
    explicit ObservedRollup(std::optional<CandleInterval> only = std::nullopt) : m_only{only} {}

    using Changes = std::vector<std::pair<CandleInterval, ObservedCandle>>;
    /** Adds the next stamp; a sequence at or before the last one added is
     * ignored. `tick` must be that entry's own clearing (same sequence and
     * hash), else the entry counts without a price. `changed`, when given,
     * receives every candle this call created or changed. */
    void Add(const ObservedStamp& stamp, const std::optional<ClearingTick>& tick, Changes* changed = nullptr);

    //! Replaces one interval's held candles (ascending opens), trimmed to
    //! its memory window.
    void Restore(CandleInterval interval, std::vector<ObservedCandle> candles);
    //! The continuity reloaded with them: the last stamp's sequence and the
    //! last LIVE time of a contiguous run ending there.
    void RestoreLast(std::optional<uint64_t> last_sequence, std::optional<int64_t> last_live_ms);

    /** The held candles with open in [before - limit x interval, before),
     * before = `before_ms` or just past the newest candle. `more` when
     * older candles are held. `limit` is clamped to 1..OBSERVED_CANDLE_PAGE_MAX. */
    ObservedCandlePage Page(CandleInterval interval, std::optional<int64_t> before_ms, size_t limit) const;
    const std::deque<ObservedCandle>& Candles(CandleInterval interval) const;
    std::optional<uint64_t> LastSequence() const { return m_last_sequence; }
    std::optional<int64_t> LastLiveMs() const { return m_last_live_ms; }

private:
    struct Series {
        std::deque<ObservedCandle> candles;
        //! The open of a bucket an unplaced DELAYED entry's upper bound fell
        //! in, before that bucket had a candle.
        std::optional<int64_t> pending_partial_open;
        //! An UNTIMED entry happened before the next candle formed.
        bool partial_next{false};
    };
    bool Tracked(CandleInterval interval) const { return !m_only || *m_only == interval; }
    static size_t Index(CandleInterval interval) { return static_cast<size_t>(interval); }

    std::optional<CandleInterval> m_only;
    std::array<Series, CANDLE_INTERVALS.size()> m_series;
    std::optional<uint64_t> m_last_sequence;
    std::optional<int64_t> m_last_live_ms;
};

/** A client's live bars: `candles` (one interval's page, oldest first, as a
 * node served it) extended with stamped ticks after its newest candle's last
 * sequence, bucketed exactly as the node buckets them. `newer` pairs each
 * stamp with its entry's tick, ascending. The newest served candle is
 * continued, never recomputed: an entry the page did not count is added to
 * it only when it follows that candle's last sequence. Returns the extended
 * page; `incomplete` is set when the first stamp does not continue the page
 * (the held ticks no longer reach it), so the caller should refetch. A late
 * or untimed stamp is bucketed as the node does (placed only inside its
 * bound, else the candles it may belong to are marked partial); the node's
 * own page, refetched later, may place it more precisely. */
std::vector<ObservedCandle> ExtendObservedCandles(std::vector<ObservedCandle> candles, CandleInterval interval,
                                                  const std::vector<std::pair<ObservedStamp, ClearingTick>>& newer,
                                                  bool* incomplete = nullptr);

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_OBSERVED_TIME_H
