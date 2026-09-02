// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/amount.h>
#include <key.h>
#include <modern/asset_output.h>
#include <modern/fn_pod.h>
#include <policy/fees/block_policy_estimator.h>
#include <script/solver.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/spend.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>

#include <boost/test/unit_test.hpp>

#include <array>

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(spend_tests, WalletTestingSetup)

BOOST_AUTO_TEST_CASE(max_signed_input_size_uses_external_outpoint)
{
    const CKey key{GenerateRandomKey()};
    FillableSigningProvider provider;
    BOOST_REQUIRE(provider.AddKey(key));

    const CTxOut txout{COIN, GetScriptForDestination(PKHash{key.GetPubKey()})};
    const COutPoint outpoint{Txid{}, 0};
    CCoinControl coin_control;
    coin_control.Select(outpoint).SetTxOut(txout);

    const int low_r{CalculateMaximumSignedInputSize(txout, COutPoint{}, &provider, /*can_grind_r=*/true, &coin_control)};
    const int high_r{CalculateMaximumSignedInputSize(txout, outpoint, &provider, /*can_grind_r=*/true, &coin_control)};
    BOOST_CHECK_EQUAL(high_r, low_r + 1);
}

BOOST_AUTO_TEST_CASE(b3_explicit_witness_inputs_fail_clearly)
{
    BOOST_REQUIRE(Params().GetConsensus().legacy_b3coin);

    const CKey key{GenerateRandomKey()};
    const CPubKey pubkey{key.GetPubKey()};
    const CScript witness_redeem{
        GetScriptForDestination(WitnessV0KeyHash{pubkey})};
    const std::array<CTxOut, 3> outputs{
        CTxOut{COIN, witness_redeem},
        CTxOut{COIN, GetScriptForDestination(ScriptHash{witness_redeem})},
        CTxOut{COIN, GetScriptForDestination(PKHash{pubkey})},
    };

    LOCK(m_wallet.cs_wallet);
    for (size_t i{0}; i < outputs.size(); ++i) {
        CCoinControl coin_control;
        coin_control.m_external_provider.keys.emplace(pubkey.GetID(), key);
        coin_control.m_external_provider.pubkeys.emplace(pubkey.GetID(),
                                                         pubkey);
        coin_control.m_external_provider.scripts.emplace(
            CScriptID{witness_redeem}, witness_redeem);
        const COutPoint outpoint{
            Txid::FromUint256(
                uint256{static_cast<uint8_t>(i + 1)}), 0};
        coin_control.Select(outpoint).SetTxOut(outputs[i]);

        FastRandomContext rng{/*fDeterministic=*/true};
        CoinSelectionParams params{rng};
        const auto selected{
            FetchSelectedInputs(m_wallet, coin_control, params)};
        if (i < 2) {
            BOOST_CHECK(!selected);
            BOOST_CHECK(util::ErrorString(selected).original.find(
                            "witness addresses are not active") != std::string::npos);
        } else {
            BOOST_REQUIRE_MESSAGE(selected,
                                  util::ErrorString(selected).original);
            BOOST_CHECK_EQUAL(selected->Size(), 1U);
        }
    }
}

