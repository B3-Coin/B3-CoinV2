// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bridge/admission.h>
#include <consensus/bridge_params.h>
#include <consensus/era.h>
#include <kernel/chainparams.h>
#include <modern/asset_validation.h>
#include <modern/bridge_asset.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>

using namespace bridge;

BOOST_AUTO_TEST_SUITE(bridge_params_tests)

namespace {

constexpr uint256 Hash(const uint8_t value) { return uint256{value}; }

Consensus::BridgeAssetParams CompleteBridgeParams()
{
    Consensus::BridgeAssetParams out;
    out.asset = Consensus::ETHEREUM_MAINNET_BUSD_IDENTITY;
    out.origin_deployment_block = 123;
    out.vault_runtime_code_hash = Hash(6);
    out.implementation_or_adapter = Hash(7);
    out.adapter_version =
        Consensus::BRIDGE_ADAPTER_VERSION_DIRECT_TOKEN_V1;
    out.recipient_encoding_version =
        Consensus::BRIDGE_RECIPIENT_VERSION_P2PKH_V1;
    out.activation_height = 500;
    out.approval_last_height = 1'000;
    out.mint_caps = Consensus::BridgeMintCaps{
        .max_per_block = 5'000'000,
        .max_per_epoch = 20'000'000,
        .epoch_length_blocks = 30,
    };

    Consensus::EthereumLightClientPins light;
    light.trusted_checkpoint_root = Hash(8);
    light.trusted_checkpoint_slot = 8'192;
    light.genesis_validators_root =
        Consensus::ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT;
    light.fork_schedule = {
        {0, {0x00, 0x00, 0x00, 0x00}},
        {10, {0x01, 0x02, 0x03, 0x04}},
    };
    light.fork_schedule_valid_through_epoch = 256;
    light.electra_epoch = 10;
    light.min_sync_committee_participants =
        Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY;
    light.max_sync_lag_slots = 65'536;
    out.light_client = light;

    out.withdrawal_mode = Consensus::BridgeWithdrawalMode::MANAGED_V1;
    Consensus::BridgeManagedWithdrawalPins withdrawal;
    withdrawal.authority_address.fill(0x55);
    withdrawal.vault_runtime_code_hash = *out.vault_runtime_code_hash;
    withdrawal.withdrawal_rules_version =
        Consensus::MANAGED_WITHDRAWAL_RULES_VERSION_V1;
    withdrawal.withdrawal_rules_commitment = Hash(11);
    out.managed_withdrawal = withdrawal;
    return out;
}

Consensus::BridgeDecentralizedWithdrawalPins DecentralizedWithdrawalPins()
{
    Consensus::BridgeDecentralizedWithdrawalPins out;
    out.ethereum_verifier_address.fill(0x66);
    out.ethereum_verifier_code_hash = Hash(12);
    out.bootstrap_validator_set_hash = Hash(13);
    out.withdrawal_rules_version =
        Consensus::DECENTRALIZED_WITHDRAWAL_RULES_VERSION_V1;
    out.withdrawal_rules_commitment =
        Consensus::DECENTRALIZED_WITHDRAWAL_RULES_COMMITMENT_V1;
    out.min_bridge_validators = 4;
    out.max_bridge_validators = 64;
    out.min_bridge_total_weight = 1'000'000;
    out.max_epoch_lag = 8;
    return out;
}

Consensus::Params CompleteConsensusParams()
{
    Consensus::Params out;
    out.hashGenesisBlock = Hash(1);
    out.legacy_final_hash = Hash(2);
    out.busd_bridge = CompleteBridgeParams();
    out.bridge_withdrawal_activation_height = 500;
    return out;
}

std::array<unsigned char, 32> RawAmount(uint64_t value)
{
    std::array<unsigned char, 32> out{};
    for (int i{31}; i >= 24; --i) {
        out[i] = static_cast<unsigned char>(value & 0xff);
        value >>= 8;
    }
    return out;
}

ProvenBridgeDeposit MatchingDeposit(const Consensus::BridgeAssetParams& params,
                                    const uint64_t amount)
{
    ProvenBridgeDeposit deposit;
    deposit.origin_chain_id = params.asset.origin_chain_id;
    deposit.execution_block_number = *params.origin_deployment_block;
    deposit.vault_address = params.asset.vault_address;
    deposit.event.deposit_id = 42;
    deposit.event.token = params.asset.token_address;
    deposit.event.amount = RawAmount(amount);
    RecipientV1 recipient;
    recipient.pubkey_hash.fill(0x33);
    deposit.event.b3_recipient = EncodeRecipientV1(recipient);
    return deposit;
}

} // namespace

