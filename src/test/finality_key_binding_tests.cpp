// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 5 of the Modern PoS V1 finality plan: FINALITY_KEY cell <-> MPA
// evidence binding at block level on the synthetic B3 chain — valid
// bind/rotate/revoke, every binding failure, no partial index mutation,
// Connect/Disconnect/rebuild exactness, txid invariance, and production
// fail-closed behaviour.

#include <chainparams.h>
#include <consensus/params.h>
#include <crypto/bls.h>
#include <key.h>
#include <modern/chain_domain.h>
#include <modern/finality_key.h>
#include <modern/finality_types.h>
#include <modern/metadata_cell.h>
#include <modern/mpa.h>
#include <node/finality_binding_index.h>
#include <primitives/transaction.h>
#include <uint256.h>
#include <validation.h>

#include <test/util/finality_fixture.h>
#include <test/util/modern_pos_setup.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <optional>
#include <vector>

using namespace b3test;
using modern::FinalityKeyEvidence;
using modern::FinalityKeyParams;
using node::FinalityBindingIndex;
using node::FinalityBindingTracker;


BOOST_FIXTURE_TEST_SUITE(finality_key_binding_tests, BasicTestingSetup)

BOOST_FIXTURE_TEST_CASE(valid_bind_rotate_revoke_and_index, BindingFixture)
{
    Prepare();
    const auto k1{Bls(1)}, k2{Bls(2)};
    // bind (seq 0)
    const auto b0{MakeBinding(m_validator_a, m_vk_a, &k1, 0)};
    const CMutableTransaction tx0{MakeTx(0, {b0.cell}, {b0.record})};
    const Txid txid0{CTransaction{tx0}.GetHash()};
    BOOST_REQUIRE(SubmitCorridor({tx0}));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, SYN_H + 1);
    {
        const auto& index{Index()};
        const auto rec{index.Get(m_vk_a)};
        BOOST_REQUIRE(rec.has_value());
        BOOST_CHECK(rec->bls_pubkey == k1.GetPublicKey().Compressed());
        BOOST_CHECK_EQUAL(rec->seq, 0u);
        BOOST_CHECK_EQUAL(rec->height, SYN_H + 1);
        BOOST_CHECK(index.OwnerOf(k1.GetPublicKey().Compressed()) == std::optional{m_vk_a});
        LOCK(cs_main);
        CCoinsViewCache& view{m_node.chainman->ActiveChainstate().CoinsTip()};
        BOOST_CHECK(view.HaveCoin(COutPoint{txid0, 0}));   // ordinary output
        BOOST_CHECK(!view.HaveCoin(COutPoint{txid0, 1}));  // the cell: never a coin
    }
    // rotate (seq 1)
    const auto b1{MakeBinding(m_validator_a, m_vk_a, &k2, 1)};
    BOOST_REQUIRE(SubmitCorridor({MakeTx(1, {b1.cell}, {b1.record})}));
    {
        const auto& index{Index()};
        BOOST_CHECK(index.Get(m_vk_a)->bls_pubkey == k2.GetPublicKey().Compressed());
        BOOST_CHECK(!index.OwnerOf(k1.GetPublicKey().Compressed()).has_value());
    }
    // revoke (seq 2): zero key, zero PoP
    const auto r{MakeBinding(m_validator_a, m_vk_a, nullptr, 2)};
    BOOST_REQUIRE(SubmitCorridor({MakeTx(2, {r.cell}, {r.record})}));
    {
        const auto& index{Index()};
        BOOST_CHECK(index.Get(m_vk_a)->IsRevoked());
        BOOST_CHECK_EQUAL(index.Get(m_vk_a)->seq, 2u);
        BOOST_CHECK(index.SnapshotActive().empty());
    }
    // a second validator binds in the same block as the first re-binds (two txs, in-block overlay)
    const auto a3{MakeBinding(m_validator_a, m_vk_a, &k1, 3)};
    const auto bb{MakeBinding(m_validator_b, m_vk_b, &k2, 0)}; // k2 was released by a's revocation
    BOOST_REQUIRE(SubmitCorridor({MakeTx(3, {a3.cell}, {a3.record}), MakeTx(4, {bb.cell}, {bb.record})}));
    {
        const auto& index{Index()};
        BOOST_CHECK(index.OwnerOf(k1.GetPublicKey().Compressed()) == std::optional{m_vk_a});
        BOOST_CHECK(index.OwnerOf(k2.GetPublicKey().Compressed()) == std::optional{m_vk_b});
        BOOST_CHECK_EQUAL(index.SnapshotActive().size(), 2u);
    }
}

