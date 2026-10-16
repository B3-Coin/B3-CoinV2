// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/params.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/memory_journal.h>
#include <flowmesh2/test/util/storage_fixtures.h>
#include <util/strencodings.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
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

    DurableDecision Decided(const StorageSlot& slot)
    {
        const PendingDecision pending{log.AppendDecision(slot.header, slot.certificate, slot.body).value()};
        RequireStorageOk(log.Barrier(), "B-DECISION");
        return log.Confirm(pending).value();
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
    RequireStub(CommitLog::Create(*used), "M4-STO-01");
}

BOOST_AUTO_TEST_CASE(decision_then_applied)
{
    CommitFixture f{4};
    const StorageSlot slot{MakeStorageSlot(1, 0, "d")};
    const PendingDecision pending{f.log.AppendDecision(slot.header, slot.certificate, slot.body).value()};
    RequireRefusal(f.log.Confirm(pending), ErrorCode::ORDER_VIOLATION, CONFIRM_BEFORE_BARRIER, "decision before B-DECISION");
    BOOST_CHECK_EQUAL(f.log.Barrier().value(), 1U);
    const DurableDecision decision{f.log.Confirm(pending).value()};
    const PendingApplied applied{f.log.AppendApplied(decision, slot.header.state_root).value()};
    RequireRefusal(f.log.Confirm(applied), ErrorCode::ORDER_VIOLATION, CONFIRM_BEFORE_BARRIER, "applied before B-APPLIED");
    BOOST_CHECK_EQUAL(f.log.Barrier().value(), 2U);
    const DurableApplied token{f.log.Confirm(applied).value()};
    BOOST_CHECK(token.Binding() == (AppliedRef{1, slot.header.Hash(), slot.header.state_root}));

    // The records decode back: the DECISION carries header, certificate and body (O-16).
    const std::vector<JournalRecord> records{f.journal->ReadAll().value()};
    BOOST_REQUIRE_EQUAL(records.size(), 2U);
    BOOST_CHECK(records[0].type == RecordType::DECISION && records[1].type == RecordType::APPLIED);
    const DecisionRecord decided{DecisionRecord::Decode(records[0].payload, Profile()).value()};
    BOOST_CHECK(decided == (DecisionRecord{slot.header, slot.certificate, slot.body}));
    const AppliedRecord applied_record{AppliedRecord::Decode(records[1].payload).value()};
    BOOST_CHECK(applied_record == (AppliedRecord{1, slot.header.Hash(), slot.header.state_root}));
}

BOOST_AUTO_TEST_CASE(applied_with_the_wrong_root_or_slot_is_refused)
{
    CommitFixture f{5};
    const StorageSlot slot{MakeStorageSlot(1, 0, "r")};
    const DurableDecision decision{f.Decided(slot)};
    RequireRefusal(f.log.AppendApplied(decision, LabelHash("fm2/test/storage/wrong-root")), ErrorCode::VERIFY_FAILED,
                   APPLIED_ROOT_MISMATCH, "wrong state root");
    RequireRefusal(f.log.AppendApplied(decision, uint256{}), ErrorCode::VERIFY_FAILED, APPLIED_ROOT_MISMATCH, "zero state root");
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, 1U);
    // A decision token of another commit log (other instance, or same instance id but another log).
    CommitFixture other{6};
    const DurableDecision foreign{other.Decided(slot)};
    RequireRefusal(f.log.AppendApplied(foreign, slot.header.state_root), ErrorCode::VERIFY_FAILED, TOKEN_OTHER_LOG, "other instance");
    CommitFixture same_id{5};
    const DurableDecision same_id_token{same_id.Decided(MakeStorageSlot(2, 0, "r"))};
    RequireRefusal(f.log.AppendApplied(same_id_token, MakeStorageSlot(2, 0, "r").header.state_root), ErrorCode::VERIFY_FAILED,
                   APPLIED_DECISION_UNKNOWN, "slot this log never decided");
    // The right root works, and again is idempotent.
    const PendingApplied applied{f.log.AppendApplied(decision, slot.header.state_root).value()};
    BOOST_CHECK(f.log.AppendApplied(decision, slot.header.state_root).value() == applied);
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, 2U);
}

BOOST_AUTO_TEST_CASE(applied_in_slot_order_only)
{
    CommitFixture f{7};
    const StorageSlot s1{MakeStorageSlot(1, 0, "o")};
    const StorageSlot s2{MakeStorageSlot(2, 0, "o")};
    const DurableDecision d1{f.Decided(s1)};
    const DurableDecision d2{f.Decided(s2)};
    RequireStorageOk(f.log.AppendApplied(d2, s2.header.state_root), "apply slot 2");
    RequireRefusal(f.log.AppendApplied(d1, s1.header.state_root), ErrorCode::ORDER_VIOLATION, APPLIED_OUT_OF_ORDER, "slot 1 after slot 2");
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
    // Same header twice is idempotent, even with another certificate; another header is a conflict.
    const PendingDecision first{f.log.AppendDecision(slot.header, slot.certificate, slot.body).value()};
    SlotCertificate other_subset{slot.certificate};
    other_subset.aggregate.fill(0x11);
    BOOST_CHECK(f.log.AppendDecision(slot.header, other_subset, slot.body).value() == first);
    const StorageSlot conflict{MakeStorageSlot(3, 0, "c-conflict")};
    RequireRefusal(f.log.AppendDecision(conflict.header, conflict.certificate, conflict.body), ErrorCode::EQUIVOCATION_REFUSED,
                   DECISION_CONFLICT, "second header for a decided slot");
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, 1U);
    RequireStub(f.log.AppendPreparedQc(PreparedQcRecord{0, 3, 0, slot.header.Hash(), Bytes("qc")}), "M4-STO-07");
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, 1U);
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
