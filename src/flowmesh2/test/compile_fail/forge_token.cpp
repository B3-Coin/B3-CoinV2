// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Compile-fail control (CTest flowmesh2_negative_compile, Clang only).
// A durability token must not be constructible outside Journal: forging a
// DurableIntent would let code sign before the B-INTENT barrier. Built as is
// it must fail with the diagnostic below; built with FM2_COMPILE_FAIL_FIXED
// (the token is received and moved on, never made) it must compile.
//
// FM2_EXPECT_ERROR: forge_token\.cpp:\d+:\d+: error: calling a private constructor of class 'flowmesh2::Durable<flowmesh2::DurableKind::INTENT>'

#include <flowmesh2/storage/durable.h>

#include <cstdint>
#include <utility>

namespace {

uint64_t SignWithIntent(flowmesh2::DurableIntent&& intent)
{
    const flowmesh2::DurableIntent owned{std::move(intent)};
    return owned.Seq();
}

} // namespace

#ifdef FM2_COMPILE_FAIL_FIXED
uint64_t Fm2CompileFailForgeToken(flowmesh2::DurableIntent&& received)
{
    return SignWithIntent(std::move(received));
}
#else
uint64_t Fm2CompileFailForgeToken()
{
    // The private constructor (journal instance, sequence number, binding):
    // only storage::Journal::Confirm may call it.
    return SignWithIntent(flowmesh2::DurableIntent{1, 1, flowmesh2::storage::VoteRef{}});
}
#endif
