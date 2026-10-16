// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_READ_READ_SERVICE_H
#define B3_FLOWMESH2_READ_READ_SERVICE_H

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/read/views.h>
#include <flowmesh2/read_slot.h>
#include <flowmesh2/storage/durable.h>
#include <sync.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

/** The read service: certified reads over the C1 ReadSlot (slice R11;
 * ARCHITECTURE.md sections 3, 4 and 10.2; [CC] boundary 5).
 *
 * Publication. The state owner's sealing path calls Publish once per slot,
 * after B-APPLIED, with the DurableApplied token of that slot and its
 * SlotView. Publish refuses (and changes nothing):
 *   - a null view                                   INVALID_INPUT READ_NULL_VIEW
 *   - a token whose slot, header hash or state root is not the view's
 *                                                   VERIFY_FAILED READ_TOKEN_MISMATCH
 *   - a view of another profile                     INVALID_INPUT READ_PROFILE_MISMATCH
 *   - an envelope above max_envelope_bytes          OVERSIZE      READ_ENVELOPE_OVERSIZE
 *   - a publish while another runs                  ORDER_VIOLATION READ_CONCURRENT_PUBLISH
 *   - a view that is not the successor of the last published one (height
 *     + 1 and parent hash; any height for the first) ORDER_VIOLATION READ_NOT_SUCCESSOR
 * Then it inserts the view into the archive and the slot's action outcomes
 * into the outcome index, and publishes the CertifiedEnvelope (header +
 * certificate, nothing else) into the C1 ReadSlot with ReadScope =
 * ScopeOf(header) and CommitIdentity = CommitOf(header) (views.h). Only
 * after that does it evict: the archive keeps the view_slots newest views,
 * the outcome index the actions of the outcome_index_slots newest slots, so
 * the current head's view is always present. A failed C1 publish
 * (EXHAUSTED) rolls the insertion back (INVARIANT READ_SLOT_EXHAUSTED).
 *
 * Reads. A ReadHandle holds exactly one C1 ReadLease. Every reply made
 * through it carries that lease's ReadScope, CommitIdentity, publication
 * counter, decoded envelope and the label CERTIFIED, and finds its SlotView
 * by the lease's commit.id, so the header, certificate, leaf, proof and
 * scope of one reply always belong to one head. The convenience methods of
 * ReadService take a fresh handle per reply (one lease per reply). A view
 * the archive no longer keeps reads UNKNOWN (UNKNOWN_VIEW_EVICTED); that
 * happens only to a handle that outlived view_slots newer publications.
 *   - Account: FOUND with the v0 leaf, the five roots and the proof, or
 *     UNKNOWN (v0 has no absence proof).
 *   - Action(original): INCLUDED with the including slot's envelope, the
 *     outcome and its proof (from the outcome index, only slots at or below
 *     the lease's height); otherwise EXPIRED only if the lease's own view
 *     holds the proof VerifyExpired accepts (the domain is the header's,
 *     max_delay is valid, height > ref_slot + max_delay, the signer's leaf
 *     exists and the core nonce rule accepts the nonce); otherwise UNKNOWN.
 *     Never EXPIRED without that proof.
 *   - OrderFills: FOUND with every outcome of the lease's slot in which the
 *     order filled, as maker or taker, with proofs; otherwise UNKNOWN.
 * Acquire refuses with BUSY (READ_NO_PERMIT) when all max_readers permits
 * are held and NEED_DATA (READ_NOTHING_PUBLISHED) before the first
 * publication; INVARIANT if a lease's payload is not a valid envelope of
 * its commit (READ_ENVELOPE_CORRUPT).
 *
 * Labels (INV-13): CERTIFIED means "certified against the seat set the
 * header names"; the reader still verifies it with b3verify against its
 * own trusted seat set. CHECKPOINTED and B3-final do not exist yet and are
 * never merged into CERTIFIED.
 *
 * Retention. Create refuses outcome_index_slots below the profile's
 * max_delay_slots (INVALID_INPUT READ_OUTCOME_INDEX_SHORT): an action stays
 * in the index while it could still be admitted, so DUPLICATE stays
 * definite for the whole expiry window (DUR-5).
 *
 * Locks. One archive Mutex guards the view map, the outcome index and the
 * publication bookkeeping. It is held only for map lookups, inserts and
 * erases: never across the C1 publish, a proof computation, I/O or the
 * destruction of an evicted view (evicted objects are released after the
 * lock). Publish never holds it together with the C1 publish mutex, so no
 * fm2 code nests two mutexes (ARCHITECTURE.md section 4). Readers never
 * take a publisher, chain or market lock, but a reply does take the
 * archive mutex for one lookup (REAL-REPLACE M5-RD-01: an RCU archive and
 * a typed view index remove the mutex from reads).
 */
