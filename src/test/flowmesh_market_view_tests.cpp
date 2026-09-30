// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! Display derivations of verified FlowMesh state: the rationed side of a
//! clearing. Checked against brute-force demand and supply on the book
//! before the clearing, which the derivation never sees.

#include <flowmesh/market_view.h>

#include <flowmesh/microblock.h>
#include <flowmesh/state.h>
#include <random.h>
#include <test/util/asset.h>
#include <test/util/flowmesh.h>
#include <test/util/setup_common.h>
#include <util/int128.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
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

//! A step ladder as up to four (price, quantity) steps on one curve: a bid
//! buys each step's quantity at its price or below, an ask sells at or above.
std::vector<Breakpoint> Ladder(const Side side, std::vector<std::pair<CAmount, CAmount>> steps)
{
    std::sort(steps.begin(), steps.end());
    std::vector<Breakpoint> out;
    const auto add = [&](const CAmount price, const CAmount qty) {
        if (!out.empty() && out.back().price == price) out.back().qty = qty;
        else out.push_back({price, qty});
    };
    if (side == Side::BID) {
        CAmount total{0};
        for (const auto& step : steps) total += step.second;
        for (const auto& [price, qty] : steps) {
            add(price, total);
            total -= qty;
            add(price + 1, total);
        }
    } else {
        CAmount total{0};
        for (const auto& [price, qty] : steps) {
            if (price > 0) add(price - 1, total);
            total += qty;
            add(price, total);
        }
    }
    return out;
}

//! A random valid curve of any shape: a limit, a ladder or a general curve.
std::vector<Breakpoint> AnyCurve(FastRandomContext& rng, const Side side)
{
    if (rng.randrange(3) != 0) return RandomCurve(rng, side);
    std::vector<std::pair<CAmount, CAmount>> steps;
    CAmount price{static_cast<CAmount>(rng.randrange(12))};
    const size_t n{static_cast<size_t>(1 + rng.randrange(4))};
    for (size_t i{0}; i < n; ++i) {
        steps.emplace_back(price, 1 + static_cast<CAmount>(rng.randrange(500)));
        price += 1 + static_cast<CAmount>(rng.randrange(9));
    }
    return Ladder(side, steps);
}

using U128 = util::Unsigned128;

//! R(p) of a curve, with R(-1) = 0.
CAmount Remaining(const flowmesh::ClearingEngine::CurveView& curve, const CAmount price)
{
    if (price < 0) return 0;
    return std::max<CAmount>(0, flowmesh::ClearingEngine::EvaluateCurve(curve.points, price) - curve.filled_quantity);
}

//! Lots whose own limit is exactly `price`.
CAmount LevelAt(const flowmesh::ClearingEngine::CurveView& curve, const CAmount price)
{
    return curve.side == Side::BID ? Remaining(curve, price) - Remaining(curve, price + 1)
                                   : Remaining(curve, price) - Remaining(curve, price - 1);
}

//! The display bucket of a price by its definition, independently of the
//! implementation: the smallest or largest index whose edge accepts it.
std::pair<U128, bool> BruteBucket(const Side side, const CAmount price, const flowmesh::LevelGrouping& grouping)
{
    if (grouping.exact) return {static_cast<U128>(price), false};
    const auto pow10 = [](int n) { U128 out{1}; while (n-- > 0) out *= 10; return out; };
    if (!grouping.inverse) {
        const U128 ticks{grouping.mantissa * pow10(grouping.exponent + 9 - grouping.decimals)};
        U128 index{0};
        if (side == Side::BID) { while ((index + 1) * ticks <= static_cast<U128>(price)) ++index; }
        else { while (index * ticks < static_cast<U128>(price)) ++index; }
        return {index, false};
    }
    if (price == 0) return {0, true};
    const U128 scale{pow10(9 - grouping.decimals - grouping.exponent)};
    const U128 unit{grouping.mantissa * static_cast<U128>(price)};
    // Displayed sell (canonical bid): the smallest b with b x unit >= scale;
    // displayed buy (canonical ask): the largest b with b x unit <= scale.
    U128 index{scale / unit};
    if (side == Side::BID) { if (index * unit < scale) ++index; }
    return {index, false};
}

struct BruteLevel {
    U128 bucket{0};
    bool zero_price{false};
    CAmount any_price{0};
    U128 size{0}, notional{0};
    std::set<flowmesh::AccountId> accounts;
    CAmount own{0};
    //! Distinct prices of each curve in the bucket.
    std::map<flowmesh::AccountId, size_t> prices;
};

//! Every level of a side by a scan of every integer price up to `top`.
std::vector<BruteLevel> BruteLevels(const std::vector<flowmesh::ClearingEngine::CurveView>& curves, const Side side,
                                    const flowmesh::LevelGrouping& grouping, const flowmesh::AccountId& own, const CAmount top)
{
    std::map<std::pair<bool, U128>, BruteLevel> buckets;
    for (CAmount price{0}; price <= top; ++price) {
        for (const auto& curve : curves) {
            if (curve.side != side) continue;
            const CAmount level{LevelAt(curve, price)};
            if (level <= 0) continue;
            const auto [index, zero]{BruteBucket(side, price, grouping)};
            auto [it, inserted]{buckets.try_emplace({zero, index})};
            auto& bucket{it->second};
            if (inserted) { bucket.bucket = index; bucket.zero_price = zero; bucket.any_price = price; }
            bucket.size += static_cast<U128>(level);
            bucket.notional += static_cast<U128>(level) * static_cast<U128>(price);
            bucket.accounts.insert(curve.account_id);
            ++bucket.prices[curve.account_id];
            if (curve.account_id == own) bucket.own += level;
        }
    }
    std::vector<BruteLevel> out;
    for (auto& [key, bucket] : buckets) out.push_back(std::move(bucket));
    std::sort(out.begin(), out.end(), [&](const BruteLevel& a, const BruteLevel& b) {
        return side == Side::BID ? a.any_price > b.any_price : a.any_price < b.any_price;
    });
    return out;
}

