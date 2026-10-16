// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** The REGTEST k = 1 dev certifier (slice R10, fm2_dev; ARCHITECTURE.md 7.3
 * and 8): the exact records and barrier order of one certified slot, the
 * certificate against bls::FastAggregateVerify, the network and seat-count
 * refusals, the authority checks before Propose and during the intent
 * barrier, and storage failures at each barrier.
 */

#include <boost/test/unit_test.hpp>

#include <crypto/bls.h>
#include <flowmesh2/agreement/agreement.h>
#include <flowmesh2/chain/chain_adapter.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/dev/dev_certifier.h>
#include <flowmesh2/dev/fake_chain.h>
#include <flowmesh2/state/state_owner.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/record.h>
#include <flowmesh2/storage/signer_log.h>
#include <flowmesh2/test/util/agreement_fixtures.h>

#include <memory>
#include <string>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;
using dev::DevSingleSeatCertifier;

namespace {

std::unique_ptr<DevSingleSeatCertifier> NewCertifier(AgreementRig& rig, chain::ChainAdapter* adapter = nullptr)
{
    Result<std::unique_ptr<DevSingleSeatCertifier>> certifier{DevSingleSeatCertifier::Create(
        rig.profile, adapter != nullptr ? *adapter : *rig.adapter, rig.seats, rig.signer, rig.signer_log, rig.commit_log)};
    RequireOk(certifier, "DevSingleSeatCertifier::Create");
    return std::move(certifier).value();
}

std::vector<storage::RecordType> DurableTypes(const storage::MemoryJournal& journal)
{
    Result<std::vector<storage::JournalRecord>> records{journal.ReadAll()};
    RequireOk(records, "ReadAll");
    std::vector<storage::RecordType> types;
    for (const auto& record : *records) types.push_back(record.type);
    return types;
}

std::string Types(const std::vector<storage::RecordType>& types)
{
    std::string out;
    for (const auto type : types) out += std::string{storage::ToString(type)} + " ";
    return out;
}

//! The events of one certified slot s of the rig, in barrier order.
std::vector<std::string> SlotEvents(uint64_t signer_seq, uint64_t commit_seq)
{
    const std::string a{std::to_string(signer_seq + 1)};
    const std::string i{std::to_string(signer_seq + 2)};
    const std::string v{std::to_string(signer_seq + 3)};
    const std::string d{std::to_string(commit_seq + 1)};
    return {"append SIGNER 1 ACCEPTED_PROPOSAL " + a, "append SIGNER 1 INTENT " + i, "barrier-start SIGNER 1 " + i,
            "barrier-end SIGNER 1 " + i + " ok",       "append SIGNER 1 SIGNED_VOTE " + v, "barrier-start SIGNER 1 " + v,
            "barrier-end SIGNER 1 " + v + " ok",       "append COMMIT 2 DECISION " + d,   "barrier-start COMMIT 2 " + d,
            "barrier-end COMMIT 2 " + d + " ok"};
}

//! True iff `cert`'s aggregate verifies with the keys of `seats` over `digest`.
bool AggregateVerifies(const SeatSet& seats, const SlotCertificate& cert, const uint256& digest)
{
    const std::optional<bls::Signature> sig{bls::Signature::Decode(cert.aggregate)};
    if (!sig.has_value()) return false;
    return bls::FastAggregateVerify(seats.Keys(), std::span<const unsigned char>{digest.data(), digest.size()}, *sig);
}

//! Apply `decision` of `candidate` on the rig's owner under `authority`, then APPLIED and B-APPLIED.
void ApplyDecided(AgreementRig& rig, state::Candidate candidate, const agreement::Decision& decision, const AuthorityVersion& authority)
{
    Result<std::shared_ptr<const state::HeadView>> head{rig.owner->Apply(std::move(candidate), decision.decision, authority)};
    RequireOk(head, "Apply");
    BOOST_CHECK((*head)->CertHash() == decision.certificate.Hash());
    const Result<storage::PendingApplied> applied{rig.commit_log.AppendApplied(decision.decision, (*head)->Header().state_root)};
    RequireOk(applied, "AppendApplied");
    RequireStorageOk(rig.commit_log.Barrier(), "B-APPLIED");
    RequireStorageOk(rig.commit_log.Confirm(*applied), "Confirm APPLIED");
}

//! An adapter that reports a non-REGTEST network (TEST double; nothing else is used).
class OtherNetworkAdapter final : public chain::ChainAdapter
{
public:
    ChainType chain{ChainType::MAIN};
    AuthorityVersion authority;
    NetworkId Network() const override
    {
        NetworkId network{TestNetwork()};
        network.chain = chain;
        return network;
    }
    AuthorityVersion CurrentAuthority() const override { return authority; }
    Result<chain::PollReply> Poll(const FactCursor&, size_t) override { return Error{ErrorCode::INVALID_INPUT, "unused"}; }
    Result<void> VerifyAnchor(const Anchor&, const Anchor&) const override { return Error{ErrorCode::INVALID_INPUT, "unused"}; }
    Result<SeatSet> SeatSetAt(uint64_t) const override { return Error{ErrorCode::INVALID_INPUT, "unused"}; }
};

} // namespace