namespace flowmesh2::read {

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view READ_LIMIT_ZERO{"zero read limit"};                                      //!< (INVALID_INPUT)
inline constexpr std::string_view READ_OUTCOME_INDEX_SHORT{"outcome index shorter than max_delay_slots"};  //!< (INVALID_INPUT)
inline constexpr std::string_view READ_NULL_VIEW{"no slot view"};                                          //!< (INVALID_INPUT)
inline constexpr std::string_view READ_TOKEN_MISMATCH{"applied token is not the view's slot"};             //!< (VERIFY_FAILED)
inline constexpr std::string_view READ_PROFILE_MISMATCH{"view of another profile"};                        //!< (INVALID_INPUT)
inline constexpr std::string_view READ_ENVELOPE_OVERSIZE{"envelope above max_envelope_bytes"};             //!< (OVERSIZE)
inline constexpr std::string_view READ_CONCURRENT_PUBLISH{"another publish is running"};                   //!< (ORDER_VIOLATION)
inline constexpr std::string_view READ_NOT_SUCCESSOR{"view is not the successor of the last publication"}; //!< (ORDER_VIOLATION)
inline constexpr std::string_view READ_SLOT_EXHAUSTED{"read slot publication counter exhausted"};          //!< (INVARIANT)
inline constexpr std::string_view READ_NO_PERMIT{"every reader permit is held"};                           //!< (BUSY)
inline constexpr std::string_view READ_NOTHING_PUBLISHED{"nothing published yet"};                         //!< (NEED_DATA)
inline constexpr std::string_view READ_ENVELOPE_CORRUPT{"lease payload is not its commit's envelope"};     //!< (INVARIANT)

// Static reasons of an UNKNOWN reply.
inline constexpr std::string_view UNKNOWN_VIEW_EVICTED{"the lease's view is no longer kept"};
inline constexpr std::string_view UNKNOWN_NO_ACCOUNT{"no such account at the head (v0 has no absence proof)"};
inline constexpr std::string_view UNKNOWN_NO_FILLS{"no fill of the order in the head's slot"};
inline constexpr std::string_view UNKNOWN_ACTION{"not in a retained slot and not provably expired"};

struct ReadLimits {
    size_t max_readers{0};           //!< C1 ReadLease permits held at once
    size_t max_envelope_bytes{0};    //!< C1 payload cap (CertifiedEnvelope bytes)
    size_t view_slots{0};            //!< SlotViews kept, newest first
    uint64_t outcome_index_slots{0}; //!< slots whose actions the outcome index keeps; >= max_delay_slots
};

//! What a reply's data is certified as. APPEND-ONLY; zero is never a value (INV-13).
enum class ReadLabel : uint8_t {
    CERTIFIED = 1, //!< certified against the seat set the header names
};
std::string_view ToString(ReadLabel label);

//! The status of an Account or OrderFills reply. APPEND-ONLY.
enum class LookupStatus : uint8_t {
    FOUND = 1,
    UNKNOWN = 2,
};

//! The status of an Action reply. APPEND-ONLY.
enum class ActionStatus : uint8_t {
    INCLUDED = 1,
    EXPIRED = 2,
    UNKNOWN = 3,
};

//! Where an action was included: slot height and outcome position.
struct IncludedRef {
    uint64_t height{0};
    uint32_t index{0};
    friend bool operator==(const IncludedRef&, const IncludedRef&) = default;
};

//! An included action's certified outcome with everything needed to verify it.
struct IncludedOutcome {
    IncludedRef ref;
    CertifiedEnvelope envelope; //!< the including slot's header and certificate
    OutcomeWitness outcome;     //!< the outcome and its proof under envelope.header.outcomes_root
};

//! What every reply carries: the lease's scope, head, publication counter and envelope, and the label.
struct ReplyHead {
    ReadScope scope;
    CommitIdentity commit;
    uint64_t publication{0};    //!< C1 local publication counter, not a consensus sequence
    CertifiedEnvelope envelope; //!< the head's header and certificate
    ReadLabel label{ReadLabel::CERTIFIED};
};

struct HeadReply {
    ReplyHead head;
};

struct AccountReply {
    ReplyHead head;
    LookupStatus status{LookupStatus::UNKNOWN};
    std::string_view reason;               //!< static; empty when FOUND
    std::optional<AccountWitness> witness; //!< iff FOUND
};

struct ActionReply {
    ReplyHead head;
    ActionStatus status{ActionStatus::UNKNOWN};
    std::string_view reason;                 //!< static; empty unless UNKNOWN
    std::optional<IncludedOutcome> included; //!< iff INCLUDED
    std::optional<AccountWitness> expiry;    //!< iff EXPIRED: the signer's leaf under head.envelope
};

//! The fills of an order in one outcome.
struct OrderFillGroup {
    OutcomeWitness outcome;
    std::vector<FillRef> fills; //!< ascending fill_index
};

struct OrderFillsReply {
    ReplyHead head;
    LookupStatus status{LookupStatus::UNKNOWN};
    std::string_view reason;             //!< static; empty when FOUND
    std::vector<OrderFillGroup> groups;  //!< ascending outcome index; non-empty iff FOUND
};

class ReadService;

/** One C1 ReadLease and the replies made from it. Move-only. It must not
 * outlive the ReadService that made it. Its methods are const and safe to
 * call from any thread.
 */
class ReadHandle
{
public:
    ReadHandle(ReadHandle&&) noexcept = default;
    ReadHandle& operator=(ReadHandle&&) noexcept = default;
    ReadHandle(const ReadHandle&) = delete;
    ReadHandle& operator=(const ReadHandle&) = delete;

