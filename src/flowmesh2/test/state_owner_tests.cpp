// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/engine/engine.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/state/state_owner.h>
#include <flowmesh2/agreement/pbft.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/journal.h>
#include <flowmesh2/test/util/state_fixtures.h>
#include <flowmesh2/test/util/verify_fixtures.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;

namespace {

using Originals = std::vector<std::vector<unsigned char>>;

//! Slot 1 of the walking skeleton plus carol: every test account funded.
void Fund(OwnerFixture& f) { (void)f.Run(f.Inputs(FundingFacts(f.actions), {})); }

//! Requires identical bytes for every part of two candidates.
void RequireSameCandidate(const state::Candidate& a, const state::Candidate& b)
{
    BOOST_CHECK(a.Header().Encode() == b.Header().Encode());
    BOOST_REQUIRE_EQUAL(a.Outcomes().size(), b.Outcomes().size());
    for (size_t i{0}; i < a.Outcomes().size(); ++i) BOOST_CHECK(a.Outcomes()[i].Encode() == b.Outcomes()[i].Encode());
    BOOST_CHECK(a.OutcomeLeaves() == b.OutcomeLeaves());
    BOOST_CHECK(a.ActionLeaves() == b.ActionLeaves());
    BOOST_CHECK(a.Body().Encode() == b.Body().Encode());
    BOOST_CHECK(*a.Next() == *b.Next());
}

//! One bit of `root` flipped.
uint256 Flipped(uint256 root)
{
    *root.begin() ^= 0x01;
    return root;
}

//! A heartbeat (no facts, no originals) after `head`, under `authority`.
state::SlotInputs Heartbeat(const state::HeadView& head, const AuthorityVersion& authority)
{
    state::SlotInputs inputs;
    inputs.authority = authority;
    inputs.anchor = head.Header().anchor;
    inputs.facts_from = head.Header().facts_to;
    inputs.facts_to = head.Header().facts_to;
    return inputs;
}

//! A genesis head whose state was changed after CoreState::Genesis by `doctor`.
state::HeadView DoctoredGenesis(const std::function<void(ledger::LedgerState&)>& doctor)
{
    const ProfileParams profile{RegtestProfile()};
    const std::vector<MarketDescriptor> markets{MarketM1(), MarketM2()};
    state::CoreState state{state::CoreState::Genesis(profile, markets, TestSeatSetRoot()).value()};
    doctor(state.engine.ledger);
    const SlotHeaderV2 header{
        GenesisHeader(TestDomain(), TestAuthority(profile), TestAnchor(129), FactCursor{130, 0}, state.engine.Commit().value().Root()).value()};
    return state::HeadView::Make(header, uint256{}, std::make_shared<const state::CoreState>(std::move(state))).value();
}

//! Real four-seat BLS history; no fake aggregate is accepted by restoration.
struct CertifiedHistory {
    const std::vector<std::string> names{"restart0", "restart1", "restart2", "restart3"};
    SeatSet seats{TestSeats(names)};
    ProfileParams profile{RegtestProfile()};
    AuthorityVersion authority{0, seats.Root(), ProfileHash(profile)};
    state::HeadView genesis{state::GenesisHead(profile, std::vector<MarketDescriptor>{MarketM1(), MarketM2()},
                                              TestDomain(), authority, TestAnchor(129), FactCursor{130, 0}).value()};
    std::unique_ptr<storage::MemoryJournal> journal{NewCommitJournal(701)};
    storage::CommitLog log{storage::CommitLog::Create(*journal).value()};
    std::unique_ptr<state::StateOwner> live{state::StateOwner::Create(profile, genesis, journal->InstanceId()).value()};

    void Next(bool applied)
    {
        state::Candidate candidate{live->Preview(state::ParentRef::Committed(*live->Head()), Heartbeat(*live->Head(), authority)).value()};
        const SlotCertificate cert{SignCertificate(candidate.Header(), names, {0, 1, 2})};
        const auto pending{log.AppendDecision(candidate.Header(), cert, candidate.Body()).value()};
        RequireOk(log.Barrier(), "certified decision barrier");
        const auto token{log.Confirm(pending).value()};
        if (!applied) return;
        const auto installed{live->Apply(std::move(candidate), token, authority).value()};
        RequireOk(log.AppendApplied(installed.receipt), "certified application");
        RequireOk(log.Barrier(), "certified application barrier");
    }

    std::unique_ptr<state::StateOwner> Fresh() const
    {
        return state::StateOwner::Create(profile, genesis, journal->InstanceId()).value();
    }
};

//! Recompute only the public integrity chain, allowing tests to show that
//! cryptographic certificate/authority checks reject internally consistent bytes.
void Rechain(std::vector<storage::JournalRecord>& records, uint64_t instance)
{
    uint256 previous{storage::JournalGenesisChain(storage::JournalRole::COMMIT, instance)};
    for (auto& record : records) {
        previous = storage::NextChain(previous, record.type, record.payload);
        record.chain = previous;
    }
}

} // namespace

BOOST_AUTO_TEST_SUITE(state_owner_tests)

