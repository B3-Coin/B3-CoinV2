// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <legacy/pos.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(legacy_pos_tests)

BOOST_AUTO_TEST_CASE(kernel_serialization_matches_old_chain)
{
    const legacy::pos::KernelInput input{
        .stake_modifier = 0x0102030405060708,
        .source_block_time = 1481667355,
        .source_transaction_offset = 1234,
        .source_transaction_time = 1481667355,
        .source_output_index = 2,
        .stake_time = 1481674555,
    };

    BOOST_CHECK_EQUAL(legacy::pos::ComputeKernelHash(input).GetHex(),
        "bdad6b08bea71027b572844c9bb43eba5854cd4f9c241e9d17f99cc6bad0d0ba");
}

BOOST_AUTO_TEST_CASE(enforces_old_chain_age_and_timestamp_rules)
{
    legacy::pos::KernelInput input{
        .stake_modifier = 1,
        .source_block_time = 1'000,
        .source_transaction_offset = 80,
        .source_transaction_time = 1'000,
        .source_output_index = 0,
        .stake_time = 1'000 + legacy::pos::Params::MIN_AGE,
    };

    BOOST_CHECK(!legacy::pos::EvaluateKernel(input, 0x1e0fffff, 10'000'000));
    ++input.stake_time;
    BOOST_CHECK(!legacy::pos::EvaluateKernel(input, 0x1e0fffff, 10'000'000));

    input.stake_time += 24 * 60 * 60;
    const auto result{legacy::pos::EvaluateKernel(input, 0x1e0fffff, 10'000'000)};
    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->coin_day_weight, 10U);

    BOOST_CHECK(legacy::pos::CheckCoinStakeTimestamp(10, 10));
    BOOST_CHECK(!legacy::pos::CheckCoinStakeTimestamp(10, 11));
}

BOOST_AUTO_TEST_SUITE_END()
