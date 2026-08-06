// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/hardfork.h>
#include <consensus/params.h>
#include <primitives/block.h>

#include <boost/test/unit_test.hpp>

#include <limits>

BOOST_AUTO_TEST_SUITE(hardfork_tests)

BOOST_AUTO_TEST_CASE(unset_height_disables_activation)
{
    const Consensus::Params params{};

    BOOST_CHECK(Consensus::GetEra(params, 0) == Consensus::Era::LEGACY);
    BOOST_CHECK(Consensus::GetEra(params, std::numeric_limits<int>::max()) == Consensus::Era::LEGACY);
    BOOST_CHECK(!Consensus::IsHardForkActive(params, 0));
}

BOOST_AUTO_TEST_CASE(activation_height_is_first_post_fork_block)
{
    Consensus::Params params{};
    params.hard_fork_height = 100;

    BOOST_CHECK(Consensus::GetEra(params, -1) == Consensus::Era::LEGACY);
    BOOST_CHECK(Consensus::GetEra(params, 0) == Consensus::Era::LEGACY);
    BOOST_CHECK(Consensus::GetEra(params, 99) == Consensus::Era::LEGACY);
    BOOST_CHECK(Consensus::GetEra(params, 100) == Consensus::Era::POST_HARD_FORK);
    BOOST_CHECK(Consensus::GetEra(params, 101) == Consensus::Era::POST_HARD_FORK);
    BOOST_CHECK(Consensus::IsHardForkActive(params, 100));
}

BOOST_AUTO_TEST_CASE(block_header_hash_switches_at_activation_height)
{
    CBlockHeader header;
    header.nVersion = 4;
    header.hashPrevBlock = uint256{"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
    header.hashMerkleRoot = uint256{"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"};
    header.nTime = 1'481'667'355;
    header.nBits = 0x1e0fffff;
    header.nNonce = 499'515;

    Consensus::Params params{};
    params.legacy_b3coin = true;
    params.hard_fork_height = 100;

    BOOST_CHECK(header.GetLegacyB3Hash() != header.GetHash());
    BOOST_CHECK_EQUAL(header.GetHash(params, 99).GetHex(), header.GetLegacyB3Hash().GetHex());
    BOOST_CHECK_EQUAL(header.GetHash(params, 100).GetHex(), header.GetHash().GetHex());
}

BOOST_AUTO_TEST_SUITE_END()
