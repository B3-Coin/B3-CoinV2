// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NET_TCP_MESH_H
#define B3_FLOWMESH2_NET_TCP_MESH_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/net/seat_mesh.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace flowmesh2::net {

inline constexpr uint16_t DEFAULT_SEAT_MESH_PORT{5649};

/** A pinned bootstrap locator, authenticated by the seat's distinct BIP340
 * transport key. A runtime persists the greatest accepted counter before
 * activating changed records; this transport never accepts endpoint gossip.
 * Numeric IPv4/IPv6 addresses only. The signature binds every field, the
 * deployment, profile, roster and epoch. */
struct SeatEndpoint {
    uint32_t seat{0};
    std::vector<std::string> addresses;
    uint16_t port{DEFAULT_SEAT_MESH_PORT};
    uint64_t valid_from_epoch{0};
    uint64_t counter{0};
    std::array<unsigned char, 64> identity_signature{};
};

struct TcpMeshConfig {
    MeshConfig mesh;
    uint16_t listen_port{DEFAULT_SEAT_MESH_PORT};
    std::vector<SeatEndpoint> endpoints;
    std::string listen_address{"127.0.0.1"};
    uint256 domain;
    uint256 seat_set_hash;
    uint64_t epoch{0};
    std::array<unsigned char, 4> network_magic{};
    std::vector<std::array<unsigned char, 32>> identity_keys;
    std::vector<uint64_t> minimum_endpoint_counters;
    std::function<Result<std::array<unsigned char, 64>>(const uint256&)> sign_session;
    size_t max_pending_connections{32};
    size_t max_messages_per_lane{64};
    size_t max_queued_bytes_per_lane{64 * 1024 * 1024};
    uint32_t handshake_timeout_ms{5000};
    uint32_t reconnect_ms{100};
};

/** Four independent persistent nonblocking TCP sessions per peer. Uses the
 * repository BIP324 packet cipher (fresh EllSwift ECDH on each connection),
 * followed by mutual BIP340 signatures of the encrypted session transcript.
 * Every signature binds session ID, both seats, role, lane, deployment,
 * profile, roster and epoch. No plaintext fallback or caller supplied sender.
 * This is a separate seat protocol, not Bitcoin P2P wire negotiation.
 *
 * All methods belong to ONE event-loop thread. Poll advances bounded socket
 * work, reconnects and handshakes, then returns an authenticated frame in lane
 * weighted priority order (4 critical, 2 proposal, 1 action, 1 bulk), skipping
 * empty lanes. Each peer has an equal reserved share of each global lane
 * budget; an offline or slow peer cannot occupy another peer's share.
 * The lower seat dials; a replacement takes over only after
 * fresh mutual authentication, closing the old session atomically. Send
 * queues only bounded bytes and BUSY means nothing was admitted. A disconnect
 * can lose frames already delivered to the kernel: agreement and application
 * protocols must retransmit/acknowledge durable objects, never infer delivery
 * from Send success. Idle peers are retried without blocking other lanes.
 *
 * Qualification remains REGTEST ONLY. Create refuses other profiles. Network
 * magic is pinned by the deployment manifest and included in the transcript.
 */
class TcpSeatMesh
{
public:
    TcpSeatMesh() = delete;
    static uint256 EndpointDigest(const TcpMeshConfig& config, const SeatEndpoint& endpoint);
    static Result<std::unique_ptr<SeatMesh>> Create(const TcpMeshConfig& config);
};

} // namespace flowmesh2::net
#endif // B3_FLOWMESH2_NET_TCP_MESH_H
