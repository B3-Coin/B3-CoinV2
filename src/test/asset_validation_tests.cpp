// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <modern/asset_validation.h>

#include <coins.h>
#include <consensus/amount.h>
#include <consensus/fn_params.h>
#include <consensus/modern_pos_params.h>
#include <consensus/params.h>
#include <crypto/common.h>
#include <modern/asset.h>
#include <modern/asset_output.h>
#include <modern/chain_domain.h>
#include <modern/fn.h>
#include <modern/policy.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr int LEGACY_FINAL_HEIGHT{100};
constexpr int FN_GENESIS_HEIGHT{LEGACY_FINAL_HEIGHT + 1};
constexpr int FN_POD_ACTIVATION_HEIGHT{120};
constexpr int ASSET_ACTIVATION_HEIGHT{130};
constexpr CAmount NATIVE_INPUT_VALUE{2'000 * KILO_COIN};
constexpr CAmount NATIVE_CHANGE_VALUE{900 * KILO_COIN};

const uint256 TEST_GENESIS{
    "00000000000000000000000000000000000000000000000000000000000000a1"};
const uint256 TEST_X{
    "00000000000000000000000000000000000000000000000000000000000000a2"};

CScript OwnerScript(const unsigned char fill = 0x31)
{
    return CScript() << OP_DUP << OP_HASH160 << std::vector<unsigned char>(20, fill)
                     << OP_EQUALVERIFY << OP_CHECKSIG;
}

CScript TreasuryScript()
{
    return CScript() << OP_DUP << OP_HASH160 << std::vector<unsigned char>(20, 0x77)
                     << OP_EQUALVERIFY << OP_CHECKSIG;
}

Consensus::Params TestParams()
{
    Consensus::Params params{};
    params.legacy_b3coin = true;
    params.hard_fork_height = FN_GENESIS_HEIGHT;
    params.hashGenesisBlock = TEST_GENESIS;
    params.legacy_final_hash = TEST_X;
    params.modern_pos.emplace();
    const CScript treasury{TreasuryScript()};
    params.modern_pos->treasury_script.assign(treasury.begin(), treasury.end());
    params.fn_pod_activation_height = FN_POD_ACTIVATION_HEIGHT;
    params.asset_activation_height = ASSET_ACTIVATION_HEIGHT;

    // FnRulesActive is deliberately only a cheap configuration-shape gate;
    // manifest/root equivalence belongs to the FN Genesis validation layer.
    params.fn_genesis_rights_root = uint256::ONE;
    Consensus::FnGenesisRight right;
    right.pod_id = uint256::ONE;
    right.recipient_key_hash.fill(0x22);
    params.fn_genesis_manifest.push_back(right);
    return params;
}

modern::AssetId TestAsset()
{
    modern::AssetId asset;
    asset.begin()[0] = 0x55;
    return asset;
}

modern::AssetId FnAsset(const Consensus::Params& params)
{
    const auto asset{modern::ConfiguredFnAssetId(params)};
    BOOST_REQUIRE(asset.has_value());
    return *asset;
}

COutPoint Prevout(const uint32_t number)
{
    uint256 hash;
    WriteBE32(hash.begin(), number);
    return COutPoint{Txid::FromUint256(hash), number};
}

CTxOut AssetOwnerOutput(const modern::AssetId& asset, const CAmount amount,
                        const modern::PolicyType policy = modern::PolicyType::OWNER,
                        const unsigned char owner_fill = 0x31)
{
    const auto out{modern::MakeAssetOwnerOutput(asset, amount, policy,
                                                 OwnerScript(owner_fill))};
    BOOST_REQUIRE(out.has_value());
    return *out;
}

CTxOut AssetBurnOutput(const modern::AssetId& asset, const CAmount amount)
{
    const auto out{modern::MakeAssetBurnOutput(asset, amount)};
    BOOST_REQUIRE(out.has_value());
    return *out;
}

Coin PreviousCoin(const CTxOut& out,
                  const int height = ASSET_ACTIVATION_HEIGHT)
{
    return Coin{out, height, /*coinbase=*/false};
}

Coin NativeCoin(const CAmount amount = NATIVE_INPUT_VALUE)
{
    return PreviousCoin(CTxOut{amount, CScript() << OP_TRUE},
                        LEGACY_FINAL_HEIGHT);
}

CMutableTransaction Spend(const std::vector<COutPoint>& prevouts)
{
    CMutableTransaction tx;
    for (const COutPoint& prevout : prevouts) tx.vin.emplace_back(prevout);
    return tx;
}

CMpaRecord AssetIssuanceRecord(const modern::AssetGenesisV1& genesis)
{
    const modern::CreationAction action{modern::MakeAssetIssuanceAction(genesis)};
    return CMpaRecord{action.action_type, action.action_version, action.payload};
}

modern::AssetId IssuedAsset(const Consensus::Params& params,
                            const COutPoint& defining_prevout,
                            const modern::AssetGenesisV1& genesis)
{
    const auto domain{modern::ModernChainDomain(params.hashGenesisBlock,
                                                 *params.legacy_final_hash)};
    BOOST_REQUIRE(domain.has_value());
    return modern::AssetIdV1(*domain, defining_prevout,
                             modern::AssetGenesisCommitment(genesis));
}

CMutableTransaction IssuanceTransaction(const Consensus::Params& params,
                                        const COutPoint& defining_prevout,
                                        const modern::AssetGenesisV1& genesis,
                                        const CAmount minted,
                                        const CAmount treasury_payment =
                                            modern::ASSET_ISSUANCE_TREASURY_FEE)
{
    CMutableTransaction tx{Spend({defining_prevout})};
    tx.mpa.push_back(AssetIssuanceRecord(genesis));
    tx.vout.push_back(AssetOwnerOutput(IssuedAsset(params, defining_prevout, genesis),
                                       minted));
    tx.vout.emplace_back(treasury_payment, TreasuryScript());
    tx.vout.emplace_back(NATIVE_CHANGE_VALUE, CScript() << OP_TRUE);
    return tx;
}

bool Check(const CMutableTransaction& tx, const std::vector<Coin>& prev_coins,
           const int height, const Consensus::Params& params, std::string& error)
{
    error.clear();
    return modern::CheckAssetTransaction(CTransaction{tx}, prev_coins, height, params,
                                         error);
}

std::string ConservationError(const modern::AssetCheck check)
{
    return "asset conservation failure (code " +
           std::to_string(static_cast<int>(check)) + ")";
}

} // namespace

