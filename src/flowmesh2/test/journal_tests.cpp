// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <flowmesh2/storage/journal.h>
#include <flowmesh2/storage/memory_journal.h>
#include <flowmesh2/storage/record.h>
#include <flowmesh2/test/util/storage_fixtures.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::storage;
using namespace flowmesh2::test;

namespace {

std::unique_ptr<MemoryJournal> NewJournal(JournalRole role, uint64_t instance, MemoryJournalFaults faults = {},
                                          BarrierObserver* observer = nullptr)
{
    Result<std::unique_ptr<MemoryJournal>> journal{MemoryJournal::Create(role, instance, faults, observer)};
    BOOST_REQUIRE(journal.has_value());
    return std::move(journal).value();
}

uint64_t MustAppend(Journal& journal, RecordType type, std::string_view payload)
{
    const Result<uint64_t> seq{journal.Append(type, Bytes(payload))};
    RequireStorageOk(seq, "append");
    return *seq;
}

} // namespace

BOOST_AUTO_TEST_SUITE(journal_tests)

BOOST_AUTO_TEST_CASE(append_and_barrier_sequence)
{
    auto journal{NewJournal(JournalRole::COMMIT, 11)};
    BOOST_CHECK(journal->Role() == JournalRole::COMMIT);
    BOOST_CHECK_EQUAL(journal->InstanceId(), 11U);
    BOOST_CHECK(journal->Status() == (JournalStatus{0, 0, false}));

    BOOST_CHECK_EQUAL(MustAppend(*journal, RecordType::DECISION, "d1"), 1U);
    BOOST_CHECK_EQUAL(MustAppend(*journal, RecordType::APPLIED, "a1"), 2U);
    BOOST_CHECK(journal->Status() == (JournalStatus{2, 0, false}));
    BOOST_CHECK(journal->DurableImage().empty());
    // Nothing is durable before the barrier.
    const auto before{journal->ReadAll()};
    RequireStorageOk(before, "read before the barrier");
    BOOST_CHECK(before->empty());

    // One barrier covers both (group commit).
    const Result<uint64_t> durable{journal->Barrier()};
    RequireStorageOk(durable, "barrier");
    BOOST_CHECK_EQUAL(*durable, 2U);
    BOOST_CHECK(journal->Status() == (JournalStatus{2, 2, false}));
    BOOST_CHECK_EQUAL(journal->DurableSeq(), 2U);
    BOOST_CHECK(!journal->IsFatal());

    BOOST_CHECK_EQUAL(MustAppend(*journal, RecordType::DECISION, "d2"), 3U);
    // A barrier with nothing new still succeeds and returns the durable sequence.
    BOOST_CHECK_EQUAL(journal->Barrier().value(), 3U);
    BOOST_CHECK_EQUAL(journal->Barrier().value(), 3U);

    const auto records{journal->ReadAll()};
    RequireStorageOk(records, "read all");
    BOOST_REQUIRE_EQUAL(records->size(), 3U);
    BOOST_CHECK((*records)[0].payload == Bytes("d1") && (*records)[0].type == RecordType::DECISION && (*records)[0].seq == 1);
    BOOST_CHECK((*records)[1].payload == Bytes("a1") && (*records)[1].type == RecordType::APPLIED && (*records)[1].seq == 2);
    BOOST_CHECK((*records)[2].payload == Bytes("d2") && (*records)[2].type == RecordType::DECISION && (*records)[2].seq == 3);
    // The image is exactly the framed records; a scan as the same instance reproduces them.
    const Result<ScanResult> scan{ScanRecords(journal->DurableImage(), JournalRole::COMMIT, 11)};
    RequireStorageOk(scan, "scan of the durable image");
    BOOST_CHECK(scan->records == *records);
    BOOST_CHECK_EQUAL(scan->end_offset, journal->DurableImage().size());
}

