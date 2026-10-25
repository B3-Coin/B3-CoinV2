// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>
#include <flowmesh2/net/tcp_mesh_wire.h>

#include <array>
#include <cstdint>
#include <span>

using namespace flowmesh2::net;

namespace {
MeshHello Hello(uint32_t from, uint32_t to, Lane lane = Lane::PROPOSAL)
{
    MeshHello hello{lane, from, to, {}};
    for (size_t i{0}; i < hello.ellswift.size(); ++i) hello.ellswift[i] = static_cast<unsigned char>(0xa0 + i);
    return hello;
}
std::span<const unsigned char, HELLO_BYTES> View(const std::array<unsigned char, HELLO_BYTES>& bytes) { return bytes; }
//! Seat 2 of 4 listening: it accepts only a lower seat dialling it.
constexpr HelloExpectation LISTENER{2, 4, false, 0, Lane::CRITICAL};
//! Seat 1 of 4 that dialled seat 3 on the PROPOSAL lane.
constexpr HelloExpectation DIALLER{1, 4, true, 3, Lane::PROPOSAL};
} // namespace

BOOST_AUTO_TEST_SUITE(tcp_mesh_wire_tests)

BOOST_AUTO_TEST_CASE(hello_golden_bytes_and_the_handshake_refusals)
{
    const auto bytes{EncodeHello(Hello(1, 2))};
    BOOST_CHECK_EQUAL(bytes[0], 1);           // version
    BOOST_CHECK_EQUAL(bytes[1], 2);           // PROPOSAL
    BOOST_CHECK_EQUAL(bytes[2], 1);           // sender seat, little endian
    BOOST_CHECK_EQUAL(bytes[3] | bytes[4] | bytes[5], 0);
    BOOST_CHECK_EQUAL(bytes[6], 2);           // receiver seat
    BOOST_CHECK_EQUAL(bytes[7] | bytes[8] | bytes[9], 0);
    BOOST_CHECK_EQUAL(bytes[10], 0xa0);       // EllSwift key
    BOOST_CHECK_EQUAL(bytes[HELLO_BYTES - 1], 0xa0 + 63);
    const auto accepted{CheckHello(View(bytes), LISTENER)};
    BOOST_REQUIRE(accepted);
    BOOST_CHECK(*accepted == Hello(1, 2));
    BOOST_CHECK(CheckHello(View(EncodeHello(Hello(3, 1))), DIALLER));

    auto version{bytes};
    version[0] = 2;
    BOOST_CHECK(!CheckHello(View(version), LISTENER));
    auto lane{bytes};
    lane[1] = 0;
    BOOST_CHECK(!CheckHello(View(lane), LISTENER));
    lane[1] = 5;
    BOOST_CHECK(!CheckHello(View(lane), LISTENER));
    BOOST_CHECK(!CheckHello(View(EncodeHello(Hello(1, 3))), LISTENER)); // not addressed to this seat
    BOOST_CHECK(!CheckHello(View(EncodeHello(Hello(2, 2))), LISTENER)); // this seat itself
    BOOST_CHECK(!CheckHello(View(EncodeHello(Hello(3, 2))), LISTENER)); // a higher seat must not dial
    BOOST_CHECK(!CheckHello(View(EncodeHello(Hello(4, 2))), LISTENER)); // outside the seat set
    auto high{EncodeHello(Hello(1, 2))};
    high[5] = 1; // seat 2^24 + 1: only the low byte matches
    BOOST_CHECK(!CheckHello(View(high), LISTENER));
    BOOST_CHECK(!CheckHello(View(EncodeHello(Hello(2, 1))), DIALLER));                // not the seat dialled
    BOOST_CHECK(!CheckHello(View(EncodeHello(Hello(3, 1, Lane::ACTION))), DIALLER));  // not the lane dialled
}

BOOST_AUTO_TEST_SUITE_END()
