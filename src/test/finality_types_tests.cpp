// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 1 of the Modern PoS V1 finality plan: frozen constants, fixed-width
// codecs, digests, the Keccak validator-set commitment, and the guardrails
// that nothing new is activated (policy 6/7/8 fail closed; creation-action
// numbers 4/5 are not registered).

#include <consensus/consensus.h>
#include <consensus/modern_pos_params.h>
#include <crypto/keccak256.h>
#include <modern/creation_action.h>
#include <modern/finality_types.h>
#include <modern/policy.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <vector>

namespace {

std::string Hex(std::span<const unsigned char> b) { return HexStr(b); }

uint256 KeccakOf(const std::string& s)
{
    uint256 out;
    Keccak256().Write(std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(s.data()), s.size())).Finalize(out);
    return out;
}

} // namespace

BOOST_AUTO_TEST_SUITE(finality_types_tests)

BOOST_AUTO_TEST_CASE(frozen_constants)
{
    // Consensus-level (consensus.h)
    BOOST_CHECK_EQUAL(MAX_PAYLOAD_RECORD_SIZE, 32768u);
    BOOST_CHECK_EQUAL(MAX_PAYLOAD_SECTION_SIZE, 65536u);
    BOOST_CHECK_EQUAL(MPA_WEIGHT_FACTOR, WITNESS_SCALE_FACTOR);
    BOOST_CHECK_EQUAL(MPA_WEIGHT_FACTOR, 4);
    BOOST_CHECK_EQUAL(MAX_BLOCK_PAYLOAD_COST, 120000);
    BOOST_CHECK_EQUAL(MAX_TX_PAYLOAD_COST, 12000);
    BOOST_CHECK_EQUAL(PAYLOAD_COST_TO_VBYTES, 1);
    // Finality layer (finality_types.h)
    BOOST_CHECK_EQUAL(modern::FINALITY_SET_TREE_DEPTH, 13u);
    BOOST_CHECK_EQUAL(modern::MAX_FINALITY_SET, 8192u);
    BOOST_CHECK_EQUAL(modern::FINALITY_CERTIFICATE_RECORD_MAX, 1232u); // 112 + 1024 + 96
    BOOST_CHECK_EQUAL(modern::FINALITY_KEY_EVIDENCE_SIZE, 244u);
    BOOST_CHECK_EQUAL(modern::FINALITY_CERTIFICATE_VERIFY_COST, 2000);
    BOOST_CHECK_EQUAL(modern::FINALITY_KEY_EVIDENCE_VERIFY_COST, 700);
    // Policy numbers, permanently reserved
    BOOST_CHECK_EQUAL(static_cast<uint16_t>(modern::PolicyType::FINALITY_CERT), 6);
    BOOST_CHECK_EQUAL(static_cast<uint16_t>(modern::PolicyType::FINALITY_KEY), 7);
    BOOST_CHECK_EQUAL(static_cast<uint16_t>(modern::PolicyType::MODERN_PAYLOAD_ROOT), 8);
    // Reserved MPA record numbers
    BOOST_CHECK_EQUAL(modern::CREATION_ACTION_FINALITY_CERTIFICATE, 4);
    BOOST_CHECK_EQUAL(modern::CREATION_ACTION_FINALITY_KEY_EVIDENCE, 5);
    // The permanent policy-state bound is untouched.
    BOOST_CHECK_EQUAL(modern::MAX_POLICY_PARAMS_SIZE, 80u);
    BOOST_CHECK_LE(modern::FinalityKeyParams::SIZE, modern::MAX_POLICY_PARAMS_SIZE);
    BOOST_CHECK_LE(modern::FinalityCertParams::SIZE, modern::MAX_POLICY_PARAMS_SIZE);
}

