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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/** A journal on disk: pre-zeroed segment files (slice R8; ARCHITECTURE.md 7.1;
 * segment rollover since M4-STO-06).
 *
 * CreateNew(empty_dir, role, instance_id, segment_bytes):
 * - refuses a directory that is missing, not a directory or not empty
 *   (INVALID_INPUT), and a segment size that is not a multiple of
 *   SEGMENT_PAGE_BYTES in [SEGMENT_PAGE_BYTES, MAX_SEGMENT_BYTES]
 *   (INVALID_INPUT);
 * - takes the directory lock: an exclusive flock on `<dir>/.lock`, held for
 *   the journal's lifetime, so a second FileJournal on the directory (a
 *   second fm2d on a datadir, a second fm2-cli on an outbox, in any
 *   process) is refused with BUSY (JOURNAL_DIR_LOCKED) before it reads,
 *   repairs or writes anything (review of 2026-11-16, defect 6);
 * - creates `<dir>/00000000.seg` (exclusive create, mode 0600) and writes
 *   explicit zero pages for the whole segment, so the file is allocated and
 *   its tail scans as the zero tail of ScanRecords. Every segment is
 *   filled and committed under `<name>.tmp` and renamed into place, so a
 *   crash during the fill leaves a `.tmp` (removed by the next
 *   OpenExisting), never a short segment (R7 of the same review);
 * - then JournalCommit(segment) and CommitDirectory(dir). Any I/O error on
 *   the way is STORAGE_FATAL and no journal is returned.
 *
 * Segments. The journal image is the concatenation of its segments
 * `00000000.seg`, `00000001.seg`, ... (SegmentFileName), each exactly
 * `segment_bytes` long: image byte b lives in segment b / segment_bytes at
 * b % segment_bytes. A record never straddles two segments: when it would
 * not fit in the rest of the current segment, AdmitAppend places it at byte
 * 0 of the next one and the rest of the current segment stays its zero
 * tail (the base journal writes those bytes as zeros). A record longer than
 * a segment is refused with OVERSIZE (JOURNAL_RECORD_EXCEEDS_SEGMENT),
 * consuming nothing. AdmitAppend never does I/O, so it can only place a
 * record into a segment that already exists on disk; every Barrier leaves
 * the segment after the one holding the next write created, zero-filled and
 * committed (the spare), and an append that needs a segment that does not
 * exist yet (only before the journal's first barrier, or when one group
 * commit spans two boundaries) is refused with BUSY (JOURNAL_SEGMENT_FULL):
 * nothing queued, nothing fenced; the owner barriers (or calls Rollover) and
 * appends again. Rollover() ends the current segment early: the next record
 * goes to byte 0 of the next segment, which Rollover creates if it does not
 * exist. At a segment start it is a no-op. It is the owner thread's call,
 * like Barrier (section 4); a creation failure makes the journal fatal.
 *
 * Append frames a record into memory (the journal lock is never held across
 * disk work). Barrier writes the queued bytes at their offsets with pwrite,
 * split by segment, and runs the commit on every segment it touched; then
 * it creates the spare if needed and closes the descriptors of segments
 * that lie entirely before the next write. Any write, commit or creation
 * error makes the journal fatal (journal.h).
 *
 * ReadAll reads every segment file from disk, scans them in order with the
 * chain continued across segments (ScanRecordsFrom) and returns the records
 * up to DurableSeq(). It is the owner thread's call too: concurrently with
 * a Barrier it could read a half-written record. A torn or corrupt image is
 * a refusal (what recovery makes of it is M4-STO-01).
 *
 * The segments carry no header: role and instance are bound by the chain
 * (record.h), so whoever reopens a journal must know them (O-94: the
 * deployer gives every journal of a role its own instance id; the first
 * record's chain refuses a wrong role or instance, JOURNAL_IDENTITY_MISMATCH).
 *
 * OpenExisting(dir, role, instance_id) (M4-STO-01) reopens a journal after a
 * restart or a crash. It reads every segment (consecutively numbered, all of
 * the first segment's size) and scans them in order with the chain continued
 * across segments. The records that scan clean are the journal: appended,
 * durable (every record on disk reached its barrier, or was in the barrier
 * the crash interrupted), the chain continues after them and the next record
 * goes right after them. The first refusal decides between two cases:
 * - a TORN TAIL: the refused bytes are at most one record long (the header
 *   is incomplete, or its length is plausible and the non-zero bytes end
 *   within the record it claims) and every byte after them, in this and in
 *   every later segment, is zero. That is what a crash in the middle of a
 *   barrier's write leaves. Nothing that left the node was in it (only
 *   durable records leave: a vote needs its DurableSignedVote, a decision
 *   its DurableDecision), so the bytes are zeroed and committed again and
 *   the journal continues where the clean records end. Recovery() reports
 *   the records and the truncated bytes. Both roles truncate.
 * - CORRUPTION: anything else (a CRC break or chain break before the tail, a
 *   non-zero byte after a zero header, garbage longer than one record, a
 *   non-zero byte in a later segment). For the COMMIT journal that is
 *   STORAGE_FATAL (JOURNAL_CORRUPT): the operator restores the decisions
 *   from peers (the certificates are public). For the SIGNER journal it is
 *   STORAGE_FATAL (SIGNER_KEY_RETIRED): the seat may have signed what the
 *   journal no longer shows, so its key must never sign again (the restart
 *   statements of M4-AGR-06 are how the seat rejoins with a new key).
 * OpenExisting takes the directory lock first (BUSY JOURNAL_DIR_LOCKED while
 * another FileJournal holds it) and removes the `.tmp` leftovers of a crash
 * during a segment's creation. A missing directory is INVALID_INPUT
 * (JOURNAL_DIR_MISSING); one without `00000000.seg`, or with a gap in the
 * numbering (a `.seg` numbered beyond the first missing index: the records
 * after the gap would be forgotten silently, for a signer journal the votes
 * it signed), INVALID_INPUT JOURNAL_SEGMENT_MISSING; a segment whose size is
 * not the first's or not a whole-page size in range STORAGE_FATAL
 * (JOURNAL_SEGMENT_SIZE); a stat error STORAGE_FATAL (JOURNAL_SEGMENT_READ),
 * never the end of the numbering. A segment or `.lock` that is a symlink is
 * never followed, whatever it points to (no check before the open follows
 * it either): STORAGE_FATAL (JOURNAL_SEGMENT_READ, JOURNAL_DIR_LOCK).
 * A missing spare (the empty segment after
 * the last record) is not a gap. After a reopen the spare segment exists,
 * as after a barrier. The logs rebuild their indexes from ReadAll
 * (SignerLog::Recover, CommitLog::Recover); state replays the DECISION
 * bodies (state::ReplayDecisions).
 *
 * POSIX only in practice: the Windows branches are not compiled on this
 * host (O-24 pattern).
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
inline constexpr std::string_view JOURNAL_SEGMENT_READ{"JOURNAL_SEGMENT_READ"};                     //!< (STORAGE_FATAL) ReadAll could not read a segment file
inline constexpr std::string_view JOURNAL_SEGMENT_FULL{"JOURNAL_SEGMENT_FULL"};                     //!< (BUSY) the next segment does not exist yet: Barrier or Rollover first
inline constexpr std::string_view JOURNAL_RECORD_EXCEEDS_SEGMENT{"JOURNAL_RECORD_EXCEEDS_SEGMENT"}; //!< (OVERSIZE) a record longer than a segment
inline constexpr std::string_view JOURNAL_SEGMENT_MISSING{"JOURNAL_SEGMENT_MISSING"};               //!< (INVALID_INPUT) no first segment, or a gap in the numbering
inline constexpr std::string_view JOURNAL_DIR_LOCKED{"JOURNAL_DIR_LOCKED"};                         //!< (BUSY) another FileJournal (in another process or this one) holds the directory's lock file
inline constexpr std::string_view JOURNAL_DIR_LOCK{"JOURNAL_DIR_LOCK"};                             //!< (STORAGE_FATAL) the directory's lock file could not be created or locked
inline constexpr std::string_view JOURNAL_IDENTITY_MISMATCH{"JOURNAL_IDENTITY_MISMATCH"};           //!< (VERIFY_FAILED) the first record is not this role's and instance's
inline constexpr std::string_view JOURNAL_CORRUPT{"JOURNAL_CORRUPT"};                               //!< (STORAGE_FATAL) a commit journal with a bad record before its tail
inline constexpr std::string_view SIGNER_KEY_RETIRED{"SIGNER_KEY_RETIRED"};                         //!< (STORAGE_FATAL) a signer journal with a gap or CRC break: the key must retire

//! What OpenExisting found (M4-STO-01).
struct RecoveryReport {
    uint64_t records{0};         //!< records that scanned clean: the journal's appended and durable sequence number
    uint64_t truncated_bytes{0}; //!< bytes of a torn tail zeroed again (0 for a clean image)
    friend bool operator==(const RecoveryReport&, const RecoveryReport&) = default;
};

//! `<index as 8 hex digits>.seg`; index 0 is FIRST_SEGMENT_NAME.
std::string SegmentFileName(uint64_t index);

/** Replaces the JournalCommit call of Barrier. Production code never passes
 * one (CreateNew uses JournalCommit). It exists for fault injection in tests
 * and the simulator: a hook receives the segment's real descriptor and must
 * end in JournalCommit or a failure, for example by closing the descriptor
 * first so the real JournalCommit fails. Segment creation (CreateNew, the
 * spare, Rollover) always uses the real JournalCommit.
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

    //! Reopen a journal after a restart or a crash (see the file comment).
    static Result<std::unique_ptr<FileJournal>> OpenExisting(const fs::path& dir, JournalRole role, uint64_t instance_id,
                                                             BarrierObserver* observer = nullptr);
    //! What OpenExisting found; zeros for a journal CreateNew made.
    const RecoveryReport& Recovery() const { return m_recovery; }
    //! The durable records, read back from every segment file (see the file comment).
    Result<std::vector<JournalRecord>> ReadAll() const override EXCLUSIVE_LOCKS_REQUIRED(!m_segments_mutex);
    //! End the current segment early (see the file comment). FENCED on a fatal journal.
    Result<void> Rollover() EXCLUSIVE_LOCKS_REQUIRED(!m_segments_mutex);

    //! The first segment's path (`<dir>/00000000.seg`).
    const fs::path& SegmentPath() const { return m_first_segment_path; }
    //! `<dir>/<SegmentFileName(index)>`, whether or not that segment exists.
    fs::path SegmentPathOf(uint64_t index) const;
    uint64_t SegmentBytes() const { return m_segment_bytes; }
    //! Segments created on disk so far (the current one and the spare included).
    uint64_t SegmentCount() const EXCLUSIVE_LOCKS_REQUIRED(!m_segments_mutex);
    //! The segment the next record goes to (a requested rollover applied).
    uint64_t CurrentSegment() const EXCLUSIVE_LOCKS_REQUIRED(!m_segments_mutex);

private:
    FileJournal(JournalRole role, uint64_t instance_id, BarrierObserver* observer, fs::path dir, int lock_fd, int first_fd,
                uint64_t segment_bytes, SegmentCommitHook commit);
    FileJournal(JournalRole role, uint64_t instance_id, BarrierObserver* observer, fs::path dir, int lock_fd, std::vector<int> fds,
                uint64_t segment_bytes, const RecoveredState& recovered, const RecoveryReport& report);
    //! OpenExisting once the directory lock `lock_fd` is held; the caller releases it on an error.
    static Result<std::unique_ptr<FileJournal>> OpenLocked(const fs::path& dir, JournalRole role, uint64_t instance_id,
                                                           BarrierObserver* observer, int lock_fd);

    Result<uint64_t> AdmitAppend(uint64_t append_number, uint64_t offset, size_t size) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_segments_mutex);
    Result<void> Persist(uint64_t barrier_number, uint64_t offset, std::span<const unsigned char> bytes) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_segments_mutex);

    //! Create `<dir>/<SegmentFileName(index)>` zero-filled and committed under a temporary name, renamed into place, and
    //! commit the directory; the open descriptor.
    static Result<int> CreateSegment(const fs::path& dir, uint64_t index, uint64_t segment_bytes);
    //! Create every missing segment up to `index` (no lock during the I/O; takes the table lock only to update it).
    Result<void> EnsureSegments(uint64_t index) EXCLUSIVE_LOCKS_REQUIRED(!m_segments_mutex);

    const fs::path m_dir;
    const fs::path m_first_segment_path;
    const uint64_t m_segment_bytes;
    const SegmentCommitHook m_commit;
    const RecoveryReport m_recovery;
    const int m_lock_fd; //!< the exclusive lock on `<dir>/.lock`, released by the destructor

    //! Never held together with the base journal's lock across I/O: AdmitAppend
    //! takes it under the base lock without I/O; Persist, Rollover and ReadAll
    //! take it only to copy or update the table.
    mutable Mutex m_segments_mutex;
    //! Descriptor per created segment, by index; -1 once a segment lies entirely before the next write.
    std::vector<int> m_fds GUARDED_BY(m_segments_mutex);
    bool m_rollover_requested GUARDED_BY(m_segments_mutex){false};
};

} // namespace flowmesh2::storage

#endif // B3_FLOWMESH2_STORAGE_FILE_JOURNAL_H
