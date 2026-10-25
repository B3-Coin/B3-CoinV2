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
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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

constexpr uint64_t PAGE{SEGMENT_PAGE_BYTES};
constexpr size_t HEADER{SEGMENT_HEADER_BYTES};

//! The record area of a segment image: everything after its header.
std::span<const unsigned char> Records(const std::vector<unsigned char>& image) { return std::span{image}.subspan(HEADER); }

//! `<dir>/<8-digit index>.seg<suffix>`, the file of segment `index`.
fs::path SegmentFile(const fs::path& dir, unsigned index, std::string_view suffix = "")
{
    char name[16];
    std::snprintf(name, sizeof(name), "%08u", index);
    return dir / fs::u8path(std::string{name} + ".seg" + std::string{suffix});
}

size_t FileCount(const fs::path& dir) { return static_cast<size_t>(std::distance(fs::directory_iterator{dir}, fs::directory_iterator{})); }

bool AllZero(std::span<const unsigned char> bytes)
{
    return std::all_of(bytes.begin(), bytes.end(), [](unsigned char b) { return b == 0; });
}

//! A segment as creation leaves it: its header, then zeros.
bool IsFreshSegment(const std::vector<unsigned char>& image, JournalRole role, uint64_t instance, uint64_t index, uint64_t size)
{
    const auto header{EncodeSegmentHeader(role, instance, index, size)};
    return image.size() == size && std::equal(header.begin(), header.end(), image.begin()) && AllZero(Records(image));
}

//! A payload whose record fills the record area of one page exactly, and one whose record fills half of it.
const std::vector<unsigned char> FILL(PAGE - HEADER - RECORD_OVERHEAD_BYTES, 0xAB);
const std::vector<unsigned char> HALF((PAGE - HEADER) / 2 - RECORD_OVERHEAD_BYTES, 0xCD);

/** One-page segments of COMMIT journal `instance`: segment 0 holds FILL;
 * segment 1 holds "a" and HALF and is zero-padded because the second HALF
 * does not fit; segment 2 holds that HALF. Returns the records read back.
 */
std::vector<JournalRecord> WriteThreeSegments(const fs::path& dir, uint64_t instance)
{
    auto journal{MustCreate(dir, JournalRole::COMMIT, instance, PAGE)};
    BOOST_CHECK_EQUAL(journal->Append(RecordType::DECISION, FILL).value(), 1U);
    BOOST_CHECK_EQUAL(journal->Append(RecordType::APPLIED, Bytes("a")).value(), 2U);
    BOOST_CHECK_EQUAL(journal->Append(RecordType::DECISION, HALF).value(), 3U);
    BOOST_CHECK_EQUAL(journal->Append(RecordType::DECISION, HALF).value(), 4U);
    BOOST_CHECK(journal->Status() == (JournalStatus{4, 0, false}));
    BOOST_CHECK_EQUAL(journal->Barrier().value(), 4U);
    const Result<std::vector<JournalRecord>> records{journal->ReadAll()};
    RequireStorageOk(records, "ReadAll of three segments");
    return records.value();
}

//! Copy segments of `from` into `to`, as {source index, target index} pairs.
void CopySegments(const fs::path& from, const fs::path& to, std::initializer_list<std::pair<unsigned, unsigned>> files)
{
    for (const auto& [source, target] : files) WriteFile(SegmentFile(to, target), ReadFile(SegmentFile(from, source)));
}

