// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_MARKET_VIEW_H
#define B3COIN_FLOWMESH_MARKET_VIEW_H

#include <flowmesh/clearing.h>
#include <flowmesh/market_data.h>

#include <deque>
#include <optional>
#include <span>
#include <string>

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

//! Work one indicative computation may spend: a unit per breakpoint read and
//! per curve evaluation. Beyond it the view is unavailable, never truncated.
inline constexpr size_t MARKET_VIEW_WORK_BUDGET{size_t{1} << 20};

/** A prospective limit order exactly as the wallet would build it
 * (MakeLimitBidCurve / MakeLimitAskCurve). Submitted, it would replace the
 * signing account's standing curve on the same side. */
struct IndicativeDraft {
    ClearingEngine::Side side{ClearingEngine::Side::BID};
    CAmount price{0};
    CAmount quantity{0};
    //! The account that would sign it. Absent (no account yet): a new curve,
    //! and the draft's fill under rationing is reported as bounds.
    std::optional<AccountId> account;
    bool operator==(const IndicativeDraft&) const = default;
};

struct IndicativeQuery {
    std::optional<IndicativeDraft> draft;
};

/** What the next clearing would be if the verified standing curves (plus
 * the draft, if any) were cleared as they are. NOT certified: every action
 * not yet in the verified state is excluded, and the draft is assumed to be
 * admitted (its balance and reservation are not checked). */
struct IndicativeClearing {
    bool available{false};
    //! "work_budget", "malformed_curve", "invalid_draft" or "overflow".
    std::string unavailable_reason;
    //! Certified head of the curves, set by the caller.
    uint256 head;
    std::optional<IndicativeDraft> draft;
    //! The draft replaces its account's standing curve on that side.
    bool draft_replaces_curve{false};
    bool cleared{false};
    CAmount price{0};
    CAmount volume{0};
    //! |demand - supply| at the price, saturated at MAX_MONEY as committed.
    CAmount imbalance{0};
    ImbalanceSide imbalance_side{ImbalanceSide::NONE};
    //! Highest price with standing bid quantity and lowest with standing ask
    //! quantity, over every verified curve (never the draft).
    std::optional<CAmount> best_bid;
    std::optional<CAmount> best_ask;
    //! The draft's fill at the price: exact largest-remainder rationing when
    //! its account is known (min == max), otherwise its bounds.
    std::optional<CAmount> draft_fill_min;
    std::optional<CAmount> draft_fill_max;
};

/** Exactly ClearSlot's price selection (maximum volume, then minimum
 * imbalance, then lowest price over every breakpoint price) and
 * Allocate's rationing, over `curves` as the book holds them, with the
 * draft replacing its account's same-side curve. Never touches a state and
 * fails closed (available=false) over `budget` or on a malformed curve. */
IndicativeClearing ComputeIndicative(std::span<const ClearingEngine::CurveView> curves, const IndicativeQuery& query,
                                     size_t budget = MARKET_VIEW_WORK_BUDGET);

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_MARKET_VIEW_H
