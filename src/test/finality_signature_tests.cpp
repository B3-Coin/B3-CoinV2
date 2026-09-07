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
#include <interfaces/chain.h>
#include <modern/finality_certificate.h>
#include <modern/finality_schedule.h>
#include <node/finality_signature.h>
#include <node/finality_tracker.h>
#include <streams.h>
#include <test/util/finality_fixture.h>
#include <util/strencodings.h>
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

BOOST_FIXTURE_TEST_CASE(pool_verification_budget_follows_exact_dedup_and_cheap_checks, FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    ProduceTo(M + 48, m_vk_a);
    const auto& params{m_node.chainman->GetConsensus()};
    LOCK(cs_main);
    const auto& chain{m_node.chainman->ActiveChain()};
    auto& tracker{Finality()};
    const auto& state{tracker.Current()};
    const auto idx_a{*state.current->IndexOf(m_vk_a)};
    const auto idx_b{*state.current->IndexOf(m_vk_b)};
    const auto a{Sig(state, chain, m_domain, 0, M + 5, idx_a, m_bls_a)};
    const auto b{Sig(state, chain, m_domain, 0, M + 5, idx_b, m_bls_b)};
    FinalitySignaturePool pool;
    unsigned budget_calls{0};
    bool allow_verification{false};
    const auto budget = [&] {
        ++budget_calls;
        return allow_verification;
    };
    const auto submit = [&](const FinalitySig& sig) {
        return pool.Submit(sig, tracker, chain, params, nullptr, budget);
    };

    BOOST_CHECK_EQUAL(FinalitySignaturePool::AcceptName(Accept::VERIFICATION_DEFERRED), "verification-deferred");
    BOOST_CHECK(submit(a) == Accept::VERIFICATION_DEFERRED);
    BOOST_CHECK_EQUAL(budget_calls, 1U);
    BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(), 0U);
    allow_verification = true;
    BOOST_REQUIRE(submit(a) == Accept::ACCEPTED);
    BOOST_CHECK_EQUAL(budget_calls, 2U);

    // Only these exact verified bytes qualify for the cheap duplicate path,
    // even when no verification capacity is currently available.
    allow_verification = false;
    BOOST_CHECK(submit(a) == Accept::DUPLICATE);
    FinalitySig poisoned{a};
    poisoned.signature.fill(0);
    BOOST_CHECK(submit(poisoned) == Accept::BAD_SIGNATURE);
    poisoned.signature = b.signature;
    BOOST_CHECK(submit(poisoned) == Accept::BAD_SIGNATURE);
    BOOST_CHECK_EQUAL(budget_calls, 2U);
    BOOST_CHECK_EQUAL(pool.SignatureCount(0, M + 5), 1U);
    BOOST_CHECK(pool.RelayableSignatures(tracker, chain, params) == std::vector<FinalitySig>{a});

    // A new index must obtain a budget even within an existing checkpoint.
    BOOST_CHECK(submit(b) == Accept::VERIFICATION_DEFERRED);
    BOOST_CHECK_EQUAL(budget_calls, 3U);
    BOOST_CHECK_EQUAL(pool.SignatureCount(0, M + 5), 1U);
    allow_verification = true;
    poisoned = b;
    poisoned.signature.fill(0);
    BOOST_CHECK(submit(poisoned) == Accept::BAD_SIGNATURE);
    BOOST_CHECK_EQUAL(budget_calls, 4U);
    BOOST_CHECK_EQUAL(pool.SignatureCount(0, M + 5), 1U);
    BOOST_REQUIRE(submit(b) == Accept::ACCEPTED);
    BOOST_CHECK_EQUAL(budget_calls, 5U);
    BOOST_CHECK(pool.BestCertificate(tracker, chain, params).has_value());

    // Malformed coordinates and shallow checkpoints are rejected before the
    // BLS budget, without relying on their signature bytes being valid.
    auto bad{a};
    bad.index = static_cast<uint32_t>(state.current->Size());
    BOOST_CHECK(submit(bad) == Accept::BAD_INDEX);
    bad = a;
    bad.epoch = 99;
    BOOST_CHECK(submit(bad) == Accept::UNKNOWN_EPOCH);
    bad = a;
    bad.height = M + 6;
    BOOST_CHECK(submit(bad) == Accept::NOT_CHECKPOINT);
    bad.height = M + 55;
    BOOST_CHECK(submit(bad) == Accept::NOT_CHECKPOINT);
    auto shallow_params{params};
    shallow_params.modern_pos->checkpoint_depth = 50;
    BOOST_CHECK(pool.Submit(a, tracker, chain, shallow_params, nullptr, budget) == Accept::TOO_SHALLOW);
    const Consensus::Params unconfigured{};
    BOOST_CHECK(pool.Submit(a, tracker, chain, unconfigured, nullptr, budget) == Accept::STALE);
    BOOST_CHECK_EQUAL(budget_calls, 5U);

    // Fill all eight slots using the unchanged local/default submission path.
    for (int i{2}; i <= 8; ++i) {
        BOOST_REQUIRE(pool.Submit(Sig(state, chain, m_domain, 0, M + 5 * i, idx_a, m_bls_a),
                                  tracker, chain, params) == Accept::ACCEPTED);
    }
    BOOST_REQUIRE_EQUAL(pool.TrackedCheckpoints(), FinalitySignaturePool::MAX_TRACKED_CHECKPOINTS);
    BOOST_CHECK(submit(Sig(state, chain, m_domain, 0, M, idx_a, m_bls_a)) == Accept::POOL_FULL);
    BOOST_CHECK_EQUAL(budget_calls, 5U);
    const auto retained{pool.RelayableSignatures(tracker, chain, params)};
    const auto newest{Sig(state, chain, m_domain, 0, M + 45, idx_a, m_bls_a)};
    allow_verification = false;
    BOOST_CHECK(submit(newest) == Accept::VERIFICATION_DEFERRED);
    BOOST_CHECK_EQUAL(budget_calls, 6U);
    BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(), FinalitySignaturePool::MAX_TRACKED_CHECKPOINTS);
    BOOST_CHECK(pool.RelayableSignatures(tracker, chain, params) == retained);
    BOOST_CHECK_EQUAL(pool.SignatureCount(0, M + 45), 0U);
    allow_verification = true;
    poisoned = newest;
    poisoned.signature.fill(0);
    BOOST_CHECK(submit(poisoned) == Accept::BAD_SIGNATURE);
    BOOST_CHECK_EQUAL(budget_calls, 7U);
    BOOST_CHECK(pool.RelayableSignatures(tracker, chain, params) == retained);
    BOOST_REQUIRE(submit(newest) == Accept::ACCEPTED);
    BOOST_CHECK_EQUAL(budget_calls, 8U);
    BOOST_CHECK_EQUAL(pool.SignatureCount(0, M + 5), 0U);
    BOOST_CHECK_EQUAL(pool.SignatureCount(0, M + 45), 1U);
    BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(), FinalitySignaturePool::MAX_TRACKED_CHECKPOINTS);
}

