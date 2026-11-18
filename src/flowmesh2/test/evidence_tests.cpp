// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/agreement/digests.h>
#include <flowmesh2/agreement/evidence.h>
#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/test/util/agreement_fixtures.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/require.h>
#include <flowmesh2/test/util/verify_fixtures.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

/** The evidence store (M4-AGR-07): a PREPARE double vote is checked, its
 * signatures verified against the epoch's seat set, persisted as an EVIDENCE
 * record behind a barrier and halts the store, also after a recovery; a
 * COMMIT double vote is unverifiable from the votes alone; conflicting
 * certificates need a quorum each.
 */
using namespace flowmesh2;
using namespace flowmesh2::agreement;
using namespace flowmesh2::test;

namespace {

Vote PrepareVote(const uint256& domain, const std::string& seat_name, uint32_t seat, uint64_t epoch, uint64_t view, uint64_t slot,
                 const uint256& header_hash)
{
    Vote vote;
    vote.phase = storage::VotePhase::PREPARE;
    vote.epoch = epoch;
    vote.view = view;
    vote.slot = slot;
    vote.header_hash = header_hash;
    vote.seat = seat;
    const uint256 digest{PrepareDigest(domain, epoch, slot, view, header_hash)};
    vote.signature = BlsKeyFromLabel(SeatLabel(seat_name)).Sign(std::span<const unsigned char>{digest.data(), digest.size()}).Compressed();
    return vote;
}

Evidence Ordered(Vote a, Vote b)
{
    if (!(a.header_hash < b.header_hash)) std::swap(a, b);
    return Evidence{DoubleVote{std::move(a), std::move(b)}};
}

} // namespace

BOOST_AUTO_TEST_SUITE(evidence_tests)

BOOST_AUTO_TEST_CASE(a_prepare_double_vote_is_verified_persisted_and_halts)
{
    RigOptions options;
    options.seat_names = {"s0", "s1", "s2", "s3"};
    AgreementRig rig{options};
    const uint256 domain{rig.genesis.Header().domain};
    Result<EvidenceStore> created{EvidenceStore::Create(rig.profile, domain, *rig.adapter, rig.commit_log)};
    RequireOk(created, "EvidenceStore::Create");
    EvidenceStore& store{*created};
    BOOST_CHECK(!store.Halted());
    BOOST_CHECK_EQUAL(store.Size(), 0U);
    const uint256 h1{LabelHash("fm2/test/evidence/h1")};
    const uint256 h2{LabelHash("fm2/test/evidence/h2")};
    const Evidence doubled{Ordered(PrepareVote(domain, "s2", 2, 0, 0, 1, h1), PrepareVote(domain, "s2", 2, 0, 0, 1, h2))};
    // Wrong order, a forged signature, a seat outside the set, a COMMIT double vote: refused, nothing recorded.
    {
        const auto& dv{std::get<DoubleVote>(doubled.content)};
        RequireRefusal(store.Record(Evidence{DoubleVote{dv.second, dv.first}}), ErrorCode::INVALID_INPUT, EVIDENCE_ORDER, "order");
        Evidence forged{doubled};
        std::get<DoubleVote>(forged.content).second.signature[5] ^= 0x01;
        RequireRefusal(store.Record(forged), ErrorCode::VERIFY_FAILED, EVIDENCE_SIGNATURE, "forged");
        Evidence impostor{Ordered(PrepareVote(domain, "s3", 2, 0, 0, 1, h1), PrepareVote(domain, "s2", 2, 0, 0, 1, h2))};
        RequireRefusal(store.Record(impostor), ErrorCode::VERIFY_FAILED, EVIDENCE_SIGNATURE, "another seat's key");
        Evidence outside{doubled};
        std::get<DoubleVote>(outside.content).first.seat = 7;
        std::get<DoubleVote>(outside.content).second.seat = 7;
        RequireRefusal(store.Record(outside), ErrorCode::INVALID_INPUT, EVIDENCE_SEAT, "seat outside the set");
        Evidence commits{doubled};
        std::get<DoubleVote>(commits.content).first.phase = storage::VotePhase::COMMIT;
        std::get<DoubleVote>(commits.content).second.phase = storage::VotePhase::COMMIT;
        RequireRefusal(store.Record(commits), ErrorCode::INVALID_INPUT, EVIDENCE_UNVERIFIABLE, "commit double vote");
        Evidence same{doubled};
        std::get<DoubleVote>(same.content).second.header_hash = h1;
        RequireRefusal(store.Record(same), ErrorCode::INVALID_INPUT, EVIDENCE_NOT_CONFLICTING, "no conflict");
    }
    BOOST_CHECK(!store.Halted());
    BOOST_CHECK_EQUAL(rig.commit_journal->Status().durable_seq, 0U);
    // The real thing: durable before Record returns, then halted; again is idempotent.
    RequireOk(store.Record(doubled), "Record");
    BOOST_CHECK(store.Halted());
    BOOST_CHECK_EQUAL(store.Size(), 1U);
    const Result<std::vector<storage::JournalRecord>> records{rig.commit_journal->ReadAll()};
    RequireOk(records, "ReadAll");
    BOOST_REQUIRE_EQUAL(records->size(), 1U);
    BOOST_CHECK(records->front().type == storage::RecordType::EVIDENCE);
    BOOST_CHECK_EQUAL(rig.commit_journal->Status().durable_seq, 1U);
    BOOST_CHECK(DecodeMessage(records->front().payload, rig.profile).value() == Message{doubled});
    RequireOk(store.Record(doubled), "Record again");
    BOOST_CHECK_EQUAL(store.Size(), 1U);
    BOOST_CHECK_EQUAL(rig.commit_journal->Status().durable_seq, 1U);
    BOOST_CHECK(store.Load().value() == std::vector<Evidence>{doubled});
    // A recovered log: the record is an evidence record, the store halted from the start.
    Result<storage::CommitLog> recovered{storage::CommitLog::Recover(*rig.commit_journal, rig.profile)};
    RequireOk(recovered, "Recover");
    BOOST_CHECK_EQUAL(recovered->EvidenceRecords().size(), 1U);
    Result<EvidenceStore> again{EvidenceStore::Create(rig.profile, domain, *rig.adapter, *recovered)};
    RequireOk(again, "Create after a recovery");
    BOOST_CHECK(again->Halted());
    BOOST_CHECK(again->Load().value() == std::vector<Evidence>{doubled});
    // A commit log refuses an empty evidence record.
    RequireRefusal(recovered->AppendEvidence({}), ErrorCode::INVALID_INPUT, storage::EVIDENCE_EMPTY, "empty");
}

