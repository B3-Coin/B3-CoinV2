// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 16 of the Modern PoS V1 finality plan: finality-bearing block
// production. The assembler discovers a quorum certificate in the local
// pool, judges it with the consensus rule, and includes cell + type-4
// record + recomputed MODERN_PAYLOAD_ROOT in the coinbase; without a
// quorum nothing is included and the epoch simply extends; the automatic
// staking loop signs scheduled checkpoints and produces the certificate-
// carrying blocks end to end.

#include <chain.h>
#include <consensus/merkle.h>
#include <modern/finality_certificate.h>
#include <modern/metadata_cell.h>
#include <modern/mpa.h>
#include <modern/policy.h>
#include <node/finality_signature.h>
#include <node/finality_tracker.h>
#include <node/miner.h>
#include <node/staking.h>
#include <test/util/finality_fixture.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

using b3test::FinalityChainFixture;

namespace {

//! The staking-loop scenario needs the widened tip-age bound (the fixture's
//! mock clock sits far past the synthetic chain's block times).
struct FinalityStakingFixture : public FinalityChainFixture {
    FinalityStakingFixture() : FinalityChainFixture{{.extra_args = {"-maxtipage=1000000000"}}} {}
};

} // namespace

BOOST_AUTO_TEST_SUITE(finality_production_tests)

BOOST_FIXTURE_TEST_CASE(assembler_includes_judged_certificate_and_root, FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    ProduceTo(M + 8, m_vk_a);
    const Consensus::Params& params{m_node.chainman->GetConsensus()};
    ChainstateManager& chainman{*m_node.chainman};

    node::BlockAssembler::Options options;
    options.coinbase_output_script = CScript() << OP_TRUE;
    options.include_dummy_extranonce = true;
    options.modern_pos_validator_key = m_vk_a;

    // Empty pool: the template carries no certificate and no root cell --
    // blocks never depend on certificates.
    {
        const auto tmpl{node::BlockAssembler(chainman.ActiveChainstate(), nullptr, options).CreateNewBlock()};
        BOOST_REQUIRE(tmpl);
        BOOST_CHECK(!tmpl->block.vtx[0]->HasMpa());
        std::optional<modern::FinalityCertificatePair> pair;
        std::string err;
        BOOST_REQUIRE(modern::MatchFinalityCertificate(*tmpl->block.vtx[0], 2, pair, err));
        BOOST_CHECK(!pair.has_value());
    }
    // Both validators sign through their signers into the CHAINSTATE pool.
    {
        LOCK(cs_main);
        Chainstate& cs{chainman.ActiveChainstate()};
        node::FinalityTracker& tracker{Finality()};
        node::FinalitySigner sa;
        sa.SetKey(m_bls_a, m_vk_a);
        node::FinalitySigner sb;
        sb.SetKey(m_bls_b, m_vk_b);
        BOOST_CHECK(!sa.MaybeSign(tracker, cs.m_chain, params, cs.FinalitySignatures()).empty());
        BOOST_CHECK(!sb.MaybeSign(tracker, cs.m_chain, params, cs.FinalitySignatures()).empty());
    }
    // The next template includes the judged certificate (highest quorum
    // checkpoint: M+5), the type-4 record, and exactly one payload-root cell.
    const auto tmpl{node::BlockAssembler(chainman.ActiveChainstate(), nullptr, options).CreateNewBlock()};
    BOOST_REQUIRE(tmpl);
    CBlock block{tmpl->block};
    {
        const CTransaction& cb{*block.vtx[0]};
        BOOST_REQUIRE(cb.HasMpa());
        BOOST_REQUIRE_EQUAL(cb.mpa.size(), 1U);
        BOOST_CHECK_EQUAL(cb.mpa[0].payload_type, modern::MPA_TYPE_FINALITY_CERTIFICATE);
        std::optional<modern::FinalityCertificatePair> pair;
        std::string err;
        BOOST_REQUIRE_MESSAGE(modern::MatchFinalityCertificate(cb, 2, pair, err), err);
        BOOST_REQUIRE(pair.has_value());
        BOOST_CHECK_EQUAL(pair->finalized_block.height, static_cast<uint64_t>(M + 5));
        BOOST_CHECK_EQUAL(pair->finalized_block.epoch, 0U);
        BOOST_CHECK(pair->finalized_block.withdrawal_root.IsNull());
        int root_cells{0};
        for (const auto& out : cb.vout) {
            const auto cell{modern::ParseMetadataCell(out.scriptPubKey)};
            if (cell && cell->policy_type == static_cast<uint16_t>(modern::PolicyType::MODERN_PAYLOAD_ROOT)) ++root_cells;
        }
        BOOST_CHECK_EQUAL(root_cells, 1);
    }
    // Sign and submit: consensus accepts the produced block and finalizes.
    block.hashMerkleRoot = BlockMerkleRoot(block);
    Sign(block, m_validator_a);
    if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
    BOOST_REQUIRE(Submit(block));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, M + 9);
    BOOST_CHECK_EQUAL(FinalityState().finalized->height, M + 5);
    BOOST_CHECK(FinalityState().handover_certified);
    // The stale slots are pruned on the next pool touch; a repeat template
    // does not re-include an old certificate (regression is pre-judged away).
    {
        const auto tmpl2{node::BlockAssembler(chainman.ActiveChainstate(), nullptr, options).CreateNewBlock()};
        BOOST_REQUIRE(tmpl2);
        BOOST_CHECK(!tmpl2->block.vtx[0]->HasMpa());
    }
}

