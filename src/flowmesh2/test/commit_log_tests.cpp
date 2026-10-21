// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/params.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/memory_journal.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/state/state_owner.h>
#include <flowmesh2/test/util/state_fixtures.h>
#include <flowmesh2/test/util/storage_fixtures.h>
#include <util/strencodings.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::storage;
using namespace flowmesh2::test;

namespace {

ProfileParams Profile() { return DevRegtestProfile(ChainType::REGTEST).value(); }

struct CommitFixture {
    std::unique_ptr<MemoryJournal> journal;
    CommitLog log;

    explicit CommitFixture(uint64_t instance, MemoryJournalFaults faults = {})
        : journal{NewMemoryJournal(JournalRole::COMMIT, instance, faults)}, log{CommitLog::Create(*journal).value()}
    {
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(commit_log_tests)

BOOST_AUTO_TEST_CASE(create_checks_role_fence_and_history)
{
    auto signer{NewMemoryJournal(JournalRole::SIGNER, 1)};
    RequireRefusal(CommitLog::Create(*signer), ErrorCode::ORDER_VIOLATION, LOG_ROLE, "signer journal");
    auto fatal{NewMemoryJournal(JournalRole::COMMIT, 2, MemoryJournalFaults{.fail_barrier = 1})};
    RequireCode(fatal->Barrier(), ErrorCode::STORAGE_FATAL, "make the journal fatal");
    RequireRefusal(CommitLog::Create(*fatal), ErrorCode::FENCED, JOURNAL_FENCED, "fatal journal");
    auto used{NewMemoryJournal(JournalRole::COMMIT, 3)};
    RequireStorageOk(used->Append(RecordType::DECISION, Bytes("history")), "append");
    RequireRefusal(CommitLog::Create(*used), ErrorCode::ORDER_VIOLATION, LOG_PENDING_RECORDS, "pending history");
    RequireRefusal(CommitLog::Create(*used, Profile()), ErrorCode::ORDER_VIOLATION, LOG_PENDING_RECORDS, "pending replay");
    RequireStorageOk(used->Barrier(), "make malformed history durable");
    RequireRefusal(CommitLog::Create(*used), ErrorCode::INVALID_INPUT, LOG_REPLAY_PROFILE_REQUIRED, "history needs decoding profile");
    RequireCode(CommitLog::Create(*used, Profile()), ErrorCode::DECODE, "malformed durable history");
}

BOOST_AUTO_TEST_CASE(recovery_restores_decisions_applied_order_and_original_tokens)
{
    OwnerFixture f{30};
    state::Candidate candidate{f.Preview(f.Inputs(FundingFacts(f.actions), {}))};
    const SlotHeaderV2 header{candidate.Header()};
    const SlotCertificate cert{TestCertificate(header, 1)};
    const SlotBody body{candidate.Body()};
    const PendingDecision pending{f.log.AppendDecision(header, cert, body).value()};
    RequireStorageOk(f.log.Barrier(), "durable first decision");
    const DurableDecision decision{f.log.Confirm(pending).value()};
    const state::AppliedSlot applied{f.owner->Apply(std::move(candidate), decision, f.authority).value()};
    const PendingApplied applied_pending{f.log.AppendApplied(applied.receipt).value()};
    RequireStorageOk(f.log.Barrier(), "durable first applied");
    f.Sync();
    state::Candidate next{f.Preview(f.Inputs({}, {}))};
    const SlotHeaderV2 next_header{next.Header()};
    const SlotBody next_body{next.Body()};
    const DurableDecision next_decision{f.Decide(next)};
    const state::AppliedSlot next_applied{f.owner->Apply(std::move(next), next_decision, f.authority).value()};
    const auto before{f.journal->DurableImage()};
    const JournalStatus before_status{f.journal->Status()};

    Result<CommitLog> created{CommitLog::Create(*f.journal, Profile())};
    RequireStorageOk(created, "rebuild commit indexes");
    CommitLog restored{std::move(created).value()};
    SlotCertificate other_cert{cert};
    other_cert.aggregate.fill(2);
    BOOST_CHECK(restored.AppendDecision(header, other_cert, body).value() == pending);
    BOOST_CHECK(restored.Confirm(pending).value().Binding() == decision.Binding());
    BOOST_CHECK(restored.AppendApplied(applied.receipt).value() == applied_pending);
    BOOST_CHECK(restored.DecisionAt(next_header.height) == std::optional<DecisionRef>{next_decision.Binding()});
    BOOST_CHECK(f.journal->DurableImage() == before);
    BOOST_CHECK(f.journal->Status() == before_status);
    SlotHeaderV2 conflicting{next_header};
    conflicting.state_root = LabelHash("other-decided-root");
    RequireRefusal(restored.AppendDecision(conflicting, TestCertificate(conflicting), next_body),
                   ErrorCode::EQUIVOCATION_REFUSED, DECISION_CONFLICT, "decision without APPLIED still reserves slot");
    const PendingApplied recovered_next{restored.AppendApplied(next_applied.receipt).value()};
    BOOST_CHECK_EQUAL(recovered_next.Seq(), before_status.appended_seq + 1);
    RequireStorageOk(restored.Barrier(), "finish next APPLIED");
    RequireStorageOk(restored.Confirm(recovered_next), "confirm recovered append");
    CommitLog restored_again{CommitLog::Create(*f.journal, Profile()).value()};
    BOOST_CHECK(restored_again.AppendApplied(next_applied.receipt).value() == recovered_next);
}

BOOST_AUTO_TEST_CASE(recovery_refuses_corrupt_decisions_and_application_history)
{
    const StorageSlot slot{MakeStorageSlot(1, 0, "recovery-corrupt")};
    const auto decision{DecisionRecord{slot.header, slot.certificate, slot.body}.Encode()};
    const AppliedRecord applied{1, slot.header.Hash(), slot.header.state_root};
    uint64_t instance{40};
    const auto refuses = [&](const std::vector<std::pair<RecordType, std::vector<unsigned char>>>& records,
                             ErrorCode code, std::string_view detail) {
        auto journal{NewMemoryJournal(JournalRole::COMMIT, instance++)};
        for (const auto& [type, payload] : records) RequireStorageOk(journal->Append(type, payload), "raw corruption fixture");
        RequireStorageOk(journal->Barrier(), "durable corruption fixture");
        const auto before{journal->DurableImage()};
        RequireRefusal(CommitLog::Create(*journal, Profile()), code, detail, "corrupt commit history");
        BOOST_CHECK(journal->DurableImage() == before);
    };
    refuses({{RecordType::APPLIED, applied.Encode()}}, ErrorCode::VERIFY_FAILED, APPLIED_DECISION_UNKNOWN);
    refuses({{RecordType::APPLIED, applied.Encode()}, {RecordType::DECISION, decision}},
            ErrorCode::VERIFY_FAILED, APPLIED_DECISION_UNKNOWN);
    AppliedRecord bad_root{applied};
    bad_root.state_root = LabelHash("wrong-state-root");
    refuses({{RecordType::DECISION, decision}, {RecordType::APPLIED, bad_root.Encode()}},
            ErrorCode::VERIFY_FAILED, APPLIED_ROOT_MISMATCH);
    AppliedRecord bad_hash{applied};
    bad_hash.header_hash = LabelHash("wrong-header");
    refuses({{RecordType::DECISION, decision}, {RecordType::APPLIED, bad_hash.Encode()}},
            ErrorCode::VERIFY_FAILED, APPLIED_DECISION_UNKNOWN);
    SlotCertificate bad_cert{slot.certificate};
    bad_cert.header_hash = LabelHash("wrong-certified-header");
    refuses({{RecordType::DECISION, DecisionRecord{slot.header, bad_cert, slot.body}.Encode()}},
            ErrorCode::VERIFY_FAILED, DECISION_CERT_HEADER);
    SlotBody bad_body{slot.body};
    bad_body.originals.push_back(Bytes("extra"));
    refuses({{RecordType::DECISION, DecisionRecord{slot.header, slot.certificate, bad_body}.Encode()}},
            ErrorCode::VERIFY_FAILED, BODY_HEADER_MISMATCH);
    const StorageSlot conflict{MakeStorageSlot(1, 0, "conflicting-decision")};
    refuses({{RecordType::DECISION, decision},
             {RecordType::DECISION, DecisionRecord{conflict.header, conflict.certificate, conflict.body}.Encode()}},
            ErrorCode::EQUIVOCATION_REFUSED, DECISION_CONFLICT);
    const StorageSlot second{MakeStorageSlot(2, 0, "second-slot")};
    refuses({{RecordType::DECISION, DecisionRecord{second.header, second.certificate, second.body}.Encode()},
             {RecordType::APPLIED, AppliedRecord{2, second.header.Hash(), second.header.state_root}.Encode()}},
            ErrorCode::ORDER_VIOLATION, APPLIED_OUT_OF_ORDER);
    const StorageSlot zero{MakeStorageSlot(0, 0, "zero-slot")};
    refuses({{RecordType::DECISION, DecisionRecord{zero.header, zero.certificate, zero.body}.Encode()},
             {RecordType::APPLIED, AppliedRecord{0, zero.header.Hash(), zero.header.state_root}.Encode()}},
            ErrorCode::ORDER_VIOLATION, APPLIED_OUT_OF_ORDER);
}

BOOST_AUTO_TEST_CASE(recovery_keeps_first_certificate_and_refuses_unsupported_records)
{
    CommitFixture f{60};
    const StorageSlot slot{MakeStorageSlot(1, 0, "duplicate-recovery")};
    const PendingDecision first{f.log.AppendDecision(slot.header, slot.certificate, slot.body).value()};
    RequireStorageOk(f.log.Barrier(), "durable first decision");
    SlotCertificate other_cert{slot.certificate};
    other_cert.aggregate.fill(3);
    RequireStorageOk(f.journal->Append(RecordType::DECISION,
        DecisionRecord{slot.header, other_cert, slot.body}.Encode()), "duplicate decision with other certificate");
    const auto applied{AppliedRecord{1, slot.header.Hash(), slot.header.state_root}.Encode()};
    RequireStorageOk(f.journal->Append(RecordType::APPLIED, applied), "first applied");
    RequireStorageOk(f.journal->Append(RecordType::APPLIED, applied), "duplicate applied");
    RequireStorageOk(f.journal->Barrier(), "durable duplicate history");
    CommitLog restored{CommitLog::Create(*f.journal, Profile()).value()};
    BOOST_CHECK(restored.AppendDecision(slot.header, other_cert, slot.body).value() == first);
    BOOST_CHECK(restored.DecisionAt(1) == std::optional<DecisionRef>{first.Key()});
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, 4U);
    for (const RecordType type : {RecordType::SNAPSHOT_MARK, RecordType::PREPARED_QC}) {
        auto journal{NewMemoryJournal(JournalRole::COMMIT, 61 + static_cast<uint8_t>(type))};
        RequireStorageOk(journal->Append(type, {}), "unsupported record");
        RequireStorageOk(journal->Barrier(), "unsupported record durable");
        if (type == RecordType::SNAPSHOT_MARK) RequireStub(CommitLog::Create(*journal, Profile()), "M4-STO-03");
        else RequireCode(CommitLog::Create(*journal, Profile()), ErrorCode::DECODE, "malformed prepared record");
    }
}

BOOST_AUTO_TEST_CASE(decision_then_applied)
{
    // DECISION, B-DECISION, Confirm; Apply (check 3) gives the ApplyReceipt;
    // APPLIED, B-APPLIED, Confirm (O-92).
    OwnerFixture f{4};
    const state::Candidate candidate{f.Preview(f.Inputs(FundingFacts(f.actions), {}))};
    const SlotHeaderV2 header{candidate.Header()};
    const SlotCertificate cert{TestCertificate(header)};
    const PendingDecision pending{f.log.AppendDecision(header, cert, candidate.Body()).value()};
    RequireRefusal(f.log.Confirm(pending), ErrorCode::ORDER_VIOLATION, CONFIRM_BEFORE_BARRIER, "decision before B-DECISION");
    BOOST_CHECK_EQUAL(f.log.Barrier().value(), 1U);
    const DurableDecision decision{f.log.Confirm(pending).value()};
    state::Candidate copy{candidate};
    const state::AppliedSlot slot{f.owner->Apply(std::move(copy), decision, f.authority).value()};
    const PendingApplied applied{f.log.AppendApplied(slot.receipt).value()};
    RequireRefusal(f.log.Confirm(applied), ErrorCode::ORDER_VIOLATION, CONFIRM_BEFORE_BARRIER, "applied before B-APPLIED");
    BOOST_CHECK_EQUAL(f.log.Barrier().value(), 2U);
    const DurableApplied token{f.log.Confirm(applied).value()};
    BOOST_CHECK(token.Binding() == (AppliedRef{1, header.Hash(), header.state_root}));

    // The records decode back: the DECISION carries header, certificate and body (O-16).
    const std::vector<JournalRecord> records{f.journal->ReadAll().value()};
    BOOST_REQUIRE_EQUAL(records.size(), 2U);
    BOOST_CHECK(records[0].type == RecordType::DECISION && records[1].type == RecordType::APPLIED);
    const DecisionRecord decided{DecisionRecord::Decode(records[0].payload, Profile()).value()};
    BOOST_CHECK(decided == (DecisionRecord{header, cert, candidate.Body()}));
    const AppliedRecord applied_record{AppliedRecord::Decode(records[1].payload).value()};
    BOOST_CHECK(applied_record == (AppliedRecord{1, header.Hash(), header.state_root}));
}

BOOST_AUTO_TEST_CASE(applied_with_the_wrong_root_or_slot_is_refused)
{
    // An APPLIED needs the ApplyReceipt of a real Apply (O-92), and Apply takes
    // the state root from the decided header, so the root can no longer be
    // wrong (the receipt binds it). What remains: receipts of other logs and
    // of decisions this log does not hold.
    OwnerFixture f{5};
    const state::Candidate candidate{f.Preview(f.Inputs(FundingFacts(f.actions), {}))};
    const DurableDecision decision{f.Decide(candidate)};
    state::Candidate copy{candidate};
    const state::AppliedSlot slot{f.owner->Apply(std::move(copy), decision, f.authority).value()};
    BOOST_CHECK(slot.receipt.StateRoot() == candidate.Header().state_root);
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, 1U);
    const auto applied_by = [](OwnerFixture& g, const state::Candidate& c) {
        const DurableDecision d{g.Decide(c)};
        state::Candidate c_copy{c};
        Result<state::AppliedSlot> applied{g.owner->Apply(std::move(c_copy), d, g.authority)};
        RequireOk(applied, "Apply");
        return std::move(applied).value();
    };
    // A receipt of another commit journal instance.
    OwnerFixture other{6};
    const state::AppliedSlot foreign{applied_by(other, other.Preview(other.Inputs(FundingFacts(other.actions), {})))};
    RequireRefusal(f.log.AppendApplied(foreign.receipt), ErrorCode::VERIFY_FAILED, TOKEN_OTHER_LOG, "other instance");
    // A log with the same instance id, for a slot this log never decided.
    OwnerFixture same_id{5};
    same_id.Run(same_id.Inputs(FundingFacts(same_id.actions), {}));
    const state::AppliedSlot slot2{applied_by(same_id, same_id.Preview(same_id.Inputs({}, {})))};
    BOOST_CHECK_EQUAL(slot2.receipt.Slot(), 2U);
    RequireRefusal(f.log.AppendApplied(slot2.receipt), ErrorCode::VERIFY_FAILED, APPLIED_DECISION_UNKNOWN, "slot this log never decided");
    // The same slot and header at another sequence number (only the sequence number differs) ...
    OwnerFixture shifted{5};
    RequireStorageOk(shifted.journal->Append(RecordType::DECISION, Bytes("raw record before the decision")), "raw append");
    const state::AppliedSlot shifted_slot{applied_by(shifted, shifted.Preview(shifted.Inputs(FundingFacts(shifted.actions), {})))};
    BOOST_CHECK(shifted_slot.receipt.DecisionSeq() != slot.receipt.DecisionSeq());
    BOOST_CHECK(shifted_slot.receipt.HeaderHash() == slot.receipt.HeaderHash());
    RequireRefusal(f.log.AppendApplied(shifted_slot.receipt), ErrorCode::VERIFY_FAILED, APPLIED_DECISION_UNKNOWN,
                   "same slot and header, other sequence number");
    // ... and another header of the same slot at the same sequence number (only the header hash differs).
    OwnerFixture other_header{5};
    const auto one_deposit{other_header.actions.DepositBytes(KeyAccount("alice"), TestAsset("rUSD"), 7, "other-header")};
    const state::AppliedSlot other_header_slot{applied_by(other_header, other_header.Preview(other_header.Inputs({one_deposit}, {})))};
    BOOST_CHECK(other_header_slot.receipt.DecisionSeq() == slot.receipt.DecisionSeq());
    BOOST_CHECK(other_header_slot.receipt.Slot() == slot.receipt.Slot());
    BOOST_CHECK(other_header_slot.receipt.HeaderHash() != slot.receipt.HeaderHash());
    RequireRefusal(f.log.AppendApplied(other_header_slot.receipt), ErrorCode::VERIFY_FAILED, APPLIED_DECISION_UNKNOWN,
                   "same slot and sequence number, other header");
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, 1U);
    // Its own receipt works, and again is idempotent.
    const PendingApplied applied{f.log.AppendApplied(slot.receipt).value()};
    BOOST_CHECK(f.log.AppendApplied(slot.receipt).value() == applied);
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, 2U);
}

