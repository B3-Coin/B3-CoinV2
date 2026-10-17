// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NET_IN_PROCESS_MESH_H
#define B3_FLOWMESH2_NET_IN_PROCESS_MESH_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/net/seat_mesh.h>

#include <cstdint>
#include <memory>
#include <vector>

/** The in-process seat mesh of the deterministic simulator (slice R13:
 * interface and stub; plan M4; ARCHITECTURE.md section 14).
 *
 * Target (M4-SIM-01): Create(prototype, seat_count) gives one SeatMesh per
 * seat (prototype.self_seat is ignored; seat i's mesh has self_seat i),
 * sharing bounded per-(sender, receiver, lane) queues inside one process.
 * Delivery order, delay, drops and partitions are decided only by the
 * simulator's FakeClock and seeded FaultPlan (sim/, M4-SIM-02), so a seed
 * replays byte-identically; Transmit answers BUSY when the queue of that
 * lane to that seat is full. The k = 1 walking skeleton (R14) needs no
 * mesh; SimCluster with n > 1 (M4-SIM-01) uses this one.
 *
 * Stub: Create returns NotImplemented(M4-SIM-01).
 */
namespace flowmesh2::net {

class InProcessSeatMesh
{
public:
    InProcessSeatMesh() = delete;

    //! One mesh end per seat (see the file comment). Stub M4-SIM-01.
    static Result<std::vector<std::unique_ptr<SeatMesh>>> Create(const MeshConfig& prototype, uint32_t seat_count);
};

} // namespace flowmesh2::net

#endif // B3_FLOWMESH2_NET_IN_PROCESS_MESH_H
