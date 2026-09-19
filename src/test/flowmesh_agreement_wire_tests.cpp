// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/agreement_wire.h>
#include <flowmesh/p2p.h>
#include <flowmesh/production_engine.h>
#include <crypto/common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <vector>

namespace {
using namespace flowmesh;

uint256 Filled(unsigned char value)
{
    uint256 out;
    std::fill(out.begin(), out.end(), value);
    return out;
}

struct Fixture {
    PreagreementContext context;
    ActiveFnBlsSeatSet seats;
    std::vector<bls::SecretKey> keys;
    ProductionEntryCore entry;

    Fixture()
    {
        context = {Filled(1), Filled(2), 3, {}, 1, Filled(4), Filled(5), Filled(6)};
        struct Row { bls::SecretKey key; BlsSeatBinding binding; SeatId id; };
        std::vector<Row> rows;
        for (uint32_t i{0}; i < 9; ++i) {
            std::array<unsigned char, 32> seed{};
            seed.fill(i + 11);
            const auto key{bls::SecretKey::FromIKM(seed)};
            BOOST_REQUIRE(key);
            BlsSeatBinding binding;
            binding.outpoint = {Txid::FromUint256(Filled(i + 30)), i};
            binding.public_key = key->GetPublicKey().Compressed();
            binding.proof_of_possession = key->SignPoP().Compressed();
            rows.push_back({*key, binding, ComputeFlowMeshSeatId(context.domain, binding.outpoint)});
        }
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return a.id < b.id || (a.id == b.id && a.binding.outpoint < b.binding.outpoint);
        });
        std::vector<BlsSeatBinding> bindings;
        for (const auto& row : rows) { keys.push_back(row.key); bindings.push_back(row.binding); }
        BlsSeatSetCheck check;
        const auto built{BuildActiveFnBlsSeatSet(context.domain, context.market_id, context.epoch,
            100, Filled(7), bindings, check)};
        BOOST_REQUIRE(built);
        seats = *built;
        context.seat_set_hash = seats.set_hash;
        entry.domain = context.domain;
        entry.market_id = context.market_id;
        entry.epoch = context.epoch;
        entry.seat_set_hash = context.seat_set_hash;
        entry.sequence = context.sequence;
        entry.parent_hash = context.parent_hash;
        entry.previous_state_root = context.previous_state_root;
        entry.anchor = {200, Filled(8)};
        entry.actions_root = ComputeProductionActionsRoot(entry.actions);
        entry.result_root = Filled(9);
        entry.state_root = Filled(10);
        entry.effect_root = modern::EmptyFlowMeshEffectRoot(0);
        BOOST_REQUIRE(EncodeProductionEntry(entry));
    }

    IndexedBlsSignature Vote(PreagreementPhase phase, uint32_t view, uint32_t seat) const
    {
        const auto digest{PreagreementVoteDigest(context, phase, view, entry.GetHash(), seat)};
        BOOST_REQUIRE(digest);
        return {seat, keys.at(seat).Sign({digest->begin(), 32})};
    }
    PreagreementPreparedCertificate Prepared(uint32_t view) const
    {
        PreagreementPreparedCertificate out{view, entry.GetHash(), {}};
        for (uint32_t i{0}; i < 7; ++i) out.votes.push_back(Vote(PreagreementPhase::PREPARE, view, i));
        return out;
    }
    PreagreementNewViewProof NewView(uint32_t view) const
    {
        PreagreementNewViewProof out{view, entry.GetHash(), {}};
        for (uint32_t i{0}; i < 7; ++i) {
            PreagreementViewChange report{view, i, Prepared(view - 1), {}};
            const auto digest{PreagreementViewChangeDigest(context, report)};
            BOOST_REQUIRE(digest);
            report.signature = keys[i].Sign({digest->begin(), 32}).Compressed();
            out.reports.push_back(std::move(report));
        }
        return out;
    }
    PreagreementCommitCertificate Decision(uint32_t view) const
    {
        PreagreementCommitCertificate out{Prepared(view), {}, std::nullopt};
        for (uint32_t i{0}; i < 7; ++i) out.commits.push_back(Vote(PreagreementPhase::COMMIT, view, i));
        if (view) out.new_view = NewView(view);
        return out;
    }
    AgreementMessage Message(AgreementStage stage) const
    {
        AgreementMessage m;
        m.stage = stage;
        m.context = context;
        m.view = 1;
        m.candidate = entry.GetHash();
        m.seat_index = ProductionProposerSeatIndex(context.sequence, m.view, seats.Size());
        if (stage == AgreementStage::PROPOSAL || stage == AgreementStage::DECISION) m.entry_bytes = *EncodeProductionEntry(entry);
        if (stage == AgreementStage::PROPOSAL) m.new_view = NewView(m.view);
        if (stage == AgreementStage::COMMIT) { m.prepared = Prepared(m.view); m.new_view = NewView(m.view); }
        if (stage == AgreementStage::VIEW_CHANGE) m.prepared = Prepared(m.view - 1);
        if (stage == AgreementStage::DECISION) {
            m.decision = Decision(m.view);
            m.seat_index = AGREEMENT_NO_SEAT;
        } else {
            const auto digest{AgreementMessageDigest(m)};
            BOOST_REQUIRE(digest);
            m.signature = keys.at(m.seat_index).Sign({digest->begin(), 32}).Compressed();
        }
        return m;
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_agreement_wire_tests)

