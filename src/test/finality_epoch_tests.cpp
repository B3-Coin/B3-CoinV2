// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 12 of the Modern PoS V1 finality plan: the derived epoch / finality
// state machine (node::FinalityTracker) wired into block validation --
// bootstrap at M, certificate acceptance on a live chain under the Commit 11
// rules, handover-gated rotation (E), extension, MAX_EPOCH_EXTENSION lineage
// break, delayed current-1 certificates, carry-over, mid-epoch key rotation,
// and deterministic rebuild.

#include <chain.h>
#include <consensus/era.h>
#include <modern/finality_schedule.h>
#include <node/finality_tracker.h>
#include <node/validator_set.h>
#include <test/util/finality_fixture.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

using b3test::FinalityChainFixture;
using node::FinalityTracker;

namespace {

bool SameState(const FinalityTracker::State& a, const FinalityTracker::State& b)
{
    auto hash_of{[](const std::shared_ptr<const node::ValidatorSetSnapshot>& s) { return s ? s->SetHash() : uint256{}; }};
    return a.bootstrapped == b.bootstrapped && a.epoch == b.epoch && a.epoch_starts == b.epoch_starts &&
           hash_of(a.bootstrap) == hash_of(b.bootstrap) &&
           hash_of(a.previous) == hash_of(b.previous) && hash_of(a.current) == hash_of(b.current) &&
           hash_of(a.next) == hash_of(b.next) && a.handover_certified == b.handover_certified &&
           a.lineage_broken == b.lineage_broken && a.finalized == b.finalized;
}

Consensus::BridgeDecentralizedWithdrawalPins BridgePins()
{
    Consensus::BridgeDecentralizedWithdrawalPins pins;
    pins.ethereum_verifier_address.fill(0x11);
    pins.ethereum_verifier_code_hash = uint256::ONE;
    pins.bootstrap_validator_set_hash = uint256{uint8_t{2}};
    pins.withdrawal_rules_version =
        Consensus::DECENTRALIZED_WITHDRAWAL_RULES_VERSION_V1;
    pins.withdrawal_rules_commitment = uint256{uint8_t{3}};
    pins.min_bridge_validators = 4;
    pins.max_bridge_validators = 4;
    pins.min_bridge_total_weight = 34;
    pins.max_epoch_lag = 1;
    return pins;
}

} // namespace

BOOST_AUTO_TEST_SUITE(finality_epoch_tests)

