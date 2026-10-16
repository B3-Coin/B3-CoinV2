// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_AGREEMENT_RESTART_FENCE_H
#define B3_FLOWMESH2_AGREEMENT_RESTART_FENCE_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/storage/tokens.h>

#include <cstdint>
#include <span>

/** The restart fence (slice R10: interface and stub; PLAN-8, DUR-3).
 *
 * A seat whose signer journal could have been rolled back (a restored VM, a
 * lost disk) must not sign again below what it may already have signed.
 * Target (M4-AGR-06): after a restart the seat collects signed high-water
 * statements (the highest slot and view each seat has seen this seat sign)
 * from at least 2f + 1 seats of the seat set, and fences itself below the
 * highest; a statement above its own journal means the journal was rolled
 * back, and the key is retired.
 *
 * Collect(statements, seats, self_seat) -> the fence point. Stub
 * M4-AGR-06. The outline makes no recovery claim (ARCHITECTURE.md 7.4).
 */
namespace flowmesh2::agreement {

//! One seat's signed statement of the highest (slot, view) it saw `subject` sign. Target shape.
struct HighWaterStatement {
    uint32_t seat{0};
    uint32_t subject{0};
    uint64_t epoch{0};
    uint64_t slot{0};
    uint64_t view{0};
    storage::VoteSignature signature{};
};

//! The restarted seat may sign only above (slot, view).
struct FencePoint {
    uint64_t slot{0};
    uint64_t view{0};
    friend bool operator==(const FencePoint&, const FencePoint&) = default;
};

class RestartFence
{
public:
    RestartFence() = delete;
    //! Stub M4-AGR-06.
    static Result<FencePoint> Collect(std::span<const HighWaterStatement> statements, const SeatSet& seats, uint32_t self_seat);
};

} // namespace flowmesh2::agreement

#endif // B3_FLOWMESH2_AGREEMENT_RESTART_FENCE_H
