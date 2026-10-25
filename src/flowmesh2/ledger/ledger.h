// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_LEDGER_LEDGER_H
#define B3_FLOWMESH2_LEDGER_LEDGER_H

#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/codes.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/ledger/merkle_levels.h>
#include <flowmesh2/ledger/nonce_window.h>
#include <flowmesh2/ledger/persistent_tree.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string_view>
#include <utility>

/** The shared spot ledger of FlowMesh V2 (slice R5; TRD-10, [CC] C2).
 *
 * One (available, reserved) balance per (AccountId, AssetId) across ALL
 * markets: "deposit once, trade every market". A reservation for an order in
 * one market therefore reduces what every other market can reserve, and two
 * reservations can never spend the same atoms (TRD-10). Each account also
 * holds its D7 nonce window and its count of open orders.
 *
 * Ownership, locks, determinism. LedgerState is a plain value type with no
 * lock: the single writer (state/StateOwner, R9) copies it into a candidate,
 * mutates the copy and publishes it immutably. Every container is ordered
 * and keyed by ids that compare as bytes; there is no clock, I/O, randomness
 * or floating point.
 *
 * Copies (M2-LED-10). The accounts and the nullifier set are persistent
 * trees (ledger/persistent_tree.h): a copy shares them, and a change clones
 * only its path, so the engine's candidate copy costs O(totals + withdrawals),
 * not O(accounts + nullifiers). Seal stores the commitment v0 levels of both
 * (ledger/merkle_levels.h), and the roots of a later copy start from them
 * and the changes since. ForEachChangedAccount and CheckConservationSince let
 * phase 9 check a slot in O(changed) instead of O(accounts).
 *
 * Amounts. Every amount argument must be in [0, MAX_ATOMS]; anything else is
 * INVALID_INPUT. Arithmetic is checked (core/amount.h); a result above
 * MAX_ATOMS is ARITHMETIC_OVERFLOW, never a wrap.
 *
 * Atomicity. Every mutating operation either succeeds completely or returns
 * without changing anything, whatever it returns: an Error, a deterministic
 * rejection (an OutcomeCode other than TRANSFERRED) or INSUFFICIENT/AT_CAP.
 * An Error means the caller broke a precondition (the engine's validity
 * predicate, R7, must exclude it); a rejection is a certified outcome.
 *
 * Canonical state. No account stores an all-zero balance entry (an absent
 * asset means zero), so Leaf() is AccountLeafV0's canonical form and
 * commitment v0 is a function of the balances alone. At most
 * MAX_LEAF_ASSETS assets per account: an operation that would add one more
 * is OVERSIZE (O-27).
 *
 * Conservation, per asset, holds after every operation (CheckConservation;
 * the ledger property tests check it after each operation, the engine
 * checks it once per slot, in phase 9):
 *   sum over accounts of (available + reserved) + pending + fees_held
 *     == deposits - effects                       (core/state_root.h)
 * The legacy profile keeps effects/pending zero. The withdrawal-enabled
 * regtest profile commits every unpaid and paid claim and independently
 * recomputes these totals from that retained obligation map.
 *
 * Accounts exist only after a deposit credits them (CreditDeposit), except
 * the two protocol accounts (seat pool, treasury), which Credit creates.
 * Protocol accounts never hold a key and never receive deposits.
 *
 * PROVISIONAL REGTEST: the TRANSFER rules follow O-7, the fee withholding
 * follows D5, and the duplicate-deposit rule is the outline's simple form
 * (M2-LED-07 replaces it with the frozen model's rules).
 */
namespace flowmesh2::ledger {

//! (INVALID_INPUT) FromLeaves: leaves out of order, repeated or not canonical.
inline constexpr std::string_view LEDGER_LEAVES_ORDER{"ledger leaves out of order or not canonical"};

//! One account's spot balance of one asset.
struct Balance {
    Atoms available{0};
    Atoms reserved{0};

    bool IsZero() const { return available == 0 && reserved == 0; }
    friend bool operator==(const Balance&, const Balance&) = default;
};

//! One account: its balances, its nonce window and its count of open orders.
struct AccountState {
    explicit AccountState(NonceWindow nonces_in) : nonces{std::move(nonces_in)} {}

    std::map<AssetId, Balance> balances; //!< ascending AssetId; never an all-zero entry
    NonceWindow nonces;
    uint32_t open_orders{0}; //!< resting orders (R7); at most profile.max_open_orders_per_account

