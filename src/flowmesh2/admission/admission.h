// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_ADMISSION_ADMISSION_H
#define B3_FLOWMESH2_ADMISSION_ADMISSION_H

#include <flowmesh2/admission/budget.h>
#include <flowmesh2/admission/pool.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/codes.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/read/read_service.h>
#include <flowmesh2/state/state_owner.h>
#include <uint256.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/** Admission of signed actions (slice R12; ARCHITECTURE.md section 10.1;
 * plan section 3.8, PLAN-9, G5, TRD-17, DUR-4..DUR-6; [CC] boundary 1).
 *
 * WHAT A REPLY MEANS. An AdmissionCode says whether THIS node queued the
 * exact action bytes now ("not admissible now" for every refusal). It is
 * never an execution outcome: execution is only an OutcomeV2 under a
 * certificate, which the client reads and verifies through the read
 * service and b3verify. ADMITTED means "in this node's pool", not "will
 * execute"; DUPLICATE means "this ActionId is already pending here or
 * included in a retained slot" and carries the reference (and, for an
 * included action, the certified outcome the read service still holds).
 *
 * ORDER (A0..A10). The first failing step's code is the reply; the BIP340
 * signature is checked at A8 only, after every cheaper refusal:
 *
 *  A0  fenced (Fence was called)                                   HALTED
 *  A1  frame: longer than ActionV2::MaxEncodedSize                 OVERSIZE
 *      the version byte is unknown (O-29: the layout of another
 *      version is unknown, so its framing cannot be judged)        WRONG_DOMAIN
 *      exact decode (truncated, trailing or non-canonical bytes;
 *      a payload above max_action_payload_bytes is OVERSIZE)       MALFORMED, OVERSIZE
 *  A2  domain is not this instance's (the head's domain)           WRONG_DOMAIN
 *      kind unknown (the frame is judged with a known kind byte
 *      first, so A1 and the domain still come first)               UNKNOWN_KIND
 *      kind not executable yet: CANCEL_ALL (M3-ENG-09), WITHDRAW
 *      (M2-LED-05), SPOT_TO_FUTURES and FUTURES_TO_SPOT
 *      (M2-LED-06); the reason is the task ID                      UNSUPPORTED_KIND
 *      the payload of a supported kind does not decode exactly as
 *      the validity predicate decodes it (PayloadAdmissible)        MALFORMED
 *  A3  the same ActionId AND signature as an original this node
 *      already authenticated: a live pool entry, or a retained
 *      INCLUDED record (pool.h, Retention)                         DUPLICATE{PendingRef | IncludedRef}
 *  --- one HeadView snapshot (StateOwner::Head) for A4..A9 ---
 *  A4  the signer's account does not exist at the snapshot         NO_ACCOUNT
 *  A5  the nonce is below the account's window (TOO_LOW)           NONCE_TOO_LOW
 *  A6  WindowRefusal against slot snapshot + 1, checked
 *      arithmetic: max_delay outside 1..max_delay_slots           BAD_EXPIRY
 *      ref_slot above the snapshot height                          REF_SLOT_AHEAD
 *      snapshot + 1 > ref_slot + max_delay                         EXPIRED
 *  A7  BudgetPolicy::Check denies                                  BUDGET
 *  A8  VerifiedAction::Verify (sig_verifications + 1)              BAD_SIG
 *  A9  the ActionId with ANOTHER valid signature is live, retained
 *      INCLUDED, or INCLUDED in the read service's outcome index   DUPLICATE{PendingRef | IncludedRef}
 *      the nonce is USED at the snapshot                           NONCE_USED
 *      another live entry has the (account, nonce)                 NONCE_PENDING
 *  A10 the pool is full                                            POOL_FULL
 *      else                                                        ADMITTED{PendingRef}
 *
 * A3 runs before the expiry check, so an included action resent after its
 * window, while its record is retained (retention_slots >=
 * max_delay_slots), is DUPLICATE with its original outcome, not EXPIRED
 * (DUR-4, DUR-5); TRD-17 holds because A3 only matches byte-identical
 * signatures this node already verified (the sigcache pattern). Inside A9
 * the DUPLICATE check comes first: an ActionId that is live or included has
 * its own nonce pending or used, so NONCE_PENDING or NONCE_USED would hide
 * it otherwise (O-69). A9's pool-side checks and A10 are one atomic
 * ActionPool::Insert; if the pool committed a newer head since the
 * snapshot (STALE_SNAPSHOT), A4..A6 and A9 run again on a fresh snapshot
 * (A7 and A8 are not repeated), at most MAX_SNAPSHOT_ATTEMPTS times, then
 * BUSY.
 *
 * What admission guarantees the pool (and so the slot builder): every
 * admitted action passes the validity predicate's per-original checks
 * (decode, domain, supported kind, payload, MaxDelayValid, account,
 * nonce, signature) against its snapshot, so an admitted action can never
 * make a proposed slot invalid (ActionPool, file comment).
 *
 * Errors (Result errors, not replies): a BudgetPolicy error (for example
 * the CreditBudget stub), an INVARIANT from the read service, and
 * INVARIANT ADMISSION_NO_HEAD. They are counted as `errors` in Stats.
 *
 * Threads. Admit may be called concurrently from any number of threads;
 * Fence from any thread (sticky: there is no Unfence, the node restarts).
 * Admission holds no lock of its own (counters and the fence are atomics)
 * and so never nests a lock: it calls the pool, the state owner, the read
 * service and the budget one at a time.
 */
