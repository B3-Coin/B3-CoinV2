// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NET_TCP_MESH_H
#define B3_FLOWMESH2_NET_TCP_MESH_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/net/seat_mesh.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/** The TCP seat mesh (slice R13: interface and stub; plan section 3.8;
 * research-network-api sections 4 and 5; D12, C-10 RE-RULE, OWNER-GATED).
 *
 * Target (M4-NET-01): a SeatMesh over persistent TCP, one connection per
 * lane per peer (CRITICAL, PROPOSAL, ACTION, BULK), on its own port
 * (default 5649, PROVISIONAL, C-10), firewalled to seats and sentries.
 * Each connection runs the BIP324 cipher (BIP324Cipher, ChaCha20-Poly1305)
 * plus one BIP340 signature by the seat key over the session ID (needs a
 * crypto review; the fallback is the BOLT 8 Noise handshake); no
 * per-frame signatures. A peer is accepted only if its key is an active or
 * next-epoch seat (or an approved sentry or observer), and only one
 * connection per (key, lane). Seat endpoint records are signed by the seat
 * key and gossiped only inside authenticated sessions. Per-peer input is
 * bounded; budgets come from protocol rates (M4-NET-04), not a fixed
 * message rate. It never touches CConnman or B3 P2P (port 5647).
 *
 * Stub: Create returns NotImplemented(M4-NET-01); it binds no socket.
 */
namespace flowmesh2::net {

//! Default seat-mesh port. PROVISIONAL (plan 3.8, C-10 RE-RULE, Tier 2).
inline constexpr uint16_t DEFAULT_SEAT_MESH_PORT{5649};

//! Target shape of a signed seat endpoint record (research 5; M4-NET-01 defines its signature and codec).
struct SeatEndpoint {
    uint32_t seat{0};
    std::vector<std::string> addresses; //!< at most 4
    uint16_t port{DEFAULT_SEAT_MESH_PORT};
    uint64_t valid_from_epoch{0};
    uint64_t counter{0}; //!< a newer counter replaces an older record
};

struct TcpMeshConfig {
    MeshConfig mesh;
    uint16_t listen_port{DEFAULT_SEAT_MESH_PORT};
    std::vector<SeatEndpoint> endpoints;
};

class TcpSeatMesh
{
public:
    TcpSeatMesh() = delete;

    //! This seat's end of the TCP mesh (see the file comment). Stub M4-NET-01.
    static Result<std::unique_ptr<SeatMesh>> Create(const TcpMeshConfig& config);
};

} // namespace flowmesh2::net

#endif // B3_FLOWMESH2_NET_TCP_MESH_H