BOOST_AUTO_TEST_CASE(role_mismatch_is_order_violation)
{
    auto signer{NewJournal(JournalRole::SIGNER, 1)};
    auto commit{NewJournal(JournalRole::COMMIT, 1)};
    for (RecordType type : {RecordType::DECISION, RecordType::APPLIED, RecordType::SNAPSHOT_MARK, RecordType::PREPARED_QC,
                            RecordType{0}, RecordType{10}, RecordType{255}}) {
        RequireRefusal(signer->Append(type, Bytes("x")), ErrorCode::ORDER_VIOLATION, JOURNAL_ROLE_MISMATCH, "signer journal");
    }
    for (RecordType type : {RecordType::INTENT, RecordType::SIGNED_VOTE, RecordType::ACCEPTED_PROPOSAL, RecordType::VIEW_STATE,
                            RecordType::NEW_VIEW_ACCEPTED, RecordType{0}, RecordType{10}}) {
        RequireRefusal(commit->Append(type, Bytes("x")), ErrorCode::ORDER_VIOLATION, JOURNAL_ROLE_MISMATCH, "commit journal");
    }
    // A refused append consumes no sequence number and does not fence the journal.
    BOOST_CHECK(signer->Status() == (JournalStatus{0, 0, false}));
    BOOST_CHECK(commit->Status() == (JournalStatus{0, 0, false}));
    BOOST_CHECK_EQUAL(MustAppend(*signer, RecordType::INTENT, "i"), 1U);
    BOOST_CHECK_EQUAL(MustAppend(*commit, RecordType::DECISION, "d"), 1U);
    // So does an oversize payload.
    const std::vector<unsigned char> huge(MAX_RECORD_PAYLOAD_BYTES + 1);
    RequireRefusal(commit->Append(RecordType::DECISION, huge), ErrorCode::OVERSIZE, RECORD_LENGTH, "oversize payload");
    BOOST_CHECK(commit->Status() == (JournalStatus{1, 0, false}));
    RequireCode(MemoryJournal::Create(JournalRole{0}, 1), ErrorCode::INVALID_INPUT, "unknown role");
}

