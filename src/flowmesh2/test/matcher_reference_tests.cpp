// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/engine/book.h>
#include <flowmesh2/engine/matcher.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/test/reference_matcher.h>
#include <flowmesh2/test/util/engine_fixtures.h>
#include <flowmesh2/test/util/ledger_fixtures.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;
namespace ref = flowmesh2::test::reference;

namespace {

//! splitmix64: deterministic test randomness, independent of the code under test.
struct Rng {
    uint64_t state;
    uint64_t Next()
    {
        uint64_t z{state += 0x9e3779b97f4a7c15ULL};
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    uint64_t Below(uint64_t n) { return n == 0 ? 0 : Next() % n; }
    template <class T>
    T Pick(std::initializer_list<T> values)
    {
        return *(values.begin() + Below(values.size()));
    }
};

ref::Stop ToRef(engine::TakerStop stop)
{
    switch (stop) {
    case engine::TakerStop::FILLED: return ref::Stop::FILLED;
    case engine::TakerStop::NO_CROSS: return ref::Stop::NO_CROSS;
    case engine::TakerStop::ORDER_FILL_CAP: return ref::Stop::ORDER_FILL_CAP;
    case engine::TakerStop::SLOT_FILL_CAP: return ref::Stop::SLOT_FILL_CAP;
    case engine::TakerStop::PENDING: break;
    }
    BOOST_FAIL("taker left the sweep without a stop");
    return ref::Stop::FILLED;
}

//! What the batches covered, so the test can require that every branch ran.
struct Coverage {
    size_t batches{0};
    size_t fills{0};
    size_t prorata_rounds{0};  // rounds where the eligible demand exceeded the level
    size_t leftover_rounds{0}; // of those, rounds that handed out largest-remainder lots
    size_t self_trades{0};
    size_t closed{0};
    size_t taker_sells{0};
    std::map<ref::Stop, size_t> stops;
    size_t nonzero_fees{0};
    size_t rank_order_differs{0};
};

/** One seeded batch: a random small book and random takers of one market,
 * swept by the engine and by the naive reference; everything is compared.
 */
void RunBatch(uint64_t seed, Coverage& coverage)
{
    Rng rng{seed};
    const MarketDescriptor market{TestAsset("ref-base"), TestAsset("ref-quote"), rng.Pick<uint64_t>({1, 3}),
                                  rng.Pick<uint64_t>({1, 7})};
    ProfileParams profile{RegtestProfile()};
    profile.fee_ppm = rng.Pick<uint32_t>({100, 30'000, 250'000, 1'000'000});
    profile.max_fills_per_order = rng.Pick<uint32_t>({1, 2, 3, 1'024});
    const uint64_t budget_start{rng.Below(4) == 0 ? profile.max_fills_per_slot : rng.Below(13)};

    EngineFixture fx{market, profile};
    const std::vector<AccountId> accounts{TestAccount("ref-a"), TestAccount("ref-b"), TestAccount("ref-c")};
    for (const auto& a : accounts) fx.FundBoth(a, 100'000'000);

    // Makers: asks in [10, 15], bids in [4, 9], so the book never crosses.
    std::vector<ref::Maker> ref_makers;
    const uint64_t maker_count{rng.Below(11)};
    for (uint64_t i{0}; i < maker_count; ++i) {
        const Side side{rng.Below(2) == 0 ? Side::BUY : Side::SELL};
        const uint64_t price{side == Side::SELL ? 10 + rng.Below(6) : 4 + rng.Below(6)};
        const uint64_t lots{1 + rng.Below(6)};
        const AccountId owner{accounts[rng.Below(accounts.size())]};
        const std::string name{"rm" + std::to_string(seed) + "-" + std::to_string(i)};
        engine::RestingOrder rested{fx.AddMaker(name, owner, side, price, lots)};
        // Some sellers already matched quote in earlier slots: their fee continues on it.
        if (side == Side::SELL && rng.Below(3) == 0) {
            engine::RestingOrder prior{*fx.book.Remove(rested.id)};
            prior.cum_quote = static_cast<Atoms>(rng.Below(5'000));
            prior.fee_charged = static_cast<Atoms>(ref::CeilFee(static_cast<uint64_t>(prior.cum_quote), profile.fee_ppm));
            prior.fills = rng.Below(4);
            RequireOk(fx.book.Rest(prior), "rest with prior quote");
            rested = prior;
        }
        ref_makers.push_back(ref::Maker{rested.id, owner, side, price, lots, rested.arrival_rank,
                                        static_cast<uint64_t>(rested.cum_quote), static_cast<uint64_t>(rested.fee_charged),
                                        rested.fills});
    }

    // Takers: buys limited in [8, 17], sells in [2, 11]; distinct shuffled ranks.
    const uint64_t taker_count{rng.Below(9)};
    std::vector<uint64_t> ranks(taker_count);
    for (uint64_t i{0}; i < taker_count; ++i) ranks[i] = 100 + 3 * i;
    for (uint64_t i{taker_count}; i > 1; --i) std::swap(ranks[i - 1], ranks[rng.Below(i)]);
    if (!std::is_sorted(ranks.begin(), ranks.end())) ++coverage.rank_order_differs;
    std::vector<engine::Taker> takers;
    std::vector<ref::Taker> ref_takers;
    for (uint64_t i{0}; i < taker_count; ++i) {
        const Side side{rng.Below(2) == 0 ? Side::BUY : Side::SELL};
        const uint64_t limit{side == Side::BUY ? 8 + rng.Below(10) : 2 + rng.Below(10)};
        const uint64_t lots{1 + rng.Below(12)};
        const AccountId owner{accounts[rng.Below(accounts.size())]};
        const TimeInForce tif{rng.Below(2) == 0 ? TimeInForce::GTC : TimeInForce::IOC};
        const std::string name{"rt" + std::to_string(seed) + "-" + std::to_string(i)};
        takers.push_back(fx.MakeTaker(name, owner, side, limit, lots, ranks[i], tif));
        ref_takers.push_back(ref::Taker{takers.back().id, owner, side, limit, lots, ranks[i]});
    }

    // The expected ledger: start from the engine's, apply the reference fills.
    std::map<std::pair<AccountId, AssetId>, ledger::Balance> expected;
    for (const auto& a : accounts) {
        for (const AssetId& asset : {market.base, market.quote}) expected[{a, asset}] = fx.ledger.BalanceOf(a, asset);
    }
    std::map<AccountId, uint32_t> expected_open;
    for (const auto& a : accounts) expected_open[a] = fx.ledger.Find(a)->open_orders;
    const Atoms fees_before{fx.ledger.TotalsOf(market.quote).fees_held};

    // Run both. A twin of the engine state sweeps the same takers in the
    // reverse span order: only canonical ranks may matter, never the span.
    EngineFixture twin{fx};
    std::vector<engine::Taker> reversed(takers.rbegin(), takers.rend());
    engine::SlotFillBudget budget{budget_start};
    const engine::SweepResult got{fx.Sweep(takers, budget)};
    engine::SlotFillBudget twin_budget{budget_start};
    const engine::SweepResult twin_got{twin.Sweep(reversed, twin_budget)};
    std::reverse(reversed.begin(), reversed.end());
    BOOST_REQUIRE(twin_got == got);
    BOOST_REQUIRE(reversed == takers);
    BOOST_REQUIRE(twin.book == fx.book);
    BOOST_REQUIRE(twin.ledger == fx.ledger);
    BOOST_REQUIRE(twin_budget == budget);
    const ref::Run want{ref::Match(ref_makers, ref_takers,
                                   ref::Rules{market.base_atoms_per_lot, market.quote_atoms_per_tick, profile.fee_ppm,
                                              profile.max_fills_per_order, budget_start})};

    // Fills and pairing, in execution order.
    BOOST_REQUIRE_EQUAL(got.fills.size(), want.fills.size());
    for (size_t i{0}; i < want.fills.size(); ++i) {
        const engine::ExecutedFill& g{got.fills[i]};
        const ref::Fill& w{want.fills[i]};
        BOOST_REQUIRE(g.fill.maker == w.maker && g.fill.taker == w.taker);
        BOOST_REQUIRE(g.maker_owner == w.maker_owner && g.taker_owner == w.taker_owner);
        BOOST_REQUIRE(g.fill.market == market.Id());
        BOOST_REQUIRE(g.fill.taker_side == w.taker_side);
        BOOST_REQUIRE_EQUAL(g.fill.price_ticks, w.price);
        BOOST_REQUIRE_EQUAL(g.fill.lots, w.lots);
        BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(g.base_atoms), w.base);
        BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(g.fill.quote_atoms), w.quote);
        BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(g.fill.seller_fee), w.fee);
        BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(g.fill.seller_quote_before), w.seller_before);
        if (w.fee > 0) ++coverage.nonzero_fees;
        if (w.maker_owner == w.taker_owner) ++coverage.self_trades;
        if (w.taker_side == Side::SELL) ++coverage.taker_sells;

        // Expected balances: base seller -> buyer, quote buyer -> seller minus the fee.
        const bool taker_buys{w.taker_side == Side::BUY};
        const AccountId& buyer{taker_buys ? w.taker_owner : w.maker_owner};
        const AccountId& seller{taker_buys ? w.maker_owner : w.taker_owner};
        expected[{seller, market.base}].reserved -= static_cast<Atoms>(w.base);
        expected[{buyer, market.base}].available += static_cast<Atoms>(w.base);
        expected[{buyer, market.quote}].reserved -= static_cast<Atoms>(w.quote);
        expected[{seller, market.quote}].available += static_cast<Atoms>(w.quote - w.fee);
    }
    BOOST_REQUIRE(got.closed_makers == want.closed);
    BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(got.fees), want.fees);
    BOOST_REQUIRE_EQUAL(budget.remaining, want.budget_left);
    coverage.fills += want.fills.size();
    coverage.closed += want.closed.size();

