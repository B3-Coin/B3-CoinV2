// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/fee_math.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/engine/book.h>
#include <flowmesh2/engine/matcher.h>
#include <flowmesh2/engine/residual.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/test/util/engine_fixtures.h>
#include <flowmesh2/test/util/ledger_fixtures.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <tuple>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::engine;
using namespace flowmesh2::test;

namespace {

const AccountId ALICE{TestAccount("alice")};
const AccountId BOB{TestAccount("bob")};
const AccountId CAROL{TestAccount("carol")};

//! One expected fill: maker name, taker name, price, lots.
struct Pair {
    std::string maker;
    std::string taker;
    uint64_t price;
    uint64_t lots;
};

void CheckPairs(const SweepResult& result, const std::vector<Pair>& expected)
{
    BOOST_REQUIRE_EQUAL(result.fills.size(), expected.size());
    for (size_t i{0}; i < expected.size(); ++i) {
        const Fill& f{result.fills[i].fill};
        BOOST_CHECK_MESSAGE(f.maker == EngineOrder(expected[i].maker), "fill " << i << " maker");
        BOOST_CHECK_MESSAGE(f.taker == EngineOrder(expected[i].taker), "fill " << i << " taker");
        BOOST_CHECK_EQUAL(f.price_ticks, expected[i].price);
        BOOST_CHECK_EQUAL(f.lots, expected[i].lots);
    }
}

//! The same profile with a different per-order fill cap.
ProfileParams WithFillCap(uint32_t cap)
{
    ProfileParams p{RegtestProfile()};
    p.max_fills_per_order = cap;
    return p;
}

//! A small-number market: 1 base atom per lot, 10 quote atoms per tick (TOK/rUSD, section 14.1 M2).
MarketDescriptor Small() { return MarketM2(); }

ledger::Balance Bal(const EngineFixture& fx, const AccountId& a, const AssetId& asset) { return fx.ledger.BalanceOf(a, asset); }

//! Naive largest-remainder allocation: floor shares, then for each taker count
//! the takers ahead of it (larger remainder, or equal remainder and lower rank).
std::vector<uint64_t> NaiveProRata(uint64_t supply, const std::vector<uint64_t>& d, const std::vector<uint64_t>& rank)
{
    uint64_t total{0};
    for (uint64_t x : d) total += x;
    if (total <= supply) return d;
    std::vector<uint64_t> out(d.size());
    std::vector<uint64_t> rem(d.size());
    uint64_t given{0};
    for (size_t i{0}; i < d.size(); ++i) {
        out[i] = supply * d[i] / total;
        rem[i] = supply * d[i] % total;
        given += out[i];
    }
    const uint64_t leftover{supply - given};
    std::vector<uint64_t> result{out};
    for (size_t i{0}; i < d.size(); ++i) {
        uint64_t ahead{0};
        for (size_t j{0}; j < d.size(); ++j) {
            if (rem[j] > rem[i] || (rem[j] == rem[i] && rank[j] < rank[i])) ++ahead;
        }
        if (ahead < leftover) result[i] += 1;
    }
    return result;
}

} // namespace

BOOST_AUTO_TEST_SUITE(matcher_tests)

