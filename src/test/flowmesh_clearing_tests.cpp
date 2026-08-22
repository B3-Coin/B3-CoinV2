// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! FlowMesh persistent demand curves and deterministic batch clearing:
//! curve validity, integer evaluation, maximum-volume uniform-price
//! clearing with golden and differential checks, largest-remainder
//! allocation, reservation backing, solvency, determinism, and
//! adversarial overflow. No floating point; no per-fill UTXO spends.

#include <flowmesh/clearing.h>
#include <test/util/asset.h>

#include <flowmesh/state.h>
#include <test/util/flowmesh.h>

#include <flowmesh/ledger.h>
#include <hash.h>
#include <modern/policy.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <limits>
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
    return modern::test_only::SyntheticAssetId(COutPoint{
        Txid::FromUint256(uint256{"0000000000000000000000000000000000000000000000000000000000000011"}), 0});
}
const modern::AssetId& Quote() { return modern::NativeAsset(); }

std::vector<Breakpoint> Pts(std::vector<std::pair<CAmount, CAmount>> raw)
{
    std::vector<Breakpoint> out;
    for (const auto& [p, q] : raw) out.push_back({p, q});
    return out;
}

// Pinned empty-book commitment (VAULT ledger, slot 0) under the fully
// framed format: clearing v2 (curve/breakpoint counts) embedding ledger
// v2 (balance/custody/receipt counts). Filled from the first computed
// value, frozen since; changes only with a reviewed format bump.
const std::string EMPTY_BOOK_ROOT_HEX{
    "1345b1674a98777966892548d7fdb38f02be7f60ed831acff3712d6b6480325c"};


//! Test funding shortcut over the test-only bridge.
inline bool Fund(flowmesh::FlowMeshState& state, const flowmesh::AccountId& account,
                 const modern::AssetId& asset, const CAmount amount)
{
    return flowmesh::test_only::StateFunding::Fund(state, account, asset, amount);
}

} // namespace

