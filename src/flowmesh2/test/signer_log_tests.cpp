// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/params.h>
#include <flowmesh2/storage/memory_journal.h>
#include <flowmesh2/storage/signer_log.h>
#include <flowmesh2/storage/tokens.h>
#include <flowmesh2/test/util/storage_fixtures.h>
#include <util/strencodings.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::storage;
using namespace flowmesh2::test;

namespace {

ProfileParams Profile() { return DevRegtestProfile(ChainType::REGTEST).value(); }

VoteSignature FakeSignature(unsigned char fill)
{
    VoteSignature sig{};
    sig.fill(fill);
    return sig;
}

//! A signer log over a fresh memory journal, with helpers for the strict order.
struct SignerFixture {
    std::unique_ptr<MemoryJournal> journal;
    SignerLog log;

    explicit SignerFixture(uint64_t instance, MemoryJournalFaults faults = {}, BarrierObserver* observer = nullptr)
        : journal{NewMemoryJournal(JournalRole::SIGNER, instance, faults, observer)}, log{SignerLog::Create(*journal).value()}
    {
    }

    PendingProposal Accept(const StorageSlot& slot, uint64_t view)
    {
        Result<PendingProposal> pending{log.AppendAcceptedProposal(ProposalKey{slot.header.authority_epoch, slot.header.height, view},
                                                                   slot.header, slot.body)};
        RequireStorageOk(pending, "AppendAcceptedProposal");
        return *pending;
    }

    PendingIntent Intend(const StorageSlot& slot, uint64_t view, const PendingProposal& proposal)
    {
        Result<PendingIntent> pending{log.AppendIntent(CommitVoteKey(slot.header, view), slot.header.CommitDigest(), proposal)};
        RequireStorageOk(pending, "AppendIntent");
        return *pending;
    }

