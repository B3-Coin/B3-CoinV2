// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Compile-fail control (CTest flowmesh2_negative_compile, Clang only).
// Reading a GUARDED_BY member without LOCK must be a build error under the fm2
// flag -Werror=thread-safety. Built as is it must fail with the diagnostic
// below; built with FM2_COMPILE_FAIL_FIXED (the LOCK added) it must compile.
//
// FM2_EXPECT_ERROR: tsa_missing_lock\.cpp:\d+:\d+: error: reading variable 'm_value' requires holding .*'m_mutex'.*\[-Werror,-Wthread-safety-analysis\]

#include <sync.h>

namespace {

class GuardedCounter
{
public:
    int Read() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
#ifdef FM2_COMPILE_FAIL_FIXED
        LOCK(m_mutex);
#endif
        return m_value;
    }

private:
    mutable Mutex m_mutex;
    int m_value GUARDED_BY(m_mutex){0};
};

} // namespace

int Fm2CompileFailTsaMissingLock()
{
    const GuardedCounter counter;
    return counter.Read();
}
