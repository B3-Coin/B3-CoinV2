// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/sim/scheduler.h>

#include <crypto/common.h>
#include <crypto/sha256.h>

#include <array>
#include <limits>
#include <string_view>
#include <utility>

namespace flowmesh2::sim {

uint256 SeedKey(uint64_t seed)
{
    static constexpr std::string_view PREFIX{"fm2/sim/seed/"};
    std::array<unsigned char, 8> le{};
    WriteLE64(le.data(), seed);
    uint256 out;
    CSHA256{}
        .Write(reinterpret_cast<const unsigned char*>(PREFIX.data()), PREFIX.size())
        .Write(le.data(), le.size())
        .Finalize(out.data());
    return out;
}

Scheduler::Scheduler(uint64_t seed) : m_seed{seed}, m_rng{SeedKey(seed)} {}

Result<EventKey> Scheduler::At(uint64_t time_ms, Task task)
{
    if (!task) return Error{ErrorCode::INVALID_INPUT, SCHED_EMPTY_TASK};
    if (time_ms < m_clock.NowMs()) return Error{ErrorCode::INVALID_INPUT, SCHED_IN_PAST};
    // TODO(rebirth:M0-SIM-01) record/replay of this event order and a trace
    // minimizer replace the plain seeded queue (the tie is drawn here).
    const EventKey key{time_ms, m_rng.rand64(), m_next_seq++};
    m_events.emplace(key, std::move(task));
    return Ok(key);
}

Result<EventKey> Scheduler::After(uint64_t delay_ms, Task task)
{
    const uint64_t now{m_clock.NowMs()};
    if (delay_ms > std::numeric_limits<uint64_t>::max() - now) return Error{ErrorCode::INVALID_INPUT, SCHED_TIME_OVERFLOW};
    return At(now + delay_ms, std::move(task));
}

Result<uint64_t> Scheduler::Jitter(uint64_t bound)
{
    if (bound == 0) return Error{ErrorCode::INVALID_INPUT, SCHED_BAD_BOUND};
    return Ok(m_rng.rand64() % bound);
}

Result<uint64_t> Scheduler::RunUntilIdle(uint64_t max_events)
{
    uint64_t ran{0};
    while (!m_events.empty()) {
        if (ran >= max_events) return Error{ErrorCode::INVARIANT, SCHED_EVENT_LIMIT};
        auto first{m_events.begin()};
        const EventKey key{first->first};
        Task task{std::move(first->second)};
        m_events.erase(first);
        // At() refuses past times, so the clock only moves forward here.
        const Result<void> moved{m_clock.AdvanceTo(key.time_ms)};
        if (!moved.has_value()) return moved.error();
        m_current = key;
        task();
        m_current.reset();
        ++ran;
    }
    return Ok(ran);
}

} // namespace flowmesh2::sim
