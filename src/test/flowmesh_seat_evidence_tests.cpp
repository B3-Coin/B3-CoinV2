// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/seat_evidence.h>

#include <flowmesh/bls_certificate.h>
#include <flowmesh/p2p.h>
#include <flowmesh/production_engine.h>
#include <flowmesh/seat_id.h>
#include <node/flowmesh_runtime.h>
#include <node/flowmesh_seat_evidence_recorder.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

uint256 Filled(const unsigned char value)
{
    uint256 out;
    std::fill(out.begin(), out.end(), value);
    return out;
}

bls::SecretKey Key(const uint32_t index, const unsigned char salt)
{
    std::array<unsigned char, 32> ikm{};
    for (size_t i{0}; i < ikm.size(); ++i) ikm[i] = static_cast<unsigned char>(salt + index * 17 + i * 11);
    const auto key{bls::SecretKey::FromIKM(ikm)};
    BOOST_REQUIRE(key.has_value());
    return *key;
}

struct SeatFixture {
    std::vector<bls::SecretKey> secrets;
    flowmesh::ActiveFnBlsSeatSet seats;
};

SeatFixture Seats(const uint256& domain, const flowmesh::MarketId& market, const size_t count,
                  const uint64_t epoch, const unsigned char salt)
{
    struct Entry {
        bls::SecretKey secret;
        flowmesh::BlsSeatBinding binding;
        flowmesh::SeatId seat_id;
    };
    std::vector<Entry> entries;
    for (size_t i{0}; i < count; ++i) {
        const bls::SecretKey secret{Key(i, salt)};
        flowmesh::BlsSeatBinding binding;
        binding.outpoint = COutPoint{Txid::FromUint256(Filled(static_cast<unsigned char>(salt + i + 30))),
                                     static_cast<uint32_t>(salt + i)};
        binding.public_key = secret.GetPublicKey().Compressed();
        binding.proof_of_possession = secret.SignPoP().Compressed();
        entries.push_back({secret, binding, flowmesh::ComputeFlowMeshSeatId(domain, binding.outpoint)});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.seat_id < b.seat_id || (a.seat_id == b.seat_id && a.binding.outpoint < b.binding.outpoint);
    });
    std::vector<flowmesh::BlsSeatBinding> bindings;
    SeatFixture out;
    for (const Entry& entry : entries) {
        out.secrets.push_back(entry.secret);
        bindings.push_back(entry.binding);
    }
    flowmesh::BlsSeatSetCheck check;
    const auto seats{flowmesh::BuildActiveFnBlsSeatSet(domain, market, epoch, 100 + epoch, Filled(static_cast<unsigned char>(0x70 + salt)),
                                                       bindings, check)};
    BOOST_REQUIRE(seats.has_value());
    out.seats = *seats;
    return out;
}

struct EvidenceFixture {
    uint256 domain{Filled(0x11)};
    flowmesh::MarketId market{Filled(0x22)};
    SeatFixture seats{Seats(domain, market, 4, 7, 1)};
    SeatFixture other_seats{Seats(domain, market, 4, 8, 9)};

