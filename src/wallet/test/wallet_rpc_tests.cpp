// Copyright (c) 2025-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/chain.h>
#include <bridge/proof.h>
#include <kernel/chainparams.h>
#include <modern/bridge_asset.h>
#include <modern/policy.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <wallet/rpc/flowmesh.h>
#include <wallet/rpc/assets.h>
#include <wallet/rpc/util.h>
#include <wallet/rpc/wallet.h>

#include <boost/test/unit_test.hpp>

#include <optional>
#include <set>
#include <string>
#include <variant>

namespace wallet {
static uint256 TestHash(const unsigned char tag)
{
    uint256 out;
    out.begin()[0] = tag;
    return out;
}

static std::string TestWalletName(const std::string& endpoint, std::optional<std::string> parameter = std::nullopt)
{
    JSONRPCRequest req;
    req.URI = endpoint;
    return EnsureUniqueWalletName(req, parameter);
}

BOOST_FIXTURE_TEST_SUITE(wallet_rpc_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(ensure_unique_wallet_name)
{
    // EnsureUniqueWalletName should only return if exactly one unique wallet name is provided
    BOOST_CHECK_EQUAL(TestWalletName("/wallet/foo"), "foo");
    BOOST_CHECK_EQUAL(TestWalletName("/wallet/foo", "foo"), "foo");
    BOOST_CHECK_EQUAL(TestWalletName("/", "foo"), "foo");
    BOOST_CHECK_EQUAL(TestWalletName("/bar", "foo"), "foo");

    BOOST_CHECK_THROW(TestWalletName("/"), UniValue);
    BOOST_CHECK_THROW(TestWalletName("/foo"), UniValue);
    BOOST_CHECK_THROW(TestWalletName("/wallet/foo", "bar"), UniValue);
    BOOST_CHECK_THROW(TestWalletName("/wallet/foo", "foobar"), UniValue);
    BOOST_CHECK_THROW(TestWalletName("/wallet/foobar", "foo"), UniValue);
}

BOOST_AUTO_TEST_CASE(flowmesh_vault_operation_discovery_json)
{
    interfaces::FlowMeshVaultOperation operation;
    operation.market_id = TestHash(0x11);
    operation.checkpoint_id = TestHash(0x12);

    modern::FlowMeshDepositAcceptanceV1 deposit;
    deposit.acceptance_id = TestHash(0x13);
    deposit.market_id = operation.market_id;
    deposit.epoch = 4;
    deposit.sequence = 9;
    deposit.deposit_outpoint =
        COutPoint{Txid::FromUint256(TestHash(0x14)), 7};
    deposit.account = TestHash(0x15);
    deposit.asset = TestHash(0x16); // Any simple-v1 colored asset, e.g. BUSD.
    deposit.amount = 25'000;
    deposit.vault_id = TestHash(0x17);
    deposit.shard = 23;
    operation.effect = deposit;
    operation.inputs.push_back(
        interfaces::FlowMeshVaultInput{deposit.deposit_outpoint, CTxOut{}});

    const UniValue json{FlowMeshVaultOperationToJSON(operation)};
    BOOST_CHECK_EQUAL(json.find_value("kind").get_str(), "deposit-sweep");
    BOOST_CHECK_EQUAL(json.find_value("effect_id").get_str(),
                      deposit.acceptance_id.GetHex());
    BOOST_CHECK_EQUAL(json.find_value("market_id").get_str(),
                      operation.market_id.GetHex());
    BOOST_CHECK_EQUAL(json.find_value("checkpoint_id").get_str(),
                      operation.checkpoint_id.GetHex());
    BOOST_CHECK_EQUAL(json.find_value("asset").get_str(),
                      deposit.asset.GetHex());
    BOOST_CHECK_EQUAL(json.find_value("amount").getInt<int64_t>(),
                      deposit.amount);
    BOOST_CHECK_EQUAL(json.find_value("deposit_txid").get_str(),
                      deposit.deposit_outpoint.hash.GetHex());
    BOOST_CHECK_EQUAL(json.find_value("deposit_vout").getInt<int>(), 7);
    BOOST_CHECK_EQUAL(json.find_value("vault_inputs").getInt<int>(), 1);
}

BOOST_AUTO_TEST_CASE(flowmesh_deposit_admission_fails_closed)
{
    using Admission = FlowMeshDepositAdmission;
    const auto check = [](const bool bootstrap, const bool rules_active,
                          const bool established, const bool base_asset,
                          const bool runtime_ready, const bool paused) {
        return CheckFlowMeshDepositAdmission(
            bootstrap, rules_active, established, base_asset,
            runtime_ready, paused);
    };

    BOOST_CHECK(check(false, false, false, true, false, true) ==
                Admission::RULES_INACTIVE);
    BOOST_CHECK(check(false, true, false, true, false, false) ==
                Admission::MARKET_NOT_ESTABLISHED);
    BOOST_CHECK(check(false, true, true, true, false, false) ==
                Admission::RUNTIME_UNAVAILABLE);
    BOOST_CHECK(check(false, true, true, true, true, true) ==
                Admission::MARKET_PAUSED);
    BOOST_CHECK(check(false, true, true, true, true, false) ==
                Admission::USER_DEPOSIT);

    // The only pre-runtime exception is explicit and can only establish the
    // first colored side of a new market. It cannot bypass a pause later.
    BOOST_CHECK(check(true, false, false, true, false, true) ==
                Admission::MARKET_BOOTSTRAP);
    BOOST_CHECK(check(true, true, false, false, false, true) ==
                Admission::BOOTSTRAP_REQUIRES_BASE_ASSET);
    BOOST_CHECK(check(true, true, true, true, false, true) ==
                Admission::BOOTSTRAP_MARKET_ALREADY_ESTABLISHED);
}

BOOST_AUTO_TEST_CASE(bridge_transaction_commands_are_registered)
{
    std::set<std::string> found;
    for (const CRPCCommand& command : GetWalletRPCCommands()) {
        if (command.name == "submitbridgecarrier" ||
            command.name == "claimbridgedeposit" ||
            command.name == "bridgewithdraw") {
            found.insert(command.name);
        }
    }
    BOOST_CHECK_EQUAL(found.size(), 3U);
}

BOOST_AUTO_TEST_CASE(bridge_withdraw_builds_the_pinned_record_kind)
{
    const uint256 registry{TestHash(0x31)};
    bridge::EthAddress recipient;
    recipient.fill(0x42);

    const auto managed{BuildBridgeWithdrawalMpaRecord(
        Consensus::BridgeWithdrawalMode::MANAGED_V1, registry, 7, 123,
        recipient)};
    BOOST_REQUIRE(managed);
    const auto decoded_managed{bridge::DecodeBridgeMpaRecordV1(*managed)};
    BOOST_REQUIRE(decoded_managed);
    BOOST_CHECK(decoded_managed->kind ==
                bridge::BridgeRecordKindV1::MANAGED_WITHDRAWAL);
    const auto* managed_payload{std::get_if<bridge::BridgeManagedWithdrawalV1>(
        &decoded_managed->payload)};
    BOOST_REQUIRE(managed_payload);
    BOOST_CHECK(managed_payload->registry_id == registry);
    BOOST_CHECK_EQUAL(managed_payload->burn_output_index, 7U);
    BOOST_CHECK_EQUAL(managed_payload->raw_amount, 123U);
    BOOST_CHECK(managed_payload->ethereum_recipient == recipient);

    const auto decentralized{BuildBridgeWithdrawalMpaRecord(
        Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1,
        registry, 9, 456, recipient)};
    BOOST_REQUIRE(decentralized);
    const auto decoded_burn{
        bridge::DecodeBridgeMpaRecordV1(*decentralized)};
    BOOST_REQUIRE(decoded_burn);
    BOOST_CHECK(decoded_burn->kind ==
                bridge::BridgeRecordKindV1::BRIDGE_BURN);
    const auto* burn_payload{
        std::get_if<bridge::BridgeBurnV1>(&decoded_burn->payload)};
    BOOST_REQUIRE(burn_payload);
    BOOST_CHECK(burn_payload->registry_id == registry);
    BOOST_CHECK_EQUAL(burn_payload->burn_output_index, 9U);
    BOOST_CHECK_EQUAL(burn_payload->raw_amount, 456U);
    BOOST_CHECK(burn_payload->ethereum_recipient == recipient);
}

BOOST_AUTO_TEST_CASE(bridge_asset_wallet_spends_follow_bridge_activation)
{
    CChainParams::RegTestOptions options;
    CChainParams::B3ModernRegTestOptions b3;
    b3.flowmesh_test = true;
    options.b3_modern = b3;
    auto chain{CChainParams::RegTest(options)};
    auto& params{const_cast<Consensus::Params&>(chain->GetConsensus())};

    const int bridge_height{*Consensus::ModernPosStartHeight(params)};
    BOOST_REQUIRE(params.busd_bridge);
    params.busd_bridge->activation_height = bridge_height;
    BOOST_REQUIRE(Consensus::BridgeRulesActive(bridge_height, params));
    BOOST_REQUIRE(!Consensus::AssetRulesActive(bridge_height, params));

    const auto bridge_asset{modern::ConfiguredBridgeAssetId(params)};
    BOOST_REQUIRE(bridge_asset);
    BOOST_CHECK_EQUAL(
        AssetOwnerPolicy(*bridge_asset, params, bridge_height),
        static_cast<uint16_t>(modern::PolicyType::OWNER));
    BOOST_CHECK_THROW(
        AssetOwnerPolicy(uint256{uint8_t{0x7a}}, params, bridge_height),
        UniValue);
}

BOOST_AUTO_TEST_CASE(bridge_bootstrap_commands_are_registered)
{
    std::set<std::string> found;
    for (const CRPCCommand& command : GetWalletRPCCommands()) {
        if (command.name == "exportbridgebootstrapidentity" ||
            command.name == "signbridgebootstrap") {
            found.insert(command.name);
        }
    }
    BOOST_CHECK_EQUAL(found.size(), 2U);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
