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
#include <flowmesh2/core/slot.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/test/util/keys.h>
#include <serialize.h>
#include <streams.h>

#include <functional>
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
                        LabelHash("fm2/test/root/totals"), uint256{}};
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

// ---- Slice R7: the resting-order leaf ----

namespace {

//! The golden order of evidence/R7/golden.py: Bob's SELL a, 3,000 lots left at 250.
RestingOrderLeafV0 SampleOrder()
{
    RestingOrderLeafV0 leaf;
    leaf.order = OrderId{LabelHash("fm2/test/order/a")};
    leaf.market = MarketId{LabelHash("fm2/test/market")};
    leaf.owner = AccountId{LabelHash("fm2/test/account/bob")};
    leaf.side = Side::SELL;
    leaf.price_ticks = 250;
    leaf.remaining_lots = 3'000;
    leaf.arrival_rank = 4'096;
    leaf.reserved_left = 3'000'000'000;
    leaf.cum_quote = 500'000;
    leaf.fee_charged = 50;
    leaf.fills = 1;
    return leaf;
}

} // namespace

BOOST_AUTO_TEST_CASE(commitment_version_is_defined_once)
{
    // This file includes core/slot.h and core/state_root.h; before R7 both
    // defined COMMITMENT_VERSION_V0, and such a translation unit did not compile.
    BOOST_CHECK_EQUAL(COMMITMENT_VERSION_V0, 0);
    BOOST_CHECK_EQUAL(SlotHeaderV2{}.commitment_version, COMMITMENT_VERSION_V0);
}

BOOST_AUTO_TEST_CASE(resting_order_leaf_golden_and_round_trip)
{
    const RestingOrderLeafV0 leaf{SampleOrder()};
    BOOST_CHECK(leaf.IsCanonical());
    const std::vector<unsigned char> bytes{leaf.Encode()};
    BOOST_CHECK_EQUAL(bytes.size(), RestingOrderLeafV0::ENCODED_BYTES);
    BOOST_CHECK_EQUAL(RestingOrderLeafV0::ENCODED_BYTES, 153U);
    // Golden from evidence/R7/golden.py.
    BOOST_CHECK_EQUAL(HexStr(bytes),
                      "982afde4b813e5349730c20ea4da69fa0c53ae40cd1255f54a151f73617e81db629d31dd15ec7d49cb9eb7ec7244a8dbfa"
                      "8cfdc9da7973340b8eb780e1f8af978258d174cebe005fec467c8cac02323a9b2f1947599a7591ebd3137ab83b986a02fa"
                      "00000000000000b80b0000000000000010000000000000005ed0b20000000020a10700000000003200000000000000010000"
                      "0000000000");
    BOOST_CHECK_EQUAL(HexStr(leaf.LeafValue()), "389e1c17b3896483c5d34187e9db601020a3107376cd3f751329b96bbd0d257c");
    BOOST_CHECK(leaf.LeafValue() == Tagged(HashTag::STATE_ORDERS, bytes));

    const Result<RestingOrderLeafV0> decoded{RestingOrderLeafV0::Decode(bytes)};
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK(*decoded == leaf);
}

