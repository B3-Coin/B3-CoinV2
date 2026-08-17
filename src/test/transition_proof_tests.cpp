// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! Typed segregated TransitionProofs: id/commitment separation, canonical
//! proof refs, per-policy dispatch, and malformed/oversized rejection.

#include <modern/proof.h>

#include <modern/policy.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <vector>

BOOST_AUTO_TEST_SUITE(transition_proof_tests)

namespace {

std::vector<unsigned char> RevealScript()
{
    const CScript script{CScript() << OP_DUP << OP_HASH160
                                   << std::vector<unsigned char>(20, 0xb3)
                                   << OP_EQUALVERIFY << OP_CHECKSIG};
    return {script.begin(), script.end()};
}

modern::ModernOutput LegacyLockPrev()
{
    modern::ModernOutput prev;
    prev.asset = modern::NativeAsset();
    prev.amount = 5'000'000;
    prev.policy_type = static_cast<uint16_t>(modern::PolicyType::LEGACY_LOCK);
    prev.policy_version = modern::POLICY_VERSION_V1;
    const auto reveal{RevealScript()};
    CSHA256().Write(reveal.data(), reveal.size()).Finalize(prev.policy_commitment.begin());
    return prev;
}

modern::ModernOutput OwnerPrev()
{
    modern::ModernOutput prev;
    prev.asset = modern::NativeAsset();
    prev.amount = 1'000;
    prev.policy_type = static_cast<uint16_t>(modern::PolicyType::OWNER);
    prev.policy_version = modern::POLICY_VERSION_V1;
    prev.policy_commitment = uint256{"00000000000000000000000000000000000000000000000000000000000000ee"};
    return prev;
}

modern::TransitionProof OwnerProof()
{
    modern::TransitionProof proof;
    proof.proof_type = static_cast<uint16_t>(modern::PolicyType::OWNER);
    proof.proof_version = modern::POLICY_VERSION_V1;
    proof.payload = {0x01, 0x02, 0x03};
    return proof;
}

modern::ModernTransition SampleTransition()
{
    modern::ModernTransition t;
    t.inputs.resize(1);
    t.inputs[0].prevout = COutPoint{
        Txid::FromUint256(uint256{"00000000000000000000000000000000000000000000000000000000000000b3"}), 1};
    t.inputs[0].sequence = 0xFFFFFFFE;
    t.inputs[0].proof_index = 0;
    modern::ModernOutput out;
    out.asset = modern::NativeAsset();
    out.amount = 4'999'000;
    out.policy_type = static_cast<uint16_t>(modern::PolicyType::OWNER);
    out.policy_version = modern::POLICY_VERSION_V1;
    out.policy_commitment = uint256{"00000000000000000000000000000000000000000000000000000000000000dd"};
    t.outputs.push_back(out);
    t.proofs.push_back(modern::MakeLegacyLockProof(RevealScript(), {0x51}));
    return t;
}

//! Frozen serialized bytes of SampleTransition(). Consensus-stable.
const std::string TRANSITION_HEX{
    "01b300000000000000000000000000000000000000000000000000000000000000"
    "01000000feffffff00000000010000000000000000000000000000000000000000"
    "00000000000000000000000058474c000000000001000100dd0000000000000000"
    "00000000000000000000000000000000000000000000000001000001001c1976a9"
    "14b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b388ac0151"};

} // namespace

BOOST_AUTO_TEST_CASE(input_retains_prevout_sequence_and_proof_ref_only)
{
    const modern::ModernTransition t{SampleTransition()};
    // prevout identifies the coin; sequence retained; ProofRef is an index
    // into the segregated area. Serialized size pins the exact field set:
    // 36-byte outpoint + 4-byte sequence + 4-byte proof ref.
    DataStream s;
    s << t.inputs[0];
    BOOST_CHECK_EQUAL(s.size(), 36U + 4U + 4U);
}

