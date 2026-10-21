// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <crypto/hex_base.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/market.h>

#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>

using namespace flowmesh2;

namespace {

constexpr uint64_t U64_MAX{std::numeric_limits<uint64_t>::max()};
constexpr uint64_t MAX_ATOMS_U64{static_cast<uint64_t>(MAX_ATOMS)};

AssetId TestAsset(std::string_view name)
{
    return AssetId{Tagged(HashTag::TEST_ASSET, std::span{reinterpret_cast<const unsigned char*>(name.data()), name.size()})};
}

//! ARCHITECTURE.md section 14.1: M1 = B3/rUSD, M2 = TOK/rUSD.
MarketDescriptor M1() { return MarketDescriptor{AssetId::Native(), TestAsset("rUSD"), 1'000'000, 1}; }
MarketDescriptor M2() { return MarketDescriptor{TestAsset("TOK"), TestAsset("rUSD"), 1, 10}; }

template <class T>
void CheckValue(const Result<T>& result, T expected)
{
    // Boost evaluates a check's message even when the check passes, so the
    // error is only read on the failure path.
    if (!result.has_value()) BOOST_FAIL("unexpected error " << ToString(result.error().code));
    BOOST_CHECK_EQUAL(*result, expected);
}

template <class T>
void CheckError(const Result<T>& result, ErrorCode expected)
{
    BOOST_REQUIRE_MESSAGE(!result.has_value(), "unexpected success");
    BOOST_CHECK_EQUAL(ToString(result.error().code), ToString(expected));
}

} // namespace

BOOST_AUTO_TEST_SUITE(market_tests)

BOOST_AUTO_TEST_CASE(walking_skeleton_markets_golden)
{
    // Goldens computed independently with Python hashlib.
    BOOST_CHECK_EQUAL(HexStr(TestAsset("rUSD").id), "6439dd037ac1af8e258b4177319230f77f127c3fcf5ec6436aba005bb25bd7ed");
    BOOST_CHECK_EQUAL(HexStr(TestAsset("TOK").id), "3bf8ece1213c3774c4834e8f734e52eb0a608b96a10b50e64355ae09fe44e9ec");
    BOOST_CHECK_EQUAL(HexStr(M1().Id().id), "311952b291d844870097bbd4c4ae582f339db2c240b3b162f51c5779f06be389");
    BOOST_CHECK_EQUAL(HexStr(M2().Id().id), "7ca79f39eb315c147829cfef3c4872b0bfd1d409f6eed8b41a2d31317146c034");
    BOOST_CHECK(ValidateMarket(M1()).has_value());
    BOOST_CHECK(ValidateMarket(M2()).has_value());
}

