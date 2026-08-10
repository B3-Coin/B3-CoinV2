// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! FlowMesh persistent demand curves and deterministic batch clearing:
//! curve validity, integer evaluation, maximum-volume uniform-price
//! clearing with golden and differential checks, largest-remainder
//! allocation, reservation backing, solvency, determinism, and
//! adversarial overflow. No floating point; no per-fill UTXO spends.

#include <flowmesh/clearing.h>

#include <flowmesh/ledger.h>
#include <modern/policy.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <map>
#include <vector>

BOOST_AUTO_TEST_SUITE(flowmesh_clearing_tests)

namespace {

using Side = flowmesh::ClearingEngine::Side;
using Breakpoint = flowmesh::ClearingEngine::Breakpoint;

const uint256 VAULT{uint256{"00000000000000000000000000000000000000000000000000000000000000f1"}};
const flowmesh::AccountId ALICE{uint256{"00000000000000000000000000000000000000000000000000000000000000a1"}};
const flowmesh::AccountId BOB{uint256{"00000000000000000000000000000000000000000000000000000000000000b1"}};
const flowmesh::AccountId ACC_A{uint256{"0000000000000000000000000000000000000000000000000000000000000001"}};
const flowmesh::AccountId ACC_B{uint256{"0000000000000000000000000000000000000000000000000000000000000002"}};
const flowmesh::AccountId ACC_C{uint256{"0000000000000000000000000000000000000000000000000000000000000003"}};

//! Base asset; quote is native B3.
modern::AssetId BaseX()
{
    return modern::IssuanceAssetId(COutPoint{
        Txid::FromUint256(uint256{"0000000000000000000000000000000000000000000000000000000000000011"}), 0});
}
const modern::AssetId& Quote() { return modern::NativeAsset(); }

std::vector<Breakpoint> Pts(std::vector<std::pair<CAmount, CAmount>> raw)
{
    std::vector<Breakpoint> out;
    for (const auto& [p, q] : raw) out.push_back({p, q});
    return out;
}

} // namespace

BOOST_AUTO_TEST_CASE(curve_validity_bounds_and_monotonicity)
{
    flowmesh::Ledger ledger{VAULT};
    flowmesh::ClearingEngine eng{BaseX(), Quote(), ledger, /*max_k=*/3};

    // Valid bid (non-increasing, terminates at zero) and ask (non-decreasing).
    BOOST_CHECK(eng.CurveIsValid(Side::BID, Pts({{10, 100}, {20, 40}, {30, 0}})));
    BOOST_CHECK(eng.CurveIsValid(Side::ASK, Pts({{10, 20}, {20, 80}})));

    // K bound: 4 breakpoints exceeds max_k = 3.
    BOOST_CHECK(!eng.CurveIsValid(Side::BID, Pts({{1, 3}, {2, 2}, {3, 1}, {4, 0}})));
    // Empty is invalid.
    BOOST_CHECK(!eng.CurveIsValid(Side::BID, {}));
    // Non-ascending prices.
    BOOST_CHECK(!eng.CurveIsValid(Side::BID, Pts({{20, 40}, {10, 0}})));
    // Bid not monotone non-increasing.
    BOOST_CHECK(!eng.CurveIsValid(Side::BID, Pts({{10, 10}, {20, 20}, {30, 0}})));
    // Bid not terminating at zero.
    BOOST_CHECK(!eng.CurveIsValid(Side::BID, Pts({{10, 100}, {20, 40}})));
    // Ask not monotone non-decreasing.
    BOOST_CHECK(!eng.CurveIsValid(Side::ASK, Pts({{10, 80}, {20, 20}})));
}

BOOST_AUTO_TEST_CASE(integer_evaluation_clamps_and_floors)
{
    // Clamp flat outside the range.
    const auto bid{Pts({{10, 100}, {20, 40}, {30, 0}})};
    BOOST_CHECK_EQUAL(flowmesh::ClearingEngine::EvaluateCurve(bid, 5), 100);
    BOOST_CHECK_EQUAL(flowmesh::ClearingEngine::EvaluateCurve(bid, 10), 100);
    BOOST_CHECK_EQUAL(flowmesh::ClearingEngine::EvaluateCurve(bid, 20), 40);
    BOOST_CHECK_EQUAL(flowmesh::ClearingEngine::EvaluateCurve(bid, 30), 0);
    BOOST_CHECK_EQUAL(flowmesh::ClearingEngine::EvaluateCurve(bid, 100), 0);

    // Exact integer floor interpolation: (0,10)->(3,0) at price 1 is
    // 10 + floor(-10 * 1 / 3) = 10 + (-4) = 6.
    const auto slope{Pts({{0, 10}, {3, 0}})};
    BOOST_CHECK_EQUAL(flowmesh::ClearingEngine::EvaluateCurve(slope, 1), 6);
    BOOST_CHECK_EQUAL(flowmesh::ClearingEngine::EvaluateCurve(slope, 2), 3);
}