BOOST_AUTO_TEST_CASE(curve_validity_bounds_and_monotonicity)
{
    flowmesh::FlowMeshState st{VAULT, BaseX(), Quote(), /*max_k=*/3};
    const flowmesh::Ledger& ledger{st.LedgerView()};

    // Valid bid (non-increasing, terminates at zero) and ask (non-decreasing).
    BOOST_CHECK(st.CurveIsValid(Side::BID, Pts({{10, 100}, {20, 40}, {30, 0}})));
    BOOST_CHECK(st.CurveIsValid(Side::ASK, Pts({{10, 20}, {20, 80}})));

    // K bound: 4 breakpoints exceeds max_k = 3.
    BOOST_CHECK(!st.CurveIsValid(Side::BID, Pts({{1, 3}, {2, 2}, {3, 1}, {4, 0}})));
    // Empty is invalid.
    BOOST_CHECK(!st.CurveIsValid(Side::BID, {}));
    // Non-ascending prices.
    BOOST_CHECK(!st.CurveIsValid(Side::BID, Pts({{20, 40}, {10, 0}})));
    // Bid not monotone non-increasing.
    BOOST_CHECK(!st.CurveIsValid(Side::BID, Pts({{10, 10}, {20, 20}, {30, 0}})));
    // Bid not terminating at zero.
    BOOST_CHECK(!st.CurveIsValid(Side::BID, Pts({{10, 100}, {20, 40}})));
    // Ask not monotone non-decreasing.
    BOOST_CHECK(!st.CurveIsValid(Side::ASK, Pts({{10, 80}, {20, 20}})));
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
    flowmesh::FlowMeshState st{VAULT, BaseX(), Quote()};
    const flowmesh::Ledger& ledger{st.LedgerView()};

    // Alice bids, Bob asks. Back them with ledger reservations.
    BOOST_REQUIRE(Fund(st, ALICE, Quote(), 2400));
    BOOST_REQUIRE(Fund(st, BOB, BaseX(), 80));
    const auto bid{Pts({{10, 100}, {20, 40}, {30, 0}})};
    const auto ask{Pts({{10, 20}, {20, 80}})};
    BOOST_REQUIRE(st.SubmitCurve(ALICE, Side::BID, bid));
    BOOST_REQUIRE(st.SubmitCurve(BOB, Side::ASK, ask));

    // Worst-case reservations under the exact integer-price bound
    // Σ (q_i − q_{i+1})·(p_{i+1} − 1): (100-40)*19 + 40*29 = 2300; ask 80.
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 2300);
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

    const auto result{*st.ClearSlot()};
    // Golden.
    BOOST_CHECK(result.cleared);
    BOOST_CHECK_EQUAL(result.price, 20);
    BOOST_CHECK_EQUAL(result.volume, 40);
    BOOST_CHECK_EQUAL(result.bid_fill.at(ALICE), 40);
    BOOST_CHECK_EQUAL(result.ask_fill.at(BOB), 40);

    // Settlement is internal: Alice paid 40*20 = 800 quote, received 40
    // base; Bob delivered 40 base, received 800 quote. No UTXO touched.
    BOOST_CHECK_EQUAL(ledger.Available(ALICE, BaseX()), 40);
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 1500); // 2300 − 800 spent
    BOOST_CHECK_EQUAL(ledger.Available(BOB, Quote()), 800);
    BOOST_CHECK_EQUAL(ledger.Reserved(BOB, BaseX()), 40);
    BOOST_CHECK_EQUAL(ledger.Custody(Quote()), 2400);
    BOOST_CHECK_EQUAL(ledger.Custody(BaseX()), 80);
    BOOST_CHECK(ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_CASE(largest_remainder_allocation_is_deterministic)
{
    flowmesh::FlowMeshState st{VAULT, BaseX(), Quote()};
    const flowmesh::Ledger& ledger{st.LedgerView()};

    // One bidder wanting 20 at price 5; three symmetric askers offering 10
    // each. The ask (long) side rations 20 across 30 desired.
    BOOST_REQUIRE(Fund(st, ACC_C, Quote(), 120)); // bidder Z == ACC_C's role
    BOOST_REQUIRE(Fund(st, ACC_A, BaseX(), 10));
    BOOST_REQUIRE(Fund(st, ACC_B, BaseX(), 10));
    BOOST_REQUIRE(Fund(st, ALICE, BaseX(), 10));

    BOOST_REQUIRE(st.SubmitCurve(ACC_C, Side::BID, Pts({{5, 20}, {6, 0}})));
    BOOST_REQUIRE(st.SubmitCurve(ACC_A, Side::ASK, Pts({{5, 10}})));
    BOOST_REQUIRE(st.SubmitCurve(ACC_B, Side::ASK, Pts({{5, 10}})));
    BOOST_REQUIRE(st.SubmitCurve(ALICE, Side::ASK, Pts({{5, 10}})));

    const auto result{*st.ClearSlot()};
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
    flowmesh::FlowMeshState st{VAULT, BaseX(), Quote()};
    const flowmesh::Ledger& ledger{st.LedgerView()};

    // Insufficient available funds: submission rejected, nothing reserved.
    BOOST_REQUIRE(Fund(st, ALICE, Quote(), 100)); // needs 2300
    BOOST_CHECK(!st.SubmitCurve(ALICE, Side::BID, Pts({{10, 100}, {20, 40}, {30, 0}})));
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 0);
    BOOST_CHECK_EQUAL(ledger.Available(ALICE, Quote()), 100);

    // A cancelled curve releases its reservation.
    BOOST_REQUIRE(Fund(st, BOB, BaseX(), 80));
    BOOST_REQUIRE(st.SubmitCurve(BOB, Side::ASK, Pts({{10, 20}, {20, 80}})));
    BOOST_CHECK_EQUAL(ledger.Reserved(BOB, BaseX()), 80);
    BOOST_CHECK(st.CancelCurve(BOB, Side::ASK));
    BOOST_CHECK_EQUAL(ledger.Reserved(BOB, BaseX()), 0);
    BOOST_CHECK_EQUAL(ledger.Available(BOB, BaseX()), 80);
}

