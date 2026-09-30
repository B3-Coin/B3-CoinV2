// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/market_view.h>

#include <flowmesh/microblock.h>
#include <flowmesh/state.h>
#include <util/int128.h>

#include <algorithm>
#include <limits>
#include <map>
#include <vector>

namespace flowmesh {
namespace {

using Side = ClearingEngine::Side;
using Breakpoint = ClearingEngine::Breakpoint;

//! One curve as the book holds it, borrowed from the caller's span.
struct Item {
    Side side{Side::BID};
    AccountId account;
    const std::vector<Breakpoint>* points{nullptr};
    CAmount filled{0};
};

//! ClearingEngine::CurveIsValid's rules at the hard protocol cap (the market
//! bound can only be tighter), plus a sane fill. Verified state satisfies
//! them; anything else makes the view unavailable rather than misleading.
bool WellFormed(const Side side, const std::vector<Breakpoint>& points, const CAmount filled)
{
    if (points.empty() || points.size() > HARD_MAX_CURVE_POINTS || filled < 0 || filled > MAX_MONEY) return false;
    for (size_t i{0}; i < points.size(); ++i) {
        if (points[i].qty < 0 || points[i].qty > MAX_MONEY || points[i].price < 0 || points[i].price > MAX_MONEY) return false;
        if (i > 0 && (points[i - 1].price >= points[i].price ||
                      (side == Side::BID ? points[i].qty > points[i - 1].qty : points[i].qty < points[i - 1].qty))) {
            return false;
        }
    }
    return side == Side::BID ? points.front().qty > 0 && points.back().qty == 0 : points.back().qty > 0;
}

class Work
{
    size_t m_left;
    bool m_exhausted{false};
public:
    explicit Work(const size_t budget) : m_left{budget} {}
    bool Spend(const size_t units)
    {
        if (m_exhausted || units > m_left) {
            m_exhausted = true;
            return false;
        }
        m_left -= units;
        return true;
    }
    bool Exhausted() const { return m_exhausted; }
};

//! Remaining quantity at a price, exactly as SideTotal128 and Allocate.
CAmount Effective(const Item& item, const CAmount price)
{
    return std::max<CAmount>(0, ClearingEngine::EvaluateCurve(*item.points, price) - item.filled);
}

//! The highest integer price with remaining bid quantity, if any.
std::optional<CAmount> HighestBid(const Item& item, Work& work)
{
    const auto& points{*item.points};
    if (points.front().qty <= item.filled) return std::nullopt;
    // A bid ends at zero, so the last breakpoint above the fill is not the last.
    size_t k{0};
    for (size_t i{1}; i < points.size(); ++i) {
        if (points[i].qty > item.filled) k = i;
    }
    if (k + 1 >= points.size()) return std::nullopt;
    CAmount low{points[k].price}, high{points[k + 1].price - 1};
    while (low < high) {
        const CAmount middle{low + (high - low + 1) / 2};
        if (!work.Spend(1)) return std::nullopt;
        if (Effective(item, middle) > 0) low = middle;
        else high = middle - 1;
    }
    return low;
}

//! The lowest integer price with remaining ask quantity, if any. An ask
//! supplies its first quantity at every price down to zero.
std::optional<CAmount> LowestAsk(const Item& item, Work& work)
{
    const auto& points{*item.points};
    if (points.back().qty <= item.filled) return std::nullopt;
    if (points.front().qty > item.filled) return CAmount{0};
    size_t k{1};
    while (points[k].qty <= item.filled) ++k;
    CAmount low{points[k - 1].price + 1}, high{points[k].price};
    while (low < high) {
        const CAmount middle{low + (high - low) / 2};
        if (!work.Spend(1)) return std::nullopt;
        if (Effective(item, middle) > 0) high = middle;
        else low = middle + 1;
    }
    return low;
}

} // namespace

ImbalanceSide ObserveImbalanceSide(const ClearingEngine::ClearingResult& clearing, const FlowMeshState& post)
{
    if (!clearing.cleared) return ImbalanceSide::NONE;
    util::Signed128 bid{0}, ask{0};
    for (const auto& [account, fill] : clearing.bid_fill) {
        bid += post.EffectiveQty(ClearingEngine::Side::BID, account, clearing.price);
    }
    for (const auto& [account, fill] : clearing.ask_fill) {
        ask += post.EffectiveQty(ClearingEngine::Side::ASK, account, clearing.price);
    }
    if (bid == 0 && ask == 0) return clearing.imbalance == 0 ? ImbalanceSide::NONE : ImbalanceSide::UNKNOWN;
    // Both sides cannot keep quantity at the price after a maximal clearing.
    if (bid > 0 && ask > 0) return ImbalanceSide::UNKNOWN;
    const util::Signed128 residual{bid > 0 ? bid : ask};
    const util::Signed128 committed{clearing.imbalance};
    // The committed imbalance saturates at MAX_MONEY, so a larger residual
    // is consistent only with a saturated value.
    const bool consistent{clearing.imbalance < MAX_MONEY ? residual == committed : residual >= committed};
    if (!consistent) return ImbalanceSide::UNKNOWN;
    return bid > 0 ? ImbalanceSide::BID : ImbalanceSide::ASK;
}

namespace {
template <typename Entry, typename ToTick>
ClearingTickPage Page(const std::deque<Entry>& items, const ClearingTickQuery& query, ToTick to_tick)
{
    ClearingTickPage out;
    if (query.limit == 0 || query.limit > MARKET_DATA_MAX_TICKS) {
        out.unavailable_reason = "invalid_limit";
        return out;
    }
    out.available = true;
    if (items.empty()) return out;
    out.oldest_retained_sequence = to_tick(items.front()).sequence;
    auto first{items.end() - std::min(query.limit, items.size())};
    if (query.after_sequence) {
        const uint64_t after{*query.after_sequence};
        first = std::find_if(items.begin(), items.end(), [&](const auto& item) { return to_tick(item).sequence > after; });
        out.gap = after < std::numeric_limits<uint64_t>::max() && after + 1 < *out.oldest_retained_sequence;
    }
    for (auto it{first}; it != items.end(); ++it) {
        if (out.ticks.size() == query.limit) {
            out.more = true;
            break;
        }
        out.ticks.push_back(to_tick(*it));
    }
    return out;
}
} // namespace

ClearingTickPage PageClearingTicks(const std::deque<MarketHistoryEntry>& history, const ClearingTickQuery& query)
{
    return Page(history, query, [](const MarketHistoryEntry& entry) { return MakeClearingTick(entry); });
}

ClearingTickPage PageClearingTicks(const std::deque<ClearingTick>& ticks, const ClearingTickQuery& query)
{
    return Page(ticks, query, [](const ClearingTick& tick) -> const ClearingTick& { return tick; });
}

IndicativeClearing ComputeIndicative(const std::span<const ClearingEngine::CurveView> curves, const IndicativeQuery& query,
                                     const size_t budget)
{
    IndicativeClearing out;
    out.draft = query.draft;
    const auto fail = [&](const char* reason) {
        IndicativeClearing failed;
        failed.draft = query.draft;
        failed.unavailable_reason = reason;
        return failed;
    };
    Work work{budget};
    std::vector<Item> items;
    items.reserve(curves.size() + 1);
    for (const auto& curve : curves) {
        if (!work.Spend(1 + curve.points.size())) return fail("work_budget");
        if (!WellFormed(curve.side, curve.points, curve.filled_quantity)) return fail("malformed_curve");
        items.push_back({curve.side, curve.account_id, &curve.points, curve.filled_quantity});
    }

    // The best standing prices, of the verified curves only.
    for (const auto& item : items) {
        const auto price{item.side == Side::BID ? HighestBid(item, work) : LowestAsk(item, work)};
        if (work.Exhausted()) return fail("work_budget");
        if (!price) continue;
        auto& best{item.side == Side::BID ? out.best_bid : out.best_ask};
        if (!best || (item.side == Side::BID ? *price > *best : *price < *best)) best = price;
    }

    std::optional<std::vector<Breakpoint>> draft_points;
    std::optional<size_t> draft_index;
    if (query.draft) {
        const auto& draft{*query.draft};
        draft_points = draft.side == Side::BID ? MakeLimitBidCurve(draft.price, draft.quantity)
                                               : MakeLimitAskCurve(draft.price, draft.quantity);
        if (!draft_points || !WellFormed(draft.side, *draft_points, 0)) return fail("invalid_draft");
        if (draft.account) {
            const auto it{std::find_if(items.begin(), items.end(), [&](const Item& item) {
                return item.side == draft.side && item.account == *draft.account;
            })};
            if (it != items.end()) {
                out.draft_replaces_curve = true;
                items.erase(it);
            }
        }
        items.push_back({draft.side, draft.account.value_or(AccountId{}), &*draft_points, 0});
        draft_index = items.size() - 1;
    }

    // Every breakpoint price is a candidate, exactly as CandidatePrices.
    std::vector<CAmount> prices;
    for (const auto& item : items) {
        for (const auto& point : *item.points) prices.push_back(point.price);
    }
    std::sort(prices.begin(), prices.end());
    prices.erase(std::unique(prices.begin(), prices.end()), prices.end());
    const size_t count{prices.size()};
    if (count == 0) {
        out.available = true;
        return out;
    }
    const auto index = [&](const CAmount price) {
        return static_cast<size_t>(std::lower_bound(prices.begin(), prices.end(), price) - prices.begin());
    };
    // Per side, a difference array over candidate indices: a curve is flat
    // at or below its first breakpoint and at or above its last, so only the
    // candidates strictly inside its span are evaluated.
    std::vector<util::Signed128> bid(count + 1, 0), ask(count + 1, 0);
    const auto add = [](std::vector<util::Signed128>& diff, const size_t from, const size_t to, const CAmount value) {
        diff[from] += value;
        diff[to + 1] -= value;
    };
    for (const auto& item : items) {
        auto& diff{item.side == Side::BID ? bid : ask};
        const auto& points{*item.points};
        const size_t first{index(points.front().price)}, last{index(points.back().price)};
        const CAmount low{std::max<CAmount>(0, points.front().qty - item.filled)};
        const CAmount high{std::max<CAmount>(0, points.back().qty - item.filled)};
        if (first == last) {
            add(diff, 0, count - 1, low);
            continue;
        }
        add(diff, 0, first, low);
        add(diff, last, count - 1, high);
        if (!work.Spend(last - first - 1)) return fail("work_budget");
        for (size_t j{first + 1}; j < last; ++j) add(diff, j, j, Effective(item, prices[j]));
    }

    // ClearSlot's selection: maximum volume, then minimum imbalance, then
    // the lowest price (ascending iteration keeps the first of equals).
    util::Signed128 demand{0}, supply{0}, best_volume{0}, best_imbalance{0}, best_demand{0}, best_supply{0};
    for (size_t j{0}; j < count; ++j) {
        demand += bid[j];
        supply += ask[j];
        const util::Signed128 volume{demand < supply ? demand : supply};
        if (volume <= 0) continue;
        const util::Signed128 imbalance{demand > supply ? demand - supply : supply - demand};
        if (!out.cleared || volume > best_volume || (volume == best_volume && imbalance < best_imbalance)) {
            // The engine treats a chosen volume beyond MAX_MONEY as fatal.
            if (volume > static_cast<util::Signed128>(MAX_MONEY)) return fail("overflow");
            out.cleared = true;
            out.price = prices[j];
            out.volume = static_cast<CAmount>(volume);
            out.imbalance = imbalance > static_cast<util::Signed128>(MAX_MONEY) ? MAX_MONEY : static_cast<CAmount>(imbalance);
            best_volume = volume;
            best_imbalance = imbalance;
            best_demand = demand;
            best_supply = supply;
        }
    }
    out.available = true;
    if (!out.cleared) {
        if (draft_index) out.draft_fill_min = out.draft_fill_max = CAmount{0};
        return out;
    }
    out.imbalance_side = best_demand > best_supply ? ImbalanceSide::BID
                         : best_demand < best_supply ? ImbalanceSide::ASK
                                                     : ImbalanceSide::NONE;
    if (!draft_index) return out;

    // The draft's fill: in full on the short side, else Allocate's
    // largest-remainder rationing in account order.
    const Item& draft{items[*draft_index]};
    const CAmount wanted{Effective(draft, out.price)};
    const util::Signed128 side_total{draft.side == Side::BID ? best_demand : best_supply};
    if (wanted == 0 || side_total <= static_cast<util::Signed128>(out.volume)) {
        out.draft_fill_min = out.draft_fill_max = wanted;
        return out;
    }
    const util::Unsigned128 total{static_cast<util::Unsigned128>(side_total)};
    const util::Unsigned128 product{static_cast<util::Unsigned128>(wanted) * static_cast<util::Unsigned128>(out.volume)};
    const CAmount floor{static_cast<CAmount>(product / total)};
    if (!query.draft->account) {
        out.draft_fill_min = floor;
        out.draft_fill_max = floor + (product % total != 0 ? 1 : 0);
        return out;
    }
    if (!work.Spend(items.size())) return fail("work_budget");
    struct Rem {
        AccountId account;
        util::Unsigned128 rem;
    };
    std::map<AccountId, CAmount> desired;
    for (const auto& item : items) {
        if (item.side != draft.side) continue;
        if (const CAmount q{Effective(item, out.price)}; q > 0) desired[item.account] = q;
    }
    std::vector<Rem> remainders;
    CAmount handed{0}, fill{0};
    for (const auto& [account, q] : desired) {
        const util::Unsigned128 num{static_cast<util::Unsigned128>(q) * static_cast<util::Unsigned128>(out.volume)};
        const CAmount base{static_cast<CAmount>(num / total)};
        handed += base;
        if (account == draft.account) fill = base;
        remainders.push_back({account, num % total});
    }
    CAmount leftover{out.volume - handed};
    std::stable_sort(remainders.begin(), remainders.end(), [](const Rem& a, const Rem& b) { return a.rem > b.rem; });
    for (size_t i{0}; leftover > 0 && i < remainders.size(); ++i, --leftover) {
        if (remainders[i].account == draft.account) ++fill;
    }
    out.draft_fill_min = out.draft_fill_max = fill;
    return out;
}

namespace {
using U128 = util::Unsigned128;

U128 Pow10(int exponent)
{
    U128 out{1};
    while (exponent-- > 0) out *= 10;
    return out;
}

CAmount Clamp(const U128 value)
{
    return value > static_cast<U128>(MAX_MONEY) ? MAX_MONEY : static_cast<CAmount>(value);
}

//! One display bucket: its index and its inclusive canonical price range.
struct Bucket {
    U128 index{0};
    bool zero_price{false};
    CAmount low{0}, high{0};
};

/** The buckets of a valid grouping (LevelGroupingError is empty). Every
 * canonical price of a side lies in exactly one bucket, and the buckets are
 * contiguous, disjoint ranges, so a bucket is identified by its low end. */
class Partition
{
    enum class Kind { EXACT, CANONICAL, INVERSE };
    Kind m_kind{Kind::EXACT};
    //! Canonical: ticks per bucket. Inverse: 10^s and the mantissa.
    CAmount m_ticks{1};
    U128 m_scale{1}, m_mantissa{1};

