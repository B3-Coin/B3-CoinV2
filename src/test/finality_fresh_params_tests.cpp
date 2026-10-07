// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <chainparams.h>
#include <chainparamsbase.h>
#include <common/args.h>
#include <consensus/era.h>
#include <consensus/params.h>
#include <kernel/chainparams.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Fresh finality lineage startup knobs (bridge V2 B-7/B-8; regtest plan
// section 3.3): -b3freshlineageheight and -b3freshmaxepochextension are
// REGTEST ONLY, need the FlowMesh regtest, put R_f - 1 on the V1 checkpoint
// grid, and leave every other network's consensus untouched.
BOOST_FIXTURE_TEST_SUITE(finality_fresh_params_tests, BasicTestingSetup)

namespace {

constexpr const char* HEIGHT_KNOB{"-b3freshlineageheight"};
constexpr const char* EXTENSION_KNOB{"-b3freshmaxepochextension"};

void SetModes(ArgsManager& args, bool modern, bool flowmesh)
{
    if (modern) args.ForceSetArg("-b3modernregtest", "1");
    if (flowmesh) args.ForceSetArg("-b3flowmeshtest", "1");
}

//! The FlowMesh regtest with the given fresh knobs (empty = unset).
std::unique_ptr<const CChainParams> FlowmeshRegtest(const std::string& height,
                                                    const std::string& extension = "",
                                                    const std::vector<std::pair<std::string, std::string>>& extra = {})
{
    ArgsManager args;
    SetModes(args, true, true);
    if (!height.empty()) args.ForceSetArg(HEIGHT_KNOB, height);
    if (!extension.empty()) args.ForceSetArg(EXTENSION_KNOB, extension);
    for (const auto& [name, value] : extra) args.ForceSetArg(name, value);
    return CreateChainParams(args, ChainType::REGTEST);
}

} // namespace

BOOST_AUTO_TEST_CASE(knobs_are_refused_outside_regtest)
{
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET,
                                  ChainType::TESTNET4, ChainType::SIGNET}) {
        for (const char* knob : {HEIGHT_KNOB, EXTENSION_KNOB}) {
            const std::string reason{std::string{knob} + " is regtest-only"};
            // The knob alone.
            ArgsManager bare;
            bare.ForceSetArg(knob, "172");
            BOOST_CHECK_EXCEPTION(CreateChainParams(bare, chain), std::runtime_error,
                                  HasReason(reason));
            // Together with the FlowMesh regtest switches, which do not
            // apply to this network either.
            ArgsManager flagged;
            SetModes(flagged, true, true);
            flagged.ForceSetArg(knob, "172");
            BOOST_CHECK_EXCEPTION(CreateChainParams(flagged, chain), std::runtime_error,
                                  HasReason(reason));
        }
        // Both knobs at once: the first is reported.
        ArgsManager both;
        SetModes(both, true, true);
        both.ForceSetArg(HEIGHT_KNOB, "172");
        both.ForceSetArg(EXTENSION_KNOB, "40");
        BOOST_CHECK_EXCEPTION(CreateChainParams(both, chain), std::runtime_error,
                              HasReason("-b3freshlineageheight is regtest-only"));

        // Without the knobs the network has no fresh lineage at any height.
        const auto params{CreateChainParams(ArgsManager{}, chain)};
        BOOST_CHECK(!params->GetConsensus().fresh_lineage);
        BOOST_CHECK(!params->GetConsensus().FreshLineageRulesActive(std::numeric_limits<int>::max()));
    }
}

BOOST_AUTO_TEST_CASE(knobs_require_flowmesh_regtest)
{
    for (const char* knob : {HEIGHT_KNOB, EXTENSION_KNOB}) {
        const std::string reason{std::string{knob} +
                                 " requires -b3modernregtest and -b3flowmeshtest (regtest-only)"};
        for (const auto& [modern, flowmesh] : {std::pair{false, false}, std::pair{true, false},
                                               std::pair{false, true}}) {
            ArgsManager args;
            SetModes(args, modern, flowmesh);
            args.ForceSetArg(knob, "172");
            BOOST_CHECK_EXCEPTION(CreateChainParams(args, ChainType::REGTEST), std::runtime_error,
                                  HasReason(reason));
        }
    }

    // The kernel refuses the same configuration when RegTest() is called
    // directly with options that bypass the argument reader.
    CChainParams::RegTestOptions opts{};
    CChainParams::B3ModernRegTestOptions b3{};
    b3.flowmesh_test = false;
    b3.fresh_lineage_height = 172;
    opts.b3_modern = b3;
    BOOST_CHECK_EXCEPTION(CChainParams::RegTest(opts), std::runtime_error,
                          HasReason("-b3freshlineageheight requires -b3flowmeshtest"));

    // A negated knob is rejected by the argument parser itself.
    for (const char* negated : {"-nob3freshlineageheight", "-nob3freshmaxepochextension"}) {
        ArgsManager parsed;
        SetupChainParamsBaseOptions(parsed);
        const char* argv[]{"b3coind", "-regtest", negated};
        std::string error;
        BOOST_CHECK(!parsed.ParseParameters(3, argv, error));
        BOOST_CHECK(HasReason(std::string{negated}.substr(3))(error));
    }
}

