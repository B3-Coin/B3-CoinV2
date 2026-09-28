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
#include <modern/chain_domain.h>
#include <modern/finality_certificate.h>
#include <modern/metadata_cell.h>
#include <modern/mpa.h>
#include <modern/policy.h>
#include <node/finality_signature.h>
#include <node/finality_signer_store.h>
#include <node/finality_tracker.h>
#include <node/miner.h>
#include <node/staking.h>
#include <streams.h>
#include <test/util/finality_fixture.h>
#include <util/fs_helpers.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>

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

    node::StakingLoop loop(*m_node.chainman, /*mempool=*/nullptr,
                           m_path_root / "finality_signer");
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

    // Checkpoint M+5 becomes signable at M+8. A carries 15 of 16 stake, but
    // the current rule deliberately also requires a validator-headcount
    // quorum, so feed the independent B signature into the same node-local
    // pool once the checkpoint is deep enough. The staking loop contributes
    // A's signature and then includes the aggregate in a later block.
    node::FinalitySigner peer_signer;
    peer_signer.SetKey(m_bls_b, m_vk_b);
    bool peer_signed{false};
    for (int i{0}; i < 1200; ++i) {
        UninterruptibleSleep(std::chrono::milliseconds{25});
        SetMockTime(GetTime() + 30);
        if (!peer_signed && Tip()->nHeight >= M + 8) {
            LOCK(cs_main);
            Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
            node::FinalityTracker& tracker{Finality()};
            peer_signed = !peer_signer
                               .MaybeSign(tracker, chainstate.m_chain,
                                          m_node.chainman->GetConsensus(),
                                          chainstate.FinalitySignatures())
                               .empty();
        }
        if (Tip()->nHeight >= M + 10 &&
            FinalityState().finalized.has_value()) {
            break;
        }
    }
    const auto running{loop.Status(std::nullopt)};
    loop.Stop();
    BOOST_REQUIRE_MESSAGE(Tip()->nHeight >= M + 10,
                          "loop state: " << running.state << " / last error: " << running.last_error);
    BOOST_CHECK(running.finality_signing);
    BOOST_CHECK_GE(running.last_signed_height, M + 5);
    BOOST_REQUIRE(peer_signed);
    // The loop's assembler included the aggregated certificate: finalized.
    const auto s{FinalityState()};
    BOOST_REQUIRE(s.finalized.has_value());
    BOOST_CHECK_GE(s.finalized->height, M + 5);
    BOOST_CHECK(s.handover_certified);
    BOOST_CHECK(WITH_LOCK(cs_main, return m_node.chainman->m_blockman.FinalityAnchor()).has_value());
}

