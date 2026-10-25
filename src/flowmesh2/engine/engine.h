// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_ENGINE_ENGINE_H
#define B3_FLOWMESH2_ENGINE_ENGINE_H

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/engine/book.h>
#include <flowmesh2/ledger/ledger.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

/** The slot engine of FlowMesh V2 (slice R7; ARCHITECTURE.md sections 3 and
 * 11; plan section 3.5). PROVISIONAL REGTEST (D4, D5, D6, D7).
 *
 * Contents: the canonical order of a slot's originals, the phase each
 * action kind executes in, the trading state the engine transforms
 * (EngineState) and its parent head (HeadView), the slot validity
 * predicate (ValidateSlotInputs), whose only product is a ValidatedSlot,
 * and Engine::ApplySlot, which executes a ValidatedSlot in nine phases and
 * returns the candidate state, the certified outcomes and the roots.
 *
 * Canonical order (O-4, O-4b). The originals of a slot at height h whose
 * parent header hash is p are first sorted by
 *   OrderKey(p, h, id) = Tagged(ORDER, p || u64 LE h || ActionId),
 * ties (only possible for a SHA-256 collision) by ActionId. Then every
 * account's positions in that sequence are given to its own actions in
 * ascending nonce. So the order across accounts depends on the parent
 * header, which no client knows when it signs, while one account's actions
 * always execute in nonce order. Plan section 3.5 seeds with the parent
 * CERTIFICATE hash; a BLS certificate is not unique (every quorum subset
 * aggregates differently), so a leader could grind it. The outline seeds
 * with the parent HEADER hash and the height instead; the owner ratifies or
 * replaces this under D4 (M3-ENG-12).
 *
 * Determinism: tagged SHA-256, ordered containers, no clock, I/O or
 * randomness. Nothing here takes a lock.
 */
