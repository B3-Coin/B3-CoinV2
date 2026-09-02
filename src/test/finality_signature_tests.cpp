// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 15 of the Modern PoS V1 finality plan: the BLS finality message
// path -- signer (scheduled checkpoints only, once, active-set key only),
// signature pool (cheap checks before BLS, dedup, bounds, stale pruning),
// leaderless aggregation to a consensus-valid certificate. Liveness only:
// the pool never touches consensus state.

#include <chain.h>
#include <modern/finality_certificate.h>
#include <modern/finality_schedule.h>
#include <node/finality_signature.h>
#include <node/finality_tracker.h>
#include <streams.h>
#include <test/util/finality_fixture.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

using b3test::FinalityChainFixture;
using node::FinalitySig;
using node::FinalitySignaturePool;
using Accept = node::FinalitySignaturePool::Accept;

namespace {

//! Hand-built signature message for (epoch, height) by `sk`, claiming `index`.
FinalitySig Sig(const node::FinalityTracker::State& state, const CChain& chain, const uint256& domain,
                const uint64_t epoch, const uint64_t height, const uint32_t index, const bls::SecretKey& sk)
{
    FinalitySig sig;
    sig.epoch = epoch;
    sig.height = height;
    sig.index = index;
    const Consensus::Params bridge_inactive{};
    if (const auto fb{FinalitySignaturePool::ExpectedFinalizedBlock(
            epoch, height, state, chain, bridge_inactive)}) {
        const uint256 digest{modern::FinalityDigest(domain, *fb)};
        sig.signature = sk.Sign(std::span<const unsigned char>(digest.begin(), 32)).Compressed();
    }
    return sig;
}

} // namespace

BOOST_AUTO_TEST_SUITE(finality_signature_tests)

BOOST_FIXTURE_TEST_CASE(pool_verifies_dedupes_bounds_and_aggregates, FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    ProduceTo(M + 8, m_vk_a);
    const Consensus::Params& params{m_node.chainman->GetConsensus()};
    LOCK(cs_main);
    const CChain& chain{m_node.chainman->ActiveChain()};
    node::FinalityTracker& tracker{Finality()};
    const auto& state{tracker.Current()};
    const auto idx_a{*state.current->IndexOf(m_vk_a)};
    const auto idx_b{*state.current->IndexOf(m_vk_b)};
    FinalitySignaturePool pool;

    // Wire round trip: fixed 116 bytes.
    {
        const FinalitySig s{Sig(state, chain, m_domain, 0, M + 5, idx_a, m_bls_a)};
        DataStream ds;
        ds << s;
        BOOST_CHECK_EQUAL(ds.size(), 116U);
        FinalitySig back;
        ds >> back;
        BOOST_CHECK(back == s);
    }

    // Accept, dedup, and every cheap rejection before BLS.
    BOOST_CHECK(pool.Submit(Sig(state, chain, m_domain, 0, M + 5, idx_a, m_bls_a), tracker, chain, params) == Accept::ACCEPTED);
    BOOST_CHECK(pool.Submit(Sig(state, chain, m_domain, 0, M + 5, idx_a, m_bls_a), tracker, chain, params) == Accept::DUPLICATE);
    BOOST_CHECK(pool.Submit(Sig(state, chain, m_domain, 0, M + 5, idx_b, m_bls_a), tracker, chain, params) == Accept::BAD_SIGNATURE); // A's sig under B's index
    BOOST_CHECK(pool.Submit(Sig(state, chain, m_domain, 0, M + 5, 7, m_bls_a), tracker, chain, params) == Accept::BAD_INDEX);
    BOOST_CHECK(pool.Submit(Sig(state, chain, m_domain, 0, M + 6, idx_a, m_bls_a), tracker, chain, params) == Accept::NOT_CHECKPOINT);
    BOOST_CHECK(pool.Submit(Sig(state, chain, m_domain, 0, M + 20, idx_a, m_bls_a), tracker, chain, params) == Accept::NOT_CHECKPOINT); // above the tip
    BOOST_CHECK(pool.Submit(Sig(state, chain, m_domain, 1, M + 5, idx_a, m_bls_a), tracker, chain, params) == Accept::UNKNOWN_EPOCH);
    {
        FinalitySig garbage{Sig(state, chain, m_domain, 0, M + 5, idx_b, m_bls_b)};
        garbage.signature[20] ^= 0x01;
        BOOST_CHECK(pool.Submit(garbage, tracker, chain, params) == Accept::BAD_SIGNATURE);
    }
    // A alone has 15 of 16 weight, but a two-member set requires both
    // validators. This matches the immutable Ethereum bridge prover.
    BOOST_CHECK(!pool.BestCertificate(tracker, chain, params).has_value());
    // B joins: both quorums are met and the bitmap carries both signers.
    BOOST_CHECK(pool.Submit(Sig(state, chain, m_domain, 0, M + 5, idx_b, m_bls_b), tracker, chain, params) == Accept::ACCEPTED);
    BOOST_CHECK_EQUAL(pool.SignatureCount(0, M + 5), 2U);
    {
        const auto best{pool.BestCertificate(tracker, chain, params)};
        BOOST_REQUIRE(best.has_value());
        BOOST_CHECK(modern::VerifyFinalityCertificate(m_domain, best->first, best->second, state.current->View(),
                                                      state.next->SetHash()) == modern::CertificateCheck::OK);
        BOOST_CHECK_EQUAL(modern::SignedWeight(best->second.signer_bitmap, state.current->View()), 16U);
    }
    // B alone never reaches quorum.
    {
        FinalitySignaturePool weak;
        BOOST_CHECK(weak.Submit(Sig(state, chain, m_domain, 0, M + 5, idx_b, m_bls_b), tracker, chain, params) == Accept::ACCEPTED);
        BOOST_CHECK(!weak.BestCertificate(tracker, chain, params).has_value());
    }
}

