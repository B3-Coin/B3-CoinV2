// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_AGREEMENT_PBFT_H
#define B3_FLOWMESH2_AGREEMENT_PBFT_H

#include <flowmesh2/agreement/agreement.h>
#include <flowmesh2/agreement/evidence.h>
#include <flowmesh2/agreement/leader.h>
#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/agreement/safety.h>
#include <flowmesh2/chain/chain_adapter.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/state/state_owner.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/seat_signer.h>
#include <flowmesh2/storage/signer_log.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

/** Stable-leader PBFT over k seats (M4-AGR-01, M4-AGR-02, M4-AGR-12;
 * plan section 3.4.1; [CC] C4; test/flowmesh_v2_agreement/PROTOCOL.md;
 * ARCHITECTURE.md sections 3, 7.3 and 8). Landed 2026-11-14 (O-97).
 *
 * One slot in flight: the slot after the state owner's head. Views are per
 * slot and start at 0; the leader of (slot, view) is
 * LeaderSchedule::LeaderForView over the seat set of the slot's epoch
 * (ChainAdapter::SeatSetAt). Quorum q = SeatSet::Quorum() = floor(2k/3) + 1;
 * f + 1 = k - q + 1 reports synchronise a view change.
 *
 * The slot, for every seat (barriers of section 7.3; every durable step
 * before the signature or the message that depends on it):
 *  1. PROPOSE (the leader; Propose(candidate, authority)): preconditions
 *     (below), authority check 1, ACCEPTED_PROPOSAL, the PROPOSAL message
 *     {epoch, view, header, parent certificate, body} to every other seat;
 *     then the leader prepares like a follower.
 *  2. PREPARE (a follower, on a PROPOSAL of its leader for the current
 *     ACTIVE view): CheckProposal; the header names the head (parent hash
 *     and certificate hash) and a verifiable anchor; FACT COMPLETENESS
 *     (M4-AGR-12): the body's facts are exactly what this seat's chain
 *     adapter delivers for [facts_from, facts_to) (BUSY and NEED_DATA defer,
 *     a difference is VERIFY_FAILED PBFT_FACTS_MISMATCH and no vote ever);
 *     StateOwner::Reexecute reproduces header and canonical body;
 *     SafetyRules::MayPrepare; ACCEPTED_PROPOSAL and the PREPARE intent
 *     (PrepareDigest(domain, epoch, slot, view, header hash)), B-INTENT, authority check 2, Sign,
 *     SIGNED_VOTE, B-VOTE, the vote to every other seat (VoteSink::SendVote).
 *  3. PREPARED (q PREPARE votes for the accepted header in this view, each
 *     verified against its seat's key): the PreparedQC {view, certificate}
 *     is written (PREPARED_QC, B) and SafetyRules::MayCommit decides the
 *     COMMIT intent (CommitDigest), B-INTENT, check 2, Sign, SIGNED_VOTE,
 *     B-VOTE, the vote to every other seat. Certificates are built only
 *     from votes that verified; this seat's own votes enter them only as
 *     OutboundVotes (durable).
 *  4. DECIDED (q COMMIT votes for the accepted header, from any view, each
 *     verified): the SlotCertificate {header hash, bitmap, aggregate}
 *     passes FastAggregateVerify, DECISION (header, certificate, body),
 *     B-DECISION; TakeDecision hands it out once. The caller re-executes
 *     (followers) or keeps its candidate (the leader), applies it (check 3)
 *     and writes APPLIED.
 *  5. VIEW CHANGE (OnTimeout(now_ms) past the view's deadline, or f + 1
 *     reports for a higher target): VIEW_STATE {epoch, slot, target} is
 *     durable first, then the report {epoch, slot, target, seat, highest
 *     durable PreparedQC below target} is signed as a statement
 *     (SignerLog::AppendStatementIntent, B-INTENT, Sign, SIGNED_VOTE, B-VOTE)
 *     and sent to every other seat (VoteSink::SendStatement). The seat is
 *     CHANGING: it prepares nothing in the old view. With q reports for a
 *     target the view timer restarts; the target's leader builds NEW_VIEW
 *     {epoch, slot, target, seat, selected, reports}, where selected is the
 *     header prepared in the highest view among the reports or zero,
 *     writes VIEW_STATE and NEW_VIEW_ACCEPTED, signs and sends it, and
 *     re-proposes the selected header's body if it holds it (otherwise it
 *     waits for the caller's Propose, which must propose that header). A
 *     follower accepts a NEW_VIEW of its leader with q verified reports
 *     (VIEW_STATE, NEW_VIEW_ACCEPTED, B) and prepares the proposal of that
 *     view only if its header is the selected one (or any valid header when
 *     nothing was prepared). A proposal that arrives before its NEW_VIEW
 *     waits (one per seat). Reports are kept one per seat per target and,
 *     above the current view, one per seat (its highest target), at most
 *     2k - 1 in all, and a report or vote already held costs no
 *     verification; a seat restarted while CHANGING rebuilds the report it
 *     signed from the signer log's digest and the durable preparations
 *     (VERIFY_FAILED PBFT_REPORT_RECOVERY when none reproduces it) and
 *     resends it (review of 2026-11-16, R5 and R6).
 *
 * Safety rules the seat never breaks (PbftSafetyRules, M4-AGR-01): one
 * proposal per (epoch, slot, view); a proposal is prepared only in its
 * view, from its view's leader, after its NEW_VIEW when view > 0 and only
 * for the selected header; a COMMIT only for the accepted proposal with a
 * PreparedQC of the same view and header; the view never moves backwards
 * and never on one arbitrary report; the cross-epoch rule of O-89: a
 * height this commit log decided is never signed for another header,
 * whatever the epoch or view (PBFT_HEIGHT_DECIDED). Votes with a bad
 * signature, from the wrong seat, or for another header than the seat's
 * own earlier vote in the same key (a double vote) are refused
 * (PBFT_VOTE_SIGNATURE, PBFT_VOTE_SEAT, PBFT_DOUBLE_VOTE); evidence
 * recording is M4-AGR-07.
 *
 * Propose refuses: FENCED (PBFT_FENCED) when a journal is fatal or the
 * instance halted; INVALID_INPUT PBFT_CANDIDATE_SLOT unless the candidate
 * is for the slot after the head; PBFT_CANDIDATE_AUTHORITY unless
 * `authority` is the header's and the epoch is the slot's; PBFT_SEAT_SET
 * unless the header names the epoch's seat set; PBFT_NOT_LEADER unless this
 * seat leads (slot, view) and the view is ACTIVE; ORDER_VIOLATION
 * PBFT_PROPOSAL_PENDING when this view already has a proposal;
 * PBFT_NEW_VIEW_SELECTED when the view's NEW_VIEW selected another header;
 * EQUIVOCATION_REFUSED PBFT_HEIGHT_DECIDED (O-89); then the adapter's
 * AUTHORITY_CHANGED and the journals' errors.
 *
 * Time enters only through OnTimeout(now_ms). The view timeout is
 * view_timeout_floor_ms * min(2^view, 8) (PROVISIONAL; adaptive timeouts
 * are M4-AGR-05); every retry_ms the seat retransmits its last proposal,
 * votes and report (the mesh may have been BUSY). Message handling is
 * bounded: at most MAX_EARLY_VOTES votes per slot wait for their proposal,
 * one proposal waits for its NEW_VIEW.
 *
 * Restart (M4-STO-01, M4-STO-07): Create reads the signer log's ViewState
 * for the current slot and resumes that view (ACTIVE if its NEW_VIEW was
 * accepted or it is view 0, CHANGING otherwise, re-sending the durable
 * report), the commit log's HighestPrepared, and the parent certificate
 * from the commit journal's last DECISION.
 *
 * Inclusion lists (M4-AGR-04), adaptive timeouts (M4-AGR-05), the restart
 * fence (M4-AGR-06), evidence (M4-AGR-07) and invalid-share exclusion
 * (M4-AGR-08) are separate tasks; this file calls none of them yet.
 * The sealing thread owns a PbftAgreement; it takes no lock of its own.
 */
