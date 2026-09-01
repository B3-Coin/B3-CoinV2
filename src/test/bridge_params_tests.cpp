// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bridge/admission.h>
#include <consensus/bridge_params.h>
#include <consensus/era.h>
#include <kernel/chainparams.h>
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
    out.implementation_or_adapter = Hash(7);
    out.adapter_version = 3;
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
    light.genesis_validators_root = Hash(9);
    light.fork_schedule = {
        {0, {0x00, 0x00, 0x00, 0x00}},
        {10, {0x01, 0x02, 0x03, 0x04}},
    };
    light.fork_schedule_valid_through_epoch = 20;
    light.electra_epoch = 10;
    light.min_sync_committee_participants =
        Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY;
    light.max_sync_lag_slots = 65'536;
    out.light_client = light;

    out.withdrawal_mode = Consensus::BridgeWithdrawalMode::MANAGED_V1;
    Consensus::BridgeManagedWithdrawalPins withdrawal;
    withdrawal.authority_address.fill(0x55);
    withdrawal.vault_runtime_code_hash = Hash(10);
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
    out.b3_genesis_validator_set_root = Hash(13);
    out.withdrawal_rules_version =
        Consensus::DECENTRALIZED_WITHDRAWAL_RULES_VERSION_V1;
    out.withdrawal_rules_commitment = Hash(14);
    out.min_b3_validator_stake = 1'000'000;
    out.max_epoch_lag = 8;
    return out;
}

Consensus::Params CompleteConsensusParams()
{
    Consensus::Params out;
    out.hashGenesisBlock = Hash(1);
    out.legacy_final_hash = Hash(2);
    out.busd_bridge = CompleteBridgeParams();
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

BOOST_AUTO_TEST_CASE(mainnet_pins_identity_but_bridge_remains_fail_closed)
{
    const auto chain{CChainParams::Main()};
    const Consensus::Params& params{chain->GetConsensus()};
    BOOST_REQUIRE(params.busd_bridge);
    const auto& busd{*params.busd_bridge};

    BOOST_CHECK(busd.asset == Consensus::ETHEREUM_MAINNET_BUSD_IDENTITY);
    BOOST_CHECK_EQUAL(busd.asset.origin_chain_id, 1U);
    BOOST_CHECK_EQUAL(HexStr(busd.asset.vault_address),
                      "143f207e23e6aebd7e974be90ac6d434f4c7bfb6");
    BOOST_CHECK_EQUAL(HexStr(busd.asset.token_address),
                      "dac17f958d2ee523a2206206994597c13d831ec7");
    BOOST_CHECK_EQUAL(busd.asset.origin_decimals, 6U);
    BOOST_CHECK_EQUAL(busd.asset.asset_decimals, 6U);
    BOOST_REQUIRE(busd.withdrawal_mode);
    BOOST_CHECK(*busd.withdrawal_mode ==
                Consensus::BridgeWithdrawalMode::MANAGED_V1);
    BOOST_REQUIRE(busd.managed_withdrawal);
    BOOST_CHECK(busd.managed_withdrawal->authority_address ==
                Consensus::BUSD_ETHEREUM_MANAGED_AUTHORITY);
    BOOST_CHECK_EQUAL(
        HexStr(busd.managed_withdrawal->authority_address),
        "76c7a245d0d2e4cf92403af0144825df1cc614f1");
    BOOST_CHECK(busd.managed_withdrawal->vault_runtime_code_hash ==
                Consensus::BUSD_ETHEREUM_VAULT_RUNTIME_CODE_HASH);
    BOOST_CHECK_EQUAL(
        HexStr(std::span<const unsigned char>{
            busd.managed_withdrawal->vault_runtime_code_hash.begin(), 32}),
        "1be220c18efa4e4cda0bb1c912c7c41346f5c04d49a36ec2c68f6ddcc5586233");
    BOOST_CHECK_EQUAL(
        busd.managed_withdrawal->withdrawal_rules_version,
        Consensus::MANAGED_WITHDRAWAL_RULES_VERSION_V1);
    BOOST_CHECK(busd.managed_withdrawal->withdrawal_rules_commitment.IsNull());
    BOOST_CHECK(!busd.decentralized_withdrawal);

    // X and the A1/A2/A3 feature schedule are pinned, so the stable bUSD
    // AssetId is now derivable. None of that completes or activates the
    // independently gated bridge registry.
    BOOST_REQUIRE(params.legacy_final_hash);
    BOOST_REQUIRE(params.flowmesh_activation_height);
    BOOST_CHECK_EQUAL(*params.flowmesh_activation_height, 815'000);
    BOOST_CHECK(!busd.activation_height);
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(busd));
    const auto asset{modern::ConfiguredBridgeAssetId(params)};
    BOOST_REQUIRE(asset);
    BOOST_CHECK(!asset->IsNull());
    BOOST_CHECK(!modern::ConfiguredBridgeRegistryId(params));
    BOOST_CHECK(!ConfiguredBridgeRegistryEntry(params));
    BOOST_CHECK(!Consensus::BridgeRulesActive(815'000, params));
    BOOST_CHECK(!Consensus::BridgeRulesActive(2'000'000'000, params));

    BridgeMintAuthorization mint;
    BOOST_CHECK(AdmitConfiguredDeposit(params, MatchingDeposit(busd, 1'000'000),
                                        1'000'000, {}, mint) ==
                BridgeAdmissionResult::CONFIGURATION_INCOMPLETE);
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
                      "92f9192bf9a9d2b14798cd51368a4022776113f1337fb59ddd18da551d95238d");
    BOOST_CHECK_EQUAL(registry->GetHex(),
                      "99a2f46f9f2984997766db581aca43780ef7aef4d9509947f4b5467e671ee299");

    // An adapter approval is a new registry identity, not a new bUSD asset.
    Consensus::Params upgraded{first};
    upgraded.busd_bridge->adapter_version = 4;
    BOOST_CHECK(modern::ConfiguredBridgeAssetId(upgraded) == asset);
    BOOST_CHECK(modern::ConfiguredBridgeRegistryId(upgraded) != registry);

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
    missing.implementation_or_adapter.reset();
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.adapter_version.reset();
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
    missing.mint_caps->max_per_epoch = missing.mint_caps->max_per_block - 1;
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.managed_withdrawal->authority_address = {};
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(missing));
    missing = complete;
    missing.managed_withdrawal->vault_runtime_code_hash = {};
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

    auto incomplete{decentralized};
    incomplete.decentralized_withdrawal->withdrawal_rules_version = 0;
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(incomplete));
    incomplete = decentralized;
    incomplete.decentralized_withdrawal->ethereum_verifier_code_hash = {};
    BOOST_CHECK(!Consensus::BridgeMintParamsReady(incomplete));
    incomplete = decentralized;
    incomplete.decentralized_withdrawal->min_b3_validator_stake = 0;
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
    BOOST_CHECK(params.busd_bridge->asset ==
                Consensus::ETHEREUM_MAINNET_BUSD_IDENTITY);
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
    BOOST_CHECK(modern::ConfiguredBridgeAssetId(params).has_value());
    BOOST_CHECK(modern::ConfiguredBridgeRegistryId(params).has_value());
    BOOST_CHECK(ConfiguredBridgeRegistryEntry(params).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
