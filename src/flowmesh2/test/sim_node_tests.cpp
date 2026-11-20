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
#include <flowmesh2/engine/engine.h>
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
#include <tuple>
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
    // The restart replays the retained committed slots into the read service (M4-STO-03's rebuild): slot 1 is
    // published again, and slot 2, never applied, is not.
    BOOST_CHECK(rig.node->Read().PublishedHeight() == std::optional<uint64_t>{1});
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

//! M4-ADM-03 cases: a k = 1 sim node whose first slot carries the deposits; bob gets MAX_LEAF_ASSETS assets, so a
//! transfer of rUSD to him gives him a 257th, an engine error that one action causes (O-34).
struct Adm03Rig {
    TraceRecorder trace;
    std::unique_ptr<SimNode> node;
    std::vector<MarketDescriptor> markets;
    const AccountId bob{SimClient::AccountOf("bob").value()};
    const AssetId rusd{SimAsset("rUSD")};
    std::vector<Fact> block;

    Adm03Rig()
    {
        Result<std::unique_ptr<SimNode>> created{SimNode::Create(SimNodeConfig{}, trace)};
        RequireOk(created, "SimNode::Create");
        node = std::move(created).value();
        markets.assign(node->Markets().begin(), node->Markets().end());
    }
    SimClient Client(const std::string& name)
    {
        Result<SimClient> made{SimClient::Create(name, node->Profile(), node->Domain(), node->Seats(), markets)};
        RequireOk(made, "SimClient::Create " + name);
        return std::move(made).value();
    }
    void Deposit(const AccountId& to, const AssetId& asset, Atoms amount)
    {
        DepositFact fact;
        fact.cursor = FactCursor{SIM_BASE_HEIGHT + 1, static_cast<uint32_t>(block.size())};
        fact.outpoint = Outpoint{SimLabelHash("fm2/test/sim_node/adm03b/" + std::to_string(block.size())), 0};
        fact.account = to;
        fact.asset = asset;
        fact.amount = amount;
        block.push_back(Fact{fact});
    }
    //! Bob's MAX_LEAF_ASSETS assets join the deposits, and slot 1 seals them all.
    void SealDeposits()
    {
        for (size_t i{0}; i < MAX_LEAF_ASSETS; ++i) Deposit(bob, SimAsset("fm2/test/adm03b/" + std::to_string(i)), 1);
        RequireOk(node->Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 1), std::move(block)), "AppendBlock 1");
        RequireOk(node->SealSlot(), "SealSlot 1");
    }
    ActionV2 Admit(const Result<std::vector<unsigned char>>& bytes, std::string_view what)
    {
        RequireOk(bytes, std::string{what});
        return Admit(*bytes, what);
    }
    ActionV2 Admit(const std::vector<unsigned char>& bytes, std::string_view what)
    {
        const Result<admission::AdmissionReply> reply{node->Admitter().Admit(bytes)};
        RequireOk(reply, "Admit " + std::string{what});
        BOOST_REQUIRE_MESSAGE(reply->code == AdmissionCode::ADMITTED, std::string{what} + " not admitted");
        Result<ActionV2> action{ActionV2::Decode(bytes, node->Profile())};
        RequireOk(action, "Decode " + std::string{what});
        return std::move(action).value();
    }
    admission::PoolStatus StatusOf(const ActionV2& action) const
    {
        const std::optional<admission::PoolEntryStatus> status{node->Pool().Status(action.Id())};
        BOOST_REQUIRE(status.has_value());
        return status->status;
    }
    bool Traced(size_t from, std::string_view step) const
    {
        const std::vector<TraceEvent> all{trace.Events()};
        for (size_t i{from}; i < all.size(); ++i) {
            if (all[i].Step() == step) return true;
        }
        return false;
    }
    uint64_t Height() const { return node->Owner().Head()->Header().height; }
};