BOOST_AUTO_TEST_CASE(modern_pos_params_finality_defaults_and_validity)
{
    Consensus::ModernPosParams p;
    BOOST_CHECK_EQUAL(p.finality_epoch_blocks, 1440);
    BOOST_CHECK_EQUAL(p.checkpoint_interval, 10);
    BOOST_CHECK_EQUAL(p.checkpoint_depth, 12);
    BOOST_CHECK_EQUAL(p.max_epoch_extension, 7 * 1440);
    BOOST_CHECK_EQUAL(p.min_finality_set, 4);
    BOOST_CHECK(p.Valid());
    { auto q{p}; q.checkpoint_interval = 0; BOOST_CHECK(!q.Valid()); }
    { auto q{p}; q.checkpoint_interval = q.finality_epoch_blocks + 1; BOOST_CHECK(!q.Valid()); }
    { auto q{p}; q.checkpoint_depth = q.finality_epoch_blocks; BOOST_CHECK(!q.Valid()); }
    { auto q{p}; q.max_epoch_extension = q.finality_epoch_blocks - 1; BOOST_CHECK(!q.Valid()); }
    { auto q{p}; q.min_finality_set = 0; BOOST_CHECK(!q.Valid()); }
    { auto q{p}; q.finality_epoch_blocks = 0; BOOST_CHECK(!q.Valid()); }
}

BOOST_AUTO_TEST_CASE(guardrails_nothing_activated)
{
    using modern::PolicyType;
    // Policy 6/7/8: never activated, with or without the test-only asset switch.
    for (const auto t : {PolicyType::FINALITY_CERT, PolicyType::FINALITY_KEY, PolicyType::MODERN_PAYLOAD_ROOT}) {
        for (const bool assets_active : {false, true}) {
            BOOST_CHECK(!modern::IsActivatedPolicy(static_cast<uint16_t>(t), modern::POLICY_VERSION_V1, assets_active));
            BOOST_CHECK(!modern::IsActivatedPolicy(static_cast<uint16_t>(t), 2, assets_active));
        }
    }
    // Existing activations unchanged.
    BOOST_CHECK(modern::IsActivatedPolicy(static_cast<uint16_t>(PolicyType::STAKE), modern::POLICY_VERSION_V1));
    BOOST_CHECK(modern::IsActivatedPolicy(static_cast<uint16_t>(PolicyType::OWNER), modern::POLICY_VERSION_V1));
    BOOST_CHECK(!modern::IsActivatedPolicy(static_cast<uint16_t>(PolicyType::FN), modern::POLICY_VERSION_V1));
    // Finality numbers 4/5 are MPA-only and not registered in the standalone
    // creation-action codec. Type 6 is the registered modern FN PoD action.
    BOOST_CHECK(!modern::IsKnownCreationAction(modern::CREATION_ACTION_FINALITY_CERTIFICATE, 1));
    BOOST_CHECK(!modern::IsKnownCreationAction(modern::CREATION_ACTION_FINALITY_KEY_EVIDENCE, 1));
    BOOST_CHECK(modern::IsKnownCreationAction(modern::CREATION_ACTION_MODERN_FN_POD, 1));
    // And the decoder still refuses a section naming them.
    modern::CreationAction a;
    a.action_type = modern::CREATION_ACTION_FINALITY_CERTIFICATE;
    a.action_version = 1;
    a.payload.assign(16, 0x00);
    BOOST_CHECK(!modern::EncodeCreationActionSection({a}).has_value());
}

BOOST_AUTO_TEST_CASE(keccak256_known_answers)
{
    // Ethereum Keccak-256 (NOT SHA3-256) known answers.
    BOOST_CHECK_EQUAL(Hex(KeccakOf("")), "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
    BOOST_CHECK_EQUAL(Hex(KeccakOf("abc")), "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45");
    // Multi-block input (> 136-byte rate), split across Write calls.
    const std::string s(200, 'a');
    uint256 one, two;
    Keccak256().Write(std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(s.data()), s.size())).Finalize(one);
    Keccak256 k;
    k.Write(std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(s.data()), 77));
    k.Write(std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(s.data()) + 77, s.size() - 77));
    k.Finalize(two);
    BOOST_CHECK(one == two);
    // Reset yields a fresh state.
    uint256 three;
    k.Reset().Finalize(three);
    BOOST_CHECK(three == KeccakOf(""));
}

