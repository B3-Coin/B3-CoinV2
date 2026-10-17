// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_SIM_FAULTS_H
#define B3_FLOWMESH2_SIM_FAULTS_H

#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/result.h>

#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

/** The faults a simulated node injects (slice R14; ARCHITECTURE.md 14.2 and
 * 14.5). A FaultPlan is data, fixed when the node is created
 * (SimNode::Create runs CheckFaultPlan first), so a run is a function of
 * (scenario, seed, plan).
 *
 * Real in the outline (the walking-skeleton variants use them):
 * - withhold_from_proposals: every pool action whose (account, nonce) is in
 *   the set is withheld from every proposal (ActionPool::TakeForSlot's
 *   withhold filter; the action stays PENDING). The skeleton withholds w so
 *   that it expires (14.2 slots 4 and 5).
 * - fail_commit_barrier_at = s: the commit journal's barrier right after
 *   slot s's DECISION append (B-DECISION of s) fails with STORAGE_FATAL
 *   (N10). The node maps it to the commit journal's barrier number 2s - 1,
 *   which is exact only while every earlier slot issued exactly two commit
 *   barriers (B-DECISION, B-APPLIED); SealSlot of s checks that count and
 *   answers INVARIANT if it does not hold, so a wrong mapping is never
 *   silent.
 * - bump_authority_at = s: the fake chain's authority is bumped between
 *   slot s's Preview and Propose, so the certifier's check 1 refuses (N11).
 * - bump_authority_before_apply_at = s: the authority is bumped after slot
 *   s's DECISION is durable and before Apply, so check 3 refuses a decided
 *   slot (O-89; sim_node_tests). The walking-skeleton variants do not use it.
 *
 * Target shapes, stub M4-SIM-02 (CheckFaultPlan returns NotImplemented
 * when any of them is set): crash and restart before a slot (needs the
 * recovery tasks M4-STO-01 and M4-STO-03), dropped and delayed seat
 * messages, partitions between seat groups and per-seat clock skew (they
 * need the multi-node mesh, M4-SIM-01).
 */
namespace flowmesh2::sim {

//! Drop seat messages from `from_seat` to `to_seat` with probability per_mille / 1000 (target shape, M4-SIM-02).
struct DropRule {
    uint32_t from_seat{0};
    uint32_t to_seat{0};
    uint32_t per_mille{0};
};

//! Delay seat messages by delay_ms (target shape, M4-SIM-02).
struct DelayRule {
    uint32_t from_seat{0};
    uint32_t to_seat{0};
    uint64_t delay_ms{0};
};

//! Skew one seat's clock by skew_ms (target shape, M4-SIM-02).
struct SkewRule {
    uint32_t seat{0};
    int64_t skew_ms{0};
};

struct FaultPlan {
    // Real (see the file comment).
    std::set<std::pair<AccountId, uint64_t>> withhold_from_proposals;
    std::optional<uint64_t> fail_commit_barrier_at;
    std::optional<uint64_t> bump_authority_at;
    std::optional<uint64_t> bump_authority_before_apply_at;

    // Target shapes; any of them set is NotImplemented(M4-SIM-02).
    std::optional<uint64_t> crash_before_slot;
    std::vector<DropRule> drop;
    std::vector<DelayRule> delay;
    std::vector<std::set<uint32_t>> partition;
    std::vector<SkewRule> skew;
};

/** Ok for a plan the outline can inject; NotImplemented(M4-SIM-02) when a
 * crash, drop, delay, partition or skew fault is set. The real faults are
 * not range-checked here: a slot that never comes simply never fires.
 */
inline Result<void> CheckFaultPlan(const FaultPlan& plan)
{
    if (plan.crash_before_slot.has_value() || !plan.drop.empty() || !plan.delay.empty() || !plan.partition.empty() ||
        !plan.skew.empty()) {
        return NotImplemented("M4-SIM-02"); // TODO(rebirth:M4-SIM-02) crash, drop, delay, partition and skew hooks
    }
    return Ok();
}

} // namespace flowmesh2::sim

#endif // B3_FLOWMESH2_SIM_FAULTS_H
