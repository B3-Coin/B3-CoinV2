// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 19 of the Modern PoS V1 finality plan: release qualification.
//
// 1. Consolidated cross-platform determinism vectors: every consensus digest,
//    commitment, tree and cost of the finality protocol pinned as explicit
//    hex so any platform / compiler / endianness divergence fails loudly.
// 2. Scale and bitmap coverage the small fixtures cannot give: a 512-member
//    set (multi-byte bitmaps, deep zero-padded keccak tree) verified and
//    attacked, and a 12-member set crossing the one-byte bitmap boundary.
// 3. A genuine multi-signer quorum end to end: three bound validators where
//    NO single validator reaches quorum, one abstains, two sign, any node
//    aggregates, the assembler includes, consensus finalizes and pins, and
//    a rebuild reproduces the identical state.

#include <chain.h>
#include <consensus/merkle.h>
#include <crypto/bls.h>
#include <modern/finality_certificate.h>
#include <modern/finality_schedule.h>
#include <modern/finality_types.h>
#include <modern/payload_cost.h>
#include <modern/payload_root.h>
#include <node/finality_signature.h>
#include <node/finality_tracker.h>
#include <node/miner.h>
#include <node/validator_set.h>
#include <test/util/finality_fixture.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <map>
#include <vector>

using b3test::FinalityChainFixture;
using node::FinalityBindingIndex;
using node::ValidatorSetSnapshot;

namespace {

const uint256 QUAL_DOMAIN{uint256{"b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3"}};

bls::SecretKey QualKey(const unsigned i)
{
    std::array<unsigned char, 32> ikm{};
    ikm[0] = static_cast<unsigned char>(i);
    ikm[1] = static_cast<unsigned char>(i >> 8);
    ikm[31] = 0x71;
    return *bls::SecretKey::FromIKM(ikm);
}

modern::ValidatorKeyBytes QualVk(const unsigned i)
{
    modern::ValidatorKeyBytes k{};
    k[0] = static_cast<unsigned char>(i);
    k[1] = static_cast<unsigned char>(i >> 8);
    k[31] = 0x51;
    return k;
}

//! A synthetic n-member set with weights w_i = (i % 7) + 1.
struct QualSet {
    std::vector<bls::SecretKey> keys_by_member; // index = member position
    ValidatorSetSnapshot snapshot;
    explicit QualSet(const unsigned n, const uint64_t epoch) : snapshot{Make(n, epoch, keys_by_member)} {}
    static ValidatorSetSnapshot Make(const unsigned n, const uint64_t epoch, std::vector<bls::SecretKey>& keys_by_member)
    {
        FinalityBindingIndex bindings;
        std::map<node::ValidatorKey, CAmount> weights;
        std::vector<FinalityBindingIndex::Transition> ts;
        std::vector<bls::SecretKey> keys;
        for (unsigned i = 0; i < n; ++i) {
            keys.push_back(QualKey(i + 1));
            ts.push_back({QualVk(i + 1), {keys.back().GetPublicKey().Compressed(), 0, 1}});
            weights[QualVk(i + 1)] = static_cast<CAmount>((i % 7) + 1) * modern::FINALITY_WEIGHT_UNIT;
        }
        bindings.ConnectBlock(1, ts);
        auto snap{*ValidatorSetSnapshot::Build(epoch, weights, bindings)};
        // Map secret keys to their member positions (members are key-sorted).
        keys_by_member.resize(n, keys[0]);
        for (unsigned i = 0; i < n; ++i) {
            keys_by_member[*snap.IndexOf(QualVk(i + 1))] = keys[i];
        }
        return snap;
    }
    modern::FinalityCertificate Sign(const modern::FinalizedBlock& fb, const std::vector<uint32_t>& signers) const
    {
        modern::FinalityCertificate cert;
        cert.signer_bitmap.assign(modern::SignerBitmapBytes(snapshot.Size()), 0);
        const uint256 digest{modern::FinalityDigest(QUAL_DOMAIN, fb)};
        std::vector<bls::Signature> sigs;
        for (const uint32_t index : signers) {
            cert.signer_bitmap[index / 8] |= static_cast<unsigned char>(1u << (index % 8));
            sigs.push_back(keys_by_member[index].Sign(std::span<const unsigned char>(digest.begin(), 32)));
        }
        cert.aggregate_sig = bls::AggregateSignatures(sigs)->Compressed();
        return cert;
    }
};

Consensus::BridgeDecentralizedWithdrawalPins BridgeThresholdPins(
    const uint32_t min_validators, const uint32_t max_validators,
    const uint64_t min_weight)
{
    Consensus::BridgeDecentralizedWithdrawalPins pins;
    pins.ethereum_verifier_address.fill(0x11);
    pins.ethereum_verifier_code_hash = uint256::ONE;
    pins.bootstrap_validator_set_hash = uint256{uint8_t{2}};
    pins.withdrawal_rules_version =
        Consensus::DECENTRALIZED_WITHDRAWAL_RULES_VERSION_V1;
    pins.withdrawal_rules_commitment = uint256{uint8_t{3}};
    pins.min_bridge_validators = min_validators;
    pins.max_bridge_validators = max_validators;
    pins.min_bridge_total_weight = min_weight;
    pins.max_epoch_lag = 1;
    return pins;
}

} // namespace

