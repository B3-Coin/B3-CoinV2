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

#include <cassert>
#include <cstdint>
#include <fstream>
#include <functional>
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

// B-23 successor stand-ins (node changes section 7.6): v' at a fresh address
// and h_m a few blocks above V = 200.
const std::string SUCCESSOR_VAULT{"0x4646464646464646464646464646464646464646"};
constexpr int MIGRATION_HEIGHT{V2_HEIGHT + 20};

//! The bUSD V2 AssetId of the launch entry a successor-free pin file
//! configures, recomputed from the chain's genesis and X (every FlowMesh
//! regtest chain shares one modern domain).
uint256 LaunchAssetIdOf(const std::string& launch_pins)
{
    const auto chain{CChainParams::RegTest(FlowMeshOptions(ParseB3RegTestBridgePins(launch_pins)))};
    const Consensus::Params& params{chain->GetConsensus()};
    assert(params.legacy_final_hash && params.bridge_v2 && !params.bridge_v2->successor);
    const auto domain{modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)};
    assert(domain);
    return modern::BridgeAssetIdV2(*domain, params.bridge_v2->vault);
}

//! LaunchAssetIdOf the default v2 section.
uint256 LaunchAssetIdV2()
{
    static const uint256 asset_id{LaunchAssetIdOf(ValidV2Pins())};
    return asset_id;
}

//! 0x-prefixed raw bytes of a 32-byte value (Vault V2 B3_ASSET_ID order).
std::string EvmHex(const uint256& value) { return "0x" + HexStr(value); }

//! A valid v2.successor object for the default v2 section.
UniValue ValidSuccessor(int migration_height = MIGRATION_HEIGHT)
{
    UniValue successor{UniValue::VOBJ};
    successor.pushKV("vault", SUCCESSOR_VAULT);
    successor.pushKV("predecessor", VAULT_V2);
    successor.pushKV("asset_id", EvmHex(LaunchAssetIdV2()));
    successor.pushKV("migration_height", static_cast<uint64_t>(migration_height));
    return successor;
}

//! `object` with <key> replaced or added.
UniValue With(UniValue object, const std::string& key, const UniValue& value)
{
    object.pushKV(key, value);
    return object;
}

//! A v2-only file whose v2 section (default ValidV2()) carries `successor`.
std::string SuccessorPins(const UniValue& successor = ValidSuccessor(), UniValue v2 = ValidV2())
{
    v2.pushKV("successor", successor);
    return V2Root(v2).write();
}