namespace flowmesh2::agreement {

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view PBFT_FENCED{"a journal of the agreement is fatal or the instance halted"};   //!< (FENCED)
inline constexpr std::string_view PBFT_SELF_KEY{"this seat's key is not the signer's"};                      //!< (INVALID_INPUT)
inline constexpr std::string_view PBFT_SELF_SEAT{"self_seat outside the seat set"};                         //!< (INVALID_INPUT)
inline constexpr std::string_view PBFT_CANDIDATE_SLOT{"candidate is not for the slot after the head"};       //!< (INVALID_INPUT)
inline constexpr std::string_view PBFT_CANDIDATE_AUTHORITY{"authority is not the candidate header's"};       //!< (INVALID_INPUT)
inline constexpr std::string_view PBFT_SEAT_SET{"candidate header names another seat set"};                  //!< (INVALID_INPUT)
inline constexpr std::string_view PBFT_NOT_LEADER{"this seat does not lead the current view"};               //!< (INVALID_INPUT)
inline constexpr std::string_view PBFT_PROPOSAL_PENDING{"this view already has a proposal"};                 //!< (ORDER_VIOLATION)
inline constexpr std::string_view PBFT_NEW_VIEW_SELECTED{"the view's NEW_VIEW selected another header"};     //!< (INVALID_INPUT)
inline constexpr std::string_view PBFT_HEIGHT_DECIDED{"the commit log decided this height with another header"}; //!< (EQUIVOCATION_REFUSED)
inline constexpr std::string_view PBFT_EPOCH{"message for another epoch"};                                   //!< (INVALID_INPUT)
inline constexpr std::string_view PBFT_WRONG_LEADER{"proposal or new view from a seat that does not lead the view"}; //!< (INVALID_INPUT)
inline constexpr std::string_view PBFT_PARENT{"proposal does not build on the head"};                        //!< (STALE_PARENT)
inline constexpr std::string_view PBFT_FACTS_MISMATCH{"proposal facts differ from the chain adapter's"};     //!< (VERIFY_FAILED)
inline constexpr std::string_view PBFT_VOTE_SEAT{"vote not from the seat it names"};                         //!< (INVALID_INPUT)
inline constexpr std::string_view PBFT_VOTE_SIGNATURE{"vote signature does not verify"};                     //!< (VERIFY_FAILED)
inline constexpr std::string_view PBFT_DOUBLE_VOTE{"a second header from one seat for one vote key"};         //!< (EQUIVOCATION_REFUSED)
inline constexpr std::string_view PBFT_REPORT_SIGNATURE{"view-change report signature does not verify"};     //!< (VERIFY_FAILED)
inline constexpr std::string_view PBFT_PREPARED_CERT{"a carried PreparedQC does not verify"};                //!< (VERIFY_FAILED)
inline constexpr std::string_view PBFT_PARENT_CERT{"the selection's parent certificate does not certify the head"}; //!< (VERIFY_FAILED)
inline constexpr std::string_view PBFT_NEW_VIEW_QUORUM{"new view without a quorum of reports"};              //!< (INVALID_INPUT)
inline constexpr std::string_view PBFT_CERTIFICATE{"the aggregate certificate does not verify"};             //!< (INVARIANT)
inline constexpr std::string_view PBFT_DECISION_NOT_TAKEN{"the previous decision was not taken"};            //!< (ORDER_VIOLATION)
inline constexpr std::string_view PBFT_MESSAGE_SLOT{"message for another slot"};                             //!< (INVALID_INPUT)
inline constexpr std::string_view PBFT_VIEW_STALE{"message for an older view"};                              //!< (INVALID_INPUT)
inline constexpr std::string_view PBFT_NO_PROPOSAL{"no accepted proposal in this view"};                     //!< (ORDER_VIOLATION)
inline constexpr std::string_view PBFT_PREPARED_MISMATCH{"the PreparedQC is not this view's or header's"};   //!< (INVALID_INPUT)
inline constexpr std::string_view PBFT_REPORT_RECOVERY{"the signed view-change report cannot be rebuilt from the durable preparations"}; //!< (VERIFY_FAILED)

//! Votes for a slot that arrive before its proposal are kept up to this many. PROVISIONAL.
inline constexpr size_t MAX_EARLY_VOTES{64};

/** The safety rules of one seat (M4-AGR-01). They read what the agreement
 * tells them (SetView, SetAccepted) and refuse what PROTOCOL.md forbids;
 * they never write.
 */
class PbftSafetyRules final : public SafetyRules
{
public:
    PbftSafetyRules() = default;

