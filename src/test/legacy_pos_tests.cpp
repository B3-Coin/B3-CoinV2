// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <legacy/consensus.h>
#include <legacy/pos.h>

#include <chain.h>

#include <boost/test/unit_test.hpp>

#include <array>

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

BOOST_AUTO_TEST_CASE(validates_the_first_historical_pos_kernel)
{
    // Block 136 on the B3Coin main chain. This is the first historical PoS
    // block encountered during initial sync and anchors the real kernel data.
    const legacy::pos::KernelInput input{
        .stake_modifier = 0xbee43f6d062b61cd,
        .source_block_time = 1'482'055'707,
        .source_transaction_offset = 174,
        .source_transaction_time = 1'482'055'237,
        .source_output_index = 1,
        .stake_time = 1'482'088'579,
    };

    const auto result{legacy::pos::EvaluateKernel(input, 0x1e0fffff,
        23'095 * legacy::pos::Params::COIN)};
    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->proof_hash.GetHex(),
        "019634c6378f4747330deb6735f0009f92e282140f22237b8b13b233f96e36e5");
    BOOST_CHECK_EQUAL(result->coin_day_weight, 7'950U);
    BOOST_CHECK(result->IsValid());
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

BOOST_AUTO_TEST_CASE(stake_modifier_uses_historical_numeric_hash_order)
{
    // The old B3Coin uint256 type compared hashes as numeric little-endian
    // values, unlike Core's bytewise uint256 ordering. These values make the
    // first selection differ if the latter is accidentally used.
    std::array<CBlockIndex, 4> indexes{};
    const std::array<uint256, 4> block_hashes{uint256{1}, uint256{2}, uint256{3}, uint256{4}};
    constexpr std::array<uint32_t, 4> block_times{0, 4'400, 4'500, 36'000};

    for (size_t i{0}; i < indexes.size(); ++i) {
        CBlockIndex& index{indexes[i]};
        index.phashBlock = &block_hashes[i];
        index.nHeight = static_cast<int>(i);
        index.nTime = block_times[i];
        index.pprev = i == 0 ? nullptr : &indexes[i - 1];
    }
    indexes[0].m_legacy_stake_modifier_generated = true;
    indexes[1].m_legacy_hash_proof = uint256{};
    indexes[2].m_legacy_hash_proof = uint256{3};
    indexes[3].m_legacy_hash_proof = uint256{};

    uint64_t stake_modifier{0};
    bool generated{false};
    BOOST_REQUIRE(legacy::ComputeNextStakeModifier(&indexes.back(), stake_modifier, generated));
    BOOST_CHECK(generated);
    BOOST_CHECK_EQUAL(stake_modifier, 2U);
}

BOOST_AUTO_TEST_CASE(preserves_historical_special_burn_fee_accounting)
{
    const CAmount output{10 * COIN};

    BOOST_CHECK_EQUAL(
        legacy::GetLegacyTransactionFee(output + legacy::LEGACY_FUNDAMENTALNODE_BURN, output, /*is_coinstake=*/false),
        0);
    BOOST_CHECK_EQUAL(
        legacy::GetLegacyTransactionFee(output + legacy::LEGACY_FUNDAMENTALNODE_BURN + COIN, output, /*is_coinstake=*/false),
        COIN);
    BOOST_CHECK_EQUAL(
        legacy::GetLegacyTransactionFee(output + COIN, output, /*is_coinstake=*/true),
        0);
}

BOOST_AUTO_TEST_SUITE_END()
