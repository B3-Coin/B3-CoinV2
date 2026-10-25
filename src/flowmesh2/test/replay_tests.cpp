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
    BOOST_CHECK(empty->decided_head.Header() == fx.genesis.Header());
    BOOST_CHECK(result->decided_head.Header() == result->head.Header());
    // A decided slot whose APPLIED record is missing (a crash before
    // B-APPLIED, or check 3 refused it) is re-executed and reported, but the
    // committed head is the last applied slot, as on the live node (O-89).
    const Records cut{fx.records.begin(), fx.records.end() - 1};
    const Result<state::ReplayResult> partial{state::ReplayDecisions(fx.genesis, cut)};
    RequireOk(partial, "replay without the last APPLIED");
    BOOST_REQUIRE_EQUAL(partial->slots.size(), 5U);
    BOOST_CHECK(!partial->slots.back().applied_seq.has_value());
    BOOST_CHECK(partial->head.Header() == fx.run.heads[3]->Header());
    BOOST_CHECK(partial->head.CertHash() == fx.run.heads[3]->CertHash());
    BOOST_CHECK(partial->head.State() == fx.run.heads[3]->State());
    BOOST_CHECK(partial->decided_head.Header() == fx.f.Head()->Header());
    BOOST_CHECK(partial->decided_head.State() == fx.f.Head()->State());
    // Without any APPLIED record nothing is committed: the head is the genesis head.
    Records decisions_only;
    for (const storage::JournalRecord& record : fx.records) {
        if (record.type == storage::RecordType::DECISION) decisions_only.push_back(record);
    }
    const Result<state::ReplayResult> undecided{state::ReplayDecisions(fx.genesis, Renumbered(decisions_only))};
    RequireOk(undecided, "replay of decisions without APPLIED");
    BOOST_CHECK_EQUAL(undecided->slots.size(), 5U);
    BOOST_CHECK(undecided->head.Header() == fx.genesis.Header());
    BOOST_CHECK(undecided->decided_head.Header() == fx.f.Head()->Header());
    // APPLIED records that follow later decisions still give the head of their own slot.
    const Records late{Renumbered({fx.records[0], fx.records[2], fx.records[1]})};
    const Result<state::ReplayResult> late_applied{state::ReplayDecisions(fx.genesis, late)};
    RequireOk(late_applied, "APPLIED 1 after DECISION 2");
    BOOST_CHECK(late_applied->head.Header() == fx.run.heads[0]->Header());
    BOOST_CHECK(late_applied->decided_head.Header() == fx.run.heads[1]->Header());
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
    // An APPLIED that skips an unapplied slot: the committed head would hold
    // slot 2's effects without slot 2 being committed.
    const Records skipped_applied{Renumbered({good[0], good[1], good[2], good[4], good[5]})};
    RequireFailure(replay(skipped_applied), ErrorCode::ORDER_VIOLATION, state::REPLAY_APPLIED_OUT_OF_ORDER, "APPLIED 3 without APPLIED 2");
    // Record types the commit journal never holds.
    Records signer_type{good};
    signer_type.push_back(storage::JournalRecord{good.size() + 1, storage::RecordType::INTENT, {}, uint256{}});
    RequireFailure(replay(signer_type), ErrorCode::ORDER_VIOLATION, storage::RECORD_ROLE, "a signer record");
    Records unknown_type{good};
    unknown_type.push_back(storage::JournalRecord{good.size() + 1, static_cast<storage::RecordType>(0x7f), {}, uint256{}});
    RequireFailure(replay(unknown_type), ErrorCode::DECODE, storage::RECORD_TYPE, "an unregistered type");
    // The records must continue the base (a snapshot base: replay_from_a_snapshot_base_equals_replay_from_genesis).
    AuthorityVersion other_seats{TestAuthority(RegtestProfile())};
    other_seats.seat_set_root = LabelHash("fm2/test/other-seats");
    const std::vector<MarketDescriptor> markets{MarketM1(), MarketM2()};
    const state::HeadView other_genesis{state::GenesisHead(RegtestProfile(), markets, TestDomain(), other_seats,
                                                           TestAnchor(129), FactCursor{130, 0})
                                            .value()};
    RequireFailure(state::ReplayDecisions(other_genesis, good), ErrorCode::STALE_PARENT, state::PARENT_NOT_HEAD, "another genesis");
}

