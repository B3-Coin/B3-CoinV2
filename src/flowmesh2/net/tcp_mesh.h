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

/** Why the mesh closed a socket or refused a connection. Diagnostic only:
 * no wire format or record carries it, and a drop never changes what an
 * authenticated peer may do. Before a HELLO, `seat` and `lane` are those of
 * the dial (initiator) or 0 and BULK (an accepted socket); after it they are
 * what the HELLO claimed, authenticated only when `authenticated` is set. */
enum class MeshDropReason : uint8_t {
    PENDING_FULL = 1,      //!< accepted while max_pending_connections sockets were already pending
    HANDSHAKE_TIMEOUT = 2, //!< not mutually authenticated within handshake_timeout_ms of its creation
    CONNECT_FAILED = 3,    //!< a dial did not connect
    PEER_CLOSED = 4,       //!< the peer closed or reset the socket, or a send or receive failed
    HELLO_REFUSED = 5,     //!< CheckHello refused the HELLO (net/tcp_mesh_wire.h)
    SIGN_FAILED = 6,       //!< our own session signature could not be made or did not verify
    AUTH_LENGTH = 7,       //!< the first packet's declared length is not that of a signature
    FRAME_OVERSIZE = 8,    //!< an authenticated packet declared a length above max_frame_bytes
    DECRYPT_FAILED = 9,    //!< a packet failed authenticated decryption or set the ignore bit
    AUTH_REFUSED = 10,     //!< the session signature does not verify under the claimed seat's pinned key
    DATA_BEFORE_AUTH = 11, //!< application data before both session signatures were exchanged
    REPLACED = 12,         //!< a freshly authenticated session of the same seat and lane took this one's place
    SOCKET_SETUP = 13,     //!< a local socket could not be made or made non-blocking
};

struct MeshDrop {
    MeshDropReason reason{MeshDropReason::PEER_CLOSED};
    bool initiator{false};
    bool hello_received{false};
    bool authenticated{false};
    uint32_t seat{0};
    Lane lane{Lane::BULK};
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
    /** Called once for every socket the mesh closes or refuses, on the
     * event-loop thread inside Poll, before the socket is closed. It must
     * not call the mesh. Optional; the drop happens either way. */
    std::function<void(const MeshDrop&)> on_drop;
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
