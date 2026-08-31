// Copyright (c) 2025-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/chain.h>
#include <rpc/request.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <wallet/rpc/flowmesh.h>
#include <wallet/rpc/util.h>

#include <boost/test/unit_test.hpp>

#include <optional>
#include <string>

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

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
