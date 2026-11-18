// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/engine/matcher.h>

#include <flowmesh2/core/fee_math.h>
#include <util/int128.h>

#include <algorithm>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <utility>

namespace flowmesh2::engine {

namespace {

constexpr Error Invalid(const char* detail) { return Error{ErrorCode::INVALID_INPUT, detail}; }
constexpr Error Broken(const char* detail) { return Error{ErrorCode::INVARIANT, detail}; }

//! Reserved atoms that this sweep's orders claim, per (owner, asset).
using Claims = std::map<std::pair<AccountId, AssetId>, Atoms>;

Result<void> AddClaim(Claims& claims, const AccountId& owner, const AssetId& asset, Atoms amount)
{
    Atoms& claimed{claims[{owner, asset}]};
    const Result<Atoms> sum{AddAtoms(claimed, amount)};
    if (!sum.has_value()) return Invalid("reservations exceed the ledger's reserved balance");
    claimed = *sum;
    return Ok();
}

//! Every precondition of SweepA2 (see engine/matcher.h); nothing is changed.
Result<void> CheckSweepInputs(const OrderBook& book, const ledger::LedgerState& ledger, std::span<const Taker> takers,
                              const ProfileParams& profile)
{
    const Result<void> valid{Validate(profile)};
    if (!valid.has_value()) return valid.error();
    if (profile.matching != MatchingProfile::A2_PRORATA) return Invalid("matching profile is not A2_PRORATA");
    if (!(ledger.Profile() == profile)) return Invalid("matcher profile differs from ledger profile");

    const MarketDescriptor& market{book.Market()};
    if (const Result<void> policy{ValidateMarketForProfile(market, profile)}; !policy.has_value()) return policy.error();
    std::set<OrderId> ids;
    std::set<uint64_t> ranks;
    Claims claims;
    for (const Taker& t : takers) {
        if (!IsKnown(t.side)) return Invalid("taker with an unknown side");
        if (t.tif != TimeInForce::GTC && t.tif != TimeInForce::IOC) return Invalid("taker time in force is not GTC or IOC");
        if (t.limit_ticks == 0 || t.remaining_lots == 0) return Invalid("taker with limit or lots 0");
        if (t.fills != 0 || t.cum_quote != 0 || t.fee_charged != 0 || t.stop != TakerStop::PENDING) {
            return Invalid("taker already swept");
        }
        if (!ids.insert(t.id).second) return Invalid("two takers share an OrderId");
        if (!ranks.insert(t.rank).second) return Invalid("two takers share a rank");
        if (book.Find(t.id) != nullptr) return Invalid("taker OrderId rests in the book");
        if (!ledger.HasAccount(t.owner) || ledger.IsProtocolAccount(t.owner)) return Invalid("taker owner has no trading account");
        const Result<Reservation> need{RequiredReservation(market, t.side, t.limit_ticks, t.remaining_lots)};
        if (!need.has_value()) return need.error();
        if (!InAtomRange(t.reserved_left) || t.reserved_left < need->amount) return Invalid("taker reservation below its order");
        const Result<void> claimed{AddClaim(claims, t.owner, need->asset, t.reserved_left)};
        if (!claimed.has_value()) return claimed.error();
    }

    std::map<AccountId, uint64_t> makers_per_owner;
    for (const RestingOrder& maker : book.Orders()) {
        if (!ledger.HasAccount(maker.owner)) return Invalid("maker owner has no account");
        ++makers_per_owner[maker.owner];
        const AssetId& asset{maker.side == Side::BUY ? market.quote : market.base};
        const Result<void> claimed{AddClaim(claims, maker.owner, asset, maker.reserved_left)};
        if (!claimed.has_value()) return claimed.error();
    }
    for (const auto& [owner, count] : makers_per_owner) {
        if (ledger.Find(owner)->open_orders < count) return Invalid("maker owner has fewer open orders than its makers");
    }
    for (const auto& [key, amount] : claims) {
        if (ledger.BalanceOf(key.first, key.second).reserved < amount) {
            return Invalid("reservations exceed the ledger's reserved balance");
        }
    }
    return Ok();
}

//! One sweep of one market: the state the fill loop works on.
class Sweeper
{
public:
    Sweeper(OrderBook& book, ledger::LedgerState& ledger, std::span<Taker> takers, const ProfileParams& profile,
            SlotFillBudget& budget, SweepResult& result)
        : m_book{book}, m_ledger{ledger}, m_takers{takers}, m_profile{profile}, m_budget{budget}, m_result{result} {}

