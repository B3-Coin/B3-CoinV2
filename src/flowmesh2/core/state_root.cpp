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

uint256 MarketLeafValue(const MarketDescriptor& market) { return TaggedObject(HashTag::STATE_MARKETS, market); }

uint256 NullifierLeafValue(const Outpoint& outpoint) { return TaggedObject(HashTag::STATE_NULLIFIERS, outpoint); }

uint256 StateRootsV0::Root() const
{
    HashWriter writer{TaggedWriter(HashTag::STATE)};
    writer << COMMITMENT_VERSION_V0 << *this;
    return writer.GetSHA256();
}

} // namespace flowmesh2