BOOST_AUTO_TEST_CASE(create_takes_a_genesis_head_of_its_profile)
{
    const ProfileParams profile{RegtestProfile()};
    const state::HeadView genesis{TestGenesisHead()};
    const auto owner{state::StateOwner::Create(profile, genesis, 1).value()};
    BOOST_CHECK(owner->Head()->Header() == genesis.Header());
    BOOST_CHECK(owner->Head()->CertHash().IsNull());
    BOOST_CHECK(owner->Head()->State() == genesis.State());
    BOOST_CHECK(owner->Profile() == profile);
    BOOST_CHECK_EQUAL(owner->CommitJournalInstance(), 1U);
    BOOST_CHECK(owner->Head() == owner->Head()); // one snapshot until Apply

    // Engine::Create runs first: a non-REGTEST profile.
    ProfileParams main{profile};
    main.chain = ChainType::MAIN;
    RequireError(state::StateOwner::Create(main, genesis, 1), ErrorCode::UNSUPPORTED_NETWORK, "MAIN profile");
    // Another (valid) profile than the head's.
    ProfileParams other{profile};
    other.fee_ppm = 101;
    RequireFailure(state::StateOwner::Create(other, genesis, 1), ErrorCode::INVALID_INPUT, state::OWNER_PROFILE_MISMATCH,
                   "head of another profile");
    // Not a genesis head: a certified slot, or a genesis header with a certificate hash.
    CandidateFixture fx;
    const state::Candidate c1{fx.Build(fx.Inputs(fx.FundingFacts(), {}))};
    const state::HeadView slot1{state::HeadView::Make(c1.Header(), LabelHash("fm2/test/cert/1"), c1.Next()).value()};
    RequireFailure(state::StateOwner::Create(profile, slot1, 1), ErrorCode::INVALID_INPUT, state::OWNER_NOT_GENESIS, "height 1");
    const state::HeadView certified{state::HeadView::Make(genesis.Header(), LabelHash("fm2/test/cert/0"), genesis.StatePtr()).value()};
    RequireFailure(state::StateOwner::Create(profile, certified, 1), ErrorCode::INVALID_INPUT, state::OWNER_NOT_GENESIS,
                   "genesis with a certificate hash");
    // Each genesis condition on its own: height, parent hash, parent certificate hash.
    for (const auto& doctor : std::vector<std::function<void(SlotHeaderV2&)>>{
             [](SlotHeaderV2& h) { h.height = 7; },
             [](SlotHeaderV2& h) { h.parent_hash = LabelHash("fm2/test/parent"); },
             [](SlotHeaderV2& h) { h.parent_cert_hash = LabelHash("fm2/test/parent-cert"); }}) {
        SlotHeaderV2 header{genesis.Header()};
        doctor(header);
        RequireFailure(state::StateOwner::Create(profile, state::HeadView::Make(header, uint256{}, genesis.StatePtr()).value(), 1),
                       ErrorCode::INVALID_INPUT, state::OWNER_NOT_GENESIS, "one genesis field changed");
    }
    // A header naming another seat set than the state, or another state root.
    const StateRootsV0 roots{genesis.State().engine.Commit().value()};
    AuthorityVersion other_seats{TestAuthority(profile)};
    other_seats.seat_set_root = LabelHash("fm2/test/other-seats");
    const SlotHeaderV2 seats_header{GenesisHeader(TestDomain(), other_seats, TestAnchor(129), FactCursor{130, 0}, roots.Root()).value()};
    RequireFailure(state::StateOwner::Create(profile, state::HeadView::Make(seats_header, uint256{}, genesis.StatePtr()).value(), 1),
                   ErrorCode::INVALID_INPUT, state::OWNER_SEAT_SET_MISMATCH, "another seat set");
    const SlotHeaderV2 root_header{
        GenesisHeader(TestDomain(), TestAuthority(profile), TestAnchor(129), FactCursor{130, 0}, LabelHash("fm2/test/other-root")).value()};
    RequireFailure(state::StateOwner::Create(profile, state::HeadView::Make(root_header, uint256{}, genesis.StatePtr()).value(), 1),
                   ErrorCode::VERIFY_FAILED, state::OWNER_STATE_ROOT_MISMATCH, "another state root");
}

BOOST_AUTO_TEST_CASE(preview_leaves_the_head_unchanged)
{
    OwnerFixture f;
    Fund(f);
    const std::shared_ptr<const state::HeadView> before{f.Head()};
    const state::CoreState state_before{before->State()};
    const state::Candidate c{f.Preview(f.Inputs({}, {f.actions.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 240, 10),
                                                    f.actions.Transfer("bob", 1, KeyAccount("carol"), AssetId::Native(), 5)}))};
    BOOST_CHECK(c.Next()->engine.ledger.BalanceOf(KeyAccount("alice"), TestAsset("rUSD")).reserved == 2'400);
    // The same head object, with the same state, before and after.
    BOOST_CHECK(f.Head() == before);
    BOOST_CHECK(before->State() == state_before);
    BOOST_CHECK(before->State().engine.ledger.BalanceOf(KeyAccount("alice"), TestAsset("rUSD")).reserved == 0);
    BOOST_CHECK(c.Next() != before->StatePtr());
    // Refused previews leave it unchanged as well.
    state::SlotInputs bad{f.Inputs({}, {})};
    bad.anchor = TestAnchor(10);
    RequireFailure(f.TryPreview(bad), ErrorCode::INVALID_INPUT, engine::SLOT_ANCHOR_BELOW_PARENT, "anchor below the parent's");
    BOOST_CHECK(f.Head() == before);
    BOOST_CHECK(before->State() == state_before);
}

BOOST_AUTO_TEST_CASE(identical_inputs_give_byte_identical_candidates)
{
    OwnerFixture f;
    Fund(f);
    const state::SlotInputs inputs{f.Inputs({}, {f.actions.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 250, 50),
                                                 f.actions.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::IOC, 260, 20),
                                                 f.actions.Place("carol", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 255, 40)})};
    const state::Candidate a{f.Preview(inputs)};
    const state::Candidate b{f.Preview(inputs)};
    RequireSameCandidate(a, b);
    BOOST_CHECK(a.Next() != b.Next()); // two independent states with equal content
    // A second owner on the same history gives the same bytes.
    OwnerFixture g{7};
    Fund(g);
    BOOST_REQUIRE(g.Head()->Header() == f.Head()->Header());
    RequireSameCandidate(a, g.Preview(inputs));
}