    //! Steps 1-4 of the matching rule for the takers of one side.
    Result<void> SweepSide(Side side)
    {
        std::vector<size_t> ranked;
        for (size_t i{0}; i < m_takers.size(); ++i) {
            if (m_takers[i].side == side) ranked.push_back(i);
        }
        std::sort(ranked.begin(), ranked.end(), [&](size_t a, size_t b) { return m_takers[a].rank < m_takers[b].rank; });

        const Side maker_side{Opposite(side)};
        const uint64_t cap{m_profile.max_fills_per_order};
        // TODO(rebirth:M3-ENG-04) slot fill budget across markets and sides (BUY before SELL); SLOT_FULL.
        while (m_budget.remaining > 0) {
            const std::optional<uint64_t> best{m_book.Best(maker_side)};
            if (!best.has_value()) return Ok();
            std::vector<size_t> eligible;
            std::vector<uint64_t> demands;
            std::vector<uint64_t> ranks;
            for (const size_t i : ranked) {
                const Taker& t{m_takers[i]};
                if (t.remaining_lots > 0 && t.fills < cap && Reaches(side, t.limit_ticks, *best)) {
                    eligible.push_back(i);
                    demands.push_back(t.remaining_lots);
                    ranks.push_back(t.rank);
                }
            }
            if (eligible.empty()) return Ok();
            const Result<uint64_t> supply{m_book.LevelLots(maker_side, *best)};
            if (!supply.has_value()) return supply.error();
            if (*supply == 0) return Broken("best level holds no lots");
            const Result<std::vector<uint64_t>> allocation{AllocateProRata(*supply, demands, ranks)};
            if (!allocation.has_value()) return allocation.error();

            // TODO(rebirth:M3-ENG-14) pairing: takers in ascending canonical rank consume the level's makers FIFO.
            const size_t fills_before{m_result.fills.size()};
            for (size_t k{0}; k < eligible.size(); ++k) {
                Taker& taker{m_takers[eligible[k]]};
                uint64_t need{(*allocation)[k]};
                while (need > 0 && taker.fills < cap) {
                    if (m_budget.remaining == 0) return Ok();
                    const RestingOrder* front{m_book.BestFront(maker_side)};
                    if (front == nullptr || front->price_ticks != *best) return Broken("allocation exceeds the level");
                    const uint64_t lots{std::min(need, front->remaining_lots)};
                    const Result<void> filled{Execute(taker, lots)};
                    if (!filled.has_value()) return filled.error();
                    need -= lots;
                }
            }
            // Every round fills at least one lot (the allocation sums to
            // min(S, D) >= 1 and every eligible taker is below its cap), so a
            // round without a fill means a broken invariant: stop instead of looping.
            if (m_result.fills.size() == fills_before) return Broken("sweep round made no fill");
        }
        return Ok();
    }

private:
    //! One fill of `lots` between `taker` and the front maker of the other side, at the maker's price.
    Result<void> Execute(Taker& taker, uint64_t lots)
    {
        const Side maker_side{Opposite(taker.side)};
        const RestingOrder maker{*m_book.BestFront(maker_side)}; // copied before the book changes
        const MarketDescriptor& market{m_book.Market()};
        const Result<Atoms> quote{QuoteAtoms(market, lots, maker.price_ticks)};
        if (!quote.has_value()) return quote.error();
        const Result<Atoms> base{BaseAtoms(market, lots)};
        if (!base.has_value()) return base.error();

        // D5: the seller pays, on its order's cumulative quote (core/fee_math.h).
        const bool taker_buys{taker.side == Side::BUY};
        const Atoms seller_before{taker_buys ? maker.cum_quote : taker.cum_quote};
        const Result<Atoms> seller_after{AddAtoms(seller_before, *quote)};
        if (!seller_after.has_value()) return seller_after.error();
        const Result<Atoms> fee{SellerFeeIncrement(seller_before, *seller_after, m_profile.fee_ppm)};
        if (!fee.has_value()) return fee.error();

        // The taker's new values, checked before anything moves.
        const Result<Atoms> taker_left{SubAtoms(taker.reserved_left, taker_buys ? *quote : *base)};
        if (!taker_left.has_value()) return Broken("taker reservation below its fill");
        const Result<Atoms> taker_cum{AddAtoms(taker.cum_quote, *quote)};
        if (!taker_cum.has_value()) return taker_cum.error();
        const Result<Atoms> taker_fee{AddAtoms(taker.fee_charged, taker_buys ? Atoms{0} : *fee)};
        if (!taker_fee.has_value()) return taker_fee.error();
        const Result<Atoms> fees{AddAtoms(m_result.fees, *fee)};
        if (!fees.has_value()) return fees.error();
        if (m_budget.remaining == 0) return Broken("fill without slot budget");

        const AccountId& buyer{taker_buys ? taker.owner : maker.owner};
        const AccountId& seller{taker_buys ? maker.owner : taker.owner};
        // TODO(rebirth:M3-ENG-13) self-trade prevention: a maker and taker of one account trade like any other pair (O-17).
        const Result<void> base_leg{m_ledger.PayFromReserved(seller, buyer, market.base, *base, 0)};
        if (!base_leg.has_value()) return base_leg.error();
        const Result<void> quote_leg{m_ledger.PayFromReserved(buyer, seller, market.quote, *quote, *fee)};
        if (!quote_leg.has_value()) return quote_leg.error();
        const Result<FrontFill> maker_fill{m_book.FillBestFront(maker_side, lots, *quote, taker_buys ? *fee : Atoms{0})};
        if (!maker_fill.has_value()) return maker_fill.error();
        if (maker_fill->closed) {
            const Result<void> closed{m_ledger.RemoveOpenOrder(maker.owner)};
            if (!closed.has_value()) return closed.error();
            m_result.closed_makers.push_back(maker.id);
        }

        taker.remaining_lots -= lots;
        taker.reserved_left = *taker_left;
        taker.cum_quote = *taker_cum;
        taker.fee_charged = *taker_fee;
        taker.fills += 1;
        m_budget.remaining -= 1;
        m_result.fees = *fees;
        m_result.fills.push_back(ExecutedFill{
            Fill{maker.id, taker.id, m_book.Id(), taker.side, maker.price_ticks, lots, *quote, *fee, seller_before},
            maker.owner, taker.owner, *base});
        return Ok();
    }