    //! The lease's scope, commit, publication, envelope and label.
    const ReplyHead& Reply() const { return m_head; }

    Result<HeadReply> Head() const;
    Result<AccountReply> Account(const AccountId& account) const;
    Result<ActionReply> Action(const ActionV2& original) const;
    Result<OrderFillsReply> OrderFills(const OrderId& order) const;

private:
    friend class ReadService;
    ReadHandle(const ReadService& service, ReadLease lease, ReplyHead head);

    const ReadService* m_service;
    ReadLease m_lease;
    ReplyHead m_head;
};

class ReadService
{
public:
    /** Validate(profile) (its errors), then INVALID_INPUT READ_LIMIT_ZERO for
     * a zero max_readers, max_envelope_bytes or view_slots, and
     * READ_OUTCOME_INDEX_SHORT for outcome_index_slots below
     * profile.max_delay_slots.
     */
    static Result<std::unique_ptr<ReadService>> Create(const ReadLimits& limits, const ProfileParams& profile);

    ReadService(const ReadService&) = delete;
    ReadService& operator=(const ReadService&) = delete;

    //! Publish one applied slot. See the file comment.
    Result<void> Publish(const DurableApplied& applied, std::shared_ptr<const SlotView> view) EXCLUSIVE_LOCKS_REQUIRED(!m_archive_mutex);

    //! A handle on the current head (one lease). BUSY, NEED_DATA or INVARIANT; see the file comment.
    Result<ReadHandle> Acquire() const EXCLUSIVE_LOCKS_REQUIRED(!m_archive_mutex);

    //! One lease per reply: Acquire(), then the handle's method.
    Result<HeadReply> Head() const EXCLUSIVE_LOCKS_REQUIRED(!m_archive_mutex);
    Result<AccountReply> Account(const AccountId& account) const EXCLUSIVE_LOCKS_REQUIRED(!m_archive_mutex);
    Result<ActionReply> Action(const ActionV2& original) const EXCLUSIVE_LOCKS_REQUIRED(!m_archive_mutex);
    Result<OrderFillsReply> OrderFills(const OrderId& order) const EXCLUSIVE_LOCKS_REQUIRED(!m_archive_mutex);

    const ReadLimits& Limits() const { return m_limits; }
    const ProfileParams& Profile() const { return m_profile; }
    //! Diagnostics for tests: views kept, actions indexed, height of the last publication.
    size_t ViewCount() const EXCLUSIVE_LOCKS_REQUIRED(!m_archive_mutex);
    size_t IndexedActions() const EXCLUSIVE_LOCKS_REQUIRED(!m_archive_mutex);
    std::optional<uint64_t> PublishedHeight() const EXCLUSIVE_LOCKS_REQUIRED(!m_archive_mutex);
    //! C1 leases held now.
    size_t Readers() const { return m_slot.Readers(); }

private:
    friend class ReadHandle;
    ReadService(const ReadLimits& limits, const ProfileParams& profile);

    //! The kept view whose header hash is `id`, or null.
    std::shared_ptr<const SlotView> FindView(const uint256& id) const EXCLUSIVE_LOCKS_REQUIRED(!m_archive_mutex);
    //! The indexed outcome of `id` if it was included at or below `max_height`, or null.
    std::shared_ptr<const IncludedOutcome> FindIncluded(const ActionId& id, uint64_t max_height) const
        EXCLUSIVE_LOCKS_REQUIRED(!m_archive_mutex);

    const ReadLimits m_limits;
    const ProfileParams m_profile;
    const uint256 m_profile_hash;
    ReadSlot m_slot;

    // The archive. TODO(rebirth:M5-RD-01) an RCU archive and a typed view index
    // replace these maps and take the mutex off the read path.
    mutable Mutex m_archive_mutex;
    std::map<uint256, std::shared_ptr<const SlotView>> m_views GUARDED_BY(m_archive_mutex);
    std::deque<uint256> m_view_order GUARDED_BY(m_archive_mutex); //!< publication order, oldest first
    std::map<ActionId, std::shared_ptr<const IncludedOutcome>> m_included GUARDED_BY(m_archive_mutex);
    //! (height, the actions it indexed), oldest first.
    std::deque<std::pair<uint64_t, std::vector<ActionId>>> m_included_order GUARDED_BY(m_archive_mutex);
    //! Height and header hash of the last publication.
    std::optional<std::pair<uint64_t, uint256>> m_last GUARDED_BY(m_archive_mutex);
    bool m_publishing GUARDED_BY(m_archive_mutex){false};
};

} // namespace flowmesh2::read

#endif // B3_FLOWMESH2_READ_READ_SERVICE_H
