// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <key.h>
#include <rpc/client.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <test/util/setup_common.h>
#include <wallet/context.h>
#include <wallet/rpc/finality_recovery.h>
#include <wallet/rpc/wallet.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

#include <set>

namespace wallet {
RPCHelpMan getfinalityrecoveryinfo();
RPCHelpMan setfinalityrecovery();
RPCHelpMan clearfinalityrecovery();

namespace {
uint256 RecoveryHash(unsigned char tag)
{
    uint256 hash;
    std::fill(hash.begin(), hash.end(), tag);
    return hash;
}

std::array<unsigned char, 32> RecoveryValidator(unsigned char tag)
{
    std::array<unsigned char, 32> key{};
    key.fill(tag);
    return key;
}

Consensus::FinalitySignerRecovery RecoveryPlan()
{
    Consensus::FinalitySignerRecovery plan;
    plan.chain_domain = RecoveryHash(1);
    plan.validator_key = RecoveryValidator(2);
    plan.incident_height = 101;
    plan.incident_block_hash = RecoveryHash(3);
    plan.incident_epoch = 4;
    plan.incident_signing_set_hash = RecoveryHash(5);
    plan.incident_successor_set_hash = RecoveryHash(6);
    plan.anchor_height = 121;
    plan.anchor_block_hash = RecoveryHash(7);
    return plan;
}

//! Actual wallet RPC handlers, a real in-memory wallet and real ChainImpl.
//! No staking worker, chain mining, journal, scheduler thread, or sockets.
struct RecoveryRpcSetup : BasicTestingSetup
{
    WalletContext context;
    std::shared_ptr<CWallet> wallet;

    RecoveryRpcSetup() : BasicTestingSetup(ChainType::REGTEST, TestOpts{.setup_net = false})
    {
        wallet = std::make_shared<CWallet>(m_node.chain.get(), "recovery-rpc-test", CreateMockableWalletDatabase());
        context.chain = m_node.chain.get();
        WITH_LOCK(context.wallets_mutex, context.wallets.push_back(wallet));
    }

    UniValue Call(RPCHelpMan method, UniValue params = UniValue{UniValue::VARR})
    {
        JSONRPCRequest request;
        request.context = &context;
        request.strMethod = method.m_name;
        request.params = std::move(params);
        return method.HandleRequest(request);
    }

    void ExpectCode(RPCHelpMan method, UniValue params, int code)
    {
        try {
            Call(std::move(method), std::move(params));
            BOOST_FAIL("expected JSON RPC failure");
        } catch (const UniValue& error) {
            BOOST_CHECK_EQUAL(error.find_value("code").getInt<int>(), code);
        }
    }

    std::array<unsigned char, 32> LoadPublicIdentity()
    {
        std::array<unsigned char, 32> bytes{};
        bytes.back() = 1;
        CKey key;
        key.Set(bytes.begin(), bytes.end(), true);
        const CPubKey pubkey{key.GetPubKey()};
        WITH_LOCK(wallet->cs_wallet, wallet->LoadValidatorPubKey(pubkey));
        const XOnlyPubKey xonly{pubkey};
        std::array<unsigned char, 32> result;
        std::copy(xonly.begin(), xonly.end(), result.begin());
        return result;
    }

