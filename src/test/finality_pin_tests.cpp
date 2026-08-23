// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 13 of the Modern PoS V1 finality plan: the finality pin. A
// reorganization that would disconnect the finalized checkpoint is refused
// at the header level (modern-finality-violation), in candidate selection
// and in ActivateBestChainStep; InvalidateBlock at or below the pin is
// refused; reorganizations strictly above the pin stay possible; the pin is
// sticky for the process (removing the certificate carrier does not reopen
// the checkpoint) and is re-derived identically after restart and reindex.

#include <chain.h>
#include <node/blockstorage.h>
#include <node/finality_tracker.h>
#include <test/util/finality_fixture.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

using b3test::FinalityChainDiskFixture;
using b3test::FinalityChainFixture;

namespace {

std::optional<std::pair<int, uint256>> Anchor(const node::NodeContext& node)
{
    LOCK(cs_main);
    return node.chainman->m_blockman.FinalityAnchor();
}

} // namespace

BOOST_AUTO_TEST_SUITE(finality_pin_tests)

BOOST_FIXTURE_TEST_CASE(forks_below_refused_above_allowed_pin_sticky, FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    ProduceTo(M + 7, m_vk_a);
    const auto set0{*FinalityState().current};
    const uint256 next_hash{FinalityState().next->SetHash()};
    Produce(m_vk_a, {MakeCertificate({M + 5, 0, next_hash}, set0)}); // M+8 pins M+5
    ProduceTo(M + 13, m_vk_a);
    const uint256 pin_hash{ChainHashAt(M + 5)};
    {
        const auto anchor{Anchor(m_node)};
        BOOST_REQUIRE(anchor.has_value());
        BOOST_CHECK_EQUAL(anchor->first, M + 5);
        BOOST_CHECK_EQUAL(anchor->second.GetHex(), pin_hash.GetHex());
    }

    // 1. A block forking BELOW the pin (parent M+4, M+3, M+1) is refused at
    //    the header level: never enters the index, tip unmoved.
    for (const int parent_height : {M + 4, M + 3, M + 1}) {
        const CBlockIndex* parent{IndexAt(parent_height)};
        const auto [side, digest] = BuildPosBlockOnSeed(parent, SeedFor(parent), m_vk_a, {}, {}, /*extra=*/parent_height);
        BOOST_CHECK(!Submit(side));
        BOOST_CHECK(WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(side.GetHash())) == nullptr);
        BOOST_CHECK_EQUAL(Tip()->nHeight, M + 13);
    }

    // 2. A branch forking AT the pin (parent = the pinned checkpoint) is an
    //    ordinary reorg: it disconnects M+6..M+13 but not the checkpoint.
    //    Build M+6..M+14 on the side (one more than the active chain).
    std::vector<uint256> side_hashes;
    {
        const CBlockIndex* parent{IndexAt(M + 5)};
        uint256 seed{SeedFor(parent)};
        for (int h{M + 6}; h <= M + 14; ++h) {
            auto [side, digest] = BuildPosBlockOnSeed(parent, seed, m_vk_a, {}, {}, /*extra=*/100 + h);
            if (side.GetBlockTime() > GetTime()) SetMockTime(side.GetBlockTime());
            BOOST_REQUIRE(Submit(side));
            side_hashes.push_back(side.GetHash());
            parent = WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(side.GetHash()));
            BOOST_REQUIRE(parent != nullptr);
            seed = digest;
        }
    }
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, M + 14);
    BOOST_CHECK_EQUAL(Tip()->GetBlockHash().GetHex(), side_hashes.back().GetHex());
    BOOST_CHECK_EQUAL(ChainHashAt(M + 5).GetHex(), pin_hash.GetHex()); // checkpoint untouched

    // 3. The certificate carrier (old M+8) is no longer on the active chain:
    //    the derived finalized state is gone, but the pin is STICKY.
    BOOST_CHECK(!FinalityState().finalized.has_value());
    {
        const auto anchor{Anchor(m_node)};
        BOOST_REQUIRE(anchor.has_value());
        BOOST_CHECK_EQUAL(anchor->first, M + 5);
    }
    // 4. ... so a fork below the pin is STILL refused (the two-step bypass is closed).
    {
        const CBlockIndex* parent{IndexAt(M + 4)};
        const auto [side, digest] = BuildPosBlockOnSeed(parent, SeedFor(parent), m_vk_a, {}, {}, /*extra=*/777);
        BOOST_CHECK(!Submit(side));
        BOOST_CHECK_EQUAL(Tip()->nHeight, M + 14);
    }
    // 5. A new certificate on the new branch (its own M+10, epoch 0) raises the pin.
    Produce(m_vk_a, {MakeCertificate({M + 10, 0, next_hash}, set0)}); // M+15
    BOOST_CHECK_EQUAL(FinalityState().finalized->height, M + 10);
    BOOST_CHECK_EQUAL(Anchor(m_node)->first, M + 10);
    BOOST_CHECK_EQUAL(Anchor(m_node)->second.GetHex(), ChainHashAt(M + 10).GetHex());
    // A fork at M+9 (below the new pin) is refused; at M+10 it would be allowed.
    {
        const CBlockIndex* parent{IndexAt(M + 9)};
        const auto [side, digest] = BuildPosBlockOnSeed(parent, SeedFor(parent), m_vk_a, {}, {}, /*extra=*/778);
        BOOST_CHECK(!Submit(side));
    }
}