    //! The seat's current (epoch, slot, view); `active` false while CHANGING. `selected` is the view's NEW_VIEW selection (zero: none).
    void SetView(uint64_t epoch, uint64_t slot, uint64_t view, bool active, const uint256& selected);
    //! The header accepted in the current view, if any.
    void SetAccepted(const std::optional<uint256>& header_hash);

    //! See the file comment: ORDER_VIOLATION PBFT_VIEW_STALE / PBFT_NO_PROPOSAL, INVALID_INPUT PBFT_NEW_VIEW_SELECTED, EQUIVOCATION_REFUSED PBFT_DOUBLE_VOTE.
    Result<void> MayPrepare(const Proposal& proposal) override;
    Result<void> MayCommit(const Proposal& proposal, const PreparedCert& prepared) override;
    //! A report moves nothing by itself: Ok iff it is structurally valid and for the current slot; the agreement counts.
    Result<void> OnViewChange(const ViewChange& report) override;

private:
    uint64_t m_epoch{0};
    uint64_t m_slot{0};
    uint64_t m_view{0};
    bool m_active{true};
    uint256 m_selected;
    std::optional<uint256> m_accepted;
};

struct PbftConfig {
    uint32_t self_seat{0};              //!< this seat's index in the seat set of each epoch
    uint64_t view_timeout_floor_ms{50}; //!< plan 3.4.1 co-located floor; PROVISIONAL (M4-AGR-05)
    uint64_t retry_ms{10};              //!< retransmission period of the last proposal, votes and report; PROVISIONAL
    /** The clock reading at Create: the first slot's view timer and retry
     * period start here, and OnTimeout refuses an earlier time. A seat
     * created in the middle of a run (a restart, M4-SIM-02) passes its
     * current clock, or its first view times out at once.
     */
    uint64_t start_ms{0};
    /** Idle grace (stage 2, O-101): the first view of a slot times out
     * idle_grace_ms + TimeoutMs(0) after the slot starts unless work shows
     * up first (a proposal arrives, this seat proposes, or the node calls
     * OnLocalWork), which tightens the deadline to TimeoutMs(view) from now.
     * 0 (the default; the simulator) is the plain PBFT timer; a daemon sets
     * its heartbeat period, so an idle cluster does not churn views.
     */
    uint64_t idle_grace_ms{0};
};

//! What a PBFT instance works with; every reference must outlive it.
struct PbftContext {
    chain::ChainAdapter& adapter;     //!< authority checks 1 and 2, seat sets per epoch, facts for M4-AGR-12
    const state::StateOwner& owner;   //!< the head; Reexecute before voting
    storage::SeatSigner& signer;      //!< the only BLS signing path
    storage::SignerLog& signer_log;   //!< ACCEPTED_PROPOSAL, INTENT, SIGNED_VOTE, VIEW_STATE, NEW_VIEW_ACCEPTED
    storage::CommitLog& commit_log;   //!< DECISION, PREPARED_QC
    SafetyRules& safety;              //!< PbftSafetyRules
    MessageSink& sink;                //!< proposals (and evidence) to the seat mesh
    VoteSink& votes;                  //!< durable votes and statements to the seat mesh
    EvidenceStore& evidence;          //!< recorded safety violations; a halted store fences the agreement (M4-AGR-07)
};

//! The agreement's view of the slot in flight, for callers and tests.
struct PbftStatus {
    uint64_t slot{0};
    uint64_t epoch{0};
    uint64_t view{0};
    bool changing{false};
    bool leader{false};          //!< this seat leads (slot, view)
    bool proposal_accepted{false};
    bool prepared{false};
    bool decided{false};         //!< a decision is waiting in TakeDecision
    bool halted{false};          //!< evidence was recorded (here or received): this seat signs and votes no more (M4-AGR-07)
    uint32_t prepare_votes{0};
    uint32_t commit_votes{0};
    bool selected_missing{false}; //!< a NEW_VIEW selected a header this seat does not hold (the leader must fetch it, O-101)
    uint64_t ahead_hint{0};       //!< AheadHint() or 0: f + 1 peers claim a slot beyond this one (catch-up, O-101)
    uint32_t reports_held{0};     //!< view-change reports kept for this slot over every target: at most 2k - 1 (R6)

