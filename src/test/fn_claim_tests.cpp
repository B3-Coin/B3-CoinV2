// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <modern/fn.h>

#include <crypto/sha256.h>
#include <hash.h>
#include <key.h>
#include <modern/policy.h>
#include <modern/proof.h>
#include <pubkey.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace modern;

namespace {

std::string Repeat(const std::string& unit, const int times)
{
    std::string out;
    for (int i{0}; i < times; ++i) out += unit;
    return out;
}

// ---- Deterministic vector components (all literal, encoder-independent).
// PoDId raw bytes exactly as they appear in policy_params.
const std::string POD_HEX{"0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"};
// The secp256k1 generator point: a well-known, definitely-valid key.
const std::string PUB_C_HEX{"0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"};
const std::string PUB_U_HEX{
    "0479be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
    "483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8"};
// Handcrafted strict-DER low-S signature (70 bytes; r=0x11…, s=0x22…).
const std::string SIG_HEX{"30440220" + Repeat("11", 32) + "0220" + Repeat("22", 32)};
// Valid strict-DER but HIGH-S signature (71 bytes; s=0x00ee…).
const std::string SIG_HIGH_S_HEX{"30450220" + Repeat("11", 32) + "022100" + Repeat("ee", 32)};

// Authorization record bytes (index 0).
const std::string AUTH_P2PKH_HEX{"00" "01" "21" + PUB_C_HEX + "46" + SIG_HEX};
const std::string AUTH_P2PK_HEX{"00" "02" "46" + SIG_HEX};

// Minimal claim-action payload vectors: fn_output_index 0, one record.
// payload = compact(0) compact(1) compact(record_len) record
const std::string ACTION_P2PKH_PAYLOAD_HEX{"00" "01" "6b" + AUTH_P2PKH_HEX};
const std::string ACTION_P2PK_PAYLOAD_HEX{"00" "01" "49" + AUTH_P2PK_HEX};

std::vector<unsigned char> FromHex(const std::string& hex)
{
    auto parsed{TryParseHex<unsigned char>(hex)};
    BOOST_REQUIRE(parsed.has_value());
    return *parsed;
}

Txid PodId()
{
    uint256 raw;
    const auto bytes{FromHex(POD_HEX)};
    std::copy(bytes.begin(), bytes.end(), raw.begin());
    return Txid::FromUint256(raw);
}

const uint256 OWNER_COMMITMENT{"00000000000000000000000000000000000000000000000000000000000000c7"};

// ---- The corrected FN model (owner ruling 2026-08-18): a synthetic
// chain domain and the ONE global FN asset id derived from it. The
// derivation (tag, preimage, byte order) is pinned by literal vectors in
// the fn_asset_identity test case below.
const uint256 TEST_GENESIS{"1111111111111111111111111111111111111111111111111111111111111111"};
const uint256 TEST_X{uint256::ONE};
// Pinned derivation vectors for the synthetic (TEST_GENESIS, TEST_X)
// domain — filled from the first computed values and frozen since.
const std::string TEST_DOMAIN_HEX{"531336c7b81523168c60b8ddadae01d32e6f9733a9ca9de82534ca647f91ae14"};
const std::string TEST_FN_ASSET_HEX{"ce7761a4dd646c10aacaaeba341006b9298824efca3547b4bcd79c6ee41e5326"};

uint256 TestDomain()
{
    const auto domain{ModernChainDomain(TEST_GENESIS, TEST_X)};
    BOOST_REQUIRE(domain);
    return *domain;
}

AssetId TestFnAsset() { return FnAssetId(TestDomain()); }

FnAuthorization P2pkhAuth(const uint32_t index)
{
    FnAuthorization auth;
    auth.funding_script_index = index;
    auth.form = FnAuthForm::P2PKH;
    auth.pubkey = FromHex(PUB_C_HEX);
    auth.signature = FromHex(SIG_HEX);
    return auth;
}

FnAuthorization P2pkAuth(const uint32_t index)
{
    FnAuthorization auth;
    auth.funding_script_index = index;
    auth.form = FnAuthForm::P2PK;
    auth.signature = FromHex(SIG_HEX);
    return auth;
}

FnOutputView View(const CAmount amount = 3)
{
    return FnOutputView{.amount = amount, .owner_commitment = OWNER_COMMITMENT};
}

CreationAction ClaimAction(const uint32_t output_index,
                           std::vector<FnAuthorization> auths = {})
{
    if (auths.empty()) auths = {P2pkAuth(0)};
    FnClaimActionV1 action{.fn_output_index = output_index, .authorizations = std::move(auths)};
    const auto encoded{EncodeFnClaimAction(action)};
    BOOST_REQUIRE(encoded);
    return *encoded;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(fn_claim_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(policy_identity_pins)
{
    // The consensus-stable registry values, pinned. Type 1 is the
    // RESERVED/SUPERSEDED claim action of the abandoned funding-signature
    // design; type 2 is the live legacy FN issuance action.
    BOOST_CHECK_EQUAL(static_cast<uint16_t>(PolicyType::FN), 5);
    BOOST_CHECK_EQUAL(FN_POLICY_TYPE, 5);
    BOOST_CHECK_EQUAL(FN_POLICY_VERSION_V1, 1);
    BOOST_CHECK_EQUAL(CREATION_ACTION_FN_CLAIM, 1);
    BOOST_CHECK_EQUAL(FN_CLAIM_ACTION_VERSION_V1, 1);
    BOOST_CHECK_EQUAL(CREATION_ACTION_LEGACY_FN_ISSUANCE, 2);
    BOOST_CHECK_EQUAL(LEGACY_FN_ISSUANCE_ACTION_VERSION_V1, 1);
    BOOST_CHECK(IsKnownCreationAction(1, 1));
    BOOST_CHECK(IsKnownCreationAction(2, 1));
    BOOST_CHECK(!IsKnownCreationAction(2, 2));
    BOOST_CHECK(!IsKnownCreationAction(3, 1));
    BOOST_CHECK_EQUAL(MAX_CREATION_ACTION_PAYLOAD, 4000U);
    BOOST_CHECK_EQUAL(MAX_FN_EVER_ISSUED, 1000U);

    // FN v1 is INACTIVE on every network: the policy model itself fails
    // closed, with or without the test-only asset activation flag.
    BOOST_CHECK(!IsActivatedPolicy(FN_POLICY_TYPE, FN_POLICY_VERSION_V1, false));
    BOOST_CHECK(!IsActivatedPolicy(FN_POLICY_TYPE, FN_POLICY_VERSION_V1, true));
}

//! The ONE global FN asset identity: derivation pinned byte-exactly on
//! synthetic vectors (the mainnet value is pinned only after mainnet
//! H/X freezes the real chain domain).
BOOST_AUTO_TEST_CASE(fn_asset_identity)
{
    // The domain input is the fail-closed ModernChainDomain.
    const uint256 domain{TestDomain()};
    BOOST_CHECK_EQUAL(domain.GetHex(), TEST_DOMAIN_HEX);
    const AssetId asset{FnAssetId(domain)};
    BOOST_CHECK_EQUAL(asset.GetHex(), TEST_FN_ASSET_HEX);
    // Never the native asset; chain-scoped (a different domain gives a
    // different id); deterministic.
    BOOST_CHECK(asset != NativeAsset());
    BOOST_CHECK(FnAssetId(uint256::ONE) != asset);
    BOOST_CHECK(FnAssetId(domain) == asset);
}

//! Pure inactive supply/conservation model (owner ruling 2026-08-18):
//! cap, monotone issued-total, extinguishment never reopening capacity,
//! and whole-unit conservation.
BOOST_AUTO_TEST_CASE(fn_supply_model)
{
    // Cap: 999 → 1000 mints; at the cap, fresh issuance rejects.
    FnSupplyModel model{.issued_total = 999, .live_supply = 999};
    BOOST_CHECK(FnAuthorizeIssuance(model));
    BOOST_CHECK_EQUAL(model.issued_total, 1000U);
    BOOST_CHECK_EQUAL(model.live_supply, 1000U);
    BOOST_CHECK(!FnAuthorizeIssuance(model));
    // Extinguishment reduces live supply, never issued_total, and never
    // reopens capacity.
    BOOST_CHECK(FnExtinguish(model, 50));
    BOOST_CHECK_EQUAL(model.issued_total, 1000U);
    BOOST_CHECK_EQUAL(model.live_supply, 950U);
    BOOST_CHECK(!FnAuthorizeIssuance(model));
    BOOST_CHECK(!FnExtinguish(model, 951)); // more than live supply
    // MALFORMED model state is rejected outright, never operated on:
    // live_supply above issued_total, or issued_total above the cap.
    {
        FnSupplyModel corrupt{.issued_total = 5, .live_supply = 6};
        BOOST_CHECK(!FnSupplyModelValid(corrupt));
        BOOST_CHECK(!FnAuthorizeIssuance(corrupt));
        BOOST_CHECK(!FnExtinguish(corrupt, 1));
        FnSupplyModel over{.issued_total = MAX_FN_EVER_ISSUED + 1,
                           .live_supply = 0};
        BOOST_CHECK(!FnSupplyModelValid(over));
        BOOST_CHECK(!FnAuthorizeIssuance(over));
        BOOST_CHECK(!FnExtinguish(over, 0));
    }
    // Whole-unit conservation: 3 → 1 + 2 and 1 + 2 → 3 balance; a lost
    // or conjured unit does not. No fractional representation exists —
    // the model only speaks in integers.
    BOOST_CHECK(CheckFnUnitConservation(/*in=*/3, /*fresh=*/0, /*out=*/3, /*ext=*/0));
    BOOST_CHECK(CheckFnUnitConservation(/*in=*/0, /*fresh=*/1, /*out=*/1, /*ext=*/0));
    BOOST_CHECK(CheckFnUnitConservation(/*in=*/3, /*fresh=*/0, /*out=*/1, /*ext=*/2));
    BOOST_CHECK(!CheckFnUnitConservation(/*in=*/3, /*fresh=*/0, /*out=*/2, /*ext=*/0));
    BOOST_CHECK(!CheckFnUnitConservation(/*in=*/3, /*fresh=*/0, /*out=*/4, /*ext=*/0));
    BOOST_CHECK(!CheckFnUnitConservation(/*in=*/0, /*fresh=*/1, /*out=*/2, /*ext=*/0));
    // UINT64 wraparound attempts can never fabricate a balance: each
    // side is overflow-guarded BEFORE addition.
    constexpr uint64_t max64{std::numeric_limits<uint64_t>::max()};
    BOOST_CHECK(!CheckFnUnitConservation(/*in=*/max64, /*fresh=*/1, /*out=*/0, /*ext=*/0));
    BOOST_CHECK(!CheckFnUnitConservation(/*in=*/0, /*fresh=*/0, /*out=*/max64, /*ext=*/1));
    BOOST_CHECK(!CheckFnUnitConservation(/*in=*/max64, /*fresh=*/2, /*out=*/max64,
                                         /*ext=*/2)); // both sides would wrap equally
    BOOST_CHECK(CheckFnUnitConservation(/*in=*/max64, /*fresh=*/0, /*out=*/max64, /*ext=*/0));
}

BOOST_AUTO_TEST_CASE(fn_output_vectors)
{
    std::string error;
    const AssetId fn_asset{TestFnAsset()};

    // Canonical FN v1 output (corrected model): the global FN asset,
    // whole-unit amount, ownership commitment, EMPTY params — no PoDId
    // anywhere in an ordinary FN output. Build, byte-stable
    // serialization, parse.
    {
        const auto out{MakeFnOutput(View(), fn_asset)};
        BOOST_REQUIRE(out);
        BOOST_CHECK(out->asset == fn_asset);
        BOOST_CHECK(out->asset != NativeAsset());
        BOOST_CHECK_EQUAL(out->amount, 3);
        BOOST_CHECK_EQUAL(out->policy_type, FN_POLICY_TYPE);
        BOOST_CHECK_EQUAL(out->policy_version, FN_POLICY_VERSION_V1);
        BOOST_CHECK(out->policy_commitment == OWNER_COMMITMENT);
        BOOST_CHECK(out->policy_params.empty()); // no PoDId, no opaque bytes

        DataStream s1;
        s1 << *out;
        DataStream s2;
        s2 << *out;
        BOOST_CHECK(std::ranges::equal(s1, s2)); // deterministic bytes
        // The serialized identity of an ordinary FN output contains the
        // asset id, amount, FN policy and owner commitment — and no
        // persistent PoDId binding (POD_HEX appears nowhere).
        BOOST_CHECK(HexStr(s1).find(POD_HEX) == std::string::npos);
        ModernOutput decoded;
        s1 >> decoded;
        BOOST_CHECK(decoded == *out);

        const auto view{ParseFnOutput(*out, fn_asset, error)};
        BOOST_REQUIRE_MESSAGE(view, error);
        BOOST_CHECK(*view == View());
    }
    // A shared/threshold ownership commitment is structurally just
    // another 32-byte commitment controlling WHOLE units: one jointly
    // controlled FN, never fractional balances.
    {
        const uint256 joint_commitment{"aa000000000000000000000000000000000000000000000000000000000000bb"};
        const auto out{MakeFnOutput(FnOutputView{.amount = 1,
                                                 .owner_commitment = joint_commitment},
                                    fn_asset)};
        BOOST_REQUIRE(out);
        BOOST_CHECK_EQUAL(out->amount, 1);
        BOOST_CHECK(ParseFnOutput(*out, fn_asset, error).has_value());
    }
    // The FN structural rules of CheckPolicyOutput are pinned even though
    // the activation gate makes them unreachable: an FN v1 output at a
    // modern height is UNKNOWN_POLICY (inactive), never OK.
    {
        Consensus::Params params;
        params.hard_fork_height = 10;
        params.legacy_final_hash = uint256::ONE;
        const auto out{MakeFnOutput(View(), fn_asset)};
        BOOST_REQUIRE(out);
        BOOST_CHECK(CheckPolicyOutput(*out, /*height=*/50, params) ==
                    PolicyOutputCheck::UNKNOWN_POLICY);
    }
    // Build-side rejections.
    {
        BOOST_CHECK(!MakeFnOutput(View(), NativeAsset())); // native asset as FN identity
        FnOutputView bad{View()};
        bad.owner_commitment = uint256{};
        BOOST_CHECK(!MakeFnOutput(bad, fn_asset)); // null owner commitment
        bad = View();
        bad.amount = 0;
        BOOST_CHECK(!MakeFnOutput(bad, fn_asset)); // zero: no output represents zero balance
        bad = View();
        bad.amount = -1;
        BOOST_CHECK(!MakeFnOutput(bad, fn_asset)); // negative
        bad = View();
        bad.amount = static_cast<CAmount>(MAX_FN_EVER_ISSUED) + 1;
        BOOST_CHECK(!MakeFnOutput(bad, fn_asset)); // above the ever-issued cap
    }
    // Parse-side rejections; every mutation of a valid output fails.
    {
        const ModernOutput good{*MakeFnOutput(View(), fn_asset)};
        ModernOutput bad{good};
        bad.asset = NativeAsset();
        BOOST_CHECK(!ParseFnOutput(bad, fn_asset, error));
        BOOST_CHECK_EQUAL(error, "FN output must not carry the native asset");
        bad = good;
        bad.asset = uint256::ONE; // some other (non-FN) asset
        BOOST_CHECK(!ParseFnOutput(bad, fn_asset, error));
        BOOST_CHECK_EQUAL(error, "FN output asset is not the chain's FN asset id");
        bad = good;
        bad.policy_commitment = uint256{};
        BOOST_CHECK(!ParseFnOutput(bad, fn_asset, error));
        BOOST_CHECK_EQUAL(error, "FN owner commitment is null");
        bad = good;
        bad.policy_params.push_back(0x00); // opaque bytes are not accepted
        BOOST_CHECK(!ParseFnOutput(bad, fn_asset, error));
        BOOST_CHECK_EQUAL(error, "FN v1 params must be empty");
        bad = good;
        bad.amount = 0; // a live FN output holds at least one unit
        BOOST_CHECK(!ParseFnOutput(bad, fn_asset, error));
        BOOST_CHECK_EQUAL(error, "FN unit count outside [1, MAX_FN_EVER_ISSUED]");
        bad = good;
        bad.amount = -1;
        BOOST_CHECK(!ParseFnOutput(bad, fn_asset, error));
        BOOST_CHECK_EQUAL(error, "FN unit count outside [1, MAX_FN_EVER_ISSUED]");
        bad = good;
        bad.amount = static_cast<CAmount>(MAX_FN_EVER_ISSUED) + 1;
        BOOST_CHECK(!ParseFnOutput(bad, fn_asset, error));
        // A different policy type simply is not FN.
        bad = good;
        bad.policy_type = static_cast<uint16_t>(PolicyType::OWNER);
        BOOST_CHECK(!IsFnPolicyOutput(bad));
        // A different policy version is not FN v1 either.
        bad = good;
        bad.policy_version = 2;
        BOOST_CHECK(!IsFnPolicyOutput(bad));
    }
}

BOOST_AUTO_TEST_CASE(authorization_record_vectors)
{
    std::string error;

    // Byte-exact stable vectors and round trips, both forms.
    {
        const auto bytes{EncodeFnAuthorization(P2pkhAuth(0))};
        BOOST_REQUIRE(bytes);
        BOOST_CHECK_EQUAL(HexStr(*bytes), AUTH_P2PKH_HEX);
        FnAuthorization decoded;
        BOOST_REQUIRE_MESSAGE(DecodeFnAuthorization(*bytes, decoded, error), error);
        BOOST_CHECK(decoded == P2pkhAuth(0));
    }
    {
        const auto bytes{EncodeFnAuthorization(P2pkAuth(0))};
        BOOST_REQUIRE(bytes);
        BOOST_CHECK_EQUAL(HexStr(*bytes), AUTH_P2PK_HEX);
        FnAuthorization decoded;
        BOOST_REQUIRE_MESSAGE(DecodeFnAuthorization(*bytes, decoded, error), error);
        BOOST_CHECK(decoded == P2pkAuth(0));
        BOOST_CHECK(decoded.pubkey.empty()); // key derives from the funding script
    }
    // Uncompressed P2PKH key round-trips too.
    {
        FnAuthorization auth{P2pkhAuth(3)};
        auth.pubkey = FromHex(PUB_U_HEX);
        const auto bytes{EncodeFnAuthorization(auth)};
        BOOST_REQUIRE(bytes);
        FnAuthorization decoded;
        BOOST_REQUIRE_MESSAGE(DecodeFnAuthorization(*bytes, decoded, error), error);
        BOOST_CHECK(decoded == auth);
    }
    // Truncation at EVERY byte boundary fails.
    {
        const auto full{FromHex(AUTH_P2PKH_HEX)};
        for (size_t len{0}; len < full.size(); ++len) {
            FnAuthorization decoded;
            BOOST_CHECK_MESSAGE(
                !DecodeFnAuthorization(std::span{full.data(), len}, decoded, error),
                "truncated record of length " << len << " unexpectedly decoded");
        }
    }
    // Trailing byte after a complete record fails.
    {
        auto bytes{FromHex(AUTH_P2PK_HEX)};
        bytes.push_back(0x00);
        FnAuthorization decoded;
        BOOST_CHECK(!DecodeFnAuthorization(bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "trailing bytes after authorization");
    }
    // Unknown form byte.
    {
        const auto bytes{FromHex("00" "03" "46" + SIG_HEX)};
        FnAuthorization decoded;
        BOOST_CHECK(!DecodeFnAuthorization(bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "unknown authorization form");
    }
    // Non-canonical compact-size index (fd-encoding of 7).
    {
        const auto bytes{FromHex("fd0700" "02" "46" + SIG_HEX)};
        FnAuthorization decoded;
        BOOST_CHECK(!DecodeFnAuthorization(bytes, decoded, error));
    }
    // Wrong pubkey length (34 bytes).
    {
        const auto bytes{FromHex("00" "01" "22" + PUB_C_HEX + "aa" + "46" + SIG_HEX)};
        FnAuthorization decoded;
        BOOST_CHECK(!DecodeFnAuthorization(bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "public key has an invalid length");
    }
    // Correct length but not a curve point.
    {
        const auto bytes{FromHex("00" "01" "21" "02" + Repeat("ff", 32) + "46" + SIG_HEX)};
        FnAuthorization decoded;
        BOOST_CHECK(!DecodeFnAuthorization(bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "public key is not a valid curve point");
    }
    // Invalid DER framing (0x31 compound tag).
    {
        std::string bad_sig{SIG_HEX};
        bad_sig[1] = '1';
        const auto bytes{FromHex("00" "02" "46" + bad_sig)};
        FnAuthorization decoded;
        BOOST_CHECK(!DecodeFnAuthorization(bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "signature is not strict bare DER");
    }
    // Valid DER but high-S.
    {
        const auto bytes{FromHex("00" "02" "47" + SIG_HIGH_S_HEX)};
        FnAuthorization decoded;
        BOOST_CHECK(!DecodeFnAuthorization(bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "signature is not low-S");
    }
    // Encode-side rejections mirror the same rules.
    {
        FnAuthorization bad{P2pkAuth(0)};
        bad.pubkey = FromHex(PUB_C_HEX); // P2PK must not carry a key
        BOOST_CHECK(!EncodeFnAuthorization(bad));
        FnAuthorization high{P2pkAuth(0)};
        high.signature = FromHex(SIG_HIGH_S_HEX);
        BOOST_CHECK(!EncodeFnAuthorization(high));
    }
}

BOOST_AUTO_TEST_CASE(claim_action_vectors)
{
    std::string error;

    // Minimal valid P2PKH and P2PK claim actions: byte-exact payload
    // vectors, exact type/version, round trips.
    {
        const auto action{
            EncodeFnClaimAction({.fn_output_index = 0, .authorizations = {P2pkhAuth(0)}})};
        BOOST_REQUIRE(action);
        BOOST_CHECK_EQUAL(action->action_type, CREATION_ACTION_FN_CLAIM);
        BOOST_CHECK_EQUAL(action->action_version, FN_CLAIM_ACTION_VERSION_V1);
        BOOST_CHECK_EQUAL(HexStr(action->payload), ACTION_P2PKH_PAYLOAD_HEX);
        FnClaimActionV1 decoded;
        BOOST_REQUIRE_MESSAGE(DecodeFnClaimAction(*action, decoded, error), error);
        BOOST_CHECK_EQUAL(decoded.fn_output_index, 0U);
        BOOST_CHECK(decoded.authorizations == std::vector<FnAuthorization>{P2pkhAuth(0)});
    }
    {
        const auto action{
            EncodeFnClaimAction({.fn_output_index = 0, .authorizations = {P2pkAuth(0)}})};
        BOOST_REQUIRE(action);
        BOOST_CHECK_EQUAL(HexStr(action->payload), ACTION_P2PK_PAYLOAD_HEX);
    }
    // Multiple distinct funding scripts in canonical order, mixed forms.
    {
        FnClaimActionV1 action{.fn_output_index = 2,
                               .authorizations = {P2pkhAuth(0), P2pkAuth(1), P2pkhAuth(2)}};
        const auto encoded{EncodeFnClaimAction(action)};
        BOOST_REQUIRE(encoded);
        FnClaimActionV1 decoded;
        BOOST_REQUIRE_MESSAGE(DecodeFnClaimAction(*encoded, decoded, error), error);
        BOOST_CHECK(decoded == action);
        // Deterministic bytes on re-encode.
        BOOST_CHECK_EQUAL(HexStr(EncodeFnClaimAction(action)->payload),
                          HexStr(encoded->payload));
    }
    // Wrong type / version are unknown.
    {
        CreationAction bad{ClaimAction(0)};
        bad.action_type = 2;
        FnClaimActionV1 decoded;
        BOOST_CHECK(!DecodeFnClaimAction(bad, decoded, error));
        BOOST_CHECK_EQUAL(error, "unknown creation-action type or version");
        bad = ClaimAction(0);
        bad.action_version = 2;
        BOOST_CHECK(!DecodeFnClaimAction(bad, decoded, error));
        BOOST_CHECK_EQUAL(error, "unknown creation-action type or version");
    }
    // Truncation at EVERY payload byte fails.
    {
        const CreationAction full{ClaimAction(0)};
        for (size_t len{0}; len < full.payload.size(); ++len) {
            CreationAction truncated{full};
            truncated.payload.resize(len);
            FnClaimActionV1 decoded;
            BOOST_CHECK_MESSAGE(!DecodeFnClaimAction(truncated, decoded, error),
                                "truncated payload of length " << len << " unexpectedly decoded");
        }
    }
    // Trailing bytes after a complete action.
    {
        CreationAction bad{ClaimAction(0)};
        bad.payload.push_back(0x00);
        FnClaimActionV1 decoded;
        BOOST_CHECK(!DecodeFnClaimAction(bad, decoded, error));
        BOOST_CHECK_EQUAL(error, "trailing bytes after the claim action");
    }
    // Duplicate / out-of-order / omitted record indexes.
    {
        for (const auto& auths :
             {std::vector<FnAuthorization>{P2pkAuth(0), P2pkAuth(0)},
              std::vector<FnAuthorization>{P2pkAuth(1), P2pkAuth(0)},
              std::vector<FnAuthorization>{P2pkAuth(1)}}) {
            BOOST_CHECK(!EncodeFnClaimAction({.fn_output_index = 0, .authorizations = auths}));
        }
        // Decode side: hand-build payload with indexes 0,0.
        std::string payload_hex{"00" "02" "49" + AUTH_P2PK_HEX + "49" + AUTH_P2PK_HEX};
        CreationAction bad;
        bad.action_type = CREATION_ACTION_FN_CLAIM;
        bad.action_version = FN_CLAIM_ACTION_VERSION_V1;
        bad.payload = FromHex(payload_hex);
        FnClaimActionV1 decoded;
        BOOST_CHECK(!DecodeFnClaimAction(bad, decoded, error));
        BOOST_CHECK_EQUAL(error, "authorization records are duplicated, omitted or out of order");
    }
    // No authorizations at all.
    {
        BOOST_CHECK(!EncodeFnClaimAction({.fn_output_index = 0, .authorizations = {}}));
        CreationAction bad;
        bad.action_type = CREATION_ACTION_FN_CLAIM;
        bad.action_version = FN_CLAIM_ACTION_VERSION_V1;
        bad.payload = FromHex("0000");
        FnClaimActionV1 decoded;
        BOOST_CHECK(!DecodeFnClaimAction(bad, decoded, error));
        BOOST_CHECK_EQUAL(error, "claim action carries no authorizations");
    }
    // Oversized: many authorizations exceed the 4,000-byte proof-area
    // bound — encode refuses; a hand-built oversized payload is refused
    // before parsing.
    {
        FnClaimActionV1 big{.fn_output_index = 0};
        for (uint32_t i{0}; i < 60; ++i) big.authorizations.push_back(P2pkhAuth(i));
        BOOST_CHECK(!EncodeFnClaimAction(big)); // 60 × ~109B > 4,000
        CreationAction bad;
        bad.action_type = CREATION_ACTION_FN_CLAIM;
        bad.action_version = FN_CLAIM_ACTION_VERSION_V1;
        bad.payload.assign(MAX_CREATION_ACTION_PAYLOAD + 1, 0x00);
        FnClaimActionV1 decoded;
        BOOST_CHECK(!DecodeFnClaimAction(bad, decoded, error));
        BOOST_CHECK_EQUAL(error, "creation-action payload exceeds the proof-area bound");
    }
    // Large-but-valid: 30 mixed authorizations fit and round-trip.
    {
        FnClaimActionV1 action{.fn_output_index = 1};
        for (uint32_t i{0}; i < 30; ++i) {
            action.authorizations.push_back(i % 2 ? P2pkAuth(i) : P2pkhAuth(i));
        }
        const auto encoded{EncodeFnClaimAction(action)};
        BOOST_REQUIRE(encoded);
        BOOST_CHECK_LE(encoded->payload.size(), MAX_CREATION_ACTION_PAYLOAD);
        FnClaimActionV1 decoded;
        BOOST_REQUIRE_MESSAGE(DecodeFnClaimAction(*encoded, decoded, error), error);
        BOOST_CHECK(decoded == action);
    }
    // The worst-case arithmetic is an upper bound on real encodings.
    {
        FnClaimActionV1 action{.fn_output_index = 0};
        for (uint32_t i{0}; i < 20; ++i) {
            FnAuthorization auth{P2pkhAuth(i)};
            auth.pubkey = FromHex(PUB_U_HEX); // worst-case key form
            action.authorizations.push_back(auth);
        }
        const auto encoded{EncodeFnClaimAction(action)};
        BOOST_REQUIRE(encoded);
        BOOST_CHECK_LE(encoded->payload.size(), WorstCaseFnClaimActionPayload(20));
    }
}

//! SUPERSEDED-record structural rules: kept compiling as the frozen
//! record of the abandoned claim design (deep output parsing moved to
//! the live issuance path).
BOOST_AUTO_TEST_CASE(transition_structural_rules)
{
    std::string error;
    const ModernOutput fn_out{*MakeFnOutput(View(), TestFnAsset())};
    ModernOutput owner_out;
    owner_out.amount = 777;
    owner_out.policy_type = static_cast<uint16_t>(PolicyType::OWNER);
    owner_out.policy_version = POLICY_VERSION_V1;
    owner_out.policy_commitment = uint256::ONE;

    // Under the corrected model, type (1, 1) is codec history only: NO
    // FN semantic checker accepts it, in ANY structural shape — one
    // action, many actions, whatever it references. An empty action set
    // is trivially fine (deep issuance validation is chain-contextual
    // and lives in the live type-2 path).
    {
        BOOST_CHECK_MESSAGE(CheckFnCreationActions({}, {owner_out, fn_out}, error), error);
        for (const std::vector<CreationAction>& actions :
             {std::vector<CreationAction>{ClaimAction(1)},
              std::vector<CreationAction>{ClaimAction(0)},
              std::vector<CreationAction>{ClaimAction(3)},
              std::vector<CreationAction>{ClaimAction(0), ClaimAction(1)}}) {
            BOOST_CHECK(!CheckFnCreationActions(actions, {owner_out, fn_out}, error));
            BOOST_CHECK_EQUAL(error, "superseded FN claim action is not accepted");
        }
    }
    // The CreationAction wrapper is a STANDALONE canonical codec:
    // byte-stable serialization and round trip. Modern-transition v1's
    // frozen wire form carries no creation actions (pinned in
    // transition_proof_tests); carrying them requires the future
    // versioned modern-transition extension, and FN cannot activate
    // before that integration exists.
    {
        const CreationAction action{ClaimAction(0)};
        DataStream s1;
        s1 << action;
        DataStream s2;
        s2 << action;
        BOOST_CHECK(std::ranges::equal(s1, s2));
        CreationAction decoded;
        s1 >> decoded;
        BOOST_CHECK(decoded == action);
        // An FN v1 output inside a v1 transition changes only the id
        // domain fields it occupies — no hidden action bytes exist in v1.
        ModernTransition t;
        t.outputs = {fn_out};
        DataStream serialized;
        serialized << t;
        ModernTransition roundtrip;
        serialized >> roundtrip;
        BOOST_CHECK(FullTransitionId(roundtrip) == FullTransitionId(t));
    }
}

BOOST_AUTO_TEST_CASE(claim_digest_vectors)
{
    const uint256 genesis{"00000000000000000000000000000000000000000000000000000000000000aa"};
    const uint256 final_hash{"00000000000000000000000000000000000000000000000000000000000000bb"};
    const uint256 other_final{"00000000000000000000000000000000000000000000000000000000000000cc"};
    const Txid pod{PodId()};
    const CAmount value{5'000'000};

    const auto domain{ModernChainDomain(genesis, final_hash)};
    BOOST_REQUIRE(domain);
    const auto digest{FnClaimDigest(*domain, pod, value, OWNER_COMMITMENT)};
    BOOST_REQUIRE(digest);

    // Fail-closed inputs: a null genesis or unset X yields no domain at
    // all; an out-of-MoneyRange value yields no digest.
    BOOST_CHECK(!ModernChainDomain(uint256{}, final_hash));
    BOOST_CHECK(!ModernChainDomain(genesis, uint256{}));
    BOOST_CHECK(!FnClaimDigest(*domain, pod, -1, OWNER_COMMITMENT));
    BOOST_CHECK(!FnClaimDigest(*domain, pod, MAX_MONEY + 1, OWNER_COMMITMENT));

    // Independent reconstruction with raw SHA256 (byte-exact, not via
    // the code under test): tagged(tag, m) = SHA256(SHA256(tag) ||
    // SHA256(tag) || m).
    const auto tagged{[](const std::string& tag, const std::vector<unsigned char>& msg) {
        unsigned char tag_hash[CSHA256::OUTPUT_SIZE];
        CSHA256().Write(reinterpret_cast<const unsigned char*>(tag.data()), tag.size())
            .Finalize(tag_hash);
        uint256 out;
        CSHA256()
            .Write(tag_hash, sizeof(tag_hash))
            .Write(tag_hash, sizeof(tag_hash))
            .Write(msg.data(), msg.size())
            .Finalize(out.begin());
        return out;
    }};
    {
        std::vector<unsigned char> msg{genesis.begin(), genesis.end()};
        msg.insert(msg.end(), final_hash.begin(), final_hash.end());
        BOOST_CHECK_EQUAL(tagged("B3/MODERN/CHAIN", msg).GetHex(), domain->GetHex());
    }
    {
        std::vector<unsigned char> msg{domain->begin(), domain->end()};
        const uint256 pod_raw{pod.ToUint256()};
        msg.insert(msg.end(), pod_raw.begin(), pod_raw.end());
        for (int i{0}; i < 8; ++i) { // int64 little-endian
            msg.push_back((static_cast<uint64_t>(value) >> (8 * i)) & 0xff);
        }
        msg.insert(msg.end(), OWNER_COMMITMENT.begin(), OWNER_COMMITMENT.end());
        BOOST_CHECK_EQUAL(tagged("B3/FN/CLAIM/V1", msg).GetHex(), digest->GetHex());
    }

    // Pinned byte-exact vectors: these hex values must NEVER change.
    BOOST_CHECK_EQUAL(domain->GetHex(),
                      "1b862dc4570b24530e26af1b056e70e9269d9b2a89bb5d6b42c4f1ad4f6b36c5");
    BOOST_CHECK_EQUAL(digest->GetHex(),
                      "a5bfb79a09dcc21ab9cac49aec4f35399ec72dd63f0408663587f6a23484fbda");

    // Cross-network / domain mismatch: a different X yields a different
    // domain and digest — an authorization never carries across networks.
    const auto other_domain{ModernChainDomain(genesis, other_final)};
    BOOST_REQUIRE(other_domain);
    BOOST_CHECK(*other_domain != *domain);
    const auto other_digest{FnClaimDigest(*other_domain, pod, value, OWNER_COMMITMENT)};
    BOOST_REQUIRE(other_digest);
    BOOST_CHECK(*other_digest != *digest);
    BOOST_CHECK_EQUAL(other_digest->GetHex(),
                      "bb479b92c14f5542aea5f8a41b5d3eae02bc9b9b5b926028992b49aa597faf94");

    // Mutation vectors: changing the FN output's VALUE or its OWNER
    // COMMITMENT (or the PoDId) changes the digest — a copied
    // authorization can only recreate the same coin.
    BOOST_CHECK(*FnClaimDigest(*domain, pod, value + 1, OWNER_COMMITMENT) != *digest);
    BOOST_CHECK(*FnClaimDigest(*domain, pod, 0, OWNER_COMMITMENT) != *digest);
    BOOST_CHECK(*FnClaimDigest(*domain, pod, value, uint256::ONE) != *digest);
    uint256 other_pod_raw{pod.ToUint256()};
    *other_pod_raw.begin() ^= 0x01;
    BOOST_CHECK(*FnClaimDigest(*domain, Txid::FromUint256(other_pod_raw), value,
                               OWNER_COMMITMENT) != *digest);

    // A REAL deterministic key signs the digest; the signature satisfies
    // the parse-level form rules and round-trips through the record and
    // action codecs. No authorization against PodRecords, no minting.
    CKey key;
    const auto seed{FromHex(Repeat("55", 32))};
    key.Set(seed.begin(), seed.end(), /*fCompressedIn=*/true);
    BOOST_REQUIRE(key.IsValid());
    std::vector<unsigned char> sig;
    BOOST_REQUIRE(key.Sign(*digest, sig));
    const CPubKey pubkey{key.GetPubKey()};
    FnAuthorization auth;
    auth.funding_script_index = 0;
    auth.form = FnAuthForm::P2PKH;
    auth.pubkey.assign(pubkey.begin(), pubkey.end());
    auth.signature = sig;
    const auto action{EncodeFnClaimAction({.fn_output_index = 0, .authorizations = {auth}})};
    BOOST_REQUIRE(action);
    std::string error;
    FnClaimActionV1 decoded;
    BOOST_REQUIRE_MESSAGE(DecodeFnClaimAction(*action, decoded, error), error);
    BOOST_CHECK(decoded.authorizations[0] == auth);
    BOOST_CHECK(pubkey.Verify(*digest, decoded.authorizations[0].signature));
}

BOOST_AUTO_TEST_CASE(v2_envelope_carriage)
{
    std::string error;

    // A claim-FORMAT/CARRIAGE fixture (zero inputs; economic claim
    // validation comes later): the FN v1 output in the id domain, its
    // claim action in the versioned proof area — encode, pinned
    // determinism, decode, and the SEMANTIC layer
    // (CheckFnCreationActions) applied to the decoded collection,
    // separately from generic decoding.
    ModernTransitionV2 t2;
    t2.outputs = {*MakeFnOutput(View(), TestFnAsset())};
    t2.creation_actions = {ClaimAction(0)};
    const auto bytes{EncodeTransitionEnvelope(t2)};
    BOOST_REQUIRE(bytes);
    // Pinned full v2 envelope carrying the RESERVED FnClaimActionV1
    // bytes (frozen codec) around the CORRECTED FN output shape
    // (encoder-independent expectation assembled from the pinned
    // component vectors): version || inputs(0) || the FN output (global
    // asset, 3 units, empty params) || proofs(0) || one claim action.
    const std::string fn_asset_hex{[&] {
        const AssetId asset{TestFnAsset()};
        return HexStr(std::span{asset.begin(), asset.size()});
    }()};
    const std::string expected_hex{
        std::string{"0200"} + "00" + "01" + fn_asset_hex +
        "0300000000000000" + "0500" + "0100" + "c7" + Repeat("00", 31) +
        "00" + "00" + "01" + "0100" + "0100" + "4c" +
        ACTION_P2PK_PAYLOAD_HEX};
    BOOST_CHECK_EQUAL(HexStr(*bytes), expected_hex);
    ModernTransitionV2 decoded;
    BOOST_REQUIRE_MESSAGE(DecodeTransitionEnvelope(*bytes, decoded, error), error);
    BOOST_REQUIRE_EQUAL(decoded.creation_actions.size(), 1U);
    BOOST_CHECK(decoded.creation_actions[0] == t2.creation_actions[0]);
    // GENERIC round-trip compatibility is preserved for the reserved
    // type-1 bytes — but the FN SEMANTIC layer rejects them under the
    // corrected model: old bytes decode, old bytes never validate.
    BOOST_CHECK(!CheckFnCreationActions(decoded.creation_actions, decoded.outputs, error));
    BOOST_CHECK_EQUAL(error, "superseded FN claim action is not accepted");
    // Round-trip identity and action-blind id domain.
    BOOST_CHECK(FullTransitionIdV2(decoded) == FullTransitionIdV2(t2));
    ModernTransitionV2 stripped{t2};
    stripped.creation_actions.clear();
    BOOST_CHECK(TransitionIdV2(stripped) == TransitionIdV2(t2));
    BOOST_CHECK(FullTransitionIdV2(stripped) != FullTransitionIdV2(t2));
}

BOOST_AUTO_TEST_CASE(capacity_arithmetic)
{
    // SUPERSEDED-record arithmetic: worst case of the ABANDONED type-1
    // claim encoding, kept exact and monotone as the frozen historical
    // measurement (non-authoritative for activation — the live type-2
    // carrier is measured separately as future work).
    BOOST_CHECK_GT(WorstCaseFnClaimActionPayload(1), 0U);
    for (size_t n{1}; n < 40; ++n) {
        BOOST_CHECK_LT(WorstCaseFnClaimActionPayload(n),
                       WorstCaseFnClaimActionPayload(n + 1));
    }
    // A single-script claim fits with enormous headroom; the bound is
    // crossed somewhere below 40 worst-case scripts.
    BOOST_CHECK_LE(WorstCaseFnClaimActionPayload(1), 200U);
    BOOST_CHECK_LE(WorstCaseFnClaimActionPayload(27), MAX_CREATION_ACTION_PAYLOAD);
    BOOST_CHECK_GT(WorstCaseFnClaimActionPayload(40), MAX_CREATION_ACTION_PAYLOAD);
}

BOOST_AUTO_TEST_SUITE_END()
