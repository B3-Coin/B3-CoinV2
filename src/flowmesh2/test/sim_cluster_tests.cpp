// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/net/in_process_mesh.h>
#include <flowmesh2/net/sync.h>
#include <flowmesh2/seat/seat_node.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/sim/cluster.h>
#include <flowmesh2/sim/scenario.h>
#include <flowmesh2/sim/sim_node.h>
#include <flowmesh2/test/util/mesh_fixtures.h>
#include <flowmesh2/test/util/require.h>

#include <boost/test/unit_test.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

/** The multi-node simulation (M4-SIM-01) and its fault hooks (M4-SIM-02):
 * the in-process fabric routes nothing by itself, a four-node cluster
 * certifies five slots under drops, delays, skew, a partition and a crash
 * (the pbft_cluster scenario, checks Q1..Q6, the same seed twice giving one
 * trace digest), a partition without a quorum decides nothing until it
 * heals, and an inconsistent fault plan is refused.
 */
using namespace flowmesh2;
using namespace flowmesh2::sim;
using namespace flowmesh2::test;

BOOST_AUTO_TEST_SUITE(sim_cluster_tests)

BOOST_AUTO_TEST_CASE(the_fabric_delivers_only_what_the_simulator_hands_over)
{
    Result<net::InProcessMesh> created{net::InProcessSeatMesh::Create(TestMeshConfig(0, 4), 4, /*queue_capacity=*/2)};
    RequireOk(created, "InProcessSeatMesh::Create");
    net::InProcessMesh& mesh{*created};
    BOOST_REQUIRE_EQUAL(mesh.ends.size(), 4U);
    BOOST_CHECK_EQUAL(mesh.ends[2]->Config().self_seat, 2U);
    const std::vector<unsigned char> bytes{1, 2, 3};
    RequireOk(mesh.ends[0]->Send(2, net::Lane::BULK, bytes), "Send 0 -> 2");
    BOOST_CHECK_EQUAL(mesh.fabric->Queued(), 1U);
    // Nothing reaches a receiver until the simulator puts it into the inbox.
    const Result<std::optional<net::InboundFrame>> early{mesh.ends[2]->Poll()};
    RequireOk(early, "Poll before delivery");
    BOOST_CHECK(!early->has_value());
    RequireOk(mesh.ends[0]->Send(2, net::Lane::BULK, bytes), "Send 0 -> 2 again");
    RequireError(mesh.ends[0]->Send(2, net::Lane::BULK, bytes), ErrorCode::BUSY, "a full (from, to, lane) queue");
    RequireOk(mesh.ends[0]->Send(2, net::Lane::ACTION, bytes), "another lane has its own queue");
    std::vector<net::InProcessFrame> drained{mesh.fabric->Drain()};
    BOOST_REQUIRE_EQUAL(drained.size(), 3U);
    BOOST_CHECK_EQUAL(mesh.fabric->Queued(), 0U);
    BOOST_CHECK_EQUAL(drained[0].from, 0U);
    BOOST_CHECK_EQUAL(drained[0].to, 2U);
    BOOST_CHECK_EQUAL(drained[0].seq, 0U);
    BOOST_CHECK_EQUAL(drained[2].seq, 2U);
    BOOST_CHECK(drained[2].lane == net::Lane::ACTION);
    RequireOk(mesh.fabric->Inbox(drained[2]), "Inbox");
    BOOST_CHECK_EQUAL(mesh.fabric->Waiting(2), 1U);
    const Result<std::optional<net::InboundFrame>> got{mesh.ends[2]->Poll()};
    RequireOk(got, "Poll");
    BOOST_REQUIRE(got->has_value());
    BOOST_CHECK_EQUAL((*got)->from_seat, 0U);
    BOOST_CHECK((*got)->lane == net::Lane::ACTION);
    BOOST_CHECK((*got)->bytes == bytes);
    BOOST_CHECK_EQUAL(mesh.fabric->Waiting(2), 0U);
    net::InProcessFrame outside{drained[0]};
    outside.to = 4;
    RequireError(mesh.fabric->Inbox(outside), ErrorCode::INVALID_INPUT, "a seat outside the fabric");
    RequireError(net::InProcessSeatMesh::Create(TestMeshConfig(0, 4), 3), ErrorCode::INVALID_INPUT, "seat_count differs from the prototype's");
}