BOOST_FIXTURE_TEST_CASE(regtest_staking_waits_for_blocks_but_keeps_checkpoint_signing, FinalityStakingFixture)
{
    PrepareFinalityChain();
    const fs::path signer_dir{m_path_root / "catchup_finality_signer"};
    std::string error;
    {
        node::FinalitySignerStore store;
        BOOST_REQUIRE_MESSAGE(store.Open(signer_dir, m_domain, m_vk_a, error), error);
        BOOST_REQUIRE_MESSAGE(store.InitializeEmpty(error), error);
    }
    ProduceTo(m_M + 8, m_vk_a); // M+5 is deep enough to sign.
    const int start_height{Tip()->nHeight};
    const CBlock downloaded_block{BuildPosBlock(m_vk_a)};
    SetMockTime(downloaded_block.GetBlockTime());
    WITH_LOCK(cs_main, m_node.chainman->UpdateIBDStatus());
    BOOST_REQUIRE(!m_node.chainman->IsInitialBlockDownload());

    // Learn a real, accepted header without its body after IBD has latched
    // false. This must pause only local production, not checkpoint signing.
    BlockValidationState header_state;
    BOOST_REQUIRE_MESSAGE(m_node.chainman->ProcessNewBlockHeaders(
                              {{CBlockHeader{downloaded_block}}},
                              /*min_pow_checked=*/true, header_state),
                          header_state.ToString());
    node::StakingLoop loop(*m_node.chainman, /*mempool=*/nullptr, signer_dir);
    BOOST_REQUIRE_MESSAGE(loop.SetFinalityKey(m_bls_a, m_vk_a, error), error);
    BOOST_REQUIRE_MESSAGE(loop.Start(m_validator_a, CScript() << OP_TRUE, error), error);
    interfaces::StakingStatus waiting;
    for (int i{0}; i < 400; ++i) {
        waiting = loop.Status(std::nullopt);
        if (waiting.blocks_produced > 0 ||
            (waiting.state == "waiting: regtest block download is behind the best header" &&
             waiting.last_signed_height >= m_M + 5)) break;
        UninterruptibleSleep(std::chrono::milliseconds{10});
        SetMockTime(GetTime() + 30);
    }
    BOOST_CHECK(waiting.finality_signing);
    BOOST_CHECK_GE(waiting.last_signed_height, m_M + 5);
    BOOST_REQUIRE_EQUAL(waiting.blocks_produced, 0U);
    BOOST_CHECK_EQUAL(waiting.next_block_time, 0);
    BOOST_CHECK_EQUAL(waiting.state, "waiting: regtest block download is behind the best header");
    BOOST_CHECK_EQUAL(Tip()->nHeight, start_height);
    UninterruptibleSleep(std::chrono::milliseconds{1100});
    const auto still_waiting{loop.Status(std::nullopt)};
    BOOST_CHECK_EQUAL(still_waiting.blocks_produced, 0U);
    BOOST_CHECK_EQUAL(still_waiting.state, waiting.state);
    BOOST_CHECK_EQUAL(still_waiting.last_signed_height, waiting.last_signed_height);

    // Receiving the body closes the gap without resetting/restarting either
    // signer or staking loop. Ordinary production then resumes.
    BOOST_REQUIRE(Submit(downloaded_block));
    interfaces::StakingStatus resumed;
    for (int i{0}; i < 600; ++i) {
        resumed = loop.Status(std::nullopt);
        if (resumed.blocks_produced > 0) break;
        UninterruptibleSleep(std::chrono::milliseconds{10});
        SetMockTime(GetTime() + 30);
    }
    loop.Stop();
    BOOST_CHECK_MESSAGE(resumed.blocks_produced > 0,
                        "loop state: " << resumed.state << " / last error: " << resumed.last_error);
    BOOST_CHECK_GE(Tip()->nHeight, start_height + 2);
    BOOST_CHECK(resumed.finality_signing);
    BOOST_CHECK_GE(resumed.last_signed_height, waiting.last_signed_height);
}

