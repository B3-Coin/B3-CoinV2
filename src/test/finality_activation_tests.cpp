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
#include <kernel/chainparams.h>
#include <modern/fn_genesis_validation.h>
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
        if (chain == ChainType::MAIN) {
            // v1 release shape (owner rulings 2026-08-26/27): H = 810,000 is
            // PINNED and the node is in the OD-10 pause-fail-closed state --
            // X and the Modern-PoS parameters remain unset, so it accepts
            // the legacy chain through H and refuses every block above it.
            BOOST_CHECK_EQUAL(c.hard_fork_height.value_or(0), 810'001); // first non-legacy height
            BOOST_CHECK_EQUAL(Consensus::LegacyFinalHeight(c).value_or(0), 810'000); // H
            BOOST_CHECK(!c.legacy_final_hash.has_value());
            BOOST_CHECK(c.fn_genesis_required);
            BOOST_CHECK(!c.fn_genesis_rights_root.has_value());
            BOOST_CHECK(c.fn_genesis_manifest.empty());
            BOOST_CHECK(Consensus::LegacyBoundaryHeightOnly(c)); // the PAUSE state
            BOOST_CHECK(!Consensus::LegacyBoundaryPinned(c));
            BOOST_CHECK_EQUAL(Consensus::ModernPosStartHeight(c).value_or(0), 811'001); // M
        } else {
            // Every OTHER shipped network stays fully unpinned.
            BOOST_CHECK(!c.hard_fork_height.has_value());
            BOOST_CHECK(!c.legacy_final_hash.has_value());
            BOOST_CHECK(!Consensus::ModernPosStartHeight(c).has_value());
            BOOST_CHECK(!c.fn_genesis_required);
            BOOST_CHECK(!c.fn_genesis_rights_root.has_value());
            BOOST_CHECK(c.fn_genesis_manifest.empty());
        }
        BOOST_CHECK(!c.modern_pos.has_value());
        BOOST_CHECK(!c.flowmesh_activation_height.has_value());
        BOOST_CHECK(!Consensus::ModernObjectRulesActive(c));
        // Cells: every policy type inactive.
        for (const uint16_t t : {6, 7, 8}) BOOST_CHECK(!modern::IsMetadataCellActive(t, modern::POLICY_VERSION_V1, c));
        // MPA: every type inactive or unknown; an MPA-bearing tx is invalid.
        for (const uint16_t t : {1, 2, 3, 4, 5, 6, 7}) {
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
    // types 4/5 (version 1 only). Types 1..3 and the separately scheduled
    // FlowMesh type 9 stay known-but-inactive here; unknown stays unknown;
    // the 80-byte policy-state bound is untouched.
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
    BOOST_CHECK(modern::GetPayloadTypeStatus(9, 1, c) == modern::PayloadTypeStatus::INACTIVE);
    BOOST_CHECK_EQUAL(modern::MAX_POLICY_PARAMS_SIZE, 80U);
    // The modern-PoS start (F = M) derives from the same pins.
    BOOST_CHECK_EQUAL(*Consensus::ModernPosStartHeight(c), 99 + 1 + c.transition_pow_length);
}

BOOST_AUTO_TEST_CASE(fn_and_asset_schedule_is_complete_ordered_and_post_modern)
{
    Consensus::Params c{};
    c.legacy_b3coin = true;
    c.hard_fork_height = 101; // H=100
    c.legacy_final_hash = uint256::ONE;
    c.modern_pos.emplace();
    c.transition_pow_length = 10; // M=111

    BOOST_CHECK(!Consensus::FnAssetActivationScheduleConfigured(c));
    c.fn_pod_activation_height = 120;
    BOOST_CHECK(!Consensus::FnAssetActivationScheduleConfigured(c));
    c.asset_activation_height = 130;
    BOOST_CHECK(Consensus::FnAssetActivationScheduleConfigured(c));

    c.fn_pod_activation_height = 111;
    BOOST_CHECK(!Consensus::FnAssetActivationScheduleConfigured(c)); // A1 must be > M
    c.fn_pod_activation_height = 120;
    c.asset_activation_height = 119;
    BOOST_CHECK(!Consensus::FnAssetActivationScheduleConfigured(c)); // A2 cannot precede A1

    c.asset_activation_height = 120;
    BOOST_CHECK(Consensus::FnAssetActivationScheduleConfigured(c)); // A1 == A2 is valid
}

BOOST_AUTO_TEST_CASE(flowmesh_release_regtest_schedule_is_complete_and_isolated)
{
    CChainParams::RegTestOptions options;
    CChainParams::B3ModernRegTestOptions b3;
    b3.flowmesh_test = true;
    options.b3_modern = b3;
    const auto params{CChainParams::RegTest(options)};
    const Consensus::Params& c{params->GetConsensus()};

    BOOST_REQUIRE(c.hard_fork_height.has_value());
    BOOST_REQUIRE(c.modern_pos.has_value());
    BOOST_REQUIRE(c.fn_genesis_rights_root.has_value());
    BOOST_CHECK(c.fn_genesis_required);
    BOOST_CHECK_EQUAL(c.fn_genesis_manifest.size(),
                      Consensus::HISTORICAL_FN_PROVEN_FLOOR);
    std::string manifest_error;
    BOOST_CHECK(modern::CheckFnGenesisConfiguration(c, manifest_error));

    const int modern_start{*c.hard_fork_height + b3.corridor_length};
    BOOST_REQUIRE(c.fn_pod_activation_height.has_value());
    BOOST_REQUIRE(c.asset_activation_height.has_value());
    BOOST_REQUIRE(c.flowmesh_activation_height.has_value());
    BOOST_CHECK_EQUAL(*c.fn_pod_activation_height, modern_start + 1);
    BOOST_CHECK_EQUAL(*c.asset_activation_height, modern_start + 2);
    BOOST_CHECK_EQUAL(*c.flowmesh_activation_height,
                      *c.asset_activation_height +
                          Consensus::FLOWMESH_ANCHOR_DEPTH);
    BOOST_CHECK(Consensus::FlowMeshSeatBindingRulesActive(
        *c.asset_activation_height, c));
    BOOST_CHECK(Consensus::FlowMeshVaultPreparationRulesActive(
        *c.asset_activation_height, c));
    BOOST_CHECK(!Consensus::FlowMeshRulesActive(
        *c.flowmesh_activation_height - 1, c));
    BOOST_CHECK(Consensus::FlowMeshRulesActive(
        *c.flowmesh_activation_height, c));
    BOOST_CHECK(!c.modern_pos->treasury_script.empty());

    // The ordinary modern-regtest configuration remains untouched and has no
    // synthetic rights, feature heights, or treasury destination.
    CChainParams::RegTestOptions plain_options;
    plain_options.b3_modern.emplace();
    const auto plain_params{CChainParams::RegTest(plain_options)};
    const Consensus::Params& plain{plain_params->GetConsensus()};
    BOOST_CHECK(!plain.fn_genesis_required);
    BOOST_CHECK(plain.fn_genesis_manifest.empty());
    BOOST_CHECK(!plain.fn_pod_activation_height.has_value());
    BOOST_CHECK(!plain.asset_activation_height.has_value());
    BOOST_CHECK(!plain.flowmesh_activation_height.has_value());
    BOOST_REQUIRE(plain.modern_pos.has_value());
    BOOST_CHECK(plain.modern_pos->treasury_script.empty());
}

BOOST_AUTO_TEST_SUITE_END()
