// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 14 of the Modern PoS V1 finality plan: one stake universe. From
// F = M a validator must hold an active, non-revoked FINALITY_KEY binding to
// be block-eligible; block-production weights (w, W) and the finality weights
// are the SAME snapshot numbers (W_block == W_finality); revoked / unbound
// validators leave (or never enter) at the snapshot boundary; a mid-epoch
// binding change never mutates the snapshot in force; with no set at all
// (bootstrap floor) nobody is eligible and the chain halts at M.

#include <chain.h>
#include <node/finality_tracker.h>
#include <node/stake_tracker.h>
#include <node/validator_set.h>
#include <test/util/finality_fixture.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

using b3test::FinalityChainFixture;

BOOST_AUTO_TEST_SUITE(finality_eligibility_tests)

BOOST_FIXTURE_TEST_CASE(binding_required_and_one_weight_universe, FinalityChainFixture)
{
    // A (15, bound), B (1, bound), C (10, staked but NOT bound) in the corridor.
    PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/200, /*with_unbound_c=*/true);
    const int M{m_M};
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::Params& consensus{chainman.GetConsensus()};

    // Independent derivation of Set_0 from the node's public trackers at M-1:
    // the epoch state must be built from exactly these numbers.
    const auto expected_set0{[&] {
        LOCK(cs_main);
        Chainstate& cs{chainman.ActiveChainstate()};
        BOOST_REQUIRE(cs.ModernStakeTracker().Sync(cs.m_chain, cs.m_blockman, consensus, *cs.m_chain.Tip()));
        BOOST_REQUIRE(cs.ModernFinalityBindings().Sync(cs.m_chain, cs.m_blockman, consensus, *cs.m_chain.Tip()));
        return *node::ValidatorSetSnapshot::BuildAt(0, cs.ModernStakeTracker(), M - 1, cs.ModernFinalityBindings().Index());
    }()};
    BOOST_CHECK_EQUAL(expected_set0.Size(), 2U); // C is staked but unbound: not a member
    BOOST_CHECK_EQUAL(expected_set0.TotalWeight(), 16U);

    const auto weights{[&](const modern::PosValidatorKey& key) {
        LOCK(cs_main);
        const auto w{chainman.ActiveChainstate().ModernEligibilityWeights(key, *chainman.ActiveChain().Tip())};
        BOOST_REQUIRE(w.has_value());
        return *w;
    }};
    // Before M: the weights for the block at M come from Set_0.
    BOOST_CHECK(weights(m_vk_a) == std::make_pair(CAmount{15}, CAmount{16}));
    BOOST_CHECK(weights(m_vk_b) == std::make_pair(CAmount{1}, CAmount{16}));
    BOOST_CHECK(weights(m_vk_c) == std::make_pair(CAmount{0}, CAmount{16})); // unbound: ineligible
    // C cannot produce the block at M; A can. W_block == W_finality.
    ProduceExpectConnectFailure(m_vk_c, {}, 1);
    Produce(m_vk_a);
    {
        const auto s{FinalityState()};
        BOOST_CHECK(*s.current == expected_set0);
        BOOST_CHECK_EQUAL(s.current->Header().total_weight, static_cast<uint64_t>(weights(m_vk_a).second));
        BOOST_CHECK_EQUAL(s.current->QuorumWeight(), 11U);
    }
    // B (bound, weight 1) produces in its own round.
    Produce(m_vk_b);

    // Mid-epoch: C binds (seq 0) and B revokes (seq 1). The snapshot in
    // force does not change: C stays ineligible, B stays eligible, W = 16.
    const auto bind_c{MakeBinding(m_validator_c, m_vk_c, &m_bls_c, 0)};
    Produce(m_vk_a, {}, {MakeTx(7, {bind_c.cell}, {bind_c.record})});
    const auto revoke_b{MakeBinding(m_validator_b, m_vk_b, nullptr, 1)};
    Produce(m_vk_a, {}, {MakeTx(8, {revoke_b.cell}, {revoke_b.record})});
    BOOST_CHECK(weights(m_vk_c) == std::make_pair(CAmount{0}, CAmount{16}));
    BOOST_CHECK(weights(m_vk_b) == std::make_pair(CAmount{1}, CAmount{16}));
    ProduceExpectConnectFailure(m_vk_c, {}, 2);
    Produce(m_vk_b);
    BOOST_CHECK_EQUAL(FinalityState().current->Size(), 2U);

    // Epoch 1 (Set_1 = Set_0 re-stamped): still no C, still B.
    const auto set0{*FinalityState().current};
    const auto set1{*FinalityState().next};
    ProduceTo(M + SCALED_E - 1, m_vk_a);
    Produce(m_vk_a, {MakeCertificate({M + SCALED_E - 5, 0, set1.SetHash()}, set0)}); // handover at M+E
    Produce(m_vk_a); // rotation at M+E+1
    const int start1{M + SCALED_E + 1};
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE_EQUAL(s.epoch, 1U);
        BOOST_CHECK_EQUAL(s.epoch_starts.back(), start1);
        BOOST_CHECK_EQUAL(s.current->Size(), 2U);
        // Set_2 = Snapshot(start1 - 1): C bound, B revoked => {A, C}, W = 25.
        BOOST_REQUIRE(s.next);
        BOOST_CHECK_EQUAL(s.next->Size(), 2U);
        BOOST_CHECK_EQUAL(s.next->TotalWeight(), 25U);
        BOOST_CHECK(s.next->IndexOf(m_vk_c).has_value());
        BOOST_CHECK(!s.next->IndexOf(m_vk_b).has_value());
    }
    BOOST_CHECK(weights(m_vk_c) == std::make_pair(CAmount{0}, CAmount{16}));
    BOOST_CHECK(weights(m_vk_b) == std::make_pair(CAmount{1}, CAmount{16}));
    ProduceExpectConnectFailure(m_vk_c, {}, 3);
    Produce(m_vk_b); // B still produces during epoch 1

    // Epoch 2: C in, B out; W_block == W_finality == 25.
    const auto set2{*FinalityState().next};
    int cp1{start1};
    while (!modern::IsCheckpointHeight(cp1, M, SCALED_INTERVAL)) ++cp1;
    ProduceTo(cp1 + SCALED_DEPTH - 1, m_vk_a);
    Produce(m_vk_a, {MakeCertificate({cp1, 1, set2.SetHash()}, set1)});
    ProduceTo(start1 + SCALED_E - 1, m_vk_a);
    Produce(m_vk_a); // rotation into epoch 2
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE_EQUAL(s.epoch, 2U);
        BOOST_CHECK_EQUAL(s.current->SetHash().GetHex(), set2.SetHash().GetHex());
    }
    BOOST_CHECK(weights(m_vk_a) == std::make_pair(CAmount{15}, CAmount{25}));
    BOOST_CHECK(weights(m_vk_c) == std::make_pair(CAmount{10}, CAmount{25}));
    BOOST_CHECK(weights(m_vk_b) == std::make_pair(CAmount{0}, CAmount{25})); // revoked: out at the boundary
    ProduceExpectConnectFailure(m_vk_b, {}, 4);
    Produce(m_vk_c); // C produces now
    BOOST_CHECK_EQUAL(FinalityState().current->Header().total_weight, 25U);
    BOOST_CHECK_EQUAL(FinalityState().current->QuorumWeight(), modern::QuorumWeightV1(25));
    // And an epoch-2 certificate signed by A + C (the same universe) verifies.
    const auto set3{*FinalityState().next};
    int cp2{start1 + SCALED_E};
    while (!modern::IsCheckpointHeight(cp2, M, SCALED_INTERVAL)) ++cp2;
    ProduceTo(cp2 + SCALED_DEPTH - 1, m_vk_a);
    Produce(m_vk_a, {MakeCertificate({cp2, 2, set3.SetHash(), /*a=*/true, /*b=*/false, /*c=*/true}, set2)});
    BOOST_CHECK_EQUAL(FinalityState().finalized->height, cp2);
    BOOST_CHECK_EQUAL(FinalityState().finalized->epoch, 2U);
}

