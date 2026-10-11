// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_action_pool.h>

#include <flowmesh/auth.h>
#include <flowmesh/production_engine.h>
#include <key.h>
#include <test/util/flowmesh.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <vector>

namespace {

uint256 Filled(const unsigned char value)
{
    uint256 out;
    std::fill(out.begin(), out.end(), value);
    return out;
}

struct PoolFixture {
    const uint256 domain{Filled(0x21)};
    const modern::AssetId asset{Filled(0x22)};
    const flowmesh::MarketId market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    flowmesh::FlowMeshState state{*flowmesh::ComputeFlowMeshVaultId(domain, market), asset,
                                  modern::NativeAsset(), flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    node::FlowMeshActionPool pool;
    std::vector<CKey> keys;
    const flowmesh::WireClock::time_point now{};

    explicit PoolFixture(size_t accounts = 2, const node::FlowMeshActionPoolLimits& limits = {})
        : pool{limits}
    {
        for (size_t i{0}; i < accounts; ++i) keys.emplace_back().MakeNewKey(true);
        pool.SetVerifier([this](const flowmesh::Action& action) {
            return flowmesh::CheckProductionActionCredential(action, domain, state.ConfigId(), market);
        });
    }

    flowmesh::AccountId Account(size_t i) const { return flowmesh::AccountForKey(XOnlyPubKey{keys[i].GetPubKey()}); }

    flowmesh::Action Signed(size_t account, uint64_t sequence,
                            flowmesh::ActionType type = flowmesh::ActionType::CANCEL_ASK)
    {
        flowmesh::Action action;
        action.signer = Account(account);
        action.sequence = sequence;
        action.type = static_cast<uint8_t>(type);
        if (type == flowmesh::ActionType::SUBMIT_BID || type == flowmesh::ActionType::SUBMIT_ASK) {
            action.curve = {{10, static_cast<CAmount>(sequence + 1)}};
        }
        if (type == flowmesh::ActionType::WITHDRAW) {
            action.asset = modern::NativeAsset();
            action.amount = 1;
            action.destination = Filled(0x55);
        }
        BOOST_REQUIRE(flowmesh::SignAction(keys[account], domain, state.ConfigId(), action));
        return action;
    }

    static flowmesh::Action Deposit(uint32_t n)
    {
        flowmesh::Action action;
        action.type = static_cast<uint8_t>(flowmesh::ActionType::DEPOSIT);
        action.outpoint = COutPoint{Txid::FromUint256(Filled(0x31)), n};
        return action;
    }
};

std::vector<uint256> Ids(const std::vector<flowmesh::Action>& actions)
{
    std::vector<uint256> out;
    for (const auto& action : actions) out.push_back(action.Id());
    return out;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_action_pool_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(action_pool_move_preserves_add_select_prune_behaviour)
{
    using Code = flowmesh::ActionReasonCode;
    PoolFixture f;
    const auto a0{f.Signed(0, 0)}, a1{f.Signed(0, 1)}, a3{f.Signed(0, 3)};
    const auto b0{f.Signed(1, 0)};
    const auto deposit{PoolFixture::Deposit(0)};
    BOOST_CHECK(f.pool.Add(a0, node::FLOWMESH_LOCAL_ACTION_PEER, f.now) == Code::ADMITTED);
    BOOST_CHECK(f.pool.Add(a1, 7, f.now) == Code::ADMITTED);
    BOOST_CHECK(f.pool.Add(a3, 7, f.now) == Code::ADMITTED);
    BOOST_CHECK(f.pool.Add(b0, 8, f.now) == Code::ADMITTED);
    BOOST_CHECK(f.pool.Add(deposit, 9, f.now) == Code::ADMITTED);
    BOOST_CHECK_EQUAL(f.pool.Size(), 5U);

    // Same id, forged credential, conflicting sequence, conflicting deposit.
    BOOST_CHECK(f.pool.Add(a0, 7, f.now) == Code::DUPLICATE_PENDING);
    BOOST_CHECK(f.pool.ContainsExact(a0));
    auto forged{f.Signed(0, 2)};
    forged.credential.back() ^= 1;
    BOOST_CHECK(f.pool.Add(forged, 7, f.now) == Code::UNAUTHENTICATED);
    const auto conflicting{f.Signed(0, 1, flowmesh::ActionType::CANCEL_BID)};
    BOOST_CHECK(f.pool.Add(conflicting, 7, f.now) == Code::SEQUENCE_CONFLICT);
    BOOST_CHECK(!f.pool.ContainsExact(conflicting));
    auto bad_shape{PoolFixture::Deposit(1)};
    bad_shape.sequence = 1;
    BOOST_CHECK(f.pool.Add(bad_shape, 7, f.now) == Code::MALFORMED_SHAPE);
    BOOST_CHECK_EQUAL(f.pool.Size(), 5U);

    // Deposits first, then each signer's contiguous run from NextSequence.
    const auto selected{Ids(f.pool.Select(f.state, 1024))};
    const std::vector<uint256> expected_order{deposit.Id(),
        f.Account(0) < f.Account(1) ? a0.Id() : b0.Id(),
        f.Account(0) < f.Account(1) ? a1.Id() : a0.Id(),
        f.Account(0) < f.Account(1) ? b0.Id() : a1.Id()};
    BOOST_CHECK(selected == expected_order);
    BOOST_CHECK_EQUAL(f.pool.Select(f.state, 2).size(), 2U);

    // Evidence requires every exact semantic action.
    const auto canonical{flowmesh::CanonicalizeActions({a0, a1})};
    const auto evidence{f.pool.EvidenceFor(canonical)};
    BOOST_REQUIRE(evidence);
    BOOST_CHECK_EQUAL(evidence->size(), 2U);
    BOOST_CHECK(!f.pool.EvidenceFor(flowmesh::CanonicalizeActions({f.Signed(0, 9)})));

    // Certified disposal removes exact ids; prune drops consumed sequences.
    f.pool.DropCertified(flowmesh::CanonicalizeActions({a0}));
    BOOST_CHECK(!f.pool.ContainsExact(a0));
    flowmesh::test_only::StateFunding::SetNextSequence(f.state, f.Account(0), 2);
    f.pool.Prune(f.state);
    BOOST_CHECK(!f.pool.ContainsExact(a1));
    BOOST_CHECK(f.pool.ContainsExact(a3));
    BOOST_CHECK(f.pool.ContainsExact(b0));
    BOOST_CHECK(f.pool.ContainsExact(deposit));
    BOOST_CHECK_EQUAL(f.pool.Size(), 3U);
    BOOST_CHECK(node::FlowMeshActionPool::StillPending(a3, f.state));
    BOOST_CHECK(!node::FlowMeshActionPool::StillPending(a1, f.state));

    // Per-peer quota: 256 entries per remote peer and market, none locally.
    PoolFixture g{1};
    for (uint64_t n{0}; n < flowmesh::FLOWMESH_ACTION_POOL_PER_PEER_MARKET_COUNT; ++n) {
        BOOST_REQUIRE(g.pool.Add(PoolFixture::Deposit(static_cast<uint32_t>(n)), 5, g.now) == Code::ADMITTED);
    }
    BOOST_CHECK(g.pool.Add(PoolFixture::Deposit(1000), 5, g.now) == Code::PEER_QUOTA);
    BOOST_CHECK(g.pool.Add(PoolFixture::Deposit(1000), 6, g.now) == Code::ADMITTED);
    BOOST_CHECK(g.pool.Add(PoolFixture::Deposit(1001), node::FLOWMESH_LOCAL_ACTION_PEER, g.now) == Code::ADMITTED);
}

BOOST_AUTO_TEST_CASE(health_probe_counts_only_includable_signed_actions)
{
    using Code = flowmesh::ActionReasonCode;
    PoolFixture f;
    BOOST_CHECK(!f.pool.HasEligibleSignedAction(f.state));
    // A deposit can wait for its anchor depth: Select would include it, but
    // the stall probe leaves deposits out.
    BOOST_REQUIRE(f.pool.Add(PoolFixture::Deposit(0), 9, f.now) == Code::ADMITTED);
    BOOST_CHECK(!f.pool.Select(f.state, 1).empty());
    BOOST_CHECK(!f.pool.HasEligibleSignedAction(f.state));
    // A gap (sequence 3 while the account expects 0) is not includable.
    BOOST_REQUIRE(f.pool.Add(f.Signed(0, 3), 7, f.now) == Code::ADMITTED);
    BOOST_CHECK(!f.pool.HasEligibleSignedAction(f.state));
    // The account's next sequence is includable, exactly as Select sees it.
    const auto next{f.Signed(1, 0)};
    BOOST_REQUIRE(f.pool.Add(next, 8, f.now) == Code::ADMITTED);
    BOOST_CHECK(f.pool.HasEligibleSignedAction(f.state));
    const auto selected{Ids(f.pool.Select(f.state, 1024))};
    BOOST_CHECK(std::find(selected.begin(), selected.end(), next.Id()) != selected.end());
}

BOOST_AUTO_TEST_CASE(action_pool_add_reports_typed_results)
{
    using Code = flowmesh::ActionReasonCode;
    PoolFixture f;
    const auto a0{f.Signed(0, 0)};
    // Same order as ever: shape, authentication, duplicate id, capacity,
    // per-peer quota, conflicting deposit or sequence.
    auto malformed{a0};
    malformed.amount = 1;
    BOOST_CHECK(f.pool.Add(malformed, 7, f.now) == Code::MALFORMED_SHAPE);
    node::FlowMeshActionPool unverified;
    BOOST_CHECK(unverified.Add(a0, 7, f.now) == Code::UNAUTHENTICATED);
    BOOST_CHECK(f.pool.Add(a0, 7, f.now) == Code::ADMITTED);
    BOOST_CHECK(f.pool.Add(a0, 8, f.now) == Code::DUPLICATE_PENDING);
    auto recredentialed{a0};
    BOOST_REQUIRE(flowmesh::SignAction(f.keys[0], f.domain, f.state.ConfigId(), recredentialed));
    BOOST_CHECK(f.pool.Add(recredentialed, 8, f.now) == Code::DUPLICATE_PENDING);
    BOOST_CHECK(f.pool.Add(f.Signed(0, 0, flowmesh::ActionType::CANCEL_BID), 8, f.now) == Code::SEQUENCE_CONFLICT);
    BOOST_CHECK(f.pool.Add(PoolFixture::Deposit(4), 8, f.now) == Code::ADMITTED);
    auto same_outpoint{PoolFixture::Deposit(4)};
    BOOST_CHECK(f.pool.Add(same_outpoint, 9, f.now) == Code::DUPLICATE_PENDING);
    // A full pool is POOL_FULL for every origin.
    PoolFixture full{1};
    for (uint32_t n{0}; n < flowmesh::FLOWMESH_ACTION_POOL_PER_MARKET_COUNT; ++n) {
        BOOST_REQUIRE(full.pool.Add(PoolFixture::Deposit(n), node::FLOWMESH_LOCAL_ACTION_PEER, full.now) == Code::ADMITTED);
    }
    BOOST_CHECK(full.pool.Add(PoolFixture::Deposit(70'000), node::FLOWMESH_LOCAL_ACTION_PEER, full.now) == Code::POOL_FULL);
    BOOST_CHECK(full.pool.Add(PoolFixture::Deposit(70'000), 3, full.now) == Code::POOL_FULL);
}

BOOST_AUTO_TEST_CASE(restored_evidence_is_not_api_origin)
{
    PoolFixture f;
    const auto api{f.Signed(0, 0)};
    const auto restored{f.Signed(1, 0)};
    const auto remote{f.Signed(0, 1)};
    BOOST_REQUIRE(f.pool.Add(api, node::FLOWMESH_LOCAL_ACTION_PEER, f.now, {.api_origin = true}) ==
                  flowmesh::ActionReasonCode::ADMITTED);
    BOOST_REQUIRE(f.pool.Add(restored, node::FLOWMESH_LOCAL_ACTION_PEER, f.now, {}) ==
                  flowmesh::ActionReasonCode::ADMITTED);
    BOOST_REQUIRE(f.pool.Add(remote, 5, f.now) == flowmesh::ActionReasonCode::ADMITTED);
    flowmesh::test_only::StateFunding::SetNextSequence(f.state, f.Account(0), 5);
    flowmesh::test_only::StateFunding::SetNextSequence(f.state, f.Account(1), 5);
    // Everything stale is dropped; only the API-origin signed action is
    // reported. Restored evidence and remote frames are never reported.
    const auto reported{f.pool.Prune(f.state, 256)};
    BOOST_CHECK_EQUAL(f.pool.Size(), 0U);
    BOOST_REQUIRE_EQUAL(reported.size(), 1U);
    BOOST_CHECK(reported.front().Id() == api.Id());
    BOOST_CHECK_EQUAL(f.pool.UnreportedDrops(), 0U);
    // The report is capped; the rest are counted.
    PoolFixture g{1};
    for (uint64_t n{0}; n < 5; ++n) {
        BOOST_REQUIRE(g.pool.Add(g.Signed(0, n), node::FLOWMESH_LOCAL_ACTION_PEER, g.now, {.api_origin = true}) ==
                      flowmesh::ActionReasonCode::ADMITTED);
    }
    flowmesh::test_only::StateFunding::SetNextSequence(g.state, g.Account(0), 5);
    BOOST_CHECK_EQUAL(g.pool.Prune(g.state, 3).size(), 3U);
    BOOST_CHECK_EQUAL(g.pool.UnreportedDrops(), 2U);
    BOOST_CHECK_EQUAL(g.pool.Size(), 0U);
    // The default report bound is zero: today's silent prune.
    PoolFixture h{1};
    BOOST_REQUIRE(h.pool.Add(h.Signed(0, 0), node::FLOWMESH_LOCAL_ACTION_PEER, h.now, {.api_origin = true}) ==
                  flowmesh::ActionReasonCode::ADMITTED);
    flowmesh::test_only::StateFunding::SetNextSequence(h.state, h.Account(0), 1);
    BOOST_CHECK(h.pool.Prune(h.state).empty());
    BOOST_CHECK_EQUAL(h.pool.UnreportedDrops(), 1U);
}

BOOST_AUTO_TEST_CASE(prune_tells_its_observer_every_dropped_action)
{
    PoolFixture f;
    const auto api{f.Signed(0, 0)};
    const auto restored{f.Signed(1, 0)};
    const auto remote{f.Signed(0, 1)};
    const auto open{f.Signed(1, 7)};
    BOOST_REQUIRE(f.pool.Add(api, node::FLOWMESH_LOCAL_ACTION_PEER, f.now, {.api_origin = true}) ==
                  flowmesh::ActionReasonCode::ADMITTED);
    BOOST_REQUIRE(f.pool.Add(restored, node::FLOWMESH_LOCAL_ACTION_PEER, f.now, {}) ==
                  flowmesh::ActionReasonCode::ADMITTED);
    BOOST_REQUIRE(f.pool.Add(remote, 5, f.now) == flowmesh::ActionReasonCode::ADMITTED);
    BOOST_REQUIRE(f.pool.Add(open, 5, f.now) == flowmesh::ActionReasonCode::ADMITTED);
    BOOST_CHECK(f.pool.Contains(api.Id()));
    flowmesh::test_only::StateFunding::SetNextSequence(f.state, f.Account(0), 5);
    flowmesh::test_only::StateFunding::SetNextSequence(f.state, f.Account(1), 5);
    // Every dropped action is observed, whatever its origin and whether or
    // not the bounded report names it; a kept one is not.
    std::vector<uint256> observed;
    const auto reported{f.pool.Prune(f.state, 0, [&](const uint256& id) { observed.push_back(id); })};
    BOOST_CHECK(reported.empty());
    std::sort(observed.begin(), observed.end());
    std::vector<uint256> expected{api.Id(), restored.Id(), remote.Id()};
    std::sort(expected.begin(), expected.end());
    BOOST_CHECK(observed == expected);
    BOOST_CHECK_EQUAL(f.pool.Size(), 1U);
    BOOST_CHECK(f.pool.Contains(open.Id()));
    BOOST_CHECK(!f.pool.Contains(api.Id()));
}

namespace {
node::FlowMeshActionPoolLimits SmallLimits()
{
    node::FlowMeshActionPoolLimits limits;
    limits.market_count = 10;
    limits.local_submit_reserve_count = 3;
    return limits;
}
constexpr node::FlowMeshPoolAddContext LOCAL_ORDER{.api_origin = true, .priority_eligible = false};
constexpr node::FlowMeshPoolAddContext LOCAL_HEAD_CANCEL{.api_origin = true, .priority_eligible = true};
} // namespace

BOOST_AUTO_TEST_CASE(local_order_refused_when_only_reserve_left)
{
    using Code = flowmesh::ActionReasonCode;
    PoolFixture f{1, SmallLimits()};
    for (uint64_t n{0}; n < 7; ++n) {
        BOOST_REQUIRE(f.pool.Add(f.Signed(0, n, flowmesh::ActionType::SUBMIT_BID), node::FLOWMESH_LOCAL_ACTION_PEER,
                                 f.now, LOCAL_ORDER) == Code::ADMITTED);
    }
    // Only the reserve is left: local orders and local deposits stop.
    BOOST_CHECK(f.pool.Add(f.Signed(0, 7, flowmesh::ActionType::SUBMIT_ASK), node::FLOWMESH_LOCAL_ACTION_PEER,
                           f.now, LOCAL_ORDER) == Code::SUBMIT_LANE_FULL);
    BOOST_CHECK(f.pool.Add(PoolFixture::Deposit(1), node::FLOWMESH_LOCAL_ACTION_PEER, f.now, LOCAL_ORDER) ==
                Code::SUBMIT_LANE_FULL);
    // local_cancel_and_withdraw_use_reserve: head cancels and withdrawals
    // fill the reserve; then the pool is full for everyone.
    PoolFixture g{3, SmallLimits()};
    for (uint64_t n{0}; n < 7; ++n) {
        BOOST_REQUIRE(g.pool.Add(g.Signed(0, n, flowmesh::ActionType::SUBMIT_BID), node::FLOWMESH_LOCAL_ACTION_PEER,
                                 g.now, LOCAL_ORDER) == Code::ADMITTED);
    }
    BOOST_CHECK(g.pool.Add(g.Signed(1, 0, flowmesh::ActionType::CANCEL_BID), node::FLOWMESH_LOCAL_ACTION_PEER,
                           g.now, LOCAL_HEAD_CANCEL) == Code::ADMITTED);
    BOOST_CHECK(g.pool.Add(g.Signed(1, 1, flowmesh::ActionType::WITHDRAW), node::FLOWMESH_LOCAL_ACTION_PEER,
                           g.now, LOCAL_HEAD_CANCEL) == Code::ADMITTED);
    BOOST_CHECK(g.pool.Add(g.Signed(2, 0, flowmesh::ActionType::CANCEL_ASK), node::FLOWMESH_LOCAL_ACTION_PEER,
                           g.now, LOCAL_HEAD_CANCEL) == Code::ADMITTED);
    BOOST_CHECK_EQUAL(g.pool.Size(), 10U);
    BOOST_CHECK(g.pool.Add(g.Signed(2, 1, flowmesh::ActionType::CANCEL_BID), node::FLOWMESH_LOCAL_ACTION_PEER,
                           g.now, LOCAL_HEAD_CANCEL) == Code::POOL_FULL);
}

BOOST_AUTO_TEST_CASE(far_head_priority_action_cannot_use_reserve)
{
    PoolFixture f{2, SmallLimits()};
    for (uint64_t n{0}; n < 7; ++n) {
        BOOST_REQUIRE(f.pool.Add(f.Signed(0, n, flowmesh::ActionType::SUBMIT_BID), node::FLOWMESH_LOCAL_ACTION_PEER,
                                 f.now, LOCAL_ORDER) == flowmesh::ActionReasonCode::ADMITTED);
    }
    // A cancel that is not at the head of its account is not priority.
    BOOST_CHECK(f.pool.Add(f.Signed(1, 20, flowmesh::ActionType::CANCEL_BID), node::FLOWMESH_LOCAL_ACTION_PEER, f.now,
                           LOCAL_ORDER) == flowmesh::ActionReasonCode::SUBMIT_LANE_FULL);
    BOOST_CHECK_EQUAL(node::FLOWMESH_POOL_PRIORITY_SEQUENCE_AHEAD, 16U);
}

BOOST_AUTO_TEST_CASE(remote_and_retained_evidence_use_full_capacity)
{
    PoolFixture f{1, SmallLimits()};
    for (uint64_t n{0}; n < 5; ++n) {
        BOOST_REQUIRE(f.pool.Add(f.Signed(0, n, flowmesh::ActionType::SUBMIT_BID), 7, f.now) ==
                      flowmesh::ActionReasonCode::ADMITTED);
    }
    for (uint64_t n{5}; n < 10; ++n) {
        BOOST_REQUIRE(f.pool.Add(f.Signed(0, n, flowmesh::ActionType::SUBMIT_ASK), node::FLOWMESH_LOCAL_ACTION_PEER,
                                 f.now, {}) == flowmesh::ActionReasonCode::ADMITTED);
    }
    BOOST_CHECK_EQUAL(f.pool.Size(), 10U);
    // The byte reserve binds the same way as the count reserve.
    node::FlowMeshActionPoolLimits bytes;
    bytes.local_submit_reserve_count = 0;
    const auto probe{f.Signed(0, 0, flowmesh::ActionType::SUBMIT_BID)};
    const size_t one{static_cast<size_t>(::GetSerializeSize(probe))};
    bytes.market_bytes = 4 * one;
    bytes.local_submit_reserve_bytes = 2 * one;
    PoolFixture g{1, bytes};
    for (uint64_t n{0}; n < 2; ++n) {
        BOOST_REQUIRE(g.pool.Add(g.Signed(0, n, flowmesh::ActionType::SUBMIT_BID), node::FLOWMESH_LOCAL_ACTION_PEER,
                                 g.now, LOCAL_ORDER) == flowmesh::ActionReasonCode::ADMITTED);
    }
    BOOST_CHECK(g.pool.Add(g.Signed(0, 2, flowmesh::ActionType::SUBMIT_BID), node::FLOWMESH_LOCAL_ACTION_PEER, g.now,
                           LOCAL_ORDER) == flowmesh::ActionReasonCode::SUBMIT_LANE_FULL);
    BOOST_CHECK(g.pool.Add(g.Signed(0, 2, flowmesh::ActionType::SUBMIT_BID), 9, g.now) ==
                flowmesh::ActionReasonCode::ADMITTED);
    // peer_quota_unchanged_256: production defaults keep the per-peer bound.
    const node::FlowMeshActionPoolLimits production;
    BOOST_CHECK_EQUAL(production.peer_count, 256U);
    BOOST_CHECK_EQUAL(production.market_count, 65'536U);
    BOOST_CHECK_EQUAL(production.local_submit_reserve_count, 4096U);
}

BOOST_AUTO_TEST_SUITE_END()