BOOST_AUTO_TEST_CASE(serialization_is_frozen)
{
    DataStream s;
    s << SampleTransition();
    BOOST_CHECK_EQUAL(HexStr(s), TRANSITION_HEX);

    modern::ModernTransition decoded;
    s >> decoded;
    BOOST_CHECK_EQUAL(modern::FullTransitionId(decoded).GetHex(),
                      modern::FullTransitionId(SampleTransition()).GetHex());
}

BOOST_AUTO_TEST_CASE(txid_excludes_segregated_proofs_and_commitment_pins_them)
{
    const modern::ModernTransition base{SampleTransition()};
    modern::ModernTransition mutated{base};
    mutated.proofs[0].payload.push_back(0x00);

    // Proof bytes never reach the transition id...
    BOOST_CHECK_EQUAL(modern::TransitionId(base).GetHex(), modern::TransitionId(mutated).GetHex());
    // ...but the proof commitment and the full id pin them unambiguously.
    BOOST_CHECK(modern::ProofAreaCommitment(base) != modern::ProofAreaCommitment(mutated));
    BOOST_CHECK(modern::FullTransitionId(base) != modern::FullTransitionId(mutated));
    BOOST_CHECK_EQUAL(modern::ProofAreaCommitment(base).GetHex(),
                      modern::ProofAreaCommitment(SampleTransition()).GetHex());

    // The id domain still commits to the structural side of every input.
    modern::ModernTransition resequenced{base};
    resequenced.inputs[0].sequence = 0;
    BOOST_CHECK(modern::TransitionId(base) != modern::TransitionId(resequenced));
}

BOOST_AUTO_TEST_CASE(dispatch_follows_the_previous_output_policy)
{
    // LEGACY_LOCK: the reveal must match the stored commitment.
    const modern::ModernOutput legacy_prev{LegacyLockPrev()};
    const modern::TransitionProof good{modern::MakeLegacyLockProof(RevealScript(), {0x51})};
    BOOST_CHECK(modern::VerifyTransitionProof(legacy_prev, good) == modern::ProofCheck::OK);

    std::vector<unsigned char> wrong_reveal{RevealScript()};
    wrong_reveal[0] ^= 0x01;
    BOOST_CHECK(modern::VerifyTransitionProof(
                    legacy_prev, modern::MakeLegacyLockProof(wrong_reveal, {0x51})) ==
                modern::ProofCheck::COMMITMENT_MISMATCH);

    // OWNER: structure enforced; dispatch selected by the prev output.
    BOOST_CHECK(modern::VerifyTransitionProof(OwnerPrev(), OwnerProof()) ==
                modern::ProofCheck::OK);

    // A proof of the wrong type never satisfies another policy.
    BOOST_CHECK(modern::VerifyTransitionProof(legacy_prev, OwnerProof()) ==
                modern::ProofCheck::TYPE_MISMATCH);
    BOOST_CHECK(modern::VerifyTransitionProof(OwnerPrev(), good) ==
                modern::ProofCheck::TYPE_MISMATCH);

    // Unknown previous policies cannot be spent at all.
    modern::ModernOutput unknown{OwnerPrev()};
    unknown.policy_type = 7;
    BOOST_CHECK(modern::VerifyTransitionProof(unknown, OwnerProof()) ==
                modern::ProofCheck::UNKNOWN_POLICY);
}

