// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_AGREEMENT_LEADER_H
#define B3_FLOWMESH2_AGREEMENT_LEADER_H

#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/result.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

/** The leader schedule (slice R10 interface; landed with M4-AGR-03, O-96;
 * plan section 3.4.1; decision D11, PROVISIONAL until the owner rules).
 *
 * Terms of TERM_SLOTS slots: slot s is in term s / TERM_SLOTS. The active
 * seats are the seat set minus the slow set (strictly ascending seat indices
 * below the seat count). The leader of term t is
 *   active[LeaderSeed(seat_set_root, epoch, t) mod |active|]
 * (agreement/digests.h; the seed binds the authority, so a new epoch or
 * seat set reshuffles, and grinding the seed needs a new seat set, which is
 * a finalized chain fact). A view change moves to the next active seat:
 * LeaderForView(authority, slot, view, slow) is
 *   active[(base index of the term + view) mod |active|],
 * so every active seat gets its turn within |active| views and a view
 * change ends the slow leader's term for the rest of that term in practice
 * (the plan's "a term ends at its first view change": the term's base
 * leader is skipped as long as views stay above 0). This replaces FN-10's
 * round robin (a Tier 2 re-rule). The k = 1 dev certifier has no schedule:
 * its only seat leads every slot.
 *
 * Refusals: INVALID_INPUT LEADER_SLOW_SET for a slow seat at or above the
 * seat count or a slow set that is not strictly ascending; INVALID_INPUT
 * LEADER_NO_ACTIVE_SEAT when no seat is active (no seats, or all slow).
 *
 * Deterministic: no clock, randomness or I/O.
 */
namespace flowmesh2::agreement {

//! Slots per leader term. PROVISIONAL (plan 3.4.1, D11).
inline constexpr uint64_t TERM_SLOTS{8};

//! Seats a certificate marked as slow; strictly ascending seat indices.
struct SlowSet {
    std::vector<uint32_t> seats;
    friend bool operator==(const SlowSet&, const SlowSet&) = default;
};

inline constexpr std::string_view LEADER_SLOW_SET{"slow set is not strictly ascending seats of the set"}; //!< (INVALID_INPUT)
inline constexpr std::string_view LEADER_NO_ACTIVE_SEAT{"no active seat to lead"};                        //!< (INVALID_INPUT)

class LeaderSchedule
{
public:
    explicit LeaderSchedule(size_t seat_count) : m_seat_count{seat_count} {}

    size_t SeatCount() const { return m_seat_count; }
    //! The base leader of `term` under `authority`, skipping `slow` (see the file comment).
    Result<uint32_t> LeaderFor(const AuthorityVersion& authority, uint64_t term, const SlowSet& slow) const;
    //! The leader of `view` of `slot`: the term's base leader rotated `view` active seats on.
    Result<uint32_t> LeaderForView(const AuthorityVersion& authority, uint64_t slot, uint64_t view, const SlowSet& slow) const;

private:
    //! The active seats (ascending), or the refusal.
    Result<std::vector<uint32_t>> Active(const SlowSet& slow) const;

    size_t m_seat_count;
};

} // namespace flowmesh2::agreement

#endif // B3_FLOWMESH2_AGREEMENT_LEADER_H
