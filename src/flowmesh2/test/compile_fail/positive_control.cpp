// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Positive control (CTest flowmesh2_negative_compile, Clang only).
// The correct form of all four negative controls in one translation unit,
// built with exactly the same flags (fm2_warnings, core_interface, fm2_types).
// It must compile; if it does not, the harness is broken and every negative
// result is meaningless.

#include <flowmesh2/core/result.h>
#include <flowmesh2/storage/durable.h>
#include <flowmesh2/storage/seat_signer.h>
#include <flowmesh2/storage/tokens.h>
#include <sync.h>

#include <cstdint>
#include <utility>

namespace {

class GuardedCounter
{
public:
    int Read() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        return m_value;
    }

private:
    mutable Mutex m_mutex;
    int m_value GUARDED_BY(m_mutex){0};
};

flowmesh2::Result<int> ParseCount(int raw)
{
    if (raw < 0) return flowmesh2::Error{flowmesh2::ErrorCode::DECODE, "negative count"};
    return flowmesh2::Ok(raw);
}

uint64_t SignWithIntent(flowmesh2::DurableIntent&& intent)
{
    const flowmesh2::DurableIntent owned{std::move(intent)};
    return owned.Seq();
}

} // namespace

int Fm2PositiveControl()
{
    const GuardedCounter counter;
    const auto count{ParseCount(1)};
    return counter.Read() + (count.has_value() ? *count : 0);
}

uint64_t Fm2PositiveControlToken(flowmesh2::DurableIntent&& received)
{
    return SignWithIntent(std::move(received));
}

flowmesh2::Result<flowmesh2::storage::VoteSignature> Fm2PositiveControlSign(const flowmesh2::storage::SeatSigner& signer,
                                                                            const flowmesh2::DurableIntent& intent)
{
    return signer.Sign(intent);
}
