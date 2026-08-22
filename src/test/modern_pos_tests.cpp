// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <chain.h>
#include <chainparams.h>
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

namespace {

//! Synthetic legacy chain constants, mirroring the transition fixtures.
constexpr uint32_t GENESIS_TIME{1'400'000'000};
constexpr int64_t MOCK_NOW{1'400'100'000};
constexpr int SYN_H{32};
constexpr int SYN_CORRIDOR{24}; // >= STAKE_ACTIVATION_DEPTH so corridor stake is ACTIVE at M
constexpr uint32_t EASY_BITS{0x207fffff};

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

    CBlock BuildCorridor(const CBlockIndex* prev, std::vector<CMutableTransaction> txs)
    {
        CMutableTransaction coinbase;
        coinbase.version = 2;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << CScriptNum{prev->nHeight + 1} << CScriptNum{7};
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 17);
        block.nBits = EASY_BITS;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        block.nNonce = 0;
        while (!CheckTransitionPowEligibility(block)) ++block.nNonce;
        return block;
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

//! Disk-backed variant for the restart/reindex scenario.
struct ModernPosDiskSetup : public ModernPosSetup {
    ModernPosDiskSetup()
        : ModernPosSetup{{.coins_db_in_memory = false, .block_tree_db_in_memory = false}} {}
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(modern_pos_tests, BasicTestingSetup)

//! The provisional-parameter guard demanded by the frozen V1 spec (§9):
//! no shipped network may configure the REVISABLE_BEFORE_MAINNET modern-PoS
//! block, and no shipped network may carry a test-only injection point.
//! While modern_pos is unset, modern-PoS validation and production fail
//! closed, so a forgotten ratification cannot silently activate scaffolding
//! numbers on a real chain.
BOOST_AUTO_TEST_CASE(no_provisional_parameters_on_shipped_networks)
{
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4,
                                  ChainType::SIGNET, ChainType::REGTEST}) {
        const auto params{CreateChainParams(*m_node.args, chain)};
        const Consensus::Params& consensus{params->GetConsensus()};
        BOOST_CHECK_MESSAGE(!consensus.modern_pos.has_value(),
                            "modern_pos configured on a shipped network");
        BOOST_CHECK_MESSAGE(consensus.test_only_modern_pos_validator == nullptr,
                            "test-only PoS validator set on a shipped network");
        BOOST_CHECK_MESSAGE(!consensus.test_only_asset_policies_active,
                            "test-only asset activation set on a shipped network");
        if (chain == ChainType::MAIN) {
            // RATIFIED 2026-08-21: minimum STAKE principal is 333 modern B3
            // (the kB3 nomination) = 333e9 base units.
            BOOST_CHECK_EQUAL(consensus.min_stake_amount.value_or(-1), 333 * CAmount{1'000'000'000});
        } else {
            BOOST_CHECK_MESSAGE(!consensus.min_stake_amount.has_value(),
                                "stake minimum set on a non-ratified network");
        }
        BOOST_CHECK_MESSAGE(!consensus.transition_pow_bits.has_value(),
                            "provisional corridor difficulty set on a shipped network");
        if (chain == ChainType::MAIN) {
            // RATIFIED 2026-08-21: mainnet corridor reward is exactly 0
            // (fees only), stated explicitly rather than defaulted.
            BOOST_CHECK(consensus.transition_pow_reward.has_value());
            BOOST_CHECK_EQUAL(consensus.transition_pow_reward.value_or(-1), 0);
        } else {
            BOOST_CHECK_MESSAGE(!consensus.transition_pow_reward.has_value(),
                                "corridor reward set on a non-ratified network");
        }
    }
}

//! Structural sanity of the provisional defaults themselves.
BOOST_AUTO_TEST_CASE(provisional_parameter_block_is_structurally_valid)
{
    Consensus::ModernPosParams pos{};
    BOOST_CHECK(pos.Valid());
    BOOST_CHECK_EQUAL(pos.reorg_horizon.value_or(-1), 1440); // D RATIFIED 2026-08-21: one day at 60 s.

    pos.round_seconds = 0;
    BOOST_CHECK(!pos.Valid());
    pos = Consensus::ModernPosParams{};
    pos.f0_den = 0;
    BOOST_CHECK(!pos.Valid());
    pos = Consensus::ModernPosParams{};
    pos.reorg_horizon = 0;
    BOOST_CHECK(!pos.Valid());
}

//! Scenario 1 — normal operation: deterministic production through the
//! assembler, signature by the validator key, seed chain verified block by
//! block, exact timestamps, sentinel bits, fees-only reward cap.
BOOST_FIXTURE_TEST_CASE(v1_normal_operation, ModernPosSetup)
{
    AdvanceToModernPos();
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    const CAmount W{STAKE_A + STAKE_B};

    node::BlockAssembler::Options options;
    options.coinbase_output_script = CScript() << OP_TRUE;
    options.include_dummy_extranonce = true;
    options.modern_pos_validator_key = m_val_a;

    for (int i{0}; i < 5; ++i) {
        const CBlockIndex* prev{Tip()};
        const int64_t expected_round{FindRound(prev, m_val_a, STAKE_A, W)};
        const uint256 expected_seed{SeedFor(prev)};

        const auto tmpl{node::BlockAssembler(chainman.ActiveChainstate(), nullptr, options).CreateNewBlock()};
        BOOST_REQUIRE(tmpl);
        CBlock block{tmpl->block};
        BOOST_CHECK(Consensus::HasB3BlockCodecV2(block.nVersion));
        BOOST_CHECK_EQUAL(block.nBits, consensus.modern_pos->sentinel_bits);
        BOOST_CHECK_EQUAL(block.nNonce, 0U);
        BOOST_CHECK_EQUAL(block.vtx[0]->GetValueOut(), 0); // fees only under the cap
        BOOST_CHECK_EQUAL(block.GetBlockTime(),
                          modern::ModernPosBlockTime(prev->GetBlockTime(), expected_round,
                                                     *consensus.modern_pos));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        Sign(block, m_key_a);
        BOOST_REQUIRE_MESSAGE(Submit(block), "modern-PoS block at height "
                                                 << prev->nHeight + 1 << " rejected");
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, prev->nHeight + 1);

        // The connected index caches the block's eligibility digest — the
        // next height's seed — and it recomputes exactly.
        const uint256 expected_digest{modern::ModernPosEligibilityDigest(
            Domain(), expected_seed, static_cast<uint32_t>(Tip()->nHeight),
            static_cast<uint32_t>(expected_round), m_val_a)};
        BOOST_CHECK_EQUAL(
            WITH_LOCK(cs_main, return Tip()->m_modern_pos_digest).GetHex(),
            expected_digest.GetHex());
    }
    BOOST_CHECK_EQUAL(Tip()->nHeight, SYN_H + SYN_CORRIDOR + 5);
}

//! Scenario 2 — low online stake: only the small validator (B, 0.5% of
//! stake) is online. Recovery rounds relax eligibility deterministically
//! until B qualifies; the block carries the exact round timestamp. A claim
//! of any earlier round is refused as ineligible. The PoS-native fork
//! choice prefers the lower-round block at equal height, and the horizon
//! refuses a deep fork.
BOOST_FIXTURE_TEST_CASE(v1_low_online_stake_recovery, ModernPosSetup)
{
    AdvanceToModernPos();
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    const CAmount W{STAKE_A + STAKE_B};

    const CBlockIndex* prev{Tip()};
    const int64_t round_b{FindRound(prev, m_val_b, STAKE_B, W)};
    BOOST_TEST_MESSAGE("validator B first eligible at round " << round_b);

    // An earlier round than B's first eligible round is refused at connect
    // (header-valid: the timestamp is exact for the claimed round).
    if (round_b > 0) {
        CBlock early{BuildPos(prev, m_val_b, round_b - 1, 0)};
        Sign(early, m_key_b);
        SubmitExpectConnectFailure(early);
    }

    // B's genuine recovery-round block connects.
    CBlock recovery{BuildPos(prev, m_val_b, round_b, 0)};
    Sign(recovery, m_key_b);
    BOOST_REQUIRE(Submit(recovery));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, prev->nHeight + 1);
    BOOST_CHECK_EQUAL(Tip()->GetBlockHash().GetHex(), recovery.GetHash().GetHex());
    BOOST_CHECK_EQUAL(recovery.GetBlockTime(),
                      modern::ModernPosBlockTime(prev->GetBlockTime(), round_b,
                                                 *consensus.modern_pos));

    // Fork choice, rule 2: A now produces at the SAME height in an earlier
    // round; equal height and equal accumulated work, but the lower round
    // wins deterministically and the chain reorganizes onto it.
    const int64_t round_a{FindRound(prev, m_val_a, STAKE_A, W)};
    if (round_a < round_b) {
        CBlock better{BuildPos(prev, m_val_a, round_a, 0)};
        Sign(better, m_key_a);
        BOOST_REQUIRE(Submit(better));
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, prev->nHeight + 1);
        BOOST_CHECK_EQUAL(Tip()->GetBlockHash().GetHex(), better.GetHash().GetHex());
    }

    // The modern reorganization horizon: a block forking deeper than D
    // below the tip is refused without ever entering the index. The horizon
    // governs modern-PoS heights only, so first extend the PoS span until
    // the fork point itself lies in the modern-PoS phase.
    {
        const int first_pos_height{SYN_H + SYN_CORRIDOR + 1};
        while (Tip()->nHeight < first_pos_height + *consensus.modern_pos->reorg_horizon) {
            const CBlockIndex* p{Tip()};
            CBlock ext{BuildPos(p, m_val_a, FindRound(p, m_val_a, STAKE_A, W), 0)};
            Sign(ext, m_key_a);
            BOOST_REQUIRE(Submit(ext));
        }
        const int deep_parent_height{Tip()->nHeight - *consensus.modern_pos->reorg_horizon - 1};
        const CBlockIndex* deep_parent{
            WITH_LOCK(cs_main, return chainman.ActiveChain()[deep_parent_height])};
        BOOST_REQUIRE(deep_parent != nullptr);
        CBlock deep{BuildPos(deep_parent, m_val_a,
                             FindRound(deep_parent, m_val_a, STAKE_A, W), 0, /*extra=*/9)};
        Sign(deep, m_key_a);
        BOOST_CHECK(!Submit(deep));
        BOOST_CHECK(WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(deep.GetHash())) ==
                    nullptr);
    }
}

