// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 17 of the Modern PoS V1 finality plan: operability. Binding
// transactions are standard (metadata-cell carve-in, dust exemption), relay
// with their evidence (TX_MODERN wire codec), pass the mempool's semantic
// pre-check, and are mined from the mempool into modern-PoS blocks; the
// interfaces::Chain::finalityStatus diagnostics carry everything the RPCs
// and the future Qt UI read.

#include <chain.h>
#include <consensus/merkle.h>
#include <interfaces/chain.h>
#include <node/finality_tracker.h>
#include <node/miner.h>
#include <addresstype.h>
#include <policy/policy.h>
#include <script/solver.h>
#include <streams.h>
#include <test/util/finality_fixture.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

using b3test::FinalityChainFixture;

BOOST_AUTO_TEST_SUITE(finality_operations_tests)

BOOST_FIXTURE_TEST_CASE(binding_tx_standard_relayable_and_minable, FinalityChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};

    // The finality tracker deliberately scans the corridor to prepare Set_0,
    // but the public epoch status must remain inactive until block M itself
    // has connected. Binding diagnostics are already available independently.
    {
        const auto pre_m{m_node.chain->finalityStatus(m_vk_a)};
        BOOST_CHECK(pre_m.configured);
        BOOST_CHECK(!pre_m.active);
        BOOST_CHECK(pre_m.bound);
        BOOST_CHECK(!pre_m.bootstrapped);
        BOOST_CHECK_EQUAL(Tip()->nHeight, M - 1);
    }

    ProduceTo(M + 2, m_vk_a);
    ChainstateManager& chainman{*m_node.chainman};

    // A new validator D: identity key, BLS key, first binding (seq 0).
    const CKey identity_d{MakeValidatorKey(0x44)};
    const modern::ValidatorKeyBytes vk_d{XOnly(identity_d)};
    const bls::SecretKey bls_d{Bls(4)};
    const auto bind_d{MakeBinding(identity_d, vk_d, &bls_d, 0)};
    CMutableTransaction tx{MakeTx(9, {bind_d.cell}, {bind_d.record})};
    tx.vout[0].nValue -= 1'000'000; // a solid fee for relay
    // A standard change output (the fixture's OP_TRUE scripts are non-standard
    // by design and would mask the cell's own standardness).
    tx.vout[0].scriptPubKey = GetScriptForDestination(PKHash(identity_d.GetPubKey()));

    // Standardness: the cell is a data-carrier-like output (never OP_RETURN),
    // dust-exempt at zero value; the whole transaction is standard.
    {
        std::vector<std::vector<unsigned char>> solutions;
        BOOST_CHECK(Solver(bind_d.cell, solutions) == TxoutType::NULL_DATA);
        BOOST_CHECK(!bind_d.cell.IsUnspendable()); // not OP_RETURN
        BOOST_CHECK_EQUAL(GetDustThreshold(CTxOut{0, bind_d.cell}, CFeeRate{DUST_RELAY_TX_FEE}), 0);
        std::string reason;
        BOOST_CHECK_MESSAGE(IsStandardTx(CTransaction{tx}, MAX_OP_RETURN_RELAY, /*permit_bare_multisig=*/true,
                                         CFeeRate{DUST_RELAY_TX_FEE}, reason),
                            reason);
        // A malformed claim stays nonstandard.
        CScript broken{bind_d.cell};
        broken << OP_1;
        BOOST_CHECK(Solver(broken, solutions) == TxoutType::NONSTANDARD);
    }
    // Wire: TX_MODERN preserves the evidence; the witness-only codec has no
    // payload form at all (relay uses TX_MODERN so nothing lossy exists).
    {
        DataStream modern_bytes;
        modern_bytes << TX_MODERN(CTransaction{tx});
        DataStream witness_bytes;
        witness_bytes << TX_WITH_WITNESS(CTransaction{tx});
        BOOST_CHECK_GT(modern_bytes.size(), witness_bytes.size());
        CMutableTransaction back;
        modern_bytes >> TX_MODERN(back);
        BOOST_REQUIRE_EQUAL(back.mpa.size(), 1U);
        BOOST_CHECK(back.mpa[0].payload == bind_d.record.payload);
        BOOST_CHECK(CTransaction{back}.GetHash() == CTransaction{tx}.GetHash());
    }
    // Mempool semantic pre-check: a wrong-sequence binding is refused before
    // it could ever waste a block; the valid one is accepted.
    {
        LOCK(cs_main);
        const auto bad{MakeBinding(identity_d, vk_d, &bls_d, 5)};
        CMutableTransaction bad_tx{MakeTx(10, {bad.cell}, {bad.record})};
        bad_tx.vout[0].nValue -= 1'000'000;
        const auto res{chainman.ProcessTransaction(MakeTransactionRef(bad_tx))};
        BOOST_CHECK(res.m_result_type == MempoolAcceptResult::ResultType::INVALID);
        BOOST_CHECK_EQUAL(res.m_state.GetRejectReason(), "finality-key-bad-first-seq");
    }
    {
        LOCK(cs_main);
        const auto res{chainman.ProcessTransaction(MakeTransactionRef(tx))};
        BOOST_CHECK_MESSAGE(res.m_result_type == MempoolAcceptResult::ResultType::VALID,
                            res.m_state.GetRejectReason());
    }
    // The assembler mines it from the mempool; the block carries the payload
    // root; connect updates the binding index.
    node::BlockAssembler::Options options;
    options.coinbase_output_script = CScript() << OP_TRUE;
    options.include_dummy_extranonce = true;
    options.modern_pos_validator_key = m_vk_a;
    const auto tmpl{node::BlockAssembler(chainman.ActiveChainstate(), m_node.mempool.get(), options).CreateNewBlock()};
    BOOST_REQUIRE(tmpl);
    CBlock block{tmpl->block};
    BOOST_REQUIRE_EQUAL(block.vtx.size(), 2U);
    BOOST_CHECK(block.vtx[1]->HasMpa());
    block.hashMerkleRoot = BlockMerkleRoot(block);
    Sign(block, m_validator_a);
    if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
    BOOST_REQUIRE(Submit(block));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, M + 3);

    // The diagnostics interface sees the new binding (not yet in the set:
    // snapshots change only at epoch boundaries) and validator A's membership.
    const auto status_d{m_node.chain->finalityStatus(vk_d)};
    BOOST_CHECK(status_d.configured && status_d.active && status_d.bootstrapped);
    BOOST_CHECK(status_d.bound);
    BOOST_CHECK(!status_d.revoked);
    BOOST_CHECK_EQUAL(status_d.binding_seq, 0U);
    {
        const auto pk_d{bls_d.GetPublicKey().Compressed()};
        BOOST_CHECK(status_d.binding_bls_pubkey == std::vector<unsigned char>(pk_d.begin(), pk_d.end()));
    }
    BOOST_CHECK(!status_d.in_current_set);
    const auto status_a{m_node.chain->finalityStatus(m_vk_a)};
    BOOST_CHECK(status_a.bound && status_a.in_current_set);
    BOOST_CHECK_EQUAL(status_a.member_weight, 15U);
    BOOST_CHECK_EQUAL(status_a.total_weight, 16U);
    BOOST_CHECK_EQUAL(status_a.quorum_weight, 11U);
    BOOST_CHECK_EQUAL(status_a.epoch, 0U);
    BOOST_CHECK_EQUAL(status_a.chain_domain.GetHex(), m_domain.GetHex());
    BOOST_CHECK(!status_a.finalized_height.has_value());

    // Finalize a checkpoint: the diagnostics carry finalized state and pin.
    ProduceTo(M + 8, m_vk_a);
    const auto set0{*FinalityState().current};
    Produce(m_vk_a, {MakeCertificate({M + 5, 0, FinalityState().next->SetHash()}, set0)});
    const auto status{m_node.chain->finalityStatus(std::nullopt)};
    BOOST_REQUIRE(status.finalized_height.has_value());
    BOOST_CHECK_EQUAL(*status.finalized_height, M + 5);
    BOOST_REQUIRE(status.pin_height.has_value());
    BOOST_CHECK_EQUAL(*status.pin_height, M + 5);
    BOOST_CHECK(status.handover_certified);
}

BOOST_AUTO_TEST_SUITE_END()