BOOST_AUTO_TEST_CASE(an_action_that_breaks_its_slot_only_after_another_is_deferred_not_excluded)
{
    // Codex review round 10 of M4-ADM-03 (2026-11-19): the bisection over admission-order prefixes found the first
    // action whose addition broke the slot, and that is not always the action that fails. Mallory holds no rUSD and
    // sends bob rUSD; carol funds mallory with rUSD, admitted after mallory's transfer but executed before it in the
    // canonical order. Mallory's transfer alone is a certified REJ_INSUFFICIENT_AVAILABLE; after carol's it gives bob
    // a 257th asset (OVERSIZE). The node excluded carol's honest transfer. Now an action is excluded only when it breaks
    // the slot alone; carol's is deferred to the next slot and mallory's is rejected as it would be alone.
    Adm03Rig r;
    SimClient mallory{r.Client("mallory")};
    SimClient carol{r.Client("carol")};
    r.Deposit(mallory.Account(), SimAsset("fm2/test/adm03b/mallory"), 1); // an account, but no rUSD
    r.Deposit(carol.Account(), r.rusd, 1'000'000);
    r.SealDeposits();
    const std::shared_ptr<const state::HeadView> head{r.node->Owner().Head()};
    const uint64_t ref{head->Header().height};
    const uint64_t height{ref + 1};
    const ActionV2 bad{r.Admit(mallory.Transfer("bad", 1, r.bob, r.rusd, 5, ref), "mallory's transfer to bob")};
    // Carol's amount is ground until her transfer runs first in this slot's canonical order (two accounts: the key decides).
    std::optional<std::vector<unsigned char>> funding;
    for (Atoms amount{7}; !funding.has_value(); ++amount) {
        BOOST_REQUIRE(amount < 100);
        Result<std::vector<unsigned char>> bytes{carol.Transfer("fund" + std::to_string(amount), 1, mallory.Account(), r.rusd, amount, ref)};
        RequireOk(bytes, "carol's transfer");
        Result<ActionV2> action{ActionV2::Decode(*bytes, r.node->Profile())};
        RequireOk(action, "Decode carol's transfer");
        if (engine::OrderKey(head->HeaderHash(), height, action->Id()) < engine::OrderKey(head->HeaderHash(), height, bad.Id())) {
            funding = std::move(bytes).value();
        }
    }
    const ActionV2 good{r.Admit(*funding, "carol's transfer to mallory")};

    const size_t events{r.trace.Size()};
    const Result<SealReport> sealed{r.node->SealSlot()};
    RequireOk(sealed, "SealSlot 2");
    BOOST_CHECK_EQUAL(sealed->originals, 1U);
    BOOST_CHECK(r.StatusOf(bad) == admission::PoolStatus::INCLUDED); // REJ_INSUFFICIENT_AVAILABLE, its nonce used
    BOOST_CHECK(r.StatusOf(good) == admission::PoolStatus::PENDING);
    BOOST_CHECK(r.Traced(events, "pool.defer"));
    BOOST_CHECK(!r.Traced(events, "pool.exclude"));
    const Result<SealReport> next{r.node->SealSlot()};
    RequireOk(next, "SealSlot 3");
    BOOST_CHECK_EQUAL(next->originals, 1U);
    BOOST_CHECK(r.StatusOf(good) == admission::PoolStatus::INCLUDED);
    const ledger::AccountState* bob_state{r.node->Owner().Head()->State().engine.ledger.Find(r.bob)};
    BOOST_REQUIRE(bob_state != nullptr);
    BOOST_CHECK_EQUAL(bob_state->balances.size(), MAX_LEAF_ASSETS);
}