//! Commit hook that fails the `g_fail_commit`-th barrier commit (counted in `g_commits`) and passes the others.
std::atomic<int> g_commits{0};
std::atomic<int> g_fail_commit{0};
Result<void> FailNthCommit(int fd)
{
    if (++g_commits == g_fail_commit) return CommitOnClosedFd(fd);
    return JournalCommit(fd);
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
    // The header (M4-STO-06), then zeros.
    BOOST_CHECK(IsFreshSegment(image, JournalRole::COMMIT, 3, 0, segment));
    BOOST_CHECK(std::equal(image.begin(), image.begin() + 8, std::string_view{"B3FM2SEG"}.begin()));
    // Exactly one file in the directory.
    BOOST_CHECK_EQUAL(std::distance(fs::directory_iterator{root.path}, fs::directory_iterator{}), 1);
    const Result<ScanResult> scan{ScanRecords(Records(image), JournalRole::COMMIT, 3)};
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
    BOOST_CHECK(IsFreshSegment(ReadFile(journal.SegmentPath()), JournalRole::SIGNER, 42, 0, SMALL_SEGMENT));
    BOOST_CHECK_EQUAL(journal.Barrier().value(), 2U);
    BOOST_CHECK_EQUAL(journal.Append(RecordType::SIGNED_VOTE, Bytes("vote")).value(), 3U);
    BOOST_CHECK_EQUAL(journal.Barrier().value(), 3U);
    BOOST_CHECK(journal.Status() == (JournalStatus{3, 3, false}));

    const std::vector<unsigned char> image{ReadFile(journal.SegmentPath())};
    BOOST_CHECK_EQUAL(image.size(), SMALL_SEGMENT);
    const Result<ScanResult> scan{ScanRecords(Records(image), JournalRole::SIGNER, 42)};
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
    BOOST_CHECK(std::equal(expected.begin(), expected.end(), image.begin() + HEADER));
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
    // A record larger than a whole segment never fits anywhere and is refused
    // without fencing (M4-STO-06); a record that only misses the rest of a
    // full segment starts the next segment instead.
    const std::vector<unsigned char> fill(SEGMENT_PAGE_BYTES - HEADER - RECORD_OVERHEAD_BYTES, 0xAB);
    const std::vector<unsigned char> too_big(SEGMENT_PAGE_BYTES - HEADER - RECORD_OVERHEAD_BYTES + 1, 0xAB);
    RequireRefusal(journal->Append(RecordType::DECISION, too_big), ErrorCode::OVERSIZE, "JOURNAL_RECORD_SEGMENT", "record above a segment");
    BOOST_CHECK(journal->Status() == (JournalStatus{0, 0, false}));
    BOOST_CHECK_EQUAL(journal->Append(RecordType::DECISION, fill).value(), 1U);
    RequireRefusal(journal->Append(RecordType::DECISION, too_big), ErrorCode::OVERSIZE, "JOURNAL_RECORD_SEGMENT",
                   "record above a segment, after a full one");
    BOOST_CHECK(journal->Status() == (JournalStatus{1, 0, false}));
    BOOST_CHECK_EQUAL(journal->Barrier().value(), 1U);
    BOOST_CHECK_EQUAL(FileCount(root.path), 1U);
    const std::vector<unsigned char> image{ReadFile(journal->SegmentPath())};
    BOOST_CHECK_EQUAL(image.size(), SEGMENT_PAGE_BYTES);
    const Result<ScanResult> scan{ScanRecords(Records(image), JournalRole::COMMIT, 6)};
    RequireStorageOk(scan, "scan of a full segment");
    BOOST_CHECK_EQUAL(scan->records.size(), 1U);
    BOOST_CHECK_EQUAL(scan->end_offset, SEGMENT_PAGE_BYTES - HEADER);
    BOOST_CHECK_EQUAL(journal->Append(RecordType::APPLIED, {}).value(), 2U);
    BOOST_CHECK_EQUAL(journal->Barrier().value(), 2U);
    BOOST_CHECK(journal->Status() == (JournalStatus{2, 2, false}));
    BOOST_CHECK_EQUAL(FileCount(root.path), 2U);
    BOOST_CHECK_EQUAL(journal->ReadAll().value().size(), 2U);
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
        BOOST_CHECK_EQUAL(journal->Append(RecordType::DECISION, FILL).value(), 1U);
        BOOST_CHECK_EQUAL(journal->Barrier().value(), 1U);
    }
    auto full_open{FileJournal::OpenExisting(full, JournalRole::COMMIT, 9)};
    RequireStorageOk(full_open, "full segment reopen");
    BOOST_CHECK((*full_open)->Status() == (JournalStatus{1, 1, false}));
    BOOST_CHECK_EQUAL((*full_open)->Append(RecordType::APPLIED, {}).value(), 2U); // starts segment 1
    BOOST_CHECK_EQUAL((*full_open)->Barrier().value(), 2U);
    BOOST_CHECK(fs::exists(SegmentFile(full, 1)));
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
    bad[HEADER + RECORD_HEADER_BYTES] ^= 1;
    refuse("crc", bad);
    for (const size_t at : {size_t{0}, size_t{8}, size_t{12}, size_t{13}, size_t{16}, size_t{24}, size_t{32}, size_t{40}, HEADER - 1}) {
        bad = image;
        bad[at] ^= 1;
        refuse("header_" + std::to_string(at), bad);
    }
    bad = image;
    bad.back() = 1;
    refuse("nonzero_tail", bad);
    const size_t first_bytes{RECORD_OVERHEAD_BYTES + 3};
    const size_t second_bytes{RECORD_OVERHEAD_BYTES + 3};
    bad.assign(image.size(), 0);
    std::copy_n(image.begin(), HEADER + first_bytes, bad.begin());
    // Remove the second record while keeping the third's original valid CRC.
    std::copy_n(image.begin() + HEADER + first_bytes + second_bytes, RECORD_OVERHEAD_BYTES + 5, bad.begin() + HEADER + first_bytes);
    refuse("gap", bad);
    bad.assign(image.size(), 0);
    std::copy_n(image.begin(), HEADER + first_bytes + RECORD_HEADER_BYTES + 1, bad.begin());
    refuse("torn_last_record", bad);
    refuse("short_segment", std::vector<unsigned char>(SEGMENT_PAGE_BYTES - 1, 0));
    // The header names the journal (M4-STO-06): another instance or role is refused before any record is read.
    RequireRefusal(FileJournal::OpenExisting(original, JournalRole::SIGNER, 24), ErrorCode::VERIFY_FAILED, JOURNAL_SEGMENT_HEADER, "wrong instance");
    RequireRefusal(FileJournal::OpenExisting(original, JournalRole::COMMIT, 23), ErrorCode::ORDER_VIOLATION, JOURNAL_SEGMENT_HEADER, "wrong role");
    BOOST_CHECK(ReadFile(segment) == image);
    // A gap in the segment numbering (a prepared next segment, 00000001.seg,
    // is valid since M4-STO-06).
    const fs::path extra{root.Sub("extra")};
    WriteFile(extra / fs::u8path(FIRST_SEGMENT_NAME), image);
    WriteFile(extra / fs::u8path("00000002.seg"), std::vector<unsigned char>(SMALL_SEGMENT, 0));
    RequireRefusal(FileJournal::OpenExisting(extra, JournalRole::SIGNER, 23), ErrorCode::INVALID_INPUT,
                   JOURNAL_SEGMENT_LAYOUT, "a gap in the segment numbering");
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
    std::vector<unsigned char> zeroed(SMALL_SEGMENT, 0);
    const auto header{EncodeSegmentHeader(JournalRole::COMMIT, 31, 0, SMALL_SEGMENT)};
    std::copy(header.begin(), header.end(), zeroed.begin());
    WriteFile((*reopened)->SegmentPath(), zeroed);
    RequireRefusal((*reopened)->ReadAll(), ErrorCode::VERIFY_FAILED, RECORD_TAIL, "zeroed durable prefix");
    WriteFile((*reopened)->SegmentPath(), std::vector<unsigned char>(SMALL_SEGMENT, 0));
    RequireRefusal((*reopened)->ReadAll(), ErrorCode::VERIFY_FAILED, JOURNAL_SEGMENT_HEADER, "zeroed header");
}

