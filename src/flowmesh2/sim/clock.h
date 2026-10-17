// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_SIM_CLOCK_H
#define B3_FLOWMESH2_SIM_CLOCK_H

#include <flowmesh2/core/result.h>

#include <cstdint>
#include <string_view>

/** FakeClock: the simulator's only clock (slice R14; ARCHITECTURE.md
 * sections 5 and 14).
 *
 * Logical milliseconds from 0. It never reads a wall clock: it moves only
 * when its owner (the Scheduler, sim/scheduler.h) runs an event at a later
 * time, so every run of one seed sees the same times. Time reaches the
 * deterministic code only as a parameter (Agreement::OnTimeout(now_ms)),
 * never through this class. Not thread-safe: one simulation thread owns it.
 *
 * The times are NOT latency figures (PERF-6): a simulated millisecond is a
 * position in the event order, nothing more.
 */
namespace flowmesh2::sim {

inline constexpr std::string_view CLOCK_BACKWARDS{"the fake clock cannot move backwards"}; //!< (INVALID_INPUT)

class FakeClock
{
public:
    uint64_t NowMs() const { return m_now_ms; }

    //! Move to `t_ms`; INVALID_INPUT (CLOCK_BACKWARDS) below the current time, nothing changed.
    Result<void> AdvanceTo(uint64_t t_ms)
    {
        if (t_ms < m_now_ms) return Error{ErrorCode::INVALID_INPUT, CLOCK_BACKWARDS};
        m_now_ms = t_ms;
        return Ok();
    }

private:
    uint64_t m_now_ms{0};
};

} // namespace flowmesh2::sim

#endif // B3_FLOWMESH2_SIM_CLOCK_H
