// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! Deterministic in-process functional test of the full legacy-to-modern
//! transition on a synthetic chain: legacy codecs and validation through H,
//! trusted replay, exact H/X enforcement, marker-modern blocks from H+1
//! through the modern PoS dispatch, pre-H spends, restarts, and reorg rules
//! around the boundary. No live network, no real datadir.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <txdb.h>
#include <consensus/block_codec.h>
#include <consensus/boundary.h>
#include <consensus/era.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <dbwrapper.h>
#include <key.h>
#include <legacy/codec.h>
#include <legacy/consensus.h>
#include <legacy/primitives.h>
#include <legacy/replay.h>
#include <node/utxo_commitment.h>
#include <node/utxo_equivalence_check.h>
#include <node/utxo_rows.h>
#include <kernel/mempool_removal_reason.h>
#include <modern/pos.h>
#include <modern/stake.h>
#include <node/mempool_persist.h>
#include <node/miner.h>
#include <node/chainstate.h>
#include <node/kernel_notifications.h>
#include <node/stake_registry.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

//! Synthetic boundary: H legacy blocks, then modern.
constexpr int SYNTHETIC_H{34};
constexpr uint32_t GENESIS_TIME{1'400'000'000};
constexpr int64_t MOCK_NOW{1'400'100'000};

CBlock MakeSyntheticLegacyGenesis()
{
    CMutableTransaction coinbase;
    coinbase.version = 1;
    coinbase.nTime = GENESIS_TIME;
    coinbase.m_legacy_encoding = true;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << CScriptNum{0} << CScriptNum{42};
    coinbase.vout.emplace_back(0, CScript{});

    CBlock genesis;
    genesis.nVersion = 1;
    genesis.hashPrevBlock.SetNull();
    genesis.nTime = GENESIS_TIME;
    genesis.nBits = 0x207fffff;
    genesis.nNonce = 0;
    genesis.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

//! Regtest-based fixture whose consensus params become a synthetic
//! legacy-B3 chain (own legacy genesis) before chainstate activation.
struct TransitionSetup : public ChainTestingSetup {
    // The synthetic chain pays to anyone-can-spend scripts, which stock
    // standardness policy would reject; the mempool boundary tests exercise
    // the era gate, not standardness, so allow non-standard transactions.
    static TestOpts WithDefaults(TestOpts opts)
    {
        opts.extra_args.push_back("-acceptnonstdtxn=1");
        return opts;
    }
    explicit TransitionSetup(TestOpts opts = {}) : ChainTestingSetup{ChainType::REGTEST, WithDefaults(std::move(opts))}
    {
        SetMockTime(MOCK_NOW);
        auto& consensus{const_cast<Consensus::Params&>(m_node.chainman->GetConsensus())};
        consensus.legacy_b3coin = true;
        consensus.legacy_last_pow_block = 1'000; // synthetic chain stays PoW
        // The historical B3 chain predates these soft forks; disable their
        // height gates as mainnet does so legacy blocks validate.
        consensus.BIP34Height = std::numeric_limits<int>::max();
        consensus.BIP65Height = std::numeric_limits<int>::max();
        consensus.BIP66Height = std::numeric_limits<int>::max();
        consensus.CSVHeight = std::numeric_limits<int>::max();
        consensus.SegwitHeight = std::numeric_limits<int>::max();
        CBlock& genesis{const_cast<CBlock&>(m_node.chainman->GetParams().GenesisBlock())};
        genesis = MakeSyntheticLegacyGenesis();
        consensus.hashGenesisBlock = genesis.GetLegacyB3Hash();
        LoadVerifyActivateChainstate();
    }
};

//! Disk-backed variant: block-tree and coins databases on disk, so a
//! simulated shutdown/restart (chainman teardown + reconstruction) reloads
//! the persisted block index through LoadBlockIndexGuts.
struct TransitionDiskSetup : public TransitionSetup {
    TransitionDiskSetup()
        : TransitionSetup{{.coins_db_in_memory = false, .block_tree_db_in_memory = false}} {}
};

//! Accept-all modern PoS adapter, counting dispatches.
class AcceptingPos final : public modern::PosValidator
{
public:
    mutable int m_calls{0};
    bool CheckStake(const CBlock&, const CBlockIndex&, const CCoinsViewCache&,
                    BlockValidationState&) const override
    {
        ++m_calls;
        return true;
    }
};

//! Round-trip a block through the legacy-chain wire codec (marker-aware),
//! proving the same bytes drive submission.
std::shared_ptr<CBlock> CodecRoundTrip(const CBlock& block)
{
    DataStream bytes;
    bytes << legacy::TX_LEGACY(block);
    auto decoded{std::make_shared<CBlock>()};
    bytes >> legacy::TX_LEGACY(*decoded);
    return decoded;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(legacy_transition_tests, TransitionSetup)

BOOST_AUTO_TEST_CASE(full_legacy_to_modern_transition)
{
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    auto& mutable_consensus{const_cast<Consensus::Params&>(consensus)};

    const uint256 genesis_hash{consensus.hashGenesisBlock};
    BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->nHeight), 0);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->GetBlockHash()).GetHex(),
                        genesis_hash.GetHex());

    // ---- (1)+(2) Build and connect a synthetic legacy chain through H,
    // driving every block through the legacy codec round-trip and full
    // legacy validation (scrypt PoW, legacy rewards, legacy scripts).
    std::vector<CBlock> legacy_blocks; // heights 1..H, for replay later
    Txid mature_coinbase{};            // coinbase at height 2, spent at H
    Txid pre_h_txid{};                 // the pre-H non-coinbase UTXO source

    const auto build_legacy{[&](const CBlockIndex* prev, std::vector<CMutableTransaction> txs) {
        const int height{prev->nHeight + 1};
        CAmount fees{0}; // all synthetic transactions pay zero fee
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        // Legacy blocks require the coinbase to start with the serialized
        // height (int push, matching CheckLegacyBlock's expectation).
        coinbase.vin[0].scriptSig = CScript() << height << CScriptNum{7};
        coinbase.vout.emplace_back(legacy::GetProofOfWorkReward(fees, height, consensus),
                                   CScript() << OP_TRUE);

        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        block.nBits = legacy::GetNextTargetRequired(prev, /*proof_of_stake=*/false, consensus);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) {
            mtx.m_legacy_encoding = true;
            block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        while (UintToArith256(block.GetLegacyB3Hash()) > target) {
            ++block.nNonce;
            BOOST_REQUIRE(block.nNonce < 10'000'000);
        }
        return block;
    }};

    Txid coinbase3{}; // mature coinbases feeding the mempool boundary tests
    Txid coinbase4{};
    for (int height{1}; height <= SYNTHETIC_H - 1; ++height) {
        const CBlockIndex* prev{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
        CBlock block{build_legacy(prev, {})};
        const auto submitted{CodecRoundTrip(block)};
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlock(submitted, /*force_processing=*/true,
                                                       /*min_pow_checked=*/true, &new_block),
                              "legacy block at height " << height << " rejected");
        BOOST_REQUIRE(new_block);
        legacy_blocks.push_back(*submitted);
        if (height == 2) mature_coinbase = submitted->vtx[0]->GetHash();
        if (height == 3) coinbase3 = submitted->vtx[0]->GetHash();
        if (height == 4) coinbase4 = submitted->vtx[0]->GetHash();
    }
    BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->nHeight), SYNTHETIC_H - 1);

    // The final legacy block H carries a transaction spending a long-matured
    // legacy coinbase: its output becomes the pre-H UTXO spent in the modern
    // era. Built here so X is known before H connects, as in production.
    // Anyone-can-spend output large enough that its spender clears the
    // 65-byte minimum standalone transaction size.
    const CScript padded_script{CScript() << std::vector<unsigned char>(24, 0xb3) << OP_DROP << OP_TRUE};
    const auto legacy_spend_of{[&](const Txid& coinbase, CAmount fee) {
        const CBlockIndex* tip{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
        CMutableTransaction spend;
        spend.version = 1;
        spend.m_legacy_encoding = true;
        spend.nTime = static_cast<uint32_t>(tip->GetBlockTime() + 17);
        spend.vin.resize(1);
        spend.vin[0].prevout = COutPoint{coinbase, 0};
        spend.vin[0].scriptSig = CScript{}; // OP_TRUE output: no signature
        spend.vout.emplace_back(legacy::GetProofOfWorkReward(0, 2, consensus) - fee,
                                padded_script);
        return spend;
    }};
    CBlock block_h;
    {
        const CBlockIndex* prev{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
        block_h = build_legacy(prev, {legacy_spend_of(mature_coinbase, /*fee=*/0)});
    }
    const auto submitted_h{CodecRoundTrip(block_h)};
    pre_h_txid = submitted_h->vtx[1]->GetHash();
    {
        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(submitted_h, true, true, &new_block));
        BOOST_REQUIRE(new_block);
        legacy_blocks.push_back(*submitted_h);
    }
    BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->nHeight), SYNTHETIC_H);

    // ---- Mempool at the boundary, part 1: within-era reorg resurrection.
    // Disconnecting H (legal here: no boundary is finalized yet) returns its
    // non-coinbase transaction to the mempool with its legacy provenance.
    CTxMemPool& pool{*Assert(m_node.mempool)};
    {
        CBlockIndex* pindex_h{WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(submitted_h->GetLegacyB3Hash()))};
        BOOST_REQUIRE(pindex_h);
        BlockValidationState state;
        BOOST_REQUIRE(chainman.ActiveChainstate().InvalidateBlock(state, pindex_h));
        BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->nHeight), SYNTHETIC_H - 1);
        if (!pool.exists(pre_h_txid)) {
            // Diagnose exactly what resurrection's silent AcceptToMemoryPool
            // call would have said.
            LOCK(cs_main);
            const auto probe{AcceptToMemoryPool(chainman.ActiveChainstate(), submitted_h->vtx[1], GetTime(),
                                                /*bypass_limits=*/true, /*test_accept=*/true)};
            BOOST_REQUIRE_MESSAGE(false, "resurrection did not enter the pool (size " << pool.size()
                                             << "); direct bypass resubmit says: " << probe.m_state.ToString());
        }
        const CTransactionRef resurrected{pool.get(pre_h_txid)};
        BOOST_REQUIRE(resurrected);
        BOOST_CHECK(resurrected->IsLegacyEncoded());
    }

    // ---- Mempool at the boundary, part 2: pre-H admission (tip = H-1, the
    // next block is legacy). A modern-encoded transaction is refused; a
    // legacy-encoded spend of a mature coinbase is admitted.
    {
        CMutableTransaction modern_tx;
        modern_tx.version = 2;
        modern_tx.vin.resize(1);
        modern_tx.vin[0].prevout = COutPoint{pre_h_txid, 0};
        modern_tx.vout.emplace_back(1 * COIN, CScript() << OP_TRUE);
        const auto res{WITH_LOCK(cs_main, return chainman.ProcessTransaction(MakeTransactionRef(modern_tx)))};
        BOOST_REQUIRE(res.m_result_type != MempoolAcceptResult::ResultType::VALID);
        BOOST_CHECK_EQUAL(res.m_state.GetRejectReason(), "modern-txn-in-legacy-era");

        CMutableTransaction legacy_tx{legacy_spend_of(coinbase3, /*fee=*/100'000)};
        const auto ok{WITH_LOCK(cs_main, return chainman.ProcessTransaction(MakeTransactionRef(legacy_tx)))};
        BOOST_REQUIRE_MESSAGE(ok.m_result_type == MempoolAcceptResult::ResultType::VALID,
                              "legacy mempool admission failed: " << ok.m_state.ToString());

        // REGRESSION (live mainnet sync, 2026-08-21): a child spending an
        // UNCONFIRMED legacy parent. Coin::nHeight is 30 bits on B3, so a
        // MEMPOOL_HEIGHT sentinel that did not fit the field truncated, the
        // parent looked like a coin at height ~1.07e9, and
        // CalculateLockPointsAtTip asserted. The sentinel now fits (pinned by
        // static_assert) and the child must simply be admitted.
        {
            const CBlockIndex* tip{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
            CMutableTransaction child;
            child.version = 1;
            child.m_legacy_encoding = true;
            child.nTime = static_cast<uint32_t>(tip->GetBlockTime() + 18);
            child.vin.resize(1);
            child.vin[0].prevout = COutPoint{CTransaction{legacy_tx}.GetHash(), 0};
            child.vin[0].scriptSig = CScript{};
            child.vout.emplace_back(legacy_tx.vout[0].nValue - 100'000, padded_script);
            const auto child_ok{WITH_LOCK(cs_main, return chainman.ProcessTransaction(MakeTransactionRef(child)))};
            BOOST_REQUIRE_MESSAGE(child_ok.m_result_type == MempoolAcceptResult::ResultType::VALID,
                                  "child of unconfirmed legacy parent refused: "
                                      << child_ok.m_state.ToString());
            // And the mempool view reports the parent at the sentinel height.
            {
                LOCK2(cs_main, pool.cs);
                const CCoinsViewMemPool view_mempool(&chainman.ActiveChainstate().CoinsTip(), pool);
                const auto parent_coin{view_mempool.GetCoin(child.vin[0].prevout)};
                BOOST_REQUIRE(parent_coin.has_value());
                BOOST_CHECK_EQUAL(parent_coin->nHeight, MEMPOOL_HEIGHT);
            }
        }
    }
    BOOST_CHECK_EQUAL(pool.size(), 3U); // the resurrected spend + the coinbase3 spend + its child

    // ---- Mempool at the boundary, part 3: persistence round trip pre-H.
    // Provenance survives dump+load: the reloaded entry is the same legacy
    // transaction with the same txid.
    const fs::path mempool_dat{m_args.GetDataDirBase() / "boundary_mempool.dat"};
    const Txid coinbase3_spend_txid{[&] {
        LOCK(pool.cs);
        for (const auto& entry_info : pool.infoAll()) {
            if (entry_info.tx->GetHash() != pre_h_txid) return entry_info.tx->GetHash();
        }
        return Txid{};
    }()};
    BOOST_REQUIRE(coinbase3_spend_txid != Txid{});
    BOOST_REQUIRE(node::DumpMempool(pool, mempool_dat));
    {
        const CTransactionRef tx3{pool.get(coinbase3_spend_txid)};
        BOOST_REQUIRE(tx3);
        WITH_LOCK(pool.cs, pool.removeRecursive(*tx3, MemPoolRemovalReason::REORG));
        BOOST_CHECK(!pool.exists(coinbase3_spend_txid));
        BOOST_REQUIRE(node::LoadMempool(pool, mempool_dat, chainman.ActiveChainstate(), {}));
        BOOST_REQUIRE(pool.exists(coinbase3_spend_txid));
        const CTransactionRef reloaded{pool.get(coinbase3_spend_txid)};
        BOOST_REQUIRE(reloaded);
        BOOST_CHECK(reloaded->IsLegacyEncoded());
    }

    // ---- Mempool at the boundary, part 4: finalize H/X BEFORE H connects --
    // the production sequence -- then reconnect H. Connecting the final legacy
    // block atomically empties the pool: the included spend leaves via the
    // block, and the still-unmined coinbase3 spend leaves via the era flush.
    mutable_consensus.hard_fork_height = SYNTHETIC_H + 1;
    mutable_consensus.legacy_final_hash = submitted_h->GetLegacyB3Hash();
    {
        CBlockIndex* pindex_h{WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(submitted_h->GetLegacyB3Hash()))};
        BOOST_REQUIRE(pindex_h);
        {
            // Mirror the reconsiderblock RPC: clearing failure flags must be
            // paired with a best-header recalculation.
            LOCK(cs_main);
            chainman.ActiveChainstate().ResetBlockFailureFlags(pindex_h);
            chainman.RecalculateBestHeader();
        }
        BlockValidationState state;
        BOOST_REQUIRE(chainman.ActiveChainstate().ActivateBestChain(state));
        BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->nHeight), SYNTHETIC_H);
    }
    BOOST_CHECK_EQUAL(pool.size(), 0U);

    // ---- Mempool at the boundary, part 5: post-H admission (tip = H, the
    // next block is modern). A legacy-encoded transaction is refused; a
    // modern-encoded spend of the pre-H UTXO is admitted; and the pre-H
    // mempool.dat does not repopulate the pool.
    Txid modern_mempool_txid{};
    {
        CMutableTransaction stale_legacy{legacy_spend_of(coinbase4, /*fee=*/100'000)};
        const auto res{WITH_LOCK(cs_main, return chainman.ProcessTransaction(MakeTransactionRef(stale_legacy)))};
        BOOST_REQUIRE(res.m_result_type != MempoolAcceptResult::ResultType::VALID);
        BOOST_CHECK_EQUAL(res.m_state.GetRejectReason(), "legacy-txn-in-modern-era");

        CMutableTransaction modern_tx;
        modern_tx.version = 2;
        modern_tx.vin.resize(1);
        modern_tx.vin[0].prevout = COutPoint{pre_h_txid, 0};
        modern_tx.vout.emplace_back(legacy::GetProofOfWorkReward(0, 2, consensus) - 100'000,
                                    padded_script);
        const auto ok{WITH_LOCK(cs_main, return chainman.ProcessTransaction(MakeTransactionRef(modern_tx)))};
        BOOST_REQUIRE_MESSAGE(ok.m_result_type == MempoolAcceptResult::ResultType::VALID,
                              "modern mempool admission failed: " << ok.m_state.ToString());
        modern_mempool_txid = CTransaction{modern_tx}.GetHash();
        BOOST_CHECK(pool.get(modern_mempool_txid) && !pool.get(modern_mempool_txid)->IsLegacyEncoded());

        // The pre-H dump holds only legacy transactions; the era gate refuses
        // every one of them, so a pre-H mempool.dat can never repopulate a
        // post-H node.
        BOOST_REQUIRE(node::LoadMempool(pool, mempool_dat, chainman.ActiveChainstate(), {}));
        BOOST_CHECK(!pool.exists(coinbase3_spend_txid));
        BOOST_CHECK_EQUAL(pool.size(), 1U);
    }

    // ---- Mempool at the boundary, part 6: with X now ACTIVE on the chain,
    // the cross-boundary prohibition is binding end to end: invalidating H is
    // refused cleanly (no crash, no shutdown), the tip stays put, and the
    // modern mempool entry survives untouched.
    {
        CBlockIndex* pindex_h{WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(submitted_h->GetLegacyB3Hash()))};
        BOOST_REQUIRE(pindex_h);
        BOOST_REQUIRE(WITH_LOCK(cs_main, return chainman.ActiveChainstate().LegacyBoundaryActive()));
        BlockValidationState state;
        BOOST_CHECK(!chainman.ActiveChainstate().InvalidateBlock(state, pindex_h));
        BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->nHeight), SYNTHETIC_H);
        BOOST_CHECK(pool.exists(modern_mempool_txid));
    }

    // ---- (5) Finalize the boundary: exact H and X.
    const uint256 X{legacy_blocks.back().GetLegacyB3Hash()};
    mutable_consensus.hard_fork_height = SYNTHETIC_H + 1;
    mutable_consensus.legacy_final_hash = X;
    BOOST_CHECK_EQUAL(*Consensus::LegacyFinalHeight(consensus), SYNTHETIC_H);
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveChain()[SYNTHETIC_H]->GetBlockHash()).GetHex(),
                      X.GetHex());

    // ---- (3)+(4) Trusted replay through H from the exact legacy wire
    // bytes, then verify the reconstructed UTXO set against the live
    // chainstate.
    const fs::path replay_path{m_args.GetDataDirBase() / "transition_replay"};
    {
        legacy::ReplayDB db{DBParams{.path = replay_path, .cache_bytes = 1 << 20}};
        legacy::PersistentReplay replay{consensus, SYNTHETIC_H, {{SYNTHETIC_H, X}}, db};
        std::string error;
        BOOST_REQUIRE_MESSAGE(replay.Load(error), error);

        DataStream genesis_bytes;
        genesis_bytes << legacy::TX_LEGACY(chainman.GetParams().GenesisBlock());
        BOOST_REQUIRE_MESSAGE(replay.ApplyRawBlock(std::span{genesis_bytes}, error), error);
        for (const CBlock& block : legacy_blocks) {
            DataStream bytes;
            bytes << legacy::TX_LEGACY(block);
            BOOST_REQUIRE_MESSAGE(replay.ApplyRawBlock(std::span{bytes}, error), error);
        }
        BOOST_REQUIRE_MESSAGE(replay.Finish(error), error);
        BOOST_CHECK_EQUAL(replay.TipHash().GetHex(), X.GetHex());

        // Reconstructed UTXOs equal the live chainstate's view at H.
        CCoinsViewCache replay_view{&db};
        LOCK(cs_main);
        CCoinsViewCache& chain_view{chainman.ActiveChainstate().CoinsTip()};
        const COutPoint spent{mature_coinbase, 0};
        const COutPoint unspent{pre_h_txid, 0};
        BOOST_CHECK(!replay_view.HaveCoin(spent));
        BOOST_CHECK(!chain_view.HaveCoin(spent));
        BOOST_REQUIRE(replay_view.HaveCoin(unspent));
        BOOST_REQUIRE(chain_view.HaveCoin(unspent));
        const Coin& a{replay_view.AccessCoin(unspent)};
        const Coin& b{chain_view.AccessCoin(unspent)};
        BOOST_CHECK(a.out == b.out);
        BOOST_CHECK_EQUAL(a.nHeight, b.nHeight);
        BOOST_CHECK_EQUAL(a.fCoinBase, b.fCoinBase);
        BOOST_CHECK_EQUAL(a.fCoinStake, b.fCoinStake);
    }

    // ---- Full-set UTXO equivalence: the whole trusted-replay reconstruction
    // of the legacy prefix (genesis..H) must equal the fully-validated live
    // chainstate -- every outpoint and the exact Coin contents (value, script,
    // height, coinbase/coinstake flags, legacy nTime/nTxOffset), not aggregate
    // supply. Compared by canonical commitment with per-outpoint diagnostics
    // (node/utxo_commitment.h). Diagnostic only; never consensus. This is the
    // deterministic mechanism a real-chain U == U' proof would run against the
    // live chainstate DB and a replay DB.
    {
        const fs::path replay_utxo_path{m_args.GetDataDirBase() / "transition_replay_utxo"};
        CCoinsViewDB replay_db{
            DBParams{.path = replay_utxo_path, .cache_bytes = size_t{1} << 20, .wipe_data = true},
            CoinsViewOptions{}};
        {
            CCoinsViewCache replay_cache{&replay_db};
            legacy::TrustedReplay replay{consensus, SYNTHETIC_H, {{SYNTHETIC_H, X}}};
            std::string error;
            DataStream genesis_bytes;
            genesis_bytes << legacy::TX_LEGACY(chainman.GetParams().GenesisBlock());
            BOOST_REQUIRE_MESSAGE(replay.ApplyRawBlock(std::span{genesis_bytes}, replay_cache, error), error);
            for (const CBlock& block : legacy_blocks) {
                DataStream bytes;
                bytes << legacy::TX_LEGACY(block);
                BOOST_REQUIRE_MESSAGE(replay.ApplyRawBlock(std::span{bytes}, replay_cache, error), error);
            }
            // The reconstruction has reached X at H; record it so the flush has
            // a best-block marker (the coins, not the marker, are compared).
            replay_cache.SetBestBlock(X);
            replay_cache.Flush();
        }

        LOCK(cs_main);
        chainman.ActiveChainstate().ForceFlushStateToDisk();
        const node::UtxoComparison cmp{
            node::CompareUtxoViews(chainman.ActiveChainstate().CoinsDB(), replay_db)};

        for (const auto& m : cmp.mismatches) {
            BOOST_TEST_MESSAGE("UTXO mismatch at " + m.outpoint.ToString() +
                               ": live=" + (m.in_a ? "present" : "absent") +
                               " replay=" + (m.in_b ? "present" : "absent"));
        }
        BOOST_CHECK(cmp.mismatches.empty());
        BOOST_CHECK(cmp.Equal());
        BOOST_CHECK_EQUAL(cmp.commitment_a.GetHex(), cmp.commitment_b.GetHex());
        BOOST_CHECK_EQUAL(cmp.count_a, cmp.count_b);
        BOOST_CHECK_GT(cmp.count_a, size_t{0}); // the reconstructed set is non-trivial
    }

    // ---- The operator-facing U == U' check (b3coin-utxo-verify's core),
    // driven end to end against this chain: same-H/X verification, replay from
    // a block source, full-set comparison. Entirely outside consensus.
    {
        const auto block_source{[&](const int height) -> std::optional<CBlock> {
            if (height == 0) return chainman.GetParams().GenesisBlock();
            if (height < 1 || height > SYNTHETIC_H) return std::nullopt;
            return legacy_blocks[static_cast<size_t>(height) - 1];
        }};
        const auto scratch_db{[&](const std::string& name) {
            return CCoinsViewDB{DBParams{.path = m_args.GetDataDirBase() / fs::u8path(name),
                                         .cache_bytes = size_t{1} << 20,
                                         .wipe_data = true},
                                CoinsViewOptions{}};
        }};

        LOCK(cs_main);
        chainman.ActiveChainstate().ForceFlushStateToDisk();
        const CCoinsViewDB& live{chainman.ActiveChainstate().CoinsDB()};

        // Positive: the reconstruction equals the validated chainstate.
        {
            CCoinsViewDB scratch{scratch_db("uve_ok")};
            const auto res{node::VerifyReplayEquivalence(
                consensus, live, block_source, scratch,
                {.final_height = SYNTHETIC_H, .final_hash = X})};
            if (!res.errors.empty()) BOOST_TEST_MESSAGE("verify error: " + res.errors.front());
            BOOST_CHECK(res.errors.empty());
            BOOST_CHECK(res.ok);
            BOOST_CHECK_EQUAL(res.blocks_replayed, SYNTHETIC_H + 1);
            BOOST_CHECK_EQUAL(res.live_commitment.GetHex(), res.replay_commitment.GetHex());
            BOOST_CHECK_EQUAL(res.live_count, res.replay_count);
            BOOST_CHECK_GT(res.live_count, size_t{0});
            BOOST_CHECK_EQUAL(res.mismatch_total, size_t{0});
        }
        // Wrong X: refused up front with a same-H/X verification error.
        {
            CCoinsViewDB scratch{scratch_db("uve_wrongx")};
            uint256 wrong{X};
            wrong.begin()[0] ^= 0x01;
            const auto res{node::VerifyReplayEquivalence(
                consensus, live, block_source, scratch,
                {.final_height = SYNTHETIC_H, .final_hash = wrong})};
            BOOST_CHECK(!res.ok);
            BOOST_REQUIRE(!res.errors.empty());
            BOOST_CHECK(res.errors.front().find("best block") != std::string::npos);
            BOOST_CHECK_EQUAL(res.blocks_replayed, 0);
        }
        // A block source with a hole fails cleanly.
        {
            CCoinsViewDB scratch{scratch_db("uve_hole")};
            const auto holed{[&](const int height) -> std::optional<CBlock> {
                if (height == 3) return std::nullopt;
                return block_source(height);
            }};
            const auto res{node::VerifyReplayEquivalence(
                consensus, live, holed, scratch,
                {.final_height = SYNTHETIC_H, .final_hash = X})};
            BOOST_CHECK(!res.ok);
            BOOST_REQUIRE(!res.errors.empty());
            BOOST_CHECK(res.errors.front().find("height 3") != std::string::npos);
        }
        // A tampered source (two blocks swapped) breaks linkage inside the
        // replay engine rather than producing a bogus comparison.
        {
            CCoinsViewDB scratch{scratch_db("uve_swap")};
            const auto swapped{[&](const int height) -> std::optional<CBlock> {
                if (height == 3) return block_source(4);
                if (height == 4) return block_source(3);
                return block_source(height);
            }};
            const auto res{node::VerifyReplayEquivalence(
                consensus, live, swapped, scratch,
                {.final_height = SYNTHETIC_H, .final_hash = X})};
            BOOST_CHECK(!res.ok);
            BOOST_REQUIRE(!res.errors.empty());
            BOOST_CHECK(res.errors.front().find("replay failed") != std::string::npos);
        }
    }

    // ---- (11) A competing branch crossing H is rejected: X pins height H,
    // so any fork block at H fails acceptance and the tip is untouched.
    {
        const CBlockIndex* fork_parent{WITH_LOCK(cs_main, return chainman.ActiveChain()[SYNTHETIC_H - 1])};
        CBlock impostor{build_legacy(fork_parent, {})};
        BOOST_REQUIRE(impostor.GetLegacyB3Hash() != X);
        bool new_block{false};
        BOOST_CHECK(!chainman.ProcessNewBlock(CodecRoundTrip(impostor), true, true, &new_block));
        BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->GetBlockHash()).GetHex(),
                          X.GetHex());
    }

    // ---- (D2) Legacy is blocks-only. Build a distinct side branch forking
    // below H: a height-3 sibling carrying a spend, so its hash differs from
    // the canonical block 3. Its header alone must not be admitted -- a legacy
    // proof-of-stake header cannot be validated without its block, so it gains
    // no index entry and thus no chain-selection weight.
    const CBlockIndex* side_parent{WITH_LOCK(cs_main, return chainman.ActiveChain()[2])};
    CMutableTransaction side_spend;
    side_spend.version = 1;
    side_spend.nTime = static_cast<uint32_t>(side_parent->GetBlockTime() + 17);
    side_spend.vin.resize(1);
    side_spend.vin[0].prevout = COutPoint{mature_coinbase, 0};
    side_spend.vin[0].scriptSig = CScript{};
    side_spend.vout.emplace_back(1 * COIN, CScript() << OP_TRUE);
    const auto side_submitted{CodecRoundTrip(build_legacy(side_parent, {side_spend}))};
    // Legacy blocks are indexed by their marker (scrypt) hash, not SHA256d.
    const uint256 side_hash{side_submitted->GetLegacyB3Hash()};
    {
        const std::vector<CBlockHeader> headers{*side_submitted};
        BlockValidationState hstate;
        BOOST_CHECK(!chainman.ProcessNewBlockHeaders(headers, /*min_pow_checked=*/true, hstate));
        BOOST_CHECK_EQUAL(hstate.GetRejectReason(), "legacy-header-only");
        // The refused header created no index entry.
        BOOST_CHECK(WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(side_hash)) == nullptr);
    }

    // ---- (D1/anchor) The same block, submitted whole, is accepted as stored
    // side-branch history, but once X is pinned it is anchor-ineligible: it
    // lies off the X-anchored chain and can never be a chain-selection
    // candidate. Blocks on the canonical prefix, and X itself, stay eligible.
    {
        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(side_submitted, /*force_processing=*/true,
                                               /*min_pow_checked=*/true, &new_block));
        BOOST_REQUIRE(new_block);
        LOCK(cs_main);
        Chainstate& cs{chainman.ActiveChainstate()};
        const CBlockIndex* side_index{chainman.m_blockman.LookupBlockIndex(side_hash)};
        BOOST_REQUIRE(side_index);
        BOOST_CHECK_EQUAL(side_index->nHeight, 3);
        BOOST_CHECK(cs.IsAnchorIneligible(*side_index));
        BOOST_CHECK(!cs.setBlockIndexCandidates.contains(side_index));
        BOOST_CHECK(!cs.IsAnchorIneligible(*chainman.ActiveChain()[SYNTHETIC_H])); // X
        BOOST_CHECK(!cs.IsAnchorIneligible(*side_parent));                          // ancestor of X
    }

    // ---- (D2 adversarial) A full legacy block forking below H that claims
    // absurd chain work through a fake nBits, with no valid stake authority.
    // It is proof-of-stake (so the scrypt proof-of-work check is skipped) with
    // a well-formed block signature but a bogus kernel. The accept path does
    // not check the retarget, so it is stored with attacker-selected
    // chainwork exceeding the tip. Membership -- not stake validation -- keeps
    // it out of fork choice: being off the X-anchored chain, it is marked
    // anchor-ineligible, never enters the candidate set, is never connected
    // (its bogus kernel is never even reached), and the tip does not move.
    {
        const CBlockIndex* fork{WITH_LOCK(cs_main, return chainman.ActiveChain()[2])};
        const uint32_t t{static_cast<uint32_t>(fork->GetBlockTime() + 17)};
        CKey stake_key;
        stake_key.MakeNewKey(/*fCompressed=*/true);
        const CPubKey stake_pub{stake_key.GetPubKey()};

        CMutableTransaction cb; // empty proof-of-stake coinbase
        cb.version = 1;
        cb.m_legacy_encoding = true;
        cb.nTime = t;
        cb.vin.resize(1);
        cb.vin[0].prevout.SetNull();
        cb.vin[0].scriptSig = CScript() << 3 << CScriptNum{7};
        cb.vout.emplace_back(0, CScript{});

        CMutableTransaction cs_tx; // coinstake with a bogus (unchecked-here) kernel
        cs_tx.version = 1;
        cs_tx.m_legacy_encoding = true;
        cs_tx.nTime = t;
        cs_tx.vin.resize(1);
        cs_tx.vin[0].prevout = COutPoint{mature_coinbase, 0};
        cs_tx.vin[0].scriptSig = CScript{};
        cs_tx.vout.emplace_back(0, CScript{}); // coinstake marker output
        cs_tx.vout.emplace_back(1 * COIN, CScript() << ToByteVector(stake_pub) << OP_CHECKSIG);

        CBlock evil;
        evil.nVersion = 4;
        evil.hashPrevBlock = fork->GetBlockHash();
        evil.nTime = t;
        evil.nBits = 0x14000001; // absurdly low target => enormous GetBlockProof
        evil.vtx = {MakeTransactionRef(std::move(cb)), MakeTransactionRef(std::move(cs_tx))};
        evil.hashMerkleRoot = BlockMerkleRoot(evil);
        BOOST_REQUIRE(evil.IsProofOfStake());
        BOOST_REQUIRE(stake_key.Sign(evil.GetLegacyB3Hash(), evil.vchBlockSig));

        const arith_uint256 tip_work{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->nChainWork)};
        const uint256 evil_hash{evil.GetLegacyB3Hash()};
        const uint256 tip_before{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->GetBlockHash())};

        bool new_block{false};
        // Accepted as stored side-branch history: structure and signature are
        // valid, and the retarget is not checked on the accept path.
        BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlock(CodecRoundTrip(evil), /*force_processing=*/true,
                                                       /*min_pow_checked=*/true, &new_block),
                              "adversarial block unexpectedly rejected at accept");
        BOOST_REQUIRE(new_block);

        LOCK(cs_main);
        Chainstate& cs{chainman.ActiveChainstate()};
        const CBlockIndex* evil_index{chainman.m_blockman.LookupBlockIndex(evil_hash)};
        BOOST_REQUIRE(evil_index);
        BOOST_CHECK(evil_index->nChainWork > tip_work);              // it did carry attacker chainwork
        BOOST_CHECK(evil_index->nStatus & BLOCK_ANCHOR_INELIGIBLE);  // yet membership excludes it
        BOOST_CHECK(!cs.setBlockIndexCandidates.contains(evil_index));
        BOOST_CHECK(cs.IsAnchorIneligible(*evil_index));
        BOOST_CHECK_EQUAL(chainman.ActiveChain().Tip()->GetBlockHash().GetHex(), tip_before.GetHex());
        // Sync-control state is not poisoned either: despite its greater work,
        // the block is not selected as the best header.
        BOOST_CHECK(chainman.m_best_header != evil_index);
        BOOST_CHECK(!chainman.m_blockman.IsAnchorIneligible(*chainman.m_best_header));
        BOOST_CHECK_EQUAL(chainman.m_best_header->GetBlockHash().GetHex(), tip_before.GetHex());
    }

    // ---- (6)+(7) Marker-modern blocks from H+1 through the modern
    // PoS dispatch (test adapter; no economic rules invented).
    AcceptingPos pos;
    mutable_consensus.test_only_modern_pos_validator = &pos;

    const auto build_modern{[&](const CBlockIndex* prev, std::vector<CMutableTransaction> txs) {
        const int height{prev->nHeight + 1};
        CMutableTransaction coinbase;
        coinbase.version = 2;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << CScriptNum{height} << CScriptNum{9};
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);

        CBlock block;
        block.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        block.nBits = prev->nBits; // regtest: no retargeting
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        while (UintToArith256(block.GetHash()) > target) {
            ++block.nNonce;
            BOOST_REQUIRE(block.nNonce < 10'000'000);
        }
        return block;
    }};

    // (8) The first modern block spends the pre-H UTXO: old legacy txid as
    // prevout, old locking script enforced by modern validation.
    {
        const CBlockIndex* prev{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
        CMutableTransaction spend;
        spend.version = 2;
        spend.vin.resize(1);
        spend.vin[0].prevout = COutPoint{pre_h_txid, 0};
        spend.vin[0].scriptSig = CScript{};
        spend.vout.emplace_back(1 * COIN, CScript() << OP_TRUE);
        CBlock block{build_modern(prev, {spend})};
        const auto submitted{CodecRoundTrip(block)};
        BOOST_REQUIRE(!submitted->vtx[1]->IsLegacyEncoded());
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlock(submitted, true, true, &new_block),
                              "modern block at H+1 rejected");
    }
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->nHeight), SYNTHETIC_H + 1);
    BOOST_CHECK_EQUAL(pos.m_calls, 1);
    // A modern block descending from X is on the anchored chain, so it is not
    // anchor-ineligible.
    BOOST_CHECK(WITH_LOCK(cs_main,
        return !chainman.ActiveChainstate().IsAnchorIneligible(*chainman.ActiveChain().Tip())));
    BOOST_CHECK(WITH_LOCK(cs_main,
        return !chainman.ActiveChainstate().CoinsTip().HaveCoin(COutPoint{pre_h_txid, 0})));

    // ---- (9) Restart: the replay database reopens to the identical
    // completed state, and the flushed chainstate agrees with itself.
    {
        legacy::ReplayDB db{DBParams{.path = replay_path, .cache_bytes = 1 << 20}};
        legacy::PersistentReplay replay{consensus, SYNTHETIC_H, {{SYNTHETIC_H, X}}, db};
        std::string error;
        BOOST_REQUIRE_MESSAGE(replay.Load(error), error);
        BOOST_CHECK(replay.Completed());
        BOOST_CHECK_EQUAL(replay.TipHash().GetHex(), X.GetHex());
        BOOST_CHECK_EQUAL(replay.NextHeight(), SYNTHETIC_H + 1);
    }
    {
        LOCK(cs_main);
        chainman.ActiveChainstate().ForceFlushStateToDisk();
        CCoinsViewCache disk_view{&chainman.ActiveChainstate().CoinsDB()};
        BOOST_CHECK(!disk_view.HaveCoin(COutPoint{pre_h_txid, 0}));
        BOOST_CHECK(disk_view.HaveCoin(COutPoint{mature_coinbase, 0}) ==
                    chainman.ActiveChainstate().CoinsTip().HaveCoin(COutPoint{mature_coinbase, 0}));
    }

    // ---- (10) Continue with several modern blocks.
    for (int i{0}; i < 3; ++i) {
        const CBlockIndex* prev{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(
            chainman.ProcessNewBlock(CodecRoundTrip(build_modern(prev, {})), true, true, &new_block),
            "modern continuation block rejected");
    }
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->nHeight), SYNTHETIC_H + 4);
    BOOST_CHECK_EQUAL(pos.m_calls, 4);

    // ---- (12) A valid modern reorg entirely above H is permitted: fork at
    // H+1, longer branch wins, block H stays untouched.
    {
        const CBlockIndex* fork_point{WITH_LOCK(cs_main, return chainman.ActiveChain()[SYNTHETIC_H + 1])};
        const CBlockIndex* prev{fork_point};
        std::vector<std::shared_ptr<CBlock>> branch;
        for (int i{0}; i < 4; ++i) {
            CMutableTransaction tag; // make branch blocks distinct
            tag.version = 2;
            tag.vin.resize(1);
            tag.vin[0].prevout.SetNull();
            CBlock block{build_modern(prev, {})};
            block.vtx[0] = MakeTransactionRef([&] {
                CMutableTransaction cb{*block.vtx[0]};
                cb.vin[0].scriptSig = CScript() << CScriptNum{prev->nHeight + 1} << CScriptNum{77};
                return cb;
            }());
            block.hashMerkleRoot = BlockMerkleRoot(block);
            const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
            block.nNonce = 0;
            while (UintToArith256(block.GetHash()) > target) ++block.nNonce;
            auto submitted{std::make_shared<CBlock>(block)};
            bool new_block{false};
            BOOST_REQUIRE(chainman.ProcessNewBlock(submitted, true, true, &new_block));
            branch.push_back(submitted);
            prev = WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(submitted->GetHash()));
            BOOST_REQUIRE(prev);
        }
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(chainman.ActiveChain().Tip()->GetBlockHash().GetHex(),
                          branch.back()->GetHash().GetHex());
        BOOST_CHECK_EQUAL(chainman.ActiveChain().Tip()->nHeight, SYNTHETIC_H + 5);
        BOOST_CHECK_EQUAL(chainman.ActiveChain()[SYNTHETIC_H]->GetBlockHash().GetHex(), X.GetHex());
    }

    // ---- (13) The synthetic chain's genesis is unchanged end to end, and
    // the real B3 mainnet genesis identity was never touched.
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveChain()[0]->GetBlockHash()).GetHex(),
                      genesis_hash.GetHex());
    BOOST_CHECK_EQUAL(legacy::CreateCoreGenesisBlock().GetLegacyB3Hash().GetHex(),
                      "4b0d7f133c5267d715d4d8992635a5490d1edd6b7072cce3f8fe116aba983b6a");
}

