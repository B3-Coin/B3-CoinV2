// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/engine/book.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::engine;
using namespace flowmesh2::test;

namespace {

OrderId TestOrder(const std::string& name) { return OrderId{LabelHash("fm2/test/order/" + name)}; }

OrderBook EmptyBook(const MarketDescriptor& market)
{
    Result<OrderBook> book{OrderBook::Create(market)};
    BOOST_REQUIRE(book.has_value());
    return std::move(book).value();
}

//! A resting order of `market` with its exact reservation.
RestingOrder Maker(const MarketDescriptor& market, const std::string& name, Side side, uint64_t price, uint64_t lots,
                   uint64_t rank, const std::string& owner = "alice")
{
    RestingOrder order;
    order.id = TestOrder(name);
    order.owner = TestAccount(owner);
    order.side = side;
    order.price_ticks = price;
    order.remaining_lots = lots;
    order.arrival_rank = rank;
    const Result<Reservation> reservation{RequiredReservation(market, side, price, lots)};
    BOOST_REQUIRE(reservation.has_value());
    order.reserved_left = reservation->amount;
    return order;
}

void RequireRest(OrderBook& book, const RestingOrder& order) { RequireOk(book.Rest(order), "Rest"); }

std::vector<OrderId> Ids(const std::vector<RestingOrder>& orders)
{
    std::vector<OrderId> ids;
    for (const auto& o : orders) ids.push_back(o.id);
    return ids;
}

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
};

/** A naive book model: every resting order in one vector in insertion order.
 * Every query scans the whole vector; nothing is shared with engine/book.cpp
 * except the RestingOrder struct.
 */
struct NaiveBook {
    MarketDescriptor market;
    std::vector<RestingOrder> orders;