BOOST_FIXTURE_TEST_CASE(binding_failures_are_rejected_without_index_mutation, BindingFixture)
{
    Prepare();
    const auto k1{Bls(1)}, k2{Bls(2)};
    const auto good{MakeBinding(m_validator_a, m_vk_a, &k1, 0)};
    // Baseline index state: empty.
    BOOST_CHECK_EQUAL(Index().Size(), 0u);
    int64_t delta{60};
    // Structural failures are refused before storage (Submit false); semantic
    // failures are refused at connect time (the block is stored and marked
    // failed, Submit may return true) — in every case the tip must not move
    // and the binding index must stay untouched.
    auto expect_reject = [&](const std::vector<CMutableTransaction>& txs) {
        const int before{Tip()->nHeight};
        (void)SubmitCorridor(txs, delta++); // distinct timestamps -> distinct blocks
        BOOST_CHECK_EQUAL(Tip()->nHeight, before);
        BOOST_CHECK_EQUAL(Index().Size(), 0u); // no partial mutation
    };
    // cell without evidence
    expect_reject({MakeTx(0, {good.cell}, {})});
    // evidence without cell (orphan)
    expect_reject({MakeTx(0, {}, {good.record})});
    // duplicate evidence
    expect_reject({MakeTx(0, {good.cell}, {good.record, good.record})});
    // duplicate cells competing for one evidence
    expect_reject({MakeTx(0, {good.cell, good.cell}, {good.record})});
    // field mismatch: cell for seq 0, evidence for seq 1
    {
        const auto other{MakeBinding(m_validator_a, m_vk_a, &k1, 1)};
        expect_reject({MakeTx(0, {good.cell}, {other.record})});
    }
    // wrong BIP340 signer
    {
        const auto bad{MakeBinding(m_validator_a, m_vk_a, &k1, 0, &m_validator_b)};
        expect_reject({MakeTx(0, {bad.cell}, {bad.record})});
    }
    // wrong domain
    {
        const uint256 other_domain{uint256{"abababababababababababababababababababababababababababababababab"}};
        const auto bad{MakeBinding(m_validator_a, m_vk_a, &k1, 0, nullptr, &other_domain)};
        expect_reject({MakeTx(0, {bad.cell}, {bad.record})});
    }
    // cross-key PoP
    {
        auto bad{MakeBinding(m_validator_a, m_vk_a, &k1, 0)};
        FinalityKeyEvidence ev{*FinalityKeyEvidence::Decode(bad.record.payload)};
        ev.pop = k2.SignPoP().Compressed();
        const auto enc{ev.Encode()};
        bad.record.payload.assign(enc.begin(), enc.end());
        expect_reject({MakeTx(0, {bad.cell}, {bad.record})});
    }
    // nonzero key with zero PoP
    {
        auto bad{MakeBinding(m_validator_a, m_vk_a, &k1, 0)};
        FinalityKeyEvidence ev{*FinalityKeyEvidence::Decode(bad.record.payload)};
        ev.pop.fill(0);
        const auto enc{ev.Encode()};
        bad.record.payload.assign(enc.begin(), enc.end());
        expect_reject({MakeTx(0, {bad.cell}, {bad.record})});
    }
    // revocation with a nonzero PoP (first transition may be a revocation at seq 0)
    {
        auto bad{MakeBinding(m_validator_a, m_vk_a, nullptr, 0)};
        FinalityKeyEvidence ev{*FinalityKeyEvidence::Decode(bad.record.payload)};
        ev.pop = k1.SignPoP().Compressed();
        const auto enc{ev.Encode()};
        bad.record.payload.assign(enc.begin(), enc.end());
        expect_reject({MakeTx(0, {bad.cell}, {bad.record})});
    }
    // wrong first seq
    {
        const auto bad{MakeBinding(m_validator_a, m_vk_a, &k1, 1)};
        expect_reject({MakeTx(0, {bad.cell}, {bad.record})});
    }
    // one valid pair + one invalid pair in the same block: whole block rejected, nothing applied
    {
        const auto bad{MakeBinding(m_validator_b, m_vk_b, &k2, 5)}; // bad first seq
        expect_reject({MakeTx(0, {good.cell}, {good.record}), MakeTx(1, {bad.cell}, {bad.record})});
    }
    // duplicate BLS key across validators in one block: second pair rejected -> block rejected
    {
        const auto dup{MakeBinding(m_validator_b, m_vk_b, &k1, 0)};
        expect_reject({MakeTx(0, {good.cell}, {good.record}), MakeTx(1, {dup.cell}, {dup.record})});
    }
    // inactive MPA type inside an otherwise valid transaction
    {
        auto tx{MakeTx(0, {good.cell}, {good.record})};
        CMpaRecord inactive;
        inactive.payload_type = 4; inactive.payload_version = 1; inactive.payload.assign(10, 0);
        tx.mpa.push_back(inactive);
        std::sort(tx.mpa.begin(), tx.mpa.end(), modern::MpaRecordLess);
        expect_reject({tx});
    }
    // Finally the good pair connects, proving the chain/fixture were fine all along.
    BOOST_REQUIRE(SubmitCorridor({MakeTx(0, {good.cell}, {good.record})}, delta));
    BOOST_CHECK_EQUAL(Index().Size(), 1u);
    // ...and a duplicate BLS key across validators in a LATER block is rejected too.
    const auto dup2{MakeBinding(m_validator_b, m_vk_b, &k1, 0)};
    const int height_before{Tip()->nHeight};
    (void)SubmitCorridor({MakeTx(1, {dup2.cell}, {dup2.record})}, delta + 1);
    BOOST_CHECK_EQUAL(Tip()->nHeight, height_before);
    BOOST_CHECK_EQUAL(Index().Size(), 1u);
}

