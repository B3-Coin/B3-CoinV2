// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** The bounded action pool (slice R12; admission/pool.h).
 *
 * IN_PROPOSAL survives TakeForSlot; expiry, a used nonce and a nonce below
 * the window give terminal statuses; capacity, uniqueness per ActionId and
 * per (account, nonce), the withhold filter, commit removal, the retained
 * INCLUDED records A3 needs, retention and terminal capacity, refusals that
 * change nothing, and the property that a taken slot always passes the
 * validity predicate.
 */

#include <boost/test/unit_test.hpp>

#include <flowmesh2/admission/pool.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/merkle.h>
#include <flowmesh2/core/nonce_rule.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/test/util/admission_fixtures.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;
using admission::ActionPool;
using admission::InsertResult;
using admission::PoolLimits;
using admission::PoolStatus;

namespace {

//! A small PLACE (BUY M1, 1 lot at `price`) of `name` with an explicit window.
std::vector<unsigned char> PlaceWindow(SlotFixture& a, std::string_view name, uint64_t nonce, uint64_t ref_slot, uint16_t max_delay,
                                       uint64_t price = 1)
{
    return a.Sign(a.Unsigned(name, nonce, ActionKind::PLACE,
                             EncodePayload(PlacePayload{MarketM1().Id(), Side::BUY, TimeInForce::GTC, price, 1}).value(), ref_slot,
                             max_delay),
                  name);
}

//! A small PLACE at the head with max_delay 16.
std::vector<unsigned char> SmallPlace(SlotFixture& a, std::string_view name, uint64_t nonce, uint64_t price = 1)
{
    return a.Place(name, nonce, MarketM1(), Side::BUY, TimeInForce::GTC, price, 1);
}

ProfileParams ProfileWith(uint32_t nonce_window, uint32_t max_delay_slots, uint32_t max_actions_per_slot = 1'024)
{
    ProfileParams profile{RegtestProfile()};
    profile.nonce_window = nonce_window;
    profile.max_delay_slots = max_delay_slots;
    profile.outcome_index_slots = max_delay_slots;
    profile.max_actions_per_slot = max_actions_per_slot;
    return profile;
}

PoolRigOptions OptionsFor(const ProfileParams& profile, std::optional<PoolLimits> limits = std::nullopt)
{
    PoolRigOptions options;
    options.rig.profile = profile;
    options.limits = limits;
    return options;
}

std::array<unsigned char, ACTION_SIG_BYTES> SigOf(const std::vector<unsigned char>& bytes)
{
    std::array<unsigned char, ACTION_SIG_BYTES> sig{};
    std::copy(bytes.end() - ACTION_SIG_BYTES, bytes.end(), sig.begin());
    return sig;
}

//! Everything a refusal must leave unchanged.
struct PoolSnapshot {
    size_t size;
    uint64_t height;
    size_t included;
    size_t terminal;
    std::vector<std::optional<admission::PoolEntryStatus>> statuses;
    friend bool operator==(const PoolSnapshot&, const PoolSnapshot&) = default;
};

PoolSnapshot SnapshotOf(const ActionPool& pool, const std::vector<ActionId>& ids)
{
    PoolSnapshot s{pool.Size(), pool.HeadHeight(), pool.IncludedRecords(), pool.TerminalRecords(), {}};
    for (const ActionId& id : ids) s.statuses.push_back(pool.Status(id));
    return s;
}

} // namespace

BOOST_AUTO_TEST_SUITE(pool_tests)

BOOST_AUTO_TEST_CASE(create_limits_and_registries)
{
    const ProfileParams profile{RegtestProfile()};
    const PoolLimits defaults{PoolLimits::FromProfile(profile)};
    BOOST_CHECK_EQUAL(defaults.capacity, 4'096U);
    BOOST_CHECK_EQUAL(defaults.retention_slots, 1'024U);
    BOOST_CHECK_EQUAL(defaults.terminal_capacity, 4'096U);
    RequireOk(ActionPool::Create(defaults, profile, 0), "default limits");

    RequireFailure(ActionPool::Create(PoolLimits{0, 1'024, 1}, profile, 0), ErrorCode::INVALID_INPUT, admission::POOL_LIMIT_ZERO, "capacity 0");
    RequireFailure(ActionPool::Create(PoolLimits{1, 0, 1}, profile, 0), ErrorCode::INVALID_INPUT, admission::POOL_LIMIT_ZERO, "retention 0");
    RequireFailure(ActionPool::Create(PoolLimits{1, 1'024, 0}, profile, 0), ErrorCode::INVALID_INPUT, admission::POOL_LIMIT_ZERO, "terminal 0");
    RequireFailure(ActionPool::Create(PoolLimits{1, 1'023, 1}, profile, 0), ErrorCode::INVALID_INPUT, admission::POOL_RETENTION_SHORT,
                   "retention below max_delay_slots");
    RequireOk(ActionPool::Create(PoolLimits{1, 1'024, 1}, profile, 7), "retention = max_delay_slots");
    ProfileParams main{profile};
    main.chain = ChainType::MAIN;
    RequireError(ActionPool::Create(defaults, main, 0), ErrorCode::UNSUPPORTED_NETWORK, "MAIN profile");

    // Registries: integers and names are append-only.
    const std::vector<std::pair<PoolStatus, std::string_view>> statuses{
        {PoolStatus::PENDING, "PENDING"}, {PoolStatus::IN_PROPOSAL, "IN_PROPOSAL"}, {PoolStatus::INCLUDED, "INCLUDED"},
        {PoolStatus::NONCE_USED, "NONCE_USED"}, {PoolStatus::NONCE_TOO_LOW, "NONCE_TOO_LOW"}, {PoolStatus::EXPIRED, "EXPIRED"}};
    for (size_t i{0}; i < statuses.size(); ++i) {
        BOOST_CHECK_EQUAL(static_cast<int>(statuses[i].first), static_cast<int>(i + 1));
        BOOST_CHECK(admission::IsKnown(statuses[i].first));
        BOOST_CHECK_EQUAL(admission::ToString(statuses[i].first), statuses[i].second);
        BOOST_CHECK_EQUAL(admission::IsTerminal(statuses[i].first), i >= 2);
    }
    BOOST_CHECK(!admission::IsKnown(static_cast<PoolStatus>(0)));
    BOOST_CHECK(!admission::IsKnown(static_cast<PoolStatus>(7)));
    BOOST_CHECK(!admission::IsTerminal(static_cast<PoolStatus>(7)));
    BOOST_CHECK_EQUAL(admission::ToString(static_cast<PoolStatus>(7)), "UNKNOWN");
    const std::vector<std::pair<InsertResult, std::string_view>> results{{InsertResult::INSERTED, "INSERTED"},
                                                                         {InsertResult::DUPLICATE, "DUPLICATE"},
                                                                         {InsertResult::NONCE_PENDING, "NONCE_PENDING"},
                                                                         {InsertResult::POOL_FULL, "POOL_FULL"},
                                                                         {InsertResult::STALE_SNAPSHOT, "STALE_SNAPSHOT"}};
    for (size_t i{0}; i < results.size(); ++i) {
        BOOST_CHECK_EQUAL(static_cast<int>(results[i].first), static_cast<int>(i + 1));
        BOOST_CHECK_EQUAL(admission::ToString(results[i].first), results[i].second);
    }
    BOOST_CHECK_EQUAL(admission::ToString(static_cast<InsertResult>(6)), "UNKNOWN");
}

BOOST_AUTO_TEST_CASE(insert_status_and_lookups)
{
    PoolRig r;
    r.Fund();
    SlotFixture& a{r.Actions()};
    const auto x{SmallPlace(a, "alice", 1)};
    const auto y{SmallPlace(a, "bob", 1)};
    const admission::InsertReply first{r.Insert(x)};
    BOOST_REQUIRE(first.result == InsertResult::INSERTED);
    BOOST_REQUIRE(first.pending.has_value());
    BOOST_CHECK(first.pending->id == IdOfBytes(x));
    BOOST_CHECK_EQUAL(first.pending->seq, 1U);
    BOOST_CHECK_EQUAL(first.pending->snapshot_height, 1U);
    BOOST_CHECK(!first.included.has_value());
    const admission::InsertReply second{r.Insert(y)};
    BOOST_REQUIRE(second.result == InsertResult::INSERTED);
    BOOST_CHECK_EQUAL(second.pending->seq, 2U);
    BOOST_CHECK_EQUAL(r.pool->Size(), 2U);

    const admission::PoolEntryStatus status{RequireStatus(*r.pool, IdOfBytes(x))};
    BOOST_CHECK(status.status == PoolStatus::PENDING);
    BOOST_CHECK(status.pending == first.pending);
    BOOST_CHECK(!status.included.has_value());
    BOOST_CHECK_EQUAL(status.height, 0U);

    // A3: the exact signature is authenticated; another one is not. A9: by id.
    const auto known{r.pool->FindAuthenticated(IdOfBytes(x), SigOf(x))};
    BOOST_REQUIRE(known.has_value());
    BOOST_CHECK(known->status == PoolStatus::PENDING);
    BOOST_CHECK(known->pending == first.pending);
    std::array<unsigned char, ACTION_SIG_BYTES> other{SigOf(x)};
    other[0] ^= 1;
    BOOST_CHECK(!r.pool->FindAuthenticated(IdOfBytes(x), other).has_value());
    BOOST_REQUIRE(r.pool->FindById(IdOfBytes(x)).has_value());
    BOOST_CHECK(r.pool->FindById(IdOfBytes(x))->pending == first.pending);
    const auto absent{SmallPlace(a, "carol", 1)};
    BOOST_CHECK(!r.pool->FindById(IdOfBytes(absent)).has_value());
    BOOST_CHECK(!r.pool->Status(IdOfBytes(absent)).has_value());
}

BOOST_AUTO_TEST_CASE(duplicate_nonce_pending_capacity_and_stale)
{
    PoolRig r{OptionsFor(RegtestProfile(), PoolLimits{2, 1'024, 4})};
    r.Fund();
    SlotFixture& a{r.Actions()};
    const ActionV2 unsigned_x{a.Unsigned("alice", 1, ActionKind::PLACE,
                                         EncodePayload(PlacePayload{MarketM1().Id(), Side::BUY, TimeInForce::GTC, 1, 1}).value())};
    const auto x1{a.Sign(unsigned_x, "alice")};
    const auto x2{a.Sign(unsigned_x, "alice")};
    BOOST_REQUIRE(x1 != x2);
    BOOST_REQUIRE(IdOfBytes(x1) == IdOfBytes(x2));
    const auto same_nonce{SmallPlace(a, "alice", 1, /*price=*/2)};
    const auto bob{SmallPlace(a, "bob", 1)};
    const auto carol{SmallPlace(a, "carol", 1)};

    const admission::InsertReply inserted{r.Insert(x1)};
    BOOST_REQUIRE(inserted.result == InsertResult::INSERTED);
    const std::vector<ActionId> ids{IdOfBytes(x1), IdOfBytes(same_nonce), IdOfBytes(bob), IdOfBytes(carol)};
    const PoolSnapshot before{SnapshotOf(*r.pool, ids)};

    // The exact bytes and another signature over the same ActionId are duplicates of the live entry.
    for (const auto* bytes : {&x1, &x2}) {
        const admission::InsertReply dup{r.Insert(*bytes)};
        BOOST_CHECK(dup.result == InsertResult::DUPLICATE);
        BOOST_CHECK(dup.pending == inserted.pending);
        BOOST_CHECK(!dup.included.has_value());
    }
    // Another action with the same (account, nonce).
    const admission::InsertReply pending{r.Insert(same_nonce)};
    BOOST_CHECK(pending.result == InsertResult::NONCE_PENDING);
    BOOST_CHECK(!pending.pending.has_value());
    BOOST_CHECK(SnapshotOf(*r.pool, ids) == before);

    // Capacity 2.
    BOOST_REQUIRE(r.Insert(bob).result == InsertResult::INSERTED);
    const PoolSnapshot full{SnapshotOf(*r.pool, ids)};
    BOOST_CHECK(r.Insert(carol).result == InsertResult::POOL_FULL);
    // A check-time conflict outranks fullness: the same id or nonce is still reported as such.
    BOOST_CHECK(r.Insert(x2).result == InsertResult::DUPLICATE);
    BOOST_CHECK(r.Insert(same_nonce).result == InsertResult::NONCE_PENDING);
    BOOST_CHECK(SnapshotOf(*r.pool, ids) == full);

    // A snapshot older than the pool's head is stale; nothing changes.
    r.Seal({}, [](const ActionV2&) { return true; }); // withhold everything: head 2
    BOOST_REQUIRE_EQUAL(r.pool->HeadHeight(), 2U);
    const PoolSnapshot after_seal{SnapshotOf(*r.pool, ids)};
    BOOST_CHECK(r.pool->Insert(VerifiedOf(carol), 1).result == InsertResult::STALE_SNAPSHOT);
    BOOST_CHECK(r.pool->Insert(VerifiedOf(x2), 0).result == InsertResult::STALE_SNAPSHOT);
    BOOST_CHECK(SnapshotOf(*r.pool, ids) == after_seal);
    // A snapshot newer than the pool's head is accepted (OnCommitted re-checks it).
    PoolRig wide{OptionsFor(RegtestProfile())};
    wide.Fund();
    BOOST_CHECK(wide.pool->Insert(VerifiedOf(carol), 9).result == InsertResult::INSERTED);
}

BOOST_AUTO_TEST_CASE(take_marks_in_proposal_and_keeps_entries)
{
    PoolRig r;
    r.Fund();
    SlotFixture& a{r.Actions()};
    const std::vector<std::vector<unsigned char>> actions{SmallPlace(a, "bob", 1), SmallPlace(a, "alice", 1), SmallPlace(a, "alice", 2)};
    for (const auto& bytes : actions) BOOST_REQUIRE(r.Insert(bytes).result == InsertResult::INSERTED);

    Result<admission::TakenSlot> taken{r.pool->TakeForSlot(2, 10)};
    RequireOk(taken, "TakeForSlot");
    BOOST_CHECK_EQUAL(taken->height, 2U);
    BOOST_CHECK(taken->originals == actions); // admission order
    BOOST_REQUIRE_EQUAL(taken->ids.size(), 3U);
    for (size_t i{0}; i < actions.size(); ++i) {
        BOOST_CHECK(taken->ids[i] == IdOfBytes(actions[i]));
        const admission::PoolEntryStatus status{RequireStatus(*r.pool, taken->ids[i])};
        BOOST_CHECK(status.status == PoolStatus::IN_PROPOSAL);
        BOOST_CHECK_EQUAL(status.height, 2U);
    }
    BOOST_CHECK_EQUAL(r.pool->Size(), 3U); // IN_PROPOSAL entries stay in the pool
    // A retried proposal takes them again; `max` bounds the count.
    Result<admission::TakenSlot> again{r.pool->TakeForSlot(2, 2)};
    RequireOk(again, "TakeForSlot again");
    BOOST_CHECK(again->originals == (std::vector<std::vector<unsigned char>>{actions[0], actions[1]}));
    BOOST_CHECK(RequireStatus(*r.pool, IdOfBytes(actions[2])).status == PoolStatus::IN_PROPOSAL);
    Result<admission::TakenSlot> none{r.pool->TakeForSlot(2, 0)};
    RequireOk(none, "TakeForSlot max 0");
    BOOST_CHECK(none->originals.empty());
    // Only the successor of the pool's head.
    RequireFailure(r.pool->TakeForSlot(1, 10), ErrorCode::ORDER_VIOLATION, admission::POOL_TAKE_HEIGHT, "height = head");
    RequireFailure(r.pool->TakeForSlot(3, 10), ErrorCode::ORDER_VIOLATION, admission::POOL_TAKE_HEIGHT, "height = head + 2");
    BOOST_CHECK_EQUAL(r.pool->Size(), 3U);

    // The profile's max_actions_per_slot also bounds a slot.
    PoolRig capped{OptionsFor(ProfileWith(100, 1'024, /*max_actions_per_slot=*/2))};
    capped.Fund();
    for (uint64_t n{1}; n <= 3; ++n) BOOST_REQUIRE(capped.Insert(SmallPlace(capped.Actions(), "alice", n)).result == InsertResult::INSERTED);
    Result<admission::TakenSlot> two{capped.pool->TakeForSlot(2, 10)};
    RequireOk(two, "capped TakeForSlot");
    BOOST_CHECK_EQUAL(two->originals.size(), 2U);
}

BOOST_AUTO_TEST_CASE(take_skips_expired_and_withholds_then_expiry_is_terminal)
{
    PoolRig r;
    r.Fund();
    r.SealOriginals({}, {});
    r.SealOriginals({}, {}); // head 3
    SlotFixture& a{r.Actions()};
    // w: valid only in slot 4 (ref 3, max_delay 1), as in ARCHITECTURE.md 14.2.
    const auto w{PlaceWindow(a, "alice", 5, 3, 1)};
    const auto x{SmallPlace(a, "alice", 6)};
    BOOST_REQUIRE(r.Insert(w).result == InsertResult::INSERTED);
    BOOST_REQUIRE(r.Insert(x).result == InsertResult::INSERTED);
    const ActionId w_id{IdOfBytes(w)};
    const auto withhold_w = [&](const ActionV2& action) { return action.Id() == w_id; };

    PoolRig::Sealed slot4{r.Seal({}, withhold_w)};
    BOOST_CHECK(slot4.taken.originals == (std::vector<std::vector<unsigned char>>{x}));
    BOOST_CHECK_EQUAL(slot4.report.included, 1U);
    BOOST_CHECK_EQUAL(slot4.report.expired, 0U); // 4 > 3 + 1 is false: w could still have been included
    BOOST_CHECK(RequireStatus(*r.pool, w_id).status == PoolStatus::PENDING);

    // Slot 5 is outside w's window: TakeForSlot skips it, OnCommitted(5) makes it terminal.
    PoolRig::Sealed slot5{r.Seal()};
    BOOST_CHECK(slot5.taken.originals.empty());
    BOOST_CHECK_EQUAL(slot5.report.expired, 1U);
    const admission::PoolEntryStatus status{RequireStatus(*r.pool, w_id)};
    BOOST_CHECK(status.status == PoolStatus::EXPIRED);
    BOOST_CHECK_EQUAL(status.height, 5U);
    BOOST_REQUIRE(status.pending.has_value());
    BOOST_CHECK(status.pending->id == w_id);
    BOOST_CHECK_EQUAL(r.pool->Size(), 0U);
    BOOST_CHECK(!r.pool->FindById(w_id).has_value()); // terminal records other than INCLUDED never deduplicate
    BOOST_CHECK_EQUAL(r.pool->TerminalRecords(), 1U);
}

BOOST_AUTO_TEST_CASE(commit_removes_included_and_retains_them)
{
    PoolRig r;
    r.Fund();
    SlotFixture& a{r.Actions()};
    const auto x{SmallPlace(a, "alice", 1)};
    const auto y{a.Transfer("bob", 1, KeyAccount("alice"), TestAsset("TOK"), 5)};
    const admission::InsertReply xr{r.Insert(x)};
    const admission::InsertReply yr{r.Insert(y)};
    PoolRig::Sealed sealed{r.Seal()};
    BOOST_CHECK_EQUAL(sealed.report.height, 2U);
    BOOST_CHECK_EQUAL(sealed.report.included, 2U);
    BOOST_CHECK_EQUAL(sealed.report.recorded, 2U);
    BOOST_CHECK_EQUAL(r.pool->Size(), 0U);
    BOOST_CHECK_EQUAL(r.pool->IncludedRecords(), 2U);
    BOOST_CHECK_EQUAL(r.pool->HeadHeight(), 2U);
    for (const auto& [bytes, reply] : {std::pair{&x, &xr}, std::pair{&y, &yr}}) {
        const ActionId id{IdOfBytes(*bytes)};
        const auto [index, outcome] = FindOutcome(sealed.slot.slot.outcomes, id.id);
        const admission::PoolEntryStatus status{RequireStatus(*r.pool, id)};
        BOOST_CHECK(status.status == PoolStatus::INCLUDED);
        BOOST_CHECK(status.pending == reply->pending);
        BOOST_REQUIRE(status.included.has_value());
        BOOST_CHECK(*status.included == (read::IncludedRef{2, index}));
        BOOST_CHECK_EQUAL(status.height, 2U);
        // The read service names the same position.
        Result<read::ActionReply> read{r.read->Action(DecodeAction(*bytes))};
        RequireOk(read, "ReadService::Action");
        BOOST_REQUIRE(read->status == read::ActionStatus::INCLUDED);
        BOOST_CHECK(read->included->ref == *status.included);
        // A3 finds the exact original; A9 finds the id; a resend is a duplicate of the record.
        const auto known{r.pool->FindAuthenticated(id, SigOf(*bytes))};
        BOOST_REQUIRE(known.has_value());
        BOOST_CHECK(known->status == PoolStatus::INCLUDED);
        BOOST_CHECK(known->included == status.included);
        BOOST_CHECK(r.pool->FindById(id).has_value());
        const admission::InsertReply dup{r.Insert(*bytes)};
        BOOST_CHECK(dup.result == InsertResult::DUPLICATE);
        BOOST_CHECK(dup.included == status.included);
    }
    // An action the slot did not include, with an unrelated nonce, stays.
    const auto z{SmallPlace(a, "alice", 7)};
    BOOST_REQUIRE(r.Insert(z).result == InsertResult::INSERTED);
    PoolRig::Sealed empty{r.Seal({}, [](const ActionV2&) { return true; })};
    BOOST_CHECK(empty.report == (admission::CommitReport{3, 0, 0, 0, 0, 0, 0, 0}));
    BOOST_CHECK(RequireStatus(*r.pool, IdOfBytes(z)).status == PoolStatus::PENDING);
}

BOOST_AUTO_TEST_CASE(included_originals_the_pool_never_held_are_recorded)
{
    PoolRig r;
    r.Fund();
    SlotFixture& a{r.Actions()};
    const ActionV2 unsigned_x{a.Unsigned("alice", 1, ActionKind::PLACE,
                                         EncodePayload(PlacePayload{MarketM1().Id(), Side::BUY, TimeInForce::GTC, 1, 1}).value())};
    const auto held{a.Sign(unsigned_x, "alice")};
    const auto other_sig{a.Sign(unsigned_x, "alice")};
    const auto foreign{SmallPlace(a, "bob", 1)};
    const admission::InsertReply reply{r.Insert(held)};
    BOOST_REQUIRE(reply.result == InsertResult::INSERTED);

    // Another seat's proposal included the other signature over the same ActionId, and bob's action.
    PoolRig::Sealed sealed{r.SealOriginals({}, {other_sig, foreign})};
    BOOST_CHECK_EQUAL(sealed.report.included, 1U); // the live entry left, whatever signature it held
    BOOST_CHECK_EQUAL(sealed.report.recorded, 2U);
    BOOST_CHECK_EQUAL(r.pool->Size(), 0U);
    const ActionId id{IdOfBytes(held)};
    const admission::PoolEntryStatus status{RequireStatus(*r.pool, id)};
    BOOST_CHECK(status.status == PoolStatus::INCLUDED);
    BOOST_CHECK(status.pending == reply.pending);
    // The record holds the INCLUDED original's signature.
    BOOST_CHECK(r.pool->FindAuthenticated(id, SigOf(other_sig)).has_value());
    BOOST_CHECK(!r.pool->FindAuthenticated(id, SigOf(held)).has_value());
    BOOST_CHECK(r.pool->FindById(id).has_value());
    const admission::PoolEntryStatus foreign_status{RequireStatus(*r.pool, IdOfBytes(foreign))};
    BOOST_CHECK(foreign_status.status == PoolStatus::INCLUDED);
    BOOST_CHECK(!foreign_status.pending.has_value());
    BOOST_CHECK(r.pool->FindAuthenticated(IdOfBytes(foreign), SigOf(foreign)).has_value());
}

BOOST_AUTO_TEST_CASE(same_nonce_loser_and_nonce_below_the_window_are_terminal)
{
    PoolRig r{OptionsFor(ProfileWith(/*nonce_window=*/2, 1'024))};
    r.Fund();
    SlotFixture& a{r.Actions()};
    const auto loser{SmallPlace(a, "alice", 1)};
    const auto low{SmallPlace(a, "bob", 1)};
    const admission::InsertReply loser_reply{r.Insert(loser)};
    BOOST_REQUIRE(loser_reply.result == InsertResult::INSERTED);
    BOOST_REQUIRE(r.Insert(low).result == InsertResult::INSERTED);
    // The winner: another alice action with nonce 1. Bob uses 2 and 3, so his window {2, 3} is full and 1 is too low.
    const auto winner{a.Transfer("alice", 1, KeyAccount("bob"), TestAsset("rUSD"), 1)};
    const auto bob2{a.Transfer("bob", 2, KeyAccount("alice"), TestAsset("TOK"), 1)};
    const auto bob3{a.Transfer("bob", 3, KeyAccount("alice"), TestAsset("TOK"), 1)};
    PoolRig::Sealed sealed{r.SealOriginals({}, {winner, bob2, bob3})};
    BOOST_CHECK_EQUAL(sealed.report.nonce_used, 1U);
    BOOST_CHECK_EQUAL(sealed.report.nonce_too_low, 1U);
    BOOST_CHECK_EQUAL(sealed.report.included, 0U);
    BOOST_CHECK_EQUAL(r.pool->Size(), 0U);
    const admission::PoolEntryStatus used{RequireStatus(*r.pool, IdOfBytes(loser))};
    BOOST_CHECK(used.status == PoolStatus::NONCE_USED);
    BOOST_CHECK(used.pending == loser_reply.pending);
    BOOST_CHECK_EQUAL(used.height, 2U);
    BOOST_CHECK(RequireStatus(*r.pool, IdOfBytes(low)).status == PoolStatus::NONCE_TOO_LOW);
    BOOST_CHECK(RequireStatus(*r.pool, IdOfBytes(winner)).status == PoolStatus::INCLUDED);
    BOOST_CHECK_EQUAL(r.pool->TerminalRecords(), 2U);
    BOOST_CHECK(!r.pool->FindById(IdOfBytes(loser)).has_value());
}

BOOST_AUTO_TEST_CASE(in_proposal_returns_to_pending_when_another_slot_commits)
{
    PoolRig r;
    r.Fund();
    const auto x{SmallPlace(r.Actions(), "alice", 1)};
    BOOST_REQUIRE(r.Insert(x).result == InsertResult::INSERTED);
    RequireOk(r.pool->TakeForSlot(2, 10), "TakeForSlot");
    BOOST_CHECK(RequireStatus(*r.pool, IdOfBytes(x)).status == PoolStatus::IN_PROPOSAL);
    // Slot 2 commits without x (another proposal won).
    PoolRig::Sealed sealed{r.SealOriginals({}, {})};
    BOOST_CHECK_EQUAL(sealed.report.returned, 1U);
    const admission::PoolEntryStatus status{RequireStatus(*r.pool, IdOfBytes(x))};
    BOOST_CHECK(status.status == PoolStatus::PENDING);
    BOOST_CHECK_EQUAL(status.height, 0U);
    PoolRig::Sealed next{r.Seal()};
    BOOST_CHECK_EQUAL(next.report.included, 1U);
}

BOOST_AUTO_TEST_CASE(on_committed_refusals_change_nothing)
{
    PoolRig r;
    const PoolRig::Sealed funded{r.Fund()};
    SlotFixture& a{r.Actions()};
    const auto x{SmallPlace(a, "alice", 1)};
    const auto y{SmallPlace(a, "bob", 1)};
    BOOST_REQUIRE(r.Insert(x).result == InsertResult::INSERTED);
    RequireOk(r.pool->TakeForSlot(2, 10), "TakeForSlot");
    // Commit slot 2 (x and y) on the owner and the read service only; the pool is told below.
    state::Candidate candidate{r.rig.Preview(r.rig.Inputs({}, {x, y}))};
    const SlotBody body{candidate.Body()};
    PublishableSlot slot{r.rig.Commit(std::move(candidate))};
    Publish(*r.read, slot);
    const state::HeadView& head{*slot.slot.head};
    const std::vector<OutcomeV2>& outcomes{slot.slot.outcomes};
    const std::vector<ActionId> ids{IdOfBytes(x), IdOfBytes(y)};
    const PoolSnapshot before{SnapshotOf(*r.pool, ids)};

    const auto doctored = [&](const SlotHeaderV2& header) {
        Result<state::HeadView> view{state::HeadView::Make(header, head.CertHash(), head.StatePtr())};
        RequireOk(view, "HeadView::Make");
        return std::move(view).value();
    };
    // Profile.
    SlotHeaderV2 profile_header{head.Header()};
    profile_header.profile_hash = uint256::ONE;
    RequireFailure(r.pool->OnCommitted(slot.applied, doctored(profile_header), outcomes, body), ErrorCode::INVALID_INPUT, admission::POOL_PROFILE_MISMATCH,
                   "profile");
    // Outcomes: one dropped, one changed.
    std::vector<OutcomeV2> fewer{outcomes.begin(), outcomes.end() - 1};
    RequireFailure(r.pool->OnCommitted(slot.applied, head, fewer, body), ErrorCode::INVALID_INPUT, admission::POOL_OUTCOMES_MISMATCH, "outcome dropped");
    std::vector<OutcomeV2> changed{outcomes};
    changed[0].remaining_lots ^= 1;
    RequireFailure(r.pool->OnCommitted(slot.applied, head, changed, body), ErrorCode::INVALID_INPUT, admission::POOL_OUTCOMES_MISMATCH, "outcome changed");
    // Body: an original dropped.
    SlotBody short_body{body};
    short_body.originals.pop_back();
    RequireFailure(r.pool->OnCommitted(slot.applied, head, outcomes, short_body), ErrorCode::INVALID_INPUT, admission::POOL_BODY_MISMATCH, "original dropped");
    // Body: an original replaced by another valid one (same count, another actions_root).
    SlotBody replaced{body};
    replaced.originals[0] = SmallPlace(a, "carol", 2);
    RequireFailure(r.pool->OnCommitted(slot.applied, head, outcomes, replaced), ErrorCode::INVALID_INPUT, admission::POOL_BODY_MISMATCH,
                   "original replaced");
    // A malformed original under a header that names it.
    SlotBody garbage{body};
    garbage.originals[0] = {0x01, 0x02};
    SlotHeaderV2 garbage_header{head.Header()};
    garbage_header.actions_root = garbage.ActionsRoot();
    RequireFailure(r.pool->OnCommitted(slot.applied, doctored(garbage_header), outcomes, garbage), ErrorCode::INVALID_INPUT,
                   admission::POOL_ORIGINAL_MALFORMED, "malformed original");
    // An original without an outcome, and an ACTION outcome without an original.
    SlotBody extra{body};
    extra.originals.push_back(SmallPlace(a, "carol", 1));
    SlotHeaderV2 extra_header{head.Header()};
    extra_header.actions_root = extra.ActionsRoot();
    extra_header.action_count = static_cast<uint32_t>(extra.originals.size());
    RequireFailure(r.pool->OnCommitted(slot.applied, doctored(extra_header), outcomes, extra), ErrorCode::INVALID_INPUT, admission::POOL_OUTCOME_MISSING,
                   "original without outcome");
    SlotBody missing{body};
    missing.originals.pop_back();
    SlotHeaderV2 missing_header{head.Header()};
    missing_header.actions_root = missing.ActionsRoot();
    missing_header.action_count = static_cast<uint32_t>(missing.originals.size());
    RequireFailure(r.pool->OnCommitted(slot.applied, doctored(missing_header), outcomes, missing), ErrorCode::INVALID_INPUT, admission::POOL_OUTCOME_MISSING,
                   "outcome without original");
    // Two ACTION outcomes for one action.
    std::vector<OutcomeV2> twice{outcomes};
    for (OutcomeV2& outcome : twice) {
        if (outcome.source == OutcomeSource::ACTION) outcome.subject = ids[0].id;
    }
    std::vector<uint256> leaves;
    for (const OutcomeV2& outcome : twice) leaves.push_back(outcome.LeafHash());
    SlotHeaderV2 twice_header{head.Header()};
    twice_header.outcomes_root = MerkleRoot(leaves);
    RequireFailure(r.pool->OnCommitted(slot.applied, doctored(twice_header), twice, body), ErrorCode::INVALID_INPUT, admission::POOL_OUTCOME_MISSING,
                   "two outcomes for one action");
    BOOST_CHECK(SnapshotOf(*r.pool, ids) == before);
    // The APPLIED token must be this head's (O-92): slot 1's token with slot 2's data is refused.
    RequireFailure(r.pool->OnCommitted(funded.slot.applied, head, outcomes, body), ErrorCode::VERIFY_FAILED,
                   admission::POOL_APPLIED_MISMATCH, "APPLIED token of slot 1");
    BOOST_CHECK(SnapshotOf(*r.pool, ids) == before);

    // The real data is accepted once; a repeat is not the successor.
    Result<admission::CommitReport> report{r.pool->OnCommitted(slot.applied, head, outcomes, body)};
    RequireOk(report, "OnCommitted");
    BOOST_CHECK_EQUAL(report->included, 1U);
    BOOST_CHECK_EQUAL(report->recorded, 2U);
    const PoolSnapshot after{SnapshotOf(*r.pool, ids)};
    RequireFailure(r.pool->OnCommitted(slot.applied, head, outcomes, body), ErrorCode::ORDER_VIOLATION, admission::POOL_NOT_SUCCESSOR, "repeat");
    BOOST_CHECK(SnapshotOf(*r.pool, ids) == after);
}

BOOST_AUTO_TEST_CASE(a_live_entry_without_an_account_is_an_invariant_failure)
{
    PoolRig r;
    r.Fund();
    // The pool never checks accounts (admission A4 does); an entry of an unfunded account is a broken invariant.
    const auto dave{SmallPlace(r.Actions(), "dave", 1)};
    BOOST_REQUIRE(r.Insert(dave).result == InsertResult::INSERTED);
    state::Candidate candidate{r.rig.Preview(r.rig.Inputs({}, {}))};
    const SlotBody body{candidate.Body()};
    PublishableSlot slot{r.rig.Commit(std::move(candidate))};
    const PoolSnapshot before{SnapshotOf(*r.pool, {IdOfBytes(dave)})};
    RequireFailure(r.pool->OnCommitted(slot.applied, *slot.slot.head, slot.slot.outcomes, body), ErrorCode::INVARIANT, admission::POOL_ACCOUNT_MISSING,
                   "unfunded account");
    BOOST_CHECK(SnapshotOf(*r.pool, {IdOfBytes(dave)}) == before);
}

BOOST_AUTO_TEST_CASE(retention_and_terminal_capacity)
{
    // max_delay_slots = retention = 2; at most one terminal record other than INCLUDED.
    const ProfileParams profile{ProfileWith(100, 2)};
    PoolRig r{OptionsFor(profile, PoolLimits{8, 2, 1})};
    r.Fund();
    SlotFixture& a{r.Actions()};
    const auto x{PlaceWindow(a, "alice", 1, 1, 2)};
    BOOST_REQUIRE(r.Insert(x).result == InsertResult::INSERTED);
    PoolRig::Sealed slot2{r.Seal()};
    BOOST_REQUIRE_EQUAL(slot2.report.included, 1U);
    BOOST_CHECK_EQUAL(r.pool->IncludedRecords(), 1U);
    // Kept while h - s < 2.
    PoolRig::Sealed slot3{r.Seal()};
    BOOST_CHECK_EQUAL(slot3.report.evicted, 0U);
    BOOST_CHECK(r.pool->FindAuthenticated(IdOfBytes(x), SigOf(x)).has_value());
    // Two losers in one slot: the oldest record beyond terminal_capacity is evicted.
    const auto l1{PlaceWindow(a, "bob", 1, 3, 2)};
    const auto l2{PlaceWindow(a, "carol", 1, 3, 2)};
    BOOST_REQUIRE(r.Insert(l1).result == InsertResult::INSERTED);
    BOOST_REQUIRE(r.Insert(l2).result == InsertResult::INSERTED);
    const auto w2{a.Sign(a.Unsigned("carol", 1, ActionKind::TRANSFER,
                                    EncodePayload(TransferPayload{KeyAccount("alice"), TestAsset("rUSD"), 1}).value(), 3, 2),
                         "carol")};
    const auto w1b{a.Sign(a.Unsigned("bob", 1, ActionKind::TRANSFER,
                                     EncodePayload(TransferPayload{KeyAccount("alice"), TestAsset("TOK"), 1}).value(), 3, 2),
                          "bob")};
    PoolRig::Sealed slot4{r.SealOriginals({}, {w1b, w2})};
    BOOST_CHECK_EQUAL(slot4.report.nonce_used, 2U);
    // h = 4, s = 2: the INCLUDED record of x is gone; one of the two NONCE_USED records is gone.
    BOOST_CHECK_EQUAL(slot4.report.evicted, 2U);
    BOOST_CHECK(!r.pool->FindAuthenticated(IdOfBytes(x), SigOf(x)).has_value());
    BOOST_CHECK(!r.pool->Status(IdOfBytes(x)).has_value());
    BOOST_CHECK_EQUAL(r.pool->IncludedRecords(), 2U); // w1b, w2
    BOOST_CHECK_EQUAL(r.pool->TerminalRecords(), 1U);
    BOOST_CHECK(!r.pool->Status(IdOfBytes(l1)).has_value()); // the first terminal record was the oldest
    BOOST_CHECK(RequireStatus(*r.pool, IdOfBytes(l2)).status == PoolStatus::NONCE_USED);
    // Retention also ends the remaining terminal record (s = 4 at h = 6).
    r.Seal();
    PoolRig::Sealed slot6{r.Seal()};
    BOOST_CHECK_EQUAL(slot6.report.evicted, 3U);
    BOOST_CHECK_EQUAL(r.pool->IncludedRecords(), 0U);
    BOOST_CHECK_EQUAL(r.pool->TerminalRecords(), 0U);
}

BOOST_AUTO_TEST_CASE(taken_slots_always_pass_the_validity_predicate)
{
    // A window of 3 and short delays make used nonces, nonces below the window and expiry common.
    // Seal runs every taken slot through StateOwner::Preview (the validity predicate) and
    // requires success; actions enter the pool only after the checks admission A4..A6 make.
    const ProfileParams profile{ProfileWith(/*nonce_window=*/3, /*max_delay_slots=*/4)};
    PoolRig r{OptionsFor(profile, PoolLimits{64, 4, 64})};
    r.Fund();
    SlotFixture& a{r.Actions()};
    const std::vector<std::string> names{"alice", "bob", "carol"};
    uint64_t state{0x5eed};
    const auto next = [&](uint64_t bound) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return (state >> 33) % bound;
    };
    struct Inserted {
        std::string name;
        uint64_t nonce;
        ActionId id;
    };
    std::vector<Inserted> inserted;
    admission::CommitReport totals;
    size_t foreign_wins{0};
    const auto add = [&](const admission::CommitReport& report) {
        totals.included += report.included;
        totals.nonce_used += report.nonce_used;
        totals.nonce_too_low += report.nonce_too_low;
        totals.expired += report.expired;
        totals.returned += report.returned;
    };
    for (int slot{0}; slot < 80; ++slot) {
        const uint64_t head{r.Height()};
        for (int k{0}; k < 6; ++k) {
            const std::string& name{names[next(names.size())]};
            const uint64_t nonce{head + next(8)};
            const uint64_t ref{head - std::min<uint64_t>(head, next(3))};
            const uint16_t delay{static_cast<uint16_t>(1 + next(4))};
            const ledger::AccountState* account{r.Head()->State().engine.ledger.Find(KeyAccount(name))};
            BOOST_REQUIRE(account != nullptr);
            if (!account->nonces.Acceptable(nonce) || !InExpiryWindow(ref, delay, head + 1)) continue;
            const auto bytes{PlaceWindow(a, name, nonce, ref, delay, 1 + next(5))};
            if (r.Insert(bytes).result == InsertResult::INSERTED) inserted.push_back(Inserted{name, nonce, IdOfBytes(bytes)});
        }
        const size_t live_before{r.pool->Size()};
        PoolRig::Sealed sealed{[&] {
            // Sometimes another seat's proposal wins with the nonce of a live entry.
            if (next(3) == 0) {
                std::vector<const Inserted*> live;
                for (const Inserted& entry : inserted) {
                    const auto status{r.pool->Status(entry.id)};
                    if (status && !IsTerminal(status->status)) live.push_back(&entry);
                }
                if (!live.empty()) {
                    const Inserted& victim{*live[next(live.size())]};
                    ++foreign_wins;
                    return r.SealOriginals({}, {PlaceWindow(a, victim.name, victim.nonce, head, 2, 9)});
                }
            }
            const bool withhold_some{next(3) == 0};
            return r.Seal({}, [&](const ActionV2& action) { return withhold_some && action.nonce % 2 == 0; },
                          static_cast<size_t>(1 + next(6)));
        }()};
        add(sealed.report);
        // No live entry is dropped silently: each one that left has exactly one terminal status.
        const size_t left{sealed.report.included + sealed.report.nonce_used + sealed.report.nonce_too_low + sealed.report.expired};
        BOOST_CHECK_EQUAL(r.pool->Size(), live_before - left);
    }
    BOOST_TEST_MESSAGE("included " << totals.included << ", nonce_used " << totals.nonce_used << ", nonce_too_low "
                                   << totals.nonce_too_low << ", expired " << totals.expired << ", returned " << totals.returned
                                   << ", foreign wins " << foreign_wins << ", inserted " << inserted.size());
    BOOST_CHECK(totals.included > 0);
    BOOST_CHECK(totals.nonce_used > 0);
    BOOST_CHECK(totals.nonce_too_low > 0);
    BOOST_CHECK(totals.expired > 0);
    BOOST_CHECK(totals.returned == 0); // Seal hands every taken entry to the slot it commits
}

BOOST_AUTO_TEST_CASE(concurrent_inserts_keep_capacity_and_uniqueness)
{
    PoolRig r{OptionsFor(RegtestProfile(), PoolLimits{100, 1'024, 100})};
    r.Fund();
    SlotFixture& a{r.Actions()};
    const std::vector<std::string> names{"alice", "bob", "carol", "dave"};
    std::vector<std::vector<VerifiedAction>> per_thread(names.size());
    for (size_t t{0}; t < names.size(); ++t) {
        for (uint64_t n{1}; n <= 64; ++n) per_thread[t].push_back(VerifiedOf(SmallPlace(a, names[t], n)));
    }
    std::atomic<size_t> inserted{0};
    std::atomic<size_t> full{0};
    std::atomic<size_t> other{0};
    std::vector<std::thread> threads;
    for (size_t t{0}; t < names.size(); ++t) {
        threads.emplace_back([&, t] {
            for (const VerifiedAction& action : per_thread[t]) {
                const admission::InsertReply reply{r.pool->Insert(action, 1)};
                if (reply.result == InsertResult::INSERTED) {
                    ++inserted;
                } else if (reply.result == InsertResult::POOL_FULL) {
                    ++full;
                } else {
                    ++other;
                }
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    BOOST_CHECK_EQUAL(inserted.load(), 100U);
    BOOST_CHECK_EQUAL(full.load(), 156U);
    BOOST_CHECK_EQUAL(other.load(), 0U);
    BOOST_CHECK_EQUAL(r.pool->Size(), 100U);

    // Four threads racing to insert the same 32 actions: each is inserted exactly once.
    PoolRig same{OptionsFor(RegtestProfile())};
    same.Fund();
    std::vector<VerifiedAction> shared;
    for (uint64_t n{1}; n <= 32; ++n) shared.push_back(VerifiedOf(SmallPlace(same.Actions(), "alice", n)));
    std::atomic<size_t> won{0};
    std::atomic<size_t> dup{0};
    threads.clear();
    for (size_t t{0}; t < 4; ++t) {
        threads.emplace_back([&] {
            for (const VerifiedAction& action : shared) {
                const admission::InsertReply reply{same.pool->Insert(action, 1)};
                if (reply.result == InsertResult::INSERTED) ++won;
                if (reply.result == InsertResult::DUPLICATE) ++dup;
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    BOOST_CHECK_EQUAL(won.load(), 32U);
    BOOST_CHECK_EQUAL(dup.load(), 96U);
    BOOST_CHECK_EQUAL(same.pool->Size(), 32U);
}

BOOST_AUTO_TEST_SUITE_END()
