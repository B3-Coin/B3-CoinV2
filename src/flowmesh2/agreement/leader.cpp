// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/agreement/leader.h>

namespace flowmesh2::agreement {

Result<uint32_t> LeaderSchedule::LeaderFor(const AuthorityVersion& /*authority*/, uint64_t /*term*/, const SlowSet& /*slow*/) const
{
    return NotImplemented("M4-AGR-03"); // TODO(rebirth:M4-AGR-03)
}

} // namespace flowmesh2::agreement
