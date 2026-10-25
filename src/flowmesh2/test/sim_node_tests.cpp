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
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/result.h>
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

#include <algorithm>
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

//! A client of the rig's node.
SimClient ClientOf(NodeRig& rig, const std::string& name)
{
    const std::vector<MarketDescriptor> markets{rig.node->Markets().begin(), rig.node->Markets().end()};
    Result<SimClient> client{SimClient::Create(name, rig.node->Profile(), rig.node->Domain(), rig.node->Seats(), markets)};
    RequireOk(client, "SimClient::Create " + name);
    return std::move(client).value();
}

//! Deposit facts of one block, cursors (height, 0), (height, 1), ... in order.
struct Deposits {
    uint32_t height{0};
    std::vector<Fact> facts;

    void Add(const AccountId& account, const AssetId& asset, Atoms amount)
    {
        const uint32_t index{static_cast<uint32_t>(facts.size())};
        facts.push_back(DepositFact{FactCursor{height, index},
                                    Outpoint{SimLabelHash("fm2/test/adm03/" + std::to_string(height) + "/" + std::to_string(index)), 0},
                                    account, asset, amount});
    }
};

//! Admit `bytes` and require ADMITTED; returns its ActionId.
ActionId AdmitOk(NodeRig& rig, const Result<std::vector<unsigned char>>& bytes, std::string_view what)
{
    RequireOk(bytes, std::string{what} + " bytes");
    const Result<admission::AdmissionReply> reply{rig.node->Admitter().Admit(*bytes)};
    RequireOk(reply, std::string{what} + " Admit");
    BOOST_REQUIRE_MESSAGE(reply->code == AdmissionCode::ADMITTED, std::string{what} + " was not ADMITTED");
    return *reply->id;
}

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

BOOST_AUTO_TEST_CASE(an_action_the_engine_refuses_is_excluded_and_the_slot_seals)
{
    // M4-ADM-03. Bob holds MAX_LEAF_ASSETS assets, so Carol's TRANSFER of
    // one more asset to him makes Preview fail with OVERSIZE: an O-34 error,
    // not a certified outcome. Without the exclusion the node retried the
    // same slot with the same actions forever and Alice's PLACE, admitted
    // after Carol's TRANSFER, never sealed.
    NodeRig rig;
    RequireOk(rig.node->SealSlot(), "SealSlot 1");
    SimClient alice{ClientOf(rig, "alice")};
    SimClient carol{ClientOf(rig, "carol")};
    const AccountId bob{SimClient::AccountOf("bob").value()};
    Deposits block{SIM_BASE_HEIGHT + 2, {}};
    for (size_t i{0}; i < MAX_LEAF_ASSETS; ++i) block.Add(bob, SimAsset("adm03-" + std::to_string(i)), 1);
    block.Add(carol.Account(), SimAsset("adm03-extra"), 5);
    block.Add(alice.Account(), SimAsset("rUSD"), 1'000'000);
    RequireOk(rig.node->Chain().AppendBlock(SimBlockHash(block.height), std::move(block.facts)), "AppendBlock");
    RequireOk(rig.node->SealSlot(), "SealSlot 2 (deposits)");

    const ActionId transfer{AdmitOk(rig, carol.Transfer("t", 1, bob, SimAsset("adm03-extra"), 1, 2), "Carol's TRANSFER")};
    const ActionId place{AdmitOk(rig, alice.Place("p", 1, SimMarketM1(), Side::BUY, TimeInForce::GTC, 250, 1, 2), "Alice's PLACE")};

    const size_t events{rig.trace.Size()};
    const Result<SealReport> third{rig.node->SealSlot()};
    RequireOk(third, "SealSlot 3");
    BOOST_CHECK_EQUAL(third->height, 3U);
    BOOST_CHECK_EQUAL(third->originals, 1U);
    BOOST_CHECK(!rig.node->Fenced());

    // Alice's PLACE waited no slot; Carol's TRANSFER is terminal, never taken again.
    const std::optional<admission::PoolEntryStatus> placed{rig.node->Pool().Status(place)};
    BOOST_REQUIRE(placed.has_value());
    BOOST_CHECK(placed->status == admission::PoolStatus::INCLUDED);
    const std::optional<admission::PoolEntryStatus> refused{rig.node->Pool().Status(transfer)};
    BOOST_REQUIRE(refused.has_value());
    BOOST_CHECK_EQUAL(admission::ToString(refused->status), "EXCLUDED");
    BOOST_CHECK(admission::IsTerminal(refused->status));
    BOOST_CHECK_EQUAL(refused->height, 2U);
    const std::vector<std::string> steps{rig.StepsFrom(events)};
    BOOST_CHECK(std::count(steps.begin(), steps.end(), "pool.exclude") == 1);

    // The client gets a definite answer: UNKNOWN, because nothing certified
    // proves a refusal, with the reason that this node excluded it.
    const Result<ActionV2> original{carol.Action("t")};
    RequireOk(original, "carol.Action");
    const Result<net::ActionStatusReply> answer{rig.node->Api().GetAction(*original)};
    RequireOk(answer, "GetAction");
    BOOST_CHECK(answer->status == net::ApiActionStatus::UNKNOWN);
    BOOST_CHECK_EQUAL(answer->reason, "this node's proposals excluded it: the engine refused every candidate holding it");

    // The next slot does not take it again; Bob's leaf still holds MAX_LEAF_ASSETS assets.
    const size_t before{rig.trace.Size()};
    const Result<SealReport> fourth{rig.node->SealSlot()};
    RequireOk(fourth, "SealSlot 4");
    BOOST_CHECK_EQUAL(fourth->originals, 0U);
    const std::vector<std::string> later{rig.StepsFrom(before)};
    BOOST_CHECK(std::count(later.begin(), later.end(), "pool.exclude") == 0);
    const ledger::AccountState* bob_state{rig.node->Owner().Head()->State().engine.ledger.Find(bob)};
    BOOST_REQUIRE(bob_state != nullptr);
    BOOST_CHECK_EQUAL(bob_state->balances.size(), MAX_LEAF_ASSETS);
}