    static CAmount CeilDiv(const CAmount a, const CAmount b) { return a / b + (a % b != 0 ? 1 : 0); }
    static U128 CeilDiv(const U128 a, const U128 b) { return a / b + (a % b != 0 ? 1 : 0); }

public:
    explicit Partition(const LevelGrouping& grouping)
    {
        if (grouping.exact) return;
        if (!grouping.inverse) {
            m_kind = Kind::CANONICAL;
            m_ticks = static_cast<CAmount>(grouping.mantissa) * static_cast<CAmount>(Pow10(grouping.exponent + 9 - grouping.decimals));
        } else {
            m_kind = Kind::INVERSE;
            m_scale = Pow10(9 - grouping.decimals - grouping.exponent);
            m_mantissa = grouping.mantissa;
        }
    }

    //! A canonical bid at `price`: a displayed buy in the canonical view
    //! (floor, labelled with the lower edge), a displayed sell in the
    //! inverse view (ceil of u, labelled with the upper edge).
    Bucket Bid(const CAmount price) const
    {
        switch (m_kind) {
        case Kind::EXACT: return {static_cast<U128>(price), false, price, price};
        case Kind::CANONICAL: {
            const CAmount index{price / m_ticks}, low{index * m_ticks};
            return {static_cast<U128>(index), false, low, std::min<CAmount>(low + (m_ticks - 1), MAX_MONEY)};
        }
        case Kind::INVERSE: {
            if (price == 0) return {0, true, 0, 0};
            const U128 index{CeilDiv(m_scale, m_mantissa * static_cast<U128>(price))};
            const U128 low{CeilDiv(m_scale, m_mantissa * index)};
            const U128 high{index >= 2 ? CeilDiv(m_scale, m_mantissa * (index - 1)) - 1 : static_cast<U128>(MAX_MONEY)};
            return {index, false, Clamp(low), Clamp(high)};
        }
        }
        return {};
    }