BOOST_FIXTURE_TEST_CASE(verified_observations_and_exact_replay_exclude_shallow_and_finalized, FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    ProduceTo(M + 6, m_vk_a);
    const auto& params{m_node.chainman->GetConsensus()};
    FinalitySignaturePool pool;
    FinalitySig early;
    {
        LOCK(cs_main);
        const auto& chain{m_node.chainman->ActiveChain()};
        auto& tracker{Finality()};
        const auto idx_a{*tracker.Current().current->IndexOf(m_vk_a)};
        early = Sig(tracker.Current(), chain, m_domain, 0, M + 5, idx_a, m_bls_a);
        BOOST_CHECK(pool.Submit(early, tracker, chain, params) == Accept::TOO_SHALLOW);
        BOOST_CHECK(pool.VerifiedCheckpoints(tracker, chain, params).empty());
        BOOST_CHECK(pool.RelayableSignatures(tracker, chain, params).empty());
    }
    ProduceTo(M + 8, m_vk_a);
    {
        LOCK(cs_main);
        const auto& chain{m_node.chainman->ActiveChain()};
        auto& tracker{Finality()};
        const auto& state{tracker.Current()};
        const auto idx_a{*state.current->IndexOf(m_vk_a)};
        const auto idx_b{*state.current->IndexOf(m_vk_b)};
        const auto other{Sig(state, chain, m_domain, 0, M + 5, idx_b, m_bls_b)};
        const auto older{Sig(state, chain, m_domain, 0, M, idx_a, m_bls_a)};
        // The identical early bytes become valid at depth; no new signing is required.
        BOOST_CHECK(pool.Submit(early, tracker, chain, params) == Accept::ACCEPTED);
        BOOST_CHECK(pool.Submit(other, tracker, chain, params) == Accept::ACCEPTED);
        BOOST_CHECK(pool.Submit(older, tracker, chain, params) == Accept::ACCEPTED);
        const auto observed{pool.VerifiedCheckpoints(tracker, chain, params)};
        BOOST_REQUIRE_EQUAL(observed.size(), 2U);
        BOOST_CHECK_EQUAL(observed[0].checkpoint.height, static_cast<uint64_t>(M + 5));
        BOOST_CHECK(observed[0].checkpoint.block_hash == chain[M + 5]->GetBlockHash());
        BOOST_CHECK(observed[0].signing_set_hash == state.current->SetHash());
        BOOST_CHECK_EQUAL(observed[0].validator_count, 2U);
        BOOST_CHECK_EQUAL(observed[0].quorum_count, 2U);
        BOOST_CHECK_EQUAL(observed[0].total_weight, 16U);
        BOOST_CHECK_EQUAL(observed[0].quorum_weight, 11U);
        BOOST_CHECK_EQUAL(observed[0].signed_weight, 16U);
        BOOST_CHECK_EQUAL(observed[0].signer_indices.size(), 2U);
        BOOST_CHECK_EQUAL(observed[1].checkpoint.height, static_cast<uint64_t>(M));
        BOOST_CHECK_EQUAL(observed[1].signed_weight, 15U);
        BOOST_CHECK_EQUAL(observed[1].signer_indices.size(), 1U);
        const auto replay{pool.RelayableSignatures(tracker, chain, params)};
        BOOST_REQUIRE_EQUAL(replay.size(), 3U);
        BOOST_CHECK(replay[0] == (idx_a < idx_b ? early : other));
        BOOST_CHECK(replay[1] == (idx_a < idx_b ? other : early));
        BOOST_CHECK(replay[2] == older);
        FinalitySignaturePool receiver;
        for (const auto& sig : replay) {
            BOOST_CHECK(receiver.Submit(sig, tracker, chain, params) == Accept::ACCEPTED);
        }
        BOOST_CHECK(receiver.BestCertificate(tracker, chain, params).has_value());
        BOOST_CHECK(pool.RelayableSignatures(tracker, chain, params) == replay);
        // Roll back the observed tip without changing the checkpoint hash:
        // formerly accepted M+5 is now shallow and must not be exported.
        CChain shorter;
        shorter.SetTip(*chain[M + 6]);
        node::FinalityTracker earlier;
        BOOST_REQUIRE(earlier.Sync(shorter, m_node.chainman->m_blockman, params, *shorter.Tip()));
        const auto shallow_observation{pool.VerifiedCheckpoints(earlier, shorter, params)};
        BOOST_REQUIRE_EQUAL(shallow_observation.size(), 1U);
        BOOST_CHECK_EQUAL(shallow_observation[0].checkpoint.height, static_cast<uint64_t>(M));
        const auto shallow_replay{pool.RelayableSignatures(earlier, shorter, params)};
        BOOST_REQUIRE_EQUAL(shallow_replay.size(), 1U);
        BOOST_CHECK(shallow_replay[0] == older);
    }
    const auto set0{*FinalityState().current};
    const auto next_hash{FinalityState().next->SetHash()};
    Produce(m_vk_a, {MakeCertificate({M + 5, 0, next_hash}, set0)});
    {
        LOCK(cs_main);
        const auto& chain{m_node.chainman->ActiveChain()};
        auto& tracker{Finality()};
        // Read-only exports suppress obsolete slots without relying on a new submission.
        BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(), 2U);
        BOOST_CHECK(pool.VerifiedCheckpoints(tracker, chain, params).empty());
        BOOST_CHECK(pool.RelayableSignatures(tracker, chain, params).empty());
    }
}

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
        BOOST_CHECK(rotated.LastError().find("missing finality private key for current epoch") != std::string::npos);
        // A non-member signs nothing.
        node::FinalitySigner outsider;
        outsider.SetKey(Bls(8), m_vk_c);
        BOOST_CHECK(outsider.MaybeSign(tracker, chain, params, pool).empty());
        BOOST_CHECK(outsider.LastError().empty());
    }
}

