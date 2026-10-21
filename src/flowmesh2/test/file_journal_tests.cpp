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
    const auto read_back{journal.ReadAll()};
    RequireStorageOk(read_back, "read durable records");
    BOOST_CHECK(*read_back == scan->records);
    BOOST_CHECK_EQUAL(journal.Append(RecordType::INTENT, Bytes("not-yet-durable")).value(), 4U);
    BOOST_CHECK(journal.ReadAll().value() == scan->records);
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

BOOST_AUTO_TEST_CASE(reopen_preserves_history_and_continues_sequence)
{
    const TestDir root{"open_existing"};
    std::vector<JournalRecord> first;
    {
        auto journal{MustCreate(root.path, JournalRole::SIGNER, 8)};
        BOOST_CHECK_EQUAL(journal->Append(RecordType::INTENT, Bytes("i")).value(), 1U);
        BOOST_CHECK_EQUAL(journal->Barrier().value(), 1U);
        first = journal->ReadAll().value();
        BOOST_CHECK_EQUAL(journal->Append(RecordType::SIGNED_VOTE, Bytes("lost-unbarriered")).value(), 2U);
        BOOST_CHECK(journal->ReadAll().value() == first);
        RequireRefusal(FileJournal::OpenExisting(root.path, JournalRole::SIGNER, 8), ErrorCode::STORAGE_FATAL,
                       JOURNAL_SEGMENT_LOCKED, "concurrent open");
    }
    auto opened{FileJournal::OpenExisting(root.path, JournalRole::SIGNER, 8)};
    RequireStorageOk(opened, "reopen");
    BOOST_CHECK((*opened)->Status() == (JournalStatus{1, 1, false}));
    BOOST_CHECK((*opened)->ReadAll().value() == first);
    BOOST_CHECK_EQUAL((*opened)->Append(RecordType::SIGNED_VOTE, Bytes("v")).value(), 2U);
    BOOST_CHECK((*opened)->ReadAll().value() == first);
    BOOST_CHECK_EQUAL((*opened)->Barrier().value(), 2U);
    auto second{(*opened)->ReadAll().value()};
    BOOST_REQUIRE_EQUAL(second.size(), 2U);
    BOOST_CHECK(second[0] == first[0]);
    BOOST_CHECK(second[1].payload == Bytes("v"));
    opened->reset();
    auto again{FileJournal::OpenExisting(root.path, JournalRole::SIGNER, 8)};
    RequireStorageOk(again, "second reopen");
    BOOST_CHECK((*again)->Status() == (JournalStatus{2, 2, false}));
    BOOST_CHECK((*again)->ReadAll().value() == second);
    BOOST_CHECK_EQUAL((*again)->Append(RecordType::INTENT, Bytes("next")).value(), 3U);
    BOOST_CHECK_EQUAL((*again)->Barrier().value(), 3U);
    // Reopening a completely full segment restores the end boundary too.
    const fs::path full{root.Sub("full")};
    {
        auto journal{MustCreate(full, JournalRole::COMMIT, 9, SEGMENT_PAGE_BYTES)};
        BOOST_CHECK_EQUAL(journal->Append(RecordType::DECISION, std::vector<unsigned char>(SEGMENT_PAGE_BYTES - RECORD_OVERHEAD_BYTES, 0xAB)).value(), 1U);
        BOOST_CHECK_EQUAL(journal->Barrier().value(), 1U);
    }
    auto full_open{FileJournal::OpenExisting(full, JournalRole::COMMIT, 9)};
    RequireStorageOk(full_open, "full segment reopen");
    RequireStub((*full_open)->Append(RecordType::APPLIED, {}), "M4-STO-06");
    BOOST_CHECK((*full_open)->Status() == (JournalStatus{1, 1, false}));
}