BOOST_FIXTURE_TEST_CASE(bootstrap_and_certificates_on_chain, FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    // Nothing is bootstrapped before M; a certificate in the corridor is invalid.
    BOOST_CHECK(!FinalityState().bootstrapped);

    Produce(m_vk_a); // height M: epoch 0 starts
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE(s.bootstrapped);
        BOOST_CHECK_EQUAL(s.epoch, 0U);
        BOOST_REQUIRE_EQUAL(s.epoch_starts.size(), 1U);
        BOOST_CHECK_EQUAL(s.epoch_starts[0], M);
        BOOST_REQUIRE(s.bootstrap && s.current && s.next && !s.previous);
        BOOST_CHECK_EQUAL(s.bootstrap->SetHash().GetHex(),
                          s.current->SetHash().GetHex());
        BOOST_CHECK_EQUAL(s.current->Size(), 2U);
        BOOST_CHECK_EQUAL(s.current->Epoch(), 0U);
        BOOST_CHECK_EQUAL(s.next->Epoch(), 1U);
        BOOST_CHECK_EQUAL(s.next->SetHash().GetHex(), s.current->WithEpoch(1).SetHash().GetHex());
        BOOST_CHECK_EQUAL(s.current->TotalWeight(), 16U);
        BOOST_CHECK_EQUAL(s.current->QuorumWeight(), 11U);
        BOOST_CHECK(!s.handover_certified && !s.lineage_broken && !s.finalized);
    }
    const auto set0{*FinalityState().current};
    const uint256 next_hash{FinalityState().next->SetHash()};

    // Checkpoints every 5 from M; depth 3. Certificate for M+5 at M+7: too shallow.
    ProduceTo(M + 6, m_vk_a);
    ProduceExpectConnectFailure(m_vk_a, {MakeCertificate({M + 5, 0, next_hash}, set0)}, 1);
    Produce(m_vk_a); // M+7
    // At M+8: accepted; the finalized tip moves, the handover is certified.
    Produce(m_vk_a, {MakeCertificate({M + 5, 0, next_hash}, set0)});
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE(s.finalized.has_value());
        BOOST_CHECK_EQUAL(s.finalized->height, M + 5);
        BOOST_CHECK_EQUAL(s.finalized->epoch, 0U);
        BOOST_CHECK_EQUAL(s.finalized->certified_at, M + 8);
        BOOST_CHECK(s.handover_certified);
        BOOST_CHECK_EQUAL(s.epoch, 0U); // rotation waits for the nominal boundary
    }
    // Rejections (each a distinct block at M+9): regression (same checkpoint
    // again), off-schedule height, wrong ancestry, wrong successor hash,
    // outside the epoch window, and a light-only signer below quorum.
    ProduceExpectConnectFailure(m_vk_a, {MakeCertificate({M + 5, 0, next_hash}, set0)}, 2);
    ProduceExpectConnectFailure(m_vk_a, {MakeCertificate({M + 6, 0, next_hash}, set0)}, 3);
    ProduceExpectConnectFailure(m_vk_a, {MakeCertificate({M + 5, 0, next_hash}, set0, uint256::ONE)}, 4);
    ProduceExpectConnectFailure(m_vk_a, {MakeCertificate({M + 5, 0, set0.SetHash()}, set0)}, 5);
    ProduceExpectConnectFailure(m_vk_a, {MakeCertificate({M + 5, 1, next_hash}, set0)}, 6);
    Produce(m_vk_a); // M+9
    ProduceTo(M + 12, m_vk_a);
    ProduceExpectConnectFailure(m_vk_a, {MakeCertificate({M + 10, 0, next_hash, /*a=*/false, /*b=*/true}, set0)}, 7);
    // Weight alone is insufficient: A has 15 of 16 stake, but only one of
    // two validator identities. The independent headcount quorum requires
    // both signers. Reject the weight-only certificate, then accept 2-of-2.
    ProduceExpectConnectFailure(
        m_vk_a,
        {MakeCertificate({M + 10, 0, next_hash, /*a=*/true,
                          /*b=*/false},
                         set0)},
        7);
    Produce(m_vk_a,
            {MakeCertificate({M + 10, 0, next_hash}, set0)});
    BOOST_CHECK_EQUAL(FinalityState().finalized->height, M + 10);
    // A missed checkpoint (M+15) is simply skipped: M+20 certifies at M+23.
    ProduceTo(M + 22, m_vk_a);
    Produce(m_vk_a, {MakeCertificate({M + 20, 0, next_hash}, set0)});
    BOOST_CHECK_EQUAL(FinalityState().finalized->height, M + 20);
    // A certificate record without its cell, or a cell without its record, is malformed.
    {
        auto [cell, rec] = MakeCertificate({M + 20, 0, next_hash}, set0);
        CBlock orphan_record{BuildPosBlock(m_vk_a, {{CScript(), rec}}, {}, 8)};
        (void)orphan_record; // a null-script "cell" is not a cell: record is orphaned
        CMutableTransaction cb{*orphan_record.vtx[0]};
        cb.vout.erase(cb.vout.begin() + 1); // drop the dummy output, keep the record
        orphan_record.vtx[0] = MakeTransactionRef(cb);
        // (payload root unchanged: leaves exclude txids; re-sign and submit)
        orphan_record.hashMerkleRoot = BlockMerkleRoot(orphan_record);
        Sign(orphan_record, m_validator_a);
        SubmitExpectConnectFailure(orphan_record);
    }

    // Deterministic rebuild: discarding the derived state and re-deriving it
    // from the chain yields the identical epoch state and finalized tip.
    const auto before{FinalityState()};
    {
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().ModernFinality().MarkDirty();
    }
    const auto after{FinalityState()};
    BOOST_CHECK(SameState(before, after));
    BOOST_CHECK(before.finalized == after.finalized);
}