BOOST_AUTO_TEST_CASE(replay_from_a_snapshot_base_equals_replay_from_genesis)
{
    // Every committed head is a base: the records below it are checked as a
    // chain, the ones above it re-executed, and the result is the genesis
    // replay's (M4-STO-03, O-95).
    const ReplayFixture fx;
    const Result<state::ReplayResult> from_genesis{state::ReplayDecisions(fx.genesis, fx.records)};
    RequireOk(from_genesis, "replay from genesis");
    for (size_t k{0}; k < fx.run.heads.size(); ++k) {
        const state::HeadView& base{*fx.run.heads[k]};
        const Result<state::ReplayResult> result{state::ReplayDecisions(base, fx.records)};
        RequireOk(result, "replay from the head of slot " + std::to_string(k + 1));
        RequireReproduced(*result, fx);
        BOOST_CHECK(result->slots == from_genesis->slots);
        BOOST_CHECK(result->decided_head.Header() == from_genesis->decided_head.Header());
        // The records below the base must be there, and must apply it.
        const Records undecided{fx.records.begin(), fx.records.begin() + static_cast<std::ptrdiff_t>(2 * k + 1)};
        RequireFailure(state::ReplayDecisions(base, undecided), ErrorCode::INVALID_INPUT, state::REPLAY_BASE_UNKNOWN,
                       "base decided but not applied");
        const Records short_of_base{fx.records.begin(), fx.records.begin() + static_cast<std::ptrdiff_t>(2 * k)};
        RequireFailure(state::ReplayDecisions(base, short_of_base), ErrorCode::INVALID_INPUT, state::REPLAY_BASE_UNKNOWN,
                       "base never decided");
    }
    // A base the records do not decide: the head of another run at slot 4.
    const ReplayFixture other{[](OwnerFixture& f) { f.Run(f.Inputs({}, {})); }}; // a heartbeat first
    BOOST_REQUIRE(!(other.run.heads[2]->Header() == fx.run.heads[2]->Header()));
    RequireFailure(state::ReplayDecisions(*other.run.heads[2], fx.records), ErrorCode::VERIFY_FAILED, state::REPLAY_BASE_CHAIN,
                   "another chain's head");
    // A decision below the base that does not chain: slot 1 with another header (and a certificate naming it).
    storage::DecisionRecord forged{DecodeDecision(fx.records[0])};
    forged.header.state_root = LabelHash("fm2/test/forged-root");
    forged.certificate = TestCertificate(forged.header);
    Records broken{fx.records};
    broken[0] = DecisionRecordOf(forged, 1);
    broken.erase(broken.begin() + 1); // its APPLIED would name the original header
    RequireFailure(state::ReplayDecisions(*fx.run.heads[2], Renumbered(broken)), ErrorCode::VERIFY_FAILED, state::REPLAY_BASE_CHAIN,
                   "slot 2 does not name the forged slot 1 as parent");
    // The base header at its height with another certificate.
    storage::DecisionRecord recertified{DecodeDecision(fx.records[4])};
    recertified.certificate = TestCertificate(recertified.header, /*variant=*/7);
    Records other_cert{fx.records};
    other_cert[4] = DecisionRecordOf(recertified, 5);
    RequireFailure(state::ReplayDecisions(*fx.run.heads[2], other_cert), ErrorCode::VERIFY_FAILED, state::REPLAY_BASE_CHAIN,
                   "the base decided with another certificate");
}