    //! A canonical ask at `price`: a displayed sell in the canonical view
    //! (ceil, labelled with the upper edge), a displayed buy in the inverse
    //! view (floor of u, labelled with the lower edge).
    Bucket Ask(const CAmount price) const
    {
        switch (m_kind) {
        case Kind::EXACT: return {static_cast<U128>(price), false, price, price};
        case Kind::CANONICAL: {
            const CAmount index{CeilDiv(price, m_ticks)};
            const CAmount high{index * m_ticks};
            return {static_cast<U128>(index), false, index == 0 ? CAmount{0} : high - m_ticks + 1, std::min(high, MAX_MONEY)};
        }
        case Kind::INVERSE: {
            if (price == 0) return {0, true, 0, 0};
            const U128 index{m_scale / (m_mantissa * static_cast<U128>(price))};
            const U128 low{m_scale / (m_mantissa * (index + 1)) + 1};
            const U128 high{index >= 1 ? m_scale / (m_mantissa * index) : static_cast<U128>(MAX_MONEY)};
            return {index, false, Clamp(low), Clamp(high)};
        }
        }
        return {};
    }
};

/** The highest price below `bound` (every price when absent) at which a bid
 * curve has remaining quantity it does not also have at `bound`: the next
 * bid level, generalising HighestBid (T = filled + R(bound)). */
std::optional<CAmount> NextBidBelow(const Item& item, const std::optional<CAmount> bound, Work& work)
{
    const auto& points{*item.points};
    CAmount threshold{item.filled};
    if (bound) {
        if (*bound <= 0 || !work.Spend(1)) return std::nullopt;
        threshold = std::max(threshold, ClearingEngine::EvaluateCurve(points, *bound));
    }
    if (points.front().qty <= threshold) return std::nullopt;
    // Quantities never rise: the breakpoints above the threshold are a prefix.
    size_t k{0};
    for (size_t i{1}; i < points.size(); ++i) {
        if (points[i].qty > threshold) k = i;
    }
    if (k + 1 >= points.size()) return std::nullopt;
    CAmount low{points[k].price}, high{points[k + 1].price - 1};
    if (bound) high = std::min(high, *bound - 1);
    if (low > high) return std::nullopt;
    while (low < high) {
        const CAmount middle{low + (high - low + 1) / 2};
        if (!work.Spend(1)) return std::nullopt;
        if (ClearingEngine::EvaluateCurve(points, middle) > threshold) low = middle;
        else high = middle - 1;
    }
    return low;
}

/** The lowest price above `bound` (-1: every price) at which an ask curve
 * has remaining quantity it does not also have at `bound`: the next ask
 * level, generalising LowestAsk (T = filled + R(bound), R(-1) = 0). An ask
 * whose first quantity exceeds its fill offers it at price 0. */
std::optional<CAmount> NextAskAbove(const Item& item, const CAmount bound, Work& work)
{
    const auto& points{*item.points};
    CAmount threshold{item.filled};
    if (bound >= 0) {
        if (bound >= MAX_MONEY || !work.Spend(1)) return std::nullopt;
        threshold = std::max(threshold, ClearingEngine::EvaluateCurve(points, bound));
    }
    if (points.back().qty <= threshold) return std::nullopt;
    size_t k{0};
    while (points[k].qty <= threshold) ++k;
    CAmount low{std::max(k == 0 ? CAmount{0} : points[k - 1].price + 1, bound + 1)}, high{points[k].price};
    if (low > high) return std::nullopt;
    while (low < high) {
        const CAmount middle{low + (high - low) / 2};
        if (!work.Spend(1)) return std::nullopt;
        if (ClearingEngine::EvaluateCurve(points, middle) > threshold) high = middle;
        else low = middle + 1;
    }
    return low;
}

//! A curve's next level and the bucket that holds it.
struct Cursor {
    Bucket bucket;
    CAmount level{0};
    size_t item{0};
};

//! One side's merge of every curve's next bucket, best first.
class LevelMerge
{
    const std::vector<Item>& m_items;
    const Partition& m_partition;
    const bool m_bid;
    std::vector<Cursor> m_heap;
    U128 m_cumulative{0};
    bool m_done{false};