BOOST_AUTO_TEST_CASE(applied_in_slot_order_only)
{
    // The owner applies slots 1 and 2; their APPLIED records must follow in
    // slot order, without a gap (slot 1 first), as replay requires (O-89).
    OwnerFixture f{7};
    const state::Candidate c1{f.Preview(f.Inputs(FundingFacts(f.actions), {}))};
    const DurableDecision d1{f.Decide(c1)};
    state::Candidate c1_copy{c1};
    const state::AppliedSlot s1{f.owner->Apply(std::move(c1_copy), d1, f.authority).value()};
    f.Sync();
    const state::Candidate c2{f.Preview(f.Inputs({}, {}))};
    const DurableDecision d2{f.Decide(c2)};
    state::Candidate c2_copy{c2};
    const state::AppliedSlot s2{f.owner->Apply(std::move(c2_copy), d2, f.authority).value()};
    RequireRefusal(f.log.AppendApplied(s2.receipt), ErrorCode::ORDER_VIOLATION, APPLIED_OUT_OF_ORDER, "slot 2 before slot 1");
    const PendingApplied a1{f.log.AppendApplied(s1.receipt).value()};
    RequireStorageOk(f.log.AppendApplied(s2.receipt), "apply slot 2");
    BOOST_CHECK(f.log.AppendApplied(s1.receipt).value() == a1); // a repeat is idempotent
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, 4U);
}

