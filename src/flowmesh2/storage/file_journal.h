// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STORAGE_FILE_JOURNAL_H
#define B3_FLOWMESH2_STORAGE_FILE_JOURNAL_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/storage/journal.h>
#include <flowmesh2/storage/record.h>
#include <sync.h>
#include <util/fs.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/** A journal on disk: a numbered sequence of pre-zeroed segment files
 * (slice R8, segment rollover M4-STO-06; ARCHITECTURE.md 7.1).
 *
 * CreateNew(empty_dir, role, instance_id, segment_bytes):
 * - refuses a directory that is missing, not a directory or not empty
 *   (INVALID_INPUT), and a segment size that is not a multiple of
 *   SEGMENT_PAGE_BYTES in [SEGMENT_PAGE_BYTES, MAX_SEGMENT_BYTES]
 *   (INVALID_INPUT);
 * - creates `<dir>/00000000.seg` (exclusive create, mode 0600) and writes
 *   explicit zero pages for the whole segment, so the file is allocated and
 *   its tail scans as the zero tail of ScanRecords, then the segment header;
 * - then JournalCommit(segment) and CommitDirectory(dir). Any I/O error on
 *   the way is STORAGE_FATAL and no journal is returned.
 *
 * Segments (M4-STO-06, O-94). Segment k is `<dir>/` + SegmentName(k), its
 * index in 8 decimal digits; every segment has the size of the first. The
 * journal image is the concatenation of the segments: segment k holds image
 * bytes [k * segment_bytes, (k + 1) * segment_bytes). Each segment starts
 * with a SEGMENT_HEADER_BYTES header naming the role, the instance, the
 * segment's index and its size (EncodeSegmentHeader), written when the
 * segment is created and never again; its records follow. A record never
 * spans two segments: an append whose record does not fit in the rest of
 * its segment is queued after zero padding up to the first record byte of
 * the next segment (AdmitAppend; a barrier skips the header bytes), and a
 * record larger than a segment's record area is refused with OVERSIZE
 * (JOURNAL_RECORD_SEGMENT) without making the journal fatal. The record
 * chain runs across segments, so a later segment is valid only after its
 * predecessors.
 *
 * Append frames a record into memory (the journal lock is never held across
 * disk work). Barrier writes the queued bytes segment by segment with pwrite
 * and commits each segment before it writes the next, so no byte of a later
 * segment is written before every earlier byte is durable. A segment the
 * barrier reaches that does not exist yet is created first: zero-filled under
 * the temporary name SegmentName(k) + SEGMENT_TEMP_SUFFIX, committed with
 * JournalCommit, renamed into place, and the directory committed (always the
 * strict commits, never the test hook); its header is written before that
 * commit. Any write, creation or commit error
 * makes the journal fatal (journal.h). Rollover() does the same creation
 * ahead of time, off the barrier path.
 *
 * OpenExisting verifies the entire image and commits every segment before
 * restoring the sequence, chain and byte boundary. It accepts segments 0 to
 * n-1 with no gap, at most one leftover temporary file of segment n (a
 * creation that crashed before its rename: ignored, and rewritten by the
 * next creation) and nothing else; each segment must continue the chain; a
 * segment without records is allowed only after the last record; and a
 * segment's first record must not have fit in the rest of the segment before
 * it, so the image is exactly what the writer produces. Every segment's
 * header must name the role, the instance, its own index and the common
 * size: VERIFY_FAILED (JOURNAL_SEGMENT_HEADER), or ORDER_VIOLATION for
 * another role. Corruption, a gap or
 * a nonzero torn tail is a refusal: no bytes are truncated, keys retired or
 * files deleted. The operator must investigate such a refusal. An exclusive
 * advisory lock on the first segment prevents cooperating concurrent
 * writers. Checksums cannot detect a coherently rolled-back backup, and two
 * journals of one role with one instance id are indistinguishable (instance
 * uniqueness is M4-STO-01); callers must retain the trusted store identity
 * externally. ReadAll reads only the immutable prefix covered by a
 * successful barrier. Snapshot/state restoration is a separate layer.
 * POSIX only in practice: the Windows branches are not compiled on this host
 * (O-24 pattern).
 */