    friend bool operator==(const PbftStatus&, const PbftStatus&) = default;
};

/** `cert` carries at least a quorum of `seats` and its aggregate verifies
 * over `digest` (VERIFY_FAILED `detail` otherwise; the bitmap's own errors
 * as they are). The agreement's and the catch-up's certificate check.
 */
Result<void> CheckCertificate(const SlotCertificate& cert, const uint256& digest, const SeatSet& seats, std::string_view detail);

class PbftAgreement final : public Agreement
{
public:
    /** Validate(profile), then INVALID_INPUT PBFT_SELF_SEAT if self_seat is
     * outside the current epoch's seat set (ChainAdapter::SeatSetAt), and
     * PBFT_SELF_KEY if that seat's key is not the signer's. Reads the signer
     * log's view state and the commit journal for the parent certificate.
     */
    static Result<std::unique_ptr<PbftAgreement>> Create(const ProfileParams& profile, const PbftConfig& config, const PbftContext& context);

    Result<void> Propose(const state::Candidate& candidate, const AuthorityVersion& authority) override;
    Result<void> OnMessage(uint32_t from_seat, std::span<const unsigned char> bytes) override;
    Result<void> OnTimeout(uint64_t now_ms) override;
    std::optional<Decision> TakeDecision() override;

    //! The slot in flight, after syncing with the head.
    PbftStatus Status();
    //! True iff this seat leads the current view of the slot after the head (and the view is ACTIVE).
    bool IsLeader();
    //! The header a NEW_VIEW of the current view selected for re-proposal; zero or nullopt when none.
    std::optional<uint256> SelectedHeader();
    //! The slot the last message given to OnMessage was about (a proposal's height, a vote's, report's or new view's slot); nullopt
    //! for evidence or before any message. The node uses it to push a decision to a peer still working on a decided slot (O-105 (10)).
    std::optional<uint64_t> LastMessageSlot() const { return m_last_message_slot; }
    //! The proposal of `header_hash` this seat holds for the slot in flight (accepted or seen), if any.
    std::optional<Proposal> KnownProposal(const uint256& header_hash);
    /** The new leader re-proposes the selected header from a fetched body
     * (O-101): the header must be the selection (PBFT_NEW_VIEW_SELECTED),
     * build on the head and reproduce exactly (Reexecute), its facts must be
     * the adapter's (CheckFacts); then the leader's path (ProposeHeaderBody).
     * The header binds its proposer's parent certificate (parent_cert_hash),
     * and two seats may hold two different valid certificates for one parent
     * (different quorums of COMMIT votes), so the re-proposal carries the
     * selection's own `parent_cert`: it must certify the head
     * (PBFT_PARENT_CERT) and is then this seat's parent certificate too. Without
     * it the re-proposal is refused whenever this seat's certificate is another
     * one (PROPOSAL_PARENT_CERT; the stall of 2026-11-14, O-105 (9)).
     */
    Result<void> ProposeSelected(const SlotHeaderV2& header, const SlotBody& body,
                                 const std::optional<SlotCertificate>& parent_cert = std::nullopt);
    //! The node has work for the slot (pending actions or facts): tighten the first view's deadline (idle_grace_ms).
    void OnLocalWork();
    /** Catch-up hint (O-101): the slot that at least f + 1 distinct peers
     * (k - Quorum() + 1) have claimed in a message beyond this seat's slot,
     * or nullopt. Claims come from authenticated peers' PROPOSAL, VOTE,
     * VIEW_CHANGE and NEW_VIEW slots and are never verified further, so one
     * Byzantine peer alone cannot drive a fetch.
     */
    std::optional<uint64_t> AheadHint() const;
    //! A peer answered that it does not hold what its claim implied: forget its claim.
    void ResetClaim(uint32_t seat);
    const SeatSet& Seats() const { return m_seats; }

private:
    struct Slot {
        uint64_t slot{0};
        uint64_t epoch{0};
        uint64_t view{0};
        bool changing{false};
        uint256 selected;                              //!< the view's NEW_VIEW selection (zero: none)
        std::optional<Proposal> accepted;              //!< the proposal accepted in the current view
        std::optional<storage::PendingProposal> accepted_pending;
        std::optional<PreparedCert> prepared;          //!< highest durable preparation (this slot)
        std::map<uint32_t, Vote> prepares;             //!< this view, the accepted header
        std::map<uint32_t, Vote> commits;              //!< the accepted header, any view
        std::map<uint64_t, std::map<uint32_t, ViewChange>> reports; //!< by target view, then seat
        std::vector<Vote> early_votes;                 //!< votes that arrived before the proposal
        std::optional<std::pair<uint32_t, Proposal>> pending_proposal; //!< a proposal of a later view, with its sender
        std::map<uint256, Proposal> known_proposals;   //!< every proposal this seat accepted this slot, by header hash
        std::optional<OutboundVote> own_prepare;
        std::optional<OutboundVote> own_commit;
        std::optional<std::vector<unsigned char>> own_report;      //!< the encoded VIEW_CHANGE message
        std::optional<OutboundVote> own_report_statement;
        std::optional<std::vector<unsigned char>> own_new_view;    //!< the encoded NEW_VIEW message
        std::optional<OutboundVote> own_new_view_statement;
        std::optional<std::vector<unsigned char>> own_proposal;    //!< the encoded PROPOSAL message
        bool new_view_built{false};
        bool commit_voted{false};
        uint64_t deadline_ms{0};
        bool timer_running{false};
        uint64_t last_retry_ms{0};
    };