//! Scenario 3 — invalid validator signatures: corrupted, missing, and
//! wrong-key signatures are all refused and never move the tip. Each
//! variant carries a distinct identity (coinbase extra data), because the
//! signature itself lives outside the block hash.
BOOST_FIXTURE_TEST_CASE(v1_invalid_signature, ModernPosSetup)
{
    AdvanceToModernPos();
    const CAmount W{STAKE_A + STAKE_B};
    const CBlockIndex* prev{Tip()};
    const int64_t round_a{FindRound(prev, m_val_a, STAKE_A, W)};

    { // Corrupted signature.
        CBlock block{BuildPos(prev, m_val_a, round_a, 0, /*extra=*/1)};
        Sign(block, m_key_a);
        block.vchBlockSig[0] ^= 0x01;
        SubmitExpectConnectFailure(block);
    }
    { // Missing signature (fails the contextual size rule).
        CBlock block{BuildPos(prev, m_val_a, round_a, 0, /*extra=*/2)};
        block.vchBlockSig.clear();
        BOOST_CHECK(!Submit(block));
    }
    { // Signed by a different key than the coinbase declares.
        CBlock block{BuildPos(prev, m_val_a, round_a, 0, /*extra=*/3)};
        Sign(block, m_key_b);
        SubmitExpectConnectFailure(block);
    }
    BOOST_CHECK_EQUAL(Tip()->nHeight, prev->nHeight); // tip never moved

    // The honest block still connects afterwards.
    CBlock good{BuildPos(prev, m_val_a, round_a, 0)};
    Sign(good, m_key_a);
    BOOST_REQUIRE(Submit(good));
    BOOST_CHECK_EQUAL(Tip()->GetBlockHash().GetHex(), good.GetHash().GetHex());
}