//! The historical checkpoint rules separated across the three modes:
//!   PRE-X LIVE LEGACY   -> hardened checkpoints + rolling deep-reorg refusal
//!   POST-X TRUSTED REPLAY -> reconstructs the canonical prefix with NO
//!                            rolling refusal (a deep block is not "deep" to
//!                            replay), so the deep-reorg rule cannot break it
//!   MODERN >= H+1        -> legacy checkpoint/depth rules do not apply
BOOST_AUTO_TEST_CASE(legacy_checkpoint_and_depth_rules_are_mode_scoped)
{
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    auto& mutable_consensus{const_cast<Consensus::Params&>(consensus)};

    // A small span keeps the chain short: a block more than 6 below the tip is
    // "deep". No checkpoints yet.
    constexpr int SPAN{6};
    constexpr int MINI_H{12};
    mutable_consensus.legacy_checkpoint_span = SPAN;

    const auto build_legacy{[&](const CBlockIndex* prev, std::vector<CMutableTransaction> txs) {
        const int height{prev->nHeight + 1};
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << height << CScriptNum{7};
        coinbase.vout.emplace_back(legacy::GetProofOfWorkReward(0, height, consensus), CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        block.nBits = legacy::GetNextTargetRequired(prev, /*proof_of_stake=*/false, consensus);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) {
            mtx.m_legacy_encoding = true;
            block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        while (UintToArith256(block.GetLegacyB3Hash()) > target) {
            ++block.nNonce;
            BOOST_REQUIRE(block.nNonce < 10'000'000);
        }
        return block;
    }};

    // A distinct sibling of the block at `height`, carrying a spend so its hash
    // differs from the canonical block. Forks at height-1.
    Txid mature_coinbase{};
    const auto build_fork{[&](int fork_height) {
        const CBlockIndex* parent{WITH_LOCK(cs_main, return chainman.ActiveChain()[fork_height - 1])};
        CMutableTransaction spend;
        spend.version = 1;
        spend.nTime = static_cast<uint32_t>(parent->GetBlockTime() + 17);
        spend.vin.resize(1);
        spend.vin[0].prevout = COutPoint{mature_coinbase, 0};
        spend.vin[0].scriptSig = CScript{};
        spend.vout.emplace_back(1 * COIN, CScript() << OP_TRUE);
        return CodecRoundTrip(build_legacy(parent, {spend}));
    }};

    // Build the canonical mini-chain in order. Forward building is never
    // refused by the depth rule (each block is at tip+1).
    std::vector<CBlock> legacy_blocks;
    for (int height{1}; height <= MINI_H; ++height) {
        const CBlockIndex* prev{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
        const auto submitted{CodecRoundTrip(build_legacy(prev, {}))};
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlock(submitted, true, true, &new_block),
                              "canonical legacy block at height " << height << " refused");
        legacy_blocks.push_back(*submitted);
        if (height == 2) mature_coinbase = submitted->vtx[0]->GetHash();
    }
    BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->nHeight), MINI_H);

    // ---- MODE 1: PRE-X LIVE LEGACY --------------------------------------

    // (a) A deep fork -- height 3, which is MINI_H - 3 = 9 > SPAN below the tip
    // -- is refused by the rolling depth rule, without a peer penalty, and no
    // index entry is created.
    {
        const auto deep_fork{build_fork(/*fork_height=*/3)};
        const uint256 deep_hash{deep_fork->GetLegacyB3Hash()};
        BlockValidationState state;
        bool new_block{false};
        BOOST_CHECK(!chainman.ProcessNewBlock(deep_fork, true, true, &new_block));
        // Re-run the header check directly to read the reason (ProcessNewBlock
        // does not surface it here).
        BOOST_CHECK(WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(deep_hash)) == nullptr);
    }

    // (b) A shallow fork -- height MINI_H, a sibling of the tip, 0 below it --
    // is NOT refused by the depth rule: it is stored as ordinary side-branch
    // history. This proves the rule refuses only DEEP forks.
    {
        const auto shallow_fork{build_fork(/*fork_height=*/MINI_H)};
        const uint256 shallow_hash{shallow_fork->GetLegacyB3Hash()};
        bool new_block{false};
        BOOST_CHECK(chainman.ProcessNewBlock(shallow_fork, true, true, &new_block));
        BOOST_CHECK(new_block);
        BOOST_CHECK(WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(shallow_hash)) != nullptr);
    }

    // (c) A hardened checkpoint mismatch at a pinned height is refused. Pin the
    // canonical block 8; a differing block at height 8 (not deep: 8 > MINI_H -
    // SPAN = 6) is refused by the checkpoint rule.
    {
        const uint256 canonical8{WITH_LOCK(cs_main, return chainman.ActiveChain()[8]->GetBlockHash())};
        mutable_consensus.legacy_checkpoints = {{8, canonical8}};
        const auto bad_cp{build_fork(/*fork_height=*/8)};
        const uint256 bad_hash{bad_cp->GetLegacyB3Hash()};
        BOOST_REQUIRE(bad_hash != canonical8);
        bool new_block{false};
        BOOST_CHECK(!chainman.ProcessNewBlock(bad_cp, true, true, &new_block));
        BOOST_CHECK(WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(bad_hash)) == nullptr);
        mutable_consensus.legacy_checkpoints.clear();
    }

    // ---- MODE 2: POST-X TRUSTED REPLAY ----------------------------------
    // The same canonical history, replayed from wire bytes, reconstructs the
    // full prefix -- even though its early blocks are far more than SPAN below
    // the final block. Replay does NOT consult the rolling depth rule, so the
    // deep-reorg refusal that mode 1 applies cannot break replay of the
    // canonical X-anchored history.
    {
        const uint256 X{legacy_blocks.back().GetLegacyB3Hash()};
        const fs::path replay_path{m_args.GetDataDirBase() / "checkpoint_replay"};
        legacy::ReplayDB db{DBParams{.path = replay_path, .cache_bytes = 1 << 20}};
        legacy::PersistentReplay replay{consensus, MINI_H, {{MINI_H, X}}, db};
        std::string error;
        BOOST_REQUIRE_MESSAGE(replay.Load(error), error);
        DataStream genesis_bytes;
        genesis_bytes << legacy::TX_LEGACY(chainman.GetParams().GenesisBlock());
        BOOST_REQUIRE_MESSAGE(replay.ApplyRawBlock(std::span{genesis_bytes}, error), error);
        for (const CBlock& block : legacy_blocks) {
            DataStream bytes;
            bytes << legacy::TX_LEGACY(block);
            // Block 1 is MINI_H - 1 = 11 below the tip, far deeper than SPAN;
            // replay accepts it regardless.
            BOOST_REQUIRE_MESSAGE(replay.ApplyRawBlock(std::span{bytes}, error), error);
        }
        BOOST_REQUIRE_MESSAGE(replay.Finish(error), error);
        BOOST_CHECK_EQUAL(replay.TipHash().GetHex(), X.GetHex());
    }

    // ---- MODE 3: MODERN >= H+1 ------------------------------------------
    // Finalize the boundary at the mini-chain tip and connect a modern block.
    // The legacy checkpoint/depth rules are gated on the legacy era, so they do
    // not apply to it even though the span is still configured.
    {
        const uint256 X{WITH_LOCK(cs_main, return chainman.ActiveChain()[MINI_H]->GetBlockHash())};
        mutable_consensus.hard_fork_height = MINI_H + 1;
        mutable_consensus.legacy_final_hash = X;
        AcceptingPos pos;
        mutable_consensus.test_only_modern_pos_validator = &pos;

        const CBlockIndex* prev{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
        CMutableTransaction coinbase;
        coinbase.version = 2;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << CScriptNum{MINI_H + 1} << CScriptNum{9};
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        block.nBits = prev->nBits;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        while (UintToArith256(block.GetHash()) > target) ++block.nNonce;
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlock(CodecRoundTrip(block), true, true, &new_block),
                              "modern block at H+1 refused");
        BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->nHeight), MINI_H + 1);
    }
}

