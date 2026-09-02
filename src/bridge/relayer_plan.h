// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_BRIDGE_RELAYER_PLAN_H
#define B3COIN_BRIDGE_RELAYER_PLAN_H

#include <bridge/exec_chain.h>
#include <bridge/proof.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <vector>

namespace bridge {

/** One backfill record plus the execution hash it creates as a new anchor. */
struct BridgeBackfillPlanV1 {
    BridgeExecutionBackfillV1 record{};
    uint256 target_block_hash{};

    friend bool operator==(const BridgeBackfillPlanV1&,
                           const BridgeBackfillPlanV1&) = default;
};

/**
 * A consensus-sized execution ancestry plan. Each backfill must confirm before
 * the following record is submitted because its target becomes that record's
 * source anchor. The final ancestry belongs in the MINT record.
 */
struct BridgeExecutionPlanV1 {
    std::vector<BridgeBackfillPlanV1> backfills{};
    uint256 mint_anchor_hash{};
    std::vector<std::vector<unsigned char>> mint_ancestry_headers{};

    friend bool operator==(const BridgeExecutionPlanV1&,
                           const BridgeExecutionPlanV1&) = default;
};

/** B3-finalized retained anchor used to start a historical relayer proof
 * without replaying ancestry from the current light-client head. */
struct BridgeRetainedExecutionSourceV1 {
    uint64_t block_number{0};
    uint256 block_hash{};
    uint256 receipts_root{};
    uint64_t source_finalized_execution_block{0};
};

inline bool VerifyRetainedExecutionSourceV1(
    const BridgeRetainedExecutionSourceV1& source,
    const uint64_t target_block_number,
    const std::vector<std::vector<unsigned char>>& headers)
{
    if (source.block_number < target_block_number ||
        source.block_hash.IsNull() || source.receipts_root.IsNull() ||
        source.source_finalized_execution_block < source.block_number ||
        source.source_finalized_execution_block - target_block_number >
            MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS ||
        headers.empty()) {
        return false;
    }
    uint256 ignored_parent;
    const auto first{exec_detail::ParseHeader(headers.front(), ignored_parent)};
    return first && first->block_number == source.block_number &&
           first->block_hash == source.block_hash &&
           first->receipts_root == source.receipts_root &&
           VerifyExecAncestry(source.block_hash, target_block_number, headers)
               .has_value();
}

namespace relayer_detail {

inline size_t FittingEnd(
    const std::vector<std::vector<unsigned char>>& headers,
    const size_t start)
{
    size_t total{0};
    size_t end{start};
    while (end < headers.size() &&
           end - start < MAX_BRIDGE_EXECUTION_ANCESTRY_HEADERS) {
        const size_t size{headers[end].size()};
        if (size == 0 || size > MAX_BRIDGE_RLP_ITEM_SIZE ||
            size > MAX_BRIDGE_EXECUTION_ANCESTRY_BYTES - total) {
            break;
        }
        total += size;
        ++end;
    }
    return end;
}

} // namespace relayer_detail

/**
 * Split one verified newest-first execution-header chain into overlapping
 * consensus-sized backfills and a final mint ancestry. Adjacent chunks repeat
 * the boundary header: that header is the new anchor created by one record and
 * the first preimage authenticated by the next record.
 *
 * Returns nullopt for a broken chain, an unreachable target, an oversized
 * individual header, a path beyond the cumulative backfill limit, or a chain
 * that cannot make progress within the frozen count/byte limits.
 */
inline std::optional<BridgeExecutionPlanV1> PlanBridgeExecutionAncestryV1(
    const uint256& finalized_anchor_hash, const uint64_t target_block_number,
    const std::vector<std::vector<unsigned char>>& headers,
    const std::set<uint256>& retained_anchor_hashes = {})
{
    if (finalized_anchor_hash.IsNull() || target_block_number == 0 ||
        headers.empty()) {
        return std::nullopt;
    }
    if (headers.size() - 1 > MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS) {
        return std::nullopt;
    }
    for (const auto& header : headers) {
        if (header.empty() || header.size() > MAX_BRIDGE_RLP_ITEM_SIZE) {
            return std::nullopt;
        }
    }
    if (!VerifyExecAncestry(finalized_anchor_hash, target_block_number,
                            headers)) {
        return std::nullopt;
    }

    BridgeExecutionPlanV1 plan;
    uint256 source_hash{finalized_anchor_hash};
    size_t start{0};
    // Start at the oldest explicitly known retained anchor on this proven
    // path. The caller must only provide anchors already on B3 or created by
    // earlier dependent records in its durable queue. Emitting a backfill
    // whose target is already retained would be a consensus-invalid no-op.
    for (size_t i{1}; i < headers.size(); ++i) {
        uint256 ignored_parent;
        const auto parsed{
            exec_detail::ParseHeader(headers[i], ignored_parent)};
        if (!parsed) return std::nullopt;
        if (retained_anchor_hashes.contains(parsed->block_hash)) {
            source_hash = parsed->block_hash;
            start = i;
        }
    }
    while (true) {
        const size_t end{relayer_detail::FittingEnd(headers, start)};
        if (end == headers.size()) {
            plan.mint_anchor_hash = source_hash;
            plan.mint_ancestry_headers.assign(headers.begin() + start,
                                               headers.end());
            if (!VerifyExecAncestry(source_hash, target_block_number,
                                    plan.mint_ancestry_headers)) {
                return std::nullopt;
            }
            return plan;
        }

        // One repeated source header cannot advance to a new anchor.
        if (end <= start + 1) return std::nullopt;
        std::vector<std::vector<unsigned char>> chunk{
            headers.begin() + start, headers.begin() + end};
        uint256 ignored_parent;
        const auto last{exec_detail::ParseHeader(chunk.back(), ignored_parent)};
        if (!last || last->block_number == 0) return std::nullopt;
        const auto verified{VerifyExecAncestry(
            source_hash, last->block_number, chunk)};
        if (!verified) return std::nullopt;

        BridgeExecutionBackfillV1 record;
        record.finalized_anchor_hash = source_hash;
        record.target_block_number = verified->block_number;
        record.ancestry_headers = std::move(chunk);
        plan.backfills.push_back(
            BridgeBackfillPlanV1{std::move(record), verified->block_hash});

        source_hash = verified->block_hash;
        start = end - 1; // repeat the newly anchored boundary header
    }
}

} // namespace bridge

#endif // B3COIN_BRIDGE_RELAYER_PLAN_H
