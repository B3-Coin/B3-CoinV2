// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_NODE_FINALITY_FRESH_LINEAGE_H
#define B3COIN_NODE_FINALITY_FRESH_LINEAGE_H

#include <consensus/params.h>
#include <modern/finality_types.h>
#include <modern/finality_v2.h>
#include <node/validator_set.h>
#include <primitives/block.h>
#include <uint256.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class CBlockIndex;

namespace node {

class BridgeStateIndex;

/**
 * Fresh finality lineage: derived state, projection and consensus judge
 * (b3-bridge-v2-finality-dependency.md sections 6.2-6.5 and 7; the bytes are
 * b3-bridge-v2-contracts-spec.md section 2's, see modern/finality_v2.h).
 *
 * Active only when Consensus::Params::fresh_lineage is set, which only the
 * regtest -b3freshlineageheight knob does; mainnet and testnet stay unset.
 * The state lives next to the V1 state inside node::FinalityTracker (one
 * tracker, one rebuild path) but shares nothing mutable with it: V1 epochs,
 * handover, break and finalized tip are untouched by anything here.
 *
 *   genesis    at h = R_f: GENESIS_HEIGHT G = R_f - 1 (a V1-grid checkpoint,
 *              history leaf 0). Set'_0 = Snapshot(G) with a ruleset-2 header
 *              stamped epoch 0, Set'_1 = the same members stamped epoch 1;
 *              lineageId = TaggedHash("B3/FINALITY/LINEAGE/V2", domain ||
 *              G || hash(G) || setHash(Set'_0)); epoch_starts = {R_f};
 *              history = {historyLeaf(G, hash(G))}. Below the bootstrap
 *              floor (min_finality_set) or without a pinned chain domain the
 *              lineage stays unbootstrapped and no V2 certificate is ever
 *              valid on that chain (fail closed).
 *   rotation   V1's handover-gated rule with V1's E: epoch e+1 begins at the
 *              first h >= epoch_start[e] + E with an epoch-e V2 certificate
 *              already included below h; Set'_{e+2} := Snapshot(h-1) stamped
 *              e+2, or (carry-over) Set'_{e+1}'s members re-stamped e+2 when
 *              the snapshot has fewer than min_finality_set members.
 *   break      without the handover, the first h with h - epoch_start[e] >=
 *              E + fresh_lineage->max_epoch_extension (FD-7, the fresh
 *              lineage's own extension) breaks the lineage for good: no
 *              further V2 certificate is valid (FD-8: no automatic healing).
 *   carrier    a block at h >= R_f may carry <= 1 MPA type-11 record in its
 *              coinbase, judged by modern::JudgeFinalityCertificateV2
 *              against the state projected for h. Valid => fresh finalized
 *              tip = the checkpoint, the history accumulator is extended
 *              through its index, and if fb.epoch == e the handover is
 *              certified.
 *
 * Deviations noted for the design (stage S3 staging scope):
 *  - Committee() of section 10.3 (B-22, the bounded 4-64 member committee)
 *    is not implemented, so the fresh committees are the full V1 snapshots
 *    and the bootstrap floor is V1's min_finality_set. A snapshot with more
 *    than 64 members or total weight below 900 yields certificates that B3
 *    accepts but Verifier V2 treats as not bridge-qualified; RT4 fixtures
 *    stay within 4-64 members and weight >= 900 until B-22 lands.
 *  - Per-epoch keys (B-37) are out of this slice, so the snapshots carry the
 *    V1 FINALITY_KEY binding keys.
 *  - An included V2 certificate pins its checkpoint against reorgs exactly
 *    as a V1 certificate does (Chainstate::RefreshFinalityAnchor and the
 *    connect-time raise in validation.cpp).
 */

using FreshHashAtFn = modern::FreshHashAtFn;
using FreshSnapshotAtFn = std::function<std::optional<ValidatorSetSnapshot>(int)>;

//! One fresh-lineage committee: V1 snapshot members under a ruleset-2
//! header stamped with the fresh epoch. The snapshot is shared, never edited.
struct FreshLineageSet {
    std::shared_ptr<const ValidatorSetSnapshot> members;
    modern::ValidatorSetHeader header{}; // ruleset 2, fresh epoch
    uint256 set_hash{};                  // keccak(header)

