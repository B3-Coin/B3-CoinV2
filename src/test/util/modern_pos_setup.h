// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_TEST_UTIL_MODERN_POS_SETUP_H
#define B3COIN_TEST_UTIL_MODERN_POS_SETUP_H

//! Shared synthetic B3 chain fixture: legacy prefix to SYN_H, configurable
//! corridor, Modern PoS V1 with two staked validators. Extracted verbatim
//! from modern_pos_tests.cpp (plan Commit 3) so later finality/MPA suites can
//! reuse it; behaviour is unchanged.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/era.h>
#include <consensus/merkle.h>
#include <consensus/modern_pos_params.h>
#include <consensus/params.h>
#include <key.h>
#include <legacy/codec.h>
#include <legacy/consensus.h>
#include <modern/fn.h>
#include <modern/pos_v1.h>
#include <modern/stake.h>
#include <node/miner.h>
#include <node/stake_registry.h>
#include <node/staking.h>
#include <pow.h>
#include <primitives/block.h>
#include <script/script.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/time.h>
#include <validation.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <memory>
#include <vector>

namespace b3test {

//! Synthetic legacy chain constants, mirroring the transition fixtures.
inline constexpr uint32_t GENESIS_TIME{1'400'000'000};
inline constexpr int64_t MOCK_NOW{1'400'100'000};
inline constexpr int SYN_H{32};
inline constexpr int SYN_CORRIDOR{24}; // >= STAKE_ACTIVATION_DEPTH so corridor stake is ACTIVE at M
inline constexpr uint32_t EASY_BITS{0x207fffff};

inline CBlock MakeSyntheticLegacyGenesis()
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
    genesis.nBits = EASY_BITS;
    genesis.nNonce = 0;
    genesis.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

//! Regtest fixture rewritten into a synthetic legacy-B3 chain, carried
//! through the corridor into a configured modern-PoS phase.
struct ModernPosSetup : public ChainTestingSetup {
    static TestOpts WithDefaults(TestOpts opts)
    {
        opts.extra_args.push_back("-acceptnonstdtxn=1");
        return opts;
    }
    explicit ModernPosSetup(TestOpts opts = {})
        : ChainTestingSetup{ChainType::REGTEST, WithDefaults(std::move(opts))}
    {
        SetMockTime(MOCK_NOW);
        auto& consensus{const_cast<Consensus::Params&>(m_node.chainman->GetConsensus())};
        consensus.legacy_b3coin = true;
        consensus.legacy_last_pow_block = 1'000;
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

    Consensus::Params& MutableConsensus()
    {
        return const_cast<Consensus::Params&>(m_node.chainman->GetConsensus());
    }
    const CBlockIndex* Tip()
    {
        return WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip());
    }
    std::shared_ptr<CBlock> CodecRoundTrip(const CBlock& block)
    {
        DataStream bytes;
        bytes << legacy::TX_LEGACY(block);
        auto decoded{std::make_shared<CBlock>()};
        bytes >> legacy::TX_LEGACY(*decoded);
        return decoded;
    }
    bool Submit(const CBlock& block)
    {
        bool new_block{false};
        return m_node.chainman->ProcessNewBlock(CodecRoundTrip(block), true, true, &new_block);
    }
    //! Submit a block whose header and structure are valid but whose CONNECT
    //! must fail: ProcessNewBlock stores it (returning true), activation
    //! refuses it, the index entry is marked failed, and the tip never moves.
    void SubmitExpectConnectFailure(const CBlock& block)
    {
        const uint256 tip_before{Tip()->GetBlockHash()};
        BOOST_REQUIRE(Submit(block));
        BOOST_CHECK_EQUAL(Tip()->GetBlockHash().GetHex(), tip_before.GetHex());
        LOCK(cs_main);
        const CBlockIndex* index{m_node.chainman->m_blockman.LookupBlockIndex(block.GetHash())};
        BOOST_REQUIRE(index != nullptr);
        BOOST_CHECK(index->nStatus & BLOCK_FAILED_VALID);
    }

    CBlock BuildLegacy(const CBlockIndex* prev, std::vector<CMutableTransaction> txs)
    {
        const Consensus::Params& consensus{m_node.chainman->GetConsensus()};
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
    }

    CBlock BuildCorridor(const CBlockIndex* prev, std::vector<CMutableTransaction> txs,
                         const int64_t time_delta = 60, const uint32_t bits = EASY_BITS,
                         std::vector<CTxOut> extra_coinbase_outputs = {})
    {
        CMutableTransaction coinbase;
        coinbase.version = 2;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << CScriptNum{prev->nHeight + 1} << CScriptNum{7};
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
        for (CTxOut& out : extra_coinbase_outputs) coinbase.vout.push_back(std::move(out));
        CBlock block;
        block.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + time_delta);
        block.nBits = bits;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        block.nNonce = 0;
        while (!CheckTransitionPowEligibility(block)) ++block.nNonce;
        return block;
    }

    //! Legacy history to SYN_H only (no boundary configured yet).
    void AdvanceLegacyToH()
    {
        for (int height{1}; height <= SYN_H; ++height) {
            BOOST_REQUIRE(Submit(BuildLegacy(Tip(), {})));
        }
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, SYN_H);
    }
    //! Configure the corridor with H = SYN_H and the given X (unset = the
    //! X-distribution pause), then rebuild the candidate set as the fixture
    //! pattern requires after mutating params mid-run.
    void ConfigureCorridor(const std::optional<uint256>& x, const uint32_t bits = EASY_BITS)
    {
        Consensus::Params& mutable_consensus{MutableConsensus()};
        mutable_consensus.hard_fork_height = SYN_H + 1;
        mutable_consensus.legacy_final_hash = x;
        mutable_consensus.transition_pow_length = SYN_CORRIDOR;
        mutable_consensus.transition_pow_bits = bits;
        mutable_consensus.transition_pow_reward = 0;
        mutable_consensus.min_stake_amount = 1000;
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        chainstate.setBlockIndexCandidates.clear();
        chainstate.PopulateBlockIndexCandidates();
    }

    // ---- Modern-PoS helpers (mirror the frozen V1 rules exactly) ----

    CKey MakeValidatorKey(const unsigned char seed)
    {
        std::vector<unsigned char> data(32, seed);
        CKey key;
        key.Set(data.begin(), data.end(), /*fCompressedIn=*/true);
        BOOST_REQUIRE(key.IsValid());
        return key;
    }
    modern::PosValidatorKey XOnly(const CKey& key)
    {
        const XOnlyPubKey xonly{key.GetPubKey()};
        modern::PosValidatorKey out;
        std::copy(xonly.data(), xonly.data() + 32, out.begin());
        return out;
    }
    uint256 Domain()
    {
        const Consensus::Params& consensus{m_node.chainman->GetConsensus()};
        const auto domain{modern::ModernChainDomain(consensus.hashGenesisBlock,
                                                    *consensus.legacy_final_hash)};
        BOOST_REQUIRE(domain.has_value());
        return *domain;
    }
    uint256 SeedFor(const CBlockIndex* prev)
    {
        const Consensus::Params& consensus{m_node.chainman->GetConsensus()};
        if (Consensus::GetConsensusPhase(prev->nHeight, consensus) ==
            Consensus::ConsensusPhase::MODERN_POS) {
            return prev->m_modern_pos_digest;
        }
        return modern::ModernPosGenesisSeed(Domain(), prev->GetBlockHash());
    }
    //! Smallest round at which `key` with weight `w` of total `W` is
    //! eligible for the child of `prev`.
    int64_t FindRound(const CBlockIndex* prev, const modern::PosValidatorKey& key,
                      const CAmount w, const CAmount W)
    {
        const Consensus::ModernPosParams& pos{*m_node.chainman->GetConsensus().modern_pos};
        const uint256 seed{SeedFor(prev)};
        for (int64_t r{0}; r < 100'000; ++r) {
            const uint256 digest{modern::ModernPosEligibilityDigest(
                Domain(), seed, static_cast<uint32_t>(prev->nHeight + 1), static_cast<uint32_t>(r), key)};
            if (modern::ModernPosEligible(digest, w, W, r, pos)) return r;
        }
        BOOST_REQUIRE_MESSAGE(false, "no eligible round found");
        return -1;
    }
    //! Hand-built modern-PoS block; `extra` varies the coinbase to give
    //! deliberately invalid variants a distinct identity from the honest
    //! block at the same round (the signature lives outside the hash).
    CBlock BuildPos(const CBlockIndex* prev, const modern::PosValidatorKey& key, const int64_t round,
                    const CAmount coinbase_value, const int extra = 0,
                    std::vector<CMutableTransaction> txs = {})
    {
        const Consensus::ModernPosParams& pos{*m_node.chainman->GetConsensus().modern_pos};
        CMutableTransaction coinbase;
        coinbase.version = 2;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << CScriptNum{prev->nHeight + 1}
                                              << std::vector<unsigned char>(key.begin(), key.end());
        if (extra != 0) coinbase.vin[0].scriptSig << CScriptNum{extra};
        coinbase.vout.emplace_back(coinbase_value, CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(modern::ModernPosBlockTime(prev->GetBlockTime(), round, pos));
        block.nBits = pos.sentinel_bits;
        block.nNonce = 0;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        return block;
    }
    void Sign(CBlock& block, const CKey& key)
    {
        BOOST_REQUIRE(node::BlockAssembler::SignModernPosBlock(
            block, key, m_node.chainman->GetConsensus()));
    }

    // ---- The shared chain: legacy history to H, boundary pinned, corridor
    // with two STAKE creations (validator A large, validator B small),
    // ending at the last corridor height M-1.
    CKey m_key_a;
    CKey m_key_b;
    modern::PosValidatorKey m_val_a{};
    modern::PosValidatorKey m_val_b{};
    static constexpr CAmount STAKE_A{200'000};
    static constexpr CAmount STAKE_B{1'000};

    void AdvanceToModernPos()
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
        fund.vin[0].scriptSig = CScript{};
        const CAmount fund_value{legacy::GetProofOfWorkReward(0, 1, consensus)};
        fund.vout.emplace_back(fund_value, CScript() << OP_TRUE);
        const CBlock block_h{BuildLegacy(Tip(), {fund})};
        const Txid fund_txid{block_h.vtx[1]->GetHash()};
        BOOST_REQUIRE(Submit(block_h));
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, SYN_H);

        Consensus::Params& mutable_consensus{MutableConsensus()};
        mutable_consensus.hard_fork_height = SYN_H + 1;
        mutable_consensus.legacy_final_hash = Tip()->GetBlockHash();
        mutable_consensus.transition_pow_length = SYN_CORRIDOR;
        mutable_consensus.transition_pow_bits = EASY_BITS;
        mutable_consensus.transition_pow_reward = 0; // ratified fees-only, stated explicitly
        mutable_consensus.min_stake_amount = 1000;
        Consensus::ModernPosParams pos{};
        pos.reorg_horizon = 12; // small-chain scaffolding override of the ratified 1440
        mutable_consensus.modern_pos = pos;
        {
            // A real node configures its params before any block index
            // exists. This fixture mutates them mid-run (the established
            // scaffolding pattern), and the candidate-set ordering depends
            // on them, so the set must be rebuilt under the new order.
            LOCK(cs_main);
            Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
            chainstate.setBlockIndexCandidates.clear();
            chainstate.PopulateBlockIndexCandidates();
        }

        m_key_a = MakeValidatorKey(0x11);
        m_key_b = MakeValidatorKey(0x22);
        m_val_a = XOnly(m_key_a);
        m_val_b = XOnly(m_key_b);

        CMutableTransaction stake_tx;
        stake_tx.version = 2;
        stake_tx.vin.resize(1);
        stake_tx.vin[0].prevout = COutPoint{fund_txid, 0};
        stake_tx.vin[0].scriptSig = CScript{};
        stake_tx.vout.emplace_back(STAKE_A, modern::MakeStakeScript(m_val_a, CScript() << OP_TRUE));
        stake_tx.vout.emplace_back(STAKE_B, modern::MakeStakeScript(m_val_b, CScript() << OP_TRUE));
        stake_tx.vout.emplace_back(fund_value - STAKE_A - STAKE_B, CScript() << OP_TRUE);

        for (int i{0}; i < SYN_CORRIDOR; ++i) {
            const CBlock block{BuildCorridor(
                Tip(), i == 0 ? std::vector<CMutableTransaction>{stake_tx}
                              : std::vector<CMutableTransaction>{})};
            BOOST_REQUIRE_MESSAGE(Submit(block), "corridor block at height "
                                                     << Tip()->nHeight + 1 << " rejected");
        }
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, SYN_H + SYN_CORRIDOR);
    }
};

//! Variant for the staking-loop scenario: the fixture's mock clock sits
//! ~27 h past the synthetic chain's block times, which the node would read
//! as initial block download (and the loop correctly idles during IBD), so
//! the tip-age bound is widened. Production nodes keep the default.
struct ModernPosStakingSetup : public ModernPosSetup {
    ModernPosStakingSetup() : ModernPosSetup{{.extra_args = {"-maxtipage=1000000000"}}} {}
};

//! Disk-backed variant for the restart/reindex scenario.
struct ModernPosDiskSetup : public ModernPosSetup {
    ModernPosDiskSetup()
        : ModernPosSetup{{.coins_db_in_memory = false, .block_tree_db_in_memory = false}} {}
};

} // namespace b3test

#endif // B3COIN_TEST_UTIL_MODERN_POS_SETUP_H
