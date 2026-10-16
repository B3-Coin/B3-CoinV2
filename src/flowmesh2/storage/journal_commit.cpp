// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/storage/journal_commit.h>

#ifdef WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace flowmesh2::storage {

Result<void> JournalCommit(int fd)
{
#if defined(WIN32)
    const HANDLE handle{reinterpret_cast<HANDLE>(_get_osfhandle(fd))};
    if (handle == INVALID_HANDLE_VALUE || FlushFileBuffers(handle) == 0) {
        return Error{ErrorCode::STORAGE_FATAL, COMMIT_FLUSHFILEBUFFERS};
    }
    return Ok();
#elif defined(__APPLE__)
    // F_FULLFSYNC asks the drive to flush its cache. There is deliberately no
    // fsync() fallback: a failure here is fatal, whatever errno says.
    if (fcntl(fd, F_FULLFSYNC, 0) != 0) return Error{ErrorCode::STORAGE_FATAL, COMMIT_F_FULLFSYNC};
    return Ok();
#elif defined(__linux__)
    if (fdatasync(fd) != 0) return Error{ErrorCode::STORAGE_FATAL, COMMIT_FDATASYNC};
    return Ok();
#else
    if (fsync(fd) != 0) return Error{ErrorCode::STORAGE_FATAL, COMMIT_FSYNC};
    return Ok();
#endif
}

Result<void> JournalDirectoryCommit(int dir_fd)
{
#ifdef WIN32
    (void)dir_fd;
    return Ok();
#else
    if (fsync(dir_fd) != 0) return Error{ErrorCode::STORAGE_FATAL, COMMIT_DIRECTORY_FSYNC};
    return Ok();
#endif
}

Result<void> CommitDirectory(const fs::path& dir)
{
#ifdef WIN32
    (void)dir;
    return Ok();
#else
    const int dir_fd{open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (dir_fd < 0) return Error{ErrorCode::STORAGE_FATAL, COMMIT_DIRECTORY_OPEN};
    const Result<void> committed{JournalDirectoryCommit(dir_fd)};
    const bool closed{close(dir_fd) == 0};
    if (!committed.has_value()) return committed.error();
    if (!closed) return Error{ErrorCode::STORAGE_FATAL, COMMIT_DIRECTORY_CLOSE};
    return Ok();
#endif
}

} // namespace flowmesh2::storage