BOOST_FIXTURE_TEST_CASE(pool_depth_stale_and_slot_bounds, FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    ProduceTo(M + 11, m_vk_a); // M+10 is a checkpoint but only 1 deep
    const Consensus::Params& params{m_node.chainman->GetConsensus()};
    FinalitySignaturePool pool;
    {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        node::FinalityTracker& tracker{Finality()};
        const auto idx_a{*tracker.Current().current->IndexOf(m_vk_a)};
        BOOST_CHECK(pool.Submit(Sig(tracker.Current(), chain, m_domain, 0, M + 10, idx_a, m_bls_a), tracker, chain, params) == Accept::TOO_SHALLOW);
    }
    // Slot bound: after 9 signable checkpoints, retain the newest
    // MAX_TRACKED_CHECKPOINTS so a quorum outage cannot freeze progress.
    ProduceTo(M + 43, m_vk_a); // checkpoints M .. M+40 all >= 3 deep
    {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        node::FinalityTracker& tracker{Finality()};
        const auto idx_a{*tracker.Current().current->IndexOf(m_vk_a)};
        const auto& state{tracker.Current()};
        for (int i{0}; i <= 8; ++i) {
            BOOST_CHECK(pool.Submit(
                            Sig(state, chain, m_domain, 0, M + 5 * i,
                                idx_a, m_bls_a),
                            tracker, chain, params) == Accept::ACCEPTED);
            BOOST_CHECK_LE(pool.TrackedCheckpoints(),
                           FinalitySignaturePool::MAX_TRACKED_CHECKPOINTS);
        }
        BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(), FinalitySignaturePool::MAX_TRACKED_CHECKPOINTS);
        BOOST_CHECK_EQUAL(pool.SignatureCount(0, M), 0U);
        BOOST_CHECK_EQUAL(pool.SignatureCount(0, M + 40), 1U);
        // A valid old replay cannot evict newer retained work.
        BOOST_CHECK(pool.Submit(
                        Sig(state, chain, m_domain, 0, M, idx_a, m_bls_a),
                        tracker, chain, params) == Accept::POOL_FULL);
        // A local signer may encounter the same full pool after catching up.
        // The evicted oldest checkpoint is skipped, while retained duplicates
        // are safe to relay and advance the local watermark through M+40.
        node::FinalitySigner signer;
        signer.SetKey(m_bls_a, m_vk_a);
        const auto before_prune{signer.MaybeSign(tracker, chain, params, pool)};
        BOOST_REQUIRE_EQUAL(before_prune.size(),
                            FinalitySignaturePool::MAX_TRACKED_CHECKPOINTS);
        BOOST_CHECK_EQUAL(before_prune.front().height,
                          static_cast<uint64_t>(M + 5));
        BOOST_CHECK_EQUAL(before_prune.back().height,
                          static_cast<uint64_t>(M + 40));
        BOOST_CHECK_EQUAL(signer.LastSignedHeight(), M + 40);
    }

    // A still newer checkpoint remains collectable without a finalization or
    // restart and evicts only the oldest retained slot.
    ProduceTo(M + 48, m_vk_a);
    {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        node::FinalityTracker& tracker{Finality()};
        const auto idx_a{*tracker.Current().current->IndexOf(m_vk_a)};
        FinalitySig invalid_newer{Sig(
            tracker.Current(), chain, m_domain, 0, M + 45, idx_a,
            m_bls_a)};
        invalid_newer.signature[20] ^= 0x01;
        BOOST_CHECK(pool.Submit(invalid_newer, tracker, chain, params) ==
                    Accept::BAD_SIGNATURE);
        // A bad newer signature cannot evict valid bounded work.
        BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(),
                          FinalitySignaturePool::MAX_TRACKED_CHECKPOINTS);
        BOOST_CHECK_EQUAL(pool.SignatureCount(0, M + 5), 1U);
        BOOST_CHECK_EQUAL(pool.SignatureCount(0, M + 45), 0U);
        BOOST_CHECK(pool.Submit(
                        Sig(tracker.Current(), chain, m_domain, 0, M + 45,
                            idx_a, m_bls_a),
                        tracker, chain, params) == Accept::ACCEPTED);
        BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(),
                          FinalitySignaturePool::MAX_TRACKED_CHECKPOINTS);
        BOOST_CHECK_EQUAL(pool.SignatureCount(0, M + 5), 0U);
        BOOST_CHECK_EQUAL(pool.SignatureCount(0, M + 45), 1U);

        // Finalizing still prunes every slot at or below the new finality pin.
        pool.Prune(M + 20);
        BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(), 5U);
    }
}