BOOST_AUTO_TEST_CASE(proof_codecs_round_trip_canonical_bytes_and_preserve_verification)
{
    const Fixture f;
    const auto prepared{EncodePreagreementPrepared(f.Prepared(0))};
    const auto next{EncodePreagreementNewView(f.NewView(1))};
    const auto commit{EncodePreagreementCommit(f.Decision(1))};
    const auto report{EncodePreagreementViewChange(f.NewView(1).reports.front())};
    BOOST_REQUIRE(prepared && next && commit && report);
    const auto p{DecodePreagreementPrepared(*prepared)};
    const auto n{DecodePreagreementNewView(*next)};
    const auto c{DecodePreagreementCommit(*commit)};
    const auto r{DecodePreagreementViewChange(*report)};
    BOOST_REQUIRE(p && n && c && r);
    BOOST_CHECK(*EncodePreagreementPrepared(*p) == *prepared);
    BOOST_CHECK(*EncodePreagreementNewView(*n) == *next);
    BOOST_CHECK(*EncodePreagreementCommit(*c) == *commit);
    BOOST_CHECK(*EncodePreagreementViewChange(*r) == *report);
    BOOST_CHECK(CheckPreagreementPrepared(f.context, f.seats, *p) == PreagreementCheck::OK);
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, *n) == PreagreementCheck::OK);
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, *c) == PreagreementCheck::OK);
}

BOOST_AUTO_TEST_CASE(all_stages_round_trip_and_unsigned_intent_is_not_authority)
{
    const Fixture f;
    for (const auto stage : {AgreementStage::PROPOSAL, AgreementStage::PREPARE, AgreementStage::COMMIT,
                             AgreementStage::VIEW_CHANGE, AgreementStage::DECISION}) {
        const auto message{f.Message(stage)};
        const auto encoded{EncodeAgreementMessage(message)};
        BOOST_REQUIRE(encoded);
        const auto decoded{DecodeAgreementMessage(*encoded)};
        BOOST_REQUIRE(decoded);
        BOOST_CHECK(*EncodeAgreementMessage(*decoded) == *encoded);
        if (stage == AgreementStage::DECISION) {
            BOOST_CHECK(!AgreementMessageDigest(*decoded));
            BOOST_CHECK(!CheckAgreementMessageSignature(*decoded, f.seats));
            BOOST_REQUIRE(decoded->decision);
            BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, *decoded->decision) == PreagreementCheck::OK);
        } else {
            BOOST_CHECK(CheckAgreementMessageSignature(*decoded, f.seats));
            auto intent{message};
            intent.signature.fill(0);
            const auto unsigned_bytes{EncodeAgreementMessage(intent)};
            BOOST_REQUIRE(unsigned_bytes);
            BOOST_CHECK(DecodeAgreementMessage(*unsigned_bytes).has_value());
            BOOST_CHECK(!CheckAgreementMessageSignature(intent, f.seats));
        }
        auto trailing{*encoded}; trailing.push_back(0);
        BOOST_CHECK(!DecodeAgreementMessage(trailing));
        for (const size_t length : {size_t{0}, size_t{2}, size_t{100}, encoded->size() - 1}) {
            BOOST_CHECK(!DecodeAgreementMessage(std::span<const unsigned char>{*encoded}.first(length)));
        }
    }
}

BOOST_AUTO_TEST_CASE(proposal_signature_binds_exact_entry_new_view_and_all_context)
{
    const Fixture f;
    const auto original{f.Message(AgreementStage::PROPOSAL)};
    BOOST_REQUIRE(CheckAgreementMessageSignature(original, f.seats));
    auto changed{original};
    ++changed.context.execution_config_id.begin()[0];
    BOOST_CHECK(!CheckAgreementMessageSignature(changed, f.seats));
    changed = original;
    changed.new_view->reports.front().prepared.reset();
    BOOST_REQUIRE(EncodeAgreementMessage(changed));
    BOOST_CHECK(!CheckAgreementMessageSignature(changed, f.seats));
    changed = original;
    auto entry{f.entry}; ++entry.anchor.height;
    changed.entry_bytes = *EncodeProductionEntry(entry);
    changed.candidate = entry.GetHash();
    changed.new_view->candidate = changed.candidate;
    BOOST_REQUIRE(EncodeAgreementMessage(changed));
    BOOST_CHECK(!CheckAgreementMessageSignature(changed, f.seats));
    changed = original;
    changed.context.parent_hash = Filled(90);
    BOOST_CHECK(!EncodeAgreementMessage(changed));
    changed = original;
    changed.seat_index = (changed.seat_index + 1) % f.seats.Size();
    const auto digest{AgreementMessageDigest(changed)};
    BOOST_REQUIRE(digest);
    changed.signature = f.keys[changed.seat_index].Sign({digest->begin(), 32}).Compressed();
    BOOST_CHECK(!CheckAgreementMessageSignature(changed, f.seats)); // Valid key, wrong scheduled proposer.
}

