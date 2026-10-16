// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STORAGE_JOURNAL_COMMIT_H
#define B3_FLOWMESH2_STORAGE_JOURNAL_COMMIT_H

#include <flowmesh2/core/result.h>
#include <util/fs.h>

#include <string_view>

/** Strict commit primitives of the journals (slice R8; ARCHITECTURE.md 7.1).
 *
 * Bitcoin's FileCommit, DirectoryCommit and AllocateFileRange ignore some
 * errors (util/fs_helpers.cpp: FileCommit tolerates EINVAL, the other two
 * return nothing). The journals must not (DUR-8), so they use these:
 *
 * - JournalCommit(fd): fdatasync on Linux; F_FULLFSYNC on macOS with NO
 *   fsync fallback; FlushFileBuffers on Windows; fsync on other POSIX
 *   systems. Any error, including EINVAL and EINTR, is STORAGE_FATAL with a
 *   detail naming the call. The caller treats it as sticky and never
 *   retries (Journal::Barrier).
 * - JournalDirectoryCommit(dir_fd): fsync of an open directory descriptor;
 *   any error is STORAGE_FATAL. CommitDirectory(path) opens the directory,
 *   commits it and closes it; failing to open or close it is fatal too. On
 *   Windows a directory cannot be committed this way; both return Ok() there,
 *   as Bitcoin's DirectoryCommit does (the outline is not built on Windows,
 *   O-24; M4-STO-01 decides).
 *
 * The functions do I/O and are not in a deterministic directory; they hold no
 * lock.
 */
namespace flowmesh2::storage {

inline constexpr std::string_view COMMIT_FDATASYNC{"COMMIT_FDATASYNC"};
inline constexpr std::string_view COMMIT_F_FULLFSYNC{"COMMIT_F_FULLFSYNC"};
inline constexpr std::string_view COMMIT_FLUSHFILEBUFFERS{"COMMIT_FLUSHFILEBUFFERS"};
inline constexpr std::string_view COMMIT_FSYNC{"COMMIT_FSYNC"};
inline constexpr std::string_view COMMIT_DIRECTORY_FSYNC{"COMMIT_DIRECTORY_FSYNC"};
inline constexpr std::string_view COMMIT_DIRECTORY_OPEN{"COMMIT_DIRECTORY_OPEN"};
inline constexpr std::string_view COMMIT_DIRECTORY_CLOSE{"COMMIT_DIRECTORY_CLOSE"};

//! Commit the data of an open file; STORAGE_FATAL on any error (see above).
Result<void> JournalCommit(int fd);
//! fsync an open directory descriptor; STORAGE_FATAL on any error.
Result<void> JournalDirectoryCommit(int dir_fd);
//! Open `dir`, JournalDirectoryCommit it, close it; STORAGE_FATAL on any error.
Result<void> CommitDirectory(const fs::path& dir);

} // namespace flowmesh2::storage

#endif // B3_FLOWMESH2_STORAGE_JOURNAL_COMMIT_H