BOOST_FIXTURE_TEST_CASE(invalidate_reconsider_and_candidate_paths_cannot_bypass, FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    ProduceTo(M + 7, m_vk_a);
    // A side branch forking at M+2 (before any pin exists), M+3..M+6, stored
    // but inactive (less work than the active M+7).
    std::vector<const CBlockIndex*> side;
    {
        const CBlockIndex* parent{IndexAt(M + 2)};
        uint256 seed{SeedFor(parent)};
        for (int h{M + 3}; h <= M + 6; ++h) {
            auto [blk, digest] = BuildPosBlockOnSeed(parent, seed, m_vk_a, {}, {}, /*extra=*/200 + h);
            BOOST_REQUIRE(Submit(blk));
            parent = WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(blk.GetHash()));
            BOOST_REQUIRE(parent != nullptr);
            side.push_back(parent);
            seed = digest;
        }
    }
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, M + 7);
    const auto set0{*FinalityState().current};
    const uint256 next_hash{FinalityState().next->SetHash()};
    Produce(m_vk_a, {MakeCertificate({M + 5, 0, next_hash}, set0)}); // M+8 pins M+5
    ProduceTo(M + 10, m_vk_a);
    BOOST_REQUIRE_EQUAL(Anchor(m_node)->first, M + 5);
    const uint256 main_tip{Tip()->GetBlockHash()};

    // InvalidateBlock at or below the pin is refused; the tip never moves.
    for (const int h : {M + 5, M + 4, M}) {
        BlockValidationState state;
        CBlockIndex* index{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain()[h])};
        BOOST_CHECK(!m_node.chainman->ActiveChainstate().InvalidateBlock(state, index));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "modern-finality-violation");
        BOOST_CHECK_EQUAL(Tip()->GetBlockHash().GetHex(), main_tip.GetHex());
    }
    // InvalidateBlock ABOVE the pin is an ordinary operation: the active
    // chain drops to M+5. The stored side branch (tip M+6, more work than
    // M+5) forks at M+2 < pin: the candidate path must NOT activate it.
    {
        BlockValidationState state;
        CBlockIndex* index{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain()[M + 6])};
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, index));
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
    }
    BOOST_CHECK_EQUAL(Tip()->nHeight, M + 5);
    BOOST_CHECK_EQUAL(Tip()->GetBlockHash().GetHex(), Anchor(m_node)->second.GetHex());
    {
        LOCK(cs_main);
        BOOST_CHECK(m_node.chainman->m_blockman.IsAnchorIneligible(*side.back()));
        BOOST_CHECK(!m_node.chainman->ActiveChain().Contains(side.back()));
    }
    // The pin survives the disconnect of its own certificate carrier (sticky).
    BOOST_CHECK(!FinalityState().finalized.has_value());
    BOOST_CHECK_EQUAL(Anchor(m_node)->first, M + 5);
    // Reconsider: the main chain reactivates to M+10.
    {
        CBlockIndex* index{WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(main_tip))};
        BOOST_REQUIRE(index != nullptr);
        CBlockIndex* invalidated{index->GetAncestor(M + 6)};
        {
            LOCK(cs_main);
            m_node.chainman->ActiveChainstate().ResetBlockFailureFlags(invalidated);
            m_node.chainman->RecalculateBestHeader();
        }
        BlockValidationState state;
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
    }
    BOOST_CHECK_EQUAL(Tip()->GetBlockHash().GetHex(), main_tip.GetHex());
    BOOST_CHECK_EQUAL(FinalityState().finalized->height, M + 5);
    // Extending the stored side branch is refused at the header level now.
    {
        const CBlockIndex* parent{side.back()};
        // The side branch was never connected: derive its seed chain from the fork point.
        const CBlockIndex* p{IndexAt(M + 2)};
        uint256 seed{SeedFor(p)};
        for (const CBlockIndex* s : side) {
            const int64_t round{modern::DecodeModernPosRound(s->pprev->GetBlockTime(), s->GetBlockTime(), *m_node.chainman->GetConsensus().modern_pos).value()};
            seed = modern::ModernPosEligibilityDigest(Domain(), seed, static_cast<uint32_t>(s->nHeight), static_cast<uint32_t>(round), m_vk_a);
        }
        const auto [blk, digest] = BuildPosBlockOnSeed(parent, seed, m_vk_a, {}, {}, /*extra=*/300);
        BOOST_CHECK(!Submit(blk));
        BOOST_CHECK(WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(blk.GetHash())) == nullptr);
    }
}