    uint64_t Epoch() const { return header.epoch; }
    size_t Size() const { return members ? members->Size() : 0; }
    uint64_t TotalWeight() const { return header.total_weight; }
    //! The verifier's view (keys, weights, quorum) of the members; the
    //! quorum is the ruleset-2 header's, which equals the snapshot's.
    const modern::ValidatorSetView& View() const { return members->View(); }
    std::optional<uint32_t> IndexOf(const modern::ValidatorKeyBytes& validator_key) const
    {
        return members ? members->IndexOf(validator_key) : std::nullopt;
    }
};

//! The fresh committee over `members` for fresh epoch `epoch` (ruleset 2).
std::shared_ptr<const FreshLineageSet> MakeFreshLineageSet(std::shared_ptr<const ValidatorSetSnapshot> members,
                                                           uint64_t epoch);

//! The highest fresh-certified checkpoint on the chain (consensus state).
struct FreshFinalizedCheckpoint {
    int height{-1};
    uint256 block_hash{};
    uint64_t epoch{0};
    uint64_t checkpoint_index{0};
    uint256 history_root{}; // the certificate's checkpointHistoryRoot (leaves 0..index-1)
    uint256 cert_id{};      // keccak(FinalizedBlockV2)
    //! Height of the block whose coinbase carried the certificate.
    int certified_at{-1};
    friend bool operator==(const FreshFinalizedCheckpoint&, const FreshFinalizedCheckpoint&) = default;
};

//! Derived fresh-lineage state of the active chain.
struct FreshLineageState {
    //! Set'_0 exists (bootstrap floor met at R_f, chain domain pinned).
    bool bootstrapped{false};
    int genesis_height{-1};
    uint256 genesis_block_hash{};
    uint256 lineage_id{};
    uint64_t epoch{0};
    //! epoch_starts[e] for every e <= epoch; [0] == R_f.
    std::vector<int> epoch_starts;
    std::shared_ptr<const FreshLineageSet> genesis;  // Set'_0, retained for the verifier bootstrap
    std::shared_ptr<const FreshLineageSet> previous; // Set'_{e-1}
    std::shared_ptr<const FreshLineageSet> current;  // Set'_e
    std::shared_ptr<const FreshLineageSet> next;     // Set'_{e+1}
    bool handover_certified{false};
    bool lineage_broken{false};
    std::optional<FreshFinalizedCheckpoint> finalized;
    //! History accumulator over leaves 0..k (k = the finalized index, or 0
    //! before the first certificate): count == k + 1. Leaves above k are
    //! derived from the chain when a root is needed (FreshHistoryRootAt).
    modern::FinalityHistoryState history;

