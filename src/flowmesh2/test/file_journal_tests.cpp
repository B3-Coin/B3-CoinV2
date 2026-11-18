// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <flowmesh2/storage/file_journal.h>
#include <flowmesh2/storage/journal_commit.h>
#include <flowmesh2/storage/record.h>
#include <flowmesh2/test/util/storage_fixtures.h>
#include <util/fs.h>

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

using namespace flowmesh2;
using namespace flowmesh2::storage;
using namespace flowmesh2::test;

namespace {

constexpr uint64_t SMALL_SEGMENT{16 * SEGMENT_PAGE_BYTES}; // 64 KiB keeps the zero fill and F_FULLFSYNC cheap

std::unique_ptr<FileJournal> MustCreate(const fs::path& dir, JournalRole role, uint64_t instance, uint64_t segment_bytes = SMALL_SEGMENT,
                                        SegmentCommitHook hook = nullptr)
{
    Result<std::unique_ptr<FileJournal>> journal{hook == nullptr ? FileJournal::CreateNew(dir, role, instance, segment_bytes)
                                                                 : FileJournal::CreateNewWithCommitHook(dir, role, instance, segment_bytes, hook)};
    RequireStorageOk(journal, "CreateNew");
    return std::move(journal).value();
}

//! Closes the segment descriptor, runs the real JournalCommit on the closed
//! descriptor (EBADF), then gives the journal its descriptor back so its
//! destructor closes its own file. The failure comes from JournalCommit itself.
Result<void> CommitOnClosedFd(int fd)
{
    const int saved{dup(fd)};
    BOOST_REQUIRE(saved >= 0);
    BOOST_REQUIRE(close(fd) == 0);
    const Result<void> committed{JournalCommit(fd)};
    BOOST_REQUIRE(dup2(saved, fd) == fd);
    BOOST_REQUIRE(close(saved) == 0);
    return committed;
}

//! The `.seg` files in `dir` (the directory also holds the journal's `.lock` file since the review fix of 2026-11-17).
size_t SegmentFiles(const fs::path& dir)
{
    size_t n{0};
    for (const fs::directory_entry& entry : fs::directory_iterator{dir}) {
        if (fs::PathToString(entry.path().filename()).ends_with(".seg")) ++n;
    }
    return n;
}

} // namespace

BOOST_AUTO_TEST_SUITE(file_journal_tests)

BOOST_AUTO_TEST_CASE(create_refuses_bad_dirs_and_sizes)
{
    const TestDir root{"create_refuses"};
    // Not empty: one stray file is enough, and nothing is created.
    const fs::path busy{root.Sub("busy")};
    WriteFile(busy / fs::u8path("stray"), Bytes("x"));
    RequireRefusal(FileJournal::CreateNew(busy, JournalRole::COMMIT, 1, SMALL_SEGMENT), ErrorCode::INVALID_INPUT, JOURNAL_DIR_NOT_EMPTY, "non-empty dir");
    BOOST_CHECK(!fs::exists(busy / fs::u8path(FIRST_SEGMENT_NAME)));
    const fs::path with_subdir{root.Sub("with_subdir")};
    fs::create_directories(with_subdir / fs::u8path("sub"));
    RequireRefusal(FileJournal::CreateNew(with_subdir, JournalRole::COMMIT, 1, SMALL_SEGMENT), ErrorCode::INVALID_INPUT, JOURNAL_DIR_NOT_EMPTY, "dir with a subdir");
    // Missing, or a file instead of a directory.
    RequireRefusal(FileJournal::CreateNew(root.path / fs::u8path("missing"), JournalRole::COMMIT, 1, SMALL_SEGMENT),
                   ErrorCode::INVALID_INPUT, JOURNAL_DIR_MISSING, "missing dir");
    RequireRefusal(FileJournal::CreateNew(busy / fs::u8path("stray"), JournalRole::COMMIT, 1, SMALL_SEGMENT),
                   ErrorCode::INVALID_INPUT, JOURNAL_DIR_MISSING, "a file, not a dir");
    // Segment sizes: whole pages within [1 page, 1 GiB].
    const fs::path empty{root.Sub("empty")};
    for (uint64_t bad : {uint64_t{0}, SEGMENT_PAGE_BYTES - 1, SEGMENT_PAGE_BYTES + 1, MAX_SEGMENT_BYTES + SEGMENT_PAGE_BYTES}) {
        RequireRefusal(FileJournal::CreateNew(empty, JournalRole::SIGNER, 1, bad), ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_SIZE, "segment size");
    }
    RequireCode(FileJournal::CreateNew(empty, JournalRole{0}, 1, SMALL_SEGMENT), ErrorCode::INVALID_INPUT, "unknown role");
    RequireCode(FileJournal::CreateNewWithCommitHook(empty, JournalRole::SIGNER, 1, SMALL_SEGMENT, nullptr), ErrorCode::INVALID_INPUT, "null hook");
    BOOST_CHECK(fs::is_empty(empty));
    // The smallest segment (one page) works.
    auto one_page{MustCreate(empty, JournalRole::SIGNER, 1, SEGMENT_PAGE_BYTES)};
    BOOST_CHECK_EQUAL(fs::file_size(one_page->SegmentPath()), SEGMENT_PAGE_BYTES);
    // A second CreateNew in the same (now non-empty) dir is refused.
    RequireRefusal(FileJournal::CreateNew(empty, JournalRole::SIGNER, 1, SMALL_SEGMENT), ErrorCode::INVALID_INPUT, JOURNAL_DIR_NOT_EMPTY, "dir reused");
}

BOOST_AUTO_TEST_CASE(fresh_segment_is_zero_filled_at_full_size)
{
    const TestDir root{"zero_filled"};
    const uint64_t segment{SMALL_SEGMENT + 3 * SEGMENT_PAGE_BYTES}; // not a multiple of the 64 KiB fill chunk
    auto journal{MustCreate(root.path, JournalRole::COMMIT, 3, segment)};
    BOOST_CHECK(journal->SegmentPath() == root.path / fs::u8path(FIRST_SEGMENT_NAME));
    BOOST_CHECK_EQUAL(journal->SegmentBytes(), segment);
    const std::vector<unsigned char> image{ReadFile(journal->SegmentPath())};
    BOOST_CHECK_EQUAL(image.size(), segment);
    BOOST_CHECK(std::all_of(image.begin(), image.end(), [](unsigned char b) { return b == 0; }));
    // Exactly one file in the directory.
    BOOST_CHECK_EQUAL(SegmentFiles(root.path), 1);
    const Result<ScanResult> scan{ScanRecords(image, JournalRole::COMMIT, 3)};
    RequireStorageOk(scan, "scan of the fresh segment");
    BOOST_CHECK(scan->records.empty());
}

