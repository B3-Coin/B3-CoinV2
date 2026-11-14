// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_SEAT_SEAT_NODE_H
#define B3_FLOWMESH2_SEAT_SEAT_NODE_H

#include <flowmesh2/admission/admission.h>
#include <flowmesh2/admission/budget.h>
#include <flowmesh2/admission/pool.h>
#include <flowmesh2/agreement/agreement.h>
#include <flowmesh2/agreement/evidence.h>
#include <flowmesh2/agreement/pbft.h>
#include <flowmesh2/chain/chain_adapter.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/net/in_process_api.h>
#include <flowmesh2/net/lanes.h>
#include <flowmesh2/net/seat_mesh.h>
#include <flowmesh2/read/read_service.h>
#include <flowmesh2/read/views.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/state/state_owner.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/journal.h>
#include <flowmesh2/storage/seat_signer.h>
#include <flowmesh2/storage/signer_log.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** One FlowMesh seat: the component assembly and the sealing steps that the
 * simulator (sim/sim_node.h, R14) and the daemon (daemon/, M5-NODE-01) share.
 * Extracted from sim::SimNode on 2026-11-14 (stage 2 of the owner's plan,
 * ARCHITECTURE.md section 16); SimNode is a dev wrapper over it and the
 * simulator's traces are byte-identical.
 *
 * Composition (every component real; references stay valid because each
 * lives behind a unique_ptr the node owns): the caller's SIGNER and COMMIT
 * journals (memory or file), their SignerLog and CommitLog, the evidence
 * store, the SeatSigner of this seat and the PoP-verified seat set, the
 * genesis head and its StateOwner, a ReadService, an ActionPool, the
 * caller's BudgetPolicy, Admission, a LaneSet (ProvisionalLaneLimits) and
 * the InProcessApi; the agreement: for k = 1 whatever the caller's factory
 * builds (the simulator's DevSingleSeatCertifier), for k > 1 PbftAgreement
 * over the caller's SeatMesh.
 *
 * SealSlot() (k = 1) seals the next slot, in this order (the hooks see each
 * step; ARCHITECTURE.md 14.4 P3):
 *  0. FENCED (SEAT_FENCED) while admission is fenced: a fenced node seals
 *     nothing, so after a refusal it never polls, signs or writes again
 *     (O-89); the slot window holds only slot.begin and slot.end;
 *  1. Poll(head.facts_to, MAX_SLOT_FACTS): BUSY and NEED_DATA are returned
 *     as such (no slot); AUTHORITY_CHANGED fences admission and is returned;
 *     FACTS gives the batch;
 *  2. ActionPool::TakeForSlot(head + 1, max_actions_per_slot, the hooks'
 *     withhold filter);
 *  3. StateOwner::Preview(committed head, {batch authority, anchor, range,
 *     fact bytes, taken originals});
 *  4. NodeHooks::AfterPreview (the simulator's authority bump and its
 *     commit-barrier mapping check);
 *  5. Agreement::Propose(candidate, batch authority) and TakeDecision
 *     (B-INTENT, sign, B-VOTE, B-DECISION); then NodeHooks::AfterDecision;
 *  6. StateOwner::Apply(candidate, decision, CurrentAuthority()) (check 3);
 *  7. CommitLog::AppendApplied(Apply's ApplyReceipt), Barrier, Confirm (B-APPLIED);
 *  8. SlotView::Build and ReadService::Publish;
 *  9. ActionPool::OnCommitted(DurableApplied, head, outcomes, certified body).
 * Any failure returns its error and nothing after it runs. Before Apply
 * moved the head, STORAGE_FATAL, FENCED, AUTHORITY_CHANGED,
 * EQUIVOCATION_REFUSED, INVARIANT and ORDER_VIOLATION also fence the node
 * (Admission::Fence); BUSY, NEED_DATA and a refusal of the slot's inputs
 * (for example an engine error one action causes, O-34) do not, and the
 * slot is tried again. After Apply moved the head, EVERY error of steps 7
 * to 9 fences the node, because the commit log, the read service or the
 * pool would disagree with the head on every later call (O-90). A fenced
 * node's new submissions get HALTED, reads stay live on the last
 * publication. Nothing is retried: a later SealSlot answers FENCED at step
 * 0. A DECISION that check 3 refused stays durable and final for its
 * height: the node halts at the last applied slot, which is also the head a
 * replay of its commit journal gives (O-89).
 *
 * k > 1 is driven by ProposeSlot (steps 0 to 4 and Propose), OnFrame (the
 * mesh's frames to the agreement), Tick (the agreement's timers) and
 * Complete (steps 6 to 9 for a decision the agreement reached: the kept
 * candidate for the leader, StateOwner::Reexecute for a follower).
 *
 * Threading (ARCHITECTURE.md section 4): one sealing thread owns the node,
 * its agreement, logs and signer; Pump, the API's submits and the read
 * service's reads may run on other threads (their own rules).
 */
namespace flowmesh2::seat {

struct SealedSlot;

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view SEAT_POLL_BUSY{"the chain adapter answered BUSY"};                         //!< (BUSY)
inline constexpr std::string_view SEAT_POLL_NEED_DATA{"the chain adapter has no data at the cursor"};        //!< (NEED_DATA)
inline constexpr std::string_view SEAT_POLL_AUTHORITY{"the chain adapter reported an authority change"};     //!< (AUTHORITY_CHANGED)
inline constexpr std::string_view SEAT_NO_DECISION{"Propose succeeded without a decision"};                  //!< (INVARIANT)
inline constexpr std::string_view SEAT_BAD_KEY{"the seat key does not give this seat's public key"};         //!< (INVALID_INPUT)
inline constexpr std::string_view SEAT_FENCED{"the node is fenced and seals nothing"};                       //!< (FENCED)
inline constexpr std::string_view SEAT_MULTI_SEAT{"a multi-seat node is driven by ProposeSlot, OnFrame, Tick and Complete"}; //!< (INVALID_INPUT)
inline constexpr std::string_view SEAT_NOT_LEADER{"this seat does not lead the current view"};                  //!< (INVALID_INPUT)
inline constexpr std::string_view SEAT_MESH_MISSING{"a multi-seat node needs its mesh end"};                   //!< (INVALID_INPUT)
inline constexpr std::string_view SEAT_RANGE{"self_seat outside seat_count"};                                 //!< (INVALID_INPUT)
inline constexpr std::string_view SEAT_NO_AGREEMENT{"a one-seat node needs an agreement factory"};             //!< (INVALID_INPUT)
inline constexpr std::string_view SEAT_PARTS_MISSING{"a node needs its chain adapter, journals and budget"};   //!< (INVALID_INPUT)
inline constexpr std::string_view SEAT_SET_SIZE{"seat_count differs from the seat set"};                       //!< (INVALID_INPUT)

/** What the owner of a node observes and may inject (the simulator's trace
 * and fault plan; a daemon's log). Every method has a no-op default and is
 * called on the sealing thread at exactly the point the file comment names.
 */
class NodeHooks
{
public:
    virtual ~NodeHooks() = default;
    //! A generic event (`chain.poll`, `pool.take`, `state.preview`, `state.apply`, `read.publish`, `pool.on_committed`, `slot.propose`, `pbft.refuse`, `node.fence`).
    virtual void Record(std::string_view /*kind*/, std::string /*detail*/) {}
    //! The slot window of SealSlot and Complete.
    virtual void BeginSlot(uint64_t /*height*/) {}
    virtual void EndSlot(uint64_t /*height*/, std::string_view /*result*/) {}
    //! True to keep `action` out of this proposal (the simulator's withhold fault).
    virtual bool Withhold(const ActionV2& /*action*/) { return false; }
    //! Step 4: after Preview, before Propose; an error fences as the sealing rules say.
    virtual Result<void> AfterPreview(uint64_t /*height*/) { return Ok(); }
    //! k = 1: after the decision is durable, before Apply.
    virtual void AfterDecision(uint64_t /*height*/) {}
    //! Every slot this node applied, after step 9 (the simulator keeps them).
    virtual void OnSealed(SealedSlot&& /*sealed*/) {}
};

//! One slot this node sealed: everything a later check or the simulator's N13 needs.
struct SealedSlot {
    uint64_t height{0};
    std::shared_ptr<const state::HeadView> head; //!< the committed head after the slot
    state::Candidate candidate;                  //!< a copy of the applied candidate (N13 re-applies it)
    agreement::Decision decision;                //!< the durable decision, its certificate and canonical body
    std::vector<OutcomeV2> outcomes;             //!< emission order
    std::shared_ptr<const read::SlotView> view;
    admission::CommitReport pool_report;
    std::vector<ActionId> taken;                 //!< what TakeForSlot handed to the proposal
};

//! What one successful SealSlot (or Complete) did.
struct SealReport {
    uint64_t height{0};
    uint256 header_hash;
    uint32_t originals{0};
    uint32_t outcomes{0};
};

//! What the k = 1 agreement factory gets (the simulator builds its DevSingleSeatCertifier from it).
struct AgreementParts {
    const ProfileParams& profile;
    chain::ChainAdapter& chain;
    const SeatSet& seats;
    storage::SeatSigner& signer;
    storage::SignerLog& signer_log;
    storage::CommitLog& commit_log;
};
using AgreementFactory = std::function<Result<std::unique_ptr<agreement::Agreement>>(const AgreementParts&)>;

struct SeatNodeConfig {
    ProfileParams profile;
    uint32_t seat_count{1};
    uint32_t self_seat{0};
    std::vector<unsigned char> ikm; //!< this seat's BLS IKM (SeatSigner::FromIKM)
    size_t max_replies{64};         //!< InProcessApi::ApiLimits
    agreement::PbftConfig pbft;     //!< k > 1; self_seat and start_ms are set by the node
};

//! What the caller owns and the node borrows; every pointer but `mesh`, `sign_observer`, `hooks` and `agreement` is required.
struct SeatNodeParts {
    chain::ChainAdapter* chain{nullptr};
    storage::Journal* signer_journal{nullptr};
    storage::Journal* commit_journal{nullptr};
    admission::BudgetPolicy* budget{nullptr};
    net::SeatMesh* mesh{nullptr};                 //!< k > 1: this seat's end
    storage::SignObserver* sign_observer{nullptr}; //!< the simulator's trace
    NodeHooks* hooks{nullptr};                     //!< no-op when null
    AgreementFactory agreement;                    //!< k = 1 only
};

class SeatNode
{
public:
    /** Build every component (their errors propagate) over `parts`; `seats`
     * is the seat set this node belongs to (seat self_seat's key must be
     * `ikm`'s: SEAT_BAD_KEY) and `genesis` the head every component starts
     * from. With `restart` the logs are recovered and the state restored from
     * the commit journal (M4-STO-01; a decided-but-unapplied tail is
     * NotImplemented M4-STO-03).
     */
    static Result<std::unique_ptr<SeatNode>> Create(const SeatNodeConfig& config, const SeatNodeParts& parts, const SeatSet& seats,
                                                    const state::HeadView& genesis, bool restart = false);
    SeatNode(const SeatNode&) = delete;
    SeatNode& operator=(const SeatNode&) = delete;
    ~SeatNode();

    //! Seal the next slot (k = 1 only; INVALID_INPUT SEAT_MULTI_SEAT otherwise); see the file comment.
    Result<SealReport> SealSlot();

    // ---- Multi-seat (k > 1) --------------------------------------------------------

    //! True iff this seat leads the current view of the slot after its head (PbftAgreement::IsLeader); false for k = 1.
    bool IsLeader();
    /** The leader's half of a slot: steps 0 to 4 of SealSlot and
     * PbftAgreement::Propose. INVALID_INPUT (SEAT_NOT_LEADER) when this seat
     * does not lead; the agreement's and the components' errors as in
     * SealSlot (the fencing rules of O-90 before the head moves). The
     * candidate is kept for Complete.
     */
    Result<uint64_t> ProposeSlot();
    //! A frame from the mesh: CRITICAL and PROPOSAL frames go to the agreement (its errors are returned; only STORAGE_FATAL and FENCED fence the node); others are ignored.
    Result<void> OnFrame(const net::InboundFrame& frame);
    //! Time: PbftAgreement::OnTimeout(now_ms).
    Result<void> Tick(uint64_t now_ms);
    /** Steps 6 to 9 of SealSlot for a decision the agreement reached: the
     * kept candidate when it is the decided header's (the leader), else
     * StateOwner::Reexecute of the decided header and body (a follower).
     * nullopt when no decision is waiting.
     */
    Result<std::optional<SealReport>> Complete();
    //! The agreement's view of the slot in flight (k > 1).
    std::optional<agreement::PbftStatus> AgreementStatus();
    /** A crash and restart: the journals and the chain stay; the logs are
     * rebuilt by Recover, the state by Restore over the commit journal
     * (NotImplemented M4-STO-03 while a decided slot is unapplied), the read
     * service, pool, admission, lanes, API, signer and agreement are fresh;
     * the agreement resumes its durable view state and starts its timers at
     * the last Tick (O-99).
     */
    Result<void> Restart();
    uint32_t SeatCount() const { return m_config.seat_count; }
    uint32_t SelfSeat() const { return m_config.self_seat; }
    //! Answer at most `max` queued API requests (InProcessApi::Pump).
    Result<net::PumpReport> Pump(size_t max);

    // Components (the node owns them; references stay valid for its life).
    const ProfileParams& Profile() const { return m_config.profile; }
    const SeatSet& Seats() const { return m_seats; }
    const state::HeadView& Genesis() const { return m_genesis; }
    chain::ChainAdapter& Chain() { return *m_chain; }
    state::StateOwner& Owner() { return *m_owner; }
    read::ReadService& Read() { return *m_read; }
    admission::ActionPool& Pool() { return *m_pool; }
    admission::Admission& Admitter() { return *m_admission; }
    net::InProcessApi& Api() { return *m_api; }
    storage::CommitLog& CommitLog() { return *m_commit_log; }
    storage::SignerLog& SignerLog() { return *m_signer_log; }
    storage::SeatSigner& Signer() { return *m_signer; }
    agreement::EvidenceStore& Evidence() { return *m_evidence; }
    //! True once STORAGE_FATAL, FENCED or AUTHORITY_CHANGED fenced admission.
    bool Fenced() const { return m_admission->Fenced(); }

private:
    SeatNode(const SeatNodeConfig& config, const SeatNodeParts& parts, const SeatSet& seats, const state::HeadView& genesis);

    //! The steps of SealSlot after slot.begin.
    Result<SealReport> SealNext(const std::shared_ptr<const state::HeadView>& head, uint64_t height);
    //! Steps 0 to 4: poll, take, preview and the hooks' step; the candidate and the taken ids.
    Result<std::pair<state::Candidate, std::vector<ActionId>>> Prepare(const std::shared_ptr<const state::HeadView>& head, uint64_t height);
    //! Steps 6 to 9: apply, APPLIED, publish, pool; the sealed slot goes to the hooks.
    Result<SealReport> Finish(uint64_t height, state::Candidate candidate, agreement::Decision decision, std::vector<ActionId> taken);
    //! Build (or rebuild, on restart) every component after the journals and the chain.
    Result<void> BuildComponents(bool restart);
    //! Fence admission (once; the hooks see node.fence with the code).
    void Fence(const Error& error);
    /** Before the head moves: fence after `error` if it is STORAGE_FATAL,
     * FENCED, AUTHORITY_CHANGED, EQUIVOCATION_REFUSED, INVARIANT or
     * ORDER_VIOLATION; returns `error` (O-90).
     */
    Error FenceOn(const Error& error);
    //! After Apply moved the head: always fence; returns `error` (O-90).
    Error FenceAfterApply(const Error& error);
    NodeHooks& Hooks() { return *m_hooks; }

    const SeatNodeConfig m_config;
    NodeHooks m_no_hooks;
    NodeHooks* m_hooks;
    chain::ChainAdapter* m_chain;
    storage::Journal* m_signer_journal;
    storage::Journal* m_commit_journal;
    admission::BudgetPolicy* m_budget;
    net::SeatMesh* m_mesh;
    storage::SignObserver* m_sign_observer;
    AgreementFactory m_factory;
    const SeatSet m_seats;
    const state::HeadView m_genesis;
    std::unique_ptr<storage::SignerLog> m_signer_log;
    std::unique_ptr<storage::CommitLog> m_commit_log;
    std::unique_ptr<agreement::EvidenceStore> m_evidence;
    std::unique_ptr<storage::SeatSigner> m_signer;
    std::unique_ptr<state::StateOwner> m_owner;
    std::unique_ptr<read::ReadService> m_read;
    std::unique_ptr<admission::ActionPool> m_pool;
    std::unique_ptr<admission::Admission> m_admission;
    std::unique_ptr<net::LaneSet> m_lanes;
    std::unique_ptr<net::InProcessApi> m_api;
    std::unique_ptr<agreement::Agreement> m_certifier; //!< k = 1 (the factory's)
    std::unique_ptr<agreement::PbftSafetyRules> m_rules;
    std::unique_ptr<agreement::PbftAgreement> m_pbft; //!< k > 1
    std::optional<state::Candidate> m_proposed;
    std::vector<ActionId> m_proposed_ids;
    uint64_t m_now_ms{0}; //!< the last Tick; a rebuilt agreement starts its timers here
};

} // namespace flowmesh2::seat

#endif // B3_FLOWMESH2_SEAT_SEAT_NODE_H
