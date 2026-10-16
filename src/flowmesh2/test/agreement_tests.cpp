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