BOOST_AUTO_TEST_CASE(append_barrier_and_scan_reproduce_records)
{
    const TestDir root{"scan_reproduces"};
    RecordingObserver observer;
    Result<std::unique_ptr<FileJournal>> created{FileJournal::CreateNew(root.path, JournalRole::SIGNER, 42, SMALL_SEGMENT, &observer)};
    RequireStorageOk(created, "CreateNew");
    FileJournal& journal{**created};

    BOOST_CHECK_EQUAL(journal.Append(RecordType::ACCEPTED_PROPOSAL, Bytes("proposal")).value(), 1U);
    BOOST_CHECK_EQUAL(journal.Append(RecordType::INTENT, Bytes("intent")).value(), 2U);
    // Before the barrier nothing reached the file.
    {
        const std::vector<unsigned char> image{ReadFile(journal.SegmentPath())};
        BOOST_CHECK(std::all_of(image.begin(), image.end(), [](unsigned char b) { return b == 0; }));
    }
    BOOST_CHECK_EQUAL(journal.Barrier().value(), 2U);
    BOOST_CHECK_EQUAL(journal.Append(RecordType::SIGNED_VOTE, Bytes("vote")).value(), 3U);
    BOOST_CHECK_EQUAL(journal.Barrier().value(), 3U);
    BOOST_CHECK(journal.Status() == (JournalStatus{3, 3, false}));

    const std::vector<unsigned char> image{ReadFile(journal.SegmentPath())};
    BOOST_CHECK_EQUAL(image.size(), SMALL_SEGMENT);
    const Result<ScanResult> scan{ScanRecords(image, JournalRole::SIGNER, 42)};
    RequireStorageOk(scan, "scan of the segment");
    BOOST_REQUIRE_EQUAL(scan->records.size(), 3U);
    BOOST_CHECK(scan->records[0].type == RecordType::ACCEPTED_PROPOSAL && scan->records[0].payload == Bytes("proposal"));
    BOOST_CHECK(scan->records[1].type == RecordType::INTENT && scan->records[1].payload == Bytes("intent"));
    BOOST_CHECK(scan->records[2].type == RecordType::SIGNED_VOTE && scan->records[2].payload == Bytes("vote"));
    BOOST_CHECK_EQUAL(scan->end_offset, 3 * RECORD_OVERHEAD_BYTES + 8 + 6 + 4);

    // The same records in a memory-equivalent image: framing is identical.
    std::vector<unsigned char> expected;
    uint256 prev{JournalGenesisChain(JournalRole::SIGNER, 42)};
    for (const auto& [type, text] : std::vector<std::pair<RecordType, std::string>>{
             {RecordType::ACCEPTED_PROPOSAL, "proposal"}, {RecordType::INTENT, "intent"}, {RecordType::SIGNED_VOTE, "vote"}}) {
        const EncodedRecord rec{EncodeRecord(prev, type, Bytes(text)).value()};
        expected.insert(expected.end(), rec.bytes.begin(), rec.bytes.end());
        prev = rec.chain;
    }
    BOOST_CHECK(std::equal(expected.begin(), expected.end(), image.begin()));
    BOOST_CHECK_EQUAL(observer.events.size(), 7U);
    // Reading back from disk (landed with M4-STO-06): the durable records only.
    BOOST_CHECK_EQUAL(journal.Append(RecordType::INTENT, Bytes("not yet durable")).value(), 4U);
    const Result<std::vector<JournalRecord>> all{journal.ReadAll()};
    RequireStorageOk(all, "ReadAll");
    BOOST_REQUIRE_EQUAL(all->size(), 3U);
    BOOST_CHECK(all->at(0) == scan->records[0] && all->at(2) == scan->records[2]);
}

BOOST_AUTO_TEST_CASE(commit_failure_on_closed_fd_is_sticky)
{
    // The primitives: a closed descriptor and a pipe (EINVAL/ENOTSUP) both fail, with no fallback.
    int pipe_fds[2];
    BOOST_REQUIRE(pipe(pipe_fds) == 0);
    RequireCode(JournalCommit(pipe_fds[1]), ErrorCode::STORAGE_FATAL, "JournalCommit on a pipe");
    RequireCode(JournalDirectoryCommit(pipe_fds[1]), ErrorCode::STORAGE_FATAL, "JournalDirectoryCommit on a pipe");
    BOOST_REQUIRE(close(pipe_fds[0]) == 0 && close(pipe_fds[1]) == 0);
    RequireCode(JournalCommit(pipe_fds[1]), ErrorCode::STORAGE_FATAL, "JournalCommit on a closed fd");
    RequireRefusal(JournalDirectoryCommit(pipe_fds[1]), ErrorCode::STORAGE_FATAL, COMMIT_DIRECTORY_FSYNC, "JournalDirectoryCommit on a closed fd");
#if defined(__APPLE__)
    RequireRefusal(JournalCommit(pipe_fds[1]), ErrorCode::STORAGE_FATAL, COMMIT_F_FULLFSYNC, "macOS uses F_FULLFSYNC");
    // No fsync fallback: on /dev/null fsync succeeds but F_FULLFSYNC does not (ENODEV), and that is fatal.
    {
        const int devnull{open("/dev/null", O_RDWR | O_CLOEXEC)};
        BOOST_REQUIRE(devnull >= 0);
        BOOST_REQUIRE(fsync(devnull) == 0);
        RequireRefusal(JournalCommit(devnull), ErrorCode::STORAGE_FATAL, COMMIT_F_FULLFSYNC, "F_FULLFSYNC unsupported, fsync would work");
        BOOST_REQUIRE(close(devnull) == 0);
    }
#elif defined(__linux__)
    RequireRefusal(JournalCommit(pipe_fds[1]), ErrorCode::STORAGE_FATAL, COMMIT_FDATASYNC, "Linux uses fdatasync");
#endif
    const TestDir root{"closed_fd"};
    RequireRefusal(CommitDirectory(root.path / fs::u8path("missing")), ErrorCode::STORAGE_FATAL, COMMIT_DIRECTORY_OPEN, "missing dir");
    RequireStorageOk(CommitDirectory(root.path), "CommitDirectory");

    // A journal whose barrier commit runs on a closed descriptor.
    auto journal{MustCreate(root.Sub("journal"), JournalRole::COMMIT, 5, SMALL_SEGMENT, &CommitOnClosedFd)};
    BOOST_CHECK_EQUAL(journal->Append(RecordType::DECISION, Bytes("d")).value(), 1U);
    RequireCode(journal->Barrier(), ErrorCode::STORAGE_FATAL, "barrier on a closed fd");
    BOOST_CHECK(journal->Status() == (JournalStatus{1, 0, true}));
    // Sticky and never retried: the hook is not called again (it would fail the same way, but
    // the journal does not even try), Append is FENCED.
    for (int i{0}; i < 3; ++i) {
        RequireRefusal(journal->Barrier(), ErrorCode::STORAGE_FATAL, JOURNAL_FATAL, "barrier after fatal");
        RequireRefusal(journal->Append(RecordType::APPLIED, Bytes("a")), ErrorCode::FENCED, JOURNAL_FENCED, "append after fatal");
    }
    BOOST_CHECK(journal->Status() == (JournalStatus{1, 0, true}));
}