namespace flowmesh2::engine {

//! What the canonical order needs to know about one original.
struct OrderingEntry {
    ActionId id;
    AccountId account;
    uint64_t nonce{0};
    friend bool operator==(const OrderingEntry&, const OrderingEntry&) = default;
};

//! Tagged(ORDER, parent_header_hash || u64 LE height || ActionId).
uint256 OrderKey(const uint256& parent_header_hash, uint64_t height, const ActionId& id);

/** The canonical order of `originals` in the slot at `height` whose parent
 * header hash is `parent_header_hash` (see the file comment). The result has
 * one entry per original: result[r] is the index into `originals` of the
 * action at canonical rank r. It does not depend on the order of the span.
 * INVALID_INPUT for a repeated ActionId or a repeated (account, nonce); the
 * validity predicate (ValidateSlotInputs) refuses both first.
 */
// TODO(rebirth:M3-ENG-12) seed hardening: grinding tests; ratify the parent-header seed or replace it (VRF, unique threshold signature).
Result<std::vector<size_t>> CanonicalOrder(std::span<const OrderingEntry> originals, const uint256& parent_header_hash,
                                           uint64_t height);

//! CanonicalOrder over verified actions: {Id(), Account(), Action().nonce}.
Result<std::vector<size_t>> CanonicalOrder(std::span<const VerifiedAction> originals, const uint256& parent_header_hash,
                                           uint64_t height);

/** The phases of one slot (plan section 3.5). APPEND-ONLY registry; zero is
 * never a value. Engine::ApplySlot runs them in this order; inside a phase,
 * actions run in canonical order.
 */
enum class SlotPhase : uint8_t {
    DEPOSITS = 1,     //!< chain facts: deposits credit accounts
    ACCOUNT = 2,      //!< TRANSFER (later WITHDRAW, M2-LED-05)
    CANCELS = 3,      //!< CANCEL by OrderId (later CANCEL_ALL, M3-ENG-09)
    POST_ONLY = 4,    //!< PLACE POST_ONLY: rest, or REJ_WOULD_CROSS
    TAKERS = 5,       //!< PLACE GTC/IOC: reserve all, then one A2 sweep per market
    REST = 6,         //!< GTC remainders rest; IOC and refused remainders are released
    FUTURES = 7,      //!< futures sweep (M9-FUT-04); only with futures_enabled
    FEES = 8,         //!< one FEES_DISTRIBUTED outcome per quote asset
    CONSERVATION = 9, //!< conservation and slot-boundary invariants, then roots
};
bool IsKnown(SlotPhase phase);
//! Registry name; "UNKNOWN" for an unregistered value.
std::string_view ToString(SlotPhase phase);

/** The phase in which `action` executes: TRANSFER in ACCOUNT, CANCEL in
 * CANCELS, a POST_ONLY PLACE in POST_ONLY, a GTC or IOC PLACE in TAKERS. A
 * PLACE whose payload does not decode returns the decoder's error. The
 * other kinds are the engine branches the outline leaves as stubs; the
 * validity predicate refuses them first (KindSupported), so a validated
 * slot never reaches them:
 * - CANCEL_ALL: NotImplemented(M3-ENG-09) (phase CANCELS once done);
 * - WITHDRAW: NotImplemented(M2-LED-05) (phase ACCOUNT once done);
 * - SPOT_TO_FUTURES, FUTURES_TO_SPOT: NotImplemented(M2-LED-06).
 * INVALID_INPUT for an unregistered kind.
 */
Result<SlotPhase> PhaseOf(const ActionV2& action);
Result<SlotPhase> PhaseOf(const ActionV2& action, const ProfileParams& profile);

/** The arrival_rank (engine/book.h) of an order that starts resting in the
 * slot at `height`, in `phase` (POST_ONLY or REST), at canonical `rank`:
 *   (2 * height + (phase == REST ? 1 : 0)) * max_actions_per_slot + rank.
 * Ranks therefore grow strictly over the life of every market: by slot,
 * then post-only placements before resting remainders, then canonical rank
 * (O-35, O-39). A post-only order rests in phase 4, before a same-slot
 * remainder in phase 6 at the same level, so it must queue ahead of it even
 * with a higher canonical rank. INVALID_INPUT for another phase, a zero cap
 * or rank >= max_actions_per_slot; ARITHMETIC_OVERFLOW above 2^64-1.
 * PROVISIONAL: the stride assumes a fixed max_actions_per_slot; changing it
 * at a versioned height (M7-GOV-03) must keep the ranks increasing.
 */
Result<uint64_t> ArrivalRank(uint64_t height, SlotPhase phase, uint64_t rank, uint32_t max_actions_per_slot);

// ---- The trading state and the parent head -----------------------------------

/** The trading state the engine transforms: the shared spot ledger
 * (ledger/ledger.h) and one book per market (engine/book.h). A value type
 * with no lock: the single writer (state/StateOwner, R9) copies it into a
 * candidate, Engine::ApplySlot mutates the copy, and the result is published
 * immutably. Markets are fixed at genesis (D6; listings are M7-GOV-01).
 */
/** A state that passed the slot-boundary invariants (EngineState::Verify,
 * M2-LED-10): its ledger and the owners of its resting orders, all phase 9
 * needs to check a later state by its changes. Only Verify makes one, right
 * after the check, so every basis satisfies the invariants; the ledger copy
 * shares the checked state's trees.
 */
class VerifiedBasis
{
public:
    const ledger::LedgerState& Ledger() const { return m_ledger; }
    //! Every owner of a resting order of the checked state.
    const std::set<AccountId>& Owners() const { return m_owners; }

private:
    friend struct EngineState;
    VerifiedBasis(ledger::LedgerState ledger, std::set<AccountId> owners) : m_ledger{std::move(ledger)}, m_owners{std::move(owners)} {}

    ledger::LedgerState m_ledger;
    std::set<AccountId> m_owners;
};

struct EngineState {
    ledger::LedgerState ledger;
    BookSet books;
    /** The last state Verify checked: this one, or an ancestor that copies
     * and changes led from. Shared by copies, never part of the value
     * (operator== ignores it). Null when nothing was verified.
     */
    std::shared_ptr<const VerifiedBasis> verified;

    /** The state at genesis: LedgerState::Create(profile) and one empty book
     * per market (BookSet::Create); their errors propagate.
     */
    static Result<EngineState> Genesis(const ProfileParams& profile, std::span<const MarketDescriptor> markets);

