// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STORAGE_COMMIT_LOG_H
#define B3_FLOWMESH2_STORAGE_COMMIT_LOG_H

#include <flowmesh2/core/body.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/slot.h>
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

/** The commit log: decisions and applications (slice R8; ARCHITECTURE.md 7).
 *
 * Writes the COMMIT journal's records and mints DurableDecision and
 * DurableApplied tokens through Journal::Confirm:
 *
 *   AppendDecision(header, cert, body) -> PendingDecision; Barrier (B-DECISION);
 *   Confirm -> DurableDecision, which StateOwner::Apply (R9) requires; the
 *   token names the header and the certificate the record holds; Apply
 *   (check 3) returns an ApplyReceipt; AppendApplied(ApplyReceipt) ->
 *   PendingApplied; Barrier (B-APPLIED); Confirm -> DurableApplied, which
 *   ReadService::Publish (R11) and ActionPool::OnCommitted (R12) require.
 *   AppendApplied takes nothing but the receipt, so no caller can write
 *   APPLIED for a slot Apply refused (O-92).
 *
 * Rules:
 * - AppendDecision: VERIFY_FAILED (DECISION_CERT_HEADER) unless
 *   cert.header_hash == header.Hash(); VERIFY_FAILED (BODY_HEADER_MISMATCH)
 *   unless the body matches the header (BodyMatchesHeader), so the DECISION
 *   record can be re-executed after a crash (O-16). The certificate's
 *   signatures are not checked here (agreement and b3verify do that). The
 *   same header for a slot again is idempotent (the first certificate is
 *   kept: certificates are not unique, O-4b; the returned PendingDecision,
 *   and so the token, names that first certificate's hash); another header for a decided
 *   slot is EQUIVOCATION_REFUSED (DECISION_CONFLICT), conflicting
 *   certificates being evidence for M4-AGR-07.
 * - AppendApplied(receipt): VERIFY_FAILED (TOKEN_OTHER_LOG) for a receipt
 *   of a decision of another journal instance; VERIFY_FAILED
 *   (APPLIED_DECISION_UNKNOWN) when this log holds no decision with the
 *   receipt's slot, decision sequence number and header hash; VERIFY_FAILED
 *   (APPLIED_ROOT_MISMATCH) unless the receipt's state root equals the
 *   decided header's (Apply takes it from that header, so only a hash
 *   collision could trip it). The same application again is idempotent;
 *   otherwise the slot must be the successor of the last applied slot
 *   (slot 1 first), else ORDER_VIOLATION (APPLIED_OUT_OF_ORDER), the same
 *   contiguity replay requires (O-89).
 * - DecisionAt(slot): the reference of the decision this log holds for the
 *   slot, if any (used by the certifier before it signs, O-89).
 * - Every Append and Confirm of a fatal journal is FENCED.
 * - Create needs a COMMIT journal (ORDER_VIOLATION, LOG_ROLE) that is not
 *   fatal (FENCED) and has no records yet; rebuilding the decision index of
 *   a journal with history is recovery, M4-STO-01.
 *
 * Record payloads (exact codecs, used by replay in R9):
 * - DECISION: header (366) | CompactSize(len) | certificate (len bytes) |
 *   body (SlotBody encoding, the rest).
 * - APPLIED: u64 slot | header_hash | state_root (72 bytes).
 *
 * Threading: a CommitLog is owned by one thread (the sealing thread,
 * section 4); it has no lock of its own and never holds the journal's.
 * Stub: AppendPreparedQc (M4-STO-07).
 */