BOOST_AUTO_TEST_CASE(four_nodes_certify_five_slots_under_faults)
{
    const RunReport first{RunPbftCluster(7)};
    std::string why{first.harness_error};
    for (const auto& [id, result] : first.checks) {
        if (!result.passed) why += " " + id + ": " + (result.ran ? result.detail : std::string{"not run"});
    }
    BOOST_CHECK_MESSAGE(first.Passed(), "pbft_cluster seed 7 failed:" + why);
    for (const std::string_view id : {"Q1", "Q2", "Q3", "Q4", "Q5", "Q6"}) {
        const auto it{first.checks.find(id)};
        BOOST_REQUIRE_MESSAGE(it != first.checks.end(), std::string{id} + " missing");
        BOOST_CHECK_MESSAGE(it->second.ran && it->second.passed, std::string{id} + ": " + it->second.detail);
    }
    BOOST_CHECK_EQUAL(first.headers.size(), 5U);
    BOOST_REQUIRE_EQUAL(first.canonical_order.size(), 5U);
    BOOST_CHECK_EQUAL(first.canonical_order[1], "alice/bid");
    BOOST_CHECK_EQUAL(first.canonical_order[2], "bob/ask");
    BOOST_CHECK(first.variant == Variant::PBFT_CLUSTER);
    BOOST_CHECK(ToString(Variant::PBFT_CLUSTER) == "pbft_cluster");
    BOOST_CHECK(VariantFromName("pbft_cluster") == std::optional<Variant>{Variant::PBFT_CLUSTER});
    // The same seed replays byte-identically (P6's first half).
    const RunReport second{RunPbftCluster(7)};
    BOOST_CHECK(second.trace_digest == first.trace_digest);
    BOOST_CHECK_EQUAL(second.trace_events, first.trace_events);
    BOOST_CHECK(second.headers == first.headers);
}

BOOST_AUTO_TEST_CASE(a_partition_without_a_quorum_decides_nothing_until_it_heals)
{
    TraceRecorder trace;
    SimNodeConfig config;
    config.faults.partition = {{0, 1}, {2, 3}};
    Result<std::unique_ptr<SimCluster>> created{SimCluster::Create(4, config, trace)};
    RequireOk(created, "SimCluster::Create(4)");
    SimCluster& cluster{**created};
    BOOST_CHECK_EQUAL(cluster.Size(), 4U);
    for (size_t i{0}; i < 4; ++i) {
        BOOST_CHECK_EQUAL(cluster.Node(i).SelfSeat(), i);
        BOOST_CHECK_EQUAL(cluster.Node(i).Seats().Size(), 4U);
        BOOST_CHECK(cluster.Node(i).Seats().Root() == cluster.Node(0).Seats().Root());
        RequireOk(cluster.Node(i).Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 1), {}), "AppendBlock");
    }
    const std::optional<uint32_t> leader{cluster.Leader()};
    BOOST_REQUIRE(leader.has_value());
    RequireOk(cluster.Node(*leader).ProposeSlot(), "ProposeSlot");
    RequireError(cluster.Node((*leader + 1) % 4).ProposeSlot(), ErrorCode::INVALID_INPUT, "a follower does not propose");
    uint64_t now{1};
    size_t dropped{0};
    for (; now < 400; ++now) {
        const Result<PumpReport> pumped{cluster.Pump(now)};
        RequireOk(pumped, "Pump");
        dropped += pumped->dropped;
        for (size_t i{0}; i < 4; ++i) BOOST_CHECK_EQUAL(cluster.Node(i).Owner().Head()->Header().height, 0U);
    }
    BOOST_CHECK_GT(dropped, 0U);
    cluster.HealPartition();
    bool all{false};
    for (; now < 4'000 && !all; ++now) {
        const Result<PumpReport> pumped{cluster.Pump(now)};
        RequireOk(pumped, "Pump after the heal");
        // A view may have changed meanwhile: the new leader proposes.
        if (const std::optional<uint32_t> l{cluster.Leader()}) {
            const Result<uint64_t> proposed{cluster.Node(*l).ProposeSlot()};
            (void)proposed;
        }
        all = true;
        for (size_t i{0}; i < 4; ++i) all = all && cluster.Node(i).Owner().Head()->Header().height == 1;
    }
    BOOST_CHECK_MESSAGE(all, "slot 1 did not decide after the heal");
    for (size_t i{1}; i < 4; ++i) BOOST_CHECK(cluster.Node(i).Owner().Head()->HeaderHash() == cluster.Node(0).Owner().Head()->HeaderHash());
    BOOST_CHECK_GT(trace.Size(), 0U);
}

