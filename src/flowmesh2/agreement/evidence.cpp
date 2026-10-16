// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/agreement/evidence.h>

namespace flowmesh2::agreement {

Result<void> EvidenceStore::Record(const Evidence& /*evidence*/)
{
    return NotImplemented("M4-AGR-07"); // TODO(rebirth:M4-AGR-07)
}

Result<std::vector<Evidence>> EvidenceStore::Load() const
{
    return NotImplemented("M4-AGR-07"); // TODO(rebirth:M4-AGR-07)
}

} // namespace flowmesh2::agreement
