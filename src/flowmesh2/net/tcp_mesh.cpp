// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/net/tcp_mesh.h>

namespace flowmesh2::net {

Result<std::unique_ptr<SeatMesh>> TcpSeatMesh::Create(const TcpMeshConfig& /*config*/)
{
    return NotImplemented("M4-NET-01"); // TODO(rebirth:M4-NET-01)
}

} // namespace flowmesh2::net
