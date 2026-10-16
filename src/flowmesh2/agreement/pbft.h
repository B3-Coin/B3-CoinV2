// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_AGREEMENT_PBFT_H
#define B3_FLOWMESH2_AGREEMENT_PBFT_H

#include <flowmesh2/agreement/agreement.h>
#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/agreement/safety.h>
#include <flowmesh2/chain/chain_adapter.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/state/state_owner.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/seat_signer.h>
#include <flowmesh2/storage/signer_log.h>

#include <cstdint>
#include <memory>

/** Stable-leader PBFT (slice R10: interfaces and stubs; plan section 3.4.1;
 * [CC] C4 profile; test/flowmesh_v2_agreement/PROTOCOL.md).
 *
 * Target (M4-AGR-01, M4-AGR-02): pre-prepare, prepare, commit;
 * execute-then-vote (the leader executes before proposing, followers
 * re-execute with StateOwner::Reexecute before preparing); quorum
 * floor(2k/3) + 1 of the seat set of the authority epoch
 * (ChainAdapter::SeatSetAt), never lowered; BLS aggregation of the COMMIT
 * shares into a SlotCertificate; the parent certificate travels in the
 * proposal (O-51); view change with durable view state (M4-STO-07); one slot
 * in flight. The barriers of section 7.3 apply to both phases.
 *
 * Stubs in the outline (every call is a typed NotImplemented):
 * - PbftSafetyRules::MayPrepare, MayCommit, OnViewChange: M4-AGR-01.
 * - PbftAgreement::Create: M4-AGR-02. The k = 1 dev certifier
 *   (dev/dev_certifier.h) is the only Agreement of the outline.
 *
 * PbftConfig values are PROVISIONAL: the view timeout floor of 50 ms is the
 * plan's co-located floor; adaptive timeouts are M4-AGR-05.
 */
namespace flowmesh2::agreement {

class PbftSafetyRules final : public SafetyRules
{
public:
    PbftSafetyRules() = default;

    //! Stub M4-AGR-01.
    Result<void> MayPrepare(const Proposal& proposal) override;
    //! Stub M4-AGR-01.
    Result<void> MayCommit(const Proposal& proposal, const PreparedCert& prepared) override;
    //! Stub M4-AGR-01.
    Result<void> OnViewChange(const ViewChange& report) override;
};

struct PbftConfig {
    uint32_t self_seat{0};             //!< this seat's index in the seat set of each epoch
    uint64_t view_timeout_floor_ms{50}; //!< plan 3.4.1 co-located floor; PROVISIONAL (M4-AGR-05)
};

//! What a PBFT instance works with; every reference must outlive it.
struct PbftContext {
    chain::ChainAdapter& adapter;     //!< authority checks 1 and 2, seat sets per epoch
    const state::StateOwner& owner;   //!< Reexecute before voting; the caller applies decisions
    storage::SeatSigner& signer;      //!< the only BLS signing path
    storage::SignerLog& signer_log;   //!< ACCEPTED_PROPOSAL, INTENT, SIGNED_VOTE, view state
    storage::CommitLog& commit_log;   //!< DECISION (and PREPARED_QC, M4-STO-07)
    SafetyRules& safety;              //!< PbftSafetyRules
    MessageSink& sink;                //!< the seat mesh (R13)
};

class PbftAgreement
{
public:
    PbftAgreement() = delete;
    //! Stub M4-AGR-02.
    static Result<std::unique_ptr<Agreement>> Create(const ProfileParams& profile, const PbftConfig& config, const PbftContext& context);
};

} // namespace flowmesh2::agreement

#endif // B3_FLOWMESH2_AGREEMENT_PBFT_H