BOOST_AUTO_TEST_CASE(every_refused_action_is_excluded_and_the_rest_keep_their_order)
{
    // Two refused TRANSFERs among three valid actions: the bisection
    // excludes each, and the slot takes the others in admission order.
    NodeRig rig;
    RequireOk(rig.node->SealSlot(), "SealSlot 1");
    SimClient alice{ClientOf(rig, "alice")};
    SimClient carol{ClientOf(rig, "carol")};
    SimClient dave{ClientOf(rig, "dave")};
    const AccountId bob{SimClient::AccountOf("bob").value()};
    Deposits block{SIM_BASE_HEIGHT + 2, {}};
    for (size_t i{0}; i < MAX_LEAF_ASSETS; ++i) block.Add(bob, SimAsset("adm03-" + std::to_string(i)), 1);
    block.Add(carol.Account(), SimAsset("adm03-c"), 5);
    block.Add(dave.Account(), SimAsset("adm03-d"), 5);
    block.Add(alice.Account(), SimAsset("rUSD"), 1'000'000);
    RequireOk(rig.node->Chain().AppendBlock(SimBlockHash(block.height), std::move(block.facts)), "AppendBlock");
    RequireOk(rig.node->SealSlot(), "SealSlot 2 (deposits)");

    const ActionId place{AdmitOk(rig, alice.Place("p", 1, SimMarketM1(), Side::BUY, TimeInForce::GTC, 250, 1, 2), "Alice's PLACE")};
    const ActionId bad_c{AdmitOk(rig, carol.Transfer("t", 1, bob, SimAsset("adm03-c"), 1, 2), "Carol's TRANSFER")};
    const ActionId good_c{AdmitOk(rig, alice.Transfer("t", 2, carol.Account(), SimAsset("rUSD"), 7, 2), "Alice's TRANSFER")};
    const ActionId bad_d{AdmitOk(rig, dave.Transfer("t", 1, bob, SimAsset("adm03-d"), 1, 2), "Dave's TRANSFER")};
    const ActionId good_d{AdmitOk(rig, carol.Transfer("u", 2, dave.Account(), SimAsset("adm03-c"), 2, 2), "Carol's second TRANSFER")};

    const size_t events{rig.trace.Size()};
    const Result<SealReport> third{rig.node->SealSlot()};
    RequireOk(third, "SealSlot 3");
    BOOST_CHECK_EQUAL(third->originals, 3U);
    const std::vector<std::string> steps{rig.StepsFrom(events)};
    BOOST_CHECK(std::count(steps.begin(), steps.end(), "pool.exclude") == 2);
    for (const ActionId& id : {place, good_c, good_d}) {
        BOOST_CHECK(rig.node->Pool().Status(id)->status == admission::PoolStatus::INCLUDED);
    }
    for (const ActionId& id : {bad_c, bad_d}) {
        BOOST_CHECK_EQUAL(admission::ToString(rig.node->Pool().Status(id)->status), "EXCLUDED");
    }
    // The pool's list keeps admission order; the body holds the same three in
    // the engine's canonical order (O-50).
    const SealedSlot& sealed{rig.node->Sealed().back()};
    BOOST_CHECK(sealed.taken == (std::vector<ActionId>{place, good_c, good_d}));
    std::vector<ActionId> in_body;
    for (const auto& original : sealed.candidate.Body().originals) {
        const Result<ActionV2> decoded{ActionV2::Decode(original, rig.node->Profile())};
        RequireOk(decoded, "decode a body original");
        in_body.push_back(decoded->Id());
    }
    std::vector<ActionId> kept{place, good_c, good_d};
    std::sort(in_body.begin(), in_body.end());
    std::sort(kept.begin(), kept.end());
    BOOST_CHECK(in_body == kept);
}