BOOST_FIXTURE_TEST_CASE(connect_disconnect_rebuild_exactness, BindingFixture)
{
    Prepare();
    const auto k1{Bls(1)}, k2{Bls(2)};
    const auto b0{MakeBinding(m_validator_a, m_vk_a, &k1, 0)};
    const auto b1{MakeBinding(m_validator_a, m_vk_a, &k2, 1)};
    const auto bb{MakeBinding(m_validator_b, m_vk_b, &k1, 0)}; // takes k1 after a moved off it
    BOOST_REQUIRE(SubmitCorridor({MakeTx(0, {b0.cell}, {b0.record})}));
    const uint256 h1{Tip()->GetBlockHash()};
    BOOST_REQUIRE(SubmitCorridor({MakeTx(1, {b1.cell}, {b1.record}), MakeTx(2, {bb.cell}, {bb.record})}));
    const uint256 h2{Tip()->GetBlockHash()};
    const auto state_after_2{Index().All()};
    BOOST_CHECK_EQUAL(state_after_2.size(), 2u);
    BOOST_CHECK(Index().OwnerOf(k1.GetPublicKey().Compressed()) == std::optional{m_vk_b});
    // Disconnect the top block: exact prior state (a bound to k1, b absent).
    {
        BlockValidationState state;
        CBlockIndex* idx{WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(h2))};
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, idx));
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
    }
    BOOST_REQUIRE(Tip()->GetBlockHash() == h1);
    {
        const auto& index{Index()};
        BOOST_CHECK_EQUAL(index.Size(), 1u);
        BOOST_CHECK(index.Get(m_vk_a)->bls_pubkey == k1.GetPublicKey().Compressed());
        BOOST_CHECK(index.OwnerOf(k1.GetPublicKey().Compressed()) == std::optional{m_vk_a});
        BOOST_CHECK(!index.Get(m_vk_b).has_value());
    }
    // Reconsider: identical state again.
    {
        CBlockIndex* idx{WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(h2))};
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().ResetBlockFailureFlags(idx);
        m_node.chainman->RecalculateBestHeader();
    }
    {
        BlockValidationState state;
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
    }
    BOOST_REQUIRE(Tip()->GetBlockHash() == h2);
    BOOST_CHECK(Index().All() == state_after_2);
    // Rebuild from disk (fresh tracker) equals the incremental state.
    {
        LOCK(cs_main);
        FinalityBindingTracker fresh;
        BOOST_REQUIRE(fresh.Sync(m_node.chainman->ActiveChain(), m_node.chainman->m_blockman,
                                 m_node.chainman->GetConsensus(), *Tip()));
        BOOST_CHECK(fresh.Index().All() == state_after_2);
        for (const auto& pk : {k1.GetPublicKey().Compressed(), k2.GetPublicKey().Compressed()}) {
            BOOST_CHECK(fresh.Index().OwnerOf(pk) == Index().OwnerOf(pk));
        }
    }
}