//! With the boundary (H, X) pinned, legacy blocks CONNECT through the
//! trusted replay engine: live-only connect rules (rewards, maturity, the
//! kernel, scripts) are never re-judged, while the engine's mechanical
//! checks (prevout existence, duplicate spends, linkage, Merkle) still
//! reject internally inconsistent data at connect time.
BOOST_AUTO_TEST_CASE(pinned_boundary_blocks_connect_through_replay)
{
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    auto& mutable_consensus{const_cast<Consensus::Params&>(consensus)};

    constexpr int LIVE_TIP{10};
    constexpr int PINNED_H{12};

    const auto build_legacy{[&](const CBlockIndex* prev) {
        const int height{prev->nHeight + 1};
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << height << CScriptNum{7};
        coinbase.vout.emplace_back(legacy::GetProofOfWorkReward(0, height, consensus), CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        block.nBits = legacy::GetNextTargetRequired(prev, /*proof_of_stake=*/false, consensus);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        while (UintToArith256(block.GetLegacyB3Hash()) > target) {
            ++block.nNonce;
            BOOST_REQUIRE(block.nNonce < 10'000'000);
        }
        return block;
    }};

    // Canonical live chain to height 10; remember block 10's coinbase (it
    // will be spent, deeply immature, in block 11).
    Txid immature_coinbase{};
    for (int height{1}; height <= LIVE_TIP; ++height) {
        const CBlockIndex* prev{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
        const auto submitted{CodecRoundTrip(build_legacy(prev))};
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlock(submitted, true, true, &new_block),
                              "canonical legacy block at height " << height << " refused");
        if (height == LIVE_TIP) immature_coinbase = submitted->vtx[0]->GetHash();
    }

    // Offline raw builder for the pinned era: deliberately no grinding and
    // no retarget conformance -- attested history is not re-judged.
    const auto make_pinned_block{[](const uint256& prev_hash, const uint32_t ntime, const int height,
                                    const CAmount coinbase_value, std::vector<CMutableTransaction> txs) {
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = ntime;
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << height << CScriptNum{21};
        coinbase.vout.emplace_back(coinbase_value, CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev_hash;
        block.nTime = ntime;
        block.nBits = 0x207fffff;
        block.nNonce = 0;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) {
            mtx.m_legacy_encoding = true;
            block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        return block;
    }};

    const CBlockIndex* live_tip{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
    const uint32_t t11{static_cast<uint32_t>(live_tip->GetBlockTime() + 17)};

    // Block 11 violates two live connect rules at once: the coinbase pays
    // far more than the allowed reward, and a plain transaction spends the
    // deeply immature block-10 coinbase. Mechanically it is fully
    // consistent.
    const CAmount excessive{legacy::GetProofOfWorkReward(0, LIVE_TIP + 1, consensus) + 12'345 * COIN};
    CMutableTransaction immature_spend;
    immature_spend.version = 1;
    immature_spend.nTime = t11;
    immature_spend.vin.resize(1);
    immature_spend.vin[0].prevout = COutPoint{immature_coinbase, 0};
    immature_spend.vin[0].scriptSig = CScript() << std::vector<unsigned char>{0x01};
    immature_spend.vout.emplace_back(1 * COIN, CScript() << OP_2);
    const CBlock good11{make_pinned_block(live_tip->GetBlockHash(), t11, LIVE_TIP + 1,
                                          excessive, {immature_spend})};
    const CBlock good12{make_pinned_block(good11.GetLegacyB3Hash(), t11 + 17, PINNED_H,
                                          legacy::GetProofOfWorkReward(0, PINNED_H, consensus), {})};

    // Pin the boundary at the not-yet-submitted block 12.
    const uint256 X{good12.GetLegacyB3Hash()};
    mutable_consensus.hard_fork_height = PINNED_H + 1;
    mutable_consensus.legacy_final_hash = X;

    // A mechanically inconsistent sibling of block 11 -- it spends an
    // outpoint that does not exist -- still fails at connect time: the
    // engine's mechanical checks are the consensus of the pinned era.
    {
        CMutableTransaction ghost_spend;
        ghost_spend.version = 1;
        ghost_spend.nTime = t11 + 1;
        ghost_spend.vin.resize(1);
        ghost_spend.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{
            "00000000000000000000000000000000000000000000000000000000000000aa"}), 0};
        ghost_spend.vin[0].scriptSig = CScript() << std::vector<unsigned char>{0x02};
        ghost_spend.vout.emplace_back(1 * COIN, CScript() << OP_TRUE);
        const auto bad11{CodecRoundTrip(make_pinned_block(live_tip->GetBlockHash(), t11 + 1, LIVE_TIP + 1,
                                                          excessive, {ghost_spend}))};
        bool new_block{false};
        chainman.ProcessNewBlock(bad11, true, true, &new_block);
        const CBlockIndex* pindex{WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(
                                      bad11->GetLegacyB3Hash()))};
        BOOST_REQUIRE(pindex != nullptr);
        BOOST_CHECK(WITH_LOCK(cs_main, return static_cast<bool>(pindex->nStatus & BLOCK_FAILED_VALID)));
        BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->nHeight), LIVE_TIP);
    }

    // The live-invalid but attested blocks connect through the replay
    // engine and the boundary is reached exactly.
    {
        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(CodecRoundTrip(good11), true, true, &new_block));
        BOOST_REQUIRE(chainman.ProcessNewBlock(CodecRoundTrip(good12), true, true, &new_block));
        LOCK(cs_main);
        BOOST_REQUIRE_EQUAL(chainman.ActiveChain().Tip()->nHeight, PINNED_H);
        BOOST_CHECK_EQUAL(chainman.ActiveChain().Tip()->GetBlockHash().GetHex(), X.GetHex());
    }

    // The reconstructed state preserves exact historical identity: the
    // excessive coinbase exists as attested, the immature spend consumed
    // its input and created its output with the legacy metadata intact.
    {
        LOCK(cs_main);
        CCoinsViewCache& coins{chainman.ActiveChainstate().CoinsTip()};
        const Coin& cb{coins.AccessCoin(COutPoint{good11.vtx[0]->GetHash(), 0})};
        BOOST_REQUIRE(!cb.IsSpent());
        BOOST_CHECK(cb.fCoinBase);
        BOOST_CHECK_EQUAL(cb.out.nValue, excessive);
        BOOST_CHECK_EQUAL(cb.nHeight, static_cast<uint32_t>(LIVE_TIP + 1));
        BOOST_CHECK(!coins.HaveCoin(COutPoint{immature_coinbase, 0}));
        const Coin& spent_out{coins.AccessCoin(COutPoint{good11.vtx[1]->GetHash(), 0})};
        BOOST_REQUIRE(!spent_out.IsSpent());
        BOOST_CHECK_EQUAL(spent_out.out.nValue, 1 * COIN);
        BOOST_CHECK_EQUAL(spent_out.nTime, t11);
    }
}

