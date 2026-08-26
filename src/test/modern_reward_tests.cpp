// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// OD-2 reward schedule (owner rulings 2026-08-26): R0 with yearly halving
// from M, treasury share of the subsidy only, corridor carries no subsidy.

#include <consensus/modern_pos_params.h>

#include <boost/test/unit_test.hpp>

using Consensus::ModernBlockSubsidy;
using Consensus::ModernPosParams;
using Consensus::ModernTreasuryShare;

BOOST_AUTO_TEST_SUITE(modern_reward_tests)

BOOST_AUTO_TEST_CASE(halving_schedule)
{
    ModernPosParams p{};
    p.reward = 1'000'000;        // R0
    p.halving_interval = 525'600; // ruled: one year
    const int M{811'001};        // ruled: first modern height

    BOOST_CHECK_EQUAL(ModernBlockSubsidy(M, M, p), 1'000'000);
    BOOST_CHECK_EQUAL(ModernBlockSubsidy(M + 525'599, M, p), 1'000'000); // last block of year 1
    BOOST_CHECK_EQUAL(ModernBlockSubsidy(M + 525'600, M, p), 500'000);   // first halving
    BOOST_CHECK_EQUAL(ModernBlockSubsidy(M + 2 * 525'600, M, p), 250'000);
    BOOST_CHECK_EQUAL(ModernBlockSubsidy(M + 10 * 525'600, M, p), 976); // 1e6 >> 10
    BOOST_CHECK_EQUAL(ModernBlockSubsidy(M + 63 * 525'600, M, p), 0);   // shift saturates
    // Below M (the corridor) there is never a subsidy.
    BOOST_CHECK_EQUAL(ModernBlockSubsidy(M - 1, M, p), 0);
    // Zero R0 = fees only forever (the shipped fail-closed default).
    p.reward = 0;
    BOOST_CHECK_EQUAL(ModernBlockSubsidy(M, M, p), 0);
    // No halving interval = flat reward.
    p.reward = 777;
    p.halving_interval = 0;
    BOOST_CHECK_EQUAL(ModernBlockSubsidy(M + 10'000'000, M, p), 777);
}

BOOST_AUTO_TEST_CASE(treasury_share)
{
    ModernPosParams p{};
    p.treasury_percent = 10;
    p.treasury_script = {0x76, 0xa9, 0x14, 0x12, 0x60, 0x24, 0x18, 0xff, 0xc7, 0x46, 0x40,
                         0xe3, 0x7f, 0x1a, 0x73, 0xd0, 0xcd, 0xc2, 0x55, 0xd2, 0xa0, 0x7c,
                         0x35, 0x88, 0xac}; // the ruled treasury P2PKH
    BOOST_CHECK_EQUAL(ModernTreasuryShare(1'000'000, p), 100'000);
    BOOST_CHECK_EQUAL(ModernTreasuryShare(999, p), 99);   // floor
    BOOST_CHECK_EQUAL(ModernTreasuryShare(9, p), 0);      // floor to zero
    BOOST_CHECK_EQUAL(ModernTreasuryShare(0, p), 0);
    // Disabled configurations yield no enforced share.
    ModernPosParams off{};
    off.treasury_percent = 0;
    BOOST_CHECK_EQUAL(ModernTreasuryShare(1'000'000, off), 0);
    ModernPosParams noscript{};
    noscript.treasury_percent = 10;
    BOOST_CHECK_EQUAL(ModernTreasuryShare(1'000'000, noscript), 0);
}

BOOST_AUTO_TEST_CASE(params_validity)
{
    ModernPosParams p{};
    BOOST_CHECK(p.Valid()); // defaults ship fail-closed and valid
    p.treasury_percent = 101;
    BOOST_CHECK(!p.Valid());
    p.treasury_percent = 10;
    BOOST_CHECK(!p.Valid()); // percent without script is invalid
    p.treasury_script = {0x51};
    BOOST_CHECK(p.Valid());
    p.halving_interval = -1;
    BOOST_CHECK(!p.Valid());
}

BOOST_AUTO_TEST_SUITE_END()