BOOST_AUTO_TEST_CASE(extension_requires_height)
{
    BOOST_CHECK_EXCEPTION(FlowmeshRegtest("", "40"), std::runtime_error,
                          HasReason("-b3freshmaxepochextension requires -b3freshlineageheight"));

    CChainParams::RegTestOptions opts{};
    CChainParams::B3ModernRegTestOptions b3{};
    b3.flowmesh_test = true;
    b3.fresh_max_epoch_extension = 40;
    opts.b3_modern = b3;
    BOOST_CHECK_EXCEPTION(CChainParams::RegTest(opts), std::runtime_error,
                          HasReason("-b3freshmaxepochextension requires -b3freshlineageheight"));
}

BOOST_AUTO_TEST_CASE(knob_values_are_range_checked)
{
    for (const std::string value : {"0", "1", "-5", "abc", "2147483648"}) {
        BOOST_CHECK_EXCEPTION(FlowmeshRegtest(value), std::runtime_error,
                              HasReason("Invalid value for -b3freshlineageheight"));
    }
    for (const std::string value : {"0", "-1", "abc", "2147483648"}) {
        BOOST_CHECK_EXCEPTION(FlowmeshRegtest("172", value), std::runtime_error,
                              HasReason("Invalid value for -b3freshmaxepochextension"));
    }
    // The kernel holds the same bounds for direct callers.
    CChainParams::RegTestOptions opts{};
    CChainParams::B3ModernRegTestOptions b3{};
    b3.flowmesh_test = true;
    b3.fresh_lineage_height = 1;
    opts.b3_modern = b3;
    BOOST_CHECK_EXCEPTION(CChainParams::RegTest(opts), std::runtime_error,
                          HasReason("Invalid value for -b3freshlineageheight"));
    b3.fresh_lineage_height = 172;
    b3.fresh_max_epoch_extension = 0;
    opts.b3_modern = b3;
    BOOST_CHECK_EXCEPTION(CChainParams::RegTest(opts), std::runtime_error,
                          HasReason("Invalid value for -b3freshmaxepochextension"));
}

