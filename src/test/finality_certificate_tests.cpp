// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 10 of the Modern PoS V1 finality plan: FINALITY_CERTIFICATE
// validation over the frozen format -- bitmap, weighted quorum, successor set
// hash, domain, aggregate signature -- plus the coinbase cell/record binding.
// Not wired into block validation (no scheduling/epochs/pin/activation yet):
// the MPA registry keeps type 4 INACTIVE, which is asserted here.

#include <consensus/params.h>
#include <crypto/bls.h>
#include <modern/finality_certificate.h>
#include <modern/finality_types.h>
#include <modern/mpa.h>
#include <node/finality_binding_index.h>
#include <node/validator_set.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <map>
#include <vector>

using modern::CertificateCheck;
using modern::FinalityCertificate;
using modern::FinalizedBlock;
using modern::ValidatorSetView;
using node::FinalityBindingIndex;
using node::ValidatorSetSnapshot;

namespace {

const uint256 CHAIN_DOMAIN{uint256{"d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0"}};
const uint256 OTHER_CHAIN_DOMAIN{uint256{"e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1"}};
const uint256 SUCCESSOR{uint256{"abababababababababababababababababababababababababababababababab"}};
constexpr CAmount UNIT{modern::FINALITY_WEIGHT_UNIT};

bls::SecretKey BlsK(const unsigned i)
{
    std::array<unsigned char, 32> ikm{};
    ikm[0] = static_cast<unsigned char>(i);
    ikm[1] = static_cast<unsigned char>(i >> 8);
    ikm[31] = 0x7D;
    return *bls::SecretKey::FromIKM(ikm);
}

modern::ValidatorKeyBytes VK(const unsigned i) { modern::ValidatorKeyBytes k{}; k[0] = static_cast<unsigned char>(i); k[1] = static_cast<unsigned char>(i >> 8); k[31] = 0x33; return k; }

//! A synthetic set of n validators with the given weights (whole B3), built
//! through the real snapshot machinery so the view is the one a node would use.
struct SetUnderTest {
    std::vector<bls::SecretKey> keys;
    ValidatorSetSnapshot snapshot;
    ValidatorSetView view;
    SetUnderTest(const std::vector<uint64_t>& weights, const uint64_t epoch = 3)
        : snapshot{Make(weights, epoch, keys)}, view{snapshot.View()} {}
    static ValidatorSetSnapshot Make(const std::vector<uint64_t>& weights, const uint64_t epoch, std::vector<bls::SecretKey>& keys)
    {
        FinalityBindingIndex bindings;
        std::map<node::ValidatorKey, CAmount> w;
        std::vector<FinalityBindingIndex::Transition> ts;
        for (size_t i = 0; i < weights.size(); ++i) {
            keys.push_back(BlsK(static_cast<unsigned>(i + 1)));
            ts.push_back({VK(static_cast<unsigned>(i + 1)), {keys.back().GetPublicKey().Compressed(), 0, 1}});
            w[VK(static_cast<unsigned>(i + 1))] = static_cast<CAmount>(weights[i]) * UNIT;
        }
        bindings.ConnectBlock(1, ts);
        return *ValidatorSetSnapshot::Build(epoch, w, bindings);
    }
    //! Index of the key derived from seed i+1 inside the snapshot (members are sorted by validator_key).
    uint32_t IndexOfSeed(const unsigned i) const { return *snapshot.IndexOf(VK(i + 1)); }
};

FinalizedBlock Fb(const uint64_t height, const uint64_t epoch, const uint256& successor = SUCCESSOR)
{
    FinalizedBlock fb;
    fb.height = height;
    fb.block_hash = uint256{"0101010101010101010101010101010101010101010101010101010101010101"};
    fb.withdrawal_root = uint256{};
    fb.validator_set_hash = successor;
    fb.epoch = epoch;
    return fb;
}

//! Certificate signed by the given signer indices (into the snapshot order).
FinalityCertificate Sign(const SetUnderTest& set, const FinalizedBlock& fb, const std::vector<uint32_t>& signers,
                         const uint256& domain = CHAIN_DOMAIN)
{
    FinalityCertificate cert;
    cert.signer_bitmap.assign(modern::SignerBitmapBytes(set.view.validator_count), 0);
    const uint256 digest{modern::FinalityDigest(domain, fb)};
    std::vector<bls::Signature> sigs;
    for (const uint32_t idx : signers) {
        cert.signer_bitmap[idx / 8] |= static_cast<unsigned char>(1u << (idx % 8));
        // find the secret key whose public key is member idx
        const auto& pk{set.snapshot.Members()[idx].bls_pubkey};
        for (const auto& k : set.keys) {
            if (k.GetPublicKey().Compressed() == pk) { sigs.push_back(k.Sign(std::span<const unsigned char>(digest.begin(), 32))); break; }
        }
    }
    if (!sigs.empty()) cert.aggregate_sig = bls::AggregateSignatures(sigs)->Compressed();
    return cert;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(finality_certificate_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(valid_certificates_and_quorum_boundaries)
{
    // weights 3,3,3,1 -> W = 10, quorum = 7
    SetUnderTest set{{3, 3, 3, 1}};
    BOOST_CHECK_EQUAL(set.view.quorum_weight, 7u);
    const FinalizedBlock fb{Fb(1000, 3)};
    // all four signers
    const auto all{Sign(set, fb, {0, 1, 2, 3})};
    BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, all, set.view, SUCCESSOR) == CertificateCheck::OK);
    // find indices of the three weight-3 members and the weight-1 member
    std::vector<uint32_t> heavy, light;
    for (uint32_t i = 0; i < 4; ++i) (set.view.weights[i] == 3 ? heavy : light).push_back(i);
    BOOST_REQUIRE_EQUAL(heavy.size(), 3u);
    // exactly the three heavy signers: 9 >= 7 -> OK
    BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, Sign(set, fb, heavy), set.view, SUCCESSOR) == CertificateCheck::OK);
    // two heavy + light = 7 -> exactly the quorum -> OK
    BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, Sign(set, fb, {heavy[0], heavy[1], light[0]}), set.view, SUCCESSOR) == CertificateCheck::OK);
    // two heavy = 6 < 7 -> insufficient
    BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, Sign(set, fb, {heavy[0], heavy[1]}), set.view, SUCCESSOR) == CertificateCheck::INSUFFICIENT_WEIGHT);
    // the light one alone: insufficient
    BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, Sign(set, fb, light), set.view, SUCCESSOR) == CertificateCheck::INSUFFICIENT_WEIGHT);
    // nobody: no signers
    BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, Sign(set, fb, {}), set.view, SUCCESSOR) == CertificateCheck::NO_SIGNERS);
    // a single validator set: its own signature is the quorum
    SetUnderTest one{{5}};
    BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, Sign(one, fb, {0}), one.view, SUCCESSOR) == CertificateCheck::OK);
}

