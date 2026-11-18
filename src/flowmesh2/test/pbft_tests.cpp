// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** Stable-leader PBFT over four seats in one process (M4-AGR-01, M4-AGR-02,
 * M4-AGR-12): four AgreementRigs, each signing as its own seat over its own
 * identically scripted fake chain, connected by RecordingMeshes whose frames
 * the test delivers in order (optionally dropping some). TEST values only.
 */

#include <boost/test/unit_test.hpp>

#include <crypto/bls.h>
#include <flowmesh2/agreement/digests.h>
#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/agreement/pbft.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/storage/record.h>
#include <flowmesh2/test/util/agreement_fixtures.h>
#include <flowmesh2/test/util/mesh_fixtures.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::agreement;
using namespace flowmesh2::test;

namespace {

struct Seat {
    std::unique_ptr<AgreementRig> rig;
    PbftSafetyRules rules;
    std::unique_ptr<RecordingMesh> mesh;
    std::unique_ptr<EvidenceStore> evidence;
    std::unique_ptr<PbftAgreement> pbft;
};

//! Keep(from, to, frame): true to deliver the frame.
using Filter = std::function<bool(uint32_t, uint32_t, const std::vector<unsigned char>&)>;

bool IsCommitVote(const std::vector<unsigned char>& frame)
{
    return frame.size() > 2 && frame[1] == static_cast<unsigned char>(MessageType::VOTE) &&
           frame[2] == static_cast<unsigned char>(storage::VotePhase::COMMIT);
}

struct Cluster {
    std::vector<std::unique_ptr<Seat>> seats;
    std::vector<Error> refusals; //!< every OnMessage error seen while delivering
    uint64_t now_ms{0};

    explicit Cluster(uint32_t k, PbftConfig base = {})
    {
        std::vector<std::string> names;
        for (uint32_t i{0}; i < k; ++i) names.push_back("s" + std::to_string(i));
        for (uint32_t i{0}; i < k; ++i) {
            auto seat{std::make_unique<Seat>()};
            RigOptions options;
            options.seat_names = names;
            options.self_seat = i;
            seat->rig = std::make_unique<AgreementRig>(options);
            seat->mesh = RecordingMesh::Make(TestMeshConfig(i, k));
            Result<EvidenceStore> evidence{EvidenceStore::Create(seat->rig->profile, seat->rig->genesis.Header().domain, *seat->rig->adapter, seat->rig->commit_log)};
            RequireOk(evidence, "EvidenceStore::Create");
            seat->evidence = std::make_unique<EvidenceStore>(std::move(evidence).value());
            const PbftContext context{*seat->rig->adapter, *seat->rig->owner, seat->rig->signer, seat->rig->signer_log,
                                      seat->rig->commit_log, seat->rules, *seat->mesh, *seat->mesh, *seat->evidence};
            PbftConfig config{base};
            config.self_seat = i;
            Result<std::unique_ptr<PbftAgreement>> pbft{PbftAgreement::Create(seat->rig->profile, config, context)};
            RequireOk(pbft, "PbftAgreement::Create");
            seat->pbft = std::move(pbft).value();
            seats.push_back(std::move(seat));
        }
    }

    Seat& At(uint32_t i) { return *seats.at(i); }
    //! The instance domain (every seat shares the genesis).
    uint256 Domain() const { return seats.front()->rig->genesis.Header().domain; }
    const AuthorityVersion& Authority() const { return seats.front()->rig->authority; }

    //! The seat that leads the current view, or k when none says so.
    uint32_t Leader()
    {
        for (uint32_t i{0}; i < seats.size(); ++i) {
            if (seats[i]->pbft->IsLeader()) return i;
        }
        return static_cast<uint32_t>(seats.size());
    }

    //! Append the same block with the same facts to every seat's fake chain.
    void AppendBlock(uint32_t height, std::vector<Fact> facts)
    {
        for (auto& seat : seats) RequireOk(seat->rig->adapter->AppendBlock(BlockHash(height), facts), "AppendBlock");
    }

    //! Inputs for the next slot from seat `i`'s own chain adapter (what an honest leader proposes).
    state::SlotInputs InputsFromChain(uint32_t i)
    {
        AgreementRig& rig{*At(i).rig};
        const std::shared_ptr<const state::HeadView> head{rig.owner->Head()};
        Result<chain::PollReply> reply{rig.adapter->Poll(head->Header().facts_to, MAX_SLOT_FACTS)};
        RequireOk(reply, "Poll");
        BOOST_REQUIRE(reply->Status() == chain::PollStatus::FACTS);
        const chain::FactBatch& batch{*reply->Batch()};
        state::SlotInputs inputs;
        inputs.authority = batch.authority;
        inputs.anchor = batch.anchor;
        inputs.facts_from = batch.from;
        inputs.facts_to = batch.to;
        for (const Fact& fact : batch.facts) inputs.body.facts.push_back(EncodeFact(fact).value());
        return inputs;
    }

    //! Deliver every queued frame until nothing moves; `keep` may drop frames.
    size_t Deliver(const Filter& keep = {})
    {
        size_t delivered{0};
        bool progress{true};
        while (progress) {
            progress = false;
            for (uint32_t from{0}; from < seats.size(); ++from) {
                std::vector<RecordingMesh::Sent> sent{std::move(seats[from]->mesh->sent)};
                seats[from]->mesh->sent.clear();
                for (const RecordingMesh::Sent& s : sent) {
                    if (keep && !keep(from, s.seat, s.frame)) continue;
                    progress = true;
                    ++delivered;
                    const Result<void> handled{seats[s.seat]->pbft->OnMessage(from, s.frame)};
                    if (!handled.has_value()) {
                        BOOST_TEST_MESSAGE("seat " << s.seat << " refused a frame from " << from << ": " << ToString(handled.error().code) << " "
                                                   << handled.error().detail);
                        refusals.push_back(handled.error());
                    }
                }
            }
        }
        return delivered;
    }

    //! Advance every seat's clock to `t` (deliver what that produces too).
    void Tick(uint64_t t, const Filter& keep = {})
    {
        now_ms = t;
        for (auto& seat : seats) RequireOk(seat->pbft->OnTimeout(t), "OnTimeout");
        Deliver(keep);
    }

    //! Take every seat's decision, re-execute (followers) and apply it; the header hash decided.
    uint256 ApplyDecisions(const std::optional<state::Candidate>& leader_candidate = std::nullopt, uint32_t leader = 0)
    {
        uint256 decided;
        for (uint32_t i{0}; i < seats.size(); ++i) {
            std::optional<Decision> decision{seats[i]->pbft->TakeDecision()};
            BOOST_REQUIRE_MESSAGE(decision.has_value(), "seat " + std::to_string(i) + " has no decision");
            if (decided.IsNull()) decided = decision->header.Hash();
            BOOST_CHECK(decision->header.Hash() == decided);
            BOOST_CHECK(decision->certificate.header_hash == decided);
            AgreementRig& rig{*seats[i]->rig};
            // Every certificate verifies against the seat set with at least a quorum of signers.
            const std::vector<uint32_t> signers{decision->certificate.SignerIndices(rig.seats.Size()).value()};
            BOOST_CHECK_GE(signers.size(), rig.seats.Quorum());
            std::vector<bls::VerifiedPublicKey> keys;
            for (const uint32_t s : signers) keys.push_back(rig.seats.Keys()[s]);
            const uint256 digest{decision->header.CommitDigest()};
            BOOST_CHECK(bls::FastAggregateVerify(keys, std::span<const unsigned char>{digest.data(), digest.size()},
                                                 bls::Signature::Decode(decision->certificate.aggregate).value()));
            std::optional<state::Candidate> candidate;
            if (leader_candidate.has_value() && i == leader) {
                candidate = *leader_candidate;
            } else {
                Result<state::Candidate> reexecuted{rig.owner->Reexecute(decision->header, decision->body)};
                RequireOk(reexecuted, "Reexecute");
                candidate = std::move(reexecuted).value();
            }
            Result<state::AppliedSlot> applied{rig.owner->Apply(std::move(*candidate), decision->decision, rig.adapter->CurrentAuthority())};
            RequireOk(applied, "Apply");
            const Result<storage::PendingApplied> pending{rig.commit_log.AppendApplied(applied->receipt)};
            RequireOk(pending, "AppendApplied");
            RequireOk(rig.commit_log.Barrier(), "B-APPLIED");
        }
        return decided;
    }