    DurableIntent DurableIntentFor(const PendingIntent& pending)
    {
        RequireStorageOk(log.Barrier(), "B-INTENT");
        return log.Confirm(pending).value();
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(signer_log_tests)

BOOST_AUTO_TEST_CASE(create_checks_role_fence_and_history)
{
    auto commit{NewMemoryJournal(JournalRole::COMMIT, 1)};
    RequireRefusal(SignerLog::Create(*commit), ErrorCode::ORDER_VIOLATION, LOG_ROLE, "commit journal");
    auto fatal{NewMemoryJournal(JournalRole::SIGNER, 2, MemoryJournalFaults{.fail_barrier = 1})};
    RequireCode(fatal->Barrier(), ErrorCode::STORAGE_FATAL, "make the journal fatal");
    RequireRefusal(SignerLog::Create(*fatal), ErrorCode::FENCED, JOURNAL_FENCED, "fatal journal");
    auto used{NewMemoryJournal(JournalRole::SIGNER, 3)};
    RequireStorageOk(used->Append(RecordType::INTENT, Bytes("history")), "append");
    RequireRefusal(SignerLog::Create(*used), ErrorCode::ORDER_VIOLATION, LOG_PENDING_RECORDS, "pending history");
    RequireRefusal(SignerLog::Create(*used, Profile()), ErrorCode::ORDER_VIOLATION, LOG_PENDING_RECORDS, "pending replay");
    RequireStorageOk(used->Barrier(), "make malformed history durable");
    RequireRefusal(SignerLog::Create(*used), ErrorCode::INVALID_INPUT, LOG_REPLAY_PROFILE_REQUIRED, "history needs decoding profile");
    RequireCode(SignerLog::Create(*used, Profile()), ErrorCode::DECODE, "malformed durable history");
}

BOOST_AUTO_TEST_CASE(recovery_preserves_signed_and_unsigned_obligations)
{
    SignerFixture f{30};
    const StorageSlot slot{MakeStorageSlot(4, 2, "recovered")};
    const PendingProposal proposal{f.Accept(slot, 0)};
    const PendingIntent pending{f.Intend(slot, 0, proposal)};
    const DurableIntent intent{f.DurableIntentFor(pending)};
    const VoteSignature sig{FakeSignature(0x41)};
    const PendingSignedVote signed_pending{f.log.AppendSignedVote(intent, sig).value()};
    RequireStorageOk(f.log.Barrier(), "make signature durable");
    const VoteKey prepare{slot.header.domain, 2, 4, 0, VotePhase::PREPARE};
    const uint256 prepare_digest{LabelHash("fm2/test/storage/unsigned-prepare")};
    const PendingIntent unsigned_pending{f.log.AppendIntent(prepare, prepare_digest, proposal).value()};
    RequireStorageOk(f.log.Barrier(), "make unsigned obligation durable");
    const auto before{f.journal->DurableImage()};
    const JournalStatus before_status{f.journal->Status()};

    Result<SignerLog> created{SignerLog::Create(*f.journal, Profile())};
    RequireStorageOk(created, "rebuild signer indexes");
    SignerLog restored{std::move(created).value()};
    BOOST_CHECK(restored.AppendAcceptedProposal(ProposalKey{2, 4, 0}, slot.header, slot.body).value() == proposal);
    BOOST_CHECK(restored.AppendIntent(CommitVoteKey(slot.header, 0), slot.header.CommitDigest(), proposal).value() == pending);
    BOOST_CHECK(restored.Knows(intent));
    BOOST_CHECK(restored.AppendSignedVote(intent, sig).value() == signed_pending);
    BOOST_CHECK(restored.Confirm(signed_pending).value().Binding() == (SignedVoteRef{CommitVoteKey(slot.header, 0), slot.header.CommitDigest(), sig}));
    BOOST_CHECK(restored.AppendIntent(prepare, prepare_digest, proposal).value() == unsigned_pending);
    RequireRefusal(restored.AppendIntent(prepare, LabelHash("other"), proposal), ErrorCode::EQUIVOCATION_REFUSED,
                   VOTE_EQUIVOCATION, "unsigned reservation survives");
    RequireRefusal(restored.AppendSignedVote(intent, FakeSignature(0x42)), ErrorCode::EQUIVOCATION_REFUSED,
                   SIGNED_VOTE_CONFLICT, "exact signature survives");
    const StorageSlot conflict{MakeStorageSlot(4, 2, "recovered-conflict")};
    RequireRefusal(restored.AppendAcceptedProposal(ProposalKey{2, 4, 0}, conflict.header, conflict.body),
                   ErrorCode::EQUIVOCATION_REFUSED, PROPOSAL_CONFLICT, "accepted proposal survives");
    BOOST_CHECK(f.journal->DurableImage() == before);
    BOOST_CHECK(f.journal->Status() == before_status);
}

BOOST_AUTO_TEST_CASE(recovery_refuses_corrupt_bindings_and_conflicts)
{
    const StorageSlot slot{MakeStorageSlot(1, 0, "recovery-corrupt")};
    const ProposalKey proposal_key{0, 1, 0};
    const VoteKey vote_key{CommitVoteKey(slot.header, 0)};
    const auto accepted{AcceptedProposalRecord{proposal_key, slot.header, slot.body}.Encode()};
    const auto intent{IntentRecord{vote_key, slot.header.CommitDigest(), 1}.Encode()};
    uint64_t instance{40};
    const auto refuses = [&](const std::vector<std::pair<RecordType, std::vector<unsigned char>>>& records,
                             ErrorCode code, std::string_view detail) {
        auto journal{NewMemoryJournal(JournalRole::SIGNER, instance++)};
        for (const auto& [type, payload] : records) RequireStorageOk(journal->Append(type, payload), "raw corruption fixture");
        RequireStorageOk(journal->Barrier(), "durable corruption fixture");
        const auto before{journal->DurableImage()};
        RequireRefusal(SignerLog::Create(*journal, Profile()), code, detail, "corrupt signer history");
        BOOST_CHECK(journal->DurableImage() == before);
    };
    refuses({{RecordType::INTENT, intent}}, ErrorCode::INVALID_INPUT, INTENT_PROPOSAL_MISMATCH);
    refuses({{RecordType::ACCEPTED_PROPOSAL, accepted},
             {RecordType::INTENT, IntentRecord{vote_key, slot.header.CommitDigest(), 2}.Encode()}},
            ErrorCode::INVALID_INPUT, INTENT_PROPOSAL_MISMATCH);
    VoteKey wrong_domain{vote_key};
    wrong_domain.domain = LabelHash("other-domain");
    refuses({{RecordType::ACCEPTED_PROPOSAL, accepted},
             {RecordType::INTENT, IntentRecord{wrong_domain, slot.header.CommitDigest(), 1}.Encode()}},
            ErrorCode::INVALID_INPUT, INTENT_PROPOSAL_MISMATCH);
    refuses({{RecordType::ACCEPTED_PROPOSAL, accepted},
             {RecordType::INTENT, IntentRecord{vote_key, LabelHash("wrong-commit"), 1}.Encode()}},
            ErrorCode::INVALID_INPUT, INTENT_DIGEST_MISMATCH);
    refuses({{RecordType::ACCEPTED_PROPOSAL, accepted}, {RecordType::INTENT, intent},
             {RecordType::SIGNED_VOTE, SignedVoteRecord{vote_key, slot.header.CommitDigest(), FakeSignature(1), 1}.Encode()}},
            ErrorCode::VERIFY_FAILED, INTENT_UNKNOWN);
    refuses({{RecordType::SIGNED_VOTE, SignedVoteRecord{vote_key, slot.header.CommitDigest(), FakeSignature(1), 2}.Encode()}},
            ErrorCode::VERIFY_FAILED, INTENT_UNKNOWN);
    const auto signed_vote{SignedVoteRecord{vote_key, slot.header.CommitDigest(), FakeSignature(1), 2}.Encode()};
    refuses({{RecordType::ACCEPTED_PROPOSAL, accepted}, {RecordType::INTENT, intent}, {RecordType::SIGNED_VOTE, signed_vote},
             {RecordType::SIGNED_VOTE, SignedVoteRecord{vote_key, slot.header.CommitDigest(), FakeSignature(2), 2}.Encode()}},
            ErrorCode::EQUIVOCATION_REFUSED, SIGNED_VOTE_CONFLICT);
    const StorageSlot conflict{MakeStorageSlot(1, 0, "conflicting-proposal")};
    refuses({{RecordType::ACCEPTED_PROPOSAL, accepted},
             {RecordType::ACCEPTED_PROPOSAL, AcceptedProposalRecord{proposal_key, conflict.header, conflict.body}.Encode()}},
            ErrorCode::EQUIVOCATION_REFUSED, PROPOSAL_CONFLICT);
    VoteKey prepare{vote_key};
    prepare.phase = VotePhase::PREPARE;
    refuses({{RecordType::ACCEPTED_PROPOSAL, accepted},
             {RecordType::INTENT, IntentRecord{prepare, LabelHash("a"), 1}.Encode()},
             {RecordType::INTENT, IntentRecord{prepare, LabelHash("b"), 1}.Encode()}},
            ErrorCode::EQUIVOCATION_REFUSED, VOTE_EQUIVOCATION);
    SlotBody wrong_body{slot.body};
    wrong_body.originals.push_back(Bytes("extra"));
    refuses({{RecordType::ACCEPTED_PROPOSAL, AcceptedProposalRecord{proposal_key, slot.header, wrong_body}.Encode()}},
            ErrorCode::VERIFY_FAILED, BODY_HEADER_MISMATCH);
}

BOOST_AUTO_TEST_CASE(recovery_keeps_first_references_and_refuses_unsupported_records)
{
    SignerFixture f{60};
    const StorageSlot slot{MakeStorageSlot(1, 0, "duplicate-recovery")};
    const PendingProposal proposal{f.Accept(slot, 0)};
    const PendingIntent pending{f.Intend(slot, 0, proposal)};
    const DurableIntent intent{f.DurableIntentFor(pending)};
    const VoteSignature signature{FakeSignature(7)};
    const PendingSignedVote vote{f.log.AppendSignedVote(intent, signature).value()};
    RequireStorageOk(f.log.Barrier(), "durable original vote");
    RequireStorageOk(f.journal->Append(RecordType::ACCEPTED_PROPOSAL,
        AcceptedProposalRecord{ProposalKey{0, 1, 0}, slot.header, slot.body}.Encode()), "duplicate proposal");
    RequireStorageOk(f.journal->Append(RecordType::INTENT,
        IntentRecord{CommitVoteKey(slot.header, 0), slot.header.CommitDigest(), proposal.Seq()}.Encode()), "duplicate intent");
    RequireStorageOk(f.journal->Append(RecordType::SIGNED_VOTE,
        SignedVoteRecord{CommitVoteKey(slot.header, 0), slot.header.CommitDigest(), signature, pending.Seq()}.Encode()), "duplicate vote");
    RequireStorageOk(f.journal->Barrier(), "durable duplicate records");
    SignerLog restored{SignerLog::Create(*f.journal, Profile()).value()};
    BOOST_CHECK(restored.AppendAcceptedProposal(ProposalKey{0, 1, 0}, slot.header, slot.body).value() == proposal);
    BOOST_CHECK(restored.AppendIntent(CommitVoteKey(slot.header, 0), slot.header.CommitDigest(), proposal).value() == pending);
    BOOST_CHECK(restored.AppendSignedVote(intent, signature).value() == vote);
    for (const RecordType type : {RecordType::VIEW_STATE, RecordType::NEW_VIEW_ACCEPTED}) {
        auto journal{NewMemoryJournal(JournalRole::SIGNER, 61 + static_cast<uint8_t>(type))};
        RequireStorageOk(journal->Append(type, {}), "unsupported record");
        RequireStorageOk(journal->Barrier(), "unsupported record durable");
        RequireStub(SignerLog::Create(*journal, Profile()), "M4-STO-07");
    }
}

BOOST_AUTO_TEST_CASE(strict_order_writes_the_expected_records)
{
    // ACCEPTED_PROPOSAL, INTENT, B-INTENT, SIGNED_VOTE, B-VOTE: the observer
    // sees exactly that, and the records decode back to what was written.
    RecordingObserver observer;
    SignerFixture f{5, {}, &observer};
    const StorageSlot slot{MakeStorageSlot(4, 1, "strict")};
    const PendingProposal proposal{f.Accept(slot, 0)};
    BOOST_CHECK(proposal.Key() == (ProposalRef{ProposalKey{1, 4, 0}, slot.header.Hash()}));
    const PendingIntent pending{f.Intend(slot, 0, proposal)};
    BOOST_CHECK(pending.Key() == (VoteRef{CommitVoteKey(slot.header, 0), slot.header.CommitDigest()}));
    RequireRefusal(f.log.Confirm(pending), ErrorCode::ORDER_VIOLATION, CONFIRM_BEFORE_BARRIER, "intent before B-INTENT");
    const DurableIntent intent{f.DurableIntentFor(pending)};
    BOOST_CHECK_EQUAL(intent.Seq(), 2U);
    BOOST_CHECK_EQUAL(intent.Slot(), 4U);
    BOOST_CHECK_EQUAL(intent.View(), 0U);
    BOOST_CHECK(intent.Digest() == slot.header.CommitDigest());
    BOOST_CHECK(f.log.Knows(intent));

    const VoteSignature sig{FakeSignature(0x5a)};
    const PendingSignedVote signed_pending{f.log.AppendSignedVote(intent, sig).value()};
    RequireRefusal(f.log.Confirm(signed_pending), ErrorCode::ORDER_VIOLATION, CONFIRM_BEFORE_BARRIER, "vote before B-VOTE");
    BOOST_CHECK_EQUAL(f.log.Barrier().value(), 3U);
    const DurableSignedVote vote{f.log.Confirm(signed_pending).value()};
    BOOST_CHECK(vote.Binding() == (SignedVoteRef{CommitVoteKey(slot.header, 0), slot.header.CommitDigest(), sig}));

    const std::vector<std::string> expected{
        "append SIGNER 5 ACCEPTED_PROPOSAL 1", "append SIGNER 5 INTENT 2",       "barrier-start SIGNER 5 2",
        "barrier-end SIGNER 5 2 ok",          "append SIGNER 5 SIGNED_VOTE 3", "barrier-start SIGNER 5 3",
        "barrier-end SIGNER 5 3 ok"};
    BOOST_CHECK_EQUAL_COLLECTIONS(observer.events.begin(), observer.events.end(), expected.begin(), expected.end());

    const std::vector<JournalRecord> records{f.journal->ReadAll().value()};
    BOOST_REQUIRE_EQUAL(records.size(), 3U);
    const AcceptedProposalRecord accepted{AcceptedProposalRecord::Decode(records[0].payload, Profile()).value()};
    BOOST_CHECK(accepted == (AcceptedProposalRecord{ProposalKey{1, 4, 0}, slot.header, slot.body}));
    const IntentRecord intent_record{IntentRecord::Decode(records[1].payload).value()};
    BOOST_CHECK(intent_record == (IntentRecord{CommitVoteKey(slot.header, 0), slot.header.CommitDigest(), 1}));
    const SignedVoteRecord vote_record{SignedVoteRecord::Decode(records[2].payload).value()};
    BOOST_CHECK(vote_record == (SignedVoteRecord{CommitVoteKey(slot.header, 0), slot.header.CommitDigest(), sig, 2}));
}

BOOST_AUTO_TEST_CASE(equivocation_is_refused)
{
    SignerFixture f{6};
    const StorageSlot slot{MakeStorageSlot(9, 3, "e")};
    const PendingProposal proposal{f.Accept(slot, 1)};
    const VoteKey key{CommitVoteKey(slot.header, 1)};
    const PendingIntent first{f.Intend(slot, 1, proposal)};
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, 2U);

    // The identical intent is idempotent: same pending, no new record.
    const Result<PendingIntent> again{f.log.AppendIntent(key, slot.header.CommitDigest(), proposal)};
    RequireStorageOk(again, "identical intent");
    BOOST_CHECK(*again == first);
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, 2U);

