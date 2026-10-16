// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// AddressSanitizer canary (target fm2_canary_asan; built only with
// -DFLOWMESH2_SANITIZER_CANARIES=ON and SANITIZERS containing "address").
//
// It reads one element past a heap array. Under AddressSanitizer it MUST exit
// non-zero with "AddressSanitizer: heap-buffer-overflow". If it exits 0, the
// sanitizer did not run, and no ctest result of this build may be trusted
// (plan section 10.2, step 5). It is deliberately defective code; never link
// it into anything.

#include <cstdio>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FM2_CANARY_INSTRUMENTED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define FM2_CANARY_INSTRUMENTED 1
#endif

int main(int argc, char** /*argv*/)
{
#ifndef FM2_CANARY_INSTRUMENTED
    std::fprintf(stderr, "fm2_canary_asan: built WITHOUT AddressSanitizer; the canary cannot fire\n");
#endif
    // The size depends on argc so the compiler cannot prove the overflow.
    const int size{4 + (argc - 1)};
    int* const buffer{new int[size]{}};
    const volatile int* const view{buffer};
    const int value{view[size]}; // one element past the end
    delete[] buffer;
    std::printf("fm2_canary_asan: read %d past the end and was NOT flagged\n", value);
    return 0;
}
