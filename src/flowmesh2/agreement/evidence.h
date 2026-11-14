// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_AGREEMENT_EVIDENCE_H
#define B3_FLOWMESH2_AGREEMENT_EVIDENCE_H

#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/chain/chain_adapter.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/storage/commit_log.h>

#include <string_view>
#include <vector>

/** The evidence store (slice R10 interface; landed with M4-AGR-07 on
 * 2026-11-14; SEC-11; plan 3.4.1 "two conflicting certificates mean evidence
 * plus halt"; ARCHITECTURE.md O-100).
 *
 * Create(profile, adapter, log) loads every EVIDENCE record the commit log
 * recovered (in recording order); a store that holds evidence is halted
 * from the start. Record(evidence) checks the shape (CheckEvidence), then
 * the proof:
 * - DOUBLE_VOTE: both votes are PREPARE votes (a COMMIT vote signs the
 *   whole header, which the vote does not carry, so two COMMIT votes cannot
 *   be verified by a third party: EVIDENCE_UNVERIFIABLE; the recordable form
 *   of a committed conflict is CONFLICTING_CERTIFICATES), and both
 *   signatures verify over PrepareDigest(view, header_hash) with the seat's
 *   key of the epoch's seat set (EVIDENCE_SIGNATURE);
 * - CONFLICTING_CERTIFICATES: each certificate carries at least a quorum of
 *   its header's epoch and its aggregate verifies over the header's
 *   CommitDigest (EVIDENCE_CERTIFICATE); the two epochs may differ (O-89: a
 *   height is never decided twice, whatever the epoch).
 * Then it appends the encoded evidence as an EVIDENCE record of the commit
 * journal and barriers it, so the halt survives a restart, and halts: every
 * agreement over this store refuses to sign or vote (reads stay live). The
 * same evidence again is idempotent (nothing appended). Load() returns the
 * recorded evidence in recording order.
 *
 * The signer log's DECISION_CONFLICT and VOTE_EQUIVOCATION refusals
 * (storage/) are local guards; evidence is what other seats did.
 */
namespace flowmesh2::agreement {

inline constexpr std::string_view EVIDENCE_UNVERIFIABLE{"a COMMIT double vote cannot be verified from the votes alone"}; //!< (INVALID_INPUT)
inline constexpr std::string_view EVIDENCE_SIGNATURE{"an evidence vote signature does not verify"};                  //!< (VERIFY_FAILED)
inline constexpr std::string_view EVIDENCE_CERTIFICATE{"an evidence certificate does not verify"};                    //!< (VERIFY_FAILED)
inline constexpr std::string_view EVIDENCE_SEAT{"an evidence vote names a seat outside the epoch's seat set"};       //!< (INVALID_INPUT)
inline constexpr std::string_view EVIDENCE_RECORD{"an EVIDENCE record does not decode as evidence"};                  //!< (DECODE)

class EvidenceStore
{
public:
    //! Load the recorded evidence from `log` (its errors, DECODE EVIDENCE_RECORD for a foreign record); halted if any.
    static Result<EvidenceStore> Create(const ProfileParams& profile, const chain::ChainAdapter& adapter, storage::CommitLog& log);

    EvidenceStore(EvidenceStore&&) noexcept = default;
    EvidenceStore& operator=(EvidenceStore&&) noexcept = default;
    EvidenceStore(const EvidenceStore&) = delete;
    EvidenceStore& operator=(const EvidenceStore&) = delete;

    //! Check, verify, persist (durably), halt. See the file comment.
    Result<void> Record(const Evidence& evidence);
    //! Every recorded evidence, in recording order.
    Result<std::vector<Evidence>> Load() const { return Ok(m_recorded); }
    //! True once any evidence is recorded (here or in a recovered journal).
    bool Halted() const { return !m_recorded.empty(); }
    size_t Size() const { return m_recorded.size(); }

private:
    EvidenceStore(const ProfileParams& profile, const chain::ChainAdapter& adapter, storage::CommitLog& log)
        : m_profile{profile}, m_adapter{&adapter}, m_log{&log}
    {
    }
    Result<void> Verify(const Evidence& evidence) const;

    ProfileParams m_profile;
    const chain::ChainAdapter* m_adapter;
    storage::CommitLog* m_log;
    std::vector<Evidence> m_recorded;
};

} // namespace flowmesh2::agreement

#endif // B3_FLOWMESH2_AGREEMENT_EVIDENCE_H