BOOST_AUTO_TEST_CASE(many_breaking_actions_and_their_resends_do_not_stop_sealing)
{
    // Codex review round 10 of M4-ADM-03: after eight exclusions (MAX_SLOT_EXCLUSIONS) step 3 returned the error and
    // proposed nothing, so nine actions that each break the slot, resent after their exclusion (an exact resend is
    // admitted again), stopped sealing for good. Now every slot seals: an action that breaks the slot alone is
    // excluded, and once step 3's preview budget is spent the slot seals the part known to have a candidate and leaves
    // the rest in the pool. An honest action waits only for the actions admitted before it.
    Adm03Rig r;
    std::vector<SimClient> spammers;
    for (size_t i{0}; i < 40; ++i) {
        spammers.push_back(r.Client("spam" + std::to_string(i)));
        r.Deposit(spammers.back().Account(), r.rusd, 100);
    }
    SimClient carol{r.Client("carol")};
    r.Deposit(carol.Account(), r.rusd, 1'000'000);
    r.SealDeposits();
    const uint64_t ref{r.Height()};
    std::vector<std::vector<unsigned char>> sent;
    std::vector<ActionV2> spam;
    for (SimClient& spammer : spammers) {
        Result<std::vector<unsigned char>> bytes{spammer.Transfer("spam", 1, r.bob, r.rusd, 1, ref)};
        RequireOk(bytes, "a spam transfer");
        sent.push_back(*bytes);
    }
    const auto admit_spam = [&](size_t n) {
        for (size_t i{0}; i < n; ++i) {
            const ActionV2 action{r.Admit(sent[i], "spam " + std::to_string(i))};
            if (spam.size() <= i) spam.push_back(action);
        }
    };
    const auto seal = [&](std::string_view what) {
        const uint64_t before{r.Height()};
        const Result<SealReport> sealed{r.node->SealSlot()};
        RequireOk(sealed, std::string{what});
        BOOST_CHECK_EQUAL(r.Height(), before + 1);
        return *sealed;
    };

    // 1. Twelve breaking transfers, then carol's: the twelve are excluded and carol's is included.
    admit_spam(12);
    const ActionV2 good1{r.Admit(carol.Transfer("good1", 1, spammers[0].Account(), r.rusd, 1, ref), "carol 1")};
    BOOST_CHECK_EQUAL(seal("SealSlot 2").originals, 1U);
    BOOST_CHECK(r.StatusOf(good1) == admission::PoolStatus::INCLUDED);
    for (size_t i{0}; i < 12; ++i) BOOST_CHECK(r.StatusOf(spam[i]) == admission::PoolStatus::EXCLUDED);

    // 2. The same twelve resent, then carol's next: the same again.
    admit_spam(12);
    const ActionV2 good2{r.Admit(carol.Transfer("good2", 2, spammers[0].Account(), r.rusd, 1, ref), "carol 2")};
    BOOST_CHECK_EQUAL(seal("SealSlot 3").originals, 1U);
    BOOST_CHECK(r.StatusOf(good2) == admission::PoolStatus::INCLUDED);

    // 3. Forty breaking transfers are more than one slot's preview budget: the first slot seals without actions and
    // leaves the rest in the pool, the next one excludes the rest and includes carol's.
    admit_spam(40);
    const ActionV2 good3{r.Admit(carol.Transfer("good3", 3, spammers[0].Account(), r.rusd, 1, ref), "carol 3")};
    const size_t events{r.trace.Size()};
    BOOST_CHECK_EQUAL(seal("SealSlot 4").originals, 0U);
    BOOST_CHECK(r.Traced(events, "pool.defer"));
    BOOST_CHECK(r.StatusOf(good3) == admission::PoolStatus::PENDING);
    BOOST_CHECK_EQUAL(seal("SealSlot 5").originals, 1U);
    BOOST_CHECK(r.StatusOf(good3) == admission::PoolStatus::INCLUDED);
    for (const ActionV2& action : spam) BOOST_CHECK(r.StatusOf(action) == admission::PoolStatus::EXCLUDED);
    BOOST_CHECK_EQUAL(r.node->Pool().Size(), 0U);
}