BOOST_AUTO_TEST_SUITE(dev_certifier_tests)

BOOST_AUTO_TEST_CASE(one_certified_slot_writes_exactly_four_records_in_barrier_order)
{
    AgreementRig rig;
    const auto certifier{NewCertifier(rig)};
    state::Candidate candidate{rig.Preview(rig.Inputs(rig.authority, 2))};
    const SlotHeaderV2 header{candidate.Header()};
    RequireOk(certifier->Propose(candidate, rig.authority), "Propose");

    const std::vector<std::string> first_events{SlotEvents(0, 0)};
    BOOST_CHECK_EQUAL_COLLECTIONS(rig.recorder.events.begin(), rig.recorder.events.end(), first_events.begin(), first_events.end());
    using storage::RecordType;
    const std::vector<RecordType> signer_types{RecordType::ACCEPTED_PROPOSAL, RecordType::INTENT, RecordType::SIGNED_VOTE};
    const std::vector<RecordType> commit_types{RecordType::DECISION};
    BOOST_CHECK_MESSAGE(DurableTypes(*rig.signer_journal) == signer_types, Types(DurableTypes(*rig.signer_journal)));
    BOOST_CHECK_MESSAGE(DurableTypes(*rig.commit_journal) == commit_types, Types(DurableTypes(*rig.commit_journal)));

    // The records say what was decided.
    const auto signer_records{rig.signer_journal->ReadAll().value()};
    const auto commit_records{rig.commit_journal->ReadAll().value()};
    const storage::IntentRecord intent{storage::IntentRecord::Decode(signer_records[1].payload).value()};
    BOOST_CHECK(intent.key == (storage::VoteKey{header.domain, header.authority_epoch, header.height, 0, storage::VotePhase::COMMIT}));
    BOOST_CHECK(intent.digest == header.CommitDigest());
    const storage::SignedVoteRecord vote{storage::SignedVoteRecord::Decode(signer_records[2].payload).value()};
    BOOST_CHECK(vote.key == intent.key);
    BOOST_CHECK_EQUAL(vote.intent_seq, 2U);
    const storage::DecisionRecord decided{storage::DecisionRecord::Decode(commit_records[0].payload, rig.profile).value()};
    BOOST_CHECK(decided.header == header);
    BOOST_CHECK(decided.body == candidate.Body());

    // The decision: once, bound to the header and to the certificate its record holds.
    std::optional<agreement::Decision> decision{certifier->TakeDecision()};
    BOOST_REQUIRE(decision.has_value());
    BOOST_CHECK(!certifier->TakeDecision().has_value());
    BOOST_CHECK_EQUAL(decision->decision.Slot(), header.height);
    BOOST_CHECK(decision->decision.Digest() == header.Hash());
    BOOST_CHECK(decision->decision.Binding().cert_hash == decision->certificate.Hash());
    BOOST_CHECK(decision->certificate == decided.certificate);
    BOOST_CHECK(decision->body == candidate.Body());

    // The certificate: seat 0 alone, verifying with FastAggregateVerify over CommitDigest only.
    const SlotCertificate& cert{decision->certificate};
    BOOST_CHECK(cert.header_hash == header.Hash());
    BOOST_CHECK(cert.bitmap == (std::vector<unsigned char>{0x01}));
    BOOST_CHECK(cert.SignerIndices(1).value() == (std::vector<uint32_t>{0}));
    BOOST_CHECK(cert.aggregate == vote.signature);
    BOOST_CHECK(AggregateVerifies(rig.seats, cert, header.CommitDigest()));
    BOOST_CHECK(!AggregateVerifies(rig.seats, cert, header.Hash()));
    BOOST_CHECK(!AggregateVerifies(TestSeats({"s1"}), cert, header.CommitDigest()));
    SlotHeaderV2 tampered{header};
    tampered.state_root = LabelHash("fm2/test/tampered");
    BOOST_CHECK(!AggregateVerifies(rig.seats, cert, tampered.CommitDigest()));

    // Check 3 and B-APPLIED belong to the caller; the next slot links to this certificate.
    ApplyDecided(rig, std::move(candidate), *decision, rig.authority);
    BOOST_CHECK_EQUAL(rig.owner->Head()->Header().height, 1U);
    rig.recorder.events.clear();
    state::Candidate second{rig.Preview(rig.Inputs(rig.authority))};
    BOOST_CHECK(second.Header().parent_cert_hash == cert.Hash());
    BOOST_CHECK(second.Header().parent_hash == header.Hash());
    RequireOk(certifier->Propose(second, rig.authority), "Propose slot 2");
    std::vector<std::string> expected{SlotEvents(3, 2)};
    BOOST_CHECK_EQUAL_COLLECTIONS(rig.recorder.events.begin(), rig.recorder.events.end(), expected.begin(), expected.end());
    std::optional<agreement::Decision> second_decision{certifier->TakeDecision()};
    BOOST_REQUIRE(second_decision.has_value());
    BOOST_CHECK(AggregateVerifies(rig.seats, second_decision->certificate, second.Header().CommitDigest()));
    ApplyDecided(rig, std::move(second), *second_decision, rig.authority);
    BOOST_CHECK_EQUAL(rig.owner->Head()->Header().height, 2U);
}

