// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/agreement/inclusion.h>

namespace flowmesh2::agreement {

Result<std::vector<ShortId>> InclusionList::Build(std::span<const HeldAction> /*held*/, uint64_t /*slot*/)
{
    return NotImplemented("M4-AGR-04"); // TODO(rebirth:M4-AGR-04)
}

Result<void> InclusionList::Check(std::span<const Vote> /*prepare_votes*/, size_t /*seat_count*/,
                                  std::span<const ShortId> /*included*/, std::span<const ShortId> /*now_invalid*/)
{
    return NotImplemented("M4-AGR-04"); // TODO(rebirth:M4-AGR-04)
}

} // namespace flowmesh2::agreement
