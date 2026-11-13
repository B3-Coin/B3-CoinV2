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
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
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
    BOOST_CHECK_EQUAL(std::distance(fs::directory_iterator{root.path}, fs::directory_iterator{}), 1);
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
    BOOST_CHECK_EQUAL(std::distance(fs::directory_iterator{root.path}, fs::directory_iterator{}), 2);
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

BOOST_AUTO_TEST_CASE(open_existing_is_not_implemented)
{
    const TestDir root{"open_existing"};
    {
        auto journal{MustCreate(root.path, JournalRole::SIGNER, 8)};
        BOOST_CHECK_EQUAL(journal->Append(RecordType::INTENT, Bytes("i")).value(), 1U);
        BOOST_CHECK_EQUAL(journal->Barrier().value(), 1U);
    }
    RequireStub(FileJournal::OpenExisting(root.path, JournalRole::SIGNER, 8), "M4-STO-01");
}

BOOST_AUTO_TEST_SUITE_END()
