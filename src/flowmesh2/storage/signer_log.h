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
 *   fatal (FENCED) and has no records yet. A journal with history needs its
 *   equivocation index rebuilt first: recovery, M4-STO-01.
 *
 * Record payloads (exact codecs; recovery in M4-STO-01 reads them):
 * - ACCEPTED_PROPOSAL: ProposalKey (u64 epoch | u64 slot | u64 view) |
 *   header (366) | body (SlotBody encoding, the rest).
 * - INTENT: VoteKey (57) | digest | u64 proposal_seq: 97 bytes.
 * - SIGNED_VOTE: VoteKey | digest | signature (96) | u64 intent_seq: 193 bytes.
 *
 * Threading: a SignerLog is owned by one thread (the sealing thread,
 * section 4); it has no lock of its own and never holds the journal's.
 * Stubs: AppendSignedFolded (M4-STO-05, folded signing; strict is the only
 * mode), AppendViewState and AppendNewViewAccepted (M4-STO-07).
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

//! Target shape of a VIEW_STATE record (M4-STO-07 owns its content).
struct ViewStateRecord {
    uint64_t epoch{0};
    uint64_t view{0};
};

//! Target shape of a NEW_VIEW_ACCEPTED record (M4-STO-07 owns its content).
struct NewViewAcceptedRecord {
    uint64_t epoch{0};
    uint64_t view{0};
    std::vector<unsigned char> message;
};

class SignerLog
{
public:
    static Result<SignerLog> Create(Journal& journal);

    SignerLog(SignerLog&&) noexcept = default;
    SignerLog& operator=(SignerLog&&) noexcept = default;
    SignerLog(const SignerLog&) = delete;
    SignerLog& operator=(const SignerLog&) = delete;

    uint64_t JournalInstance() const { return m_journal->InstanceId(); }
    bool IsFatal() const { return m_journal->IsFatal(); }

    Result<PendingProposal> AppendAcceptedProposal(const ProposalKey& key, const SlotHeaderV2& header, const SlotBody& body);
    Result<PendingIntent> AppendIntent(const VoteKey& key, const uint256& digest, const PendingProposal& proposal);
    Result<PendingSignedVote> AppendSignedVote(const DurableIntent& intent, const VoteSignature& signature);
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
    //! Durable PBFT view state. Stub: M4-STO-07.
    Result<uint64_t> AppendViewState(const ViewStateRecord& record);
    //! Durable accepted new-view message. Stub: M4-STO-07.
    Result<uint64_t> AppendNewViewAccepted(const NewViewAcceptedRecord& record);

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

    Journal* m_journal;
    std::map<ProposalKey, ProposalEntry> m_proposals;
    std::map<VoteKey, VoteEntry> m_votes;
};

} // namespace flowmesh2::storage

#endif // B3_FLOWMESH2_STORAGE_SIGNER_LOG_H
