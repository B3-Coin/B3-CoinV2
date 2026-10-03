// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bridge/proof.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <common/args.h>
#include <consensus/bridge_params.h>
#include <crypto/sha256.h>
#include <modern/bridge_asset.h>
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
#include <string>
#include <string_view>

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

    UniValue v2_root{ValidRoot()};
    v2_root.pushKV("v2", UniValue{UniValue::VOBJ});
    CheckRefused(v2_root.write(), "unknown key file.v2");
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
    CheckRefused(Without(ValidRoot(), "v1_decentralized").write(), "missing key file.v1_decentralized");

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

BOOST_AUTO_TEST_SUITE_END()
