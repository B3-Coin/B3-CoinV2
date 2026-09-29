// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_NODE_FLOWMESH_CLIENT_READINESS_H
#define BITCOIN_NODE_FLOWMESH_CLIENT_READINESS_H

#include <univalue.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace node {

/** Client side of an endpoint's advertised readiness (the restricted API's
 * health_api_version, endpoint_ready and endpoint_not_ready status keys).
 *
 * Display only: never evidence, never an input to verification, retry,
 * signing, delivery or endpoint selection. The client shows what the endpoint
 * last said about itself; the endpoint order and every request are unchanged. */
struct FlowMeshAdvertisedReadiness {
    enum class State : uint8_t {
        //! Absent (an older endpoint) or malformed.
        UNKNOWN = 0,
        READY,
        NOT_READY,
    };
    State state{State::UNKNOWN};
    std::vector<std::string> not_ready;
};

inline constexpr size_t FLOWMESH_CLIENT_READINESS_MAX_CODES{16};
inline constexpr size_t FLOWMESH_CLIENT_READINESS_MAX_CODE_SIZE{64};

inline const char* FlowMeshAdvertisedReadinessName(const FlowMeshAdvertisedReadiness::State state) noexcept
{
    switch (state) {
    case FlowMeshAdvertisedReadiness::State::UNKNOWN: return "unknown";
    case FlowMeshAdvertisedReadiness::State::READY: return "ready";
    case FlowMeshAdvertisedReadiness::State::NOT_READY: return "not_ready";
    }
    return "unknown";
}

//! Bounded parse of a status object's readiness keys. Absent, malformed,
//! oversized or self-contradictory hints are UNKNOWN and never fail a read.
//! A code the client does not know is kept: it still means not ready.
inline FlowMeshAdvertisedReadiness FlowMeshAdvertisedReadinessOf(const UniValue& status) noexcept
{
    try {
        if (!status.isObject()) return {};
        const auto& version{status.find_value("health_api_version")};
        const auto& ready{status.find_value("endpoint_ready")};
        const auto& codes{status.find_value("endpoint_not_ready")};
        if (!version.isNum() || version.getInt<int64_t>() < 1 || !ready.isBool() || !codes.isArray() ||
            codes.size() > FLOWMESH_CLIENT_READINESS_MAX_CODES) return {};
        FlowMeshAdvertisedReadiness out;
        for (const auto& code : codes.getValues()) {
            if (!code.isStr()) return {};
            const auto& text{code.get_str()};
            if (text.empty() || text.size() > FLOWMESH_CLIENT_READINESS_MAX_CODE_SIZE ||
                !std::all_of(text.begin(), text.end(), [](const char c) {
                    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                })) return {};
            out.not_ready.push_back(text);
        }
        if (ready.get_bool() != out.not_ready.empty()) return {};
        out.state = ready.get_bool() ? FlowMeshAdvertisedReadiness::State::READY
                                     : FlowMeshAdvertisedReadiness::State::NOT_READY;
        return out;
    } catch (...) {
        return {};
    }
}

} // namespace node
#endif // BITCOIN_NODE_FLOWMESH_CLIENT_READINESS_H
