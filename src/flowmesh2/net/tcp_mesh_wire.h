// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef B3_FLOWMESH2_NET_TCP_MESH_WIRE_H
#define B3_FLOWMESH2_NET_TCP_MESH_WIRE_H

#include <flowmesh2/net/lanes.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

/** The clear-text opening of an authenticated seat mesh socket (net/tcp_mesh.h).
 *
 * Each side first sends a HELLO of exactly HELLO_BYTES: u8 version (1),
 * u8 lane, u32 sender seat, u32 receiver seat (little endian), then the
 * sender's 64-byte EllSwift key for the BIP324 cipher. Nothing in it is
 * authenticated until the BIP340 AUTH packet that follows, so CheckHello only
 * decides whether the socket may go on to that step; a refusal closes it.
 * Pure functions, split out of the socket code so they can be fuzzed
 * (M0-FUZ-01).
 */
namespace flowmesh2::net {

inline constexpr size_t HELLO_BYTES{74};
inline constexpr unsigned char MESH_WIRE_VERSION{1};

struct MeshHello {
    Lane lane{Lane::CRITICAL};
    uint32_t from_seat{0};
    uint32_t to_seat{0};
    std::array<unsigned char, 64> ellswift{};

    friend bool operator==(const MeshHello&, const MeshHello&) = default;
};

//! What a seat expects of the HELLO it reads on one socket.
struct HelloExpectation {
    uint32_t self_seat{0};
    uint32_t seat_count{0};
    bool initiator{false}; //!< this seat dialled: the peer and lane are already fixed
    uint32_t peer{0};      //!< initiator only: the seat dialled
    Lane lane{Lane::CRITICAL}; //!< initiator only: the lane dialled
};

std::array<unsigned char, HELLO_BYTES> EncodeHello(const MeshHello& hello);

/** The HELLO in `bytes` if this seat may continue the handshake, else nullopt:
 * the version is 1, the lane is registered, the receiver is this seat, the
 * sender is another seat of the set, and either this seat dialled that
 * sender on that lane, or the sender is a lower seat (the lower seat dials).
 */
std::optional<MeshHello> CheckHello(std::span<const unsigned char, HELLO_BYTES> bytes, const HelloExpectation& expect);

} // namespace flowmesh2::net

#endif // B3_FLOWMESH2_NET_TCP_MESH_WIRE_H