BOOST_AUTO_TEST_SUITE(asset_validation_tests)

BOOST_AUTO_TEST_CASE(colored_assets_fail_before_and_pass_at_activation)
{
    const Consensus::Params params{TestParams()};
    const modern::AssetId asset{TestAsset()};
    const COutPoint prevout{Prevout(1)};
    CMutableTransaction tx{Spend({prevout})};
    tx.vout.push_back(AssetOwnerOutput(asset, 10, modern::PolicyType::OWNER, 0x32));
    const std::vector<Coin> coins{PreviousCoin(AssetOwnerOutput(asset, 10))};
    std::string error;

    BOOST_CHECK(!Check(tx, coins, ASSET_ACTIVATION_HEIGHT - 1, params, error));
    BOOST_CHECK_EQUAL(error, "input 0: colored-asset owner output is not active");
    BOOST_CHECK(Check(tx, coins, ASSET_ACTIVATION_HEIGHT, params, error));

    // The activation gate does not reinterpret ordinary native transactions.
    CMutableTransaction native{Spend({Prevout(2)})};
    native.vout.emplace_back(9 * KILO_COIN, CScript() << OP_TRUE);
    BOOST_CHECK(Check(native, {NativeCoin(10 * KILO_COIN)},
                      ASSET_ACTIVATION_HEIGHT - 1, params, error));
}