BOOST_AUTO_TEST_SUITE(finality_qualification_tests)

BOOST_FIXTURE_TEST_CASE(frozen_cross_platform_vectors, BasicTestingSetup)
{
    // ---- FinalizedBlock codec + digests (fixed-width, big-endian ints).
    modern::FinalizedBlock fb;
    fb.height = 821'000;
    fb.block_hash = uint256{"1111111111111111111111111111111111111111111111111111111111111111"};
    fb.withdrawal_root = uint256{};
    fb.validator_set_hash = uint256{"2222222222222222222222222222222222222222222222222222222222222222"};
    fb.epoch = 3;
    const auto enc{fb.Encode()};
    BOOST_CHECK_EQUAL(enc.size(), 112U);
    BOOST_CHECK_EQUAL(HexStr(enc).substr(0, 16), "00000000000c8708"); // u64 BE height
    BOOST_CHECK_EQUAL(HexStr(modern::FinalityDigest(QUAL_DOMAIN, fb)), "33ca0cbd7306e12efa55c2f3a2b507bbbfddc38007ba81ba38eea76820101110");
    const modern::ValidatorKeyBytes vk{QualVk(1)};
    modern::BlsPubkeyBytes pk{};
    pk[0] = 0xAB;
    pk[47] = 0xCD;
    BOOST_CHECK_EQUAL(HexStr(modern::FinalityBindDigest(QUAL_DOMAIN, vk, pk, /*seq=*/5)), "97d79eb8a2cb3ad3cc719980bf5ea87cf7dc68f2ca961e4fb8be7cf951626dbe");
    const std::vector<unsigned char> payload(200, 0x42);
    BOOST_CHECK_EQUAL(HexStr(modern::FinalityCertCommitment(payload)), "06407e0f2596ec0fdddf45ffb4fbefcebed4bd8fbc0e06a0256beb0764771ce6");

    // ---- Validator-set tree (keccak leaves, depth-13 zero padding, header hash).
    QualSet set4{4, /*epoch=*/9};
    BOOST_CHECK_EQUAL(set4.snapshot.Size(), 4U);
    BOOST_CHECK_EQUAL(set4.snapshot.TotalWeight(), 1U + 2U + 3U + 4U);
    BOOST_CHECK_EQUAL(set4.snapshot.QuorumWeight(), modern::QuorumWeightV1(10));
    BOOST_CHECK_EQUAL(HexStr(set4.snapshot.Leaves()[0]), "6d85e202c47f796ec0fa34ffcb3168bde9e6358d0d8019907fe9915530f8c543");
    BOOST_CHECK_EQUAL(HexStr(set4.snapshot.Header().members_root), "70cba023f4bc3c8eb65d8e64fe56a38bf13c9dd88532d15f9a7a88b29b876897");
    BOOST_CHECK_EQUAL(HexStr(set4.snapshot.SetHash()), "8231edb4d8ed3ca7f1a6f1c07e8ea293473aa5362908ecaa3d0f754ee9f50dd5");

    // ---- Payload commitment (section hash, BIG-ENDIAN leaf index, root).
    CMutableTransaction mtx;
    mtx.version = 2;
    mtx.vin.resize(1);
    mtx.vout.emplace_back(0, CScript() << OP_TRUE);
    CMpaRecord rec;
    rec.payload_type = modern::MPA_TYPE_FINALITY_CERTIFICATE;
    rec.payload_version = modern::MPA_VERSION_V1;
    rec.payload = payload;
    mtx.mpa = {rec};
    const CTransaction tx{mtx};
    BOOST_CHECK_EQUAL(HexStr(modern::MpaSectionHash(tx)), "5f476478632ac81c239eb50330753f1c181d396a626a694a363c15fbc157a7c0");
    BOOST_CHECK_EQUAL(HexStr(modern::PayloadLeaf(1, modern::MpaSectionHash(tx))), "d44e8217d25c2620b124e9be62ea38e43dc8866dc0f048096d627f6498998047");

    // ---- Deterministic verification-cost table and vsize arithmetic.
    BOOST_CHECK_EQUAL(modern::PayloadRecordVerifyCost(modern::MPA_TYPE_FINALITY_CERTIFICATE, modern::MPA_VERSION_V1), 2000);
    BOOST_CHECK_EQUAL(modern::PayloadRecordVerifyCost(modern::MPA_TYPE_FINALITY_KEY_EVIDENCE, modern::MPA_VERSION_V1), 700);
    BOOST_CHECK_EQUAL(modern::PayloadVerifyCost(tx), 2000);
    BOOST_CHECK_EQUAL(MAX_BLOCK_PAYLOAD_COST, 120'000);
    BOOST_CHECK_EQUAL(MAX_TX_PAYLOAD_COST, 12'000);
    BOOST_CHECK_EQUAL(PAYLOAD_COST_TO_VBYTES, 1);
}

BOOST_FIXTURE_TEST_CASE(large_set_and_multibyte_bitmaps, BasicTestingSetup)
{
    // 12 members: the bitmap crosses one byte; signers on both sides.
    {
        QualSet set{12, 2};
        modern::FinalizedBlock fb;
        fb.height = 500;
        fb.block_hash = uint256::ONE;
        fb.validator_set_hash = uint256{"3333333333333333333333333333333333333333333333333333333333333333"};
        fb.epoch = 2;
        std::vector<uint32_t> signers;
        for (uint32_t i = 0; i < 12; ++i) signers.push_back(i); // all: weight == W
        BOOST_CHECK(modern::VerifyFinalityCertificate(QUAL_DOMAIN, fb, set.Sign(fb, signers), set.snapshot.View(),
                                                      fb.validator_set_hash) == modern::CertificateCheck::OK);
        // A set bit beyond n (bit 12 of byte 1) is malformed.
        auto cert{set.Sign(fb, signers)};
        cert.signer_bitmap[1] |= 0x10;
        BOOST_CHECK(modern::VerifyFinalityCertificate(QUAL_DOMAIN, fb, cert, set.snapshot.View(),
                                                      fb.validator_set_hash) == modern::CertificateCheck::MALFORMED_BITMAP);
    }
    // 512 members: MAX-scale-representative tree (zero-padded to depth 13),
    // 64-byte bitmap, aggregate over hundreds of keys; quorum boundary exact.
    {
        QualSet set{512, 7};
        BOOST_CHECK_EQUAL(set.snapshot.Size(), 512U);
        const uint64_t W{set.snapshot.TotalWeight()};
        const uint64_t quorum{set.snapshot.QuorumWeight()};
        BOOST_CHECK_EQUAL(quorum, (2 * W) / 3 + 1);
        modern::FinalizedBlock fb;
        fb.height = 1'000;
        fb.block_hash = uint256::ONE;
        fb.validator_set_hash = uint256{"4444444444444444444444444444444444444444444444444444444444444444"};
        fb.epoch = 7;
        // Greedy: sign with members 0..k until both the stake quorum and the
        // cross-chain signer-count quorum are reached.
        std::vector<uint32_t> signers;
        uint64_t weight{0};
        const uint32_t headcount{modern::FinalityHeadcountQuorum(512)};
        for (uint32_t i = 0;
             i < 512 &&
             (weight < quorum || signers.size() < headcount);
             ++i) {
            signers.push_back(i);
            weight += set.snapshot.Members()[i].weight;
        }
        BOOST_REQUIRE_GE(weight, quorum);
        BOOST_REQUIRE_GE(signers.size(), headcount);
        const auto at_quorum{set.Sign(fb, signers)};
        BOOST_CHECK(modern::VerifyFinalityCertificate(QUAL_DOMAIN, fb, at_quorum, set.snapshot.View(),
                                                      fb.validator_set_hash) == modern::CertificateCheck::OK);
        // Dropping the last signer falls below quorum (exact boundary).
        const uint64_t last_weight{set.snapshot.Members()[signers.back()].weight};
        if (weight - last_weight < quorum) {
            std::vector<uint32_t> below{signers.begin(), signers.end() - 1};
            BOOST_CHECK(modern::VerifyFinalityCertificate(QUAL_DOMAIN, fb, set.Sign(fb, below), set.snapshot.View(),
                                                          fb.validator_set_hash) == modern::CertificateCheck::INSUFFICIENT_WEIGHT);
        }
        // A single flipped signature bit fails the aggregate.
        auto tampered{at_quorum};
        tampered.aggregate_sig[42] ^= 0x01;
        BOOST_CHECK(modern::VerifyFinalityCertificate(QUAL_DOMAIN, fb, tampered, set.snapshot.View(),
                                                      fb.validator_set_hash) == modern::CertificateCheck::BAD_SIGNATURE);
    }
}

BOOST_FIXTURE_TEST_CASE(three_validator_quorum_end_to_end, FinalityChainFixture)
{
    // A (15), B (1), C (10) all bound in the corridor: W = 26, quorum = 18 --
    // NO single validator reaches quorum.
    PrepareFinalityChain(/*min_finality_set=*/2, /*reorg_horizon=*/200,
                         /*with_unbound_c=*/false, /*with_bound_c=*/true);
    const int M{m_M};
    Produce(m_vk_a);
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE(s.bootstrapped);
        BOOST_CHECK_EQUAL(s.current->Size(), 3U);
        BOOST_CHECK_EQUAL(s.current->TotalWeight(), 26U);
        BOOST_CHECK_EQUAL(s.current->QuorumWeight(), 18U);
    }
    // Multi-producer chain: C produces too (bound + staked => eligible).
    Produce(m_vk_c);
    ProduceTo(M + 8, m_vk_a);
    const Consensus::Params& params{m_node.chainman->GetConsensus()};
    ChainstateManager& chainman{*m_node.chainman};

    node::BlockAssembler::Options options;
    options.coinbase_output_script = CScript() << OP_TRUE;
    options.include_dummy_extranonce = true;
    options.modern_pos_validator_key = m_vk_a;

    // A signs alone: 15 < 18 -- no certificate may be emitted; the chain
    // keeps moving without one (frozen extension behaviour).
    {
        LOCK(cs_main);
        Chainstate& cs{chainman.ActiveChainstate()};
        node::FinalitySigner sa;
        sa.SetKey(m_bls_a, m_vk_a);
        BOOST_CHECK(!sa.MaybeSign(Finality(), cs.m_chain, params, cs.FinalitySignatures()).empty());
        BOOST_CHECK(!cs.FinalitySignatures().BestCertificate(Finality(), cs.m_chain, params).has_value());
    }
    {
        const auto tmpl{node::BlockAssembler(chainman.ActiveChainstate(), nullptr, options).CreateNewBlock()};
        BOOST_REQUIRE(tmpl);
        BOOST_CHECK(!tmpl->block.vtx[0]->HasMpa());
    }
    // C joins (B still abstains): 25 >= 18 by weight, but two of three does
    // not satisfy the independent >2/3 headcount quorum. This is intentionally
    // identical to the Ethereum verifier's rule, so no certificate accepted by
    // B3 can become impossible to relay.
    {
        LOCK(cs_main);
        Chainstate& cs{chainman.ActiveChainstate()};
        node::FinalitySigner sc;
        sc.SetKey(m_bls_c, m_vk_c);
        BOOST_CHECK(!sc.MaybeSign(Finality(), cs.m_chain, params, cs.FinalitySignatures()).empty());
        BOOST_CHECK(!cs.FinalitySignatures().BestCertificate(Finality(), cs.m_chain, params).has_value());
    }
    // B supplies the third signature. Both the 18/26 weight threshold and the
    // 3/3 headcount threshold are now satisfied.
    {
        LOCK(cs_main);
        Chainstate& cs{chainman.ActiveChainstate()};
        node::FinalitySigner sb;
        sb.SetKey(m_bls_b, m_vk_b);
        BOOST_CHECK(!sb.MaybeSign(Finality(), cs.m_chain, params, cs.FinalitySignatures()).empty());
    }
    const auto tmpl{node::BlockAssembler(chainman.ActiveChainstate(), nullptr, options).CreateNewBlock()};
    BOOST_REQUIRE(tmpl);
    CBlock block{tmpl->block};
    {
        std::optional<modern::FinalityCertificatePair> pair;
        std::string err;
        BOOST_REQUIRE_MESSAGE(modern::MatchFinalityCertificate(*block.vtx[0], 3, pair, err), err);
        BOOST_REQUIRE(pair.has_value());
        BOOST_CHECK_EQUAL(pair->finalized_block.height, static_cast<uint64_t>(M + 5));
        const auto view{FinalityState().current->View()};
        BOOST_CHECK_EQUAL(modern::SignedWeight(pair->certificate.signer_bitmap, view), 26U);
    }
    block.hashMerkleRoot = BlockMerkleRoot(block);
    Sign(block, m_validator_a);
    if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
    BOOST_REQUIRE(Submit(block));
    BOOST_CHECK_EQUAL(FinalityState().finalized->height, M + 5);
    BOOST_CHECK(FinalityState().handover_certified);
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return m_node.chainman->m_blockman.FinalityAnchor())->first, M + 5);
    // Deterministic rebuild reproduces the identical multi-validator state.
    const auto before{FinalityState()};
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().ModernFinality().MarkDirty());
    const auto after{FinalityState()};
    BOOST_CHECK(before.finalized == after.finalized);
    BOOST_CHECK_EQUAL(before.current->SetHash().GetHex(), after.current->SetHash().GetHex());
}

