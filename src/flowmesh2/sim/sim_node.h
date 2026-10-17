// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_SIM_SIM_NODE_H
#define B3_FLOWMESH2_SIM_SIM_NODE_H

#ifndef FM2_DEV_COMPONENTS
#error "sim/sim_node.h uses the REGTEST dev components: only targets that link fm2_dev (FM2_DEV_COMPONENTS) may include it"
#endif

#include <flowmesh2/admission/admission.h>
#include <flowmesh2/admission/pool.h>
#include <flowmesh2/agreement/agreement.h>
#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/dev/dev_budget.h>
#include <flowmesh2/dev/dev_certifier.h>
#include <flowmesh2/dev/fake_chain.h>
#include <flowmesh2/net/in_process_api.h>
#include <flowmesh2/net/lanes.h>
#include <flowmesh2/read/read_service.h>
#include <flowmesh2/read/views.h>
#include <flowmesh2/sim/faults.h>
#include <flowmesh2/sim/trace.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/state/state_owner.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/memory_journal.h>
#include <flowmesh2/storage/seat_signer.h>
#include <flowmesh2/storage/signer_log.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/** One simulated FlowMesh node (slice R14; ARCHITECTURE.md sections 3 and
 * 14). REGTEST and TEST values only: the synthetic network SimNetwork()
 * (a TEST magic and ModernChainDomain = SHA256("fm2/sim/domain")), label
 * block hashes, the one seat's BLS IKM SHA256("fm2/sim/seat0"). It never
 * touches a real network, datadir or key.
 *
 * Composition (every component real; references stay valid because each
 * lives behind a unique_ptr the node owns):
 *   FakeChainAdapter over base SimAnchor(129), seen through a
 *   TracingChainAdapter; a SIGNER and a COMMIT MemoryJournal observed by the
 *   TraceRecorder, their SignerLog and CommitLog; the SeatSigner (observed
 *   too) and its PoP-verified one-seat SeatSet; the genesis head (markets
 *   M1 = B3/rUSD and M2 = TOK/rUSD, anchor SimAnchor(129), facts from
 *   (130, 0)) and its StateOwner; a ReadService; an ActionPool; the
 *   UnlimitedDevBudget; Admission; a LaneSet (ProvisionalLaneLimits) and
 *   the InProcessApi; the DevSingleSeatCertifier (k = 1, REGTEST only).
 *
 * SealSlot() seals the next slot, in this order (the trace records each
 * step; ARCHITECTURE.md 14.4 P3):
 *  0. FENCED (SIM_FENCED) while admission is fenced: a fenced node seals
 *     nothing, so after a refusal it never polls, signs or writes again
 *     (O-89); the slot window holds only slot.begin and slot.end;
 *  1. Poll(head.facts_to, MAX_SLOT_FACTS): BUSY and NEED_DATA are returned
 *     as such (no slot); AUTHORITY_CHANGED fences admission and is returned;
 *     FACTS gives the batch;
 *  2. ActionPool::TakeForSlot(head + 1, max_actions_per_slot, the plan's
 *     withhold filter);
 *  3. StateOwner::Preview(committed head, {batch authority, anchor, range,
 *     fact bytes, taken originals});
 *  4. the plan's authority bump, if this is its slot;
 *  5. DevSingleSeatCertifier::Propose(candidate, batch authority) and
 *     TakeDecision (B-INTENT, sign, B-VOTE, B-DECISION); then the plan's
 *     bump_authority_before_apply_at bump, if this is its slot;
 *  6. StateOwner::Apply(candidate, decision, CurrentAuthority()) (check 3);
 *  7. CommitLog::AppendApplied, Barrier, Confirm (B-APPLIED);
 *  8. SlotView::Build and ReadService::Publish;
 *  9. ActionPool::OnCommitted(head, outcomes, certified body).
 * Any failure returns its error and nothing after it runs. STORAGE_FATAL,
 * FENCED and AUTHORITY_CHANGED also fence admission (Admission::Fence):
 * new submissions get HALTED, reads stay live on the last publication.
 * Nothing is retried: a later SealSlot answers FENCED at step 0. A
 * DECISION that check 3 refused stays durable and final for its height:
 * the node halts at the last applied slot, which is also the head a replay
 * of its commit journal gives (O-89).
 *
 * The node is driven by one simulation thread (the scheduler's); the
 * components keep their own locking rules.
 */
namespace flowmesh2::sim {

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view SIM_POLL_BUSY{"the chain adapter answered BUSY"};                         //!< (BUSY)
inline constexpr std::string_view SIM_POLL_NEED_DATA{"the chain adapter has no data at the cursor"};        //!< (NEED_DATA)
inline constexpr std::string_view SIM_POLL_AUTHORITY{"the chain adapter reported an authority change"};     //!< (AUTHORITY_CHANGED)
inline constexpr std::string_view SIM_NO_DECISION{"Propose succeeded without a decision"};                  //!< (INVARIANT)
inline constexpr std::string_view SIM_FAULT_MAPPING{"commit barriers so far do not match the fault plan"};  //!< (INVARIANT)
inline constexpr std::string_view SIM_FAULT_SLOT{"fault plan slot must be at least 1"};                     //!< (INVALID_INPUT)
inline constexpr std::string_view SIM_NOT_SEALED{"no sealed slot at that height"};                          //!< (INVALID_INPUT)
inline constexpr std::string_view SIM_BAD_KEY{"a simulator key label gives no key"};                        //!< (INVALID_INPUT)
inline constexpr std::string_view SIM_FENCED{"the node is fenced and seals nothing"};                       //!< (FENCED)

//! The synthetic TEST network of the simulator: chain REGTEST, magic f2 51 4d 01, ModernChainDomain SHA256("fm2/sim/domain").
NetworkId SimNetwork();
//! The same network with magic f2 51 4d 02: "another magic's domain" (N12).
NetworkId SimForeignNetwork();
//! The block hash of `height`: SHA256("fm2/sim/block/<height>").
uint256 SimBlockHash(uint32_t height);
inline Anchor SimAnchor(uint32_t height) { return Anchor{height, SimBlockHash(height)}; }
//! SHA256(label).
uint256 SimLabelHash(std::string_view label);
//! Tagged(TEST_ASSET, name): rUSD and TOK (ARCHITECTURE.md 14.1).
AssetId SimAsset(std::string_view name);
//! M1 = B3/rUSD (1,000,000 atoms per lot, 1 atom per tick); M2 = TOK/rUSD (1 per lot, 10 per tick).
MarketDescriptor SimMarketM1();
MarketDescriptor SimMarketM2();
//! The genesis base: the fake chain's base block and the genesis anchor.
inline constexpr uint32_t SIM_BASE_HEIGHT{129};

struct SimNodeConfig {
    FaultPlan faults;
    size_t max_replies{64};
    uint64_t signer_journal_instance{1};
    uint64_t commit_journal_instance{2};
};

//! One slot this node sealed: everything a later check or N13 needs.
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

//! What one successful SealSlot did.
struct SealReport {
    uint64_t height{0};
    uint256 header_hash;
    uint32_t originals{0};
    uint32_t outcomes{0};
};

class SimNode
{
public:
    //! CheckFaultPlan first (M4-SIM-02), then builds every component (their errors propagate).
    static Result<std::unique_ptr<SimNode>> Create(const SimNodeConfig& config, TraceRecorder& trace);
    SimNode(const SimNode&) = delete;
    SimNode& operator=(const SimNode&) = delete;
    ~SimNode();

    //! Seal the next slot; see the file comment.
    Result<SealReport> SealSlot();
    //! Answer at most `max` queued API requests (InProcessApi::Pump).
    Result<net::PumpReport> Pump(size_t max);
    /** Apply sealed slot `height`'s candidate copy and durable decision again,
     * under the current authority (N13). INVALID_INPUT (SIM_NOT_SEALED) if
     * this node sealed no such slot; otherwise StateOwner::Apply's answer
     * (STALE_PARENT once a later slot is applied).
     */
    Result<std::shared_ptr<const state::HeadView>> ReapplySealed(uint64_t height);

    // Components (the node owns them; references stay valid for its life).
    const ProfileParams& Profile() const { return m_profile; }
    const NetworkId& Network() const { return m_network; }
    const uint256& Domain() const { return m_domain; }
    const SeatSet& Seats() const { return m_seats; }
    const AuthorityVersion& GenesisAuthority() const { return m_genesis_authority; }
    const state::HeadView& Genesis() const { return m_genesis; }
    std::span<const MarketDescriptor> Markets() const { return m_markets; }
    const FaultPlan& Faults() const { return m_faults; }
    dev::FakeChainAdapter& Chain() { return *m_fake; }
    chain::ChainAdapter& TracedChain() { return *m_traced; }
    state::StateOwner& Owner() { return *m_owner; }
    read::ReadService& Read() { return *m_read; }
    admission::ActionPool& Pool() { return *m_pool; }
    admission::Admission& Admitter() { return *m_admission; }
    net::InProcessApi& Api() { return *m_api; }
    storage::MemoryJournal& SignerJournal() { return *m_signer_journal; }
    storage::MemoryJournal& CommitJournal() { return *m_commit_journal; }
    TraceRecorder& Trace() { return m_trace; }
    const std::vector<SealedSlot>& Sealed() const { return m_sealed; }
    //! True once STORAGE_FATAL, FENCED or AUTHORITY_CHANGED fenced admission.
    bool Fenced() const { return m_admission->Fenced(); }

private:
    struct Parts;
    explicit SimNode(Parts&& parts);

    //! The steps of SealSlot after slot.begin.
    Result<SealReport> SealNext(const std::shared_ptr<const state::HeadView>& head, uint64_t height);
    //! Fence admission after `error` if it is STORAGE_FATAL, FENCED or AUTHORITY_CHANGED; returns `error`.
    Error FenceOn(const Error& error);

    TraceRecorder& m_trace;
    const ProfileParams m_profile;
    const NetworkId m_network;
    const uint256 m_domain;
    const FaultPlan m_faults;
    const std::vector<MarketDescriptor> m_markets;
    std::unique_ptr<storage::MemoryJournal> m_signer_journal;
    std::unique_ptr<storage::MemoryJournal> m_commit_journal;
    std::unique_ptr<storage::SignerLog> m_signer_log;
    std::unique_ptr<storage::CommitLog> m_commit_log;
    std::unique_ptr<storage::SeatSigner> m_signer;
    const SeatSet m_seats;
    const AuthorityVersion m_genesis_authority;
    std::unique_ptr<dev::FakeChainAdapter> m_fake;
    std::unique_ptr<TracingChainAdapter> m_traced;
    const state::HeadView m_genesis;
    std::unique_ptr<state::StateOwner> m_owner;
    std::unique_ptr<read::ReadService> m_read;
    std::unique_ptr<admission::ActionPool> m_pool;
    std::unique_ptr<dev::UnlimitedDevBudget> m_budget;
    std::unique_ptr<admission::Admission> m_admission;
    std::unique_ptr<net::LaneSet> m_lanes;
    std::unique_ptr<net::InProcessApi> m_api;
    std::unique_ptr<dev::DevSingleSeatCertifier> m_certifier;
    std::vector<SealedSlot> m_sealed;
};

} // namespace flowmesh2::sim

#endif // B3_FLOWMESH2_SIM_SIM_NODE_H