//! Every record of every segment file in `dir`, scanned the way a reopen will (chain continued across segments).
std::vector<JournalRecord> ScanSegments(const fs::path& dir, JournalRole role, uint64_t instance, size_t segments)
{
    std::vector<JournalRecord> out;
    uint256 prev{JournalGenesisChain(role, instance)};
    for (size_t index{0}; index < segments; ++index) {
        const Result<ScanResult> scan{ScanRecordsFrom(ReadFile(dir / fs::u8path(SegmentFileName(index))), role, prev, out.size())};
        RequireStorageOk(scan, "scan of segment " + std::to_string(index));
        out.insert(out.end(), scan->records.begin(), scan->records.end());
        prev = scan->last_chain;
    }
    return out;
}

BOOST_AUTO_TEST_CASE(append_past_the_segment_end_moves_to_the_next_segment)
{
    // M4-STO-06. One-page segments: the first record fills page 0 exactly.
    const TestDir root{"rollover_append"};
    auto journal{MustCreate(root.path, JournalRole::COMMIT, 6, SEGMENT_PAGE_BYTES)};
    const std::vector<unsigned char> fill(SEGMENT_PAGE_BYTES - RECORD_OVERHEAD_BYTES, 0xAB);
    // A record that fits in no segment is OVERSIZE, consumes nothing and fences nothing.
    const std::vector<unsigned char> too_big(SEGMENT_PAGE_BYTES - RECORD_OVERHEAD_BYTES + 1, 0xAB);
    RequireRefusal(journal->Append(RecordType::DECISION, too_big), ErrorCode::OVERSIZE, JOURNAL_RECORD_EXCEEDS_SEGMENT, "record above a segment");
    BOOST_CHECK(journal->Status() == (JournalStatus{0, 0, false}));
    BOOST_CHECK_EQUAL(journal->SegmentCount(), 1U);
    BOOST_CHECK_EQUAL(journal->Append(RecordType::DECISION, fill).value(), 1U);
    // Before the first barrier only segment 0 exists, so a record that does
    // not fit is BUSY (nothing queued, nothing fenced): the owner barriers first.
    RequireRefusal(journal->Append(RecordType::APPLIED, {}), ErrorCode::BUSY, JOURNAL_SEGMENT_FULL, "no next segment yet");
    BOOST_CHECK(journal->Status() == (JournalStatus{1, 0, false}));
    BOOST_CHECK_EQUAL(journal->Barrier().value(), 1U);
    // The barrier created the spare segment 00000001.seg, zero-filled and committed.
    BOOST_CHECK_EQUAL(journal->SegmentCount(), 2U);
    BOOST_CHECK(fs::exists(journal->SegmentPathOf(1)));
    BOOST_CHECK_EQUAL(ReadFile(journal->SegmentPathOf(1)).size(), SEGMENT_PAGE_BYTES);
    BOOST_CHECK_EQUAL(SegmentFiles(root.path), 2);
    // Now the append goes to byte 0 of segment 1.
    BOOST_CHECK_EQUAL(journal->Append(RecordType::APPLIED, Bytes("applied")).value(), 2U);
    BOOST_CHECK_EQUAL(journal->CurrentSegment(), 1U);
    BOOST_CHECK_EQUAL(journal->Barrier().value(), 2U);
    BOOST_CHECK_EQUAL(journal->SegmentCount(), 3U); // the invariant: a spare after every barrier

    const std::vector<unsigned char> image0{ReadFile(journal->SegmentPathOf(0))};
    const Result<ScanResult> scan0{ScanRecords(image0, JournalRole::COMMIT, 6)};
    RequireStorageOk(scan0, "scan of the full segment 0");
    BOOST_CHECK_EQUAL(scan0->records.size(), 1U);
    BOOST_CHECK_EQUAL(scan0->end_offset, SEGMENT_PAGE_BYTES);
    const std::vector<unsigned char> image1{ReadFile(journal->SegmentPathOf(1))};
    const Result<ScanResult> scan1{ScanRecordsFrom(image1, JournalRole::COMMIT, scan0->last_chain, 1)};
    RequireStorageOk(scan1, "scan of segment 1 continued from segment 0");
    BOOST_REQUIRE_EQUAL(scan1->records.size(), 1U);
    BOOST_CHECK_EQUAL(scan1->records[0].seq, 2U);
    BOOST_CHECK(scan1->records[0].payload == Bytes("applied"));
    // Segment 1 alone does not continue the genesis chain.
    RequireRefusal(ScanRecords(image1, JournalRole::COMMIT, 6), ErrorCode::VERIFY_FAILED, RECORD_CHAIN, "segment 1 from genesis");
    // ReadAll returns both, across the segments, from disk.
    const Result<std::vector<JournalRecord>> all{journal->ReadAll()};
    RequireStorageOk(all, "ReadAll across two segments");
    BOOST_REQUIRE_EQUAL(all->size(), 2U);
    BOOST_CHECK(all->at(0).payload == fill && all->at(1).payload == Bytes("applied"));
    BOOST_CHECK(*all == ScanSegments(root.path, JournalRole::COMMIT, 6, journal->SegmentCount()));
}

