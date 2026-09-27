// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_NODE_FLOWMESH_CLIENT_ACTION_WAIT_H
#define BITCOIN_NODE_FLOWMESH_CLIENT_ACTION_WAIT_H

#include <univalue.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string_view>

namespace node {

/** Client side of the restricted API's bounded 'action' wait.
 *
 * Local latency policy only, NOT evidence, admission or a retry rule. A waited
 * read is an ordinary 'action' read that the endpoint may answer later; its
 * reply is verified and applied exactly like any other status reply. The wait
 * is requested only from an endpoint that advertises it, because older
 * endpoints reject wait_ms as an unknown field. */

//! Longest wait the client requests, whatever an endpoint advertises.
inline constexpr std::chrono::milliseconds FLOWMESH_CLIENT_ACTION_WAIT_MAX{2500};
//! Error an endpoint without the capability returns for a request carrying
//! wait_ms; seeing it withdraws the capability learned for that endpoint.
inline constexpr std::string_view FLOWMESH_CLIENT_UNKNOWN_FIELD_ERROR{"Unknown or duplicate client API field"};

//! Capability in a market status object: action_wait_ms_max, capped at the
//! client maximum. Zero (never send wait_ms) when absent or not an unsigned
//! integer: a malformed hint only disables the wait, never fails the read.
inline std::chrono::milliseconds FlowMeshAdvertisedActionWait(const UniValue& status) noexcept
{
    try {
        if (!status.isObject()) return {};
        const auto& value{status.find_value("action_wait_ms_max")};
        if (!value.isNum()) return {};
        return std::chrono::milliseconds{std::min<uint64_t>(value.getInt<uint64_t>(), FLOWMESH_CLIENT_ACTION_WAIT_MAX.count())};
    } catch (...) {
        return {};
    }
}

//! A retained action's state as seen when a caller asks to wait for it.
struct FlowMeshActionWaitRequest {
    //! Caller's wait; zero or less never waits.
    std::chrono::milliseconds requested{0};
    //! Explicit retries keep their own fresh-query-then-resend path.
    bool retry{false};
    bool may_have_been_sent{false};
    bool certificate_verified{false};
    bool previously_certified{false};
    bool rejected{false};
    //! An endpoint acknowledged this process's latest delivery of the action,
    //! so it holds the action's events and can grant a wait.
    bool delivery_endpoint_known{false};
    //! That endpoint's advertised maximum; zero when not advertised.
    std::chrono::milliseconds advertised{0};
};

/** The wait to request, or zero when the read takes the ordinary path. Only
 * an action this process may have delivered and that has no verified or
 * previously verified inclusion and no definite refusal qualifies: a waited
 * read can then only resolve it towards certified inclusion, and a previously
 * certified action keeps its no-resubmission recovery path. */
inline std::chrono::milliseconds FlowMeshActionWaitFor(const FlowMeshActionWaitRequest& request) noexcept
{
    if (request.retry || request.requested.count() <= 0 || !request.may_have_been_sent ||
        request.certificate_verified || request.previously_certified || request.rejected ||
        !request.delivery_endpoint_known || request.advertised.count() <= 0) return {};
    return std::min({request.requested, request.advertised, FLOWMESH_CLIENT_ACTION_WAIT_MAX});
}

} // namespace node
#endif // BITCOIN_NODE_FLOWMESH_CLIENT_ACTION_WAIT_H