BOOST_FIXTURE_TEST_CASE(regtest_staking_preserves_last_handover_carrier, FinalityStakingFixture)
{
    PrepareFinalityChain();
    const int last_carrier{m_M + SCALED_E + SCALED_MAX_EXTENSION - 1};
    ProduceTo(last_carrier - 1, m_vk_a);
    BOOST_REQUIRE(!FinalityState().handover_certified);
    BOOST_REQUIRE(!FinalityState().lineage_broken);
    const fs::path signer_dir{m_path_root / "handover_finality_signer"};
    std::string error;
    {
        node::FinalitySignerStore store;
        BOOST_REQUIRE_MESSAGE(store.Open(signer_dir, m_domain, m_vk_a, error), error);
        BOOST_REQUIRE_MESSAGE(store.InitializeEmpty(error), error);
    }
    SetMockTime(Tip()->GetBlockTime() + 1);
    WITH_LOCK(cs_main, m_node.chainman->UpdateIBDStatus());
    BOOST_REQUIRE(!m_node.chainman->IsInitialBlockDownload());
    node::StakingLoop loop(*m_node.chainman, nullptr, signer_dir);
    BOOST_REQUIRE_MESSAGE(loop.StartWithFinalityKey(
                              m_validator_a, CScript() << OP_TRUE, m_bls_a, error), error);
    interfaces::StakingStatus waiting;
    for (int i{0}; i < 600; ++i) {
        waiting = loop.Status(std::nullopt);
        if (waiting.blocks_produced > 0 ||
            waiting.state.find("regtest finality handover required") != std::string::npos) break;
        UninterruptibleSleep(std::chrono::milliseconds{10});
        SetMockTime(GetTime() + 30);
    }
    // Before the fix an empty final carrier is produced. Stop the worker
    // before the fatal assertion; preserve this as a real failed regression.
    if (waiting.blocks_produced != 0) loop.Stop();
    BOOST_REQUIRE_EQUAL(waiting.blocks_produced, 0);
    BOOST_REQUIRE_MESSAGE(waiting.state.find("regtest finality handover required") != std::string::npos,
                          waiting.state << " / " << waiting.last_error);
    BOOST_CHECK(waiting.finality_signing);
    BOOST_CHECK_GE(waiting.last_signed_height, m_M + 55);
    BOOST_CHECK_EQUAL(waiting.next_block_time, 0);
    BOOST_CHECK_EQUAL(Tip()->nHeight, last_carrier - 1);
    const auto journal_bytes = [&] {
        const auto path{node::FinalitySignerStore::StatePath(signer_dir, m_domain, m_vk_a)};
        std::ifstream stream(path.std_path(), std::ios::binary);
        BOOST_REQUIRE(stream.is_open());
        return std::string(std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{});
    };
    const std::string saved_journal{journal_bytes()};
    BOOST_REQUIRE(!saved_journal.empty());

    // Repeated scheduling attempts must retain the same signing watermark,
    // and must not consume the final carrier while quorum is unavailable.
    UninterruptibleSleep(std::chrono::milliseconds{2200});
    const auto held{loop.Status(std::nullopt)};
    BOOST_CHECK_EQUAL(held.blocks_produced, 0);
    BOOST_CHECK_EQUAL(held.last_signed_height, waiting.last_signed_height);
    BOOST_CHECK(journal_bytes() == saved_journal);
    BOOST_CHECK(!FinalityState().lineage_broken);
    // Restart the SAME local signer state while no quorum exists.
    loop.Stop();
    {
        node::FinalitySignerStore store;
        BOOST_REQUIRE_MESSAGE(store.Open(signer_dir, m_domain, m_vk_a, error), error);
        BOOST_REQUIRE(store.State().has_value());
        BOOST_CHECK_EQUAL(store.State()->last_signed_height, waiting.last_signed_height);
    }
    BOOST_REQUIRE_MESSAGE(loop.StartWithFinalityKey(
                              m_validator_a, CScript() << OP_TRUE, m_bls_a, error), error);
    for (int i{0}; i < 600; ++i) {
        waiting = loop.Status(std::nullopt);
        if (waiting.blocks_produced > 0 ||
            waiting.state.find("regtest finality handover required") != std::string::npos) break;
        UninterruptibleSleep(std::chrono::milliseconds{10});
        SetMockTime(GetTime() + 30);
    }
    BOOST_CHECK_EQUAL(waiting.blocks_produced, 0);
    BOOST_CHECK(waiting.finality_signing);
    BOOST_REQUIRE_MESSAGE(waiting.state.find("regtest finality handover required") != std::string::npos,
                          "restart did not reach the guarded production attempt: " << waiting.state);
    BOOST_CHECK(journal_bytes() == saved_journal);

    // An independent B signature supplies the missing headcount. A's own
    // durable vote is still binding; no recovery exception or reset is used.
    node::FinalitySigner peer_signer;
    peer_signer.SetKey(m_bls_b, m_vk_b);
    {
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        BOOST_REQUIRE(!peer_signer.MaybeSign(
            Finality(), chainstate.m_chain, m_node.chainman->GetConsensus(),
            chainstate.FinalitySignatures()).empty());
    }
    for (int i{0}; i < 1200 && Tip()->nHeight < last_carrier + 1; ++i) {
        UninterruptibleSleep(std::chrono::milliseconds{10});
        SetMockTime(GetTime() + 30);
    }
    const auto resumed{loop.Status(std::nullopt)};
    loop.Stop();
    BOOST_REQUIRE_MESSAGE(Tip()->nHeight >= last_carrier + 1,
                          resumed.state << " / " << resumed.last_error);
    BOOST_CHECK(resumed.finality_signing);
    BOOST_CHECK(!FinalityState().lineage_broken);
    BOOST_CHECK_EQUAL(FinalityState().epoch, 1U);
    CBlock carrier;
    BOOST_REQUIRE(m_node.chainman->m_blockman.ReadBlock(carrier, *IndexAt(last_carrier)));
    std::optional<modern::FinalityCertificatePair> pair;
    BOOST_REQUIRE_MESSAGE(modern::MatchFinalityCertificate(*carrier.vtx.at(0), 2, pair, error), error);
    BOOST_REQUIRE(pair.has_value());
    BOOST_CHECK_EQUAL(pair->finalized_block.epoch, 0U);
    BOOST_CHECK_EQUAL(pair->finalized_block.height, static_cast<uint64_t>(m_M + 55));
    // Tracker reconstruction must agree; this is not a memory-only waiver.
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().ModernFinality().MarkDirty());
    BOOST_CHECK(!FinalityState().lineage_broken);
    BOOST_CHECK_EQUAL(FinalityState().epoch, 1U);
}

