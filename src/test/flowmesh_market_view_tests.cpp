// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! Display derivations of verified FlowMesh state: the rationed side of a
//! clearing. Checked against brute-force demand and supply on the book
//! before the clearing, which the derivation never sees.

#include <flowmesh/market_view.h>

#include <flowmesh/state.h>
#include <random.h>
#include <test/util/asset.h>
#include <test/util/flowmesh.h>
#include <test/util/setup_common.h>
#include <util/int128.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <deque>
#include <limits>
#include <optional>
#include <vector>

namespace {

using Side = flowmesh::ClearingEngine::Side;
using Breakpoint = flowmesh::ClearingEngine::Breakpoint;
using flowmesh::ImbalanceSide;

const uint256 VAULT{uint256{"00000000000000000000000000000000000000000000000000000000000000f1"}};

modern::AssetId Base()
{
    return modern::test_only::SyntheticAssetId(COutPoint{
        Txid::FromUint256(uint256{"0000000000000000000000000000000000000000000000000000000000000011"}), 0});
}

flowmesh::AccountId Account(const uint8_t index)
{
    uint256 out;
    *out.begin() = index + 1;
    return out;
}

flowmesh::FlowMeshState EmptyState()
{
    return flowmesh::FlowMeshState{VAULT, Base(), modern::NativeAsset(), flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
}

void Fund(flowmesh::FlowMeshState& state, const flowmesh::AccountId& account, const CAmount amount = 1'000'000'000'000)
{
    BOOST_REQUIRE(flowmesh::test_only::StateFunding::Fund(state, account, modern::NativeAsset(), amount));
    BOOST_REQUIRE(flowmesh::test_only::StateFunding::Fund(state, account, Base(), amount));
}

//! A valid random curve: a limit shape or a general monotone curve.
std::vector<Breakpoint> RandomCurve(FastRandomContext& rng, const Side side)
{
    const bool limit{rng.randbool()};
    const CAmount price{static_cast<CAmount>(1 + rng.randrange(40))};
    const CAmount quantity{static_cast<CAmount>(1 + rng.randrange(1000))};
    if (limit) {
        return side == Side::BID ? *flowmesh::MakeLimitBidCurve(price, quantity)
                                 : *flowmesh::MakeLimitAskCurve(price, quantity);
    }
    const size_t points{2 + rng.randrange(flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS - 1)};
    std::vector<CAmount> prices;
    CAmount p{static_cast<CAmount>(rng.randrange(20))};
    for (size_t i{0}; i < points; ++i) {
        prices.push_back(p);
        p += 1 + static_cast<CAmount>(rng.randrange(8));
    }
    std::vector<CAmount> quantities;
    for (size_t i{0}; i < points; ++i) quantities.push_back(static_cast<CAmount>(rng.randrange(1000)));
    std::sort(quantities.begin(), quantities.end());
    if (side == Side::BID) {
        std::reverse(quantities.begin(), quantities.end());
        quantities.back() = 0;
        if (quantities.front() == 0) quantities.front() = 1 + static_cast<CAmount>(rng.randrange(1000));
    } else if (quantities.back() == 0) {
        quantities.back() = 1 + static_cast<CAmount>(rng.randrange(1000));
    }
    std::vector<Breakpoint> out;
    for (size_t i{0}; i < points; ++i) out.push_back({prices[i], quantities[i]});
    return out;
}

//! Demand or supply at `price` on the book, straight from the curves.
util::Signed128 SideTotal(const flowmesh::FlowMeshState& state, const Side side, const CAmount price)
{
    util::Signed128 total{0};
    for (const auto& curve : state.ReadCurves(std::numeric_limits<size_t>::max()).curves) {
        if (curve.side != side) continue;
        total += std::max<CAmount>(0, flowmesh::ClearingEngine::EvaluateCurve(curve.points, price) - curve.filled_quantity);
    }
    return total;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_market_view_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(imbalance_side_matches_brute_force_demand_and_supply)
{
    FastRandomContext rng{uint256{41}};
    size_t bids_long{0}, asks_long{0}, exact{0}, unmatched{0};
    for (int iteration{0}; iteration < 400; ++iteration) {
        auto state{EmptyState()};
        const uint8_t accounts{static_cast<uint8_t>(2 + rng.randrange(10))};
        for (uint8_t i{0}; i < accounts; ++i) Fund(state, Account(i));
        const auto submit = [&] {
            for (uint8_t i{0}; i < accounts; ++i) {
                for (const Side side : {Side::BID, Side::ASK}) {
                    if (rng.randrange(3) == 0) continue;
                    const auto curve{RandomCurve(rng, side)};
                    BOOST_REQUIRE(state.CurveIsValid(side, curve));
                    BOOST_REQUIRE(state.SubmitCurve(Account(i), side, curve));
                }
            }
        };
        // A first clearing leaves partially filled standing curves behind.
        submit();
        BOOST_REQUIRE(state.ClearSlot());
        if (rng.randbool()) submit();

        const auto pre{state};
        const auto clearing{state.ClearSlot()};
        BOOST_REQUIRE(clearing);
        const auto side{flowmesh::ObserveImbalanceSide(*clearing, state)};
        if (!clearing->cleared) {
            BOOST_CHECK(side == ImbalanceSide::NONE);
            ++unmatched;
            continue;
        }
        const auto demand{SideTotal(pre, Side::BID, clearing->price)};
        const auto supply{SideTotal(pre, Side::ASK, clearing->price)};
        const auto expected{demand > supply ? ImbalanceSide::BID : demand < supply ? ImbalanceSide::ASK : ImbalanceSide::NONE};
        BOOST_CHECK(side == expected);
        // The observed long-side residual is exactly the committed imbalance.
        const util::Signed128 residual{expected == ImbalanceSide::BID ? SideTotal(state, Side::BID, clearing->price)
                                                                     : SideTotal(state, Side::ASK, clearing->price)};
        BOOST_CHECK(residual == static_cast<util::Signed128>(clearing->imbalance));
        bids_long += expected == ImbalanceSide::BID;
        asks_long += expected == ImbalanceSide::ASK;
        exact += expected == ImbalanceSide::NONE;
    }
    // The sample covers every outcome.
    BOOST_CHECK(bids_long > 20 && asks_long > 20 && exact > 0 && unmatched > 0);
}

BOOST_AUTO_TEST_CASE(imbalance_side_counts_long_curves_that_received_nothing)
{
    // Three one-unit bids against one unit: largest-remainder rationing
    // gives the first account the unit and the other two nothing.
    auto bids{EmptyState()};
    for (uint8_t i{0}; i < 4; ++i) Fund(bids, Account(i));
    for (uint8_t i{0}; i < 3; ++i) BOOST_REQUIRE(bids.SubmitCurve(Account(i), Side::BID, *flowmesh::MakeLimitBidCurve(10, 1)));
    BOOST_REQUIRE(bids.SubmitCurve(Account(3), Side::ASK, *flowmesh::MakeLimitAskCurve(10, 1)));
    const auto rationed{bids.ClearSlot()};
    BOOST_REQUIRE(rationed && rationed->cleared);
    BOOST_CHECK_EQUAL(rationed->volume, 1);
    BOOST_CHECK_EQUAL(rationed->imbalance, 2);
    BOOST_CHECK_EQUAL(rationed->bid_fill.size(), 3U);
    BOOST_CHECK_EQUAL(rationed->bid_fill.at(Account(1)), 0);
    BOOST_CHECK(flowmesh::ObserveImbalanceSide(*rationed, bids) == ImbalanceSide::BID);

    // The mirror image rations the asks.
    auto asks{EmptyState()};
    for (uint8_t i{0}; i < 4; ++i) Fund(asks, Account(i));
    for (uint8_t i{0}; i < 3; ++i) BOOST_REQUIRE(asks.SubmitCurve(Account(i), Side::ASK, *flowmesh::MakeLimitAskCurve(10, 1)));
    BOOST_REQUIRE(asks.SubmitCurve(Account(3), Side::BID, *flowmesh::MakeLimitBidCurve(10, 1)));
    const auto sellers{asks.ClearSlot()};
    BOOST_REQUIRE(sellers && sellers->cleared);
    BOOST_CHECK_EQUAL(sellers->imbalance, 2);
    BOOST_CHECK(flowmesh::ObserveImbalanceSide(*sellers, asks) == ImbalanceSide::ASK);
}

BOOST_AUTO_TEST_CASE(imbalance_side_is_unknown_when_the_book_disagrees)
{
    auto state{EmptyState()};
    for (uint8_t i{0}; i < 2; ++i) Fund(state, Account(i));
    BOOST_REQUIRE(state.SubmitCurve(Account(0), Side::BID, *flowmesh::MakeLimitBidCurve(10, 5)));
    BOOST_REQUIRE(state.SubmitCurve(Account(1), Side::ASK, *flowmesh::MakeLimitAskCurve(10, 3)));
    const auto pre{state};
    const auto clearing{state.ClearSlot()};
    BOOST_REQUIRE(clearing && clearing->cleared);
    BOOST_CHECK(flowmesh::ObserveImbalanceSide(*clearing, state) == ImbalanceSide::BID);
    // The book before the clearing still has quantity on both sides.
    BOOST_CHECK(flowmesh::ObserveImbalanceSide(*clearing, pre) == ImbalanceSide::UNKNOWN);
    // A residual that is not the committed imbalance.
    auto altered{*clearing};
    altered.imbalance = 1;
    BOOST_CHECK(flowmesh::ObserveImbalanceSide(altered, state) == ImbalanceSide::UNKNOWN);
    altered.imbalance = MAX_MONEY;
    BOOST_CHECK(flowmesh::ObserveImbalanceSide(altered, state) == ImbalanceSide::UNKNOWN);
    // No residual, but a nonzero imbalance.
    auto empty{EmptyState()};
    altered.imbalance = 2;
    BOOST_CHECK(flowmesh::ObserveImbalanceSide(altered, empty) == ImbalanceSide::UNKNOWN);
}

BOOST_AUTO_TEST_CASE(imbalance_side_accepts_only_a_residual_consistent_with_saturation)
{
    // Two zero-price bids of MAX_MONEY reserve nothing; one unit of supply
    // leaves 2*MAX_MONEY - 1 of demand, committed as MAX_MONEY.
    auto state{EmptyState()};
    for (uint8_t i{0}; i < 3; ++i) Fund(state, Account(i));
    BOOST_REQUIRE(state.SubmitCurve(Account(0), Side::BID, *flowmesh::MakeLimitBidCurve(0, MAX_MONEY)));
    BOOST_REQUIRE(state.SubmitCurve(Account(1), Side::BID, *flowmesh::MakeLimitBidCurve(0, MAX_MONEY)));
    BOOST_REQUIRE(state.SubmitCurve(Account(2), Side::ASK, *flowmesh::MakeLimitAskCurve(0, 1)));
    const auto clearing{state.ClearSlot()};
    BOOST_REQUIRE(clearing && clearing->cleared);
    BOOST_CHECK_EQUAL(clearing->price, 0);
    BOOST_CHECK_EQUAL(clearing->imbalance, MAX_MONEY);
    BOOST_CHECK(flowmesh::ObserveImbalanceSide(*clearing, state) == ImbalanceSide::BID);
}

BOOST_AUTO_TEST_CASE(imbalance_side_is_none_without_a_clearing_or_for_an_exact_match)
{
    auto state{EmptyState()};
    for (uint8_t i{0}; i < 2; ++i) Fund(state, Account(i));
    BOOST_REQUIRE(state.SubmitCurve(Account(0), Side::BID, *flowmesh::MakeLimitBidCurve(9, 5)));
    BOOST_REQUIRE(state.SubmitCurve(Account(1), Side::ASK, *flowmesh::MakeLimitAskCurve(10, 5)));
    const auto uncrossed{state.ClearSlot()};
    BOOST_REQUIRE(uncrossed && !uncrossed->cleared);
    BOOST_CHECK(flowmesh::ObserveImbalanceSide(*uncrossed, state) == ImbalanceSide::NONE);
    BOOST_REQUIRE(state.SubmitCurve(Account(0), Side::BID, *flowmesh::MakeLimitBidCurve(10, 5)));
    const auto matched{state.ClearSlot()};
    BOOST_REQUIRE(matched && matched->cleared);
    BOOST_CHECK_EQUAL(matched->imbalance, 0);
    BOOST_CHECK(flowmesh::ObserveImbalanceSide(*matched, state) == ImbalanceSide::NONE);
    BOOST_CHECK(state.ReadCurves(8).curves.empty());
}

BOOST_AUTO_TEST_CASE(clearing_tick_pages_are_bounded_ascending_and_report_eviction_gaps)
{
    // A full cache after eviction: sequences 300..555.
    std::deque<flowmesh::MarketHistoryEntry> history;
    for (uint64_t sequence{300}; sequence < 300 + flowmesh::MARKET_DATA_HISTORY_CAPACITY; ++sequence) {
        flowmesh::MarketHistoryEntry entry;
        entry.sequence = sequence;
        *entry.microblock_hash.begin() = static_cast<uint8_t>(sequence);
        entry.cleared = sequence % 3 == 0;
        entry.price = entry.cleared ? 1'000 + static_cast<CAmount>(sequence) : 0;
        entry.quantity = entry.cleared ? 7 : 0;
        entry.imbalance = entry.cleared ? 2 : 0;
        entry.imbalance_side = entry.cleared ? ImbalanceSide::ASK : ImbalanceSide::NONE;
        entry.handoff = sequence == 400;
        entry.settlement = sequence == 401;
        history.push_back(entry);
    }
    const auto page = [&](std::optional<uint64_t> after, size_t limit) {
        return flowmesh::PageClearingTicks(history, {after, limit});
    };
    const auto sequences = [](const flowmesh::ClearingTickPage& value) {
        std::vector<uint64_t> out;
        for (const auto& tick : value.ticks) out.push_back(tick.sequence);
        return out;
    };

    const auto newest{page(std::nullopt, 3)};
    BOOST_REQUIRE(newest.available);
    BOOST_CHECK((sequences(newest) == std::vector<uint64_t>{553, 554, 555}));
    BOOST_CHECK(!newest.more && !newest.gap);
    BOOST_CHECK_EQUAL(*newest.oldest_retained_sequence, 300U);
    BOOST_CHECK_EQUAL(page(std::nullopt, flowmesh::MARKET_DATA_MAX_TICKS).ticks.size(), flowmesh::MARKET_DATA_HISTORY_CAPACITY);

    // Resuming: the first ticks after a position, then the rest.
    const auto resumed{page(398, 2)};
    BOOST_CHECK((sequences(resumed) == std::vector<uint64_t>{399, 400}));
    BOOST_CHECK(resumed.more && !resumed.gap);
    BOOST_REQUIRE_EQUAL(resumed.ticks.size(), 2U);
    BOOST_CHECK(resumed.ticks[0].cleared);
    BOOST_CHECK_EQUAL(resumed.ticks[0].price, 1'399);
    BOOST_CHECK_EQUAL(resumed.ticks[0].volume, 7);
    BOOST_CHECK_EQUAL(resumed.ticks[0].imbalance, 2);
    BOOST_CHECK(resumed.ticks[0].imbalance_side == ImbalanceSide::ASK);
    BOOST_CHECK(resumed.ticks[1].kind == flowmesh::ClearingTick::Kind::HANDOFF);
    BOOST_CHECK(resumed.ticks[1].imbalance_side == ImbalanceSide::NONE);
    BOOST_CHECK(page(400, 1).ticks.front().kind == flowmesh::ClearingTick::Kind::SETTLEMENT);
    const auto caught_up{page(555, 64)};
    BOOST_CHECK(caught_up.available && caught_up.ticks.empty() && !caught_up.more && !caught_up.gap);
    BOOST_CHECK(page(std::numeric_limits<uint64_t>::max(), 64).ticks.empty());

    // A position whose next tick was evicted is a gap, not a quiet market.
    const auto evicted{page(100, 2)};
    BOOST_CHECK(evicted.gap && evicted.more);
    BOOST_CHECK((sequences(evicted) == std::vector<uint64_t>{300, 301}));
    BOOST_CHECK(!page(299, 1).gap);
    BOOST_CHECK(page(298, 1).gap);

    for (const size_t limit : {size_t{0}, flowmesh::MARKET_DATA_MAX_TICKS + 1}) {
        const auto refused{page(std::nullopt, limit)};
        BOOST_CHECK(!refused.available && refused.ticks.empty());
        BOOST_CHECK_EQUAL(refused.unavailable_reason, "invalid_limit");
    }
    const auto empty{flowmesh::PageClearingTicks({}, {})};
    BOOST_CHECK(empty.available && empty.ticks.empty() && !empty.oldest_retained_sequence);
}

BOOST_AUTO_TEST_SUITE_END()
