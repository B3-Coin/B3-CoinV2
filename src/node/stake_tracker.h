// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_STAKE_TRACKER_H
#define B3COIN_NODE_STAKE_TRACKER_H

#include <consensus/amount.h>
#include <consensus/params.h>
#include <node/stake_registry.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <utility>
#include <vector>

class CBlockIndex;
class CChain;

namespace node {
class BlockManager;

/**
 * Maintains the modern-era STAKE output set at the active tip, so modern-PoS
 * eligibility (frozen V1 spec section 2) can read a validator's aggregated
 * ACTIVE weight `w` and the total ACTIVE weight `W` without scanning the
 * UTXO set per block.
 *
 * Derived state with a bounded-undo recovery model (B0a):
 *  - the common path is incremental: BlockConnected() applies one block's
 *    STAKE creations and spends when the tracker is synced to its parent;
 *  - every applied block keeps an undo record (the entries it spent and the
 *    outpoints it created) for the last UndoDepth() blocks, and
 *    BlockDisconnected() reverses the tip block exactly with it;
 *  - any other event (restart, a disconnect deeper than the kept history,
 *    a gap) marks the tracker dirty, and the next Sync() rebuilds by walking
 *    the active chain's modern-era blocks from H+1 to the tip. The rebuild
 *    stays the reference: undo must leave exactly the state it would build.
 * REVISABLE_BEFORE_MAINNET: the restart rebuild is still O(modern span)
 * block reads; a persisted registry snapshot (B0b) is the follow-up.
 *
 * Maturity is not tracker state: records carry their creation height and
 * ACTIVE-vs-PENDING is evaluated per query against the evaluation height
 * (IsStakeMature), so no state transition ever needs replaying.
 *
 * Everything here is called under cs_main (validation's connect/disconnect
 * paths and block production); the tracker adds no locking of its own.
 */
class StakeTracker
{
public:
    /**
     * Bring the tracker in sync with `target`, which must lie on `chain`
     * (the connect paths pass the parent of the block being judged: the
     * active tip for ConnectTip/TestBlockValidity, an ancestor during a
     * level-4 VerifyDB reconnect). Cheap when already synced; extends
     * forward incrementally when synced to an ancestor on the same chain;
     * otherwise rebuilds by walking the modern span. Returns false only if
     * a block of the span cannot be read.
     */
    bool Sync(const CChain& chain, const BlockManager& blockman, const Consensus::Params& params,
              const CBlockIndex& target);

    //! Incremental update for a block just connected to the tip whose parent
    //! the tracker is synced to; any mismatch just marks the tracker dirty
    //! (the next Sync() rebuilds).
    void BlockConnected(const CBlock& block, const CBlockIndex& index, const Consensus::Params& params);

    //! Reverse `index`, the block just disconnected from the tip, from the
    //! undo history; marks the tracker dirty (the next Sync() rebuilds) when
    //! the tracker is not synced to `index` or its record is no longer kept.
    void BlockDisconnected(const CBlockIndex& index);

    //! Forget the incremental state: the next Sync() rebuilds.
    void MarkDirty() { m_dirty = true; }

    bool Synced(const uint256& tip_hash) const { return !m_dirty && m_synced_tip == tip_hash; }

    //! How many full rebuilds Sync() has started (tests: a reorg the undo
    //! history covers must not add one).
    uint64_t Rebuilds() const { return m_rebuilds; }
    //! Whether both trackers hold the same STAKE outputs (tests compare an
    //! undone tracker with a rebuilt one).
    bool SameRegistry(const StakeTracker& other) const { return m_stakes == other.m_stakes; }

    //! Blocks of undo history kept: the modern reorganization horizon, or
    //! DEFAULT_UNDO_DEPTH when the horizon is unset. A deeper disconnect
    //! falls back to the rebuild.
    static size_t UndoDepth(const Consensus::Params& params);
    static constexpr size_t DEFAULT_UNDO_DEPTH{1'440};

    //! {w, W}: the validator's aggregated ACTIVE weight and the total ACTIVE
    //! weight, both evaluated at `eval_height` (the height of the block being
    //! judged or produced).
    std::pair<CAmount, CAmount> ActiveWeight(const ValidatorKey& key, int eval_height) const;
    //! Every validator key with ACTIVE (mature) stake at `eval_height` and its
    //! aggregated weight (base units), plus the total. The single source the
    //! validator-set snapshot enumerates from (the same registry block
    //! eligibility uses).
    std::map<ValidatorKey, CAmount> ActiveWeights(int eval_height, CAmount& total) const;

    size_t TrackedOutputs() const { return m_stakes.size(); }

private:
    struct Entry {
        CAmount amount{0};
        ValidatorKey key{};
        int creation_height{0};
        friend bool operator==(const Entry&, const Entry&) = default;
    };
    //! One change to m_stakes, in apply order: `prior` is the entry a spend
    //! removed, or nullopt for an outpoint the block created.
    struct UndoOp {
        COutPoint outpoint;
        std::optional<Entry> prior;
    };
    struct BlockUndo {
        uint256 hash;
        std::vector<UndoOp> ops;
    };

    //! Apply one modern-era block: spends first (a transaction can never
    //! spend an output created later in the same block), then creations.
    //! Keeps the block's undo record, at most `undo_depth` records in all.
    void ApplyBlock(const CBlock& block, const uint256& hash, int height, size_t undo_depth);

    std::map<COutPoint, Entry> m_stakes;
    //! Undo records of the last applied blocks, oldest first; the back one
    //! is the synced tip's.
    std::deque<BlockUndo> m_undo;
    uint256 m_synced_tip{};
    int m_synced_height{-1};
    bool m_dirty{true};
    uint64_t m_rebuilds{0};
};

} // namespace node

#endif // B3COIN_NODE_STAKE_TRACKER_H
