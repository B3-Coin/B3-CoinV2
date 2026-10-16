// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// UndefinedBehaviorSanitizer canary (target fm2_canary_ubsan; built only with
// -DFLOWMESH2_SANITIZER_CANARIES=ON and SANITIZERS containing "undefined").
//
// It overflows a signed int. Under UBSan with UBSAN_OPTIONS=halt_on_error=1
// (the environment the flowmesh2 CTests run in) it MUST exit non-zero with
// "runtime error: signed integer overflow". If it exits 0, either UBSan did
// not run or undefined behaviour does not stop a test, and no ctest result of
// this build may be trusted. It is deliberately defective code; never link it
// into anything.

#include <climits>
#include <cstdio>

int main(int argc, char** /*argv*/)
{
    const volatile int base{INT_MAX};
    const int sum{base + argc}; // argc >= 1: signed overflow
    std::printf("fm2_canary_ubsan: INT_MAX + %d = %d and was NOT flagged\n", argc, sum);
    return 0;
}