BOOST_AUTO_TEST_CASE(clearing_is_order_independent)
{
    const auto run{[](bool ask_first) {
        flowmesh::FlowMeshState st{VAULT, BaseX(), Quote()};
        const flowmesh::Ledger& ledger{st.LedgerView()};
        Fund(st, ALICE, Quote(), 2400);
        Fund(st, BOB, BaseX(), 80);
        if (ask_first) {
            st.SubmitCurve(BOB, Side::ASK, Pts({{10, 20}, {20, 80}}));
            st.SubmitCurve(ALICE, Side::BID, Pts({{10, 100}, {20, 40}, {30, 0}}));
        } else {
            st.SubmitCurve(ALICE, Side::BID, Pts({{10, 100}, {20, 40}, {30, 0}}));
            st.SubmitCurve(BOB, Side::ASK, Pts({{10, 20}, {20, 80}}));
        }
        const auto result{*st.ClearSlot()};
        return std::make_pair(result, st.BookRoot());
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
    flowmesh::FlowMeshState st{VAULT, BaseX(), Quote()};
    const flowmesh::Ledger& ledger{st.LedgerView()};
    Fund(st, ALICE, Quote(), 2400);
    BOOST_REQUIRE(st.SubmitCurve(ALICE, Side::BID, Pts({{10, 100}, {20, 40}, {30, 0}})));

    const auto result{*st.ClearSlot()};
    BOOST_CHECK(!result.cleared);
    BOOST_CHECK_EQUAL(result.volume, 0);
    // Reservation untouched; the curve stands for a future slot.
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 2300);
    BOOST_CHECK(ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_CASE(adversarial_overflow_is_rejected)
{
    flowmesh::FlowMeshState st{VAULT, BaseX(), Quote()};
    const flowmesh::Ledger& ledger{st.LedgerView()};
    Fund(st, ALICE, Quote(), MAX_MONEY);

    // A bid whose worst-case spend overflows the monetary range is
    // rejected outright; nothing is reserved. (MAX_MONEY lots that can
    // only fill at up to price 2: worst = 2·MAX_MONEY, overflow.)
    BOOST_CHECK(!st.SubmitCurve(ALICE, Side::BID, Pts({{2, MAX_MONEY}, {3, 0}})));
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 0);
    // Exact boundary: the same quantity fillable only at up to price 1
    // needs exactly MAX_MONEY — accepted with a full-balance reservation.
    BOOST_CHECK(st.SubmitCurve(ALICE, Side::BID, Pts({{1, MAX_MONEY}, {2, 0}})));
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), MAX_MONEY);
    BOOST_CHECK(ledger.SolvencyHolds());
}