BOOST_FIXTURE_TEST_CASE(signer_signs_each_scheduled_checkpoint_once_with_the_active_key, FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    ProduceTo(M + 8, m_vk_a);
    const Consensus::Params& params{m_node.chainman->GetConsensus()};
    FinalitySignaturePool pool;
    node::FinalitySigner signer;
    {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        node::FinalityTracker& tracker{Finality()};
        BOOST_CHECK(signer.MaybeSign(tracker, chain, params, pool).empty()); // no key yet
        signer.SetKey(m_bls_a, m_vk_a);

        // Signable now: M and M+5 (tip M+8, depth 3). Signed once, self-submitted.
        const auto sigs{signer.MaybeSign(tracker, chain, params, pool)};
        BOOST_REQUIRE_EQUAL(sigs.size(), 2U);
        BOOST_CHECK_EQUAL(sigs[0].height, static_cast<uint64_t>(M));
        BOOST_CHECK_EQUAL(sigs[1].height, static_cast<uint64_t>(M + 5));
        BOOST_CHECK_EQUAL(pool.SignatureCount(0, M), 1U);
        BOOST_CHECK_EQUAL(pool.SignatureCount(0, M + 5), 1U);
        BOOST_CHECK(signer.MaybeSign(tracker, chain, params, pool).empty()); // one signature per checkpoint
        // Every produced signature verifies in a fresh pool (independent aggregator).
        FinalitySignaturePool other;
        for (const auto& s : sigs) BOOST_CHECK(other.Submit(s, tracker, chain, params) == Accept::ACCEPTED);
    }
    // The chain advances: exactly the newly signable checkpoint is signed.
    ProduceTo(M + 13, m_vk_a);
    {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        node::FinalityTracker& tracker{Finality()};
        const auto sigs{signer.MaybeSign(tracker, chain, params, pool)};
        BOOST_REQUIRE_EQUAL(sigs.size(), 1U);
        BOOST_CHECK_EQUAL(sigs[0].height, static_cast<uint64_t>(M + 10));
        // A signer whose BLS key is NOT the one recorded by the active snapshot
        // (e.g. freshly rotated, not yet in force) signs nothing.
        node::FinalitySigner rotated;
        rotated.SetKey(Bls(9), m_vk_a);
        BOOST_CHECK(rotated.MaybeSign(tracker, chain, params, pool).empty());
        // A non-member signs nothing.
        node::FinalitySigner outsider;
        outsider.SetKey(Bls(8), m_vk_c);
        BOOST_CHECK(outsider.MaybeSign(tracker, chain, params, pool).empty());
    }
}