BOOST_AUTO_TEST_CASE(walking_skeleton_amounts)
{
    // Slot 2 a: Bob sells 5,000 M1 lots: 5e9 B3 atoms reserved.
    CheckValue(BaseAtoms(M1(), 5'000), Atoms{5'000'000'000});
    // Slot 3 c: 2,000 M1 lots at 250: quote 500,000; Alice reserves at 260.
    CheckValue(QuoteAtoms(M1(), 2'000, 250), Atoms{500'000});
    CheckValue(QuoteAtoms(M1(), 2'000, 260), Atoms{520'000});
    // Slot 2 b: Alice buys 100,000 M2 lots at 600: 600,000,000 rUSD reserved.
    CheckValue(QuoteAtoms(M2(), 100'000, 600), Atoms{600'000'000});
    CheckValue(BaseAtoms(M2(), 100'000), Atoms{100'000});
    CheckValue(QuoteAtoms(M1(), 0, 250), Atoms{0});
    CheckValue(QuoteAtoms(M1(), 10, 0), Atoms{0});
}

BOOST_AUTO_TEST_CASE(quote_atoms_overflow_refused)
{
    const MarketDescriptor unit{AssetId::Native(), TestAsset("rUSD"), 1, 1};
    CheckValue(QuoteAtoms(unit, 2'147'483'648ULL, 2'147'483'648ULL), MAX_ATOMS); // 2^31 * 2^31 = 2^62
    CheckError(QuoteAtoms(unit, 2'147'483'648ULL, 2'147'483'649ULL), ErrorCode::ARITHMETIC_OVERFLOW);
    CheckError(QuoteAtoms(unit, U64_MAX, U64_MAX), ErrorCode::ARITHMETIC_OVERFLOW);
    CheckError(QuoteAtoms(unit, U64_MAX, 1), ErrorCode::ARITHMETIC_OVERFLOW);
    CheckValue(QuoteAtoms(unit, MAX_ATOMS_U64, 1), MAX_ATOMS);
    CheckError(QuoteAtoms(unit, MAX_ATOMS_U64 + 1, 1), ErrorCode::ARITHMETIC_OVERFLOW);

    // The per-tick factor overflows on its own.
    const MarketDescriptor wide{AssetId::Native(), TestAsset("rUSD"), 1, MAX_ATOMS_U64};
    CheckValue(QuoteAtoms(wide, 1, 1), MAX_ATOMS);
    CheckError(QuoteAtoms(wide, 2, 1), ErrorCode::ARITHMETIC_OVERFLOW);
    CheckError(QuoteAtoms(wide, 1, 2), ErrorCode::ARITHMETIC_OVERFLOW);
    // Both products below 2^64 but the result above 2^62.
    const MarketDescriptor ticks{AssetId::Native(), TestAsset("rUSD"), 1, 4};
    CheckValue(QuoteAtoms(ticks, 1'073'741'824ULL, 1'073'741'824ULL), MAX_ATOMS); // 2^30 * 2^30 * 4
    CheckError(QuoteAtoms(ticks, 1'073'741'824ULL, 1'073'741'825ULL), ErrorCode::ARITHMETIC_OVERFLOW);
}

BOOST_AUTO_TEST_CASE(base_atoms_overflow_refused)
{
    CheckValue(BaseAtoms(M1(), MAX_ATOMS_U64 / 1'000'000), Atoms{static_cast<Atoms>(MAX_ATOMS_U64 / 1'000'000 * 1'000'000)});
    CheckError(BaseAtoms(M1(), MAX_ATOMS_U64 / 1'000'000 + 1), ErrorCode::ARITHMETIC_OVERFLOW);
    CheckError(BaseAtoms(M1(), U64_MAX), ErrorCode::ARITHMETIC_OVERFLOW);
}

BOOST_AUTO_TEST_CASE(invalid_markets_refused)
{
    MarketDescriptor same{M1()};
    same.quote = same.base;
    CheckError(ValidateMarket(same), ErrorCode::INVALID_INPUT);
    CheckError(QuoteAtoms(same, 1, 1), ErrorCode::INVALID_INPUT);
    CheckError(BaseAtoms(same, 1), ErrorCode::INVALID_INPUT);

    MarketDescriptor no_lot{M1()};
    no_lot.base_atoms_per_lot = 0;
    CheckError(ValidateMarket(no_lot), ErrorCode::INVALID_INPUT);
    CheckError(BaseAtoms(no_lot, 1), ErrorCode::INVALID_INPUT);

    MarketDescriptor no_tick{M1()};
    no_tick.quote_atoms_per_tick = 0;
    CheckError(ValidateMarket(no_tick), ErrorCode::INVALID_INPUT);
    CheckError(QuoteAtoms(no_tick, 1, 1), ErrorCode::INVALID_INPUT);

    MarketDescriptor huge_lot{M1()};
    huge_lot.base_atoms_per_lot = MAX_ATOMS_U64 + 1;
    CheckError(ValidateMarket(huge_lot), ErrorCode::INVALID_INPUT);
    MarketDescriptor huge_tick{M1()};
    huge_tick.quote_atoms_per_tick = MAX_ATOMS_U64 + 1;
    CheckError(ValidateMarket(huge_tick), ErrorCode::INVALID_INPUT);
}

BOOST_AUTO_TEST_CASE(market_id_depends_on_every_field)
{
    const MarketDescriptor base{M1()};
    std::set<uint256> ids{base.Id().id};
    MarketDescriptor m{base};
    m.base = TestAsset("OTHER");
    BOOST_CHECK(ids.insert(m.Id().id).second);
    m = base;
    m.quote = TestAsset("OTHER");
    BOOST_CHECK(ids.insert(m.Id().id).second);
    m = base;
    ++m.base_atoms_per_lot;
    BOOST_CHECK(ids.insert(m.Id().id).second);
    m = base;
    ++m.quote_atoms_per_tick;
    BOOST_CHECK(ids.insert(m.Id().id).second);
    // Swapping base and quote is another market.
    BOOST_CHECK(ids.insert(MarketDescriptor{base.quote, base.base, base.base_atoms_per_lot, base.quote_atoms_per_tick}.Id().id).second);
}

BOOST_AUTO_TEST_CASE(stable_q_market_policy)
{
    const ProfileParams profile{StableQRegtestProfile(ChainType::REGTEST).value()};
    BOOST_CHECK(ValidateMarketForProfile(M1(), profile).has_value());
    BOOST_CHECK(ValidateMarketForProfile(M2(), profile).has_value());
    const MarketDescriptor native_quote{TestAsset("TOK"), AssetId::Native(), 1, 1};
    const MarketDescriptor other_quote{AssetId::Native(), TestAsset("other-rUSD"), 1, 1};
    for (const MarketDescriptor& market : {native_quote, other_quote}) {
        const auto denied{ValidateMarketForProfile(market, profile)};
        BOOST_REQUIRE(!denied.has_value());
        BOOST_CHECK(denied.error().code == ErrorCode::INVALID_INPUT);
        BOOST_CHECK_EQUAL(denied.error().detail, STABLE_QUOTE_REQUIRED);
        BOOST_CHECK(ValidateMarketForProfile(market, DevRegtestProfile(ChainType::REGTEST).value()).has_value());
    }
    MarketDescriptor invalid{M1()};
    invalid.base_atoms_per_lot = 0;
    CheckError(ValidateMarketForProfile(invalid, profile), ErrorCode::INVALID_INPUT);
}

BOOST_AUTO_TEST_SUITE_END()
