// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <crypto/hex_base.h>
#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/ids.h>
#include <serialize.h>
#include <streams.h>
#include <util/strencodings.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace flowmesh2;

namespace {

//! Element type that counts default constructions, to prove that a refused
//! count allocates and constructs nothing.
struct Counted {
    static inline int constructed{0};
    uint8_t value{0};
    Counted() { ++constructed; }
    SERIALIZE_METHODS(Counted, obj) { READWRITE(obj.value); }
};

//! A capped list of Counted, cap 4.
struct SmallList {
    static constexpr size_t CAP{4};
    std::vector<Counted> items;
    template <typename Stream>
    void Serialize(Stream& s) const { WriteCappedVector(s, items, CAP); }
    template <typename Stream>
    void Unserialize(Stream& s) { ReadCappedVector(s, items, CAP); }
};

//! A capped byte string, cap 8.
struct SmallBytes {
    static constexpr size_t CAP{8};
    std::vector<unsigned char> bytes;
    template <typename Stream>
    void Serialize(Stream& s) const { WriteCappedVector(s, bytes, CAP); }
    template <typename Stream>
    void Unserialize(Stream& s) { ReadCappedVector(s, bytes, CAP); }
};

std::vector<unsigned char> Bytes(std::string_view hex) { return ParseHex(hex); }

template <class T>
void CheckError(const Result<T>& result, ErrorCode expected)
{
    BOOST_REQUIRE_MESSAGE(!result.has_value(), "unexpected success");
    BOOST_CHECK_EQUAL(ToString(result.error().code), ToString(expected));
}

} // namespace

BOOST_AUTO_TEST_SUITE(codec_tests)

BOOST_AUTO_TEST_CASE(decode_exact_round_trip)
{
    Outpoint outpoint;
    outpoint.txid.data()[0] = 0xaa;
    outpoint.vout = 0x01020304;
    const auto bytes{EncodeBytes(outpoint)};
    const auto decoded{DecodeExact<Outpoint>(bytes, bytes.size())};
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK(*decoded == outpoint);
}

BOOST_AUTO_TEST_CASE(decode_exact_refuses_trailing_short_and_long_input)
{
    auto bytes{EncodeBytes(Outpoint{})};
    BOOST_REQUIRE_EQUAL(bytes.size(), 36U);

    auto trailing{bytes};
    trailing.push_back(0x00);
    CheckError(DecodeExact<Outpoint>(trailing, 64), ErrorCode::DECODE);

    const std::vector<unsigned char> short_input(bytes.begin(), bytes.end() - 1);
    CheckError(DecodeExact<Outpoint>(short_input, 64), ErrorCode::DECODE);
    CheckError(DecodeExact<Outpoint>(std::vector<unsigned char>{}, 64), ErrorCode::DECODE);

    // Longer than the caller's maximum: refused before parsing.
    CheckError(DecodeExact<Outpoint>(bytes, 35), ErrorCode::OVERSIZE);
    CheckError(DecodeExact<Outpoint>(trailing, 36), ErrorCode::OVERSIZE);
}

BOOST_AUTO_TEST_CASE(capped_vector_at_and_over_cap)
{
    SmallList list;
    list.items.resize(SmallList::CAP);
    for (size_t i{0}; i < list.items.size(); ++i) list.items[i].value = static_cast<uint8_t>(i + 1);
    const auto bytes{EncodeBytes(list)};
    BOOST_CHECK_EQUAL(HexStr(bytes), "0401020304");
    const auto decoded{DecodeExact<SmallList>(bytes, 64)};
    BOOST_REQUIRE(decoded.has_value());
    BOOST_REQUIRE_EQUAL(decoded->items.size(), SmallList::CAP);
    BOOST_CHECK_EQUAL(decoded->items[3].value, 4);

    // Count CAP + 1 with enough items behind it: OVERSIZE, not DECODE.
    CheckError(DecodeExact<SmallList>(Bytes("050102030405"), 64), ErrorCode::OVERSIZE);
    // An encoder never writes what the decoder refuses.
    SmallList over;
    over.items.resize(SmallList::CAP + 1);
    BOOST_CHECK_THROW(EncodeBytes(over), CapExceeded);
}

BOOST_AUTO_TEST_CASE(oversize_count_refused_without_allocating)
{
    const std::vector<std::string> huge_counts{
        "05",                 // CAP + 1, no items behind it
        "fdffff",             // 65,535
        "feffffffff",         // 2^32 - 1
        "ffffffffffffffffff", // 2^64 - 1: allocating it would throw bad_alloc or length_error
    };
    for (const auto& hex : huge_counts) {
        Counted::constructed = 0;
        const auto data{Bytes(hex)};
        SpanReader reader{data};
        std::vector<Counted> out;
        BOOST_CHECK_THROW(ReadCappedVector(reader, out, SmallList::CAP), CapExceeded);
        BOOST_CHECK_EQUAL(out.capacity(), 0U);
        BOOST_CHECK_EQUAL(Counted::constructed, 0);
        CheckError(DecodeExact<SmallList>(Bytes(hex), 64), ErrorCode::OVERSIZE);
        BOOST_CHECK_EQUAL(Counted::constructed, 0);
    }
}

BOOST_AUTO_TEST_CASE(non_canonical_compact_size_refused)
{
    // Each encodes a small count in a longer form than necessary.
    const std::vector<std::string> non_canonical{
        "fd0300",             // 3 in 3 bytes
        "fdfc00",             // 252 in 3 bytes
        "fe03000000",         // 3 in 5 bytes
        "feffff0000",         // 65,535 in 5 bytes
        "ff0300000000000000", // 3 in 9 bytes
    };
    for (const auto& hex : non_canonical) {
        CheckError(DecodeExact<SmallList>(Bytes(hex + "010203"), 64), ErrorCode::DECODE);
    }
    // The canonical form of 3 decodes.
    BOOST_CHECK(DecodeExact<SmallList>(Bytes("03010203"), 64).has_value());
}

BOOST_AUTO_TEST_CASE(capped_byte_vector)
{
    SmallBytes value;
    value.bytes = {0xde, 0xad, 0xbe, 0xef};
    const auto bytes{EncodeBytes(value)};
    BOOST_CHECK_EQUAL(HexStr(bytes), "04deadbeef");
    const auto decoded{DecodeExact<SmallBytes>(bytes, 64)};
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK(decoded->bytes == value.bytes);
    CheckError(DecodeExact<SmallBytes>(Bytes("09000000000000000000"), 64), ErrorCode::OVERSIZE);
    CheckError(DecodeExact<SmallBytes>(Bytes("04deadbe"), 64), ErrorCode::DECODE);
}

BOOST_AUTO_TEST_CASE(decode_exact_with_custom_reader)
{
    const auto bytes{Bytes("2a000000")};
    const auto read_u32{[](SpanReader& reader) {
        uint32_t value{0};
        reader >> value;
        return value;
    }};
    const auto decoded{DecodeExactWith(bytes, 4, read_u32)};
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 42U);
    CheckError(DecodeExactWith(Bytes("2a00000000"), 8, read_u32), ErrorCode::DECODE);
    // A reader that throws CapExceeded itself maps to OVERSIZE.
    const auto refuse{[](SpanReader&) -> uint32_t { throw CapExceeded{"test cap"}; }};
    CheckError(DecodeExactWith(bytes, 4, refuse), ErrorCode::OVERSIZE);
}

BOOST_AUTO_TEST_SUITE_END()
