// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef B3COIN_NODE_FLOWMESH_DELIVERY_H
#define B3COIN_NODE_FLOWMESH_DELIVERY_H

#include <flowmesh/p2p.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace node {

/** Local admission/completion only. Never serialized into an action, vote,
 * microblock, certificate or B3 settlement object. */
enum class FlowMeshDeliveryAdmission : uint8_t {
    ADMITTED,
    FULL,
    DISCONNECTED,
    STOPPED,
    RECONCILING,
    INVALID,
    LEGACY_UNTRACKED,
};

inline const char* FlowMeshDeliveryAdmissionName(FlowMeshDeliveryAdmission admission)
{
    switch (admission) {
    case FlowMeshDeliveryAdmission::ADMITTED: return "admitted";
    case FlowMeshDeliveryAdmission::FULL: return "queue_full";
    case FlowMeshDeliveryAdmission::DISCONNECTED: return "disconnected";
    case FlowMeshDeliveryAdmission::STOPPED: return "stopped";
    case FlowMeshDeliveryAdmission::RECONCILING: return "b3_reconciling";
    case FlowMeshDeliveryAdmission::INVALID: return "invalid";
    case FlowMeshDeliveryAdmission::LEGACY_UNTRACKED: return "legacy_queued_without_socket_completion";
    }
    return "unknown";
}

struct FlowMeshPeerAdmission {
    flowmesh::WirePeerId peer{0};
    FlowMeshDeliveryAdmission admission{FlowMeshDeliveryAdmission::DISCONNECTED};
    std::string reason;
};

struct FlowMeshRelayResult {
    std::vector<FlowMeshPeerAdmission> peers;
    FlowMeshDeliveryAdmission no_peer_reason{FlowMeshDeliveryAdmission::DISCONNECTED};
    std::string reason;
};

/** SOCKET_WRITTEN is a local completed socket write, NOT peer receipt,
 * signature verification, certification or durable application. */
enum class FlowMeshDeliveryOutcome : uint8_t {
    SOCKET_WRITTEN,
    DISCONNECTED,
    STOPPED,
    CANCELLED,
};

inline const char* FlowMeshDeliveryOutcomeName(FlowMeshDeliveryOutcome outcome)
{
    switch (outcome) {
    case FlowMeshDeliveryOutcome::SOCKET_WRITTEN: return "socket_written";
    case FlowMeshDeliveryOutcome::DISCONNECTED: return "disconnected";
    case FlowMeshDeliveryOutcome::STOPPED: return "stopped";
    case FlowMeshDeliveryOutcome::CANCELLED: return "cancelled";
    }
    return "unknown";
}

struct FlowMeshDeliveryEvent {
    uint64_t delivery_id{0};
    flowmesh::WirePeerId peer{0};
    FlowMeshDeliveryOutcome outcome{FlowMeshDeliveryOutcome::DISCONNECTED};
    std::string reason;
};

/** False means the consumer's bounded notification queue refused the event.
 * The sender must count this; pending runtime work expires/retries explicitly. */
using FlowMeshDeliveryCallback = std::function<bool(const FlowMeshDeliveryEvent&)>;

} // namespace node
#endif // B3COIN_NODE_FLOWMESH_DELIVERY_H