BOOST_AUTO_TEST_CASE(decision_rules)
{
    CommitFixture f{8};
    const StorageSlot slot{MakeStorageSlot(3, 0, "c")};
    // The certificate must name this header.
    SlotCertificate wrong_cert{slot.certificate};
    wrong_cert.header_hash = MakeStorageSlot(3, 0, "c-other").header.Hash();
    RequireRefusal(f.log.AppendDecision(slot.header, wrong_cert, slot.body), ErrorCode::VERIFY_FAILED, DECISION_CERT_HEADER, "cert of another header");
    // The body must be the header's: originals, count and facts.
    SlotBody extra{slot.body};
    extra.originals.push_back(Bytes("extra"));
    RequireRefusal(f.log.AppendDecision(slot.header, slot.certificate, extra), ErrorCode::VERIFY_FAILED, BODY_HEADER_MISMATCH, "extra original");
    SlotBody changed{slot.body};
    changed.originals[0].push_back(0);
    RequireRefusal(f.log.AppendDecision(slot.header, slot.certificate, changed), ErrorCode::VERIFY_FAILED, BODY_HEADER_MISMATCH, "changed original");
    SlotBody with_fact{slot.body};
    with_fact.facts.push_back(Bytes("fact"));
    RequireRefusal(f.log.AppendDecision(slot.header, slot.certificate, with_fact), ErrorCode::VERIFY_FAILED, BODY_HEADER_MISMATCH, "extra fact");
    SlotHeaderV2 wrong_count{slot.header};
    wrong_count.action_count = 2;
    SlotCertificate count_cert{slot.certificate};
    count_cert.header_hash = wrong_count.Hash();
    RequireRefusal(f.log.AppendDecision(wrong_count, count_cert, slot.body), ErrorCode::VERIFY_FAILED, BODY_HEADER_MISMATCH, "action_count");
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, 0U);
    BOOST_CHECK(!f.log.DecisionAt(3).has_value()); // refused appends decide nothing
    // Same header twice is idempotent, even with another certificate; another header is a conflict.
    const PendingDecision first{f.log.AppendDecision(slot.header, slot.certificate, slot.body).value()};
    SlotCertificate other_subset{slot.certificate};
    other_subset.aggregate.fill(0x11);
    BOOST_CHECK(f.log.AppendDecision(slot.header, other_subset, slot.body).value() == first);
    const StorageSlot conflict{MakeStorageSlot(3, 0, "c-conflict")};
    RequireRefusal(f.log.AppendDecision(conflict.header, conflict.certificate, conflict.body), ErrorCode::EQUIVOCATION_REFUSED,
                   DECISION_CONFLICT, "second header for a decided slot");
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, 1U);
    // DecisionAt names the kept decision (first certificate) and nothing else.
    BOOST_CHECK(f.log.DecisionAt(3) == std::optional<DecisionRef>{first.Key()});
    BOOST_CHECK(!f.log.DecisionAt(2).has_value());
    BOOST_CHECK(!f.log.DecisionAt(4).has_value());
    RequireStub(f.log.AppendPreparedQc(PreparedQcRecord{0, 3, 0, slot.header.Hash(), Bytes("qc")}), "M4-STO-07");
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, 1U);
}

