// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_AGREEMENT_LEADER_H
#define B3_FLOWMESH2_AGREEMENT_LEADER_H

#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/result.h>

#include <cstddef>
#include <cstdint>
#include <vector>

/** The leader schedule (slice R10: interface and stub; plan section 3.4.1;
 * decision D11, BLOCKED-ON-RULING).
 *
 * Target (M4-AGR-03): terms of TERM_SLOTS slots; the leader of term t is
 * active[H(epoch_seed || t) mod k], skipping the seats of the certified
 * slow set; a term ends at its first view change. This replaces FN-10's
 * round robin (a Tier 2 re-rule). The seed derives from the authority
 * (epoch, seat set); how exactly is M4-AGR-03's, together with M3-ENG-12's
 * grinding analysis.
 *
 * LeaderFor(authority, term, slow) -> the seat index. Stub M4-AGR-03. The
 * k = 1 dev certifier has no schedule: its only seat leads every slot.
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

class LeaderSchedule
{
public:
    explicit LeaderSchedule(size_t seat_count) : m_seat_count{seat_count} {}

    size_t SeatCount() const { return m_seat_count; }
    //! The leader of `term` under `authority`, skipping `slow`. Stub M4-AGR-03.
    Result<uint32_t> LeaderFor(const AuthorityVersion& authority, uint64_t term, const SlowSet& slow) const;

private:
    size_t m_seat_count;
};

} // namespace flowmesh2::agreement

#endif // B3_FLOWMESH2_AGREEMENT_LEADER_H
