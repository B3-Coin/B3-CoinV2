// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <crypto/common.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/engine/band_book.h>
#include <flowmesh2/engine/book.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::engine;
using namespace flowmesh2::test;

namespace {

// A Boost assertion costs microseconds (far more under TSan), and the
// differential test makes about fifteen per operation. These call Boost only
// when the check fails, so a failure still reports the expression and, for
// _EQUAL, both values.
#define BAND_REQUIRE(expr)     do {                           if (!(expr)) BOOST_REQUIRE(expr);     } while (false)
#define BAND_REQUIRE_EQUAL(a, b)     do {                                 if (!((a) == (b))) BOOST_REQUIRE_EQUAL(a, b);     } while (false)

OrderId TestOrder(const std::string& name) { return OrderId{LabelHash("fm2/test/band/" + name)}; }

//! An OrderId whose first 8 bytes (the table's hash) are `home` and whose next 8 make it unique.
OrderId GroundOrder(uint64_t home, uint64_t unique)
{
    std::array<unsigned char, 32> bytes{};
    WriteLE64(bytes.data(), home);
    WriteLE64(bytes.data() + 8, unique);
    bytes[31] = 0xb3;
    return OrderId{uint256{bytes}};
}

RestingOrder Maker(const MarketDescriptor& market, const OrderId& id, Side side, uint64_t price, uint64_t lots, uint64_t rank)
{
    RestingOrder order;
    order.id = id;
    order.owner = TestAccount("alice");
    order.side = side;
    order.price_ticks = price;
    order.remaining_lots = lots;
    order.arrival_rank = rank;
    const Result<Reservation> reservation{RequiredReservation(market, side, price, lots)};
    if (reservation.has_value()) order.reserved_left = reservation->amount;
    return order;
}

BandBook MakeBand(const MarketDescriptor& market, uint64_t band_low, uint32_t capacity)
{
    Result<BandBook> book{BandBook::Create(market, BookLimits{band_low, capacity})};
    RequireOk(book, "BandBook::Create");
    return std::move(book).value();
}

OrderBook MakeSkeleton(const MarketDescriptor& market)
{
    Result<OrderBook> book{OrderBook::Create(market)};
    RequireOk(book, "OrderBook::Create");
    return std::move(book).value();
}

void RequireRefused(const Result<void>& got, ErrorCode code, std::string_view detail, const std::string& what)
{
    BOOST_TEST_CONTEXT(what)
    {
        BOOST_REQUIRE(!got.has_value());
        BOOST_REQUIRE(got.error().code == code);
        BOOST_REQUIRE_EQUAL(got.error().detail, detail);
    }
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

template <class T>
void RequireSameVerdict(const Result<T>& skeleton, const Result<T>& band)
{
    BAND_REQUIRE_EQUAL(skeleton.has_value(), band.has_value());
    if (!skeleton.has_value()) {
        BAND_REQUIRE(skeleton.error().code == band.error().code);
        BAND_REQUIRE_EQUAL(skeleton.error().detail, band.error().detail);
    }
}

//! The skeleton's depth, from its ladder: asks at or below `price`, bids at or above it.
uint64_t SkeletonDepth(const OrderBook& book, Side side, uint64_t price)
{
    util::Unsigned128 sum{0};
    for (const auto& [level_price, level] : book.LadderOf(side)) {
        if (side == Side::BUY ? level_price < price : level_price > price) continue;
        for (const RestingOrder& o : level) sum += util::Unsigned128{o.remaining_lots};
    }
    BAND_REQUIRE(sum <= util::Unsigned128{std::numeric_limits<uint64_t>::max()});
    return static_cast<uint64_t>(sum);
}

void RequireSameReads(const OrderBook& skeleton, const BandBook& band)
{
    BAND_REQUIRE(skeleton.BestBid() == band.BestBid());
    BAND_REQUIRE(skeleton.BestAsk() == band.BestAsk());
    BAND_REQUIRE_EQUAL(skeleton.Size(), band.Size());
    for (const Side side : {Side::BUY, Side::SELL}) {
        const RestingOrder* a{skeleton.BestFront(side)};
        const RestingOrder* b{band.BestFront(side)};
        BAND_REQUIRE_EQUAL(a == nullptr, b == nullptr);
        if (a) BAND_REQUIRE(*a == *b);
        BAND_REQUIRE_EQUAL(skeleton.LadderOf(side).size(), band.LevelCount(side));
    }
}

void RequireSameBook(const OrderBook& skeleton, const BandBook& band)
{
    RequireSameReads(skeleton, band);
    BAND_REQUIRE(skeleton.Orders() == band.Orders());
    for (const Side side : {Side::BUY, Side::SELL}) {
        for (const auto& [price, level] : skeleton.LadderOf(side)) {
            BAND_REQUIRE(std::vector<RestingOrder>(level.begin(), level.end()) == band.LevelOrders(side, price));
        }
    }
}

} // namespace

BOOST_AUTO_TEST_SUITE(band_book_tests)

BOOST_AUTO_TEST_CASE(create_refuses_limits_outside_their_ranges)
{
    const MarketDescriptor m{MarketM2()};
    constexpr uint64_t U64_MAX{std::numeric_limits<uint64_t>::max()};
    RequireError(BandBook::Create(m, BookLimits{0, 8}), ErrorCode::INVALID_INPUT, "band at price 0");
    RequireError(BandBook::Create(m, BookLimits{U64_MAX - BAND_TICKS + 2, 8}), ErrorCode::INVALID_INPUT, "band past 2^64-1");
    RequireError(BandBook::Create(m, BookLimits{1, 0}), ErrorCode::INVALID_INPUT, "capacity 0");
    RequireError(BandBook::Create(m, BookLimits{1, MAX_BOOK_CAPACITY + 1}), ErrorCode::INVALID_INPUT, "capacity too large");
    RequireError(BandBook::Create(MarketDescriptor{}, BookLimits{1, 8}), ErrorCode::INVALID_INPUT, "invalid market");

    const BandBook top{MakeBand(m, U64_MAX - BAND_TICKS + 1, 1)};
    BOOST_CHECK(top.InBand(U64_MAX));
    BOOST_CHECK(!top.InBand(U64_MAX - BAND_TICKS));
    const BandBook book{MakeBand(m, 1'000, 8)};
    BOOST_CHECK(book.Empty());
    BOOST_CHECK(!book.BestBid().has_value() && !book.BestAsk().has_value());
    BOOST_CHECK(book.BestFront(Side::BUY) == nullptr && book.BestFront(Side::SELL) == nullptr);
    BOOST_CHECK(book.Limits() == (BookLimits{1'000, 8}));
    BOOST_CHECK(book.Market() == m);
    BOOST_CHECK(book.Id() == m.Id());
    BOOST_CHECK(!book.InBand(999) && book.InBand(1'000) && book.InBand(1'000 + BAND_TICKS - 1) && !book.InBand(1'000 + BAND_TICKS));
    RequireOk(book.CheckInvariants(), "empty invariants");
}

BOOST_AUTO_TEST_CASE(the_band_edges_rest_and_are_found_as_best)
{
    // Index 0 and index 2^17-1 are the first bit of the first word and the
    // last bit of the last word under the last summary word.
    const MarketDescriptor m{MarketM2()};
    const uint64_t low{5'000};
    const uint64_t high{low + BAND_TICKS - 1};
    BandBook book{MakeBand(m, low, 16)};
    RequireOk(book.Rest(Maker(m, TestOrder("bid-low"), Side::BUY, low, 2, 1)), "bid at the low edge");
    RequireOk(book.Rest(Maker(m, TestOrder("ask-high"), Side::SELL, high, 3, 2)), "ask at the high edge");
    BOOST_CHECK(book.BestBid() == low);
    BOOST_CHECK(book.BestAsk() == high);
    RequireOk(book.Rest(Maker(m, TestOrder("bid-63"), Side::BUY, low + 63, 1, 3)), "bid at the end of word 0");
    RequireOk(book.Rest(Maker(m, TestOrder("bid-64"), Side::BUY, low + 64, 1, 4)), "bid at the start of word 1");
    BOOST_CHECK(book.BestBid() == low + 64);
    RequireOk(book.Rest(Maker(m, TestOrder("ask-4095"), Side::SELL, low + 4'095, 1, 5)), "ask at the end of summary word 0");
    RequireOk(book.Rest(Maker(m, TestOrder("ask-4096"), Side::SELL, low + 4'096, 1, 6)), "ask at the start of summary word 1");
    BOOST_CHECK(book.BestAsk() == low + 4'095);
    RequireOk(book.Remove(TestOrder("ask-4095")), "remove the best ask");
    BOOST_CHECK(book.BestAsk() == low + 4'096);
    RequireOk(book.Remove(TestOrder("bid-64")), "remove the best bid");
    BOOST_CHECK(book.BestBid() == low + 63);
    RequireOk(book.Remove(TestOrder("bid-63")), "remove the next bid");
    BOOST_CHECK(book.BestBid() == low);
    RequireOk(book.CheckInvariants(), "invariants at the edges");
    BOOST_CHECK_EQUAL(book.LevelCount(Side::BUY), 1U);
    BOOST_CHECK_EQUAL(book.LevelCount(Side::SELL), 2U);
    BOOST_CHECK_EQUAL(*book.DepthThrough(Side::SELL, high), 4U);
    BOOST_CHECK_EQUAL(*book.DepthThrough(Side::SELL, high - 1), 1U);
    BOOST_CHECK_EQUAL(*book.DepthThrough(Side::BUY, low), 2U);
    BOOST_CHECK_EQUAL(*book.DepthThrough(Side::BUY, low + 1), 0U);
}

BOOST_AUTO_TEST_CASE(a_price_outside_the_band_is_refused_after_every_skeleton_refusal)
{
    const MarketDescriptor m{MarketM2()};
    const uint64_t low{100};
    const uint64_t high{low + BAND_TICKS - 1};
    BandBook book{MakeBand(m, low, 16)};
    RequireRefused(book.Rest(Maker(m, TestOrder("under"), Side::BUY, low - 1, 1, 1)), ErrorCode::INVALID_INPUT, BAND_REFUSED,
                   "one tick under the band");
    RequireRefused(book.Rest(Maker(m, TestOrder("over"), Side::SELL, high + 1, 1, 2)), ErrorCode::INVALID_INPUT, BAND_REFUSED,
                   "one tick over the band");
    RequireOk(book.Rest(Maker(m, TestOrder("ask"), Side::SELL, 500, 1, 3)), "an ask in the band");
    // A BUY above the band that would cross gets the skeleton's refusal, not the band's.
    RequireRefused(book.Rest(Maker(m, TestOrder("crossing"), Side::BUY, high + 10, 1, 4)), ErrorCode::INVALID_INPUT,
                   "resting order would cross the book", "a crossing order outside the band");
    // An inexact reservation outside the band: the skeleton's refusal.
    RestingOrder wrong{Maker(m, TestOrder("wrong"), Side::SELL, high + 5, 2, 5)};
    wrong.reserved_left -= 1;
    RequireRefused(book.Rest(wrong), ErrorCode::INVALID_INPUT, "resting order is not exactly reserved", "an inexact order outside the band");
    // Reads outside the band see nothing.
    BOOST_CHECK_EQUAL(*book.LevelLots(Side::SELL, high + 1), 0U);
    BOOST_CHECK(book.LevelOrders(Side::SELL, low - 1).empty());
    BOOST_CHECK_EQUAL(*book.DepthThrough(Side::SELL, low - 1), 0U);
    BOOST_CHECK_EQUAL(*book.DepthThrough(Side::SELL, std::numeric_limits<uint64_t>::max()), 1U);
    BOOST_CHECK_EQUAL(book.Size(), 1U);
    RequireOk(book.CheckInvariants(), "invariants");
}

BOOST_AUTO_TEST_CASE(a_full_book_refuses_with_book_full_until_an_order_leaves)
{
    const MarketDescriptor m{MarketM2()};
    BandBook book{MakeBand(m, 1, 3)};
    RequireOk(book.Rest(Maker(m, TestOrder("b1"), Side::BUY, 10, 1, 1)), "rest 1");
    RequireOk(book.Rest(Maker(m, TestOrder("b2"), Side::BUY, 11, 1, 2)), "rest 2");
    RequireOk(book.Rest(Maker(m, TestOrder("a1"), Side::SELL, 20, 2, 3)), "rest 3");
    RequireRefused(book.Rest(Maker(m, TestOrder("b3"), Side::BUY, 12, 1, 4)), ErrorCode::OVERSIZE, BOOK_FULL, "the fourth order");
    RequireRefused(book.Rest(Maker(m, TestOrder("a2"), Side::SELL, 20, 1, 5)), ErrorCode::OVERSIZE, BOOK_FULL, "the fourth order, other side");
    // The skeleton's refusals come first even in a full book.
    RequireRefused(book.Rest(Maker(m, TestOrder("b1"), Side::BUY, 12, 1, 6)), ErrorCode::INVALID_INPUT, "order id already resting",
                   "a duplicate in a full book");
    RequireRefused(book.Rest(Maker(m, TestOrder("x"), Side::BUY, 20, 1, 7)), ErrorCode::INVALID_INPUT,
                   "resting order would cross the book", "a crossing order in a full book");
    RequireRefused(book.Rest(Maker(m, TestOrder("y"), Side::SELL, 20, 1, 1)), ErrorCode::INVALID_INPUT,
                   "arrival rank not above the level's last order", "a stale rank in a full book");
    BOOST_CHECK_EQUAL(book.Size(), 3U);
    RequireOk(book.CheckInvariants(), "full invariants");

    // A removal frees a node.
    RequireOk(book.Remove(TestOrder("b1")), "remove b1");
    RequireOk(book.Rest(Maker(m, TestOrder("b3"), Side::BUY, 12, 1, 8)), "rest after the removal");
    RequireRefused(book.Rest(Maker(m, TestOrder("b4"), Side::BUY, 9, 1, 9)), ErrorCode::OVERSIZE, BOOK_FULL, "full again");
    // A partial fill frees nothing; a closing fill frees a node.
    RequireOk(book.FillBestFront(Side::SELL, 1, 200, 0), "partial fill");
    RequireRefused(book.Rest(Maker(m, TestOrder("b4"), Side::BUY, 9, 1, 10)), ErrorCode::OVERSIZE, BOOK_FULL, "still full");
    const Result<FrontFill> close{book.FillBestFront(Side::SELL, 1, 200, 3)};
    RequireOk(close, "closing fill");
    BOOST_CHECK(close->closed);
    RequireOk(book.Rest(Maker(m, TestOrder("b4"), Side::BUY, 9, 1, 11)), "rest after the closing fill");
    BOOST_CHECK_EQUAL(book.Size(), 3U);
    RequireOk(book.CheckInvariants(), "invariants after reuse");

    // Capacity 1.
    BandBook one{MakeBand(m, 1, 1)};
    RequireOk(one.Rest(Maker(m, TestOrder("only"), Side::SELL, 5, 1, 1)), "the only order");
    RequireRefused(one.Rest(Maker(m, TestOrder("second"), Side::SELL, 6, 1, 2)), ErrorCode::OVERSIZE, BOOK_FULL, "capacity 1");
}

BOOST_AUTO_TEST_CASE(level_lots_and_depth_report_overflow_like_the_skeleton)
{
    // MarketM2 reserves one base atom per lot, so a SELL may hold 2^62 lots
    // and four of them at one price exceed 2^64-1.
    const MarketDescriptor m{MarketM2()};
    BandBook band{MakeBand(m, 1, 8)};
    OrderBook skeleton{MakeSkeleton(m)};
    const uint64_t big{uint64_t{1} << 62};
    for (uint64_t i{0}; i < 4; ++i) {
        const RestingOrder order{Maker(m, TestOrder("big" + std::to_string(i)), Side::SELL, 50, big, i + 1)};
        RequireOk(skeleton.Rest(order), "skeleton rest");
        RequireOk(band.Rest(order), "band rest");
        RequireSameVerdict(skeleton.LevelLots(Side::SELL, 50), band.LevelLots(Side::SELL, 50));
    }
    RequireError(band.LevelLots(Side::SELL, 50), ErrorCode::ARITHMETIC_OVERFLOW, "level lots above 2^64-1");
    RequireError(band.DepthThrough(Side::SELL, 50), ErrorCode::ARITHMETIC_OVERFLOW, "depth above 2^64-1");
    BOOST_CHECK_EQUAL(*band.DepthThrough(Side::SELL, 49), 0U);
    // Three left: 3 x 2^62 fits.
    RequireOk(band.Remove(TestOrder("big0")), "remove one");
    BOOST_CHECK_EQUAL(*band.LevelLots(Side::SELL, 50), 3 * big);
    BOOST_CHECK_EQUAL(*band.DepthThrough(Side::SELL, 50), 3 * big);
    RequireOk(band.CheckInvariants(), "invariants");
}

BOOST_AUTO_TEST_CASE(equality_ignores_the_pool_layout_and_a_copy_is_independent)
{
    const MarketDescriptor m{MarketM2()};
    BandBook a{MakeBand(m, 1, 8)};
    BandBook b{MakeBand(m, 1, 8)};
    // a reaches {o2, o3} through a removal, so its pool and table differ from b's.
    RequireOk(a.Rest(Maker(m, TestOrder("o1"), Side::BUY, 10, 1, 1)), "a o1");
    RequireOk(a.Rest(Maker(m, TestOrder("o2"), Side::BUY, 10, 1, 2)), "a o2");
    RequireOk(a.Remove(TestOrder("o1")), "a remove o1");
    RequireOk(a.Rest(Maker(m, TestOrder("o3"), Side::SELL, 30, 1, 3)), "a o3");
    RequireOk(b.Rest(Maker(m, TestOrder("o3"), Side::SELL, 30, 1, 3)), "b o3");
    RequireOk(b.Rest(Maker(m, TestOrder("o2"), Side::BUY, 10, 1, 2)), "b o2");
    BOOST_CHECK(a == b);

    BandBook copy{a};
    BOOST_CHECK(copy == a);
    RequireOk(copy.Remove(TestOrder("o2")), "copy remove");
    BOOST_CHECK(!(copy == a));
    BOOST_REQUIRE(a.Find(TestOrder("o2")) != nullptr);
    RequireOk(a.CheckInvariants(), "original invariants");
    RequireOk(copy.CheckInvariants(), "copy invariants");

    const BandBook other_limits{MakeBand(m, 1, 9)};
    BOOST_CHECK(!(other_limits == MakeBand(m, 1, 8)));
}

BOOST_AUTO_TEST_CASE(ground_order_ids_that_share_a_home_slot_are_found_and_removed)
{
    // Every id hashes to table slot 5 (the table of capacity 32 has 64 slots).
    const MarketDescriptor m{MarketM2()};
    BandBook book{MakeBand(m, 1, 32)};
    for (uint64_t i{0}; i < 32; ++i) {
        RequireOk(book.Rest(Maker(m, GroundOrder(5 + 64 * i, i), Side::BUY, 1 + i, 1, i + 1)), "rest a colliding id");
    }
    // Remove from the middle of the cluster, then check every survivor and the
    // removed ids, forwards and backwards.
    for (const uint64_t i : std::initializer_list<uint64_t>{7, 0, 31, 15, 16, 1}) RequireOk(book.Remove(GroundOrder(5 + 64 * i, i)), "remove from the cluster");
    for (uint64_t i{0}; i < 32; ++i) {
        const bool removed{i == 7 || i == 0 || i == 31 || i == 15 || i == 16 || i == 1};
        BOOST_CHECK_EQUAL(book.Find(GroundOrder(5 + 64 * i, i)) == nullptr, removed);
    }
    // Wrap-around: a cluster at the last slot continues at slot 0.
    BandBook wrap{MakeBand(m, 1, 4)}; // 8 slots
    for (uint64_t i{0}; i < 4; ++i) RequireOk(wrap.Rest(Maker(m, GroundOrder(7, i), Side::SELL, 100 + i, 1, i + 1)), "rest at slot 7");
    RequireOk(wrap.Remove(GroundOrder(7, 0)), "remove the head of a wrapped cluster");
    for (uint64_t i{1}; i < 4; ++i) BOOST_CHECK(wrap.Find(GroundOrder(7, i)) != nullptr);
    RequireOk(book.CheckInvariants(), "cluster invariants");
    RequireOk(wrap.CheckInvariants(), "wrap invariants");
}

BOOST_AUTO_TEST_CASE(one_hundred_thousand_random_operations_match_the_skeleton_book)
{
    // Eight configurations x 12,500 operations of Rest, Remove and
    // FillBestFront, applied to the skeleton OrderBook and to the BandBook.
    // Every verdict (code and detail) and every result must be the same; the
    // bests, fronts, sizes, level counts and the touched level are compared
    // after every operation, Find on a live and a removed id each time, depth
    // every 64 operations, the whole listing every 256 and the invariants every
    // 4,096. Refusals the skeleton cannot give (band, BOOK_FULL) are checked
    // against the skeleton, which must accept the same order (it rests and is
    // removed again). The live ids are indexed, so the test stays linear in
    // operations (TSan intercepts every id comparison).
    struct Config {
        uint64_t band_low;
        uint32_t capacity;
        bool ground_ids; //!< ids that collide in the table
        const char* name;
    };
    constexpr uint64_t U64_MAX{std::numeric_limits<uint64_t>::max()};
    const std::vector<Config> configs{
        {1, 1u << 16, false, "band at 1"},
        {1, 1u << 16, true, "band at 1, colliding ids"},
        {1'000'000, 1u << 16, false, "band at 10^6"},
        {uint64_t{1} << 40, 1u << 16, false, "band at 2^40 (BUY reservations near their cap)"},
        {U64_MAX - BAND_TICKS + 1, 1u << 16, false, "band at the top (BUY reservations overflow)"},
        {1, 64, false, "capacity 64"},
        {1'000, 64, true, "capacity 64, colliding ids"},
        {7, 1u << 16, false, "band at 7, deep levels"},
    };
    const MarketDescriptor m{MarketM2()};
    constexpr int OPS_PER_CONFIG{12'500};
    size_t total_ops{0}, rests{0}, rest_refusals{0}, band_refusals{0}, full_refusals{0}, removes{0}, remove_refusals{0},
        fills{0}, closes{0}, fill_refusals{0}, edge_rests{0}, max_size{0};
    for (size_t c{0}; c < configs.size(); ++c) {
        const Config& config{configs[c]};
        BOOST_TEST_CONTEXT("config " << config.name)
        {
            Rng rng{0xb3b00c + c};
            OrderBook skeleton{MakeSkeleton(m)};
            BandBook band{MakeBand(m, config.band_low, config.capacity)};
            const uint64_t span{c == 7 ? uint64_t{8} : uint64_t{40}};
            const uint64_t centre{config.band_low + BAND_TICKS / 2};
            std::vector<OrderId> live;
            std::map<OrderId, size_t> live_at; //!< position of each live id in `live`
            std::vector<OrderId> gone;
            uint64_t next_rank{1};
            uint64_t next_id{0};
            const auto fresh_id{[&]() {
                const uint64_t n{next_id++};
                // Colliding ids share one of four home slots in the smallest table this test uses (128 slots).
                return config.ground_ids ? GroundOrder(3 + 128 * rng.Below(4) + (uint64_t{1} << 32) * n, n + 1'000'000 * c)
                                         : TestOrder(std::to_string(c) + "/" + std::to_string(n));
            }};
            const auto remember{[&](const OrderId& id) {
                BAND_REQUIRE(live_at.emplace(id, live.size()).second);
                live.push_back(id);
            }};
            const auto forget{[&](const OrderId& id) {
                const auto at{live_at.find(id)};
                BAND_REQUIRE(at != live_at.end());
                const size_t index{at->second};
                live_at.erase(at);
                if (index + 1 != live.size()) {
                    live[index] = live.back();
                    live_at.at(live[index]) = index;
                }
                live.pop_back();
                gone.push_back(id);
            }};
            for (int step{0}; step < OPS_PER_CONFIG; ++step, ++total_ops) {
                const uint64_t op{rng.Below(100)};
                const Side side{rng.Below(2) == 0 ? Side::BUY : Side::SELL};
                uint64_t price{0};
                const uint64_t mode{rng.Below(100)};
                if (mode < 70) {
                    // Near the middle: bids just below, asks just above, some crossing.
                    const uint64_t off{rng.Below(span)};
                    price = side == Side::BUY ? centre + 2 - off : centre - 2 + off;
                } else if (mode < 88) {
                    price = config.band_low + rng.Below(BAND_TICKS);
                } else if (mode < 97) {
                    price = rng.Below(2) == 0 ? config.band_low + rng.Below(4) : config.band_low + BAND_TICKS - 1 - rng.Below(4);
                } else {
                    // Outside the band, where representable.
                    const bool under{config.band_low > 1 && (rng.Below(2) == 0 || config.band_low + BAND_TICKS - 1 == U64_MAX)};
                    price = under ? config.band_low - 1 - rng.Below(std::min<uint64_t>(config.band_low - 1, 3))
                                  : config.band_low + BAND_TICKS + rng.Below(3);
                    if (!under && config.band_low + BAND_TICKS - 1 == U64_MAX) price = 0; // nothing above: a price-0 refusal
                }
                if (op < 55 && !(live.size() > 3'000 && op < 40)) {
                    // Rest: mostly a fresh id and rank; sometimes a live id or a stale rank.
                    const bool reuse{!live.empty() && rng.Below(40) == 0};
                    const OrderId id{reuse ? live[rng.Below(live.size())] : fresh_id()};
                    const uint64_t rank{rng.Below(10) == 0 && next_rank > 3 ? next_rank - 3 : next_rank++};
                    const uint64_t lots{1 + rng.Below(rng.Below(8) == 0 ? 1'000 : 5)};
                    RestingOrder order{Maker(m, id, side, price, lots, rank)};
                    if (rng.Below(50) == 0) order.reserved_left += 1;
                    if (side == Side::SELL && rng.Below(25) == 0) {
                        order.cum_quote = static_cast<Atoms>(rng.Below(1'000));
                        order.fee_charged = static_cast<Atoms>(rng.Below(static_cast<uint64_t>(order.cum_quote) + 2));
                    }
                    const Result<void> b{band.Rest(order)};
                    if (!b.has_value() && (b.error().detail == BAND_REFUSED || b.error().detail == BOOK_FULL)) {
                        // The skeleton has neither refusal: it must accept the same order.
                        RequireOk(skeleton.Rest(order), "skeleton accepts what the band book refuses");
                        const Result<RestingOrder> probe{skeleton.Remove(order.id)};
                        RequireOk(probe, "skeleton removes the probe");
                        BAND_REQUIRE(*probe == order);
                        if (b.error().detail == BAND_REFUSED) {
                            BAND_REQUIRE(b.error().code == ErrorCode::INVALID_INPUT);
                            BAND_REQUIRE(!band.InBand(price));
                            ++band_refusals;
                        } else {
                            BAND_REQUIRE(b.error().code == ErrorCode::OVERSIZE);
                            BAND_REQUIRE_EQUAL(band.Size(), config.capacity);
                            ++full_refusals;
                        }
                    } else {
                        const Result<void> s{skeleton.Rest(order)};
                        RequireSameVerdict(s, b);
                        if (b.has_value()) {
                            remember(id);
                            ++rests;
                            if (price - config.band_low < 4 || config.band_low + BAND_TICKS - 1 - price < 4) ++edge_rests;
                        } else {
                            ++rest_refusals;
                        }
                    }
                } else if (op < 75) {
                    // Remove: a live id, a removed id or one never seen.
                    const uint64_t pick{rng.Below(10)};
                    OrderId id{};
                    if (pick < 8 && !live.empty()) {
                        id = live[rng.Below(live.size())];
                    } else if (pick < 9 && !gone.empty()) {
                        id = gone[rng.Below(gone.size())];
                    } else {
                        id = TestOrder("never/" + std::to_string(rng.Next()));
                    }
                    const Result<RestingOrder> s{skeleton.Remove(id)};
                    const Result<RestingOrder> b{band.Remove(id)};
                    RequireSameVerdict(s, b);
                    if (s.has_value()) {
                        BAND_REQUIRE(*s == *b);
                        forget(id);
                        ++removes;
                    } else {
                        ++remove_refusals;
                    }
                } else {
                    // Fill the front of `side`: usually valid, sometimes too many lots, a wrong quote or a fee on a BUY.
                    const RestingOrder* front{skeleton.BestFront(side)};
                    uint64_t lots{1};
                    Atoms quote{10};
                    Atoms fee{0};
                    if (front != nullptr) {
                        lots = 1 + rng.Below(front->remaining_lots + (rng.Below(10) == 0 ? 1 : 0));
                        const Result<Atoms> q{QuoteAtoms(m, lots, front->price_ticks)};
                        quote = q.has_value() ? *q : Atoms{1};
                        if (rng.Below(30) == 0) quote += 1;
                        if (side == Side::SELL) fee = static_cast<Atoms>(rng.Below(static_cast<uint64_t>(std::max<Atoms>(quote, 0)) + 1));
                        if (side == Side::BUY && rng.Below(30) == 0) fee = 1;
                    }
                    const OrderId front_id{front != nullptr ? front->id : OrderId{}};
                    const Result<FrontFill> s{skeleton.FillBestFront(side, lots, quote, fee)};
                    const Result<FrontFill> b{band.FillBestFront(side, lots, quote, fee)};
                    RequireSameVerdict(s, b);
                    if (s.has_value()) {
                        BAND_REQUIRE(s->order == b->order);
                        BAND_REQUIRE_EQUAL(s->closed, b->closed);
                        ++fills;
                        if (s->closed) {
                            forget(front_id);
                            ++closes;
                        }
                    } else {
                        ++fill_refusals;
                    }
                }
                max_size = std::max(max_size, band.Size());

                RequireSameReads(skeleton, band);
                RequireSameVerdict(skeleton.LevelLots(side, price), band.LevelLots(side, price));
                if (skeleton.LevelLots(side, price).has_value()) BAND_REQUIRE_EQUAL(*skeleton.LevelLots(side, price), *band.LevelLots(side, price));
                if (!live.empty()) {
                    const OrderId& id{live[rng.Below(live.size())]};
                    BAND_REQUIRE(skeleton.Find(id) != nullptr && band.Find(id) != nullptr);
                    BAND_REQUIRE(*skeleton.Find(id) == *band.Find(id));
                }
                if (!gone.empty()) {
                    const OrderId& id{gone[rng.Below(gone.size())]};
                    BAND_REQUIRE_EQUAL(skeleton.Find(id) == nullptr, band.Find(id) == nullptr);
                }
                if (step % 64 == 0) {
                    const uint64_t at{config.band_low + rng.Below(BAND_TICKS)};
                    for (const Side s2 : {Side::BUY, Side::SELL}) {
                        const Result<uint64_t> depth{band.DepthThrough(s2, at)};
                        RequireOk(depth, "depth");
                        BAND_REQUIRE_EQUAL(*depth, SkeletonDepth(skeleton, s2, at));
                    }
                }
                if (step % 256 == 0) RequireSameBook(skeleton, band);
                if (step % 4'096 == 0) {
                    RequireOk(skeleton.CheckInvariants(), "skeleton invariants");
                    RequireOk(band.CheckInvariants(), "band invariants");
                }
            }
            RequireSameBook(skeleton, band);
            RequireOk(band.CheckInvariants(), "final band invariants");
            BAND_REQUIRE_EQUAL(live.size(), band.Size());
        }
    }
    BOOST_TEST_MESSAGE("band book differential: ops " << total_ops << " rests " << rests << " refused " << rest_refusals
                                                      << " band " << band_refusals << " full " << full_refusals << " removes "
                                                      << removes << " refused " << remove_refusals << " fills " << fills
                                                      << " closes " << closes << " refused " << fill_refusals << " edge rests "
                                                      << edge_rests << " max size " << max_size);
    BOOST_CHECK_EQUAL(total_ops, 100'000U);
    // Every branch was exercised.
    BOOST_CHECK(rests > 20'000 && rest_refusals > 5'000 && band_refusals > 300 && full_refusals > 1'000);
    BOOST_CHECK(removes > 10'000 && remove_refusals > 2'000 && fills > 10'000 && closes > 3'000 && fill_refusals > 1'000);
    BOOST_CHECK(edge_rests > 500 && max_size > 1'000);
}

BOOST_AUTO_TEST_SUITE_END()