    // Another digest under the same key is refused, PREPARE-phase or not.
    const uint256 other{LabelHash("fm2/test/storage/other-digest")};
    VoteKey prepare_key{key};
    prepare_key.phase = VotePhase::PREPARE;
    RequireStorageOk(f.log.AppendIntent(prepare_key, other, proposal), "PREPARE intent for its own key");
    RequireRefusal(f.log.AppendIntent(prepare_key, slot.header.CommitDigest(), proposal), ErrorCode::EQUIVOCATION_REFUSED,
                   VOTE_EQUIVOCATION, "second PREPARE digest");
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, 3U);

    // A COMMIT intent must sign the proposal's CommitDigest.
    RequireRefusal(f.log.AppendIntent(key, other, proposal), ErrorCode::INVALID_INPUT, INTENT_DIGEST_MISMATCH, "COMMIT digest of another header");

    // A new view is a new key: a re-vote after a view change is allowed,
    // for the proposal accepted in that view.
    const StorageSlot reproposed{MakeStorageSlot(9, 3, "e-view2")};
    const PendingProposal proposal2{f.Accept(reproposed, 2)};
    const PendingIntent second{f.Intend(reproposed, 2, proposal2)};
    BOOST_CHECK(second.Seq() > first.Seq());
    BOOST_CHECK(second.Key().digest != first.Key().digest);
    // But the old view's key still maps to the old digest only.
    RequireRefusal(f.log.AppendIntent(key, reproposed.header.CommitDigest(), proposal), ErrorCode::INVALID_INPUT,
                   INTENT_DIGEST_MISMATCH, "other digest under the old proposal");
    RequireRefusal(f.log.AppendIntent(key, reproposed.header.CommitDigest(), proposal2), ErrorCode::INVALID_INPUT,
                   INTENT_PROPOSAL_MISMATCH, "old key with the new view's proposal");
}