BOOST_AUTO_TEST_CASE(fixed_width_codecs_round_trip_and_reject_lengths)
{
    modern::FinalizedBlock fb;
    fb.height = 0x0102030405060708ULL;
    fb.block_hash = uint256{"1111111111111111111111111111111111111111111111111111111111111111"};
    fb.withdrawal_root = uint256{"2222222222222222222222222222222222222222222222222222222222222222"};
    fb.validator_set_hash = uint256{"3333333333333333333333333333333333333333333333333333333333333333"};
    fb.epoch = 7;
    const auto enc{fb.Encode()};
    BOOST_CHECK_EQUAL(enc.size(), 112u); // 8+32+32+32+8
    BOOST_CHECK_EQUAL(Hex(std::span<const unsigned char>(enc.data(), 8)), "0102030405060708"); // big-endian
    BOOST_CHECK_EQUAL(Hex(std::span<const unsigned char>(enc.data() + 104, 8)), "0000000000000007");
    const auto dec{modern::FinalizedBlock::Decode(enc)};
    BOOST_REQUIRE(dec.has_value());
    BOOST_CHECK(*dec == fb);
    BOOST_CHECK(!modern::FinalizedBlock::Decode(std::span<const unsigned char>(enc.data(), 111)).has_value());
    std::vector<unsigned char> longer(enc.begin(), enc.end());
    longer.push_back(0);
    BOOST_CHECK(!modern::FinalizedBlock::Decode(longer).has_value());

    modern::ValidatorSetHeader h;
    h.epoch = 9;
    h.ruleset_version = modern::FINALITY_RULESET_V1;
    h.validator_count = 3500;
    h.total_weight = 1'000'000;
    h.quorum_weight = modern::QuorumWeightV1(h.total_weight);
    for (size_t i = 0; i < h.aggregate_pubkey.size(); ++i) h.aggregate_pubkey[i] = static_cast<unsigned char>(i);
    h.members_root = uint256{"4444444444444444444444444444444444444444444444444444444444444444"};
    const auto henc{h.Encode()};
    BOOST_CHECK_EQUAL(henc.size(), 110u); // 8+2+4+8+8+48+32
    BOOST_CHECK_EQUAL(Hex(std::span<const unsigned char>(henc.data() + 8, 2)), "0001");
    BOOST_CHECK_EQUAL(Hex(std::span<const unsigned char>(henc.data() + 10, 4)), "00000dac");
    const auto hdec{modern::ValidatorSetHeader::Decode(henc)};
    BOOST_REQUIRE(hdec.has_value());
    BOOST_CHECK(*hdec == h);
    BOOST_CHECK(!modern::ValidatorSetHeader::Decode(std::span<const unsigned char>(henc.data(), 109)).has_value());

    modern::FinalityKeyParams kp;
    for (size_t i = 0; i < kp.bls_pubkey.size(); ++i) kp.bls_pubkey[i] = static_cast<unsigned char>(0xA0 + i);
    kp.seq = 0x00010203;
    const auto kenc{kp.Encode()};
    BOOST_CHECK_EQUAL(kenc.size(), 52u);
    BOOST_CHECK_EQUAL(Hex(std::span<const unsigned char>(kenc.data() + 48, 4)), "00010203");
    const auto kdec{modern::FinalityKeyParams::Decode(kenc)};
    BOOST_REQUIRE(kdec.has_value());
    BOOST_CHECK(kdec->bls_pubkey == kp.bls_pubkey && kdec->seq == kp.seq);
    BOOST_CHECK(!kdec->IsRevocation());
    modern::FinalityKeyParams revoke; // all-zero key
    BOOST_CHECK(revoke.IsRevocation());
    BOOST_CHECK(!modern::FinalityKeyParams::Decode(std::span<const unsigned char>(kenc.data(), 51)).has_value());

    const modern::FinalityCertParams cp{.epoch = 5, .height = 123456};
    const auto cenc{cp.Encode()};
    BOOST_CHECK_EQUAL(cenc.size(), 16u);
    const auto cdec{modern::FinalityCertParams::Decode(cenc)};
    BOOST_REQUIRE(cdec.has_value());
    BOOST_CHECK_EQUAL(cdec->epoch, 5u);
    BOOST_CHECK_EQUAL(cdec->height, 123456u);

    modern::FinalityKeyEvidence ev;
    ev.validator_key.fill(0x11);
    ev.bls_pubkey.fill(0x22);
    ev.seq = 42;
    ev.bip340_sig.fill(0x33);
    ev.pop.fill(0x44);
    const auto eenc{ev.Encode()};
    BOOST_CHECK_EQUAL(eenc.size(), 244u);
    const auto edec{modern::FinalityKeyEvidence::Decode(eenc)};
    BOOST_REQUIRE(edec.has_value());
    BOOST_CHECK(edec->Encode() == eenc);
    BOOST_CHECK_EQUAL(edec->seq, 42u);
    BOOST_CHECK(!modern::FinalityKeyEvidence::Decode(std::span<const unsigned char>(eenc.data(), 243)).has_value());
}

