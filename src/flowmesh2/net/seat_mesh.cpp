// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/net/seat_mesh.h>

#include <utility>

namespace flowmesh2::net {

namespace {

//! The MessageType byte of an agreement message envelope, if it has one of this version.
std::optional<agreement::MessageType> EnvelopeType(std::span<const unsigned char> bytes)
{
    if (bytes.size() < 2 || bytes[0] != agreement::MESSAGE_VERSION) return std::nullopt;
    return static_cast<agreement::MessageType>(bytes[1]);
}

} // namespace

Result<void> SeatMesh::CheckConfig(const MeshConfig& config)
{
    const Result<void> valid{Validate(config.profile)};
    if (!valid.has_value()) return valid.error();
    if (config.seat_count == 0 || config.self_seat >= config.seat_count || config.max_frame_bytes == 0) {
        return Error{ErrorCode::INVALID_INPUT, MESH_CONFIG_INVALID};
    }
    return Ok();
}

Result<void> SeatMesh::CheckRoute(uint32_t seat, Lane lane, size_t bytes) const
{
    if (!IsKnown(lane)) return Error{ErrorCode::INVALID_INPUT, MESH_LANE_UNKNOWN};
    if (seat >= m_config.seat_count) return Error{ErrorCode::INVALID_INPUT, MESH_SEAT_RANGE};
    if (seat == m_config.self_seat) return Error{ErrorCode::INVALID_INPUT, MESH_SELF};
    if (bytes > m_config.max_frame_bytes) return Error{ErrorCode::OVERSIZE, MESH_FRAME_OVERSIZE};
    return Ok();
}

Result<void> SeatMesh::SendVote(uint32_t seat, const agreement::OutboundVote& vote, const uint256& header_hash)
{
    // The route first (as for every send), the frame's size once it is encoded.
    if (Result<void> route{CheckRoute(seat, Lane::CRITICAL, 0)}; !route.has_value()) return route.error();
    agreement::Vote message;
    message.phase = vote.Key().phase;
    message.epoch = vote.Key().epoch;
    message.view = vote.Key().view;
    message.slot = vote.Key().slot;
    message.header_hash = header_hash;
    message.seat = m_config.self_seat;
    message.signature = vote.Signature();
    // Inclusion IDs ride on PREPARE votes once M4-AGR-04 defines them.
    if (Result<void> checked{agreement::CheckVote(message)}; !checked.has_value()) return checked.error();
    Result<std::vector<unsigned char>> bytes{agreement::EncodeMessage(agreement::Message{std::move(message)}, m_config.profile)};
    if (!bytes.has_value()) return bytes.error();
    if (bytes->size() > m_config.max_frame_bytes) return Error{ErrorCode::OVERSIZE, MESH_FRAME_OVERSIZE};
    return Transmit(seat, Lane::CRITICAL, *bytes);
}

Result<void> SeatMesh::Send(uint32_t seat, Lane lane, std::span<const unsigned char> bytes)
{
    if (Result<void> route{CheckRoute(seat, lane, bytes.size())}; !route.has_value()) return route.error();
    if (lane == Lane::CRITICAL || lane == Lane::PROPOSAL) {
        const std::optional<agreement::MessageType> type{EnvelopeType(bytes)};
        if (type == agreement::MessageType::VOTE) return Error{ErrorCode::INVALID_INPUT, MESH_VOTE_NOT_TYPED};
        const bool fits{lane == Lane::PROPOSAL
                            ? type == agreement::MessageType::PROPOSAL
                            : (type == agreement::MessageType::VIEW_CHANGE || type == agreement::MessageType::NEW_VIEW ||
                               type == agreement::MessageType::EVIDENCE)};
        if (!fits) return Error{ErrorCode::INVALID_INPUT, MESH_LANE_CONTENT};
    }
    return Transmit(seat, lane, bytes);
}

Result<void> SeatMesh::Send(uint32_t seat, std::span<const unsigned char> message)
{
    const std::optional<agreement::MessageType> type{EnvelopeType(message)};
    if (type == agreement::MessageType::VOTE) return Error{ErrorCode::INVALID_INPUT, MESH_VOTE_NOT_TYPED};
    if (type == agreement::MessageType::PROPOSAL) return Send(seat, Lane::PROPOSAL, message);
    if (type == agreement::MessageType::VIEW_CHANGE || type == agreement::MessageType::NEW_VIEW || type == agreement::MessageType::EVIDENCE) {
        return Send(seat, Lane::CRITICAL, message);
    }
    return Error{ErrorCode::INVALID_INPUT, MESH_LANE_CONTENT};
}

} // namespace flowmesh2::net
