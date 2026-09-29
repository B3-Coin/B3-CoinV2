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

ClearingTickPage PageClearingTicks(const std::deque<MarketHistoryEntry>& history, const ClearingTickQuery& query)
{
    ClearingTickPage out;
    if (query.limit == 0 || query.limit > MARKET_DATA_MAX_TICKS) {
        out.unavailable_reason = "invalid_limit";
        return out;
    }
    out.available = true;
    if (history.empty()) return out;
    out.oldest_retained_sequence = history.front().sequence;
    auto first{history.end() - std::min(query.limit, history.size())};
    if (query.after_sequence) {
        const uint64_t after{*query.after_sequence};
        first = std::find_if(history.begin(), history.end(), [&](const auto& entry) { return entry.sequence > after; });
        out.gap = after < std::numeric_limits<uint64_t>::max() && after + 1 < *out.oldest_retained_sequence;
    }
    for (auto it{first}; it != history.end(); ++it) {
        if (out.ticks.size() == query.limit) {
            out.more = true;
            break;
        }
        out.ticks.push_back(MakeClearingTick(*it));
    }
    return out;
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

} // namespace flowmesh