    //! The balance of `asset`; zero when the account does not list it.
    Balance BalanceOf(const AssetId& asset) const;
    friend bool operator==(const AccountState&, const AccountState&) = default;
};

//! Every account, ascending AccountId; copies share structure.
using AccountMap = PersistentMap<AccountId, AccountState>;
//! Every credited deposit outpoint, ascending; copies share structure.
using NullifierSet = PersistentSet<Outpoint>;
using TotalsMap = std::map<AssetId, AssetTotals>;
using WithdrawalMap = std::map<ActionId, WithdrawalRecord>;
//! Regtest live-obligation bound. Paid records/nullifiers are never pruned.
inline constexpr size_t MAX_PENDING_WITHDRAWALS{4096};

//! Result of a reservation. APPEND-ONLY; zero is never a value.
enum class ReserveResult : uint8_t {
    OK = 1,
    INSUFFICIENT = 2, //!< available < amount; nothing changed (the engine answers REJ_INSUFFICIENT_AVAILABLE)
};
std::string_view ToString(ReserveResult result);

//! Result of opening an order slot. APPEND-ONLY; zero is never a value.
enum class OpenOrderResult : uint8_t {
    OK = 1,
    AT_CAP = 2, //!< already max_open_orders_per_account; nothing changed (REJ_OPEN_ORDER_CAP)
};
std::string_view ToString(OpenOrderResult result);

/** The conservation check over explicit maps (LedgerState::CheckConservation
 * calls it with its own state). INVARIANT if, for any asset, the equation in
 * the file comment fails, a sum leaves [0, MAX_ATOMS], effects exceed
 * deposits, an account stores an all-zero entry, a balance names an asset
 * with no totals, an account has more open orders than `max_open_orders`,
 * or a nonce window is malformed.
 */
Result<void> CheckConservation(const AccountMap& accounts, const TotalsMap& totals, uint32_t max_open_orders);

class LedgerState
{
public:
    /** An empty ledger for `profile`. Validate(profile) first: a non-REGTEST
     * chain is UNSUPPORTED_NETWORK, a structural error INVALID_INPUT, and a
     * profile that turns on an outline stub returns that stub's error.
     */
    static Result<LedgerState> Create(const ProfileParams& profile);

    /** The ledger whose commitment leaves these are (M4-STO-03 snapshots,
     * state/snapshot.h): every account's canonical leaf, ascending
     * AccountId; the nullifier set, ascending; every asset's totals,
     * ascending AssetId, none all-zero; every withdrawal record, ascending
     * ActionId. Create(profile)'s errors first. INVALID_INPUT
     * (LEDGER_LEAVES_ORDER) for leaves out of order or repeated, a leaf that
     * is not canonical, or a nonce list the profile's window does not hold.
     * Then CheckConservation (INVARIANT), so a ledger no operation sequence
     * could reach is refused, as far as the conservation rules can tell.
     */
    static Result<LedgerState> FromLeaves(const ProfileParams& profile, std::span<const AccountLeafV0> accounts,
                                          std::span<const Outpoint> nullifiers,
                                          std::span<const std::pair<AssetId, AssetTotals>> totals,
                                          std::span<const WithdrawalRecord> withdrawals);

    // ---- Reads --------------------------------------------------------------

    const ProfileParams& Profile() const { return m_profile; }
    const AccountId& SeatPool() const { return m_seat_pool; }
    const AccountId& Treasury() const { return m_treasury; }
    bool IsProtocolAccount(const AccountId& account) const;

    const AccountMap& Accounts() const { return m_accounts; }
    //! nullptr if the account does not exist.
    const AccountState* Find(const AccountId& account) const;
    bool HasAccount(const AccountId& account) const { return Find(account) != nullptr; }
    //! Zero for an absent account or asset.
    Balance BalanceOf(const AccountId& account, const AssetId& asset) const;

    //! Every credited deposit outpoint (the nullifier set), ascending.
    const NullifierSet& Nullifiers() const { return m_nullifiers; }
    bool Credited(const Outpoint& outpoint) const { return m_nullifiers.contains(outpoint); }

    //! Totals of every asset the ledger has seen, ascending; never all-zero.
    const TotalsMap& Totals() const { return m_totals; }
    //! All-zero for an asset the ledger has not seen.
    AssetTotals TotalsOf(const AssetId& asset) const;

    /** The canonical commitment v0 leaf of `account` (core/account_leaf.h):
     * non-zero balances ascending, used nonces ascending, open orders.
     * INVALID_INPUT if the account does not exist.
     */
    Result<AccountLeafV0> Leaf(const AccountId& account) const;

    //! CheckConservation over this ledger (see the free function).
    Result<void> CheckConservation() const;