    flowmesh::BlsCertificateContext Context(const uint256& hash, const uint64_t sequence = 5) const
    {
        return {domain, market, seats.seats.epoch, seats.seats.set_hash, sequence, hash};
    }
    bls::Signature Sign(const uint32_t seat, const flowmesh::BlsCertificateContext& context) const
    {
        const auto signature{flowmesh::SignBlsMicroblockCertificate(seats.secrets[seat], context, seats.seats)};
        BOOST_REQUIRE(signature);
        return *signature;
    }
    flowmesh::SeatEvidence Equivocation(const uint32_t seat = 2) const
    {
        const auto a{Context(Filled(0xa1))};
        const auto b{Context(Filled(0xb2))};
        return {flowmesh::SeatEvidenceKind::ATTESTATION_EQUIVOCATION, domain, market,
                flowmesh::SeatAttestationStatement(a, seat, Sign(seat, a)),
                flowmesh::SeatAttestationStatement(b, seat, Sign(seat, b)),
                seats.seats.members[seat].key.Key().Compressed()};
    }
    flowmesh::BlsMicroblockCertificate Certify(const flowmesh::BlsCertificateContext& context,
                                               const std::vector<uint32_t>& signers) const
    {
        std::vector<flowmesh::IndexedBlsSignature> partials;
        for (const uint32_t seat : signers) partials.push_back({seat, Sign(seat, context)});
        flowmesh::BlsCertificateAssemblyCheck check;
        const auto certificate{flowmesh::AssembleBlsMicroblockCertificate(context, seats.seats, partials, check)};
        BOOST_REQUIRE(certificate);
        return *certificate;
    }
    flowmesh::SeatEvidence Conflict() const
    {
        return {.kind = flowmesh::SeatEvidenceKind::CERTIFICATE_CONFLICT, .domain = domain, .market_id = market,
                .first = flowmesh::SeatCertificateStatement(Certify(Context(Filled(0xa1)), {0, 1, 2}), seats.seats.set_hash),
                .second = flowmesh::SeatCertificateStatement(Certify(Context(Filled(0xb2)), {1, 2, 3}), seats.seats.set_hash)};
    }
    flowmesh::SeatEvidenceCheck Check(const flowmesh::SeatEvidence& evidence) const
    {
        return flowmesh::CheckSeatEvidence(evidence, seats.seats, seats.seats);
    }
};

//! The members of the runtime's market that the seat-evidence recorder
//! (node/flowmesh_seat_evidence_recorder.h) uses, with an in-memory store,
//! chain and clock.
struct RecorderMarket {
    struct Store {
        std::vector<flowmesh::SeatEvidence> records; // newest first
        size_t writes{0};
        //! Every write throws, as an allocation failure would.
        bool throws{false};
        bool RecordSeatEvidence(const flowmesh::SeatEvidence& evidence, bool& inserted, std::string&)
        {
            if (throws) throw std::bad_alloc{};
            ++writes;
            inserted = std::none_of(records.begin(), records.end(),
                                    [&](const auto& record) { return record.Id() == evidence.Id(); });
            if (inserted) records.insert(records.begin(), evidence);
            return true;
        }
        bool ReadSeatEvidence(node::SeatEvidenceLog& log, std::string&) const
        {
            log.recorded = records.size();
            log.retained.clear();
            for (const auto& record : records) log.retained.push_back({0, record});
            return true;
        }
    };
    struct Chain {
        std::optional<flowmesh::ActiveFnBlsSeatSet> known;
        std::optional<flowmesh::ActiveFnBlsSeatSet> SeatSet(const uint256&, const flowmesh::MarketId&,
                                                            const uint64_t epoch, const uint256& set_hash) const
        {
            if (known && known->epoch == epoch && known->set_hash == set_hash) return known;
            return std::nullopt;
        }
    };
    struct Clock {
        flowmesh::WireClock::time_point now{};
        flowmesh::WireClock::time_point Now() const { return now; }
    };
    struct Candidate {
        flowmesh::ProductionEntryCore entry;
    };

    explicit RecorderMarket(const EvidenceFixture& f) : domain{f.domain}, market_id{f.market}, seats{f.seats.seats} {}

    uint256 domain;
    flowmesh::MarketId market_id;
    flowmesh::ActiveFnBlsSeatSet seats;
    Store store_impl;
    Chain chain_impl;
    Clock clock_impl;
    Store* store{&store_impl};
    const Chain* chain{&chain_impl};
    const Clock* clock{&clock_impl};
    uint64_t seat_evidence_recorded{0};
    std::shared_ptr<const std::vector<flowmesh::SeatEvidence>> seat_evidence;
    std::set<uint256> seat_evidence_ids;
    uint64_t seat_evidence_unrecorded{0};
    std::set<uint32_t> seat_evidence_slot_seats;
    flowmesh::WireClock::time_point seat_evidence_window{};
    size_t seat_evidence_window_checks{0};
    node::FlowMeshSeatEvidenceWork seat_evidence_work;
    std::map<uint256, Candidate> candidates;
    std::map<uint256, std::map<uint32_t, flowmesh::IndexedBlsSignature>> attestations;

