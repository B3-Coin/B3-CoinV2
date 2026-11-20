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
#include <flowmesh2/agreement/digests.h>
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

} // namespace

// Decision carries a move-only durability token, so it is move-only too.
static_assert(!std::is_copy_constructible_v<Decision>);
static_assert(std::is_nothrow_move_constructible_v<Decision>);
static_assert(std::is_abstract_v<Agreement>);
static_assert(std::is_abstract_v<SafetyRules>);
static_assert(std::is_abstract_v<MessageSink>);
static_assert(std::is_base_of_v<SafetyRules, PbftSafetyRules>);
static_assert(!std::is_abstract_v<PbftSafetyRules>);
// Inclusion lists and the restart fence have no instances in the outline; PBFT is created by Create.
static_assert(!std::is_default_constructible_v<PbftAgreement>);
static_assert(std::is_base_of_v<Agreement, PbftAgreement>);
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

BOOST_AUTO_TEST_CASE(leader_schedule_is_seeded_and_skips_slow_seats)
{
    // M4-AGR-03. The leader of a term is a pure function of the authority
    // (epoch, seat set root) and the term; a view change rotates to the next
    // active seat; slow seats are skipped; every seat leads over many terms.
    const SeatSet seats{TestSeats({"s0", "s1", "s2", "s3"})};
    const AuthorityVersion authority{SeatAuthority(seats)};
    const LeaderSchedule schedule{seats.Size()};
    const uint32_t base{schedule.LeaderFor(authority, 3, SlowSet{}).value()};
    BOOST_CHECK_EQUAL(schedule.LeaderFor(authority, 3, SlowSet{}).value(), base);
    BOOST_CHECK_LT(base, 4U);
    // The seed binds the seat set root and the epoch, so another authority gives another schedule somewhere.
    bool differs{false};
    for (uint64_t term{0}; term < 64 && !differs; ++term) {
        differs = schedule.LeaderFor(authority, term, SlowSet{}).value() != schedule.LeaderFor(SeatAuthority(seats, 1), term, SlowSet{}).value();
    }
    BOOST_CHECK(differs);
    std::vector<unsigned> led(4, 0);
    for (uint64_t term{0}; term < 400; ++term) ++led[schedule.LeaderFor(authority, term, SlowSet{}).value()];
    for (const unsigned count : led) BOOST_CHECK_GT(count, 40U);
    // Views rotate through the active seats from the term's base leader.
    const uint64_t slot{3 * TERM_SLOTS + 2}; // term 3
    BOOST_CHECK_EQUAL(schedule.LeaderForView(authority, slot, 0, SlowSet{}).value(), base);
    BOOST_CHECK_EQUAL(schedule.LeaderForView(authority, slot, 1, SlowSet{}).value(), (base + 1) % 4);
    BOOST_CHECK_EQUAL(schedule.LeaderForView(authority, slot, 5, SlowSet{}).value(), (base + 5) % 4);
    BOOST_CHECK_EQUAL(schedule.LeaderForView(authority, slot + 1, 0, SlowSet{}).value(), base); // same term
    // Slow seats never lead; the rest keep their relative order.
    const SlowSet slow{{1, 3}};
    for (uint64_t term{0}; term < 50; ++term) {
        const uint32_t leader{schedule.LeaderFor(authority, term, slow).value()};
        BOOST_CHECK(leader == 0 || leader == 2);
        for (uint64_t view{0}; view < 4; ++view) {
            const uint32_t rotated{schedule.LeaderForView(authority, term * TERM_SLOTS, view, slow).value()};
            BOOST_CHECK(rotated == 0 || rotated == 2);
        }
    }
    // Refusals: a slow seat outside the set, an unordered slow set, every seat slow, no seats.
    RequireRefusal(schedule.LeaderFor(authority, 0, SlowSet{{4}}), ErrorCode::INVALID_INPUT, LEADER_SLOW_SET, "slow seat outside the set");
    RequireRefusal(schedule.LeaderFor(authority, 0, SlowSet{{2, 1}}), ErrorCode::INVALID_INPUT, LEADER_SLOW_SET, "unordered slow set");
    RequireRefusal(schedule.LeaderFor(authority, 0, SlowSet{{0, 1, 2, 3}}), ErrorCode::INVALID_INPUT, LEADER_NO_ACTIVE_SEAT, "every seat slow");
    RequireRefusal(LeaderSchedule{0}.LeaderFor(authority, 0, SlowSet{}), ErrorCode::INVALID_INPUT, LEADER_NO_ACTIVE_SEAT, "no seats");
    // One seat always leads itself.
    BOOST_CHECK_EQUAL(LeaderSchedule{1}.LeaderForView(authority, 9, 7, SlowSet{}).value(), 0U);
}