BOOST_FIXTURE_TEST_CASE(gated_rotation_extension_delayed_and_carry_over, FinalityChainFixture)
{
    // MIN_FINALITY_SET = 2 so that a one-member snapshot triggers carry-over.
    PrepareFinalityChain(/*min_finality_set=*/2);
    const int M{m_M};
    const int E{SCALED_E};
    bls::SecretKey bls_b2{Bls(7)};

    // Reaching the nominal boundary M+E alone does not rotate: no certificate
    // yet, so epoch 0 extends past M+E.
    ProduceTo(M + E + 3, m_vk_a);
    {
        const auto s{FinalityState()};
        BOOST_CHECK_EQUAL(s.epoch, 0U);
        BOOST_CHECK_EQUAL(s.epoch_starts.size(), 1U);
        BOOST_CHECK(!s.handover_certified && !s.lineage_broken);
        BOOST_CHECK(ProjectedNext().epoch == 0U);
    }
    const auto set0{*FinalityState().current};
    const auto set1{*FinalityState().next};
    // Handover: an epoch-0 certificate (for M+25, depth 9) at M+E+4.
    Produce(m_vk_a, {MakeCertificate({M + 25, 0, set1.SetHash()}, set0)});
    {
        const auto s{FinalityState()};
        BOOST_CHECK(s.handover_certified);
        BOOST_CHECK_EQUAL(s.epoch, 0U); // the including block itself is still epoch 0
        BOOST_CHECK_EQUAL(s.finalized->height, M + 25);
        BOOST_CHECK(ProjectedNext().epoch == 1U); // the next block rotates
    }
    // Rotation at M+E+5: Set_1 current, Set_0 previous, Set_2 = Snapshot(M+E+4).
    Produce(m_vk_a);
    const int start1{M + E + 5};
    {
        const auto s{FinalityState()};
        BOOST_CHECK_EQUAL(s.epoch, 1U);
        BOOST_REQUIRE_EQUAL(s.epoch_starts.size(), 2U);
        BOOST_CHECK_EQUAL(s.epoch_starts[1], start1);
        BOOST_REQUIRE(s.previous && s.current && s.next);
        BOOST_CHECK_EQUAL(s.previous->SetHash().GetHex(), set0.SetHash().GetHex());
        BOOST_CHECK_EQUAL(s.current->SetHash().GetHex(), set1.SetHash().GetHex());
        BOOST_CHECK_EQUAL(s.next->Epoch(), 2U);
        BOOST_CHECK_EQUAL(s.next->Size(), 2U);
        BOOST_CHECK(!s.handover_certified);
        BOOST_CHECK_EQUAL(s.finalized->height, M + 25); // finalized state survives rotation
    }
    const auto set2{*FinalityState().next};

    // Delayed current-1 certificate: Set_0 certifies M+30 (an epoch-0
    // checkpoint) after the rotation, attesting hash(Set_1); accepted,
    // finalized tip advances, the epoch-1 handover is NOT certified by it.
    Produce(m_vk_a, {MakeCertificate({M + 30, 0, set1.SetHash()}, set0)});
    {
        const auto s{FinalityState()};
        BOOST_CHECK_EQUAL(s.finalized->height, M + 30);
        BOOST_CHECK_EQUAL(s.finalized->epoch, 0U);
        BOOST_CHECK(!s.handover_certified);
        BOOST_CHECK_EQUAL(s.epoch, 1U);
    }
    // Delayed rejections: wrong successor (hash(Set_2)), regression (M+20),
    // and the epoch relation (an epoch-1 object for M+30, which lies in epoch 0).
    ProduceExpectConnectFailure(m_vk_a, {MakeCertificate({M + 30, 0, set2.SetHash()}, set0)}, 1);
    ProduceExpectConnectFailure(m_vk_a, {MakeCertificate({M + 20, 0, set1.SetHash()}, set0)}, 2);
    ProduceExpectConnectFailure(m_vk_a, {MakeCertificate({M + 30, 1, set2.SetHash()}, set1)}, 3);
    // Epoch-1 checkpoints start at its first height: start1 is a checkpoint
    // only if on the M-anchored schedule; the first epoch-1 checkpoint is the
    // first multiple of 5 from M at or above start1.
    int cp1{start1};
    while (!modern::IsCheckpointHeight(cp1, M, SCALED_INTERVAL)) ++cp1;
    ProduceTo(cp1 + SCALED_DEPTH - 1, m_vk_a);
    Produce(m_vk_a, {MakeCertificate({cp1, 1, set2.SetHash()}, set1)});
    {
        const auto s{FinalityState()};
        BOOST_CHECK(s.handover_certified);
        BOOST_CHECK_EQUAL(s.finalized->height, cp1);
        BOOST_CHECK_EQUAL(s.finalized->epoch, 1U);
    }
    // An epoch-0 certificate (current-2 after the next rotation) is still in
    // the window now (current-1) but will not be after rotation.

    // Mid-epoch key rotation by B (seq 1): the active Set_1 and the already
    // derived Set_2 keep B's OLD key; only the snapshot taken at the next
    // rotation sees the new one.
    const auto old_b_key{m_bls_b.GetPublicKey().Compressed()};
    const auto rotate_b{MakeBinding(m_validator_b, m_vk_b, &bls_b2, 1)};
    Produce(m_vk_a, {}, {MakeTx(4, {rotate_b.cell}, {rotate_b.record})});
    {
        const auto s{FinalityState()};
        for (const auto& m : s.current->Members()) if (m.validator_key == m_vk_b) BOOST_CHECK(m.bls_pubkey == old_b_key);
        for (const auto& m : s.next->Members()) if (m.validator_key == m_vk_b) BOOST_CHECK(m.bls_pubkey == old_b_key);
    }
    // Rotation into epoch 2 at the first h >= start1 + E (handover certified).
    ProduceTo(start1 + E - 1, m_vk_a);
    BOOST_CHECK_EQUAL(FinalityState().epoch, 1U);
    Produce(m_vk_a);
    const int start2{start1 + E};
    {
        const auto s{FinalityState()};
        BOOST_CHECK_EQUAL(s.epoch, 2U);
        BOOST_REQUIRE_EQUAL(s.epoch_starts.size(), 3U);
        BOOST_CHECK_EQUAL(s.epoch_starts[2], start2);
        BOOST_CHECK_EQUAL(s.previous->SetHash().GetHex(), set1.SetHash().GetHex());
        BOOST_CHECK_EQUAL(s.current->SetHash().GetHex(), set2.SetHash().GetHex());
        BOOST_CHECK_EQUAL(s.next->Epoch(), 3U);
        BOOST_REQUIRE(s.bootstrap);
        BOOST_CHECK_EQUAL(s.bootstrap->SetHash().GetHex(),
                          set0.SetHash().GetHex());
        bool saw_new{false};
        for (const auto& m : s.next->Members()) {
            if (m.validator_key == m_vk_b) saw_new = (m.bls_pubkey == bls_b2.GetPublicKey().Compressed());
        }
        BOOST_CHECK(saw_new); // Set_3 carries B's rotated key
        // Epoch 0 is now outside the window {2, 1}.
        BOOST_CHECK(!s.handover_certified);
    }
    const auto set3{*FinalityState().next};
    ProduceExpectConnectFailure(m_vk_a, {MakeCertificate({M + 30, 0, set1.SetHash()}, set0)}, 4); // epoch-window (also regression)
    // Set_2 (B's old key) signs epoch-2 certificates; B's new key must not.
    int cp2{start2};
    while (!modern::IsCheckpointHeight(cp2, M, SCALED_INTERVAL)) ++cp2;
    ProduceTo(cp2 + SCALED_DEPTH - 1, m_vk_a);
    Produce(m_vk_a, {MakeCertificate({cp2, 2, set3.SetHash()}, set2)});
    BOOST_CHECK(FinalityState().handover_certified);

    // Carry-over: B revokes (seq 2, zero key). The next snapshot has one
    // member (< MIN_FINALITY_SET = 2), so Set_4 = Set_3 re-stamped epoch 4.
    const auto revoke_b{MakeBinding(m_validator_b, m_vk_b, nullptr, 2)};
    Produce(m_vk_a, {}, {MakeTx(5, {revoke_b.cell}, {revoke_b.record})});
    ProduceTo(start2 + E - 1, m_vk_a);
    Produce(m_vk_a);
    {
        const auto s{FinalityState()};
        BOOST_CHECK_EQUAL(s.epoch, 3U);
        BOOST_CHECK_EQUAL(s.current->SetHash().GetHex(), set3.SetHash().GetHex());
        BOOST_REQUIRE(s.next);
        BOOST_CHECK_EQUAL(s.next->Size(), 2U); // carried over, B still listed
        BOOST_CHECK_EQUAL(s.next->SetHash().GetHex(), set3.WithEpoch(4).SetHash().GetHex());
        // No chain ever jumped an epoch: starts are strictly increasing, one per epoch.
        for (size_t i = 1; i < s.epoch_starts.size(); ++i) BOOST_CHECK(s.epoch_starts[i] > s.epoch_starts[i - 1]);
        BOOST_CHECK_EQUAL(s.epoch_starts.size(), s.epoch + 1);
    }
    // Rebuild equality after everything above.
    const auto before{FinalityState()};
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().ModernFinality().MarkDirty());
    BOOST_CHECK(SameState(before, FinalityState()));
}