//! Fake-chain-first poisoning: with the boundary pinned, a fabricated
//! legacy chain claiming an extremely hard target can become the active
//! chain before X is known (admission no longer re-judges difficulty and
//! connection is mechanical). Once the genuine chain arrives and X's index
//! entry exists, the node must classify the fabricated tip off-anchor,
//! unwind it through the standard undo path, and activate the attested
//! chain even though it carries far less recorded work; the best header
//! must recover with it.
BOOST_AUTO_TEST_CASE(off_anchor_active_tip_recovers_once_x_is_known)
{
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    auto& mutable_consensus{const_cast<Consensus::Params&>(consensus)};

    constexpr int PINNED_H{8};

    // Offline raw chain builder off the synthetic genesis. No grinding, no
    // retarget conformance: the pinned era never re-judges either.
    const auto make_chain{[&](const uint32_t nbits, const int64_t tag, const int length) {
        std::vector<CBlock> blocks;
        uint256 prev{consensus.hashGenesisBlock};
        uint32_t ntime{GENESIS_TIME};
        for (int height{1}; height <= length; ++height) {
            ntime += 17;
            CMutableTransaction coinbase;
            coinbase.version = 1;
            coinbase.nTime = ntime;
            coinbase.m_legacy_encoding = true;
            coinbase.vin.resize(1);
            coinbase.vin[0].prevout.SetNull();
            coinbase.vin[0].scriptSig = CScript() << height << CScriptNum{tag};
            coinbase.vout.emplace_back(100 * COIN, CScript() << OP_TRUE);
            CBlock block;
            block.nVersion = 4;
            block.hashPrevBlock = prev;
            block.nTime = ntime;
            block.nBits = nbits;
            block.nNonce = 0;
            block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
            block.hashMerkleRoot = BlockMerkleRoot(block);
            blocks.push_back(block);
            prev = block.GetLegacyB3Hash();
        }
        return blocks;
    }};

    // The genuine chain claims ordinary regtest-grade work; the fabricated
    // one claims a target hard enough that a single block outweighs the
    // whole attested chain.
    const std::vector<CBlock> genuine{make_chain(0x207fffff, 1, PINNED_H)};
    const std::vector<CBlock> fake{make_chain(0x1c00ffff, 2, PINNED_H - 1)};

    const uint256 X{genuine.back().GetLegacyB3Hash()};
    mutable_consensus.hard_fork_height = PINNED_H + 1;
    mutable_consensus.legacy_final_hash = X;

    // The fabricated chain arrives first and becomes the active chain: with
    // X not yet in the block index it is not classifiable.
    for (const CBlock& block : fake) {
        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(CodecRoundTrip(block), true, true, &new_block));
    }
    const uint256 fake_tip{fake.back().GetLegacyB3Hash()};
    {
        LOCK(cs_main);
        BOOST_REQUIRE_EQUAL(chainman.ActiveChain().Tip()->GetBlockHash().GetHex(), fake_tip.GetHex());
        BOOST_CHECK(chainman.m_best_header == chainman.ActiveChain().Tip());
    }

    // The genuine chain arrives second, every block below the fabricated
    // tip's recorded work. When X itself is stored, the fabricated tip
    // becomes classifiable and must be abandoned.
    for (const CBlock& block : genuine) {
        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(CodecRoundTrip(block), true, true, &new_block));
    }
    {
        LOCK(cs_main);
        const CBlockIndex* tip{chainman.ActiveChain().Tip()};
        BOOST_REQUIRE_EQUAL(tip->nHeight, PINNED_H);
        BOOST_CHECK_EQUAL(tip->GetBlockHash().GetHex(), X.GetHex());
        const CBlockIndex* fake_index{chainman.m_blockman.LookupBlockIndex(fake_tip)};
        BOOST_REQUIRE(fake_index != nullptr);
        // The fabricated chain still records more work than the attested
        // chain -- membership, not work, decided...
        BOOST_CHECK(fake_index->nChainWork > tip->nChainWork);
        // ...and it is now persistently off-anchor: stored side history,
        // not invalid, never again a fork-choice or sync influence.
        BOOST_CHECK(fake_index->nStatus & BLOCK_ANCHOR_INELIGIBLE);
        BOOST_CHECK(!(fake_index->nStatus & BLOCK_FAILED_VALID));
        BOOST_CHECK(chainman.m_best_header == tip);
        BOOST_CHECK(chainman.ActiveChainstate().LegacyBoundaryActive());
    }
}

//! Once the boundary (H, X) is pinned, legacy admission is replay-scoped:
//! it keeps the structural checks, the hardened checkpoints, the exact X at
//! H and the physical future-time bound, but stops re-judging attested
//! history with the live rules (version whitelist, PoW and target, block
//! signature, rolling depth bar). Off-anchor data that passes admission is
//! stored as side history, classified anchor-ineligible, and never moves
//! the tip.
BOOST_AUTO_TEST_CASE(pinned_admission_stops_re_judging_live_rules)
{
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    auto& mutable_consensus{const_cast<Consensus::Params&>(consensus)};

    constexpr int SPAN{6};
    constexpr int MINI_H{12};
    mutable_consensus.legacy_checkpoint_span = SPAN;

    const auto build_legacy{[&](const CBlockIndex* prev, std::vector<CMutableTransaction> txs) {
        const int height{prev->nHeight + 1};
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << height << CScriptNum{7};
        coinbase.vout.emplace_back(legacy::GetProofOfWorkReward(0, height, consensus), CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        block.nBits = legacy::GetNextTargetRequired(prev, /*proof_of_stake=*/false, consensus);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) {
            mtx.m_legacy_encoding = true;
            block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        while (UintToArith256(block.GetLegacyB3Hash()) > target) {
            ++block.nNonce;
            BOOST_REQUIRE(block.nNonce < 10'000'000);
        }
        return block;
    }};

    // Build the canonical mini-chain in live mode (boundary unpinned).
    Txid mature_coinbase{};
    for (int height{1}; height <= MINI_H; ++height) {
        const CBlockIndex* prev{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
        const auto submitted{CodecRoundTrip(build_legacy(prev, {}))};
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlock(submitted, true, true, &new_block),
                              "canonical legacy block at height " << height << " refused");
        if (height == 2) mature_coinbase = submitted->vtx[0]->GetHash();
    }
    BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->nHeight), MINI_H);

    // A sibling of the block at `height`, spending the mature coinbase so
    // its hash differs; live rules (target, PoW grinding) deliberately
    // violated when `wreck` is set.
    const auto build_fork{[&](const int fork_height, const bool wreck, const int32_t version) {
        const CBlockIndex* parent{WITH_LOCK(cs_main, return chainman.ActiveChain()[fork_height - 1])};
        CMutableTransaction spend;
        spend.version = 1;
        spend.nTime = static_cast<uint32_t>(parent->GetBlockTime() + 17);
        spend.vin.resize(1);
        spend.vin[0].prevout = COutPoint{mature_coinbase, 0};
        spend.vin[0].scriptSig = CScript{};
        spend.vout.emplace_back(1 * COIN, CScript() << OP_TRUE);
        CBlock block{build_legacy(parent, {spend})};
        block.nVersion = version;
        if (wreck) {
            block.nBits = 0x1d00ffff; // not the required target, and no grinding
            block.nNonce = 0;
        }
        return CodecRoundTrip(block);
    }};

    // Live-mode contrast: an unknown legacy version is refused outright.
    {
        const auto unknown_version{build_fork(MINI_H, /*wreck=*/false, /*version=*/7)};
        bool new_block{false};
        BOOST_CHECK(!chainman.ProcessNewBlock(unknown_version, true, true, &new_block));
        BOOST_CHECK(WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(
                                  unknown_version->GetLegacyB3Hash())) == nullptr);
    }

    // Pin the boundary at the mini-chain tip.
    const uint256 X{WITH_LOCK(cs_main, return chainman.ActiveChain()[MINI_H]->GetBlockHash())};
    mutable_consensus.hard_fork_height = MINI_H + 1;
    mutable_consensus.legacy_final_hash = X;

    // (a) A DEEP fork (height 3, more than SPAN below the tip) with a wrong,
    // ungrounded target and an unknown version: the depth bar, version
    // whitelist, PoW and target checks are all live-only, so admission now
    // stores it as side history — immediately anchor-ineligible, tip unmoved.
    {
        const auto deep{build_fork(3, /*wreck=*/true, /*version=*/7)};
        const uint256 deep_hash{deep->GetLegacyB3Hash()};
        bool new_block{false};
        BOOST_CHECK(chainman.ProcessNewBlock(deep, true, true, &new_block));
        BOOST_CHECK(new_block);
        const CBlockIndex* pindex{WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(deep_hash))};
        BOOST_REQUIRE(pindex != nullptr);
        BOOST_CHECK(WITH_LOCK(cs_main, return static_cast<bool>(pindex->nStatus & BLOCK_ANCHOR_INELIGIBLE)));
        BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->GetBlockHash().GetHex()), X.GetHex());
    }

    // (b) A proof-of-stake-shaped fork (below H, so the boundary pin does not
    // apply) with a garbage block signature: signature validation is
    // live-only adjudication, so admission stores it.
    {
        const CBlockIndex* parent{WITH_LOCK(cs_main, return chainman.ActiveChain()[MINI_H - 2])};
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(parent->GetBlockTime() + 16);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (MINI_H - 1) << CScriptNum{11};
        coinbase.vout.emplace_back(0, CScript{});
        CMutableTransaction stake;
        stake.version = 1;
        stake.nTime = static_cast<uint32_t>(parent->GetBlockTime() + 16);
        stake.m_legacy_encoding = true;
        stake.vin.resize(1);
        stake.vin[0].prevout = COutPoint{mature_coinbase, 0};
        stake.vin[0].scriptSig = CScript() << std::vector<unsigned char>{0xaa};
        stake.vout.emplace_back(0, CScript{});
        stake.vout.emplace_back(1 * COIN, CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = parent->GetBlockHash();
        block.nTime = static_cast<uint32_t>(parent->GetBlockTime() + 16);
        block.nBits = 0x1d00ffff;
        block.nNonce = 0;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        block.vtx.push_back(MakeTransactionRef(std::move(stake)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        block.vchBlockSig = {0xde, 0xad};
        const auto submitted{CodecRoundTrip(block)};
        bool new_block{false};
        BOOST_CHECK(chainman.ProcessNewBlock(submitted, true, true, &new_block));
        BOOST_CHECK(WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(
                                  submitted->GetLegacyB3Hash())) != nullptr);
        BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->GetBlockHash().GetHex()), X.GetHex());
    }

    // (c) A hardened checkpoint mismatch is still refused when pinned: the
    // checkpoints are the membership anchors of the attested chain.
    {
        const uint256 canonical8{WITH_LOCK(cs_main, return chainman.ActiveChain()[8]->GetBlockHash())};
        mutable_consensus.legacy_checkpoints = {{8, canonical8}};
        const auto bad_cp{build_fork(8, /*wreck=*/true, /*version=*/4)};
        bool new_block{false};
        BOOST_CHECK(!chainman.ProcessNewBlock(bad_cp, true, true, &new_block));
        BOOST_CHECK(WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(
                                  bad_cp->GetLegacyB3Hash())) == nullptr);
        mutable_consensus.legacy_checkpoints.clear();
    }

    // (d) A block at H that is not X is still refused: the boundary pin is
    // exact identity, not a live rule.
    {
        const auto not_x{build_fork(MINI_H, /*wreck=*/true, /*version=*/4)};
        BOOST_REQUIRE(not_x->GetLegacyB3Hash() != X);
        bool new_block{false};
        BOOST_CHECK(!chainman.ProcessNewBlock(not_x, true, true, &new_block));
        BOOST_CHECK(WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(
                                  not_x->GetLegacyB3Hash())) == nullptr);
    }

    // (e) The physical future-time bound survives pinning (below H, so only
    // the time bound can refuse): attested history is in the past by
    // construction, so this only refuses fabricated data.
    {
        const CBlockIndex* parent{WITH_LOCK(cs_main, return chainman.ActiveChain()[MINI_H - 2])};
        CBlock block{build_legacy(parent, {})};
        block.nTime = static_cast<uint32_t>(MOCK_NOW + legacy::MAX_FUTURE_BLOCK_TIME + 100);
        CMutableTransaction cb{*block.vtx[0]};
        cb.nTime = block.nTime;
        block.vtx[0] = MakeTransactionRef(std::move(cb));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const auto submitted{CodecRoundTrip(block)};
        bool new_block{false};
        BOOST_CHECK(!chainman.ProcessNewBlock(submitted, true, true, &new_block));
        BOOST_CHECK(WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(
                                  submitted->GetLegacyB3Hash())) == nullptr);
    }
}

