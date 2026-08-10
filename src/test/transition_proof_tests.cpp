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

BOOST_AUTO_TEST_SUITE_END()
