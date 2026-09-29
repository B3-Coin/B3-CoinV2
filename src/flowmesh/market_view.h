// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_MARKET_VIEW_H
#define B3COIN_FLOWMESH_MARKET_VIEW_H

#include <flowmesh/clearing.h>
#include <flowmesh/market_data.h>

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

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_MARKET_VIEW_H