//! Scenario 4 — invalid eligibility proofs: a validator with no stake, a
//! coinbase without a key declaration, a non-exact timestamp, wrong
//! sentinel bits, and a non-zero nonce are all refused.
BOOST_FIXTURE_TEST_CASE(v1_invalid_eligibility, ModernPosSetup)
{
    AdvanceToModernPos();
    ChainstateManager& chainman{*m_node.chainman};
    const CAmount W{STAKE_A + STAKE_B};
    const CBlockIndex* prev{Tip()};

    { // A key with no active stake is never eligible, whatever the round.
        const modern::PosValidatorKey stranger{XOnly(MakeValidatorKey(0x33))};
        CBlock block{BuildPos(prev, stranger, /*round=*/0, 0)};
        Sign(block, MakeValidatorKey(0x33));
        SubmitExpectConnectFailure(block);
    }
    const int64_t round_a{FindRound(prev, m_val_a, STAKE_A, W)};
    { // Coinbase without the validator declaration.
        CBlock block{BuildPos(prev, m_val_a, round_a, 0, /*extra=*/4)};
        CMutableTransaction cb{*block.vtx[0]};
        cb.vin[0].scriptSig = CScript() << CScriptNum{prev->nHeight + 1}; // no key push
        block.vtx[0] = MakeTransactionRef(std::move(cb));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        Sign(block, m_key_a);
        SubmitExpectConnectFailure(block);
    }
    { // Non-exact timestamp: refused at the header, never stored.
        CBlock block{BuildPos(prev, m_val_a, round_a, 0, /*extra=*/5)};
        ++block.nTime;
        block.hashMerkleRoot = BlockMerkleRoot(block);
        Sign(block, m_key_a);
        BOOST_CHECK(!Submit(block));
        BOOST_CHECK(WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(block.GetHash())) ==
                    nullptr);
    }
    { // Wrong nBits (not the sentinel): refused at the header.
        CBlock block{BuildPos(prev, m_val_a, round_a, 0, /*extra=*/6)};
        block.nBits = EASY_BITS - 1;
        Sign(block, m_key_a);
        BOOST_CHECK(!Submit(block));
    }
    { // Non-zero nonce: refused at the header.
        CBlock block{BuildPos(prev, m_val_a, round_a, 0, /*extra=*/7)};
        block.nNonce = 1;
        Sign(block, m_key_a);
        BOOST_CHECK(!Submit(block));
    }
    BOOST_CHECK_EQUAL(Tip()->nHeight, prev->nHeight);
}

