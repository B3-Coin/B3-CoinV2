// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NET_TCP_MESH_H
#define B3_FLOWMESH2_NET_TCP_MESH_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/net/seat_mesh.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/** The TCP seat mesh (slice R13 interface; landed with M4-NET-01 on
 * 2026-11-14, stage 2 step 3; plan section 3.8; D12 PROVISIONAL, the crypto
 * review is M4-NET-05; ARCHITECTURE.md O-102). The implementation lives in
 * netio/tcp_mesh.cpp (library fm2_netio, which links bitcoin_common for the
 * ElligatorSwift ECDH, the BIP340 signatures and the socket helpers that
 * fm2_core may not link).
 *
 * A SeatMesh over persistent TCP: one connection per (peer, lane), four
 * lanes, on this seat's own port (default 5649, PROVISIONAL, C-10), firewalled
 * to seats. The seat with the lower index dials the higher; both listen.
 * Every connection runs a BIP324-style channel (netio/bip324_session.h: a
 * fresh ElligatorSwift key exchange, HKDF over the shared secret salted with
 * FlowMesh's own magic, FSChaCha20 lengths and FSChaCha20Poly1305 payloads
 * rekeyed every 224 packets; no garbage, no decoys, a header byte that must
 * be zero). The first packet each way is the HELLO (netio/mesh_handshake.h),
 * sent by the dialer first and by the acceptor only after it verified the
 * dialer's: `{version, from_seat, to_seat, lane, epoch, seat_set_root,
 * transport_key, signature}`, the signature a BIP340 one by the sender's
 * TRANSPORT key over Tagged(MESH_SESSION, session_id || every other field ||
 * magic). The transport key is bound to the seat by a SeatEndpointRecord
 * signed by the seat's BLS key over Tagged(MESH_ENDPOINT, domain || fields);
 * the mesh verifies every record against the seat set at Create and accepts
 * a HELLO only when its seat is in the set, its epoch and seat-set root are
 * this seat's, its transport key is that seat's record's and not our own,
 * its to_seat is us, and (for the dialer) its from_seat is the seat dialed.
 * A frame's `from_seat` is therefore the index of the authenticated key in
 * this seat's own seat set (O-57: proposals carry no leader signature).
 * One connection per (seat, lane): a second HELLO for an open pair wins
 * only if the open one carried nothing for a second (a duplicate deployment
 * is refused, not flapped). Per-peer input is bounded by the lane caps
 * (protocol-derived budgets are M4-NET-04): when a peer's inbox share is
 * full the mesh stops reading it. A dropped connection is redialled with a
 * backoff (reconnect_min_ms to reconnect_max_ms); the agreement's retry
 * timer resends what was in flight. Outboxes hold at most outbox_frames
 * byte-distinct frames per (seat, lane) (BUSY above that; an identical frame
 * is already queued). Poll hands frames out CRITICAL first, then PROPOSAL,
 * ACTION, BULK. It never touches CConnman or B3 P2P (port 5647).
 *
 * Threading: one network thread owns every socket; Transmit and Poll take
 * the mesh Mutex briefly and never hold it across I/O; the wake pipe is
 * written after the lock is released. Stop (or the destructor) joins the
 * thread; afterwards Transmit answers FENCED.
 */
namespace flowmesh2::net {

//! Default seat-mesh port. PROVISIONAL (plan 3.8, C-10 RE-RULE, Tier 2).
inline constexpr uint16_t DEFAULT_SEAT_MESH_PORT{5649};
inline constexpr size_t MESH_ENDPOINT_MAX_ADDRESSES{4};
inline constexpr size_t MESH_TRANSPORT_KEY_BYTES{32};
inline constexpr size_t MESH_BLS_SIGNATURE_BYTES{96};

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view TCP_MESH_SEATS_MISSING{"the TCP mesh needs its seat set"};                    //!< (INVALID_INPUT)
inline constexpr std::string_view TCP_MESH_TRANSPORT_KEY{"the transport secret gives no key"};                   //!< (INVALID_INPUT)
inline constexpr std::string_view TCP_MESH_ENDPOINT_SEAT{"an endpoint record names a seat outside the set"};    //!< (INVALID_INPUT)
inline constexpr std::string_view TCP_MESH_ENDPOINT_SHAPE{"an endpoint record has no address, too many, or a zero port"}; //!< (INVALID_INPUT)
inline constexpr std::string_view TCP_MESH_ENDPOINT_SIGNATURE{"an endpoint record's seat signature does not verify"}; //!< (VERIFY_FAILED)
inline constexpr std::string_view TCP_MESH_OWN_ENDPOINT{"this seat's endpoint record names another transport key"}; //!< (INVALID_INPUT)
inline constexpr std::string_view TCP_MESH_ENDPOINTS_MISSING{"a peer has no endpoint record"};                   //!< (INVALID_INPUT)
inline constexpr std::string_view TCP_MESH_BIND{"the mesh could not bind its listening port"};                   //!< (STORAGE_FATAL)
inline constexpr std::string_view TCP_MESH_WAKE{"the mesh could not create its wake pipe"};                      //!< (STORAGE_FATAL)
inline constexpr std::string_view TCP_MESH_CLOSED{"the mesh is stopped"};                                        //!< (FENCED)

/** A seat's endpoint record: where it listens and which transport key it
 * uses, vouched by its BLS seat key. The signature covers
 * Tagged(MESH_ENDPOINT, domain || SigningBytes()) (netio/mesh_handshake.h).
 * A newer counter replaces an older record; valid_until_epoch 0 is open.
 */
struct SeatEndpointRecord {
    uint32_t seat{0};
    std::vector<std::string> addresses; //!< numeric IPs, at most MESH_ENDPOINT_MAX_ADDRESSES
    uint16_t port{DEFAULT_SEAT_MESH_PORT};
    uint64_t valid_from_epoch{0};
    uint64_t valid_until_epoch{0};
    uint64_t counter{0};
    std::array<unsigned char, MESH_TRANSPORT_KEY_BYTES> transport_key{}; //!< x-only secp256k1 public key
    std::array<unsigned char, MESH_BLS_SIGNATURE_BYTES> signature{};     //!< BLS, by the seat key

    //! Every field but the signature: u32 seat | u16 port | u64 valid_from | u64 valid_until | u64 counter | key | u8 n | n x (u8 len, bytes).
    std::vector<unsigned char> SigningBytes() const;
    friend bool operator==(const SeatEndpointRecord&, const SeatEndpointRecord&) = default;
};

struct TcpMeshLimits {
    uint64_t handshake_timeout_ms{5'000};
    uint64_t reconnect_min_ms{100};
    uint64_t reconnect_max_ms{2'000};
    uint64_t poll_interval_ms{50};        //!< the network thread's longest sleep
    uint64_t duplicate_grace_ms{1'000};   //!< an open connection that carried a frame this recently keeps its (seat, lane)
    size_t outbox_frames{64};             //!< distinct frames queued per (seat, lane) before BUSY
    size_t inbox_frames_per_peer{256};    //!< frames waiting in Poll per peer before the mesh stops reading it
    size_t max_unauthenticated{64};       //!< accepted connections that have not completed a HELLO
    size_t read_bytes_per_step{256 * 1024}; //!< bytes read from one connection per network-thread step before the others are served
    size_t write_buffer_bytes{1024 * 1024}; //!< bytes queued to one socket before its outbox holds frames back (the outbox cap then says BUSY)
    uint64_t send_stall_ms{10'000};       //!< a connection whose queued bytes make no progress for this long is closed
};

struct TcpMeshConfig {
    MeshConfig mesh;
    std::string listen_address{"127.0.0.1"};
    uint16_t listen_port{DEFAULT_SEAT_MESH_PORT}; //!< 0: an ephemeral port (ListenPort tells)
    std::shared_ptr<const SeatSet> seats;         //!< the current epoch's seat set (required)
    uint256 domain;                               //!< the endpoint records' signing domain (DomainHash of the network)
    std::array<uint8_t, 4> magic{};               //!< FlowMesh's own mesh magic, never B3 P2P's
    uint64_t epoch{0};
    std::array<unsigned char, MESH_TRANSPORT_KEY_BYTES> transport_secret{}; //!< this seat's transport key (secp256k1 secret)
    std::vector<SeatEndpointRecord> endpoints;    //!< every seat's record (this seat's too); verified at Create
    TcpMeshLimits limits;
};

//! The index of `lane` in TcpPeerStatus::open (lanes are numbered from 1).
inline size_t LaneIndex(Lane lane) { return static_cast<size_t>(lane) - 1; }

struct TcpPeerStatus {
    uint32_t seat{0};
    std::array<bool, LANE_COUNT> open{}; //!< by lane index (LANES_BY_PRIORITY order is not implied; index = IndexOf(lane))
    uint64_t frames_in{0};
    uint64_t frames_out{0};
    uint64_t refused{0}; //!< handshakes or packets this seat refused from that peer
    uint64_t pending_bytes{0}; //!< bytes queued to the peer's sockets and not yet sent (sampled once per step)
    uint64_t inbox_frames{0};  //!< frames from the peer waiting in Poll (sampled once per step)
    uint64_t stalls{0};        //!< connections to the peer closed because queued bytes made no progress for send_stall_ms
};

struct TcpMeshStatus {
    uint16_t listen_port{0};
    uint64_t steps{0};     //!< network-thread iterations so far (a spinning thread shows here)
    size_t connections{0}; //!< every connection, open or not
    std::vector<TcpPeerStatus> peers; //!< one per other seat, by seat
};

class TcpSeatMesh final : public SeatMesh
{
public:
    //! This seat's end of the TCP mesh (see the file comment); binds the port and starts the network thread.
    static Result<std::unique_ptr<TcpSeatMesh>> Create(const TcpMeshConfig& config);
    ~TcpSeatMesh() override;

    Result<std::optional<InboundFrame>> Poll() override;
    //! The bound port (the config's, or the ephemeral one chosen for port 0).
    uint16_t ListenPort() const;
    TcpMeshStatus Status() const;
    //! Close every connection now (they are redialled): a test seam and an operator's reset.
    void DropConnections();
    //! Join the network thread and close every socket; Transmit answers FENCED afterwards.
    void Stop();

protected:
    Result<void> Transmit(uint32_t seat, Lane lane, std::span<const unsigned char> frame) override;

private:
    struct Impl;
    TcpSeatMesh(const MeshConfig& config, std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace flowmesh2::net

#endif // B3_FLOWMESH2_NET_TCP_MESH_H
