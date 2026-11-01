// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <chain.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <common/args.h>
#include <consensus/amount.h>
#include <consensus/boundary.h>
#include <consensus/bridge_params.h>
#include <consensus/era.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <kernel/chainparams.h>
#include <modern/finality_schedule.h>
#include <modern/pos_v1.h>
#include <node/chainstate.h>
#include <node/derived_state.h>
#include <node/finality_tracker.h>
#include <node/miner.h>
#include <node/stake_tracker.h>
#include <node/validator_set.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/common.h>
#include <test/util/finality_fixture.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/strencodings.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// H1 producer rotation and H2 hardened checkpoints (bridge V2 finality
// dependency section 5, FD-1..FD-4; owner ruling R8). The startup knobs
// -b3h1height, -b3h1escaperounds and -b3h2checkpoint are REGTEST ONLY, need
// the FlowMesh regtest, and leave every other network's consensus
// untouched: no network but regtest sets Consensus::Params::h1, and
// mainnet's modern checkpoint table stays exactly {810001, 811641}. The
// regtest plan's -b3regtestwithholdcertificates is not provided: the
// argument parser refuses it as an unknown parameter.
//
// The chain cases (FinalityChainFixture, scaled E = 30) cover the producer
// schedule in node::FinalityTracker: V1 behaviour bit-identical below R_h
// and a producer set that differs from V1's from R_h, rotation every E from
// R_h without certificates on a broken V1 lineage, the bootstrap fallback
// and carry-over below MIN_FINALITY_SET, the deterministic rebuild, and the
// FD-2 escape: a block whose consensus-checked round is at least R_escape is
// judged against Snapshot(parent), whatever its size
// (Chainstate::ModernEligibilityWeights' round) and restarts the schedule,
// also at R_h itself and across restart and -reindex-chainstate. The local
// regtest production policy no longer pauses on the V1 lineage from R_h.
//
// H2 (finality dependency section 5.3, FD-3) adds no new mechanism: a pin is
// an entry in Consensus::Params::modern_checkpoints, enforced at header
// admission and in ConnectBlock (bad-modern-checkpoint) and against the
// active chain at startup. The H2 chain case pins a block above R_h from
// the -b3h2checkpoint display-order hash.
BOOST_FIXTURE_TEST_SUITE(finality_h1_tests, BasicTestingSetup)

namespace {

constexpr const char* H1_HEIGHT_KNOB{"-b3h1height"};
constexpr const char* H1_ESCAPE_KNOB{"-b3h1escaperounds"};
constexpr const char* H2_KNOB{"-b3h2checkpoint"};

//! A display-order block hash whose 64 hex digits all equal `digit`.
std::string HashHex(char digit) { return std::string(64, digit); }

//! One valid value for each knob, usable on its own.
std::vector<std::pair<const char*, std::string>> ValidKnobValues()
{
    return {{H1_HEIGHT_KNOB, "220"},
            {H1_ESCAPE_KNOB, "60"},
            {H2_KNOB, "300:" + HashHex('a')}};
}

void SetModes(ArgsManager& args, bool modern, bool flowmesh)
{
    if (modern) args.ForceSetArg("-b3modernregtest", "1");
    if (flowmesh) args.ForceSetArg("-b3flowmeshtest", "1");
}

//! The FlowMesh regtest with the given extra knobs (single values).
std::unique_ptr<const CChainParams> FlowmeshRegtest(const std::vector<std::pair<std::string, std::string>>& extra = {})
{
    ArgsManager args;
    SetModes(args, true, true);
    for (const auto& [name, value] : extra) args.ForceSetArg(name, value);
    return CreateChainParams(args, ChainType::REGTEST);
}

//! Parse a real command line (repeatable knobs, negation) and build the
//! chain it selects. Throws std::runtime_error("parse: <error>") when the
//! argument parser itself refuses the line.
std::unique_ptr<const CChainParams> ParsedChain(const std::vector<std::string>& extra, ChainType chain = ChainType::REGTEST)
{
    ArgsManager parsed;
    SetupChainParamsBaseOptions(parsed);
    std::vector<const char*> argv{"b3coind"};
    for (const std::string& arg : extra) argv.push_back(arg.c_str());
    std::string error;
    if (!parsed.ParseParameters(static_cast<int>(argv.size()), argv.data(), error)) {
        throw std::runtime_error("parse: " + error);
    }
    return CreateChainParams(parsed, chain);
}

//! The FlowMesh regtest command line plus `extra`.
std::vector<std::string> FlowmeshLine(std::vector<std::string> extra)
{
    std::vector<std::string> line{"-regtest", "-b3modernregtest", "-b3flowmeshtest"};
    line.insert(line.end(), extra.begin(), extra.end());
    return line;
}

const std::map<int, uint256> MAINNET_MODERN_CHECKPOINTS{
    {810'001, uint256{"913fb38c75e0f12d8d5e6ea65a0ffce33a22a6908392a94661eab7c8506f6014"}},
    {811'641, uint256{"5dbb0e582be41444933d43c9dda576f15a2922a870c3fb9d1c47b84b473b1f75"}},
};

} // namespace

// (a) Every knob is refused on every network but regtest, alone and with the
// FlowMesh regtest switches; without them those networks have no H1 and
// mainnet's hardened modern checkpoints are exactly the shipped two.
BOOST_AUTO_TEST_CASE(h1_knobs_refused_outside_regtest)
{
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET,
                                  ChainType::TESTNET4, ChainType::SIGNET}) {
        for (const auto& [knob, value] : ValidKnobValues()) {
            const std::string reason{std::string{knob} + " is regtest-only"};
            // The knob alone.
            ArgsManager bare;
            bare.ForceSetArg(knob, value);
            BOOST_CHECK_EXCEPTION(CreateChainParams(bare, chain), std::runtime_error,
                                  HasReason(reason));
            // Together with the FlowMesh regtest switches, which do not
            // apply to this network either.
            ArgsManager flagged;
            SetModes(flagged, true, true);
            flagged.ForceSetArg(knob, value);
            BOOST_CHECK_EXCEPTION(CreateChainParams(flagged, chain), std::runtime_error,
                                  HasReason(reason));
        }
        // All three at once: the first H1/H2 knob is reported.
        ArgsManager all;
        SetModes(all, true, true);
        for (const auto& [knob, value] : ValidKnobValues()) all.ForceSetArg(knob, value);
        BOOST_CHECK_EXCEPTION(CreateChainParams(all, chain), std::runtime_error,
                              HasReason("-b3h1height is regtest-only"));
        // A parsed config line naming the knob is refused as well.
        BOOST_CHECK_EXCEPTION(ParsedChain({"-b3h2checkpoint=300:" + HashHex('a')}, chain),
                              std::runtime_error, HasReason("-b3h2checkpoint is regtest-only"));

        // Without the knobs the network has no H1 at any height and no
        // escape round.
        const auto params{CreateChainParams(ArgsManager{}, chain)};
        const Consensus::Params& consensus{params->GetConsensus()};
        BOOST_CHECK(!consensus.h1);
        BOOST_CHECK(!consensus.H1RulesActive(0));
        BOOST_CHECK(!consensus.H1RulesActive(std::numeric_limits<int>::max()));
        BOOST_CHECK(!consensus.H1EscapeRound(std::numeric_limits<int>::max(),
                                             std::numeric_limits<int64_t>::max()));
        if (chain == ChainType::MAIN) {
            BOOST_CHECK(consensus.modern_checkpoints == MAINNET_MODERN_CHECKPOINTS);
        } else {
            BOOST_CHECK(consensus.modern_checkpoints.empty());
        }
    }
    // Plain regtest and the modern regtest without the knobs have none either.
    BOOST_CHECK(!CreateChainParams(ArgsManager{}, ChainType::REGTEST)->GetConsensus().h1);
    ArgsManager modern;
    SetModes(modern, true, false);
    BOOST_CHECK(!CreateChainParams(modern, ChainType::REGTEST)->GetConsensus().h1);
}

// (b) On regtest every knob needs both -b3modernregtest and -b3flowmeshtest;
// the kernel refuses the same configurations from direct callers.
BOOST_AUTO_TEST_CASE(h1_knobs_require_flowmesh_regtest)
{
    for (const auto& [knob, value] : ValidKnobValues()) {
        const std::string reason{std::string{knob} +
                                 " requires -b3modernregtest and -b3flowmeshtest (regtest-only)"};
        for (const auto& [modern, flowmesh] : {std::pair{false, false}, std::pair{true, false},
                                               std::pair{false, true}}) {
            ArgsManager args;
            SetModes(args, modern, flowmesh);
            args.ForceSetArg(knob, value);
            BOOST_CHECK_EXCEPTION(CreateChainParams(args, ChainType::REGTEST), std::runtime_error,
                                  HasReason(reason));
        }
    }

    CChainParams::RegTestOptions opts{};
    CChainParams::B3ModernRegTestOptions b3{};
    b3.flowmesh_test = false;
    b3.h1_height = 220;
    opts.b3_modern = b3;
    BOOST_CHECK_EXCEPTION(CChainParams::RegTest(opts), std::runtime_error,
                          HasReason("-b3h1height requires -b3flowmeshtest"));
    b3.h1_height.reset();
    b3.h2_checkpoints = {{300, uint256::ONE}};
    opts.b3_modern = b3;
    BOOST_CHECK_EXCEPTION(CChainParams::RegTest(opts), std::runtime_error,
                          HasReason("-b3h2checkpoint requires -b3flowmeshtest"));
}