BOOST_AUTO_TEST_CASE(signer_bitmap_rules_and_certificate_payload)
{
    using modern::SignerBitmapBytes;
    BOOST_CHECK_EQUAL(SignerBitmapBytes(1), 1u);
    BOOST_CHECK_EQUAL(SignerBitmapBytes(8), 1u);
    BOOST_CHECK_EQUAL(SignerBitmapBytes(9), 2u);
    BOOST_CHECK_EQUAL(SignerBitmapBytes(8192), 1024u);
    // n mod 8 in {0, 1, 7}
    BOOST_CHECK(modern::IsWellFormedSignerBitmap(std::vector<unsigned char>{0xFF}, 8));
    BOOST_CHECK(modern::IsWellFormedSignerBitmap(std::vector<unsigned char>{0x01}, 1));
    BOOST_CHECK(!modern::IsWellFormedSignerBitmap(std::vector<unsigned char>{0x02}, 1)); // bit 1 set, n = 1
    BOOST_CHECK(modern::IsWellFormedSignerBitmap(std::vector<unsigned char>{0x7F}, 7));
    BOOST_CHECK(!modern::IsWellFormedSignerBitmap(std::vector<unsigned char>{0x80}, 7));
    BOOST_CHECK(!modern::IsWellFormedSignerBitmap(std::vector<unsigned char>{0xFF, 0x00}, 8)); // wrong width
    BOOST_CHECK(!modern::IsWellFormedSignerBitmap(std::vector<unsigned char>{}, 0));
    BOOST_CHECK(!modern::IsWellFormedSignerBitmap(std::vector<unsigned char>(1025, 0), 8193));
    // LSB-first bit order
    const std::vector<unsigned char> bm{0x05, 0x80}; // bits 0,2 and 15
    BOOST_CHECK(modern::SignerBit(bm, 0));
    BOOST_CHECK(!modern::SignerBit(bm, 1));
    BOOST_CHECK(modern::SignerBit(bm, 2));
    BOOST_CHECK(modern::SignerBit(bm, 15));
    BOOST_CHECK(!modern::SignerBit(bm, 16)); // out of range reads 0

    modern::FinalizedBlock fb;
    fb.height = 100;
    fb.epoch = 1;
    modern::FinalityCertificate cert;
    cert.signer_bitmap = {0x0F, 0x01}; // n = 9
    cert.aggregate_sig.fill(0x99);
    const auto payload{modern::EncodeCertificatePayload(fb, cert)};
    BOOST_CHECK_EQUAL(payload.size(), 112u + 2 + 96);
    const auto dec{modern::DecodeCertificatePayload(payload, 9)};
    BOOST_REQUIRE(dec.has_value());
    BOOST_CHECK(dec->first == fb);
    BOOST_CHECK(dec->second.signer_bitmap == cert.signer_bitmap);
    BOOST_CHECK(dec->second.aggregate_sig == cert.aggregate_sig);
    BOOST_CHECK(!modern::DecodeCertificatePayload(payload, 8).has_value());  // width mismatch
    // (n = 10 shares the 2-byte width; the payload decodes under n = 10 too — the
    // binding of n to the epoch's set is a validation-layer rule, not a codec rule.)
    const auto dec10{modern::DecodeCertificatePayload(payload, 10)};
    BOOST_CHECK(dec10.has_value());
    std::vector<unsigned char> bad{payload};
    bad[112 + 1] = 0x02; // bit 9 set with n = 9 -> high-bit violation
    BOOST_CHECK(!modern::DecodeCertificatePayload(bad, 9).has_value());
    BOOST_CHECK(!modern::DecodeCertificatePayload(std::span<const unsigned char>(payload.data(), payload.size() - 1), 9).has_value());
    // Maximum set: 1,240-byte payload exactly.
    modern::FinalityCertificate big;
    big.signer_bitmap.assign(1024, 0xFF);
    const auto bigp{modern::EncodeCertificatePayload(fb, big)};
    BOOST_CHECK_EQUAL(bigp.size(), modern::FINALITY_CERTIFICATE_RECORD_MAX);
    BOOST_CHECK(modern::DecodeCertificatePayload(bigp, 8192).has_value());
}

