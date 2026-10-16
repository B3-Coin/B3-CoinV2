// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STORAGE_FILE_JOURNAL_H
#define B3_FLOWMESH2_STORAGE_FILE_JOURNAL_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/storage/journal.h>
#include <flowmesh2/storage/record.h>
#include <util/fs.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

/** A journal on disk: one pre-zeroed segment file (slice R8; ARCHITECTURE.md 7.1).
 *
 * CreateNew(empty_dir, role, instance_id, segment_bytes):
 * - refuses a directory that is missing, not a directory or not empty
 *   (INVALID_INPUT), and a segment size that is not a multiple of
 *   SEGMENT_PAGE_BYTES in [SEGMENT_PAGE_BYTES, MAX_SEGMENT_BYTES]
 *   (INVALID_INPUT);
 * - creates `<dir>/00000000.seg` (exclusive create, mode 0600) and writes
 *   explicit zero pages for the whole segment, so the file is allocated and
 *   its tail scans as the zero tail of ScanRecords;
 * - then JournalCommit(segment) and CommitDirectory(dir). Any I/O error on
 *   the way is STORAGE_FATAL and no journal is returned.
 *
 * Append frames a record into memory (the journal lock is never held across
 * disk work). Barrier writes the queued bytes at their offsets with pwrite
 * and runs JournalCommit; any write or commit error makes the journal fatal
 * (journal.h). A record that would not fit in the rest of the segment is
 * refused with NotImplemented(M4-STO-06) (segment rollover) without making
 * the journal fatal; Rollover() itself is that stub.
 *
 * The segment carries no header: role and instance are bound by the chain
 * (record.h), so whoever reopens a segment must know them.
 *
 * Stubs: OpenExisting and ReadAll (M4-STO-01: reopen, torn-tail handling,
 * key retirement on a gap or CRC break); Rollover (M4-STO-06). Until
 * M4-STO-01 lands the outline makes no crash-recovery claim for real files.
 * POSIX only in practice: the Windows branches are not compiled on this host
 * (O-24 pattern).
 */
namespace flowmesh2::storage {

inline constexpr uint64_t SEGMENT_PAGE_BYTES{4096};
//! Plan section 3.7: 64 MiB segments. PROVISIONAL.
inline constexpr uint64_t DEFAULT_SEGMENT_BYTES{64ULL * 1024 * 1024};
inline constexpr uint64_t MAX_SEGMENT_BYTES{1024ULL * 1024 * 1024};
inline constexpr std::string_view FIRST_SEGMENT_NAME{"00000000.seg"};

inline constexpr std::string_view JOURNAL_DIR_NOT_EMPTY{"JOURNAL_DIR_NOT_EMPTY"};
inline constexpr std::string_view JOURNAL_DIR_MISSING{"JOURNAL_DIR_MISSING"};
inline constexpr std::string_view JOURNAL_SEGMENT_SIZE{"JOURNAL_SEGMENT_SIZE"};
inline constexpr std::string_view JOURNAL_SEGMENT_CREATE{"JOURNAL_SEGMENT_CREATE"};
inline constexpr std::string_view JOURNAL_SEGMENT_WRITE{"JOURNAL_SEGMENT_WRITE"};

/** Replaces the JournalCommit call of Barrier. Production code never passes
 * one (CreateNew uses JournalCommit). It exists for fault injection in tests
 * and the simulator: a hook receives the segment's real descriptor and must
 * end in JournalCommit or a failure, for example by closing the descriptor
 * first so the real JournalCommit fails.
 */
using SegmentCommitHook = Result<void> (*)(int fd);

class FileJournal final : public Journal
{
public:
    ~FileJournal() override;

    static Result<std::unique_ptr<FileJournal>> CreateNew(const fs::path& empty_dir, JournalRole role, uint64_t instance_id,
                                                          uint64_t segment_bytes, BarrierObserver* observer = nullptr);
    //! CreateNew whose barriers call `commit` instead of JournalCommit (see SegmentCommitHook).
    static Result<std::unique_ptr<FileJournal>> CreateNewWithCommitHook(const fs::path& empty_dir, JournalRole role,
                                                                        uint64_t instance_id, uint64_t segment_bytes,
                                                                        SegmentCommitHook commit,
                                                                        BarrierObserver* observer = nullptr);

    //! Reopen a journal after a restart. Stub: M4-STO-01.
    static Result<std::unique_ptr<FileJournal>> OpenExisting(const fs::path& dir, JournalRole role, uint64_t instance_id);
    //! Read the durable records back from disk. Stub: M4-STO-01.
    Result<std::vector<JournalRecord>> ReadAll() const override;
    //! Start the next segment. Stub: M4-STO-06.
    Result<void> Rollover();

    const fs::path& SegmentPath() const { return m_segment_path; }
    uint64_t SegmentBytes() const { return m_segment_bytes; }

private:
    FileJournal(JournalRole role, uint64_t instance_id, BarrierObserver* observer, fs::path segment_path, int fd,
                uint64_t segment_bytes, SegmentCommitHook commit);

    Result<void> AdmitAppend(uint64_t append_number, uint64_t offset, size_t size) const override;
    Result<void> Persist(uint64_t barrier_number, uint64_t offset, std::span<const unsigned char> bytes) override;

    const fs::path m_segment_path;
    const int m_fd;
    const uint64_t m_segment_bytes;
    const SegmentCommitHook m_commit;
};

} // namespace flowmesh2::storage

#endif // B3_FLOWMESH2_STORAGE_FILE_JOURNAL_H