//! Non-empty regtest transition, ending at the deliberate fail-closed modern
//! PoS gate. Integration-only: it does not stand in for the real-history
//! three-way U_master == U_port == U_replay proof. The legacy chain is built
//! through the LIVE legacy consensus path (unpinned: real target, reward cap,
//! maturity, script and codec checks), carries meaningful UTXO history
//! (matured coinbase spends, an output split, a two-input merge, unclaimed
//! fees, varied scripts), then the boundary is frozen at (H, X) and the flow
//! is verified end to end: boundary active, exact coin-level state, full-set
//! replay equivalence with byte-identical canonical row files, and — with no
//! modern PoS rule set installed — every modern block at H+1 refused by the
//! fail-closed gate while the chain stays exactly at X.
BOOST_AUTO_TEST_CASE(non_empty_transition_fails_closed_at_h_plus_one)
{
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    auto& mutable_consensus{const_cast<Consensus::Params&>(consensus)};

    constexpr int H{36};
    BOOST_REQUIRE(consensus.test_only_modern_pos_validator == nullptr); // fail-closed setup

    const auto build_legacy{[&](const CBlockIndex* prev, std::vector<CMutableTransaction> txs) {
        const int height{prev->nHeight + 1};
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << height << CScriptNum{13};
        coinbase.vout.emplace_back(legacy::GetProofOfWorkReward(0, height, consensus),
                                   CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        block.nBits = legacy::GetNextTargetRequired(prev, /*proof_of_stake=*/false, consensus);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) {
            mtx.m_legacy_encoding = true;
            block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        while (UintToArith256(block.GetLegacyB3Hash()) > target) {
            ++block.nNonce;
            BOOST_REQUIRE(block.nNonce < 10'000'000);
        }
        return block;
    }};
    const auto submit{[&](const CBlock& block, std::vector<CBlock>& chain_log) {
        const auto submitted{CodecRoundTrip(block)};
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlock(submitted, true, true, &new_block),
                              "live legacy block at height "
                                  << WITH_LOCK(cs_main, return chainman.ActiveChain().Height() + 1)
                                  << " rejected");
        BOOST_REQUIRE(new_block);
        chain_log.push_back(*submitted);
        return submitted;
    }};
    const auto tip{[&] { return WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()); }};

    // Distinct anyone-can-spend scripts so the set carries varied script bytes.
    const CScript script_a{CScript() << std::vector<unsigned char>(24, 0xb3) << OP_DROP << OP_TRUE};
    const CScript script_b{CScript() << std::vector<unsigned char>(24, 0xc4) << OP_DROP << OP_TRUE};
    const CScript script_c{CScript() << std::vector<unsigned char>(24, 0xd5) << OP_DROP << OP_TRUE};

    // ---- Live legacy history. Heights 1..31: plain reward coinbases.
    std::vector<CBlock> legacy_blocks;
    Txid coinbase1{};
    Txid coinbase2{};
    for (int height{1}; height <= 31; ++height) {
        const auto submitted{submit(build_legacy(tip(), {}), legacy_blocks)};
        if (height == 1) coinbase1 = submitted->vtx[0]->GetHash();
        if (height == 2) coinbase2 = submitted->vtx[0]->GetHash();
    }
    const CAmount r1{legacy::GetProofOfWorkReward(0, 1, consensus)};
    const CAmount r2{legacy::GetProofOfWorkReward(0, 2, consensus)};

    // Height 32: spend the long-matured coinbase 1, splitting it in two and
    // leaving an unclaimed fee (the reward cap is an upper bound).
    CMutableTransaction split;
    split.version = 1;
    split.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 17);
    split.vin.resize(1);
    split.vin[0].prevout = COutPoint{coinbase1, 0};
    split.vin[0].scriptSig = CScript{};
    split.vout.emplace_back(r1 / 4, script_a);
    split.vout.emplace_back(r1 / 2, script_b);
    const Txid split_txid{submit(build_legacy(tip(), {split}), legacy_blocks)->vtx[1]->GetHash()};

    // Height 33: chain-spend the second split output into three coins.
    CMutableTransaction fan;
    fan.version = 1;
    fan.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 17);
    fan.vin.resize(1);
    fan.vin[0].prevout = COutPoint{split_txid, 1};
    fan.vin[0].scriptSig = CScript{};
    fan.vout.emplace_back(r1 / 8, script_a);
    fan.vout.emplace_back(r1 / 8, script_b);
    fan.vout.emplace_back(r1 / 8, script_c);
    const Txid fan_txid{submit(build_legacy(tip(), {fan}), legacy_blocks)->vtx[1]->GetHash()};

    // Height 34: a two-input merge of the matured coinbase 2 and a fanned
    // coin into one output, again leaving a fee unclaimed.
    CMutableTransaction merge;
    merge.version = 1;
    merge.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 17);
    merge.vin.resize(2);
    merge.vin[0].prevout = COutPoint{coinbase2, 0};
    merge.vin[0].scriptSig = CScript{};
    merge.vin[1].prevout = COutPoint{fan_txid, 0};
    merge.vin[1].scriptSig = CScript{};
    merge.vout.emplace_back(r2, script_c);
    const Txid merge_txid{submit(build_legacy(tip(), {merge}), legacy_blocks)->vtx[1]->GetHash()};

    // Height 35 plain; height 36 = H carries one final spend.
    submit(build_legacy(tip(), {}), legacy_blocks);
    CMutableTransaction last;
    last.version = 1;
    last.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 17);
    last.vin.resize(1);
    last.vin[0].prevout = COutPoint{fan_txid, 1};
    last.vin[0].scriptSig = CScript{};
    last.vout.emplace_back(r1 / 8 - 1000, script_a);
    const auto block_h{submit(build_legacy(tip(), {last}), legacy_blocks)};
    const Txid last_txid{block_h->vtx[1]->GetHash()};
    BOOST_REQUIRE_EQUAL(tip()->nHeight, H);

    // ---- Freeze the boundary at the just-built (H, X).
    const uint256 X{block_h->GetLegacyB3Hash()};
    mutable_consensus.hard_fork_height = H + 1;
    mutable_consensus.legacy_final_hash = X;
    BOOST_CHECK(WITH_LOCK(cs_main, return chainman.ActiveChainstate().LegacyBoundaryActive()));

    // Exact coin-level state at the frozen boundary.
    {
        LOCK(cs_main);
        CCoinsViewCache& coins{chainman.ActiveChainstate().CoinsTip()};
        BOOST_CHECK(!coins.HaveCoin(COutPoint{coinbase1, 0}));    // spent at 32
        BOOST_CHECK(!coins.HaveCoin(COutPoint{coinbase2, 0}));    // spent at 34
        BOOST_CHECK(!coins.HaveCoin(COutPoint{split_txid, 1}));   // fanned at 33
        BOOST_CHECK(coins.HaveCoin(COutPoint{split_txid, 0}));
        BOOST_CHECK(!coins.HaveCoin(COutPoint{fan_txid, 0}));     // merged at 34
        BOOST_CHECK(!coins.HaveCoin(COutPoint{fan_txid, 1}));     // spent at 36
        BOOST_CHECK(coins.HaveCoin(COutPoint{fan_txid, 2}));
        const Coin& merged{coins.AccessCoin(COutPoint{merge_txid, 0})};
        BOOST_REQUIRE(!merged.IsSpent());
        BOOST_CHECK_EQUAL(merged.out.nValue, r2);
        BOOST_CHECK(merged.out.scriptPubKey == script_c);
        BOOST_CHECK_EQUAL(merged.nHeight, 34U);
        BOOST_CHECK(!merged.fCoinBase);
        BOOST_CHECK(coins.HaveCoin(COutPoint{last_txid, 0}));
    }

    // ---- Full-set replay equivalence at the frozen boundary, plus canonical
    // row files: the reconstruction and the live state must be byte-identical
    // rows, and the set must be genuinely non-empty.
    {
        const fs::path replay_path{m_args.GetDataDirBase() / "nonempty_replay_utxo"};
        CCoinsViewDB replay_db{DBParams{.path = replay_path, .cache_bytes = size_t{1} << 20,
                                        .wipe_data = true},
                               CoinsViewOptions{}};
        {
            CCoinsViewCache replay_cache{&replay_db};
            legacy::TrustedReplay replay{consensus, H, {{H, X}}};
            std::string error;
            DataStream genesis_bytes;
            genesis_bytes << legacy::TX_LEGACY(chainman.GetParams().GenesisBlock());
            BOOST_REQUIRE_MESSAGE(replay.ApplyRawBlock(std::span{genesis_bytes}, replay_cache, error), error);
            for (const CBlock& block : legacy_blocks) {
                DataStream bytes;
                bytes << legacy::TX_LEGACY(block);
                BOOST_REQUIRE_MESSAGE(replay.ApplyRawBlock(std::span{bytes}, replay_cache, error), error);
            }
            replay_cache.SetBestBlock(X);
            replay_cache.Flush();
        }

        LOCK(cs_main);
        chainman.ActiveChainstate().ForceFlushStateToDisk();
        const node::UtxoComparison cmp{
            node::CompareUtxoViews(chainman.ActiveChainstate().CoinsDB(), replay_db)};
        BOOST_CHECK(cmp.Equal());
        BOOST_CHECK_EQUAL(cmp.count_a, cmp.count_b);
        BOOST_CHECK_GE(cmp.count_a, size_t{35}); // ~34 unspent coinbases + the spend outputs

        std::ostringstream port_rows;
        std::ostringstream replay_rows;
        std::string error;
        BOOST_REQUIRE(node::WriteUtxoRows(
            port_rows, {X, H, node::EnumerateUtxos(chainman.ActiveChainstate().CoinsDB())}, error));
        BOOST_REQUIRE(node::WriteUtxoRows(
            replay_rows, {X, H, node::EnumerateUtxos(replay_db)}, error));
        BOOST_CHECK(port_rows.str() == replay_rows.str());
        BOOST_CHECK(port_rows.str().find("b3-utxo-rows/v1") == 0);
    }

    // ---- H+1: with no modern rule set installed, the modern era fails
    // closed. Build a well-formed marker-modern block on X.
    const auto build_modern{[&](const int64_t tag) {
        const CBlockIndex* prev{tip()};
        CMutableTransaction coinbase;
        coinbase.version = 2;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << CScriptNum{H + 1} << CScriptNum{tag};
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        block.nBits = prev->nBits;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        while (UintToArith256(block.GetHash()) > target) ++block.nNonce;
        return block;
    }};

    // The precise refusal, observed at connect via the just-check path.
    {
        LOCK(cs_main);
        const BlockValidationState state{TestBlockValidity(
            chainman.ActiveChainstate(), build_modern(1), /*check_pow=*/true, /*check_merkle_root=*/true)};
        BOOST_REQUIRE(state.IsInvalid());
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "no-modern-pos-rules");
    }

    // The network path: the block is stored (structurally fine), fails at
    // connect, and the chain stays exactly at X — twice, deterministically.
    for (int64_t tag : {2, 3}) {
        const CBlock modern{build_modern(tag)};
        bool new_block{false};
        chainman.ProcessNewBlock(CodecRoundTrip(modern), true, true, &new_block);
        LOCK(cs_main);
        const CBlockIndex* pindex{chainman.m_blockman.LookupBlockIndex(modern.GetHash())};
        BOOST_REQUIRE(pindex != nullptr);
        BOOST_CHECK(pindex->nStatus & BLOCK_FAILED_VALID);
        BOOST_CHECK(!chainman.ActiveChain().Contains(pindex));
        BOOST_CHECK_EQUAL(chainman.ActiveChain().Tip()->GetBlockHash().GetHex(), X.GetHex());
    }

    // A LEGACY-codec block at H+1 is refused outright at the header: the
    // frozen boundary hard-switches the codec, so no legacy block can ever
    // extend X.
    {
        const CBlockIndex* prev{tip()};
        CBlock stale{build_legacy(prev, {})};
        bool new_block{false};
        BOOST_CHECK(!chainman.ProcessNewBlock(CodecRoundTrip(stale), true, true, &new_block));
        BOOST_CHECK(WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(
                                  stale.GetLegacyB3Hash())) == nullptr);
    }

    // The boundary survives it all: still active, tip still X at H.
    {
        LOCK(cs_main);
        BOOST_CHECK(chainman.ActiveChainstate().LegacyBoundaryActive());
        BOOST_CHECK_EQUAL(chainman.ActiveChain().Tip()->nHeight, H);
        BOOST_CHECK_EQUAL(chainman.ActiveChain().Tip()->GetBlockHash().GetHex(), X.GetHex());
    }
}

//! Temporary-PoW corridor validation (stage 2 of the corridor build-out):
//! with a pinned boundary and a configured corridor, marker-modern blocks in
//! H+1..H+length validate by the historical scrypt eligibility hash against
//! the constant corridor target — block identity stays in the modern hash
//! domain — and connect WITHOUT modern PoS; the fail-closed modern gate
//! moves to the first post-corridor height. An unset corridor target fails
//! closed; wrong nBits and an insufficient scrypt hash are refused.
BOOST_AUTO_TEST_CASE(transition_pow_corridor_validation)
{
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    auto& mutable_consensus{const_cast<Consensus::Params&>(consensus)};

    constexpr int H{3};
    constexpr int CORRIDOR{5};
    constexpr uint32_t EASY_BITS{0x207fffff};

    const auto tip{[&] { return WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()); }};

    // A short live-legacy chain to H.
    const auto build_legacy{[&](const CBlockIndex* prev) {
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (prev->nHeight + 1) << CScriptNum{99};
        coinbase.vout.emplace_back(legacy::GetProofOfWorkReward(0, prev->nHeight + 1, consensus),
                                   CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        block.nBits = legacy::GetNextTargetRequired(prev, /*proof_of_stake=*/false, consensus);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        while (UintToArith256(block.GetLegacyB3Hash()) > target) ++block.nNonce;
        return block;
    }};
    for (int height{1}; height <= H; ++height) {
        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(CodecRoundTrip(build_legacy(tip())), true, true, &new_block));
        BOOST_REQUIRE(new_block);
    }
    const uint256 X{tip()->GetBlockHash()};

    // Pin the boundary and configure the corridor (regtest scaffolding bits).
    mutable_consensus.hard_fork_height = H + 1;
    mutable_consensus.legacy_final_hash = X;
    mutable_consensus.transition_pow_length = CORRIDOR;
    mutable_consensus.transition_pow_bits = EASY_BITS;
    mutable_consensus.transition_pow_reward = 0; // ratified fees-only, stated explicitly
    BOOST_REQUIRE(consensus.test_only_modern_pos_validator == nullptr);

    // Corridor block builder: modern codec identity, scrypt-ground nBits.
    const auto build_corridor{[&](const CBlockIndex* prev, const uint32_t bits, const bool grind_scrypt) {
        CMutableTransaction coinbase;
        coinbase.version = 2;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << CScriptNum{prev->nHeight + 1} << CScriptNum{7};
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 60);
        block.nBits = bits;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        if (grind_scrypt) {
            while (UintToArith256(block.GetLegacyB3Hash()) > target) ++block.nNonce;
        } else {
            // Find a nonce whose scrypt eligibility hash FAILS the target.
            while (UintToArith256(block.GetLegacyB3Hash()) <= target) ++block.nNonce;
        }
        return block;
    }};

    // Wrong corridor difficulty is refused at the header.
    {
        CBlock bad{build_corridor(tip(), /*bits=*/0x207ffffe, /*grind_scrypt=*/true)};
        bool new_block{false};
        BOOST_CHECK(!chainman.ProcessNewBlock(CodecRoundTrip(bad), true, true, &new_block));
        BOOST_CHECK(WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(bad.GetHash())) == nullptr);
    }
    // A failing scrypt eligibility hash is refused, regardless of the
    // modern-domain identity hash.
    {
        CBlock bad{build_corridor(tip(), EASY_BITS, /*grind_scrypt=*/false)};
        bool new_block{false};
        BOOST_CHECK(!chainman.ProcessNewBlock(CodecRoundTrip(bad), true, true, &new_block));
        BOOST_CHECK(WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(bad.GetHash())) == nullptr);
    }
    // An unset corridor target fails closed.
    {
        mutable_consensus.transition_pow_bits.reset();
        CBlock blocked{build_corridor(tip(), EASY_BITS, /*grind_scrypt=*/true)};
        bool new_block{false};
        BOOST_CHECK(!chainman.ProcessNewBlock(CodecRoundTrip(blocked), true, true, &new_block));
        BOOST_CHECK(WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(blocked.GetHash())) == nullptr);
        mutable_consensus.transition_pow_bits = EASY_BITS;
        mutable_consensus.transition_pow_reward = 0; // ratified fees-only, stated explicitly
    }

    // The whole corridor connects under temporary PoW — no modern PoS
    // involvement, no test validator installed.
    for (int i{0}; i < CORRIDOR; ++i) {
        CBlock block{build_corridor(tip(), EASY_BITS, /*grind_scrypt=*/true)};
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlock(CodecRoundTrip(block), true, true, &new_block),
                              "corridor block at height " << tip()->nHeight + 1 << " rejected");
        BOOST_REQUIRE(new_block);
    }
    BOOST_CHECK_EQUAL(tip()->nHeight, H + CORRIDOR);

    // The first post-corridor height is modern PoS: fail-closed, exactly the
    // reason the modern gate gives, and the tip stays at the corridor end.
    {
        CBlock modern{build_corridor(tip(), EASY_BITS, /*grind_scrypt=*/true)};
        // Post-corridor nBits follow the stock modern rule (placeholder until
        // the modern PoS spec defines them); regtest's min-difficulty
        // walk-back lands on the last non-limit legacy bits. Grind the
        // modern-domain identity hash so only the PoS gate can refuse it.
        modern.nBits = GetNextWorkRequired(tip(), &modern, consensus);
        {
            const arith_uint256 sha_target{arith_uint256().SetCompact(modern.nBits)};
            modern.nNonce = 0;
            while (UintToArith256(modern.GetHash()) > sha_target) ++modern.nNonce;
        }
        {
            LOCK(cs_main);
            const BlockValidationState state{TestBlockValidity(
                chainman.ActiveChainstate(), modern, /*check_pow=*/false, /*check_merkle_root=*/true)};
            BOOST_REQUIRE(state.IsInvalid());
            BOOST_CHECK_EQUAL(state.GetRejectReason(), "no-modern-pos-rules");
        }
        BOOST_CHECK_EQUAL(tip()->nHeight, H + CORRIDOR);
    }
}

