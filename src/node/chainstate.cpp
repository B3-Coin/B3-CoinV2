// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/chainstate.h>

#include <arith_uint256.h>
#include <chain.h>
#include <coins.h>
#include <consensus/era.h>
#include <consensus/params.h>
#include <kernel/caches.h>
#include <node/blockstorage.h>
#include <sync.h>
#include <threadsafety.h>
#include <tinyformat.h>
#include <txdb.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/log.h>
#include <util/signalinterrupt.h>
#include <util/time.h>
#include <util/translation.h>
#include <validation.h>

#include <algorithm>
#include <cassert>
#include <vector>

using kernel::CacheSizes;

namespace node {
// Complete initialization of chainstates after the initial call has been made
// to ChainstateManager::InitializeChainstate().
static ChainstateLoadResult CompleteChainstateInitialization(
    ChainstateManager& chainman,
    const ChainstateLoadOptions& options) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    if (chainman.m_interrupt) return {ChainstateLoadStatus::INTERRUPTED, {}};

    // LoadBlockIndex will load m_have_pruned if we've ever removed a
    // block file from disk.
    // Note that it also sets m_blockfiles_indexed based on the disk flag!
    if (!chainman.LoadBlockIndex()) {
        if (chainman.m_interrupt) return {ChainstateLoadStatus::INTERRUPTED, {}};
        // The transition release persists additional B3 block-index fields.
        // Current transition-beta databases already use this layout and are
        // compatible. Much older legacy or otherwise incompatible unversioned
        // records cannot be decoded safely; rebuilding the index from the
        // original block files is the correct recovery.
        if (chainman.GetConsensus().legacy_b3coin) {
            return {ChainstateLoadStatus::FAILURE,
                    _("Error loading the B3 block database. Current transition-beta databases do not require a reindex solely for this update. If this data directory was created by a much older legacy or incompatible release, restart with -reindex to rebuild the block index; otherwise check the disk and block files for corruption.")};
        }
        return {ChainstateLoadStatus::FAILURE, _("Error loading block database")};
    }

    if (!chainman.BlockIndex().empty() &&
            !chainman.m_blockman.LookupBlockIndex(chainman.GetConsensus().hashGenesisBlock)) {
        // If the loaded chain has a wrong genesis, bail out immediately
        // (we're likely using a testnet datadir, or the other way around).
        return {ChainstateLoadStatus::FAILURE_INCOMPATIBLE_DB, _("Incorrect or no genesis block found. Wrong datadir for network?")};
    }

    // Check for changed -prune state.  What we are concerned about is a user who has pruned blocks
    // in the past, but is now trying to run unpruned.
    if (chainman.m_blockman.m_have_pruned && !options.prune) {
        return {ChainstateLoadStatus::FAILURE, _("You need to rebuild the database using -reindex to go back to unpruned mode.  This will redownload the entire blockchain")};
    }

    // At this point blocktree args are consistent with what's on disk.
    // If we're not mid-reindex (based on disk + args), add a genesis block on disk
    // (otherwise we use the one already on disk).
    // This is called again in ImportBlocks after the reindex completes.
    if (chainman.m_blockman.m_blockfiles_indexed && !chainman.ActiveChainstate().LoadGenesisBlock()) {
        return {ChainstateLoadStatus::FAILURE, _("Error initializing block database")};
    }