    /** CheckConservation of a ledger derived from `parent`, which must pass
     * CheckConservation itself (induction: the engine runs this in phase 9,
     * and every restored head and snapshot runs the full check). The account
     * rules are checked on each changed account only, and each asset's
     * equation from the parent's totals and the changed accounts' balances:
     * O(changed + assets + withdrawals) instead of O(accounts). INVARIANT
     * where CheckConservation would fail, and for a parent under another
     * profile, a parent whose totals are not conserved or a removed account.
     */
    Result<void> CheckConservationSince(const LedgerState& parent) const;

    /** visit(account, before, after) for every account whose state differs
     * from its state in `before`, ascending AccountId; `before` is nullptr
     * for an account `before` lacks (the ledger removes none, so `after` is
     * never nullptr for a ledger derived from `before`). Subtrees the two
     * ledgers share are skipped (AccountMap::Diff).
     */
    template <class Visit>
    void ForEachChangedAccount(const LedgerState& before, Visit&& visit) const
    {
        AccountMap::Diff(before.m_accounts, m_accounts, [&visit](const AccountMap::value_type* was, const AccountMap::value_type* now) {
            visit(now != nullptr ? now->first : was->first, was != nullptr ? &was->second : nullptr,
                  now != nullptr ? &now->second : nullptr);
            return true;
        });
    }

    // ---- Commitment v0 components (ledger/state_commitment.h) -----------------

    /** MerkleRoot of AccountLeafV0::LeafValue() of every account, ascending
     * AccountId: the accounts component. From the levels the last Seal stored
     * and the accounts changed since, or from every leaf when none are
     * stored. INVARIANT for an account that is not canonical.
     */
    Result<uint256> AccountsRootV0() const;
    //! MerkleRoot of NullifierLeafValue of every nullifier, ascending: the nullifiers component.
    uint256 NullifiersRootV0() const;
    //! The accounts component's inclusion proof of `account`; INVALID_INPUT if it has no account.
    Result<MerkleProof> ProveAccountLeafV0(const AccountId& account) const;

    /** Store the levels of both components for the current state, so the
     * roots of this ledger and of its later copies start from them. Changes
     * no value: operator== and every read answer the same afterwards.
     * FromLeaves seals, and the engine seals every slot's state before it
     * commits it. Errors as AccountsRootV0, with nothing stored.
     */
    Result<void> Seal();

    // ---- Facts --------------------------------------------------------------

    /** Credit one deposit fact (engine phase 1): creates the account if it is
     * new and records fact.outpoint in the nullifier set. Refusals, all with
     * no change: amount outside [1, MAX_ATOMS] or a protocol account as the
     * target (INVALID_INPUT, as the frozen model's BAD_DEPOSIT_TARGET); an
     * outpoint already credited (INVALID_INPUT: a duplicate is never credited
     * twice); deposits of the asset above MAX_ATOMS (ARITHMETIC_OVERFLOW); a
     * new asset beyond MAX_LEAF_ASSETS (OVERSIZE). The fact's cursor is not
     * checked here; the slot validity predicate (R7) owns the cursor range.
     */
    // TODO(rebirth:M2-LED-07) model-exact duplicate-fact rules: identical facts collapse, contradictory ones refuse.
    Result<void> CreditDeposit(const DepositFact& fact);

    // ---- Spot balance primitives (used by the engine, R6/R7) ---------------

    /** Move `amount` of `asset` from available to reserved. INSUFFICIENT and
     * no change if available < amount. INVALID_INPUT for an unknown account.
     */
    Result<ReserveResult> Reserve(const AccountId& account, const AssetId& asset, Atoms amount);

    //! Move `amount` back from reserved to available; INVALID_INPUT if reserved < amount.
    Result<void> Release(const AccountId& account, const AssetId& asset, Atoms amount);

    /** Settle one leg of a fill: `from` pays `amount` out of its reserved
     * `asset`; `to` receives amount - withheld_fee in available; the
     * withheld fee joins TotalsOf(asset).fees_held until Credit distributes it
     * (D5: the seller pays the fee in the quote asset, so the engine passes
     * the fee increment on the quote leg and 0 on the base leg). `from` may
     * equal `to` (self-trade, O-17). INVALID_INPUT for an unknown account,
     * withheld_fee > amount or reserved < amount.
     */
    Result<void> PayFromReserved(const AccountId& from, const AccountId& to, const AssetId& asset, Atoms amount,
                                 Atoms withheld_fee);

    /** Credit `amount` of the fees held for `asset` to a protocol account
     * (engine phase 8: floor(seat_share_percent) to the seat pool, the rest to
     * the treasury). Creates the protocol account on its first non-zero
     * credit; a zero credit changes nothing. INVALID_INPUT for any other
     * account and for amount > fees_held.
     */
    Result<void> Credit(const AccountId& account, const AssetId& asset, Atoms amount);