BOOST_AUTO_TEST_CASE(rollover_pads_the_segment_and_group_commits_across_it)
{
    // M4-STO-06. Rollover() starts the next segment early: the rest of the
    // current one stays its zero tail and the next record lands at byte 0 of
    // the next segment, all in one barrier.
    const TestDir root{"rollover_explicit"};
    RecordingObserver observer;
    Result<std::unique_ptr<FileJournal>> created{FileJournal::CreateNew(root.path, JournalRole::SIGNER, 9, 4 * SEGMENT_PAGE_BYTES, &observer)};
    RequireStorageOk(created, "CreateNew");
    FileJournal& journal{**created};
    // At a segment start a rollover has nothing to pad: a no-op that creates no file.
    RequireStorageOk(journal.Rollover(), "Rollover of a fresh journal");
    BOOST_CHECK_EQUAL(journal.CurrentSegment(), 0U);
    BOOST_CHECK_EQUAL(journal.SegmentCount(), 1U);
    BOOST_CHECK_EQUAL(journal.Append(RecordType::INTENT, Bytes("one")).value(), 1U);
    BOOST_CHECK_EQUAL(journal.Barrier().value(), 1U);
    BOOST_CHECK_EQUAL(journal.SegmentCount(), 2U);
    // Rollover with record 2 still unwritten in segment 0: the next record
    // lands at byte 0 of segment 1 (the spare the barrier created); a second
    // Rollover is idempotent.
    BOOST_CHECK_EQUAL(journal.Append(RecordType::INTENT, Bytes("two")).value(), 2U);
    RequireStorageOk(journal.Rollover(), "Rollover");
    RequireStorageOk(journal.Rollover(), "Rollover twice");
    BOOST_CHECK_EQUAL(journal.CurrentSegment(), 1U);
    BOOST_CHECK_EQUAL(journal.Append(RecordType::SIGNED_VOTE, Bytes("three")).value(), 3U);
    BOOST_CHECK_EQUAL(journal.CurrentSegment(), 1U);
    BOOST_CHECK_EQUAL(journal.Barrier().value(), 3U); // records 2 (segment 0) and 3 (segment 1) in one barrier
    BOOST_CHECK_EQUAL(journal.SegmentCount(), 3U);
    BOOST_CHECK(journal.Status() == (JournalStatus{3, 3, false}));

    const std::vector<unsigned char> image0{ReadFile(journal.SegmentPathOf(0))};
    const Result<ScanResult> scan0{ScanRecords(image0, JournalRole::SIGNER, 9)};
    RequireStorageOk(scan0, "segment 0");
    BOOST_REQUIRE_EQUAL(scan0->records.size(), 2U);
    BOOST_CHECK(std::all_of(image0.begin() + static_cast<std::ptrdiff_t>(scan0->end_offset), image0.end(), [](unsigned char b) { return b == 0; }));
    const Result<ScanResult> scan1{ScanRecordsFrom(ReadFile(journal.SegmentPathOf(1)), JournalRole::SIGNER, scan0->last_chain, 2)};
    RequireStorageOk(scan1, "segment 1");
    BOOST_REQUIRE_EQUAL(scan1->records.size(), 1U);
    BOOST_CHECK_EQUAL(scan1->records[0].seq, 3U);
    BOOST_CHECK_EQUAL(scan1->end_offset, RECORD_OVERHEAD_BYTES + 5);
    const Result<std::vector<JournalRecord>> all{journal.ReadAll()};
    RequireStorageOk(all, "ReadAll");
    BOOST_CHECK_EQUAL(all->size(), 3U);
    // Observer: 3 appends, 2 barriers with start and end.
    BOOST_CHECK_EQUAL(observer.events.size(), 7U);

    // Rollover on a fatal journal is refused like an append.
    {
        const TestDir fatal_root{"rollover_fatal"};
        auto fatal{MustCreate(fatal_root.path, JournalRole::COMMIT, 1, SEGMENT_PAGE_BYTES, &CommitOnClosedFd)};
        BOOST_CHECK_EQUAL(fatal->Append(RecordType::DECISION, Bytes("d")).value(), 1U);
        RequireCode(fatal->Barrier(), ErrorCode::STORAGE_FATAL, "failed commit");
        RequireRefusal(fatal->Rollover(), ErrorCode::FENCED, JOURNAL_FENCED, "rollover of a fatal journal");
    }
}

BOOST_AUTO_TEST_CASE(many_segments_read_back_after_a_crash)
{
    // M4-STO-06 crash test: records across five one-page segments, the
    // journal object dropped after every barrier (what a crash leaves), the
    // files scanned afresh and compared with what the journal appended.
    const TestDir root{"rollover_crash"};
    std::vector<std::vector<unsigned char>> appended;
    size_t segments{0};
    {
        auto journal{MustCreate(root.path, JournalRole::COMMIT, 5, SEGMENT_PAGE_BYTES)};
        for (unsigned round{0}; round < 12; ++round) {
            // Three records of about a third of a page: every third append crosses a boundary.
            std::vector<unsigned char> payload(SEGMENT_PAGE_BYTES / 3, static_cast<unsigned char>(round));
            Result<uint64_t> seq{journal->Append(RecordType::DECISION, payload)};
            if (!seq.has_value()) {
                // The spare does not exist before the first barrier of the journal: barrier, then retry once.
                RequireRefusal(seq, ErrorCode::BUSY, JOURNAL_SEGMENT_FULL, "append before the spare exists");
                BOOST_CHECK_EQUAL(journal->Barrier().value(), journal->Status().appended_seq);
                seq = journal->Append(RecordType::DECISION, payload);
            }
            RequireStorageOk(seq, "append");
            appended.push_back(payload);
            BOOST_CHECK_EQUAL(journal->Barrier().value(), appended.size());
            // The invariant after every barrier: the segment after the current one exists.
            BOOST_CHECK(fs::exists(journal->SegmentPathOf(journal->CurrentSegment() + 1)));
        }
        segments = journal->SegmentCount();
        const Result<std::vector<JournalRecord>> all{journal->ReadAll()};
        RequireStorageOk(all, "ReadAll");
        BOOST_CHECK_EQUAL(all->size(), appended.size());
    }
    BOOST_CHECK_GE(segments, 5U);
    const std::vector<JournalRecord> records{ScanSegments(root.path, JournalRole::COMMIT, 5, segments)};
    BOOST_REQUIRE_EQUAL(records.size(), appended.size());
    for (size_t i{0}; i < records.size(); ++i) {
        BOOST_CHECK_EQUAL(records[i].seq, i + 1);
        BOOST_CHECK(records[i].payload == appended[i]);
    }
    // A record never straddles two segments: every segment alone scans clean from its predecessor's chain.
    for (size_t index{0}; index < segments; ++index) {
        BOOST_CHECK_EQUAL(ReadFile(root.path / fs::u8path(SegmentFileName(index))).size(), SEGMENT_PAGE_BYTES);
    }
}

