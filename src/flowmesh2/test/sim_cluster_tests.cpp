// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

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

#include <memory>
#include <optional>
#include <string>
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

} // namespace

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
    const std::optional<std::vector<unsigned char>> two{cluster.Node(0).CommitLog().RecentDecisionRecord(2)};
    const std::optional<std::vector<unsigned char>> one{cluster.Node(0).CommitLog().RecentDecisionRecord(1)};
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
    // A reply nobody asked for is refused and changes nothing.
    const Result<std::vector<unsigned char>> stray{net::EncodeSync(net::DecisionsReply{4, *two}, profile)};
    RequireOk(stray, "EncodeSync");
    RequireRefusal(cluster.Node(3).OnFrame(net::InboundFrame{0, net::Lane::BULK, *stray}), ErrorCode::INVALID_INPUT, seat::SEAT_SYNC_UNSOLICITED,
                   "unsolicited reply");
    BOOST_CHECK_EQUAL(cluster.Node(3).Owner().Head()->Header().height, 3U);
}

BOOST_AUTO_TEST_SUITE_END()
