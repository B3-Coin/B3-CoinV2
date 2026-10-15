// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_NODE_FLOWMESH_CLIENT_READ_ADMISSION_H
#define BITCOIN_NODE_FLOWMESH_CLIENT_READ_ADMISSION_H

#include <sync.h>

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <stdexcept>

namespace node {

enum class FlowMeshReadAdmission { IMMEDIATE, FOREGROUND_ACCOUNT };

/** Node-local scheduling only: one ordinary read/body owner and at most eight
 * authenticated foreground callers waiting on their own stacks. No request,
 * account, callback, network body or authority is retained by this gate.
 * The absolute admission deadline is supplied once at the original call's
 * entry. It is NOT an HTTPS, consensus, owner-preemption or shutdown deadline.
 */
class FlowMeshClientReadAdmission {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr size_t MAX_WAITERS{8};
    static constexpr std::chrono::seconds QUEUE_TIMEOUT{5};
    static constexpr const char* BUSY{"Ordinary market read already in progress; retained data is not a fresh response"};
    static constexpr const char* FULL{"Foreground market read admission queue is full; retained data is not a fresh response"};
    static constexpr const char* EXPIRED{"Foreground market read admission deadline expired; retained data is not a fresh response"};

private:
    struct Waiter {
        Waiter* previous{nullptr};
        Waiter* next{nullptr};
    };
    mutable Mutex m_mutex;
    std::condition_variable m_changed;
    bool m_active GUARDED_BY(m_mutex){false};
    Waiter* m_first GUARDED_BY(m_mutex){nullptr};
    Waiter* m_last GUARDED_BY(m_mutex){nullptr};
    size_t m_waiting GUARDED_BY(m_mutex){0};

    void Push(Waiter& waiter) EXCLUSIVE_LOCKS_REQUIRED(m_mutex)
    {
        waiter.previous = m_last;
        if (m_last) m_last->next = &waiter;
        else m_first = &waiter;
        m_last = &waiter;
        ++m_waiting;
    }
    void Remove(Waiter& waiter) EXCLUSIVE_LOCKS_REQUIRED(m_mutex)
    {
        if (waiter.previous) waiter.previous->next = waiter.next;
        else m_first = waiter.next;
        if (waiter.next) waiter.next->previous = waiter.previous;
        else m_last = waiter.previous;
        --m_waiting;
    }
    void Acquire(FlowMeshReadAdmission policy, Clock::time_point deadline) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        WAIT_LOCK(m_mutex, lock);
        if (policy == FlowMeshReadAdmission::IMMEDIATE) {
            if (m_active || m_first) throw std::runtime_error{BUSY};
            m_active = true;
            return;
        }
        if (Clock::now() >= deadline) throw std::runtime_error{EXPIRED};
        if (!m_active && !m_first) {
            m_active = true;
            return;
        }
        if (m_waiting == MAX_WAITERS) throw std::runtime_error{FULL};
        Waiter waiter;
        Push(waiter);
        try {
            for (;;) {
                // Expiry wins a simultaneous release. Never restart deadline
                // on a spurious wake or when another caller leaves the queue.
                if (Clock::now() >= deadline) throw std::runtime_error{EXPIRED};
                if (!m_active && m_first == &waiter) break;
                m_changed.wait_until(lock, deadline);
            }
        } catch (...) {
            Remove(waiter);
            m_changed.notify_all();
            throw;
        }
        Remove(waiter);
        m_active = true;
        // No throwing work follows grant: the caller's Lease already owns
        // this object and construction completes without an allocation.
    }
    void Release() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        {
            LOCK(m_mutex);
            assert(m_active);
            m_active = false;
        }
        m_changed.notify_all();
    }

public:
    FlowMeshClientReadAdmission() = default;
    FlowMeshClientReadAdmission(const FlowMeshClientReadAdmission&) = delete;
    FlowMeshClientReadAdmission& operator=(const FlowMeshClientReadAdmission&) = delete;

    class Lease {
        FlowMeshClientReadAdmission& m_gate;
    public:
        Lease(FlowMeshClientReadAdmission& gate, FlowMeshReadAdmission policy, Clock::time_point deadline)
            EXCLUSIVE_LOCKS_REQUIRED(!gate.m_mutex) : m_gate{gate}
        {
            gate.Acquire(policy, deadline);
        }
        ~Lease() EXCLUSIVE_LOCKS_REQUIRED(!m_gate.m_mutex) { m_gate.Release(); }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
    };

    struct Observation { bool active; size_t waiting; };
    //! Read-only native-test observation; owns no pointer to a stack waiter.
    Observation Inspect() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        return {m_active, m_waiting};
    }
    //! A notification alone changes no owner, position, deadline or policy.
    void NotifyForTest() { m_changed.notify_all(); }
};
} // namespace node

#endif // BITCOIN_NODE_FLOWMESH_CLIENT_READ_ADMISSION_H
