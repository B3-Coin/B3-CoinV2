// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/validation.h>
#include <interfaces/chain.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <script/solver.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

using interfaces::FoundBlock;

// TestChain100Setup retains its upstream name but mines COINBASE_MATURITY
// blocks, so interface ranges must follow the actual fixture tip.
BOOST_FIXTURE_TEST_SUITE(interfaces_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(findBlock)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();
    const int tip_height = active.Height();
    BOOST_REQUIRE(chain);
    BOOST_REQUIRE_GE(tip_height, 6);

    const int hash_height = tip_height / 6;
    const int height_height = tip_height * 2 / 6;
    const int data_height = tip_height * 3 / 6;
    const int time_height = tip_height * 4 / 6;
    const int max_time_height = tip_height * 5 / 6;
    const int mtp_height = tip_height;

    uint256 hash;
    BOOST_CHECK(chain->findBlock(active[hash_height]->GetBlockHash(), FoundBlock().hash(hash)));
    BOOST_CHECK_EQUAL(hash, active[hash_height]->GetBlockHash());

    int height = -1;
    BOOST_CHECK(chain->findBlock(active[height_height]->GetBlockHash(), FoundBlock().height(height)));
    BOOST_CHECK_EQUAL(height, active[height_height]->nHeight);

    CBlock data;
    BOOST_CHECK(chain->findBlock(active[data_height]->GetBlockHash(), FoundBlock().data(data)));
    BOOST_CHECK_EQUAL(data.GetHash(), active[data_height]->GetBlockHash());

    int64_t time = -1;
    BOOST_CHECK(chain->findBlock(active[time_height]->GetBlockHash(), FoundBlock().time(time)));
    BOOST_CHECK_EQUAL(time, active[time_height]->GetBlockTime());

    int64_t max_time = -1;
    BOOST_CHECK(chain->findBlock(active[max_time_height]->GetBlockHash(), FoundBlock().maxTime(max_time)));
    BOOST_CHECK_EQUAL(max_time, active[max_time_height]->GetBlockTimeMax());

    int64_t mtp_time = -1;
    BOOST_CHECK(chain->findBlock(active[mtp_height]->GetBlockHash(), FoundBlock().mtpTime(mtp_time)));
    BOOST_CHECK_EQUAL(mtp_time, active[mtp_height]->GetMedianTimePast());

    bool cur_active{false}, next_active{false};
    uint256 next_hash;
    BOOST_CHECK(chain->findBlock(active[tip_height - 1]->GetBlockHash(), FoundBlock().inActiveChain(cur_active).nextBlock(FoundBlock().inActiveChain(next_active).hash(next_hash))));
    BOOST_CHECK(cur_active);
    BOOST_CHECK(next_active);
    BOOST_CHECK_EQUAL(next_hash, active[tip_height]->GetBlockHash());
    cur_active = next_active = false;
    BOOST_CHECK(chain->findBlock(active[tip_height]->GetBlockHash(), FoundBlock().inActiveChain(cur_active).nextBlock(FoundBlock().inActiveChain(next_active))));
    BOOST_CHECK(cur_active);
    BOOST_CHECK(!next_active);

    BOOST_CHECK(!chain->findBlock({}, FoundBlock()));
}

BOOST_AUTO_TEST_CASE(findFirstBlockWithTimeAndHeight)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();
    BOOST_REQUIRE_GE(active.Height(), 6);
    const int min_height = active.Height() / 6;
    uint256 hash;
    int height;
    BOOST_CHECK(chain->findFirstBlockWithTimeAndHeight(/* min_time= */ 0, min_height, FoundBlock().hash(hash).height(height)));
    BOOST_CHECK_EQUAL(hash, active[min_height]->GetBlockHash());
    BOOST_CHECK_EQUAL(height, min_height);
    BOOST_CHECK(!chain->findFirstBlockWithTimeAndHeight(/* min_time= */ active.Tip()->GetBlockTimeMax() + 1, /* min_height= */ 0));
}

BOOST_AUTO_TEST_CASE(findAncestorByHeight)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();
    BOOST_REQUIRE_GE(active.Height(), 3);
    const int ancestor_height = active.Height() / 3;
    const int descendant_height = active.Height() * 2 / 3;
    uint256 hash;
    BOOST_CHECK(chain->findAncestorByHeight(active[descendant_height]->GetBlockHash(), ancestor_height, FoundBlock().hash(hash)));
    BOOST_CHECK_EQUAL(hash, active[ancestor_height]->GetBlockHash());
    BOOST_CHECK(!chain->findAncestorByHeight(active[ancestor_height]->GetBlockHash(), descendant_height));
}

