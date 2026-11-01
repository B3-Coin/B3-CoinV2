// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_NODE_FINALITY_BINDING_INDEX_H
#define B3COIN_NODE_FINALITY_BINDING_INDEX_H

#include <consensus/params.h>
#include <modern/finality_key.h>
#include <primitives/block.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <string>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
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
    //! A binding's 96-byte BLS proof of possession (all zero for a revocation).
    using PopBytes = std::array<unsigned char, modern::BLS_SIGNATURE_SIZE>;
    struct Transition {
        modern::ValidatorKeyBytes validator_key{};
        modern::BindingRecord record{}; // record.height is the block height of the transition
        //! B0b: the proof of possession the transition carried, kept per key
        //! (KeyPops) so that a persisted index can re-verify every key.
        PopBytes pop{};
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

    //! The whole index, undo trail included, as one canonical byte string
    //! (B0b persistence). The key owners are not encoded: Decode derives them.
    std::vector<unsigned char> Encode() const;
    //! Inverse of Encode for an index whose blocks lie in [first_height,
    //! last_height]. Nullopt unless `bytes` is exactly one encoding of an
    //! index that ConnectBlock could have built: block heights strictly
    //! increasing; for each validator, its first undo entry without a
    //! previous record, each later one's previous record set at the block
    //! of the entry before it, and its current record set at the block of
    //! its last entry; no nonzero key held by two validators.
    static std::optional<FinalityBindingIndex> Decode(std::span<const unsigned char> bytes, int first_height,
                                                      int last_height);
    //! Equal records, key owners and undo trail. The proofs are not
    //! compared (an index decoded from Encode() has none): see KeyPops().
    bool SameState(const FinalityBindingIndex& other) const;

    //! B0b (P1): every nonzero key that a record holds, or held before a
    //! transition of the undo trail, with the proof of possession its
    //! binding carried, in key order. These are the keys a validator set
    //! built from this index can hold, now or after an undo. Nullopt when
    //! a key has no proof (an index built or decoded without them).
    std::optional<std::vector<std::pair<modern::BlsPubkeyBytes, PopBytes>>> KeyPops() const;
    //! KeyPops() as one canonical byte string, kept apart from Encode():
    //! the chainstate's record stores no proofs. Nullopt as KeyPops().
    std::optional<std::vector<unsigned char>> EncodePops() const;
    //! Take the proofs of an index decoded by Decode(). True only when
    //! `bytes` is exactly EncodePops() of these keys and proofs: ascending
    //! keys, each one this index holds, none missing, no zero proof. On
    //! false the index is unchanged. No proof is verified here.
    bool DecodePops(std::span<const unsigned char> bytes);

private:
    struct UndoEntry {
        modern::ValidatorKeyBytes validator_key{};
        std::optional<modern::BindingRecord> previous; // nullopt = the validator had no binding
        friend bool operator==(const UndoEntry&, const UndoEntry&) = default;
    };
    void Set(const modern::ValidatorKeyBytes& validator_key, const modern::BindingRecord& record);
    void Restore(const UndoEntry& undo);
    //! The keys KeyPops() names.
    std::set<modern::BlsPubkeyBytes> HeldKeys() const;

    std::map<modern::ValidatorKeyBytes, modern::BindingRecord> m_by_validator;
    std::map<modern::BlsPubkeyBytes, modern::ValidatorKeyBytes> m_owner_of_key;
    std::vector<int> m_heights;                      // connected block heights, in order
    std::vector<std::vector<UndoEntry>> m_undo;      // parallel to m_heights; entries in apply order
    //! The proof each nonzero key arrived with, from a transition or from
    //! DecodePops(). A valid proof is unique to its key, so the first kept
    //! is the only one; a key an undo released keeps its entry.
    std::map<modern::BlsPubkeyBytes, PopBytes> m_pops;
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

    //! B0b: a digest of every consensus parameter this tracker's derivation
    //! reads. A persisted state is used only under the same digest.
    static uint256 ParamsDigest(const Consensus::Params& params);
    //! B0b: take `index`, the state persisted at `block` and checked by the
    //! caller, as the state there; the tracker is then in step at `block`.
    void Install(FinalityBindingIndex index, const CBlockIndex& block);
    //! Rebuilds from scratch (Sync's fallback) so far, for tests.
    uint64_t Rebuilds() const { return m_rebuilds; }

private:
    //! Verify+apply one block; false (and dirty) on failure.
    bool ApplyBlock(const CBlock& block, int height, const Consensus::Params& params);

    FinalityBindingIndex m_index;
    uint256 m_synced_tip{};
    int m_synced_height{-1};
    bool m_dirty{true};
    uint64_t m_rebuilds{0};
};

} // namespace node

#endif // B3COIN_NODE_FINALITY_BINDING_INDEX_H
