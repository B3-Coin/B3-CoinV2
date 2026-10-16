// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/engine/book.h>

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace flowmesh2::engine {

namespace {

constexpr Error Invalid(const char* detail) { return Error{ErrorCode::INVALID_INPUT, detail}; }
constexpr Error Broken(const char* detail) { return Error{ErrorCode::INVARIANT, detail}; }

//! Position of `id` inside `level`; level.end() if absent.
OrderBook::Level::const_iterator FindIn(const OrderBook::Level& level, const OrderId& id)
{
    return std::find_if(level.begin(), level.end(), [&](const RestingOrder& o) { return o.id == id; });
}

//! The best (most aggressive) level of `ladder` for resting orders of `side`.
template <class LadderT>
auto BestLevel(LadderT& ladder, Side side)
{
    // Bids: the highest price is best; asks: the lowest price is best.
    return side == Side::BUY ? std::prev(ladder.end()) : ladder.begin();
}

} // namespace

Result<Reservation> RequiredReservation(const MarketDescriptor& market, Side side, uint64_t price_ticks, uint64_t lots)
{
    if (!IsKnown(side)) return Invalid("unknown side");
    if (side == Side::BUY) {
        const Result<Atoms> quote{QuoteAtoms(market, lots, price_ticks)};
        if (!quote.has_value()) return quote.error();
        return Ok(Reservation{market.quote, *quote});
    }
    const Result<Atoms> base{BaseAtoms(market, lots)};
    if (!base.has_value()) return base.error();
    return Ok(Reservation{market.base, *base});
}

Result<OrderBook> OrderBook::Create(const MarketDescriptor& market)
{
    const Result<void> valid{ValidateMarket(market)};
    if (!valid.has_value()) return valid.error();
    return Ok(OrderBook{market, market.Id()});
}

std::optional<uint64_t> OrderBook::BestBid() const
{
    if (m_bids.empty()) return std::nullopt;
    return m_bids.rbegin()->first;
}

std::optional<uint64_t> OrderBook::BestAsk() const
{
    if (m_asks.empty()) return std::nullopt;
    return m_asks.begin()->first;
}

std::optional<uint64_t> OrderBook::Best(Side side) const { return side == Side::BUY ? BestBid() : BestAsk(); }

bool OrderBook::WouldCross(Side side, uint64_t price_ticks) const
{
    const std::optional<uint64_t> other{Best(Opposite(side))};
    return other.has_value() && Reaches(side, price_ticks, *other);
}

const RestingOrder* OrderBook::Find(const OrderId& id) const
{
    const auto at{m_index.find(id)};
    if (at == m_index.end()) return nullptr;
    const Ladder& ladder{LadderOf(at->second.side)};
    const auto level{ladder.find(at->second.price_ticks)};
    if (level == ladder.end()) return nullptr;
    const auto it{FindIn(level->second, id)};
    return it == level->second.end() ? nullptr : &*it;
}

const RestingOrder* OrderBook::BestFront(Side side) const
{
    const Ladder& ladder{LadderOf(side)};
    if (ladder.empty()) return nullptr;
    return &BestLevel(ladder, side)->second.front();
}

Result<uint64_t> OrderBook::LevelLots(Side side, uint64_t price_ticks) const
{
    const Ladder& ladder{LadderOf(side)};
    const auto level{ladder.find(price_ticks)};
    if (level == ladder.end()) return Ok(uint64_t{0});
    uint64_t sum{0};
    for (const RestingOrder& order : level->second) {
        if (order.remaining_lots > std::numeric_limits<uint64_t>::max() - sum) {
            return Error{ErrorCode::ARITHMETIC_OVERFLOW, "level lots above 2^64-1"};
        }
        sum += order.remaining_lots;
    }
    return Ok(sum);
}

std::vector<RestingOrder> OrderBook::Orders() const
{
    std::vector<RestingOrder> out;
    out.reserve(m_index.size());
    for (auto level{m_bids.rbegin()}; level != m_bids.rend(); ++level) {
        out.insert(out.end(), level->second.begin(), level->second.end());
    }
    for (const auto& [price, level] : m_asks) out.insert(out.end(), level.begin(), level.end());
    return out;
}

Result<void> OrderBook::Rest(RestingOrder order)
{
    if (!IsKnown(order.side)) return Invalid("unknown side");
    if (order.price_ticks == 0) return Invalid("resting order with price 0");
    if (order.remaining_lots == 0) return Invalid("resting order with 0 lots");
    if (m_index.contains(order.id)) return Invalid("order id already resting");
    const Result<Reservation> required{RequiredReservation(m_market, order.side, order.price_ticks, order.remaining_lots)};
    if (!required.has_value()) return required.error();
    if (order.reserved_left != required->amount) return Invalid("resting order is not exactly reserved");
    if (!InAtomRange(order.cum_quote) || !InAtomRange(order.fee_charged) || order.fee_charged > order.cum_quote ||
        (order.side == Side::BUY && order.fee_charged != 0)) {
        return Invalid("resting order quote or fee outside its range");
    }
    if (WouldCross(order.side, order.price_ticks)) return Invalid("resting order would cross the book");
    Ladder& ladder{MutableLadder(order.side)};
    const auto level{ladder.find(order.price_ticks)};
    if (level != ladder.end() && level->second.back().arrival_rank >= order.arrival_rank) {
        return Invalid("arrival rank not above the level's last order");
    }

    // Commit: nothing below can fail.
    const Locator where{order.side, order.price_ticks};
    m_index.emplace(order.id, where);
    ladder[order.price_ticks].push_back(std::move(order));
    return Ok();
}

Result<RestingOrder> OrderBook::Remove(const OrderId& id)
{
    const auto at{m_index.find(id)};
    if (at == m_index.end()) return Invalid("order is not resting");
    Ladder& ladder{MutableLadder(at->second.side)};
    const auto level{ladder.find(at->second.price_ticks)};
    if (level == ladder.end()) return Broken("index names a missing level");
    const auto it{FindIn(level->second, id)};
    if (it == level->second.end()) return Broken("index names a missing order");

    RestingOrder removed{*it};
    level->second.erase(it);
    if (level->second.empty()) ladder.erase(level);
    m_index.erase(at);
    return Ok(std::move(removed));
}

Result<FrontFill> OrderBook::FillBestFront(Side side, uint64_t lots, Atoms quote, Atoms fee)
{
    if (!IsKnown(side)) return Invalid("unknown side");
    Ladder& ladder{MutableLadder(side)};
    if (ladder.empty()) return Invalid("no resting order on that side");
    const auto level{BestLevel(ladder, side)};
    RestingOrder& front{level->second.front()};
    if (lots == 0 || lots > front.remaining_lots) return Invalid("fill lots outside [1, remaining_lots]");
    const Result<Atoms> at_maker_price{QuoteAtoms(m_market, lots, front.price_ticks)};
    if (!at_maker_price.has_value()) return at_maker_price.error();
    if (quote != *at_maker_price) return Invalid("fill quote is not at the maker price");
    if (!InAtomRange(fee) || fee > quote) return Invalid("fill fee outside [0, quote]");
    if (front.side == Side::BUY && fee != 0) return Invalid("a buying maker pays no seller fee");
    const Result<Reservation> spent{RequiredReservation(m_market, side, front.price_ticks, lots)};
    if (!spent.has_value()) return spent.error();

    const Result<Atoms> reserved_left{SubAtoms(front.reserved_left, spent->amount)};
    if (!reserved_left.has_value()) return Broken("maker reservation below its fill");
    const Result<Atoms> cum_quote{AddAtoms(front.cum_quote, quote)};
    if (!cum_quote.has_value()) return cum_quote.error();
    const Result<Atoms> fee_charged{AddAtoms(front.fee_charged, fee)};
    if (!fee_charged.has_value()) return fee_charged.error();
    if (front.fills == std::numeric_limits<uint64_t>::max()) {
        return Error{ErrorCode::ARITHMETIC_OVERFLOW, "fill count above 2^64-1"};
    }
    const uint64_t remaining{front.remaining_lots - lots};
    if (remaining == 0 && *reserved_left != 0) return Broken("closed maker keeps a reservation");

    // Commit: nothing below can fail.
    front.remaining_lots = remaining;
    front.reserved_left = *reserved_left;
    front.cum_quote = *cum_quote;
    front.fee_charged = *fee_charged;
    front.fills += 1;
    FrontFill result{front, remaining == 0};
    if (remaining == 0) {
        m_index.erase(front.id);
        level->second.pop_front();
        if (level->second.empty()) ladder.erase(level);
    }
    return Ok(std::move(result));
}

Result<void> OrderBook::CheckInvariants() const
{
    size_t counted{0};
    for (const Side side : {Side::BUY, Side::SELL}) {
        for (const auto& [price, level] : LadderOf(side)) {
            if (level.empty()) return Broken("empty level stored");
            bool first{true};
            uint64_t last_rank{0};
            for (const RestingOrder& order : level) {
                if (order.side != side || order.price_ticks != price) return Broken("order on the wrong level");
                if (price == 0 || order.remaining_lots == 0) return Broken("resting order with price or lots 0");
                if (!first && order.arrival_rank <= last_rank) return Broken("level not in arrival order");
                first = false;
                last_rank = order.arrival_rank;
                const Result<Reservation> required{RequiredReservation(m_market, side, price, order.remaining_lots)};
                if (!required.has_value() || required->amount != order.reserved_left) {
                    return Broken("resting order is not exactly reserved");
                }
                if (!InAtomRange(order.cum_quote) || !InAtomRange(order.fee_charged) ||
                    order.fee_charged > order.cum_quote || (side == Side::BUY && order.fee_charged != 0)) {
                    return Broken("resting order quote or fee outside its range");
                }
                const auto at{m_index.find(order.id)};
                if (at == m_index.end() || !(at->second == Locator{side, price})) return Broken("index disagrees with the levels");
                ++counted;
            }
        }
    }
    if (counted != m_index.size()) return Broken("index names orders that do not rest");
    const std::optional<uint64_t> bid{BestBid()};
    const std::optional<uint64_t> ask{BestAsk()};
    if (bid.has_value() && ask.has_value() && *bid >= *ask) return Broken("book is crossed");
    return Ok();
}

Result<BookSet> BookSet::Create(std::span<const MarketDescriptor> markets)
{
    BookSet set;
    for (const MarketDescriptor& market : markets) {
        Result<OrderBook> book{OrderBook::Create(market)};
        if (!book.has_value()) return book.error();
        const MarketId id{book->Id()};
        if (!set.m_books.emplace(id, std::move(book).value()).second) return Invalid("duplicate market");
    }
    return Ok(std::move(set));
}

const OrderBook* BookSet::Find(const MarketId& market) const
{
    const auto it{m_books.find(market)};
    return it == m_books.end() ? nullptr : &it->second;
}

OrderBook* BookSet::FindMutable(const MarketId& market)
{
    const auto it{m_books.find(market)};
    return it == m_books.end() ? nullptr : &it->second;
}

const OrderBook* BookSet::FindOrder(const OrderId& order) const
{
    for (const auto& [id, book] : m_books) {
        if (book.Find(order) != nullptr) return &book;
    }
    return nullptr;
}

OrderBook* BookSet::FindOrderMutable(const OrderId& order)
{
    for (auto& [id, book] : m_books) {
        if (book.Find(order) != nullptr) return &book;
    }
    return nullptr;
}

size_t BookSet::OrderCount() const
{
    size_t count{0};
    for (const auto& [id, book] : m_books) count += book.Size();
    return count;
}

Result<void> BookSet::CheckInvariants() const
{
    std::set<OrderId> seen;
    for (const auto& [id, book] : m_books) {
        if (!(book.Id() == id)) return Broken("book stored under another market id");
        const Result<void> ok{book.CheckInvariants()};
        if (!ok.has_value()) return ok.error();
        for (const RestingOrder& order : book.Orders()) {
            if (!seen.insert(order.id).second) return Broken("order id rests in two books");
        }
    }
    return Ok();
}

} // namespace flowmesh2::engine
