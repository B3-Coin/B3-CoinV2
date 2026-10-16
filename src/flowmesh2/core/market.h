// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_MARKET_H
#define B3_FLOWMESH2_CORE_MARKET_H

#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/result.h>
#include <serialize.h>

#include <cstdint>

/** Spot market descriptor and its integer money conversions (slice R3).
 *
 * Prices are integer ticks and quantities integer lots (plan section 3.5).
 * A market trades `base` against `quote`:
 *   BaseAtoms(lots)         = lots * base_atoms_per_lot
 *   QuoteAtoms(lots, price) = lots * price_ticks * quote_atoms_per_tick
 * both exact, refused (ARITHMETIC_OVERFLOW) above MAX_ATOMS, never rounded.
 *
 * MarketId = Tagged(MARKET, base || quote || u64 LE base_atoms_per_lot ||
 * u64 LE quote_atoms_per_tick), the Bitcoin serialization below.
 *
 * PROVISIONAL (D6): tick and lot are fixed per market at genesis and there is
 * no minimum order size. Per-market tick, lot and minimum size at listing are
 * task M3-ENG-11, which replaces this descriptor (REAL-REPLACE).
 */
namespace flowmesh2 {

// TODO(rebirth:M3-ENG-11) per-market tick, lot and minimum size at listing.
struct MarketDescriptor {
    AssetId base;
    AssetId quote;
    uint64_t base_atoms_per_lot{0};
    uint64_t quote_atoms_per_tick{0};

    //! Tagged(MARKET, serialization).
    MarketId Id() const;

    friend bool operator==(const MarketDescriptor&, const MarketDescriptor&) = default;
    SERIALIZE_METHODS(MarketDescriptor, obj)
    {
        READWRITE(obj.base, obj.quote, obj.base_atoms_per_lot, obj.quote_atoms_per_tick);
    }
};

/** INVALID_INPUT unless base != quote and both per-unit factors are in
 * [1, MAX_ATOMS].
 */
Result<void> ValidateMarket(const MarketDescriptor& market);

//! lots * base_atoms_per_lot; INVALID_INPUT for an invalid market.
Result<Atoms> BaseAtoms(const MarketDescriptor& market, uint64_t lots);

//! lots * price_ticks * quote_atoms_per_tick; INVALID_INPUT for an invalid market.
Result<Atoms> QuoteAtoms(const MarketDescriptor& market, uint64_t lots, uint64_t price_ticks);

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_MARKET_H
