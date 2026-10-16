// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/storage/snapshot_store.h>

namespace flowmesh2::storage {

Result<void> SnapshotStore::Write(const SnapshotMark& /*mark*/, std::span<const unsigned char> /*state_bytes*/)
{
    return NotImplemented("M4-STO-03"); // TODO(rebirth:M4-STO-03)
}

Result<std::vector<unsigned char>> SnapshotStore::Load(const SnapshotMark& /*mark*/) const
{
    return NotImplemented("M4-STO-03"); // TODO(rebirth:M4-STO-03)
}

} // namespace flowmesh2::storage