BOOST_AUTO_TEST_CASE(findAncestorByHash)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();
    BOOST_REQUIRE_GE(active.Height(), 3);
    const int ancestor_height = active.Height() / 3;
    const int descendant_height = active.Height() * 2 / 3;
    int height = -1;
    BOOST_CHECK(chain->findAncestorByHash(active[descendant_height]->GetBlockHash(), active[ancestor_height]->GetBlockHash(), FoundBlock().height(height)));
    BOOST_CHECK_EQUAL(height, ancestor_height);
    BOOST_CHECK(!chain->findAncestorByHash(active[ancestor_height]->GetBlockHash(), active[descendant_height]->GetBlockHash()));
}

BOOST_AUTO_TEST_CASE(findCommonAncestor)
{
    auto& chain = m_node.chain;
    const CChain& active{*WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return &Assert(m_node.chainman)->ActiveChain())};
    auto* orig_tip = active.Tip();
    BOOST_REQUIRE(orig_tip);
    BOOST_REQUIRE_GE(orig_tip->nHeight, 3);
    const int fork_depth = orig_tip->nHeight / 3;
    for (int i = 0; i < fork_depth; ++i) {
        BlockValidationState state;
        m_node.chainman->ActiveChainstate().InvalidateBlock(state, active.Tip());
    }
    BOOST_CHECK_EQUAL(active.Height(), orig_tip->nHeight - fork_depth);
    coinbaseKey.MakeNewKey(true);
    for (int i = 0; i < fork_depth * 2; ++i) {
        CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    }
    BOOST_CHECK_EQUAL(active.Height(), orig_tip->nHeight + fork_depth);
    uint256 fork_hash;
    int fork_height;
    int orig_height;
    BOOST_CHECK(chain->findCommonAncestor(orig_tip->GetBlockHash(), active.Tip()->GetBlockHash(), FoundBlock().height(fork_height).hash(fork_hash), FoundBlock().height(orig_height)));
    BOOST_CHECK_EQUAL(orig_height, orig_tip->nHeight);
    BOOST_CHECK_EQUAL(fork_height, orig_tip->nHeight - fork_depth);
    BOOST_CHECK_EQUAL(fork_hash, active[fork_height]->GetBlockHash());

    uint256 active_hash, orig_hash;
    BOOST_CHECK(!chain->findCommonAncestor(active.Tip()->GetBlockHash(), {}, {}, FoundBlock().hash(active_hash), {}));
    BOOST_CHECK(!chain->findCommonAncestor({}, orig_tip->GetBlockHash(), {}, {}, FoundBlock().hash(orig_hash)));
    BOOST_CHECK_EQUAL(active_hash, active.Tip()->GetBlockHash());
    BOOST_CHECK_EQUAL(orig_hash, orig_tip->GetBlockHash());
}

BOOST_AUTO_TEST_CASE(hasBlocks)
{
    LOCK(::cs_main);
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();
    const int tip_height = active.Height();
    BOOST_REQUIRE_GE(tip_height, 10);

    const int min_height = tip_height / 10;
    const int max_height = tip_height * 9 / 10;
    const int below_min = min_height - 1;
    const int inside_range = (min_height + max_height) / 2;
    const int above_max = max_height + 1;

    // Test ranges
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), min_height, max_height));
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), min_height, {}));
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 0, max_height));
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 0, {}));
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), -1000, 1000));
    active[below_min]->nStatus &= ~BLOCK_HAVE_DATA;
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), min_height, max_height));
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), min_height, {}));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, max_height));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, {}));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), -1000, 1000));
    active[above_max]->nStatus &= ~BLOCK_HAVE_DATA;
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), min_height, max_height));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), min_height, {}));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, max_height));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, {}));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), -1000, 1000));
    active[inside_range]->nStatus &= ~BLOCK_HAVE_DATA;
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), min_height, max_height));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), min_height, {}));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, max_height));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, {}));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), -1000, 1000));

    // Test edge cases
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), below_min + 1, inside_range - 1));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), below_min, inside_range - 1));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), below_min + 1, inside_range));
}

BOOST_AUTO_TEST_SUITE_END()