BOOST_AUTO_TEST_CASE(rejections)
{
    SetUnderTest set{{3, 3, 3, 1}};
    const FinalizedBlock fb{Fb(1000, 3)};
    const auto good{Sign(set, fb, {0, 1, 2, 3})};
    // wrong successor set hash in the signed object
    {
        const FinalizedBlock other{Fb(1000, 3, uint256{"0909090909090909090909090909090909090909090909090909090909090909"})};
        const auto c{Sign(set, other, {0, 1, 2, 3})};
        BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, other, c, set.view, SUCCESSOR) == CertificateCheck::WRONG_SUCCESSOR_SET);
    }
    // wrong domain: signatures made under another chain domain
    {
        const auto c{Sign(set, fb, {0, 1, 2, 3}, OTHER_CHAIN_DOMAIN)};
        BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, c, set.view, SUCCESSOR) == CertificateCheck::BAD_SIGNATURE);
        BOOST_CHECK(modern::VerifyFinalityCertificate(OTHER_CHAIN_DOMAIN, fb, c, set.view, SUCCESSOR) == CertificateCheck::OK);
    }
    // malformed bitmap: wrong width; high bit set (n = 4 -> bits 4..7 must be zero)
    {
        auto c{good};
        c.signer_bitmap.push_back(0x00);
        BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, c, set.view, SUCCESSOR) == CertificateCheck::MALFORMED_BITMAP);
        auto d{good};
        d.signer_bitmap[0] |= 0x10; // "signer" index 4 does not exist
        BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, d, set.view, SUCCESSOR) == CertificateCheck::MALFORMED_BITMAP);
        auto e{good};
        e.signer_bitmap.clear();
        BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, e, set.view, SUCCESSOR) == CertificateCheck::MALFORMED_BITMAP);
    }
    // bitmap claims a signer whose signature is not in the aggregate
    {
        auto c{Sign(set, fb, {0, 1, 2})};
        c.signer_bitmap[0] |= 0x08; // claims member 3 too
        BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, c, set.view, SUCCESSOR) == CertificateCheck::BAD_SIGNATURE);
    }
    // aggregate contains a signature the bitmap does not claim
    {
        auto c{Sign(set, fb, {0, 1, 2, 3})};
        c.signer_bitmap[0] &= ~0x08; // drop member 3 from the claim, keep its signature aggregated
        BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, c, set.view, SUCCESSOR) == CertificateCheck::BAD_SIGNATURE);
    }
    // a non-member's signature aggregated in
    {
        const auto outsider{BlsK(99)};
        const uint256 digest{modern::FinalityDigest(CHAIN_DOMAIN, fb)};
        auto c{Sign(set, fb, {0, 1, 2, 3})};
        const std::vector<bls::Signature> sigs{*bls::Signature::Decode(c.aggregate_sig), outsider.Sign(std::span<const unsigned char>(digest.begin(), 32))};
        c.aggregate_sig = bls::AggregateSignatures(sigs)->Compressed();
        BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, c, set.view, SUCCESSOR) == CertificateCheck::BAD_SIGNATURE);
    }
    // tampered / malformed aggregate signature
    {
        auto c{good};
        c.aggregate_sig[20] ^= 0x01;
        BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, c, set.view, SUCCESSOR) == CertificateCheck::BAD_SIGNATURE);
        auto d{good};
        d.aggregate_sig.fill(0);
        d.aggregate_sig[0] = 0xc0; // infinity encoding -> rejected at decode
        BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, d, set.view, SUCCESSOR) == CertificateCheck::BAD_SIGNATURE);
    }
    // signatures over a different FinalizedBlock (height) do not verify for this one
    {
        const auto c{Sign(set, Fb(1001, 3), {0, 1, 2, 3})};
        BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, c, set.view, SUCCESSOR) == CertificateCheck::BAD_SIGNATURE);
    }
    // inconsistent view fails closed
    {
        ValidatorSetView broken{set.view};
        broken.weights.pop_back();
        BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, good, broken, SUCCESSOR) == CertificateCheck::SET_VIEW_INCONSISTENT);
    }
}

