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

#include <cstdint>
#include <memory>
#include <string>
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
    RequireStub(SignerLog::Create(*used), "M4-STO-01");
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