BOOST_FIXTURE_TEST_CASE(extension_exhaustion_breaks_lineage, FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    const int last_chance{M + SCALED_E + SCALED_MAX_EXTENSION - 1};
    ProduceTo(last_chance, m_vk_a);
    {
        const auto s{FinalityState()};
        BOOST_CHECK(!s.lineage_broken);
        BOOST_CHECK_EQUAL(s.epoch, 0U);
        BOOST_CHECK(ProjectedNext().lineage_broken); // the next height exhausts the extension
    }
    const auto set0{*FinalityState().current};
    const uint256 next_hash{FinalityState().next->SetHash()};
    Produce(m_vk_a); // blocks never depend on certificates: the chain continues
    BOOST_CHECK(FinalityState().lineage_broken);
    // A perfect certificate is now invalid; blocks still connect.
    ProduceExpectConnectFailure(m_vk_a, {MakeCertificate({M + 55, 0, next_hash}, set0)}, 1);
    Produce(m_vk_a);
    BOOST_CHECK(FinalityState().lineage_broken);
    BOOST_CHECK_EQUAL(FinalityState().epoch, 0U);
}

BOOST_FIXTURE_TEST_CASE(last_chance_handover_rotates, FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    const int last_chance{M + SCALED_E + SCALED_MAX_EXTENSION - 1};
    ProduceTo(last_chance - 1, m_vk_a);
    const auto set0{*FinalityState().current};
    const uint256 next_hash{FinalityState().next->SetHash()};
    Produce(m_vk_a, {MakeCertificate({M + 55, 0, next_hash}, set0)}); // at last_chance
    BOOST_CHECK(FinalityState().handover_certified);
    BOOST_CHECK(!FinalityState().lineage_broken);
    Produce(m_vk_a); // rotation, not a break
    const auto s{FinalityState()};
    BOOST_CHECK_EQUAL(s.epoch, 1U);
    BOOST_CHECK(!s.lineage_broken);
    BOOST_CHECK_EQUAL(s.epoch_starts.back(), last_chance + 1);
}

