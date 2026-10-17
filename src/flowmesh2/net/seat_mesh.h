// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NET_SEAT_MESH_H
#define B3_FLOWMESH2_NET_SEAT_MESH_H

#include <flowmesh2/agreement/agreement.h>
#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/agreement/outbound.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/net/lanes.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/** The seat mesh: authenticated seat-to-seat transport (slice R13:
 * interface; plan sections 3.2 and 3.8; research-network-api section 4;
 * [CC] boundaries 3 and 4; SEC-3, PLAN-10).
 *
 * One SeatMesh object is one seat's end of the mesh: `self_seat` of
 * `seat_count` seats (indices into the epoch's SeatSet). Every frame goes
 * to one seat on one Lane (net/lanes.h); the transport keeps one
 * connection per lane per peer (plan 3.8), so a full proposal lane never
 * delays a vote. The base class holds the rules every transport must
 * keep, in non-virtual methods; a transport implements only Transmit and
 * Poll.
 *
 *  - SendVote(seat, vote, header_hash): THE ONLY WAY A VOTE LEAVES. An
 *    agreement::OutboundVote exists only for a DurableSignedVote (its
 *    SIGNED_VOTE record passed B-VOTE, agreement/outbound.h), so a vote is
 *    never sent before it is durable (ARCHITECTURE.md section 7.3). It is
 *    encoded as an agreement VOTE message (phase, epoch, view and slot
 *    from the VoteKey, `header_hash`, seat = self_seat, the signature; no
 *    inclusion IDs until M4-AGR-04), passes CheckVote, and goes on
 *    CRITICAL. The vote's signature covers its digest (the CommitDigest
 *    for COMMIT), which binds the header; `header_hash` only names it
 *    (O-57, O-76).
 *  - Send(seat, lane, bytes): every other frame. A frame on CRITICAL must
 *    be an agreement message of type VIEW_CHANGE, NEW_VIEW or EVIDENCE, a
 *    frame on PROPOSAL one of type PROPOSAL (the envelope's version and
 *    type bytes are checked; the receiver decodes the rest); a VOTE on
 *    either lane is refused (MESH_VOTE_NOT_TYPED), so no code path can
 *    send a vote without the durable token. ACTION and BULK frames are
 *    opaque here (client actions, catch-up): a receiver interprets a
 *    frame only as its lane's content, so a vote-shaped ACTION frame is an
 *    ActionV2 frame to it, never a vote.
 *  - Send(seat, message), the agreement::MessageSink port (final): routes
 *    an encoded agreement message by its type (PROPOSAL to PROPOSAL;
 *    VIEW_CHANGE, NEW_VIEW, EVIDENCE to CRITICAL); a VOTE is refused.
 *  - Poll(): the next received frame, or nullopt. The transport
 *    authenticates the sender (M4-NET-01: BIP324 cipher plus one BIP340
 *    session signature, or the simulator's in-process queues M4-SIM-01).
 *
 * Refusals, in this order and with nothing transmitted: INVALID_INPUT
 * MESH_LANE_UNKNOWN, MESH_SEAT_RANGE, MESH_SELF; for SendVote CheckVote's
 * refusal (VOTE_GENESIS); OVERSIZE MESH_FRAME_OVERSIZE (above
 * max_frame_bytes); INVALID_INPUT MESH_VOTE_NOT_TYPED, MESH_LANE_CONTENT
 * (the MessageSink port refuses a VOTE, or an envelope of no agreement
 * type, before it picks a lane); then the transport's answer:
 * BUSY means that lane to that seat is full and nothing was queued (the
 * agreement resends on its timer; a resent durable vote is the same bytes).
 *
 * Implementations: none in the outline. InProcessSeatMesh (M4-SIM-01) and
 * TcpSeatMesh (M4-NET-01) are stubs; the tests use a recording transport.
 */
namespace flowmesh2::net {

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view MESH_CONFIG_INVALID{"mesh without seats, self outside them, or a zero frame cap"}; //!< (INVALID_INPUT)
inline constexpr std::string_view MESH_LANE_UNKNOWN{"unregistered lane"};                                          //!< (INVALID_INPUT)
inline constexpr std::string_view MESH_SEAT_RANGE{"seat index outside the mesh"};                                 //!< (INVALID_INPUT)
inline constexpr std::string_view MESH_SELF{"a seat does not send to itself"};                                    //!< (INVALID_INPUT)
inline constexpr std::string_view MESH_FRAME_OVERSIZE{"frame above the mesh's max_frame_bytes"};                  //!< (OVERSIZE)
inline constexpr std::string_view MESH_VOTE_NOT_TYPED{"a vote leaves only through SendVote"};                      //!< (INVALID_INPUT)
inline constexpr std::string_view MESH_LANE_CONTENT{"the frame is not a message of its lane"};                    //!< (INVALID_INPUT)

struct MeshConfig {
    uint32_t self_seat{0};
    uint32_t seat_count{0};
    size_t max_frame_bytes{0}; //!< largest frame on any lane
    ProfileParams profile;     //!< the vote codec's caps
};

//! A frame one seat received from another.
struct InboundFrame {
    uint32_t from_seat{0};
    Lane lane{Lane::BULK};
    std::vector<unsigned char> bytes;
};

class SeatMesh : public agreement::MessageSink
{
public:
    ~SeatMesh() override = default;
    SeatMesh(const SeatMesh&) = delete;
    SeatMesh& operator=(const SeatMesh&) = delete;

    //! Validate(profile) (its errors), then INVALID_INPUT MESH_CONFIG_INVALID for no seats, self_seat >= seat_count or a zero frame cap.
    static Result<void> CheckConfig(const MeshConfig& config);

    const MeshConfig& Config() const { return m_config; }

    //! Send a durable vote to `seat` on CRITICAL (see the file comment).
    Result<void> SendVote(uint32_t seat, const agreement::OutboundVote& vote, const uint256& header_hash);
    //! Send a non-vote frame to `seat` on `lane` (see the file comment).
    Result<void> Send(uint32_t seat, Lane lane, std::span<const unsigned char> bytes);
    //! agreement::MessageSink: an encoded agreement message, routed by its type; a VOTE is refused.
    Result<void> Send(uint32_t seat, std::span<const unsigned char> message) final;

    //! The next frame received from another seat; nullopt when none is waiting.
    virtual Result<std::optional<InboundFrame>> Poll() = 0;

protected:
    //! `config` must pass CheckConfig (the transport's Create checks it first).
    explicit SeatMesh(const MeshConfig& config) : m_config{config} {}

    /** Queue `frame` to `seat` on `lane`, already checked by the base class.
     * Ok: queued; BUSY: that lane to that seat is full and nothing was
     * queued; FENCED: the transport is closed.
     */
    virtual Result<void> Transmit(uint32_t seat, Lane lane, std::span<const unsigned char> frame) = 0;

private:
    //! The route and size checks shared by every send.
    Result<void> CheckRoute(uint32_t seat, Lane lane, size_t bytes) const;

    const MeshConfig m_config;
};

} // namespace flowmesh2::net

#endif // B3_FLOWMESH2_NET_SEAT_MESH_H