    std::vector<storage::RecordType> SignerTypes(uint32_t i)
    {
        std::vector<storage::RecordType> types;
        const Result<std::vector<storage::JournalRecord>> records{At(i).rig->signer_journal->ReadAll()};
        RequireOk(records, "signer ReadAll");
        for (const storage::JournalRecord& r : *records) types.push_back(r.type);
        return types;
    }
};

Vote SignedVote(const std::string& seat_name, uint32_t seat, storage::VotePhase phase, uint64_t epoch, uint64_t view, uint64_t slot,
                const uint256& header_hash, const uint256& digest)
{
    Vote vote;
    vote.phase = phase;
    vote.epoch = epoch;
    vote.view = view;
    vote.slot = slot;
    vote.header_hash = header_hash;
    vote.seat = seat;
    vote.signature = BlsKeyFromLabel(SeatLabel(seat_name)).Sign(std::span<const unsigned char>{digest.data(), digest.size()}).Compressed();
    return vote;
}

} // namespace

BOOST_AUTO_TEST_SUITE(pbft_tests)

BOOST_AUTO_TEST_CASE(four_seats_certify_two_slots)
{
    Cluster c{4};
    c.AppendBlock(130, {Fact{DepositAtCursor(FactCursor{130, 0}, "d0")}});
    const uint32_t leader{c.Leader()};
    BOOST_REQUIRE_LT(leader, 4U);
    for (uint32_t i{0}; i < 4; ++i) BOOST_CHECK_EQUAL(c.At(i).pbft->IsLeader(), i == leader);
    // A follower cannot propose.
    const uint32_t follower{(leader + 1) % 4};
    const state::Candidate stray{c.At(follower).rig->Preview(c.InputsFromChain(follower))};
    RequireRefusal(c.At(follower).pbft->Propose(stray, c.Authority()), ErrorCode::INVALID_INPUT, PBFT_NOT_LEADER, "a follower proposes");

    const state::Candidate candidate{c.At(leader).rig->Preview(c.InputsFromChain(leader))};
    RequireOk(c.At(leader).pbft->Propose(candidate, c.Authority()), "Propose");
    BOOST_REQUIRE(c.At(leader).pbft->Status().proposal_accepted);
    // Another header in the same view is a second proposal; the same one again is idempotent.
    const state::Candidate another{c.At(leader).rig->Preview(c.At(leader).rig->Inputs(c.Authority(), 1, "synthetic"))};
    BOOST_REQUIRE(another.Header().Hash() != candidate.Header().Hash());
    RequireRefusal(c.At(leader).pbft->Propose(another, c.Authority()), ErrorCode::ORDER_VIOLATION, PBFT_PROPOSAL_PENDING, "a second proposal");
    RequireOk(c.At(leader).pbft->Propose(candidate, c.Authority()), "the same proposal again");
    c.Deliver();
    BOOST_CHECK(c.refusals.empty());
    for (uint32_t i{0}; i < 4; ++i) {
        const PbftStatus status{c.At(i).pbft->Status()};
        BOOST_CHECK_EQUAL(status.slot, 1U);
        BOOST_CHECK_EQUAL(status.view, 0U);
        BOOST_CHECK(status.decided);
        BOOST_CHECK_EQUAL(status.prepare_votes, 4U);
        BOOST_CHECK_EQUAL(status.commit_votes, 4U);
    }
    const uint256 decided{c.ApplyDecisions(candidate, leader)};
    BOOST_CHECK(decided == candidate.Header().Hash());
    for (uint32_t i{0}; i < 4; ++i) {
        BOOST_CHECK(c.At(i).rig->owner->Head()->HeaderHash() == decided);
        BOOST_CHECK_EQUAL(c.At(i).rig->owner->Head()->Header().height, 1U);
        // The deposit reached every seat's state.
        BOOST_CHECK(c.At(i).rig->owner->Head()->State().engine.ledger.BalanceOf(TestAccount("alice"), TestAsset("rUSD")).available == 1'000);
        // Signer journal: ACCEPTED_PROPOSAL, INTENT(PREPARE), SIGNED_VOTE, INTENT(COMMIT), SIGNED_VOTE.
        const std::vector<storage::RecordType> expected{storage::RecordType::ACCEPTED_PROPOSAL, storage::RecordType::INTENT,
                                                        storage::RecordType::SIGNED_VOTE, storage::RecordType::INTENT,
                                                        storage::RecordType::SIGNED_VOTE};
        BOOST_CHECK(c.SignerTypes(i) == expected);
        // Commit journal: PREPARED_QC, DECISION, APPLIED.
        const std::vector<storage::JournalRecord> commit{c.At(i).rig->commit_journal->ReadAll().value()};
        BOOST_REQUIRE_EQUAL(commit.size(), 3U);
        BOOST_CHECK(commit[0].type == storage::RecordType::PREPARED_QC && commit[1].type == storage::RecordType::DECISION &&
                    commit[2].type == storage::RecordType::APPLIED);
    }
    // Slot 2, a heartbeat: the next proposal carries the parent certificate; same leader (same term).
    BOOST_CHECK_EQUAL(c.Leader(), leader);
    const state::Candidate second{c.At(leader).rig->Preview(c.InputsFromChain(leader))};
    BOOST_CHECK_EQUAL(second.Header().height, 2U);
    RequireOk(c.At(leader).pbft->Propose(second, c.Authority()), "Propose slot 2");
    c.Deliver();
    BOOST_CHECK(c.refusals.empty());
    BOOST_CHECK(c.ApplyDecisions(second, leader) == second.Header().Hash());
    for (uint32_t i{0}; i < 4; ++i) BOOST_CHECK_EQUAL(c.At(i).rig->owner->Head()->Header().height, 2U);
}

BOOST_AUTO_TEST_CASE(follower_refuses_a_proposal_that_drops_a_deposit)
{
    // M4-AGR-12. The leader proposes a body without the block's deposit while
    // advancing facts_to past it; no honest seat prepares. After the timeout a
    // new view's leader proposes honestly and the deposit is credited.
    Cluster c{4};
    c.AppendBlock(130, {Fact{DepositAtCursor(FactCursor{130, 0}, "d0")}, Fact{DepositAtCursor(FactCursor{130, 1}, "d1")}});
    const uint32_t leader{c.Leader()};
    state::SlotInputs dropped{c.InputsFromChain(leader)};
    BOOST_REQUIRE_EQUAL(dropped.body.facts.size(), 2U);
    dropped.body.facts.pop_back(); // the second deposit vanishes; facts_to still covers it
    const state::Candidate dishonest{c.At(leader).rig->Preview(dropped)};
    RequireOk(c.At(leader).pbft->Propose(dishonest, c.Authority()), "the dishonest proposal is well formed");
    c.Deliver();
    BOOST_REQUIRE_EQUAL(c.refusals.size(), 3U);
    for (const Error& e : c.refusals) BOOST_CHECK(e.code == ErrorCode::VERIFY_FAILED && e.detail == PBFT_FACTS_MISMATCH);
    for (uint32_t i{0}; i < 4; ++i) {
        const PbftStatus status{c.At(i).pbft->Status()};
        BOOST_CHECK(!status.decided);
        BOOST_CHECK_EQUAL(status.prepare_votes, i == leader ? 1U : 0U);
    }
    c.refusals.clear();
    // Time out: every seat reports, the quorum forms, view 1's leader builds the NEW_VIEW.
    const uint64_t timeout{PbftConfig{}.view_timeout_floor_ms};
    c.Tick(timeout);
    BOOST_CHECK(c.refusals.empty());
    const uint32_t next{c.Leader()};
    BOOST_REQUIRE_LT(next, 4U);
    BOOST_CHECK_NE(next, leader);
    for (uint32_t i{0}; i < 4; ++i) {
        const PbftStatus status{c.At(i).pbft->Status()};
        BOOST_CHECK_EQUAL(status.view, 1U);
        BOOST_CHECK(!status.changing);
        const storage::ViewStateRecord expected_view{0, 1, 1};
        BOOST_CHECK(c.At(i).rig->signer_log.ViewState() == std::optional<storage::ViewStateRecord>{expected_view});
        BOOST_CHECK(c.At(i).rig->signer_log.NewViewAccepted(0, 1, 1).has_value());
    }
    BOOST_CHECK(!c.At(next).pbft->SelectedHeader().has_value()); // nothing was prepared
    const state::Candidate honest{c.At(next).rig->Preview(c.InputsFromChain(next))};
    RequireOk(c.At(next).pbft->Propose(honest, c.Authority()), "the honest proposal in view 1");
    c.Deliver();
    BOOST_CHECK(c.refusals.empty());
    BOOST_CHECK(c.ApplyDecisions(honest, next) == honest.Header().Hash());
    for (uint32_t i{0}; i < 4; ++i) {
        BOOST_CHECK(c.At(i).rig->owner->Head()->State().engine.ledger.BalanceOf(TestAccount("alice"), TestAsset("rUSD")).available == 2'000);
    }
}

BOOST_AUTO_TEST_CASE(silent_leader_is_replaced_and_a_restart_resumes_the_view)
{
    Cluster c{4};
    const uint32_t leader{c.Leader()};
    const uint64_t timeout{PbftConfig{}.view_timeout_floor_ms};
    // Before the deadline nothing moves; retransmissions have nothing to send.
    c.Tick(timeout - 1);
    BOOST_CHECK_EQUAL(c.Leader(), leader);
    // Seat (leader + 2) hears the timeout first and reports alone: one report moves nobody else.
    RequireOk(c.At((leader + 2) % 4).pbft->OnTimeout(timeout), "one seat times out");
    BOOST_CHECK(c.At((leader + 2) % 4).pbft->Status().changing);
    c.Deliver();
    for (uint32_t i{0}; i < 4; ++i) {
        if (i == (leader + 2) % 4) continue;
        BOOST_CHECK_EQUAL(c.At(i).pbft->Status().view, 0U);
    }
    // A second report (f + 1 = 2 of 4) synchronises the rest; with q = 3 reports the new leader builds the view.
    RequireOk(c.At((leader + 3) % 4).pbft->OnTimeout(timeout), "a second seat times out");
    c.Deliver();
    BOOST_CHECK(c.refusals.empty());
    const uint32_t next{c.Leader()};
    BOOST_REQUIRE_LT(next, 4U);
    BOOST_CHECK_NE(next, leader);
    for (uint32_t i{0}; i < 4; ++i) {
        BOOST_CHECK_EQUAL(c.At(i).pbft->Status().view, 1U);
        BOOST_CHECK(!c.At(i).pbft->Status().changing);
    }
    // Restart one follower over its own logs: it resumes view 1 ACTIVE from VIEW_STATE and NEW_VIEW_ACCEPTED.
    const uint32_t restarted{(next + 1) % 4};
    {
        Seat& seat{c.At(restarted)};
        seat.pbft.reset();
        const PbftContext context{*seat.rig->adapter, *seat.rig->owner, seat.rig->signer, seat.rig->signer_log, seat.rig->commit_log,
                                  seat.rules, *seat.mesh, *seat.mesh, *seat.evidence};
        PbftConfig config;
        config.self_seat = restarted;
        Result<std::unique_ptr<PbftAgreement>> pbft{PbftAgreement::Create(seat.rig->profile, config, context)};
        RequireOk(pbft, "Create after a restart");
        seat.pbft = std::move(pbft).value();
        BOOST_CHECK_EQUAL(seat.pbft->Status().view, 1U);
        BOOST_CHECK(!seat.pbft->Status().changing);
    }
    const state::Candidate candidate{c.At(next).rig->Preview(c.InputsFromChain(next))};
    RequireOk(c.At(next).pbft->Propose(candidate, c.Authority()), "propose in view 1");
    c.Deliver();
    BOOST_CHECK(c.refusals.empty());
    BOOST_CHECK(c.ApplyDecisions(candidate, next) == candidate.Header().Hash());
    // The leader's silence in view 0 left no vote: its signer journal starts with the VIEW_STATE of view 1.
    for (uint32_t i{0}; i < 4; ++i) {
        BOOST_TEST_MESSAGE("seat " << i << " signer records=" << c.SignerTypes(i).size() << " view=" << c.At(i).pbft->Status().view
                                   << (i == leader ? " (view-0 leader)" : "") << (i == next ? " (view-1 leader)" : ""));
    }
    const std::vector<storage::RecordType> leader_types{c.SignerTypes(leader)};
    BOOST_REQUIRE_MESSAGE(!leader_types.empty(), "the silent leader's signer journal is empty");
    BOOST_CHECK(leader_types.front() == storage::RecordType::VIEW_STATE);
}

BOOST_AUTO_TEST_CASE(a_prepared_value_survives_the_view_change)
{
    // Every COMMIT vote is dropped, so the slot prepares in view 0 but never
    // decides; the reports carry the PreparedQC, the new leader selects and
    // re-proposes the same header, and view 1 decides exactly it.
    Cluster c{4};
    c.AppendBlock(130, {Fact{DepositAtCursor(FactCursor{130, 0}, "d0")}});
    const uint32_t leader{c.Leader()};
    const state::Candidate candidate{c.At(leader).rig->Preview(c.InputsFromChain(leader))};
    RequireOk(c.At(leader).pbft->Propose(candidate, c.Authority()), "Propose");
    const Filter drop_commits{[](uint32_t, uint32_t, const std::vector<unsigned char>& frame) { return !IsCommitVote(frame); }};
    c.Deliver(drop_commits);
    BOOST_CHECK(c.refusals.empty());
    for (uint32_t i{0}; i < 4; ++i) {
        const PbftStatus status{c.At(i).pbft->Status()};
        BOOST_CHECK(status.prepared);
        BOOST_CHECK(!status.decided);
        BOOST_CHECK_EQUAL(status.commit_votes, 1U); // only its own
        BOOST_CHECK(c.At(i).rig->commit_log.HighestPrepared(1).has_value());
    }
    const uint64_t timeout{PbftConfig{}.view_timeout_floor_ms};
    c.Tick(timeout, drop_commits);
    BOOST_CHECK(c.refusals.empty());
    const uint32_t next{c.Leader()};
    BOOST_REQUIRE_LT(next, 4U);
    const std::optional<uint256> selected{c.At(next).pbft->SelectedHeader()};
    BOOST_CHECK(selected == std::optional<uint256>{candidate.Header().Hash()});
    // The new leader held the body and re-proposed it by itself.
    BOOST_CHECK(c.At(next).pbft->Status().proposal_accepted);
    // The view-1 commit votes were dropped too; the retry timer resends them and view 1 decides the prepared header.
    c.Tick(timeout + PbftConfig{}.retry_ms);
    BOOST_CHECK(c.refusals.empty());
    for (uint32_t i{0}; i < 4; ++i) BOOST_CHECK(c.At(i).pbft->Status().decided);
    BOOST_CHECK(c.ApplyDecisions() == candidate.Header().Hash());
    for (uint32_t i{0}; i < 4; ++i) {
        // The DECISION's certificate was built in view 1, over the same header.
        BOOST_CHECK(c.At(i).rig->commit_log.DecisionAt(1)->header_hash == candidate.Header().Hash());
        BOOST_CHECK_EQUAL(c.At(i).rig->commit_log.HighestPrepared(1)->view, 1U);
    }
}

BOOST_AUTO_TEST_CASE(forged_wrong_seat_and_double_votes_are_refused)
{
    Cluster c{4};
    const uint32_t leader{c.Leader()};
    const state::Candidate candidate{c.At(leader).rig->Preview(c.InputsFromChain(leader))};
    RequireOk(c.At(leader).pbft->Propose(candidate, c.Authority()), "Propose");
    // Deliver only the proposal, so every follower has accepted and prepared but nobody has the others' votes.
    c.Deliver([](uint32_t, uint32_t, const std::vector<unsigned char>& frame) {
        return frame.size() > 1 && frame[1] == static_cast<unsigned char>(MessageType::PROPOSAL);
    });
    for (auto& seat : c.seats) seat->mesh->sent.clear();
    const uint32_t target{(leader + 1) % 4};
    const uint32_t other{(leader + 2) % 4};
    PbftAgreement& pbft{*c.At(target).pbft};
    const uint256 hash{candidate.Header().Hash()};
    const uint256 prepare_digest{PrepareDigest(c.Domain(), 0, 1, 0, hash)};
    // A vote whose signature is another seat's.
    Vote forged{SignedVote("s" + std::to_string(leader), other, storage::VotePhase::PREPARE, 0, 0, 1, hash, prepare_digest)};
    RequireRefusal(pbft.OnMessage(other, EncodeMessage(Message{forged}, c.At(target).rig->profile).value()), ErrorCode::VERIFY_FAILED,
                   PBFT_VOTE_SIGNATURE, "forged signature");
    // A vote that names another seat than its sender.
    Vote impostor{SignedVote("s" + std::to_string(other), other, storage::VotePhase::PREPARE, 0, 0, 1, hash, prepare_digest)};
    RequireRefusal(pbft.OnMessage((other + 1) % 4 == target ? (other + 2) % 4 : (other + 1) % 4,
                                  EncodeMessage(Message{impostor}, c.At(target).rig->profile).value()),
                   ErrorCode::INVALID_INPUT, PBFT_VOTE_SEAT, "wrong sender");
    // A valid vote; the same valid vote again is a retransmission.
    RequireOk(pbft.OnMessage(other, EncodeMessage(Message{impostor}, c.At(target).rig->profile).value()), "a valid prepare vote");
    RequireOk(pbft.OnMessage(other, EncodeMessage(Message{impostor}, c.At(target).rig->profile).value()), "retransmission");
    BOOST_CHECK_EQUAL(pbft.Status().prepare_votes, 2U);
    // A VOTE message with a statement phase never counts.
    Vote statement_shaped{impostor};
    statement_shaped.phase = storage::VotePhase::VIEW_CHANGE;
    RequireRefusal(pbft.OnMessage(other, EncodeMessage(Message{statement_shaped}, c.At(target).rig->profile).value()), ErrorCode::INVALID_INPUT,
                   VOTE_PHASE_NOT_VOTE, "a statement phase on a vote");
    // A proposal from a seat that does not lead the view, and a message from itself.
    Proposal stray;
    stray.epoch = 0;
    stray.view = 0;
    stray.header = candidate.Header();
    stray.body = candidate.Body();
    RequireRefusal(pbft.OnMessage(other, EncodeMessage(Message{stray}, c.At(target).rig->profile).value()), ErrorCode::INVALID_INPUT,
                   PBFT_WRONG_LEADER, "proposal from a non-leader");
    RequireRefusal(pbft.OnMessage(target, EncodeMessage(Message{impostor}, c.At(target).rig->profile).value()), ErrorCode::INVALID_INPUT,
                   PBFT_VOTE_SEAT, "a message from itself");
    // A second vote from the same seat for another header: a double vote, refused, recorded, spread, and the seat halts (M4-AGR-07).
    const uint256 other_hash{LabelHash("fm2/test/pbft/other-header")};
    Vote doubled{SignedVote("s" + std::to_string(other), other, storage::VotePhase::PREPARE, 0, 0, 1, other_hash, PrepareDigest(c.Domain(), 0, 1, 0, other_hash))};
    BOOST_CHECK(!pbft.Status().halted);
    RequireRefusal(pbft.OnMessage(other, EncodeMessage(Message{doubled}, c.At(target).rig->profile).value()), ErrorCode::EQUIVOCATION_REFUSED,
                   PBFT_DOUBLE_VOTE, "double vote");
    BOOST_CHECK(pbft.Status().halted);
    BOOST_CHECK(c.At(target).evidence->Halted());
    RequireRefusal(pbft.OnMessage(other, EncodeMessage(Message{impostor}, c.At(target).rig->profile).value()), ErrorCode::FENCED, PBFT_FENCED,
                   "a halted seat");
}

BOOST_AUTO_TEST_CASE(a_double_vote_halts_the_seat_and_spreads_as_evidence)
{
    Cluster c{4};
    const uint32_t leader{c.Leader()};
    const state::Candidate candidate{c.At(leader).rig->Preview(c.InputsFromChain(leader))};
    RequireOk(c.At(leader).pbft->Propose(candidate, c.Authority()), "Propose");
    c.Deliver([](uint32_t, uint32_t, const std::vector<unsigned char>& frame) {
        return frame.size() > 1 && frame[1] == static_cast<unsigned char>(MessageType::PROPOSAL);
    });
    for (auto& seat : c.seats) seat->mesh->sent.clear();
    const uint32_t target{(leader + 1) % 4};
    const uint32_t other{(leader + 2) % 4};
    const uint256 hash{candidate.Header().Hash()};
    const uint256 other_hash{LabelHash("fm2/test/pbft/evidence-header")};
    const Vote honest{SignedVote("s" + std::to_string(other), other, storage::VotePhase::PREPARE, 0, 0, 1, hash, PrepareDigest(c.Domain(), 0, 1, 0, hash))};
    const Vote doubled{SignedVote("s" + std::to_string(other), other, storage::VotePhase::PREPARE, 0, 0, 1, other_hash,
                                  PrepareDigest(c.Domain(), 0, 1, 0, other_hash))};
    const ProfileParams& profile{c.At(target).rig->profile};
    RequireOk(c.At(target).pbft->OnMessage(other, EncodeMessage(Message{honest}, profile).value()), "the honest vote");
    RequireRefusal(c.At(target).pbft->OnMessage(other, EncodeMessage(Message{doubled}, profile).value()), ErrorCode::EQUIVOCATION_REFUSED,
                   PBFT_DOUBLE_VOTE, "the double vote");
    // The target recorded it durably (an EVIDENCE record after a barrier) and sent it to every other seat on CRITICAL.
    const Result<std::vector<Evidence>> recorded{c.At(target).evidence->Load()};
    RequireOk(recorded, "Load");
    BOOST_REQUIRE_EQUAL(recorded->size(), 1U);
    BOOST_CHECK(recorded->front().Kind() == EvidenceKind::DOUBLE_VOTE);
    const Result<std::vector<storage::JournalRecord>> commit{c.At(target).rig->commit_journal->ReadAll()};
    RequireOk(commit, "ReadAll");
    BOOST_REQUIRE(!commit->empty());
    BOOST_CHECK(commit->back().type == storage::RecordType::EVIDENCE);
    BOOST_CHECK_EQUAL(c.At(target).rig->commit_journal->Status().durable_seq, commit->back().seq);
    size_t evidence_frames{0};
    for (const RecordingMesh::Sent& s : c.At(target).mesh->sent) {
        if (s.frame.size() > 1 && s.frame[1] == static_cast<unsigned char>(MessageType::EVIDENCE)) {
            ++evidence_frames;
            BOOST_CHECK(s.lane == net::Lane::CRITICAL);
        }
    }
    BOOST_CHECK_EQUAL(evidence_frames, 3U);
    // Delivered, every seat verifies, records and halts; the leader's slot never decides.
    c.Deliver();
    for (uint32_t i{0}; i < 4; ++i) {
        BOOST_CHECK_MESSAGE(c.At(i).pbft->Status().halted, "seat " << i << " not halted");
        BOOST_CHECK_EQUAL(c.At(i).evidence->Size(), 1U);
        BOOST_CHECK(!c.At(i).pbft->Status().decided);
    }
    RequireRefusal(c.At(other).pbft->OnTimeout(1'000), ErrorCode::FENCED, PBFT_FENCED, "a halted seat's timer");
    // A recovered commit log carries the evidence: the store stays halted after a restart.
    Result<storage::CommitLog> recovered{storage::CommitLog::Recover(*c.At(target).rig->commit_journal, profile)};
    RequireOk(recovered, "CommitLog::Recover");
    Result<EvidenceStore> again{EvidenceStore::Create(profile, c.Domain(), *c.At(target).rig->adapter, *recovered)};
    RequireOk(again, "EvidenceStore::Create after a restart");
    BOOST_CHECK(again->Halted());
    BOOST_CHECK(again->Load().value() == *recorded);
}

BOOST_AUTO_TEST_CASE(create_checks_the_seat_and_the_key)
{
    RigOptions options;
    options.seat_names = {"s0", "s1", "s2", "s3"};
    options.self_seat = 1;
    AgreementRig rig{options};
    PbftSafetyRules rules;
    auto mesh{RecordingMesh::Make(TestMeshConfig(1, 4))};
    EvidenceStore evidence{EvidenceStore::Create(rig.profile, rig.genesis.Header().domain, *rig.adapter, rig.commit_log).value()};
    const PbftContext context{*rig.adapter, *rig.owner, rig.signer, rig.signer_log, rig.commit_log, rules, *mesh, *mesh, evidence};
    PbftConfig config;
    config.self_seat = 4;
    RequireRefusal(PbftAgreement::Create(rig.profile, config, context), ErrorCode::INVALID_INPUT, PBFT_SELF_SEAT, "seat outside the set");
    config.self_seat = 0; // the rig signs as seat 1
    RequireRefusal(PbftAgreement::Create(rig.profile, config, context), ErrorCode::INVALID_INPUT, PBFT_SELF_KEY, "another seat's key");
    config.self_seat = 1;
    Result<std::unique_ptr<PbftAgreement>> pbft{PbftAgreement::Create(rig.profile, config, context)};
    RequireOk(pbft, "Create");
    const PbftStatus status{(*pbft)->Status()};
    BOOST_CHECK_EQUAL(status.slot, 1U);
    BOOST_CHECK_EQUAL(status.view, 0U);
    BOOST_CHECK(!status.decided);
    BOOST_CHECK(!(*pbft)->TakeDecision().has_value());
    // A candidate for the wrong slot or authority is refused before anything is written.
    const state::Candidate candidate{rig.Preview(rig.Inputs(rig.authority))};
    AuthorityVersion other{rig.authority};
    other.epoch += 1;
    RequireRefusal((*pbft)->Propose(candidate, other), ErrorCode::INVALID_INPUT, PBFT_CANDIDATE_AUTHORITY, "another authority");
    BOOST_CHECK_EQUAL(rig.signer_journal->Status().appended_seq, 0U);
    // The one-seat rules object answers for its view.
    PbftSafetyRules fresh;
    fresh.SetView(0, 1, 0, true, uint256{});
    Proposal p;
    p.header = candidate.Header();
    p.body = candidate.Body();
    RequireOk(fresh.MayPrepare(p), "may prepare in view 0");
    fresh.SetView(0, 1, 1, false, uint256{});
    RequireRefusal(fresh.MayPrepare(p), ErrorCode::ORDER_VIOLATION, PBFT_VIEW_STALE, "while changing");
    fresh.SetView(0, 1, 1, true, LabelHash("fm2/test/pbft/selected"));
    p.view = 1;
    RequireRefusal(fresh.MayPrepare(p), ErrorCode::INVALID_INPUT, PBFT_NEW_VIEW_SELECTED, "another header than the selected one");
    RequireRefusal(fresh.MayCommit(p, PreparedCert{}), ErrorCode::ORDER_VIOLATION, PBFT_NO_PROPOSAL, "commit without a proposal");
}

BOOST_AUTO_TEST_CASE(idle_grace_holds_the_first_timeout_until_work_arrives)
{
    PbftConfig base;
    base.idle_grace_ms = 1'000;
    const uint64_t timeout{base.view_timeout_floor_ms};
    Cluster c{4, base};
    const uint32_t leader{c.Leader()};
    // Nothing happens for far longer than a view timeout: nobody changes view while the slot is idle.
    c.Tick(timeout * 5);
    for (uint32_t i{0}; i < 4; ++i) BOOST_CHECK(!c.At(i).pbft->Status().changing);
    // Local work tightens every seat's deadline to a plain view timeout from now.
    for (auto& seat : c.seats) seat->pbft->OnLocalWork();
    c.Tick(timeout * 5 + timeout - 1);
    for (uint32_t i{0}; i < 4; ++i) BOOST_CHECK(!c.At(i).pbft->Status().changing);
    c.Tick(timeout * 6);
    for (uint32_t i{0}; i < 4; ++i) BOOST_CHECK_EQUAL(c.At(i).pbft->Status().view, 1U);
    BOOST_CHECK_NE(c.Leader(), leader);

    // A proposal tightens the followers' deadlines too: with the votes dropped, they time out a view timeout after it.
    Cluster d{4, base};
    const uint32_t first{d.Leader()};
    const state::Candidate candidate{d.At(first).rig->Preview(d.InputsFromChain(first))};
    RequireOk(d.At(first).pbft->Propose(candidate, d.Authority()), "Propose");
    d.Deliver([](uint32_t, uint32_t, const std::vector<unsigned char>& frame) {
        return frame.size() > 1 && frame[1] == static_cast<unsigned char>(MessageType::PROPOSAL);
    });
    d.Tick(timeout - 1, [](uint32_t, uint32_t, const std::vector<unsigned char>& frame) {
        return frame.size() > 1 && frame[1] == static_cast<unsigned char>(MessageType::PROPOSAL);
    });
    for (uint32_t i{0}; i < 4; ++i) BOOST_CHECK(!d.At(i).pbft->Status().changing);
    d.Tick(timeout, [](uint32_t, uint32_t, const std::vector<unsigned char>& frame) {
        return frame.size() > 1 && frame[1] == static_cast<unsigned char>(MessageType::PROPOSAL);
    });
    for (uint32_t i{0}; i < 4; ++i) BOOST_CHECK(d.At(i).pbft->Status().view >= 1 || d.At(i).pbft->Status().changing);
}

BOOST_AUTO_TEST_CASE(claims_from_f_plus_one_peers_give_the_ahead_hint)
{
    Cluster c{4};
    PbftAgreement& pbft{*c.At(0).pbft};
    const ProfileParams& profile{c.At(0).rig->profile};
    BOOST_CHECK(!pbft.AheadHint().has_value());
    BOOST_CHECK_EQUAL(pbft.Status().ahead_hint, 0U);
    // One peer's claim is not a hint (one Byzantine seat cannot drive a fetch); the vote itself is for another slot and ignored.
    const uint256 h{LabelHash("fm2/test/pbft/ahead")};
    const Vote far{SignedVote("s1", 1, storage::VotePhase::PREPARE, 0, 0, 5, h, PrepareDigest(c.Domain(), 0, 5, 0, h))};
    RequireOk(pbft.OnMessage(1, EncodeMessage(Message{far}, profile).value()), "a vote for slot 5");
    BOOST_CHECK(!pbft.AheadHint().has_value());
    // A second peer claiming slot 7: the f + 1 = 2 claims give the lower one, slot 5.
    const Vote further{SignedVote("s2", 2, storage::VotePhase::PREPARE, 0, 0, 7, h, PrepareDigest(c.Domain(), 0, 7, 0, h))};
    RequireOk(pbft.OnMessage(2, EncodeMessage(Message{further}, profile).value()), "a vote for slot 7");
    BOOST_CHECK(pbft.AheadHint() == std::optional<uint64_t>{5});
    BOOST_CHECK_EQUAL(pbft.Status().ahead_hint, 5U);
    // A claim for the current slot (or below) is none; a reset forgets a peer's claim.
    const Vote current{SignedVote("s3", 3, storage::VotePhase::PREPARE, 0, 0, 1, h, PrepareDigest(c.Domain(), 0, 1, 0, h))};
    (void)pbft.OnMessage(3, EncodeMessage(Message{current}, profile).value());
    BOOST_CHECK(pbft.AheadHint() == std::optional<uint64_t>{5});
    pbft.ResetClaim(2);
    BOOST_CHECK(!pbft.AheadHint().has_value());
    pbft.ResetClaim(1);
    BOOST_CHECK(!pbft.AheadHint().has_value());
}

BOOST_AUTO_TEST_CASE(a_new_leader_reproposes_a_fetched_selection)
{
    // Which seat leads view 1 of slot 1? The schedule is seeded by the seat set, so a throwaway cluster tells.
    uint32_t next_leader{4};
    {
        Cluster probe{4};
        probe.Tick(PbftConfig{}.view_timeout_floor_ms);
        next_leader = probe.Leader();
        BOOST_REQUIRE_LT(next_leader, 4U);
    }
    Cluster c{4};
    const uint32_t leader{c.Leader()};
    BOOST_REQUIRE_NE(leader, next_leader);
    const state::Candidate candidate{c.At(leader).rig->Preview(c.InputsFromChain(leader))};
    RequireOk(c.At(leader).pbft->Propose(candidate, c.Authority()), "Propose");
    const uint256 hash{candidate.Header().Hash()};
    // The proposal never reaches the next leader; commit votes are dropped everywhere, so the view times out prepared.
    const Filter keep{[next_leader](uint32_t, uint32_t to, const std::vector<unsigned char>& frame) {
        if (frame.size() > 1 && frame[1] == static_cast<unsigned char>(MessageType::PROPOSAL) && to == next_leader) return false;
        return !IsCommitVote(frame);
    }};
    c.Deliver(keep);
    BOOST_CHECK(!c.At(next_leader).pbft->Status().proposal_accepted);
    BOOST_CHECK(c.At(leader).pbft->Status().prepared);
    const uint64_t timeout{PbftConfig{}.view_timeout_floor_ms};
    c.Tick(timeout, keep);
    BOOST_REQUIRE_EQUAL(c.Leader(), next_leader);
    PbftAgreement& fresh{*c.At(next_leader).pbft};
    const PbftStatus status{fresh.Status()};
    BOOST_CHECK_EQUAL(status.view, 1U);
    BOOST_CHECK(!status.changing);
    BOOST_CHECK(fresh.SelectedHeader() == std::optional<uint256>{hash});
    BOOST_CHECK(status.selected_missing);
    BOOST_CHECK(!fresh.KnownProposal(hash).has_value());
    // Another header is not the selection; the body a peer holds is, and re-proposing it decides the slot in view 1.
    const std::optional<Proposal> held{c.At(leader).pbft->KnownProposal(hash)};
    BOOST_REQUIRE(held.has_value());
    SlotHeaderV2 other{held->header};
    other.anchor.height += 1;
    RequireRefusal(fresh.ProposeSelected(other, held->body), ErrorCode::INVALID_INPUT, PBFT_NEW_VIEW_SELECTED, "another header");
    RequireOk(fresh.ProposeSelected(held->header, held->body), "ProposeSelected");
    BOOST_CHECK(!fresh.Status().selected_missing);
    BOOST_CHECK(fresh.KnownProposal(hash).has_value());
    c.Deliver();
    c.Tick(timeout + PbftConfig{}.retry_ms);
    for (uint32_t i{0}; i < 4; ++i) BOOST_CHECK_MESSAGE(c.At(i).pbft->Status().decided, "seat " << i << " undecided");
    const uint256 decided{c.ApplyDecisions()};
    BOOST_CHECK(decided == hash);
}

BOOST_AUTO_TEST_CASE(a_reproposal_carries_the_selections_parent_certificate)
{
    // The stall of 2026-11-14 (O-105 (9)): two seats decide one slot with two different valid certificates (two quorums of
    // COMMIT votes); the next slot's header binds its proposer's; after a view change the new leader re-proposes the
    // selected header, which only works with the selection's own parent certificate, never with the new leader's.
    uint32_t next_leader{4};
    {
        Cluster probe{4};
        probe.Tick(PbftConfig{}.view_timeout_floor_ms);
        next_leader = probe.Leader();
        BOOST_REQUIRE_LT(next_leader, 4U);
    }
    Cluster c{4};
    const uint32_t leader{c.Leader()};
    BOOST_REQUIRE_NE(leader, next_leader);
    // Slot 1 decides everywhere, but the leader's COMMIT vote never reaches the next leader: the leader's certificate
    // aggregates its own vote and two more, the next leader's the three other seats'.
    const state::Candidate first{c.At(leader).rig->Preview(c.InputsFromChain(leader))};
    RequireOk(c.At(leader).pbft->Propose(first, c.Authority()), "Propose slot 1");
    const Filter without_leader_commit{[leader, next_leader](uint32_t from, uint32_t to, const std::vector<unsigned char>& frame) {
        return !(IsCommitVote(frame) && from == leader && to == next_leader);
    }};
    c.Deliver(without_leader_commit);
    for (uint32_t i{0}; i < 4; ++i) BOOST_REQUIRE_MESSAGE(c.At(i).pbft->Status().decided, "seat " << i << " undecided at slot 1");
    BOOST_CHECK(c.ApplyDecisions(first, leader) == first.Header().Hash());
    const std::optional<SlotCertificate> leaders{c.At(leader).rig->commit_log.CertificateAt(1)};
    const std::optional<SlotCertificate> nexts{c.At(next_leader).rig->commit_log.CertificateAt(1)};
    BOOST_REQUIRE(leaders.has_value() && nexts.has_value());
    BOOST_CHECK(leaders->header_hash == nexts->header_hash);
    BOOST_REQUIRE(leaders->Hash() != nexts->Hash());
    // Slot 2 as in the previous case: the proposal never reaches the next leader, commits are dropped, the view times out
    // prepared and the next leader must re-propose the selection.
    BOOST_REQUIRE_EQUAL(c.Leader(), leader);
    const state::Candidate second{c.At(leader).rig->Preview(c.InputsFromChain(leader))};
    RequireOk(c.At(leader).pbft->Propose(second, c.Authority()), "Propose slot 2");
    const uint256 hash{second.Header().Hash()};
    const Filter keep{[next_leader](uint32_t, uint32_t to, const std::vector<unsigned char>& frame) {
        if (frame.size() > 1 && frame[1] == static_cast<unsigned char>(MessageType::PROPOSAL) && to == next_leader) return false;
        return !IsCommitVote(frame);
    }};
    c.Deliver(keep);
    const uint64_t timeout{PbftConfig{}.view_timeout_floor_ms};
    c.Tick(timeout, keep);
    BOOST_REQUIRE_EQUAL(c.Leader(), next_leader);
    PbftAgreement& fresh{*c.At(next_leader).pbft};
    BOOST_REQUIRE(fresh.SelectedHeader() == std::optional<uint256>{hash});
    const std::optional<Proposal> held{c.At(leader).pbft->KnownProposal(hash)};
    BOOST_REQUIRE(held.has_value() && held->parent_cert.has_value());
    BOOST_CHECK(held->parent_cert->Hash() == leaders->Hash());
    // With its own certificate the re-proposal cannot match the header; a forged certificate is refused; the selection's is right.
    RequireRefusal(fresh.ProposeSelected(held->header, held->body), ErrorCode::INVALID_INPUT, PROPOSAL_PARENT_CERT, "own certificate");
    SlotCertificate forged{*held->parent_cert};
    forged.aggregate[0] ^= 0x01;
    RequireRefusal(fresh.ProposeSelected(held->header, held->body, forged), ErrorCode::VERIFY_FAILED, PBFT_PARENT_CERT, "a forged certificate");
    SlotCertificate other_head{*held->parent_cert};
    other_head.header_hash = LabelHash("fm2/test/pbft/other-head");
    RequireRefusal(fresh.ProposeSelected(held->header, held->body, other_head), ErrorCode::VERIFY_FAILED, PBFT_PARENT_CERT, "another head");
    RequireOk(fresh.ProposeSelected(held->header, held->body, held->parent_cert), "ProposeSelected with the selection's certificate");
    BOOST_CHECK(!fresh.Status().selected_missing);
    c.Deliver();
    c.Tick(timeout + PbftConfig{}.retry_ms);
    for (uint32_t i{0}; i < 4; ++i) BOOST_CHECK_MESSAGE(c.At(i).pbft->Status().decided, "seat " << i << " undecided at slot 2");
    BOOST_CHECK(c.ApplyDecisions() == hash);
    for (uint32_t i{0}; i < 4; ++i) BOOST_CHECK_EQUAL(c.At(i).rig->owner->Head()->Header().height, 2U);
}

BOOST_AUTO_TEST_CASE(a_prepared_certificate_from_another_slot_is_refused)
{
    // Review of 2026-11-16, confirmed defect 1. Slot 1 decides in view 0, so every seat holds slot 1's genuine
    // prepare certificate. Before slot 2 prepares, one seat reports a view change for slot 2 that carries that
    // certificate as its "prepared" value. The prepare digest binds domain, epoch and slot, so a certificate of
    // slot 1 never verifies as a preparation of slot 2: the report is refused, nothing is written as slot 2's
    // PreparedQC, and slot 2 decides its own header in view 0. (Before the fix the three honest seats stored
    // slot 1's header as slot 2's preparation and the slot never decided again, through every view change.)
    Cluster c{4};
    c.AppendBlock(130, {Fact{DepositAtCursor(FactCursor{130, 0}, "d0")}});
    const uint32_t leader{c.Leader()};
    BOOST_REQUIRE_LT(leader, 4U);
    const state::Candidate first{c.At(leader).rig->Preview(c.InputsFromChain(leader))};
    RequireOk(c.At(leader).pbft->Propose(first, c.Authority()), "Propose slot 1");
    c.Deliver();
    BOOST_REQUIRE(c.refusals.empty());
    BOOST_CHECK(c.ApplyDecisions(first, leader) == first.Header().Hash());
    // Slot 1's prepare certificate, as every seat recorded it.
    const std::optional<storage::PreparedQcRecord> qc{c.At(leader).rig->commit_log.HighestPrepared(1)};
    BOOST_REQUIRE(qc.has_value());
    BOOST_CHECK(qc->header_hash == first.Header().Hash());
    const Result<SlotCertificate> cert{SlotCertificate::Decode(qc->qc)};
    RequireOk(cert, "decode slot 1's prepare certificate");
    // A seat that is not the leader signs a report for slot 2, target view 1, carrying it.
    const uint32_t byzantine{(leader + 3) % 4};
    ViewChange report;
    report.epoch = 0;
    report.slot = 2;
    report.new_view = 1;
    report.seat = byzantine;
    report.prepared = PreparedCert{qc->view, *cert};
    const uint256 domain{c.At(byzantine).rig->owner->Head()->Header().domain};
    const uint256 digest{ViewChangeDigest(domain, report)};
    report.signature = BlsKeyFromLabel(SeatLabel("s" + std::to_string(byzantine)))
                           .Sign(std::span<const unsigned char>{digest.data(), digest.size()})
                           .Compressed();
    const ProfileParams& profile{c.At(leader).rig->profile};
    const std::vector<unsigned char> bytes{EncodeMessage(Message{report}, profile).value()};
    for (uint32_t i{0}; i < 4; ++i) {
        if (i == byzantine) continue;
        RequireRefusal(c.At(i).pbft->OnMessage(byzantine, bytes), ErrorCode::VERIFY_FAILED, PBFT_PREPARED_CERT,
                       "slot 1's certificate as slot 2's preparation");
        BOOST_CHECK(!c.At(i).rig->commit_log.HighestPrepared(2).has_value());
        BOOST_CHECK(!c.At(i).pbft->Status().prepared);
    }
    // Slot 2 decides its own header in view 0.
    const state::Candidate second{c.At(leader).rig->Preview(c.InputsFromChain(leader))};
    BOOST_CHECK_EQUAL(second.Header().height, 2U);
    RequireOk(c.At(leader).pbft->Propose(second, c.Authority()), "Propose slot 2");
    c.Deliver();
    BOOST_CHECK(c.refusals.empty());
    for (uint32_t i{0}; i < 4; ++i) {
        const PbftStatus status{c.At(i).pbft->Status()};
        BOOST_CHECK_EQUAL(status.slot, 2U);
        BOOST_CHECK_EQUAL(status.view, 0U);
        BOOST_CHECK(status.decided);
    }
    BOOST_CHECK(c.ApplyDecisions(second, leader) == second.Header().Hash());
    for (uint32_t i{0}; i < 4; ++i) BOOST_CHECK(c.At(i).rig->commit_log.HighestPrepared(2)->header_hash == second.Header().Hash());
}

namespace {

MessageType TypeOf(const std::vector<unsigned char>& frame)
{
    return frame.size() > 1 ? static_cast<MessageType>(frame[1]) : MessageType::EVIDENCE;
}

//! The leader restarted over its logs (the agreement rebuilt, everything else kept).
void Restart(Cluster& c, uint32_t i)
{
    Seat& seat{c.At(i)};
    seat.pbft.reset();
    const PbftContext context{*seat.rig->adapter, *seat.rig->owner, seat.rig->signer, seat.rig->signer_log, seat.rig->commit_log, seat.rules,
                              *seat.mesh, *seat.mesh, *seat.evidence};
    PbftConfig config;
    config.self_seat = i;
    Result<std::unique_ptr<PbftAgreement>> pbft{PbftAgreement::Create(seat.rig->profile, config, context)};
    RequireOk(pbft, "Create after a restart");
    seat.pbft = std::move(pbft).value();
}

} // namespace

BOOST_AUTO_TEST_CASE(the_proposal_leaves_only_after_accepted_proposal_is_durable)
{
    // Review of 2026-11-16, R4: the leader sent its proposal before ACCEPTED_PROPOSAL was durable, so a crash between the
    // send and its own B-INTENT brought it back without the proposal it had put on the wire, and the view was lost.
    Cluster c{4};
    c.AppendBlock(130, {Fact{DepositAtCursor(FactCursor{130, 0}, "d0")}});
    const uint32_t leader{c.Leader()};
    Seat& seat{c.At(leader)};
    std::vector<storage::JournalStatus> at_send; // the leader's signer journal as each PROPOSAL frame leaves
    seat.mesh->on_transmit = [&](const RecordingMesh::Sent& s) {
        if (TypeOf(s.frame) == MessageType::PROPOSAL) at_send.push_back(seat.rig->signer_journal->Status());
    };
    const state::Candidate candidate{seat.rig->Preview(c.InputsFromChain(leader))};
    RequireOk(seat.pbft->Propose(candidate, c.Authority()), "Propose");
    seat.mesh->on_transmit = nullptr;
    BOOST_REQUIRE_EQUAL(at_send.size(), 3U); // one frame per other seat
    for (const storage::JournalStatus& status : at_send) {
        BOOST_CHECK_GE(status.appended_seq, 1U);
        // ACCEPTED_PROPOSAL is the last record appended when the proposal leaves, and it is durable by then.
        BOOST_CHECK_EQUAL(status.durable_seq, status.appended_seq);
    }
    const std::vector<storage::RecordType> types{c.SignerTypes(leader)};
    BOOST_REQUIRE(!types.empty());
    BOOST_CHECK(types.front() == storage::RecordType::ACCEPTED_PROPOSAL);
    c.Deliver();
    BOOST_CHECK(c.refusals.empty());
    BOOST_CHECK(c.ApplyDecisions(candidate, leader) == candidate.Header().Hash());
}

BOOST_AUTO_TEST_CASE(a_restarted_seat_resends_the_report_it_signed)
{
    // Review of 2026-11-16, R5: a seat restarted while CHANGING never resent its report, and the report it rebuilt
    // carried the highest preparation it held, which may be one it learnt after signing, so the rebuilt report no
    // longer matched its signature. Here one seat sees nothing of view 0, reports target 1 with no preparation, then
    // learns the view-0 PreparedQC from the others' reports, and restarts.
    Cluster c{4};
    c.AppendBlock(130, {Fact{DepositAtCursor(FactCursor{130, 0}, "d0")}});
    const uint32_t leader{c.Leader()};
    const uint32_t late{(leader + 3) % 4};
    const Filter view0{[&](uint32_t, uint32_t to, const std::vector<unsigned char>& frame) { return to != late && !IsCommitVote(frame); }};
    const state::Candidate candidate{c.At(leader).rig->Preview(c.InputsFromChain(leader))};
    RequireOk(c.At(leader).pbft->Propose(candidate, c.Authority()), "Propose");
    c.Deliver(view0);
    BOOST_CHECK(c.refusals.empty());
    for (uint32_t i{0}; i < 4; ++i) BOOST_CHECK_EQUAL(c.At(i).pbft->Status().prepared, i != late);
    // Every seat times out into view 1. No NEW_VIEW reaches anyone and the late seat's report reaches nobody; the others'
    // reports reach the late seat, which learns the preparation (never downgrade) after it signed its own report.
    const uint64_t timeout{PbftConfig{}.view_timeout_floor_ms};
    const Filter changing{[&](uint32_t from, uint32_t, const std::vector<unsigned char>& frame) {
        const MessageType type{TypeOf(frame)};
        if (type == MessageType::NEW_VIEW) return false;
        if (type == MessageType::VIEW_CHANGE && from == late) return false;
        return !IsCommitVote(frame);
    }};
    c.Tick(timeout, changing);
    BOOST_CHECK(c.refusals.empty());
    for (uint32_t i{0}; i < 4; ++i) BOOST_CHECK_EQUAL(c.At(i).pbft->Status().view, 1U);
    BOOST_CHECK(c.At(late).pbft->Status().changing);
    BOOST_REQUIRE(c.At(late).rig->commit_log.HighestPrepared(1).has_value()); // learnt from the reports, after signing
    BOOST_CHECK_EQUAL(c.At(late).rig->commit_log.HighestPrepared(1)->view, 0U);
    for (uint32_t i{0}; i < 4; ++i) BOOST_CHECK_EQUAL(c.At(i).pbft->Status().reports_held, i == late ? 4U : 3U);
    Restart(c, late);
    {
        const PbftStatus status{c.At(late).pbft->Status()};
        BOOST_CHECK_EQUAL(status.view, 1U);
        BOOST_CHECK(status.changing);
        BOOST_CHECK_EQUAL(status.reports_held, 1U); // its own report, rebuilt
    }
    // The retry timer resends the report: the one signed (no preparation), accepted by every seat.
    std::vector<ViewChange> resent;
    c.At(late).mesh->on_transmit = [&](const RecordingMesh::Sent& s) {
        if (TypeOf(s.frame) != MessageType::VIEW_CHANGE) return;
        Result<Message> decoded{DecodeMessage(s.frame, c.At(late).rig->profile)};
        RequireOk(decoded, "DecodeMessage");
        resent.push_back(std::get<ViewChange>(*decoded));
    };
    const Filter no_new_view{[&](uint32_t, uint32_t, const std::vector<unsigned char>& frame) { return TypeOf(frame) != MessageType::NEW_VIEW; }};
    c.Tick(timeout + PbftConfig{}.retry_ms, no_new_view);
    c.At(late).mesh->on_transmit = nullptr;
    BOOST_CHECK(c.refusals.empty());
    BOOST_REQUIRE_EQUAL(resent.size(), 3U);
    for (const ViewChange& report : resent) {
        BOOST_CHECK_EQUAL(report.new_view, 1U);
        BOOST_CHECK_EQUAL(report.seat, late);
        BOOST_CHECK(!report.prepared.has_value()); // what was signed, not the preparation learnt afterwards
    }
    // Every other seat now holds all four reports; the restarted seat holds its own and what the seats still CHANGING
    // retransmitted (the view-1 leader, ACTIVE since it built its NEW_VIEW, retransmits no report).
    for (uint32_t i{0}; i < 4; ++i) {
        if (i == late) {
            BOOST_CHECK_GE(c.At(i).pbft->Status().reports_held, 3U);
        } else {
            BOOST_CHECK_EQUAL(c.At(i).pbft->Status().reports_held, 4U);
        }
    }
}

BOOST_AUTO_TEST_CASE(a_seat_restarted_before_signing_its_report_signs_and_sends_it)
{
    // Review workflow of 2026-11-17 on 17.3 item 11: a crash between the durable VIEW_STATE and the report's signature
    // left a restarted seat CHANGING with no report and, since R5, no timer: with f faulty seats every honest seat would
    // wait for a quorum nobody asks for. The crash is staged by writing the VIEW_STATE the seat would have written.
    Cluster c{4};
    const uint32_t restarted{1};
    Seat& seat{c.At(restarted)};
    RequireOk(seat.rig->signer_log.AppendViewState(storage::ViewStateRecord{0, 1, 1}), "VIEW_STATE");
    RequireOk(seat.rig->signer_log.Barrier(), "barrier");
    const storage::VoteKey key{c.Domain(), 0, 1, 1, storage::VotePhase::VIEW_CHANGE};
    BOOST_REQUIRE(!seat.rig->signer_log.SignedStatement(key).has_value());
    Restart(c, restarted);
    {
        const PbftStatus status{seat.pbft->Status()};
        BOOST_CHECK_EQUAL(status.view, 1U);
        BOOST_CHECK(status.changing);
        BOOST_CHECK_EQUAL(status.reports_held, 1U); // its own report, signed at the restart
    }
    BOOST_CHECK(seat.rig->signer_log.SignedStatement(key).has_value());
    // The report went out at the restart; the others, still in view 0, hold it above their view. The retry resends it
    // and the replay is dropped.
    c.Deliver();
    BOOST_CHECK(c.refusals.empty());
    for (uint32_t i{0}; i < 4; ++i) BOOST_CHECK_EQUAL(c.At(i).pbft->Status().reports_held, 1U);
    c.Tick(PbftConfig{}.retry_ms);
    BOOST_CHECK(c.refusals.empty());
    for (uint32_t i{0}; i < 4; ++i) {
        BOOST_CHECK_EQUAL(c.At(i).pbft->Status().reports_held, 1U);
        BOOST_CHECK_EQUAL(c.At(i).pbft->Status().view, i == restarted ? 1U : 0U);
    }
}

BOOST_AUTO_TEST_CASE(a_seat_restarted_in_view_zero_keeps_its_preparation)
{
    // A seat that prepared in view 0 and restarted came back without its preparation: view 0 writes no VIEW_STATE, and
    // ResumeViewState returned before it read the commit log's preparations, although pbft.h says a restart reads
    // HighestPrepared. Its next report then carried none. Its commit log still held the PreparedQC, and it may have signed
    // COMMIT for that header; q reports without the preparation let a NEW_VIEW select nothing and the slot decide another
    // header.
    Cluster c{4};
    c.AppendBlock(130, {Fact{DepositAtCursor(FactCursor{130, 0}, "d0")}});
    const uint32_t leader{c.Leader()};
    const uint32_t restarted{(leader + 2) % 4};
    const state::Candidate candidate{c.At(leader).rig->Preview(c.InputsFromChain(leader))};
    RequireOk(c.At(leader).pbft->Propose(candidate, c.Authority()), "Propose");
    const Filter drop_commits{[](uint32_t, uint32_t, const std::vector<unsigned char>& frame) { return !IsCommitVote(frame); }};
    c.Deliver(drop_commits);
    BOOST_CHECK(c.refusals.empty());
    BOOST_REQUIRE(c.At(restarted).pbft->Status().prepared);
    BOOST_REQUIRE(c.At(restarted).rig->commit_log.HighestPrepared(1).has_value());
    Restart(c, restarted);
    {
        const PbftStatus status{c.At(restarted).pbft->Status()};
        BOOST_CHECK_EQUAL(status.view, 0U);
        BOOST_CHECK(!status.changing);
        BOOST_CHECK(status.prepared); // from its commit log
    }
    // Its view-1 report carries the view-0 preparation of the candidate.
    std::vector<ViewChange> reports;
    c.At(restarted).mesh->on_transmit = [&](const RecordingMesh::Sent& s) {
        if (TypeOf(s.frame) != MessageType::VIEW_CHANGE) return;
        Result<Message> decoded{DecodeMessage(s.frame, c.At(restarted).rig->profile)};
        RequireOk(decoded, "DecodeMessage");
        reports.push_back(std::get<ViewChange>(*decoded));
    };
    const uint64_t timeout{PbftConfig{}.view_timeout_floor_ms};
    c.Tick(timeout, drop_commits);
    c.At(restarted).mesh->on_transmit = nullptr;
    BOOST_CHECK(c.refusals.empty());
    BOOST_REQUIRE(!reports.empty());
    for (const ViewChange& report : reports) {
        BOOST_CHECK_EQUAL(report.new_view, 1U);
        BOOST_REQUIRE(report.prepared.has_value());
        BOOST_CHECK_EQUAL(report.prepared->view, 0U);
        BOOST_CHECK(report.prepared->cert.header_hash == candidate.Header().Hash());
    }
    // View 1 decides the prepared header.
    c.Tick(timeout + PbftConfig{}.retry_ms);
    BOOST_CHECK(c.refusals.empty());
    for (uint32_t i{0}; i < 4; ++i) BOOST_CHECK(c.At(i).pbft->Status().decided);
    BOOST_CHECK(c.ApplyDecisions() == candidate.Header().Hash());
}

BOOST_AUTO_TEST_CASE(reports_are_bounded_per_seat_and_replays_are_dropped_before_verification)
{
    // Review of 2026-11-16, R6: reports were kept per attacker-chosen target view with no cap, and every report and vote
    // was BLS-verified before the duplicate check. Seat 1 names a hundred targets to seat 0.
    Cluster c{4};
    const uint256 domain{c.Domain()};
    const auto report_from{[&](uint32_t seat, uint64_t target, bool valid) {
        ViewChange report;
        report.epoch = 0;
        report.slot = 1;
        report.new_view = target;
        report.seat = seat;
        const uint256 digest{ViewChangeDigest(domain, report)};
        report.signature = BlsKeyFromLabel(SeatLabel("s" + std::to_string(seat))).Sign(std::span<const unsigned char>{digest.data(), digest.size()}).Compressed();
        if (!valid) report.signature.front() ^= 1;
        return EncodeMessage(Message{report}, c.At(0).rig->profile).value();
    }};
    Seat& seat0{c.At(0)};
    for (uint64_t target{2}; target <= 101; ++target) RequireOk(seat0.pbft->OnMessage(1, report_from(1, target, true)), "a report");
    PbftStatus status{seat0.pbft->Status()};
    BOOST_CHECK_EQUAL(status.reports_held, 1U); // the highest target only
    BOOST_CHECK_EQUAL(status.view, 0U);
    BOOST_CHECK(!status.changing);
    // A lower target from that seat after a higher one is stale.
    RequireOk(seat0.pbft->OnMessage(1, report_from(1, 50, true)), "a stale lower target");
    BOOST_CHECK_EQUAL(seat0.pbft->Status().reports_held, 1U);
    // The held report again with a broken signature: known (seat, target), dropped before any verification.
    RequireOk(seat0.pbft->OnMessage(1, report_from(1, 101, false)), "a replay with a broken signature");
    // A report not held with a broken signature is still refused.
    const Result<void> forged{seat0.pbft->OnMessage(2, report_from(2, 101, false))};
    BOOST_REQUIRE(!forged.has_value());
    BOOST_CHECK(forged.error().code == ErrorCode::VERIFY_FAILED);
    BOOST_CHECK(forged.error().detail == PBFT_REPORT_SIGNATURE);
    BOOST_CHECK_EQUAL(seat0.pbft->Status().reports_held, 1U);
    // f + 1 = 2 reports for target 101 synchronise seat 0 into CHANGING 101; nothing below the target is kept.
    RequireOk(seat0.pbft->OnMessage(2, report_from(2, 101, true)), "the second report");
    status = seat0.pbft->Status();
    BOOST_CHECK_EQUAL(status.view, 101U);
    BOOST_CHECK(status.changing);
    BOOST_CHECK_EQUAL(status.reports_held, 3U); // seats 1 and 2 and its own
}

BOOST_AUTO_TEST_SUITE_END()
