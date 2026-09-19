// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_agreement.h>
#include <node/flowmesh_production_store.h>
#include <flowmesh/production_engine.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <stdexcept>

namespace {
using namespace flowmesh;
using Bytes = std::vector<unsigned char>;

uint256 Filled(const unsigned char byte)
{
    uint256 result; std::fill(result.begin(), result.end(), byte); return result;
}

struct AgreementFixture {
    PreagreementContext context{Filled(1), Filled(2), 17, {}, 0, {}, Filled(3), Filled(4)};
    ActiveFnBlsSeatSet seats;
    std::vector<bls::SecretKey> secrets;
    Bytes a;
    Bytes b;
    uint256 hash_a;
    uint256 hash_b;

    AgreementFixture()
    {
        struct Item { bls::SecretKey key; BlsSeatBinding binding; SeatId id; };
        std::vector<Item> items;
        for (uint32_t i{0}; i < 9; ++i) {
            std::array<unsigned char, 32> ikm{}; ikm.fill(i + 1);
            const auto key{bls::SecretKey::FromIKM(ikm)}; BOOST_REQUIRE(key);
            BlsSeatBinding binding;
            binding.outpoint = {Txid::FromUint256(Filled(i + 80)), i};
            binding.public_key = key->GetPublicKey().Compressed();
            binding.proof_of_possession = key->SignPoP().Compressed();
            items.push_back({*key, binding, ComputeFlowMeshSeatId(context.domain, binding.outpoint)});
        }
        std::sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.id < y.id; });
        std::vector<BlsSeatBinding> bindings;
        for (const auto& item : items) { secrets.push_back(item.key); bindings.push_back(item.binding); }
        BlsSeatSetCheck check;
        const auto built{BuildActiveFnBlsSeatSet(context.domain, context.market_id, context.epoch,
            100, Filled(5), bindings, check)};
        BOOST_REQUIRE(built); seats = *built; context.seat_set_hash = seats.set_hash;
        ProductionEntryCore entry;
        entry.domain = context.domain; entry.market_id = context.market_id;
        entry.epoch = context.epoch; entry.seat_set_hash = context.seat_set_hash;
        entry.previous_state_root = context.previous_state_root; entry.anchor = {200, Filled(6)};
        entry.actions_root = ComputeProductionActionsRoot(entry.actions);
        entry.result_root = Filled(7); entry.state_root = Filled(8);
        entry.effect_root = modern::EmptyFlowMeshEffectRoot(0);
        a = *EncodeProductionEntry(entry); hash_a = entry.GetHash();
        entry.anchor = {201, Filled(9)};
        b = *EncodeProductionEntry(entry); hash_b = entry.GetHash();
        BOOST_REQUIRE(hash_a != hash_b);
    }

    AgreementMessage Sign(AgreementMessage message) const
    {
        const auto digest{AgreementMessageDigest(message)}; BOOST_REQUIRE(digest);
        message.signature = secrets.at(message.seat_index).Sign(
            std::span<const unsigned char>{digest->begin(), 32}).Compressed();
        return message;
    }
    AgreementMessage Proposal(const Bytes& entry, uint32_t view = 0,
                              std::optional<PreagreementNewViewProof> proof = std::nullopt) const
    {
        AgreementMessage result;
        result.stage = AgreementStage::PROPOSAL; result.context = context;
        result.view = view; result.candidate = DecodeProductionEntry(entry)->GetHash();
        result.seat_index = ProductionProposerSeatIndex(context.sequence, view, seats.Size());
        result.entry_bytes = entry; result.new_view = std::move(proof);
        return Sign(result);
    }
    AgreementMessage Vote(AgreementStage stage, uint32_t seat, const uint256& hash, uint32_t view = 0,
                          std::optional<PreagreementPreparedCertificate> prepared = std::nullopt) const
    {
        AgreementMessage result;
        result.stage = stage; result.context = context; result.view = view;
        result.candidate = hash; result.seat_index = seat; result.prepared = std::move(prepared);
        return Sign(result);
    }
    PreagreementPreparedCertificate Prepared(const uint256& hash, uint32_t view = 0) const
    {
        PreagreementPreparedCertificate result{view, hash, {}};
        for (uint32_t seat{0}; seat < 7; ++seat) {
            const auto vote{Vote(AgreementStage::PREPARE, seat, hash, view)};
            result.votes.push_back({seat, *bls::Signature::Decode(vote.signature)});
        }
        return result;
    }
    AgreementMessage Report(uint32_t seat, uint32_t view,
                            std::optional<PreagreementPreparedCertificate> prepared = std::nullopt) const
    {
        AgreementMessage result;
        result.stage = AgreementStage::VIEW_CHANGE; result.context = context;
        result.view = view; result.seat_index = seat; result.prepared = std::move(prepared);
        if (result.prepared) result.candidate = result.prepared->candidate;
        return Sign(result);
    }
    AgreementMessage Decision(const Bytes& entry) const
    {
        AgreementMessage result;
        result.stage = AgreementStage::DECISION; result.context = context;
        result.candidate = DecodeProductionEntry(entry)->GetHash(); result.seat_index = AGREEMENT_NO_SEAT;
        result.entry_bytes = entry;
        PreagreementCommitCertificate certificate{Prepared(result.candidate), {}, std::nullopt};
        for (uint32_t seat{0}; seat < 7; ++seat) {
            const auto commit{Vote(AgreementStage::COMMIT, seat, result.candidate)};
            certificate.commits.push_back({seat, *bls::Signature::Decode(commit.signature)});
        }
        result.decision = std::move(certificate);
        return result;
    }
    node::FlowMeshAgreementCallbacks Callbacks(uint32_t seat, std::vector<AgreementMessage>& output,
                                               bool& available) const
    {
        node::FlowMeshAgreementCallbacks result;
        result.local_keys = [this, seat] { return std::vector<bls::SecretKey>{secrets.at(seat)}; };
        result.validate_candidate = [this, &available](std::span<const unsigned char> bytes,
            std::optional<std::span<const unsigned char>> evidence) -> std::optional<Bytes> {
            if (!available) return std::nullopt;
            const auto entry{DecodeProductionEntry(bytes)};
            if (!entry || (entry->GetHash() != hash_a && entry->GetHash() != hash_b)) return std::nullopt;
            // Public evidence fixture; validates restoration and exact durability.
            const Bytes expected{0xe1, entry->GetHash().begin()[0], 0x5a};
            if (evidence && Bytes{evidence->begin(), evidence->end()} != expected) return std::nullopt;
            return expected;
        };
        result.publish = [&output](const AgreementMessage& message) { output.push_back(message); return true; };
        return result;
    }
};

