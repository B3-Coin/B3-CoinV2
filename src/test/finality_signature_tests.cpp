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
    if (const auto fb{FinalitySignaturePool::ExpectedFinalizedBlock(epoch, height, state, chain)}) {
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
    // A alone (15 of 16, quorum 11) certifies; the aggregate is consensus-valid.
    {
        const auto best{pool.BestCertificate(tracker, chain, params)};
        BOOST_REQUIRE(best.has_value());
        BOOST_CHECK_EQUAL(best->first.height, static_cast<uint64_t>(M + 5));
        BOOST_CHECK(modern::VerifyFinalityCertificate(m_domain, best->first, best->second, state.current->View(),
                                                      state.next->SetHash()) == modern::CertificateCheck::OK);
        BOOST_CHECK_EQUAL(modern::SignedWeight(best->second.signer_bitmap, state.current->View()), 15U);
    }
    // B joins: two signers aggregated, bitmap carries both (accountability).
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
    // Slot bound: 9 signable checkpoints, only MAX_TRACKED_CHECKPOINTS stick.
    ProduceTo(M + 43, m_vk_a); // checkpoints M .. M+40 all >= 3 deep
    {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        node::FinalityTracker& tracker{Finality()};
        const auto idx_a{*tracker.Current().current->IndexOf(m_vk_a)};
        const auto& state{tracker.Current()};
        for (int i{0}; i <= 8; ++i) {
            const auto expected{i < static_cast<int>(FinalitySignaturePool::MAX_TRACKED_CHECKPOINTS) ? Accept::ACCEPTED : Accept::POOL_FULL};
            BOOST_CHECK(pool.Submit(Sig(state, chain, m_domain, 0, M + 5 * i, idx_a, m_bls_a), tracker, chain, params) == expected);
        }
        BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(), FinalitySignaturePool::MAX_TRACKED_CHECKPOINTS);
        // Finalizing prunes everything at or below and frees room; lower heights are STALE.
        pool.Prune(M + 20);
        BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(), 3U); // M+25, M+30, M+35
        BOOST_CHECK(pool.Submit(Sig(state, chain, m_domain, 0, M + 40, idx_a, m_bls_a), tracker, chain, params) == Accept::ACCEPTED);
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