BOOST_FIXTURE_TEST_CASE(txid_unchanged_and_production_fail_closed, BindingFixture)
{
    Prepare();
    const auto k1{Bls(1)}, k2{Bls(2)};
    const auto e1{MakeBinding(m_validator_a, m_vk_a, &k1, 0)};
    const auto e2{MakeBinding(m_validator_a, m_vk_a, &k1, 0)};
    // Same base transaction, different evidence bytes (a second signature with the
    // same inputs is identical under BIP340 with zero aux... so perturb the PoP
    // by using another key's PoP): txid stays stable, while the full relay
    // identities commit to the changed MPA payload.
    CMutableTransaction ta{MakeTx(0, {e1.cell}, {e1.record})};
    CMutableTransaction tb{MakeTx(0, {e2.cell}, {e2.record})};
    FinalityKeyEvidence ev{*FinalityKeyEvidence::Decode(tb.mpa[0].payload)};
    ev.pop = k2.SignPoP().Compressed();
    const auto enc{ev.Encode()};
    tb.mpa[0].payload.assign(enc.begin(), enc.end());
    const CTransaction tx_a{ta};
    const CTransaction tx_b{tb};
    BOOST_CHECK(tx_a.GetHash() == tx_b.GetHash());
    BOOST_CHECK(tx_a.GetWitnessHash() != tx_b.GetWitnessHash());
    BOOST_CHECK(!(tx_a.GetPtxid() == tx_b.GetPtxid()));
    BOOST_CHECK(!(ta.mpa == tb.mpa));
    // Production: the same block shape is refused while the X-pin
    // configuration is incomplete (the F = M activation predicate is off).
    const auto saved_pos{MutableConsensus().modern_pos};
    MutableConsensus().modern_pos.reset();
    BOOST_CHECK(!SubmitCorridor({ta}));
    BOOST_CHECK(!SubmitCorridor({ta}, 61));
    MutableConsensus().modern_pos = saved_pos;
    BOOST_CHECK_EQUAL(Tip()->nHeight, SYN_H);
    BOOST_CHECK(Consensus::ModernObjectRulesActive(
        CreateChainParams(ArgsManager{}, ChainType::MAIN)->GetConsensus()));
    for (const auto chain : {ChainType::TESTNET, ChainType::TESTNET4,
                             ChainType::SIGNET, ChainType::REGTEST}) {
        const auto chain_params{CreateChainParams(ArgsManager{}, chain)};
        const auto& c{chain_params->GetConsensus()};
        BOOST_CHECK(!Consensus::ModernObjectRulesActive(c));
    }
    // Legacy era: a legacy block cannot carry an MPA at all (codec), and the
    // binding machinery never runs below H+1 (the era gate in ConnectBlock).
    BOOST_CHECK(Consensus::GetB3Era(SYN_H, m_node.chainman->GetConsensus()) == Consensus::B3Era::LEGACY);
}

BOOST_AUTO_TEST_SUITE_END()
