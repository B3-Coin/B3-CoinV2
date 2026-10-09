// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// Owner ruling R10 (open issue OI-8): the wallet side of sponsored native-B3
// fees for bridge V2 burns. A bUSD-only user signs the burn partial
// SIGHASH_ALL|SIGHASH_ANYONECANPAY; a relayer checks the partial
// (CheckSponsoredBurnPartial), adds one native fee coin (SelectSponsorFeeCoin)
// and proves the user's signatures still hold over the completed transaction
// (VerifySponsoredUserInputs). The relayer can add inputs but change no
// output: changing the tip, the change or the recipient breaks the user's
// signature.

#include <wallet/bridge_sponsor.h>

#include <bridge/proof.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/era.h>
#include <key.h>
#include <key_io.h>
#include <modern/asset_output.h>
#include <modern/bridge_binding.h>
#include <modern/bridge_forwarder.h>
#include <modern/policy.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <primitives/transaction_identifier.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/result.h>
#include <util/translation.h>
#include <wallet/test/util.h>
#include <wallet/transaction.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wallet {
namespace {

//! A B3 modern-era regtest (H = 0), so the wallet signs post-H asset coins
//! through their owner suffix as on a pinned chain. No chain is loaded.
struct BridgeSponsorTestingSetup : public BasicTestingSetup {
    BridgeSponsorTestingSetup()
        : BasicTestingSetup{ChainType::REGTEST,
                            {.extra_args = {"-b3modernregtest"}}} {}
};

//! The legacy boundary the pure helpers are given in these tests.
constexpr int H{100};
constexpr int USER_COIN_HEIGHT{H + 5};
constexpr int FEE_COIN_HEIGHT{H + 6};

constexpr unsigned char ACP_NONE{SIGHASH_NONE | SIGHASH_ANYONECANPAY};
constexpr unsigned char ACP_SINGLE{SIGHASH_SINGLE | SIGHASH_ANYONECANPAY};

constexpr CAmount COIN_AMOUNT{1'000'000};
constexpr CAmount BURN_AMOUNT{600'000};
constexpr CAmount TIP_AMOUNT{50'000};
constexpr CAmount CHANGE_AMOUNT{COIN_AMOUNT - BURN_AMOUNT - TIP_AMOUNT};
constexpr CAmount FEE_COIN_VALUE{20'000};

modern::AssetId TestAsset(const unsigned char tag)
{
    modern::AssetId asset;
    asset.begin()[0] = tag;
    return asset;
}

const modern::AssetId BUSD{TestAsset(0xb5)};
const modern::AssetId OTHER_ASSET{TestAsset(0x0c)};

CTxOut OwnerOutput(const modern::AssetId& asset, const CAmount amount,
                   const CScript& owner,
                   const modern::PolicyType policy = modern::PolicyType::OWNER)
{
    const auto output{modern::MakeAssetOwnerOutput(asset, amount, policy, owner)};
    BOOST_REQUIRE(output);
    return *output;
}

CTxOut BusdOutput(const CAmount amount, const CScript& owner)
{
    return OwnerOutput(BUSD, amount, owner);
}

//! The kind 6 record of a route (d1) burn, or a route (d3) burn with `exit`.
CMpaRecord BurnRecord(const unsigned char recipient_tag,
                      const std::optional<modern::ExitForwarderParamsV2>& exit =
                          std::nullopt)
{
    bridge::BridgeBurnV1 burn;
    burn.registry_id = uint256::ONE;
    burn.burn_output_index = 0;
    burn.raw_amount = BURN_AMOUNT;
    burn.ethereum_recipient.fill(recipient_tag);
    burn.exit = exit;
    const auto record{bridge::MakeBridgeMpaRecord(
        bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::BRIDGE_BURN, burn},
        bridge::BRIDGE_MPT_LIMITS_V2)};
    BOOST_REQUIRE(record);
    return *record;
}

modern::ExitForwarderParamsV2 TestExit()
{
    modern::ExitForwarderParamsV2 exit;
    exit.dst_eid = 30110;
    exit.to.fill(0x44);
    exit.max_bridge_fee_bps = 10;
    exit.fee_cap = 1'000;
    exit.refund_eoa.fill(0x55);
    return exit;
}

//! Replaces the record and its binding output together, as anyone who wanted
//! to redirect the burn would have to.
void SetRecord(CMutableTransaction& tx, const CMpaRecord& record)
{
    const auto binding{modern::MakeBridgeBindingOutput(record)};
    BOOST_REQUIRE(binding);
    tx.vout.at(1) = *binding;
    tx.mpa = {record};
}

template <typename T>
void CheckRefused(const util::Result<T>& result, const std::string& prefix)
{
    BOOST_REQUIRE_MESSAGE(!result, "expected a '" << prefix << "' refusal");
    const std::string error{util::ErrorString(result).original};
    BOOST_CHECK_MESSAGE(error.starts_with(prefix),
                        "'" << error << "' does not start with '" << prefix
                            << "'");
}

/**
 * One sponsored burn: a user with one bUSD coin, a relayer with one native
 * fee coin, and a third party. Signing providers are not copyable, so the
 * scenario owns them.
 */
class Scenario
{
public:
    CKey user_key;
    CKey relayer_key;
    CKey other_key;
    CScript user_script;
    CScript relayer_script;
    CScript other_script;
    FillableSigningProvider user_signer;
    FillableSigningProvider relayer_signer;
    const COutPoint user_prevout{Txid::FromUint256(uint256::ONE), 0};
    const COutPoint fee_prevout{Txid::FromUint256(uint256{2}), 1};
    Coin user_coin;
    Coin fee_coin;

    explicit Scenario(const bool witness_user = false)
    {
        user_key.MakeNewKey(true);
        relayer_key.MakeNewKey(true);
        other_key.MakeNewKey(true);
        user_script = witness_user
                          ? GetScriptForDestination(
                                WitnessV0KeyHash{user_key.GetPubKey()})
                          : GetScriptForDestination(PKHash{user_key.GetPubKey()});
        relayer_script = GetScriptForDestination(PKHash{relayer_key.GetPubKey()});
        other_script = GetScriptForDestination(PKHash{other_key.GetPubKey()});
        BOOST_REQUIRE(user_signer.AddKey(user_key));
        BOOST_REQUIRE(relayer_signer.AddKey(relayer_key));
        user_coin = Coin{BusdOutput(COIN_AMOUNT, user_script), USER_COIN_HEIGHT,
                         /*fCoinBaseIn=*/false};
        fee_coin = Coin{CTxOut{FEE_COIN_VALUE, relayer_script}, FEE_COIN_HEIGHT,
                        /*fCoinBaseIn=*/false};
    }

    //! The relayer recognises its own tip by the owner suffix, as the RPC
    //! does with the relayer wallet's IsMine.
    std::function<bool(const CTxOut&)> PaysRelayer() const
    {
        return [this](const CTxOut& out) {
            const auto owner{modern::AssetOwnerScript(out)};
            return owner && *owner == relayer_script;
        };
    }

    //! The unsigned partial: [0] burn, [1] binding, [2] tip, [3] change.
    CMutableTransaction Partial(
        const std::optional<modern::ExitForwarderParamsV2>& exit =
            std::nullopt) const
    {
        const auto burn{modern::MakeAssetBurnOutput(BUSD, BURN_AMOUNT)};
        BOOST_REQUIRE(burn);
        CMutableTransaction tx;
        tx.version = 2;
        tx.vin.emplace_back(user_prevout);
        tx.vin[0].nSequence = CTxIn::MAX_SEQUENCE_NONFINAL;
        tx.vout = {*burn, CTxOut{}, BusdOutput(TIP_AMOUNT, relayer_script),
                   BusdOutput(CHANGE_AMOUNT, user_script)};
        SetRecord(tx, BurnRecord(0x22, exit));
        return tx;
    }

    void SignUser(CMutableTransaction& tx, const int sighash) const
    {
        const std::map<COutPoint, Coin> coins{{user_prevout, user_coin}};
        std::map<int, bilingual_str> errors;
        BOOST_REQUIRE(::SignTransaction(tx, &user_signer, coins, sighash,
                                        errors, H));
        BOOST_REQUIRE(errors.empty());
    }

    CMutableTransaction SignedPartial(
        const std::optional<modern::ExitForwarderParamsV2>& exit =
            std::nullopt) const
    {
        CMutableTransaction tx{Partial(exit)};
        SignUser(tx, SPONSORED_BURN_SIGHASH);
        return tx;
    }

    //! The relayer appends its fee coin and signs that input alone: the
    //! user's input is outside the relayer's coin map and stays untouched.
    CMutableTransaction Complete(const CMutableTransaction& partial) const
    {
        CMutableTransaction completed{partial};
        completed.vin.emplace_back(fee_prevout);
        completed.vin.back().nSequence = CTxIn::MAX_SEQUENCE_NONFINAL;
        const std::map<COutPoint, Coin> coins{{fee_prevout, fee_coin}};
        std::map<int, bilingual_str> errors;
        ::SignTransaction(completed, &relayer_signer, coins, SIGHASH_ALL,
                          errors, H);
        const int fee_index{static_cast<int>(completed.vin.size()) - 1};
        BOOST_CHECK(!errors.contains(fee_index));
        BOOST_CHECK(!completed.vin.back().scriptSig.empty());
        for (size_t i{0}; i < partial.vin.size(); ++i) {
            BOOST_CHECK(completed.vin[i] == partial.vin[i]);
            BOOST_CHECK(completed.vin[i].scriptWitness.stack ==
                        partial.vin[i].scriptWitness.stack);
        }
        return completed;
    }

    std::vector<Coin> Spent(const CMutableTransaction& tx) const
    {
        std::vector<Coin> spent;
        for (const CTxIn& input : tx.vin) {
            if (input.prevout == user_prevout) {
                spent.push_back(user_coin);
            } else if (input.prevout == fee_prevout) {
                spent.push_back(fee_coin);
            } else {
                spent.emplace_back(CTxOut{FEE_COIN_VALUE, relayer_script},
                                   FEE_COIN_HEIGHT, /*fCoinBaseIn=*/false);
            }
        }
        return spent;
    }

    util::Result<SponsoredBurnTip> Check(const CMutableTransaction& partial,
                                         const CAmount min_tip = TIP_AMOUNT) const
    {
        return CheckSponsoredBurnPartial(CTransaction{partial}, BUSD, min_tip,
                                         PaysRelayer());
    }

    util::Result<void> Verify(const CMutableTransaction& completed,
                              const size_t user_inputs = 1,
                              const std::optional<int> legacy_final_height =
                                  H) const
    {
        return VerifySponsoredUserInputs(CTransaction{completed},
                                         Spent(completed), user_inputs,
                                         legacy_final_height);
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(bridge_sponsor_tests, BridgeSponsorTestingSetup)

BOOST_AUTO_TEST_CASE(sponsored_partial_accepted_and_completed)
{
    for (const bool witness_user : {false, true}) {
        for (const bool d3 : {false, true}) {
            BOOST_TEST_CONTEXT((witness_user ? "P2WPKH" : "P2PKH")
                               << (d3 ? " route d3" : " route d1"))
            {
                const Scenario s{witness_user};
                const std::optional<modern::ExitForwarderParamsV2> exit{
                    d3 ? std::optional{TestExit()} : std::nullopt};
                const CMutableTransaction partial{s.SignedPartial(exit)};

                const auto tip{s.Check(partial)};
                BOOST_REQUIRE_MESSAGE(tip, util::ErrorString(tip).original);
                BOOST_CHECK_EQUAL(tip->vout, 2U);
                BOOST_CHECK_EQUAL(tip->amount, TIP_AMOUNT);
                // The minimum is inclusive.
                BOOST_CHECK(s.Check(partial, /*min_tip=*/0));
                BOOST_CHECK(s.Check(partial, /*min_tip=*/TIP_AMOUNT - 1));

                // The partial alone already verifies under the R10 rule.
                const auto alone{s.Verify(partial)};
                BOOST_CHECK_MESSAGE(alone, util::ErrorString(alone).original);

                const CMutableTransaction completed{s.Complete(partial)};
                const auto verified{s.Verify(completed)};
                BOOST_CHECK_MESSAGE(verified,
                                    util::ErrorString(verified).original);
                // The completed transaction is still a well-formed sponsored
                // burn: the relayer added no output.
                BOOST_CHECK(completed.vout == partial.vout);
                BOOST_CHECK(s.Check(completed));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(partial_structure_refusals)
{
    const Scenario s;

    // No inputs.
    {
        CMutableTransaction tx{s.SignedPartial()};
        tx.vin.clear();
        CheckRefused(s.Check(tx), "sponsor-no-inputs");
    }

    // Not a bridge burn: no record, two records, a binding mismatch, a
    // non-bridge record, and a kind 16 sweep (sweeps are never sponsored).
    {
        CMutableTransaction tx{s.SignedPartial()};
        tx.mpa.clear();
        CheckRefused(s.Check(tx), "sponsor-not-a-bridge-burn");
    }
    {
        CMutableTransaction tx{s.SignedPartial()};
        tx.mpa.push_back(tx.mpa.front());
        CheckRefused(s.Check(tx), "sponsor-not-a-bridge-burn");
    }
    {
        CMutableTransaction tx{s.SignedPartial()};
        tx.mpa = {BurnRecord(0x33)};
        CheckRefused(s.Check(tx), "sponsor-not-a-bridge-burn: bridge-binding-mismatch");
    }
    {
        CMutableTransaction tx{s.SignedPartial()};
        tx.vout.erase(tx.vout.begin() + 1);
        CheckRefused(s.Check(tx), "sponsor-not-a-bridge-burn: bridge-binding-missing");
    }
    {
        CMutableTransaction tx{s.SignedPartial()};
        tx.mpa.front().payload_type = 7;
        CheckRefused(s.Check(tx), "sponsor-not-a-bridge-burn");
    }
    {
        bridge::BridgeVaultSweepV2 sweep;
        sweep.registry_id = uint256::ONE;
        const auto record{bridge::MakeBridgeMpaRecord(
            bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::VAULT_SWEEP,
                                   sweep},
            bridge::BRIDGE_MPT_LIMITS_V2)};
        BOOST_REQUIRE(record);
        CMutableTransaction tx{s.Partial()};
        SetRecord(tx, *record);
        s.SignUser(tx, SPONSORED_BURN_SIGHASH);
        CheckRefused(s.Check(tx), "sponsor-not-a-bridge-burn");
    }

    // Unsigned input.
    CheckRefused(s.Check(s.Partial()), "sponsor-input-unsigned: input 0");

    // A native output, which the relayer's fee coin would fund.
    {
        CMutableTransaction tx{s.Partial()};
        tx.vout.emplace_back(1'000, s.user_script);
        s.SignUser(tx, SPONSORED_BURN_SIGHASH);
        CheckRefused(s.Check(tx), "sponsor-native-output: output 4");
    }

    // Tip to another key: no output pays the relayer.
    {
        CMutableTransaction tx{s.Partial()};
        tx.vout[2] = BusdOutput(TIP_AMOUNT, s.other_script);
        s.SignUser(tx, SPONSORED_BURN_SIGHASH);
        CheckRefused(s.Check(tx), "sponsor-tip-missing");
    }

    // Tip below the minimum.
    CheckRefused(s.Check(s.SignedPartial(), /*min_tip=*/TIP_AMOUNT + 1),
                 strprintf("sponsor-tip-below-minimum: tip %d < %d",
                           TIP_AMOUNT, TIP_AMOUNT + 1));

    // Two relayer outputs.
    {
        CMutableTransaction tx{s.Partial()};
        tx.vout[3] = BusdOutput(CHANGE_AMOUNT, s.relayer_script);
        s.SignUser(tx, SPONSORED_BURN_SIGHASH);
        CheckRefused(s.Check(tx), "sponsor-tip-ambiguous: outputs 2 and 3");
    }

    // A relayer output in another asset, or under another owner policy.
    {
        CMutableTransaction tx{s.Partial()};
        tx.vout[2] = OwnerOutput(OTHER_ASSET, TIP_AMOUNT, s.relayer_script);
        s.SignUser(tx, SPONSORED_BURN_SIGHASH);
        CheckRefused(s.Check(tx), "sponsor-tip-not-busd: output 2");
    }
    {
        CMutableTransaction tx{s.Partial()};
        tx.vout[2] = OwnerOutput(BUSD, TIP_AMOUNT, s.relayer_script,
                                 modern::PolicyType::FN);
        s.SignUser(tx, SPONSORED_BURN_SIGHASH);
        CheckRefused(s.Check(tx), "sponsor-tip-not-busd: output 2");
    }
}

BOOST_AUTO_TEST_CASE(relayer_cannot_change_outputs)
{
    for (const bool d3 : {false, true}) {
        BOOST_TEST_CONTEXT((d3 ? "route d3" : "route d1"))
        {
            const Scenario s;
            const std::optional<modern::ExitForwarderParamsV2> exit{
                d3 ? std::optional{TestExit()} : std::nullopt};
            const CMutableTransaction signed_partial{s.SignedPartial(exit)};

            // Each tampered partial is still structurally a sponsored burn,
            // so only the user's signature can refuse it.
            const auto tampered_fails = [&](const char* what, auto&& mutate) {
                BOOST_TEST_CONTEXT(what)
                {
                    CMutableTransaction tampered{signed_partial};
                    mutate(tampered);
                    BOOST_CHECK(s.Check(tampered, /*min_tip=*/0));
                    CheckRefused(s.Verify(s.Complete(tampered)),
                                 "sponsor-signature-invalid: input 0");
                }
            };
            tampered_fails("raised tip", [&](CMutableTransaction& tx) {
                tx.vout[2] = BusdOutput(TIP_AMOUNT + 1, s.relayer_script);
                tx.vout[3] = BusdOutput(CHANGE_AMOUNT - 1, s.user_script);
            });
            tampered_fails("lowered tip", [&](CMutableTransaction& tx) {
                tx.vout[2] = BusdOutput(TIP_AMOUNT - 1, s.relayer_script);
                tx.vout[3] = BusdOutput(CHANGE_AMOUNT + 1, s.user_script);
            });
            tampered_fails("redirected change", [&](CMutableTransaction& tx) {
                tx.vout[3] = BusdOutput(CHANGE_AMOUNT, s.other_script);
            });
            tampered_fails("rebuilt recipient", [&](CMutableTransaction& tx) {
                SetRecord(tx, BurnRecord(0x33, exit));
            });
            if (exit) {
                tampered_fails("rebuilt exit block",
                               [&](CMutableTransaction& tx) {
                                   modern::ExitForwarderParamsV2 other{*exit};
                                   ++other.dst_eid;
                                   SetRecord(tx, BurnRecord(0x22, other));
                               });
            }
            tampered_fails("changed nLockTime",
                           [](CMutableTransaction& tx) { tx.nLockTime = 1; });
        }
    }
}

BOOST_AUTO_TEST_CASE(user_input_sighash_and_provenance)
{
    const Scenario s;

    // A plain SIGHASH_ALL signature verifies only while no input is added.
    {
        CMutableTransaction plain{s.Partial()};
        s.SignUser(plain, SIGHASH_ALL);
        BOOST_CHECK(s.Check(plain));
        BOOST_CHECK(s.Verify(plain));
        CheckRefused(s.Verify(s.Complete(plain)),
                     "sponsor-signature-invalid: input 0");
    }

    // NONE|ACP and SINGLE|ACP leave outputs unsigned; the R10 rule refuses
    // the bytes even before an input is added.
    for (const int weak : {ACP_NONE, ACP_SINGLE}) {
        BOOST_TEST_CONTEXT("hash type " << weak)
        {
            CMutableTransaction tx{s.Partial()};
            s.SignUser(tx, weak);
            BOOST_CHECK(s.Check(tx));
            CheckRefused(s.Verify(tx), "sponsor-signature-invalid: input 0");
            CheckRefused(s.Verify(s.Complete(tx)),
                         "sponsor-signature-invalid: input 0");
        }
    }

    // bUSD never predates H: a spent coin at or below H is refused before
    // any script runs.
    {
        const CMutableTransaction completed{s.Complete(s.SignedPartial())};
        std::vector<Coin> spent{s.Spent(completed)};
        for (const int height : {H, H - 1, 0}) {
            spent[0].nHeight = height;
            CheckRefused(VerifySponsoredUserInputs(CTransaction{completed},
                                                   spent, 1, H),
                         "sponsor-legacy-input: input 0");
        }
        spent[0].nHeight = H + 1;
        BOOST_CHECK(VerifySponsoredUserInputs(CTransaction{completed}, spent,
                                              1, H));
    }

    // The user-input count and the coin vector must describe the completed
    // transaction; a missing coin is refused rather than skipped.
    {
        const CMutableTransaction completed{s.Complete(s.SignedPartial())};
        CheckRefused(s.Verify(completed, /*user_inputs=*/0),
                     "sponsor-user-inputs-invalid");
        CheckRefused(s.Verify(completed, /*user_inputs=*/3),
                     "sponsor-user-inputs-invalid");
        std::vector<Coin> spent{s.Spent(completed)};
        CheckRefused(VerifySponsoredUserInputs(
                         CTransaction{completed},
                         std::vector<Coin>{spent.begin(), spent.end() - 1}, 1,
                         H),
                     "sponsor-spent-mismatch");
        spent[1].Clear();
        CheckRefused(VerifySponsoredUserInputs(CTransaction{completed}, spent,
                                               1, H),
                     "sponsor-input-unknown: input 1");
        // Counting the relayer's fee input as a user input proves nothing
        // about it, but it is signed ALL, which the R10 rule admits.
        BOOST_CHECK(s.Verify(completed, /*user_inputs=*/2));
    }

    // Without a legacy boundary consensus runs every coin's full script, and
    // so does the check: a suffix signature then fails, as it would on chain.
    CheckRefused(s.Verify(s.SignedPartial(), 1, std::nullopt),
                 "sponsor-signature-invalid: input 0");
}

BOOST_AUTO_TEST_CASE(select_sponsor_fee_coin)
{
    const std::array<CAmount, 5> values{5'000, 3'000, 4'000, 3'000, 9'000};
    // The smallest coin that covers, ties broken by the lowest index.
    BOOST_CHECK(SelectSponsorFeeCoin(values, 2'500, 10'000) == 1U);
    BOOST_CHECK(SelectSponsorFeeCoin(values, 3'500, 10'000) == 2U);
    BOOST_CHECK(SelectSponsorFeeCoin(values, 4'001, 10'000) == 0U);
    // Nothing above max_fee is ever spent as fee.
    BOOST_CHECK(!SelectSponsorFeeCoin(values, 1'000, 2'999));
    BOOST_CHECK(SelectSponsorFeeCoin(values, 1'000, 3'000) == 1U);
    BOOST_CHECK(!SelectSponsorFeeCoin(values, 4'100, 4'900));
    BOOST_CHECK(SelectSponsorFeeCoin(values, 6'000, 8'999) == std::nullopt);
    // Both bounds are inclusive.
    BOOST_CHECK(SelectSponsorFeeCoin(values, 4'000, 4'000) == 2U);
    BOOST_CHECK(SelectSponsorFeeCoin(values, 9'000, 9'000) == 4U);
    BOOST_CHECK(SelectSponsorFeeCoin(values, 5'000, 5'000) == 0U);
    // Nothing qualifies.
    BOOST_CHECK(!SelectSponsorFeeCoin(values, 9'001, 20'000));
    BOOST_CHECK(!SelectSponsorFeeCoin(values, 4'000, 3'999));
    BOOST_CHECK(!SelectSponsorFeeCoin({}, 0, 1'000));
}

BOOST_AUTO_TEST_CASE(wallet_coins_for_inputs_and_sponsored_signing)
{
    const std::optional<int> legacy_final_height{
        Consensus::LegacyFinalHeight(Params().GetConsensus())};
    BOOST_REQUIRE(legacy_final_height);
    const int confirmed_height{*legacy_final_height + 7};

    CKey user_key;
    user_key.MakeNewKey(true);
    const CScript user_script{
        GetScriptForDestination(PKHash{user_key.GetPubKey()})};
    CWallet wallet{nullptr, "sponsor-user", CreateMockableWalletDatabase()};
    BOOST_REQUIRE(CreateDescriptor(
        wallet, "pkh(" + EncodeSecret(user_key) + ")", /*success=*/true));

    const auto funding = [&](const uint8_t tag, const bool legacy_encoded) {
        CMutableTransaction tx;
        tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{tag}), 0});
        tx.vout = {CTxOut{1'000, user_script},
                   BusdOutput(COIN_AMOUNT, user_script)};
        tx.m_legacy_encoding = legacy_encoded;
        return MakeTransactionRef(std::move(tx));
    };
    const CTransactionRef confirmed{funding(0x11, false)};
    const CTransactionRef in_mempool{funding(0x12, false)};
    const CTransactionRef inactive{funding(0x13, false)};
    const CTransactionRef legacy_mempool{funding(0x14, true)};

    LOCK(wallet.cs_wallet);
    // Load the parents as the wallet database would: there is no chain here.
    const auto load = [&](const CTransactionRef& tx, const TxState& state)
                          EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet) {
        BOOST_REQUIRE(wallet.LoadToWallet(
            tx->GetHash(), [&](CWalletTx& wtx, bool /*new_tx*/) {
                wtx.SetTx(tx);
                wtx.m_state = state;
                return true;
            }));
    };
    load(confirmed, TxStateConfirmed{uint256{0x21}, confirmed_height, 1});
    load(in_mempool, TxStateInMempool{});
    load(inactive, TxStateInactive{});
    load(legacy_mempool, TxStateInMempool{});

    // CWallet::CoinsForInputs, the map CWallet::SignTransaction(tx) signs
    // with: unknown prevouts and out-of-range indexes are left out.
    {
        CMutableTransaction tx;
        const COutPoint unknown{Txid::FromUint256(uint256{0x7f}), 0};
        const COutPoint out_of_range{confirmed->GetHash(), 9};
        for (const COutPoint& prevout :
             {COutPoint{confirmed->GetHash(), 1}, COutPoint{in_mempool->GetHash(), 1},
              COutPoint{inactive->GetHash(), 0},
              COutPoint{legacy_mempool->GetHash(), 0}, unknown, out_of_range}) {
            tx.vin.emplace_back(prevout);
        }
        const std::map<COutPoint, Coin> coins{wallet.CoinsForInputs(tx)};
        BOOST_CHECK_EQUAL(coins.size(), 4U);
        BOOST_CHECK(!coins.contains(unknown));
        BOOST_CHECK(!coins.contains(out_of_range));
        const auto height_of = [&](const size_t index) {
            return static_cast<uint32_t>(coins.at(tx.vin[index].prevout).nHeight);
        };
        BOOST_CHECK_EQUAL(height_of(0), static_cast<uint32_t>(confirmed_height));
        BOOST_CHECK(coins.at(tx.vin[0].prevout).out == confirmed->vout[1]);
        BOOST_CHECK_EQUAL(height_of(1), MEMPOOL_HEIGHT);
        BOOST_CHECK_EQUAL(height_of(2), 0U);
        BOOST_CHECK_EQUAL(height_of(3), 0U);
        // SignTransaction(tx) still refuses a transaction with an input the
        // wallet does not know, before signing anything.
        CMutableTransaction unsigned_tx{tx};
        BOOST_CHECK(!wallet.SignTransaction(unsigned_tx));
        BOOST_CHECK(unsigned_tx.vin == tx.vin);
    }

    // The bridgewithdraw signing path: the wallet signs its bUSD input with
    // SPONSORED_BURN_SIGHASH through the post-H owner suffix.
    const Scenario relayer;
    const COutPoint user_prevout{confirmed->GetHash(), 1};
    CMutableTransaction partial{relayer.Partial()};
    partial.vin[0].prevout = user_prevout;
    partial.vout[3] = BusdOutput(CHANGE_AMOUNT, user_script);
    {
        std::map<int, bilingual_str> errors;
        BOOST_REQUIRE(wallet.SignTransaction(
            partial, wallet.CoinsForInputs(partial),
            SPONSORED_BURN_SIGHASH, errors));
        BOOST_CHECK(errors.empty());
    }
    BOOST_CHECK(!partial.vin[0].scriptSig.empty());
    const auto tip{CheckSponsoredBurnPartial(CTransaction{partial}, BUSD,
                                             TIP_AMOUNT, relayer.PaysRelayer())};
    BOOST_REQUIRE_MESSAGE(tip, util::ErrorString(tip).original);
    BOOST_CHECK_EQUAL(tip->vout, 2U);

    // The relayer completes it; the user's signature holds.
    CMutableTransaction completed{partial};
    completed.vin.emplace_back(relayer.fee_prevout);
    {
        std::map<int, bilingual_str> errors;
        ::SignTransaction(completed, &relayer.relayer_signer,
                          {{relayer.fee_prevout, relayer.fee_coin}},
                          SIGHASH_ALL, errors, legacy_final_height);
        BOOST_CHECK(!errors.contains(1));
    }
    const Coin user_coin{confirmed->vout[1], confirmed_height,
                         /*fCoinBaseIn=*/false};
    const auto verified{VerifySponsoredUserInputs(
        CTransaction{completed}, {user_coin, relayer.fee_coin}, 1,
        legacy_final_height)};
    BOOST_CHECK_MESSAGE(verified, util::ErrorString(verified).original);

    // Re-signing the completed transaction in the user wallet: the relayer's
    // input is unknown to it, left byte for byte as it was, and reported.
    {
        CMutableTransaction resigned{completed};
        std::map<int, bilingual_str> errors;
        BOOST_CHECK(!wallet.SignTransaction(
            resigned, wallet.CoinsForInputs(resigned),
            SPONSORED_BURN_SIGHASH, errors));
        BOOST_CHECK(errors.contains(1));
        BOOST_CHECK(resigned.vin[1] == completed.vin[1]);
        BOOST_CHECK(resigned.vin[1].scriptWitness.stack ==
                    completed.vin[1].scriptWitness.stack);
        BOOST_CHECK(VerifySponsoredUserInputs(
            CTransaction{resigned}, {user_coin, relayer.fee_coin}, 1,
            legacy_final_height));
    }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace wallet
