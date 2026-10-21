// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see COPYING.
#include <boost/test/unit_test.hpp>
#include <flowmesh2/agreement/agreement.h>
#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/agreement/pbft.h>
#include <flowmesh2/agreement/control_messages.h>
#include <flowmesh2/agreement/safety.h>
#include <flowmesh2/storage/control_log.h>
#include <flowmesh2/net/seat_mesh.h>
#include <flowmesh2/test/util/agreement_fixtures.h>
#include <algorithm>
#include <array>
#include <deque>
#include <functional>
#include <set>
#include <util/chaintype.h>
using namespace flowmesh2;
using namespace flowmesh2::agreement;
using namespace flowmesh2::test;
namespace {

struct ViewFrame {
    uint32_t from;
    uint32_t to;
    std::vector<unsigned char> bytes;
};

class ViewQueueSink final : public MessageSink
{
public:
    ViewQueueSink(uint32_t self, std::deque<ViewFrame>& frames) : m_self{self}, m_frames{frames} {}
    std::set<uint32_t> busy;
    Result<void> Send(uint32_t seat, std::span<const unsigned char> bytes) override
    {
        if (busy.contains(seat)) return Error{ErrorCode::BUSY, "peer deliberately never accepts a frame"};
        m_frames.push_back(ViewFrame{m_self, seat, {bytes.begin(), bytes.end()}});
        return Ok();
    }
private:
    uint32_t m_self;
    std::deque<ViewFrame>& m_frames;
};

struct ViewReplica {
    ProfileParams profile{StableQViewRegtestProfile(ChainType::REGTEST).value()};
    SeatSet seats{TestSeats({"s0", "s1", "s2", "s3"})};
    AuthorityVersion authority{0, seats.Root(), ProfileHash(profile)};
    std::unique_ptr<dev::FakeChainAdapter> adapter{dev::FakeChainAdapter::Create(TestNetwork(), authority, seats, TestAnchor(129)).value()};
    RecordingObserver observer;
    std::unique_ptr<storage::MemoryJournal> signer_journal;
    std::unique_ptr<storage::MemoryJournal> commit_journal;
    std::unique_ptr<storage::MemoryJournal> control_journal;
    storage::SignerLog signer_log;
    storage::CommitLog commit_log;
    storage::ControlLog control_log;
    storage::SeatSigner signer;
    state::HeadView genesis{state::GenesisHead(profile, std::vector<MarketDescriptor>{MarketM1(), MarketM2()}, TestDomain(), authority, TestAnchor(129), {130, 0}).value()};
    std::unique_ptr<state::StateOwner> owner;
    PbftSafetyRules safety;
    ViewQueueSink sink;
    std::unique_ptr<Agreement> agreement;