BOOST_AUTO_TEST_CASE(a_leader_restarted_after_proposing_resumes_instead_of_fencing)
{
    // ARCHITECTURE.md 17.3 item 15(a), through SeatNode: the leader proposes, crashes (its frames lost) and restarts over its
    // journals while the chain moved on. Before, its next ProposeSlot built a fresh candidate, the signer log answered
    // PROPOSAL_CONFLICT for (epoch, slot, view) and the node fenced. Now it resumes the durable proposal: ProposeSlot is
    // BUSY (the retry timer resends it) and the slot decides the original header.
    TraceRecorder trace;
    Result<std::unique_ptr<SimCluster>> created{SimCluster::Create(4, SimNodeConfig{}, trace)};
    RequireOk(created, "SimCluster::Create(4)");
    SimCluster& cluster{**created};
    for (size_t i{0}; i < 4; ++i) RequireOk(cluster.Node(i).Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 1), {}), "AppendBlock");
    const std::optional<uint32_t> leader{cluster.Leader()};
    BOOST_REQUIRE(leader.has_value());
    RequireOk(cluster.Node(*leader).ProposeSlot(), "ProposeSlot");
    RequireOk(cluster.RestartNode(*leader), "RestartNode");
    for (size_t i{0}; i < 4; ++i) RequireOk(cluster.Node(i).Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 2), {}), "AppendBlock");
    SimNode& node{cluster.Node(*leader)};
    const std::optional<agreement::PbftStatus> status{node.AgreementStatus()};
    BOOST_REQUIRE(status.has_value());
    BOOST_CHECK(status->proposal_accepted); // before: false
    RequireError(node.ProposeSlot(), ErrorCode::BUSY, "a ProposeSlot after the restart (SEAT_PROPOSAL_IN_FLIGHT)");
    BOOST_REQUIRE(!node.Fenced()); // before: fenced on PROPOSAL_CONFLICT
    bool all{false};
    for (uint64_t now{1}; now < 2'000 && !all; ++now) {
        RequireOk(cluster.Pump(now), "Pump");
        all = true;
        for (size_t i{0}; i < 4; ++i) all = all && cluster.Node(i).Owner().Head()->Header().height == 1;
    }
    BOOST_REQUIRE_MESSAGE(all, "slot 1 did not decide");
    for (size_t i{0}; i < 4; ++i) BOOST_CHECK(!cluster.Node(i).Fenced());
    for (size_t i{1}; i < 4; ++i) BOOST_CHECK(cluster.Node(i).Owner().Head()->HeaderHash() == cluster.Node(0).Owner().Head()->HeaderHash());
}