BOOST_AUTO_TEST_CASE(permuted_originals_give_the_same_header)
{
    OwnerFixture f;
    Fund(f);
    SlotFixture& a{f.actions};
    const Originals originals{a.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 250, 5'000),
                              a.Place("alice", 1, MarketM2(), Side::BUY, TimeInForce::GTC, 600, 100'000),
                              a.Place("alice", 2, MarketM1(), Side::BUY, TimeInForce::IOC, 260, 2'000),
                              a.Transfer("carol", 1, KeyAccount("bob"), TestAsset("rUSD"), 3),
                              a.Place("carol", 2, MarketM1(), Side::BUY, TimeInForce::GTC, 240, 1)};
    const state::Candidate reference{f.Preview(f.Inputs({}, originals))};
    const std::vector<std::vector<size_t>> permutations{{4, 3, 2, 1, 0}, {1, 0, 3, 2, 4}, {2, 4, 0, 3, 1}, {3, 1, 4, 0, 2}};
    for (const auto& permutation : permutations) {
        Originals permuted;
        for (const size_t index : permutation) permuted.push_back(originals[index]);
        const state::Candidate other{f.Preview(f.Inputs({}, permuted))};
        BOOST_CHECK(other.Header() == reference.Header());
        RequireSameCandidate(other, reference);
    }
}

BOOST_AUTO_TEST_CASE(a_wrong_parent_gives_stale_parent)
{
    OwnerFixture f;
    const state::SlotInputs inputs{f.Inputs({}, {})};
    const state::ParentRef head{f.Parent()};
    state::ParentRef other_hash{head};
    other_hash.header_hash = LabelHash("fm2/test/other-parent");
    RequireFailure(f.owner->Preview(other_hash, inputs), ErrorCode::STALE_PARENT, state::PARENT_NOT_HEAD, "another hash");
    state::ParentRef other_height{head};
    other_height.height = 1;
    RequireFailure(f.owner->Preview(other_height, inputs), ErrorCode::STALE_PARENT, state::PARENT_NOT_HEAD, "another height");
    state::ParentRef unknown_kind{head};
    unknown_kind.kind = static_cast<state::ParentKind>(9);
    RequireFailure(f.owner->Preview(unknown_kind, inputs), ErrorCode::INVALID_INPUT, state::PARENT_KIND_UNKNOWN, "unknown kind");
    // Building on an uncommitted candidate (pipelining) is the stub M4-AGR-10.
    const state::Candidate c{f.Preview(inputs)};
    RequireStub(f.owner->Preview(state::ParentRef::Uncommitted(c), Heartbeat(*f.Head(), f.authority)), "M4-AGR-10");
    // Once the head moves, the old head is a stale parent.
    (void)f.Commit(c);
    RequireFailure(f.owner->Preview(head, inputs), ErrorCode::STALE_PARENT, state::PARENT_NOT_HEAD, "previous head");
}

BOOST_AUTO_TEST_CASE(a_token_for_another_header_gives_verify_failed)
{
    OwnerFixture f{3};
    Fund(f);
    const std::shared_ptr<const state::HeadView> head{f.Head()};
    const state::Candidate c{f.Preview(f.Inputs({}, {f.actions.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 240, 10)}))};
    const state::Candidate other{f.Preview(f.Inputs({}, {f.actions.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 300, 10)}))};
    BOOST_REQUIRE(c.Header().Hash() != other.Header().Hash());
    const DurableDecision decision{f.Decide(c)};
    state::Candidate other_copy{other};
    RequireFailure(f.owner->Apply(std::move(other_copy), decision, f.authority), ErrorCode::VERIFY_FAILED, state::DECISION_OTHER_HEADER,
                   "decision for another header");
    BOOST_CHECK(f.Head() == head);
    // A token of another commit journal, even for this very header.
    OwnerFixture g{4};
    Fund(g);
    BOOST_REQUIRE(g.Head()->HeaderHash() == head->HeaderHash());
    const DurableDecision foreign{g.Decide(c)};
    state::Candidate c_copy{c};
    RequireFailure(f.owner->Apply(std::move(c_copy), foreign, f.authority), ErrorCode::VERIFY_FAILED, storage::TOKEN_OTHER_LOG,
                   "token of another commit journal");
    BOOST_CHECK(f.Head() == head);
    // The candidate and its own decision still apply.
    state::Candidate c_again{c};
    const auto applied{f.owner->Apply(std::move(c_again), decision, f.authority)};
    RequireOk(applied, "apply with the right token");
    BOOST_CHECK(applied->head->Header() == c.Header());
    BOOST_CHECK(applied->head->CertHash() == TestCertificate(c.Header()).Hash());
    BOOST_CHECK(f.Head() == applied->head);
    // The receipt names the applied decision record and the new head (O-92).
    BOOST_CHECK_EQUAL(applied->receipt.JournalInstance(), decision.JournalInstance());
    BOOST_CHECK_EQUAL(applied->receipt.DecisionSeq(), decision.Seq());
    BOOST_CHECK_EQUAL(applied->receipt.Slot(), c.Header().height);
    BOOST_CHECK(applied->receipt.HeaderHash() == c.Header().Hash());
    BOOST_CHECK(applied->receipt.StateRoot() == c.Header().state_root);
}

BOOST_AUTO_TEST_CASE(a_superseded_epoch_gives_authority_changed)
{
    OwnerFixture f;
    Fund(f);
    const std::shared_ptr<const state::HeadView> head{f.Head()};
    const state::Candidate c{f.Preview(f.Inputs({}, {f.actions.Place("carol", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 240, 10)}))};
    const DurableDecision decision{f.Decide(c)};
    AuthorityVersion bumped{f.authority};
    bumped.epoch += 1;
    state::Candidate attempt{c};
    RequireFailure(f.owner->Apply(std::move(attempt), decision, bumped), ErrorCode::AUTHORITY_CHANGED, state::AUTHORITY_SUPERSEDED,
                   "epoch moved on");
    BOOST_CHECK(f.Head() == head);
    BOOST_CHECK(head->State() == f.Head()->State());
    // Same epoch, another seat set: the whole AuthorityVersion is compared.
    AuthorityVersion reseated{f.authority};
    reseated.seat_set_root = LabelHash("fm2/test/other-seats");
    state::Candidate attempt2{c};
    RequireFailure(f.owner->Apply(std::move(attempt2), decision, reseated), ErrorCode::AUTHORITY_CHANGED, state::AUTHORITY_SUPERSEDED,
                   "seat set moved on");
    BOOST_CHECK(f.Head() == head);
    // The refused candidate is unchanged and applies under its own authority.
    state::Candidate attempt3{c};
    RequireOk(f.owner->Apply(std::move(attempt3), decision, f.authority), "apply under the candidate's authority");
    BOOST_CHECK(f.Head()->Header() == c.Header());
}

BOOST_AUTO_TEST_CASE(applying_the_same_decision_twice_gives_stale_parent)
{
    OwnerFixture f;
    Fund(f);
    const state::SlotInputs inputs{f.Inputs({}, {f.actions.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 240, 10)})};
    const state::Candidate first{f.Preview(inputs)};
    const state::Candidate second{f.Preview(inputs)}; // the same slot, previewed again
    const DurableDecision decision{f.Decide(first)};
    state::Candidate first_copy{first};
    const auto applied{f.owner->Apply(std::move(first_copy), decision, f.authority)};
    RequireOk(applied, "first apply");
    const std::shared_ptr<const state::HeadView> head{f.Head()};
    const state::CoreState state{head->State()};
    state::Candidate second_copy{second};
    RequireFailure(f.owner->Apply(std::move(second_copy), decision, f.authority), ErrorCode::STALE_PARENT, state::PARENT_NOT_HEAD,
                   "same decision again");
    state::Candidate first_again{first};
    RequireFailure(f.owner->Apply(std::move(first_again), decision, f.authority), ErrorCode::STALE_PARENT, state::PARENT_NOT_HEAD,
                   "same candidate again");
    BOOST_CHECK(f.Head() == head);
    BOOST_CHECK(head->State() == state);
    BOOST_CHECK(f.Head()->State().engine.ledger.BalanceOf(KeyAccount("alice"), TestAsset("rUSD")).reserved == 2'400);
}

