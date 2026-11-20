// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STORAGE_SIGNER_LOG_H
#define B3_FLOWMESH2_STORAGE_SIGNER_LOG_H

#include <flowmesh2/core/body.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/durable.h>
#include <flowmesh2/storage/journal.h>
#include <flowmesh2/storage/tokens.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/** The signer log: what this seat accepted and voted (slice R8; ARCHITECTURE.md 7).
 *
 * Writes the SIGNER journal (never deleted, DUR-2) and mints DurableIntent
 * and DurableSignedVote tokens through Journal::Confirm. The strict signing
 * order of one vote (section 7.3) is
 *
 *   AppendAcceptedProposal(key, header, body) -> PendingProposal
 *   AppendIntent(vote_key, digest, proposal)  -> PendingIntent
 *   Barrier (B-INTENT); Confirm -> DurableIntent
 *   SeatSigner::Sign(DurableIntent)           -> signature  (storage/seat_signer.h)
 *   AppendSignedVote(DurableIntent, signature) -> PendingSignedVote
 *   Barrier (B-VOTE); Confirm -> DurableSignedVote -> OutboundVote::From
 *
 * Rules:
 * - AppendAcceptedProposal: INVALID_INPUT (PROPOSAL_KEY_MISMATCH) unless the
 *   header's authority_epoch and height equal the key's epoch and slot;
 *   VERIFY_FAILED (BODY_HEADER_MISMATCH) unless the body matches the header
 *   (BodyMatchesHeader, storage/commit_log.h).
 *   One proposal per ProposalKey: the same header again is idempotent (the
 *   original PendingProposal, no new record); another header is
 *   EQUIVOCATION_REFUSED (PROPOSAL_CONFLICT).
 * - AppendIntent(key, digest, proposal): INVALID_INPUT (VOTE_PHASE) for an
 *   unknown phase; VERIFY_FAILED (TOKEN_OTHER_LOG) for a proposal of another
 *   journal instance; INVALID_INPUT (INTENT_PROPOSAL_MISMATCH) unless the
 *   key's epoch, slot, view and domain are the proposal's; for the COMMIT
 *   phase INVALID_INPUT (INTENT_DIGEST_MISMATCH) unless digest is the
 *   proposal header's CommitDigest() (the PREPARE digest is M4-AGR-02's, so
 *   a PREPARE intent is bound to its proposal by key only). Equivocation:
 *   a VoteKey maps to one digest forever; the same digest again is
 *   idempotent (the original PendingIntent, no new record), another digest
 *   is EQUIVOCATION_REFUSED (VOTE_EQUIVOCATION). A new view is a new key.
 * - AppendSignedVote(intent, signature): VERIFY_FAILED (TOKEN_OTHER_LOG) for
 *   a token of another journal instance, VERIFY_FAILED (INTENT_UNKNOWN) when
 *   this log holds no such intent. The same signature again is idempotent;
 *   another is EQUIVOCATION_REFUSED (SIGNED_VOTE_CONFLICT). The log does not
 *   verify the signature: SeatSigner made it, and peers verify it.
 * - Every Append and Confirm of a fatal journal is FENCED.
 * - Create needs a SIGNER journal (ORDER_VIOLATION, LOG_ROLE) that is not
 *   fatal (FENCED) and has no records yet (ORDER_VIOLATION, LOG_HAS_HISTORY
 *   otherwise); Recover rebuilds the indexes of a journal with history
 *   (M4-STO-01).
 *
 * Record payloads (exact codecs; Recover reads them):
 * - ACCEPTED_PROPOSAL: ProposalKey (u64 epoch | u64 slot | u64 view) |
 *   header (366) | body (SlotBody encoding, the rest).
 * - INTENT: VoteKey (57) | digest | u64 proposal_seq: 97 bytes.
 * - SIGNED_VOTE: VoteKey | digest | signature (96) | u64 intent_seq: 193 bytes.
 *
 * Lifetime: a SeatSigner keeps a pointer to its SignerLog, so a log that a
 * signer uses must not be moved or destroyed while the signer lives.
 * Threading: a SignerLog is owned by one thread (the sealing thread,
 * section 4); it has no lock of its own and never holds the journal's.
 * View state (M4-STO-07): AppendViewState and AppendNewViewAccepted write
 * VIEW_STATE and NEW_VIEW_ACCEPTED (codecs above); ViewState and
 * NewViewAccepted read the index, which Recover rebuilds.
 * Stub: AppendSignedFolded (M4-STO-05, folded signing; strict is the only mode).
 */
