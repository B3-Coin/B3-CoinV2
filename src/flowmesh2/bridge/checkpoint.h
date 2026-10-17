// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_BRIDGE_CHECKPOINT_H
#define B3_FLOWMESH2_BRIDGE_CHECKPOINT_H

#include <flowmesh2/bridge/bridge_manager.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/state/candidate.h>
#include <uint256.h>

#include <cstdint>
#include <span>

/** The checkpoint builder (slice R13: interface and stub; plan sections 3.6
 * and 3.9; PLAN-3; [CC] C8a).
 *
 * Target (M8-CKP-01): Build(head, cert, packets) commits one certified slot
 * as a B3 checkpoint for CheckpointV2 (MB1-B3D-02): its height, header
 * hash and state root, and the message_root over the slot's withdrawal
 * packets, by default every 10 B3 blocks or on demand. b3d accepts a valid
 * checkpoint and refuses a forged one (conservation against the vault);
 * clients check one with verify::VerifyCheckpoint (M8-CKP-01, R11).
 * Checkpoint has the fields of verify::CheckpointClaim; M8-CKP-01 moves the
 * one shared value type into core/ (the O-33 pattern), since fm2_core
 * never links fm2_verify.
 *
 * Stub: Build returns NotImplemented(M8-CKP-01).
 */
namespace flowmesh2::bridge {

//! Target shape of a checkpoint (M8-CKP-01).
struct Checkpoint {
    uint64_t height{0};
    uint256 header_hash;
    uint256 state_root;
    uint256 message_root;
};

class CheckpointBuilder
{
public:
    CheckpointBuilder() = delete;

    //! Stub M8-CKP-01.
    static Result<Checkpoint> Build(const state::HeadView& head, const SlotCertificate& cert, std::span<const WithdrawalPacket> packets);
};

} // namespace flowmesh2::bridge

#endif // B3_FLOWMESH2_BRIDGE_CHECKPOINT_H
