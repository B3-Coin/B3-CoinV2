// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_MARKET_VIEW_H
#define B3COIN_FLOWMESH_MARKET_VIEW_H

#include <flowmesh/clearing.h>
#include <flowmesh/market_data.h>

#include <deque>
#include <span>

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

/** Exactly ClearSlot's price selection (maximum volume, then minimum
 * imbalance, then lowest price over every breakpoint price) and
 * Allocate's rationing, over `curves` as the book holds them, with the
 * draft replacing its account's same-side curve. Never touches a state and
 * fails closed (available=false) over `budget` or on a malformed curve. */
IndicativeClearing ComputeIndicative(std::span<const ClearingEngine::CurveView> curves, const IndicativeQuery& query,
                                     size_t budget = MARKET_VIEW_WORK_BUDGET);

/** Fills data.indicative from data.book when the query asks for it, and
 * drops data.book. For callers that hold no node lock: the local engine's
 * reads and the remote client after it releases its work gate. */
void ComputeMarketViews(MarketData& data, const MarketDataQuery& query);

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_MARKET_VIEW_H
