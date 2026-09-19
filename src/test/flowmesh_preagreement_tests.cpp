// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/preagreement.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <vector>

namespace {

using namespace flowmesh;

uint256 Filled(const unsigned char byte)
{
    uint256 result;
    std::fill(result.begin(), result.end(), byte);
    return result;
}

// Generated in-memory test secrets only. There is deliberately no production
// signing API, signing store, active runtime, or live data in this test target.
struct Fixture {
    PreagreementContext context;
    ActiveFnBlsSeatSet seats;
    std::vector<bls::SecretKey> secrets;

    Fixture()
    {
        struct Entry {
            bls::SecretKey secret;
            BlsSeatBinding binding;
            SeatId id;
        };
        context = {Filled(0x11), Filled(0x22), 17, {}, 123,
                   Filled(0x33), Filled(0x44), Filled(0x55)};
        std::vector<Entry> entries;
        for (uint32_t i{0}; i < 9; ++i) {
            std::array<unsigned char, 32> ikm{};
            ikm.fill(static_cast<unsigned char>(i + 1));
            const auto secret{bls::SecretKey::FromIKM(ikm)};
            BOOST_REQUIRE(secret);
            BlsSeatBinding binding;
            binding.outpoint = {Txid::FromUint256(Filled(static_cast<unsigned char>(i + 80))), i};
            binding.public_key = secret->GetPublicKey().Compressed();
            binding.proof_of_possession = secret->SignPoP().Compressed();
            entries.push_back({*secret, binding, ComputeFlowMeshSeatId(context.domain, binding.outpoint)});
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.id < b.id || (a.id == b.id && a.binding.outpoint < b.binding.outpoint);
        });
        std::vector<BlsSeatBinding> bindings;
        for (const auto& entry : entries) {
            secrets.push_back(entry.secret);
            bindings.push_back(entry.binding);
        }
        BlsSeatSetCheck check{BlsSeatSetCheck::BAD_PUBLIC_KEY};
        const auto built{BuildActiveFnBlsSeatSet(context.domain, context.market_id, context.epoch,
            100, Filled(0x66), bindings, check)};
        BOOST_REQUIRE(built);
        BOOST_REQUIRE(check == BlsSeatSetCheck::OK);
        seats = *built;
        context.seat_set_hash = seats.set_hash;
        BOOST_REQUIRE_EQUAL(FlowMeshBlsThreshold(seats.Size()), 7U);
    }

    IndexedBlsSignature Vote(const PreagreementPhase phase, const uint32_t view,
                             const uint256& candidate, const uint32_t seat) const
    {
        const auto digest{PreagreementVoteDigest(context, phase, view, candidate, seat)};
        BOOST_REQUIRE(digest);
        return {seat, secrets.at(seat).Sign(std::span<const unsigned char>{digest->begin(), 32})};
    }

    PreagreementPreparedCertificate Prepared(const uint32_t view, const uint256& candidate,
                                             const std::vector<uint32_t>& indices = {0, 1, 2, 3, 4, 5, 6}) const
    {
        PreagreementPreparedCertificate proof{view, candidate, {}};
        for (const uint32_t seat : indices) proof.votes.push_back(Vote(PreagreementPhase::PREPARE, view, candidate, seat));
        return proof;
    }

    void SignReport(PreagreementViewChange& report) const
    {
        const auto digest{PreagreementViewChangeDigest(context, report)};
        BOOST_REQUIRE(digest);
        report.signature = secrets.at(report.seat_index).Sign(
            std::span<const unsigned char>{digest->begin(), 32}).Compressed();
    }

    PreagreementNewViewProof NewView(const uint32_t view, const uint256& candidate,
                                    const std::optional<PreagreementPreparedCertificate>& prior = std::nullopt,
                                    const std::vector<uint32_t>& indices = {0, 1, 2, 3, 4, 5, 6}) const
    {
        PreagreementNewViewProof proof{view, candidate, {}};
        for (const uint32_t seat : indices) {
            PreagreementViewChange report{view, seat, prior, {}};
            SignReport(report);
            proof.reports.push_back(report);
        }
        return proof;
    }