namespace flowmesh2::storage {

inline constexpr std::string_view PROPOSAL_KEY_MISMATCH{"PROPOSAL_KEY_MISMATCH"};       //!< (INVALID_INPUT)
inline constexpr std::string_view PROPOSAL_CONFLICT{"PROPOSAL_CONFLICT"};               //!< (EQUIVOCATION_REFUSED)
inline constexpr std::string_view VOTE_PHASE{"VOTE_PHASE"};                             //!< (INVALID_INPUT)
inline constexpr std::string_view INTENT_PROPOSAL_MISMATCH{"INTENT_PROPOSAL_MISMATCH"}; //!< (INVALID_INPUT)
inline constexpr std::string_view INTENT_DIGEST_MISMATCH{"INTENT_DIGEST_MISMATCH"};     //!< (INVALID_INPUT)
inline constexpr std::string_view VOTE_EQUIVOCATION{"VOTE_EQUIVOCATION"};               //!< (EQUIVOCATION_REFUSED)
inline constexpr std::string_view INTENT_UNKNOWN{"INTENT_UNKNOWN"};                     //!< (VERIFY_FAILED)
inline constexpr std::string_view SIGNED_VOTE_CONFLICT{"SIGNED_VOTE_CONFLICT"};         //!< (EQUIVOCATION_REFUSED)
inline constexpr std::string_view VIEW_STATE_BACKWARDS{"VIEW_STATE_BACKWARDS"};         //!< (ORDER_VIOLATION) a view state at or below the current (slot, view) that is not the same record
inline constexpr std::string_view NEW_VIEW_CONFLICT{"NEW_VIEW_CONFLICT"};               //!< (EQUIVOCATION_REFUSED) another NEW_VIEW bundle for an (epoch, slot, view) already accepted
inline constexpr std::string_view NEW_VIEW_EMPTY_MESSAGE{"NEW_VIEW_EMPTY_MESSAGE"};     //!< (INVALID_INPUT) an accepted NEW_VIEW without its message bytes
//! Largest NEW_VIEW message the signer log keeps. PROVISIONAL (the agreement's codec caps it lower).
inline constexpr size_t MAX_NEW_VIEW_MESSAGE_BYTES{1024 * 1024};
inline constexpr size_t VIEW_STATE_RECORD_BYTES{24};

struct AcceptedProposalRecord {
    ProposalKey key;
    SlotHeaderV2 header;
    SlotBody body;

    friend bool operator==(const AcceptedProposalRecord&, const AcceptedProposalRecord&) = default;

    std::vector<unsigned char> Encode() const;
    static size_t MaxEncodedSize(const ProfileParams& profile);
    static Result<AcceptedProposalRecord> Decode(std::span<const unsigned char> bytes, const ProfileParams& profile);
};

inline constexpr size_t INTENT_RECORD_BYTES{VOTE_KEY_BYTES + 32 + 8};
inline constexpr size_t SIGNED_VOTE_RECORD_BYTES{VOTE_KEY_BYTES + 32 + bls::SIGNATURE_SIZE + 8};

struct IntentRecord {
    VoteKey key;
    uint256 digest;
    uint64_t proposal_seq{0};

    friend bool operator==(const IntentRecord&, const IntentRecord&) = default;

    std::vector<unsigned char> Encode() const;
    static Result<IntentRecord> Decode(std::span<const unsigned char> bytes);
};

struct SignedVoteRecord {
    VoteKey key;
    uint256 digest;
    VoteSignature signature{};
    uint64_t intent_seq{0};

    friend bool operator==(const SignedVoteRecord&, const SignedVoteRecord&) = default;