BOOST_AUTO_TEST_CASE(malformed_and_oversized_proofs_are_rejected)
{
    const modern::ModernOutput legacy_prev{LegacyLockPrev()};

    // Truncated payload.
    modern::TransitionProof truncated{modern::MakeLegacyLockProof(RevealScript(), {0x51})};
    truncated.payload.resize(truncated.payload.size() / 2);
    BOOST_CHECK(modern::VerifyTransitionProof(legacy_prev, truncated) ==
                modern::ProofCheck::MALFORMED);

    // Trailing bytes after a well-formed payload.
    modern::TransitionProof trailing{modern::MakeLegacyLockProof(RevealScript(), {0x51})};
    trailing.payload.push_back(0x00);
    BOOST_CHECK(modern::VerifyTransitionProof(legacy_prev, trailing) ==
                modern::ProofCheck::MALFORMED);

    // Empty OWNER payload.
    modern::TransitionProof empty_owner{OwnerProof()};
    empty_owner.payload.clear();
    BOOST_CHECK(modern::VerifyTransitionProof(OwnerPrev(), empty_owner) ==
                modern::ProofCheck::MALFORMED);

    // Oversized payload.
    modern::TransitionProof oversized{OwnerProof()};
    oversized.payload.assign(modern::MAX_TRANSITION_PROOF_SIZE + 1, 0xaa);
    BOOST_CHECK(modern::VerifyTransitionProof(OwnerPrev(), oversized) ==
                modern::ProofCheck::OVERSIZED);
}

BOOST_AUTO_TEST_CASE(proof_area_layout_is_canonical)
{
    const std::vector<modern::ModernOutput> prevs{LegacyLockPrev()};
    modern::ModernTransition t{SampleTransition()};
    BOOST_CHECK(modern::VerifyTransitionProofs(prevs, t) == modern::ProofCheck::OK);

    // Dangling proof data is not allowed...
    modern::ModernTransition extra{t};
    extra.proofs.push_back(OwnerProof());
    BOOST_CHECK(modern::VerifyTransitionProofs(prevs, extra) ==
                modern::ProofCheck::PROOF_COUNT_MISMATCH);
    // ...nor a missing proof...
    modern::ModernTransition missing{t};
    missing.proofs.clear();
    BOOST_CHECK(modern::VerifyTransitionProofs(prevs, missing) ==
                modern::ProofCheck::PROOF_COUNT_MISMATCH);
    // ...nor a non-positional reference.
    modern::ModernTransition skewed{t};
    skewed.inputs[0].proof_index = 5;
    BOOST_CHECK(modern::VerifyTransitionProofs(prevs, skewed) ==
                modern::ProofCheck::PROOF_REF_NONCANONICAL);
}

// ====================================================================
// Versioned transition envelope (v2). Raw v1 stays byte-frozen; the
// envelope is a separate API accepting version 2 only.
// ====================================================================

namespace {

//! A registered-type action frame with an arbitrary payload. The
//! GENERIC layer validates framing and bounds only; FN payload
//! semantics are fn_claim_tests' separate concern.
modern::CreationAction GenericAction(const size_t payload_size = 3)
{
    modern::CreationAction action;
    action.action_type = modern::CREATION_ACTION_FN_CLAIM;
    action.action_version = modern::FN_CLAIM_ACTION_VERSION_V1;
    action.payload.assign(payload_size, 0xab);
    return action;
}

modern::ModernTransitionV2 SampleV2(std::vector<modern::CreationAction> actions = {})
{
    const modern::ModernTransition v1{SampleTransition()};
    modern::ModernTransitionV2 t2;
    t2.inputs = v1.inputs;
    t2.outputs = v1.outputs;
    t2.proofs = v1.proofs;
    t2.creation_actions = std::move(actions);
    return t2;
}

std::vector<unsigned char> MustEncode(const modern::ModernTransitionV2& t2)
{
    const auto bytes{modern::EncodeTransitionEnvelope(t2)};
    BOOST_REQUIRE(bytes);
    return *bytes;
}

} // namespace