BOOST_AUTO_TEST_CASE(records_roll_into_the_next_segment_and_reopen_across_segments)
{
    // M4-STO-06. A record that does not fit in the rest of its segment starts
    // the next one; the rest of the old segment stays zero; the record chain
    // runs across segments, so no segment after the first is valid alone.
    const TestDir root{"rollover"};
    const fs::path dir{root.Sub("j")};
    const std::vector<JournalRecord> written{WriteThreeSegments(dir, 41)};
    BOOST_REQUIRE_EQUAL(written.size(), 4U);
    for (size_t i{0}; i < written.size(); ++i) BOOST_CHECK_EQUAL(written[i].seq, i + 1);
    BOOST_CHECK(written[0].payload == FILL);
    BOOST_CHECK(written[1].payload == Bytes("a"));
    BOOST_CHECK(written[2].payload == HALF && written[3].payload == HALF);
    BOOST_CHECK_EQUAL(FileCount(dir), 3U);
    for (unsigned i{0}; i < 3; ++i) BOOST_CHECK_EQUAL(fs::file_size(SegmentFile(dir, i)), PAGE);
    const std::vector<unsigned char> middle{ReadFile(SegmentFile(dir, 1))};
    const auto header{EncodeSegmentHeader(JournalRole::COMMIT, 41, 1, PAGE)};
    BOOST_CHECK(std::equal(header.begin(), header.end(), middle.begin()));
    const size_t used{HEADER + 2 * RECORD_OVERHEAD_BYTES + 1 + HALF.size()};
    BOOST_CHECK(AllZero(std::span{middle}.subspan(used)));
    RequireRefusal(ScanRecords(Records(middle), JournalRole::COMMIT, 41), ErrorCode::VERIFY_FAILED, RECORD_CHAIN, "segment 1 alone");

    auto opened{FileJournal::OpenExisting(dir, JournalRole::COMMIT, 41)};
    RequireStorageOk(opened, "reopen three segments");
    BOOST_CHECK((*opened)->Status() == (JournalStatus{4, 4, false}));
    BOOST_CHECK((*opened)->ReadAll().value() == written);
    RequireRefusal(FileJournal::OpenExisting(dir, JournalRole::COMMIT, 41), ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_LOCKED,
                   "concurrent open of a multi-segment journal");
    BOOST_CHECK_EQUAL((*opened)->Append(RecordType::APPLIED, Bytes("b")).value(), 5U); // fits after HALF in segment 2
    BOOST_CHECK_EQUAL((*opened)->Barrier().value(), 5U);
    BOOST_CHECK_EQUAL(FileCount(dir), 3U);
    const std::vector<JournalRecord> five{(*opened)->ReadAll().value()};
    BOOST_REQUIRE_EQUAL(five.size(), 5U);
    BOOST_CHECK(std::equal(written.begin(), written.end(), five.begin()));
    opened->reset();
    auto again{FileJournal::OpenExisting(dir, JournalRole::COMMIT, 41)};
    RequireStorageOk(again, "second reopen");
    BOOST_CHECK((*again)->Status() == (JournalStatus{5, 5, false}));
    BOOST_CHECK((*again)->ReadAll().value() == five);
}