BOOST_AUTO_TEST_CASE(snapshot_marks_name_applied_slots_and_move_no_head)
{
    const ReplayFixture fx;
    const auto mark_of{[&](size_t slot) {
        const SlotHeaderV2& header{fx.run.heads[slot - 1]->Header()};
        return storage::SnapshotMark{slot, header.Hash(), header.state_root};
    }};
    const auto with{[&](Records records, size_t at, const std::vector<unsigned char>& payload) {
        records.insert(records.begin() + static_cast<std::ptrdiff_t>(at),
                       storage::JournalRecord{0, storage::RecordType::SNAPSHOT_MARK, payload, uint256{}});
        return Renumbered(records);
    }};
    // Marks of slots 2 and 4 (slot 2 twice), each after its APPLIED.
    const Records marked{with(with(with(fx.records, 8, mark_of(4).Encode()), 4, mark_of(2).Encode()), 5, mark_of(2).Encode())};
    const Result<state::ReplayResult> result{state::ReplayDecisions(fx.genesis, marked)};
    RequireOk(result, "replay with marks");
    RequireReproduced(*result, fx);
    BOOST_CHECK((result->marks == std::vector<storage::SnapshotMark>{mark_of(2), mark_of(4)}));
    // A mark before its slot's APPLIED, of slot 0, or of a slot never applied.
    RequireFailure(state::ReplayDecisions(fx.genesis, with(fx.records, 3, mark_of(2).Encode())), ErrorCode::ORDER_VIOLATION,
                   state::REPLAY_MARK_UNAPPLIED, "mark before APPLIED");
    RequireFailure(state::ReplayDecisions(fx.genesis, with(fx.records, 2, storage::SnapshotMark{0, fx.genesis.HeaderHash(), fx.genesis.Header().state_root}.Encode())),
                   ErrorCode::ORDER_VIOLATION, state::REPLAY_MARK_UNAPPLIED, "mark of slot 0");
    storage::SnapshotMark beyond{mark_of(5)};
    beyond.slot = 6;
    RequireFailure(state::ReplayDecisions(fx.genesis, with(fx.records, 10, beyond.Encode())), ErrorCode::ORDER_VIOLATION,
                   state::REPLAY_MARK_UNAPPLIED, "mark of an undecided slot");
    storage::SnapshotMark other_root{mark_of(3)};
    other_root.state_root = LabelHash("fm2/test/other-root");
    RequireFailure(state::ReplayDecisions(fx.genesis, with(fx.records, 6, other_root.Encode())), ErrorCode::VERIFY_FAILED,
                   state::REPLAY_MARK_MISMATCH, "mark with another root");
    storage::SnapshotMark other_header{mark_of(3)};
    other_header.header_hash = mark_of(2).header_hash;
    RequireFailure(state::ReplayDecisions(fx.genesis, with(fx.records, 6, other_header.Encode())), ErrorCode::VERIFY_FAILED,
                   state::REPLAY_MARK_MISMATCH, "mark with another header");
    RequireError(state::ReplayDecisions(fx.genesis, with(fx.records, 6, {})), ErrorCode::DECODE, "empty mark");
}

// The name is historical: neither record is a stub any more (SNAPSHOT_MARK:
// snapshot_marks_name_applied_slots_and_move_no_head). Renaming the case
// removes an INVENTORY.txt line, which needs an audit and owner sign-off.
BOOST_AUTO_TEST_CASE(replay_of_snapshot_and_view_records_is_a_stub)
{
    const ReplayFixture fx;
    Records prepared{fx.records};
    prepared.push_back(storage::JournalRecord{prepared.size() + 1, storage::RecordType::PREPARED_QC, {}, uint256{}});
    RequireError(state::ReplayDecisions(fx.genesis, prepared), ErrorCode::DECODE, "truncated prepared value");
}

BOOST_AUTO_TEST_SUITE_END()
