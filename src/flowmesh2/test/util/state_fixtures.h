// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_TEST_UTIL_STATE_FIXTURES_H
#define B3_FLOWMESH2_TEST_UTIL_STATE_FIXTURES_H

#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/engine/engine.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/state/state_owner.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/durable.h>
#include <flowmesh2/storage/memory_journal.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/slot_fixtures.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** TEST fixtures of the state owner suites (slice R9). Synthetic values only:
 * the TEST network and keys of test/util/slot_fixtures.h, the seat set root
 * LabelHash("fm2/test/seatset") and label anchors. The genesis head equals
 * the R7 SlotFixture's genesis, so engine-level and state-level results can
 * be compared directly.
 */
namespace flowmesh2::test {

inline uint256 TestSeatSetRoot() { return LabelHash("fm2/test/seatset"); }

//! The authority of the state tests at `epoch`.
inline AuthorityVersion TestAuthority(const ProfileParams& profile, uint64_t epoch = 0)
{
    return AuthorityVersion{epoch, TestSeatSetRoot(), ProfileHash(profile)};
}

//! The genesis head of the state tests (the SlotFixture genesis: anchor 129, facts from (130, 0)).
inline state::HeadView TestGenesisHead(const ProfileParams& profile = RegtestProfile(),
                                       const std::vector<MarketDescriptor>& markets = {MarketM1(), MarketM2()})
{
    Result<state::HeadView> head{state::GenesisHead(profile, markets, TestDomain(), TestAuthority(profile), TestAnchor(129),
                                                    FactCursor{130, 0})};
    RequireOk(head, "GenesisHead");
    return std::move(head).value();
}

//! Requires an error with exactly `code` and `detail`, reading error() only on failure.
template <class T>
void RequireFailure(const Result<T>& result, ErrorCode code, std::string_view detail, std::string_view what)
{
    if (result.has_value()) BOOST_FAIL(std::string{what} + " unexpectedly succeeded");
    BOOST_CHECK_MESSAGE(result.error().code == code && result.error().detail == detail,
                        std::string{what} + ": got " + std::string{ToString(result.error().code)} + " '" +
                            std::string{result.error().detail} + "', want " + std::string{ToString(code)} + " '" +
                            std::string{detail} + "'");
}

/** Deposits for alice (rUSD), bob (B3 and TOK) and carol (rUSD), as in the
 * walking skeleton plus carol, at the next cursors of `actions`.
 */
inline std::vector<std::vector<unsigned char>> FundingFacts(SlotFixture& actions)
{
    return {actions.DepositBytes(KeyAccount("alice"), TestAsset("rUSD"), 1'000'000'000, "fund/alice"),
            actions.DepositBytes(KeyAccount("bob"), AssetId::Native(), 10'000'000'000, "fund/bob/b3"),
            actions.DepositBytes(KeyAccount("bob"), TestAsset("TOK"), 500'000, "fund/bob/tok"),
            actions.DepositBytes(KeyAccount("carol"), TestAsset("rUSD"), 1'000'000'000, "fund/carol")};
}

/** A head the tests advance by hand with BuildCandidate (no state owner):
 * `actions` signs actions and builds deposit bytes, and its header and
 * state follow `head`, so ref_slot defaults to the head height.
 */
struct CandidateFixture {
    SlotFixture actions;
    engine::Engine engine;
    AuthorityVersion authority;
    state::HeadView head;

    explicit CandidateFixture(const ProfileParams& profile = RegtestProfile())
        : actions{profile},
          engine{engine::Engine::Create(profile).value()},
          authority{TestAuthority(profile)},
          head{TestGenesisHead(profile)}
    {
        BOOST_REQUIRE(head.Header() == actions.header);
    }

    //! Inputs that follow the head: facts_from = its facts_to, facts_to = the next deposit cursor.
    state::SlotInputs Inputs(std::vector<std::vector<unsigned char>> facts, std::vector<std::vector<unsigned char>> originals) const
    {
        state::SlotInputs inputs;
        inputs.authority = authority;
        inputs.anchor = TestAnchor(actions.anchor_height);
        inputs.facts_from = head.Header().facts_to;
        inputs.facts_to = FactCursor{actions.anchor_height, actions.next_fact_index};
        inputs.body.facts = std::move(facts);
        inputs.body.originals = std::move(originals);
        return inputs;
    }

    Result<state::Candidate> TryBuild(const state::SlotInputs& inputs) const { return state::BuildCandidate(engine, head, inputs); }

    state::Candidate Build(const state::SlotInputs& inputs) const
    {
        Result<state::Candidate> candidate{TryBuild(inputs)};
        RequireOk(candidate, "BuildCandidate");
        return std::move(candidate).value();
    }

    //! Make `candidate` the head, as decided by a certificate with hash `cert_hash`.
    void Advance(const state::Candidate& candidate, const uint256& cert_hash)
    {
        Result<state::HeadView> next{state::HeadView::Make(candidate.Header(), cert_hash, candidate.Next())};
        RequireOk(next, "HeadView::Make");
        head = std::move(next).value();
        actions.header = head.Header();
        actions.state = head.State().engine;
    }

    std::vector<std::vector<unsigned char>> FundingFacts() { return test::FundingFacts(actions); }
};

/** A certificate naming `header` (seat 0). The aggregate is not a signature:
 * neither the commit log nor the state owner verifies it (agreement and
 * b3verify do, R10/R11). `variant` changes the aggregate, so one header can
 * have several certificates, as a BLS quorum certificate can.
 */
inline SlotCertificate TestCertificate(const SlotHeaderV2& header, uint8_t variant = 0)
{
    SlotCertificate cert;
    cert.header_hash = header.Hash();
    cert.bitmap = {0x01};
    cert.aggregate.fill(variant);
    return cert;
}

//! A COMMIT MemoryJournal, failing the test if Create fails.
inline std::unique_ptr<storage::MemoryJournal> NewCommitJournal(uint64_t instance, storage::MemoryJournalFaults faults = {})
{
    Result<std::unique_ptr<storage::MemoryJournal>> journal{storage::MemoryJournal::Create(storage::JournalRole::COMMIT, instance, faults)};
    RequireOk(journal, "MemoryJournal::Create");
    return std::move(journal).value();
}

/** A StateOwner over the test genesis with its own commit journal and log.
 * `actions` signs actions and builds deposit bytes; Sync() makes its header
 * and state follow the owner's head (ref_slot defaults to the head height).
 * Commit() runs one slot the way the sealing path will (R10): DECISION,
 * B-DECISION, Confirm, Apply, APPLIED, B-APPLIED, Confirm.
 */
struct OwnerFixture {
    SlotFixture actions;
    AuthorityVersion authority;
    state::HeadView genesis;
    uint64_t journal_instance;
    std::unique_ptr<storage::MemoryJournal> journal;
    storage::CommitLog log;
    std::unique_ptr<state::StateOwner> owner;
    //! When set, Commit calls it after B-DECISION and before Apply (journal fault and replay tests).
    std::function<void(const state::Candidate&)> after_decision;

    explicit OwnerFixture(uint64_t journal_instance_in = 1, const state::HeadView& genesis_in = TestGenesisHead())
        : authority{TestAuthority(RegtestProfile())},
          genesis{genesis_in},
          journal_instance{journal_instance_in},
          journal{NewCommitJournal(journal_instance_in)},
          log{storage::CommitLog::Create(*journal).value()},
          owner{state::StateOwner::Create(RegtestProfile(), genesis_in, journal_instance_in).value()}
    {
        Sync();
    }

    std::shared_ptr<const state::HeadView> Head() const { return owner->Head(); }
    state::ParentRef Parent() const { return state::ParentRef::Committed(*owner->Head()); }

    void Sync()
    {
        const std::shared_ptr<const state::HeadView> head{owner->Head()};
        actions.header = head->Header();
        actions.state = head->State().engine;
    }

    //! Inputs that follow the head: facts_from = its facts_to, facts_to = the next deposit cursor.
    state::SlotInputs Inputs(std::vector<std::vector<unsigned char>> facts, std::vector<std::vector<unsigned char>> originals) const
    {
        const std::shared_ptr<const state::HeadView> head{owner->Head()};
        state::SlotInputs inputs;
        inputs.authority = authority;
        inputs.anchor = TestAnchor(actions.anchor_height);
        inputs.facts_from = head->Header().facts_to;
        inputs.facts_to = FactCursor{actions.anchor_height, actions.next_fact_index};
        inputs.body.facts = std::move(facts);
        inputs.body.originals = std::move(originals);
        return inputs;
    }

    Result<state::Candidate> TryPreview(const state::SlotInputs& inputs) const { return owner->Preview(Parent(), inputs); }

    state::Candidate Preview(const state::SlotInputs& inputs) const
    {
        Result<state::Candidate> candidate{TryPreview(inputs)};
        RequireOk(candidate, "Preview");
        return std::move(candidate).value();
    }

    //! B-DECISION for `candidate` in this fixture's commit log, with TestCertificate(header, variant).
    DurableDecision Decide(const state::Candidate& candidate, uint8_t variant = 0)
    {
        const Result<storage::PendingDecision> pending{
            log.AppendDecision(candidate.Header(), TestCertificate(candidate.Header(), variant), candidate.Body())};
        RequireOk(pending, "AppendDecision");
        RequireOk(log.Barrier(), "B-DECISION");
        Result<DurableDecision> decision{log.Confirm(*pending)};
        RequireOk(decision, "Confirm DECISION");
        return std::move(decision).value();
    }

    //! Decide, Apply under `authority`, then B-APPLIED; returns the new head.
    std::shared_ptr<const state::HeadView> Commit(state::Candidate candidate, uint8_t variant = 0)
    {
        const DurableDecision decision{Decide(candidate, variant)};
        if (after_decision) after_decision(candidate);
        Result<std::shared_ptr<const state::HeadView>> head{owner->Apply(std::move(candidate), decision, authority)};
        RequireOk(head, "Apply");
        const Result<storage::PendingApplied> applied{log.AppendApplied(decision, (*head)->Header().state_root)};
        RequireOk(applied, "AppendApplied");
        RequireOk(log.Barrier(), "B-APPLIED");
        RequireOk(log.Confirm(*applied), "Confirm APPLIED");
        Sync();
        return *head;
    }

    //! Preview `inputs` and Commit the candidate.
    std::shared_ptr<const state::HeadView> Run(const state::SlotInputs& inputs, uint8_t variant = 0)
    {
        return Commit(Preview(inputs), variant);
    }
};

//! The heads of the walking-skeleton slots 1-5 and the two resting orders of slot 2.
struct SkeletonRun {
    std::vector<std::shared_ptr<const state::HeadView>> heads;
    OrderId order_a; //!< Bob SELL M1 GTC 5,000 lots at 250
    OrderId order_b; //!< Alice BUY M2 GTC 100,000 lots at 600
};

/** ARCHITECTURE.md section 14.2, slots 1-5, through the state owner (w, which
 * the simulator withholds, is not proposed): deposits D1-D3; a and b rest;
 * c fills 2,000 lots of a and d is REJ_INSUFFICIENT_AVAILABLE; e transfers
 * and f cancels b; slot 5 is a heartbeat.
 */
inline SkeletonRun RunWalkingSkeleton(OwnerFixture& f)
{
    SlotFixture& a{f.actions};
    const AssetId rusd{TestAsset("rUSD")};
    SkeletonRun run;
    const auto d1{a.DepositBytes(KeyAccount("alice"), rusd, 1'000'000'000, "T1")};
    const auto d2{a.DepositBytes(KeyAccount("bob"), AssetId::Native(), 10'000'000'000, "T2")};
    const auto d3{a.DepositBytes(KeyAccount("bob"), TestAsset("TOK"), 500'000, "T3")};
    run.heads.push_back(f.Run(f.Inputs({d1, d2, d3}, {})));
    const auto order_a{a.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 250, 5'000)};
    const auto order_b{a.Place("alice", 1, MarketM2(), Side::BUY, TimeInForce::GTC, 600, 100'000)};
    run.order_a = a.IdOf(order_a);
    run.order_b = a.IdOf(order_b);
    run.heads.push_back(f.Run(f.Inputs({}, {order_a, order_b})));
    const auto c{a.Place("alice", 2, MarketM1(), Side::BUY, TimeInForce::IOC, 260, 2'000)};
    const auto d{a.Place("alice", 3, MarketM2(), Side::BUY, TimeInForce::GTC, 600, 100'000)};
    run.heads.push_back(f.Run(f.Inputs({}, {c, d})));
    const auto e{a.Transfer("bob", 2, KeyAccount("alice"), rusd, 100'000)};
    const auto cancel_b{a.Cancel("alice", 4, run.order_b)};
    run.heads.push_back(f.Run(f.Inputs({}, {e, cancel_b})));
    run.heads.push_back(f.Run(f.Inputs({}, {})));
    return run;
}

//! ARCHITECTURE.md section 14.3: the final state of the walking skeleton.
inline void CheckWalkingSkeletonFinalState(const state::CoreState& state, const SkeletonRun& run)
{
    const ledger::LedgerState& ledger{state.engine.ledger};
    const AssetId rusd{TestAsset("rUSD")};
    BOOST_CHECK(ledger.BalanceOf(KeyAccount("alice"), rusd) == (ledger::Balance{999'600'000, 0}));
    BOOST_CHECK(ledger.BalanceOf(KeyAccount("alice"), AssetId::Native()) == (ledger::Balance{2'000'000'000, 0}));
    BOOST_CHECK(ledger.BalanceOf(KeyAccount("bob"), AssetId::Native()) == (ledger::Balance{5'000'000'000, 3'000'000'000}));
    BOOST_CHECK(ledger.BalanceOf(KeyAccount("bob"), rusd) == (ledger::Balance{399'950, 0}));
    BOOST_CHECK(ledger.BalanceOf(KeyAccount("bob"), TestAsset("TOK")) == (ledger::Balance{500'000, 0}));
    BOOST_CHECK(ledger.BalanceOf(ledger.SeatPool(), rusd) == (ledger::Balance{40, 0}));
    BOOST_CHECK(ledger.BalanceOf(ledger.Treasury(), rusd) == (ledger::Balance{10, 0}));
    const ledger::AccountState* alice{ledger.Find(KeyAccount("alice"))};
    const ledger::AccountState* bob{ledger.Find(KeyAccount("bob"))};
    BOOST_REQUIRE(alice != nullptr && bob != nullptr);
    BOOST_CHECK((std::vector<uint64_t>{alice->nonces.Used().begin(), alice->nonces.Used().end()}) == (std::vector<uint64_t>{1, 2, 3, 4}));
    BOOST_CHECK((std::vector<uint64_t>{bob->nonces.Used().begin(), bob->nonces.Used().end()}) == (std::vector<uint64_t>{1, 2}));
    const std::vector<RestingOrderLeafV0> orders{state.engine.OrderLeaves()};
    BOOST_REQUIRE_EQUAL(orders.size(), 1U);
    BOOST_CHECK(orders[0].order == run.order_a);
    BOOST_CHECK(orders[0].market == MarketM1().Id());
    BOOST_CHECK(orders[0].side == Side::SELL);
    BOOST_CHECK_EQUAL(orders[0].price_ticks, 250U);
    BOOST_CHECK_EQUAL(orders[0].remaining_lots, 3'000U);
    RequireOk(state.engine.CheckInvariants(), "slot-boundary invariants");
}

} // namespace flowmesh2::test

#endif // B3_FLOWMESH2_TEST_UTIL_STATE_FIXTURES_H