BOOST_FIXTURE_TEST_CASE(regtest_carrier_rejects_previous_epoch_and_broken_lineage, FinalityChainFixture)
{
    PrepareFinalityChain();
    ProduceTo(m_M + 8, m_vk_a);
    const auto set0{*FinalityState().current};
    Produce(m_vk_a, {MakeCertificate({m_M + 5, 0, FinalityState().next->SetHash()}, set0)});
    ProduceTo(m_M + SCALED_E, m_vk_a);
    BOOST_REQUIRE_EQUAL(FinalityState().epoch, 1U);
    const int last_carrier{m_M + 2 * SCALED_E + SCALED_MAX_EXTENSION - 1};
    ProduceTo(last_carrier - 1, m_vk_a);
    node::BlockAssembler::Options options;
    options.coinbase_output_script = CScript() << OP_TRUE;
    options.modern_pos_validator_key = m_vk_a;
    options.preserve_regtest_finality = true;
    const auto assemble = [&] {
        return node::BlockAssembler(m_node.chainman->ActiveChainstate(), nullptr, options).CreateNewBlock();
    };
    const auto handover_wait = [](const std::runtime_error& e) {
        return std::string{e.what()}.find("regtest finality handover required") != std::string::npos;
    };
    BOOST_CHECK_EXCEPTION(assemble(), std::runtime_error, handover_wait);
    // Default/manual assembly is not consensus enforcement; it still builds
    // an empty carrier under the original rules.
    options.preserve_regtest_finality = false;
    BOOST_CHECK(!assemble()->block.vtx.at(0)->HasMpa());
    options.preserve_regtest_finality = true;
    const auto submit_vote = [&](const uint64_t epoch, const int height,
                                 const modern::ValidatorKeyBytes& validator,
                                 const bls::SecretKey& key) {
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        const auto& tracker{Finality()};
        const auto& state{tracker.Current()};
        const auto set{epoch == state.epoch ? state.current : state.previous};
        BOOST_REQUIRE(set);
        const auto index{set->IndexOf(validator)};
        BOOST_REQUIRE(index.has_value());
        const auto finalized{node::FinalitySignaturePool::ExpectedFinalizedBlock(
            epoch, height, state, chainstate.m_chain, m_node.chainman->GetConsensus())};
        BOOST_REQUIRE(finalized.has_value());
        const uint256 digest{modern::FinalityDigest(m_domain, *finalized)};
        node::FinalitySig vote;
        vote.epoch = epoch;
        vote.height = height;
        vote.index = *index;
        vote.signature = key.Sign(std::span<const unsigned char>{digest.begin(), 32}).Compressed();
        return chainstate.FinalitySignatures().Submit(
            vote, tracker, chainstate.m_chain, m_node.chainman->GetConsensus());
    };
    using Accept = node::FinalitySignaturePool::Accept;
    // This is a genuinely valid, newer previous-epoch certificate. It can be
    // included by the default assembler, but cannot authorize epoch1 handover.
    BOOST_REQUIRE(submit_vote(0, m_M + 25, m_vk_a, m_bls_a) == Accept::ACCEPTED);
    BOOST_REQUIRE(submit_vote(0, m_M + 25, m_vk_b, m_bls_b) == Accept::ACCEPTED);
    options.preserve_regtest_finality = false;
    auto previous{assemble()};
    std::optional<modern::FinalityCertificatePair> pair;
    std::string error;
    BOOST_REQUIRE_MESSAGE(modern::MatchFinalityCertificate(*previous->block.vtx.at(0), 2, pair, error), error);
    BOOST_REQUIRE(pair);
    BOOST_CHECK_EQUAL(pair->finalized_block.epoch, 0U);
    options.preserve_regtest_finality = true;
    BOOST_CHECK_EXCEPTION(assemble(), std::runtime_error, handover_wait);
    // Forged or insufficient current-epoch evidence must still be refused.
    BOOST_CHECK(submit_vote(1, m_M + 85, m_vk_b, m_bls_a) == Accept::BAD_SIGNATURE);
    BOOST_REQUIRE(submit_vote(1, m_M + 85, m_vk_a, m_bls_a) == Accept::ACCEPTED);
    BOOST_CHECK_EXCEPTION(assemble(), std::runtime_error, handover_wait);
    BOOST_REQUIRE(submit_vote(1, m_M + 85, m_vk_b, m_bls_b) == Accept::ACCEPTED);
    auto ready{assemble()};
    BOOST_REQUIRE_MESSAGE(modern::MatchFinalityCertificate(*ready->block.vtx.at(0), 2, pair, error), error);
    BOOST_REQUIRE(pair);
    BOOST_CHECK_EQUAL(pair->finalized_block.epoch, 1U);
    CBlock block{ready->block};
    block.hashMerkleRoot = BlockMerkleRoot(block);
    Sign(block, m_validator_a);
    if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
    BOOST_REQUIRE(Submit(block));
    Produce(m_vk_a);
    BOOST_CHECK_EQUAL(FinalityState().epoch, 2U);
    BOOST_CHECK(!FinalityState().lineage_broken);
    // An unguarded external/manual producer can still exhaust a later epoch.
    // The guarded assembler must report that condition, never clear it.
    const int expiry{FinalityState().epoch_starts.back() + SCALED_E + SCALED_MAX_EXTENSION};
    ProduceTo(expiry, m_vk_a);
    BOOST_REQUIRE(FinalityState().lineage_broken);
    const auto expired = [](const std::runtime_error& e) {
        return std::string{e.what()}.find("regtest finality lineage is already broken") != std::string::npos;
    };
    BOOST_CHECK_EXCEPTION(assemble(), std::runtime_error, expired);
    BOOST_CHECK(FinalityState().lineage_broken);
    options.preserve_regtest_finality = false;
    BOOST_REQUIRE(assemble());
    BOOST_CHECK(FinalityState().lineage_broken);
}