    auto is_coinsview_empty = [&](Chainstate& chainstate) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        return options.wipe_chainstate_db || chainstate.CoinsTip().GetBestBlock().IsNull();
    };

    assert(chainman.m_total_coinstip_cache > 0);
    assert(chainman.m_total_coinsdb_cache > 0);

    // If running with multiple chainstates, limit the cache sizes with a
    // discount factor. If discounted the actual cache size will be
    // recalculated by `chainman.MaybeRebalanceCaches()`. The discount factor
    // is conservatively chosen such that the sum of the caches does not exceed
    // the allowable amount during this temporary initialization state.
    double init_cache_fraction = chainman.HistoricalChainstate() ? 0.2 : 1.0;

    // At this point we're either in reindex or we've loaded a useful
    // block tree into BlockIndex()!

    for (const auto& chainstate : chainman.m_chainstates) {
        LogInfo("Initializing chainstate %s", chainstate->ToString());

        try {
            chainstate->InitCoinsDB(
                /*cache_size_bytes=*/chainman.m_total_coinsdb_cache * init_cache_fraction,
                /*in_memory=*/options.coins_db_in_memory,
                /*should_wipe=*/options.wipe_chainstate_db);
        } catch (dbwrapper_error& err) {
            LogError("%s\n", err.what());
            return {ChainstateLoadStatus::FAILURE, _("Error opening coins database")};
        }

        if (options.coins_error_cb) {
            chainstate->CoinsErrorCatcher().AddReadErrCallback(options.coins_error_cb);
        }

        // Refuse to load unsupported database format.
        // This is a no-op if we cleared the coinsviewdb with -reindex or -reindex-chainstate
        if (chainstate->CoinsDB().NeedsUpgrade()) {
            return {ChainstateLoadStatus::FAILURE_INCOMPATIBLE_DB, _("Unsupported chainstate database format found. "
                                                                     "Please restart with -reindex-chainstate. This will "
                                                                     "rebuild the chainstate database.")};
        }

        // ReplayBlocks is a no-op if we cleared the coinsviewdb with -reindex or -reindex-chainstate
        if (!chainstate->ReplayBlocks()) {
            return {ChainstateLoadStatus::FAILURE, _("Unable to replay blocks. You will need to rebuild the database using -reindex-chainstate.")};
        }

        // The on-disk coinsdb is now in a good state, create the cache
        chainstate->InitCoinsCache(chainman.m_total_coinstip_cache * init_cache_fraction);
        assert(chainstate->CanFlushToDisk());

        if (!is_coinsview_empty(*chainstate)) {
            // LoadChainTip initializes the chain based on CoinsTip()'s best block
            if (!chainstate->LoadChainTip()) {
                return {ChainstateLoadStatus::FAILURE, _("Error initializing block database")};
            }
            assert(chainstate->m_chain.Tip() != nullptr);
        }
    }

    // Populate setBlockIndexCandidates in a separate loop, after all LoadChainTip()
    // calls have finished modifying nSequenceId. Because nSequenceId is used in the
    // set's comparator, changing it while blocks are in the set would be UB.
    for (const auto& chainstate : chainman.m_chainstates) {
        chainstate->PopulateBlockIndexCandidates();
    }

    const auto& chainstates{chainman.m_chainstates};
    if (std::any_of(chainstates.begin(), chainstates.end(),
                    [](const auto& cs) EXCLUSIVE_LOCKS_REQUIRED(cs_main) { return cs->NeedsRedownload(); })) {
        return {ChainstateLoadStatus::FAILURE, strprintf(_("Witness data for blocks after height %d requires validation. Please restart with -reindex."),
                                                         chainman.GetConsensus().SegwitHeight)};
    };

    // Now that chainstates are loaded and we're able to flush to
    // disk, rebalance the coins caches to desired levels based
    // on the condition of each chainstate.
    chainman.MaybeRebalanceCaches();

    return {ChainstateLoadStatus::SUCCESS, {}};
}

