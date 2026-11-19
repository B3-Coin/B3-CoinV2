// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** The simulated node's halting rules (review of 18051472..ffc43baf;
 * ARCHITECTURE.md O-89, O-90): a fenced node seals nothing; a DECISION that
 * check 3 refuses leaves the node at the last applied slot, which is also
 * what a replay of its commit journal gives, with exactly one signature for
 * the decided height; and an error after the head moved fences the node.
 */

#include <boost/test/unit_test.hpp>

#include <flowmesh2/admission/admission.h>
#include <flowmesh2/admission/pool.h>
#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/net/client_api.h>
#include <flowmesh2/sim/faults.h>
#include <flowmesh2/sim/sim_client.h>
#include <flowmesh2/sim/sim_node.h>
#include <flowmesh2/sim/trace.h>
#include <flowmesh2/state/replay.h>
#include <flowmesh2/state/state_owner.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/record.h>
#include <flowmesh2/storage/signer_log.h>
#include <flowmesh2/test/util/require.h>
#include <flowmesh2/test/util/slot_fixtures.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::sim;
using namespace flowmesh2::test;

namespace {

//! A simulated node over an empty block 130 (every slot is a heartbeat).
struct NodeRig {
    TraceRecorder trace;
    std::unique_ptr<SimNode> node;

    explicit NodeRig(const SimNodeConfig& config = {})
    {
        Result<std::unique_ptr<SimNode>> created{SimNode::Create(config, trace)};
        RequireOk(created, "SimNode::Create");
        node = std::move(created).value();
        RequireOk(node->Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 1), {}), "AppendBlock");
    }

    //! The P3 steps of every trace event from index `from` on.
    std::vector<std::string> StepsFrom(size_t from) const
    {
        const std::vector<TraceEvent> events{trace.Events()};
        std::vector<std::string> steps;
        for (size_t i{from}; i < events.size(); ++i) steps.push_back(events[i].Step());
        return steps;
    }

    //! Durable SIGNED_VOTE records of the signer journal for `height`.
    size_t SignedVotesAt(uint64_t height) const
    {
        const Result<std::vector<storage::JournalRecord>> records{node->SignerJournal().ReadAll()};
        RequireOk(records, "signer ReadAll");
        size_t votes{0};
        for (const storage::JournalRecord& record : *records) {
            if (record.type != storage::RecordType::SIGNED_VOTE) continue;
            const Result<storage::SignedVoteRecord> vote{storage::SignedVoteRecord::Decode(record.payload)};
            RequireOk(vote, "SignedVoteRecord::Decode");
            votes += vote->key.slot == height ? 1 : 0;
        }
        return votes;
    }
};

template <class T>
void RequireCodeAndDetail(const Result<T>& result, ErrorCode code, std::string_view detail, std::string_view what)
{
    if (result.has_value()) BOOST_FAIL(std::string{what} + " unexpectedly succeeded");
    BOOST_CHECK_MESSAGE(result.error().code == code && result.error().detail == detail,
                        std::string{what} + ": got " + std::string{ToString(result.error().code)} + " '" +
                            std::string{result.error().detail} + "'");
}

} // namespace

BOOST_AUTO_TEST_SUITE(sim_node_tests)

BOOST_AUTO_TEST_CASE(a_fenced_node_seals_nothing)
{
    // Check 1 refuses slot 2 and fences the node. Without a fence on
    // sealing, the next SealSlot polled, took and previewed again and went
    // back to the certifier (here the plan bumps again at slot 2).
    SimNodeConfig config;
    config.faults.bump_authority_at = 2;
    NodeRig rig{config};
    RequireOk(rig.node->SealSlot(), "SealSlot 1");
    RequireError(rig.node->SealSlot(), ErrorCode::AUTHORITY_CHANGED, "SealSlot 2 (check 1)");
    BOOST_REQUIRE(rig.node->Fenced());

    const storage::JournalStatus signer_before{rig.node->SignerJournal().Status()};
    const storage::JournalStatus commit_before{rig.node->CommitJournal().Status()};
    const size_t events{rig.trace.Size()};
    const uint64_t polls_before{rig.node->Chain().Reached(dev::HookPoint::POLL)};
    RequireCodeAndDetail(rig.node->SealSlot(), ErrorCode::FENCED, SIM_FENCED, "SealSlot while fenced");
    BOOST_CHECK(rig.node->SignerJournal().Status() == signer_before);
    BOOST_CHECK(rig.node->CommitJournal().Status() == commit_before);
    BOOST_CHECK_EQUAL(rig.node->Chain().Reached(dev::HookPoint::POLL), polls_before);
    // Only the slot window: no poll, take, preview, signature or write.
    const std::vector<std::string> window{rig.StepsFrom(events)};
    const std::vector<std::string> expected{"slot.begin", "slot.end"};
    BOOST_CHECK_EQUAL_COLLECTIONS(window.begin(), window.end(), expected.begin(), expected.end());
    BOOST_CHECK_EQUAL(rig.node->Owner().Head()->Header().height, 1U);
    BOOST_CHECK(rig.node->Read().PublishedHeight() == std::optional<uint64_t>{1});
    BOOST_CHECK_EQUAL(rig.node->Sealed().size(), 1U);
    BOOST_CHECK_EQUAL(rig.SignedVotesAt(2), 0U);
}