BOOST_FIXTURE_TEST_CASE(regtest_carrier_uses_older_valid_pooled_certificate, FinalityChainFixture)
{
    PrepareFinalityChain();
    // A delayed first handover shifts epoch1 off the checkpoint grid. Its
    // final carrier is M+92; checkpoint M+90 is too shallow for that carrier.
    ProduceTo(m_M + 31, m_vk_a);
    const auto set0{*FinalityState().current};
    Produce(m_vk_a, {MakeCertificate({m_M + 25, 0, FinalityState().next->SetHash()}, set0)});
    Produce(m_vk_a);
    BOOST_REQUIRE_EQUAL(FinalityState().epoch, 1U);
    BOOST_REQUIRE_EQUAL(FinalityState().epoch_starts.back(), m_M + 33);
    const int last_carrier{m_M + 92};
    ProduceTo(last_carrier - 1, m_vk_a);
    const Consensus::Params& params{m_node.chainman->GetConsensus()};
    using Accept = node::FinalitySignaturePool::Accept;
    const auto make_votes = [&](const int height) {
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        const auto& state{Finality().Current()};
        const auto set{state.epoch == 1 ? state.current : state.previous};
        BOOST_REQUIRE(set);
        const auto finalized{node::FinalitySignaturePool::ExpectedFinalizedBlock(
            1, height, state, chainstate.m_chain, params)};
        BOOST_REQUIRE(finalized);
        const uint256 digest{modern::FinalityDigest(m_domain, *finalized)};
        std::vector<node::FinalitySig> votes;
        for (const auto& [validator, key] : {
                 std::pair{m_vk_a, &m_bls_a}, std::pair{m_vk_b, &m_bls_b}}) {
            const auto index{set->IndexOf(validator)};
            BOOST_REQUIRE(index);
            node::FinalitySig vote;
            vote.epoch = 1;
            vote.height = height;
            vote.index = *index;
            vote.signature = key->Sign(std::span<const unsigned char>{digest.begin(), 32}).Compressed();
            votes.push_back(vote);
        }
        return votes;
    };
    const auto submit_votes = [&](const std::vector<node::FinalitySig>& votes) {
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        const auto& tracker{Finality()};
        for (const auto& vote : votes) {
            BOOST_REQUIRE(chainstate.FinalitySignatures().Submit(
                              vote, tracker, chainstate.m_chain, params) == Accept::ACCEPTED);
        }
    };
    const auto raw_best = [&] {
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        return chainstate.FinalitySignatures().BestCertificate(
            Finality(), chainstate.m_chain, params);
    };
    // Retain these exact received messages for replay after the carrier is
    // disconnected. No signer is rewound or asked to create an older vote.
    const auto older_votes{make_votes(m_M + 85)};
    submit_votes(older_votes);
    const auto older{raw_best()};
    BOOST_REQUIRE(older);
    BOOST_REQUIRE_EQUAL(older->first.height, static_cast<uint64_t>(m_M + 85));
    const auto [older_payload, older_cell]{modern::BuildFinalityCertificate(older->first, older->second)};
    CMpaRecord record;
    record.payload_type = modern::MPA_TYPE_FINALITY_CERTIFICATE;
    record.payload_version = modern::MPA_VERSION_V1;
    record.payload = older_payload;
    const uint256 original_carrier{Produce(m_vk_a, {{older_cell, record}})};
    Produce(m_vk_a);
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, m_M + 93);
    BOOST_REQUIRE_EQUAL(FinalityState().epoch, 2U);
    const auto newer_votes{make_votes(m_M + 90)};
    submit_votes(newer_votes); // Valid previous-epoch votes, now depth3.
    const auto newer{raw_best()};
    BOOST_REQUIRE(newer);
    BOOST_REQUIRE_EQUAL(newer->first.height, static_cast<uint64_t>(m_M + 90));
    const auto newer_payload{modern::BuildFinalityCertificate(newer->first, newer->second).first};

    // Supported operator rollback on disposable data, NOT spontaneous
    // higher-work fork choice: remove only the carrier and its descendant,
    // strictly above the sticky M+85 checkpoint. The pool survives normally.
    const auto anchor{WITH_LOCK(cs_main, return m_node.chainman->m_blockman.FinalityAnchor())};
    BOOST_REQUIRE(anchor);
    BOOST_REQUIRE_EQUAL(anchor->first, m_M + 85);
    BOOST_REQUIRE(anchor->second == ChainHashAt(m_M + 85));
    CBlockIndex* const carrier_index{WITH_LOCK(
        cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(original_carrier))};
    BOOST_REQUIRE(carrier_index);
    BOOST_REQUIRE_GT(carrier_index->nHeight - 1, anchor->first);
    BlockValidationState rollback_state;
    BOOST_REQUIRE_MESSAGE(m_node.chainman->ActiveChainstate().InvalidateBlock(rollback_state, carrier_index),
                          rollback_state.ToString());
    BOOST_REQUIRE_MESSAGE(m_node.chainman->ActiveChainstate().ActivateBestChain(rollback_state),
                          rollback_state.ToString());
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, last_carrier - 1);
    BOOST_REQUIRE_EQUAL(FinalityState().epoch, 1U);
    BOOST_REQUIRE(!FinalityState().handover_certified);
    BOOST_REQUIRE(!FinalityState().lineage_broken);
    BOOST_REQUIRE(FinalityState().finalized);
    BOOST_REQUIRE_EQUAL(FinalityState().finalized->height, m_M + 25);
    BOOST_CHECK(WITH_LOCK(cs_main, return m_node.chainman->m_blockman.FinalityAnchor()) == anchor);
    submit_votes(older_votes); // Replay the exact saved M+85 signatures.
    {
        LOCK(cs_main);
        auto& tracker{Finality()};
        const auto newest{raw_best()};
        BOOST_REQUIRE(newest);
        BOOST_CHECK_EQUAL(newest->first.height, static_cast<uint64_t>(m_M + 90));
        BOOST_CHECK(modern::BuildFinalityCertificate(newest->first, newest->second).first == newer_payload);
        std::string error;
        BOOST_CHECK(!tracker.JudgeCandidateCertificate(newest->first, newest->second, *Tip(), params, error));
        BOOST_CHECK_EQUAL(error, "insufficient-depth");
        BOOST_REQUIRE_MESSAGE(tracker.JudgeCandidateCertificate(older->first, older->second, *Tip(), params, error), error);
    }
    node::BlockAssembler::Options options;
    options.coinbase_output_script = CScript() << OP_2; // Distinct from the invalidated carrier.
    options.modern_pos_validator_key = m_vk_a;
    options.preserve_regtest_finality = true;
    // Before candidate-aware selection this throws: raw BestCertificate keeps
    // returning M+90 and the guard cannot deepen the chain to make it usable.
    std::unique_ptr<node::CBlockTemplate> ready;
    std::string assembly_error;
    try {
        ready = node::BlockAssembler(m_node.chainman->ActiveChainstate(), nullptr, options).CreateNewBlock();
    } catch (const std::runtime_error& e) {
        assembly_error = e.what();
    }
    BOOST_REQUIRE_MESSAGE(ready, "guarded assembly refused the usable M+85 certificate: " << assembly_error);
    std::optional<modern::FinalityCertificatePair> pair;
    std::string error;
    BOOST_REQUIRE_MESSAGE(modern::MatchFinalityCertificate(*ready->block.vtx.at(0), 2, pair, error), error);
    BOOST_REQUIRE(pair);
    BOOST_CHECK_EQUAL(pair->finalized_block.epoch, 1U);
    BOOST_CHECK_EQUAL(pair->finalized_block.height, static_cast<uint64_t>(m_M + 85));
    BOOST_CHECK(modern::BuildFinalityCertificate(pair->finalized_block, pair->certificate).first == older_payload);
    {
        LOCK(cs_main);
        const auto& pool{m_node.chainman->ActiveChainstate().FinalitySignatures()};
        BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(), 2U);
        BOOST_CHECK_EQUAL(pool.SignatureCount(1, m_M + 85), older_votes.size());
        BOOST_CHECK_EQUAL(pool.SignatureCount(1, m_M + 90), newer_votes.size());
        const auto newest{raw_best()};
        BOOST_REQUIRE(newest);
        BOOST_CHECK(modern::BuildFinalityCertificate(newest->first, newest->second).first == newer_payload);
    }
    CBlock block{ready->block};
    block.hashMerkleRoot = BlockMerkleRoot(block);
    Sign(block, m_validator_a);
    BOOST_REQUIRE(block.GetHash() != original_carrier);
    if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
    BOOST_REQUIRE(Submit(block));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, last_carrier);
    BOOST_CHECK(FinalityState().handover_certified);
    Produce(m_vk_a);
    BOOST_CHECK_EQUAL(FinalityState().epoch, 2U);
    BOOST_CHECK(!FinalityState().lineage_broken);
    BOOST_CHECK(WITH_LOCK(cs_main, return m_node.chainman->m_blockman.FinalityAnchor()) == anchor);
}