//! Corridor block production (stage 3): the assembler produces marker-modern
//! corridor templates with the configured corridor difficulty and a
//! fees-plus-corridor-reward coinbase; ground with the scrypt eligibility
//! helper they connect through the full network path. Production fails
//! cleanly when the corridor difficulty is unset, for legacy-era heights,
//! and at the first modern-PoS height (fail-closed gate).
BOOST_AUTO_TEST_CASE(transition_pow_corridor_production)
{
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    auto& mutable_consensus{const_cast<Consensus::Params&>(consensus)};

    constexpr int H{3};
    constexpr int CORRIDOR{3};
    constexpr uint32_t EASY_BITS{0x207fffff};

    const auto tip{[&] { return WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()); }};
    const auto build_legacy{[&](const CBlockIndex* prev) {
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (prev->nHeight + 1) << CScriptNum{99};
        coinbase.vout.emplace_back(legacy::GetProofOfWorkReward(0, prev->nHeight + 1, consensus),
                                   CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        block.nBits = legacy::GetNextTargetRequired(prev, /*proof_of_stake=*/false, consensus);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        while (UintToArith256(block.GetLegacyB3Hash()) > target) ++block.nNonce;
        return block;
    }};

    node::BlockAssembler::Options options;
    options.coinbase_output_script = CScript() << OP_TRUE;
    options.include_dummy_extranonce = true; // two-byte coinbase scriptSig at tiny heights

    // Legacy-era production is refused up front.
    BOOST_CHECK_THROW(
        node::BlockAssembler(chainman.ActiveChainstate(), nullptr, options).CreateNewBlock(),
        std::runtime_error);

    for (int height{1}; height <= H; ++height) {
        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(CodecRoundTrip(build_legacy(tip())), true, true, &new_block));
        BOOST_REQUIRE(new_block);
    }
    mutable_consensus.hard_fork_height = H + 1;
    mutable_consensus.legacy_final_hash = tip()->GetBlockHash();
    mutable_consensus.transition_pow_length = CORRIDOR;

    // Unset corridor difficulty refuses production (fail closed).
    BOOST_CHECK_THROW(
        node::BlockAssembler(chainman.ActiveChainstate(), nullptr, options).CreateNewBlock(),
        std::runtime_error);
    mutable_consensus.transition_pow_bits = EASY_BITS;
    mutable_consensus.transition_pow_reward = 0; // ratified fees-only, stated explicitly

    // Produce, grind and submit the whole corridor through the assembler.
    for (int i{0}; i < CORRIDOR; ++i) {
        SetMockTime(GetTime<std::chrono::seconds>() + std::chrono::seconds{60}); // corridor pacing: >= 60 s per block
        const auto tmpl{node::BlockAssembler(chainman.ActiveChainstate(), nullptr, options).CreateNewBlock()};
        BOOST_REQUIRE(tmpl);
        CBlock block{tmpl->block};
        BOOST_CHECK(Consensus::HasB3BlockCodecV2(block.nVersion));
        BOOST_CHECK_EQUAL(block.nBits, EASY_BITS);
        BOOST_CHECK_EQUAL(block.vtx[0]->GetValueOut(), 0); // fees only, reward param 0
        block.hashMerkleRoot = BlockMerkleRoot(block);
        block.nNonce = 0;
        while (!CheckTransitionPowEligibility(block)) ++block.nNonce;
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(
            chainman.ProcessNewBlock(std::make_shared<const CBlock>(block), true, true, &new_block),
            "assembled corridor block at height " << tip()->nHeight + 1 << " rejected");
        BOOST_REQUIRE(new_block);
    }
    BOOST_CHECK_EQUAL(tip()->nHeight, H + CORRIDOR);

    // The first modern-PoS height cannot be produced: the template's own
    // validity check hits the fail-closed modern gate.
    BOOST_CHECK_THROW(
        node::BlockAssembler(chainman.ActiveChainstate(), nullptr, options).CreateNewBlock(),
        std::runtime_error);
    BOOST_CHECK_EQUAL(tip()->nHeight, H + CORRIDOR);
}

//! Legacy UTXO -> modern OWNER crossing (stage 4): inside the corridor, a
//! modern transaction spends pre-H coins through the LEGACY_LOCK view under
//! the FROZEN legacy script rules — proven by a witness-program-shaped
//! legacy script that modern flags would refuse and the frozen rules accept
//! — while the same script created as a MODERN coin is refused under modern
//! rules, and the frozen legacy maturity rides with legacy coinbases.
BOOST_AUTO_TEST_CASE(legacy_lock_crossing_spend)
{
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    auto& mutable_consensus{const_cast<Consensus::Params&>(consensus)};

    constexpr int H{32};
    constexpr uint32_t EASY_BITS{0x207fffff};
    const auto tip{[&] { return WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()); }};

    const auto build_legacy{[&](const CBlockIndex* prev, std::vector<CMutableTransaction> txs) {
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (prev->nHeight + 1) << CScriptNum{99};
        coinbase.vout.emplace_back(legacy::GetProofOfWorkReward(0, prev->nHeight + 1, consensus),
                                   CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        block.nBits = legacy::GetNextTargetRequired(prev, /*proof_of_stake=*/false, consensus);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) {
            mtx.m_legacy_encoding = true;
            block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        while (UintToArith256(block.GetLegacyB3Hash()) > target) ++block.nNonce;
        return block;
    }};

    // Legacy chain: 31 plain blocks, then H = 32 carries a spend of the
    // (matured) coinbase 1 into a witness-program-SHAPED legacy script:
    // OP_0 <32 bytes>. Historically creatable; under frozen legacy rules it
    // is a plain push script, under modern flags it demands witness data.
    Txid coinbase1{};
    Txid coinbase31{};
    for (int height{1}; height <= H - 1; ++height) {
        const auto submitted{CodecRoundTrip(build_legacy(tip(), {}))};
        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(submitted, true, true, &new_block));
        if (height == 1) coinbase1 = submitted->vtx[0]->GetHash();
        if (height == 31) coinbase31 = submitted->vtx[0]->GetHash();
    }
    const CScript witness_shaped{CScript() << OP_0 << std::vector<unsigned char>(32, 0xab)};
    CMutableTransaction lock_spend;
    lock_spend.version = 1;
    lock_spend.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 17);
    lock_spend.vin.resize(1);
    lock_spend.vin[0].prevout = COutPoint{coinbase1, 0};
    lock_spend.vin[0].scriptSig = CScript{};
    lock_spend.vout.emplace_back(legacy::GetProofOfWorkReward(0, 1, consensus), witness_shaped);
    const auto block_h{CodecRoundTrip(build_legacy(tip(), {lock_spend}))};
    const Txid witness_shaped_txid{block_h->vtx[1]->GetHash()};
    {
        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(block_h, true, true, &new_block));
    }
    BOOST_REQUIRE_EQUAL(tip()->nHeight, H);

    mutable_consensus.hard_fork_height = H + 1;
    mutable_consensus.legacy_final_hash = tip()->GetBlockHash();
    mutable_consensus.transition_pow_length = 8;
    mutable_consensus.transition_pow_bits = EASY_BITS;
    mutable_consensus.transition_pow_reward = 0; // ratified fees-only, stated explicitly

    const auto build_corridor{[&](const CBlockIndex* prev, std::vector<CMutableTransaction> txs) {
        CMutableTransaction coinbase;
        coinbase.version = 2;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << CScriptNum{prev->nHeight + 1} << CScriptNum{7};
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 60);
        block.nBits = EASY_BITS;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        block.nNonce = 0;
        while (!CheckTransitionPowEligibility(block)) ++block.nNonce;
        return block;
    }};

    // Corridor block H+1: a MODERN transaction spends the witness-shaped
    // legacy coin with an empty scriptSig — valid ONLY under the frozen
    // legacy rule set — and a matured legacy coinbase, creating ordinary
    // modern outputs (the OWNER policy view).
    const CScript owner_script{CScript() << std::vector<unsigned char>(24, 0xc4) << OP_DROP << OP_TRUE};
    CMutableTransaction crossing;
    crossing.version = 2;
    crossing.vin.resize(1);
    crossing.vin[0].prevout = COutPoint{witness_shaped_txid, 0};
    crossing.vin[0].scriptSig = CScript{};
    crossing.vout.emplace_back(legacy::GetProofOfWorkReward(0, 1, consensus) - 1000, owner_script);
    const Txid crossing_txid{CTransaction{crossing}.GetHash()};
    {
        CBlock block{build_corridor(tip(), {crossing})};
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlock(std::make_shared<const CBlock>(block), true, true, &new_block),
                              "crossing spend refused");
        BOOST_REQUIRE(new_block);
        BOOST_REQUIRE_EQUAL(tip()->nHeight, H + 1);
    }
    {
        LOCK(cs_main);
        CCoinsViewCache& coins{chainman.ActiveChainstate().CoinsTip()};
        BOOST_CHECK(!coins.HaveCoin(COutPoint{witness_shaped_txid, 0}));
        const Coin& owner{coins.AccessCoin(COutPoint{crossing_txid, 0})};
        BOOST_REQUIRE(!owner.IsSpent());
        BOOST_CHECK(owner.out.scriptPubKey == owner_script);
        BOOST_CHECK(!owner.fCoinBase);
        BOOST_CHECK_EQUAL(owner.nHeight, static_cast<uint32_t>(H + 1));
    }

    // Control: the SAME witness-shaped script created as a MODERN coin is
    // governed by modern rules — an empty-scriptSig spend is refused.
    CMutableTransaction make_modern_witness_shaped;
    make_modern_witness_shaped.version = 2;
    make_modern_witness_shaped.vin.resize(1);
    make_modern_witness_shaped.vin[0].prevout = COutPoint{crossing_txid, 0};
    make_modern_witness_shaped.vin[0].scriptSig = CScript{};
    make_modern_witness_shaped.vout.emplace_back(
        legacy::GetProofOfWorkReward(0, 1, consensus) - 2000, witness_shaped);
    const Txid modern_ws_txid{CTransaction{make_modern_witness_shaped}.GetHash()};
    {
        CBlock block{build_corridor(tip(), {make_modern_witness_shaped})};
        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(std::make_shared<const CBlock>(block), true, true, &new_block));
        BOOST_REQUIRE_EQUAL(tip()->nHeight, H + 2);
    }
    {
        CMutableTransaction bad_spend;
        bad_spend.version = 2;
        bad_spend.vin.resize(1);
        bad_spend.vin[0].prevout = COutPoint{modern_ws_txid, 0};
        bad_spend.vin[0].scriptSig = CScript{};
        bad_spend.vout.emplace_back(1000, owner_script);
        CBlock block{build_corridor(tip(), {bad_spend})};
        LOCK(cs_main);
        const BlockValidationState state{TestBlockValidity(
            chainman.ActiveChainstate(), block, /*check_pow=*/true, /*check_merkle_root=*/true)};
        BOOST_REQUIRE(state.IsInvalid());
        BOOST_CHECK(state.GetRejectReason().find("block-script-verify-flag-failed") != std::string::npos);
    }

    // The frozen legacy maturity rides with legacy coins: coinbase 31 has
    // depth 3 < 30 at H + 3 and is refused; after enough corridor burial the
    // same spend connects.
    CMutableTransaction premature;
    premature.version = 2;
    premature.vin.resize(1);
    premature.vin[0].prevout = COutPoint{coinbase31, 0};
    premature.vin[0].scriptSig = CScript{};
    premature.vout.emplace_back(legacy::GetProofOfWorkReward(0, 31, consensus) - 1000, owner_script);
    {
        CBlock block{build_corridor(tip(), {premature})};
        LOCK(cs_main);
        const BlockValidationState state{TestBlockValidity(
            chainman.ActiveChainstate(), block, /*check_pow=*/true, /*check_merkle_root=*/true)};
        BOOST_REQUIRE(state.IsInvalid());
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-premature-spend-of-legacy-coin");
    }
    // Bury to depth >= 30 for coinbase 31 (spend height must reach 61) while
    // staying inside the corridor? Depth 30 needs height 61 > corridor end
    // (H + 8 = 40): instead prove the positive control with coinbase at
    // height 5, depth 37 at the next corridor height.
    {
        // Rebuild a positive-control spend of an old, deep coinbase.
        // coinbase1 is spent; use the block-2 coinbase via its known pattern:
        // heights 2..31 all hold OP_TRUE coinbases; fetch block 2's coinbase
        // from the chain index.
        const CBlockIndex* idx2{WITH_LOCK(cs_main, return chainman.ActiveChain()[2])};
        CBlock block2;
        BOOST_REQUIRE(chainman.m_blockman.ReadBlock(block2, *idx2));
        const Txid coinbase2{block2.vtx[0]->GetHash()};
        CMutableTransaction deep;
        deep.version = 2;
        deep.vin.resize(1);
        deep.vin[0].prevout = COutPoint{coinbase2, 0};
        deep.vin[0].scriptSig = CScript{};
        deep.vout.emplace_back(legacy::GetProofOfWorkReward(0, 2, consensus) - 1000, owner_script);
        CBlock block{build_corridor(tip(), {deep})};
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlock(std::make_shared<const CBlock>(block), true, true, &new_block),
                              "deep legacy coinbase crossing refused");
        BOOST_REQUIRE_EQUAL(tip()->nHeight, H + 3);
    }
}

