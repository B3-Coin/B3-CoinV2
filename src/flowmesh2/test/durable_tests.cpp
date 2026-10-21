// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/durable.h>
#include <flowmesh2/storage/journal.h>
#include <flowmesh2/storage/memory_journal.h>
#include <flowmesh2/storage/signer_log.h>
#include <flowmesh2/storage/tokens.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/state/state_owner.h>
#include <flowmesh2/test/util/state_fixtures.h>
#include <flowmesh2/test/util/storage_fixtures.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::storage;
using namespace flowmesh2::test;

namespace {

// Compile-time contract of the durability tokens (ARCHITECTURE.md 7.2). The
// constructor is private, so is_constructible is false outside Journal; the
// compile-fail control test/compile_fail/forge_token.cpp proves the same with
// a real construction attempt.
template <class Token>
constexpr bool Unforgeable()
{
    return !std::is_default_constructible_v<Token> &&
           !std::is_copy_constructible_v<Token> &&
           !std::is_copy_assignable_v<Token> &&
           !std::is_move_assignable_v<Token> &&
           std::is_nothrow_move_constructible_v<Token> &&
           !std::is_constructible_v<Token, uint64_t, uint64_t> &&
           !std::is_constructible_v<Token, int, int> &&
           !std::is_constructible_v<Token, uint64_t, uint64_t, const typename Token::Ref&> &&
           !std::is_constructible_v<Token, const typename Token::Ref&> &&
           !std::is_convertible_v<uint64_t, Token>;
}

static_assert(Unforgeable<DurableControlIntent>());
static_assert(Unforgeable<DurableSignedControl>());
static_assert(Unforgeable<DurableIntent>());
static_assert(Unforgeable<DurableSignedVote>());
static_assert(Unforgeable<DurableDecision>());
static_assert(Unforgeable<DurableApplied>());
static_assert(!std::is_constructible_v<DurableIntent, DurableDecision&&>, "one barrier's token never stands in for another's");
static_assert(!std::is_constructible_v<DurableApplied, DurableDecision&&>);

// The ApplyReceipt (O-92): only StateOwner::Apply makes one; the compile-fail
// control test/compile_fail/forge_receipt.cpp proves it with a construction.
static_assert(!std::is_default_constructible_v<ApplyReceipt> && !std::is_copy_constructible_v<ApplyReceipt> &&
              !std::is_copy_assignable_v<ApplyReceipt> && !std::is_move_assignable_v<ApplyReceipt> &&
              std::is_nothrow_move_constructible_v<ApplyReceipt> &&
              !std::is_constructible_v<ApplyReceipt, uint64_t, uint64_t, uint64_t, const uint256&, const uint256&> &&
              !std::is_constructible_v<ApplyReceipt, const DurableDecision&> && !std::is_constructible_v<ApplyReceipt, DurableDecision&&>);

// A Pending is copyable (it names a record) but only the logs can make one.
template <class P>
constexpr bool PendingUnforgeable()
{
    return !std::is_default_constructible_v<P> && std::is_copy_constructible_v<P> &&
           !std::is_constructible_v<P, uint64_t, uint64_t, const typename P::Ref&>;
}
static_assert(PendingUnforgeable<PendingControlIntent>());
static_assert(PendingUnforgeable<PendingSignedControl>());
static_assert(PendingUnforgeable<PendingProposal>());
static_assert(PendingUnforgeable<PendingIntent>());
static_assert(PendingUnforgeable<PendingSignedVote>());
static_assert(PendingUnforgeable<PendingDecision>());
static_assert(PendingUnforgeable<PendingApplied>());

// The token kinds Confirm can mint, and nothing else.
static_assert(HasDurableKind(RecordType::INTENT) && HasDurableKind(RecordType::SIGNED_VOTE) &&
              HasDurableKind(RecordType::DECISION) && HasDurableKind(RecordType::APPLIED));
static_assert(!HasDurableKind(RecordType::ACCEPTED_PROPOSAL) && !HasDurableKind(RecordType::SNAPSHOT_MARK) &&
              !HasDurableKind(RecordType::VIEW_STATE) && !HasDurableKind(RecordType::NEW_VIEW_ACCEPTED) &&
              !HasDurableKind(RecordType::PREPARED_QC));

//! A commit log over a fresh memory journal.
struct CommitFixture {
    std::unique_ptr<MemoryJournal> journal;
    CommitLog log;

