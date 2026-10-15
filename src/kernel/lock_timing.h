// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_KERNEL_LOCK_TIMING_H
#define BITCOIN_KERNEL_LOCK_TIMING_H

#include <chrono>
#include <cstdint>

namespace kernel {

/** Diagnostic timestamps only, never ownership or consensus evidence. */
struct LockHoldTimes {
    uint64_t requested_us{0};
    uint64_t acquired_us{0};
    uint64_t releasing_us{0};
};

/** Optional, immutable diagnostic seam. No node/logger/JSON dependency.
 * enabled is called before acquisition; emit is called only by the outer
 * timing scope after the real lock guard has been destroyed. The callbacks
 * must have static lifetime and must not throw. Null callbacks disable all
 * clock reads and emission (the standalone kernel's default). */
struct LockTimingCallbacks {
    bool (*enabled)() noexcept{nullptr};
    void (*emit)(const char* stage, const LockHoldTimes& times) noexcept{nullptr};
};

inline uint64_t LockTimingNow() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** Declare before the actual lock's scope, so its callback runs after unlock,
 * also on return/exception. Only the scalar setters run while locked. */
class LockTimingSpan {
    const LockTimingCallbacks m_callbacks;
    const char* const m_stage;
    const bool m_enabled;
    LockHoldTimes m_times{};
    bool m_acquired{false};
    bool m_releasing{false};
public:
    LockTimingSpan(const LockTimingCallbacks& callbacks, const char* stage) noexcept
        : m_callbacks{callbacks}, m_stage{stage},
          m_enabled{callbacks.enabled && callbacks.emit && callbacks.enabled()}
    {
        if (m_enabled) m_times.requested_us = LockTimingNow();
    }
    ~LockTimingSpan()
    {
        if (m_enabled && m_acquired && m_releasing) m_callbacks.emit(m_stage, m_times);
    }
    LockTimingSpan(const LockTimingSpan&) = delete;
    LockTimingSpan& operator=(const LockTimingSpan&) = delete;

    void Acquired() noexcept
    {
        if (m_enabled) {
            m_times.acquired_us = LockTimingNow();
            m_acquired = true;
        }
    }
    void Releasing() noexcept
    {
        if (m_enabled) {
            m_times.releasing_us = LockTimingNow();
            m_releasing = true;
        }
    }
};

/** Declare immediately after the actual guard. Never acquires a lock. */
class LockHoldObservation {
    LockTimingSpan& m_span;
public:
    explicit LockHoldObservation(LockTimingSpan& span) noexcept : m_span{span} { m_span.Acquired(); }
    ~LockHoldObservation() { m_span.Releasing(); }
    LockHoldObservation(const LockHoldObservation&) = delete;
    LockHoldObservation& operator=(const LockHoldObservation&) = delete;
};

} // namespace kernel
#endif // BITCOIN_KERNEL_LOCK_TIMING_H