//! STAKE policy in the corridor (stage 5): a real STAKE Policy Output is
//! created from legacy value crossing through LEGACY_LOCK, a malformed
//! STAKE-claiming output is a block consensus failure, and cancelling is an
//! ordinary owner spend of the principal (self-policing).
BOOST_AUTO_TEST_CASE(stake_policy_in_corridor)
{
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    auto& mutable_consensus{const_cast<Consensus::Params&>(consensus)};

    constexpr int H{32};
    constexpr uint32_t EASY_BITS{0x207fffff};
    const auto tip{[&] { return WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()); }};

    const auto build_legacy{[&](const CBlockIndex* prev, std::vector<CMutableTransaction> txs) {
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (prev->nHeight + 1) << CScriptNum{99};
        coinbase.vout.emplace_back(legacy::GetProofOfWorkReward(0, prev->nHeight + 1, consensus),
                                   CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        block.nBits = legacy::GetNextTargetRequired(prev, /*proof_of_stake=*/false, consensus);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) {
            mtx.m_legacy_encoding = true;
            block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        while (UintToArith256(block.GetLegacyB3Hash()) > target) ++block.nNonce;
        return block;
    }};
    const auto build_corridor{[&](const CBlockIndex* prev, std::vector<CMutableTransaction> txs) {
        CMutableTransaction coinbase;
        coinbase.version = 2;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << CScriptNum{prev->nHeight + 1} << CScriptNum{7};
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 60);
        block.nBits = EASY_BITS;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        block.nNonce = 0;
        while (!CheckTransitionPowEligibility(block)) ++block.nNonce;
        return block;
    }};

    // Legacy history; H carries a spend of the matured coinbase 1 into a
    // plain legacy value coin (non-coinbase: no crossing maturity).
    Txid coinbase1{};
    for (int height{1}; height <= H - 1; ++height) {
        const auto submitted{CodecRoundTrip(build_legacy(tip(), {}))};
        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(submitted, true, true, &new_block));
        if (height == 1) coinbase1 = submitted->vtx[0]->GetHash();
    }
    CMutableTransaction fund;
    fund.version = 1;
    fund.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 17);
    fund.vin.resize(1);
    fund.vin[0].prevout = COutPoint{coinbase1, 0};
    fund.vin[0].scriptSig = CScript{};
    fund.vout.emplace_back(legacy::GetProofOfWorkReward(0, 1, consensus), CScript() << OP_TRUE);
    const auto block_h{CodecRoundTrip(build_legacy(tip(), {fund}))};
    const Txid fund_txid{block_h->vtx[1]->GetHash()};
    {
        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(block_h, true, true, &new_block));
    }
    mutable_consensus.hard_fork_height = H + 1;
    mutable_consensus.legacy_final_hash = tip()->GetBlockHash();
    mutable_consensus.transition_pow_length = 6;
    mutable_consensus.transition_pow_bits = EASY_BITS;
    mutable_consensus.transition_pow_reward = 0; // ratified fees-only, stated explicitly
    mutable_consensus.min_stake_amount = 1000; // regtest scaffolding

    // Corridor block 1: legacy value crosses into a real STAKE output plus
    // ordinary change.
    std::array<unsigned char, 32> validator_key{};
    validator_key.fill(0x42);
    const CScript owner{CScript() << OP_TRUE};
    const CAmount principal{legacy::GetProofOfWorkReward(0, 1, consensus) / 2};
    CMutableTransaction stake_tx;
    stake_tx.version = 2;
    stake_tx.vin.resize(1);
    stake_tx.vin[0].prevout = COutPoint{fund_txid, 0};
    stake_tx.vin[0].scriptSig = CScript{};
    stake_tx.vout.emplace_back(principal, modern::MakeStakeScript(validator_key, owner));
    stake_tx.vout.emplace_back(legacy::GetProofOfWorkReward(0, 1, consensus) - principal - 1000,
                               CScript() << OP_TRUE);
    const Txid stake_txid{CTransaction{stake_tx}.GetHash()};
    {
        CBlock block{build_corridor(tip(), {stake_tx})};
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlock(std::make_shared<const CBlock>(block), true, true, &new_block),
                              "stake creation refused");
        BOOST_REQUIRE_EQUAL(tip()->nHeight, H + 1);
    }
    {
        LOCK(cs_main);
        const Coin& stake{chainman.ActiveChainstate().CoinsTip().AccessCoin(COutPoint{stake_txid, 0})};
        BOOST_REQUIRE(!stake.IsSpent());
        BOOST_REQUIRE(modern::ClaimsStakeMagic(stake.out.scriptPubKey));
        std::string error;
        const auto view{modern::ParseStakeOutput(stake.out, error)};
        BOOST_REQUIRE_MESSAGE(view.has_value(), error);
        BOOST_CHECK_EQUAL(view->amount, principal);
        BOOST_CHECK(view->validator_key == validator_key);
        BOOST_CHECK(view->owner_script == owner);
    }

    // A malformed STAKE claim (zero validator key) is a block failure.
    {
        CMutableTransaction bad;
        bad.version = 2;
        bad.vin.resize(1);
        bad.vin[0].prevout = COutPoint{stake_txid, 1};
        bad.vin[0].scriptSig = CScript{};
        bad.vout.emplace_back(1000, modern::MakeStakeScript(std::array<unsigned char, 32>{}, owner));
        CBlock block{build_corridor(tip(), {bad})};
        LOCK(cs_main);
        const BlockValidationState state{TestBlockValidity(
            chainman.ActiveChainstate(), block, /*check_pow=*/true, /*check_merkle_root=*/true)};
        BOOST_REQUIRE(state.IsInvalid());
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-stake-output");
    }

    // Cancelling is an ordinary owner spend of the principal.
    {
        CMutableTransaction cancel;
        cancel.version = 2;
        cancel.vin.resize(1);
        cancel.vin[0].prevout = COutPoint{stake_txid, 0};
        cancel.vin[0].scriptSig = CScript{};
        cancel.vout.emplace_back(principal - 1000, CScript() << OP_TRUE);
        CBlock block{build_corridor(tip(), {cancel})};
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlock(std::make_shared<const CBlock>(block), true, true, &new_block),
                              "stake cancel refused");
        BOOST_REQUIRE_EQUAL(tip()->nHeight, H + 2);
        LOCK(cs_main);
        BOOST_CHECK(!chainman.ActiveChainstate().CoinsTip().HaveCoin(COutPoint{stake_txid, 0}));
    }
}

//! The complete transition corridor end to end (stage 8): a live legacy
//! chain to H, the frozen boundary, all 1,000 temporary-PoW corridor blocks
//! produced and connected, real STAKE outputs created from crossing legacy
//! value (including a split-stake validator proving e2e aggregation and a
//! late stake that stays PENDING), the registry derived at H+1000, and the
//! first attempted modern-PoS block at H+1001 stopped by the deliberate
//! fail-closed no-modern-pos-rules gate with the chain exactly at the
//! corridor end.
BOOST_AUTO_TEST_CASE(full_corridor_end_to_end)
{
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    auto& mutable_consensus{const_cast<Consensus::Params&>(consensus)};

    constexpr int H{32};
    constexpr int CORRIDOR{1000};
    constexpr uint32_t EASY_BITS{0x207fffff};
    const auto tip{[&] { return WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()); }};

    const auto build_legacy{[&](const CBlockIndex* prev, std::vector<CMutableTransaction> txs) {
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (prev->nHeight + 1) << CScriptNum{99};
        coinbase.vout.emplace_back(legacy::GetProofOfWorkReward(0, prev->nHeight + 1, consensus),
                                   CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        block.nBits = legacy::GetNextTargetRequired(prev, /*proof_of_stake=*/false, consensus);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) {
            mtx.m_legacy_encoding = true;
            block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        while (UintToArith256(block.GetLegacyB3Hash()) > target) ++block.nNonce;
        return block;
    }};
    const auto build_corridor{[&](const CBlockIndex* prev, std::vector<CMutableTransaction> txs) {
        CMutableTransaction coinbase;
        coinbase.version = 2;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << CScriptNum{prev->nHeight + 1} << CScriptNum{7};
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 60);
        block.nBits = EASY_BITS;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        block.nNonce = 0;
        while (!CheckTransitionPowEligibility(block)) ++block.nNonce;
        return block;
    }};
    const auto submit_corridor{[&](std::vector<CMutableTransaction> txs) {
        SetMockTime(std::max<int64_t>(MOCK_NOW, tip()->GetBlockTime() + 60));
        CBlock block{build_corridor(tip(), std::move(txs))};
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlock(std::make_shared<const CBlock>(block), true, true, &new_block),
                              "corridor block at height " << tip()->nHeight + 1 << " rejected");
        BOOST_REQUIRE(new_block);
    }};
    // Mining-path checkpoint: the ASSEMBLER produces the corridor block —
    // marker-modern template with the corridor difficulty and a
    // fees-plus-corridor-reward coinbase — then the shared scrypt grind and
    // the ordinary submit path connect it.
    const auto assemble_and_submit{[&](const CAmount expected_fees) {
        SetMockTime(std::max<int64_t>(MOCK_NOW, tip()->GetBlockTime() + 60));
        node::BlockAssembler::Options options;
        options.coinbase_output_script = CScript() << OP_TRUE;
        options.include_dummy_extranonce = true;
        const auto tmpl{node::BlockAssembler(chainman.ActiveChainstate(), m_node.mempool.get(), options)
                            .CreateNewBlock()};
        BOOST_REQUIRE(tmpl);
        CBlock block{tmpl->block};
        BOOST_CHECK(Consensus::HasB3BlockCodecV2(block.nVersion));
        BOOST_CHECK_EQUAL(block.nBits, EASY_BITS);
        BOOST_CHECK_EQUAL(block.vtx[0]->GetValueOut(), expected_fees); // fees only, reward param 0
        block.hashMerkleRoot = BlockMerkleRoot(block);
        block.nNonce = 0;
        while (!CheckTransitionPowEligibility(block)) ++block.nNonce;
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlock(std::make_shared<const CBlock>(block), true, true, &new_block),
                              "assembled corridor block at height " << tip()->nHeight + 1 << " rejected");
        BOOST_REQUIRE(new_block);
    }};

    // ---- Phase A: live legacy to H, with two crossing-fund outputs.
    Txid coinbase1{};
    for (int height{1}; height <= H - 1; ++height) {
        const auto submitted{CodecRoundTrip(build_legacy(tip(), {}))};
        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(submitted, true, true, &new_block));
        if (height == 1) coinbase1 = submitted->vtx[0]->GetHash();
    }
    const CAmount fund_total{legacy::GetProofOfWorkReward(0, 1, consensus)};
    CMutableTransaction fund;
    fund.version = 1;
    fund.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 17);
    fund.vin.resize(1);
    fund.vin[0].prevout = COutPoint{coinbase1, 0};
    fund.vin[0].scriptSig = CScript{};
    fund.vout.emplace_back(fund_total / 2, CScript() << OP_TRUE);
    fund.vout.emplace_back(fund_total / 2, CScript() << OP_TRUE);
    const auto block_h{CodecRoundTrip(build_legacy(tip(), {fund}))};
    const Txid fund_txid{block_h->vtx[1]->GetHash()};
    {
        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(block_h, true, true, &new_block));
    }
    BOOST_REQUIRE_EQUAL(tip()->nHeight, H);

    // ---- Freeze the boundary; configure the full 1,000-block corridor.
    const uint256 X{tip()->GetBlockHash()};
    mutable_consensus.hard_fork_height = H + 1;
    mutable_consensus.legacy_final_hash = X;
    mutable_consensus.transition_pow_length = CORRIDOR;
    mutable_consensus.transition_pow_bits = EASY_BITS;
    mutable_consensus.transition_pow_reward = 0; // ratified fees-only, stated explicitly
    mutable_consensus.min_stake_amount = 1000; // regtest scaffolding
    BOOST_REQUIRE(consensus.test_only_modern_pos_validator == nullptr);
    BOOST_CHECK(WITH_LOCK(cs_main, return chainman.ActiveChainstate().LegacyBoundaryActive()));

    // ---- Phase B: the corridor. Block H+1 stakes validator A in a single
    // output; block H+2 stakes validator B with the SAME total split in
    // three; the bulk of the corridor is plain temporary-PoW blocks; a late
    // stake for validator C lands 5 blocks before the end and must stay
    // immature at the handoff.
    std::array<unsigned char, 32> key_a{};
    key_a.fill(0xaa);
    std::array<unsigned char, 32> key_b{};
    key_b.fill(0xbb);
    std::array<unsigned char, 32> key_c{};
    key_c.fill(0xcc);
    const CScript owner{CScript() << OP_TRUE};
    const CAmount stake_total{900'000};

    CMutableTransaction stake_a;
    stake_a.version = 2;
    stake_a.vin.resize(1);
    stake_a.vin[0].prevout = COutPoint{fund_txid, 0};
    stake_a.vin[0].scriptSig = CScript{};
    stake_a.vout.emplace_back(stake_total, modern::MakeStakeScript(key_a, owner));
    stake_a.vout.emplace_back(fund_total / 2 - stake_total - 1000, CScript() << OP_TRUE);
    {
        // H+1 through the full production path: mempool admission of the
        // crossing STAKE transaction, template creation, grind, submit.
        const auto res{WITH_LOCK(cs_main, return chainman.ProcessTransaction(MakeTransactionRef(stake_a)))};
        BOOST_REQUIRE_MESSAGE(res.m_result_type == MempoolAcceptResult::ResultType::VALID,
                              "stake_a mempool admission failed: " << res.m_state.ToString());
        assemble_and_submit(/*expected_fees=*/1000);
        BOOST_REQUIRE_EQUAL(tip()->nHeight, H + 1);
        BOOST_CHECK_EQUAL(m_node.mempool->size(), 0U);
    }

    CMutableTransaction stake_b;
    stake_b.version = 2;
    stake_b.vin.resize(1);
    stake_b.vin[0].prevout = COutPoint{fund_txid, 1};
    stake_b.vin[0].scriptSig = CScript{};
    stake_b.vout.emplace_back(stake_total / 3, modern::MakeStakeScript(key_b, owner));
    stake_b.vout.emplace_back(stake_total / 3, modern::MakeStakeScript(key_b, owner));
    stake_b.vout.emplace_back(stake_total - 2 * (stake_total / 3), modern::MakeStakeScript(key_b, owner));
    stake_b.vout.emplace_back(fund_total / 2 - stake_total - 1000, CScript() << OP_TRUE);
    const Txid change_b_txid{CTransaction{stake_b}.GetHash()};
    submit_corridor({stake_b}); // H+2

    while (tip()->nHeight < H + 499) submit_corridor({});
    assemble_and_submit(/*expected_fees=*/0); // H+500 via the mining path
    BOOST_REQUIRE_EQUAL(tip()->nHeight, H + 500);
    while (tip()->nHeight < H + CORRIDOR - 5) submit_corridor({});

    CMutableTransaction stake_c;
    stake_c.version = 2;
    stake_c.vin.resize(1);
    stake_c.vin[0].prevout = COutPoint{change_b_txid, 3};
    stake_c.vin[0].scriptSig = CScript{};
    stake_c.vout.emplace_back(100'000, modern::MakeStakeScript(key_c, owner));
    submit_corridor({stake_c}); // H+996

    while (tip()->nHeight < H + CORRIDOR - 1) submit_corridor({});
    assemble_and_submit(/*expected_fees=*/0); // H+1000 via the mining path
    BOOST_REQUIRE_EQUAL(tip()->nHeight, H + CORRIDOR);

    // ---- The registry at the corridor end: A and B carry the identical
    // aggregated weight (split == single, end to end); C is immature.
    {
        LOCK(cs_main);
        chainman.ActiveChainstate().ForceFlushStateToDisk();
        const node::StakeRegistry registry{node::DeriveStakeRegistry(
            chainman.ActiveChainstate().CoinsDB(), H + CORRIDOR, consensus)};
        BOOST_REQUIRE_EQUAL(registry.validators.size(), 3U);
        BOOST_CHECK_EQUAL(registry.validators.at(key_a).total_weight, stake_total);
        BOOST_CHECK_EQUAL(registry.validators.at(key_b).total_weight, stake_total);
        BOOST_CHECK_EQUAL(registry.validators.at(key_b).outputs.size(), 3U);
        BOOST_CHECK_EQUAL(registry.total_weight, 2 * stake_total);
        BOOST_CHECK_EQUAL(registry.mature_outputs, 4U);
        BOOST_CHECK_EQUAL(registry.immature_outputs, 1U); // validator C
        BOOST_CHECK_EQUAL(registry.validators.at(key_c).total_weight, 0); // PENDING, attributed
        BOOST_REQUIRE_EQUAL(registry.validators.at(key_c).outputs.size(), 1U);
        BOOST_CHECK(!registry.validators.at(key_c).outputs[0].active);
    }

    // ---- Phase C begins: H+1001 is modern PoS, and with no rule set
    // installed the fail-closed gate stops everything, deterministically.
    {
        CBlock modern_attempt{build_corridor(tip(), {})};
        modern_attempt.nBits = GetNextWorkRequired(tip(), &modern_attempt, consensus);
        const arith_uint256 sha_target{arith_uint256().SetCompact(modern_attempt.nBits)};
        modern_attempt.nNonce = 0;
        while (UintToArith256(modern_attempt.GetHash()) > sha_target) ++modern_attempt.nNonce;
        {
            LOCK(cs_main);
            const BlockValidationState state{TestBlockValidity(
                chainman.ActiveChainstate(), modern_attempt, /*check_pow=*/true, /*check_merkle_root=*/true)};
            BOOST_REQUIRE(state.IsInvalid());
            BOOST_CHECK_EQUAL(state.GetRejectReason(), "no-modern-pos-rules");
        }
        bool new_block{false};
        chainman.ProcessNewBlock(std::make_shared<const CBlock>(modern_attempt), true, true, &new_block);
        LOCK(cs_main);
        const CBlockIndex* pindex{chainman.m_blockman.LookupBlockIndex(modern_attempt.GetHash())};
        BOOST_REQUIRE(pindex != nullptr);
        BOOST_CHECK(pindex->nStatus & BLOCK_FAILED_VALID);
        BOOST_CHECK(!chainman.ActiveChain().Contains(pindex));
    }
    // A corridor-style block past the corridor end can never CONNECT: the
    // placeholder post-corridor header rules may store it (on regtest the
    // min-difficulty walk-back can even legitimize the corridor bits), but
    // the phase dispatch sends it to the fail-closed modern gate and the tip
    // never moves. A legacy-codec block is refused outright by the era codec
    // switch.
    {
        CBlock stale_corridor{build_corridor(tip(), {})};
        // Distinct time so it is not a duplicate of the failed attempt above.
        stale_corridor.nTime += 1;
        stale_corridor.nNonce = 0;
        while (!CheckTransitionPowEligibility(stale_corridor)) ++stale_corridor.nNonce;
        bool new_block{false};
        chainman.ProcessNewBlock(std::make_shared<const CBlock>(stale_corridor), true, true, &new_block);
        {
            LOCK(cs_main);
            BOOST_CHECK_EQUAL(chainman.ActiveChain().Tip()->nHeight, H + CORRIDOR);
            if (const CBlockIndex* pindex{chainman.m_blockman.LookupBlockIndex(stale_corridor.GetHash())}) {
                BOOST_CHECK(pindex->nStatus & BLOCK_FAILED_VALID);
                BOOST_CHECK(!chainman.ActiveChain().Contains(pindex));
            }
        }
        CBlock stale_legacy{build_legacy(tip(), {})};
        BOOST_CHECK(!chainman.ProcessNewBlock(CodecRoundTrip(stale_legacy), true, true, &new_block));
        BOOST_CHECK(WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(
                                  stale_legacy.GetLegacyB3Hash())) == nullptr);
    }
    // The chain holds exactly at the corridor end, boundary intact.
    {
        LOCK(cs_main);
        BOOST_CHECK(chainman.ActiveChainstate().LegacyBoundaryActive());
        BOOST_CHECK_EQUAL(chainman.ActiveChain().Tip()->nHeight, H + CORRIDOR);
    }
}

