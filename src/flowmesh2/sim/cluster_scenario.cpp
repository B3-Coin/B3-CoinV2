// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/sim/scenario.h>

#include <flowmesh2/agreement/pbft.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/sim/checks.h>
#include <flowmesh2/sim/cluster.h>
#include <flowmesh2/sim/sim_client.h>
#include <flowmesh2/sim/sim_node.h>
#include <flowmesh2/verify/b3verify.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/** The four-seat cluster scenario (M4-SIM-01, M4-SIM-02; ARCHITECTURE.md
 * section 14.7). Four nodes certify five slots under faults:
 *   slot 1  the deposits of block 130 (alice 1e9 rUSD, bob 1e10 B3)
 *   slot 2  alice bids 3 lots of M1 at 100: the bid rests
 *   slot 3  bob asks 3 lots at 100 (admitted before the partition): the
 *           trade; under the partition {0, 1} | {2, 3} nothing can decide
 *           (no quorum on either side, Q6); it heals after PARTITION_MS and
 *           the slot decides. (Two crossing orders in one slot are refused
 *           until the uniform-price call of M3-ENG-02 lands.)
 *   slot 4  node crash_seat crashed and restarted before it (Q5)
 *   slot 5  a heartbeat
 * throughout: 5 % of the frames from seat 1 to seat 2 vanish, frames from
 * seat 0 to seat 3 arrive 2 ms late, seat 2's clock runs 7 ms ahead. The
 * submissions go to every seat's API (actions are not gossiped between seats).
 * Checks: Q1 every node's head (hash and state root) equal after every
 * slot; Q2 every node's decision certificate verifies with b3verify against
 * the seat set; Q3 every node's commit log holds the same decided header per
 * slot; Q4 the trade settled on every node (alice holds 3 lots of B3, bob
 * holds the quote); Q5 the restarted node rejoins and ends at the common
 * head; Q6 no node advanced during the partition. P6 (suite): the same seed
 * twice gives the same trace digest; headers may differ across seeds (the
 * leader a view change picks, and so the body, depend on the drops).
 */
