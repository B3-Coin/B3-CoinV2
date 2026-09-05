// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_CONSENSUS_FINALITY_SIGNER_RECOVERY_H
#define B3COIN_CONSENSUS_FINALITY_SIGNER_RECOVERY_H

#include <uint256.h>

#include <cstdint>

namespace Consensus {

/**
 * One-time recovery of a finality signer journal whose ancestry lock points
 * at an orphaned checkpoint. The journal move is validator behaviour; the
 * configured recovery anchor must also be enforced by the network as a
 * hardened modern block checkpoint.
 *
 * A validator's durable journal refuses to sign on any branch that does not
 * descend from its last signed checkpoint, and the sole protocol unlock proof
 * is a strictly newer quorum certificate included on the active chain. That
 * proof cannot exist when the locked validators themselves hold the weight
 * the quorum needs: finality then deadlocks even though every node agrees on
 * the active chain. This record pins, per network, exactly one such incident
 * and exactly one agreed recovery anchor on the hardened current chain. A
 * journal that holds precisely the pinned incident vote, and no newer vote,
 * may move ONLY its ancestry lock to the pinned anchor after normal
 * finality-signing depth; the recorded last vote is kept.
 *
 * Everything else fails closed: another chain domain, another height or hash,
 * another epoch or validator set, a journal that differs from the incident in
 * any field, an anchor that is absent from or differs on the active chain, or
 * an anchor not yet buried to checkpoint depth. There is no operator switch,
 * no timeout, and no generic unlock; the pin is compiled into the network's
 * consensus parameters alongside the checkpoint.
 */
struct FinalitySignerRecovery {
    //! The modern chain domain the journal must belong to (wrong network
    //! fails closed even when every other field would match).
    uint256 chain_domain{};
    //! The orphaned vote: the exact checkpoint the journal must hold as both
    //! its last signed checkpoint and its ancestry lock.
    int incident_height{-1};
    uint256 incident_block_hash{};
    uint64_t incident_epoch{0};
    //! The exact signing set and committed successor set of that vote.
    uint256 incident_signing_set_hash{};
    uint256 incident_successor_set_hash{};
    //! The agreed recovery anchor on the current chain: a scheduled
    //! checkpoint strictly above the incident. The next signature must then be
    //! strictly above this height as well.
    int anchor_height{-1};
    uint256 anchor_block_hash{};

    bool Valid() const
    {
        return !chain_domain.IsNull() && incident_height >= 0 &&
               !incident_block_hash.IsNull() &&
               !incident_signing_set_hash.IsNull() &&
               !incident_successor_set_hash.IsNull() &&
               anchor_height > incident_height && !anchor_block_hash.IsNull() &&
               anchor_block_hash != incident_block_hash;
    }
};

} // namespace Consensus

#endif // B3COIN_CONSENSUS_FINALITY_SIGNER_RECOVERY_H