    PreagreementCommitCertificate Commit(const uint32_t view, const uint256& candidate,
                                         const std::optional<PreagreementNewViewProof>& new_view = std::nullopt) const
    {
        PreagreementCommitCertificate proof{Prepared(view, candidate), {}, new_view};
        for (uint32_t seat{0}; seat < 7; ++seat) proof.commits.push_back(Vote(PreagreementPhase::COMMIT, view, candidate, seat));
        return proof;
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_preagreement_tests)

BOOST_AUTO_TEST_CASE(prepare_is_not_a_commit_certificate_and_phases_are_separate)
{
    const Fixture f;
    const auto candidate{Filled(0x90)};
    auto proof{f.Commit(0, candidate)};
    BOOST_CHECK(CheckPreagreementPrepared(f.context, f.seats, proof.prepared) == PreagreementCheck::OK);
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, proof) == PreagreementCheck::OK);

    proof.commits.clear();
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, proof) == PreagreementCheck::WRONG_QUORUM);
    proof.commits = proof.prepared.votes;
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, proof) == PreagreementCheck::BAD_SIGNATURE);

    const auto prepare{PreagreementVoteDigest(f.context, PreagreementPhase::PREPARE, 0, candidate, 0)};
    const auto commit{PreagreementVoteDigest(f.context, PreagreementPhase::COMMIT, 0, candidate, 0)};
    const auto v1{FlowMeshBlsCertificateDigest({f.context.domain, f.context.market_id, f.context.epoch,
        f.context.seat_set_hash, f.context.sequence, candidate})};
    BOOST_REQUIRE(prepare && commit);
    BOOST_CHECK(*prepare != *commit);
    BOOST_CHECK(*prepare != v1);
    BOOST_CHECK(*commit != v1);
    BOOST_CHECK(!bls::Verify(f.seats.members[0].key.Key(),
        std::span<const unsigned char>{v1.begin(), 32}, proof.prepared.votes[0].signature));
    BOOST_CHECK(!PreagreementVoteDigest(f.context, static_cast<PreagreementPhase>(3), 0, candidate, 0));
}

BOOST_AUTO_TEST_CASE(partition_six_of_nine_never_forms_a_quorum_and_sets_are_canonical)
{
    const Fixture f;
    const auto candidate{Filled(0x90)};
    auto prepared{f.Prepared(0, candidate)};
    prepared.votes.pop_back();
    BOOST_CHECK(CheckPreagreementPrepared(f.context, f.seats, prepared) == PreagreementCheck::WRONG_QUORUM);
    prepared.votes.push_back(f.Vote(PreagreementPhase::PREPARE, 0, candidate, 6));
    prepared.votes.push_back(f.Vote(PreagreementPhase::PREPARE, 0, candidate, 7));
    BOOST_CHECK(CheckPreagreementPrepared(f.context, f.seats, prepared) == PreagreementCheck::WRONG_QUORUM);
    prepared.votes.pop_back();
    prepared.votes[1] = prepared.votes[0];
    BOOST_CHECK(CheckPreagreementPrepared(f.context, f.seats, prepared) == PreagreementCheck::NON_CANONICAL_SIGNERS);
    prepared = f.Prepared(0, candidate);
    std::swap(prepared.votes[0], prepared.votes[1]);
    BOOST_CHECK(CheckPreagreementPrepared(f.context, f.seats, prepared) == PreagreementCheck::NON_CANONICAL_SIGNERS);
    prepared = f.Prepared(0, candidate);
    prepared.votes.back().seat_index = 9;
    BOOST_CHECK(CheckPreagreementPrepared(f.context, f.seats, prepared) == PreagreementCheck::NON_CANONICAL_SIGNERS);

    auto commit{f.Commit(0, candidate)};
    commit.commits.pop_back();
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, commit) == PreagreementCheck::WRONG_QUORUM);
    auto next{f.NewView(1, candidate)};
    next.reports.pop_back();
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, next) == PreagreementCheck::WRONG_QUORUM);
    next = f.NewView(1, candidate);
    next.reports[1] = next.reports[0];
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, next) == PreagreementCheck::NON_CANONICAL_SIGNERS);
    next = f.NewView(1, candidate);
    std::swap(next.reports[0], next.reports[1]);
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, next) == PreagreementCheck::NON_CANONICAL_SIGNERS);
    next = f.NewView(1, candidate);
    next.reports.back().seat_index = 9;
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, next) == PreagreementCheck::NON_CANONICAL_SIGNERS);
}

