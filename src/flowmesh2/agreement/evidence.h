// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_AGREEMENT_EVIDENCE_H
#define B3_FLOWMESH2_AGREEMENT_EVIDENCE_H

#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/core/result.h>

#include <vector>

/** The evidence store (slice R10: interface and stubs; SEC-11; plan 3.4.1
 * "two conflicting certificates mean evidence plus halt").
 *
 * Target (M4-AGR-07): Record(evidence) checks it (CheckEvidence, then the
 * signatures against the seat set of its epoch), persists it durably before
 * returning, exposes it to clients, and halts signing for the instance
 * (reads stay live). Load() returns every recorded evidence in recording
 * order, also after a restart. The signer log's DECISION_CONFLICT and
 * VOTE_EQUIVOCATION refusals (storage/) are local guards; evidence is what
 * other seats did.
 *
 * Stubs M4-AGR-07: Record and Load.
 */
namespace flowmesh2::agreement {

class EvidenceStore
{
public:
    EvidenceStore() = default;

    //! Stub M4-AGR-07.
    Result<void> Record(const Evidence& evidence);
    //! Stub M4-AGR-07.
    Result<std::vector<Evidence>> Load() const;
};

} // namespace flowmesh2::agreement

#endif // B3_FLOWMESH2_AGREEMENT_EVIDENCE_H
