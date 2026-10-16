// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_ENGINE_RESIDUAL_H
#define B3_FLOWMESH2_ENGINE_RESIDUAL_H

#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/engine/book.h>
#include <flowmesh2/engine/matcher.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/** The residual cross of same-slot takers (slice R6; ARCHITECTURE.md
 * section 11; D4, O-5). PROVISIONAL REGTEST.
 *
 * After SweepA2, a BUY and a SELL taker of the same market and slot can both
 * keep GTC lots whose limits still cross each other (the BUY limit is at or
 * above the SELL limit): neither could reach the other during the sweep,
 * because takers only match resting makers. Plan section 3.5 matches them
 * in one max-volume uniform-price call before they rest; its tie-break is
 * part of the D4 ruling and is not settled (BLOCKED-ON-RULING).
 *
 * The outline therefore only DETECTS the residual cross (FindResidualCross,
 * real), and the engine (R7) gives each crossing taker the certified typed
 * refusal REJ_RESIDUAL_CROSS_UNSUPPORTED for its remainder and releases the
 * reservation. ResolveResidualCross, the call itself, is a stub (M3-ENG-02).
 */
namespace flowmesh2::engine {

//! Indices (into the spans given to FindResidualCross) of the remainders that cross.
struct ResidualCross {
    std::vector<size_t> buys;  //!< ascending
    std::vector<size_t> sells; //!< ascending
    bool Empty() const { return buys.empty() && sells.empty(); }
    friend bool operator==(const ResidualCross&, const ResidualCross&) = default;
};

/** The GTC remainders that would rest crossed. `buys` and `sells` are the
 * remainders R7 would rest (one market, lots left). A BUY crosses iff its
 * limit is at or above the lowest SELL limit; a SELL crosses iff its limit
 * is at or below the highest BUY limit. INVALID_INPUT if an entry of `buys`
 * is not a BUY, an entry of `sells` is not a SELL, or an entry has no lots
 * left.
 */
Result<ResidualCross> FindResidualCross(std::span<const Taker> buys, std::span<const Taker> sells);

/** The outcome of one residual call: a clearing price and the lots each
 * remainder executes, aligned with the spans given to ResolveResidualCross.
 * Target shape only; M3-ENG-02 may reshape it.
 */
struct ResidualCall {
    uint64_t clearing_price_ticks{0};
    std::vector<uint64_t> buy_lots;
    std::vector<uint64_t> sell_lots;
    friend bool operator==(const ResidualCall&, const ResidualCall&) = default;
};

/** The same-slot max-volume uniform-price call over the crossing GTC
 * remainders (plan section 3.5). Stub: NotImplemented(M3-ENG-02), waiting
 * for the D4 tie-break ruling; the outline answers each crossing remainder
 * with the certified refusal instead (R7).
 */
// TODO(rebirth:M3-ENG-02) residual same-slot max-volume uniform-price call.
Result<ResidualCall> ResolveResidualCross(OrderBook& book, std::span<const Taker> buys, std::span<const Taker> sells,
                                          const ProfileParams& profile);

} // namespace flowmesh2::engine

#endif // B3_FLOWMESH2_ENGINE_RESIDUAL_H
