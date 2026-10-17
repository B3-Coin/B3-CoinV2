// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_ADMISSION_POOL_H
#define B3_FLOWMESH2_ADMISSION_POOL_H

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/read/read_service.h>
#include <flowmesh2/state/candidate.h>
#include <sync.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

/** The bounded action pool of one node (slice R12; ARCHITECTURE.md sections
 * 3, 4 and 10.1; plan PLAN-9, DUR-4, DUR-6).
 *
 * The pool holds the actions this node's admission authenticated (only a
 * VerifiedAction can enter), until a certified slot includes them or they
 * become definitely unincludable. It is NOT certified state: it answers what
 * this node will propose, never how an action executed (that is an
 * OutcomeV2 under a certificate).
 *
 * Entries and statuses (PoolStatus, append-only):
 * - live: PENDING (admitted, not in a proposal) and IN_PROPOSAL (handed to a
 *   proposal by TakeForSlot; still in the pool until a slot commits);
 * - terminal: INCLUDED (a certified slot executed it), NONCE_USED (another
 *   action used its nonce), NONCE_TOO_LOW (the account's window moved past
 *   its nonce) and EXPIRED (height > ref_slot + max_delay, so no later slot
 *   can include it; the same rule VerifyExpired and the read service use,
 *   O-63). Every live entry ends in exactly one terminal status or stays
 *   live; nothing is dropped silently (DUR-6).
 *
 * Invariants kept by every method (checked by the tests):
 * - at most one live entry per ActionId and per (account, nonce), so the
 *   pool never holds two actions the validity predicate would refuse
 *   together (SLOT_DUPLICATE_ACTION, SLOT_DUPLICATE_NONCE);
 * - at most `capacity` live entries (Insert answers POOL_FULL beyond);
 * - after OnCommitted(head h), every live entry's nonce is ACCEPTABLE in h's
 *   window, its account exists at h and h <= ref_slot + max_delay; with
 *   distinct nonces per account that makes every TakeForSlot(h + 1) result
 *   pass the validity predicate on h (the tests run it through
 *   StateOwner::Preview).
 *
 * Snapshot rule. Admission checks an action against one committed head (its
 * snapshot) and then inserts it. Insert refuses an action checked against a
 * head older than the last head OnCommitted saw (STALE_SNAPSHOT), because
 * that commit may have used its nonce after the check; admission then
 * re-checks against a fresh snapshot. An insert checked against a newer head
 * than the pool's (the state owner applied a slot the pool has not seen) is
 * accepted: the next OnCommitted re-checks every live entry anyway.
 *
 * Retention. An INCLUDED record (ActionId, the included original's
 * signature, IncludedRef) is kept while h - s < retention_slots (s its slot,
 * h the committed height), the read service's outcome-index rule. Because
 * retention_slots >= max_delay_slots, an exact resend of an included action
 * finds its record (admission A3: DUPLICATE, no signature check) for as long
 * as the action could otherwise still be admitted (DUR-4, DUR-5); after that
 * its window has passed and admission answers EXPIRED. Records come from
 * every committed original (the body), not only from entries this node
 * held. The other terminal records are kept by the same rule and at most
 * terminal_capacity of them (oldest evicted first), so Status can report
 * them for a while; they are not used for deduplication.
 *
 * Locks. One Mutex (ARCHITECTURE.md section 4) guards every map and counter.
 * It is held for map operations only: never across a signature check (only
 * verified actions enter), a call into another object, or the withhold
 * filter of TakeForSlot (called without the lock). The pool nests no other
 * fm2 mutex.
 *
 * Not deterministic state, but deterministic code: ordered maps, no clock or
 * randomness; selection is in admission order.
 */