//! Overwrite bytes of a file at `offset` (a torn write or corruption).
void PatchFile(const fs::path& file, uint64_t offset, const std::vector<unsigned char>& bytes)
{
    const int fd{open(file.c_str(), O_WRONLY)};
    BOOST_REQUIRE(fd >= 0);
    BOOST_REQUIRE(pwrite(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset)) == static_cast<ssize_t>(bytes.size()));
    BOOST_REQUIRE(close(fd) == 0);
}

BOOST_AUTO_TEST_CASE(open_existing_reopens_and_continues_the_chain)
{
    // M4-STO-01. Three durable records over two one-page segments, the
    // journal object dropped (a restart), then reopened.
    const TestDir root{"reopen"};
    {
        auto journal{MustCreate(root.path, JournalRole::COMMIT, 8, SEGMENT_PAGE_BYTES)};
        const std::vector<unsigned char> big(SEGMENT_PAGE_BYTES / 2, 0x11);
        BOOST_CHECK_EQUAL(journal->Append(RecordType::DECISION, big).value(), 1U);
        BOOST_CHECK_EQUAL(journal->Barrier().value(), 1U);
        BOOST_CHECK_EQUAL(journal->Append(RecordType::DECISION, big).value(), 2U); // segment 1
        BOOST_CHECK_EQUAL(journal->Append(RecordType::APPLIED, Bytes("a")).value(), 3U);
        BOOST_CHECK_EQUAL(journal->Barrier().value(), 3U);
        BOOST_CHECK_EQUAL(journal->CurrentSegment(), 1U);
    }
    // The segments carry no header: the first record's chain binds role and instance (O-94).
    RequireRefusal(FileJournal::OpenExisting(root.path, JournalRole::SIGNER, 8), ErrorCode::VERIFY_FAILED, JOURNAL_IDENTITY_MISMATCH, "wrong role");
    RequireRefusal(FileJournal::OpenExisting(root.path, JournalRole::COMMIT, 9), ErrorCode::VERIFY_FAILED, JOURNAL_IDENTITY_MISMATCH, "wrong instance");
    RequireRefusal(FileJournal::OpenExisting(root.path / fs::u8path("missing"), JournalRole::COMMIT, 8), ErrorCode::INVALID_INPUT, JOURNAL_DIR_MISSING, "missing dir");
    const fs::path no_segment{root.Sub("no_segment")};
    RequireRefusal(FileJournal::OpenExisting(no_segment, JournalRole::COMMIT, 8), ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_MISSING, "dir without a first segment");

    Result<std::unique_ptr<FileJournal>> reopened{FileJournal::OpenExisting(root.path, JournalRole::COMMIT, 8)};
    RequireStorageOk(reopened, "OpenExisting");
    FileJournal& journal{**reopened};
    BOOST_CHECK(journal.Status() == (JournalStatus{3, 3, false}));
    BOOST_CHECK_EQUAL(journal.SegmentBytes(), SEGMENT_PAGE_BYTES);
    BOOST_CHECK_EQUAL(journal.Recovery().records, 3U);
    BOOST_CHECK_EQUAL(journal.Recovery().truncated_bytes, 0U);
    BOOST_CHECK_EQUAL(journal.CurrentSegment(), 1U);
    BOOST_CHECK_EQUAL(journal.SegmentCount(), 3U); // the spare exists after a reopen too
    const Result<std::vector<JournalRecord>> all{journal.ReadAll()};
    RequireStorageOk(all, "ReadAll after reopen");
    BOOST_REQUIRE_EQUAL(all->size(), 3U);
    // The chain continues: record 4 chains on record 3 and the whole image scans clean.
    BOOST_CHECK_EQUAL(journal.Append(RecordType::APPLIED, Bytes("b")).value(), 4U);
    BOOST_CHECK_EQUAL(journal.Barrier().value(), 4U);
    const std::vector<JournalRecord> records{ScanSegments(root.path, JournalRole::COMMIT, 8, journal.SegmentCount())};
    BOOST_REQUIRE_EQUAL(records.size(), 4U);
    BOOST_CHECK(records[3].payload == Bytes("b"));
    BOOST_CHECK(records[3].chain == NextChain(records[2].chain, RecordType::APPLIED, Bytes("b")));
    // Tokens mint against the recovered sequence numbers too.
    BOOST_CHECK_EQUAL(journal.DurableSeq(), 4U);

    // A fresh, empty journal reopens with nothing.
    const TestDir empty{"reopen_empty"};
    { MustCreate(empty.path, JournalRole::SIGNER, 1, SEGMENT_PAGE_BYTES); }
    Result<std::unique_ptr<FileJournal>> fresh{FileJournal::OpenExisting(empty.path, JournalRole::SIGNER, 1)};
    RequireStorageOk(fresh, "reopen an empty journal");
    BOOST_CHECK((*fresh)->Status() == (JournalStatus{0, 0, false}));
}