    // Takers: state and stop.
    for (size_t i{0}; i < takers.size(); ++i) {
        const engine::Taker& g{takers[i]};
        const ref::Taker& w{want.takers[i]};
        BOOST_REQUIRE_EQUAL(g.remaining_lots, w.lots);
        BOOST_REQUIRE_EQUAL(g.fills, w.fills);
        BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(g.cum_quote), w.cum_quote);
        BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(g.fee_charged), w.fee_charged);
        const Result<engine::Reservation> start{engine::RequiredReservation(market, w.side, w.limit, ref_takers[i].lots)};
        BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(g.reserved_left), static_cast<uint64_t>(start->amount) - w.paid);
        BOOST_REQUIRE(ToRef(g.stop) == w.stop);
        ++coverage.stops[w.stop];
    }

    // The book: every surviving maker, in the canonical listing order, with its progress.
    std::vector<ref::Maker> survivors;
    for (const ref::Maker& m : want.makers) {
        if (m.lots > 0) survivors.push_back(m);
    }
    std::stable_sort(survivors.begin(), survivors.end(), [](const ref::Maker& a, const ref::Maker& b) {
        if (a.side != b.side) return a.side == Side::BUY;
        if (a.price != b.price) return a.side == Side::BUY ? a.price > b.price : a.price < b.price;
        return a.arrival < b.arrival;
    });
    const std::vector<engine::RestingOrder> book{fx.book.Orders()};
    BOOST_REQUIRE_EQUAL(book.size(), survivors.size());
    for (size_t i{0}; i < book.size(); ++i) {
        BOOST_REQUIRE(book[i].id == survivors[i].id);
        BOOST_REQUIRE_EQUAL(book[i].remaining_lots, survivors[i].lots);
        BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(book[i].cum_quote), survivors[i].cum_quote);
        BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(book[i].fee_charged), survivors[i].fee_charged);
        BOOST_REQUIRE_EQUAL(book[i].fills, survivors[i].fills);
    }

    // The ledger: balances, open orders, held fees, conservation.
    for (const auto& id : want.closed) {
        for (const ref::Maker& m : want.makers) {
            if (m.id == id) expected_open[m.owner] -= 1;
        }
    }
    for (const auto& [key, balance] : expected) {
        BOOST_REQUIRE(fx.ledger.BalanceOf(key.first, key.second) == balance);
    }
    for (const auto& [owner, open] : expected_open) BOOST_REQUIRE_EQUAL(fx.ledger.Find(owner)->open_orders, open);
    BOOST_REQUIRE_EQUAL(fx.ledger.TotalsOf(market.quote).fees_held - fees_before, static_cast<Atoms>(want.fees));
    RequireOk(fx.ledger.CheckConservation(), "conservation");

    coverage.prorata_rounds += want.prorata_rounds;
    coverage.leftover_rounds += want.leftover_rounds;
    ++coverage.batches;
}

} // namespace

