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
#include <flowmesh2/agreement/evidence.h>
#include <flowmesh2/agreement/pbft.h>
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
#include <flowmesh2/net/seat_mesh.h>
#include <flowmesh2/read/read_service.h>
#include <flowmesh2/read/views.h>
#include <flowmesh2/seat/seat_node.h>
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
 * 14): a dev wrapper over seat::SeatNode (seat/seat_node.h, the component
 * assembly and sealing steps shared with the daemon since stage 2). REGTEST
 * and TEST values only: the synthetic network SimNetwork() (a TEST magic and
 * ModernChainDomain = SHA256("fm2/sim/domain")), label block hashes, the
 * seats' BLS IKMs SHA256("fm2/sim/seat<i>"). It never touches a real
 * network, datadir or key.
 *
 * What the wrapper adds to the seat: a FakeChainAdapter over base
 * SimAnchor(129), seen through a TracingChainAdapter; a SIGNER and a COMMIT
 * MemoryJournal observed by the TraceRecorder; the genesis head (markets
 * M1 = B3/rUSD and M2 = TOK/rUSD, anchor SimAnchor(129), facts from
 * (130, 0)); the UnlimitedDevBudget; the DevSingleSeatCertifier for k = 1
 * (REGTEST only); the fault plan (withhold, the authority bumps, the
 * commit-barrier mapping check) and the trace events, through
 * seat::NodeHooks at exactly the points the seat's file comment names; and
 * every sealed slot kept for the checks (Sealed, ReapplySealed). The sealing
 * order, the fencing rules and the k > 1 drive are the seat's (see
 * seat/seat_node.h; the traces are byte-identical to R14's).
 *
 * The node is driven by one simulation thread (the scheduler's); the
 * components keep their own locking rules.
 */
namespace flowmesh2::sim {

// Static details (ErrorCode in parentheses); the sealing ones are the seat's.
inline constexpr std::string_view SIM_POLL_BUSY{seat::SEAT_POLL_BUSY};           //!< (BUSY)
inline constexpr std::string_view SIM_POLL_NEED_DATA{seat::SEAT_POLL_NEED_DATA}; //!< (NEED_DATA)
inline constexpr std::string_view SIM_POLL_AUTHORITY{seat::SEAT_POLL_AUTHORITY}; //!< (AUTHORITY_CHANGED)
inline constexpr std::string_view SIM_NO_DECISION{seat::SEAT_NO_DECISION};       //!< (INVARIANT)
inline constexpr std::string_view SIM_FAULT_MAPPING{"commit barriers so far do not match the fault plan"};  //!< (INVARIANT)
inline constexpr std::string_view SIM_FAULT_SLOT{"fault plan slot must be at least 1"};                     //!< (INVALID_INPUT)
inline constexpr std::string_view SIM_NOT_SEALED{"no sealed slot at that height"};                          //!< (INVALID_INPUT)
inline constexpr std::string_view SIM_BAD_KEY{"a simulator key label gives no key"};                        //!< (INVALID_INPUT)
inline constexpr std::string_view SIM_FENCED{seat::SEAT_FENCED};                                            //!< (FENCED)
inline constexpr std::string_view SIM_MULTI_SEAT{seat::SEAT_MULTI_SEAT};                                    //!< (INVALID_INPUT)
inline constexpr std::string_view SIM_NOT_LEADER{seat::SEAT_NOT_LEADER};                                    //!< (INVALID_INPUT)
inline constexpr std::string_view SIM_MESH_MISSING{seat::SEAT_MESH_MISSING};                                //!< (INVALID_INPUT)
inline constexpr std::string_view SIM_SEAT_RANGE{seat::SEAT_RANGE};                                         //!< (INVALID_INPUT)

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
    // Multi-seat (M4-SIM-01): with seat_count > 1 the node signs as seat
    // self_seat of the k simulator seats (IKM SHA256("fm2/sim/seat<i>")) and
    // runs PbftAgreement over `mesh`, its end of the in-process mesh, which
    // the cluster owns and which must outlive the node. seat_count 1 is the
    // k = 1 dev certifier of the walking skeleton, byte for byte.
    uint32_t seat_count{1};
    uint32_t self_seat{0};
    net::SeatMesh* mesh{nullptr};
    agreement::PbftConfig pbft; //!< self_seat is taken from above
};

using SealedSlot = seat::SealedSlot;
using SealReport = seat::SealReport;

class SimNode
{
public:
    //! CheckFaultPlan first (M4-SIM-02), then builds every component (their errors propagate).
    static Result<std::unique_ptr<SimNode>> Create(const SimNodeConfig& config, TraceRecorder& trace);
    SimNode(const SimNode&) = delete;
    SimNode& operator=(const SimNode&) = delete;
    ~SimNode();

    //! Seal the next slot (k = 1 only; INVALID_INPUT SIM_MULTI_SEAT otherwise); seat::SeatNode::SealSlot.
    Result<SealReport> SealSlot() { return m_node->SealSlot(); }

    // ---- Multi-seat (k > 1, M4-SIM-01): the cluster drives these (seat::SeatNode) ----

    bool IsLeader() { return m_node->IsLeader(); }
    Result<uint64_t> ProposeSlot() { return m_node->ProposeSlot(); }
    Result<void> OnFrame(const net::InboundFrame& frame) { return m_node->OnFrame(frame); }
    Result<void> Tick(uint64_t now_ms) { return m_node->Tick(now_ms); }
    Result<std::optional<SealReport>> Complete() { return m_node->Complete(); }
    std::optional<agreement::PbftStatus> AgreementStatus() { return m_node->AgreementStatus(); }
    /** A crash and restart (M4-SIM-02): the journals and the chain stay; the
     * seat rebuilds everything else (seat::SeatNode::Restart). The trace
     * records node.restart.
     */
    Result<void> Restart();
    uint32_t SeatCount() const { return m_node->SeatCount(); }
    uint32_t SelfSeat() const { return m_node->SelfSeat(); }
    //! Answer at most `max` queued API requests (InProcessApi::Pump).
    Result<net::PumpReport> Pump(size_t max) { return m_node->Pump(max); }
    /** Apply sealed slot `height`'s candidate copy and durable decision again,
     * under the current authority (N13). INVALID_INPUT (SIM_NOT_SEALED) if
     * this node sealed no such slot; otherwise StateOwner::Apply's answer
     * (STALE_PARENT once a later slot is applied).
     */
    Result<std::shared_ptr<const state::HeadView>> ReapplySealed(uint64_t height);

    // Components (the node owns them; references stay valid for its life).
    const ProfileParams& Profile() const { return m_node->Profile(); }
    const NetworkId& Network() const { return m_network; }
    const uint256& Domain() const { return m_domain; }
    const SeatSet& Seats() const { return m_node->Seats(); }
    const AuthorityVersion& GenesisAuthority() const { return m_genesis_authority; }
    const state::HeadView& Genesis() const { return m_node->Genesis(); }
    std::span<const MarketDescriptor> Markets() const { return m_markets; }
    const FaultPlan& Faults() const { return m_faults; }
    dev::FakeChainAdapter& Chain() { return *m_fake; }
    chain::ChainAdapter& TracedChain() { return *m_traced; }
    state::StateOwner& Owner() { return m_node->Owner(); }
    read::ReadService& Read() { return m_node->Read(); }
    admission::ActionPool& Pool() { return m_node->Pool(); }
    admission::Admission& Admitter() { return m_node->Admitter(); }
    net::InProcessApi& Api() { return m_node->Api(); }
    storage::MemoryJournal& SignerJournal() { return *m_signer_journal; }
    storage::MemoryJournal& CommitJournal() { return *m_commit_journal; }
    storage::CommitLog& CommitLog() { return m_node->CommitLog(); }
    agreement::EvidenceStore& Evidence() { return m_node->Evidence(); }
    storage::SignerLog& SignerLog() { return m_node->SignerLog(); }
    seat::SeatNode& Seat() { return *m_node; }
    TraceRecorder& Trace() { return m_trace; }
    const std::vector<SealedSlot>& Sealed() const { return m_sealed; }
    //! True once STORAGE_FATAL, FENCED or AUTHORITY_CHANGED fenced admission.
    bool Fenced() const { return m_node->Fenced(); }

private:
    struct Parts;
    //! The seat's hooks: the trace and the fault plan.
    class Hooks final : public seat::NodeHooks
    {
    public:
        explicit Hooks(SimNode& node) : m_node{node} {}
        void Record(std::string_view kind, std::string detail) override;
        void BeginSlot(uint64_t height) override;
        void EndSlot(uint64_t height, std::string_view result) override;
        bool Withhold(const ActionV2& action) override;
        Result<void> AfterPreview(uint64_t height) override;
        void AfterDecision(uint64_t height) override;
        void OnSealed(SealedSlot&& sealed) override;

    private:
        SimNode& m_node;
    };
    explicit SimNode(Parts&& parts);

    TraceRecorder& m_trace;
    const NetworkId m_network;
    const uint256 m_domain;
    const FaultPlan m_faults;
    const std::vector<MarketDescriptor> m_markets;
    std::unique_ptr<storage::MemoryJournal> m_signer_journal;
    std::unique_ptr<storage::MemoryJournal> m_commit_journal;
    const AuthorityVersion m_genesis_authority;
    std::unique_ptr<dev::FakeChainAdapter> m_fake;
    std::unique_ptr<TracingChainAdapter> m_traced;
    std::unique_ptr<dev::UnlimitedDevBudget> m_budget;
    Hooks m_hooks;
    std::unique_ptr<seat::SeatNode> m_node;
    std::vector<SealedSlot> m_sealed;
};

} // namespace flowmesh2::sim

#endif // B3_FLOWMESH2_SIM_SIM_NODE_H