BOOST_AUTO_TEST_CASE(decision_token_binds_the_kept_certificate)
{
    // The state owner (R9) takes the new head's certificate hash from the
    // decision token, so the token must name the certificate the DECISION
    // record holds: the first one, also when the same header is appended
    // again with another certificate (certificates are not unique).
    CommitFixture f{9};
    const StorageSlot slot{MakeStorageSlot(4, 1, "kept")};
    const PendingDecision first{f.log.AppendDecision(slot.header, slot.certificate, slot.body).value()};
    BOOST_CHECK(first.Key() == (DecisionRef{4, slot.header.Hash(), slot.certificate.Hash()}));
    SlotCertificate other_subset{slot.certificate};
    other_subset.aggregate.fill(0x22);
    BOOST_REQUIRE(other_subset.Hash() != slot.certificate.Hash());
    const PendingDecision again{f.log.AppendDecision(slot.header, other_subset, slot.body).value()};
    BOOST_CHECK(again.Key().cert_hash == slot.certificate.Hash());
    BOOST_CHECK(again == first);
    BOOST_CHECK_EQUAL(f.log.Barrier().value(), 1U);
    const DurableDecision token{f.log.Confirm(again).value()};
    BOOST_CHECK(token.Binding().cert_hash == slot.certificate.Hash());
    // The record holds that same certificate.
    const std::vector<JournalRecord> records{f.journal->ReadAll().value()};
    BOOST_REQUIRE_EQUAL(records.size(), 1U);
    BOOST_CHECK(DecisionRecord::Decode(records[0].payload, Profile()).value().certificate.Hash() == token.Binding().cert_hash);
    // A decision appended first with the other certificate binds that one.
    CommitFixture g{10};
    const PendingDecision other_first{g.log.AppendDecision(slot.header, other_subset, slot.body).value()};
    BOOST_CHECK(other_first.Key().cert_hash == other_subset.Hash());
}

