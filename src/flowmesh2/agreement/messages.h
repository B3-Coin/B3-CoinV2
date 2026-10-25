// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_AGREEMENT_MESSAGES_H
#define B3_FLOWMESH2_AGREEMENT_MESSAGES_H

#include <flowmesh2/core/body.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/storage/tokens.h>
#include <flowmesh2/storage/control_types.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

/** Agreement messages between seats (slice R10; ARCHITECTURE.md section 3;
 * plan section 3.4.1; test/flowmesh_v2_agreement/PROTOCOL.md).
 *
 * The codecs are real and capped; what the messages mean and when a seat may
 * send one is the PBFT task's (M4-AGR-01, M4-AGR-02). The k = 1 dev
 * certifier sends none (dev/dev_certifier.h). Signatures are carried as
 * opaque 96-byte compressed BLS G2 points: decoding never parses them as
 * curve points (b3verify and the agreement do, bls::Signature::Decode).
 *
 * Envelope: u8 MESSAGE_VERSION (1) | u8 MessageType | payload. Integers are
 * little-endian, hashes 32 raw bytes, counts canonical CompactSize checked
 * against their cap BEFORE anything is allocated.
 *
 *   PROPOSAL     u64 epoch | u64 view | header (366)
 *                | CompactSize(cert_len) | parent certificate (cert_len bytes;
 *                  0 = none, the parent is the genesis head)
 *                | body (SlotBody encoding under the profile's caps, the rest)
 *   VOTE         u8 phase | u64 epoch | u64 view | u64 slot | header_hash
 *                | u32 seat | signature (96)
 *                | CompactSize(n <= MAX_VOTE_INCLUSION_IDS) | n short IDs (6 B)
 *   VIEW_CHANGE  u64 epoch | u64 slot | u64 new_view | u32 seat
 *                | u8 has_prepared (0 or 1)
 *                | [u64 prepared view | CompactSize(cert_len) | certificate]
 *                | signature (96)
 *   NEW_VIEW     u64 epoch | u64 slot | u64 view | u32 seat | selected (32)
 *                | CompactSize(n <= MAX_NEW_VIEW_REPORTS)
 *                | n x (CompactSize(len) | VIEW_CHANGE payload) | signature (96)
 *   EVIDENCE     u8 kind | DOUBLE_VOTE: 2 x (CompactSize(len) | VOTE payload)
 *                | CONFLICTING_CERTIFICATES: 2 x (header (366)
 *                  | CompactSize(cert_len) | certificate)
 *
 * DecodeMessage(bytes, profile) is exact: OVERSIZE above MaxMessageBytes,
 * above the longest payload of its type, or above a cap (more than MAX_VOTE_INCLUSION_IDS short IDs, more than
 * MAX_NEW_VIEW_REPORTS reports, a certificate or nested message longer than
 * its maximum encoding, a body outside the profile's caps); DECODE for an
 * unknown version, message type, vote phase or evidence kind, a seat index
 * at or above MAX_SEATS, has_prepared other than 0 or 1, malformed or
 * non-canonical bytes and trailing bytes. EncodeMessage(message, profile)
 * produces exactly the bytes DecodeMessage accepts: it encodes, then refuses
 * (with the decoder's error) any value the decoder would refuse, and returns
 * INVARIANT if the bytes would not decode back to the same message.
 *
 * The Check* functions are the structural rules a receiver applies after
 * decoding, each INVALID_INPUT with its detail; none of them verifies a
 * signature or needs the seat set (that is M4-AGR-01/02):
 * - CheckProposal: a slot after genesis (PROPOSAL_GENESIS); epoch equal to
 *   the header's authority_epoch (PROPOSAL_EPOCH); a parent certificate iff
 *   the header names one, naming the header's parent and hashing to its
 *   parent_cert_hash (PROPOSAL_PARENT_CERT); a body that matches the header
 *   (PROPOSAL_BODY: storage::BodyMatchesHeader).
 * - CheckVote: a slot after genesis (VOTE_GENESIS); short IDs strictly
 *   ascending (VOTE_INCLUSION_ORDER); only a PREPARE vote carries them
 *   (VOTE_INCLUSION_PHASE; plan 3.4.1 inclusion lists).
 * - CheckViewChange: a target view of at least 1 (VIEW_CHANGE_TARGET); a
 *   carried preparation from a strictly older view (VIEW_CHANGE_PREPARED,
 *   PROTOCOL.md: "a report may only carry preparation from below its
 *   target").
 * - CheckNewView: a view of at least 1 (NEW_VIEW_TARGET); at least one
 *   report (NEW_VIEW_EMPTY), each passing CheckViewChange, for this epoch,
 *   slot and view (NEW_VIEW_REPORT_MISMATCH), from strictly ascending seats
 *   (NEW_VIEW_REPORT_ORDER); two reports carrying different headers prepared
 *   in the same highest view (NEW_VIEW_PREPARED_CONFLICT; a halt in
 *   PROTOCOL.md); if any report carries a preparation, `selected` is the
 *   header prepared in the highest view (NEW_VIEW_SELECTED). Whether the
 *   reports reach a quorum needs the seat set (M4-AGR-02).
 * - CheckEvidence: DOUBLE_VOTE: two votes of one seat for one (phase, epoch,
 *   view, slot) with different header hashes (EVIDENCE_NOT_CONFLICTING);
 *   CONFLICTING_CERTIFICATES: two headers of one domain and height, each
 *   named by its certificate (EVIDENCE_CERT_HEADER), with different hashes
 *   (EVIDENCE_NOT_CONFLICTING). Both in ascending header-hash order, so one
 *   conflict has one encoding (EVIDENCE_ORDER).
 *
 * PROVISIONAL (O-57): the shapes follow the R10 slice text and PROTOCOL.md;
 * M4-AGR-02 owns their meaning, the digests the signatures cover (only the
 * COMMIT digest, SlotHeaderV2::CommitDigest, is defined in the outline),
 * whether a proposal carries a leader signature or relies on the seat
 * session (M4-NET-01), and whether the inclusion IDs are signed.
 *
 * Deterministic: no clock, I/O, lock or randomness.
 */