BOOST_FIXTURE_TEST_CASE(durable_signer_survives_restart_locks_forks_and_rebases_only_on_included_certificate,
                        FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    const Consensus::Params& params{m_node.chainman->GetConsensus()};
    const fs::path store_dir{m_path_root / "durable-finality-signer"};

    // Arm before the first checkpoint can be signed. The first call writes a
    // durable empty marker, distinguishing this fresh signer from a deleted
    // post-signing journal.
    node::FinalitySigner original;
    std::string error;
    BOOST_REQUIRE_MESSAGE(original.SetKeyPersistent(
                              m_bls_a, m_vk_a, m_domain, store_dir, error),
                          error);
    {
        LOCK(cs_main);
        FinalitySignaturePool initial_pool;
        BOOST_CHECK(original.MaybeSign(
                        Finality(), m_node.chainman->ActiveChain(), params,
                        initial_pool)
                        .empty());
        BOOST_CHECK(original.LastError().empty());
    }

    ProduceTo(M + 8, m_vk_a);
    // An absent journal first seen after checkpoints became signable is not
    // immediately a safe "fresh install": it is indistinguishable from
    // deletion of a vote on a competing corridor-derived Set0. It must fail
    // closed while no included certificate pins the active branch.
    node::FinalitySigner late_without_journal;
    error.clear();
    BOOST_REQUIRE_MESSAGE(late_without_journal.SetKeyPersistent(
                              m_bls_a, m_vk_a, m_domain,
                              store_dir / "late", error),
                          error);
    {
        LOCK(cs_main);
        FinalitySignaturePool late_pool;
        BOOST_CHECK(late_without_journal
                        .MaybeSign(Finality(),
                                   m_node.chainman->ActiveChain(), params,
                                   late_pool)
                        .empty());
        BOOST_CHECK(late_without_journal.LastError().find(
                        "possibly deleted anti-equivocation record") !=
                    std::string::npos);
    }

    node::FinalitySigner newcomer;
    error.clear();
    const fs::path newcomer_dir{store_dir / "newcomer"};
    BOOST_REQUIRE_MESSAGE(newcomer.SetKeyPersistent(
                              m_bls_c, m_vk_c, m_domain, newcomer_dir,
                              error),
                          error);
    {
        LOCK(cs_main);
        FinalitySignaturePool newcomer_pool;
        BOOST_CHECK(newcomer.MaybeSign(
                        Finality(), m_node.chainman->ActiveChain(), params,
                        newcomer_pool)
                        .empty());
        BOOST_CHECK(!newcomer.LastError().empty());
    }

    FinalitySignaturePool old_branch_pool;
    {
        LOCK(cs_main);
        const auto signed_old{original.MaybeSign(
            Finality(), m_node.chainman->ActiveChain(), params,
            old_branch_pool)};
        BOOST_REQUIRE_EQUAL(signed_old.size(), 2U);
        BOOST_CHECK_EQUAL(original.LastSignedHeight(), M + 5);
    }
    const uint256 old_lock_hash{ChainHashAt(M + 5)};

    // Process restart: the last height, exact digest and ancestry lock reload
    // before any new signature is possible.
    node::FinalitySigner restarted;
    error.clear();
    BOOST_REQUIRE_MESSAGE(restarted.SetKeyPersistent(
                              m_bls_a, m_vk_a, m_domain, store_dir, error),
                          error);
    BOOST_CHECK_EQUAL(restarted.LastSignedHeight(), M + 5);

    // Replace M+5 and extend far enough that M+10 would be signable. There is
    // no included certificate on this branch yet, so a mere higher checkpoint
    // cannot unlock the vote for old M+5. The signer halts safely.
    {
        const CBlockIndex* parent{IndexAt(M + 4)};
        uint256 seed{SeedFor(parent)};
        for (int height{M + 5}; height <= M + 13; ++height) {
            auto [block, digest]{BuildPosBlockOnSeed(
                parent, seed, m_vk_a, {}, {}, /*extra=*/20'000 + height)};
            if (block.GetBlockTime() > GetTime()) {
                SetMockTime(block.GetBlockTime());
            }
            BOOST_REQUIRE(Submit(block));
            parent = WITH_LOCK(
                cs_main,
                return m_node.chainman->m_blockman.LookupBlockIndex(
                    block.GetHash()));
            BOOST_REQUIRE(parent != nullptr);
            seed = digest;
        }
    }
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, M + 13);
    BOOST_CHECK(ChainHashAt(M + 5) != old_lock_hash);
    FinalitySignaturePool new_branch_pool;
    {
        LOCK(cs_main);
        BOOST_CHECK(restarted.MaybeSign(
                        Finality(), m_node.chainman->ActiveChain(), params,
                        new_branch_pool)
                        .empty());
        BOOST_CHECK(!restarted.LastError().empty());
        BOOST_CHECK_EQUAL(new_branch_pool.SignatureCount(0, M + 10), 0U);
    }

    // A certificate validly signed by the quorum and INCLUDED on B3 is the
    // only current-protocol lock-change proof. Once M+10 on the new branch is
    // certified, the signer may atomically rebase and later sign descendants.
    const auto set0{*FinalityState().current};
    const uint256 next_hash{FinalityState().next->SetHash()};
    Produce(m_vk_a,
            {MakeCertificate({M + 10, 0, next_hash}, set0)});
    BOOST_REQUIRE(FinalityState().finalized.has_value());
    BOOST_CHECK_EQUAL(FinalityState().finalized->height, M + 10);
    ProduceTo(M + 18, m_vk_a);
    {
        LOCK(cs_main);
        const auto signed_new{restarted.MaybeSign(
            Finality(), m_node.chainman->ActiveChain(), params,
            new_branch_pool)};
        BOOST_REQUIRE_EQUAL(signed_new.size(), 1U);
        BOOST_CHECK_EQUAL(signed_new.front().height,
                          static_cast<uint64_t>(M + 15));
        BOOST_CHECK(restarted.LastError().empty());
    }

    // A genuinely new validator may be armed after M once an included
    // certificate pins this branch, provided it has no still-accepted,
    // signable checkpoint in current/current-1. Otherwise validator-set
    // growth would become impossible after the first global checkpoint.
    {
        LOCK(cs_main);
        FinalitySignaturePool newcomer_pool;
        BOOST_CHECK(newcomer.MaybeSign(
                        Finality(), m_node.chainman->ActiveChain(), params,
                        newcomer_pool)
                        .empty());
        BOOST_CHECK(newcomer.LastError().empty());
    }
    node::FinalitySignerStore newcomer_store;
    error.clear();
    BOOST_REQUIRE_MESSAGE(newcomer_store.Open(
                              newcomer_dir, m_domain, m_vk_c, error),
                          error);
    BOOST_REQUIRE(newcomer_store.State().has_value());
    BOOST_CHECK_EQUAL(newcomer_store.State()->last_signed_height, -1);
    BOOST_CHECK_EQUAL(newcomer_store.State()->lock_height, -1);

    node::FinalitySignerStore store;
    error.clear();
    BOOST_REQUIRE_MESSAGE(store.Open(store_dir, m_domain, m_vk_a, error),
                          error);
    BOOST_REQUIRE(store.State().has_value());
    BOOST_CHECK_EQUAL(store.State()->last_signed_height, M + 15);
    BOOST_CHECK_EQUAL(store.State()->lock_height, M + 15);
    BOOST_CHECK(store.State()->lock_block_hash == ChainHashAt(M + 15));
}

BOOST_FIXTURE_TEST_CASE(pool_prunes_during_more_than_eight_sequential_finalizations, FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    const Consensus::Params& params{m_node.chainman->GetConsensus()};
    FinalitySignaturePool pool;
    node::FinalitySigner signer_a;
    signer_a.SetKey(m_bls_a, m_vk_a);
    node::FinalitySigner signer_b;
    signer_b.SetKey(m_bls_b, m_vk_b);

    // Exercise more checkpoints than the pool's hard slot bound, including an
    // epoch handover at M+30. Each accepted certificate advances consensus
    // finality; the next ordinary signature submission must reclaim the old
    // slot without relying on a stale signature replay or node restart.
    for (size_t i{0}; i <= FinalitySignaturePool::MAX_TRACKED_CHECKPOINTS; ++i) {
        const int checkpoint{M + static_cast<int>(i) * SCALED_INTERVAL};
        ProduceTo(checkpoint + SCALED_DEPTH, m_vk_a);

        std::optional<std::pair<modern::FinalizedBlock,
                                modern::FinalityCertificate>> best;
        {
            LOCK(cs_main);
            const CChain& chain{m_node.chainman->ActiveChain()};
            node::FinalityTracker& tracker{Finality()};
            const auto a{signer_a.MaybeSign(tracker, chain, params, pool)};
            const auto b{signer_b.MaybeSign(tracker, chain, params, pool)};
            BOOST_REQUIRE_EQUAL(a.size(), 1U);
            BOOST_REQUIRE_EQUAL(b.size(), 1U);
            BOOST_CHECK_EQUAL(a[0].height, static_cast<uint64_t>(checkpoint));
            BOOST_CHECK_EQUAL(b[0].height, static_cast<uint64_t>(checkpoint));
            best = pool.BestCertificate(tracker, chain, params);
        }
        BOOST_REQUIRE(best.has_value());
        BOOST_CHECK_EQUAL(best->first.height,
                          static_cast<uint64_t>(checkpoint));

        const auto [payload, cell]{
            modern::BuildFinalityCertificate(best->first, best->second)};
        CMpaRecord rec;
        rec.payload_type = modern::MPA_TYPE_FINALITY_CERTIFICATE;
        rec.payload_version = modern::MPA_VERSION_V1;
        rec.payload = payload;
        Produce(m_vk_a, {{cell, rec}});
        BOOST_REQUIRE(FinalityState().finalized.has_value());
        BOOST_CHECK_EQUAL(FinalityState().finalized->height, checkpoint);
    }

    BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(), 1U);
}

