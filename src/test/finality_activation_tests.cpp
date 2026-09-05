// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 18 of the Modern PoS V1 finality plan: F = M activation plumbing.
// The modern object rules (metadata-cell policies 6/7/8, MPA types 4/5)
// activate through ONE predicate -- Consensus::ModernObjectRulesActive: the
// legacy boundary H set, X pinned, and the Modern-PoS rule set present (the
// X-pin release configuration). Mainnet now carries that exact sealed
// configuration and its independently pinned FN PoD/assets/FlowMesh heights;
// the still-incomplete bUSD security envelope remains fail-closed.

#include <chainparams.h>
#include <common/args.h>
#include <consensus/boundary.h>
#include <consensus/era.h>
#include <consensus/params.h>
#include <kernel/chainparams.h>
#include <modern/bridge_binding.h>
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

BOOST_AUTO_TEST_CASE(mainnet_sealed_transition_pins_are_complete)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const Consensus::Params& c{params->GetConsensus()};

    BOOST_CHECK_EQUAL(c.hard_fork_height.value_or(0), 810'001);
    BOOST_CHECK_EQUAL(Consensus::LegacyFinalHeight(c).value_or(0), 810'000);
    BOOST_CHECK(c.legacy_final_hash == uint256{
        "2413ba59476afb9a01b971c350b2c5a51494b37925055be42dde774f30d865c6"});
    BOOST_CHECK(Consensus::LegacyBoundaryPinned(c));
    BOOST_CHECK(!Consensus::LegacyBoundaryHeightOnly(c));
    BOOST_CHECK_EQUAL(Consensus::ModernPosStartHeight(c).value_or(0), 811'001);
    BOOST_CHECK_EQUAL(c.legacy_checkpoints.count(810'001), 0U);
    BOOST_REQUIRE_EQUAL(c.modern_checkpoints.size(), 2U);
    BOOST_REQUIRE_EQUAL(c.modern_checkpoints.count(810'001), 1U);
    BOOST_CHECK(c.modern_checkpoints.at(810'001) == uint256{
        "913fb38c75e0f12d8d5e6ea65a0ffce33a22a6908392a94661eab7c8506f6014"});
    BOOST_REQUIRE_EQUAL(c.modern_checkpoints.count(811'641), 1U);
    BOOST_CHECK(c.modern_checkpoints.at(811'641) == uint256{
        "5dbb0e582be41444933d43c9dda576f15a2922a870c3fb9d1c47b84b473b1f75"});

    BOOST_CHECK(c.fn_genesis_required);
    BOOST_CHECK_EQUAL(c.fn_genesis_manifest_version, 1);
    BOOST_CHECK_EQUAL(c.fn_genesis_manifest.size(), 3'592U);
    BOOST_CHECK(c.fn_genesis_rights_root == uint256{
        "e8f282a7dcaa9a8fbcfcc5c22ba4f456e5b50968fcf899aaacdaca65bef898ec"});
    std::string manifest_error;
    BOOST_CHECK_MESSAGE(modern::CheckFnGenesisConfiguration(c, manifest_error),
                        manifest_error);
    BOOST_CHECK(!Consensus::FnRulesActive(810'000, c));
    BOOST_CHECK(Consensus::FnRulesActive(810'001, c));

    BOOST_REQUIRE(c.modern_pos.has_value());
    BOOST_CHECK(c.modern_pos->Valid());
    BOOST_CHECK(Consensus::ModernObjectRulesActive(c));
    for (const uint16_t t : {6, 7, 8}) {
        BOOST_CHECK(modern::IsMetadataCellActive(
            t, modern::POLICY_VERSION_V1, c));
    }
    for (const uint16_t t : {4, 5}) {
        BOOST_CHECK(modern::GetPayloadTypeStatus(t, 1, c) ==
                    modern::PayloadTypeStatus::ACTIVE);
    }

    // The independently ratified feature schedule is exact: A1 turns on FN
    // PoD, A2 turns on assets plus seat/vault preparation, and A3 turns on
    // FlowMesh after a 2,000-block (well above 30-block) anchor runway.
    BOOST_REQUIRE(c.fn_pod_activation_height.has_value());
    BOOST_REQUIRE(c.asset_activation_height.has_value());
    BOOST_REQUIRE(c.flowmesh_activation_height.has_value());
    BOOST_CHECK_EQUAL(*c.fn_pod_activation_height, 812'000);
    BOOST_CHECK_EQUAL(*c.asset_activation_height, 813'000);
    BOOST_CHECK_EQUAL(*c.flowmesh_activation_height, 815'000);
    BOOST_CHECK_EQUAL(*c.flowmesh_activation_height -
                          *c.asset_activation_height,
                      2'000);
    BOOST_CHECK_GE(*c.flowmesh_activation_height -
                       *c.asset_activation_height,
                   Consensus::FLOWMESH_ANCHOR_DEPTH);
    BOOST_CHECK(Consensus::FnAssetActivationScheduleConfigured(c));
    BOOST_CHECK(!Consensus::FnPodRulesActive(811'999, c));
    BOOST_CHECK(Consensus::FnPodRulesActive(812'000, c));
    BOOST_CHECK(!Consensus::AssetRulesActive(812'999, c));
    BOOST_CHECK(Consensus::AssetRulesActive(813'000, c));
    BOOST_CHECK(!Consensus::FlowMeshSeatBindingRulesActive(812'999, c));
    BOOST_CHECK(Consensus::FlowMeshSeatBindingRulesActive(813'000, c));
    BOOST_CHECK(Consensus::FlowMeshVaultPreparationRulesActive(813'000, c));
    BOOST_CHECK(!Consensus::FlowMeshRulesActive(814'999, c));
    BOOST_CHECK(Consensus::FlowMeshRulesActive(815'000, c));
    BOOST_CHECK(modern::GetPayloadTypeStatus(3, 1, c, 812'999) ==
                modern::PayloadTypeStatus::INACTIVE);
    BOOST_CHECK(modern::GetPayloadTypeStatus(3, 1, c, 813'000) ==
                modern::PayloadTypeStatus::ACTIVE);
    BOOST_CHECK(modern::GetPayloadTypeStatus(6, 1, c, 811'999) ==
                modern::PayloadTypeStatus::INACTIVE);
    BOOST_CHECK(modern::GetPayloadTypeStatus(6, 1, c, 812'000) ==
                modern::PayloadTypeStatus::ACTIVE);
    BOOST_CHECK(modern::GetPayloadTypeStatus(7, 1, c, 812'999) ==
                modern::PayloadTypeStatus::INACTIVE);
    BOOST_CHECK(modern::GetPayloadTypeStatus(7, 1, c, 813'000) ==
                modern::PayloadTypeStatus::ACTIVE);
    for (const uint16_t t : {8, 9}) {
        BOOST_CHECK(modern::GetPayloadTypeStatus(t, 1, c, 814'999) ==
                    modern::PayloadTypeStatus::INACTIVE);
        BOOST_CHECK(modern::GetPayloadTypeStatus(t, 1, c, 815'000) ==
                    modern::PayloadTypeStatus::ACTIVE);
    }

    // The bridge has its own complete security envelope and activates at the
    // independently pinned first modern-PoS height, before FlowMesh trading.
    BOOST_REQUIRE(c.busd_bridge.has_value());
    BOOST_REQUIRE(c.busd_bridge->activation_height.has_value());
    BOOST_CHECK_EQUAL(*c.busd_bridge->activation_height, 811'001);
    BOOST_CHECK(Consensus::BridgeMintParamsReady(*c.busd_bridge));
    BOOST_REQUIRE(c.bridge_withdrawal_activation_height.has_value());
    BOOST_CHECK_EQUAL(*c.bridge_withdrawal_activation_height, 811'001);
    BOOST_CHECK(!Consensus::BridgeRulesActive(811'000, c));
    BOOST_CHECK(Consensus::BridgeRulesActive(811'001, c));
    BOOST_CHECK(!Consensus::BridgeWithdrawalRulesActive(811'000, c));
    BOOST_CHECK(Consensus::BridgeWithdrawalRulesActive(811'001, c));
    BOOST_CHECK(modern::IsMetadataCellActive(
        static_cast<uint16_t>(modern::PolicyType::BRIDGE_RECORD),
        modern::POLICY_VERSION_V1, c, 815'000));
    BOOST_CHECK(modern::GetPayloadTypeStatus(10, 1, c, 815'000) ==
                modern::PayloadTypeStatus::ACTIVE);
    for (const uint16_t t : {1, 2}) {
        BOOST_CHECK(modern::GetPayloadTypeStatus(t, 1, c, 815'000) ==
                    modern::PayloadTypeStatus::INACTIVE);
    }
}

BOOST_AUTO_TEST_CASE(mainnet_first_corridor_checkpoint_accepts_only_exact_hash)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const Consensus::Params& c{params->GetConsensus()};
    const uint256 exact{
        "913fb38c75e0f12d8d5e6ea65a0ffce33a22a6908392a94661eab7c8506f6014"};
    const uint256 wrong{
        "913fb38c75e0f12d8d5e6ea65a0ffce33a22a6908392a94661eab7c8506f6015"};

    BOOST_CHECK(Consensus::ModernCheckpointAllows(c, 810'001, exact));
    BOOST_CHECK(!Consensus::ModernCheckpointAllows(c, 810'001, wrong));
    // Unpinned modern heights remain unconstrained, and this modern-only
    // table cannot accidentally override attested legacy history.
    BOOST_CHECK(Consensus::ModernCheckpointAllows(c, 810'002, wrong));
    BOOST_CHECK(Consensus::ModernCheckpointAllows(c, 810'000, wrong));
}

BOOST_AUTO_TEST_CASE(mainnet_recovery_anchor_checkpoint_accepts_only_exact_hash)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const Consensus::Params& c{params->GetConsensus()};
    const uint256 exact{
        "5dbb0e582be41444933d43c9dda576f15a2922a870c3fb9d1c47b84b473b1f75"};
    const uint256 wrong{
        "5dbb0e582be41444933d43c9dda576f15a2922a870c3fb9d1c47b84b473b1f74"};

    BOOST_CHECK(Consensus::ModernCheckpointAllows(c, 811'641, exact));
    BOOST_CHECK(!Consensus::ModernCheckpointAllows(c, 811'641, wrong));
}

BOOST_AUTO_TEST_CASE(other_shipped_networks_remain_fail_closed)
{
    for (const auto chain : {ChainType::TESTNET, ChainType::TESTNET4,
                             ChainType::REGTEST, ChainType::SIGNET}) {
        const auto params{CreateChainParams(ArgsManager{}, chain)};
        const Consensus::Params& c{params->GetConsensus()};
        BOOST_CHECK(!c.hard_fork_height.has_value());
        BOOST_CHECK(!c.legacy_final_hash.has_value());
        BOOST_CHECK(!Consensus::ModernPosStartHeight(c).has_value());
        BOOST_CHECK(!c.fn_genesis_required);
        BOOST_CHECK(!c.fn_genesis_rights_root.has_value());
        BOOST_CHECK(c.fn_genesis_manifest.empty());
        BOOST_CHECK(c.modern_checkpoints.empty());
        BOOST_CHECK(!c.modern_pos.has_value());
        BOOST_CHECK(!c.flowmesh_activation_height.has_value());
        BOOST_CHECK(!Consensus::ModernObjectRulesActive(c));
        // Cells: every policy type inactive.
        for (const uint16_t t : {6, 7, 8, 9}) BOOST_CHECK(!modern::IsMetadataCellActive(t, modern::POLICY_VERSION_V1, c));
        // MPA: every type inactive or unknown; an MPA-bearing tx is invalid.
        for (const uint16_t t : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}) {
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
    // FlowMesh type 9 and bridge type 10 stay known-but-inactive here; the
    // bridge binding policy 9 also remains height-gated; unknown stays
    // unknown; the 80-byte policy-state bound is untouched.
    for (const uint16_t t : {6, 7, 8}) {
        BOOST_CHECK(modern::IsMetadataCellActive(t, modern::POLICY_VERSION_V1, c));
        BOOST_CHECK(!modern::IsMetadataCellActive(t, 2, c));
    }
    BOOST_CHECK(!modern::IsMetadataCellActive(5, modern::POLICY_VERSION_V1, c));
    BOOST_CHECK(!modern::IsMetadataCellActive(
        static_cast<uint16_t>(modern::PolicyType::BRIDGE_RECORD),
        modern::POLICY_VERSION_V1, c));
    for (const uint16_t t : {4, 5}) {
        BOOST_CHECK(modern::GetPayloadTypeStatus(t, 1, c) == modern::PayloadTypeStatus::ACTIVE);
        BOOST_CHECK(modern::GetPayloadTypeStatus(t, 2, c) == modern::PayloadTypeStatus::UNKNOWN);
    }
    for (const uint16_t t : {1, 2, 3}) {
        BOOST_CHECK(modern::GetPayloadTypeStatus(t, 1, c) == modern::PayloadTypeStatus::INACTIVE);
    }
    BOOST_CHECK(modern::GetPayloadTypeStatus(9, 1, c) == modern::PayloadTypeStatus::INACTIVE);
    BOOST_CHECK(modern::GetPayloadTypeStatus(10, 1, c) == modern::PayloadTypeStatus::INACTIVE);
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
    BOOST_REQUIRE(c.busd_bridge);
    BOOST_REQUIRE(c.busd_bridge->activation_height);
    const int bridge_activation{*c.busd_bridge->activation_height};
    BOOST_CHECK_EQUAL(bridge_activation, *c.flowmesh_activation_height);
    BOOST_CHECK(Consensus::BridgeMintParamsReady(*c.busd_bridge));
    BOOST_CHECK(!Consensus::BridgeRulesActive(bridge_activation - 1, c));
    BOOST_CHECK(Consensus::BridgeRulesActive(bridge_activation, c));
    BOOST_REQUIRE(c.bridge_withdrawal_activation_height);
    BOOST_CHECK_EQUAL(*c.bridge_withdrawal_activation_height,
                      bridge_activation);
    BOOST_CHECK(!Consensus::BridgeWithdrawalRulesActive(
        bridge_activation - 1, c));
    BOOST_CHECK(Consensus::BridgeWithdrawalRulesActive(bridge_activation, c));
    BOOST_CHECK(modern::GetPayloadTypeStatus(
                    modern::CREATION_ACTION_BRIDGE, modern::MPA_VERSION_V1,
                    c, bridge_activation - 1) ==
                modern::PayloadTypeStatus::INACTIVE);
    BOOST_CHECK(modern::GetPayloadTypeStatus(
                    modern::CREATION_ACTION_BRIDGE, modern::MPA_VERSION_V1,
                    c, bridge_activation) ==
                modern::PayloadTypeStatus::ACTIVE);

    CMpaRecord bridge_record;
    bridge_record.payload_type = modern::CREATION_ACTION_BRIDGE;
    bridge_record.payload_version = modern::MPA_VERSION_V1;
    bridge_record.payload = {0x01};
    const auto bridge_binding{
        modern::MakeBridgeBindingOutput(bridge_record)};
    BOOST_REQUIRE(bridge_binding);
    CMutableTransaction bridge_tx;
    bridge_tx.vout.push_back(*bridge_binding);
    std::string bridge_error;
    BOOST_CHECK(!modern::CheckMetadataCellOutputs(
        CTransaction{bridge_tx}, c, bridge_activation - 1, bridge_error));
    BOOST_CHECK(bridge_error.find("inactive") != std::string::npos);
    BOOST_CHECK(modern::CheckMetadataCellOutputs(
        CTransaction{bridge_tx}, c, bridge_activation, bridge_error));
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

BOOST_AUTO_TEST_CASE(bridge_activation_is_independent_of_flowmesh_a3)
{
    CChainParams::RegTestOptions options;
    CChainParams::B3ModernRegTestOptions b3;
    b3.flowmesh_test = true;
    options.b3_modern = b3;
    auto params{CChainParams::RegTest(options)};
    auto& c{const_cast<Consensus::Params&>(params->GetConsensus())};

    const int modern_start{*Consensus::ModernPosStartHeight(c)};
    const int flowmesh_start{*c.flowmesh_activation_height};
    BOOST_REQUIRE_LT(modern_start, flowmesh_start);
    BOOST_REQUIRE(c.busd_bridge);
    BOOST_REQUIRE(Consensus::BridgeMintParamsReady(*c.busd_bridge));

    c.busd_bridge->activation_height = modern_start;
    BOOST_CHECK(!Consensus::BridgeRulesActive(modern_start - 1, c));
    BOOST_CHECK(Consensus::BridgeRulesActive(modern_start, c));
    // Inbound proofs/mints can start at B while irreversible burns remain
    // closed until the separately pinned W height.
    BOOST_CHECK(!Consensus::BridgeWithdrawalRulesActive(modern_start, c));
    BOOST_CHECK(!Consensus::FlowMeshRulesActive(modern_start, c));
    BOOST_CHECK(Consensus::BridgeRulesActive(flowmesh_start - 1, c));
    BOOST_CHECK(!Consensus::BridgeWithdrawalRulesActive(
        flowmesh_start - 1, c));
    BOOST_CHECK(Consensus::FlowMeshRulesActive(flowmesh_start, c));
    BOOST_CHECK(Consensus::BridgeRulesActive(flowmesh_start, c));
    BOOST_CHECK(Consensus::BridgeWithdrawalRulesActive(flowmesh_start, c));

    c.bridge_withdrawal_activation_height.reset();
    BOOST_CHECK(Consensus::BridgeRulesActive(flowmesh_start, c));
    BOOST_CHECK(!Consensus::BridgeWithdrawalRulesActive(flowmesh_start, c));
    c.bridge_withdrawal_activation_height = modern_start - 1;
    BOOST_CHECK(!Consensus::BridgeWithdrawalRulesActive(flowmesh_start, c));
    c.bridge_withdrawal_activation_height = modern_start;
    BOOST_CHECK(Consensus::BridgeWithdrawalRulesActive(modern_start, c));

    // A complete bridge envelope cannot opt into the PoW corridor.
    c.busd_bridge->activation_height = modern_start - 1;
    BOOST_CHECK(!Consensus::BridgeRulesActive(modern_start - 1, c));
    BOOST_CHECK(!Consensus::BridgeRulesActive(flowmesh_start, c));
}

BOOST_AUTO_TEST_SUITE_END()