BOOST_AUTO_TEST_CASE(wrong_set_same_bitmap_width)
{
    // Same n (4) but different members: the bitmap shape matches, the keys do not.
    SetUnderTest a{{3, 3, 3, 1}};
    std::vector<bls::SecretKey> other_keys;
    FinalityBindingIndex bindings;
    std::map<node::ValidatorKey, CAmount> w;
    std::vector<FinalityBindingIndex::Transition> ts;
    for (unsigned i = 0; i < 4; ++i) {
        other_keys.push_back(BlsK(500 + i));
        ts.push_back({VK(i + 1), {other_keys.back().GetPublicKey().Compressed(), 0, 1}});
        w[VK(i + 1)] = 3 * UNIT;
    }
    bindings.ConnectBlock(1, ts);
    const auto b{ValidatorSetSnapshot::Build(3, w, bindings)};
    BOOST_REQUIRE(b.has_value());
    const FinalizedBlock fb{Fb(1000, 3)};
    const auto cert{Sign(a, fb, {0, 1, 2, 3})};
    BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, cert, a.view, SUCCESSOR) == CertificateCheck::OK);
    BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, fb, cert, b->View(), SUCCESSOR) == CertificateCheck::BAD_SIGNATURE);
    BOOST_CHECK(a.snapshot.SetHash() != b->SetHash());
}