BOOST_AUTO_TEST_CASE(proposal_rules)
{
    SignerFixture f{7};
    const StorageSlot slot{MakeStorageSlot(3, 1, "p")};
    // Key and header must agree on epoch and slot.
    RequireRefusal(f.log.AppendAcceptedProposal(ProposalKey{2, 3, 0}, slot.header, slot.body), ErrorCode::INVALID_INPUT,
                   PROPOSAL_KEY_MISMATCH, "epoch mismatch");
    RequireRefusal(f.log.AppendAcceptedProposal(ProposalKey{1, 4, 0}, slot.header, slot.body), ErrorCode::INVALID_INPUT,
                   PROPOSAL_KEY_MISMATCH, "slot mismatch");
    // The body must be the header's.
    SlotBody wrong_body{slot.body};
    wrong_body.originals.push_back(Bytes("extra"));
    RequireRefusal(f.log.AppendAcceptedProposal(ProposalKey{1, 3, 0}, slot.header, wrong_body), ErrorCode::VERIFY_FAILED,
                   BODY_HEADER_MISMATCH, "body of another header");
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, 0U);
    // One proposal per key: identical is idempotent, another header is refused.
    const PendingProposal first{f.Accept(slot, 0)};
    BOOST_CHECK(f.Accept(slot, 0) == first);
    const StorageSlot other{MakeStorageSlot(3, 1, "p-other")};
    RequireRefusal(f.log.AppendAcceptedProposal(ProposalKey{1, 3, 0}, other.header, other.body), ErrorCode::EQUIVOCATION_REFUSED,
                   PROPOSAL_CONFLICT, "second proposal for one key");
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, 1U);
    // Intent binding to its proposal: epoch, slot, view and domain.
    VoteKey key{CommitVoteKey(slot.header, 0)};
    key.view = 1;
    RequireRefusal(f.log.AppendIntent(key, slot.header.CommitDigest(), first), ErrorCode::INVALID_INPUT, INTENT_PROPOSAL_MISMATCH, "view");
    key = CommitVoteKey(slot.header, 0);
    key.domain = LabelHash("fm2/test/storage/other-domain");
    RequireRefusal(f.log.AppendIntent(key, slot.header.CommitDigest(), first), ErrorCode::INVALID_INPUT, INTENT_PROPOSAL_MISMATCH, "domain");
    key = CommitVoteKey(slot.header, 0);
    key.phase = VotePhase{7};
    RequireRefusal(f.log.AppendIntent(key, slot.header.CommitDigest(), first), ErrorCode::INVALID_INPUT, VOTE_PHASE, "unknown phase");
    // A proposal pending of another signer log is refused.
    SignerFixture g{8};
    const PendingProposal foreign{g.Accept(slot, 0)};
    RequireRefusal(f.log.AppendIntent(CommitVoteKey(slot.header, 0), slot.header.CommitDigest(), foreign), ErrorCode::VERIFY_FAILED,
                   TOKEN_OTHER_LOG, "proposal of another log");
}