BOOST_AUTO_TEST_CASE(a_decision_refused_at_check_3_halts_the_node_and_replay_agrees)
{
    // The authority moves after slot 2's DECISION is durable: check 3 refuses it.
    SimNodeConfig config;
    config.faults.bump_authority_before_apply_at = 2;
    NodeRig rig{config};
    RequireOk(rig.node->SealSlot(), "SealSlot 1");
    RequireCodeAndDetail(rig.node->SealSlot(), ErrorCode::AUTHORITY_CHANGED, state::AUTHORITY_SUPERSEDED, "SealSlot 2 (check 3)");
    BOOST_REQUIRE(rig.node->Fenced());
    const std::shared_ptr<const state::HeadView> live{rig.node->Owner().Head()};
    BOOST_CHECK_EQUAL(live->Header().height, 1U);
    BOOST_CHECK(rig.node->Read().PublishedHeight() == std::optional<uint64_t>{1});
    BOOST_CHECK_EQUAL(rig.SignedVotesAt(2), 1U);

    // DECISION(2) is durable and APPLIED(2) is not.
    const Result<std::vector<storage::JournalRecord>> records{rig.node->CommitJournal().ReadAll()};
    RequireOk(records, "commit ReadAll");
    BOOST_REQUIRE_EQUAL(records->size(), 3U); // DECISION 1, APPLIED 1, DECISION 2
    BOOST_CHECK(records->back().type == storage::RecordType::DECISION);
    BOOST_CHECK_EQUAL(storage::DecisionRecord::Decode(records->back().payload, rig.node->Profile()).value().header.height, 2U);

    // Replay of the commit journal gives the live head; slot 2 is decided, not committed (O-89).
    const Result<state::ReplayResult> replay{state::ReplayDecisions(rig.node->Genesis(), *records)};
    RequireOk(replay, "ReplayDecisions");
    BOOST_CHECK(replay->head.Header() == live->Header());
    BOOST_CHECK(replay->head.HeaderHash() == live->HeaderHash());
    BOOST_CHECK(replay->head.CertHash() == live->CertHash());
    BOOST_CHECK(replay->head.State() == live->State());
    BOOST_CHECK_EQUAL(replay->decided_head.Header().height, 2U);
    BOOST_REQUIRE_EQUAL(replay->slots.size(), 2U);
    BOOST_CHECK(!replay->slots[1].applied_seq.has_value());

    // The halted node never signs or decides another header for height 2.
    const storage::JournalStatus signer_before{rig.node->SignerJournal().Status()};
    const storage::JournalStatus commit_before{rig.node->CommitJournal().Status()};
    RequireCodeAndDetail(rig.node->SealSlot(), ErrorCode::FENCED, SIM_FENCED, "SealSlot after the check-3 refusal");
    BOOST_CHECK(rig.node->SignerJournal().Status() == signer_before);
    BOOST_CHECK(rig.node->CommitJournal().Status() == commit_before);
    BOOST_CHECK_EQUAL(rig.SignedVotesAt(2), 1U);
    BOOST_CHECK_EQUAL(rig.node->Owner().Head()->Header().height, 1U);
}