BOOST_AUTO_TEST_CASE(agreement_digests_are_domain_separated)
{
    // Every signed agreement statement has its own digest under the AGREEMENT
    // tag, bound to the instance domain; the prepare digest binds domain,
    // epoch, slot and view (review of 2026-11-16, defect 1: a certificate of
    // another slot must never verify as this slot's preparation).
    const uint256 domain{LabelHash("fm2/test/digest-domain")};
    const uint256 header{LabelHash("fm2/test/digest-header")};
    const uint256 prepare{PrepareDigest(domain, 0, 5, 0, header)};
    BOOST_CHECK(prepare != PrepareDigest(domain, 0, 5, 1, header));
    BOOST_CHECK(prepare != PrepareDigest(domain, 0, 4, 0, header));
    BOOST_CHECK(prepare != PrepareDigest(domain, 1, 5, 0, header));
    BOOST_CHECK(prepare != PrepareDigest(LabelHash("other"), 0, 5, 0, header));
    BOOST_CHECK(prepare != header);
    const PreparedCert prepared{2, SlotCertificate{header, {0x0f}, {}}};
    const uint256 vc{ViewChangeDigest(domain, 0, 5, 3, 1, prepared)};
    BOOST_CHECK(vc != ViewChangeDigest(domain, 0, 5, 3, 1, std::nullopt));
    BOOST_CHECK(vc != ViewChangeDigest(domain, 0, 5, 3, 2, prepared));
    BOOST_CHECK(vc != ViewChangeDigest(LabelHash("other"), 0, 5, 3, 1, prepared));
    BOOST_CHECK(vc != ViewChangeDigest(domain, 0, 5, 4, 1, prepared));
    const std::vector<uint256> reports{vc, ViewChangeDigest(domain, 0, 5, 3, 2, std::nullopt)};
    const uint256 nv{NewViewDigest(domain, 0, 5, 3, 1, header, reports)};
    BOOST_CHECK(nv != NewViewDigest(domain, 0, 5, 3, 1, uint256{}, reports));
    BOOST_CHECK(nv != NewViewDigest(domain, 0, 5, 3, 1, header, {}));
    BOOST_CHECK(nv != vc);
    const uint256 hw{HighWaterDigest(domain, 2, 0, 9, 1)};
    BOOST_CHECK(hw != HighWaterDigest(domain, 2, 0, 9, 2) && hw != HighWaterDigest(domain, 3, 0, 9, 1));
    // Statement digests never collide with a vote digest for the same numbers.
    BOOST_CHECK(hw != PrepareDigest(domain, 0, 9, 1, header) && vc != PrepareDigest(domain, 0, 5, 3, header));
}

BOOST_AUTO_TEST_CASE(inclusion_fence_and_evidence_are_stubs)
{
    const SeatSet seats{TestSeats({"s0", "s1", "s2", "s3"})};
    const std::vector<HeldAction> held{HeldAction{ShortId{1, 2, 3, 4, 5, 6}, 1}};
    RequireStub(InclusionList::Build(held, 9), "M4-AGR-04");
    const std::vector<Vote> votes{Vote{}};
    const std::vector<ShortId> ids{ShortId{1, 2, 3, 4, 5, 6}};
    RequireStub(InclusionList::Check(votes, seats.Size(), ids, {}), "M4-AGR-04");

    const std::vector<HighWaterStatement> statements{HighWaterStatement{1, 0, 0, 5, 0, {}}};
    RequireStub(RestartFence::Collect(statements, seats, 0), "M4-AGR-06");
    // The evidence store landed with M4-AGR-07 (evidence_tests).
}

BOOST_AUTO_TEST_SUITE_END()
