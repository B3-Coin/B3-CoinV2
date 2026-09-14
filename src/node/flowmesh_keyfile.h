// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_NODE_FLOWMESH_KEYFILE_H
#define BITCOIN_NODE_FLOWMESH_KEYFILE_H

#include <cerrno>
#include <cstdio>
#include <filesystem>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#endif

namespace node::detail {
/** Open only the operator identity, without changing other wallet file I/O.
 * Creation must be exclusive even if a file appears after the caller's check.
 * The caller still commits the contents and directory before publishing a key.
 */
inline FILE* OpenFlowMeshKeyFile(const std::filesystem::path& path, bool create_new)
{
#ifdef _WIN32
    if (!create_new) return ::_wfopen(path.c_str(), L"rb");
    // The distributed MinGW/MSVCRT build does not support fopen's C11 "x".
    // Never fall back to "wb": it could overwrite an existing identity.
    const int fd{::_wopen(path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT,
                         _S_IREAD | _S_IWRITE)};
    if (fd == -1) return nullptr;
    FILE* file{::_fdopen(fd, "wb")};
    if (!file) {
        const int saved_errno{errno};
        ::_close(fd);
        errno = saved_errno;
        // Preserve the created file on failure. Do not unlink a path which
        // could have been replaced, or silently retry with a new identity.
    }
    return file;
#else
    return ::fopen(path.c_str(), create_new ? "wbx" : "rb");
#endif
}
} // namespace node::detail

#endif // BITCOIN_NODE_FLOWMESH_KEYFILE_H