BOOST_AUTO_TEST_CASE(an_action_that_breaks_its_slot_alone_but_not_with_the_sealed_slot_is_kept)
{
    // Codex review round 11 of M4-ADM-03 (2026-11-19): breaking the slot alone does not make an action terminal.
    // Alice's transfer of rUSD to bob (MAX_LEAF_ASSETS assets) fails alone, but bob's transfer of his whole balance
    // of one asset, executed before it, frees an asset entry. Dave's transfer to erin (also full) breaks the slot
    // whatever else is in it. The search tried alice's alone, saw OVERSIZE and excluded it, then sealed bob's. Now an
    // action that breaks the slot alone is excluded only if the slot being sealed has no candidate with it: alice's
    // is included after bob's, and only dave's is excluded.
    Adm03Rig r;
    SimClient alice{r.Client("alice")};
    SimClient bob{r.Client("bob")};
    SimClient carol{r.Client("carol")};
    SimClient dave{r.Client("dave")};
    BOOST_REQUIRE(bob.Account() == r.bob);
    const AccountId erin{SimClient::AccountOf("erin").value()};
    r.Deposit(alice.Account(), r.rusd, 1'000'000);
    r.Deposit(dave.Account(), r.rusd, 1'000'000);
    r.Deposit(carol.Account(), SimAsset("fm2/test/adm03b/carol"), 1);
    for (size_t i{0}; i < MAX_LEAF_ASSETS; ++i) r.Deposit(erin, SimAsset("fm2/test/adm03b/erin/" + std::to_string(i)), 1);
    r.SealDeposits();
    const std::shared_ptr<const state::HeadView> head{r.node->Owner().Head()};
    const uint64_t ref{head->Header().height};
    const uint64_t height{ref + 1};
    const AssetId freed{SimAsset("fm2/test/adm03b/0")}; // bob holds 1 atom of it
    Result<std::vector<unsigned char>> freeing_bytes{bob.Transfer("free", 1, carol.Account(), freed, 1, ref)};
    RequireOk(freeing_bytes, "bob's transfer");
    Result<ActionV2> freeing_action{ActionV2::Decode(*freeing_bytes, r.node->Profile())};
    RequireOk(freeing_action, "Decode bob's transfer");
    // Alice's amount is ground until bob's transfer runs before hers in this slot's canonical order.
    std::optional<std::vector<unsigned char>> paying_bytes;
    Atoms paid{0};
    for (Atoms amount{5}; !paying_bytes.has_value(); ++amount) {
        BOOST_REQUIRE(amount < 100);
        Result<std::vector<unsigned char>> bytes{alice.Transfer("pay" + std::to_string(amount), 1, r.bob, r.rusd, amount, ref)};
        RequireOk(bytes, "alice's transfer");
        Result<ActionV2> action{ActionV2::Decode(*bytes, r.node->Profile())};
        RequireOk(action, "Decode alice's transfer");
        if (engine::OrderKey(head->HeaderHash(), height, freeing_action->Id()) < engine::OrderKey(head->HeaderHash(), height, action->Id())) {
            paying_bytes = std::move(bytes).value();
            paid = amount;
        }
    }
    const ActionV2 paying{r.Admit(*paying_bytes, "alice's transfer to bob")};
    const ActionV2 freeing{r.Admit(*freeing_bytes, "bob's transfer to carol")};
    const ActionV2 breaking{r.Admit(dave.Transfer("break", 1, erin, r.rusd, 1, ref), "dave's transfer to erin")};

    const Result<SealReport> sealed{r.node->SealSlot()};
    RequireOk(sealed, "SealSlot 2");
    BOOST_CHECK_EQUAL(sealed->originals, 2U);
    BOOST_CHECK(r.StatusOf(paying) == admission::PoolStatus::INCLUDED);
    BOOST_CHECK(r.StatusOf(freeing) == admission::PoolStatus::INCLUDED);
    BOOST_CHECK(r.StatusOf(breaking) == admission::PoolStatus::EXCLUDED);
    const ledger::AccountState* bob_state{r.node->Owner().Head()->State().engine.ledger.Find(r.bob)};
    BOOST_REQUIRE(bob_state != nullptr);
    BOOST_CHECK_EQUAL(bob_state->balances.size(), MAX_LEAF_ASSETS);
    BOOST_CHECK_EQUAL(bob_state->BalanceOf(r.rusd).available, paid);
    BOOST_CHECK(!bob_state->balances.contains(freed));
}