BOOST_AUTO_TEST_CASE(fn_requires_h_plus_one_identity_and_fn_policy)
{
    const Consensus::Params params{TestParams()};
    const modern::AssetId fn{FnAsset(params)};
    const COutPoint prevout{Prevout(3)};
    CMutableTransaction transfer{Spend({prevout})};
    transfer.vout.push_back(AssetOwnerOutput(fn, 2, modern::PolicyType::FN, 0x34));
    const std::vector<Coin> coins{
        PreviousCoin(AssetOwnerOutput(fn, 2, modern::PolicyType::FN, 0x33))};
    std::string error;

    BOOST_CHECK(!Check(transfer, coins, LEGACY_FINAL_HEIGHT, params, error));
    BOOST_CHECK_EQUAL(error, "input 0: FN output is not active");
    BOOST_CHECK(Check(transfer, coins, FN_GENESIS_HEIGHT, params, error));

    CMutableTransaction wrong_id{Spend({prevout})};
    wrong_id.vout.push_back(
        AssetOwnerOutput(TestAsset(), 2, modern::PolicyType::FN, 0x35));
    BOOST_CHECK(!Check(wrong_id, coins, FN_GENESIS_HEIGHT, params, error));
    BOOST_CHECK_EQUAL(error,
                      "output 0: FN policy carries the wrong chain-scoped asset id");

    CMutableTransaction wrong_policy{Spend({prevout})};
    wrong_policy.vout.push_back(
        AssetOwnerOutput(fn, 2, modern::PolicyType::OWNER, 0x36));
    BOOST_CHECK(!Check(wrong_policy, coins, ASSET_ACTIVATION_HEIGHT, params, error));
    BOOST_CHECK_EQUAL(error, "output 0: FN asset must use the FN policy");
}