BOOST_AUTO_TEST_CASE(torn_tail_is_truncated_on_reopen)
{
    // M4-STO-01. A crash in the middle of a barrier's write leaves a record's
    // prefix at the image end, then the segment's zero tail. Nothing that left
    // the node was in it (only durable records leave), so recovery drops it
    // and the journal continues where the durable records end, for both roles.
    for (const JournalRole role : {JournalRole::COMMIT, JournalRole::SIGNER}) {
        const TestDir root{role == JournalRole::COMMIT ? "torn_commit" : "torn_signer"};
        const RecordType type{role == JournalRole::COMMIT ? RecordType::DECISION : RecordType::INTENT};
        uint256 last_chain;
        uint64_t end{0};
        {
            auto journal{MustCreate(root.path, role, 3, SMALL_SEGMENT)};
            BOOST_CHECK_EQUAL(journal->Append(type, Bytes("one")).value(), 1U);
            BOOST_CHECK_EQUAL(journal->Append(type, Bytes("two")).value(), 2U);
            BOOST_CHECK_EQUAL(journal->Barrier().value(), 2U);
            const ScanResult scan{ScanRecords(ReadFile(journal->SegmentPath()), role, 3).value()};
            last_chain = scan.last_chain;
            end = scan.end_offset;
        }
        const fs::path seg{root.path / fs::u8path(FIRST_SEGMENT_NAME)};
        const EncodedRecord third{EncodeRecord(last_chain, type, Bytes("three-torn")).value()};
        const std::vector<unsigned char> half(third.bytes.begin(), third.bytes.begin() + static_cast<std::ptrdiff_t>(third.bytes.size() / 2));
        PatchFile(seg, end, half);
        // With an intact header the zero tail supplies the missing bytes, so the torn record fails its CRC.
        RequireRefusal(ScanRecords(ReadFile(seg), role, 3), ErrorCode::VERIFY_FAILED, RECORD_CRC, "the torn image does not scan");
        Result<std::unique_ptr<FileJournal>> reopened{FileJournal::OpenExisting(root.path, role, 3)};
        RequireStorageOk(reopened, "reopen with a torn tail");
        FileJournal& journal{**reopened};
        BOOST_CHECK(journal.Status() == (JournalStatus{2, 2, false}));
        BOOST_CHECK_EQUAL(journal.Recovery().records, 2U);
        BOOST_CHECK_EQUAL(journal.Recovery().truncated_bytes, half.size());
        // The tail is zero again on disk, and the journal continues at record 3.
        const std::vector<unsigned char> image{ReadFile(seg)};
        BOOST_CHECK(std::all_of(image.begin() + static_cast<std::ptrdiff_t>(end), image.end(), [](unsigned char b) { return b == 0; }));
        BOOST_CHECK_EQUAL(journal.Append(type, Bytes("three")).value(), 3U);
        BOOST_CHECK_EQUAL(journal.Barrier().value(), 3U);
        const ScanResult after{ScanRecords(ReadFile(seg), role, 3).value()};
        BOOST_REQUIRE_EQUAL(after.records.size(), 3U);
        BOOST_CHECK(after.records[2].payload == Bytes("three"));
        // A torn header shorter than the 9 header bytes is a torn tail as well.
        {
            const TestDir short_root{role == JournalRole::COMMIT ? "torn_commit_short" : "torn_signer_short"};
            uint64_t short_end{0};
            {
                auto j{MustCreate(short_root.path, role, 3, SMALL_SEGMENT)};
                BOOST_CHECK_EQUAL(j->Append(type, Bytes("one")).value(), 1U);
                BOOST_CHECK_EQUAL(j->Barrier().value(), 1U);
                short_end = ScanRecords(ReadFile(j->SegmentPath()), role, 3).value().end_offset;
            }
            PatchFile(short_root.path / fs::u8path(FIRST_SEGMENT_NAME), short_end, {0x05, 0x01, 0x02});
            Result<std::unique_ptr<FileJournal>> j{FileJournal::OpenExisting(short_root.path, role, 3)};
            RequireStorageOk(j, "reopen with a torn header");
            BOOST_CHECK((*j)->Status() == (JournalStatus{1, 1, false}));
            BOOST_CHECK_EQUAL((*j)->Recovery().truncated_bytes, 3U);
        }
    }
}

BOOST_AUTO_TEST_CASE(corruption_is_fatal_and_retires_the_signer_key)
{
    // M4-STO-01. A flipped byte inside a durable record, or non-zero bytes
    // beyond one torn record, is corruption, not a torn tail: the commit
    // journal refuses to open (JOURNAL_CORRUPT, restore from peers) and the
    // signer journal retires its key (SIGNER_KEY_RETIRED, M4-AGR-06).
    for (const JournalRole role : {JournalRole::COMMIT, JournalRole::SIGNER}) {
        const TestDir root{role == JournalRole::COMMIT ? "corrupt_commit" : "corrupt_signer"};
        const RecordType type{role == JournalRole::COMMIT ? RecordType::DECISION : RecordType::INTENT};
        const std::string_view want{role == JournalRole::COMMIT ? JOURNAL_CORRUPT : SIGNER_KEY_RETIRED};
        {
            auto journal{MustCreate(root.path, role, 4, SMALL_SEGMENT)};
            for (int i{0}; i < 3; ++i) RequireStorageOk(journal->Append(type, Bytes("r")), "append");
            BOOST_CHECK_EQUAL(journal->Barrier().value(), 3U);
        }
        const fs::path seg{root.path / fs::u8path(FIRST_SEGMENT_NAME)};
        const std::vector<unsigned char> clean{ReadFile(seg)};
        // Flip the payload byte of record 2.
        const size_t flip{RECORD_OVERHEAD_BYTES + 1 + RECORD_HEADER_BYTES};
        PatchFile(seg, flip, {static_cast<unsigned char>(clean[flip] ^ 0xff)});
        RequireRefusal(FileJournal::OpenExisting(root.path, role, 4), ErrorCode::STORAGE_FATAL, want, "flipped byte in the middle");
        // Restored, it opens; garbage longer than the record its header claims is corruption again.
        PatchFile(seg, 0, clean);
        RequireStorageOk(FileJournal::OpenExisting(root.path, role, 4), "clean again");
        const ScanResult scan{ScanRecords(clean, role, 4).value()};
        const std::vector<unsigned char> garbage(2 * RECORD_OVERHEAD_BYTES + 64, 0xEE);
        PatchFile(seg, scan.end_offset, garbage);
        RequireRefusal(FileJournal::OpenExisting(root.path, role, 4), ErrorCode::STORAGE_FATAL, want, "garbage beyond one record");
        // Non-zero bytes in a later segment after the image end are corruption too.
        PatchFile(seg, 0, clean);
        PatchFile(root.path / fs::u8path(SegmentFileName(1)), 100, {0x01});
        RequireRefusal(FileJournal::OpenExisting(root.path, role, 4), ErrorCode::STORAGE_FATAL, want, "bytes in the spare");
    }
}

