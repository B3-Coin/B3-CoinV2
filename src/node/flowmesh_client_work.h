// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_NODE_FLOWMESH_CLIENT_WORK_H
#define BITCOIN_NODE_FLOWMESH_CLIENT_WORK_H

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace node {

enum class FlowMeshClientWorkPriority { FOREGROUND, PASSIVE };

/** Whether a FOREGROUND owner's release reserves the idle gate for its next
 * step (see FlowMeshClientWorkGate). NONE is for a caller that is itself the
 * only source of the passive work the window would hold back and runs one job
 * at a time, such as the Qt trading worker: a window after its own foreground
 * job could only delay its own next refresh. It never changes priority.
 */
enum class FlowMeshClientWorkLinger { AFTER_RELEASE, NONE };

/** Request context, not RPC-method classification. A market or saved-action
 * read may be a prerequisite of an approved trade/status request. Such calls
 * remain foreground unless their caller explicitly enters a passive scope.
 * The scope follows synchronous RPC dispatch on this thread, not new threads.
 * Each scope sets both priority and linger policy (default AFTER_RELEASE);
 * leaving it restores both.
 */
class FlowMeshClientWorkScope {
    inline static thread_local FlowMeshClientWorkPriority s_priority{FlowMeshClientWorkPriority::FOREGROUND};
    inline static thread_local FlowMeshClientWorkLinger s_linger{FlowMeshClientWorkLinger::AFTER_RELEASE};
    const FlowMeshClientWorkPriority m_previous;
    const FlowMeshClientWorkLinger m_previous_linger;

public:
    explicit FlowMeshClientWorkScope(FlowMeshClientWorkPriority priority,
                                     FlowMeshClientWorkLinger linger = FlowMeshClientWorkLinger::AFTER_RELEASE) noexcept
        : m_previous{s_priority}, m_previous_linger{s_linger}
    {
        s_priority = priority;
        s_linger = linger;
    }
    ~FlowMeshClientWorkScope()
    {
        s_priority = m_previous;
        s_linger = m_previous_linger;
    }
    FlowMeshClientWorkScope(const FlowMeshClientWorkScope&) = delete;
    FlowMeshClientWorkScope& operator=(const FlowMeshClientWorkScope&) = delete;

    static FlowMeshClientWorkPriority Current() noexcept { return s_priority; }
    static FlowMeshClientWorkLinger Linger() noexcept { return s_linger; }
    //! Whether an acquisition in this context leaves a window at its release.
    static bool LingersAfterRelease() noexcept
    {
        return s_priority == FlowMeshClientWorkPriority::FOREGROUND && s_linger == FlowMeshClientWorkLinger::AFTER_RELEASE;
    }
};

/** Exclusive, non-preemptive client work gate. This is local scheduling policy,
 * NOT a consensus timer, network admission policy, or permission to sign.
 *
 * Foreground work precedes queued passive refreshes; each class is FIFO. After
 * eight foreground acquisitions while passive work is waiting, one passive
 * caller runs. This bounds scheduling starvation, not wall time: an acquired
 * request, including its HTTPS attempts and durable writes, is never interrupted.
 *
 * A trade is several backend calls (preflight, submit, status). When a
 * foreground owner releases, the idle gate lingers for a short window: a queued
 * passive caller waits until it expires, while a foreground caller takes the
 * gate at once. Linger never delays a foreground caller or try_lock on an empty
 * queue, never follows a passive owner or a foreground owner that acquired in a
 * FlowMeshClientWorkLinger::NONE scope (recorded at acquisition), and yields to
 * the burst bound, so a passive caller waits at most eight windows beyond the
 * eight foreground holds.
 * Expiry is not notified; a passive waiter held by a window waits for its
 * deadline. Local scheduling only: no request is reordered across callers of
 * the same class, and nothing is cancelled.
 *
 * Only constant-size queue heads/counts are owned here. Each blocked caller
 * owns its waiter on its stack; no requests/callbacks/history are retained.
 * The surrounding RPC/Qt execution limits bound the number of blocked callers.
 * Queue insertion/removal/selection are O(1); notification can wake all waiting
 * threads. BasicLockable ownership rules apply, including no recursive locking.
 */
class FlowMeshClientWorkGate {
    using Clock = std::chrono::steady_clock;
    struct Waiter {
        Waiter* previous{nullptr};
        Waiter* next{nullptr};
    };
    struct Queue {
        Waiter* first{nullptr};
        Waiter* last{nullptr};
        size_t count{0};

        void Push(Waiter& waiter) noexcept
        {
            waiter.previous = last;
            if (last) last->next = &waiter;
            else first = &waiter;
            last = &waiter;
            ++count;
        }
        void Remove(Waiter& waiter) noexcept
        {
            if (waiter.previous) waiter.previous->next = waiter.next;
            else first = waiter.next;
            if (waiter.next) waiter.next->previous = waiter.previous;
            else last = waiter.previous;
            --count;
        }
    };

    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    Queue m_foreground, m_passive;
    bool m_active{false};
    size_t m_foreground_burst{0};
    const std::chrono::milliseconds m_linger;
    // Set only while idle after a foreground owner released.
    std::optional<Clock::time_point> m_linger_until;
    // Current owner, for its own diagnostics span; m_owner_lingers (recorded
    // at acquisition) also decides whether its release opens a window.
    bool m_owner_foreground{false}, m_owner_linger_window{false}, m_owner_lingers{false};
    // Diagnostics only: foreground acquisitions inside a window while passive
    // work waited, windows that expired into a waiting passive caller, and
    // passive turns forced by the burst bound over a foreground caller/window.
    uint64_t m_linger_captures{0}, m_linger_expired_with_passive_waiting{0}, m_burst_forced_passive_turns{0};

    bool LingerWindow(Clock::time_point now) const noexcept { return m_linger_until && now < *m_linger_until; }
    bool Lingering(Clock::time_point now) const noexcept
    {
        return LingerWindow(now) && m_foreground_burst < MAX_FOREGROUND_BURST;
    }
    Waiter* Next(Clock::time_point now) const noexcept
    {
        if (m_foreground.first && (!m_passive.first || m_foreground_burst < MAX_FOREGROUND_BURST)) return m_foreground.first;
        if (Lingering(now)) return nullptr;
        return m_passive.first;
    }

public:
    static constexpr size_t MAX_FOREGROUND_BURST{8};
    //! Covers the local gap between one trade step's release and the next
    //! step's request (RPC dispatch, signing, a local client process).
    static constexpr std::chrono::milliseconds FOREGROUND_LINGER{50};
    struct Observation {
        size_t foreground;
        size_t passive;
        bool active;
        //! Idle and still reserving the gate for a foreground caller.
        bool lingering;
        //! Current owner's class, and whether it acquired inside a window.
        bool owner_foreground;
        bool owner_linger_window;
        uint64_t linger_captures;
        uint64_t linger_expired_with_passive_waiting;
        uint64_t burst_forced_passive_turns;
        //! Whether the current owner's release will open a window.
        bool owner_lingers;
    };

    explicit FlowMeshClientWorkGate(std::chrono::milliseconds linger = FOREGROUND_LINGER) noexcept : m_linger{linger} {}
    FlowMeshClientWorkGate(const FlowMeshClientWorkGate&) = delete;
    FlowMeshClientWorkGate& operator=(const FlowMeshClientWorkGate&) = delete;

    void lock()
    {
        const bool foreground{FlowMeshClientWorkScope::Current() == FlowMeshClientWorkPriority::FOREGROUND};
        const bool lingers{FlowMeshClientWorkScope::LingersAfterRelease()};
        Waiter waiter;
        std::unique_lock lock{m_mutex};
        auto& queue{foreground ? m_foreground : m_passive};
        queue.Push(waiter);
        const auto queued{Clock::now()};
        auto now{queued};
        m_changed.notify_all();
        try {
            while (m_active || Next(now) != &waiter) {
                if (!foreground && !m_active && Lingering(now)) {
                    // Copy: the window may be replaced while unlocked.
                    const auto until{*m_linger_until};
                    m_changed.wait_until(lock, until);
                } else {
                    m_changed.wait(lock);
                }
                now = Clock::now();
            }
        } catch (...) {
            // A failed wait cannot leave a pointer to a destroyed stack frame.
            queue.Remove(waiter);
            m_changed.notify_all();
            throw;
        }
        queue.Remove(waiter);
        const bool window{LingerWindow(now)};
        if (foreground) {
            if (window && m_passive.first) ++m_linger_captures;
        } else if (m_foreground_burst >= MAX_FOREGROUND_BURST && (m_foreground.first || window)) {
            ++m_burst_forced_passive_turns;
        } else if (m_linger_until && queued < *m_linger_until) {
            ++m_linger_expired_with_passive_waiting;
        }
        m_active = true;
        m_owner_foreground = foreground;
        m_owner_linger_window = window;
        m_owner_lingers = lingers;
        m_linger_until.reset();
        if (foreground && m_passive.first) ++m_foreground_burst;
        else m_foreground_burst = 0;
        m_changed.notify_all();
    }

    bool try_lock()
    {
        std::lock_guard lock{m_mutex};
        // Reconnect/probe callers do not bypass a queued trade or refresh.
        if (m_active || m_foreground.first || m_passive.first) return false;
        m_active = true;
        m_owner_foreground = FlowMeshClientWorkScope::Current() == FlowMeshClientWorkPriority::FOREGROUND;
        m_owner_linger_window = LingerWindow(Clock::now());
        m_owner_lingers = FlowMeshClientWorkScope::LingersAfterRelease();
        m_linger_until.reset();
        m_foreground_burst = 0;
        return true;
    }

    void unlock()
    {
        {
            std::lock_guard lock{m_mutex};
            assert(m_active);
            m_active = false;
            if (m_owner_lingers && m_linger.count() > 0) m_linger_until = Clock::now() + m_linger;
            else m_linger_until.reset();
        }
        m_changed.notify_all();
    }

    //! Read-only diagnostics; counts exclude the current owner. Owner fields
    //! describe the current owner only while the gate is held.
    Observation Inspect() const
    {
        std::lock_guard lock{m_mutex};
        return {m_foreground.count, m_passive.count, m_active, !m_active && Lingering(Clock::now()),
                m_active && m_owner_foreground, m_active && m_owner_linger_window,
                m_linger_captures, m_linger_expired_with_passive_waiting, m_burst_forced_passive_turns,
                m_active && m_owner_lingers};
    }

    //! Read-only test observation; never services or changes queued work.
    bool WaitForQueuedForTest(size_t foreground, size_t passive, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{m_mutex};
        return m_changed.wait_for(lock, timeout, [&] {
            return m_foreground.count == foreground && m_passive.count == passive;
        });
    }

    //! Test-only: end the current linger window now, as if it had expired.
    void ExpireLingerForTest()
    {
        {
            std::lock_guard lock{m_mutex};
            if (m_linger_until) m_linger_until = Clock::now();
        }
        m_changed.notify_all();
    }
};

} // namespace node
#endif // BITCOIN_NODE_FLOWMESH_CLIENT_WORK_H
