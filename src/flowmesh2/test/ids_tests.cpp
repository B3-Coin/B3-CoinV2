// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <crypto/hex_base.h>
#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/ids.h>
#include <pubkey.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <array>
#include <string>
#include <type_traits>
#include <vector>

using namespace flowmesh2;

namespace {

uint256 BytesToUint256(std::string_view hex)
{
    const auto bytes{ParseHex(hex)};
    BOOST_REQUIRE_EQUAL(bytes.size(), 32U);
    return uint256{bytes};
}

std::string Hex(const uint256& value) { return HexStr(value); }

uint256 WithByte(size_t pos, unsigned char value)
{
    uint256 out;
    out.data()[pos] = value;
    return out;
}

// The id types never convert into one another.
static_assert(!std::is_convertible_v<AccountId, MarketId>);
static_assert(!std::is_convertible_v<MarketId, AccountId>);
static_assert(!std::is_convertible_v<ActionId, OrderId>, "an OrderId is made only by OrderId::FromPlace");
static_assert(!std::is_convertible_v<OrderId, ActionId>);
static_assert(!std::is_convertible_v<AssetId, AccountId>);
static_assert(!std::is_convertible_v<uint256, AccountId>);

} // namespace

BOOST_AUTO_TEST_SUITE(ids_tests)

BOOST_AUTO_TEST_CASE(native_asset_is_zero)
{
    BOOST_CHECK(AssetId::Native().id.IsNull());
    BOOST_CHECK(AssetId::Native().IsNative());
    BOOST_CHECK(!AssetId{WithByte(31, 1)}.IsNative());
    BOOST_CHECK(AssetId::Native() == AssetId{});
}

BOOST_AUTO_TEST_CASE(account_id_golden)
{
    // Goldens computed independently from the plan text with Python hashlib:
    // SHA256(SHA256("B3/FM2/ACCOUNT") * 2 || xonly).
    const struct {
        const char* xonly;
        const char* account;
    } vectors[]{
        // BIP340 test vector 0 public key.
        {"f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9",
         "ad9e163e809c15854b711e857113c839fcb5bd997cf5c09922d51215be761d85"},
        // x coordinate of the secp256k1 generator.
        {"79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
         "c5cb6e628ddf8dbd6267be680c5c0d8994672fbcee132e2f0c63fcd5492e64ca"},
    };
    for (const auto& v : vectors) {
        const auto bytes{ParseHex(v.xonly)};
        const XOnlyPubKey key{bytes};
        BOOST_CHECK_EQUAL(Hex(AccountId::FromKey(key).id), v.account);
    }
    const auto a{ParseHex(vectors[0].xonly)};
    const auto b{ParseHex(vectors[1].xonly)};
    BOOST_CHECK(AccountId::FromKey(XOnlyPubKey{a}) != AccountId::FromKey(XOnlyPubKey{b}));
}

BOOST_AUTO_TEST_CASE(ids_order_lexicographic)
{
    // Byte 0 dominates: memcmp order, not the reversed GetHex order.
    const uint256 first_byte_set{WithByte(0, 1)};
    const uint256 last_byte_set{WithByte(31, 0xff)};
    BOOST_CHECK(AssetId{last_byte_set} < AssetId{first_byte_set});
    BOOST_CHECK(AccountId{last_byte_set} < AccountId{first_byte_set});
    BOOST_CHECK(MarketId{last_byte_set} < MarketId{first_byte_set});
    BOOST_CHECK(ActionId{last_byte_set} < ActionId{first_byte_set});
    BOOST_CHECK(OrderId{last_byte_set} < OrderId{first_byte_set});
    BOOST_CHECK(AssetId{} < AssetId{last_byte_set});
    BOOST_CHECK(!(AssetId{first_byte_set} < AssetId{first_byte_set}));
    BOOST_CHECK((AssetId{first_byte_set} <=> AssetId{first_byte_set}) == std::strong_ordering::equal);
}

BOOST_AUTO_TEST_CASE(order_id_from_place)
{
    const ActionId place{BytesToUint256("0102030405060708091011121314151617181920212223242526272829303132")};
    BOOST_CHECK(OrderId::FromPlace(place).id == place.id);
}

BOOST_AUTO_TEST_CASE(outpoint_order_and_encoding)
{
    const Outpoint low{WithByte(0, 1), 7};
    const Outpoint low_next{WithByte(0, 1), 8};
    const Outpoint high{WithByte(0, 2), 0};
    BOOST_CHECK(low < low_next);
    BOOST_CHECK(low_next < high);
    BOOST_CHECK(low < high);

    const auto bytes{EncodeBytes(low_next)};
    BOOST_CHECK_EQUAL(bytes.size(), 36U);
    BOOST_CHECK_EQUAL(HexStr(bytes), "01" + std::string(62, '0') + "08000000");
    const auto decoded{DecodeExact<Outpoint>(bytes, 36)};
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK(*decoded == low_next);
}

BOOST_AUTO_TEST_CASE(ids_encode_as_raw_bytes)
{
    const uint256 value{BytesToUint256("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff")};
    BOOST_CHECK_EQUAL(HexStr(EncodeBytes(AssetId{value})), Hex(value));
    BOOST_CHECK_EQUAL(HexStr(EncodeBytes(AccountId{value})), Hex(value));
    BOOST_CHECK_EQUAL(HexStr(EncodeBytes(MarketId{value})), Hex(value));
    BOOST_CHECK_EQUAL(HexStr(EncodeBytes(ActionId{value})), Hex(value));
    BOOST_CHECK_EQUAL(HexStr(EncodeBytes(OrderId{value})), Hex(value));
}

BOOST_AUTO_TEST_SUITE_END()