    PbftAgreement(const ProfileParams& profile, const PbftConfig& config, const PbftContext& context, SeatSet seats,
                  std::optional<SlotCertificate> parent_cert);

    // Slot and view bookkeeping.
    Result<void> SyncHead();
    void StartSlot(const state::HeadView& head);
    Result<void> ResumeViewState(const state::HeadView& head);
    uint64_t TimeoutMs(uint64_t view) const;
    //! Work showed up: the deadline becomes now + TimeoutMs(view) if that is sooner.
    void TightenDeadline();
    void NoteClaim(uint32_t from, uint64_t slot);
    Result<uint32_t> LeaderOf(uint64_t view) const;
    bool Fenced() const;
    void Tell();

    // The leader's and the follower's paths.
    Result<void> ProposeHeaderBody(const SlotHeaderV2& header, const SlotBody& body, const AuthorityVersion& authority);
    Result<void> OnProposal(uint32_t from, Proposal proposal);
    Result<void> CheckFacts(const Proposal& proposal);
    Result<void> VotePrepare(const Proposal& proposal);
    Result<void> OnVote(uint32_t from, const Vote& vote);
    Result<void> CountPrepare(const Vote& vote);
    Result<void> CountCommit(const Vote& vote);
    Result<void> VoteCommit();
    Result<void> Decide();
    Result<void> ReplayEarlyVotes();

