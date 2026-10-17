// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Compile-fail control (CTest flowmesh2_negative_compile, Clang only).
// An ApplyReceipt proves that StateOwner::Apply passed check 3 (O-92); it must
// not be constructible anywhere else, or a caller could write APPLIED, and
// publish, a slot that check 3 refused. Built as is it must fail with the
// diagnostic below; built with FM2_COMPILE_FAIL_FIXED (the receipt is
// received and moved on, never made) it must compile.
//
// FM2_EXPECT_ERROR: forge_receipt\.cpp:\d+:\d+: error: calling a private constructor of class 'flowmesh2::ApplyReceipt'

#include <flowmesh2/storage/durable.h>
#include <uint256.h>

#include <cstdint>
#include <utility>

namespace {

uint64_t WriteApplied(flowmesh2::ApplyReceipt&& receipt)
{
    const flowmesh2::ApplyReceipt owned{std::move(receipt)};
    return owned.Slot();
}

} // namespace

#ifdef FM2_COMPILE_FAIL_FIXED
uint64_t Fm2CompileFailForgeReceipt(flowmesh2::ApplyReceipt&& received)
{
    return WriteApplied(std::move(received));
}
#else
uint64_t Fm2CompileFailForgeReceipt()
{
    // The private constructor (journal instance, decision sequence, slot,
    // header hash, state root): only state::StateOwner::Apply may call it.
    return WriteApplied(flowmesh2::ApplyReceipt{1, 1, 1, uint256{}, uint256{}});
}
#endif