namespace flowmesh2::admission {

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view POOL_LIMIT_ZERO{"zero pool limit"};                                              //!< (INVALID_INPUT)
inline constexpr std::string_view POOL_RETENTION_SHORT{"pool retention shorter than max_delay_slots"};             //!< (INVALID_INPUT)
inline constexpr std::string_view POOL_TAKE_HEIGHT{"slot height is not the successor of the pool's head"};         //!< (ORDER_VIOLATION)
inline constexpr std::string_view POOL_NOT_SUCCESSOR{"committed head is not the successor of the pool's head"};    //!< (ORDER_VIOLATION)
inline constexpr std::string_view POOL_PROFILE_MISMATCH{"committed head of another profile"};                      //!< (INVALID_INPUT)
inline constexpr std::string_view POOL_OUTCOMES_MISMATCH{"outcomes do not give the head's outcomes_root"};         //!< (INVALID_INPUT)
inline constexpr std::string_view POOL_BODY_MISMATCH{"originals do not give the head's actions_root"};             //!< (INVALID_INPUT)
inline constexpr std::string_view POOL_ORIGINAL_MALFORMED{"committed original does not decode"};                   //!< (INVALID_INPUT)
inline constexpr std::string_view POOL_OUTCOME_MISSING{"committed original without exactly one action outcome"};   //!< (INVALID_INPUT)
inline constexpr std::string_view POOL_ACCOUNT_MISSING{"a live entry's account is absent at the committed head"};  //!< (INVARIANT)

struct PoolLimits {
    size_t capacity{0};          //!< live (PENDING or IN_PROPOSAL) entries; Insert answers POOL_FULL beyond
    uint64_t retention_slots{0}; //!< slots a terminal record is kept; >= max_delay_slots
    size_t terminal_capacity{0}; //!< most terminal records other than INCLUDED; the oldest are evicted first

    //! {pool_capacity, outcome_index_slots, pool_capacity} of `profile`.
    static PoolLimits FromProfile(const ProfileParams& profile);
};

//! The status of one action in the pool. APPEND-ONLY; zero is never a value.
enum class PoolStatus : uint8_t {
    PENDING = 1,       //!< live: admitted, not in a proposal
    IN_PROPOSAL = 2,   //!< live: handed to the proposal of slot `height`
    INCLUDED = 3,      //!< terminal: a certified slot executed it (its outcome is certified)
    NONCE_USED = 4,    //!< terminal: another action used its nonce
    NONCE_TOO_LOW = 5, //!< terminal: the account's window moved past its nonce
    EXPIRED = 6,       //!< terminal: height > ref_slot + max_delay
};
bool IsKnown(PoolStatus status);
//! True for INCLUDED, NONCE_USED, NONCE_TOO_LOW and EXPIRED.
bool IsTerminal(PoolStatus status);
//! Registry name; "UNKNOWN" for an unregistered value.
std::string_view ToString(PoolStatus status);

//! How an action entered this pool: its ActionId, the pool's admission sequence (from 1) and the snapshot height it was checked at.
struct PendingRef {
    ActionId id;
    uint64_t seq{0};
    uint64_t snapshot_height{0};
    friend bool operator==(const PendingRef&, const PendingRef&) = default;
};

//! What the pool knows about one ActionId.
struct PoolEntryStatus {
    PoolStatus status{PoolStatus::PENDING};
    std::optional<PendingRef> pending;         //!< set iff this pool held the action (an included original may never have been here)
    std::optional<read::IncludedRef> included; //!< set iff INCLUDED
    //! IN_PROPOSAL: the slot it was handed to; terminal: the committed height that made it terminal; PENDING: 0.
    uint64_t height{0};
    friend bool operator==(const PoolEntryStatus&, const PoolEntryStatus&) = default;
};

//! The result of Insert. APPEND-ONLY; zero is never a value.
enum class InsertResult : uint8_t {
    INSERTED = 1,       //!< a new live entry (admission ADMITTED)
    DUPLICATE = 2,      //!< the ActionId is live or retained INCLUDED (admission DUPLICATE)
    NONCE_PENDING = 3,  //!< another live entry has the (account, nonce) (admission NONCE_PENDING)
    POOL_FULL = 4,      //!< `capacity` live entries (admission POOL_FULL)
    STALE_SNAPSHOT = 5, //!< checked against a head older than the pool's; nothing changed, re-check
};
std::string_view ToString(InsertResult result);

struct [[nodiscard]] InsertReply {
    InsertResult result{InsertResult::INSERTED};
    std::optional<PendingRef> pending;         //!< INSERTED: the new entry; DUPLICATE of a live entry: that entry
    std::optional<read::IncludedRef> included; //!< DUPLICATE of a retained INCLUDED record
};

//! An action this pool already knows: a live entry or a retained INCLUDED record.
struct KnownAction {
    PoolStatus status{PoolStatus::PENDING};    //!< PENDING, IN_PROPOSAL or INCLUDED
    std::optional<PendingRef> pending;         //!< set for a live entry (and for an INCLUDED one this pool held)
    std::optional<read::IncludedRef> included; //!< set iff INCLUDED
};

/** Returns true to WITHHOLD an action from the slot being built (it stays
 * PENDING). Called without the pool lock; it must not call the pool.
 */
using WithholdFilter = std::function<bool(const ActionV2&)>;

//! What TakeForSlot hands to a proposal: the slot height and the exact original bytes, in admission order.
struct TakenSlot {
    uint64_t height{0};
    std::vector<ActionId> ids;
    std::vector<std::vector<unsigned char>> originals;
};

//! What OnCommitted did, for traces and tests.
struct CommitReport {
    uint64_t height{0};
    size_t included{0};      //!< live entries removed because the slot included them
    size_t recorded{0};      //!< INCLUDED records written (every original of the slot)
    size_t nonce_used{0};    //!< live entries made terminal NONCE_USED
    size_t nonce_too_low{0}; //!< live entries made terminal NONCE_TOO_LOW
    size_t expired{0};       //!< live entries made terminal EXPIRED
    size_t returned{0};      //!< IN_PROPOSAL entries back to PENDING
    size_t evicted{0};       //!< terminal records dropped by retention or terminal_capacity
    friend bool operator==(const CommitReport&, const CommitReport&) = default;
};

class ActionPool
{
public:
    /** An empty pool whose committed head is at `head_height` (the state
     * owner's head when the pool starts). Validate(profile) first (its
     * errors), then INVALID_INPUT POOL_LIMIT_ZERO for a zero capacity,
     * retention_slots or terminal_capacity, and POOL_RETENTION_SHORT for
     * retention_slots below profile.max_delay_slots.
     */
    static Result<std::unique_ptr<ActionPool>> Create(const PoolLimits& limits, const ProfileParams& profile, uint64_t head_height);

