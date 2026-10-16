// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/agreement/restart_fence.h>

namespace flowmesh2::agreement {

Result<FencePoint> RestartFence::Collect(std::span<const HighWaterStatement> /*statements*/, const SeatSet& /*seats*/,
                                         uint32_t /*self_seat*/)
{
    return NotImplemented("M4-AGR-06"); // TODO(rebirth:M4-AGR-06)
}

} // namespace flowmesh2::agreement
