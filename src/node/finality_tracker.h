// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_NODE_FINALITY_TRACKER_H
#define B3COIN_NODE_FINALITY_TRACKER_H

#include <consensus/params.h>
#include <modern/finality_schedule.h>
#include <modern/finality_types.h>
#include <node/finality_binding_index.h>
#include <node/stake_tracker.h>
#include <node/validator_set.h>
#include <primitives/block.h>
#include <uint256.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class CBlockIndex;
class CChain;

namespace node {

class BlockManager;
class BridgeStateIndex;

//! The highest certified checkpoint on the chain (consensus state).
struct FinalizedCheckpoint {
    int height{-1};
    uint256 block_hash{};
    uint64_t epoch{0};
    //! Height of the block whose coinbase carried the certificate.
    int certified_at{-1};
    friend bool operator==(const FinalizedCheckpoint& a, const FinalizedCheckpoint& b)
    {
        return a.height == b.height && a.block_hash == b.block_hash && a.epoch == b.epoch && a.certified_at == b.certified_at;
    }
};

/**
 * Derived finality / epoch state of the active chain (plan Commit 12;
 * normative b3-cross-chain-finality-v1.md section 4, owner rulings
 * 2026-08-23: handover-gated rotation FINAL, E = 1440, MAX_EPOCH_EXTENSION =
 * 7 E, MIN_FINALITY_SET = 2 bootstrap floor as updated 2026-09-01).
 *
 * State machine (all heights are modern-PoS heights; M = first modern-PoS
 * height = ModernPosStartHeight):
 *
 *   bootstrap   at h = M: Set_0 = Snapshot(M-1) stamped epoch 0 and Set_1 =
 *               the same members stamped epoch 1; epoch = 0, epoch_start[0]
 *               = M. If Snapshot(M-1) has fewer than MIN_FINALITY_SET
 *               members there is NO set: no certificate is ever valid on
 *               that chain (fail closed; the bootstrap floor).
 *   Snapshot(b) = ValidatorSetSnapshot::BuildAt over the STAKE registry and
 *               the FINALITY_KEY binding index exactly as of height b
 *               (ACTIVE stake at b, non-revoked binding at b). The tracker
 *               keeps its OWN StakeTracker / FinalityBindingTracker stepped
 *               in lockstep, so the boundary state is always available
 *               without replaying; they are the same classes the rest of the
 *               node uses, so this is one universe, not a second one.
 *   certificate a block at h may carry <= 1 FINALITY_CERT cell + record in
 *               its coinbase; it is judged by modern::JudgeFinalityCertificate
 *               against the epoch state projected for h (schedule, depth,
 *               window {e, e-1}, relation, ancestry, monotone, successor hash,
 *               Set_{fb.epoch} signing, quorum, BLS). Valid => finalized tip =
 *               the checkpoint; if fb.epoch == e => handover certified.
 *   rotation    epoch e+1 begins at the first h >= epoch_start[e] + E such
 *               that a valid epoch-e certificate is included BELOW h
 *               (handover certified). Then Set_{e+1} (known all of epoch e)
 *               becomes current, Set_e previous, Set_{e+2} := Snapshot(h-1)
 *               stamped e+2 (carry-over: Set_{e+1} re-stamped when the
 *               snapshot has fewer than MIN_FINALITY_SET members), epoch_start
 *               [e+1] = h. Reaching the nominal boundary alone never rotates;
 *               no chain can jump from e to e+2; the old set authorizes the
 *               successor before the successor gains authority.
 *   extension   without the handover the epoch extends past epoch_start + E
 *               (checkpoints continue under Set_e). At the first h with
 *               h - epoch_start[e] >= E + MAX_EPOCH_EXTENSION and no
 *               handover the lineage is BROKEN: no further certificate is
 *               valid (only a consensus re-bootstrap rule, not in V1, could
 *               resume it); block production continues under Set_e.
 *
 * Recovery model = StakeTracker's: incremental BlockConnected when in step
 * with the parent; anything else (restart, disconnect, gap) marks dirty and
 * the next Sync() rebuilds by walking the modern span from H+1, so restart,
 * reindex and reconstruction reproduce the identical epoch state and
 * finalized tip. REVISABLE_BEFORE_MAINNET: the full-walk rebuild (with BLS
 * re-verification of every certificate and binding) is O(modern span); a
 * persisted snapshot is the recorded follow-up.
 *
 * All calls under cs_main; the tracker adds no locking.
 */
class FinalityTracker
{
public:
    struct State {
        //! Set_0 exists (the bootstrap floor was met at M).
        bool bootstrapped{false};
        uint64_t epoch{0};
        //! epoch_starts[e] for every e <= epoch; [0] == M. Filled from M on
        //! even when not bootstrapped (the schedule is height-defined).
        std::vector<int> epoch_starts;
        std::shared_ptr<const ValidatorSetSnapshot> previous; // Set_{e-1}
        std::shared_ptr<const ValidatorSetSnapshot> current;  // Set_e
        std::shared_ptr<const ValidatorSetSnapshot> next;     // Set_{e+1}
        bool handover_certified{false};
        bool lineage_broken{false};
        std::optional<FinalizedCheckpoint> finalized;
        //! The epoch view the pure rules consume (pointers into this state).
        modern::FinalityEpochView View() const;
        //! Size of Set_epoch on this chain, for the coinbase matcher.
        std::optional<uint32_t> SetSize(uint64_t epoch) const;
    };