    //! Every market, ascending MarketId.
    std::vector<MarketDescriptor> Markets() const;
    //! Every resting order as its commitment leaf: ascending MarketId, then book order.
    std::vector<RestingOrderLeafV0> OrderLeaves() const;
    //! State commitment v0: ledger::CommitTradingStateV0(ledger, Markets(), OrderLeaves()).
    Result<StateRootsV0> Commit() const;

    /** The invariants that hold at every slot boundary; INVARIANT if one
     * fails (engine phase 9 checks them before any candidate exists):
     * 1. ledger conservation (LedgerState::CheckConservation, O-31);
     * 2. every book's invariants and no OrderId in two books;
     * 3. no fees held: phase 8 distributed every withheld fee (O-31);
     * 4. every resting order's owner is a trading account (not a protocol
     *    account), and for every (account, asset) the ledger's reserved
     *    balance equals the sum of reserved_left of that account's resting
     *    orders holding that asset: nothing is reserved for anything else;
     * 5. every account's open-order count equals its resting orders.
     */
    Result<void> CheckInvariants() const;

    /** CheckInvariants, given that `basis` satisfies them (M2-LED-10):
     * conservation by LedgerState::CheckConservationSince(basis ledger), the
     * books and invariant 3 in full, and invariants 4 and 5 for every account
     * that changed since the basis or owns a resting order here or in the
     * basis. Any other account is the basis's own and ordered nothing, so its
     * basis check still holds. The same verdict as CheckInvariants, in
     * O(changed + orders + assets + withdrawals) instead of O(accounts +
     * orders); "changed" counts every change since the basis, however made.
     */
    Result<void> CheckInvariantsSince(const VerifiedBasis& basis) const;

    /** Engine phase 9: CheckInvariantsSince(*verified), or CheckInvariants
     * when nothing was verified; on success `verified` becomes this state.
     * Genesis and snapshot loads verify too, so a slot on any head the
     * engine or a restore produced checks only its own changes.
     */
    Result<void> Verify();

    //! The ledger and the books; never `verified`.
    friend bool operator==(const EngineState& a, const EngineState& b) { return a.ledger == b.ledger && a.books == b.books; }
};

/** A committed head as the engine reads it: the parent header and the state
 * after it. Non-owning: the caller keeps both alive while it uses the view
 * (the state owner, R9, holds them in one immutable snapshot, so a reader
 * never sees a header with another head's state). Temporaries are refused
 * at compile time. HeaderHash() is header.Hash(), computed once.
 */
class HeadView
{
public:
    HeadView(const SlotHeaderV2& header, const EngineState& state) : m_header{&header}, m_state{&state}, m_hash{header.Hash()} {}
    HeadView(SlotHeaderV2&&, const EngineState&) = delete;
    HeadView(const SlotHeaderV2&, EngineState&&) = delete;
    HeadView(SlotHeaderV2&&, EngineState&&) = delete;

    const SlotHeaderV2& Header() const { return *m_header; }
    const EngineState& State() const { return *m_state; }
    const uint256& HeaderHash() const { return m_hash; }

private:
    const SlotHeaderV2* m_header;
    const EngineState* m_state;
    uint256 m_hash;
};

// ---- The slot validity predicate ----------------------------------------------

/** What a proposer puts into the slot that follows a parent: everything the
 * certified header carries except the parent link (from the HeadView), the
 * authority epoch and seat set (the state owner's, R9) and the commitments
 * (which follow from executing the slot).
 */
struct SlotInputs {
    uint256 domain;        //!< DomainHash of the network; equal to the parent's
    uint256 profile_hash;  //!< ProfileHash; equal to the parent's and the profile's
    Anchor anchor;         //!< the B3 block the slot is anchored to
    FactCursor facts_from; //!< equal to the parent's facts_to
    FactCursor facts_to;   //!< end of the consumed fact range (exclusive)
    SlotBody body;         //!< canonical fact bytes in cursor order; exact original action bytes in proposal order