//! D5 fix: curve evaluation is a TOTAL function — no crash on empty or
//! garbage input, and interpolation is exact at magnitudes where the
//! old 64-bit product overflowed.
BOOST_AUTO_TEST_CASE(curve_evaluation_is_total_and_exact)
{
    using flowmesh::ClearingEngine;
    // Empty curve: zero demand, never undefined behavior.
    BOOST_CHECK_EQUAL(ClearingEngine::EvaluateCurve({}, 0), 0);
    BOOST_CHECK_EQUAL(ClearingEngine::EvaluateCurve({}, MAX_MONEY), 0);
    // Garbage shapes (equal / descending prices — impossible for stored
    // validated curves) degrade deterministically instead of dividing by
    // zero or reading out of range.
    BOOST_CHECK_EQUAL(ClearingEngine::EvaluateCurve(Pts({{10, 5}, {10, 9}}), 10), 5);
    BOOST_CHECK_EQUAL(ClearingEngine::EvaluateCurve(Pts({{10, 5}, {3, 9}}), 5), 5);
    // Exact interpolation where the old (b.qty − a.qty) · (price −
    // a.price) 64-bit product overflowed: MAX_MONEY · 9,999 ≈ 6.6e21.
    const auto big{Pts({{0, MAX_MONEY}, {10'000, 0}})};
    BOOST_CHECK_EQUAL(ClearingEngine::EvaluateCurve(big, 9'999), MAX_MONEY / 10'000);
    BOOST_CHECK_EQUAL(ClearingEngine::EvaluateCurve(big, 1),
                      MAX_MONEY - MAX_MONEY / 10'000);
    BOOST_CHECK_EQUAL(ClearingEngine::EvaluateCurve(big, 5'000), MAX_MONEY / 2);
}

//! D5 fix: settlement consumes the recorded reservation exactly, so
//! cancel and exhaustion release precisely the remainder — nothing is
//! stranded in `reserved` forever (the old code released the ORIGINAL
//! amount, which failed after any partial fill and silently froze the
//! rest, invisible to the solvency invariant).
BOOST_AUTO_TEST_CASE(partial_fill_reservations_release_exactly)
{
    flowmesh::FlowMeshState st{VAULT, BaseX(), Quote()};
    const flowmesh::Ledger& ledger{st.LedgerView()};
    BOOST_REQUIRE(Fund(st, ALICE, Quote(), 2400));
    BOOST_REQUIRE(Fund(st, BOB, BaseX(), 80));
    BOOST_REQUIRE(st.SubmitCurve(ALICE, Side::BID, Pts({{10, 100}, {20, 40}, {30, 0}})));
    BOOST_REQUIRE(st.SubmitCurve(BOB, Side::ASK, Pts({{10, 20}, {20, 80}})));
    const auto result{*st.ClearSlot()}; // 40 lots at price 20: spend 800
    BOOST_REQUIRE(result.cleared);
    BOOST_REQUIRE_EQUAL(result.volume, 40);

    // Partial fill consumed 800 quote of Alice's 2300 reservation and 40
    // base of Bob's; cancelling both releases EXACTLY the remainder.
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 1500);
    BOOST_CHECK(st.CancelCurve(ALICE, Side::BID));
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 0);
    BOOST_CHECK_EQUAL(ledger.Available(ALICE, Quote()), 1600); // 100 + 1500 released
    BOOST_CHECK_EQUAL(ledger.Reserved(BOB, BaseX()), 40);
    BOOST_CHECK(st.CancelCurve(BOB, Side::ASK));
    BOOST_CHECK_EQUAL(ledger.Reserved(BOB, BaseX()), 0);
    BOOST_CHECK_EQUAL(ledger.Available(BOB, BaseX()), 40);
    BOOST_CHECK(ledger.SolvencyHolds());

    // Fresh resubmission after the cancellations above (both curves are
    // gone, nothing is left to replace): reserves the new staircase
    // bound from scratch out of the released funds.
    BOOST_REQUIRE(st.SubmitCurve(ALICE, Side::BID, Pts({{10, 10}, {20, 0}})));
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 190); // 10·(20−1)
    BOOST_CHECK(ledger.SolvencyHolds());
}

//! D5+ (found in adversarial self-review, beyond the catalogue): a
//! PERSISTENT bid can be filled across many slots at a descending
//! sequence of prices whose total spend exceeds any single-slot
//! max-rectangle bound. The reservation is the exact integer-price
//! staircase sum Σ (q_i − q_{i+1})·(price_{i+1} − 1) — here 870 versus
//! the old max-rectangle 600 — and this path spends 601, which the old
//! bound could not honor.
BOOST_AUTO_TEST_CASE(persistent_bid_survives_descending_price_fill_sequence)
{
    flowmesh::FlowMeshState st{VAULT, BaseX(), Quote()};
    const flowmesh::Ledger& ledger{st.LedgerView()};
    // Bid staircase: (30−20)·19 + (20−10)·29 + (10−0)·39 = 870.
    // Old max-rectangle bound: max(30·20, 20·30, 10·40) = 600.
    BOOST_REQUIRE(Fund(st, ALICE, Quote(), 870));
    BOOST_REQUIRE(Fund(st, BOB, BaseX(), 63));
    BOOST_REQUIRE(st.SubmitCurve(ALICE, Side::BID,
                                  Pts({{10, 30}, {20, 20}, {30, 10}, {40, 0}})));
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 870);

    CAmount total_spend{0};
    const auto sell_slot{[&](const std::vector<Breakpoint>& ask, const CAmount want_price,
                             const CAmount want_volume) {
        BOOST_REQUIRE(st.SubmitCurve(BOB, Side::ASK, ask));
        const auto result{*st.ClearSlot()};
        BOOST_REQUIRE(result.cleared);
        BOOST_CHECK_EQUAL(result.price, want_price);
        BOOST_CHECK_EQUAL(result.volume, want_volume);
        total_spend += want_price * want_volume;
        BOOST_CHECK(ledger.SolvencyHolds());
    }};
    // Descending-price fill sequence: 5@35, 10@25, 13@12, 2@10.
    sell_slot(Pts({{34, 0}, {35, 5}}), 35, 5);    // spend 175
    sell_slot(Pts({{24, 0}, {25, 10}}), 25, 10);  // spend 250
    sell_slot(Pts({{11, 0}, {12, 18}}), 12, 13);  // spend 156
    sell_slot(Pts({{9, 0}, {10, 30}}), 10, 2);    // spend 20 — bid exhausted

    BOOST_CHECK_EQUAL(total_spend, 601); // > 600: max-rectangle was insufficient
    // The exhausted bid released its exact remainder (870 − 601 = 269).
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 0);
    BOOST_CHECK_EQUAL(ledger.Available(ALICE, Quote()), 269);
    BOOST_CHECK_EQUAL(ledger.Available(ALICE, BaseX()), 30); // 30 lots bought
    // Bob: sold 30 of 63 base; cancel the partly-filled final ask —
    // exact remainder released.
    BOOST_CHECK(st.CancelCurve(BOB, Side::ASK));
    BOOST_CHECK_EQUAL(ledger.Reserved(BOB, BaseX()), 0);
    BOOST_CHECK_EQUAL(ledger.Available(BOB, BaseX()), 33);
    BOOST_CHECK_EQUAL(ledger.Available(BOB, Quote()), 601);
    BOOST_CHECK(ledger.SolvencyHolds());
}

//! Pass-2 fix: SubmitCurve is atomic. A failed FIRST submission or a
//! failed REPLACEMENT leaves balances, reservations, the curve, the
//! effective quantity and the state root byte-identical — no ghost
//! curve is ever inserted.
BOOST_AUTO_TEST_CASE(submit_curve_is_atomic)
{
    flowmesh::FlowMeshState st{VAULT, BaseX(), Quote()};
    const flowmesh::Ledger& ledger{st.LedgerView()};

    // Failed FIRST submission (valid curve, unfunded account).
    const uint256 empty_root{st.BookRoot()};
    BOOST_CHECK(!st.SubmitCurve(ALICE, Side::BID, Pts({{10, 100}, {20, 40}, {30, 0}})));
    BOOST_CHECK_EQUAL(st.BookRoot().GetHex(), empty_root.GetHex()); // no ghost curve
    BOOST_CHECK_EQUAL(st.EffectiveQty(Side::BID, ALICE, 10), 0);
    BOOST_CHECK(!st.CancelCurve(ALICE, Side::BID)); // nothing to cancel
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 0);

    // Failed REPLACEMENT preserves the previous curve entirely.
    BOOST_REQUIRE(Fund(st, ALICE, Quote(), 300));
    BOOST_REQUIRE(st.SubmitCurve(ALICE, Side::BID, Pts({{10, 10}, {20, 0}}))); // needs 190
    const uint256 standing_root{st.BookRoot()};
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 190);
    // Replacement needs 2300; only 110 more is available: rejected.
    BOOST_CHECK(!st.SubmitCurve(ALICE, Side::BID, Pts({{10, 100}, {20, 40}, {30, 0}})));
    BOOST_CHECK_EQUAL(st.BookRoot().GetHex(), standing_root.GetHex());
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 190);
    BOOST_CHECK_EQUAL(st.EffectiveQty(Side::BID, ALICE, 10), 10);

    // Successful replacement adjusts by the DELTA only.
    BOOST_REQUIRE(st.SubmitCurve(ALICE, Side::BID, Pts({{10, 5}, {20, 0}}))); // needs 95
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 95);
    BOOST_CHECK_EQUAL(ledger.Available(ALICE, Quote()), 205);
}

