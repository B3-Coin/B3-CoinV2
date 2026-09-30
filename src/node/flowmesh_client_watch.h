// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_NODE_FLOWMESH_CLIENT_WATCH_H
#define BITCOIN_NODE_FLOWMESH_CLIENT_WATCH_H

#include <univalue.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace node {

/** Client side of the restricted API's bounded 'updates' watch.
 *
 * Local latency policy only, NOT evidence, admission or a retry rule. A
 * watched page is verified and applied exactly like an ordinary 'updates'
 * page, and a head change still goes through the fully verified snapshot.
 * The watch is requested only from an endpoint that advertises it, because
 * older endpoints reject watch_ms as an unknown field. */

//! Longest watch the client requests, whatever an endpoint advertises: a
//! quiet market's replies then arrive well inside a 3 s display freshness
//! gate even on slow links.
inline constexpr std::chrono::milliseconds FLOWMESH_CLIENT_UPDATES_WATCH_MAX{1500};
//! A watch never shrinks below this for lane overhead alone.
inline constexpr std::chrono::milliseconds FLOWMESH_CLIENT_UPDATES_WATCH_MIN{250};
//! The wait plus twice the measured lane overhead (request, response and
//! local work outside the endpoint's wait) stays within this.
inline constexpr std::chrono::milliseconds FLOWMESH_CLIENT_WATCH_CYCLE_BUDGET{2400};
//! Advertised maxima above this are taken as this (the largest a v1
//! endpoint serves).
inline constexpr std::chrono::milliseconds FLOWMESH_CLIENT_ADVERTISED_WATCH_MAX{2500};
//! A watched reply: one event page and one status object, never
//! reported_data. The server refuses anything larger.
inline constexpr size_t FLOWMESH_CLIENT_WATCH_MAX_REPLY{512 * 1024};

//! Capability in a market status object: updates_watch_ms_max, capped.
//! Zero (never send watch_ms) when absent or not an unsigned integer: a
//! malformed hint only disables the watch, never fails the read.
inline std::chrono::milliseconds FlowMeshAdvertisedUpdatesWatch(const UniValue& status) noexcept
{
    try {
        if (!status.isObject()) return {};
        const auto& value{status.find_value("updates_watch_ms_max")};
        if (!value.isNum()) return {};
        return std::chrono::milliseconds{std::min<uint64_t>(value.getInt<uint64_t>(), FLOWMESH_CLIENT_ADVERTISED_WATCH_MAX.count())};
    } catch (...) {
        return {};
    }
}

//! A selected market's cache and its endpoint, as seen when a caller asks to watch.
struct FlowMeshWatchEligibility {
    //! Caller's wait; zero or less never watches.
    std::chrono::milliseconds requested{0};
    //! The cache's endpoint's advertised maximum; zero when not advertised.
    std::chrono::milliseconds advertised{0};
    //! Measured lane overhead outside the endpoint's wait.
    std::chrono::milliseconds lane_overhead{0};
    //! A verified snapshot is cached and still current under local B3 authority.
    bool cache_current{false};
    //! No page applied since the last snapshot is still waiting for one.
    bool watch_ready{false};
    //! The cached cursor and reported data belong to this account scope.
    bool scope_matches{false};
    //! The endpoint is in transport cooldown, or left a watch unanswered.
    bool cooldown{false}, unanswered{false};
    //! History or curve paging: never a live page.
    bool paging{false};
};

/** The wait to request, or zero when the read takes the ordinary path. */
inline std::chrono::milliseconds FlowMeshUpdatesWatchFor(const FlowMeshWatchEligibility& e) noexcept
{
    if (e.requested.count() <= 0 || e.advertised.count() <= 0 || !e.cache_current || !e.watch_ready ||
        !e.scope_matches || e.cooldown || e.unanswered || e.paging) return {};
    const auto overhead{std::clamp(e.lane_overhead, std::chrono::milliseconds{0}, FLOWMESH_CLIENT_WATCH_CYCLE_BUDGET)};
    const auto budget{std::max(FLOWMESH_CLIENT_UPDATES_WATCH_MIN, FLOWMESH_CLIENT_WATCH_CYCLE_BUDGET - 2 * overhead)};
    return std::min({e.requested, e.advertised, FLOWMESH_CLIENT_UPDATES_WATCH_MAX, budget});
}

//! The closed set of watch labels a caller sees. The first ten are the
//! endpoint's own watch_status values; the rest are this client's.
inline constexpr std::array<std::string_view, 19> FLOWMESH_CLIENT_WATCH_LABELS{
    // Endpoint.
    "none", "no_cursor", "head_changed", "ready", "gap", "timeout", "interrupted", "restarted", "busy", "deadline",
    // Client: not sent, lane in use, reply older than the cache, capability
    // withdrawn, transport or reply failure, authority changed while it
    // slept, reply failed verification, an ordinary read taken once per head
    // for clearing ticks instead of an eligible watch (watch again at once),
    // unknown endpoint label (always last).
    "not_eligible", "lane_busy", "superseded", "unsupported", "failed", "revalidate", "invalid", "ticks_catchup", "other"};

//! An endpoint's watch_status mapped into the closed set; anything the
//! endpoint may not send is "other".
inline std::string_view FlowMeshWatchStatusLabel(const std::string_view server) noexcept
{
    const auto end{FLOWMESH_CLIENT_WATCH_LABELS.begin() + 10};
    const auto it{std::find(FLOWMESH_CLIENT_WATCH_LABELS.begin(), end, server)};
    return it != end ? *it : FLOWMESH_CLIENT_WATCH_LABELS.back();
}

//! The endpoint waited, or reported a change or a gap: a caller may watch
//! again at once. After any other label a caller paces itself (>= 500 ms).
inline bool FlowMeshWatchWaited(const std::string_view label) noexcept
{
    return label == "timeout" || label == "ready" || label == "head_changed" || label == "gap" || label == "restarted";
}

//! After these endpoint labels the lane's connection is released: the watch
//! is not going to be used again soon, and an idle kept-alive connection
//! still counts against the endpoint's connection limit.
inline bool FlowMeshWatchReleasesLane(const std::string_view label) noexcept
{
    return label == "busy" || label == "deadline" || label == "interrupted" || label == "other";
}

} // namespace node
#endif // BITCOIN_NODE_FLOWMESH_CLIENT_WATCH_H
