// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** Agreement interfaces and the PBFT-side stubs (slice R10): the shapes of
 * Decision, Agreement, SafetyRules and MessageSink, and the typed stubs of
 * PBFT safety (M4-AGR-01), PBFT agreement (M4-AGR-02), the leader schedule
 * (M4-AGR-03), inclusion lists (M4-AGR-04), the restart fence (M4-AGR-06)
 * and the evidence store (M4-AGR-07). Each stub is also in stub_inventory.
 */

#include <boost/test/unit_test.hpp>

#include <flowmesh2/agreement/agreement.h>
#include <flowmesh2/agreement/evidence.h>
#include <flowmesh2/agreement/inclusion.h>
#include <flowmesh2/agreement/leader.h>
#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/agreement/pbft.h>
#include <flowmesh2/agreement/restart_fence.h>
#include <flowmesh2/agreement/safety.h>
#include <flowmesh2/test/util/agreement_fixtures.h>

#include <array>
#include <deque>
#include <set>
#include <util/chaintype.h>
#include <type_traits>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::agreement;
using namespace flowmesh2::test;

namespace {

//! A sink that refuses everything; PbftAgreement::Create never sends.
class NullSink final : public MessageSink
{
public:
    Result<void> Send(uint32_t, std::span<const unsigned char>) override { return Error{ErrorCode::BUSY, "test sink"}; }
};

} // namespace

// Decision carries a move-only durability token, so it is move-only too.
static_assert(!std::is_copy_constructible_v<Decision>);
static_assert(std::is_nothrow_move_constructible_v<Decision>);
static_assert(std::is_abstract_v<Agreement>);
static_assert(std::is_abstract_v<SafetyRules>);
static_assert(std::is_abstract_v<MessageSink>);
static_assert(std::is_base_of_v<SafetyRules, PbftSafetyRules>);
static_assert(!std::is_abstract_v<PbftSafetyRules>);
// PBFT agreement, inclusion lists and the restart fence have no instances in the outline.
static_assert(!std::is_default_constructible_v<PbftAgreement>);
static_assert(!std::is_default_constructible_v<InclusionList>);
static_assert(!std::is_default_constructible_v<RestartFence>);

BOOST_AUTO_TEST_SUITE(agreement_tests)

BOOST_AUTO_TEST_CASE(provisional_constants_pinned)
{
    BOOST_CHECK_EQUAL(TERM_SLOTS, 8U);
    BOOST_CHECK_EQUAL(INCLUSION_AGE_SLOTS, 2U);
    BOOST_CHECK_EQUAL(PbftConfig{}.view_timeout_floor_ms, 50U);
    BOOST_CHECK_EQUAL(LeaderSchedule{7}.SeatCount(), 7U);
}

BOOST_AUTO_TEST_CASE(pbft_safety_rules_are_stubs)
{
    PbftSafetyRules rules;
    SafetyRules& as_interface{rules};
    const Proposal proposal{};
    RequireStub(as_interface.MayPrepare(proposal), "M4-AGR-01");
    RequireStub(as_interface.MayCommit(proposal, PreparedCert{}), "M4-AGR-01");
    RequireStub(as_interface.OnViewChange(ViewChange{}), "M4-AGR-01");
}

BOOST_AUTO_TEST_CASE(pbft_agreement_is_a_stub)
{
    AgreementRig rig;
    PbftSafetyRules rules;
    NullSink sink;
    const PbftContext context{*rig.adapter, *rig.owner, rig.signer, rig.signer_log, rig.commit_log, rules, sink};
    RequireStub(PbftAgreement::Create(rig.profile, PbftConfig{}, context), "M4-AGR-02");
    // Nothing was written.
    BOOST_CHECK(rig.recorder.events.empty());
}