//! Pass-2 fix: zero-demand curves are rejected — an unfunded account
//! cannot add candidate prices or permanent book state for free.
BOOST_AUTO_TEST_CASE(zero_demand_curves_are_rejected)
{
    flowmesh::FlowMeshState st{VAULT, BaseX(), Quote()};
    const flowmesh::Ledger& ledger{st.LedgerView()};
    // Validity: a bid must OPEN positive (and still terminate at zero);
    // an ask must REACH positive.
    BOOST_CHECK(!st.CurveIsValid(Side::BID, Pts({{10, 0}})));
    BOOST_CHECK(!st.CurveIsValid(Side::BID, Pts({{10, 0}, {20, 0}})));
    BOOST_CHECK(!st.CurveIsValid(Side::ASK, Pts({{10, 0}})));
    BOOST_CHECK(!st.CurveIsValid(Side::ASK, Pts({{5, 0}, {10, 0}})));
    BOOST_CHECK(st.CurveIsValid(Side::BID, Pts({{10, 1}, {20, 0}})));
    BOOST_CHECK(st.CurveIsValid(Side::ASK, Pts({{5, 0}, {10, 1}})));

    // A zero-balance account gets NOTHING into the book for free: the
    // state root — and with it the candidate-price set and per-slot scan
    // work — is untouched by its attempts.
    const uint256 empty_root{st.BookRoot()};
    BOOST_CHECK(!st.SubmitCurve(ALICE, Side::BID, Pts({{10, 0}})));
    BOOST_CHECK(!st.SubmitCurve(ALICE, Side::ASK, Pts({{999, 0}})));
    BOOST_CHECK_EQUAL(st.BookRoot().GetHex(), empty_root.GetHex());
    BOOST_CHECK(ledger.SolvencyHolds());
}

