// Copyright (c) 2017-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/block_codec.h>
#include <consensus/era.h>
#include <node/blockstorage.h>
#include <rpc/blockchain.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <util/string.h>

#include <boost/test/unit_test.hpp>

#include <cstdlib>

using util::ToString;

/* Equality between doubles is imprecise. Comparison should be done
 * with a small threshold of tolerance, rather than exact equality.
 */
static bool DoubleEquals(double a, double b, double epsilon)
{
    return std::abs(a - b) < epsilon;
}

static CBlockIndex* CreateBlockIndexWithNbits(uint32_t nbits)
{
    CBlockIndex* block_index = new CBlockIndex();
    block_index->nHeight = 46367;
    block_index->nTime = 1269211443;
    block_index->nBits = nbits;
    return block_index;
}

static void RejectDifficultyMismatch(double difficulty, double expected_difficulty) {
     BOOST_CHECK_MESSAGE(
        DoubleEquals(difficulty, expected_difficulty, 0.00001),
        "Difficulty was " + ToString(difficulty)
            + " but was expected to be " + ToString(expected_difficulty));
}

/* Given a BlockIndex with the provided nbits,
 * verify that the expected difficulty results.
 */
static void TestDifficulty(uint32_t nbits, double expected_difficulty)
{
    CBlockIndex* block_index = CreateBlockIndexWithNbits(nbits);
    double difficulty = GetDifficulty(*block_index);
    delete block_index;

    RejectDifficultyMismatch(difficulty, expected_difficulty);
}

BOOST_FIXTURE_TEST_SUITE(blockchain_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(get_difficulty_for_very_low_target)
{
    TestDifficulty(0x1f111111, 0.000001);
}

BOOST_AUTO_TEST_CASE(get_difficulty_for_low_target)
{
    TestDifficulty(0x1ef88f6f, 0.000016);
}

BOOST_AUTO_TEST_CASE(get_difficulty_for_mid_target)
{
    TestDifficulty(0x1df88f6f, 0.004023);
}

BOOST_AUTO_TEST_CASE(get_difficulty_for_high_target)
{
    TestDifficulty(0x1cf88f6f, 1.029916);
}

BOOST_AUTO_TEST_CASE(get_difficulty_for_very_high_target)
{
    TestDifficulty(0x12345678, 5913134931067755359633408.0);
}

BOOST_AUTO_TEST_CASE(get_target_for_b3_modern_phases)
{
    const Consensus::Params& consensus{Params().GetConsensus()};
    BOOST_REQUIRE(consensus.legacy_b3coin);
    BOOST_REQUIRE(consensus.hard_fork_height.has_value());
    BOOST_REQUIRE(consensus.transition_pow_bits.has_value());
    BOOST_REQUIRE(consensus.modern_pos.has_value());

    auto check_target = [&](const int height, const uint32_t bits) {
        CBlockIndex index;
        index.nHeight = height;
        index.nBits = bits;
        bool negative{false};
        bool overflow{false};
        arith_uint256 expected;
        expected.SetCompact(bits, &negative, &overflow);
        BOOST_REQUIRE(!negative);
        BOOST_REQUIRE(!overflow);
        BOOST_REQUIRE(expected != 0);
        BOOST_CHECK(GetTarget(index, consensus) == ArithToUint256(expected));
    };

    // Both values deliberately exceed the preserved legacy B3 PoW limit.
    check_target(*consensus.hard_fork_height, *consensus.transition_pow_bits);
    check_target(*Consensus::ModernPosStartHeight(consensus),
                 consensus.modern_pos->sentinel_bits);
}

BOOST_AUTO_TEST_CASE(next_block_info_for_b3_modern_phases)
{
    const Consensus::Params& consensus{Params().GetConsensus()};
    BOOST_REQUIRE(consensus.legacy_b3coin);
    BOOST_REQUIRE(consensus.hard_fork_height.has_value());
    BOOST_REQUIRE(consensus.transition_pow_bits.has_value());
    BOOST_REQUIRE(consensus.modern_pos.has_value());
    BOOST_REQUIRE(Consensus::TransitionPowFinalHeight(consensus).has_value());
    BOOST_REQUIRE(Consensus::ModernPosStartHeight(consensus).has_value());

    const int corridor_start{*consensus.hard_fork_height};
    const int corridor_end{*Consensus::TransitionPowFinalHeight(consensus)};
    const int modern_pos_start{*Consensus::ModernPosStartHeight(consensus)};

    auto check_next = [&](const int next_height, const uint32_t tip_bits,
                          const uint32_t expected_bits) {
        CBlockIndex tip;
        const uint256 tip_hash{uint256::ONE};
        tip.phashBlock = &tip_hash;
        tip.nHeight = next_height - 1;
        tip.nTime = 1;
        tip.nBits = tip_bits;

        CBlockIndex next;
        NextEmptyBlockIndex(tip, consensus, next);
        BOOST_CHECK_EQUAL(next.nHeight, next_height);
        BOOST_CHECK_EQUAL(next.nBits, expected_bits);
        BOOST_CHECK(Consensus::HasB3BlockCodecV2(next.nVersion));
    };

    // Cover the phase edges plus legacy-retarget boundaries within each
    // modern phase. Those boundaries must not alter B3's fixed values.
    check_next(corridor_start, 0x1a31bb15U, *consensus.transition_pow_bits);
    check_next(corridor_start + 19, *consensus.transition_pow_bits,
               *consensus.transition_pow_bits);
    check_next(corridor_end, *consensus.transition_pow_bits,
               *consensus.transition_pow_bits);
    check_next(modern_pos_start, *consensus.transition_pow_bits,
               consensus.modern_pos->sentinel_bits);
    check_next(modern_pos_start + 19, consensus.modern_pos->sentinel_bits,
               consensus.modern_pos->sentinel_bits);
}

//! Prune chain from height down to genesis block and check that
//! GetPruneHeight returns the correct value
static void CheckGetPruneHeight(const node::BlockManager& blockman, const CChain& chain, int height) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    AssertLockHeld(::cs_main);

    // Emulate pruning all blocks from `height` down to the genesis block
    // by unsetting the `BLOCK_HAVE_DATA` flag from `nStatus`
    for (CBlockIndex* it{chain[height]}; it != nullptr && it->nHeight > 0; it = it->pprev) {
        it->nStatus &= ~BLOCK_HAVE_DATA;
    }

    const auto prune_height{GetPruneHeight(blockman, chain)};
    BOOST_REQUIRE(prune_height.has_value());
    BOOST_CHECK_EQUAL(*prune_height, height);
}

