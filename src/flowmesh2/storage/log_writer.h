// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STORAGE_LOG_WRITER_H
#define B3_FLOWMESH2_STORAGE_LOG_WRITER_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/storage/journal.h>

#include <cstddef>
#include <memory>

/** The group-commit log writer (target contract; stub M4-STO-02).
 *
 * In the outline every barrier is issued by the sealing thread, one per
 * journal step (section 7.3). M4-STO-02 adds a writer thread per journal that
 * collects the records appended since its last barrier and covers them with
 * one Barrier, so several slots or votes share one commit. It works over
 * Pending/Confirm: callers append, then wait until the writer's durable
 * sequence number reaches their record and Confirm it. Group commit may cover
 * several records with one barrier but never lets a token exist before its
 * barrier, and it never holds the state owner's lock (plan section 3.7). The
 * thread will be TSan-checked (acceptance of M4-STO-02).
 */
namespace flowmesh2::storage {

struct LogWriterOptions {
    //! Most records one barrier may cover. PROVISIONAL target value.
    size_t max_group_records{1024};
};

class LogWriter
{
public:
    //! Start the writer thread over `journal`. Stub: M4-STO-02.
    static Result<std::unique_ptr<LogWriter>> Start(Journal& journal, const LogWriterOptions& options);

private:
    LogWriter() = default;
};

} // namespace flowmesh2::storage

#endif // B3_FLOWMESH2_STORAGE_LOG_WRITER_H
