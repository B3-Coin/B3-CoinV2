// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_BRIDGE_FORCED_EXIT_H
#define B3_FLOWMESH2_BRIDGE_FORCED_EXIT_H

#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/merkle.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/state_root.h>

#include <cstdint>
#include <utility>
#include <vector>

/** Forced exit (slice R13: interface and stubs; PLAN-16, D20, OWNER-GATED;
 * [CC] C8a).
 *
 * Target (M8-EXIT-01): Open(last_checkpoint_height, b3_height) is true once
 * FORCED_EXIT_DELAY_BLOCKS B3 blocks have passed without an accepted
 * checkpoint, counted by height, never by wall clock. Redeem(claim,
 * b3_height) then pays an account's available + reserved + futures equity
 * as proven by its leaf under the last accepted checkpoint's state root,
 * once: the redemption is recorded by an ExitNullifierFact (core/facts.h),
 * so a minority cannot block an exit and nobody can exit twice.
 *
 * Stubs: Open and Redeem return NotImplemented(M8-EXIT-01).
 */
namespace flowmesh2::bridge {

//! Blocks without an accepted checkpoint before exits open (about 7 days). PROVISIONAL (PLAN-16).
inline constexpr uint64_t FORCED_EXIT_DELAY_BLOCKS{10'080};

//! Target shape: an exit claim against the last accepted checkpoint (M8-EXIT-01).
struct ExitClaim {
    uint64_t checkpoint_height{0};
    AccountLeafV0 leaf;
    StateRootsV0 roots;
    MerkleProof proof;
};

//! Target shape: what one exit pays (M8-EXIT-01).
struct ExitRedemption {
    AccountId account;
    std::vector<std::pair<AssetId, Atoms>> amounts;
};

class ForcedExit
{
public:
    ForcedExit() = delete;

    //! Stub M8-EXIT-01.
    static Result<bool> Open(uint64_t last_checkpoint_height, uint64_t b3_height);
    //! Stub M8-EXIT-01.
    static Result<ExitRedemption> Redeem(const ExitClaim& claim, uint64_t b3_height);
};

} // namespace flowmesh2::bridge

#endif // B3_FLOWMESH2_BRIDGE_FORCED_EXIT_H
