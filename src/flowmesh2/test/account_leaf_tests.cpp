// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <crypto/hex_base.h>
#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/test/util/keys.h>
#include <serialize.h>
#include <streams.h>

#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;

namespace {

ProfileParams Profile() { return DevRegtestProfile(ChainType::REGTEST).value(); }

template <class T>
void CheckError(const Result<T>& result, ErrorCode expected)
{
    BOOST_REQUIRE_MESSAGE(!result.has_value(), "unexpected success");
    BOOST_CHECK_EQUAL(ToString(result.error().code), ToString(expected));
}

const AssetId RUSD{LabelHash("fm2/test/rusd")};

//! The golden leaf of evidence/R4/golden.py.
AccountLeafV0 Sample()
{
    AccountLeafV0 leaf;
    leaf.account = AccountId{LabelHash("fm2/test/alice-account")};
    leaf.balances = {AssetBalance{AssetId::Native(), 2'000'000'000, 0}, AssetBalance{RUSD, 399'500'000, 100'000'000}};
    leaf.used_nonces = {1, 2, 3};
    leaf.open_orders = 1;
    return leaf;
}

void Flip(uint256& value) { value.data()[0] ^= 0x01; }

} // namespace

BOOST_AUTO_TEST_SUITE(account_leaf_tests)

BOOST_AUTO_TEST_CASE(golden_bytes_and_leaf_value)
{
    // Goldens from evidence/R4/golden.py (independent of the C++ code).
    const AccountLeafV0 leaf{Sample()};
    BOOST_CHECK_EQUAL(leaf.Encode().size(), 158U);
    BOOST_CHECK_EQUAL(HexStr(leaf.Encode()),
                      "57ce18f5adc74afd420707bebef578bfc681e003a6bfea041163200e6dd7483b020000000000000000000000000000"
                      "00000000000000000000000000000000000000943577000000000000000000000000351cc7086ec37dd5bebb5775297a"
                      "065b4f785b3bac0375eed91baf048aa96e85e0e2cf170000000000e1f505000000000301000000000000000200000000"
                      "000000030000000000000001000000");
    BOOST_CHECK_EQUAL(HexStr(leaf.LeafValue()), "a56156a231fb9b9da3efe8ad00d7e26ec682d8e3777c62ec2c81660f07984dbe");
    BOOST_CHECK(leaf.LeafValue() == Tagged(HashTag::STATE_ACCOUNTS, leaf.Encode()));
}

BOOST_AUTO_TEST_CASE(round_trip_and_accessors)
{
    const ProfileParams profile{Profile()};
    const AccountLeafV0 leaf{Sample()};
    BOOST_CHECK(leaf.IsCanonical(profile));
    const auto decoded{AccountLeafV0::Decode(leaf.Encode(), profile)};
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK(*decoded == leaf);

    BOOST_CHECK_EQUAL(leaf.Available(AssetId::Native()), 2'000'000'000);
    BOOST_CHECK_EQUAL(leaf.Reserved(AssetId::Native()), 0);
    BOOST_CHECK_EQUAL(leaf.Available(RUSD), 399'500'000);
    BOOST_CHECK_EQUAL(leaf.Reserved(RUSD), 100'000'000);
    BOOST_CHECK_EQUAL(leaf.Available(AssetId{LabelHash("fm2/test/tok")}), 0); // absent means zero
    BOOST_CHECK(leaf.NonceUsed(2));
    BOOST_CHECK(!leaf.NonceUsed(4));

    // An empty leaf (a fresh account) round-trips too.
    const AccountLeafV0 empty{AccountId{LabelHash("fm2/test/carol-account")}, {}, {}, 0};
    BOOST_CHECK(AccountLeafV0::Decode(empty.Encode(), profile).value() == empty);
}

BOOST_AUTO_TEST_CASE(canonical_form_enforced)
{
    const ProfileParams profile{Profile()};
    const std::vector<std::pair<std::string, std::function<void(AccountLeafV0&)>>> breaks{
        {"assets out of order", [](AccountLeafV0& l) { std::swap(l.balances[0], l.balances[1]); }},
        {"asset repeated", [](AccountLeafV0& l) { l.balances[1].asset = l.balances[0].asset; }},
        {"all-zero entry", [](AccountLeafV0& l) { l.balances[0].available = 0; }},
        {"negative available", [](AccountLeafV0& l) { l.balances[0].available = -1; }},
        {"reserved above MAX_ATOMS", [](AccountLeafV0& l) { l.balances[1].reserved = MAX_ATOMS + 1; }},
        {"nonces out of order", [](AccountLeafV0& l) { l.used_nonces = {2, 1}; }},
        {"nonce repeated", [](AccountLeafV0& l) { l.used_nonces = {1, 1}; }},
        {"open orders above cap", [](AccountLeafV0& l) { l.open_orders = 1'001; }},
    };
    for (const auto& [name, mutate] : breaks) {
        AccountLeafV0 leaf{Sample()};
        mutate(leaf);
        BOOST_CHECK_MESSAGE(!leaf.IsCanonical(profile), name);
        const auto decoded{AccountLeafV0::Decode(leaf.Encode(), profile)};
        BOOST_CHECK_MESSAGE(!decoded.has_value() && decoded.error().code == ErrorCode::DECODE, name);
    }
    // A zero entry is fine when the other side is non-zero.
    AccountLeafV0 reserved_only{Sample()};
    reserved_only.balances[1].available = 0;
    BOOST_CHECK(AccountLeafV0::Decode(reserved_only.Encode(), profile).has_value());

    auto trailing{Sample().Encode()};
    trailing.push_back(0x00);
    CheckError(AccountLeafV0::Decode(trailing, profile), ErrorCode::DECODE);
    const auto bytes{Sample().Encode()};
    const std::vector<unsigned char> truncated(bytes.begin(), bytes.end() - 1);
    CheckError(AccountLeafV0::Decode(truncated, profile), ErrorCode::DECODE);
}

BOOST_AUTO_TEST_CASE(caps_nonces_and_assets)
{
    const ProfileParams profile{Profile()};
    BOOST_REQUIRE_EQUAL(profile.nonce_window, 100U);

    AccountLeafV0 full{Sample()};
    full.used_nonces.clear();
    for (uint64_t n{1}; n <= 100; ++n) full.used_nonces.push_back(n);
    full.balances.clear();
    for (size_t i{0}; i < MAX_LEAF_ASSETS; ++i) {
        uint256 id;
        id.data()[30] = static_cast<unsigned char>(i >> 8);
        id.data()[31] = static_cast<unsigned char>(i & 0xff);
        id.data()[0] = 1; // keep every id distinct from the native asset
        full.balances.push_back(AssetBalance{AssetId{id}, 1, 0});
    }
    std::sort(full.balances.begin(), full.balances.end(),
              [](const AssetBalance& a, const AssetBalance& b) { return a.asset < b.asset; });
    full.open_orders = 1'000;
    BOOST_CHECK_EQUAL(full.Encode().size(), AccountLeafV0::MaxEncodedSize(profile));
    BOOST_CHECK(AccountLeafV0::Decode(full.Encode(), profile).has_value());

    AccountLeafV0 too_many_nonces{full};
    too_many_nonces.balances.clear();
    too_many_nonces.used_nonces.push_back(101);
    BOOST_CHECK(!too_many_nonces.IsCanonical(profile));
    CheckError(AccountLeafV0::Decode(too_many_nonces.Encode(), profile), ErrorCode::OVERSIZE);

    AccountLeafV0 too_many_assets{full};
    too_many_assets.used_nonces.clear();
    uint256 extra;
    extra.data()[0] = 0xff;
    too_many_assets.balances.push_back(AssetBalance{AssetId{extra}, 1, 0});
    BOOST_CHECK(!too_many_assets.IsCanonical(profile));
    CheckError(AccountLeafV0::Decode(too_many_assets.Encode(), profile), ErrorCode::OVERSIZE);

    // A nonce count above the window, with nothing behind it.
    std::vector<unsigned char> count_only;
    VectorWriter writer{count_only, 0};
    writer << AccountId{};
    WriteCompactSize(writer, 0);
    WriteCompactSize(writer, 1'000'000);
    CheckError(AccountLeafV0::Decode(count_only, profile), ErrorCode::OVERSIZE);
}

BOOST_AUTO_TEST_CASE(every_field_changes_the_leaf_value)
{
    const AccountLeafV0 base{Sample()};
    const auto& [f1, f2, f3, f4] = base; // 4 fields
    (void)f1, (void)f2, (void)f3, (void)f4;
    const std::vector<std::pair<std::string, std::function<void(AccountLeafV0&)>>> mutations{
        {"account", [](AccountLeafV0& l) { Flip(l.account.id); }},
        {"balance asset", [](AccountLeafV0& l) { Flip(l.balances[1].asset.id); }},
        {"balance available", [](AccountLeafV0& l) { ++l.balances[0].available; }},
        {"balance reserved", [](AccountLeafV0& l) { ++l.balances[1].reserved; }},
        {"balance removed", [](AccountLeafV0& l) { l.balances.pop_back(); }},
        {"nonce changed", [](AccountLeafV0& l) { l.used_nonces.back() = 9; }},
        {"nonce added", [](AccountLeafV0& l) { l.used_nonces.push_back(4); }},
        {"open orders", [](AccountLeafV0& l) { ++l.open_orders; }},
    };
    std::set<uint256> values{base.LeafValue()};
    for (const auto& [name, mutate] : mutations) {
        AccountLeafV0 changed{base};
        mutate(changed);
        BOOST_CHECK_MESSAGE(values.insert(changed.LeafValue()).second, name << ": leaf value unchanged or repeated");
    }
}

BOOST_AUTO_TEST_SUITE_END()