BOOST_FIXTURE_TEST_CASE(staking_loop_signs_aggregates_and_finalizes, FinalityStakingFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    Produce(m_vk_a); // tip = M so the loop starts inside the modern phase

    node::StakingLoop loop(*m_node.chainman, /*mempool=*/nullptr);
    std::string error;
    BOOST_REQUIRE_MESSAGE(loop.SetFinalityKey(m_bls_a, m_vk_a, error), error);
    {
        const auto idle{loop.Status(std::nullopt)};
        BOOST_CHECK(idle.finality_signing);
        BOOST_CHECK_EQUAL(idle.last_signed_height, -1);
    }
    SetMockTime(Tip()->GetBlockTime() + 1);
    WITH_LOCK(cs_main, m_node.chainman->UpdateIBDStatus());
    BOOST_REQUIRE(!m_node.chainman->IsInitialBlockDownload());
    BOOST_REQUIRE_MESSAGE(loop.Start(m_validator_a, CScript() << OP_TRUE, error), error);
    BOOST_CHECK(!loop.SetFinalityKey(m_bls_a, m_vk_a, error)); // refused while running

    // Run until the chain reaches M+10 (checkpoint M+5 becomes signable at
    // M+8 and includable immediately: A alone is 15 of 16 >= quorum 11).
    for (int i{0}; i < 1200 && Tip()->nHeight < M + 10; ++i) {
        UninterruptibleSleep(std::chrono::milliseconds{25});
        SetMockTime(GetTime() + 30);
    }
    const auto running{loop.Status(std::nullopt)};
    loop.Stop();
    BOOST_REQUIRE_MESSAGE(Tip()->nHeight >= M + 10,
                          "loop state: " << running.state << " / last error: " << running.last_error);
    BOOST_CHECK(running.finality_signing);
    BOOST_CHECK_GE(running.last_signed_height, M + 5);
    // The loop's assembler included the aggregated certificate: finalized.
    const auto s{FinalityState()};
    BOOST_REQUIRE(s.finalized.has_value());
    BOOST_CHECK_GE(s.finalized->height, M + 5);
    BOOST_CHECK(s.handover_certified);
    BOOST_CHECK(WITH_LOCK(cs_main, return m_node.chainman->m_blockman.FinalityAnchor()).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
