// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_CONSENSUS_FLOWMESH_PARAMS_H
#define B3COIN_CONSENSUS_FLOWMESH_PARAMS_H

namespace Consensus {

/**
 * The same 30-block depth governs the FN-seat pre-binding runway, FlowMesh
 * anchor recognition, and certified withdrawal maturity. Kept in consensus
 * rather than node code because the A2/A3 schedule itself depends on it.
 */
inline constexpr int FLOWMESH_ANCHOR_DEPTH{30};

} // namespace Consensus

#endif // B3COIN_CONSENSUS_FLOWMESH_PARAMS_H
