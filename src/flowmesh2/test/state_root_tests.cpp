// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <crypto/hex_base.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/test/util/keys.h>
#include <serialize.h>
#include <streams.h>

#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;

namespace {

ProfileParams Profile() { return DevRegtestProfile(ChainType::REGTEST).value(); }

AssetId TestAsset(std::string_view name)
{
    return AssetId{Tagged(HashTag::TEST_ASSET, std::span{reinterpret_cast<const unsigned char*>(name.data()), name.size()})};
}

TotalsLeafV0 SampleTotals() { return TotalsLeafV0{TestAsset("rUSD"), AssetTotals{1'000'000'000, 7, 11, 50}}; }

StateRootsV0 LabelledRoots()
{
    return StateRootsV0{LabelHash("fm2/test/root/accounts"), LabelHash("fm2/test/root/orders"),
                        LabelHash("fm2/test/root/markets"), LabelHash("fm2/test/root/nullifiers"),
                        LabelHash("fm2/test/root/totals")};
}

void Flip(uint256& value) { value.data()[0] ^= 0x01; }

} // namespace

BOOST_AUTO_TEST_SUITE(state_root_tests)

BOOST_AUTO_TEST_CASE(protocol_accounts_golden)
{
    // Goldens from evidence/R5/golden.py: Tagged(PROTOCOL_ACCOUNT, name || profile_hash).
    const ProfileParams profile{Profile()};
    BOOST_CHECK_EQUAL(HexStr(SeatPoolAccount(profile).id), "fd93419098c3c5e92abf35c6137b61a61256c79b7ecbfc8035629686e365fafe");
    BOOST_CHECK_EQUAL(HexStr(TreasuryAccount(profile).id), "f01fabb0e7ab2ad143342973b39fe69befe3a43481c84071f98105d0f240eac1");
    BOOST_CHECK(SeatPoolAccount(profile) == SeatPoolAccount(ProfileHash(profile)));
    BOOST_CHECK(TreasuryAccount(profile) == TreasuryAccount(ProfileHash(profile)));
    BOOST_CHECK(SeatPoolAccount(profile) != TreasuryAccount(profile));

    // Bound to the profile: any rule change gives other protocol accounts.
    ProfileParams other{profile};
    other.fee_ppm = 101;
    BOOST_CHECK(SeatPoolAccount(other) != SeatPoolAccount(profile));
    BOOST_CHECK(TreasuryAccount(other) != TreasuryAccount(profile));
    // Never the AccountId of a key: another tag (ACCOUNT) and another length.
    const uint256 profile_hash{ProfileHash(profile)};
    BOOST_CHECK(SeatPoolAccount(profile).id != Tagged(HashTag::ACCOUNT, std::span{profile_hash.begin(), profile_hash.end()}));
}

BOOST_AUTO_TEST_CASE(totals_leaf_golden_and_round_trip)
{
    const TotalsLeafV0 leaf{SampleTotals()};
    BOOST_CHECK_EQUAL(leaf.Encode().size(), TotalsLeafV0::ENCODED_BYTES);
    BOOST_CHECK_EQUAL(HexStr(leaf.Encode()),
                      "6439dd037ac1af8e258b4177319230f77f127c3fcf5ec6436aba005bb25bd7ed00ca9a3b0000000007000000000000"
                      "000b000000000000003200000000000000");
    BOOST_CHECK_EQUAL(HexStr(leaf.LeafValue()), "07910e487e30bbe3d6fb8a382cf9b3df817f9a1c2fc5c7e27a9a1d54da30f59c");
    BOOST_CHECK(leaf.LeafValue() == Tagged(HashTag::STATE_TOTALS, leaf.Encode()));

    const auto decoded{TotalsLeafV0::Decode(leaf.Encode())};
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK(*decoded == leaf);

    // Every field changes the leaf value.
    std::set<uint256> values{leaf.LeafValue()};
    for (int field{0}; field < 5; ++field) {
        TotalsLeafV0 changed{leaf};
        switch (field) {
        case 0: Flip(changed.asset.id); break;
        case 1: changed.totals.deposits += 1; break;
        case 2: changed.totals.effects += 1; break;
        case 3: changed.totals.pending += 1; break;
        case 4: changed.totals.fees_held += 1; break;
        }
        values.insert(changed.LeafValue());
    }
    BOOST_CHECK_EQUAL(values.size(), 6U);
}

BOOST_AUTO_TEST_CASE(totals_leaf_decode_refusals)
{
    const std::vector<unsigned char> good{SampleTotals().Encode()};

    std::vector<unsigned char> longer{good};
    longer.push_back(0);
    const auto oversize{TotalsLeafV0::Decode(longer)};
    BOOST_REQUIRE(!oversize.has_value());
    BOOST_CHECK(oversize.error().code == ErrorCode::OVERSIZE);

    const std::vector<unsigned char> shorter(good.begin(), good.end() - 1);
    const auto truncated{TotalsLeafV0::Decode(shorter)};
    BOOST_REQUIRE(!truncated.has_value());
    BOOST_CHECK(truncated.error().code == ErrorCode::DECODE);

    // All-zero totals are not canonical (an unseen asset has no leaf).
    const TotalsLeafV0 zero{TestAsset("rUSD"), AssetTotals{}};
    const auto all_zero{TotalsLeafV0::Decode(zero.Encode())};
    BOOST_REQUIRE(!all_zero.has_value());
    BOOST_CHECK(all_zero.error().code == ErrorCode::DECODE);

    // Each amount outside [0, MAX_ATOMS] is refused.
    for (const Atoms bad : {Atoms{-1}, MAX_ATOMS + 1}) {
        for (int field{0}; field < 4; ++field) {
            TotalsLeafV0 leaf{SampleTotals()};
            Atoms* amounts[]{&leaf.totals.deposits, &leaf.totals.effects, &leaf.totals.pending, &leaf.totals.fees_held};
            *amounts[field] = bad;
            const auto decoded{TotalsLeafV0::Decode(leaf.Encode())};
            BOOST_REQUIRE(!decoded.has_value());
            BOOST_CHECK(decoded.error().code == ErrorCode::DECODE);
        }
    }
    TotalsLeafV0 edge{SampleTotals()};
    edge.totals.deposits = MAX_ATOMS;
    BOOST_CHECK(TotalsLeafV0::Decode(edge.Encode()).has_value());
}

BOOST_AUTO_TEST_CASE(market_and_nullifier_leaves_golden)
{
    const MarketDescriptor m1{AssetId::Native(), TestAsset("rUSD"), 1'000'000, 1};
    BOOST_CHECK_EQUAL(HexStr(MarketLeafValue(m1)), "f9ce30bd61833304f61f2e062039bf7c88975b570185cfe58906e8619879ec17");
    BOOST_CHECK(MarketLeafValue(m1) != m1.Id().id);

    const uint256 txid{LabelHash("fm2/test/T1")};
    BOOST_CHECK_EQUAL(HexStr(NullifierLeafValue(Outpoint{txid, 0})), "0a1f8c51460c42eb30982e1e3ece013006484760cf3b94b8c0a26b72c613bc38");
    BOOST_CHECK_EQUAL(HexStr(NullifierLeafValue(Outpoint{txid, 1})), "7dab8aaebf792873afac2cd45fb95107d6d95e2f274e330a2116f6e06c4aef53");
}

BOOST_AUTO_TEST_CASE(root_of_roots_golden_and_every_root_changes_it)
{
    const StateRootsV0 roots{LabelledRoots()};
    BOOST_CHECK_EQUAL(HexStr(roots.Root()), "53f2f3f916a26a4ed3b5032dc50fc60c061a5aa333661458aeaddaa2e2381dc1");

    // 160-byte round trip.
    DataStream stream;
    stream << roots;
    BOOST_CHECK_EQUAL(stream.size(), StateRootsV0::ENCODED_BYTES);
    StateRootsV0 back;
    stream >> back;
    BOOST_CHECK(back == roots);

    std::set<uint256> seen{roots.Root()};
    for (int field{0}; field < 5; ++field) {
        StateRootsV0 changed{roots};
        uint256* fields[]{&changed.accounts, &changed.orders, &changed.markets, &changed.nullifiers, &changed.totals};
        Flip(*fields[field]);
        seen.insert(changed.Root());
    }
    BOOST_CHECK_EQUAL(seen.size(), 6U);
    // Swapping two components changes the root: the order is fixed.
    StateRootsV0 swapped{roots};
    std::swap(swapped.accounts, swapped.orders);
    BOOST_CHECK(swapped.Root() != roots.Root());
}

BOOST_AUTO_TEST_SUITE_END()
