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
#include <string_view>
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
 * Cluster faults (M4-SIM-02, landed 2026-11-14; sim/cluster.h applies them
 * to a cluster of more than one node, the fabric's seed being fabric_seed):
 * - crash_before_slot = s, crash_seat = i: the driver restarts node i before
 *   slot s (SimCluster::RestartNode: journals and chain stay, everything
 *   else is rebuilt, M4-STO-01 recovery; a decided-but-unapplied slot is
 *   M4-STO-03's and refuses);
 * - DropRule{from, to, per_mille}: that fraction of the frames from one
 *   seat to another vanishes (seeded);
 * - DelayRule{from, to, delay_ms}: frames from one seat to another arrive
 *   that much later (rules add up);
 * - partition: groups of seats; a frame between seats of different groups
 *   vanishes until partition_heal_at_ms (never, when unset); a seat in no
 *   group is alone;
 * - SkewRule{seat, skew_ms}: that seat's agreement sees now + skew_ms.
 * CheckFaultPlan refuses an inconsistent plan (INVALID_INPUT): a seat twice
 * in the partition groups, a per_mille above 1,000, crash_before_slot 0.
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

    // Cluster faults (M4-SIM-02; see the file comment).
    std::optional<uint64_t> crash_before_slot;
    uint32_t crash_seat{0};
    std::vector<DropRule> drop;
    std::vector<DelayRule> delay;
    std::vector<std::set<uint32_t>> partition;
    std::optional<uint64_t> partition_heal_at_ms;
    std::vector<SkewRule> skew;
    uint64_t fabric_seed{1};
};

inline constexpr std::string_view FAULT_PLAN_INVALID{"fault plan: a seat in two partition groups, a per_mille above 1000 or a crash before slot 0"}; //!< (INVALID_INPUT)

/** Ok for a consistent plan (see the file comment); INVALID_INPUT
 * (FAULT_PLAN_INVALID) otherwise. Slots are not range-checked: a slot that
 * never comes simply never fires.
 */
inline Result<void> CheckFaultPlan(const FaultPlan& plan)
{
    if (plan.crash_before_slot.has_value() && *plan.crash_before_slot == 0) return Error{ErrorCode::INVALID_INPUT, FAULT_PLAN_INVALID};
    for (const DropRule& rule : plan.drop) {
        if (rule.per_mille > 1000) return Error{ErrorCode::INVALID_INPUT, FAULT_PLAN_INVALID};
    }
    std::set<uint32_t> seen;
    for (const std::set<uint32_t>& group : plan.partition) {
        for (const uint32_t seat : group) {
            if (!seen.insert(seat).second) return Error{ErrorCode::INVALID_INPUT, FAULT_PLAN_INVALID};
        }
    }
    return Ok();
}

} // namespace flowmesh2::sim

#endif // B3_FLOWMESH2_SIM_FAULTS_H