    //! A candidate entry at `sequence` naming `epoch` and `set_hash`, told
    //! apart by `salt`; returns its hash.
    uint256 AddCandidate(const uint64_t sequence, const uint64_t epoch, const uint256& set_hash,
                         const unsigned char salt)
    {
        flowmesh::ProductionEntryCore entry;
        entry.domain = domain;
        entry.market_id = market_id;
        entry.epoch = epoch;
        entry.seat_set_hash = set_hash;
        entry.sequence = sequence;
        entry.parent_hash = Filled(0x61);
        entry.anchor = {100, Filled(0x71)};
        entry.previous_state_root = Filled(0x62);
        entry.actions_root = Filled(0x63);
        entry.result_root = Filled(0x64);
        entry.effect_root = Filled(0x65);
        entry.state_root = Filled(salt);
        const uint256 hash{entry.GetHash()};
        BOOST_REQUIRE(!hash.IsNull());
        BOOST_REQUIRE(candidates.emplace(hash, Candidate{entry}).second);
        return hash;
    }
    //! `seat`'s vote for candidate `hash`, as HandleAttestation keeps it: over
    //! the candidate's context when that names the fixture's set, otherwise
    //! over the same hash and sequence in that set's context.
    bls::Signature Vote(const EvidenceFixture& f, const uint32_t seat, const uint256& hash)
    {
        const auto signature{f.Sign(seat, f.Context(hash, candidates.at(hash).entry.sequence))};
        attestations[hash].insert_or_assign(seat, flowmesh::IndexedBlsSignature{seat, signature});
        return signature;
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_seat_evidence_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(attestation_equivocation_is_self_verifiable)
{
    EvidenceFixture f;
    const auto evidence{f.Equivocation()};
    BOOST_CHECK(f.Check(evidence) == flowmesh::SeatEvidenceCheck::OK);
    BOOST_CHECK(flowmesh::SeatEvidenceDoubleSigners(evidence).empty());
    // The identity ignores which statement a node happened to hold first.
    auto swapped{evidence};
    std::swap(swapped.first, swapped.second);
    BOOST_CHECK(swapped.Id() == evidence.Id());
    BOOST_CHECK(f.Check(swapped) == flowmesh::SeatEvidenceCheck::OK);

    // The same microblock twice, another sequence, another seat or another
    // epoch is no equivocation.
    auto same{evidence};
    same.second = same.first;
    BOOST_CHECK(f.Check(same) == flowmesh::SeatEvidenceCheck::NOT_CONFLICTING);
    auto later{evidence};
    const auto later_context{f.Context(Filled(0xb2), 6)};
    later.second = flowmesh::SeatAttestationStatement(later_context, 2, f.Sign(2, later_context));
    BOOST_CHECK(f.Check(later) == flowmesh::SeatEvidenceCheck::NOT_CONFLICTING);
    auto two_seats{evidence};
    const auto b{f.Context(Filled(0xb2))};
    two_seats.second = flowmesh::SeatAttestationStatement(b, 1, f.Sign(1, b));
    BOOST_CHECK(f.Check(two_seats) == flowmesh::SeatEvidenceCheck::NOT_CONFLICTING);
    auto other_epoch{evidence};
    other_epoch.second.seat_epoch = f.other_seats.seats.epoch;
    other_epoch.second.seat_set_hash = f.other_seats.seats.set_hash;
    BOOST_CHECK(f.Check(other_epoch) == flowmesh::SeatEvidenceCheck::NOT_CONFLICTING);

    // The checker uses the caller's chain-derived sets, never the record's.
    BOOST_CHECK(flowmesh::CheckSeatEvidence(evidence, f.other_seats.seats, f.seats.seats) ==
                flowmesh::SeatEvidenceCheck::WRONG_SEAT_SET);
    // A signature by another seat, or over another microblock, fails.
    auto forged_first{evidence};
    forged_first.first.signature = f.Sign(1, f.Context(Filled(0xa1))).Compressed();
    BOOST_CHECK(f.Check(forged_first) == flowmesh::SeatEvidenceCheck::BAD_FIRST_SIGNATURE);
    auto forged_second{evidence};
    forged_second.second.microblock_hash = Filled(0xc3);
    BOOST_CHECK(f.Check(forged_second) == flowmesh::SeatEvidenceCheck::BAD_SECOND_SIGNATURE);
    auto out_of_range{evidence};
    out_of_range.first.seat_index = out_of_range.second.seat_index = 4;
    BOOST_CHECK(f.Check(out_of_range) == flowmesh::SeatEvidenceCheck::WRONG_SEAT_KEY);
    // The named key must be the named seat's.
    auto other_key{evidence};
    other_key.seat_public_key = f.seats.seats.members[1].key.Key().Compressed();
    BOOST_CHECK(f.Check(other_key) == flowmesh::SeatEvidenceCheck::WRONG_SEAT_KEY);
    BOOST_CHECK(other_key.Id() == evidence.Id());
    // An attestation carries no bitmap and always names its key.
    auto shaped{evidence};
    shaped.first.signer_bitmap = {0x07};
    BOOST_CHECK(f.Check(shaped) == flowmesh::SeatEvidenceCheck::MALFORMED);
    BOOST_CHECK(!flowmesh::EncodeSeatEvidence(shaped));
    auto keyless{evidence};
    keyless.seat_public_key.reset();
    BOOST_CHECK(f.Check(keyless) == flowmesh::SeatEvidenceCheck::MALFORMED);
    BOOST_CHECK(!flowmesh::EncodeSeatEvidence(keyless));
}

BOOST_AUTO_TEST_CASE(certificate_conflict_is_self_verifiable_and_names_double_signers)
{
    EvidenceFixture f;
    const auto evidence{f.Conflict()};
    BOOST_CHECK(f.Check(evidence) == flowmesh::SeatEvidenceCheck::OK);
    // Seats 1 and 2 are in both quorums: each signed both microblocks.
    BOOST_CHECK((flowmesh::SeatEvidenceDoubleSigners(evidence) == std::vector<uint32_t>{1, 2}));

    auto tampered{evidence};
    tampered.second.signer_bitmap = {0x0f};
    BOOST_CHECK(f.Check(tampered) == flowmesh::SeatEvidenceCheck::BAD_SECOND_SIGNATURE);
    auto below_quorum{evidence};
    below_quorum.first.signer_bitmap = {0x03};
    BOOST_CHECK(f.Check(below_quorum) == flowmesh::SeatEvidenceCheck::BAD_FIRST_SIGNATURE);
    auto same{evidence};
    same.second = same.first;
    BOOST_CHECK(f.Check(same) == flowmesh::SeatEvidenceCheck::NOT_CONFLICTING);
    // A certificate statement names no single seat.
    auto shaped{evidence};
    shaped.first.seat_index = 1;
    BOOST_CHECK(f.Check(shaped) == flowmesh::SeatEvidenceCheck::MALFORMED);
    auto empty{evidence};
    empty.second.signer_bitmap.clear();
    BOOST_CHECK(f.Check(empty) == flowmesh::SeatEvidenceCheck::MALFORMED);
    // Nor a single key.
    auto keyed{evidence};
    keyed.seat_public_key = f.seats.seats.members[1].key.Key().Compressed();
    BOOST_CHECK(f.Check(keyed) == flowmesh::SeatEvidenceCheck::MALFORMED);
}

BOOST_AUTO_TEST_CASE(prechecked_steps_are_skipped_and_everything_else_still_checked)
{
    EvidenceFixture f;
    const auto check = [&](const flowmesh::SeatEvidence& evidence, const flowmesh::SeatEvidencePrechecked& prechecked,
                           flowmesh::SeatEvidenceWork& work, const flowmesh::ActiveFnBlsSeatSet* first = nullptr) {
        work = {};
        return flowmesh::CheckSeatEvidence(evidence, first ? *first : f.seats.seats, f.seats.seats, prechecked, &work);
    };
    const flowmesh::SeatEvidencePrechecked all{true, true, true, true};
    flowmesh::SeatEvidenceWork work;

    // The full check validates both sets and verifies both signatures.
    const auto equivocation{f.Equivocation()};
    BOOST_CHECK(check(equivocation, {}, work) == flowmesh::SeatEvidenceCheck::OK);
    BOOST_CHECK_EQUAL(work.seat_set_checks, 2U);
    BOOST_CHECK_EQUAL(work.signature_checks, 2U);
    // What the caller already did is not repeated.
    BOOST_CHECK(check(equivocation, all, work) == flowmesh::SeatEvidenceCheck::OK);
    BOOST_CHECK_EQUAL(work.seat_set_checks, 0U);
    BOOST_CHECK_EQUAL(work.signature_checks, 0U);
    BOOST_CHECK(check(equivocation, {true, true, false, true}, work) == flowmesh::SeatEvidenceCheck::OK);
    BOOST_CHECK_EQUAL(work.seat_set_checks, 0U);
    BOOST_CHECK_EQUAL(work.signature_checks, 1U);
    // Only what is named is skipped: an unflagged bad signature still fails.
    auto forged{equivocation};
    forged.second.signature = f.Sign(1, f.Context(Filled(0xb2))).Compressed();
    BOOST_CHECK(check(forged, {true, true, true, false}, work) == flowmesh::SeatEvidenceCheck::BAD_SECOND_SIGNATURE);
    BOOST_CHECK_EQUAL(work.signature_checks, 1U);
    // The cheap checks always run, whatever is flagged.
    auto same{equivocation};
    same.second = same.first;
    BOOST_CHECK(check(same, all, work) == flowmesh::SeatEvidenceCheck::NOT_CONFLICTING);
    BOOST_CHECK(check(equivocation, all, work, &f.other_seats.seats) == flowmesh::SeatEvidenceCheck::WRONG_SEAT_SET);
    auto other_key{equivocation};
    other_key.seat_public_key = f.seats.seats.members[1].key.Key().Compressed();
    BOOST_CHECK(check(other_key, all, work) == flowmesh::SeatEvidenceCheck::WRONG_SEAT_KEY);
    auto keyless{equivocation};
    keyless.seat_public_key.reset();
    BOOST_CHECK(check(keyless, all, work) == flowmesh::SeatEvidenceCheck::MALFORMED);
    BOOST_CHECK_EQUAL(work.seat_set_checks + work.signature_checks, 0U);

    // A certificate check validates its set again: two steps per statement.
    const auto conflict{f.Conflict()};
    BOOST_CHECK(check(conflict, {}, work) == flowmesh::SeatEvidenceCheck::OK);
    BOOST_CHECK_EQUAL(work.seat_set_checks, 4U);
    BOOST_CHECK_EQUAL(work.signature_checks, 2U);
    BOOST_CHECK(check(conflict, {true, true, false, true}, work) == flowmesh::SeatEvidenceCheck::OK);
    BOOST_CHECK_EQUAL(work.seat_set_checks, 1U);
    BOOST_CHECK_EQUAL(work.signature_checks, 1U);
    // Without a work sink the result is the same.
    BOOST_CHECK(flowmesh::CheckSeatEvidence(conflict, f.seats.seats, f.seats.seats, all) ==
                flowmesh::SeatEvidenceCheck::OK);
}

BOOST_AUTO_TEST_CASE(recorder_records_nothing_for_candidates_naming_another_epoch_or_set)
{
    EvidenceFixture f;
    RecorderMarket market{f};
    const auto& own{f.seats.seats};
    const auto& other{f.other_seats.seats};
    const uint256 first{market.AddCandidate(5, own.epoch, own.set_hash, 0xa1)};
    // Votes the recorder takes as verified are recorded only when both
    // candidate contexts name this market's installed set; otherwise the
    // detection is unrecorded before any check, and spends no pace.
    const uint256 other_epoch{market.AddCandidate(5, other.epoch, own.set_hash, 0xb2)};
    const uint256 other_set{market.AddCandidate(5, own.epoch, other.set_hash, 0xb3)};
    const uint256 other_both{market.AddCandidate(5, other.epoch, other.set_hash, 0xb4)};
    uint32_t seat{0};
    for (const uint256& second : {other_epoch, other_set, other_both}) {
        market.Vote(f, seat, first);
        const auto signature{market.Vote(f, seat, second)};
        node::RecordAttestationEquivocation(market, seat, first, second, signature);
        ++seat;
    }
    // The guard runs whichever candidate the seat voted for first.
    market.Vote(f, seat, other_set);
    node::RecordAttestationEquivocation(market, seat, other_set, first, market.Vote(f, seat, first));
    BOOST_CHECK_EQUAL(market.seat_evidence_unrecorded, 4U);
    BOOST_CHECK_EQUAL(market.seat_evidence_work.checks, 0U);
    BOOST_CHECK_EQUAL(market.seat_evidence_work.chain_lookups, 0U);
    BOOST_CHECK_EQUAL(market.seat_evidence_window_checks, 0U);
    BOOST_CHECK_EQUAL(market.store_impl.writes, 0U);
    BOOST_CHECK_EQUAL(market.seat_evidence_recorded, 0U);

    // Both contexts naming the installed set: checked once, without
    // verifying either vote again, and recorded.
    market.seat_evidence_slot_seats.clear();
    const uint256 second{market.AddCandidate(5, own.epoch, own.set_hash, 0xb5)};
    market.Vote(f, 1, first);
    node::RecordAttestationEquivocation(market, 1, first, second, market.Vote(f, 1, second));
    BOOST_CHECK_EQUAL(market.seat_evidence_unrecorded, 4U);
    BOOST_CHECK_EQUAL(market.seat_evidence_work.checks, 1U);
    BOOST_CHECK_EQUAL(market.seat_evidence_work.signature_checks, 0U);
    BOOST_CHECK_EQUAL(market.seat_evidence_work.seat_set_checks, 0U);
    BOOST_CHECK_EQUAL(market.store_impl.writes, 1U);
    BOOST_REQUIRE_EQUAL(market.seat_evidence_recorded, 1U);
    BOOST_CHECK(f.Check(market.seat_evidence->front()) == flowmesh::SeatEvidenceCheck::OK);
}

BOOST_AUTO_TEST_CASE(recorder_spends_the_pace_on_a_detection_that_fails_its_check)
{
    EvidenceFixture f;
    RecorderMarket market{f};
    // A stored certificate whose signature does not verify (corrupt on
    // disk, say): the detection is checked and fails.
    auto bad{f.Conflict()};
    bad.first.signature = bad.second.signature;
    BOOST_REQUIRE(f.Check(bad) == flowmesh::SeatEvidenceCheck::BAD_FIRST_SIGNATURE);
    node::RecordSeatEvidence(market, bad, /*first_signature_verified=*/false, /*second_signature_verified=*/true);
    BOOST_CHECK_EQUAL(market.seat_evidence_work.checks, 1U);
    BOOST_CHECK_EQUAL(market.seat_evidence_work.signature_checks, 1U);
    BOOST_CHECK_EQUAL(market.seat_evidence_window_checks, 1U);
    BOOST_CHECK_EQUAL(market.seat_evidence_unrecorded, 1U);
    BOOST_CHECK_EQUAL(market.store_impl.writes, 0U);
    // One naming a seat set this node's chain does not have fails too, after
    // one chain lookup and no signature check.
    auto unknown{f.Conflict()};
    unknown.second.seat_set_hash = f.other_seats.seats.set_hash;
    node::RecordSeatEvidence(market, unknown, false, true);
    BOOST_CHECK_EQUAL(market.seat_evidence_work.checks, 2U);
    BOOST_CHECK_EQUAL(market.seat_evidence_work.chain_lookups, 1U);
    BOOST_CHECK_EQUAL(market.seat_evidence_work.signature_checks, 1U);
    BOOST_CHECK_EQUAL(market.seat_evidence_unrecorded, 2U);
    // Failed checks are not remembered, so repeats spend the pace until it
    // is gone; then a valid detection costs nothing and is not recorded.
    for (size_t n{2}; n < node::SEAT_EVIDENCE_CHECKS_PER_MINUTE; ++n) node::RecordSeatEvidence(market, bad, false, true);
    BOOST_CHECK_EQUAL(market.seat_evidence_window_checks, node::SEAT_EVIDENCE_CHECKS_PER_MINUTE);
    const auto good{f.Conflict()};
    node::RecordSeatEvidence(market, good, false, true);
    BOOST_CHECK_EQUAL(market.seat_evidence_work.checks, node::SEAT_EVIDENCE_CHECKS_PER_MINUTE);
    BOOST_CHECK_EQUAL(market.seat_evidence_unrecorded, node::SEAT_EVIDENCE_CHECKS_PER_MINUTE + 1);
    BOOST_CHECK_EQUAL(market.store_impl.writes, 0U);
    // The next minute checks it and records it.
    market.clock_impl.now += std::chrono::minutes{1};
    node::RecordSeatEvidence(market, good, false, true);
    BOOST_CHECK_EQUAL(market.store_impl.writes, 1U);
    BOOST_CHECK_EQUAL(market.seat_evidence_recorded, 1U);
    BOOST_CHECK_EQUAL(market.seat_evidence_unrecorded, node::SEAT_EVIDENCE_CHECKS_PER_MINUTE + 1);
}

BOOST_AUTO_TEST_CASE(recorder_keeps_a_check_each_minute_for_a_certificate_conflict)
{
    EvidenceFixture f;
    RecorderMarket market{f};
    // Eight different equivocations in one minute: seats 0..3 at two
    // sequences.
    const auto equivocation = [&](const uint32_t seat, const uint64_t sequence) {
        const auto a{f.Context(Filled(0xa1), sequence)};
        const auto b{f.Context(Filled(0xb2), sequence)};
        return flowmesh::SeatEvidence{flowmesh::SeatEvidenceKind::ATTESTATION_EQUIVOCATION, f.domain, f.market,
                                      flowmesh::SeatAttestationStatement(a, seat, f.Sign(seat, a)),
                                      flowmesh::SeatAttestationStatement(b, seat, f.Sign(seat, b)),
                                      f.seats.seats.members[seat].key.Key().Compressed()};
    };
    for (const uint64_t sequence : {5U, 6U}) {
        for (uint32_t seat{0}; seat < 4; ++seat) node::RecordSeatEvidence(market, equivocation(seat, sequence), false, false);
    }
    // They stop one check short of the minute's eight.
    const size_t equivocations{node::SEAT_EVIDENCE_CHECKS_PER_MINUTE - node::SEAT_EVIDENCE_CONFLICT_RESERVE};
    BOOST_CHECK_EQUAL(equivocations, 7U);
    BOOST_CHECK_EQUAL(market.seat_evidence_work.checks, equivocations);
    BOOST_CHECK_EQUAL(market.seat_evidence_recorded, equivocations);
    BOOST_CHECK_EQUAL(market.seat_evidence_unrecorded, 1U);
    // A certificate conflict in the same minute is still checked and recorded.
    node::RecordSeatEvidence(market, f.Conflict(), false, false);
    BOOST_CHECK_EQUAL(market.seat_evidence_work.checks, node::SEAT_EVIDENCE_CHECKS_PER_MINUTE);
    BOOST_CHECK_EQUAL(market.seat_evidence_recorded, equivocations + 1);
    BOOST_REQUIRE(market.seat_evidence);
    BOOST_CHECK(market.seat_evidence->front().kind == flowmesh::SeatEvidenceKind::CERTIFICATE_CONFLICT);
    // The minute's pace still bounds everything: nothing more is checked.
    auto another{f.Conflict()};
    std::swap(another.first, another.second);
    another.second.microblock_hash = Filled(0xc3);
    node::RecordSeatEvidence(market, another, false, false);
    node::RecordSeatEvidence(market, equivocation(0, 7), false, false);
    BOOST_CHECK_EQUAL(market.seat_evidence_work.checks, node::SEAT_EVIDENCE_CHECKS_PER_MINUTE);
    BOOST_CHECK_EQUAL(market.seat_evidence_unrecorded, 3U);
    BOOST_CHECK_EQUAL(market.store_impl.writes, equivocations + 1);
}

BOOST_AUTO_TEST_CASE(recorder_failure_never_reaches_its_caller)
{
    EvidenceFixture f;
    RecorderMarket market{f};
    // HandleCertificate halts the market around recording a conflict; no
    // failure while recording, an exception included, may reach it.
    market.store_impl.throws = true;
    BOOST_CHECK_NO_THROW(node::RecordSeatEvidence(market, f.Conflict(), false, false));
    BOOST_CHECK_EQUAL(market.seat_evidence_work.checks, 1U);
    BOOST_CHECK_EQUAL(market.seat_evidence_unrecorded, 1U);
    BOOST_CHECK_EQUAL(market.seat_evidence_recorded, 0U);
    // Nothing is left half done: once the store works, it is recorded.
    market.store_impl.throws = false;
    node::RecordSeatEvidence(market, f.Conflict(), false, false);
    BOOST_CHECK_EQUAL(market.seat_evidence_recorded, 1U);
    BOOST_CHECK_EQUAL(market.seat_evidence_unrecorded, 1U);
}

BOOST_AUTO_TEST_CASE(encoding_is_canonical_and_strict)
{
    EvidenceFixture f;
    for (const auto& evidence : {f.Equivocation(), f.Conflict()}) {
        const auto bytes{flowmesh::EncodeSeatEvidence(evidence)};
        BOOST_REQUIRE(bytes);
        BOOST_CHECK_LE(bytes->size(), flowmesh::SEAT_EVIDENCE_MAX_BYTES);
        const auto decoded{flowmesh::DecodeSeatEvidence(*bytes)};
        BOOST_REQUIRE(decoded);
        BOOST_CHECK(*decoded == evidence);
        BOOST_CHECK(decoded->Id() == evidence.Id());
        BOOST_CHECK(f.Check(*decoded) == flowmesh::SeatEvidenceCheck::OK);

        auto truncated{*bytes};
        truncated.pop_back();
        BOOST_CHECK(!flowmesh::DecodeSeatEvidence(truncated));
        auto extended{*bytes};
        extended.push_back(0);
        BOOST_CHECK(!flowmesh::DecodeSeatEvidence(extended));
        auto version{*bytes};
        version[0] = flowmesh::SEAT_EVIDENCE_VERSION + 1;
        BOOST_CHECK(!flowmesh::DecodeSeatEvidence(version));
        auto kind{*bytes};
        kind[1] = 3;
        BOOST_CHECK(!flowmesh::DecodeSeatEvidence(kind));
        // Kinds are not interchangeable: each has its own statement shape.
        auto other_kind{*bytes};
        other_kind[1] = evidence.kind == flowmesh::SeatEvidenceKind::CERTIFICATE_CONFLICT ? 1 : 2;
        BOOST_CHECK(!flowmesh::DecodeSeatEvidence(other_kind));
    }
    BOOST_CHECK(!flowmesh::DecodeSeatEvidence({}));
    // A bitmap larger than the largest seat set is refused before it is read.
    auto oversized{f.Conflict()};
    oversized.first.signer_bitmap.assign(flowmesh::FLOWMESH_MAX_SIGNER_BITMAP_BYTES + 1, 0xff);
    BOOST_CHECK(!flowmesh::EncodeSeatEvidence(oversized));
    auto null_market{f.Equivocation()};
    null_market.market_id.SetNull();
    BOOST_CHECK(!flowmesh::EncodeSeatEvidence(null_market));
    BOOST_CHECK(f.Check(null_market) == flowmesh::SeatEvidenceCheck::MALFORMED);
}

BOOST_AUTO_TEST_SUITE_END()
