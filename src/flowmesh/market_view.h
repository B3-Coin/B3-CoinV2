// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_MARKET_VIEW_H
#define B3COIN_FLOWMESH_MARKET_VIEW_H

#include <flowmesh/clearing.h>
#include <flowmesh/market_data.h>

#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <utility>

namespace flowmesh {

class FlowMeshState;

/** Display derivations of verified FlowMesh state.
 *
 * Nothing here is read by execution, admission, signing, a state root, a
 * commitment or any certified object. Every function takes verified state
 * (or a copy of it) through public const methods only and never mutates it. */

/** The side a clearing left unfilled, observed on `post`, the state right
 * after the execution that produced `clearing`.
 *
 * Exact for that state: Allocate() lists every curve with positive effective
 * quantity at the price on both sides (the long side possibly with a zero
 * fill), and the clearing is the last book mutation of an ordinary execution.
 * So after it the short side has no quantity left at the price and the long
 * side's residual sums to the committed imbalance. Any other observation is
 * UNKNOWN, never a guess. NONE for no clearing or an exact match. */
ImbalanceSide ObserveImbalanceSide(const ClearingEngine::ClearingResult& clearing, const FlowMeshState& post);

/** A clearing-tick page of `history` (ascending sequences, as the bounded
 * market-history cache holds them). Without after_sequence: the newest
 * `limit` ticks; with it: the first `limit` ticks after it, and `gap` when
 * the tick right after it was already evicted. Unavailable for a limit
 * outside 1..MARKET_DATA_MAX_TICKS. */
ClearingTickPage PageClearingTicks(const std::deque<MarketHistoryEntry>& history, const ClearingTickQuery& query);
//! The same page of ticks already held, consecutive and oldest first.
ClearingTickPage PageClearingTicks(const std::deque<ClearingTick>& ticks, const ClearingTickQuery& query);

//! Work one indicative computation may spend: a unit per breakpoint read and
//! per curve evaluation. Beyond it the view is unavailable, never truncated.
inline constexpr size_t MARKET_VIEW_WORK_BUDGET{size_t{1} << 20};
//! Most standing curves a display view copies: a larger book is not copied
//! and its views are unavailable ("work_budget"), as the computation would
//! exhaust MARKET_VIEW_WORK_BUDGET on such a book in any case. It bounds the
//! copy's time and memory (a few MiB) wherever it runs.
inline constexpr size_t MARKET_VIEW_MAX_BOOK_CURVES{size_t{1} << 14};

/** Exactly ClearSlot's price selection (maximum volume, then minimum
 * imbalance, then lowest price over every breakpoint price) and
 * Allocate's rationing, over `curves` as the book holds them, with the
 * draft replacing its account's same-side curve. Never touches a state and
 * fails closed (available=false) over `budget` or on a malformed curve. */
IndicativeClearing ComputeIndicative(std::span<const ClearingEngine::CurveView> curves, const IndicativeQuery& query,
                                     size_t budget = MARKET_VIEW_WORK_BUDGET);

//! Work one book-levels computation may spend over both sides: a unit per
//! breakpoint read and per curve evaluation. Its first pass reads every
//! curve's best level, so a book whose first pass exceeds it has no levels
//! ("work_budget"); after that the levels stop early, as an exact prefix.
inline constexpr size_t MARKET_VIEW_LEVELS_WORK_BUDGET{size_t{1} << 20};
//! Distinct prices of one curve a grouped level reads for its exact B3
//! notional; beyond it the level has no notional (its size stays exact).
inline constexpr size_t MARKET_VIEW_LEVEL_NOTIONAL_PRICES{16};

/** Why `grouping` cannot be computed, or empty. Canonical: the display step
 * h = m x 10^e B3 per token is g = m x 10^(e + 9 - d) canonical ticks (a
 * canonical price P is P x 10^(d - 9) B3 per token), which must be a whole
 * number of ticks from 1 to MAX_MONEY. Inverse: u(P) = 10^s / (m x P) with
 * s = 9 - d - e, which must lie in 0..36. m is 1, 2 or 5 and d is 0..18. */
std::string LevelGroupingError(const LevelGrouping& grouping);
//! A valid levels query: depth 1..MARKET_DATA_MAX_LEVELS and a valid grouping.
bool LevelsQueryValid(const BookLevelsQuery& query);

/** The inclusive canonical price range of bucket `index` of a side (canonical
 * bids when `bid`) in a valid grouping, exactly as ComputeBookLevels reports
 * it (an unbounded end is MAX_MONEY), or nullopt when no canonical price
 * falls in that bucket. `zero_price` is the inverse view's price-zero
 * bucket. For a client to check a reported level against its bucket and to
 * label it; display only. */
std::optional<std::pair<CAmount, CAmount>> LevelBucketRange(const LevelGrouping& grouping, bool bid, util::Unsigned128 index,
                                                            bool zero_price);

/** The top `query.depth` price levels per side of `curves` as the book holds
 * them, best first, in the query's grouping; the standing clearing (no
 * draft) and the exact best prices. Every level is exact: a bid bucket
 * [lo, hi] holds R(lo) - R(hi + 1) of each curve and an ask bucket R(hi) -
 * R(lo - 1) (R(-1) = 0), where R(p) = max(0, evaluate(p) - filled) is the
 * curve's remaining quantity at p; cumulative is the running sum from the
 * best level, equal to the side's total demand at lo (bids) or supply at hi
 * (asks). A level is emitted only after every curve's part of it is summed.
 * Within `budget` (MARKET_VIEW_LEVELS_WORK_BUDGET) the whole view is
 * unavailable when the first pass does not fit, and otherwise stops early
 * as an exact prefix; a level or cumulative size over MAX_MONEY stops its
 * side ("overflow"), never saturated. Never touches a state; head is left
 * to the caller. */
BookLevels ComputeBookLevels(std::span<const ClearingEngine::CurveView> curves, const BookLevelsQuery& query,
                             size_t budget = MARKET_VIEW_LEVELS_WORK_BUDGET);

//! One page of at most `limit` standing curves after `after`, exactly as
//! ClearingEngine::ReadCurves returns it at the head the caller pins, or
//! nullopt when that head is no longer current.
using BookPageReader = std::function<std::optional<ClearingEngine::CurvePage>(
    const std::optional<ClearingEngine::CurveKey>& after, size_t limit)>;

/** The standing curves of `head` for display views, read through `read` one
 * page of at most MARKET_DATA_MAX_CURVES curves per call, so a caller can
 * hold its lock for one page at a time instead of copying the whole book in
 * one hold. Nothing more is copied for a book over `max_curves` (unavailable
 * "work_budget", after its first page) or once `read` reports the head moved
 * ("head_changed"). `pages` counts the calls to `read`. */
BookCurves CopyBookCurves(const uint256& head, const BookPageReader& read, size_t& pages,
                          size_t max_curves = MARKET_VIEW_MAX_BOOK_CURVES);

//! The query asks for book levels and this reply carries them: its head
//! changed, or the query asked for them also for an unchanged head. The
//! caller copies the curves (data.book) only then or for the indicative.
bool LevelsWanted(const MarketData& data, const MarketDataQuery& query);

/** Fills data.indicative and data.levels from data.book when the query asks
 * for them (levels only when LevelsWanted), and drops data.book. A book that
 * was not copied (unavailable_reason) makes the views unavailable for that
 * reason. Levels are computed once per copy of the curves and query, and
 * reused while the caller keeps that copy (the head's). For callers that
 * hold no node lock: the local engine's reads and the remote client after
 * it releases its work gate. */
void ComputeMarketViews(MarketData& data, const MarketDataQuery& query);

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_MARKET_VIEW_H