BOOST_AUTO_TEST_CASE(full_fill_at_the_maker_price)
{
    // ARCHITECTURE.md section 14.2 slots 2-3 in M1 = B3/rUSD: Bob's a rests
    // SELL 5,000 lots at 250; Alice's IOC c buys 2,000 lots limited at 260.
    EngineFixture fx{MarketM1()};
    const AssetId rusd{TestAsset("rUSD")};
    fx.Fund(ALICE, rusd, 1'000'000'000);
    fx.Fund(BOB, AssetId::Native(), 10'000'000'000);
    fx.AddMaker("a", BOB, Side::SELL, 250, 5'000);
    Taker c{fx.MakeTaker("c", ALICE, Side::BUY, 260, 2'000, 1, TimeInForce::IOC)};
    BOOST_CHECK_EQUAL(c.reserved_left, 520'000);

    SlotFillBudget budget{SlotFillBudget::ForSlot(fx.profile)};
    std::vector<Taker> takers{c};
    const SweepResult result{fx.Sweep(takers, budget)};

    // One fill at the MAKER price 250, not the limit 260.
    BOOST_REQUIRE_EQUAL(result.fills.size(), 1U);
    const ExecutedFill& f{result.fills[0]};
    BOOST_CHECK(f.fill.maker == EngineOrder("a"));
    BOOST_CHECK(f.fill.taker == EngineOrder("c"));
    BOOST_CHECK(f.fill.market == MarketM1().Id());
    BOOST_CHECK(f.fill.taker_side == Side::BUY);
    BOOST_CHECK_EQUAL(f.fill.price_ticks, 250U);
    BOOST_CHECK_EQUAL(f.fill.lots, 2'000U);
    BOOST_CHECK_EQUAL(f.fill.quote_atoms, 500'000);
    BOOST_CHECK_EQUAL(f.fill.seller_fee, 50); // ceil(500,000 * 100 / 1e6)
    BOOST_CHECK_EQUAL(f.fill.seller_quote_before, 0);
    BOOST_CHECK(f.maker_owner == BOB);
    BOOST_CHECK(f.taker_owner == ALICE);
    BOOST_CHECK_EQUAL(f.base_atoms, 2'000'000'000);
    BOOST_CHECK_EQUAL(result.fees, 50);
    BOOST_CHECK(result.closed_makers.empty());
    BOOST_CHECK_EQUAL(budget.remaining, fx.profile.max_fills_per_slot - 1U);

    // The taker: filled, 20,000 of its reservation left for phase 6 to release.
    const Taker& done{takers[0]};
    BOOST_CHECK(done.stop == TakerStop::FILLED);
    BOOST_CHECK_EQUAL(done.remaining_lots, 0U);
    BOOST_CHECK_EQUAL(done.reserved_left, 20'000);
    BOOST_CHECK_EQUAL(done.cum_quote, 500'000);
    BOOST_CHECK_EQUAL(done.fee_charged, 0);
    BOOST_CHECK_EQUAL(done.fills, 1U);

    // The maker: 3,000 lots left, exactly reserved, the fee on its cumulative quote.
    const RestingOrder* a{fx.book.Find(EngineOrder("a"))};
    BOOST_REQUIRE(a != nullptr);
    BOOST_CHECK_EQUAL(a->remaining_lots, 3'000U);
    BOOST_CHECK_EQUAL(a->reserved_left, 3'000'000'000);
    BOOST_CHECK_EQUAL(a->cum_quote, 500'000);
    BOOST_CHECK_EQUAL(a->fee_charged, 50);
    BOOST_CHECK_EQUAL(a->fills, 1U);

    // Balances (section 14.2 P5): Alice B3 0 -> 2e9, rUSD reserved 520,000 -> 20,000;
    // Bob rUSD +499,950, B3 reserved 5e9 -> 3e9; 50 held for phase 8.
    BOOST_CHECK(Bal(fx, ALICE, AssetId::Native()) == (ledger::Balance{2'000'000'000, 0}));
    BOOST_CHECK(Bal(fx, ALICE, rusd) == (ledger::Balance{999'480'000, 20'000}));
    BOOST_CHECK(Bal(fx, BOB, rusd) == (ledger::Balance{499'950, 0}));
    BOOST_CHECK(Bal(fx, BOB, AssetId::Native()) == (ledger::Balance{5'000'000'000, 3'000'000'000}));
    BOOST_CHECK_EQUAL(fx.ledger.TotalsOf(rusd).fees_held, 50);
    BOOST_CHECK_EQUAL(fx.ledger.Find(BOB)->open_orders, 1U);

    // A second taker takes the rest: the maker closes and leaves the book.
    std::vector<Taker> rest{fx.MakeTaker("c2", ALICE, Side::BUY, 250, 3'000, 2)};
    const SweepResult closing{fx.Sweep(rest, budget)};
    CheckPairs(closing, {{"a", "c2", 250, 3'000}});
    BOOST_CHECK_EQUAL(closing.fills[0].fill.seller_quote_before, 500'000);
    BOOST_CHECK_EQUAL(closing.fills[0].fill.seller_fee, 75); // ceil(1,250,000e-4) - 50
    BOOST_CHECK(closing.closed_makers == std::vector<OrderId>{EngineOrder("a")});
    BOOST_CHECK(rest[0].stop == TakerStop::FILLED);
    BOOST_CHECK_EQUAL(rest[0].reserved_left, 0);
    BOOST_CHECK(fx.book.Empty());
    BOOST_CHECK_EQUAL(fx.ledger.Find(BOB)->open_orders, 0U);
    BOOST_CHECK(Bal(fx, BOB, AssetId::Native()) == (ledger::Balance{5'000'000'000, 0}));
    BOOST_CHECK_EQUAL(fx.ledger.TotalsOf(rusd).fees_held, 125);
}

BOOST_AUTO_TEST_CASE(partial_fills_on_both_sides)
{
    EngineFixture fx{Small()};
    fx.FundBoth(ALICE, 1'000'000);
    fx.FundBoth(BOB, 1'000'000);
    fx.AddMaker("m", BOB, Side::SELL, 100, 10);

    // A taker smaller than the maker: the maker keeps 6 lots.
    std::vector<Taker> small{fx.MakeTaker("t4", ALICE, Side::BUY, 100, 4, 1)};
    SlotFillBudget budget{SlotFillBudget::ForSlot(fx.profile)};
    CheckPairs(fx.Sweep(small, budget), {{"m", "t4", 100, 4}});
    BOOST_CHECK(small[0].stop == TakerStop::FILLED);
    BOOST_CHECK_EQUAL(fx.book.Find(EngineOrder("m"))->remaining_lots, 6U);
    BOOST_CHECK_EQUAL(fx.book.Find(EngineOrder("m"))->reserved_left, 6);

    // A taker larger than the book: 6 lots fill, 9 remain, nothing crosses any more.
    std::vector<Taker> big{fx.MakeTaker("t15", ALICE, Side::BUY, 120, 15, 2)};
    CheckPairs(fx.Sweep(big, budget), {{"m", "t15", 100, 6}});
    BOOST_CHECK(big[0].stop == TakerStop::NO_CROSS);
    BOOST_CHECK_EQUAL(big[0].remaining_lots, 9U);
    BOOST_CHECK_EQUAL(big[0].fills, 1U);
    // reserved at 120 for 15 lots = 18,000; paid 6 * 100 * 10 = 6,000.
    BOOST_CHECK_EQUAL(big[0].reserved_left, 12'000);
    BOOST_CHECK(fx.book.Empty());
}

BOOST_AUTO_TEST_CASE(fifo_within_a_level_and_price_priority)
{
    EngineFixture fx{Small()};
    fx.FundBoth(ALICE, 1'000'000);
    fx.FundBoth(BOB, 1'000'000);
    fx.FundBoth(CAROL, 1'000'000);
    fx.AddMaker("m1", BOB, Side::SELL, 101, 2);   // oldest, but a worse price
    fx.AddMaker("m2", CAROL, Side::SELL, 100, 3); // best price, first in time there
    fx.AddMaker("m3", BOB, Side::SELL, 100, 4);   // best price, second in time
    fx.AddMaker("m4", CAROL, Side::SELL, 101, 5);

    std::vector<Taker> takers{fx.MakeTaker("t", ALICE, Side::BUY, 101, 8, 1)};
    SlotFillBudget budget{SlotFillBudget::ForSlot(fx.profile)};
    const SweepResult result{fx.Sweep(takers, budget)};
    CheckPairs(result, {{"m2", "t", 100, 3}, {"m3", "t", 100, 4}, {"m1", "t", 101, 1}});
    BOOST_CHECK_EQUAL(result.fills[0].fill.quote_atoms, 3'000);
    BOOST_CHECK_EQUAL(result.fills[2].fill.quote_atoms, 1'010);
    BOOST_CHECK(result.closed_makers == (std::vector<OrderId>{EngineOrder("m2"), EngineOrder("m3")}));
    BOOST_CHECK(takers[0].stop == TakerStop::FILLED);
    // Level 101 keeps FIFO: m1 (1 lot left) is still ahead of m4.
    BOOST_CHECK(fx.book.BestFront(Side::SELL)->id == EngineOrder("m1"));
    BOOST_CHECK_EQUAL(fx.book.BestFront(Side::SELL)->remaining_lots, 1U);
    BOOST_CHECK_EQUAL(fx.ledger.Find(CAROL)->open_orders, 1U);
    BOOST_CHECK_EQUAL(fx.ledger.Find(BOB)->open_orders, 1U);
}

BOOST_AUTO_TEST_CASE(three_takers_split_4_3_3_golden_pairing)
{
    // 10 maker lots at one level (FIFO: m1 3, m2 5, m3 2) against three
    // same-slot takers of 4 lots each. D = 12 > S = 10: each floor share is
    // floor(10 * 4 / 12) = 3 with remainder 4; the one leftover lot goes to the
    // lowest canonical rank. Ranks: tb 3 < tc 5 < ta 7 (span order a, b, c).
    EngineFixture fx{Small()};
    fx.FundBoth(ALICE, 1'000'000);
    fx.FundBoth(BOB, 1'000'000);
    fx.FundBoth(CAROL, 1'000'000);
    fx.AddMaker("m1", CAROL, Side::SELL, 100, 3);
    fx.AddMaker("m2", CAROL, Side::SELL, 100, 5);
    fx.AddMaker("m3", CAROL, Side::SELL, 100, 2);
    std::vector<Taker> takers{fx.MakeTaker("ta", ALICE, Side::BUY, 100, 4, 7), fx.MakeTaker("tb", BOB, Side::BUY, 100, 4, 3),
                              fx.MakeTaker("tc", ALICE, Side::BUY, 100, 4, 5)};

    const Result<std::vector<uint64_t>> split{AllocateProRata(10, std::vector<uint64_t>{4, 4, 4}, std::vector<uint64_t>{7, 3, 5})};
    RequireOk(split, "AllocateProRata");
    BOOST_CHECK(*split == (std::vector<uint64_t>{3, 4, 3}));

    SlotFillBudget budget{SlotFillBudget::ForSlot(fx.profile)};
    const SweepResult result{fx.Sweep(takers, budget)};
    // Golden pairing vector: takers in rank order consume makers FIFO.
    CheckPairs(result, {{"m1", "tb", 100, 3}, {"m2", "tb", 100, 1}, {"m2", "tc", 100, 3}, {"m2", "ta", 100, 1}, {"m3", "ta", 100, 2}});
    BOOST_CHECK(result.closed_makers == (std::vector<OrderId>{EngineOrder("m1"), EngineOrder("m2"), EngineOrder("m3")}));
    // tb got 4, tc and ta 3 each; one lot each stays unfilled and the book is empty.
    BOOST_CHECK_EQUAL(takers[1].remaining_lots, 0U);
    BOOST_CHECK(takers[1].stop == TakerStop::FILLED);
    BOOST_CHECK_EQUAL(takers[2].remaining_lots, 1U);
    BOOST_CHECK(takers[2].stop == TakerStop::NO_CROSS);
    BOOST_CHECK_EQUAL(takers[0].remaining_lots, 1U);
    BOOST_CHECK(takers[0].stop == TakerStop::NO_CROSS);
    BOOST_CHECK(fx.book.Empty());
    // Seller fees are on m2's cumulative quote: 1,000, then 4,000, then 5,000 quote atoms.
    BOOST_CHECK_EQUAL(result.fills[1].fill.seller_quote_before, 0);
    BOOST_CHECK_EQUAL(result.fills[2].fill.seller_quote_before, 1'000);
    BOOST_CHECK_EQUAL(result.fills[3].fill.seller_quote_before, 4'000);
    Atoms m2_fees{0};
    for (size_t i{1}; i <= 3; ++i) m2_fees += result.fills[i].fill.seller_fee;
    BOOST_CHECK_EQUAL(m2_fees, *SellerFeeTotal(5'000, fx.profile.fee_ppm));
}

BOOST_AUTO_TEST_CASE(allocate_pro_rata_exhaustive_small_domain)
{
    // Every supply 0..15 against 1..4 takers with demands 1..5 and every rank
    // permutation equals the naive count-ahead allocation; sums and bounds hold.
    size_t cases{0};
    for (size_t n{1}; n <= 4; ++n) {
        std::vector<uint64_t> d(n, 1);
        while (true) {
            std::vector<uint64_t> rank(n);
            std::iota(rank.begin(), rank.end(), uint64_t{10});
            do {
                for (uint64_t supply{0}; supply <= 15; ++supply) {
                    const Result<std::vector<uint64_t>> got{AllocateProRata(supply, d, rank)};
                    BOOST_REQUIRE(got.has_value());
                    BOOST_REQUIRE(*got == NaiveProRata(supply, d, rank));
                    uint64_t sum{0}, demand{0};
                    for (size_t i{0}; i < n; ++i) {
                        BOOST_REQUIRE((*got)[i] <= d[i]);
                        sum += (*got)[i];
                        demand += d[i];
                    }
                    BOOST_REQUIRE_EQUAL(sum, std::min(supply, demand));
                    ++cases;
                }
            } while (std::next_permutation(rank.begin(), rank.end()));
            size_t k{0};
            while (k < n && d[k] == 5) d[k++] = 1;
            if (k == n) break;
            ++d[k];
        }
    }
    BOOST_TEST_MESSAGE("pro-rata cases: " << cases);

    // A rank tie-break that is not the span order.
    const auto alloc = [](uint64_t s, std::vector<uint64_t> d, std::vector<uint64_t> r) { return *AllocateProRata(s, d, r); };
    BOOST_CHECK(alloc(1, {1, 1}, {9, 2}) == (std::vector<uint64_t>{0, 1}));
    BOOST_CHECK(alloc(2, {3, 3, 3}, {5, 1, 3}) == (std::vector<uint64_t>{0, 1, 1}));
    // Largest remainder beats rank: 7 lots over 5 and 2 (D 7) all fit; 6 lots: 4.28 / 1.71 -> 4 + 2.
    BOOST_CHECK(alloc(6, {5, 2}, {1, 2}) == (std::vector<uint64_t>{4, 2}));
    // Very large values use the exact 128-bit products.
    const uint64_t big{uint64_t{1} << 62};
    BOOST_CHECK(alloc(big, {big, big}, {1, 2}) == (std::vector<uint64_t>{big / 2, big / 2}));
    BOOST_CHECK(alloc(big - 1, {big, big}, {2, 1}) == (std::vector<uint64_t>{big / 2 - 1, big / 2}));

    RequireError(AllocateProRata(1, std::vector<uint64_t>{1, 2}, std::vector<uint64_t>{1}), ErrorCode::INVALID_INPUT, "sizes");
    RequireError(AllocateProRata(1, std::vector<uint64_t>{0}, std::vector<uint64_t>{1}), ErrorCode::INVALID_INPUT, "zero demand");
    RequireError(AllocateProRata(1, std::vector<uint64_t>{1, 1}, std::vector<uint64_t>{4, 4}), ErrorCode::INVALID_INPUT, "repeated rank");
    RequireError(AllocateProRata(1, std::vector<uint64_t>{UINT64_MAX, 1}, std::vector<uint64_t>{1, 2}), ErrorCode::ARITHMETIC_OVERFLOW,
                 "demand sum");
    const Result<std::vector<uint64_t>> none{AllocateProRata(5, std::vector<uint64_t>{}, std::vector<uint64_t>{})};
    RequireOk(none, "no takers");
    BOOST_CHECK(none->empty());
}

BOOST_AUTO_TEST_CASE(multi_level_sweep)
{
    // Asks 100 (3), 101 (4), 103 (5), 104 (2). Taker A: BUY 10 limit 103,
    // rank 1. Taker B: BUY 6 limit 101, rank 2.
    // Level 100: S 3, D 16 -> A 1 r14, B 1 r2, leftover 1 to A: A 2, B 1.
    // Level 101: S 4, D 13 -> A 2 r6, B 1 r7, leftover 1 to B: A 2, B 2.
    // Level 103: only A reaches: S 5, D 6 -> A 5. Level 104 is above A's limit.
    EngineFixture fx{Small()};
    fx.FundBoth(ALICE, 1'000'000);
    fx.FundBoth(BOB, 1'000'000);
    fx.FundBoth(CAROL, 1'000'000);
    fx.AddMaker("p100", CAROL, Side::SELL, 100, 3);
    fx.AddMaker("p101", CAROL, Side::SELL, 101, 4);
    fx.AddMaker("p103", CAROL, Side::SELL, 103, 5);
    fx.AddMaker("p104", CAROL, Side::SELL, 104, 2);
    std::vector<Taker> takers{fx.MakeTaker("B", BOB, Side::BUY, 101, 6, 2), fx.MakeTaker("A", ALICE, Side::BUY, 103, 10, 1)};
    SlotFillBudget budget{SlotFillBudget::ForSlot(fx.profile)};
    const SweepResult result{fx.Sweep(takers, budget)};
    CheckPairs(result, {{"p100", "A", 100, 2}, {"p100", "B", 100, 1}, {"p101", "A", 101, 2}, {"p101", "B", 101, 2}, {"p103", "A", 103, 5}});
    BOOST_CHECK_EQUAL(result.fills[4].fill.quote_atoms, 5'150);
    const Taker& b{takers[0]};
    const Taker& a{takers[1]};
    BOOST_CHECK_EQUAL(a.remaining_lots, 1U);
    BOOST_CHECK(a.stop == TakerStop::NO_CROSS);
    BOOST_CHECK_EQUAL(a.fills, 3U);
    BOOST_CHECK_EQUAL(a.cum_quote, 2'000 + 2'020 + 5'150);
    BOOST_CHECK_EQUAL(a.reserved_left, 10 * 103 * 10 - a.cum_quote);
    BOOST_CHECK_EQUAL(b.remaining_lots, 3U);
    BOOST_CHECK(b.stop == TakerStop::NO_CROSS);
    BOOST_CHECK_EQUAL(*fx.book.BestAsk(), 104U);
    BOOST_CHECK_EQUAL(fx.book.Size(), 1U);
    // Each fill at its maker's price; the taker's quote is the sum.
    Atoms total{0};
    for (const auto& f : result.fills) {
        BOOST_CHECK_EQUAL(f.fill.quote_atoms, static_cast<Atoms>(f.fill.lots * f.fill.price_ticks * 10));
        total += f.fill.quote_atoms;
    }
    BOOST_CHECK_EQUAL(total, a.cum_quote + b.cum_quote);
}

BOOST_AUTO_TEST_CASE(sell_takers_sweep_bids_and_pay_the_fee_on_their_cumulative_quote)
{
    // M1-like quote: 1 quote atom per tick, so fills of 10,001 quote atoms show
    // the cumulative ceiling: per-fill rounding would charge 2 + 2 = 4, the
    // order's total 20,002 is charged ceil(2.0002) = 3.
    EngineFixture fx{MarketDescriptor{TestAsset("TOK"), TestAsset("rUSD"), 1, 1}};
    fx.FundBoth(ALICE, 1'000'000);
    fx.FundBoth(BOB, 1'000'000);
    fx.AddMaker("b1", BOB, Side::BUY, 10'001, 1);
    fx.AddMaker("b2", BOB, Side::BUY, 10'001, 1);
    fx.AddMaker("b3", BOB, Side::BUY, 9'000, 1);
    std::vector<Taker> takers{fx.MakeTaker("s", ALICE, Side::SELL, 9'500, 3, 1)};
    SlotFillBudget budget{SlotFillBudget::ForSlot(fx.profile)};
    const SweepResult result{fx.Sweep(takers, budget)};
    CheckPairs(result, {{"b1", "s", 10'001, 1}, {"b2", "s", 10'001, 1}});
    BOOST_CHECK(result.fills[0].fill.taker_side == Side::SELL);
    BOOST_CHECK_EQUAL(result.fills[0].fill.seller_fee, 2);
    BOOST_CHECK_EQUAL(result.fills[0].fill.seller_quote_before, 0);
    BOOST_CHECK_EQUAL(result.fills[1].fill.seller_fee, 1);
    BOOST_CHECK_EQUAL(result.fills[1].fill.seller_quote_before, 10'001);
    BOOST_CHECK_EQUAL(result.fees, *SellerFeeTotal(20'002, fx.profile.fee_ppm));
    const Taker& s{takers[0]};
    BOOST_CHECK_EQUAL(s.fee_charged, 3);
    BOOST_CHECK_EQUAL(s.cum_quote, 20'002);
    BOOST_CHECK_EQUAL(s.remaining_lots, 1U);
    BOOST_CHECK(s.stop == TakerStop::NO_CROSS); // the 9,000 bid is below the 9,500 limit
    BOOST_CHECK_EQUAL(s.reserved_left, 1);      // one base atom for the remaining lot
    // The seller receives quote minus fee; the buying makers pay no fee.
    BOOST_CHECK(Bal(fx, ALICE, TestAsset("rUSD")) == (ledger::Balance{1'000'000 + 20'002 - 3, 0}));
    BOOST_CHECK(Bal(fx, ALICE, TestAsset("TOK")) == (ledger::Balance{1'000'000 - 3, 1}));
    BOOST_CHECK(Bal(fx, BOB, TestAsset("TOK")) == (ledger::Balance{1'000'000 + 2, 0}));
    // Bob reserved 29,002 for his three bids and paid 20,002 of it.
    BOOST_CHECK(Bal(fx, BOB, TestAsset("rUSD")) == (ledger::Balance{1'000'000 - 29'002, 9'000}));
    BOOST_CHECK_EQUAL(fx.ledger.TotalsOf(TestAsset("rUSD")).fees_held, 3);
}

BOOST_AUTO_TEST_CASE(cumulative_seller_fee_continues_across_sweeps)
{
    // A resting seller filled in two slots pays the fee of its total quote.
    EngineFixture fx{MarketDescriptor{TestAsset("TOK"), TestAsset("rUSD"), 1, 1}};
    fx.FundBoth(ALICE, 1'000'000);
    fx.FundBoth(BOB, 1'000'000);
    fx.AddMaker("ask", BOB, Side::SELL, 10'001, 3);
    SlotFillBudget budget{SlotFillBudget::ForSlot(fx.profile)};
    std::vector<Taker> slot1{fx.MakeTaker("t1", ALICE, Side::BUY, 10'001, 1, 1)};
    std::vector<Taker> slot2{fx.MakeTaker("t2", ALICE, Side::BUY, 10'001, 2, 1)};
    const SweepResult first{fx.Sweep(slot1, budget)};
    const SweepResult second{fx.Sweep(slot2, budget)};
    BOOST_CHECK_EQUAL(first.fees, 2);                      // ceil(1.0001)
    BOOST_CHECK_EQUAL(second.fills[0].fill.seller_quote_before, 10'001);
    BOOST_CHECK_EQUAL(second.fees, 2);                     // ceil(3.0003) - 2
    BOOST_CHECK_EQUAL(first.fees + second.fees, *SellerFeeTotal(30'003, fx.profile.fee_ppm));
    BOOST_CHECK(second.closed_makers == std::vector<OrderId>{EngineOrder("ask")});
}

BOOST_AUTO_TEST_CASE(fill_cap_at_cap_minus_one_cap_and_cap_plus_one)
{
    // max_fills_per_order = 4, one-lot makers, so every lot is one fill.
    for (const uint64_t makers : {3U, 4U, 5U}) {
        EngineFixture fx{Small(), WithFillCap(4)};
        fx.FundBoth(ALICE, 1'000'000);
        fx.FundBoth(BOB, 1'000'000);
        for (uint64_t i{0}; i < makers; ++i) fx.AddMaker("m" + std::to_string(i), BOB, Side::SELL, 100, 1);
        std::vector<Taker> takers{fx.MakeTaker("t", ALICE, Side::BUY, 100, makers, 1, TimeInForce::IOC)};
        SlotFillBudget budget{SlotFillBudget::ForSlot(fx.profile)};
        const SweepResult result{fx.Sweep(takers, budget)};
        const uint64_t expected_fills{std::min<uint64_t>(makers, 4)};
        BOOST_CHECK_EQUAL(result.fills.size(), expected_fills);
        BOOST_CHECK_EQUAL(takers[0].fills, expected_fills);
        BOOST_CHECK_EQUAL(takers[0].remaining_lots, makers - expected_fills);
        BOOST_CHECK(takers[0].stop == (makers <= 4 ? TakerStop::FILLED : TakerStop::ORDER_FILL_CAP));
        BOOST_CHECK_EQUAL(fx.book.Size(), makers - expected_fills);
    }

    // At the cap with lots left but nothing crossing any more: NO_CROSS, not the cap.
    {
        EngineFixture fx{Small(), WithFillCap(4)};
        fx.FundBoth(ALICE, 1'000'000);
        fx.FundBoth(BOB, 1'000'000);
        for (int i{0}; i < 4; ++i) fx.AddMaker("m" + std::to_string(i), BOB, Side::SELL, 100, 1);
        fx.AddMaker("far", BOB, Side::SELL, 102, 1);
        std::vector<Taker> takers{fx.MakeTaker("t", ALICE, Side::BUY, 101, 5, 1)};
        SlotFillBudget budget{SlotFillBudget::ForSlot(fx.profile)};
        BOOST_CHECK_EQUAL(fx.Sweep(takers, budget).fills.size(), 4U);
        BOOST_CHECK(takers[0].stop == TakerStop::NO_CROSS);
    }

    // A capped taker's unconsumed share is re-allocated at the same level.
    // 8 one-lot makers at 100, 1 at 101. A 6 lots (limit 101, rank 1), B 2, C 2
    // (limit 100, ranks 2 and 3). D 10 > S 8: A 4 r8, B 1 r6, C 1 r6; leftover 2
    // to A then B: A 5, B 2, C 1. A stops at 4 fills; the next round gives C the
    // lot A left.
    {
        EngineFixture fx{Small(), WithFillCap(4)};
        fx.FundBoth(ALICE, 1'000'000);
        fx.FundBoth(BOB, 1'000'000);
        fx.FundBoth(CAROL, 1'000'000);
        for (int i{0}; i < 8; ++i) fx.AddMaker("m" + std::to_string(i), CAROL, Side::SELL, 100, 1);
        fx.AddMaker("m101", CAROL, Side::SELL, 101, 1);
        std::vector<Taker> takers{fx.MakeTaker("A", ALICE, Side::BUY, 101, 6, 1), fx.MakeTaker("B", BOB, Side::BUY, 100, 2, 2),
                                  fx.MakeTaker("C", BOB, Side::BUY, 100, 2, 3)};
        SlotFillBudget budget{SlotFillBudget::ForSlot(fx.profile)};
        const SweepResult result{fx.Sweep(takers, budget)};
        CheckPairs(result, {{"m0", "A", 100, 1}, {"m1", "A", 100, 1}, {"m2", "A", 100, 1}, {"m3", "A", 100, 1},
                            {"m4", "B", 100, 1}, {"m5", "B", 100, 1}, {"m6", "C", 100, 1}, {"m7", "C", 100, 1}});
        BOOST_CHECK(takers[0].stop == TakerStop::ORDER_FILL_CAP); // 2 lots left, 101 still crosses
        BOOST_CHECK_EQUAL(takers[0].remaining_lots, 2U);
        BOOST_CHECK(takers[1].stop == TakerStop::FILLED);
        BOOST_CHECK(takers[2].stop == TakerStop::FILLED);
        BOOST_CHECK(fx.book.BestFront(Side::SELL)->id == EngineOrder("m101"));
    }

    // The D6 value itself: 1,024 fills per order, at 1,023, 1,024 and 1,025 makers.
    for (const uint64_t makers : {1'023U, 1'024U, 1'025U}) {
        EngineFixture fx{Small()};
        BOOST_REQUIRE_EQUAL(fx.profile.max_fills_per_order, 1'024U);
        fx.FundBoth(ALICE, 100'000'000);
        fx.FundBoth(BOB, 100'000'000);
        fx.FundBoth(CAROL, 100'000'000);
        // At most 1,000 open orders per account: split the makers over two accounts.
        for (uint64_t i{0}; i < makers; ++i) fx.AddMaker("k" + std::to_string(i), i % 2 == 0 ? BOB : CAROL, Side::SELL, 100, 1);
        std::vector<Taker> takers{fx.MakeTaker("t", ALICE, Side::BUY, 100, makers, 1, TimeInForce::IOC)};
        SlotFillBudget budget{SlotFillBudget::ForSlot(fx.profile)};
        const SweepResult result{fx.Sweep(takers, budget)};
        const uint64_t expected_fills{std::min<uint64_t>(makers, 1'024)};
        BOOST_CHECK_EQUAL(result.fills.size(), expected_fills);
        BOOST_CHECK_EQUAL(takers[0].remaining_lots, makers - expected_fills);
        BOOST_CHECK(takers[0].stop == (makers <= 1'024 ? TakerStop::FILLED : TakerStop::ORDER_FILL_CAP));
    }
}

BOOST_AUTO_TEST_CASE(slot_fill_budget_stops_matching_everywhere)
{
    EngineFixture fx{Small()};
    fx.FundBoth(ALICE, 1'000'000);
    fx.FundBoth(BOB, 1'000'000);
    fx.FundBoth(CAROL, 1'000'000);
    for (int i{0}; i < 5; ++i) fx.AddMaker("ask" + std::to_string(i), BOB, Side::SELL, 100, 1);
    fx.AddMaker("bid", BOB, Side::BUY, 90, 2);
    std::vector<Taker> takers{fx.MakeTaker("buyer", ALICE, Side::BUY, 100, 5, 1),
                              fx.MakeTaker("low", CAROL, Side::BUY, 99, 1, 2),
                              fx.MakeTaker("seller", CAROL, Side::SELL, 90, 2, 3)};
    SlotFillBudget budget{3};
    const SweepResult result{fx.Sweep(takers, budget)};
    // The BUY takers sweep first and use the whole budget.
    CheckPairs(result, {{"ask0", "buyer", 100, 1}, {"ask1", "buyer", 100, 1}, {"ask2", "buyer", 100, 1}});
    BOOST_CHECK_EQUAL(budget.remaining, 0U);
    BOOST_CHECK(takers[0].stop == TakerStop::SLOT_FILL_CAP);
    BOOST_CHECK_EQUAL(takers[0].remaining_lots, 2U);
    BOOST_CHECK(takers[1].stop == TakerStop::NO_CROSS);
    BOOST_CHECK(takers[2].stop == TakerStop::SLOT_FILL_CAP);
    BOOST_CHECK_EQUAL(takers[2].fills, 0U);

    // The exhausted budget carries into the next market's sweep of the slot.
    EngineFixture other{MarketM1()};
    other.FundBoth(ALICE, 10'000'000'000);
    other.FundBoth(BOB, 10'000'000'000);
    other.AddMaker("ask", BOB, Side::SELL, 250, 1);
    std::vector<Taker> later{other.MakeTaker("t", ALICE, Side::BUY, 250, 1, 1)};
    BOOST_CHECK(other.Sweep(later, budget).fills.empty());
    BOOST_CHECK(later[0].stop == TakerStop::SLOT_FILL_CAP);

    // Exactly enough budget: everything fills and the budget ends at zero.
    EngineFixture exact{Small()};
    exact.FundBoth(ALICE, 1'000'000);
    exact.FundBoth(BOB, 1'000'000);
    for (int i{0}; i < 3; ++i) exact.AddMaker("a" + std::to_string(i), BOB, Side::SELL, 100, 1);
    std::vector<Taker> fits{exact.MakeTaker("t", ALICE, Side::BUY, 100, 3, 1)};
    SlotFillBudget three{3};
    BOOST_CHECK_EQUAL(exact.Sweep(fits, three).fills.size(), 3U);
    BOOST_CHECK(fits[0].stop == TakerStop::FILLED);
    BOOST_CHECK_EQUAL(three.remaining, 0U);

    // No takers is a no-op.
    std::vector<Taker> none;
    SlotFillBudget full{SlotFillBudget::ForSlot(exact.profile)};
    const SweepResult empty{exact.Sweep(none, full)};
    BOOST_CHECK(empty.fills.empty() && empty.closed_makers.empty() && empty.fees == 0);
    BOOST_CHECK_EQUAL(full.remaining, exact.profile.max_fills_per_slot);
}

BOOST_AUTO_TEST_CASE(self_trade_conserves_and_charges_the_fee_once)
{
    for (const Side taker_side : {Side::BUY, Side::SELL}) {
        EngineFixture fx{Small()};
        const AssetId tok{TestAsset("TOK")};
        const AssetId rusd{TestAsset("rUSD")};
        fx.FundBoth(ALICE, 1'000'000);
        fx.AddMaker("own", ALICE, taker_side == Side::BUY ? Side::SELL : Side::BUY, 100, 10);
        std::vector<Taker> takers{fx.MakeTaker("self", ALICE, taker_side, 100, 10, 1)};
        SlotFillBudget budget{SlotFillBudget::ForSlot(fx.profile)};
        const SweepResult result{fx.Sweep(takers, budget)};

        // One fill of quote 10 * 100 * 10 = 10,000 with fee ceil(1.0) = 1.
        BOOST_REQUIRE_EQUAL(result.fills.size(), 1U);
        BOOST_CHECK(result.fills[0].maker_owner == ALICE && result.fills[0].taker_owner == ALICE);
        BOOST_CHECK_EQUAL(result.fills[0].fill.quote_atoms, 10'000);
        BOOST_CHECK_EQUAL(result.fills[0].fill.seller_fee, 1);
        BOOST_CHECK_EQUAL(result.fees, 1);
        // Charged once, to the selling order only.
        const Atoms taker_fee{takers[0].fee_charged};
        BOOST_CHECK_EQUAL(taker_fee, taker_side == Side::SELL ? 1 : 0);
        BOOST_CHECK(fx.book.Empty());
        BOOST_CHECK(result.closed_makers == std::vector<OrderId>{EngineOrder("own")});
        // Alice ends where she started, minus the one fee atom held for phase 8.
        BOOST_CHECK(Bal(fx, ALICE, tok) == (ledger::Balance{1'000'000, 0}));
        BOOST_CHECK(Bal(fx, ALICE, rusd) == (ledger::Balance{1'000'000 - 1, 0}));
        BOOST_CHECK_EQUAL(fx.ledger.TotalsOf(rusd).fees_held, 1);
        BOOST_CHECK_EQUAL(fx.ledger.TotalsOf(rusd).deposits, 1'000'000);
        BOOST_CHECK_EQUAL(fx.ledger.TotalsOf(tok).fees_held, 0);
        BOOST_CHECK_EQUAL(fx.ledger.Find(ALICE)->open_orders, 0U);
        RequireOk(fx.ledger.CheckConservation(), "conservation");
    }
}

BOOST_AUTO_TEST_CASE(preconditions_refuse_and_change_nothing)
{
    EngineFixture fx{Small()};
    fx.FundBoth(ALICE, 1'000'000);
    fx.FundBoth(BOB, 1'000'000);
    fx.AddMaker("ask", BOB, Side::SELL, 100, 5);
    const Taker good{fx.MakeTaker("good", ALICE, Side::BUY, 100, 3, 1)};
    // Bob's sell on the other side shares good's rank: only the rank is wrong.
    const Taker same_rank{fx.MakeTaker("same_rank", BOB, Side::SELL, 1, 1, 1)};
    const engine::OrderBook book_before{fx.book};
    const ledger::LedgerState ledger_before{fx.ledger};

    const auto refused = [&](std::vector<Taker> takers, ErrorCode code, const std::string& what,
                             const ProfileParams* profile = nullptr) {
        const std::vector<Taker> takers_before{takers};
        SlotFillBudget budget{7};
        RequireError(SweepA2(fx.book, fx.ledger, takers, profile ? *profile : fx.profile, budget), code, what);
        BOOST_CHECK_MESSAGE(fx.book == book_before, what + ": book changed");
        BOOST_CHECK_MESSAGE(fx.ledger == ledger_before, what + ": ledger changed");
        BOOST_CHECK_MESSAGE(takers == takers_before, what + ": takers changed");
        BOOST_CHECK_MESSAGE(budget.remaining == 7, what + ": budget changed");
    };
    const auto with = [&](auto change) {
        Taker t{good};
        change(t);
        return std::vector<Taker>{t};
    };

    refused(with([](Taker& t) { t.tif = TimeInForce::POST_ONLY; }), ErrorCode::INVALID_INPUT, "post-only taker");
    refused(with([](Taker& t) { t.side = static_cast<Side>(0); }), ErrorCode::INVALID_INPUT, "unknown side");
    refused(with([](Taker& t) { t.tif = static_cast<TimeInForce>(9); }), ErrorCode::INVALID_INPUT, "unknown tif");
    refused(with([](Taker& t) { t.remaining_lots = 0; }), ErrorCode::INVALID_INPUT, "zero lots");
    refused(with([](Taker& t) { t.limit_ticks = 0; }), ErrorCode::INVALID_INPUT, "zero limit");
    refused(with([](Taker& t) { t.fills = 1; }), ErrorCode::INVALID_INPUT, "fills already made");
    refused(with([](Taker& t) { t.cum_quote = 1; }), ErrorCode::INVALID_INPUT, "quote already matched");
    refused(with([](Taker& t) { t.fee_charged = 1; }), ErrorCode::INVALID_INPUT, "fee already charged");
    refused(with([](Taker& t) { t.stop = TakerStop::FILLED; }), ErrorCode::INVALID_INPUT, "already swept");
    refused(with([](Taker& t) { t.id = EngineOrder("ask"); }), ErrorCode::INVALID_INPUT, "taker id rests in the book");
    refused(with([](Taker& t) { t.owner = CAROL; }), ErrorCode::INVALID_INPUT, "owner without an account");
    refused(with([&](Taker& t) { t.owner = fx.ledger.SeatPool(); }), ErrorCode::INVALID_INPUT, "protocol account owner");
    refused(with([](Taker& t) { t.reserved_left -= 1; }), ErrorCode::INVALID_INPUT, "reservation one atom short");
    refused(with([](Taker& t) { t.reserved_left = -1; }), ErrorCode::INVALID_INPUT, "negative reservation");
    {
        Taker second{good};
        second.id = EngineOrder("second");
        second.rank = 2;
        refused({good, second}, ErrorCode::INVALID_INPUT, "two takers claim one reservation");
        second.rank = 1;
        refused({good, second}, ErrorCode::INVALID_INPUT, "repeated rank, one owner");
        refused({good, same_rank}, ErrorCode::INVALID_INPUT, "repeated rank across sides and owners");
        refused({good, good}, ErrorCode::INVALID_INPUT, "repeated OrderId");
        Taker other_rank{same_rank};
        other_rank.rank = 2;
        SlotFillBudget probe{7};
        ledger::LedgerState ledger_copy{fx.ledger};
        engine::OrderBook book_copy{fx.book};
        std::vector<Taker> both{good, other_rank};
        RequireOk(SweepA2(book_copy, ledger_copy, both, fx.profile, probe), "the same pair with distinct ranks sweeps");
    }
    ProfileParams per_side{fx.profile};
    per_side.fee_incidence = FeeIncidence::PER_SIDE;
    refused({good}, ErrorCode::NOT_IMPLEMENTED, "PER_SIDE profile", &per_side);
    ProfileParams bad_matching{fx.profile};
    bad_matching.matching = static_cast<MatchingProfile>(2);
    refused({good}, ErrorCode::INVALID_INPUT, "unknown matching profile", &bad_matching);

    // A maker owner whose open-order count no longer covers its makers.
    RequireOk(fx.ledger.RemoveOpenOrder(BOB), "break open orders");
    const ledger::LedgerState broken{fx.ledger};
    std::vector<Taker> takers{good};
    SlotFillBudget budget{7};
    RequireError(SweepA2(fx.book, fx.ledger, takers, fx.profile, budget), ErrorCode::INVALID_INPUT, "maker open orders");
    BOOST_CHECK(fx.ledger == broken && fx.book == book_before && budget.remaining == 7);

    // The good taker still sweeps once the count is restored.
    RequireOk(fx.ledger.AddOpenOrder(BOB), "restore open orders");
    BOOST_CHECK_EQUAL(fx.Sweep(takers, budget).fills.size(), 1U);
}

BOOST_AUTO_TEST_CASE(taker_stop_registry_pinned)
{
    BOOST_CHECK_EQUAL(static_cast<int>(TakerStop::PENDING), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(TakerStop::FILLED), 2);
    BOOST_CHECK_EQUAL(static_cast<int>(TakerStop::NO_CROSS), 3);
    BOOST_CHECK_EQUAL(static_cast<int>(TakerStop::ORDER_FILL_CAP), 4);
    BOOST_CHECK_EQUAL(static_cast<int>(TakerStop::SLOT_FILL_CAP), 5);
    BOOST_CHECK_EQUAL(ToString(TakerStop::NO_CROSS), "NO_CROSS");
    BOOST_CHECK_EQUAL(ToString(TakerStop::SLOT_FILL_CAP), "SLOT_FILL_CAP");
    BOOST_CHECK(!IsKnown(static_cast<TakerStop>(0)));
    BOOST_CHECK(!IsKnown(static_cast<TakerStop>(6)));
    BOOST_CHECK_EQUAL(ToString(static_cast<TakerStop>(0)), "UNKNOWN");
    BOOST_CHECK(Opposite(Side::BUY) == Side::SELL && Opposite(Side::SELL) == Side::BUY);
    BOOST_CHECK(Reaches(Side::BUY, 100, 100) && Reaches(Side::BUY, 100, 99) && !Reaches(Side::BUY, 100, 101));
    BOOST_CHECK(Reaches(Side::SELL, 100, 100) && Reaches(Side::SELL, 100, 101) && !Reaches(Side::SELL, 100, 99));
}

BOOST_AUTO_TEST_CASE(residual_cross_detection)
{
    const auto remainder = [](Side side, uint64_t limit) {
        Taker t;
        t.side = side;
        t.limit_ticks = limit;
        t.remaining_lots = 1;
        return t;
    };
    const std::vector<Taker> buys{remainder(Side::BUY, 99), remainder(Side::BUY, 101), remainder(Side::BUY, 103)};
    const std::vector<Taker> sells{remainder(Side::SELL, 100), remainder(Side::SELL, 102), remainder(Side::SELL, 104)};
    const Result<ResidualCross> cross{FindResidualCross(buys, sells)};
    RequireOk(cross, "FindResidualCross");
    BOOST_CHECK(cross->buys == (std::vector<size_t>{1, 2}));  // limits >= the lowest sell, 100
    BOOST_CHECK(cross->sells == (std::vector<size_t>{0, 1})); // limits <= the highest buy, 103
    BOOST_CHECK(!cross->Empty());

    const std::vector<Taker> equal_buy{remainder(Side::BUY, 100)};
    const std::vector<Taker> equal_sell{remainder(Side::SELL, 100)};
    // Equal limits cross, on both sides.
    const Result<ResidualCross> equal{FindResidualCross(equal_buy, equal_sell)};
    BOOST_CHECK(equal->buys == std::vector<size_t>{0});
    BOOST_CHECK(equal->sells == std::vector<size_t>{0});
    // The lowest SELL limit and the highest BUY limit are the boundaries, inclusive.
    const std::vector<Taker> at_bounds_buys{remainder(Side::BUY, 100), remainder(Side::BUY, 99)};
    const std::vector<Taker> at_bounds_sells{remainder(Side::SELL, 101), remainder(Side::SELL, 100)};
    const Result<ResidualCross> bounds{FindResidualCross(at_bounds_buys, at_bounds_sells)};
    BOOST_CHECK(bounds->buys == std::vector<size_t>{0});
    BOOST_CHECK(bounds->sells == std::vector<size_t>{1});
    const std::vector<Taker> apart{remainder(Side::SELL, 101)};
    BOOST_CHECK(FindResidualCross(equal_buy, apart)->Empty());
    BOOST_CHECK(FindResidualCross(equal_buy, {})->Empty());
    BOOST_CHECK(FindResidualCross({}, apart)->Empty());

    RequireError(FindResidualCross(equal_sell, apart), ErrorCode::INVALID_INPUT, "a SELL among the buys");
    RequireError(FindResidualCross(equal_buy, equal_buy), ErrorCode::INVALID_INPUT, "a BUY among the sells");
    std::vector<Taker> no_lots{remainder(Side::BUY, 100)};
    no_lots[0].remaining_lots = 0;
    RequireError(FindResidualCross(no_lots, apart), ErrorCode::INVALID_INPUT, "a remainder without lots");

    // How a residual cross arises: on an empty book a BUY at 105 and a SELL
    // at 100 both end NO_CROSS (takers never match each other), so their GTC
    // remainders would rest crossed. R7 refuses them; the call is M3-ENG-02.
    EngineFixture fx{Small()};
    fx.FundBoth(ALICE, 1'000'000);
    fx.FundBoth(BOB, 1'000'000);
    std::vector<Taker> takers{fx.MakeTaker("b", ALICE, Side::BUY, 105, 2, 1), fx.MakeTaker("s", BOB, Side::SELL, 100, 2, 2)};
    SlotFillBudget budget{SlotFillBudget::ForSlot(fx.profile)};
    BOOST_CHECK(fx.Sweep(takers, budget).fills.empty());
    BOOST_CHECK(takers[0].stop == TakerStop::NO_CROSS && takers[1].stop == TakerStop::NO_CROSS);
    const Result<ResidualCross> found{FindResidualCross(std::span{takers}.first(1), std::span{takers}.last(1))};
    BOOST_CHECK(found->buys == std::vector<size_t>{0} && found->sells == std::vector<size_t>{0});
    const engine::OrderBook before{fx.book};
    RequireStub(ResolveResidualCross(fx.book, std::span{takers}.first(1), std::span{takers}.last(1), fx.profile), "M3-ENG-02");
    BOOST_CHECK(fx.book == before);
}

BOOST_AUTO_TEST_SUITE_END()