BOOST_AUTO_TEST_CASE(failed_barrier_is_sticky_fatal)
{
    RecordingObserver observer;
    auto journal{NewJournal(JournalRole::SIGNER, 4, MemoryJournalFaults{.fail_barrier = 2}, &observer)};
    MustAppend(*journal, RecordType::ACCEPTED_PROPOSAL, "p");
    BOOST_CHECK_EQUAL(journal->Barrier().value(), 1U);
    MustAppend(*journal, RecordType::INTENT, "i");
    // Barrier 2 fails: STORAGE_FATAL with the fault's detail; nothing new became durable.
    RequireRefusal(journal->Barrier(), ErrorCode::STORAGE_FATAL, MEMORY_JOURNAL_BARRIER_FAULT, "barrier 2");
    BOOST_CHECK(journal->Status() == (JournalStatus{2, 1, true}));
    BOOST_CHECK(journal->IsFatal());
    // Sticky: Append is FENCED, every later Barrier is STORAGE_FATAL, nothing retried.
    for (int i{0}; i < 3; ++i) {
        RequireRefusal(journal->Append(RecordType::INTENT, Bytes("again")), ErrorCode::FENCED, JOURNAL_FENCED, "append after fatal");
        RequireRefusal(journal->Barrier(), ErrorCode::STORAGE_FATAL, JOURNAL_FATAL, "barrier after fatal");
    }
    BOOST_CHECK(journal->Status() == (JournalStatus{2, 1, true}));
    // Only the first record survived.
    const auto records{journal->ReadAll()};
    RequireStorageOk(records, "read all");
    BOOST_REQUIRE_EQUAL(records->size(), 1U);
    BOOST_CHECK((*records)[0].type == RecordType::ACCEPTED_PROPOSAL);
    // The barriers after the fatal one never reached the implementation.
    const std::vector<std::string> expected{
        "append SIGNER 4 ACCEPTED_PROPOSAL 1", "barrier-start SIGNER 4 1", "barrier-end SIGNER 4 1 ok",
        "append SIGNER 4 INTENT 2",            "barrier-start SIGNER 4 2", "barrier-end SIGNER 4 2 fail"};
    BOOST_CHECK_EQUAL_COLLECTIONS(observer.events.begin(), observer.events.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(failed_append_is_sticky_fatal)
{
    auto journal{NewJournal(JournalRole::COMMIT, 4, MemoryJournalFaults{.fail_append = 3})};
    MustAppend(*journal, RecordType::DECISION, "d1");
    MustAppend(*journal, RecordType::APPLIED, "a1");
    // Refused appends that never reached the write step do not count.
    RequireCode(journal->Append(RecordType::INTENT, Bytes("x")), ErrorCode::ORDER_VIOLATION, "role refusal");
    RequireRefusal(journal->Append(RecordType::DECISION, Bytes("d2")), ErrorCode::STORAGE_FATAL, MEMORY_JOURNAL_APPEND_FAULT, "append 3");
    BOOST_CHECK(journal->Status() == (JournalStatus{2, 0, true}));
    RequireRefusal(journal->Append(RecordType::DECISION, Bytes("d3")), ErrorCode::FENCED, JOURNAL_FENCED, "append after fatal");
    RequireRefusal(journal->Barrier(), ErrorCode::STORAGE_FATAL, JOURNAL_FATAL, "barrier after fatal");
    BOOST_CHECK(journal->DurableImage().empty());
}

BOOST_AUTO_TEST_CASE(observer_sees_the_exact_sequence)
{
    RecordingObserver observer;
    auto journal{NewJournal(JournalRole::SIGNER, 21, {}, &observer)};
    MustAppend(*journal, RecordType::ACCEPTED_PROPOSAL, "p1");
    MustAppend(*journal, RecordType::INTENT, "i1");
    BOOST_CHECK_EQUAL(journal->Barrier().value(), 2U);
    MustAppend(*journal, RecordType::SIGNED_VOTE, "v1");
    BOOST_CHECK_EQUAL(journal->Barrier().value(), 3U);
    RequireCode(journal->Append(RecordType::DECISION, Bytes("x")), ErrorCode::ORDER_VIOLATION, "refused append");
    const std::vector<std::string> expected{
        "append SIGNER 21 ACCEPTED_PROPOSAL 1",
        "append SIGNER 21 INTENT 2",
        "barrier-start SIGNER 21 2",
        "barrier-end SIGNER 21 2 ok",
        "append SIGNER 21 SIGNED_VOTE 3",
        "barrier-start SIGNER 21 3",
        "barrier-end SIGNER 21 3 ok",
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(observer.events.begin(), observer.events.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(append_during_barrier_is_not_covered)
{
    // An append that lands while a barrier is writing belongs to the next barrier.
    RecordingObserver observer;
    auto journal{NewJournal(JournalRole::COMMIT, 8, {}, &observer)};
    std::optional<uint64_t> during;
    observer.on_barrier_start = [&] {
        if (!during) during = journal->Append(RecordType::APPLIED, Bytes("late")).value();
    };
    MustAppend(*journal, RecordType::DECISION, "early");
    BOOST_CHECK_EQUAL(journal->Barrier().value(), 1U);
    BOOST_REQUIRE(during.has_value());
    BOOST_CHECK_EQUAL(*during, 2U);
    BOOST_CHECK(journal->Status() == (JournalStatus{2, 1, false}));
    BOOST_CHECK_EQUAL(journal->ReadAll().value().size(), 1U);
    BOOST_CHECK_EQUAL(journal->Barrier().value(), 2U);
    BOOST_CHECK_EQUAL(journal->ReadAll().value().size(), 2U);
}

BOOST_AUTO_TEST_CASE(concurrent_barrier_is_refused)
{
    // A second Barrier while one runs is a caller error, not a storage failure.
    RecordingObserver observer;
    auto journal{NewJournal(JournalRole::COMMIT, 9, {}, &observer)};
    std::optional<Error> nested;
    observer.on_barrier_start = [&] {
        if (nested) return;
        const Result<uint64_t> inner{journal->Barrier()};
        nested = inner.has_value() ? Error{ErrorCode::INVARIANT, "nested barrier succeeded"} : inner.error();
    };
    MustAppend(*journal, RecordType::DECISION, "d");
    BOOST_CHECK_EQUAL(journal->Barrier().value(), 1U);
    BOOST_REQUIRE(nested.has_value());
    BOOST_CHECK(nested->code == ErrorCode::ORDER_VIOLATION && nested->detail == JOURNAL_BARRIER_CONCURRENT);
    BOOST_CHECK(journal->Status() == (JournalStatus{1, 1, false}));
}

BOOST_AUTO_TEST_CASE(instances_and_roles_bind_the_image)
{
    // Two journals with the same records but other instances or roles write different images.
    auto a{NewJournal(JournalRole::COMMIT, 1)};
    auto b{NewJournal(JournalRole::COMMIT, 2)};
    for (auto* j : {a.get(), b.get()}) {
        MustAppend(*j, RecordType::DECISION, "same");
        BOOST_CHECK_EQUAL(j->Barrier().value(), 1U);
    }
    BOOST_CHECK(a->DurableImage() != b->DurableImage());
    RequireRefusal(ScanRecords(a->DurableImage(), JournalRole::COMMIT, 2), ErrorCode::VERIFY_FAILED, RECORD_CHAIN, "a as instance 2");
}

BOOST_AUTO_TEST_SUITE_END()