BOOST_AUTO_TEST_CASE(create_refuses_other_networks_and_seat_counts)
{
    AgreementRig rig;
    ProfileParams mainnet_profile{rig.profile};
    mainnet_profile.chain = ChainType::MAIN;
    RequireCode(DevSingleSeatCertifier::Create(mainnet_profile, *rig.adapter, rig.seats, rig.signer, rig.signer_log, rig.commit_log),
                ErrorCode::UNSUPPORTED_NETWORK, "non-REGTEST profile");
    for (const ChainType chain_type : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4, ChainType::SIGNET}) {
        OtherNetworkAdapter adapter;
        adapter.chain = chain_type;
        adapter.authority = rig.authority;
        RequireRefusal(DevSingleSeatCertifier::Create(rig.profile, adapter, rig.seats, rig.signer, rig.signer_log, rig.commit_log),
                       ErrorCode::UNSUPPORTED_NETWORK, dev::DEV_NETWORK, "adapter of a real network");
    }
    {
        // k = 2: refused even though seat 0 is this signer's key.
        AgreementRig two{RigOptions{{"s0", "s1"}, {}, {}}};
        RequireRefusal(DevSingleSeatCertifier::Create(two.profile, *two.adapter, two.seats, two.signer, two.signer_log, two.commit_log),
                       ErrorCode::INVALID_INPUT, dev::DEV_SEAT_COUNT, "k = 2");
    }
    RequireRefusal(DevSingleSeatCertifier::Create(rig.profile, *rig.adapter, TestSeats({"s1"}), rig.signer, rig.signer_log, rig.commit_log),
                   ErrorCode::INVALID_INPUT, dev::DEV_SEAT_KEY, "signer key of another seat");
    BOOST_CHECK(rig.recorder.events.empty());
    BOOST_CHECK(NewCertifier(rig)->Seats().Root() == rig.seats.Root());
}

