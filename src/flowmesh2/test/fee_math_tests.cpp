// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/fee_math.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace flowmesh2;

namespace {

template <class T>
void CheckError(const Result<T>& result, ErrorCode expected)
{
    BOOST_REQUIRE_MESSAGE(!result.has_value(), "unexpected success");
    BOOST_CHECK_EQUAL(ToString(result.error().code), ToString(expected));
}

//! SplitMix64 with a fixed seed: deterministic test randomness, no globals.
class SplitMix
{
public:
    explicit SplitMix(uint64_t seed) : m_state{seed} {}
    uint64_t Next()
    {
        uint64_t z{m_state += 0x9e3779b97f4a7c15ULL};
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    //! Uniform enough in [0, bound] for test inputs.
    uint64_t Below(uint64_t bound) { return bound == UINT64_MAX ? Next() : Next() % (bound + 1); }

private:
    uint64_t m_state;
};

//! ceil(q * ppm / 1e6) in plain 64-bit arithmetic, written independently of
//! MulDivCeil: q = qh * 1e6 + ql, so every product stays below 2^64 for q <= 2^62.
uint64_t ReferenceCeil(uint64_t q, uint64_t ppm)
{
    const uint64_t d{1'000'000};
    const uint64_t qh{q / d};
    const uint64_t ql{q % d};
    // q * ppm / d = qh * ppm + ql * ppm / d
    const uint64_t whole{qh * ppm};
    const uint64_t rem_product{ql * ppm}; // < 1e6 * 1e6 = 1e12
    return whole + rem_product / d + (rem_product % d != 0 ? 1 : 0);
}

} // namespace

BOOST_AUTO_TEST_SUITE(fee_math_tests)

BOOST_AUTO_TEST_CASE(walking_skeleton_values)
{
    // ARCHITECTURE.md 14.2 slot 3: Bob's seller fee on 500,000 quote atoms at
    // 100 ppm is 50; the slot split at 80 % gives the seat pool 40 and the treasury 10.
    BOOST_CHECK_EQUAL(SellerFeeTotal(500'000, 100).value(), 50);
    BOOST_CHECK_EQUAL(SellerFeeIncrement(0, 500'000, 100).value(), 50);
    BOOST_CHECK(SplitSlotFees(50, 80).value() == (FeeSplit{40, 10}));
    BOOST_CHECK_EQUAL(SellerFeeTotal(1, 100).value(), 1); // rounded up, against the actor
    BOOST_CHECK_EQUAL(SellerFeeTotal(0, 100).value(), 0);
}

BOOST_AUTO_TEST_CASE(increments_sum_to_the_fee_of_the_total)
{
    SplitMix rng{0x464d32'52345f'46ULL};
    for (int order{0}; order < 5'000; ++order) {
        const uint64_t range{order % 10 == 0 ? static_cast<uint64_t>(MAX_ATOMS) : uint64_t{1'000'000'000'000}};
        const Atoms total{static_cast<Atoms>(rng.Below(range))};
        const uint32_t ppm{order % 7 == 0 ? 100U : static_cast<uint32_t>(rng.Below(1'000'000))};
        const size_t fills{1 + static_cast<size_t>(rng.Below(49))};

        // Random cut points of [0, total] into `fills` pieces.
        std::vector<Atoms> cuts{0};
        for (size_t i{1}; i < fills; ++i) cuts.push_back(static_cast<Atoms>(rng.Below(static_cast<uint64_t>(total))));
        cuts.push_back(total);
        std::sort(cuts.begin(), cuts.end());

        Atoms sum{0};
        for (size_t i{1}; i < cuts.size(); ++i) {
            const Atoms increment{SellerFeeIncrement(cuts[i - 1], cuts[i], ppm).value()};
            const uint64_t piece{static_cast<uint64_t>(cuts[i] - cuts[i - 1])};
            // floor(piece fee) <= increment <= ceil(piece fee): never more than
            // a fill's own rounded-up fee, never less than its rounded-down fee.
            BOOST_REQUIRE(static_cast<uint64_t>(increment) <= ReferenceCeil(piece, ppm));
            BOOST_REQUIRE(static_cast<uint64_t>(increment) + 1 >= ReferenceCeil(piece, ppm));
            sum += increment;
        }
        BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(sum), ReferenceCeil(static_cast<uint64_t>(total), ppm));
        BOOST_REQUIRE_EQUAL(sum, SellerFeeTotal(total, ppm).value());
    }
}

BOOST_AUTO_TEST_CASE(split_exhaustive_0_to_10000)
{
    for (Atoms fees{0}; fees <= 10'000; ++fees) {
        for (uint32_t percent{0}; percent <= 100; ++percent) {
            const FeeSplit split{SplitSlotFees(fees, percent).value()};
            BOOST_REQUIRE(split.seat_pool >= 0 && split.treasury >= 0);
            BOOST_REQUIRE_EQUAL(split.seat_pool + split.treasury, fees);
            // seat_pool = floor(fees * percent / 100).
            BOOST_REQUIRE(split.seat_pool * 100 <= fees * percent);
            BOOST_REQUIRE((split.seat_pool + 1) * 100 > fees * percent);
        }
    }
}

BOOST_AUTO_TEST_CASE(edges_and_refusals)
{
    // Extremes stay exact (128-bit intermediates).
    BOOST_CHECK_EQUAL(SellerFeeTotal(MAX_ATOMS, 1'000'000).value(), MAX_ATOMS);
    BOOST_CHECK_EQUAL(SellerFeeTotal(MAX_ATOMS, 0).value(), 0);
    BOOST_CHECK_EQUAL(SellerFeeTotal(MAX_ATOMS, 100).value(), static_cast<Atoms>(ReferenceCeil(MAX_ATOMS, 100)));
    BOOST_CHECK(SplitSlotFees(MAX_ATOMS, 80).value() == (FeeSplit{MAX_ATOMS / 100 * 80 + (MAX_ATOMS % 100) * 80 / 100,
                                                                  MAX_ATOMS - (MAX_ATOMS / 100 * 80 + (MAX_ATOMS % 100) * 80 / 100)}));
    BOOST_CHECK_EQUAL(SellerFeeIncrement(7, 7, 100).value(), 0);

    CheckError(SellerFeeTotal(-1, 100), ErrorCode::INVALID_INPUT);
    CheckError(SellerFeeTotal(MAX_ATOMS + 1, 100), ErrorCode::INVALID_INPUT);
    CheckError(SellerFeeTotal(1, 1'000'001), ErrorCode::INVALID_INPUT);
    CheckError(SellerFeeIncrement(10, 9, 100), ErrorCode::INVALID_INPUT);
    CheckError(SellerFeeIncrement(-1, 9, 100), ErrorCode::INVALID_INPUT);
    CheckError(SellerFeeIncrement(0, MAX_ATOMS + 1, 100), ErrorCode::INVALID_INPUT);
    CheckError(SplitSlotFees(-1, 80), ErrorCode::INVALID_INPUT);
    CheckError(SplitSlotFees(MAX_ATOMS + 1, 80), ErrorCode::INVALID_INPUT);
    CheckError(SplitSlotFees(10, 101), ErrorCode::INVALID_INPUT);
}

BOOST_AUTO_TEST_SUITE_END()