    std::optional<uint64_t> Best(Side side) const
    {
        std::optional<uint64_t> best;
        for (const auto& o : orders) {
            if (o.side != side) continue;
            if (!best || (side == Side::BUY ? o.price_ticks > *best : o.price_ticks < *best)) best = o.price_ticks;
        }
        return best;
    }
    bool Crosses(Side side, uint64_t price) const
    {
        const auto other{Best(side == Side::BUY ? Side::SELL : Side::BUY)};
        return other && (side == Side::BUY ? *other <= price : *other >= price);
    }
    //! Insertion position of the first order at the best price of `side` (FIFO), or -1.
    int Front(Side side) const
    {
        const auto best{Best(side)};
        if (!best) return -1;
        for (size_t i{0}; i < orders.size(); ++i) {
            if (orders[i].side == side && orders[i].price_ticks == *best) return static_cast<int>(i);
        }
        return -1;
    }
    //! The canonical listing: bids by descending price, then asks by ascending price, FIFO inside a price.
    std::vector<RestingOrder> Listing() const
    {
        std::vector<RestingOrder> out{orders};
        std::stable_sort(out.begin(), out.end(), [](const RestingOrder& a, const RestingOrder& b) {
            if (a.side != b.side) return a.side == Side::BUY;
            if (a.price_ticks != b.price_ticks) {
                return a.side == Side::BUY ? a.price_ticks > b.price_ticks : a.price_ticks < b.price_ticks;
            }
            return false; // stable: insertion order inside a price
        });
        return out;
    }
    uint64_t LastRankAt(Side side, uint64_t price, bool& any) const
    {
        any = false;
        uint64_t last{0};
        for (const auto& o : orders) {
            if (o.side == side && o.price_ticks == price) {
                any = true;
                last = o.arrival_rank;
            }
        }
        return last;
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(book_tests)

BOOST_AUTO_TEST_CASE(required_reservation_buy_quote_sell_base)
{
    const MarketDescriptor m1{MarketM1()}; // B3/rUSD, 1,000,000 base atoms per lot, 1 quote atom per tick
    const MarketDescriptor m2{MarketM2()}; // TOK/rUSD, 1 base atom per lot, 10 quote atoms per tick

    const Result<Reservation> buy{RequiredReservation(m1, Side::BUY, 250, 5'000)};
    RequireOk(buy, "buy");
    BOOST_CHECK(*buy == (Reservation{TestAsset("rUSD"), 1'250'000}));
    const Result<Reservation> sell{RequiredReservation(m1, Side::SELL, 250, 5'000)};
    RequireOk(sell, "sell");
    BOOST_CHECK(*sell == (Reservation{AssetId::Native(), 5'000'000'000}));
    // The section 14.2 slot 2 order b: 100,000 lots of TOK at 600 reserve 600,000,000 rUSD.
    const Result<Reservation> b{RequiredReservation(m2, Side::BUY, 600, 100'000)};
    RequireOk(b, "b");
    BOOST_CHECK_EQUAL(b->amount, 600'000'000);

    RequireError(RequiredReservation(m1, static_cast<Side>(0), 1, 1), ErrorCode::INVALID_INPUT, "side 0");
    RequireError(RequiredReservation(m1, static_cast<Side>(3), 1, 1), ErrorCode::INVALID_INPUT, "side 3");
    RequireError(RequiredReservation(m2, Side::BUY, uint64_t{1} << 40, uint64_t{1} << 40), ErrorCode::ARITHMETIC_OVERFLOW,
                 "quote overflow");
    RequireError(RequiredReservation(m1, Side::SELL, 1, (uint64_t{1} << 62) / 1'000'000 + 1), ErrorCode::ARITHMETIC_OVERFLOW,
                 "base overflow");
    RequireError(RequiredReservation(MarketDescriptor{}, Side::BUY, 1, 1), ErrorCode::INVALID_INPUT, "invalid market");
}

BOOST_AUTO_TEST_CASE(empty_book_reads_and_create_refusal)
{
    const OrderBook book{EmptyBook(MarketM2())};
    BOOST_CHECK(book.Id() == MarketM2().Id());
    BOOST_CHECK(book.Market() == MarketM2());
    BOOST_CHECK(!book.BestBid().has_value());
    BOOST_CHECK(!book.BestAsk().has_value());
    BOOST_CHECK(book.BestFront(Side::BUY) == nullptr);
    BOOST_CHECK(book.BestFront(Side::SELL) == nullptr);
    BOOST_CHECK(!book.WouldCross(Side::BUY, UINT64_MAX));
    BOOST_CHECK(!book.WouldCross(Side::SELL, 1));
    BOOST_CHECK(book.Empty());
    BOOST_CHECK_EQUAL(book.Size(), 0U);
    BOOST_CHECK(book.Orders().empty());
    RequireOk(book.CheckInvariants(), "invariants");
    const Result<uint64_t> lots{book.LevelLots(Side::BUY, 5)};
    RequireOk(lots, "LevelLots");
    BOOST_CHECK_EQUAL(*lots, 0U);
    BOOST_CHECK(book.Find(TestOrder("none")) == nullptr);

    MarketDescriptor same_assets{MarketM2()};
    same_assets.base = same_assets.quote;
    RequireError(OrderBook::Create(same_assets), ErrorCode::INVALID_INPUT, "base == quote");
    MarketDescriptor zero_tick{MarketM2()};
    zero_tick.quote_atoms_per_tick = 0;
    RequireError(OrderBook::Create(zero_tick), ErrorCode::INVALID_INPUT, "zero tick");
}

BOOST_AUTO_TEST_CASE(rest_find_best_levels_and_fifo)
{
    const MarketDescriptor m{MarketM2()};
    OrderBook book{EmptyBook(m)};
    RequireRest(book, Maker(m, "b99", Side::BUY, 99, 4, 1));
    RequireRest(book, Maker(m, "b100a", Side::BUY, 100, 3, 2));
    RequireRest(book, Maker(m, "a103", Side::SELL, 103, 5, 3));
    RequireRest(book, Maker(m, "b100b", Side::BUY, 100, 7, 4, "bob"));
    RequireRest(book, Maker(m, "a101a", Side::SELL, 101, 2, 5, "bob"));
    RequireRest(book, Maker(m, "a101b", Side::SELL, 101, 6, 6));

    BOOST_CHECK_EQUAL(book.Size(), 6U);
    BOOST_CHECK_EQUAL(*book.BestBid(), 100U);
    BOOST_CHECK_EQUAL(*book.BestAsk(), 101U);
    BOOST_CHECK_EQUAL(*book.Best(Side::BUY), 100U);
    BOOST_CHECK_EQUAL(*book.Best(Side::SELL), 101U);
    // FIFO: the oldest order of the best level is first.
    BOOST_REQUIRE(book.BestFront(Side::BUY) != nullptr);
    BOOST_CHECK(book.BestFront(Side::BUY)->id == TestOrder("b100a"));
    BOOST_REQUIRE(book.BestFront(Side::SELL) != nullptr);
    BOOST_CHECK(book.BestFront(Side::SELL)->id == TestOrder("a101a"));
    BOOST_CHECK_EQUAL(*book.LevelLots(Side::BUY, 100), 10U);
    BOOST_CHECK_EQUAL(*book.LevelLots(Side::SELL, 101), 8U);
    BOOST_CHECK_EQUAL(*book.LevelLots(Side::SELL, 102), 0U);
    BOOST_CHECK_EQUAL(book.LadderOf(Side::BUY).size(), 2U);
    BOOST_CHECK_EQUAL(book.LadderOf(Side::SELL).size(), 2U);

    // Crossing predicate: a bid at 101 or more crosses, an ask at 100 or less crosses.
    BOOST_CHECK(!book.WouldCross(Side::BUY, 100));
    BOOST_CHECK(book.WouldCross(Side::BUY, 101));
    BOOST_CHECK(!book.WouldCross(Side::SELL, 101));
    BOOST_CHECK(book.WouldCross(Side::SELL, 100));

    // Golden listing: bids best first, then asks best first, FIFO inside a level.
    const std::vector<OrderId> expected{TestOrder("b100a"), TestOrder("b100b"), TestOrder("b99"),
                                        TestOrder("a101a"), TestOrder("a101b"), TestOrder("a103")};
    BOOST_CHECK(Ids(book.Orders()) == expected);

    const RestingOrder* found{book.Find(TestOrder("b100b"))};
    BOOST_REQUIRE(found != nullptr);
    BOOST_CHECK(*found == Maker(m, "b100b", Side::BUY, 100, 7, 4, "bob"));
    BOOST_CHECK(book.Find(TestOrder("missing")) == nullptr);
    RequireOk(book.CheckInvariants(), "invariants");
}

BOOST_AUTO_TEST_CASE(rest_refusals_change_nothing)
{
    const MarketDescriptor m{MarketM2()};
    OrderBook book{EmptyBook(m)};
    RequireRest(book, Maker(m, "bid", Side::BUY, 100, 3, 10));
    RequireRest(book, Maker(m, "ask", Side::SELL, 105, 3, 11));
    const OrderBook before{book};

    const auto refused = [&](RestingOrder order, const char* what) {
        RequireError(book.Rest(order), ErrorCode::INVALID_INPUT, what);
        BOOST_CHECK_MESSAGE(book == before, std::string{what} + " changed the book");
    };

    RestingOrder unknown_side{Maker(m, "x1", Side::BUY, 99, 1, 20)};
    unknown_side.side = static_cast<Side>(0);
    refused(unknown_side, "unknown side");
    RestingOrder zero_price{Maker(m, "x2", Side::BUY, 99, 1, 20)};
    zero_price.price_ticks = 0;
    zero_price.reserved_left = 0;
    refused(zero_price, "price 0");
    RestingOrder zero_lots{Maker(m, "x3", Side::BUY, 99, 1, 20)};
    zero_lots.remaining_lots = 0;
    zero_lots.reserved_left = 0;
    refused(zero_lots, "lots 0");
    refused(Maker(m, "bid", Side::BUY, 98, 1, 20), "duplicate id");
    RestingOrder under{Maker(m, "x4", Side::BUY, 99, 2, 20)};
    under.reserved_left -= 1;
    refused(under, "reservation one atom short");
    RestingOrder over{Maker(m, "x5", Side::SELL, 106, 2, 20)};
    over.reserved_left += 1;
    refused(over, "reservation one atom over");
    RestingOrder negative_quote{Maker(m, "x6", Side::SELL, 106, 2, 20)};
    negative_quote.cum_quote = -1;
    refused(negative_quote, "negative cum_quote");
    RestingOrder fee_over_quote{Maker(m, "x7", Side::SELL, 106, 2, 20)};
    fee_over_quote.cum_quote = 5;
    fee_over_quote.fee_charged = 6;
    refused(fee_over_quote, "fee above cum_quote");
    RestingOrder buyer_fee{Maker(m, "x8", Side::BUY, 99, 2, 20)};
    buyer_fee.cum_quote = 100;
    buyer_fee.fee_charged = 1;
    refused(buyer_fee, "fee on a BUY");
    refused(Maker(m, "x9", Side::BUY, 105, 1, 20), "bid at the best ask crosses");
    refused(Maker(m, "x10", Side::BUY, 200, 1, 20), "bid above the best ask crosses");
    refused(Maker(m, "x11", Side::SELL, 100, 1, 20), "ask at the best bid crosses");
    refused(Maker(m, "x12", Side::SELL, 1, 1, 20), "ask below the best bid crosses");
    refused(Maker(m, "x13", Side::BUY, 100, 1, 10), "arrival rank equal to the level's last");
    refused(Maker(m, "x14", Side::BUY, 100, 1, 9), "arrival rank below the level's last");
    RequireError(book.Rest(Maker(m, "x15", Side::BUY, (uint64_t{1} << 40), 1, 20)), ErrorCode::INVALID_INPUT,
                 "crossing check after the reservation");
    BOOST_CHECK(book == before);

    // A lower arrival rank is fine at another level (FIFO is per level).
    RequireRest(book, Maker(m, "low_rank_other_level", Side::BUY, 99, 1, 1));
    // The fees of a SELL may be anything in [0, cum_quote].
    RestingOrder seller{Maker(m, "seller", Side::SELL, 106, 2, 30)};
    seller.cum_quote = 10;
    seller.fee_charged = 10;
    RequireRest(book, seller);
    RequireOk(book.CheckInvariants(), "invariants");
}

BOOST_AUTO_TEST_CASE(remove_cancels_and_erases_empty_levels)
{
    const MarketDescriptor m{MarketM1()};
    OrderBook book{EmptyBook(m)};
    RequireRest(book, Maker(m, "a1", Side::SELL, 250, 5, 1));
    RequireRest(book, Maker(m, "a2", Side::SELL, 250, 6, 2));
    RequireRest(book, Maker(m, "a3", Side::SELL, 250, 7, 3));
    RequireRest(book, Maker(m, "a4", Side::SELL, 260, 1, 4));

    const Result<RestingOrder> middle{book.Remove(TestOrder("a2"))};
    RequireOk(middle, "remove a2");
    BOOST_CHECK(*middle == Maker(m, "a2", Side::SELL, 250, 6, 2));
    BOOST_CHECK(Ids(book.Orders()) == (std::vector<OrderId>{TestOrder("a1"), TestOrder("a3"), TestOrder("a4")}));
    BOOST_CHECK(book.Find(TestOrder("a2")) == nullptr);

    RequireOk(book.Remove(TestOrder("a1")), "remove a1");
    RequireOk(book.Remove(TestOrder("a3")), "remove a3");
    BOOST_CHECK_EQUAL(book.LadderOf(Side::SELL).size(), 1U); // level 250 erased
    BOOST_CHECK_EQUAL(*book.BestAsk(), 260U);

    const OrderBook before{book};
    RequireError(book.Remove(TestOrder("a1")), ErrorCode::INVALID_INPUT, "remove twice");
    RequireError(book.Remove(TestOrder("never")), ErrorCode::INVALID_INPUT, "remove unknown");
    BOOST_CHECK(book == before);
    RequireOk(book.Remove(TestOrder("a4")), "remove a4");
    BOOST_CHECK(book.Empty());
    BOOST_CHECK(book.LadderOf(Side::SELL).empty());
    RequireOk(book.CheckInvariants(), "invariants");
}

BOOST_AUTO_TEST_CASE(fill_best_front_at_the_maker_price)
{
    const MarketDescriptor m{MarketM2()}; // 10 quote atoms per tick
    OrderBook book{EmptyBook(m)};
    RequireRest(book, Maker(m, "a1", Side::SELL, 101, 5, 1));
    RequireRest(book, Maker(m, "a2", Side::SELL, 101, 4, 2));
    RequireRest(book, Maker(m, "a3", Side::SELL, 104, 4, 3));
    RequireRest(book, Maker(m, "b1", Side::BUY, 90, 3, 4));

    // 2 lots of a1 at its own price 101: quote 2 * 101 * 10 = 2,020.
    const Result<FrontFill> first{book.FillBestFront(Side::SELL, 2, 2'020, 7)};
    RequireOk(first, "fill 2");
    BOOST_CHECK(!first->closed);
    BOOST_CHECK(first->order.id == TestOrder("a1"));
    BOOST_CHECK_EQUAL(first->order.remaining_lots, 3U);
    BOOST_CHECK_EQUAL(first->order.reserved_left, 3); // SELL: 3 lots of 1 base atom
    BOOST_CHECK_EQUAL(first->order.cum_quote, 2'020);
    BOOST_CHECK_EQUAL(first->order.fee_charged, 7);
    BOOST_CHECK_EQUAL(first->order.fills, 1U);
    BOOST_CHECK(*book.Find(TestOrder("a1")) == first->order);

    // Refusals leave the book unchanged.
    const OrderBook before{book};
    RequireError(book.FillBestFront(Side::SELL, 0, 0, 0), ErrorCode::INVALID_INPUT, "0 lots");
    RequireError(book.FillBestFront(Side::SELL, 4, 4'040, 0), ErrorCode::INVALID_INPUT, "more than remaining");
    RequireError(book.FillBestFront(Side::SELL, 1, 1'040, 0), ErrorCode::INVALID_INPUT, "quote at another price");
    RequireError(book.FillBestFront(Side::SELL, 1, 1'009, 0), ErrorCode::INVALID_INPUT, "quote one tick-atom short");
    RequireError(book.FillBestFront(Side::SELL, 1, 1'010, 1'011), ErrorCode::INVALID_INPUT, "fee above quote");
    RequireError(book.FillBestFront(Side::SELL, 1, 1'010, -1), ErrorCode::INVALID_INPUT, "negative fee");
    RequireError(book.FillBestFront(Side::BUY, 1, 900, 1), ErrorCode::INVALID_INPUT, "fee on a buying maker");
    RequireError(book.FillBestFront(static_cast<Side>(9), 1, 900, 0), ErrorCode::INVALID_INPUT, "unknown side");
    BOOST_CHECK(book == before);

    // The rest of a1 closes it: it leaves the book and a2 becomes the front.
    const Result<FrontFill> close{book.FillBestFront(Side::SELL, 3, 3'030, 0)};
    RequireOk(close, "close a1");
    BOOST_CHECK(close->closed);
    BOOST_CHECK_EQUAL(close->order.remaining_lots, 0U);
    BOOST_CHECK_EQUAL(close->order.reserved_left, 0);
    BOOST_CHECK_EQUAL(close->order.cum_quote, 5'050);
    BOOST_CHECK_EQUAL(close->order.fills, 2U);
    BOOST_CHECK(book.Find(TestOrder("a1")) == nullptr);
    BOOST_CHECK(book.BestFront(Side::SELL)->id == TestOrder("a2"));

    // Closing a2 empties level 101; the best ask moves to 104.
    RequireOk(book.FillBestFront(Side::SELL, 4, 4'040, 0), "close a2");
    BOOST_CHECK_EQUAL(*book.BestAsk(), 104U);
    BOOST_CHECK_EQUAL(book.LadderOf(Side::SELL).size(), 1U);

    // A buying maker: the quote is its own reservation, exactly.
    const Result<FrontFill> bid{book.FillBestFront(Side::BUY, 1, 900, 0)};
    RequireOk(bid, "bid fill");
    BOOST_CHECK_EQUAL(bid->order.reserved_left, 1'800);
    BOOST_CHECK_EQUAL(bid->order.cum_quote, 900);
    RequireOk(book.CheckInvariants(), "invariants");

    OrderBook empty{EmptyBook(m)};
    RequireError(empty.FillBestFront(Side::SELL, 1, 10, 0), ErrorCode::INVALID_INPUT, "empty side");
}

BOOST_AUTO_TEST_CASE(book_set_create_find_and_find_order)
{
    const std::vector<MarketDescriptor> markets{MarketM2(), MarketM1()};
    Result<BookSet> created{BookSet::Create(markets)};
    RequireOk(created, "BookSet::Create");
    BookSet set{std::move(created).value()};
    BOOST_CHECK_EQUAL(set.MarketCount(), 2U);
    // Books are kept by ascending MarketId whatever the input order.
    std::vector<MarketId> ids;
    for (const auto& [id, book] : set.Books()) ids.push_back(id);
    BOOST_CHECK(std::is_sorted(ids.begin(), ids.end()));
    BOOST_REQUIRE(set.Find(MarketM1().Id()) != nullptr);
    BOOST_CHECK(set.Find(MarketM1().Id())->Market() == MarketM1());
    BOOST_CHECK(set.Find(MarketId{}) == nullptr);
    BOOST_CHECK(set.FindMutable(MarketId{}) == nullptr);

    RequireRest(*set.FindMutable(MarketM2().Id()), Maker(MarketM2(), "o2", Side::BUY, 600, 3, 1));
    RequireRest(*set.FindMutable(MarketM1().Id()), Maker(MarketM1(), "o1", Side::SELL, 250, 3, 2));
    BOOST_CHECK_EQUAL(set.OrderCount(), 2U);
    BOOST_REQUIRE(set.FindOrder(TestOrder("o2")) != nullptr);
    BOOST_CHECK(set.FindOrder(TestOrder("o2"))->Id() == MarketM2().Id());
    BOOST_CHECK(set.FindOrderMutable(TestOrder("o1"))->Id() == MarketM1().Id());
    BOOST_CHECK(set.FindOrder(TestOrder("o3")) == nullptr);
    BOOST_CHECK(set.FindOrderMutable(TestOrder("o3")) == nullptr);
    RequireOk(set.CheckInvariants(), "invariants");

    // One OrderId resting in two books breaks the set's invariant.
    RequireRest(*set.FindMutable(MarketM1().Id()), Maker(MarketM1(), "o2", Side::SELL, 260, 1, 3));
    RequireError(set.CheckInvariants(), ErrorCode::INVARIANT, "same id in two books");

    const std::vector<MarketDescriptor> duplicate{MarketM1(), MarketM2(), MarketM1()};
    RequireError(BookSet::Create(duplicate), ErrorCode::INVALID_INPUT, "duplicate market");
    const std::vector<MarketDescriptor> invalid{MarketM1(), MarketDescriptor{}};
    RequireError(BookSet::Create(invalid), ErrorCode::INVALID_INPUT, "invalid market");
    const Result<BookSet> none{BookSet::Create({})};
    RequireOk(none, "empty set");
    BOOST_CHECK_EQUAL(none->MarketCount(), 0U);
}

BOOST_AUTO_TEST_CASE(random_operations_equal_a_naive_book)
{
    // 64 seeds x 400 operations of Rest, Remove and FillBestFront against the
    // naive vector model: every verdict is predicted and the whole book is
    // compared after every operation (listing, bests, fronts, level sums, Find).
    const MarketDescriptor m{MarketM2()};
    size_t rests{0}, refused_rests{0}, removes{0}, fills{0}, closes{0}, refused_fills{0};
    for (uint64_t seed{1}; seed <= 64; ++seed) {
        Rng rng{seed};
        OrderBook book{EmptyBook(m)};
        NaiveBook model{m, {}};
        uint64_t next_rank{1};
        uint64_t next_name{0};
        for (int step{0}; step < 400; ++step) {
            const uint64_t op{rng.Below(10)};
            if (op < 5) {
                const Side side{rng.Below(2) == 0 ? Side::BUY : Side::SELL};
                const uint64_t price{1 + rng.Below(20)};
                const uint64_t lots{1 + rng.Below(5)};
                // Mostly a fresh rank; sometimes a stale one, which a non-empty level refuses.
                const uint64_t rank{rng.Below(8) == 0 && next_rank > 2 ? next_rank - 2 : next_rank++};
                const RestingOrder order{Maker(m, "r" + std::to_string(next_name++), side, price, lots, rank)};
                bool level_has_orders{false};
                const uint64_t last{model.LastRankAt(side, price, level_has_orders)};
                const bool expect_ok{!model.Crosses(side, price) && !(level_has_orders && last >= rank)};
                const Result<void> rested{book.Rest(order)};
                BOOST_REQUIRE_EQUAL(rested.has_value(), expect_ok);
                if (expect_ok) {
                    model.orders.push_back(order);
                    ++rests;
                } else {
                    ++refused_rests;
                }
            } else if (op < 7) {
                if (model.orders.empty() || rng.Below(6) == 0) {
                    RequireError(book.Remove(TestOrder("never")), ErrorCode::INVALID_INPUT, "remove unknown");
                    continue;
                }
                const size_t at{static_cast<size_t>(rng.Below(model.orders.size()))};
                const Result<RestingOrder> removed{book.Remove(model.orders[at].id)};
                RequireOk(removed, "remove");
                BOOST_REQUIRE(*removed == model.orders[at]);
                model.orders.erase(model.orders.begin() + static_cast<std::ptrdiff_t>(at));
                ++removes;
            } else {
                const Side side{rng.Below(2) == 0 ? Side::BUY : Side::SELL};
                const int front{model.Front(side)};
                if (front < 0) {
                    RequireError(book.FillBestFront(side, 1, 10, 0), ErrorCode::INVALID_INPUT, "fill empty side");
                    ++refused_fills;
                    continue;
                }
                RestingOrder& maker{model.orders[static_cast<size_t>(front)]};
                const uint64_t lots{1 + rng.Below(maker.remaining_lots + 1)}; // sometimes one too many
                const Atoms quote{static_cast<Atoms>(lots * maker.price_ticks * 10)};
                const Atoms fee{side == Side::SELL ? static_cast<Atoms>(rng.Below(static_cast<uint64_t>(quote) + 1)) : 0};
                const Result<FrontFill> filled{book.FillBestFront(side, lots, quote, fee)};
                if (lots > maker.remaining_lots) {
                    RequireError(filled, ErrorCode::INVALID_INPUT, "fill too many lots");
                    ++refused_fills;
                } else {
                    RequireOk(filled, "fill");
                    maker.remaining_lots -= lots;
                    maker.reserved_left = side == Side::BUY ? static_cast<Atoms>(maker.remaining_lots * maker.price_ticks * 10)
                                                            : static_cast<Atoms>(maker.remaining_lots);
                    maker.cum_quote += quote;
                    maker.fee_charged += fee;
                    maker.fills += 1;
                    BOOST_REQUIRE(filled->order == maker);
                    BOOST_REQUIRE_EQUAL(filled->closed, maker.remaining_lots == 0);
                    ++fills;
                    if (maker.remaining_lots == 0) {
                        model.orders.erase(model.orders.begin() + front);
                        ++closes;
                    }
                }
            }
            // Full comparison.
            BOOST_REQUIRE(book.Orders() == model.Listing());
            BOOST_REQUIRE(book.BestBid() == model.Best(Side::BUY));
            BOOST_REQUIRE(book.BestAsk() == model.Best(Side::SELL));
            for (const Side side : {Side::BUY, Side::SELL}) {
                const int front{model.Front(side)};
                const RestingOrder* got{book.BestFront(side)};
                BOOST_REQUIRE_EQUAL(got == nullptr, front < 0);
                if (got) BOOST_REQUIRE(*got == model.orders[static_cast<size_t>(front)]);
            }
            for (uint64_t price{1}; price <= 20; ++price) {
                for (const Side side : {Side::BUY, Side::SELL}) {
                    uint64_t sum{0};
                    for (const auto& o : model.orders) {
                        if (o.side == side && o.price_ticks == price) sum += o.remaining_lots;
                    }
                    BOOST_REQUIRE_EQUAL(*book.LevelLots(side, price), sum);
                }
            }
            for (const auto& o : model.orders) {
                const RestingOrder* got{book.Find(o.id)};
                BOOST_REQUIRE(got != nullptr && *got == o);
            }
            BOOST_REQUIRE_EQUAL(book.Size(), model.orders.size());
            BOOST_REQUIRE(book.CheckInvariants().has_value());
        }
    }
    BOOST_TEST_MESSAGE("book ops: rests " << rests << " refused " << refused_rests << " removes " << removes << " fills "
                                          << fills << " closes " << closes << " refused fills " << refused_fills);
    // Every branch was exercised.
    BOOST_CHECK(rests > 1000 && refused_rests > 100 && removes > 500 && fills > 1000 && closes > 300 && refused_fills > 100);
}

BOOST_AUTO_TEST_SUITE_END()
