// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_NODE_FLOWMESH_CLIENT_JOIN_H
#define BITCOIN_NODE_FLOWMESH_CLIENT_JOIN_H

#include <uint256.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>

namespace node {

/** When a foreground signing preflight may reuse a market refresh that has
 * just completed instead of issuing another 'updates' round trip. Local
 * client policy only: the reused state is the same BLS-verified cache that a
 * refresh with an unchanged head returns, and the caller still re-runs every
 * local chain/authority check. The windows bound how much older the reused
 * observation can be than a fresh one. */
struct FlowMeshJoinWindows {
    //! Maximum time since the reused refresh was validated.
    std::chrono::milliseconds completion{50};
    //! Maximum time since the reused refresh's request was started, which
    //! bounds the age of the endpoint's observation even for a slow call.
    std::chrono::milliseconds max_observation_age{1000};
};

//! One retained own action of the signing account on the joined market.
struct FlowMeshJoinOwnAction {
    enum class Kind {
        //! Refused on a first attempt; never delivered, consumes nothing.
        DEFINITE_REJECTED,
        //! Certified inclusion, now or before a restart.
        CERTIFIED,
        //! Any other state, including a restored or possibly delivered one.
        UNRESOLVED,
    };
    Kind kind{Kind::UNRESOLVED};
    //! Certified microblock, when this process verified it. Unknown for an
    //! action certified before a restart.
    std::optional<uint64_t> microblock_sequence;
};

/** Pure join predicate. A join is allowed only when
 * - the refresh completed within the completion window and its request
 *   started within the observation-age cap (a clock inversion refuses);
 * - the cached entry is at or after every microblock this process has seen
 *   certify one of its own actions on this market (own_certified_through),
 *   including actions already evicted from the retained map;
 * - no own action is unresolved; and every certified own action is known to
 *   be included at or before the cached entry.
 * Anything else falls back to the ordinary refresh, so a join never yields
 * an older own account state than that refresh would. */
inline bool FlowMeshCanJoinFreshPreflight(std::chrono::steady_clock::time_point now,
                                          std::chrono::steady_clock::time_point started,
                                          std::chrono::steady_clock::time_point validated,
                                          uint64_t cached_entry_sequence,
                                          std::optional<uint64_t> own_certified_through,
                                          std::span<const FlowMeshJoinOwnAction> own,
                                          const FlowMeshJoinWindows& windows = {})
{
    if (validated < started || now < validated) return false;
    if (now - validated > windows.completion || now - started > windows.max_observation_age) return false;
    if (own_certified_through && cached_entry_sequence < *own_certified_through) return false;
    for (const auto& action : own) {
        if (action.kind == FlowMeshJoinOwnAction::Kind::UNRESOLVED) return false;
        if (action.kind == FlowMeshJoinOwnAction::Kind::CERTIFIED &&
            (!action.microblock_sequence || *action.microblock_sequence > cached_entry_sequence)) return false;
    }
    return true;
}

/** Marks this thread's synchronous dispatch as a wallet signing preflight.
 * Only a market read made inside it may join; an explicit balance or
 * market-data read always refreshes. Like the work scope it follows
 * synchronous dispatch on this thread only, so a read that crosses a
 * process boundary simply never joins. */
class FlowMeshSigningPreflightScope {
    inline static thread_local bool s_active{false};
    const bool m_previous;

public:
    FlowMeshSigningPreflightScope() noexcept : m_previous{s_active} { s_active = true; }
    ~FlowMeshSigningPreflightScope() { s_active = m_previous; }
    FlowMeshSigningPreflightScope(const FlowMeshSigningPreflightScope&) = delete;
    FlowMeshSigningPreflightScope& operator=(const FlowMeshSigningPreflightScope&) = delete;

    static bool Active() noexcept { return s_active; }
};

/** Per-market microblock high-water of this process's own verified action
 * inclusions. Volatile, and kept apart from the retained-action map so that
 * evicting a certified action does not forget that the account advanced.
 * Bounded; a market beyond the bound makes the record incomplete, and an
 * incomplete record disables every join (fail safe to a refresh). */
class FlowMeshOwnCertifiedThrough {
    std::map<uint256, uint64_t> m_through;
    const size_t m_max_markets;
    bool m_complete{true};

public:
    explicit FlowMeshOwnCertifiedThrough(size_t max_markets) noexcept : m_max_markets{max_markets} {}

    void Record(const uint256& market, uint64_t microblock_sequence) noexcept
    {
        const auto it{m_through.find(market)};
        if (it != m_through.end()) {
            it->second = std::max(it->second, microblock_sequence);
            return;
        }
        try {
            if (m_through.size() < m_max_markets) {
                m_through.emplace(market, microblock_sequence);
                return;
            }
        } catch (...) {
        }
        m_complete = false;
    }
    bool Complete() const noexcept { return m_complete; }
    std::optional<uint64_t> Through(const uint256& market) const
    {
        const auto it{m_through.find(market)};
        return it == m_through.end() ? std::nullopt : std::optional{it->second};
    }
};

} // namespace node
#endif // BITCOIN_NODE_FLOWMESH_CLIENT_JOIN_H
