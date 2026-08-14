// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <legacy/replay.h>

#include <coins.h>
#include <consensus/block_codec.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <legacy/codec.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <dbwrapper.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <undo.h>

#include <boost/test/unit_test.hpp>

#include <map>
#include <string>
#include <vector>

namespace {

//! Deliberately meaningless PoW/difficulty/timestamp values everywhere:
//! trusted replay must never look at them.

CMutableTransaction CoinbaseTx(const uint32_t ntime, const CAmount value, const uint32_t tag)
{
    CMutableTransaction mtx;
    mtx.version = 1;
    mtx.nTime = ntime;
    mtx.m_legacy_encoding = true;
    mtx.vin.resize(1);
    mtx.vin[0].prevout.SetNull();
    mtx.vin[0].scriptSig = CScript() << CScriptNum{static_cast<int64_t>(tag)};
    mtx.vout.emplace_back(value, CScript() << OP_TRUE);
    return mtx;
}

CBlock MakeBlock(const uint256& prev, const uint32_t ntime, std::vector<CMutableTransaction> txs)
{
    CBlock block;
    block.nVersion = 4;
    block.hashPrevBlock = prev;
    block.nTime = ntime;
    block.nBits = 0x12345678; // nonsense difficulty: skipped
    block.nNonce = 0;         // no proof of work: skipped
    for (CMutableTransaction& mtx : txs) {
        mtx.m_legacy_encoding = true;
        block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
    }
    block.hashMerkleRoot = BlockMerkleRoot(block);
    return block;
}

struct SyntheticChain {
    Consensus::Params params{};
    CBlock genesis;
    CBlock block1;
    CBlock block2;
    Txid genesis_coinbase;
    Txid spend_txid;
    Txid coinstake_txid;

    SyntheticChain()
    {
        genesis = MakeBlock(uint256{}, 1'000, {CoinbaseTx(1'000, 1000 * COIN, 0)});
        genesis_coinbase = genesis.vtx[0]->GetHash();

        params.legacy_b3coin = true;
        params.hashGenesisBlock = genesis.GetLegacyB3Hash();