BOOST_FIXTURE_TEST_CASE(restart_and_reindex_reproduce_finalized_tip, FinalityChainDiskFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    ProduceTo(M + 7, m_vk_a);
    const auto set0{*FinalityState().current};
    const uint256 next_hash{FinalityState().next->SetHash()};
    Produce(m_vk_a, {MakeCertificate({M + 5, 0, next_hash}, set0)});
    ProduceTo(M + 10, m_vk_a);
    const auto before{FinalityState()};
    const uint256 pin_hash{ChainHashAt(M + 5)};
    const int height_before{Tip()->nHeight};

    const auto restart{[&](const bool reindex_chainstate) {
        {
            LOCK(cs_main);
            m_node.chainman->ActiveChainstate().ForceFlushStateToDisk();
        }
        m_node.chainman.reset();
        if (reindex_chainstate) m_args.ForceSetArg("-reindex-chainstate", "1");
        m_make_chainman();
        LoadVerifyActivateChainstate();
        if (reindex_chainstate) m_args.ForceSetArg("-reindex-chainstate", "0");
    }};
    const auto verify{[&] {
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, height_before);
        const auto after{FinalityState()};
        BOOST_CHECK_EQUAL(after.epoch, before.epoch);
        BOOST_CHECK(after.epoch_starts == before.epoch_starts);
        BOOST_REQUIRE(after.finalized.has_value());
        BOOST_CHECK(after.finalized == before.finalized);
        BOOST_CHECK_EQUAL(after.current->SetHash().GetHex(), before.current->SetHash().GetHex());
        BOOST_CHECK_EQUAL(after.next->SetHash().GetHex(), before.next->SetHash().GetHex());
        // The pin is re-derived from the chain and enforced again: a fork
        // below it is refused after the restart.
        {
            LOCK(cs_main);
            m_node.chainman->ActiveChainstate().RefreshFinalityAnchor();
        }
        BOOST_REQUIRE(Anchor(m_node).has_value());
        BOOST_CHECK_EQUAL(Anchor(m_node)->first, M + 5);
        BOOST_CHECK_EQUAL(Anchor(m_node)->second.GetHex(), pin_hash.GetHex());
        const CBlockIndex* parent{IndexAt(M + 4)};
        const auto [side, digest] = BuildPosBlockOnSeed(parent, SeedFor(parent), m_vk_a, {}, {}, /*extra=*/900 + height_before);
        BOOST_CHECK(!Submit(side));
        BOOST_CHECK_EQUAL(Tip()->nHeight, height_before);
    }};
    restart(/*reindex_chainstate=*/false);
    verify();
    restart(/*reindex_chainstate=*/true);
    verify();
    // And the chain keeps extending with certificates after the reindex.
    ProduceTo(M + 12, m_vk_a);
    Produce(m_vk_a, {MakeCertificate({M + 10, 0, next_hash}, set0)});
    BOOST_CHECK_EQUAL(FinalityState().finalized->height, M + 10);
    BOOST_CHECK_EQUAL(Anchor(m_node)->first, M + 10);
}

BOOST_AUTO_TEST_SUITE_END()
