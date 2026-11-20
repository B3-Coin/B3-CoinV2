// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NETIO_MESH_HANDSHAKE_H
#define B3_FLOWMESH2_NETIO_MESH_HANDSHAKE_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/net/lanes.h>
#include <flowmesh2/net/tcp_mesh.h>
#include <key.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

/** The seat-mesh HELLO and the endpoint record signature (M4-NET-01,
 * stage 2; O-102). Both digests go through the HashTag registry
 * (MESH_SESSION = 31, MESH_ENDPOINT = 32; core/hashes.h), never a string tag.
 *
 * HELLO (MESH_HELLO_BYTES = 146 bytes, little-endian integers):
 *   u8 version (1) | u32 from_seat | u32 to_seat | u8 lane | u64 epoch |
 *   seat_set_root (32) | transport_key (32, x-only) | signature (64, BIP340)
 * The signature is by the transport key over
 *   SessionDigest = Tagged(MESH_SESSION, session_id (32) || u8 version ||
 *                          u32 from || u32 to || u8 lane || u64 epoch ||
 *                          seat_set_root || magic (4))
 * so it binds the channel (its session id binds both ephemeral keys), the
 * direction, the lane, the epoch and seat set the sender believes in, and the
 * network. A HELLO replayed on another connection, lane, direction or network
 * does not verify.
 *
 * Endpoint record: the seat's BLS key signs
 *   EndpointDigest = Tagged(MESH_ENDPOINT, domain (32) || SigningBytes())
 * where SigningBytes is every field but the signature (net/tcp_mesh.h).
 */
namespace flowmesh2::netio {

inline constexpr uint8_t MESH_HELLO_VERSION{1};
inline constexpr size_t MESH_HELLO_BYTES{1 + 4 + 4 + 1 + 8 + 32 + 32 + 64};

inline constexpr std::string_view HELLO_SHAPE{"HELLO of another size"};                      //!< (DECODE)
inline constexpr std::string_view HELLO_VERSION_UNKNOWN{"HELLO of another version"};         //!< (DECODE)
inline constexpr std::string_view HELLO_LANE_UNKNOWN{"HELLO names an unregistered lane"};    //!< (DECODE)
inline constexpr std::string_view HELLO_SIGNATURE{"HELLO signature does not verify"};        //!< (VERIFY_FAILED)
inline constexpr std::string_view HELLO_KEY{"HELLO transport key is not a valid x-only key"}; //!< (DECODE)

struct MeshHello {
    uint8_t version{MESH_HELLO_VERSION};
    uint32_t from_seat{0};
    uint32_t to_seat{0};
    net::Lane lane{net::Lane::BULK};
    uint64_t epoch{0};
    uint256 seat_set_root;
    std::array<unsigned char, net::MESH_TRANSPORT_KEY_BYTES> transport_key{};
    std::array<unsigned char, 64> signature{};

    std::vector<unsigned char> Encode() const;
    //! Exact: DECODE for another size, version or lane.
    static Result<MeshHello> Decode(std::span<const unsigned char> bytes);
    friend bool operator==(const MeshHello&, const MeshHello&) = default;
};

//! What the HELLO's signature covers.
uint256 SessionDigest(const std::array<unsigned char, 32>& session_id, const MeshHello& hello, const std::array<uint8_t, 4>& magic);
//! Sign `hello` (its signature field is overwritten) with the transport key; false if the key cannot sign.
bool SignHello(const CKey& transport_key, const std::array<unsigned char, 32>& session_id, const std::array<uint8_t, 4>& magic, MeshHello& hello);
//! VERIFY_FAILED unless hello.signature is the transport key's over SessionDigest (DECODE for an invalid key).
Result<void> VerifyHello(const MeshHello& hello, const std::array<unsigned char, 32>& session_id, const std::array<uint8_t, 4>& magic);

//! What a seat's BLS signature over its endpoint record covers.
uint256 EndpointDigest(const net::SeatEndpointRecord& record, const uint256& domain);
//! INVALID_INPUT TCP_MESH_ENDPOINT_SEAT/SHAPE, VERIFY_FAILED TCP_MESH_ENDPOINT_SIGNATURE (net/tcp_mesh.h details).
Result<void> VerifyEndpointRecord(const net::SeatEndpointRecord& record, const SeatSet& seats, const uint256& domain);

} // namespace flowmesh2::netio

#endif // B3_FLOWMESH2_NETIO_MESH_HANDSHAKE_H