BOOST_AUTO_TEST_SUITE(matcher_reference_tests)

BOOST_AUTO_TEST_CASE(reference_reproduces_the_hand_vectors)
{
    // The oracle itself must reproduce the golden 4/3/3 pairing of matcher_tests.
    const AccountId carol{TestAccount("carol")};
    std::vector<ref::Maker> makers{{EngineOrder("m1"), carol, Side::SELL, 100, 3, 1},
                                   {EngineOrder("m2"), carol, Side::SELL, 100, 5, 2},
                                   {EngineOrder("m3"), carol, Side::SELL, 100, 2, 3}};
    std::vector<ref::Taker> takers{{EngineOrder("ta"), TestAccount("alice"), Side::BUY, 100, 4, 7},
                                   {EngineOrder("tb"), TestAccount("bob"), Side::BUY, 100, 4, 3},
                                   {EngineOrder("tc"), TestAccount("alice"), Side::BUY, 100, 4, 5}};
    const ref::Run run{ref::Match(makers, takers, ref::Rules{1, 10, 100, 1'024, 16'384})};
    const std::vector<std::tuple<std::string, std::string, uint64_t>> golden{
        {"m1", "tb", 3}, {"m2", "tb", 1}, {"m2", "tc", 3}, {"m2", "ta", 1}, {"m3", "ta", 2}};
    BOOST_REQUIRE_EQUAL(run.fills.size(), golden.size());
    for (size_t i{0}; i < golden.size(); ++i) {
        BOOST_CHECK(run.fills[i].maker == EngineOrder(std::get<0>(golden[i])));
        BOOST_CHECK(run.fills[i].taker == EngineOrder(std::get<1>(golden[i])));
        BOOST_CHECK_EQUAL(run.fills[i].lots, std::get<2>(golden[i]));
    }
    BOOST_CHECK(run.takers[0].stop == ref::Stop::NO_CROSS && run.takers[1].stop == ref::Stop::FILLED);
    // One fee atom per selling order: ceil(0.3), ceil(0.5) and ceil(0.2) for m1, m2 and m3.
    BOOST_CHECK_EQUAL(run.fees, 3U);
    BOOST_CHECK_EQUAL(run.prorata_rounds, 1U);
    BOOST_CHECK_EQUAL(run.leftover_rounds, 1U);
}