    //! The epoch view the pure carrier rule consumes (pointers into this state).
    modern::FreshLineageEpochView View() const;
    //! Size of Set'_e (e in {epoch, epoch-1}), for the coinbase matcher.
    std::optional<uint32_t> SetSize(uint64_t e) const;
    //! Set'_e for e in {epoch, epoch-1}; null otherwise.
    std::shared_ptr<const FreshLineageSet> SetForEpoch(uint64_t e) const;
    //! The ruleset-2 hash an epoch-e certificate must name as successor
    //! (hash(Set'_{e+1})), for e in {epoch, epoch-1}.
    std::optional<uint256> SuccessorHashForEpoch(uint64_t e) const;
};

/**
 * Node-changes section 7.4 interface: what the bridge V2 regime reads of the
 * fresh lineage (bootstrapped, broken, epoch, set summaries, finalized
 * height, handover status). Not consumed yet: the bridge V2 burn gate is a
 * later slice, which also needs the section's CertIdAt(index) (not provided).
 */
struct LineageV2View {
    bool configured{false};
    bool bootstrapped{false};
    bool broken{false};
    uint256 lineage_id{};
    int genesis_height{-1};
    uint64_t epoch{0};
    uint256 current_set_hash{};
    uint256 next_set_hash{};
    uint32_t current_count{0};
    uint64_t current_weight{0};
    uint32_t next_count{0};
    uint64_t next_weight{0};
    std::optional<int> finalized_height;
    bool handover_certified{false};
};

LineageV2View MakeLineageV2View(const Consensus::Params& params, const std::optional<FreshLineageState>& state);

/**
 * withdrawalRoot a fresh-lineage certificate must carry at `height`. This
 * slice signs zero at every height: contracts spec section 2.4 asks for zero
 * below V (BRIDGE_ACTIVATION_HEIGHT) and FinalityWithdrawalRootV2(height)
 * from V, but no V exists on B3 before the bridge V2 regime (V will come
 * from the regtest pin file, effective only above the tip when pinned, so
 * already-connected blocks keep this rule on replay). Until that regime
 * replaces this hook, a Verifier V2 must be deployed with a V above every
 * fresh-certified height: a zero root at or above V reverts on Ethereum
 * (verifier step 5) and the verifier could no longer follow rotations.
 */
std::optional<uint256> FreshLineageWithdrawalRoot(int height, const Consensus::Params& params,
                                                  const BridgeStateIndex* bridge_index);
/**
 * bridgeFlags a fresh-lineage certificate must carry at `height`. This
 * slice signs 0 at every height (no sticky FINALITY_CONFLICT exists yet);
 * the bridge V2 regime replaces this hook with BridgeFlagsV2 (node changes
 * sections 7.4 and 8.5), under the same V constraint as the withdrawal root.
 */
std::optional<uint8_t> FreshLineageBridgeFlags(int height, const Consensus::Params& params,
                                               const BridgeStateIndex* bridge_index);

/**
 * The history root a certificate at index k must sign: the root over leaves
 * 0..k-1, leaf i = historyLeaf(G + i * interval, hash_at(G + i * interval)).
 * From the accumulator when k >= history.count (it holds leaves 0..count-1
 * of this chain); a k below it is rebuilt from leaf 0 (O(k)). The carrier
 * rule never asks for such a k: it refuses the regression first
 * (modern::CheckCertificatePlacementV2). nullopt when not bootstrapped,
 * k >= 2^32 or a hash is unavailable.
 */
std::optional<uint256> FreshHistoryRootAt(const FreshLineageState& state, uint64_t k, const FreshHashAtFn& hash_at,
                                          int interval);

/**
 * Project the fresh state for the block at `height` whose parent is
 * `parent_hash` (genesis at R_f, then rotation and break), before any
 * certificate of that block. `snapshot_at(h)` must return Snapshot(h) for
 * h == height - 1 (the tracker's private stake/binding trackers are synced
 * there). No-op when the fresh lineage is not configured; resets the state
 * below R_f.
 */
void ProjectFreshLineage(std::optional<FreshLineageState>& fresh, int height, const uint256& parent_hash,
                         const Consensus::Params& params, const FreshSnapshotAtFn& snapshot_at);

/**
 * The fresh carrier rule for one candidate certificate included at
 * `including_height` on the chain `hash_at` describes, against the state
 * projected for that height. False with a stable "fresh-..." reason.
 */
bool JudgeFreshCertificate(const modern::FinalizedBlockV2& fb, const modern::FinalityCertificate& cert,
                           int including_height, const Consensus::Params& params,
                           const std::optional<FreshLineageState>& projected, const FreshHashAtFn& hash_at,
                           const BridgeStateIndex* bridge_index, std::string& error);

/**
 * Consensus judgement of the type-11 record (if any) in `block`'s coinbase:
 * structural match (at most one record, payload sized for Set'_{epoch}),
 * then JudgeFreshCertificate. A record while the fresh lineage is not
 * configured or not bootstrapped is "fresh-cert-inactive" (below R_f the MPA
 * registry already refuses type 11 as mpa-inactive-type). Does not mutate.
 */
bool JudgeFreshCoinbaseCertificate(const CBlock& block, const CBlockIndex& index, const Consensus::Params& params,
                                   const std::optional<FreshLineageState>& projected, const FreshHashAtFn& hash_at,
                                   const BridgeStateIndex* bridge_index,
                                   std::optional<modern::FinalityCertificateV2Record>& out, std::string& error);

/**
 * Apply an accepted certificate to the projected state of its including
 * block: finalized tip (with certId), history accumulator through the
 * certificate's index, and the handover when fb.epoch == the current epoch.
 * False (state untouched) when a history leaf cannot be derived.
 */
bool ApplyFreshCertificate(FreshLineageState& state, const modern::FinalityCertificateV2Record& record,
                           int including_height, const FreshHashAtFn& hash_at, int interval);

/**
 * The FinalizedBlockV2 an epoch-`epoch` member signs for checkpoint `height`
 * on the chain `hash_at` describes (signer, pool, RPC): lineage id, block
 * hash, withdrawal root and flags from the hooks above, the ruleset-2
 * successor hash, index and history root. nullopt when the state cannot
 * produce one (not bootstrapped, broken, off-grid, index 0, unknown epoch,
 * unavailable hash or root).
 */
std::optional<modern::FinalizedBlockV2> ExpectedFinalizedBlockV2(uint64_t epoch, int height,
                                                                 const FreshLineageState& state,
                                                                 const FreshHashAtFn& hash_at,
                                                                 const Consensus::Params& params,
                                                                 const BridgeStateIndex* bridge_index);

} // namespace node

#endif // B3COIN_NODE_FINALITY_FRESH_LINEAGE_H