BOOST_FIXTURE_TEST_CASE(bootstrap_floor_halts_production, FinalityChainFixture)
{
    // MIN_FINALITY_SET = 3 with two validators: no Set_0, nobody is eligible,
    // the chain cannot advance past M-1 (fail closed).
    PrepareFinalityChain(/*min_finality_set=*/3);
    const int M{m_M};
    BOOST_CHECK_EQUAL(Tip()->nHeight, M - 1);
    {
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        const auto w{chainstate.ModernEligibilityWeights(
            m_vk_a, *m_node.chainman->ActiveChain().Tip())};
        BOOST_REQUIRE(w.has_value());
        BOOST_CHECK(*w == std::make_pair(CAmount{0}, CAmount{0}));
        // getfinalitystatus derives ready=false from this exact projection at
        // M-1; an undersized candidate must never become Set0.
        BOOST_CHECK(!chainstate.ModernFinality().SetInForceAt(
            M, m_node.chainman->GetConsensus()));
    }
    ProduceExpectConnectFailure(m_vk_a, {}, 1);
    ProduceExpectConnectFailure(m_vk_b, {}, 2);
    BOOST_CHECK_EQUAL(Tip()->nHeight, M - 1);
    BOOST_CHECK(!FinalityState().bootstrapped);
}

BOOST_AUTO_TEST_SUITE_END()
