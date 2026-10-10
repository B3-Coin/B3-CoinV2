// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// Owner ruling R10 (open issue OI-8): sponsored native-B3 fees for bridge V2
// burns. A bUSD-only user signs the burn SIGHASH_ALL|SIGHASH_ANYONECANPAY, so
// a relayer can add a native-B3 fee input but change no output. These tests pin
// the script rule that admits that mode, SCRIPT_VERIFY_BRIDGE_SIGHASH_ALL_OUTPUTS,
// and show that such a signature still commits every output, including the
// BRIDGE_RECORD binding that fixes the type-10 record. The managed-V1 rule,
// SCRIPT_VERIFY_BRIDGE_SIGHASH_ALL, is unchanged (asset_authorization_tests).

#include <addresstype.h>
#include <bridge/proof.h>
#include <chainparams.h>
#include <coins.h>
#include <common/args.h>
#include <consensus/amount.h>
#include <consensus/bridge_params.h>
#include <consensus/era.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <key.h>
#include <key_io.h>
#include <modern/asset_output.h>
#include <modern/bridge_binding.h>
#include <modern/bridge_forwarder.h>
#include <modern/policy.h>
#include <node/bridge_state.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <primitives/transaction_identifier.h>
#include <script/descriptor.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/script_error.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/verify_flags.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Non-static in validation.cpp, redeclared as in asset_authorization_tests.
bool CheckInputScripts(const CTransaction& tx, TxValidationState& state,
                       const CCoinsViewCache& inputs, script_verify_flags flags,
                       bool cacheSigStore, bool cacheFullScriptStore,
                       PrecomputedTransactionData& txdata,
                       ValidationCache& validation_cache,
                       std::vector<CScriptCheck>* pvChecks,
                       const std::optional<LegacyLockSpendContext>& legacy_lock,
                       bool require_bridge_sighash_all)
    EXCLUSIVE_LOCKS_REQUIRED(cs_main);