        // Block 1: a plain transaction spending the (immature, unsigned)
        // genesis coinbase with a garbage scriptSig — scripts, signatures
        // and maturity are not replay's business.
        CMutableTransaction spend;
        spend.version = 1;
        spend.nTime = 1'100;
        spend.m_legacy_encoding = true;
        spend.vin.resize(1);
        spend.vin[0].prevout = COutPoint{genesis_coinbase, 0};
        spend.vin[0].scriptSig = CScript() << std::vector<unsigned char>{0xde, 0xad};
        spend.vout.emplace_back(400 * COIN, CScript() << OP_TRUE);
        spend.vout.emplace_back(100 * COIN, CScript() << OP_2);
        block1 = MakeBlock(params.hashGenesisBlock, 1'100,
                           {CoinbaseTx(1'100, 500 * COIN, 1), spend});
        spend_txid = block1.vtx[1]->GetHash();

        // Block 2: a coinstake-shaped transaction creating more value than
        // it spends — rewards are attested, not validated.
        CMutableTransaction stake;
        stake.version = 1;
        stake.nTime = 1'200;
        stake.m_legacy_encoding = true;
        stake.vin.resize(1);
        stake.vin[0].prevout = COutPoint{spend_txid, 0};
        stake.vin[0].scriptSig = CScript() << std::vector<unsigned char>{0xbe, 0xef};
        stake.vout.emplace_back(0, CScript{}); // coinstake marker output
        stake.vout.emplace_back(450 * COIN, CScript() << OP_TRUE);
        block2 = MakeBlock(block1.GetLegacyB3Hash(), 1'200,
                           {CoinbaseTx(1'200, 0, 2), stake});
        coinstake_txid = block2.vtx[1]->GetHash();
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(legacy_replay_tests)

BOOST_AUTO_TEST_CASE(replays_a_synthetic_chain_mechanically)
{
    const SyntheticChain chain;
    legacy::TrustedReplay replay{chain.params, /*final_height=*/2, {}};
    CCoinsView base;
    CCoinsViewCache view{&base};
    std::string error;

    BOOST_REQUIRE_MESSAGE(replay.ApplyBlock(chain.genesis, view, error), error);
    BOOST_REQUIRE_MESSAGE(replay.ApplyBlock(chain.block1, view, error), error);
    BOOST_REQUIRE_MESSAGE(replay.ApplyBlock(chain.block2, view, error), error);
    BOOST_CHECK_EQUAL(replay.NextHeight(), 3);
    BOOST_CHECK_EQUAL(replay.TipHash().GetHex(), chain.block2.GetLegacyB3Hash().GetHex());

    // Exact input removal: the genesis coinbase output is spent.
    BOOST_CHECK(!view.HaveCoin(COutPoint{chain.genesis_coinbase, 0}));

    // Exact output creation under the historical txid, preserving amount,
    // scriptPubKey, creation height, class and transaction time.
    const Coin& change{view.AccessCoin(COutPoint{chain.spend_txid, 1})};
    BOOST_REQUIRE(!change.IsSpent());
    BOOST_CHECK_EQUAL(change.out.nValue, 100 * COIN);
    BOOST_CHECK(change.out.scriptPubKey == (CScript() << OP_2));
    BOOST_CHECK_EQUAL(change.nHeight, 1U);
    BOOST_CHECK(!change.fCoinBase);
    BOOST_CHECK(!change.fCoinStake);
    BOOST_CHECK_EQUAL(change.nTime, 1'100U);

    const Coin& cb{view.AccessCoin(COutPoint{chain.block1.vtx[0]->GetHash(), 0})};
    BOOST_REQUIRE(!cb.IsSpent());
    BOOST_CHECK(cb.fCoinBase);
    BOOST_CHECK_EQUAL(cb.nHeight, 1U);

    const Coin& stake{view.AccessCoin(COutPoint{chain.coinstake_txid, 1})};
    BOOST_REQUIRE(!stake.IsSpent());
    BOOST_CHECK(stake.fCoinStake);
    BOOST_CHECK_EQUAL(stake.out.nValue, 450 * COIN);

    // The coinstake marker output creates no UTXO.
    BOOST_CHECK(!view.HaveCoin(COutPoint{chain.coinstake_txid, 0}));
}

BOOST_AUTO_TEST_CASE(raw_decoding_is_safe_and_round_trips)
{
    const SyntheticChain chain;
    legacy::TrustedReplay replay{chain.params, /*final_height=*/2, {}};
    CCoinsView base;
    CCoinsViewCache view{&base};
    std::string error;

    DataStream bytes;
    bytes << legacy::TX_LEGACY(chain.genesis);
    BOOST_REQUIRE_MESSAGE(replay.ApplyRawBlock(std::span{bytes}, view, error), error);
    BOOST_CHECK_EQUAL(replay.NextHeight(), 1);

    // Truncated bytes are rejected without touching the view or position.
    DataStream more;
    more << legacy::TX_LEGACY(chain.block1);
    BOOST_CHECK(!replay.ApplyRawBlock(std::span{more}.first(more.size() / 2), view, error));
    BOOST_CHECK_EQUAL(replay.NextHeight(), 1);
}

BOOST_AUTO_TEST_CASE(captures_standard_undo_data)
{
    const SyntheticChain chain;
    legacy::TrustedReplay replay{chain.params, /*final_height=*/2, {}};
    CCoinsView base;
    CCoinsViewCache view{&base};
    std::string error;

    BOOST_REQUIRE_MESSAGE(replay.ApplyBlock(chain.genesis, view, error), error);
    BOOST_REQUIRE_MESSAGE(replay.ApplyBlock(chain.block1, view, error), error);

    CBlockUndo undo;
    BOOST_REQUIRE_MESSAGE(replay.ApplyBlock(chain.block2, view, error, &undo), error);

    // Standard undo layout: one entry per transaction after the coinbase,
    // spent coins in input order.
    BOOST_REQUIRE_EQUAL(undo.vtxundo.size(), chain.block2.vtx.size() - 1);
    BOOST_REQUIRE_EQUAL(undo.vtxundo[0].vprevout.size(), chain.block2.vtx[1]->vin.size());

    // The captured coin is the exact coin the coinstake spent: value,
    // script, creation height, class and legacy transaction time.
    const Coin& spent{undo.vtxundo[0].vprevout[0]};
    BOOST_CHECK_EQUAL(spent.out.nValue, 400 * COIN);
    BOOST_CHECK(spent.out.scriptPubKey == (CScript() << OP_TRUE));
    BOOST_CHECK_EQUAL(spent.nHeight, 1U);
    BOOST_CHECK(!spent.fCoinBase);
    BOOST_CHECK(!spent.fCoinStake);
    BOOST_CHECK_EQUAL(spent.nTime, 1'100U);

    // Undoing with the captured data restores the pre-block state exactly:
    // remove the block's created outputs, re-add its spent inputs.
    for (size_t i{chain.block2.vtx.size()}; i-- > 0;) {
        const CTransaction& tx{*chain.block2.vtx[i]};
        for (size_t o{0}; o < tx.vout.size(); ++o) {
            view.SpendCoin(COutPoint{tx.GetHash(), static_cast<uint32_t>(o)});
        }
        if (i == 0) continue; // the coinbase spent nothing
        for (size_t in{tx.vin.size()}; in-- > 0;) {
            Coin restored{undo.vtxundo[i - 1].vprevout[in]};
            view.AddCoin(tx.vin[in].prevout, std::move(restored), /*possible_overwrite=*/true);
        }
    }

    BOOST_CHECK(!view.HaveCoin(COutPoint{chain.block2.vtx[0]->GetHash(), 0}));
    BOOST_CHECK(!view.HaveCoin(COutPoint{chain.coinstake_txid, 1}));
    const Coin& back{view.AccessCoin(COutPoint{chain.spend_txid, 0})};
    BOOST_REQUIRE(!back.IsSpent());
    BOOST_CHECK_EQUAL(back.out.nValue, 400 * COIN);
    BOOST_CHECK_EQUAL(back.nHeight, 1U);
    BOOST_CHECK_EQUAL(back.nTime, 1'100U);
    BOOST_CHECK(view.HaveCoin(COutPoint{chain.spend_txid, 1}));
}

BOOST_AUTO_TEST_CASE(rejects_inconsistent_data)
{
    const SyntheticChain chain;
    CCoinsView base;
    std::string error;

    // Broken previous-hash linkage.
    {
        legacy::TrustedReplay replay{chain.params, 2, {}};
        CCoinsViewCache view{&base};
        BOOST_REQUIRE(replay.ApplyBlock(chain.genesis, view, error));
        CBlock bad{chain.block1};
        bad.hashPrevBlock = uint256{"00000000000000000000000000000000000000000000000000000000000000ff"};
        BOOST_CHECK(!replay.ApplyBlock(bad, view, error));
        BOOST_CHECK(error.find("linkage") != std::string::npos);
    }

    // Checkpoint mismatch.
    {
        legacy::TrustedReplay replay{chain.params, 2,
            {{1, uint256{"00000000000000000000000000000000000000000000000000000000000000ff"}}}};
        CCoinsViewCache view{&base};
        BOOST_REQUIRE(replay.ApplyBlock(chain.genesis, view, error));
        BOOST_CHECK(!replay.ApplyBlock(chain.block1, view, error));
        BOOST_CHECK(error.find("checkpoint") != std::string::npos);
    }
    // The correct checkpoint passes.
    {
        legacy::TrustedReplay replay{chain.params, 2, {{1, chain.block1.GetLegacyB3Hash()}}};
        CCoinsViewCache view{&base};
        BOOST_REQUIRE(replay.ApplyBlock(chain.genesis, view, error));
        BOOST_CHECK_MESSAGE(replay.ApplyBlock(chain.block1, view, error), error);
    }

    // Tampered transaction data breaks the Merkle root.
    {
        legacy::TrustedReplay replay{chain.params, 2, {}};
        CCoinsViewCache view{&base};
        BOOST_REQUIRE(replay.ApplyBlock(chain.genesis, view, error));
        CBlock bad{chain.block1};
        CMutableTransaction tamper{*bad.vtx[1]};
        tamper.vout[0].nValue += 1;
        bad.vtx[1] = MakeTransactionRef(std::move(tamper));
        BOOST_CHECK(!replay.ApplyBlock(bad, view, error));
        BOOST_CHECK(error.find("merkle") != std::string::npos);
    }

    // Missing prevout, and per-block atomicity: the failed block's coinbase
    // coin must not leak into the view.
    {
        legacy::TrustedReplay replay{chain.params, 2, {}};
        CCoinsViewCache view{&base};
        BOOST_REQUIRE(replay.ApplyBlock(chain.genesis, view, error));
        CMutableTransaction ghost;
        ghost.version = 1;
        ghost.nTime = 1'150;
        ghost.vin.resize(1);
        ghost.vin[0].prevout = COutPoint{
            Txid::FromUint256(uint256{"00000000000000000000000000000000000000000000000000000000000000ee"}), 0};
        ghost.vout.emplace_back(1 * COIN, CScript() << OP_TRUE);
        const CBlock bad{MakeBlock(chain.params.hashGenesisBlock, 1'150,
                                   {CoinbaseTx(1'150, 500 * COIN, 1), ghost})};
        BOOST_CHECK(!replay.ApplyBlock(bad, view, error));
        BOOST_CHECK(error.find("missing") != std::string::npos);
        BOOST_CHECK(!view.HaveCoin(COutPoint{bad.vtx[0]->GetHash(), 0}));
        BOOST_CHECK_EQUAL(replay.NextHeight(), 1);
    }

    // Duplicate spend of the same outpoint inside one block.
    {
        legacy::TrustedReplay replay{chain.params, 2, {}};
        CCoinsViewCache view{&base};
        BOOST_REQUIRE(replay.ApplyBlock(chain.genesis, view, error));
        CMutableTransaction first{*chain.block1.vtx[1]};
        CMutableTransaction second{*chain.block1.vtx[1]};
        second.nTime = 1'101; // distinct txid, same prevout
        const CBlock bad{MakeBlock(chain.params.hashGenesisBlock, 1'100,
                                   {CoinbaseTx(1'100, 500 * COIN, 1), first, second})};
        BOOST_CHECK(!replay.ApplyBlock(bad, view, error));
        BOOST_CHECK(error.find("already-spent") != std::string::npos);
    }

    // A plain transaction creating value out of nothing is inconsistent.
    {
        legacy::TrustedReplay replay{chain.params, 2, {}};
        CCoinsViewCache view{&base};
        BOOST_REQUIRE(replay.ApplyBlock(chain.genesis, view, error));
        CMutableTransaction inflate{*chain.block1.vtx[1]};
        inflate.vout[0].nValue = 2000 * COIN;
        const CBlock bad{MakeBlock(chain.params.hashGenesisBlock, 1'100,
                                   {CoinbaseTx(1'100, 500 * COIN, 1), inflate})};
        BOOST_CHECK(!replay.ApplyBlock(bad, view, error));
        BOOST_CHECK(error.find("creates value") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(respects_the_boundary_and_the_codec)
{
    const SyntheticChain chain;
    CCoinsView base;
    std::string error;

    // Blocks beyond the finalized boundary are refused.
    {
        legacy::TrustedReplay replay{chain.params, /*final_height=*/0, {}};
        CCoinsViewCache view{&base};
        BOOST_REQUIRE(replay.ApplyBlock(chain.genesis, view, error));
        BOOST_CHECK(!replay.ApplyBlock(chain.block1, view, error));
        BOOST_CHECK(error.find("boundary") != std::string::npos);
    }

    // A marker-modern block can never be replayed as legacy history.
    {
        legacy::TrustedReplay replay{chain.params, 2, {}};
        CCoinsViewCache view{&base};
        BOOST_REQUIRE(replay.ApplyBlock(chain.genesis, view, error));
        CBlock bad{chain.block1};
        bad.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
        BOOST_CHECK(!replay.ApplyBlock(bad, view, error));
        BOOST_CHECK(error.find("modern codec") != std::string::npos);
    }

    // Resumption continues exactly where a persisted view left off.
    {
        legacy::TrustedReplay replay{chain.params, 2, {}};
        CCoinsViewCache view{&base};
        BOOST_REQUIRE(replay.ApplyBlock(chain.genesis, view, error));
        BOOST_REQUIRE(replay.ApplyBlock(chain.block1, view, error));

        legacy::TrustedReplay resumed{chain.params, 2, {}};
        resumed.ResumeAt(2, chain.block1.GetLegacyB3Hash());
        BOOST_CHECK_MESSAGE(resumed.ApplyBlock(chain.block2, view, error), error);
        BOOST_CHECK_EQUAL(resumed.NextHeight(), 3);
    }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(legacy_replay_persist_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(persists_and_resumes_atomically)
{
    const SyntheticChain chain;
    const fs::path path{m_args.GetDataDirBase() / "replay_persist"};
    std::string error;

    {
        legacy::ReplayDB db{DBParams{.path = path, .cache_bytes = 1 << 20}};
        legacy::PersistentReplay replay{chain.params, 2, {}, db};
        BOOST_REQUIRE_MESSAGE(replay.Load(error), error);
        BOOST_CHECK_EQUAL(replay.NextHeight(), 0);
        BOOST_REQUIRE_MESSAGE(replay.ApplyBlock(chain.genesis, error), error);
        BOOST_REQUIRE_MESSAGE(replay.ApplyBlock(chain.block1, error), error);
    } // simulated shutdown

    {
        legacy::ReplayDB db{DBParams{.path = path, .cache_bytes = 1 << 20}};
        legacy::PersistentReplay replay{chain.params, 2, {}, db};
        BOOST_REQUIRE_MESSAGE(replay.Load(error), error);
        // Restart resumes at exactly the next block.
        BOOST_CHECK_EQUAL(replay.NextHeight(), 2);
        BOOST_CHECK_EQUAL(replay.TipHash().GetHex(), chain.block1.GetLegacyB3Hash().GetHex());
        BOOST_REQUIRE_MESSAGE(replay.ApplyBlock(chain.block2, error), error);
        BOOST_REQUIRE_MESSAGE(replay.Finish(error), error);

        CCoinsViewCache view{&db};
        BOOST_CHECK(!view.HaveCoin(COutPoint{chain.genesis_coinbase, 0}));
        const Coin& stake{view.AccessCoin(COutPoint{chain.coinstake_txid, 1})};
        BOOST_REQUIRE(!stake.IsSpent());
        BOOST_CHECK(stake.fCoinStake);
    }

    {
        legacy::ReplayDB db{DBParams{.path = path, .cache_bytes = 1 << 20}};
        legacy::PersistentReplay replay{chain.params, 2, {}, db};
        BOOST_REQUIRE_MESSAGE(replay.Load(error), error);
        BOOST_CHECK(replay.Completed());
        BOOST_CHECK(!replay.ApplyBlock(chain.block2, error));
        BOOST_CHECK(error.find("completed") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(interrupted_block_is_never_considered_applied)
{
    const SyntheticChain chain;
    const fs::path path{m_args.GetDataDirBase() / "replay_interrupt"};
    std::string error;

    {
        legacy::ReplayDB db{DBParams{.path = path, .cache_bytes = 1 << 20}};
        legacy::PersistentReplay replay{chain.params, 2, {}, db};
        BOOST_REQUIRE_MESSAGE(replay.Load(error), error);
        BOOST_REQUIRE_MESSAGE(replay.ApplyBlock(chain.genesis, error), error);

        // Simulated interruption mid-block: block 1 is fully processed into
        // a cache, but the process dies before the atomic commit.
        legacy::TrustedReplay engine{chain.params, 2, {}};
        engine.ResumeAt(1, chain.params.hashGenesisBlock);
        CCoinsViewCache doomed{&db};
        BOOST_REQUIRE_MESSAGE(engine.ApplyBlock(chain.block1, doomed, error), error);
        // No Flush(): the in-memory cache dies with the "process".
    }

    {
        legacy::ReplayDB db{DBParams{.path = path, .cache_bytes = 1 << 20}};
        legacy::PersistentReplay replay{chain.params, 2, {}, db};
        BOOST_REQUIRE_MESSAGE(replay.Load(error), error);
        // The partial block was never considered applied.
        BOOST_CHECK_EQUAL(replay.NextHeight(), 1);
        CCoinsViewCache view{&db};
        BOOST_CHECK(view.HaveCoin(COutPoint{chain.genesis_coinbase, 0}));
        BOOST_CHECK(!view.HaveCoin(COutPoint{chain.spend_txid, 1}));
        // And the same block replays cleanly now.
        BOOST_REQUIRE_MESSAGE(replay.ApplyBlock(chain.block1, error), error);
    }
}

BOOST_AUTO_TEST_CASE(recovery_never_guesses)
{
    const SyntheticChain chain;
    const fs::path path{m_args.GetDataDirBase() / "replay_guess"};
    std::string error;

    {
        legacy::ReplayDB db{DBParams{.path = path, .cache_bytes = 1 << 20}};
        legacy::PersistentReplay replay{chain.params, 2, {}, db};
        BOOST_REQUIRE_MESSAGE(replay.Load(error), error);
        BOOST_REQUIRE_MESSAGE(replay.ApplyBlock(chain.genesis, error), error);
    }

    // Marker erased while coins remain: refuse instead of guessing.
    {
        CDBWrapper raw{DBParams{.path = path, .cache_bytes = 1 << 20}};
        raw.Erase(uint8_t{'R'}, /*fSync=*/true);
    }
    {
        legacy::ReplayDB db{DBParams{.path = path, .cache_bytes = 1 << 20}};
        legacy::PersistentReplay replay{chain.params, 2, {}, db};
        BOOST_CHECK(!replay.Load(error));
        BOOST_CHECK(error.find("no marker") != std::string::npos);
    }

    // Unsupported format version fails safely.
    {
        CDBWrapper raw{DBParams{.path = path, .cache_bytes = 1 << 20}};
        const legacy::ReplayDB::Marker bogus{.version = 99, .height = 0,
                                             .hash = chain.params.hashGenesisBlock,
                                             .completed = false};
        raw.Write(uint8_t{'R'}, bogus, /*fSync=*/true);
    }
    {
        legacy::ReplayDB db{DBParams{.path = path, .cache_bytes = 1 << 20}};
        legacy::PersistentReplay replay{chain.params, 2, {}, db};
        BOOST_CHECK(!replay.Load(error));
        BOOST_CHECK(error.find("version") != std::string::npos);
    }

    // A marker beyond the configured boundary fails safely.
    {
        CDBWrapper raw{DBParams{.path = path, .cache_bytes = 1 << 20}};
        const legacy::ReplayDB::Marker bogus{.version = legacy::ReplayDB::FORMAT_VERSION,
                                             .height = 7,
                                             .hash = chain.params.hashGenesisBlock,
                                             .completed = false};
        raw.Write(uint8_t{'R'}, bogus, /*fSync=*/true);
    }
    {
        legacy::ReplayDB db{DBParams{.path = path, .cache_bytes = 1 << 20}};
        legacy::PersistentReplay replay{chain.params, 2, {}, db};
        BOOST_CHECK(!replay.Load(error));
        BOOST_CHECK(error.find("boundary") != std::string::npos);
    }
}

BOOST_AUTO_TEST_SUITE_END()