BOOST_AUTO_TEST_CASE(authority_bumped_before_propose_writes_nothing)
{
    AgreementRig rig;
    const auto certifier{NewCertifier(rig)};
    const state::Candidate candidate{rig.Preview(rig.Inputs(rig.authority, 1))};
    rig.adapter->BumpAuthority();
    RequireRefusal(certifier->Propose(candidate, rig.authority), ErrorCode::AUTHORITY_CHANGED, chain::CHAIN_AUTHORITY_CHANGED,
                   "authority check 1");
    BOOST_CHECK(rig.recorder.events.empty());
    BOOST_CHECK_EQUAL(rig.signer_journal->Status().appended_seq, 0U);
    BOOST_CHECK_EQUAL(rig.commit_journal->Status().appended_seq, 0U);
    BOOST_CHECK(!certifier->TakeDecision().has_value());
}

BOOST_AUTO_TEST_CASE(authority_bumped_during_the_intent_barrier_leaves_a_durable_intent_and_no_signature)
{
    AgreementRig rig;
    const auto certifier{NewCertifier(rig)};
    const state::Candidate candidate{rig.Preview(rig.Inputs(rig.authority, 1))};
    RequireOk(rig.adapter->BumpAt(dev::HookPoint::SIGNER_BARRIER_END, 1), "arm the hook on B-INTENT");
    RequireRefusal(certifier->Propose(candidate, rig.authority), ErrorCode::AUTHORITY_CHANGED, chain::CHAIN_AUTHORITY_CHANGED,
                   "authority check 2");

    // The INTENT is durable; no SIGNED_VOTE, no DECISION, no decision.
    using storage::RecordType;
    const std::vector<RecordType> durable{RecordType::ACCEPTED_PROPOSAL, RecordType::INTENT};
    BOOST_CHECK_MESSAGE(DurableTypes(*rig.signer_journal) == durable, Types(DurableTypes(*rig.signer_journal)));
    BOOST_CHECK(rig.signer_journal->Status() == (storage::JournalStatus{2, 2, false}));
    BOOST_CHECK_EQUAL(rig.commit_journal->Status().appended_seq, 0U);
    BOOST_CHECK(!certifier->TakeDecision().has_value());
    const AuthorityVersion bumped{rig.adapter->CurrentAuthority()};
    BOOST_CHECK_EQUAL(bumped.epoch, rig.authority.epoch + 1);

    // Another digest for the same (epoch, slot, view) is refused for good, here
    // through a second certifier whose adapter never saw the bump.
    Result<std::unique_ptr<dev::FakeChainAdapter>> stale{
        dev::FakeChainAdapter::Create(TestNetwork(), rig.authority, rig.seats, TestAnchor(129))};
    RequireOk(stale, "stale adapter");
    const auto stale_certifier{NewCertifier(rig, stale->get())};
    const state::Candidate other{rig.Preview(rig.Inputs(rig.authority, 1, "other"))};
    BOOST_REQUIRE(other.Header().height == candidate.Header().height);
    BOOST_REQUIRE(other.Header().CommitDigest() != candidate.Header().CommitDigest());
    RequireRefusal(stale_certifier->Propose(other, rig.authority), ErrorCode::EQUIVOCATION_REFUSED, storage::PROPOSAL_CONFLICT,
                   "another digest for (epoch 0, slot 1, view 0)");
    BOOST_CHECK_EQUAL(rig.signer_journal->Status().appended_seq, 2U);

    // The same candidate under the old authority is refused at check 1 and writes nothing.
    RequireRefusal(certifier->Propose(candidate, rig.authority), ErrorCode::AUTHORITY_CHANGED, chain::CHAIN_AUTHORITY_CHANGED,
                   "retry under the old authority");
    BOOST_CHECK_EQUAL(rig.signer_journal->Status().appended_seq, 2U);

    // Under the new epoch the slot is a new VoteKey and is certified.
    state::Candidate renewed{rig.Preview(rig.Inputs(bumped, 1))};
    BOOST_CHECK_EQUAL(renewed.Header().authority_epoch, bumped.epoch);
    RequireOk(certifier->Propose(renewed, bumped), "Propose under the new authority");
    std::optional<agreement::Decision> decision{certifier->TakeDecision()};
    BOOST_REQUIRE(decision.has_value());
    BOOST_CHECK(AggregateVerifies(rig.seats, decision->certificate, renewed.Header().CommitDigest()));
    const std::vector<RecordType> after{RecordType::ACCEPTED_PROPOSAL, RecordType::INTENT, RecordType::ACCEPTED_PROPOSAL,
                                        RecordType::INTENT, RecordType::SIGNED_VOTE};
    BOOST_CHECK_MESSAGE(DurableTypes(*rig.signer_journal) == after, Types(DurableTypes(*rig.signer_journal)));
    ApplyDecided(rig, std::move(renewed), *decision, bumped);
}