namespace flowmesh2::sim {

namespace {

constexpr uint64_t SLOT_LIMIT_MS{4'000};  //!< a slot that takes longer is a harness error
constexpr uint64_t PARTITION_MS{300};     //!< how long slot 3's partition lasts
constexpr uint64_t START_MS{1};

class ClusterDriver
{
public:
    explicit ClusterDriver(uint64_t seed) : m_seed{seed} {}
    RunReport Run();

private:
    template <class T>
    bool Got(const Result<T>& result, std::string_view what)
    {
        if (result.has_value()) return true;
        if (m_harness.empty()) m_harness = std::string{what} + ": " + detail::ErrorText(result.error());
        return false;
    }
    bool Fail(std::string what)
    {
        if (m_harness.empty()) m_harness = std::move(what);
        return false;
    }
    SimClient& Client(const std::string& name) { return m_clients.at(name); }
    bool Setup();
    //! Submit `frame` to the leader's API and wait for an ADMITTED reply.
    bool Submit(const std::string& who, const std::string& label, const std::vector<unsigned char>& frame);
    //! Run the cluster until every node is at `height` or the slot limit passes; false on a harness error.
    bool RunSlot(uint64_t height);
    void AfterSlot(uint64_t height);
    void FillReport(RunReport& report);

    const uint64_t m_seed;
    detail::Checks m_checks;
    TraceRecorder m_trace;
    std::unique_ptr<SimCluster> m_cluster;
    std::map<std::string, SimClient> m_clients;
    std::vector<std::string> m_order; //!< per slot: the labels of the originals, "," separated
    std::map<ActionId, std::string> m_labels;
    uint64_t m_now{START_MS};
    std::string m_harness;
    std::optional<uint32_t> m_crashed;
};

bool ClusterDriver::Setup()
{
    SimNodeConfig config;
    config.faults.drop.push_back(DropRule{1, 2, 50});
    config.faults.delay.push_back(DelayRule{0, 3, 2});
    config.faults.skew.push_back(SkewRule{2, 7});
    config.faults.partition = {{0, 1}, {2, 3}};
    config.faults.partition_heal_at_ms = 0; // set when slot 3 starts
    config.faults.crash_before_slot = 4;
    config.faults.crash_seat = 3;
    config.faults.fabric_seed = m_seed;
    // The partition is armed by the driver at slot 3: until then it is healed.
    Result<std::unique_ptr<SimCluster>> cluster{SimCluster::Create(4, config, m_trace)};
    if (!Got(cluster, "SimCluster::Create")) return false;
    m_cluster = std::move(cluster).value();
    m_cluster->HealPartition();
    SimNode& node{m_cluster->Node(0)};
    const std::vector<MarketDescriptor> markets{node.Markets().begin(), node.Markets().end()};
    for (const std::string name : {"alice", "bob"}) {
        Result<SimClient> client{SimClient::Create(name, node.Profile(), node.Domain(), node.Seats(), markets)};
        if (!Got(client, "SimClient::Create " + name)) return false;
        m_clients.emplace(name, std::move(client).value());
    }
    std::vector<Fact> facts;
    DepositFact alice;
    alice.cursor = FactCursor{SIM_BASE_HEIGHT + 1, 0};
    alice.outpoint = Outpoint{SimLabelHash("fm2/sim/cluster/T1"), 0};
    alice.account = Client("alice").Account();
    alice.asset = SimAsset("rUSD");
    alice.amount = 1'000'000'000;
    DepositFact bob;
    bob.cursor = FactCursor{SIM_BASE_HEIGHT + 1, 1};
    bob.outpoint = Outpoint{SimLabelHash("fm2/sim/cluster/T2"), 0};
    bob.account = Client("bob").Account();
    bob.asset = AssetId::Native();
    bob.amount = 10'000'000'000;
    facts.push_back(Fact{alice});
    facts.push_back(Fact{bob});
    for (size_t i{0}; i < m_cluster->Size(); ++i) {
        if (!Got(m_cluster->Node(i).Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 1), facts), "AppendBlock 130")) return false;
    }
    return true;
}

bool ClusterDriver::Submit(const std::string& who, const std::string& label, const std::vector<unsigned char>& frame)
{
    // To every seat's API, as a client with a sentry per seat would: actions are not gossiped between seats (M4-NET-03 is
    // a stub), so the one that leads when the slot is proposed must hold it in its own pool, whichever view that is.
    for (size_t i{0}; i < m_cluster->Size(); ++i) {
        SimNode& node{m_cluster->Node(i)};
        const std::string where{label + " at seat " + std::to_string(i)};
        const Result<net::RequestId> id{node.Api().Submit(frame)};
        if (!Got(id, "Submit " + where)) return false;
        const Result<net::PumpReport> pumped{node.Pump(1)};
        if (!Got(pumped, "Pump " + where)) return false;
        const std::optional<net::ApiReply> reply{node.Api().TakeReply(*id)};
        if (!reply.has_value()) return Fail("no reply for " + where);
        const auto* admitted{std::get_if<admission::AdmissionReply>(&reply->body)};
        if (admitted == nullptr || admitted->code != AdmissionCode::ADMITTED) return Fail(where + " was not admitted");
    }
    Result<ActionV2> action{ActionV2::Decode(frame, m_cluster->Node(0).Profile())};
    if (!Got(action, "decode " + label)) return false;
    m_labels[action->Id()] = who + "/" + label;
    m_trace.Record("client.submit", who + "," + label + ",seats=" + std::to_string(m_cluster->Size()));
    return true;
}

bool ClusterDriver::RunSlot(uint64_t height)
{
    const uint64_t deadline{m_now + SLOT_LIMIT_MS};
    bool proposed{false};
    while (m_now < deadline) {
        if (!proposed) {
            const std::optional<uint32_t> leader{m_cluster->Leader()};
            if (leader.has_value() && m_cluster->Node(*leader).Owner().Head()->Header().height + 1 == height) {
                const Result<uint64_t> slot{m_cluster->Node(*leader).ProposeSlot()};
                // BUSY and NEED_DATA come back later; a leader that cannot propose (SIM_NOT_LEADER after a view moved) is tried next step.
                if (slot.has_value()) {
                    proposed = true;
                } else if (slot.error().code != ErrorCode::BUSY && slot.error().code != ErrorCode::NEED_DATA &&
                           slot.error().code != ErrorCode::INVALID_INPUT) {
                    return Got(slot, "ProposeSlot " + std::to_string(height));
                }
            }
        }
        const Result<PumpReport> pumped{m_cluster->Pump(m_now)};
        if (!Got(pumped, "Pump")) return false;
        bool all{true};
        for (size_t i{0}; i < m_cluster->Size(); ++i) {
            all = all && m_cluster->Node(i).Owner().Head()->Header().height >= height;
        }
        ++m_now;
        if (all) return true;
        // A view change elects another leader: its proposal is the next one.
        if (proposed) {
            const std::optional<uint32_t> leader{m_cluster->Leader()};
            if (leader.has_value()) {
                const std::optional<agreement::PbftStatus> status{m_cluster->Node(*leader).AgreementStatus()};
                if (status.has_value() && !status->proposal_accepted && !status->decided) proposed = false;
            }
        }
    }
    return Fail("slot " + std::to_string(height) + " did not decide within the limit");
}

void ClusterDriver::AfterSlot(uint64_t height)
{
    const std::string where{"slot " + std::to_string(height) + ": "};
    const std::shared_ptr<const state::HeadView> first{m_cluster->Node(0).Owner().Head()};
    for (size_t i{0}; i < m_cluster->Size(); ++i) {
        SimNode& node{m_cluster->Node(i)};
        const std::shared_ptr<const state::HeadView> head{node.Owner().Head()};
        m_checks.Expect("Q1", head->Header().height == height, where + "node " + std::to_string(i) + " is not at the slot");
        m_checks.Expect("Q1", head->HeaderHash() == first->HeaderHash() && head->Header().state_root == first->Header().state_root,
                        where + "node " + std::to_string(i) + " has another head or root");
        const SealedSlot* sealed{nullptr};
        for (const SealedSlot& s : node.Sealed()) {
            if (s.height == height) sealed = &s;
        }
        if (sealed == nullptr) {
            m_checks.Expect("Q2", false, where + "node " + std::to_string(i) + " sealed nothing");
            continue;
        }
        m_checks.ExpectOk("Q2", verify::VerifyCertificate(sealed->head->Header(), sealed->decision.certificate, node.Seats()),
                          where + "node " + std::to_string(i) + "'s certificate");
        const std::optional<storage::DecisionRef> decided{node.CommitLog().DecisionAt(height)};
        m_checks.Expect("Q3", decided.has_value() && decided->header_hash == first->HeaderHash(),
                        where + "node " + std::to_string(i) + "'s commit log decided another header");
        if (i == 0) {
            std::string order;
            for (const std::vector<unsigned char>& original : sealed->decision.body.originals) {
                Result<ActionV2> action{ActionV2::Decode(original, node.Profile())};
                const auto it{action.has_value() ? m_labels.find(action->Id()) : m_labels.end()};
                order += (order.empty() ? "" : ",") + (it == m_labels.end() ? std::string{"?"} : it->second);
            }
            m_order.push_back(order);
        }
    }
}

RunReport ClusterDriver::Run()
{
    RunReport report;
    if (!Setup()) {
        FillReport(report);
        return report;
    }
    // Slot 1: the deposits.
    if (!RunSlot(1)) return FillReport(report), report;
    AfterSlot(1);
    // Slot 2: alice BUY 3 lots of M1 at 100 ticks rests.
    const MarketDescriptor m1{SimMarketM1()};
    {
        Result<std::vector<unsigned char>> bid{Client("alice").Place("bid", 1, m1, Side::BUY, TimeInForce::GTC, 100, 3, 1)};
        if (!Got(bid, "sign bid") || !Submit("alice", "bid", *bid)) return FillReport(report), report;
    }
    if (!RunSlot(2)) return FillReport(report), report;
    AfterSlot(2);
    // Slot 3: bob SELL 3 lots at 100 takes the bid; admitted before the partition, decided after it heals.
    {
        Result<std::vector<unsigned char>> ask{Client("bob").Place("ask", 1, m1, Side::SELL, TimeInForce::GTC, 100, 3, 2)};
        if (!Got(ask, "sign ask") || !Submit("bob", "ask", *ask)) return FillReport(report), report;
    }
    // Slot 3: the partition {0, 1} | {2, 3} for PARTITION_MS: no quorum anywhere, so nothing decides until it heals.
    {
        const uint64_t heal{m_now + PARTITION_MS};
        m_cluster->SetPartitionHeal(heal);
        const uint64_t deadline{m_now + SLOT_LIMIT_MS};
        bool proposed{false};
        while (m_now < deadline) {
            if (!proposed) {
                const std::optional<uint32_t> leader{m_cluster->Leader()};
                if (leader.has_value() && m_cluster->Node(*leader).Owner().Head()->Header().height == 2) {
                    const Result<uint64_t> slot{m_cluster->Node(*leader).ProposeSlot()};
                    if (slot.has_value()) proposed = true;
                }
            }
            const Result<PumpReport> pumped{m_cluster->Pump(m_now)};
            if (!Got(pumped, "Pump")) return FillReport(report), report;
            bool any_advanced{false};
            bool all{true};
            for (size_t i{0}; i < m_cluster->Size(); ++i) {
                const uint64_t h{m_cluster->Node(i).Owner().Head()->Header().height};
                any_advanced = any_advanced || h >= 3;
                all = all && h >= 3;
            }
            if (m_now < heal) m_checks.Expect("Q6", !any_advanced, "a node decided slot 3 during the partition");
            ++m_now;
            if (all) break;
            if (proposed) {
                const std::optional<uint32_t> leader{m_cluster->Leader()};
                if (leader.has_value()) {
                    const std::optional<agreement::PbftStatus> status{m_cluster->Node(*leader).AgreementStatus()};
                    if (status.has_value() && !status->proposal_accepted && !status->decided) proposed = false;
                }
            }
        }
        bool all{true};
        for (size_t i{0}; i < m_cluster->Size(); ++i) all = all && m_cluster->Node(i).Owner().Head()->Header().height >= 3;
        if (!all) return Fail("slot 3 did not decide after the partition healed"), FillReport(report), report;
        m_checks.Expect("Q6", true, "partition");
    }
    AfterSlot(3);
    for (size_t i{0}; i < m_cluster->Size(); ++i) {
        const ledger::LedgerState& ledger{m_cluster->Node(i).Owner().Head()->State().engine.ledger};
        const ledger::Balance alice_b3{ledger.BalanceOf(Client("alice").Account(), AssetId::Native())};
        const ledger::Balance bob_rusd{ledger.BalanceOf(Client("bob").Account(), SimAsset("rUSD"))};
        m_checks.Expect("Q4", alice_b3.available == 3 * static_cast<int64_t>(m1.base_atoms_per_lot),
                        "node " + std::to_string(i) + ": alice holds " + std::to_string(alice_b3.available) + " B3 atoms, not the 3 lots");
        m_checks.Expect("Q4", bob_rusd.available > 0 && bob_rusd.available <= 300,
                        "node " + std::to_string(i) + ": bob holds " + std::to_string(bob_rusd.available) + " rUSD atoms, not the quote");
    }
    // Slot 4: node crash_seat crashes and restarts first.
    {
        const uint32_t seat{m_cluster->Faults().crash_seat};
        if (!Got(m_cluster->RestartNode(seat), "RestartNode")) return FillReport(report), report;
        m_crashed = seat;
        m_checks.Expect("Q5", m_cluster->Node(seat).Owner().Head()->Header().height == 3, "the restarted node did not replay to slot 3");
    }
    if (!RunSlot(4)) return FillReport(report), report;
    AfterSlot(4);
    // Slot 5: a heartbeat.
    if (!RunSlot(5)) return FillReport(report), report;
    AfterSlot(5);
    if (m_crashed.has_value()) {
        m_checks.Expect("Q5", m_cluster->Node(*m_crashed).Owner().Head()->HeaderHash() == m_cluster->Node(0).Owner().Head()->HeaderHash(),
                        "the restarted node ended at another head");
    }
    FillReport(report);
    return report;
}

void ClusterDriver::FillReport(RunReport& report)
{
    report.variant = Variant::PBFT_CLUSTER;
    report.seed = m_seed;
    report.harness_error = m_harness;
    report.checks = m_checks.Results();
    for (const std::string_view id : ManifestOf(Variant::PBFT_CLUSTER)) {
        if (!report.checks.contains(id)) report.checks.emplace(std::string{id}, CheckResult{false, false, "not run"});
    }
    if (m_cluster) {
        SimNode& node{m_cluster->Node(0)};
        for (const SealedSlot& slot : node.Sealed()) report.headers.push_back(slot.head->HeaderHash());
        report.final_state_root = node.Owner().Head()->Header().state_root;
        report.canonical_order = m_order;
    }
    report.trace_digest = m_trace.Digest();
    report.trace_events = m_trace.Size();
}

} // namespace

RunReport RunPbftCluster(uint64_t seed)
{
    ClusterDriver driver{seed};
    return driver.Run();
}

} // namespace flowmesh2::sim