BOOST_AUTO_TEST_CASE(a_missing_middle_segment_is_refused_on_reopen)
{
    // Review of 2026-11-16, defect 7: OpenExisting used to stop at the first missing segment and silently forget
    // every record after it (for a signer journal: votes it signed). A gap in the numbering is the refusal the
    // header promises, JOURNAL_SEGMENT_MISSING. A missing spare (the empty segment after the last record) is not
    // a gap: the numbering just ends there and the reopen recreates it.
    const TestDir root{"segment_gap"};
    size_t segments{0};
    uint64_t current{0};
    {
        auto journal{MustCreate(root.path, JournalRole::SIGNER, 3, SEGMENT_PAGE_BYTES)};
        const std::vector<unsigned char> big(SEGMENT_PAGE_BYTES / 2, 0x33);
        for (unsigned i{0}; i < 6; ++i) {
            Result<uint64_t> seq{journal->Append(RecordType::INTENT, big)};
            if (!seq.has_value()) {
                RequireRefusal(seq, ErrorCode::BUSY, JOURNAL_SEGMENT_FULL, "append before the spare exists");
                BOOST_CHECK_EQUAL(journal->Barrier().value(), journal->Status().appended_seq);
                seq = journal->Append(RecordType::INTENT, big);
            }
            RequireStorageOk(seq, "append");
            BOOST_CHECK_EQUAL(journal->Barrier().value(), i + 1);
        }
        segments = journal->SegmentCount();
        current = journal->CurrentSegment();
    }
    BOOST_REQUIRE_GE(segments, 4U);
    BOOST_REQUIRE_EQUAL(segments, current + 2); // the spare after the current segment
    // The spare removed: no gap, the reopen recreates it and every record is there.
    BOOST_REQUIRE(fs::remove(root.path / fs::u8path(SegmentFileName(segments - 1))));
    {
        Result<std::unique_ptr<FileJournal>> reopened{FileJournal::OpenExisting(root.path, JournalRole::SIGNER, 3)};
        RequireStorageOk(reopened, "reopen without the spare");
        BOOST_CHECK_EQUAL((*reopened)->Status().durable_seq, 6U);
        BOOST_CHECK_EQUAL((*reopened)->SegmentCount(), segments);
        BOOST_CHECK(fs::exists(root.path / fs::u8path(SegmentFileName(segments - 1))));
    }
    // A middle segment removed: a gap, refused before any record is trusted.
    BOOST_REQUIRE(fs::remove(root.path / fs::u8path(SegmentFileName(1))));
    RequireRefusal(FileJournal::OpenExisting(root.path, JournalRole::SIGNER, 3), ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_MISSING,
                   "a gap in the segment numbering");
}

BOOST_AUTO_TEST_CASE(a_crash_while_creating_a_segment_leaves_no_short_segment)
{
    // Review of 2026-11-16, lead R7: a segment used to be created under its final name and zero-filled in place,
    // so a crash during the fill left a short `.seg` that blocked every later open (JOURNAL_SEGMENT_SIZE). A
    // segment is now filled and committed under `<name>.tmp` and renamed into place; a leftover `.tmp` is what
    // such a crash leaves, and the reopen removes it.
    const TestDir root{"segment_tmp"};
    {
        auto journal{MustCreate(root.path, JournalRole::COMMIT, 4, SEGMENT_PAGE_BYTES)};
        BOOST_CHECK_EQUAL(journal->Append(RecordType::DECISION, Bytes("d")).value(), 1U);
        BOOST_CHECK_EQUAL(journal->Barrier().value(), 1U);
        BOOST_CHECK_EQUAL(journal->SegmentCount(), 2U); // the spare
    }
    // What a crash in the middle of creating segment 2 leaves: a short temporary file.
    const fs::path leftover{root.path / fs::u8path(SegmentFileName(2) + ".tmp")};
    {
        std::ofstream out{static_cast<const std::filesystem::path&>(leftover), std::ios::binary};
        out << std::string(100, '\0');
    }
    BOOST_REQUIRE(fs::exists(leftover));
    Result<std::unique_ptr<FileJournal>> reopened{FileJournal::OpenExisting(root.path, JournalRole::COMMIT, 4)};
    RequireStorageOk(reopened, "reopen with a leftover temporary segment");
    BOOST_CHECK(!fs::exists(leftover));
    BOOST_CHECK_EQUAL((*reopened)->Status().durable_seq, 1U);
    BOOST_CHECK_EQUAL((*reopened)->SegmentCount(), 2U);
    // Every segment on disk is a full one.
    for (const fs::directory_entry& entry : fs::directory_iterator{root.path}) {
        const std::string name{fs::PathToString(entry.path().filename())};
        if (name.ends_with(".seg")) BOOST_CHECK_EQUAL(fs::file_size(entry.path()), SEGMENT_PAGE_BYTES);
    }
}

BOOST_AUTO_TEST_CASE(a_crash_while_creating_the_first_segment_leaves_a_directory_create_accepts)
{
    // Review workflow of 2026-11-18 over R7: the fix above covered spare segments, not segment 0. A crash during
    // CreateNew's fill leaves `.lock` and `00000000.seg.tmp`. No segment was ever renamed into place, so nothing was
    // committed, but CreateNew refused the directory as not empty (the CLI outbox, which creates when segment 0 is
    // absent, then failed on every later run), and OpenExisting refused it as missing segment 0. CreateNew now takes such
    // a directory as empty, removes the leftovers under the lock, and creates the journal.
    const TestDir root{"first_segment_tmp"};
    const fs::path crashed{root.Sub("crashed")};
    WriteFile(crashed / fs::u8path(".lock"), {});
    WriteFile(crashed / fs::u8path(SegmentFileName(0) + ".tmp"), std::vector<unsigned char>(100, 0));
    RequireRefusal(FileJournal::OpenExisting(crashed, JournalRole::COMMIT, 7), ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_MISSING, "no journal to open");
    {
        Result<std::unique_ptr<FileJournal>> created{FileJournal::CreateNew(crashed, JournalRole::COMMIT, 7, SEGMENT_PAGE_BYTES)};
        RequireStorageOk(created, "create over an interrupted creation");
        BOOST_CHECK_EQUAL((*created)->Append(RecordType::DECISION, Bytes("r")).value(), 1U);
        BOOST_CHECK_EQUAL((*created)->Barrier().value(), 1U);
    }
    BOOST_CHECK(!fs::exists(crashed / fs::u8path(SegmentFileName(0) + ".tmp")));
    Result<std::unique_ptr<FileJournal>> reopened{FileJournal::OpenExisting(crashed, JournalRole::COMMIT, 7)};
    RequireStorageOk(reopened, "reopen");
    BOOST_CHECK_EQUAL((*reopened)->Status().durable_seq, 1U);
    reopened->reset();
    // Only those two kinds of leftover: a journal's segment, or any other file, still makes the directory not empty, and
    // nothing in it is touched.
    const fs::path with_segment{root.Sub("with_segment")};
    WriteFile(with_segment / fs::u8path(".lock"), {});
    WriteFile(with_segment / fs::u8path(SegmentFileName(1) + ".tmp"), std::vector<unsigned char>(100, 0));
    WriteFile(with_segment / fs::u8path(FIRST_SEGMENT_NAME), std::vector<unsigned char>(SEGMENT_PAGE_BYTES, 0));
    RequireRefusal(FileJournal::CreateNew(with_segment, JournalRole::COMMIT, 7, SEGMENT_PAGE_BYTES), ErrorCode::INVALID_INPUT, JOURNAL_DIR_NOT_EMPTY, "a segment");
    BOOST_CHECK(fs::exists(with_segment / fs::u8path(SegmentFileName(1) + ".tmp")));
    const fs::path with_stray{root.Sub("with_stray")};
    WriteFile(with_stray / fs::u8path(".lock"), {});
    WriteFile(with_stray / fs::u8path(SegmentFileName(0) + ".tmp"), std::vector<unsigned char>(100, 0));
    WriteFile(with_stray / fs::u8path("notes.seg.tmp"), Bytes("x"));
    RequireRefusal(FileJournal::CreateNew(with_stray, JournalRole::COMMIT, 7, SEGMENT_PAGE_BYTES), ErrorCode::INVALID_INPUT, JOURNAL_DIR_NOT_EMPTY, "a stray file");
    BOOST_CHECK(fs::exists(with_stray / fs::u8path(SegmentFileName(0) + ".tmp")));
    // While another journal holds the lock, the leftovers are not touched either.
    const fs::path locked{root.Sub("locked")};
    {
        auto holder{MustCreate(locked, JournalRole::COMMIT, 7, SEGMENT_PAGE_BYTES)};
        fs::remove(holder->SegmentPath());
        WriteFile(locked / fs::u8path(SegmentFileName(0) + ".tmp"), std::vector<unsigned char>(100, 0));
        RequireRefusal(FileJournal::CreateNew(locked, JournalRole::COMMIT, 7, SEGMENT_PAGE_BYTES), ErrorCode::BUSY, JOURNAL_DIR_LOCKED, "locked");
        BOOST_CHECK(fs::exists(locked / fs::u8path(SegmentFileName(0) + ".tmp")));
    }
}

