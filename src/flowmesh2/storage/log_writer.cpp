// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/storage/log_writer.h>

namespace flowmesh2::storage {

Result<std::unique_ptr<LogWriter>> LogWriter::Start(Journal& /*journal*/, const LogWriterOptions& /*options*/)
{
    return NotImplemented("M4-STO-02"); // TODO(rebirth:M4-STO-02)
}

} // namespace flowmesh2::storage
