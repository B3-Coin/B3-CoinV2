// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/codes.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/test/util/ledger_fixtures.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::ledger;
using namespace flowmesh2::test;

namespace {

const AssetId B3{AssetId::Native()};
const AssetId RUSD{TestAsset("rUSD")};
const AssetId TOK{TestAsset("TOK")};
const AccountId ALICE{TestAccount("alice")};
const AccountId BOB{TestAccount("bob")};
const AccountId CAROL{TestAccount("carol")};

//! Alice 1e9 rUSD (T1:0), Bob 1e10 B3 (T2:0) and 500,000 TOK (T3:0): slot 1 of section 14.2.
LedgerState Funded()
{
    LedgerState ledger{EmptyLedger()};
    RequireOk(ledger.CreditDeposit(Deposit(ALICE, RUSD, 1'000'000'000, TestOutpoint("T1", 0))), "D1");
    RequireOk(ledger.CreditDeposit(Deposit(BOB, B3, 10'000'000'000, TestOutpoint("T2", 0))), "D2");
    RequireOk(ledger.CreditDeposit(Deposit(BOB, TOK, 500'000, TestOutpoint("T3", 0))), "D3");
    RequireOk(ledger.CheckConservation(), "conservation");
    return ledger;
}

void CheckBalance(const LedgerState& ledger, const AccountId& account, const AssetId& asset, Atoms available, Atoms reserved)
{
    const Balance b{ledger.BalanceOf(account, asset)};
    BOOST_CHECK_EQUAL(b.available, available);
    BOOST_CHECK_EQUAL(b.reserved, reserved);
}

} // namespace

BOOST_AUTO_TEST_SUITE(ledger_tests)

BOOST_AUTO_TEST_CASE(create_validates_the_profile)
{
    const ProfileParams profile{RegtestProfile()};
    const LedgerState ledger{EmptyLedger(profile)};
    BOOST_CHECK(ledger.Accounts().empty());
    BOOST_CHECK(ledger.Nullifiers().empty());
    BOOST_CHECK(ledger.Totals().empty());
    BOOST_CHECK(ledger.SeatPool() == SeatPoolAccount(profile));
    BOOST_CHECK(ledger.Treasury() == TreasuryAccount(profile));
    BOOST_CHECK(ledger.IsProtocolAccount(ledger.SeatPool()));
    BOOST_CHECK(ledger.IsProtocolAccount(ledger.Treasury()));
    BOOST_CHECK(!ledger.IsProtocolAccount(ALICE));
    RequireOk(ledger.CheckConservation(), "empty conservation");

    ProfileParams mainnet{profile};
    mainnet.chain = ChainType::MAIN;
    RequireError(LedgerState::Create(mainnet), ErrorCode::UNSUPPORTED_NETWORK, "MAIN profile");
    ProfileParams broken{profile};
    broken.nonce_window = 0;
    RequireError(LedgerState::Create(broken), ErrorCode::INVALID_INPUT, "zero nonce window");
    // A profile that turns on an outline stub returns that stub's error.
    ProfileParams futures{profile};
    futures.futures_enabled = true;
    const Result<LedgerState> refused{LedgerState::Create(futures)};
    BOOST_REQUIRE(!refused.has_value());
    BOOST_CHECK(refused.error() == NotImplemented("M9-FUT-01"));
}

BOOST_AUTO_TEST_CASE(deposit_creates_the_account_and_credits_each_outpoint_once)
{
    LedgerState ledger{EmptyLedger()};
    BOOST_CHECK(!ledger.HasAccount(ALICE));
    RequireOk(ledger.CreditDeposit(Deposit(ALICE, RUSD, 1'000'000'000, TestOutpoint("T1", 0))), "first deposit");
    BOOST_REQUIRE(ledger.HasAccount(ALICE));
    CheckBalance(ledger, ALICE, RUSD, 1'000'000'000, 0);
    BOOST_CHECK(ledger.Find(ALICE)->nonces.Used().empty());
    BOOST_CHECK_EQUAL(ledger.Find(ALICE)->nonces.Size(), 100U);
    BOOST_CHECK_EQUAL(ledger.Find(ALICE)->open_orders, 0U);
    BOOST_CHECK(ledger.Credited(TestOutpoint("T1", 0)));
    BOOST_CHECK(!ledger.Credited(TestOutpoint("T1", 1)));
    BOOST_CHECK_EQUAL(ledger.TotalsOf(RUSD).deposits, 1'000'000'000);

    // Another outpoint of the same transaction is a separate deposit.
    RequireOk(ledger.CreditDeposit(Deposit(ALICE, RUSD, 5, TestOutpoint("T1", 1))), "second output");
    CheckBalance(ledger, ALICE, RUSD, 1'000'000'005, 0);

    // The same outpoint again is refused, whatever the fact claims, and nothing changes.
    const LedgerState before{ledger};
    RequireError(ledger.CreditDeposit(Deposit(ALICE, RUSD, 1'000'000'000, TestOutpoint("T1", 0))),
                 ErrorCode::INVALID_INPUT, "identical duplicate");
    RequireError(ledger.CreditDeposit(Deposit(BOB, B3, 7, TestOutpoint("T1", 0))), ErrorCode::INVALID_INPUT,
                 "contradictory duplicate");
    BOOST_CHECK(ledger == before);
    BOOST_CHECK(!ledger.HasAccount(BOB));
    BOOST_CHECK_EQUAL(ledger.Nullifiers().size(), 2U);
    RequireOk(ledger.CheckConservation(), "conservation");
}

BOOST_AUTO_TEST_CASE(deposit_refusals_and_overflow_leave_state_unchanged)
{
    LedgerState ledger{Funded()};
    const LedgerState before{ledger};
    for (const Atoms bad : {Atoms{0}, Atoms{-1}, MAX_ATOMS + 1}) {
        RequireError(ledger.CreditDeposit(Deposit(CAROL, RUSD, bad, TestOutpoint("bad", 0))), ErrorCode::INVALID_INPUT,
                     "bad amount " + std::to_string(bad));
    }
    RequireError(ledger.CreditDeposit(Deposit(ledger.SeatPool(), RUSD, 1, TestOutpoint("bad", 1))), ErrorCode::INVALID_INPUT,
                 "deposit to the seat pool");
    RequireError(ledger.CreditDeposit(Deposit(ledger.Treasury(), RUSD, 1, TestOutpoint("bad", 2))), ErrorCode::INVALID_INPUT,
                 "deposit to the treasury");
    BOOST_CHECK(ledger == before);

    // Deposits of one asset may total MAX_ATOMS and not one atom more.
    const AssetId big{TestAsset("BIG")};
    RequireOk(ledger.CreditDeposit(Deposit(ALICE, big, MAX_ATOMS - 1, TestOutpoint("big", 0))), "near max");
    RequireOk(ledger.CreditDeposit(Deposit(BOB, big, 1, TestOutpoint("big", 1))), "exactly max");
    const LedgerState at_max{ledger};
    RequireError(ledger.CreditDeposit(Deposit(CAROL, big, 1, TestOutpoint("big", 2))), ErrorCode::ARITHMETIC_OVERFLOW,
                 "one atom above MAX_ATOMS");
    BOOST_CHECK(ledger == at_max);
    BOOST_CHECK(!ledger.HasAccount(CAROL));
    BOOST_CHECK(!ledger.Credited(TestOutpoint("big", 2)));
    // Another asset is unaffected.
    RequireOk(ledger.CreditDeposit(Deposit(CAROL, RUSD, 1, TestOutpoint("big", 2))), "other asset");
    RequireOk(ledger.CheckConservation(), "conservation");
}

BOOST_AUTO_TEST_CASE(reserve_and_release_and_over_reserve_changes_nothing)
{
    LedgerState ledger{Funded()};
    // Section 14.2 slot 2 b: Alice reserves 600,000,000 rUSD for M2.
    BOOST_CHECK(ledger.Reserve(ALICE, RUSD, 600'000'000).value() == ReserveResult::OK);
    CheckBalance(ledger, ALICE, RUSD, 400'000'000, 600'000'000);

    // One shared balance across markets (TRD-10): another 6e8 for a second
    // market is refused because only 4e8 is available, and nothing changes.
    const LedgerState before{ledger};
    BOOST_CHECK(ledger.Reserve(ALICE, RUSD, 600'000'000).value() == ReserveResult::INSUFFICIENT);
    BOOST_CHECK(ledger.Reserve(ALICE, RUSD, 400'000'001).value() == ReserveResult::INSUFFICIENT);
    BOOST_CHECK(ledger.Reserve(ALICE, TOK, 1).value() == ReserveResult::INSUFFICIENT);
    BOOST_CHECK(ledger == before);

    // Exactly the available amount, and zero, are fine.
    BOOST_CHECK(ledger.Reserve(ALICE, RUSD, 400'000'000).value() == ReserveResult::OK);
    CheckBalance(ledger, ALICE, RUSD, 0, 1'000'000'000);
    const LedgerState all_reserved{ledger};
    BOOST_CHECK(ledger.Reserve(ALICE, RUSD, 0).value() == ReserveResult::OK);
    BOOST_CHECK(ledger == all_reserved);

    // Release, and releasing more than is reserved is refused without change.
    RequireOk(ledger.Release(ALICE, RUSD, 600'000'000), "release");
    CheckBalance(ledger, ALICE, RUSD, 600'000'000, 400'000'000);
    const LedgerState released{ledger};
    RequireError(ledger.Release(ALICE, RUSD, 400'000'001), ErrorCode::INVALID_INPUT, "over-release");
    RequireError(ledger.Release(ALICE, TOK, 1), ErrorCode::INVALID_INPUT, "release of an unheld asset");
    RequireError(ledger.Reserve(CAROL, RUSD, 1), ErrorCode::INVALID_INPUT, "unknown account");
    RequireError(ledger.Release(CAROL, RUSD, 0), ErrorCode::INVALID_INPUT, "unknown account release");
    RequireError(ledger.Reserve(ALICE, RUSD, -1), ErrorCode::INVALID_INPUT, "negative reserve");
    RequireError(ledger.Release(ALICE, RUSD, MAX_ATOMS + 1), ErrorCode::INVALID_INPUT, "release above MAX_ATOMS");
    BOOST_CHECK(ledger == released);
    RequireOk(ledger.CheckConservation(), "conservation");
}

BOOST_AUTO_TEST_CASE(pay_from_reserved_settles_and_withholds_the_seller_fee)
{
    LedgerState ledger{Funded()};
    // Section 14.2 slot 3 c: Alice buys 2,000 M1 lots at 250 from Bob's ask.
    BOOST_REQUIRE(ledger.Reserve(BOB, B3, 5'000'000'000).value() == ReserveResult::OK);
    BOOST_REQUIRE(ledger.Reserve(ALICE, RUSD, 520'000).value() == ReserveResult::OK);
    RequireOk(ledger.PayFromReserved(ALICE, BOB, RUSD, 500'000, 50), "quote leg");
    RequireOk(ledger.PayFromReserved(BOB, ALICE, B3, 2'000'000'000, 0), "base leg");
    RequireOk(ledger.Release(ALICE, RUSD, 20'000), "refund");
    CheckBalance(ledger, ALICE, RUSD, 999'500'000, 0);
    CheckBalance(ledger, ALICE, B3, 2'000'000'000, 0);
    CheckBalance(ledger, BOB, RUSD, 499'950, 0);
    CheckBalance(ledger, BOB, B3, 5'000'000'000, 3'000'000'000);
    BOOST_CHECK_EQUAL(ledger.TotalsOf(RUSD).fees_held, 50);
    RequireOk(ledger.CheckConservation(), "conservation with fees held");

    // Refusals change nothing.
    const LedgerState before{ledger};
    RequireError(ledger.PayFromReserved(BOB, ALICE, B3, 3'000'000'001, 0), ErrorCode::INVALID_INPUT, "above reservation");
    RequireError(ledger.PayFromReserved(BOB, ALICE, B3, 10, 11), ErrorCode::INVALID_INPUT, "fee above payment");
    RequireError(ledger.PayFromReserved(BOB, CAROL, B3, 10, 0), ErrorCode::INVALID_INPUT, "unknown payee");
    RequireError(ledger.PayFromReserved(CAROL, BOB, B3, 0, 0), ErrorCode::INVALID_INPUT, "unknown payer");
    RequireError(ledger.PayFromReserved(BOB, ALICE, B3, -1, 0), ErrorCode::INVALID_INPUT, "negative payment");
    BOOST_CHECK(ledger == before);

    // Self-trade (O-17): reserved goes to available minus the fee.
    RequireOk(ledger.PayFromReserved(BOB, BOB, B3, 1'000, 3), "self-trade");
    CheckBalance(ledger, BOB, B3, 5'000'000'997, 2'999'999'000);
    BOOST_CHECK_EQUAL(ledger.TotalsOf(B3).fees_held, 3);
    RequireOk(ledger.CheckConservation(), "conservation after self-trade");

    // Edge: a fee equal to the payment is valid; the payee receives nothing.
    BOOST_REQUIRE(ledger.Reserve(ALICE, RUSD, 10).value() == ReserveResult::OK);
    RequireOk(ledger.PayFromReserved(ALICE, BOB, RUSD, 10, 10), "fee equal to the payment");
    CheckBalance(ledger, BOB, RUSD, 499'950, 0);
    BOOST_CHECK_EQUAL(ledger.TotalsOf(RUSD).fees_held, 60);
    RequireOk(ledger.CheckConservation(), "conservation after a 100% fee");
}

BOOST_AUTO_TEST_CASE(credit_only_to_protocol_accounts_and_only_from_fees_held)
{
    LedgerState ledger{Funded()};
    BOOST_REQUIRE(ledger.Reserve(ALICE, RUSD, 500'000).value() == ReserveResult::OK);
    RequireOk(ledger.PayFromReserved(ALICE, BOB, RUSD, 500'000, 50), "fill with fee 50");

    const LedgerState before{ledger};
    RequireError(ledger.Credit(ALICE, RUSD, 1), ErrorCode::INVALID_INPUT, "credit to a user account");
    RequireError(ledger.Credit(CAROL, RUSD, 1), ErrorCode::INVALID_INPUT, "credit to an unknown account");
    RequireError(ledger.Credit(ledger.SeatPool(), RUSD, 51), ErrorCode::INVALID_INPUT, "credit above fees held");
    RequireError(ledger.Credit(ledger.SeatPool(), B3, 1), ErrorCode::INVALID_INPUT, "credit of an asset with no fees");
    RequireError(ledger.Credit(ledger.Treasury(), RUSD, -1), ErrorCode::INVALID_INPUT, "negative credit");
    // A zero credit changes nothing and does not create the account.
    RequireOk(ledger.Credit(ledger.SeatPool(), RUSD, 0), "zero credit");
    BOOST_CHECK(ledger == before);
    BOOST_CHECK(!ledger.HasAccount(ledger.SeatPool()));

    // Section 14.2 slot 3: seat pool +40, treasury +10; the fees held drop to zero.
    RequireOk(ledger.Credit(ledger.SeatPool(), RUSD, 40), "seat pool");
    RequireOk(ledger.Credit(ledger.Treasury(), RUSD, 10), "treasury");
    CheckBalance(ledger, ledger.SeatPool(), RUSD, 40, 0);
    CheckBalance(ledger, ledger.Treasury(), RUSD, 10, 0);
    BOOST_CHECK_EQUAL(ledger.TotalsOf(RUSD).fees_held, 0);
    BOOST_CHECK(ledger.Find(ledger.SeatPool())->nonces.Used().empty());
    RequireOk(ledger.CheckConservation(), "conservation");
}

BOOST_AUTO_TEST_CASE(transfer_rules_o7)
{
    LedgerState ledger{Funded()};
    // Section 14.2 slot 4 e is Bob to Alice; here the same rules on fresh funds.
    BOOST_CHECK(ledger.Transfer(ALICE, BOB, RUSD, 100'000).value() == OutcomeCode::TRANSFERRED);
    CheckBalance(ledger, ALICE, RUSD, 999'900'000, 0);
    CheckBalance(ledger, BOB, RUSD, 100'000, 0);

    const LedgerState before{ledger};
    BOOST_CHECK(ledger.Transfer(ALICE, CAROL, RUSD, 1).value() == OutcomeCode::REJ_NO_RECIPIENT);
    BOOST_CHECK(ledger.Transfer(ALICE, ALICE, RUSD, 1).value() == OutcomeCode::REJ_BAD_PARAMS);
    BOOST_CHECK(ledger.Transfer(ALICE, BOB, RUSD, 0).value() == OutcomeCode::REJ_BAD_PARAMS);
    // Bad parameters are decided before the recipient and the balance.
    BOOST_CHECK(ledger.Transfer(ALICE, CAROL, RUSD, 0).value() == OutcomeCode::REJ_BAD_PARAMS);
    BOOST_CHECK(ledger.Transfer(ALICE, BOB, RUSD, 999'900'001).value() == OutcomeCode::REJ_INSUFFICIENT_AVAILABLE);
    BOOST_CHECK(ledger.Transfer(ALICE, BOB, TOK, 1).value() == OutcomeCode::REJ_INSUFFICIENT_AVAILABLE);
    RequireError(ledger.Transfer(CAROL, ALICE, RUSD, 1), ErrorCode::INVALID_INPUT, "unknown sender");
    RequireError(ledger.Transfer(ALICE, BOB, RUSD, -5), ErrorCode::INVALID_INPUT, "negative amount");
    RequireError(ledger.Transfer(ALICE, BOB, RUSD, MAX_ATOMS + 1), ErrorCode::INVALID_INPUT, "amount above MAX_ATOMS");
    BOOST_CHECK(ledger == before);

    // Only available funds move; reserved funds are not transferable.
    BOOST_REQUIRE(ledger.Reserve(ALICE, RUSD, 999'000'000).value() == ReserveResult::OK);
    BOOST_CHECK(ledger.Transfer(ALICE, BOB, RUSD, 900'001).value() == OutcomeCode::REJ_INSUFFICIENT_AVAILABLE);
    BOOST_CHECK(ledger.Transfer(ALICE, BOB, RUSD, 900'000).value() == OutcomeCode::TRANSFERRED);
    CheckBalance(ledger, ALICE, RUSD, 0, 999'000'000);
    RequireOk(ledger.CheckConservation(), "conservation");
}

BOOST_AUTO_TEST_CASE(nonces_and_open_orders)
{
    LedgerState ledger{Funded()};
    RequireOk(ledger.ConsumeNonce(ALICE, 1), "nonce 1");
    RequireOk(ledger.ConsumeNonce(ALICE, 3), "nonce 3");
    const LedgerState before{ledger};
    RequireError(ledger.ConsumeNonce(ALICE, 3), ErrorCode::INVALID_INPUT, "used nonce");
    RequireError(ledger.ConsumeNonce(CAROL, 1), ErrorCode::INVALID_INPUT, "unknown account");
    BOOST_CHECK(ledger == before);
    BOOST_CHECK((std::vector<uint64_t>(ledger.Find(ALICE)->nonces.Used().begin(), ledger.Find(ALICE)->nonces.Used().end()) ==
                 std::vector<uint64_t>{1, 3}));
    // Nonces are per account.
    RequireOk(ledger.ConsumeNonce(BOB, 3), "Bob's nonce 3");

    const uint32_t cap{ledger.Profile().max_open_orders_per_account};
    BOOST_CHECK_EQUAL(cap, 1'000U);
    for (uint32_t i{0}; i < cap; ++i) BOOST_REQUIRE(ledger.AddOpenOrder(ALICE).value() == OpenOrderResult::OK);
    const LedgerState full{ledger};
    BOOST_CHECK(ledger.AddOpenOrder(ALICE).value() == OpenOrderResult::AT_CAP);
    BOOST_CHECK(ledger == full);
    RequireOk(ledger.RemoveOpenOrder(ALICE), "remove one");
    BOOST_CHECK_EQUAL(ledger.Find(ALICE)->open_orders, cap - 1);
    RequireError(ledger.RemoveOpenOrder(BOB), ErrorCode::INVALID_INPUT, "remove at zero");
    RequireError(ledger.RemoveOpenOrder(CAROL), ErrorCode::INVALID_INPUT, "remove for an unknown account");
    RequireError(ledger.AddOpenOrder(CAROL), ErrorCode::INVALID_INPUT, "add for an unknown account");
    RequireOk(ledger.CheckConservation(), "conservation");
}

BOOST_AUTO_TEST_CASE(leaf_is_canonical_and_omits_empty_balances)
{
    LedgerState ledger{Funded()};
    RequireOk(ledger.ConsumeNonce(BOB, 1), "nonce");
    BOOST_REQUIRE(ledger.AddOpenOrder(BOB).value() == OpenOrderResult::OK);
    BOOST_REQUIRE(ledger.Reserve(BOB, TOK, 500'000).value() == ReserveResult::OK);

    const Result<AccountLeafV0> leaf{ledger.Leaf(BOB)};
    BOOST_REQUIRE(leaf.has_value());
    BOOST_CHECK(leaf->account == BOB);
    BOOST_REQUIRE_EQUAL(leaf->balances.size(), 2U);
    BOOST_CHECK(leaf->Available(B3) == 10'000'000'000);
    BOOST_CHECK(leaf->Reserved(TOK) == 500'000);
    BOOST_CHECK(leaf->Available(TOK) == 0);
    BOOST_CHECK(leaf->used_nonces == std::vector<uint64_t>{1});
    BOOST_CHECK_EQUAL(leaf->open_orders, 1U);
    BOOST_CHECK(leaf->IsCanonical(ledger.Profile()));
    const auto decoded{AccountLeafV0::Decode(leaf->Encode(), ledger.Profile())};
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK(*decoded == *leaf);

    // Moving all TOK away removes the entry from the state and the leaf.
    RequireOk(ledger.PayFromReserved(BOB, ALICE, TOK, 500'000, 0), "all TOK to Alice");
    BOOST_CHECK(!ledger.Find(BOB)->balances.contains(TOK));
    BOOST_CHECK_EQUAL(ledger.Leaf(BOB).value().balances.size(), 1U);
    RequireError(ledger.Leaf(CAROL), ErrorCode::INVALID_INPUT, "leaf of an unknown account");
    RequireOk(ledger.CheckConservation(), "conservation");
}

BOOST_AUTO_TEST_CASE(asset_cap_per_account_is_oversize)
{
    LedgerState ledger{EmptyLedger()};
    for (size_t i{0}; i < MAX_LEAF_ASSETS; ++i) {
        const AssetId asset{TestAsset("cap-" + std::to_string(i))};
        RequireOk(ledger.CreditDeposit(Deposit(ALICE, asset, 1, TestOutpoint("cap", static_cast<uint32_t>(i)))), "fill");
    }
    RequireOk(ledger.CreditDeposit(Deposit(BOB, TOK, 1, TestOutpoint("cap-bob", 0))), "Bob TOK");
    const LedgerState full{ledger};
    RequireError(ledger.CreditDeposit(Deposit(ALICE, RUSD, 1, TestOutpoint("cap-over", 0))), ErrorCode::OVERSIZE,
                 "deposit of a 257th asset");
    RequireError(ledger.Transfer(BOB, ALICE, TOK, 1), ErrorCode::OVERSIZE, "transfer of a 257th asset");
    BOOST_CHECK(ledger == full);
    // A held asset still works.
    RequireOk(ledger.CreditDeposit(Deposit(ALICE, TestAsset("cap-0"), 1, TestOutpoint("cap-over", 1))), "held asset");
    BOOST_CHECK_EQUAL(ledger.Leaf(ALICE).value().balances.size(), MAX_LEAF_ASSETS);
    RequireOk(ledger.CheckConservation(), "conservation");
}

BOOST_AUTO_TEST_CASE(conservation_check_detects_broken_states)
{
    const LedgerState ledger{Funded()};
    const uint32_t cap{ledger.Profile().max_open_orders_per_account};
    RequireOk(CheckConservation(ledger.Accounts(), ledger.Totals(), cap), "intact");

    {   // One atom more in an account.
        AccountMap accounts{ledger.Accounts()};
        accounts.at(ALICE).balances.at(RUSD).available += 1;
        RequireError(CheckConservation(accounts, ledger.Totals(), cap), ErrorCode::INVARIANT, "inflated balance");
    }
    {   // One atom fewer deposited.
        TotalsMap totals{ledger.Totals()};
        totals.at(RUSD).deposits -= 1;
        RequireError(CheckConservation(ledger.Accounts(), totals, cap), ErrorCode::INVARIANT, "deflated deposits");
    }
    {   // Fees held count as owed.
        TotalsMap totals{ledger.Totals()};
        totals.at(RUSD).fees_held = 1;
        RequireError(CheckConservation(ledger.Accounts(), totals, cap), ErrorCode::INVARIANT, "phantom fees");
    }
    {   // Effects above deposits.
        TotalsMap totals{ledger.Totals()};
        totals.at(TOK).effects = totals.at(TOK).deposits + 1;
        RequireError(CheckConservation(ledger.Accounts(), totals, cap), ErrorCode::INVARIANT, "effects above deposits");
    }
    {   // A balance of an asset without totals.
        TotalsMap totals{ledger.Totals()};
        totals.erase(TOK);
        RequireError(CheckConservation(ledger.Accounts(), totals, cap), ErrorCode::INVARIANT, "missing totals");
    }
    {   // An all-zero entry is not canonical.
        AccountMap accounts{ledger.Accounts()};
        accounts.at(ALICE).balances[TOK] = Balance{};
        RequireError(CheckConservation(accounts, ledger.Totals(), cap), ErrorCode::INVARIANT, "zero entry");
    }
    {   // Too many open orders.
        AccountMap accounts{ledger.Accounts()};
        accounts.at(ALICE).open_orders = cap + 1;
        RequireError(CheckConservation(accounts, ledger.Totals(), cap), ErrorCode::INVARIANT, "open orders");
    }
}

BOOST_AUTO_TEST_CASE(stubs_refuse_without_changing_state)
{
    LedgerState ledger{Funded()};
    const LedgerState before{ledger};
    const WithdrawPayload withdrawal{RUSD, 1, {0x51}};
    RequireStub(ledger.RequestWithdrawal(ALICE, withdrawal), "M2-LED-05");
    RequireStub(ledger.TransferToFutures(ALICE, RUSD, 1), "M2-LED-06");
    RequireStub(ledger.TransferFromFutures(ALICE, RUSD, 1), "M2-LED-06");
    BOOST_CHECK(ledger == before);
}

BOOST_AUTO_TEST_SUITE_END()