BOOST_AUTO_TEST_CASE(signed_vote_rules)
{
    SignerFixture f{9};
    const StorageSlot slot{MakeStorageSlot(2, 0, "v")};
    const DurableIntent intent{f.DurableIntentFor(f.Intend(slot, 0, f.Accept(slot, 0)))};
    const VoteSignature sig{FakeSignature(1)};
    const PendingSignedVote first{f.log.AppendSignedVote(intent, sig).value()};
    // The same signature again is idempotent; another one is refused.
    BOOST_CHECK(f.log.AppendSignedVote(intent, sig).value() == first);
    RequireRefusal(f.log.AppendSignedVote(intent, FakeSignature(2)), ErrorCode::EQUIVOCATION_REFUSED, SIGNED_VOTE_CONFLICT, "second signature");
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, 3U);
    // An intent token of another signer log is refused, and unknown there.
    SignerFixture g{10};
    RequireRefusal(g.log.AppendSignedVote(intent, sig), ErrorCode::VERIFY_FAILED, TOKEN_OTHER_LOG, "intent of another log");
    BOOST_CHECK(!g.log.Knows(intent));
    // A log over another journal with the same instance id does not know it either.
    SignerFixture same_id{9};
    RequireRefusal(same_id.log.AppendSignedVote(intent, sig), ErrorCode::VERIFY_FAILED, INTENT_UNKNOWN, "same instance id, other log");
}

