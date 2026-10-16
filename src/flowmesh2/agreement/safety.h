// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_AGREEMENT_SAFETY_H
#define B3_FLOWMESH2_AGREEMENT_SAFETY_H

#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/core/result.h>

/** PBFT safety rules (slice R10: interface; M4-AGR-01 implements it as
 * PbftSafetyRules in agreement/pbft.h).
 *
 * The rules decide whether this seat may vote, from its durable view state
 * only (VIEW_STATE, NEW_VIEW_ACCEPTED and PREPARED_QC records, M4-STO-07),
 * never from a private chain tip (test/flowmesh_v2_agreement/PROTOCOL.md,
 * transition table). Ok means "may"; a refusal names the rule. The rules
 * do not write: the caller persists the accepted proposal and the intent
 * (storage/signer_log.h) before it signs.
 *
 * - MayPrepare(proposal): a structurally valid proposal (CheckProposal) of
 *   the scheduled leader for the current, ACTIVE view; at view > 0 only
 *   with a matching, durably accepted NEW_VIEW; never a second proposal for
 *   one (epoch, slot, view).
 * - MayCommit(proposal, prepared): the accepted proposal of the view, with
 *   a complete PreparedQC of that view for its header.
 * - OnViewChange(report): a structurally valid report (CheckViewChange);
 *   moves the view state monotonically (f + 1 reports for a higher target,
 *   or a valid NEW_VIEW); never backwards and never on one arbitrary report.
 *
 * Deterministic and lock-free; the sealing thread owns it.
 */
namespace flowmesh2::agreement {

class SafetyRules
{
public:
    virtual ~SafetyRules() = default;

    virtual Result<void> MayPrepare(const Proposal& proposal) = 0;
    virtual Result<void> MayCommit(const Proposal& proposal, const PreparedCert& prepared) = 0;
    virtual Result<void> OnViewChange(const ViewChange& report) = 0;
};

} // namespace flowmesh2::agreement

#endif // B3_FLOWMESH2_AGREEMENT_SAFETY_H
