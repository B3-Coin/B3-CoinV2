// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_local_admission.h>

#include <flowmesh/market.h>
#include <test/util/flowmesh.h>
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

using Kind = node::FlowMeshLocalAdmission::Kind;
using Code = flowmesh::ActionReasonCode;

struct AdmissionFixture {
    const uint256 domain{Filled(0x21)};
    const modern::AssetId asset{Filled(0x22)};
    const flowmesh::MarketId market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    flowmesh::FlowMeshState state{*flowmesh::ComputeFlowMeshVaultId(domain, market), asset, modern::NativeAsset(),
                                  flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    node::FlowMeshRecentCertifiedWindow recent;
    const flowmesh::AccountId signer{Filled(0x31)};

    flowmesh::Action Signed(uint64_t sequence, uint8_t type = 3) const
    {
        flowmesh::Action action;
        action.signer = signer;
        action.sequence = sequence;
        action.type = type;
        return action;
    }
    static flowmesh::Action Deposit(uint32_t n)
    {
        flowmesh::Action action;
        action.type = static_cast<uint8_t>(flowmesh::ActionType::DEPOSIT);
        action.outpoint = COutPoint{Txid::FromUint256(Filled(0x41)), n};
        return action;
    }
    //! Retain one certified entry with a re-execution record.
    uint256 Certify(uint64_t sequence, std::vector<flowmesh::Action> body, std::vector<uint256> applied,
                    std::vector<std::pair<uint256, flowmesh::ActionReject>> rejected = {})
    {
        flowmesh::ProductionCertifiedEnvelope certified;
        certified.entry.sequence = sequence;
        certified.entry.actions = flowmesh::CanonicalizeActions(std::move(body));
        node::FlowMeshRecentCertifiedWindow::Item item;
        item.sequence = sequence;
        item.hash = certified.entry.GetHash();
        std::vector<std::pair<uint256, uint32_t>> ids;
        for (const auto& action : certified.entry.actions) ids.emplace_back(action.Id(), static_cast<uint32_t>(ids.size()));
        item.ids = node::FlowMeshRecentCertifiedWindow::IndexIds(std::move(ids));
        item.envelope = std::make_shared<const flowmesh::ProductionCertifiedEnvelope>(std::move(certified));
        flowmesh::BatchResult result;
        result.applied = std::move(applied);
        result.rejected = std::move(rejected);
        std::sort(result.rejected.begin(), result.rejected.end());
        item.outcome = node::MakeProductionOutcomeRecord(sequence, item.hash, std::move(result), {});
        const auto hash{item.hash};
        recent.Push(std::move(item), {}, 0, 0);
        return hash;
    }
    node::FlowMeshLocalAdmission Decide(const flowmesh::Action& action,
                                        const std::optional<flowmesh::ClientEvent>& ring = std::nullopt) const
    {
        return node::FlowMeshDecideLocalAdmission(state, recent, ring, action);
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_local_admission_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(local_window_admits_next_plus_64_refuses_65)
{
    AdmissionFixture f;
    flowmesh::test_only::StateFunding::SetNextSequence(f.state, f.signer, 10);
    for (const uint64_t sequence : {10U, 11U, 74U}) {
        const auto decision{f.Decide(f.Signed(sequence))};
        BOOST_CHECK(decision.kind == Kind::ENQUEUE);
        BOOST_CHECK_EQUAL(decision.next_account_sequence, 10U);
    }
    const auto far_seq{f.Decide(f.Signed(75))};
    BOOST_CHECK(far_seq.kind == Kind::REFUSE);
    BOOST_CHECK(far_seq.code == Code::FAR_FUTURE_SEQUENCE);
    BOOST_CHECK(!far_seq.consumer);
    BOOST_CHECK(f.Decide(f.Signed(std::numeric_limits<uint64_t>::max())).kind == Kind::REFUSE);
    BOOST_CHECK_EQUAL(node::FlowMeshLocalAdmissionText(far_seq),
                      "FlowMesh action account sequence is more than 64 ahead of the certified next sequence; not queued");
}

BOOST_AUTO_TEST_CASE(resubmit_of_self_consumed_action_returns_certified_not_rejected)
{
    AdmissionFixture f;
    const auto action{f.Signed(0)};
    const auto hash{f.Certify(7, {action}, {}, {{action.Id(), flowmesh::ActionReject::REJECTED_BY_STATE}})};
    flowmesh::test_only::StateFunding::SetNextSequence(f.state, f.signer, 1);
    const auto decision{f.Decide(action)};
    BOOST_CHECK(decision.kind == Kind::ALREADY_INCLUDED);
    BOOST_CHECK(decision.code == Code::ALREADY_INCLUDED);
    BOOST_REQUIRE(decision.included);
    BOOST_CHECK_EQUAL(decision.included->first, 7U);
    BOOST_CHECK(decision.included->second == hash);
    BOOST_CHECK_EQUAL(node::FlowMeshLocalAdmissionText(decision),
                      "Exact action already included in certified microblock #7; not queued again");
    // Outside the window, the event ring's inclusion answers the same way.
    node::FlowMeshRecentCertifiedWindow empty;
    flowmesh::ClientEvent ring;
    ring.kind = flowmesh::ClientEventKind::CERTIFIED_INCLUDED;
    ring.microblock_sequence = 3;
    ring.microblock_hash = Filled(9);
    const auto from_ring{node::FlowMeshDecideLocalAdmission(f.state, empty, ring, action)};
    BOOST_CHECK(from_ring.kind == Kind::ALREADY_INCLUDED);
    BOOST_CHECK_EQUAL(from_ring.included->first, 3U);
    // A non-inclusion ring status does not count.
    ring.kind = flowmesh::ClientEventKind::POOL_ADMITTED;
    BOOST_CHECK(node::FlowMeshDecideLocalAdmission(f.state, empty, ring, action).kind == Kind::UNKNOWN_OUTCOME);
}

BOOST_AUTO_TEST_CASE(resubmit_of_credited_deposit_returns_certified_not_rejected)
{
    AdmissionFixture f;
    const auto deposit{AdmissionFixture::Deposit(1)};
    BOOST_CHECK(f.Decide(deposit).kind == Kind::ENQUEUE); // not consumed: queue as ever
    f.Certify(4, {deposit}, {deposit.Id()});
    BOOST_REQUIRE(flowmesh::test_only::StateFunding::CreditDeposit(f.state, deposit.outpoint, f.signer, f.asset, 5));
    const auto decision{f.Decide(deposit)};
    BOOST_CHECK(decision.kind == Kind::ALREADY_INCLUDED);
    BOOST_CHECK_EQUAL(decision.included->first, 4U);
}

BOOST_AUTO_TEST_CASE(credited_deposit_outside_window_is_unknown_not_rejected)
{
    AdmissionFixture f;
    const auto deposit{AdmissionFixture::Deposit(2)};
    BOOST_REQUIRE(flowmesh::test_only::StateFunding::CreditDeposit(f.state, deposit.outpoint, f.signer, f.asset, 5));
    const auto decision{f.Decide(deposit)};
    BOOST_CHECK(decision.kind == Kind::UNKNOWN_OUTCOME);
    BOOST_CHECK(decision.code == Code::DEPOSIT_ALREADY_CREDITED);
    BOOST_CHECK(!decision.included && !decision.consumer);
}

BOOST_AUTO_TEST_CASE(stale_sequence_with_other_consumer_refused_with_consumer)
{
    AdmissionFixture f;
    const auto consumer{f.Signed(0, 3)};
    const auto loser{f.Signed(0, 2)};
    const auto hash{f.Certify(9, {consumer}, {consumer.Id()})};
    flowmesh::test_only::StateFunding::SetNextSequence(f.state, f.signer, 1);
    const auto decision{f.Decide(loser)};
    BOOST_CHECK(decision.kind == Kind::REFUSE);
    BOOST_CHECK(decision.code == Code::STALE_SEQUENCE);
    BOOST_REQUIRE(decision.consumer);
    BOOST_CHECK(decision.consumer->action_id == consumer.Id());
    BOOST_CHECK_EQUAL(decision.consumer->microblock_sequence, 9U);
    BOOST_CHECK(decision.consumer->microblock_hash == hash);
    BOOST_CHECK_EQUAL(decision.next_account_sequence, 1U);
}

BOOST_AUTO_TEST_CASE(unattributed_stale_is_unknown_without_v1_event)
{
    AdmissionFixture f;
    const auto consumer{f.Signed(0, 3)};
    // The consumer is retained but only as BAD_SEQUENCE: no attribution.
    f.Certify(9, {consumer}, {}, {{consumer.Id(), flowmesh::ActionReject::BAD_SEQUENCE}});
    flowmesh::test_only::StateFunding::SetNextSequence(f.state, f.signer, 1);
    const auto decision{f.Decide(f.Signed(0, 2))};
    BOOST_CHECK(decision.kind == Kind::UNKNOWN_OUTCOME);
    BOOST_CHECK(decision.code == Code::STALE_SEQUENCE_UNATTRIBUTED);
    BOOST_CHECK(!decision.consumer);
    BOOST_CHECK(node::FlowMeshLocalAdmissionText(decision).find("outcome unknown") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(open_sequence_is_queued_even_when_included_without_execution)
{
    AdmissionFixture f;
    // Included as BAD_SEQUENCE while its sequence is still open: the exact
    // bytes may still execute, so a local resend is queued as before.
    const auto action{f.Signed(2)};
    f.Certify(1, {action}, {}, {{action.Id(), flowmesh::ActionReject::BAD_SEQUENCE}});
    flowmesh::test_only::StateFunding::SetNextSequence(f.state, f.signer, 1);
    BOOST_CHECK(f.Decide(action).kind == Kind::ENQUEUE);
}

BOOST_AUTO_TEST_CASE(ring_is_read_only_for_a_used_sequence_or_deposit)
{
    AdmissionFixture f;
    flowmesh::test_only::StateFunding::SetNextSequence(f.state, f.signer, 10);
    unsigned reads{0};
    const auto ring = [&]() -> std::optional<flowmesh::ClientEvent> { ++reads; return std::nullopt; };
    // The common path, an open sequence (or one too far ahead) or an unused
    // deposit, never scans the ring.
    BOOST_CHECK(node::FlowMeshDecideLocalAdmission(f.state, f.recent, ring, f.Signed(10)).kind == Kind::ENQUEUE);
    BOOST_CHECK(node::FlowMeshDecideLocalAdmission(f.state, f.recent, ring, f.Signed(75)).kind == Kind::REFUSE);
    BOOST_CHECK(node::FlowMeshDecideLocalAdmission(f.state, f.recent, ring, AdmissionFixture::Deposit(3)).kind == Kind::ENQUEUE);
    BOOST_CHECK_EQUAL(reads, 0U);
    // A used sequence consults it once.
    BOOST_CHECK(node::FlowMeshDecideLocalAdmission(f.state, f.recent, ring, f.Signed(9)).kind == Kind::UNKNOWN_OUTCOME);
    BOOST_CHECK_EQUAL(reads, 1U);
}

BOOST_AUTO_TEST_SUITE_END()