    friend bool operator==(const SlotInputs&, const SlotInputs&) = default;
};

//! The decoded payload of an action kind the outline executes.
using ActionPayload = std::variant<PlacePayload, CancelPayload, TransferPayload, WithdrawPayload>;

//! One validated original: the verified action, its decoded payload and its phase.
struct SlotAction {
    VerifiedAction action;
    ActionPayload payload;
    SlotPhase phase;
};

// Static details of ValidateSlotInputs' refusals (code INVALID_INPUT), in
// precedence order. Tests and later slices compare them; they are not
// certified data.
inline constexpr std::string_view SLOT_PROFILE_MISMATCH{"slot profile mismatch"};
inline constexpr std::string_view SLOT_DOMAIN_MISMATCH{"slot domain mismatch"};
inline constexpr std::string_view SLOT_HEIGHT_EXHAUSTED{"parent height has no successor"};
inline constexpr std::string_view SLOT_ANCHOR_BELOW_PARENT{"anchor below the parent's anchor"};
inline constexpr std::string_view SLOT_ANCHOR_FORK{"anchor at the parent's height with another hash"};
inline constexpr std::string_view SLOT_FACTS_NOT_CONTIGUOUS{"facts not contiguous with the parent's facts_to"};
inline constexpr std::string_view SLOT_FACTS_REVERSED{"facts_to before facts_from"};
inline constexpr std::string_view SLOT_FACTS_BEYOND_ANCHOR{"fact range beyond the anchor"};
inline constexpr std::string_view SLOT_FACT_COUNT_CAP{"more facts than MAX_SLOT_FACTS"};
inline constexpr std::string_view SLOT_FACT_BYTES_CAP{"fact longer than MAX_FACT_BYTES"};
inline constexpr std::string_view SLOT_ACTION_COUNT_CAP{"more originals than max_actions_per_slot"};
inline constexpr std::string_view SLOT_ACTION_BYTES_CAP{"original longer than the action byte cap"};
inline constexpr std::string_view SLOT_FACT_MALFORMED{"fact does not decode"};
inline constexpr std::string_view SLOT_FACT_KIND{"fact kind the engine does not apply"};
inline constexpr std::string_view SLOT_FACT_CURSOR{"fact cursor outside the range or out of order"};
inline constexpr std::string_view SLOT_DEPOSIT_REFUSED{"deposit fact the ledger would refuse"};
inline constexpr std::string_view SLOT_ACTION_MALFORMED{"original does not decode"};
inline constexpr std::string_view SLOT_ACTION_DOMAIN{"action domain mismatch"};
inline constexpr std::string_view SLOT_ACTION_KIND{"unsupported action kind"};
inline constexpr std::string_view SLOT_ACTION_PAYLOAD{"action payload does not decode"};
inline constexpr std::string_view SLOT_ACTION_WINDOW{"action outside its window"};
inline constexpr std::string_view SLOT_DUPLICATE_ACTION{"duplicate ActionId"};
inline constexpr std::string_view SLOT_DUPLICATE_NONCE{"two originals with one (account, nonce)"};
inline constexpr std::string_view SLOT_ACCOUNT_ABSENT{"account absent in the parent state"};
inline constexpr std::string_view SLOT_NONCE_EXCLUDED{"nonce excluded by the parent window"};
inline constexpr std::string_view SLOT_BAD_SIGNATURE{"invalid action signature"};

class ValidatedSlot;

/** The slot validity predicate (plan section 3.4.1; ARCHITECTURE.md section
 * 11): whether `inputs` may follow `parent` under `profile`. On success it
 * returns the only kind of value Engine::ApplySlot executes. Followers run
 * it on every proposal; a leader runs it on its own.
 *
 * Checks, in this order (the first failure is returned; each is
 * INVALID_INPUT with the detail named, unless stated):
 *  0. Validate(profile) (its own error);
 *  1. ProfileHash(profile), inputs.profile_hash, the parent header's
 *     profile_hash and the parent ledger's profile agree (PROFILE_MISMATCH);
 *  2. inputs.domain is the parent's (DOMAIN_MISMATCH);
 *  3. the parent height has a successor (HEIGHT_EXHAUSTED); the slot
 *     height is parent height + 1;
 *  4. the anchor is not below the parent's (ANCHOR_BELOW_PARENT), and at
 *     the parent's height it is the same block (ANCHOR_FORK);
 *  5. facts_from is the parent's facts_to (FACTS_NOT_CONTIGUOUS),
 *     facts_to >= facts_from (FACTS_REVERSED), and facts_to <= (anchor
 *     height + 1, 0), so no fact above the anchor is consumed
 *     (FACTS_BEYOND_ANCHOR);
 *  6. caps, before any decoding: at most MAX_SLOT_FACTS facts
 *     (FACT_COUNT_CAP) of at most MAX_FACT_BYTES (FACT_BYTES_CAP), at most
 *     max_actions_per_slot originals (ACTION_COUNT_CAP) of at most
 *     ActionV2::MaxEncodedSize bytes (ACTION_BYTES_CAP);
 *  7. every fact, in body order: DecodeFact; a stub fact kind returns its
 *     NotImplemented error unchanged (M7-SEAT-01, M7-GOV-01, MB1-B3D-02,
 *     M8-BRG-02, M8-EXIT-01), any other decode error is FACT_MALFORMED and
 *     a decoded kind other than DEPOSIT is FACT_KIND; its cursor lies in
 *     [facts_from, facts_to) and above the previous fact's (FACT_CURSOR); a
 *     deposit the ledger would refuse (amount 0, a protocol account, an
 *     outpoint credited in the parent or twice in the slot) is
 *     DEPOSIT_REFUSED;
 *  8. every original, in proposal order: exact decode under the profile
 *     (ACTION_MALFORMED); its domain (ACTION_DOMAIN); KindSupported, so
 *     CANCEL_ALL, WITHDRAW and the futures transfers are refused here
 *     (ACTION_KIND); its payload decodes (ACTION_PAYLOAD); MaxDelayValid
 *     and InExpiryWindow(ref_slot, max_delay, height) (ACTION_WINDOW); no
 *     earlier original has its ActionId (DUPLICATE_ACTION) or its (account,
 *     nonce) (DUPLICATE_NONCE); the signer's account exists in the parent
 *     state (ACCOUNT_ABSENT); the nonce is ACCEPTABLE in the parent window
 *     (NONCE_EXCLUDED);
 *  9. each account's slot nonces, inserted in ascending order into a copy
 *     of its parent window, are all accepted (NONCE_EXCLUDED; implied by 8
 *     and the D7 rule, checked so that apply cannot fail on it);
 * 10. every BIP340 signature, last because it is the most expensive
 *     (BAD_SIGNATURE).
 * Then CanonicalOrder(actions, parent header hash, height).
 *
 * A ValidatedSlot is bound to its parent by the parent header hash;
 * Engine::ApplySlot refuses another parent (STALE_PARENT). Pure: no lock,
 * clock, I/O or randomness.
 *
 * REAL-REPLACE: M4-AGR-11 checks this predicate against the agreement
 * PROTOCOL.md and Twins; M3-ENG-04 adds the changed-leaves cap and the
 * SLOT_FULL rule. Batch signature verification is M4-ADM-01.
 */
// TODO(rebirth:M4-AGR-11) conformance of the validity predicate with PROTOCOL.md (checker and Twins agreement).
Result<ValidatedSlot> ValidateSlotInputs(const HeadView& parent, const SlotInputs& inputs, const ProfileParams& profile);

/** Slot inputs that passed ValidateSlotInputs: the decoded deposits and
 * actions, the canonical order and the parent they were checked against.
 * Only ValidateSlotInputs makes one (private constructor), so
 * Engine::ApplySlot only ever executes validated inputs. Immutable; copies
 * stay validated.
 */
class ValidatedSlot
{
public:
    //! Hash of the parent header the inputs were validated against.
    const uint256& ParentHash() const { return m_parent_hash; }
    //! Parent height + 1.
    uint64_t Height() const { return m_height; }
    //! The inputs exactly as given.
    const SlotInputs& Inputs() const { return m_inputs; }
    //! The decoded deposits, in body order (aligned with Inputs().body.facts).
    std::span<const DepositFact> Deposits() const { return m_deposits; }
    //! FactId of each deposit (= FactLeaf of its bytes), aligned with Deposits().
    std::span<const uint256> FactIds() const { return m_fact_ids; }
    //! The originals in proposal order (aligned with Inputs().body.originals).
    std::span<const SlotAction> Actions() const { return m_actions; }
    //! Canonical order: Order()[rank] is the index into Actions() at that rank.
    std::span<const size_t> Order() const { return m_order; }

private:
    friend Result<ValidatedSlot> ValidateSlotInputs(const HeadView&, const SlotInputs&, const ProfileParams&);
    ValidatedSlot(uint256 parent_hash, uint64_t height, SlotInputs inputs, std::vector<DepositFact> deposits,
                  std::vector<uint256> fact_ids, std::vector<SlotAction> actions, std::vector<size_t> order);

