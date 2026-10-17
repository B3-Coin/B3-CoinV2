// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_SIM_SCHEDULER_H
#define B3_FLOWMESH2_SIM_SCHEDULER_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/sim/clock.h>
#include <random.h>
#include <uint256.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string_view>

/** The deterministic event scheduler of the simulator (slice R14;
 * ARCHITECTURE.md section 14.1, "Determinism protocol").
 *
 * One Scheduler drives one simulation on one thread. It owns the FakeClock
 * and the only randomness of the run: FastRandomContext(SeedKey(seed)), a
 * ChaCha20 keystream keyed by SeedKey(seed) = SHA256("fm2/sim/seed/" ||
 * u64 little-endian seed). The same seed therefore gives the same event
 * order and the same jitter, on every host; another seed permutes them.
 *
 * Events. At(t, task) queues `task` to run at fake time t (t >= Now(),
 * otherwise INVALID_INPUT SCHED_IN_PAST); After(d, task) is At(Now() + d)
 * (INVALID_INPUT SCHED_TIME_OVERFLOW if that overflows). Each queued event
 * gets the key (time, tie, seq): tie is the next rand64() of the seeded
 * stream, drawn at queueing, and seq counts queued events from 0. Events
 * run in ascending key order, so events of one time run in seeded order
 * (the seed permutes them), and seq makes the order total even if two ties
 * collide. A task may queue further events (at or after the current time).
 *
 * RunUntilIdle(max) runs events until none is left and returns how many
 * ran; it moves the clock to each event's time before running it. It stops
 * with INVARIANT (SCHED_EVENT_LIMIT) after `max` events with events still
 * queued, so a run that never settles fails instead of spinning.
 *
 * Jitter(bound) draws rand64() % bound from the same stream (INVALID_INPUT
 * SCHED_BAD_BOUND for 0). The modulo bias is irrelevant here: the value
 * only perturbs the event order. Ties and jitter share one stream, so the
 * order of draws is part of the schedule.
 *
 * REAL-REPLACE M0-SIM-01: record/replay of the event order and a trace
 * minimizer replace this plain seeded queue.
 */
namespace flowmesh2::sim {

inline constexpr std::string_view SCHED_IN_PAST{"event time before the fake clock"};        //!< (INVALID_INPUT)
inline constexpr std::string_view SCHED_TIME_OVERFLOW{"event time overflows"};             //!< (INVALID_INPUT)
inline constexpr std::string_view SCHED_BAD_BOUND{"jitter bound is zero"};                 //!< (INVALID_INPUT)
inline constexpr std::string_view SCHED_EVENT_LIMIT{"RunUntilIdle hit its event limit"};   //!< (INVARIANT)
inline constexpr std::string_view SCHED_EMPTY_TASK{"an event without a task"};             //!< (INVALID_INPUT)

//! Events RunUntilIdle runs at most by default.
inline constexpr uint64_t DEFAULT_MAX_EVENTS{1'000'000};

//! SHA256("fm2/sim/seed/" || u64 little-endian seed): the scheduler's ChaCha20 key.
uint256 SeedKey(uint64_t seed);

//! The order key of one queued event.
struct EventKey {
    uint64_t time_ms{0};
    uint64_t tie{0}; //!< seeded draw at queueing
    uint64_t seq{0}; //!< queueing order, from 0

    friend bool operator==(const EventKey&, const EventKey&) = default;
    friend auto operator<=>(const EventKey&, const EventKey&) = default;
};

class Scheduler
{
public:
    using Task = std::function<void()>;

    explicit Scheduler(uint64_t seed);
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    uint64_t Seed() const { return m_seed; }
    uint64_t NowMs() const { return m_clock.NowMs(); }
    const FakeClock& Clock() const { return m_clock; }

    //! Queue `task` at fake time `time_ms`; see the file comment.
    Result<EventKey> At(uint64_t time_ms, Task task);
    //! Queue `task` `delay_ms` after Now().
    Result<EventKey> After(uint64_t delay_ms, Task task);
    //! A seeded value in [0, bound).
    Result<uint64_t> Jitter(uint64_t bound);

    //! Events queued and not run yet.
    size_t Pending() const { return m_events.size(); }
    //! The key of the event that is running, if any.
    const std::optional<EventKey>& Current() const { return m_current; }

    //! Run events until none is left; the number run. See the file comment.
    Result<uint64_t> RunUntilIdle(uint64_t max_events = DEFAULT_MAX_EVENTS);

private:
    const uint64_t m_seed;
    FastRandomContext m_rng;
    FakeClock m_clock;
    uint64_t m_next_seq{0};
    std::map<EventKey, Task> m_events;
    std::optional<EventKey> m_current;
};

} // namespace flowmesh2::sim

#endif // B3_FLOWMESH2_SIM_SCHEDULER_H