namespace flowmesh2::storage {

inline constexpr std::string_view BODY_HEADER_MISMATCH{"BODY_HEADER_MISMATCH"};           //!< (VERIFY_FAILED)
inline constexpr std::string_view DECISION_CERT_HEADER{"DECISION_CERT_HEADER"};           //!< (VERIFY_FAILED)
inline constexpr std::string_view DECISION_CONFLICT{"DECISION_CONFLICT"};                 //!< (EQUIVOCATION_REFUSED)
inline constexpr std::string_view APPLIED_DECISION_UNKNOWN{"APPLIED_DECISION_UNKNOWN"};   //!< (VERIFY_FAILED)
inline constexpr std::string_view APPLIED_ROOT_MISMATCH{"APPLIED_ROOT_MISMATCH"};         //!< (VERIFY_FAILED)
inline constexpr std::string_view APPLIED_OUT_OF_ORDER{"APPLIED_OUT_OF_ORDER"};           //!< (ORDER_VIOLATION)

/** True iff `body` is the body of `header`: same actions_root, facts_root
 * and number of originals (action_count). The validity predicate (R7) and
 * b3verify check the rest. Shared with SignerLog::AppendAcceptedProposal.
 */
bool BodyMatchesHeader(const SlotHeaderV2& header, const SlotBody& body);

struct DecisionRecord {
    SlotHeaderV2 header;
    SlotCertificate certificate;
    SlotBody body;

    friend bool operator==(const DecisionRecord&, const DecisionRecord&) = default;

    std::vector<unsigned char> Encode() const;
    static size_t MaxEncodedSize(const ProfileParams& profile);
    //! Exact: OVERSIZE above MaxEncodedSize or a cap, DECODE for malformed or trailing bytes.
    static Result<DecisionRecord> Decode(std::span<const unsigned char> bytes, const ProfileParams& profile);
};

inline constexpr size_t APPLIED_RECORD_BYTES{72};

struct AppliedRecord {
    uint64_t slot{0};
    uint256 header_hash;
    uint256 state_root;

    friend bool operator==(const AppliedRecord&, const AppliedRecord&) = default;

    std::vector<unsigned char> Encode() const;
    static Result<AppliedRecord> Decode(std::span<const unsigned char> bytes);
};

//! Target shape of a PREPARED_QC record (M4-STO-07 owns its content).
struct PreparedQcRecord {
    uint64_t epoch{0};
    uint64_t slot{0};
    uint64_t view{0};
    uint256 header_hash;
    std::vector<unsigned char> qc;
};

class CommitLog
{
public:
    static Result<CommitLog> Create(Journal& journal);

    CommitLog(CommitLog&&) noexcept = default;
    CommitLog& operator=(CommitLog&&) noexcept = default;
    CommitLog(const CommitLog&) = delete;
    CommitLog& operator=(const CommitLog&) = delete;

    uint64_t JournalInstance() const { return m_journal->InstanceId(); }
    bool IsFatal() const { return m_journal->IsFatal(); }

    Result<PendingDecision> AppendDecision(const SlotHeaderV2& header, const SlotCertificate& certificate, const SlotBody& body);
    Result<PendingApplied> AppendApplied(const ApplyReceipt& receipt);
    //! Journal::Barrier of the commit journal.
    Result<uint64_t> Barrier() { return m_journal->Barrier(); }
    //! Journal::Confirm: B-DECISION.
    Result<DurableDecision> Confirm(const PendingDecision& pending) const { return m_journal->Confirm(pending); }
    //! Journal::Confirm: B-APPLIED.
    Result<DurableApplied> Confirm(const PendingApplied& pending) const { return m_journal->Confirm(pending); }

    /** The decision this log holds for `slot`: its reference with the first
     * certificate's hash, or nullopt if the slot was never decided here. A
     * decided slot stays decided (also when check 3 refused to apply it), so
     * the certifier asks this before it signs a height (O-89).
     */
    std::optional<DecisionRef> DecisionAt(uint64_t slot) const;

    //! Durable PBFT view state. Stub: M4-STO-07.
    Result<uint64_t> AppendPreparedQc(const PreparedQcRecord& record);

private:
    explicit CommitLog(Journal& journal) : m_journal{&journal} {}

    struct DecisionEntry {
        uint256 header_hash;
        uint256 cert_hash; //!< of the certificate the DECISION record holds
        uint256 state_root;
        uint64_t seq{0};
        std::optional<uint64_t> applied_seq;
    };

    Journal* m_journal;
    std::map<uint64_t, DecisionEntry> m_decisions; //!< by slot
    uint64_t m_last_applied_slot{0};               //!< 0 before the first application (the genesis height)
};

} // namespace flowmesh2::storage

#endif // B3_FLOWMESH2_STORAGE_COMMIT_LOG_H