    uint256 m_parent_hash;
    uint64_t m_height;
    SlotInputs m_inputs;
    std::vector<DepositFact> m_deposits;
    std::vector<uint256> m_fact_ids;
    std::vector<SlotAction> m_actions;
    std::vector<size_t> m_order;
};

// ---- Executing a slot -------------------------------------------------------------

/** What one slot produced: the candidate state and its certified outcomes. */
struct SlotResult {
    EngineState state;               //!< the state after the slot
    std::vector<OutcomeV2> outcomes; //!< emission order (Engine::ApplySlot)
    uint256 outcomes_root;           //!< MerkleRoot of the outcomes' LeafHash in emission order
    StateRootsV0 roots;              //!< state.Commit(); roots.Root() is the header's state_root
};

/** Phase 7, the futures sweep (plan section 3.11): Ok() with no change
 * unless profile.futures_enabled, else NotImplemented(M9-FUT-04).
 * Validate() refuses futures_enabled (M9-FUT-01), so Engine::ApplySlot
 * never reaches the stub in the outline.
 */
Result<void> RunFuturesPhase(EngineState& state, const ProfileParams& profile);

/** The slot engine of one profile. PROVISIONAL REGTEST (D4, D5, D6, D7).
 *
 * ApplySlot(parent, slot) copies the parent state and executes the
 * validated slot on the copy. Every action consumes its nonce first, in
 * canonical order, whatever its outcome (DUR-5); then the phases run in
 * order, each over its actions in canonical rank:
 * 1. DEPOSITS: each deposit is credited (one DEPOSIT_CREDITED outcome per
 *    fact, source FACT, subject its FactId).
 * 2. ACCOUNT: TRANSFER (O-7): TRANSFERRED, or REJ_BAD_PARAMS (self or
 *    zero), REJ_NO_RECIPIENT, REJ_INSUFFICIENT_AVAILABLE.
 * 3. CANCELS by OrderId: CANCELLED (the order leaves its book, its
 *    reservation is released, its owner's open-order count drops), or
 *    REJ_UNKNOWN_ORDER (not resting: never placed, already closed, or
 *    placed in this slot, whose PLACE runs later) or REJ_NOT_OWN_ORDER.
 *    REJ_ORDER_CLOSED needs an index of closed orders; the outline does
 *    not keep one and answers REJ_UNKNOWN_ORDER.
 * 4. POST_ONLY: RESTING at its own price with
 *    ArrivalRank(height, POST_ONLY, rank), or REJ_WOULD_CROSS if it would
 *    execute against the book as it stands after the earlier post-only
 *    orders of the slot.
 * 5. TAKERS (GTC and IOC PLACEs): every taker is reserved in canonical
 *    order at its limit (a BUY in quote, a SELL in base), so same-slot
 *    proceeds can never fund a same-slot reservation and two markets that
 *    share a quote asset compete for one balance (TRD-10); a GTC taker also
 *    takes an open-order slot now, because it may rest. Then one SweepA2
 *    per market, in ascending MarketId, all sharing one SlotFillBudget of
 *    max_fills_per_slot (O-36).
 * 6. REST, per taker in canonical order: FILLED (no lots left); else
 *    REJ_FILL_CAP if a fill cap stopped it while the book still crosses
 *    (ORDER_FILL_CAP or SLOT_FILL_CAP; M3-ENG-04 rules SLOT_FULL); else
 *    IOC_DONE for an IOC; else REJ_RESIDUAL_CROSS_UNSUPPORTED for a GTC
 *    remainder that crosses a same-slot opposite GTC remainder
 *    (FindResidualCross; M3-ENG-02); else the remainder rests at its limit
 *    with ArrivalRank(height, REST, rank): RESTING without fills,
 *    PARTIAL_RESTING with. Whatever does not rest is released (price
 *    improvement, IOC and refused remainders), and a GTC taker that does
 *    not rest gives its open-order slot back. REJ_FILL_CAP and
 *    REJ_RESIDUAL_CROSS_UNSUPPORTED refuse the REMAINDER only: their
 *    outcome still lists the fills made before (O-40).
 * 7. FUTURES: RunFuturesPhase.
 * 8. FEES: for each quote asset whose fills withheld fees, ascending
 *    AssetId: SplitSlotFees(total, seat_share_percent) is credited to the
 *    seat pool and the treasury, one FEES_DISTRIBUTED outcome (source SLOT,
 *    subject the AssetId); afterwards no fee is held (O-31).
 * 9. CONSERVATION: EngineState::CheckInvariants; every outcome within the
 *    profile's fill and delta caps; the outcomes' deltas, summed, equal
 *    the balance change from the parent for every (account, asset); then
 *    the roots. Any failure is INVARIANT and there is no candidate.
 *
 * PLACE rejections, in this order and all with no change besides the
 * nonce: REJ_UNKNOWN_MARKET; REJ_BAD_PARAMS (price or lots 0, or a
 * reservation above MAX_ATOMS); REJ_WOULD_CROSS (post-only only);
 * REJ_OPEN_ORDER_CAP (GTC and post-only: max_open_orders_per_account
 * already reached); REJ_INSUFFICIENT_AVAILABLE. REJ_BOOK_FULL,
 * REJ_PRICE_BAND and REJ_UNKNOWN_ASSET are not produced by the outline
 * (M3-ENG-01, M3-ENG-03).
 *
 * Outcomes (core/outcome.h), in emission order: one per deposit in body
 * order; one per action in canonical rank order (subject its ActionId,
 * account its signer, its nonce); one FEES_DISTRIBUTED per quote asset.
 * deltas list every balance the outcome changed, aggregated per (account,
 * asset) and sorted, with no all-zero entry; a taker's outcome carries its
 * reservation, both legs of each of its fills (its own and its makers'
 * balances; the seller receives quote - fee) and its release. fills are the
 * fills the action made as taker, in execution order. remaining_lots is
 * what rests (PLACE), else 0. A deterministic rejection has no fills and
 * no deltas.
 *
 * Errors (no candidate): STALE_PARENT if the slot was validated against
 * another head; INVALID_INPUT if the slot or the parent belongs to another
 * profile; INVARIANT if the parent holds undistributed fees or phase 9
 * fails; and any error of the ledger or the books, which the validity
 * predicate excludes unless an invariant broke elsewhere (O-34 lists the
 * ledger refusals a chain fact could still trigger).
 *
 * Pure: no lock, clock, I/O or randomness; the result depends only on the
 * parent and the slot, not on the order of the originals in the body.
 */
class Engine
{
public:
    //! INVALID_INPUT unless the matching profile is A2_PRORATA; Validate(profile)'s errors first.
    static Result<Engine> Create(const ProfileParams& profile);

    const ProfileParams& Profile() const { return m_profile; }

    //! Executes `slot` on a copy of parent.State(); see the class comment.
    Result<SlotResult> ApplySlot(const HeadView& parent, const ValidatedSlot& slot) const;

private:
    explicit Engine(ProfileParams profile) : m_profile{std::move(profile)} {}

    ProfileParams m_profile;
};

} // namespace flowmesh2::engine

#endif // B3_FLOWMESH2_ENGINE_ENGINE_H
