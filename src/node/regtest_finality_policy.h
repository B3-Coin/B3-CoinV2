// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_NODE_REGTEST_FINALITY_POLICY_H
#define B3COIN_NODE_REGTEST_FINALITY_POLICY_H

#include <node/finality_tracker.h>
#include <util/chaintype.h>

#include <cstdint>

namespace node {

enum class RegtestFinalityProduction {
    ALLOW,
    REQUIRE_HANDOVER,
    LINEAGE_BROKEN,
    STATE_UNAVAILABLE,
};

struct RegtestFinalityPlan {
    RegtestFinalityProduction action{RegtestFinalityProduction::ALLOW};
    uint64_t epoch{0};
    int64_t last_carrier{-1};
};

/** Local automatic-production policy, NOT consensus or signer recovery.
 * Consume the parent-synced state PROJECTED for next_height (including any
 * normal rotation). Preserve the final legal carrier, not merely the first
 * invalid height. REQUIRE_HANDOVER is permission only if the assembler
 * actually includes a consensus-judged certificate for this exact epoch.
 * Incoming blocks, manual producers and non-regtest networks are unchanged.
 */
inline RegtestFinalityPlan PlanRegtestFinalityProduction(
    const ChainType network, const FinalityTracker::State& projected,
    const Consensus::ModernPosParams& pos, const int next_height)
{
    if (network != ChainType::REGTEST) return {};
    RegtestFinalityPlan plan;
    plan.epoch = projected.epoch;
    if (projected.lineage_broken) {
        plan.action = RegtestFinalityProduction::LINEAGE_BROKEN;
        return plan;
    }
    // V1 may intentionally have no bootstrap set; do not invent authority
    // or change that existing no-finality mode as part of this policy.
    if (!projected.bootstrapped) return plan;
    if (!projected.current || !projected.next || projected.epoch_starts.empty() ||
        projected.epoch != projected.epoch_starts.size() - 1 ||
        next_height < projected.epoch_starts.back() ||
        pos.finality_epoch_blocks <= 0 || pos.max_epoch_extension < 0) {
        plan.action = RegtestFinalityProduction::STATE_UNAVAILABLE;
        return plan;
    }
    plan.last_carrier = int64_t{projected.epoch_starts.back()} +
                        int64_t{pos.finality_epoch_blocks} +
                        int64_t{pos.max_epoch_extension} - 1;
    if (!projected.handover_certified) {
        if (int64_t{next_height} > plan.last_carrier) {
            plan.action = RegtestFinalityProduction::LINEAGE_BROKEN;
        } else if (int64_t{next_height} == plan.last_carrier) {
            plan.action = RegtestFinalityProduction::REQUIRE_HANDOVER;
        }
    }
    return plan;
}

} // namespace node
#endif // B3COIN_NODE_REGTEST_FINALITY_POLICY_H