BOOST_FIXTURE_TEST_CASE(bridge_withdrawal_readiness_threshold_boundaries,
                        BasicTestingSetup)
{
    const QualSet set3{3, 7};
    const QualSet set4{4, 7};
    const QualSet next4{4, 8};
    const QualSet next5{5, 8};

    node::FinalityTracker::State state;
    state.bootstrapped = true;
    state.epoch = 7;
    state.current =
        std::make_shared<const ValidatorSetSnapshot>(set4.snapshot);
    state.next =
        std::make_shared<const ValidatorSetSnapshot>(next4.snapshot);

    // Count and weight comparisons are inclusive, matching the immutable
    // Ethereum verifier. The four-member fixture weighs exactly 10.
    const auto exact{BridgeThresholdPins(4, 4, 10)};
    BOOST_REQUIRE(exact.Valid());
    BOOST_CHECK(node::BridgeWithdrawalValidatorSetsReady(state, exact));

    const auto weight_too_high{BridgeThresholdPins(4, 4, 11)};
    BOOST_REQUIRE(weight_too_high.Valid());
    BOOST_CHECK(!node::BridgeWithdrawalValidatorSetsReady(
        state, weight_too_high));

    state.current =
        std::make_shared<const ValidatorSetSnapshot>(set3.snapshot);
    const auto minimum_four{BridgeThresholdPins(4, 64, 1)};
    BOOST_CHECK(!node::BridgeWithdrawalValidatorSetsReady(
        state, minimum_four));

    state.current =
        std::make_shared<const ValidatorSetSnapshot>(set4.snapshot);
    state.next =
        std::make_shared<const ValidatorSetSnapshot>(next5.snapshot);
    BOOST_CHECK(!node::BridgeWithdrawalValidatorSetsReady(state, exact));
    const auto maximum_five{BridgeThresholdPins(4, 5, 10)};
    BOOST_CHECK(node::BridgeWithdrawalValidatorSetsReady(
        state, maximum_five));

    state.next.reset();
    BOOST_CHECK(!node::BridgeWithdrawalValidatorSetsReady(
        state, maximum_five));
    state.next =
        std::make_shared<const ValidatorSetSnapshot>(next4.snapshot);
    state.lineage_broken = true;
    BOOST_CHECK(!node::BridgeWithdrawalValidatorSetsReady(state, exact));
}

BOOST_AUTO_TEST_SUITE_END()