BOOST_AUTO_TEST_CASE(a_suspect_excluded_before_another_joined_is_checked_again)
{
    // Codex review round 12 of M4-ADM-03 (2026-11-19): suspects were checked once each, in admission order, so one
    // that a later suspect's joining would have made valid was already excluded. Bob and frank hold MAX_LEAF_ASSETS
    // assets each. Alice sends bob rUSD (a); bob sends frank his whole balance of one asset (b); frank sends gina his
    // whole balance of one of his (c); dave sends erin, also full, rUSD (d). In the canonical order c runs before b,
    // and b before a: c frees an entry for b, and b one for a. Checked against the settled slot {c}, a failed and was
    // excluded before b joined, although the slot {c, b} takes it. Now the checks repeat until a pass adds nothing,
    // so the slot is {c, b, a} and only d is excluded.
    Adm03Rig r;
    SimClient alice{r.Client("alice")};
    SimClient bob{r.Client("bob")};
    SimClient frank{r.Client("frank")};
    SimClient dave{r.Client("dave")};
    BOOST_REQUIRE(bob.Account() == r.bob);
    const AccountId erin{SimClient::AccountOf("erin").value()};
    const AccountId gina{SimClient::AccountOf("gina").value()};
    r.Deposit(alice.Account(), r.rusd, 1'000'000);
    r.Deposit(dave.Account(), r.rusd, 1'000'000);
    r.Deposit(gina, SimAsset("fm2/test/adm03b/gina"), 1);
    const auto frank_asset = [](size_t i) { return SimAsset("fm2/test/adm03b/frank/" + std::to_string(i)); };
    for (size_t i{0}; i < MAX_LEAF_ASSETS; ++i) {
        r.Deposit(frank.Account(), frank_asset(i), 1);
        r.Deposit(erin, SimAsset("fm2/test/adm03b/erin/" + std::to_string(i)), 1);
    }
    r.SealDeposits();
    const std::shared_ptr<const state::HeadView> head{r.node->Owner().Head()};
    const uint64_t ref{head->Header().height};
    const uint64_t height{ref + 1};
    const auto key = [&](const std::vector<unsigned char>& bytes) {
        Result<ActionV2> action{ActionV2::Decode(bytes, r.node->Profile())};
        RequireOk(action, "Decode");
        return engine::OrderKey(head->HeaderHash(), height, action->Id());
    };
    const auto signed_bytes = [](Result<std::vector<unsigned char>> bytes) {
        RequireOk(bytes, "Transfer");
        return std::move(bytes).value();
    };
    // b: bob's whole balance of one asset to frank, who lacks it.
    const AssetId bobs{SimAsset("fm2/test/adm03b/0")};
    const std::vector<unsigned char> b{signed_bytes(bob.Transfer("b", 1, frank.Account(), bobs, 1, ref))};
    // c: frank's whole balance of the first of his assets whose transfer runs before b.
    std::optional<std::vector<unsigned char>> c;
    for (size_t i{0}; !c.has_value(); ++i) {
        BOOST_REQUIRE(i < 64);
        std::vector<unsigned char> bytes{signed_bytes(frank.Transfer("c" + std::to_string(i), 1, gina, frank_asset(i), 1, ref))};
        if (key(bytes) < key(b)) c = std::move(bytes);
    }
    // a: alice's rUSD to bob, the amount ground until it runs after b.
    std::optional<std::vector<unsigned char>> a;
    Atoms paid{0};
    for (Atoms amount{5}; !a.has_value(); ++amount) {
        BOOST_REQUIRE(amount < 100);
        std::vector<unsigned char> bytes{signed_bytes(alice.Transfer("a" + std::to_string(amount), 1, r.bob, r.rusd, amount, ref))};
        if (key(b) < key(bytes)) {
            a = std::move(bytes);
            paid = amount;
        }
    }
    const ActionV2 paying{r.Admit(*a, "a")};
    const ActionV2 freeing_bob{r.Admit(b, "b")};
    const ActionV2 freeing_frank{r.Admit(*c, "c")};
    const ActionV2 breaking{r.Admit(dave.Transfer("d", 1, erin, r.rusd, 1, ref), "d")};

    const Result<SealReport> sealed{r.node->SealSlot()};
    RequireOk(sealed, "SealSlot 2");
    BOOST_CHECK_EQUAL(sealed->originals, 3U);
    BOOST_CHECK(r.StatusOf(paying) == admission::PoolStatus::INCLUDED);
    BOOST_CHECK(r.StatusOf(freeing_bob) == admission::PoolStatus::INCLUDED);
    BOOST_CHECK(r.StatusOf(freeing_frank) == admission::PoolStatus::INCLUDED);
    BOOST_CHECK(r.StatusOf(breaking) == admission::PoolStatus::EXCLUDED);
    const ledger::AccountState* bob_state{r.node->Owner().Head()->State().engine.ledger.Find(r.bob)};
    BOOST_REQUIRE(bob_state != nullptr);
    BOOST_CHECK_EQUAL(bob_state->BalanceOf(r.rusd).available, paid);
    BOOST_CHECK_EQUAL(bob_state->balances.size(), MAX_LEAF_ASSETS);
}