BOOST_AUTO_TEST_CASE(reexecute_rejects_a_header_with_one_flipped_root)
{
    OwnerFixture f;
    Fund(f);
    const state::Candidate c{f.Preview(f.Inputs({f.actions.DepositBytes(KeyAccount("dave"), TestAsset("rUSD"), 5, "fund/dave")},
                                                {f.actions.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 240, 10),
                                                 f.actions.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 250, 4)}))};
    const std::shared_ptr<const state::HeadView> head{f.Head()};
    const Result<state::Candidate> again{f.owner->Reexecute(c.Header(), c.Body())};
    RequireOk(again, "re-execute the genuine header");
    RequireSameCandidate(*again, c);
    for (uint256 SlotHeaderV2::*root : {&SlotHeaderV2::facts_root, &SlotHeaderV2::actions_root, &SlotHeaderV2::outcomes_root,
                                        &SlotHeaderV2::state_root}) {
        SlotHeaderV2 flipped{c.Header()};
        flipped.*root = Flipped(flipped.*root);
        RequireFailure(f.owner->Reexecute(flipped, c.Body()), ErrorCode::VERIFY_FAILED, state::REEXECUTE_HEADER_MISMATCH,
                       "one flipped root");
    }
    // A flipped seat-set root names another authority: a leader fault.
    SlotHeaderV2 seats{c.Header()};
    seats.seat_set_root = Flipped(seats.seat_set_root);
    RequireFailure(f.owner->Reexecute(seats, c.Body()), ErrorCode::INVALID_INPUT, state::SLOT_SEAT_SET_MISMATCH, "flipped seat set root");
    // A changed count, as well.
    SlotHeaderV2 count{c.Header()};
    count.outcome_count += 1;
    RequireFailure(f.owner->Reexecute(count, c.Body()), ErrorCode::VERIFY_FAILED, state::REEXECUTE_HEADER_MISMATCH, "outcome count");
    BOOST_CHECK(f.Head() == head);
    // After the slot is applied its header no longer follows the head.
    (void)f.Commit(c);
    RequireFailure(f.owner->Reexecute(c.Header(), c.Body()), ErrorCode::STALE_PARENT, state::PARENT_NOT_HEAD, "applied slot");
}

BOOST_AUTO_TEST_CASE(a_five_slot_chain_links)
{
    OwnerFixture f;
    const state::HeadView genesis{*f.Head()};
    const SkeletonRun run{RunWalkingSkeleton(f)};
    BOOST_REQUIRE_EQUAL(run.heads.size(), 5U);
    const state::HeadView* parent{&genesis};
    for (size_t i{0}; i < run.heads.size(); ++i) {
        const state::HeadView& head{*run.heads[i]};
        const SlotHeaderV2& h{head.Header()};
        BOOST_CHECK_EQUAL(h.height, i + 1);
        BOOST_CHECK(h.parent_hash == parent->HeaderHash());
        BOOST_CHECK(h.parent_cert_hash == parent->CertHash());
        BOOST_CHECK(h.facts_from == parent->Header().facts_to);
        BOOST_CHECK(h.anchor.height >= parent->Header().anchor.height);
        BOOST_CHECK(head.CertHash() == TestCertificate(h).Hash());
        BOOST_CHECK(h.state_root == head.State().engine.Commit().value().Root());
        BOOST_CHECK(h.Authority() == f.authority);
        parent = &head;
    }
    BOOST_CHECK(f.Head() == run.heads.back());
    BOOST_CHECK_EQUAL(run.heads[0]->Header().outcome_count, 3U);
    BOOST_CHECK_EQUAL(run.heads[2]->Header().outcome_count, 3U); // c, d and the rUSD fee outcome
    BOOST_CHECK_EQUAL(run.heads[4]->Header().action_count, 0U);
    CheckWalkingSkeletonFinalState(f.Head()->State(), run);
    // Every slot was decided and applied exactly once in the commit journal.
    const std::vector<storage::JournalRecord> records{f.journal->ReadAll().value()};
    BOOST_REQUIRE_EQUAL(records.size(), 10U);
    for (size_t i{0}; i < records.size(); ++i) {
        BOOST_CHECK(records[i].type == (i % 2 == 0 ? storage::RecordType::DECISION : storage::RecordType::APPLIED));
    }
}

