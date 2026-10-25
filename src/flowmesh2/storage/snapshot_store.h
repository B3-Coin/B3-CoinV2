// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STORAGE_SNAPSHOT_STORE_H
#define B3_FLOWMESH2_STORAGE_SNAPSHOT_STORE_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/storage/snapshot_mark.h>
#include <uint256.h>
#include <util/fs.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace leveldb {
class DB;
} // namespace leveldb

/** State snapshots on LevelDB (M4-STO-03; plan section 3.7).
 *
 * Plan section 3.7: state is held in memory; the node takes LevelDB
 * snapshots at checkpoints, and recovery is the latest snapshot plus an
 * exactly-once replay of the DECISION records after it, idempotent by slot.
 * A SNAPSHOT_MARK record in the commit journal (RecordType 5,
 * storage/snapshot_mark.h) names the slot, header hash and state root a
 * snapshot holds; the state bytes are
 * state/snapshot.h's (O-95). LevelDB is never on the per-action path.
 *
 * Layout: one LevelDB database in `dir`; key 'S' followed by the slot as 8
 * big-endian bytes (so keys sort by slot); value u32 SNAPSHOT_RECORD_VERSION
 * | header hash | state root | the state bytes. Writes are synchronous;
 * reads verify LevelDB's block checksums, and the database is opened with
 * paranoid checks.
 *
 * - Open(dir) creates the database if it is missing. STORAGE_FATAL if
 *   LevelDB cannot open it; VERIFY_FAILED (SNAPSHOT_STORE_CORRUPT) if it
 *   reports corruption.
 * - Write(mark, bytes) stores the snapshot of `mark.slot`. Writing the same
 *   mark and bytes again is a success that changes nothing; another mark or
 *   other bytes for a stored slot are INVALID_INPUT (SNAPSHOT_STORE_CONFLICT),
 *   because a slot has exactly one certified state. Empty bytes are
 *   INVALID_INPUT (SNAPSHOT_STORE_EMPTY).
 * - Load(mark) gives the bytes stored for `mark.slot`: INVALID_INPUT
 *   (SNAPSHOT_STORE_ABSENT) if there are none, VERIFY_FAILED
 *   (SNAPSHOT_STORE_MARK) if the stored header hash or state root differ.
 * - Marks() lists every stored mark, ascending slot; a value that does not
 *   parse is VERIFY_FAILED (SNAPSHOT_STORE_CORRUPT).
 * - Erase(slot) removes a stored snapshot (none stored: success).
 *
 * The store does not decide whether bytes are a header's state: a restart
 * checks that with state::SnapshotHead against the certified header. Any
 * LevelDB I/O error is STORAGE_FATAL. Thread-safe (LevelDB's own locking;
 * the store keeps no other state).
 */
namespace flowmesh2::storage {

inline constexpr uint32_t SNAPSHOT_RECORD_VERSION{1};

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view SNAPSHOT_STORE_CORRUPT{"snapshot store is corrupt"};                        //!< (VERIFY_FAILED)
inline constexpr std::string_view SNAPSHOT_STORE_CONFLICT{"another snapshot is stored for this slot"};       //!< (INVALID_INPUT)
inline constexpr std::string_view SNAPSHOT_STORE_EMPTY{"empty snapshot bytes"};                              //!< (INVALID_INPUT)
inline constexpr std::string_view SNAPSHOT_STORE_ABSENT{"no snapshot stored for this slot"};                 //!< (INVALID_INPUT)
inline constexpr std::string_view SNAPSHOT_STORE_MARK{"the stored snapshot has another header or root"};     //!< (VERIFY_FAILED)

class SnapshotStore
{
public:
    //! Open (or create) the store in `dir`. See the file comment.
    static Result<std::unique_ptr<SnapshotStore>> Open(fs::path dir);
    ~SnapshotStore();
    SnapshotStore(const SnapshotStore&) = delete;
    SnapshotStore& operator=(const SnapshotStore&) = delete;

    const fs::path& Dir() const { return m_dir; }

    //! Store the serialised state of `mark`. See the file comment.
    Result<void> Write(const SnapshotMark& mark, std::span<const unsigned char> state_bytes);
    //! The serialised state stored for `mark`. See the file comment.
    Result<std::vector<unsigned char>> Load(const SnapshotMark& mark) const;
    //! Every stored mark, ascending slot.
    Result<std::vector<SnapshotMark>> Marks() const;
    //! Remove the snapshot of `slot`, if one is stored.
    Result<void> Erase(uint64_t slot);

private:
    SnapshotStore(fs::path dir, std::unique_ptr<leveldb::DB> db);

    fs::path m_dir;
    std::unique_ptr<leveldb::DB> m_db;
};

} // namespace flowmesh2::storage

#endif // B3_FLOWMESH2_STORAGE_SNAPSHOT_STORE_H
