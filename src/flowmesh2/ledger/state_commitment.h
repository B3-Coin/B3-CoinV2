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
 *   orders     every OrderLeafV0 given, ascending OrderId; since R7 the
 *              value is RestingOrderLeafV0::LeafValue() (core/state_root.h)
 *              of each resting order (CommitTradingStateV0);
 *   markets    every MarketDescriptor given, ascending MarketId, value MarketLeafValue;
 *   nullifiers every credited deposit outpoint, ascending, value NullifierLeafValue;
 *   totals     every asset the ledger has seen, ascending AssetId, value
 *              TotalsLeafV0::LeafValue().
 * Each component is MerkleRoot (core/merkle.h: RFC 6962 shape, size-bound)
 * of its values in key order, so the commitment depends only on the state,
 * never on the order of the operations that produced it or of the inputs.
 * The state root is StateRootsV0::Root().
 *
 * Cost (M2-LED-10): the accounts and nullifiers roots come from the levels
 * the ledger stored at its last Seal (LedgerState::AccountsRootV0), so a
 * changed account costs O(log n) hashes and a key inserted at position p
 * about n - p (v0's sorted leaves shift); orders, markets, totals and
 * withdrawals are still hashed whole, O(orders + markets + assets +
 * withdrawals). Commitment v1 (ledger/state_commitment_v1.h, M2-LED-02)
 * puts each component in a sparse Merkle tree with absence proofs and
 * O(changed) updates; it is built but not live. v0 has no absence proof.
 *
 * Deterministic, no locks, no I/O. Errors: INVALID_INPUT for an invalid or
 * repeated market, a repeated OrderId, or (ProveAccountV0) an absent account;
 * INVARIANT if the ledger holds a non-canonical account.
 */
namespace flowmesh2::ledger {

//! One resting order in the orders component: its id and its leaf value.
struct OrderLeafV0 {
    OrderId order;
    uint256 value;
};

//! The five component roots (see the file comment).
Result<StateRootsV0> CommitV0(const LedgerState& ledger, std::span<const MarketDescriptor> markets,
                              std::span<const OrderLeafV0> orders);

//! The orders-component entry of one resting order: {leaf.order, leaf.LeafValue()}.
OrderLeafV0 OrderLeafOf(const RestingOrderLeafV0& leaf);

/** The commitment of the whole trading state (slice R7): CommitV0 whose
 * markets component is `markets` (every market of the instance, books empty
 * or not) and whose orders component is OrderLeafOf of every resting order.
 * Besides CommitV0's errors, INVALID_INPUT for an order leaf that is not
 * canonical (RestingOrderLeafV0::IsCanonical), for an order whose market is
 * not in `markets`, and for an order whose owner has no account in the
 * ledger. The engine (engine/engine.h EngineState::Commit) calls it with its
 * books; b3verify (R11) recomputes the same leaf values from core/ alone.
 */
Result<StateRootsV0> CommitTradingStateV0(const LedgerState& ledger, std::span<const MarketDescriptor> markets,
                                          std::span<const RestingOrderLeafV0> orders);

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

//! A canonical withdrawal record and inclusion proof under the v2 sixth root.
struct WithdrawalWitness {
    WithdrawalRecord record;
    MerkleProof proof;
};
//! INVALID_INPUT for a disabled profile or absent claim. No absence proof.
Result<WithdrawalWitness> ProveWithdrawal(const LedgerState& ledger, const ActionId& id);

// ---- Stubs -------------------------------------------------------------------

//! Merkle-sum root of the liabilities of one asset (sums in [0, 2^62]).
// TODO(rebirth:M2-LED-03) Merkle-sum liability tree per asset.
Result<uint256> LiabilitySumRoot(const LedgerState& ledger, const AssetId& asset);

//! exit_value of one account and asset: available + reserved + futures equity, floored at 0.
// TODO(rebirth:M2-LED-04) exit_value per account leaf.
Result<Atoms> ExitValue(const LedgerState& ledger, const AccountId& account, const AssetId& asset);

} // namespace flowmesh2::ledger

#endif // B3_FLOWMESH2_LEDGER_STATE_COMMITMENT_H
