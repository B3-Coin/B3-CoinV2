// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <crypto/hex_base.h>
#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/codes.h>
#include <flowmesh2/core/merkle.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/ledger/state_commitment.h>
#include <flowmesh2/test/util/ledger_fixtures.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
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

std::vector<MarketDescriptor> Markets() { return {MarketM1(), MarketM2()}; }

StateRootsV0 Commit(const LedgerState& ledger, const std::vector<MarketDescriptor>& markets = Markets(),
                    const std::vector<OrderLeafV0>& orders = {})
{
    const Result<StateRootsV0> roots{CommitV0(ledger, markets, orders)};
    RequireOk(roots, "CommitV0");
    return *roots;
}

/** ARCHITECTURE.md section 14.2 through slot 3, on the ledger alone: the
 * deposits of slot 1, the reservations of slot 2, the fill c (fee 50), the
 * rejected d (nonce 3 consumed, nothing else) and the 40/10 fee split.
 */
LedgerState WalkingSkeletonAfterSlot3()
{
    LedgerState ledger{EmptyLedger()};
    RequireOk(ledger.CreditDeposit(Deposit(ALICE, RUSD, 1'000'000'000, TestOutpoint("T1", 0))), "D1");
    RequireOk(ledger.CreditDeposit(Deposit(BOB, B3, 10'000'000'000, TestOutpoint("T2", 0))), "D2");
    RequireOk(ledger.CreditDeposit(Deposit(BOB, TOK, 500'000, TestOutpoint("T3", 0))), "D3");
    // Slot 2: a) Bob sells 5,000 M1 lots; b) Alice buys 100,000 M2 lots at 600.
    RequireOk(ledger.ConsumeNonce(BOB, 1), "a nonce");
    BOOST_REQUIRE(ledger.Reserve(BOB, B3, 5'000'000'000).value() == ReserveResult::OK);
    BOOST_REQUIRE(ledger.AddOpenOrder(BOB).value() == OpenOrderResult::OK);
    RequireOk(ledger.ConsumeNonce(ALICE, 1), "b nonce");
    BOOST_REQUIRE(ledger.Reserve(ALICE, RUSD, 600'000'000).value() == ReserveResult::OK);
    BOOST_REQUIRE(ledger.AddOpenOrder(ALICE).value() == OpenOrderResult::OK);
    // Slot 3: c) Alice IOC 2,000 lots at 260 fills at 250; d) rejected.
    RequireOk(ledger.ConsumeNonce(ALICE, 2), "c nonce");
    BOOST_REQUIRE(ledger.Reserve(ALICE, RUSD, 520'000).value() == ReserveResult::OK);
    RequireOk(ledger.PayFromReserved(ALICE, BOB, RUSD, 500'000, 50), "c quote leg");
    RequireOk(ledger.PayFromReserved(BOB, ALICE, B3, 2'000'000'000, 0), "c base leg");
    RequireOk(ledger.Release(ALICE, RUSD, 20'000), "c refund");
    RequireOk(ledger.ConsumeNonce(ALICE, 3), "d nonce");
    BOOST_REQUIRE(ledger.Reserve(ALICE, RUSD, 600'000'000).value() == ReserveResult::INSUFFICIENT);
    RequireOk(ledger.Credit(ledger.SeatPool(), RUSD, 40), "seat pool");
    RequireOk(ledger.Credit(ledger.Treasury(), RUSD, 10), "treasury");
    RequireOk(ledger.CheckConservation(), "conservation");
    return ledger;
}

//! A ledger with `n` funded accounts (two assets each) and some nonces.
LedgerState ManyAccounts(size_t n)
{
    LedgerState ledger{EmptyLedger()};
    for (size_t i{0}; i < n; ++i) {
        const AccountId a{TestAccount("many-" + std::to_string(i))};
        RequireOk(ledger.CreditDeposit(Deposit(a, RUSD, 1'000 + static_cast<Atoms>(i), TestOutpoint("many", static_cast<uint32_t>(2 * i)))), "rUSD");
        RequireOk(ledger.CreditDeposit(Deposit(a, B3, 7 * (static_cast<Atoms>(i) + 1), TestOutpoint("many", static_cast<uint32_t>(2 * i + 1)))), "B3");
        RequireOk(ledger.ConsumeNonce(a, i + 1), "nonce");
    }
    return ledger;
}

uint64_t Lcg(uint64_t& state)
{
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state >> 33;
}

template <class T>
void Shuffle(std::vector<T>& items, uint64_t seed)
{
    for (size_t i{items.size()}; i > 1; --i) std::swap(items[i - 1], items[Lcg(seed) % i]);
}

} // namespace

BOOST_AUTO_TEST_SUITE(commitment_tests)

BOOST_AUTO_TEST_CASE(empty_ledger_commits_to_empty_trees)
{
    const StateRootsV0 roots{Commit(EmptyLedger(), {})};
    const uint256 empty{MerkleRoot({})};
    BOOST_CHECK(roots.accounts == empty && roots.orders == empty && roots.markets == empty);
    BOOST_CHECK(roots.nullifiers == empty && roots.totals == empty);
    // Goldens from evidence/R5/golden.py.
    BOOST_CHECK_EQUAL(HexStr(empty), "c1ca51c946def6db4b8c28b3783d789630a7096fe5d199c1fa76a772b005832d");
    BOOST_CHECK_EQUAL(HexStr(roots.Root()), "04391e27522d42294e6329df22b881ff511f669577e6be19d1c6acb0ac65a23c");
}

BOOST_AUTO_TEST_CASE(walking_skeleton_slot3_golden_roots_and_leaf)
{
    const LedgerState ledger{WalkingSkeletonAfterSlot3()};
    // The final balances of section 14.3 that slot 3 already fixes.
    BOOST_CHECK_EQUAL(ledger.BalanceOf(BOB, RUSD).available, 499'950);
    BOOST_CHECK_EQUAL(ledger.BalanceOf(ledger.SeatPool(), RUSD).available, 40);
    BOOST_CHECK_EQUAL(ledger.BalanceOf(ledger.Treasury(), RUSD).available, 10);

    const StateRootsV0 roots{Commit(ledger)};
    // Goldens from evidence/R5/golden.py (independent of the C++ code).
    BOOST_CHECK_EQUAL(HexStr(roots.accounts), "9ba21d680c1429c1198e0849d306ecd427018262a7d7e6d64b89004697a68cac");
    BOOST_CHECK_EQUAL(HexStr(roots.orders), "c1ca51c946def6db4b8c28b3783d789630a7096fe5d199c1fa76a772b005832d");
    BOOST_CHECK_EQUAL(HexStr(roots.markets), "12dfcc4779b5165aa4dd34020050166ef1b8235ddac4371814a87eca44325a0c");
    BOOST_CHECK_EQUAL(HexStr(roots.nullifiers), "784310b2660c81cf14fe4b46bfa67b7bb5b34856d59aa1393fa76e5eeb2c0dff");
    BOOST_CHECK_EQUAL(HexStr(roots.totals), "b296451e56d97d8072fbb0050e0a101d9a795f31c6fdd1309b08c29047ade1ce");
    BOOST_CHECK_EQUAL(HexStr(roots.Root()), "aef5cd0c3f0d1462baa4dfdd7b33f829581c5b1d431bc83b672e7a3d5ebcaab5");

    // Leaf golden: Alice after slot 3.
    const Result<AccountWitnessV0> alice{ProveAccountV0(ledger, ALICE)};
    BOOST_REQUIRE(alice.has_value());
    BOOST_CHECK_EQUAL(HexStr(alice->leaf.Encode()),
                      "daf1aad624c3e66f021f1bae2409ef0c06d431d7930b2f5a0bf573619ac8be81020000000000000000000000000000"
                      "000000000000000000000000000000000000009435770000000000000000000000006439dd037ac1af8e258b417731"
                      "9230f77f127c3fcf5ec6436aba005bb25bd7ede0e2cf17000000000046c32300000000030100000000000000020000"
                      "0000000000030000000000000001000000");
    BOOST_CHECK(MerkleVerify(alice->leaf.LeafValue(), alice->proof, roots.accounts));
    // Accounts sort by id: bob, alice, treasury, seat pool (golden.py account_order).
    BOOST_CHECK_EQUAL(alice->proof.index, 1U);
    BOOST_CHECK_EQUAL(alice->proof.size, 4U);
}

BOOST_AUTO_TEST_CASE(root_is_deterministic_and_insertion_order_independent)
{
    // 12 accounts x 3 assets of deposits, nonces and reservations, applied in
    // 24 different orders, with markets and orders given in shuffled order.
    struct Op {
        int kind; // 0 deposit, 1 nonce, 2 reserve
        AccountId account;
        AssetId asset;
        Atoms amount;
        uint32_t index;
    };
    std::vector<Op> deposits;
    std::vector<Op> later;
    for (uint32_t i{0}; i < 12; ++i) {
        const AccountId a{TestAccount("order-" + std::to_string(i))};
        for (uint32_t k{0}; k < 3; ++k) {
            const AssetId asset{k == 0 ? B3 : k == 1 ? RUSD : TOK};
            deposits.push_back(Op{0, a, asset, 1'000 * (i + 1) + k, i * 3 + k});
        }
        later.push_back(Op{1, a, B3, 0, 1 + i % 5});
        later.push_back(Op{1, a, B3, 0, 7 + i % 3});
        later.push_back(Op{2, a, RUSD, static_cast<Atoms>(i), 0});
    }
    std::vector<OrderLeafV0> orders;
    for (int i{0}; i < 5; ++i) {
        orders.push_back(OrderLeafV0{OrderId{LabelHash("fm2/test/order/" + std::to_string(i))},
                                     LabelHash("fm2/test/order-leaf/" + std::to_string(i))});
    }

    std::set<uint256> state_roots;
    std::vector<StateRootsV0> all;
    for (uint64_t seed{1}; seed <= 24; ++seed) {
        std::vector<Op> d{deposits};
        std::vector<Op> l{later};
        std::vector<MarketDescriptor> markets{Markets()};
        std::vector<OrderLeafV0> o{orders};
        if (seed > 1) {
            Shuffle(d, seed);
            Shuffle(l, seed * 7919);
            Shuffle(markets, seed);
            Shuffle(o, seed * 104729);
        }
        LedgerState ledger{EmptyLedger()};
        for (const Op& op : d) {
            RequireOk(ledger.CreditDeposit(Deposit(op.account, op.asset, op.amount, TestOutpoint("order", op.index))), "deposit");
        }
        for (const Op& op : l) {
            if (op.kind == 1) RequireOk(ledger.ConsumeNonce(op.account, op.index), "nonce");
            if (op.kind == 2) BOOST_REQUIRE(ledger.Reserve(op.account, op.asset, op.amount).value() == ReserveResult::OK);
        }
        const StateRootsV0 roots{Commit(ledger, markets, o)};
        // Committing twice gives the same result.
        BOOST_CHECK(Commit(ledger, markets, o) == roots);
        state_roots.insert(roots.Root());
        all.push_back(roots);
    }
    BOOST_CHECK_EQUAL(state_roots.size(), 1U);
    BOOST_CHECK(std::all_of(all.begin(), all.end(), [&](const StateRootsV0& r) { return r == all.front(); }));
}

BOOST_AUTO_TEST_CASE(every_balance_nonce_and_component_change_changes_the_root)
{
    const LedgerState base{WalkingSkeletonAfterSlot3()};
    const StateRootsV0 base_roots{Commit(base)};

    // Each mutation, and which components it must change (a = accounts,
    // o = orders, m = markets, n = nullifiers, t = totals).
    struct Mutation {
        std::string name;
        std::string changes;
        std::function<void(LedgerState&)> apply;
        std::vector<MarketDescriptor> markets;
        std::vector<OrderLeafV0> orders;
    };
    const OrderLeafV0 one_order{OrderId{LabelHash("fm2/test/order/a")}, LabelHash("fm2/test/order-leaf/a")};
    const std::vector<Mutation> mutations{
        {"reserve 1", "a", [](LedgerState& l) { BOOST_REQUIRE(l.Reserve(BOB, RUSD, 1).value() == ReserveResult::OK); }, Markets(), {}},
        {"release 1", "a", [](LedgerState& l) { RequireOk(l.Release(ALICE, RUSD, 1), "release"); }, Markets(), {}},
        {"transfer 1", "a", [](LedgerState& l) { BOOST_REQUIRE(l.Transfer(BOB, ALICE, RUSD, 1).value() == OutcomeCode::TRANSFERRED); }, Markets(), {}},
        {"consume nonce", "a", [](LedgerState& l) { RequireOk(l.ConsumeNonce(BOB, 2), "nonce"); }, Markets(), {}},
        {"open order", "a", [](LedgerState& l) { BOOST_REQUIRE(l.AddOpenOrder(BOB).value() == OpenOrderResult::OK); }, Markets(), {}},
        {"withheld fee", "at", [](LedgerState& l) {
             BOOST_REQUIRE(l.Reserve(BOB, TOK, 10).value() == ReserveResult::OK);
             RequireOk(l.PayFromReserved(BOB, ALICE, TOK, 10, 1), "pay");
         }, Markets(), {}},
        {"deposit", "ant", [](LedgerState& l) { RequireOk(l.CreditDeposit(Deposit(BOB, TOK, 1, TestOutpoint("T4", 0))), "deposit"); }, Markets(), {}},
        {"new account", "ant", [](LedgerState& l) { RequireOk(l.CreditDeposit(Deposit(TestAccount("dave"), RUSD, 1, TestOutpoint("T5", 0))), "deposit"); }, Markets(), {}},
        {"one market fewer", "m", [](LedgerState&) {}, {MarketM1()}, {}},
        {"one order", "o", [](LedgerState&) {}, Markets(), {one_order}},
    };

    std::set<uint256> state_roots{base_roots.Root()};
    for (const Mutation& m : mutations) {
        LedgerState ledger{base};
        m.apply(ledger);
        RequireOk(ledger.CheckConservation(), m.name);
        const StateRootsV0 roots{Commit(ledger, m.markets, m.orders)};
        const auto changed = [&](char c) { return m.changes.find(c) != std::string::npos; };
        BOOST_CHECK_MESSAGE((roots.accounts != base_roots.accounts) == changed('a'), m.name + ": accounts");
        BOOST_CHECK_MESSAGE((roots.orders != base_roots.orders) == changed('o'), m.name + ": orders");
        BOOST_CHECK_MESSAGE((roots.markets != base_roots.markets) == changed('m'), m.name + ": markets");
        BOOST_CHECK_MESSAGE((roots.nullifiers != base_roots.nullifiers) == changed('n'), m.name + ": nullifiers");
        BOOST_CHECK_MESSAGE((roots.totals != base_roots.totals) == changed('t'), m.name + ": totals");
        BOOST_CHECK_MESSAGE(state_roots.insert(roots.Root()).second, m.name + ": state root not new");
    }
    BOOST_CHECK_EQUAL(state_roots.size(), mutations.size() + 1);
}

BOOST_AUTO_TEST_CASE(account_proofs_verify_with_merkle_verify)
{
    for (const size_t n : {size_t{1}, size_t{2}, size_t{3}, size_t{5}, size_t{8}, size_t{17}}) {
        const LedgerState ledger{ManyAccounts(n)};
        const StateRootsV0 roots{Commit(ledger)};
        std::set<uint64_t> indices;
        for (const auto& entry : ledger.Accounts()) {
            const Result<AccountWitnessV0> w{ProveAccountV0(ledger, entry.first)};
            BOOST_REQUIRE(w.has_value());
            BOOST_CHECK(w->leaf == ledger.Leaf(entry.first).value());
            BOOST_CHECK_EQUAL(w->proof.size, n);
            indices.insert(w->proof.index);
            BOOST_CHECK(MerkleVerify(w->leaf.LeafValue(), w->proof, roots.accounts));
            // A changed leaf, another component's root or a wrong size fails.
            AccountLeafV0 tampered{w->leaf};
            tampered.balances.front().available += 1;
            BOOST_CHECK(!MerkleVerify(tampered.LeafValue(), w->proof, roots.accounts));
            BOOST_CHECK(!MerkleVerify(w->leaf.LeafValue(), w->proof, roots.totals));
            MerkleProof resized{w->proof};
            resized.size += 1;
            BOOST_CHECK(!MerkleVerify(w->leaf.LeafValue(), resized, roots.accounts));
        }
        BOOST_CHECK_EQUAL(indices.size(), n);
    }
    const LedgerState ledger{ManyAccounts(3)};
    RequireError(ProveAccountV0(ledger, TestAccount("absent")), ErrorCode::INVALID_INPUT, "absent account");
}

BOOST_AUTO_TEST_CASE(commit_refuses_bad_markets_and_repeated_keys)
{
    const LedgerState ledger{WalkingSkeletonAfterSlot3()};
    const MarketDescriptor same_assets{RUSD, RUSD, 1, 1};
    RequireError(CommitV0(ledger, std::vector<MarketDescriptor>{same_assets}, {}), ErrorCode::INVALID_INPUT, "invalid market");
    RequireError(CommitV0(ledger, std::vector<MarketDescriptor>{MarketM1(), MarketM2(), MarketM1()}, {}), ErrorCode::INVALID_INPUT,
                 "repeated market");
    const OrderLeafV0 order{OrderId{LabelHash("fm2/test/order/x")}, LabelHash("fm2/test/order-leaf/x")};
    const OrderLeafV0 same_id{order.order, LabelHash("fm2/test/order-leaf/y")};
    RequireError(CommitV0(ledger, Markets(), std::vector<OrderLeafV0>{order, same_id}), ErrorCode::INVALID_INPUT, "repeated order");
}

BOOST_AUTO_TEST_CASE(stubs_return_not_implemented)
{
    const LedgerState ledger{WalkingSkeletonAfterSlot3()};
    RequireStub(CommitV1(ledger), "M2-LED-02");
    RequireStub(LiabilitySumRoot(ledger, RUSD), "M2-LED-03");
    RequireStub(ExitValue(ledger, ALICE, RUSD), "M2-LED-04");
}

// ---- Slice R7: the orders and markets components of the trading state ----

namespace {

RestingOrderLeafV0 OrderLeaf(std::string_view name, const MarketId& market, const AccountId& owner, Side side,
                             uint64_t price, Atoms reserved)
{
    RestingOrderLeafV0 leaf;
    leaf.order = OrderId{LabelHash("fm2/test/order/" + std::string{name})};
    leaf.market = market;
    leaf.owner = owner;
    leaf.side = side;
    leaf.price_ticks = price;
    leaf.remaining_lots = 7;
    leaf.arrival_rank = 1;
    leaf.reserved_left = reserved;
    return leaf;
}

} // namespace

BOOST_AUTO_TEST_CASE(orders_component_golden)
{
    // The three leaves of evidence/R7/golden.py, committed in any input order.
    const MarketId market{LabelHash("fm2/test/market")};
    const AccountId bob{LabelHash("fm2/test/account/bob")};
    std::vector<OrderLeafV0> leaves{OrderLeafOf(OrderLeaf("x", market, bob, Side::BUY, 90, 7 * 90)),
                                    OrderLeafOf(OrderLeaf("y", market, bob, Side::SELL, 110, 7)),
                                    OrderLeafOf(OrderLeaf("z", market, bob, Side::BUY, 95, 7 * 95))};
    for (uint64_t seed{1}; seed <= 6; ++seed) {
        Shuffle(leaves, seed);
        const StateRootsV0 roots{Commit(EmptyLedger(), {}, leaves)};
        BOOST_CHECK_EQUAL(HexStr(roots.orders), "8a81aa2fb0d25cdd4dc056633d0b847099217d68fd1e1981f5eaeca695d8a203");
    }
    const RestingOrderLeafV0 x{OrderLeaf("x", market, bob, Side::BUY, 90, 7 * 90)};
    BOOST_CHECK(OrderLeafOf(x).order == x.order);
    BOOST_CHECK(OrderLeafOf(x).value == x.LeafValue());
}

BOOST_AUTO_TEST_CASE(trading_state_commits_orders_and_markets)
{
    const LedgerState ledger{WalkingSkeletonAfterSlot3()};
    const MarketId m1{MarketM1().Id()};
    const MarketId m2{MarketM2().Id()};
    // Section 14.3 after slot 3: Bob's a rests with 3,000 lots; Alice's b rests in M2.
    std::vector<RestingOrderLeafV0> orders{OrderLeaf("a", m1, BOB, Side::SELL, 250, 3'000'000'000),
                                           OrderLeaf("b", m2, ALICE, Side::BUY, 600, 600'000'000)};
    const Result<StateRootsV0> roots{CommitTradingStateV0(ledger, Markets(), orders)};
    RequireOk(roots, "CommitTradingStateV0");

    // Equal to CommitV0 over OrderLeafOf: the markets component lists every
    // market, the orders component every order, both sorted by key.
    std::vector<OrderLeafV0> leaves;
    for (const auto& o : orders) leaves.push_back(OrderLeafOf(o));
    const StateRootsV0 expected{Commit(ledger, Markets(), leaves)};
    BOOST_CHECK(*roots == expected);
    std::vector<std::pair<OrderId, uint256>> keyed;
    for (const auto& o : orders) keyed.emplace_back(o.order, o.LeafValue());
    std::sort(keyed.begin(), keyed.end());
    std::vector<uint256> sorted_values;
    for (const auto& [id, value] : keyed) sorted_values.push_back(value);
    BOOST_CHECK(roots->orders == MerkleRoot(sorted_values));
    BOOST_CHECK(roots->orders != MerkleRoot({}));

    // Input order does not matter; a market with an empty book still counts.
    std::reverse(orders.begin(), orders.end());
    const std::vector<MarketDescriptor> reversed_markets{MarketM2(), MarketM1()};
    const Result<StateRootsV0> again{CommitTradingStateV0(ledger, reversed_markets, orders)};
    RequireOk(again, "CommitTradingStateV0 reversed");
    BOOST_CHECK(*again == *roots);
    const Result<StateRootsV0> one_market{CommitTradingStateV0(ledger, std::vector<MarketDescriptor>{MarketM1()},
                                                               std::vector<RestingOrderLeafV0>{orders.back()})};
    RequireOk(one_market, "one market");
    BOOST_CHECK(one_market->markets != roots->markets);

    // Any order field changes the orders root and the state root.
    RestingOrderLeafV0 changed{orders.front()};
    ++changed.arrival_rank;
    const Result<StateRootsV0> other{CommitTradingStateV0(ledger, Markets(), std::vector<RestingOrderLeafV0>{changed, orders.back()})};
    RequireOk(other, "changed order");
    BOOST_CHECK(other->orders != roots->orders);
    BOOST_CHECK(other->Root() != roots->Root());
    BOOST_CHECK(other->accounts == roots->accounts);
}

BOOST_AUTO_TEST_CASE(trading_state_refuses_inconsistent_orders)
{
    const LedgerState ledger{WalkingSkeletonAfterSlot3()};
    const MarketId m1{MarketM1().Id()};
    const RestingOrderLeafV0 good{OrderLeaf("a", m1, BOB, Side::SELL, 250, 3'000'000'000)};
    RequireOk(CommitTradingStateV0(ledger, Markets(), std::vector<RestingOrderLeafV0>{good}), "good");

    RestingOrderLeafV0 bad{good};
    bad.remaining_lots = 0;
    RequireError(CommitTradingStateV0(ledger, Markets(), std::vector<RestingOrderLeafV0>{bad}), ErrorCode::INVALID_INPUT,
                 "non-canonical order");
    bad = good;
    bad.market = MarketDescriptor{TOK, B3, 1, 1}.Id();
    RequireError(CommitTradingStateV0(ledger, Markets(), std::vector<RestingOrderLeafV0>{bad}), ErrorCode::INVALID_INPUT,
                 "unlisted market");
    bad = good;
    bad.owner = TestAccount("carol");
    RequireError(CommitTradingStateV0(ledger, Markets(), std::vector<RestingOrderLeafV0>{bad}), ErrorCode::INVALID_INPUT,
                 "owner without an account");
    RequireError(CommitTradingStateV0(ledger, Markets(), std::vector<RestingOrderLeafV0>{good, good}),
                 ErrorCode::INVALID_INPUT, "repeated order");
}

BOOST_AUTO_TEST_SUITE_END()
