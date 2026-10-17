// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/bridge/forced_exit.h>

namespace flowmesh2::bridge {

Result<bool> ForcedExit::Open(uint64_t /*last_checkpoint_height*/, uint64_t /*b3_height*/)
{
    return NotImplemented("M8-EXIT-01"); // TODO(rebirth:M8-EXIT-01)
}

Result<ExitRedemption> ForcedExit::Redeem(const ExitClaim& /*claim*/, uint64_t /*b3_height*/)
{
    return NotImplemented("M8-EXIT-01"); // TODO(rebirth:M8-EXIT-01)
}

} // namespace flowmesh2::bridge
