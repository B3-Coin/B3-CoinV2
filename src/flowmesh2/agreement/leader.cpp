// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/agreement/leader.h>

#include <crypto/common.h>
#include <flowmesh2/agreement/digests.h>

#include <utility>

namespace flowmesh2::agreement {

Result<std::vector<uint32_t>> LeaderSchedule::Active(const SlowSet& slow) const
{
    std::vector<uint32_t> active;
    size_t next_slow{0};
    for (uint32_t seat{0}; seat < m_seat_count; ++seat) {
        if (next_slow < slow.seats.size() && slow.seats[next_slow] == seat) {
            ++next_slow;
            // The slow set must be strictly ascending: a repeat or a smaller seat next is a refusal.
            if (next_slow < slow.seats.size() && slow.seats[next_slow] <= seat) return Error{ErrorCode::INVALID_INPUT, LEADER_SLOW_SET};
            continue;
        }
        active.push_back(seat);
    }
    if (next_slow != slow.seats.size()) return Error{ErrorCode::INVALID_INPUT, LEADER_SLOW_SET}; // a slow seat outside the set
    if (active.empty()) return Error{ErrorCode::INVALID_INPUT, LEADER_NO_ACTIVE_SEAT};
    return Ok(std::move(active));
}

Result<uint32_t> LeaderSchedule::LeaderFor(const AuthorityVersion& authority, uint64_t term, const SlowSet& slow) const
{
    const Result<std::vector<uint32_t>> active{Active(slow)};
    if (!active.has_value()) return active.error();
    const uint256 seed{LeaderSeed(authority.seat_set_root, authority.epoch, term)};
    return Ok(active->at(static_cast<size_t>(ReadLE64(seed.begin()) % active->size())));
}

Result<uint32_t> LeaderSchedule::LeaderForView(const AuthorityVersion& authority, uint64_t slot, uint64_t view, const SlowSet& slow) const
{
    const Result<std::vector<uint32_t>> active{Active(slow)};
    if (!active.has_value()) return active.error();
    const uint256 seed{LeaderSeed(authority.seat_set_root, authority.epoch, slot / TERM_SLOTS)};
    const size_t base{static_cast<size_t>(ReadLE64(seed.begin()) % active->size())};
    return Ok(active->at(static_cast<size_t>((base + view % active->size()) % active->size())));
}

} // namespace flowmesh2::agreement
