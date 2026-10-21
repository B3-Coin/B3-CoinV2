// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/storage/file_journal.h>

#include <flowmesh2/storage/journal_commit.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <system_error>
#include <utility>

#ifdef WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/locking.h>
#include <windows.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace flowmesh2::storage {

namespace {

//! Write all of `bytes` at `offset`; STORAGE_FATAL on any error or zero-length write.
Result<void> WriteAllAt(int fd, std::span<const unsigned char> bytes, uint64_t offset)
{
#ifdef WIN32
    const HANDLE handle{reinterpret_cast<HANDLE>(_get_osfhandle(fd))};
    while (!bytes.empty()) {
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(offset);
        at.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD written{0};
        const DWORD chunk{static_cast<DWORD>(std::min<size_t>(bytes.size(), 1U << 30))};
        if (!WriteFile(handle, bytes.data(), chunk, &written, &at) || written == 0) {
            return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_WRITE};
        }
        bytes = bytes.subspan(written);
        offset += written;
    }
#else
    while (!bytes.empty()) {
        if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
            return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_WRITE};
        }
        const ssize_t written{pwrite(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset))};
        if (written <= 0) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_WRITE};
        bytes = bytes.subspan(static_cast<size_t>(written));
        offset += static_cast<uint64_t>(written);
    }
#endif
    return Ok();
}

int OpenNewSegment(const fs::path& path)
{
#ifdef WIN32
    return _wopen(path.c_str(), _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    return open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
#endif
}

void CloseFd(int fd)
{
#ifdef WIN32
    _close(fd);
#else
    close(fd);
#endif
}

//! Every journal instance holds this lock until descriptor destruction.
bool LockSegment(int fd)
{
#ifdef WIN32
    return _lseeki64(fd, 0, SEEK_SET) == 0 && _locking(fd, _LK_NBLCK, 1) == 0;
#else
    return flock(fd, LOCK_EX | LOCK_NB) == 0;
#endif
}

class SegmentFd
{
public:
    explicit SegmentFd(int fd) : m_fd{fd} {}
    ~SegmentFd() { if (m_fd >= 0) CloseFd(m_fd); }
    SegmentFd(const SegmentFd&) = delete;
    SegmentFd& operator=(const SegmentFd&) = delete;
    void Release() { m_fd = -1; }
private:
    int m_fd;
};

Result<uint64_t> SegmentSize(int fd)
{
#ifdef WIN32
    struct _stat64 info;
    if (_fstat64(fd, &info) != 0 || (info.st_mode & _S_IFREG) == 0 || info.st_size < 0) {
#else
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
#endif
        return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_READ};
    }
    return Ok(static_cast<uint64_t>(info.st_size));
}

//! Read an immutable prefix. POSIX pread does not share the writer's offset.
Result<std::vector<unsigned char>> ReadPrefix(int fd, uint64_t bytes)
{
    std::vector<unsigned char> image(static_cast<size_t>(bytes));
    size_t offset{0};
    while (offset < image.size()) {
#ifdef WIN32
        const HANDLE handle{reinterpret_cast<HANDLE>(_get_osfhandle(fd))};
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(offset);
        at.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(offset) >> 32);
        DWORD count{0};
        const DWORD chunk{static_cast<DWORD>(std::min<size_t>(image.size() - offset, 1U << 30))};
        if (!ReadFile(handle, image.data() + offset, chunk, &count, &at)) {
            return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_READ};
        }
#else
        const ssize_t count{pread(fd, image.data() + offset, image.size() - offset, static_cast<off_t>(offset))};
        if (count < 0 && errno == EINTR) continue;
#endif
        if (count <= 0) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_READ};
        offset += static_cast<size_t>(count);
    }
    return Ok(std::move(image));
}

} // namespace