BOOST_AUTO_TEST_CASE(maximum_volume_clearing_golden_and_differential)
{
    flowmesh::Ledger ledger{VAULT};
    flowmesh::ClearingEngine eng{BaseX(), Quote(), ledger};

    // Alice bids, Bob asks. Back them with ledger reservations.
    BOOST_REQUIRE(ledger.Deposit(ALICE, Quote(), 2000));
    BOOST_REQUIRE(ledger.Deposit(BOB, BaseX(), 80));
    const auto bid{Pts({{10, 100}, {20, 40}, {30, 0}})};
    const auto ask{Pts({{10, 20}, {20, 80}})};
    BOOST_REQUIRE(eng.SubmitCurve(ALICE, Side::BID, bid));
    BOOST_REQUIRE(eng.SubmitCurve(BOB, Side::ASK, ask));

    // Worst-case reservations: bid max(100*20, 40*30) = 2000; ask 80.
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 2000);
    BOOST_CHECK_EQUAL(ledger.Reserved(BOB, BaseX()), 80);

    // Differential: independently recompute max-volume price over the
    // candidate levels {10, 20, 30}.
    CAmount best_price{0}, best_vol{-1};
    for (const CAmount p : {CAmount{10}, CAmount{20}, CAmount{30}}) {
        const CAmount vol{std::min(flowmesh::ClearingEngine::EvaluateCurve(bid, p),
                                   flowmesh::ClearingEngine::EvaluateCurve(ask, p))};
        if (vol > best_vol) { best_vol = vol; best_price = p; }
    }
    BOOST_CHECK_EQUAL(best_price, 20);
    BOOST_CHECK_EQUAL(best_vol, 40);

    const auto result{eng.ClearSlot()};
    // Golden.
    BOOST_CHECK(result.cleared);
    BOOST_CHECK_EQUAL(result.price, 20);
    BOOST_CHECK_EQUAL(result.volume, 40);
    BOOST_CHECK_EQUAL(result.bid_fill.at(ALICE), 40);
    BOOST_CHECK_EQUAL(result.ask_fill.at(BOB), 40);

    // Settlement is internal: Alice paid 40*20 = 800 quote, received 40
    // base; Bob delivered 40 base, received 800 quote. No UTXO touched.
    BOOST_CHECK_EQUAL(ledger.Available(ALICE, BaseX()), 40);
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 1200);
    BOOST_CHECK_EQUAL(ledger.Available(BOB, Quote()), 800);
    BOOST_CHECK_EQUAL(ledger.Reserved(BOB, BaseX()), 40);
    BOOST_CHECK_EQUAL(ledger.Custody(Quote()), 2000);
    BOOST_CHECK_EQUAL(ledger.Custody(BaseX()), 80);
    BOOST_CHECK(ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_CASE(largest_remainder_allocation_is_deterministic)
{
    flowmesh::Ledger ledger{VAULT};
    flowmesh::ClearingEngine eng{BaseX(), Quote(), ledger};

    // One bidder wanting 20 at price 5; three symmetric askers offering 10
    // each. The ask (long) side rations 20 across 30 desired.
    BOOST_REQUIRE(ledger.Deposit(ACC_C, Quote(), 120)); // bidder Z == ACC_C's role
    BOOST_REQUIRE(ledger.Deposit(ACC_A, BaseX(), 10));
    BOOST_REQUIRE(ledger.Deposit(ACC_B, BaseX(), 10));
    BOOST_REQUIRE(ledger.Deposit(ALICE, BaseX(), 10));

    BOOST_REQUIRE(eng.SubmitCurve(ACC_C, Side::BID, Pts({{5, 20}, {6, 0}})));
    BOOST_REQUIRE(eng.SubmitCurve(ACC_A, Side::ASK, Pts({{5, 10}})));
    BOOST_REQUIRE(eng.SubmitCurve(ACC_B, Side::ASK, Pts({{5, 10}})));
    BOOST_REQUIRE(eng.SubmitCurve(ALICE, Side::ASK, Pts({{5, 10}})));

    const auto result{eng.ClearSlot()};
    BOOST_CHECK_EQUAL(result.price, 5);
    BOOST_CHECK_EQUAL(result.volume, 20);
    // floor(10*20/30) = 6 each (18), remainder 2 to the two lowest-id
    // accounts: ACC_A (…01) and ACC_B (…02) get 7, ACC_C-ordered ALICE
    // (…a1) gets 6.
    BOOST_CHECK_EQUAL(result.ask_fill.at(ACC_A), 7);
    BOOST_CHECK_EQUAL(result.ask_fill.at(ACC_B), 7);
    BOOST_CHECK_EQUAL(result.ask_fill.at(ALICE), 6);
    BOOST_CHECK_EQUAL(result.bid_fill.at(ACC_C), 20);
    BOOST_CHECK(ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_CASE(reservation_backing_is_required)
{
    flowmesh::Ledger ledger{VAULT};
    flowmesh::ClearingEngine eng{BaseX(), Quote(), ledger};

    // Insufficient available funds: submission rejected, nothing reserved.
    BOOST_REQUIRE(ledger.Deposit(ALICE, Quote(), 100)); // needs 2000
    BOOST_CHECK(!eng.SubmitCurve(ALICE, Side::BID, Pts({{10, 100}, {20, 40}, {30, 0}})));
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 0);
    BOOST_CHECK_EQUAL(ledger.Available(ALICE, Quote()), 100);

    // A cancelled curve releases its reservation.
    BOOST_REQUIRE(ledger.Deposit(BOB, BaseX(), 80));
    BOOST_REQUIRE(eng.SubmitCurve(BOB, Side::ASK, Pts({{10, 20}, {20, 80}})));
    BOOST_CHECK_EQUAL(ledger.Reserved(BOB, BaseX()), 80);
    BOOST_CHECK(eng.CancelCurve(BOB, Side::ASK));
    BOOST_CHECK_EQUAL(ledger.Reserved(BOB, BaseX()), 0);
    BOOST_CHECK_EQUAL(ledger.Available(BOB, BaseX()), 80);
}

BOOST_AUTO_TEST_CASE(clearing_is_order_independent)
{
    const auto run{[](bool ask_first) {
        flowmesh::Ledger ledger{VAULT};
        flowmesh::ClearingEngine eng{BaseX(), Quote(), ledger};
        ledger.Deposit(ALICE, Quote(), 2000);
        ledger.Deposit(BOB, BaseX(), 80);
        if (ask_first) {
            eng.SubmitCurve(BOB, Side::ASK, Pts({{10, 20}, {20, 80}}));
            eng.SubmitCurve(ALICE, Side::BID, Pts({{10, 100}, {20, 40}, {30, 0}}));
        } else {
            eng.SubmitCurve(ALICE, Side::BID, Pts({{10, 100}, {20, 40}, {30, 0}}));
            eng.SubmitCurve(BOB, Side::ASK, Pts({{10, 20}, {20, 80}}));
        }
        const auto result{eng.ClearSlot()};
        return std::make_pair(result, eng.StateRoot());
    }};

    const auto [r1, root1]{run(false)};
    const auto [r2, root2]{run(true)};
    BOOST_CHECK_EQUAL(r1.price, r2.price);
    BOOST_CHECK_EQUAL(r1.volume, r2.volume);
    BOOST_CHECK(r1.bid_fill == r2.bid_fill);
    BOOST_CHECK(r1.ask_fill == r2.ask_fill);
    // The deterministic state root is submission-order independent.
    BOOST_CHECK_EQUAL(root1.GetHex(), root2.GetHex());
}

BOOST_AUTO_TEST_CASE(no_clearing_without_both_sides)
{
    flowmesh::Ledger ledger{VAULT};
    flowmesh::ClearingEngine eng{BaseX(), Quote(), ledger};
    ledger.Deposit(ALICE, Quote(), 2000);
    BOOST_REQUIRE(eng.SubmitCurve(ALICE, Side::BID, Pts({{10, 100}, {20, 40}, {30, 0}})));

    const auto result{eng.ClearSlot()};
    BOOST_CHECK(!result.cleared);
    BOOST_CHECK_EQUAL(result.volume, 0);
    // Reservation untouched; the curve stands for a future slot.
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 2000);
    BOOST_CHECK(ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_CASE(adversarial_overflow_is_rejected)
{
    flowmesh::Ledger ledger{VAULT};
    flowmesh::ClearingEngine eng{BaseX(), Quote(), ledger};
    ledger.Deposit(ALICE, Quote(), MAX_MONEY);

    // A bid whose worst-case spend (qty * price) overflows the monetary
    // range is rejected outright; nothing is reserved.
    BOOST_CHECK(!eng.SubmitCurve(ALICE, Side::BID, Pts({{1, MAX_MONEY}, {2, 0}})));
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 0);
    BOOST_CHECK(ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_SUITE_END()
