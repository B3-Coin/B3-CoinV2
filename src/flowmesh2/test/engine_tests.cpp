// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/codes.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/engine/book.h>
#include <flowmesh2/engine/engine.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/slot_fixtures.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::engine;
using namespace flowmesh2::test;

namespace {

const AssetId B3{AssetId::Native()};
const AssetId RUSD{TestAsset("rUSD")};
const AssetId TOK{TestAsset("TOK")};
//! A third market quoted in B3, so fees can accrue in two quote assets.
MarketDescriptor MarketM3() { return MarketDescriptor{TOK, B3, 1, 1}; }

using Deltas = std::map<std::pair<AccountId, AssetId>, std::pair<int64_t, int64_t>>;

Deltas DeltasOf(const OutcomeV2& outcome)
{
    Deltas out;
    for (const BalanceDelta& d : outcome.deltas) out[{d.account, d.asset}] = {d.available_delta, d.reserved_delta};
    return out;
}

const OutcomeV2& Outcome(const SlotResult& result, const std::vector<unsigned char>& original, const SlotFixture& fx)
{
    return OutcomeOf(result, fx.IdOf(original).id);
}

void CheckRejected(const OutcomeV2& outcome, OutcomeCode code, std::string_view what)
{
    BOOST_CHECK_MESSAGE(outcome.code == code, std::string{what} + ": got " + std::string{ToString(outcome.code)} + ", want " +
                                                  std::string{ToString(code)});
    BOOST_CHECK_MESSAGE(outcome.fills.empty() && outcome.deltas.empty() && outcome.remaining_lots == 0,
                        std::string{what} + ": a rejection must not move money");
}

bool HasNonce(const std::vector<uint64_t>& used, uint64_t nonce)
{
    return std::find(used.begin(), used.end(), nonce) != used.end();
}

//! Slot 1 of most tests: alice, bob and carol hold every asset.
void FundAll(SlotFixture& fx, Atoms amount = 1'000'000'000'000)
{
    std::vector<std::vector<unsigned char>> facts;
    for (const char* name : {"alice", "bob", "carol"}) {
        for (const AssetId& asset : {B3, RUSD, TOK}) {
            facts.push_back(fx.DepositBytes(KeyAccount(name), asset, amount, std::string{"fund-"} + name + "-" + asset.id.GetHex()));
        }
    }
    fx.Apply(fx.Inputs(facts, {}));
}

SourceRangeFact NetworkSource(SlotFixture& fx, uint64_t previous_height = 10, uint64_t to_height = 11)
{
    SourceRangeFact source{FactCursor{fx.anchor_height, fx.next_fact_index++}, fx.domain, 31337,
                           previous_height, LabelHash("source/previous"), to_height, LabelHash("source/to"), 2};
    if (previous_height == to_height) source.to_hash = source.previous_hash;
    return source;
}

} // namespace

BOOST_AUTO_TEST_SUITE(engine_tests)

