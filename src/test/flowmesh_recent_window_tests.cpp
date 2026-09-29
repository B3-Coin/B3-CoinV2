// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_recent_window.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <memory>
#include <vector>

namespace {

uint256 Filled(const unsigned char value)
{
    uint256 out;
    std::fill(out.begin(), out.end(), value);
    return out;
}

flowmesh::Action Signed(const flowmesh::AccountId& signer, uint64_t sequence, uint8_t type = 3)
{
    flowmesh::Action action;
    action.signer = signer;
    action.sequence = sequence;
    action.type = type;
    return action;
}

using Window = node::FlowMeshRecentCertifiedWindow;

Window::Item Item(uint64_t sequence, std::vector<flowmesh::Action> actions,
                  std::shared_ptr<const node::ProductionOutcomeRecord> outcome = nullptr)
{
    flowmesh::ProductionCertifiedEnvelope certified;
    certified.entry.sequence = sequence;
    certified.entry.actions = flowmesh::CanonicalizeActions(std::move(actions));
    Window::Item item;
    item.sequence = sequence;
    item.hash = certified.entry.GetHash();
    std::vector<std::pair<uint256, uint32_t>> ids;
    for (const auto& action : certified.entry.actions) ids.emplace_back(action.Id(), static_cast<uint32_t>(ids.size()));
    item.ids = Window::IndexIds(std::move(ids));
    item.payload_bytes = Window::EstimatePayloadBytes(certified);
    item.envelope = std::make_shared<const flowmesh::ProductionCertifiedEnvelope>(std::move(certified));
    item.seat_count = 4;
    item.outcome = std::move(outcome);
    return item;
}

std::shared_ptr<const node::ProductionOutcomeRecord> Record(uint64_t sequence, const uint256& hash,
                                                            std::vector<uint256> applied,
                                                            std::vector<std::pair<uint256, flowmesh::ActionReject>> rejected = {})
{
    flowmesh::BatchResult result;
    result.applied = std::move(applied);
    result.rejected = std::move(rejected);
    std::sort(result.rejected.begin(), result.rejected.end());
    return node::MakeProductionOutcomeRecord(sequence, hash, std::move(result), {});
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_recent_window_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(recent_window_bounds_entries_bytes_and_global)
{
    // Counts: payloads keep the newest 64, outcome records the newest 1,024.
    Window window;
    for (uint64_t n{0}; n < 1100; ++n) {
        auto item{Item(n, {Signed(Filled(1), n)})};
        item.outcome = Record(n, item.hash, {item.envelope->entry.actions.front().Id()});
        window.Push(std::move(item), {}, 0, 0);
    }
    BOOST_CHECK_EQUAL(window.PayloadCount(), 64U);
    BOOST_CHECK_EQUAL(window.OutcomeCount(), 1024U);
    BOOST_CHECK_EQUAL(window.Size(), 1024U);
    BOOST_CHECK(!window.Find(75));
    BOOST_REQUIRE(window.Find(76));
    BOOST_CHECK(!window.Find(76)->envelope);
    BOOST_CHECK(window.Find(76)->outcome);
    BOOST_CHECK(window.Find(1036)->envelope);
    BOOST_CHECK(!window.Find(1035)->envelope);
    BOOST_CHECK(window.Find(1099)->envelope);

    // Per-market bytes: the newest envelope (the shared head) is never
    // evicted or counted; older ones leave oldest first.
    node::FlowMeshRecentWindowLimits small;
    const size_t one{Item(0, {Signed(Filled(2), 0)}).payload_bytes};
    small.payload_bytes = 2 * one;
    small.outcome_bytes = 1;
    Window bytes;
    for (uint64_t n{0}; n < 5; ++n) {
        auto item{Item(n, {Signed(Filled(2), n)})};
        item.outcome = Record(n, item.hash, {});
        bytes.Push(std::move(item), small, 0, 0);
    }
    BOOST_CHECK_EQUAL(bytes.PayloadCount(), 3U);
    BOOST_CHECK_EQUAL(bytes.PayloadExtraBytes(), 2 * one);
    BOOST_CHECK(bytes.Find(4)->envelope);
    BOOST_CHECK_EQUAL(bytes.OutcomeCount(), 0U); // even the newest record yields to the bound

    // Global: other markets' counted bytes leave no room, so only the head
    // (uncounted) and no record stay. Every bound holds after each push.
    Window global;
    for (uint64_t n{0}; n < 3; ++n) {
        auto item{Item(n, {Signed(Filled(3), n)})};
        item.outcome = Record(n, item.hash, {});
        global.Push(std::move(item), {}, node::FLOWMESH_RECENT_PAYLOAD_GLOBAL_BYTES,
                    node::FLOWMESH_RECENT_OUTCOME_GLOBAL_BYTES);
        BOOST_CHECK_EQUAL(global.PayloadExtraBytes(), 0U);
        BOOST_CHECK_EQUAL(global.OutcomeBytes(), 0U);
        BOOST_CHECK(global.Find(n)->envelope);
    }
    BOOST_CHECK_EQUAL(global.Size(), 1U);

    // A sequence gap clears the window (fail closed).
    auto gap{Item(1200, {Signed(Filled(1), 1200)})};
    window.Push(std::move(gap), {}, 0, 0);
    BOOST_CHECK_EQUAL(window.Size(), 1U);
    BOOST_CHECK_EQUAL(window.PayloadCount(), 1U);
    BOOST_CHECK_EQUAL(window.OutcomeCount(), 0U);
    BOOST_CHECK(!window.Find(1099));
}

BOOST_AUTO_TEST_CASE(recent_window_inclusion_prefers_decisive_disposition_then_newest)
{
    Window window;
    const auto action{Signed(Filled(4), 7)};
    // M0: BAD_SEQUENCE (not decisive); M1: applied; M2: BAD_SEQUENCE again.
    auto m0{Item(0, {action})};
    m0.outcome = Record(0, m0.hash, {}, {{action.Id(), flowmesh::ActionReject::BAD_SEQUENCE}});
    auto m1{Item(1, {action})};
    m1.outcome = Record(1, m1.hash, {action.Id()});
    auto m2{Item(2, {action})};
    m2.outcome = Record(2, m2.hash, {}, {{action.Id(), flowmesh::ActionReject::BAD_SEQUENCE}});
    const auto h1{m1.hash}, h2{m2.hash};
    window.Push(std::move(m0), {}, 0, 0);
    window.Push(std::move(m1), {}, 0, 0);
    window.Push(std::move(m2), {}, 0, 0);
    const auto found{window.Inclusion(action.Id())};
    BOOST_REQUIRE(found);
    BOOST_CHECK_EQUAL(found->first, 1U);
    BOOST_CHECK(found->second == h1);
    // Without any decisive record the newest inclusion is returned.
    Window plain;
    plain.Push(Item(0, {action}), {}, 0, 0);
    plain.Push(Item(1, {action}), {}, 0, 0);
    const auto newest{plain.Inclusion(action.Id())};
    BOOST_REQUIRE(newest);
    BOOST_CHECK_EQUAL(newest->first, 1U);
    BOOST_CHECK(!plain.Inclusion(Signed(Filled(4), 8).Id()));
    (void)h2;
}

BOOST_AUTO_TEST_CASE(recent_window_consumer_requires_a_retained_consuming_record)
{
    Window window;
    const auto consumer{Signed(Filled(5), 3)};
    const auto rejected{Signed(Filled(6), 0)};
    const auto bad{Signed(Filled(7), 9)};
    flowmesh::Action deposit;
    deposit.type = static_cast<uint8_t>(flowmesh::ActionType::DEPOSIT);
    deposit.outpoint = COutPoint{Txid::FromUint256(Filled(8)), 1};
    auto item{Item(0, {consumer, rejected, bad, deposit})};
    item.outcome = Record(0, item.hash, {consumer.Id(), deposit.Id()},
                          {{rejected.Id(), flowmesh::ActionReject::REJECTED_BY_STATE},
                           {bad.Id(), flowmesh::ActionReject::BAD_SEQUENCE}});
    const auto hash{item.hash};
    window.Push(std::move(item), {}, 0, 0);
    const auto found{window.FindConsumer(Filled(5), 3)};
    BOOST_REQUIRE(found);
    BOOST_CHECK(found->action_id == consumer.Id());
    BOOST_CHECK_EQUAL(found->microblock_sequence, 0U);
    BOOST_CHECK(found->microblock_hash == hash);
    BOOST_CHECK(window.FindConsumer(Filled(6), 0)); // rejected by state consumed it
    BOOST_CHECK(!window.FindConsumer(Filled(7), 9)); // BAD_SEQUENCE consumed nothing
    BOOST_CHECK(!window.FindConsumer(Filled(5), 4));
    // No record, or a too-large marker: no attribution.
    Window bare;
    bare.Push(Item(0, {consumer}), {}, 0, 0);
    BOOST_CHECK(!bare.FindConsumer(Filled(5), 3));
    Window marker;
    auto marked{Item(0, {consumer})};
    auto record{std::make_shared<node::ProductionOutcomeRecord>(*Record(0, marked.hash, {consumer.Id()}))};
    record->too_large = true;
    marked.outcome = record;
    marker.Push(std::move(marked), {}, 0, 0);
    BOOST_CHECK(!marker.FindConsumer(Filled(5), 3));
    // A handoff-like item carries a payload and no record.
    Window handoff;
    handoff.Push(Item(0, {}), {}, 0, 0);
    BOOST_CHECK(handoff.Find(0)->envelope);
    BOOST_CHECK(!handoff.Find(0)->outcome);
}

BOOST_AUTO_TEST_SUITE_END()