namespace flowmesh2::admission {

// Static details of Create refusals (ErrorCode in parentheses) and errors.
inline constexpr std::string_view ADMISSION_PROFILE_MISMATCH{"an admission component has another profile"}; //!< (INVALID_INPUT)
inline constexpr std::string_view ADMISSION_NO_HEAD{"the state owner has no head"};                          //!< (INVARIANT)
inline constexpr std::string_view ADMISSION_KIND_UNREGISTERED{"unregistered action kind"};                   //!< (INVALID_INPUT)
inline constexpr std::string_view ADMISSION_INSERT_RESULT{"unregistered pool insert result"};                //!< (INVARIANT)

// Static reasons of replies (AdmissionReply::reason). UNSUPPORTED_KIND carries the task ID instead.
inline constexpr std::string_view REASON_ADMITTED{"admitted to this node's pool"};
inline constexpr std::string_view REASON_DUPLICATE_PENDING{"the ActionId is pending in this node's pool"};
inline constexpr std::string_view REASON_DUPLICATE_INCLUDED{"the ActionId is included in a retained slot"};
inline constexpr std::string_view REASON_HALTED{"admission is fenced"};
inline constexpr std::string_view REASON_FRAME_OVERSIZE{"frame or payload above its cap"};
inline constexpr std::string_view REASON_UNKNOWN_VERSION{"unknown ActionV2 version"};
inline constexpr std::string_view REASON_FRAME_MALFORMED{"frame does not decode exactly"};
inline constexpr std::string_view REASON_WRONG_DOMAIN{"domain of another FlowMesh instance"};
inline constexpr std::string_view REASON_UNKNOWN_KIND{"unknown action kind"};
inline constexpr std::string_view REASON_PAYLOAD_MALFORMED{"payload does not decode for its kind"};
inline constexpr std::string_view REASON_NO_ACCOUNT{"no account at the head; it exists after a deposit"};
inline constexpr std::string_view REASON_NONCE_TOO_LOW{"nonce below the account's window"};
inline constexpr std::string_view REASON_BAD_EXPIRY{"max_delay outside 1..max_delay_slots"};
inline constexpr std::string_view REASON_REF_SLOT_AHEAD{"ref_slot above the committed head"};
inline constexpr std::string_view REASON_EXPIRED{"the window ends before the next slot"};
inline constexpr std::string_view REASON_HEIGHT_EXHAUSTED{"the head has no successor slot"};
inline constexpr std::string_view REASON_BUDGET{"budget denied"};
inline constexpr std::string_view REASON_BAD_SIG{"invalid BIP340 signature"};
inline constexpr std::string_view REASON_NONCE_USED{"nonce used at the head"};
inline constexpr std::string_view REASON_NONCE_PENDING{"another pending action has this nonce"};
inline constexpr std::string_view REASON_POOL_FULL{"this node's pool is full"};
inline constexpr std::string_view REASON_BUSY_SNAPSHOT{"the head moved during every snapshot attempt"};

//! Snapshots one Admit takes at most before it answers BUSY.
inline constexpr int MAX_SNAPSHOT_ATTEMPTS{3};

//! One refusal: its code and static reason.
struct Refusal {
    AdmissionCode code;
    std::string_view reason;
    friend bool operator==(const Refusal&, const Refusal&) = default;
};

/** A2 kind rule: Ok for PLACE, CANCEL and TRANSFER (exactly KindSupported);
 * Unsupported with the task ID for CANCEL_ALL (M3-ENG-09), WITHDRAW
 * (M2-LED-05), SPOT_TO_FUTURES and FUTURES_TO_SPOT (M2-LED-06);
 * INVALID_INPUT for an unregistered kind.
 */
Result<void> KindAdmissible(ActionKind kind);

/** A2 payload rule, the validity predicate's (engine ValidateSlotInputs step
 * 8): a supported kind's payload decodes exactly (ActionV2::Place, Cancel
 * or Transfer) and engine::PhaseOf accepts the action. False for every
 * other kind.
 */
bool PayloadAdmissible(const ActionV2& action);

/** A6 against the slot after `head_height`, in this order: BAD_EXPIRY
 * unless MaxDelayValid; REF_SLOT_AHEAD if ref_slot > head_height; EXPIRED
 * if head_height is UINT64_MAX (no successor) or ExpiredAt(ref_slot,
 * max_delay, head_height + 1); nullopt when the action may be included in
 * slot head_height + 1. No arithmetic can overflow, for any inputs.
 * REAL-REPLACE M2-ADM-01 (the D7 model and exhaustive tests).
 */
std::optional<Refusal> WindowRefusal(uint64_t ref_slot, uint16_t max_delay, uint64_t head_height, const ProfileParams& profile);

//! The reply to one submitted frame (see the file comment).
struct AdmissionReply {
    AdmissionCode code{AdmissionCode::MALFORMED};
    std::string_view reason;                      //!< static; the task ID for UNSUPPORTED_KIND
    std::optional<ActionId> id;                   //!< once the frame decoded
    std::optional<PendingRef> pending;            //!< ADMITTED: the new entry; DUPLICATE of a pending action: the original
    std::optional<read::IncludedRef> included;    //!< DUPLICATE of an included action
    std::optional<read::IncludedOutcome> outcome; //!< with `included`, if the read service still holds that slot
    std::optional<uint64_t> head_height;          //!< the snapshot height, once A4 ran
};

//! Counters since Create.
struct AdmissionStats {
    uint64_t sig_verifications{0}; //!< VerifiedAction::Verify calls (A8)
    uint64_t replies{0};           //!< replies of any code
    uint64_t errors{0};            //!< Admit calls that returned an Error
    std::map<AdmissionCode, uint64_t> by_code; //!< every registered code, zeros included
    //! The count of `code` (zero for an unregistered value).
    uint64_t Count(AdmissionCode code) const;
};

class Admission
{
public:
    /** Admission over `owner` (the snapshot source), `read` (included
     * outcomes), `pool` and `budget`, which must outlive it. Validate(profile)
     * first (its errors), then INVALID_INPUT ADMISSION_PROFILE_MISMATCH
     * unless the owner, the read service and the pool use `profile`. The
     * domain is the owner's head's; it never changes for an instance.
     */
    static Result<std::unique_ptr<Admission>> Create(const ProfileParams& profile, const state::StateOwner& owner,
                                                     const read::ReadService& read, ActionPool& pool, BudgetPolicy& budget);

