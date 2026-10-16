// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// ThreadSanitizer canary (target fm2_canary_tsan; built only with
// -DFLOWMESH2_SANITIZER_CANARIES=ON and SANITIZERS containing "thread").
//
// Two threads increment one plain int without synchronization. Under TSan it
// MUST exit non-zero with "ThreadSanitizer: data race".
// If it exits 0, the sanitizer did not run, and no ctest result of this build
// may be trusted. Deliberately defective code: it uses std::thread and an
// unsynchronized global on purpose, which is why it lives outside
// src/flowmesh2/. Never link it into anything.

#include <cstdio>
#include <thread>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define FM2_CANARY_INSTRUMENTED 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define FM2_CANARY_INSTRUMENTED 1
#endif

namespace {

int g_shared{0};

void Bump()
{
    for (int i{0}; i < 1000; ++i) ++g_shared;
}

} // namespace

int main()
{
#ifndef FM2_CANARY_INSTRUMENTED
    std::fprintf(stderr, "fm2_canary_tsan: built WITHOUT ThreadSanitizer; the canary cannot fire\n");
#endif
    std::thread first{Bump};
    std::thread second{Bump};
    first.join();
    second.join();
    std::printf("fm2_canary_tsan: counter %d, the data race was NOT flagged\n", g_shared);
    return 0;
}