    //! Bring the tracker to `target` (on `chain`); false if a block of the
    //! span cannot be read or a connected block fails re-verification.
    bool Sync(const CChain& chain, const BlockManager& blockman, const Consensus::Params& params,
              const CBlockIndex& target,
              const BridgeStateIndex* bridge_index = nullptr);
    //! Apply a just-connected block when in step with its parent; otherwise dirty.
    void BlockConnected(const CBlock& block, const CBlockIndex& index,
                        const Consensus::Params& params,
                        const BridgeStateIndex* bridge_index = nullptr);
    void BlockDisconnected(const CBlockIndex& index) { MarkDirty(); }
    void MarkDirty() { m_dirty = true; }
    bool Synced(const uint256& tip_hash) const { return !m_dirty && m_synced_tip == tip_hash; }
    int SyncedHeight() const { return m_synced_height; }

    /**
     * Consensus judgement of the certificate (if any) carried by `block`,
     * whose parent is the synced tip: projects the epoch state for
     * index.nHeight (rotation / lineage rules), matches the coinbase cell
     * and record for the certificate's own epoch, and judges it. Does not
     * mutate the tracker. False with a stable reason on failure.
     */
    bool CheckBlockCertificate(const CBlock& block, const CBlockIndex& index, const Consensus::Params& params,
                               std::string& error,
                               const BridgeStateIndex* bridge_index = nullptr) const;
    /**
     * Judge a candidate certificate for the block that would extend `parent`
     * (block assembly, plan Commit 16): the identical consensus rule, run
     * before anything is emitted -- no invalid certificate ever leaves the
     * assembler. The tracker must be synced to `parent`.
     */
    bool JudgeCandidateCertificate(const modern::FinalizedBlock& fb, const modern::FinalityCertificate& cert,
                                   const CBlockIndex& parent, const Consensus::Params& params,
                                   std::string& error,
                                   const BridgeStateIndex* bridge_index = nullptr) const;

    //! The state as of the synced tip.
    const State& Current() const { return m_state; }
    //! The state projected for the block at `height` == SyncedHeight() + 1
    //! (rotation and lineage rules applied; no certificate yet).
    State Projected(int height, const Consensus::Params& params) const;
    //! The set whose weights govern the block at `height` == SyncedHeight()+1
    //! (Commit 14 reads block-production weights here): Set_{e+1} when the
    //! rotation fires at `height`, else Set_e; null when no set exists.
    std::shared_ptr<const ValidatorSetSnapshot> SetInForceAt(int height, const Consensus::Params& params) const;

private:
    //! Advance the state with one modern-PoS block (h >= M); false on a
    //! certificate that fails (a connected block cannot carry one: corruption).
    bool ApplyModern(const CBlock& block, const CBlockIndex& index,
                     const Consensus::Params& params,
                     const BridgeStateIndex* bridge_index);
    //! Step the private stake / binding trackers with one modern-era block.
    bool StepTrackers(const CBlock& block, const CBlockIndex& index, const CChain* chain, const BlockManager* blockman,
                      const Consensus::Params& params);
    //! Snapshot(height) from the private trackers (which must be synced to `height`), stamped `epoch`.
    std::optional<ValidatorSetSnapshot> SnapshotAt(uint64_t epoch, int height) const;
    //! Set_{e+1} for the state whose boundary is `boundary_height`: snapshot or carry-over.
    std::shared_ptr<const ValidatorSetSnapshot> SuccessorSet(const State& s, int boundary_height,
                                                             const Consensus::ModernPosParams& pos) const;
    void Reset();

    State m_state;
    StakeTracker m_stakes;
    FinalityBindingTracker m_bindings;
    uint256 m_synced_tip{};
    int m_synced_height{-1};
    bool m_dirty{true};
};

} // namespace node

#endif // B3COIN_NODE_FINALITY_TRACKER_H