    // View change.
    Result<void> EnterChanging(uint64_t target);
    //! The tail of EnterChanging: sign the report for `target` (SignStatement), keep and send it, count it. Also the
    //! restart whose VIEW_STATE is durable but whose report was never signed (ResumeViewState).
    Result<void> SignReport(uint64_t target);
    Result<void> OnViewChangeReport(uint32_t from, const ViewChange& report);
    Result<void> BuildNewView(uint64_t target);
    Result<void> OnNewView(uint32_t from, NewView message, std::span<const unsigned char> bytes);
    Result<void> AcceptNewView(const NewView& message, std::span<const unsigned char> bytes);
    Result<void> Retransmit();

    // Signing helpers.
    Result<OutboundVote> SignVote(const storage::VoteKey& key, const uint256& digest, const storage::PendingProposal& proposal,
                                  const AuthorityVersion& authority);
    Result<OutboundVote> SignStatement(const storage::VoteKey& key, const uint256& digest);
    Result<void> VerifySeatSignature(uint32_t seat, const uint256& digest, const storage::VoteSignature& signature,
                                     std::string_view detail) const;
    Result<SlotCertificate> AggregateVotes(const std::map<uint32_t, Vote>& votes, const uint256& header_hash, const uint256& digest) const;
    Result<void> VerifyCertificate(const SlotCertificate& cert, const uint256& digest) const;
    Result<void> SendToAll(std::span<const unsigned char> message);
    Result<void> SendVoteToAll(const OutboundVote& vote, const uint256& header_hash);
    Result<void> SendStatementToAll(const OutboundVote& statement, std::span<const unsigned char> message);
    //! Two verified PREPARE votes of one seat for two headers: record the evidence, spread it, halt (M4-AGR-07).
    Result<void> OnDoubleVote(const Vote& first, const Vote& second);
    //! Evidence from another seat: record it (verified) and halt.
    Result<void> OnEvidence(const Evidence& evidence);

    const ProfileParams m_profile;
    const PbftConfig m_config;
    chain::ChainAdapter& m_adapter;
    const state::StateOwner& m_owner;
    storage::SeatSigner& m_signer;
    storage::SignerLog& m_signer_log;
    storage::CommitLog& m_commit_log;
    SafetyRules& m_safety;
    MessageSink& m_sink;
    VoteSink& m_votes;
    EvidenceStore& m_evidence;
    SeatSet m_seats;
    uint256 m_domain;
    uint64_t m_now_ms; //!< config.start_ms, then the last OnTimeout
    std::optional<SlotCertificate> m_parent_cert; //!< the head's certificate (none at genesis)
    Slot m_slot;
    std::optional<Decision> m_decision;
    bool m_halted{false};
    std::map<uint32_t, uint64_t> m_claims; //!< per peer: the highest slot it named beyond this seat's (O-101)
    std::optional<uint64_t> m_last_message_slot;
};

} // namespace flowmesh2::agreement

#endif // B3_FLOWMESH2_AGREEMENT_PBFT_H