BOOST_AUTO_TEST_CASE(rollover_prepares_the_next_segment_before_a_barrier_needs_it)
{
    const TestDir root{"rollover_prepare"};
    const fs::path dir{root.Sub("j")};
    {
        auto journal{MustCreate(dir, JournalRole::COMMIT, 43, PAGE)};
        RequireStorageOk(journal->Rollover(), "Rollover");
        BOOST_CHECK_EQUAL(FileCount(dir), 2U);
        BOOST_CHECK(IsFreshSegment(ReadFile(SegmentFile(dir, 1)), JournalRole::COMMIT, 43, 1, PAGE));
        // At most one segment ahead of the one the next record goes to.
        RequireStorageOk(journal->Rollover(), "second Rollover");
        BOOST_CHECK_EQUAL(FileCount(dir), 2U);
        BOOST_CHECK(journal->Status() == (JournalStatus{0, 0, false}));
        BOOST_CHECK_EQUAL(journal->Append(RecordType::DECISION, FILL).value(), 1U);
        BOOST_CHECK_EQUAL(journal->Barrier().value(), 1U);
    }
    // A prepared, still empty segment reopens, and the next record uses it.
    {
        auto opened{FileJournal::OpenExisting(dir, JournalRole::COMMIT, 43)};
        RequireStorageOk(opened, "reopen with a prepared segment");
        BOOST_CHECK((*opened)->Status() == (JournalStatus{1, 1, false}));
        BOOST_CHECK_EQUAL((*opened)->Append(RecordType::APPLIED, Bytes("a")).value(), 2U);
        BOOST_CHECK_EQUAL((*opened)->Barrier().value(), 2U);
        BOOST_CHECK_EQUAL(FileCount(dir), 2U);
        BOOST_CHECK_EQUAL((*opened)->ReadAll().value().size(), 2U);
        RequireStorageOk((*opened)->Rollover(), "Rollover past segment 1");
        BOOST_CHECK_EQUAL(FileCount(dir), 3U);
    }
    // A fatal journal prepares nothing.
    const fs::path failed{root.Sub("failed")};
    auto journal{MustCreate(failed, JournalRole::COMMIT, 44, PAGE, &CommitOnClosedFd)};
    BOOST_CHECK_EQUAL(journal->Append(RecordType::APPLIED, Bytes("x")).value(), 1U);
    RequireCode(journal->Barrier(), ErrorCode::STORAGE_FATAL, "failed barrier");
    RequireRefusal(journal->Rollover(), ErrorCode::FENCED, JOURNAL_FENCED, "Rollover of a fatal journal");
    BOOST_CHECK_EQUAL(FileCount(failed), 1U);
}

