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

#include <map>
#include <utility>

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
 * Derived state with a deliberately simple V1 recovery model:
 *  - the common path is incremental: BlockConnected() applies one block's
 *    STAKE creations and spends when the tracker is synced to its parent;
 *  - any other event (restart, reorg via BlockDisconnected(), a gap) marks
 *    the tracker dirty, and the next Sync() rebuilds by walking the active
 *    chain's modern-era blocks from H+1 to the tip.
 * REVISABLE_BEFORE_MAINNET: the full-walk rebuild is O(modern span) block
 * reads — fine for the corridor era and early modern span; a persisted
 * registry snapshot is the recorded follow-up before the span grows large.
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

    //! A disconnect (reorg) invalidates incremental state: rebuild lazily.
    void MarkDirty() { m_dirty = true; }

    bool Synced(const uint256& tip_hash) const { return !m_dirty && m_synced_tip == tip_hash; }

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
    };

    //! Apply one modern-era block: spends first (a transaction can never
    //! spend an output created later in the same block), then creations.
    void ApplyBlock(const CBlock& block, int height);

    std::map<COutPoint, Entry> m_stakes;
    uint256 m_synced_tip{};
    int m_synced_height{-1};
    bool m_dirty{true};
};

} // namespace node

#endif // B3COIN_NODE_STAKE_TRACKER_H