namespace {

constexpr unsigned char ACP_ALL{SIGHASH_ALL | SIGHASH_ANYONECANPAY};
constexpr unsigned char ACP_NONE{SIGHASH_NONE | SIGHASH_ANYONECANPAY};
constexpr unsigned char ACP_SINGLE{SIGHASH_SINGLE | SIGHASH_ANYONECANPAY};

//! The six defined sighash bytes.
constexpr std::array<unsigned char, 6> DEFINED_HASH_TYPES{
    SIGHASH_ALL, SIGHASH_NONE, SIGHASH_SINGLE, ACP_ALL, ACP_NONE, ACP_SINGLE};

//! Bytes outside the defined set. STRICTENC refuses them; the historical
//! LEGACY_B3_STRICTENC profile (frozen legacy-lock inputs) permits them.
constexpr std::array<unsigned char, 3> UNDEFINED_HASH_TYPES{0x00, 0x04, 0x84};

struct BridgeFlagCase {
    const char* name;
    script_verify_flags flags;
    std::set<unsigned char> accepted;
};

//! The bridge flag combinations and the sighash bytes each one admits.
std::vector<BridgeFlagCase> BridgeFlagCases()
{
    return {
        {"no bridge flag", SCRIPT_VERIFY_NONE,
         {SIGHASH_ALL, SIGHASH_NONE, SIGHASH_SINGLE, ACP_ALL, ACP_NONE,
          ACP_SINGLE}},
        {"BRIDGE_SIGHASH_ALL", SCRIPT_VERIFY_BRIDGE_SIGHASH_ALL,
         {SIGHASH_ALL}},
        {"BRIDGE_SIGHASH_ALL_OUTPUTS", SCRIPT_VERIFY_BRIDGE_SIGHASH_ALL_OUTPUTS,
         {SIGHASH_ALL, ACP_ALL}},
        {"both bridge flags",
         SCRIPT_VERIFY_BRIDGE_SIGHASH_ALL |
             SCRIPT_VERIFY_BRIDGE_SIGHASH_ALL_OUTPUTS,
         {SIGHASH_ALL}},
    };
}

class AcceptingSchnorrChecker final : public BaseSignatureChecker
{
public:
    bool CheckSchnorrSignature(std::span<const unsigned char>,
                               std::span<const unsigned char>, SigVersion,
                               ScriptExecutionData&,
                               ScriptError*) const override
    {
        return true;
    }
};

modern::AssetId TestAsset(const unsigned char tag)
{
    modern::AssetId asset;
    asset.begin()[0] = tag;
    return asset;
}

enum class OwnerType { P2PKH, P2WPKH, P2TR };

const char* OwnerTypeName(const OwnerType type)
{
    switch (type) {
    case OwnerType::P2PKH: return "P2PKH";
    case OwnerType::P2WPKH: return "P2WPKH";
    case OwnerType::P2TR: return "P2TR";
    }
    return "?";
}

//! A key's owner script of one type with a provider that can sign it: legacy
//! sighash (P2PKH), BIP143 (P2WPKH) or BIP341 key path (P2TR).
struct Owner {
    CScript script;
    FlatSigningProvider provider;
};

Owner MakeOwner(const CKey& key, const OwnerType type)
{
    const std::string secret{EncodeSecret(key)};
    std::string descriptor;
    switch (type) {
    case OwnerType::P2PKH: descriptor = "pkh(" + secret + ")"; break;
    case OwnerType::P2WPKH: descriptor = "wpkh(" + secret + ")"; break;
    case OwnerType::P2TR: descriptor = "tr(" + secret + ")"; break;
    }
    Owner owner;
    std::string error;
    const auto parsed{Parse(descriptor, owner.provider, error)};
    BOOST_REQUIRE_MESSAGE(parsed.size() == 1, error);
    std::vector<CScript> scripts;
    FlatSigningProvider expanded;
    BOOST_REQUIRE(parsed[0]->Expand(0, owner.provider, scripts, expanded));
    BOOST_REQUIRE_EQUAL(scripts.size(), 1U);
    owner.provider.Merge(std::move(expanded));
    owner.script = scripts[0];
    return owner;
}

constexpr unsigned char ASSET_TAG{0x5a};
constexpr CAmount COIN_AMOUNT{1'000'000};
constexpr CAmount BURN_AMOUNT{600'000};
constexpr CAmount TIP_AMOUNT{50'000};
constexpr CAmount FEE_COIN_VALUE{20'000};

CTxOut OwnerOutput(const CAmount amount, const CScript& owner)
{
    const auto output{modern::MakeAssetOwnerOutput(
        TestAsset(ASSET_TAG), amount, modern::PolicyType::OWNER, owner)};
    BOOST_REQUIRE(output);
    return *output;
}

//! The kind 6 record of a route (d1) burn, or a route (d3) burn with `exit`.
CMpaRecord BurnRecord(const unsigned char recipient_tag,
                      const std::optional<modern::ExitForwarderParamsV2>& exit)
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

//! Replaces the record and its binding output together, as a relayer that
//! wanted to redirect the burn would have to.
void SetRecord(CMutableTransaction& tx, const CMpaRecord& record)
{
    const auto binding{modern::MakeBridgeBindingOutput(record)};
    BOOST_REQUIRE(binding);
    tx.vout.at(1) = *binding;
    tx.mpa = {record};
}

bool BindingHolds(const CMutableTransaction& tx)
{
    BOOST_REQUIRE_EQUAL(tx.mpa.size(), 1U);
    std::string error;
    return modern::CheckBridgeRecordBinding(CTransaction{tx}, tx.mpa[0], error);
}

//! The partial a sponsored user signs: [0] burn, [1] BRIDGE_RECORD binding,
//! [2] bUSD tip to the relayer, [3] bUSD change, and only the user's input.
CMutableTransaction SponsoredBurn(
    const COutPoint& user_coin, const CScript& tip_owner,
    const CScript& change_owner,
    const std::optional<modern::ExitForwarderParamsV2>& exit)
{
    const auto burn{modern::MakeAssetBurnOutput(TestAsset(ASSET_TAG),
                                                BURN_AMOUNT)};
    BOOST_REQUIRE(burn);
    CMutableTransaction tx;
    tx.version = 2;
    tx.vin.emplace_back(user_coin);
    tx.vin[0].nSequence = CTxIn::MAX_SEQUENCE_NONFINAL;
    tx.vout = {*burn, CTxOut{}, OwnerOutput(TIP_AMOUNT, tip_owner),
               OwnerOutput(COIN_AMOUNT - BURN_AMOUNT - TIP_AMOUNT,
                           change_owner)};
    SetRecord(tx, BurnRecord(0x22, exit));
    BOOST_REQUIRE(BindingHolds(tx));
    return tx;
}

bool SignInput(const SigningProvider& provider, CMutableTransaction& tx,
               const unsigned int index, const std::vector<CTxOut>& spent,
               const int hash_type, const AssetSigningContext context)
{
    BOOST_REQUIRE_EQUAL(spent.size(), tx.vin.size());
    PrecomputedTransactionData txdata;
    txdata.Init(tx, std::vector<CTxOut>{spent}, /*force=*/true);
    const MutableTransactionSignatureCreator creator{
        tx, index, spent.at(index).nValue, &txdata, hash_type};
    SignatureData signature_data;
    const bool complete{ProduceSignature(provider, creator,
                                         spent.at(index).scriptPubKey,
                                         signature_data, context)};
    UpdateInput(tx.vin.at(index), signature_data);
    return complete;
}

std::pair<bool, ScriptError> VerifyInput(const CMutableTransaction& spend,
                                         const unsigned int index,
                                         const std::vector<CTxOut>& spent,
                                         const script_verify_flags flags)
{
    BOOST_REQUIRE_EQUAL(spent.size(), spend.vin.size());
    const CTransaction tx{spend};
    PrecomputedTransactionData txdata;
    txdata.Init(tx, std::vector<CTxOut>{spent}, /*force=*/true);
    ScriptError error{SCRIPT_ERR_UNKNOWN_ERROR};
    const bool ok{VerifyScript(
        tx.vin[index].scriptSig, spent[index].scriptPubKey,
        &tx.vin[index].scriptWitness, flags,
        TransactionSignatureChecker{&tx, index, spent[index].nValue, txdata,
                                    MissingDataBehavior::FAIL},
        &error, /*enable_asset_owner=*/true)};
    return {ok, error};
}

constexpr script_verify_flags SPONSORED_FLAGS{
    STANDARD_SCRIPT_VERIFY_FLAGS | SCRIPT_VERIFY_BRIDGE_SIGHASH_ALL_OUTPUTS};
constexpr script_verify_flags MANAGED_FLAGS{
    STANDARD_SCRIPT_VERIFY_FLAGS | SCRIPT_VERIFY_BRIDGE_SIGHASH_ALL};

bool UserInputVerifies(const CMutableTransaction& spend,
                       const std::vector<CTxOut>& spent,
                       const unsigned int user_index = 0)
{
    return VerifyInput(spend, user_index, spent, SPONSORED_FLAGS).first;
}

void CheckOwnerAllAnyoneCanPay(const OwnerType type, const bool d3)
{
    CKey user_key;
    user_key.MakeNewKey(true);
    CKey relayer_key;
    relayer_key.MakeNewKey(true);
    CKey other_key;
    other_key.MakeNewKey(true);
    const Owner user{MakeOwner(user_key, type)};
    const CScript relayer_owner{
        GetScriptForDestination(PKHash{relayer_key.GetPubKey()})};
    const CScript other_owner{
        GetScriptForDestination(PKHash{other_key.GetPubKey()})};
    FillableSigningProvider relayer;
    BOOST_REQUIRE(relayer.AddKey(relayer_key));

    const std::optional<modern::ExitForwarderParamsV2> exit{
        d3 ? std::optional{TestExit()} : std::nullopt};
    const COutPoint user_coin{Txid::FromUint256(uint256::ONE), 0};
    const COutPoint fee_coin{Txid::FromUint256(uint256{2}), 1};
    const CTxOut user_previous{OwnerOutput(COIN_AMOUNT, user.script)};
    const CTxOut fee_previous{FEE_COIN_VALUE, relayer_owner};
    const ScriptError hashtype_error{type == OwnerType::P2TR
                                         ? SCRIPT_ERR_SCHNORR_SIG_HASHTYPE
                                         : SCRIPT_ERR_SIG_HASHTYPE};

    // The user signs the partial with ALL|ANYONECANPAY. The R10 rule accepts
    // it; the managed-V1 rule (and so mainnet) still refuses the byte.
    CMutableTransaction partial{
        SponsoredBurn(user_coin, relayer_owner, user.script, exit)};
    BOOST_REQUIRE(SignInput(user.provider, partial, 0, {user_previous},
                            ACP_ALL, AssetSigningContext::OWNER_SUFFIX));
    BOOST_CHECK(UserInputVerifies(partial, {user_previous}));
    {
        const auto [ok, error]{
            VerifyInput(partial, 0, {user_previous}, MANAGED_FLAGS)};
        BOOST_CHECK(!ok);
        BOOST_CHECK_EQUAL(error, hashtype_error);
        const auto [both_ok, both_error]{VerifyInput(
            partial, 0, {user_previous},
            SPONSORED_FLAGS | SCRIPT_VERIFY_BRIDGE_SIGHASH_ALL)};
        BOOST_CHECK(!both_ok);
        BOOST_CHECK_EQUAL(both_error, hashtype_error);
    }

    // The relayer appends one native fee input and signs it SIGHASH_ALL. The
    // user's signature still holds and the whole transaction verifies.
    CMutableTransaction completed{partial};
    completed.vin.emplace_back(fee_coin);
    const std::vector<CTxOut> spent{user_previous, fee_previous};
    BOOST_REQUIRE(SignInput(relayer, completed, 1, spent, SIGHASH_ALL,
                            AssetSigningContext::FULL_SCRIPT));
    BOOST_CHECK(UserInputVerifies(completed, spent));
    BOOST_CHECK(VerifyInput(completed, 1, spent, SPONSORED_FLAGS).first);
    BOOST_CHECK(BindingHolds(completed));

    // The input set and order are what ANYONECANPAY leaves open: a second
    // added input, or the fee input placed first, keeps the signature valid.
    {
        CMutableTransaction more_inputs{completed};
        more_inputs.vin.emplace_back(
            COutPoint{Txid::FromUint256(uint256{3}), 0});
        BOOST_CHECK(UserInputVerifies(
            more_inputs, {user_previous, fee_previous, fee_previous}));

        CMutableTransaction fee_first{partial};
        fee_first.vin.insert(fee_first.vin.begin(), CTxIn{fee_coin});
        BOOST_CHECK(UserInputVerifies(fee_first, {fee_previous, user_previous},
                                      /*user_index=*/1));
    }

    // Every output, and the fields of the user's own input and of the
    // transaction, stay committed.
    const auto mutation_fails = [&](const char* what, auto&& mutate) {
        CMutableTransaction changed{completed};
        mutate(changed);
        BOOST_CHECK_MESSAGE(!UserInputVerifies(changed, spent),
                            what << " was accepted");
    };
    const CAmount change_amount{COIN_AMOUNT - BURN_AMOUNT - TIP_AMOUNT};
    mutation_fails("raised tip", [&](CMutableTransaction& tx) {
        tx.vout[2] = OwnerOutput(TIP_AMOUNT + 1, relayer_owner);
        tx.vout[3] = OwnerOutput(change_amount - 1, user.script);
    });
    mutation_fails("lowered tip", [&](CMutableTransaction& tx) {
        tx.vout[2] = OwnerOutput(TIP_AMOUNT - 1, relayer_owner);
        tx.vout[3] = OwnerOutput(change_amount + 1, user.script);
    });
    mutation_fails("redirected tip", [&](CMutableTransaction& tx) {
        tx.vout[2] = OwnerOutput(TIP_AMOUNT, other_owner);
    });
    mutation_fails("changed change amount", [&](CMutableTransaction& tx) {
        tx.vout[3] = OwnerOutput(change_amount - 1, user.script);
    });
    mutation_fails("redirected change", [&](CMutableTransaction& tx) {
        tx.vout[3] = OwnerOutput(change_amount, other_owner);
    });
    mutation_fails("removed tip", [&](CMutableTransaction& tx) {
        tx.vout.erase(tx.vout.begin() + 2);
    });
    mutation_fails("added native change output", [&](CMutableTransaction& tx) {
        tx.vout.emplace_back(FEE_COIN_VALUE / 2, relayer_owner);
    });
    mutation_fails("changed burn amount", [&](CMutableTransaction& tx) {
        const auto burn{modern::MakeAssetBurnOutput(TestAsset(ASSET_TAG),
                                                    BURN_AMOUNT - 1)};
        BOOST_REQUIRE(burn);
        tx.vout[0] = *burn;
    });
    mutation_fails("changed nLockTime", [&](CMutableTransaction& tx) {
        tx.nLockTime = 1;
    });
    mutation_fails("changed version", [&](CMutableTransaction& tx) {
        tx.version = 3;
    });
    mutation_fails("changed user nSequence", [&](CMutableTransaction& tx) {
        tx.vin[0].nSequence = CTxIn::SEQUENCE_FINAL;
    });

    // A new recipient needs a new record and binding; the binding output is
    // signed, so the consistent redirected transaction fails the signature.
    {
        CMutableTransaction redirected{completed};
        SetRecord(redirected, BurnRecord(0x33, exit));
        BOOST_CHECK(BindingHolds(redirected));
        BOOST_CHECK(!UserInputVerifies(redirected, spent));
    }
    if (exit) {
        // Route (d3): the exit block is part of the record as well.
        modern::ExitForwarderParamsV2 other_exit{*exit};
        ++other_exit.dst_eid;
        CMutableTransaction other_route{completed};
        SetRecord(other_route, BurnRecord(0x22, other_exit));
        BOOST_CHECK(BindingHolds(other_route));
        BOOST_CHECK(!UserInputVerifies(other_route, spent));
    }
    {
        // The record alone sits outside the sighash, which is exactly why the
        // binding exists: swapping only the record leaves the signature valid
        // but breaks the exact record binding consensus requires.
        CMutableTransaction record_only{completed};
        record_only.mpa = {BurnRecord(0x33, exit)};
        BOOST_CHECK(UserInputVerifies(record_only, spent));
        BOOST_CHECK(!BindingHolds(record_only));
    }

    // A plain ALL signature commits the input set, so it cannot be sponsored
    // after the fact; it still satisfies both bridge rules unextended.
    {
        CMutableTransaction plain{
            SponsoredBurn(user_coin, relayer_owner, user.script, exit)};
        BOOST_REQUIRE(SignInput(user.provider, plain, 0, {user_previous},
                                SIGHASH_ALL,
                                AssetSigningContext::OWNER_SUFFIX));
        BOOST_CHECK(UserInputVerifies(plain, {user_previous}));
        BOOST_CHECK(VerifyInput(plain, 0, {user_previous}, MANAGED_FLAGS).first);
        plain.vin.emplace_back(fee_coin);
        BOOST_CHECK(!UserInputVerifies(plain, spent));
    }

    // NONE|ACP and SINGLE|ACP leave outputs unsigned. Ordinary script rules
    // accept them, and the binding (with it the recipient) or the tip could
    // then be changed freely; the R10 rule refuses both bytes.
    for (const unsigned char weak : {ACP_NONE, ACP_SINGLE}) {
        BOOST_TEST_CONTEXT("hash type " << int{weak})
        {
            CMutableTransaction signed_weak{
                SponsoredBurn(user_coin, relayer_owner, user.script, exit)};
            BOOST_REQUIRE(SignInput(user.provider, signed_weak, 0,
                                    {user_previous}, weak,
                                    AssetSigningContext::OWNER_SUFFIX));
            BOOST_CHECK(VerifyInput(signed_weak, 0, {user_previous},
                                    STANDARD_SCRIPT_VERIFY_FLAGS)
                            .first);
            const auto [ok, error]{VerifyInput(signed_weak, 0, {user_previous},
                                               SPONSORED_FLAGS)};
            BOOST_CHECK(!ok);
            BOOST_CHECK_EQUAL(error, hashtype_error);

            CMutableTransaction exploited{signed_weak};
            SetRecord(exploited, BurnRecord(0x33, exit));
            exploited.vout[2] = OwnerOutput(TIP_AMOUNT, other_owner);
            BOOST_CHECK(BindingHolds(exploited));
            BOOST_CHECK(VerifyInput(exploited, 0, {user_previous},
                                    STANDARD_SCRIPT_VERIFY_FLAGS)
                            .first);
        }
    }
}

uint256 Fill(const unsigned char b)
{
    uint256 out;
    std::fill(out.begin(), out.end(), b);
    return out;
}

Consensus::BridgeEthAddress FillAddress(const unsigned char b)
{
    Consensus::BridgeEthAddress out{};
    out.fill(b);
    return out;
}

//! As bridge_v2_tests: light-client pins of a regtest-shaped envelope.
Consensus::EthereumLightClientPins LightClient(const uint256& gvr)
{
    Consensus::EthereumLightClientPins light;
    light.trusted_checkpoint_root = Fill(0x08);
    light.trusted_checkpoint_slot = 8'192;
    light.genesis_validators_root = gvr;
    light.fork_schedule = {
        {0, {0x00, 0x00, 0x00, 0xb3}},
        {10, {0x01, 0x00, 0x00, 0xb3}},
    };
    light.fork_schedule_valid_through_epoch = 1'024;
    light.electra_epoch = 10;
    light.min_sync_committee_participants =
        Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY;
    light.max_sync_lag_slots = 65'536;
    return light;
}

//! As bridge_v2_tests: a complete regtest-shaped envelope with V = 400, plus
//! the R10 sponsored sighash pin at V.
Consensus::BridgeV2Params RegtestV2Params()
{
    Consensus::BridgeV2Params out;
    out.activation_height = 400;
    out.burn_height = 400;
    out.sponsored_sighash_height = 400;
    out.light_client = LightClient(Fill(0x09));
    out.verifier.address = FillAddress(0x66);
    out.verifier.min_bridge_validators = 4;
    out.verifier.max_bridge_validators = 64;
    out.verifier.min_bridge_total_weight = 4;
    out.vault.origin_chain_id = 31'337;
    out.vault.vault_address = FillAddress(0x44);
    out.vault.token_address = FillAddress(0x55);
    out.vault.token_runtime_code_hash = Fill(0x0a);
    out.vault.origin_deployment_block = 3;
    out.vault.max_deposit_raw = Consensus::BRIDGE_V2_DEFAULT_CAP_RAW;
    out.vault.mint_caps = Consensus::BridgeMintCaps{
        .max_per_block = Consensus::BRIDGE_V2_DEFAULT_CAP_RAW,
        .max_per_epoch = Consensus::BRIDGE_V2_DEFAULT_CAP_RAW,
        .epoch_length_blocks = 1'440,
    };
    return out;
}

Consensus::Params ConsensusWith(
    const std::optional<Consensus::BridgeV2Params>& v2)
{
    Consensus::Params out;
    out.hashGenesisBlock = Fill(0x01);
    out.legacy_final_hash = Fill(0x02);
    out.bridge_v2 = v2;
    return out;
}

node::BridgeTxAuthorization BurnAuthorization(const bool sweep)
{
    node::BridgeTxAuthorization authorization;
    authorization.withdrawal_v2 = node::BridgeWithdrawalV2Request{};
    authorization.withdrawal_v2->sweep = sweep;
    return authorization;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(bridge_sponsored_fee_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(bridge_sighash_flags_ecdsa)
{
    CKey key;
    key.MakeNewKey(true);
    std::vector<unsigned char> der;
    BOOST_REQUIRE(key.Sign(uint256::ONE, der));

    const auto check = [&](const script_verify_flags flags,
                           const unsigned char hash_type,
                           const bool expected) {
        std::vector<unsigned char> signature{der};
        signature.push_back(hash_type);
        ScriptError error{SCRIPT_ERR_OK};
        const bool ok{CheckSignatureEncoding(signature, flags, &error)};
        BOOST_CHECK_MESSAGE(ok == expected,
                            "hash type " << int{hash_type} << " under flags "
                                         << flags.as_int());
        if (!expected) BOOST_CHECK_EQUAL(error, SCRIPT_ERR_SIG_HASHTYPE);
    };

    for (const BridgeFlagCase& bridge : BridgeFlagCases()) {
        BOOST_TEST_CONTEXT(bridge.name)
        {
            for (const unsigned char hash_type : DEFINED_HASH_TYPES) {
                check(SCRIPT_VERIFY_STRICTENC | bridge.flags, hash_type,
                      bridge.accepted.contains(hash_type));
            }
            for (const unsigned char hash_type : UNDEFINED_HASH_TYPES) {
                check(SCRIPT_VERIFY_STRICTENC | bridge.flags, hash_type,
                      false);
            }

            // Frozen legacy-lock inputs keep the historical profile, which
            // admits undefined bytes; either bridge flag still restricts it.
            const bool restricted{bridge.flags != SCRIPT_VERIFY_NONE};
            for (const unsigned char hash_type : DEFINED_HASH_TYPES) {
                check(SCRIPT_VERIFY_LEGACY_B3_STRICTENC | bridge.flags,
                      hash_type, bridge.accepted.contains(hash_type));
            }
            for (const unsigned char hash_type : UNDEFINED_HASH_TYPES) {
                check(SCRIPT_VERIFY_LEGACY_B3_STRICTENC | bridge.flags,
                      hash_type, !restricted);
            }
        }
    }

    // An empty signature stays the compact invalid signature under every
    // bridge flag.
    for (const BridgeFlagCase& bridge : BridgeFlagCases()) {
        ScriptError error{SCRIPT_ERR_OK};
        BOOST_CHECK(CheckSignatureEncoding(
            {}, SCRIPT_VERIFY_STRICTENC | bridge.flags, &error));
    }
}

BOOST_AUTO_TEST_CASE(bridge_sighash_flags_schnorr)
{
    const CScript taproot{
        CScript{} << OP_1 << std::vector<unsigned char>(32, 0x42)};
    const AcceptingSchnorrChecker checker;
    const auto verify = [&](const std::vector<unsigned char>& signature,
                            const script_verify_flags bridge_flags) {
        CScriptWitness witness;
        witness.stack = {signature};
        ScriptError error{SCRIPT_ERR_UNKNOWN_ERROR};
        const bool ok{VerifyScript(
            CScript{}, taproot, &witness,
            SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_TAPROOT |
                bridge_flags,
            checker, &error)};
        return std::pair{ok, error};
    };

    for (const BridgeFlagCase& bridge : BridgeFlagCases()) {
        BOOST_TEST_CONTEXT(bridge.name)
        {
            // A 64-byte signature is SIGHASH_DEFAULT, equivalent to ALL.
            BOOST_CHECK(verify(std::vector<unsigned char>(64, 0x31),
                               bridge.flags)
                            .first);
            for (const unsigned char hash_type : DEFINED_HASH_TYPES) {
                std::vector<unsigned char> signature(65, 0x32);
                signature.back() = hash_type;
                const auto [ok, error]{verify(signature, bridge.flags)};
                const bool expected{bridge.accepted.contains(hash_type)};
                BOOST_CHECK_MESSAGE(ok == expected,
                                    "hash type " << int{hash_type});
                if (!expected) {
                    BOOST_CHECK_EQUAL(error, SCRIPT_ERR_SCHNORR_SIG_HASHTYPE);
                }
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(owner_all_anyonecanpay_commits_every_output)
{
    for (const OwnerType type :
         {OwnerType::P2PKH, OwnerType::P2WPKH, OwnerType::P2TR}) {
        for (const bool d3 : {false, true}) {
            BOOST_TEST_CONTEXT(OwnerTypeName(type)
                               << (d3 ? " route d3" : " route d1"))
            {
                CheckOwnerAllAnyoneCanPay(type, d3);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(sponsored_sighash_gate_regtest_only)
{
    constexpr int V{400};
    constexpr int MAX_HEIGHT{std::numeric_limits<int>::max()};
    const Consensus::Params params{ConsensusWith(RegtestV2Params())};
    BOOST_REQUIRE(params.BridgeV2ConfigurationValid());

    // A kind 6 burn leaf: from V only.
    const node::BridgeTxAuthorization burn{BurnAuthorization(/*sweep=*/false)};
    BOOST_CHECK(!node::BridgeV2SponsoredSighashAllowed(burn, params, 0));
    BOOST_CHECK(!node::BridgeV2SponsoredSighashAllowed(burn, params, V - 1));
    for (const int height : {V, V + 1, MAX_HEIGHT}) {
        BOOST_CHECK_MESSAGE(
            node::BridgeV2SponsoredSighashAllowed(burn, params, height),
            "height " << height);
        BOOST_CHECK(node::BridgeV2SponsoredSighashAllowed(
            *burn.withdrawal_v2, params, height));
    }
    BOOST_CHECK(!node::BridgeV2SponsoredSighashAllowed(*burn.withdrawal_v2,
                                                       params, V - 1));

    // Never a kind 16 sweep, a V1 withdrawal, a mint or an empty result,
    // and never a V2 leaf with any other bridge result beside it.
    const node::BridgeTxAuthorization sweep{BurnAuthorization(/*sweep=*/true)};
    node::BridgeTxAuthorization managed;
    managed.withdrawal = node::BridgeManagedWithdrawalRequest{};
    node::BridgeTxAuthorization decentralized;
    decentralized.decentralized_withdrawal =
        node::BridgeDecentralizedWithdrawalRequest{};
    node::BridgeTxAuthorization mint;
    mint.mint = node::BridgeTxMintAuthorization{};
    const node::BridgeTxAuthorization empty;
    node::BridgeTxAuthorization burn_and_mint{burn};
    burn_and_mint.mint = node::BridgeTxMintAuthorization{};
    node::BridgeTxAuthorization burn_and_managed{burn};
    burn_and_managed.withdrawal = node::BridgeManagedWithdrawalRequest{};
    node::BridgeTxAuthorization burn_and_decentralized{burn};
    burn_and_decentralized.decentralized_withdrawal =
        node::BridgeDecentralizedWithdrawalRequest{};
    for (const int height : {0, V - 1, V, V + 1, MAX_HEIGHT}) {
        BOOST_TEST_CONTEXT("height " << height)
        {
            BOOST_CHECK(!node::BridgeV2SponsoredSighashAllowed(sweep, params,
                                                               height));
            BOOST_CHECK(!node::BridgeV2SponsoredSighashAllowed(
                *sweep.withdrawal_v2, params, height));
            BOOST_CHECK(!node::BridgeV2SponsoredSighashAllowed(managed, params,
                                                               height));
            BOOST_CHECK(!node::BridgeV2SponsoredSighashAllowed(
                decentralized, params, height));
            BOOST_CHECK(!node::BridgeV2SponsoredSighashAllowed(mint, params,
                                                               height));
            BOOST_CHECK(!node::BridgeV2SponsoredSighashAllowed(empty, params,
                                                               height));
            BOOST_CHECK(!node::BridgeV2SponsoredSighashAllowed(
                burn_and_mint, params, height));
            BOOST_CHECK(!node::BridgeV2SponsoredSighashAllowed(
                burn_and_managed, params, height));
            BOOST_CHECK(!node::BridgeV2SponsoredSighashAllowed(
                burn_and_decentralized, params, height));
        }
    }

    // An incomplete envelope never activates the rule, at any height.
    Consensus::BridgeV2Params incomplete{RegtestV2Params()};
    incomplete.vault.token_runtime_code_hash = uint256{};
    const Consensus::Params bad{ConsensusWith(incomplete)};
    BOOST_REQUIRE(!bad.BridgeV2ConfigurationValid());
    const Consensus::Params unset{ConsensusWith(std::nullopt)};
    for (const int height : {0, V - 1, V, V + 1, MAX_HEIGHT}) {
        BOOST_CHECK(!node::BridgeV2SponsoredSighashAllowed(burn, bad, height));
        BOOST_CHECK(
            !node::BridgeV2SponsoredSighashAllowed(burn, unset, height));
    }

    // The rule has a pin of its own. A complete envelope without it never
    // selects the rule, though V2 burns are active: pinning bridge V2 alone
    // does not turn R10's sighash relaxation on.
    Consensus::BridgeV2Params unpinned{RegtestV2Params()};
    unpinned.sponsored_sighash_height.reset();
    const Consensus::Params without_pin{ConsensusWith(unpinned)};
    BOOST_REQUIRE(without_pin.BridgeV2ConfigurationValid());
    for (const int height : {0, V - 1, V, V + 1, MAX_HEIGHT}) {
        BOOST_TEST_CONTEXT("no pin, height " << height)
        {
            BOOST_CHECK_EQUAL(without_pin.BridgeV2BurnRulesActive(height),
                              height >= V);
            BOOST_CHECK(!without_pin.BridgeV2SponsoredSighashActive(height));
            BOOST_CHECK(!node::BridgeV2SponsoredSighashAllowed(
                burn, without_pin, height));
            BOOST_CHECK(!node::BridgeV2SponsoredSighashAllowed(
                *burn.withdrawal_v2, without_pin, height));
        }
    }
    // A pin above V: V2 burns from V, the sponsored form only from the pin.
    constexpr int PIN{V + 10};
    Consensus::BridgeV2Params later{RegtestV2Params()};
    later.sponsored_sighash_height = PIN;
    const Consensus::Params pinned_later{ConsensusWith(later)};
    BOOST_REQUIRE(pinned_later.BridgeV2ConfigurationValid());
    for (const int height : {V - 1, V, PIN - 1, PIN, PIN + 1, MAX_HEIGHT}) {
        BOOST_TEST_CONTEXT("pin " << PIN << ", height " << height)
        {
            BOOST_CHECK_EQUAL(pinned_later.BridgeV2SponsoredSighashActive(height),
                              height >= PIN);
            BOOST_CHECK_EQUAL(node::BridgeV2SponsoredSighashAllowed(
                                  burn, pinned_later, height),
                              height >= PIN);
            BOOST_CHECK(!node::BridgeV2SponsoredSighashAllowed(
                sweep, pinned_later, height));
        }
    }
    // A pin below V is an incomplete envelope: never active.
    Consensus::BridgeV2Params early{RegtestV2Params()};
    early.sponsored_sighash_height = V - 1;
    BOOST_CHECK(!early.Valid());
    const Consensus::Params pinned_early{ConsensusWith(early)};
    BOOST_REQUIRE(!pinned_early.BridgeV2ConfigurationValid());
    for (const int height : {0, V - 1, V, MAX_HEIGHT}) {
        BOOST_CHECK(!pinned_early.BridgeV2SponsoredSighashActive(height));
        BOOST_CHECK(
            !node::BridgeV2SponsoredSighashAllowed(burn, pinned_early, height));
    }

    // Every shipped network leaves bridge_v2 unset, so the sponsored form is
    // never selected there; nor on regtest without the -b3bridgepins file.
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET,
                                  ChainType::TESTNET4, ChainType::SIGNET,
                                  ChainType::REGTEST}) {
        BOOST_TEST_CONTEXT(ChainTypeToString(chain))
        {
            const auto chain_params{CreateChainParams(ArgsManager{}, chain)};
            const Consensus::Params& consensus{chain_params->GetConsensus()};
            BOOST_CHECK(!consensus.bridge_v2);
            const int after_h{
                Consensus::LegacyFinalHeight(consensus).value_or(0) + 1};
            for (const int height : {0, 1, after_h, 1'000'000, MAX_HEIGHT}) {
                BOOST_CHECK_MESSAGE(!node::BridgeV2SponsoredSighashAllowed(
                                        burn, consensus, height),
                                    "height " << height);
                BOOST_CHECK(!consensus.BridgeV2SponsoredSighashActive(height));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(check_input_scripts_selects_sponsored_rule)
{
    // validation.cpp selects the R10 form of the bridge rule by passing
    // SCRIPT_VERIFY_BRIDGE_SIGHASH_ALL_OUTPUTS in `flags` together with
    // require_bridge_sighash_all; without the flag the bridge rule is the
    // managed-V1 (and mainnet) SIGHASH_ALL rule.
    constexpr int LEGACY_FINAL_HEIGHT{100};
    CKey user_key;
    user_key.MakeNewKey(true);
    CKey relayer_key;
    relayer_key.MakeNewKey(true);
    CKey legacy_key;
    legacy_key.MakeNewKey(true);
    const Owner user{MakeOwner(user_key, OwnerType::P2PKH)};
    const CScript relayer_owner{
        GetScriptForDestination(PKHash{relayer_key.GetPubKey()})};
    const CScript legacy_owner{
        GetScriptForDestination(PKHash{legacy_key.GetPubKey()})};
    FillableSigningProvider relayer;
    BOOST_REQUIRE(relayer.AddKey(relayer_key));
    BOOST_REQUIRE(relayer.AddKey(legacy_key));

    const COutPoint user_coin{Txid::FromUint256(uint256::ONE), 0};
    const COutPoint fee_coin{Txid::FromUint256(uint256{2}), 1};
    const COutPoint legacy_coin{Txid::FromUint256(uint256{3}), 2};
    const CTxOut user_previous{OwnerOutput(COIN_AMOUNT, user.script)};
    const CTxOut fee_previous{FEE_COIN_VALUE, relayer_owner};
    const CTxOut legacy_previous{FEE_COIN_VALUE, legacy_owner};
    CCoinsView base;
    CCoinsViewCache inputs{&base};
    // Post-H: the bUSD owner coin and a native fee coin. At H: a sealed-era
    // native coin, checked under the frozen legacy flags.
    inputs.AddCoin(user_coin, Coin{user_previous, LEGACY_FINAL_HEIGHT + 1, false},
                   /*possible_overwrite=*/false);
    inputs.AddCoin(fee_coin, Coin{fee_previous, LEGACY_FINAL_HEIGHT + 1, false},
                   /*possible_overwrite=*/false);
    inputs.AddCoin(legacy_coin, Coin{legacy_previous, LEGACY_FINAL_HEIGHT, false},
                   /*possible_overwrite=*/false);
    // Frozen flags without STRICTENC: the bridge rule still applies there.
    constexpr script_verify_flags LEGACY_FLAGS{SCRIPT_VERIFY_P2SH};
    const LegacyLockSpendContext lock_context{LEGACY_FINAL_HEIGHT, LEGACY_FLAGS};
    ValidationCache validation_cache{1U << 20, 1U << 20};
    LOCK(cs_main);

    const auto check = [&](const CMutableTransaction& spend,
                           const script_verify_flags flags, const bool require)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main) {
        const CTransaction tx{spend};
        PrecomputedTransactionData txdata;
        TxValidationState state;
        return CheckInputScripts(tx, state, inputs, flags,
                                 /*cacheSigStore=*/true,
                                 /*cacheFullScriptStore=*/true, txdata,
                                 validation_cache, /*pvChecks=*/nullptr,
                                 lock_context, require);
    };
    constexpr script_verify_flags STANDARD{STANDARD_SCRIPT_VERIFY_FLAGS};
    const auto user_signed = [&](const int hash_type) {
        CMutableTransaction partial{SponsoredBurn(
            user_coin, relayer_owner, user.script, std::nullopt)};
        BOOST_REQUIRE(SignInput(user.provider, partial, 0, {user_previous},
                                hash_type, AssetSigningContext::OWNER_SUFFIX));
        return partial;
    };
    //! The partial plus one relayer input, signed `hash_type` alone.
    const auto completed_with = [&](CMutableTransaction partial,
                                    const COutPoint& coin,
                                    const CTxOut& previous,
                                    const int hash_type) {
        partial.vin.emplace_back(coin);
        BOOST_REQUIRE(SignInput(relayer, partial, 1, {user_previous, previous},
                                hash_type, AssetSigningContext::FULL_SCRIPT));
        return partial;
    };

    // User ALL|ANYONECANPAY plus a post-H fee input signed ALL: accepted only
    // with the R10 flag. The first, cached, approval never carries over to
    // the managed-V1 rule, which keys the cache with a different flag.
    const CMutableTransaction sponsored{completed_with(
        user_signed(ACP_ALL), fee_coin, fee_previous, SIGHASH_ALL)};
    BOOST_CHECK(check(sponsored, SPONSORED_FLAGS, /*require=*/true));
    BOOST_CHECK(!check(sponsored, STANDARD, /*require=*/true));
    BOOST_CHECK(check(sponsored, SPONSORED_FLAGS, /*require=*/true));
    BOOST_CHECK(!check(sponsored, STANDARD, /*require=*/true));
    // Not a bridge withdrawal: the ordinary rules accept it either way.
    BOOST_CHECK(check(sponsored, STANDARD, /*require=*/false));
    // Both bridge flags at once admit SIGHASH_ALL only.
    BOOST_CHECK(!check(sponsored,
                       SPONSORED_FLAGS | SCRIPT_VERIFY_BRIDGE_SIGHASH_ALL,
                       /*require=*/true));
    // The user input pays no native value, so the relayer's input can even
    // be signed ALL|ANYONECANPAY; NONE and SINGLE stay refused for it too.
    BOOST_CHECK(check(completed_with(user_signed(ACP_ALL), fee_coin,
                                     fee_previous, ACP_ALL),
                      SPONSORED_FLAGS, /*require=*/true));
    for (const int weak : {int{SIGHASH_NONE}, int{SIGHASH_SINGLE}, int{ACP_NONE},
                           int{ACP_SINGLE}}) {
        BOOST_TEST_CONTEXT("relayer hash type " << weak)
        {
            const CMutableTransaction relayer_weak{completed_with(
                user_signed(ACP_ALL), fee_coin, fee_previous, weak)};
            BOOST_CHECK(!check(relayer_weak, SPONSORED_FLAGS, /*require=*/true));
            BOOST_CHECK(check(relayer_weak, STANDARD, /*require=*/false));
        }
    }

    // A plain ALL user signature: valid alone under both bridge rules, but
    // it committed the input set, so no input can be added.
    const CMutableTransaction plain{user_signed(SIGHASH_ALL)};
    BOOST_CHECK(check(plain, SPONSORED_FLAGS, /*require=*/true));
    BOOST_CHECK(check(plain, STANDARD, /*require=*/true));
    const CMutableTransaction plain_extended{
        completed_with(plain, fee_coin, fee_previous, SIGHASH_ALL)};
    BOOST_CHECK(!check(plain_extended, SPONSORED_FLAGS, /*require=*/true));
    BOOST_CHECK(!check(plain_extended, STANDARD, /*require=*/true));
    BOOST_CHECK(!check(plain_extended, STANDARD, /*require=*/false));

    // NONE|ACP and SINGLE|ACP user signatures leave outputs unsigned: valid
    // under ordinary rules even with the fee input added, refused by both
    // bridge rules.
    for (const int weak : {int{ACP_NONE}, int{ACP_SINGLE}}) {
        BOOST_TEST_CONTEXT("user hash type " << weak)
        {
            const CMutableTransaction user_weak{completed_with(
                user_signed(weak), fee_coin, fee_previous, SIGHASH_ALL)};
            BOOST_CHECK(check(user_weak, STANDARD, /*require=*/false));
            BOOST_CHECK(!check(user_weak, SPONSORED_FLAGS, /*require=*/true));
            BOOST_CHECK(!check(user_weak, STANDARD, /*require=*/true));
        }
    }

    // A sealed-era fee input keeps its frozen flags, and the selected bridge
    // rule rides along: ALL and ALL|ANYONECANPAY pass under the R10 form,
    // SINGLE (valid under the frozen flags alone) is refused.
    for (const int hash_type : {int{SIGHASH_ALL}, int{ACP_ALL}}) {
        BOOST_TEST_CONTEXT("legacy hash type " << hash_type)
        {
            const CMutableTransaction legacy_fee{completed_with(
                user_signed(ACP_ALL), legacy_coin, legacy_previous, hash_type)};
            BOOST_CHECK(check(legacy_fee, SPONSORED_FLAGS, /*require=*/true));
            BOOST_CHECK(!check(legacy_fee, STANDARD, /*require=*/true));
        }
    }
    {
        // Isolate the frozen input: the user signs SIGHASH_ALL over both
        // inputs, so only the legacy input's ALL|ANYONECANPAY decides. The
        // managed-V1 rule reaches the frozen input and refuses it; the R10
        // form admits it.
        CMutableTransaction both{SponsoredBurn(user_coin, relayer_owner,
                                               user.script, std::nullopt)};
        both.vin.emplace_back(legacy_coin);
        const std::vector<CTxOut> spent{user_previous, legacy_previous};
        BOOST_REQUIRE(SignInput(relayer, both, 1, spent, ACP_ALL,
                                AssetSigningContext::FULL_SCRIPT));
        BOOST_REQUIRE(SignInput(user.provider, both, 0, spent, SIGHASH_ALL,
                                AssetSigningContext::OWNER_SUFFIX));
        BOOST_CHECK(check(both, STANDARD, /*require=*/false));
        BOOST_CHECK(check(both, SPONSORED_FLAGS, /*require=*/true));
        BOOST_CHECK(!check(both, STANDARD, /*require=*/true));
    }
    const CMutableTransaction legacy_single{completed_with(
        user_signed(ACP_ALL), legacy_coin, legacy_previous, SIGHASH_SINGLE)};
    BOOST_CHECK(check(legacy_single, STANDARD, /*require=*/false));
    // The flag alone selects the R10 form, with or without
    // require_bridge_sighash_all, so the cache key (which commits the flags)
    // fully determines the rule: the frozen input is bound either way, and
    // no approval cached under one call can carry over to the other.
    BOOST_CHECK(!check(legacy_single, SPONSORED_FLAGS, /*require=*/false));
    BOOST_CHECK(!check(legacy_single, SPONSORED_FLAGS, /*require=*/true));
    BOOST_CHECK(!check(legacy_single, SPONSORED_FLAGS, /*require=*/false));
    for (const int hash_type : {int{SIGHASH_ALL}, int{ACP_ALL}}) {
        const CMutableTransaction legacy_fee{completed_with(
            user_signed(ACP_ALL), legacy_coin, legacy_previous, hash_type)};
        BOOST_CHECK(check(legacy_fee, SPONSORED_FLAGS, /*require=*/false));
    }
}

BOOST_AUTO_TEST_SUITE_END()