    bool Before(const Cursor& a, const Cursor& b) const { return m_bid ? a.bucket.low < b.bucket.low : a.bucket.low > b.bucket.low; }
    void Push(const Cursor& cursor)
    {
        m_heap.push_back(cursor);
        std::push_heap(m_heap.begin(), m_heap.end(), [&](const Cursor& a, const Cursor& b) { return Before(a, b); });
    }
    Cursor Pop()
    {
        std::pop_heap(m_heap.begin(), m_heap.end(), [&](const Cursor& a, const Cursor& b) { return Before(a, b); });
        auto out{m_heap.back()};
        m_heap.pop_back();
        return out;
    }
    //! R(p), with R(-1) = 0 for an ask.
    std::optional<CAmount> Remaining(const Item& item, const CAmount price, Work& work) const
    {
        if (price < 0) return CAmount{0};
        if (!work.Spend(1)) return std::nullopt;
        return std::max<CAmount>(0, ClearingEngine::EvaluateCurve(*item.points, price) - item.filled);
    }
    std::optional<CAmount> Next(const Item& item, const Bucket& from, Work& work) const
    {
        return m_bid ? NextBidBelow(item, from.low, work) : NextAskAbove(item, from.high, work);
    }
    void Stop(const char* reason)
    {
        m_done = true;
        if (side.levels.empty()) {
            side.available = false;
            side.unavailable_reason = reason;
        } else {
            side.truncated_reason = reason;
        }
    }

public:
    BookLevelsSide side;

