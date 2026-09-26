// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_NODE_FLOWMESH_CLIENT_WORK_H
#define BITCOIN_NODE_FLOWMESH_CLIENT_WORK_H

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace node {

enum class FlowMeshClientWorkPriority { FOREGROUND, PASSIVE };

/** Request context, not RPC-method classification. A market or saved-action
 * read may be a prerequisite of an approved trade/status request. Such calls
 * remain foreground unless their caller explicitly enters a passive scope.
 * The scope follows synchronous RPC dispatch on this thread, not new threads.
 */
class FlowMeshClientWorkScope {
    inline static thread_local FlowMeshClientWorkPriority s_priority{FlowMeshClientWorkPriority::FOREGROUND};
    const FlowMeshClientWorkPriority m_previous;

public:
    explicit FlowMeshClientWorkScope(FlowMeshClientWorkPriority priority) noexcept : m_previous{s_priority}
    {
        s_priority = priority;
    }
    ~FlowMeshClientWorkScope() { s_priority = m_previous; }
    FlowMeshClientWorkScope(const FlowMeshClientWorkScope&) = delete;
    FlowMeshClientWorkScope& operator=(const FlowMeshClientWorkScope&) = delete;

    static FlowMeshClientWorkPriority Current() noexcept { return s_priority; }
};

/** Exclusive, non-preemptive client work gate. This is local scheduling policy,
 * NOT a consensus timer, network admission policy, or permission to sign.
 *
 * Foreground work precedes queued passive refreshes; each class is FIFO. After
 * eight foreground acquisitions while passive work is waiting, one passive
 * caller runs. This bounds scheduling starvation, not wall time: an acquired
 * request, including its HTTPS attempts and durable writes, is never interrupted.
 *
 * Only constant-size queue heads/counts are owned here. Each blocked caller
 * owns its waiter on its stack; no requests/callbacks/history are retained.
 * The surrounding RPC/Qt execution limits bound the number of blocked callers.
 * Queue insertion/removal/selection are O(1); notification can wake all waiting
 * threads. BasicLockable ownership rules apply, including no recursive locking.
 */
class FlowMeshClientWorkGate {
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

    Waiter* Next() const noexcept
    {
        if (m_foreground.first && (!m_passive.first || m_foreground_burst < MAX_FOREGROUND_BURST)) return m_foreground.first;
        return m_passive.first;
    }

public:
    static constexpr size_t MAX_FOREGROUND_BURST{8};
    struct Observation {
        size_t foreground;
        size_t passive;
        bool active;
    };

    FlowMeshClientWorkGate() = default;
    FlowMeshClientWorkGate(const FlowMeshClientWorkGate&) = delete;
    FlowMeshClientWorkGate& operator=(const FlowMeshClientWorkGate&) = delete;

    void lock()
    {
        const bool foreground{FlowMeshClientWorkScope::Current() == FlowMeshClientWorkPriority::FOREGROUND};
        Waiter waiter;
        std::unique_lock lock{m_mutex};
        auto& queue{foreground ? m_foreground : m_passive};
        queue.Push(waiter);
        m_changed.notify_all();
        try {
            m_changed.wait(lock, [&] { return !m_active && Next() == &waiter; });
        } catch (...) {
            // A failed wait cannot leave a pointer to a destroyed stack frame.
            queue.Remove(waiter);
            m_changed.notify_all();
            throw;
        }
        queue.Remove(waiter);
        m_active = true;
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
        m_foreground_burst = 0;
        return true;
    }

    void unlock()
    {
        {
            std::lock_guard lock{m_mutex};
            assert(m_active);
            m_active = false;
        }
        m_changed.notify_all();
    }

    //! Read-only diagnostics; counts exclude the current owner.
    Observation Inspect() const
    {
        std::lock_guard lock{m_mutex};
        return {m_foreground.count, m_passive.count, m_active};
    }

    //! Read-only test observation; never services or changes queued work.
    bool WaitForQueuedForTest(size_t foreground, size_t passive, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{m_mutex};
        return m_changed.wait_for(lock, timeout, [&] {
            return m_foreground.count == foreground && m_passive.count == passive;
        });
    }
};

} // namespace node
#endif // BITCOIN_NODE_FLOWMESH_CLIENT_WORK_H
