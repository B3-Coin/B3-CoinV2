// Copyright (c) 2022-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/check.h>

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <clientversion.h>
#include <tinyformat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <source_location>
#include <string>
#include <string_view>

std::string StrFormatInternalBug(std::string_view msg, const std::source_location& loc)
{
    return strprintf("Internal bug detected: %s\n%s:%d (%s)\n"
                     "%s %s\n"
                     "Please report this issue here: %s\n",
                     msg, loc.file_name(), loc.line(), loc.function_name(),
                     CLIENT_NAME, FormatFullVersion(), CLIENT_BUGREPORT);
}

NonFatalCheckError::NonFatalCheckError(std::string_view msg, const std::source_location& loc)
    : std::runtime_error{StrFormatInternalBug(msg, loc)}
{
}

bool g_detail_test_only_CheckFailuresAreExceptionsNotAborts{false};

namespace {
char g_assertion_failure[1024];
} // namespace

const char* AssertionFailureMessage() { return g_assertion_failure; }

void assertion_fail(const std::source_location& loc, std::string_view assertion)
{
    if (g_detail_test_only_CheckFailuresAreExceptionsNotAborts) {
        throw NonFatalCheckError{assertion, loc};
    }
    auto str = strprintf("%s:%s %s: Assertion `%s' failed.\n", loc.file_name(), loc.line(), loc.function_name(), assertion);
    // Keep the message, without its newline, for a crash report.
    const size_t len{std::min(str.size() - 1, sizeof(g_assertion_failure) - 1)};
    std::memcpy(g_assertion_failure, str.data(), len);
    fwrite(str.data(), 1, str.size(), stderr);
    std::abort();
}

std::atomic<bool> g_enable_dynamic_fuzz_determinism{false};