BOOST_AUTO_TEST_CASE(v1_frozen_vectors_are_untouched_by_v2)
{
    // The raw v1 wire form, id, proof commitment and full id are the
    // pre-Commit-4 values byte-for-byte, pinned as literals so any
    // accidental v1 drift fails HERE, next to v2.
    DataStream s;
    s << SampleTransition();
    BOOST_CHECK_EQUAL(HexStr(s), TRANSITION_HEX);
    const modern::ModernTransition t{SampleTransition()};
    BOOST_CHECK_EQUAL(modern::TransitionId(t).GetHex(), "11b400b80fcfbd68f17ea604fccad00f4eac81e5bc9e16810a23add1ee996579");
    BOOST_CHECK_EQUAL(modern::ProofAreaCommitment(t).GetHex(), "d713d07e800c5f9589f2626f3a7d3423db356da1bb1108870ef59ae312fce687");
    BOOST_CHECK_EQUAL(modern::FullTransitionId(t).GetHex(), "5b1d40a0d706cfaa0941e0040ca87351e6d3ddf3ddb6312cccd6ccae5d06b460");

    // COMPATIBILITY RULE (binding, see proof.h): raw-v1 and v2-envelope
    // bytes are NOT universally disjoint languages — the outer context
    // explicitly selects the decoder; never sniffing, never fallback,
    // always exact exhaustion. The checks below prove rejection for
    // THESE fixtures only, not a universal byte-level property.
    const auto envelope{MustEncode(SampleV2({GenericAction()}))};
    DataStream raw{};
    raw.write(std::as_bytes(std::span{envelope}));
    bool v1_accepted_envelope{false};
    try {
        modern::ModernTransition decoded;
        raw >> decoded;
        if (raw.empty()) {
            DataStream re;
            re << decoded;
            v1_accepted_envelope = HexStr(re) == HexStr(envelope);
        }
    } catch (const std::exception&) {
        // rejection by throw: fine
    }
    BOOST_CHECK(!v1_accepted_envelope);

    // And the envelope decoder rejects raw v1 bytes at the version
    // field (a v1 body's first two bytes never read as LE 2 here).
    DataStream v1_stream;
    v1_stream << SampleTransition();
    const auto v1_span{MakeUCharSpan(v1_stream)};
    const std::vector<unsigned char> v1_vec{v1_span.begin(), v1_span.end()};
    std::string error;
    modern::ModernTransitionV2 decoded;
    BOOST_CHECK(!modern::DecodeTransitionEnvelope(v1_vec, decoded, error));
    BOOST_CHECK_EQUAL(error, "unsupported transition version");
}

BOOST_AUTO_TEST_CASE(v2_envelope_vectors_and_roundtrip)
{
    std::string error;

    // Pinned empty-action envelope: version bytes 0200 + the v1-shaped
    // body + the empty action section (00).
    {
        const auto bytes{MustEncode(SampleV2())};
        BOOST_CHECK_EQUAL(HexStr(bytes), std::string{"0200"} + TRANSITION_HEX + "00");
        modern::ModernTransitionV2 decoded;
        BOOST_REQUIRE_MESSAGE(modern::DecodeTransitionEnvelope(bytes, decoded, error), error);
        BOOST_CHECK(modern::FullTransitionIdV2(decoded) ==
                    modern::FullTransitionIdV2(SampleV2()));
        // Deterministic re-encode.
        BOOST_CHECK_EQUAL(HexStr(MustEncode(decoded)), HexStr(bytes));
    }
    // Pinned action-carrying envelope: one registered action frame
    // (type 0100, version 0100, payload 03 ababab).
    {
        const auto bytes{MustEncode(SampleV2({GenericAction()}))};
        BOOST_CHECK_EQUAL(HexStr(bytes),
                          std::string{"0200"} + TRANSITION_HEX + "01" + "0100" + "0100" +
                              "03" + "ababab");
        modern::ModernTransitionV2 decoded;
        BOOST_REQUIRE_MESSAGE(modern::DecodeTransitionEnvelope(bytes, decoded, error), error);
        BOOST_REQUIRE_EQUAL(decoded.creation_actions.size(), 1U);
        BOOST_CHECK(decoded.creation_actions[0] == GenericAction());
    }
}

