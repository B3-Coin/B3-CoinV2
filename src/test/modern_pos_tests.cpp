// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <chainparams.h>
#include <consensus/modern_pos_params.h>
#include <consensus/params.h>
#include <util/chaintype.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(modern_pos_tests, BasicTestingSetup)

//! The provisional-parameter guard demanded by the frozen V1 spec (§9):
//! no shipped network may configure the REVISABLE_BEFORE_MAINNET modern-PoS
//! block, and no shipped network may carry a test-only injection point.
//! While modern_pos is unset, modern-PoS validation and production fail
//! closed, so a forgotten ratification cannot silently activate scaffolding
//! numbers on a real chain.
BOOST_AUTO_TEST_CASE(no_provisional_parameters_on_shipped_networks)
{
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4,
                                  ChainType::SIGNET, ChainType::REGTEST}) {
        const auto params{CreateChainParams(*m_node.args, chain)};
        const Consensus::Params& consensus{params->GetConsensus()};
        BOOST_CHECK_MESSAGE(!consensus.modern_pos.has_value(),
                            "modern_pos configured on a shipped network");
        BOOST_CHECK_MESSAGE(consensus.test_only_modern_pos_validator == nullptr,
                            "test-only PoS validator set on a shipped network");
        BOOST_CHECK_MESSAGE(!consensus.test_only_asset_policies_active,
                            "test-only asset activation set on a shipped network");
        BOOST_CHECK_MESSAGE(!consensus.min_stake_amount.has_value(),
                            "provisional stake minimum set on a shipped network");
        BOOST_CHECK_MESSAGE(!consensus.transition_pow_bits.has_value(),
                            "provisional corridor difficulty set on a shipped network");
    }
}

//! Structural sanity of the provisional defaults themselves.
BOOST_AUTO_TEST_CASE(provisional_parameter_block_is_structurally_valid)
{
    Consensus::ModernPosParams pos{};
    BOOST_CHECK(pos.Valid());
    BOOST_CHECK(!pos.reorg_horizon.has_value()); // D is an owner decision: no default.

    pos.round_seconds = 0;
    BOOST_CHECK(!pos.Valid());
    pos = Consensus::ModernPosParams{};
    pos.f0_den = 0;
    BOOST_CHECK(!pos.Valid());
    pos = Consensus::ModernPosParams{};
    pos.reorg_horizon = 0;
    BOOST_CHECK(!pos.Valid());
}

BOOST_AUTO_TEST_SUITE_END()