//! Scenario 5 — invalid rewards: the unconditional cap refuses a coinbase
//! above fees plus the configured reward, and ruling M6 refuses a reward
//! paid directly into a STAKE output even when it fits under the cap.
BOOST_FIXTURE_TEST_CASE(v1_invalid_reward, ModernPosSetup)
{
    AdvanceToModernPos();
    const CAmount W{STAKE_A + STAKE_B};
    const CBlockIndex* prev{Tip()};
    const int64_t round_a{FindRound(prev, m_val_a, STAKE_A, W)};

    { // Over the fees-only cap by a single unit.
        CBlock block{BuildPos(prev, m_val_a, round_a, /*coinbase_value=*/1)};
        Sign(block, m_key_a);
        SubmitExpectConnectFailure(block);
    }
    { // M6: reward into a STAKE output. Raise the provisional reward so the
      // amount fits under the cap and only the STAKE prohibition can fire.
        Consensus::Params& mutable_consensus{MutableConsensus()};
        mutable_consensus.modern_pos->reward = 2'000;
        CBlock block{BuildPos(prev, m_val_a, round_a, 0, /*extra=*/8)};
        CMutableTransaction cb{*block.vtx[0]};
        cb.vout[0].nValue = 1'500;
        cb.vout[0].scriptPubKey = modern::MakeStakeScript(m_val_a, CScript() << OP_TRUE);
        block.vtx[0] = MakeTransactionRef(std::move(cb));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        Sign(block, m_key_a);
        SubmitExpectConnectFailure(block);
        mutable_consensus.modern_pos->reward = 0;
    }
    BOOST_CHECK_EQUAL(Tip()->nHeight, prev->nHeight);

    // The compliant block connects.
    CBlock good{BuildPos(prev, m_val_a, round_a, 0)};
    Sign(good, m_key_a);
    BOOST_REQUIRE(Submit(good));
}

