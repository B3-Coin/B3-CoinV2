// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/storage/file_journal.h>

#include <flowmesh2/storage/journal_commit.h>

#include <algorithm>
#include <array>
#include <limits>
#include <system_error>
#include <utility>

#ifdef WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace flowmesh2::storage {

namespace {

//! Write all of `bytes` at `offset`; STORAGE_FATAL on any error or zero-length write.
Result<void> WriteAllAt(int fd, std::span<const unsigned char> bytes, uint64_t offset)
{
#ifdef WIN32
    if (_lseeki64(fd, static_cast<__int64>(offset), SEEK_SET) < 0) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_WRITE};
    while (!bytes.empty()) {
        const unsigned int chunk{static_cast<unsigned int>(std::min<size_t>(bytes.size(), 1U << 30))};
        const int written{_write(fd, bytes.data(), chunk)};
        if (written <= 0) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_WRITE};
        bytes = bytes.subspan(static_cast<size_t>(written));
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

} // namespace

FileJournal::FileJournal(JournalRole role, uint64_t instance_id, BarrierObserver* observer, fs::path segment_path, int fd,
                         uint64_t segment_bytes, SegmentCommitHook commit)
    : Journal(ImplKey{}, role, instance_id, observer),
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

    // Explicit zero pages for the whole segment (plan section 3.7, S-9): the
    // blocks are allocated now, not on the first barrier, and the unused tail
    // reads back as zeros.
    static constexpr std::array<unsigned char, 64 * 1024> ZEROS{};
    for (uint64_t offset{0}; offset < segment_bytes; offset += ZEROS.size()) {
        const size_t chunk{static_cast<size_t>(std::min<uint64_t>(ZEROS.size(), segment_bytes - offset))};
        if (const Result<void> written{WriteAllAt(fd, std::span{ZEROS}.first(chunk), offset)}; !written.has_value()) {
            CloseFd(fd);
            return written.error();
        }
    }
    // Creation always uses the real strict commit; a hook only replaces the barriers' commit.
    if (const Result<void> committed{JournalCommit(fd)}; !committed.has_value()) {
        CloseFd(fd);
        return committed.error();
    }
    if (const Result<void> dir_committed{CommitDirectory(empty_dir)}; !dir_committed.has_value()) {
        CloseFd(fd);
        return dir_committed.error();
    }
    return Ok(std::unique_ptr<FileJournal>{
        new FileJournal{role, instance_id, observer, std::move(segment_path), fd, segment_bytes, commit}});
}

Result<std::unique_ptr<FileJournal>> FileJournal::OpenExisting(const fs::path& /*dir*/, JournalRole /*role*/,
                                                               uint64_t /*instance_id*/)
{
    return NotImplemented("M4-STO-01"); // TODO(rebirth:M4-STO-01)
}

Result<std::vector<JournalRecord>> FileJournal::ReadAll() const
{
    return NotImplemented("M4-STO-01"); // TODO(rebirth:M4-STO-01)
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
