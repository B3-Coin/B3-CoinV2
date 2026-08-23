// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_NODE_FINALITY_BINDING_INDEX_H
#define B3COIN_NODE_FINALITY_BINDING_INDEX_H

#include <modern/finality_key.h>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

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

} // namespace node

#endif // B3COIN_NODE_FINALITY_BINDING_INDEX_H
