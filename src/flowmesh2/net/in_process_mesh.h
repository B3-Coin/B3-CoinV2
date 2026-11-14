// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NET_IN_PROCESS_MESH_H
#define B3_FLOWMESH2_NET_IN_PROCESS_MESH_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/net/lanes.h>
#include <flowmesh2/net/seat_mesh.h>
#include <sync.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

/** The in-process seat mesh of the deterministic simulator (slice R13
 * interface; landed with M4-SIM-01; plan M4; ARCHITECTURE.md section 14).
 *
 * Create(prototype, seat_count) gives one SeatMesh end per seat (seat i's
 * end has self_seat i) over one shared InProcessFabric. A Transmit queues
 * the frame in the fabric's (from, to, lane) queue, bounded by
 * `queue_capacity` frames (BUSY when full, nothing queued, as a real lane
 * would answer; the agreement resends on its timer). Nothing is delivered by
 * itself: the simulator (sim/cluster.h) calls Drain() to take every queued
 * frame in queueing order, decides with its seeded fault plan whether and
 * when each one reaches its receiver, and puts it into the receiver's inbox
 * with Inbox(); the receiver's end hands it out through Poll(). Delivery
 * order, delay, drops and partitions are therefore the simulator's alone,
 * and a seed replays byte-identically. No clock, randomness or I/O here.
 *
 * Threading: the simulation thread owns the fabric; one Mutex guards the
 * queues and inboxes for the locking rules of the base class.
 */
namespace flowmesh2::net {

inline constexpr std::string_view FABRIC_QUEUE_FULL{"the (from, to, lane) queue is full"}; //!< (BUSY)
inline constexpr std::string_view FABRIC_SEAT_RANGE{"seat outside the fabric"};            //!< (INVALID_INPUT)
//! Frames one (from, to, lane) queue holds before Transmit answers BUSY. PROVISIONAL.
inline constexpr size_t DEFAULT_FABRIC_QUEUE_CAPACITY{256};

//! One frame on its way through the fabric.
struct InProcessFrame {
    uint32_t from{0};
    uint32_t to{0};
    Lane lane{Lane::BULK};
    std::vector<unsigned char> bytes;
    uint64_t seq{0}; //!< queueing order over the whole fabric, from 0

    friend bool operator==(const InProcessFrame&, const InProcessFrame&) = default;
};

class InProcessFabric
{
public:
    InProcessFabric(uint32_t seat_count, size_t queue_capacity);

    uint32_t SeatCount() const { return m_seat_count; }
    //! Queue a frame (the ends call this); BUSY when the (from, to, lane) queue is full.
    Result<void> Transmit(uint32_t from, uint32_t to, Lane lane, std::span<const unsigned char> frame) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Every queued frame in queueing order; the queues are empty afterwards.
    std::vector<InProcessFrame> Drain() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Put a frame into its receiver's inbox (the simulator decided it arrives now). INVALID_INPUT for a seat outside the fabric.
    Result<void> Inbox(const InProcessFrame& frame) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The next frame in `seat`'s inbox, if any (the ends call this).
    std::optional<InboundFrame> Poll(uint32_t seat) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Frames queued and not drained yet.
    size_t Queued() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Frames waiting in `seat`'s inbox.
    size_t Waiting(uint32_t seat) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    using Route = std::tuple<uint32_t, uint32_t, Lane>;

    const uint32_t m_seat_count;
    const size_t m_queue_capacity;
    mutable Mutex m_mutex;
    uint64_t m_next_seq GUARDED_BY(m_mutex){0};
    std::map<Route, size_t> m_queued GUARDED_BY(m_mutex);  //!< frames per route, for the cap
    std::vector<InProcessFrame> m_pending GUARDED_BY(m_mutex);
    std::vector<std::deque<InboundFrame>> m_inboxes GUARDED_BY(m_mutex);
};

//! One fabric and its ends.
struct InProcessMesh {
    std::shared_ptr<InProcessFabric> fabric;
    std::vector<std::unique_ptr<SeatMesh>> ends; //!< ends[i] is seat i's
};

class InProcessSeatMesh final : public SeatMesh
{
public:
    /** `prototype`'s seat_count must equal `seat_count` and pass CheckConfig
     * (its self_seat is ignored); INVALID_INPUT otherwise.
     */
    static Result<InProcessMesh> Create(const MeshConfig& prototype, uint32_t seat_count, size_t queue_capacity = DEFAULT_FABRIC_QUEUE_CAPACITY);

    Result<std::optional<InboundFrame>> Poll() override;

protected:
    Result<void> Transmit(uint32_t seat, Lane lane, std::span<const unsigned char> frame) override;

private:
    InProcessSeatMesh(const MeshConfig& config, std::shared_ptr<InProcessFabric> fabric);

    std::shared_ptr<InProcessFabric> m_fabric;
};

} // namespace flowmesh2::net

#endif // B3_FLOWMESH2_NET_IN_PROCESS_MESH_H
