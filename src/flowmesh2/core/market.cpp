// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/market.h>

#include <flowmesh2/core/hashes.h>
#include <util/int128.h>

namespace flowmesh2 {
namespace {

constexpr uint64_t MAX_ATOMS_U64{static_cast<uint64_t>(MAX_ATOMS)};

Error AboveMaxAtoms() { return Error{ErrorCode::ARITHMETIC_OVERFLOW, "market amount above 2^62"}; }

} // namespace

MarketId MarketDescriptor::Id() const { return MarketId{TaggedObject(HashTag::MARKET, *this)}; }

Result<void> ValidateMarket(const MarketDescriptor& market)
{
    if (market.base == market.quote) return Error{ErrorCode::INVALID_INPUT, "market base equals quote"};
    if (market.base_atoms_per_lot == 0 || market.base_atoms_per_lot > MAX_ATOMS_U64) {
        return Error{ErrorCode::INVALID_INPUT, "base_atoms_per_lot outside [1, 2^62]"};
    }
    if (market.quote_atoms_per_tick == 0 || market.quote_atoms_per_tick > MAX_ATOMS_U64) {
        return Error{ErrorCode::INVALID_INPUT, "quote_atoms_per_tick outside [1, 2^62]"};
    }
    return Ok();
}

Result<void> ValidateMarketForProfile(const MarketDescriptor& market, const ProfileParams& profile)
{
    if (const Result<void> valid{Validate(profile)}; !valid.has_value()) return valid.error();
    if (const Result<void> valid{ValidateMarket(market)}; !valid.has_value()) return valid.error();
    if (profile.stable_fee_asset.has_value() && market.quote != *profile.stable_fee_asset) {
        return Error{ErrorCode::INVALID_INPUT, STABLE_QUOTE_REQUIRED};
    }
    return Ok();
}

Result<Atoms> BaseAtoms(const MarketDescriptor& market, uint64_t lots)
{
    if (auto valid{ValidateMarket(market)}; !valid) return valid.error();
    const util::Unsigned128 atoms{util::Unsigned128{lots} * util::Unsigned128{market.base_atoms_per_lot}};
    if (atoms > util::Unsigned128{MAX_ATOMS_U64}) return AboveMaxAtoms();
    return Ok(static_cast<Atoms>(atoms));
}

Result<Atoms> QuoteAtoms(const MarketDescriptor& market, uint64_t lots, uint64_t price_ticks)
{
    if (auto valid{ValidateMarket(market)}; !valid) return valid.error();
    // lots * price fits in 128 bits; once it is above 2^62 the result is too,
    // because quote_atoms_per_tick >= 1. Below that, the second product is
    // below 2^62 * 2^62 and fits as well.
    const util::Unsigned128 notional_ticks{util::Unsigned128{lots} * util::Unsigned128{price_ticks}};
    if (notional_ticks > util::Unsigned128{MAX_ATOMS_U64}) return AboveMaxAtoms();
    const util::Unsigned128 atoms{notional_ticks * util::Unsigned128{market.quote_atoms_per_tick}};
    if (atoms > util::Unsigned128{MAX_ATOMS_U64}) return AboveMaxAtoms();
    return Ok(static_cast<Atoms>(atoms));
}

} // namespace flowmesh2