BOOST_AUTO_TEST_CASE(resting_order_leaf_canonical_form_enforced)
{
    const auto refused = [](const RestingOrderLeafV0& leaf, std::string_view what) {
        BOOST_CHECK_MESSAGE(!leaf.IsCanonical(), what);
        const Result<RestingOrderLeafV0> decoded{RestingOrderLeafV0::Decode(leaf.Encode())};
        BOOST_REQUIRE_MESSAGE(!decoded.has_value(), what);
        BOOST_CHECK_MESSAGE(decoded.error().code == ErrorCode::DECODE, what);
    };
    RestingOrderLeafV0 leaf{SampleOrder()};
    leaf.side = static_cast<Side>(0);
    refused(leaf, "side 0");
    leaf = SampleOrder();
    leaf.side = static_cast<Side>(3);
    refused(leaf, "side 3");
    leaf = SampleOrder();
    leaf.price_ticks = 0;
    refused(leaf, "price 0");
    leaf = SampleOrder();
    leaf.remaining_lots = 0;
    refused(leaf, "lots 0");
    leaf = SampleOrder();
    leaf.reserved_left = -1;
    refused(leaf, "negative reservation");
    leaf = SampleOrder();
    leaf.cum_quote = MAX_ATOMS + 1;
    refused(leaf, "cum_quote above MAX_ATOMS");
    leaf = SampleOrder();
    leaf.fee_charged = leaf.cum_quote + 1;
    refused(leaf, "fee above cum_quote");
    leaf = SampleOrder();
    leaf.side = Side::BUY;
    refused(leaf, "a BUY with a fee");
    leaf.fee_charged = 0;
    BOOST_CHECK(leaf.IsCanonical());
    // Edges that are canonical: zero reservation and MAX_ATOMS amounts.
    leaf = SampleOrder();
    leaf.reserved_left = 0;
    leaf.cum_quote = MAX_ATOMS;
    leaf.fee_charged = MAX_ATOMS;
    BOOST_CHECK(leaf.IsCanonical());
    BOOST_CHECK(RestingOrderLeafV0::Decode(leaf.Encode()).has_value());

    // Length: one byte short is DECODE, one byte long is OVERSIZE.
    std::vector<unsigned char> bytes{SampleOrder().Encode()};
    bytes.pop_back();
    const Result<RestingOrderLeafV0> short_input{RestingOrderLeafV0::Decode(bytes)};
    BOOST_REQUIRE(!short_input.has_value());
    BOOST_CHECK(short_input.error().code == ErrorCode::DECODE);
    bytes = SampleOrder().Encode();
    bytes.push_back(0);
    const Result<RestingOrderLeafV0> long_input{RestingOrderLeafV0::Decode(bytes)};
    BOOST_REQUIRE(!long_input.has_value());
    BOOST_CHECK(long_input.error().code == ErrorCode::OVERSIZE);
}

BOOST_AUTO_TEST_CASE(resting_order_leaf_every_field_changes_the_value)
{
    const RestingOrderLeafV0 base{SampleOrder()};
    std::vector<std::function<void(RestingOrderLeafV0&)>> mutations{
        [](RestingOrderLeafV0& l) { l.order.id.data()[0] ^= 1; },
        [](RestingOrderLeafV0& l) { l.market.id.data()[0] ^= 1; },
        [](RestingOrderLeafV0& l) { l.owner.id.data()[0] ^= 1; },
        [](RestingOrderLeafV0& l) { l.side = Side::BUY; l.fee_charged = 0; },
        [](RestingOrderLeafV0& l) { ++l.price_ticks; },
        [](RestingOrderLeafV0& l) { ++l.remaining_lots; },
        [](RestingOrderLeafV0& l) { ++l.arrival_rank; },
        [](RestingOrderLeafV0& l) { ++l.reserved_left; },
        [](RestingOrderLeafV0& l) { ++l.cum_quote; },
        [](RestingOrderLeafV0& l) { ++l.fee_charged; },
        [](RestingOrderLeafV0& l) { ++l.fills; },
    };
    std::set<uint256> values{base.LeafValue()};
    for (const auto& mutate : mutations) {
        RestingOrderLeafV0 changed{base};
        mutate(changed);
        BOOST_CHECK(changed.IsCanonical());
        values.insert(changed.LeafValue());
    }
    BOOST_CHECK_EQUAL(values.size(), mutations.size() + 1);
}