//! Pass-2 fix: EvaluateCurve is genuinely total — out-of-MoneyRange
//! points yield the deterministic zero result, and every subtraction is
//! widened to 128 bits BEFORE it happens, so INT64_MIN/INT64_MAX inputs
//! cannot overflow any intermediate.
BOOST_AUTO_TEST_CASE(curve_evaluation_extreme_inputs)
{
    using flowmesh::ClearingEngine;
    constexpr CAmount min64{std::numeric_limits<int64_t>::min()};
    constexpr CAmount max64{std::numeric_limits<int64_t>::max()};
    // Out-of-range points: deterministic zero.
    BOOST_CHECK_EQUAL(ClearingEngine::EvaluateCurve(Pts({{min64, 5}, {10, 0}}), 1), 0);
    BOOST_CHECK_EQUAL(ClearingEngine::EvaluateCurve(Pts({{0, max64}, {10, 0}}), 5), 0);
    BOOST_CHECK_EQUAL(ClearingEngine::EvaluateCurve(Pts({{-1, 5}, {10, 0}}), 5), 0);
    BOOST_CHECK_EQUAL(ClearingEngine::EvaluateCurve(Pts({{0, -1}, {10, 0}}), 5), 0);
    // Extreme PRICE arguments against a valid curve: clamp, exactly.
    const auto valid{Pts({{10, 100}, {20, 40}, {30, 0}})};
    BOOST_CHECK_EQUAL(ClearingEngine::EvaluateCurve(valid, min64), 100);
    BOOST_CHECK_EQUAL(ClearingEngine::EvaluateCurve(valid, max64), 0);
}

//! Pass-2 fix: replacement works directly after a partial fill (no
//! cancel in between), adjusting by the exact delta of the remaining
//! reservation.
BOOST_AUTO_TEST_CASE(replacement_after_partial_fill_without_cancel)
{
    flowmesh::FlowMeshState st{VAULT, BaseX(), Quote()};
    const flowmesh::Ledger& ledger{st.LedgerView()};
    BOOST_REQUIRE(Fund(st, ALICE, Quote(), 2400));
    BOOST_REQUIRE(Fund(st, BOB, BaseX(), 80));
    BOOST_REQUIRE(st.SubmitCurve(ALICE, Side::BID, Pts({{10, 100}, {20, 40}, {30, 0}})));
    BOOST_REQUIRE(st.SubmitCurve(BOB, Side::ASK, Pts({{10, 20}, {20, 80}})));
    const auto result{*st.ClearSlot()}; // 40 @ 20: Alice spends 800
    BOOST_REQUIRE(result.cleared);
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 1500); // 2300 − 800

    // Direct replacement: new need 190, delta-release 1310.
    BOOST_REQUIRE(st.SubmitCurve(ALICE, Side::BID, Pts({{10, 10}, {20, 0}})));
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 190);
    BOOST_CHECK_EQUAL(ledger.Available(ALICE, Quote()), 1410);
    BOOST_CHECK_EQUAL(st.EffectiveQty(Side::BID, ALICE, 10), 10); // filled reset
    BOOST_CHECK(ledger.SolvencyHolds());
}

