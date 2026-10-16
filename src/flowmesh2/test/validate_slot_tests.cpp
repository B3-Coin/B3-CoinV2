// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/engine/book.h>
#include <flowmesh2/engine/engine.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/ledger/state_commitment.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/slot_fixtures.h>

#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::engine;
using namespace flowmesh2::test;

// Only ValidateSlotInputs makes a ValidatedSlot; a HeadView never binds a temporary.
static_assert(!std::is_default_constructible_v<ValidatedSlot>);
static_assert(!std::is_constructible_v<ValidatedSlot, SlotInputs>);
static_assert(!std::is_aggregate_v<ValidatedSlot>);
static_assert(std::is_copy_constructible_v<ValidatedSlot>);
static_assert(!std::is_constructible_v<HeadView, SlotHeaderV2&&, const EngineState&>);
static_assert(!std::is_constructible_v<HeadView, const SlotHeaderV2&, EngineState&&>);
static_assert(std::is_constructible_v<HeadView, const SlotHeaderV2&, const EngineState&>);

namespace {

const AssetId B3{AssetId::Native()};
const AssetId RUSD{TestAsset("rUSD")};

//! Credit `amount` straight into the parent ledger (validation reads only the state).
void FundParent(SlotFixture& fx, std::string_view name, const AssetId& asset, Atoms amount, std::string_view outpoint)
{
    RequireOk(fx.state.ledger.CreditDeposit(Deposit(KeyAccount(name), asset, amount, TestOutpoint(outpoint, 0))), "FundParent");
}

//! A fixture whose parent state has funded accounts alice and bob.
SlotFixture Funded(const ProfileParams& profile = RegtestProfile())
{
    SlotFixture fx{profile};
    FundParent(fx, "alice", RUSD, 1'000'000'000, "fund-alice");
    FundParent(fx, "bob", B3, 10'000'000'000, "fund-bob");
    return fx;
}

std::vector<unsigned char> AlicePlace(SlotFixture& fx, uint64_t nonce)
{
    return fx.Place("alice", nonce, MarketM1(), Side::BUY, TimeInForce::GTC, 250, 10);
}

} // namespace

BOOST_AUTO_TEST_SUITE(validate_slot_tests)

BOOST_AUTO_TEST_CASE(accepts_a_valid_slot_and_exposes_its_parts)
{
    SlotFixture fx{Funded()};
    std::vector<std::vector<unsigned char>> facts{fx.DepositBytes(KeyAccount("carol"), RUSD, 5, "d1"),
                                                  fx.DepositBytes(KeyAccount("alice"), B3, 7, "d2")};
    std::vector<std::vector<unsigned char>> originals{
        AlicePlace(fx, 1), fx.Transfer("bob", 1, KeyAccount("alice"), B3, 3),
        fx.Place("bob", 2, MarketM1(), Side::SELL, TimeInForce::POST_ONLY, 300, 1),
        fx.Cancel("alice", 2, OrderId{LabelHash("fm2/test/order/none")})};
    const SlotInputs inputs{fx.Inputs(facts, originals)};
    const Result<ValidatedSlot> slot{fx.Validate(inputs)};
    RequireOk(slot, "ValidateSlotInputs");
    BOOST_CHECK(slot->ParentHash() == fx.header.Hash());
    BOOST_CHECK_EQUAL(slot->Height(), 1U);
    BOOST_CHECK(slot->Inputs() == inputs);
    BOOST_REQUIRE_EQUAL(slot->Deposits().size(), 2U);
    BOOST_CHECK(slot->Deposits()[0].account == KeyAccount("carol"));
    BOOST_CHECK(slot->FactIds()[1] == FactLeaf(facts[1]));
    BOOST_CHECK(slot->FactIds()[0] == FactId(Fact{slot->Deposits()[0]}).value());
    BOOST_REQUIRE_EQUAL(slot->Actions().size(), 4U);
    BOOST_CHECK(slot->Actions()[0].phase == SlotPhase::TAKERS);
    BOOST_CHECK(slot->Actions()[1].phase == SlotPhase::ACCOUNT);
    BOOST_CHECK(slot->Actions()[2].phase == SlotPhase::POST_ONLY);
    BOOST_CHECK(slot->Actions()[3].phase == SlotPhase::CANCELS);
    BOOST_CHECK(slot->Actions()[1].action.Account() == KeyAccount("bob"));
    BOOST_CHECK(std::get<TransferPayload>(slot->Actions()[1].payload).amount == 3);
    // Proposal order is kept; the canonical order is CanonicalOrder over the parent header.
    for (size_t i{0}; i < originals.size(); ++i) BOOST_CHECK(slot->Actions()[i].action.Action().Encode() == originals[i]);
    std::vector<VerifiedAction> verified;
    for (const SlotAction& a : slot->Actions()) verified.push_back(a.action);
    const Result<std::vector<size_t>> order{CanonicalOrder(std::span<const VerifiedAction>{verified}, fx.header.Hash(), 1)};
    RequireOk(order, "CanonicalOrder");
    BOOST_CHECK(std::vector<size_t>(slot->Order().begin(), slot->Order().end()) == *order);

    // An empty slot (a heartbeat) is valid too.
    RequireOk(fx.Validate(fx.Inputs({}, {})), "empty slot");
}

BOOST_AUTO_TEST_CASE(profile_mismatch)
{
    SlotFixture fx{Funded()};
    SlotInputs inputs{fx.Inputs({}, {AlicePlace(fx, 1)})};
    inputs.profile_hash = LabelHash("fm2/test/other-profile");
    RequireRefusal(fx.Validate(inputs), SLOT_PROFILE_MISMATCH, "inputs profile hash");

    ProfileParams other{fx.profile};
    other.fee_ppm = 101;
    RequireRefusal(ValidateSlotInputs(fx.Head(), fx.Inputs({}, {}), other), SLOT_PROFILE_MISMATCH, "another profile");

    SlotFixture header_fx{Funded()};
    header_fx.header.profile_hash = LabelHash("fm2/test/other-profile");
    RequireRefusal(header_fx.Validate(header_fx.Inputs({}, {})), SLOT_PROFILE_MISMATCH, "parent header profile hash");

    // A profile that Validate refuses returns Validate's own error first.
    ProfileParams stub{fx.profile};
    stub.futures_enabled = true;
    RequireStub(ValidateSlotInputs(fx.Head(), fx.Inputs({}, {}), stub), "M9-FUT-01");
}

BOOST_AUTO_TEST_CASE(domain_mismatch_and_height_exhausted)
{
    SlotFixture fx{Funded()};
    SlotInputs inputs{fx.Inputs({}, {})};
    inputs.domain = LabelHash("fm2/test/other-domain");
    RequireRefusal(fx.Validate(inputs), SLOT_DOMAIN_MISMATCH, "slot domain");

    fx.header.height = std::numeric_limits<uint64_t>::max();
    RequireRefusal(fx.Validate(fx.Inputs({}, {})), SLOT_HEIGHT_EXHAUSTED, "no successor height");
}

BOOST_AUTO_TEST_CASE(anchor_below_or_forked_from_the_parent)
{
    SlotFixture fx{Funded()};
    // The parent (genesis) is anchored at 129.
    SlotInputs inputs{fx.Inputs({}, {})};
    inputs.anchor = TestAnchor(128);
    inputs.facts_to = inputs.facts_from;
    RequireRefusal(fx.Validate(inputs), SLOT_ANCHOR_BELOW_PARENT, "anchor 128 below 129");
    inputs.anchor = Anchor{129, LabelHash("fm2/test/anchor/129-other")};
    RequireRefusal(fx.Validate(inputs), SLOT_ANCHOR_FORK, "another block at 129");
    // The same anchor as the parent, or a higher one, is fine.
    inputs.anchor = TestAnchor(129);
    RequireOk(fx.Validate(inputs), "same anchor");
    inputs.anchor = TestAnchor(500);
    RequireOk(fx.Validate(inputs), "higher anchor");
}

BOOST_AUTO_TEST_CASE(fact_range_must_continue_the_parent_and_stay_below_the_anchor)
{
    SlotFixture fx{Funded()};
    SlotInputs inputs{fx.Inputs({}, {})};
    inputs.facts_from = FactCursor{130, 1};
    inputs.facts_to = FactCursor{130, 1};
    RequireRefusal(fx.Validate(inputs), SLOT_FACTS_NOT_CONTIGUOUS, "a gap after the parent's facts_to");
    inputs.facts_from = FactCursor{129, 9};
    RequireRefusal(fx.Validate(inputs), SLOT_FACTS_NOT_CONTIGUOUS, "overlapping the parent's facts");

    inputs = fx.Inputs({}, {});
    inputs.facts_to = FactCursor{129, 5};
    RequireRefusal(fx.Validate(inputs), SLOT_FACTS_REVERSED, "facts_to before facts_from");

    // Anchor 130: facts_to may reach (131, 0) and no further.
    inputs = fx.Inputs({}, {});
    inputs.facts_to = FactCursor{131, 0};
    RequireOk(fx.Validate(inputs), "facts_to (131, 0)");
    inputs.facts_to = FactCursor{131, 1};
    RequireRefusal(fx.Validate(inputs), SLOT_FACTS_BEYOND_ANCHOR, "facts_to (131, 1)");
    inputs.facts_to = FactCursor{132, 0};
    RequireRefusal(fx.Validate(inputs), SLOT_FACTS_BEYOND_ANCHOR, "facts_to (132, 0)");
}

BOOST_AUTO_TEST_CASE(fact_and_action_caps)
{
    SlotFixture fx{Funded()};
    SlotInputs inputs{fx.Inputs({}, {})};
    inputs.body.facts.assign(MAX_SLOT_FACTS + 1, std::vector<unsigned char>{0x01});
    RequireRefusal(fx.Validate(inputs), SLOT_FACT_COUNT_CAP, "1,025 facts");
    inputs.body.facts.assign(1, std::vector<unsigned char>(MAX_FACT_BYTES + 1, 0x01));
    RequireRefusal(fx.Validate(inputs), SLOT_FACT_BYTES_CAP, "a 1,025-byte fact");
    inputs.body.facts.clear();
    inputs.body.originals.assign(1, std::vector<unsigned char>(ActionV2::MaxEncodedSize(fx.profile) + 1, 0x01));
    RequireRefusal(fx.Validate(inputs), SLOT_ACTION_BYTES_CAP, "an original above the byte cap");

    // max_actions_per_slot = 4: four originals pass, five are refused before decoding.
    ProfileParams small{RegtestProfile()};
    small.max_actions_per_slot = 4;
    SlotFixture capped{Funded(small)};
    std::vector<std::vector<unsigned char>> originals;
    for (uint64_t n{1}; n <= 4; ++n) originals.push_back(AlicePlace(capped, n));
    RequireOk(capped.Validate(capped.Inputs({}, originals)), "four originals");
    originals.push_back({0x00});
    RequireRefusal(capped.Validate(capped.Inputs({}, originals)), SLOT_ACTION_COUNT_CAP, "five originals");
}

BOOST_AUTO_TEST_CASE(facts_must_decode_and_stub_kinds_propagate_their_ids)
{
    SlotFixture fx{Funded()};
    for (const std::vector<unsigned char>& bad : {std::vector<unsigned char>{0x01, 0x00}, std::vector<unsigned char>{0x09},
                                                  std::vector<unsigned char>{}}) {
        RequireRefusal(fx.Validate(fx.Inputs({bad}, {})), SLOT_FACT_MALFORMED, "malformed fact");
    }
    const std::vector<std::pair<uint8_t, std::string>> stubs{
        {2, "M7-SEAT-01"}, {3, "M7-GOV-01"}, {4, "MB1-B3D-02"}, {5, "M8-BRG-02"}, {6, "M8-EXIT-01"}};
    for (const auto& [kind, task] : stubs) RequireStub(fx.Validate(fx.Inputs({{kind}}, {})), task);
}

BOOST_AUTO_TEST_CASE(fact_cursors_inside_the_range_and_ascending)
{
    const auto deposit_at = [](const FactCursor& cursor, std::string_view outpoint) {
        DepositFact fact;
        fact.cursor = cursor;
        fact.outpoint = TestOutpoint(outpoint, 0);
        fact.account = KeyAccount("alice");
        fact.asset = RUSD;
        fact.amount = 1;
        return EncodeFact(Fact{fact}).value();
    };
    SlotFixture fx{Funded()};
    SlotInputs inputs{fx.Inputs({}, {})};
    inputs.facts_to = FactCursor{130, 3};
    inputs.body.facts = {deposit_at({130, 0}, "a"), deposit_at({130, 2}, "b")};
    RequireOk(fx.Validate(inputs), "cursors 0 and 2 of [0, 3)");
    inputs.body.facts = {deposit_at({129, 7}, "a")};
    RequireRefusal(fx.Validate(inputs), SLOT_FACT_CURSOR, "below facts_from");
    inputs.body.facts = {deposit_at({130, 3}, "a")};
    RequireRefusal(fx.Validate(inputs), SLOT_FACT_CURSOR, "at facts_to");
    inputs.body.facts = {deposit_at({130, 2}, "a"), deposit_at({130, 1}, "b")};
    RequireRefusal(fx.Validate(inputs), SLOT_FACT_CURSOR, "descending");
    inputs.body.facts = {deposit_at({130, 1}, "a"), deposit_at({130, 1}, "b")};
    RequireRefusal(fx.Validate(inputs), SLOT_FACT_CURSOR, "repeated cursor");
}

BOOST_AUTO_TEST_CASE(deposits_the_ledger_would_refuse)
{
    SlotFixture fx{Funded()};
    RequireRefusal(fx.Validate(fx.Inputs({fx.DepositBytes(KeyAccount("alice"), RUSD, 0, "zero")}, {})),
                   SLOT_DEPOSIT_REFUSED, "amount 0");
    fx.next_fact_index = 0;
    RequireRefusal(fx.Validate(fx.Inputs({fx.DepositBytes(SeatPoolAccount(fx.profile), RUSD, 5, "pool")}, {})),
                   SLOT_DEPOSIT_REFUSED, "deposit to the seat pool");
    fx.next_fact_index = 0;
    RequireRefusal(fx.Validate(fx.Inputs({fx.DepositBytes(KeyAccount("alice"), RUSD, 5, "fund-alice")}, {})),
                   SLOT_DEPOSIT_REFUSED, "outpoint credited in the parent");
    fx.next_fact_index = 0;
    std::vector<std::vector<unsigned char>> twice{fx.DepositBytes(KeyAccount("alice"), RUSD, 5, "same"),
                                                  fx.DepositBytes(KeyAccount("bob"), RUSD, 6, "same")};
    RequireRefusal(fx.Validate(fx.Inputs(twice, {})), SLOT_DEPOSIT_REFUSED, "one outpoint twice in the slot");
}

BOOST_AUTO_TEST_CASE(originals_must_decode)
{
    SlotFixture fx{Funded()};
    RequireRefusal(fx.Validate(fx.Inputs({}, {{0x01, 0x02}})), SLOT_ACTION_MALFORMED, "truncated");
    std::vector<unsigned char> unknown_kind{AlicePlace(fx, 1)};
    unknown_kind[ACTION_KIND_OFFSET] = 99;
    RequireRefusal(fx.Validate(fx.Inputs({}, {unknown_kind})), SLOT_ACTION_MALFORMED, "unknown kind byte");
    std::vector<unsigned char> trailing{AlicePlace(fx, 1)};
    trailing.push_back(0);
    RequireRefusal(fx.Validate(fx.Inputs({}, {trailing})), SLOT_ACTION_MALFORMED, "trailing byte");
}

BOOST_AUTO_TEST_CASE(action_domain_kind_and_payload)
{
    SlotFixture fx{Funded()};
    ActionV2 foreign{fx.Unsigned("alice", 1, ActionKind::PLACE,
                                 EncodePayload(PlacePayload{MarketM1().Id(), Side::BUY, TimeInForce::GTC, 1, 1}).value())};
    foreign.domain = LabelHash("fm2/test/other-domain");
    RequireRefusal(fx.Validate(fx.Inputs({}, {fx.Sign(foreign, "alice")})), SLOT_ACTION_DOMAIN, "another domain");

    for (const ActionKind kind : {ActionKind::CANCEL_ALL, ActionKind::WITHDRAW, ActionKind::SPOT_TO_FUTURES,
                                  ActionKind::FUTURES_TO_SPOT}) {
        const std::vector<unsigned char> payload{kind == ActionKind::WITHDRAW
                                                     ? EncodePayload(WithdrawPayload{RUSD, 1, {0x51}}).value()
                                                     : std::vector<unsigned char>{}};
        RequireRefusal(fx.Validate(fx.Inputs({}, {fx.Sign(fx.Unsigned("alice", 1, kind, payload), "alice")})),
                       SLOT_ACTION_KIND, std::string{"kind "} + std::string{ToString(kind)});
    }

    RequireRefusal(fx.Validate(fx.Inputs({}, {fx.Sign(fx.Unsigned("alice", 1, ActionKind::PLACE, {0x01}), "alice")})),
                   SLOT_ACTION_PAYLOAD, "truncated PLACE payload");
    std::vector<unsigned char> bad_side{EncodePayload(PlacePayload{MarketM1().Id(), Side::BUY, TimeInForce::GTC, 1, 1}).value()};
    bad_side[32] = 7;
    RequireRefusal(fx.Validate(fx.Inputs({}, {fx.Sign(fx.Unsigned("alice", 1, ActionKind::PLACE, bad_side), "alice")})),
                   SLOT_ACTION_PAYLOAD, "unknown side");
    RequireRefusal(fx.Validate(fx.Inputs({}, {fx.Sign(fx.Unsigned("alice", 1, ActionKind::CANCEL, {0x01}), "alice")})),
                   SLOT_ACTION_PAYLOAD, "truncated CANCEL payload");
}

BOOST_AUTO_TEST_CASE(action_outside_its_window)
{
    SlotFixture fx{Funded()};
    fx.header.height = 20; // the slot is 21
    const std::vector<unsigned char> place{EncodePayload(PlacePayload{MarketM1().Id(), Side::BUY, TimeInForce::GTC, 1, 1}).value()};
    const auto with_window = [&](uint64_t ref_slot, uint16_t max_delay) {
        return fx.Validate(fx.Inputs({}, {fx.Sign(fx.Unsigned("alice", 1, ActionKind::PLACE, place, ref_slot, max_delay), "alice")}));
    };
    RequireOk(with_window(20, 1), "ref 20, delay 1: last slot 21");
    RequireOk(with_window(5, 16), "ref 5, delay 16: last slot 21");
    RequireRefusal(with_window(4, 16), SLOT_ACTION_WINDOW, "ref 4, delay 16: expired at 21");
    RequireRefusal(with_window(21, 16), SLOT_ACTION_WINDOW, "ref_slot not below the slot");
    RequireRefusal(with_window(20, 0), SLOT_ACTION_WINDOW, "max_delay 0");
    RequireRefusal(with_window(20, 1'025), SLOT_ACTION_WINDOW, "max_delay above max_delay_slots");
    RequireRefusal(with_window(std::numeric_limits<uint64_t>::max(), 1'024), SLOT_ACTION_WINDOW, "ref_slot UINT64_MAX");
}

BOOST_AUTO_TEST_CASE(duplicate_action_id_and_duplicate_nonce)
{
    SlotFixture fx{Funded()};
    const std::vector<unsigned char> once{AlicePlace(fx, 1)};
    RequireRefusal(fx.Validate(fx.Inputs({}, {once, once})), SLOT_DUPLICATE_ACTION, "the same bytes twice");
    // Two valid signatures over one ActionId are two distinct originals but one action.
    const ActionV2 unsigned_place{fx.Unsigned("alice", 1, ActionKind::PLACE,
                                              EncodePayload(PlacePayload{MarketM1().Id(), Side::BUY, TimeInForce::GTC, 250, 10}).value())};
    const std::vector<unsigned char> sig_a{fx.Sign(unsigned_place, "alice")};
    const std::vector<unsigned char> sig_b{fx.Sign(unsigned_place, "alice")};
    BOOST_REQUIRE(sig_a != sig_b);
    RequireRefusal(fx.Validate(fx.Inputs({}, {sig_a, sig_b})), SLOT_DUPLICATE_ACTION, "two signatures, one ActionId");

    // Two different actions of one account with one nonce.
    RequireRefusal(fx.Validate(fx.Inputs({}, {AlicePlace(fx, 3), fx.Transfer("alice", 3, KeyAccount("bob"), RUSD, 1)})),
                   SLOT_DUPLICATE_NONCE, "one (account, nonce) twice");
    // The same nonce from two accounts is fine.
    RequireOk(fx.Validate(fx.Inputs({}, {AlicePlace(fx, 3), fx.Transfer("bob", 3, KeyAccount("alice"), B3, 1)})),
              "one nonce, two accounts");
}

BOOST_AUTO_TEST_CASE(account_absent_in_the_parent)
{
    SlotFixture fx{Funded()};
    RequireRefusal(fx.Validate(fx.Inputs({}, {fx.Place("carol", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 1, 1)})),
                   SLOT_ACCOUNT_ABSENT, "carol has no account");
    // A deposit in the same slot does not count: the parent state decides.
    std::vector<std::vector<unsigned char>> facts{fx.DepositBytes(KeyAccount("carol"), RUSD, 5, "carol")};
    RequireRefusal(fx.Validate(fx.Inputs(facts, {fx.Place("carol", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 1, 1)})),
                   SLOT_ACCOUNT_ABSENT, "carol's deposit is in the same slot");
}

BOOST_AUTO_TEST_CASE(nonce_excluded_by_the_parent_window)
{
    ProfileParams w3{RegtestProfile()};
    w3.nonce_window = 3;
    SlotFixture fx{Funded(w3)};
    RequireOk(fx.state.ledger.ConsumeNonce(KeyAccount("alice"), 5), "used 5");
    RequireRefusal(fx.Validate(fx.Inputs({}, {AlicePlace(fx, 5)})), SLOT_NONCE_EXCLUDED, "nonce 5 used");
    RequireOk(fx.Validate(fx.Inputs({}, {AlicePlace(fx, 4)})), "nonce 4 below 5 while the window is not full");

    for (const uint64_t n : {10, 11}) RequireOk(fx.state.ledger.ConsumeNonce(KeyAccount("alice"), n), "used");
    // Window {5, 10, 11} is full: 4 is too low, 6..9 and 12+ are acceptable.
    RequireRefusal(fx.Validate(fx.Inputs({}, {AlicePlace(fx, 4)})), SLOT_NONCE_EXCLUDED, "nonce 4 below a full window");
    RequireRefusal(fx.Validate(fx.Inputs({}, {AlicePlace(fx, 10)})), SLOT_NONCE_EXCLUDED, "nonce 10 used");
    // Several nonces of one account in one slot: each is acceptable against the
    // parent, and inserted in ascending order they all fit (step 9).
    RequireOk(fx.Validate(fx.Inputs({}, {AlicePlace(fx, 13), AlicePlace(fx, 6), AlicePlace(fx, 12), AlicePlace(fx, 7)})),
              "nonces 6, 7, 12, 13 into {5, 10, 11}");
}

BOOST_AUTO_TEST_CASE(invalid_signature_is_checked_last)
{
    SlotFixture fx{Funded()};
    std::vector<unsigned char> bad{AlicePlace(fx, 1)};
    bad.back() ^= 0x01;
    RequireRefusal(fx.Validate(fx.Inputs({}, {bad})), SLOT_BAD_SIGNATURE, "flipped signature bit");
    // Cheaper checks win over the signature, so they never cost a BIP340 verification.
    std::vector<unsigned char> bad_carol{fx.Place("carol", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 1, 1)};
    bad_carol.back() ^= 0x01;
    RequireRefusal(fx.Validate(fx.Inputs({}, {bad, bad_carol})), SLOT_ACCOUNT_ABSENT, "absent account before signatures");
    RequireRefusal(fx.Validate(fx.Inputs({{0x09}}, {bad})), SLOT_FACT_MALFORMED, "facts before originals");
}

BOOST_AUTO_TEST_CASE(engine_state_invariants_and_commit)
{
    SlotFixture fx{Funded()};
    RequireOk(fx.state.CheckInvariants(), "funded genesis");
    const std::vector<MarketDescriptor> markets{fx.state.Markets()};
    BOOST_REQUIRE_EQUAL(markets.size(), 2U);
    BOOST_CHECK(markets[0].Id() < markets[1].Id());
    const Result<StateRootsV0> roots{fx.state.Commit()};
    RequireOk(roots, "Commit");
    BOOST_CHECK(*roots == ledger::CommitTradingStateV0(fx.state.ledger, markets, fx.state.OrderLeaves()).value());

    const AccountId alice{KeyAccount("alice")};
    // A resting order with its exact reservation and open-order count keeps the invariants.
    EngineState with_order{fx.state};
    BOOST_REQUIRE(with_order.ledger.Reserve(alice, RUSD, 2'500).value() == ledger::ReserveResult::OK);
    BOOST_REQUIRE(with_order.ledger.AddOpenOrder(alice).value() == ledger::OpenOrderResult::OK);
    RestingOrder order;
    order.id = OrderId{LabelHash("fm2/test/order/o")};
    order.owner = alice;
    order.side = Side::BUY;
    order.price_ticks = 250;
    order.remaining_lots = 10;
    order.arrival_rank = 1;
    order.reserved_left = 2'500;
    RequireOk(with_order.books.FindMutable(MarketM1().Id())->Rest(order), "Rest");
    RequireOk(with_order.CheckInvariants(), "one resting order");
    BOOST_REQUIRE_EQUAL(with_order.OrderLeaves().size(), 1U);
    BOOST_CHECK(with_order.OrderLeaves()[0].market == MarketM1().Id());
    BOOST_CHECK(with_order.Commit().value().orders != roots->orders);

    EngineState broken{fx.state};
    BOOST_REQUIRE(broken.ledger.Reserve(alice, RUSD, 1).value() == ledger::ReserveResult::OK);
    RequireError(broken.CheckInvariants(), ErrorCode::INVARIANT, "a reservation without an order");
    broken = fx.state;
    BOOST_REQUIRE(broken.ledger.AddOpenOrder(alice).value() == ledger::OpenOrderResult::OK);
    RequireError(broken.CheckInvariants(), ErrorCode::INVARIANT, "an open order without a book entry");
    broken = with_order;
    RequireOk(broken.ledger.Release(alice, RUSD, 2'500), "release");
    RequireError(broken.CheckInvariants(), ErrorCode::INVARIANT, "a resting order the ledger does not cover");
    broken = fx.state;
    BOOST_REQUIRE(broken.ledger.Reserve(alice, RUSD, 100).value() == ledger::ReserveResult::OK);
    RequireOk(broken.ledger.PayFromReserved(alice, KeyAccount("bob"), RUSD, 100, 1), "pay with a fee");
    RequireError(broken.CheckInvariants(), ErrorCode::INVARIANT, "fees held at a slot boundary");
}

BOOST_AUTO_TEST_SUITE_END()
