// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Compile-fail control (CTest flowmesh2_negative_compile, Clang only).
// CommitLog::AppendApplied takes only the ApplyReceipt of a successful
// StateOwner::Apply (O-92). Writing APPLIED from a decision token and a
// state root, as before the review fix, would let a caller mark applied, and
// publish, a slot that check 3 refused. Built as is it must fail with the
// diagnostic below; built with FM2_COMPILE_FAIL_FIXED (the receipt is
// passed) it must compile.
//
// FM2_EXPECT_ERROR: apply_without_receipt\.cpp:\d+:\d+: error: too many arguments to function call, expected single argument 'receipt'

#include <flowmesh2/core/result.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/durable.h>
#include <flowmesh2/storage/tokens.h>

flowmesh2::Result<flowmesh2::storage::PendingApplied> Fm2CompileFailApplyWithoutReceipt(flowmesh2::storage::CommitLog& log,
                                                                                       const flowmesh2::DurableDecision& decision,
                                                                                       const flowmesh2::ApplyReceipt& receipt)
{
#ifdef FM2_COMPILE_FAIL_FIXED
    (void)decision;
    return log.AppendApplied(receipt);
#else
    (void)receipt;
    // A durable decision is not proof that Apply passed check 3.
    return log.AppendApplied(decision, decision.Digest());
#endif
}