BOOST_AUTO_TEST_CASE(a_restart_rebuilds_the_retained_pool_records_and_reads)
{
    // M4-STO-03's pool rebuild (O-71): a restarted seat built its pool and read service empty at the replayed head, so
    // an exact resend of an action included before the restart was no longer DUPLICATE, GetAction no longer proved its
    // inclusion and nothing was readable until the next slot. The restart now replays the retained slots into both.
    TraceRecorder trace;
    Result<std::unique_ptr<SimNode>> created{SimNode::Create(SimNodeConfig{}, trace)};
    RequireOk(created, "SimNode::Create");
    SimNode& node{**created};
    const std::vector<MarketDescriptor> markets{node.Markets().begin(), node.Markets().end()};
    Result<SimClient> alice{SimClient::Create("alice", node.Profile(), node.Domain(), node.Seats(), markets)};
    RequireOk(alice, "SimClient::Create");
    const AccountId bob{SimClient::AccountOf("bob").value()};
    const AssetId rusd{SimAsset("rUSD")};
    std::vector<Fact> facts;
    for (const AccountId& to : {alice->Account(), bob}) {
        DepositFact fact;
        fact.cursor = FactCursor{SIM_BASE_HEIGHT + 1, static_cast<uint32_t>(facts.size())};
        fact.outpoint = Outpoint{SimLabelHash("fm2/test/sim_node/sto03/" + std::to_string(facts.size())), 0};
        fact.account = to;
        fact.asset = rusd;
        fact.amount = 1'000'000;
        facts.push_back(Fact{fact});
    }
    RequireOk(node.Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 1), std::move(facts)), "AppendBlock");
    RequireOk(node.SealSlot(), "SealSlot 1");
    const Result<std::vector<unsigned char>> bytes{alice->Transfer("t", 1, bob, rusd, 9, 1)};
    RequireOk(bytes, "Transfer");
    const Result<admission::AdmissionReply> first{node.Admitter().Admit(*bytes)};
    RequireOk(first, "Admit");
    BOOST_REQUIRE(first->code == AdmissionCode::ADMITTED);
    Result<ActionV2> action{ActionV2::Decode(*bytes, node.Profile())};
    RequireOk(action, "Decode");
    RequireOk(node.SealSlot(), "SealSlot 2 (includes the transfer)");
    RequireOk(node.SealSlot(), "SealSlot 3");
    const std::optional<admission::PoolEntryStatus> before{node.Pool().Status(action->Id())};
    BOOST_REQUIRE(before.has_value() && before->status == admission::PoolStatus::INCLUDED);
    const uint256 head_before{node.Owner().Head()->HeaderHash()};

    RequireOk(node.Restart(), "Restart");
    BOOST_CHECK(!node.Fenced());
    BOOST_CHECK(node.Owner().Head()->HeaderHash() == head_before);
    // The read service has the replayed slots: the head is readable at once, the transfer's inclusion is proved.
    BOOST_CHECK(node.Read().PublishedHeight() == std::optional<uint64_t>{3});
    const Result<net::ActionStatusReply> status{node.Api().GetAction(*action)};
    RequireOk(status, "GetAction after the restart");
    BOOST_CHECK(status->status == net::ApiActionStatus::INCLUDED);
    // The pool has the INCLUDED record again, so the exact resend is DUPLICATE (A3), not answered by A5/A6.
    const std::optional<admission::PoolEntryStatus> after{node.Pool().Status(action->Id())};
    BOOST_REQUIRE(after.has_value());
    BOOST_CHECK(after->status == admission::PoolStatus::INCLUDED);
    BOOST_CHECK(after->included == before->included);
    BOOST_CHECK_EQUAL(node.Pool().HeadHeight(), 3U);
    const Result<admission::AdmissionReply> resend{node.Admitter().Admit(*bytes)};
    RequireOk(resend, "Admit the resend");
    BOOST_CHECK_MESSAGE(resend->code == AdmissionCode::DUPLICATE, "resend after the restart: " + std::string{ToString(resend->code)});
    // The rebuild wrote nothing: the journal still ends with APPLIED 3, and the node seals on from there.
    const Result<std::vector<storage::JournalRecord>> records{node.CommitJournal().ReadAll()};
    RequireOk(records, "commit ReadAll");
    BOOST_CHECK_EQUAL(records->size(), 6U); // DECISION and APPLIED for slots 1 to 3
    RequireOk(node.SealSlot(), "SealSlot 4");
    BOOST_CHECK_EQUAL(node.Owner().Head()->Header().height, 4U);
}

