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

FnOutputView View(const CAmount amount = 5'000'000)
{
    return FnOutputView{.amount = amount, .owner_commitment = OWNER_COMMITMENT,
                        .pod_id = PodId()};
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
    // The consensus-stable registry values, pinned.
    BOOST_CHECK_EQUAL(static_cast<uint16_t>(PolicyType::FN), 5);
    BOOST_CHECK_EQUAL(FN_POLICY_TYPE, 5);
    BOOST_CHECK_EQUAL(FN_POLICY_VERSION_V1, 1);
    BOOST_CHECK_EQUAL(CREATION_ACTION_FN_CLAIM, 1);
    BOOST_CHECK_EQUAL(FN_CLAIM_ACTION_VERSION_V1, 1);
    BOOST_CHECK_EQUAL(MAX_CREATION_ACTION_PAYLOAD, 4000U);

    // FN v1 is INACTIVE on every network: the policy model itself fails
    // closed, with or without the test-only asset activation flag.
    BOOST_CHECK(!IsActivatedPolicy(FN_POLICY_TYPE, FN_POLICY_VERSION_V1, false));
    BOOST_CHECK(!IsActivatedPolicy(FN_POLICY_TYPE, FN_POLICY_VERSION_V1, true));
}

BOOST_AUTO_TEST_CASE(fn_output_vectors)
{
    std::string error;

    // Canonical FN v1 output: build, byte-stable serialization, parse.
    {
        const auto out{MakeFnOutput(View())};
        BOOST_REQUIRE(out);
        BOOST_CHECK(out->asset == NativeAsset());
        BOOST_CHECK_EQUAL(out->amount, 5'000'000);
        BOOST_CHECK_EQUAL(out->policy_type, FN_POLICY_TYPE);
        BOOST_CHECK_EQUAL(out->policy_version, FN_POLICY_VERSION_V1);
        BOOST_CHECK(out->policy_commitment == OWNER_COMMITMENT);
        BOOST_CHECK_EQUAL(HexStr(out->policy_params), POD_HEX);

        DataStream s1;
        s1 << *out;
        DataStream s2;
        s2 << *out;
        BOOST_CHECK(std::ranges::equal(s1, s2)); // deterministic bytes
        ModernOutput decoded;
        s1 >> decoded;
        BOOST_CHECK(decoded == *out);

        const auto view{ParseFnOutput(*out, error)};
        BOOST_REQUIRE_MESSAGE(view, error);
        BOOST_CHECK(*view == View());
    }
    // The FN structural rules of CheckPolicyOutput are pinned even though
    // the activation gate makes them unreachable: an FN v1 output at a
    // modern height is UNKNOWN_POLICY (inactive), never OK.
    {
        Consensus::Params params;
        params.hard_fork_height = 10;
        params.legacy_final_hash = uint256::ONE;
        const auto out{MakeFnOutput(View())};
        BOOST_REQUIRE(out);
        BOOST_CHECK(CheckPolicyOutput(*out, /*height=*/50, params) ==
                    PolicyOutputCheck::UNKNOWN_POLICY);
    }
    // Build-side rejections.
    {
        FnOutputView bad{View()};
        bad.pod_id = Txid{};
        BOOST_CHECK(!MakeFnOutput(bad)); // zero PoDId
        bad = View();
        bad.owner_commitment = uint256{};
        BOOST_CHECK(!MakeFnOutput(bad)); // null owner commitment
        bad = View();
        bad.amount = -1;
        BOOST_CHECK(!MakeFnOutput(bad)); // negative
        bad = View();
        bad.amount = MAX_MONEY + 1;
        BOOST_CHECK(!MakeFnOutput(bad)); // above MoneyRange
    }
    // Parse-side rejections; every mutation of a valid output fails.
    {
        const ModernOutput good{*MakeFnOutput(View())};
        ModernOutput bad{good};
        bad.asset = uint256::ONE;
        BOOST_CHECK(!ParseFnOutput(bad, error));
        BOOST_CHECK_EQUAL(error, "FN output asset must be native B3");
        bad = good;
        bad.policy_commitment = uint256{};
        BOOST_CHECK(!ParseFnOutput(bad, error));
        BOOST_CHECK_EQUAL(error, "FN owner commitment is null");
        bad = good;
        bad.policy_params.pop_back();
        BOOST_CHECK(!ParseFnOutput(bad, error));
        BOOST_CHECK_EQUAL(error, "FN params must be exactly the 32-byte PoDId");
        bad = good;
        std::fill(bad.policy_params.begin(), bad.policy_params.end(), 0);
        BOOST_CHECK(!ParseFnOutput(bad, error));
        BOOST_CHECK_EQUAL(error, "PoDId is zero");
        bad = good;
        bad.amount = -1;
        BOOST_CHECK(!ParseFnOutput(bad, error));
        BOOST_CHECK_EQUAL(error, "FN output amount outside MoneyRange");
        // A different policy type simply is not FN.
        bad = good;
        bad.policy_type = static_cast<uint16_t>(PolicyType::OWNER);
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

BOOST_AUTO_TEST_CASE(transition_structural_rules)
{
    std::string error;
    const ModernOutput fn_out{*MakeFnOutput(View())};
    ModernOutput owner_out;
    owner_out.amount = 777;
    owner_out.policy_type = static_cast<uint16_t>(PolicyType::OWNER);
    owner_out.policy_version = POLICY_VERSION_V1;
    owner_out.policy_commitment = uint256::ONE;

    // One FN output, one action, unrelated outputs ignored.
    {
        const std::vector<ModernOutput> outputs{owner_out, fn_out};
        const std::vector<CreationAction> actions{ClaimAction(1)};
        BOOST_CHECK_MESSAGE(CheckFnCreationActions(actions, outputs, error), error);
    }
    // No FN output without an action.
    {
        const std::vector<ModernOutput> outputs{fn_out};
        BOOST_CHECK(!CheckFnCreationActions({}, outputs, error));
        BOOST_CHECK_EQUAL(error, "FN output has no creation action");
    }
    // No action without a corresponding FN output.
    {
        const std::vector<ModernOutput> outputs{owner_out};
        BOOST_CHECK(!CheckFnCreationActions({ClaimAction(0)}, outputs, error));
        BOOST_CHECK_EQUAL(error, "creation action references a non-FN output");
    }
    // Referenced index must exist.
    {
        const std::vector<ModernOutput> outputs{fn_out};
        BOOST_CHECK(!CheckFnCreationActions({ClaimAction(3)}, outputs, error));
        BOOST_CHECK_EQUAL(error, "creation action references a nonexistent output");
    }
    // Duplicate output indexes rejected (also covers unsorted: equal is
    // not ascending).
    {
        const std::vector<ModernOutput> outputs{fn_out};
        BOOST_CHECK(
            !CheckFnCreationActions({ClaimAction(0), ClaimAction(0)}, outputs, error));
        BOOST_CHECK_EQUAL(error, "creation actions not in ascending output-index order");
    }
    // Actions must be sorted ascending.
    {
        ModernOutput fn2{fn_out};
        // A second FN output needs a different PoDId to be meaningful at
        // the model level; structural checks don't dedup PoDIds (that is
        // claim validation's job) but the ordering rule fires first here.
        const std::vector<ModernOutput> outputs{fn_out, fn2};
        BOOST_CHECK(
            !CheckFnCreationActions({ClaimAction(1), ClaimAction(0)}, outputs, error));
        BOOST_CHECK_EQUAL(error, "creation actions not in ascending output-index order");
    }
    // Two FN outputs, two ascending actions: structurally valid.
    {
        const std::vector<ModernOutput> outputs{fn_out, fn_out};
        BOOST_CHECK_MESSAGE(
            CheckFnCreationActions({ClaimAction(0), ClaimAction(1)}, outputs, error), error);
    }
    // A malformed FN output is rejected through the action that names it.
    {
        ModernOutput bad{fn_out};
        std::fill(bad.policy_params.begin(), bad.policy_params.end(), 0);
        const std::vector<ModernOutput> outputs{bad};
        BOOST_CHECK(!CheckFnCreationActions({ClaimAction(0)}, outputs, error));
        BOOST_CHECK(error.find("malformed FN output") != std::string::npos);
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
    t2.outputs = {*MakeFnOutput(View())};
    t2.creation_actions = {ClaimAction(0)};
    const auto bytes{EncodeTransitionEnvelope(t2)};
    BOOST_REQUIRE(bytes);
    // Literal pinned full v2 envelope carrying a VALID FnClaimActionV1
    // (encoder-independent expectation assembled from the pinned
    // component vectors): version || inputs(0) || the FN output ||
    // proofs(0) || one claim action.
    const std::string expected_hex{
        std::string{"0200"} + "00" + "01" + Repeat("00", 32) +
        "404b4c0000000000" + "0500" + "0100" + "c7" + Repeat("00", 31) +
        "20" + POD_HEX + "00" + "01" + "0100" + "0100" + "4c" +
        ACTION_P2PK_PAYLOAD_HEX};
    BOOST_CHECK_EQUAL(HexStr(*bytes), expected_hex);
    ModernTransitionV2 decoded;
    BOOST_REQUIRE_MESSAGE(DecodeTransitionEnvelope(*bytes, decoded, error), error);
    BOOST_REQUIRE_EQUAL(decoded.creation_actions.size(), 1U);
    BOOST_CHECK(decoded.creation_actions[0] == t2.creation_actions[0]);
    BOOST_CHECK_MESSAGE(
        CheckFnCreationActions(decoded.creation_actions, decoded.outputs, error), error);
    // Round-trip identity and action-blind id domain.
    BOOST_CHECK(FullTransitionIdV2(decoded) == FullTransitionIdV2(t2));
    ModernTransitionV2 stripped{t2};
    stripped.creation_actions.clear();
    BOOST_CHECK(TransitionIdV2(stripped) == TransitionIdV2(t2));
    BOOST_CHECK(FullTransitionIdV2(stripped) != FullTransitionIdV2(t2));

    // The GENERIC decoder accepts a registered frame whose payload is
    // FN-semantically wrong; the FN checker rejects it separately —
    // duplicate actions for one output here.
    ModernTransitionV2 dup{t2};
    dup.creation_actions = {ClaimAction(0), ClaimAction(0)};
    const auto dup_bytes{EncodeTransitionEnvelope(dup)};
    BOOST_REQUIRE(dup_bytes);
    ModernTransitionV2 dup_decoded;
    BOOST_REQUIRE_MESSAGE(DecodeTransitionEnvelope(*dup_bytes, dup_decoded, error), error);
    BOOST_CHECK(
        !CheckFnCreationActions(dup_decoded.creation_actions, dup_decoded.outputs, error));
    BOOST_CHECK_EQUAL(error, "creation actions not in ascending output-index order");
}

BOOST_AUTO_TEST_CASE(capacity_arithmetic)
{
    // The offline capacity report's single source of truth: exact and
    // monotone, and the historically plausible cases fit comfortably.
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
