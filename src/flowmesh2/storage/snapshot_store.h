// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STORAGE_SNAPSHOT_STORE_H
#define B3_FLOWMESH2_STORAGE_SNAPSHOT_STORE_H

#include <flowmesh2/core/result.h>
#include <uint256.h>
#include <util/fs.h>

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

/** State snapshots (target contract; stub M4-STO-03).
 *
 * Plan section 3.7: state is held in memory; the read replica takes LevelDB
 * snapshots at checkpoints, and recovery is the latest snapshot plus an
 * exactly-once replay of the DECISION records after it, idempotent by slot.
 * A SNAPSHOT_MARK record in the commit journal (RecordType 5) names the slot,
 * header hash and state root a snapshot holds. M4-STO-03 owns the format of
 * the state bytes (R9's CoreState serialisation), the LevelDB layout, the
 * mark record and StateOwner::Restore (R9); LevelDB never sits on the
 * per-action path.
 *
 * In the outline the store only remembers its directory; it touches no file.
 */
namespace flowmesh2::storage {

//! What a snapshot holds: the state after `slot`, whose header hash and state root are given.
struct SnapshotMark {
    uint64_t slot{0};
    uint256 header_hash;
    uint256 state_root;

    friend bool operator==(const SnapshotMark&, const SnapshotMark&) = default;
};

class SnapshotStore
{
public:
    explicit SnapshotStore(fs::path dir) : m_dir{std::move(dir)} {}

    const fs::path& Dir() const { return m_dir; }

    //! Store the serialised state of `mark`. Stub: M4-STO-03.
    Result<void> Write(const SnapshotMark& mark, std::span<const unsigned char> state_bytes);
    //! The serialised state stored for `mark`. Stub: M4-STO-03.
    Result<std::vector<unsigned char>> Load(const SnapshotMark& mark) const;

private:
    fs::path m_dir;
};

} // namespace flowmesh2::storage

#endif // B3_FLOWMESH2_STORAGE_SNAPSHOT_STORE_H