BOOST_AUTO_TEST_CASE(a_restart_past_the_retention_window_rebuilds_what_the_live_pool_kept)
{
    // The rebuild starts max(retention_slots, outcome_index_slots, view_slots) = 1,024 slots below the committed head:
    // an action included in slot 2 was evicted by the live pool at slot 1,026 and stays evicted, one included near the
    // head is retained, and the restarted pool and read service agree with the live ones.
    TraceRecorder trace;
    Result<std::unique_ptr<SimNode>> created{SimNode::Create(SimNodeConfig{}, trace)};
    RequireOk(created, "SimNode::Create");
    SimNode& node{**created};
    const std::vector<MarketDescriptor> markets{node.Markets().begin(), node.Markets().end()};
    Result<SimClient> alice{SimClient::Create("alice", node.Profile(), node.Domain(), node.Seats(), markets)};
    RequireOk(alice, "SimClient::Create");
    const AccountId bob{SimClient::AccountOf("bob").value()};
    const AssetId rusd{SimAsset("rUSD")};
    std::vector<Fact> facts;
    for (const AccountId& to : {alice->Account(), bob}) {
        DepositFact fact;
        fact.cursor = FactCursor{SIM_BASE_HEIGHT + 1, static_cast<uint32_t>(facts.size())};
        fact.outpoint = Outpoint{SimLabelHash("fm2/test/sim_node/sto03w/" + std::to_string(facts.size())), 0};
        fact.account = to;
        fact.asset = rusd;
        fact.amount = 1'000'000;
        facts.push_back(Fact{fact});
    }
    RequireOk(node.Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 1), std::move(facts)), "AppendBlock");
    RequireOk(node.SealSlot(), "SealSlot 1");
    const auto include = [&](uint64_t nonce) {
        const uint64_t head{node.Owner().Head()->Header().height};
        const Result<std::vector<unsigned char>> bytes{alice->Transfer("t" + std::to_string(nonce), nonce, bob, rusd, 1, head)};
        RequireOk(bytes, "Transfer");
        const Result<admission::AdmissionReply> reply{node.Admitter().Admit(*bytes)};
        RequireOk(reply, "Admit");
        BOOST_REQUIRE(reply->code == AdmissionCode::ADMITTED);
        RequireOk(node.SealSlot(), "SealSlot (includes the transfer)");
        return ActionV2::Decode(*bytes, node.Profile()).value().Id();
    };
    const ActionId old_id{include(1)}; // slot 2
    while (node.Owner().Head()->Header().height < 1'028) RequireOk(node.SealSlot(), "SealSlot (heartbeat)");
    const ActionId recent_id{include(2)}; // slot 1,029
    RequireOk(node.SealSlot(), "SealSlot 1,030");
    BOOST_REQUIRE_EQUAL(node.Owner().Head()->Header().height, 1'030U);
    const auto snapshot = [&] {
        return std::tuple{node.Pool().Status(old_id), node.Pool().Status(recent_id), node.Pool().IncludedRecords(),
                          node.Pool().TerminalRecords(), node.Pool().HeadHeight(), node.Read().PublishedHeight()};
    };
    const auto live{snapshot()};
    BOOST_CHECK(!std::get<0>(live).has_value());
    BOOST_REQUIRE(std::get<1>(live).has_value());
    BOOST_CHECK(std::get<1>(live)->status == admission::PoolStatus::INCLUDED);

    RequireOk(node.Restart(), "Restart");
    const auto restarted{snapshot()};
    BOOST_CHECK(std::get<0>(restarted) == std::get<0>(live));
    BOOST_REQUIRE(std::get<1>(restarted).has_value());
    // The same INCLUDED record; only its PendingRef is gone (the restarted pool never held the action, O-71).
    BOOST_CHECK(std::get<1>(restarted)->status == admission::PoolStatus::INCLUDED);
    BOOST_CHECK(std::get<1>(restarted)->included == std::get<1>(live)->included);
    BOOST_CHECK_EQUAL(std::get<1>(restarted)->height, std::get<1>(live)->height);
    BOOST_CHECK(!std::get<1>(restarted)->pending.has_value());
    BOOST_CHECK_EQUAL(std::get<2>(restarted), std::get<2>(live));
    BOOST_CHECK_EQUAL(std::get<3>(restarted), std::get<3>(live));
    BOOST_CHECK_EQUAL(std::get<4>(restarted), std::get<4>(live));
    BOOST_CHECK(std::get<5>(restarted) == std::get<5>(live));
    RequireOk(node.SealSlot(), "SealSlot 1,031");
}

BOOST_AUTO_TEST_SUITE_END()
