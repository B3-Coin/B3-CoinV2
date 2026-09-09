// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/amount.h>
#include <rpc/client.h>
#include <rpc/protocol.h>
#include <rpc/request.h>
#include <rpc/util.h>
#include <test/util/setup_common.h>
#include <wallet/context.h>
#include <wallet/rpc/assets.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace wallet {
RPCHelpMan sendasset();
RPCHelpMan burnasset();
RPCHelpMan flowmeshdeposit();

namespace {
constexpr CAmount LARGE_UNITS{9'007'199'254'740'993}; // 2^53 + 1, below MAX_MONEY.
const std::string ASSET_ID(64, '1');
using RpcFactory = RPCHelpMan (*)();
const std::array<RpcFactory, 3> AMOUNT_RPCS{sendasset, burnasset, flowmeshdeposit};

std::vector<UniValue> InvalidAssetAmounts()
{
    std::vector<UniValue> values{
        UniValue{}, UniValue{true}, UniValue{false},
        UniValue{UniValue::VARR}, UniValue{UniValue::VOBJ},
    };
    for (const std::string& text : {std::string{}, std::string{" 1"}, std::string{"1 "},
             std::string{"\t1"}, std::string{"+1"}, std::string{"-1"}, std::string{"-0"},
             std::string{"0"}, std::string{"01"}, std::string{"1.0"}, std::string{"0.1"},
             std::string{"1e3"}, std::string{"1E+3"}, std::string{"0x10"},
             std::string{"NaN"}, std::string{"Infinity"}, std::string{"\xD9\xA1"},
             std::to_string(MAX_MONEY + 1), std::string{"9223372036854775808"},
             std::string(100, '9')}) {
        values.emplace_back(text);
    }
    for (const std::string& text : {"0", "-1", "-0", "1.0", "0.1", "1e3",
                                   "9223372036854775808"}) {
        UniValue number;
        if (!number.read(text)) throw std::runtime_error{"invalid test JSON number"};
        values.push_back(number);
    }
    values.emplace_back(MAX_MONEY + 1);
    return values;
}

//! Real RPC argument validation with an unfunded in-memory wallet. The
//! deliberately invalid options field is checked immediately after amount
//! parsing, before chain snapshots, coin selection, signing, or broadcasting.
struct AssetAmountRpcSetup : ChainTestingSetup {
    WalletContext context;
    std::shared_ptr<CWallet> wallet;

    AssetAmountRpcSetup() : ChainTestingSetup(ChainType::REGTEST, TestOpts{.setup_net = false})
    {
        wallet = std::make_shared<CWallet>(m_node.chain.get(), "asset-amount-test", CreateMockableWalletDatabase());
        context.chain = m_node.chain.get();
        WITH_LOCK(context.wallets_mutex, context.wallets.push_back(wallet));
    }

    UniValue Parameters(const std::string& method, const UniValue& amount, bool native = false)
    {
        UniValue params{UniValue::VARR};
        params.push_back(ASSET_ID);
        if (method == "flowmeshdeposit") params.push_back(native ? "B3" : ASSET_ID);
        params.push_back(amount);
        if (method == "sendasset") params.push_back("recipient-is-never-used");
        UniValue options{UniValue::VOBJ};
        options.pushKV("broadcast", false);
        options.pushKV("__offline_stop", true);
        params.push_back(options);
        return params;
    }

    UniValue Error(RpcFactory factory, const UniValue& amount, bool native = false)
    {
        const auto records{GetMockableDatabase(*wallet).m_records};
        auto method{factory()};
        JSONRPCRequest request;
        request.context = &context;
        request.strMethod = method.m_name;
        request.params = Parameters(method.m_name, amount, native);
        UniValue error;
        try {
            method.HandleRequest(request);
            BOOST_FAIL("offline options sentinel must prevent transaction construction");
        } catch (const UniValue& caught) {
            error = caught;
        }
        BOOST_CHECK(GetMockableDatabase(*wallet).m_records == records);
        BOOST_CHECK(WITH_LOCK(wallet->cs_wallet, return wallet->mapWallet.empty()));
        return error;
    }

    void Accepted(RpcFactory factory, const UniValue& amount, bool native = false)
    {
        const auto error{Error(factory, amount, native)};
        BOOST_CHECK_EQUAL(error.find_value("code").getInt<int>(), RPC_INVALID_PARAMETER);
        BOOST_CHECK_EQUAL(error.find_value("message").get_str(), "Unknown option '__offline_stop'");
    }

    void Rejected(RpcFactory factory, const UniValue& amount, bool native = false)
    {
        const auto error{Error(factory, amount, native)};
        const auto code{error.find_value("code").getInt<int>()};
        BOOST_CHECK(code == RPC_TYPE_ERROR || code == RPC_INVALID_PARAMETER);
        BOOST_CHECK(error.find_value("message").get_str().find("Unknown option") == std::string::npos);
    }
};
} // namespace

BOOST_AUTO_TEST_SUITE(asset_amount_rpc_tests)

BOOST_AUTO_TEST_CASE(integer_parser_preserves_atomic_units_in_numbers_and_strings)
{
    static_assert(LARGE_UNITS < MAX_MONEY);
    for (const CAmount units : {CAmount{1}, CAmount{123}, LARGE_UNITS, MAX_MONEY}) {
        BOOST_CHECK_EQUAL(AssetUnitsFromValue(UniValue{units}, "amount"), units);
        BOOST_CHECK_EQUAL(AssetUnitsFromValue(UniValue{std::to_string(units)}, "amount"), units);
    }
    // The asset parser deliberately does not turn one unit into one B3 or
    // consult display metadata, including for assets whose precision is unknown.
    BOOST_CHECK_EQUAL(AssetUnitsFromValue(UniValue{"1"}, "amount"), 1);
}

BOOST_AUTO_TEST_CASE(integer_parser_rejects_ambiguous_invalid_and_out_of_range_values)
{
    for (const auto& value : InvalidAssetAmounts()) {
        BOOST_TEST_CONTEXT("value=" << value.write()) {
            BOOST_CHECK_THROW(AssetUnitsFromValue(value, "amount"), UniValue);
        }
    }
}

BOOST_FIXTURE_TEST_CASE(all_three_handlers_accept_exact_integer_numbers_and_strings, AssetAmountRpcSetup)
{
    for (const auto factory : AMOUNT_RPCS) {
        for (const CAmount units : {CAmount{1}, LARGE_UNITS, MAX_MONEY}) {
            BOOST_TEST_CONTEXT(factory().m_name << ": " << units) {
                Accepted(factory, UniValue{units});
                // String-form clients preserve the same exact atomic units.
                Accepted(factory, UniValue{std::to_string(units)});
            }
        }
    }
}

BOOST_FIXTURE_TEST_CASE(all_three_handlers_reject_invalid_asset_amounts_before_construction, AssetAmountRpcSetup)
{
    for (const auto factory : AMOUNT_RPCS) {
        for (const auto& value : InvalidAssetAmounts()) {
            BOOST_TEST_CONTEXT(factory().m_name << ": " << value.write()) {
                Rejected(factory, value);
            }
        }
    }
}

BOOST_FIXTURE_TEST_CASE(native_flowmesh_deposits_keep_decimal_b3_semantics, AssetAmountRpcSetup)
{
    for (const std::string& text : {"1", "1.25", "0.000000001"}) {
        UniValue number;
        BOOST_REQUIRE(number.read(text));
        Accepted(flowmeshdeposit, number, true);
        Accepted(flowmeshdeposit, UniValue{text}, true);
    }
    for (const std::string& text : {"0", "-1", "0.0000000001", "662200001"}) {
        UniValue number;
        BOOST_REQUIRE(number.read(text));
        Rejected(flowmeshdeposit, number, true);
        Rejected(flowmeshdeposit, UniValue{text}, true);
    }
    Rejected(flowmeshdeposit, UniValue{true}, true);
}

BOOST_AUTO_TEST_CASE(cli_positional_and_named_conversion_preserves_exact_asset_amounts)
{
    const std::string units{std::to_string(LARGE_UNITS)};
    const std::string options{R"({"broadcast":false,"minconf":2})"};
    for (const auto factory : AMOUNT_RPCS) {
        const std::string name{factory().m_name};
        const size_t amount_index{name == "flowmeshdeposit" ? 2U : 1U};
        for (const std::string& amount : {units, "\"" + units + "\""}) {
            std::vector<std::string> positional{ASSET_ID};
            std::vector<std::string> named;
            if (name == "flowmeshdeposit") {
                positional.push_back(ASSET_ID);
                named = {"base_asset_id=" + ASSET_ID, "deposit_asset=" + ASSET_ID};
            } else {
                named = {"asset_id=" + ASSET_ID};
            }
            positional.push_back(amount);
            named.push_back("amount=" + amount);
            if (name == "sendasset") {
                positional.push_back("destination");
                named.push_back("address=destination");
            }
            positional.push_back(options);
            named.push_back("options=" + options);
            const auto converted{RPCConvertValues(name, positional)};
            const auto converted_named{RPCConvertNamedValues(name, named)};
            BOOST_CHECK_EQUAL(converted[0].get_str(), ASSET_ID);
            BOOST_CHECK_EQUAL(AssetUnitsFromValue(converted[amount_index], "amount"), LARGE_UNITS);
            BOOST_CHECK_EQUAL(AssetUnitsFromValue(converted_named.find_value("amount"), "amount"), LARGE_UNITS);
            const auto& converted_options{converted[converted.size() - 1]};
            BOOST_CHECK(converted_options.isObject());
            BOOST_CHECK(!converted_options.find_value("broadcast").get_bool());
            BOOST_CHECK_EQUAL(converted_options.find_value("minconf").getInt<int>(), 2);
            BOOST_CHECK_EQUAL(converted_named.find_value("options").write(), options);
            if (name == "flowmeshdeposit") {
                BOOST_CHECK_EQUAL(converted[1].get_str(), ASSET_ID);
                BOOST_CHECK_EQUAL(converted_named.find_value("base_asset_id").get_str(), ASSET_ID);
                BOOST_CHECK_EQUAL(converted_named.find_value("deposit_asset").get_str(), ASSET_ID);
            }
        }
    }
    const auto native{RPCConvertValues("flowmeshdeposit", {ASSET_ID, "B3", "1.25", options})};
    BOOST_CHECK_EQUAL(native[1].get_str(), "B3");
    BOOST_CHECK_EQUAL(AmountFromValue(native[2]), KILO_COIN + KILO_COIN / 4);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
