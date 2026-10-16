// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_FEE_MATH_H
#define B3_FLOWMESH2_CORE_FEE_MATH_H

#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/result.h>

#include <cstdint>

/** D5 fee arithmetic: the shared spec functions (slice R4; ARCHITECTURE.md
 * section 11, O-6).
 *
 * PROVISIONAL REGTEST (D5; gate G-2: the D3 reconciliation "stable quote and
 * fees in Q" has not landed). Used by the engine (R6/R7) and by b3verify's
 * VerifyFill (R11), so both compute fees with one definition.
 *
 * Seller incidence (profile SELLER): the seller pays fee_ppm of the matched
 * quote notional, once, in the quote asset, rounded up on the CUMULATIVE
 * notional of the order. With q the order's cumulative matched quote:
 *   SellerFeeTotal(q)            = ceil(q * fee_ppm / 1,000,000)
 *   SellerFeeIncrement(q0, q1)   = SellerFeeTotal(q1) - SellerFeeTotal(q0)
 * so however an order is split into fills, its fees sum to the fee of the
 * total: rounding happens once per order, not once per fill.
 *
 * Slot split: of the total fees F collected in one quote asset in a slot,
 * the seat pool gets floor(F * seat_share_percent / 100) and the treasury
 * the residual (the dust goes to the treasury; the D6 dust rule is OPEN).
 *
 * Every function is total over its domain and deterministic; products use a
 * 128-bit intermediate (core/amount.h). An operand outside [0, MAX_ATOMS],
 * q0 > q1, fee_ppm above 1,000,000 or a share above 100 % is INVALID_INPUT.
 */
namespace flowmesh2 {

// TODO(rebirth:M3-ENG-08) PER_SIDE incidence and the D6 dust vectors extend these functions.

inline constexpr uint64_t FEE_PPM_DENOMINATOR{1'000'000};

//! ceil(cum_quote * fee_ppm / 1,000,000).
Result<Atoms> SellerFeeTotal(Atoms cum_quote, uint32_t fee_ppm);

//! SellerFeeTotal(cum_quote_after) - SellerFeeTotal(cum_quote_before).
Result<Atoms> SellerFeeIncrement(Atoms cum_quote_before, Atoms cum_quote_after, uint32_t fee_ppm);

struct FeeSplit {
    Atoms seat_pool{0};
    Atoms treasury{0};
    friend bool operator==(const FeeSplit&, const FeeSplit&) = default;
};

//! seat_pool = floor(total * seat_share_percent / 100), treasury = total - seat_pool.
Result<FeeSplit> SplitSlotFees(Atoms total, uint32_t seat_share_percent);

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_FEE_MATH_H