BOOST_AUTO_TEST_CASE(leader_inclusion_fence_and_evidence_are_stubs)
{
    const SeatSet seats{TestSeats({"s0", "s1", "s2", "s3"})};
    const LeaderSchedule schedule{seats.Size()};
    RequireStub(schedule.LeaderFor(SeatAuthority(seats), 3, SlowSet{{1}}), "M4-AGR-03");

    const std::vector<HeldAction> held{HeldAction{ShortId{1, 2, 3, 4, 5, 6}, 1}};
    RequireStub(InclusionList::Build(held, 9), "M4-AGR-04");
    const std::vector<Vote> votes{Vote{}};
    const std::vector<ShortId> ids{ShortId{1, 2, 3, 4, 5, 6}};
    RequireStub(InclusionList::Check(votes, seats.Size(), ids, {}), "M4-AGR-04");

    const std::vector<HighWaterStatement> statements{HighWaterStatement{1, 0, 0, 5, 0, {}}};
    RequireStub(RestartFence::Collect(statements, seats, 0), "M4-AGR-06");

    EvidenceStore store;
    const Evidence evidence{DoubleVote{}};
    RequireStub(store.Record(evidence), "M4-AGR-07");
    RequireStub(store.Load(), "M4-AGR-07");
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

struct PbftFrame {
    uint32_t from;
    uint32_t to;
    std::vector<unsigned char> bytes;
};

class QueueSink final : public MessageSink
{
public:
    QueueSink(uint32_t self, std::deque<PbftFrame>& frames) : m_self{self}, m_frames{frames} {}
    Result<void> Send(uint32_t seat, std::span<const unsigned char> bytes) override
    {
        m_frames.push_back(PbftFrame{m_self, seat, {bytes.begin(), bytes.end()}});
        return Ok();
    }
private:
    uint32_t m_self;
    std::deque<PbftFrame>& m_frames;
};

struct PbftReplica {
    ProfileParams profile{StableQRegtestProfile(ChainType::REGTEST).value()};
    SeatSet seats{TestSeats({"s0", "s1", "s2", "s3"})};
    AuthorityVersion authority{0, seats.Root(), ProfileHash(profile)};
    std::unique_ptr<dev::FakeChainAdapter> adapter{dev::FakeChainAdapter::Create(TestNetwork(), authority, seats, TestAnchor(129)).value()};
    RecordingObserver observer;
    std::unique_ptr<storage::MemoryJournal> signer_journal;
    std::unique_ptr<storage::MemoryJournal> commit_journal;
    storage::SignerLog signer_log;
    storage::CommitLog commit_log;
    storage::SeatSigner signer;
    state::HeadView genesis{state::GenesisHead(profile, std::vector<MarketDescriptor>{MarketM1(), MarketM2()}, TestDomain(), authority, TestAnchor(129), {130, 0}).value()};
    std::unique_ptr<state::StateOwner> owner;
    PbftSafetyRules safety;
    QueueSink sink;
    std::unique_ptr<Agreement> agreement;

    PbftReplica(uint32_t self, std::deque<PbftFrame>& frames, storage::MemoryJournalFaults faults = {})
        : signer_journal{NewMemoryJournal(storage::JournalRole::SIGNER, 10 + self, faults, &observer)},
          commit_journal{NewMemoryJournal(storage::JournalRole::COMMIT, 20 + self, {}, &observer)},
          signer_log{storage::SignerLog::Create(*signer_journal).value()},
          commit_log{storage::CommitLog::Create(*commit_journal).value()},
          signer{storage::SeatSigner::FromIKM(AgreementRig::SeatIkm("s" + std::to_string(self)), signer_log).value()},
          owner{state::StateOwner::Create(profile, genesis, commit_journal->InstanceId()).value()}, sink{self, frames}
    {
        RequireOk(adapter->AppendBlock(TestAnchor(130).hash, {Fact{DepositAtCursor({130, 0}, "pbft-deposit", 10'000)}}), "chain fact");
        PbftConfig config;
        config.self_seat = self;
        config.regtest_fixed_view = true;
        config.view_timeout_floor_ms = 500;
        const PbftContext context{*adapter, *owner, signer, signer_log, commit_log, safety, sink};
        auto created{PbftAgreement::Create(profile, config, context)};
        RequireOk(created, "fixed view PBFT factory");
        agreement = std::move(*created);
    }

    state::Candidate Candidate() const
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
        auto result{owner->Preview(state::ParentRef::Committed(*owner->Head()), inputs)};
        RequireOk(result, "candidate");
        return std::move(*result);
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

struct PbftNetwork {
    std::deque<PbftFrame> frames;
    std::array<std::unique_ptr<PbftReplica>, 4> nodes;
    PbftNetwork()
    {
        for (uint32_t i{0}; i < 4; ++i) nodes[i] = std::make_unique<PbftReplica>(i, frames);
    }
    void Drain(const std::set<uint32_t>& offline = {})
    {
        size_t work{0};
        while (!frames.empty()) {
            BOOST_REQUIRE(++work < 1'000);
            PbftFrame frame{std::move(frames.front())};
            frames.pop_front();
            if (offline.contains(frame.from) || offline.contains(frame.to)) continue;
            RequireOk(nodes[frame.to]->agreement->OnMessage(frame.from, frame.bytes), "receive authenticated message");
            nodes[frame.to]->ApplyDecision();
        }
        for (auto& node : nodes) node->ApplyDecision();
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(pbft_fixed_view_tests)

BOOST_AUTO_TEST_CASE(real_four_seat_two_phase_quorum_with_one_offline)
{
    PbftNetwork network;
    auto candidate{network.nodes[1]->Candidate()}; // (sequence 1 + view 0) % 4
    const auto expected{candidate.Header().Hash()};
    RequireOk(network.nodes[1]->agreement->Propose(candidate, network.nodes[1]->authority), "leader propose");
    network.Drain({3});
    for (uint32_t i : {0U, 1U, 2U}) {
        const auto& node{*network.nodes[i]};
        BOOST_CHECK(node.owner->Head()->Header().Hash() == expected);
        BOOST_CHECK_EQUAL(node.owner->Head()->Header().height, 1U);
        auto records{node.commit_journal->ReadAll().value()};
        BOOST_REQUIRE(records.size() >= 3);
        BOOST_CHECK(records.front().type == storage::RecordType::PREPARED_QC);
        BOOST_CHECK(records[1].type == storage::RecordType::DECISION);
        BOOST_CHECK(records.back().type == storage::RecordType::APPLIED);
        auto recovered{storage::CommitLog::Create(*node.commit_journal, node.profile)};
        RequireOk(recovered, "prepared proof recovery");
        BOOST_CHECK(recovered->PreparedAt(0, 1, 0).has_value());
        BOOST_CHECK(!network.nodes[i]->agreement->TakeDecision());
        const Proposal old{0, 0, candidate.Header(), std::nullopt, candidate.Body()};
        RequireOk(network.nodes[i]->agreement->OnMessage(1, EncodeMessage(Message{old}, node.profile).value()), "duplicate decided proposal");
        const auto final{storage::DecisionRecord::Decode(records[1].payload, node.profile).value()};
        const CertifiedProposal proof{old, final.certificate};
        RequireOk(network.nodes[i]->agreement->OnMessage(2, EncodeMessage(Message{proof}, node.profile).value()), "relayed duplicate decision");
        BOOST_CHECK(!network.nodes[i]->agreement->TakeDecision());
        BOOST_CHECK_EQUAL(node.owner->Head()->Header().height, 1U);
    }
    BOOST_CHECK_EQUAL(network.nodes[3]->owner->Head()->Header().height, 0U);
}

BOOST_AUTO_TEST_CASE(two_seats_do_not_prepare_or_decide_and_timeout_fences)
{
    PbftNetwork network;
    auto candidate{network.nodes[1]->Candidate()};
    RequireOk(network.nodes[1]->agreement->Propose(candidate, network.nodes[1]->authority), "leader propose");
    network.Drain({2, 3});
    for (uint32_t i : {0U, 1U}) {
        auto& node{*network.nodes[i]};
        BOOST_CHECK_EQUAL(node.owner->Head()->Header().height, 0U);
        BOOST_CHECK(!node.commit_log.PreparedAt(0, 1, 0));
        const auto records{node.signer_journal->ReadAll()};
        BOOST_REQUIRE(records.has_value());
        for (const auto& record : *records) {
            if (record.type == storage::RecordType::SIGNED_VOTE) {
                auto vote{storage::SignedVoteRecord::Decode(record.payload).value()};
                BOOST_CHECK(vote.key.phase == storage::VotePhase::PREPARE);
            }
        }
        RequireOk(node.agreement->OnTimeout(100), "start timer");
        RequireCode(node.agreement->OnTimeout(600), ErrorCode::FENCED, "no unsafe view switch");
        RequireCode(node.agreement->OnTimeout(601), ErrorCode::FENCED, "sticky timeout fence");
    }
}

BOOST_AUTO_TEST_CASE(forged_phase_replay_session_and_missing_facts_refused)
{
    PbftNetwork network;
    auto candidate{network.nodes[1]->Candidate()};
    RequireOk(network.nodes[1]->agreement->Propose(candidate, network.nodes[1]->authority), "leader propose");
    auto proposal{EncodeMessage(Message{Proposal{0, 0, candidate.Header(), std::nullopt, candidate.Body()}}, network.nodes[0]->profile).value()};
    RequireCode(network.nodes[0]->agreement->OnMessage(2, proposal), ErrorCode::VERIFY_FAILED, "wrong leader session");
    RequireOk(network.nodes[0]->agreement->OnMessage(1, proposal), "right leader session");
    Vote fake{storage::VotePhase::COMMIT, 0, 0, 1, candidate.Header().Hash(), 2, {}, {}};
    auto bytes{EncodeMessage(Message{fake}, network.nodes[0]->profile).value()};
    RequireCode(network.nodes[0]->agreement->OnMessage(3, bytes), ErrorCode::VERIFY_FAILED, "spoofed session");
    RequireCode(network.nodes[0]->agreement->OnMessage(2, bytes), ErrorCode::VERIFY_FAILED, "forged BLS share");
    auto records{network.nodes[0]->signer_journal->ReadAll().value()};
    for (const auto& record : records) {
        if (record.type == storage::RecordType::SIGNED_VOTE) {
            auto signed_vote{storage::SignedVoteRecord::Decode(record.payload).value()};
            fake.seat = 0;
            fake.signature = signed_vote.signature; // a PREPARE cannot be used as COMMIT
        }
    }
    bytes = EncodeMessage(Message{fake}, network.nodes[0]->profile).value();
    RequireCode(network.nodes[0]->agreement->OnMessage(0, bytes), ErrorCode::VERIFY_FAILED, "prepare as commit");
    BOOST_CHECK(!network.nodes[0]->commit_log.DecisionAt(1));
    // An economically valid reexecution can still contain an invented source
    // credit. Each operator compares exact facts with its complete adapter.
    state::SlotInputs invented;
    invented.authority = network.nodes[1]->authority;
    invented.anchor = candidate.Header().anchor;
    invented.facts_from = candidate.Header().facts_from;
    invented.facts_to = candidate.Header().facts_to;
    invented.body.facts.push_back(EncodeFact(DepositAtCursor({130, 0}, "pbft-deposit", 10'001)).value());
    auto fabricated{network.nodes[1]->owner->Preview(state::ParentRef::Committed(*network.nodes[1]->owner->Head()), invented)};
    RequireOk(fabricated, "executable fabricated credit");
    auto fabricated_message{EncodeMessage(Message{Proposal{0, 0, fabricated->Header(), std::nullopt, fabricated->Body()}}, network.nodes[2]->profile).value()};
    RequireCode(network.nodes[2]->agreement->OnMessage(1, fabricated_message), ErrorCode::VERIFY_FAILED, "invented source credit");
    BOOST_CHECK_EQUAL(network.nodes[2]->signer_journal->Status().appended_seq, 0U);
}

BOOST_AUTO_TEST_CASE(intent_barrier_failure_never_publishes_a_vote)
{
    std::deque<PbftFrame> frames;
    storage::MemoryJournalFaults faults;
    faults.fail_barrier = 1;
    PbftReplica node{1, frames, faults};
    auto candidate{node.Candidate()};
    RequireCode(node.agreement->Propose(candidate, node.authority), ErrorCode::STORAGE_FATAL, "intent fsync failure");
    // Proposal data may be queued internally, but no frame is published after
    // a signing-journal failure and no signature record exists.
    BOOST_CHECK(frames.empty());
    BOOST_CHECK(!node.commit_log.DecisionAt(1));
    BOOST_CHECK(node.signer_journal->IsFatal());
}

BOOST_AUTO_TEST_CASE(next_sequence_uses_certified_value_parent_and_new_scheduled_leader)
{
    PbftNetwork network;
    auto first{network.nodes[1]->Candidate()};
    RequireOk(network.nodes[1]->agreement->Propose(first, network.nodes[1]->authority), "first proposal");
    network.Drain();
    auto second{network.nodes[2]->Candidate()};
    BOOST_CHECK(second.Header().parent_cert_hash == first.Header().Hash());
    RequireOk(network.nodes[2]->agreement->Propose(second, network.nodes[2]->authority), "next leader proposal");
    network.Drain();
    for (const auto& node : network.nodes) {
        BOOST_CHECK_EQUAL(node->owner->Head()->Header().height, 2U);
        BOOST_CHECK(node->owner->Head()->Header().Hash() == second.Header().Hash());
    }
}

BOOST_AUTO_TEST_SUITE_END()