BOOST_AUTO_TEST_CASE(an_inconsistent_fault_plan_is_refused)
{
    FaultPlan plan;
    plan.drop.push_back(DropRule{0, 1, 1'000});
    plan.delay.push_back(DelayRule{0, 1, 5});
    plan.partition = {{0}, {1, 2}};
    plan.skew.push_back(SkewRule{0, 3});
    plan.crash_before_slot = 2;
    RequireOk(CheckFaultPlan(plan), "a consistent plan");
    FaultPlan too_many{plan};
    too_many.drop[0].per_mille = 1'001;
    RequireError(CheckFaultPlan(too_many), ErrorCode::INVALID_INPUT, "per_mille above 1000");
    FaultPlan twice{plan};
    twice.partition = {{0, 1}, {1, 2}};
    RequireError(CheckFaultPlan(twice), ErrorCode::INVALID_INPUT, "a seat in two groups");
    FaultPlan zero{plan};
    zero.crash_before_slot = 0;
    RequireError(CheckFaultPlan(zero), ErrorCode::INVALID_INPUT, "a crash before slot 0");
    TraceRecorder trace;
    SimNodeConfig config;
    config.faults = twice;
    RequireError(SimCluster::Create(4, config, trace), ErrorCode::INVALID_INPUT, "the cluster checks the plan");
}

namespace {

//! Drive the leader among `able` until every node in `wait_for` is at `height` or `limit_ms` passes; the clock reached.
uint64_t DriveUntil(SimCluster& cluster, uint64_t& now, uint64_t height, const std::vector<uint32_t>& wait_for, uint64_t limit_ms)
{
    const uint64_t deadline{now + limit_ms};
    while (now < deadline) {
        if (const std::optional<uint32_t> leader{cluster.Leader()}) {
            SimNode& node{cluster.Node(*leader)};
            if (node.Owner().Head()->Header().height < height) {
                const std::optional<agreement::PbftStatus> status{node.AgreementStatus()};
                if (status.has_value() && !status->proposal_accepted && !status->decided) (void)node.ProposeSlot();
            }
        }
        RequireOk(cluster.Pump(now), "Pump");
        ++now;
        bool all{true};
        for (const uint32_t i : wait_for) all = all && cluster.Node(i).Owner().Head()->Header().height >= height;
        if (all) return now;
    }
    return now;
}

size_t CountEvents(const TraceRecorder& trace, std::string_view kind, std::string_view detail_prefix = {})
{
    size_t n{0};
    for (const TraceEvent& event : trace.Events()) {
        if (event.kind == kind && (detail_prefix.empty() || event.detail.starts_with(detail_prefix))) ++n;
    }
    return n;
}

//! The slot an agreement frame is about; nullopt for evidence or bytes that do not decode.
std::optional<uint64_t> SlotOf(const std::vector<unsigned char>& bytes, const ProfileParams& profile)
{
    const Result<agreement::Message> decoded{agreement::DecodeMessage(bytes, profile)};
    if (!decoded.has_value()) return std::nullopt;
    return std::visit(
        [](const auto& message) -> std::optional<uint64_t> {
            using T = std::decay_t<decltype(message)>;
            if constexpr (std::is_same_v<T, agreement::Proposal>) {
                return message.header.height;
            } else if constexpr (std::is_same_v<T, agreement::Vote> || std::is_same_v<T, agreement::ViewChange> ||
                                 std::is_same_v<T, agreement::NewView>) {
                return message.slot;
            } else {
                return std::nullopt;
            }
        },
        *decoded);
}

//! Drain the fabric: the DECISIONS_REQUESTs `from` sent, as (receiver, request); every other frame is dropped.
std::vector<std::pair<uint32_t, net::DecisionsRequest>> TakeRequests(SimCluster& cluster, uint32_t from, const ProfileParams& profile)
{
    std::vector<std::pair<uint32_t, net::DecisionsRequest>> out;
    for (const net::InProcessFrame& frame : cluster.Fabric()->Drain()) {
        if (frame.from != from || frame.lane != net::Lane::BULK) continue;
        const Result<net::SyncMessage> decoded{net::DecodeSync(frame.bytes, profile)};
        if (!decoded.has_value()) continue;
        if (const auto* request{std::get_if<net::DecisionsRequest>(&*decoded)}) out.emplace_back(frame.to, *request);
    }
    return out;
}

} // namespace

BOOST_AUTO_TEST_CASE(a_seat_catching_up_does_not_ask_again_while_a_slow_batch_still_applies)
{
    // Review workflow of 2026-11-18 (Low): the retry period of a catch-up request ran from its send, so a batch whose eight
    // replies took longer than sync_backoff_ms (250 ms) was asked for again from the next peer while it still applied, and
    // each later record of the first batch (taken as a push) freed that request for yet another one. Seat 3 misses twelve
    // slots; seat 0's eight replies reach it one per 100 ms, a Tick before each.
    TraceRecorder trace;
    SimNodeConfig config;
    config.faults.partition = {{0, 1, 2}, {3}};
    Result<std::unique_ptr<SimCluster>> created{SimCluster::Create(4, config, trace)};
    RequireOk(created, "SimCluster::Create(4)");
    SimCluster& cluster{**created};
    for (size_t i{0}; i < 4; ++i) RequireOk(cluster.Node(i).Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 1), {}), "AppendBlock");
    constexpr uint64_t MISSED{12}; // more than one batch: seat 3 is still behind when the first batch ends
    uint64_t now{1};
    now = DriveUntil(cluster, now, MISSED, {0, 1, 2}, 120'000);
    for (uint32_t i{0}; i < 3; ++i) BOOST_REQUIRE_EQUAL(cluster.Node(i).Owner().Head()->Header().height, MISSED);
    BOOST_REQUIRE_EQUAL(cluster.Node(3).Owner().Head()->Header().height, 0U);
    const ProfileParams& profile{cluster.Node(0).Profile()};

    // No Pump from here on: every frame goes through this test's hands, and seat 3's agreement frames reach nobody (so no
    // peer pushes it a decision). Whatever the last Pump left queued is dropped.
    (void)cluster.Fabric()->Drain();
    // Seats 0..2 work on slot MISSED + 1; the first such frame from each of two of them is kept for seat 3: f + 1 = 2 claims
    // beyond its head.
    std::map<uint32_t, net::InProcessFrame> claims;
    for (const uint64_t deadline{now + 20'000}; claims.size() < 2 && now < deadline; ++now) {
        for (uint32_t i{0}; i < 3; ++i) {
            SimNode& node{cluster.Node(i)};
            const std::optional<agreement::PbftStatus> status{node.AgreementStatus()};
            if (node.IsLeader() && status.has_value() && !status->proposal_accepted && !status->decided) (void)node.ProposeSlot();
        }
        for (const net::InProcessFrame& frame : cluster.Fabric()->Drain()) {
            if (frame.from == 3 || frame.lane == net::Lane::BULK) continue;
            if (frame.to == 3) {
                const std::optional<uint64_t> slot{SlotOf(frame.bytes, profile)};
                if (slot.has_value() && *slot > MISSED) claims.try_emplace(frame.from, frame);
                continue;
            }
            (void)cluster.Node(frame.to).OnFrame(net::InboundFrame{frame.from, frame.lane, frame.bytes});
        }
        for (uint32_t i{0}; i < 3; ++i) (void)cluster.Node(i).Tick(now);
    }
    BOOST_REQUIRE_GE(claims.size(), 2U); // one drain may bring all three
    seat::SeatNode& behind{cluster.Node(3).Seat()};
    // A future slot's message is refused, but its slot is noted as the sender's claim first (PbftAgreement::OnMessage).
    for (const auto& [from, frame] : claims) (void)behind.OnFrame(net::InboundFrame{from, frame.lane, frame.bytes});
    (void)cluster.Fabric()->Drain();
    const size_t requests_before{CountEvents(trace, "sync.request")};
    const size_t refusals_before{CountEvents(trace, "sync.refuse")};

    // One request for the first batch, to seat 0 (round robin from seat 0).
    uint64_t t{now};
    RequireOk(behind.Tick(t), "Tick: seat 3 is behind");
    const std::vector<std::pair<uint32_t, net::DecisionsRequest>> first{TakeRequests(cluster, 3, profile)};
    BOOST_REQUIRE_EQUAL(first.size(), 1U);
    BOOST_CHECK_EQUAL(first[0].first, 0U);
    BOOST_CHECK_EQUAL(first[0].second.from_slot, 1U);
    BOOST_CHECK_EQUAL(first[0].second.max, net::MAX_SYNC_DECISIONS);
    const Result<std::vector<unsigned char>> asked{net::EncodeSync(first[0].second, profile)};
    RequireOk(asked, "EncodeSync");
    RequireOk(cluster.Node(0).OnFrame(net::InboundFrame{3, net::Lane::BULK, *asked}), "seat 0 serves the request");
    std::vector<net::InProcessFrame> replies;
    for (net::InProcessFrame& frame : cluster.Fabric()->Drain()) {
        if (frame.from == 0 && frame.to == 3 && frame.lane == net::Lane::BULK) replies.push_back(std::move(frame));
    }
    BOOST_REQUIRE_EQUAL(replies.size(), size_t{net::MAX_SYNC_DECISIONS});

    // One reply per 100 ms: the batch takes 800 ms, more than three retry periods, and applies all along.
    std::vector<std::pair<uint32_t, net::DecisionsRequest>> again;
    for (size_t i{0}; i < replies.size(); ++i) {
        t += 100;
        RequireOk(behind.Tick(t), "Tick");
        RequireOk(behind.OnFrame(net::InboundFrame{0, net::Lane::BULK, replies[i].bytes}), "reply " + std::to_string(i + 1));
        BOOST_CHECK_EQUAL(cluster.Node(3).Owner().Head()->Header().height, static_cast<uint64_t>(i + 1));
        for (const auto& request : TakeRequests(cluster, 3, profile)) again.push_back(request);
    }
    BOOST_CHECK_MESSAGE(again.empty(), "asked again " << again.size() << " times while the batch applied"); // before: 4
    BOOST_CHECK_EQUAL(cluster.Node(3).Owner().Head()->Header().height, uint64_t{net::MAX_SYNC_DECISIONS});

    // The batch is complete: the next Tick asks the next peer for the next batch.
    t += 100;
    RequireOk(behind.Tick(t), "Tick after the batch");
    const std::vector<std::pair<uint32_t, net::DecisionsRequest>> second{TakeRequests(cluster, 3, profile)};
    BOOST_REQUIRE_EQUAL(second.size(), 1U);
    BOOST_CHECK_EQUAL(second[0].first, 1U);
    BOOST_CHECK_EQUAL(second[0].second.from_slot, uint64_t{net::MAX_SYNC_DECISIONS} + 1);
    // Nobody answers it: the retry period still runs from the send and doubles (250 ms, then 500 ms).
    RequireOk(behind.Tick(t + 249), "Tick");
    BOOST_CHECK(TakeRequests(cluster, 3, profile).empty());
    RequireOk(behind.Tick(t + 250), "Tick");
    const std::vector<std::pair<uint32_t, net::DecisionsRequest>> retry{TakeRequests(cluster, 3, profile)};
    BOOST_REQUIRE_EQUAL(retry.size(), 1U);
    BOOST_CHECK_EQUAL(retry[0].first, 2U);
    RequireOk(behind.Tick(t + 749), "Tick");
    BOOST_CHECK(TakeRequests(cluster, 3, profile).empty());
    RequireOk(behind.Tick(t + 750), "Tick");
    BOOST_CHECK_EQUAL(TakeRequests(cluster, 3, profile).size(), 1U);
    BOOST_CHECK_EQUAL(CountEvents(trace, "sync.request") - requests_before, 4U);
    BOOST_CHECK_EQUAL(CountEvents(trace, "sync.refuse"), refusals_before);
    BOOST_CHECK(!cluster.Node(3).Fenced());
}

BOOST_AUTO_TEST_CASE(a_seat_alone_behind_a_partition_catches_up_by_fetching_decisions)
{
    TraceRecorder trace;
    SimNodeConfig config;
    config.faults.partition = {{0, 1, 2}, {3}};
    Result<std::unique_ptr<SimCluster>> created{SimCluster::Create(4, config, trace)};
    RequireOk(created, "SimCluster::Create(4)");
    SimCluster& cluster{**created};
    for (size_t i{0}; i < 4; ++i) RequireOk(cluster.Node(i).Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 1), {}), "AppendBlock");
    // The majority decides two slots while seat 3 is alone.
    uint64_t now{1};
    now = DriveUntil(cluster, now, 2, {0, 1, 2}, 6'000);
    for (uint32_t i{0}; i < 3; ++i) BOOST_REQUIRE_EQUAL(cluster.Node(i).Owner().Head()->Header().height, 2U);
    BOOST_CHECK_EQUAL(cluster.Node(3).Owner().Head()->Header().height, 0U);
    BOOST_CHECK_EQUAL(CountEvents(trace, "sync.apply"), 0U);

    // Before the heal: a fetched record is checked. Slot 2 is not the slot after seat 3's head; a tampered certificate fails;
    // the real record of slot 1 applies.
    seat::SeatNode& behind{cluster.Node(3).Seat()};
    const ProfileParams& profile{cluster.Node(0).Profile()};
    const std::optional<std::vector<unsigned char>> two{cluster.Node(0).CommitLog().DecisionRecordAt(2)};
    const std::optional<std::vector<unsigned char>> one{cluster.Node(0).CommitLog().DecisionRecordAt(1)};
    BOOST_REQUIRE(two.has_value() && one.has_value());
    const storage::DecisionRecord record_two{storage::DecisionRecord::Decode(*two, profile).value()};
    storage::DecisionRecord record_one{storage::DecisionRecord::Decode(*one, profile).value()};
    RequireRefusal(behind.ApplyFetched(record_two), ErrorCode::STALE_PARENT, seat::SEAT_SYNC_SLOT, "slot 2 before slot 1");
    storage::DecisionRecord forged{record_one};
    forged.certificate.aggregate[7] ^= 0x01;
    RequireRefusal(behind.ApplyFetched(forged), ErrorCode::VERIFY_FAILED, seat::SEAT_SYNC_CERT, "a tampered certificate");
    storage::DecisionRecord renamed{record_one};
    renamed.certificate.header_hash = uint256::ONE;
    RequireRefusal(behind.ApplyFetched(renamed), ErrorCode::VERIFY_FAILED, seat::SEAT_SYNC_CERT_HEADER, "a certificate of another header");
    BOOST_CHECK_EQUAL(cluster.Node(3).Owner().Head()->Header().height, 0U);
    BOOST_CHECK(!cluster.Node(3).Fenced());
    RequireOk(behind.ApplyFetched(record_one), "the real record of slot 1");
    BOOST_CHECK_EQUAL(cluster.Node(3).Owner().Head()->Header().height, 1U);
    BOOST_CHECK(cluster.Node(3).Owner().Head()->HeaderHash() == cluster.Node(0).Sealed()[0].head->HeaderHash());
    BOOST_CHECK_EQUAL(CountEvents(trace, "sync.apply", "slot=1"), 1U);
    // An applied slot is below the head: the record is refused as not the next slot, and nothing moves.
    RequireRefusal(behind.ApplyFetched(record_one), ErrorCode::STALE_PARENT, seat::SEAT_SYNC_SLOT, "slot 1 again");
    BOOST_CHECK_EQUAL(cluster.Node(3).Owner().Head()->Header().height, 1U);

    // The heal: the majority goes on to slot 3; seat 3 hears about it from f + 1 peers, fetches slot 2 and takes part in slot 3.
    cluster.HealPartition();
    now = DriveUntil(cluster, now, 3, {0, 1, 2, 3}, 6'000);
    for (uint32_t i{0}; i < 4; ++i) {
        BOOST_CHECK_MESSAGE(cluster.Node(i).Owner().Head()->Header().height == 3, "seat " << i << " is at "
                                                                                             << cluster.Node(i).Owner().Head()->Header().height);
        BOOST_CHECK(cluster.Node(i).Owner().Head()->HeaderHash() == cluster.Node(0).Owner().Head()->HeaderHash());
        BOOST_CHECK(cluster.Node(i).Owner().Head()->Header().state_root == cluster.Node(0).Owner().Head()->Header().state_root);
    }
    BOOST_CHECK_GE(CountEvents(trace, "sync.request"), 1U);
    BOOST_CHECK_EQUAL(CountEvents(trace, "sync.apply", "slot=2"), 1U);
    BOOST_CHECK_EQUAL(cluster.Node(3).Sealed().size(), 3U);
    BOOST_CHECK_EQUAL(cluster.Node(3).Sealed()[1].height, 2U);
    BOOST_CHECK(!cluster.Node(3).Fenced());
    // A reply nobody asked for: one for the slot after the head is a push (O-105 (10)) and is checked like any fetched record
    // (this one holds slot 2, not slot 4); one for a farther slot is refused; one for an applied slot is late and ignored.
    const Result<std::vector<unsigned char>> pushed{net::EncodeSync(net::DecisionsReply{4, *two}, profile)};
    RequireOk(pushed, "EncodeSync");
    RequireRefusal(cluster.Node(3).OnFrame(net::InboundFrame{0, net::Lane::BULK, *pushed}), ErrorCode::STALE_PARENT, seat::SEAT_SYNC_SLOT,
                   "a pushed record of another slot");
    const Result<std::vector<unsigned char>> stray{net::EncodeSync(net::DecisionsReply{5, *two}, profile)};
    RequireOk(stray, "EncodeSync");
    RequireRefusal(cluster.Node(3).OnFrame(net::InboundFrame{0, net::Lane::BULK, *stray}), ErrorCode::INVALID_INPUT, seat::SEAT_SYNC_UNSOLICITED,
                   "unsolicited reply");
    const Result<std::vector<unsigned char>> late{net::EncodeSync(net::DecisionsReply{1, *one}, profile)};
    RequireOk(late, "EncodeSync");
    RequireOk(cluster.Node(3).OnFrame(net::InboundFrame{0, net::Lane::BULK, *late}), "a late push for an applied slot is ignored");
    BOOST_CHECK_EQUAL(cluster.Node(3).Owner().Head()->Header().height, 3U);
}

BOOST_AUTO_TEST_CASE(a_seat_that_decided_pushes_the_decision_to_peers_stuck_in_a_view_change)
{
    // O-105 (10), the second stall the e2e found: with seat 3 away, seat 2 decides slot 2 alone (its COMMIT votes never reach
    // seats 0 and 1, which hold two commits each) and moves on; seats 0 and 1 time out into a view change that can never
    // gather a quorum of reports (two reporters, seat 2 past the slot, seat 3 gone), and nobody is ahead by f + 1 claims, so
    // the request path never fires. The seat that holds the decision hears their reports about the decided slot and pushes it.
    TraceRecorder trace;
    SimNodeConfig config;
    config.faults.partition = {{0}, {1}, {2}, {3}}; // the pump delivers nothing: every frame goes through this test's hands
    Result<std::unique_ptr<SimCluster>> created{SimCluster::Create(4, config, trace)};
    RequireOk(created, "SimCluster::Create(4)");
    SimCluster& cluster{**created};
    for (size_t i{0}; i < 4; ++i) RequireOk(cluster.Node(i).Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 1), {}), "AppendBlock");
    const auto is_commit{[](const std::vector<unsigned char>& frame) {
        return frame.size() > 2 && frame[1] == static_cast<unsigned char>(agreement::MessageType::VOTE) &&
               frame[2] == static_cast<unsigned char>(storage::VotePhase::COMMIT);
    }};
    size_t dropped{0};
    uint64_t now{1};
    // Propose when a leader can, deliver everything among seats 0..2 (seat 3 is away; seat 2's COMMIT votes are dropped on
    // request) until nothing moves, then pump time; until `wait_for` reach `height` or the limit passes.
    const auto drive{[&](uint64_t height, const std::vector<uint32_t>& wait_for, bool drop_commits_from_2) {
        const uint64_t deadline{now + 6'000};
        while (now < deadline) {
            if (const std::optional<uint32_t> leader{cluster.Leader()}) {
                SimNode& node{cluster.Node(*leader)};
                const std::optional<agreement::PbftStatus> status{node.AgreementStatus()};
                if (node.Owner().Head()->Header().height < height && status.has_value() && !status->proposal_accepted && !status->decided) {
                    (void)node.ProposeSlot();
                }
            }
            for (int round{0}; round < 64; ++round) {
                const std::vector<net::InProcessFrame> frames{cluster.Fabric()->Drain()};
                if (frames.empty()) break;
                for (const net::InProcessFrame& frame : frames) {
                    if (frame.from == 3 || frame.to == 3) continue;
                    if (drop_commits_from_2 && frame.from == 2 && is_commit(frame.bytes)) {
                        ++dropped;
                        continue;
                    }
                    (void)cluster.Node(frame.to).OnFrame(net::InboundFrame{frame.from, frame.lane, frame.bytes});
                }
            }
            RequireOk(cluster.Pump(now), "Pump");
            ++now;
            bool all{true};
            for (const uint32_t i : wait_for) all = all && cluster.Node(i).Owner().Head()->Header().height >= height;
            if (all) return;
        }
    }};
    drive(1, {0, 1, 2}, false);
    for (uint32_t i{0}; i < 3; ++i) BOOST_REQUIRE_EQUAL(cluster.Node(i).Owner().Head()->Header().height, 1U);
    // Slot 2: seat 2 decides alone.
    drive(2, {2}, true);
    BOOST_REQUIRE_EQUAL(cluster.Node(2).Owner().Head()->Header().height, 2U);
    BOOST_CHECK_GE(dropped, 2U);
    BOOST_CHECK_EQUAL(cluster.Node(0).Owner().Head()->Header().height, 1U);
    BOOST_CHECK_EQUAL(cluster.Node(1).Owner().Head()->Header().height, 1U);
    // Everything flows again among the three. Without the push, seats 0 and 1 stay at height 1 for ever.
    drive(2, {0, 1, 2}, false);
    for (uint32_t i{0}; i < 3; ++i) {
        BOOST_CHECK_MESSAGE(cluster.Node(i).Owner().Head()->Header().height >= 2, "seat " << i << " is stuck at height "
                                                                                           << cluster.Node(i).Owner().Head()->Header().height);
    }
    BOOST_CHECK_GE(CountEvents(trace, "sync.push", "slot=2"), 1U);
    BOOST_CHECK_EQUAL(CountEvents(trace, "sync.apply", "slot=2"), 2U);
    BOOST_CHECK_EQUAL(CountEvents(trace, "sync.request"), 0U); // one peer ahead is not f + 1 claims: the request path never fired
    // And the three go on to slot 3 on one head.
    drive(3, {0, 1, 2}, false);
    for (uint32_t i{0}; i < 3; ++i) {
        BOOST_CHECK_EQUAL(cluster.Node(i).Owner().Head()->Header().height, 3U);
        BOOST_CHECK(cluster.Node(i).Owner().Head()->HeaderHash() == cluster.Node(0).Owner().Head()->HeaderHash());
        BOOST_CHECK(!cluster.Node(i).Fenced());
    }
}

BOOST_AUTO_TEST_CASE(a_seat_behind_by_more_than_sixteen_slots_catches_up)
{
    // Review of 2026-11-16, confirmed defect 2: catch-up served only the last sixteen decisions, so a seat that
    // missed more than that (about sixteen seconds of heartbeats) never rejoined. Seat 3 is alone behind a
    // partition while the majority decides twenty slots; after the heal it fetches every one of them, several
    // per request, and takes part in the next slot.
    TraceRecorder trace;
    SimNodeConfig config;
    config.faults.partition = {{0, 1, 2}, {3}};
    Result<std::unique_ptr<SimCluster>> created{SimCluster::Create(4, config, trace)};
    RequireOk(created, "SimCluster::Create(4)");
    SimCluster& cluster{**created};
    for (size_t i{0}; i < 4; ++i) RequireOk(cluster.Node(i).Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 1), {}), "AppendBlock");
    constexpr uint64_t MISSED{20};
    uint64_t now{1};
    now = DriveUntil(cluster, now, MISSED, {0, 1, 2}, 120'000);
    for (uint32_t i{0}; i < 3; ++i) BOOST_REQUIRE_EQUAL(cluster.Node(i).Owner().Head()->Header().height, MISSED);
    BOOST_CHECK_EQUAL(cluster.Node(3).Owner().Head()->Header().height, 0U);
    BOOST_CHECK_EQUAL(CountEvents(trace, "sync.apply"), 0U);
    cluster.HealPartition();
    now = DriveUntil(cluster, now, MISSED + 1, {0, 1, 2, 3}, 120'000);
    for (uint32_t i{0}; i < 4; ++i) {
        BOOST_CHECK_MESSAGE(cluster.Node(i).Owner().Head()->Header().height >= MISSED + 1,
                            "seat " << i << " is at " << cluster.Node(i).Owner().Head()->Header().height);
        BOOST_CHECK(cluster.Node(i).Owner().Head()->HeaderHash() == cluster.Node(0).Owner().Head()->HeaderHash());
    }
    // Every missed slot was fetched and applied, in far fewer requests than slots (several decisions per request).
    BOOST_CHECK_GE(CountEvents(trace, "sync.apply"), MISSED);
    BOOST_CHECK_EQUAL(CountEvents(trace, "sync.apply", "slot=1,"), 1U);
    BOOST_CHECK_EQUAL(CountEvents(trace, "sync.apply", "slot=20,"), 1U);
    BOOST_CHECK_LT(CountEvents(trace, "sync.request"), MISSED);
    BOOST_CHECK(!cluster.Node(3).Fenced());
}

BOOST_AUTO_TEST_SUITE_END()