// (c) Malformed, out-of-range and inconsistent values are refused.
BOOST_AUTO_TEST_CASE(h1_knob_values_are_checked)
{
    // Escape rounds without R_h.
    BOOST_CHECK_EXCEPTION(FlowmeshRegtest({{H1_ESCAPE_KNOB, "60"}}), std::runtime_error,
                          HasReason("-b3h1escaperounds requires -b3h1height"));
    {
        CChainParams::RegTestOptions opts{};
        CChainParams::B3ModernRegTestOptions b3{};
        b3.flowmesh_test = true;
        b3.h1_escape_rounds = 60;
        opts.b3_modern = b3;
        BOOST_CHECK_EXCEPTION(CChainParams::RegTest(opts), std::runtime_error,
                              HasReason("-b3h1escaperounds requires -b3h1height"));
    }

    // The argument reader refuses R_h below 2 and outside int.
    for (const std::string value : {"0", "1", "-5", "abc", "2147483648"}) {
        BOOST_CHECK_EXCEPTION(FlowmeshRegtest({{H1_HEIGHT_KNOB, value}}), std::runtime_error,
                              HasReason("Invalid value for -b3h1height: must be in [2, 2147483647]"));
    }
    // R_h must lie above M: default corridor 160 gives M = 161.
    for (const int value : {2, 100, 160, 161}) {
        BOOST_CHECK_EXCEPTION(FlowmeshRegtest({{H1_HEIGHT_KNOB, std::to_string(value)}}), std::runtime_error,
                              HasReason("Invalid value for -b3h1height: " + std::to_string(value) +
                                        " must be above the first modern-PoS height M=161"));
    }
    // The bound follows the corridor: corridor 130 gives M = 131.
    BOOST_CHECK_EXCEPTION(FlowmeshRegtest({{"-b3corridorlength", "130"}, {H1_HEIGHT_KNOB, "131"}}),
                          std::runtime_error,
                          HasReason("Invalid value for -b3h1height: 131 must be above the first modern-PoS height M=131"));
    BOOST_CHECK(FlowmeshRegtest({{"-b3corridorlength", "130"}, {H1_HEIGHT_KNOB, "132"}})->GetConsensus().h1);
    {
        // The kernel holds the same bound for direct callers.
        CChainParams::RegTestOptions opts{};
        CChainParams::B3ModernRegTestOptions b3{};
        b3.flowmesh_test = true;
        for (const int value : {-1, 0, 161}) {
            b3.h1_height = value;
            opts.b3_modern = b3;
            BOOST_CHECK_EXCEPTION(CChainParams::RegTest(opts), std::runtime_error,
                                  HasReason("Invalid value for -b3h1height: " + std::to_string(value) +
                                            " must be above the first modern-PoS height M=161"));
        }
    }

    // Escape rounds in [1, 99999]: the assembler searches rounds below
    // 100,000, so a larger R_escape could never be reached by it.
    for (const std::string value : {"0", "-1", "abc", "100000", "100001", "9223372036854775807"}) {
        BOOST_CHECK_EXCEPTION(FlowmeshRegtest({{H1_HEIGHT_KNOB, "220"}, {H1_ESCAPE_KNOB, value}}),
                              std::runtime_error,
                              HasReason("Invalid value for -b3h1escaperounds"));
    }
    BOOST_CHECK_EXCEPTION(FlowmeshRegtest({{H1_HEIGHT_KNOB, "220"}, {H1_ESCAPE_KNOB, "100001"}}),
                          std::runtime_error,
                          HasReason("Invalid value for -b3h1escaperounds: 100001 must be in [1, 99999]"));
    for (const int64_t rounds : {int64_t{1}, int64_t{99'999}}) {
        const auto chain{FlowmeshRegtest({{H1_HEIGHT_KNOB, "220"}, {H1_ESCAPE_KNOB, std::to_string(rounds)}})};
        BOOST_REQUIRE(chain->GetConsensus().h1);
        BOOST_CHECK_EQUAL(chain->GetConsensus().h1->escape_rounds, rounds);
    }
    BOOST_CHECK_EQUAL(CChainParams::B3ModernRegTestOptions::MAX_H1_ESCAPE_ROUNDS, 99'999);

    // Malformed H2 pins.
    const std::string hash{HashHex('a')};
    for (const std::string& value : {std::string{""}, std::string{"300"}, std::string{"300:"}, ":" + hash,
                                    "0:" + hash, "-1:" + hash, "abc:" + hash, "300:" + hash.substr(1),
                                    "300:" + hash + "0", "300:0x" + hash.substr(2), "300:" + HashHex('g'),
                                    "300 :" + hash, "2147483648:" + hash}) {
        BOOST_CHECK_EXCEPTION(FlowmeshRegtest({{H2_KNOB, value}}), std::runtime_error,
                              HasReason("Invalid -b3h2checkpoint=" + value + ": expected <height>:<block hash>"));
    }
    // A duplicate height, with the same or another hash.
    for (const char other : {'a', 'b'}) {
        BOOST_CHECK_EXCEPTION(ParsedChain(FlowmeshLine({"-b3h2checkpoint=300:" + hash,
                                                        std::string{"-b3h2checkpoint=300:"} + HashHex(other)})),
                              std::runtime_error, HasReason("Duplicate -b3h2checkpoint height 300"));
    }
    {
        // The kernel refuses a legacy-era height (regtest's legacy final
        // height is 0) and a duplicate from direct callers.
        CChainParams::RegTestOptions opts{};
        CChainParams::B3ModernRegTestOptions b3{};
        b3.flowmesh_test = true;
        b3.h2_checkpoints = {{0, uint256::ONE}};
        opts.b3_modern = b3;
        BOOST_CHECK_EXCEPTION(CChainParams::RegTest(opts), std::runtime_error,
                              HasReason("Invalid -b3h2checkpoint height 0: must be above the legacy final height 0"));
        b3.h2_checkpoints = {{300, uint256::ONE}, {300, uint256::ONE}};
        opts.b3_modern = b3;
        BOOST_CHECK_EXCEPTION(CChainParams::RegTest(opts), std::runtime_error,
                              HasReason("Duplicate -b3h2checkpoint height 300"));
    }

    // A negated knob is rejected by the argument parser itself.
    for (const char* negated : {"-nob3h1height", "-nob3h1escaperounds", "-nob3h2checkpoint"}) {
        BOOST_CHECK_EXCEPTION(ParsedChain(FlowmeshLine({negated})), std::runtime_error,
                              HasReason(std::string{negated}.substr(3)));
    }

    // The regtest plan's certificate-withholding knob is not provided in
    // this build (finality dependency section 19.8 item 1): rather than
    // accept it as a silent no-op, the argument parser refuses it on every
    // network, so no drill can believe certificates are withheld.
    for (const ChainType chain : {ChainType::REGTEST, ChainType::MAIN}) {
        BOOST_CHECK_EXCEPTION(ParsedChain(FlowmeshLine({"-b3regtestwithholdcertificates=131-211"}), chain),
                              std::runtime_error,
                              HasReason("parse: Invalid parameter -b3regtestwithholdcertificates=131-211"));
    }
}

// (d) Valid regtest configurations reach consensus exactly as given and
// change nothing else.
BOOST_AUTO_TEST_CASE(h1_valid_regtest_configuration)
{
    // R_h = 220 with the default R_escape (2,880 rounds, 24 h at 30 s).
    const auto chain{FlowmeshRegtest({{H1_HEIGHT_KNOB, "220"}})};
    const Consensus::Params& params{chain->GetConsensus()};
    BOOST_REQUIRE(params.h1);
    BOOST_CHECK_EQUAL(params.h1->activation_height, 220);
    BOOST_CHECK_EQUAL(params.h1->escape_rounds, 2'880);
    BOOST_CHECK_EQUAL(params.h1->escape_rounds, Consensus::H1Params::DEFAULT_ESCAPE_ROUNDS);
    BOOST_CHECK(!params.H1RulesActive(*Consensus::ModernPosStartHeight(params)));
    BOOST_CHECK(!params.H1RulesActive(219));
    BOOST_CHECK(params.H1RulesActive(220));
    BOOST_CHECK(params.H1RulesActive(std::numeric_limits<int>::max()));
    BOOST_CHECK(!params.H1EscapeRound(220, 2'879));
    BOOST_CHECK(params.H1EscapeRound(220, 2'880));
    BOOST_CHECK(!params.H1EscapeRound(219, 1'000'000));
    BOOST_CHECK(!params.H1EscapeRound(220, std::nullopt));

    // R_escape = 60 with the knob; fixture B's corridor 130 (M = 131).
    const auto fixture_b{FlowmeshRegtest({{"-b3corridorlength", "130"}, {"-b3epochlength", "40"},
                                          {"-b3maxepochextension", "40"},
                                          {H1_HEIGHT_KNOB, "220"}, {H1_ESCAPE_KNOB, "60"}})};
    const Consensus::Params& b{fixture_b->GetConsensus()};
    BOOST_REQUIRE(b.h1);
    BOOST_CHECK_EQUAL(b.h1->activation_height, 220);
    BOOST_CHECK_EQUAL(b.h1->escape_rounds, 60);
    BOOST_CHECK(!b.H1EscapeRound(220, 59));
    BOOST_CHECK(b.H1EscapeRound(220, 60));
    // E is the V1 finality epoch length, not a new parameter.
    BOOST_CHECK_EQUAL(b.modern_pos->finality_epoch_blocks, 40);

    // H1 changes nothing else: same genesis, magic, PoS block, schedule and
    // (empty) modern checkpoint table as the FlowMesh regtest without it.
    const auto plain{FlowmeshRegtest()};
    const Consensus::Params& v1{plain->GetConsensus()};
    BOOST_CHECK(!v1.h1);
    BOOST_CHECK(params.hashGenesisBlock == v1.hashGenesisBlock);
    BOOST_CHECK(chain->MessageStart() == plain->MessageStart());
    BOOST_CHECK_EQUAL(params.modern_pos->finality_epoch_blocks, v1.modern_pos->finality_epoch_blocks);
    BOOST_CHECK_EQUAL(params.modern_pos->min_finality_set, v1.modern_pos->min_finality_set);
    BOOST_CHECK_EQUAL(params.modern_pos->max_epoch_extension, v1.modern_pos->max_epoch_extension);
    BOOST_CHECK(*params.flowmesh_activation_height == *v1.flowmesh_activation_height);
    BOOST_CHECK(params.modern_checkpoints.empty());
    BOOST_CHECK(v1.modern_checkpoints.empty());
    BOOST_CHECK(!params.fresh_lineage);

    // H2 pins join the modern checkpoint table; another block at a pinned
    // height is refused and unpinned heights are unaffected.
    const uint256 pinned_300{uint256::FromHex(HashHex('a')).value()};
    const uint256 pinned_301{uint256::FromHex("00000000000000000000000000000000000000000000000000000000000001ff").value()};
    const auto pinned{ParsedChain(FlowmeshLine({"-b3h1height=220",
                                                "-b3h2checkpoint=300:" + HashHex('a'),
                                                "-b3h2checkpoint=301:00000000000000000000000000000000000000000000000000000000000001ff"}))};
    const Consensus::Params& h2{pinned->GetConsensus()};
    BOOST_REQUIRE(h2.h1);
    BOOST_CHECK_EQUAL(h2.h1->activation_height, 220);
    BOOST_CHECK(h2.modern_checkpoints == (std::map<int, uint256>{{300, pinned_300}, {301, pinned_301}}));
    // Display order: the hash's GetHex() is the string given on the line.
    BOOST_CHECK_EQUAL(h2.modern_checkpoints.at(301).GetHex(),
                      "00000000000000000000000000000000000000000000000000000000000001ff");
    BOOST_CHECK(Consensus::ModernCheckpointAllows(h2, 300, pinned_300));
    BOOST_CHECK(!Consensus::ModernCheckpointAllows(h2, 300, pinned_301));
    BOOST_CHECK(!Consensus::ModernCheckpointAllows(h2, 301, pinned_300));
    BOOST_CHECK(Consensus::ModernCheckpointAllows(h2, 299, pinned_301));
    BOOST_CHECK(Consensus::ModernCheckpointAllows(h2, 302, pinned_301));
    // An H2 pin on its own (no H1) is also valid, and the lowest modern
    // height, the first corridor block, may be pinned.
    const auto h2_only{FlowmeshRegtest({{H2_KNOB, "1:" + HashHex('c')}})};
    BOOST_CHECK(!h2_only->GetConsensus().h1);
    BOOST_CHECK_EQUAL(h2_only->GetConsensus().modern_checkpoints.size(), 1U);
    BOOST_CHECK(!Consensus::ModernCheckpointAllows(h2_only->GetConsensus(), 1, pinned_300));

    // The whole fixture-B command line parses and reaches consensus.
    const auto full{ParsedChain(FlowmeshLine({"-b3corridorlength=130", "-b3epochlength=40", "-b3maxepochextension=40",
                                              "-b3h1height=220", "-b3h1escaperounds=60",
                                              "-b3h2checkpoint=300:" + HashHex('a')}))};
    BOOST_REQUIRE(full->GetConsensus().h1);
    BOOST_CHECK_EQUAL(full->GetConsensus().h1->activation_height, 220);
    BOOST_CHECK_EQUAL(full->GetConsensus().h1->escape_rounds, 60);
    BOOST_CHECK(full->GetConsensus().modern_checkpoints == (std::map<int, uint256>{{300, pinned_300}}));
}

namespace {

using b3test::FinalityChainFixture;
using node::FinalityTracker;
using node::ProducerSchedule;
using node::ValidatorSetSnapshot;
using ValidatorKeyOpt = std::optional<std::array<unsigned char, 32>>;

uint256 HashOf(const std::shared_ptr<const ValidatorSetSnapshot>& set) { return set ? set->SetHash() : uint256{}; }

//! Every V1 field of the derived state (H1-R3: H1 changes none of them).
bool SameV1(const FinalityTracker::State& a, const FinalityTracker::State& b)
{
    return a.bootstrapped == b.bootstrapped && a.epoch == b.epoch && a.epoch_starts == b.epoch_starts &&
           HashOf(a.bootstrap) == HashOf(b.bootstrap) && HashOf(a.previous) == HashOf(b.previous) &&
           HashOf(a.current) == HashOf(b.current) && HashOf(a.next) == HashOf(b.next) &&
           a.handover_certified == b.handover_certified && a.lineage_broken == b.lineage_broken &&
           a.finalized == b.finalized;
}

//! Every field of the H1 producer schedule, sets compared by value.
bool SameProducers(const std::optional<ProducerSchedule>& a, const std::optional<ProducerSchedule>& b)
{
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    return a->epoch == b->epoch && a->epoch_start == b->epoch_start &&
           HashOf(a->current) == HashOf(b->current) && HashOf(a->next) == HashOf(b->next) &&
           (!a->current || *a->current == *b->current) && (!a->next || *a->next == *b->next) &&
           a->last_escape_height == b->last_escape_height && a->escapes == b->escapes;
}

//! Withdrawal pins that a four-member set of weight >= 34 meets.
Consensus::BridgeDecentralizedWithdrawalPins FourMemberPins()
{
    Consensus::BridgeDecentralizedWithdrawalPins pins;
    pins.ethereum_verifier_address.fill(0x11);
    pins.ethereum_verifier_code_hash = uint256::ONE;
    pins.bootstrap_validator_set_hash = uint256{uint8_t{2}};
    pins.withdrawal_rules_version = Consensus::DECENTRALIZED_WITHDRAWAL_RULES_VERSION_V1;
    pins.withdrawal_rules_commitment = Consensus::DECENTRALIZED_WITHDRAWAL_RULES_COMMITMENT_V1;
    pins.min_bridge_validators = 4;
    pins.max_bridge_validators = 4;
    pins.min_bridge_total_weight = 34;
    pins.max_epoch_lag = 1;
    return pins;
}

struct H1ChainFixture : public FinalityChainFixture {
    //! R_escape for the chain cases: above every round the fixture's round
    //! search can reach (the lightest member, weight 1 of at most 34, is
    //! saturated by round 6), so no block here is an escape block.
    static constexpr int64_t TEST_ESCAPE_ROUNDS{12};

    explicit H1ChainFixture(TestOpts opts = {}) : FinalityChainFixture{std::move(opts)} {}

    //! Configure H1 on the live consensus params; the tracker is rebuilt.
    void ConfigureH1(const int activation, const int64_t escape_rounds = TEST_ESCAPE_ROUNDS)
    {
        MutableConsensus().h1 = Consensus::H1Params{.activation_height = activation, .escape_rounds = escape_rounds};
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().ModernFinality().MarkDirty());
    }
    //! ModernEligibilityWeights for the next block (tip + 1).
    std::pair<CAmount, CAmount> Weights(const ValidatorKeyOpt& key)
    {
        LOCK(cs_main);
        Chainstate& cs{m_node.chainman->ActiveChainstate()};
        const auto w{cs.ModernEligibilityWeights(key, *cs.m_chain.Tip())};
        BOOST_REQUIRE(w.has_value());
        return *w;
    }
    //! ModernEligibilityWeights for a block at tip + 1 in `round` (from R_h
    //! a round >= R_escape reads the escape set).
    std::pair<CAmount, CAmount> WeightsInRound(const ValidatorKeyOpt& key, const int64_t round)
    {
        LOCK(cs_main);
        Chainstate& cs{m_node.chainman->ActiveChainstate()};
        const auto w{cs.ModernEligibilityWeights(key, *cs.m_chain.Tip(), round)};
        BOOST_REQUIRE(w.has_value());
        return *w;
    }
    //! The H1 schedule projected for a block at tip + 1 in `round` (must exist).
    ProducerSchedule ProducersNextInRound(const int64_t round)
    {
        LOCK(cs_main);
        const auto s{Finality().Projected(Tip()->nHeight + 1, m_node.chainman->GetConsensus(), round)};
        BOOST_REQUIRE(s.producers.has_value());
        return *s.producers;
    }
    //! A signed block by `key` on the tip at exactly `round` (no round
    //! search); the mock clock moves to its time so the time rules admit it.
    CBlock BuildInRound(const modern::PosValidatorKey& key, const int64_t round)
    {
        CBlock block{FinishPosBlock(BuildPos(Tip(), key, round, 0), key, {})};
        if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
        return block;
    }
    //! A signed block by `key` on `parent` (any branch) at exactly `round`;
    //! the mock clock moves to its time so the time rules admit it.
    CBlock BuildOnParent(const CBlockIndex* parent, const modern::PosValidatorKey& key, const int64_t round)
    {
        CBlock block{FinishPosBlock(BuildPos(parent, key, round, 0), key, {})};
        if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
        return block;
    }
    const CBlockIndex* BlockIndexOf(const uint256& hash)
    {
        LOCK(cs_main);
        const CBlockIndex* index{m_node.chainman->m_blockman.LookupBlockIndex(hash)};
        BOOST_REQUIRE(index != nullptr);
        return index;
    }
    //! H-H1: the live tracker's state equals that of a tracker rebuilt from
    //! scratch on the active chain (no schedule left over from another branch).
    bool AsRebuilt()
    {
        LOCK(cs_main);
        Chainstate& cs{m_node.chainman->ActiveChainstate()};
        FinalityTracker rebuilt;
        BOOST_REQUIRE(rebuilt.Sync(cs.m_chain, cs.m_blockman, m_node.chainman->GetConsensus(), *cs.m_chain.Tip()));
        const auto& live{Finality().Current()};
        return SameV1(live, rebuilt.Current()) && SameProducers(live.producers, rebuilt.Current().producers);
    }
    //! Produce and connect one block by `key` at exactly `round`.
    uint256 ProduceInRound(const modern::PosValidatorKey& key, const int64_t round)
    {
        const int prev_height{Tip()->nHeight};
        const CBlock block{BuildInRound(key, round)};
        const std::string probe{Probe(block)};
        BOOST_REQUIRE_MESSAGE(probe.empty(), "round " << round << " block at height " << prev_height + 1 << ": " << probe);
        BOOST_REQUIRE(Submit(block));
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, prev_height + 1);
        BOOST_CHECK(Tip()->GetBlockHash() == block.GetHash());
        return block.GetHash();
    }
    //! A block by `key` at exactly `round` that validation refuses with
    //! `reason` (TestBlockValidity) and whose connect fails: the tip stays.
    void ExpectRefusedInRound(const modern::PosValidatorKey& key, const int64_t round, const std::string& reason)
    {
        const CBlock block{BuildInRound(key, round)};
        const std::string probe{Probe(block)};
        BOOST_CHECK_MESSAGE(probe.find(reason) != std::string::npos,
                            "round " << round << ": expected " << reason << ", got '" << probe << "'");
        SubmitExpectConnectFailure(block);
    }
    //! The set SetInForceAt reports for the next block (tip + 1).
    std::shared_ptr<const ValidatorSetSnapshot> InForceNext()
    {
        LOCK(cs_main);
        return Finality().SetInForceAt(Tip()->nHeight + 1, m_node.chainman->GetConsensus());
    }
    //! The H1 schedule projected for the next block (must exist).
    ProducerSchedule ProducersNext()
    {
        const auto s{ProjectedNext()};
        BOOST_REQUIRE(s.producers.has_value());
        return *s.producers;
    }
    //! H1-R3: the live tracker's V1 fields equal those of a tracker that
    //! walks the same chain without H1 (which never has a schedule).
    bool V1FieldsAsWithoutH1()
    {
        LOCK(cs_main);
        Chainstate& cs{m_node.chainman->ActiveChainstate()};
        Consensus::Params no_h1{m_node.chainman->GetConsensus()};
        no_h1.h1.reset();
        FinalityTracker v1_only;
        BOOST_REQUIRE(v1_only.Sync(cs.m_chain, cs.m_blockman, no_h1, *cs.m_chain.Tip()));
        return SameV1(Finality().Current(), v1_only.Current()) && !v1_only.Current().producers;
    }
    //! Snapshot(tip) stamped `epoch`, derived independently from the node's
    //! public stake and binding trackers (not the finality tracker's own).
    ValidatorSetSnapshot SnapshotAtTip(const uint64_t epoch)
    {
        LOCK(cs_main);
        Chainstate& cs{m_node.chainman->ActiveChainstate()};
        const Consensus::Params& consensus{m_node.chainman->GetConsensus()};
        const CBlockIndex& tip{*cs.m_chain.Tip()};
        BOOST_REQUIRE(cs.ModernStakeTracker().Sync(cs.m_chain, cs.m_blockman, consensus, tip));
        BOOST_REQUIRE(cs.ModernFinalityBindings().Sync(cs.m_chain, cs.m_blockman, consensus, tip));
        const auto snap{ValidatorSetSnapshot::BuildAt(epoch, cs.ModernStakeTracker(), tip.nHeight,
                                                      cs.ModernFinalityBindings().Index())};
        BOOST_REQUIRE(snap.has_value());
        return *snap;
    }
    //! Produce one block by `key` (with `txs`); once the current V1 epoch's
    //! first checkpoint is deep enough, include its handover certificate
    //! (signed by every member), so an intact V1 lineage rotates every E
    //! blocks.
    void ProduceCertifying(const modern::PosValidatorKey& key, std::vector<CMutableTransaction> txs = {})
    {
        const auto s{FinalityState()};
        const int next{Tip()->nHeight + 1};
        std::vector<std::pair<CScript, CMpaRecord>> payload;
        if (s.bootstrapped && !s.lineage_broken && !s.handover_certified && s.current && s.next) {
            int cp{s.epoch_starts.back()};
            while (!modern::IsCheckpointHeight(cp, m_M, SCALED_INTERVAL)) ++cp;
            if (next - cp >= SCALED_DEPTH && (!s.finalized || cp > s.finalized->height)) {
                payload.push_back(MakeCertificate({cp, s.epoch, s.next->SetHash()}, *s.current));
            }
        }
        Produce(key, payload, std::move(txs));
    }
};

//! Disk-backed variant for restart / -reindex-chainstate scenarios.
struct H1ChainDiskFixture : public H1ChainFixture {
    H1ChainDiskFixture() : H1ChainFixture{{.coins_db_in_memory = false, .block_tree_db_in_memory = false}} {}
};

} // namespace

// (e) H1-R1: below R_h the producer set and the eligibility weights are V1's,
// bit for bit (the same snapshot object), whatever the round; the V1 fields
// equal a V1-only tracker's at every height, also above R_h (H1-R3), while
// an intact V1 lineage keeps certifying and rotating; without H1 no producer
// schedule ever exists. From R_h, inside V1 epoch 2 and not on a V1 epoch
// start, producers follow the H1 schedule and not V1's set: C binds and B
// revokes after V1's Set_2 snapshot (M + E - 1) but before R_h - 1, so
// ProducerSet_0 = Snapshot(R_h - 1) = {A, C} (W 25) while V1's Set_2 =
// {A, B} (W 16) stays in force until M + 3E. At R_h, B's block (V1 would
// accept it) is refused and C's block (V1 would refuse it) connects.
BOOST_FIXTURE_TEST_CASE(below_rh_bit_identical, H1ChainFixture)
{
    PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/200, /*with_unbound_c=*/true);
    const int M{m_M};
    const int E{SCALED_E};
    const int R_h{M + 2 * E + 13}; // inside V1 epoch 2 = [M + 2E, M + 3E)
    ConfigureH1(R_h);
    const auto bind_c{MakeBinding(m_validator_c, m_vk_c, &m_bls_c, 0)};
    const auto revoke_b{MakeBinding(m_validator_b, m_vk_b, nullptr, 1)};
    ChainstateManager& chainman{*m_node.chainman};
    Consensus::Params no_h1{chainman.GetConsensus()};
    no_h1.h1.reset();
    FinalityTracker v1_only; // the reference: the same chain without H1
    int producers_seen{0};

    const auto check{[&] {
        LOCK(cs_main);
        Chainstate& cs{chainman.ActiveChainstate()};
        const Consensus::Params& consensus{chainman.GetConsensus()};
        const CBlockIndex& tip{*cs.m_chain.Tip()};
        const int next{tip.nHeight + 1};
        FinalityTracker& tracker{Finality()};
        BOOST_REQUIRE(v1_only.Sync(cs.m_chain, cs.m_blockman, no_h1, tip));
        const FinalityTracker::State projected{tracker.Projected(next, consensus)};
        BOOST_CHECK(SameV1(tracker.Current(), v1_only.Current()));
        BOOST_CHECK(SameV1(projected, v1_only.Projected(next, no_h1)));
        BOOST_CHECK(!v1_only.Current().producers);
        BOOST_CHECK(!v1_only.Projected(next, no_h1, TEST_ESCAPE_ROUNDS).producers);
        BOOST_CHECK(tracker.ProducerScheduleAvailable(next, consensus));
        BOOST_CHECK(v1_only.ProducerScheduleAvailable(next, no_h1));
        if (next >= R_h) {
            BOOST_CHECK(projected.producers.has_value());
            BOOST_CHECK(HashOf(tracker.SetInForceAt(next, consensus)) ==
                        (projected.producers ? HashOf(projected.producers->current) : uint256{}));
            ++producers_seen;
            return;
        }
        BOOST_CHECK(!tracker.Current().producers);
        BOOST_CHECK(!projected.producers);
        const auto with_h1{tracker.SetInForceAt(next, consensus)};
        const auto escape_round{tracker.SetInForceAt(next, consensus, TEST_ESCAPE_ROUNDS)};
        const auto without_h1{tracker.SetInForceAt(next, no_h1)};
        const auto reference{v1_only.SetInForceAt(next, no_h1)};
        BOOST_REQUIRE(with_h1 && escape_round && without_h1 && reference);
        BOOST_CHECK(*with_h1 == *reference);
        BOOST_CHECK(with_h1->SetHash() == reference->SetHash());
        if (next > M) {
            // The V1 member itself, not a rebuilt copy.
            BOOST_CHECK(with_h1 == without_h1);
            BOOST_CHECK(escape_round == without_h1);
        } else {
            BOOST_CHECK(*with_h1 == *without_h1);
            BOOST_CHECK(*escape_round == *without_h1);
        }
        for (const ValidatorKeyOpt& key : {ValidatorKeyOpt{m_vk_a}, ValidatorKeyOpt{m_vk_b}, ValidatorKeyOpt{m_vk_c},
                                           ValidatorKeyOpt{}}) {
            const auto weights_h1{cs.ModernEligibilityWeights(key, tip)};
            Consensus::Params& mutable_consensus{MutableConsensus()};
            const auto saved{mutable_consensus.h1};
            mutable_consensus.h1.reset();
            const auto weights_v1{cs.ModernEligibilityWeights(key, tip)};
            mutable_consensus.h1 = saved;
            BOOST_REQUIRE(weights_h1.has_value() && weights_v1.has_value());
            BOOST_CHECK(*weights_h1 == *weights_v1);
        }
    }};

    //! (w, W) for `key` at tip + 1 judged as if the chain had no H1.
    const auto weights_without_h1{[&](const ValidatorKeyOpt& key) {
        LOCK(cs_main);
        Chainstate& cs{chainman.ActiveChainstate()};
        Consensus::Params& mutable_consensus{MutableConsensus()};
        const auto saved{mutable_consensus.h1};
        mutable_consensus.h1.reset();
        const auto weights{cs.ModernEligibilityWeights(key, *cs.m_chain.Tip())};
        mutable_consensus.h1 = saved;
        BOOST_REQUIRE(weights.has_value());
        return *weights;
    }};

    check(); // tip M - 1: the Set_0 preview at M
    while (Tip()->nHeight < R_h + E + 10) {
        const int next{Tip()->nHeight + 1};
        if (next == M + E + 5) {
            ProduceCertifying(m_vk_a, {MakeTx(8, {bind_c.cell}, {bind_c.record})});
        } else if (next == M + E + 6) {
            ProduceCertifying(m_vk_a, {MakeTx(9, {revoke_b.cell}, {revoke_b.record})});
        } else if (next == R_h) {
            // ProducerSet_0 = Snapshot(R_h - 1) = {A, C}; V1's set in force
            // at R_h is still Set_2 = Snapshot(M + E - 1) = {A, B}.
            const auto v1_set{v1_only.SetInForceAt(R_h, no_h1)}; // synced to R_h - 1 by check()
            const auto producer_set{InForceNext()};
            BOOST_REQUIRE(v1_set && producer_set);
            BOOST_CHECK(*producer_set == SnapshotAtTip(0));
            BOOST_CHECK(producer_set->SetHash() != v1_set->SetHash());
            BOOST_CHECK(producer_set->IndexOf(m_vk_c) && !producer_set->IndexOf(m_vk_b));
            BOOST_CHECK(v1_set->IndexOf(m_vk_b) && !v1_set->IndexOf(m_vk_c));
            BOOST_CHECK_EQUAL(producer_set->TotalWeight(), 25U);
            BOOST_CHECK_EQUAL(v1_set->TotalWeight(), 16U);
            BOOST_CHECK(HashOf(FinalityState().current) == v1_set->SetHash());
            BOOST_CHECK(Weights(m_vk_c) == std::make_pair(CAmount{10}, CAmount{25}));
            BOOST_CHECK(Weights(m_vk_b) == std::make_pair(CAmount{0}, CAmount{25}));
            BOOST_CHECK(weights_without_h1(m_vk_c) == std::make_pair(CAmount{0}, CAmount{16}));
            BOOST_CHECK(weights_without_h1(m_vk_b) == std::make_pair(CAmount{1}, CAmount{16}));
            ExpectRefusedInRound(m_vk_b, 0, "bad-pos-ineligible");
            ProduceCertifying(m_vk_c); // R_h: only the H1 schedule admits C
        } else if (next < R_h) {
            ProduceCertifying(next % 7 == 3 ? m_vk_b : m_vk_a);
        } else {
            ProduceCertifying(next % 5 == 2 ? m_vk_c : m_vk_a);
        }
        check();
    }
    BOOST_CHECK_EQUAL(producers_seen, E + 12); // tips R_h - 1 .. R_h + E + 10
    BOOST_CHECK(Tip()->nHeight > M + 3 * E);
    // V1 kept certifying and rotating through and after R_h, unaffected:
    // its Set_3 = Snapshot(M + 2E - 1) = {A, C} took force only at M + 3E.
    const auto s{FinalityState()};
    BOOST_CHECK(!s.lineage_broken);
    BOOST_CHECK_EQUAL(s.epoch, 3U);
    BOOST_CHECK(s.epoch_starts == (std::vector<int>{M, M + E, M + 2 * E, M + 3 * E}));
    BOOST_REQUIRE(s.finalized.has_value());
    BOOST_CHECK(s.finalized->height >= M + 3 * E);
    BOOST_REQUIRE(s.current);
    BOOST_CHECK(s.current->IndexOf(m_vk_c) && !s.current->IndexOf(m_vk_b));
    // Producers rotated at R_h + E on their own schedule.
    BOOST_REQUIRE(s.producers.has_value());
    BOOST_CHECK_EQUAL(s.producers->epoch, 1U);
    BOOST_CHECK_EQUAL(s.producers->epoch_start, R_h + E);
    BOOST_CHECK_EQUAL(s.producers->escapes, 0U);
    BOOST_REQUIRE(s.producers->current);
    BOOST_CHECK(s.producers->current->IndexOf(m_vk_c) && !s.producers->current->IndexOf(m_vk_b));
}

// (f) A broken V1 lineage freezes producers (a newly bound staker is never
// eligible); from R_h the schedule rotates every E without certificates:
// the new staker produces at R_h, a revoked member keeps producing for its
// residual epochs and leaves exactly at R_h + 2E, with ProducerSet_(k+1) =
// Snapshot(start_k - 1). The V1 fields stay frozen and the bridge burn gate,
// which reads only them, stays closed.
BOOST_FIXTURE_TEST_CASE(broken_lineage_frozen_then_rotates, H1ChainFixture)
{
    // V1 Set_0 = {A 15, B 1, D 8}, W 24; C (10) is staked but not bound.
    PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/200, /*with_unbound_c=*/true,
                         /*with_bound_c=*/false, /*with_bound_d=*/true);
    const int M{m_M};
    const int E{SCALED_E};
    const int R_h{M + 70}; // no certificate ever: V1 breaks at M + E + MAX_EXTENSION = M + 60
    ConfigureH1(R_h);
    const Consensus::Params& consensus{m_node.chainman->GetConsensus()};
    const auto pins{FourMemberPins()};
    BOOST_REQUIRE(pins.Valid());

    Produce(m_vk_a); // M: V1 bootstrap
    const auto v1_set0{FinalityState().current};
    BOOST_REQUIRE(v1_set0);
    BOOST_CHECK_EQUAL(v1_set0->Size(), 3U);
    BOOST_CHECK_EQUAL(v1_set0->TotalWeight(), 24U);
    const auto v1_frozen{[&] {
        const auto s{FinalityState()};
        BOOST_CHECK_EQUAL(s.epoch, 0U);
        BOOST_CHECK(s.epoch_starts == std::vector<int>{M});
        BOOST_CHECK(HashOf(s.current) == v1_set0->SetHash());
        BOOST_CHECK(!s.finalized);
        BOOST_CHECK(!s.handover_certified);
        return s;
    }};

    // C binds at M + 5; V1 never hands over, so C never enters its set.
    ProduceTo(M + 4, m_vk_a);
    const auto bind_c{MakeBinding(m_validator_c, m_vk_c, &m_bls_c, 0)};
    Produce(m_vk_a, {}, {MakeTx(8, {bind_c.cell}, {bind_c.record})});
    ProduceTo(M + E + SCALED_MAX_EXTENSION - 1, m_vk_a);
    BOOST_CHECK(!v1_frozen().lineage_broken);
    Produce(m_vk_a); // M + 60
    BOOST_CHECK(v1_frozen().lineage_broken);

    // Frozen through R_h - 1: C, ACTIVE and bound, is not eligible.
    ProduceTo(R_h - 2, m_vk_a);
    BOOST_CHECK(Weights(m_vk_c) == std::make_pair(CAmount{0}, CAmount{24}));
    BOOST_CHECK(Weights(m_vk_b) == std::make_pair(CAmount{1}, CAmount{24}));
    BOOST_CHECK(!ProjectedNext().producers);
    ProduceExpectConnectFailure(m_vk_c, {}, 1);
    Produce(m_vk_a); // R_h - 1
    BOOST_CHECK(!FinalityState().producers);

    // R_h: ProducerSet_0 = ProducerSet_1 = Snapshot(R_h - 1) = {A, B, C, D}, W 34.
    const auto snap_rh{SnapshotAtTip(0)};
    BOOST_CHECK_EQUAL(snap_rh.Size(), 4U);
    BOOST_CHECK_EQUAL(snap_rh.TotalWeight(), 34U);
    BOOST_CHECK(Weights(m_vk_c) == std::make_pair(CAmount{10}, CAmount{34}));
    BOOST_CHECK(Weights(m_vk_b) == std::make_pair(CAmount{1}, CAmount{34}));
    BOOST_CHECK(Weights(m_vk_d) == std::make_pair(CAmount{8}, CAmount{34}));
    BOOST_CHECK(Weights(std::nullopt) == std::make_pair(CAmount{0}, CAmount{34}));
    {
        const auto projected{ProjectedNext()};
        BOOST_REQUIRE(projected.producers);
        const ProducerSchedule& p{*projected.producers};
        BOOST_CHECK_EQUAL(p.epoch, 0U);
        BOOST_CHECK_EQUAL(p.epoch_start, R_h);
        BOOST_REQUIRE(p.current && p.next);
        BOOST_CHECK(*p.current == snap_rh);
        BOOST_CHECK(*p.next == snap_rh.WithEpoch(1));
        BOOST_CHECK_EQUAL(p.last_escape_height, -1);
        BOOST_CHECK_EQUAL(p.escapes, 0U);
        BOOST_CHECK(HashOf(InForceNext()) == p.current->SetHash());
        // The V1 projection is untouched (broken, Set_0), so burns stay
        // closed although the producer sets alone would meet the pins.
        BOOST_CHECK(projected.lineage_broken);
        BOOST_CHECK(HashOf(projected.current) == v1_set0->SetHash());
        BOOST_CHECK(!node::BridgeWithdrawalValidatorSetsReady(projected, pins));
        FinalityTracker::State as_if_intact{projected};
        as_if_intact.lineage_broken = false;
        as_if_intact.current = p.current;
        as_if_intact.next = p.next;
        BOOST_CHECK(node::BridgeWithdrawalValidatorSetsReady(as_if_intact, pins));
    }
    Produce(m_vk_c); // R_h: the new staker produces
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE(s.producers);
        BOOST_CHECK_EQUAL(s.producers->epoch, 0U);
        BOOST_CHECK_EQUAL(s.producers->epoch_start, R_h);
        BOOST_CHECK(*s.producers->current == snap_rh);
    }

    // B revokes at R_h + 3 and stays a producer through R_h + 2E - 1.
    ProduceTo(R_h + 2, m_vk_a);
    const auto revoke_b{MakeBinding(m_validator_b, m_vk_b, nullptr, 1)};
    Produce(m_vk_a, {}, {MakeTx(9, {revoke_b.cell}, {revoke_b.record})});
    BOOST_CHECK(Weights(m_vk_b) == std::make_pair(CAmount{1}, CAmount{34}));
    ProduceTo(R_h + E - 2, m_vk_a);
    BOOST_CHECK_EQUAL(ProducersNext().epoch, 0U); // no rotation before R_h + E
    Produce(m_vk_a); // R_h + E - 1

    // Rotation 1 at exactly R_h + E, no certificate: ProducerSet_1 takes
    // force, ProducerSet_2 = Snapshot(R_h + E - 1) = {A, C, D}, W 33.
    const auto snap_e1{SnapshotAtTip(2)};
    BOOST_CHECK_EQUAL(snap_e1.Size(), 3U);
    BOOST_CHECK_EQUAL(snap_e1.TotalWeight(), 33U);
    BOOST_CHECK(!snap_e1.IndexOf(m_vk_b));
    {
        const auto p{ProducersNext()};
        BOOST_CHECK_EQUAL(p.epoch, 1U);
        BOOST_CHECK_EQUAL(p.epoch_start, R_h + E);
        BOOST_CHECK(*p.current == snap_rh.WithEpoch(1));
        BOOST_CHECK(*p.next == snap_e1);
    }
    BOOST_CHECK(Weights(m_vk_b) == std::make_pair(CAmount{1}, CAmount{34}));
    Produce(m_vk_b); // R_h + E: the revoked B still produces (residual)
    ProduceTo(R_h + 2 * E - 2, m_vk_a);
    BOOST_CHECK_EQUAL(ProducersNext().epoch, 1U);
    BOOST_CHECK(Weights(m_vk_b) == std::make_pair(CAmount{1}, CAmount{34}));
    Produce(m_vk_b); // R_h + 2E - 1: B's last eligible height

    // Rotation 2 at exactly R_h + 2E: B is out, W 33.
    const auto snap_e2{SnapshotAtTip(3)};
    {
        const auto p{ProducersNext()};
        BOOST_CHECK_EQUAL(p.epoch, 2U);
        BOOST_CHECK_EQUAL(p.epoch_start, R_h + 2 * E);
        BOOST_CHECK(*p.current == snap_e1);
        BOOST_CHECK(*p.next == snap_e2);
    }
    BOOST_CHECK(Weights(m_vk_b) == std::make_pair(CAmount{0}, CAmount{33}));
    BOOST_CHECK(Weights(m_vk_c) == std::make_pair(CAmount{10}, CAmount{33}));
    ProduceExpectConnectFailure(m_vk_b, {}, 2);
    Produce(m_vk_c); // R_h + 2E
    {
        const auto s{v1_frozen()};
        BOOST_CHECK(s.lineage_broken);
        BOOST_REQUIRE(s.producers);
        BOOST_CHECK_EQUAL(s.producers->epoch, 2U);
        BOOST_CHECK_EQUAL(s.producers->epoch_start, R_h + 2 * E);
        BOOST_CHECK(*s.producers->current == snap_e1);
        BOOST_CHECK(!node::BridgeWithdrawalValidatorSetsReady(ProjectedNext(), pins));
    }

    // The schedule is only defined for the synced tip + 1 under H1.
    {
        LOCK(cs_main);
        FinalityTracker& tracker{Finality()};
        const int next{Tip()->nHeight + 1};
        BOOST_CHECK(tracker.ProducerScheduleAvailable(next, consensus));
        BOOST_CHECK(!tracker.ProducerScheduleAvailable(next + 1, consensus));
        BOOST_CHECK(!tracker.SetInForceAt(next + 1, consensus));
        // Projected() itself requires the synced tip + 1 (its V1 projection
        // Assumes it, which aborts a Debug build), so it is not probed here.
        BOOST_CHECK(tracker.ProducerScheduleAvailable(R_h - 1, consensus)); // below R_h: V1, always available
    }
}

// (g) Carry-over below MIN_FINALITY_SET = 2. Bootstrap fallback: B revoked
// before R_h - 1 leaves Snapshot(R_h - 1) = {A}, so ProducerSet_0 is the V1
// set in force at R_h - 1 ({A, B}) re-stamped 0 and B produces at R_h. A
// rotation whose snapshot is still {A} carries the current set over; once C
// binds, the next snapshot meets the floor and replaces it.
BOOST_FIXTURE_TEST_CASE(carry_over_and_bootstrap_fallback, H1ChainFixture)
{
    PrepareFinalityChain(/*min_finality_set=*/2, /*reorg_horizon=*/200, /*with_unbound_c=*/true);
    const int M{m_M};
    const int E{SCALED_E};
    const int R_h{M + 10};
    ConfigureH1(R_h);

    Produce(m_vk_a); // M
    const auto v1_set0{FinalityState().current};
    BOOST_REQUIRE(v1_set0);
    BOOST_CHECK_EQUAL(v1_set0->Size(), 2U);
    ProduceTo(M + 2, m_vk_a);
    const auto revoke_b{MakeBinding(m_validator_b, m_vk_b, nullptr, 1)};
    Produce(m_vk_a, {}, {MakeTx(4, {revoke_b.cell}, {revoke_b.record})}); // M + 3
    ProduceTo(R_h - 1, m_vk_a);
    BOOST_CHECK_EQUAL(SnapshotAtTip(0).Size(), 1U);
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE(s.current);
        BOOST_CHECK(s.current->SetHash() == v1_set0->SetHash()); // the V1 set in force at R_h - 1
        const auto p{ProducersNext()};
        BOOST_CHECK_EQUAL(p.epoch, 0U);
        BOOST_CHECK_EQUAL(p.epoch_start, R_h);
        BOOST_REQUIRE(p.current && p.next);
        BOOST_CHECK(*p.current == s.current->WithEpoch(0));
        BOOST_CHECK_EQUAL(p.current->Size(), 2U);
        BOOST_CHECK(*p.next == s.current->WithEpoch(1));
    }
    BOOST_CHECK(Weights(m_vk_b) == std::make_pair(CAmount{1}, CAmount{16}));
    Produce(m_vk_b); // R_h: the revoked B produces from the fallback set

    // Rotation at R_h + E: Snapshot(R_h + E - 1) is still {A}, so
    // ProducerSet_2 = ProducerSet_1 re-stamped 2 (V1's carry-over).
    ProduceTo(R_h + E - 1, m_vk_a);
    BOOST_CHECK_EQUAL(SnapshotAtTip(0).Size(), 1U);
    {
        const auto p{ProducersNext()};
        BOOST_CHECK_EQUAL(p.epoch, 1U);
        BOOST_CHECK_EQUAL(p.epoch_start, R_h + E);
        BOOST_CHECK(*p.current == v1_set0->WithEpoch(1));
        BOOST_CHECK(*p.next == p.current->WithEpoch(2));
    }
    Produce(m_vk_a); // R_h + E

    // C binds at R_h + E + 2: Snapshot(R_h + 2E - 1) = {A, C} meets the floor.
    ProduceTo(R_h + E + 1, m_vk_a);
    const auto bind_c{MakeBinding(m_validator_c, m_vk_c, &m_bls_c, 0)};
    Produce(m_vk_a, {}, {MakeTx(5, {bind_c.cell}, {bind_c.record})});
    ProduceTo(R_h + 2 * E - 1, m_vk_a);
    const auto snap{SnapshotAtTip(3)};
    BOOST_CHECK_EQUAL(snap.Size(), 2U);
    BOOST_CHECK_EQUAL(snap.TotalWeight(), 25U);
    {
        const auto p{ProducersNext()};
        BOOST_CHECK_EQUAL(p.epoch, 2U);
        BOOST_CHECK(*p.current == v1_set0->WithEpoch(2)); // the carried set governs epoch 2
        BOOST_CHECK(*p.next == snap);
    }
    BOOST_CHECK(Weights(m_vk_b) == std::make_pair(CAmount{1}, CAmount{16}));
    BOOST_CHECK(Weights(m_vk_c) == std::make_pair(CAmount{0}, CAmount{16}));
    Produce(m_vk_b); // R_h + 2E
    ProduceTo(R_h + 3 * E - 1, m_vk_a);
    BOOST_CHECK(Weights(m_vk_c) == std::make_pair(CAmount{10}, CAmount{25}));
    BOOST_CHECK(Weights(m_vk_b) == std::make_pair(CAmount{0}, CAmount{25}));
    ProduceExpectConnectFailure(m_vk_b, {}, 1);
    Produce(m_vk_c); // R_h + 3E
    BOOST_CHECK_EQUAL(FinalityState().producers->epoch, 3U);
}

// (h) H1-R4: the schedule is a function of block data alone. The live
// tracker (BlockConnected), a tracker stepped only by Sync's walk, a full
// rebuild and a fresh tracker agree at every height; a tracker whose state
// lacks the schedule under H1 fails closed (no weights, never weight 0)
// until it is rebuilt.
BOOST_FIXTURE_TEST_CASE(rebuild_reproduces_schedule, H1ChainFixture)
{
    PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/200, /*with_unbound_c=*/true);
    const int M{m_M};
    const int E{SCALED_E};
    const int R_h{M + 5};
    ConfigureH1(R_h);
    ChainstateManager& chainman{*m_node.chainman};
    FinalityTracker walked;
    const auto bind_c{MakeBinding(m_validator_c, m_vk_c, &m_bls_c, 0)};
    const auto revoke_b{MakeBinding(m_validator_b, m_vk_b, nullptr, 1)};
    while (Tip()->nHeight < R_h + 2 * E + 7) {
        const int next{Tip()->nHeight + 1};
        if (next == R_h + 3) {
            Produce(m_vk_a, {}, {MakeTx(8, {bind_c.cell}, {bind_c.record})});
        } else if (next == R_h + E + 4) {
            Produce(m_vk_a, {}, {MakeTx(9, {revoke_b.cell}, {revoke_b.record})});
        } else if (next % 3 == 0 && Weights(m_vk_c).first > 0) {
            Produce(m_vk_c);
        } else if (next % 4 == 1 && Weights(m_vk_b).first > 0) {
            Produce(m_vk_b);
        } else {
            Produce(m_vk_a);
        }
        LOCK(cs_main);
        Chainstate& cs{chainman.ActiveChainstate()};
        BOOST_REQUIRE(walked.Sync(cs.m_chain, cs.m_blockman, chainman.GetConsensus(), *cs.m_chain.Tip()));
        const FinalityTracker::State& live{Finality().Current()};
        BOOST_CHECK(SameV1(live, walked.Current()));
        BOOST_CHECK(SameProducers(live.producers, walked.Current().producers));
        BOOST_CHECK_EQUAL(live.producers.has_value(), next >= R_h);
    }
    const auto before{FinalityState()};
    BOOST_REQUIRE(before.producers);
    BOOST_CHECK_EQUAL(before.producers->epoch, 2U);
    BOOST_CHECK_EQUAL(before.producers->epoch_start, R_h + 2 * E);
    BOOST_CHECK(before.producers->current->IndexOf(m_vk_c).has_value());
    BOOST_CHECK(before.producers->current->IndexOf(m_vk_b).has_value()); // revoked after R_h + E - 1
    BOOST_CHECK(!before.producers->next->IndexOf(m_vk_b).has_value());
    const auto weights_c{Weights(m_vk_c)};

    // Full rebuild of the live tracker, and a fresh tracker from nothing.
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().ModernFinality().MarkDirty());
    const auto after{FinalityState()};
    BOOST_CHECK(SameV1(before, after));
    BOOST_CHECK(SameProducers(before.producers, after.producers));
    {
        LOCK(cs_main);
        Chainstate& cs{chainman.ActiveChainstate()};
        FinalityTracker fresh;
        BOOST_REQUIRE(fresh.Sync(cs.m_chain, cs.m_blockman, chainman.GetConsensus(), *cs.m_chain.Tip()));
        BOOST_CHECK(SameV1(before, fresh.Current()));
        BOOST_CHECK(SameProducers(before.producers, fresh.Current().producers));
    }

    // Fail closed: a state derived without H1 (no schedule) asked under H1.
    {
        LOCK(cs_main);
        Chainstate& cs{chainman.ActiveChainstate()};
        Consensus::Params& consensus{MutableConsensus()};
        const auto saved{consensus.h1};
        consensus.h1.reset();
        cs.ModernFinality().MarkDirty();
        BOOST_CHECK(!Finality().Current().producers);
        consensus.h1 = saved;
        const int next{cs.m_chain.Tip()->nHeight + 1};
        BOOST_CHECK(!Finality().ProducerScheduleAvailable(next, consensus));
        BOOST_CHECK(!Finality().SetInForceAt(next, consensus));
        BOOST_CHECK(!cs.ModernEligibilityWeights(m_vk_c, *cs.m_chain.Tip()).has_value());
        cs.ModernFinality().MarkDirty();
        BOOST_CHECK(Finality().ProducerScheduleAvailable(next, consensus));
        BOOST_CHECK(SameProducers(before.producers, Finality().Current().producers));
        // R_h at or below M (refused by the chain parameters) is never
        // answered from a schedule: an error, not "nobody is eligible".
        consensus.h1->activation_height = M;
        BOOST_CHECK(!Finality().ProducerScheduleAvailable(next, consensus));
        BOOST_CHECK(!Finality().SetInForceAt(next, consensus));
        BOOST_CHECK(!cs.ModernEligibilityWeights(m_vk_c, *cs.m_chain.Tip()).has_value());
        consensus.h1 = saved;
        BOOST_CHECK(Finality().ProducerScheduleAvailable(next, consensus));
    }
    BOOST_CHECK(Weights(m_vk_c) == weights_c);
    Produce(m_vk_c);
}

