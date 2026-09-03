// Copyright (c) 2022-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <clientversion.h>
#include <consensus/block_codec.h>
#include <consensus/era.h>
#include <legacy/codec.h>
#include <legacy/consensus.h>
#include <modern/pos_v1.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <node/kernel_notifications.h>
#include <pow.h>
#include <script/solver.h>
#include <primitives/block.h>
#include <util/chaintype.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>
#include <test/util/common.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>

#include <deque>
#include <vector>

using kernel::CBlockFileInfo;
using kernel::BlockTreeDB;
using node::STORAGE_HEADER_BYTES;
using node::BlockManager;
using node::KernelNotifications;
using node::MAX_BLOCKFILE_SIZE;

// use BasicTestingSetup here for the data directory configuration, setup, and cleanup
BOOST_FIXTURE_TEST_SUITE(blockmanager_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(fn_pod_issued_total_sidecar_roundtrip)
{
    const CChainParams& params{Params()};
    const Consensus::Params& consensus{params.GetConsensus()};
    BlockTreeDB db{DBParams{
        .path = "",
        .cache_bytes = 1 << 20,
        .memory_only = true,
    }};

    const CBlock& genesis_block{params.GenesisBlock()};
    const uint256 genesis_hash{consensus.hashGenesisBlock};
    CBlockIndex stored{genesis_block};
    stored.phashBlock = &genesis_hash;
    stored.nHeight = 0;
    stored.m_fn_pod_issued_total = 37;
    stored.m_fn_pod_issued_total_known = true;
    db.WriteBatchSync({}, /*nLastFile=*/0, {&stored});

    node::BlockMap loaded;
    const auto insert{[&](const uint256& hash) -> CBlockIndex* {
        if (hash.IsNull()) return nullptr;
        const auto [it, inserted]{loaded.try_emplace(hash)};
        if (inserted) it->second.phashBlock = &it->first;
        return &it->second;
    }};
    BOOST_REQUIRE(WITH_LOCK(cs_main, return db.LoadBlockIndexGuts(consensus, insert, m_interrupt)));

    const auto it{loaded.find(genesis_hash)};
    BOOST_REQUIRE(it != loaded.end());
    BOOST_CHECK(it->second.m_fn_pod_issued_total_known);
    BOOST_CHECK_EQUAL(it->second.m_fn_pod_issued_total, 37U);

    // A known zero remains distinct from a missing/unknown sidecar.
    stored.m_fn_pod_issued_total = 0;
    db.WriteBatchSync({}, /*nLastFile=*/0, {&stored});
    node::BlockMap zero_loaded;
    const auto zero_insert{[&](const uint256& hash) -> CBlockIndex* {
        if (hash.IsNull()) return nullptr;
        const auto [zero_it, inserted]{zero_loaded.try_emplace(hash)};
        if (inserted) zero_it->second.phashBlock = &zero_it->first;
        return &zero_it->second;
    }};
    BOOST_REQUIRE(WITH_LOCK(cs_main, return db.LoadBlockIndexGuts(consensus, zero_insert, m_interrupt)));
    const auto zero_it{zero_loaded.find(genesis_hash)};
    BOOST_REQUIRE(zero_it != zero_loaded.end());
    BOOST_CHECK(zero_it->second.m_fn_pod_issued_total_known);
    BOOST_CHECK_EQUAL(zero_it->second.m_fn_pod_issued_total, 0U);

    // An old database without the sidecar remains readable.
    BlockTreeDB old_db{DBParams{
        .path = "",
        .cache_bytes = 1 << 20,
        .memory_only = true,
    }};
    stored.m_fn_pod_issued_total = 0;
    stored.m_fn_pod_issued_total_known = false;
    old_db.WriteBatchSync({}, /*nLastFile=*/0, {&stored});
    node::BlockMap old_loaded;
    const auto old_insert{[&](const uint256& hash) -> CBlockIndex* {
        if (hash.IsNull()) return nullptr;
        const auto [old_it, inserted]{old_loaded.try_emplace(hash)};
        if (inserted) old_it->second.phashBlock = &old_it->first;
        return &old_it->second;
    }};
    BOOST_REQUIRE(WITH_LOCK(cs_main, return old_db.LoadBlockIndexGuts(consensus, old_insert, m_interrupt)));
    const auto old_it{old_loaded.find(genesis_hash)};
    BOOST_REQUIRE(old_it != old_loaded.end());
    BOOST_CHECK(!old_it->second.m_fn_pod_issued_total_known);
    BOOST_CHECK_EQUAL(old_it->second.m_fn_pod_issued_total, 0U);
}

BOOST_FIXTURE_TEST_CASE(block_index_restart_rejects_invalid_non_b3_pow,
                        ChainTestingSetup)
{
    auto& consensus{
        const_cast<Consensus::Params&>(Params().GetConsensus())};
    consensus.legacy_b3coin = false;

    CBlockHeader invalid_header;
    invalid_header.nVersion = 4;
    invalid_header.nTime = 1;
    invalid_header.nBits = 0; // Non-canonical and therefore never valid PoW.
    const uint256 invalid_hash{invalid_header.GetHash()};
    CBlockIndex stored{invalid_header};
    stored.phashBlock = &invalid_hash;
    stored.nHeight = 0;
    WITH_LOCK(cs_main, stored.nStatus = BLOCK_VALID_TREE);

    m_node.chainman.reset();
    m_make_chainman();
    ChainstateManager& chainman{*m_node.chainman};
    WITH_LOCK(cs_main,
              chainman.InitializeChainstate(Assert(m_node.mempool.get())));
    WITH_LOCK(cs_main,
              chainman.m_blockman.m_block_tree_db->WriteBatchSync(
                  {}, /*nLastFile=*/0, {&stored}));

    // The transition restart refactor must not bypass the ordinary PoW
    // validation retained by Bitcoin-style test/regtest networks.
    BOOST_CHECK(!WITH_LOCK(cs_main, return chainman.LoadBlockIndex()));
}

BOOST_FIXTURE_TEST_CASE(block_index_restart_quarantines_invalid_transition_branches,
                        ChainTestingSetup)
{
    auto& consensus{
        const_cast<Consensus::Params&>(Params().GetConsensus())};
    consensus.legacy_b3coin = true;
    consensus.hard_fork_height = 1; // synthetic H=0, first corridor height=1
    consensus.transition_pow_length = 1;
    consensus.transition_pow_bits = 0x207fffffU;
    consensus.transition_pow_min_spacing = 60;
    BOOST_REQUIRE(consensus.modern_pos.has_value());

    const CBlockHeader genesis{Params().GenesisBlock()};
    // Enabling the legacy B3 hash domain changes this synthetic genesis
    // identity from SHA256d to scrypt. Keep the chain parameter and H pin in
    // that same domain so the production loader can link height 1 to height 0.
    consensus.hashGenesisBlock = genesis.GetMarkerHash(consensus);
    consensus.legacy_final_hash = consensus.hashGenesisBlock;

    const auto make_corridor{[&](const uint32_t time) {
        CBlockHeader header;
        header.nVersion =
            static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
        header.hashPrevBlock = consensus.hashGenesisBlock;
        header.nTime = time;
        header.nBits = *consensus.transition_pow_bits;
        while (!CheckTransitionPowEligibility(header)) ++header.nNonce;
        return header;
    }};

    struct RestartResult {
        bool loaded{false};
        uint32_t tip_status{0};
    };

    // Persist exactly these headers, then use the production block-manager
    // restart loader (including its height-sorted second pass). Invalid
    // historical side branches are retained but quarantined, so they cannot
    // influence best-header or chain-candidate selection and cannot prevent a
    // node with an unrelated valid active chain from starting.
    const auto restart_loads{[&](const std::vector<CBlockHeader>& headers) {
        BOOST_TEST_CHECKPOINT("recreate chain manager");
        m_node.chainman.reset();
        m_make_chainman();
        ChainstateManager& chainman{*m_node.chainman};
        WITH_LOCK(cs_main,
                  chainman.InitializeChainstate(Assert(m_node.mempool.get())));

        BOOST_TEST_CHECKPOINT("derive stored hashes");
        std::vector<uint256> hashes;
        hashes.reserve(headers.size());
        for (const CBlockHeader& header : headers) {
            hashes.push_back(header.GetMarkerHash(consensus));
        }
        // CBlockIndex is intentionally non-movable, and its pprev pointers
        // must remain stable while the records are serialized.
        std::deque<CBlockIndex> stored;
        {
            LOCK(cs_main);
            for (size_t height{0}; height < headers.size(); ++height) {
                stored.emplace_back(headers[height]);
                stored.back().phashBlock = &hashes[height];
                stored.back().nHeight = static_cast<int>(height);
                stored.back().pprev = height == 0 ? nullptr : &stored[height - 1];
                stored.back().nStatus = BLOCK_VALID_TREE;
            }
        }
        std::vector<const CBlockIndex*> entries;
        entries.reserve(stored.size());
        for (const CBlockIndex& index : stored) entries.push_back(&index);
        BOOST_TEST_CHECKPOINT("write synthetic block index");
        WITH_LOCK(cs_main,
                  chainman.m_blockman.m_block_tree_db->WriteBatchSync(
                      {}, /*nLastFile=*/0, entries));
        BOOST_TEST_CHECKPOINT("load synthetic block index");
        RestartResult result;
        result.loaded = WITH_LOCK(cs_main, return chainman.LoadBlockIndex());
        if (result.loaded) {
            LOCK(cs_main);
            CBlockIndex* loaded_tip{
                chainman.m_blockman.LookupBlockIndex(hashes.back())};
            BOOST_REQUIRE(loaded_tip != nullptr);
            result.tip_status = loaded_tip->nStatus;
        }
        return result;
    }};

    const CBlockHeader valid_corridor{
        make_corridor(genesis.nTime + consensus.transition_pow_min_spacing)};
    RestartResult result{restart_loads({genesis, valid_corridor})};
    BOOST_REQUIRE(result.loaded);
    BOOST_CHECK(!(result.tip_status & BLOCK_FAILED_VALID));

    const CBlockHeader early_corridor{
        make_corridor(genesis.nTime + consensus.transition_pow_min_spacing - 1)};
    result = restart_loads({genesis, early_corridor});
    BOOST_REQUIRE(result.loaded);
    BOOST_CHECK(result.tip_status & BLOCK_FAILED_VALID);

    CBlockHeader wrong_bits{valid_corridor};
    wrong_bits.nBits = 0x1f008000U;
    result = restart_loads({genesis, wrong_bits});
    BOOST_REQUIRE(result.loaded);
    BOOST_CHECK(result.tip_status & BLOCK_FAILED_VALID);

    CBlockHeader valid_modern;
    valid_modern.nVersion =
        static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
    valid_modern.hashPrevBlock = valid_corridor.GetHash();
    valid_modern.nBits = consensus.modern_pos->sentinel_bits;
    valid_modern.nNonce = 0;
    valid_modern.nTime = modern::ModernPosBlockTime(
        valid_corridor.nTime, /*round=*/0, *consensus.modern_pos);
    result = restart_loads({genesis, valid_corridor, valid_modern});
    BOOST_REQUIRE(result.loaded);
    BOOST_CHECK(!(result.tip_status & BLOCK_FAILED_VALID));

    CBlockHeader off_round{valid_modern};
    ++off_round.nTime;
    result = restart_loads({genesis, valid_corridor, off_round});
    BOOST_REQUIRE(result.loaded);
    BOOST_CHECK(result.tip_status & BLOCK_FAILED_VALID);

    CBlockHeader nonzero_nonce{valid_modern};
    nonzero_nonce.nNonce = 1;
    result = restart_loads({genesis, valid_corridor, nonzero_nonce});
    BOOST_REQUIRE(result.loaded);
    BOOST_CHECK(result.tip_status & BLOCK_FAILED_VALID);
}

BOOST_FIXTURE_TEST_CASE(block_index_restart_retains_pre_pin_competing_boundary,
                        ChainTestingSetup)
{
    auto& consensus{
        const_cast<Consensus::Params&>(Params().GetConsensus())};
    consensus.legacy_b3coin = true;
    consensus.hard_fork_height = 2; // H=1, first corridor height=2
    consensus.transition_pow_length = 2;
    consensus.transition_pow_bits = 0x207fffffU;
    consensus.transition_pow_min_spacing = 60;

    const CBlockHeader genesis{Params().GenesisBlock()};
    consensus.hashGenesisBlock = genesis.GetMarkerHash(consensus);

    CBlockHeader canonical_h;
    canonical_h.nVersion = 4;
    canonical_h.hashPrevBlock = consensus.hashGenesisBlock;
    canonical_h.nTime = genesis.nTime + 1;
    canonical_h.nBits = genesis.nBits;
    const uint256 canonical_h_hash{canonical_h.GetMarkerHash(consensus)};
    consensus.legacy_final_hash = canonical_h_hash;

    CBlockHeader competing_h{canonical_h};
    ++competing_h.nNonce;
    const uint256 competing_h_hash{competing_h.GetMarkerHash(consensus)};
    BOOST_REQUIRE(competing_h_hash != canonical_h_hash);

    const auto make_corridor{[&](const uint256& parent, const uint32_t time) {
        CBlockHeader header;
        header.nVersion =
            static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
        header.hashPrevBlock = parent;
        header.nTime = time;
        header.nBits = *consensus.transition_pow_bits;
        while (!CheckTransitionPowEligibility(header)) ++header.nNonce;
        return header;
    }};
    const CBlockHeader canonical_corridor{make_corridor(
        canonical_h_hash,
        canonical_h.nTime + consensus.transition_pow_min_spacing)};
    const CBlockHeader early_corridor{make_corridor(
        canonical_h_hash,
        canonical_h.nTime + consensus.transition_pow_min_spacing - 1)};
    const CBlockHeader early_child{make_corridor(
        early_corridor.GetMarkerHash(consensus),
        early_corridor.nTime + consensus.transition_pow_min_spacing)};

    // This was a valid legacy extension before the boundary was pinned, but
    // its codec is no longer legal at H+1. Because its H parent is already off
    // X, both records are historical side-branch data, not startup failures.
    CBlockHeader competing_extension{competing_h};
    competing_extension.hashPrevBlock = competing_h_hash;
    competing_extension.nTime = competing_h.nTime + 1;
    ++competing_extension.nNonce;

    m_node.chainman.reset();
    m_make_chainman();
    ChainstateManager& chainman{*m_node.chainman};
    Chainstate& chainstate{WITH_LOCK(
        cs_main,
        return chainman.InitializeChainstate(Assert(m_node.mempool.get())))};

    std::deque<uint256> hashes;
    std::deque<CBlockIndex> stored;
    const auto add{[&](const CBlockHeader& header, const int height,
                       CBlockIndex* parent) EXCLUSIVE_LOCKS_REQUIRED(cs_main) {
        hashes.push_back(header.GetMarkerHash(consensus));
        stored.emplace_back(header);
        CBlockIndex& index{stored.back()};
        index.phashBlock = &hashes.back();
        index.nHeight = height;
        index.pprev = parent;
        index.nStatus = BLOCK_VALID_TREE;
        return &index;
    }};

    CBlockIndex* genesis_index;
    CBlockIndex* canonical_h_index;
    CBlockIndex* competing_h_index;
    CBlockIndex* canonical_index;
    CBlockIndex* early_index;
    CBlockIndex* early_child_index;
    CBlockIndex* competing_extension_index;
    {
        LOCK(cs_main);
        genesis_index = add(genesis, 0, nullptr);
        canonical_h_index = add(canonical_h, 1, genesis_index);
        competing_h_index = add(competing_h, 1, genesis_index);
        canonical_index = add(canonical_corridor, 2, canonical_h_index);
        early_index = add(early_corridor, 2, canonical_h_index);
        early_child_index = add(early_child, 3, early_index);
        competing_extension_index =
            add(competing_extension, 2, competing_h_index);
    }

    std::vector<const CBlockIndex*> entries;
    entries.reserve(stored.size());
    for (const CBlockIndex& index : stored) entries.push_back(&index);
    WITH_LOCK(cs_main,
              chainman.m_blockman.m_block_tree_db->WriteBatchSync(
                  {}, /*nLastFile=*/0, entries));

    BOOST_REQUIRE(WITH_LOCK(cs_main, return chainman.LoadBlockIndex()));
    chainstate.InitCoinsDB(/*cache_size_bytes=*/1 << 20,
                           /*in_memory=*/true,
                           /*should_wipe=*/false);
    {
        LOCK(cs_main);
        chainstate.InitCoinsCache(/*cache_size_bytes=*/1 << 20);
        const auto loaded{[&](const CBlockIndex* original)
                              EXCLUSIVE_LOCKS_REQUIRED(cs_main) {
            return Assert(chainman.m_blockman.LookupBlockIndex(
                original->GetBlockHash()));
        }};
        CBlockIndex* loaded_canonical{loaded(canonical_index)};
        CBlockIndex* loaded_competing_h{loaded(competing_h_index)};
        CBlockIndex* loaded_competing_extension{
            loaded(competing_extension_index)};
        CBlockIndex* loaded_early{loaded(early_index)};
        CBlockIndex* loaded_early_child{loaded(early_child_index)};

        // Restart must rebuild the whole skip list before anchor topology is
        // classified. Otherwise querying the not-yet-visited boundary anchor
        // once per historical entry degenerates into quadratic parent walks.
        for (const auto& [_, index] : chainman.m_blockman.m_block_index) {
            if (index.pprev) BOOST_CHECK(index.pskip != nullptr);
        }

        BOOST_CHECK(!(loaded_canonical->nStatus &
                      (BLOCK_FAILED_VALID | BLOCK_ANCHOR_INELIGIBLE)));
        BOOST_CHECK(loaded_competing_h->nStatus &
                    BLOCK_ANCHOR_INELIGIBLE);
        BOOST_CHECK(loaded_competing_extension->nStatus &
                    BLOCK_ANCHOR_INELIGIBLE);
        BOOST_CHECK(!(loaded_competing_h->nStatus & BLOCK_FAILED_VALID));
        BOOST_CHECK(loaded_early->nStatus & BLOCK_FAILED_VALID);
        BOOST_CHECK(loaded_early_child->nStatus & BLOCK_FAILED_VALID);
        BOOST_CHECK(!(loaded_early->nStatus & BLOCK_ANCHOR_INELIGIBLE));
        BOOST_CHECK(!(loaded_early_child->nStatus &
                      BLOCK_ANCHOR_INELIGIBLE));
        BOOST_CHECK_EQUAL(chainman.m_best_header, loaded_canonical);

        // Retaining an invalid side branch is safe only if the coins database
        // is not based on it. The same failed flag which excludes candidates
        // must stop an old active coins tip from being trusted, while the
        // canonical branch remains loadable.
        chainstate.CoinsTip().SetBestBlock(
            loaded_early_child->GetBlockHash());
        BOOST_CHECK(!chainstate.LoadChainTip());
        chainstate.CoinsTip().SetBestBlock(loaded_canonical->GetBlockHash());
        BOOST_CHECK(chainstate.LoadChainTip());
    }
}

BOOST_AUTO_TEST_CASE(blockmanager_find_block_pos)
{
    const auto params {CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    KernelNotifications notifications{Assert(m_node.shutdown_request), m_node.exit_status, *Assert(m_node.warnings)};
    const BlockManager::Options blockman_opts{
        .chainparams = *params,
        .blocks_dir = m_args.GetBlocksDirPath(),
        .notifications = notifications,
        .block_tree_db_params = DBParams{
            .path = m_args.GetDataDirNet() / "blocks" / "index",
            .cache_bytes = 0,
        },
    };
    BlockManager blockman{*Assert(m_node.shutdown_signal), blockman_opts};
    // simulate adding a genesis block normally
    BOOST_CHECK_EQUAL(blockman.WriteBlock(params->GenesisBlock(), 0).nPos, STORAGE_HEADER_BYTES);
    // simulate what happens during reindex
    // simulate a well-formed genesis block being found at offset 8 in the blk00000.dat file
    // the block is found at offset 8 because there is an 8 byte serialization header
    // consisting of 4 magic bytes + 4 length bytes before each block in a well-formed blk file.
    const FlatFilePos pos{0, STORAGE_HEADER_BYTES};
    blockman.UpdateBlockInfo(params->GenesisBlock(), 0, pos);
    // now simulate what happens after reindex for the first new block processed
    // the actual block contents don't matter, just that it's a block.
    // verify that the write position is at offset 0x12d.
    // this is a check to make sure that https://github.com/bitcoin/bitcoin/issues/21379 does not recur
    // 8 bytes (for serialization header) + 285 (for serialized genesis block) = 293
    // add another 8 bytes for the second block's serialization header and we get 293 + 8 = 301
    FlatFilePos actual{blockman.WriteBlock(params->GenesisBlock(), 1)};
    // Legacy-chain block files store the historical B3 encoding.
    BOOST_CHECK_EQUAL(actual.nPos, STORAGE_HEADER_BYTES + ::GetSerializeSize(legacy::TX_LEGACY(params->GenesisBlock())) + STORAGE_HEADER_BYTES);
}

BOOST_AUTO_TEST_CASE(legacy_genesis_reindex_initializes_stake_modifier)
{
    const CChainParams& params{Params()};
    KernelNotifications notifications{Assert(m_node.shutdown_request), m_node.exit_status, *Assert(m_node.warnings)};
    const BlockManager::Options blockman_opts{
        .chainparams = params,
        .blocks_dir = m_args.GetBlocksDirPath(),
        .notifications = notifications,
        .block_tree_db_params = DBParams{
            .path = m_args.GetDataDirNet() / "blocks" / "index",
            .cache_bytes = 0,
        },
    };
    BlockManager blockman{*Assert(m_node.shutdown_signal), blockman_opts};

    CBlockIndex* best_header{nullptr};
    CBlockIndex* genesis{nullptr};
    {
        LOCK(cs_main);
        genesis = blockman.AddToBlockIndex(params.GenesisBlock(), best_header);
    }

    BOOST_REQUIRE(genesis);
    BOOST_CHECK(genesis->m_legacy_stake_modifier_generated);
    BOOST_CHECK_EQUAL(genesis->m_legacy_stake_modifier, 0U);
    BOOST_CHECK_EQUAL(genesis->m_legacy_hash_proof, params.GetConsensus().hashGenesisBlock);

    uint64_t stake_modifier{0};
    bool generated{true};
    // This is the exact inherited-modifier calculation block 1 performs.
    BOOST_CHECK(legacy::ComputeNextStakeModifier(genesis, stake_modifier, generated));
    BOOST_CHECK_EQUAL(stake_modifier, 0U);
    BOOST_CHECK(!generated);
}

BOOST_FIXTURE_TEST_CASE(blockmanager_scan_unlink_already_pruned_files, TestChain100Setup)
{
    // Cap last block file size, and mine new block in a new block file.
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{chainman.m_blockman};
    const CBlockIndex* old_tip{WITH_LOCK(chainman.GetMutex(), return chainman.ActiveChain().Tip())};
    WITH_LOCK(chainman.GetMutex(), blockman.GetBlockFileInfo(old_tip->GetBlockPos().nFile)->nSize = MAX_BLOCKFILE_SIZE);
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

    // Prune the older block file, but don't unlink it
    int file_number;
    {
        LOCK(chainman.GetMutex());
        file_number = old_tip->GetBlockPos().nFile;
        blockman.PruneOneBlockFile(file_number);
    }

    const FlatFilePos pos(file_number, 0);

    // Check that the file is not unlinked after ScanAndUnlinkAlreadyPrunedFiles
    // if m_have_pruned is not yet set
    WITH_LOCK(chainman.GetMutex(), blockman.ScanAndUnlinkAlreadyPrunedFiles());
    BOOST_CHECK(!blockman.OpenBlockFile(pos, true).IsNull());

    // Check that the file is unlinked after ScanAndUnlinkAlreadyPrunedFiles
    // once m_have_pruned is set
    blockman.m_have_pruned = true;
    WITH_LOCK(chainman.GetMutex(), blockman.ScanAndUnlinkAlreadyPrunedFiles());
    BOOST_CHECK(blockman.OpenBlockFile(pos, true).IsNull());

    // Check that calling with already pruned files doesn't cause an error
    WITH_LOCK(chainman.GetMutex(), blockman.ScanAndUnlinkAlreadyPrunedFiles());

    // Check that the new tip file has not been removed
    const CBlockIndex* new_tip{WITH_LOCK(chainman.GetMutex(), return chainman.ActiveChain().Tip())};
    BOOST_CHECK_NE(old_tip, new_tip);
    const int new_file_number{WITH_LOCK(chainman.GetMutex(), return new_tip->GetBlockPos().nFile)};
    const FlatFilePos new_pos(new_file_number, 0);
    BOOST_CHECK(!blockman.OpenBlockFile(new_pos, true).IsNull());
}

BOOST_FIXTURE_TEST_CASE(blockmanager_block_data_availability, TestChain100Setup)
{
    // The goal of the function is to return the first not pruned block in the range [upper_block, lower_block].
    LOCK(::cs_main);
    auto& chainman = m_node.chainman;
    auto& blockman = chainman->m_blockman;
    const CBlockIndex& tip = *chainman->ActiveTip();

    // Function to prune all blocks from 'last_pruned_block' down to the genesis block
    const auto& func_prune_blocks = [&](CBlockIndex* last_pruned_block)
    {
        LOCK(::cs_main);
        CBlockIndex* it = last_pruned_block;
        while (it != nullptr && it->nStatus & BLOCK_HAVE_DATA) {
            it->nStatus &= ~BLOCK_HAVE_DATA;
            it = it->pprev;
        }
    };

    // 1) Return genesis block when all blocks are available
    BOOST_CHECK_EQUAL(&blockman.GetFirstBlock(tip, BLOCK_HAVE_DATA), chainman->ActiveChain()[0]);
    BOOST_CHECK(blockman.CheckBlockDataAvailability(tip, *chainman->ActiveChain()[0]));

    // 2) Check lower_block when all blocks are available
    CBlockIndex* lower_block = chainman->ActiveChain()[tip.nHeight / 2];
    BOOST_CHECK(blockman.CheckBlockDataAvailability(tip, *lower_block));

    // Ensure we don't fail due to the expected absence of undo data in the genesis block
    CBlockIndex* upper_block = chainman->ActiveChain()[2];
    CBlockIndex* genesis = chainman->ActiveChain()[0];
    BOOST_CHECK(blockman.CheckBlockDataAvailability(*upper_block, *genesis, BlockStatus{BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO}));
    // Ensure we detect absence of undo data in the first block
    chainman->ActiveChain()[1]->nStatus &= ~BLOCK_HAVE_UNDO;
    BOOST_CHECK(!blockman.CheckBlockDataAvailability(tip, *genesis, BlockStatus{BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO}));

    // Prune half of the blocks
    int height_to_prune = tip.nHeight / 2;
    CBlockIndex* first_available_block = chainman->ActiveChain()[height_to_prune + 1];
    CBlockIndex* last_pruned_block = first_available_block->pprev;
    func_prune_blocks(last_pruned_block);

    // 3) The last block not pruned is in-between upper-block and the genesis block
    BOOST_CHECK_EQUAL(&blockman.GetFirstBlock(tip, BLOCK_HAVE_DATA), first_available_block);
    BOOST_CHECK(blockman.CheckBlockDataAvailability(tip, *first_available_block));
    BOOST_CHECK(!blockman.CheckBlockDataAvailability(tip, *last_pruned_block));

    // Simulate that the first available block is missing undo data and
    // detect this by using a status mask.
    first_available_block->nStatus &= ~BLOCK_HAVE_UNDO;
    BOOST_CHECK(!blockman.CheckBlockDataAvailability(tip, *first_available_block, BlockStatus{BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO}));
    BOOST_CHECK(blockman.CheckBlockDataAvailability(tip, *first_available_block, BlockStatus{BLOCK_HAVE_DATA}));
}

BOOST_FIXTURE_TEST_CASE(blockmanager_block_data_part, TestChain100Setup)
{
    LOCK(::cs_main);
    auto& chainman{m_node.chainman};
    auto& blockman{chainman->m_blockman};
    const CBlockIndex& tip{*chainman->ActiveTip()};
    const FlatFilePos tip_block_pos{tip.GetBlockPos()};

    auto block{blockman.ReadRawBlock(tip_block_pos)};
    BOOST_REQUIRE(block);
    BOOST_REQUIRE_GE(block->size(), 200);

    const auto expect_part{[&](size_t offset, size_t size) {
        auto res{blockman.ReadRawBlock(tip_block_pos, std::pair{offset, size})};
        BOOST_CHECK(res);
        const auto& part{res.value()};
        BOOST_CHECK_EQUAL_COLLECTIONS(part.begin(), part.end(), block->begin() + offset, block->begin() + offset + size);
    }};

    expect_part(0, 20);
    expect_part(0, block->size() - 1);
    expect_part(0, block->size() - 10);
    expect_part(0, block->size());
    expect_part(1, block->size() - 1);
    expect_part(10, 20);
    expect_part(block->size() - 1, 1);
}

BOOST_FIXTURE_TEST_CASE(blockmanager_block_data_part_error, TestChain100Setup)
{
    LOCK(::cs_main);
    auto& chainman{m_node.chainman};
    auto& blockman{chainman->m_blockman};
    const CBlockIndex& tip{*chainman->ActiveTip()};
    const FlatFilePos tip_block_pos{tip.GetBlockPos()};

    auto block{blockman.ReadRawBlock(tip_block_pos)};
    BOOST_REQUIRE(block);
    BOOST_REQUIRE_GE(block->size(), 200);

    const auto expect_part_error{[&](size_t offset, size_t size) {
        auto res{blockman.ReadRawBlock(tip_block_pos, std::pair{offset, size})};
        BOOST_CHECK(!res);
        BOOST_CHECK_EQUAL(res.error(), node::ReadRawError::BadPartRange);
    }};

    expect_part_error(0, 0);
    expect_part_error(0, block->size() + 1);
    expect_part_error(0, std::numeric_limits<size_t>::max());
    expect_part_error(1, block->size());
    expect_part_error(2, block->size() - 1);
    expect_part_error(block->size() - 1, 2);
    expect_part_error(block->size() - 2, 3);
    expect_part_error(block->size() + 1, 0);
    expect_part_error(block->size() + 1, 1);
    expect_part_error(block->size() + 2, 2);
    expect_part_error(block->size(), 0);
    expect_part_error(block->size(), 1);
    expect_part_error(std::numeric_limits<size_t>::max(), 1);
    expect_part_error(std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max());
}

BOOST_FIXTURE_TEST_CASE(blockmanager_readblock_hash_mismatch, TestingSetup)
{
    CBlockIndex index;
    {
        LOCK(cs_main);
        const auto tip{m_node.chainman->ActiveTip()};
        index.nStatus = tip->nStatus;
        index.nDataPos = tip->nDataPos;
        index.phashBlock = &uint256::ONE; // mismatched block hash
    }

    ASSERT_DEBUG_LOG("GetHash() doesn't match index");
    CBlock block;
    BOOST_CHECK(!m_node.chainman->m_blockman.ReadBlock(block, index));
}

BOOST_AUTO_TEST_CASE(blockmanager_flush_block_file)
{
    KernelNotifications notifications{Assert(m_node.shutdown_request), m_node.exit_status, *Assert(m_node.warnings)};
    node::BlockManager::Options blockman_opts{
        .chainparams = Params(),
        .blocks_dir = m_args.GetBlocksDirPath(),
        .notifications = notifications,
        .block_tree_db_params = DBParams{
            .path = m_args.GetDataDirNet() / "blocks" / "index",
            .cache_bytes = 0,
        },
    };
    BlockManager blockman{*Assert(m_node.shutdown_signal), blockman_opts};

    // Test blocks with no transactions, not even a coinbase
    CBlock block1;
    block1.nVersion = 1;
    CBlock block2;
    block2.nVersion = 2;
    CBlock block3;
    block3.nVersion = 3;

    // They are 80 bytes header + 1 byte 0x00 for vtx length
    // 80-byte header + 1-byte empty vtx count + 1-byte empty legacy block
    // signature: this fixture writes blocks in the legacy-chain encoding.
    constexpr int TEST_BLOCK_SIZE{82};

    // Blockstore is empty
    BOOST_CHECK_EQUAL(blockman.CalculateCurrentUsage(), 0);

    // Write the first block to a new location.
    FlatFilePos pos1{blockman.WriteBlock(block1, /*nHeight=*/1)};

    // Write second block
    FlatFilePos pos2{blockman.WriteBlock(block2, /*nHeight=*/2)};

    // Two blocks in the file
    BOOST_CHECK_EQUAL(blockman.CalculateCurrentUsage(), (TEST_BLOCK_SIZE + STORAGE_HEADER_BYTES) * 2);

    // First two blocks are written as expected. On a pinned B3 chain,
    // historical headers are attested by (H, X) and ReadBlock deliberately
    // does not re-judge their claimed proof of work. Other networks retain
    // the original invalid-header expectation.
    CBlock read_block;
    BOOST_CHECK_EQUAL(read_block.nVersion, 0);
    const bool pinned_legacy{
        Params().GetConsensus().legacy_b3coin &&
        Consensus::LegacyBoundaryPinned(Params().GetConsensus())};
    if (pinned_legacy) {
        BOOST_CHECK(blockman.ReadBlock(read_block, pos1, {}));
        BOOST_CHECK_EQUAL(read_block.nVersion, 1);
    } else {
        ASSERT_DEBUG_LOG("Errors in block header");
        BOOST_CHECK(!blockman.ReadBlock(read_block, pos1, {}));
        BOOST_CHECK_EQUAL(read_block.nVersion, 1);
    }
    if (pinned_legacy) {
        BOOST_CHECK(blockman.ReadBlock(read_block, pos2, {}));
        BOOST_CHECK_EQUAL(read_block.nVersion, 2);
    } else {
        ASSERT_DEBUG_LOG("Errors in block header");
        BOOST_CHECK(!blockman.ReadBlock(read_block, pos2, {}));
        BOOST_CHECK_EQUAL(read_block.nVersion, 2);
    }

    // During reindex, the flat file block storage will not be written to.
    // UpdateBlockInfo will, however, update the blockfile metadata.
    // Verify this behavior by attempting (and failing) to write block 3 data
    // to block 2 location.
    CBlockFileInfo* block_data = blockman.GetBlockFileInfo(0);
    BOOST_CHECK_EQUAL(block_data->nBlocks, 2);
    blockman.UpdateBlockInfo(block3, /*nHeight=*/3, /*pos=*/pos2);
    // Metadata is updated...
    BOOST_CHECK_EQUAL(block_data->nBlocks, 3);
    // ...but there are still only two blocks in the file
    BOOST_CHECK_EQUAL(blockman.CalculateCurrentUsage(), (TEST_BLOCK_SIZE + STORAGE_HEADER_BYTES) * 2);

    // Block 2 was not overwritten:
    BOOST_CHECK_EQUAL(blockman.ReadBlock(read_block, pos2, {}), pinned_legacy);
    BOOST_CHECK_EQUAL(read_block.nVersion, 2);
}

BOOST_AUTO_TEST_SUITE_END()