    std::vector<unsigned char> Encode() const;
    static Result<SignedVoteRecord> Decode(std::span<const unsigned char> bytes);
};

/** VIEW_STATE (M4-STO-07): the seat entered `view` of `slot` under `epoch`
 * (PROTOCOL.md "persist view+1 CHANGING" and the ACTIVE entry a NEW_VIEW
 * gives). Durable before the view-change report's intent is signed. 24 bytes:
 * u64 epoch | u64 slot | u64 view.
 */
struct ViewStateRecord {
    uint64_t epoch{0};
    uint64_t slot{0};
    uint64_t view{0};

    friend bool operator==(const ViewStateRecord&, const ViewStateRecord&) = default;

    std::vector<unsigned char> Encode() const;
    static Result<ViewStateRecord> Decode(std::span<const unsigned char> bytes);
};

/** NEW_VIEW_ACCEPTED (M4-STO-07): the exact NEW_VIEW message the seat
 * accepted for (epoch, slot, view); a proposal of that view is accepted only
 * against it ("an accepted NEW_VIEW is exact"). u64 epoch | u64 slot | u64
 * view | CompactSize(len) | message (1 .. MAX_NEW_VIEW_MESSAGE_BYTES).
 */
struct NewViewAcceptedRecord {
    uint64_t epoch{0};
    uint64_t slot{0};
    uint64_t view{0};
    std::vector<unsigned char> message;

    friend bool operator==(const NewViewAcceptedRecord&, const NewViewAcceptedRecord&) = default;

    std::vector<unsigned char> Encode() const;
    static Result<NewViewAcceptedRecord> Decode(std::span<const unsigned char> bytes);
};

class SignerLog
{
public:
    //! A log over an empty SIGNER journal; a journal with records is ORDER_VIOLATION (LOG_HAS_HISTORY): use Recover.
    static Result<SignerLog> Create(Journal& journal);
    /** A log over a SIGNER journal with history (M4-STO-01): rebuilds the
     * proposal and equivocation indexes from Journal::ReadAll, so re-appends
     * are idempotent against the history and another digest for a VoteKey
     * is refused exactly as before the restart. ORDER_VIOLATION (LOG_ROLE)
     * for another role, FENCED for a fatal journal, the record decoders'
     * errors for a payload that does not decode, VERIFY_FAILED
     * (RECOVER_INCONSISTENT) for a history this log would never have written
     * (an intent without its proposal or with another digest for its key, a
     * signed vote without its intent or with another signature, two
     * proposals for one key). Records of M4-STO-07 are NotImplemented until
     * it lands. An empty journal recovers to the same log Create gives.
     */
    static Result<SignerLog> Recover(Journal& journal, const ProfileParams& profile);

    SignerLog(SignerLog&&) noexcept = default;
    SignerLog& operator=(SignerLog&&) noexcept = default;
    SignerLog(const SignerLog&) = delete;
    SignerLog& operator=(const SignerLog&) = delete;

    uint64_t JournalInstance() const { return m_journal->InstanceId(); }
    bool IsFatal() const { return m_journal->IsFatal(); }