BOOST_AUTO_TEST_CASE(mainnet_bridge_pins_are_complete_and_literal)
{
    const auto chain{CChainParams::Main()};
    const Consensus::Params& params{chain->GetConsensus()};
    BOOST_REQUIRE(params.busd_bridge);
    const auto& busd{*params.busd_bridge};

    BOOST_CHECK(busd.asset == Consensus::ETHEREUM_MAINNET_BUSD_IDENTITY);
    BOOST_CHECK_EQUAL(busd.asset.origin_chain_id, 1U);
    BOOST_CHECK_EQUAL(HexStr(busd.asset.vault_address),
                      "077839b12cebfbf163acaeac3a59a015d100c64b");
    BOOST_CHECK_EQUAL(HexStr(busd.asset.token_address),
                      "dac17f958d2ee523a2206206994597c13d831ec7");
    BOOST_CHECK_EQUAL(busd.asset.origin_decimals, 6U);
    BOOST_CHECK_EQUAL(busd.asset.asset_decimals, 6U);
    BOOST_REQUIRE(busd.origin_deployment_block);
    BOOST_CHECK_EQUAL(*busd.origin_deployment_block, 25'898'729U);
    BOOST_REQUIRE(busd.vault_runtime_code_hash);
    BOOST_CHECK_EQUAL(HexStr(std::span<const unsigned char>{
                          busd.vault_runtime_code_hash->begin(), 32}),
                      "db267712887568bffd394e46538bddba01da11cefc38e32b2428c00911237f8d");
    BOOST_REQUIRE(busd.implementation_or_adapter);
    BOOST_CHECK_EQUAL(HexStr(std::span<const unsigned char>{
                          busd.implementation_or_adapter->begin(), 32}),
                      "b44fb4e949d0f78f87f79ee46428f23a2a5713ce6fc6e0beb3dda78c2ac1ea55");
    BOOST_REQUIRE(busd.adapter_version);
    BOOST_CHECK_EQUAL(*busd.adapter_version, 1U);
    BOOST_REQUIRE(busd.recipient_encoding_version);
    BOOST_CHECK_EQUAL(*busd.recipient_encoding_version, 1U);
    BOOST_REQUIRE(busd.activation_height);
    BOOST_CHECK_EQUAL(*busd.activation_height, 811'001);
    BOOST_CHECK(!busd.approval_last_height);
    BOOST_REQUIRE(busd.mint_caps);
    BOOST_CHECK_EQUAL(busd.mint_caps->max_per_block, 10'000'000'000);
    BOOST_CHECK_EQUAL(busd.mint_caps->max_per_epoch, 10'000'000'000);
    BOOST_CHECK_EQUAL(busd.mint_caps->epoch_length_blocks, 1'440U);

    BOOST_REQUIRE(busd.light_client);
    const auto& light{*busd.light_client};
    BOOST_CHECK_EQUAL(HexStr(std::span<const unsigned char>{
                          light.trusted_checkpoint_root.begin(), 32}),
                      "f6744774a1bcfe910c643e447cd09fe8443cc2edc25d9ae65155b3cbbef3b646");
    BOOST_CHECK_EQUAL(light.trusted_checkpoint_slot, 15'136'512U);
    BOOST_CHECK(light.genesis_validators_root ==
                Consensus::ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT);
    const std::vector<Consensus::EthereumForkVersionPin> expected_forks{
        {0, {0x00, 0x00, 0x00, 0x00}},
        {74'240, {0x01, 0x00, 0x00, 0x00}},
        {144'896, {0x02, 0x00, 0x00, 0x00}},
        {194'048, {0x03, 0x00, 0x00, 0x00}},
        {269'568, {0x04, 0x00, 0x00, 0x00}},
        {364'032, {0x05, 0x00, 0x00, 0x00}},
        {411'392, {0x06, 0x00, 0x00, 0x00}},
    };
    BOOST_CHECK(light.fork_schedule == expected_forks);
    BOOST_CHECK_EQUAL(light.fork_schedule_valid_through_epoch, 479'999U);
    BOOST_CHECK_EQUAL(light.electra_epoch, 364'032U);
    BOOST_CHECK_EQUAL(light.min_sync_committee_participants, 342U);
    BOOST_CHECK_EQUAL(light.max_sync_lag_slots, 8'192U);

    BOOST_REQUIRE(busd.withdrawal_mode);
    BOOST_CHECK(*busd.withdrawal_mode ==
                Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1);
    BOOST_CHECK(!busd.managed_withdrawal);
    BOOST_REQUIRE(busd.decentralized_withdrawal);
    const auto& withdrawal{*busd.decentralized_withdrawal};
    BOOST_CHECK_EQUAL(HexStr(withdrawal.ethereum_verifier_address),
                      "e72b3fe73f0d42a6e964d33e7bb1cc2ea7a3f690");
    BOOST_CHECK_EQUAL(HexStr(std::span<const unsigned char>{
                          withdrawal.ethereum_verifier_code_hash.begin(), 32}),
                      "afdba8befb1aacc832bff4e08dcd92e6645a012ea8a8088b0f2811d916022902");
    BOOST_CHECK_EQUAL(HexStr(std::span<const unsigned char>{
                          withdrawal.bootstrap_validator_set_hash.begin(), 32}),
                      "7a0b8aaca4e778df114ad13dcbb8cfdbb0c8cdf45760a564a2ac6c39dd6b2327");
    BOOST_CHECK_EQUAL(withdrawal.withdrawal_rules_version, 1U);
    BOOST_CHECK(withdrawal.withdrawal_rules_commitment ==
                Consensus::DECENTRALIZED_WITHDRAWAL_RULES_COMMITMENT_V1);
    BOOST_CHECK_EQUAL(withdrawal.min_bridge_validators, 4U);
    BOOST_CHECK_EQUAL(withdrawal.max_bridge_validators, 64U);
    BOOST_CHECK_EQUAL(withdrawal.min_bridge_total_weight, 900U);
    BOOST_CHECK_EQUAL(withdrawal.max_epoch_lag, 2'592'000U);

    BOOST_REQUIRE(params.legacy_final_hash);
    BOOST_REQUIRE(params.flowmesh_activation_height);
    BOOST_CHECK_EQUAL(*params.flowmesh_activation_height, 815'000);
    BOOST_CHECK(Consensus::BridgeMintParamsReady(busd));
    BOOST_REQUIRE(params.bridge_withdrawal_activation_height);
    BOOST_CHECK_EQUAL(*params.bridge_withdrawal_activation_height, 811'001);
    const auto asset{modern::ConfiguredBridgeAssetId(params)};
    BOOST_REQUIRE(asset);
    BOOST_CHECK_EQUAL(asset->GetHex(),
                      "ad615d316693dc97d54c20cf2c50ec794cf34699cf970de88f18c381d56b9cc6");
    BOOST_CHECK_EQUAL(HexStr(std::span<const unsigned char>{asset->begin(), 32}),
                      "c69c6bd581c3188fe80d97cf9946f34c79ec502ccf204cd597dc9366315d61ad");
    BOOST_CHECK(modern::ConfiguredDecentralizedBridgeAssetId(params) == asset);
    const auto registry{modern::ConfiguredBridgeRegistryId(params)};
    BOOST_REQUIRE(registry);
    BOOST_CHECK_EQUAL(registry->GetHex(),
                      "cc10d9d9e702e228ab1cb4c5f3fc7821a145f0c0948f1213f09598d5b9806b00");
    BOOST_CHECK_EQUAL(
        HexStr(std::span<const unsigned char>{registry->begin(), 32}),
        "006b80b9d59895f013128f94c0f045a12178fcf3c5b41cab28e202e7d9d910cc");
    BOOST_CHECK(ConfiguredBridgeRegistryEntry(params));
    BOOST_CHECK(!Consensus::BridgeRulesActive(811'000, params));
    BOOST_CHECK(Consensus::BridgeRulesActive(811'001, params));
    BOOST_CHECK(!Consensus::BridgeWithdrawalRulesActive(811'000, params));
    BOOST_CHECK(Consensus::BridgeWithdrawalRulesActive(811'001, params));

    BridgeMintAuthorization mint;
    BOOST_CHECK(AdmitConfiguredDeposit(params, MatchingDeposit(busd, 1'000'000),
                                        1'000'000, {}, mint) ==
                BridgeAdmissionResult::OK);
}

BOOST_AUTO_TEST_CASE(asset_and_registry_ids_are_deterministic_and_domain_bound)
{
    Consensus::Params first{CompleteConsensusParams()};
    const auto asset{modern::ConfiguredBridgeAssetId(first)};
    const auto registry{modern::ConfiguredBridgeRegistryId(first)};
    BOOST_REQUIRE(asset);
    BOOST_REQUIRE(registry);
    BOOST_CHECK(!asset->IsNull());
    BOOST_CHECK(!registry->IsNull());
    BOOST_CHECK_EQUAL(asset->GetHex(),
                      "dff7406176b62792ac0227cc8cc7b76042e8eeaa103624d11782ca3a149f2735");
    BOOST_CHECK_EQUAL(registry->GetHex(),
                      "7e106fb63a6ea3d7a06ea657517ed7f2488ee8de40ce27748a15d32a8409f377");

    // An unknown adapter version cannot borrow direct-token-v1 consensus.
    Consensus::Params upgraded{first};
    upgraded.busd_bridge->adapter_version = 2;
    BOOST_CHECK(modern::ConfiguredBridgeAssetId(upgraded) == asset);
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(*upgraded.busd_bridge));
    BOOST_CHECK(!modern::ConfiguredBridgeRegistryId(upgraded));

    // The first admissible Ethereum block is part of an approval interval,
    // not the stable bUSD balance namespace.
    Consensus::Params later_deployment{first};
    ++*later_deployment.busd_bridge->origin_deployment_block;
    BOOST_CHECK(modern::ConfiguredBridgeAssetId(later_deployment) == asset);
    BOOST_CHECK(modern::ConfiguredBridgeRegistryId(later_deployment) != registry);

    // The same address with different runtime bytecode is a different
    // approval interval and can never reuse the reviewed registry id.
    Consensus::Params different_vault_code{first};
    *different_vault_code.busd_bridge->vault_runtime_code_hash = Hash(15);
    different_vault_code.busd_bridge->managed_withdrawal->vault_runtime_code_hash =
        *different_vault_code.busd_bridge->vault_runtime_code_hash;
    BOOST_CHECK(modern::ConfiguredBridgeAssetId(different_vault_code) == asset);
    BOOST_CHECK(modern::ConfiguredBridgeRegistryId(different_vault_code) != registry);

    // Caps, the Ethereum trust anchor, and withdrawal authorization are all
    // part of the full approval fingerprint as well.
    Consensus::Params different_caps{first};
    ++different_caps.busd_bridge->mint_caps->max_per_epoch;
    BOOST_CHECK(modern::ConfiguredBridgeRegistryId(different_caps) != registry);

    Consensus::Params different_light_client{first};
    different_light_client.busd_bridge->light_client->trusted_checkpoint_root =
        Hash(16);
    BOOST_CHECK(modern::ConfiguredBridgeRegistryId(different_light_client) !=
                registry);

    Consensus::Params different_withdrawal_rules{first};
    different_withdrawal_rules.busd_bridge->managed_withdrawal
        ->withdrawal_rules_commitment = Hash(17);
    BOOST_CHECK(modern::ConfiguredBridgeRegistryId(different_withdrawal_rules) !=
                registry);

    Consensus::Params decentralized{first};
    decentralized.busd_bridge->withdrawal_mode =
        Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1;
    decentralized.busd_bridge->managed_withdrawal.reset();
    decentralized.busd_bridge->decentralized_withdrawal =
        DecentralizedWithdrawalPins();
    BOOST_REQUIRE(Consensus::BridgeMintParamsReady(*decentralized.busd_bridge));
    BOOST_CHECK(modern::ConfiguredBridgeRegistryId(decentralized) != registry);

    // A different sealed B3 chain/fork has a different identity namespace.
    Consensus::Params other_domain{first};
    other_domain.legacy_final_hash = Hash(13);
    BOOST_CHECK(modern::ConfiguredBridgeAssetId(other_domain) != asset);
    BOOST_CHECK(modern::ConfiguredBridgeRegistryId(other_domain) != registry);

    // A different origin vault is a different reserve and therefore asset.
    Consensus::Params other_vault{first};
    other_vault.busd_bridge->asset.vault_address[0] ^= 1;
    BOOST_CHECK(modern::ConfiguredBridgeAssetId(other_vault) != asset);
}

BOOST_AUTO_TEST_CASE(every_security_category_is_a_fail_closed_gate)
{
    const Consensus::BridgeAssetParams complete{CompleteBridgeParams()};
    BOOST_REQUIRE(Consensus::BridgeMintParamsReady(complete));

    auto missing{complete};
    missing.origin_deployment_block.reset();
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.origin_deployment_block = 0;
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.vault_runtime_code_hash.reset();
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.vault_runtime_code_hash = {};
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.implementation_or_adapter.reset();
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.adapter_version.reset();
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.adapter_version = 2;
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.recipient_encoding_version.reset();
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.activation_height.reset();
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.mint_caps.reset();
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.light_client.reset();
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.asset.origin_decimals = 18;
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.asset.asset_decimals = 18;
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.asset.token_address[0] ^= 1;
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.light_client->genesis_validators_root = Hash(18);
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.withdrawal_mode.reset();
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.managed_withdrawal.reset();
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));

    missing = complete;
    missing.light_client->min_sync_committee_participants = 341;
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.light_client->fork_schedule_valid_through_epoch = 9;
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.light_client->trusted_checkpoint_slot =
        (missing.light_client->fork_schedule_valid_through_epoch + 1) *
            Consensus::ETHEREUM_SLOTS_PER_EPOCH -
        1;
    BOOST_CHECK(Consensus::BridgeMintParamsReady(missing));
    // The first slot of the next unknown epoch fails closed. The all-zero
    // genesis fork version in CompleteBridgeParams remains valid.
    ++missing.light_client->trusted_checkpoint_slot;
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.mint_caps->max_per_epoch = missing.mint_caps->max_per_block - 1;
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.managed_withdrawal->authority_address = {};
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.managed_withdrawal->vault_runtime_code_hash = {};
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.managed_withdrawal->vault_runtime_code_hash = Hash(10);
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.managed_withdrawal->withdrawal_rules_version = 0;
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.managed_withdrawal->withdrawal_rules_commitment = {};
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
}

BOOST_AUTO_TEST_CASE(withdrawal_modes_are_explicit_exclusive_and_versioned)
{
    const Consensus::BridgeAssetParams managed{CompleteBridgeParams()};
    BOOST_REQUIRE(Consensus::BridgeMintParamsReady(managed));

    auto ambiguous{managed};
    ambiguous.decentralized_withdrawal = DecentralizedWithdrawalPins();
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(ambiguous));

    auto decentralized{managed};
    decentralized.withdrawal_mode =
        Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1;
    decentralized.managed_withdrawal.reset();
    decentralized.decentralized_withdrawal = DecentralizedWithdrawalPins();
    BOOST_REQUIRE(Consensus::BridgeMintParamsReady(decentralized));

    auto decentralized_params{CompleteConsensusParams()};
    decentralized_params.busd_bridge = decentralized;
    BOOST_CHECK(
        modern::ConfiguredDecentralizedBridgeAssetId(decentralized_params));

    auto incomplete{decentralized};
    incomplete.decentralized_withdrawal->withdrawal_rules_version = 0;
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(incomplete));
    incomplete = decentralized;
    incomplete.decentralized_withdrawal->withdrawal_rules_commitment =
        Hash(14);
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(incomplete));
    incomplete = decentralized;
    incomplete.decentralized_withdrawal->ethereum_verifier_code_hash = {};
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(incomplete));
    incomplete = decentralized;
    incomplete.decentralized_withdrawal->bootstrap_validator_set_hash = {};
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(incomplete));
    incomplete = decentralized;
    incomplete.decentralized_withdrawal->min_bridge_total_weight = 0;
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(incomplete));
    incomplete = decentralized;
    incomplete.decentralized_withdrawal->min_bridge_validators = 3;
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(incomplete));
    incomplete = decentralized;
    incomplete.decentralized_withdrawal->max_bridge_validators = 3;
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(incomplete));
    incomplete = decentralized;
    incomplete.decentralized_withdrawal->max_bridge_validators = 65;
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(incomplete));
    incomplete = decentralized;
    incomplete.decentralized_withdrawal->ethereum_verifier_address =
        incomplete.asset.vault_address;
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(incomplete));
    incomplete = decentralized;
    incomplete.decentralized_withdrawal->ethereum_verifier_address =
        incomplete.asset.token_address;
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(incomplete));
}

BOOST_AUTO_TEST_CASE(configured_admission_binds_tuple_activation_and_caps)
{
    const Consensus::Params params{CompleteConsensusParams()};
    const auto entry{ConfiguredBridgeRegistryEntry(params)};
    BOOST_REQUIRE(entry);
    BOOST_CHECK(entry->origin_chain_id == Consensus::BUSD_ETHEREUM_CHAIN_ID);
    BOOST_CHECK(entry->vault_address == Consensus::BUSD_ETHEREUM_VAULT);
    BOOST_CHECK(entry->token_address == Consensus::BUSD_ETHEREUM_USDT);
    BOOST_CHECK(entry->b3_asset_id == *modern::ConfiguredBridgeAssetId(params));

    const ProvenBridgeDeposit deposit{MatchingDeposit(*params.busd_bridge, 1'000'000)};
    BridgeMintAuthorization mint;
    BOOST_CHECK(AdmitConfiguredDeposit(params, deposit, 499, {}, mint) ==
                BridgeAdmissionResult::REGISTRY_INACTIVE);
    BOOST_REQUIRE(AdmitConfiguredDeposit(params, deposit, 500, {}, mint) ==
                  BridgeAdmissionResult::OK);
    BOOST_CHECK_EQUAL(mint.amount, 1'000'000);
    BOOST_CHECK(mint.asset == entry->b3_asset_id);

    BOOST_CHECK(AdmitConfiguredDeposit(
                    params, deposit, 500,
                    BridgeMintBudget{.minted_this_block = 4'000'001}, mint) ==
                BridgeAdmissionResult::BLOCK_CAP_EXCEEDED);
    BOOST_CHECK(mint.asset.IsNull());
    BOOST_CHECK(AdmitConfiguredDeposit(
                    params, deposit, 500,
                    BridgeMintBudget{.minted_this_epoch = 19'000'001}, mint) ==
                BridgeAdmissionResult::EPOCH_CAP_EXCEEDED);
    BOOST_CHECK(mint.asset.IsNull());
}

BOOST_AUTO_TEST_CASE(flowmesh_regtest_has_complete_test_only_bridge_params)
{
    CChainParams::RegTestOptions options;
    CChainParams::B3ModernRegTestOptions reg_modern;
    reg_modern.flowmesh_test = true;
    options.b3_modern = reg_modern;
    const auto chain{CChainParams::RegTest(options)};
    const Consensus::Params& params{chain->GetConsensus()};

    BOOST_REQUIRE(params.busd_bridge);
    BOOST_CHECK_EQUAL(params.busd_bridge->asset.origin_chain_id, 31'337U);
    BOOST_CHECK(params.busd_bridge->asset.vault_address ==
                Consensus::BUSD_ETHEREUM_VAULT);
    BOOST_CHECK(params.busd_bridge->asset.token_address ==
                Consensus::BUSD_ETHEREUM_USDT);
    BOOST_REQUIRE(params.busd_bridge->origin_deployment_block);
    BOOST_CHECK_EQUAL(*params.busd_bridge->origin_deployment_block, 1U);
    BOOST_REQUIRE(params.busd_bridge->vault_runtime_code_hash);
    BOOST_CHECK(!params.busd_bridge->vault_runtime_code_hash->IsNull());
    BOOST_CHECK(Consensus::BridgeMintParamsReady(*params.busd_bridge));
    BOOST_REQUIRE(params.busd_bridge->withdrawal_mode);
    BOOST_CHECK(*params.busd_bridge->withdrawal_mode ==
                Consensus::BridgeWithdrawalMode::MANAGED_V1);
    BOOST_REQUIRE(params.busd_bridge->managed_withdrawal);
    BOOST_CHECK(params.busd_bridge->managed_withdrawal->Valid());
    BOOST_CHECK(!params.busd_bridge->decentralized_withdrawal);
    BOOST_REQUIRE(params.flowmesh_activation_height);
    BOOST_CHECK(params.busd_bridge->activation_height ==
                params.flowmesh_activation_height);
    BOOST_CHECK(!Consensus::BridgeRulesActive(
        *params.flowmesh_activation_height - 1, params));
    BOOST_CHECK(Consensus::BridgeRulesActive(
        *params.flowmesh_activation_height, params));
    BOOST_REQUIRE(params.bridge_withdrawal_activation_height);
    BOOST_CHECK_EQUAL(*params.bridge_withdrawal_activation_height,
                      *params.flowmesh_activation_height);
    BOOST_CHECK(Consensus::BridgeWithdrawalRulesActive(
        *params.flowmesh_activation_height, params));
    BOOST_CHECK(modern::ConfiguredBridgeAssetId(params).has_value());
    BOOST_CHECK(!modern::ConfiguredDecentralizedBridgeAssetId(params));
    BOOST_CHECK(modern::ConfiguredBridgeRegistryId(params).has_value());
    BOOST_CHECK(ConfiguredBridgeRegistryEntry(params).has_value());
}

BOOST_AUTO_TEST_CASE(bridge_asset_outputs_activate_at_m_without_colored_assets)
{
    CChainParams::RegTestOptions options;
    CChainParams::B3ModernRegTestOptions b3;
    b3.flowmesh_test = true;
    options.b3_modern = b3;
    auto chain{CChainParams::RegTest(options)};
    auto& params{const_cast<Consensus::Params&>(chain->GetConsensus())};

    const int modern_start{*Consensus::ModernPosStartHeight(params)};
    const int asset_start{*params.asset_activation_height};
    BOOST_REQUIRE_LT(modern_start, asset_start);
    BOOST_REQUIRE(params.busd_bridge);
    params.busd_bridge->activation_height = modern_start;
    BOOST_REQUIRE(Consensus::BridgeRulesActive(modern_start, params));
    BOOST_REQUIRE(!Consensus::AssetRulesActive(modern_start, params));

    const auto bridge_asset{modern::ConfiguredBridgeAssetId(params)};
    BOOST_REQUIRE(bridge_asset);
    const CScript owner_script{CScript() << OP_TRUE};
    const auto bridge_owner{
        modern::MakeAssetOwnerOutput(*bridge_asset, 1'000'000, owner_script)};
    const auto bridge_burn{
        modern::MakeAssetBurnOutput(*bridge_asset, 1'000'000)};
    BOOST_REQUIRE(bridge_owner);
    BOOST_REQUIRE(bridge_burn);

    std::string error;
    BOOST_CHECK(modern::ViewAssetAwareOutput(
                    *bridge_owner, modern_start, params, error)
                    .has_value());
    BOOST_CHECK(modern::ViewAssetAwareOutput(
                    *bridge_burn, modern_start, params, error)
                    .has_value());
    // Parsing the configured bridge output does not change or recursively
    // depend on the activation result.
    BOOST_CHECK(Consensus::BridgeRulesActive(modern_start, params));

    modern::AssetId generic_asset{uint256{uint8_t{0x7a}}};
    BOOST_REQUIRE(generic_asset != *bridge_asset);
    const auto generic_owner{
        modern::MakeAssetOwnerOutput(generic_asset, 1'000'000, owner_script)};
    const auto generic_burn{
        modern::MakeAssetBurnOutput(generic_asset, 1'000'000)};
    BOOST_REQUIRE(generic_owner);
    BOOST_REQUIRE(generic_burn);
    BOOST_CHECK(!modern::ViewAssetAwareOutput(
                     *generic_owner, modern_start, params, error)
                     .has_value());
    BOOST_CHECK(!modern::ViewAssetAwareOutput(
                     *generic_burn, modern_start, params, error)
                     .has_value());
    BOOST_CHECK(modern::ViewAssetAwareOutput(
                    *generic_owner, asset_start, params, error)
                    .has_value());
    BOOST_CHECK(modern::ViewAssetAwareOutput(
                    *generic_burn, asset_start, params, error)
                    .has_value());
}

BOOST_AUTO_TEST_SUITE_END()