BOOST_AUTO_TEST_CASE(walking_skeleton_slots_1_to_4)
{
    // ARCHITECTURE.md section 14.2 without admission: the engine alone.
    SlotFixture fx;
    const AccountId alice{KeyAccount("alice")};
    const AccountId bob{KeyAccount("bob")};

    // Slot 1: three deposits.
    std::vector<std::vector<unsigned char>> facts{fx.DepositBytes(alice, RUSD, 1'000'000'000, "T1"),
                                                  fx.DepositBytes(bob, B3, 10'000'000'000, "T2"),
                                                  fx.DepositBytes(bob, TOK, 500'000, "T3")};
    const SlotResult s1{fx.Apply(fx.Inputs(facts, {}))};
    BOOST_REQUIRE_EQUAL(s1.outcomes.size(), 3U);
    for (size_t i{0}; i < 3; ++i) {
        BOOST_CHECK(s1.outcomes[i].source == OutcomeSource::FACT);
        BOOST_CHECK(s1.outcomes[i].code == OutcomeCode::DEPOSIT_CREDITED);
        BOOST_CHECK(s1.outcomes[i].subject == FactLeaf(facts[i]));
    }
    BOOST_CHECK(DeltasOf(s1.outcomes[0]) == (Deltas{{{alice, RUSD}, {1'000'000'000, 0}}}));

    // Slot 2: a) Bob SELL M1 GTC 5,000 at 250; b) Alice BUY M2 GTC 100,000 at 600.
    const auto a{fx.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 250, 5'000)};
    const auto b{fx.Place("alice", 1, MarketM2(), Side::BUY, TimeInForce::GTC, 600, 100'000)};
    const SlotResult s2{fx.Apply(fx.Inputs({}, {a, b}))};
    BOOST_CHECK(Outcome(s2, a, fx).code == OutcomeCode::RESTING);
    BOOST_CHECK_EQUAL(Outcome(s2, a, fx).remaining_lots, 5'000U);
    BOOST_CHECK(DeltasOf(Outcome(s2, a, fx)) == (Deltas{{{bob, B3}, {-5'000'000'000, 5'000'000'000}}}));
    BOOST_CHECK(Outcome(s2, b, fx).code == OutcomeCode::RESTING);
    BOOST_CHECK(DeltasOf(Outcome(s2, b, fx)) == (Deltas{{{alice, RUSD}, {-600'000'000, 600'000'000}}}));

    // Slot 3: c) Alice BUY M1 IOC 2,000 at 260; d) Alice BUY M2 GTC 100,000 at 600.
    const auto c{fx.Place("alice", 2, MarketM1(), Side::BUY, TimeInForce::IOC, 260, 2'000)};
    const auto d{fx.Place("alice", 3, MarketM2(), Side::BUY, TimeInForce::GTC, 600, 100'000)};
    const SlotResult s3{fx.Apply(fx.Inputs({}, {d, c}))};
    const OutcomeV2& oc{Outcome(s3, c, fx)};
    BOOST_CHECK(oc.code == OutcomeCode::FILLED);
    BOOST_REQUIRE_EQUAL(oc.fills.size(), 1U);
    const Fill& fill{oc.fills[0]};
    BOOST_CHECK(fill.maker == fx.IdOf(a) && fill.taker == fx.IdOf(c) && fill.market == MarketM1().Id());
    BOOST_CHECK(fill.taker_side == Side::BUY);
    BOOST_CHECK_EQUAL(fill.price_ticks, 250U);
    BOOST_CHECK_EQUAL(fill.lots, 2'000U);
    BOOST_CHECK_EQUAL(fill.quote_atoms, 500'000);
    BOOST_CHECK_EQUAL(fill.seller_fee, 50); // ceil(500,000 * 100 / 1e6)
    BOOST_CHECK_EQUAL(fill.seller_quote_before, 0);
    // Reservation 520,000, quote paid 500,000, refund 20,000; Bob receives 499,950.
    BOOST_CHECK(DeltasOf(oc) == (Deltas{{{alice, RUSD}, {-500'000, 0}},
                                        {{alice, B3}, {2'000'000'000, 0}},
                                        {{bob, B3}, {0, -2'000'000'000}},
                                        {{bob, RUSD}, {499'950, 0}}}));
    // d: 399,480,000 available after c's reservation, 600,000,000 needed.
    CheckRejected(Outcome(s3, d, fx), OutcomeCode::REJ_INSUFFICIENT_AVAILABLE, "d");
    BOOST_CHECK(HasNonce(fx.UsedNonces("alice"), 3));
    // Fees: one outcome for rUSD, after the actions: seat pool 40, treasury 10.
    const OutcomeV2& fees{s3.outcomes.back()};
    BOOST_CHECK(fees.source == OutcomeSource::SLOT && fees.code == OutcomeCode::FEES_DISTRIBUTED);
    BOOST_CHECK(fees.subject == RUSD.id);
    const AccountId seat_pool{SeatPoolAccount(fx.profile)};
    const AccountId treasury{TreasuryAccount(fx.profile)};
    BOOST_CHECK(DeltasOf(fees) == (Deltas{{{seat_pool, RUSD}, {40, 0}}, {{treasury, RUSD}, {10, 0}}}));
    BOOST_CHECK_EQUAL(s3.outcomes.size(), 3U);

    // Slot 4: e) Bob TRANSFER 100,000 rUSD to Alice; f) Alice CANCEL b.
    const auto e{fx.Transfer("bob", 2, alice, RUSD, 100'000)};
    const auto f{fx.Cancel("alice", 4, fx.IdOf(b))};
    const SlotResult s4{fx.Apply(fx.Inputs({}, {e, f}))};
    BOOST_CHECK(Outcome(s4, e, fx).code == OutcomeCode::TRANSFERRED);
    BOOST_CHECK(DeltasOf(Outcome(s4, e, fx)) == (Deltas{{{bob, RUSD}, {-100'000, 0}}, {{alice, RUSD}, {100'000, 0}}}));
    BOOST_CHECK(Outcome(s4, f, fx).code == OutcomeCode::CANCELLED);
    BOOST_CHECK(DeltasOf(Outcome(s4, f, fx)) == (Deltas{{{alice, RUSD}, {600'000'000, -600'000'000}}}));

    // Final state of section 14.3 (without w, which admission handles).
    BOOST_CHECK(fx.BalanceOf("alice", RUSD) == (ledger::Balance{999'600'000, 0}));
    BOOST_CHECK(fx.BalanceOf("alice", B3) == (ledger::Balance{2'000'000'000, 0}));
    BOOST_CHECK(fx.UsedNonces("alice") == (std::vector<uint64_t>{1, 2, 3, 4}));
    BOOST_CHECK(fx.BalanceOf("bob", B3) == (ledger::Balance{5'000'000'000, 3'000'000'000}));
    BOOST_CHECK(fx.BalanceOf("bob", RUSD) == (ledger::Balance{399'950, 0}));
    BOOST_CHECK(fx.BalanceOf("bob", TOK) == (ledger::Balance{500'000, 0}));
    BOOST_CHECK(fx.UsedNonces("bob") == (std::vector<uint64_t>{1, 2}));
    BOOST_CHECK_EQUAL(fx.state.ledger.BalanceOf(seat_pool, RUSD).available, 40);
    BOOST_CHECK_EQUAL(fx.state.ledger.BalanceOf(treasury, RUSD).available, 10);
    const OrderBook& m1{*fx.state.books.Find(MarketM1().Id())};
    BOOST_REQUIRE_EQUAL(m1.Size(), 1U);
    BOOST_CHECK(m1.BestFront(Side::SELL)->id == fx.IdOf(a));
    BOOST_CHECK_EQUAL(m1.BestFront(Side::SELL)->remaining_lots, 3'000U);
    BOOST_CHECK_EQUAL(m1.BestFront(Side::SELL)->fee_charged, 50);
    BOOST_CHECK(fx.state.books.Find(MarketM2().Id())->Empty());
    for (const AssetId& asset : {RUSD, B3, TOK}) BOOST_CHECK_EQUAL(fx.state.ledger.TotalsOf(asset).fees_held, 0);
    RequireOk(fx.state.CheckInvariants(), "final invariants");
    // The head chain links: each header names its parent.
    BOOST_CHECK_EQUAL(fx.header.height, 4U);
}

BOOST_AUTO_TEST_CASE(shared_quote_reservation_in_m2_blocks_an_m1_order)
{
    SlotFixture fx;
    fx.Apply(fx.Inputs({fx.DepositBytes(KeyAccount("alice"), RUSD, 1'000'000'000, "alice")}, {}));
    // Same slot: the M2 order (nonce 1) reserves first in canonical order, so the
    // M1 order (nonce 2) sees only 400,000,000 rUSD left of the one balance.
    const auto m2{fx.Place("alice", 1, MarketM2(), Side::BUY, TimeInForce::GTC, 600, 100'000)};
    const auto m1{fx.Place("alice", 2, MarketM1(), Side::BUY, TimeInForce::GTC, 500'000, 1'000)};
    const SlotResult s2{fx.Apply(fx.Inputs({}, {m1, m2}))};
    BOOST_CHECK(Outcome(s2, m2, fx).code == OutcomeCode::RESTING);
    CheckRejected(Outcome(s2, m1, fx), OutcomeCode::REJ_INSUFFICIENT_AVAILABLE, "M1 after M2");
    // A later slot: the resting M2 reservation still blocks M1 above 400,000,000.
    const auto m1_again{fx.Place("alice", 3, MarketM1(), Side::BUY, TimeInForce::GTC, 400'000, 1'001)};
    const auto m1_fits{fx.Place("alice", 4, MarketM1(), Side::BUY, TimeInForce::GTC, 400'000, 1'000)};
    const SlotResult s3{fx.Apply(fx.Inputs({}, {m1_again, m1_fits}))};
    CheckRejected(Outcome(s3, m1_again, fx), OutcomeCode::REJ_INSUFFICIENT_AVAILABLE, "400,400,000 needed");
    BOOST_CHECK(Outcome(s3, m1_fits, fx).code == OutcomeCode::RESTING);
    BOOST_CHECK(fx.BalanceOf("alice", RUSD) == (ledger::Balance{0, 1'000'000'000}));
}

BOOST_AUTO_TEST_CASE(same_slot_proceeds_cannot_fund_a_same_slot_reservation)
{
    SlotFixture fx;
    fx.Apply(fx.Inputs({fx.DepositBytes(KeyAccount("alice"), RUSD, 1'000'000'000, "alice"),
                        fx.DepositBytes(KeyAccount("bob"), B3, 10'000'000'000, "bob")},
                       {}));
    const auto bid{fx.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 300, 1'000)};
    fx.Apply(fx.Inputs({}, {bid}));
    // Bob sells into the bid (proceeds 300,000 - 30) and, in the same slot, bids
    // 10,000 rUSD on M2. All reservations happen before any sweep: refused.
    const auto sell{fx.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::IOC, 300, 1'000)};
    const auto buy{fx.Place("bob", 2, MarketM2(), Side::BUY, TimeInForce::GTC, 100, 10)};
    const SlotResult s3{fx.Apply(fx.Inputs({}, {sell, buy}))};
    BOOST_CHECK(Outcome(s3, sell, fx).code == OutcomeCode::FILLED);
    CheckRejected(Outcome(s3, buy, fx), OutcomeCode::REJ_INSUFFICIENT_AVAILABLE, "same-slot proceeds");
    BOOST_CHECK(fx.BalanceOf("bob", RUSD) == (ledger::Balance{299'970, 0}));
    // The next slot can use them.
    const auto buy_later{fx.Place("bob", 3, MarketM2(), Side::BUY, TimeInForce::GTC, 100, 10)};
    const SlotResult s4{fx.Apply(fx.Inputs({}, {buy_later}))};
    BOOST_CHECK(Outcome(s4, buy_later, fx).code == OutcomeCode::RESTING);
    BOOST_CHECK(fx.BalanceOf("bob", RUSD) == (ledger::Balance{289'970, 10'000}));
}

BOOST_AUTO_TEST_CASE(transfers_move_available_or_are_rejected)
{
    SlotFixture fx;
    FundAll(fx, 1'000);
    const AccountId bob{KeyAccount("bob")};
    const auto ok{fx.Transfer("alice", 1, bob, RUSD, 400)};
    const auto self{fx.Transfer("alice", 2, KeyAccount("alice"), RUSD, 1)};
    const auto zero{fx.Transfer("alice", 3, bob, RUSD, 0)};
    const auto nobody{fx.Transfer("alice", 4, KeyAccount("erin"), RUSD, 1)};
    const auto too_much{fx.Transfer("alice", 5, bob, RUSD, 601)};
    const SlotResult s{fx.Apply(fx.Inputs({}, {ok, self, zero, nobody, too_much}))};
    BOOST_CHECK(Outcome(s, ok, fx).code == OutcomeCode::TRANSFERRED);
    BOOST_CHECK(DeltasOf(Outcome(s, ok, fx)) == (Deltas{{{KeyAccount("alice"), RUSD}, {-400, 0}}, {{bob, RUSD}, {400, 0}}}));
    CheckRejected(Outcome(s, self, fx), OutcomeCode::REJ_BAD_PARAMS, "self-transfer");
    CheckRejected(Outcome(s, zero, fx), OutcomeCode::REJ_BAD_PARAMS, "zero amount");
    CheckRejected(Outcome(s, nobody, fx), OutcomeCode::REJ_NO_RECIPIENT, "unknown recipient");
    CheckRejected(Outcome(s, too_much, fx), OutcomeCode::REJ_INSUFFICIENT_AVAILABLE, "601 of 600");
    BOOST_CHECK(fx.UsedNonces("alice") == (std::vector<uint64_t>{1, 2, 3, 4, 5}));
    BOOST_CHECK(fx.BalanceOf("alice", RUSD) == (ledger::Balance{600, 0}));
    BOOST_CHECK(fx.BalanceOf("bob", RUSD) == (ledger::Balance{1'400, 0}));
}

BOOST_AUTO_TEST_CASE(cancels_release_or_are_rejected)
{
    SlotFixture fx;
    FundAll(fx);
    const auto alice_bid{fx.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 100, 7)};
    const auto bob_ask{fx.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 200, 3)};
    fx.Apply(fx.Inputs({}, {alice_bid, bob_ask}));
    BOOST_CHECK(fx.BalanceOf("alice", RUSD).reserved == 700);
    BOOST_CHECK_EQUAL(fx.state.ledger.Find(KeyAccount("alice"))->open_orders, 1U);

    // Own order: CANCELLED; someone else's: REJ_NOT_OWN_ORDER; an unknown id and
    // an order placed in the same slot (its PLACE runs after the cancels): REJ_UNKNOWN_ORDER.
    const auto cancel{fx.Cancel("alice", 2, fx.IdOf(alice_bid))};
    const auto steal{fx.Cancel("carol", 1, fx.IdOf(bob_ask))};
    const auto unknown{fx.Cancel("carol", 2, OrderId{LabelHash("fm2/test/order/none")})};
    const auto later{fx.Place("carol", 3, MarketM1(), Side::BUY, TimeInForce::GTC, 90, 1)};
    const auto too_early{fx.Cancel("carol", 4, fx.IdOf(later))};
    const SlotResult s{fx.Apply(fx.Inputs({}, {cancel, steal, unknown, later, too_early}))};
    BOOST_CHECK(Outcome(s, cancel, fx).code == OutcomeCode::CANCELLED);
    BOOST_CHECK(DeltasOf(Outcome(s, cancel, fx)) == (Deltas{{{KeyAccount("alice"), RUSD}, {700, -700}}}));
    CheckRejected(Outcome(s, steal, fx), OutcomeCode::REJ_NOT_OWN_ORDER, "another owner's order");
    CheckRejected(Outcome(s, unknown, fx), OutcomeCode::REJ_UNKNOWN_ORDER, "unknown order");
    CheckRejected(Outcome(s, too_early, fx), OutcomeCode::REJ_UNKNOWN_ORDER, "an order of the same slot");
    BOOST_CHECK(Outcome(s, later, fx).code == OutcomeCode::RESTING);
    BOOST_CHECK(fx.BalanceOf("alice", RUSD).reserved == 0);
    BOOST_CHECK_EQUAL(fx.state.ledger.Find(KeyAccount("alice"))->open_orders, 0U);
    BOOST_CHECK(fx.state.books.FindOrder(fx.IdOf(alice_bid)) == nullptr);
    BOOST_CHECK(fx.state.books.FindOrder(fx.IdOf(bob_ask)) != nullptr);
    // The same cancel again in a later slot: the order is gone.
    const auto again{fx.Cancel("alice", 3, fx.IdOf(alice_bid))};
    CheckRejected(Outcome(fx.Apply(fx.Inputs({}, {again})), again, fx), OutcomeCode::REJ_UNKNOWN_ORDER, "cancel twice");
}

BOOST_AUTO_TEST_CASE(post_only_rests_or_would_cross)
{
    SlotFixture fx;
    FundAll(fx);
    const auto ask{fx.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::POST_ONLY, 200, 3)};
    const SlotResult s2{fx.Apply(fx.Inputs({}, {ask}))};
    BOOST_CHECK(Outcome(s2, ask, fx).code == OutcomeCode::RESTING);
    BOOST_CHECK_EQUAL(Outcome(s2, ask, fx).remaining_lots, 3U);
    // Crossing the resting ask, and crossing a post-only bid placed earlier in the
    // same slot (alice's nonces run in order): REJ_WOULD_CROSS, no money moves.
    const auto crossing{fx.Place("carol", 1, MarketM1(), Side::BUY, TimeInForce::POST_ONLY, 200, 1)};
    const auto bid{fx.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::POST_ONLY, 150, 2)};
    const auto under_bid{fx.Place("alice", 2, MarketM1(), Side::SELL, TimeInForce::POST_ONLY, 150, 1)};
    const auto fine{fx.Place("alice", 3, MarketM1(), Side::SELL, TimeInForce::POST_ONLY, 151, 1)};
    const SlotResult s3{fx.Apply(fx.Inputs({}, {crossing, bid, under_bid, fine}))};
    CheckRejected(Outcome(s3, crossing, fx), OutcomeCode::REJ_WOULD_CROSS, "crossing the book");
    BOOST_CHECK(Outcome(s3, bid, fx).code == OutcomeCode::RESTING);
    CheckRejected(Outcome(s3, under_bid, fx), OutcomeCode::REJ_WOULD_CROSS, "crossing a same-slot post-only");
    BOOST_CHECK(Outcome(s3, fine, fx).code == OutcomeCode::RESTING);
    BOOST_CHECK_EQUAL(fx.state.books.Find(MarketM1().Id())->Size(), 3U);
}

BOOST_AUTO_TEST_CASE(post_only_queues_ahead_of_a_same_slot_remainder)
{
    // A post-only order rests in phase 4 and a GTC remainder in phase 6, so at one
    // level the post-only order is first even when its canonical rank is higher.
    bool seen_higher_rank{false};
    for (uint64_t lots{1}; lots <= 16 && !seen_higher_rank; ++lots) {
        SlotFixture fx;
        FundAll(fx);
        const auto post{fx.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::POST_ONLY, 100, lots)};
        const auto gtc{fx.Place("bob", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 100, 5)};
        const SlotInputs inputs{fx.Inputs({}, {post, gtc})};
        const Result<ValidatedSlot> slot{fx.Validate(inputs)};
        RequireOk(slot, "validate");
        // order[rank] = index; index 0 is the post-only order.
        seen_higher_rank = slot->Order()[0] == 1;
        fx.Apply(inputs);
        const OrderBook& m1{*fx.state.books.Find(MarketM1().Id())};
        BOOST_CHECK(m1.BestFront(Side::BUY)->id == fx.IdOf(post));
        const OrderBook::Level& level{m1.LadderOf(Side::BUY).at(100)};
        BOOST_REQUIRE_EQUAL(level.size(), 2U);
        BOOST_CHECK(level[1].id == fx.IdOf(gtc));
        BOOST_CHECK_LT(level[0].arrival_rank, level[1].arrival_rank);
    }
    BOOST_CHECK_MESSAGE(seen_higher_rank, "no case where the post-only order had the higher canonical rank");
}

BOOST_AUTO_TEST_CASE(ioc_remainder_and_price_improvement_refund)
{
    SlotFixture fx;
    FundAll(fx);
    const AccountId alice{KeyAccount("alice")};
    const auto ask{fx.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 250, 3)};
    fx.Apply(fx.Inputs({}, {ask}));
    // IOC: 3 of 5 lots fill at 250; reservation 5 x 260 = 1,300; paid 750; 550 released.
    const auto ioc{fx.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::IOC, 260, 5)};
    const SlotResult s3{fx.Apply(fx.Inputs({}, {ioc}))};
    const OutcomeV2& o3{Outcome(s3, ioc, fx)};
    BOOST_CHECK(o3.code == OutcomeCode::IOC_DONE);
    BOOST_CHECK_EQUAL(o3.remaining_lots, 0U);
    BOOST_REQUIRE_EQUAL(o3.fills.size(), 1U);
    BOOST_CHECK_EQUAL(o3.fills[0].quote_atoms, 750);
    BOOST_CHECK(DeltasOf(o3).at({alice, RUSD}) == (std::pair<int64_t, int64_t>{-750, 0}));
    BOOST_CHECK(DeltasOf(o3).at({alice, B3}) == (std::pair<int64_t, int64_t>{3'000'000, 0}));
    BOOST_CHECK(fx.BalanceOf("alice", RUSD).reserved == 0);
    BOOST_CHECK(fx.state.books.Find(MarketM1().Id())->Empty());

    // GTC filled in full at a better price: FILLED, the improvement is released.
    const auto ask2{fx.Place("bob", 2, MarketM1(), Side::SELL, TimeInForce::GTC, 250, 5)};
    fx.Apply(fx.Inputs({}, {ask2}));
    const auto full{fx.Place("alice", 2, MarketM1(), Side::BUY, TimeInForce::GTC, 260, 2)};
    const SlotResult s5{fx.Apply(fx.Inputs({}, {full}))};
    BOOST_CHECK(Outcome(s5, full, fx).code == OutcomeCode::FILLED);
    BOOST_CHECK(DeltasOf(Outcome(s5, full, fx)).at({alice, RUSD}) == (std::pair<int64_t, int64_t>{-500, 0}));
    BOOST_CHECK_EQUAL(fx.state.ledger.Find(alice)->open_orders, 0U);

    // GTC partially filled: 3 lots at 250, the remaining 2 rest at 260 holding
    // exactly 520; the 3 x 10 improvement is released.
    const auto partial{fx.Place("alice", 3, MarketM1(), Side::BUY, TimeInForce::GTC, 260, 5)};
    const SlotResult s6{fx.Apply(fx.Inputs({}, {partial}))};
    const OutcomeV2& o6{Outcome(s6, partial, fx)};
    BOOST_CHECK(o6.code == OutcomeCode::PARTIAL_RESTING);
    BOOST_CHECK_EQUAL(o6.remaining_lots, 2U);
    BOOST_CHECK(DeltasOf(o6).at({alice, RUSD}) == (std::pair<int64_t, int64_t>{-1'270, 520}));
    const RestingOrder* rest{fx.state.books.Find(MarketM1().Id())->Find(fx.IdOf(partial))};
    BOOST_REQUIRE(rest != nullptr);
    BOOST_CHECK_EQUAL(rest->price_ticks, 260U);
    BOOST_CHECK_EQUAL(rest->remaining_lots, 2U);
    BOOST_CHECK_EQUAL(rest->reserved_left, 520);
    BOOST_CHECK_EQUAL(rest->cum_quote, 750);
    BOOST_CHECK_EQUAL(rest->fills, 1U);
    BOOST_CHECK(fx.BalanceOf("alice", RUSD).reserved == 520);
    BOOST_CHECK_EQUAL(fx.state.ledger.Find(alice)->open_orders, 1U);
}

BOOST_AUTO_TEST_CASE(deterministic_rejections_consume_the_nonce_with_zero_deltas)
{
    ProfileParams profile{RegtestProfile()};
    profile.max_open_orders_per_account = 1;
    SlotFixture fx{profile};
    FundAll(fx, 1'000'000);
    const auto resting{fx.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 10, 1)};
    const auto ask{fx.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 50, 1)};
    fx.Apply(fx.Inputs({}, {resting, ask}));
    const ledger::Balance before{fx.BalanceOf("carol", RUSD)};

    const MarketDescriptor unlisted{TOK, RUSD, 7, 7};
    const std::vector<std::pair<std::vector<unsigned char>, OutcomeCode>> cases{
        {fx.Place("carol", 1, unlisted, Side::BUY, TimeInForce::GTC, 1, 1), OutcomeCode::REJ_UNKNOWN_MARKET},
        {fx.Place("carol", 2, MarketM1(), Side::BUY, TimeInForce::GTC, 1, 0), OutcomeCode::REJ_BAD_PARAMS},
        {fx.Place("carol", 3, MarketM1(), Side::BUY, TimeInForce::IOC, 0, 1), OutcomeCode::REJ_BAD_PARAMS},
        {fx.Place("carol", 4, MarketM1(), Side::BUY, TimeInForce::GTC, uint64_t{1} << 40, uint64_t{1} << 40),
         OutcomeCode::REJ_BAD_PARAMS}, // 2^80 quote atoms
        {fx.Place("carol", 5, MarketM1(), Side::SELL, TimeInForce::POST_ONLY, 5, 1), OutcomeCode::REJ_WOULD_CROSS},
        {fx.Place("carol", 6, MarketM1(), Side::BUY, TimeInForce::IOC, 49, 1'000'000), OutcomeCode::REJ_INSUFFICIENT_AVAILABLE},
        {fx.Place("alice", 2, MarketM2(), Side::BUY, TimeInForce::GTC, 1, 1), OutcomeCode::REJ_OPEN_ORDER_CAP},
        {fx.Place("alice", 3, MarketM2(), Side::SELL, TimeInForce::POST_ONLY, 900, 1), OutcomeCode::REJ_OPEN_ORDER_CAP},
        {fx.Cancel("carol", 7, OrderId{LabelHash("fm2/test/order/none")}), OutcomeCode::REJ_UNKNOWN_ORDER},
        {fx.Cancel("carol", 8, fx.IdOf(ask)), OutcomeCode::REJ_NOT_OWN_ORDER},
        {fx.Transfer("carol", 9, KeyAccount("carol"), RUSD, 1), OutcomeCode::REJ_BAD_PARAMS},
    };
    std::vector<std::vector<unsigned char>> originals;
    for (const auto& [original, code] : cases) originals.push_back(original);
    const SlotResult s{fx.Apply(fx.Inputs({}, originals))};
    for (const auto& [original, code] : cases) CheckRejected(Outcome(s, original, fx), code, std::string{ToString(code)});
    // Every nonce was consumed; nothing else changed.
    BOOST_CHECK(fx.UsedNonces("carol") == (std::vector<uint64_t>{1, 2, 3, 4, 5, 6, 7, 8, 9}));
    BOOST_CHECK(fx.UsedNonces("alice") == (std::vector<uint64_t>{1, 2, 3}));
    BOOST_CHECK(fx.BalanceOf("carol", RUSD) == before);
    BOOST_CHECK_EQUAL(fx.state.books.Find(MarketM1().Id())->Size(), 2U);
    // An IOC never takes an open-order slot, so it is not refused at the cap.
    const auto ioc{fx.Place("alice", 4, MarketM1(), Side::BUY, TimeInForce::IOC, 1, 1)};
    BOOST_CHECK(Outcome(fx.Apply(fx.Inputs({}, {ioc})), ioc, fx).code == OutcomeCode::IOC_DONE);
}

BOOST_AUTO_TEST_CASE(fill_caps_refuse_the_remainder_only)
{
    ProfileParams profile{RegtestProfile()};
    profile.max_fills_per_order = 2;
    profile.max_fills_per_slot = 3;
    SlotFixture fx{profile};
    FundAll(fx);
    std::vector<std::vector<unsigned char>> asks;
    for (uint64_t k{0}; k < 5; ++k) asks.push_back(fx.Place("bob", k + 1, MarketM1(), Side::SELL, TimeInForce::GTC, 100 + k, 1));
    fx.Apply(fx.Inputs({}, asks));

    // One taker: two fills, then the per-order cap while the book still crosses.
    const auto capped{fx.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 104, 3)};
    const SlotResult s3{fx.Apply(fx.Inputs({}, {capped}))};
    const OutcomeV2& o3{Outcome(s3, capped, fx)};
    BOOST_CHECK(o3.code == OutcomeCode::REJ_FILL_CAP);
    BOOST_CHECK_EQUAL(o3.fills.size(), 2U);
    BOOST_CHECK_EQUAL(o3.remaining_lots, 0U);
    // Paid 100 + 101; the rest of the 312 reserved is released; the GTC took no resting slot.
    BOOST_CHECK(DeltasOf(o3).at({KeyAccount("alice"), RUSD}) == (std::pair<int64_t, int64_t>{-201, 0}));
    BOOST_CHECK_EQUAL(fx.state.ledger.Find(KeyAccount("alice"))->open_orders, 0U);
    BOOST_CHECK(fx.state.books.FindOrder(fx.IdOf(capped)) == nullptr);

    // Two takers share the slot budget of 3 fills against the remaining 3 asks.
    const auto t1{fx.Place("alice", 2, MarketM1(), Side::BUY, TimeInForce::IOC, 104, 3)};
    const auto t2{fx.Place("carol", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 104, 3)};
    // Two more asks so the book still crosses when the budget runs out.
    const auto more{fx.Place("bob", 6, MarketM1(), Side::SELL, TimeInForce::POST_ONLY, 104, 9)};
    const SlotResult s4{fx.Apply(fx.Inputs({}, {t1, t2, more}))};
    const OutcomeV2& o1{Outcome(s4, t1, fx)};
    const OutcomeV2& o2{Outcome(s4, t2, fx)};
    BOOST_CHECK_EQUAL(o1.fills.size() + o2.fills.size(), 3U);
    BOOST_CHECK_LE(o1.fills.size(), 2U);
    BOOST_CHECK_LE(o2.fills.size(), 2U);
    BOOST_CHECK(o1.code == OutcomeCode::REJ_FILL_CAP);
    BOOST_CHECK(o2.code == OutcomeCode::REJ_FILL_CAP);
    BOOST_CHECK(fx.state.books.FindOrder(fx.IdOf(t2)) == nullptr);
    BOOST_CHECK(fx.BalanceOf("carol", RUSD).reserved == 0);
}

BOOST_AUTO_TEST_CASE(slot_fill_budget_is_shared_by_all_markets)
{
    ProfileParams profile{RegtestProfile()};
    profile.max_fills_per_order = 2;
    profile.max_fills_per_slot = 3;
    SlotFixture fx{profile};
    FundAll(fx);
    fx.Apply(fx.Inputs({}, {fx.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 100, 1),
                            fx.Place("bob", 2, MarketM1(), Side::SELL, TimeInForce::GTC, 101, 1),
                            fx.Place("bob", 3, MarketM2(), Side::SELL, TimeInForce::GTC, 10, 1),
                            fx.Place("bob", 4, MarketM2(), Side::SELL, TimeInForce::GTC, 11, 1)}));
    const auto in_m1{fx.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::IOC, 101, 2)};
    const auto in_m2{fx.Place("carol", 1, MarketM2(), Side::BUY, TimeInForce::IOC, 11, 2)};
    const SlotResult s{fx.Apply(fx.Inputs({}, {in_m1, in_m2}))};
    // Markets sweep in ascending MarketId; the first takes 2 fills, the second
    // gets the last unit of the budget of 3 and stops with lots left.
    const bool m1_first{MarketM1().Id() < MarketM2().Id()};
    const OutcomeV2& first{Outcome(s, m1_first ? in_m1 : in_m2, fx)};
    const OutcomeV2& second{Outcome(s, m1_first ? in_m2 : in_m1, fx)};
    BOOST_CHECK(first.code == OutcomeCode::FILLED);
    BOOST_CHECK_EQUAL(first.fills.size(), 2U);
    BOOST_CHECK(second.code == OutcomeCode::REJ_FILL_CAP);
    BOOST_CHECK_EQUAL(second.fills.size(), 1U);
}

BOOST_AUTO_TEST_CASE(residual_cross_is_refused_released_and_conserved)
{
    SlotFixture fx;
    FundAll(fx);
    // An empty M1: a GTC buy at 105 and a GTC sell at 100 cannot reach each other
    // in the sweep; their remainders would rest crossed. Both are refused.
    const auto buy{fx.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 105, 5)};
    const auto sell{fx.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 100, 5)};
    const auto apart{fx.Place("carol", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 200, 5)};
    const ledger::Balance alice_before{fx.BalanceOf("alice", RUSD)};
    const SlotResult s2{fx.Apply(fx.Inputs({}, {buy, sell, apart}))};
    CheckRejected(Outcome(s2, buy, fx), OutcomeCode::REJ_RESIDUAL_CROSS_UNSUPPORTED, "buy remainder");
    CheckRejected(Outcome(s2, sell, fx), OutcomeCode::REJ_RESIDUAL_CROSS_UNSUPPORTED, "sell remainder");
    BOOST_CHECK(Outcome(s2, apart, fx).code == OutcomeCode::RESTING); // 200 is above the buy's 105
    BOOST_CHECK(fx.BalanceOf("alice", RUSD) == alice_before);
    BOOST_CHECK_EQUAL(fx.state.ledger.Find(KeyAccount("alice"))->open_orders, 0U);
    BOOST_CHECK_EQUAL(fx.state.ledger.Find(KeyAccount("bob"))->open_orders, 0U);
    BOOST_CHECK_EQUAL(fx.state.books.Find(MarketM1().Id())->Size(), 1U);

    // With a maker: the buy fills 2 lots against it, then its remainder crosses a
    // same-slot sell; the fills stay, only the remainder is refused.
    const auto maker{fx.Place("carol", 2, MarketM1(), Side::SELL, TimeInForce::GTC, 100, 2)};
    fx.Apply(fx.Inputs({}, {maker}));
    const auto buy2{fx.Place("alice", 2, MarketM1(), Side::BUY, TimeInForce::GTC, 105, 5)};
    const auto sell2{fx.Place("bob", 2, MarketM1(), Side::SELL, TimeInForce::GTC, 102, 5)};
    const SlotResult s4{fx.Apply(fx.Inputs({}, {buy2, sell2}))};
    const OutcomeV2& ob{Outcome(s4, buy2, fx)};
    BOOST_CHECK(ob.code == OutcomeCode::REJ_RESIDUAL_CROSS_UNSUPPORTED);
    BOOST_CHECK_EQUAL(ob.fills.size(), 1U);
    BOOST_CHECK(DeltasOf(ob).at({KeyAccount("alice"), RUSD}) == (std::pair<int64_t, int64_t>{-200, 0}));
    CheckRejected(Outcome(s4, sell2, fx), OutcomeCode::REJ_RESIDUAL_CROSS_UNSUPPORTED, "sell2 remainder");
    BOOST_CHECK(fx.BalanceOf("alice", RUSD).reserved == 0);
    BOOST_CHECK(fx.BalanceOf("bob", B3).reserved == 0);
    RequireOk(fx.state.CheckInvariants(), "invariants after the residual cross");
}

BOOST_AUTO_TEST_CASE(one_fee_outcome_per_quote_asset)
{
    SlotFixture fx{RegtestProfile(), {MarketM1(), MarketM3()}};
    FundAll(fx);
    const auto ask1{fx.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 1'000, 50)};
    const auto ask3{fx.Place("bob", 2, MarketM3(), Side::SELL, TimeInForce::GTC, 20'000, 1)};
    fx.Apply(fx.Inputs({}, {ask1, ask3}));
    // M1: quote 50,000 rUSD, fee 5 -> seats 4, treasury 1. M3: quote 20,000 B3, fee 2 -> 1/1.
    const auto take1{fx.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::IOC, 1'000, 50)};
    const auto take3{fx.Place("alice", 2, MarketM3(), Side::BUY, TimeInForce::IOC, 20'000, 1)};
    const SlotResult s{fx.Apply(fx.Inputs({}, {take3, take1}))};
    BOOST_REQUIRE_EQUAL(s.outcomes.size(), 4U);
    const OutcomeV2& first{s.outcomes[2]};
    const OutcomeV2& second{s.outcomes[3]};
    // Ascending AssetId: B3 (all zero) before rUSD.
    BOOST_CHECK(first.code == OutcomeCode::FEES_DISTRIBUTED && first.subject == B3.id);
    BOOST_CHECK(second.code == OutcomeCode::FEES_DISTRIBUTED && second.subject == RUSD.id);
    const AccountId seat_pool{SeatPoolAccount(fx.profile)};
    const AccountId treasury{TreasuryAccount(fx.profile)};
    BOOST_CHECK(DeltasOf(first) == (Deltas{{{seat_pool, B3}, {1, 0}}, {{treasury, B3}, {1, 0}}}));
    BOOST_CHECK(DeltasOf(second) == (Deltas{{{seat_pool, RUSD}, {4, 0}}, {{treasury, RUSD}, {1, 0}}}));
    for (const AssetId& asset : {B3, RUSD, TOK}) BOOST_CHECK_EQUAL(fx.state.ledger.TotalsOf(asset).fees_held, 0);

    // A fee of 1 atom: the seat pool's floor share is 0, so only the treasury appears.
    const auto ask_small{fx.Place("bob", 3, MarketM1(), Side::SELL, TimeInForce::GTC, 1, 1)};
    fx.Apply(fx.Inputs({}, {ask_small}));
    const auto take_small{fx.Place("alice", 3, MarketM1(), Side::BUY, TimeInForce::IOC, 1, 1)};
    const SlotResult s5{fx.Apply(fx.Inputs({}, {take_small}))};
    BOOST_CHECK(DeltasOf(s5.outcomes.back()) == (Deltas{{{treasury, RUSD}, {1, 0}}}));
    // A slot without fills has no fee outcome.
    BOOST_CHECK(fx.Apply(fx.Inputs({}, {})).outcomes.empty());
}

BOOST_AUTO_TEST_CASE(permuting_originals_gives_identical_outcomes_and_roots)
{
    SlotFixture fx;
    FundAll(fx);
    fx.Apply(fx.Inputs({}, {fx.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 100, 6),
                            fx.Place("carol", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 90, 4)}));
    const std::vector<std::vector<unsigned char>> facts{fx.DepositBytes(KeyAccount("alice"), TOK, 77, "extra")};
    std::vector<std::vector<unsigned char>> originals{
        fx.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 101, 3),
        fx.Place("alice", 2, MarketM1(), Side::BUY, TimeInForce::IOC, 100, 2),
        fx.Place("carol", 2, MarketM1(), Side::BUY, TimeInForce::GTC, 100, 4),
        fx.Place("bob", 2, MarketM1(), Side::SELL, TimeInForce::GTC, 95, 1),
        fx.Place("bob", 3, MarketM2(), Side::SELL, TimeInForce::POST_ONLY, 7, 5),
        fx.Transfer("carol", 3, KeyAccount("alice"), RUSD, 5),
        fx.Cancel("carol", 4, OrderId{LabelHash("fm2/test/order/none")}),
        fx.Place("alice", 3, MarketM2(), Side::BUY, TimeInForce::GTC, 8, 2)};
    const Result<SlotResult> base{fx.TryApply(fx.Inputs(facts, originals))};
    RequireOk(base, "base order");
    BOOST_CHECK_GT(base->outcomes.size(), originals.size());
    uint64_t seed{7};
    for (int round{0}; round < 24; ++round) {
        for (size_t i{originals.size()}; i > 1; --i) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            std::swap(originals[i - 1], originals[(seed >> 33) % i]);
        }
        const SlotInputs inputs{fx.Inputs(facts, originals)};
        const Result<SlotResult> permuted{fx.TryApply(inputs)};
        RequireOk(permuted, "permuted order");
        BOOST_CHECK(permuted->outcomes == base->outcomes);
        BOOST_CHECK(permuted->outcomes_root == base->outcomes_root);
        BOOST_CHECK(permuted->roots == base->roots);
        BOOST_CHECK(permuted->state == base->state);
    }
}

BOOST_AUTO_TEST_CASE(stale_parent_and_profile_checks)
{
    SlotFixture fx;
    FundAll(fx);
    const SlotInputs inputs{fx.Inputs({}, {fx.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 1, 1)})};
    const Result<ValidatedSlot> slot{fx.Validate(inputs)};
    RequireOk(slot, "validate");
    const Engine engine{Engine::Create(fx.profile).value()};
    RequireOk(engine.ApplySlot(fx.Head(), *slot), "apply on its parent");

    // Another head (here: the same state under another header) is a stale parent.
    SlotHeaderV2 other{fx.header};
    other.anchor = TestAnchor(131);
    RequireError(engine.ApplySlot(HeadView{other, fx.state}, *slot), ErrorCode::STALE_PARENT, "another parent");
    // An engine of another profile refuses the slot.
    ProfileParams profile{fx.profile};
    profile.fee_ppm = 101;
    RequireError(Engine::Create(profile).value().ApplySlot(fx.Head(), *slot), ErrorCode::INVALID_INPUT, "another profile");
    // Engine::Create runs Validate first.
    profile.futures_enabled = true;
    RequireStub(Engine::Create(profile), "M9-FUT-01");
    profile = fx.profile;
    profile.chain = ChainType::MAIN;
    RequireError(Engine::Create(profile), ErrorCode::UNSUPPORTED_NETWORK, "MAIN");
    // A parent that still holds fees is not a slot boundary.
    EngineState held{fx.state};
    BOOST_REQUIRE(held.ledger.Reserve(KeyAccount("alice"), RUSD, 100).value() == ledger::ReserveResult::OK);
    RequireOk(held.ledger.PayFromReserved(KeyAccount("alice"), KeyAccount("bob"), RUSD, 100, 1), "withhold a fee");
    const HeadView held_head{fx.header, held};
    const Result<ValidatedSlot> held_slot{ValidateSlotInputs(held_head, inputs, fx.profile)};
    RequireOk(held_slot, "validate on the doctored parent");
    RequireError(engine.ApplySlot(held_head, *held_slot), ErrorCode::INVARIANT, "fees held by the parent");
}

BOOST_AUTO_TEST_CASE(futures_phase_is_a_no_op_while_disabled)
{
    SlotFixture fx;
    EngineState state{fx.state};
    RequireOk(RunFuturesPhase(state, fx.profile), "futures disabled");
    BOOST_CHECK(state == fx.state);
    ProfileParams enabled{fx.profile};
    enabled.futures_enabled = true;
    RequireStub(RunFuturesPhase(state, enabled), "M9-FUT-04");
    BOOST_CHECK(state == fx.state);
}

BOOST_AUTO_TEST_CASE(stable_q_two_markets_charge_and_distribute_only_q)
{
    SlotFixture fx{StableQRegtestProfile(ChainType::REGTEST).value()};
    FundAll(fx);
    const auto ask1{fx.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 1'001, 5)};
    const auto ask2{fx.Place("bob", 2, MarketM2(), Side::SELL, TimeInForce::GTC, 1'000, 2)};
    fx.Apply(fx.Inputs({}, {ask1, ask2}));
    const auto first1{fx.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::IOC, 1'001, 2)};
    const auto first2{fx.Place("alice", 2, MarketM2(), Side::BUY, TimeInForce::IOC, 1'000, 2)};
    const SlotResult first{fx.Apply(fx.Inputs({}, {first1, first2}))};
    Atoms fees{0};
    size_t fee_outcomes{0};
    for (const OutcomeV2& outcome : first.outcomes) {
        for (const Fill& fill : outcome.fills) fees += fill.seller_fee;
        if (outcome.code != OutcomeCode::FEES_DISTRIBUTED) continue;
        ++fee_outcomes;
        BOOST_CHECK(outcome.subject == RUSD.id);
        for (const BalanceDelta& delta : outcome.deltas) {
            BOOST_CHECK(delta.asset == RUSD);
            BOOST_CHECK(delta.available_delta > 0 && delta.reserved_delta == 0);
        }
    }
    BOOST_CHECK_EQUAL(fees, 3);
    BOOST_CHECK_EQUAL(fee_outcomes, 1U);
    const AccountId seat_pool{SeatPoolAccount(fx.profile)};
    const AccountId treasury{TreasuryAccount(fx.profile)};
    BOOST_CHECK_EQUAL(fx.state.ledger.BalanceOf(seat_pool, RUSD).available, 2);
    BOOST_CHECK_EQUAL(fx.state.ledger.BalanceOf(treasury, RUSD).available, 1);
    for (const AssetId& asset : {B3, TOK}) {
        BOOST_CHECK(fx.state.ledger.BalanceOf(seat_pool, asset).IsZero());
        BOOST_CHECK(fx.state.ledger.BalanceOf(treasury, asset).IsZero());
    }
    RequireOk(fx.state.ledger.CheckConservation(), "stable-Q two-market conservation");
    // The remainder of the first order costs no second ceiling atom:
    // ceil(5005 * 100 / 1e6) - ceil(2002 * 100 / 1e6) == 0.
    const auto rest{fx.Place("alice", 3, MarketM1(), Side::BUY, TimeInForce::IOC, 1'001, 3)};
    const SlotResult second{fx.Apply(fx.Inputs({}, {rest}))};
    const OutcomeV2& taken{Outcome(second, rest, fx)};
    BOOST_REQUIRE_EQUAL(taken.fills.size(), 1U);
    BOOST_CHECK_EQUAL(taken.fills.front().seller_fee, 0);
    RequireOk(fx.state.ledger.CheckConservation(), "stable-Q partial-fill conservation");
    for (const OutcomeV2& outcome : second.outcomes) BOOST_CHECK(outcome.code != OutcomeCode::FEES_DISTRIBUTED);
}

BOOST_AUTO_TEST_CASE(stable_q_rejects_native_quote_at_creation_and_execution)
{
    const ProfileParams profile{StableQRegtestProfile(ChainType::REGTEST).value()};
    const std::vector<MarketDescriptor> bad{MarketM1(), MarketM3()};
    const auto genesis{EngineState::Genesis(profile, bad)};
    BOOST_REQUIRE(!genesis.has_value());
    BOOST_CHECK_EQUAL(genesis.error().detail, STABLE_QUOTE_REQUIRED);
    SlotFixture fx{profile};
    const SlotInputs inputs{fx.Inputs({}, {})};
    const auto valid{fx.Validate(inputs)};
    RequireOk(valid, "validate before incompatible state replacement");
    // Direct callers have access to value-type state; all application paths
    // must reject an incompatible book even when no order would touch it.
    const auto books{BookSet::Create(bad)};
    RequireOk(books, "structurally valid generic books");
    fx.state.books = *books;
    const auto rejected{fx.Validate(inputs)};
    BOOST_REQUIRE(!rejected.has_value());
    BOOST_CHECK_EQUAL(rejected.error().detail, STABLE_QUOTE_REQUIRED);
    const auto engine{Engine::Create(profile)};
    RequireOk(engine, "stable-Q engine");
    const auto applied{engine->ApplySlot(fx.Head(), *valid)};
    BOOST_REQUIRE(!applied.has_value());
    BOOST_CHECK_EQUAL(applied.error().detail, STABLE_QUOTE_REQUIRED);
}


BOOST_AUTO_TEST_CASE(stable_q_trade_withdraw_and_paid_effect_roundtrip)
{
    SlotFixture fx{StableQRegtestProfile(ChainType::REGTEST).value()};
    const auto alice{KeyAccount("alice")};
    const auto bob{KeyAccount("bob")};
    fx.Apply(fx.Inputs({fx.DepositBytes(alice, RUSD, 20'000, "stable/alice"),
                        fx.DepositBytes(bob, B3, 2'000'000, "stable/bob")}, {}));
    const auto maker{fx.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 100, 1)};
    fx.Apply(fx.Inputs({}, {maker}));
    const auto taker{fx.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::IOC, 100, 1)};
    const auto traded{fx.Apply(fx.Inputs({}, {taker}))};
    BOOST_CHECK(Outcome(traded, taker, fx).code == OutcomeCode::FILLED);
    BOOST_CHECK_EQUAL(Outcome(traded, taker, fx).fills.front().seller_fee, 1);
    BOOST_CHECK(fx.BalanceOf("alice", RUSD) == (ledger::Balance{19'900, 0}));

    const WithdrawPayload request{RUSD, 5'000, std::vector<unsigned char>(20, 0x42)};
    const auto action{fx.Sign(fx.Unsigned("alice", 2, ActionKind::WITHDRAW, EncodePayload(request).value()), "alice")};
    const ActionId id{ActionV2::Decode(action, fx.profile)->Id()};
    const auto withdrawn{fx.Apply(fx.Inputs({}, {action}))};
    BOOST_CHECK(OutcomeOf(withdrawn, id.id).code == OutcomeCode::WITHDRAWAL_PENDING);
    BOOST_CHECK(DeltasOf(OutcomeOf(withdrawn, id.id)) == (Deltas{{{alice, RUSD}, {-6'000, 0}}}));
    BOOST_CHECK(fx.BalanceOf("alice", RUSD) == (ledger::Balance{13'900, 0}));
    BOOST_CHECK_EQUAL(fx.state.ledger.TotalsOf(RUSD).pending, 5'000);
    BOOST_CHECK_EQUAL(fx.state.ledger.TotalsOf(RUSD).fees_held, 0);
    BOOST_CHECK_EQUAL(fx.state.ledger.BalanceOf(SeatPoolAccount(fx.profile), RUSD).available +
                      fx.state.ledger.BalanceOf(TreasuryAccount(fx.profile), RUSD).available, 1'001);
    BOOST_CHECK_EQUAL(fx.header.commitment_version, COMMITMENT_VERSION_WITHDRAWALS);

    PayoutConfirmedFact payout;
    payout.cursor = {fx.anchor_height, fx.next_fact_index++};
    payout.withdrawal = id;
    payout.account = alice;
    payout.asset = RUSD;
    payout.amount = 5'000;
    payout.recipient.fill(0x42);
    payout.payout_event = TestOutpoint("stable/payout", 0);
    payout.domain = fx.domain;
    const auto paid_bytes{EncodeFact(Fact{payout}).value()};
    const auto deposit{fx.DepositBytes(bob, RUSD, 7, "stable/late-deposit")};
    const auto paid{fx.Apply(fx.Inputs({paid_bytes, deposit}, {}))};
    BOOST_REQUIRE_EQUAL(paid.outcomes.size(), 2U);
    BOOST_CHECK(paid.outcomes[0].subject == FactLeaf(paid_bytes));
    BOOST_CHECK(paid.outcomes[0].code == OutcomeCode::WITHDRAWAL_PAID);
    BOOST_CHECK(paid.outcomes[0].deltas.empty());
    BOOST_CHECK(paid.outcomes[1].subject == FactLeaf(deposit));
    BOOST_CHECK(paid.outcomes[1].code == OutcomeCode::DEPOSIT_CREDITED);
    BOOST_CHECK_EQUAL(fx.state.ledger.TotalsOf(RUSD).pending, 0);
    BOOST_CHECK_EQUAL(fx.state.ledger.TotalsOf(RUSD).effects, 5'000);
    BOOST_CHECK(fx.state.ledger.Withdrawals().at(id).paid);
    BOOST_CHECK(fx.BalanceOf("alice", RUSD) == (ledger::Balance{13'900, 0}));
    payout.cursor = {fx.anchor_height, fx.next_fact_index++};
    fx.Apply(fx.Inputs({EncodeFact(Fact{payout}).value()}, {}));
    BOOST_CHECK_EQUAL(fx.state.ledger.TotalsOf(RUSD).effects, 5'000);
    RequireOk(fx.state.CheckInvariants(), "stable roundtrip conservation");
}

BOOST_AUTO_TEST_CASE(stable_q_payout_binding_and_insufficient_withdraw_are_atomic)
{
    SlotFixture fx{StableQRegtestProfile(ChainType::REGTEST).value()};
    fx.Apply(fx.Inputs({fx.DepositBytes(KeyAccount("alice"), RUSD, 10'000, "stable-negative/alice")}, {}));
    const WithdrawPayload request{RUSD, 5'000, std::vector<unsigned char>(20, 0x43)};
    const auto action{fx.Sign(fx.Unsigned("alice", 1, ActionKind::WITHDRAW, EncodePayload(request).value()), "alice")};
    const auto id{ActionV2::Decode(action, fx.profile)->Id()};
    PayoutConfirmedFact payout;
    payout.cursor = {fx.anchor_height, fx.next_fact_index++};
    payout.withdrawal = id;
    payout.account = KeyAccount("alice");
    payout.asset = RUSD;
    payout.amount = 5'000;
    payout.recipient.fill(0x43);
    payout.payout_event = TestOutpoint("stable-negative/payout", 0);
    payout.domain = fx.domain;
    BOOST_CHECK(!fx.TryApply(fx.Inputs({EncodeFact(Fact{payout}).value()}, {action})).has_value());
    --fx.next_fact_index; // refused same-slot fact did not advance the stream
    fx.Apply(fx.Inputs({}, {action}));
    ++fx.next_fact_index; // the payout now sits inside the next slot's range
    const auto before{fx.header.Hash()};
    for (int mutation = 0; mutation < 5; ++mutation) {
        auto wrong{payout};
        if (mutation == 0) wrong.domain = LabelHash("another-domain");
        if (mutation == 1) wrong.recipient[0] ^= 1;
        if (mutation == 2) wrong.amount += 1;
        if (mutation == 3) wrong.account = KeyAccount("bob");
        if (mutation == 4) wrong.withdrawal.id = LabelHash("unknown-claim");
        BOOST_CHECK(!fx.TryApply(fx.Inputs({EncodeFact(Fact{wrong}).value()}, {})).has_value());
        BOOST_CHECK(fx.header.Hash() == before);
        BOOST_CHECK_EQUAL(fx.state.ledger.TotalsOf(RUSD).pending, 5'000);
    }
    const auto insufficient{fx.Sign(fx.Unsigned("alice", 2, ActionKind::WITHDRAW, EncodePayload(request).value()), "alice")};
    const auto rejected{fx.Apply(fx.Inputs({}, {insufficient}))};
    CheckRejected(Outcome(rejected, insufficient, fx), OutcomeCode::REJ_INSUFFICIENT_AVAILABLE, "withdraw amount plus stable fee");
    BOOST_CHECK(fx.BalanceOf("alice", RUSD) == (ledger::Balance{4'000, 0}));
    BOOST_CHECK(HasNonce(fx.UsedNonces("alice"), 2));
    BOOST_CHECK_EQUAL(fx.state.ledger.Withdrawals().size(), 1U);
    RequireOk(fx.state.CheckInvariants(), "refused withdrawals conserve funds");
}

BOOST_AUTO_TEST_CASE(network_source_range_is_committed_without_ledger_effect)
{
    SlotFixture fx{StableQNetworkRegtestProfile(ChainType::REGTEST).value()};
    const auto before{fx.state.Commit().value().Root()};
    const auto source{NetworkSource(fx, 10, 10)};
    const auto first{fx.Apply(fx.Inputs({EncodeFact(source).value()}, {}))};
    BOOST_CHECK(first.outcomes.empty());
    BOOST_CHECK(first.roots.Root() == before);
    BOOST_CHECK(fx.header.facts_root != SlotBody{}.FactsRoot());
    BOOST_CHECK(fx.header.facts_to == (FactCursor{fx.anchor_height, 1}));
    BOOST_CHECK_EQUAL(fx.header.outcome_count, 0U);
    const auto deposit{fx.DepositBytes(KeyAccount("alice"), RUSD, 5'000, "source/alice")};
    const auto next{NetworkSource(fx)};
    const auto applied{fx.Apply(fx.Inputs({deposit, EncodeFact(next).value()}, {}))};
    BOOST_REQUIRE_EQUAL(applied.outcomes.size(), 1U);
    BOOST_CHECK(applied.outcomes.front().code == OutcomeCode::DEPOSIT_CREDITED);
    BOOST_CHECK(applied.outcomes.front().subject == FactLeaf(deposit));
    BOOST_CHECK(fx.BalanceOf("alice", RUSD) == (ledger::Balance{5'000, 0}));
    BOOST_CHECK_EQUAL(fx.state.ledger.TotalsOf(RUSD).effects, 0);
    RequireOk(fx.state.CheckInvariants(), "source boundary has no financial effect");
}

BOOST_AUTO_TEST_CASE(network_source_range_refusals_are_atomic)
{
    SlotFixture fx{StableQNetworkRegtestProfile(ChainType::REGTEST).value()};
    const auto deposit{fx.DepositBytes(KeyAccount("alice"), RUSD, 5'000, "source/negative")};
    const auto source{NetworkSource(fx)};
    const auto valid{fx.Inputs({deposit, EncodeFact(source).value()}, {})};
    const auto parent_hash{fx.header.Hash()}, parent_state{fx.state.Commit().value().Root()};
    BOOST_REQUIRE(fx.TryApply(valid).has_value());
    for (int mutation{0}; mutation < 8; ++mutation) {
        auto wrong{valid}; auto marker{source};
        if (mutation == 0) wrong.body.facts.pop_back();
        if (mutation == 1) {
            auto duplicate{source}; ++duplicate.cursor.index;
            wrong.body.facts.push_back(EncodeFact(duplicate).value());
            ++wrong.facts_to.index;
        }
        if (mutation == 2) std::swap(wrong.body.facts[0], wrong.body.facts[1]);
        if (mutation == 3) marker.domain = LabelHash("source/wrong-domain");
        if (mutation == 4) marker.chain_id = 1;
        if (mutation == 5) marker.confirmations = 1;
        if (mutation == 6) ++wrong.facts_to.index;
        if (mutation == 7) { ++marker.cursor.height; ++wrong.facts_to.height; }
        if (mutation >= 3) wrong.body.facts.back() = EncodeFact(marker).value();
        BOOST_CHECK_MESSAGE(!fx.TryApply(wrong).has_value(), "source-range mutation " << mutation << " was accepted");
        BOOST_CHECK(fx.header.Hash() == parent_hash);
        BOOST_CHECK(fx.state.Commit().value().Root() == parent_state);
        BOOST_CHECK(fx.BalanceOf("alice", RUSD) == (ledger::Balance{0, 0}));
    }
    auto empty{valid}; empty.body.facts.clear();
    BOOST_CHECK(!fx.TryApply(empty).has_value());
}

BOOST_AUTO_TEST_CASE(legacy_profiles_refuse_source_range_fact)
{
    for (const auto& profile : {RegtestProfile(), StableQRegtestProfile(ChainType::REGTEST).value(),
                               StableQViewRegtestProfile(ChainType::REGTEST).value()}) {
        SlotFixture fx{profile};
        const auto source{NetworkSource(fx)};
        const auto before{fx.state.Commit().value().Root()};
        BOOST_CHECK(!fx.TryApply(fx.Inputs({EncodeFact(source).value()}, {})).has_value());
        BOOST_CHECK(fx.state.Commit().value().Root() == before);
    }
}

BOOST_AUTO_TEST_CASE(network_source_boundaries_distinguish_identical_empty_event_sets)
{
    SlotFixture fx{StableQNetworkRegtestProfile(ChainType::REGTEST).value()};
    const auto first{NetworkSource(fx)};
    auto second{first}; ++second.to_height; second.to_hash = LabelHash("source/later-empty-block");
    const auto a{fx.Inputs({EncodeFact(first).value()}, {})};
    const auto b{fx.Inputs({EncodeFact(second).value()}, {})};
    const auto ar{fx.TryApply(a)}, br{fx.TryApply(b)};
    BOOST_REQUIRE(ar.has_value() && br.has_value());
    BOOST_CHECK(ar->roots.Root() == br->roots.Root());
    BOOST_CHECK(ar->outcomes.empty() && br->outcomes.empty());
    BOOST_CHECK(a.body.FactsRoot() != b.body.FactsRoot());
    auto left{fx}; auto right{fx};
    left.Advance(a, *ar); right.Advance(b, *br);
    BOOST_CHECK(left.header.Hash() != right.header.Hash());
}

BOOST_AUTO_TEST_SUITE_END()
