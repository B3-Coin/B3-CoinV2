// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_AGREEMENT_INCLUSION_H
#define B3_FLOWMESH2_AGREEMENT_INCLUSION_H

#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/core/result.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/** Inclusion lists against censorship (slice R10: interface and stubs; plan
 * section 3.4.1, PLAN-7).
 *
 * Target (M4-AGR-04): each PREPARE vote carries at most
 * MAX_VOTE_INCLUSION_IDS short IDs (agreement/messages.h) of valid actions
 * the seat has held for more than INCLUSION_AGE_SLOTS slots without seeing
 * them included. Proposal s + 1 must include every action listed by at
 * least f + 1 PREPARE votes of slot s's certificate, unless the action is
 * now invalid; a follower refuses to prepare otherwise.
 *
 * - Build(held, slot): the IDs this seat lists for `slot`, strictly
 *   ascending, at most MAX_VOTE_INCLUSION_IDS. Stub M4-AGR-04.
 * - Check(prepare_votes, seat_count, included, now_invalid): Ok iff every ID
 *   listed by at least f + 1 of the certificate's PREPARE votes is in
 *   `included` (the next proposal's short IDs) or `now_invalid`. Stub
 *   M4-AGR-04.
 * Short IDs are salted SipHash (M4-NET-02).
 */
namespace flowmesh2::agreement {

//! Slots an action must be held before a seat lists it. PROVISIONAL (plan 3.4.1).
inline constexpr uint64_t INCLUSION_AGE_SLOTS{2};

//! An admitted action this seat holds, by short ID, since `held_since_slot`.
struct HeldAction {
    ShortId id{};
    uint64_t held_since_slot{0};
};

class InclusionList
{
public:
    InclusionList() = delete;

    //! Stub M4-AGR-04.
    static Result<std::vector<ShortId>> Build(std::span<const HeldAction> held, uint64_t slot);
    //! Stub M4-AGR-04.
    static Result<void> Check(std::span<const Vote> prepare_votes, size_t seat_count, std::span<const ShortId> included,
                              std::span<const ShortId> now_invalid);
};

} // namespace flowmesh2::agreement

#endif // B3_FLOWMESH2_AGREEMENT_INCLUSION_H