namespace flowmesh2::agreement {

inline constexpr uint8_t MESSAGE_VERSION{1};
inline constexpr size_t SHORT_ID_BYTES{6};
//! A 6-byte salted SipHash short ID of an action (BIP152 pattern; M4-NET-02).
using ShortId = std::array<unsigned char, SHORT_ID_BYTES>;
//! Inclusion-list short IDs per prepare vote (plan 3.4.1). PROVISIONAL.
inline constexpr size_t MAX_VOTE_INCLUSION_IDS{32};
//! VIEW_CHANGE reports per NEW_VIEW: at most one per seat.
inline constexpr size_t MAX_NEW_VIEW_REPORTS{MAX_SEATS};

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view MESSAGE_VERSION_UNKNOWN{"unknown message version"};                   //!< (DECODE)
inline constexpr std::string_view MESSAGE_TYPE_UNKNOWN{"unknown message type"};                         //!< (DECODE)
inline constexpr std::string_view MESSAGE_NOT_CANONICAL{"message does not re-encode to itself"};       //!< (INVARIANT)
inline constexpr std::string_view PROPOSAL_GENESIS{"proposal for the genesis height"};                  //!< (INVALID_INPUT)
inline constexpr std::string_view PROPOSAL_EPOCH{"proposal epoch is not the header's"};                 //!< (INVALID_INPUT)
inline constexpr std::string_view PROPOSAL_PARENT_CERT{"parent certificate does not match the header"}; //!< (INVALID_INPUT)
inline constexpr std::string_view PROPOSAL_BODY{"proposal body does not match the header"};             //!< (INVALID_INPUT)
inline constexpr std::string_view VOTE_GENESIS{"vote for the genesis height"};                          //!< (INVALID_INPUT)
inline constexpr std::string_view VOTE_INCLUSION_ORDER{"inclusion IDs not strictly ascending"};         //!< (INVALID_INPUT)
inline constexpr std::string_view VOTE_INCLUSION_PHASE{"inclusion IDs on a vote that is not PREPARE"};  //!< (INVALID_INPUT)
inline constexpr std::string_view VIEW_CHANGE_TARGET{"view change to view 0"};                          //!< (INVALID_INPUT)
inline constexpr std::string_view VIEW_CHANGE_PREPARED{"preparation not from below the target view"};   //!< (INVALID_INPUT)
inline constexpr std::string_view NEW_VIEW_TARGET{"new view 0"};                                        //!< (INVALID_INPUT)
inline constexpr std::string_view NEW_VIEW_EMPTY{"new view without reports"};                           //!< (INVALID_INPUT)
inline constexpr std::string_view NEW_VIEW_REPORT_MISMATCH{"report for another epoch, slot or view"};   //!< (INVALID_INPUT)
inline constexpr std::string_view NEW_VIEW_REPORT_ORDER{"report seats not strictly ascending"};         //!< (INVALID_INPUT)
inline constexpr std::string_view NEW_VIEW_PREPARED_CONFLICT{"two headers prepared in the highest view"}; //!< (INVALID_INPUT)
inline constexpr std::string_view NEW_VIEW_SELECTED{"selected is not the highest prepared header"};    //!< (INVALID_INPUT)
inline constexpr std::string_view EVIDENCE_NOT_CONFLICTING{"evidence does not show a conflict"};        //!< (INVALID_INPUT)
inline constexpr std::string_view EVIDENCE_CERT_HEADER{"evidence certificate names another header"};    //!< (INVALID_INPUT)
inline constexpr std::string_view EVIDENCE_ORDER{"evidence not in ascending header-hash order"};        //!< (INVALID_INPUT)

//! Message types. APPEND-ONLY registry; zero is never a value.
enum class MessageType : uint8_t {
    PROPOSAL = 1,
    VOTE = 2,
    VIEW_CHANGE = 3,
    NEW_VIEW = 4,
    EVIDENCE = 5,
    CERTIFIED_PROPOSAL = 6, //!< exact final certificate and its available proposal (regtest PBFT)
    CONTROL_REPORT = 7,
    CONTROL_NEW_VIEW = 8,
    OFFER = 9,
};

//! Evidence kinds. APPEND-ONLY registry; zero is never a value.
enum class EvidenceKind : uint8_t {
    DOUBLE_VOTE = 1,              //!< one seat signed two headers for one vote key
    CONFLICTING_CERTIFICATES = 2, //!< two certified headers for one height
};

bool IsKnown(MessageType type);
bool IsKnown(EvidenceKind kind);
//! Registry names; "UNKNOWN" for an unregistered value.
std::string_view ToString(MessageType type);
std::string_view ToString(EvidenceKind kind);

//! A leader's proposal of one slot.
struct Proposal {
    uint64_t epoch{0};
    uint64_t view{0};
    SlotHeaderV2 header;
    //! The certificate of the parent (absent when the parent is the genesis head).
    std::optional<SlotCertificate> parent_cert;
    SlotBody body;

