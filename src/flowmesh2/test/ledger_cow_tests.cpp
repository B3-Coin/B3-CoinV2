// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/merkle.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/engine/engine.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/slot_fixtures.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;

namespace {

//! splitmix64.
struct Rng {
    uint64_t state;
    uint64_t Next()
    {
        uint64_t z{state += 0x9e3779b97f4a7c15ULL};
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    uint64_t Below(uint64_t n) { return n == 0 ? 0 : Next() % n; }
};

AccountId Trader(uint64_t n) { return TestAccount("cow/" + std::to_string(n)); }

//! The two components computed from every leaf, with no stored level.
std::pair<uint256, uint256> FromEveryLeaf(const ledger::LedgerState& ledger)
{
    std::vector<uint256> accounts;
    for (const auto& [id, state] : ledger.Accounts()) accounts.push_back(ledger.Leaf(id).value().LeafValue());
    std::vector<uint256> nullifiers;
    for (const Outpoint& outpoint : ledger.Nullifiers()) nullifiers.push_back(NullifierLeafValue(outpoint));
    return {MerkleRoot(accounts), MerkleRoot(nullifiers)};
}

//! One random ledger operation, the same on every ledger for the same `rng` state.
void RandomOperation(ledger::LedgerState& ledger, Rng& rng, uint64_t& outpoints)
{
    const AssetId asset{rng.Below(2) == 0 ? TestAsset("rUSD") : TestAsset("TOK")};
    const AccountId a{Trader(rng.Below(300))};
    const AccountId b{Trader(rng.Below(300))};
    switch (rng.Below(5)) {
    case 0:
    case 1: // mostly new accounts early, top-ups later
        (void)ledger.CreditDeposit(Deposit(a, asset, 1 + static_cast<Atoms>(rng.Below(10'000)), TestOutpoint("cow", static_cast<uint32_t>(outpoints++))));
        break;
    case 2:
        (void)ledger.Transfer(a, b, asset, 1 + static_cast<Atoms>(rng.Below(500)));
        break;
    case 3:
        (void)ledger.ConsumeNonce(a, 1 + rng.Below(8));
        break;
    default: { // a reservation and its release: no net change of the state, a cloned path
        const Atoms amount{1 + static_cast<Atoms>(rng.Below(50))};
        const Result<ledger::ReserveResult> reserved{ledger.Reserve(a, asset, amount)};
        if (reserved.has_value() && *reserved == ledger::ReserveResult::OK) RequireOk(ledger.Release(a, asset, amount), "release");
        break;
    }
    }
}

//! A copy of `state` with one change that breaks invariant 4, 5 or 2.
engine::EngineState Doctored(const engine::EngineState& state, uint64_t kind, const AccountId& account)
{
    engine::EngineState doctored{state};
    switch (kind % 3) {
    case 0:
        if (!doctored.ledger.Reserve(account, TestAsset("rUSD"), 1).has_value()) BOOST_FAIL("doctor: reserve");
        break;
    case 1:
        if (!doctored.ledger.AddOpenOrder(account).has_value()) BOOST_FAIL("doctor: open order");
        break;
    default:
        if (!doctored.ledger.Reserve(account, TestAsset("rUSD"), 2).has_value()) BOOST_FAIL("doctor: reserve");
        RequireOk(doctored.ledger.PayFromReserved(account, KeyAccount("carol"), TestAsset("rUSD"), 1, 1), "doctor: withhold a fee");
        break;
    }
    return doctored;
}

void RequireSameVerdict(const Result<void>& a, const Result<void>& b, std::string_view what)
{
    BOOST_TEST_CONTEXT(what)
    {
        BOOST_REQUIRE_EQUAL(a.has_value(), b.has_value());
        if (!a.has_value()) {
            BOOST_REQUIRE(a.error().code == b.error().code);
            BOOST_REQUIRE_EQUAL(a.error().detail, b.error().detail);
        }
    }
}

} // namespace

BOOST_AUTO_TEST_SUITE(ledger_cow_tests)

BOOST_AUTO_TEST_CASE(sealed_ledgers_give_the_roots_and_proofs_of_every_leaf)
{
    // One operation sequence on two ledgers: `sealed` stores its levels
    // every few operations, `plain` never does. Both must answer as the
    // components computed from every leaf, and copies taken on the way must
    // keep the roots they had.
    const ProfileParams profile{RegtestProfile()};
    ledger::LedgerState sealed{EmptyLedger(profile)};
    ledger::LedgerState plain{EmptyLedger(profile)};
    Rng rng_sealed{0xc0};
    Rng rng_plain{0xc0};
    uint64_t outpoints_sealed{0};
    uint64_t outpoints_plain{0};
    std::vector<std::pair<ledger::LedgerState, std::pair<uint256, uint256>>> copies;
    for (int op{1}; op <= 2'000; ++op) {
        RandomOperation(sealed, rng_sealed, outpoints_sealed);
        RandomOperation(plain, rng_plain, outpoints_plain);
        if (op % 7 == 0) RequireOk(sealed.Seal(), "seal");
        if (op % 97 == 0 || op == 2'000) {
            const std::pair<uint256, uint256> expected{FromEveryLeaf(plain)};
            BOOST_TEST_CONTEXT("operation " << op)
            {
                BOOST_REQUIRE(sealed == plain);
                BOOST_REQUIRE(sealed.AccountsRootV0().value() == expected.first);
                BOOST_REQUIRE(plain.AccountsRootV0().value() == expected.first);
                BOOST_REQUIRE(sealed.NullifiersRootV0() == expected.second);
                BOOST_REQUIRE(plain.NullifiersRootV0() == expected.second);
                const AccountId probe{Trader(rng_sealed.Below(300))};
                rng_plain.Next();
                if (sealed.HasAccount(probe)) {
                    const MerkleProof proof{sealed.ProveAccountLeafV0(probe).value()};
                    BOOST_REQUIRE(proof == plain.ProveAccountLeafV0(probe).value());
                    BOOST_REQUIRE(MerkleVerify(sealed.Leaf(probe).value().LeafValue(), proof, expected.first));
                } else {
                    BOOST_REQUIRE(!sealed.ProveAccountLeafV0(probe).has_value());
                }
            }
            copies.emplace_back(sealed, expected);
        }
    }
    BOOST_CHECK_GT(sealed.Accounts().size(), 200U);
    for (const auto& [copy, expected] : copies) {
        BOOST_REQUIRE(copy.AccountsRootV0().value() == expected.first);
        BOOST_REQUIRE(copy.NullifiersRootV0() == expected.second);
        BOOST_REQUIRE(FromEveryLeaf(copy) == expected);
    }
}

BOOST_AUTO_TEST_CASE(conservation_since_a_parent_gives_the_full_check_verdict)
{
    // No ledger operation breaks conservation, so every child of a conserved
    // parent passes both checks; the by-changes check must not refuse one.
    // Held fees and stray reservations are engine invariants, not ledger ones.
    const ProfileParams profile{RegtestProfile()};
    ledger::LedgerState parent{EmptyLedger(profile)};
    Rng rng{0xc05};
    uint64_t outpoints{0};
    for (int i{0}; i < 600; ++i) RandomOperation(parent, rng, outpoints);
    RequireOk(parent.CheckConservation(), "parent");
    RequireOk(parent.Seal(), "seal");
    std::vector<ledger::LedgerState> children;
    for (int round{0}; round < 200; ++round) {
        ledger::LedgerState child{parent};
        for (uint64_t i{rng.Below(20)}; i > 0; --i) RandomOperation(child, rng, outpoints);
        const AccountId account{Trader(rng.Below(300))};
        if (rng.Below(3) == 0 && child.HasAccount(account)) {
            const Result<ledger::ReserveResult> reserved{child.Reserve(account, TestAsset("rUSD"), 2)};
            if (reserved.has_value() && *reserved == ledger::ReserveResult::OK && rng.Below(2) == 0) {
                (void)child.PayFromReserved(account, Trader(0), TestAsset("rUSD"), 1, 1);
            }
        }
        RequireOk(child.CheckConservation(), "child, full check");
        RequireOk(child.CheckConservationSince(parent), "child, by changes");
        children.push_back(std::move(child));
    }
    // Any conserved ledger may serve as the parent of another that keeps its
    // accounts: the unchanged accounts cancel out of both equations.
    for (size_t i{0}; i + 1 < children.size(); i += 2) {
        const ledger::LedgerState& a{children[i]};
        const ledger::LedgerState& b{children[i + 1]};
        bool b_keeps_a{true};
        for (const auto& [id, state] : a.Accounts()) b_keeps_a = b_keeps_a && b.HasAccount(id);
        if (b_keeps_a) RequireOk(b.CheckConservationSince(a), "a sibling as the parent");
    }
    // A parent with an account the child lacks: the ledger removes none.
    RequireError(EmptyLedger(profile).CheckConservationSince(parent), ErrorCode::INVARIANT, "an account removed");
    RequireOk(parent.CheckConservationSince(EmptyLedger(profile)), "the empty ledger as the parent");
}

BOOST_AUTO_TEST_CASE(phase_nine_by_changes_gives_the_full_check_verdict)
{
    // States derived from a verified state by real slots and by doctored
    // ledger changes: Verify gives CheckInvariants' verdict, and a failed
    // Verify leaves the verified basis where it was.
    SlotFixture f;
    for (const char* name : {"alice", "bob", "carol"}) {
        RequireOk(f.state.ledger.CreditDeposit(Deposit(KeyAccount(name), TestAsset("rUSD"), 1'000'000, TestOutpoint(std::string{"verify/"} + name, 0))), "fund");
        RequireOk(f.state.ledger.CreditDeposit(Deposit(KeyAccount(name), TestAsset("TOK"), 1'000, TestOutpoint(std::string{"verify/TOK/"} + name, 0))), "fund");
    }
    const Result<StateRootsV0> funded{f.state.Commit()};
    RequireOk(funded, "commit");
    f.header = GenesisHeader(f.domain, AuthorityVersion{0, LabelHash("fm2/test/seatset"), f.profile_hash}, TestAnchor(f.anchor_height - 1),
                             FactCursor{f.anchor_height, 0}, funded->Root(), funded->version)
                   .value();
    // Resting orders by alice and bob, so the basis has owners.
    f.Apply(f.Inputs({}, {f.Place("alice", 1, MarketM2(), Side::BUY, TimeInForce::GTC, 90, 5),
                          f.Place("bob", 1, MarketM2(), Side::SELL, TimeInForce::GTC, 110, 5)}));
    BOOST_REQUIRE(f.state.verified != nullptr);
    BOOST_REQUIRE_EQUAL(f.state.verified->Owners().size(), 2U);

    Rng rng{0x9};
    for (int round{0}; round < 90; ++round) {
        const AccountId account{std::vector<AccountId>{KeyAccount("alice"), KeyAccount("bob"), KeyAccount("carol")}[rng.Below(3)]};
        engine::EngineState doctored{Doctored(f.state, rng.Next(), account)};
        const std::shared_ptr<const engine::VerifiedBasis> basis{doctored.verified};
        const Result<void> full{doctored.CheckInvariants()};
        BOOST_REQUIRE(!full.has_value());
        RequireSameVerdict(doctored.Verify(), full, "doctored round " + std::to_string(round));
        BOOST_REQUIRE(doctored.verified == basis);
    }

    // An order leaves the book with its reservation still held: the owner's
    // account is unchanged since the basis, and only the basis's owner list
    // names it.
    engine::EngineState removed{f.state};
    const engine::RestingOrder order{removed.books.FindMutable(MarketM2().Id())->Orders().front()};
    BOOST_REQUIRE(removed.books.FindMutable(MarketM2().Id())->Remove(order.id).has_value());
    BOOST_REQUIRE_GT(removed.ledger.Find(order.owner)->open_orders, 0U);
    RequireSameVerdict(removed.Verify(), removed.CheckInvariants(), "an order removed without its reservation");
    BOOST_CHECK(!removed.CheckInvariants().has_value());

    // Valid changes: a deposit, a transfer and a reservation that is released.
    engine::EngineState valid{f.state};
    RequireOk(valid.ledger.CreditDeposit(Deposit(KeyAccount("dave"), TestAsset("rUSD"), 5, TestOutpoint("verify/dave", 0))), "deposit");
    BOOST_REQUIRE(valid.ledger.Transfer(KeyAccount("carol"), KeyAccount("dave"), TestAsset("rUSD"), 5).has_value());
    BOOST_REQUIRE(valid.ledger.Reserve(KeyAccount("carol"), TestAsset("rUSD"), 3).value() == ledger::ReserveResult::OK);
    RequireOk(valid.ledger.Release(KeyAccount("carol"), TestAsset("rUSD"), 3), "release");
    RequireOk(valid.CheckInvariants(), "valid changes, full check");
    const std::shared_ptr<const engine::VerifiedBasis> before{valid.verified};
    RequireOk(valid.Verify(), "valid changes, by changes");
    BOOST_CHECK(valid.verified != before);
    // Two steps from the same basis: a stray reservation made before a
    // Verify that failed is still caught by the next one.
    engine::EngineState two{Doctored(f.state, 0, KeyAccount("carol"))};
    BOOST_REQUIRE(!two.Verify().has_value());
    RequireOk(two.ledger.CreditDeposit(Deposit(KeyAccount("erin"), TestAsset("rUSD"), 5, TestOutpoint("verify/erin", 0))), "deposit");
    RequireSameVerdict(two.Verify(), two.CheckInvariants(), "a break before a failed Verify");
    BOOST_CHECK(!two.CheckInvariants().has_value());
}

BOOST_AUTO_TEST_CASE(the_verified_basis_is_never_part_of_the_value)
{
    SlotFixture f;
    engine::EngineState copy{f.state};
    copy.verified.reset();
    BOOST_CHECK(copy == f.state);
    RequireOk(copy.Verify(), "full check");
    BOOST_CHECK(copy.verified != nullptr);
    BOOST_CHECK(copy == f.state);
}

BOOST_AUTO_TEST_SUITE_END()