BOOST_FIXTURE_TEST_CASE(SubtractFee, TestChain100Setup)
{
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    auto wallet = CreateSyncedWallet(*m_node.chain, WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain()), coinbaseKey);

    // Check that a subtract-from-recipient transaction slightly less than the
    // coinbase input amount does not create a change output (because it would
    // be uneconomical to add and spend the output), and make sure it pays the
    // leftover input amount which would have been change to the recipient
    // instead of the miner.
    auto check_tx = [&wallet](CAmount leftover_input_amount) {
        CRecipient recipient{PubKeyDestination({}), 50 * COIN - leftover_input_amount, /*subtract_fee=*/true};
        CCoinControl coin_control;
        coin_control.m_feerate.emplace(10000);
        coin_control.fOverrideFeeRate = true;
        // We need to use a change type with high cost of change so that the leftover amount will be dropped to fee instead of added as a change output
        coin_control.m_change_type = OutputType::LEGACY;
        auto res = CreateTransaction(*wallet, {recipient}, /*change_pos=*/std::nullopt, coin_control);
        BOOST_CHECK(res);
        const auto& txr = *res;
        BOOST_CHECK_EQUAL(txr.tx->vout.size(), 1);
        BOOST_CHECK_EQUAL(txr.tx->vout[0].nValue, recipient.nAmount + leftover_input_amount - txr.fee);
        BOOST_CHECK_GT(txr.fee, 0);
        return txr.fee;
    };

    // Send full input amount to recipient, check that only nonzero fee is
    // subtracted (to_reduce == fee).
    const CAmount fee{check_tx(0)};

    // Send slightly less than full input amount to recipient, check leftover
    // input amount is paid to recipient not the miner (to_reduce == fee - 123)
    BOOST_CHECK_EQUAL(fee, check_tx(123));

    // Send full input minus fee amount to recipient, check leftover input
    // amount is paid to recipient not the miner (to_reduce == 0)
    BOOST_CHECK_EQUAL(fee, check_tx(fee));

    // Send full input minus more than the fee amount to recipient, check
    // leftover input amount is paid to recipient not the miner (to_reduce ==
    // -123). This overpays the recipient instead of overpaying the miner more
    // than double the necessary fee.
    BOOST_CHECK_EQUAL(fee, check_tx(fee + 123));
}

BOOST_FIXTURE_TEST_CASE(wallet_duplicated_preset_inputs_test, TestChain100Setup)
{
    // Verify that the wallet's Coin Selection process does not include pre-selected inputs twice in a transaction.

    // Add 4 spendable UTXO, 50 BTC each, to the wallet (total balance 200 BTC)
    for (int i = 0; i < 4; i++) CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    auto wallet = CreateSyncedWallet(*m_node.chain, WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain()), coinbaseKey);

    LOCK(wallet->cs_wallet);
    auto available_coins = AvailableCoins(*wallet);
    std::vector<COutput> coins = available_coins.All();
    // Preselect the first 3 UTXO (150 BTC total)
    std::set<COutPoint> preset_inputs = {coins[0].outpoint, coins[1].outpoint, coins[2].outpoint};

    // Try to create a tx that spends more than what preset inputs + wallet selected inputs are covering for.
    // The wallet can cover up to 200 BTC, and the tx target is 299 BTC.
    std::vector<CRecipient> recipients{{*Assert(wallet->GetNewDestination(OutputType::BECH32, "dummy")),
                                           /*nAmount=*/299 * COIN, /*fSubtractFeeFromAmount=*/true}};
    CCoinControl coin_control;
    coin_control.m_allow_other_inputs = true;
    for (const auto& outpoint : preset_inputs) {
        coin_control.Select(outpoint);
    }

    // Attempt to send 299 BTC from a wallet that only has 200 BTC. The wallet should exclude
    // the preset inputs from the pool of available coins, realize that there is not enough
    // money to fund the 299 BTC payment, and fail with "Insufficient funds".
    //
    // Even with SFFO, the wallet can only afford to send 200 BTC.
    // If the wallet does not properly exclude preset inputs from the pool of available coins
    // prior to coin selection, it may create a transaction that does not fund the full payment
    // amount or, through SFFO, incorrectly reduce the recipient's amount by the difference
    // between the original target and the wrongly counted inputs (in this case 99 BTC)
    // so that the recipient's amount is no longer equal to the user's selected target of 299 BTC.

    // First case, use 'subtract_fee_from_outputs=true'
    BOOST_CHECK(!CreateTransaction(*wallet, recipients, /*change_pos=*/std::nullopt, coin_control));

    // Second case, don't use 'subtract_fee_from_outputs'.
    recipients[0].fSubtractFeeFromAmount = false;
    BOOST_CHECK(!CreateTransaction(*wallet, recipients, /*change_pos=*/std::nullopt, coin_control));
}