BOOST_AUTO_TEST_CASE(quorum_rule_v1)
{
    using modern::QuorumWeightV1;
    BOOST_CHECK_EQUAL(QuorumWeightV1(0), 1u);
    BOOST_CHECK_EQUAL(QuorumWeightV1(1), 1u);
    BOOST_CHECK_EQUAL(QuorumWeightV1(2), 2u);
    BOOST_CHECK_EQUAL(QuorumWeightV1(3), 3u);
    BOOST_CHECK_EQUAL(QuorumWeightV1(4), 3u);
    BOOST_CHECK_EQUAL(QuorumWeightV1(5), 4u);
    BOOST_CHECK_EQUAL(QuorumWeightV1(6), 5u);
    BOOST_CHECK_EQUAL(QuorumWeightV1(100), 67u);
    BOOST_CHECK_EQUAL(QuorumWeightV1(101), 68u);
    BOOST_CHECK_EQUAL(QuorumWeightV1(3'000'000'000ULL), 2'000'000'001ULL);
    // Two quorums always overlap in more than W/3: q + q - W > W/3 for all W >= 1.
    for (uint64_t W : {1ULL, 2ULL, 3ULL, 4ULL, 7ULL, 100ULL, 1001ULL}) {
        const uint64_t q{QuorumWeightV1(W)};
        BOOST_CHECK_GT(3 * (2 * q - W), W);
    }
}

BOOST_AUTO_TEST_CASE(digests_are_deterministic_and_domain_separated)
{
    const uint256 domain{uint256{"abababababababababababababababababababababababababababababababab"}};
    modern::FinalizedBlock fb;
    fb.height = 821'001 + 10;
    fb.block_hash = uint256{"0101010101010101010101010101010101010101010101010101010101010101"};
    fb.validator_set_hash = uint256{"0202020202020202020202020202020202020202020202020202020202020202"};
    fb.epoch = 0;
    const uint256 d1{modern::FinalityDigest(domain, fb)};
    BOOST_CHECK(d1 == modern::FinalityDigest(domain, fb));
    // Pinned vector (generated by this implementation; any layout/tag change breaks it).
    BOOST_CHECK_EQUAL(Hex(d1), "9c20be9e0480313361d86dbcf1e80096da2b0142b3dd8e6de8cdb5f97d26571a");
    auto fb2{fb};
    fb2.epoch = 1;
    BOOST_CHECK(modern::FinalityDigest(domain, fb2) != d1);
    const uint256 other_domain{uint256{"cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"}};
    BOOST_CHECK(modern::FinalityDigest(other_domain, fb) != d1);

    std::array<unsigned char, 32> vk; vk.fill(0x77);
    std::array<unsigned char, 48> pk; pk.fill(0x88);
    const uint256 b0{modern::FinalityBindDigest(domain, vk, pk, 0)};
    const uint256 b1{modern::FinalityBindDigest(domain, vk, pk, 1)};
    BOOST_CHECK(b0 != b1);
    BOOST_CHECK(b0 != d1); // different tag
    BOOST_CHECK_EQUAL(Hex(b0), "33c2620b805a987e10d61bbb47fc7852ecbd63271211ac895af04f5f65536a70");

    const std::vector<unsigned char> payload(200, 0x5A);
    BOOST_CHECK_EQUAL(Hex(modern::FinalityCertCommitment(payload)), "0a5feae09bf868c5e2a6e97811b8c33c878c237614b15173d285c242b7b819f4");
}

BOOST_AUTO_TEST_CASE(validator_set_commitment)
{
    // Leaves: keccak(u32 i || pk48 || u64 w)
    std::array<unsigned char, 48> pk; pk.fill(0x01);
    const uint256 l0{modern::ValidatorSetLeaf(0, pk, 333)};
    const uint256 l0b{modern::ValidatorSetLeaf(0, pk, 333)};
    BOOST_CHECK(l0 == l0b);
    BOOST_CHECK(l0 != modern::ValidatorSetLeaf(1, pk, 333));
    BOOST_CHECK(l0 != modern::ValidatorSetLeaf(0, pk, 334));
    BOOST_CHECK_EQUAL(Hex(l0), "ce40c83f064af0dafd1cf7f6312e91bdea2eccedf09cf92f3f97a74d20d6ab65");
    // Root over zero leaves = tree of 8,192 zero leaves (deterministic constant)
    const auto empty_root{modern::ValidatorSetMembersRoot({})};
    BOOST_REQUIRE(empty_root.has_value());
    BOOST_CHECK_EQUAL(Hex(*empty_root), "c1df82d9c4b87413eae2ef048f94b4d3554cea73d92b0f7af96e0271c691e2bb");
    const auto one{modern::ValidatorSetMembersRoot({l0})};
    BOOST_REQUIRE(one.has_value());
    BOOST_CHECK(*one != *empty_root);
    // Padding semantics: a trailing zero leaf is indistinguishable from absence
    // (validator_count in the header disambiguates), but position matters.
    const auto two_a{modern::ValidatorSetMembersRoot({l0, uint256{}})};
    BOOST_CHECK(*two_a == *one);
    const auto two_b{modern::ValidatorSetMembersRoot({uint256{}, l0})};
    BOOST_CHECK(*two_b != *one);
    // Too many leaves is refused.
    BOOST_CHECK(!modern::ValidatorSetMembersRoot(std::vector<uint256>(8193)).has_value());
    BOOST_CHECK(modern::ValidatorSetMembersRoot(std::vector<uint256>(8192)).has_value());

    modern::ValidatorSetHeader h;
    h.epoch = 0;
    h.validator_count = 1;
    h.total_weight = 333;
    h.quorum_weight = modern::QuorumWeightV1(333);
    h.aggregate_pubkey = pk;
    h.members_root = *one;
    const uint256 sh{modern::ValidatorSetHash(h)};
    BOOST_CHECK(sh == modern::Keccak(h.Encode()));
    auto h2{h};
    h2.epoch = 1; // carry-over re-stamp changes the hash
    BOOST_CHECK(modern::ValidatorSetHash(h2) != sh);
    BOOST_CHECK_EQUAL(Hex(sh), "cf95ff9208d49a80e7dd75d2cf56227268cee65b7c810e832e5fe4f7ac6afbe8");
}

BOOST_AUTO_TEST_SUITE_END()