BOOST_AUTO_TEST_CASE(withdrawal_records_bind_obligations_and_paid_nullifiers)
{
    WithdrawalRecord record;
    record.id = ActionId{LabelHash("record-id")};
    record.account = AccountId{LabelHash("record-owner")};
    record.asset = TestAsset("rUSD");
    record.amount = 5000;
    record.fee = 1000;
    record.recipient.fill(0x42);
    record.slot = 7;
    BOOST_REQUIRE(record.IsCanonical());
    BOOST_CHECK_EQUAL(record.Encode().size(), 177U);
    BOOST_CHECK(WithdrawalRecord::Decode(record.Encode()).value() == record);
    const auto unpaid{record.LeafValue()};
    record.paid = true;
    BOOST_CHECK(!record.IsCanonical());
    record.payout_event = Outpoint{LabelHash("record-paid-tx"), 1};
    BOOST_REQUIRE(record.IsCanonical());
    BOOST_CHECK(WithdrawalRecord::Decode(record.Encode()).value() == record);
    BOOST_CHECK(record.LeafValue() != unpaid);
    const auto paid{record.LeafValue()};
    ++record.payout_event.vout;
    BOOST_CHECK(record.LeafValue() != paid);
    auto bad{record.Encode()};
    bad[140] = 2; // paid is exactly one canonical byte
    BOOST_CHECK(!WithdrawalRecord::Decode(bad).has_value());
    bad = record.Encode();
    bad.push_back(0);
    BOOST_CHECK(!WithdrawalRecord::Decode(bad).has_value());
}

BOOST_AUTO_TEST_CASE(six_root_version_does_not_change_legacy_root_or_encoding)
{
    StateRootsV0 old{LabelledRoots()};
    DataStream legacy;
    legacy << old;
    BOOST_CHECK_EQUAL(legacy.size(), 160U);
    const auto old_root{old.Root()};
    StateRootsV0 extended{old};
    extended.version = COMMITMENT_VERSION_WITHDRAWALS;
    extended.withdrawals = LabelHash("sixth-root");
    BOOST_CHECK(extended.Root() != old_root);
    auto changed{extended};
    Flip(changed.withdrawals);
    BOOST_CHECK(changed.Root() != extended.Root());
    BOOST_CHECK(changed.accounts == extended.accounts && changed.totals == extended.totals);
    DataStream truncated;
    BOOST_CHECK_THROW(truncated << extended, std::ios_base::failure);
    StateRootsV0 roundtrip;
    legacy >> roundtrip;
    BOOST_CHECK(roundtrip == old);
    BOOST_CHECK(roundtrip.Root() == old_root);
}

BOOST_AUTO_TEST_CASE(view_bound_profile_versions_six_roots_without_reencoding_archives)
{
    BOOST_CHECK_EQUAL(ExpectedCommitmentVersion(DevRegtestProfile(ChainType::REGTEST).value()), COMMITMENT_VERSION_V0);
    BOOST_CHECK_EQUAL(ExpectedCommitmentVersion(StableQRegtestProfile(ChainType::REGTEST).value()), COMMITMENT_VERSION_WITHDRAWALS);
    BOOST_CHECK_EQUAL(ExpectedCommitmentVersion(StableQViewRegtestProfile(ChainType::REGTEST).value()), COMMITMENT_VERSION_VIEW_BOUND);
    StateRootsV0 archived{LabelledRoots()};
    archived.version = COMMITMENT_VERSION_WITHDRAWALS;
    archived.withdrawals = LabelHash("view-bound-sixth-root");
    const auto archived_hash{archived.Root()};
    auto current{archived}; current.version = COMMITMENT_VERSION_VIEW_BOUND;
    BOOST_CHECK(current.Root() != archived_hash);
    auto changed{current}; Flip(changed.withdrawals);
    BOOST_CHECK(changed.Root() != current.Root());
    changed = current; Flip(changed.accounts);
    BOOST_CHECK(changed.Root() != current.Root());
    BOOST_CHECK(archived.Root() == archived_hash);
    DataStream legacy_transport;
    BOOST_CHECK_THROW(legacy_transport << current, std::ios_base::failure);
}

BOOST_AUTO_TEST_SUITE_END()