    OrderBook& m_book;
    ledger::LedgerState& m_ledger;
    std::span<Taker> m_takers;
    const ProfileParams& m_profile;
    SlotFillBudget& m_budget;
    SweepResult& m_result;
};

} // namespace

bool IsKnown(TakerStop stop)
{
    switch (stop) {
    case TakerStop::PENDING:
    case TakerStop::FILLED:
    case TakerStop::NO_CROSS:
    case TakerStop::ORDER_FILL_CAP:
    case TakerStop::SLOT_FILL_CAP:
        return true;
    }
    return false;
}

std::string_view ToString(TakerStop stop)
{
    switch (stop) {
    case TakerStop::PENDING: return "PENDING";
    case TakerStop::FILLED: return "FILLED";
    case TakerStop::NO_CROSS: return "NO_CROSS";
    case TakerStop::ORDER_FILL_CAP: return "ORDER_FILL_CAP";
    case TakerStop::SLOT_FILL_CAP: return "SLOT_FILL_CAP";
    }
    return "UNKNOWN";
}

Result<std::vector<uint64_t>> AllocateProRata(uint64_t supply, std::span<const uint64_t> demands,
                                              std::span<const uint64_t> ranks)
{
    if (demands.size() != ranks.size()) return Invalid("demands and ranks differ in size");
    uint64_t total{0};
    std::set<uint64_t> seen;
    for (size_t i{0}; i < demands.size(); ++i) {
        if (demands[i] == 0) return Invalid("zero demand");
        if (!seen.insert(ranks[i]).second) return Invalid("repeated rank");
        if (demands[i] > std::numeric_limits<uint64_t>::max() - total) {
            return Error{ErrorCode::ARITHMETIC_OVERFLOW, "demands above 2^64-1"};
        }
        total += demands[i];
    }
    std::vector<uint64_t> allocation(demands.begin(), demands.end());
    if (total <= supply) return Ok(std::move(allocation));

    // supply < total: floor(supply * d_i / total) each, exact in 128 bits;
    // each share is below its demand, so one extra lot never exceeds it.
    std::vector<uint64_t> remainder(demands.size());
    uint64_t given{0};
    for (size_t i{0}; i < demands.size(); ++i) {
        const util::Unsigned128 product{util::Unsigned128{supply} * util::Unsigned128{demands[i]}};
        allocation[i] = static_cast<uint64_t>(product / util::Unsigned128{total});
        remainder[i] = static_cast<uint64_t>(product % util::Unsigned128{total});
        given += allocation[i];
    }
    const uint64_t leftover{supply - given};
    if (leftover > demands.size()) return Broken("pro-rata leftover above the number of takers");
    std::vector<size_t> order(demands.size());
    std::iota(order.begin(), order.end(), size_t{0});
    // Largest remainder first; ties to the lower rank (ranks are unique, so this is a total order).
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (remainder[a] != remainder[b]) return remainder[a] > remainder[b];
        return ranks[a] < ranks[b];
    });
    for (uint64_t k{0}; k < leftover; ++k) allocation[order[k]] += 1;
    return Ok(std::move(allocation));
}