namespace flowmesh2::storage {

inline constexpr uint64_t SEGMENT_PAGE_BYTES{4096};
//! Plan section 3.7: 64 MiB segments. PROVISIONAL.
inline constexpr uint64_t DEFAULT_SEGMENT_BYTES{64ULL * 1024 * 1024};
inline constexpr uint64_t MAX_SEGMENT_BYTES{1024ULL * 1024 * 1024};
inline constexpr std::string_view FIRST_SEGMENT_NAME{"00000000.seg"};
//! Segment names have 8 decimal digits, so indexes stay below this (M4-STO-06).
inline constexpr uint64_t MAX_SEGMENTS{100'000'000};
//! Suffix of a segment being created, before its rename into place.
inline constexpr std::string_view SEGMENT_TEMP_SUFFIX{".tmp"};

/** Segment header (M4-STO-06, O-94), little-endian:
 * magic "B3FM2SEG" (8) | u32 version SEGMENT_FORMAT_VERSION | u8 role |
 * 3 zero bytes | u64 instance id | u64 segment index | u64 segment bytes |
 * 20 zero bytes | u32 crc32c of the 60 bytes before it.
 */
inline constexpr size_t SEGMENT_HEADER_BYTES{64};
inline constexpr uint32_t SEGMENT_FORMAT_VERSION{1};

inline constexpr std::string_view JOURNAL_DIR_NOT_EMPTY{"JOURNAL_DIR_NOT_EMPTY"};
inline constexpr std::string_view JOURNAL_DIR_MISSING{"JOURNAL_DIR_MISSING"};
inline constexpr std::string_view JOURNAL_SEGMENT_SIZE{"JOURNAL_SEGMENT_SIZE"};
inline constexpr std::string_view JOURNAL_SEGMENT_CREATE{"JOURNAL_SEGMENT_CREATE"};
inline constexpr std::string_view JOURNAL_SEGMENT_WRITE{"JOURNAL_SEGMENT_WRITE"};
inline constexpr std::string_view JOURNAL_SEGMENT_OPEN{"JOURNAL_SEGMENT_OPEN"};
inline constexpr std::string_view JOURNAL_SEGMENT_READ{"JOURNAL_SEGMENT_READ"};
inline constexpr std::string_view JOURNAL_SEGMENT_LAYOUT{"JOURNAL_SEGMENT_LAYOUT"};
inline constexpr std::string_view JOURNAL_SEGMENT_LOCKED{"JOURNAL_SEGMENT_LOCKED"};
inline constexpr std::string_view JOURNAL_RECORD_SEGMENT{"JOURNAL_RECORD_SEGMENT"};         //!< (OVERSIZE) a record larger than a segment
inline constexpr std::string_view JOURNAL_SEGMENTS_EXHAUSTED{"JOURNAL_SEGMENTS_EXHAUSTED"}; //!< (OVERSIZE) segment index MAX_SEGMENTS reached
inline constexpr std::string_view JOURNAL_SEGMENT_HEADER{"JOURNAL_SEGMENT_HEADER"}; //!< (VERIFY_FAILED; ORDER_VIOLATION for another role)

//! The file name of segment `index` (index below MAX_SEGMENTS): 8 decimal digits and ".seg".
std::string SegmentName(uint64_t index);

//! The header of segment `index` of a journal (see SEGMENT_HEADER_BYTES).
std::array<unsigned char, SEGMENT_HEADER_BYTES> EncodeSegmentHeader(JournalRole role, uint64_t instance_id, uint64_t index,
                                                                    uint64_t segment_bytes);

/** Replaces the JournalCommit call of Barrier. Production code never passes
 * one (CreateNew uses JournalCommit). It exists for fault injection in tests
 * and the simulator: a hook receives a segment's real descriptor and must
 * end in JournalCommit or a failure, for example by closing the descriptor
 * first so the real JournalCommit fails. A barrier calls it once for each
 * segment it wrote, in segment order, and once for the segment of its
 * offset when it wrote nothing.
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

    //! Reopen a complete, valid sequence of segments without repairing or deleting anything.
    static Result<std::unique_ptr<FileJournal>> OpenExisting(const fs::path& dir, JournalRole role, uint64_t instance_id);
    //! Read the successfully barriered prefix back from disk.
    Result<std::vector<JournalRecord>> ReadAll() const override EXCLUSIVE_LOCKS_REQUIRED(!m_segments_mutex);

    /** Create the next segment now, so the barrier that first needs it does
     * not pay for its zero fill. Creates at most one segment beyond the one
     * the next durable record goes to, and otherwise does nothing. FENCED
     * (JOURNAL_FENCED) for a fatal journal; ORDER_VIOLATION
     * (JOURNAL_BARRIER_CONCURRENT) while a barrier writes; OVERSIZE
     * (JOURNAL_SEGMENTS_EXHAUSTED) at MAX_SEGMENTS. A failed creation is
     * STORAGE_FATAL for this call only: it acknowledges nothing, so the
     * journal stays usable, and a barrier that needs the segment creates it
     * itself (and turns the journal fatal if it cannot). Called by the
     * thread that issues barriers (section 4).
     */
    Result<void> Rollover() EXCLUSIVE_LOCKS_REQUIRED(!m_segments_mutex);

    //! The first segment.
    const fs::path& SegmentPath() const { return m_segment_path; }
    uint64_t SegmentBytes() const { return m_segment_bytes; }
    //! Segments that exist, written or prepared.
    size_t SegmentCount() const EXCLUSIVE_LOCKS_REQUIRED(!m_segments_mutex);

private:
    FileJournal(JournalRole role, uint64_t instance_id, BarrierObserver* observer, fs::path dir, std::vector<int> fds,
                uint64_t segment_bytes, SegmentCommitHook commit, const ScanResult* recovered = nullptr);

    Result<uint64_t> AdmitAppend(uint64_t append_number, uint64_t offset, size_t size) const override;
    Result<void> Persist(uint64_t barrier_number, uint64_t offset, std::span<const unsigned char> bytes) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_segments_mutex);

    //! The descriptor of segment `index`, creating it if it is the next one. Only with disk ownership.
    Result<int> SegmentFd(uint64_t index) EXCLUSIVE_LOCKS_REQUIRED(!m_segments_mutex);
    std::vector<int> SegmentFds() const EXCLUSIVE_LOCKS_REQUIRED(!m_segments_mutex);

    const fs::path m_dir;
    const fs::path m_segment_path;
    const uint64_t m_segment_bytes;
    const SegmentCommitHook m_commit;
    //! Guards the descriptor list only; never held across disk work.
    mutable Mutex m_segments_mutex;
    std::vector<int> m_fds GUARDED_BY(m_segments_mutex);
    //! Who may write or create segments: 0 nobody, 1 a barrier, 2 Rollover.
    std::atomic<int> m_disk_owner{0};
};

} // namespace flowmesh2::storage

#endif // B3_FLOWMESH2_STORAGE_FILE_JOURNAL_H