ChainstateLoadResult LoadChainstate(ChainstateManager& chainman, const CacheSizes& cache_sizes,
                                    const ChainstateLoadOptions& options)
{
    if (!chainman.AssumedValidBlock().IsNull()) {
        LogInfo("Assuming ancestors of block %s have valid signatures.", chainman.AssumedValidBlock().GetHex());
    } else {
        LogInfo("Validating signatures for all blocks.");
    }
    LogInfo("Setting nMinimumChainWork=%s", chainman.MinimumChainWork().GetHex());
    if (chainman.MinimumChainWork() < UintToArith256(chainman.GetConsensus().nMinimumChainWork)) {
        LogWarning("nMinimumChainWork set below default value of %s", chainman.GetConsensus().nMinimumChainWork.GetHex());
    }
    if (chainman.m_blockman.GetPruneTarget() == BlockManager::PRUNE_TARGET_MANUAL) {
        LogInfo("Block pruning enabled. Use RPC call pruneblockchain(height) to manually prune block and undo files.");
    } else if (chainman.m_blockman.GetPruneTarget()) {
        LogInfo("Prune configured to target %u MiB on disk for block and undo files.",
                chainman.m_blockman.GetPruneTarget() / 1024 / 1024);
    }

    LOCK(cs_main);

    chainman.m_total_coinstip_cache = cache_sizes.coins;
    chainman.m_total_coinsdb_cache = cache_sizes.coins_db;

    // Load the fully validated chainstate.
    Chainstate& validated_cs{chainman.InitializeChainstate(options.mempool)};

    // Load a chain created from a UTXO snapshot, if any exist.
    Chainstate* assumeutxo_cs{chainman.LoadAssumeutxoChainstate()};

    if (assumeutxo_cs && options.wipe_chainstate_db) {
        // Reset chainstate target to network tip instead of snapshot block.
        validated_cs.SetTargetBlock(nullptr);
        LogInfo("[snapshot] deleting snapshot chainstate due to reindexing");
        if (!chainman.DeleteChainstate(*assumeutxo_cs)) {
            return {ChainstateLoadStatus::FAILURE_FATAL, Untranslated("Couldn't remove snapshot chainstate.")};
        }
        assumeutxo_cs = nullptr;
    }

    auto [init_status, init_error] = CompleteChainstateInitialization(chainman, options);
    if (init_status != ChainstateLoadStatus::SUCCESS) {
        return {init_status, init_error};
    }

    // If a snapshot chainstate was fully validated by a background chainstate during
    // the last run, detect it here and clean up the now-unneeded background
    // chainstate.
    //
    // Why is this cleanup done here (on subsequent restart) and not just when the
    // snapshot is actually validated? Because this entails unusual
    // filesystem operations to move leveldb data directories around, and that seems
    // too risky to do in the middle of normal runtime.
    auto snapshot_completion{assumeutxo_cs
                             ? chainman.MaybeValidateSnapshot(validated_cs, *assumeutxo_cs)
                             : SnapshotCompletionResult::SKIPPED};

    if (snapshot_completion == SnapshotCompletionResult::SKIPPED) {
        // do nothing; expected case
    } else if (snapshot_completion == SnapshotCompletionResult::SUCCESS) {
        LogInfo("[snapshot] cleaning up unneeded background chainstate, then reinitializing");
        if (!chainman.ValidatedSnapshotCleanup(validated_cs, *assumeutxo_cs)) {
            return {ChainstateLoadStatus::FAILURE_FATAL, Untranslated("Background chainstate cleanup failed unexpectedly.")};
        }

        // Because ValidatedSnapshotCleanup() has torn down chainstates with
        // ChainstateManager::ResetChainstates(), reinitialize them here without
        // duplicating the blockindex work above.
        assert(chainman.m_chainstates.empty());

        chainman.InitializeChainstate(options.mempool);

        // A reload of the block index is required to recompute setBlockIndexCandidates
        // for the fully validated chainstate.
        chainman.ActiveChainstate().ClearBlockIndexCandidates();

        auto [init_status, init_error] = CompleteChainstateInitialization(chainman, options);
        if (init_status != ChainstateLoadStatus::SUCCESS) {
            return {init_status, init_error};
        }
    } else {
        return {ChainstateLoadStatus::FAILURE_FATAL, _(
           "UTXO snapshot failed to validate. "
           "Restart to resume normal initial block download, or try loading a different snapshot.")};
    }

    return {ChainstateLoadStatus::SUCCESS, {}};
}