flowmesh::LevelGrouping Grouping(const bool inverse, const uint8_t mantissa, const int32_t exponent, const uint8_t decimals)
{
    flowmesh::LevelGrouping grouping;
    grouping.exact = false;
    grouping.inverse = inverse;
    grouping.mantissa = mantissa;
    grouping.exponent = exponent;
    grouping.decimals = decimals;
    return grouping;
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

BOOST_AUTO_TEST_CASE(imbalance_side_observation_is_cheap_for_a_large_rationed_clearing)
{
    // 4,096 one-unit bids against 2,048 units: every bid is listed in the
    // fill map, half of them with nothing. The observation costs one lookup
    // per listed curve, after the commit's own allocation over the same set.
    constexpr uint16_t BIDS{4096};
    auto state{EmptyState()};
    const auto account = [](const uint16_t index) {
        uint256 out;
        *out.begin() = static_cast<uint8_t>(index & 0xff);
        *(out.begin() + 1) = static_cast<uint8_t>(index >> 8);
        *(out.begin() + 2) = 1;
        return out;
    };
    const flowmesh::AccountId seller{Account(0)};
    Fund(state, seller);
    BOOST_REQUIRE(state.SubmitCurve(seller, Side::ASK, *flowmesh::MakeLimitAskCurve(10, BIDS / 2)));
    for (uint16_t i{0}; i < BIDS; ++i) {
        Fund(state, account(i), 1'000);
        BOOST_REQUIRE(state.SubmitCurve(account(i), Side::BID, *flowmesh::MakeLimitBidCurve(10, 1)));
    }
    const auto clearing{state.ClearSlot()};
    BOOST_REQUIRE(clearing && clearing->cleared);
    BOOST_CHECK_EQUAL(clearing->bid_fill.size(), size_t{BIDS});
    BOOST_CHECK_EQUAL(clearing->imbalance, BIDS / 2);
    const auto start{std::chrono::steady_clock::now()};
    const auto side{flowmesh::ObserveImbalanceSide(*clearing, state)};
    const auto elapsed{std::chrono::steady_clock::now() - start};
    BOOST_CHECK(side == ImbalanceSide::BID);
    BOOST_TEST_MESSAGE("ObserveImbalanceSide over " << BIDS << " fills: "
                       << std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() << " us");
    // Generous for loaded test machines; it is a few hundred microseconds.
    BOOST_CHECK(elapsed < std::chrono::milliseconds{50});
}

BOOST_AUTO_TEST_CASE(book_copy_takes_one_bounded_page_per_read_of_a_large_book)
{
    // 10,000 standing curves: a display view's copy never reads more than one
    // curve-paging page per call (per hold of the caller's lock).
    constexpr uint16_t CURVES{10'000};
    auto state{EmptyState()};
    const auto account = [](const uint16_t index) {
        uint256 out;
        *out.begin() = static_cast<uint8_t>(index & 0xff);
        *(out.begin() + 1) = static_cast<uint8_t>(index >> 8);
        *(out.begin() + 2) = 2;
        return out;
    };
    for (uint16_t i{0}; i < CURVES; ++i) {
        Fund(state, account(i), 1'000'000);
        const auto side{i % 2 ? Side::ASK : Side::BID};
        const CAmount price{static_cast<CAmount>(1 + i % 40)};
        BOOST_REQUIRE(state.SubmitCurve(account(i), side, side == Side::BID ? *flowmesh::MakeLimitBidCurve(price, 1 + i % 7)
                                                                             : *flowmesh::MakeLimitAskCurve(price, 1 + i % 7)));
    }
    const uint256 head{uint256::ONE};
    size_t calls{0}, largest{0};
    const flowmesh::BookPageReader read = [&](const auto& after, const size_t limit) -> std::optional<flowmesh::ClearingEngine::CurvePage> {
        ++calls;
        BOOST_CHECK(limit <= flowmesh::MARKET_DATA_MAX_CURVES);
        auto page{state.ReadCurves(limit, after)};
        largest = std::max(largest, page.curves.size());
        return page;
    };
    size_t pages{0};
    const auto book{flowmesh::CopyBookCurves(head, read, pages)};
    BOOST_CHECK(book.unavailable_reason.empty());
    BOOST_CHECK(book.head == head);
    BOOST_CHECK_EQUAL(pages, calls);
    BOOST_CHECK_EQUAL(pages, (size_t{CURVES} + flowmesh::MARKET_DATA_MAX_CURVES - 1) / flowmesh::MARKET_DATA_MAX_CURVES);
    BOOST_CHECK_EQUAL(largest, flowmesh::MARKET_DATA_MAX_CURVES);
    // Exactly the whole book, in the book's order.
    const auto whole{state.ReadCurves(std::numeric_limits<size_t>::max()).curves};
    BOOST_REQUIRE_EQUAL(book.curves.size(), whole.size());
    for (size_t i{0}; i < whole.size(); ++i) {
        BOOST_CHECK(book.curves[i].account_id == whole[i].account_id);
        BOOST_CHECK(book.curves[i].side == whole[i].side);
        BOOST_CHECK(book.curves[i].filled_quantity == whole[i].filled_quantity);
        BOOST_CHECK_EQUAL(book.curves[i].points.size(), whole[i].points.size());
    }
    // The same indicative as over the whole book at once.
    const auto paged{flowmesh::ComputeIndicative(book.curves, {})};
    const auto direct{flowmesh::ComputeIndicative(whole, {})};
    BOOST_CHECK_EQUAL(paged.available, direct.available);
    BOOST_CHECK_EQUAL(paged.price, direct.price);
    BOOST_CHECK_EQUAL(paged.volume, direct.volume);

    // A book over the bound: one page is read, nothing more is copied.
    calls = 0;
    pages = 0;
    const auto over{flowmesh::CopyBookCurves(head, read, pages, CURVES - 1)};
    BOOST_CHECK_EQUAL(over.unavailable_reason, "work_budget");
    BOOST_CHECK(over.curves.empty());
    BOOST_CHECK_EQUAL(pages, 1U);
    // A head that moves between reads ends the copy.
    calls = 0;
    pages = 0;
    const flowmesh::BookPageReader moving = [&](const auto& after, const size_t limit) -> std::optional<flowmesh::ClearingEngine::CurvePage> {
        if (++calls == 3) return std::nullopt;
        return state.ReadCurves(limit, after);
    };
    const auto moved{flowmesh::CopyBookCurves(head, moving, pages)};
    BOOST_CHECK_EQUAL(moved.unavailable_reason, "head_changed");
    BOOST_CHECK(moved.curves.empty());
    BOOST_CHECK_EQUAL(pages, 3U);

    // A book that was not copied makes the view unavailable for its reason.
    for (const char* reason : {"work_budget", "head_changed"}) {
        flowmesh::MarketData data;
        data.book = std::make_shared<const flowmesh::BookCurves>(flowmesh::BookCurves{.head = head, .curves = {}, .unavailable_reason = reason});
        flowmesh::MarketDataQuery query;
        query.indicative = flowmesh::IndicativeQuery{};
        flowmesh::ComputeMarketViews(data, query);
        BOOST_CHECK(!data.book);
        BOOST_REQUIRE(data.indicative);
        BOOST_CHECK(!data.indicative->available);
        BOOST_CHECK_EQUAL(data.indicative->unavailable_reason, reason);
        BOOST_CHECK(data.indicative->head == head);
    }
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
    const auto empty{flowmesh::PageClearingTicks(std::deque<flowmesh::MarketHistoryEntry>{}, {})};
    BOOST_CHECK(empty.available && empty.ticks.empty() && !empty.oldest_retained_sequence);
}

BOOST_AUTO_TEST_CASE(indicative_clearing_matches_clearslot_on_a_state_copy)
{
    FastRandomContext rng{uint256{43}};
    size_t cleared{0}, rationed_drafts{0};
    for (int iteration{0}; iteration < 400; ++iteration) {
        auto state{EmptyState()};
        const uint8_t accounts{static_cast<uint8_t>(2 + rng.randrange(10))};
        // One more funded account that never has a standing curve.
        for (uint8_t i{0}; i <= accounts; ++i) Fund(state, Account(i));
        const auto submit = [&] {
            for (uint8_t i{0}; i < accounts; ++i) {
                for (const Side side : {Side::BID, Side::ASK}) {
                    if (rng.randrange(3) == 0) continue;
                    BOOST_REQUIRE(state.SubmitCurve(Account(i), side, RandomCurve(rng, side)));
                }
            }
        };
        submit();
        BOOST_REQUIRE(state.ClearSlot());
        if (rng.randbool()) submit();
        const auto curves{state.ReadCurves(std::numeric_limits<size_t>::max()).curves};
        const auto root{state.Root()};

        flowmesh::IndicativeQuery query;
        const int mode{static_cast<int>(rng.randrange(4))};
        // Without a known account the draft is a new curve: the oracle signs
        // it with the account that has none.
        const flowmesh::AccountId signer{Account(mode == 3 ? accounts : static_cast<uint8_t>(rng.randrange(accounts + 1)))};
        if (mode > 0) {
            flowmesh::IndicativeDraft draft;
            draft.side = rng.randbool() ? Side::BID : Side::ASK;
            draft.price = 1 + static_cast<CAmount>(rng.randrange(40));
            draft.quantity = 1 + static_cast<CAmount>(rng.randrange(1000));
            if (mode != 3) draft.account = signer;
            query.draft = draft;
        }
        const auto indicative{flowmesh::ComputeIndicative(curves, query)};
        BOOST_REQUIRE(indicative.available);
        BOOST_CHECK(state.Root() == root);

        // Oracle: the engine itself, on a copy with the draft submitted.
        auto copy{state};
        if (query.draft) {
            const auto& draft{*query.draft};
            const auto points{draft.side == Side::BID ? flowmesh::MakeLimitBidCurve(draft.price, draft.quantity)
                                                      : flowmesh::MakeLimitAskCurve(draft.price, draft.quantity)};
            const bool replaces{!copy.AccountCurves(signer).empty() &&
                                std::any_of(curves.begin(), curves.end(), [&](const auto& curve) {
                                    return curve.account_id == signer && curve.side == draft.side;
                                })};
            BOOST_CHECK_EQUAL(indicative.draft_replaces_curve, draft.account.has_value() && replaces);
            BOOST_REQUIRE(copy.SubmitCurve(signer, draft.side, *points));
        }
        const auto clearing{copy.ClearSlot()};
        BOOST_REQUIRE(clearing);
        BOOST_REQUIRE_EQUAL(indicative.cleared, clearing->cleared);
        if (!clearing->cleared) {
            BOOST_CHECK(indicative.imbalance_side == ImbalanceSide::NONE);
            if (query.draft) BOOST_CHECK_EQUAL(*indicative.draft_fill_max, 0);
            continue;
        }
        ++cleared;
        BOOST_CHECK_EQUAL(indicative.price, clearing->price);
        BOOST_CHECK_EQUAL(indicative.volume, clearing->volume);
        BOOST_CHECK_EQUAL(indicative.imbalance, clearing->imbalance);
        BOOST_CHECK(indicative.imbalance_side == flowmesh::ObserveImbalanceSide(*clearing, copy));
        if (!query.draft) continue;
        const auto& fills{query.draft->side == Side::BID ? clearing->bid_fill : clearing->ask_fill};
        const auto fill{fills.contains(signer) ? fills.at(signer) : CAmount{0}};
        BOOST_REQUIRE(indicative.draft_fill_min && indicative.draft_fill_max);
        BOOST_CHECK(*indicative.draft_fill_min <= fill && fill <= *indicative.draft_fill_max);
        if (query.draft->account) BOOST_CHECK_EQUAL(*indicative.draft_fill_min, *indicative.draft_fill_max);
        BOOST_CHECK(*indicative.draft_fill_max - *indicative.draft_fill_min <= 1);
        rationed_drafts += (query.draft->side == Side::BID ? indicative.imbalance_side == ImbalanceSide::BID
                                                           : indicative.imbalance_side == ImbalanceSide::ASK) && fill > 0;
    }
    BOOST_CHECK(cleared > 100 && rationed_drafts > 10);
}

BOOST_AUTO_TEST_CASE(indicative_best_prices_match_a_brute_force_scan)
{
    FastRandomContext rng{uint256{47}};
    for (int iteration{0}; iteration < 200; ++iteration) {
        auto state{EmptyState()};
        const uint8_t accounts{static_cast<uint8_t>(1 + rng.randrange(8))};
        for (uint8_t i{0}; i < accounts; ++i) {
            Fund(state, Account(i));
            for (const Side side : {Side::BID, Side::ASK}) {
                if (rng.randbool()) BOOST_REQUIRE(state.SubmitCurve(Account(i), side, RandomCurve(rng, side)));
            }
        }
        if (rng.randbool()) BOOST_REQUIRE(state.ClearSlot());
        const auto indicative{flowmesh::ComputeIndicative(state.ReadCurves(std::numeric_limits<size_t>::max()).curves, {})};
        BOOST_REQUIRE(indicative.available);
        std::optional<CAmount> best_bid, best_ask;
        for (CAmount price{0}; price < 200; ++price) {
            if (SideTotal(state, Side::BID, price) > 0) best_bid = price;
            if (!best_ask && SideTotal(state, Side::ASK, price) > 0) best_ask = price;
        }
        BOOST_CHECK(indicative.best_bid == best_bid);
        BOOST_CHECK(indicative.best_ask == best_ask);
    }
}

BOOST_AUTO_TEST_CASE(indicative_draft_replaces_its_accounts_curve_and_reports_its_fill)
{
    auto state{EmptyState()};
    for (uint8_t i{0}; i < 3; ++i) Fund(state, Account(i));
    BOOST_REQUIRE(state.SubmitCurve(Account(0), Side::BID, *flowmesh::MakeLimitBidCurve(6, 10)));
    BOOST_REQUIRE(state.SubmitCurve(Account(1), Side::ASK, *flowmesh::MakeLimitAskCurve(6, 5)));
    // The standing book crosses: 5 at 6, bids rationed.
    const auto curves{state.ReadCurves(8).curves};
    const auto standing{flowmesh::ComputeIndicative(curves, {})};
    BOOST_REQUIRE(standing.available && standing.cleared);
    BOOST_CHECK_EQUAL(standing.price, 6);
    BOOST_CHECK_EQUAL(standing.volume, 5);
    BOOST_CHECK(standing.imbalance_side == ImbalanceSide::BID);
    BOOST_CHECK(!standing.draft && !standing.draft_fill_min);
    BOOST_CHECK(standing.best_bid == CAmount{6} && standing.best_ask == CAmount{6});

    // Account 0's own bid of 3 at 7 replaces its 10 at 6: asks are long.
    flowmesh::IndicativeQuery replace;
    replace.draft = flowmesh::IndicativeDraft{Side::BID, 7, 3, Account(0)};
    const auto replaced{flowmesh::ComputeIndicative(curves, replace)};
    BOOST_REQUIRE(replaced.available && replaced.cleared);
    BOOST_CHECK(replaced.draft_replaces_curve);
    BOOST_CHECK(replaced.draft == replace.draft);
    BOOST_CHECK_EQUAL(replaced.price, 6);
    BOOST_CHECK_EQUAL(replaced.volume, 3);
    BOOST_CHECK_EQUAL(replaced.imbalance, 2);
    BOOST_CHECK(replaced.imbalance_side == ImbalanceSide::ASK);
    BOOST_CHECK(replaced.draft_fill_min == CAmount{3} && replaced.draft_fill_max == CAmount{3});
    // The best prices stay those of the verified book.
    BOOST_CHECK(replaced.best_bid == CAmount{6});

    // Another account's same draft joins the rationed bids: 13 wanted, 5
    // traded; largest remainders give it 1.
    flowmesh::IndicativeQuery join;
    join.draft = flowmesh::IndicativeDraft{Side::BID, 7, 3, Account(2)};
    const auto joined{flowmesh::ComputeIndicative(curves, join)};
    BOOST_REQUIRE(joined.available && joined.cleared);
    BOOST_CHECK(!joined.draft_replaces_curve);
    BOOST_CHECK_EQUAL(joined.volume, 5);
    BOOST_CHECK_EQUAL(joined.imbalance, 8);
    BOOST_CHECK(joined.draft_fill_min == CAmount{1} && joined.draft_fill_max == CAmount{1});
    // Without the signing account its place in account order is unknown.
    join.draft->account.reset();
    const auto unknown{flowmesh::ComputeIndicative(curves, join)};
    BOOST_CHECK(unknown.draft_fill_min == CAmount{1} && unknown.draft_fill_max == CAmount{2});
}

BOOST_AUTO_TEST_CASE(indicative_fails_closed_on_budget_malformed_curves_and_invalid_drafts)
{
    auto state{EmptyState()};
    for (uint8_t i{0}; i < 2; ++i) Fund(state, Account(i));
    BOOST_REQUIRE(state.SubmitCurve(Account(0), Side::BID, {{1, 50}, {5, 30}, {9, 0}}));
    BOOST_REQUIRE(state.SubmitCurve(Account(1), Side::ASK, {{2, 0}, {3, 10}, {8, 40}}));
    const auto curves{state.ReadCurves(8).curves};
    BOOST_CHECK(flowmesh::ComputeIndicative(curves, {}).available);
    for (const size_t budget : {size_t{0}, size_t{4}, size_t{9}}) {
        const auto starved{flowmesh::ComputeIndicative(curves, {}, budget)};
        BOOST_CHECK(!starved.available && !starved.cleared);
        BOOST_CHECK_EQUAL(starved.unavailable_reason, "work_budget");
    }

    auto malformed{curves};
    std::swap(malformed.front().points.front(), malformed.front().points.back());
    const auto refused{flowmesh::ComputeIndicative(malformed, {})};
    BOOST_CHECK(!refused.available);
    BOOST_CHECK_EQUAL(refused.unavailable_reason, "malformed_curve");

    for (const auto& [price, quantity] : std::vector<std::pair<CAmount, CAmount>>{{-1, 5}, {5, 0}, {MAX_MONEY, 1}, {5, MAX_MONEY + 1}}) {
        flowmesh::IndicativeQuery query;
        query.draft = flowmesh::IndicativeDraft{Side::BID, price, quantity, Account(0)};
        const auto invalid{flowmesh::ComputeIndicative(curves, query)};
        BOOST_CHECK(!invalid.available);
        BOOST_CHECK_EQUAL(invalid.unavailable_reason, "invalid_draft");
        BOOST_CHECK(invalid.draft == query.draft);
    }
    // An empty book: available, nothing to clear.
    const auto empty{flowmesh::ComputeIndicative({}, {})};
    BOOST_CHECK(empty.available && !empty.cleared && !empty.best_bid && !empty.best_ask);
}

BOOST_AUTO_TEST_CASE(book_levels_match_an_integer_scan_in_every_grouping)
{
    FastRandomContext rng{uint256{53}};
    // Exact; canonical steps of 1, 2, 5, 10 and 50 ticks (d = 9); inverse
    // u = 10^3 / P (d = 6, h = 1), 10^4 / (5 P) and 10^2 / (2 P).
    const std::vector<flowmesh::LevelGrouping> groupings{
        flowmesh::LevelGrouping{}, Grouping(false, 1, 0, 9), Grouping(false, 2, 0, 9), Grouping(false, 5, 0, 9),
        Grouping(false, 1, 1, 9), Grouping(false, 5, 1, 9), Grouping(true, 1, 0, 6), Grouping(true, 5, -1, 6),
        Grouping(true, 2, 1, 6)};
    size_t levels_checked{0}, truncated_by_depth{0}, ladders{0}, notionals{0};
    for (int iteration{0}; iteration < 160; ++iteration) {
        auto state{EmptyState()};
        const uint8_t accounts{static_cast<uint8_t>(1 + rng.randrange(10))};
        for (uint8_t i{0}; i < accounts; ++i) Fund(state, Account(i));
        const auto submit = [&] {
            for (uint8_t i{0}; i < accounts; ++i) {
                for (const Side side : {Side::BID, Side::ASK}) {
                    if (rng.randrange(3) == 0) continue;
                    const auto curve{AnyCurve(rng, side)};
                    ladders += curve.size() > 2;
                    BOOST_REQUIRE(state.SubmitCurve(Account(i), side, curve));
                }
            }
        };
        // Random fills: a clearing leaves partially filled curves behind.
        submit();
        BOOST_REQUIRE(state.ClearSlot());
        if (rng.randbool()) submit();
        const auto curves{state.ReadCurves(std::numeric_limits<size_t>::max()).curves};
        const auto root{state.Root()};
        const flowmesh::AccountId own{Account(static_cast<uint8_t>(rng.randrange(accounts)))};
        // Every level lies at or below the highest breakpoint.
        CAmount top{0};
        for (const auto& curve : curves) top = std::max(top, curve.points.back().price + 1);
        std::optional<CAmount> best_bid, best_ask;
        for (CAmount price{0}; price <= top; ++price) {
            if (SideTotal(state, Side::BID, price) > 0) best_bid = price;
            if (!best_ask && SideTotal(state, Side::ASK, price) > 0) best_ask = price;
        }
        for (const auto& grouping : groupings) {
            flowmesh::BookLevelsQuery query;
            query.grouping = grouping;
            query.depth = 1 + rng.randrange(flowmesh::MARKET_DATA_MAX_LEVELS);
            query.account = own;
            const auto levels{flowmesh::ComputeBookLevels(curves, query)};
            BOOST_REQUIRE(levels.available);
            BOOST_CHECK(levels.grouping == grouping);
            BOOST_CHECK_EQUAL(levels.depth, query.depth);
            BOOST_CHECK_EQUAL(levels.total_curves, curves.size());
            for (const Side side : {Side::BID, Side::ASK}) {
                const auto& got{side == Side::BID ? levels.bids : levels.asks};
                BOOST_REQUIRE(got.available);
                BOOST_CHECK(got.truncated_reason.empty());
                const auto expected{BruteLevels(curves, side, grouping, own, top)};
                BOOST_REQUIRE_EQUAL(got.levels.size(), std::min(expected.size(), query.depth));
                BOOST_CHECK_EQUAL(got.more, expected.size() > query.depth);
                truncated_by_depth += got.more;
                U128 cumulative{0};
                for (size_t i{0}; i < got.levels.size(); ++i) {
                    const auto& level{got.levels[i]};
                    const auto& brute{expected[i]};
                    cumulative += brute.size;
                    BOOST_CHECK(level.bucket == brute.bucket);
                    BOOST_CHECK_EQUAL(level.zero_price, brute.zero_price);
                    BOOST_CHECK(static_cast<U128>(level.size) == brute.size);
                    BOOST_CHECK(static_cast<U128>(level.cumulative) == cumulative);
                    BOOST_CHECK_EQUAL(level.curves, brute.accounts.size());
                    BOOST_CHECK_EQUAL(level.own, brute.own);
                    // Exact notional unless a curve has more prices in the
                    // level than the bounded walk reads.
                    const bool walked{std::all_of(brute.prices.begin(), brute.prices.end(), [](const auto& item) {
                        return item.second <= flowmesh::MARKET_VIEW_LEVEL_NOTIONAL_PRICES;
                    })};
                    BOOST_CHECK_EQUAL(level.notional.has_value(), walked);
                    if (level.notional) BOOST_CHECK(*level.notional == brute.notional);
                    notionals += walked;
                    BOOST_CHECK(level.low <= brute.any_price && brute.any_price <= level.high);
                    // Cumulative is the side's total at the level's far edge.
                    const CAmount edge{side == Side::BID ? level.low : level.high};
                    BOOST_CHECK(static_cast<util::Signed128>(level.cumulative) == SideTotal(state, side, edge));
                    ++levels_checked;
                }
            }
            // Best prices are exact, whatever the grouping.
            BOOST_CHECK(levels.best_bid == best_bid);
            BOOST_CHECK(levels.best_ask == best_ask);
            // The standing clearing is the indicative without a draft.
            BOOST_CHECK(levels.standing == flowmesh::ComputeIndicative(curves, {}));
        }
        BOOST_CHECK(state.Root() == root);
    }
    BOOST_CHECK(levels_checked > 5'000 && truncated_by_depth > 50 && ladders > 100 && notionals > levels_checked / 2);
    BOOST_TEST_MESSAGE("book levels checked: " << levels_checked << ", with notional " << notionals << ", depth-limited sides " << truncated_by_depth);
}

BOOST_AUTO_TEST_CASE(book_levels_canonical_step_is_m_times_ten_to_e_plus_nine_minus_d)
{
    // A price P shows as P x 10^(d - 9) B3 per token. With d = 6 a step of
    // 0.01 B3 per token is 10 ticks (m x 10^(e + 9 - d)), never 10^-5 (the
    // sign-inverted e + d - 9); with d = 9 a tick is 1 B3 per token.
    BOOST_CHECK(flowmesh::LevelGroupingError(Grouping(false, 1, -2, 6)).empty());
    auto state{EmptyState()};
    for (uint8_t i{0}; i < 3; ++i) Fund(state, Account(i));
    BOOST_REQUIRE(state.SubmitCurve(Account(0), Side::BID, *flowmesh::MakeLimitBidCurve(1234, 7)));
    BOOST_REQUIRE(state.SubmitCurve(Account(1), Side::BID, *flowmesh::MakeLimitBidCurve(1239, 5)));
    BOOST_REQUIRE(state.SubmitCurve(Account(2), Side::ASK, *flowmesh::MakeLimitAskCurve(1241, 3)));
    const auto curves{state.ReadCurves(8).curves};
    flowmesh::BookLevelsQuery query;
    query.grouping = Grouping(false, 1, -2, 6);
    const auto levels{flowmesh::ComputeBookLevels(curves, query)};
    BOOST_REQUIRE(levels.available);
    BOOST_REQUIRE_EQUAL(levels.bids.levels.size(), 1U);
    // 1.234 and 1.239 B3 per token buy at 1.23: bucket 123, ticks 1230-1239.
    BOOST_CHECK(levels.bids.levels[0].bucket == 123);
    BOOST_CHECK_EQUAL(levels.bids.levels[0].low, 1230);
    BOOST_CHECK_EQUAL(levels.bids.levels[0].high, 1239);
    BOOST_CHECK_EQUAL(levels.bids.levels[0].size, 12);
    BOOST_CHECK_EQUAL(levels.bids.levels[0].curves, 2U);
    // 1.241 sells at 1.25: bucket 125, ticks 1241-1250.
    BOOST_REQUIRE_EQUAL(levels.asks.levels.size(), 1U);
    BOOST_CHECK(levels.asks.levels[0].bucket == 125);
    BOOST_CHECK_EQUAL(levels.asks.levels[0].low, 1241);
    BOOST_CHECK_EQUAL(levels.asks.levels[0].high, 1250);
    // Every label is a price every lot of its level accepts.
    BOOST_CHECK(levels.bids.levels[0].low <= *levels.best_bid && levels.asks.levels[0].high >= *levels.best_ask);
    BOOST_CHECK(!flowmesh::LevelGroupingError(Grouping(false, 1, -2, 9)).empty());
    // 10 B3 per token with d = 6: 10^4 ticks, one bucket from 0 to 9999.
    query.grouping = Grouping(false, 1, 1, 6);
    const auto coarse{flowmesh::ComputeBookLevels(curves, query)};
    BOOST_REQUIRE_EQUAL(coarse.bids.levels.size(), 1U);
    BOOST_CHECK_EQUAL(coarse.bids.levels[0].low, 0);
    BOOST_CHECK_EQUAL(coarse.bids.levels[0].high, 9'999);
    BOOST_CHECK(coarse.asks.levels[0].bucket == 1 && coarse.asks.levels[0].high == 10'000);

    // Valid ranges: e + 9 - d in 0..18 with the step within the monetary
    // range (canonical), 9 - d - e in 0..36 (inverse), m in {1, 2, 5}.
    BOOST_CHECK(!flowmesh::LevelGroupingError(Grouping(false, 1, -4, 6)).empty()); // 10^-1 ticks
    BOOST_CHECK(flowmesh::LevelGroupingError(Grouping(false, 1, -3, 6)).empty());  // 1 tick
    BOOST_CHECK(flowmesh::LevelGroupingError(Grouping(false, 5, 17, 9)).empty());  // 5 x 10^17 ticks
    BOOST_CHECK(!flowmesh::LevelGroupingError(Grouping(false, 1, 18, 9)).empty()); // over MAX_MONEY
    BOOST_CHECK(!flowmesh::LevelGroupingError(Grouping(false, 3, 0, 9)).empty());
    BOOST_CHECK(!flowmesh::LevelGroupingError(Grouping(false, 1, 0, 19)).empty());
    BOOST_CHECK(flowmesh::LevelGroupingError(Grouping(true, 1, 3, 6)).empty());    // s = 0
    BOOST_CHECK(!flowmesh::LevelGroupingError(Grouping(true, 1, 4, 6)).empty());   // s = -1
    BOOST_CHECK(flowmesh::LevelGroupingError(Grouping(true, 2, -33, 6)).empty());  // s = 36
    BOOST_CHECK(!flowmesh::LevelGroupingError(Grouping(true, 2, -34, 6)).empty()); // s = 37
    BOOST_CHECK(!flowmesh::LevelGroupingError(Grouping(true, 1, std::numeric_limits<int32_t>::min(), 6)).empty());
    query.grouping = Grouping(true, 1, 4, 6);
    const auto refused{flowmesh::ComputeBookLevels(curves, query)};
    BOOST_CHECK(!refused.available);
    BOOST_CHECK_EQUAL(refused.unavailable_reason, "invalid_grouping");
    for (const size_t depth : {size_t{0}, flowmesh::MARKET_DATA_MAX_LEVELS + 1}) {
        flowmesh::BookLevelsQuery bad;
        bad.depth = depth;
        BOOST_CHECK(!flowmesh::LevelsQueryValid(bad));
    }
}

BOOST_AUTO_TEST_CASE(book_levels_inverse_partition_is_exact_contiguous_and_conservative)
{
    const auto pow10 = [](int n) { U128 out{1}; while (n-- > 0) out *= 10; return out; };
    for (const auto& [mantissa, exponent, decimals] : std::vector<std::tuple<uint8_t, int32_t, uint8_t>>{
             {1, -2, 6}, {5, -3, 6}, {2, -4, 9}, {1, 0, 9}, {2, 1, 6}, {5, 0, 3}}) {
        const auto grouping{Grouping(true, mantissa, exponent, decimals)};
        BOOST_REQUIRE(flowmesh::LevelGroupingError(grouping).empty());
        const U128 scale{pow10(9 - decimals - exponent)};
        // One unit of one account at each price, one side at a time: every
        // level then holds exactly its prices.
        for (const Side side : {Side::BID, Side::ASK}) {
            std::vector<flowmesh::ClearingEngine::CurveView> curves;
            const CAmount top{std::min<CAmount>(static_cast<CAmount>(scale) + 3, 2'500)};
            for (CAmount price{1}; price <= top; ++price) {
                uint256 account;
                *account.begin() = static_cast<uint8_t>(price & 0xff);
                *(account.begin() + 1) = static_cast<uint8_t>(price >> 8);
                const auto points{side == Side::BID ? *flowmesh::MakeLimitBidCurve(price, 1) : *flowmesh::MakeLimitAskCurve(price, 1)};
                curves.push_back({account, side, points, 0, 1, 0});
            }
            flowmesh::BookLevelsQuery query;
            query.grouping = grouping;
            query.depth = flowmesh::MARKET_DATA_MAX_LEVELS;
            const auto levels{flowmesh::ComputeBookLevels(curves, query, 1 << 24)};
            BOOST_REQUIRE(levels.available);
            const auto& got{side == Side::BID ? levels.bids : levels.asks};
            BOOST_REQUIRE(!got.levels.empty());
            std::optional<CAmount> previous_edge;
            for (const auto& level : got.levels) {
                BOOST_CHECK(level.low <= level.high);
                // Contiguous and disjoint: each level starts where the last ended.
                if (previous_edge) BOOST_CHECK_EQUAL(side == Side::BID ? level.high : level.low, *previous_edge);
                previous_edge = side == Side::BID ? level.low - 1 : level.high + 1;
                // Exactly the prices of the range that exist in the book.
                const CAmount first{std::max<CAmount>(level.low, 1)}, last{std::min(level.high, top)};
                BOOST_CHECK_EQUAL(level.size, last - first + 1);
                BOOST_CHECK_EQUAL(level.curves, static_cast<uint32_t>(last - first + 1));
                for (CAmount price{first}; price <= last; ++price) {
                    BOOST_CHECK(BruteBucket(side, price, grouping).first == level.bucket);
                    // Conservative labels: a displayed sell (canonical bid) is
                    // labelled at or above u, a displayed buy at or below it.
                    const U128 unit{mantissa * static_cast<U128>(price)};
                    if (side == Side::BID) BOOST_CHECK(level.bucket * unit >= scale);
                    else BOOST_CHECK(level.bucket * unit <= scale);
                }
                // Just outside the range, the bucket differs.
                if (level.low > 1) BOOST_CHECK(BruteBucket(side, level.low - 1, grouping).first != level.bucket);
                if (level.high < top) BOOST_CHECK(BruteBucket(side, level.high + 1, grouping).first != level.bucket);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(book_levels_decompose_ladders_slopes_zero_prices_and_fills)
{
    auto state{EmptyState()};
    for (uint8_t i{0}; i < 6; ++i) Fund(state, Account(i));
    // A four-step ladder per side, eight breakpoints each.
    const auto bid_ladder{Ladder(Side::BID, {{100, 10}, {103, 20}, {104, 5}, {110, 30}})};
    const auto ask_ladder{Ladder(Side::ASK, {{120, 7}, {125, 8}, {126, 9}, {140, 11}})};
    BOOST_CHECK_EQUAL(bid_ladder.size(), 7U); // 103 and 104 share a point
    BOOST_CHECK(ask_ladder.size() <= 8U);
    BOOST_REQUIRE(state.SubmitCurve(Account(0), Side::BID, bid_ladder));
    BOOST_REQUIRE(state.SubmitCurve(Account(1), Side::ASK, ask_ladder));
    // A sloped bid: E(190) = 10 + floor(-9) = 1 and E(199) = 0.
    BOOST_REQUIRE(state.SubmitCurve(Account(2), Side::BID, {{100, 10}, {200, 0}}));
    BOOST_CHECK_EQUAL(flowmesh::ClearingEngine::EvaluateCurve({{100, 10}, {200, 0}}, 190), 1);
    BOOST_CHECK_EQUAL(flowmesh::ClearingEngine::EvaluateCurve({{100, 10}, {200, 0}}, 199), 0);
    // An ask whose first quantity is positive sells at any price down to 0.
    BOOST_REQUIRE(state.SubmitCurve(Account(3), Side::ASK, {{150, 4}, {160, 6}}));
    const auto curves{state.ReadCurves(8).curves};

    flowmesh::BookLevelsQuery query;
    query.depth = flowmesh::MARKET_DATA_MAX_LEVELS;
    query.account = Account(0);
    const auto exact{flowmesh::ComputeBookLevels(curves, query)};
    BOOST_REQUIRE(exact.available);
    // The ladder's steps appear as its own levels, the slope's lots at 190
    // (190 has 1 lot: E(190) - E(191) = 1 - 0 with E(191) = 10 + floor(-9.1)).
    const auto find = [](const flowmesh::BookLevelsSide& side, const CAmount price) -> const flowmesh::BookLevel* {
        for (const auto& level : side.levels) if (level.low == price) return &level;
        return nullptr;
    };
    // The slope has one lot at each of 100, 110, ..., 190.
    for (const auto& [price, size] : std::vector<std::pair<CAmount, CAmount>>{{110, 30}, {104, 5}, {103, 20}, {100, 10}}) {
        const auto* level{find(exact.bids, price)};
        BOOST_REQUIRE(level);
        BOOST_CHECK_EQUAL(level->own, size);
        BOOST_CHECK_EQUAL(level->size, size + (price % 10 == 0 ? 1 : 0));
        BOOST_CHECK_EQUAL(level->curves, price % 10 == 0 ? 2U : 1U);
    }
    for (CAmount price{120}; price <= 190; price += 10) BOOST_CHECK(find(exact.bids, price) && find(exact.bids, price)->size == 1);
    BOOST_CHECK_EQUAL(exact.bids.levels.size(), 12U);
    BOOST_CHECK(exact.best_bid == CAmount{190});
    // Asks: 4 at price 0 first, then the ladder and the slope's rise.
    BOOST_CHECK_EQUAL(exact.asks.levels.front().low, 0);
    BOOST_CHECK_EQUAL(exact.asks.levels.front().size, 4);
    BOOST_CHECK(exact.best_ask == CAmount{0});

    // Canonical groups of 50 ticks (d = 9, h = 5 x 10^1): the slope's bucket
    // at 100 holds D(100) - D(150) of it = 10 - 5, and 150 the other 5.
    query.grouping = Grouping(false, 5, 1, 9);
    query.account.reset();
    const auto grouped{flowmesh::ComputeBookLevels(curves, query)};
    BOOST_REQUIRE(grouped.available);
    BOOST_REQUIRE_EQUAL(grouped.bids.levels.size(), 2U);
    BOOST_CHECK(grouped.bids.levels[0].bucket == 3 && grouped.bids.levels[0].size == 5);
    BOOST_CHECK(grouped.bids.levels[1].bucket == 2 && grouped.bids.levels[1].size == 5 + 65);
    BOOST_CHECK_EQUAL(grouped.bids.levels[1].curves, 2U);
    // The ladder's notional is its exact quantity x price per step, the
    // slope's 100 + 110 + ... + 140.
    const auto ladder_notional{U128{10} * 100 + U128{20} * 103 + U128{5} * 104 + U128{30} * 110};
    BOOST_REQUIRE(grouped.bids.levels[1].notional);
    BOOST_CHECK(*grouped.bids.levels[1].notional == ladder_notional + 100 + 110 + 120 + 130 + 140);
    BOOST_REQUIRE(grouped.bids.levels[0].notional);
    BOOST_CHECK(*grouped.bids.levels[0].notional == 150 + 160 + 170 + 180 + 190);

    // One wide bucket holding a slope with a lot at each of 100 prices:
    // more than the bounded walk reads, so the level has no notional; its
    // size stays exact.
    std::vector<flowmesh::ClearingEngine::CurveView> steep{{Account(9), Side::BID, {{0, 100}, {100, 0}}, 0, 100, 0}};
    query.grouping = Grouping(false, 5, 2, 9);
    const auto wide{flowmesh::ComputeBookLevels(steep, query)};
    BOOST_REQUIRE_EQUAL(wide.bids.levels.size(), 1U);
    BOOST_CHECK_EQUAL(wide.bids.levels[0].size, 100);
    BOOST_CHECK(!wide.bids.levels[0].notional);
    query.grouping = {};
    BOOST_CHECK_EQUAL(flowmesh::ComputeBookLevels(steep, query).bids.levels.size(), flowmesh::MARKET_DATA_MAX_LEVELS);

    // The inverse view: the zero-price ask is its own first level, with no
    // finite token-per-B3 price.
    query.grouping = Grouping(true, 1, -2, 6);
    const auto inverse{flowmesh::ComputeBookLevels(curves, query)};
    BOOST_REQUIRE(inverse.available);
    BOOST_CHECK(inverse.asks.levels.front().zero_price);
    BOOST_CHECK_EQUAL(inverse.asks.levels.front().size, 4);

    // Fills remove the best-priced lots first: a fill of 35 empties the
    // ladder's 110 (30) and 104 (5) steps and leaves 103 and 100 whole.
    auto filled{curves};
    BOOST_REQUIRE(filled[0].account_id == Account(0) && filled[0].side == Side::BID);
    filled[0].filled_quantity = 35;
    query.grouping = {};
    query.account = Account(0);
    const auto after{flowmesh::ComputeBookLevels(filled, query)};
    BOOST_REQUIRE(find(after.bids, 110));
    BOOST_CHECK_EQUAL(find(after.bids, 110)->own, 0); // only the slope's lot
    BOOST_CHECK(!find(after.bids, 104));
    BOOST_CHECK_EQUAL(find(after.bids, 103)->own, 20);
    BOOST_CHECK_EQUAL(find(after.bids, 100)->own, 10);
    BOOST_CHECK_EQUAL(after.bids.levels.back().cumulative, 65 - 35 + 10);
}

BOOST_AUTO_TEST_CASE(book_levels_show_a_crossed_residual_after_a_prorata_clearing)
{
    // Bids A@10 x5 and B@12 x5, asks C@9 x7 and E@11 x5. The auction clears
    // 7 at 9 (volume 7 at 9 and 10, the lower price wins) and rations A and
    // B: the residual book is crossed, 12 above 11. The ids are pinned
    // (Allocate gives a tie to the lower id), and the standing clearing is
    // compared with ClearSlot on a copy rather than assumed.
    auto state{EmptyState()};
    for (uint8_t i{0}; i < 4; ++i) Fund(state, Account(i));
    BOOST_REQUIRE(state.SubmitCurve(Account(0), Side::BID, *flowmesh::MakeLimitBidCurve(10, 5)));
    BOOST_REQUIRE(state.SubmitCurve(Account(1), Side::BID, *flowmesh::MakeLimitBidCurve(12, 5)));
    BOOST_REQUIRE(state.SubmitCurve(Account(2), Side::ASK, *flowmesh::MakeLimitAskCurve(9, 7)));
    BOOST_REQUIRE(state.SubmitCurve(Account(3), Side::ASK, *flowmesh::MakeLimitAskCurve(11, 5)));
    const auto first{state.ClearSlot()};
    BOOST_REQUIRE(first && first->cleared);
    BOOST_CHECK_EQUAL(first->price, 9);
    BOOST_CHECK_EQUAL(first->volume, 7);
    const auto curves{state.ReadCurves(8).curves};
    const auto levels{flowmesh::ComputeBookLevels(curves, {})};
    BOOST_REQUIRE(levels.available);
    BOOST_CHECK(levels.best_bid == CAmount{12});
    BOOST_CHECK(levels.best_ask == CAmount{11});
    BOOST_CHECK(*levels.best_bid > *levels.best_ask);
    auto copy{state};
    const auto next{copy.ClearSlot()};
    BOOST_REQUIRE(next && next->cleared);
    BOOST_REQUIRE(levels.standing.available && levels.standing.cleared);
    BOOST_CHECK_EQUAL(levels.standing.price, next->price);
    BOOST_CHECK_EQUAL(levels.standing.volume, next->volume);
    BOOST_CHECK_EQUAL(levels.standing.imbalance, next->imbalance);
    BOOST_CHECK_EQUAL(next->price, 11);
    BOOST_CHECK(!levels.standing.draft);
}

BOOST_AUTO_TEST_CASE(book_levels_fail_closed_or_stop_as_an_exact_prefix)
{
    auto state{EmptyState()};
    for (uint8_t i{0}; i < 20; ++i) {
        Fund(state, Account(i));
        BOOST_REQUIRE(state.SubmitCurve(Account(i), Side::BID, {{static_cast<CAmount>(i), 50}, {static_cast<CAmount>(i) + 300, 0}}));
        BOOST_REQUIRE(state.SubmitCurve(Account(i), Side::ASK, *flowmesh::MakeLimitAskCurve(400 + i, 3)));
    }
    const auto curves{state.ReadCurves(64).curves};
    flowmesh::BookLevelsQuery query;
    query.depth = flowmesh::MARKET_DATA_MAX_LEVELS;
    const auto full{flowmesh::ComputeBookLevels(curves, query)};
    BOOST_REQUIRE(full.available && full.bids.more);
    // Not even the first pass fits: no levels at all.
    for (const size_t budget : {size_t{0}, size_t{50}, size_t{200}}) {
        const auto starved{flowmesh::ComputeBookLevels(curves, query, budget)};
        BOOST_CHECK(!starved.available);
        BOOST_CHECK_EQUAL(starved.unavailable_reason, "work_budget");
        BOOST_CHECK(starved.bids.levels.empty() && starved.asks.levels.empty());
    }
    // Anything in between is an exact prefix of the full levels.
    size_t prefixes{0};
    for (size_t budget{300}; budget < 20'000; budget += 97) {
        const auto partial{flowmesh::ComputeBookLevels(curves, query, budget)};
        if (!partial.available) continue;
        for (const auto* pair : {&partial.bids, &partial.asks}) {
            const auto& whole{pair == &partial.bids ? full.bids : full.asks};
            if (!pair->available) {
                BOOST_CHECK_EQUAL(pair->unavailable_reason, "work_budget");
                continue;
            }
            BOOST_REQUIRE(pair->levels.size() <= whole.levels.size());
            for (size_t i{0}; i < pair->levels.size(); ++i) BOOST_CHECK(pair->levels[i] == whole.levels[i]);
            if (pair->levels.size() < whole.levels.size()) {
                BOOST_CHECK_EQUAL(pair->truncated_reason, "work_budget");
                ++prefixes;
            }
        }
    }
    BOOST_CHECK(prefixes > 10);

    // A malformed curve (never in verified state) makes the view unavailable.
    auto malformed{curves};
    std::swap(malformed.front().points.front(), malformed.front().points.back());
    const auto refused{flowmesh::ComputeBookLevels(malformed, query)};
    BOOST_CHECK(!refused.available);
    BOOST_CHECK_EQUAL(refused.unavailable_reason, "malformed_curve");

    // Sizes beyond MAX_MONEY stop the side, never saturate: two MAX_MONEY
    // bids at one price have no level; at two prices, only the first.
    std::vector<flowmesh::ClearingEngine::CurveView> huge;
    for (uint8_t i{0}; i < 2; ++i) huge.push_back({Account(i), Side::BID, *flowmesh::MakeLimitBidCurve(5, MAX_MONEY), 0, MAX_MONEY, 0});
    const auto same{flowmesh::ComputeBookLevels(huge, query)};
    BOOST_REQUIRE(same.available);
    BOOST_CHECK(!same.bids.available);
    BOOST_CHECK_EQUAL(same.bids.unavailable_reason, "overflow");
    huge[1].points = *flowmesh::MakeLimitBidCurve(4, MAX_MONEY);
    const auto apart{flowmesh::ComputeBookLevels(huge, query)};
    BOOST_REQUIRE(apart.bids.available);
    BOOST_REQUIRE_EQUAL(apart.bids.levels.size(), 1U);
    BOOST_CHECK_EQUAL(apart.bids.levels[0].size, MAX_MONEY);
    BOOST_CHECK_EQUAL(apart.bids.truncated_reason, "overflow");
    // An empty book: available, no levels.
    const auto empty{flowmesh::ComputeBookLevels({}, query)};
    BOOST_CHECK(empty.available && empty.bids.levels.empty() && empty.asks.levels.empty() && !empty.bids.more);
}

BOOST_AUTO_TEST_CASE(book_levels_of_a_full_limit_book_fit_the_budget)
{
    // 16384 limit curves (the display copy's bound): the first pass and 50
    // levels a side stay well within the budget. Logged, not a timing claim.
    std::vector<flowmesh::ClearingEngine::CurveView> curves;
    for (uint32_t i{0}; i < flowmesh::MARKET_VIEW_MAX_BOOK_CURVES; ++i) {
        uint256 account;
        std::memcpy(account.begin(), &i, sizeof(i));
        const bool bid{i % 2 == 0};
        const CAmount price{bid ? 1000 - static_cast<CAmount>((i / 2) % 500) : 1001 + static_cast<CAmount>((i / 2) % 500)};
        curves.push_back({account, bid ? Side::BID : Side::ASK,
                          bid ? *flowmesh::MakeLimitBidCurve(price, 1 + i % 9) : *flowmesh::MakeLimitAskCurve(price, 1 + i % 9), 0, 1 + i % 9, 0});
    }
    flowmesh::BookLevelsQuery query;
    query.depth = flowmesh::MARKET_DATA_MAX_LEVELS;
    const auto started{std::chrono::steady_clock::now()};
    const auto levels{flowmesh::ComputeBookLevels(curves, query)};
    const auto elapsed{std::chrono::steady_clock::now() - started};
    BOOST_REQUIRE(levels.available);
    BOOST_CHECK_EQUAL(levels.bids.levels.size(), flowmesh::MARKET_DATA_MAX_LEVELS);
    BOOST_CHECK(levels.bids.truncated_reason.empty() && levels.asks.truncated_reason.empty());
    BOOST_CHECK(levels.best_bid == CAmount{1000} && levels.best_ask == CAmount{1001});
    BOOST_TEST_MESSAGE("book levels of 16384 limit curves: " << std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() << " us");
}

BOOST_AUTO_TEST_CASE(market_views_compute_levels_once_per_copy_and_only_when_wanted)
{
    // A copy of 300 standing curves, as the local engine's paged copy or the
    // remote client's verified state gives it: every curve counts, not only
    // the first 128-curve page.
    auto state{EmptyState()};
    std::vector<flowmesh::AccountId> accounts;
    for (uint16_t i{0}; i < 300; ++i) {
        uint256 account;
        *account.begin() = static_cast<uint8_t>(i & 0xff);
        *(account.begin() + 1) = static_cast<uint8_t>(i >> 8);
        *(account.begin() + 2) = 7;
        Fund(state, account);
        const bool bid{i < 150};
        BOOST_REQUIRE(state.SubmitCurve(account, bid ? Side::BID : Side::ASK,
                                        bid ? *flowmesh::MakeLimitBidCurve(1000 - i % 25, 1 + i % 4) : *flowmesh::MakeLimitAskCurve(1001 + i % 25, 2)));
    }
    const uint256 head{uint256::ONE};
    auto book{std::make_shared<flowmesh::BookCurves>(flowmesh::BookCurves{.head = head, .curves = state.ReadCurves(1000).curves})};
    BOOST_REQUIRE_EQUAL(book->curves.size(), 300U);
    flowmesh::MarketDataQuery query;
    query.levels = flowmesh::BookLevelsQuery{};
    query.levels->depth = flowmesh::MARKET_DATA_MAX_LEVELS;
    const auto compute = [&](const bool unchanged) {
        flowmesh::MarketData data;
        data.unchanged = unchanged;
        data.book = book;
        flowmesh::ComputeMarketViews(data, query);
        BOOST_CHECK(!data.book);
        return data;
    };
    const auto first{compute(false)};
    BOOST_REQUIRE(first.levels && first.levels->available);
    BOOST_CHECK(first.levels->head == head);
    BOOST_CHECK_EQUAL(first.levels->total_curves, 300U);
    BOOST_CHECK_EQUAL(first.levels->bids.levels.size(), 25U);
    BOOST_CHECK_EQUAL(first.levels->asks.levels.size(), 25U);
    BOOST_CHECK_EQUAL(first.levels->bids.levels.back().cumulative, 373); // sum of 1 + i % 4 over 150 bids
    BOOST_CHECK_EQUAL(first.levels->asks.levels.back().cumulative, 300);
    auto expected{flowmesh::ComputeBookLevels(book->curves, *query.levels)};
    expected.head = head;
    expected.standing.head = head;
    BOOST_CHECK(*first.levels == expected);
    BOOST_CHECK_EQUAL(book->memo->levels.size(), 1U);
    // The same query at the same copy reuses the computation.
    BOOST_CHECK(*compute(false).levels == expected);
    BOOST_CHECK_EQUAL(book->memo->levels.size(), 1U);
    // An unchanged reply carries them only when asked.
    BOOST_CHECK(!compute(true).levels);
    query.levels->include_unchanged = true;
    BOOST_CHECK(compute(true).levels);
    BOOST_CHECK_EQUAL(book->memo->levels.size(), 2U);
    // Another grouping or account is another entry, within the memo bound.
    for (uint8_t i{0}; i < 12; ++i) {
        query.levels->account = Account(i);
        BOOST_CHECK(compute(false).levels->available);
    }
    BOOST_CHECK_EQUAL(book->memo->levels.size(), flowmesh::BookViewMemo::MAX_ENTRIES);
    // No copy: unavailable, with the reason.
    flowmesh::MarketData none;
    flowmesh::ComputeMarketViews(none, query);
    BOOST_REQUIRE(none.levels);
    BOOST_CHECK(!none.levels->available);
    BOOST_CHECK_EQUAL(none.levels->unavailable_reason, "no_certified_state");
}

BOOST_AUTO_TEST_SUITE_END()