    Admission(const Admission&) = delete;
    Admission& operator=(const Admission&) = delete;

    //! A0..A10 for one frame of exact ActionV2 bytes (see the file comment).
    Result<AdmissionReply> Admit(std::span<const unsigned char> frame);

    /** A8 in batches of at most 64 through BatchVerifier, every other step
     * unchanged and the replies in input order. Outline stub:
     * NotImplemented(M4-ADM-01); it counts nothing.
     */
    Result<std::vector<AdmissionReply>> AdmitBatch(std::span<const std::vector<unsigned char>> frames);

    //! From now on every Admit answers HALTED (A0). Sticky.
    void Fence() { m_fenced.store(true); }
    bool Fenced() const { return m_fenced.load(); }

    AdmissionStats Stats() const;
    const uint256& Domain() const { return m_domain; }
    const ProfileParams& Profile() const { return m_profile; }

private:
    //! What one Admit did besides its reply (for the counters and the budget).
    struct Attempt {
        std::optional<BudgetRequest> budget_allowed; //!< set iff Check answered ALLOW
    };

    Admission(const ProfileParams& profile, const uint256& domain, const state::StateOwner& owner, const read::ReadService& read,
              ActionPool& pool, BudgetPolicy& budget);

    Result<AdmissionReply> Evaluate(std::span<const unsigned char> frame, Attempt& attempt);
    //! A1 and A2 for a frame whose kind byte is unknown.
    AdmissionReply UnknownKind(std::span<const unsigned char> frame) const;
    //! A DUPLICATE reply for `known`, with the certified outcome when the read service still holds it.
    Result<AdmissionReply> Duplicate(AdmissionReply reply, const KnownAction& known, const ActionV2& action) const;
    //! The read service's INCLUDED answer for `action` (by ActionId), if any.
    Result<std::optional<read::IncludedOutcome>> IncludedOutcomeOf(const ActionV2& action) const;

    const ProfileParams m_profile;
    const uint256 m_domain;
    const state::StateOwner& m_owner;
    const read::ReadService& m_read;
    ActionPool& m_pool;
    BudgetPolicy& m_budget;

    std::atomic<bool> m_fenced{false};
    std::atomic<uint64_t> m_sig_verifications{0};
    std::atomic<uint64_t> m_replies{0};
    std::atomic<uint64_t> m_errors{0};
    //! Indexed by the AdmissionCode integer (registered codes are 1..63).
    std::array<std::atomic<uint64_t>, 64> m_by_code{};
};

} // namespace flowmesh2::admission

#endif // B3_FLOWMESH2_ADMISSION_ADMISSION_H