//! Corridor restart and chainstate-reindex correctness with a NON-trivial
//! corridor target: the persisted block index reloads through the
//! phase-aware proof check (the old two-state code ran SHA256d over
//! corridor entries and would fail here with overwhelming probability),
//! the chain continues after the restart, and a chainstate rebuild
//! reconnects the whole legacy+corridor history to the same tip and
//! registry.
BOOST_FIXTURE_TEST_CASE(corridor_restart_and_reindex, TransitionDiskSetup)
{
    const Consensus::Params& consensus{m_node.chainman->GetConsensus()};
    auto& mutable_consensus{const_cast<Consensus::Params&>(consensus)};

    constexpr int H{32};
    // Non-trivial corridor target: a random SHA256d identity hash fails it
    // with probability ~255/256 per block, while the scrypt grind satisfies
    // it deterministically. Six corridor blocks make the old SHA256d
    // restart bug fail with probability ~1 - 2^-48.
    constexpr uint32_t HARD_BITS{0x2000ffff};
    const auto tip{[&] {
        return WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip());
    }};

    const auto build_legacy{[&](const CBlockIndex* prev, std::vector<CMutableTransaction> txs) {
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (prev->nHeight + 1) << CScriptNum{99};
        coinbase.vout.emplace_back(legacy::GetProofOfWorkReward(0, prev->nHeight + 1, consensus),
                                   CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        block.nBits = legacy::GetNextTargetRequired(prev, /*proof_of_stake=*/false, consensus);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) {
            mtx.m_legacy_encoding = true;
            block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        while (UintToArith256(block.GetLegacyB3Hash()) > target) ++block.nNonce;
        return block;
    }};
    const auto build_corridor{[&](const CBlockIndex* prev, std::vector<CMutableTransaction> txs) {
        CMutableTransaction coinbase;
        coinbase.version = 2;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << CScriptNum{prev->nHeight + 1} << CScriptNum{7};
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 60);
        block.nBits = HARD_BITS;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        block.nNonce = 0;
        while (!CheckTransitionPowEligibility(block)) ++block.nNonce;
        return block;
    }};

    // Legacy history with a crossing-fund output at H, boundary pinned,
    // corridor with the hard target, one STAKE inside the corridor.
    Txid coinbase1{};
    for (int height{1}; height <= H - 1; ++height) {
        const auto submitted{CodecRoundTrip(build_legacy(tip(), {}))};
        bool new_block{false};
        BOOST_REQUIRE(m_node.chainman->ProcessNewBlock(submitted, true, true, &new_block));
        if (height == 1) coinbase1 = submitted->vtx[0]->GetHash();
    }
    CMutableTransaction fund;
    fund.version = 1;
    fund.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 17);
    fund.vin.resize(1);
    fund.vin[0].prevout = COutPoint{coinbase1, 0};
    fund.vin[0].scriptSig = CScript{};
    fund.vout.emplace_back(legacy::GetProofOfWorkReward(0, 1, consensus), CScript() << OP_TRUE);
    const auto block_h{CodecRoundTrip(build_legacy(tip(), {fund}))};
    const Txid fund_txid{block_h->vtx[1]->GetHash()};
    {
        bool new_block{false};
        BOOST_REQUIRE(m_node.chainman->ProcessNewBlock(block_h, true, true, &new_block));
    }
    mutable_consensus.hard_fork_height = H + 1;
    mutable_consensus.legacy_final_hash = tip()->GetBlockHash();
    mutable_consensus.transition_pow_length = 8;
    mutable_consensus.transition_pow_bits = HARD_BITS;
    mutable_consensus.transition_pow_reward = 0; // ratified fees-only, stated explicitly
    mutable_consensus.min_stake_amount = 1000;

    std::array<unsigned char, 32> validator_key{};
    validator_key.fill(0x42);
    CMutableTransaction stake_tx;
    stake_tx.version = 2;
    stake_tx.vin.resize(1);
    stake_tx.vin[0].prevout = COutPoint{fund_txid, 0};
    stake_tx.vin[0].scriptSig = CScript{};
    stake_tx.vout.emplace_back(200'000, modern::MakeStakeScript(validator_key, CScript() << OP_TRUE));
    for (int i{0}; i < 6; ++i) {
        bool new_block{false};
        CBlock block{build_corridor(tip(), i == 0 ? std::vector<CMutableTransaction>{stake_tx}
                                                  : std::vector<CMutableTransaction>{})};
        BOOST_REQUIRE_MESSAGE(
            m_node.chainman->ProcessNewBlock(std::make_shared<const CBlock>(block), true, true, &new_block),
            "corridor block at height " << tip()->nHeight + 1 << " rejected");
    }
    const int pre_restart_height{tip()->nHeight};
    const uint256 pre_restart_hash{tip()->GetBlockHash()};
    BOOST_REQUIRE_EQUAL(pre_restart_height, H + 6);

    // ---- Simulated shutdown + restart: tear the chainman down and rebuild
    // it over the persisted databases. LoadBlockIndexGuts must accept every
    // corridor entry through the scrypt eligibility check.
    {
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().ForceFlushStateToDisk();
    }
    m_node.chainman.reset();
    m_make_chainman();
    LoadVerifyActivateChainstate();
    BOOST_REQUIRE_EQUAL(tip()->nHeight, pre_restart_height);
    BOOST_CHECK_EQUAL(tip()->GetBlockHash().GetHex(), pre_restart_hash.GetHex());

    // The chain continues after the restart.
    {
        bool new_block{false};
        CBlock block{build_corridor(tip(), {})};
        BOOST_REQUIRE(m_node.chainman->ProcessNewBlock(std::make_shared<const CBlock>(block), true, true, &new_block));
        BOOST_REQUIRE_EQUAL(tip()->nHeight, pre_restart_height + 1);
    }

    // ---- Chainstate reindex: wipe the chainstate database and rebuild it
    // by reconnecting the entire legacy + corridor history from the block
    // files, reaching the same tip with the same derived registry.
    {
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().ForceFlushStateToDisk();
    }
    m_node.chainman.reset();
    m_args.ForceSetArg("-reindex-chainstate", "1");
    m_make_chainman();
    LoadVerifyActivateChainstate();
    m_args.ForceSetArg("-reindex-chainstate", "0");
    BOOST_REQUIRE_EQUAL(tip()->nHeight, pre_restart_height + 1);
    {
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().ForceFlushStateToDisk();
        const node::StakeRegistry registry{node::DeriveStakeRegistry(
            m_node.chainman->ActiveChainstate().CoinsDB(), tip()->nHeight, consensus)};
        // Created at H+1, evaluated at H+7: attributed but PENDING (depth
        // 6 < STAKE_ACTIVATION_DEPTH) with zero weight — the reindex
        // reproduced the exact registry state, not merely the tip.
        BOOST_REQUIRE_EQUAL(registry.validators.size(), 1U);
        const node::ValidatorRecord& rec{registry.validators.at(validator_key)};
        BOOST_CHECK_EQUAL(rec.total_weight, 0);
        BOOST_REQUIRE_EQUAL(rec.outputs.size(), 1U);
        BOOST_CHECK(!rec.outputs[0].active);
        BOOST_CHECK_EQUAL(rec.outputs[0].amount, 200'000);
        BOOST_CHECK_EQUAL(rec.outputs[0].creation_height, H + 1);
    }
}

//! Two-node corridor sync: node A builds the evolution chain (legacy
//! history, frozen boundary, corridor with STAKE creation); node B starts
//! from nothing but the same consensus parameters and syncs block by block
//! through the transition — exercising exactly the pinned-boundary path a
//! fresh node takes. Same tip, same UTXO commitment, same derived registry.
BOOST_AUTO_TEST_CASE(two_node_corridor_sync)
{
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    auto& mutable_consensus{const_cast<Consensus::Params&>(consensus)};

    constexpr int H{32};
    constexpr int CORRIDOR{40};
    constexpr uint32_t EASY_BITS{0x207fffff};
    const auto tip{[&] { return WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()); }};

    const auto build_legacy{[&](const CBlockIndex* prev, std::vector<CMutableTransaction> txs) {
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (prev->nHeight + 1) << CScriptNum{99};
        coinbase.vout.emplace_back(legacy::GetProofOfWorkReward(0, prev->nHeight + 1, consensus),
                                   CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        block.nBits = legacy::GetNextTargetRequired(prev, /*proof_of_stake=*/false, consensus);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) {
            mtx.m_legacy_encoding = true;
            block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        while (UintToArith256(block.GetLegacyB3Hash()) > target) ++block.nNonce;
        return block;
    }};
    const auto build_corridor{[&](const CBlockIndex* prev, std::vector<CMutableTransaction> txs) {
        CMutableTransaction coinbase;
        coinbase.version = 2;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << CScriptNum{prev->nHeight + 1} << CScriptNum{7};
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 60);
        block.nBits = EASY_BITS;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        block.nNonce = 0;
        while (!CheckTransitionPowEligibility(block)) ++block.nNonce;
        return block;
    }};

    // ---- Node A builds the chain: legacy history with a crossing fund,
    // frozen boundary, corridor with a STAKE and its cancellation-free life.
    Txid coinbase1{};
    for (int height{1}; height <= H - 1; ++height) {
        const auto submitted{CodecRoundTrip(build_legacy(tip(), {}))};
        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(submitted, true, true, &new_block));
        if (height == 1) coinbase1 = submitted->vtx[0]->GetHash();
    }
    CMutableTransaction fund;
    fund.version = 1;
    fund.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 17);
    fund.vin.resize(1);
    fund.vin[0].prevout = COutPoint{coinbase1, 0};
    fund.vin[0].scriptSig = CScript{};
    fund.vout.emplace_back(legacy::GetProofOfWorkReward(0, 1, consensus), CScript() << OP_TRUE);
    const auto block_h{CodecRoundTrip(build_legacy(tip(), {fund}))};
    const Txid fund_txid{block_h->vtx[1]->GetHash()};
    {
        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(block_h, true, true, &new_block));
    }
    mutable_consensus.hard_fork_height = H + 1;
    mutable_consensus.legacy_final_hash = tip()->GetBlockHash();
    mutable_consensus.transition_pow_length = CORRIDOR;
    mutable_consensus.transition_pow_bits = EASY_BITS;
    mutable_consensus.transition_pow_reward = 0; // ratified fees-only, stated explicitly
    mutable_consensus.min_stake_amount = 1000;

    std::array<unsigned char, 32> validator_key{};
    validator_key.fill(0x77);
    CMutableTransaction stake_tx;
    stake_tx.version = 2;
    stake_tx.vin.resize(1);
    stake_tx.vin[0].prevout = COutPoint{fund_txid, 0};
    stake_tx.vin[0].scriptSig = CScript{};
    stake_tx.vout.emplace_back(300'000, modern::MakeStakeScript(validator_key, CScript() << OP_TRUE));
    for (int i{0}; i < CORRIDOR; ++i) {
        bool new_block{false};
        CBlock block{build_corridor(tip(), i == 0 ? std::vector<CMutableTransaction>{stake_tx}
                                                  : std::vector<CMutableTransaction>{})};
        BOOST_REQUIRE(chainman.ProcessNewBlock(std::make_shared<const CBlock>(block), true, true, &new_block));
    }
    BOOST_REQUIRE_EQUAL(tip()->nHeight, H + CORRIDOR);

    // ---- Node B: a fresh ChainstateManager over its own directories, same
    // (pinned) consensus parameters, fed the raw blocks in height order —
    // the fresh-sync path with the boundary active from genesis.
    const fs::path b_dir{m_args.GetDataDirNet() / "nodeB"};
    fs::create_directories(b_dir / "blocks");
    {
        ChainstateManager::Options b_chainman_opts{
            .chainparams = chainman.GetParams(),
            .datadir = b_dir,
            .check_block_index = 1,
            .notifications = *m_node.notifications,
            .worker_threads_num = 0,
        };
        const node::BlockManager::Options b_blockman_opts{
            .chainparams = b_chainman_opts.chainparams,
            .blocks_dir = b_dir / "blocks",
            .notifications = b_chainman_opts.notifications,
            .block_tree_db_params = DBParams{
                .path = b_dir / "blocks" / "index",
                .cache_bytes = m_kernel_cache_sizes.block_tree_db,
                .memory_only = true,
            },
        };
        ChainstateManager chainman_b{*Assert(m_node.shutdown_signal), b_chainman_opts, b_blockman_opts};
        {
            node::ChainstateLoadOptions b_load;
            b_load.mempool = nullptr;
            b_load.coins_db_in_memory = true;
            const auto [status, error]{node::LoadChainstate(chainman_b, m_kernel_cache_sizes, b_load)};
            BOOST_REQUIRE_MESSAGE(status == node::ChainstateLoadStatus::SUCCESS, error.original);
            BlockValidationState state;
            BOOST_REQUIRE(chainman_b.ActiveChainstate().ActivateBestChain(state));
        }
        BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainman_b.ActiveChain().Height()), 0);

        for (int height{1}; height <= H + CORRIDOR; ++height) {
            const CBlockIndex* pindex{WITH_LOCK(cs_main, return chainman.ActiveChain()[height])};
            BOOST_REQUIRE(pindex != nullptr);
            CBlock block;
            BOOST_REQUIRE(chainman.m_blockman.ReadBlock(block, *pindex));
            bool new_block{false};
            BOOST_REQUIRE_MESSAGE(
                chainman_b.ProcessNewBlock(std::make_shared<const CBlock>(block), true, true, &new_block),
                "node B refused block at height " << height);
            BOOST_REQUIRE(new_block);
        }

        // Same tip, same chainstate, same UTXO commitment, same registry.
        LOCK(cs_main);
        BOOST_REQUIRE_EQUAL(chainman_b.ActiveChain().Height(), H + CORRIDOR);
        BOOST_CHECK_EQUAL(chainman_b.ActiveChain().Tip()->GetBlockHash().GetHex(),
                          chainman.ActiveChain().Tip()->GetBlockHash().GetHex());
        chainman.ActiveChainstate().ForceFlushStateToDisk();
        chainman_b.ActiveChainstate().ForceFlushStateToDisk();
        const node::UtxoComparison cmp{node::CompareUtxoViews(
            chainman.ActiveChainstate().CoinsDB(), chainman_b.ActiveChainstate().CoinsDB())};
        BOOST_CHECK(cmp.Equal());
        BOOST_CHECK_EQUAL(cmp.commitment_a.GetHex(), cmp.commitment_b.GetHex());
        const node::StakeRegistry reg_a{node::DeriveStakeRegistry(
            chainman.ActiveChainstate().CoinsDB(), H + CORRIDOR, consensus)};
        const node::StakeRegistry reg_b{node::DeriveStakeRegistry(
            chainman_b.ActiveChainstate().CoinsDB(), H + CORRIDOR, consensus)};
        BOOST_CHECK(reg_a.WeightView() == reg_b.WeightView());
        BOOST_CHECK_EQUAL(reg_a.total_weight, reg_b.total_weight);
        BOOST_CHECK_EQUAL(reg_b.validators.at(validator_key).total_weight, 300'000);
        BOOST_CHECK_EQUAL(reg_a.mature_outputs, reg_b.mature_outputs);
    }
}

BOOST_AUTO_TEST_SUITE_END()