BOOST_AUTO_TEST_CASE(coinbase_cell_record_binding)
{
    SetUnderTest set{{3, 3, 3, 1}};
    const FinalizedBlock fb{Fb(1000, 3)};
    const auto cert{Sign(set, fb, {0, 1, 2, 3})};
    const auto [payload, cell] = modern::BuildFinalityCertificate(fb, cert);
    BOOST_CHECK_EQUAL(payload.size(), 112u + 1 + 96);
    CMpaRecord rec;
    rec.payload_type = modern::MPA_TYPE_FINALITY_CERTIFICATE;
    rec.payload_version = 1;
    rec.payload = payload;
    auto coinbase = [&](const std::vector<CScript>& cells, const std::vector<CMpaRecord>& records) {
        CMutableTransaction cb;
        cb.version = 2;
        cb.vin.resize(1);
        cb.vin[0].prevout.SetNull();
        cb.vout.emplace_back(0, CScript() << OP_TRUE);
        for (const auto& c : cells) cb.vout.emplace_back(0, c);
        cb.mpa = records;
        return CTransaction{cb};
    };
    std::string err;
    std::optional<modern::FinalityCertificatePair> pair;
    // neither: fine, no pair
    BOOST_CHECK(modern::MatchFinalityCertificate(coinbase({}, {}), 4, pair, err) && !pair.has_value());
    // valid pair
    BOOST_CHECK(modern::MatchFinalityCertificate(coinbase({cell}, {rec}), 4, pair, err));
    BOOST_REQUIRE(pair.has_value());
    BOOST_CHECK(pair->finalized_block == fb);
    BOOST_CHECK(pair->certificate.signer_bitmap == cert.signer_bitmap && pair->certificate.aggregate_sig == cert.aggregate_sig);
    BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, pair->finalized_block, pair->certificate, set.view, SUCCESSOR) == CertificateCheck::OK);
    // duplicates, orphan, cell-without-record
    BOOST_CHECK(!modern::MatchFinalityCertificate(coinbase({cell, cell}, {rec}), 4, pair, err)); BOOST_CHECK_EQUAL(err, "finality-cert-duplicate-cell");
    BOOST_CHECK(!modern::MatchFinalityCertificate(coinbase({cell}, {rec, rec}), 4, pair, err)); BOOST_CHECK_EQUAL(err, "finality-cert-duplicate-record");
    BOOST_CHECK(!modern::MatchFinalityCertificate(coinbase({}, {rec}), 4, pair, err)); BOOST_CHECK_EQUAL(err, "finality-cert-orphan-record");
    BOOST_CHECK(!modern::MatchFinalityCertificate(coinbase({cell}, {}), 4, pair, err)); BOOST_CHECK_EQUAL(err, "finality-cert-cell-without-record");
    // commitment mismatch (record payload differs from the cell's commitment)
    {
        CMpaRecord other{rec};
        other.payload[5] ^= 0x01;
        BOOST_CHECK(!modern::MatchFinalityCertificate(coinbase({cell}, {other}), 4, pair, err)); BOOST_CHECK_EQUAL(err, "finality-cert-commitment-mismatch");
    }
    // params mismatch (cell says another epoch/height than the payload)
    {
        const modern::FinalityCertParams wrong{.epoch = 9, .height = 1000};
        const auto bad_cell{*modern::MakeMetadataCellScript(6, 1, modern::FinalityCertCommitment(payload), wrong.Encode())};
        BOOST_CHECK(!modern::MatchFinalityCertificate(coinbase({bad_cell}, {rec}), 4, pair, err)); BOOST_CHECK_EQUAL(err, "finality-cert-params-mismatch");
    }
    // malformed payload for the set size (decoded for n = 9 -> wrong width)
    BOOST_CHECK(!modern::MatchFinalityCertificate(coinbase({cell}, {rec}), 9, pair, err)); BOOST_CHECK_EQUAL(err, "finality-cert-malformed-payload");
    // placement: a cell or record outside the coinbase is invalid
    {
        CBlock block;
        block.vtx.push_back(MakeTransactionRef(coinbase({cell}, {rec})));
        CMutableTransaction other;
        other.version = 2;
        other.vin.resize(1);
        other.vout.emplace_back(0, cell);
        block.vtx.push_back(MakeTransactionRef(other));
        BOOST_CHECK(!modern::CheckFinalityCertificatePlacement(block, err)); BOOST_CHECK_EQUAL(err, "finality-cert-not-in-coinbase");
        CMutableTransaction other2;
        other2.version = 2;
        other2.vin.resize(1);
        other2.vout.emplace_back(1, CScript() << OP_TRUE);
        other2.mpa = {rec};
        block.vtx[1] = MakeTransactionRef(other2);
        BOOST_CHECK(!modern::CheckFinalityCertificatePlacement(block, err)); BOOST_CHECK_EQUAL(err, "finality-cert-record-not-in-coinbase");
        block.vtx.pop_back();
        BOOST_CHECK(modern::CheckFinalityCertificatePlacement(block, err));
    }
    // The cell is a metadata cell: zero value, type 6, 16-byte params, commitment = hash(payload)
    const auto parsed{modern::ParseMetadataCell(cell)};
    BOOST_REQUIRE(parsed.has_value());
    BOOST_CHECK_EQUAL(parsed->policy_type, 6);
    BOOST_CHECK_EQUAL(parsed->params.size(), 16u);
    BOOST_CHECK(parsed->commitment == modern::FinalityCertCommitment(payload));
}