BOOST_FIXTURE_TEST_CASE(bridge_readiness_rebuild_and_reorg_are_exact,
                        FinalityChainFixture)
{
    PrepareFinalityChain(/*min_finality_set=*/4,
                         /*reorg_horizon=*/200,
                         /*with_unbound_c=*/false,
                         /*with_bound_c=*/true,
                         /*with_bound_d=*/true);
    const int M{m_M};
    Produce(m_vk_a);
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, M);
    const uint256 modern_tip{Tip()->GetBlockHash()};
    const auto pins{BridgePins()};
    BOOST_REQUIRE(pins.Valid());
    BOOST_CHECK(node::BridgeWithdrawalValidatorSetsReady(
        FinalityState(), pins));

    // A full tracker replay derives the same readiness bit.
    WITH_LOCK(cs_main,
              m_node.chainman->ActiveChainstate().ModernFinality().MarkDirty());
    BOOST_CHECK(node::BridgeWithdrawalValidatorSetsReady(
        FinalityState(), pins));

    // Disconnecting the Modern-PoS bootstrap block removes the sets and
    // closes burns. Reconsidering the identical block reconstructs them and
    // reopens the gate without any persisted readiness flag.
    {
        BlockValidationState state;
        CBlockIndex* index{WITH_LOCK(
            cs_main,
            return m_node.chainman->m_blockman.LookupBlockIndex(modern_tip))};
        BOOST_REQUIRE(index != nullptr);
        BOOST_REQUIRE(
            m_node.chainman->ActiveChainstate().InvalidateBlock(state, index));
        BOOST_REQUIRE(
            m_node.chainman->ActiveChainstate().ActivateBestChain(state));
    }
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, M - 1);
    BOOST_CHECK(!node::BridgeWithdrawalValidatorSetsReady(
        FinalityState(), pins));

    {
        CBlockIndex* index{WITH_LOCK(
            cs_main,
            return m_node.chainman->m_blockman.LookupBlockIndex(modern_tip))};
        BOOST_REQUIRE(index != nullptr);
        {
            LOCK(cs_main);
            m_node.chainman->ActiveChainstate().ResetBlockFailureFlags(index);
            m_node.chainman->RecalculateBestHeader();
        }
        BlockValidationState state;
        BOOST_REQUIRE(
            m_node.chainman->ActiveChainstate().ActivateBestChain(state));
    }
    BOOST_REQUIRE(Tip()->GetBlockHash() == modern_tip);
    BOOST_CHECK(node::BridgeWithdrawalValidatorSetsReady(
        FinalityState(), pins));
}

BOOST_AUTO_TEST_SUITE_END()