FileJournal::FileJournal(JournalRole role, uint64_t instance_id, BarrierObserver* observer, fs::path segment_path, int fd,
                         uint64_t segment_bytes, SegmentCommitHook commit, const ScanResult* recovered)
    : Journal(ImplKey{}, role, instance_id, observer, recovered),
      m_segment_path{std::move(segment_path)},
      m_fd{fd},
      m_segment_bytes{segment_bytes},
      m_commit{commit}
{
}

FileJournal::~FileJournal() { CloseFd(m_fd); }

Result<std::unique_ptr<FileJournal>> FileJournal::CreateNew(const fs::path& empty_dir, JournalRole role, uint64_t instance_id,
                                                            uint64_t segment_bytes, BarrierObserver* observer)
{
    return CreateNewWithCommitHook(empty_dir, role, instance_id, segment_bytes, &JournalCommit, observer);
}

Result<std::unique_ptr<FileJournal>> FileJournal::CreateNewWithCommitHook(const fs::path& empty_dir, JournalRole role,
                                                                          uint64_t instance_id, uint64_t segment_bytes,
                                                                          SegmentCommitHook commit, BarrierObserver* observer)
{
    if (!IsKnown(role)) return Error{ErrorCode::INVALID_INPUT, "unknown journal role"};
    if (commit == nullptr) return Error{ErrorCode::INVALID_INPUT, "missing segment commit"};
    if (segment_bytes < SEGMENT_PAGE_BYTES || segment_bytes > MAX_SEGMENT_BYTES || segment_bytes % SEGMENT_PAGE_BYTES != 0) {
        return Error{ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_SIZE};
    }
    std::error_code ec;
    if (!fs::is_directory(empty_dir, ec) || ec) return Error{ErrorCode::INVALID_INPUT, JOURNAL_DIR_MISSING};
    const fs::directory_iterator first{empty_dir, ec};
    if (ec) return Error{ErrorCode::INVALID_INPUT, JOURNAL_DIR_MISSING};
    if (first != fs::directory_iterator{}) return Error{ErrorCode::INVALID_INPUT, JOURNAL_DIR_NOT_EMPTY};

    fs::path segment_path{empty_dir / fs::u8path(FIRST_SEGMENT_NAME)};
    const int fd{OpenNewSegment(segment_path)};
    if (fd < 0) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_CREATE};
    SegmentFd owned{fd};
    if (!LockSegment(fd)) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_LOCKED};

    // Explicit zero pages for the whole segment (plan section 3.7, S-9): the
    // blocks are allocated now, not on the first barrier, and the unused tail
    // reads back as zeros.
    static constexpr std::array<unsigned char, 64 * 1024> ZEROS{};
    for (uint64_t offset{0}; offset < segment_bytes; offset += ZEROS.size()) {
        const size_t chunk{static_cast<size_t>(std::min<uint64_t>(ZEROS.size(), segment_bytes - offset))};
        if (const Result<void> written{WriteAllAt(fd, std::span{ZEROS}.first(chunk), offset)}; !written.has_value()) {
            return written.error();
        }
    }
    // Creation always uses the real strict commit; a hook only replaces the barriers' commit.
    if (const Result<void> committed{JournalCommit(fd)}; !committed.has_value()) {
        return committed.error();
    }
    if (const Result<void> dir_committed{CommitDirectory(empty_dir)}; !dir_committed.has_value()) {
        return dir_committed.error();
    }
    auto journal{std::unique_ptr<FileJournal>{
        new FileJournal{role, instance_id, observer, std::move(segment_path), fd, segment_bytes, commit}}};
    owned.Release();
    return Ok(std::move(journal));
}