ChainstateLoadResult VerifyLoadedChainstate(ChainstateManager& chainman, const ChainstateLoadOptions& options)
{
    auto is_coinsview_empty = [&](Chainstate& chainstate) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        return options.wipe_chainstate_db || chainstate.CoinsTip().GetBestBlock().IsNull();
    };

    LOCK(cs_main);

    // beta.2's normal network admission checked post-H block bodies, but its
    // -reindex-chainstate path did not repeat every deterministic contextual
    // rule in ConnectBlock. An existing beta database can therefore be used
    // without a full reindex only after one level-4 disconnect/reconnect pass
    // over every post-H block under the repaired ConnectBlock implementation.
    //
    // The versioned marker belongs to each coins database and records its exact
    // best-block hash. Once enabled, BatchWrite advances marker and
    // DB_BEST_BLOCK atomically. An older binary does not know that key, so
    // advancing or rebuilding the chainstate makes the marker stale and forces
    // this check again after returning to patched software. Fresh/pre-H or
    // freshly wiped chainstates are safe to mark because no old post-H state
    // remains and every future connection uses this implementation.
    const std::optional<int> legacy_final_height{
        Consensus::LegacyFinalHeight(chainman.GetConsensus())};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    std::vector<CCoinsViewDB*> schema_dbs_to_mark;

    for (auto& chainstate : chainman.m_chainstates) {
        const CBlockIndex* tip{chainstate->m_chain.Tip()};

        // Check every configured modern checkpoint against the loaded active
        // chain on every startup. This is deliberately independent of the
        // validation-schema marker: that marker can have been written by a
        // release which predates a newly hardened checkpoint, so it cannot
        // attest the identity of an already-connected ancestor.
        if (tip) {
            for (const auto& [checkpoint_height, checkpoint_hash] :
                 consensus.modern_checkpoints) {
                if (checkpoint_height < 0) {
                    return {ChainstateLoadStatus::FAILURE,
                            strprintf(_("The B3 release contains an invalid hardened modern checkpoint height %d. Startup stopped before using the chainstate; install a corrected release before continuing."),
                                      checkpoint_height)};
                }
                if (checkpoint_height > tip->nHeight) break;
                if (Consensus::GetB3Era(checkpoint_height, consensus) !=
                    Consensus::B3Era::MODERN) {
                    continue;
                }

                const CBlockIndex* active_checkpoint{
                    chainstate->m_chain[checkpoint_height]};
                if (!active_checkpoint ||
                    active_checkpoint->GetBlockHash() != checkpoint_hash) {
                    return {ChainstateLoadStatus::FAILURE,
                            strprintf(_("The active B3 chain does not match the hardened modern checkpoint at height %d. Startup stopped before using this chainstate. Restart with -reindex-chainstate to rebuild against the pinned chain; if the canonical block is not available locally, restart with -reindex to rebuild and download the block history."),
                                      checkpoint_height)};
                }
            }
        }

        const bool off_anchor_tip{
            tip && chainman.m_blockman.IsAnchorIneligible(*tip)};
        bool needs_schema_check{
            legacy_final_height &&
            !chainstate->CoinsDB().B3ValidationSchemaV1Current()};
        if (legacy_final_height && off_anchor_tip) {
            // A marker may legitimately predate the X pin (for example, a
            // height-only pause release verified the then-active competing H
            // block). Do not let undo/reconnect atomically carry that old
            // marker onto the canonical branch. Revoke it synchronously
            // before recovery; only a later canonical level-4 pass may restore
            // it.
            bool marker_cleared{false};
            try {
                marker_cleared =
                    chainstate->CoinsDB().ClearB3ValidationSchemaV1();
            } catch (const dbwrapper_error& error) {
                LogError("Unable to revoke B3 validation-schema marker before off-anchor recovery: %s\n",
                         error.what());
            }
            if (!marker_cleared) {
                return {ChainstateLoadStatus::FAILURE,
                        _("The active chain lies off the finalized B3 anchor, but its old validation marker could not be revoked safely. Startup stopped before recovery; check the disk and chainstate database permissions.")};
            }
            needs_schema_check = true;
        }
        const bool defer_schema_for_off_anchor_tip{
            needs_schema_check && off_anchor_tip};
        if (!is_coinsview_empty(*chainstate)) {
            if (tip && tip->nTime > GetTime() + MAX_FUTURE_BLOCK_TIME) {
                return {ChainstateLoadStatus::FAILURE, _("The block database contains a block which appears to be from the future. "
                                                         "This may be due to your computer's date and time being set incorrectly. "
                                                         "Only rebuild the block database if you are sure that your computer's date and time are correct")};
            }

            // A pre-pin active branch may contain legacy-codec blocks above
            // the newly pinned boundary. Levels 0-3 still prove that its block
            // and undo data can be read and disconnected. Level 4 would try
            // to reconnect history which is now deliberately ineligible and
            // prevent ActivateBestChain() from reaching the recovery path.
            // Never relax verification for an on-anchor tip.
            const int64_t ordinary_check_level{
                off_anchor_tip
                    ? std::min<int64_t>(options.check_level, 3)
                    : options.check_level};
            VerifyDBResult result = CVerifyDB(chainman.GetNotifications()).VerifyDB(
                *chainstate, chainman.GetConsensus(), chainstate->CoinsDB(),
                ordinary_check_level,
                options.check_blocks);
            switch (result) {
            case VerifyDBResult::SUCCESS:
            case VerifyDBResult::SKIPPED_MISSING_BLOCKS:
                break;
            case VerifyDBResult::INTERRUPTED:
                return {ChainstateLoadStatus::INTERRUPTED, _("Block verification was interrupted")};
            case VerifyDBResult::CORRUPTED_BLOCK_DB:
                return {ChainstateLoadStatus::FAILURE, _("Corrupted block database detected")};
            case VerifyDBResult::SKIPPED_L3_CHECKS:
                if (options.require_full_verification) {
                    return {ChainstateLoadStatus::FAILURE_INSUFFICIENT_DBCACHE, _("Insufficient dbcache for block verification")};
                }
                break;
            } // no default case, so the compiler can warn about missing cases

            if (needs_schema_check && !defer_schema_for_off_anchor_tip && tip &&
                tip->nHeight > *legacy_final_height) {
                const int post_h_blocks{tip->nHeight - *legacy_final_height};
                const CBlockIndex* first_post_h{
                    chainstate->m_chain[*legacy_final_height + 1]};
                assert(first_post_h != nullptr);
                if (!chainman.m_blockman.CheckBlockDataAvailability(
                        *tip, *first_post_h)) {
                    return {ChainstateLoadStatus::FAILURE,
                            strprintf(_("The mandatory B3 post-transition validation cannot read every block from height %d through the current tip. This node cannot safely use the existing chainstate. Restart with -reindex to restore and validate the complete block history (pruned nodes must redownload the missing data)."),
                                      *legacy_final_height + 1)};
                }
                LogInfo("Running mandatory one-time B3 post-H validation-schema check over %d blocks", post_h_blocks);
                const VerifyDBResult schema_result{
                    CVerifyDB(chainman.GetNotifications())
                        .VerifyDB(*chainstate, chainman.GetConsensus(),
                                  chainstate->CoinsDB(),
                                  /*nCheckLevel=*/4,
                                  /*nCheckDepth=*/post_h_blocks)};
                switch (schema_result) {
                case VerifyDBResult::SUCCESS:
                    break;
                case VerifyDBResult::INTERRUPTED:
                    return {ChainstateLoadStatus::INTERRUPTED,
                            _("The mandatory B3 post-transition validation was interrupted. The database was not marked as verified.")};
                case VerifyDBResult::SKIPPED_MISSING_BLOCKS:
                    return {ChainstateLoadStatus::FAILURE,
                            _("The mandatory B3 post-transition validation could not read every post-H block. This node cannot safely use the existing chainstate. Restart with -reindex to restore and validate the complete block history (pruned nodes must redownload the missing data).")};
                case VerifyDBResult::CORRUPTED_BLOCK_DB:
                    return {ChainstateLoadStatus::FAILURE,
                            _("The existing B3 post-transition chainstate failed validation under the repaired consensus rules. The database was not marked as verified. Inspect debug.log for the exact block and reject reason. Reindexing can repair local database, undo, or block-file corruption, but it cannot make a block which reproducibly violates consensus valid; in that case stop and verify the release and chain before proceeding.")};
                case VerifyDBResult::SKIPPED_L3_CHECKS:
                    return {ChainstateLoadStatus::FAILURE_INSUFFICIENT_DBCACHE,
                            _("Insufficient dbcache for the mandatory one-time B3 post-transition validation. Increase -dbcache and restart, or rebuild the chainstate with -reindex-chainstate.")};
                } // no default case, so the compiler can warn about missing cases
            }
        }
        if (defer_schema_for_off_anchor_tip) {
            // A pre-pin database may still have an old branch active when X
            // first becomes known. That branch must reach
            // ActivateBestChain(), whose first action is the existing
            // undo-backed AbandonOffAnchorTip() recovery. Reconnecting it
            // under the now-pinned boundary would reject it before that safe
            // unwind can run. Do not grant the schema marker either: after
            // recovery the canonical chain is checked and marked on the next
            // startup (or an explicit verification pass).
            LogWarning("Deferring B3 validation-schema marker for off-anchor active tip %s at height %d until the canonical branch is active\n",
                       tip->GetBlockHash().ToString(), tip->nHeight);
        } else if (needs_schema_check) {
            schema_dbs_to_mark.push_back(&chainstate->CoinsDB());
        }
    }

    // Delay all marker writes until every chainstate has passed. Each marker
    // is synchronously read back; subsequent tip changes keep it atomic with
    // that chainstate's DB_BEST_BLOCK.
    for (CCoinsViewDB* coins_db : schema_dbs_to_mark) {
        bool marker_stored{false};
        try {
            marker_stored = coins_db->MarkB3ValidationSchemaV1Current();
        } catch (const dbwrapper_error& error) {
            LogError("Unable to store B3 validation-schema marker: %s\n",
                     error.what());
        }
        if (!marker_stored) {
            return {ChainstateLoadStatus::FAILURE,
                    _("The B3 post-transition validation succeeded, but its database marker could not be stored safely. Startup was stopped; check the disk and database permissions before retrying.")};
        }
        LogInfo("B3 post-H validation schema v1 is verified for one chainstate");
    }

    return {ChainstateLoadStatus::SUCCESS, {}};
}
} // namespace node