// (i) FD-2 escape. C, bound after R_h, is in Snapshot(parent) but not in the
// producer set in force: its block in round R_escape - 1 is refused
// (bad-pos-ineligible) and in round R_escape it connects, judged against
// Snapshot(h_e - 1). The escape starts producer epoch k + 1 at h_e with both
// sets equal to that snapshot, and the next rotation is at h_e + E, not at
// the old epoch_start + E. A member's escape block (A) restarts the schedule
// the same way and drops a revoked member (B) at once, where a rotation
// would have kept it for its residual epochs. The round never moves a V1
// field (H1-R3).
BOOST_FIXTURE_TEST_CASE(escape_restarts_schedule, H1ChainFixture)
{
    PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/200, /*with_unbound_c=*/true);
    const int M{m_M};
    const int E{SCALED_E};
    const int R_h{M + 10};
    constexpr int64_t R_ESC{TEST_ESCAPE_ROUNDS};
    ConfigureH1(R_h);
    const Consensus::Params& consensus{m_node.chainman->GetConsensus()};

    ProduceTo(R_h + 1, m_vk_a);
    const auto bind_c{MakeBinding(m_validator_c, m_vk_c, &m_bls_c, 0)};
    Produce(m_vk_a, {}, {MakeTx(8, {bind_c.cell}, {bind_c.record})}); // R_h + 2
    const int h_e{R_h + 20}; // inside producer epoch 0, before its rotation at R_h + E
    ProduceTo(h_e - 1, m_vk_a);

    // In force: ProducerSet_0 = {A, B} (W 16); Snapshot(h_e - 1) = {A, B, C} (W 26).
    const auto snap{SnapshotAtTip(0)};
    BOOST_CHECK_EQUAL(snap.Size(), 3U);
    BOOST_CHECK_EQUAL(snap.TotalWeight(), 26U);
    {
        const auto p{ProducersNext()};
        BOOST_CHECK_EQUAL(p.epoch, 0U);
        BOOST_CHECK_EQUAL(p.epoch_start, R_h);
        BOOST_REQUIRE(p.current && p.next);
        BOOST_CHECK(!p.current->IndexOf(m_vk_c));
        BOOST_CHECK(!p.next->IndexOf(m_vk_c));
    }
    BOOST_CHECK(Weights(m_vk_c) == std::make_pair(CAmount{0}, CAmount{16}));
    BOOST_CHECK(WeightsInRound(m_vk_c, 0) == std::make_pair(CAmount{0}, CAmount{16}));
    BOOST_CHECK(WeightsInRound(m_vk_c, R_ESC - 1) == std::make_pair(CAmount{0}, CAmount{16}));
    BOOST_CHECK(WeightsInRound(m_vk_c, R_ESC) == std::make_pair(CAmount{10}, CAmount{26}));
    BOOST_CHECK(WeightsInRound(m_vk_c, R_ESC + 1'000) == std::make_pair(CAmount{10}, CAmount{26}));
    BOOST_CHECK(WeightsInRound(m_vk_a, R_ESC) == std::make_pair(CAmount{15}, CAmount{26}));
    BOOST_CHECK(WeightsInRound(m_vk_b, R_ESC) == std::make_pair(CAmount{1}, CAmount{26}));
    BOOST_CHECK(WeightsInRound(std::nullopt, R_ESC) == std::make_pair(CAmount{0}, CAmount{26}));
    {
        LOCK(cs_main);
        FinalityTracker& tracker{Finality()};
        const auto none{tracker.Projected(h_e, consensus)};
        const auto normal{tracker.Projected(h_e, consensus, R_ESC - 1)};
        const auto escape{tracker.Projected(h_e, consensus, R_ESC)};
        BOOST_CHECK(SameV1(none, normal));
        BOOST_CHECK(SameV1(none, escape));
        BOOST_CHECK(SameProducers(none.producers, normal.producers));
        BOOST_REQUIRE(normal.producers && escape.producers);
        BOOST_CHECK_EQUAL(normal.producers->epoch, 0U);
        BOOST_CHECK_EQUAL(normal.producers->escapes, 0U);
        BOOST_CHECK_EQUAL(escape.producers->epoch, 1U);
        BOOST_CHECK_EQUAL(escape.producers->epoch_start, h_e);
        BOOST_REQUIRE(escape.producers->current);
        BOOST_CHECK(*escape.producers->current == snap.WithEpoch(1));
        BOOST_CHECK(HashOf(tracker.SetInForceAt(h_e, consensus, R_ESC)) == snap.WithEpoch(1).SetHash());
        BOOST_CHECK(HashOf(tracker.SetInForceAt(h_e, consensus, R_ESC - 1)) == HashOf(normal.producers->current));
        BOOST_CHECK(HashOf(tracker.SetInForceAt(h_e, consensus)) == HashOf(normal.producers->current));
    }

    // C in round R_escape - 1: refused. In round R_escape: connects.
    ExpectRefusedInRound(m_vk_c, R_ESC - 1, "bad-pos-ineligible");
    ProduceInRound(m_vk_c, R_ESC); // h_e
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE(s.producers);
        const ProducerSchedule& p{*s.producers};
        BOOST_CHECK_EQUAL(p.epoch, 1U);
        BOOST_CHECK_EQUAL(p.epoch_start, h_e);
        BOOST_REQUIRE(p.current && p.next);
        BOOST_CHECK(*p.current == snap.WithEpoch(1));
        BOOST_CHECK(*p.next == snap.WithEpoch(2));
        BOOST_CHECK(p.current->IndexOf(m_vk_c).has_value());
        BOOST_CHECK_EQUAL(p.last_escape_height, h_e);
        BOOST_CHECK_EQUAL(p.escapes, 1U);
    }
    BOOST_CHECK(V1FieldsAsWithoutH1());
    // C now produces in normal rounds.
    BOOST_CHECK(Weights(m_vk_c) == std::make_pair(CAmount{10}, CAmount{26}));
    Produce(m_vk_c); // h_e + 1

    // No rotation at the old epoch_start + E = R_h + E ...
    ProduceTo(R_h + E - 1, m_vk_a);
    {
        const auto p{ProducersNext()};
        BOOST_CHECK_EQUAL(p.epoch, 1U);
        BOOST_CHECK_EQUAL(p.epoch_start, h_e);
        BOOST_CHECK(*p.current == snap.WithEpoch(1));
    }
    Produce(m_vk_c); // R_h + E
    ProduceTo(h_e + E - 2, m_vk_a);
    BOOST_CHECK_EQUAL(ProducersNext().epoch, 1U);
    Produce(m_vk_a); // h_e + E - 1
    // ... but at exactly h_e + E: ProducerSet_2 = the escape's next set and
    // ProducerSet_3 = Snapshot(h_e + E - 1).
    const auto snap_rot{SnapshotAtTip(3)};
    {
        const auto p{ProducersNext()};
        BOOST_CHECK_EQUAL(p.epoch, 2U);
        BOOST_CHECK_EQUAL(p.epoch_start, h_e + E);
        BOOST_CHECK(*p.current == snap.WithEpoch(2));
        BOOST_CHECK(*p.next == snap_rot);
        BOOST_CHECK_EQUAL(p.last_escape_height, h_e);
        BOOST_CHECK_EQUAL(p.escapes, 1U);
    }
    Produce(m_vk_a); // h_e + E

    // B revokes; a rotation would keep it for its residual epochs, the escape
    // set Snapshot(parent) does not have it.
    const auto revoke_b{MakeBinding(m_validator_b, m_vk_b, nullptr, 1)};
    Produce(m_vk_a, {}, {MakeTx(9, {revoke_b.cell}, {revoke_b.record})}); // h_e + E + 1
    const int h_e2{h_e + E + 4};
    ProduceTo(h_e2 - 1, m_vk_a);
    const auto snap2{SnapshotAtTip(0)}; // {A, C}, W 25
    BOOST_CHECK_EQUAL(snap2.Size(), 2U);
    BOOST_CHECK_EQUAL(snap2.TotalWeight(), 25U);
    BOOST_CHECK(Weights(m_vk_b) == std::make_pair(CAmount{1}, CAmount{26}));
    BOOST_CHECK(WeightsInRound(m_vk_b, R_ESC) == std::make_pair(CAmount{0}, CAmount{25}));
    ExpectRefusedInRound(m_vk_b, R_ESC, "bad-pos-ineligible");
    // A's escape block (a later escape round: every round >= R_escape is one).
    ProduceInRound(m_vk_a, R_ESC + 3); // h_e2
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE(s.producers);
        const ProducerSchedule& p{*s.producers};
        BOOST_CHECK_EQUAL(p.epoch, 3U);
        BOOST_CHECK_EQUAL(p.epoch_start, h_e2);
        BOOST_REQUIRE(p.current && p.next);
        BOOST_CHECK(*p.current == snap2.WithEpoch(3));
        BOOST_CHECK(*p.next == snap2.WithEpoch(4));
        BOOST_CHECK_EQUAL(p.last_escape_height, h_e2);
        BOOST_CHECK_EQUAL(p.escapes, 2U);
    }
    BOOST_CHECK(Weights(m_vk_b) == std::make_pair(CAmount{0}, CAmount{25}));
    ProduceExpectConnectFailure(m_vk_b, {}, 1);
    Produce(m_vk_c); // h_e2 + 1
    BOOST_CHECK(V1FieldsAsWithoutH1());
}

// (i2) The escape race (dependency document sections 5.2 and 13, H-H1). On one
// parent P, A (in force) signs a late block L in round R_escape - 1, and C
// (outside the set in force, in Snapshot(P)) signs an escape block E in round
// R_escape. Both are valid. Every modern-PoS block carries the same work, so
// at equal height the fork choice takes the branch whose first block has the
// earlier timestamp: L. E arrives first and connects; L then reorganizes it
// away. E's child makes E's branch longer and it wins again; L's child
// restores equal height and L's branch wins again. After every switch the
// live tracker equals a tracker rebuilt from scratch (no escape survives on
// L's branch), and the next block follows the winning branch's schedule.
// Consequence (reported, not a rule change): a member in force that holds a
// block for round R_escape - 1 defeats any escape on the same parent.
BOOST_FIXTURE_TEST_CASE(escape_race_late_in_force_block_wins_at_equal_height, H1ChainFixture)
{
    PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/200, /*with_unbound_c=*/true);
    const int M{m_M};
    const int R_h{M + 10};
    constexpr int64_t R_ESC{TEST_ESCAPE_ROUNDS};
    constexpr int64_t SATURATED{7}; // every key in force is eligible from round 7 (fixture weights)
    static_assert(SATURATED < R_ESC);
    ConfigureH1(R_h);

    ProduceTo(R_h + 1, m_vk_a);
    const auto bind_c{MakeBinding(m_validator_c, m_vk_c, &m_bls_c, 0)};
    Produce(m_vk_a, {}, {MakeTx(8, {bind_c.cell}, {bind_c.record})}); // R_h + 2
    const int h_e{R_h + 20};
    ProduceTo(h_e - 1, m_vk_a);
    const CBlockIndex* parent{Tip()};
    BOOST_CHECK(WeightsInRound(m_vk_a, R_ESC - 1) == std::make_pair(CAmount{15}, CAmount{16}));
    BOOST_CHECK(WeightsInRound(m_vk_c, R_ESC - 1) == std::make_pair(CAmount{0}, CAmount{16}));
    BOOST_CHECK(WeightsInRound(m_vk_c, R_ESC) == std::make_pair(CAmount{10}, CAmount{26}));

    const CBlock late{BuildOnParent(parent, m_vk_a, R_ESC - 1)};
    const CBlock escape{BuildOnParent(parent, m_vk_c, R_ESC)};
    BOOST_CHECK_MESSAGE(Probe(late).empty(), Probe(late));
    BOOST_CHECK_MESSAGE(Probe(escape).empty(), Probe(escape));
    BOOST_CHECK_LT(late.GetBlockTime(), escape.GetBlockTime());

    // E first: it connects and restarts the schedule.
    BOOST_REQUIRE(Submit(escape));
    BOOST_CHECK(Tip()->GetBlockHash() == escape.GetHash());
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE(s.producers);
        BOOST_CHECK_EQUAL(s.producers->escapes, 1U);
        BOOST_CHECK_EQUAL(s.producers->last_escape_height, h_e);
    }
    BOOST_CHECK(AsRebuilt());

    // L later, same height: the node reorganizes to L. No escape on L's branch.
    BOOST_REQUIRE(Submit(late));
    BOOST_CHECK(Tip()->GetBlockHash() == late.GetHash());
    BOOST_CHECK_EQUAL(Tip()->nHeight, h_e);
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE(s.producers);
        BOOST_CHECK_EQUAL(s.producers->epoch, 0U);
        BOOST_CHECK_EQUAL(s.producers->epoch_start, R_h);
        BOOST_CHECK_EQUAL(s.producers->escapes, 0U);
        BOOST_REQUIRE(s.producers->current);
        BOOST_CHECK(!s.producers->current->IndexOf(m_vk_c));
    }
    BOOST_CHECK(AsRebuilt());
    BOOST_CHECK(V1FieldsAsWithoutH1());
    BOOST_CHECK(Weights(m_vk_c) == std::make_pair(CAmount{0}, CAmount{16}));

    // E's child: E's branch is longer and wins; the escape is back.
    const CBlock escape_child{BuildOnParent(BlockIndexOf(escape.GetHash()), m_vk_c, SATURATED)};
    BOOST_REQUIRE(Submit(escape_child));
    BOOST_CHECK(Tip()->GetBlockHash() == escape_child.GetHash());
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE(s.producers);
        BOOST_CHECK_EQUAL(s.producers->escapes, 1U);
        BOOST_CHECK_EQUAL(s.producers->last_escape_height, h_e);
        BOOST_REQUIRE(s.producers->current);
        BOOST_CHECK(s.producers->current->IndexOf(m_vk_c).has_value());
    }
    BOOST_CHECK(AsRebuilt());

    // L's child (A, in force on L's branch): equal height again, L's branch wins.
    const CBlock late_child{BuildOnParent(BlockIndexOf(late.GetHash()), m_vk_a, SATURATED)};
    BOOST_REQUIRE(Submit(late_child));
    BOOST_CHECK(Tip()->GetBlockHash() == late_child.GetHash());
    BOOST_CHECK_EQUAL(Tip()->nHeight, h_e + 1);
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE(s.producers);
        BOOST_CHECK_EQUAL(s.producers->escapes, 0U);
    }
    BOOST_CHECK(AsRebuilt());
    BOOST_CHECK(V1FieldsAsWithoutH1());
    BOOST_CHECK(Weights(m_vk_c) == std::make_pair(CAmount{0}, CAmount{16}));
    {
        LOCK(cs_main);
        for (const uint256& h : {escape.GetHash(), escape_child.GetHash()}) {
            const CBlockIndex* index{m_node.chainman->m_blockman.LookupBlockIndex(h)};
            BOOST_REQUIRE(index);
            BOOST_CHECK(!(index->nStatus & BLOCK_FAILED_VALID)); // valid, only outranked
        }
    }
}

// (i3) The same race in the other arrival order: L first, then E. The node
// never connects E, and ends on the same tip as a node that saw E first.
BOOST_FIXTURE_TEST_CASE(escape_race_order_independent, H1ChainFixture)
{
    PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/200, /*with_unbound_c=*/true);
    const int M{m_M};
    const int R_h{M + 10};
    constexpr int64_t R_ESC{TEST_ESCAPE_ROUNDS};
    ConfigureH1(R_h);

    ProduceTo(R_h + 1, m_vk_a);
    const auto bind_c{MakeBinding(m_validator_c, m_vk_c, &m_bls_c, 0)};
    Produce(m_vk_a, {}, {MakeTx(8, {bind_c.cell}, {bind_c.record})}); // R_h + 2
    const int h_e{R_h + 20};
    ProduceTo(h_e - 1, m_vk_a);
    const CBlockIndex* parent{Tip()};
    const CBlock late{BuildOnParent(parent, m_vk_a, R_ESC - 1)};
    const CBlock escape{BuildOnParent(parent, m_vk_c, R_ESC)};

    BOOST_REQUIRE(Submit(late));
    BOOST_CHECK(Tip()->GetBlockHash() == late.GetHash());
    BOOST_REQUIRE(Submit(escape));
    BOOST_CHECK(Tip()->GetBlockHash() == late.GetHash());
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE(s.producers);
        BOOST_CHECK_EQUAL(s.producers->escapes, 0U);
        BOOST_CHECK_EQUAL(s.producers->epoch, 0U);
    }
    BOOST_CHECK(AsRebuilt());
    LOCK(cs_main);
    const CBlockIndex* e{m_node.chainman->m_blockman.LookupBlockIndex(escape.GetHash())};
    BOOST_REQUIRE(e);
    BOOST_CHECK(!(e->nStatus & BLOCK_FAILED_VALID));
}

