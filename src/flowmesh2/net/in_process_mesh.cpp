// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/net/in_process_mesh.h>

namespace flowmesh2::net {

Result<std::vector<std::unique_ptr<SeatMesh>>> InProcessSeatMesh::Create(const MeshConfig& /*prototype*/, uint32_t /*seat_count*/)
{
    return NotImplemented("M4-SIM-01"); // TODO(rebirth:M4-SIM-01)
}

} // namespace flowmesh2::net
