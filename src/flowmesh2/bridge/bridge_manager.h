// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_BRIDGE_BRIDGE_MANAGER_H
#define B3_FLOWMESH2_BRIDGE_BRIDGE_MANAGER_H

#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/result.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <vector>

/** The bridge boundary of the new core (slice R13: interfaces and stubs;
 * plan section 3.9; BRG-1, BRG-4, BRG-5, PLAN-14; [CC] C8a).
 *
 * The outline has no custody code (D2 not used). Every method is a stub,
 * and every bridge task is gated: CLAUDE.md rules 5 and 6 (bridge-backed
 * bUSD through the managed-v1 Ethereum vault, the managed withdrawal leg,
 * authority and runtime pins from independent mainnet observations) and
 * plan C-6 must be reconciled before any mainnet bridge work. Nothing here
 * names a vault, a token contract or a key.
 *
 * Target:
 * - OnFinalizedMint (M8-BRG-01): a bUSD mint that the sync-committee light
 *   client in b3d finalized becomes a VaultV2 deposit for a FlowMesh
 *   account; the result is the DepositFact the chain adapter delivers
 *   (one per vault outpoint, so a mint is credited once).
 * - BuildWithdrawalPacket (M8-BRG-02): a certified withdrawal effect
 *   (TRD-21: reserved -> certified -> settlement pending) becomes a packet
 *   in the B3 message_root that the checkpoint commits; the payout is
 *   confirmed by a PayoutConfirmedFact (core/facts.h), so it is paid once.
 * - CheckReleaseLimits (M8-BRG-03, OWNER-GATED): the validity floor (a
 *   certificate is bridge-valid only with >= 50 % of total active stake),
 *   the safe exporter (sign only after a confirmed 2/3 commit) and fixed
 *   caps at versioned heights (plan 3.9, PROVISIONAL: pilot 10,000/day,
 *   then <= 250,000/day; >= 50,000 waits 2 h; mints <= 10,000 per block
 *   and per epoch).
 */
namespace flowmesh2::bridge {

//! Target shape: a finalized bUSD mint (M8-BRG-01).
struct FinalizedMint {
    uint256 eth_tx_hash;
    uint32_t log_index{0};
    AccountId account;
    AssetId asset;
    Atoms amount{0};
    Outpoint vault_outpoint; //!< the VaultV2 deposit output that carries it
    FactCursor cursor;       //!< where the chain adapter delivers it
};

//! Target shape: a certified withdrawal effect (M8-BRG-02).
struct WithdrawalRequest {
    AccountId account;
    AssetId asset;
    Atoms amount{0};
    std::array<unsigned char, 20> eth_recipient{};
    uint64_t slot{0}; //!< the certified slot that reserved it
};

//! Target shape: one packet of the B3 message_root (M8-BRG-02).
struct WithdrawalPacket {
    uint256 packet_id;
    std::vector<unsigned char> encoded;
};

//! Target shape: the caps of one versioned height (M8-BRG-03).
struct ReleaseLimits {
    Atoms daily_cap{0};
    Atoms delay_threshold{0};
    uint64_t delay_blocks{0};
    Atoms per_block_mint_cap{0};
};

//! Target shape: what was released so far in the current window (M8-BRG-03).
struct ReleaseWindow {
    Atoms released{0};
    uint64_t window_start_height{0};
    uint64_t height{0};
};

class BridgeManagerV2
{
public:
    BridgeManagerV2() = default;

    //! Stub M8-BRG-01.
    Result<DepositFact> OnFinalizedMint(const FinalizedMint& mint);
    //! Stub M8-BRG-02.
    Result<WithdrawalPacket> BuildWithdrawalPacket(const WithdrawalRequest& request);
    //! Stub M8-BRG-03.
    Result<void> CheckReleaseLimits(const WithdrawalRequest& request, const ReleaseWindow& window, const ReleaseLimits& limits) const;
};

} // namespace flowmesh2::bridge

#endif // B3_FLOWMESH2_BRIDGE_BRIDGE_MANAGER_H