BOOST_AUTO_TEST_CASE(reopen_refuses_corruption_gaps_and_torn_tails_without_repair)
{
    const TestDir root{"corrupt_reopen"};
    const fs::path original{root.Sub("original")};
    fs::path segment;
    {
        auto journal{MustCreate(original, JournalRole::SIGNER, 23)};
        segment = journal->SegmentPath();
        BOOST_CHECK_EQUAL(journal->Append(RecordType::INTENT, Bytes("one")).value(), 1U);
        BOOST_CHECK_EQUAL(journal->Append(RecordType::SIGNED_VOTE, Bytes("two")).value(), 2U);
        BOOST_CHECK_EQUAL(journal->Append(RecordType::INTENT, Bytes("three")).value(), 3U);
        BOOST_CHECK_EQUAL(journal->Barrier().value(), 3U);
    }
    const auto image{ReadFile(segment)};
    auto refuse = [&](std::string_view name, const std::vector<unsigned char>& bad) {
        const fs::path dir{root.Sub(name)};
        const fs::path file{dir / fs::u8path(FIRST_SEGMENT_NAME)};
        WriteFile(file, bad);
        const auto result{FileJournal::OpenExisting(dir, JournalRole::SIGNER, 23)};
        BOOST_CHECK(!result.has_value());
        BOOST_CHECK(ReadFile(file) == bad);
        BOOST_CHECK_EQUAL(std::distance(fs::directory_iterator{dir}, fs::directory_iterator{}), 1);
    };
    auto bad{image};
    bad[RECORD_HEADER_BYTES] ^= 1;
    refuse("crc", bad);
    bad = image;
    bad.back() = 1;
    refuse("nonzero_tail", bad);
    const size_t first_bytes{RECORD_OVERHEAD_BYTES + 3};
    const size_t second_bytes{RECORD_OVERHEAD_BYTES + 3};
    bad.assign(image.size(), 0);
    std::copy_n(image.begin(), first_bytes, bad.begin());
    // Remove the second record while keeping the third's original valid CRC.
    std::copy_n(image.begin() + first_bytes + second_bytes, RECORD_OVERHEAD_BYTES + 5, bad.begin() + first_bytes);
    refuse("gap", bad);
    bad.assign(image.size(), 0);
    std::copy_n(image.begin(), first_bytes + RECORD_HEADER_BYTES + 1, bad.begin());
    refuse("torn_last_record", bad);
    refuse("short_segment", std::vector<unsigned char>(SEGMENT_PAGE_BYTES - 1, 0));
    RequireRefusal(FileJournal::OpenExisting(original, JournalRole::SIGNER, 24), ErrorCode::VERIFY_FAILED, RECORD_CHAIN, "wrong instance");
    RequireRefusal(FileJournal::OpenExisting(original, JournalRole::COMMIT, 23), ErrorCode::ORDER_VIOLATION, RECORD_ROLE, "wrong role");
    BOOST_CHECK(ReadFile(segment) == image);
    const fs::path extra{root.Sub("extra")};
    WriteFile(extra / fs::u8path(FIRST_SEGMENT_NAME), image);
    WriteFile(extra / fs::u8path("00000001.seg"), std::vector<unsigned char>(SEGMENT_PAGE_BYTES, 0));
    RequireRefusal(FileJournal::OpenExisting(extra, JournalRole::SIGNER, 23), ErrorCode::INVALID_INPUT,
                   JOURNAL_SEGMENT_LAYOUT, "unsupported extra segment");
    const fs::path empty{root.Sub("empty")};
    RequireRefusal(FileJournal::OpenExisting(empty, JournalRole::SIGNER, 23), ErrorCode::INVALID_INPUT,
                   JOURNAL_SEGMENT_LAYOUT, "missing segment");
}

BOOST_AUTO_TEST_CASE(read_excludes_failed_barrier_and_detects_lost_durable_prefix)
{
    const TestDir root{"durable_prefix"};
    const fs::path failed{root.Sub("failed")};
    {
        auto journal{MustCreate(failed, JournalRole::COMMIT, 31, SMALL_SEGMENT, &CommitOnClosedFd)};
        BOOST_CHECK_EQUAL(journal->Append(RecordType::DECISION, Bytes("written-but-unacknowledged")).value(), 1U);
        RequireCode(journal->Barrier(), ErrorCode::STORAGE_FATAL, "failed barrier");
        const auto records{journal->ReadAll()};
        RequireStorageOk(records, "last durable prefix after failure");
        BOOST_CHECK(records->empty());
    }
    // A complete record from the failed acknowledgement is reconciled by a
    // new real strict commit on reopen, never silently overwritten.
    auto reopened{FileJournal::OpenExisting(failed, JournalRole::COMMIT, 31)};
    RequireStorageOk(reopened, "reconcile complete unacknowledged record");
    BOOST_CHECK((*reopened)->Status() == (JournalStatus{1, 1, false}));
    BOOST_CHECK_EQUAL((*reopened)->ReadAll().value().size(), 1U);
    // Simulate external damage to an acknowledged byte range. Reading must
    // refuse rather than accepting a shorter all-zero history.
    WriteFile((*reopened)->SegmentPath(), std::vector<unsigned char>(SMALL_SEGMENT, 0));
    RequireRefusal((*reopened)->ReadAll(), ErrorCode::VERIFY_FAILED, RECORD_TAIL, "zeroed durable prefix");
}

BOOST_AUTO_TEST_SUITE_END()
