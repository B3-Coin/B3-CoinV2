// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Compile-fail control (CTest flowmesh2_negative_compile, Clang only).
// SeatSigner::Sign, the only BLS vote signing path, must take a DurableIntent
// (the token of the B-INTENT barrier) and nothing else: signing a pending
// intent would sign before the intent is durable (ARCHITECTURE.md 7.3).
// Built as is it must fail with the diagnostic below; built with
// FM2_COMPILE_FAIL_FIXED (the pending intent is confirmed first and the
// token is signed) it must compile.
//
// FM2_EXPECT_ERROR: sign_without_intent\.cpp:\d+:\d+: error: no viable conversion from 'const flowmesh2::storage::PendingIntent' \(aka '[^']*'\) to 'const DurableIntent'

#include <flowmesh2/core/result.h>
#include <flowmesh2/storage/durable.h>
#include <flowmesh2/storage/seat_signer.h>
#include <flowmesh2/storage/signer_log.h>
#include <flowmesh2/storage/tokens.h>

flowmesh2::Result<flowmesh2::storage::VoteSignature> Fm2CompileFailSign(const flowmesh2::storage::SeatSigner& signer,
                                                                        const flowmesh2::storage::SignerLog& log,
                                                                        const flowmesh2::storage::PendingIntent& pending)
{
#ifdef FM2_COMPILE_FAIL_FIXED
    const flowmesh2::Result<flowmesh2::DurableIntent> intent{log.Confirm(pending)};
    if (!intent.has_value()) return intent.error();
    return signer.Sign(*intent);
#else
    (void)log;
    return signer.Sign(pending);
#endif
}
