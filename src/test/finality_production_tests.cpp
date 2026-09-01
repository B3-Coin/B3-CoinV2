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
#include <streams.h>
#include <test/util/finality_fixture.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <span>
#include <stdexcept>

using b3test::FinalityChainFixture;

namespace {

//! The staking-loop scenario needs the widened tip-age bound (the fixture's
//! mock clock sits far past the synthetic chain's block times).
struct FinalityStakingFixture : public FinalityChainFixture {
    FinalityStakingFixture() : FinalityChainFixture{{.extra_args = {"-maxtipage=1000000000"}}} {}
};

//! Rebuild the exact assembler coinbase from the fields exposed through the
//! mining interface. The payout script is the one miner-controlled field in
//! this test; all mandatory outputs and the MPA section come from CoinbaseTx.
CTransaction ReconstructCoinbase(const node::CoinbaseTx& fields,
                                 const CScript& payout_script)
{
    CMutableTransaction tx;
    tx.version = static_cast<int32_t>(fields.version);
    tx.vin.resize(1);
    tx.vin[0].prevout.SetNull();
    tx.vin[0].nSequence = fields.sequence;
    tx.vin[0].scriptSig = fields.script_sig_prefix;
    if (fields.witness) {
        tx.vin[0].scriptWitness.stack.emplace_back(fields.witness->begin(),
                                                    fields.witness->end());
    }
    tx.vout.emplace_back(fields.block_reward_remaining, payout_script);
    tx.vout.insert(tx.vout.end(), fields.required_outputs.begin(),
                   fields.required_outputs.end());
    tx.nLockTime = fields.lock_time;
    if (!fields.mpa_section.empty()) {
        DataStream section{std::span<const uint8_t>{fields.mpa_section}};
        UnserializeMpaSection(section, tx.mpa);
        if (!section.empty()) {
            throw std::runtime_error("trailing bytes in coinbase MPA section");
        }
    }
    return CTransaction{std::move(tx)};
}

} // namespace

BOOST_AUTO_TEST_SUITE(finality_production_tests)

BOOST_FIXTURE_TEST_CASE(assembler_includes_judged_certificate_and_root, FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    ProduceTo(M + 8, m_vk_a);

    // Turn on a distinct reward split only for the template under test.
    // The fixture's hand-built preparation blocks intentionally use its
    // default zero-reward coinbase and therefore must be produced first.
    Consensus::ModernPosParams& pos{*MutableConsensus().modern_pos};
    pos.reward = 1'000;
    pos.treasury_percent = 10;
    const CScript treasury_script{CScript() << OP_2};
    pos.treasury_script.assign(treasury_script.begin(), treasury_script.end());
    BOOST_REQUIRE(pos.Valid());
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
        BOOST_CHECK_EQUAL(tmpl->m_coinbase_tx.block_reward_remaining, 900);
        const int witness_index{GetWitnessCommitmentIndex(tmpl->block)};
        BOOST_REQUIRE(witness_index != NO_WITNESS_COMMITMENT);
        BOOST_REQUIRE_EQUAL(tmpl->m_coinbase_tx.required_outputs.size(), 2U);
        BOOST_CHECK(tmpl->m_coinbase_tx.required_outputs[0] ==
                    CTxOut(100, treasury_script));
        BOOST_CHECK(tmpl->m_coinbase_tx.required_outputs[1] ==
                    tmpl->block.vtx[0]->vout.at(static_cast<size_t>(witness_index)));
        BOOST_CHECK(tmpl->m_coinbase_tx.mpa_section.empty());
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

        // Every non-miner-controlled field needed by GBT/IPC reconstruction
        // is present: treasury, certificate cell, payload root, and the exact
        // serialized coinbase MPA. The normative full-form id must match.
        const node::CoinbaseTx& fields{tmpl->m_coinbase_tx};
        BOOST_CHECK_EQUAL(fields.block_reward_remaining, 900);
        const int witness_index{GetWitnessCommitmentIndex(block)};
        BOOST_REQUIRE(witness_index != NO_WITNESS_COMMITMENT);
        BOOST_REQUIRE_EQUAL(fields.required_outputs.size(), 4U);
        BOOST_CHECK(fields.required_outputs[0] == CTxOut(100, treasury_script));
        BOOST_CHECK(fields.required_outputs[1] == cb.vout[2]);
        BOOST_CHECK(fields.required_outputs[2] == cb.vout[3]);
        BOOST_CHECK(fields.required_outputs[3] ==
                    cb.vout.at(static_cast<size_t>(witness_index)));
        BOOST_CHECK(!fields.mpa_section.empty());
        const CTransaction rebuilt{ReconstructCoinbase(fields, CScript() << OP_TRUE)};
        BOOST_CHECK(rebuilt.GetPtxid() == cb.GetPtxid());
        BOOST_CHECK(rebuilt.mpa == cb.mpa);
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

BOOST_FIXTURE_TEST_CASE(staking_stop_forgets_finality_key_before_another_wallet_starts, FinalityStakingFixture)
{
    node::StakingLoop loop(*m_node.chainman, /*mempool=*/nullptr);
    std::string error;

    // Wallet A arms finality and starts the node-global loop.
    BOOST_REQUIRE_MESSAGE(loop.SetFinalityKey(m_bls_a, m_vk_a, error), error);
    BOOST_REQUIRE_MESSAGE(loop.Start(m_validator_a, CScript() << OP_TRUE, error), error);
    {
        const auto running_a{loop.Status(std::nullopt)};
        BOOST_REQUIRE(running_a.validator_key.has_value());
        BOOST_CHECK(*running_a.validator_key == m_vk_a);
        BOOST_CHECK(running_a.finality_signing);
    }

    // Stopping is the authorization boundary: no copied finality key remains.
    loop.Stop();
    BOOST_CHECK(!loop.HasFinalityKey());
    BOOST_CHECK(!loop.Status(std::nullopt).finality_signing);

    // Wallet B starts without arming a key. It must not inherit wallet A's
    // BLS secret merely because both wallets share the same node loop.
    BOOST_REQUIRE_MESSAGE(loop.Start(m_validator_b, CScript() << OP_2, error), error);
    {
        const auto running_b{loop.Status(std::nullopt)};
        BOOST_REQUIRE(running_b.validator_key.has_value());
        BOOST_CHECK(*running_b.validator_key == m_vk_b);
        BOOST_CHECK(!running_b.finality_signing);
    }
    loop.Stop();
}

BOOST_FIXTURE_TEST_CASE(staking_atomic_start_replaces_a_stale_same_validator_finality_key, FinalityStakingFixture)
{
    node::StakingLoop loop(*m_node.chainman, /*mempool=*/nullptr);
    std::string error;

    // Simulate a key armed by an earlier attempt for the same validator. A
    // fresh wallet/RPC decision that has no usable live binding must replace
    // it with no signer, rather than silently retaining the stale secret.
    BOOST_REQUIRE_MESSAGE(loop.SetFinalityKey(m_bls_a, m_vk_a, error), error);
    BOOST_REQUIRE(loop.HasFinalityKey());
    BOOST_REQUIRE_MESSAGE(
        loop.StartWithFinalityKey(m_validator_a, CScript() << OP_TRUE,
                                  /*finality_key=*/std::nullopt, error),
        error);
    BOOST_CHECK(!loop.Status(std::nullopt).finality_signing);
    loop.Stop();
}

BOOST_AUTO_TEST_SUITE_END()