    ViewReplica(uint32_t self, std::deque<ViewFrame>& frames, storage::MemoryJournalFaults faults = {})
        : signer_journal{NewMemoryJournal(storage::JournalRole::SIGNER, 10 + self, faults, &observer)},
          commit_journal{NewMemoryJournal(storage::JournalRole::COMMIT, 20 + self, {}, &observer)},
          control_journal{NewMemoryJournal(storage::JournalRole::SIGNER, 30 + self, {}, &observer)},
          signer_log{storage::SignerLog::Create(*signer_journal, profile).value()},
          commit_log{storage::CommitLog::Create(*commit_journal, profile).value()},
          control_log{storage::ControlLog::Create(*control_journal, {TestDomain(), ProfileHash(profile), seats.Root(), 0, self, 4, seats.Keys()[self].Key()}).value()},
          signer{storage::SeatSigner::FromIKM(AgreementRig::SeatIkm("s" + std::to_string(self)), signer_log, control_log).value()},
          owner{state::StateOwner::Create(profile, genesis, commit_journal->InstanceId()).value()}, sink{self, frames}
    {
        auto deposit{DepositAtCursor({130, 0}, "pbft-deposit", 10'000)};
        deposit.account = KeyAccount("alice");
        RequireOk(adapter->AppendBlock(TestAnchor(130).hash, {Fact{deposit}}), "chain fact");
        PbftConfig config;
        config.self_seat = self;
        config.regtest_full_view = true;
        config.view_timeout_floor_ms = 500;
        const PbftContext context{*adapter, *owner, signer, signer_log, commit_log, safety, sink, &control_log};
        auto created{PbftAgreement::Create(profile, config, context)};
        RequireOk(created, "full view PBFT factory");
        agreement = std::move(*created);
    }

    state::Candidate Candidate(std::vector<std::vector<unsigned char>> originals = {}) const
    {
        auto poll{adapter->Poll(owner->Head()->Header().facts_to, MAX_SLOT_FACTS)};
        RequireOk(poll, "poll complete fact stream");
        BOOST_REQUIRE(poll->Batch());
        const auto& batch{*poll->Batch()};
        state::SlotInputs inputs;
        inputs.authority = authority;
        inputs.anchor = batch.anchor;
        inputs.facts_from = batch.from;
        inputs.facts_to = batch.to;
        for (const Fact& fact : batch.facts) inputs.body.facts.push_back(EncodeFact(fact).value());
        inputs.body.originals = std::move(originals);
        auto result{owner->Preview(state::ParentRef::Committed(*owner->Head()), inputs)};
        RequireOk(result, "candidate");
        return std::move(*result);
    }

    void RestartAgreement(uint32_t self)
    {
        agreement.reset();
        PbftConfig config; config.self_seat = self; config.regtest_full_view = true; config.view_timeout_floor_ms = 500;
        if (owner->Head()->Header().height) {
            const auto records{commit_journal->ReadAll()};
            RequireOk(records, "retained parent decision history");
            for (const auto& record : *records) if (record.type == storage::RecordType::DECISION) {
                const auto decision{storage::DecisionRecord::Decode(record.payload, profile)};
                RequireOk(decision, "retained parent decision record");
                if (decision->header.height == owner->Head()->Header().height) config.parent_certificate = decision->certificate;
            }
        }
        const PbftContext context{*adapter, *owner, signer, signer_log, commit_log, safety, sink, &control_log};
        auto restored{PbftAgreement::Create(profile, config, context)};
        RequireOk(restored, "restart control proof recovery"); agreement = std::move(*restored);
    }

    void ApplyDecision()
    {
        auto decision{agreement->TakeDecision()};
        if (!decision) return;
        // The exact header is already independently reexecuted at proposal
        // acceptance; recover it from this node's durable decision record.
        auto records{commit_journal->ReadAll()};
        RequireOk(records, "decision history");
        std::optional<storage::DecisionRecord> record;
        for (const auto& item : *records) {
            if (item.type == storage::RecordType::DECISION) record = storage::DecisionRecord::Decode(item.payload, profile).value();
        }
        BOOST_REQUIRE(record);
        auto candidate{owner->Reexecute(record->header, record->body)};
        RequireOk(candidate, "apply reexecution");
        auto applied{owner->Apply(std::move(*candidate), decision->decision, authority)};
        RequireOk(applied, "apply");
        auto pending{commit_log.AppendApplied(applied->receipt)};
        RequireOk(pending, "applied marker");
        RequireOk(commit_log.Barrier(), "applied barrier");
        RequireOk(commit_log.Confirm(*pending), "durable applied marker");
    }
};

struct ViewNetwork {
    std::deque<ViewFrame> frames;
    std::array<std::unique_ptr<ViewReplica>, 4> nodes;
    ViewNetwork()
    {
        for (uint32_t i{0}; i < 4; ++i) nodes[i] = std::make_unique<ViewReplica>(i, frames);
    }
    std::vector<ViewFrame> history;
    void Drain(const std::set<uint32_t>& offline = {}, std::function<bool(const ViewFrame&)> filter = {})
    {
        size_t work{0};
        while (!frames.empty()) {
            BOOST_REQUIRE(++work < 5'000);
            ViewFrame frame{std::move(frames.front())};
            frames.pop_front();
            history.push_back(frame);
            if (filter && !filter(frame)) continue;
            if (offline.contains(frame.from) || offline.contains(frame.to)) continue;
            RequireOk(nodes[frame.to]->agreement->OnMessage(frame.from, frame.bytes), "receive authenticated message");
            nodes[frame.to]->ApplyDecision();
        }
        for (auto& node : nodes) node->ApplyDecision();
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(pbft_view_change_tests)

BOOST_AUTO_TEST_CASE(silent_initial_leader_moves_without_user_resubmission)
{
    ViewNetwork network;
    auto candidate{network.nodes[0]->Candidate()};
    const auto expected{candidate.Header().Hash()};
    RequireOk(network.nodes[0]->agreement->Propose(candidate, network.nodes[0]->authority), "nonleader offers original work");
    network.Drain({1});
    for (uint32_t i : {0U, 2U, 3U}) RequireOk(network.nodes[i]->agreement->OnTimeout(0), "start active timer");
    for (uint32_t i : {0U, 2U, 3U}) RequireOk(network.nodes[i]->agreement->OnTimeout(500), "enter next view");
    network.Drain({1});
    for (uint32_t i : {0U, 2U, 3U}) {
        auto& node{*network.nodes[i]};
        BOOST_CHECK_EQUAL(node.owner->Head()->Header().height, 1U);
        BOOST_CHECK(node.owner->Head()->Header().Hash() == expected);
        BOOST_CHECK_EQUAL(node.agreement->CurrentView(), 0U); // next slot, before Refresh
        BOOST_CHECK(!node.agreement->ChangingView());
        const auto records{node.commit_journal->ReadAll().value()};
        for (const auto& record : records) if (record.type == storage::RecordType::DECISION) {
            const auto decision{storage::DecisionRecord::Decode(record.payload, node.profile).value()};
            BOOST_REQUIRE(decision.certificate.view);
            BOOST_CHECK_EQUAL(*decision.certificate.view, 1U);
        }
    }
    BOOST_CHECK_EQUAL(network.nodes[1]->owner->Head()->Header().height, 0U);
}

BOOST_AUTO_TEST_CASE(prepared_a_body_reaches_new_leader_and_defeats_fresh_b)
{
    ViewNetwork network;
    auto funding{network.nodes[1]->Candidate()};
    RequireOk(network.nodes[1]->agreement->Propose(funding, network.nodes[1]->authority), "certified trusted funding first");
    network.Drain();
    SlotFixture actions{network.nodes[0]->profile};
    const auto original_a{actions.Cancel("alice", 1, OrderId{LabelHash("prepared cancel A")})};
    const auto original_b{actions.Cancel("alice", 1, OrderId{LabelHash("fresh cancel B")})};
    auto a{network.nodes[2]->Candidate({original_a})};
    auto b{network.nodes[3]->Candidate({original_b})};
    BOOST_CHECK(a.Header().anchor == b.Header().anchor);
    BOOST_CHECK(a.Body().facts == b.Body().facts);
    BOOST_REQUIRE(a.Header().Hash() != b.Header().Hash());
    RequireOk(network.nodes[2]->agreement->Propose(a, network.nodes[2]->authority), "original leader A");
    network.Drain({}, [&](const ViewFrame& frame) {
        if (frame.to == 3 || frame.from == 3) return false;
        const auto decoded{DecodeMessage(frame.bytes, network.nodes[0]->profile).value()};
        const auto* vote{std::get_if<Vote>(&decoded)};
        return !vote || vote->phase != storage::VotePhase::COMMIT;
    });
    for (uint32_t i : {0U, 1U, 2U}) {
        BOOST_REQUIRE(network.nodes[i]->commit_log.PreparedAt(0, 2, 0));
        BOOST_CHECK(!network.nodes[i]->commit_log.DecisionAt(2));
    }
    RequireOk(network.nodes[3]->agreement->Propose(b, network.nodes[3]->authority), "future leader only knows fresh B");
    network.Drain({2});
    for (uint32_t i : {0U, 1U, 3U}) RequireOk(network.nodes[i]->agreement->OnTimeout(0), "timer baseline");
    for (uint32_t i : {0U, 1U, 3U}) RequireOk(network.nodes[i]->agreement->OnTimeout(500), "prepared leader loss");
    network.Drain({2});
    for (uint32_t i : {0U, 1U, 3U}) {
        BOOST_CHECK_EQUAL(network.nodes[i]->owner->Head()->Header().height, 2U);
        BOOST_CHECK(network.nodes[i]->owner->Head()->Header().Hash() == a.Header().Hash());
        BOOST_CHECK(network.nodes[i]->owner->Head()->Header().Hash() != b.Header().Hash());
    }
}

BOOST_AUTO_TEST_CASE(naked_prepare_quorum_does_not_create_proposal_acceptance)
{
    ViewNetwork network;
    auto a{network.nodes[1]->Candidate()};
    RequireOk(network.nodes[1]->agreement->Propose(a, network.nodes[1]->authority), "leader A");
    network.Drain({}, [&](const ViewFrame& frame) {
        if (frame.to == 2 || frame.from == 2) return false;
        const auto decoded{DecodeMessage(frame.bytes, network.nodes[0]->profile).value()};
        const auto* vote{std::get_if<Vote>(&decoded)};
        return !vote || vote->phase != storage::VotePhase::COMMIT;
    });
    const Proposal offered{0, 0, a.Header(), std::nullopt, a.Body()};
    const auto bytes{EncodeMessage(Message{OfferedProposal{offered}}, network.nodes[2]->profile).value()};
    RequireOk(network.nodes[2]->agreement->OnMessage(0, bytes), "nonvoting body availability");
    std::set<uint32_t> delivered;
    for (const auto& frame : network.history) {
        const auto decoded{DecodeMessage(frame.bytes, network.nodes[0]->profile).value()};
        const auto* vote{std::get_if<Vote>(&decoded)};
        if (vote && vote->phase == storage::VotePhase::PREPARE && delivered.insert(vote->seat).second) {
            RequireOk(network.nodes[2]->agreement->OnMessage(vote->seat, frame.bytes), "real prepare share without proposal");
        }
    }
    BOOST_REQUIRE_EQUAL(delivered.size(), 3U);
    BOOST_CHECK(network.nodes[2]->commit_log.PreparedAt(0, 1, 0));
    BOOST_CHECK_EQUAL(network.nodes[2]->signer_journal->Status().appended_seq, 0U);
    BOOST_CHECK(!network.nodes[2]->commit_log.DecisionAt(1));
}

BOOST_AUTO_TEST_CASE(changing_restart_reuses_exact_report_without_advancing_timer)
{
    std::deque<ViewFrame> frames;
    ViewReplica node{0, frames};
    auto candidate{node.Candidate()};
    RequireOk(node.agreement->Propose(candidate, node.authority), "offer");
    RequireOk(node.agreement->OnTimeout(0), "start timer");
    RequireOk(node.agreement->OnTimeout(500), "enter changing");
    BOOST_REQUIRE(node.agreement->ChangingView());
    const auto before{node.control_journal->ReadAll().value()};
    BOOST_REQUIRE_EQUAL(before.size(), 2U);
    node.RestartAgreement(0);
    frames.clear();
    RequireOk(node.agreement->OnTimeout(1'000'000), "restart retry report");
    RequireOk(node.agreement->OnTimeout(2'000'000), "one local report grants no changing timer");
    BOOST_CHECK_EQUAL(node.agreement->CurrentView(), 1U);
    BOOST_CHECK(node.agreement->ChangingView());
    const auto after{node.control_journal->ReadAll().value()};
    BOOST_REQUIRE_EQUAL(after.size(), before.size());
    for (size_t i{0}; i < before.size(); ++i) BOOST_CHECK(before[i].payload == after[i].payload);
    const auto signed_record{storage::SignedControlRecord::Decode(before.back().payload).value()};
    size_t reports{0};
    for (const auto& frame : frames) {
        const auto decoded{DecodeMessage(frame.bytes, node.profile).value()};
        if (const auto* report{std::get_if<ControlReport>(&decoded)}) {
            ++reports;
            BOOST_CHECK(report->signature == signed_record.signature);
            BOOST_CHECK_EQUAL(report->key.view, 1U);
        }
    }
    BOOST_CHECK(reports >= 3U);
    BOOST_CHECK_EQUAL(node.signer_journal->Status().appended_seq, 0U);
}

BOOST_AUTO_TEST_CASE(unsigned_new_view_intent_recovers_full_selected_body_before_first_signature)
{
    ViewNetwork network;
    auto funding{network.nodes[1]->Candidate()};
    RequireOk(network.nodes[1]->agreement->Propose(funding, network.nodes[1]->authority), "certified trusted funding first");
    network.Drain();
    auto candidate{network.nodes[0]->Candidate()};
    RequireOk(network.nodes[0]->agreement->Propose(candidate, network.nodes[0]->authority), "pending original work");
    network.Drain({2});
    for (uint32_t i : {0U, 1U, 3U}) RequireOk(network.nodes[i]->agreement->OnTimeout(0), "baseline");
    for (uint32_t i : {0U, 1U, 3U}) RequireOk(network.nodes[i]->agreement->OnTimeout(500), "next view");
    network.Drain({2});
    std::optional<ControlNewView> genuine;
    for (const auto& frame : network.history) {
        auto decoded{DecodeMessage(frame.bytes, network.nodes[0]->profile).value()};
        if (const auto* view{std::get_if<ControlNewView>(&decoded)}) { genuine = *view; break; }
    }
    BOOST_REQUIRE(genuine);
    BOOST_REQUIRE(genuine->selected_proposal);
    ViewNetwork restored_network;
    auto retained_funding{restored_network.nodes[1]->Candidate()};
    RequireOk(restored_network.nodes[1]->agreement->Propose(retained_funding, restored_network.nodes[1]->authority), "same certified trusted funding prefix");
    restored_network.Drain();
    auto& restored{*restored_network.nodes[3]};
    auto& frames{restored_network.frames};
    BOOST_REQUIRE(restored.owner->Head()->Header().Hash() == funding.Header().Hash());
    const auto own{std::find_if(genuine->reports.begin(), genuine->reports.end(), [](const auto& report) { return report.key.seat == 3; })};
    BOOST_REQUIRE(own != genuine->reports.end());
    auto pending_report{restored.control_log.AppendIntent(ControlIntent(*own, restored.profile).value())};
    RequireOk(pending_report, "atomic report intent");
    RequireOk(restored.control_log.Barrier(), "atomic changing entry");
    auto report_intent{restored.control_log.Confirm(*pending_report)};
    RequireOk(report_intent, "durable report intent");
    auto issued_report{restored.signer.SignControlAndPersist(*report_intent, restored.control_log)};
    RequireOk(issued_report, "report issued durably before constructing leader bundle");
    BOOST_CHECK(issued_report->Binding().signature == own->signature);
    RequireOk(restored.control_log.AppendIntent(ControlIntent(*genuine, restored.profile).value()), "durable unsigned complete new-view intent");
    RequireOk(restored.control_log.Barrier(), "leader new-view intent barrier");
    const auto before{restored.control_journal->ReadAll().value()};
    BOOST_REQUIRE_EQUAL(before.size(), 3U);
    restored.RestartAgreement(3);
    BOOST_CHECK(restored.agreement->ChangingView());
    // A late fourth report changes the first sorted quorum and advertises
    // another real preparation whose body this leader has not received.
    // It cannot strand or substitute the already durable complete NV A.
    SlotFixture actions{restored.profile};
    auto b{restored.Candidate({actions.Cancel("alice", 1, OrderId{LabelHash("late prepared B")})})};
    const auto prepare_digest{PbftPrepareDigest(b.Header(), 0)};
    std::vector<bls::Signature> shares;
    for (uint32_t i : {0U, 1U, 2U}) {
        const auto key{BlsKeyFromLabel(SeatLabel("s" + std::to_string(i)))};
        shares.push_back(key.Sign(std::span{prepare_digest.data(), prepare_digest.size()}));
    }
    SlotCertificate b_qc{b.Header().Hash(), SlotCertificate::MakeBitmap(4, std::vector<uint32_t>{0, 1, 2}).value(),
                         bls::AggregateSignatures(shares)->Compressed(), 0};
    ControlReport late;
    late.key = genuine->key; late.key.phase = storage::ControlPhase::VIEW_CHANGE; late.key.seat = 2;
    late.prepared_watermark = 4;
    late.prepared = ControlPrepared{0, b.Header(), b_qc};
    const auto late_digest{ControlIntent(late, restored.profile).value().Digest()};
    const auto late_key{BlsKeyFromLabel(SeatLabel("s2"))};
    late.signature = late_key.Sign(std::span{late_digest.data(), late_digest.size()}).Compressed();
    RequireOk(restored.agreement->OnMessage(2, EncodeMessage(Message{late}, restored.profile).value()), "late report cannot replace durable new-view intent");
    RequireOk(restored.agreement->OnTimeout(0), "resume without client or body resubmission");
    BOOST_CHECK_EQUAL(restored.agreement->CurrentView(), 1U);
    BOOST_CHECK(!restored.agreement->ChangingView());
    BOOST_CHECK(restored.signer_log.AcceptedAt({0, 2, 1}, restored.profile).value().has_value());
    BOOST_CHECK(!restored.commit_log.DecisionAt(2));
    size_t new_views{0}, proposals{0};
    for (const auto& frame : frames) {
        const auto decoded{DecodeMessage(frame.bytes, restored.profile).value()};
        if (std::holds_alternative<ControlNewView>(decoded)) ++new_views;
        if (const auto* proposal{std::get_if<Proposal>(&decoded)}) {
            ++proposals;
            BOOST_CHECK(proposal->header.Hash() == candidate.Header().Hash());
        }
    }
    BOOST_CHECK(new_views >= 3U);
    BOOST_CHECK(proposals >= 3U);
}

BOOST_AUTO_TEST_CASE(control_egress_requires_exact_durable_message_witness)
{
    class Mesh final : public net::SeatMesh {
    public:
        explicit Mesh(const net::MeshConfig& config) : SeatMesh{config} {}
        Result<std::optional<net::InboundFrame>> Poll() override { return Ok(std::optional<net::InboundFrame>{}); }
        size_t sent{0};
    protected:
        Result<void> Transmit(uint32_t, net::Lane lane, std::span<const unsigned char>) override
        {
            BOOST_CHECK(lane == net::Lane::CRITICAL); ++sent; return Ok();
        }
    };
    std::deque<ViewFrame> frames;
    ViewReplica node{0, frames};
    auto candidate{node.Candidate()};
    RequireOk(node.agreement->Propose(candidate, node.authority), "offer");
    RequireOk(node.agreement->OnTimeout(0), "baseline");
    RequireOk(node.agreement->OnTimeout(500), "durable report");
    std::optional<ControlReport> report;
    for (const auto& frame : frames) {
        const auto decoded{DecodeMessage(frame.bytes, node.profile).value()};
        if (const auto* item{std::get_if<ControlReport>(&decoded)}) { report = *item; break; }
    }
    BOOST_REQUIRE(report);
    const auto pending{node.control_log.SignedAt(report->key)};
    BOOST_REQUIRE(pending);
    auto durable{node.control_log.Confirm(*pending)};
    RequireOk(durable, "durable control signature token");
    const auto outbound{OutboundControl::From(*durable)};
    const auto bytes{EncodeMessage(Message{*report}, node.profile).value()};
    Mesh mesh{{0, 4, MaxMessageBytes(node.profile), node.profile}};
    RequireCode(mesh.Send(1, bytes), ErrorCode::INVALID_INPUT, "bare own control blocked");
    RequireCode(mesh.Send(1, net::Lane::CRITICAL, bytes), ErrorCode::INVALID_INPUT, "raw critical port blocked");
    RequireOk(mesh.SendDurableControl(1, outbound, bytes), "exact typed control frame");
    BOOST_CHECK_EQUAL(mesh.sent, 1U);
    report->prepared_watermark++;
    const auto substituted{EncodeMessage(Message{*report}, node.profile).value()};
    RequireCode(mesh.SendDurableControl(1, outbound, substituted), ErrorCode::VERIFY_FAILED, "durable token cannot authorize substituted body");
    Mesh wrong_seat{{1, 4, MaxMessageBytes(node.profile), node.profile}};
    RequireCode(wrong_seat.SendDurableControl(2, outbound, bytes), ErrorCode::VERIFY_FAILED, "foreign local signer capability");
    BOOST_CHECK_EQUAL(mesh.sent, 1U);
    BOOST_CHECK_EQUAL(wrong_seat.sent, 0U);
}

BOOST_AUTO_TEST_CASE(permanently_busy_peer_never_blocks_healthy_quorum_and_retry_cache_stays_bounded)
{
    ViewNetwork network;
    for (uint32_t i : {0U, 1U, 2U}) network.nodes[i]->sink.busy.insert(3);
    uint64_t now{0};
    for (uint64_t slot{1}; slot <= 30; ++slot) {
        auto candidate{network.nodes[0]->Candidate()};
        const auto expected{candidate.Header().Hash()};
        RequireOk(network.nodes[0]->agreement->Propose(candidate, network.nodes[0]->authority), "pending work despite unreachable peer");
        network.Drain({3});
        if (network.nodes[0]->owner->Head()->Header().height < slot) {
            // The unreachable peer is sometimes the scheduled leader. Its
            // replacement still needs only the three responsive operators.
            for (uint32_t i : {0U, 1U, 2U}) RequireOk(network.nodes[i]->agreement->OnTimeout(now), "leader loss timer");
            now += 500;
            for (uint32_t i : {0U, 1U, 2U}) RequireOk(network.nodes[i]->agreement->OnTimeout(now), "replace busy leader");
            network.Drain({3});
        }
        for (uint32_t i : {0U, 1U, 2U}) {
            BOOST_REQUIRE_EQUAL(network.nodes[i]->owner->Head()->Header().height, slot);
            BOOST_CHECK(network.nodes[i]->owner->Head()->Header().Hash() == expected);
        }
    }
    BOOST_CHECK_EQUAL(network.nodes[3]->owner->Head()->Header().height, 0U);
    network.frames.clear();
    for (uint32_t i : {0U, 1U, 2U}) {
        network.nodes[i]->sink.busy.clear();
        RequireOk(network.nodes[i]->agreement->OnTimeout(now), "release bounded per-peer delivery cache");
    }
    std::array<size_t, 4> released{};
    for (const auto& frame : network.frames) {
        BOOST_CHECK_EQUAL(frame.to, 3U);
        ++released[frame.from];
    }
    for (uint32_t i : {0U, 1U, 2U}) {
        BOOST_CHECK(released[i] > 0U);
        BOOST_CHECK(released[i] <= 16U);
    }
    // This test proves healthy quorum progress and bounded delivery only.
    // Thirty-slot historical catchup needs an authenticated receipt stream.
}

BOOST_AUTO_TEST_CASE(partial_control_rollback_refuses_retained_real_higher_view_votes_and_preparations)
{
    for (bool prepared : {false, true}) {
        ViewNetwork network;
        auto candidate{network.nodes[0]->Candidate()};
        RequireOk(network.nodes[0]->agreement->Propose(candidate, network.nodes[0]->authority), "offer with silent leader");
        network.Drain({1});
        for (uint32_t i : {0U, 2U, 3U}) RequireOk(network.nodes[i]->agreement->OnTimeout(0), "baseline");
        for (uint32_t i : {0U, 2U, 3U}) RequireOk(network.nodes[i]->agreement->OnTimeout(500), "enter real next view");
        network.Drain({1}, [&](const ViewFrame& frame) {
            const auto decoded{DecodeMessage(frame.bytes, network.nodes[0]->profile).value()};
            const auto* vote{std::get_if<Vote>(&decoded)};
            return !vote || (prepared && vote->phase == storage::VotePhase::PREPARE);
        });
        auto& source{*network.nodes[0]};
        BOOST_REQUIRE_EQUAL(source.owner->Head()->Header().height, 0U);
        BOOST_REQUIRE(source.signer_log.AcceptedAt({0, 1, 1}, source.profile).value());
        BOOST_REQUIRE(source.signer_log.SignedAt({TestDomain(), 0, 1, 1, storage::VotePhase::PREPARE}));
        BOOST_CHECK_EQUAL(source.commit_log.PreparedAt(0, 1, 1).has_value(), prepared);
        const auto full_view{source.control_log.AcceptedAt(1, 1)};
        BOOST_REQUIRE(full_view);
        auto report_key{full_view->key}; report_key.seat = 0; report_key.phase = storage::ControlPhase::VIEW_CHANGE;
        const auto original_report{source.control_log.IntentAt(report_key)};
        BOOST_REQUIRE(original_report);
        source.RestartAgreement(0); // the complete prefix is valid
        for (uint64_t missing : {0U, 1U, 2U}) {
            auto journal{NewMemoryJournal(storage::JournalRole::SIGNER, 800 + missing + (prepared ? 10 : 0))};
            auto rollback_result{storage::ControlLog::Create(*journal, source.control_log.Config())};
            RequireOk(rollback_result, "structurally valid partial control prefix");
            auto rollback{std::move(*rollback_result)};
            auto signer_result{storage::SeatSigner::FromIKM(AgreementRig::SeatIkm("s0"), source.signer_log, rollback)};
            RequireOk(signer_result, "same original private key bound to partial history");
            auto signer{std::move(*signer_result)};
            if (missing == 1) {
                RequireOk(rollback.AppendIntent(*original_report), "retain changing report but lose accepted bundle");
                RequireOk(rollback.Barrier(), "retained report prefix barrier");
            } else if (missing == 2) {
                RequireOk(rollback.AcceptNewView(*full_view), "retain accepted bundle but lose own atomic view entry");
                RequireOk(rollback.Barrier(), "retained accepted bundle barrier");
            }
            const auto before{source.signer_journal->Status()};
            PbftConfig config; config.self_seat = 0; config.regtest_full_view = true; config.view_timeout_floor_ms = 500;
            const PbftContext context{*source.adapter, *source.owner, signer, source.signer_log, source.commit_log,
                                      source.safety, source.sink, &rollback};
            RequireCode(PbftAgreement::Create(source.profile, config, context), ErrorCode::FENCED, "partial control rollback cannot resume signing");
            BOOST_CHECK(source.signer_journal->Status() == before);
            BOOST_CHECK_EQUAL(source.owner->Head()->Header().height, 0U);
        }
        // The accepted bundle alone must not grant the first new-view vote
        // when this replica has no ordinary history to reveal the omission.
        auto& fresh{*network.nodes[1]};
        BOOST_REQUIRE_EQUAL(fresh.signer_journal->Status().appended_seq, 0U);
        auto fresh_journal{NewMemoryJournal(storage::JournalRole::SIGNER, prepared ? 901 : 900)};
        auto fresh_control_result{storage::ControlLog::Create(*fresh_journal, fresh.control_log.Config())};
        RequireOk(fresh_control_result, "fresh structurally accepted-only control prefix");
        auto fresh_control{std::move(*fresh_control_result)};
        RequireOk(fresh_control.AcceptNewView(*full_view), "retain authenticated NV without own atomic report");
        RequireOk(fresh_control.Barrier(), "accepted-only prefix barrier");
        auto fresh_signer_result{storage::SeatSigner::FromIKM(AgreementRig::SeatIkm("s1"), fresh.signer_log, fresh_control)};
        RequireOk(fresh_signer_result, "fresh signer bound to accepted-only prefix");
        auto fresh_signer{std::move(*fresh_signer_result)};
        PbftConfig fresh_config; fresh_config.self_seat = 1; fresh_config.regtest_full_view = true; fresh_config.view_timeout_floor_ms = 500;
        const PbftContext fresh_context{*fresh.adapter, *fresh.owner, fresh_signer, fresh.signer_log, fresh.commit_log,
                                        fresh.safety, fresh.sink, &fresh_control};
        RequireCode(PbftAgreement::Create(fresh.profile, fresh_config, fresh_context), ErrorCode::FENCED, "accepted-only prefix cannot authorize the first ordinary vote");
        BOOST_CHECK_EQUAL(fresh.signer_journal->Status().appended_seq, 0U);
    }
}

BOOST_AUTO_TEST_CASE(changing_view_can_retain_real_preparation_without_local_new_view_acceptance)
{
    ViewNetwork network;
    auto candidate{network.nodes[0]->Candidate()};
    RequireOk(network.nodes[0]->agreement->Propose(candidate, network.nodes[0]->authority), "offer");
    network.Drain({1});
    for (uint32_t i : {0U, 2U, 3U}) RequireOk(network.nodes[i]->agreement->OnTimeout(0), "baseline");
    for (uint32_t i : {0U, 2U, 3U}) RequireOk(network.nodes[i]->agreement->OnTimeout(500), "real new view");
    network.Drain({1}, [&](const ViewFrame& frame) {
        const auto decoded{DecodeMessage(frame.bytes, network.nodes[0]->profile).value()};
        const auto* vote{std::get_if<Vote>(&decoded)};
        return !vote || vote->phase == storage::VotePhase::PREPARE;
    });
    auto& lagging{*network.nodes[1]};
    ControlReport report;
    report.key = {TestDomain(), ProfileHash(lagging.profile), lagging.seats.Root(), 0, 1,
                  lagging.owner->Head()->Header().Hash(), 1, 1, storage::ControlPhase::VIEW_CHANGE};
    report.prepared_watermark = lagging.commit_log.DurableSeq();
    const auto transcript{ControlIntent(report, lagging.profile).value()};
    auto pending{lagging.control_log.AppendIntent(transcript)};
    RequireOk(pending, "durable changing entry without accepted NV");
    RequireOk(lagging.control_log.Barrier(), "changing entry barrier");
    auto intent{lagging.control_log.Confirm(*pending)};
    RequireOk(intent, "changing intent token");
    RequireOk(lagging.signer.SignControlAndPersist(*intent, lagging.control_log), "real local report issued");
    lagging.RestartAgreement(1);
    const Proposal offered{0, 1, candidate.Header(), std::nullopt, candidate.Body()};
    RequireOk(lagging.agreement->OnMessage(0, EncodeMessage(Message{OfferedProposal{offered}}, lagging.profile).value()), "body without accepted proposal");
    std::set<uint32_t> seen;
    for (const auto& frame : network.history) {
        const auto decoded{DecodeMessage(frame.bytes, lagging.profile).value()};
        const auto* vote{std::get_if<Vote>(&decoded)};
        if (vote && vote->view == 1 && vote->phase == storage::VotePhase::PREPARE && seen.insert(vote->seat).second) {
            RequireOk(lagging.agreement->OnMessage(vote->seat, frame.bytes), "learn real target-view QC while changing");
        }
    }
    BOOST_REQUIRE_EQUAL(seen.size(), 3U);
    BOOST_REQUIRE(lagging.commit_log.PreparedAt(0, 1, 1));
    BOOST_CHECK(!lagging.control_log.AcceptedAt(1, 1));
    BOOST_CHECK_EQUAL(lagging.signer_journal->Status().appended_seq, 0U);
    lagging.RestartAgreement(1);
    BOOST_CHECK(lagging.agreement->ChangingView());
    BOOST_CHECK_EQUAL(lagging.agreement->CurrentView(), 1U);
    BOOST_CHECK(!lagging.agreement->TakeDecision());
}

namespace {
template <class T> void SignObservationControl(T& message, const ViewReplica& node)
{
    const auto digest{ControlIntent(message, node.profile).value().Digest()};
    const auto key{BlsKeyFromLabel(SeatLabel("s" + std::to_string(message.key.seat)))};
    message.signature = key.Sign(std::span{digest.data(), digest.size()}).Compressed();
}
ControlReport ObservationReport(const ViewReplica& node, uint32_t seat, uint64_t view,
                                std::optional<ControlPrepared> prepared = std::nullopt)
{
    ControlReport report{{TestDomain(), ProfileHash(node.profile), node.seats.Root(), 0, 1,
        node.genesis.Header().Hash(), seat, view, storage::ControlPhase::VIEW_CHANGE}, 0, prepared, std::nullopt, {}};
    SignObservationControl(report, node); return report;
}
SlotCertificate ObservationCertificate(const ViewReplica& node, const SlotHeaderV2& header, uint64_t view, bool commit)
{
    const auto digest{commit ? VersionedCommitDigest(header, view) : PbftPrepareDigest(header, view)};
    std::vector<bls::Signature> signatures;
    for (uint32_t seat : {0U, 1U, 2U}) signatures.push_back(BlsKeyFromLabel(SeatLabel("s" + std::to_string(seat))).Sign(std::span{digest.data(), digest.size()}));
    return {header.Hash(), SlotCertificate::MakeBitmap(4, std::array<uint32_t, 3>{0, 1, 2}).value(),
        bls::AggregateSignatures(signatures)->Compressed(), view};
}
void FeedObservationReport(ViewReplica& node, const ControlReport& report)
{
    RequireOk(node.agreement->OnMessage(report.key.seat, EncodeMessage(Message{report}, node.profile).value()), "actual controller receives authenticated quorum report");
}
void DrainObservationBusy(ViewReplica& node)
{
    for (int i{0}; i < 20; ++i) RequireOk(node.adapter->Poll(node.owner->Head()->Header().facts_to, MAX_SLOT_FACTS), "independent source becomes available");
}
Vote ObservationVote(const ViewReplica& node, const SlotHeaderV2& header, uint32_t seat, uint64_t view, storage::VotePhase phase)
{
    Vote vote{phase, 0, view, header.height, header.Hash(), seat, {}, {}};
    const auto digest{phase == storage::VotePhase::PREPARE ? PbftPrepareDigest(header, view) : VersionedCommitDigest(header, view)};
    vote.signature = BlsKeyFromLabel(SeatLabel("s" + std::to_string(seat))).Sign(std::span{digest.data(), digest.size()}).Compressed();
    return vote;
}
Result<void> FeedObservationVote(ViewReplica& node, const Vote& vote)
{
    return node.agreement->OnMessage(vote.seat, EncodeMessage(Message{vote}, node.profile).value());
}
}

BOOST_AUTO_TEST_CASE(authenticated_far_view_with_known_decision_fences_before_source_availability)
{
    std::deque<ViewFrame> frames; ViewReplica node{0, frames};
    auto candidate{node.Candidate()}; node.adapter->QueueBusy(20);
    auto first{ObservationReport(node, 1, 17)};
    first.decision = CertifiedHeader{candidate.Header(), ObservationCertificate(node, candidate.Header(), 0, true)};
    SignObservationControl(first, node);
    ControlNewView view{{TestDomain(), ProfileHash(node.profile), node.seats.Root(), 0, 1, node.genesis.Header().Hash(),
        2, 17, storage::ControlPhase::NEW_VIEW}, candidate.Header().Hash(),
        {first, ObservationReport(node, 2, 17), ObservationReport(node, 3, 17)}, {}};
    view.selected_proposal = Proposal{0, 17, candidate.Header(), std::nullopt, candidate.Body()};
    SignObservationControl(view, node);
    RequireOk(node.agreement->OnMessage(2, EncodeMessage(Message{view}, node.profile).value()), "full quorum proof may bridge more than the report discovery window");
    BOOST_CHECK_EQUAL(node.agreement->CurrentView(), 17U);
    const auto control{node.control_log.ViewAt(1)}; BOOST_REQUIRE(control);
    BOOST_CHECK_EQUAL(control->view, 17U); BOOST_CHECK(control->changing);
    BOOST_CHECK(!node.control_log.AllowsVote(TestDomain(), 0, 1, 0));
    BOOST_CHECK_EQUAL(node.signer_journal->DurableSeq(), 0U);
    BOOST_CHECK(!node.agreement->TakeDecision());
    DrainObservationBusy(node);
    RequireOk(node.agreement->OnMessage(2, EncodeMessage(Message{view}, node.profile).value()), "same exact certified body resumes after independent source availability");
    node.ApplyDecision();
    BOOST_CHECK_EQUAL(node.owner->Head()->Header().height, 1U);
    BOOST_CHECK(node.owner->Head()->Header().Hash() == candidate.Header().Hash());
}

BOOST_AUTO_TEST_CASE(own_quorum_observation_export_is_pure_and_resumes_exact_durable_leader_transcript)
{
    std::deque<ViewFrame> frames; ViewReplica node{2, frames};
    auto candidate{node.Candidate()};
    RequireOk(node.agreement->Propose(candidate, node.authority), "cache independently valid initial offer");
    node.adapter->QueueBusy(20);
    FeedObservationReport(node, ObservationReport(node, 0, 1));
    FeedObservationReport(node, ObservationReport(node, 1, 1));
    BOOST_REQUIRE(node.agreement->ChangingView());
    const auto before{node.control_journal->Status()}; const auto votes_before{node.signer_journal->Status()};
    const auto pending{node.agreement->PendingObservationNewView()}; RequireOk(pending, "pure own leader availability port");
    BOOST_REQUIRE(*pending); const auto proof{**pending};
    RequireOk(VerifyControlNewViewEvidence(proof, node.genesis.Header(), node.seats, node.profile), "real report quorum and canonical selection");
    BOOST_CHECK(!VerifyControlNewView(proof, node.genesis.Header(), node.seats, node.profile));
    BOOST_CHECK(node.control_journal->Status() == before); BOOST_CHECK(node.signer_journal->Status() == votes_before);
    auto weak{proof}; weak.reports.pop_back();
    BOOST_CHECK(!node.agreement->ResumeObservationNewView(weak));
    BOOST_CHECK(!node.agreement->ResumeObservationNewView(proof)); // Source still independently unavailable.
    BOOST_CHECK(node.control_journal->Status() == before); BOOST_CHECK(node.signer_journal->Status() == votes_before);
    DrainObservationBusy(node);
    RequireOk(node.agreement->ResumeObservationNewView(proof), "normal intent/sign/barriers after source validation");
    const auto accepted{node.control_log.AcceptedAt(1, 1)}; BOOST_REQUIRE(accepted);
    const auto decoded{DecodeMessage(accepted->message, node.profile).value()};
    const auto* actual{std::get_if<ControlNewView>(&decoded)}; BOOST_REQUIRE(actual);
    RequireOk(VerifyControlNewView(*actual, node.genesis.Header(), node.seats, node.profile), "real durable leader signature");
    BOOST_CHECK(ControlIntent(*actual, node.profile).value() == ControlIntent(proof, node.profile).value());
    BOOST_CHECK(node.signer_journal->DurableSeq() > votes_before.durable_seq);
    BOOST_CHECK(!node.agreement->ChangingView());
    const auto no_longer_pending{node.agreement->PendingObservationNewView()}; RequireOk(no_longer_pending, "active leader has no unsigned thaw capability");
    BOOST_CHECK(!*no_longer_pending);
}

BOOST_AUTO_TEST_CASE(foreign_highest_prepared_source_waits_without_signing_for_cached_or_offered_body)
{
    for (const bool cached : {false, true}) {
        std::deque<ViewFrame> frames; ViewReplica node{2, frames};
        auto a{node.Candidate()};
        if (cached) RequireOk(node.agreement->Propose(a, node.authority), "previously cache independently valid source A");
        auto deposit{DepositAtCursor({131, 0}, "additional-source-deposit", 20'000)}; deposit.account = KeyAccount("bob");
        RequireOk(node.adapter->AppendBlock(TestAnchor(131).hash, {Fact{deposit}}), "current observer sees a longer source B");
        auto b{node.Candidate()}; RequireOk(node.agreement->Propose(b, node.authority), "current source B offer");
        const ControlPrepared prepared{0, a.Header(), ObservationCertificate(node, a.Header(), 0, false)};
        FeedObservationReport(node, ObservationReport(node, 0, 1, prepared));
        FeedObservationReport(node, ObservationReport(node, 1, 1, prepared));
        BOOST_REQUIRE(node.agreement->ChangingView());
        BOOST_CHECK(!node.control_log.AcceptedAt(1, 1)); BOOST_CHECK_EQUAL(node.signer_journal->DurableSeq(), 0U);
        const Proposal wrong{0, 0, b.Header(), std::nullopt, b.Body()};
        const auto without_a{node.agreement->PendingObservationNewView(wrong)}; RequireOk(without_a, "wrong offered body cannot replace highest prepared A");
        if (*without_a) BOOST_CHECK((**without_a).selected == a.Header().Hash());
        else BOOST_CHECK(!cached);
        const Proposal available{0, 0, a.Header(), std::nullopt, a.Body()};
        const auto ready{node.agreement->PendingObservationNewView(available)}; RequireOk(ready, "bounded authenticated body availability may carry highest prepared A");
        BOOST_REQUIRE(*ready); BOOST_CHECK((**ready).selected == a.Header().Hash());
        const auto before{node.control_journal->Status()};
        const auto refused{node.agreement->ResumeObservationNewView(**ready)};
        BOOST_REQUIRE(!refused); BOOST_CHECK(refused.error().code == ErrorCode::VERIFY_FAILED);
        BOOST_CHECK(node.control_journal->Status() == before); BOOST_CHECK_EQUAL(node.signer_journal->DurableSeq(), 0U);
    }
}

BOOST_AUTO_TEST_CASE(remote_new_view_after_long_idle_gets_its_full_active_timeout)
{
    std::deque<ViewFrame> frames; ViewReplica node{0, frames};
    auto candidate{node.Candidate()};
    RequireOk(node.agreement->OnTimeout(0), "observe an idle clock without active work");
    ControlNewView view{{TestDomain(), ProfileHash(node.profile), node.seats.Root(), 0, 1,
        node.genesis.Header().Hash(), 2, 1, storage::ControlPhase::NEW_VIEW}, candidate.Header().Hash(),
        {ObservationReport(node, 1, 1), ObservationReport(node, 2, 1), ObservationReport(node, 3, 1)}, {}};
    view.selected_proposal = Proposal{0, 1, candidate.Header(), std::nullopt, candidate.Body()};
    SignObservationControl(view, node);
    RequireOk(node.agreement->OnMessage(2, EncodeMessage(Message{view}, node.profile).value()), "first full authenticated NEW_VIEW after idle");
    BOOST_CHECK_EQUAL(node.agreement->CurrentView(), 1U);
    BOOST_CHECK(!node.agreement->ChangingView());
    RequireOk(node.agreement->OnTimeout(10'000), "first actual tick arms the newly received view");
    BOOST_CHECK_EQUAL(node.agreement->CurrentView(), 1U);
    BOOST_CHECK(!node.agreement->ChangingView());
    RequireOk(node.agreement->OnTimeout(10'999), "full view-one timeout has not elapsed");
    BOOST_CHECK_EQUAL(node.agreement->CurrentView(), 1U);
    RequireOk(node.agreement->OnTimeout(11'000), "ordinary timeout remains bounded from the fresh tick");
    BOOST_CHECK_EQUAL(node.agreement->CurrentView(), 2U);
    BOOST_CHECK(node.agreement->ChangingView());
}

BOOST_AUTO_TEST_CASE(pure_crypto_memo_never_accepts_changed_signature_phase_seat_view_bitmap_or_domain)
{
    std::deque<ViewFrame> frames; ViewReplica node{0, frames};
    auto candidate{node.Candidate()};
    Proposal proposal{0, 0, candidate.Header(), std::nullopt, candidate.Body()};
    RequireOk(node.agreement->OnMessage(1, EncodeMessage(Message{proposal}, node.profile).value()), "real leader proposal and durable own prepare");
    const auto prepare{ObservationVote(node, candidate.Header(), 1, 0, storage::VotePhase::PREPARE)};
    RequireOk(FeedObservationVote(node, prepare), "verify and retain a peer's exact prepare");
    RequireOk(FeedObservationVote(node, prepare), "exact verified vote retransmission remains valid");
    for (unsigned attack{0}; attack < 3; ++attack) {
        auto changed{prepare};
        if (attack == 0) changed.signature[20] ^= 1;
        if (attack == 1) changed.phase = storage::VotePhase::COMMIT;
        if (attack == 2) changed.seat = 2;
        const auto before{node.signer_journal->Status()};
        for (unsigned retry{0}; retry < 2; ++retry) {
            const auto refused{FeedObservationVote(node, changed)};
            BOOST_REQUIRE(!refused); BOOST_CHECK(refused.error().code == ErrorCode::VERIFY_FAILED);
        }
        BOOST_CHECK(node.signer_journal->Status() == before);
    }
    RequireOk(FeedObservationVote(node, ObservationVote(node, candidate.Header(), 2, 0, storage::VotePhase::PREPARE)), "real q3 preparation verified and cached");
    const auto prepared{node.commit_log.PreparedAt(0, 1, 0)}; BOOST_REQUIRE(prepared);
    const auto preparation{SlotCertificate::Decode(prepared->qc).value()};
    // Exactly the same bytes cannot cross from PREPARE to COMMIT authority.
    for (unsigned retry{0}; retry < 2; ++retry) {
        const auto refused{node.agreement->OnMessage(1, EncodeMessage(Message{CertifiedProposal{proposal, preparation}}, node.profile).value())};
        BOOST_REQUIRE(!refused); BOOST_CHECK(refused.error().code == ErrorCode::VERIFY_FAILED);
    }
    const auto certificate{ObservationCertificate(node, candidate.Header(), 0, true)};
    RequireOk(node.agreement->OnMessage(1, EncodeMessage(Message{CertifiedProposal{proposal, certificate}}, node.profile).value()), "new genuine commit signature still fully verifies");
    for (unsigned attack{0}; attack < 4; ++attack) {
        auto changed{certificate}; auto other{proposal};
        if (attack == 0) changed.aggregate[20] ^= 1;
        if (attack == 1) changed.bitmap = SlotCertificate::MakeBitmap(4, std::array<uint32_t, 3>{0, 1, 3}).value();
        if (attack == 2) { changed.view = 1; other.view = 1; }
        if (attack == 3) { other.header.domain = LabelHash("another certified domain"); changed.header_hash = other.header.Hash(); }
        for (unsigned retry{0}; retry < 2; ++retry) {
            const auto refused{node.agreement->OnMessage(1, EncodeMessage(Message{CertifiedProposal{other, changed}}, node.profile).value())};
            BOOST_REQUIRE(!refused); BOOST_CHECK(refused.error().code == ErrorCode::VERIFY_FAILED);
        }
    }
    node.ApplyDecision();
    BOOST_CHECK(node.owner->Head()->HeaderHash() == candidate.Header().Hash());
    // A new parent uses fresh immutable context and still verifies its real
    // retained parent certificate when the controller refreshes its caches.
    auto next{node.Candidate()};
    RequireOk(node.agreement->Propose(next, node.authority), "new-parent refresh keeps genuine cryptographic authority valid");
}

BOOST_AUTO_TEST_CASE(pure_crypto_memo_fifo_eviction_falls_back_to_verification_without_refusing_valid_evidence)
{
    std::deque<ViewFrame> frames; ViewReplica node{0, frames};
    auto candidate{node.Candidate()};
    ControlNewView view{{TestDomain(), ProfileHash(node.profile), node.seats.Root(), 0, 1,
        node.genesis.Header().Hash(), 3, 66, storage::ControlPhase::NEW_VIEW}, candidate.Header().Hash(),
        {ObservationReport(node, 1, 66), ObservationReport(node, 2, 66), ObservationReport(node, 3, 66)}, {}};
    view.selected_proposal = Proposal{0, 66, candidate.Header(), std::nullopt, candidate.Body()};
    SignObservationControl(view, node);
    RequireOk(node.agreement->OnMessage(3, EncodeMessage(Message{view}, node.profile).value()), "authenticated current view without fresh ordinary proposal");
    // Late valid evidence for the same value is legitimate. Sixty-five real
    // prepared QCs and260 distinct valid shares exceed both bounded caches.
    for (uint64_t old{0}; old < 65; ++old) {
        for (uint32_t seat : {1U, 2U, 3U}) RequireOk(FeedObservationVote(node,
            ObservationVote(node, candidate.Header(), seat, old, storage::VotePhase::PREPARE)), "new old-view share takes the full verification path");
        RequireOk(FeedObservationVote(node, ObservationVote(node, candidate.Header(), 1, old, storage::VotePhase::COMMIT)), "distinct commit digest is independently verified");
    }
    BOOST_REQUIRE(node.commit_log.PreparedAt(0, 1, 64));
    const auto before{node.commit_journal->Status()};
    RequireOk(FeedObservationVote(node, ObservationVote(node, candidate.Header(), 1, 0, storage::VotePhase::PREPARE)), "evicted valid share and QC are verified again, never refused");
    BOOST_CHECK(node.commit_journal->Status() == before);
    auto changed{ObservationVote(node, candidate.Header(), 1, 0, storage::VotePhase::PREPARE)}; changed.view = 65;
    const auto refused{FeedObservationVote(node, changed)};
    BOOST_REQUIRE(!refused); BOOST_CHECK(refused.error().code == ErrorCode::VERIFY_FAILED);
    RequireOk(FeedObservationVote(node, ObservationVote(node, candidate.Header(), 1, 65, storage::VotePhase::PREPARE)), "new genuine view signature remains admissible after eviction");
    BOOST_CHECK_EQUAL(node.signer_journal->DurableSeq(), 0U);
    BOOST_CHECK_EQUAL(node.agreement->CurrentView(), 66U);
    BOOST_CHECK(!node.agreement->TakeDecision());
}

BOOST_AUTO_TEST_SUITE_END()