BOOST_AUTO_TEST_CASE(record_codecs_golden_and_exact)
{
    // APPLIED golden from evidence/R8/golden.py.
    const AppliedRecord applied{9, LabelHash("fm2/test/header"), LabelHash("fm2/test/state")};
    BOOST_CHECK_EQUAL(HexStr(applied.Encode()),
                      "0900000000000000790f0bba29f4981147021667ddc4d0b7de7661161fd2e8dd03b340903afac46ad8c5d5260591d10c7bc72011dc65157f"
                      "a072eb8d4754d398f4d53840ee14063b");
    BOOST_CHECK_EQUAL(applied.Encode().size(), APPLIED_RECORD_BYTES);
    BOOST_CHECK(AppliedRecord::Decode(applied.Encode()).value() == applied);
    std::vector<unsigned char> bytes{applied.Encode()};
    bytes.push_back(0);
    RequireCode(AppliedRecord::Decode(bytes), ErrorCode::OVERSIZE, "applied + 1");
    bytes.resize(APPLIED_RECORD_BYTES - 1);
    RequireCode(AppliedRecord::Decode(bytes), ErrorCode::DECODE, "applied - 1");

    // DECISION layout: header | CompactSize(cert) | cert | body.
    const StorageSlot slot{MakeStorageSlot(4, 0, "codec")};
    const DecisionRecord decision{slot.header, slot.certificate, slot.body};
    const std::vector<unsigned char> encoded{decision.Encode()};
    const std::vector<unsigned char> header_bytes{slot.header.Encode()};
    const std::vector<unsigned char> cert_bytes{slot.certificate.Encode()};
    const std::vector<unsigned char> body_bytes{slot.body.Encode()};
    BOOST_REQUIRE_EQUAL(encoded.size(), header_bytes.size() + 1 + cert_bytes.size() + body_bytes.size());
    BOOST_CHECK(std::equal(header_bytes.begin(), header_bytes.end(), encoded.begin()));
    BOOST_CHECK_EQUAL(size_t{encoded[SLOT_HEADER_BYTES]}, cert_bytes.size());
    BOOST_CHECK(std::equal(cert_bytes.begin(), cert_bytes.end(), encoded.begin() + SLOT_HEADER_BYTES + 1));
    BOOST_CHECK(std::equal(body_bytes.begin(), body_bytes.end(), encoded.begin() + SLOT_HEADER_BYTES + 1 + cert_bytes.size()));
    BOOST_CHECK(DecisionRecord::Decode(encoded, Profile()).value() == decision);
    // Refusals: cut inside the certificate, a certificate length above its cap, trailing byte, oversize.
    std::vector<unsigned char> cut(encoded.begin(), encoded.begin() + SLOT_HEADER_BYTES + 5);
    RequireCode(DecisionRecord::Decode(cut, Profile()), ErrorCode::DECODE, "cut inside the certificate");
    std::vector<unsigned char> big_cert{encoded};
    big_cert[SLOT_HEADER_BYTES] = 0xfd; // CompactSize 0xfd prefix: the next two bytes are the length
    big_cert.insert(big_cert.begin() + SLOT_HEADER_BYTES + 1, {0xff, 0xff});
    RequireCode(DecisionRecord::Decode(big_cert, Profile()), ErrorCode::OVERSIZE, "certificate length above its cap");
    std::vector<unsigned char> trailing{encoded};
    trailing.push_back(0);
    RequireCode(DecisionRecord::Decode(trailing, Profile()), ErrorCode::DECODE, "trailing byte");
    RequireCode(DecisionRecord::Decode(std::vector<unsigned char>(SLOT_HEADER_BYTES - 1), Profile()), ErrorCode::DECODE, "short");
    const std::vector<unsigned char> huge(DecisionRecord::MaxEncodedSize(Profile()) + 1);
    RequireCode(DecisionRecord::Decode(huge, Profile()), ErrorCode::OVERSIZE, "above the maximum");
    // The largest decision of the regtest profile fits in one record.
    BOOST_CHECK(DecisionRecord::MaxEncodedSize(Profile()) <= MAX_RECORD_PAYLOAD_BYTES);
}

BOOST_AUTO_TEST_SUITE_END()