BOOST_FIXTURE_TEST_CASE(snapshot_keys_share_a_durable_watermark_across_rotation_and_restart, FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    auto& params{MutableConsensus()};
    auto& pos{*params.modern_pos};
    pos.finality_epoch_blocks = 4;
    pos.checkpoint_interval = 1;
    pos.checkpoint_depth = 0;
    pos.max_epoch_extension = 8;
    BOOST_REQUIRE(pos.Valid());
    const bls::SecretKey rotated{Bls(9)};
    const fs::path directory{m_path_root / "snapshot-key-journal"};
    node::FinalitySigner signer;
    std::string error;
    BOOST_REQUIRE_MESSAGE(signer.SetKeysPersistent(
                              {rotated, m_bls_a, m_bls_a}, m_vk_a,
                              m_domain, directory, error), error);
    {
        LOCK(cs_main);
        FinalitySignaturePool pool;
        BOOST_CHECK(signer.MaybeSign(Finality(), m_node.chainman->ActiveChain(), params, pool).empty());
        BOOST_CHECK(signer.LastError().empty());
    }

    Produce(m_vk_a);
    const auto set0{*FinalityState().current};
    const auto set1{*FinalityState().next};
    const auto binding{MakeBinding(m_validator_a, m_vk_a, &rotated, 1)};
    Produce(m_vk_a, {MakeCertificate({M, 0, set1.SetHash()}, set0)},
            {MakeTx(4, {binding.cell}, {binding.record})});
    ProduceTo(M + 3, m_vk_a);
    {
        LOCK(cs_main);
        FinalitySignaturePool pool;
        const auto sigs{signer.MaybeSign(Finality(), m_node.chainman->ActiveChain(), params, pool)};
        BOOST_REQUIRE_EQUAL(sigs.size(), 3U);
        BOOST_CHECK_EQUAL(signer.LastSignedHeight(), M + 3);
        BOOST_CHECK(signer.LastError().empty());
    }
    Produce(m_vk_a); // Set_1 retains the old key; Set_2 commits the rotation.
    const auto set2{*FinalityState().next};
    BOOST_REQUIRE_EQUAL(FinalityState().epoch, 1U);
    BOOST_CHECK(set2.Members()[*set2.IndexOf(m_vk_a)].bls_pubkey == rotated.GetPublicKey().Compressed());
    Produce(m_vk_a, {MakeCertificate({M + 4, 1, set2.SetHash()}, set1)});
    ProduceTo(M + 9, m_vk_a);
    BOOST_REQUIRE_EQUAL(FinalityState().epoch, 2U);
    {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        node::FinalityTracker& tracker{Finality()};
        FinalitySignaturePool pool;
        const auto sigs{signer.MaybeSign(tracker, chain, params, pool)};
        BOOST_REQUIRE_EQUAL(sigs.size(), 5U);
        FinalitySignaturePool independent;
        for (size_t i{0}; i < sigs.size(); ++i) {
            BOOST_CHECK_EQUAL(sigs[i].height, static_cast<uint64_t>(M + 5 + i));
            BOOST_CHECK_EQUAL(sigs[i].epoch, i < 3 ? 1U : 2U);
            BOOST_CHECK(independent.Submit(sigs[i], tracker, chain, params) == Accept::ACCEPTED);
        }
        BOOST_CHECK_EQUAL(signer.LastSignedHeight(), M + 9);
        BOOST_CHECK(signer.LastError().empty());
    }

    node::FinalitySigner restarted;
    BOOST_REQUIRE_MESSAGE(restarted.SetKeysPersistent(
                              {m_bls_a, rotated}, m_vk_a,
                              m_domain, directory, error), error);
    BOOST_CHECK_EQUAL(restarted.LastSignedHeight(), M + 9);
    // Invalid replacements cannot discard an existing journal or anti-repeat state.
    BOOST_CHECK(!restarted.SetKeys({}, m_vk_a, error));
    BOOST_CHECK(!restarted.SetKeysPersistent(
        std::vector<bls::SecretKey>(node::FinalitySigner::MAX_KEYS + 1, rotated),
        m_vk_a, m_domain, directory, error));
    BOOST_CHECK(restarted.HasKey());
    BOOST_CHECK_EQUAL(restarted.LastSignedHeight(), M + 9);
    {
        LOCK(cs_main);
        FinalitySignaturePool pool;
        BOOST_CHECK(restarted.MaybeSign(Finality(), m_node.chainman->ActiveChain(), params, pool).empty());
        BOOST_CHECK(restarted.LastError().empty());
    }
    Produce(m_vk_a);
    {
        LOCK(cs_main);
        FinalitySignaturePool pool;
        const auto sigs{restarted.MaybeSign(Finality(), m_node.chainman->ActiveChain(), params, pool)};
        BOOST_REQUIRE_EQUAL(sigs.size(), 1U);
        BOOST_CHECK_EQUAL(sigs[0].height, static_cast<uint64_t>(M + 10));
        BOOST_CHECK_EQUAL(sigs[0].epoch, 2U);
    }
}

