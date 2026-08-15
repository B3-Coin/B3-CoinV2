// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/era.h>
#include <consensus/params.h>

#include <boost/test/unit_test.hpp>

using Consensus::B3Era;
using Consensus::ConsensusPhase;
using Consensus::GetB3Era;
using Consensus::GetConsensusPhase;
using Consensus::TransitionPowFinalHeight;

BOOST_AUTO_TEST_SUITE(consensus_phase_tests)

//! The three-phase partition on a boundary-configured B3 chain with a
//! corridor: LEGACY_POS through H, TRANSITION_POW for exactly
//! transition_pow_length blocks, MODERN_POS from H+length+1 onward. The
//! era (format) dimension stays two-valued: corridor blocks are MODERN.
BOOST_AUTO_TEST_CASE(three_phase_partition_with_corridor)
{
    Consensus::Params params;
    params.legacy_b3coin = true;
    const int H{1'000'000};
    params.hard_fork_height = H + 1;
    params.transition_pow_length = 1000;

    BOOST_CHECK(GetConsensusPhase(0, params) == ConsensusPhase::LEGACY_POS);
    BOOST_CHECK(GetConsensusPhase(H, params) == ConsensusPhase::LEGACY_POS);
    BOOST_CHECK(GetConsensusPhase(H + 1, params) == ConsensusPhase::TRANSITION_POW);
    BOOST_CHECK(GetConsensusPhase(H + 1000, params) == ConsensusPhase::TRANSITION_POW);
    BOOST_CHECK(GetConsensusPhase(H + 1001, params) == ConsensusPhase::MODERN_POS);

    // Format/era dimension: every corridor block is a MODERN-format block.
    BOOST_CHECK(GetB3Era(H, params) == B3Era::LEGACY);
    BOOST_CHECK(GetB3Era(H + 1, params) == B3Era::MODERN);
    BOOST_CHECK(GetB3Era(H + 1000, params) == B3Era::MODERN);
    BOOST_CHECK(GetB3Era(H + 1001, params) == B3Era::MODERN);

    BOOST_REQUIRE(TransitionPowFinalHeight(params).has_value());
    BOOST_CHECK_EQUAL(*TransitionPowFinalHeight(params), H + 1000);
}

//! Zero corridor length preserves the two-phase behavior exactly: modern
//! PoS directly after H, and no corridor final height exists.
BOOST_AUTO_TEST_CASE(zero_length_corridor_is_two_phase)
{
    Consensus::Params params;
    params.legacy_b3coin = true;
    params.hard_fork_height = 35;

    BOOST_CHECK(GetConsensusPhase(34, params) == ConsensusPhase::LEGACY_POS);
    BOOST_CHECK(GetConsensusPhase(35, params) == ConsensusPhase::MODERN_POS);
    BOOST_CHECK(!TransitionPowFinalHeight(params).has_value());
}

//! Without a configured boundary a legacy-B3 chain is in live legacy
//! operation at every height; the corridor length alone activates nothing.
BOOST_AUTO_TEST_CASE(unpinned_boundary_stays_legacy)
{
    Consensus::Params params;
    params.legacy_b3coin = true;
    params.transition_pow_length = 1000;

    BOOST_CHECK(GetConsensusPhase(0, params) == ConsensusPhase::LEGACY_POS);
    BOOST_CHECK(GetConsensusPhase(10'000'000, params) == ConsensusPhase::LEGACY_POS);
    BOOST_CHECK(!TransitionPowFinalHeight(params).has_value());
}

//! Chains without a legacy B3 history never have a corridor regardless of
//! the configured length.
BOOST_AUTO_TEST_CASE(non_b3_chains_are_modern_pos_everywhere)
{
    Consensus::Params params;
    params.legacy_b3coin = false;
    params.transition_pow_length = 1000;
    params.hard_fork_height = 100;

    BOOST_CHECK(GetConsensusPhase(0, params) == ConsensusPhase::MODERN_POS);
    BOOST_CHECK(GetConsensusPhase(150, params) == ConsensusPhase::MODERN_POS);
    BOOST_CHECK(!TransitionPowFinalHeight(params).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
