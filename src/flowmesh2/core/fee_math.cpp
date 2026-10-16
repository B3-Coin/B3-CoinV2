// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/fee_math.h>

namespace flowmesh2 {

Result<Atoms> SellerFeeTotal(Atoms cum_quote, uint32_t fee_ppm)
{
    if (!InAtomRange(cum_quote)) return Error{ErrorCode::INVALID_INPUT, "cumulative quote outside [0, MAX_ATOMS]"};
    if (fee_ppm > FEE_PPM_DENOMINATOR) return Error{ErrorCode::INVALID_INPUT, "fee_ppm above one million"};
    const auto fee{MulDivCeil(static_cast<uint64_t>(cum_quote), fee_ppm, FEE_PPM_DENOMINATOR)};
    if (!fee.has_value()) return fee.error();
    // fee <= cum_quote <= MAX_ATOMS because fee_ppm <= 1,000,000.
    return Ok(static_cast<Atoms>(*fee));
}

Result<Atoms> SellerFeeIncrement(Atoms cum_quote_before, Atoms cum_quote_after, uint32_t fee_ppm)
{
    if (!InAtomRange(cum_quote_before) || !InAtomRange(cum_quote_after)) {
        return Error{ErrorCode::INVALID_INPUT, "cumulative quote outside [0, MAX_ATOMS]"};
    }
    if (cum_quote_before > cum_quote_after) {
        return Error{ErrorCode::INVALID_INPUT, "cumulative quote decreased"};
    }
    const auto before{SellerFeeTotal(cum_quote_before, fee_ppm)};
    if (!before.has_value()) return before.error();
    const auto after{SellerFeeTotal(cum_quote_after, fee_ppm)};
    if (!after.has_value()) return after.error();
    return SubAtoms(*after, *before);
}

Result<FeeSplit> SplitSlotFees(Atoms total, uint32_t seat_share_percent)
{
    if (!InAtomRange(total)) return Error{ErrorCode::INVALID_INPUT, "slot fees outside [0, MAX_ATOMS]"};
    if (seat_share_percent > 100) return Error{ErrorCode::INVALID_INPUT, "seat share above 100 percent"};
    const auto seat{MulDivFloor(static_cast<uint64_t>(total), seat_share_percent, 100)};
    if (!seat.has_value()) return seat.error();
    const Atoms seat_pool{static_cast<Atoms>(*seat)};
    const auto treasury{SubAtoms(total, seat_pool)};
    if (!treasury.has_value()) return treasury.error();
    return Ok(FeeSplit{seat_pool, *treasury});
}

} // namespace flowmesh2