// (i4) A withheld branch. C escapes on P and C's branch grows to three
// blocks. A, in force on P, made L in round R_escape - 1 and two children,
// and publishes them only now, every one of them later in time than the tip
// it replaces. At equal height the first divergent blocks decide and L is
// one round earlier than E, so the node disconnects all three of C's blocks.
// Within the reorganization horizon nothing bounds the depth but the
// withheld branch's length.
BOOST_FIXTURE_TEST_CASE(escape_race_withheld_branch_replaces_the_escape_branch, H1ChainFixture)
{
    PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/200, /*with_unbound_c=*/true);
    const int M{m_M};
    const int R_h{M + 10};
    constexpr int64_t R_ESC{TEST_ESCAPE_ROUNDS};
    constexpr int64_t SATURATED{7};
    ConfigureH1(R_h);

    ProduceTo(R_h + 1, m_vk_a);
    const auto bind_c{MakeBinding(m_validator_c, m_vk_c, &m_bls_c, 0)};
    Produce(m_vk_a, {}, {MakeTx(8, {bind_c.cell}, {bind_c.record})}); // R_h + 2
    const int h_e{R_h + 20};
    ProduceTo(h_e - 1, m_vk_a);
    const CBlockIndex* parent{Tip()};

    // A's withheld branch starts with L in round R_escape - 1; its two
    // children, also in round R_escape - 1, are built below.
    const CBlock l1{BuildOnParent(parent, m_vk_a, R_ESC - 1)};
    // C's public branch: the escape, then C in force on its own branch.
    const CBlock e1{BuildOnParent(parent, m_vk_c, R_ESC)};
    BOOST_REQUIRE(Submit(e1));
    const CBlock e2{BuildInRound(m_vk_c, SATURATED)};
    BOOST_REQUIRE(Submit(e2));
    const CBlock e3{BuildInRound(m_vk_c, SATURATED)};
    BOOST_REQUIRE(Submit(e3));
    BOOST_REQUIRE(Tip()->GetBlockHash() == e3.GetHash());
    BOOST_CHECK_EQUAL(Tip()->nHeight, h_e + 2);
    BOOST_CHECK(AsRebuilt());

    // The fixture builds only on indexed blocks, so A's blocks reach the node
    // one by one; the first two are shorter and stay a side branch.
    BOOST_REQUIRE(Submit(l1));
    BOOST_CHECK(Tip()->GetBlockHash() == e3.GetHash());
    const CBlock l2{BuildOnParent(BlockIndexOf(l1.GetHash()), m_vk_a, R_ESC - 1)};
    BOOST_CHECK_GT(l2.GetBlockTime(), e2.GetBlockTime());
    BOOST_REQUIRE(Submit(l2));
    BOOST_CHECK(Tip()->GetBlockHash() == e3.GetHash());
    const CBlock l3{BuildOnParent(BlockIndexOf(l2.GetHash()), m_vk_a, R_ESC - 1)};
    BOOST_CHECK_GT(l3.GetBlockTime(), e3.GetBlockTime());
    BOOST_REQUIRE(Submit(l3)); // equal height: L's branch wins
    BOOST_CHECK(Tip()->GetBlockHash() == l3.GetHash());
    BOOST_CHECK_EQUAL(Tip()->nHeight, h_e + 2);
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE(s.producers);
        BOOST_CHECK_EQUAL(s.producers->escapes, 0U);
    }
    BOOST_CHECK(AsRebuilt());
    BOOST_CHECK(V1FieldsAsWithoutH1());
    LOCK(cs_main);
    for (const uint256& h : {e1.GetHash(), e2.GetHash(), e3.GetHash()}) {
        const CBlockIndex* index{m_node.chainman->m_blockman.LookupBlockIndex(h)};
        BOOST_REQUIRE(index);
        BOOST_CHECK(!m_node.chainman->ActiveChain().Contains(index));
        BOOST_CHECK(!(index->nStatus & BLOCK_FAILED_VALID));
    }
}

// (j) An escape at R_h itself: the bootstrap and the escape apply at the same
// height. Below R_h the round is ignored (C, bound but outside the V1 set,
// stays refused even in round R_escape). A's block at R_h in round R_escape
// starts producer epoch 1 at R_h with both sets Snapshot(R_h - 1), so the
// chain can leave R_h through an escape even when no member of the bootstrap
// set produces in a normal round; the next rotation is at R_h + E.
BOOST_FIXTURE_TEST_CASE(escape_at_activation, H1ChainFixture)
{
    PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/200, /*with_unbound_c=*/true);
    const int M{m_M};
    const int E{SCALED_E};
    const int R_h{M + 10};
    constexpr int64_t R_ESC{TEST_ESCAPE_ROUNDS};
    ConfigureH1(R_h);
    const Consensus::Params& consensus{m_node.chainman->GetConsensus()};

    Produce(m_vk_a); // M
    ProduceTo(M + 2, m_vk_a);
    const auto bind_c{MakeBinding(m_validator_c, m_vk_c, &m_bls_c, 0)};
    Produce(m_vk_a, {}, {MakeTx(8, {bind_c.cell}, {bind_c.record})}); // M + 3
    ProduceTo(R_h - 2, m_vk_a);
    BOOST_CHECK(Weights(m_vk_c) == std::make_pair(CAmount{0}, CAmount{16}));
    BOOST_CHECK(WeightsInRound(m_vk_c, R_ESC) == std::make_pair(CAmount{0}, CAmount{16}));
    ExpectRefusedInRound(m_vk_c, R_ESC, "bad-pos-ineligible"); // at R_h - 1
    Produce(m_vk_a); // R_h - 1
    BOOST_CHECK(!FinalityState().producers);

    const auto snap{SnapshotAtTip(0)}; // {A, B, C}, W 26
    BOOST_CHECK_EQUAL(snap.Size(), 3U);
    BOOST_CHECK_EQUAL(snap.TotalWeight(), 26U);
    {
        const auto normal{ProducersNext()};
        BOOST_CHECK_EQUAL(normal.epoch, 0U);
        BOOST_CHECK_EQUAL(normal.epoch_start, R_h);
        BOOST_CHECK(*normal.current == snap);
        BOOST_CHECK_EQUAL(normal.escapes, 0U);
        const auto escape{ProducersNextInRound(R_ESC)};
        BOOST_CHECK_EQUAL(escape.epoch, 1U);
        BOOST_CHECK_EQUAL(escape.epoch_start, R_h);
        BOOST_REQUIRE(escape.current && escape.next);
        BOOST_CHECK(*escape.current == snap.WithEpoch(1));
        BOOST_CHECK(*escape.next == snap.WithEpoch(2));
        BOOST_CHECK_EQUAL(escape.last_escape_height, R_h);
        BOOST_CHECK_EQUAL(escape.escapes, 1U);
        LOCK(cs_main);
        BOOST_CHECK(SameV1(Finality().Projected(R_h, consensus), Finality().Projected(R_h, consensus, R_ESC)));
    }
    BOOST_CHECK(WeightsInRound(m_vk_a, R_ESC) == std::make_pair(CAmount{15}, CAmount{26}));
    const auto escape_next{ProducersNextInRound(R_ESC)};
    ProduceInRound(m_vk_a, R_ESC); // R_h
    BOOST_CHECK(SameProducers(FinalityState().producers, escape_next));
    BOOST_CHECK(V1FieldsAsWithoutH1());

    // The next rotation is at R_h + E: ProducerSet_2 = Snapshot(R_h - 1)
    // re-stamped, ProducerSet_3 = Snapshot(R_h + E - 1).
    ProduceTo(R_h + E - 1, m_vk_a);
    const auto snap_rot{SnapshotAtTip(3)};
    {
        const auto p{ProducersNext()};
        BOOST_CHECK_EQUAL(p.epoch, 2U);
        BOOST_CHECK_EQUAL(p.epoch_start, R_h + E);
        BOOST_CHECK(*p.current == snap.WithEpoch(2));
        BOOST_CHECK(*p.next == snap_rot);
        BOOST_CHECK_EQUAL(p.last_escape_height, R_h);
        BOOST_CHECK_EQUAL(p.escapes, 1U);
    }
    Produce(m_vk_c); // R_h + E
}

// (j') The escape takes Snapshot(parent) whatever its size (finality
// dependency section 5.1, FD-2): it exists for liveness when the set in force
// is offline, so V1's MIN_FINALITY_SET floor (2 here) does not apply to it,
// while bootstrap and rotation keep V1's carry-over. Only an empty snapshot
// carries the set otherwise in force over.
// - A and B revoke before R_h - 1: Snapshot(R_h - 1) is empty, ProducerSet_0
//   is the V1 set {A, B} (bootstrap fallback), and A's escape at R_h carries
//   that set over.
// - C then binds and is the only bound, ACTIVE staker while the set in force
//   ({A, B}) stays offline: Snapshot(h_e - 1) = {C} is below the floor, yet
//   C's escape block connects (in round R_escape - 1 it is refused) and A,
//   in the set in force, is refused in that escape round. C alone produces
//   from then on, and the next rotation carries {C} over (V1's floor).
BOOST_FIXTURE_TEST_CASE(escape_takes_snapshot_below_floor, H1ChainFixture)
{
    PrepareFinalityChain(/*min_finality_set=*/2, /*reorg_horizon=*/200, /*with_unbound_c=*/true);
    const int M{m_M};
    const int E{SCALED_E};
    const int R_h{M + 10};
    constexpr int64_t R_ESC{TEST_ESCAPE_ROUNDS};
    ConfigureH1(R_h);

    Produce(m_vk_a); // M
    const auto v1_set0{FinalityState().current};
    BOOST_REQUIRE(v1_set0);
    BOOST_CHECK_EQUAL(v1_set0->Size(), 2U);
    ProduceTo(M + 2, m_vk_a);
    const auto revoke_a{MakeBinding(m_validator_a, m_vk_a, nullptr, 1)};
    const auto revoke_b{MakeBinding(m_validator_b, m_vk_b, nullptr, 1)};
    Produce(m_vk_a, {}, {MakeTx(4, {revoke_a.cell}, {revoke_a.record}),
                         MakeTx(5, {revoke_b.cell}, {revoke_b.record})}); // M + 3
    ProduceTo(R_h - 1, m_vk_a);

    // R_h: Snapshot(R_h - 1) is empty. Both the bootstrap and the escape
    // carry the V1 set {A, B} over.
    {
        const auto normal{ProducersNext()};
        BOOST_CHECK_EQUAL(normal.epoch, 0U);
        BOOST_REQUIRE(normal.current);
        BOOST_CHECK(*normal.current == v1_set0->WithEpoch(0));
        const auto escape{ProducersNextInRound(R_ESC)};
        BOOST_CHECK_EQUAL(escape.epoch, 1U);
        BOOST_CHECK_EQUAL(escape.epoch_start, R_h);
        BOOST_REQUIRE(escape.current && escape.next);
        BOOST_CHECK(*escape.current == v1_set0->WithEpoch(1));
        BOOST_CHECK(*escape.next == v1_set0->WithEpoch(2));
        BOOST_CHECK_EQUAL(escape.escapes, 1U);
    }
    BOOST_CHECK(WeightsInRound(m_vk_a, R_ESC) == std::make_pair(CAmount{15}, CAmount{16}));
    BOOST_CHECK(WeightsInRound(m_vk_c, R_ESC) == std::make_pair(CAmount{0}, CAmount{16}));
    ProduceInRound(m_vk_a, R_ESC); // R_h
    BOOST_CHECK_EQUAL(FinalityState().producers->escapes, 1U);

    // C binds at R_h + 1; A keeps producing until h_e - 1, then the set in
    // force goes offline.
    const auto bind_c{MakeBinding(m_validator_c, m_vk_c, &m_bls_c, 0)};
    Produce(m_vk_a, {}, {MakeTx(8, {bind_c.cell}, {bind_c.record})}); // R_h + 1
    const int h_e{R_h + 6};
    ProduceTo(h_e - 1, m_vk_a);
    const auto snap_c{SnapshotAtTip(0)};
    BOOST_CHECK_EQUAL(snap_c.Size(), 1U);
    BOOST_CHECK(snap_c.IndexOf(m_vk_c).has_value());
    BOOST_CHECK(Weights(m_vk_c) == std::make_pair(CAmount{0}, CAmount{16}));
    BOOST_CHECK(WeightsInRound(m_vk_c, R_ESC) == std::make_pair(CAmount{10}, CAmount{10}));
    BOOST_CHECK(WeightsInRound(m_vk_a, R_ESC) == std::make_pair(CAmount{0}, CAmount{10}));
    {
        const auto escape{ProducersNextInRound(R_ESC)};
        BOOST_CHECK_EQUAL(escape.epoch, 2U);
        BOOST_CHECK_EQUAL(escape.epoch_start, h_e);
        BOOST_REQUIRE(escape.current && escape.next);
        BOOST_CHECK(*escape.current == snap_c.WithEpoch(2));
        BOOST_CHECK(*escape.next == snap_c.WithEpoch(3));
    }
    ExpectRefusedInRound(m_vk_c, R_ESC - 1, "bad-pos-ineligible");
    ExpectRefusedInRound(m_vk_a, R_ESC, "bad-pos-ineligible");
    ProduceInRound(m_vk_c, R_ESC); // h_e
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE(s.producers && s.producers->current);
        BOOST_CHECK_EQUAL(s.producers->epoch, 2U);
        BOOST_CHECK_EQUAL(s.producers->epoch_start, h_e);
        BOOST_CHECK_EQUAL(s.producers->last_escape_height, h_e);
        BOOST_CHECK_EQUAL(s.producers->escapes, 2U);
        BOOST_CHECK(*s.producers->current == snap_c.WithEpoch(2));
    }
    BOOST_CHECK(Weights(m_vk_c) == std::make_pair(CAmount{10}, CAmount{10}));
    BOOST_CHECK(Weights(m_vk_a) == std::make_pair(CAmount{0}, CAmount{10}));
    ProduceExpectConnectFailure(m_vk_a, {}, 1);
    Produce(m_vk_c); // h_e + 1

    // The rotation at h_e + E keeps V1's floor: Snapshot(h_e + E - 1) = {C}
    // is below it, so ProducerSet_4 carries {C} over.
    ProduceTo(h_e + E - 1, m_vk_c);
    {
        const auto p{ProducersNext()};
        BOOST_CHECK_EQUAL(p.epoch, 3U);
        BOOST_CHECK_EQUAL(p.epoch_start, h_e + E);
        BOOST_REQUIRE(p.current && p.next);
        BOOST_CHECK(*p.current == snap_c.WithEpoch(3));
        BOOST_CHECK(*p.next == snap_c.WithEpoch(4));
    }
    Produce(m_vk_c); // h_e + E
    BOOST_CHECK(V1FieldsAsWithoutH1());
}

// (k) H1-R4 across escapes: an escape is decided by block data alone (the
// exact timestamps of the block and its parent), so the live tracker, a
// tracker stepped only by Sync's walk, a full in-memory rebuild, a fresh
// tracker, a node restart and -reindex-chainstate all reproduce the same
// schedule; the reindex re-judges C's escape block (C was eligible only in
// its escape round) and accepts it again.
BOOST_FIXTURE_TEST_CASE(rebuild_and_reload_reproduce_escape, H1ChainDiskFixture)
{
    PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/200, /*with_unbound_c=*/true);
    const int M{m_M};
    const int E{SCALED_E};
    const int R_h{M + 5};
    constexpr int64_t R_ESC{TEST_ESCAPE_ROUNDS};
    ConfigureH1(R_h);
    const int h_e{R_h + 8};
    const int h_e2{h_e + E + 3};
    const auto bind_c{MakeBinding(m_validator_c, m_vk_c, &m_bls_c, 0)};
    const auto revoke_b{MakeBinding(m_validator_b, m_vk_b, nullptr, 1)};
    FinalityTracker walked;
    while (Tip()->nHeight < h_e2 + 6) {
        const int next{Tip()->nHeight + 1};
        if (next == R_h + 2) {
            Produce(m_vk_a, {}, {MakeTx(8, {bind_c.cell}, {bind_c.record})});
        } else if (next == h_e) {
            BOOST_CHECK_EQUAL(Weights(m_vk_c).first, 0); // C can only enter by the escape
            ProduceInRound(m_vk_c, R_ESC);
        } else if (next == h_e + E - 4) {
            Produce(m_vk_a, {}, {MakeTx(9, {revoke_b.cell}, {revoke_b.record})});
        } else if (next == h_e2) {
            ProduceInRound(m_vk_a, R_ESC + 7);
        } else if (next % 3 == 0 && Weights(m_vk_c).first > 0) {
            Produce(m_vk_c);
        } else if (next % 4 == 1 && Weights(m_vk_b).first > 0) {
            Produce(m_vk_b);
        } else {
            Produce(m_vk_a);
        }
        LOCK(cs_main);
        ChainstateManager& chainman{*m_node.chainman};
        Chainstate& cs{chainman.ActiveChainstate()};
        BOOST_REQUIRE(walked.Sync(cs.m_chain, cs.m_blockman, chainman.GetConsensus(), *cs.m_chain.Tip()));
        const FinalityTracker::State& live{Finality().Current()};
        BOOST_CHECK(SameV1(live, walked.Current()));
        BOOST_CHECK(SameProducers(live.producers, walked.Current().producers));
    }
    const auto before{FinalityState()};
    BOOST_REQUIRE(before.producers);
    BOOST_CHECK_EQUAL(before.producers->epoch, 3U); // escape, rotation at h_e + E, escape
    BOOST_CHECK_EQUAL(before.producers->epoch_start, h_e2);
    BOOST_CHECK_EQUAL(before.producers->last_escape_height, h_e2);
    BOOST_CHECK_EQUAL(before.producers->escapes, 2U);
    BOOST_CHECK(before.producers->current->IndexOf(m_vk_c).has_value());
    BOOST_CHECK(!before.producers->current->IndexOf(m_vk_b).has_value()); // dropped by the escape
    BOOST_CHECK(V1FieldsAsWithoutH1());
    const int height_before{Tip()->nHeight};
    const uint256 tip_before{Tip()->GetBlockHash()};
    const auto weights_c{Weights(m_vk_c)};

    const auto verify{[&] {
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, height_before);
        BOOST_CHECK(Tip()->GetBlockHash() == tip_before);
        const auto after{FinalityState()};
        BOOST_CHECK(SameV1(before, after));
        BOOST_CHECK(SameProducers(before.producers, after.producers));
        BOOST_CHECK(Weights(m_vk_c) == weights_c);
    }};
    // In-memory rebuild of the live tracker, and a fresh tracker.
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().ModernFinality().MarkDirty());
    verify();
    {
        LOCK(cs_main);
        ChainstateManager& chainman{*m_node.chainman};
        Chainstate& cs{chainman.ActiveChainstate()};
        FinalityTracker fresh;
        BOOST_REQUIRE(fresh.Sync(cs.m_chain, cs.m_blockman, chainman.GetConsensus(), *cs.m_chain.Tip()));
        BOOST_CHECK(SameV1(before, fresh.Current()));
        BOOST_CHECK(SameProducers(before.producers, fresh.Current().producers));
    }
    // Restart, then -reindex-chainstate (every block connected again).
    const auto restart{[&](const bool reindex_chainstate) {
        {
            LOCK(cs_main);
            m_node.chainman->ActiveChainstate().ForceFlushStateToDisk();
        }
        m_node.chainman.reset();
        if (reindex_chainstate) m_args.ForceSetArg("-reindex-chainstate", "1");
        m_make_chainman();
        LoadVerifyActivateChainstate();
        if (reindex_chainstate) m_args.ForceSetArg("-reindex-chainstate", "0");
    }};
    restart(/*reindex_chainstate=*/false);
    verify();
    restart(/*reindex_chainstate=*/true);
    verify();
    // The chain extends on the reloaded schedule: C produces, B does not.
    ProduceExpectConnectFailure(m_vk_b, {}, 1);
    Produce(m_vk_c);
    BOOST_CHECK_EQUAL(FinalityState().producers->epoch, 3U);
}

// (l) The block assembler judges every round with the (w, W) validation will
// apply (ModernEligibilityWeights' round). Below R_h one pair governs every
// round: B, revoked but still in the frozen V1 set, builds in round R_escape
// although Snapshot(parent) lacks it, and C (not in the V1 set) is refused in
// round R_escape too. From R_h: B (now in neither set) and the unbound C are
// refused before any round is tried. Once C binds after R_h it is only in the
// escape set: an explicit round below R_escape is not eligible, round
// R_escape gives a template that passes TestBlockValidity and connects as the
// escape, and without a round the search skips every normal round and returns
// exactly R_escape (C is saturated there). A, in both sets, keeps its first
// normal round and is also eligible in an explicit escape round.
BOOST_FIXTURE_TEST_CASE(assembler_escape_round, H1ChainFixture)
{
    PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/200, /*with_unbound_c=*/true);
    const int M{m_M};
    const int R_h{M + 10};
    constexpr int64_t R_ESC{TEST_ESCAPE_ROUNDS};
    ConfigureH1(R_h);
    const Consensus::ModernPosParams& pos{*m_node.chainman->GetConsensus().modern_pos};

    node::BlockAssembler::Options options;
    options.coinbase_output_script = CScript() << OP_TRUE;
    // The template's own TestBlockValidity applies the future-time rule:
    // move the mock clock to the tip's round-R_escape time, the latest round
    // any template below claims.
    const auto assemble{[&](const modern::PosValidatorKey& key, const std::optional<int64_t> round) {
        const int64_t latest{modern::ModernPosBlockTime(Tip()->GetBlockTime(), R_ESC, pos)};
        if (latest > GetTime()) SetMockTime(latest);
        options.modern_pos_validator_key = key;
        options.modern_pos_round = round ? std::optional{static_cast<uint32_t>(*round)} : std::nullopt;
        return node::BlockAssembler(m_node.chainman->ActiveChainstate(), nullptr, options).CreateNewBlock();
    }};
    //! The consensus round a template on the tip claims (its exact time).
    const auto round_of{[&](const CBlock& block) {
        BOOST_REQUIRE(block.hashPrevBlock == Tip()->GetBlockHash());
        const auto round{modern::DecodeModernPosRound(Tip()->GetBlockTime(), block.GetBlockTime(), pos)};
        BOOST_REQUIRE(round.has_value());
        return *round;
    }};
    const HasReason not_in_set{"validator is not in the active validator set (no bound, active stake)"};
    const HasReason not_eligible{"validator is not eligible in the requested modern-PoS round"};

    Produce(m_vk_a); // M
    ProduceTo(M + 2, m_vk_a);
    const auto revoke_b{MakeBinding(m_validator_b, m_vk_b, nullptr, 1)};
    Produce(m_vk_a, {}, {MakeTx(4, {revoke_b.cell}, {revoke_b.record})}); // M + 3
    ProduceTo(R_h - 2, m_vk_a);

    // R_h - 1, below R_h: the round never selects another set.
    BOOST_CHECK(Weights(m_vk_b) == std::make_pair(CAmount{1}, CAmount{16}));
    BOOST_CHECK(WeightsInRound(m_vk_b, R_ESC) == std::make_pair(CAmount{1}, CAmount{16}));
    BOOST_CHECK_EQUAL(round_of(assemble(m_vk_b, R_ESC)->block), R_ESC);
    BOOST_CHECK(round_of(assemble(m_vk_b, std::nullopt)->block) < R_ESC);
    BOOST_CHECK_EXCEPTION(assemble(m_vk_c, R_ESC), std::runtime_error, not_in_set);
    Produce(m_vk_a); // R_h - 1

    // R_h: ProducerSet_0 = Snapshot(R_h - 1) = {A}, which is also the escape
    // set; B and the unbound C are in neither.
    BOOST_CHECK(Weights(m_vk_b) == std::make_pair(CAmount{0}, CAmount{15}));
    BOOST_CHECK(WeightsInRound(m_vk_b, R_ESC) == std::make_pair(CAmount{0}, CAmount{15}));
    for (const auto& key : {m_vk_b, m_vk_c}) {
        for (const std::optional<int64_t> round : {std::optional<int64_t>{}, std::optional<int64_t>{0},
                                                   std::optional<int64_t>{R_ESC}}) {
            BOOST_CHECK_EXCEPTION(assemble(key, round), std::runtime_error, not_in_set);
        }
    }
    BOOST_CHECK(round_of(assemble(m_vk_a, std::nullopt)->block) < R_ESC);

    // C binds at R_h + 2: in Snapshot(h_e - 1) = {A, C} (W 25), not in the
    // producer set in force ({A}, W 15).
    ProduceTo(R_h + 1, m_vk_a);
    const auto bind_c{MakeBinding(m_validator_c, m_vk_c, &m_bls_c, 0)};
    Produce(m_vk_a, {}, {MakeTx(8, {bind_c.cell}, {bind_c.record})}); // R_h + 2
    const int h_e{R_h + 8};
    ProduceTo(h_e - 1, m_vk_a);
    BOOST_CHECK(Weights(m_vk_c) == std::make_pair(CAmount{0}, CAmount{15}));
    BOOST_CHECK(WeightsInRound(m_vk_c, R_ESC) == std::make_pair(CAmount{10}, CAmount{25}));
    BOOST_CHECK(WeightsInRound(m_vk_a, R_ESC) == std::make_pair(CAmount{15}, CAmount{25}));

    BOOST_CHECK_EXCEPTION(assemble(m_vk_c, 0), std::runtime_error, not_eligible);
    BOOST_CHECK_EXCEPTION(assemble(m_vk_c, R_ESC - 1), std::runtime_error, not_eligible);
    const auto searched{assemble(m_vk_c, std::nullopt)};
    BOOST_CHECK_EQUAL(round_of(searched->block), R_ESC);
    BOOST_CHECK(round_of(assemble(m_vk_a, std::nullopt)->block) < R_ESC);
    BOOST_CHECK_EQUAL(round_of(assemble(m_vk_a, R_ESC)->block), R_ESC);
    BOOST_CHECK_EXCEPTION(assemble(m_vk_b, std::nullopt), std::runtime_error, not_in_set);
    BOOST_CHECK_EXCEPTION(assemble(m_vk_b, R_ESC), std::runtime_error, not_in_set);

    // C's explicit round-R_escape template connects as the escape at h_e.
    CBlock block{assemble(m_vk_c, R_ESC)->block};
    BOOST_CHECK_EQUAL(block.GetBlockTime(), searched->block.GetBlockTime());
    block.hashMerkleRoot = BlockMerkleRoot(block);
    Sign(block, m_validator_c);
    const std::string probe{Probe(block)};
    BOOST_REQUIRE_MESSAGE(probe.empty(), probe);
    BOOST_REQUIRE(Submit(block));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, h_e);
    BOOST_CHECK(Tip()->GetBlockHash() == block.GetHash());
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE(s.producers && s.producers->current);
        BOOST_CHECK_EQUAL(s.producers->epoch, 1U);
        BOOST_CHECK_EQUAL(s.producers->epoch_start, h_e);
        BOOST_CHECK_EQUAL(s.producers->last_escape_height, h_e);
        BOOST_CHECK_EQUAL(s.producers->escapes, 1U);
        BOOST_CHECK(s.producers->current->IndexOf(m_vk_c).has_value());
    }
    BOOST_CHECK(V1FieldsAsWithoutH1());
    // C now assembles in a normal round.
    BOOST_CHECK(Weights(m_vk_c) == std::make_pair(CAmount{10}, CAmount{25}));
    BOOST_CHECK(round_of(assemble(m_vk_c, std::nullopt)->block) < R_ESC);
}