BOOST_AUTO_TEST_CASE(reopen_checks_the_segment_sequence_without_repair)
{
    const TestDir root{"rollover_layouts"};
    const fs::path valid{root.Sub("valid")};
    const std::vector<JournalRecord> written{WriteThreeSegments(valid, 45)};
    auto refuse = [&](std::string_view name, std::initializer_list<std::pair<unsigned, unsigned>> files, std::string_view detail) {
        const fs::path dir{root.Sub(name)};
        CopySegments(valid, dir, files);
        std::vector<std::vector<unsigned char>> before;
        for (const auto& [source, target] : files) before.push_back(ReadFile(SegmentFile(dir, target)));
        const auto result{FileJournal::OpenExisting(dir, JournalRole::COMMIT, 45)};
        BOOST_CHECK_MESSAGE(!result.has_value() && result.error().detail == detail,
                            std::string{name} + ": want " + std::string{detail} + ", got " +
                                (result.has_value() ? std::string{"success"} : std::string{result.error().detail}));
        size_t i{0};
        for (const auto& [source, target] : files) BOOST_CHECK(ReadFile(SegmentFile(dir, target)) == before[i++]);
        BOOST_CHECK_EQUAL(FileCount(dir), files.size());
    };
    refuse("gap", {{0, 0}, {2, 2}}, JOURNAL_SEGMENT_LAYOUT);
    refuse("no_first", {{1, 1}, {2, 2}}, JOURNAL_SEGMENT_LAYOUT);
    // A segment under another index: its header names its own.
    refuse("swapped", {{0, 0}, {2, 1}, {1, 2}}, JOURNAL_SEGMENT_HEADER);
    refuse("dropped_middle", {{0, 0}, {2, 1}}, JOURNAL_SEGMENT_HEADER);
    {
        // A segment of another history of the same journal: the header fits, the chain does not.
        const fs::path other{root.Sub("other_history")};
        {
            auto journal{MustCreate(other, JournalRole::COMMIT, 45, PAGE)};
            BOOST_CHECK_EQUAL(journal->Append(RecordType::DECISION, std::vector<unsigned char>(FILL.size(), 0xEE)).value(), 1U);
            BOOST_CHECK_EQUAL(journal->Append(RecordType::APPLIED, Bytes("a")).value(), 2U);
            BOOST_CHECK_EQUAL(journal->Barrier().value(), 2U);
        }
        const fs::path dir{root.Sub("foreign")};
        CopySegments(valid, dir, {{0, 0}});
        CopySegments(other, dir, {{1, 1}});
        RequireRefusal(FileJournal::OpenExisting(dir, JournalRole::COMMIT, 45), ErrorCode::VERIFY_FAILED, RECORD_CHAIN,
                       "a segment of another history");
    }
    // A prepared, empty segment 1 of this journal.
    const fs::path prepared{root.Sub("prepared")};
    {
        auto journal{MustCreate(prepared, JournalRole::COMMIT, 45, PAGE)};
        RequireStorageOk(journal->Rollover(), "Rollover");
    }
    {
        const fs::path dir{root.Sub("size")};
        CopySegments(valid, dir, {{0, 0}, {1, 1}});
        std::vector<unsigned char> longer{ReadFile(SegmentFile(valid, 2))};
        longer.resize(2 * PAGE, 0);
        WriteFile(SegmentFile(dir, 2), longer);
        RequireRefusal(FileJournal::OpenExisting(dir, JournalRole::COMMIT, 45), ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_SIZE,
                       "segments of different sizes");
    }
    {
        // An empty segment before one with records: the writer starts a
        // segment only for a record that does not fit, and every record fits an empty one.
        const fs::path dir{root.Sub("empty_middle")};
        CopySegments(valid, dir, {{0, 0}, {2, 2}});
        CopySegments(prepared, dir, {{1, 1}});
        RequireRefusal(FileJournal::OpenExisting(dir, JournalRole::COMMIT, 45), ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_LAYOUT,
                       "empty segment before records");
    }
    {
        // Two records with a valid chain, the second moved to segment 1 although it fits in segment 0.
        const fs::path one{root.Sub("one_segment")};
        {
            auto journal{MustCreate(one, JournalRole::COMMIT, 46, PAGE)};
            BOOST_CHECK_EQUAL(journal->Append(RecordType::APPLIED, Bytes("a")).value(), 1U);
            BOOST_CHECK_EQUAL(journal->Append(RecordType::APPLIED, Bytes("b")).value(), 2U);
            BOOST_CHECK_EQUAL(journal->Barrier().value(), 2U);
            RequireStorageOk(journal->Rollover(), "Rollover"); // the header of segment 1
        }
        const std::vector<unsigned char> image{ReadFile(SegmentFile(one, 0))};
        const size_t first{RECORD_OVERHEAD_BYTES + 1};
        std::vector<unsigned char> head(PAGE, 0), tail{ReadFile(SegmentFile(one, 1))};
        std::copy_n(image.begin(), HEADER + first, head.begin());
        std::copy_n(image.begin() + HEADER + first, first, tail.begin() + HEADER);
        const fs::path dir{root.Sub("needless_padding")};
        WriteFile(SegmentFile(dir, 0), head);
        WriteFile(SegmentFile(dir, 1), tail);
        RequireRefusal(FileJournal::OpenExisting(dir, JournalRole::COMMIT, 46), ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_LAYOUT,
                       "a record that fit the previous segment");
    }
    {
        const fs::path dir{root.Sub("stray")};
        CopySegments(valid, dir, {{0, 0}, {1, 1}, {2, 2}});
        WriteFile(dir / fs::u8path("notes.txt"), Bytes("x"));
        RequireRefusal(FileJournal::OpenExisting(dir, JournalRole::COMMIT, 45), ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_LAYOUT, "stray file");
        fs::remove(dir / fs::u8path("notes.txt"));
        WriteFile(SegmentFile(dir, 5, ".tmp"), Bytes("partial"));
        RequireRefusal(FileJournal::OpenExisting(dir, JournalRole::COMMIT, 45), ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_LAYOUT,
                       "a temporary segment that is not the next one");
    }
    auto opened{FileJournal::OpenExisting(valid, JournalRole::COMMIT, 45)};
    RequireStorageOk(opened, "the valid copy");
    BOOST_CHECK((*opened)->ReadAll().value() == written);
}

