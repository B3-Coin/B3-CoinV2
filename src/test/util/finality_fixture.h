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
#include <node/finality_tracker.h>
#include <node/validator_set.h>
#include <primitives/transaction.h>
#include <uint256.h>
#include <util/time.h>
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

    explicit BindingFixture(TestOpts opts = {}) : ModernPosSetup{std::move(opts)}
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
        for (int i = 0; i < 12; ++i) fund.vout.emplace_back(m_fund_value / 16, CScript() << OP_TRUE);
        const CBlock block_h{BuildLegacy(Tip(), {fund})};
        m_fund_txid = block_h.vtx[1]->GetHash();
        BOOST_REQUIRE(Submit(block_h));
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, SYN_H);
        ConfigureCorridor(Tip()->GetBlockHash());
        // The X-pin configuration (H + X + Modern-PoS rule set) IS the
        // activation switch for cells and MPA (F = M plumbing). Scaffolding
        // values; finality-chain tests override them in PrepareFinalityChain.
        Consensus::ModernPosParams pos{};
        pos.reorg_horizon = 200;
        pos.min_finality_set = 1;
        MutableConsensus().modern_pos = pos;
        m_domain = Domain();
    }

    CMutableTransaction MakeTx(const unsigned n, const std::vector<CScript>& cells, std::vector<CMpaRecord> records)
    {
        CMutableTransaction tx;
        tx.version = 2;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint{m_fund_txid, n};
        tx.vout.emplace_back(m_fund_value / 16 - 100, CScript() << OP_TRUE);
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


/**
 * A modern-PoS chain with the finality gadget live (plan Commits 12-14):
 * legacy history to H, corridor with two bound, staked validators (A heavy,
 * B light; stakes in whole modern B3 so both have snapshot weight), scaled
 * finality constants, and helpers to produce modern-PoS blocks that may carry
 * a FINALITY_CERT cell + certificate record in their coinbase.
 *
 * Scaffolding constants (fixtures may scale exactly like reorg_horizon):
 *   E = 30, CHECKPOINT_INTERVAL = 5, CHECKPOINT_DEPTH = 3,
 *   MAX_EPOCH_EXTENSION = 30, MIN_FINALITY_SET = 1 (two validators).
 */
struct FinalityChainFixture : public BindingFixture {
    static constexpr int SCALED_E{30};
    static constexpr int SCALED_INTERVAL{5};
    static constexpr int SCALED_DEPTH{3};
    static constexpr int SCALED_MAX_EXTENSION{30};
    static constexpr CAmount STAKE_HEAVY{15 * modern::FINALITY_WEIGHT_UNIT}; // weight 15
    static constexpr CAmount STAKE_LIGHT{1 * modern::FINALITY_WEIGHT_UNIT};  // weight 1
    static constexpr CAmount STAKE_C{10 * modern::FINALITY_WEIGHT_UNIT};     // weight 10 (optional third validator)

    int m_M{0};
    //! Optional third validator C: staked in the corridor but NOT bound
    //! (Commit 14 eligibility tests bind it later).
    CKey m_validator_c{MakeValidatorKey(0x33)};
    modern::ValidatorKeyBytes m_vk_c{XOnly(m_validator_c)};
    bls::SecretKey m_bls_c{Bls(3)};

    explicit FinalityChainFixture(TestOpts opts = {}) : BindingFixture{std::move(opts)} {}

    const CBlockIndex* IndexAt(const int height)
    {
        LOCK(cs_main);
        const CBlockIndex* index{m_node.chainman->ActiveChain()[height]};
        BOOST_REQUIRE(index != nullptr);
        return index;
    }
    uint256 ChainHashAt(const int height) { return IndexAt(height)->GetBlockHash(); }

    //! Legacy -> corridor (stakes + bindings in the first corridor block) ->
    //! last corridor height M-1, with the modern-PoS rule set configured.
    void PrepareFinalityChain(const int min_finality_set = 1, const int reorg_horizon = 200,
                              const bool with_unbound_c = false, const bool with_bound_c = false)
    {
        Prepare();
        Consensus::Params& c{MutableConsensus()};
        Consensus::ModernPosParams pos{};
        pos.reorg_horizon = reorg_horizon;
        pos.finality_epoch_blocks = SCALED_E;
        pos.checkpoint_interval = SCALED_INTERVAL;
        pos.checkpoint_depth = SCALED_DEPTH;
        pos.max_epoch_extension = SCALED_MAX_EXTENSION;
        pos.min_finality_set = min_finality_set;
        BOOST_REQUIRE(pos.Valid());
        c.modern_pos = pos;
        {
            LOCK(cs_main);
            Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
            chainstate.setBlockIndexCandidates.clear();
            chainstate.PopulateBlockIndexCandidates();
        }
        m_M = *Consensus::ModernPosStartHeight(c);

        // Corridor block 1: STAKE outputs and FINALITY_KEY bindings for A and B.
        CMutableTransaction stake_a{MakeTx(0, {}, {})};
        stake_a.vout.insert(stake_a.vout.begin(), CTxOut{STAKE_HEAVY, modern::MakeStakeScript(m_vk_a, CScript() << OP_TRUE)});
        stake_a.vout.back().nValue -= STAKE_HEAVY;
        CMutableTransaction stake_b{MakeTx(1, {}, {})};
        stake_b.vout.insert(stake_b.vout.begin(), CTxOut{STAKE_LIGHT, modern::MakeStakeScript(m_vk_b, CScript() << OP_TRUE)});
        stake_b.vout.back().nValue -= STAKE_LIGHT;
        const auto bind_a{MakeBinding(m_validator_a, m_vk_a, &m_bls_a, 0)};
        const auto bind_b{MakeBinding(m_validator_b, m_vk_b, &m_bls_b, 0)};
        {
            std::vector<CMutableTransaction> txs{stake_a, stake_b, MakeTx(2, {bind_a.cell}, {bind_a.record}),
                                                 MakeTx(3, {bind_b.cell}, {bind_b.record})};
            if (with_unbound_c || with_bound_c) {
                CMutableTransaction stake_c{MakeTx(6, {}, {})};
                stake_c.vout.insert(stake_c.vout.begin(), CTxOut{STAKE_C, modern::MakeStakeScript(m_vk_c, CScript() << OP_TRUE)});
                stake_c.vout.back().nValue -= STAKE_C;
                txs.push_back(stake_c);
            }
            if (with_bound_c) {
                const auto bind_c{MakeBinding(m_validator_c, m_vk_c, &m_bls_c, 0)};
                txs.push_back(MakeTx(11, {bind_c.cell}, {bind_c.record}));
            }
            const CBlock first{BuildCorridorWithRoot(txs)};
            BOOST_REQUIRE_MESSAGE(Probe(first).empty(), "first corridor block invalid: " << Probe(first));
            BOOST_REQUIRE(Submit(first));
            BOOST_REQUIRE_EQUAL(Tip()->nHeight, SYN_H + 1);
        }
        for (int i{1}; i < SYN_CORRIDOR; ++i) BOOST_REQUIRE(SubmitCorridor({}));
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, m_M - 1);
    }

    //! Full validity probe of a block on the tip (empty string = valid).
    std::string Probe(const CBlock& block)
    {
        LOCK(cs_main);
        const BlockValidationState state{TestBlockValidity(m_node.chainman->ActiveChainstate(), block,
                                                           /*check_pow=*/false, /*check_merkle_root=*/true)};
        return state.IsValid() ? std::string{} : state.ToString();
    }

    node::FinalityTracker& Finality()
    {
        AssertLockHeld(cs_main);
        node::FinalityTracker& t{m_node.chainman->ActiveChainstate().ModernFinality()};
        BOOST_REQUIRE(t.Sync(m_node.chainman->ActiveChain(), m_node.chainman->m_blockman,
                             m_node.chainman->GetConsensus(), *m_node.chainman->ActiveChain().Tip()));
        return t;
    }
    node::FinalityTracker::State FinalityState()
    {
        LOCK(cs_main);
        return Finality().Current();
    }
    //! The epoch state projected for the next block (tip + 1).
    node::FinalityTracker::State ProjectedNext()
    {
        LOCK(cs_main);
        return Finality().Projected(Tip()->nHeight + 1, m_node.chainman->GetConsensus());
    }

    struct CertSpec {
        int checkpoint_height;
        uint64_t epoch;
        uint256 successor_hash;
        //! Which validators sign (default all members).
        bool sign_a{true};
        bool sign_b{true};
        bool sign_c{true};
    };
    //! Build cell + record for a certificate over `snapshot` (the signing set).
    std::pair<CScript, CMpaRecord> MakeCertificate(const CertSpec& spec, const node::ValidatorSetSnapshot& signing_set,
                                                   const std::optional<uint256>& block_hash = std::nullopt)
    {
        modern::FinalizedBlock fb;
        fb.height = static_cast<uint64_t>(spec.checkpoint_height);
        fb.block_hash = block_hash ? *block_hash
                                   : WITH_LOCK(cs_main, return m_node.chainman->ActiveChain()[spec.checkpoint_height]->GetBlockHash());
        fb.withdrawal_root = uint256{};
        fb.validator_set_hash = spec.successor_hash;
        fb.epoch = spec.epoch;
        modern::FinalityCertificate cert;
        cert.signer_bitmap.assign(modern::SignerBitmapBytes(signing_set.Size()), 0);
        const uint256 digest{modern::FinalityDigest(m_domain, fb)};
        std::vector<bls::Signature> sigs;
        for (uint32_t i = 0; i < signing_set.Size(); ++i) {
            const auto& pk{signing_set.Members()[i].bls_pubkey};
            const bls::SecretKey* sk{nullptr};
            if (pk == m_bls_a.GetPublicKey().Compressed() && spec.sign_a) sk = &m_bls_a;
            if (pk == m_bls_b.GetPublicKey().Compressed() && spec.sign_b) sk = &m_bls_b;
            if (pk == m_bls_c.GetPublicKey().Compressed() && spec.sign_c) sk = &m_bls_c;
            if (!sk) continue;
            cert.signer_bitmap[i / 8] |= static_cast<unsigned char>(1u << (i % 8));
            sigs.push_back(sk->Sign(std::span<const unsigned char>(digest.begin(), 32)));
        }
        BOOST_REQUIRE(!sigs.empty());
        cert.aggregate_sig = bls::AggregateSignatures(sigs)->Compressed();
        const auto [payload, cell] = modern::BuildFinalityCertificate(fb, cert);
        CMpaRecord rec;
        rec.payload_type = modern::MPA_TYPE_FINALITY_CERTIFICATE;
        rec.payload_version = modern::MPA_VERSION_V1;
        rec.payload = payload;
        return {cell, rec};
    }

    //! Static fallback weights (whole B3) for the round search when the
    //! consensus weights are unavailable (a side-branch parent that was never
    //! connected): the default two-member set {A: 15, B: 1}.
    CAmount WeightOf(const modern::PosValidatorKey& key) const { return key == m_vk_a ? 15 : key == m_vk_c ? 10 : 1; }
    //! The (w, W) the validation rule will apply to the child of `prev`:
    //! consensus-derived when `prev` is on the active chain, static otherwise.
    std::pair<CAmount, CAmount> RoundWeights(const CBlockIndex* prev, const modern::PosValidatorKey& key)
    {
        LOCK(cs_main);
        Chainstate& cs{m_node.chainman->ActiveChainstate()};
        if (cs.m_chain.Contains(prev)) {
            if (const auto w{cs.ModernEligibilityWeights(key, *prev)}) {
                if (w->second > 0) return *w;
            }
        }
        return {WeightOf(key), 16};
    }

    //! A modern-PoS block by `key` on `prev` whose height is governed by
    //! `seed` (the parent's eligibility digest -- for a side branch whose
    //! parent was never connected the caller threads it), with optional
    //! extra coinbase cells/records (certificate) and extra transactions;
    //! the payload-root cell is appended automatically when the block
    //! carries any MPA. Returns the block and its own digest (= the next
    //! height's seed).
    std::pair<CBlock, uint256> BuildPosBlockOnSeed(const CBlockIndex* prev, const uint256& seed,
                                                   const modern::PosValidatorKey& key,
                                                   const std::vector<std::pair<CScript, CMpaRecord>>& coinbase_payload = {},
                                                   std::vector<CMutableTransaction> txs = {}, const int extra = 0)
    {
        const Consensus::ModernPosParams& pos{*m_node.chainman->GetConsensus().modern_pos};
        // For an ineligible key (w = 0) search as if it had the static weight:
        // the block is built so the rule can refuse it.
        auto [w, W] = RoundWeights(prev, key);
        if (w <= 0) { w = WeightOf(key); W = std::max<CAmount>(W, w + 1); }
        int64_t round{-1};
        uint256 digest;
        for (int64_t r{0}; r < 100'000 && round < 0; ++r) {
            const uint256 d{modern::ModernPosEligibilityDigest(Domain(), seed, static_cast<uint32_t>(prev->nHeight + 1),
                                                               static_cast<uint32_t>(r), key)};
            if (modern::ModernPosEligible(d, w, W, r, pos)) { round = r; digest = d; }
        }
        BOOST_REQUIRE(round >= 0);
        return {FinishPosBlock(BuildPos(prev, key, round, 0, extra, std::move(txs)), key, coinbase_payload), digest};
    }
    CBlock BuildPosBlock(const modern::PosValidatorKey& key,
                         const std::vector<std::pair<CScript, CMpaRecord>>& coinbase_payload = {},
                         std::vector<CMutableTransaction> txs = {}, const int extra = 0)
    {
        const CBlockIndex* prev{Tip()};
        return BuildPosBlockOnSeed(prev, SeedFor(prev), key, coinbase_payload, std::move(txs), extra).first;
    }
    //! Append the coinbase payload (+ root cell), recompute the merkle root, sign.
    CBlock FinishPosBlock(CBlock block, const modern::PosValidatorKey& key,
                          const std::vector<std::pair<CScript, CMpaRecord>>& coinbase_payload)
    {
        CMutableTransaction coinbase{*block.vtx[0]};
        std::vector<CMpaRecord> records;
        for (const auto& [cell, rec] : coinbase_payload) {
            coinbase.vout.emplace_back(0, cell);
            records.push_back(rec);
        }
        std::sort(records.begin(), records.end(), modern::MpaRecordLess);
        coinbase.mpa = records;
        block.vtx[0] = MakeTransactionRef(coinbase);
        if (modern::BlockHasAnyMpa(block)) {
            const uint256 root{modern::ComputePayloadRoot(block)};
            CMutableTransaction cb2{*block.vtx[0]};
            cb2.vout.emplace_back(0, modern::MakePayloadRootCellScript(root));
            block.vtx[0] = MakeTransactionRef(cb2);
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        Sign(block, key == m_vk_a ? m_validator_a : key == m_vk_c ? m_validator_c : m_validator_b);
        return block;
    }
    //! Produce and connect one block; returns its hash.
    uint256 Produce(const modern::PosValidatorKey& key,
                    const std::vector<std::pair<CScript, CMpaRecord>>& coinbase_payload = {},
                    std::vector<CMutableTransaction> txs = {})
    {
        const int prev_height{Tip()->nHeight};
        const CBlock block{BuildPosBlock(key, coinbase_payload, std::move(txs))};
        if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
        BOOST_REQUIRE_MESSAGE(Submit(block), "modern-PoS block at height " << prev_height + 1 << " rejected");
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, prev_height + 1);
        return block.GetHash();
    }
    //! Produce blocks until the tip is at `height`.
    void ProduceTo(const int height, const modern::PosValidatorKey& key)
    {
        while (Tip()->nHeight < height) Produce(key);
    }
    //! A block whose connect must fail; returns the reject reason recorded for it.
    void ProduceExpectConnectFailure(const modern::PosValidatorKey& key,
                                     const std::vector<std::pair<CScript, CMpaRecord>>& coinbase_payload,
                                     const int extra = 0)
    {
        const CBlock block{BuildPosBlock(key, coinbase_payload, {}, extra)};
        if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
        SubmitExpectConnectFailure(block);
    }
};

//! Disk-backed variant for restart / reindex scenarios.
struct FinalityChainDiskFixture : public FinalityChainFixture {
    FinalityChainDiskFixture()
        : FinalityChainFixture{{.coins_db_in_memory = false, .block_tree_db_in_memory = false}} {}
};

} // namespace b3test

#endif // B3COIN_TEST_UTIL_FINALITY_FIXTURE_H