BOOST_AUTO_TEST_CASE(every_frozen_context_field_binds_the_signatures)
{
    const Fixture f;
    const auto candidate{Filled(0x90)};
    const auto proof{f.Commit(0, candidate)};
    const auto original{PreagreementVoteDigest(f.context, PreagreementPhase::PREPARE, 0, candidate, 0)};
    BOOST_REQUIRE(original);
    std::vector<PreagreementContext> contexts(8, f.context);
    contexts[0].domain = Filled(0xa0);
    contexts[1].market_id = Filled(0xa1);
    ++contexts[2].epoch;
    contexts[3].seat_set_hash = Filled(0xa3);
    ++contexts[4].sequence;
    contexts[5].parent_hash = Filled(0xa5);
    contexts[6].previous_state_root = Filled(0xa6);
    contexts[7].execution_config_id = Filled(0xa7);
    for (const auto& changed : contexts) {
        const auto digest{PreagreementVoteDigest(changed, PreagreementPhase::PREPARE, 0, candidate, 0)};
        BOOST_REQUIRE(digest);
        BOOST_CHECK(*digest != *original);
        BOOST_CHECK(CheckPreagreementCommit(changed, f.seats, proof) != PreagreementCheck::OK);
    }
    BOOST_CHECK(PreagreementVoteDigest(f.context, PreagreementPhase::PREPARE, 1, candidate, 0) != original);
    BOOST_CHECK(PreagreementVoteDigest(f.context, PreagreementPhase::PREPARE, 0, Filled(0x91), 0) != original);
    BOOST_CHECK(PreagreementVoteDigest(f.context, PreagreementPhase::PREPARE, 0, candidate, 1) != original);
    auto invalid{f.context};
    invalid.previous_state_root.SetNull();
    BOOST_CHECK(CheckPreagreementCommit(invalid, f.seats, proof) == PreagreementCheck::INVALID_CONTEXT);
    invalid = f.context;
    invalid.sequence = 0;
    BOOST_CHECK(CheckPreagreementCommit(invalid, f.seats, proof) == PreagreementCheck::INVALID_CONTEXT);
    invalid.parent_hash.SetNull();
    BOOST_CHECK(PreagreementVoteDigest(invalid, PreagreementPhase::PREPARE, 0, candidate, 0));
    invalid = f.context;
    invalid.parent_hash.SetNull();
    BOOST_CHECK(CheckPreagreementCommit(invalid, f.seats, proof) == PreagreementCheck::INVALID_CONTEXT);
    auto altered_set{f.seats};
    ++altered_set.anchor_height;
    BOOST_CHECK(CheckPreagreementCommit(f.context, altered_set, proof) == PreagreementCheck::INVALID_SEAT_SET);
}

BOOST_AUTO_TEST_CASE(later_view_requires_a_complete_matching_new_view_proof)
{
    const Fixture f;
    const auto candidate{Filled(0x90)};
    const auto next{f.NewView(1, candidate)};
    auto proof{f.Commit(1, candidate)};
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, proof) == PreagreementCheck::MISSING_NEW_VIEW);
    proof.new_view = next;
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, proof) == PreagreementCheck::OK);
    proof.new_view->candidate = Filled(0x91);
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, proof) == PreagreementCheck::WRONG_NEW_VIEW);
    proof.new_view = next;
    proof.new_view->view = 2;
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, proof) == PreagreementCheck::WRONG_NEW_VIEW);
    proof.new_view = next;
    proof.new_view->reports.pop_back();
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, proof) == PreagreementCheck::WRONG_QUORUM);
    proof = f.Commit(0, candidate, next);
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, proof) == PreagreementCheck::WRONG_NEW_VIEW);
    auto invalid{next};
    invalid.view = 0;
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, invalid) == PreagreementCheck::INVALID_VIEW);
    invalid = next;
    invalid.reports[0].view = 2;
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, invalid) == PreagreementCheck::INVALID_VIEW);
}

BOOST_AUTO_TEST_CASE(highest_prepared_certificate_is_required_even_when_only_one_report_carries_it)
{
    const Fixture f;
    const auto a{Filled(0x90)}, b{Filled(0x91)};
    const auto older{f.Prepared(0, a)};
    const auto newer{f.Prepared(1, b)};
    auto next{f.NewView(2, b, older)};
    next.reports.back().prepared = newer;
    f.SignReport(next.reports.back());
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, next) == PreagreementCheck::OK);
    next.candidate = a;
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, next) == PreagreementCheck::UNSAFE_NEW_VIEW_CANDIDATE);
    next.candidate = b;
    const auto commit{f.Commit(2, b, next)};
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, commit) == PreagreementCheck::OK);
    next.reports.back().prepared->view = next.view;
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, next) == PreagreementCheck::INVALID_VIEW);
}

