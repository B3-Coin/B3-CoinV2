// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <crypto/hex_base.h>
#include <crypto/sha256.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/engine/matcher.h>
#include <flowmesh2/test/util/int128_vectors.h>
#include <util/int128.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

/** Shared 128-bit vectors (M2-LED-09).
 *
 * contrib/flowmesh2/int128_vectors.py computes every fm2 function with a
 * 128-bit intermediate in Python integers over a seeded, edge-biased input
 * stream and writes the digests to int128_vectors.h. This test draws the same
 * stream and digests the C++ results. A native __int128 build and a Boost
 * fallback build (MSVC, or -U__SIZEOF_INT128__) must both match.
 */

using namespace flowmesh2;
namespace iv = flowmesh2::int128_vectors;

namespace {

constexpr uint64_t U64_MAX{std::numeric_limits<uint64_t>::max()};
constexpr uint64_t MAX_ATOMS_U64{static_cast<uint64_t>(MAX_ATOMS)};

//! splitmix64 and the edge-biased picker of int128_vectors.py, draw for draw.
class Stream
{
public:
    explicit Stream(uint64_t seed) : m_state{seed} {}

    uint64_t Next()
    {
        m_state += 0x9E3779B97F4A7C15ULL;
        uint64_t z{m_state};
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    uint64_t Pick()
    {
        const uint64_t kind{Next() % 8};
        if (kind == 0) return Next();
        if (kind == 1) {
            const uint64_t value{Next()};
            return value >> (Next() % 64);
        }
        if (kind == 2) return Near(uint64_t{1} << (Next() % 64));
        if (kind == 3) return U64_MAX - Next() % 4;
        if (kind == 4) return Next() % 17;
        if (kind == 5) return Near(MAX_ATOMS_U64);
        if (kind == 6) return U64_MAX >> (64 - (1 + Next() % 64));
        return Near(uint64_t{1} << (29 + Next() % 6));
    }

private:
    //! base + {-2..2}, wrapping like the generator's mask.
    uint64_t Near(uint64_t base) { return base + Next() % 5 - 2; }

    uint64_t m_state;
};

struct Case {
    uint16_t code{0};
    std::vector<uint64_t> values;
};

template <class T>
Case Single(const Result<T>& result)
{
    if (!result.has_value()) return Case{static_cast<uint16_t>(result.error().code), {0}};
    return Case{0, {static_cast<uint64_t>(*result)}};
}

AssetId TestAsset(std::string_view name)
{
    return AssetId{Tagged(HashTag::TEST_ASSET, std::span{reinterpret_cast<const unsigned char*>(name.data()), name.size()})};
}

MarketDescriptor DrawMarket(Stream& s)
{
    const uint64_t lot{s.Pick()};
    const uint64_t tick{s.Pick()};
    return MarketDescriptor{AssetId::Native(), TestAsset("rUSD"), lot, tick};
}

Case DrawCase(std::string_view family, Stream& s)
{
    if (family == "MulChecked") {
        const uint64_t a{s.Pick()};
        const uint64_t b{s.Pick()};
        return Single(MulChecked(a, b));
    }
    if (family == "MulDivFloor" || family == "MulDivCeil") {
        const uint64_t a{s.Pick()};
        const uint64_t b{s.Pick()};
        const uint64_t d{s.Pick()};
        return Single(family == "MulDivFloor" ? MulDivFloor(a, b, d) : MulDivCeil(a, b, d));
    }
    if (family == "BaseAtoms") {
        const MarketDescriptor market{DrawMarket(s)};
        return Single(BaseAtoms(market, s.Pick()));
    }
    if (family == "QuoteAtoms") {
        const MarketDescriptor market{DrawMarket(s)};
        const uint64_t lots{s.Pick()};
        const uint64_t price{s.Pick()};
        return Single(QuoteAtoms(market, lots, price));
    }
    BOOST_REQUIRE_EQUAL(family, "AllocateProRata");
    const uint64_t supply{s.Pick()};
    const size_t m{static_cast<size_t>(1 + s.Next() % 6)};
    std::vector<uint64_t> demands(m);
    for (uint64_t& d : demands) d = s.Pick();
    std::vector<uint64_t> ranks(m);
    for (uint64_t& r : ranks) r = s.Next() % 8; // small, so repeats occur
    const auto result{engine::AllocateProRata(supply, demands, ranks)};
    if (!result.has_value()) return Case{static_cast<uint16_t>(result.error().code), std::vector<uint64_t>(m, 0)};
    return Case{0, *result};
}

void AppendRecord(CSHA256& hasher, const Case& c)
{
    const unsigned char code[2]{static_cast<unsigned char>(c.code & 0xff), static_cast<unsigned char>(c.code >> 8)};
    hasher.Write(code, sizeof(code));
    for (const uint64_t v : c.values) {
        unsigned char le[8];
        for (int i{0}; i < 8; ++i) le[i] = static_cast<unsigned char>(v >> (8 * i));
        hasher.Write(le, sizeof(le));
    }
}

void CheckFamily(const iv::Family& family)
{
    BOOST_TEST_MESSAGE("family " << family.name);
    Stream s{family.seed};
    CSHA256 hasher;
    for (size_t i{0}; i < iv::CASES; ++i) {
        const Case c{DrawCase(family.name, s)};
        if (i < iv::SHOWN) {
            const iv::Shown& want{family.shown[i]};
            BOOST_CHECK_MESSAGE(c.code == want.code, family.name << " case " << i << ": code " << c.code << ", want " << want.code);
            BOOST_REQUIRE_EQUAL(c.values.size(), want.count);
            for (size_t k{0}; k < want.count; ++k) {
                BOOST_CHECK_MESSAGE(c.values[k] == want.values[k], family.name << " case " << i << " value " << k << ": "
                                                                               << c.values[k] << ", want " << want.values[k]);
            }
        }
        AppendRecord(hasher, c);
    }
    std::array<unsigned char, CSHA256::OUTPUT_SIZE> digest;
    hasher.Finalize(digest.data());
    BOOST_CHECK_EQUAL(HexStr(digest), std::string{family.digest});
}

} // namespace

BOOST_AUTO_TEST_SUITE(int128_vector_tests)

BOOST_AUTO_TEST_CASE(backend_is_reported)
{
    // Boost's fixed-width integer is a class; a native __int128 is not. The
    // evidence of a fallback run must show "boost".
    constexpr bool boost{std::is_class_v<util::Unsigned128>};
    static_assert(boost == std::is_class_v<util::Signed128>);
    BOOST_TEST_MESSAGE("util::Unsigned128 backend: " << (boost ? "boost" : "native"));
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(mul_checked_vectors) { CheckFamily(iv::MULCHECKED); }
BOOST_AUTO_TEST_CASE(mul_div_floor_vectors) { CheckFamily(iv::MULDIVFLOOR); }
BOOST_AUTO_TEST_CASE(mul_div_ceil_vectors) { CheckFamily(iv::MULDIVCEIL); }
BOOST_AUTO_TEST_CASE(base_atoms_vectors) { CheckFamily(iv::BASEATOMS); }
BOOST_AUTO_TEST_CASE(quote_atoms_vectors) { CheckFamily(iv::QUOTEATOMS); }
BOOST_AUTO_TEST_CASE(allocate_pro_rata_vectors) { CheckFamily(iv::ALLOCATEPRORATA); }

BOOST_AUTO_TEST_SUITE_END()
