// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_LEDGER_STATE_COMMITMENT_V1_H
#define B3_FLOWMESH2_LEDGER_STATE_COMMITMENT_V1_H

#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/core/state_root_v1.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/ledger/smt_tree.h>
#include <flowmesh2/ledger/state_commitment.h>
#include <uint256.h>

#include <span>

/** State commitment v1 of the shared ledger (M2-LED-02; PROVISIONAL
 * REGTEST, ARCHITECTURE.md O-96).
 *
 * The six components of core/state_root_v1.h, each a compact sparse Merkle
 * tree (core/smt.h, ledger/smt_tree.h) of the same leaf values commitment
 * v0 commits to. Unlike v0, a key's position does not depend on the other
 * keys, so a change costs one path, about log2(n) hashes, and an absent key
 * has a proof (AccountProofV1 ABSENT).
 *
 * Not live: the engine commits v0, and which profiles switch, with which
 * certificate format, is BLOCKED-ON-RULING (engine::EngineState::CommitV1).
 * These functions are the ledger half of the switch, tested against
 * core/smt.h's reference functions.
 *
 * Deterministic, no locks, no I/O. Errors as CommitV0: INVALID_INPUT for an
 * invalid or repeated market or a repeated OrderId, INVARIANT for a
 * non-canonical account or withdrawal or withdrawals in a profile without
 * them.
 */
namespace flowmesh2::ledger {

//! The accounts tree of `ledger`: path AccountPathV1, value AccountLeafV0::LeafValue(). O(accounts).
Result<SmtTree> AccountsTreeV1(const LedgerState& ledger);

/** `tree`, the accounts tree of `before`, becomes the accounts tree of
 * `after`, a ledger derived from `before`: one path per account
 * ForEachChangedAccount reports, O(changed log n). INVARIANT for a
 * non-canonical changed account, with `tree` unchanged. The caller owns
 * the precondition that `tree` is the tree of `before`.
 */
Result<void> UpdateAccountsTreeV1(SmtTree& tree, const LedgerState& before, const LedgerState& after);

//! The nullifiers tree: path SmtKeyPath(NULLIFIERS, txid || u32 LE vout), value NullifierLeafValue. O(nullifiers).
SmtTree NullifiersTreeV1(const LedgerState& ledger);

//! The nullifiers path of `outpoint`.
uint256 NullifierPathV1(const Outpoint& outpoint);

/** The six roots of commitment v1, from `accounts` (the accounts tree of
 * `ledger`, as AccountsTreeV1 or UpdateAccountsTreeV1 give it) and the
 * ledger's other components. The other components are built here,
 * O(orders + markets + nullifiers + assets + withdrawals).
 */
Result<StateRootsV1> CommitV1(const LedgerState& ledger, const SmtTree& accounts, std::span<const MarketDescriptor> markets,
                              std::span<const OrderLeafV0> orders);

//! CommitV1 with the checks of CommitTradingStateV0 on the resting orders.
Result<StateRootsV1> CommitTradingStateV1(const LedgerState& ledger, const SmtTree& accounts,
                                          std::span<const MarketDescriptor> markets,
                                          std::span<const RestingOrderLeafV0> orders);

/** The v1 proof of `account` in `accounts` (the accounts tree of `ledger`):
 * PRESENT with the canonical leaf if the ledger has the account, ABSENT
 * otherwise. INVARIANT if the tree and the ledger disagree on the account.
 */
Result<AccountProofV1> ProveAccountV1(const LedgerState& ledger, const SmtTree& accounts, const AccountId& account);

} // namespace flowmesh2::ledger

#endif // B3_FLOWMESH2_LEDGER_STATE_COMMITMENT_V1_H