BOOST_AUTO_TEST_CASE(fatal_barrier_fences_the_signer)
{
    // B-INTENT fails: no DurableIntent can be minted, so nothing can be signed.
    SignerFixture f{11, MemoryJournalFaults{.fail_barrier = 1}};
    const StorageSlot slot{MakeStorageSlot(5, 0, "fatal")};
    const PendingProposal proposal{f.Accept(slot, 0)};
    const PendingIntent pending{f.Intend(slot, 0, proposal)};
    RequireRefusal(f.log.Barrier(), ErrorCode::STORAGE_FATAL, MEMORY_JOURNAL_BARRIER_FAULT, "B-INTENT");
    BOOST_CHECK(f.log.IsFatal());
    RequireRefusal(f.log.Confirm(pending), ErrorCode::FENCED, JOURNAL_FENCED, "confirm after fatal");
    RequireRefusal(f.log.AppendIntent(CommitVoteKey(slot.header, 0), slot.header.CommitDigest(), proposal), ErrorCode::FENCED,
                   JOURNAL_FENCED, "intent after fatal");
    RequireRefusal(f.log.AppendAcceptedProposal(ProposalKey{0, 6, 0}, MakeStorageSlot(6, 0, "fatal").header, MakeStorageSlot(6, 0, "fatal").body),
                   ErrorCode::FENCED, JOURNAL_FENCED, "proposal after fatal");
    RequireRefusal(f.log.Barrier(), ErrorCode::STORAGE_FATAL, JOURNAL_FATAL, "barrier after fatal");
}

BOOST_AUTO_TEST_CASE(record_codecs_golden_and_exact)
{
    // Goldens from evidence/R8/golden.py.
    const uint256 domain{LabelHash("fm2/test/domain")};
    const uint256 digest{LabelHash("fm2/test/digest")};
    const VoteKey key{domain, 3, 9, 1, VotePhase::COMMIT};
    const IntentRecord intent{key, digest, 5};
    BOOST_CHECK_EQUAL(HexStr(intent.Encode()),
                      "4f9cfaa7394620a6c3a45dac67ecb79cee9af65b59dbcd3095205bb5bed63a7c030000000000000009000000000000000100000000000000"
                      "02010f8bafcab50ee72d5b89c0b146e0d5be413be57848c7e780446eb2190bb8f80500000000000000");
    BOOST_CHECK_EQUAL(intent.Encode().size(), INTENT_RECORD_BYTES);
    BOOST_CHECK(IntentRecord::Decode(intent.Encode()).value() == intent);
    VoteSignature sig{};
    for (size_t i{0}; i < sig.size(); ++i) sig[i] = static_cast<unsigned char>(i);
    const SignedVoteRecord vote{key, digest, sig, 6};
    BOOST_CHECK_EQUAL(HexStr(vote.Encode()),
                      "4f9cfaa7394620a6c3a45dac67ecb79cee9af65b59dbcd3095205bb5bed63a7c030000000000000009000000000000000100000000000000"
                      "02010f8bafcab50ee72d5b89c0b146e0d5be413be57848c7e780446eb2190bb8f8000102030405060708090a0b0c0d0e0f1011121314151617"
                      "18191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f50"
                      "5152535455565758595a5b5c5d5e5f0600000000000000");
    BOOST_CHECK_EQUAL(vote.Encode().size(), SIGNED_VOTE_RECORD_BYTES);
    BOOST_CHECK(SignedVoteRecord::Decode(vote.Encode()).value() == vote);

    // Exactness: a trailing byte, a short input and an unknown phase are refused.
    std::vector<unsigned char> bytes{intent.Encode()};
    bytes.push_back(0);
    RequireCode(IntentRecord::Decode(bytes), ErrorCode::OVERSIZE, "intent + 1 byte");
    bytes.resize(INTENT_RECORD_BYTES - 1);
    RequireCode(IntentRecord::Decode(bytes), ErrorCode::DECODE, "intent - 1 byte");
    bytes = intent.Encode();
    bytes[VOTE_KEY_BYTES - 1] = 3; // phase byte
    RequireCode(IntentRecord::Decode(bytes), ErrorCode::DECODE, "unknown phase");
    std::vector<unsigned char> vote_bytes{vote.Encode()};
    vote_bytes.pop_back();
    RequireCode(SignedVoteRecord::Decode(vote_bytes), ErrorCode::DECODE, "short vote");

    // ACCEPTED_PROPOSAL round trip and refusals.
    const StorageSlot slot{MakeStorageSlot(4, 1, "codec")};
    const AcceptedProposalRecord accepted{ProposalKey{1, 4, 2}, slot.header, slot.body};
    BOOST_CHECK(AcceptedProposalRecord::Decode(accepted.Encode(), Profile()).value() == accepted);
    std::vector<unsigned char> accepted_bytes{accepted.Encode()};
    accepted_bytes.push_back(0);
    RequireCode(AcceptedProposalRecord::Decode(accepted_bytes, Profile()), ErrorCode::DECODE, "trailing byte in the body");
    accepted_bytes.resize(24 + SLOT_HEADER_BYTES - 1);
    RequireCode(AcceptedProposalRecord::Decode(accepted_bytes, Profile()), ErrorCode::DECODE, "short proposal");
    const std::vector<unsigned char> huge(AcceptedProposalRecord::MaxEncodedSize(Profile()) + 1);
    RequireCode(AcceptedProposalRecord::Decode(huge, Profile()), ErrorCode::OVERSIZE, "proposal above its maximum");
    BOOST_CHECK(AcceptedProposalRecord::MaxEncodedSize(Profile()) <= MAX_RECORD_PAYLOAD_BYTES);
}

