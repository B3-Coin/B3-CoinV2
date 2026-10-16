// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_LEDGER_STATE_COMMITMENT_H
#define B3_FLOWMESH2_LEDGER_STATE_COMMITMENT_H

#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/merkle.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/ledger/ledger.h>
#include <uint256.h>

#include <span>

/** State commitment v0 of the shared ledger (slice R5; O-9).
 *
 * CommitV0 computes the five component roots of core/state_root.h
 * (commitment_version = 0) from a LedgerState plus the two inputs the ledger
 * does not own:
 *   accounts   every account, ascending AccountId, value AccountLeafV0::LeafValue()
 *              of LedgerState::Leaf (the canonical leaf);
 *   orders     every OrderLeafV0 given, ascending OrderId (empty until R7,
 *              whose engine defines the order leaf value);
 *   markets    every MarketDescriptor given, ascending MarketId, value MarketLeafValue;
 *   nullifiers every credited deposit outpoint, ascending, value NullifierLeafValue;
 *   totals     every asset the ledger has seen, ascending AssetId, value
 *              TotalsLeafV0::LeafValue().
 * Each component is MerkleRoot (core/merkle.h: RFC 6962 shape, size-bound)
 * of its values in key order, so the commitment depends only on the state,
 * never on the order of the operations that produced it or of the inputs.
 * The state root is StateRootsV0::Root().
 *
 * Cost: O(state) hashing per call, with no caching. The planned sparse Merkle
 * trees (M2-LED-02, with non-membership proofs and O(changed) updates)
 * replace v0; v0 has no absence proof.
 *
 * Deterministic, no locks, no I/O. Errors: INVALID_INPUT for an invalid or
 * repeated market, a repeated OrderId, or (ProveAccountV0) an absent account;
 * INVARIANT if the ledger holds a non-canonical account.
 */
namespace flowmesh2::ledger {

//! One resting order in the orders component: its id and the leaf value R7 defines.
struct OrderLeafV0 {
    OrderId order;
    uint256 value;
};

//! The five component roots (see the file comment).
Result<StateRootsV0> CommitV0(const LedgerState& ledger, std::span<const MarketDescriptor> markets,
                              std::span<const OrderLeafV0> orders);

//! An account leaf and its inclusion proof under StateRootsV0::accounts.
struct AccountWitnessV0 {
    AccountLeafV0 leaf;
    MerkleProof proof;
};

/** The canonical leaf of `account` and its MerkleProof in the accounts tree
 * of CommitV0 (the accounts tree depends on the ledger alone). A verifier
 * checks MerkleVerify(leaf.LeafValue(), proof, roots.accounts) and
 * roots.Root() == header.state_root. INVALID_INPUT for an absent account.
 */
Result<AccountWitnessV0> ProveAccountV0(const LedgerState& ledger, const AccountId& account);

// ---- Stubs -------------------------------------------------------------------

//! Commitment v1: per-component sparse Merkle trees (commitment_version = 1).
// TODO(rebirth:M2-LED-02) SMT commitment v1 with non-membership proofs and O(changed) updates.
Result<uint256> CommitV1(const LedgerState& ledger);

//! Merkle-sum root of the liabilities of one asset (sums in [0, 2^62]).
// TODO(rebirth:M2-LED-03) Merkle-sum liability tree per asset.
Result<uint256> LiabilitySumRoot(const LedgerState& ledger, const AssetId& asset);

//! exit_value of one account and asset: available + reserved + futures equity, floored at 0.
// TODO(rebirth:M2-LED-04) exit_value per account leaf.
Result<Atoms> ExitValue(const LedgerState& ledger, const AccountId& account, const AssetId& asset);

} // namespace flowmesh2::ledger

#endif // B3_FLOWMESH2_LEDGER_STATE_COMMITMENT_H