    Result<PendingProposal> AppendAcceptedProposal(const ProposalKey& key, const SlotHeaderV2& header, const SlotBody& body);
    Result<PendingIntent> AppendIntent(const VoteKey& key, const uint256& digest, const PendingProposal& proposal);
    /** The intent of a signed STATEMENT (M4-AGR-02): a VIEW_CHANGE report, a
     * NEW_VIEW or a HIGH_WATER statement, which have no proposal. INVALID_INPUT
     * (VOTE_PHASE) for a vote phase (use AppendIntent). The equivocation rule
     * is the same: one digest per VoteKey forever (one report per target
     * view), the same digest again idempotent. The record's proposal_seq is 0.
     * SeatSigner::Sign and AppendSignedVote then work exactly as for a vote.
     */
    Result<PendingIntent> AppendStatementIntent(const VoteKey& key, const uint256& digest);
    Result<PendingSignedVote> AppendSignedVote(const DurableIntent& intent, const VoteSignature& signature);
    //! The signature this log holds for `key` (a vote or a statement), if it was signed.
    std::optional<VoteSignature> SignedStatement(const VoteKey& key) const;
    /** The signed record of `key` (its digest, signature and intent), if it
     * was signed: what a restarted seat needs to rebuild and resend the
     * statement it signed before (review of 2026-11-16, R5). Durable by
     * construction: the log read it back from its journal.
     */
    std::optional<SignedVoteRecord> SignedVote(const VoteKey& key) const;
    /** The last ACCEPTED_PROPOSAL this log appended or recovered (journal
     * order), header and body, if its key is `key` (ARCHITECTURE.md 17.3 item
     * 15(a)): a leader restarted in the view it proposed in resends exactly
     * that proposal instead of a second header for the key. Only the last
     * record is kept in memory (one body, at most
     * AcceptedProposalRecord::MaxEncodedSize); an older key answers nullopt
     * although the journal holds it. A copy each call: for the restart path.
     */
    std::optional<AcceptedProposalRecord> AcceptedProposal(const ProposalKey& key) const;
    //! Journal::Barrier of the signer journal (B-INTENT, B-VOTE).
    Result<uint64_t> Barrier() { return m_journal->Barrier(); }
    //! Journal::Confirm: B-INTENT.
    Result<DurableIntent> Confirm(const PendingIntent& pending) const { return m_journal->Confirm(pending); }
    //! Journal::Confirm: B-VOTE.
    Result<DurableSignedVote> Confirm(const PendingSignedVote& pending) const { return m_journal->Confirm(pending); }

    //! True iff `intent` is an intent of this log: same instance, sequence number, key and digest.
    bool Knows(const DurableIntent& intent) const;

    //! Folded signing (sign, then one barrier). Stub: M4-STO-05.
    Result<PendingSignedVote> AppendSignedFolded(const VoteKey& key, const uint256& digest, const PendingProposal& proposal,
                                                 const VoteSignature& signature);
    /** The seat enters (slot, view) (M4-STO-07): the record's sequence
     * number. (slot, view) moves forward only: the same record again is
     * idempotent (its original sequence number, nothing new); any record at
     * or below the current (slot, view) that is not the same is
     * ORDER_VIOLATION (VIEW_STATE_BACKWARDS). FENCED on a fatal journal.
     * The caller barriers before it signs the report of that view.
     */
    Result<uint64_t> AppendViewState(const ViewStateRecord& record);
    //! The current view state, if any VIEW_STATE record exists.
    std::optional<ViewStateRecord> ViewState() const { return m_view_state; }
    /** The exact NEW_VIEW the seat accepted for (epoch, slot, view)
     * (M4-STO-07). The same message again is idempotent; another message for
     * that key is EQUIVOCATION_REFUSED (NEW_VIEW_CONFLICT); an empty message
     * or one above MAX_NEW_VIEW_MESSAGE_BYTES is INVALID_INPUT or OVERSIZE.
     */
    Result<uint64_t> AppendNewViewAccepted(const NewViewAcceptedRecord& record);
    //! The accepted NEW_VIEW message of (epoch, slot, view), if any.
    std::optional<std::vector<unsigned char>> NewViewAccepted(uint64_t epoch, uint64_t slot, uint64_t view) const;

private:
    explicit SignerLog(Journal& journal) : m_journal{&journal} {}

    struct ProposalEntry {
        uint256 header_hash;
        uint256 domain;
        uint256 commit_digest;
        uint64_t seq{0};
    };
    struct VoteEntry {
        uint256 digest;
        uint64_t intent_seq{0};
        std::optional<VoteSignature> signature;
        uint64_t signed_seq{0};
    };

    //! The journal sequence number of the record that set m_view_state.
    struct NewViewEntry {
        std::vector<unsigned char> message;
        uint64_t seq{0};
    };

    Journal* m_journal;
    std::map<ProposalKey, ProposalEntry> m_proposals;
    std::optional<AcceptedProposalRecord> m_last_proposal; //!< the last ACCEPTED_PROPOSAL appended or recovered
    std::map<VoteKey, VoteEntry> m_votes;
    std::optional<ViewStateRecord> m_view_state;
    uint64_t m_view_state_seq{0};
    std::map<ProposalKey, NewViewEntry> m_new_views; //!< by (epoch, slot, view)
};

} // namespace flowmesh2::storage

#endif // B3_FLOWMESH2_STORAGE_SIGNER_LOG_H
