// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_TEST_UTIL_ENGINE_FIXTURES_H
#define B3_FLOWMESH2_TEST_UTIL_ENGINE_FIXTURES_H

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/engine/book.h>
#include <flowmesh2/engine/matcher.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

/** TEST fixtures of the engine tests (slice R6): one market's book over a
 * ledger, with helpers that fund accounts, rest makers and reserve takers
 * the way the engine (R7) will (phase 5 reserves; phase 6 rests).
 * Synthetic values only (ARCHITECTURE.md section 14.1).
 */
namespace flowmesh2::test {

//! OrderId{SHA256("fm2/test/order/" + name)}.
inline OrderId EngineOrder(std::string_view name) { return OrderId{LabelHash("fm2/test/order/" + std::string{name})}; }

inline engine::OrderBook EngineBook(const MarketDescriptor& market)
{
    Result<engine::OrderBook> book{engine::OrderBook::Create(market)};
    BOOST_REQUIRE(book.has_value());
    return std::move(book).value();
}

//! A book and a ledger of one market.
struct EngineFixture {
    ProfileParams profile;
    MarketDescriptor market;
    ledger::LedgerState ledger;
    engine::OrderBook book;
    uint64_t next_arrival{1};
    uint32_t next_outpoint{0};

    explicit EngineFixture(const MarketDescriptor& market_in, const ProfileParams& profile_in = RegtestProfile())
        : profile{profile_in}, market{market_in}, ledger{EmptyLedger(profile_in)}, book{EngineBook(market_in)} {}

    //! Credit a deposit fact of `amount` to `account` (creates it).
    void Fund(const AccountId& account, const AssetId& asset, Atoms amount)
    {
        RequireOk(ledger.CreditDeposit(Deposit(account, asset, amount, TestOutpoint("engine-fund", next_outpoint++))), "Fund");
    }
    //! Fund `account` with `amount` of both assets of the market.
    void FundBoth(const AccountId& account, Atoms amount)
    {
        Fund(account, market.base, amount);
        Fund(account, market.quote, amount);
    }

    /** Rest a maker the way phase 6 does: reserve its exact reservation, count
     * one open order, rest it with the next arrival rank.
     */
    engine::RestingOrder AddMaker(std::string_view name, const AccountId& owner, Side side, uint64_t price, uint64_t lots)
    {
        engine::RestingOrder order;
        order.id = EngineOrder(name);
        order.owner = owner;
        order.side = side;
        order.price_ticks = price;
        order.remaining_lots = lots;
        order.arrival_rank = next_arrival++;
        const Result<engine::Reservation> need{engine::RequiredReservation(market, side, price, lots)};
        RequireOk(need, "maker reservation");
        order.reserved_left = need->amount;
        const Result<ledger::ReserveResult> reserved{ledger.Reserve(owner, need->asset, need->amount)};
        RequireOk(reserved, "maker Reserve");
        BOOST_REQUIRE(*reserved == ledger::ReserveResult::OK);
        const Result<ledger::OpenOrderResult> opened{ledger.AddOpenOrder(owner)};
        RequireOk(opened, "maker AddOpenOrder");
        BOOST_REQUIRE(*opened == ledger::OpenOrderResult::OK);
        RequireOk(book.Rest(order), "maker Rest");
        return order;
    }

    //! A taker reserved the way phase 5 does: the full reservation at its limit.
    engine::Taker MakeTaker(std::string_view name, const AccountId& owner, Side side, uint64_t limit, uint64_t lots,
                            uint64_t rank, TimeInForce tif = TimeInForce::GTC)
    {
        engine::Taker taker;
        taker.id = EngineOrder(name);
        taker.owner = owner;
        taker.side = side;
        taker.tif = tif;
        taker.limit_ticks = limit;
        taker.remaining_lots = lots;
        taker.rank = rank;
        const Result<engine::Reservation> need{engine::RequiredReservation(market, side, limit, lots)};
        RequireOk(need, "taker reservation");
        const Result<ledger::ReserveResult> reserved{ledger.Reserve(owner, need->asset, need->amount)};
        RequireOk(reserved, "taker Reserve");
        BOOST_REQUIRE(*reserved == ledger::ReserveResult::OK);
        taker.reserved_left = need->amount;
        return taker;
    }

    //! Run SweepA2 and require success.
    engine::SweepResult Sweep(std::span<engine::Taker> takers, engine::SlotFillBudget& budget)
    {
        Result<engine::SweepResult> result{engine::SweepA2(book, ledger, takers, profile, budget)};
        RequireOk(result, "SweepA2");
        RequireOk(ledger.CheckConservation(), "conservation after the sweep");
        RequireOk(book.CheckInvariants(), "book invariants after the sweep");
        return std::move(result).value();
    }
};

} // namespace flowmesh2::test

#endif // B3_FLOWMESH2_TEST_UTIL_ENGINE_FIXTURES_H