BOOST_FIXTURE_TEST_CASE(staking_stop_forgets_finality_key_before_another_wallet_starts, FinalityStakingFixture)
{
    PrepareFinalityChain();
    node::StakingLoop loop(*m_node.chainman, /*mempool=*/nullptr,
                           m_path_root / "finality_signer");
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
    PrepareFinalityChain();
    node::StakingLoop loop(*m_node.chainman, /*mempool=*/nullptr,
                           m_path_root / "finality_signer");
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

BOOST_FIXTURE_TEST_CASE(staking_reports_corrupt_finality_journal_as_disabled, FinalityStakingFixture)
{
    PrepareFinalityChain();
    const Consensus::Params& params{m_node.chainman->GetConsensus()};
    BOOST_REQUIRE(params.legacy_final_hash.has_value());
    const auto domain{modern::ModernChainDomain(
        params.hashGenesisBlock, *params.legacy_final_hash)};
    BOOST_REQUIRE(domain.has_value());
    const fs::path signer_dir{m_path_root / "corrupt_finality_signer"};
    BOOST_REQUIRE(TryCreateDirectories(signer_dir));
    {
        std::ofstream corrupt{
            node::FinalitySignerStore::StatePath(signer_dir, *domain, m_vk_a)
                .std_path(),
            std::ios::binary | std::ios::trunc};
        BOOST_REQUIRE(corrupt.is_open());
        corrupt << "not a finality signer journal";
        corrupt.close();
        BOOST_REQUIRE(corrupt.good());
    }

    node::StakingLoop loop(*m_node.chainman, /*mempool=*/nullptr,
                           signer_dir);
    std::string error;
    BOOST_REQUIRE_MESSAGE(loop.SetFinalityKey(m_bls_a, m_vk_a, error), error);
    BOOST_REQUIRE_MESSAGE(loop.Start(m_validator_a, CScript() << OP_TRUE,
                                     error),
                          error);

    interfaces::StakingStatus status;
    for (int i{0}; i < 200; ++i) {
        status = loop.Status(std::nullopt);
        if (!status.finality_signing &&
            status.last_error.find("finality signing disabled safely") !=
                std::string::npos) {
            break;
        }
        UninterruptibleSleep(std::chrono::milliseconds{5});
    }
    loop.Stop();
    BOOST_CHECK(!status.finality_signing);
    BOOST_CHECK(status.last_error.find("unsafe finality signer state") !=
                std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(staking_reports_missing_snapshot_key_and_resumes_with_retained_key, FinalityStakingFixture)
{
    PrepareFinalityChain();
    const fs::path signer_dir{m_path_root / "snapshot_finality_signer"};
    std::string error;
    node::FinalitySignerStore store;
    BOOST_REQUIRE_MESSAGE(store.Open(signer_dir, m_domain, m_vk_a, error), error);
    BOOST_REQUIRE_MESSAGE(store.InitializeEmpty(error), error);
    ProduceTo(m_M + 8, m_vk_a);
    SetMockTime(Tip()->GetBlockTime() + 1);
    WITH_LOCK(cs_main, m_node.chainman->UpdateIBDStatus());
    BOOST_REQUIRE(!m_node.chainman->IsInitialBlockDownload());

    node::StakingLoop loop(*m_node.chainman, /*mempool=*/nullptr, signer_dir);
    const bls::SecretKey future_key{Bls(9)};
    BOOST_CHECK(!loop.StartWithFinalityKeys(
        m_validator_a, CScript() << OP_TRUE,
        std::vector<bls::SecretKey>(node::FinalitySigner::MAX_KEYS + 1, future_key), error));
    BOOST_CHECK(!loop.HasFinalityKey());
    BOOST_REQUIRE_MESSAGE(loop.StartWithFinalityKeys(
                              m_validator_a, CScript() << OP_TRUE,
                              {future_key}, error), error);
    interfaces::StakingStatus status;
    for (int i{0}; i < 200; ++i) {
        status = loop.Status(std::nullopt);
        if (!status.finality_signing && status.last_error.find(
                "missing finality private key for current epoch") != std::string::npos) break;
        UninterruptibleSleep(std::chrono::milliseconds{5});
    }
    loop.Stop();
    BOOST_CHECK(!status.finality_signing);
    BOOST_CHECK_EQUAL(status.last_signed_height, -1);
    BOOST_CHECK(status.last_error.find("missing finality private key for current epoch") != std::string::npos);
    BOOST_CHECK(!loop.HasFinalityKey());

    // Reload the retained current key alongside the future key, keeping the
    // exact same validator journal. Selection must ignore collection order.
    BOOST_REQUIRE_MESSAGE(loop.StartWithFinalityKeys(
                              m_validator_a, CScript() << OP_TRUE,
                              {future_key, m_bls_a}, error), error);
    for (int i{0}; i < 200; ++i) {
        status = loop.Status(std::nullopt);
        if (status.finality_signing && status.last_signed_height >= m_M + 5) break;
        UninterruptibleSleep(std::chrono::milliseconds{5});
    }
    loop.Stop();
    BOOST_CHECK(status.finality_signing);
    BOOST_CHECK_GE(status.last_signed_height, m_M + 5);
    BOOST_CHECK(status.last_error.empty());
    BOOST_CHECK(!loop.HasFinalityKey());
}

BOOST_AUTO_TEST_SUITE_END()