BOOST_FIXTURE_TEST_CASE(pool_replaces_same_checkpoint_after_prefinality_reorg, FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    ProduceTo(M + 8, m_vk_a);
    const Consensus::Params& params{m_node.chainman->GetConsensus()};
    FinalitySignaturePool pool;
    FinalitySig old_a;
    {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        node::FinalityTracker& tracker{Finality()};
        const auto idx_a{*tracker.Current().current->IndexOf(m_vk_a)};
        const auto idx_b{*tracker.Current().current->IndexOf(m_vk_b)};
        old_a = Sig(tracker.Current(), chain, m_domain, 0, M + 5, idx_a,
                    m_bls_a);
        BOOST_CHECK(pool.Submit(old_a, tracker, chain, params) ==
                    Accept::ACCEPTED);
        BOOST_CHECK(pool.Submit(
                        Sig(tracker.Current(), chain, m_domain, 0, M + 5,
                            idx_b, m_bls_b),
                        tracker, chain, params) == Accept::ACCEPTED);
        BOOST_REQUIRE(pool.BestCertificate(tracker, chain, params).has_value());
    }

    // Build a longer branch from M+4, replacing the checkpoint at M+5 before
    // either branch has finalized. The pool coordinates are unchanged, but
    // the signed block hash and therefore the finality digest are different.
    {
        const CBlockIndex* parent{IndexAt(M + 4)};
        uint256 seed{SeedFor(parent)};
        for (int height{M + 5}; height <= M + 9; ++height) {
            auto [block, digest]{BuildPosBlockOnSeed(
                parent, seed, m_vk_a, {}, {}, /*extra=*/10'000 + height)};
            if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
            BOOST_REQUIRE(Submit(block));
            parent = WITH_LOCK(
                cs_main,
                return m_node.chainman->m_blockman.LookupBlockIndex(
                    block.GetHash()));
            BOOST_REQUIRE(parent != nullptr);
            seed = digest;
        }
    }
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, M + 9);

    {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        node::FinalityTracker& tracker{Finality()};
        const auto idx_a{*tracker.Current().current->IndexOf(m_vk_a)};
        const auto idx_b{*tracker.Current().current->IndexOf(m_vk_b)};
        // The old branch had quorum, but it must stop being eligible before a
        // replacement signature has arrived.
        BOOST_CHECK(!pool.BestCertificate(tracker, chain, params).has_value());
        BOOST_CHECK(pool.Submit(old_a, tracker, chain, params) ==
                    Accept::BAD_SIGNATURE);
        BOOST_CHECK_EQUAL(pool.SignatureCount(0, M + 5), 0U);
        const FinalitySig new_a{Sig(tracker.Current(), chain, m_domain, 0,
                                    M + 5, idx_a, m_bls_a)};
        BOOST_CHECK(old_a.signature != new_a.signature);
        BOOST_CHECK(pool.Submit(new_a, tracker, chain, params) ==
                    Accept::ACCEPTED);
        BOOST_CHECK_EQUAL(pool.SignatureCount(0, M + 5), 1U);
        BOOST_CHECK(pool.Submit(
                        Sig(tracker.Current(), chain, m_domain, 0, M + 5,
                            idx_b, m_bls_b),
                        tracker, chain, params) == Accept::ACCEPTED);
        const auto best{pool.BestCertificate(tracker, chain, params)};
        BOOST_REQUIRE(best.has_value());
        BOOST_CHECK_EQUAL(best->first.block_hash.GetHex(),
                          chain[M + 5]->GetBlockHash().GetHex());
        BOOST_CHECK(modern::VerifyFinalityCertificate(
                        m_domain, best->first, best->second,
                        tracker.Current().current->View(),
                        tracker.Current().next->SetHash()) ==
                    modern::CertificateCheck::OK);
    }
}

BOOST_FIXTURE_TEST_CASE(pool_full_old_epoch_advances_removed_signer_watermark,
                        FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};

    // Keep the regression compact while preserving the production ordering:
    // Set_0/Set_1 include A+B, Set_2 contains only B, and epoch 2 has room for
    // all eight bounded slots before its extension is exhausted.
    Consensus::ModernPosParams& pos{*MutableConsensus().modern_pos};
    pos.finality_epoch_blocks = 4;
    pos.checkpoint_interval = 1;
    pos.checkpoint_depth = 0;
    pos.max_epoch_extension = 8;
    BOOST_REQUIRE(pos.Valid());

    Produce(m_vk_a); // M: bootstrap Set_0/Set_1.
    const auto set0{*FinalityState().current};
    const auto set1{*FinalityState().next};

    // Revoke A after bootstrap. The retained Set_1 still authorizes A, while
    // the Set_2 snapshot at the first rotation contains B alone.
    const auto revoke_a{MakeBinding(m_validator_a, m_vk_a, nullptr, 1)};
    Produce(m_vk_a,
            {MakeCertificate({M, 0, set1.SetHash()}, set0)},
            {MakeTx(4, {revoke_a.cell}, {revoke_a.record})});
    ProduceTo(M + 3, m_vk_a);
    Produce(m_vk_a); // M+4: rotate into Set_1 and derive Set_2.
    BOOST_REQUIRE_EQUAL(FinalityState().epoch, 1U);
    const auto set2{*FinalityState().next};
    BOOST_REQUIRE_EQUAL(set2.Size(), 1U);
    BOOST_CHECK(set2.IndexOf(m_vk_b).has_value());
    BOOST_CHECK(!set2.IndexOf(m_vk_a).has_value());

    // Certify only the first epoch-1 checkpoint, leaving M+5..M+7 valid for
    // delayed signing after the next rotation.
    Produce(m_vk_a,
            {MakeCertificate({M + 4, 1, set2.SetHash()}, set1)});
    ProduceTo(M + 7, m_vk_a);
    Produce(m_vk_b); // M+8: Set_2 current, Set_1 previous.
    ProduceTo(M + 15, m_vk_b);
    BOOST_REQUIRE_EQUAL(FinalityState().epoch, 2U);
    BOOST_REQUIRE(FinalityState().finalized.has_value());
    BOOST_REQUIRE_EQUAL(FinalityState().finalized->height, M + 4);

    FinalitySignaturePool pool;
    {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        node::FinalityTracker& tracker{Finality()};
        const auto idx_b{*tracker.Current().current->IndexOf(m_vk_b)};
        for (int h{M + 8}; h <= M + 15; ++h) {
            BOOST_CHECK(pool.Submit(
                            Sig(tracker.Current(), chain, m_domain, 2, h,
                                idx_b, m_bls_b),
                            tracker, chain, MutableConsensus()) ==
                        Accept::ACCEPTED);
        }
        BOOST_REQUIRE_EQUAL(pool.TrackedCheckpoints(),
                            FinalitySignaturePool::MAX_TRACKED_CHECKPOINTS);

        // A can still sign the previous epoch's M+5..M+7 checkpoints, but
        // each is older than every retained slot. They must not be relayed,
        // and the local watermark must advance so the staking loop does not
        // repeat those BLS signatures forever after A left the current set.
        node::FinalitySigner signer_a;
        signer_a.SetKey(m_bls_a, m_vk_a);
        BOOST_CHECK(signer_a.MaybeSign(
                        tracker, chain, MutableConsensus(), pool).empty());
        BOOST_CHECK_EQUAL(signer_a.LastSignedHeight(), M + 7);
        BOOST_CHECK(signer_a.MaybeSign(
                        tracker, chain, MutableConsensus(), pool).empty());
        BOOST_CHECK_EQUAL(signer_a.LastSignedHeight(), M + 7);
        BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(),
                          FinalitySignaturePool::MAX_TRACKED_CHECKPOINTS);
    }
}