    ActionPool(const ActionPool&) = delete;
    ActionPool& operator=(const ActionPool&) = delete;

    const PoolLimits& Limits() const { return m_limits; }
    const ProfileParams& Profile() const { return m_profile; }

    /** Admission A9 (pool side) and A10, atomically: STALE_SNAPSHOT if
     * `snapshot_height` is below the pool's head; DUPLICATE if the ActionId
     * is live (any signature) or retained INCLUDED; NONCE_PENDING if another
     * live entry has the action's (account, nonce); POOL_FULL at `capacity`
     * live entries; else a new PENDING entry with the next sequence number
     * (a terminal record of the same ActionId other than INCLUDED is
     * replaced). Only the INSERTED result changes the pool.
     */
    InsertReply Insert(const VerifiedAction& action, uint64_t snapshot_height) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Admission A3: a live entry or retained INCLUDED record with this
     * ActionId AND exactly this signature, i.e. an original this node already
     * authenticated; nullopt otherwise (never a NONCE_USED, NONCE_TOO_LOW or
     * EXPIRED record).
     */
    std::optional<KnownAction> FindAuthenticated(const ActionId& id, std::span<const unsigned char, ACTION_SIG_BYTES> sig) const
        EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Admission A9: a live entry or retained INCLUDED record with this ActionId, whatever its signature.
    std::optional<KnownAction> FindById(const ActionId& id) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** The live entries for the slot at `next_height`, in admission order:
     * ORDER_VIOLATION POOL_TAKE_HEIGHT unless next_height is the pool's head
     * height + 1. Entries outside their window for next_height
     * (!InExpiryWindow) are skipped, so are entries `withhold` returns true
     * for; at most min(max, profile.max_actions_per_slot) are taken. Taken
     * entries become IN_PROPOSAL(next_height) and STAY in the pool; an
     * IN_PROPOSAL entry is taken again by a later call (a retried proposal).
     * Skipped and withheld entries keep their status.
     */
    Result<TakenSlot> TakeForSlot(uint64_t next_height, size_t max, const WithholdFilter& withhold = {})
        EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** The state owner committed `head` (after B-APPLIED and read
     * publication), whose slot emitted `outcomes` (in emission order) from
     * `body` (the certified canonical body). Refused, changing nothing:
     * ORDER_VIOLATION POOL_NOT_SUCCESSOR unless head height = pool head + 1;
     * INVALID_INPUT POOL_PROFILE_MISMATCH, POOL_OUTCOMES_MISMATCH (count or
     * outcomes_root), POOL_BODY_MISMATCH (count or actions_root),
     * POOL_ORIGINAL_MALFORMED, POOL_OUTCOME_MISSING (an original without
     * exactly one ACTION outcome, or an ACTION outcome without an original);
     * INVARIANT POOL_ACCOUNT_MISSING. Then, atomically:
     *  1. every original of the body gets an INCLUDED record (its signature
     *     and IncludedRef{height, outcome index}); a live entry with its
     *     ActionId leaves the pool (whatever signature it held);
     *  2. every other live entry is checked against `head`: its nonce USED
     *     gives terminal NONCE_USED, TOO_LOW terminal NONCE_TOO_LOW; else
     *     ExpiredAt(ref_slot, max_delay, height) gives terminal EXPIRED; else
     *     an IN_PROPOSAL entry returns to PENDING;
     *  3. retention eviction (see the file comment); the pool's head becomes
     *     `head`.
     */
    Result<CommitReport> OnCommitted(const state::HeadView& head, std::span<const OutcomeV2> outcomes, const SlotBody& body)
        EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! What the pool knows about `id`; nullopt if nothing (never admitted, or evicted).
    std::optional<PoolEntryStatus> Status(const ActionId& id) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! Live entries (PENDING or IN_PROPOSAL).
    size_t Size() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The height of the last head OnCommitted saw (or the creation height).
    uint64_t HeadHeight() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Retained INCLUDED records and other terminal records (diagnostics for tests).
    size_t IncludedRecords() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    size_t TerminalRecords() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    ActionPool(const PoolLimits& limits, const ProfileParams& profile, uint64_t head_height);