BOOST_AUTO_TEST_CASE(an_error_after_the_head_moved_fences_the_node)
{
    // Dave's PLACE enters the pool directly (admission A4 would refuse an
    // account that does not exist) and is withheld from every proposal, so
    // OnCommitted finds a live entry without an account: INVARIANT, after
    // Apply, B-APPLIED and publication. Without the fence the node kept
    // admitting, and every later SealSlot failed at TakeForSlot.
    const AccountId dave{SimClient::AccountOf("dave").value()};
    SimNodeConfig config;
    config.faults.withhold_from_proposals.insert({dave, 1});
    NodeRig rig{config};
    const std::vector<MarketDescriptor> markets{rig.node->Markets().begin(), rig.node->Markets().end()};
    Result<SimClient> client{SimClient::Create("dave", rig.node->Profile(), rig.node->Domain(), rig.node->Seats(), markets)};
    RequireOk(client, "SimClient::Create");
    const Result<std::vector<unsigned char>> bytes{client->Place("x", 1, SimMarketM1(), Side::BUY, TimeInForce::GTC, 250, 1, 0)};
    RequireOk(bytes, "Place");
    Result<ActionV2> action{ActionV2::Decode(*bytes, rig.node->Profile())};
    RequireOk(action, "ActionV2::Decode");
    Result<VerifiedAction> verified{VerifiedAction::Verify(std::move(action).value())};
    RequireOk(verified, "VerifiedAction::Verify");
    BOOST_REQUIRE(rig.node->Pool().Insert(*verified, 0).result == admission::InsertResult::INSERTED);

    RequireCodeAndDetail(rig.node->SealSlot(), ErrorCode::INVARIANT, admission::POOL_ACCOUNT_MISSING, "SealSlot 1 (late error)");
    BOOST_CHECK_EQUAL(rig.node->Owner().Head()->Header().height, 1U);
    BOOST_CHECK(rig.node->Read().PublishedHeight() == std::optional<uint64_t>{1});
    BOOST_CHECK(rig.node->Fenced());
    const storage::JournalStatus signer_before{rig.node->SignerJournal().Status()};
    RequireCodeAndDetail(rig.node->SealSlot(), ErrorCode::FENCED, SIM_FENCED, "SealSlot after the late error");
    BOOST_CHECK(rig.node->SignerJournal().Status() == signer_before);
    const Result<std::vector<unsigned char>> later{client->Place("y", 2, SimMarketM1(), Side::BUY, TimeInForce::GTC, 250, 1, 1)};
    RequireOk(later, "Place y");
    const Result<admission::AdmissionReply> reply{rig.node->Admitter().Admit(*later)};
    RequireOk(reply, "Admit");
    BOOST_CHECK(reply->code == AdmissionCode::HALTED);
}

BOOST_AUTO_TEST_CASE(a_restart_applies_a_decided_but_unapplied_slot)
{
    // Review of 2026-11-16, defect 8: a crash between B-DECISION and B-APPLIED (a window open on every slot) left a
    // node that could not restart: Restore answered NotImplemented for the decided-but-unapplied tail. The restart
    // now re-executes that slot, runs check 3 under the current authority, writes APPLIED and publishes it.
    SimNodeConfig config;
    config.faults.crash_after_decision_at = 2;
    NodeRig rig{config};
    RequireOk(rig.node->SealSlot(), "SealSlot 1");
    RequireCodeAndDetail(rig.node->SealSlot(), ErrorCode::STORAGE_FATAL, SIM_CRASH_AFTER_DECISION, "SealSlot 2 (the crash)");
    BOOST_REQUIRE(rig.node->Fenced());
    BOOST_CHECK_EQUAL(rig.node->Owner().Head()->Header().height, 1U);
    {
        const Result<std::vector<storage::JournalRecord>> records{rig.node->CommitJournal().ReadAll()};
        RequireOk(records, "commit ReadAll");
        BOOST_REQUIRE_EQUAL(records->size(), 3U); // DECISION 1, APPLIED 1, DECISION 2
        BOOST_CHECK(records->back().type == storage::RecordType::DECISION);
    }
    const size_t sealed_before{rig.node->Sealed().size()};
    RequireOk(rig.node->Restart(), "Restart");
    BOOST_CHECK(!rig.node->Fenced());
    BOOST_CHECK_EQUAL(rig.node->Owner().Head()->Header().height, 2U);
    BOOST_CHECK(rig.node->Read().PublishedHeight() == std::optional<uint64_t>{2});
    BOOST_CHECK_EQUAL(rig.node->Sealed().size(), sealed_before + 1);
    {
        const Result<std::vector<storage::JournalRecord>> records{rig.node->CommitJournal().ReadAll()};
        RequireOk(records, "commit ReadAll after the restart");
        BOOST_REQUIRE_EQUAL(records->size(), 4U); // ... APPLIED 2
        BOOST_CHECK(records->back().type == storage::RecordType::APPLIED);
        BOOST_CHECK_EQUAL(storage::AppliedRecord::Decode(records->back().payload).value().slot, 2U);
    }
    // The node goes on from there.
    RequireOk(rig.node->SealSlot(), "SealSlot 3");
    BOOST_CHECK_EQUAL(rig.node->Owner().Head()->Header().height, 3U);
    BOOST_CHECK_EQUAL(rig.SignedVotesAt(2), 1U); // slot 2 was never re-decided
}