    friend bool operator==(const Proposal&, const Proposal&) = default;
};

//! One seat's PREPARE or COMMIT vote for a header.
struct Vote {
    storage::VotePhase phase{storage::VotePhase::COMMIT};
    uint64_t epoch{0};
    uint64_t view{0};
    uint64_t slot{0};
    uint256 header_hash;
    uint32_t seat{0};
    storage::VoteSignature signature{};
    std::vector<ShortId> inclusion; //!< PREPARE only; strictly ascending

    friend bool operator==(const Vote&, const Vote&) = default;
};

//! A PreparedQC: a quorum of PREPARE signatures over one header in one view.
struct PreparedCert {
    uint64_t view{0};
    SlotCertificate cert;

    friend bool operator==(const PreparedCert&, const PreparedCert&) = default;
};

//! One seat's report that it moves to `new_view` (PROTOCOL.md VIEW_CHANGE).
struct ViewChange {
    uint64_t epoch{0};
    uint64_t slot{0};
    uint64_t new_view{0};
    uint32_t seat{0};
    std::optional<PreparedCert> prepared; //!< its highest durable preparation below new_view
    storage::VoteSignature signature{};

    friend bool operator==(const ViewChange&, const ViewChange&) = default;
};

//! The new leader's justification of view `view` (PROTOCOL.md NEW_VIEW).
struct NewView {
    uint64_t epoch{0};
    uint64_t slot{0};
    uint64_t view{0};
    uint32_t seat{0};
    uint256 selected;                //!< the header it re-proposes (zero: nothing prepared)
    std::vector<ViewChange> reports; //!< strictly ascending seats
    storage::VoteSignature signature{};