BOOST_FIXTURE_TEST_CASE(snapshot_key_errors_recover_on_handover_without_blocking_other_usable_keys, FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    auto& params{MutableConsensus()};
    auto& pos{*params.modern_pos};
    pos.finality_epoch_blocks = 4;
    pos.checkpoint_interval = 1;
    pos.checkpoint_depth = 0;
    pos.max_epoch_extension = 8;
    BOOST_REQUIRE(pos.Valid());
    const bls::SecretKey rotated{Bls(9)};
    node::FinalitySigner future_only;
    future_only.SetKey(rotated, m_vk_a);
    node::FinalitySigner old_only;
    old_only.SetKey(m_bls_a, m_vk_a);

    Produce(m_vk_a);
    const auto set0{*FinalityState().current};
    const auto set1{*FinalityState().next};
    const auto binding{MakeBinding(m_validator_a, m_vk_a, &rotated, 1)};
    Produce(m_vk_a, {MakeCertificate({M, 0, set1.SetHash()}, set0)},
            {MakeTx(4, {binding.cell}, {binding.record})});
    ProduceTo(M + 4, m_vk_a);
    BOOST_REQUIRE_EQUAL(FinalityState().epoch, 1U);
    const auto set2{*FinalityState().next};
    {
        LOCK(cs_main);
        FinalitySignaturePool pool;
        BOOST_CHECK(future_only.MaybeSign(Finality(), m_node.chainman->ActiveChain(), params, pool).empty());
        BOOST_CHECK(future_only.LastError().find(HexStr(m_bls_a.GetPublicKey().Compressed())) != std::string::npos);
        BOOST_CHECK_EQUAL(future_only.LastSignedHeight(), -1);
        // A missing future key does not disable the available current key.
        BOOST_CHECK(!old_only.MaybeSign(Finality(), m_node.chainman->ActiveChain(), params, pool).empty());
        BOOST_CHECK(old_only.LastError().empty());
    }
    Produce(m_vk_a, {MakeCertificate({M + 4, 1, set2.SetHash()}, set1)});
    ProduceTo(M + 9, m_vk_a);
    BOOST_REQUIRE_EQUAL(FinalityState().epoch, 2U);
    {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        node::FinalityTracker& tracker{Finality()};
        FinalitySignaturePool old_pool;
        const auto previous{old_only.MaybeSign(tracker, chain, params, old_pool)};
        BOOST_REQUIRE_EQUAL(previous.size(), 3U);
        for (const auto& sig : previous) BOOST_CHECK_EQUAL(sig.epoch, 1U);
        // Missing current key is actionable, but did not suppress previous signatures.
        BOOST_CHECK(old_only.LastError().find(HexStr(rotated.GetPublicKey().Compressed())) != std::string::npos);

        FinalitySignaturePool current_pool;
        const auto current{future_only.MaybeSign(tracker, chain, params, current_pool)};
        BOOST_REQUIRE_EQUAL(current.size(), 2U);
        for (const auto& sig : current) BOOST_CHECK_EQUAL(sig.epoch, 2U);
        // The key became current without replacing keys or resetting any watermark.
        BOOST_CHECK(future_only.LastError().empty());
        BOOST_CHECK_EQUAL(future_only.LastSignedHeight(), M + 9);
        BOOST_CHECK(future_only.MaybeSign(tracker, chain, params, current_pool).empty());
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
        const auto observed{original.RecoveryStatus(
            Finality(), m_node.chainman->ActiveChain(), params)};
        BOOST_CHECK_EQUAL(observed.state, "no_ancestry_lock");
        BOOST_CHECK(observed.journal_present);
        BOOST_CHECK(!observed.lock_height);
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
        const auto observed{late_without_journal.RecoveryStatus(
            Finality(), m_node.chainman->ActiveChain(), params)};
        BOOST_CHECK_EQUAL(observed.state, "journal_missing");
        BOOST_CHECK(observed.journal_open);
        BOOST_CHECK(!observed.journal_present);
        BOOST_CHECK(!observed.blocked_on_orphan_vote);
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
        const auto observed{restarted.RecoveryStatus(
            Finality(), m_node.chainman->ActiveChain(), params)};
        BOOST_CHECK_EQUAL(observed.state, "blocked_on_orphan_vote");
        BOOST_CHECK(observed.blocked_on_orphan_vote);
        BOOST_REQUIRE(observed.lock_height);
        BOOST_CHECK_EQUAL(*observed.lock_height, M + 5);
        BOOST_CHECK_EQUAL(observed.last_signed_height, M + 5);
        BOOST_CHECK(observed.last_signed_hash == old_lock_hash);
        BOOST_CHECK(observed.last_signed_digest == observed.lock_digest);
        BOOST_CHECK(observed.lock_hash == old_lock_hash);
        BOOST_REQUIRE(observed.current_chain_hash);
        BOOST_CHECK(*observed.current_chain_hash == m_node.chainman->ActiveChain()[M + 5]->GetBlockHash());
        BOOST_CHECK_EQUAL(observed.lock_epoch, 0U);
        BOOST_CHECK(observed.lock_signing_set_hash == Finality().Current().current->SetHash());
        BOOST_CHECK_EQUAL(observed.observed_tip_height, M + 13);
        BOOST_CHECK(!observed.certificate_strictly_newer);

        // An observation must never move the last vote or its orphan lock.
        BOOST_CHECK_EQUAL(restarted.LastSignedHeight(), M + 5);
        BOOST_CHECK(restarted.RecoveryStatus(Finality(), m_node.chainman->ActiveChain(), params).lock_hash == old_lock_hash);
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
        const auto before{restarted.RecoveryStatus(
            Finality(), m_node.chainman->ActiveChain(), params)};
        BOOST_CHECK(before.blocked_on_orphan_vote);
        BOOST_CHECK(before.certificate_included);
        BOOST_CHECK(before.certificate_strictly_newer);
        BOOST_CHECK(before.certificate_same_epoch);
        BOOST_CHECK(before.certificate_same_set);
        BOOST_CHECK(before.certificate_reconstructible);
        BOOST_CHECK(before.lock_hash == old_lock_hash);
        BOOST_CHECK_EQUAL(restarted.LastSignedHeight(), M + 5);
        const auto signed_new{restarted.MaybeSign(
            Finality(), m_node.chainman->ActiveChain(), params,
            new_branch_pool)};
        BOOST_REQUIRE_EQUAL(signed_new.size(), 1U);
        BOOST_CHECK_EQUAL(signed_new.front().height,
                          static_cast<uint64_t>(M + 15));
        BOOST_CHECK(restarted.LastError().empty());
        const auto after{restarted.RecoveryStatus(
            Finality(), m_node.chainman->ActiveChain(), params)};
        BOOST_CHECK_EQUAL(after.state, "lock_matches_chain");
        BOOST_CHECK(!after.blocked_on_orphan_vote);
        BOOST_REQUIRE(after.lock_height);
        BOOST_CHECK_EQUAL(*after.lock_height, M + 15);
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
        BOOST_CHECK(pool.VerifiedCheckpoints(tracker, chain, params).empty());
        BOOST_CHECK(pool.RelayableSignatures(tracker, chain, params).empty());
        unsigned budget_calls{0};
        // Bytes verified on the abandoned branch do not become a cheap
        // duplicate on this branch: the current digest must match first.
        BOOST_CHECK(pool.Submit(old_a, tracker, chain, params, nullptr, [&] {
                        ++budget_calls;
                        return false;
                    }) == Accept::VERIFICATION_DEFERRED);
        BOOST_CHECK_EQUAL(budget_calls, 1U);
        BOOST_CHECK(pool.Submit(old_a, tracker, chain, params, nullptr, [&] {
                        ++budget_calls;
                        return true;
                    }) == Accept::BAD_SIGNATURE);
        BOOST_CHECK_EQUAL(budget_calls, 2U);
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