BOOST_AUTO_TEST_CASE(stubs_return_their_task_ids)
{
    SignerFixture f{12};
    const StorageSlot slot{MakeStorageSlot(1, 0, "stub")};
    const PendingProposal proposal{f.Accept(slot, 0)};
    RequireStub(f.log.AppendSignedFolded(CommitVoteKey(slot.header, 0), slot.header.CommitDigest(), proposal, FakeSignature(3)), "M4-STO-05");
    RequireStub(f.log.AppendViewState(ViewStateRecord{0, 1}), "M4-STO-07");
    RequireStub(f.log.AppendNewViewAccepted(NewViewAcceptedRecord{0, 1, Bytes("nv")}), "M4-STO-07");
    // The stubs wrote nothing.
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, 1U);
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

/** The answers AcceptedAt gave before it was indexed, from one scan of the
 * durable journal: the first ACCEPTED_PROPOSAL record of each key.
 */
std::map<ProposalKey, AcceptedProposalRecord> ScanAccepted(const Journal& journal, const ProfileParams& profile)
{
    std::map<ProposalKey, AcceptedProposalRecord> first;
    const auto records{journal.ReadAll()};
    RequireStorageOk(records, "scan the durable journal");
    for (const auto& record : *records) {
        if (record.type != RecordType::ACCEPTED_PROPOSAL) continue;
        auto proposal{AcceptedProposalRecord::Decode(record.payload, profile)};
        RequireStorageOk(proposal, "decode a durable proposal");
        first.emplace(proposal->key, std::move(*proposal));
    }
    return first;
}

//! "" when the log answers `key` as the scan does, byte for byte.
std::string SameAnswer(const SignerLog& log, const std::map<ProposalKey, AcceptedProposalRecord>& scan, const ProposalKey& key,
                       const ProfileParams& profile)
{
    const auto got{log.AcceptedAt(key, profile)};
    if (!got.has_value()) return "lookup failed: " + std::string{got.error().detail};
    const auto want{scan.find(key)};
    if (got->has_value() != (want != scan.end())) return got->has_value() ? "an answer the scan does not have" : "a missing answer";
    if (got->has_value() && (*got)->Encode() != want->second.Encode()) return "other bytes";
    return "";
}

using AcceptedClock = std::chrono::steady_clock;

//! Fastest of `reps` mean AcceptedAt times, in microseconds, of the newest of `slots` durable proposals.
double TimeAcceptedAt(uint64_t slots, int reps)
{
    SignerFixture f{70};
    for (uint64_t height{1}; height <= slots; ++height) f.Accept(MakeStorageSlot(height, 0, "cost/" + std::to_string(height)), 0);
    RequireStorageOk(f.log.Barrier(), "make the proposals durable");
    const ProposalKey newest{0, slots, 0};
    const ProfileParams profile{Profile()};
    BOOST_REQUIRE(f.log.AcceptedAt(newest, profile).value().has_value());
    double best{1e300};
    for (int rep{0}; rep < reps; ++rep) {
        constexpr int CALLS{50};
        const auto start{AcceptedClock::now()};
        for (int call{0}; call < CALLS; ++call) {
            const auto found{f.log.AcceptedAt(newest, profile)};
            if (!found.has_value() || !found->has_value()) BOOST_FAIL("AcceptedAt lost the newest proposal");
        }
        const std::chrono::duration<double, std::micro> took{AcceptedClock::now() - start};
        best = std::min(best, took.count() / CALLS);
    }
    return best;
}

} // namespace

BOOST_AUTO_TEST_SUITE(signer_log_accepted_tests)