//! Scenario 6 — restart and reindex: the persisted eligibility digests
//! reload with the block index (production continues without recomputing
//! seeds from block bodies), and a chainstate reindex reconnects the whole
//! legacy + corridor + modern-PoS history to the same tip.
BOOST_FIXTURE_TEST_CASE(v1_restart_and_reindex, ModernPosDiskSetup)
{
    AdvanceToModernPos();
    const CAmount W{STAKE_A + STAKE_B};

    const auto produce{[&] {
        const CBlockIndex* prev{Tip()};
        CBlock block{BuildPos(prev, m_val_a, FindRound(prev, m_val_a, STAKE_A, W), 0)};
        Sign(block, m_key_a);
        BOOST_REQUIRE(Submit(block));
        return block.GetHash();
    }};
    for (int i{0}; i < 3; ++i) produce();
    const int pre_restart_height{Tip()->nHeight};
    const uint256 pre_restart_hash{Tip()->GetBlockHash()};
    const uint256 pre_restart_digest{WITH_LOCK(cs_main, return Tip()->m_modern_pos_digest)};
    BOOST_REQUIRE(!pre_restart_digest.IsNull());

    // ---- Simulated shutdown + restart over the persisted databases.
    {
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().ForceFlushStateToDisk();
    }
    m_node.chainman.reset();
    m_make_chainman();
    LoadVerifyActivateChainstate();
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, pre_restart_height);
    BOOST_CHECK_EQUAL(Tip()->GetBlockHash().GetHex(), pre_restart_hash.GetHex());
    // The cached digest — the next block's seed — survived on disk.
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return Tip()->m_modern_pos_digest).GetHex(),
                      pre_restart_digest.GetHex());

    // Production continues seamlessly from the reloaded seed.
    produce();
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, pre_restart_height + 1);

    // ---- Chainstate reindex: rebuild by reconnecting the entire history
    // (legacy replay-scoped admission, corridor scrypt, modern-PoS
    // eligibility and signatures) from the block files.
    {
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().ForceFlushStateToDisk();
    }
    m_node.chainman.reset();
    m_args.ForceSetArg("-reindex-chainstate", "1");
    m_make_chainman();
    LoadVerifyActivateChainstate();
    m_args.ForceSetArg("-reindex-chainstate", "0");
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, pre_restart_height + 1);

    // And the chain still extends after the reindex.
    produce();
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, pre_restart_height + 2);
}

//! The restored header-spam pre-filter: marker-modern headers are
//! header-only checkable once a corridor or modern-PoS policy is
//! configured; legacy headers stay body-judged; an unconfigured chain
//! keeps the filter open (status quo ante).
BOOST_AUTO_TEST_CASE(header_prefilter_is_marker_and_policy_aware)
{
    Consensus::Params params{};
    params.legacy_b3coin = true;

    CBlockHeader legacy_header;
    legacy_header.nVersion = 4;
    legacy_header.nBits = 0x207fffff;
    legacy_header.nTime = 1'900'000'000;

    CBlockHeader modern_header{legacy_header};
    modern_header.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);

    // Nothing configured: nothing checkable, filter open for both.
    BOOST_CHECK(HasValidProofOfWork({&legacy_header, 1}, params));
    BOOST_CHECK(HasValidProofOfWork({&modern_header, 1}, params));

    // Modern-PoS policy configured: the sentinel is required of
    // marker-modern headers; legacy headers stay unfiltered.
    params.modern_pos = Consensus::ModernPosParams{};
    modern_header.nBits = params.modern_pos->sentinel_bits;
    BOOST_CHECK(HasValidProofOfWork({&modern_header, 1}, params));
    modern_header.nBits = 0x207ffffe;
    BOOST_CHECK(!HasValidProofOfWork({&modern_header, 1}, params));
    BOOST_CHECK(HasValidProofOfWork({&legacy_header, 1}, params));

    // Corridor policy configured as well: corridor-ground scrypt at the
    // corridor target also satisfies the pre-filter (phase is unknowable
    // header-only, so either policy admits the header).
    params.transition_pow_bits = 0x207fffff;
    modern_header.nBits = 0x207fffff;
    modern_header.nNonce = 0;
    while (!CheckTransitionPowEligibility(modern_header)) ++modern_header.nNonce;
    BOOST_CHECK(HasValidProofOfWork({&modern_header, 1}, params));

    // Corridor bits without a passing scrypt hash and without the sentinel:
    // refused as spam.
    while (CheckTransitionPowEligibility(modern_header)) ++modern_header.nNonce;
    params.modern_pos->sentinel_bits = 0x1d00ffff; // sentinel no longer matches
    BOOST_CHECK(!HasValidProofOfWork({&modern_header, 1}, params));
}

BOOST_AUTO_TEST_SUITE_END()
