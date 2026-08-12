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
#include <consensus/block_codec.h>
#include <consensus/boundary.h>
#include <consensus/era.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <dbwrapper.h>
#include <legacy/codec.h>
#include <legacy/consensus.h>
#include <legacy/primitives.h>
#include <legacy/replay.h>
#include <modern/pos.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <map>
#include <memory>
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
    TransitionSetup() : ChainTestingSetup{ChainType::REGTEST}
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
    ~TransitionSetup() { modern::SetModernPosValidatorForTesting(nullptr); }
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

    for (int height{1}; height <= SYNTHETIC_H; ++height) {
        const CBlockIndex* prev{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
        std::vector<CMutableTransaction> extra;
        if (height == SYNTHETIC_H) {
            // A legacy transaction spending a long-matured legacy coinbase:
            // its output becomes the pre-H UTXO spent in the modern era.
            CMutableTransaction spend;
            spend.version = 1;
            spend.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
            spend.vin.resize(1);
            spend.vin[0].prevout = COutPoint{mature_coinbase, 0};
            spend.vin[0].scriptSig = CScript{}; // OP_TRUE output: no signature
            spend.vout.emplace_back(legacy::GetProofOfWorkReward(0, 2, consensus),
                                    CScript() << OP_TRUE);
            extra.push_back(spend);
        }
        CBlock block{build_legacy(prev, std::move(extra))};
        const auto submitted{CodecRoundTrip(block)};
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlock(submitted, /*force_processing=*/true,
                                                       /*min_pow_checked=*/true, &new_block),
                              "legacy block at height " << height << " rejected");
        BOOST_REQUIRE(new_block);
        legacy_blocks.push_back(*submitted);
        if (height == 2) mature_coinbase = submitted->vtx[0]->GetHash();
        if (height == SYNTHETIC_H) pre_h_txid = submitted->vtx[1]->GetHash();
    }
    BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->nHeight), SYNTHETIC_H);

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

    // ---- (6)+(7) Marker-modern blocks from H+1 through the modern
    // PoS dispatch (test adapter; no economic rules invented).
    AcceptingPos pos;
    modern::SetModernPosValidatorForTesting(&pos);

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

BOOST_AUTO_TEST_SUITE_END()