//! Pass-2/3 fix: the book commitment is canonically framed (clearing v2
//! domain): curve and breakpoint counts precede the variable-length
//! data. The layout comparison here genuinely isolates the curve
//! framing: both engines hold BYTE-IDENTICAL ledger state (equal ledger
//! roots, asserted), the same flattened breakpoint stream, the same
//! per-curve filled/reserved values and the same account order — only
//! the placement of the curve boundary differs. And the framed preimage
//! is reconstructed byte-exactly: with the counts it reproduces the
//! engine root; without them it does not — the counts are load-bearing,
//! not decorative.
BOOST_AUTO_TEST_CASE(state_root_is_canonically_framed)
{
    // Pinned empty-book vector (clearing v2 embedding ledger v2).
    {
        flowmesh::FlowMeshState st{VAULT, BaseX(), Quote()};
        const flowmesh::Ledger& ledger{st.LedgerView()};
        BOOST_CHECK_EQUAL(st.BookRoot().GetHex(), EMPTY_BOOK_ROOT_HEX);
    }

    // Two curve-book layouts over the same flattened breakpoint stream
    // (10,5),(20,5),(30,7), split [1|2] versus [2|1]. Ask reservations
    // are the terminal quantities (5 and 7) in BOTH layouts and deposits
    // match them exactly, so the two ledgers finish byte-identical.
    flowmesh::FlowMeshState st_x{VAULT, BaseX(), Quote()};
    const flowmesh::Ledger& ledger_x{st_x.LedgerView()};
    BOOST_REQUIRE(Fund(st_x, ALICE, BaseX(), 5));
    BOOST_REQUIRE(Fund(st_x, BOB, BaseX(), 7));
    BOOST_REQUIRE(st_x.SubmitCurve(ALICE, Side::ASK, Pts({{10, 5}})));
    BOOST_REQUIRE(st_x.SubmitCurve(BOB, Side::ASK, Pts({{20, 5}, {30, 7}})));

    flowmesh::FlowMeshState st_y{VAULT, BaseX(), Quote()};
    const flowmesh::Ledger& ledger_y{st_y.LedgerView()};
    BOOST_REQUIRE(Fund(st_y, ALICE, BaseX(), 5));
    BOOST_REQUIRE(Fund(st_y, BOB, BaseX(), 7));
    BOOST_REQUIRE(st_y.SubmitCurve(ALICE, Side::ASK, Pts({{10, 5}, {20, 5}})));
    BOOST_REQUIRE(st_y.SubmitCurve(BOB, Side::ASK, Pts({{30, 7}})));

    // The ledger halves of the commitment really are identical, so the
    // engine roots below can differ only through the curve-book framing.
    BOOST_REQUIRE_EQUAL(ledger_x.StateRoot().GetHex(), ledger_y.StateRoot().GetHex());
    BOOST_CHECK(st_x.BookRoot() != st_y.BookRoot());

    // Byte-exact reconstruction of layout X's preimage. Curves iterate
    // in (side, account) map order: (ASK, ALICE), then (ASK, BOB).
    const auto reconstruct{[&](const bool framed) {
        HashWriter h;
        h << std::string{"b3/flowmesh/clearing/v2"} << BaseX() << Quote()
          << static_cast<uint64_t>(8) /*default max_k*/ << ledger_x.Slot();
        if (framed) h << static_cast<uint64_t>(2); // curve count
        h << static_cast<uint8_t>(Side::ASK) << ALICE << CAmount{0} << CAmount{5};
        if (framed) h << static_cast<uint64_t>(1); // breakpoint count
        h << CAmount{10} << CAmount{5};
        h << static_cast<uint8_t>(Side::ASK) << BOB << CAmount{0} << CAmount{7};
        if (framed) h << static_cast<uint64_t>(2); // breakpoint count
        h << CAmount{20} << CAmount{5} << CAmount{30} << CAmount{7};
        h << ledger_x.StateRoot();
        return h.GetHash();
    }};
    BOOST_CHECK_EQUAL(reconstruct(true).GetHex(), st_x.BookRoot().GetHex());
    BOOST_CHECK(reconstruct(false) != st_x.BookRoot());
}

