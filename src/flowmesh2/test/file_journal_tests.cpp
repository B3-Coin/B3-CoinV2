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
    // Reading back from disk is M4-STO-01.
    RequireStub(journal.ReadAll(), "M4-STO-01");
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

BOOST_AUTO_TEST_CASE(full_segment_is_refused_without_fencing)
{
    const TestDir root{"full_segment"};
    auto journal{MustCreate(root.path, JournalRole::COMMIT, 6, SEGMENT_PAGE_BYTES)};
    // One record that fills the page exactly, then any record is refused (rollover is M4-STO-06).
    const std::vector<unsigned char> fill(SEGMENT_PAGE_BYTES - RECORD_OVERHEAD_BYTES, 0xAB);
    const std::vector<unsigned char> too_big(SEGMENT_PAGE_BYTES - RECORD_OVERHEAD_BYTES + 1, 0xAB);
    RequireStub(journal->Append(RecordType::DECISION, too_big), "M4-STO-06");
    BOOST_CHECK(journal->Status() == (JournalStatus{0, 0, false}));
    BOOST_CHECK_EQUAL(journal->Append(RecordType::DECISION, fill).value(), 1U);
    RequireStub(journal->Append(RecordType::APPLIED, {}), "M4-STO-06");
    BOOST_CHECK(journal->Status() == (JournalStatus{1, 0, false}));
    BOOST_CHECK_EQUAL(journal->Barrier().value(), 1U);
    const std::vector<unsigned char> image{ReadFile(journal->SegmentPath())};
    BOOST_CHECK_EQUAL(image.size(), SEGMENT_PAGE_BYTES);
    const Result<ScanResult> scan{ScanRecords(image, JournalRole::COMMIT, 6)};
    RequireStorageOk(scan, "scan of a full segment");
    BOOST_CHECK_EQUAL(scan->records.size(), 1U);
    BOOST_CHECK_EQUAL(scan->end_offset, SEGMENT_PAGE_BYTES);
    RequireStub(journal->Rollover(), "M4-STO-06");
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