BOOST_AUTO_TEST_CASE(accepted_at_costs_the_same_with_two_thousand_slots_as_with_a_hundred)
{
    // Every agreement step of the regtest PBFT looks up the proposal it
    // accepted (agreement/regtest_pbft, dev/regtest_operator); a scan of the
    // journal made each step slower with every slot signed (latency report).
    const double small{TimeAcceptedAt(100, 5)};
    const double large{TimeAcceptedAt(2'000, 5)};
    BOOST_TEST_MESSAGE("AcceptedAt of the newest proposal: " << small << " us at 100 slots, " << large << " us at 2000 slots, ratio "
                                                             << large / small);
    BOOST_CHECK_LT(large / small, 5.0);
}

BOOST_AUTO_TEST_CASE(accepted_at_answers_like_a_scan_of_the_durable_journal)
{
    // 400 slots, a few in two or three views; barriers now and then, so that
    // some records are durable and some only appended; a repeated accept;
    // and keys that were never accepted. Every answer must be the scan's,
    // byte for byte, before and after a rebuild from the journal.
    // Thinned to a checkpoint every 50 slots to keep the sanitizer time low.
    SignerFixture f{71};
    const ProfileParams profile{Profile()};
    std::vector<ProposalKey> keys;
    uint64_t state{0x4acc};
    const auto next{[&state] {
        state = state * 6'364'136'223'846'793'005ULL + 1'442'695'040'888'963'407ULL;
        return state >> 33;
    }};
    size_t durable_answers{0}, pending_answers{0};
    for (uint64_t height{1}; height <= 400; ++height) {
        const uint64_t views{next() % 8 == 0 ? 1 + next() % 3 : 1};
        for (uint64_t view{0}; view < views; ++view) {
            const StorageSlot slot{MakeStorageSlot(height, 0, "scan/" + std::to_string(height) + "/" + std::to_string(view))};
            const PendingProposal first{f.Accept(slot, view)};
            if (next() % 16 == 0) BOOST_REQUIRE(f.Accept(slot, view) == first); // the repeat keeps the first record
            keys.push_back(ProposalKey{0, height, view});
        }
        if (next() % 5 == 0) RequireStorageOk(f.log.Barrier(), "barrier");
        if (height % 50 == 0) {
            const auto scan{ScanAccepted(*f.journal, profile)};
            for (const ProposalKey& key : keys) {
                const std::string difference{SameAnswer(f.log, scan, key, profile)};
                if (!difference.empty()) BOOST_FAIL("height " << height << " key " << key.slot << "/" << key.view << ": " << difference);
                if (scan.contains(key)) {
                    ++durable_answers;
                } else {
                    ++pending_answers;
                }
            }
        }
    }
    const std::vector<ProposalKey> absent{{0, 401, 0}, {0, 1, 7}, {1, 1, 0}, {0, 0, 0}};
    for (const ProposalKey& key : absent) BOOST_CHECK(!f.log.AcceptedAt(key, profile).value().has_value());
    BOOST_TEST_MESSAGE("answers compared: " << durable_answers << " durable, " << pending_answers << " not yet durable");
    BOOST_CHECK_GT(durable_answers, 1'000U);
    BOOST_CHECK_GT(pending_answers, 10U);

    // A log rebuilt from the journal answers the same.
    RequireStorageOk(f.log.Barrier(), "last barrier");
    Result<SignerLog> created{SignerLog::Create(*f.journal, profile)};
    RequireStorageOk(created, "rebuild");
    const SignerLog restored{std::move(created).value()};
    const auto scan{ScanAccepted(*f.journal, profile)};
    BOOST_CHECK_EQUAL(scan.size(), keys.size());
    for (const ProposalKey& key : keys) {
        BOOST_REQUIRE_EQUAL(SameAnswer(f.log, scan, key, profile), "");
        BOOST_REQUIRE_EQUAL(SameAnswer(restored, scan, key, profile), "");
    }
}

BOOST_AUTO_TEST_CASE(accepted_at_after_a_fatal_barrier_is_fenced)
{
    SignerFixture f{72, MemoryJournalFaults{.fail_barrier = 2}};
    const StorageSlot slot{MakeStorageSlot(1, 0, "fenced")};
    f.Accept(slot, 0);
    RequireStorageOk(f.log.Barrier(), "first barrier");
    const ProfileParams profile{Profile()};
    BOOST_REQUIRE(f.log.AcceptedAt(ProposalKey{0, 1, 0}, profile).value().has_value());
    f.Accept(MakeStorageSlot(2, 0, "fenced/2"), 0);
    RequireRefusal(f.log.Barrier(), ErrorCode::STORAGE_FATAL, MEMORY_JOURNAL_BARRIER_FAULT, "second barrier");
    RequireRefusal(f.log.AcceptedAt(ProposalKey{0, 1, 0}, profile), ErrorCode::FENCED, JOURNAL_FENCED, "lookup after fatal");
}

BOOST_AUTO_TEST_SUITE_END()