Result<SweepResult> SweepA2(OrderBook& book, ledger::LedgerState& ledger, std::span<Taker> takers,
                            const ProfileParams& profile, SlotFillBudget& budget)
{
    const Result<void> inputs{CheckSweepInputs(book, ledger, takers, profile)};
    if (!inputs.has_value()) return inputs.error();

    SweepResult result;
    Sweeper sweeper{book, ledger, takers, profile, budget, result};
    for (const Side side : {Side::BUY, Side::SELL}) {
        const Result<void> swept{sweeper.SweepSide(side)};
        if (!swept.has_value()) return swept.error();
    }

    // Stops, from the final book.
    for (Taker& t : takers) {
        if (t.remaining_lots == 0) {
            t.stop = TakerStop::FILLED;
        } else if (!book.WouldCross(t.side, t.limit_ticks)) {
            t.stop = TakerStop::NO_CROSS;
        } else if (t.fills >= profile.max_fills_per_order) {
            t.stop = TakerStop::ORDER_FILL_CAP;
        } else if (budget.remaining == 0) {
            t.stop = TakerStop::SLOT_FILL_CAP;
        } else {
            return Broken("taker stopped while the book still crosses");
        }
    }
    return Ok(std::move(result));
}

Result<bool> PriceBandCheck(const MarketDescriptor&, uint64_t, const ReferencePrice&)
{
    return NotImplemented("M3-ENG-03"); // TODO(rebirth:M3-ENG-03)
}

} // namespace flowmesh2::engine
