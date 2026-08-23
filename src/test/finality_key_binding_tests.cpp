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

namespace {

struct BindingFixture : public ModernPosSetup {
    Txid m_fund_txid{};
    CAmount m_fund_value{0};
    uint256 m_domain{};
    CKey m_validator_a, m_validator_b;
    modern::ValidatorKeyBytes m_vk_a{}, m_vk_b{};

    BindingFixture()
    {
        m_validator_a = MakeValidatorKey(0x11);
        m_validator_b = MakeValidatorKey(0x22);
        m_vk_a = XOnly(m_validator_a);
        m_vk_b = XOnly(m_validator_b);
    }

    //! Legacy prefix with a funding output at H; corridor configured; test contexts on.
    void Prepare()
    {
        const Consensus::Params& consensus{m_node.chainman->GetConsensus()};
        Txid coinbase1{};
        for (int height{1}; height <= SYN_H - 1; ++height) {
            const CBlock block{BuildLegacy(Tip(), {})};
            BOOST_REQUIRE(Submit(block));
            if (height == 1) coinbase1 = block.vtx[0]->GetHash();
        }
        CMutableTransaction fund;
        fund.version = 1;
        fund.nTime = static_cast<uint32_t>(Tip()->GetBlockTime() + 17);
        fund.vin.resize(1);
        fund.vin[0].prevout = COutPoint{coinbase1, 0};
        m_fund_value = legacy::GetProofOfWorkReward(0, 1, consensus);
        // several spendable outputs for several blocks
        for (int i = 0; i < 6; ++i) fund.vout.emplace_back(m_fund_value / 8, CScript() << OP_TRUE);
        const CBlock block_h{BuildLegacy(Tip(), {fund})};
        m_fund_txid = block_h.vtx[1]->GetHash();
        BOOST_REQUIRE(Submit(block_h));
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, SYN_H);
        ConfigureCorridor(Tip()->GetBlockHash());
        MutableConsensus().test_only_metadata_cells_active = true;
        MutableConsensus().test_only_mpa_active = true;
        m_domain = Domain();
    }

    static bls::SecretKey Bls(const unsigned i)
    {
        std::array<unsigned char, 32> ikm{};
        ikm[0] = static_cast<unsigned char>(i);
        ikm[31] = 0x99;
        return *bls::SecretKey::FromIKM(ikm);
    }

    struct CellAndEvidence {
        CScript cell;
        CMpaRecord record;
    };

    //! Build a FINALITY_KEY cell and its evidence. bls == nullptr -> revocation.
    CellAndEvidence MakeBinding(const CKey& identity, const modern::ValidatorKeyBytes& vk, const bls::SecretKey* bls,
                                const uint32_t seq, const CKey* signer = nullptr, const uint256* domain = nullptr)
    {
        FinalityKeyParams params;
        params.bls_pubkey = bls ? bls->GetPublicKey().Compressed() : modern::BlsPubkeyBytes{};
        params.seq = seq;
        uint256 commitment;
        std::copy(vk.begin(), vk.end(), commitment.begin());
        const auto script{modern::MakeMetadataCellScript(static_cast<uint16_t>(modern::PolicyType::FINALITY_KEY),
                                                          modern::POLICY_VERSION_V1, commitment, params.Encode())};
        BOOST_REQUIRE(script.has_value());
        FinalityKeyEvidence ev;
        ev.validator_key = vk;
        ev.bls_pubkey = params.bls_pubkey;
        ev.seq = seq;
        const uint256 digest{modern::FinalityBindDigest(domain ? *domain : m_domain, ev.validator_key, ev.bls_pubkey, seq)};
        uint256 aux{};
        BOOST_REQUIRE((signer ? *signer : identity).SignSchnorr(digest, ev.bip340_sig, nullptr, aux));
        if (bls) ev.pop = bls->SignPoP().Compressed();
        CMpaRecord rec;
        rec.payload_type = modern::MPA_TYPE_FINALITY_KEY_EVIDENCE;
        rec.payload_version = modern::MPA_VERSION_V1;
        const auto enc{ev.Encode()};
        rec.payload.assign(enc.begin(), enc.end());
        return {*script, rec};
    }

    //! A transaction spending fund output `n`, carrying the given cells and records.
    CMutableTransaction MakeTx(const unsigned n, const std::vector<CScript>& cells, std::vector<CMpaRecord> records)
    {
        CMutableTransaction tx;
        tx.version = 2;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint{m_fund_txid, n};
        tx.vout.emplace_back(m_fund_value / 8 - 100, CScript() << OP_TRUE);
        for (const auto& c : cells) tx.vout.emplace_back(0, c);
        std::sort(records.begin(), records.end(), modern::MpaRecordLess); // canonical order
        tx.mpa = std::move(records);
        return tx;
    }

    const FinalityBindingIndex& Index()
    {
        LOCK(cs_main);
        FinalityBindingTracker& t{m_node.chainman->ActiveChainstate().ModernFinalityBindings()};
        BOOST_REQUIRE(t.Sync(m_node.chainman->ActiveChain(), m_node.chainman->m_blockman,
                             m_node.chainman->GetConsensus(), *Tip()));
        return t.Index();
    }

    bool SubmitCorridor(const std::vector<CMutableTransaction>& txs, const int64_t time_delta = 60)
    {
        return Submit(BuildCorridor(Tip(), txs, time_delta));
    }
};

} // namespace

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
    // by using another key's PoP): txid identical.
    CMutableTransaction ta{MakeTx(0, {e1.cell}, {e1.record})};
    CMutableTransaction tb{MakeTx(0, {e2.cell}, {e2.record})};
    FinalityKeyEvidence ev{*FinalityKeyEvidence::Decode(tb.mpa[0].payload)};
    ev.pop = k2.SignPoP().Compressed();
    const auto enc{ev.Encode()};
    tb.mpa[0].payload.assign(enc.begin(), enc.end());
    BOOST_CHECK(CTransaction{ta}.GetHash() == CTransaction{tb}.GetHash());
    BOOST_CHECK(CTransaction{ta}.GetWitnessHash() == CTransaction{tb}.GetWitnessHash());
    BOOST_CHECK(!(ta.mpa == tb.mpa));
    // Production: the same block shape is refused when the test contexts are off.
    MutableConsensus().test_only_mpa_active = false;
    BOOST_CHECK(!SubmitCorridor({ta}));
    MutableConsensus().test_only_mpa_active = true;
    MutableConsensus().test_only_metadata_cells_active = false;
    BOOST_CHECK(!SubmitCorridor({ta}, 61));
    BOOST_CHECK_EQUAL(Tip()->nHeight, SYN_H);
    for (const auto chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::REGTEST}) {
        const auto& c{CreateChainParams(ArgsManager{}, chain)->GetConsensus()};
        BOOST_CHECK(!c.test_only_mpa_active && !c.test_only_metadata_cells_active);
    }
    // Legacy era: a legacy block cannot carry an MPA at all (codec), and the
    // binding machinery never runs below H+1 (the era gate in ConnectBlock).
    BOOST_CHECK(Consensus::GetB3Era(SYN_H, m_node.chainman->GetConsensus()) == Consensus::B3Era::LEGACY);
}

BOOST_AUTO_TEST_SUITE_END()