BOOST_AUTO_TEST_CASE(seeded_batches_equal_the_naive_reference)
{
    // 1,000 seeded small batches: fills, pairing, fees, taker states and stops,
    // the surviving book, the ledger and the slot budget all equal the naive
    // O(n^2) reference matcher, and the engine's result does not depend on the
    // order of the takers in the span.
    Coverage coverage;
    for (uint64_t seed{1}; seed <= 1'000; ++seed) {
        BOOST_TEST_CONTEXT("seed " << seed) { RunBatch(seed, coverage); }
    }
    BOOST_TEST_MESSAGE("batches " << coverage.batches << " fills " << coverage.fills << " pro-rata rounds "
                                  << coverage.prorata_rounds << " leftover rounds " << coverage.leftover_rounds
                                  << " self-trades " << coverage.self_trades << " closed "
                                  << coverage.closed << " taker sells " << coverage.taker_sells << " nonzero fees "
                                  << coverage.nonzero_fees << " shuffled ranks " << coverage.rank_order_differs
                                  << " FILLED " << coverage.stops[ref::Stop::FILLED] << " NO_CROSS "
                                  << coverage.stops[ref::Stop::NO_CROSS] << " ORDER_FILL_CAP "
                                  << coverage.stops[ref::Stop::ORDER_FILL_CAP] << " SLOT_FILL_CAP "
                                  << coverage.stops[ref::Stop::SLOT_FILL_CAP]);
    BOOST_CHECK_EQUAL(coverage.batches, 1'000U);
    // Every branch ran.
    BOOST_CHECK(coverage.fills > 1'000);
    BOOST_CHECK(coverage.prorata_rounds > 100);
    BOOST_CHECK(coverage.leftover_rounds > 50);
    BOOST_CHECK(coverage.self_trades > 50);
    BOOST_CHECK(coverage.closed > 200);
    BOOST_CHECK(coverage.taker_sells > 200);
    BOOST_CHECK(coverage.nonzero_fees > 200);
    BOOST_CHECK(coverage.rank_order_differs > 500);
    for (const ref::Stop stop : {ref::Stop::FILLED, ref::Stop::NO_CROSS, ref::Stop::ORDER_FILL_CAP, ref::Stop::SLOT_FILL_CAP}) {
        BOOST_CHECK(coverage.stops[stop] > 50);
    }
}

BOOST_AUTO_TEST_SUITE_END()