BOOST_AUTO_TEST_CASE(hidden_prepare_qc_is_not_confused_with_a_committed_value)
{
    const Fixture f;
    const auto a{Filled(0x90)}, b{Filled(0x91)};
    const auto hidden{f.Prepared(0, a)};
    BOOST_CHECK(CheckPreagreementPrepared(f.context, f.seats, hidden) == PreagreementCheck::OK);
    PreagreementCommitCertificate incomplete{hidden, {}, std::nullopt};
    incomplete.commits.push_back(f.Vote(PreagreementPhase::COMMIT, 0, a, 0));
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, incomplete) == PreagreementCheck::WRONG_QUORUM);

    // Seat 0 alone received the full prepare QC. The other prepare voters do
    // not become permanently locked merely by casting PREPARE. A new-view
    // quorum excluding seat 0 may honestly report no full prepared proof.
    auto next{f.NewView(1, b, std::nullopt, {1, 2, 3, 4, 5, 6, 7})};
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, next) == PreagreementCheck::OK);
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, f.Commit(1, b, next)) == PreagreementCheck::OK);

    // If even one of the complete reports does carry the older QC, B cannot
    // be selected. Verifying proofs cannot detect evidence a sender conceals.
    next.reports.back().prepared = hidden;
    f.SignReport(next.reports.back());
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, next) == PreagreementCheck::UNSAFE_NEW_VIEW_CANDIDATE);
    next.candidate = a;
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, next) == PreagreementCheck::OK);
}

BOOST_AUTO_TEST_CASE(formed_commit_quorum_is_preserved_by_honest_complete_view_change_reports)
{
    const Fixture f;
    const auto a{Filled(0x90)}, b{Filled(0x91)};
    const auto committed{f.Commit(0, a)};
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, committed) == PreagreementCheck::OK);

    // First quorum is 0..6. New-view quorum 2..8 intersects in five seats;
    // allowing at most two Byzantine seats still leaves honest reporters.
    // This fixture models their REQUIRED durable knowledge, not a runtime
    // implementation or proof that the current signer persists it correctly.
    auto next{f.NewView(1, b, std::nullopt, {2, 3, 4, 5, 6, 7, 8})};
    for (auto& report : next.reports) {
        if (report.seat_index >= 4 && report.seat_index <= 6) {
            report.prepared = committed.prepared;
            f.SignReport(report);
        }
    }
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, next) == PreagreementCheck::UNSAFE_NEW_VIEW_CANDIDATE);
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, f.Commit(1, b, next)) == PreagreementCheck::UNSAFE_NEW_VIEW_CANDIDATE);
    next.candidate = a;
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, f.Commit(1, a, next)) == PreagreementCheck::OK);
}

BOOST_AUTO_TEST_CASE(conflicting_prepared_qcs_are_rejected_at_any_report_position)
{
    const Fixture f;
    const auto a{Filled(0x90)}, b{Filled(0x91)}, c{Filled(0x92)};
    const auto older_a{f.Prepared(0, a)}, older_b{f.Prepared(0, b)}, newer{f.Prepared(1, c)};
    // Intentionally generate more Byzantine equivocation than the protocol's
    // tolerated fault model. The verifier refuses contradictory supplied QCs
    // even if a later/highest certificate occurs first in canonical seat order.
    for (const bool highest_first : {false, true}) {
        auto next{f.NewView(2, c)};
        next.reports[0].prepared = highest_first ? newer : older_a;
        next.reports[1].prepared = highest_first ? older_a : older_b;
        next.reports[2].prepared = highest_first ? older_b : newer;
        for (size_t i{0}; i < 3; ++i) f.SignReport(next.reports[i]);
        BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, next) == PreagreementCheck::CONFLICTING_PREPARED);
    }
}

