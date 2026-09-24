// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_NODE_FLOWMESH_CLIENT_POLL_H
#define BITCOIN_NODE_FLOWMESH_CLIENT_POLL_H

#include <uint256.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <utility>

namespace node {

/** Volatile scheduling of automatic reads of retained, unresolved actions.
 *
 * This is a client request-budget policy, NOT a protocol timer. Half of the
 * public API's existing 32 requests/peer/second allowance is available to
 * automatic action reads. The remainder is headroom, not a promise that
 * explicit retries, other methods or another client cannot meet the API cap.
 * A two-attempt burst permits ordinary failover; smooth replenishment avoids
 * spending the whole allowance on an initial burst of identical observations.
 * The rolling cap applies across markets and endpoints, including failures.
 *
 * No receipt, certificate or signed-action obligation is owned here. A cached
 * observation remains nonfinal; current certificate authority must always be
 * rechecked by the caller before consulting this scheduler. Explicit retries
 * bypass automatic scheduling and keep their existing bounded endpoint cycle.
 *
 * The caller serializes access, supplies actual steady-clock time, and charges
 * immediately before EACH automatic HTTP attempt. Taking a demand is not an
 * HTTP attempt. If failover exhausts the allowance, the caller must retain its
 * next endpoint instead of restarting indefinitely at the same failed ones.
 */
class FlowMeshClientPollScheduler {
public:
    using Key = std::pair<uint256, uint256>; // Exact market and ActionId.
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    static constexpr size_t MAX_KEYS{512};
    static constexpr size_t PUBLIC_PEER_REQUESTS_PER_SECOND{32};
    static constexpr size_t AUTOMATIC_ATTEMPTS_PER_SECOND{PUBLIC_PEER_REQUESTS_PER_SECOND / 2};
    static constexpr size_t BURST_CAPACITY{2};
    static constexpr auto WINDOW{std::chrono::seconds{1}};
    static constexpr auto REFILL_INTERVAL{std::chrono::microseconds{1'000'000 / AUTOMATIC_ATTEMPTS_PER_SECOND}};

    //! Coalesce duplicate demand. A serviced key returns only on fresh demand.
    bool Demand(const Key& key)
    {
        auto it{m_keys.find(key)};
        if (it == m_keys.end()) {
            if (m_keys.size() == MAX_KEYS) return false;
            it = m_keys.emplace(key, State{}).first;
        }
        if (!it->second.queued) {
            m_demand.push_back(key);
            it->second.queued = true;
        }
        return true;
    }

    //! Any caller may service this oldest demanded key, never expose its reply.
    std::optional<Key> Take(TimePoint now)
    {
        if (m_demand.empty() || !CanAttempt(now)) return std::nullopt;
        const auto key{m_demand.front()};
        m_demand.pop_front();
        m_keys.at(key).queued = false;
        return key;
    }

    //! Charge before transport starts, regardless of its eventual outcome.
    bool TryChargeAttempt(TimePoint now)
    {
        now = Advance(now);
        if (!Available()) return false;
        --m_tokens;
        m_attempts.push_back(now);
        return true;
    }

    bool CanAttempt(TimePoint now)
    {
        Advance(now);
        return Available();
    }

    //! Only call after an actual, usable remote action-status observation.
    bool MarkObserved(const Key& key)
    {
        const auto it{m_keys.find(key)};
        if (it != m_keys.end()) {
            it->second.observed = true;
            return true;
        }
        if (m_keys.size() == MAX_KEYS) return false;
        m_keys.emplace(key, State{false, true});
        return true;
    }

    bool HasObservation(const Key& key) const
    {
        const auto it{m_keys.find(key)};
        return it != m_keys.end() && it->second.observed;
    }

    //! Forget only volatile scheduling/cache knowledge, never refund attempts.
    void Remove(const Key& key)
    {
        std::erase(m_demand, key);
        m_keys.erase(key);
    }

    size_t DemandCount() const { return m_demand.size(); }
    size_t KeyCount() const { return m_keys.size(); }
    size_t AttemptsInWindow(TimePoint now)
    {
        Advance(now);
        return m_attempts.size();
    }

private:
    struct State {
        bool queued{false};
        bool observed{false};
    };
    std::map<Key, State> m_keys;
    std::deque<Key> m_demand;
    std::deque<TimePoint> m_attempts;
    size_t m_tokens{BURST_CAPACITY};
    std::optional<TimePoint> m_last_now;
    std::optional<TimePoint> m_refill_at;

    bool Available() const
    {
        return m_tokens != 0 && m_attempts.size() < AUTOMATIC_ATTEMPTS_PER_SECOND;
    }

    TimePoint Advance(TimePoint now)
    {
        // Actual steady time cannot go backwards. A faulty injected clock must
        // not refill/reset the budget or turn old observations into new ones.
        if (m_last_now && now < *m_last_now) now = *m_last_now;
        m_last_now = now;
        if (!m_refill_at) m_refill_at = now;
        const auto replenished{(now - *m_refill_at) / REFILL_INTERVAL};
        if (replenished > 0) {
            m_tokens += static_cast<size_t>(std::min<std::chrono::microseconds::rep>(
                replenished, static_cast<std::chrono::microseconds::rep>(BURST_CAPACITY - m_tokens)));
            *m_refill_at += REFILL_INTERVAL * replenished;
        }
        while (!m_attempts.empty() && now - m_attempts.front() >= WINDOW) m_attempts.pop_front();
        return now;
    }
};

} // namespace node
#endif // BITCOIN_NODE_FLOWMESH_CLIENT_POLL_H
