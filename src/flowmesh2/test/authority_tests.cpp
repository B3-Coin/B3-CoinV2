// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <crypto/hex_base.h>
#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/codec.h>

#include <compare>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace flowmesh2;

namespace {

uint256 Filled(unsigned char byte)
{
    uint256 out;
    for (auto& b : out) b = byte;
    return out;
}

} // namespace

BOOST_AUTO_TEST_SUITE(authority_tests)

BOOST_AUTO_TEST_CASE(fact_cursor_total_order)
{
    constexpr uint32_t U32_MAX{std::numeric_limits<uint32_t>::max()};
    const std::vector<uint32_t> values{0, 1, 2, 3, U32_MAX - 1, U32_MAX};
    std::vector<FactCursor> cursors;
    for (uint32_t height : values) {
        for (uint32_t index : values) cursors.push_back(FactCursor{height, index});
    }
    // Exactly the lexicographic order of (height, index), for every pair.
    for (const auto& a : cursors) {
        for (const auto& b : cursors) {
            const auto expected{std::pair{a.height, a.index} <=> std::pair{b.height, b.index}};
            BOOST_REQUIRE((a <=> b) == expected);
            BOOST_REQUIRE((a == b) == (expected == 0));
            // Trichotomy: exactly one of <, ==, > holds.
            BOOST_REQUIRE(int{a < b} + int{a == b} + int{a > b} == 1);
        }
    }
    BOOST_CHECK((FactCursor{1, U32_MAX} < FactCursor{2, 0}));
}

BOOST_AUTO_TEST_CASE(authority_version_differs_per_field)
{
    const AuthorityVersion base{7, Filled(0x11), Filled(0x22)};
    AuthorityVersion epoch{base};
    ++epoch.epoch;
    AuthorityVersion seats{base};
    seats.seat_set_root.data()[31] ^= 1;
    AuthorityVersion profile{base};
    profile.profile_hash.data()[0] ^= 1;
    BOOST_CHECK(base == AuthorityVersion(base));
    BOOST_CHECK(!(base == epoch));
    BOOST_CHECK(!(base == seats));
    BOOST_CHECK(!(base == profile));
}

BOOST_AUTO_TEST_CASE(encodings_pinned_and_round_trip)
{
    const Anchor anchor{130, Filled(0xab)};
    const auto anchor_bytes{EncodeBytes(anchor)};
    std::string expected_anchor{"82000000"}; // height 130, u32 LE
    for (int i{0}; i < 32; ++i) expected_anchor += "ab";
    BOOST_CHECK_EQUAL(HexStr(anchor_bytes), expected_anchor);
    BOOST_CHECK_EQUAL(anchor_bytes.size(), 36U);
    const auto anchor_back{DecodeExact<Anchor>(anchor_bytes, 36)};
    BOOST_REQUIRE(anchor_back.has_value());
    BOOST_CHECK(*anchor_back == anchor);

    const FactCursor cursor{0x01020304, 0x0a0b0c0d};
    const auto cursor_bytes{EncodeBytes(cursor)};
    BOOST_CHECK_EQUAL(HexStr(cursor_bytes), "040302010d0c0b0a");
    const auto cursor_back{DecodeExact<FactCursor>(cursor_bytes, 8)};
    BOOST_REQUIRE(cursor_back.has_value());
    BOOST_CHECK(*cursor_back == cursor);

    const AuthorityVersion version{0x0102030405060708, Filled(0x11), Filled(0x22)};
    const auto version_bytes{EncodeBytes(version)};
    BOOST_CHECK_EQUAL(HexStr(version_bytes), "0807060504030201" + HexStr(Filled(0x11)) + HexStr(Filled(0x22)));
    BOOST_CHECK_EQUAL(version_bytes.size(), 72U);
    const auto version_back{DecodeExact<AuthorityVersion>(version_bytes, 72)};
    BOOST_REQUIRE(version_back.has_value());
    BOOST_CHECK(*version_back == version);
}

BOOST_AUTO_TEST_SUITE_END()
