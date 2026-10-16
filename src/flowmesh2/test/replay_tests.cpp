// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/params.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/state/replay.h>
#include <flowmesh2/state/state_owner.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/record.h>
#include <flowmesh2/test/util/state_fixtures.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;

namespace {

using Records = std::vector<storage::JournalRecord>;

//! The walking skeleton through a state owner, and the durable records of its commit journal.
struct ReplayFixture {
    OwnerFixture f;
    state::HeadView genesis;
    SkeletonRun run;
    Records records;

    explicit ReplayFixture(std::function<void(OwnerFixture&)> setup = {}) : genesis{TestGenesisHead()}
    {
        if (setup) setup(f);
        run = RunWalkingSkeleton(f);
        records = f.journal->ReadAll().value();
    }
};

//! `records` renumbered 1, 2, 3, ... (the doctored journals below stay contiguous).
Records Renumbered(Records records)
{
    for (size_t i{0}; i < records.size(); ++i) records[i].seq = i + 1;
    return records;
}

storage::DecisionRecord DecodeDecision(const storage::JournalRecord& record)
{
    return storage::DecisionRecord::Decode(record.payload, RegtestProfile()).value();
}

storage::JournalRecord DecisionRecordOf(const storage::DecisionRecord& decision, uint64_t seq)
{
    return storage::JournalRecord{seq, storage::RecordType::DECISION, decision.Encode(), uint256{}};
}

//! Requires the replay to reproduce the owner's heads exactly.
void RequireReproduced(const state::ReplayResult& result, const ReplayFixture& fx)
{
    BOOST_REQUIRE_EQUAL(result.slots.size(), fx.run.heads.size());
    for (size_t i{0}; i < result.slots.size(); ++i) {
        BOOST_CHECK(result.slots[i].header == fx.run.heads[i]->Header());
        BOOST_CHECK(result.slots[i].cert_hash == fx.run.heads[i]->CertHash());
    }
    const std::shared_ptr<const state::HeadView> head{fx.f.Head()};
    BOOST_CHECK(result.head.Header() == head->Header());
    BOOST_CHECK(result.head.HeaderHash() == head->HeaderHash());
    BOOST_CHECK(result.head.CertHash() == head->CertHash());
    BOOST_CHECK(result.head.Header().state_root == head->Header().state_root);
    BOOST_CHECK(result.head.State() == head->State());
}

} // namespace

BOOST_AUTO_TEST_SUITE(replay_tests)

BOOST_AUTO_TEST_CASE(replay_reproduces_every_header_and_the_final_root)
{
    const ReplayFixture fx;
    BOOST_REQUIRE_EQUAL(fx.records.size(), 10U);
    const Result<state::ReplayResult> result{state::ReplayDecisions(fx.genesis, fx.records)};
    RequireOk(result, "replay the commit journal");
    RequireReproduced(*result, fx);
    for (size_t i{0}; i < result->slots.size(); ++i) {
        BOOST_CHECK_EQUAL(result->slots[i].decision_seq, 2 * i + 1);
        BOOST_REQUIRE(result->slots[i].applied_seq.has_value());
        BOOST_CHECK_EQUAL(*result->slots[i].applied_seq, 2 * i + 2);
    }
    BOOST_CHECK_EQUAL(result->duplicate_decisions, 0U);
    BOOST_CHECK_EQUAL(result->duplicate_applied, 0U);
    CheckWalkingSkeletonFinalState(result->head.State(), fx.run);
    // The replayed state is a fresh object built from the records, not the owner's.
    BOOST_CHECK(result->head.StatePtr() != fx.f.Head()->StatePtr());

    // Nothing to replay: the genesis head.
    const Result<state::ReplayResult> empty{state::ReplayDecisions(fx.genesis, {})};
    RequireOk(empty, "replay an empty journal");
    BOOST_CHECK(empty->slots.empty());
    BOOST_CHECK(empty->head.Header() == fx.genesis.Header());
    // A decided slot whose APPLIED record is missing (a crash before B-APPLIED) is replayed too.
    const Records cut{fx.records.begin(), fx.records.end() - 1};
    const Result<state::ReplayResult> partial{state::ReplayDecisions(fx.genesis, cut)};
    RequireOk(partial, "replay without the last APPLIED");
    BOOST_REQUIRE_EQUAL(partial->slots.size(), 5U);
    BOOST_CHECK(!partial->slots.back().applied_seq.has_value());
    BOOST_CHECK(partial->head.Header() == fx.f.Head()->Header());
}

