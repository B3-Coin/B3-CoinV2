// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_TEST_UTIL_FINALITY_FIXTURE_H
#define B3COIN_TEST_UTIL_FINALITY_FIXTURE_H

//! Shared fixture for FINALITY_KEY / MPA / payload-root block-level tests:
//! the synthetic chain to H with a multi-output funding transaction, the
//! corridor configured, both test-only activation contexts on, helpers to
//! build cell+evidence pairs, MPA-bearing transactions and corridor blocks
//! carrying the MODERN_PAYLOAD_ROOT cell. Extracted from
//! finality_key_binding_tests.cpp (plan Commit 7).

#include <chainparams.h>
#include <consensus/params.h>
#include <crypto/bls.h>
#include <key.h>
#include <modern/chain_domain.h>
#include <modern/finality_key.h>
#include <modern/finality_types.h>
#include <modern/metadata_cell.h>
#include <modern/mpa.h>
#include <modern/payload_root.h>
#include <node/finality_binding_index.h>
#include <primitives/transaction.h>
#include <uint256.h>
#include <validation.h>

#include <test/util/modern_pos_setup.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

namespace b3test {

using modern::FinalityKeyEvidence;
using modern::FinalityKeyParams;
using node::FinalityBindingIndex;
using node::FinalityBindingTracker;

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

    //! A corridor block carrying `txs`; when any of them has an MPA the coinbase
    //! gets the MODERN_PAYLOAD_ROOT cell (Commit 7). The root depends only on the
    //! MPA sections and positions, so it is computed on a probe block first.
    CBlock BuildCorridorWithRoot(const std::vector<CMutableTransaction>& txs, const int64_t time_delta = 60)
    {
        CBlock probe{BuildCorridor(Tip(), txs, time_delta)};
        if (!modern::BlockHasAnyMpa(probe)) return probe;
        const uint256 root{modern::ComputePayloadRoot(probe)};
        return BuildCorridor(Tip(), txs, time_delta, EASY_BITS, {CTxOut{0, modern::MakePayloadRootCellScript(root)}});
    }
    bool SubmitCorridor(const std::vector<CMutableTransaction>& txs, const int64_t time_delta = 60)
    {
        return Submit(BuildCorridorWithRoot(txs, time_delta));
    }
};


} // namespace b3test

#endif // B3COIN_TEST_UTIL_FINALITY_FIXTURE_H