//! Pass-3 fix: a ZERO uniform clearing price is a valid outcome (curve
//! validity deliberately permits price-0 breakpoints — banning them
//! would be a new market-economics decision, not a bug fix), and
//! settlement handles it atomically: the base leg settles in full while
//! the quote leg is an explicit successful no-op, because zero value
//! changes hands on the quote side. Before this fix the zero-amount
//! quote move was REJECTED by the ledger while the base move succeeded,
//! and the engine asserted only after that partial mutation.
BOOST_AUTO_TEST_CASE(zero_price_clearing_settles_base_with_noop_quote_leg)
{
    flowmesh::FlowMeshState st{VAULT, BaseX(), Quote()};
    const flowmesh::Ledger& ledger{st.LedgerView()};
    // A bid fillable ONLY at price 0 can spend nothing: its exact
    // integer-price reservation is ZERO — Alice needs no quote at all.
    BOOST_REQUIRE(Fund(st, ALICE, Quote(), 10));
    BOOST_REQUIRE(Fund(st, BOB, BaseX(), 10));
    BOOST_REQUIRE(st.SubmitCurve(ALICE, Side::BID, Pts({{0, 10}, {1, 0}})));
    BOOST_REQUIRE(st.SubmitCurve(BOB, Side::ASK, Pts({{0, 10}})));
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 0);
    BOOST_CHECK_EQUAL(ledger.Reserved(BOB, BaseX()), 10);

    // The slot clears 10 lots at price 0 — no assertion, no partial
    // failure.
    const auto result{*st.ClearSlot()};
    BOOST_REQUIRE(result.cleared);
    BOOST_CHECK_EQUAL(result.price, 0);
    BOOST_CHECK_EQUAL(result.volume, 10);
    BOOST_CHECK_EQUAL(result.bid_fill.at(ALICE), 10);
    BOOST_CHECK_EQUAL(result.ask_fill.at(BOB), 10);

    // NO quote balance moved anywhere: Alice's 10 quote is back available
    // (the exhausted bid released its whole reservation, none consumed),
    // and Bob received none.
    BOOST_CHECK_EQUAL(ledger.Available(ALICE, Quote()), 10);
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, Quote()), 0);
    BOOST_CHECK_EQUAL(ledger.Available(BOB, Quote()), 0);
    BOOST_CHECK_EQUAL(ledger.Reserved(BOB, Quote()), 0);
    // 10 base units moved from seller to buyer.
    BOOST_CHECK_EQUAL(ledger.Available(ALICE, BaseX()), 10);
    BOOST_CHECK_EQUAL(ledger.Available(BOB, BaseX()), 0);
    BOOST_CHECK_EQUAL(ledger.Reserved(BOB, BaseX()), 0);
    // Both curves filled exactly to exhaustion: erased from the book,
    // reservations fully unwound.
    BOOST_CHECK_EQUAL(st.EffectiveQty(Side::BID, ALICE, 0), 0);
    BOOST_CHECK_EQUAL(st.EffectiveQty(Side::ASK, BOB, 0), 0);
    BOOST_CHECK(!st.CancelCurve(ALICE, Side::BID)); // nothing left to cancel
    BOOST_CHECK(!st.CancelCurve(BOB, Side::ASK));
    // Custody untouched by internal settlement; solvency holds.
    BOOST_CHECK_EQUAL(ledger.Custody(Quote()), 10);
    BOOST_CHECK_EQUAL(ledger.Custody(BaseX()), 10);
    BOOST_CHECK(ledger.SolvencyHolds());
}

//! Codex re-audit item 3 regression: aggregate demand may exceed
//! MAX_MONEY (it is computed EXACTLY, widened — never saturated in a
//! way that changes semantics), and the advertised fills are always a
//! fully rationed partition of the clearing volume. Two MAX_MONEY bids
//! at price 0 against a single 1-lot ask must clear volume 1 with the
//! bid fills summing to exactly 1 — never an un-rationed map — and
//! settlement enforces sum(fills) == volume before any mutation.
BOOST_AUTO_TEST_CASE(zero_price_two_bidder_allocation_is_exactly_rationed)
{
    flowmesh::FlowMeshState st{VAULT, BaseX(), Quote()};
    const flowmesh::Ledger& ledger{st.LedgerView()};
    // Zero-price bids reserve zero quote; the ask reserves 1 base.
    BOOST_REQUIRE(Fund(st, ACC_C, BaseX(), 1));
    BOOST_REQUIRE(st.SubmitCurve(ALICE, Side::BID, Pts({{0, MAX_MONEY}, {1, 0}})));
    BOOST_REQUIRE(st.SubmitCurve(BOB, Side::BID, Pts({{0, MAX_MONEY}, {1, 0}})));
    BOOST_REQUIRE(st.SubmitCurve(ACC_C, Side::ASK, Pts({{0, 1}})));

    const auto result{st.ClearSlot()};
    BOOST_REQUIRE(result.has_value()); // exact arithmetic: no fatal path
    BOOST_REQUIRE(result->cleared);
    BOOST_CHECK_EQUAL(result->price, 0);
    BOOST_CHECK_EQUAL(result->volume, 1);
    CAmount bid_sum{0};
    for (const auto& [account, fill] : result->bid_fill) bid_sum += fill;
    CAmount ask_sum{0};
    for (const auto& [account, fill] : result->ask_fill) ask_sum += fill;
    BOOST_CHECK_EQUAL(bid_sum, 1); // fully rationed: never sums past the volume
    BOOST_CHECK_EQUAL(ask_sum, 1);
    // Largest remainder on equal claims: the lowest-id account (ALICE
    // ...a1 < BOB ...b1) receives the single lot.
    BOOST_CHECK_EQUAL(result->bid_fill.at(ALICE), 1);
    BOOST_CHECK_EQUAL(result->bid_fill.at(BOB), 0);
    // The lot settled: base moved seller -> buyer, zero quote anywhere.
    BOOST_CHECK_EQUAL(ledger.Available(ALICE, BaseX()), 1);
    BOOST_CHECK_EQUAL(ledger.Available(ACC_C, Quote()), 0);
    BOOST_CHECK(ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_SUITE_END()