BOOST_AUTO_TEST_CASE(a_duplicated_decision_is_applied_once)
{
    // Slot 3's DECISION is written twice (identical bytes), slot 4's again
    // with another certificate, before their APPLIED records; slot 2's again
    // at the end of the journal.
    const ReplayFixture fx{[](OwnerFixture& f) {
        f.after_decision = [&f](const state::Candidate& candidate) {
            const uint64_t height{candidate.Header().height};
            if (height != 3 && height != 4) return;
            const uint8_t variant{static_cast<uint8_t>(height == 3 ? 0 : 9)};
            const storage::DecisionRecord again{candidate.Header(), TestCertificate(candidate.Header(), variant), candidate.Body()};
            RequireOk(f.journal->Append(storage::RecordType::DECISION, again.Encode()), "duplicate DECISION");
            RequireOk(f.journal->Barrier(), "barrier");
        };
    }};
    Records records{fx.records};
    BOOST_REQUIRE_EQUAL(records.size(), 12U);
    BOOST_CHECK(records[4].payload == records[5].payload); // slot 3, twice
    BOOST_CHECK(records[7].payload != records[8].payload); // slot 4, another certificate
    records.push_back(storage::JournalRecord{13, storage::RecordType::DECISION, records[2].payload, uint256{}});
    const Result<state::ReplayResult> result{state::ReplayDecisions(fx.genesis, records)};
    RequireOk(result, "replay with duplicated decisions");
    RequireReproduced(*result, fx);
    BOOST_CHECK_EQUAL(result->duplicate_decisions, 3U);
    // The first record of each height counts: its sequence number and certificate.
    BOOST_CHECK_EQUAL(result->slots[2].decision_seq, 5U);
    BOOST_CHECK_EQUAL(result->slots[3].decision_seq, 8U);
    BOOST_CHECK(result->slots[3].cert_hash == TestCertificate(result->slots[3].header, 0).Hash());
    CheckWalkingSkeletonFinalState(result->head.State(), fx.run);
    // A repeated APPLIED record is skipped as well.
    records.push_back(storage::JournalRecord{14, storage::RecordType::APPLIED, records[1].payload, uint256{}});
    const Result<state::ReplayResult> repeated{state::ReplayDecisions(fx.genesis, records)};
    RequireOk(repeated, "replay with a repeated APPLIED");
    BOOST_CHECK_EQUAL(repeated->duplicate_applied, 1U);
}

