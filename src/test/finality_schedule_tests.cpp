// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 11 of the Modern PoS V1 finality plan: checkpoint schedule, depth,
// the {current, current-1} epoch window, the epoch relation, ancestry,
// withdrawal-root and strict-monotone rules over a derived epoch view, and
// the epoch-aware coinbase matcher. Pure rule tests with real BLS sets; the
// derived epoch state machine and ConnectBlock wiring are Commit 12.

#include <consensus/modern_pos_params.h>
#include <crypto/bls.h>
#include <modern/finality_certificate.h>
#include <modern/finality_schedule.h>
#include <modern/finality_types.h>
#include <node/finality_binding_index.h>
#include <node/validator_set.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <map>
#include <vector>

using modern::CertificatePlacement;
using modern::FinalityCertificate;
using modern::FinalityEpochView;
using modern::FinalizedBlock;
using node::FinalityBindingIndex;
using node::ValidatorSetSnapshot;

namespace {

const uint256 CHAIN_DOMAIN{uint256{"d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0"}};
constexpr CAmount UNIT{modern::FINALITY_WEIGHT_UNIT};
constexpr int M{1000}; // synthetic modern-PoS start height

bls::SecretKey BlsK(const unsigned i)
{
    std::array<unsigned char, 32> ikm{};
    ikm[0] = static_cast<unsigned char>(i);
    ikm[31] = 0x5C;
    return *bls::SecretKey::FromIKM(ikm);
}
modern::ValidatorKeyBytes VK(const unsigned i) { modern::ValidatorKeyBytes k{}; k[0] = static_cast<unsigned char>(i); k[31] = 0x44; return k; }

struct SetUnderTest {
    std::vector<bls::SecretKey> keys;
    ValidatorSetSnapshot snapshot;
    SetUnderTest(const std::vector<uint64_t>& weights, const uint64_t epoch, const unsigned seed_base)
        : snapshot{Make(weights, epoch, seed_base, keys)} {}
    static ValidatorSetSnapshot Make(const std::vector<uint64_t>& weights, const uint64_t epoch, const unsigned seed_base,
                                     std::vector<bls::SecretKey>& keys)
    {
        FinalityBindingIndex bindings;
        std::map<node::ValidatorKey, CAmount> w;
        std::vector<FinalityBindingIndex::Transition> ts;
        for (size_t i = 0; i < weights.size(); ++i) {
            keys.push_back(BlsK(seed_base + static_cast<unsigned>(i)));
            ts.push_back({VK(seed_base + static_cast<unsigned>(i)), {keys.back().GetPublicKey().Compressed(), 0, 1}});
            w[VK(seed_base + static_cast<unsigned>(i))] = static_cast<CAmount>(weights[i]) * UNIT;
        }
        bindings.ConnectBlock(1, ts);
        return *ValidatorSetSnapshot::Build(epoch, w, bindings);
    }
    FinalityCertificate Sign(const FinalizedBlock& fb, const uint256& domain = CHAIN_DOMAIN) const
    {
        FinalityCertificate cert;
        cert.signer_bitmap.assign(modern::SignerBitmapBytes(snapshot.Size()), 0);
        const uint256 digest{modern::FinalityDigest(domain, fb)};
        std::vector<bls::Signature> sigs;
        for (uint32_t idx = 0; idx < snapshot.Size(); ++idx) {
            cert.signer_bitmap[idx / 8] |= static_cast<unsigned char>(1u << (idx % 8));
            const auto& pk{snapshot.Members()[idx].bls_pubkey};
            for (const auto& k : keys) {
                if (k.GetPublicKey().Compressed() == pk) { sigs.push_back(k.Sign(std::span<const unsigned char>(digest.begin(), 32))); break; }
            }
        }
        cert.aggregate_sig = bls::AggregateSignatures(sigs)->Compressed();
        return cert;
    }
};

//! A synthetic chain: hash_at(h) = H(h), deterministic.
uint256 HashAt(const int h)
{
    uint256 x{};
    x.begin()[0] = static_cast<unsigned char>(h);
    x.begin()[1] = static_cast<unsigned char>(h >> 8);
    x.begin()[31] = 0xC4;
    return x;
}
std::optional<uint256> ChainHashAt(const int h) { if (h < 0) return std::nullopt; return HashAt(h); }

Consensus::ModernPosParams Pos()
{
    Consensus::ModernPosParams pos{};
    pos.finality_epoch_blocks = 100;
    pos.checkpoint_interval = 10;
    pos.checkpoint_depth = 12;
    pos.max_epoch_extension = 700;
    pos.min_finality_set = 1;
    BOOST_REQUIRE(pos.Valid());
    return pos;
}

//! Epoch view: epoch 2 current (starts M, M+100, M+230 -- epoch 1 extended),
//! Set_1 previous, Set_2 current, Set_3 next.
struct World {
    SetUnderTest set1{{5, 5, 5, 5}, 1, 10};
    SetUnderTest set2{{7, 3, 2, 1, 1}, 2, 20};
    SetUnderTest set3{{4, 4, 4}, 3, 30};
    FinalityEpochView view;
    World()
    {
        view.current_epoch = 2;
        view.epoch_starts = {M, M + 100, M + 230};
        view.lineage_broken = false;
        view.finalized_height = M + 200; // the last certified checkpoint (by Set_1, epoch 1)
        view.current_set = &set2.snapshot.View();
        view.current_set_hash = set2.snapshot.SetHash();
        view.next_set_hash = set3.snapshot.SetHash();
        view.previous_set = &set1.snapshot.View();
    }
    FinalizedBlock Fb(const int height, const uint64_t epoch, const uint256& successor) const
    {
        FinalizedBlock fb;
        fb.height = static_cast<uint64_t>(height);
        fb.block_hash = HashAt(height);
        fb.withdrawal_root = uint256{};
        fb.validator_set_hash = successor;
        fb.epoch = epoch;
        return fb;
    }
    bool Judge(const FinalizedBlock& fb, const FinalityCertificate& cert, const int including, std::string& err) const
    {
        return modern::JudgeFinalityCertificate(CHAIN_DOMAIN, fb, cert, including, view, Pos(), ChainHashAt, err);
    }
    CertificatePlacement Place(const FinalizedBlock& fb, const int including) const
    {
        return modern::CheckCertificatePlacement(fb, including, view, Pos(), ChainHashAt);
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(finality_schedule_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(checkpoint_schedule_and_depth)
{
    // Checkpoints every CHECKPOINT_INTERVAL from M; M itself is one.
    BOOST_CHECK(modern::IsCheckpointHeight(M, M, 10));
    BOOST_CHECK(modern::IsCheckpointHeight(M + 10, M, 10));
    BOOST_CHECK(modern::IsCheckpointHeight(M + 1440, M, 10));
    BOOST_CHECK(!modern::IsCheckpointHeight(M + 1, M, 10));
    BOOST_CHECK(!modern::IsCheckpointHeight(M + 9, M, 10));
    BOOST_CHECK(!modern::IsCheckpointHeight(M - 10, M, 10)); // below M: never
    BOOST_CHECK(!modern::IsCheckpointHeight(M, M, 0));       // degenerate interval fails closed
    // Depth: inclusion at h_b requires h_b - h_c >= CHECKPOINT_DEPTH.
    BOOST_CHECK(modern::CheckpointDepthSatisfied(M + 10, M + 22, 12));
    BOOST_CHECK(modern::CheckpointDepthSatisfied(M + 10, M + 23, 12));
    BOOST_CHECK(!modern::CheckpointDepthSatisfied(M + 10, M + 21, 12));
    BOOST_CHECK(!modern::CheckpointDepthSatisfied(M + 10, M + 10, 12));
    BOOST_CHECK(!modern::CheckpointDepthSatisfied(M + 10, M + 9, 12)); // checkpoint above the block: never
    // Epoch of a height from the epoch-start table.
    const std::vector<int> starts{M, M + 100, M + 230};
    BOOST_CHECK(!modern::EpochOfHeight(starts, M - 1));
    BOOST_CHECK_EQUAL(*modern::EpochOfHeight(starts, M), 0U);
    BOOST_CHECK_EQUAL(*modern::EpochOfHeight(starts, M + 99), 0U);
    BOOST_CHECK_EQUAL(*modern::EpochOfHeight(starts, M + 100), 1U);
    BOOST_CHECK_EQUAL(*modern::EpochOfHeight(starts, M + 229), 1U); // extended epoch 1
    BOOST_CHECK_EQUAL(*modern::EpochOfHeight(starts, M + 230), 2U);
    BOOST_CHECK_EQUAL(*modern::EpochOfHeight(starts, M + 100'000), 2U);
}

BOOST_AUTO_TEST_CASE(current_epoch_certificates)
{
    World w;
    std::string err;
    // Epoch-2 checkpoints: M+230 (epoch start), M+240, ... ; certified with
    // Set_2 attesting hash(Set_3); included >= 12 deep; above the finalized height.
    {
        const auto fb{w.Fb(M + 240, 2, w.set3.snapshot.SetHash())};
        BOOST_CHECK(w.Judge(fb, w.set2.Sign(fb), M + 252, err));
        BOOST_CHECK(w.Judge(fb, w.set2.Sign(fb), M + 300, err)); // any later inclusion is fine
        // Exactly one below the depth: refused.
        BOOST_CHECK(!w.Judge(fb, w.set2.Sign(fb), M + 251, err));
        BOOST_CHECK_EQUAL(err, "insufficient-depth");
    }
    // A missed checkpoint is simply skipped: certifying M+260 directly after
    // M+200 is valid (strictly increasing, not necessarily consecutive).
    {
        const auto fb{w.Fb(M + 260, 2, w.set3.snapshot.SetHash())};
        BOOST_CHECK(w.Judge(fb, w.set2.Sign(fb), M + 272, err));
    }
    // Not on the schedule.
    {
        const auto fb{w.Fb(M + 245, 2, w.set3.snapshot.SetHash())};
        BOOST_CHECK(!w.Judge(fb, w.set2.Sign(fb), M + 300, err));
        BOOST_CHECK_EQUAL(err, "not-checkpoint");
    }
    // Wrong successor set hash (claims Set_2's own hash, or garbage).
    {
        const auto fb{w.Fb(M + 240, 2, w.set2.snapshot.SetHash())};
        BOOST_CHECK(!w.Judge(fb, w.set2.Sign(fb), M + 260, err));
        BOOST_CHECK_EQUAL(err, "wrong-successor-set");
    }
    // Signed by the wrong set (Set_1 signs an epoch-2 object): the bitmap
    // (4 of 5 bits) is well-formed for Set_2, so the signers resolve to Set_2
    // members whose keys did not sign -- it fails at the BLS stage, never
    // verifies against Set_1.
    {
        const auto fb{w.Fb(M + 240, 2, w.set3.snapshot.SetHash())};
        BOOST_CHECK(!w.Judge(fb, w.set1.Sign(fb), M + 260, err));
        BOOST_CHECK_EQUAL(err, "bad-signature");
    }
    // Wrong ancestry: the certified hash is not the block at that height on this chain.
    {
        auto fb{w.Fb(M + 240, 2, w.set3.snapshot.SetHash())};
        fb.block_hash = HashAt(M + 241);
        BOOST_CHECK(!w.Judge(fb, w.set2.Sign(fb), M + 260, err));
        BOOST_CHECK_EQUAL(err, "wrong-block-hash");
    }
    // Withdrawal root must be zero before bridge activation.
    {
        auto fb{w.Fb(M + 240, 2, w.set3.snapshot.SetHash())};
        fb.withdrawal_root.begin()[5] = 1;
        BOOST_CHECK(!w.Judge(fb, w.set2.Sign(fb), M + 260, err));
        BOOST_CHECK_EQUAL(err, "withdrawal-root-nonzero");
    }
    // Once the bridge supplies a cumulative root for the checkpoint, the
    // certificate must sign that exact root and validation fails closed when
    // the root index is absent.
    {
        auto fb{w.Fb(M + 240, 2, w.set3.snapshot.SetHash())};
        const uint256 expected_root{uint8_t{0x44}};
        fb.withdrawal_root = expected_root;
        const auto root_at = [&](const int height) -> std::optional<uint256> {
            return height == M + 240
                       ? std::optional<uint256>{expected_root}
                       : std::nullopt;
        };
        BOOST_CHECK(modern::JudgeFinalityCertificate(
            CHAIN_DOMAIN, fb, w.set2.Sign(fb), M + 260, w.view, Pos(),
            ChainHashAt, err, root_at));
        auto wrong{fb};
        wrong.withdrawal_root.begin()[0] ^= 1;
        BOOST_CHECK(!modern::JudgeFinalityCertificate(
            CHAIN_DOMAIN, wrong, w.set2.Sign(wrong), M + 260, w.view,
            Pos(), ChainHashAt, err, root_at));
        BOOST_CHECK_EQUAL(err, "withdrawal-root-mismatch");
        BOOST_CHECK(!modern::JudgeFinalityCertificate(
            CHAIN_DOMAIN, fb, w.set2.Sign(fb), M + 260, w.view, Pos(),
            ChainHashAt, err,
            [](int) { return std::optional<uint256>{}; }));
        BOOST_CHECK_EQUAL(err, "withdrawal-root-unavailable");
    }
    // Epoch relation: an epoch-2 object for a checkpoint inside epoch 1's span.
    {
        const auto fb{w.Fb(M + 220, 2, w.set3.snapshot.SetHash())};
        BOOST_CHECK(!w.Judge(fb, w.set2.Sign(fb), M + 300, err));
        BOOST_CHECK_EQUAL(err, "epoch-relation");
    }
    // Tampered signature reaches the BLS stage and fails there (full-cost path).
    {
        const auto fb{w.Fb(M + 240, 2, w.set3.snapshot.SetHash())};
        auto cert{w.set2.Sign(fb)};
        cert.aggregate_sig[10] ^= 0x01;
        BOOST_CHECK(!w.Judge(fb, cert, M + 260, err));
        BOOST_CHECK_EQUAL(err, "bad-signature");
    }
}

BOOST_AUTO_TEST_CASE(delayed_previous_epoch_certificates)
{
    World w;
    std::string err;
    // Epoch 1 ran M+100..M+229 (extended). Set_1 certificates attest hash(Set_2).
    // Accepted after rotation when strictly above the finalized height M+200:
    {
        const auto fb{w.Fb(M + 210, 1, w.set2.snapshot.SetHash())};
        BOOST_CHECK(w.Judge(fb, w.set1.Sign(fb), M + 240, err));
        const auto fb2{w.Fb(M + 220, 1, w.set2.snapshot.SetHash())};
        BOOST_CHECK(w.Judge(fb2, w.set1.Sign(fb2), M + 232, err));
    }
    // Regression: an old-set certificate at or below the finalized height can
    // never overwrite newer finalized state.
    {
        const auto fb{w.Fb(M + 200, 1, w.set2.snapshot.SetHash())};
        BOOST_CHECK(!w.Judge(fb, w.set1.Sign(fb), M + 240, err));
        BOOST_CHECK_EQUAL(err, "finality-regression");
        const auto fb2{w.Fb(M + 190, 1, w.set2.snapshot.SetHash())};
        BOOST_CHECK(!w.Judge(fb2, w.set1.Sign(fb2), M + 240, err));
        BOOST_CHECK_EQUAL(err, "finality-regression");
    }
    // Wrong set hash for a delayed certificate (must equal hash(Set_2), the
    // set that is current now).
    {
        const auto fb{w.Fb(M + 210, 1, w.set3.snapshot.SetHash())};
        BOOST_CHECK(!w.Judge(fb, w.set1.Sign(fb), M + 240, err));
        BOOST_CHECK_EQUAL(err, "wrong-successor-set");
    }
    // Epoch relation: an epoch-1 object for a checkpoint that lies in epoch 2.
    {
        const auto fb{w.Fb(M + 240, 1, w.set2.snapshot.SetHash())};
        BOOST_CHECK(!w.Judge(fb, w.set1.Sign(fb), M + 260, err));
        BOOST_CHECK_EQUAL(err, "epoch-relation");
    }
    // Outside the window: epoch 0 (current-2) and epoch 3 (current+1) are refused
    // before any cryptography.
    {
        const auto fb0{w.Fb(M + 210, 0, w.set2.snapshot.SetHash())};
        BOOST_CHECK(w.Place(fb0, M + 240) == CertificatePlacement::EPOCH_WINDOW);
        const auto fb3{w.Fb(M + 240, 3, w.set3.snapshot.SetHash())};
        BOOST_CHECK(w.Place(fb3, M + 260) == CertificatePlacement::EPOCH_WINDOW);
    }
    // Depth applies to delayed certificates identically.
    {
        const auto fb{w.Fb(M + 220, 1, w.set2.snapshot.SetHash())};
        BOOST_CHECK(w.Place(fb, M + 231) == CertificatePlacement::INSUFFICIENT_DEPTH);
        BOOST_CHECK(w.Place(fb, M + 232) == CertificatePlacement::OK);
    }
}

BOOST_AUTO_TEST_CASE(epoch_zero_and_fail_closed_states)
{
    // Epoch 0 has no previous set: the window is {0} only.
    SetUnderTest set0{{3, 3, 3, 3}, 0, 40};
    SetUnderTest set1{{3, 3, 3, 3}, 1, 40}; // same members re-stamped (carry-over shape)
    FinalityEpochView view;
    view.current_epoch = 0;
    view.epoch_starts = {M};
    view.current_set = &set0.snapshot.View();
    view.current_set_hash = set0.snapshot.SetHash();
    view.next_set_hash = set1.snapshot.SetHash();
    const auto pos{Pos()};
    FinalizedBlock fb;
    fb.height = M + 20;
    fb.block_hash = HashAt(M + 20);
    fb.validator_set_hash = set1.snapshot.SetHash();
    fb.epoch = 0;
    std::string err;
    BOOST_CHECK(modern::JudgeFinalityCertificate(CHAIN_DOMAIN, fb, set0.Sign(fb), M + 32, view, pos, ChainHashAt, err));
    // The first checkpoint is M itself.
    FinalizedBlock first{fb};
    first.height = M;
    first.block_hash = HashAt(M);
    BOOST_CHECK(modern::JudgeFinalityCertificate(CHAIN_DOMAIN, first, set0.Sign(first), M + 12, view, pos, ChainHashAt, err));
    BOOST_CHECK(!modern::JudgeFinalityCertificate(CHAIN_DOMAIN, first, set0.Sign(first), M + 11, view, pos, ChainHashAt, err));
    // "current - 1" does not exist at epoch 0 (u64 wrap must not open a window).
    FinalizedBlock wrapped{fb};
    wrapped.epoch = std::numeric_limits<uint64_t>::max();
    BOOST_CHECK(modern::CheckCertificatePlacement(wrapped, M + 32, view, pos, ChainHashAt) == CertificatePlacement::EPOCH_WINDOW);
    // Lineage broken: nothing is valid any more, even a perfect certificate.
    view.lineage_broken = true;
    BOOST_CHECK(!modern::JudgeFinalityCertificate(CHAIN_DOMAIN, fb, set0.Sign(fb), M + 32, view, pos, ChainHashAt, err));
    BOOST_CHECK_EQUAL(err, "lineage-broken");
    view.lineage_broken = false;
    // No set (bootstrap floor not met): nothing can be certified.
    FinalityEpochView empty;
    BOOST_CHECK(modern::CheckCertificatePlacement(fb, M + 32, empty, pos, ChainHashAt) == CertificatePlacement::NO_FINALITY_SET);
    // Ancestry unavailable (hash_at returns nullopt): fail closed.
    BOOST_CHECK(modern::CheckCertificatePlacement(fb, M + 32, view, pos, [](int) { return std::optional<uint256>{}; }) == CertificatePlacement::WRONG_BLOCK_HASH);
}

BOOST_AUTO_TEST_CASE(epoch_aware_coinbase_matcher)
{
    // The bitmap width is resolved from the certificate's own epoch: a
    // 4-member epoch and a 5-member epoch decode differently.
    SetUnderTest set4{{1, 1, 1, 1}, 1, 50};
    SetUnderTest set5{{1, 1, 1, 1, 1}, 2, 60};
    const auto resolver{[&](uint64_t epoch) -> std::optional<uint32_t> {
        if (epoch == 1) return 4;
        if (epoch == 2) return 5;
        return std::nullopt;
    }};
    FinalizedBlock fb;
    fb.height = M + 10;
    fb.block_hash = HashAt(M + 10);
    fb.epoch = 2;
    const auto [payload, cell] = modern::BuildFinalityCertificate(fb, set5.Sign(fb));
    CMutableTransaction cb;
    cb.vin.resize(1);
    cb.vin[0].prevout.SetNull();
    cb.vout.emplace_back(0, cell);
    CMpaRecord rec;
    rec.payload_type = modern::MPA_TYPE_FINALITY_CERTIFICATE;
    rec.payload_version = modern::MPA_VERSION_V1;
    rec.payload = payload;
    cb.mpa.push_back(rec);
    std::optional<modern::FinalityCertificatePair> out;
    std::string err;
    BOOST_REQUIRE(modern::MatchFinalityCertificateForEpoch(CTransaction{cb}, resolver, out, err));
    BOOST_REQUIRE(out.has_value());
    BOOST_CHECK_EQUAL(out->finalized_block.epoch, 2U);
    BOOST_CHECK_EQUAL(out->certificate.signer_bitmap.size(), 1U);
    // A fixed wrong width is malformed; an epoch without a set is refused.
    BOOST_CHECK(!modern::MatchFinalityCertificate(CTransaction{cb}, 9, out, err));
    BOOST_CHECK_EQUAL(err, "finality-cert-malformed-payload");
    fb.epoch = 7;
    const auto [p7, c7] = modern::BuildFinalityCertificate(fb, set5.Sign(fb));
    cb.vout[0].scriptPubKey = c7;
    cb.mpa[0].payload = p7;
    BOOST_CHECK(!modern::MatchFinalityCertificateForEpoch(CTransaction{cb}, resolver, out, err));
    BOOST_CHECK_EQUAL(err, "finality-cert-unknown-epoch-set");
    // A payload shorter than the FinalizedBlock prefix is malformed.
    cb.mpa[0].payload.resize(50);
    BOOST_CHECK(!modern::MatchFinalityCertificateForEpoch(CTransaction{cb}, resolver, out, err));
    BOOST_CHECK_EQUAL(err, "finality-cert-malformed-payload");
    // No certificate at all: nullopt, no error.
    cb.mpa.clear();
    cb.vout.resize(0);
    BOOST_CHECK(modern::MatchFinalityCertificateForEpoch(CTransaction{cb}, resolver, out, err));
    BOOST_CHECK(!out.has_value());
}

BOOST_AUTO_TEST_SUITE_END()