BOOST_AUTO_TEST_CASE(a_restart_after_a_check_3_refusal_keeps_the_live_head_and_stays_fenced)
{
    // The other way into the same journal state: slot 2's DECISION durable, check 3 refused under a moved authority.
    // The restart re-runs check 3, which refuses again; the head stays at slot 1, nothing is applied or published for
    // slot 2, and the node starts fenced (O-89: the height is never re-decided with another header).
    SimNodeConfig config;
    config.faults.bump_authority_before_apply_at = 2;
    NodeRig rig{config};
    RequireOk(rig.node->SealSlot(), "SealSlot 1");
    RequireCodeAndDetail(rig.node->SealSlot(), ErrorCode::AUTHORITY_CHANGED, state::AUTHORITY_SUPERSEDED, "SealSlot 2 (check 3)");
    BOOST_REQUIRE(rig.node->Fenced());
    RequireOk(rig.node->Restart(), "Restart");
    BOOST_CHECK(rig.node->Fenced());
    BOOST_CHECK_EQUAL(rig.node->Owner().Head()->Header().height, 1U);
    // The read service is rebuilt empty on every restart and publishes from the next applied slot on (M4-STO-01); nothing
    // was applied here, so nothing is published, and slot 2 in particular never was.
    BOOST_CHECK(!rig.node->Read().PublishedHeight().has_value());
    const Result<std::vector<storage::JournalRecord>> records{rig.node->CommitJournal().ReadAll()};
    RequireOk(records, "commit ReadAll");
    BOOST_CHECK_EQUAL(records->size(), 3U); // no APPLIED 2
    RequireCodeAndDetail(rig.node->SealSlot(), ErrorCode::FENCED, SIM_FENCED, "SealSlot after the restart");
    BOOST_CHECK_EQUAL(rig.SignedVotesAt(2), 1U);
}