BOOST_AUTO_TEST_CASE(genesis_must_be_a_v1_grid_checkpoint)
{
    // Default FlowMesh regtest: H = 0, corridor 160, so M = 161; interval 5.
    {
        const auto chain{FlowmeshRegtest("")};
        const Consensus::Params& params{chain->GetConsensus()};
        BOOST_REQUIRE_EQUAL(*Consensus::ModernPosStartHeight(params), 161);
        BOOST_REQUIRE_EQUAL(params.modern_pos->checkpoint_interval, 5);
        BOOST_CHECK(!params.fresh_lineage);
    }

    // G = 262 is off the 161 + 5k grid; G = 160 lies below M.
    BOOST_CHECK_EXCEPTION(FlowmeshRegtest("263"), std::runtime_error,
                          HasReason("-b3freshlineageheight=263: genesis checkpoint 262 must be a checkpoint height >= M=161 on the 5-block grid"));
    BOOST_CHECK_EXCEPTION(FlowmeshRegtest("161"), std::runtime_error,
                          HasReason("-b3freshlineageheight=161: genesis checkpoint 160 must be a checkpoint height >= M=161 on the 5-block grid"));
    BOOST_CHECK_EXCEPTION(FlowmeshRegtest("2"), std::runtime_error,
                          HasReason("genesis checkpoint 1 must be a checkpoint height >= M=161"));

    // G = M is the first checkpoint height.
    {
        const auto chain{FlowmeshRegtest("162")};
        const auto& fresh{chain->GetConsensus().fresh_lineage};
        BOOST_REQUIRE(fresh);
        BOOST_CHECK_EQUAL(fresh->activation_height, 162);
        BOOST_CHECK_EQUAL(fresh->GenesisHeight(), 161);
        BOOST_CHECK_EQUAL(fresh->max_epoch_extension, 1'280);
        BOOST_CHECK(fresh->staging_signer);
    }

    // G = 171: the default extension is regtest profile R's 1,280.
    const auto chain{FlowmeshRegtest("172")};
    const Consensus::Params& params{chain->GetConsensus()};
    BOOST_REQUIRE(params.fresh_lineage);
    BOOST_CHECK_EQUAL(params.fresh_lineage->activation_height, 172);
    BOOST_CHECK_EQUAL(params.fresh_lineage->GenesisHeight(), 171);
    BOOST_CHECK_EQUAL(params.fresh_lineage->max_epoch_extension,
                      CChainParams::B3ModernRegTestOptions::DEFAULT_FRESH_MAX_EPOCH_EXTENSION);
    BOOST_CHECK_EQUAL(params.fresh_lineage->max_epoch_extension, 1'280);
    BOOST_CHECK(params.fresh_lineage->staging_signer);
    BOOST_CHECK(!params.FreshLineageRulesActive(171));
    BOOST_CHECK(params.FreshLineageRulesActive(172));
    BOOST_CHECK(params.FreshLineageRulesActive(173));

    // The V1 lineage parameters are untouched by the knob.
    const auto plain{FlowmeshRegtest("")};
    const Consensus::Params& v1{plain->GetConsensus()};
    BOOST_CHECK(params.hashGenesisBlock == v1.hashGenesisBlock);
    BOOST_CHECK_EQUAL(params.modern_pos->max_epoch_extension, v1.modern_pos->max_epoch_extension);
    BOOST_CHECK_EQUAL(params.modern_pos->checkpoint_interval, v1.modern_pos->checkpoint_interval);
    BOOST_CHECK_EQUAL(params.modern_pos->finality_epoch_blocks, v1.modern_pos->finality_epoch_blocks);
    BOOST_CHECK(*params.flowmesh_activation_height == *v1.flowmesh_activation_height);
    BOOST_CHECK(chain->MessageStart() == plain->MessageStart());

    // An explicit fresh extension changes only the fresh lineage.
    const auto extended{FlowmeshRegtest("172", "40")};
    BOOST_REQUIRE(extended->GetConsensus().fresh_lineage);
    BOOST_CHECK_EQUAL(extended->GetConsensus().fresh_lineage->max_epoch_extension, 40);
    BOOST_CHECK_EQUAL(extended->GetConsensus().modern_pos->max_epoch_extension,
                      v1.modern_pos->max_epoch_extension);

    // The grid follows the configured corridor and interval: the functional
    // fixture's corridor 130 gives M = 131 and R_f = 262 (G = 261 = 131 + 26*5).
    const auto fixture_i{FlowmeshRegtest("262", "1280", {{"-b3corridorlength", "130"}})};
    BOOST_REQUIRE(fixture_i->GetConsensus().fresh_lineage);
    BOOST_CHECK_EQUAL(fixture_i->GetConsensus().fresh_lineage->GenesisHeight(), 261);
    BOOST_CHECK_EXCEPTION(FlowmeshRegtest("167", "", {{"-b3checkpointinterval", "10"}}), std::runtime_error,
                          HasReason("-b3freshlineageheight=167: genesis checkpoint 166 must be a checkpoint height >= M=161 on the 10-block grid"));
    BOOST_CHECK(FlowmeshRegtest("172", "", {{"-b3checkpointinterval", "10"}})->GetConsensus().fresh_lineage);
}

BOOST_AUTO_TEST_CASE(parsed_command_line_reaches_consensus)
{
    ArgsManager parsed;
    SetupChainParamsBaseOptions(parsed);
    const char* argv[]{"b3coind", "-regtest", "-b3modernregtest", "-b3flowmeshtest",
                       "-b3freshlineageheight=172", "-b3freshmaxepochextension=40"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(parsed.ParseParameters(6, argv, error), error);
    const auto chain{CreateChainParams(parsed, ChainType::REGTEST)};
    const auto& fresh{chain->GetConsensus().fresh_lineage};
    BOOST_REQUIRE(fresh);
    BOOST_CHECK_EQUAL(fresh->activation_height, 172);
    BOOST_CHECK_EQUAL(fresh->max_epoch_extension, 40);
    BOOST_CHECK(fresh->staging_signer);
}

BOOST_AUTO_TEST_CASE(fresh_lineage_is_unset_by_default)
{
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4,
                                  ChainType::SIGNET, ChainType::REGTEST}) {
        const auto params{CreateChainParams(ArgsManager{}, chain)};
        BOOST_CHECK(!params->GetConsensus().fresh_lineage);
        BOOST_CHECK(!params->GetConsensus().FreshLineageRulesActive(0));
        BOOST_CHECK(!params->GetConsensus().FreshLineageRulesActive(std::numeric_limits<int>::max()));
    }
    // The modern and FlowMesh regtests without the knob have none either.
    ArgsManager modern;
    SetModes(modern, true, false);
    BOOST_CHECK(!CreateChainParams(modern, ChainType::REGTEST)->GetConsensus().fresh_lineage);
    BOOST_CHECK(!FlowmeshRegtest("")->GetConsensus().fresh_lineage);
}

BOOST_AUTO_TEST_SUITE_END()