// TODO(rebirth:M4-STO-01) strict single-segment recovery is real; high-water/key-retirement qualification remains.
Result<std::unique_ptr<FileJournal>> FileJournal::OpenExisting(const fs::path& dir, JournalRole role,
                                                               uint64_t instance_id)
{
    if (!IsKnown(role)) return Error{ErrorCode::INVALID_INPUT, "unknown journal role"};
    std::error_code ec;
    const fs::file_status dir_status{fs::symlink_status(dir, ec)};
    if (ec || !fs::is_directory(dir_status)) return Error{ErrorCode::INVALID_INPUT, JOURNAL_DIR_MISSING};
    fs::directory_iterator entry{dir, ec};
    if (ec || entry == fs::directory_iterator{}) return Error{ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_LAYOUT};
    const fs::path segment_path{dir / fs::u8path(FIRST_SEGMENT_NAME)};
    if (entry->path() != segment_path) return Error{ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_LAYOUT};
    const fs::file_status segment_status{fs::symlink_status(segment_path, ec)};
    if (ec || !fs::is_regular_file(segment_status)) return Error{ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_LAYOUT};
    entry.increment(ec);
    if (ec || entry != fs::directory_iterator{}) return Error{ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_LAYOUT};
#ifdef WIN32
    const int fd{_wopen(segment_path.c_str(), _O_RDWR | _O_BINARY)};
#else
    const int fd{open(segment_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW)};
#endif
    if (fd < 0) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_OPEN};
    SegmentFd owned{fd};
    if (!LockSegment(fd)) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_LOCKED};
    const Result<uint64_t> size{SegmentSize(fd)};
    if (!size.has_value()) return size.error();
    if (*size < SEGMENT_PAGE_BYTES || *size > MAX_SEGMENT_BYTES || *size % SEGMENT_PAGE_BYTES != 0) {
        return Error{ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_SIZE};
    }
    const Result<std::vector<unsigned char>> image{ReadPrefix(fd, *size)};
    if (!image.has_value()) return image.error();
    const Result<ScanResult> scanned{ScanRecords(*image, role, instance_id)};
    if (!scanned.has_value()) return scanned.error();
    // A fully written record may have survived an unacknowledged barrier.
    // Commit the verified prefix now before it can mint any durable token.
    if (const Result<void> committed{JournalCommit(fd)}; !committed.has_value()) return committed.error();
    if (const Result<void> committed{CommitDirectory(dir)}; !committed.has_value()) return committed.error();
    auto journal{std::unique_ptr<FileJournal>{new FileJournal{role, instance_id, nullptr, segment_path, fd, *size,
                                                            &JournalCommit, &*scanned}}};
    owned.Release();
    return Ok(std::move(journal));
}

Result<std::vector<JournalRecord>> FileJournal::ReadAll() const
{
    const uint64_t durable_bytes{DurableImageSize()};
    const Result<uint64_t> size{SegmentSize(m_fd)};
    if (!size.has_value()) return size.error();
    if (*size != m_segment_bytes) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_SIZE};
    const Result<std::vector<unsigned char>> image{ReadPrefix(m_fd, durable_bytes)};
    if (!image.has_value()) return image.error();
    Result<ScanResult> scanned{ScanRecords(*image, Role(), InstanceId())};
    if (!scanned.has_value()) return scanned.error();
    // A zeroed suffix inside a previously committed prefix must never silently
    // turn an acknowledged history into a shorter valid journal.
    if (scanned->end_offset != durable_bytes) return Error{ErrorCode::VERIFY_FAILED, RECORD_TAIL};
    return Ok(std::move(scanned->records));
}

Result<void> FileJournal::Rollover()
{
    return NotImplemented("M4-STO-06"); // TODO(rebirth:M4-STO-06)
}

Result<void> FileJournal::AdmitAppend(uint64_t /*append_number*/, uint64_t offset, size_t size) const
{
    // The record must fit in the rest of the segment; the next segment is M4-STO-06.
    if (offset > m_segment_bytes || size > m_segment_bytes - offset) {
        return NotImplemented("M4-STO-06"); // TODO(rebirth:M4-STO-06)
    }
    return Ok();
}

Result<void> FileJournal::Persist(uint64_t /*barrier_number*/, uint64_t offset, std::span<const unsigned char> bytes)
{
    if (const Result<void> written{WriteAllAt(m_fd, bytes, offset)}; !written.has_value()) return written.error();
    return m_commit(m_fd);
}

} // namespace flowmesh2::storage