BOOST_AUTO_TEST_CASE(production_fail_closed_test_context_active)
{
    // Type 4 is verified end to end from Commit 12 (FinalityTracker) and is
    // therefore ACTIVE under the test MPA context; production stays
    // fail-closed (no network sets test_only_mpa_active) until the F = M
    // activation plumbing commit.
    Consensus::Params production{};
    production.legacy_b3coin = true;
    Consensus::Params test_ctx{production};
    test_ctx.test_only_mpa_active = true;
    BOOST_CHECK(modern::GetPayloadTypeStatus(4, 1, production) == modern::PayloadTypeStatus::INACTIVE);
    BOOST_CHECK(modern::GetPayloadTypeStatus(4, 1, test_ctx) == modern::PayloadTypeStatus::ACTIVE);
    CMutableTransaction m;
    m.version = 2;
    m.vin.resize(1);
    m.vout.emplace_back(0, CScript() << OP_TRUE);
    CMpaRecord rec;
    rec.payload_type = 4;
    rec.payload_version = 1;
    rec.payload.assign(300, 0);
    m.mpa = {rec};
    std::string err;
    BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{m}, production, err));
    BOOST_CHECK_EQUAL(err, "mpa-not-active");
    // Under the test context the frame passes the registry; an oversize
    // record (> 1,232) still fails the per-type size rule.
    BOOST_CHECK(modern::CheckTransactionMpa(CTransaction{m}, test_ctx, err));
    rec.payload.assign(modern::FINALITY_CERTIFICATE_RECORD_MAX + 1, 0);
    m.mpa = {rec};
    BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{m}, test_ctx, err));
    BOOST_CHECK_EQUAL(err, "mpa-bad-record-size");
    BOOST_CHECK(!modern::IsActivatedPolicy(6, 1));
}

BOOST_AUTO_TEST_SUITE_END()
