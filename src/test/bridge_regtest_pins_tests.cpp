// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bridge/proof.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <common/args.h>
#include <consensus/bridge_params.h>
#include <crypto/sha256.h>
#include <consensus/amount.h>
#include <consensus/params.h>
#include <modern/bridge_asset.h>
#include <modern/bridge_v2.h>
#include <modern/chain_domain.h>
#include <span.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/chaintype.h>
#include <util/fs.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(bridge_regtest_pins_tests, BasicTestingSetup)

namespace {

// Anvil's deterministic first three deployment addresses (deployer nonce 0,
// 1, 2): the mock token, the verifier and the vault.
const std::string TOKEN{"0x5fbdb2315678afecb367f032d93f642f64180aa3"};
const std::string VERIFIER{"0xe7f1725e7734ce288f8367e1bb143e90bb3f0512"};
const std::string VAULT{"0x9fe46736679d2d9a65f0992f2272de9f3c7fa6e0"};
constexpr uint64_t DEPLOYMENT_BLOCK{3};

std::string Repeat(const std::string& byte_hex, size_t count)
{
    std::string out{"0x"};
    for (size_t i{0}; i < count; ++i) out += byte_hex;
    return out;
}

UniValue Fork(uint64_t activation_epoch, const std::string& version)
{
    UniValue fork{UniValue::VOBJ};
    fork.pushKV("activation_epoch", activation_epoch);
    fork.pushKV("fork_version", version);
    return fork;
}

UniValue ValidLightClient()
{
    UniValue light{UniValue::VOBJ};
    light.pushKV("trusted_checkpoint_root", Repeat("c1", 32));
    light.pushKV("trusted_checkpoint_slot", uint64_t{8'192 * 2 + 4'096});
    light.pushKV("checkpoint_execution_block", DEPLOYMENT_BLOCK);
    light.pushKV("genesis_validators_root", Repeat("d1", 32));
    UniValue forks{UniValue::VARR};
    forks.push_back(Fork(0, "0xb3000001"));
    light.pushKV("fork_schedule", forks);
    light.pushKV("fork_schedule_valid_through_epoch", uint64_t{(8'192 * 2 + 4'096) / 32 + 1'024});
    light.pushKV("electra_epoch", uint64_t{0});
    return light;
}

UniValue ValidV1()
{
    UniValue v1{UniValue::VOBJ};
    v1.pushKV("origin_chain_id", uint64_t{31'337});
    v1.pushKV("vault", VAULT);
    v1.pushKV("token", TOKEN);
    v1.pushKV("origin_deployment_block", DEPLOYMENT_BLOCK);
    v1.pushKV("vault_runtime_code_hash", Repeat("a1", 32));
    v1.pushKV("token_runtime_code_hash", Repeat("b1", 32));
    v1.pushKV("max_deposit_raw", uint64_t{1'000'000'000});
    v1.pushKV("light_client", ValidLightClient());
    v1.pushKV("verifier", VERIFIER);
    v1.pushKV("verifier_runtime_code_hash", Repeat("e1", 32));
    v1.pushKV("bootstrap_validator_set_hash", Repeat("f1", 32));
    v1.pushKV("min_bridge_validators", uint64_t{4});
    v1.pushKV("max_bridge_validators", uint64_t{64});
    v1.pushKV("min_bridge_total_weight", uint64_t{4});
    v1.pushKV("max_epoch_lag", uint64_t{86'400});
    return v1;
}

UniValue ValidRoot(const UniValue& v1 = ValidV1())
{
    UniValue root{UniValue::VOBJ};
    root.pushKV("format", "b3-regtest-bridge-pins/1");
    root.pushKV("v1_decentralized", v1);
    return root;
}

std::string ValidPins() { return ValidRoot().write(); }

//! The default file with v1_decentralized.<key> replaced or added.
std::string WithV1(const std::string& key, const UniValue& value)
{
    UniValue v1{ValidV1()};
    v1.pushKV(key, value);
    return ValidRoot(v1).write();
}

//! The default file with v1_decentralized.light_client.<key> replaced or added.
std::string WithLight(const std::string& key, const UniValue& value)
{
    UniValue light{ValidLightClient()};
    light.pushKV(key, value);
    return WithV1("light_client", light);
}

UniValue Without(const UniValue& object, std::string_view key)
{
    UniValue out{UniValue::VOBJ};
    for (size_t i{0}; i < object.size(); ++i) {
        if (object.getKeys()[i] != key) out.pushKV(object.getKeys()[i], object.getValues()[i]);
    }
    return out;
}

uint256 Sha256(std::string_view bytes)
{
    uint256 out;
    CSHA256().Write(UCharCast(bytes.data()), bytes.size()).Finalize(out.begin());
    return out;
}

void CheckRefused(const std::string& json, const std::string& reason)
{
    BOOST_CHECK_EXCEPTION(ParseB3RegTestBridgePins(json), std::runtime_error,
                          HasReason("Invalid -b3bridgepins file: "));
    BOOST_CHECK_EXCEPTION(ParseB3RegTestBridgePins(json), std::runtime_error,
                          HasReason(reason));
}

// Bridge V2 stand-ins: anvil's default deployer at nonces 3, 4 and 5. The
// v2 section may reuse the V1 mock token (one USDT on the local chain).
const std::string VAULT_V2{"0xcf7ed3acca5a467e9e704c703e8d87f634fb0fc9"};
const std::string VERIFIER_V2{"0xdc64a140aa3e981100a9beca4e685f962f0cf6c9"};
const std::string TOKEN_V2{"0x5fc8d32690cc91d4c39d9d3abcbd16989f875707"};
// The default -b3modernregtest -b3flowmeshtest schedule: M = 161 on the
// 5-block grid and A3 = 193. R_f = 172 puts R_f - 1 = 171 on the grid.
constexpr int DEFAULT_A3{193};
constexpr int FRESH_HEIGHT{172};
constexpr int V2_HEIGHT{200};

UniValue ValidV2(int activation_height = V2_HEIGHT)
{
    UniValue v2{UniValue::VOBJ};
    v2.pushKV("origin_chain_id", uint64_t{31'337});
    v2.pushKV("vault", VAULT_V2);
    v2.pushKV("token", TOKEN);
    v2.pushKV("token_runtime_code_hash", Repeat("b2", 32));
    v2.pushKV("origin_deployment_block", DEPLOYMENT_BLOCK);
    v2.pushKV("max_deposit_raw", uint64_t{10'000'000'000});
    v2.pushKV("light_client", ValidLightClient());
    v2.pushKV("verifier", VERIFIER_V2);
    v2.pushKV("min_bridge_validators", uint64_t{4});
    v2.pushKV("max_bridge_validators", uint64_t{64});
    v2.pushKV("min_bridge_total_weight", uint64_t{4});
    v2.pushKV("activation_height", static_cast<uint64_t>(activation_height));
    return v2;
}

//! A file with only a v2 section.
UniValue V2Root(const UniValue& v2 = ValidV2())
{
    UniValue root{UniValue::VOBJ};
    root.pushKV("format", "b3-regtest-bridge-pins/1");
    root.pushKV("v2", v2);
    return root;
}

std::string ValidV2Pins() { return V2Root().write(); }

//! The v2-only file with v2.<key> replaced or added.
std::string WithV2(const std::string& key, const UniValue& value)
{
    UniValue v2{ValidV2()};
    v2.pushKV(key, value);
    return V2Root(v2).write();
}

//! The v2-only file with v2.light_client.<key> replaced or added.
std::string WithV2Light(const std::string& key, const UniValue& value)
{
    UniValue light{ValidLightClient()};
    light.pushKV(key, value);
    return WithV2("light_client", light);
}

//! A file with the default V1 section and the given v2 section.
std::string BothPins(const UniValue& v2 = ValidV2())
{
    UniValue root{ValidRoot()};
    root.pushKV("v2", v2);
    return root.write();
}

//! FlowMesh regtest options with the given pins and fresh lineage height.
CChainParams::RegTestOptions FlowMeshOptions(std::optional<CChainParams::B3RegTestBridgePins> pins,
                                             std::optional<int> fresh_height = FRESH_HEIGHT)
{
    CChainParams::B3ModernRegTestOptions b3;
    b3.flowmesh_test = true;
    b3.bridge_pins = std::move(pins);
    b3.fresh_lineage_height = fresh_height;
    CChainParams::RegTestOptions options;
    options.b3_modern = b3;
    return options;
}

} // namespace

struct BridgePinsSetup : public BasicTestingSetup {
    std::string WriteFile(const std::string& name, std::string_view contents)
    {
        const fs::path path{m_path_root / fs::PathFromString(name)};
        std::ofstream file{path.std_path(), std::ios::binary | std::ios::trunc};
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        file.close();
        BOOST_REQUIRE(path.is_absolute());
        return fs::PathToString(path);
    }

    static void SetArgs(ArgsManager& args, const std::string& pins_path,
                        bool modern = true, bool flowmesh = true)
    {
        if (modern) args.ForceSetArg("-b3modernregtest", "1");
        if (flowmesh) args.ForceSetArg("-b3flowmeshtest", "1");
        args.ForceSetArg("-b3bridgepins", pins_path);
    }
};

BOOST_FIXTURE_TEST_CASE(knob_is_refused_outside_flowmesh_regtest, BridgePinsSetup)
{
    const std::string path{WriteFile("pins.json", ValidPins())};
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET,
                                  ChainType::TESTNET4, ChainType::SIGNET}) {
        ArgsManager args;
        SetArgs(args, path);
        BOOST_CHECK_EXCEPTION(CreateChainParams(args, chain), std::runtime_error,
                              HasReason("-b3bridgepins is regtest-only"));
        ArgsManager bare;
        bare.ForceSetArg("-b3bridgepins", path);
        BOOST_CHECK_EXCEPTION(CreateChainParams(bare, chain), std::runtime_error,
                              HasReason("-b3bridgepins is regtest-only"));
        // Without the flag nothing changes and no pin hash is reported.
        BOOST_CHECK(!CreateChainParams(ArgsManager{}, chain)->B3BridgePinsHash());
    }

    for (const auto& [modern, flowmesh] : {std::pair{false, false}, std::pair{true, false},
                                           std::pair{false, true}}) {
        ArgsManager args;
        SetArgs(args, path, modern, flowmesh);
        BOOST_CHECK_EXCEPTION(CreateChainParams(args, ChainType::REGTEST), std::runtime_error,
                              HasReason("-b3bridgepins requires -b3modernregtest and -b3flowmeshtest (regtest-only)"));
    }

    // A negated knob is rejected by the argument parser itself.
    ArgsManager parsed;
    SetupChainParamsBaseOptions(parsed);
    const char* argv[]{"b3coind", "-regtest", "-nob3bridgepins"};
    std::string error;
    BOOST_CHECK(!parsed.ParseParameters(3, argv, error));
    BOOST_CHECK(HasReason("b3bridgepins")(error));
}

BOOST_FIXTURE_TEST_CASE(pin_file_points_flowmesh_regtest_at_anvil, BridgePinsSetup)
{
    // Default flowmesh regtest: MANAGED_V1 synthetic bridge, no pin hash.
    ArgsManager default_args;
    default_args.ForceSetArg("-b3modernregtest", "1");
    default_args.ForceSetArg("-b3flowmeshtest", "1");
    const auto default_chain{CreateChainParams(default_args, ChainType::REGTEST)};
    const Consensus::Params& defaults{default_chain->GetConsensus()};
    BOOST_CHECK(!default_chain->B3BridgePinsHash());
    BOOST_REQUIRE(defaults.busd_bridge);
    BOOST_CHECK(*defaults.busd_bridge->withdrawal_mode ==
                Consensus::BridgeWithdrawalMode::MANAGED_V1);
    const auto default_registry{modern::ConfiguredBridgeRegistryId(defaults)};
    const auto default_asset{modern::ConfiguredBridgeAssetId(defaults)};
    BOOST_REQUIRE(default_registry);
    BOOST_REQUIRE(default_asset);

    const std::string contents{ValidPins()};
    const std::string path{WriteFile("pins.json", contents)};
    ArgsManager args;
    SetArgs(args, path);
    const auto chain{CreateChainParams(args, ChainType::REGTEST)};
    const Consensus::Params& params{chain->GetConsensus()};

    BOOST_REQUIRE(chain->B3BridgePinsHash());
    BOOST_CHECK(*chain->B3BridgePinsHash() == Sha256(contents));

    BOOST_REQUIRE(params.busd_bridge);
    const Consensus::BridgeAssetParams& busd{*params.busd_bridge};
    BOOST_CHECK(Consensus::BridgeMintParamsReady(busd));
    BOOST_CHECK_EQUAL(busd.asset.origin_chain_id, 31'337U);
    // Raw Ethereum byte order, exactly as getbridgeinfo prints with HexStr.
    BOOST_CHECK_EQUAL("0x" + HexStr(busd.asset.vault_address), VAULT);
    BOOST_CHECK_EQUAL("0x" + HexStr(busd.asset.token_address), TOKEN);
    BOOST_CHECK_EQUAL(*busd.origin_deployment_block, DEPLOYMENT_BLOCK);
    BOOST_CHECK_EQUAL("0x" + HexStr(*busd.vault_runtime_code_hash), Repeat("a1", 32));
    BOOST_CHECK_EQUAL("0x" + HexStr(*busd.implementation_or_adapter), Repeat("b1", 32));
    BOOST_CHECK_EQUAL(*busd.adapter_version, Consensus::BRIDGE_ADAPTER_VERSION_DIRECT_TOKEN_V1);
    BOOST_REQUIRE(busd.mint_caps);
    BOOST_CHECK_EQUAL(busd.mint_caps->max_per_block, CChainParams::B3RegTestBridgePins::DEFAULT_MAX_PER_BLOCK);
    BOOST_CHECK_EQUAL(busd.mint_caps->max_per_epoch, CChainParams::B3RegTestBridgePins::DEFAULT_MAX_PER_EPOCH);
    BOOST_CHECK_EQUAL(busd.mint_caps->epoch_length_blocks,
                      defaults.busd_bridge->mint_caps->epoch_length_blocks);

    const Consensus::EthereumLightClientPins& light{*busd.light_client};
    BOOST_CHECK_EQUAL("0x" + HexStr(light.trusted_checkpoint_root), Repeat("c1", 32));
    BOOST_CHECK_EQUAL(light.trusted_checkpoint_slot, 8'192U * 2 + 4'096);
    BOOST_CHECK_EQUAL("0x" + HexStr(light.genesis_validators_root), Repeat("d1", 32));
    BOOST_REQUIRE_EQUAL(light.fork_schedule.size(), 1U);
    BOOST_CHECK_EQUAL(light.fork_schedule[0].activation_epoch, 0U);
    BOOST_CHECK_EQUAL(HexStr(light.fork_schedule[0].fork_version), "b3000001");
    BOOST_CHECK_EQUAL(light.fork_schedule_valid_through_epoch, (8'192U * 2 + 4'096) / 32 + 1'024);
    BOOST_CHECK_EQUAL(light.electra_epoch, 0U);
    BOOST_CHECK_EQUAL(light.min_sync_committee_participants,
                      Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY);
    BOOST_CHECK_EQUAL(light.max_sync_lag_slots, 8'192U);

    BOOST_REQUIRE(busd.withdrawal_mode);
    BOOST_CHECK(*busd.withdrawal_mode ==
                Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1);
    BOOST_CHECK(!busd.managed_withdrawal);
    BOOST_REQUIRE(busd.decentralized_withdrawal);
    const Consensus::BridgeDecentralizedWithdrawalPins& withdrawal{*busd.decentralized_withdrawal};
    BOOST_CHECK_EQUAL("0x" + HexStr(withdrawal.ethereum_verifier_address), VERIFIER);
    BOOST_CHECK_EQUAL("0x" + HexStr(withdrawal.ethereum_verifier_code_hash), Repeat("e1", 32));
    BOOST_CHECK_EQUAL("0x" + HexStr(withdrawal.bootstrap_validator_set_hash), Repeat("f1", 32));
    BOOST_CHECK_EQUAL(withdrawal.withdrawal_rules_version,
                      Consensus::DECENTRALIZED_WITHDRAWAL_RULES_VERSION_V1);
    BOOST_CHECK(withdrawal.withdrawal_rules_commitment ==
                Consensus::DECENTRALIZED_WITHDRAWAL_RULES_COMMITMENT_V1);
    BOOST_CHECK_EQUAL(withdrawal.min_bridge_validators, 4U);
    BOOST_CHECK_EQUAL(withdrawal.max_bridge_validators, 64U);
    BOOST_CHECK_EQUAL(withdrawal.min_bridge_total_weight, 4U);
    BOOST_CHECK_EQUAL(withdrawal.max_epoch_lag, 86'400U);

    // Activation and withdrawal heights stay at A3, exactly as without pins.
    BOOST_CHECK(busd.activation_height == defaults.busd_bridge->activation_height);
    BOOST_CHECK(busd.activation_height == params.flowmesh_activation_height);
    BOOST_CHECK(params.bridge_withdrawal_activation_height ==
                defaults.bridge_withdrawal_activation_height);
    BOOST_CHECK(params.hashGenesisBlock == defaults.hashGenesisBlock);
    // A V1-only file configures no bridge V2.
    BOOST_CHECK(!defaults.bridge_v2);
    BOOST_CHECK(!params.bridge_v2);

    const auto registry{modern::ConfiguredBridgeRegistryId(params)};
    BOOST_REQUIRE(registry);
    BOOST_CHECK(*registry != *default_registry);
    const auto asset{modern::ConfiguredDecentralizedBridgeAssetId(params)};
    BOOST_REQUIRE(asset);
    BOOST_CHECK(*asset != *default_asset);
    BOOST_CHECK(!modern::ConfiguredDecentralizedBridgeAssetId(defaults));

    // Optional cap overrides; the mint epoch length stays b3.epoch_length.
    UniValue v1{ValidV1()};
    v1.pushKV("max_per_block", uint64_t{2'000'000'000});
    v1.pushKV("max_per_epoch", uint64_t{4'000'000'000});
    const std::string capped{ValidRoot(v1).write()};
    ArgsManager capped_args;
    SetArgs(capped_args, WriteFile("capped.json", capped));
    const auto capped_chain{CreateChainParams(capped_args, ChainType::REGTEST)};
    const Consensus::Params& capped_params{capped_chain->GetConsensus()};
    BOOST_CHECK_EQUAL(capped_params.busd_bridge->mint_caps->max_per_block, 2'000'000'000);
    BOOST_CHECK_EQUAL(capped_params.busd_bridge->mint_caps->max_per_epoch, 4'000'000'000);
    BOOST_CHECK_EQUAL(capped_params.busd_bridge->mint_caps->epoch_length_blocks,
                      busd.mint_caps->epoch_length_blocks);
    BOOST_CHECK(*capped_chain->B3BridgePinsHash() == Sha256(capped));
    BOOST_CHECK(*modern::ConfiguredBridgeRegistryId(capped_params) != *registry);

    // Mixed-case (EIP-55 style) hex parses to the same raw bytes.
    const auto upper{ParseB3RegTestBridgePins(WithV1("vault", "0x9fE46736679d2D9a65F0992F2272dE9f3c7fa6e0"))};
    BOOST_CHECK(upper.vault == busd.asset.vault_address);
}

BOOST_AUTO_TEST_CASE(kernel_refuses_pins_without_complete_flowmesh_bridge)
{
    CChainParams::RegTestOptions options;
    CChainParams::B3ModernRegTestOptions b3;
    b3.bridge_pins = ParseB3RegTestBridgePins(ValidPins());
    options.b3_modern = b3;
    BOOST_CHECK_EXCEPTION(CChainParams::RegTest(options), std::runtime_error,
                          HasReason("-b3bridgepins requires -b3flowmeshtest"));

    b3.flowmesh_test = true;
    b3.bridge_pins = CChainParams::B3RegTestBridgePins{};
    options.b3_modern = b3;
    BOOST_CHECK_EXCEPTION(CChainParams::RegTest(options), std::runtime_error,
                          HasReason("-b3bridgepins does not form a complete bridge configuration"));

    b3.bridge_pins = ParseB3RegTestBridgePins(ValidPins());
    options.b3_modern = b3;
    const auto chain{CChainParams::RegTest(options)};
    BOOST_REQUIRE(chain->B3BridgePinsHash());
    BOOST_CHECK(*chain->B3BridgePinsHash() == Sha256(ValidPins()));
}

BOOST_AUTO_TEST_CASE(checkpoint_window_matches_consensus_backfill_bound)
{
    const auto at{[](uint64_t offset) {
        return WithLight("checkpoint_execution_block", DEPLOYMENT_BLOCK + offset);
    }};
    BOOST_CHECK_NO_THROW(ParseB3RegTestBridgePins(at(0)));
    BOOST_CHECK_NO_THROW(ParseB3RegTestBridgePins(at(bridge::MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS)));
    BOOST_CHECK_EQUAL(bridge::MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS, 20'000U);
    CheckRefused(at(20'001), "within 20000 blocks");
    CheckRefused(WithLight("checkpoint_execution_block", DEPLOYMENT_BLOCK - 1), "within 20000 blocks");
}

BOOST_AUTO_TEST_CASE(real_network_domains_are_refused)
{
    for (const uint64_t chain_id : {uint64_t{1}, uint64_t{11'155'111}, uint64_t{17'000}}) {
        CheckRefused(WithV1("origin_chain_id", chain_id), "origin_chain_id must be 31337");
    }
    CheckRefused(WithLight("genesis_validators_root",
                           "0x" + HexStr(Consensus::ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT_BYTES)),
                 "must not be the Ethereum mainnet root");
    for (const std::string version : {"0x00000000", "0x05000000", "0x90000074"}) {
        UniValue forks{UniValue::VARR};
        forks.push_back(Fork(0, version));
        CheckRefused(WithLight("fork_schedule", forks), "synthetic 0xb3 namespace");
        // Also refused when it is only a later fork.
        UniValue later{UniValue::VARR};
        later.push_back(Fork(0, "0xb3000001"));
        later.push_back(Fork(100, version));
        CheckRefused(WithLight("fork_schedule", later), "synthetic 0xb3 namespace");
    }
    UniValue two{UniValue::VARR};
    two.push_back(Fork(0, "0xb3000001"));
    two.push_back(Fork(100, "0xb3000002"));
    BOOST_CHECK_NO_THROW(ParseB3RegTestBridgePins(WithLight("fork_schedule", two)));
}

BOOST_AUTO_TEST_CASE(caps_participants_and_withdrawal_bounds_are_enforced)
{
    // Check 1: MAX_DEPOSIT_RAW must fit the per-block cap.
    BOOST_CHECK_NO_THROW(ParseB3RegTestBridgePins(
        WithV1("max_deposit_raw", uint64_t{CChainParams::B3RegTestBridgePins::DEFAULT_MAX_PER_BLOCK})));
    CheckRefused(WithV1("max_deposit_raw", uint64_t{CChainParams::B3RegTestBridgePins::DEFAULT_MAX_PER_BLOCK + 1}),
                 "caps must satisfy");
    CheckRefused(WithV1("max_deposit_raw", uint64_t{0}), "caps must satisfy");
    CheckRefused(WithV1("max_per_block", uint64_t{999'999'999}), "caps must satisfy");
    CheckRefused(WithV1("max_per_epoch", uint64_t{999'999'999}), "caps must satisfy");
    CheckRefused(WithV1("max_per_epoch", uint64_t{MAX_MONEY} + 1), "exceeds MAX_MONEY");

    CheckRefused(WithLight("min_sync_committee_participants", uint64_t{341}),
                 "min_sync_committee_participants must be between 342 and 512");
    CheckRefused(WithLight("min_sync_committee_participants", uint64_t{513}),
                 "min_sync_committee_participants must be between 342 and 512");
    BOOST_CHECK_EQUAL(ParseB3RegTestBridgePins(WithLight("min_sync_committee_participants", uint64_t{512}))
                          .light_client.min_sync_committee_participants,
                      512U);

    CheckRefused(WithV1("min_bridge_validators", uint64_t{3}), "withdrawal pins need");
    CheckRefused(WithV1("max_bridge_validators", uint64_t{65}), "withdrawal pins need");
    CheckRefused(WithV1("min_bridge_total_weight", uint64_t{0}), "withdrawal pins need");
    CheckRefused(WithV1("max_epoch_lag", uint64_t{0}), "withdrawal pins need");

    CheckRefused(WithV1("origin_deployment_block", uint64_t{0}), "origin_deployment_block must be at least 1");
    CheckRefused(WithLight("max_sync_lag_slots", uint64_t{0}), "pins are inconsistent");
    CheckRefused(WithLight("fork_schedule_valid_through_epoch", uint64_t{100}), "pins are inconsistent");
    // The synthetic beacon serves Electra from genesis: electra_epoch is
    // required and must be 0.
    for (const uint64_t electra : {uint64_t{1}, uint64_t{1'000'000}, std::numeric_limits<uint64_t>::max()}) {
        CheckRefused(WithLight("electra_epoch", electra), "light_client.electra_epoch must be 0");
    }
    CheckRefused(WithV1("light_client", Without(ValidLightClient(), "electra_epoch")),
                 "missing key v1_decentralized.light_client.electra_epoch");
    UniValue late_start{UniValue::VARR};
    late_start.push_back(Fork(1, "0xb3000001"));
    CheckRefused(WithLight("fork_schedule", late_start), "pins are inconsistent");
}

BOOST_AUTO_TEST_CASE(addresses_and_hashes_are_checked)
{
    CheckRefused(WithV1("token", VAULT), "pairwise distinct");
    CheckRefused(WithV1("verifier", VAULT), "pairwise distinct");
    CheckRefused(WithV1("verifier", TOKEN), "pairwise distinct");
    CheckRefused(WithV1("vault", Repeat("00", 20)), "must be non-zero");
    CheckRefused(WithV1("vault_runtime_code_hash", Repeat("00", 32)), "vault_runtime_code_hash must be non-zero");
    CheckRefused(WithV1("token_runtime_code_hash", Repeat("00", 32)), "token_runtime_code_hash must be non-zero");
    CheckRefused(WithV1("verifier_runtime_code_hash", Repeat("00", 32)), "verifier_runtime_code_hash must be non-zero");
    CheckRefused(WithV1("bootstrap_validator_set_hash", Repeat("00", 32)), "must be non-zero");
    CheckRefused(WithLight("trusted_checkpoint_root", Repeat("00", 32)), "must be non-zero");

    // Hex must be 0x-prefixed, exact-length, raw byte order.
    CheckRefused(WithV1("vault", VAULT.substr(2)), "0x-prefixed 20-byte hex string");
    CheckRefused(WithV1("vault", VAULT + "00"), "0x-prefixed 20-byte hex string");
    CheckRefused(WithV1("vault", "0x" + std::string(40, 'g')), "0x-prefixed 20-byte hex string");
    CheckRefused(WithV1("vault", "0x9fe46736679d2d9a65f0992f2272de9f3c7fa6e "), "0x-prefixed 20-byte hex string");
    CheckRefused(WithV1("vault_runtime_code_hash", Repeat("a1", 31)), "0x-prefixed 32-byte hex string");
    UniValue forks{UniValue::VARR};
    forks.push_back(Fork(0, "0xb30000"));
    CheckRefused(WithLight("fork_schedule", forks), "0x-prefixed 4-byte hex string");
    CheckRefused(WithV1("vault", uint64_t{1}), "0x-prefixed 20-byte hex string");
}

BOOST_AUTO_TEST_CASE(schema_is_strict)
{
    BOOST_CHECK_NO_THROW(ParseB3RegTestBridgePins(ValidPins()));

    UniValue v3_root{ValidRoot()};
    v3_root.pushKV("v3", UniValue{UniValue::VOBJ});
    CheckRefused(v3_root.write(), "unknown key file.v3");
    UniValue extra_root{ValidRoot()};
    extra_root.pushKV("comment", "x");
    CheckRefused(extra_root.write(), "unknown key file.comment");
    CheckRefused(WithV1("withdrawal_rules_commitment", Repeat("11", 32)),
                 "unknown key v1_decentralized.withdrawal_rules_commitment");
    CheckRefused(WithV1("origin_chain", uint64_t{31'337}), "unknown key v1_decentralized.origin_chain");
    CheckRefused(WithLight("slot", uint64_t{1}), "unknown key v1_decentralized.light_client.slot");
    UniValue forks{UniValue::VARR};
    UniValue fork{Fork(0, "0xb3000001")};
    fork.pushKV("epoch", uint64_t{0});
    forks.push_back(fork);
    CheckRefused(WithLight("fork_schedule", forks), "unknown key v1_decentralized.light_client.fork_schedule[0].epoch");

    CheckRefused(ValidRoot(Without(ValidV1(), "max_epoch_lag")).write(),
                 "missing key v1_decentralized.max_epoch_lag");
    CheckRefused(WithV1("light_client", Without(ValidLightClient(), "checkpoint_execution_block")),
                 "missing key v1_decentralized.light_client.checkpoint_execution_block");
    CheckRefused(Without(ValidRoot(), "v1_decentralized").write(), "file needs v1_decentralized or v2");

    // Duplicate keys are refused even when the values agree.
    std::string duplicate{ValidPins()};
    duplicate.insert(duplicate.find("\"v1_decentralized\""), "\"format\":\"b3-regtest-bridge-pins/1\",");
    CheckRefused(duplicate, "duplicate key file.format");

    UniValue wrong_format{ValidRoot()};
    wrong_format.pushKV("format", "b3-regtest-bridge-pins/2");
    CheckRefused(wrong_format.write(), "format must be");
    CheckRefused("", "not a valid JSON document");
    CheckRefused("{\"format\":", "not a valid JSON document");
    CheckRefused(ValidPins() + "{}", "not a valid JSON document");
    CheckRefused("[]", "file must be a JSON object");
    CheckRefused(WithV1("light_client", UniValue{UniValue::VARR}), "light_client must be a JSON object");
    CheckRefused(WithLight("fork_schedule", UniValue{UniValue::VARR}), "fork_schedule must be a non-empty array");

    // Integers: unsigned, integral, in range.
    CheckRefused(WithV1("origin_deployment_block", int64_t{-1}), "must be an unsigned integer");
    CheckRefused(WithV1("origin_deployment_block", 3.5), "must be an unsigned integer");
    CheckRefused(WithV1("origin_deployment_block", "3"), "must be an unsigned integer");
    CheckRefused(WithV1("max_epoch_lag", uint64_t{1} << 32), "must be an unsigned integer of at most 32 bits");
}

BOOST_FIXTURE_TEST_CASE(pin_file_path_and_size_are_checked, BridgePinsSetup)
{
    {
        ArgsManager args;
        SetArgs(args, "pins.json");
        BOOST_CHECK_EXCEPTION(CreateChainParams(args, ChainType::REGTEST), std::runtime_error,
                              HasReason("-b3bridgepins requires an absolute path"));
    }
    {
        ArgsManager args;
        SetArgs(args, "./pins.json");
        BOOST_CHECK_EXCEPTION(CreateChainParams(args, ChainType::REGTEST), std::runtime_error,
                              HasReason("-b3bridgepins requires an absolute path"));
    }
    {
        ArgsManager args;
        SetArgs(args, "");
        BOOST_CHECK_EXCEPTION(CreateChainParams(args, ChainType::REGTEST), std::runtime_error,
                              HasReason("-b3bridgepins requires an absolute path"));
    }
    {
        ArgsManager args;
        SetArgs(args, fs::PathToString(m_path_root / "missing.json"));
        BOOST_CHECK_EXCEPTION(CreateChainParams(args, ChainType::REGTEST), std::runtime_error,
                              HasReason("Cannot open -b3bridgepins file"));
    }
    {
        // A refused file surfaces the parser's reason through startup.
        ArgsManager args;
        SetArgs(args, WriteFile("mainnet_chain.json", WithV1("origin_chain_id", uint64_t{1})));
        BOOST_CHECK_EXCEPTION(CreateChainParams(args, ChainType::REGTEST), std::runtime_error,
                              HasReason("Invalid -b3bridgepins file: v1_decentralized.origin_chain_id must be 31337"));
    }
    {
        // Exactly the size limit is read; one byte more is refused.
        std::string padded{ValidPins()};
        padded.resize(MAX_B3_BRIDGE_PINS_FILE_SIZE, ' ');
        ArgsManager args;
        SetArgs(args, WriteFile("max.json", padded));
        const auto chain{CreateChainParams(args, ChainType::REGTEST)};
        BOOST_CHECK(*chain->B3BridgePinsHash() == Sha256(padded));

        padded.push_back(' ');
        ArgsManager big_args;
        SetArgs(big_args, WriteFile("big.json", padded));
        BOOST_CHECK_EXCEPTION(CreateChainParams(big_args, ChainType::REGTEST), std::runtime_error,
                              HasReason("is larger than 65536 bytes"));
    }
}

BOOST_FIXTURE_TEST_CASE(v2_only_file_applies, BridgePinsSetup)
{
    // FlowMesh regtest with the fresh lineage and no pin file: no bridge V2.
    ArgsManager default_args;
    default_args.ForceSetArg("-b3modernregtest", "1");
    default_args.ForceSetArg("-b3flowmeshtest", "1");
    default_args.ForceSetArg("-b3freshlineageheight", std::to_string(FRESH_HEIGHT));
    const auto default_chain{CreateChainParams(default_args, ChainType::REGTEST)};
    const Consensus::Params& defaults{default_chain->GetConsensus()};
    BOOST_REQUIRE(defaults.busd_bridge);
    BOOST_REQUIRE_EQUAL(*defaults.busd_bridge->activation_height, DEFAULT_A3);
    BOOST_REQUIRE(defaults.fresh_lineage);
    BOOST_CHECK(!defaults.bridge_v2);
    BOOST_CHECK(!modern::ConfiguredBridgeV2AssetId(defaults));
    BOOST_CHECK(!default_chain->B3BridgePinsHash());

    const std::string contents{ValidV2Pins()};
    const auto parsed{ParseB3RegTestBridgePins(contents)};
    BOOST_CHECK(!parsed.has_v1);
    BOOST_REQUIRE(parsed.v2);
    BOOST_CHECK(parsed.file_sha256 == Sha256(contents));

    ArgsManager args;
    SetArgs(args, WriteFile("v2.json", contents));
    args.ForceSetArg("-b3freshlineageheight", std::to_string(FRESH_HEIGHT));
    const auto chain{CreateChainParams(args, ChainType::REGTEST)};
    const Consensus::Params& params{chain->GetConsensus()};

    // The record guard hashes the whole file, as for a V1 file.
    BOOST_REQUIRE(chain->B3BridgePinsHash());
    BOOST_CHECK(*chain->B3BridgePinsHash() == Sha256(contents));

    // The synthetic MANAGED_V1 bUSD stays exactly as without a file.
    BOOST_REQUIRE(params.busd_bridge);
    BOOST_CHECK(*params.busd_bridge->withdrawal_mode == Consensus::BridgeWithdrawalMode::MANAGED_V1);
    BOOST_CHECK(*modern::ConfiguredBridgeRegistryId(params) == *modern::ConfiguredBridgeRegistryId(defaults));
    BOOST_CHECK(*modern::ConfiguredBridgeAssetId(params) == *modern::ConfiguredBridgeAssetId(defaults));
    BOOST_CHECK(params.busd_bridge->activation_height == defaults.busd_bridge->activation_height);
    BOOST_CHECK(params.bridge_withdrawal_activation_height == defaults.bridge_withdrawal_activation_height);
    BOOST_CHECK(!modern::ConfiguredDecentralizedBridgeAssetId(params));
    BOOST_CHECK(params.hashGenesisBlock == defaults.hashGenesisBlock);

    BOOST_REQUIRE(params.bridge_v2);
    const Consensus::BridgeV2Params& v2{*params.bridge_v2};
    BOOST_CHECK(v2.Valid());
    BOOST_CHECK_EQUAL(v2.activation_height, V2_HEIGHT);
    BOOST_CHECK_EQUAL(v2.burn_height, V2_HEIGHT);

    const Consensus::BridgeV2VaultEntry& entry{v2.vault};
    BOOST_CHECK_EQUAL(entry.origin_chain_id, 31'337U);
    BOOST_CHECK_EQUAL("0x" + HexStr(entry.vault_address), VAULT_V2);
    BOOST_CHECK_EQUAL("0x" + HexStr(entry.token_address), TOKEN);
    BOOST_CHECK_EQUAL(entry.origin_decimals, 6);
    BOOST_CHECK_EQUAL(entry.asset_decimals, 6);
    BOOST_CHECK_EQUAL("0x" + HexStr(entry.token_runtime_code_hash), Repeat("b2", 32));
    BOOST_CHECK_EQUAL(entry.proof_kind, Consensus::BRIDGE_V2_PROOF_KIND_ETHEREUM_RECEIPT);
    BOOST_CHECK_EQUAL(entry.origin_deployment_block, DEPLOYMENT_BLOCK);
    BOOST_CHECK_EQUAL(entry.max_deposit_raw, 10'000'000'000);
    BOOST_CHECK_EQUAL(entry.mint_caps.max_per_block, CChainParams::B3RegTestBridgePins::DEFAULT_V2_MAX_PER_BLOCK);
    BOOST_CHECK_EQUAL(entry.mint_caps.max_per_epoch, CChainParams::B3RegTestBridgePins::DEFAULT_V2_MAX_PER_EPOCH);
    BOOST_CHECK_EQUAL(entry.mint_caps.max_per_block, Consensus::BRIDGE_V2_DEFAULT_CAP_RAW);
    BOOST_CHECK_EQUAL(entry.mint_caps.epoch_length_blocks,
                      defaults.busd_bridge->mint_caps->epoch_length_blocks);
    // OI-3: both code hashes commit to blockHash(R_f - 1) and stay unset.
    BOOST_CHECK(!entry.vault_runtime_code_hash);
    BOOST_CHECK(!v2.verifier.runtime_code_hash);

    BOOST_CHECK_EQUAL("0x" + HexStr(v2.verifier.address), VERIFIER_V2);
    BOOST_CHECK_EQUAL(v2.verifier.min_bridge_validators, 4U);
    BOOST_CHECK_EQUAL(v2.verifier.max_bridge_validators, 64U);
    BOOST_CHECK_EQUAL(v2.verifier.min_bridge_total_weight, 4U);

    // Burn gate thresholds (node changes section 8.6): always regtest
    // profile R, which no pin-file key can change.
    BOOST_CHECK(v2.gate == Consensus::BridgeV2GateParams::RegtestProfileR());
    BOOST_CHECK_EQUAL(v2.gate.observation_max_age_seconds, 4'800);
    BOOST_CHECK_EQUAL(v2.gate.max_finality_lag_blocks, 30);
    BOOST_CHECK_EQUAL(v2.gate.RotationAgeLimit(), 52'800);

    const Consensus::EthereumLightClientPins& light{v2.light_client};
    BOOST_CHECK_EQUAL("0x" + HexStr(light.trusted_checkpoint_root), Repeat("c1", 32));
    BOOST_CHECK_EQUAL(light.trusted_checkpoint_slot, 8'192U * 2 + 4'096);
    BOOST_CHECK_EQUAL("0x" + HexStr(light.genesis_validators_root), Repeat("d1", 32));
    BOOST_REQUIRE_EQUAL(light.fork_schedule.size(), 1U);
    BOOST_CHECK_EQUAL(HexStr(light.fork_schedule[0].fork_version), "b3000001");
    BOOST_CHECK_EQUAL(light.electra_epoch, 0U);
    BOOST_CHECK_EQUAL(light.min_sync_committee_participants,
                      Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY);
    BOOST_CHECK_EQUAL(light.max_sync_lag_slots, 8'192U);

    BOOST_CHECK(!params.BridgeV2RulesActive(V2_HEIGHT - 1));
    BOOST_CHECK(params.BridgeV2RulesActive(V2_HEIGHT));
    BOOST_CHECK(!params.BridgeV2BurnRulesActive(V2_HEIGHT - 1));
    BOOST_CHECK(params.BridgeV2BurnRulesActive(V2_HEIGHT));

    // Section 3.2 identities under this chain's domain; the bUSD V2 AssetId
    // is bound to the vault and differs from the V1 bUSD AssetId.
    BOOST_REQUIRE(params.legacy_final_hash);
    const auto domain{modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)};
    BOOST_REQUIRE(domain);
    const auto asset_v2{modern::ConfiguredBridgeV2AssetId(params)};
    BOOST_REQUIRE(asset_v2);
    BOOST_CHECK(*asset_v2 == modern::BridgeAssetIdV2(*domain, entry));
    BOOST_CHECK(*asset_v2 != *modern::ConfiguredBridgeAssetId(params));
    const auto entry_id{modern::ConfiguredBridgeV2EntryId(params)};
    BOOST_REQUIRE(entry_id);
    BOOST_CHECK(*entry_id == modern::BridgeVaultEntryIdV2(*domain, entry));
    const auto registry_v2{modern::ConfiguredBridgeV2RegistryId(params)};
    BOOST_REQUIRE(registry_v2);

    // Optional W2 and caps: the registry id V2 commits to the caps, never to
    // V or W2; the AssetId and the entry id commit to neither.
    UniValue tuned{ValidV2()};
    tuned.pushKV("burn_height", uint64_t{V2_HEIGHT + 5});
    tuned.pushKV("max_per_block", uint64_t{20'000'000'000});
    tuned.pushKV("max_per_epoch", uint64_t{40'000'000'000});
    const std::string tuned_contents{V2Root(tuned).write()};
    ArgsManager tuned_args;
    SetArgs(tuned_args, WriteFile("v2_tuned.json", tuned_contents));
    tuned_args.ForceSetArg("-b3freshlineageheight", std::to_string(FRESH_HEIGHT));
    const auto tuned_chain{CreateChainParams(tuned_args, ChainType::REGTEST)};
    const Consensus::Params& tuned_params{tuned_chain->GetConsensus()};
    BOOST_REQUIRE(tuned_params.bridge_v2);
    BOOST_CHECK_EQUAL(tuned_params.bridge_v2->burn_height, V2_HEIGHT + 5);
    BOOST_CHECK(tuned_params.BridgeV2RulesActive(V2_HEIGHT));
    BOOST_CHECK(!tuned_params.BridgeV2BurnRulesActive(V2_HEIGHT + 4));
    BOOST_CHECK(tuned_params.BridgeV2BurnRulesActive(V2_HEIGHT + 5));
    BOOST_CHECK_EQUAL(tuned_params.bridge_v2->vault.mint_caps.max_per_block, 20'000'000'000);
    BOOST_CHECK_EQUAL(tuned_params.bridge_v2->vault.mint_caps.max_per_epoch, 40'000'000'000);
    BOOST_CHECK(*tuned_chain->B3BridgePinsHash() == Sha256(tuned_contents));
    BOOST_CHECK(*modern::ConfiguredBridgeV2AssetId(tuned_params) == *asset_v2);
    BOOST_CHECK(*modern::ConfiguredBridgeV2EntryId(tuned_params) == *entry_id);
    BOOST_CHECK(*modern::ConfiguredBridgeV2RegistryId(tuned_params) != *registry_v2);
}

BOOST_FIXTURE_TEST_CASE(v1_and_v2_file_applies_both, BridgePinsSetup)
{
    // One file may carry both sections; the V2 entry may reuse the V1 mock
    // token but never a V1 vault or verifier (kernel_v2_cross_checks).
    const std::string contents{BothPins()};
    const auto parsed{ParseB3RegTestBridgePins(contents)};
    BOOST_CHECK(parsed.has_v1);
    BOOST_REQUIRE(parsed.v2);

    ArgsManager args;
    SetArgs(args, WriteFile("both.json", contents));
    args.ForceSetArg("-b3freshlineageheight", std::to_string(FRESH_HEIGHT));
    const auto chain{CreateChainParams(args, ChainType::REGTEST)};
    const Consensus::Params& params{chain->GetConsensus()};
    BOOST_CHECK(*chain->B3BridgePinsHash() == Sha256(contents));

    BOOST_REQUIRE(params.busd_bridge);
    BOOST_CHECK(*params.busd_bridge->withdrawal_mode ==
                Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1);
    BOOST_CHECK_EQUAL("0x" + HexStr(params.busd_bridge->asset.vault_address), VAULT);
    BOOST_REQUIRE(params.bridge_v2);
    BOOST_CHECK_EQUAL("0x" + HexStr(params.bridge_v2->vault.vault_address), VAULT_V2);
    BOOST_CHECK(params.bridge_v2->vault.token_address == params.busd_bridge->asset.token_address);
    BOOST_CHECK(*modern::ConfiguredBridgeV2AssetId(params) != *modern::ConfiguredDecentralizedBridgeAssetId(params));

    // The V1 section alone gives the same V1 bridge and no bridge V2.
    ArgsManager v1_args;
    SetArgs(v1_args, WriteFile("v1.json", ValidPins()));
    v1_args.ForceSetArg("-b3freshlineageheight", std::to_string(FRESH_HEIGHT));
    const auto v1_chain{CreateChainParams(v1_args, ChainType::REGTEST)};
    BOOST_CHECK(!v1_chain->GetConsensus().bridge_v2);
    BOOST_CHECK(*modern::ConfiguredBridgeRegistryId(v1_chain->GetConsensus()) ==
                *modern::ConfiguredBridgeRegistryId(params));
}

BOOST_AUTO_TEST_CASE(v2_schema_and_value_refusals)
{
    const auto pins{ParseB3RegTestBridgePins(ValidV2Pins())};
    BOOST_CHECK(!pins.has_v1);
    BOOST_REQUIRE(pins.v2);
    const CChainParams::B3RegTestBridgePins::V2Section& v2{*pins.v2};
    BOOST_CHECK_EQUAL(v2.origin_chain_id, 31'337U);
    BOOST_CHECK_EQUAL("0x" + HexStr(v2.vault), VAULT_V2);
    BOOST_CHECK_EQUAL("0x" + HexStr(v2.token), TOKEN);
    BOOST_CHECK_EQUAL("0x" + HexStr(v2.verifier), VERIFIER_V2);
    BOOST_CHECK_EQUAL(v2.origin_deployment_block, DEPLOYMENT_BLOCK);
    BOOST_CHECK_EQUAL(v2.activation_height, V2_HEIGHT);
    BOOST_CHECK(!v2.burn_height);
    BOOST_CHECK(!v2.max_per_block);
    BOOST_CHECK(!v2.max_per_epoch);
    BOOST_CHECK(!v2.verifier_max_epoch_lag);

    // The verifier's MAX_EPOCH_LAG mirror (optional; RegTest() compares it
    // with the gate's set expiry).
    BOOST_CHECK_EQUAL(*ParseB3RegTestBridgePins(WithV2("verifier_max_epoch_lag", uint64_t{72'000}))
                           .v2->verifier_max_epoch_lag,
                      72'000);
    CheckRefused(WithV2("verifier_max_epoch_lag", uint64_t{0}), "v2.verifier_max_epoch_lag must be positive");
    CheckRefused(WithV2("verifier_max_epoch_lag", uint64_t{1} << 32),
                 "v2.verifier_max_epoch_lag must be an unsigned integer of at most 32 bits");

    // Real network domains are refused exactly as in v1_decentralized.
    for (const uint64_t chain_id : {uint64_t{1}, uint64_t{11'155'111}, uint64_t{17'000}}) {
        CheckRefused(WithV2("origin_chain_id", chain_id), "v2.origin_chain_id must be 31337");
    }
    CheckRefused(WithV2Light("genesis_validators_root",
                             "0x" + HexStr(Consensus::ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT_BYTES)),
                 "v2.light_client.genesis_validators_root must not be the Ethereum mainnet root");
    for (const std::string version : {"0x55000000", "0x00000000", "0x05000000"}) {
        UniValue forks{UniValue::VARR};
        forks.push_back(Fork(0, version));
        CheckRefused(WithV2Light("fork_schedule", forks),
                     "v2.light_client.fork_schedule[0].fork_version must use the synthetic 0xb3 namespace");
    }
    CheckRefused(WithV2Light("electra_epoch", uint64_t{1}), "v2.light_client.electra_epoch must be 0");
    CheckRefused(WithV2Light("max_sync_lag_slots", uint64_t{0}), "v2.light_client pins are inconsistent");

    // Addresses: non-zero and pairwise distinct.
    for (const std::string key : {"vault", "token", "verifier"}) {
        CheckRefused(WithV2(key, Repeat("00", 20)), "v2 vault, token and verifier addresses must be non-zero");
    }
    CheckRefused(WithV2("token", VAULT_V2), "v2 vault, token and verifier addresses must be pairwise distinct");
    CheckRefused(WithV2("verifier", VAULT_V2), "v2 vault, token and verifier addresses must be pairwise distinct");
    CheckRefused(WithV2("verifier", TOKEN), "v2 vault, token and verifier addresses must be pairwise distinct");
    CheckRefused(WithV2("vault", VAULT_V2.substr(2)), "v2.vault must be a 0x-prefixed 20-byte hex string");
    CheckRefused(WithV2("token_runtime_code_hash", Repeat("00", 32)), "v2.token_runtime_code_hash must be non-zero");

    // Caps: 0 < max_deposit_raw <= max_per_block <= max_per_epoch <= MAX_MONEY.
    BOOST_CHECK_NO_THROW(ParseB3RegTestBridgePins(WithV2("max_deposit_raw", uint64_t{1})));
    CheckRefused(WithV2("max_deposit_raw", uint64_t{0}), "v2 caps must satisfy");
    CheckRefused(WithV2("max_deposit_raw", uint64_t{CChainParams::B3RegTestBridgePins::DEFAULT_V2_MAX_PER_BLOCK + 1}),
                 "v2 caps must satisfy");
    CheckRefused(WithV2("max_per_block", uint64_t{9'999'999'999}), "v2 caps must satisfy");
    CheckRefused(WithV2("max_per_epoch", uint64_t{9'999'999'999}), "v2 caps must satisfy");
    CheckRefused(WithV2("max_per_epoch", uint64_t{MAX_MONEY} + 1), "v2.max_per_epoch exceeds MAX_MONEY");
    CheckRefused(WithV2("max_deposit_raw", uint64_t{MAX_MONEY} + 1), "v2.max_deposit_raw exceeds MAX_MONEY");
    {
        UniValue raised{ValidV2()};
        raised.pushKV("max_per_block", uint64_t{20'000'000'000});
        raised.pushKV("max_per_epoch", uint64_t{20'000'000'000});
        raised.pushKV("max_deposit_raw", uint64_t{20'000'000'000});
        const auto raised_pins{ParseB3RegTestBridgePins(V2Root(raised).write())};
        BOOST_CHECK_EQUAL(*raised_pins.v2->max_per_block, 20'000'000'000);
        BOOST_CHECK_EQUAL(*raised_pins.v2->max_per_epoch, 20'000'000'000);
        UniValue inverted{raised};
        inverted.pushKV("max_per_epoch", uint64_t{19'999'999'999});
        CheckRefused(V2Root(inverted).write(), "v2 caps must satisfy");
    }

    // The bootstrap window binds to the v2 vault's deployment block.
    BOOST_CHECK_NO_THROW(ParseB3RegTestBridgePins(
        WithV2Light("checkpoint_execution_block", DEPLOYMENT_BLOCK + bridge::MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS)));
    CheckRefused(WithV2Light("checkpoint_execution_block", DEPLOYMENT_BLOCK + 20'001),
                 "v2.light_client.checkpoint_execution_block (20004) must lie within 20000 blocks at or after origin_deployment_block (3)");
    CheckRefused(WithV2Light("checkpoint_execution_block", DEPLOYMENT_BLOCK - 1), "within 20000 blocks");
    CheckRefused(WithV2("origin_deployment_block", DEPLOYMENT_BLOCK + 1), "within 20000 blocks");
    CheckRefused(WithV2("origin_deployment_block", uint64_t{0}), "v2.origin_deployment_block must be at least 1");

    // Verifier V2 member and weight bounds.
    CheckRefused(WithV2("min_bridge_validators", uint64_t{3}), "v2 verifier pins need 4 <= min_bridge_validators");
    CheckRefused(WithV2("max_bridge_validators", uint64_t{65}), "v2 verifier pins need 4 <= min_bridge_validators");
    CheckRefused(WithV2("min_bridge_validators", uint64_t{65}), "v2 verifier pins need 4 <= min_bridge_validators");
    CheckRefused(WithV2("min_bridge_total_weight", uint64_t{0}), "v2 verifier pins need 4 <= min_bridge_validators");
    {
        UniValue narrow{ValidV2()};
        narrow.pushKV("min_bridge_validators", uint64_t{10});
        narrow.pushKV("max_bridge_validators", uint64_t{9});
        CheckRefused(V2Root(narrow).write(), "v2 verifier pins need");
        narrow.pushKV("max_bridge_validators", uint64_t{10});
        BOOST_CHECK_NO_THROW(ParseB3RegTestBridgePins(V2Root(narrow).write()));
    }

    // V and W2.
    const std::string max_height{std::to_string(std::numeric_limits<int>::max())};
    CheckRefused(WithV2("activation_height", uint64_t{0}), "v2.activation_height must be between 1 and " + max_height);
    CheckRefused(WithV2("activation_height", uint64_t{1} << 31), "v2.activation_height must be between 1 and " + max_height);
    CheckRefused(WithV2("activation_height", uint64_t{1} << 32),
                 "v2.activation_height must be an unsigned integer of at most 32 bits");
    CheckRefused(WithV2("activation_height", int64_t{-1}), "v2.activation_height must be an unsigned integer");
    CheckRefused(WithV2("burn_height", uint64_t{V2_HEIGHT - 1}),
                 "v2.burn_height (199) must be between activation_height (200) and " + max_height);
    CheckRefused(WithV2("burn_height", uint64_t{1} << 31), "v2.burn_height (2147483648) must be between");
    BOOST_CHECK_EQUAL(*ParseB3RegTestBridgePins(WithV2("burn_height", uint64_t{V2_HEIGHT})).v2->burn_height, V2_HEIGHT);

    // OI-3: values that commit to blockHash(R_f - 1), and V1-only pins, are
    // not keys of the v2 section.
    for (const std::string key : {"vault_runtime_code_hash", "verifier_runtime_code_hash", "lineage_id",
                                  "genesis_set_hash", "bootstrap_validator_set_hash", "max_epoch_lag",
                                  "set_expiry"}) {
        CheckRefused(WithV2(key, Repeat("11", 32)), "unknown key v2." + key);
    }
    // The burn gate is profile R by construction: neither a gate section nor
    // any threshold is a key (node changes section 8.6).
    {
        UniValue gate{UniValue::VOBJ};
        gate.pushKV("observation_max_age_seconds", uint64_t{4'800});
        CheckRefused(WithV2("gate", gate), "unknown key v2.gate");
    }
    for (const std::string key : {"set_expiry_seconds", "release_margin_seconds", "burn_margin_seconds",
                                  "observation_max_age_seconds", "max_epoch_gap", "max_finality_lag_blocks",
                                  "wind_down_margin_ppm", "mempool_close_margin_seconds"}) {
        CheckRefused(WithV2(key, uint64_t{1}), "unknown key v2." + key);
    }
    CheckRefused(WithV2Light("slot", uint64_t{1}), "unknown key v2.light_client.slot");

    // Every required key is required.
    for (const std::string key : {"origin_chain_id", "vault", "token", "token_runtime_code_hash",
                                  "origin_deployment_block", "max_deposit_raw", "light_client", "verifier",
                                  "min_bridge_validators", "max_bridge_validators", "min_bridge_total_weight",
                                  "activation_height"}) {
        CheckRefused(V2Root(Without(ValidV2(), key)).write(), "missing key v2." + key);
    }
    CheckRefused(WithV2("light_client", Without(ValidLightClient(), "electra_epoch")),
                 "missing key v2.light_client.electra_epoch");

    // Section and file shape.
    CheckRefused(V2Root(UniValue{UniValue::VOBJ}).write(), "missing key v2.");
    CheckRefused(V2Root(UniValue{UniValue::VARR}).write(), "v2 must be a JSON object");
    CheckRefused(V2Root(UniValue{}).write(), "v2 must be a JSON object");
    CheckRefused(Without(V2Root(), "v2").write(), "file needs v1_decentralized or v2");
    std::string duplicate{ValidV2Pins()};
    duplicate.insert(duplicate.find("\"v2\""), "\"v2\":{},");
    CheckRefused(duplicate, "duplicate key file.v2");
    UniValue wrong_format{V2Root()};
    wrong_format.pushKV("format", "b3-regtest-bridge-pins/2");
    CheckRefused(wrong_format.write(), "format must be");

    // A refused v2 section fails the whole file, even beside a valid V1 one.
    CheckRefused(BothPins(Without(ValidV2(), "verifier")), "missing key v2.verifier");
    CheckRefused(BothPins(ValidV2(0)), "v2.activation_height must be between 1 and");
}

BOOST_AUTO_TEST_CASE(kernel_v2_cross_checks)
{
    const auto v2_pins{ParseB3RegTestBridgePins(ValidV2Pins())};
    BOOST_CHECK_NO_THROW(CChainParams::RegTest(FlowMeshOptions(v2_pins)));

    // The V2 withdrawal root rides on fresh-lineage certificates.
    BOOST_CHECK_EXCEPTION(CChainParams::RegTest(FlowMeshOptions(v2_pins, std::nullopt)), std::runtime_error,
                          HasReason("-b3bridgepins v2 requires -b3freshlineageheight"));

    // V >= A3: R_f = 172 <= V = 192 < A3 = 193 is refused; V = A3 applies.
    BOOST_CHECK_EXCEPTION(
        CChainParams::RegTest(FlowMeshOptions(ParseB3RegTestBridgePins(V2Root(ValidV2(DEFAULT_A3 - 1)).write()))),
        std::runtime_error,
        HasReason("-b3bridgepins v2.activation_height (192) must be at or above the bUSD activation height A3 (193)"));
    {
        const auto chain{CChainParams::RegTest(
            FlowMeshOptions(ParseB3RegTestBridgePins(V2Root(ValidV2(DEFAULT_A3)).write())))};
        BOOST_REQUIRE(chain->GetConsensus().bridge_v2);
        BOOST_CHECK_EQUAL(chain->GetConsensus().bridge_v2->activation_height, DEFAULT_A3);
    }

    // V >= R_f: R_f = 262 (261 = 161 + 100 on the grid) above V = 200.
    BOOST_CHECK_EXCEPTION(CChainParams::RegTest(FlowMeshOptions(v2_pins, 262)), std::runtime_error,
                          HasReason("-b3bridgepins v2.activation_height (200) must be at or above -b3freshlineageheight (262)"));
    // V = R_f applies: R_f = 202 (201 = 161 + 40).
    {
        const auto chain{CChainParams::RegTest(
            FlowMeshOptions(ParseB3RegTestBridgePins(V2Root(ValidV2(202)).write()), 202))};
        BOOST_REQUIRE(chain->GetConsensus().bridge_v2);
        BOOST_CHECK(chain->GetConsensus().BridgeV2RulesActive(202));
        BOOST_CHECK(!chain->GetConsensus().BridgeV2RulesActive(201));
    }

    // The verifier's MAX_EPOCH_LAG must be the gate's set expiry (profile R
    // 72,000 s): a shorter verifier window would leave burns open after the
    // verifier stops accepting roots.
    {
        const auto chain{CChainParams::RegTest(FlowMeshOptions(
            ParseB3RegTestBridgePins(WithV2("verifier_max_epoch_lag", uint64_t{72'000}))))};
        BOOST_REQUIRE(chain->GetConsensus().bridge_v2);
        BOOST_CHECK_EQUAL(*chain->GetConsensus().bridge_v2->verifier.max_epoch_lag_seconds, 72'000);
        BOOST_CHECK_EQUAL(Consensus::BridgeV2GateParams::RegtestProfileR().set_expiry_seconds, 72'000);
        BOOST_CHECK(!CChainParams::RegTest(FlowMeshOptions(v2_pins))->GetConsensus()
                         .bridge_v2->verifier.max_epoch_lag_seconds);
        for (const uint64_t lag : {uint64_t{71'999}, uint64_t{72'001}, uint64_t{2'592'000}}) {
            BOOST_CHECK_EXCEPTION(
                CChainParams::RegTest(FlowMeshOptions(ParseB3RegTestBridgePins(WithV2("verifier_max_epoch_lag", lag)))),
                std::runtime_error,
                HasReason("-b3bridgepins v2.verifier_max_epoch_lag (" + std::to_string(lag) +
                          ") must equal the regtest burn gate's set expiry (72000)"));
        }
    }

    // Hand-built sections: W2 < V, and an incomplete entry.
    {
        auto early_burn{v2_pins};
        early_burn.v2->burn_height = V2_HEIGHT - 1;
        BOOST_CHECK_EXCEPTION(CChainParams::RegTest(FlowMeshOptions(early_burn)), std::runtime_error,
                              HasReason("-b3bridgepins v2.burn_height (199) must be at or above v2.activation_height (200)"));
        auto incomplete{v2_pins};
        incomplete.v2->token_runtime_code_hash = uint256{};
        BOOST_CHECK_EXCEPTION(CChainParams::RegTest(FlowMeshOptions(incomplete)), std::runtime_error,
                              HasReason("-b3bridgepins v2 does not form a complete bridge V2 configuration"));
        auto blank{v2_pins};
        blank.v2 = CChainParams::B3RegTestBridgePins::V2Section{};
        blank.v2->activation_height = V2_HEIGHT;
        BOOST_CHECK_EXCEPTION(CChainParams::RegTest(FlowMeshOptions(blank)), std::runtime_error,
                              HasReason("-b3bridgepins v2 does not form a complete bridge V2 configuration"));
        // has_v1 = false keeps the synthetic V1 bridge even though the V1
        // fields of these hand-built pins are blank.
        BOOST_CHECK(!v2_pins.has_v1);
        BOOST_CHECK(Consensus::BridgeAddressIsNull(v2_pins.vault));
    }

    // No V2 contract may sit on a V1 bridge contract: a V2 entry on the V1
    // vault would mint one deposit twice, once per nullifier namespace.
    const std::string overlap{"-b3bridgepins v2 vault and verifier must differ from every V1 bridge contract"};
    for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
             {"vault", VAULT}, {"vault", TOKEN}, {"vault", VERIFIER},
             {"verifier", VAULT}, {"verifier", TOKEN}, {"verifier", VERIFIER},
             {"token", VAULT}, {"token", VERIFIER}}) {
        UniValue v2{ValidV2()};
        v2.pushKV("token", TOKEN_V2);
        v2.pushKV(key, value);
        BOOST_CHECK_EXCEPTION(CChainParams::RegTest(FlowMeshOptions(ParseB3RegTestBridgePins(BothPins(v2)))),
                              std::runtime_error, HasReason(overlap));
    }
    // The same contracts are fine without the V1 section (synthetic V1).
    {
        UniValue v2{ValidV2()};
        v2.pushKV("vault", VAULT);
        v2.pushKV("verifier", VERIFIER);
        BOOST_CHECK_NO_THROW(CChainParams::RegTest(FlowMeshOptions(ParseB3RegTestBridgePins(V2Root(v2).write()))));
    }
    // Reusing the V1 token, or a fresh token, is fine beside the V1 section.
    BOOST_CHECK_NO_THROW(CChainParams::RegTest(FlowMeshOptions(ParseB3RegTestBridgePins(BothPins()))));
    {
        UniValue v2{ValidV2()};
        v2.pushKV("token", TOKEN_V2);
        BOOST_CHECK_NO_THROW(CChainParams::RegTest(FlowMeshOptions(ParseB3RegTestBridgePins(BothPins(v2)))));
    }
    // Against the synthetic MANAGED_V1 bridge: its vault and its authority.
    {
        UniValue on_vault{ValidV2()};
        on_vault.pushKV("vault", "0x" + HexStr(Consensus::ETHEREUM_MAINNET_BUSD_IDENTITY.vault_address));
        BOOST_CHECK_EXCEPTION(CChainParams::RegTest(FlowMeshOptions(ParseB3RegTestBridgePins(V2Root(on_vault).write()))),
                              std::runtime_error, HasReason(overlap));
        UniValue on_authority{ValidV2()};
        on_authority.pushKV("verifier", Repeat("42", 20));
        BOOST_CHECK_EXCEPTION(CChainParams::RegTest(FlowMeshOptions(ParseB3RegTestBridgePins(V2Root(on_authority).write()))),
                              std::runtime_error, HasReason(overlap));
    }

    // Pins still require -b3flowmeshtest, whatever sections they carry.
    CChainParams::RegTestOptions no_flowmesh{FlowMeshOptions(v2_pins)};
    no_flowmesh.b3_modern->flowmesh_test = false;
    no_flowmesh.b3_modern->fresh_lineage_height.reset();
    BOOST_CHECK_EXCEPTION(CChainParams::RegTest(no_flowmesh), std::runtime_error,
                          HasReason("-b3bridgepins requires -b3flowmeshtest"));
}

BOOST_FIXTURE_TEST_CASE(v2_pins_refused_off_regtest, BridgePinsSetup)
{
    const std::string path{WriteFile("v2.json", ValidV2Pins())};
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET,
                                  ChainType::TESTNET4, ChainType::SIGNET}) {
        ArgsManager args;
        SetArgs(args, path);
        args.ForceSetArg("-b3freshlineageheight", std::to_string(FRESH_HEIGHT));
        BOOST_CHECK_EXCEPTION(CreateChainParams(args, chain), std::runtime_error,
                              HasReason("-b3bridgepins is regtest-only"));
        ArgsManager bare;
        bare.ForceSetArg("-b3bridgepins", path);
        BOOST_CHECK_EXCEPTION(CreateChainParams(bare, chain), std::runtime_error,
                              HasReason("-b3bridgepins is regtest-only"));
        // Without the knob nothing changes: no bridge V2 and no pin hash.
        const auto params{CreateChainParams(ArgsManager{}, chain)};
        BOOST_CHECK(!params->GetConsensus().bridge_v2);
        BOOST_CHECK(!params->GetConsensus().BridgeV2RulesActive(std::numeric_limits<int>::max()));
        BOOST_CHECK(!params->B3BridgePinsHash());
    }

    // Regtest outside the FlowMesh configuration refuses the file too.
    for (const auto& [modern, flowmesh] : {std::pair{false, false}, std::pair{true, false},
                                           std::pair{false, true}}) {
        ArgsManager args;
        SetArgs(args, path, modern, flowmesh);
        BOOST_CHECK_EXCEPTION(CreateChainParams(args, ChainType::REGTEST), std::runtime_error,
                              HasReason("-b3bridgepins requires -b3modernregtest and -b3flowmeshtest (regtest-only)"));
    }
    BOOST_CHECK(!CreateChainParams(ArgsManager{}, ChainType::REGTEST)->GetConsensus().bridge_v2);

    // FlowMesh regtest without -b3freshlineageheight refuses a v2 file.
    ArgsManager no_fresh;
    SetArgs(no_fresh, path);
    BOOST_CHECK_EXCEPTION(CreateChainParams(no_fresh, ChainType::REGTEST), std::runtime_error,
                          HasReason("-b3bridgepins v2 requires -b3freshlineageheight"));
}

BOOST_AUTO_TEST_SUITE_END()
