// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/engine/band_book.h>

#include <crypto/common.h>

#include <bit>
#include <limits>
#include <utility>

namespace flowmesh2::engine {

namespace {

constexpr Error Invalid(std::string_view detail) { return Error{ErrorCode::INVALID_INPUT, detail}; }
constexpr Error Broken(std::string_view detail) { return Error{ErrorCode::INVARIANT, detail}; }

using U128 = util::Unsigned128;
constexpr uint64_t U64_MAX{std::numeric_limits<uint64_t>::max()};

Result<uint64_t> FitU64(const U128& value, std::string_view detail)
{
    if (value > U128{U64_MAX}) return Error{ErrorCode::ARITHMETIC_OVERFLOW, detail};
    return Ok(static_cast<uint64_t>(value));
}

} // namespace

BandBook::BandBook(MarketDescriptor market, MarketId id, BookLimits limits)
    : m_market{market}, m_id{id}, m_limits{limits}
{
    for (SideBook* s : {&m_bids, &m_asks}) {
        s->levels.assign(BAND_TICKS, Level{});
        s->words.assign(WORDS, 0);
        s->summary.assign(SUMMARY_WORDS, 0);
        s->fenwick.assign(BAND_TICKS + 1, U128{0});
    }
    m_table.assign(std::bit_ceil(size_t{2} * limits.order_capacity), NONE);
}

Result<BandBook> BandBook::Create(const MarketDescriptor& market, const BookLimits& limits)
{
    const Result<void> valid{ValidateMarket(market)};
    if (!valid.has_value()) return valid.error();
    if (limits.band_low_ticks == 0) return Invalid("band starts at price 0");
    if (limits.band_low_ticks > U64_MAX - (BAND_TICKS - 1)) return Invalid("band ends above 2^64-1");
    if (limits.order_capacity == 0 || limits.order_capacity > MAX_BOOK_CAPACITY) {
        return Invalid("order capacity outside [1, MAX_BOOK_CAPACITY]");
    }
    return Ok(BandBook{market, market.Id(), limits});
}

bool BandBook::InBand(uint64_t price_ticks) const
{
    return price_ticks >= m_limits.band_low_ticks && price_ticks - m_limits.band_low_ticks < BAND_TICKS;
}

// ---- Bitmap and Fenwick tree -------------------------------------------------

void BandBook::SetBit(SideBook& s, size_t index)
{
    const size_t w{index / 64};
    s.words[w] |= uint64_t{1} << (index % 64);
    s.summary[w / 64] |= uint64_t{1} << (w % 64);
    ++s.level_count;
}

void BandBook::ClearBit(SideBook& s, size_t index)
{
    const size_t w{index / 64};
    s.words[w] &= ~(uint64_t{1} << (index % 64));
    if (s.words[w] == 0) s.summary[w / 64] &= ~(uint64_t{1} << (w % 64));
    --s.level_count;
}

std::optional<size_t> BandBook::BestIndex(Side side) const
{
    // As OrderBook: BUY reads the bids, any other value the asks.
    const SideBook& s{SideOf(side)};
    if (side != Side::BUY) {
        // Asks: the lowest price is best.
        for (size_t i{0}; i < SUMMARY_WORDS; ++i) {
            if (s.summary[i] == 0) continue;
            const size_t w{i * 64 + static_cast<size_t>(std::countr_zero(s.summary[i]))};
            return w * 64 + static_cast<size_t>(std::countr_zero(s.words[w]));
        }
        return std::nullopt;
    }
    // Bids: the highest price is best.
    for (size_t i{SUMMARY_WORDS}; i-- > 0;) {
        if (s.summary[i] == 0) continue;
        const size_t w{i * 64 + 63 - static_cast<size_t>(std::countl_zero(s.summary[i]))};
        return w * 64 + 63 - static_cast<size_t>(std::countl_zero(s.words[w]));
    }
    return std::nullopt;
}

void BandBook::FenwickAdd(SideBook& s, size_t index, U128 lots)
{
    for (size_t i{index + 1}; i <= BAND_TICKS; i += i & (~i + 1)) s.fenwick[i] += lots;
}

void BandBook::FenwickSub(SideBook& s, size_t index, U128 lots)
{
    for (size_t i{index + 1}; i <= BAND_TICKS; i += i & (~i + 1)) s.fenwick[i] -= lots;
}

U128 BandBook::Prefix(const SideBook& s, size_t index)
{
    U128 sum{0};
    for (size_t i{index + 1}; i > 0; i -= i & (~i + 1)) sum += s.fenwick[i];
    return sum;
}

U128 BandBook::PointLots(const SideBook& s, size_t index)
{
    return index == 0 ? Prefix(s, 0) : Prefix(s, index) - Prefix(s, index - 1);
}

// ---- OrderId table -------------------------------------------------------------

size_t BandBook::Home(const OrderId& id) const
{
    return static_cast<size_t>(ReadLE64(id.id.data())) & (m_table.size() - 1);
}

std::optional<size_t> BandBook::Slot(const OrderId& id) const
{
    const size_t mask{m_table.size() - 1};
    for (size_t slot{Home(id)};; slot = (slot + 1) & mask) {
        const uint32_t node{m_table[slot]};
        if (node == NONE) return std::nullopt;
        if (m_pool[node].order.id == id) return slot;
    }
}

void BandBook::TableInsert(uint32_t node)
{
    const size_t mask{m_table.size() - 1};
    size_t slot{Home(m_pool[node].order.id)};
    while (m_table[slot] != NONE) slot = (slot + 1) & mask;
    m_table[slot] = node;
}

void BandBook::TableErase(size_t slot)
{
    // Backward-shift deletion: no tombstones, so probe lengths never grow
    // with churn. An entry moves into the hole iff its home is not inside
    // the cyclic range (hole, entry].
    const size_t mask{m_table.size() - 1};
    size_t hole{slot};
    for (size_t next{(hole + 1) & mask}; m_table[next] != NONE; next = (next + 1) & mask) {
        const size_t home{Home(m_pool[m_table[next]].order.id)};
        const bool home_in_range{hole <= next ? (home > hole && home <= next) : (home > hole || home <= next)};
        if (!home_in_range) {
            m_table[hole] = m_table[next];
            hole = next;
        }
    }
    m_table[hole] = NONE;
}

// ---- Reads ---------------------------------------------------------------------

std::optional<uint64_t> BandBook::BestBid() const
{
    const std::optional<size_t> at{BestIndex(Side::BUY)};
    if (!at) return std::nullopt;
    return PriceOf(*at);
}

std::optional<uint64_t> BandBook::BestAsk() const
{
    const std::optional<size_t> at{BestIndex(Side::SELL)};
    if (!at) return std::nullopt;
    return PriceOf(*at);
}

std::optional<uint64_t> BandBook::Best(Side side) const { return side == Side::BUY ? BestBid() : BestAsk(); }

bool BandBook::WouldCross(Side side, uint64_t price_ticks) const
{
    const std::optional<uint64_t> other{Best(Opposite(side))};
    return other.has_value() && Reaches(side, price_ticks, *other);
}

const RestingOrder* BandBook::Find(const OrderId& id) const
{
    const std::optional<size_t> slot{Slot(id)};
    if (!slot) return nullptr;
    return &m_pool[m_table[*slot]].order;
}

const RestingOrder* BandBook::BestFront(Side side) const
{
    const std::optional<size_t> at{BestIndex(side)};
    if (!at) return nullptr;
    return &m_pool[SideOf(side).levels[*at].head].order;
}

Result<uint64_t> BandBook::LevelLots(Side side, uint64_t price_ticks) const
{
    if (!InBand(price_ticks)) return Ok(uint64_t{0});
    return FitU64(PointLots(SideOf(side), IndexOf(price_ticks)), "level lots above 2^64-1");
}

Result<uint64_t> BandBook::DepthThrough(Side side, uint64_t price_ticks) const
{
    const SideBook& s{SideOf(side)};
    const U128 total{Prefix(s, BAND_TICKS - 1)};
    U128 depth{0};
    if (side != Side::BUY) {
        // Asks at or below the price.
        if (price_ticks < m_limits.band_low_ticks) {
            depth = 0;
        } else if (!InBand(price_ticks)) {
            depth = total;
        } else {
            depth = Prefix(s, IndexOf(price_ticks));
        }
    } else {
        // Bids at or above the price.
        if (price_ticks <= m_limits.band_low_ticks) {
            depth = total;
        } else if (!InBand(price_ticks)) {
            depth = 0;
        } else {
            depth = total - Prefix(s, IndexOf(price_ticks) - 1);
        }
    }
    return FitU64(depth, "depth above 2^64-1");
}

size_t BandBook::LevelCount(Side side) const { return SideOf(side).level_count; }

std::vector<RestingOrder> BandBook::LevelOrders(Side side, uint64_t price_ticks) const
{
    std::vector<RestingOrder> out;
    if (!InBand(price_ticks)) return out;
    for (uint32_t n{SideOf(side).levels[IndexOf(price_ticks)].head}; n != NONE; n = m_pool[n].next) {
        out.push_back(m_pool[n].order);
    }
    return out;
}

std::vector<RestingOrder> BandBook::Orders() const
{
    std::vector<RestingOrder> out;
    out.reserve(m_size);
    const auto append_level{[&](const SideBook& s, size_t index) {
        for (uint32_t n{s.levels[index].head}; n != NONE; n = m_pool[n].next) out.push_back(m_pool[n].order);
    }};
    // Bids from the highest price down.
    for (size_t w{WORDS}; w-- > 0;) {
        for (uint64_t bits{m_bids.words[w]}; bits != 0;) {
            const size_t bit{63 - static_cast<size_t>(std::countl_zero(bits))};
            append_level(m_bids, w * 64 + bit);
            bits &= ~(uint64_t{1} << bit);
        }
    }
    // Asks from the lowest price up.
    for (size_t w{0}; w < WORDS; ++w) {
        for (uint64_t bits{m_asks.words[w]}; bits != 0; bits &= bits - 1) {
            append_level(m_asks, w * 64 + static_cast<size_t>(std::countr_zero(bits)));
        }
    }
    return out;
}

// ---- Mutations -----------------------------------------------------------------

Result<void> BandBook::Rest(RestingOrder order)
{
    // OrderBook::Rest's refusals, in its order.
    if (!IsKnown(order.side)) return Invalid("unknown side");
    if (order.price_ticks == 0) return Invalid("resting order with price 0");
    if (order.remaining_lots == 0) return Invalid("resting order with 0 lots");
    if (Slot(order.id).has_value()) return Invalid("order id already resting");
    const Result<Reservation> required{RequiredReservation(m_market, order.side, order.price_ticks, order.remaining_lots)};
    if (!required.has_value()) return required.error();
    if (order.reserved_left != required->amount) return Invalid("resting order is not exactly reserved");
    if (!InAtomRange(order.cum_quote) || !InAtomRange(order.fee_charged) || order.fee_charged > order.cum_quote ||
        (order.side == Side::BUY && order.fee_charged != 0)) {
        return Invalid("resting order quote or fee outside its range");
    }
    if (WouldCross(order.side, order.price_ticks)) return Invalid("resting order would cross the book");
    // A price outside the band has no level, so OrderBook's rank refusal
    // cannot apply to it; the band refusal follows.
    if (!InBand(order.price_ticks)) return Invalid(BAND_REFUSED);
    SideBook& s{MutableSide(order.side)};
    const size_t index{IndexOf(order.price_ticks)};
    Level& level{s.levels[index]};
    if (level.tail != NONE && m_pool[level.tail].order.arrival_rank >= order.arrival_rank) {
        return Invalid("arrival rank not above the level's last order");
    }
    if (m_size >= m_limits.order_capacity) return Error{ErrorCode::OVERSIZE, BOOK_FULL};

    // Commit: nothing below can fail (the pool never exceeds its capacity,
    // reserved once it is reached).
    uint32_t node{m_free};
    if (node != NONE) {
        m_free = m_pool[node].next;
    } else {
        node = static_cast<uint32_t>(m_pool.size());
        m_pool.emplace_back();
    }
    const U128 lots{order.remaining_lots};
    m_pool[node] = Node{std::move(order), level.tail, NONE};
    if (level.tail == NONE) {
        level.head = node;
        SetBit(s, index);
    } else {
        m_pool[level.tail].next = node;
    }
    level.tail = node;
    FenwickAdd(s, index, lots);
    TableInsert(node);
    ++m_size;
    return Ok();
}

void BandBook::Unlink(uint32_t node, U128 lots_to_remove)
{
    Node& n{m_pool[node]};
    SideBook& s{MutableSide(n.order.side)};
    const size_t index{IndexOf(n.order.price_ticks)};
    Level& level{s.levels[index]};
    if (n.prev == NONE) {
        level.head = n.next;
    } else {
        m_pool[n.prev].next = n.next;
    }
    if (n.next == NONE) {
        level.tail = n.prev;
    } else {
        m_pool[n.next].prev = n.prev;
    }
    if (level.head == NONE) ClearBit(s, index);
    FenwickSub(s, index, lots_to_remove);
    TableErase(*Slot(n.order.id));
    n.prev = NONE;
    n.next = m_free;
    m_free = node;
    --m_size;
}

Result<RestingOrder> BandBook::Remove(const OrderId& id)
{
    const std::optional<size_t> slot{Slot(id)};
    if (!slot) return Invalid("order is not resting");
    const uint32_t node{m_table[*slot]};
    RestingOrder removed{m_pool[node].order};
    Unlink(node, U128{removed.remaining_lots});
    return Ok(std::move(removed));
}

Result<FrontFill> BandBook::FillBestFront(Side side, uint64_t lots, Atoms quote, Atoms fee)
{
    if (!IsKnown(side)) return Invalid("unknown side");
    const std::optional<size_t> best{BestIndex(side)};
    if (!best) return Invalid("no resting order on that side");
    SideBook& s{MutableSide(side)};
    const uint32_t node{s.levels[*best].head};
    RestingOrder& front{m_pool[node].order};
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
        Unlink(node, U128{lots});
    } else {
        FenwickSub(s, *best, U128{lots});
    }
    return Ok(std::move(result));
}