//! The v2-only successor file with v2.successor.<key> replaced or added.
std::string WithSuccessor(const std::string& key, const UniValue& value)
{
    return SuccessorPins(With(ValidSuccessor(), key, value));
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
    // Owner ruling R10: without its own pin the sponsored sighash rule stays
    // off at every height, although V2 burns are active.
    BOOST_CHECK(!v2.sponsored_sighash_height);
    for (const int height : {V2_HEIGHT - 1, V2_HEIGHT, V2_HEIGHT + 1, std::numeric_limits<int>::max()}) {
        BOOST_CHECK(!params.BridgeV2SponsoredSighashActive(height));
    }

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

    // Owner ruling R10's own pin: the sponsored sighash rule from its height
    // (>= V) only. It enters no identity, and the record guard's file hash
    // covers it like every other key.
    UniValue sponsored{ValidV2()};
    sponsored.pushKV("sponsored_sighash_height", uint64_t{V2_HEIGHT + 7});
    const std::string sponsored_contents{V2Root(sponsored).write()};
    ArgsManager sponsored_args;
    SetArgs(sponsored_args, WriteFile("v2_sponsored.json", sponsored_contents));
    sponsored_args.ForceSetArg("-b3freshlineageheight", std::to_string(FRESH_HEIGHT));
    const auto sponsored_chain{CreateChainParams(sponsored_args, ChainType::REGTEST)};
    const Consensus::Params& sponsored_params{sponsored_chain->GetConsensus()};
    BOOST_REQUIRE(sponsored_params.bridge_v2);
    BOOST_CHECK(sponsored_params.bridge_v2->sponsored_sighash_height == std::optional<int32_t>{V2_HEIGHT + 7});
    BOOST_CHECK(sponsored_params.BridgeV2BurnRulesActive(V2_HEIGHT));
    BOOST_CHECK(!sponsored_params.BridgeV2SponsoredSighashActive(V2_HEIGHT));
    BOOST_CHECK(!sponsored_params.BridgeV2SponsoredSighashActive(V2_HEIGHT + 6));
    BOOST_CHECK(sponsored_params.BridgeV2SponsoredSighashActive(V2_HEIGHT + 7));
    BOOST_CHECK(*sponsored_chain->B3BridgePinsHash() == Sha256(sponsored_contents));
    BOOST_CHECK(*sponsored_chain->B3BridgePinsHash() != Sha256(contents));
    BOOST_CHECK(*modern::ConfiguredBridgeV2AssetId(sponsored_params) == *asset_v2);
    BOOST_CHECK(*modern::ConfiguredBridgeV2EntryId(sponsored_params) == *entry_id);
    BOOST_CHECK(*modern::ConfiguredBridgeV2RegistryId(sponsored_params) == *registry_v2);
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
    // Owner ruling R10's sponsored sighash pin: absent means off; when
    // present, at or above V.
    BOOST_CHECK(!ParseB3RegTestBridgePins(ValidV2Pins()).v2->sponsored_sighash_height);
    CheckRefused(WithV2("sponsored_sighash_height", uint64_t{V2_HEIGHT - 1}),
                 "v2.sponsored_sighash_height (199) must be between activation_height (200) and " + max_height);
    CheckRefused(WithV2("sponsored_sighash_height", uint64_t{1} << 31),
                 "v2.sponsored_sighash_height (2147483648) must be between");
    CheckRefused(WithV2("sponsored_sighash_height", int64_t{-1}),
                 "v2.sponsored_sighash_height must be an unsigned integer");
    CheckRefused(WithV2("sponsored_sighash_height", "300"),
                 "v2.sponsored_sighash_height must be an unsigned integer");
    BOOST_CHECK_EQUAL(*ParseB3RegTestBridgePins(WithV2("sponsored_sighash_height", uint64_t{V2_HEIGHT}))
                           .v2->sponsored_sighash_height,
                      V2_HEIGHT);

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

BOOST_FIXTURE_TEST_CASE(v2_successor_applies, BridgePinsSetup)
{
    // The launch-only chain the successor file extends: no successor, so the
    // migration never activates and no successor identity exists.
    ArgsManager launch_args;
    SetArgs(launch_args, WriteFile("v2_launch.json", ValidV2Pins()));
    launch_args.ForceSetArg("-b3freshlineageheight", std::to_string(FRESH_HEIGHT));
    const auto launch_chain{CreateChainParams(launch_args, ChainType::REGTEST)};
    const Consensus::Params& launch{launch_chain->GetConsensus()};
    BOOST_REQUIRE(launch.bridge_v2);
    BOOST_CHECK(!launch.bridge_v2->successor);
    BOOST_CHECK(!modern::ConfiguredBridgeV2EntryId(launch, Consensus::BridgeV2EntryRole::SUCCESSOR));
    for (const int height : {V2_HEIGHT, MIGRATION_HEIGHT, std::numeric_limits<int>::max()}) {
        BOOST_CHECK(!launch.BridgeV2MigrationActive(height));
    }

    // Parsed fields; the AssetId keeps raw EVM byte order.
    const std::string contents{SuccessorPins()};
    const auto parsed{ParseB3RegTestBridgePins(contents)};
    BOOST_REQUIRE(parsed.v2 && parsed.v2->successor);
    const CChainParams::B3RegTestBridgePins::V2Section::SuccessorSection& pinned{*parsed.v2->successor};
    BOOST_CHECK_EQUAL("0x" + HexStr(pinned.vault), SUCCESSOR_VAULT);
    BOOST_CHECK_EQUAL("0x" + HexStr(pinned.predecessor), VAULT_V2);
    BOOST_CHECK(pinned.asset_id == LaunchAssetIdV2());
    const std::string pinned_asset_hex{ValidSuccessor().find_value("asset_id").get_str()};
    BOOST_CHECK_EQUAL(EvmHex(pinned.asset_id), pinned_asset_hex);
    BOOST_CHECK_EQUAL(pinned.migration_height, MIGRATION_HEIGHT);
    BOOST_CHECK(!pinned.origin_deployment_block);
    BOOST_CHECK(!ParseB3RegTestBridgePins(ValidV2Pins()).v2->successor);

    ArgsManager args;
    SetArgs(args, WriteFile("v2_successor.json", contents));
    args.ForceSetArg("-b3freshlineageheight", std::to_string(FRESH_HEIGHT));
    const auto chain{CreateChainParams(args, ChainType::REGTEST)};
    const Consensus::Params& params{chain->GetConsensus()};

    // Pinned before first start: the record guard hashes the whole file,
    // successor included, so it freezes with v from the first block >= A3.
    BOOST_REQUIRE(chain->B3BridgePinsHash());
    BOOST_CHECK(*chain->B3BridgePinsHash() == Sha256(contents));
    BOOST_CHECK(*chain->B3BridgePinsHash() != *launch_chain->B3BridgePinsHash());

    BOOST_REQUIRE(params.bridge_v2);
    const Consensus::BridgeV2Params& v2{*params.bridge_v2};
    BOOST_CHECK(v2.Valid());
    BOOST_CHECK(v2.SuccessorValid());
    BOOST_REQUIRE(v2.successor);
    const Consensus::BridgeV2SuccessorEntry& successor{*v2.successor};
    const Consensus::BridgeV2VaultEntry& entry{v2.vault};
    const Consensus::BridgeV2VaultEntry& next{successor.entry};
    BOOST_CHECK(v2.Entry(Consensus::BridgeV2EntryRole::SUCCESSOR) == &next);

    // The launch envelope is exactly the launch-only file's.
    const Consensus::BridgeV2Params& launch_v2{*launch.bridge_v2};
    BOOST_CHECK_EQUAL(v2.activation_height, launch_v2.activation_height);
    BOOST_CHECK_EQUAL(v2.burn_height, launch_v2.burn_height);
    BOOST_CHECK(entry.vault_address == launch_v2.vault.vault_address);
    BOOST_CHECK(entry.origin_deployment_block == launch_v2.vault.origin_deployment_block);
    BOOST_CHECK(v2.verifier.address == launch_v2.verifier.address);
    BOOST_CHECK(v2.gate == launch_v2.gate);

    // Mapped successor fields.
    BOOST_CHECK_EQUAL("0x" + HexStr(next.vault_address), SUCCESSOR_VAULT);
    BOOST_CHECK(!next.vault_runtime_code_hash);
    BOOST_CHECK_EQUAL(next.origin_deployment_block, DEPLOYMENT_BLOCK);
    BOOST_CHECK(successor.predecessor == entry.vault_address);
    BOOST_CHECK(successor.asset_id == LaunchAssetIdV2());
    BOOST_CHECK_EQUAL(successor.migration_height, MIGRATION_HEIGHT);

    // Inherited from v: chain, token, token code hash, decimals, proof kind,
    // MAX_DEPOSIT_RAW and the mint caps.
    BOOST_CHECK_EQUAL(next.origin_chain_id, entry.origin_chain_id);
    BOOST_CHECK_EQUAL(next.origin_chain_id, 31'337U);
    BOOST_CHECK(next.token_address == entry.token_address);
    BOOST_CHECK(next.token_runtime_code_hash == entry.token_runtime_code_hash);
    BOOST_CHECK_EQUAL(next.origin_decimals, entry.origin_decimals);
    BOOST_CHECK_EQUAL(next.asset_decimals, entry.asset_decimals);
    BOOST_CHECK_EQUAL(next.proof_kind, entry.proof_kind);
    BOOST_CHECK_EQUAL(next.max_deposit_raw, entry.max_deposit_raw);
    BOOST_CHECK_EQUAL(next.mint_caps.max_per_block, entry.mint_caps.max_per_block);
    BOOST_CHECK_EQUAL(next.mint_caps.max_per_epoch, entry.mint_caps.max_per_epoch);
    BOOST_CHECK_EQUAL(next.mint_caps.epoch_length_blocks, entry.mint_caps.epoch_length_blocks);

    // Identities: v' carries v's AssetId (not the one its own address would
    // give), has its own entry id, leaves the launch ids alone and extends
    // the registry id.
    const auto domain{modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)};
    BOOST_REQUIRE(domain);
    const auto asset{modern::ConfiguredBridgeV2AssetId(params)};
    BOOST_REQUIRE(asset);
    BOOST_CHECK(*asset == *modern::ConfiguredBridgeV2AssetId(launch));
    BOOST_CHECK(*asset == successor.asset_id);
    BOOST_CHECK(*asset == modern::BridgeAssetIdV2(*domain, entry));
    BOOST_CHECK(*asset != modern::BridgeAssetIdV2(*domain, next));
    const auto launch_id{modern::ConfiguredBridgeV2EntryId(params)};
    const auto successor_id{modern::ConfiguredBridgeV2EntryId(params, Consensus::BridgeV2EntryRole::SUCCESSOR)};
    BOOST_REQUIRE(launch_id && successor_id);
    BOOST_CHECK(*launch_id == *modern::ConfiguredBridgeV2EntryId(launch));
    BOOST_CHECK(*successor_id == modern::BridgeVaultEntryIdV2(*domain, next));
    BOOST_CHECK(*successor_id != *launch_id);
    const auto registry{modern::ConfiguredBridgeV2RegistryId(params)};
    BOOST_REQUIRE(registry);
    BOOST_CHECK(*registry != *modern::ConfiguredBridgeV2RegistryId(launch));

    // The migration activates only from h_m.
    BOOST_CHECK(params.BridgeV2RulesActive(V2_HEIGHT));
    for (const int height : {V2_HEIGHT - 1, V2_HEIGHT, MIGRATION_HEIGHT - 1}) {
        BOOST_CHECK(!params.BridgeV2MigrationActive(height));
        BOOST_CHECK(modern::ActiveBridgeV2Role(params, height) == Consensus::BridgeV2EntryRole::LAUNCH);
        BOOST_CHECK(!modern::ResolveBridgeV2Entry(params, *successor_id, height));
    }
    for (const int height : {MIGRATION_HEIGHT, MIGRATION_HEIGHT + 1, std::numeric_limits<int>::max()}) {
        BOOST_CHECK(params.BridgeV2MigrationActive(height));
        BOOST_CHECK(modern::ActiveBridgeV2Role(params, height) == Consensus::BridgeV2EntryRole::SUCCESSOR);
        BOOST_CHECK(modern::ResolveBridgeV2Entry(params, *successor_id, height) ==
                    Consensus::BridgeV2EntryRole::SUCCESSOR);
        BOOST_CHECK(modern::ResolveBridgeV2Entry(params, *launch_id, height) ==
                    Consensus::BridgeV2EntryRole::LAUNCH);
    }

    // Optional deployment bound: committed by the registry id, not by the
    // entry id; v's own bound is untouched.
    {
        const auto tuned_chain{CChainParams::RegTest(FlowMeshOptions(
            ParseB3RegTestBridgePins(WithSuccessor("origin_deployment_block", uint64_t{7}))))};
        const Consensus::Params& tuned{tuned_chain->GetConsensus()};
        BOOST_REQUIRE(tuned.bridge_v2 && tuned.bridge_v2->successor);
        BOOST_CHECK_EQUAL(tuned.bridge_v2->successor->entry.origin_deployment_block, 7U);
        BOOST_CHECK_EQUAL(tuned.bridge_v2->vault.origin_deployment_block, DEPLOYMENT_BLOCK);
        BOOST_CHECK(*modern::ConfiguredBridgeV2EntryId(tuned, Consensus::BridgeV2EntryRole::SUCCESSOR) == *successor_id);
        BOOST_CHECK(*modern::ConfiguredBridgeV2RegistryId(tuned) != *registry);
    }
    // h_m, like V and W2, is not part of the registry id.
    {
        const auto moved_chain{CChainParams::RegTest(FlowMeshOptions(
            ParseB3RegTestBridgePins(SuccessorPins(ValidSuccessor(MIGRATION_HEIGHT + 5)))))};
        const Consensus::Params& moved{moved_chain->GetConsensus()};
        BOOST_CHECK(*modern::ConfiguredBridgeV2RegistryId(moved) == *registry);
        BOOST_CHECK(!moved.BridgeV2MigrationActive(MIGRATION_HEIGHT + 4));
        BOOST_CHECK(moved.BridgeV2MigrationActive(MIGRATION_HEIGHT + 5));
    }
    // h_m == W2 > V applies, and v's tuned caps are inherited.
    {
        UniValue tuned_v2{ValidV2()};
        tuned_v2.pushKV("burn_height", uint64_t{V2_HEIGHT + 5});
        tuned_v2.pushKV("max_per_block", uint64_t{20'000'000'000});
        tuned_v2.pushKV("max_per_epoch", uint64_t{40'000'000'000});
        const auto tuned_chain{CChainParams::RegTest(FlowMeshOptions(
            ParseB3RegTestBridgePins(SuccessorPins(ValidSuccessor(V2_HEIGHT + 5), tuned_v2))))};
        const Consensus::Params& tuned{tuned_chain->GetConsensus()};
        BOOST_REQUIRE(tuned.bridge_v2 && tuned.bridge_v2->successor);
        BOOST_CHECK(tuned.bridge_v2->Valid());
        BOOST_CHECK(!tuned.BridgeV2MigrationActive(V2_HEIGHT + 4));
        BOOST_CHECK(tuned.BridgeV2MigrationActive(V2_HEIGHT + 5));
        BOOST_CHECK_EQUAL(tuned.bridge_v2->successor->entry.mint_caps.max_per_block, 20'000'000'000);
        BOOST_CHECK_EQUAL(tuned.bridge_v2->successor->entry.mint_caps.max_per_epoch, 40'000'000'000);
    }
    // Beside a V1 section the successor applies the same way.
    {
        UniValue with_successor{ValidV2()};
        with_successor.pushKV("successor", ValidSuccessor());
        const auto both_chain{CChainParams::RegTest(FlowMeshOptions(ParseB3RegTestBridgePins(BothPins(with_successor))))};
        const Consensus::Params& both{both_chain->GetConsensus()};
        BOOST_REQUIRE(both.bridge_v2 && both.bridge_v2->successor);
        BOOST_CHECK(*both.busd_bridge->withdrawal_mode == Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1);
        BOOST_CHECK(*modern::ConfiguredBridgeV2RegistryId(both) == *registry);
    }
}

BOOST_AUTO_TEST_CASE(v2_successor_refusals)
{
    BOOST_CHECK_NO_THROW(ParseB3RegTestBridgePins(SuccessorPins()));
    const std::string max_height{std::to_string(std::numeric_limits<int>::max())};

    // Shape: an object holding exactly the successor keys.
    CheckRefused(SuccessorPins(UniValue{}), "v2.successor must be a JSON object");
    CheckRefused(SuccessorPins(UniValue{UniValue::VARR}), "v2.successor must be a JSON object");
    CheckRefused(SuccessorPins(UniValue{UniValue::VOBJ}), "missing key v2.successor.");
    for (const std::string key : {"vault", "predecessor", "asset_id", "migration_height"}) {
        CheckRefused(SuccessorPins(Without(ValidSuccessor(), key)), "missing key v2.successor." + key);
    }
    std::string duplicate{SuccessorPins()};
    duplicate.insert(duplicate.find("\"predecessor\""), "\"predecessor\":\"" + VAULT_V2 + "\",");
    CheckRefused(duplicate, "duplicate key v2.successor.predecessor");

    // Everything else is inherited from v, and v''s code hash commits to
    // blockHash(R_f - 1) (OI-3): none of them is a successor key.
    for (const std::string key : {"token", "origin_chain_id", "vault_runtime_code_hash", "token_runtime_code_hash",
                                  "max_deposit_raw", "max_per_block", "max_per_epoch", "proof_kind",
                                  "origin_decimals", "asset_decimals", "verifier", "activation_height",
                                  "burn_height", "light_client"}) {
        CheckRefused(WithSuccessor(key, Repeat("11", 32)), "unknown key v2.successor." + key);
    }

    // Addresses: 0x hex, non-zero; v' is a contract of its own and v is its
    // predecessor.
    CheckRefused(WithSuccessor("vault", SUCCESSOR_VAULT.substr(2)),
                 "v2.successor.vault must be a 0x-prefixed 20-byte hex string");
    CheckRefused(WithSuccessor("vault", "0x" + std::string(40, 'z')),
                 "v2.successor.vault must be a 0x-prefixed 20-byte hex string");
    CheckRefused(WithSuccessor("vault", uint64_t{1}), "v2.successor.vault must be a 0x-prefixed 20-byte hex string");
    CheckRefused(WithSuccessor("predecessor", Repeat("46", 19)),
                 "v2.successor.predecessor must be a 0x-prefixed 20-byte hex string");
    CheckRefused(WithSuccessor("vault", Repeat("00", 20)), "v2.successor.vault must be non-zero");
    CheckRefused(WithSuccessor("predecessor", Repeat("00", 20)), "v2.successor.predecessor must be non-zero");
    for (const std::string& address : {VAULT_V2, TOKEN, VERIFIER_V2}) {
        CheckRefused(WithSuccessor("vault", address),
                     "v2.successor.vault must differ from v2.vault, v2.token and v2.verifier");
    }
    for (const std::string& address : {TOKEN, VERIFIER_V2, SUCCESSOR_VAULT, VAULT}) {
        CheckRefused(WithSuccessor("predecessor", address),
                     "v2.successor.predecessor must equal v2.vault (the launch vault it retires)");
    }

    // AssetId: 32 raw bytes, non-zero; RegTest() checks the value (below).
    CheckRefused(WithSuccessor("asset_id", Repeat("00", 32)), "v2.successor.asset_id must be non-zero");
    CheckRefused(WithSuccessor("asset_id", Repeat("11", 20)),
                 "v2.successor.asset_id must be a 0x-prefixed 32-byte hex string");
    BOOST_CHECK_NO_THROW(ParseB3RegTestBridgePins(WithSuccessor("asset_id", Repeat("11", 32))));

    // h_m > V, h_m >= W2, h_m <= INT_MAX.
    CheckRefused(WithSuccessor("migration_height", uint64_t{V2_HEIGHT}),
                 "v2.successor.migration_height (200) must be above activation_height (200), "
                 "at or above burn_height (200) and at most " + max_height);
    CheckRefused(WithSuccessor("migration_height", uint64_t{V2_HEIGHT - 1}),
                 "v2.successor.migration_height (199) must be above activation_height (200)");
    CheckRefused(WithSuccessor("migration_height", uint64_t{0}),
                 "v2.successor.migration_height (0) must be above activation_height (200)");
    BOOST_CHECK_EQUAL(ParseB3RegTestBridgePins(WithSuccessor("migration_height", uint64_t{V2_HEIGHT + 1}))
                          .v2->successor->migration_height,
                      V2_HEIGHT + 1);
    {
        UniValue late_burn{ValidV2()};
        late_burn.pushKV("burn_height", uint64_t{V2_HEIGHT + 5});
        CheckRefused(SuccessorPins(ValidSuccessor(V2_HEIGHT + 4), late_burn),
                     "v2.successor.migration_height (204) must be above activation_height (200), "
                     "at or above burn_height (205)");
        BOOST_CHECK_EQUAL(ParseB3RegTestBridgePins(SuccessorPins(ValidSuccessor(V2_HEIGHT + 5), late_burn))
                              .v2->successor->migration_height,
                          V2_HEIGHT + 5);
    }
    CheckRefused(WithSuccessor("migration_height", uint64_t{1} << 31),
                 "v2.successor.migration_height (2147483648) must be above");
    BOOST_CHECK_EQUAL(ParseB3RegTestBridgePins(WithSuccessor("migration_height",
                                                             uint64_t{std::numeric_limits<int>::max()}))
                          .v2->successor->migration_height,
                      std::numeric_limits<int>::max());
    CheckRefused(WithSuccessor("migration_height", uint64_t{1} << 32),
                 "v2.successor.migration_height must be an unsigned integer of at most 32 bits");
    CheckRefused(WithSuccessor("migration_height", int64_t{-1}),
                 "v2.successor.migration_height must be an unsigned integer");
    CheckRefused(WithSuccessor("migration_height", "220"),
                 "v2.successor.migration_height must be an unsigned integer");

    // The optional deployment bound.
    CheckRefused(WithSuccessor("origin_deployment_block", uint64_t{0}),
                 "v2.successor.origin_deployment_block must be at least 1");
    BOOST_CHECK_EQUAL(*ParseB3RegTestBridgePins(WithSuccessor("origin_deployment_block", uint64_t{1}))
                           .v2->successor->origin_deployment_block,
                      1U);

    // A refused successor fails the whole file, also beside a V1 section.
    CheckRefused(BothPins(With(ValidV2(), "successor", Without(ValidSuccessor(), "asset_id"))),
                 "missing key v2.successor.asset_id");

    // Kernel (RegTest()): the AssetId must be v's, recomputed under this
    // chain's domain, in raw EVM order.
    BOOST_CHECK_NO_THROW(CChainParams::RegTest(FlowMeshOptions(ParseB3RegTestBridgePins(SuccessorPins()))));
    const std::string asset_mismatch{
        "-b3bridgepins v2.successor.asset_id must equal the launch vault's bUSD V2 AssetId (B-23)"};
    const auto regtest{[](const std::string& json) {
        return CChainParams::RegTest(FlowMeshOptions(ParseB3RegTestBridgePins(json)));
    }};
    BOOST_CHECK_EXCEPTION(regtest(WithSuccessor("asset_id", Repeat("11", 32))), std::runtime_error,
                          HasReason(asset_mismatch));
    BOOST_CHECK_EXCEPTION(regtest(WithSuccessor("asset_id", "0x" + LaunchAssetIdV2().GetHex())),
                          std::runtime_error, HasReason(asset_mismatch));
    {
        // The id v''s own address would give is not v's.
        const auto chain{regtest(SuccessorPins())};
        const Consensus::Params& params{chain->GetConsensus()};
        const auto domain{modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)};
        BOOST_REQUIRE(domain);
        const uint256 own{modern::BridgeAssetIdV2(*domain, params.bridge_v2->successor->entry)};
        BOOST_CHECK(own != LaunchAssetIdV2());
        BOOST_CHECK_EXCEPTION(regtest(WithSuccessor("asset_id", EvmHex(own))), std::runtime_error,
                              HasReason(asset_mismatch));
    }
    {
        // Another launch vault has another AssetId: v's id from one file
        // does not carry over to a file whose v (and predecessor) differ.
        const std::string other_vault{Repeat("47", 20)};
        UniValue other_v2{ValidV2()};
        other_v2.pushKV("vault", other_vault);
        const uint256 other_asset{LaunchAssetIdOf(V2Root(other_v2).write())};
        BOOST_CHECK(other_asset != LaunchAssetIdV2());
        UniValue other_successor{With(ValidSuccessor(), "predecessor", other_vault)};
        BOOST_CHECK_EXCEPTION(regtest(SuccessorPins(other_successor, other_v2)), std::runtime_error,
                              HasReason(asset_mismatch));
        BOOST_CHECK_NO_THROW(regtest(SuccessorPins(With(other_successor, "asset_id", EvmHex(other_asset)), other_v2)));
    }

    // Kernel: v' must not sit on any V1 bridge contract.
    const std::string v1_overlap{"-b3bridgepins v2.successor.vault must differ from every V1 bridge contract"};
    for (const std::string& address : {VAULT, VERIFIER}) {
        UniValue v2{ValidV2()};
        v2.pushKV("successor", With(ValidSuccessor(), "vault", address));
        BOOST_CHECK_EXCEPTION(regtest(BothPins(v2)), std::runtime_error, HasReason(v1_overlap));
        // Without the V1 section those addresses are not bridge contracts.
        BOOST_CHECK_NO_THROW(regtest(SuccessorPins(With(ValidSuccessor(), "vault", address))));
    }
    {
        // The V1 token, when the v2 section uses a token of its own.
        UniValue v2{ValidV2()};
        v2.pushKV("token", TOKEN_V2);
        const uint256 asset{LaunchAssetIdOf(V2Root(v2).write())};
        UniValue on_token{v2};
        on_token.pushKV("successor", With(With(ValidSuccessor(), "vault", TOKEN), "asset_id", EvmHex(asset)));
        BOOST_CHECK_EXCEPTION(regtest(BothPins(on_token)), std::runtime_error, HasReason(v1_overlap));
        v2.pushKV("successor", With(ValidSuccessor(), "asset_id", EvmHex(asset)));
        BOOST_CHECK_NO_THROW(regtest(BothPins(v2)));
    }
    // The synthetic MANAGED_V1 bridge's vault and authority.
    for (const std::string& address : {"0x" + HexStr(Consensus::ETHEREUM_MAINNET_BUSD_IDENTITY.vault_address),
                                       Repeat("42", 20)}) {
        BOOST_CHECK_EXCEPTION(regtest(WithSuccessor("vault", address)), std::runtime_error, HasReason(v1_overlap));
    }

    // Hand-built sections reach the kernel's own checks.
    const auto pins{ParseB3RegTestBridgePins(SuccessorPins())};
    {
        auto at_v{pins};
        at_v.v2->successor->migration_height = V2_HEIGHT;
        BOOST_CHECK_EXCEPTION(CChainParams::RegTest(FlowMeshOptions(at_v)), std::runtime_error,
                              HasReason("-b3bridgepins v2.successor.migration_height (200) must be above "
                                        "v2.activation_height (200) and at or above v2.burn_height (200)"));
        auto below_w2{pins};
        below_w2.v2->burn_height = V2_HEIGHT + 5;
        below_w2.v2->successor->migration_height = V2_HEIGHT + 4;
        BOOST_CHECK_EXCEPTION(CChainParams::RegTest(FlowMeshOptions(below_w2)), std::runtime_error,
                              HasReason("-b3bridgepins v2.successor.migration_height (204) must be above "
                                        "v2.activation_height (200) and at or above v2.burn_height (205)"));
        auto zero_asset{pins};
        zero_asset.v2->successor->asset_id = uint256{};
        BOOST_CHECK_EXCEPTION(CChainParams::RegTest(FlowMeshOptions(zero_asset)), std::runtime_error,
                              HasReason(asset_mismatch));
    }
    const std::string incomplete{"-b3bridgepins v2 does not form a complete bridge V2 configuration"};
    {
        auto wrong_predecessor{pins};
        wrong_predecessor.v2->successor->predecessor = pins.v2->token;
        BOOST_CHECK_EXCEPTION(CChainParams::RegTest(FlowMeshOptions(wrong_predecessor)), std::runtime_error,
                              HasReason(incomplete));
        for (const Consensus::BridgeEthAddress& address : {pins.v2->vault, pins.v2->token, pins.v2->verifier}) {
            auto reused{pins};
            reused.v2->successor->vault = address;
            BOOST_CHECK_EXCEPTION(CChainParams::RegTest(FlowMeshOptions(reused)), std::runtime_error,
                                  HasReason(incomplete));
        }
        auto zero_vault{pins};
        zero_vault.v2->successor->vault = Consensus::BridgeEthAddress{};
        BOOST_CHECK_EXCEPTION(CChainParams::RegTest(FlowMeshOptions(zero_vault)), std::runtime_error,
                              HasReason(incomplete));
        auto zero_deployment{pins};
        zero_deployment.v2->successor->origin_deployment_block = 0;
        BOOST_CHECK_EXCEPTION(CChainParams::RegTest(FlowMeshOptions(zero_deployment)), std::runtime_error,
                              HasReason(incomplete));
    }
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

    // A file carrying a B-23 successor is refused the same way, so no
    // migration can activate anywhere but FlowMesh regtest.
    const std::string successor_path{WriteFile("v2_successor.json", SuccessorPins())};
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET,
                                  ChainType::TESTNET4, ChainType::SIGNET}) {
        ArgsManager args;
        SetArgs(args, successor_path);
        args.ForceSetArg("-b3freshlineageheight", std::to_string(FRESH_HEIGHT));
        BOOST_CHECK_EXCEPTION(CreateChainParams(args, chain), std::runtime_error,
                              HasReason("-b3bridgepins is regtest-only"));
        ArgsManager bare;
        bare.ForceSetArg("-b3bridgepins", successor_path);
        BOOST_CHECK_EXCEPTION(CreateChainParams(bare, chain), std::runtime_error,
                              HasReason("-b3bridgepins is regtest-only"));
        const auto params{CreateChainParams(ArgsManager{}, chain)};
        BOOST_CHECK(!params->GetConsensus().BridgeV2MigrationActive(std::numeric_limits<int>::max()));
    }
    for (const auto& [modern, flowmesh] : {std::pair{false, false}, std::pair{true, false},
                                           std::pair{false, true}}) {
        ArgsManager args;
        SetArgs(args, successor_path, modern, flowmesh);
        args.ForceSetArg("-b3freshlineageheight", std::to_string(FRESH_HEIGHT));
        BOOST_CHECK_EXCEPTION(CreateChainParams(args, ChainType::REGTEST), std::runtime_error,
                              HasReason("-b3bridgepins requires -b3modernregtest and -b3flowmeshtest (regtest-only)"));
    }
    BOOST_CHECK(!CreateChainParams(ArgsManager{}, ChainType::REGTEST)
                     ->GetConsensus()
                     .BridgeV2MigrationActive(std::numeric_limits<int>::max()));
    ArgsManager successor_no_fresh;
    SetArgs(successor_no_fresh, successor_path);
    BOOST_CHECK_EXCEPTION(CreateChainParams(successor_no_fresh, ChainType::REGTEST), std::runtime_error,
                          HasReason("-b3bridgepins v2 requires -b3freshlineageheight"));
}

// ------------------------- forwarder pins (node changes 3.1; routes b, d3)

namespace {

// Stand-ins: anvil's default deployer at nonces 6 and 7 (DeployV2 places the
// factories at its nonces n+4 and n+5) and an asymmetric init-code hash, so
// the raw EVM byte order is visible.
const std::string DEPOSIT_FACTORY{"0x0165878a594ca255338adfa4d48449f69242eb8f"};
const std::string EXIT_FACTORY{"0xa513e6e4b8f2a923d98304ec87f64353c4d5c853"};
const std::string FORWARDER_INIT_CODE_HASH{"0xea331de4dceed5e35c277c300018c413571f89f3d116bdbf57e8c5fc1c8ee43c"};
constexpr uint64_t ARBITRUM_EID{30'110};

UniValue ValidDepositForwarder()
{
    UniValue forwarder{UniValue::VOBJ};
    forwarder.pushKV("factory", DEPOSIT_FACTORY);
    forwarder.pushKV("init_code_hash", FORWARDER_INIT_CODE_HASH);
    return forwarder;
}

UniValue ValidExitForwarder()
{
    UniValue forwarder{UniValue::VOBJ};
    forwarder.pushKV("factory", EXIT_FACTORY);
    forwarder.pushKV("init_code_hash", FORWARDER_INIT_CODE_HASH);
    forwarder.pushKV("dst_eid", ARBITRUM_EID);
    return forwarder;
}

//! `v2` (default ValidV2()) with both forwarder objects.
UniValue ForwarderV2(const UniValue& deposit = ValidDepositForwarder(),
                     const UniValue& exit = ValidExitForwarder(), UniValue v2 = ValidV2())
{
    v2.pushKV("deposit_forwarder", deposit);
    v2.pushKV("exit_forwarder", exit);
    return v2;
}

//! A v2-only file with both forwarder objects.
std::string ForwarderPins(const UniValue& deposit = ValidDepositForwarder(),
                          const UniValue& exit = ValidExitForwarder())
{
    return V2Root(ForwarderV2(deposit, exit)).write();
}

//! A v2-only file whose v2.<section> is `value`, without the other forwarder.
std::string OneForwarderPins(const std::string& section, const UniValue& value)
{
    return V2Root(With(ValidV2(), section, value)).write();
}

std::string WithDepositForwarder(const std::string& key, const UniValue& value)
{
    return OneForwarderPins("deposit_forwarder", With(ValidDepositForwarder(), key, value));
}

std::string WithExitForwarder(const std::string& key, const UniValue& value)
{
    return OneForwarderPins("exit_forwarder", With(ValidExitForwarder(), key, value));
}

} // namespace

BOOST_FIXTURE_TEST_CASE(v2_forwarders_apply, BridgePinsSetup)
{
    // Parsed fields; the init-code hash keeps raw EVM byte order.
    const std::string contents{ForwarderPins()};
    const auto parsed{ParseB3RegTestBridgePins(contents)};
    BOOST_REQUIRE(parsed.v2 && parsed.v2->deposit_forwarder && parsed.v2->exit_forwarder);
    const auto& deposit{*parsed.v2->deposit_forwarder};
    const auto& exit_pins{*parsed.v2->exit_forwarder};
    BOOST_CHECK_EQUAL("0x" + HexStr(deposit.factory), DEPOSIT_FACTORY);
    BOOST_CHECK_EQUAL(EvmHex(deposit.init_code_hash), FORWARDER_INIT_CODE_HASH);
    BOOST_CHECK_NE("0x" + deposit.init_code_hash.GetHex(), FORWARDER_INIT_CODE_HASH);
    BOOST_CHECK_EQUAL("0x" + HexStr(exit_pins.factory), EXIT_FACTORY);
    BOOST_CHECK_EQUAL(EvmHex(exit_pins.init_code_hash), FORWARDER_INIT_CODE_HASH);
    BOOST_CHECK_EQUAL(exit_pins.dst_eid, ARBITRUM_EID);
    BOOST_CHECK(!parsed.v2->successor);

    // Both keys are optional and independent.
    {
        const auto none{ParseB3RegTestBridgePins(ValidV2Pins())};
        BOOST_CHECK(!none.v2->deposit_forwarder && !none.v2->exit_forwarder);
        const auto deposit_only{ParseB3RegTestBridgePins(OneForwarderPins("deposit_forwarder", ValidDepositForwarder()))};
        BOOST_CHECK(deposit_only.v2->deposit_forwarder == deposit && !deposit_only.v2->exit_forwarder);
        const auto exit_only{ParseB3RegTestBridgePins(OneForwarderPins("exit_forwarder", ValidExitForwarder()))};
        BOOST_CHECK(!exit_only.v2->deposit_forwarder && exit_only.v2->exit_forwarder == exit_pins);
        // Any non-zero dst_eid, and one init-code hash per factory.
        BOOST_CHECK_EQUAL(ParseB3RegTestBridgePins(WithExitForwarder("dst_eid", uint64_t{30'101}))
                              .v2->exit_forwarder->dst_eid,
                          30'101U);
        BOOST_CHECK_EQUAL(ParseB3RegTestBridgePins(WithExitForwarder("dst_eid", uint64_t{std::numeric_limits<uint32_t>::max()}))
                              .v2->exit_forwarder->dst_eid,
                          std::numeric_limits<uint32_t>::max());
        BOOST_CHECK_NO_THROW(ParseB3RegTestBridgePins(ForwarderPins(
            ValidDepositForwarder(), With(ValidExitForwarder(), "init_code_hash", Repeat("f8", 32)))));
    }

    // The launch-only chain these pins extend.
    ArgsManager launch_args;
    SetArgs(launch_args, WriteFile("v2_launch.json", ValidV2Pins()));
    launch_args.ForceSetArg("-b3freshlineageheight", std::to_string(FRESH_HEIGHT));
    const auto launch_chain{CreateChainParams(launch_args, ChainType::REGTEST)};
    const Consensus::Params& launch{launch_chain->GetConsensus()};
    BOOST_REQUIRE(launch.bridge_v2);
    BOOST_CHECK(!launch.bridge_v2->deposit_forwarder && !launch.bridge_v2->exit_forwarder);

    ArgsManager args;
    SetArgs(args, WriteFile("v2_forwarders.json", contents));
    args.ForceSetArg("-b3freshlineageheight", std::to_string(FRESH_HEIGHT));
    const auto chain{CreateChainParams(args, ChainType::REGTEST)};
    const Consensus::Params& params{chain->GetConsensus()};

    // Pinned before first start: the record guard hashes the whole file,
    // forwarders included, so they freeze with v from the first block >= A3.
    BOOST_REQUIRE(chain->B3BridgePinsHash());
    BOOST_CHECK(*chain->B3BridgePinsHash() == Sha256(contents));
    BOOST_CHECK(*chain->B3BridgePinsHash() != *launch_chain->B3BridgePinsHash());

    BOOST_REQUIRE(params.bridge_v2);
    const Consensus::BridgeV2Params& v2{*params.bridge_v2};
    BOOST_CHECK(v2.Valid());
    BOOST_CHECK(v2.ForwardersValid());
    BOOST_REQUIRE(v2.deposit_forwarder && v2.exit_forwarder);
    BOOST_CHECK(*v2.deposit_forwarder == deposit);
    BOOST_CHECK(*v2.exit_forwarder == exit_pins);

    // The launch envelope is exactly the launch-only file's.
    BOOST_CHECK_EQUAL(v2.activation_height, launch.bridge_v2->activation_height);
    BOOST_CHECK(v2.vault.vault_address == launch.bridge_v2->vault.vault_address);
    BOOST_CHECK(v2.verifier.address == launch.bridge_v2->verifier.address);
    BOOST_CHECK(v2.gate == launch.bridge_v2->gate);
    BOOST_CHECK(!v2.successor);

    // Identities: the AssetId and the entry id records name are unchanged;
    // the registry id commits to each forwarder pin.
    BOOST_CHECK(*modern::ConfiguredBridgeV2AssetId(params) == *modern::ConfiguredBridgeV2AssetId(launch));
    BOOST_CHECK(*modern::ConfiguredBridgeV2EntryId(params) == *modern::ConfiguredBridgeV2EntryId(launch));
    const auto registry{modern::ConfiguredBridgeV2RegistryId(params)};
    const auto launch_registry{modern::ConfiguredBridgeV2RegistryId(launch)};
    BOOST_REQUIRE(registry && launch_registry);
    BOOST_CHECK(*registry != *launch_registry);
    {
        const auto regtest{[](const std::string& json) {
            return CChainParams::RegTest(FlowMeshOptions(ParseB3RegTestBridgePins(json)));
        }};
        const auto deposit_only{regtest(OneForwarderPins("deposit_forwarder", ValidDepositForwarder()))};
        const auto exit_only{regtest(OneForwarderPins("exit_forwarder", ValidExitForwarder()))};
        const auto other_eid{regtest(ForwarderPins(ValidDepositForwarder(),
                                                   With(ValidExitForwarder(), "dst_eid", uint64_t{30'101})))};
        const auto deposit_registry{modern::ConfiguredBridgeV2RegistryId(deposit_only->GetConsensus())};
        const auto exit_registry{modern::ConfiguredBridgeV2RegistryId(exit_only->GetConsensus())};
        const auto other_eid_registry{modern::ConfiguredBridgeV2RegistryId(other_eid->GetConsensus())};
        BOOST_REQUIRE(deposit_registry && exit_registry && other_eid_registry);
        for (const uint256& id : {*deposit_registry, *exit_registry, *other_eid_registry}) {
            BOOST_CHECK(id != *launch_registry);
            BOOST_CHECK(id != *registry);
        }
        BOOST_CHECK(*deposit_registry != *exit_registry);
        BOOST_CHECK(*exit_only->GetConsensus().bridge_v2->exit_forwarder == exit_pins);
        BOOST_CHECK(!exit_only->GetConsensus().bridge_v2->deposit_forwarder);
    }

    // Beside a successor, and beside a V1 section, the forwarders apply the
    // same way.
    {
        UniValue with_successor{ForwarderV2()};
        with_successor.pushKV("successor", ValidSuccessor());
        const auto successor_chain{CChainParams::RegTest(
            FlowMeshOptions(ParseB3RegTestBridgePins(V2Root(with_successor).write())))};
        const Consensus::Params& both{successor_chain->GetConsensus()};
        BOOST_REQUIRE(both.bridge_v2 && both.bridge_v2->successor);
        BOOST_CHECK(*both.bridge_v2->deposit_forwarder == deposit);
        BOOST_CHECK(*both.bridge_v2->exit_forwarder == exit_pins);
        BOOST_CHECK(both.bridge_v2->Valid());
        const auto successor_only{CChainParams::RegTest(FlowMeshOptions(ParseB3RegTestBridgePins(SuccessorPins())))};
        BOOST_CHECK(*modern::ConfiguredBridgeV2RegistryId(both) !=
                    *modern::ConfiguredBridgeV2RegistryId(successor_only->GetConsensus()));

        const auto v1_chain{CChainParams::RegTest(FlowMeshOptions(ParseB3RegTestBridgePins(BothPins(ForwarderV2()))))};
        const Consensus::Params& with_v1{v1_chain->GetConsensus()};
        BOOST_REQUIRE(with_v1.bridge_v2);
        BOOST_CHECK(*with_v1.busd_bridge->withdrawal_mode == Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1);
        BOOST_CHECK(*modern::ConfiguredBridgeV2RegistryId(with_v1) == *registry);
    }
}

BOOST_AUTO_TEST_CASE(v2_forwarder_refusals)
{
    BOOST_CHECK_NO_THROW(ParseB3RegTestBridgePins(ForwarderPins()));
    const std::vector<std::pair<std::string, UniValue>> sections{
        {"deposit_forwarder", ValidDepositForwarder()},
        {"exit_forwarder", ValidExitForwarder()},
    };
    for (const auto& entry : sections) {
        const std::string& section{entry.first};
        const UniValue& valid{entry.second};
        const std::string where{"v2." + section};
        const auto with_key{[&](const std::string& key, const UniValue& value) {
            return OneForwarderPins(section, With(valid, key, value));
        }};
        BOOST_TEST_CONTEXT(where)
        {
            // Shape: an object holding exactly the section's keys; JSON null
            // is not "absent".
            CheckRefused(OneForwarderPins(section, UniValue{}), where + " must be a JSON object");
            CheckRefused(OneForwarderPins(section, UniValue{UniValue::VARR}), where + " must be a JSON object");
            CheckRefused(OneForwarderPins(section, UniValue{"0x00"}), where + " must be a JSON object");
            CheckRefused(OneForwarderPins(section, UniValue{UniValue::VOBJ}), "missing key " + where + ".");
            for (const std::string& key : valid.getKeys()) {
                CheckRefused(OneForwarderPins(section, Without(valid, key)), "missing key " + where + "." + key);
            }
            // The tag is a protocol constant; accounts, salts and the vault
            // are derived, never pinned.
            for (const std::string key : {"tag", "vault", "token", "account", "salt", "refund_eoa", "to",
                                          "max_bridge_fee_bps", "fee_cap", "not_after", "code_hash"}) {
                CheckRefused(with_key(key, Repeat("11", 32)), "unknown key " + where + "." + key);
            }
            std::string duplicate{OneForwarderPins(section, valid)};
            duplicate.insert(duplicate.find("\"factory\""), "\"factory\":\"" + DEPOSIT_FACTORY + "\",");
            CheckRefused(duplicate, "duplicate key " + where + ".factory");

            // The factory: 0x hex, non-zero, a contract of its own.
            const std::string bad_factory{where + ".factory must be a 0x-prefixed 20-byte hex string"};
            CheckRefused(with_key("factory", DEPOSIT_FACTORY.substr(2)), bad_factory);
            CheckRefused(with_key("factory", Repeat("71", 19)), bad_factory);
            CheckRefused(with_key("factory", Repeat("71", 32)), bad_factory);
            CheckRefused(with_key("factory", "0x" + std::string(40, 'z')), bad_factory);
            CheckRefused(with_key("factory", uint64_t{1}), bad_factory);
            CheckRefused(with_key("factory", Repeat("00", 20)), where + ".factory must be non-zero");
            const std::string on_contract{where + ".factory must differ from v2.vault, v2.token, v2.verifier "
                                                  "and v2.successor.vault"};
            for (const std::string& address : {VAULT_V2, TOKEN, VERIFIER_V2}) {
                CheckRefused(with_key("factory", address), on_contract);
            }
            {
                UniValue v2{ValidV2()};
                v2.pushKV("successor", ValidSuccessor());
                v2.pushKV(section, With(valid, "factory", SUCCESSOR_VAULT));
                CheckRefused(V2Root(v2).write(), on_contract);
                // Without a successor that address is any other contract.
                BOOST_CHECK_NO_THROW(ParseB3RegTestBridgePins(with_key("factory", SUCCESSOR_VAULT)));
            }

            // The init-code hash: 32 raw bytes, non-zero.
            CheckRefused(with_key("init_code_hash", Repeat("00", 32)), where + ".init_code_hash must be non-zero");
            CheckRefused(with_key("init_code_hash", Repeat("f7", 20)),
                         where + ".init_code_hash must be a 0x-prefixed 32-byte hex string");
            CheckRefused(with_key("init_code_hash", uint64_t{1}),
                         where + ".init_code_hash must be a 0x-prefixed 32-byte hex string");

            // A refused forwarder fails the whole file, also beside a V1
            // section.
            CheckRefused(BothPins(With(ValidV2(), section, Without(valid, "factory"))),
                         "missing key " + where + ".factory");
        }
    }

    // dst_eid: an unsigned 32-bit LayerZero endpoint id, non-zero.
    CheckRefused(WithExitForwarder("dst_eid", uint64_t{0}), "v2.exit_forwarder.dst_eid must be non-zero");
    CheckRefused(WithExitForwarder("dst_eid", uint64_t{1} << 32),
                 "v2.exit_forwarder.dst_eid must be an unsigned integer of at most 32 bits");
    CheckRefused(WithExitForwarder("dst_eid", int64_t{-1}), "v2.exit_forwarder.dst_eid must be an unsigned integer");
    CheckRefused(WithExitForwarder("dst_eid", "30110"), "v2.exit_forwarder.dst_eid must be an unsigned integer");
    // The deposit factory takes no dst_eid.
    CheckRefused(WithDepositForwarder("dst_eid", ARBITRUM_EID), "unknown key v2.deposit_forwarder.dst_eid");

    // One factory per route.
    CheckRefused(ForwarderPins(ValidDepositForwarder(), With(ValidExitForwarder(), "factory", DEPOSIT_FACTORY)),
                 "v2.deposit_forwarder.factory and v2.exit_forwarder.factory must differ");

    // Kernel (RegTest()): neither factory may sit on a V1 bridge contract.
    const auto regtest{[](const std::string& json) {
        return CChainParams::RegTest(FlowMeshOptions(ParseB3RegTestBridgePins(json)));
    }};
    BOOST_CHECK_NO_THROW(regtest(ForwarderPins()));
    BOOST_CHECK_NO_THROW(regtest(BothPins(ForwarderV2())));
    const std::string v1_overlap{"-b3bridgepins v2 forwarder factories must differ from every V1 bridge contract"};
    for (const std::string& address : {VAULT, TOKEN, VERIFIER}) {
        // The v2 section uses a token of its own, so the V1 token is only a
        // V1 contract.
        UniValue v2{ValidV2()};
        v2.pushKV("token", TOKEN_V2);
        BOOST_CHECK_EXCEPTION(regtest(BothPins(ForwarderV2(With(ValidDepositForwarder(), "factory", address),
                                                           ValidExitForwarder(), v2))),
                              std::runtime_error, HasReason(v1_overlap));
        BOOST_CHECK_EXCEPTION(regtest(BothPins(ForwarderV2(ValidDepositForwarder(),
                                                           With(ValidExitForwarder(), "factory", address), v2))),
                              std::runtime_error, HasReason(v1_overlap));
    }
    // Without the V1 section the V1 vault and verifier are not bridge
    // contracts (the synthetic MANAGED_V1 bridge's vault and authority are).
    BOOST_CHECK_NO_THROW(regtest(ForwarderPins(With(ValidDepositForwarder(), "factory", VAULT),
                                               With(ValidExitForwarder(), "factory", VERIFIER))));
    for (const std::string& address : {"0x" + HexStr(Consensus::ETHEREUM_MAINNET_BUSD_IDENTITY.vault_address),
                                       Repeat("42", 20)}) {
        BOOST_CHECK_EXCEPTION(regtest(ForwarderPins(With(ValidDepositForwarder(), "factory", address))),
                              std::runtime_error, HasReason(v1_overlap));
        BOOST_CHECK_EXCEPTION(regtest(ForwarderPins(ValidDepositForwarder(),
                                                    With(ValidExitForwarder(), "factory", address))),
                              std::runtime_error, HasReason(v1_overlap));
    }

    // Hand-built sections reach BridgeV2Params::Valid().
    const std::string incomplete{"-b3bridgepins v2 does not form a complete bridge V2 configuration"};
    const auto pins{ParseB3RegTestBridgePins(ForwarderPins())};
    using Mutation = std::function<void(CChainParams::B3RegTestBridgePins::V2Section&)>;
    const std::vector<Mutation> refused{
        [](auto& v2) { v2.deposit_forwarder->factory = {}; },
        [](auto& v2) { v2.deposit_forwarder->init_code_hash = uint256{}; },
        [](auto& v2) { v2.exit_forwarder->factory = {}; },
        [](auto& v2) { v2.exit_forwarder->init_code_hash = uint256{}; },
        [](auto& v2) { v2.exit_forwarder->dst_eid = 0; },
        [](auto& v2) { v2.deposit_forwarder->factory = v2.vault; },
        [](auto& v2) { v2.deposit_forwarder->factory = v2.token; },
        [](auto& v2) { v2.exit_forwarder->factory = v2.verifier; },
        [](auto& v2) { v2.exit_forwarder->factory = v2.deposit_forwarder->factory; },
    };
    for (size_t i{0}; i < refused.size(); ++i) {
        auto hand_built{pins};
        refused[i](*hand_built.v2);
        BOOST_CHECK_EXCEPTION(CChainParams::RegTest(FlowMeshOptions(hand_built)), std::runtime_error,
                              HasReason(incomplete));
    }
    {
        // A hand-built factory on a pinned successor vault.
        auto on_successor{ParseB3RegTestBridgePins(V2Root(With(ForwarderV2(), "successor", ValidSuccessor())).write())};
        on_successor.v2->exit_forwarder->factory = on_successor.v2->successor->vault;
        BOOST_CHECK_EXCEPTION(CChainParams::RegTest(FlowMeshOptions(on_successor)), std::runtime_error,
                              HasReason(incomplete));
    }
}

BOOST_FIXTURE_TEST_CASE(v2_forwarder_pins_refused_off_regtest, BridgePinsSetup)
{
    // A pin file carrying forwarder pins is refused off FlowMesh regtest like
    // any other, so no network but FlowMesh regtest ever derives a forwarder
    // or checks an exit block against one.
    const std::string path{WriteFile("v2_forwarders.json", ForwarderPins())};
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
        const auto params{CreateChainParams(ArgsManager{}, chain)};
        BOOST_CHECK(!params->GetConsensus().bridge_v2);
    }
    for (const auto& [modern, flowmesh] : {std::pair{false, false}, std::pair{true, false},
                                           std::pair{false, true}}) {
        ArgsManager args;
        SetArgs(args, path, modern, flowmesh);
        args.ForceSetArg("-b3freshlineageheight", std::to_string(FRESH_HEIGHT));
        BOOST_CHECK_EXCEPTION(CreateChainParams(args, ChainType::REGTEST), std::runtime_error,
                              HasReason("-b3bridgepins requires -b3modernregtest and -b3flowmeshtest (regtest-only)"));
    }
    BOOST_CHECK(!CreateChainParams(ArgsManager{}, ChainType::REGTEST)->GetConsensus().bridge_v2);
    ArgsManager no_fresh;
    SetArgs(no_fresh, path);
    BOOST_CHECK_EXCEPTION(CreateChainParams(no_fresh, ChainType::REGTEST), std::runtime_error,
                          HasReason("-b3bridgepins v2 requires -b3freshlineageheight"));
}

BOOST_AUTO_TEST_SUITE_END()