BOOST_AUTO_TEST_CASE(a_crash_at_each_rollover_step_reopens_with_every_acknowledged_record)
{
    const TestDir root{"rollover_crash"};
    const fs::path base{root.Sub("base")};
    std::vector<JournalRecord> full;
    {
        auto journal{MustCreate(base, JournalRole::COMMIT, 47, PAGE)};
        BOOST_CHECK_EQUAL(journal->Append(RecordType::DECISION, FILL).value(), 1U);
        BOOST_CHECK_EQUAL(journal->Barrier().value(), 1U);
        full = journal->ReadAll().value();
        RequireStorageOk(journal->Rollover(), "Rollover"); // segment 1 as its creation leaves it
    }
    // 1. Crash while the next segment was being zero-filled: a partial
    //    temporary file. It is not part of the journal; the next rollover
    //    writes it again from the start and renames it into place.
    {
        const fs::path dir{root.Sub("partial_fill")};
        CopySegments(base, dir, {{0, 0}});
        WriteFile(SegmentFile(dir, 1, ".tmp"), std::vector<unsigned char>(100, 0x77));
        auto opened{FileJournal::OpenExisting(dir, JournalRole::COMMIT, 47)};
        RequireStorageOk(opened, "reopen beside a partial temporary segment");
        BOOST_CHECK((*opened)->Status() == (JournalStatus{1, 1, false}));
        BOOST_CHECK((*opened)->ReadAll().value() == full);
        BOOST_CHECK_EQUAL((*opened)->Append(RecordType::APPLIED, Bytes("a")).value(), 2U);
        BOOST_CHECK_EQUAL((*opened)->Barrier().value(), 2U);
        BOOST_CHECK(!fs::exists(SegmentFile(dir, 1, ".tmp")));
        BOOST_CHECK_EQUAL(fs::file_size(SegmentFile(dir, 1)), PAGE);
        BOOST_CHECK_EQUAL((*opened)->ReadAll().value().size(), 2U);
    }
    // 2. Crash after the new segment was in place, before the barrier wrote
    //    its record: that record was never acknowledged and is not there.
    //    (A segment in place always has its header: it is written and
    //    committed before the rename, so a zeroed one is damage.)
    {
        const fs::path zeroed{root.Sub("headerless_new_segment")};
        CopySegments(base, zeroed, {{0, 0}});
        WriteFile(SegmentFile(zeroed, 1), std::vector<unsigned char>(PAGE, 0));
        RequireRefusal(FileJournal::OpenExisting(zeroed, JournalRole::COMMIT, 47), ErrorCode::VERIFY_FAILED, JOURNAL_SEGMENT_HEADER,
                       "a segment without its header");
        const fs::path dir{root.Sub("empty_new_segment")};
        CopySegments(base, dir, {{0, 0}, {1, 1}});
        auto opened{FileJournal::OpenExisting(dir, JournalRole::COMMIT, 47)};
        RequireStorageOk(opened, "reopen with an empty new segment");
        BOOST_CHECK((*opened)->Status() == (JournalStatus{1, 1, false}));
        BOOST_CHECK_EQUAL((*opened)->Append(RecordType::APPLIED, Bytes("a")).value(), 2U);
        BOOST_CHECK_EQUAL((*opened)->Barrier().value(), 2U);
        BOOST_CHECK_EQUAL(FileCount(dir), 2U);
    }
    // 3. The commit of the new segment fails: the journal is fatal and the
    //    record unacknowledged. Its bytes were written, so a reopen keeps the
    //    complete record after a new strict commit (the single-segment rule).
    {
        const fs::path dir{root.Sub("failed_commit")};
        g_commits = 0;
        g_fail_commit = 2; // this barrier commits the old segment, then the new one
        {
            auto journal{MustCreate(dir, JournalRole::COMMIT, 48, PAGE, &FailNthCommit)};
            BOOST_CHECK_EQUAL(journal->Append(RecordType::DECISION, FILL).value(), 1U);
            BOOST_CHECK_EQUAL(journal->Append(RecordType::APPLIED, Bytes("a")).value(), 2U);
            RequireCode(journal->Barrier(), ErrorCode::STORAGE_FATAL, "barrier into the new segment");
            BOOST_CHECK(journal->Status() == (JournalStatus{2, 0, true}));
            BOOST_CHECK(journal->ReadAll().value().empty());
        }
        g_fail_commit = 0;
        auto opened{FileJournal::OpenExisting(dir, JournalRole::COMMIT, 48)};
        RequireStorageOk(opened, "reopen after the failed commit");
        BOOST_CHECK((*opened)->Status() == (JournalStatus{2, 2, false}));
    }
    // 4. Crash while the first record of the new segment was being written: a
    //    torn record. Still a refusal, as in one segment (torn-tail recovery is M4-STO-01).
    {
        const fs::path dir{root.Sub("torn_new_segment")};
        {
            auto journal{MustCreate(dir, JournalRole::COMMIT, 49, PAGE)};
            BOOST_CHECK_EQUAL(journal->Append(RecordType::DECISION, FILL).value(), 1U);
            BOOST_CHECK_EQUAL(journal->Append(RecordType::APPLIED, Bytes("abc")).value(), 2U);
            BOOST_CHECK_EQUAL(journal->Barrier().value(), 2U);
        }
        std::vector<unsigned char> torn{ReadFile(SegmentFile(dir, 1))};
        std::fill(torn.begin() + HEADER + RECORD_HEADER_BYTES + 1, torn.end(), 0);
        WriteFile(SegmentFile(dir, 1), torn);
        BOOST_CHECK(!FileJournal::OpenExisting(dir, JournalRole::COMMIT, 49).has_value());
        BOOST_CHECK(ReadFile(SegmentFile(dir, 1)) == torn);
    }
}

BOOST_AUTO_TEST_SUITE_END()