BOOST_AUTO_TEST_CASE(conflicting_certificates_are_evidence_and_weak_ones_are_not)
{
    RigOptions options;
    options.seat_names = {"s0", "s1", "s2", "s3"};
    AgreementRig rig{options};
    EvidenceStore store{EvidenceStore::Create(rig.profile, rig.genesis.Header().domain, *rig.adapter, rig.commit_log).value()};
    const StateRootsV0 roots_a{LabelHash("fm2/test/evidence/a/accounts"), LabelHash("fm2/test/evidence/a/orders"),
                               LabelHash("fm2/test/evidence/a/markets"), LabelHash("fm2/test/evidence/a/nullifiers"),
                               LabelHash("fm2/test/evidence/a/totals"), uint256{}};
    StateRootsV0 roots_b{roots_a};
    roots_b.accounts = LabelHash("fm2/test/evidence/b/accounts");
    SlotHeaderV2 a{SyntheticHeader(rig.seats, 1, roots_a)};
    SlotHeaderV2 b{SyntheticHeader(rig.seats, 1, roots_b)};
    if (!(a.Hash() < b.Hash())) std::swap(a, b);
    const std::vector<std::string>& names{options.seat_names};
    CertifiedHeader first{a, SignCertificate(a, names, {0, 1, 2})};
    CertifiedHeader second{b, SignCertificate(b, names, {1, 2, 3})};
    // A certificate below the quorum, and one whose aggregate is not its bitmap's, do not prove a conflict.
    RequireRefusal(store.Record(Evidence{ConflictingCertificates{first, CertifiedHeader{b, SignCertificate(b, names, {3})}}}),
                   ErrorCode::VERIFY_FAILED, EVIDENCE_CERTIFICATE, "below the quorum");
    RequireRefusal(store.Record(Evidence{ConflictingCertificates{first, CertifiedHeader{b, SignCertificate(b, names, {1, 2, 3}, std::vector<uint32_t>{0, 1, 2})}}}),
                   ErrorCode::VERIFY_FAILED, EVIDENCE_CERTIFICATE, "aggregate of other seats");
    SlotHeaderV2 other_height{SyntheticHeader(rig.seats, 2, roots_b)};
    RequireRefusal(store.Record(Evidence{ConflictingCertificates{first, CertifiedHeader{other_height, SignCertificate(other_height, names, {1, 2, 3})}}}),
                   ErrorCode::INVALID_INPUT, EVIDENCE_NOT_CONFLICTING, "another height");
    BOOST_CHECK(!store.Halted());
    RequireOk(store.Record(Evidence{ConflictingCertificates{first, second}}), "two certified headers of one height");
    BOOST_CHECK(store.Halted());
    BOOST_CHECK_EQUAL(store.Size(), 1U);
    BOOST_CHECK(store.Load().value().front().Kind() == EvidenceKind::CONFLICTING_CERTIFICATES);
}

BOOST_AUTO_TEST_SUITE_END()