// (m) StakingLoop's local regtest policy (preserve_regtest_finality) pauses
// production on a broken V1 lineage only below R_h. From R_h block producers
// no longer depend on that lineage (finality dependency section 5.1), so the
// guarded assembler builds again; its template connects and the V1 lineage
// stays broken. The unguarded assembler is unchanged throughout, and the same
// chain without H1 stays paused.
BOOST_FIXTURE_TEST_CASE(regtest_broken_pause_lifted_from_rh, H1ChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    const int R_h{M + 70}; // no certificate ever: V1 breaks at M + E + MAX_EXTENSION = M + 60
    ConfigureH1(R_h);

    node::BlockAssembler::Options options;
    options.coinbase_output_script = CScript() << OP_TRUE;
    options.modern_pos_validator_key = m_vk_a;
    options.preserve_regtest_finality = true;
    const auto assemble{[&] {
        return node::BlockAssembler(m_node.chainman->ActiveChainstate(), nullptr, options).CreateNewBlock();
    }};
    const HasReason paused{"regtest finality lineage is already broken; local production paused"};

    ProduceTo(M + SCALED_E + SCALED_MAX_EXTENSION, m_vk_a); // manual producer: V1 breaks at M + 60
    BOOST_REQUIRE(FinalityState().lineage_broken);
    BOOST_CHECK_EXCEPTION(assemble(), std::runtime_error, paused);
    ProduceTo(R_h - 2, m_vk_a);
    BOOST_CHECK_EXCEPTION(assemble(), std::runtime_error, paused); // R_h - 1
    options.preserve_regtest_finality = false;
    BOOST_CHECK(assemble()); // the default assembler never pauses
    options.preserve_regtest_finality = true;
    Produce(m_vk_a); // R_h - 1

    // R_h: the guarded template builds, validates and connects.
    CBlock block{assemble()->block};
    block.hashMerkleRoot = BlockMerkleRoot(block);
    Sign(block, m_validator_a);
    if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
    BOOST_REQUIRE(Submit(block));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, R_h);
    BOOST_CHECK(Tip()->GetBlockHash() == block.GetHash());
    BOOST_CHECK(!block.vtx.at(0)->HasMpa()); // no certificate: nothing to include
    {
        const auto s{FinalityState()};
        BOOST_CHECK(s.lineage_broken);
        BOOST_REQUIRE(s.producers);
        BOOST_CHECK_EQUAL(s.producers->epoch_start, R_h);
    }
    BOOST_CHECK(assemble()); // R_h + 1

    // The same chain judged without H1 stays paused.
    {
        Consensus::Params& consensus{MutableConsensus()};
        const auto saved{consensus.h1};
        consensus.h1.reset();
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().ModernFinality().MarkDirty());
        BOOST_CHECK_EXCEPTION(assemble(), std::runtime_error, paused);
        consensus.h1 = saved;
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().ModernFinality().MarkDirty());
    }
    BOOST_CHECK(assemble());
}

// (m') From R_h the policy no longer pauses for the V1 lineage at all (regtest
// plan section 3.6: H1 switches local production to the producer schedule).
// At an intact lineage's last handover carrier (REQUIRE_HANDOVER) the guarded
// assembler still includes a pooled current-epoch certificate, but with none
// it builds anyway instead of stopping every staking loop for good: with
// R_h = M + 10 and epoch 0 never certified, its template at the carrier
// M + 59 connects without a certificate and V1 breaks at M + 60, after which
// it keeps building. The same chain judged without H1 still refuses the
// carrier ("handover required").
BOOST_FIXTURE_TEST_CASE(regtest_handover_best_effort_under_h1, H1ChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    const int R_h{M + 10};
    ConfigureH1(R_h);

    node::BlockAssembler::Options options;
    options.coinbase_output_script = CScript() << OP_TRUE;
    options.modern_pos_validator_key = m_vk_a;
    options.preserve_regtest_finality = true;
    const auto assemble{[&] {
        return node::BlockAssembler(m_node.chainman->ActiveChainstate(), nullptr, options).CreateNewBlock();
    }};
    const HasReason handover{"regtest finality handover required"};

    const int last_carrier{M + SCALED_E + SCALED_MAX_EXTENSION - 1};
    ProduceTo(last_carrier - 1, m_vk_a);
    BOOST_REQUIRE(FinalityState().producers);
    BOOST_CHECK(!FinalityState().lineage_broken);
    BOOST_CHECK(!ProjectedNext().handover_certified);

    // The same chain judged without H1: the V1 policy preserves the carrier.
    {
        Consensus::Params& consensus{MutableConsensus()};
        const auto saved{consensus.h1};
        consensus.h1.reset();
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().ModernFinality().MarkDirty());
        BOOST_CHECK_EXCEPTION(assemble(), std::runtime_error, handover);
        consensus.h1 = saved;
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().ModernFinality().MarkDirty());
    }

    // From R_h: no certificate in the pool, the carrier is built anyway.
    CBlock block{assemble()->block};
    BOOST_CHECK(!block.vtx.at(0)->HasMpa()); // nothing to include
    block.hashMerkleRoot = BlockMerkleRoot(block);
    Sign(block, m_validator_a);
    if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
    BOOST_REQUIRE(Submit(block));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, last_carrier);
    BOOST_CHECK(Tip()->GetBlockHash() == block.GetHash());
    BOOST_CHECK(!FinalityState().lineage_broken);
    BOOST_CHECK(ProjectedNext().lineage_broken); // V1 breaks at M + 60
    BOOST_CHECK(assemble()); // and production goes on
    BOOST_CHECK(V1FieldsAsWithoutH1());
}

// (n) H2 above R_h (the follow-up release's "harden a block above R_h",
// finality dependency section 5.3). The pin at R_h + 1 comes from the
// -b3h2checkpoint knob's display-order hash (an internal-order hash pins
// another identity), and it is enforced at every point of the existing
// mechanism:
// - Header admission refuses a competing child of R_h with
//   bad-modern-checkpoint before it enters the block index. Two competitors
//   are tried: another member's block (B) and the pinned producer's own
//   block in a later round. Both are fully valid without the pin, and the
//   same header is admitted as a side branch once the pin is removed.
// - ConnectBlock's recheck, which also covers -reindex-chainstate, refuses
//   a competitor admitted before the pin when a longer branch tries to
//   activate it.
// - The startup check refuses an active chain that does not match the pin.
// The H1 producer schedule is unaffected throughout.
BOOST_FIXTURE_TEST_CASE(h2_pin_above_rh_refuses_competitor, H1ChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    const int R_h{M + 10};
    const int pinned{R_h + 1};
    ConfigureH1(R_h);
    ChainstateManager& chainman{*m_node.chainman};
    Consensus::Params& consensus{MutableConsensus()};
    const Consensus::ModernPosParams& pos{*consensus.modern_pos};
    BOOST_REQUIRE(consensus.modern_checkpoints.empty());
    const auto lookup{[&](const uint256& hash) {
        return WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(hash));
    }};
    const auto status_of{[&](const uint256& hash) {
        LOCK(cs_main);
        const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(hash)};
        BOOST_REQUIRE(index != nullptr);
        return index->nStatus;
    }};

    ProduceTo(R_h, m_vk_a);
    // ProducerSet_0 = Snapshot(R_h - 1) = {A 15, B 1}: both may produce R_h + 1.
    BOOST_REQUIRE(FinalityState().producers);
    BOOST_CHECK_EQUAL(FinalityState().producers->epoch_start, R_h);
    const auto [w_a, W_a] = Weights(m_vk_a);
    BOOST_CHECK_EQUAL(w_a, 15);
    BOOST_CHECK_EQUAL(W_a, 16);
    BOOST_CHECK(Weights(m_vk_b) == std::make_pair(CAmount{1}, CAmount{16}));
    const CBlockIndex* parent{Tip()};
    const auto round_of{[&](const CBlock& block) {
        BOOST_REQUIRE(block.hashPrevBlock == parent->GetBlockHash());
        const auto round{modern::DecodeModernPosRound(parent->GetBlockTime(), block.GetBlockTime(), pos)};
        BOOST_REQUIRE(round.has_value());
        return *round;
    }};

    // Three children of R_h: A's block in its first eligible round (the one
    // that will be pinned), B's block, and A's block in a later eligible
    // round. None is an escape block.
    const CBlock honest{BuildPosBlock(m_vk_a)};
    const auto built_b{BuildPosBlockOnSeed(parent, SeedFor(parent), m_vk_b)};
    const CBlock& by_key{built_b.first};
    const int64_t honest_round{round_of(honest)};
    int64_t later_round{-1};
    for (int64_t r{honest_round + 1}; r < TEST_ESCAPE_ROUNDS && later_round < 0; ++r) {
        const uint256 digest{modern::ModernPosEligibilityDigest(Domain(), SeedFor(parent), static_cast<uint32_t>(pinned),
                                                                static_cast<uint32_t>(r), m_vk_a)};
        if (modern::ModernPosEligible(digest, w_a, W_a, r, pos)) later_round = r;
    }
    BOOST_REQUIRE(later_round > honest_round);
    const CBlock by_round{BuildInRound(m_vk_a, later_round)};
    BOOST_CHECK_EQUAL(round_of(by_round), later_round);
    BOOST_CHECK(round_of(by_key) < TEST_ESCAPE_ROUNDS);
    for (const CBlock* block : {&honest, &by_key}) {
        if (block->GetBlockTime() > GetTime()) SetMockTime(block->GetBlockTime());
    }
    const std::vector<const CBlock*> rivals{&by_key, &by_round};
    for (const CBlock* rival : rivals) BOOST_CHECK(rival->GetHash() != honest.GetHash());
    // Without a pin each one is a valid child of R_h.
    for (const CBlock* block : {&honest, &by_key, &by_round}) {
        const std::string probe{Probe(*block)};
        BOOST_CHECK_MESSAGE(probe.empty(), "unpinned child of R_h refused: " << probe);
    }

    // Pin A's block at R_h + 1 through the -b3h2checkpoint knob. It takes the
    // hash in display order, as getblockhash prints it, and CreateChainParams
    // turns the line into exactly the identity validation compares. The same
    // hash written in internal byte order pins another identity and would
    // refuse A's block.
    const auto table_from_line{[&](const std::string& hex) {
        const auto params{ParsedChain(FlowmeshLine({strprintf("-b3h2checkpoint=%d:%s", pinned, hex)}))};
        return params->GetConsensus().modern_checkpoints;
    }};
    const std::string display{honest.GetHash().GetHex()};
    const std::string internal{HexStr(honest.GetHash())};
    BOOST_REQUIRE(display != internal);
    const auto from_internal{table_from_line(internal)};
    BOOST_REQUIRE_EQUAL(from_internal.size(), 1U);
    BOOST_CHECK(from_internal.at(pinned) != honest.GetHash());
    consensus.modern_checkpoints = from_internal;
    BOOST_CHECK(Probe(honest).find("bad-modern-checkpoint") != std::string::npos);
    const auto from_display{table_from_line(display)};
    BOOST_CHECK(from_display == (std::map<int, uint256>{{pinned, honest.GetHash()}}));
    // Validation admits exactly that identity.
    consensus.modern_checkpoints = from_display;
    BOOST_CHECK(Consensus::ModernCheckpointAllows(consensus, pinned, honest.GetHash()));
    BOOST_CHECK(Probe(honest).empty());
    for (const CBlock* rival : rivals) {
        BOOST_CHECK(!Consensus::ModernCheckpointAllows(consensus, pinned, rival->GetHash()));
        // Only the pinned height is constrained.
        BOOST_CHECK(Consensus::ModernCheckpointAllows(consensus, pinned - 1, rival->GetHash()));
        BOOST_CHECK(Consensus::ModernCheckpointAllows(consensus, pinned + 1, rival->GetHash()));
        const std::string probe{Probe(*rival)};
        BOOST_CHECK_MESSAGE(probe.find("bad-modern-checkpoint") != std::string::npos, "pinned rival: '" << probe << "'");
    }
    BOOST_REQUIRE(Submit(honest)); // ConnectBlock's recheck admits the pinned block
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, pinned);
    BOOST_CHECK(Tip()->GetBlockHash() == honest.GetHash());
    BOOST_CHECK_EQUAL(ChainHashAt(pinned).GetHex(), display);
    Produce(m_vk_b); // R_h + 2
    const uint256 tip_hash{Tip()->GetBlockHash()};
    const auto producers{FinalityState().producers};
    BOOST_REQUIRE(producers);
    BOOST_CHECK_EQUAL(producers->epoch, 0U);
    BOOST_CHECK_EQUAL(producers->epoch_start, R_h);

    // Header admission: each rival is refused before it enters the index,
    // as a header and as a full block.
    for (const CBlock* rival : rivals) {
        BlockValidationState state;
        BOOST_CHECK(!chainman.ProcessNewBlockHeaders({{CBlockHeader{*rival}}}, /*min_pow_checked=*/true, state));
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_INVALID_HEADER);
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-modern-checkpoint");
        BOOST_CHECK(lookup(rival->GetHash()) == nullptr);
        BOOST_CHECK(!Submit(*rival));
        BOOST_CHECK(lookup(rival->GetHash()) == nullptr);
    }
    BOOST_CHECK(Tip()->GetBlockHash() == tip_hash);

    // Without the pin the same headers are admitted as a side branch.
    consensus.modern_checkpoints.erase(pinned);
    for (const CBlock* rival : rivals) {
        BlockValidationState state;
        const CBlockIndex* index{nullptr};
        BOOST_CHECK(chainman.ProcessNewBlockHeaders({{CBlockHeader{*rival}}}, /*min_pow_checked=*/true, state, &index));
        BOOST_CHECK_MESSAGE(state.IsValid(), state.ToString());
        BOOST_REQUIRE(index != nullptr);
        BOOST_CHECK(index->GetBlockHash() == rival->GetHash());
        BOOST_CHECK_EQUAL(index->nHeight, pinned);
        BOOST_CHECK(!(status_of(rival->GetHash()) & BLOCK_FAILED_VALID));
        BOOST_CHECK(!WITH_LOCK(cs_main, return chainman.ActiveChain().Contains(index)));
    }
    // B's full block is stored; with less work than the tip it stays inactive.
    BOOST_REQUIRE(Submit(by_key));
    BOOST_CHECK(status_of(by_key.GetHash()) & BLOCK_HAVE_DATA);
    BOOST_CHECK(Tip()->GetBlockHash() == tip_hash);

    // ConnectBlock's recheck: with the pin back, B's stored block can never
    // connect. A branch on it with more work than the active chain is tried
    // and refused at B's block, and the pinned chain stays active.
    consensus.modern_checkpoints[pinned] = honest.GetHash();
    {
        ASSERT_DEBUG_LOG("ConnectBlock " + by_key.GetHash().ToString() + " failed, bad-modern-checkpoint");
        const CBlockIndex* side_parent{lookup(by_key.GetHash())};
        BOOST_REQUIRE(side_parent != nullptr);
        uint256 seed{built_b.second};
        for (int h{pinned + 1}; h <= pinned + 2; ++h) {
            auto [side, digest] = BuildPosBlockOnSeed(side_parent, seed, m_vk_a, {}, {}, /*extra=*/600 + h);
            if (side.GetBlockTime() > GetTime()) SetMockTime(side.GetBlockTime());
            BOOST_REQUIRE(Submit(side));
            side_parent = lookup(side.GetHash());
            BOOST_REQUIRE(side_parent != nullptr);
            seed = digest;
        }
        BOOST_CHECK_EQUAL(side_parent->nHeight, pinned + 2);
        BOOST_CHECK(!WITH_LOCK(cs_main, return chainman.ActiveChain().Contains(side_parent)));
    }
    BOOST_CHECK(status_of(by_key.GetHash()) & BLOCK_FAILED_VALID);
    BOOST_CHECK(Tip()->GetBlockHash() == tip_hash);
    BOOST_CHECK(ChainHashAt(pinned) == honest.GetHash());
    BOOST_CHECK(SameProducers(FinalityState().producers, producers));

    // The startup check: an active chain whose block at R_h + 1 is not the
    // pinned identity stops startup before the chainstate is used.
    {
        uint256 wrong{honest.GetHash()};
        wrong.begin()[0] ^= 1;
        consensus.modern_checkpoints[pinned] = wrong;
        node::ChainstateLoadOptions options;
        options.mempool = Assert(m_node.mempool.get());
        options.coins_db_in_memory = m_coins_db_in_memory;
        options.prune = chainman.m_blockman.IsPruneMode();
        const auto [status, error]{node::VerifyLoadedChainstate(chainman, options)};
        BOOST_CHECK(status == node::ChainstateLoadStatus::FAILURE);
        BOOST_CHECK_MESSAGE(error.original.find(strprintf("hardened modern checkpoint at height %d", pinned)) !=
                                std::string::npos,
                            error.original);
        consensus.modern_checkpoints[pinned] = honest.GetHash();
    }

    // The pinned chain extends on the unchanged schedule.
    Produce(m_vk_b); // R_h + 3
    BOOST_CHECK_EQUAL(FinalityState().producers->epoch_start, R_h);
    BOOST_CHECK(V1FieldsAsWithoutH1());
}

// (q) The quorum-reduction counter-argument (interim repair, task 3.1).
// Lowering the V1 certificate threshold cannot unfreeze a broken lineage:
// once the extension is exhausted, placement refuses every certificate as
// lineage-broken before it looks at a signature or a quorum. The test signs
// a certificate with every member of the frozen set (so it meets any
// threshold, 1-of-2 and 2-of-3 included), shows that exactly this block is
// valid one height before the break, and that it is refused after it. The
// V1 set stays frozen; from R_h the H1 schedule lets the new stake produce.
BOOST_FIXTURE_TEST_CASE(unanimous_certificate_after_break_refused_h1_unfreezes, H1ChainFixture)
{
    // V1 Set_0 = {A 15, B 1}, W 16; C (10) is staked but not bound.
    PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/200, /*with_unbound_c=*/true);
    const int M{m_M};
    const int E{SCALED_E};
    const int break_height{M + E + SCALED_MAX_EXTENSION};
    const int R_h{break_height + 10};
    ConfigureH1(R_h);

    Produce(m_vk_a); // M: V1 bootstrap
    const auto set0{FinalityState().current};
    BOOST_REQUIRE(set0);
    BOOST_CHECK_EQUAL(set0->Size(), 2U);
    BOOST_CHECK_EQUAL(set0->TotalWeight(), 16U);

    // C binds at M + 5; V1 never hands over, so C never enters its set.
    ProduceTo(M + 4, m_vk_a);
    const auto bind_c{MakeBinding(m_validator_c, m_vk_c, &m_bls_c, 0)};
    Produce(m_vk_a, {}, {MakeTx(8, {bind_c.cell}, {bind_c.record})});

    // The handover certificate of epoch 0, signed by A and B: every member.
    ProduceTo(break_height - 2, m_vk_a);
    const auto before{FinalityState()};
    BOOST_REQUIRE(before.next);
    BOOST_CHECK(!before.lineage_broken);
    int cp{M};
    while (!modern::IsCheckpointHeight(cp, M, SCALED_INTERVAL)) ++cp;
    const auto cert{MakeCertificate({cp, 0, before.next->SetHash()}, *set0)};
    {
        // One height before the break the same certificate is valid.
        const CBlock block{BuildPosBlock(m_vk_a, {cert})};
        BOOST_CHECK_EQUAL(Probe(block), "");
    }
    Produce(m_vk_a); // break_height - 1, without the certificate
    BOOST_CHECK(!FinalityState().lineage_broken);
    Produce(m_vk_a); // break_height: the extension is exhausted
    BOOST_CHECK(FinalityState().lineage_broken);
    {
        const CBlock block{BuildPosBlock(m_vk_a, {cert})};
        const std::string probe{Probe(block)};
        BOOST_CHECK_MESSAGE(probe.find("lineage-broken") != std::string::npos, "got '" << probe << "'");
    }
    ProduceExpectConnectFailure(m_vk_a, {cert});
    {
        const auto s{FinalityState()};
        BOOST_CHECK(s.lineage_broken);
        BOOST_CHECK_EQUAL(s.epoch, 0U);
        BOOST_CHECK(HashOf(s.current) == set0->SetHash());
        BOOST_CHECK(!s.finalized);
    }

    // Frozen through R_h - 1: C, ACTIVE and bound, has no weight.
    ProduceTo(R_h - 2, m_vk_a);
    BOOST_CHECK(Weights(m_vk_c) == std::make_pair(CAmount{0}, CAmount{16}));
    ProduceExpectConnectFailure(m_vk_c, {}, 1);
    Produce(m_vk_a); // R_h - 1

    // From R_h the producer set is Snapshot(R_h - 1) = {A, B, C}, W 26.
    BOOST_CHECK(Weights(m_vk_c) == std::make_pair(CAmount{10}, CAmount{26}));
    Produce(m_vk_c); // R_h: the new stake produces
    const auto s{FinalityState()};
    BOOST_REQUIRE(s.producers);
    BOOST_CHECK_EQUAL(s.producers->epoch_start, R_h);
    BOOST_CHECK(s.lineage_broken); // V1 itself stays frozen and broken
    BOOST_CHECK(HashOf(s.current) == set0->SetHash());
}

// ---------------------------------------------------------------------------
// B0a (doc/design/b3-height-c-fast-track.md section 4): bounded undo of the
// derived stake and finality state. A disconnect the undo history covers
// restores the parent's state in place; it must equal a tracker rebuilt from
// scratch on the new active chain, and must not start a rebuild.

namespace {

struct UndoChainFixture : public H1ChainFixture {
    explicit UndoChainFixture(TestOpts opts = {}) : H1ChainFixture{std::move(opts)} {}
    //! The live trackers, read without syncing them (a sync would rebuild a
    //! dirty tracker and hide the missing undo).
    FinalityTracker& LiveFinality() EXCLUSIVE_LOCKS_REQUIRED(cs_main)
    {
        return m_node.chainman->ActiveChainstate().ModernFinality();
    }
    node::StakeTracker& LiveStakes() EXCLUSIVE_LOCKS_REQUIRED(cs_main)
    {
        return m_node.chainman->ActiveChainstate().ModernStakeTracker();
    }
    //! The chainstate's stake registry is created on first use (validation
    //! reads the finality tracker's own); sync it once so that validation
    //! keeps it in step from here on.
    void ArmStakes()
    {
        LOCK(cs_main);
        Chainstate& cs{m_node.chainman->ActiveChainstate()};
        BOOST_REQUIRE(LiveStakes().Sync(cs.m_chain, cs.m_blockman, m_node.chainman->GetConsensus(), *cs.m_chain.Tip()));
    }
    //! Rebuilds of both live trackers (the finality tracker's own and its
    //! private stake registry's, and the chainstate's stake registry).
    uint64_t Rebuilds()
    {
        LOCK(cs_main);
        return LiveFinality().Rebuilds() + LiveStakes().Rebuilds();
    }
    //! Both live trackers are synced to the tip as they stand, and each
    //! equals a tracker rebuilt from scratch on the active chain.
    bool InStepAndAsRebuilt()
    {
        {
            LOCK(cs_main);
            const uint256 tip{Tip()->GetBlockHash()};
            if (!LiveFinality().Synced(tip) || !LiveStakes().Synced(tip)) return false;
        }
        if (!AsRebuilt()) return false;
        LOCK(cs_main);
        Chainstate& cs{m_node.chainman->ActiveChainstate()};
        node::StakeTracker rebuilt;
        BOOST_REQUIRE(rebuilt.Sync(cs.m_chain, cs.m_blockman, m_node.chainman->GetConsensus(), *cs.m_chain.Tip()));
        return LiveStakes().SameRegistry(rebuilt);
    }
    //! Disconnect the active block at `height` and every block above it.
    void InvalidateFrom(const int height)
    {
        CBlockIndex* index{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain()[height])};
        BOOST_REQUIRE(index != nullptr);
        BlockValidationState state;
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, index));
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, height - 1);
    }
    //! Undo InvalidateFrom: the same blocks reconnect.
    void Reconsider(const uint256& hash)
    {
        {
            LOCK(cs_main);
            CBlockIndex* index{m_node.chainman->m_blockman.LookupBlockIndex(hash)};
            BOOST_REQUIRE(index != nullptr);
            m_node.chainman->ActiveChainstate().ResetBlockFailureFlags(index);
            m_node.chainman->RecalculateBestHeader();
        }
        BlockValidationState state;
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
    }
    //! A transaction that creates a STAKE output (vout 0) for `vk` from
    //! funding output `n`.
    CMutableTransaction StakeTx(const unsigned n, const modern::ValidatorKeyBytes& vk, const CAmount amount)
    {
        CMutableTransaction tx{MakeTx(n, {}, {})};
        tx.vout.insert(tx.vout.begin(), CTxOut{amount, modern::MakeStakeScript(vk, CScript() << OP_TRUE)});
        tx.vout.back().nValue -= amount;
        return tx;
    }
    //! The owner's spend of STAKE output `stake`:0 (unstaking).
    static CMutableTransaction Unstake(const CMutableTransaction& stake)
    {
        CMutableTransaction tx;
        tx.version = 2;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint{stake.GetHash(), 0};
        tx.vout.emplace_back(stake.vout[0].nValue - 1'000, CScript() << OP_TRUE);
        return tx;
    }
};

} // namespace

