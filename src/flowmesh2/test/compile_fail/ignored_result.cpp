// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Compile-fail control (CTest flowmesh2_negative_compile, Clang only).
// Ignoring a [[nodiscard]] flowmesh2::Result must be a build error under the
// fm2 flag -Werror=unused-result. Built as is it must fail with the diagnostic
// below; built with FM2_COMPILE_FAIL_FIXED (the result is checked) it must
// compile.
//
// FM2_EXPECT_ERROR: ignored_result\.cpp:\d+:\d+: error: ignoring return value of .*nodiscard.*\[-Werror,-Wunused-result\]

#include <flowmesh2/core/result.h>

namespace {

flowmesh2::Result<int> ParseCount(int raw)
{
    if (raw < 0) return flowmesh2::Error{flowmesh2::ErrorCode::DECODE, "negative count"};
    return flowmesh2::Ok(raw);
}

} // namespace

int Fm2CompileFailIgnoredResult()
{
#ifdef FM2_COMPILE_FAIL_FIXED
    const auto count{ParseCount(1)};
    return count.has_value() ? *count : 0;
#else
    ParseCount(1);
    return 0;
#endif
}