BOOST_AUTO_TEST_CASE(v2_identities_are_version_domain_separated)
{
    const modern::ModernTransitionV2 base{SampleV2()};
    const modern::ModernTransition v1{SampleTransition()};

    // Identical economic bodies NEVER share an id across versions.
    BOOST_CHECK(modern::TransitionIdV2(base) != modern::TransitionId(v1));
    BOOST_CHECK(modern::ProofAreaCommitmentV2(base) != modern::ProofAreaCommitment(v1));
    BOOST_CHECK(modern::FullTransitionIdV2(base) != modern::FullTransitionId(v1));

    // Action mutations: add / remove / reorder / mutate all change the
    // v2 proof commitment and full id — and never TransitionIdV2.
    const uint256 id{modern::TransitionIdV2(base)};
    const uint256 proofs{modern::ProofAreaCommitmentV2(base)};
    const uint256 full{modern::FullTransitionIdV2(base)};

    modern::CreationAction a{GenericAction(3)};
    modern::CreationAction b{GenericAction(4)};
    for (const auto& variant :
         {SampleV2({a}), SampleV2({a, b}), SampleV2({b, a}), SampleV2({b})}) {
        BOOST_CHECK(modern::TransitionIdV2(variant) == id);
        BOOST_CHECK(modern::ProofAreaCommitmentV2(variant) != proofs);
        BOOST_CHECK(modern::FullTransitionIdV2(variant) != full);
    }
    // Pairwise: reordering {a,b} vs {b,a} changes BOTH the proof
    // commitment and the full id.
    BOOST_CHECK(modern::ProofAreaCommitmentV2(SampleV2({a, b})) !=
                modern::ProofAreaCommitmentV2(SampleV2({b, a})));
    BOOST_CHECK(modern::FullTransitionIdV2(SampleV2({a, b})) !=
                modern::FullTransitionIdV2(SampleV2({b, a})));
    // Pairwise: mutating one payload byte changes both as well.
    modern::CreationAction mutated{a};
    mutated.payload[0] ^= 0x01;
    BOOST_CHECK(modern::ProofAreaCommitmentV2(SampleV2({mutated})) !=
                modern::ProofAreaCommitmentV2(SampleV2({a})));
    BOOST_CHECK(modern::FullTransitionIdV2(SampleV2({mutated})) !=
                modern::FullTransitionIdV2(SampleV2({a})));

    // The exact v2 domains and preimages, pinned by independent
    // reconstruction: TaggedHash(tag) << u16(2) << fields.
    {
        HashWriter h{TaggedHash("B3/MODERN/TX/ID/V2")};
        h << modern::TRANSITION_VERSION_V2 << base.inputs << base.outputs;
        BOOST_CHECK_EQUAL(h.GetSHA256().GetHex(), modern::TransitionIdV2(base).GetHex());
    }
    {
        HashWriter h{TaggedHash("B3/MODERN/TX/PROOFAREA/V2")};
        h << modern::TRANSITION_VERSION_V2 << base.proofs << base.creation_actions;
        BOOST_CHECK_EQUAL(h.GetSHA256().GetHex(),
                          modern::ProofAreaCommitmentV2(base).GetHex());
    }
    {
        HashWriter h{TaggedHash("B3/MODERN/TX/FULL/V2")};
        h << modern::TRANSITION_VERSION_V2 << base.inputs << base.outputs << base.proofs
          << base.creation_actions;
        BOOST_CHECK_EQUAL(h.GetSHA256().GetHex(), modern::FullTransitionIdV2(base).GetHex());
    }
    // Pinned hex vectors: these values must NEVER change.
    BOOST_CHECK_EQUAL(modern::TransitionIdV2(base).GetHex(),
                      "2a107db6d13975111cc1e9a4e148bbc30369ba72c061baa697e6c465889e93d3");
    BOOST_CHECK_EQUAL(modern::ProofAreaCommitmentV2(base).GetHex(),
                      "1712ee9ace9e13b09633a16805a45be4eb5d5ba2313f7f62023041c77efbfc58");
    BOOST_CHECK_EQUAL(modern::FullTransitionIdV2(base).GetHex(),
                      "d598cd8bb6051f6bfa8edecb3eb508620e01cfc85aa1ec13372e09635551c7e9");
}