BOOST_FIXTURE_TEST_CASE(get_prune_height, TestChain100Setup)
{
    LOCK(::cs_main);
    const auto& chain = m_node.chainman->ActiveChain();
    const auto& blockman = m_node.chainman->m_blockman;

    // TestChain100Setup mines COINBASE_MATURITY blocks. B3's historical
    // maturity is 30 rather than Bitcoin's 100, so derive the boundary from
    // the fixture instead of embedding the upstream height.
    const int tip_height{chain.Height()};
    BOOST_REQUIRE_GT(tip_height, 1);

    // Fresh fixture chain without any pruned blocks, so std::nullopt should be returned
    BOOST_CHECK(!GetPruneHeight(blockman, chain).has_value());

    // Start pruning
    CheckGetPruneHeight(blockman, chain, 1);
    CheckGetPruneHeight(blockman, chain, tip_height - 1);
    CheckGetPruneHeight(blockman, chain, tip_height);
}

BOOST_AUTO_TEST_CASE(num_chain_tx_max)
{
    CBlockIndex block_index{};
    block_index.m_chain_tx_count = std::numeric_limits<uint64_t>::max();
    BOOST_CHECK_EQUAL(block_index.m_chain_tx_count, std::numeric_limits<uint64_t>::max());
}

BOOST_FIXTURE_TEST_CASE(invalidate_block, TestChain100Setup)
{
    const CChain& active{*WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return &Assert(m_node.chainman)->ActiveChain())};

    // Check BlockStatus when doing InvalidateBlock()
    BlockValidationState state;
    auto* orig_tip = active.Tip();
    int height_to_invalidate = orig_tip->nHeight - 10;
    auto* tip_to_invalidate = active[height_to_invalidate];
    m_node.chainman->ActiveChainstate().InvalidateBlock(state, tip_to_invalidate);

    // tip_to_invalidate just got invalidated, so it's BLOCK_FAILED_VALID
    WITH_LOCK(::cs_main, assert(tip_to_invalidate->nStatus & BLOCK_FAILED_VALID));

    // check all ancestors of the invalidated block are validated up to BLOCK_VALID_TRANSACTIONS and are not invalid
    auto pindex = tip_to_invalidate->pprev;
    while (pindex) {
        WITH_LOCK(::cs_main, assert(pindex->IsValid(BLOCK_VALID_TRANSACTIONS)));
        WITH_LOCK(::cs_main, assert((pindex->nStatus & BLOCK_FAILED_VALID) == 0));
        pindex = pindex->pprev;
    }

    // check all descendants of the invalidated block are BLOCK_FAILED_VALID
    pindex = orig_tip;
    while (pindex && pindex != tip_to_invalidate) {
        WITH_LOCK(::cs_main, assert(pindex->nStatus & BLOCK_FAILED_VALID));
        pindex = pindex->pprev;
    }
}

BOOST_AUTO_TEST_SUITE_END()