BOOST_AUTO_TEST_CASE(transfers_conserve_every_non_native_unit_exactly)
{
    const Consensus::Params params{TestParams()};
    const modern::AssetId asset{TestAsset()};
    const COutPoint prevout{Prevout(4)};
    const std::vector<Coin> coins{PreviousCoin(AssetOwnerOutput(asset, 1'000))};
    std::string error;

    CMutableTransaction exact{Spend({prevout})};
    exact.vout.push_back(AssetOwnerOutput(asset, 600, modern::PolicyType::OWNER, 0x40));
    exact.vout.push_back(AssetOwnerOutput(asset, 400, modern::PolicyType::OWNER, 0x41));
    BOOST_CHECK(Check(exact, coins, ASSET_ACTIVATION_HEIGHT, params, error));

    CMutableTransaction implicit_loss{Spend({prevout})};
    implicit_loss.vout.push_back(AssetOwnerOutput(asset, 999));
    BOOST_CHECK(!Check(implicit_loss, coins, ASSET_ACTIVATION_HEIGHT, params, error));
    BOOST_CHECK_EQUAL(error, ConservationError(modern::AssetCheck::CONSERVATION_MISMATCH));

    CMutableTransaction explicit_burn{Spend({prevout})};
    explicit_burn.vout.push_back(AssetOwnerOutput(asset, 999));
    explicit_burn.vout.push_back(AssetBurnOutput(asset, 1));
    BOOST_CHECK(Check(explicit_burn, coins, ASSET_ACTIVATION_HEIGHT, params, error));
}

BOOST_AUTO_TEST_CASE(mint_without_a_type_three_action_is_unauthorized)
{
    const Consensus::Params params{TestParams()};
    CMutableTransaction tx{Spend({Prevout(5)})};
    tx.vout.push_back(AssetOwnerOutput(TestAsset(), 1));
    std::string error;

    BOOST_CHECK(!Check(tx, {NativeCoin()}, ASSET_ACTIVATION_HEIGHT, params, error));
    BOOST_CHECK_EQUAL(error, ConservationError(modern::AssetCheck::UNAUTHORIZED_MINT));
}

BOOST_AUTO_TEST_CASE(bridge_authorization_opens_only_its_exact_surplus)
{
    const Consensus::Params params{TestParams()};
    const modern::AssetId asset{TestAsset()};
    CMutableTransaction tx{Spend({Prevout(51)})};
    tx.vout.push_back(AssetOwnerOutput(asset, 25));
    const std::vector<Coin> coins{NativeCoin()};
    std::string error;
    modern::AssetTransactionEffects effects;

    BOOST_CHECK(modern::CheckAssetTransaction(
        CTransaction{tx}, coins, ASSET_ACTIVATION_HEIGHT, params,
        modern::AssetTransactionContext{
            std::nullopt, NATIVE_INPUT_VALUE,
            modern::AuthorizedAssetMint{asset, 25}},
        effects, error));

    BOOST_CHECK(!modern::CheckAssetTransaction(
        CTransaction{tx}, coins, ASSET_ACTIVATION_HEIGHT, params,
        modern::AssetTransactionContext{
            std::nullopt, NATIVE_INPUT_VALUE,
            modern::AuthorizedAssetMint{asset, 24}},
        effects, error));
    BOOST_CHECK_EQUAL(error,
                      ConservationError(modern::AssetCheck::UNAUTHORIZED_MINT));

    const modern::AssetGenesisV1 genesis{
        .max_supply = 25,
        .decimals = 0,
        .issuance_mode = modern::ASSET_ISSUANCE_MODE_GENESIS_FIXED,
    };
    CMutableTransaction mixed{IssuanceTransaction(
        params, Prevout(51), genesis, genesis.max_supply)};
    BOOST_CHECK(!modern::CheckAssetTransaction(
        CTransaction{mixed}, coins, ASSET_ACTIVATION_HEIGHT, params,
        modern::AssetTransactionContext{
            std::nullopt, NATIVE_INPUT_VALUE,
            modern::AuthorizedAssetMint{asset, 25}},
        effects, error));
    BOOST_CHECK_EQUAL(error,
                      "a transaction cannot combine asset genesis and bridge minting");
}

BOOST_AUTO_TEST_CASE(legacy_b3a1_lookalike_never_becomes_an_asset_input)
{
    const Consensus::Params params{TestParams()};
    const modern::AssetId asset{TestAsset()};
    const CTxOut lookalike{AssetOwnerOutput(asset, 25)};
    const COutPoint prevout{Prevout(50)};
    CMutableTransaction spend{Spend({prevout})};
    spend.vout.push_back(AssetOwnerOutput(asset, 25, modern::PolicyType::OWNER, 0x51));
    std::string error;

    // The exact same bytes created after H are an asset and transfer normally.
    BOOST_CHECK(Check(spend,
                      {PreviousCoin(lookalike, FN_GENESIS_HEIGHT)},
                      ASSET_ACTIVATION_HEIGHT, params, error));

    // If those bytes came from the sealed legacy chain they remain a native,
    // zero-value historical output.  They cannot bootstrap colored supply.
    BOOST_CHECK(!Check(spend,
                       {PreviousCoin(lookalike, LEGACY_FINAL_HEIGHT)},
                       ASSET_ACTIVATION_HEIGHT, params, error));
    BOOST_CHECK_EQUAL(error,
                      ConservationError(modern::AssetCheck::UNAUTHORIZED_MINT));
}

BOOST_AUTO_TEST_CASE(type_three_fixed_supply_issuance_mints_exact_declaration)
{
    const Consensus::Params params{TestParams()};
    const COutPoint defining_prevout{Prevout(6)};
    const modern::AssetGenesisV1 genesis{
        .max_supply = 1'234,
        .decimals = 2,
        .issuance_mode = modern::ASSET_ISSUANCE_MODE_GENESIS_FIXED,
    };
    CMutableTransaction exact{
        IssuanceTransaction(params, defining_prevout, genesis, genesis.max_supply)};
    std::string error;

    BOOST_REQUIRE_EQUAL(exact.mpa.size(), 1U);
    BOOST_CHECK_EQUAL(exact.mpa[0].payload_type,
                      modern::CREATION_ACTION_ASSET_ISSUANCE);
    BOOST_CHECK_EQUAL(exact.mpa[0].payload_version,
                      modern::ASSET_ISSUANCE_ACTION_VERSION_V1);
    BOOST_CHECK(modern::HasAssetCreationAction(CTransaction{exact}));
    BOOST_REQUIRE_EQUAL(modern::AssetCreationActions(CTransaction{exact}).size(), 1U);
    BOOST_CHECK(Check(exact, {NativeCoin()}, ASSET_ACTIVATION_HEIGHT, params, error));

    CMutableTransaction under{
        IssuanceTransaction(params, defining_prevout, genesis, genesis.max_supply - 1)};
    BOOST_CHECK(!Check(under, {NativeCoin()}, ASSET_ACTIVATION_HEIGHT, params, error));
    BOOST_CHECK_EQUAL(error, ConservationError(modern::AssetCheck::ISSUANCE_INVALID));

    CMutableTransaction over{
        IssuanceTransaction(params, defining_prevout, genesis, genesis.max_supply + 1)};
    BOOST_CHECK(!Check(over, {NativeCoin()}, ASSET_ACTIVATION_HEIGHT, params, error));
    BOOST_CHECK_EQUAL(error, ConservationError(modern::AssetCheck::ISSUANCE_INVALID));
}

BOOST_AUTO_TEST_CASE(issuance_requires_at_least_one_thousand_b3_to_treasury)
{
    const Consensus::Params params{TestParams()};
    const COutPoint defining_prevout{Prevout(7)};
    const modern::AssetGenesisV1 genesis{
        .max_supply = 500,
        .decimals = 0,
        .issuance_mode = modern::ASSET_ISSUANCE_MODE_GENESIS_FIXED,
    };
    std::string error;

    CMutableTransaction under{IssuanceTransaction(
        params, defining_prevout, genesis, genesis.max_supply,
        modern::ASSET_ISSUANCE_TREASURY_FEE - 1)};
    BOOST_CHECK(!modern::PaysAssetIssuanceTreasuryFee(CTransaction{under}, params));
    BOOST_CHECK(!Check(under, {NativeCoin()}, ASSET_ACTIVATION_HEIGHT, params, error));
    BOOST_CHECK_EQUAL(error, "asset issuance does not pay 1,000 B3 to the treasury");

    CMutableTransaction exact{IssuanceTransaction(
        params, defining_prevout, genesis, genesis.max_supply)};
    BOOST_CHECK(modern::PaysAssetIssuanceTreasuryFee(CTransaction{exact}, params));
    BOOST_CHECK(Check(exact, {NativeCoin()}, ASSET_ACTIVATION_HEIGHT, params, error));

    CMutableTransaction over{IssuanceTransaction(
        params, defining_prevout, genesis, genesis.max_supply,
        modern::ASSET_ISSUANCE_TREASURY_FEE + 1)};
    BOOST_CHECK(modern::PaysAssetIssuanceTreasuryFee(CTransaction{over}, params));
    BOOST_CHECK(Check(over, {NativeCoin()}, ASSET_ACTIVATION_HEIGHT, params, error));

    // Matching treasury outputs are summed, but lookalike scripts do not
    // count. The combined payment must still reach the exact threshold.
    CMutableTransaction split{exact};
    split.vout.erase(split.vout.begin() + 1);
    split.vout.emplace_back(400 * KILO_COIN, TreasuryScript());
    split.vout.emplace_back(600 * KILO_COIN, TreasuryScript());
    BOOST_CHECK(modern::PaysAssetIssuanceTreasuryFee(CTransaction{split}, params));
    BOOST_CHECK(Check(split, {NativeCoin()}, ASSET_ACTIVATION_HEIGHT, params, error));

    CMutableTransaction wrong_script{exact};
    wrong_script.vout[1].scriptPubKey = OwnerScript(0x78);
    BOOST_CHECK(!modern::PaysAssetIssuanceTreasuryFee(CTransaction{wrong_script}, params));
    BOOST_CHECK(!Check(wrong_script, {NativeCoin()}, ASSET_ACTIVATION_HEIGHT, params,
                       error));
    BOOST_CHECK_EQUAL(error, "asset issuance does not pay 1,000 B3 to the treasury");
}

BOOST_AUTO_TEST_SUITE_END()