    struct Entry {
        std::shared_ptr<const VerifiedAction> action;
        std::vector<unsigned char> original; //!< action->Action().Encode(), the exact bytes
        PendingRef ref;
        PoolStatus status{PoolStatus::PENDING};
        uint64_t proposal_height{0};
    };
    struct Terminal {
        PoolStatus status{PoolStatus::INCLUDED};
        std::optional<PendingRef> pending;
        std::optional<read::IncludedRef> included;
        std::array<unsigned char, ACTION_SIG_BYTES> sig{}; //!< INCLUDED: the included original's signature
        uint64_t height{0};
    };

    void EraseLive(std::map<ActionId, Entry>::iterator it) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    const PoolLimits m_limits;
    const ProfileParams m_profile;
    const uint256 m_profile_hash;

    mutable Mutex m_mutex;
    uint64_t m_head_height GUARDED_BY(m_mutex);
    uint64_t m_next_seq GUARDED_BY(m_mutex){1};
    std::map<ActionId, Entry> m_live GUARDED_BY(m_mutex);
    std::map<uint64_t, ActionId> m_by_seq GUARDED_BY(m_mutex);                              //!< live, admission order
    std::map<std::pair<AccountId, uint64_t>, ActionId> m_by_nonce GUARDED_BY(m_mutex);      //!< live
    std::map<ActionId, Terminal> m_terminal GUARDED_BY(m_mutex);
    std::deque<std::pair<uint64_t, ActionId>> m_included_order GUARDED_BY(m_mutex);  //!< (height, id), oldest first
    std::deque<std::pair<uint64_t, ActionId>> m_other_order GUARDED_BY(m_mutex);     //!< (height, id), oldest first
    size_t m_other_count GUARDED_BY(m_mutex){0};
};

} // namespace flowmesh2::admission

#endif // B3_FLOWMESH2_ADMISSION_POOL_H
