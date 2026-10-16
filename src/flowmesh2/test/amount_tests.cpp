// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/amount.h>

#include <cstdint>
#include <limits>

using namespace flowmesh2;

namespace {

constexpr Atoms I64_MAX{std::numeric_limits<int64_t>::max()};
constexpr Atoms I64_MIN{std::numeric_limits<int64_t>::min()};
constexpr uint64_t U64_MAX{std::numeric_limits<uint64_t>::max()};

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

//! Naive reference: the largest q with q * d <= a * b, found by counting.
uint64_t NaiveFloor(uint64_t a, uint64_t b, uint64_t d)
{
    const uint64_t product{a * b};
    uint64_t q{0};
    while ((q + 1) * d <= product) ++q;
    return q;
}

//! Naive reference: the smallest q with q * d >= a * b, found by counting.
uint64_t NaiveCeil(uint64_t a, uint64_t b, uint64_t d)
{
    const uint64_t product{a * b};
    uint64_t q{0};
    while (q * d < product) ++q;
    return q;
}

} // namespace

BOOST_AUTO_TEST_SUITE(amount_tests)

BOOST_AUTO_TEST_CASE(max_atoms_is_two_to_the_62)
{
    static_assert(MAX_ATOMS == 4'611'686'018'427'387'904);
    BOOST_CHECK(InAtomRange(0));
    BOOST_CHECK(InAtomRange(MAX_ATOMS));
    BOOST_CHECK(!InAtomRange(MAX_ATOMS + 1));
    BOOST_CHECK(!InAtomRange(-1));
    BOOST_CHECK(!InAtomRange(I64_MIN));
    BOOST_CHECK(!InAtomRange(I64_MAX));
    CheckValue(CheckAtoms(MAX_ATOMS), MAX_ATOMS);
    CheckError(CheckAtoms(MAX_ATOMS + 1), ErrorCode::INVALID_INPUT);
    CheckError(CheckAtoms(-1), ErrorCode::INVALID_INPUT);
}

BOOST_AUTO_TEST_CASE(add_refuses_overflow_at_the_edges)
{
    CheckValue(AddAtoms(0, 0), Atoms{0});
    CheckValue(AddAtoms(MAX_ATOMS, 0), MAX_ATOMS);
    CheckValue(AddAtoms(0, MAX_ATOMS), MAX_ATOMS);
    CheckValue(AddAtoms(MAX_ATOMS - 1, 1), MAX_ATOMS);
    CheckValue(AddAtoms(MAX_ATOMS / 2, MAX_ATOMS / 2), MAX_ATOMS);
    CheckError(AddAtoms(MAX_ATOMS, 1), ErrorCode::ARITHMETIC_OVERFLOW);
    CheckError(AddAtoms(1, MAX_ATOMS), ErrorCode::ARITHMETIC_OVERFLOW);
    CheckError(AddAtoms(MAX_ATOMS, MAX_ATOMS), ErrorCode::ARITHMETIC_OVERFLOW);
    CheckError(AddAtoms(MAX_ATOMS / 2 + 1, MAX_ATOMS / 2), ErrorCode::ARITHMETIC_OVERFLOW);
}

BOOST_AUTO_TEST_CASE(add_refuses_operands_outside_the_range)
{
    CheckError(AddAtoms(-1, 0), ErrorCode::INVALID_INPUT);
    CheckError(AddAtoms(0, -1), ErrorCode::INVALID_INPUT);
    CheckError(AddAtoms(5, -3), ErrorCode::INVALID_INPUT); // would be 2 if negatives were allowed
    CheckError(AddAtoms(I64_MIN, I64_MIN), ErrorCode::INVALID_INPUT);
    CheckError(AddAtoms(MAX_ATOMS + 1, 0), ErrorCode::INVALID_INPUT);
    CheckError(AddAtoms(I64_MAX, 0), ErrorCode::INVALID_INPUT);
    CheckError(AddAtoms(I64_MAX, I64_MAX), ErrorCode::INVALID_INPUT);
}

BOOST_AUTO_TEST_CASE(sub_refuses_underflow_and_bad_operands)
{
    CheckValue(SubAtoms(0, 0), Atoms{0});
    CheckValue(SubAtoms(MAX_ATOMS, MAX_ATOMS), Atoms{0});
    CheckValue(SubAtoms(MAX_ATOMS, 0), MAX_ATOMS);
    CheckValue(SubAtoms(MAX_ATOMS, 1), MAX_ATOMS - 1);
    CheckError(SubAtoms(0, 1), ErrorCode::ARITHMETIC_OVERFLOW);
    CheckError(SubAtoms(MAX_ATOMS - 1, MAX_ATOMS), ErrorCode::ARITHMETIC_OVERFLOW);
    CheckError(SubAtoms(1, -1), ErrorCode::INVALID_INPUT); // would be 2 if negatives were allowed
    CheckError(SubAtoms(-1, 0), ErrorCode::INVALID_INPUT);
    CheckError(SubAtoms(I64_MIN, 0), ErrorCode::INVALID_INPUT);
    CheckError(SubAtoms(0, I64_MIN), ErrorCode::INVALID_INPUT);
    CheckError(SubAtoms(MAX_ATOMS + 1, 1), ErrorCode::INVALID_INPUT);
}

