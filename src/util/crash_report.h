// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_UTIL_CRASH_REPORT_H
#define BITCOIN_UTIL_CRASH_REPORT_H

#include <util/fs.h>

#include <optional>
#include <string_view>
#include <vector>

static constexpr bool DEFAULT_CRASH_REPORTS{true};

/**
 * Crash reports that are safe to share.
 *
 * When the process dies from a fatal signal or Windows exception, an
 * uncaught C++ exception or a failed assertion, a short text file is written
 * to the report directory. It holds the version, the platform, the reason,
 * the crashing thread's name, an assertion or exception message, and the
 * call stack as module name plus offset. It never holds memory contents,
 * keys, wallet data or log lines, and nothing is sent anywhere.
 */
namespace crash_report {

//! Name of the report directory inside the network data directory.
inline constexpr std::string_view DIRECTORY_NAME{"crash_reports"};

/**
 * Prepare `directory` and install the process-wide handlers. Returns the
 * reports earlier runs left that were not announced yet (each is announced
 * once), or std::nullopt if the directory cannot be prepared, in which case
 * no handler is installed.
 */
std::optional<std::vector<fs::path>> Install(const fs::path& directory);

//! Reports found by Install() (empty before Install or if none were new).
std::vector<fs::path> PreviousReports();

/**
 * Crash deliberately so tests can check the report. `kind` is one of
 * segv, abort, assert, check, throw or overflow; anything else is ignored.
 */
void TriggerTestCrash(std::string_view kind);

} // namespace crash_report

#endif // BITCOIN_UTIL_CRASH_REPORT_H
