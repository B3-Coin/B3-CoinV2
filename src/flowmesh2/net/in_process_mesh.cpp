// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/net/in_process_mesh.h>

#include <utility>

namespace flowmesh2::net {

InProcessFabric::InProcessFabric(uint32_t seat_count, size_t queue_capacity)
    : m_seat_count{seat_count}, m_queue_capacity{queue_capacity}, m_inboxes(seat_count)
{
}

Result<void> InProcessFabric::Transmit(uint32_t from, uint32_t to, Lane lane, std::span<const unsigned char> frame)
{
    if (from >= m_seat_count || to >= m_seat_count) return Error{ErrorCode::INVALID_INPUT, FABRIC_SEAT_RANGE};
    LOCK(m_mutex);
    size_t& count{m_queued[Route{from, to, lane}]};
    if (count >= m_queue_capacity) return Error{ErrorCode::BUSY, FABRIC_QUEUE_FULL};
    ++count;
    m_pending.push_back(InProcessFrame{from, to, lane, std::vector<unsigned char>{frame.begin(), frame.end()}, m_next_seq++});
    return Ok();
}

std::vector<InProcessFrame> InProcessFabric::Drain()
{
    LOCK(m_mutex);
    std::vector<InProcessFrame> out{std::move(m_pending)};
    m_pending.clear();
    m_queued.clear();
    return out;
}

Result<void> InProcessFabric::Inbox(const InProcessFrame& frame)
{
    if (frame.to >= m_seat_count || frame.from >= m_seat_count) return Error{ErrorCode::INVALID_INPUT, FABRIC_SEAT_RANGE};
    LOCK(m_mutex);
    m_inboxes[frame.to].push_back(InboundFrame{frame.from, frame.lane, frame.bytes});
    return Ok();
}

std::optional<InboundFrame> InProcessFabric::Poll(uint32_t seat)
{
    if (seat >= m_seat_count) return std::nullopt;
    LOCK(m_mutex);
    std::deque<InboundFrame>& inbox{m_inboxes[seat]};
    if (inbox.empty()) return std::nullopt;
    InboundFrame frame{std::move(inbox.front())};
    inbox.pop_front();
    return frame;
}

size_t InProcessFabric::Queued() const
{
    LOCK(m_mutex);
    return m_pending.size();
}

size_t InProcessFabric::Waiting(uint32_t seat) const
{
    if (seat >= m_seat_count) return 0;
    LOCK(m_mutex);
    return m_inboxes[seat].size();
}

InProcessSeatMesh::InProcessSeatMesh(const MeshConfig& config, std::shared_ptr<InProcessFabric> fabric)
    : SeatMesh{config}, m_fabric{std::move(fabric)}
{
}

Result<InProcessMesh> InProcessSeatMesh::Create(const MeshConfig& prototype, uint32_t seat_count, size_t queue_capacity)
{
    if (const Result<void> ok{CheckConfig(prototype)}; !ok.has_value()) return ok.error();
    if (prototype.seat_count != seat_count || queue_capacity == 0) return Error{ErrorCode::INVALID_INPUT, MESH_CONFIG_INVALID};
    InProcessMesh mesh;
    mesh.fabric = std::make_shared<InProcessFabric>(seat_count, queue_capacity);
    for (uint32_t seat{0}; seat < seat_count; ++seat) {
        MeshConfig config{prototype};
        config.self_seat = seat;
        mesh.ends.push_back(std::unique_ptr<SeatMesh>{new InProcessSeatMesh{config, mesh.fabric}});
    }
    return Ok(std::move(mesh));
}

Result<std::optional<InboundFrame>> InProcessSeatMesh::Poll() { return Ok(m_fabric->Poll(Config().self_seat)); }

Result<void> InProcessSeatMesh::Transmit(uint32_t seat, Lane lane, std::span<const unsigned char> frame)
{
    return m_fabric->Transmit(Config().self_seat, seat, lane, frame);
}

} // namespace flowmesh2::net