BOOST_AUTO_TEST_CASE(failed_signed_vote_barrier_is_storage_fatal_and_writes_no_decision)
{
    AgreementRig rig{RigOptions{{"s0"}, storage::MemoryJournalFaults{0, 2}, {}}};
    const auto certifier{NewCertifier(rig)};
    const state::Candidate candidate{rig.Preview(rig.Inputs(rig.authority, 1))};
    RequireCode(certifier->Propose(candidate, rig.authority), ErrorCode::STORAGE_FATAL, "B-VOTE");
    BOOST_CHECK(rig.signer_journal->IsFatal());
    BOOST_CHECK(rig.signer_journal->Status() == (storage::JournalStatus{3, 2, true}));
    using storage::RecordType;
    const std::vector<RecordType> durable{RecordType::ACCEPTED_PROPOSAL, RecordType::INTENT};
    BOOST_CHECK_MESSAGE(DurableTypes(*rig.signer_journal) == durable, Types(DurableTypes(*rig.signer_journal)));
    BOOST_CHECK_EQUAL(rig.commit_journal->Status().appended_seq, 0U);
    BOOST_CHECK(!certifier->TakeDecision().has_value());
    // Fenced from now on: nothing new is written.
    const state::Candidate next{rig.Preview(rig.Inputs(rig.authority, 1, "next"))};
    RequireRefusal(certifier->Propose(next, rig.authority), ErrorCode::FENCED, dev::DEV_FENCED, "after a fatal signer journal");
    BOOST_CHECK_EQUAL(rig.signer_journal->Status().appended_seq, 3U);
}

BOOST_AUTO_TEST_CASE(failed_intent_barrier_leaves_no_signature)
{
    AgreementRig rig{RigOptions{{"s0"}, storage::MemoryJournalFaults{0, 1}, {}}};
    const auto certifier{NewCertifier(rig)};
    const state::Candidate candidate{rig.Preview(rig.Inputs(rig.authority, 1))};
    RequireCode(certifier->Propose(candidate, rig.authority), ErrorCode::STORAGE_FATAL, "B-INTENT");
    BOOST_CHECK(rig.signer_journal->Status() == (storage::JournalStatus{2, 0, true}));
    BOOST_CHECK(DurableTypes(*rig.signer_journal).empty());
    BOOST_CHECK_EQUAL(rig.commit_journal->Status().appended_seq, 0U);
    BOOST_CHECK(!certifier->TakeDecision().has_value());
    // Fenced from now on: nothing new is written.
    const state::Candidate next{rig.Preview(rig.Inputs(rig.authority, 1, "next"))};
    RequireRefusal(certifier->Propose(next, rig.authority), ErrorCode::FENCED, dev::DEV_FENCED, "after a failed B-INTENT");
    BOOST_CHECK_EQUAL(rig.signer_journal->Status().appended_seq, 2U);
}