BOOST_AUTO_TEST_CASE(an_injected_invariant_failure_gives_invariant_and_no_candidate)
{
    // A reservation that no resting order holds: phase 9 (CONSERVATION) fails
    // on every candidate built on this state.
    const state::HeadView stray{DoctoredGenesis([](ledger::LedgerState& ledger) {
        RequireOk(ledger.CreditDeposit(Deposit(KeyAccount("alice"), TestAsset("rUSD"), 1'000, TestOutpoint("doctored", 0))), "deposit");
        BOOST_REQUIRE(ledger.Reserve(KeyAccount("alice"), TestAsset("rUSD"), 100).value() == ledger::ReserveResult::OK);
    })};
    OwnerFixture f{1, stray};
    const std::shared_ptr<const state::HeadView> head{f.Head()};
    RequireError(f.TryPreview(Heartbeat(*head, f.authority)), ErrorCode::INVARIANT, "stray reservation");
    RequireError(f.TryPreview(f.Inputs({}, {f.actions.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 1, 1)})),
                 ErrorCode::INVARIANT, "stray reservation with an action");
    BOOST_CHECK(f.Head() == head);

    // Fees withheld and never distributed: the engine refuses the parent.
    const state::HeadView held{DoctoredGenesis([](ledger::LedgerState& ledger) {
        RequireOk(ledger.CreditDeposit(Deposit(KeyAccount("alice"), TestAsset("rUSD"), 1'000, TestOutpoint("doctored", 0))), "deposit");
        RequireOk(ledger.CreditDeposit(Deposit(KeyAccount("bob"), TestAsset("rUSD"), 1'000, TestOutpoint("doctored", 1))), "deposit");
        BOOST_REQUIRE(ledger.Reserve(KeyAccount("alice"), TestAsset("rUSD"), 100).value() == ledger::ReserveResult::OK);
        RequireOk(ledger.PayFromReserved(KeyAccount("alice"), KeyAccount("bob"), TestAsset("rUSD"), 100, 1), "withhold a fee");
    })};
    OwnerFixture g{1, held};
    RequireError(g.TryPreview(Heartbeat(*g.Head(), g.authority)), ErrorCode::INVARIANT, "fees held by the parent");
}

BOOST_AUTO_TEST_CASE(each_leader_fault_input_gives_invalid_input_and_no_candidate)
{
    OwnerFixture f;
    f.authority.epoch = 1; // the parent of the faulty slots is certified under epoch 1
    Fund(f);
    (void)f.Run(f.Inputs({}, {f.actions.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 240, 10)}));
    SlotFixture& a{f.actions};
    const std::shared_ptr<const state::HeadView> head{f.Head()};
    const std::vector<unsigned char> good{a.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 300, 1)};
    RequireOk(f.TryPreview(f.Inputs({}, {good})), "the unfaulted slot");

    struct Fault {
        std::string name;
        std::function<void(state::SlotInputs&)> apply;
        std::string_view detail;
    };
    ActionV2 expired{a.Unsigned("bob", 2, ActionKind::PLACE,
                                EncodePayload(PlacePayload{MarketM1().Id(), Side::SELL, TimeInForce::GTC, 300, 1}).value(), 0, 1)};
    ActionV2 cancel_all{a.Unsigned("bob", 3, ActionKind::CANCEL_ALL, {})};
    std::vector<unsigned char> bad_sig{a.Place("carol", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 1, 1)};
    bad_sig.back() ^= 0x01;
    const std::vector<Fault> faults{
        {"epoch below the parent's", [](state::SlotInputs& in) { in.authority.epoch = 0; }, state::SLOT_EPOCH_REGRESSED},
        {"another seat set", [](state::SlotInputs& in) { in.authority.seat_set_root = LabelHash("fm2/test/other-seats"); },
         state::SLOT_SEAT_SET_MISMATCH},
        {"another profile", [](state::SlotInputs& in) { in.authority.profile_hash = LabelHash("fm2/test/other-profile"); },
         engine::SLOT_PROFILE_MISMATCH},
        {"anchor below the parent's", [](state::SlotInputs& in) { in.anchor = TestAnchor(129); }, engine::SLOT_ANCHOR_BELOW_PARENT},
        {"anchor fork", [](state::SlotInputs& in) { in.anchor.hash = LabelHash("fm2/test/fork"); }, engine::SLOT_ANCHOR_FORK},
        {"facts not contiguous", [](state::SlotInputs& in) { in.facts_from = FactCursor{130, 0}; }, engine::SLOT_FACTS_NOT_CONTIGUOUS},
        {"facts reversed", [](state::SlotInputs& in) { in.facts_to = FactCursor{130, 1}; }, engine::SLOT_FACTS_REVERSED},
        {"facts beyond the anchor", [](state::SlotInputs& in) { in.facts_to = FactCursor{132, 0}; }, engine::SLOT_FACTS_BEYOND_ANCHOR},
        {"malformed fact", [](state::SlotInputs& in) { in.body.facts = {{0x01, 0x02}}; in.facts_to = FactCursor{131, 0}; in.anchor = TestAnchor(130); },
         engine::SLOT_FACT_MALFORMED},
        {"too many originals", [&](state::SlotInputs& in) { in.body.originals.assign(RegtestProfile().max_actions_per_slot + 1, good); },
         engine::SLOT_ACTION_COUNT_CAP},
        {"malformed original", [](state::SlotInputs& in) { in.body.originals = {{0x00, 0x01}}; }, engine::SLOT_ACTION_MALFORMED},
        {"unsupported kind", [&](state::SlotInputs& in) { in.body.originals = {a.Sign(cancel_all, "bob")}; }, engine::SLOT_ACTION_KIND},
        {"outside its window", [&](state::SlotInputs& in) { in.body.originals = {a.Sign(expired, "bob")}; }, engine::SLOT_ACTION_WINDOW},
        {"duplicate original", [&](state::SlotInputs& in) { in.body.originals = {good, good}; }, engine::SLOT_DUPLICATE_ACTION},
        {"duplicate nonce",
         [&](state::SlotInputs& in) {
             in.body.originals = {good, a.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 301, 1)};
         },
         engine::SLOT_DUPLICATE_NONCE},
        {"account absent",
         [&](state::SlotInputs& in) { in.body.originals = {a.Place("dave", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 1, 1)}; },
         engine::SLOT_ACCOUNT_ABSENT},
        {"nonce already used",
         [&](state::SlotInputs& in) { in.body.originals = {a.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 2, 1)}; },
         engine::SLOT_NONCE_EXCLUDED},
        {"bad signature", [&](state::SlotInputs& in) { in.body.originals = {bad_sig}; }, engine::SLOT_BAD_SIGNATURE},
    };
    for (const Fault& fault : faults) {
        state::SlotInputs inputs{f.Inputs({}, {good})};
        fault.apply(inputs);
        RequireFailure(f.TryPreview(inputs), ErrorCode::INVALID_INPUT, fault.detail, fault.name);
        BOOST_CHECK_MESSAGE(f.Head() == head, fault.name + ": head changed");
    }
    BOOST_CHECK(f.Head()->State() == head->State());
}

BOOST_AUTO_TEST_CASE(readers_see_one_coherent_head_while_apply_runs)
{
    // Readers take Head() snapshots and preview heartbeats on them while the
    // writer applies five slots. Every snapshot's header commits to its own
    // state, heights never go back, and a preview is either a candidate on
    // the snapshot or STALE_PARENT once the writer has moved on.
    OwnerFixture f;
    std::atomic<bool> done{false};
    std::atomic<uint64_t> previews{0};
    std::atomic<uint64_t> problems{0};
    const auto reader{[&] {
        uint64_t last_height{0};
        while (!done.load()) {
            const std::shared_ptr<const state::HeadView> head{f.owner->Head()};
            if (head->Header().height < last_height) ++problems;
            last_height = head->Header().height;
            const Result<StateRootsV0> roots{head->State().engine.Commit()};
            if (!roots.has_value() || roots->Root() != head->Header().state_root) ++problems;
            const Result<state::Candidate> c{f.owner->Preview(state::ParentRef::Committed(*head), Heartbeat(*head, f.authority))};
            if (c.has_value()) {
                if (c->Header().parent_hash != head->HeaderHash()) ++problems;
            } else if (c.error().code != ErrorCode::STALE_PARENT) {
                ++problems;
            }
            ++previews;
        }
    }};
    std::vector<std::thread> readers;
    for (int i{0}; i < 3; ++i) readers.emplace_back(reader);
    const SkeletonRun run{RunWalkingSkeleton(f)};
    while (previews.load() < 30) std::this_thread::yield();
    done = true;
    for (auto& t : readers) t.join();
    BOOST_CHECK_EQUAL(problems.load(), 0U);
    BOOST_CHECK(previews.load() >= 30U);
    CheckWalkingSkeletonFinalState(f.Head()->State(), run);
}

BOOST_AUTO_TEST_CASE(certified_genesis_replay_installs_only_applied_and_resumes_decided_tail)
{
    CertifiedHistory history;
    history.Next(true);
    const auto committed{history.live->Head()};
    history.Next(false);
    const auto records{history.journal->ReadAll().value()};
    BOOST_REQUIRE_EQUAL(records.size(), 3U);
    auto restored{history.Fresh()};
    const auto result{restored->RestoreVerifiedGenesis(history.genesis, records, history.seats)};
    RequireOk(result, "certified restart");
    BOOST_CHECK_EQUAL(restored->Head()->Header().height, 1U);
    BOOST_CHECK(restored->Head()->Header() == committed->Header());
    BOOST_CHECK(restored->Head()->State() == committed->State());
    BOOST_CHECK(restored->Head()->CertHash() == committed->CertHash());
    BOOST_CHECK_EQUAL(result->head.Header().height, 1U);
    BOOST_CHECK_EQUAL(result->decided_head.Header().height, 2U);
    BOOST_REQUIRE_EQUAL(result->slots.size(), 2U);
    BOOST_CHECK(result->slots[0].applied_seq.has_value());
    BOOST_CHECK(!result->slots[1].applied_seq.has_value());
    // The same certified tail completes through the real recovered log and
    // check-3 ApplyReceipt path, without a forged APPLIED marker.
    storage::CommitLog recovered{storage::CommitLog::Create(*history.journal, history.profile).value()};
    const auto decision{storage::DecisionRecord::Decode(records.back().payload, history.profile).value()};
    state::Candidate candidate{restored->Reexecute(decision.header, decision.body).value()};
    const auto pending{recovered.AppendDecision(decision.header, decision.certificate, decision.body).value()};
    const auto token{recovered.Confirm(pending).value()};
    const auto applied{restored->Apply(std::move(candidate), token, history.authority).value()};
    const auto marker{recovered.AppendApplied(applied.receipt).value()};
    RequireOk(recovered.Barrier(), "recovered applied barrier");
    RequireOk(recovered.Confirm(marker), "durable recovered applied");
    BOOST_CHECK_EQUAL(restored->Head()->Header().height, 2U);
    auto restarted_again{history.Fresh()};
    const auto complete{restarted_again->RestoreVerifiedGenesis(history.genesis, history.journal->ReadAll().value(), history.seats)};
    RequireOk(complete, "second certified restart");
    BOOST_CHECK(complete->head.Header() == complete->decided_head.Header());
    BOOST_CHECK(restarted_again->Head()->Header() == restored->Head()->Header());
}

BOOST_AUTO_TEST_CASE(certified_replay_checks_every_certificate_authority_body_and_chain)
{
    CertifiedHistory history;
    history.Next(true);
    const auto original{history.journal->ReadAll().value()};
    const auto decision{storage::DecisionRecord::Decode(original.front().payload, history.profile).value()};
    const auto refuse = [&](const std::vector<storage::JournalRecord>& records, ErrorCode code, std::string_view detail) {
        auto owner{history.Fresh()};
        const auto before{owner->Head()};
        RequireFailure(owner->RestoreVerifiedGenesis(history.genesis, records, history.seats), code, detail, "corrupt certified restart");
        BOOST_CHECK(owner->Head() == before);
    };
    const std::vector<std::function<void(storage::DecisionRecord&)>> certificate_faults{
        [](auto& d) { d.certificate.aggregate.fill(0); },
        [](auto& d) { d.certificate.header_hash = Flipped(d.certificate.header_hash); },
        [](auto& d) { d.certificate.bitmap = {0}; },
        [](auto& d) { d.certificate.bitmap = {0x80}; },
        [](auto& d) { d.certificate.bitmap.clear(); },
        [&](auto& d) { d.certificate = SignCertificate(d.header, history.names, {0, 1}); },
        [&](auto& d) {
            SlotHeaderV2 other{d.header};
            other.state_root = Flipped(other.state_root);
            d.certificate = SignCertificate(other, history.names, {0, 1, 2});
            d.certificate.header_hash = d.header.Hash();
        }};
    for (const auto& fault : certificate_faults) {
        auto changed{decision};
        fault(changed);
        auto records{original};
        records[0].payload = changed.Encode();
        Rechain(records, history.journal->InstanceId());
        refuse(records, ErrorCode::VERIFY_FAILED, state::RESTORE_CERTIFICATE);
    }
    for (int field{0}; field < 4; ++field) {
        auto changed{decision};
        if (field == 0) changed.header.domain = Flipped(changed.header.domain);
        if (field == 1) changed.header.profile_hash = Flipped(changed.header.profile_hash);
        if (field == 2) changed.header.authority_epoch = 1;
        if (field == 3) changed.header.seat_set_root = Flipped(changed.header.seat_set_root);
        changed.certificate = SignCertificate(changed.header, history.names, {0, 1, 2});
        auto records{original};
        records[0].payload = changed.Encode();
        Rechain(records, history.journal->InstanceId());
        refuse(records, ErrorCode::VERIFY_FAILED, state::RESTORE_AUTHORITY);
    }
    auto changed{decision};
    changed.header.commitment_version = COMMITMENT_VERSION_WITHDRAWALS;
    changed.certificate = SignCertificate(changed.header, history.names, {0, 1, 2});
    auto records{original};
    records[0].payload = changed.Encode();
    Rechain(records, history.journal->InstanceId());
    refuse(records, ErrorCode::VERIFY_FAILED, state::OWNER_COMMITMENT_VERSION);
    // Invalid duplicate records cannot hide behind the first valid decision.
    for (bool bad_body : {false, true}) {
        records = original;
        changed = decision;
        if (bad_body) changed.body.originals.push_back({1});
        else changed.certificate.aggregate.fill(0);
        records.push_back(storage::JournalRecord{3, storage::RecordType::DECISION, changed.Encode(), {}});
        Rechain(records, history.journal->InstanceId());
        refuse(records, ErrorCode::VERIFY_FAILED, bad_body ? storage::BODY_HEADER_MISMATCH : state::RESTORE_CERTIFICATE);
    }
    records = original;
    Rechain(records, history.journal->InstanceId() + 1);
    refuse(records, ErrorCode::VERIFY_FAILED, state::RESTORE_RECORD_CHAIN);
    records = original;
    records.front().seq = 2;
    refuse(records, ErrorCode::ORDER_VIOLATION, state::REPLAY_SEQ);
    records = original;
    auto applied{storage::AppliedRecord::Decode(records[1].payload).value()};
    applied.state_root = Flipped(applied.state_root);
    records[1].payload = applied.Encode();
    Rechain(records, history.journal->InstanceId());
    refuse(records, ErrorCode::VERIFY_FAILED, state::REPLAY_APPLIED_MISMATCH);
    // A valid alternative quorum certificate is a valid duplicate; the
    // first certificate/hash and state remain authoritative.
    changed = decision;
    changed.certificate = SignCertificate(changed.header, history.names, {1, 2, 3});
    records = original;
    records.push_back(storage::JournalRecord{3, storage::RecordType::DECISION, changed.Encode(), {}});
    Rechain(records, history.journal->InstanceId());
    auto owner{history.Fresh()};
    const auto duplicate{owner->RestoreVerifiedGenesis(history.genesis, records, history.seats)};
    RequireOk(duplicate, "valid duplicate certificate");
    BOOST_CHECK_EQUAL(duplicate->duplicate_decisions, 1U);
    BOOST_CHECK(owner->Head()->CertHash() == decision.certificate.Hash());
}

BOOST_AUTO_TEST_CASE(certified_restore_requires_exact_configured_genesis_and_refuses_live_rollback)
{
    CertifiedHistory history;
    history.Next(true);
    const auto records{history.journal->ReadAll().value()};
    const auto live_before{history.live->Head()};
    RequireFailure(history.live->RestoreVerifiedGenesis(history.genesis, records, history.seats),
                   ErrorCode::STALE_PARENT, state::PARENT_NOT_HEAD, "no live rollback");
    BOOST_CHECK(history.live->Head() == live_before);
    auto owner{history.Fresh()};
    const auto before{owner->Head()};
    auto changed_header{history.genesis.Header()};
    changed_header.anchor.hash = Flipped(changed_header.anchor.hash);
    const auto changed{state::HeadView::Make(changed_header, {}, history.genesis.StatePtr()).value()};
    RequireFailure(owner->RestoreVerifiedGenesis(changed, records, history.seats), ErrorCode::VERIFY_FAILED,
                   state::RESTORE_BASE_MISMATCH, "another genesis header");
    const auto other_seats{TestSeats({"other0", "other1", "other2", "other3"})};
    RequireFailure(owner->RestoreVerifiedGenesis(history.genesis, records, other_seats), ErrorCode::VERIFY_FAILED,
                   state::RESTORE_AUTHORITY, "untrusted roster");
    BOOST_CHECK(owner->Head() == before);
    // An unapplied-only journal restores genesis and returns the fenced tail.
    CertifiedHistory unsigned_application;
    unsigned_application.Next(false);
    auto no_applied{unsigned_application.Fresh()};
    const auto tail{no_applied->RestoreVerifiedGenesis(unsigned_application.genesis,
                      unsigned_application.journal->ReadAll().value(), unsigned_application.seats)};
    RequireOk(tail, "unapplied-only certified tail");
    BOOST_CHECK_EQUAL(no_applied->Head()->Header().height, 0U);
    BOOST_CHECK_EQUAL(tail->decided_head.Header().height, 1U);
    // The explicit static-authority path refuses an otherwise valid epoch1 base.
    auto next_authority{history.authority};
    next_authority.epoch = 1;
    const auto next_genesis{state::GenesisHead(history.profile, std::vector<MarketDescriptor>{MarketM1(), MarketM2()},
        TestDomain(), next_authority, TestAnchor(129), FactCursor{130, 0}).value()};
    auto next_owner{state::StateOwner::Create(history.profile, next_genesis, 701).value()};
    RequireFailure(next_owner->RestoreVerifiedGenesis(next_genesis, {}, history.seats), ErrorCode::VERIFY_FAILED,
                   state::RESTORE_AUTHORITY, "unsupported changing/static epoch");
}

BOOST_AUTO_TEST_CASE(owner_creation_checks_commitment_version_and_restore_cas_cannot_overwrite_apply)
{
    for (const auto& profile : {RegtestProfile(), StableQRegtestProfile(ChainType::REGTEST).value()}) {
        const auto genesis{TestGenesisHead(profile)};
        auto header{genesis.Header()};
        header.commitment_version = profile.withdrawals_enabled ? COMMITMENT_VERSION_V0 : COMMITMENT_VERSION_WITHDRAWALS;
        const auto wrong{state::HeadView::Make(header, {}, genesis.StatePtr()).value()};
        RequireFailure(state::StateOwner::Create(profile, wrong, 701), ErrorCode::VERIFY_FAILED,
                       state::OWNER_COMMITMENT_VERSION, "mislabeled genesis commitment");
    }
    CertifiedHistory history;
    history.Next(true);
    const auto records{history.journal->ReadAll().value()};
    const auto decision{storage::DecisionRecord::Decode(records[0].payload, history.profile).value()};
    auto owner{history.Fresh()};
    state::Candidate candidate{owner->Reexecute(decision.header, decision.body).value()};
    const auto pending{history.log.AppendDecision(decision.header, decision.certificate, decision.body).value()};
    const auto token{history.log.Confirm(pending).value()};
    std::atomic<bool> start{false};
    bool restored{false}, restore_stale{false};
    std::thread restart{[&] {
        while (!start.load()) std::this_thread::yield();
        const auto result{owner->RestoreVerifiedGenesis(history.genesis, records, history.seats)};
        restored = result.has_value();
        restore_stale = !result.has_value() && result.error().code == ErrorCode::STALE_PARENT;
    }};
    start = true;
    const auto applied{owner->Apply(std::move(candidate), token, history.authority)};
    restart.join();
    BOOST_CHECK(restored != applied.has_value());
    BOOST_CHECK(restored || restore_stale);
    if (!applied.has_value()) BOOST_CHECK(applied.error().code == ErrorCode::STALE_PARENT);
    BOOST_CHECK(owner->Head()->Header() == history.live->Head()->Header());
    BOOST_CHECK(owner->Head()->CertHash() == history.live->Head()->CertHash());
    BOOST_CHECK(owner->Head()->State() == history.live->Head()->State());
}


BOOST_AUTO_TEST_CASE(certified_replay_authenticates_prepared_only_crash_tail_without_applying)
{
    CertifiedHistory history;
    const auto candidate{history.live->Preview(state::ParentRef::Committed(*history.live->Head()),
                                               Heartbeat(*history.live->Head(), history.authority)).value()};
    auto qc{SignCertificate(candidate.Header(), history.names, {0, 1, 2})};
    const auto digest{agreement::PbftPrepareDigest(candidate.Header(), 0)};
    std::vector<bls::Signature> signatures;
    for (uint32_t i : {0U, 1U, 2U}) signatures.push_back(BlsKeyFromLabel(SeatLabel(history.names[i])).Sign(std::span{digest.begin(), digest.end()}));
    qc.aggregate = bls::AggregateSignatures(signatures)->Compressed();
    storage::PreparedQcRecord prepared{0, candidate.Header().height, 0, candidate.Header().Hash(), qc.Encode(), candidate.Header(), candidate.Body()};
    RequireOk(history.log.RecordVerifiedPreparedQc(prepared), "prepared record");
    RequireOk(history.log.Barrier(), "prepared barrier");
    const auto records{history.journal->ReadAll().value()};
    auto owner{history.Fresh()};
    const auto replayed{owner->RestoreVerifiedGenesis(history.genesis, records, history.seats)};
    RequireOk(replayed, "prepared-only restart");
    BOOST_CHECK_EQUAL(owner->Head()->Header().height, 0U);
    BOOST_CHECK_EQUAL(replayed->decided_head.Header().height, 0U);
    BOOST_CHECK(replayed->slots.empty());
    const auto recovered_log{storage::CommitLog::Create(*history.journal, history.profile)};
    RequireOk(recovered_log, "prepared index recovery");
    BOOST_CHECK(recovered_log->PreparedAt(0, 1, 0).has_value());
    for (int mutation = 0; mutation < 5; ++mutation) {
        auto wrong{prepared};
        if (mutation == 0) { auto invalid{qc}; invalid.aggregate.fill(0); wrong.qc = invalid.Encode(); }
        if (mutation == 1) wrong.body.originals.push_back({1});
        if (mutation == 2) wrong.header_hash = LabelHash("another-prepared-header");
        if (mutation == 3) wrong.view = 1;
        if (mutation == 4) wrong.qc = SignCertificate(candidate.Header(), history.names, {0, 1, 2}).Encode();
        auto damaged{records};
        damaged.front().payload = wrong.Encode();
        Rechain(damaged, history.journal->InstanceId());
        auto fresh{history.Fresh()};
        BOOST_CHECK(!fresh->RestoreVerifiedGenesis(history.genesis, damaged, history.seats).has_value());
        BOOST_CHECK_EQUAL(fresh->Head()->Header().height, 0U);
    }
    // The same value later receives COMMIT and APPLIED; preparation is not
    // replayed as an application and cannot double-count the slot.
    history.Next(true);
    auto committed{history.Fresh()};
    const auto complete{committed->RestoreVerifiedGenesis(history.genesis, history.journal->ReadAll().value(), history.seats)};
    RequireOk(complete, "prepared then committed restart");
    BOOST_CHECK_EQUAL(committed->Head()->Header().height, 1U);
    BOOST_CHECK_EQUAL(complete->slots.size(), 1U);
}


BOOST_AUTO_TEST_CASE(view_bound_certified_restore_verifies_nonzero_prepare_and_commit_view)
{
    const auto profile{StableQViewRegtestProfile(ChainType::REGTEST).value()};
    const std::vector<std::string> names{"bound0", "bound1", "bound2", "bound3"};
    const auto seats{TestSeats(names)};
    const AuthorityVersion authority{0, seats.Root(), ProfileHash(profile)};
    const auto genesis{state::GenesisHead(profile, std::vector<MarketDescriptor>{MarketM1(), MarketM2()},
                                          TestDomain(), authority, TestAnchor(129), FactCursor{130, 0}).value()};
    auto journal{NewCommitJournal(911)};
    auto log{storage::CommitLog::Create(*journal, profile).value()};
    auto owner{state::StateOwner::Create(profile, genesis, 911).value()};
    auto candidate{owner->Preview(state::ParentRef::Committed(genesis), Heartbeat(genesis, authority)).value()};
    BOOST_CHECK_EQUAL(candidate.Header().commitment_version, COMMITMENT_VERSION_VIEW_BOUND);
    auto prepare{SignCertificate(candidate.Header(), names, {0, 1, 2}, std::nullopt, 7)};
    const auto digest{agreement::PbftPrepareDigest(candidate.Header(), 7)};
    std::vector<bls::Signature> signatures;
    for (uint32_t i : {0U, 1U, 2U}) signatures.push_back(BlsKeyFromLabel(SeatLabel(names[i])).Sign(std::span{digest.begin(), digest.end()}));
    prepare.aggregate = bls::AggregateSignatures(signatures)->Compressed();
    RequireOk(log.RecordVerifiedPreparedQc({0, 1, 7, candidate.Header().Hash(), prepare.Encode(), candidate.Header(), candidate.Body()}), "view7 preparation");
    RequireOk(log.Barrier(), "view7 prepared barrier");
    const auto certificate{SignCertificate(candidate.Header(), names, {0, 1, 2}, std::nullopt, 7)};
    const auto pending{log.AppendDecision(candidate.Header(), certificate, candidate.Body()).value()};
    RequireOk(log.Barrier(), "view7 decision barrier");
    const auto applied{owner->Apply(std::move(candidate), log.Confirm(pending).value(), authority).value()};
    RequireOk(log.AppendApplied(applied.receipt), "view7 applied record");
    RequireOk(log.Barrier(), "view7 applied barrier");
    const auto records{journal->ReadAll().value()};
    auto restored{state::StateOwner::Create(profile, genesis, 911).value()};
    RequireOk(restored->RestoreVerifiedGenesis(genesis, records, seats), "view7 full certified restore");
    BOOST_CHECK(restored->Head()->Header() == owner->Head()->Header());
    BOOST_CHECK(restored->Head()->State() == owner->Head()->State());
    for (int mutation = 0; mutation < 3; ++mutation) {
        auto bad{records};
        if (mutation == 0) {
            auto decision{storage::DecisionRecord::Decode(bad[1].payload, profile).value()};
            decision.certificate.view = 8;
            bad[1].payload = decision.Encode();
        } else if (mutation == 1) {
            auto prepared{storage::PreparedQcRecord::Decode(bad[0].payload, profile).value()};
            prepared.view = 8;
            bad[0].payload = prepared.Encode();
        } else {
            auto decision{storage::DecisionRecord::Decode(bad[1].payload, profile).value()};
            decision.certificate.view.reset();
            bad[1].payload = decision.Encode();
        }
        Rechain(bad, 911);
        auto fresh{state::StateOwner::Create(profile, genesis, 911).value()};
        BOOST_CHECK(!fresh->RestoreVerifiedGenesis(genesis, bad, seats).has_value());
        BOOST_CHECK_EQUAL(fresh->Head()->Header().height, 0U);
    }
}

BOOST_AUTO_TEST_SUITE_END()