// (B0a-1) A one-block reorganization reverts both trackers in place: no
// rebuild, and each equals a tracker rebuilt from scratch.
BOOST_FIXTURE_TEST_CASE(b0a_one_block_reorg_does_not_rebuild, UndoChainFixture)
{
    PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/200, /*with_unbound_c=*/true);
    const int M{m_M};
    ConfigureH1(M + 5);
    while (Tip()->nHeight < M + 12) ProduceCertifying(m_vk_a);
    ArmStakes();
    BOOST_REQUIRE(InStepAndAsRebuilt());
    const uint64_t rebuilds{Rebuilds()};

    const int h{Tip()->nHeight};
    InvalidateFrom(h);
    BOOST_CHECK(InStepAndAsRebuilt());
    BOOST_CHECK_EQUAL(Rebuilds(), rebuilds);
    // A different block at the same height takes its place.
    Produce(m_vk_b);
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, h);
    BOOST_CHECK(InStepAndAsRebuilt());
    BOOST_CHECK_EQUAL(Rebuilds(), rebuilds);
}

// (B0a-2) Undo equals replay at every depth the finality pin allows (a
// disconnect at or below the finalized checkpoint is refused), over a chain
// that crosses, above that checkpoint: the block carrying V1 epoch 0's
// handover certificate, V1's rotation to epoch 1, H1's activation and first
// rotation, a FINALITY_KEY binding, STAKE creations, a block that creates
// and spends one STAKE output, an escape and an unstake. Only epoch 0 is
// certified, so the pin stays at its first checkpoint. After each
// disconnect and after reconnecting the same blocks, both trackers equal a
// rebuild and no rebuild ran.
BOOST_FIXTURE_TEST_CASE(b0a_undo_equals_replay_at_every_depth, UndoChainFixture)
{
    PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/200, /*with_unbound_c=*/true);
    const int M{m_M};
    const int E{SCALED_E};
    const int R_h{M + 10};
    constexpr int64_t R_ESC{TEST_ESCAPE_ROUNDS};
    ConfigureH1(R_h);

    const CMutableTransaction stake_d{StakeTx(4, m_vk_d, STAKE_D)};
    const CMutableTransaction stake_d2{StakeTx(5, m_vk_d, STAKE_D)};
    const auto bind_c{MakeBinding(m_validator_c, m_vk_c, &m_bls_c, 0)};
    const int h_e{R_h + E + 5};
    // Certify V1 epoch 0 only (its handover rotates V1 at M + E); later
    // blocks carry no certificate, so the pin stays in epoch 0.
    const auto produce{[&](std::vector<CMutableTransaction> txs = {}) {
        if (Tip()->nHeight + 1 <= M + E) {
            ProduceCertifying(m_vk_a, std::move(txs));
        } else {
            Produce(m_vk_a, {}, std::move(txs));
        }
    }};
    while (Tip()->nHeight < h_e - 1) {
        const int next{Tip()->nHeight + 1};
        if (next == R_h + 2) {
            produce({MakeTx(8, {bind_c.cell}, {bind_c.record})});
        } else if (next == R_h + 4) {
            produce({stake_d});
        } else if (next == R_h + 5) {
            produce({stake_d2, Unstake(stake_d2)}); // created and spent in one block
        } else {
            produce();
        }
    }
    // C is bound but outside the producer set in force: its escape block.
    BOOST_REQUIRE(!ProducersNext().current->IndexOf(m_vk_c).has_value());
    ProduceInRound(m_vk_c, R_ESC); // h_e
    BOOST_REQUIRE_EQUAL(FinalityState().producers->last_escape_height, h_e);
    produce();
    produce({Unstake(stake_d)}); // h_e + 2
    while (Tip()->nHeight < h_e + 6) produce();

    const auto before{FinalityState()};
    BOOST_REQUIRE(before.finalized.has_value());
    const int pin{before.finalized->height};
    BOOST_REQUIRE_LT(pin, before.finalized->certified_at); // the certificate's block is undone too
    BOOST_REQUIRE_LT(before.finalized->certified_at, R_h);
    BOOST_REQUIRE_EQUAL(before.epoch, 1U);
    BOOST_REQUIRE_EQUAL(before.epoch_starts.back(), M + E);
    BOOST_REQUIRE(before.producers.has_value());
    BOOST_REQUIRE_GE(before.producers->epoch, 2U);
    ArmStakes();
    BOOST_REQUIRE(InStepAndAsRebuilt());
    const uint64_t rebuilds{Rebuilds()};
    const int tip_height{Tip()->nHeight};
    const uint256 tip_hash{Tip()->GetBlockHash()};

    for (int depth{1}; depth <= tip_height - pin; ++depth) {
        const int from{tip_height - depth + 1};
        const uint256 first{ChainHashAt(from)};
        InvalidateFrom(from);
        BOOST_CHECK_MESSAGE(InStepAndAsRebuilt(), "after disconnecting " << depth << " blocks");
        BOOST_CHECK_EQUAL(Rebuilds(), rebuilds);
        Reconsider(first);
        BOOST_REQUIRE(Tip()->GetBlockHash() == tip_hash);
        BOOST_CHECK_MESSAGE(InStepAndAsRebuilt(), "after reconnecting " << depth << " blocks");
        BOOST_CHECK_EQUAL(Rebuilds(), rebuilds);
    }
    const auto after{FinalityState()};
    BOOST_CHECK(SameV1(before, after));
    BOOST_CHECK(SameProducers(before.producers, after.producers));
}

// (B0a-3) A disconnect deeper than the kept history falls back to the
// rebuild, which still equals a fresh tracker and refills the history: the
// next one-block reorganization is again undone in place.
BOOST_FIXTURE_TEST_CASE(b0a_disconnect_beyond_history_rebuilds, UndoChainFixture)
{
    constexpr int HORIZON{4};
    PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/HORIZON, /*with_unbound_c=*/true);
    const int M{m_M};
    ConfigureH1(M + 5);
    while (Tip()->nHeight < M + 14) ProduceCertifying(m_vk_a);
    ArmStakes();
    BOOST_REQUIRE(InStepAndAsRebuilt());
    const uint64_t rebuilds{Rebuilds()};
    const uint256 tip_hash{Tip()->GetBlockHash()};

    const int from{Tip()->nHeight - HORIZON};
    const uint256 first{ChainHashAt(from)};
    InvalidateFrom(from); // HORIZON + 1 blocks: one more than the history holds
    BOOST_CHECK(AsRebuilt());
    BOOST_CHECK_GT(Rebuilds(), rebuilds);
    Reconsider(first);
    BOOST_REQUIRE(Tip()->GetBlockHash() == tip_hash);
    ArmStakes(); // validation never syncs the chainstate's registry
    BOOST_CHECK(InStepAndAsRebuilt());

    const uint64_t refilled{Rebuilds()};
    InvalidateFrom(Tip()->nHeight);
    BOOST_CHECK(InStepAndAsRebuilt());
    BOOST_CHECK_EQUAL(Rebuilds(), refilled);
}

// ---------------------------------------------------------------------------
// B0b (doc/design/b3-b0b-durable-derived-state.md, section 4.3): the finality
// group's record and its parts. A decoded tracker must equal the one that
// wrote it, undo history included, and every byte string that is not exactly
// such an encoding at its block must be refused.

namespace {

//! Every offset at which `needle` starts in `bytes`.
std::vector<size_t> Occurrences(const std::vector<unsigned char>& bytes, const std::span<const unsigned char> needle)
{
    std::vector<size_t> at;
    for (auto it{bytes.begin()}; (it = std::search(it, bytes.end(), needle.begin(), needle.end())) != bytes.end(); ++it) {
        at.push_back(static_cast<size_t>(it - bytes.begin()));
    }
    return at;
}

//! The serialization of `value`, to search for.
template <typename T>
std::vector<unsigned char> Serialized(const T& value)
{
    std::vector<unsigned char> out;
    VectorWriter{out, 0} << value;
    return out;
}

//! The little-endian integer at `at`, as the codecs write integers.
template <typename T>
T GetLE(const std::vector<unsigned char>& bytes, const size_t at)
{
    uint64_t value{0};
    for (size_t i{0}; i < sizeof(T); ++i) value |= uint64_t{bytes[at + i]} << (8 * i);
    return static_cast<T>(value);
}

template <typename T>
void PutLE(std::vector<unsigned char>& bytes, const size_t at, const T value)
{
    for (size_t i{0}; i < sizeof(T); ++i) bytes[at + i] = static_cast<unsigned char>(static_cast<uint64_t>(value) >> (8 * i));
}

} // namespace

// (B0b-1) The stake registry's encoding gives back the registry, its undo
// history and its block. The decoded tracker catches up forward and undoes
// in place, through an unstake and a block that creates and spends one
// output, without a rebuild. After a reorganization the live tracker's
// shorter history is a suffix of a rebuild's, and decodes as such. Every
// truncation, an extra byte, another block, a shorter horizon, reordered or
// repeated outpoints, an amount outside MoneyRange, more stake than money,
// a creation height outside the modern era up to the block, an undo record
// off the chain and a bad flag are refused.
BOOST_FIXTURE_TEST_CASE(b0b_stake_registry_codec, UndoChainFixture)
{
    constexpr int HORIZON{12};
    PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/HORIZON, /*with_unbound_c=*/true);
    const int M{m_M};
    const CMutableTransaction stake_d{StakeTx(4, m_vk_d, STAKE_D)};
    const CMutableTransaction stake_d2{StakeTx(5, m_vk_d, STAKE_D)};
    while (Tip()->nHeight < M + 2) Produce(m_vk_a);
    Produce(m_vk_a, {}, {stake_d});                     // M + 3
    Produce(m_vk_a, {}, {stake_d2, Unstake(stake_d2)}); // M + 4: created and spent in one block
    Produce(m_vk_a);
    Produce(m_vk_a, {}, {Unstake(stake_d)}); // M + 6
    while (Tip()->nHeight < M + 9) Produce(m_vk_a);
    ArmStakes();
    {
        LOCK(cs_main);
        Chainstate& cs{m_node.chainman->ActiveChainstate()};
        const Consensus::Params& params{m_node.chainman->GetConsensus()};
        const CBlockIndex& tip{*cs.m_chain.Tip()};
        const int H{*Consensus::LegacyFinalHeight(params)};
        const node::StakeTracker& live{LiveStakes()};
        BOOST_REQUIRE(live.Synced(tip.GetBlockHash()));
        node::StakeTracker rebuilt;
        BOOST_REQUIRE(rebuilt.Sync(cs.m_chain, cs.m_blockman, params, tip));
        BOOST_REQUIRE(live.SameState(rebuilt) && rebuilt.SameState(live));

        const std::vector<unsigned char> bytes{live.Encode()};
        const auto decoded{node::StakeTracker::Decode(bytes, tip, params)};
        BOOST_REQUIRE(decoded);
        BOOST_CHECK(decoded->Synced(tip.GetBlockHash()));
        BOOST_CHECK(decoded->SameState(live) && live.SameState(*decoded));
        BOOST_CHECK(decoded->Encode() == bytes);
        BOOST_CHECK_EQUAL(decoded->Rebuilds(), 0U);

        // Decoded three blocks back, it catches up forward.
        {
            const CBlockIndex& older{*cs.m_chain[tip.nHeight - 3]};
            node::StakeTracker before;
            BOOST_REQUIRE(before.Sync(cs.m_chain, cs.m_blockman, params, older));
            auto resumed{node::StakeTracker::Decode(before.Encode(), older, params)};
            BOOST_REQUIRE(resumed);
            BOOST_REQUIRE(resumed->Sync(cs.m_chain, cs.m_blockman, params, tip));
            BOOST_CHECK_EQUAL(resumed->Rebuilds(), 0U);
            BOOST_CHECK(resumed->SameState(rebuilt) && rebuilt.SameState(*resumed));
        }
        // It undoes in place, down to M + 2: each parent as rebuilt there.
        {
            node::StakeTracker undone{*decoded};
            for (int height{tip.nHeight}; height > M + 2; --height) {
                undone.BlockDisconnected(*cs.m_chain[height]);
                node::StakeTracker at_parent;
                BOOST_REQUIRE(at_parent.Sync(cs.m_chain, cs.m_blockman, params, *cs.m_chain[height - 1]));
                BOOST_CHECK_MESSAGE(undone.SameState(at_parent), "after undoing height " << height);
            }
            BOOST_CHECK_EQUAL(undone.Rebuilds(), 0U);
        }

        const auto refused{[&](const std::vector<unsigned char>& doctored, const CBlockIndex& at,
                               const Consensus::Params& with) {
            return !node::StakeTracker::Decode(doctored, at, with).has_value();
        }};
        for (size_t n{0}; n < bytes.size(); ++n) {
            BOOST_CHECK(refused({bytes.begin(), bytes.begin() + n}, tip, params));
        }
        std::vector<unsigned char> longer{bytes};
        longer.push_back(0);
        BOOST_CHECK(refused(longer, tip, params));
        // Another block; a horizon shorter than the history, or longer.
        BOOST_CHECK(refused(bytes, *tip.pprev, params));
        Consensus::Params other{params};
        other.modern_pos->reorg_horizon = HORIZON - 1;
        BOOST_CHECK(refused(bytes, tip, other));
        other.modern_pos->reorg_horizon = HORIZON + 1;
        BOOST_CHECK(!refused(bytes, tip, other));

        // The registry: a count byte, then 80-byte entries: outpoint (36),
        // amount (8), validator key (32), creation height (4). A, B and C
        // staked at H + 1; both of D's stakes are spent.
        BOOST_REQUIRE_EQUAL(bytes[0], 3);
        const auto entry{[](const size_t i) { return size_t{1} + 80 * i; }};
        {
            std::vector<unsigned char> swapped{bytes};
            std::swap_ranges(swapped.begin() + entry(0), swapped.begin() + entry(1), swapped.begin() + entry(1));
            BOOST_CHECK(refused(swapped, tip, params));
            std::vector<unsigned char> twice{bytes};
            std::copy(twice.begin() + entry(0), twice.begin() + entry(1), twice.begin() + entry(1));
            BOOST_CHECK(refused(twice, tip, params));
        }
        {
            const auto amount{[&](const size_t i) { return entry(i) + 36; }};
            // Last, so that the running total stays in range.
            std::vector<unsigned char> negative{bytes};
            PutLE<int64_t>(negative, amount(2), -1);
            BOOST_CHECK(refused(negative, tip, params));
            // Up to MAX_MONEY in all, and not one unit more.
            const int64_t rest{GetLE<int64_t>(bytes, amount(1)) + GetLE<int64_t>(bytes, amount(2))};
            std::vector<unsigned char> all{bytes};
            PutLE<int64_t>(all, amount(0), MAX_MONEY - rest);
            BOOST_CHECK(!refused(all, tip, params));
            PutLE<int64_t>(all, amount(0), MAX_MONEY - rest + 1);
            BOOST_CHECK(refused(all, tip, params));
        }
        {
            const size_t created{entry(0) + 36 + 8 + 32};
            BOOST_REQUIRE_EQUAL(GetLE<int32_t>(bytes, created), H + 1);
            std::vector<unsigned char> doctored{bytes};
            PutLE<int32_t>(doctored, created, H);
            BOOST_CHECK(refused(doctored, tip, params));
            PutLE<int32_t>(doctored, created, tip.nHeight + 1);
            BOOST_CHECK(refused(doctored, tip, params));
            // Created at the tip itself decodes, but is not the state.
            PutLE<int32_t>(doctored, created, tip.nHeight);
            const auto later{node::StakeTracker::Decode(doctored, tip, params)};
            BOOST_REQUIRE(later);
            BOOST_CHECK(!later->SameState(rebuilt));
        }
        // The newest and the oldest undo record, each naming another block.
        for (const int height : {tip.nHeight, tip.nHeight - HORIZON + 1}) {
            const uint256 hash{cs.m_chain[height]->GetBlockHash()};
            const auto at{Occurrences(bytes, std::span<const unsigned char>{hash.begin(), hash.end()})};
            BOOST_REQUIRE_EQUAL(at.size(), 1U);
            std::vector<unsigned char> doctored{bytes};
            doctored[at[0]] ^= 1;
            BOOST_CHECK_MESSAGE(refused(doctored, tip, params), "undo record of height " << height);
        }
        // D's first stake: created at M + 3 (flag 0), spent at M + 6 (flag 1,
        // then the entry it removed).
        {
            const auto at{Occurrences(bytes, Serialized(COutPoint{stake_d.GetHash(), 0}))};
            BOOST_REQUIRE_EQUAL(at.size(), 2U);
            BOOST_REQUIRE_EQUAL(bytes[at[0] + 36], 0);
            BOOST_REQUIRE_EQUAL(bytes[at[1] + 36], 1);
            std::vector<unsigned char> flag{bytes};
            flag[at[1] + 36] = 2;
            BOOST_CHECK(refused(flag, tip, params));
            // The entry it removed is checked as a kept one is.
            std::vector<unsigned char> over{bytes};
            PutLE<int64_t>(over, at[1] + 37, MAX_MONEY + 1);
            BOOST_CHECK(refused(over, tip, params));
            std::vector<unsigned char> early{bytes};
            PutLE<int32_t>(early, at[1] + 37 + 8 + 32, H);
            BOOST_CHECK(refused(early, tip, params));
            // A removed entry one unit off decodes, but is not the state.
            std::vector<unsigned char> prior{bytes};
            PutLE<int64_t>(prior, at[1] + 37, GetLE<int64_t>(bytes, at[1] + 37) - 1);
            const auto wrong{node::StakeTracker::Decode(prior, tip, params)};
            BOOST_REQUIRE(wrong);
            BOOST_CHECK(!wrong->SameState(rebuilt));
        }
    }

    // Two blocks undone and one in their place: the live history is one
    // record short of a rebuild's, a suffix of it, and decodes as such.
    InvalidateFrom(Tip()->nHeight - 1);
    Produce(m_vk_b);
    {
        LOCK(cs_main);
        Chainstate& cs{m_node.chainman->ActiveChainstate()};
        const Consensus::Params& params{m_node.chainman->GetConsensus()};
        const CBlockIndex& tip{*cs.m_chain.Tip()};
        const node::StakeTracker& live{LiveStakes()};
        BOOST_REQUIRE(live.Synced(tip.GetBlockHash()));
        node::StakeTracker rebuilt;
        BOOST_REQUIRE(rebuilt.Sync(cs.m_chain, cs.m_blockman, params, tip));
        BOOST_CHECK(live.SameState(rebuilt));
        BOOST_CHECK(!rebuilt.SameState(live));
        const auto decoded{node::StakeTracker::Decode(live.Encode(), tip, params)};
        BOOST_REQUIRE(decoded);
        BOOST_CHECK(decoded->SameState(rebuilt) && decoded->SameState(live) && live.SameState(*decoded));
    }
}