struct Network {
    const AgreementFixture& fixture;
    std::array<bool, 9> available{true, true, true, true, true, true, true, true, true};
    std::array<std::vector<AgreementMessage>, 9> outputs;
    std::array<std::unique_ptr<node::FlowMeshAgreement>, 9> nodes;
    std::array<size_t, 9> delivered{};

    explicit Network(const AgreementFixture& f) : fixture(f)
    {
        std::string error;
        for (size_t i{0}; i < nodes.size(); ++i) {
            nodes[i] = std::make_unique<node::FlowMeshAgreement>(DBParams{
                .path = fs::PathFromString("agreement-test-" + std::to_string(i)), .cache_bytes = 1 << 20, .memory_only = true},
                fixture.Callbacks(i, outputs[i], available[i]));
            BOOST_REQUIRE_MESSAGE(nodes[i]->Open(f.context, f.seats, Filled(100 + i), true, error), error);
        }
    }
    void Drain(bool partitioned = false, size_t online = 9)
    {
        std::string error;
        for (size_t passes{0}; passes < 100; ++passes) {
            bool work{false};
            for (size_t sender{0}; sender < online; ++sender) {
                while (delivered[sender] < outputs[sender].size()) {
                    const AgreementMessage message{outputs[sender][delivered[sender]++]}; work = true;
                    for (size_t receiver{0}; receiver < online; ++receiver) {
                        if (partitioned && (sender < 6) != (receiver < 6)) continue;
                        nodes[receiver]->Receive(message, error);
                        BOOST_REQUIRE_MESSAGE(!nodes[receiver]->Halted(), nodes[receiver]->LastError());
                    }
                }
            }
            if (!work) return;
        }
        BOOST_FAIL("agreement network did not quiesce within its work bound");
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_agreement_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(six_three_conflicting_prepares_recover_with_full_view_change)
{
    const AgreementFixture f;
    Network network{f};
    std::string error;
    // A faulty view-zero leader equivocates. The honest engines each persist
    // just one PREPARE, and neither partition reaches the unchanged quorum 7.
    const auto proposal_a{f.Proposal(f.a)};
    const auto proposal_b{f.Proposal(f.b)};
    for (size_t i{0}; i < 9; ++i) {
        BOOST_REQUIRE(network.nodes[i]->Receive(i < 6 ? proposal_a : proposal_b, error));
    }
    network.Drain(true);
    for (const auto& node : network.nodes) BOOST_CHECK(!node->DecidedCandidate());
    for (const auto& node : network.nodes) BOOST_REQUIRE_MESSAGE(node->Timeout(error), error);
    network.Drain();
    for (const auto& node : network.nodes) {
        BOOST_REQUIRE(node->DecidedCandidate());
        BOOST_CHECK(*node->DecidedCandidate() == f.hash_a);
        BOOST_CHECK_EQUAL(node->View(), 1U);
    }
    for (const auto& output : network.outputs) {
        for (const auto& message : output) {
            if (message.stage == AgreementStage::DECISION) {
                BOOST_REQUIRE(message.decision && message.decision->new_view);
                BOOST_CHECK_EQUAL(message.decision->new_view->reports.size(), 7U);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(two_missing_seats_progress_three_missing_seats_stall)
{
    const AgreementFixture f;
    std::string error;
    for (size_t online : {6U, 7U}) {
        Network network{f};
        BOOST_REQUIRE(network.nodes[0]->SubmitCandidate(f.a, error));
        network.Drain(false, online);
        for (size_t i{0}; i < online; ++i) {
            BOOST_CHECK_EQUAL(network.nodes[i]->DecidedCandidate().has_value(), online == 7);
        }
    }
}

BOOST_AUTO_TEST_CASE(hidden_prepare_weak_final_gate_strands_but_commit_gate_recovers)
{
    const AgreementFixture f;
    Network network{f};
    std::string error;
    std::array<std::unique_ptr<node::FlowMeshProductionStore>, 9> weak_stores;
    const auto weak_final = [&](uint32_t seat, const Bytes& bytes) {
        if (!weak_stores[seat]) {
            weak_stores[seat] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
                .path = m_args.GetDataDirBase() / fs::PathFromString("weak-final-control-" + std::to_string(seat)),
                .cache_bytes = 1 << 20});
            BOOST_REQUIRE(weak_stores[seat]->OpenForMarket(f.context.domain, f.context.market_id,
                f.seats, f.context.previous_state_root, error));
        }
        BOOST_REQUIRE(weak_stores[seat]->LockCandidate(*DecodeProductionEntry(bytes), {}) == ProductionLockResult::LOCKED);
        ProductionSigningGuard guard{*weak_stores[seat]};
        ProductionLockResult lock;
        const auto vote{SignProductionEntryAttestation(f.secrets[seat], seat,
            *DecodeProductionEntry(bytes), f.seats, guard, lock)};
        BOOST_REQUIRE(vote);
    };
    // Byzantine seat0 selectively releases A's prepared proof only to1,2.
    // Everyone else signed PREPARE, but did not learn its quorum.
    for (uint32_t seat{1}; seat < 9; ++seat) BOOST_REQUIRE(network.nodes[seat]->Receive(f.Proposal(f.a), error));
    for (uint32_t receiver : {1U, 2U}) {
        for (uint32_t sender{0}; sender < 7; ++sender) {
            BOOST_REQUIRE(network.nodes[receiver]->Receive(f.Vote(AgreementStage::PREPARE, sender, f.hash_a), error));
        }
        BOOST_REQUIRE(std::any_of(network.outputs[receiver].begin(), network.outputs[receiver].end(),
            [](const auto& message) { return message.stage == AgreementStage::COMMIT; }));
        BOOST_CHECK(!network.nodes[receiver]->DecidedCandidate());
        // Deliberately broken integration, confined to these test-only stores:
        // final-sign on own COMMIT/prepared proof instead of Q COMMIT.
        weak_final(receiver, f.a);
    }
    // A later scheduled Byzantine leader has seven nil reports excluding1,2.
    // Choosing B is justified: no A commit quorum has been formed.
    constexpr uint32_t later_view{9};
    PreagreementNewViewProof proof{later_view, f.hash_b, {}};
    for (uint32_t seat : {0U, 3U, 4U, 5U, 6U, 7U, 8U}) {
        const auto report{f.Report(seat, later_view)};
        proof.reports.push_back({later_view, seat, std::nullopt, report.signature});
    }
    BOOST_REQUIRE(CheckPreagreementNewView(f.context, f.seats, proof) == PreagreementCheck::OK);
    const auto proposal_b{f.Proposal(f.b, later_view, proof)};
    for (uint32_t receiver{3}; receiver < 9; ++receiver) {
        BOOST_REQUIRE(network.nodes[receiver]->Receive(proposal_b, error));
        BOOST_REQUIRE(network.nodes[receiver]->Receive(f.Vote(AgreementStage::PREPARE, 0, f.hash_b, later_view), error));
    }
    const auto deliver = [&](uint32_t first) {
        std::array<size_t, 9> cursors{};
        for (size_t pass{0}; pass < 32; ++pass) {
            bool work{false};
            for (uint32_t sender{first}; sender < 9; ++sender) {
                while (cursors[sender] < network.outputs[sender].size()) {
                    const auto message{network.outputs[sender][cursors[sender]++]};
                    if (message.view != later_view) continue;
                    work = true;
                    for (uint32_t receiver{first}; receiver < 9; ++receiver) {
                        BOOST_REQUIRE_MESSAGE(network.nodes[receiver]->Receive(message, error), error);
                    }
                }
            }
            if (!work) return;
        }
        BOOST_FAIL("selective-delivery control exceeded its bounded work");
    };
    deliver(3);
    for (uint32_t seat{3}; seat < 9; ++seat) {
        BOOST_REQUIRE(std::any_of(network.outputs[seat].begin(), network.outputs[seat].end(),
            [](const auto& message) { return message.stage == AgreementStage::COMMIT && message.view == later_view; }));
        weak_final(seat, f.b);
    }
    for (uint32_t seat{1}; seat < 9; ++seat) BOOST_CHECK(!network.nodes[seat]->DecidedCandidate());
    // Weak policy has permanent A2/B6; withholding seat0 strands both values.
    for (uint32_t seat{1}; seat < 9; ++seat) {
        std::optional<uint256> lock;
        BOOST_REQUIRE(weak_stores[seat]->ReadLock({f.context.epoch, 0}, lock, error));
        BOOST_REQUIRE(lock);
        BOOST_CHECK(*lock == (seat < 3 ? f.hash_a : f.hash_b));
    }
    // Heal honest links. The strict engines can still collect eight B commits
    // without Byzantine cooperation, because neither prematurely finalized A.
    for (uint32_t seat : {1U, 2U}) {
        BOOST_REQUIRE(network.nodes[seat]->Receive(proposal_b, error));
        BOOST_REQUIRE(network.nodes[seat]->Receive(f.Vote(AgreementStage::PREPARE, 0, f.hash_b, later_view), error));
    }
    deliver(1);
    for (uint32_t seat{1}; seat < 9; ++seat) {
        BOOST_REQUIRE(network.nodes[seat]->DecidedCandidate());
        BOOST_CHECK(*network.nodes[seat]->DecidedCandidate() == f.hash_b);
    }
    for (uint32_t seat : {1U, 2U}) {
        ProductionSigningGuard guard{*weak_stores[seat]};
        ProductionLockResult lock;
        BOOST_CHECK(!SignProductionEntryAttestation(f.secrets[seat], seat,
            *DecodeProductionEntry(f.b), f.seats, guard, lock));
        BOOST_CHECK(lock == ProductionLockResult::CONFLICT); // No erasure/unlock shortcut.
    }
}

BOOST_AUTO_TEST_CASE(hidden_prepared_certificate_survives_restart_and_selects_new_view)
{
    const AgreementFixture f;
    const fs::path path{m_args.GetDataDirBase() / "agreement-hidden-prepare"};
    std::vector<AgreementMessage> output;
    bool available{true}; std::string error;
    const auto prepared{f.Prepared(f.hash_a)};
    {
        node::FlowMeshAgreement engine{DBParams{.path = path, .cache_bytes = 1 << 20}, f.Callbacks(1, output, available)};
        BOOST_REQUIRE(engine.Open(f.context, f.seats, Filled(77), true, error));
        BOOST_REQUIRE(engine.Receive(f.Proposal(f.a), error));
        // Only this replica learns the prepared quorum; its commit is hidden.
        BOOST_REQUIRE(engine.Receive(f.Vote(AgreementStage::COMMIT, 0, f.hash_a, 0, prepared), error));
        BOOST_CHECK(!engine.DecidedCandidate());
    }
    output.clear();
    node::FlowMeshAgreement restarted{DBParams{.path = path, .cache_bytes = 1 << 20}, f.Callbacks(1, output, available)};
    BOOST_REQUIRE_MESSAGE(restarted.Open(f.context, f.seats, Filled(77), false, error), error);
    BOOST_REQUIRE(restarted.SubmitCandidate(f.b, error));
    BOOST_REQUIRE(restarted.Timeout(error));
    const auto own_report{std::find_if(output.begin(), output.end(), [](const auto& message) {
        return message.stage == AgreementStage::VIEW_CHANGE;
    })};
    BOOST_REQUIRE(own_report != output.end());
    BOOST_REQUIRE(own_report->prepared);
    BOOST_CHECK(own_report->prepared->candidate == f.hash_a);
    for (uint32_t seat : {0U, 2U, 3U, 4U, 5U, 6U}) BOOST_REQUIRE(restarted.Receive(f.Report(seat, 1), error));
    const auto new_proposal{std::find_if(output.begin(), output.end(), [](const auto& message) {
        return message.stage == AgreementStage::PROPOSAL && message.view == 1;
    })};
    BOOST_REQUIRE(new_proposal != output.end());
    BOOST_CHECK(new_proposal->candidate == f.hash_a);
    BOOST_REQUIRE(new_proposal->new_view);
    BOOST_CHECK(CheckPreagreementNewView(f.context, f.seats, *new_proposal->new_view) == PreagreementCheck::OK);
}

BOOST_AUTO_TEST_CASE(delayed_prepared_quorum_cannot_resurrect_old_commit_after_nil_view_change)
{
    const AgreementFixture f;
    std::string error; bool available{true}; std::vector<AgreementMessage> output;
    const fs::path path{m_args.GetDataDirBase() / "agreement-stale-prepare"};
    {
        node::FlowMeshAgreement engine{DBParams{.path = path, .cache_bytes = 1 << 20}, f.Callbacks(8, output, available)};
        BOOST_REQUIRE(engine.Open(f.context, f.seats, Filled(77), true, error));
        BOOST_REQUIRE(engine.Receive(f.Proposal(f.a), error));
        BOOST_REQUIRE(engine.Timeout(error));
        BOOST_CHECK_EQUAL(engine.View(), 1U);
        BOOST_REQUIRE(engine.Receive(f.Vote(AgreementStage::COMMIT, 0, f.hash_a, 0, f.Prepared(f.hash_a)), error));
    }
    node::FlowMeshAgreement restarted{DBParams{.path = path, .cache_bytes = 1 << 20}, f.Callbacks(8, output, available)};
    BOOST_REQUIRE_MESSAGE(restarted.Open(f.context, f.seats, Filled(77), false, error), error);
    BOOST_REQUIRE(restarted.Receive(f.Vote(AgreementStage::COMMIT, 1, f.hash_a, 0, f.Prepared(f.hash_a)), error));
    BOOST_REQUIRE(restarted.Retry(error));
    for (const auto& message : output) {
        BOOST_CHECK(message.stage != AgreementStage::COMMIT);
        if (message.stage == AgreementStage::VIEW_CHANGE) BOOST_CHECK(!message.prepared);
    }
}

BOOST_AUTO_TEST_CASE(formed_commit_quorum_decides_only_after_candidate_evidence_is_available)
{
    const AgreementFixture f;
    std::string error; bool available{false}; std::vector<AgreementMessage> output;
    const fs::path path{m_args.GetDataDirBase() / "agreement-hidden-decision"};
    const auto decision{f.Decision(f.a)};
    {
        node::FlowMeshAgreement engine{DBParams{.path = path, .cache_bytes = 1 << 20}, f.Callbacks(8, output, available)};
        BOOST_REQUIRE(engine.Open(f.context, f.seats, Filled(77), true, error));
        BOOST_REQUIRE(engine.Receive(decision, error));
        BOOST_CHECK(!engine.DecidedCandidate());
        BOOST_CHECK(output.empty());
        available = true;
        BOOST_REQUIRE(engine.Retry(error));
        BOOST_REQUIRE(engine.DecidedCandidate());
        BOOST_CHECK(*engine.DecidedCandidate() == f.hash_a);
        BOOST_REQUIRE(engine.RestoreCandidateBlob(f.hash_a));
    }
    available = false; output.clear();
    node::FlowMeshAgreement restarted{DBParams{.path = path, .cache_bytes = 1 << 20}, f.Callbacks(8, output, available)};
    BOOST_REQUIRE_MESSAGE(restarted.Open(f.context, f.seats, Filled(77), false, error), error);
    // A later invalid/unavailable anchor does not erase an already durable
    // decision. Runtime must continue to revalidate before any V1 signing.
    BOOST_REQUIRE(restarted.DecidedCandidate());
    BOOST_CHECK(*restarted.DecidedCandidate() == f.hash_a);
    BOOST_REQUIRE(restarted.Retry(error));
    BOOST_REQUIRE(!output.empty());
    BOOST_CHECK(EncodeAgreementMessage(output.back()) == EncodeAgreementMessage(decision));
}

BOOST_AUTO_TEST_CASE(old_view_candidate_retries_retained_bytes_after_evidence_recovers)
{
    const AgreementFixture f;
    std::string error; bool available{false}; std::vector<AgreementMessage> output;
    const fs::path path{m_args.GetDataDirBase() / "agreement-old-view-evidence"};
    const auto proposal{f.Proposal(f.a)};
    const auto exact{EncodeAgreementMessage(proposal)};
    {
        node::FlowMeshAgreement engine{DBParams{.path = path, .cache_bytes = 1 << 20},
            f.Callbacks(8, output, available)};
        BOOST_REQUIRE(engine.Open(f.context, f.seats, Filled(77), true, error));
        BOOST_REQUIRE(engine.Timeout(error));
        BOOST_REQUIRE_EQUAL(engine.View(), 1U);
        BOOST_REQUIRE(engine.Receive(proposal, error));
        BOOST_CHECK(engine.RequiredCandidateHash() == f.hash_a);
        BOOST_CHECK(!engine.CandidateBytes(f.hash_a));
        BOOST_REQUIRE(engine.Retry(error));
        BOOST_CHECK(!engine.CandidateBytes(f.hash_a));
        available = true;
        output.clear();
        // No second Receive: admission may suppress this exact proposal for
        // its full relay backoff. The first local retry must recover its body.
        BOOST_REQUIRE(engine.Retry(error));
        BOOST_CHECK(engine.CandidateBytes(f.hash_a) == f.a);
        BOOST_CHECK(!engine.RequiredCandidateHash());
        BOOST_CHECK(!engine.DecidedCandidate());
        BOOST_CHECK(std::any_of(output.begin(), output.end(), [&](const auto& message) {
            return EncodeAgreementMessage(message) == exact;
        }));
        BOOST_CHECK(std::none_of(output.begin(), output.end(), [](const auto& message) {
            return message.view == 0 && (message.stage == AgreementStage::PREPARE ||
                                        message.stage == AgreementStage::COMMIT);
        }));
    }
    output.clear();
    node::FlowMeshAgreement restarted{DBParams{.path = path, .cache_bytes = 1 << 20},
        f.Callbacks(8, output, available)};
    BOOST_REQUIRE_MESSAGE(restarted.Open(f.context, f.seats, Filled(77), false, error), error);
    BOOST_CHECK_EQUAL(restarted.View(), 1U);
    BOOST_CHECK(restarted.CandidateBytes(f.hash_a) == f.a);
    BOOST_REQUIRE(restarted.Retry(error));
    BOOST_CHECK(std::any_of(output.begin(), output.end(), [&](const auto& message) {
        return EncodeAgreementMessage(message) == exact;
    }));
}

BOOST_AUTO_TEST_CASE(old_view_candidate_retry_rotates_past_unavailable_required_hash)
{
    const AgreementFixture f;
    std::string error; bool available{true}, recover_a{false}; std::vector<AgreementMessage> output;
    auto callbacks{f.Callbacks(8, output, available)};
    const auto validate{callbacks.validate_candidate};
    size_t validations{0};
    callbacks.validate_candidate = [&](std::span<const unsigned char> bytes,
        std::optional<std::span<const unsigned char>> evidence) -> std::optional<Bytes> {
        ++validations;
        if (!recover_a || DecodeProductionEntry(bytes)->GetHash() != f.hash_a) return std::nullopt;
        return validate(bytes, evidence);
    };
    node::FlowMeshAgreement engine{DBParams{
        .path = "agreement-old-view-fair-retry", .cache_bytes = 1 << 20, .memory_only = true}, std::move(callbacks)};
    BOOST_REQUIRE(engine.Open(f.context, f.seats, Filled(77), true, error));
    BOOST_REQUIRE(engine.Receive(f.Proposal(f.a), error));
    PreagreementNewViewProof proof{1, f.hash_b, {}};
    for (uint32_t seat{0}; seat < 7; ++seat) {
        const auto report{f.Report(seat, 1)};
        proof.reports.push_back({1, seat, std::nullopt, report.signature});
    }
    BOOST_REQUIRE(engine.Receive(f.Proposal(f.b, 1, proof), error));
    BOOST_REQUIRE(engine.Timeout(error));
    BOOST_REQUIRE_EQUAL(engine.View(), 2U);
    BOOST_CHECK(engine.RequiredCandidateHash() == f.hash_b);
    recover_a = true;
    const auto before{validations};
    BOOST_REQUIRE(engine.Retry(error));
    BOOST_CHECK_LE(validations - before, 2U);
    BOOST_CHECK(engine.CandidateBytes(f.hash_a) == f.a);
    BOOST_CHECK(!engine.CandidateBytes(f.hash_b));
    BOOST_CHECK(engine.RequiredCandidateHash() == f.hash_b);
    BOOST_CHECK(!engine.DecidedCandidate());
    BOOST_CHECK(!engine.Halted());
}

BOOST_AUTO_TEST_CASE(pending_decision_evidence_takes_priority_over_old_proposal_recovery)
{
    const AgreementFixture f;
    std::string error; bool available{false}; std::vector<AgreementMessage> output;
    auto callbacks{f.Callbacks(8, output, available)};
    const auto validate{callbacks.validate_candidate};
    // Model a runtime with one candidate-cache position remaining. Full
    // quorum validation is unchanged; only local execution capacity is scarce.
    std::optional<uint256> retained;
    size_t validations{0};
    callbacks.validate_candidate = [&](std::span<const unsigned char> bytes,
        std::optional<std::span<const unsigned char>> evidence) -> std::optional<Bytes> {
        ++validations;
        const auto hash{DecodeProductionEntry(bytes)->GetHash()};
        if (retained && *retained != hash) return std::nullopt;
        const auto result{validate(bytes, evidence)};
        if (result) retained = hash;
        return result;
    };
    node::FlowMeshAgreement engine{DBParams{
        .path = "agreement-decision-recovery-priority", .cache_bytes = 1 << 20, .memory_only = true}, std::move(callbacks)};
    BOOST_REQUIRE(engine.Open(f.context, f.seats, Filled(77), true, error));
    BOOST_REQUIRE(engine.Timeout(error));
    BOOST_REQUIRE(engine.Receive(f.Proposal(f.b), error));
    BOOST_REQUIRE(engine.Receive(f.Decision(f.a), error));
    BOOST_CHECK(!retained);
    BOOST_CHECK(!engine.DecidedCandidate());
    available = true;
    BOOST_REQUIRE(engine.Retry(error));
    BOOST_CHECK(retained == f.hash_a);
    BOOST_CHECK(engine.DecidedCandidate() == f.hash_a);
    BOOST_CHECK(!engine.CandidateBytes(f.hash_b));
    const auto after_decision{validations};
    BOOST_REQUIRE(engine.Retry(error));
    BOOST_CHECK_EQUAL(validations, after_decision);
    BOOST_CHECK(!engine.Halted());
}

BOOST_AUTO_TEST_CASE(duplicate_commit_can_supply_missing_proof_without_another_vote)
{
    const AgreementFixture f;
    std::string error; bool available{true}; std::vector<AgreementMessage> output;
    auto callbacks{f.Callbacks(8, output, available)};
    callbacks.local_keys = [] { return std::vector<bls::SecretKey>{}; };
    node::FlowMeshAgreement engine{DBParams{
        .path = m_args.GetDataDirBase() / "agreement-proof-enrichment", .cache_bytes = 1 << 20},
        std::move(callbacks)};
    BOOST_REQUIRE(engine.Open(f.context, f.seats, Filled(77), true, error));
    BOOST_REQUIRE(engine.Receive(f.Proposal(f.a), error));
    // A relayer can deliver the authenticated COMMIT shares before their
    // optional supporting proof. Seven shares alone are not a full decision.
    for (uint32_t seat{0}; seat < 7; ++seat) {
        BOOST_REQUIRE(engine.Receive(f.Vote(AgreementStage::COMMIT, seat, f.hash_a), error));
    }
    BOOST_CHECK(!engine.DecidedCandidate());
    const auto enriched{f.Vote(AgreementStage::COMMIT, 0, f.hash_a, 0, f.Prepared(f.hash_a))};
    BOOST_CHECK(enriched.signature == f.Vote(AgreementStage::COMMIT, 0, f.hash_a).signature);
    BOOST_REQUIRE(engine.Receive(enriched, error));
    BOOST_REQUIRE(engine.DecidedCandidate());
    BOOST_CHECK(*engine.DecidedCandidate() == f.hash_a);
    BOOST_REQUIRE(!output.empty());
    BOOST_REQUIRE(output.back().decision);
    BOOST_CHECK_EQUAL(output.back().decision->commits.size(), 7U);
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, *output.back().decision) == PreagreementCheck::OK);
}

BOOST_AUTO_TEST_CASE(duplicate_commit_enrichment_preserves_quorum_and_new_view_requirement)
{
    const AgreementFixture f;
    std::string error; bool available{true}; std::vector<AgreementMessage> output;
    auto callbacks{f.Callbacks(8, output, available)};
    callbacks.local_keys = [] { return std::vector<bls::SecretKey>{}; };
    node::FlowMeshAgreement engine{DBParams{
        .path = m_args.GetDataDirBase() / "agreement-new-view-enrichment", .cache_bytes = 1 << 20},
        std::move(callbacks)};
    BOOST_REQUIRE(engine.Open(f.context, f.seats, Filled(77), true, error));
    BOOST_REQUIRE(engine.SubmitCandidate(f.a, error));
    for (uint32_t seat{0}; seat < 3; ++seat) BOOST_REQUIRE(engine.Receive(f.Report(seat, 1), error));
    BOOST_REQUIRE_EQUAL(engine.View(), 1U);
    for (uint32_t seat{0}; seat < 6; ++seat) {
        BOOST_REQUIRE(engine.Receive(f.Vote(AgreementStage::COMMIT, seat, f.hash_a, 1), error));
    }
    auto enriched{f.Vote(AgreementStage::COMMIT, 0, f.hash_a, 1, f.Prepared(f.hash_a, 1))};
    for (size_t repeat{0}; repeat < 16; ++repeat) BOOST_REQUIRE(engine.Receive(enriched, error));
    BOOST_CHECK(!engine.DecidedCandidate()); // Six seats do not become seven by retrying.
    BOOST_REQUIRE(engine.Receive(f.Vote(AgreementStage::COMMIT, 6, f.hash_a, 1), error));
    BOOST_CHECK(!engine.DecidedCandidate()); // Still missing the complete new-view justification.
    PreagreementNewViewProof proof{1, f.hash_a, {}};
    for (uint32_t seat{0}; seat < 7; ++seat) {
        const auto report{f.Report(seat, 1)};
        proof.reports.push_back({1, seat, std::nullopt, report.signature});
    }
    enriched.new_view = proof;
    BOOST_REQUIRE(engine.Receive(enriched, error));
    BOOST_REQUIRE(engine.DecidedCandidate());
    BOOST_CHECK(*engine.DecidedCandidate() == f.hash_a);
    BOOST_REQUIRE(output.back().decision);
    BOOST_CHECK_EQUAL(output.back().decision->commits.size(), 7U);
    BOOST_CHECK(CheckPreagreementCommit(f.context, f.seats, *output.back().decision) == PreagreementCheck::OK);
}

BOOST_AUTO_TEST_CASE(exact_sign_intent_and_signature_resume_at_every_crash_boundary)
{
    const AgreementFixture f;
    for (const auto point : {node::FlowMeshAgreementCrashPoint::AFTER_INTENT_PERSIST,
                            node::FlowMeshAgreementCrashPoint::AFTER_SIGNATURE,
                            node::FlowMeshAgreementCrashPoint::AFTER_SIGNED_PERSIST}) {
        const fs::path path{m_args.GetDataDirBase() / fs::PathFromString("agreement-crash-" + std::to_string(static_cast<int>(point)))};
        std::string error; bool available{true}; std::vector<AgreementMessage> output;
        auto callbacks{f.Callbacks(8, output, available)};
        callbacks.crash = [point](const auto reached) { if (point == reached) throw std::runtime_error("simulated interruption"); };
        {
            node::FlowMeshAgreement engine{DBParams{.path = path, .cache_bytes = 1 << 20}, std::move(callbacks)};
            BOOST_REQUIRE(engine.Open(f.context, f.seats, Filled(77), true, error));
            BOOST_CHECK(!engine.Receive(f.Proposal(f.a), error));
            BOOST_CHECK(engine.Halted());
            BOOST_CHECK(output.empty());
        }
        node::FlowMeshAgreement restarted{DBParams{.path = path, .cache_bytes = 1 << 20}, f.Callbacks(8, output, available)};
        BOOST_REQUIRE_MESSAGE(restarted.Open(f.context, f.seats, Filled(77), false, error), error);
        BOOST_REQUIRE(restarted.Retry(error));
        BOOST_REQUIRE(!output.empty());
        const auto exact{EncodeAgreementMessage(f.Vote(AgreementStage::PREPARE, 8, f.hash_a))};
        for (const auto& message : output) {
            if (message.stage == AgreementStage::PROPOSAL) BOOST_CHECK(EncodeAgreementMessage(message) == EncodeAgreementMessage(f.Proposal(f.a)));
            else BOOST_CHECK(EncodeAgreementMessage(message) == exact);
        }
        BOOST_CHECK(!restarted.Receive(f.Proposal(f.b), error));
        BOOST_CHECK(!restarted.Halted());
        BOOST_REQUIRE(restarted.Retry(error));
        for (const auto& message : output) {
            if (message.stage == AgreementStage::PROPOSAL) BOOST_CHECK(EncodeAgreementMessage(message) == EncodeAgreementMessage(f.Proposal(f.a)));
            else BOOST_CHECK(EncodeAgreementMessage(message) == exact);
        }
    }
}

BOOST_AUTO_TEST_CASE(decision_publication_waits_for_sync_and_restart_retains_the_complete_proof)
{
    const AgreementFixture f;
    for (const auto point : {node::FlowMeshAgreementCrashPoint::BEFORE_DECISION_PERSIST,
                            node::FlowMeshAgreementCrashPoint::AFTER_DECISION_PERSIST}) {
        const fs::path path{m_args.GetDataDirBase() / fs::PathFromString("agreement-decision-crash-" + std::to_string(static_cast<int>(point)))};
        std::string error; bool available{true}; std::vector<AgreementMessage> output;
        auto callbacks{f.Callbacks(8, output, available)};
        callbacks.crash = [point](const auto reached) { if (point == reached) throw std::runtime_error("simulated interruption"); };
        {
            node::FlowMeshAgreement engine{DBParams{.path = path, .cache_bytes = 1 << 20}, std::move(callbacks)};
            BOOST_REQUIRE(engine.Open(f.context, f.seats, Filled(77), true, error));
            BOOST_CHECK(!engine.Receive(f.Decision(f.a), error));
            BOOST_CHECK(!engine.DecidedCandidate());
            BOOST_CHECK(output.empty());
        }
        node::FlowMeshAgreement restarted{DBParams{.path = path, .cache_bytes = 1 << 20}, f.Callbacks(8, output, available)};
        BOOST_REQUIRE_MESSAGE(restarted.Open(f.context, f.seats, Filled(77), false, error), error);
        BOOST_CHECK_EQUAL(restarted.DecidedCandidate().has_value(), point == node::FlowMeshAgreementCrashPoint::AFTER_DECISION_PERSIST);
        BOOST_REQUIRE(restarted.Receive(f.Decision(f.a), error));
        BOOST_REQUIRE(restarted.DecidedCandidate());
        BOOST_REQUIRE(restarted.Retry(error));
        for (const auto& message : output) BOOST_CHECK(EncodeAgreementMessage(message) == EncodeAgreementMessage(f.Decision(f.a)));
    }
}

BOOST_AUTO_TEST_CASE(higher_claims_require_f_plus_one_reports_and_wrong_or_missing_journal_fails_closed)
{
    const AgreementFixture f;
    std::string error; bool available{true}; std::vector<AgreementMessage> output;
    const fs::path path{m_args.GetDataDirBase() / "agreement-authenticated-view"};
    {
        node::FlowMeshAgreement engine{DBParams{.path = path, .cache_bytes = 1 << 20}, f.Callbacks(8, output, available)};
        BOOST_REQUIRE(engine.Open(f.context, f.seats, Filled(77), true, error));
        BOOST_REQUIRE(engine.Receive(f.Report(0, 4), error));
        BOOST_REQUIRE(engine.Receive(f.Report(1, 4), error));
        BOOST_CHECK_EQUAL(engine.View(), 0U);
        BOOST_REQUIRE(engine.Receive(f.Report(2, 4), error));
        BOOST_CHECK_EQUAL(engine.View(), 4U);
        BOOST_CHECK(!engine.DecidedCandidate());
    }
    {
        node::FlowMeshAgreement wrong{DBParams{.path = path, .cache_bytes = 1 << 20}, f.Callbacks(8, output, available)};
        BOOST_CHECK(!wrong.Open(f.context, f.seats, Filled(78), false, error));
        BOOST_CHECK(wrong.Halted());
    }
    node::FlowMeshAgreement missing{DBParams{.path = m_args.GetDataDirBase() / "agreement-missing", .cache_bytes = 1 << 20},
        f.Callbacks(8, output, available)};
    BOOST_CHECK(!missing.Open(f.context, f.seats, Filled(77), false, error));
    BOOST_CHECK(missing.Halted());
}

BOOST_AUTO_TEST_CASE(restart_waits_for_keys_and_evidence_before_resuming_an_exact_intent)
{
    const AgreementFixture f;
    std::string error; bool available{true}; std::vector<AgreementMessage> output;
    const fs::path path{m_args.GetDataDirBase() / "agreement-unarmed-restart"};
    auto callbacks{f.Callbacks(8, output, available)};
    callbacks.crash = [](const auto point) {
        if (point == node::FlowMeshAgreementCrashPoint::AFTER_INTENT_PERSIST) throw std::runtime_error("interrupted intent");
    };
    {
        node::FlowMeshAgreement engine{DBParams{.path = path, .cache_bytes = 1 << 20}, std::move(callbacks)};
        BOOST_REQUIRE(engine.Open(f.context, f.seats, Filled(77), true, error));
        BOOST_CHECK(!engine.Receive(f.Proposal(f.a), error));
        BOOST_CHECK(output.empty());
    }
    bool armed{false}; available = false;
    callbacks = f.Callbacks(8, output, available);
    callbacks.local_keys = [&] { return armed ? std::vector<bls::SecretKey>{f.secrets[8]} : std::vector<bls::SecretKey>{}; };
    node::FlowMeshAgreement restarted{DBParams{.path = path, .cache_bytes = 1 << 20}, std::move(callbacks)};
    BOOST_REQUIRE_MESSAGE(restarted.Open(f.context, f.seats, Filled(77), false, error), error);
    BOOST_REQUIRE(restarted.Retry(error));
    BOOST_REQUIRE(restarted.Timeout(error));
    BOOST_CHECK_EQUAL(restarted.View(), 0U);
    for (const auto& message : output) BOOST_CHECK(message.stage == AgreementStage::PROPOSAL);
    armed = true;
    BOOST_REQUIRE(restarted.Retry(error));
    for (const auto& message : output) BOOST_CHECK(message.stage == AgreementStage::PROPOSAL);
    available = true;
    BOOST_REQUIRE(restarted.Retry(error));
    const auto resumed{std::find_if(output.begin(), output.end(), [](const auto& message) { return message.stage == AgreementStage::PREPARE; })};
    BOOST_REQUIRE(resumed != output.end());
    BOOST_CHECK(EncodeAgreementMessage(*resumed) == EncodeAgreementMessage(f.Vote(AgreementStage::PREPARE, 8, f.hash_a)));
    BOOST_CHECK(!restarted.Halted());
}

BOOST_AUTO_TEST_CASE(certified_advancement_retains_history_and_cannot_reopen_an_old_slot)
{
    const AgreementFixture f;
    std::string error; bool available{true}; std::vector<AgreementMessage> output;
    const fs::path path{m_args.GetDataDirBase() / "agreement-forward-history"};
    auto next{f.context}; next.sequence = 1; next.parent_hash = f.hash_a; next.previous_state_root = Filled(8);
    {
        node::FlowMeshAgreement engine{DBParams{.path = path, .cache_bytes = 1 << 20}, f.Callbacks(8, output, available)};
        BOOST_REQUIRE(engine.Open(f.context, f.seats, Filled(77), true, error));
        BOOST_REQUIRE(engine.Receive(f.Decision(f.a), error));
        BOOST_REQUIRE(engine.Advance(next, f.seats, error));
        BOOST_CHECK(!engine.DecidedCandidate());
        BOOST_CHECK_EQUAL(engine.Context().sequence, 1U);
    }
    {
        node::FlowMeshAgreement restarted{DBParams{.path = path, .cache_bytes = 1 << 20}, f.Callbacks(8, output, available)};
        BOOST_REQUIRE_MESSAGE(restarted.Open(next, f.seats, Filled(77), false, error), error);
        BOOST_CHECK_EQUAL(restarted.Context().sequence, 1U);
    }
    {
        CDBWrapper db{DBParams{.path = path, .cache_bytes = 1 << 20}};
        std::unique_ptr<CDBIterator> it{db.NewIterator()};
        size_t slots{0};
        for (it->Seek(uint8_t{'s'}); it->Valid(); it->Next()) {
            uint8_t prefix{0}; BOOST_REQUIRE(it->GetKey(prefix));
            if (prefix == 's') ++slots;
        }
        BOOST_CHECK_EQUAL(slots, 2U);
    }
    node::FlowMeshAgreement backwards{DBParams{.path = path, .cache_bytes = 1 << 20}, f.Callbacks(8, output, available)};
    BOOST_CHECK(!backwards.Open(f.context, f.seats, Filled(77), false, error));
    BOOST_CHECK(backwards.Halted());
}

BOOST_AUTO_TEST_CASE(retry_keeps_exact_denied_message_for_the_next_admission)
{
    const AgreementFixture f;
    std::string error; bool available{true}; std::vector<AgreementMessage> output;
    auto callbacks{f.Callbacks(8, output, available)};
    bool admitting{false}; size_t attempts{0};
    callbacks.publish = [&](const AgreementMessage& message) {
        ++attempts;
        if (!admitting) return false;
        output.push_back(message); return true;
    };
    node::FlowMeshAgreement engine{DBParams{.path = "agreement-admission", .cache_bytes = 1 << 20, .memory_only = true}, std::move(callbacks)};
    BOOST_REQUIRE(engine.Open(f.context, f.seats, Filled(77), true, error));
    BOOST_REQUIRE(engine.Receive(f.Proposal(f.a), error));
    BOOST_CHECK_EQUAL(attempts, 1U);
    BOOST_CHECK(output.empty());
    BOOST_REQUIRE(engine.Timeout(error));
    BOOST_REQUIRE(engine.Retry(error));
    BOOST_CHECK(output.empty());
    admitting = true;
    BOOST_REQUIRE(engine.Retry(error));
    BOOST_REQUIRE(!output.empty());
    BOOST_CHECK(EncodeAgreementMessage(output.front()) == EncodeAgreementMessage(f.Vote(AgreementStage::PREPARE, 8, f.hash_a)));
    const auto report{std::find_if(output.begin(), output.end(), [](const auto& message) { return message.stage == AgreementStage::VIEW_CHANGE; })};
    BOOST_REQUIRE(report != output.end());
    BOOST_CHECK_EQUAL(report->view, 1U);
}

BOOST_AUTO_TEST_SUITE_END()