BOOST_AUTO_TEST_CASE(a_create_that_fails_keeps_its_lock_file)
{
    // Codex review of 2026-11-18 over 0bb5567d: when CreateSegment failed, CreateNew released the lock and then unlinked
    // `.lock`. A creator that took the lock in between held it on the unlinked inode, and a later opener created and locked
    // a fresh `.lock` beside it: two owners of one journal. Since 0bb5567d CreateNew takes a directory holding `.lock` and
    // segment leftovers, so the lock file can stay. The failure here is a file-size limit below the segment's size.
    const TestDir root{"failed_create"};
    const fs::path dir{root.Sub("journal")};
    rlimit saved{};
    BOOST_REQUIRE_EQUAL(::getrlimit(RLIMIT_FSIZE, &saved), 0);
    void (*const previous)(int){std::signal(SIGXFSZ, SIG_IGN)}; // a write past the limit fails with EFBIG instead
    rlimit small{saved};
    small.rlim_cur = SEGMENT_PAGE_BYTES;
    BOOST_REQUIRE_EQUAL(::setrlimit(RLIMIT_FSIZE, &small), 0);
    Result<std::unique_ptr<FileJournal>> failed{FileJournal::CreateNew(dir, JournalRole::SIGNER, 7, 4 * SEGMENT_PAGE_BYTES)};
    BOOST_REQUIRE_EQUAL(::setrlimit(RLIMIT_FSIZE, &saved), 0);
    std::signal(SIGXFSZ, previous);
    BOOST_REQUIRE(!failed.has_value());
    BOOST_CHECK(failed.error().code == ErrorCode::STORAGE_FATAL);
    BOOST_CHECK(fs::exists(dir / fs::u8path(".lock")));
    BOOST_CHECK(!fs::exists(dir / fs::u8path(FIRST_SEGMENT_NAME)));
    // The directory is still one CreateNew takes.
    Result<std::unique_ptr<FileJournal>> created{FileJournal::CreateNew(dir, JournalRole::SIGNER, 7, 4 * SEGMENT_PAGE_BYTES)};
    RequireStorageOk(created, "create after the failure");
}

BOOST_AUTO_TEST_CASE(a_second_opener_of_a_journal_directory_is_refused)
{
    // Review of 2026-11-16, defect 6: nothing stopped a second process (or a second FileJournal in this one) from
    // opening a live journal directory and signing or deciding beside it. A FileJournal holds an exclusive lock on
    // `<dir>/.lock` for its lifetime; a second opener is refused with BUSY before it reads, repairs or writes anything.
    const TestDir root{"dir_lock"};
    auto journal{MustCreate(root.path, JournalRole::SIGNER, 2, SEGMENT_PAGE_BYTES)};
    BOOST_CHECK(fs::exists(root.path / fs::u8path(".lock")));
    BOOST_CHECK_EQUAL(journal->Append(RecordType::INTENT, Bytes("i")).value(), 1U);
    BOOST_CHECK_EQUAL(journal->Barrier().value(), 1U);
    RequireRefusal(FileJournal::OpenExisting(root.path, JournalRole::SIGNER, 2), ErrorCode::BUSY, JOURNAL_DIR_LOCKED, "a second opener");
    RequireRefusal(FileJournal::CreateNew(root.path, JournalRole::SIGNER, 2, SEGMENT_PAGE_BYTES), ErrorCode::INVALID_INPUT, JOURNAL_DIR_NOT_EMPTY,
                   "a second creator");
    // The holder goes on; the lock lives as long as it does.
    BOOST_CHECK_EQUAL(journal->Append(RecordType::INTENT, Bytes("j")).value(), 2U);
    BOOST_CHECK_EQUAL(journal->Barrier().value(), 2U);
    journal.reset();
    Result<std::unique_ptr<FileJournal>> reopened{FileJournal::OpenExisting(root.path, JournalRole::SIGNER, 2)};
    RequireStorageOk(reopened, "reopen once the holder is gone");
    BOOST_CHECK_EQUAL((*reopened)->Status().durable_seq, 2U);
    RequireRefusal(FileJournal::OpenExisting(root.path, JournalRole::SIGNER, 2), ErrorCode::BUSY, JOURNAL_DIR_LOCKED,
                   "a second opener of the reopened journal");
    // A directory holding only a stale lock file is not a journal.
    const fs::path stale{root.Sub("stale")};
    {
        std::ofstream out{static_cast<const std::filesystem::path&>(stale / fs::u8path(".lock")), std::ios::binary};
    }
    RequireRefusal(FileJournal::OpenExisting(stale, JournalRole::SIGNER, 2), ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_MISSING, "a lock file alone");
}

BOOST_AUTO_TEST_SUITE_END()