// (B0b-2) The finality group's parameters digest changes with every field its
// derivation reads, every bridge pin and the A1, A2 and A3 heights included
// (a superset), tells an unset field from a zero one, and ignores fields
// outside that set.
BOOST_AUTO_TEST_CASE(b0b_finality_group_params_digest)
{
    using P = Consensus::Params;
    P params{CreateChainParams(ArgsManager{}, ChainType::REGTEST)->GetConsensus()};
    // Every field set, each optional one to something other than its zero.
    params.legacy_b3coin = true;
    params.hard_fork_height = 100;
    params.legacy_final_hash = uint256{7};
    params.transition_pow_length = 5;
    params.modern_pos = Consensus::ModernPosParams{};
    params.modern_pos->treasury_percent = 10;
    params.modern_pos->treasury_script = {0x51};
    params.h1 = Consensus::H1Params{.activation_height = 140, .escape_rounds = 9};
    params.fresh_lineage.reset();
    params.bridge_v2.reset();
    params.fn_pod_activation_height = 150;
    params.asset_activation_height = 160;
    params.flowmesh_activation_height = 170;
    params.bridge_withdrawal_activation_height = 180;
    Consensus::BridgeAssetParams& bridge{params.busd_bridge.emplace()};
    bridge.asset.origin_chain_id = 1;
    bridge.asset.vault_address[0] = 1;
    bridge.asset.token_address[0] = 2;
    bridge.asset.origin_decimals = 6;
    bridge.asset.asset_decimals = 6;
    bridge.origin_deployment_block = 11;
    bridge.vault_runtime_code_hash = uint256{12};
    bridge.implementation_or_adapter = uint256{13};
    bridge.adapter_version = 14;
    bridge.recipient_encoding_version = 15;
    bridge.activation_height = 16;
    bridge.approval_last_height = 17;
    bridge.mint_caps = Consensus::BridgeMintCaps{.max_per_block = 18, .max_per_epoch = 19, .epoch_length_blocks = 20};
    Consensus::EthereumLightClientPins& light{bridge.light_client.emplace()};
    light.trusted_checkpoint_root = uint256{21};
    light.trusted_checkpoint_slot = 22;
    light.genesis_validators_root = uint256{23};
    light.fork_schedule = {Consensus::EthereumForkVersionPin{.activation_epoch = 24, .fork_version = {1, 2, 3, 4}}};
    light.fork_schedule_valid_through_epoch = 25;
    light.electra_epoch = 26;
    light.min_sync_committee_participants = 27;
    light.max_sync_lag_slots = 28;
    bridge.withdrawal_mode = Consensus::BridgeWithdrawalMode::MANAGED_V1;
    Consensus::BridgeManagedWithdrawalPins& managed{bridge.managed_withdrawal.emplace()};
    managed.authority_address[0] = 3;
    managed.vault_runtime_code_hash = uint256{29};
    managed.withdrawal_rules_version = 30;
    managed.withdrawal_rules_commitment = uint256{31};
    Consensus::BridgeDecentralizedWithdrawalPins& decentralized{bridge.decentralized_withdrawal.emplace()};
    decentralized.ethereum_verifier_address[0] = 4;
    decentralized.ethereum_verifier_code_hash = uint256{32};
    decentralized.bootstrap_validator_set_hash = uint256{33};
    decentralized.withdrawal_rules_version = 34;
    decentralized.withdrawal_rules_commitment = uint256{35};
    decentralized.min_bridge_validators = 36;
    decentralized.max_bridge_validators = 37;
    decentralized.min_bridge_total_weight = 38;
    decentralized.max_epoch_lag = 39;

    using Mutate = std::function<void(P&)>;
    const auto digest_with{[&](const Mutate& mutate) {
        P changed{params};
        mutate(changed);
        return FinalityTracker::ParamsDigest(changed);
    }};
    const uint256 base{FinalityTracker::ParamsDigest(params)};
    const std::vector<std::pair<std::string_view, Mutate>> fields{
        // The binding tracker's, through its own digest.
        {"genesis", [](P& p) { p.hashGenesisBlock = uint256{8}; }},
        {"legacy_b3coin", [](P& p) { p.legacy_b3coin = false; }},
        {"hard_fork_height", [](P& p) { p.hard_fork_height = 101; }},
        {"legacy_final_hash", [](P& p) { p.legacy_final_hash = uint256{9}; }},
        {"transition_pow_length", [](P& p) { p.transition_pow_length = 6; }},
        {"modern_pos", [](P& p) { p.modern_pos.reset(); }},
        {"block_interval_seconds", [](P& p) { ++p.modern_pos->block_interval_seconds; }},
        {"round_seconds", [](P& p) { ++p.modern_pos->round_seconds; }},
        {"f0_num", [](P& p) { ++p.modern_pos->f0_num; }},
        {"f0_den", [](P& p) { ++p.modern_pos->f0_den; }},
        {"sentinel_bits", [](P& p) { ++p.modern_pos->sentinel_bits; }},
        {"max_future_seconds", [](P& p) { ++p.modern_pos->max_future_seconds; }},
        {"reward", [](P& p) { ++p.modern_pos->reward; }},
        {"halving_interval", [](P& p) { ++p.modern_pos->halving_interval; }},
        {"treasury_percent", [](P& p) { ++p.modern_pos->treasury_percent; }},
        {"treasury_script", [](P& p) { p.modern_pos->treasury_script.push_back(0x52); }},
        {"reorg_horizon", [](P& p) { ++*p.modern_pos->reorg_horizon; }},
        {"finality_epoch_blocks", [](P& p) { ++p.modern_pos->finality_epoch_blocks; }},
        {"checkpoint_interval", [](P& p) { ++p.modern_pos->checkpoint_interval; }},
        {"checkpoint_depth", [](P& p) { ++p.modern_pos->checkpoint_depth; }},
        {"max_epoch_extension", [](P& p) { ++p.modern_pos->max_epoch_extension; }},
        {"min_finality_set", [](P& p) { ++p.modern_pos->min_finality_set; }},
        {"h1", [](P& p) { p.h1.reset(); }},
        {"h1.activation_height", [](P& p) { ++p.h1->activation_height; }},
        {"h1.escape_rounds", [](P& p) { ++p.h1->escape_rounds; }},
        {"fresh_lineage", [](P& p) { p.fresh_lineage.emplace(); }},
        {"bridge_v2", [](P& p) { p.bridge_v2.emplace(); }},
        {"fn_pod_activation_height", [](P& p) { ++*p.fn_pod_activation_height; }},
        {"asset_activation_height", [](P& p) { ++*p.asset_activation_height; }},
        {"flowmesh_activation_height", [](P& p) { ++*p.flowmesh_activation_height; }},
        {"bridge_withdrawal_activation_height", [](P& p) { ++*p.bridge_withdrawal_activation_height; }},
        {"busd_bridge", [](P& p) { p.busd_bridge.reset(); }},
        {"asset.version", [](P& p) { ++p.busd_bridge->asset.version; }},
        {"asset.origin_chain_id", [](P& p) { ++p.busd_bridge->asset.origin_chain_id; }},
        {"asset.vault_address", [](P& p) { ++p.busd_bridge->asset.vault_address[19]; }},
        {"asset.token_address", [](P& p) { ++p.busd_bridge->asset.token_address[19]; }},
        {"asset.origin_decimals", [](P& p) { ++p.busd_bridge->asset.origin_decimals; }},
        {"asset.asset_decimals", [](P& p) { ++p.busd_bridge->asset.asset_decimals; }},
        {"origin_deployment_block", [](P& p) { ++*p.busd_bridge->origin_deployment_block; }},
        {"vault_runtime_code_hash", [](P& p) { p.busd_bridge->vault_runtime_code_hash = uint256{40}; }},
        {"implementation_or_adapter", [](P& p) { p.busd_bridge->implementation_or_adapter = uint256{41}; }},
        {"adapter_version", [](P& p) { ++*p.busd_bridge->adapter_version; }},
        {"recipient_encoding_version", [](P& p) { ++*p.busd_bridge->recipient_encoding_version; }},
        {"bridge activation_height", [](P& p) { ++*p.busd_bridge->activation_height; }},
        {"approval_last_height", [](P& p) { ++*p.busd_bridge->approval_last_height; }},
        {"mint_caps", [](P& p) { p.busd_bridge->mint_caps.reset(); }},
        {"max_per_block", [](P& p) { ++p.busd_bridge->mint_caps->max_per_block; }},
        {"max_per_epoch", [](P& p) { ++p.busd_bridge->mint_caps->max_per_epoch; }},
        {"epoch_length_blocks", [](P& p) { ++p.busd_bridge->mint_caps->epoch_length_blocks; }},
        {"light_client", [](P& p) { p.busd_bridge->light_client.reset(); }},
        {"trusted_checkpoint_root", [](P& p) { p.busd_bridge->light_client->trusted_checkpoint_root = uint256{42}; }},
        {"trusted_checkpoint_slot", [](P& p) { ++p.busd_bridge->light_client->trusted_checkpoint_slot; }},
        {"genesis_validators_root", [](P& p) { p.busd_bridge->light_client->genesis_validators_root = uint256{43}; }},
        {"fork_schedule", [](P& p) { p.busd_bridge->light_client->fork_schedule.clear(); }},
        {"fork activation_epoch", [](P& p) { ++p.busd_bridge->light_client->fork_schedule[0].activation_epoch; }},
        {"fork_version", [](P& p) { ++p.busd_bridge->light_client->fork_schedule[0].fork_version[3]; }},
        {"fork_schedule_valid_through_epoch",
         [](P& p) { ++p.busd_bridge->light_client->fork_schedule_valid_through_epoch; }},
        {"electra_epoch", [](P& p) { ++p.busd_bridge->light_client->electra_epoch; }},
        {"min_sync_committee_participants",
         [](P& p) { ++p.busd_bridge->light_client->min_sync_committee_participants; }},
        {"max_sync_lag_slots", [](P& p) { ++p.busd_bridge->light_client->max_sync_lag_slots; }},
        {"withdrawal_mode",
         [](P& p) { p.busd_bridge->withdrawal_mode = Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1; }},
        {"managed_withdrawal", [](P& p) { p.busd_bridge->managed_withdrawal.reset(); }},
        {"managed authority_address", [](P& p) { ++p.busd_bridge->managed_withdrawal->authority_address[19]; }},
        {"managed vault_runtime_code_hash",
         [](P& p) { p.busd_bridge->managed_withdrawal->vault_runtime_code_hash = uint256{44}; }},
        {"managed withdrawal_rules_version",
         [](P& p) { ++p.busd_bridge->managed_withdrawal->withdrawal_rules_version; }},
        {"managed withdrawal_rules_commitment",
         [](P& p) { p.busd_bridge->managed_withdrawal->withdrawal_rules_commitment = uint256{45}; }},
        {"decentralized_withdrawal", [](P& p) { p.busd_bridge->decentralized_withdrawal.reset(); }},
        {"ethereum_verifier_address",
         [](P& p) { ++p.busd_bridge->decentralized_withdrawal->ethereum_verifier_address[19]; }},
        {"ethereum_verifier_code_hash",
         [](P& p) { p.busd_bridge->decentralized_withdrawal->ethereum_verifier_code_hash = uint256{46}; }},
        {"bootstrap_validator_set_hash",
         [](P& p) { p.busd_bridge->decentralized_withdrawal->bootstrap_validator_set_hash = uint256{47}; }},
        {"decentralized withdrawal_rules_version",
         [](P& p) { ++p.busd_bridge->decentralized_withdrawal->withdrawal_rules_version; }},
        {"decentralized withdrawal_rules_commitment",
         [](P& p) { p.busd_bridge->decentralized_withdrawal->withdrawal_rules_commitment = uint256{48}; }},
        {"min_bridge_validators", [](P& p) { ++p.busd_bridge->decentralized_withdrawal->min_bridge_validators; }},
        {"max_bridge_validators", [](P& p) { ++p.busd_bridge->decentralized_withdrawal->max_bridge_validators; }},
        {"min_bridge_total_weight",
         [](P& p) { ++p.busd_bridge->decentralized_withdrawal->min_bridge_total_weight; }},
        {"max_epoch_lag", [](P& p) { ++p.busd_bridge->decentralized_withdrawal->max_epoch_lag; }},
    };
    for (const auto& [name, mutate] : fields) {
        BOOST_CHECK_MESSAGE(digest_with(mutate) != base, "the digest ignores " << name);
    }
    // Unset is not zero.
    const std::vector<std::pair<std::string_view, std::pair<Mutate, Mutate>>> optionals{
        {"reorg_horizon", {[](P& p) { p.modern_pos->reorg_horizon = 0; }, [](P& p) { p.modern_pos->reorg_horizon.reset(); }}},
        {"fn_pod_activation_height",
         {[](P& p) { p.fn_pod_activation_height = 0; }, [](P& p) { p.fn_pod_activation_height.reset(); }}},
        {"asset_activation_height",
         {[](P& p) { p.asset_activation_height = 0; }, [](P& p) { p.asset_activation_height.reset(); }}},
        {"flowmesh_activation_height",
         {[](P& p) { p.flowmesh_activation_height = 0; }, [](P& p) { p.flowmesh_activation_height.reset(); }}},
        {"bridge_withdrawal_activation_height",
         {[](P& p) { p.bridge_withdrawal_activation_height = 0; },
          [](P& p) { p.bridge_withdrawal_activation_height.reset(); }}},
        {"origin_deployment_block",
         {[](P& p) { p.busd_bridge->origin_deployment_block = 0; },
          [](P& p) { p.busd_bridge->origin_deployment_block.reset(); }}},
        {"vault_runtime_code_hash",
         {[](P& p) { p.busd_bridge->vault_runtime_code_hash = uint256{}; },
          [](P& p) { p.busd_bridge->vault_runtime_code_hash.reset(); }}},
        {"implementation_or_adapter",
         {[](P& p) { p.busd_bridge->implementation_or_adapter = uint256{}; },
          [](P& p) { p.busd_bridge->implementation_or_adapter.reset(); }}},
        {"adapter_version",
         {[](P& p) { p.busd_bridge->adapter_version = 0; }, [](P& p) { p.busd_bridge->adapter_version.reset(); }}},
        {"recipient_encoding_version",
         {[](P& p) { p.busd_bridge->recipient_encoding_version = 0; },
          [](P& p) { p.busd_bridge->recipient_encoding_version.reset(); }}},
        {"bridge activation_height",
         {[](P& p) { p.busd_bridge->activation_height = 0; }, [](P& p) { p.busd_bridge->activation_height.reset(); }}},
        {"approval_last_height",
         {[](P& p) { p.busd_bridge->approval_last_height = 0; },
          [](P& p) { p.busd_bridge->approval_last_height.reset(); }}},
        {"mint_caps",
         {[](P& p) { p.busd_bridge->mint_caps = Consensus::BridgeMintCaps{}; },
          [](P& p) { p.busd_bridge->mint_caps.reset(); }}},
        {"light_client",
         {[](P& p) { p.busd_bridge->light_client = Consensus::EthereumLightClientPins{}; },
          [](P& p) { p.busd_bridge->light_client.reset(); }}},
        {"managed_withdrawal",
         {[](P& p) { p.busd_bridge->managed_withdrawal = Consensus::BridgeManagedWithdrawalPins{}; },
          [](P& p) { p.busd_bridge->managed_withdrawal.reset(); }}},
        {"decentralized_withdrawal",
         {[](P& p) { p.busd_bridge->decentralized_withdrawal = Consensus::BridgeDecentralizedWithdrawalPins{}; },
          [](P& p) { p.busd_bridge->decentralized_withdrawal.reset(); }}},
    };
    for (const auto& [name, pair] : optionals) {
        BOOST_CHECK_MESSAGE(digest_with(pair.first) != digest_with(pair.second), "unset " << name << " is zero");
    }
    // Parameters the derivation does not read leave the digest alone.
    BOOST_CHECK(digest_with([](P& p) { ++p.nPowTargetSpacing; }) == base);
    BOOST_CHECK(digest_with([](P& p) { ++p.nPowTargetTimespan; }) == base);
}

namespace {

//! The B0a chain (a certificate, a rotation, H1 with an escape, a binding,
//! stakes created, spent, and created and spent in one block) with A's key
//! rebound at seq 1, so that sets hold a nonzero binding_seq.
struct FinalityGroupFixture : public UndoChainFixture {
    int m_R_h{0};
    int m_escape{0};

    explicit FinalityGroupFixture(TestOpts opts = {}) : UndoChainFixture{std::move(opts)} {}

    void BuildGroupChain(const int horizon)
    {
        PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/horizon, /*with_unbound_c=*/true);
        const int M{m_M};
        const int E{SCALED_E};
        m_R_h = M + 10;
        m_escape = m_R_h + E + 5;
        ConfigureH1(m_R_h);
        const CMutableTransaction stake_d{StakeTx(4, m_vk_d, STAKE_D)};
        const CMutableTransaction stake_d2{StakeTx(5, m_vk_d, STAKE_D)};
        const auto bind_c{MakeBinding(m_validator_c, m_vk_c, &m_bls_c, 0)};
        const auto rebind_a{MakeBinding(m_validator_a, m_vk_a, &m_bls_a, 1)}; // its own key again
        // Certify V1 epoch 0 only (its handover rotates V1 at M + E).
        const auto produce{[&](std::vector<CMutableTransaction> txs = {}) {
            if (Tip()->nHeight + 1 <= M + E) {
                ProduceCertifying(m_vk_a, std::move(txs));
            } else {
                Produce(m_vk_a, {}, std::move(txs));
            }
        }};
        while (Tip()->nHeight < m_escape - 1) {
            const int next{Tip()->nHeight + 1};
            if (next == m_R_h + 2) {
                produce({MakeTx(8, {bind_c.cell}, {bind_c.record})});
            } else if (next == m_R_h + 3) {
                produce({MakeTx(9, {rebind_a.cell}, {rebind_a.record})});
            } else if (next == m_R_h + 4) {
                produce({stake_d});
            } else if (next == m_R_h + 5) {
                produce({stake_d2, Unstake(stake_d2)}); // created and spent in one block
            } else {
                produce();
            }
        }
        // C is bound but outside the producer set in force: its escape block.
        BOOST_REQUIRE(!ProducersNext().current->IndexOf(m_vk_c).has_value());
        ProduceInRound(m_vk_c, TEST_ESCAPE_ROUNDS);
        BOOST_REQUIRE_EQUAL(FinalityState().producers->last_escape_height, m_escape);
        produce();
        produce({Unstake(stake_d)});
        while (Tip()->nHeight < m_escape + 6) produce();
        ArmStakes();
        BOOST_REQUIRE(InStepAndAsRebuilt());
    }
};

//! Where the parts of a finality group record start (section 4.3). Every
//! count here is below 253, so each takes one byte.
struct GroupLayout {
    size_t pops{0};          //!< the proofs: a count, then key || proof
    size_t stakes_prefix{0}; //!< the stake registry's length, then its bytes
    size_t stakes_end{0};
    size_t table{0};         //!< the set count, then the sets
    std::vector<size_t> sets;
    size_t state{0};         //!< the state, after the table
};

GroupLayout LayoutOf(const std::vector<unsigned char>& bytes)
{
    GroupLayout at;
    SpanReader r{bytes};
    const auto here{[&] { return bytes.size() - r.size(); }};
    r.ignore(ReadCompactSize(r)); // the binding index
    const uint64_t pops{ReadCompactSize(r)};
    at.pops = here();
    r.ignore(pops);
    at.stakes_prefix = here();
    r.ignore(ReadCompactSize(r));
    at.stakes_end = here();
    at.table = here();
    const uint64_t sets{ReadCompactSize(r)};
    for (uint64_t i{0}; i < sets; ++i) {
        at.sets.push_back(here());
        r.ignore(8 + 32);
        r.ignore(ReadCompactSize(r) * (32 + 48 + 8 + 4));
    }
    at.state = here();
    return at;
}

//! Where the fields of a state start that begins at `at` and has a finalized
//! checkpoint and a producer schedule: its epoch starts written out (the
//! state) or only counted (a prior state).
struct StateLayout {
    size_t epoch_starts{0}; //!< the first start, when written out
    size_t sets{0};         //!< bootstrap, previous, current, next
    size_t finalized{0};    //!< its height
    size_t producer_epoch_start{0};
    size_t escapes{0};
};

StateLayout StateFields(const size_t at, const size_t starts, const bool prior)
{
    StateLayout s;
    s.epoch_starts = at + 1 + 8 + 1;
    s.sets = s.epoch_starts + (prior ? 0 : 4 * starts);
    s.finalized = s.sets + 4 + 1 + 1 + 1;
    const size_t producers{s.finalized + 4 + 32 + 8 + 4 + 1};
    s.producer_epoch_start = producers + 8;
    s.escapes = s.producer_epoch_start + 4 + 2 + 4;
    return s;
}

} // namespace

// (B0b-3) The finality group's record gives back the tracker it was written
// from: the state, the undo history, every set with its binding_seq, and the
// private stake registry, binding index and proofs. The chain carries a
// certificate, a rotation, H1 with an escape, bindings, a rebinding and
// stake changes. The decoded tracker resumes forward and undoes in place
// without a rebuild; after a reorganization the live tracker's shorter
// history decodes as a suffix of a rebuild's. Nothing is written for a
// tracker out of step or under the fresh lineage or bridge V2. Refused:
// every truncation, a byte more, another block, a shorter horizon (also for
// the finality history alone), proofs of possession that fail, a stored set
// hash that does not match, an undo record or finalized checkpoint off the
// chain, epoch starts not from M, sets of the wrong epochs, a producer
// schedule from before R_h, a non-empty prior state below M, and a set no
// state names. A changed binding_seq or escape count, of the state or of a
// prior state, decodes but is not the state.
BOOST_FIXTURE_TEST_CASE(b0b_finality_group_codec, FinalityGroupFixture)
{
    constexpr int HORIZON{40};
    BuildGroupChain(HORIZON);
    const int M{m_M};
    {
        LOCK(cs_main);
        Chainstate& cs{m_node.chainman->ActiveChainstate()};
        const Consensus::Params& params{m_node.chainman->GetConsensus()};
        const CBlockIndex& tip{*cs.m_chain.Tip()};
        const int H{*Consensus::LegacyFinalHeight(params)};
        const FinalityTracker& live{LiveFinality()};
        BOOST_REQUIRE(live.Synced(tip.GetBlockHash()));
        FinalityTracker rebuilt;
        BOOST_REQUIRE(rebuilt.Sync(cs.m_chain, cs.m_blockman, params, tip));
        BOOST_REQUIRE(live.SameState(rebuilt) && rebuilt.SameState(live));
        // What the record has to keep.
        const FinalityTracker::State& state{live.Current()};
        BOOST_REQUIRE(state.finalized && state.epoch == 1 && state.epoch_starts.size() == 2);
        BOOST_REQUIRE(state.producers && state.producers->escapes == 1);
        BOOST_REQUIRE(std::ranges::any_of(state.next->Members(), [](const auto& m) { return m.binding_seq == 1; }));

        const auto bytes{live.Encode(params)};
        BOOST_REQUIRE(bytes);
        const auto decoded{FinalityTracker::Decode(*bytes, tip, params)};
        BOOST_REQUIRE(decoded);
        BOOST_CHECK(decoded->Synced(tip.GetBlockHash()));
        BOOST_CHECK(decoded->SameState(live) && live.SameState(*decoded) && decoded->SameState(rebuilt));
        BOOST_CHECK(decoded->Encode(params) == bytes);
        BOOST_CHECK_EQUAL(decoded->Rebuilds(), 0U);
        BOOST_CHECK(decoded->PinnedCheckpoint() == live.PinnedCheckpoint());
        // The next block's producers, the escape's from the private trackers.
        for (const std::optional<int64_t> round : {std::optional<int64_t>{}, std::optional<int64_t>{TEST_ESCAPE_ROUNDS}}) {
            const auto ours{decoded->SetInForceAt(tip.nHeight + 1, params, round)};
            const auto theirs{live.SetInForceAt(tip.nHeight + 1, params, round)};
            BOOST_REQUIRE(ours && theirs);
            BOOST_CHECK(*ours == *theirs);
        }

        // Nothing written for a tracker out of step, or under the knobs.
        {
            FinalityTracker dirty{live};
            dirty.MarkDirty();
            BOOST_CHECK(!dirty.Encode(params));
            Consensus::Params fresh{params};
            fresh.fresh_lineage.emplace();
            BOOST_CHECK(!live.Encode(fresh));
            BOOST_CHECK(!FinalityTracker::Decode(*bytes, tip, fresh));
            Consensus::Params v2{params};
            v2.bridge_v2.emplace();
            BOOST_CHECK(!live.Encode(v2));
            BOOST_CHECK(!FinalityTracker::Decode(*bytes, tip, v2));
        }
        // Decoded four blocks back, it catches up forward.
        {
            const CBlockIndex& older{*cs.m_chain[tip.nHeight - 4]};
            FinalityTracker before;
            BOOST_REQUIRE(before.Sync(cs.m_chain, cs.m_blockman, params, older));
            const auto older_bytes{before.Encode(params)};
            BOOST_REQUIRE(older_bytes);
            auto resumed{FinalityTracker::Decode(*older_bytes, older, params)};
            BOOST_REQUIRE(resumed);
            BOOST_REQUIRE(resumed->Sync(cs.m_chain, cs.m_blockman, params, tip));
            BOOST_CHECK_EQUAL(resumed->Rebuilds(), 0U);
            BOOST_CHECK(resumed->SameState(rebuilt) && rebuilt.SameState(*resumed));
        }
        // It undoes in place through the escape: each parent as rebuilt there.
        {
            FinalityTracker undone{*decoded};
            for (int height{tip.nHeight}; height >= m_escape; --height) {
                undone.BlockDisconnected(*cs.m_chain[height], params);
                FinalityTracker at_parent;
                BOOST_REQUIRE(at_parent.Sync(cs.m_chain, cs.m_blockman, params, *cs.m_chain[height - 1]));
                BOOST_CHECK_MESSAGE(undone.SameState(at_parent), "after undoing height " << height);
            }
            BOOST_CHECK_EQUAL(undone.Rebuilds(), 0U);
        }

        const auto refused{[&](const std::vector<unsigned char>& doctored, const CBlockIndex& at,
                               const Consensus::Params& with) {
            return !FinalityTracker::Decode(doctored, at, with).has_value();
        }};
        size_t accepted{0};
        for (size_t n{0}; n < bytes->size(); ++n) {
            accepted += !refused({bytes->begin(), bytes->begin() + n}, tip, params);
        }
        BOOST_CHECK_EQUAL(accepted, 0U);
        std::vector<unsigned char> longer{*bytes};
        longer.push_back(0);
        BOOST_CHECK(refused(longer, tip, params));
        BOOST_CHECK(refused(*bytes, *tip.pprev, params));
        Consensus::Params shorter{params};
        shorter.modern_pos->reorg_horizon = HORIZON - 1;
        BOOST_CHECK(refused(*bytes, tip, shorter));

        const GroupLayout at{LayoutOf(*bytes)};
        // The finality history alone longer than the horizon: with a stake
        // registry of the shorter history spliced in, it decodes under the
        // full horizon only.
        {
            node::StakeTracker stakes;
            BOOST_REQUIRE(stakes.Sync(cs.m_chain, cs.m_blockman, shorter, tip));
            std::vector<unsigned char> spliced{bytes->begin(), bytes->begin() + at.stakes_prefix};
            VectorWriter{spliced, spliced.size()} << stakes.Encode();
            spliced.insert(spliced.end(), bytes->begin() + at.stakes_end, bytes->end());
            BOOST_CHECK(!refused(spliced, tip, params));
            BOOST_CHECK(refused(spliced, tip, shorter));
        }
        // P1: two keys' proofs swapped, each a valid signature by another key.
        {
            BOOST_REQUIRE_GE(int{bytes->at(at.pops)}, 2);
            const size_t first{at.pops + 1 + 48};
            const size_t second{first + 96 + 48};
            std::vector<unsigned char> swapped{*bytes};
            std::swap_ranges(swapped.begin() + first, swapped.begin() + first + 96, swapped.begin() + second);
            BOOST_CHECK(refused(swapped, tip, params));
        }
        // A stored set hash that its members do not give.
        {
            std::vector<unsigned char> doctored{*bytes};
            doctored[at.sets[0] + 8] ^= 1;
            BOOST_CHECK(refused(doctored, tip, params));
        }
        // The tip's hash: the newest undo record of the stake registry, then
        // of the group. Each is refused off the chain.
        size_t newest{0};
        {
            const uint256 hash{tip.GetBlockHash()};
            const auto found{Occurrences(*bytes, std::span<const unsigned char>{hash.begin(), hash.end()})};
            BOOST_REQUIRE_EQUAL(found.size(), 2U);
            BOOST_REQUIRE_GE(found[1], at.state);
            newest = found[1];
            for (const size_t offset : found) {
                std::vector<unsigned char> doctored{*bytes};
                doctored[offset] ^= 1;
                BOOST_CHECK_MESSAGE(refused(doctored, tip, params), "tip hash at " << offset);
            }
        }
        // The state's own fields.
        const StateLayout fields{StateFields(at.state, state.epoch_starts.size(), /*prior=*/false)};
        BOOST_REQUIRE_EQUAL(GetLE<int32_t>(*bytes, fields.epoch_starts), M);
        BOOST_REQUIRE_EQUAL(int{bytes->at(fields.sets + 2)}, 2); // Set_e
        BOOST_REQUIRE_EQUAL(int{bytes->at(fields.sets + 3)}, 3); // Set_{e+1}
        BOOST_REQUIRE_EQUAL(GetLE<int32_t>(*bytes, fields.finalized), state.finalized->height);
        BOOST_REQUIRE_EQUAL(GetLE<int32_t>(*bytes, fields.producer_epoch_start), state.producers->epoch_start);
        BOOST_REQUIRE_EQUAL(GetLE<uint64_t>(*bytes, fields.escapes), 1U);
        // The finalized checkpoint off the chain: in the state, and in the
        // newest prior state, where the hash occurs last (the stake registry
        // and the undo records hold it too, as a block's hash).
        {
            const uint256 hash{state.finalized->block_hash};
            const auto found{Occurrences(*bytes, std::span<const unsigned char>{hash.begin(), hash.end()})};
            const size_t own{fields.finalized + 4};
            BOOST_REQUIRE(std::ranges::find(found, own) != found.end());
            BOOST_REQUIRE_GT(found.back(), newest);
            for (const size_t offset : {own, found.back()}) {
                std::vector<unsigned char> doctored{*bytes};
                doctored[offset] ^= 1;
                BOOST_CHECK_MESSAGE(refused(doctored, tip, params), "finalized hash at " << offset);
            }
        }
        {
            std::vector<unsigned char> doctored{*bytes};
            PutLE<int32_t>(doctored, fields.epoch_starts, M - 1);
            BOOST_CHECK(refused(doctored, tip, params));
            doctored = *bytes;
            std::swap(doctored[fields.sets + 2], doctored[fields.sets + 3]);
            BOOST_CHECK(refused(doctored, tip, params));
            doctored = *bytes;
            PutLE<int32_t>(doctored, fields.producer_epoch_start, m_R_h - 1);
            BOOST_CHECK(refused(doctored, tip, params));
            doctored = *bytes;
            PutLE<uint64_t>(doctored, fields.escapes, 2);
            const auto wrong{FinalityTracker::Decode(doctored, tip, params)};
            BOOST_REQUIRE(wrong);
            BOOST_CHECK(!wrong->SameState(rebuilt));
        }
        // The newest prior state's escape count.
        {
            const StateLayout prior{StateFields(newest + 32, 0, /*prior=*/true)};
            BOOST_REQUIRE_EQUAL(GetLE<uint64_t>(*bytes, prior.escapes), 1U);
            std::vector<unsigned char> doctored{*bytes};
            PutLE<uint64_t>(doctored, prior.escapes, 2);
            const auto wrong{FinalityTracker::Decode(doctored, tip, params)};
            BOOST_REQUIRE(wrong);
            BOOST_CHECK(!wrong->SameState(rebuilt));
        }
        // A member's binding_seq.
        {
            std::optional<size_t> seq_at;
            for (const size_t set : at.sets) {
                const size_t members{bytes->at(set + 8 + 32)};
                for (size_t j{0}; j < members && !seq_at; ++j) {
                    const size_t offset{set + 8 + 32 + 1 + j * 92 + 32 + 48 + 8};
                    if (GetLE<uint32_t>(*bytes, offset) == 1) seq_at = offset;
                }
            }
            BOOST_REQUIRE(seq_at);
            std::vector<unsigned char> doctored{*bytes};
            PutLE<uint32_t>(doctored, *seq_at, 2);
            const auto wrong{FinalityTracker::Decode(doctored, tip, params)};
            BOOST_REQUIRE(wrong);
            BOOST_CHECK(!wrong->SameState(rebuilt));
        }
        // A set no state names: a copy of the first, appended to the table.
        {
            std::vector<unsigned char> doctored{bytes->begin(), bytes->begin() + at.state};
            doctored.insert(doctored.end(), bytes->begin() + at.sets[0], bytes->begin() + at.sets[1]);
            doctored.insert(doctored.end(), bytes->begin() + at.state, bytes->end());
            ++doctored[at.table];
            BOOST_CHECK(refused(doctored, tip, params));
        }
        // Below M a prior state is the empty one: a tracker two blocks past M
        // keeps the priors from H on, and the oldest, as of H, is doctored.
        {
            const CBlockIndex& early{*cs.m_chain[M + 2]};
            FinalityTracker at_early;
            BOOST_REQUIRE(at_early.Sync(cs.m_chain, cs.m_blockman, params, early));
            const auto early_bytes{at_early.Encode(params)};
            BOOST_REQUIRE(early_bytes);
            BOOST_REQUIRE(FinalityTracker::Decode(*early_bytes, early, params));
            const uint256 oldest{cs.m_chain[H + 1]->GetBlockHash()};
            const auto found{Occurrences(*early_bytes, std::span<const unsigned char>{oldest.begin(), oldest.end()})};
            BOOST_REQUIRE_EQUAL(found.size(), 2U); // the stake registry's oldest undo record, then the group's
            std::vector<unsigned char> doctored{*early_bytes};
            BOOST_REQUIRE_EQUAL(int{doctored.at(found[1] + 32)}, 0);
            doctored[found[1] + 32] = 1; // bootstrapped
            BOOST_CHECK(refused(doctored, early, params));
        }
    }

    // Two blocks undone and one in their place: the live history is one
    // record short of a rebuild's, a suffix of it, and decodes as such.
    InvalidateFrom(Tip()->nHeight - 1);
    Produce(m_vk_b);
    {
        LOCK(cs_main);
        Chainstate& cs{m_node.chainman->ActiveChainstate()};
        const Consensus::Params& params{m_node.chainman->GetConsensus()};
        const CBlockIndex& tip{*cs.m_chain.Tip()};
        const FinalityTracker& live{LiveFinality()};
        BOOST_REQUIRE(live.Synced(tip.GetBlockHash()));
        FinalityTracker rebuilt;
        BOOST_REQUIRE(rebuilt.Sync(cs.m_chain, cs.m_blockman, params, tip));
        BOOST_CHECK(live.SameState(rebuilt));
        BOOST_CHECK(!rebuilt.SameState(live));
        const auto bytes{live.Encode(params)};
        BOOST_REQUIRE(bytes);
        const auto decoded{FinalityTracker::Decode(*bytes, tip, params)};
        BOOST_REQUIRE(decoded);
        BOOST_CHECK(decoded->SameState(rebuilt) && decoded->SameState(live) && live.SameState(*decoded));
    }
}