BOOST_AUTO_TEST_CASE(a_fact_error_never_excludes_an_action)
{
    // The exclusion blames an action only when the slot without actions is
    // valid. Here the facts alone overflow the cumulative deposits of one
    // asset (O-34): SealSlot returns the error as before, unfenced, and
    // Alice's PLACE stays live.
    NodeRig rig;
    RequireOk(rig.node->SealSlot(), "SealSlot 1");
    SimClient alice{ClientOf(rig, "alice")};
    Deposits funding{SIM_BASE_HEIGHT + 2, {}};
    funding.Add(alice.Account(), SimAsset("rUSD"), 1'000'000);
    RequireOk(rig.node->Chain().AppendBlock(SimBlockHash(funding.height), std::move(funding.facts)), "AppendBlock funding");
    RequireOk(rig.node->SealSlot(), "SealSlot 2 (deposit)");
    const ActionId place{AdmitOk(rig, alice.Place("p", 1, SimMarketM1(), Side::BUY, TimeInForce::GTC, 250, 1, 2), "Alice's PLACE")};

    const AccountId dave{SimClient::AccountOf("dave").value()};
    Deposits overflow{SIM_BASE_HEIGHT + 3, {}};
    overflow.Add(dave, SimAsset("adm03-big"), MAX_ATOMS);
    overflow.Add(dave, SimAsset("adm03-big"), 1);
    RequireOk(rig.node->Chain().AppendBlock(SimBlockHash(overflow.height), std::move(overflow.facts)), "AppendBlock overflow");

    const size_t events{rig.trace.Size()};
    RequireError(rig.node->SealSlot(), ErrorCode::ARITHMETIC_OVERFLOW, "SealSlot 3 (fact error)");
    BOOST_CHECK(!rig.node->Fenced());
    const std::optional<admission::PoolEntryStatus> status{rig.node->Pool().Status(place)};
    BOOST_REQUIRE(status.has_value());
    BOOST_CHECK(!admission::IsTerminal(status->status));
    const std::vector<std::string> steps{rig.StepsFrom(events)};
    BOOST_CHECK(std::count(steps.begin(), steps.end(), "pool.exclude") == 0);
}

BOOST_AUTO_TEST_SUITE_END()
