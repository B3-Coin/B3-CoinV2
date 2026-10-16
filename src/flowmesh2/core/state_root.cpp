// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/state_root.h>

#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/hashes.h>
#include <hash.h>
#include <streams.h>

#include <ios>
#include <string_view>

namespace flowmesh2 {

namespace {

AccountId ProtocolAccountId(std::string_view name, const uint256& profile_hash)
{
    HashWriter writer{TaggedWriter(HashTag::PROTOCOL_ACCOUNT)};
    writer.write(MakeByteSpan(name));
    writer << profile_hash;
    return AccountId{writer.GetSHA256()};
}

} // namespace

AccountId SeatPoolAccount(const uint256& profile_hash) { return ProtocolAccountId("SEAT_POOL", profile_hash); }
AccountId SeatPoolAccount(const ProfileParams& profile) { return SeatPoolAccount(ProfileHash(profile)); }
AccountId TreasuryAccount(const uint256& profile_hash) { return ProtocolAccountId("TREASURY", profile_hash); }
AccountId TreasuryAccount(const ProfileParams& profile) { return TreasuryAccount(ProfileHash(profile)); }

std::vector<unsigned char> TotalsLeafV0::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter writer{out, 0};
    writer << asset << totals;
    return out;
}

Result<TotalsLeafV0> TotalsLeafV0::Decode(std::span<const unsigned char> bytes)
{
    return DecodeExactWith(bytes, ENCODED_BYTES, [](SpanReader& reader) {
        TotalsLeafV0 leaf;
        reader >> leaf.asset >> leaf.totals;
        if (leaf.totals.IsZero()) throw std::ios_base::failure("TotalsLeafV0 is all zero");
        return leaf;
    });
}

uint256 TotalsLeafV0::LeafValue() const { return Tagged(HashTag::STATE_TOTALS, Encode()); }

bool RestingOrderLeafV0::IsCanonical() const
{
    if (!IsKnown(side) || price_ticks == 0 || remaining_lots == 0) return false;
    if (!InAtomRange(reserved_left) || !InAtomRange(cum_quote) || !InAtomRange(fee_charged)) return false;
    if (fee_charged > cum_quote) return false;
    return side == Side::SELL || fee_charged == 0;
}

std::vector<unsigned char> RestingOrderLeafV0::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter writer{out, 0};
    writer << order << market << owner << static_cast<uint8_t>(side) << price_ticks << remaining_lots << arrival_rank
           << reserved_left << cum_quote << fee_charged << fills;
    return out;
}

Result<RestingOrderLeafV0> RestingOrderLeafV0::Decode(std::span<const unsigned char> bytes)
{
    return DecodeExactWith(bytes, ENCODED_BYTES, [](SpanReader& reader) {
        RestingOrderLeafV0 leaf;
        uint8_t side_byte{0};
        reader >> leaf.order >> leaf.market >> leaf.owner >> side_byte >> leaf.price_ticks >> leaf.remaining_lots >>
            leaf.arrival_rank >> leaf.reserved_left >> leaf.cum_quote >> leaf.fee_charged >> leaf.fills;
        leaf.side = static_cast<Side>(side_byte);
        if (!leaf.IsCanonical()) throw std::ios_base::failure("RestingOrderLeafV0 is not canonical");
        return leaf;
    });
}

uint256 RestingOrderLeafV0::LeafValue() const { return Tagged(HashTag::STATE_ORDERS, Encode()); }

uint256 MarketLeafValue(const MarketDescriptor& market) { return TaggedObject(HashTag::STATE_MARKETS, market); }

uint256 NullifierLeafValue(const Outpoint& outpoint) { return TaggedObject(HashTag::STATE_NULLIFIERS, outpoint); }

uint256 StateRootsV0::Root() const
{
    HashWriter writer{TaggedWriter(HashTag::STATE)};
    writer << COMMITMENT_VERSION_V0 << *this;
    return writer.GetSHA256();
}

} // namespace flowmesh2
