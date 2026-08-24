// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 18 of the Modern PoS V1 finality plan: F = M activation plumbing.
// The modern object rules (metadata-cell policies 6/7/8, MPA types 4/5)
// activate through ONE predicate -- Consensus::ModernObjectRulesActive: the
// legacy boundary H set, X pinned, and the Modern-PoS rule set present (the
// X-pin release configuration). Every real network ships without them and
// is fail-closed; nothing activates early; known-but-inactive types stay
// invalid regardless.

#include <chainparams.h>
#include <common/args.h>
#include <consensus/era.h>
#include <consensus/params.h>
#include <modern/metadata_cell.h>
#include <modern/mpa.h>
#include <modern/policy.h>
#include <primitives/transaction.h>
#include <uint256.h>
#include <util/chaintype.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(finality_activation_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(mainnet_and_all_shipped_networks_fail_closed)
{
    for (const auto chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4, ChainType::REGTEST, ChainType::SIGNET}) {
        const auto params{CreateChainParams(ArgsManager{}, chain)};
        const Consensus::Params& c{params->GetConsensus()};
        // The X / M parameters are deliberately NOT pinned anywhere yet.
        BOOST_CHECK(!c.hard_fork_height.has_value());
        BOOST_CHECK(!c.legacy_final_hash.has_value());
        BOOST_CHECK(!c.modern_pos.has_value());
        BOOST_CHECK(!Consensus::ModernObjectRulesActive(c));
        BOOST_CHECK(!Consensus::ModernPosStartHeight(c).has_value());
        // Cells: every policy type inactive.
        for (const uint16_t t : {6, 7, 8}) BOOST_CHECK(!modern::IsMetadataCellActive(t, modern::POLICY_VERSION_V1, c));
        // MPA: every type inactive or unknown; an MPA-bearing tx is invalid.
        for (const uint16_t t : {1, 2, 3, 4, 5}) {
            BOOST_CHECK(modern::GetPayloadTypeStatus(t, 1, c) != modern::PayloadTypeStatus::ACTIVE);
        }
        CMutableTransaction m;
        m.version = 2;
        m.vin.resize(1);
        m.vout.emplace_back(0, CScript() << OP_TRUE);
        CMpaRecord rec;
        rec.payload_type = modern::MPA_TYPE_FINALITY_KEY_EVIDENCE;
        rec.payload_version = modern::MPA_VERSION_V1;
        rec.payload.assign(modern::FINALITY_KEY_EVIDENCE_SIZE, 0);
        m.mpa = {rec};
        std::string err;
        BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{m}, c, err));
        BOOST_CHECK_EQUAL(err, "mpa-not-active");
    }
}

BOOST_AUTO_TEST_CASE(activation_is_exactly_the_x_pin_configuration)
{
    Consensus::Params c{};
    c.legacy_b3coin = true;
    // Each ingredient alone is not enough: H, X and the rule set must all be pinned.
    BOOST_CHECK(!Consensus::ModernObjectRulesActive(c));
    c.hard_fork_height = 100;
    BOOST_CHECK(!Consensus::ModernObjectRulesActive(c));
    c.legacy_final_hash = uint256::ONE;
    BOOST_CHECK(!Consensus::ModernObjectRulesActive(c));
    c.modern_pos = Consensus::ModernPosParams{};
    BOOST_CHECK(Consensus::ModernObjectRulesActive(c));
    // A non-B3 chain can never activate, whatever else is set.
    Consensus::Params vanilla{c};
    vanilla.legacy_b3coin = false;
    BOOST_CHECK(!Consensus::ModernObjectRulesActive(vanilla));

    // Active exactly where specified: policies 6/7/8 (version 1 only), MPA
    // types 4/5 (version 1 only). Types 1..3 are OTHER features and stay
    // known-but-inactive even under the full configuration; unknown stays
    // unknown; the 80-byte policy-state bound is untouched.
    for (const uint16_t t : {6, 7, 8}) {
        BOOST_CHECK(modern::IsMetadataCellActive(t, modern::POLICY_VERSION_V1, c));
        BOOST_CHECK(!modern::IsMetadataCellActive(t, 2, c));
    }
    BOOST_CHECK(!modern::IsMetadataCellActive(5, modern::POLICY_VERSION_V1, c));
    for (const uint16_t t : {4, 5}) {
        BOOST_CHECK(modern::GetPayloadTypeStatus(t, 1, c) == modern::PayloadTypeStatus::ACTIVE);
        BOOST_CHECK(modern::GetPayloadTypeStatus(t, 2, c) == modern::PayloadTypeStatus::UNKNOWN);
    }
    for (const uint16_t t : {1, 2, 3}) {
        BOOST_CHECK(modern::GetPayloadTypeStatus(t, 1, c) == modern::PayloadTypeStatus::INACTIVE);
    }
    BOOST_CHECK(modern::GetPayloadTypeStatus(9, 1, c) == modern::PayloadTypeStatus::UNKNOWN);
    BOOST_CHECK_EQUAL(modern::MAX_POLICY_PARAMS_SIZE, 80U);
    // The modern-PoS start (F = M) derives from the same pins.
    BOOST_CHECK_EQUAL(*Consensus::ModernPosStartHeight(c), 99 + 1 + c.transition_pow_length);
}

BOOST_AUTO_TEST_SUITE_END()