BOOST_AUTO_TEST_CASE(mul_checked_edges)
{
    CheckValue(MulChecked(0, U64_MAX), uint64_t{0});
    CheckValue(MulChecked(U64_MAX, 1), U64_MAX);
    CheckValue(MulChecked(4'294'967'295ULL, 4'294'967'297ULL), U64_MAX); // (2^32-1)(2^32+1)
    CheckError(MulChecked(4'294'967'296ULL, 4'294'967'296ULL), ErrorCode::ARITHMETIC_OVERFLOW);
    CheckError(MulChecked(U64_MAX, 2), ErrorCode::ARITHMETIC_OVERFLOW);
    CheckError(MulChecked(U64_MAX, U64_MAX), ErrorCode::ARITHMETIC_OVERFLOW);
}

BOOST_AUTO_TEST_CASE(muldiv_exhaustive_small_domain)
{
    // Every (a, b, d) with a, b in [0, 24] and d in [1, 24] against counting.
    size_t checked{0};
    for (uint64_t a{0}; a <= 24; ++a) {
        for (uint64_t b{0}; b <= 24; ++b) {
            for (uint64_t d{1}; d <= 24; ++d) {
                const auto floor{MulDivFloor(a, b, d)};
                const auto ceil{MulDivCeil(a, b, d)};
                BOOST_REQUIRE(floor.has_value() && ceil.has_value());
                if (*floor != NaiveFloor(a, b, d) || *ceil != NaiveCeil(a, b, d)) {
                    BOOST_ERROR("MulDiv mismatch at a=" << a << " b=" << b << " d=" << d);
                }
                ++checked;
            }
        }
    }
    BOOST_CHECK_EQUAL(checked, 25U * 25U * 24U);
    // A wider domain against plain 64-bit division (no overflow there).
    for (uint64_t a{0}; a <= 300; a += 7) {
        for (uint64_t b{0}; b <= 300; b += 11) {
            for (uint64_t d{1}; d <= 300; d += 13) {
                BOOST_REQUIRE(*MulDivFloor(a, b, d) == (a * b) / d);
                BOOST_REQUIRE(*MulDivCeil(a, b, d) == (a * b + d - 1) / d);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(muldiv_wide_intermediates)
{
    // Values computed independently with Python integers.
    CheckValue(MulDivFloor(U64_MAX, U64_MAX, U64_MAX), U64_MAX);
    CheckValue(MulDivCeil(U64_MAX, U64_MAX, U64_MAX), U64_MAX);
    CheckValue(MulDivFloor(U64_MAX, 2, 2), U64_MAX);
    CheckValue(MulDivCeil(U64_MAX, 2, 2), U64_MAX);
    CheckValue(MulDivFloor(U64_MAX - 1, U64_MAX, U64_MAX), U64_MAX - 1);
    CheckValue(MulDivCeil(U64_MAX - 1, U64_MAX, U64_MAX), U64_MAX - 1);
    CheckValue(MulDivFloor(3, 5, 7), uint64_t{2});
    CheckValue(MulDivCeil(3, 5, 7), uint64_t{3});
    CheckError(MulDivFloor(U64_MAX, U64_MAX, U64_MAX - 1), ErrorCode::ARITHMETIC_OVERFLOW);
    CheckError(MulDivCeil(U64_MAX, U64_MAX, U64_MAX - 1), ErrorCode::ARITHMETIC_OVERFLOW);
    CheckError(MulDivFloor(9'223'372'036'854'775'808ULL, 2, 1), ErrorCode::ARITHMETIC_OVERFLOW);
    CheckError(MulDivCeil(9'223'372'036'854'775'808ULL, 2, 1), ErrorCode::ARITHMETIC_OVERFLOW);
    CheckError(MulDivFloor(U64_MAX, 2, 1), ErrorCode::ARITHMETIC_OVERFLOW);
}

BOOST_AUTO_TEST_CASE(muldiv_refuses_zero_divisor)
{
    CheckError(MulDivFloor(1, 1, 0), ErrorCode::INVALID_INPUT);
    CheckError(MulDivCeil(0, 0, 0), ErrorCode::INVALID_INPUT);
}

BOOST_AUTO_TEST_SUITE_END()
