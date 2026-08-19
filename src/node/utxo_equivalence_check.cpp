// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/utxo_equivalence_check.h>

#include <coins.h>
#include <legacy/replay.h>
#include <node/fn_pod.h>
#include <undo.h>
#include <tinyformat.h>

#include <algorithm>
#include <utility>

namespace node {

ReplayEquivalenceResult SummarizeComparison(const UtxoComparison& cmp, const size_t max_sample)
{
    ReplayEquivalenceResult res;
    res.live_count = cmp.count_a;
    res.replay_count = cmp.count_b;
    res.live_commitment = cmp.commitment_a;
    res.replay_commitment = cmp.commitment_b;
    res.mismatch_total = cmp.mismatches.size();
    const size_t keep{std::min(max_sample, cmp.mismatches.size())};
    res.mismatch_sample.assign(cmp.mismatches.begin(), cmp.mismatches.begin() + keep);
    res.ok = cmp.Equal();
    return res;
}

ReplayEquivalenceResult VerifyReplayEquivalence(
    const Consensus::Params& params,
    const CCoinsView& live_view,
    const std::function<std::optional<CBlock>(int)>& block_source,
    CCoinsViewDB& replay_scratch,
    const ReplayEquivalenceOptions& opts)
{
    ReplayEquivalenceResult res;

    if (opts.final_height < 0 || opts.final_hash.IsNull()) {
        res.errors.push_back("a final height H and block hash X must be provided");
        return res;
    }

    // Both sides must refer to the same (H, X): the live chainstate's best
    // block is the block it was validated to, and the replay below is anchored
    // to the same hash by its configured checkpoint.
    const uint256 live_best{live_view.GetBestBlock()};
    if (live_best != opts.final_hash) {
        res.errors.push_back(strprintf(
            "live chainstate best block %s is not the expected block %s at height %d; "
            "stop the node exactly at H (e.g. -stopatheight=%d) and retry",
            live_best.ToString(), opts.final_hash.ToString(), opts.final_height, opts.final_height));
        return res;
    }

    // Reconstruct genesis..H through the trusted replay engine. Linkage,
    // Merkle roots and the checkpoint at H (which pins X) are verified by the
    // engine itself, so a wrong or tampered block source fails here instead of
    // yielding a misleading comparison.
    // TRANSACTIONAL PoD publication (hardening ruling 2026-08-17):
    // records accumulate PRIVATELY during the replay and are published
    // into the result only after the complete H/X-anchored replay
    // succeeds. Any failure — early or late — returns no records and no
    // report, never a partial set.
    std::vector<PodRecord> pod_records;
    {
        CCoinsViewCache cache{&replay_scratch};
        legacy::TrustedReplay replay{params, opts.final_height,
                                     {{opts.final_height, opts.final_hash}}};
        std::string error;
        for (int height{0}; height <= opts.final_height; ++height) {
            const std::optional<CBlock> block{block_source(height)};
            if (!block) {
                res.errors.push_back(strprintf("no block available at height %d", height));
                return res;
            }
            CBlockUndo undo;
            if (!replay.ApplyBlock(*block, cache, error,
                                   opts.derive_pod_report ? &undo : nullptr)) {
                res.errors.push_back(strprintf("trusted replay failed at height %d: %s", height, error));
                return res;
            }
            if (opts.derive_pod_report) {
                std::vector<PodRecord> records;
                if (!DerivePodRecords(*block, undo, height, params, records, error)) {
                    res.errors.push_back(
                        strprintf("PoD derivation failed at height %d: %s", height, error));
                    return res;
                }
                for (auto& record : records) {
                    pod_records.push_back(std::move(record));
                }
            }
            ++res.blocks_replayed;
            if (height % 1000 == 0) {
                cache.SetBestBlock(replay.TipHash());
                cache.Flush();
            }
        }
        if (replay.TipHash() != opts.final_hash) {
            // Unreachable while the checkpoint above is configured; kept as a
            // belt-and-braces invariant on the engine.
            res.errors.push_back(strprintf("replay tip %s is not the expected block %s",
                                           replay.TipHash().ToString(), opts.final_hash.ToString()));
            return res;
        }
        cache.SetBestBlock(replay.TipHash());
        cache.Flush();
    }

    const int blocks_replayed{res.blocks_replayed};
    res = SummarizeComparison(CompareUtxoViews(live_view, replay_scratch), opts.max_mismatch_sample);
    res.blocks_replayed = blocks_replayed;
    if (opts.derive_pod_report && res.ok) {
        // Publication requires BOTH the complete H/X-anchored replay
        // AND U_port == U_replay: an activation-gate report is never
        // presented as authoritative when equivalence failed.
        res.pod_report = BuildPodCapacityReport(pod_records);
        res.pod_records = std::move(pod_records);
    }
    return res;
}

} // namespace node