BOOST_FIXTURE_TEST_CASE(modern_mpa_and_disintegration_fee_accounting,
                        TestChain100Setup)
{
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    auto wallet = CreateSyncedWallet(
        *m_node.chain,
        WITH_LOCK(Assert(m_node.chainman)->GetMutex(),
                  return m_node.chainman->ActiveChain()),
        coinbaseKey);

    const auto destination{
        wallet->GetNewDestination(OutputType::BECH32, "modern-options-test")};
    BOOST_REQUIRE(destination);
    modern::AssetId test_asset;
    test_asset.begin()[0] = 0x42;
    const auto asset_output{modern::MakeAssetOwnerOutput(
        test_asset, /*amount=*/1, modern::PolicyType::OWNER,
        GetScriptForDestination(*destination))};
    BOOST_REQUIRE(asset_output);
    const std::vector<CRecipient> recipients{
        {CNoDestination{asset_output->scriptPubKey}, asset_output->nValue,
         /*fSubtractFeeFromAmount=*/false}};

    CCoinControl coin_control;
    coin_control.m_feerate = CFeeRate{1'000};
    coin_control.fOverrideFeeRate = true;

    // Deliberately exceed the wallet's ordinary max-fee guard. The destroyed
    // amount must not be classified as a producer fee, while the actual
    // network fee remains bounded and covers the full MPA-aware vsize.
    wallet->m_default_max_tx_fee = COIN;
    const CAmount disintegration{wallet->m_default_max_tx_fee + 1};
    std::optional<COutPoint> selected_anchor;
    {
        LOCK(wallet->cs_wallet);
        for (const auto& [outpoint, txo] : wallet->GetTXOs()) {
            if (txo.GetTxOut().nValue > disintegration + COIN &&
                !wallet->IsTxImmatureCoinBase(txo.GetWalletTx()) &&
                !wallet->IsSpent(outpoint)) {
                selected_anchor = outpoint;
                break;
            }
        }
    }
    BOOST_REQUIRE(selected_anchor);
    coin_control.Select(*selected_anchor);
    BOOST_REQUIRE_LT(disintegration, 10 * COIN);
    const ModernTransactionOptions modern_options{
        .mpa = {modern::MakeModernFnPodRecord(/*created_before=*/0,
                                              /*output_index=*/0)},
        .native_disintegration = disintegration};
    const auto result{CreateTransaction(
        *wallet, recipients, /*change_pos=*/1, coin_control, /*sign=*/true,
        modern_options)};
    BOOST_REQUIRE(result);
    BOOST_CHECK(result->tx->mpa == modern_options.mpa);
    BOOST_REQUIRE(!result->tx->vin.empty());
    BOOST_CHECK(result->tx->vin[0].prevout == *selected_anchor);
    BOOST_REQUIRE_GE(result->tx->vout.size(), 1);
    BOOST_CHECK(result->tx->vout[0] == *asset_output);
    BOOST_CHECK_LE(result->fee, wallet->m_default_max_tx_fee);

    CAmount input_value{0};
    {
        LOCK(wallet->cs_wallet);
        for (const CTxIn& input : result->tx->vin) {
            const CWalletTx* parent{wallet->GetWalletTx(input.prevout.hash)};
            BOOST_REQUIRE(parent);
            BOOST_REQUIRE_LT(input.prevout.n, parent->tx->vout.size());
            input_value += parent->tx->vout[input.prevout.n].nValue;
        }
    }
    const CAmount native_gap{input_value - result->tx->GetValueOut()};
    BOOST_CHECK_EQUAL(native_gap, disintegration + result->fee);
    BOOST_CHECK_GT(native_gap, wallet->m_default_max_tx_fee);
    BOOST_CHECK_GE(
        result->fee,
        coin_control.m_feerate->GetFee(GetVirtualTransactionSize(*result->tx)));

    // A CPU-priced record may have a relay vsize much larger than its bytes.
    // Preselection must fund that floor rather than discovering the deficit
    // only after the final signed-size calculation.
    const ModernTransactionOptions costly_options{
        .mpa = {CMpaRecord{
            modern::CREATION_ACTION_FLOWMESH_SEAT_BINDING,
            modern::FLOWMESH_SEAT_BINDING_ACTION_VERSION_V1, {}}},
        .native_disintegration = 0};
    const auto costly{CreateTransaction(
        *wallet, recipients, /*change_pos=*/1, coin_control, /*sign=*/true,
        costly_options)};
    BOOST_REQUIRE(costly);
    BOOST_CHECK_EQUAL(GetVirtualTransactionSize(*costly->tx),
                      modern::FLOWMESH_SEAT_BINDING_VERIFY_COST);
    BOOST_CHECK_EQUAL(
        costly->fee,
        coin_control.m_feerate->GetFee(GetVirtualTransactionSize(*costly->tx)));
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