namespace {

//! FinalityGroupFixture on disk, keeping derived state across restarts (as
//! PersistFixture in finality_key_binding_tests).
struct GroupPersistFixture : public FinalityGroupFixture {
    static constexpr std::string_view BUILD{"b0b-test-build"};
    static constexpr node::DerivedStateId ID{node::DerivedStateId::FINALITY_GROUP};
    static constexpr int HORIZON{40};

    GroupPersistFixture() : FinalityGroupFixture{{.coins_db_in_memory = false, .block_tree_db_in_memory = false}}
    {
        // ChainTestingSetup keeps its coins in memory whatever TestOpts say
        // (see finality_pin_persist_tests); reopen them on disk.
        Reload([&] {
            m_coins_db_in_memory = false;
            m_persist_derived_state = true;
            m_derived_state_build_id = std::string{BUILD};
            m_check_derived_state = true;
        });
    }
    //! Start again; `between` runs while no chainstate holds the store.
    void Reload(const std::function<void()>& between = {})
    {
        if (m_node.validation_signals) m_node.validation_signals->SyncWithValidationInterfaceQueue();
        m_node.chainman.reset();
        if (between) between();
        m_make_chainman();
        LoadVerifyActivateChainstate();
    }
    //! Shut down (a forced flush), then Reload.
    void Restart(const std::function<void()>& between = {})
    {
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().ForceFlushStateToDisk());
        Reload(between);
    }
    //! The group chain, then a restart. The first start of a coins database
    //! without the B3 validation-schema marker reconnects every post-H block
    //! once (node/chainstate.cpp), which rebuilds the trackers; later starts
    //! do not, as on a node that has run this release before.
    void PrepareRestarted()
    {
        BuildGroupChain(HORIZON);
        Restart();
    }
    fs::path StorePath() { return m_args.GetDataDirNet() / "derivedstate"; }
    //! The store, opened while no chainstate holds it.
    node::DerivedStateStore OpenStore()
    {
        return node::DerivedStateStore{DBParams{.path = StorePath(), .cache_bytes = 1 << 20}};
    }
    std::optional<std::vector<unsigned char>> ReadRecord() { return OpenStore().Read(ID); }
    void WriteRecord(const std::vector<unsigned char>& bytes) { OpenStore().Write(ID, bytes); }
    node::DerivedStateRecord DecodeRecord(const std::vector<unsigned char>& bytes)
    {
        node::DerivedStateRecord record;
        BOOST_REQUIRE(node::DecodeDerivedStateRecord(bytes, BUILD,
                                                     FinalityTracker::ParamsDigest(m_node.chainman->GetConsensus()),
                                                     record) == node::DerivedStateCheck::OK);
        return record;
    }
    //! Rebuilds of the live finality tracker, its private trackers' included.
    uint64_t GroupRebuilds()
    {
        LOCK(cs_main);
        return LiveFinality().Rebuilds();
    }
    //! The live finality tracker is in step with the tip and equals a tracker
    //! rebuilt from blocks, its undo history a suffix of the rebuild's.
    bool GroupAsRebuilt()
    {
        LOCK(cs_main);
        Chainstate& cs{m_node.chainman->ActiveChainstate()};
        FinalityTracker rebuilt;
        BOOST_REQUIRE(rebuilt.Sync(cs.m_chain, cs.m_blockman, m_node.chainman->GetConsensus(), *cs.m_chain.Tip()));
        return LiveFinality().Synced(cs.m_chain.Tip()->GetBlockHash()) && LiveFinality().SameState(rebuilt);
    }
    //! Restart as Restart does; whether the start logged `message`. A miss
    //! fails the caller's check instead of aborting, so that the checks after
    //! it still report.
    bool RestartLogging(const std::string& message, const std::function<void()>& between = {})
    {
        bool seen{false};
        {
            DebugLogHelper watch{message, [&](const std::string* line) {
                                     if (line) seen = true;
                                     return line != nullptr;
                                 }};
            Restart(between);
        }
        return seen;
    }
    //! One escape more in the state an encoding holds: a wrong state that
    //! passes the record's own checks.
    static void AddEscape(std::vector<unsigned char>& payload)
    {
        const GroupLayout at{LayoutOf(payload)};
        const StateLayout fields{StateFields(at.state, payload.at(at.state + 1 + 8), /*prior=*/false)};
        BOOST_REQUIRE_EQUAL(GetLE<uint64_t>(payload, fields.escapes), 1U);
        PutLE<uint64_t>(payload, fields.escapes, 2);
    }
};

} // namespace

// (B0b-4) The finality group kept across a restart. On the group chain, the
// record the shutdown writes is installed at the next start without a
// rebuild, with the state, the undo history and the private trackers the
// tracker had. A one-block reorganization right after the start is undone
// and redone in place, and the chain extends, still without a rebuild. The
// next shutdown writes both records at the new tip, in one batch.
BOOST_FIXTURE_TEST_CASE(b0b_finality_group_restart, GroupPersistFixture)
{
    PrepareRestarted();
    // As outside regtest: the record is used without a rebuild to check it.
    m_check_derived_state = false;
    const FinalityTracker before{WITH_LOCK(cs_main, return FinalityTracker{LiveFinality()})};
    BOOST_REQUIRE(before.Synced(Tip()->GetBlockHash()));
    Restart();
    BOOST_CHECK_EQUAL(GroupRebuilds(), 0U);
    BOOST_CHECK(GroupAsRebuilt());
    {
        LOCK(cs_main);
        const FinalityTracker& live{LiveFinality()};
        BOOST_CHECK(live.SameState(before) && before.SameState(live));
        BOOST_CHECK(live.PinnedCheckpoint() == before.PinnedCheckpoint());
    }
    const uint256 top{Tip()->GetBlockHash()};
    InvalidateFrom(Tip()->nHeight);
    BOOST_CHECK(GroupAsRebuilt());
    Reconsider(top);
    BOOST_REQUIRE(Tip()->GetBlockHash() == top);
    BOOST_CHECK(GroupAsRebuilt());
    BOOST_CHECK(WITH_LOCK(cs_main, return LiveFinality().SameState(before)));
    Produce(m_vk_a);
    Produce(m_vk_a);
    BOOST_CHECK(GroupAsRebuilt());
    BOOST_CHECK_EQUAL(GroupRebuilds(), 0U);

    // Both records were loaded at this start, at the old tip, so both are due
    // at the shutdown.
    std::optional<std::vector<unsigned char>> group, bindings;
    Restart([&] {
        group = ReadRecord();
        bindings = OpenStore().Read(node::DerivedStateId::FINALITY_BINDINGS);
    });
    BOOST_REQUIRE(group && bindings);
    BOOST_CHECK_EQUAL(DecodeRecord(*group).height, Tip()->nHeight);
    node::DerivedStateRecord index;
    BOOST_REQUIRE(node::DecodeDerivedStateRecord(
                      *bindings, BUILD, node::FinalityBindingTracker::ParamsDigest(m_node.chainman->GetConsensus()),
                      index) == node::DerivedStateCheck::OK);
    BOOST_CHECK_EQUAL(index.height, Tip()->nHeight);
    BOOST_CHECK_EQUAL(GroupRebuilds(), 0U);
}

// (B0b-5) An older record, as after a crash between two writes, is installed
// and brought to the tip by the first FindMostWorkChain without a rebuild. A
// shutdown writes the tip although the record is fewer than
// DERIVED_STATE_WRITE_INTERVAL blocks old. A tracker out of step with the tip
// at a flush is not written: the store keeps the record it has.
BOOST_FIXTURE_TEST_CASE(b0b_finality_group_older_record_and_write_rule, GroupPersistFixture)
{
    PrepareRestarted();
    std::optional<std::vector<unsigned char>> older;
    Restart([&] { older = ReadRecord(); });
    BOOST_REQUIRE(older);
    const int older_height{DecodeRecord(*older).height};
    BOOST_REQUIRE_EQUAL(older_height, Tip()->nHeight);
    Produce(m_vk_a);
    Produce(m_vk_a);
    Restart([&] { WriteRecord(*older); });
    BOOST_CHECK_EQUAL(GroupRebuilds(), 0U);
    BOOST_CHECK(GroupAsRebuilt());

    std::optional<std::vector<unsigned char>> latest;
    Restart([&] { latest = ReadRecord(); });
    BOOST_REQUIRE(latest);
    BOOST_CHECK_EQUAL(DecodeRecord(*latest).height, older_height + 2);
    BOOST_CHECK_EQUAL(Tip()->nHeight, older_height + 2);

    // Synced one block back (a rebuild there), the tracker is out of step
    // with the tip at the shutdown flush.
    Produce(m_vk_a);
    {
        LOCK(cs_main);
        Chainstate& cs{m_node.chainman->ActiveChainstate()};
        BOOST_REQUIRE(LiveFinality().Sync(cs.m_chain, cs.m_blockman, m_node.chainman->GetConsensus(),
                                          *cs.m_chain.Tip()->pprev));
    }
    std::optional<std::vector<unsigned char>> kept;
    Restart([&] { kept = ReadRecord(); });
    BOOST_REQUIRE(kept);
    BOOST_CHECK_EQUAL(DecodeRecord(*kept).height, Tip()->nHeight - 1);
    BOOST_CHECK(GroupAsRebuilt());
}

// (B0b-6) Records the finality group cannot use are passed over, and the
// tracker rebuilds from blocks to the true state: a bad checksum, another
// build, other parameters (forged, and H1's escape rounds changed, which only
// the finality group's digest covers), a malformed or truncated payload, two
// keys' proofs of possession swapped, and, under -checkderivedstate, a wrong
// state that passes the record's own checks. Without the check, a record
// whose block left the active chain is passed over, and one that passes its
// own checks is trusted, as the coins database is.
BOOST_FIXTURE_TEST_CASE(b0b_finality_group_unusable_records_are_rebuilt, GroupPersistFixture)
{
    PrepareRestarted();
    std::optional<std::vector<unsigned char>> good;
    Restart([&] { good = ReadRecord(); });
    BOOST_REQUIRE(good);
    BOOST_REQUIRE_EQUAL(GroupRebuilds(), 0U);
    const node::DerivedStateRecord record{DecodeRecord(*good)};
    const auto with{[&](const auto& mutate, const std::string_view build = BUILD) {
        node::DerivedStateRecord forged{record};
        mutate(forged);
        return node::EncodeDerivedStateRecord(forged, build);
    }};
    const auto refused{[&](const std::vector<unsigned char>& forged, const std::string_view what) {
        Restart([&] { WriteRecord(forged); });
        BOOST_CHECK_MESSAGE(GroupRebuilds() > 0, what << ": installed");
        BOOST_CHECK_MESSAGE(GroupAsRebuilt(), what << ": not the true state");
    }};
    std::vector<unsigned char> flipped{*good};
    flipped.back() ^= 1;
    refused(flipped, "bad checksum");
    refused(with([](auto&) {}, "another-build"), "another build");
    refused(with([](auto& r) { r.params_digest = uint256::ONE; }), "other parameters");
    refused(with([](auto& r) { r.payload.push_back(0); }), "malformed payload");
    refused(with([](auto& r) { r.payload.pop_back(); }), "truncated payload");
    refused(with([](auto& r) {
                const GroupLayout at{LayoutOf(r.payload)};
                BOOST_REQUIRE_GE(int{r.payload.at(at.pops)}, 2);
                const size_t first{at.pops + 1 + 48};
                std::swap_ranges(r.payload.begin() + first, r.payload.begin() + first + 96,
                                 r.payload.begin() + first + 96 + 48);
            }),
            "proofs of possession swapped");
    refused(with([](auto& r) { AddEscape(r.payload); }), "wrong state");
    ConfigureH1(m_R_h, TEST_ESCAPE_ROUNDS - 1); // the round-12 block is still C's escape
    refused(*good, "escape rounds changed");
    ConfigureH1(m_R_h);

    m_check_derived_state = false;
    InvalidateFrom(record.height);
    {
        ASSERT_DEBUG_LOG(strprintf("Persisted finality group at height %d not used: the block is not a modern "
                                   "block of the active chain", record.height));
        Restart([&] { WriteRecord(*good); });
    }
    BOOST_CHECK(GroupRebuilds() > 0);
    BOOST_CHECK(GroupAsRebuilt());
    auto payload{WITH_LOCK(cs_main, return LiveFinality().Encode(m_node.chainman->GetConsensus()))};
    BOOST_REQUIRE(payload);
    AddEscape(*payload);
    const auto wrong{with([&](auto& r) {
        r.block_hash = Tip()->GetBlockHash();
        r.height = Tip()->nHeight;
        r.payload = *payload;
    })};
    Restart([&] { WriteRecord(wrong); });
    BOOST_CHECK_EQUAL(GroupRebuilds(), 0U);
    BOOST_CHECK(!GroupAsRebuilt());
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return LiveFinality().Current().producers->escapes), 2U);
}

// (B0b-7) A record never raises the finality pin, which no restart lowers. A
// record whose finalized checkpoint is a later block of the chain, in the
// epoch of its height and certified there, passes the record's own checks;
// installed without the check against a rebuild (as outside regtest), it
// would pin that block for good. It is passed over, as is a record with a
// checkpoint when the pin file is gone, and the tracker rebuilds to the true
// state and pins the true checkpoint. A record at the pin is installed.
BOOST_FIXTURE_TEST_CASE(b0b_finality_group_record_never_raises_the_pin, GroupPersistFixture)
{
    PrepareRestarted();
    m_check_derived_state = false;
    std::optional<std::vector<unsigned char>> good;
    Restart([&] { good = ReadRecord(); });
    BOOST_REQUIRE(good);
    BOOST_REQUIRE_EQUAL(GroupRebuilds(), 0U);
    const auto pin{[&] { return WITH_LOCK(cs_main, return m_node.chainman->m_blockman.FinalityAnchor()); }};
    const auto anchor{pin()};
    BOOST_REQUIRE(anchor);
    BOOST_REQUIRE(WITH_LOCK(cs_main, return LiveFinality().PinnedCheckpoint()) == anchor);

    // The state's checkpoint moved to the block below the record's.
    node::DerivedStateRecord record{DecodeRecord(*good)};
    const int later{record.height - 1};
    BOOST_REQUIRE_GT(later, anchor->first);
    uint64_t epoch{0};
    uint256 hash;
    {
        LOCK(cs_main);
        const FinalityTracker::State& state{LiveFinality().Current()};
        BOOST_REQUIRE_GE(later, state.epoch_starts.back());
        epoch = state.epoch;
        hash = m_node.chainman->ActiveChain()[later]->GetBlockHash();
    }
    const GroupLayout at{LayoutOf(record.payload)};
    const StateLayout fields{StateFields(at.state, record.payload.at(at.state + 1 + 8), /*prior=*/false)};
    BOOST_REQUIRE_EQUAL(GetLE<int32_t>(record.payload, fields.finalized), anchor->first);
    PutLE<int32_t>(record.payload, fields.finalized, later);
    std::copy(hash.begin(), hash.end(), record.payload.begin() + fields.finalized + 4);
    PutLE<uint64_t>(record.payload, fields.finalized + 4 + 32, epoch);
    PutLE<int32_t>(record.payload, fields.finalized + 4 + 32 + 8, later);
    {
        LOCK(cs_main);
        const auto forged{FinalityTracker::Decode(record.payload, *m_node.chainman->ActiveChain()[record.height],
                                                  m_node.chainman->GetConsensus())};
        BOOST_REQUIRE(forged);
        BOOST_REQUIRE(forged->PinnedCheckpoint() == std::make_pair(later, hash));
    }
    BOOST_CHECK(RestartLogging(strprintf("Persisted finality group at height %d not used: its finalized checkpoint "
                                         "at height %d is above the finality pin (height %d)",
                                         record.height, later, anchor->first),
                               [&] { WriteRecord(node::EncodeDerivedStateRecord(record, BUILD)); }));
    BOOST_CHECK(pin() == anchor);
    BOOST_CHECK(GroupRebuilds() > 0);
    BOOST_CHECK(GroupAsRebuilt());

    // No pin file: the record is passed over, and the rebuild pins again.
    const fs::path pin_path{m_node.chainman->m_blockman.FinalityPinPath()};
    BOOST_CHECK(RestartLogging(strprintf("Persisted finality group at height %d not used: its finalized checkpoint "
                                         "at height %d is above the finality pin (none)",
                                         record.height, anchor->first),
                               [&] {
                                   WriteRecord(*good);
                                   BOOST_REQUIRE(fs::remove(pin_path));
                               }));
    BOOST_CHECK(GroupRebuilds() > 0);
    BOOST_CHECK(GroupAsRebuilt());
    BOOST_CHECK(pin() == anchor);
    Restart([&] { WriteRecord(*good); });
    BOOST_CHECK_EQUAL(GroupRebuilds(), 0U);
    BOOST_CHECK(GroupAsRebuilt());
    BOOST_CHECK(pin() == anchor);
}

// (B0b-8) A record's epoch starts are the chain's: the state's at least E
// apart (a rotation comes no sooner), and each prior state's exactly those at
// or below its block's parent. A state that rotated one block early, and a
// prior state just after a rotation replaced by the one before it, pass the
// record's other checks and are refused for this.
BOOST_FIXTURE_TEST_CASE(b0b_finality_group_epoch_starts_follow_the_chain, FinalityGroupFixture)
{
    constexpr int HORIZON{10};
    BuildGroupChain(HORIZON);
    const int M{m_M};
    const int E{SCALED_E};
    LOCK(cs_main);
    Chainstate& cs{m_node.chainman->ActiveChainstate()};
    const Consensus::Params& params{m_node.chainman->GetConsensus()};
    const CBlockIndex& tip{*cs.m_chain.Tip()};
    const FinalityTracker::State& state{LiveFinality().Current()};
    BOOST_REQUIRE(state.epoch_starts == (std::vector<int>{M, M + E}));
    BOOST_REQUIRE(state.finalized && state.finalized->height < M + E - 1);
    {
        // Rotated at M + E - 1. Every prior state in the history is of a
        // block above M + E, so only the spacing tells.
        BOOST_REQUIRE_GE(tip.nHeight - HORIZON, M + E);
        const auto bytes{LiveFinality().Encode(params)};
        BOOST_REQUIRE(bytes && FinalityTracker::Decode(*bytes, tip, params));
        const GroupLayout at{LayoutOf(*bytes)};
        const StateLayout fields{StateFields(at.state, state.epoch_starts.size(), /*prior=*/false)};
        BOOST_REQUIRE_EQUAL(GetLE<int32_t>(*bytes, fields.epoch_starts + 4), M + E);
        std::vector<unsigned char> early{*bytes};
        PutLE<int32_t>(early, fields.epoch_starts + 4, M + E - 1);
        BOOST_CHECK(!FinalityTracker::Decode(early, tip, params));
    }
    {
        // At M + E + 5 the history holds the rotation: the prior state of
        // block M + E + 1, as of the rotation block, replaced by the prior
        // state of block M + E, as of the block before.
        const CBlockIndex& after{*cs.m_chain[M + E + 5]};
        FinalityTracker at_after;
        BOOST_REQUIRE(at_after.Sync(cs.m_chain, cs.m_blockman, params, after));
        const auto bytes{at_after.Encode(params)};
        BOOST_REQUIRE(bytes && FinalityTracker::Decode(*bytes, after, params));
        const auto group_record{[&](const int height) {
            const uint256 hash{cs.m_chain[height]->GetBlockHash()};
            const auto found{Occurrences(*bytes, std::span<const unsigned char>{hash.begin(), hash.end()})};
            BOOST_REQUIRE_EQUAL(found.size(), 2U); // the stake registry's undo record, then the group's
            return found[1];
        }};
        const size_t before_rotation{group_record(M + E)};
        const size_t rotation{group_record(M + E + 1)};
        const size_t next{group_record(M + E + 2)};
        std::vector<unsigned char> stale{bytes->begin(), bytes->begin() + rotation + 32};
        stale.insert(stale.end(), bytes->begin() + before_rotation + 32, bytes->begin() + rotation);
        stale.insert(stale.end(), bytes->begin() + next, bytes->end());
        BOOST_CHECK(!FinalityTracker::Decode(stale, after, params));
    }
}

// (B0b-9) Decode holds the epoch starts it reads to the chain's before a
// prior state copies a prefix of them. Two records name 4M starts, and each
// of their HORIZON prior states claims all of them: one's starts end at M
// (none from M), the other's run far above its block (a prior state has two
// at most). Both are refused before the HORIZON copies of 16 MB each are
// made (the R1 evidence measures this case's peak memory).
BOOST_FIXTURE_TEST_CASE(b0b_finality_group_decode_bounds_the_epoch_starts, FinalityGroupFixture)
{
    constexpr int HORIZON{40};
    BuildGroupChain(HORIZON);
    const int M{m_M};
    const int E{SCALED_E};
    LOCK(cs_main);
    const Consensus::Params& params{m_node.chainman->GetConsensus()};
    const CBlockIndex& tip{*m_node.chainman->ActiveChain().Tip()};
    const auto bytes{LiveFinality().Encode(params)};
    BOOST_REQUIRE(bytes);
    const GroupLayout at{LayoutOf(*bytes)};
    const size_t count_at{at.state + 1 + 8};
    const StateLayout fields{StateFields(at.state, bytes->at(count_at), /*prior=*/false)};
    constexpr int64_t STARTS{4'000'000};
    // The state with `first`, first + E, ... as its starts, and HORIZON prior
    // states (no set, checkpoint or producer schedule) that claim them all.
    const auto crafted{[&](const int64_t first) {
        std::vector<unsigned char> out{bytes->begin(), bytes->begin() + count_at};
        VectorWriter w{out, out.size()};
        WriteCompactSize(w, STARTS);
        for (int64_t i{0}; i < STARTS; ++i) w << static_cast<int32_t>(first + i * E);
        out.insert(out.end(), bytes->begin() + fields.sets, bytes->begin() + fields.escapes + 8);
        VectorWriter priors{out, out.size()};
        WriteCompactSize(priors, HORIZON);
        for (int i{0}; i < HORIZON; ++i) {
            priors << uint256{} << uint8_t{0} << uint64_t{0};
            WriteCompactSize(priors, STARTS);
            priors << uint8_t{0} << uint8_t{0} << uint8_t{0} << uint8_t{0} << uint8_t{0} << uint8_t{0} << uint8_t{0}
                   << uint8_t{0};
        }
        return out;
    }};
    BOOST_CHECK(!FinalityTracker::Decode(crafted(M - (STARTS - 1) * E), tip, params));
    BOOST_CHECK(!FinalityTracker::Decode(crafted(M), tip, params));
}

BOOST_AUTO_TEST_SUITE_END()