BOOST_AUTO_TEST_CASE(replay_refuses_an_inconsistent_journal)
{
    const ReplayFixture fx;
    const Records& good{fx.records};
    const auto replay{[&](const Records& records) { return state::ReplayDecisions(fx.genesis, records); }};

    // Records that are not the whole journal in order.
    Records gap{good};
    gap.erase(gap.begin() + 1);
    RequireFailure(replay(gap), ErrorCode::ORDER_VIOLATION, state::REPLAY_SEQ, "a missing record");
    // A decided height missing (both records of slot 2 gone).
    Records skipped{good};
    skipped.erase(skipped.begin() + 2, skipped.begin() + 4);
    RequireFailure(replay(Renumbered(skipped)), ErrorCode::ORDER_VIOLATION, state::REPLAY_DECISION_GAP, "slot 2 missing");
    // Another header for a replayed height.
    storage::DecisionRecord conflict{DecodeDecision(good[4])};
    conflict.header.outcome_count += 1;
    conflict.certificate.header_hash = conflict.header.Hash();
    Records conflicting{good};
    conflicting.push_back(DecisionRecordOf(conflict, good.size() + 1));
    RequireFailure(replay(conflicting), ErrorCode::EQUIVOCATION_REFUSED, storage::DECISION_CONFLICT, "another header for slot 3");
    // A decided header the body does not reproduce (here: its state root).
    storage::DecisionRecord forged{DecodeDecision(good[2])};
    forged.header.state_root = LabelHash("fm2/test/forged-root");
    forged.certificate.header_hash = forged.header.Hash();
    Records forged_records{good};
    forged_records[2] = DecisionRecordOf(forged, 3);
    RequireFailure(replay(forged_records), ErrorCode::VERIFY_FAILED, state::REEXECUTE_HEADER_MISMATCH, "forged state root");
    // A certificate that names another header.
    storage::DecisionRecord wrong_cert{DecodeDecision(good[2])};
    wrong_cert.certificate.header_hash = LabelHash("fm2/test/other-header");
    Records wrong_cert_records{good};
    wrong_cert_records[2] = DecisionRecordOf(wrong_cert, 3);
    RequireFailure(replay(wrong_cert_records), ErrorCode::VERIFY_FAILED, storage::DECISION_CERT_HEADER, "certificate of another header");
    // A body whose originals are not in canonical order.
    storage::DecisionRecord reordered{DecodeDecision(good[2])};
    BOOST_REQUIRE_EQUAL(reordered.body.originals.size(), 2U);
    std::swap(reordered.body.originals[0], reordered.body.originals[1]);
    Records reordered_records{good};
    reordered_records[2] = DecisionRecordOf(reordered, 3);
    RequireFailure(replay(reordered_records), ErrorCode::VERIFY_FAILED, state::REEXECUTE_BODY_MISMATCH, "body in another order");
    // A malformed DECISION payload.
    Records malformed{good};
    malformed[2].payload.resize(10);
    RequireError(replay(malformed), ErrorCode::DECODE, "truncated DECISION");
    // APPLIED with another state root, before its decision, and out of slot order.
    storage::AppliedRecord applied{storage::AppliedRecord::Decode(good[1].payload).value()};
    applied.state_root = LabelHash("fm2/test/other-root");
    Records wrong_root{good};
    wrong_root[1].payload = applied.Encode();
    RequireFailure(replay(wrong_root), ErrorCode::VERIFY_FAILED, state::REPLAY_APPLIED_MISMATCH, "APPLIED with another root");
    storage::AppliedRecord other_header{storage::AppliedRecord::Decode(good[1].payload).value()};
    other_header.header_hash = LabelHash("fm2/test/other-header");
    Records wrong_header{good};
    wrong_header[1].payload = other_header.Encode();
    RequireFailure(replay(wrong_header), ErrorCode::VERIFY_FAILED, state::REPLAY_APPLIED_MISMATCH, "APPLIED with another header");
    Records applied_first{good};
    std::swap(applied_first[0], applied_first[1]);
    RequireFailure(replay(Renumbered(applied_first)), ErrorCode::ORDER_VIOLATION, state::REPLAY_APPLIED_UNKNOWN, "APPLIED before DECISION");
    const Records out_of_order{Renumbered({good[0], good[2], good[3], good[1]})};
    RequireFailure(replay(out_of_order), ErrorCode::ORDER_VIOLATION, state::REPLAY_APPLIED_OUT_OF_ORDER, "APPLIED 1 after APPLIED 2");
    // Record types the commit journal never holds.
    Records signer_type{good};
    signer_type.push_back(storage::JournalRecord{good.size() + 1, storage::RecordType::INTENT, {}, uint256{}});
    RequireFailure(replay(signer_type), ErrorCode::ORDER_VIOLATION, storage::RECORD_ROLE, "a signer record");
    Records unknown_type{good};
    unknown_type.push_back(storage::JournalRecord{good.size() + 1, static_cast<storage::RecordType>(0x7f), {}, uint256{}});
    RequireFailure(replay(unknown_type), ErrorCode::DECODE, storage::RECORD_TYPE, "an unregistered type");
    // The base must be a genesis head, and the records must continue it.
    RequireFailure(state::ReplayDecisions(*fx.run.heads[0], good), ErrorCode::INVALID_INPUT, state::REPLAY_BASE_NOT_GENESIS,
                   "base at height 1");
    AuthorityVersion other_seats{TestAuthority(RegtestProfile())};
    other_seats.seat_set_root = LabelHash("fm2/test/other-seats");
    const std::vector<MarketDescriptor> markets{MarketM1(), MarketM2()};
    const state::HeadView other_genesis{state::GenesisHead(RegtestProfile(), markets, TestDomain(), other_seats,
                                                           TestAnchor(129), FactCursor{130, 0})
                                            .value()};
    RequireFailure(state::ReplayDecisions(other_genesis, good), ErrorCode::STALE_PARENT, state::PARENT_NOT_HEAD, "another genesis");
}

BOOST_AUTO_TEST_CASE(replay_of_snapshot_and_view_records_is_a_stub)
{
    const ReplayFixture fx;
    Records snapshot{fx.records};
    snapshot.push_back(storage::JournalRecord{snapshot.size() + 1, storage::RecordType::SNAPSHOT_MARK, {}, uint256{}});
    RequireStub(state::ReplayDecisions(fx.genesis, snapshot), "M4-STO-03");
    Records prepared{fx.records};
    prepared.push_back(storage::JournalRecord{prepared.size() + 1, storage::RecordType::PREPARED_QC, {}, uint256{}});
    RequireStub(state::ReplayDecisions(fx.genesis, prepared), "M4-STO-07");
}

BOOST_AUTO_TEST_SUITE_END()