BOOST_AUTO_TEST_CASE(view_change_authenticates_the_entire_carried_proof_and_sender)
{
    const Fixture f;
    const auto a{Filled(0x90)}, b{Filled(0x91)};
    const auto prepared{f.Prepared(0, a)};
    const auto next{f.NewView(1, a, prepared)};
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, next) == PreagreementCheck::OK);
    auto changed{next};
    changed.reports[0].prepared = f.Prepared(0, a, {0, 1, 2, 3, 4, 5, 7});
    // Both prepared QCs are valid for A; changing their exact records without
    // signing the enclosing report is still rejected.
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, changed) == PreagreementCheck::BAD_SIGNATURE);
    changed = next;
    changed.reports[0].prepared.reset();
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, changed) == PreagreementCheck::BAD_SIGNATURE);
    changed = next;
    changed.reports[0].signature.fill(0);
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, changed) == PreagreementCheck::BAD_SIGNATURE);
    changed = next;
    changed.reports[0].signature = changed.reports[1].signature;
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, changed) == PreagreementCheck::BAD_SIGNATURE);
    changed = next;
    changed.reports[0].prepared->votes[0] = f.Vote(PreagreementPhase::PREPARE, 0, b, 0);
    f.SignReport(changed.reports[0]);
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, changed) == PreagreementCheck::BAD_SIGNATURE);
    changed = next;
    changed.reports[0].prepared->votes.pop_back();
    f.SignReport(changed.reports[0]);
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, changed) == PreagreementCheck::WRONG_QUORUM);
}

BOOST_AUTO_TEST_CASE(exact_slot_and_cross_phase_replays_are_rejected)
{
    const Fixture f;
    const auto candidate{Filled(0x90)};
    auto proof{f.Commit(0, candidate)};
    proof.prepared.votes[0] = f.Vote(PreagreementPhase::COMMIT, 0, candidate, 0);
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, proof) == PreagreementCheck::BAD_SIGNATURE);
    proof = f.Commit(0, candidate);
    proof.prepared.view = 1;
    proof.new_view = f.NewView(1, candidate);
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, proof) == PreagreementCheck::BAD_SIGNATURE);
    proof = f.Commit(0, candidate);
    proof.commits[0] = f.Vote(PreagreementPhase::COMMIT, 1, candidate, 0);
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, proof) == PreagreementCheck::BAD_SIGNATURE);
    proof = f.Commit(0, candidate);
    proof.commits[0] = f.Vote(PreagreementPhase::COMMIT, 0, Filled(0x91), 0);
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, proof) == PreagreementCheck::BAD_SIGNATURE);
    auto later_slot{f.context};
    ++later_slot.sequence;
    BOOST_CHECK(CheckPreagreementCommit(later_slot, f.seats, f.Commit(0, candidate)) == PreagreementCheck::BAD_SIGNATURE);
    auto next{f.NewView(1, candidate)};
    next.reports[0].signature = f.Vote(PreagreementPhase::COMMIT, 1, candidate, 0).signature.Compressed();
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, next) == PreagreementCheck::BAD_SIGNATURE);
}

BOOST_AUTO_TEST_CASE(proof_resource_budget_counts_nested_signature_records_before_context_work)
{
    const Fixture f;
    const auto candidate{Filled(0x90)};
    const auto vote{f.Vote(PreagreementPhase::PREPARE, 0, candidate, 0)};
    auto prepared{f.Prepared(0, candidate)};
    prepared.votes.assign(PREAGREEMENT_MAX_PROOF_SIGNATURES + 1, vote);
    const PreagreementContext invalid_context;
    BOOST_CHECK(CheckPreagreementPrepared(invalid_context, f.seats, prepared) == PreagreementCheck::PROOF_TOO_LARGE);
    auto next{f.NewView(1, candidate)};
    next.reports[0].prepared = prepared;
    BOOST_CHECK(CheckPreagreementNewView(invalid_context, f.seats, next) == PreagreementCheck::PROOF_TOO_LARGE);
    BOOST_CHECK(!PreagreementViewChangeDigest(f.context, next.reports[0]));

    prepared.votes.assign(PREAGREEMENT_MAX_PROOF_SIGNATURES / 2, vote);
    next.reports[0].prepared = prepared;
    next.reports[1].prepared = prepared;
    BOOST_CHECK(CheckPreagreementNewView(invalid_context, f.seats, next) == PreagreementCheck::PROOF_TOO_LARGE);
    auto commit{f.Commit(1, candidate, next)};
    BOOST_CHECK(CheckPreagreementCommit(invalid_context, f.seats, commit) == PreagreementCheck::PROOF_TOO_LARGE);
    next = f.NewView(1, candidate);
    next.reports.assign(PREAGREEMENT_MAX_PROOF_SIGNATURES + 1, next.reports[0]);
    BOOST_CHECK(CheckPreagreementNewView(invalid_context, f.seats, next) == PreagreementCheck::PROOF_TOO_LARGE);
}

BOOST_AUTO_TEST_SUITE_END()