    UniValue Params(const Consensus::FinalitySignerRecovery& plan)
    {
        UniValue result{UniValue::VARR};
        result.push_back(FinalityRecoveryManifestToJSON(plan));
        result.push_back(plan.anchor_block_hash.GetHex());
        return result;
    }
};
} // namespace

BOOST_AUTO_TEST_SUITE(finality_recovery_rpc_tests)

BOOST_AUTO_TEST_CASE(strict_public_manifest_roundtrip_and_failed_parse_preserves_output)
{
    const auto plan{RecoveryPlan()};
    const auto manifest{FinalityRecoveryManifestToJSON(plan)};
    Consensus::FinalitySignerRecovery output;
    std::string error;
    BOOST_REQUIRE(ParseWalletFinalityRecoveryManifest(manifest, UniValue{plan.anchor_block_hash.GetHex()},
                                                     *plan.validator_key, output, error));
    BOOST_CHECK_EQUAL(FinalityRecoveryManifestToJSON(output).write(), manifest.write());
    const auto unchanged{manifest.write()};
    auto reject = [&](const UniValue& candidate, const UniValue& anchor,
                      const std::array<unsigned char, 32>& validator) {
        BOOST_CHECK(!ParseWalletFinalityRecoveryManifest(candidate, anchor, validator, output, error));
        BOOST_CHECK(!error.empty());
        BOOST_CHECK_EQUAL(FinalityRecoveryManifestToJSON(output).write(), unchanged);
    };
    reject(manifest, UniValue{RecoveryHash(8).GetHex()}, *plan.validator_key);
    reject(manifest, UniValue{plan.anchor_block_hash.GetHex()}, RecoveryValidator(9));
    reject(UniValue{manifest.write()}, UniValue{plan.anchor_block_hash.GetHex()}, *plan.validator_key);
    reject(manifest, UniValue{UniValue::VNUM, "7"}, *plan.validator_key);
    auto extra{manifest};
    extra.pushKV("private_key", "must never be accepted");
    reject(extra, UniValue{plan.anchor_block_hash.GetHex()}, *plan.validator_key);
    auto duplicate{manifest};
    duplicate.pushKVEnd("anchor_height", UniValue{131});
    reject(duplicate, UniValue{plan.anchor_block_hash.GetHex()}, *plan.validator_key);
}

BOOST_AUTO_TEST_CASE(control_preflight_requires_stopped_matching_wallet_without_mutation)
{
    interfaces::FinalityRecoveryControl control;
    const auto plan{RecoveryPlan()};
    std::string error;
    BOOST_CHECK(!CheckWalletFinalityRecoveryControl(control, *plan.validator_key, error));
    control.supported = true;
    BOOST_CHECK(CheckWalletFinalityRecoveryControl(control, *plan.validator_key, error));
    control.running = true;
    BOOST_CHECK(!CheckWalletFinalityRecoveryControl(control, *plan.validator_key, error));
    control.configured = plan;
    const std::string unchanged{FinalityRecoveryManifestToJSON(*control.configured).write()};
    BOOST_CHECK(!CheckWalletFinalityRecoveryControl(control, *plan.validator_key, error));
    control.running = false;
    BOOST_CHECK(CheckWalletFinalityRecoveryControl(control, *plan.validator_key, error));
    BOOST_CHECK(!CheckWalletFinalityRecoveryControl(control, RecoveryValidator(9), error));
    BOOST_CHECK_EQUAL(FinalityRecoveryManifestToJSON(*control.configured).write(), unchanged);
    control.configured->validator_key.reset();
    BOOST_CHECK(!CheckWalletFinalityRecoveryControl(control, *plan.validator_key, error));
}

BOOST_AUTO_TEST_CASE(control_status_exports_only_own_public_plan_and_never_claims_applied)
{
    interfaces::FinalityRecoveryControl control;
    control.supported = true;
    control.configured = RecoveryPlan();
    const auto own{FinalityRecoveryControlToJSON(control, control.configured->validator_key)};
    BOOST_CHECK(own.find_value("configured_for_wallet").get_bool());
    BOOST_CHECK(!own.find_value("configured_for_other_wallet").get_bool());
    BOOST_CHECK_EQUAL(own.find_value("manifest").write(), FinalityRecoveryManifestToJSON(*control.configured).write());
    BOOST_CHECK(own.find_value("recovered").isNull());
    BOOST_CHECK(own.find_value("safe_to_sign").isNull());
    for (const auto key : {std::optional{RecoveryValidator(9)}, std::optional<std::array<unsigned char, 32>>{}}) {
        const auto other{FinalityRecoveryControlToJSON(control, key)};
        BOOST_CHECK(!other.find_value("configured_for_wallet").get_bool());
        BOOST_CHECK(other.find_value("configured_for_other_wallet").get_bool());
        BOOST_CHECK(other.find_value("manifest").isNull());
    }
}

BOOST_AUTO_TEST_CASE(recovery_rpcs_registered_and_cli_object_conversion)
{
    std::set<std::string> methods;
    for (const auto& command : GetWalletRPCCommands()) {
        if (command.name == "getfinalityrecoveryinfo" || command.name == "setfinalityrecovery" ||
            command.name == "clearfinalityrecovery") methods.insert(command.name);
    }
    BOOST_CHECK_EQUAL(methods.size(), 3U);
    const auto plan{RecoveryPlan()};
    const auto manifest{FinalityRecoveryManifestToJSON(plan).write()};
    const auto converted{RPCConvertValues("setfinalityrecovery", {manifest, plan.anchor_block_hash.GetHex()})};
    BOOST_CHECK(converted[0].isObject());
    BOOST_CHECK(converted[1].isStr());
    const auto named{RPCConvertNamedValues("setfinalityrecovery", {
        "manifest=" + manifest, "accepted_anchor=" + plan.anchor_block_hash.GetHex()})};
    BOOST_CHECK(named.find_value("manifest").isObject());
    BOOST_CHECK_EQUAL(named.find_value("accepted_anchor").get_str(), plan.anchor_block_hash.GetHex());
    BOOST_CHECK_THROW(RPCConvertValues("setfinalityrecovery", {"not-json", plan.anchor_block_hash.GetHex()}), std::runtime_error);
}

BOOST_FIXTURE_TEST_CASE(actual_rpc_read_only_no_identity_and_absent_validator_rejected, RecoveryRpcSetup)
{
    const auto records{GetMockableDatabase(*wallet).m_records};
    const auto result{Call(getfinalityrecoveryinfo())};
    BOOST_CHECK(result.find_value("validator_key").isNull());
    BOOST_CHECK_EQUAL(result.find_value("signer").find_value("state").get_str(), "wallet_has_no_validator_key");
    BOOST_CHECK(!result.find_value("operator_recovery").find_value("supported").get_bool());
    ExpectCode(setfinalityrecovery(), Params(RecoveryPlan()), RPC_WALLET_ERROR);
    ExpectCode(clearfinalityrecovery(), UniValue{UniValue::VARR}, RPC_WALLET_ERROR);
    BOOST_CHECK(!WITH_LOCK(wallet->cs_wallet, return wallet->GetValidatorPubKey()).has_value());
    BOOST_CHECK(GetMockableDatabase(*wallet).m_records == records);
}

BOOST_FIXTURE_TEST_CASE(actual_rpc_types_target_and_unavailable_control_fail_without_mutation, RecoveryRpcSetup)
{
    const auto vk{LoadPublicIdentity()};
    const auto records{GetMockableDatabase(*wallet).m_records};
    auto plan{RecoveryPlan()};
    ExpectCode(setfinalityrecovery(), Params(plan), RPC_INVALID_PARAMETER);
    plan.validator_key = vk;
    ExpectCode(setfinalityrecovery(), Params(plan), RPC_WALLET_ERROR); // no staking service
    ExpectCode(clearfinalityrecovery(), UniValue{UniValue::VARR}, RPC_WALLET_ERROR);
    UniValue wrong_type{UniValue::VARR};
    wrong_type.push_back(FinalityRecoveryManifestToJSON(plan).write());
    wrong_type.push_back(plan.anchor_block_hash.GetHex());
    ExpectCode(setfinalityrecovery(), wrong_type, RPC_TYPE_ERROR);
    UniValue wrong_anchor{UniValue::VARR};
    wrong_anchor.push_back(FinalityRecoveryManifestToJSON(plan));
    wrong_anchor.push_back(RecoveryHash(9).GetHex());
    ExpectCode(setfinalityrecovery(), wrong_anchor, RPC_INVALID_PARAMETER);
    const auto info{Call(getfinalityrecoveryinfo())};
    BOOST_CHECK_EQUAL(info.find_value("validator_key").get_str(), HexStr(vk));
    BOOST_CHECK_EQUAL(info.find_value("signer").find_value("state").get_str(), "signer_not_running");
    BOOST_CHECK(GetMockableDatabase(*wallet).m_records == records);
}

BOOST_FIXTURE_TEST_CASE(actual_rpc_locked_wallet_refuses_set_and_clear_but_allows_readonly_status, RecoveryRpcSetup)
{
    {
        LOCK(wallet->cs_wallet);
        wallet->m_keypool_size = 1;
        wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        wallet->SetupDescriptorScriptPubKeyMans();
    }
    const auto vk{LoadPublicIdentity()};
    BOOST_REQUIRE(wallet->EncryptWallet("isolated recovery RPC passphrase"));
    BOOST_REQUIRE(wallet->IsLocked());
    const auto records{GetMockableDatabase(*wallet).m_records};
    auto plan{RecoveryPlan()};
    plan.validator_key = vk;
    ExpectCode(setfinalityrecovery(), Params(plan), RPC_WALLET_UNLOCK_NEEDED);
    ExpectCode(clearfinalityrecovery(), UniValue{UniValue::VARR}, RPC_WALLET_UNLOCK_NEEDED);
    const auto info{Call(getfinalityrecoveryinfo())};
    BOOST_CHECK_EQUAL(info.find_value("validator_key").get_str(), HexStr(vk));
    BOOST_CHECK(wallet->IsLocked());
    BOOST_CHECK(GetMockableDatabase(*wallet).m_records == records);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
