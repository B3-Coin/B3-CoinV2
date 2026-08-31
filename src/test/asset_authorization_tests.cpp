// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <modern/asset_output.h>

#include <addresstype.h>
#include <coins.h>
#include <consensus/consensus.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <key.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/sigcache.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <test/util/transaction_utils.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <optional>
#include <vector>

bool CheckInputScripts(const CTransaction& tx, TxValidationState& state,
                       const CCoinsViewCache& inputs, script_verify_flags flags,
                       bool cacheSigStore, bool cacheFullScriptStore,
                       PrecomputedTransactionData& txdata,
                       ValidationCache& validation_cache,
                       std::vector<CScriptCheck>* pvChecks,
                       const std::optional<LegacyLockSpendContext>& legacy_lock)
    EXCLUSIVE_LOCKS_REQUIRED(cs_main);

namespace {

modern::AssetId TestAsset(const unsigned char tag)
{
    modern::AssetId asset;
    asset.begin()[0] = tag;
    return asset;
}

CTxOut AssetOutput(const CScript& owner, const modern::PolicyType policy,
                   const unsigned char tag = 0x51)
{
    const auto output{modern::MakeAssetOwnerOutput(TestAsset(tag), 1, policy, owner)};
    BOOST_REQUIRE(output.has_value());
    return *output;
}

CTransaction FundingTransaction(const CTxOut& output)
{
    CMutableTransaction funding;
    funding.version = 2;
    funding.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
    funding.vout.push_back(output);
    return CTransaction{funding};
}

CMutableTransaction SpendingTransaction(const CTransaction& funding)
{
    CMutableTransaction spend;
    spend.version = 2;
    spend.vin.emplace_back(COutPoint{funding.GetHash(), 0});
    spend.vout.emplace_back(0, CScript() << OP_RETURN << std::vector<unsigned char>{0x01});
    return spend;
}

bool VerifyAgainst(const CMutableTransaction& spend, const CTxOut& previous,
                   const CScript& authorization_script,
                   const bool enable_asset_owner = true)
{
    const CTransaction tx{spend};
    ScriptError error{SCRIPT_ERR_UNKNOWN_ERROR};
    return VerifyScript(tx.vin[0].scriptSig, authorization_script,
                        &tx.vin[0].scriptWitness, STANDARD_SCRIPT_VERIFY_FLAGS,
                        TransactionSignatureChecker{&tx, 0, previous.nValue,
                                                    MissingDataBehavior::ASSERT_FAIL},
                        &error, enable_asset_owner);
}

bool SignSpend(const FillableSigningProvider& provider, const CTransaction& funding,
               CMutableTransaction& spend)
{
    SignatureData signature_data;
    const CTxOut& previous{funding.vout.at(spend.vin.at(0).prevout.n)};
    MutableTransactionSignatureCreator creator{spend, 0, previous.nValue,
                                               SIGHASH_ALL};
    const bool complete{ProduceSignature(provider, creator,
                                         previous.scriptPubKey,
                                         signature_data,
                                         AssetSigningContext::OWNER_SUFFIX)};
    UpdateInput(spend.vin.at(0), signature_data);
    return complete;
}

CScript TwoOfThree(const CKey& first, const CKey& second, const CKey& third)
{
    return CScript() << OP_2 << ToByteVector(first.GetPubKey())
                     << ToByteVector(second.GetPubKey())
                     << ToByteVector(third.GetPubKey()) << OP_3 << OP_CHECKMULTISIG;
}

void CheckTransactionMutationFails(const CMutableTransaction& signed_spend,
                                   const CTxOut& previous)
{
    CMutableTransaction changed_output{signed_spend};
    changed_output.vout[0].scriptPubKey = CScript() << OP_RETURN
                                                    << std::vector<unsigned char>{0x02};
    BOOST_CHECK(!VerifyAgainst(changed_output, previous, previous.scriptPubKey));

    CMutableTransaction changed_locktime{signed_spend};
    changed_locktime.nLockTime = 1;
    BOOST_CHECK(!VerifyAgainst(changed_locktime, previous, previous.scriptPubKey));
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(asset_authorization_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(signing_and_solver_require_trusted_post_h_provenance)
{
    constexpr int LEGACY_FINAL_HEIGHT{100};
    CKey key;
    key.MakeNewKey(true);
    FillableSigningProvider provider;
    BOOST_REQUIRE(provider.AddKey(key));
    const CScript owner{GetScriptForDestination(PKHash{key.GetPubKey()})};
    const CTxOut previous{AssetOutput(owner, modern::PolicyType::OWNER, 0x50)};
    const CTransaction funding{FundingTransaction(previous)};

    std::vector<std::vector<unsigned char>> solutions;
    BOOST_CHECK(Solver(previous.scriptPubKey, solutions) ==
                TxoutType::NONSTANDARD);
    BOOST_CHECK(Solver(previous.scriptPubKey, solutions,
                       /*enable_asset_owner=*/true) ==
                TxoutType::PUBKEYHASH);

    // Even post-H-looking bytes stay on their complete stored script when a
    // caller has no trusted boundary/height context.
    CMutableTransaction contextless{SpendingTransaction(funding)};
    std::map<COutPoint, Coin> post_h_coins;
    post_h_coins.emplace(contextless.vin[0].prevout,
                         Coin{previous, LEGACY_FINAL_HEIGHT + 1,
                              /*coinbase=*/false});
    std::map<int, bilingual_str> errors;
    BOOST_CHECK(!SignTransaction(contextless, &provider, post_h_coins,
                                 SIGHASH_ALL, errors));
    BOOST_CHECK(contextless.vin[0].scriptSig.empty());

    // An identical sealed-era output must likewise keep legacy full-script
    // semantics even when the boundary itself is known.
    CMutableTransaction pre_h{SpendingTransaction(funding)};
    std::map<COutPoint, Coin> pre_h_coins;
    pre_h_coins.emplace(pre_h.vin[0].prevout,
                        Coin{previous, LEGACY_FINAL_HEIGHT,
                             /*coinbase=*/false});
    errors.clear();
    BOOST_CHECK(!SignTransaction(pre_h, &provider, pre_h_coins, SIGHASH_ALL,
                                 errors, LEGACY_FINAL_HEIGHT));
    BOOST_CHECK(pre_h.vin[0].scriptSig.empty());

    // Only the locally authenticated post-H Coin height selects the ordinary
    // owner suffix and produces a valid asset transfer authorization.
    CMutableTransaction post_h{SpendingTransaction(funding)};
    errors.clear();
    BOOST_REQUIRE(SignTransaction(post_h, &provider, post_h_coins, SIGHASH_ALL,
                                  errors, LEGACY_FINAL_HEIGHT));
    BOOST_CHECK(VerifyAgainst(post_h, previous, previous.scriptPubKey,
                              /*enable_asset_owner=*/true));
    BOOST_CHECK(!VerifyAgainst(post_h, previous, previous.scriptPubKey,
                               /*enable_asset_owner=*/false));
}

BOOST_AUTO_TEST_CASE(p2pkh_owner_and_fn_use_the_ordinary_owner_authorization)
{
    CKey key;
    key.MakeNewKey(true);
    FillableSigningProvider provider;
    BOOST_REQUIRE(provider.AddKey(key));
    const CScript owner{GetScriptForDestination(PKHash{key.GetPubKey()})};

    for (const modern::PolicyType policy : {modern::PolicyType::OWNER,
                                            modern::PolicyType::FN}) {
        const CTxOut previous{AssetOutput(owner, policy,
                                          policy == modern::PolicyType::OWNER ? 0x51 : 0x52)};
        const CTransaction funding{FundingTransaction(previous)};
        CMutableTransaction spend{SpendingTransaction(funding)};

        BOOST_REQUIRE(SignSpend(provider, funding, spend));

        // The complete B3A1 carrier authorizes exactly as its owner suffix.
        BOOST_CHECK(VerifyAgainst(spend, previous, previous.scriptPubKey));
        BOOST_CHECK(VerifyAgainst(spend, previous, owner));

        // Authorization remains a normal signature over the complete spend.
        CheckTransactionMutationFails(spend, previous);
    }
}

BOOST_AUTO_TEST_CASE(p2pkh_wrong_key_and_signature_mutation_fail)
{
    CKey owner_key;
    CKey wrong_key;
    owner_key.MakeNewKey(true);
    wrong_key.MakeNewKey(true);
    const CScript owner{GetScriptForDestination(PKHash{owner_key.GetPubKey()})};
    const CTxOut previous{AssetOutput(owner, modern::PolicyType::FN)};
    const CTransaction funding{FundingTransaction(previous)};

    FillableSigningProvider wrong_provider;
    BOOST_REQUIRE(wrong_provider.AddKey(wrong_key));
    CMutableTransaction unauthorized{SpendingTransaction(funding)};
    BOOST_CHECK(!SignSpend(wrong_provider, funding, unauthorized));
    BOOST_CHECK(!VerifyAgainst(unauthorized, previous, previous.scriptPubKey));

    FillableSigningProvider owner_provider;
    BOOST_REQUIRE(owner_provider.AddKey(owner_key));
    CMutableTransaction signed_spend{SpendingTransaction(funding)};
    BOOST_REQUIRE(SignSpend(owner_provider, funding, signed_spend));
    BOOST_REQUIRE_GT(signed_spend.vin[0].scriptSig.size(), 3U);

    CMutableTransaction damaged_signature{signed_spend};
    damaged_signature.vin[0].scriptSig[2] ^= 0x01;
    BOOST_CHECK(!VerifyAgainst(damaged_signature, previous, previous.scriptPubKey));
}

BOOST_AUTO_TEST_CASE(bare_two_of_three_multisig_authorizes_fn)
{
    CKey keys[3];
    for (CKey& key : keys) key.MakeNewKey(true);
    const CScript owner{TwoOfThree(keys[0], keys[1], keys[2])};
    const CTxOut previous{AssetOutput(owner, modern::PolicyType::FN, 0x53)};
    const CTransaction funding{FundingTransaction(previous)};

    FillableSigningProvider provider;
    BOOST_REQUIRE(provider.AddKey(keys[0]));
    BOOST_REQUIRE(provider.AddKey(keys[1]));
    CMutableTransaction spend{SpendingTransaction(funding)};
    BOOST_REQUIRE(SignSpend(provider, funding, spend));
    BOOST_CHECK(VerifyAgainst(spend, previous, previous.scriptPubKey));
    BOOST_CHECK(VerifyAgainst(spend, previous, owner));
    CheckTransactionMutationFails(spend, previous);

    FillableSigningProvider one_key_provider;
    BOOST_REQUIRE(one_key_provider.AddKey(keys[0]));
    CMutableTransaction insufficient{SpendingTransaction(funding)};
    BOOST_CHECK(!SignSpend(one_key_provider, funding, insufficient));
    BOOST_CHECK(!VerifyAgainst(insufficient, previous, previous.scriptPubKey));
}

BOOST_AUTO_TEST_CASE(p2sh_two_of_three_multisig_authorizes_owner)
{
    CKey keys[3];
    for (CKey& key : keys) key.MakeNewKey(true);
    const CScript redeem_script{TwoOfThree(keys[0], keys[1], keys[2])};
    const CScript owner{GetScriptForDestination(ScriptHash{redeem_script})};
    const CTxOut previous{AssetOutput(owner, modern::PolicyType::OWNER, 0x54)};
    const CTransaction funding{FundingTransaction(previous)};

    FillableSigningProvider provider;
    BOOST_REQUIRE(provider.AddKey(keys[0]));
    BOOST_REQUIRE(provider.AddKey(keys[2]));
    BOOST_REQUIRE(provider.AddCScript(redeem_script));
    CMutableTransaction spend{SpendingTransaction(funding)};
    BOOST_REQUIRE(SignSpend(provider, funding, spend));
    BOOST_CHECK(VerifyAgainst(spend, previous, previous.scriptPubKey));
    BOOST_CHECK(VerifyAgainst(spend, previous, owner));
    CheckTransactionMutationFails(spend, previous);

    FillableSigningProvider one_key_provider;
    BOOST_REQUIRE(one_key_provider.AddKey(keys[0]));
    BOOST_REQUIRE(one_key_provider.AddCScript(redeem_script));
    CMutableTransaction insufficient{SpendingTransaction(funding)};
    BOOST_CHECK(!SignSpend(one_key_provider, funding, insufficient));
    BOOST_CHECK(!VerifyAgainst(insufficient, previous, previous.scriptPubKey));
}

BOOST_AUTO_TEST_CASE(provenance_gate_separates_p2sh_multisig_and_sigop_rules)
{
    constexpr int LEGACY_FINAL_HEIGHT{100};
    CKey keys[3];
    for (CKey& key : keys) key.MakeNewKey(true);
    const CScript redeem_script{TwoOfThree(keys[0], keys[1], keys[2])};
    const CScript owner{GetScriptForDestination(ScriptHash{redeem_script})};
    const CTxOut previous{AssetOutput(owner, modern::PolicyType::FN, 0x55)};
    const CTransaction funding{FundingTransaction(previous)};

    // A post-H coin must execute the P2SH owner and therefore needs two keys.
    FillableSigningProvider provider;
    BOOST_REQUIRE(provider.AddKey(keys[0]));
    BOOST_REQUIRE(provider.AddKey(keys[2]));
    BOOST_REQUIRE(provider.AddCScript(redeem_script));
    CMutableTransaction modern_spend{SpendingTransaction(funding)};
    BOOST_REQUIRE(SignSpend(provider, funding, modern_spend));
    BOOST_CHECK(VerifyAgainst(modern_spend, previous, previous.scriptPubKey,
                              /*enable_asset_owner=*/true));

    // The identical bytes created at or below H are not an asset namespace.
    // Under historical script behavior the full stored script is evaluated:
    // its trailing P2SH-looking fragment only checks the redeem-script hash
    // and does not invoke the P2SH special evaluation rule.
    CMutableTransaction legacy_style_spend{SpendingTransaction(funding)};
    legacy_style_spend.vin[0].scriptSig = CScript() << ToByteVector(redeem_script);
    BOOST_CHECK(VerifyAgainst(legacy_style_spend, previous, previous.scriptPubKey,
                              /*enable_asset_owner=*/false));
    BOOST_CHECK(!VerifyAgainst(legacy_style_spend, previous, previous.scriptPubKey,
                               /*enable_asset_owner=*/true));

    CCoinsView legacy_base;
    CCoinsViewCache legacy_inputs{&legacy_base};
    legacy_inputs.AddCoin(legacy_style_spend.vin[0].prevout,
                          Coin{previous, LEGACY_FINAL_HEIGHT, /*coinbase=*/false},
                          /*possible_overwrite=*/false);
    CCoinsView modern_base;
    CCoinsViewCache modern_inputs{&modern_base};
    modern_inputs.AddCoin(legacy_style_spend.vin[0].prevout,
                          Coin{previous, LEGACY_FINAL_HEIGHT + 1, /*coinbase=*/false},
                          /*possible_overwrite=*/false);

    const CTransaction legacy_style_tx{legacy_style_spend};
    constexpr script_verify_flags flags{STANDARD_SCRIPT_VERIFY_FLAGS};
    BOOST_CHECK_EQUAL(GetP2SHSigOpCount(legacy_style_tx, legacy_inputs,
                                       LEGACY_FINAL_HEIGHT),
                      0U);
    BOOST_CHECK_EQUAL(GetP2SHSigOpCount(legacy_style_tx, modern_inputs,
                                       LEGACY_FINAL_HEIGHT),
                      3U);
    BOOST_CHECK_EQUAL(GetTransactionSigOpCost(legacy_style_tx, legacy_inputs, flags,
                                              LEGACY_FINAL_HEIGHT),
                      0);
    BOOST_CHECK_EQUAL(GetTransactionSigOpCost(legacy_style_tx, modern_inputs, flags,
                                              LEGACY_FINAL_HEIGHT),
                      3 * WITNESS_SCALE_FACTOR);

    // Populate the full-script cache with the valid pre-H interpretation.
    // The same transaction, flags, outpoint and script must still be executed
    // (and rejected) for a post-H coin rather than reusing that cached result.
    ValidationCache validation_cache{1U << 20, 1U << 20};
    const LegacyLockSpendContext lock_context{LEGACY_FINAL_HEIGHT, flags};
    LOCK(cs_main);
    PrecomputedTransactionData legacy_txdata;
    TxValidationState legacy_state;
    BOOST_REQUIRE(CheckInputScripts(legacy_style_tx, legacy_state, legacy_inputs,
                                    flags, /*cacheSigStore=*/true,
                                    /*cacheFullScriptStore=*/true, legacy_txdata,
                                    validation_cache, /*pvChecks=*/nullptr,
                                    lock_context));
    std::vector<CScriptCheck> cached_checks;
    TxValidationState cached_state;
    BOOST_REQUIRE(CheckInputScripts(legacy_style_tx, cached_state, legacy_inputs,
                                    flags, /*cacheSigStore=*/true,
                                    /*cacheFullScriptStore=*/true, legacy_txdata,
                                    validation_cache, &cached_checks, lock_context));
    BOOST_CHECK(cached_checks.empty());

    PrecomputedTransactionData modern_txdata;
    TxValidationState modern_state;
    BOOST_CHECK(!CheckInputScripts(legacy_style_tx, modern_state, modern_inputs,
                                   flags, /*cacheSigStore=*/true,
                                   /*cacheFullScriptStore=*/true, modern_txdata,
                                   validation_cache, /*pvChecks=*/nullptr,
                                   lock_context));
}

BOOST_AUTO_TEST_CASE(provenance_gate_cache_cannot_reuse_modern_owner_approval)
{
    constexpr int LEGACY_FINAL_HEIGHT{100};
    CKey key;
    key.MakeNewKey(true);
    FillableSigningProvider provider;
    BOOST_REQUIRE(provider.AddKey(key));
    const CScript owner{GetScriptForDestination(PKHash{key.GetPubKey()})};
    const CTxOut previous{AssetOutput(owner, modern::PolicyType::OWNER, 0x56)};
    const CTransaction funding{FundingTransaction(previous)};
    CMutableTransaction spend{SpendingTransaction(funding)};
    BOOST_REQUIRE(SignSpend(provider, funding, spend));

    // This signature was made for the ordinary owner suffix. It is valid for
    // a post-H asset coin, but not for the full pre-H stored scriptCode.
    BOOST_CHECK(VerifyAgainst(spend, previous, previous.scriptPubKey,
                              /*enable_asset_owner=*/true));
    BOOST_CHECK(!VerifyAgainst(spend, previous, previous.scriptPubKey,
                               /*enable_asset_owner=*/false));

    CCoinsView legacy_base;
    CCoinsViewCache legacy_inputs{&legacy_base};
    legacy_inputs.AddCoin(spend.vin[0].prevout,
                          Coin{previous, LEGACY_FINAL_HEIGHT, /*coinbase=*/false},
                          /*possible_overwrite=*/false);
    CCoinsView modern_base;
    CCoinsViewCache modern_inputs{&modern_base};
    modern_inputs.AddCoin(spend.vin[0].prevout,
                          Coin{previous, LEGACY_FINAL_HEIGHT + 1, /*coinbase=*/false},
                          /*possible_overwrite=*/false);

    const CTransaction tx{spend};
    constexpr script_verify_flags flags{STANDARD_SCRIPT_VERIFY_FLAGS};
    const LegacyLockSpendContext lock_context{LEGACY_FINAL_HEIGHT, flags};
    ValidationCache validation_cache{1U << 20, 1U << 20};
    LOCK(cs_main);

    PrecomputedTransactionData modern_txdata;
    TxValidationState modern_state;
    BOOST_REQUIRE(CheckInputScripts(tx, modern_state, modern_inputs, flags,
                                    /*cacheSigStore=*/true,
                                    /*cacheFullScriptStore=*/true, modern_txdata,
                                    validation_cache, /*pvChecks=*/nullptr,
                                    lock_context));
    std::vector<CScriptCheck> cached_checks;
    TxValidationState cached_state;
    BOOST_REQUIRE(CheckInputScripts(tx, cached_state, modern_inputs, flags,
                                    /*cacheSigStore=*/true,
                                    /*cacheFullScriptStore=*/true, modern_txdata,
                                    validation_cache, &cached_checks, lock_context));
    BOOST_CHECK(cached_checks.empty());

    PrecomputedTransactionData legacy_txdata;
    TxValidationState legacy_state;
    BOOST_CHECK(!CheckInputScripts(tx, legacy_state, legacy_inputs, flags,
                                   /*cacheSigStore=*/true,
                                   /*cacheFullScriptStore=*/true, legacy_txdata,
                                   validation_cache, /*pvChecks=*/nullptr,
                                   lock_context));
}

BOOST_AUTO_TEST_SUITE_END()
