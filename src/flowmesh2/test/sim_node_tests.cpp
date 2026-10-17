// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** The simulated node's halting rules (review of 18051472..ffc43baf;
 * ARCHITECTURE.md O-89): a fenced node seals nothing, and a DECISION that
 * check 3 refuses leaves the node at the last applied slot, which is also
 * what a replay of its commit journal gives, with exactly one signature for
 * the decided height.
 */

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/result.h>
#include <flowmesh2/sim/faults.h>
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

BOOST_AUTO_TEST_SUITE_END()