    /** TRANSFER (engine phase 2; O-7, PROVISIONAL): same AssetId, available
     * only, no fee. Deterministic rejections, all with no change:
     * REJ_BAD_PARAMS for from == to or amount == 0, then REJ_NO_RECIPIENT
     * for a recipient without an account, then REJ_INSUFFICIENT_AVAILABLE.
     * Success is TRANSFERRED. INVALID_INPUT for an unknown sender (admission
     * and the validity predicate require it) or an amount outside the range.
     */
    Result<OutcomeCode> Transfer(const AccountId& from, const AccountId& to, const AssetId& asset, Atoms amount);

    /** Consume `nonce` in the account's window (every executed action does,
     * including deterministic rejections, DUR-5). INVALID_INPUT for an
     * unknown account or a nonce the window does not accept.
     */
    Result<void> ConsumeNonce(const AccountId& account, uint64_t nonce);

    //! One more resting order; AT_CAP and no change at max_open_orders_per_account.
    Result<OpenOrderResult> AddOpenOrder(const AccountId& account);
    //! One resting order fewer; INVALID_INPUT at zero or for an unknown account.
    Result<void> RemoveOpenOrder(const AccountId& account);

    //! Exact authenticated action identity supplied by the engine; principal
    //! moves available -> pending, service fee moves available -> fees_held.
    //! Repeating the same obligation consumes no funds; conflicting reuse
    //! refuses. The engine owns signatures/nonces/slot and domain validation.
    Result<OutcomeCode> RequestWithdrawal(const ActionId& id, const AccountId& account,
                                           const WithdrawPayload& withdrawal, uint64_t slot);
    //! The trusted chain adapter/engine validates external receipt and domain
    //! first. Match every obligation field, preserve paid status/event forever.
    Result<void> ConfirmWithdrawal(const PayoutConfirmedFact& payout);
    const WithdrawalMap& Withdrawals() const { return m_withdrawals; }

    // ---- Stubs ----------------------------------------------------------------

    //! WITHDRAW into a pending reservation with a capacity bound (TRD-21).
    // TODO(rebirth:M2-LED-05) pending withdrawal reservation; conservation counts it in `pending`.
    Result<void> RequestWithdrawal(const AccountId& account, const WithdrawPayload& withdrawal);
    //! SPOT_TO_FUTURES and FUTURES_TO_SPOT cash transfers with a risk gate (TRD-13).
    // TODO(rebirth:M2-LED-06) spot/futures cash transfers.
    Result<void> TransferToFutures(const AccountId& account, const AssetId& asset, Atoms amount);
    Result<void> TransferFromFutures(const AccountId& account, const AssetId& asset, Atoms amount);

    //! Every value member; never the stored levels, which Seal adds without changing the state.
    friend bool operator==(const LedgerState& a, const LedgerState& b)
    {
        return a.m_profile == b.m_profile && a.m_seat_pool == b.m_seat_pool && a.m_treasury == b.m_treasury &&
               a.m_empty_window == b.m_empty_window && a.m_accounts == b.m_accounts && a.m_nullifiers == b.m_nullifiers &&
               a.m_totals == b.m_totals && a.m_withdrawals == b.m_withdrawals;
    }

private:
    //! The commitment v0 levels of `accounts` and `nullifiers` (Seal).
    struct CommitmentLevels;

    LedgerState(ProfileParams profile, AccountId seat_pool, AccountId treasury, NonceWindow empty_window);

    AccountState* FindMutable(const AccountId& account);
    void PutTotals(const AssetId& asset, const AssetTotals& totals);
    Result<AccountLeafV0> LeafOf(const AccountId& account, const AccountState& state) const;
    //! The withdrawal half of CheckConservation (obligations against totals).
    Result<void> CheckWithdrawalObligations() const;
    Result<MerkleLevels> AccountLevels() const;
    MerkleLevels NullifierLevels() const;

    ProfileParams m_profile;
    AccountId m_seat_pool;
    AccountId m_treasury;
    NonceWindow m_empty_window; //!< the window every new account starts with
    // A new value member joins operator== above.
    AccountMap m_accounts;
    NullifierSet m_nullifiers;
    TotalsMap m_totals;
    WithdrawalMap m_withdrawals;
    std::shared_ptr<const CommitmentLevels> m_levels; //!< from Seal; shared by copies, never written
};

} // namespace flowmesh2::ledger

#endif // B3_FLOWMESH2_LEDGER_LEDGER_H
