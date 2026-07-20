// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/hardfork.h>
#include <consensus/params.h>

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

BOOST_AUTO_TEST_SUITE_END()