    LevelMerge(const std::vector<Item>& items, const Partition& partition, const bool bid)
        : m_items{items}, m_partition{partition}, m_bid{bid}
    {
        side.available = true;
    }

    //! Adds every curve of this side at its best level; false when the
    //! budget ran out. `best` receives the side's exact best price.
    bool Start(Work& work, std::optional<CAmount>& best)
    {
        for (size_t i{0}; i < m_items.size(); ++i) {
            const Item& item{m_items[i]};
            if ((item.side == Side::BID) != m_bid) continue;
            const auto level{m_bid ? NextBidBelow(item, std::nullopt, work) : NextAskAbove(item, -1, work)};
            if (work.Exhausted()) return false;
            if (!level) continue;
            Push({m_bid ? m_partition.Bid(*level) : m_partition.Ask(*level), *level, i});
            if (!best || (m_bid ? *level > *best : *level < *best)) best = level;
        }
        return true;
    }

    bool Done() const { return m_done; }

    //! Emits the next level, or ends the side (depth, empty, budget, overflow).
    void Step(const size_t depth, const std::optional<AccountId>& account, Work& work)
    {
        if (m_done) return;
        if (m_heap.empty()) {
            m_done = true;
            return;
        }
        if (side.levels.size() >= depth) {
            side.more = true;
            m_done = true;
            return;
        }
        std::vector<Cursor> members{Pop()};
        while (!m_heap.empty() && m_heap.front().bucket.low == members.front().bucket.low) members.push_back(Pop());
        const Bucket& bucket{members.front().bucket};
        BookLevel level;
        level.bucket = bucket.index;
        level.zero_price = bucket.zero_price;
        level.low = bucket.low;
        level.high = bucket.high;
        U128 size{0}, notional{0};
        bool notional_known{true};
        for (const auto& member : members) {
            const Item& item{m_items[member.item]};
            // The bucket's lots of this curve: R(lo) - R(hi + 1) for a bid,
            // R(hi) - R(lo - 1) for an ask.
            const auto inner{Remaining(item, m_bid ? bucket.low : bucket.high, work)};
            const auto outer{Remaining(item, m_bid ? bucket.high + 1 : bucket.low - 1, work)};
            if (!inner || !outer) return Stop("work_budget");
            const CAmount part{*inner - *outer};
            size += static_cast<U128>(part);
            if (size > static_cast<U128>(MAX_MONEY)) return Stop("overflow");
            ++level.curves;
            if (account && item.account == *account) level.own += part;
            if (!notional_known) continue;
            // Exact notional: the lots at each of this curve's prices in the
            // bucket, from its best one, within a bounded walk.
            const auto at_level{Remaining(item, member.level, work)};
            if (!at_level) return Stop("work_budget");
            if (*at_level == *inner) {
                notional += static_cast<U128>(part) * static_cast<U128>(member.level);
                continue;
            }
            CAmount price{member.level}, beyond{*outer}, here{*at_level};
            for (size_t prices{1};; ++prices) {
                notional += static_cast<U128>(here - beyond) * static_cast<U128>(price);
                const auto next{m_bid ? NextBidBelow(item, price, work) : NextAskAbove(item, price, work)};
                if (work.Exhausted()) return Stop("work_budget");
                if (!next || (m_bid ? *next < bucket.low : *next > bucket.high)) break;
                if (prices >= MARKET_VIEW_LEVEL_NOTIONAL_PRICES) {
                    notional_known = false;
                    break;
                }
                const auto remaining{Remaining(item, *next, work)};
                if (!remaining) return Stop("work_budget");
                beyond = here;
                here = *remaining;
                price = *next;
            }
        }
        m_cumulative += size;
        if (m_cumulative > static_cast<U128>(MAX_MONEY)) return Stop("overflow");
        level.size = static_cast<CAmount>(size);
        level.cumulative = static_cast<CAmount>(m_cumulative);
        if (notional_known) level.notional = notional;
        side.levels.push_back(std::move(level));
        for (const auto& member : members) {
            const auto next{Next(m_items[member.item], bucket, work)};
            if (work.Exhausted()) return Stop("work_budget");
            if (next) Push({m_bid ? m_partition.Bid(*next) : m_partition.Ask(*next), *next, member.item});
        }
    }
};
} // namespace

std::string LevelGroupingError(const LevelGrouping& grouping)
{
    if (grouping.exact) return {};
    if (grouping.mantissa != 1 && grouping.mantissa != 2 && grouping.mantissa != 5) return "mantissa must be 1, 2 or 5";
    if (grouping.decimals > 18) return "decimals must be 0-18";
    if (grouping.exponent < -64 || grouping.exponent > 64) return "exponent is outside its supported range";
    if (!grouping.inverse) {
        // A canonical price P shows as P x 10^(d - 9) B3 per token.
        const int power{grouping.exponent + 9 - grouping.decimals};
        if (power < 0 || power > 18 || static_cast<U128>(grouping.mantissa) * Pow10(power) > static_cast<U128>(MAX_MONEY)) {
            return "a canonical price step must be a whole number of price ticks up to the monetary maximum";
        }
        return {};
    }
    const int scale{9 - grouping.decimals - grouping.exponent};
    if (scale < 0 || scale > 36) return "an inverse price step must lie between 10^-36 and 1 times the exact inverse scale";
    return {};
}

std::optional<std::pair<CAmount, CAmount>> LevelBucketRange(const LevelGrouping& grouping, const bool bid, const U128 index,
                                                            const bool zero_price)
{
    if (!LevelGroupingError(grouping).empty()) return std::nullopt;
    const bool inverse{!grouping.exact && grouping.inverse};
    if (zero_price) return inverse ? std::optional{std::pair<CAmount, CAmount>{0, 0}} : std::nullopt;
    // No valid bucket index exceeds 10^36 (the largest inverse scale); the
    // bound keeps every product below within 128 bits.
    if (index > Pow10(37)) return std::nullopt;
    const U128 money{static_cast<U128>(MAX_MONEY)};
    U128 low, high;
    if (grouping.exact) {
        if (index > money) return std::nullopt;
        low = high = index;
    } else if (!inverse) {
        const U128 ticks{grouping.mantissa * Pow10(grouping.exponent + 9 - grouping.decimals)};
        if (bid) {
            low = index * ticks;
            high = low + ticks - 1;
        } else {
            high = index * ticks;
            low = index == 0 ? 0 : high - ticks + 1;
        }
    } else {
        const U128 scale{Pow10(9 - grouping.decimals - grouping.exponent)}, mantissa{grouping.mantissa};
        const auto ceil = [](const U128 a, const U128 b) { return a / b + (a % b != 0 ? 1 : 0); };
        if (bid) {
            if (index == 0) return std::nullopt;
            low = ceil(scale, mantissa * index);
            high = index >= 2 ? ceil(scale, mantissa * (index - 1)) - 1 : money;
        } else {
            low = scale / (mantissa * (index + 1)) + 1;
            high = index >= 1 ? scale / (mantissa * index) : money;
        }
        if (low == 0) low = 1; // Price zero is its own bucket.
    }
    if (high > money) high = money;
    if (low > high) return std::nullopt;
    return std::pair<CAmount, CAmount>{static_cast<CAmount>(low), static_cast<CAmount>(high)};
}

bool LevelsQueryValid(const BookLevelsQuery& query)
{
    return query.depth >= 1 && query.depth <= MARKET_DATA_MAX_LEVELS && LevelGroupingError(query.grouping).empty();
}

BookLevels ComputeBookLevels(const std::span<const ClearingEngine::CurveView> curves, const BookLevelsQuery& query,
                             const size_t budget)
{
    BookLevels out;
    out.depth = query.depth;
    out.grouping = query.grouping;
    out.total_curves = curves.size();
    const auto fail = [&](const char* reason) {
        BookLevels failed;
        failed.depth = query.depth;
        failed.grouping = query.grouping;
        failed.total_curves = curves.size();
        failed.unavailable_reason = reason;
        return failed;
    };
    if (!LevelsQueryValid(query)) return fail("invalid_grouping");
    Work work{budget};
    std::vector<Item> items;
    items.reserve(curves.size());
    for (const auto& curve : curves) {
        if (!work.Spend(1 + curve.points.size())) return fail("work_budget");
        if (!WellFormed(curve.side, curve.points, curve.filled_quantity)) return fail("malformed_curve");
        items.push_back({curve.side, curve.account_id, &curve.points, curve.filled_quantity});
    }
    const Partition partition{query.grouping};
    LevelMerge bids{items, partition, true}, asks{items, partition, false};
    if (!bids.Start(work, out.best_bid) || !asks.Start(work, out.best_ask)) return fail("work_budget");
    // Both sides share the budget, a level at a time.
    while (!bids.Done() || !asks.Done()) {
        bids.Step(query.depth, query.account, work);
        asks.Step(query.depth, query.account, work);
    }
    out.available = true;
    out.bids = std::move(bids.side);
    out.asks = std::move(asks.side);
    out.standing = ComputeIndicative(curves, IndicativeQuery{});
    return out;
}

BookCurves CopyBookCurves(const uint256& head, const BookPageReader& read, size_t& pages, const size_t max_curves)
{
    BookCurves out{.head = head, .curves = {}, .unavailable_reason = {}};
    const auto unavailable = [&](const char* reason) { return BookCurves{.head = head, .curves = {}, .unavailable_reason = reason}; };
    std::optional<ClearingEngine::CurveKey> after;
    for (;;) {
        auto page{read(after, MARKET_DATA_MAX_CURVES)};
        ++pages;
        if (!page) return unavailable("head_changed");
        if (page->total_curves > max_curves || page->curves.size() > max_curves - out.curves.size() ||
            (page->next_cursor && page->curves.empty())) {
            return unavailable("work_budget");
        }
        // Allocated by the caller of `read`, outside any lock it takes.
        if (out.curves.capacity() < page->total_curves) out.curves.reserve(page->total_curves);
        for (auto& curve : page->curves) out.curves.push_back(std::move(curve));
        if (!page->next_cursor) return out;
        after = page->next_cursor;
    }
}

bool LevelsWanted(const MarketData& data, const MarketDataQuery& query)
{
    return query.levels && (!data.unchanged || query.levels->include_unchanged);
}

namespace {
BookLevels Levels(const BookCurves* book, const BookLevelsQuery& query)
{
    const auto unavailable = [&](const std::string& reason) {
        BookLevels out;
        out.depth = query.depth;
        out.grouping = query.grouping;
        out.unavailable_reason = reason;
        if (book) out.head = book->head;
        return out;
    };
    if (!book) return unavailable("no_certified_state");
    if (!book->unavailable_reason.empty()) return unavailable(book->unavailable_reason);
    // Computed once per copy of the curves (so per head) and query.
    if (book->memo) {
        std::lock_guard<std::mutex> lock{book->memo->mutex};
        for (const auto& [key, levels] : book->memo->levels) {
            if (key == query) return *levels;
        }
    }
    auto levels{std::make_shared<BookLevels>(ComputeBookLevels(book->curves, query))};
    levels->head = book->head;
    levels->standing.head = book->head;
    if (book->memo) {
        std::lock_guard<std::mutex> lock{book->memo->mutex};
        auto& memo{book->memo->levels};
        if (memo.size() >= BookViewMemo::MAX_ENTRIES) memo.erase(memo.begin());
        memo.emplace_back(query, levels);
    }
    return *levels;
}
} // namespace

void ComputeMarketViews(MarketData& data, const MarketDataQuery& query)
{
    const auto book{std::move(data.book)};
    data.book.reset();
    if (LevelsWanted(data, query)) data.levels = Levels(book.get(), *query.levels);
    if (!query.indicative) return;
    if (!book) {
        auto& unavailable{data.indicative.emplace()};
        unavailable.draft = query.indicative->draft;
        unavailable.unavailable_reason = "no_certified_state";
        return;
    }
    if (!book->unavailable_reason.empty()) {
        auto& unavailable{data.indicative.emplace()};
        unavailable.draft = query.indicative->draft;
        unavailable.unavailable_reason = book->unavailable_reason;
        unavailable.head = book->head;
        return;
    }
    data.indicative = ComputeIndicative(book->curves, *query.indicative);
    data.indicative->head = book->head;
}

} // namespace flowmesh