BOOST_FIXTURE_TEST_CASE(aggregated_certificate_is_accepted_by_consensus, FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    ProduceTo(M + 9, m_vk_a);
    const Consensus::Params& params{m_node.chainman->GetConsensus()};
    // Both validators sign checkpoint M+5 through their signers; any node can
    // then aggregate and include the certificate; consensus accepts it.
    std::optional<std::pair<modern::FinalizedBlock, modern::FinalityCertificate>> best;
    {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        node::FinalityTracker& tracker{Finality()};
        FinalitySignaturePool pool;
        node::FinalitySigner sa;
        sa.SetKey(m_bls_a, m_vk_a);
        node::FinalitySigner sb;
        sb.SetKey(m_bls_b, m_vk_b);
        BOOST_CHECK(!sa.MaybeSign(tracker, chain, params, pool).empty());
        BOOST_CHECK(!sb.MaybeSign(tracker, chain, params, pool).empty());
        best = pool.BestCertificate(tracker, chain, params);
    }
    BOOST_REQUIRE(best.has_value());
    BOOST_CHECK_EQUAL(best->first.height, static_cast<uint64_t>(M + 5));
    const auto [payload, cell] = modern::BuildFinalityCertificate(best->first, best->second);
    CMpaRecord rec;
    rec.payload_type = modern::MPA_TYPE_FINALITY_CERTIFICATE;
    rec.payload_version = modern::MPA_VERSION_V1;
    rec.payload = payload;
    Produce(m_vk_a, {{cell, rec}});
    BOOST_CHECK_EQUAL(FinalityState().finalized->height, M + 5);
    BOOST_CHECK(FinalityState().handover_certified);
}

BOOST_AUTO_TEST_SUITE_END()