    friend bool operator==(const NewView&, const NewView&) = default;
};

struct CertifiedHeader {
    SlotHeaderV2 header;
    SlotCertificate cert;

    friend bool operator==(const CertifiedHeader&, const CertifiedHeader&) = default;
};

struct DoubleVote {
    Vote first;
    Vote second;

    friend bool operator==(const DoubleVote&, const DoubleVote&) = default;
};

struct ConflictingCertificates {
    CertifiedHeader first;
    CertifiedHeader second;

    friend bool operator==(const ConflictingCertificates&, const ConflictingCertificates&) = default;
};

//! Proof of a safety violation (SEC-11; stored and acted on by M4-AGR-07).
struct Evidence {
    std::variant<DoubleVote, ConflictingCertificates> content;

    EvidenceKind Kind() const;
    friend bool operator==(const Evidence&, const Evidence&) = default;
};

//! A transferable final proof. Its proposal supplies the exact executable body.
struct CertifiedProposal {
    Proposal proposal;
    SlotCertificate certificate;
    friend bool operator==(const CertifiedProposal&, const CertifiedProposal&) = default;
};

//! Complete transferable preparation header/proof; body is separately available.
struct ControlPrepared {
    uint64_t view{0};
    SlotHeaderV2 header;
    SlotCertificate certificate;
    friend bool operator==(const ControlPrepared&, const ControlPrepared&) = default;
};
struct ControlReport {
    storage::ControlKey key;
    uint64_t prepared_watermark{0}; //!< local durable commit prefix at atomic view entry
    std::optional<ControlPrepared> prepared;
    std::optional<CertifiedHeader> decision;
    storage::VoteSignature signature{};
    friend bool operator==(const ControlReport&, const ControlReport&) = default;
};
inline constexpr size_t MAX_CONTROL_REPORTS{REGTEST_PBFT_MAX_SEATS}; //!< at most one report per seat of a regtest PBFT (O-98)
struct ControlNewView {
    storage::ControlKey key;
    uint256 selected;
    std::vector<ControlReport> reports;
    storage::VoteSignature signature{};
    std::optional<Proposal> selected_proposal; //!< exact durable body; does not itself grant PREPARE permission
    friend bool operator==(const ControlNewView&, const ControlNewView&) = default;
};
//! Non-voting body availability and pending-work signal. It grants no vote.
struct OfferedProposal {
    Proposal proposal;
    friend bool operator==(const OfferedProposal&, const OfferedProposal&) = default;
};

using Message = std::variant<Proposal, Vote, ViewChange, NewView, Evidence, CertifiedProposal,
                             ControlReport, ControlNewView, OfferedProposal>;

MessageType TypeOf(const Message& message);

//! Longest encoding DecodeMessage accepts under `profile`.
size_t MaxMessageBytes(const ProfileParams& profile);

//! The envelope bytes; see the file comment.
Result<std::vector<unsigned char>> EncodeMessage(const Message& message, const ProfileParams& profile);
//! Exact decode; see the file comment.
Result<Message> DecodeMessage(std::span<const unsigned char> bytes, const ProfileParams& profile);

Result<void> CheckProposal(const Proposal& proposal);
Result<void> CheckVote(const Vote& vote);
Result<void> CheckViewChange(const ViewChange& report);
Result<void> CheckNewView(const NewView& new_view);
Result<void> CheckEvidence(const Evidence& evidence);

} // namespace flowmesh2::agreement

#endif // B3_FLOWMESH2_AGREEMENT_MESSAGES_H
