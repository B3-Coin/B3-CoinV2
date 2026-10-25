// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STORAGE_SNAPSHOT_MARK_H
#define B3_FLOWMESH2_STORAGE_SNAPSHOT_MARK_H

#include <flowmesh2/core/result.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/** The SNAPSHOT_MARK record (RecordType 5; M4-STO-03, O-95).
 *
 * A mark names the committed slot whose state a snapshot holds: the slot,
 * its header hash and its state root. The node writes the snapshot to its
 * SnapshotStore first (synchronously), then appends the mark to the commit
 * journal; a mark that is durable therefore names a snapshot that was
 * stored, and a restart may start from it (StateOwner::Restore).
 *
 * Payload: u64 slot | header hash (32) | state root (32), 72 bytes, Bitcoin
 * serialization. Decode is exact: OVERSIZE when longer, DECODE when shorter.
 */
namespace flowmesh2::storage {

//! What a snapshot holds: the state after `slot`, whose header hash and state root are given.
struct SnapshotMark {
    uint64_t slot{0};
    uint256 header_hash;
    uint256 state_root;

    static constexpr size_t ENCODED_BYTES{8 + 32 + 32};
    std::vector<unsigned char> Encode() const;
    static Result<SnapshotMark> Decode(std::span<const unsigned char> bytes);

    friend bool operator==(const SnapshotMark&, const SnapshotMark&) = default;
};

} // namespace flowmesh2::storage

#endif // B3_FLOWMESH2_STORAGE_SNAPSHOT_MARK_H