BOOST_AUTO_TEST_CASE(stage_fields_and_domain_separation_are_strict)
{
    const Fixture f;
    auto prepare{f.Message(AgreementStage::PREPARE)};
    BOOST_REQUIRE(CheckAgreementMessageSignature(prepare, f.seats));
    auto commit{prepare}; commit.stage = AgreementStage::COMMIT;
    BOOST_CHECK(!CheckAgreementMessageSignature(commit, f.seats));
    const auto v1{FlowMeshBlsCertificateDigest({f.context.domain, f.context.market_id, f.context.epoch,
        f.context.seat_set_hash, f.context.sequence, prepare.candidate})};
    const auto signature{bls::Signature::Decode(prepare.signature)};
    BOOST_REQUIRE(signature);
    BOOST_CHECK(!bls::Verify(f.seats.members[prepare.seat_index].key.Key(), {v1.begin(), 32}, *signature));
    prepare.entry_bytes = *EncodeProductionEntry(f.entry);
    BOOST_CHECK(!EncodeAgreementMessage(prepare));
    auto proposal{f.Message(AgreementStage::PROPOSAL)};
    proposal.new_view.reset();
    BOOST_CHECK(!EncodeAgreementMessage(proposal));
    auto decision{f.Message(AgreementStage::DECISION)};
    decision.signature[0] = 1;
    BOOST_CHECK(!EncodeAgreementMessage(decision));
    auto view_change{f.Message(AgreementStage::VIEW_CHANGE)};
    view_change.prepared.reset(); view_change.candidate.SetNull();
    BOOST_REQUIRE(EncodeAgreementMessage(view_change));
    view_change.candidate = Filled(99);
    BOOST_CHECK(!EncodeAgreementMessage(view_change));
}

BOOST_AUTO_TEST_CASE(malformed_lengths_counts_signers_and_nested_budget_are_refused)
{
    const Fixture f;
    auto encoded{*EncodeAgreementMessage(f.Message(AgreementStage::PREPARE))};
    static_assert(AGREEMENT_MAX_BYTES == FLOWMESH_AGREEMENT_MAX_BYTES);
    BOOST_CHECK_EQUAL(encoded.size(), FLOWMESH_AGREEMENT_MIN_BYTES);
    encoded[1] = 2; BOOST_CHECK(!DecodeAgreementMessage(encoded));
    encoded[1] = 1; encoded[2] = 0; BOOST_CHECK(!DecodeAgreementMessage(encoded));
    encoded = *EncodeAgreementMessage(f.Message(AgreementStage::COMMIT));
    encoded[FLOWMESH_AGREEMENT_MIN_BYTES] = 2; // Noncanonical optional flag.
    BOOST_CHECK(!DecodeAgreementMessage(encoded));
    encoded = *EncodeAgreementMessage(f.Message(AgreementStage::PROPOSAL));
    WriteBE32(encoded.data() + FLOWMESH_AGREEMENT_MIN_BYTES, 0xffffffffU);
    BOOST_CHECK(!DecodeAgreementMessage(encoded));
    BOOST_CHECK(!DecodeAgreementMessage(std::vector<unsigned char>(AGREEMENT_MAX_BYTES + 1)));
    BOOST_CHECK(!DecodePreagreementCommit(std::vector<unsigned char>(AGREEMENT_MAX_PROOF_BYTES + 1)));
    auto prepared{f.Prepared(0)};
    prepared.votes[1] = prepared.votes[0];
    BOOST_CHECK(!EncodePreagreementPrepared(prepared));
    auto malformed_vote{f.Message(AgreementStage::COMMIT)};
    malformed_vote.prepared->votes[1] = malformed_vote.prepared->votes[0];
    BOOST_CHECK(!AgreementMessageDigest(malformed_vote));
    encoded = *EncodePreagreementPrepared(f.Prepared(0));
    WriteBE32(encoded.data() + 2 + 4 + 32, 0xffffffffU);
    BOOST_CHECK(!DecodePreagreementPrepared(encoded));
    prepared = f.Prepared(0);
    prepared.votes.resize(PREAGREEMENT_MAX_PROOF_SIGNATURES + 1, prepared.votes.front());
    BOOST_CHECK(!EncodePreagreementPrepared(prepared));
    auto next{f.NewView(1)};
    next.reports[1].seat_index = next.reports[0].seat_index;
    BOOST_CHECK(!EncodePreagreementNewView(next));
    next = f.NewView(1);
    // Individually bounded subproofs must not evade the complete-object cap.
    for (auto& report : next.reports) {
        report.prepared->votes.assign(600, report.prepared->votes.front());
        for (uint32_t i{0}; i < 600; ++i) report.prepared->votes[i].seat_index = i;
    }
    BOOST_CHECK(!EncodePreagreementNewView(next));
}

BOOST_AUTO_TEST_SUITE_END()