BOOST_AUTO_TEST_CASE(v2_envelope_rejections)
{
    std::string error;
    modern::ModernTransitionV2 decoded;
    const auto valid{MustEncode(SampleV2({GenericAction()}))};

    // Version 0, 1, unknown, wrong-endian and truncated version values.
    for (const std::string version_hex : {"0000", "0100", "0300", "0002" /*BE 2 = 512*/}) {
        auto bytes{TryParseHex<unsigned char>(version_hex).value()};
        bytes.insert(bytes.end(), valid.begin() + 2, valid.end());
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "unsupported transition version");
    }
    {
        const std::vector<unsigned char> one_byte{0x02};
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(one_byte, decoded, error));
        BOOST_CHECK_EQUAL(error, "truncated transition version");
        BOOST_CHECK(!modern::DecodeTransitionEnvelope({}, decoded, error));
    }
    // Truncation at EVERY byte boundary fails.
    for (size_t len{0}; len < valid.size(); ++len) {
        const std::span<const unsigned char> prefix{valid.data(), len};
        BOOST_CHECK_MESSAGE(!modern::DecodeTransitionEnvelope(prefix, decoded, error),
                            "truncated envelope of length " << len << " unexpectedly decoded");
    }
    // Trailing bytes after a complete envelope.
    {
        auto bytes{valid};
        bytes.push_back(0x00);
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "trailing bytes after the transition envelope");
    }
    // Unknown action type / version inside the section.
    {
        auto bytes{MustEncode(SampleV2())};
        bytes.back() = 0x01; // count 1
        const std::vector<unsigned char> frame{0x02, 0x00, 0x01, 0x00, 0x00}; // type 2
        bytes.insert(bytes.end(), frame.begin(), frame.end());
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "unknown creation-action type or version");
    }
    // Non-minimal action count (fd-encoding of 1).
    {
        auto bytes{MustEncode(SampleV2())};
        bytes.pop_back(); // drop the canonical 00 count
        for (const unsigned char c : {0xfd, 0x01, 0x00}) bytes.push_back(c);
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "truncated or non-canonical action count");
    }
    // A huge claimed count rejects BEFORE allocation (count bound and
    // byte-feasibility both fire long before any element is read).
    {
        auto bytes{MustEncode(SampleV2())};
        bytes.pop_back();
        for (const unsigned char c : {0xfe, 0xff, 0xff, 0xff, 0x7f}) bytes.push_back(c);
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "creation-action count exceeds the transition bound");
    }
    // Count exceeding the count bound (65 canonical).
    {
        auto bytes{MustEncode(SampleV2())};
        bytes.back() = 65;
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "creation-action count exceeds the transition bound");
    }
    // Per-action payload above the 4,000-byte bound.
    {
        BOOST_CHECK(!modern::EncodeTransitionEnvelope(SampleV2({GenericAction(4001)})));
        auto bytes{MustEncode(SampleV2())};
        bytes.back() = 0x01;
        for (const unsigned char c : {0x01, 0x00, 0x01, 0x00}) bytes.push_back(c);
        for (const unsigned char c : {0xfd, 0xa1, 0x0f}) bytes.push_back(c); // len 4001
        bytes.insert(bytes.end(), 4001, 0xab);
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "creation-action payload exceeds the proof-area bound");
    }
    // Aggregate section bound, encode side: five near-worst-case
    // payloads fit under the 20,000-byte section bound; the sixth is
    // rejected (aggregate limit + 1 territory).
    {
        std::vector<modern::CreationAction> five(5, GenericAction(3990));
        BOOST_CHECK(modern::EncodeTransitionEnvelope(SampleV2(five)));
        std::vector<modern::CreationAction> six(6, GenericAction(3990));
        BOOST_CHECK(!modern::EncodeTransitionEnvelope(SampleV2(six)));
    }
    // Aggregate section bound, decode side: hand-framed six-action
    // section rejects at the aggregate check with checked arithmetic.
    {
        auto bytes{MustEncode(SampleV2())};
        bytes.back() = 0x06; // count 6
        for (int i{0}; i < 6; ++i) {
            for (const unsigned char c : {0x01, 0x00, 0x01, 0x00}) bytes.push_back(c);
            for (const unsigned char c : {0xfd, 0x96, 0x0f}) bytes.push_back(c); // 3990
            bytes.insert(bytes.end(), 3990, 0xab);
        }
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "creation-action section exceeds the aggregate bound");
    }
    // Envelope size bound rejects before any parsing (decode side) and
    // at the encoder (250 maximum proofs push the envelope past the
    // temporary 1,000,000-byte parser ceiling).
    {
        std::vector<unsigned char> huge(modern::MAX_TRANSITION_ENVELOPE_SIZE + 1, 0x00);
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(huge, decoded, error));
        BOOST_CHECK_EQUAL(error, "transition envelope exceeds the size bound");
        modern::ModernTransitionV2 fat{SampleV2()};
        modern::TransitionProof big_proof;
        big_proof.proof_type = 1;
        big_proof.proof_version = 1;
        big_proof.payload.assign(4000, 0xcd);
        fat.proofs.assign(250, big_proof);
        BOOST_CHECK(!modern::EncodeTransitionEnvelope(fat));
    }
    // Exactly 64 actions accepted; 65 rejected on BOTH sides.
    {
        std::vector<modern::CreationAction> sixty_four(64, GenericAction(3));
        const auto ok{modern::EncodeTransitionEnvelope(SampleV2(sixty_four))};
        BOOST_REQUIRE(ok);
        BOOST_REQUIRE_MESSAGE(modern::DecodeTransitionEnvelope(*ok, decoded, error), error);
        BOOST_CHECK_EQUAL(decoded.creation_actions.size(), 64U);
        std::vector<modern::CreationAction> sixty_five(65, GenericAction(3));
        BOOST_CHECK(!modern::EncodeTransitionEnvelope(SampleV2(sixty_five)));
    }
    // Exactly 4,000-byte payload accepted; 4,001 rejected (encode side
    // covered above; this is the accept boundary with round trip).
    {
        const auto ok{modern::EncodeTransitionEnvelope(SampleV2({GenericAction(4000)}))};
        BOOST_REQUIRE(ok);
        BOOST_REQUIRE_MESSAGE(modern::DecodeTransitionEnvelope(*ok, decoded, error), error);
        BOOST_CHECK_EQUAL(decoded.creation_actions[0].payload.size(), 4000U);
    }
    // Exactly 20,000-byte action section accepted; 20,001 rejected.
    // count(1) + 4 x (7 + 4000) + (7 + q): q = 3964 hits 20,000 exactly.
    {
        std::vector<modern::CreationAction> exact(4, GenericAction(4000));
        exact.push_back(GenericAction(3964));
        const auto ok{modern::EncodeTransitionEnvelope(SampleV2(exact))};
        BOOST_REQUIRE(ok);
        BOOST_REQUIRE_MESSAGE(modern::DecodeTransitionEnvelope(*ok, decoded, error), error);
        std::vector<modern::CreationAction> over(4, GenericAction(4000));
        over.push_back(GenericAction(3965)); // section 20,001
        BOOST_CHECK(!modern::EncodeTransitionEnvelope(SampleV2(over)));
        // Decode side: the same exactly-20,001-byte section, hand-framed,
        // rejects at the aggregate check.
        auto bytes{MustEncode(SampleV2())};
        bytes.back() = 0x05; // count 5
        for (int i{0}; i < 4; ++i) {
            for (const unsigned char c : {0x01, 0x00, 0x01, 0x00}) bytes.push_back(c);
            for (const unsigned char c : {0xfd, 0xa0, 0x0f}) bytes.push_back(c); // 4000
            bytes.insert(bytes.end(), 4000, 0xab);
        }
        for (const unsigned char c : {0x01, 0x00, 0x01, 0x00}) bytes.push_back(c);
        for (const unsigned char c : {0xfd, 0x7d, 0x0f}) bytes.push_back(c); // 3965
        bytes.insert(bytes.end(), 3965, 0xab);
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "creation-action section exceeds the aggregate bound");
    }
    // Non-minimal per-action payload length (fd-encoding of 3).
    {
        auto bytes{MustEncode(SampleV2())};
        bytes.back() = 0x01;
        for (const unsigned char c : {0x01, 0x00, 0x01, 0x00}) bytes.push_back(c);
        for (const unsigned char c : {0xfd, 0x03, 0x00}) bytes.push_back(c);
        bytes.insert(bytes.end(), 3, 0xab);
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "truncated or non-canonical action payload length");
    }
    // uint64-max payload length and uint64-max count: canonical
    // encodings of absurd values reject at their bounds, before any
    // allocation.
    {
        auto bytes{MustEncode(SampleV2())};
        bytes.back() = 0x01;
        for (const unsigned char c : {0x01, 0x00, 0x01, 0x00}) bytes.push_back(c);
        bytes.push_back(0xff);
        bytes.insert(bytes.end(), 8, 0xff); // len = uint64 max
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "creation-action payload exceeds the proof-area bound");
        auto count_bytes{MustEncode(SampleV2())};
        count_bytes.pop_back();
        count_bytes.push_back(0xff);
        count_bytes.insert(count_bytes.end(), 8, 0xff); // count = uint64 max
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(count_bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "creation-action count exceeds the transition bound");
    }
    // In-bounds count with insufficient remaining bytes rejects before
    // any element is read.
    {
        auto bytes{MustEncode(SampleV2())};
        bytes.back() = 0x03; // three actions claimed, zero frame bytes
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "creation-action count exceeds the available bytes");
    }
    // Known action type with an UNKNOWN version rejects.
    {
        auto bytes{MustEncode(SampleV2())};
        bytes.back() = 0x01;
        for (const unsigned char c : {0x01, 0x00, 0x02, 0x00, 0x00}) bytes.push_back(c);
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(bytes, decoded, error));
        BOOST_CHECK_EQUAL(error, "unknown creation-action type or version");
    }
    // Bounded decode applies to the v1-shaped collections too: claimed
    // counts beyond the remaining bytes reject for inputs, outputs and
    // proofs alike.
    {
        const std::vector<unsigned char> bad_inputs{0x02, 0x00, 0x09};
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(bad_inputs, decoded, error));
        BOOST_CHECK_EQUAL(error, "input count exceeds the available bytes");
        const std::vector<unsigned char> bad_outputs{0x02, 0x00, 0x00, 0x01};
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(bad_outputs, decoded, error));
        BOOST_CHECK_EQUAL(error, "output count exceeds the available bytes");
        const std::vector<unsigned char> bad_proofs{0x02, 0x00, 0x00, 0x00, 0x01};
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(bad_proofs, decoded, error));
        BOOST_CHECK_EQUAL(error, "proof count exceeds the available bytes");
        // An oversized policy-params length inside an output rejects.
        std::vector<unsigned char> bad_params{0x02, 0x00, 0x00, 0x01};
        bad_params.insert(bad_params.end(), 32 + 8 + 2 + 2 + 32, 0x00);
        bad_params.push_back(0x51); // params length 81 > 80
        bad_params.insert(bad_params.end(), 81, 0x00);
        BOOST_CHECK(!modern::DecodeTransitionEnvelope(bad_params, decoded, error));
        BOOST_CHECK_EQUAL(error, "policy params exceed the bound");
    }
}

BOOST_AUTO_TEST_SUITE_END()