BOOST_AUTO_TEST_CASE(an_action_that_breaks_its_slot_is_left_out_and_the_slot_seals)
{
    // M4-ADM-03 (O-34, O-90): bob holds MAX_LEAF_ASSETS assets, so alice's TRANSFER of rUSD to him makes the ledger
    // refuse the whole slot (OVERSIZE, no candidate). That error does not fence; the action stayed IN_PROPOSAL, was
    // taken again by every later slot, and no slot sealed again, carol's good transfer never included. The node now
    // leaves that one action out: the slot seals with carol's transfer, alice's is terminal EXCLUDED, and the API
    // answers UNKNOWN for a terminal pool status instead of PENDING. A slot that a chain fact breaks excludes nothing.
    TraceRecorder trace;
    Result<std::unique_ptr<SimNode>> created{SimNode::Create(SimNodeConfig{}, trace)};
    RequireOk(created, "SimNode::Create");
    SimNode& node{**created};
    const std::vector<MarketDescriptor> markets{node.Markets().begin(), node.Markets().end()};
    const auto client = [&](const std::string& name) {
        Result<SimClient> made{SimClient::Create(name, node.Profile(), node.Domain(), node.Seats(), markets)};
        RequireOk(made, "SimClient::Create " + name);
        return std::move(made).value();
    };
    SimClient alice{client("alice")};
    SimClient carol{client("carol")};
    const AccountId bob{SimClient::AccountOf("bob").value()};
    const AssetId rusd{SimAsset("rUSD")};
    const auto deposit = [](std::vector<Fact>& facts, uint32_t height, const AccountId& to, const AssetId& asset, Atoms amount) {
        DepositFact fact;
        fact.cursor = FactCursor{height, static_cast<uint32_t>(facts.size())};
        fact.outpoint = Outpoint{SimLabelHash("fm2/test/sim_node/adm03/" + std::to_string(height) + "/" + std::to_string(facts.size())), 0};
        fact.account = to;
        fact.asset = asset;
        fact.amount = amount;
        facts.push_back(Fact{fact});
    };
    std::vector<Fact> block1;
    deposit(block1, SIM_BASE_HEIGHT + 1, alice.Account(), rusd, 1'000'000);
    deposit(block1, SIM_BASE_HEIGHT + 1, carol.Account(), rusd, 1'000'000);
    for (size_t i{0}; i < MAX_LEAF_ASSETS; ++i) {
        deposit(block1, SIM_BASE_HEIGHT + 1, bob, SimAsset("fm2/test/adm03/" + std::to_string(i)), 1);
    }
    RequireOk(node.Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 1), std::move(block1)), "AppendBlock 1");
    RequireOk(node.SealSlot(), "SealSlot 1");

    const auto admit = [&](const Result<std::vector<unsigned char>>& bytes, std::string_view what) {
        RequireOk(bytes, std::string{what});
        const Result<admission::AdmissionReply> reply{node.Admitter().Admit(*bytes)};
        RequireOk(reply, "Admit " + std::string{what});
        BOOST_REQUIRE_MESSAGE(reply->code == AdmissionCode::ADMITTED, std::string{what} + " not admitted");
        Result<ActionV2> action{ActionV2::Decode(*bytes, node.Profile())};
        RequireOk(action, "Decode " + std::string{what});
        return std::move(action).value();
    };
    const ActionV2 bad{admit(alice.Transfer("bad", 1, bob, rusd, 5, 1), "alice's transfer to bob")};
    const ActionV2 good{admit(carol.Transfer("good", 1, alice.Account(), rusd, 7, 1), "carol's transfer to alice")};

    const size_t events{trace.Size()};
    const Result<SealReport> sealed{node.SealSlot()};
    RequireOk(sealed, "SealSlot 2 (alice's transfer left out)");
    BOOST_CHECK_EQUAL(sealed->originals, 1U);
    const std::optional<admission::PoolEntryStatus> good_status{node.Pool().Status(good.Id())};
    BOOST_REQUIRE(good_status.has_value());
    BOOST_CHECK(good_status->status == admission::PoolStatus::INCLUDED);
    const read::IncludedRef only_outcome{2, 0}; // the slot's only outcome
    BOOST_CHECK(good_status->included == std::optional<read::IncludedRef>{only_outcome});
    const std::optional<admission::PoolEntryStatus> bad_status{node.Pool().Status(bad.Id())};
    BOOST_REQUIRE(bad_status.has_value());
    BOOST_CHECK(bad_status->status == admission::PoolStatus::EXCLUDED);
    BOOST_CHECK(admission::IsTerminal(admission::PoolStatus::EXCLUDED));
    BOOST_CHECK_EQUAL(bad_status->height, 1U); // the committed head the slot was built on
    BOOST_CHECK_EQUAL(node.Pool().Size(), 0U);
    bool traced{false};
    const std::vector<TraceEvent> all{trace.Events()};
    for (size_t i{events}; i < all.size(); ++i) traced |= all[i].Step() == "pool.exclude";
    BOOST_CHECK(traced);
    const Result<net::ActionStatusReply> answer{node.Api().GetAction(bad)};
    RequireOk(answer, "GetAction bad");
    BOOST_CHECK(answer->status == net::ApiActionStatus::UNKNOWN);
    BOOST_CHECK_EQUAL(answer->reason, net::API_UNKNOWN_TERMINAL_UNPROVEN);
    // Nothing of alice's reached bob, and the node goes on.
    const ledger::AccountState* bob_state{node.Owner().Head()->State().engine.ledger.Find(bob)};
    BOOST_REQUIRE(bob_state != nullptr);
    BOOST_CHECK_EQUAL(bob_state->balances.size(), MAX_LEAF_ASSETS);
    RequireOk(node.SealSlot(), "SealSlot 3");

    // A chain fact that breaks the slot (a 257th asset deposited to bob) is no action's fault: nothing is excluded and
    // the error comes back as before (O-34: certified refusals of facts are M2-LED-07).
    const ActionV2 later{admit(carol.Transfer("later", 2, alice.Account(), rusd, 3, 3), "carol's second transfer")};
    std::vector<Fact> block2;
    deposit(block2, SIM_BASE_HEIGHT + 2, bob, SimAsset("fm2/test/adm03/extra"), 1);
    RequireOk(node.Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 2), std::move(block2)), "AppendBlock 2");
    RequireError(node.SealSlot(), ErrorCode::OVERSIZE, "SealSlot 4 (a fact breaks the slot)");
    BOOST_CHECK(!node.Fenced());
    const std::optional<admission::PoolEntryStatus> later_status{node.Pool().Status(later.Id())};
    BOOST_REQUIRE(later_status.has_value());
    BOOST_CHECK(!admission::IsTerminal(later_status->status));
}

BOOST_AUTO_TEST_SUITE_END()