    explicit CommitFixture(uint64_t instance, MemoryJournalFaults faults = {})
        : journal{NewMemoryJournal(JournalRole::COMMIT, instance, faults)}, log{CommitLog::Create(*journal).value()}
    {
    }

    PendingDecision Decide(const StorageSlot& slot)
    {
        Result<PendingDecision> pending{log.AppendDecision(slot.header, slot.certificate, slot.body)};
        RequireStorageOk(pending, "AppendDecision");
        return *pending;
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(durable_tests)

BOOST_AUTO_TEST_CASE(token_kinds_pinned)
{
    // DurableKind values follow RecordType where they match (INTENT=1,
    // SIGNED_VOTE=2, DECISION=3, APPLIED=4); they are pinned here.
    BOOST_CHECK_EQUAL(static_cast<int>(DurableIntent::KIND), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(DurableSignedVote::KIND), 2);
    BOOST_CHECK_EQUAL(static_cast<int>(DurableDecision::KIND), 3);
    BOOST_CHECK_EQUAL(static_cast<int>(DurableApplied::KIND), 4);
    BOOST_CHECK(RecordTypeOf(DurableKind::DECISION) == RecordType::DECISION);
    BOOST_CHECK(DurableKindOf(RecordType::APPLIED) == DurableKind::APPLIED);
}

BOOST_AUTO_TEST_CASE(confirm_before_barrier_is_refused)
{
    CommitFixture f{1};
    const StorageSlot slot{MakeStorageSlot(1, 0, "a")};
    const PendingDecision pending{f.Decide(slot)};
    RequireRefusal(f.log.Confirm(pending), ErrorCode::ORDER_VIOLATION, CONFIRM_BEFORE_BARRIER, "confirm before the barrier");
    BOOST_CHECK_EQUAL(f.log.Barrier().value(), 1U);
    const Result<DurableDecision> token{f.log.Confirm(pending)};
    RequireStorageOk(token, "confirm after the barrier");
    // Confirm can be repeated: it proves a durable fact.
    RequireStorageOk(f.log.Confirm(pending), "second confirm");
}

BOOST_AUTO_TEST_CASE(group_commit_confirms_each_record)
{
    // Three appends and one barrier give three confirms.
    RecordingObserver observer;
    auto journal{NewMemoryJournal(JournalRole::COMMIT, 2, {}, &observer)};
    CommitLog log{CommitLog::Create(*journal).value()};
    std::vector<PendingDecision> pendings;
    for (uint64_t height{1}; height <= 3; ++height) {
        Result<PendingDecision> pending{log.AppendDecision(MakeStorageSlot(height, 0, "g").header, MakeStorageSlot(height, 0, "g").certificate,
                                                           MakeStorageSlot(height, 0, "g").body)};
        RequireStorageOk(pending, "AppendDecision");
        BOOST_CHECK_EQUAL(pending->Seq(), height);
        RequireRefusal(log.Confirm(*pending), ErrorCode::ORDER_VIOLATION, CONFIRM_BEFORE_BARRIER, "before the barrier");
        pendings.push_back(*pending);
    }
    BOOST_CHECK_EQUAL(log.Barrier().value(), 3U);
    for (uint64_t i{0}; i < 3; ++i) {
        const Result<DurableDecision> token{log.Confirm(pendings[i])};
        RequireStorageOk(token, "confirm");
        BOOST_CHECK_EQUAL(token->Seq(), i + 1);
        BOOST_CHECK_EQUAL(token->Slot(), i + 1);
        BOOST_CHECK(token->Digest() == MakeStorageSlot(i + 1, 0, "g").header.Hash());
    }
    // Exactly one barrier covered the three records.
    BOOST_CHECK_EQUAL(std::count_if(observer.events.begin(), observer.events.end(),
                                    [](const std::string& e) { return e.starts_with("barrier-start"); }),
                      1);
}

BOOST_AUTO_TEST_CASE(confirm_is_refused_after_a_fatal_barrier)
{
    // A failed barrier is sticky: Append is FENCED and Confirm is refused,
    // even for a record that was durable before the failure.
    CommitFixture f{3, MemoryJournalFaults{.fail_barrier = 2}};
    const PendingDecision first{f.Decide(MakeStorageSlot(1, 0, "a"))};
    BOOST_CHECK_EQUAL(f.log.Barrier().value(), 1U);
    RequireStorageOk(f.log.Confirm(first), "confirm before the failure");
    const PendingDecision second{f.Decide(MakeStorageSlot(2, 0, "a"))};
    RequireRefusal(f.log.Barrier(), ErrorCode::STORAGE_FATAL, MEMORY_JOURNAL_BARRIER_FAULT, "failed barrier");
    RequireRefusal(f.log.Barrier(), ErrorCode::STORAGE_FATAL, JOURNAL_FATAL, "barrier after the failure");
    RequireRefusal(f.log.Confirm(second), ErrorCode::FENCED, JOURNAL_FENCED, "confirm of the uncovered record");
    RequireRefusal(f.log.Confirm(first), ErrorCode::FENCED, JOURNAL_FENCED, "confirm of the covered record");
    const StorageSlot third{MakeStorageSlot(3, 0, "a")};
    RequireRefusal(f.log.AppendDecision(third.header, third.certificate, third.body), ErrorCode::FENCED, JOURNAL_FENCED, "append after fatal");
    RequireRefusal(f.journal->Append(RecordType::DECISION, Bytes("raw")), ErrorCode::FENCED, JOURNAL_FENCED, "raw append after fatal");
}

BOOST_AUTO_TEST_CASE(confirm_is_refused_for_another_journal)
{
    CommitFixture a{10};
    CommitFixture b{11};
    const PendingDecision pending{a.Decide(MakeStorageSlot(1, 0, "x"))};
    BOOST_CHECK_EQUAL(a.log.Barrier().value(), 1U);
    BOOST_CHECK_EQUAL(b.log.Barrier().value(), 0U);
    RequireRefusal(b.log.Confirm(pending), ErrorCode::VERIFY_FAILED, CONFIRM_OTHER_JOURNAL, "pending of another instance");
    // A journal with the same instance id but without that record does not know its sequence number.
    CommitFixture same_id{10};
    RequireRefusal(same_id.log.Confirm(pending), ErrorCode::VERIFY_FAILED, CONFIRM_UNKNOWN_SEQ, "same instance, no such record");
}

BOOST_AUTO_TEST_CASE(confirm_is_refused_for_a_record_type_of_another_role)
{
    // A signer journal and a commit journal that share instance id 5 (nothing
    // forces instance ids to be unique). The signer journal must not mint a
    // DurableDecision from its own barrier, or StateOwner::Apply, which checks
    // only the instance id, would accept a decision that is not durable.
    CommitFixture commit{5};
    const PendingDecision decision{commit.Decide(MakeStorageSlot(1, 0, "role"))};
    BOOST_CHECK_EQUAL(commit.journal->DurableSeq(), 0U);
    auto signer{NewMemoryJournal(JournalRole::SIGNER, 5)};
    RequireStorageOk(signer->Append(RecordType::INTENT, Bytes("intent")), "signer append");
    BOOST_CHECK_EQUAL(signer->Barrier().value(), 1U);
    RequireRefusal(signer->Confirm(decision), ErrorCode::VERIFY_FAILED, CONFIRM_ROLE, "a DECISION confirmed by the signer journal");

    // The other way round: a commit journal does not confirm a signer log's intent.
    auto signer_journal{NewMemoryJournal(JournalRole::SIGNER, 6)};
    SignerLog signer_log{SignerLog::Create(*signer_journal).value()};
    const StorageSlot slot{MakeStorageSlot(1, 0, "role-intent")};
    const PendingProposal proposal{signer_log.AppendAcceptedProposal(ProposalKey{0, 1, 0}, slot.header, slot.body).value()};
    const PendingIntent intent{signer_log.AppendIntent(CommitVoteKey(slot.header, 0), slot.header.CommitDigest(), proposal).value()};
    BOOST_CHECK_EQUAL(intent.Seq(), 2U);
    auto commit_journal{NewMemoryJournal(JournalRole::COMMIT, 6)};
    RequireStorageOk(commit_journal->Append(RecordType::DECISION, Bytes("one")), "commit append 1");
    RequireStorageOk(commit_journal->Append(RecordType::DECISION, Bytes("two")), "commit append 2");
    BOOST_CHECK_EQUAL(commit_journal->Barrier().value(), 2U);
    RequireRefusal(commit_journal->Confirm(intent), ErrorCode::VERIFY_FAILED, CONFIRM_ROLE, "an INTENT confirmed by the commit journal");
    // Each journal still confirms its own records.
    BOOST_CHECK_EQUAL(commit.log.Barrier().value(), 1U);
    RequireStorageOk(commit.log.Confirm(decision), "own DECISION");
    BOOST_CHECK_EQUAL(signer_log.Barrier().value(), 2U);
    RequireStorageOk(signer_log.Confirm(intent), "own INTENT");
}

BOOST_AUTO_TEST_CASE(token_binding_and_moves)
{
    CommitFixture f{4};
    const StorageSlot slot{MakeStorageSlot(7, 2, "bind")};
    const PendingDecision pending{f.Decide(slot)};
    BOOST_CHECK_EQUAL(pending.JournalInstance(), 4U);
    BOOST_CHECK_EQUAL(pending.Seq(), 1U);
    BOOST_CHECK(pending.Key() == (DecisionRef{7, slot.header.Hash(), slot.certificate.Hash()}));
    BOOST_CHECK_EQUAL(f.log.Barrier().value(), 1U);
    DurableDecision token{f.log.Confirm(pending).value()};
    BOOST_CHECK_EQUAL(token.JournalInstance(), 4U);
    BOOST_CHECK_EQUAL(token.Seq(), 1U);
    BOOST_CHECK_EQUAL(token.Slot(), 7U);
    BOOST_CHECK(token.Digest() == slot.header.Hash());
    BOOST_CHECK(token.Binding() == pending.Key());
    // A move hands the binding on; the source keeps it (a proof stays true).
    const DurableDecision moved{std::move(token)};
    BOOST_CHECK_EQUAL(moved.Seq(), 1U);
    BOOST_CHECK(moved.Binding() == pending.Key());
    BOOST_CHECK(token.Binding() == moved.Binding()); // NOLINT(bugprone-use-after-move): documented semantics
    // The applied token binds the state root as well. An APPLIED needs the
    // ApplyReceipt of a real Apply (O-92), so it comes from a state owner.
    OwnerFixture owner{12};
    const state::Candidate candidate{owner.Preview(owner.Inputs(FundingFacts(owner.actions), {}))};
    const DurableDecision decided{owner.Decide(candidate)};
    state::Candidate copy{candidate};
    const state::AppliedSlot applied_slot{owner.owner->Apply(std::move(copy), decided, owner.authority).value()};
    const PendingApplied applied{owner.log.AppendApplied(applied_slot.receipt).value()};
    BOOST_CHECK_EQUAL(owner.log.Barrier().value(), 2U);
    const DurableApplied applied_token{owner.log.Confirm(applied).value()};
    BOOST_CHECK(applied_token.Binding() == (AppliedRef{1, candidate.Header().Hash(), candidate.Header().state_root}));
    BOOST_CHECK_EQUAL(applied_token.Slot(), 1U);
    BOOST_CHECK(applied_token.Digest() == candidate.Header().Hash());
}

BOOST_AUTO_TEST_SUITE_END()
