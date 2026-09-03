// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_NODE_FINALITY_BINDING_INDEX_H
#define B3COIN_NODE_FINALITY_BINDING_INDEX_H

#include <consensus/params.h>
#include <modern/finality_key.h>
#include <primitives/block.h>
#include <uint256.h>

#include <cstdint>
#include <string>
#include <map>
#include <optional>
#include <vector>

class CBlockIndex;
class CChain;
namespace node { class BlockManager; }

namespace node {

/**
 * Derived consensus state of FINALITY_KEY bindings (plan Commit 4):
 *   validator_key -> BindingRecord {bls_pubkey (0 = revoked), seq, height}
 *   nonzero bls_pubkey -> validator_key that actively holds it
 *
 * Discipline (the stake tracker's): the index is DERIVED from validated
 * blocks and REBUILDABLE. It is mutated only through whole-block operations
 * that keep a per-height undo trail, so ConnectBlock / DisconnectBlock are
 * exact inverses, and a rebuild from the same block sequence yields the same
 * state as incremental maintenance (the reindex property).
 *
 * Every transition handed to ConnectBlock MUST already have passed
 * modern::CheckFinalityKeyTransition against this index's state at that
 * point (the caller verifies in block order); the index does not re-verify
 * cryptography. The production feeder — extracting verified transitions
 * from connected blocks once the evidence carrier exists — is Commit 5;
 * nothing in the tree feeds this index yet.
 *
 * Snapshots: SnapshotActive() returns an immutable copy of the active
 * (non-revoked) bindings. A validator set snapshotted from it is frozen for
 * its epoch; later bindings change only later snapshots (owner rule: binding
 * updates become effective only through the next validator snapshot).
 */
class FinalityBindingIndex
{
public:
    struct Transition {
        modern::ValidatorKeyBytes validator_key{};
        modern::BindingRecord record{}; // record.height is the block height of the transition
    };

    //! Current binding of a validator (revoked records are returned too; see IsRevoked()).
    std::optional<modern::BindingRecord> Get(const modern::ValidatorKeyBytes& validator_key) const;
    //! Active (non-revoked) holder of a nonzero BLS key.
    std::optional<modern::ValidatorKeyBytes> OwnerOf(const modern::BlsPubkeyBytes& bls_pubkey) const;
    //! The lookup shape modern::CheckFinalityKeyTransition expects.
    modern::BlsKeyOwnerLookup OwnerLookup() const;

    //! Apply all transitions of one block, in block order; records an undo
    //! entry for the height. Heights must be strictly increasing.
    void ConnectBlock(int height, const std::vector<Transition>& transitions);
    //! Revert the most recently connected block; `height` must match it.
    void DisconnectBlock(int height);
    //! Forget everything (before a rebuild).
    void Clear();

    int ConnectedHeight() const { return m_heights.empty() ? -1 : m_heights.back(); }
    size_t Size() const { return m_by_validator.size(); }

    //! Immutable copy of the active bindings (validator_key -> bls_pubkey/seq/height).
    std::map<modern::ValidatorKeyBytes, modern::BindingRecord> SnapshotActive() const;
    //! Full state for equality checks (incl. revoked records).
    const std::map<modern::ValidatorKeyBytes, modern::BindingRecord>& All() const { return m_by_validator; }

private:
    struct UndoEntry {
        modern::ValidatorKeyBytes validator_key{};
        std::optional<modern::BindingRecord> previous; // nullopt = the validator had no binding
    };
    void Set(const modern::ValidatorKeyBytes& validator_key, const modern::BindingRecord& record);
    void Restore(const UndoEntry& undo);

    std::map<modern::ValidatorKeyBytes, modern::BindingRecord> m_by_validator;
    std::map<modern::BlsPubkeyBytes, modern::ValidatorKeyBytes> m_owner_of_key;
    std::vector<int> m_heights;                      // connected block heights, in order
    std::vector<std::vector<UndoEntry>> m_undo;      // parallel to m_heights; entries in apply order
};

/**
 * Candidate-local FINALITY_KEY state layered over the confirmed binding
 * index. This is the common state machine used by block verification and by
 * block assembly: a transaction accepted against the confirmed tip may still
 * conflict with another unconfirmed transaction selected earlier for the
 * same candidate block.
 *
 * The overlay is cheap to copy (only candidate transitions are copied), so a
 * block assembler can test a mempool chunk on a copy and commit that copy
 * only when the whole chunk is selected. ApplyTransaction() must only be
 * followed by another call when it returned true.
 */
class FinalityBindingOverlay
{
public:
    FinalityBindingOverlay(const FinalityBindingIndex& base, int height,
                           const uint256& chain_domain)
        : m_base{&base}, m_height{height}, m_chain_domain{chain_domain}
    {
    }

    /** Verify and apply every FINALITY_KEY pair in one transaction. Ordinary
     * transactions are a no-op. On success, `out` contains this transaction's
     * transitions; on failure it is empty and `error` names the rule. */
    bool ApplyTransaction(const CTransaction& tx,
                          std::vector<FinalityBindingIndex::Transition>& out,
                          std::string& error);

private:
    const FinalityBindingIndex* m_base;
    int m_height;
    uint256 m_chain_domain;
    std::map<modern::ValidatorKeyBytes, modern::BindingRecord> m_pending;
    std::map<modern::BlsPubkeyBytes, modern::ValidatorKeyBytes> m_pending_owner;
};

/**
 * Verify every FINALITY_KEY cell+evidence pair of a block (in block / tx /
 * key order) against `index` plus an in-block overlay, and collect the
 * resulting transitions. All-or-nothing: on any failure `out` is left empty
 * and `error` names the first failing rule. The caller applies `out` via
 * FinalityBindingIndex::ConnectBlock only after the whole block is valid.
 * Pure with respect to `index` (const).
 */
bool VerifyBlockFinalityBindings(const CBlock& block, int height, const uint256& chain_domain,
                                 const Consensus::Params& params, const FinalityBindingIndex& index,
                                 std::vector<FinalityBindingIndex::Transition>& out, std::string& error);

/**
 * Keeps the binding index in step with the active chain — the stake
 * tracker's discipline, plus exact undo: BlockConnected applies (verifying),
 * BlockDisconnected reverts the top block exactly, Sync() rebuilds the whole
 * modern span (re-verifying every pair from disk) when not in step.
 */
class FinalityBindingTracker
{
public:
    //! Bring the index to `target` (on `chain`); false if unavailable.
    bool Sync(const CChain& chain, const BlockManager& blockman, const Consensus::Params& params,
              const CBlockIndex& target);
    //! Apply a just-connected block (verifying its pairs); marks dirty when not in step.
    void BlockConnected(const CBlock& block, const CBlockIndex& index, const Consensus::Params& params);
    //! Revert a just-disconnected block exactly; marks dirty when not in step.
    void BlockDisconnected(const CBlockIndex& index);
    void MarkDirty() { m_dirty = true; }
    bool Synced(const uint256& tip_hash) const { return !m_dirty && m_synced_tip == tip_hash; }
    const FinalityBindingIndex& Index() const { return m_index; }

private:
    //! Verify+apply one block; false (and dirty) on failure.
    bool ApplyBlock(const CBlock& block, int height, const Consensus::Params& params);

    FinalityBindingIndex m_index;
    uint256 m_synced_tip{};
    int m_synced_height{-1};
    bool m_dirty{true};
};

} // namespace node

#endif // B3COIN_NODE_FINALITY_BINDING_INDEX_H