BOOST_AUTO_TEST_CASE(failed_decision_barrier_fences_before_the_next_intent)
{
    AgreementRig rig{RigOptions{{"s0"}, {}, storage::MemoryJournalFaults{0, 1}}};
    const auto certifier{NewCertifier(rig)};
    const state::Candidate candidate{rig.Preview(rig.Inputs(rig.authority, 1))};
    RequireCode(certifier->Propose(candidate, rig.authority), ErrorCode::STORAGE_FATAL, "B-DECISION");
    BOOST_CHECK(rig.commit_journal->Status() == (storage::JournalStatus{1, 0, true}));
    BOOST_CHECK(DurableTypes(*rig.commit_journal).empty());
    BOOST_CHECK_EQUAL(DurableTypes(*rig.signer_journal).size(), 3U);
    BOOST_CHECK(!certifier->TakeDecision().has_value());
    // The next Propose writes no new ACCEPTED_PROPOSAL or INTENT.
    const state::Candidate next{rig.Preview(rig.Inputs(rig.authority, 1, "next"))};
    RequireRefusal(certifier->Propose(next, rig.authority), ErrorCode::FENCED, dev::DEV_FENCED, "after a fatal commit journal");
    BOOST_CHECK_EQUAL(rig.signer_journal->Status().appended_seq, 3U);
}

BOOST_AUTO_TEST_CASE(preconditions_messages_and_timeouts)
{
    AgreementRig rig;
    const auto certifier{NewCertifier(rig)};
    const std::vector<unsigned char> bytes{1, 2, 3};
    const Result<void> message{certifier->OnMessage(0, bytes)};
    BOOST_REQUIRE(!message.has_value());
    BOOST_CHECK(message.error().code == ErrorCode::UNSUPPORTED);
    BOOST_CHECK_EQUAL(message.error().detail, "M4-AGR-02");
    BOOST_CHECK(certifier->OnTimeout(123).has_value());
    BOOST_CHECK(!certifier->TakeDecision().has_value());

    const state::Candidate candidate{rig.Preview(rig.Inputs(rig.authority, 1))};
    AuthorityVersion other_epoch{rig.authority};
    other_epoch.epoch += 1;
    RequireRefusal(certifier->Propose(candidate, other_epoch), ErrorCode::INVALID_INPUT, dev::DEV_CANDIDATE_AUTHORITY,
                   "authority other than the candidate's");
    {
        // A candidate of another seat set (its own owner and authority).
        AgreementRig other{RigOptions{{"s1"}, {}, {}}};
        const state::Candidate foreign{other.Preview(other.Inputs(other.authority, 1))};
        RequireRefusal(certifier->Propose(foreign, other.authority), ErrorCode::INVALID_INPUT, dev::DEV_SEAT_SET, "other seat set");
    }
    BOOST_CHECK(rig.recorder.events.empty());

    RequireOk(certifier->Propose(candidate, rig.authority), "Propose");
    const size_t events{rig.recorder.events.size()};
    const state::Candidate next{rig.Preview(rig.Inputs(rig.authority, 1, "next"))};
    RequireRefusal(certifier->Propose(next, rig.authority), ErrorCode::ORDER_VIOLATION, dev::DEV_DECISION_PENDING,
                   "a decision was not taken");
    BOOST_CHECK_EQUAL(rig.recorder.events.size(), events);
    BOOST_CHECK(certifier->OnTimeout(456).has_value());
    BOOST_CHECK_EQUAL(rig.recorder.events.size(), events);
    BOOST_CHECK(certifier->TakeDecision().has_value());
}

BOOST_AUTO_TEST_SUITE_END()