Result<void> BandBook::CheckInvariants() const
{
    if (m_pool.size() > m_limits.order_capacity || m_size > m_pool.size()) return Broken("pool above its capacity");
    if (m_table.size() < size_t{2} * m_limits.order_capacity || !std::has_single_bit(m_table.size())) {
        return Broken("table smaller than twice the capacity");
    }
    size_t counted{0};
    for (const Side side : {Side::BUY, Side::SELL}) {
        const SideBook& s{SideOf(side)};
        if (s.levels.size() != BAND_TICKS || s.words.size() != WORDS || s.summary.size() != SUMMARY_WORDS ||
            s.fenwick.size() != BAND_TICKS + 1) {
            return Broken("side arrays of the wrong size");
        }
        std::vector<U128> rebuilt(BAND_TICKS + 1, U128{0});
        size_t levels{0};
        for (size_t index{0}; index < BAND_TICKS; ++index) {
            const Level& level{s.levels[index]};
            const bool bit{((s.words[index / 64] >> (index % 64)) & 1) != 0};
            if (bit != (level.head != NONE) || (level.head == NONE) != (level.tail == NONE)) {
                return Broken("level bit disagrees with its list");
            }
            if (level.head == NONE) continue;
            ++levels;
            const uint64_t price{PriceOf(index)};
            U128 lots{0};
            uint32_t prev{NONE};
            uint64_t last_rank{0};
            size_t steps{0};
            for (uint32_t n{level.head}; n != NONE; prev = n, n = m_pool[n].next) {
                if (n >= m_pool.size() || ++steps > m_size) return Broken("level list leaves the pool");
                const Node& node{m_pool[n]};
                const RestingOrder& order{node.order};
                if (node.prev != prev) return Broken("level list links disagree");
                if (order.side != side || order.price_ticks != price) return Broken("order on the wrong level");
                if (order.remaining_lots == 0) return Broken("resting order with price or lots 0");
                if (prev != NONE && order.arrival_rank <= last_rank) return Broken("level not in arrival order");
                last_rank = order.arrival_rank;
                const Result<Reservation> required{RequiredReservation(m_market, side, price, order.remaining_lots)};
                if (!required.has_value() || required->amount != order.reserved_left) {
                    return Broken("resting order is not exactly reserved");
                }
                if (!InAtomRange(order.cum_quote) || !InAtomRange(order.fee_charged) ||
                    order.fee_charged > order.cum_quote || (side == Side::BUY && order.fee_charged != 0)) {
                    return Broken("resting order quote or fee outside its range");
                }
                const std::optional<size_t> slot{Slot(order.id)};
                if (!slot || m_table[*slot] != n) return Broken("table disagrees with the levels");
                lots += U128{order.remaining_lots};
                ++counted;
            }
            if (prev != level.tail) return Broken("level tail is not its last order");
            rebuilt[index + 1] = lots;
        }
        if (levels != s.level_count) return Broken("level count disagrees with the bitmap");
        for (size_t w{0}; w < WORDS; ++w) {
            const bool bit{((s.summary[w / 64] >> (w % 64)) & 1) != 0};
            if (bit != (s.words[w] != 0)) return Broken("summary bit disagrees with its word");
        }
        // Build the Fenwick tree of the level sums in O(BAND_TICKS) and compare.
        for (size_t i{1}; i <= BAND_TICKS; ++i) {
            const size_t parent{i + (i & (~i + 1))};
            if (parent <= BAND_TICKS) rebuilt[parent] += rebuilt[i];
        }
        if (rebuilt != s.fenwick) return Broken("Fenwick tree disagrees with the levels");
    }
    if (counted != m_size) return Broken("size disagrees with the levels");
    size_t free_nodes{0};
    for (uint32_t n{m_free}; n != NONE; n = m_pool[n].next) {
        if (n >= m_pool.size() || ++free_nodes > m_pool.size()) return Broken("free list leaves the pool");
    }
    if (free_nodes + m_size != m_pool.size()) return Broken("pool nodes neither resting nor free");
    const std::optional<uint64_t> bid{BestBid()};
    const std::optional<uint64_t> ask{BestAsk()};
    if (bid.has_value() && ask.has_value() && *bid >= *ask) return Broken("book is crossed");
    return Ok();
}

bool operator==(const BandBook& a, const BandBook& b)
{
    return a.m_market == b.m_market && a.m_id == b.m_id && a.m_limits == b.m_limits && a.Orders() == b.Orders();
}

} // namespace flowmesh2::engine
