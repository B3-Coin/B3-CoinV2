// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/merkle.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/engine/engine.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/test/util/state_fixtures.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;

namespace {

using Originals = std::vector<std::vector<unsigned char>>;

//! The walking-skeleton accounts funded in slot 1 (no certificate yet: cert hash `cert1`).
void Fund(CandidateFixture& fx, const uint256& cert1)
{
    const state::Candidate funded{fx.Build(fx.Inputs(fx.FundingFacts(), {}))};
    fx.Advance(funded, cert1);
}

//! Six originals of four accounts; two accounts have two nonces each.
Originals SixOriginals(CandidateFixture& fx)
{
    SlotFixture& a{fx.actions};
    return {a.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 250, 5'000),
            a.Place("alice", 1, MarketM2(), Side::BUY, TimeInForce::GTC, 600, 100'000),
            a.Place("alice", 2, MarketM1(), Side::BUY, TimeInForce::IOC, 260, 2'000),
            a.Place("carol", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 1, 3),
            a.Transfer("carol", 2, KeyAccount("alice"), TestAsset("rUSD"), 7),
            a.Place("bob", 2, MarketM2(), Side::SELL, TimeInForce::POST_ONLY, 900, 10)};
}

//! `originals` in engine::CanonicalOrder over the head the fixture builds on.
Originals CanonicalOf(const CandidateFixture& fx, const Originals& originals)
{
    std::vector<engine::OrderingEntry> entries;
    for (const auto& bytes : originals) {
        const ActionV2 action{ActionV2::Decode(bytes, fx.engine.Profile()).value()};
        entries.push_back(engine::OrderingEntry{action.Id(), action.Account(), action.nonce});
    }
    const std::vector<size_t> order{engine::CanonicalOrder(entries, fx.head.HeaderHash(), fx.head.Header().height + 1).value()};
    Originals canonical;
    for (const size_t index : order) canonical.push_back(originals[index]);
    return canonical;
}

} // namespace

BOOST_AUTO_TEST_SUITE(candidate_tests)

BOOST_AUTO_TEST_CASE(genesis_head_matches_the_engine_genesis)
{
    const SlotFixture engine_fx;
    const state::HeadView genesis{TestGenesisHead()};
    BOOST_CHECK(genesis.Header() == engine_fx.header);
    BOOST_CHECK(genesis.HeaderHash() == engine_fx.header.Hash());
    BOOST_CHECK(genesis.CertHash().IsNull());
    BOOST_CHECK(genesis.State().engine == engine_fx.state);
    BOOST_CHECK(genesis.State().seat_set_root == TestSeatSetRoot());
    BOOST_CHECK(genesis.State().Profile() == RegtestProfile());
    BOOST_CHECK(genesis.Header().state_root == genesis.State().engine.Commit().value().Root());
    BOOST_CHECK(state::ParentRef::Committed(genesis) == (state::ParentRef{state::ParentKind::COMMITTED, 0, genesis.HeaderHash()}));

    const ProfileParams profile{RegtestProfile()};
    const std::vector<MarketDescriptor> markets{MarketM1(), MarketM2()};
    RequireFailure(state::CoreState::Genesis(profile, markets, uint256{}), ErrorCode::INVALID_INPUT, state::GENESIS_ZERO_SEAT_SET,
                   "zero seat set root");
    AuthorityVersion other_profile{TestAuthority(profile)};
    other_profile.profile_hash = LabelHash("fm2/test/other-profile");
    RequireFailure(state::GenesisHead(profile, markets, TestDomain(), other_profile, TestAnchor(129), FactCursor{130, 0}),
                   ErrorCode::INVALID_INPUT, state::GENESIS_PROFILE_MISMATCH, "authority of another profile");
    RequireError(state::GenesisHead(profile, markets, uint256{}, TestAuthority(profile), TestAnchor(129), FactCursor{130, 0}),
                 ErrorCode::INVALID_INPUT, "zero domain (GenesisHeader)");
    RequireFailure(state::HeadView::Make(genesis.Header(), uint256{}, nullptr), ErrorCode::INVALID_INPUT, state::HEAD_STATE_MISSING,
                   "head without a state");
    ProfileParams main{profile};
    main.chain = ChainType::MAIN;
    RequireError(state::CoreState::Genesis(main, markets, TestSeatSetRoot()), ErrorCode::UNSUPPORTED_NETWORK, "MAIN profile");
}

BOOST_AUTO_TEST_CASE(candidate_header_links_and_commits)
{
    CandidateFixture fx;
    const state::HeadView genesis{fx.head};
    const state::Candidate c1{fx.Build(fx.Inputs(fx.FundingFacts(), {}))};
    const SlotHeaderV2& h{c1.Header()};
    BOOST_CHECK_EQUAL(h.version, SLOT_HEADER_VERSION);
    BOOST_CHECK(h.domain == genesis.Header().domain);
    BOOST_CHECK(h.profile_hash == ProfileHash(RegtestProfile()));
    BOOST_CHECK_EQUAL(h.authority_epoch, 0U);
    BOOST_CHECK_EQUAL(h.height, 1U);
    BOOST_CHECK(h.parent_hash == genesis.HeaderHash());
    BOOST_CHECK(h.parent_cert_hash.IsNull());
    BOOST_CHECK(h.anchor == TestAnchor(130));
    BOOST_CHECK(h.facts_from == genesis.Header().facts_to);
    BOOST_CHECK(h.facts_to == (FactCursor{130, 4}));
    BOOST_CHECK(h.seat_set_root == TestSeatSetRoot());
    BOOST_CHECK(h.facts_root == c1.Body().FactsRoot());
    BOOST_CHECK(h.actions_root == c1.Body().ActionsRoot());
    BOOST_CHECK(h.actions_root == MerkleRoot(c1.ActionLeaves()));
    BOOST_CHECK(h.outcomes_root == MerkleRoot(c1.OutcomeLeaves()));
    BOOST_CHECK(h.state_root == c1.Next()->engine.Commit().value().Root());
    BOOST_CHECK_EQUAL(h.action_count, 0U);
    BOOST_CHECK_EQUAL(h.outcome_count, 4U);
    BOOST_CHECK_EQUAL(h.commitment_version, COMMITMENT_VERSION_V0);
    BOOST_REQUIRE_EQUAL(c1.OutcomeLeaves().size(), c1.Outcomes().size());
    for (size_t i{0}; i < c1.Outcomes().size(); ++i) BOOST_CHECK(c1.OutcomeLeaves()[i] == c1.Outcomes()[i].LeafHash());
    BOOST_CHECK(c1.Next()->seat_set_root == TestSeatSetRoot());
    // Building the candidate left the parent as it was.
    BOOST_CHECK(fx.head.State() == TestGenesisHead().State());

    // Slot 2 builds on slot 1 decided by a certificate: its hash is the parent_cert_hash.
    fx.Advance(c1, LabelHash("fm2/test/cert/1"));
    const Originals originals{fx.actions.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 250, 5'000),
                              fx.actions.Place("alice", 1, MarketM2(), Side::BUY, TimeInForce::GTC, 600, 100'000)};
    const state::Candidate c2{fx.Build(fx.Inputs({}, originals))};
    BOOST_CHECK(c2.Header().parent_hash == h.Hash());
    BOOST_CHECK(c2.Header().parent_cert_hash == LabelHash("fm2/test/cert/1"));
    BOOST_CHECK_EQUAL(c2.Header().height, 2U);
    BOOST_CHECK(c2.Header().facts_from == h.facts_to && c2.Header().facts_to == h.facts_to);
    BOOST_CHECK_EQUAL(c2.Header().action_count, 2U);
    BOOST_CHECK_EQUAL(c2.Header().outcome_count, 2U);
    BOOST_REQUIRE_EQUAL(c2.ActionLeaves().size(), 2U);
    for (size_t i{0}; i < 2; ++i) BOOST_CHECK(c2.ActionLeaves()[i] == ActionLeaf(c2.Body().originals[i]));
    BOOST_CHECK(c2.Header().state_root == c2.Next()->engine.Commit().value().Root());
}

BOOST_AUTO_TEST_CASE(candidate_equals_the_engine_fixture_for_a_canonical_body)
{
    // The R7 SlotFixture assembles the next header "as R9 will" from the body
    // in the order given. With the candidate's canonical body both agree.
    CandidateFixture fx;
    SlotFixture engine_fx;
    const Originals no_originals;
    const auto facts{fx.FundingFacts()};
    engine_fx.next_fact_index = fx.actions.next_fact_index;
    const state::Candidate c1{fx.Build(fx.Inputs(facts, no_originals))};
    (void)engine_fx.Apply(engine_fx.Inputs(facts, no_originals));
    BOOST_CHECK(engine_fx.header == c1.Header());
    BOOST_CHECK(engine_fx.state == c1.Next()->engine);

    fx.Advance(c1, uint256{}); // the R7 fixture has no certificate (zero parent_cert_hash)
    const Originals originals{SixOriginals(fx)};
    const state::Candidate c2{fx.Build(fx.Inputs({}, originals))};
    (void)engine_fx.Apply(engine_fx.Inputs({}, c2.Body().originals));
    BOOST_CHECK(engine_fx.header == c2.Header());
    BOOST_CHECK(engine_fx.state == c2.Next()->engine);
}

BOOST_AUTO_TEST_CASE(permuted_originals_give_one_header_and_a_canonical_body)
{
    CandidateFixture fx;
    Fund(fx, LabelHash("fm2/test/cert/1"));
    const Originals originals{SixOriginals(fx)};
    const state::Candidate reference{fx.Build(fx.Inputs({}, originals))};
    BOOST_CHECK(reference.Body().originals == CanonicalOf(fx, originals));
    BOOST_CHECK(reference.Body().facts.empty());

    Originals permuted{originals};
    uint64_t seed{0x5eed};
    size_t non_canonical{0};
    for (int round{0}; round < 24; ++round) {
        for (size_t i{permuted.size()}; i > 1; --i) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            std::swap(permuted[i - 1], permuted[(seed >> 33) % i]);
        }
        if (permuted != reference.Body().originals) ++non_canonical;
        const state::Candidate other{fx.Build(fx.Inputs({}, permuted))};
        BOOST_CHECK(other.Header() == reference.Header());
        BOOST_CHECK(other.Body() == reference.Body());
        BOOST_CHECK(other.Outcomes() == reference.Outcomes());
        BOOST_CHECK(*other.Next() == *reference.Next());
    }
    BOOST_CHECK(non_canonical > 0);

    // Re-execution accepts the canonical body and refuses the same originals in another order.
    const Result<state::Candidate> again{state::ReexecuteOn(fx.engine, fx.head, reference.Header(), reference.Body())};
    RequireOk(again, "re-execute the canonical body");
    BOOST_CHECK(again->Header() == reference.Header());
    BOOST_CHECK(*again->Next() == *reference.Next());
    SlotBody reordered{reference.Body()};
    std::swap(reordered.originals[0], reordered.originals[1]);
    RequireFailure(state::ReexecuteOn(fx.engine, fx.head, reference.Header(), reordered), ErrorCode::VERIFY_FAILED,
                   state::REEXECUTE_BODY_MISMATCH, "body not in canonical order");
}

BOOST_AUTO_TEST_CASE(authority_rules)
{
    CandidateFixture fx;
    // The epoch may grow; the header carries it.
    state::SlotInputs bumped{fx.Inputs({}, {})};
    bumped.authority.epoch = 2;
    const state::Candidate c{fx.Build(bumped)};
    BOOST_CHECK_EQUAL(c.Header().authority_epoch, 2U);
    BOOST_CHECK(c.Header().Authority() == bumped.authority);
    fx.Advance(c, LabelHash("fm2/test/cert/1"));
    fx.authority.epoch = 2;
    RequireOk(fx.TryBuild(fx.Inputs({}, {})), "same epoch as the parent");

    state::SlotInputs regressed{fx.Inputs({}, {})};
    regressed.authority.epoch = 1;
    RequireFailure(fx.TryBuild(regressed), ErrorCode::INVALID_INPUT, state::SLOT_EPOCH_REGRESSED, "epoch below the parent's");
    state::SlotInputs seats{fx.Inputs({}, {})};
    seats.authority.seat_set_root = LabelHash("fm2/test/other-seats");
    RequireFailure(fx.TryBuild(seats), ErrorCode::INVALID_INPUT, state::SLOT_SEAT_SET_MISMATCH, "seat set not the state's");
    state::SlotInputs profile{fx.Inputs({}, {})};
    profile.authority.profile_hash = LabelHash("fm2/test/other-profile");
    RequireFailure(fx.TryBuild(profile), ErrorCode::INVALID_INPUT, engine::SLOT_PROFILE_MISMATCH, "profile hash not the engine's");
}

BOOST_AUTO_TEST_CASE(reexecute_on_checks_genesis_and_parent)
{
    CandidateFixture fx;
    Fund(fx, LabelHash("fm2/test/cert/1"));
    const state::Candidate c{fx.Build(fx.Inputs({}, SixOriginals(fx)))};
    RequireFailure(state::ReexecuteOn(fx.engine, fx.head, TestGenesisHead().Header(), SlotBody{}), ErrorCode::INVALID_INPUT,
                   state::REEXECUTE_GENESIS, "genesis header");
    SlotHeaderV2 other_parent{c.Header()};
    other_parent.parent_hash = LabelHash("fm2/test/other-parent");
    RequireFailure(state::ReexecuteOn(fx.engine, fx.head, other_parent, c.Body()), ErrorCode::STALE_PARENT, state::PARENT_NOT_HEAD,
                   "another parent");
    SlotHeaderV2 skipped{c.Header()};
    skipped.height += 1;
    RequireFailure(state::ReexecuteOn(fx.engine, fx.head, skipped, c.Body()), ErrorCode::STALE_PARENT, state::PARENT_NOT_HEAD,
                   "a height that skips one");
    // The parent certificate is part of the header: a follower holding another
    // certificate for the same parent re-executes to another header (O-51).
    SlotHeaderV2 other_cert{c.Header()};
    other_cert.parent_cert_hash = LabelHash("fm2/test/cert/other");
    RequireFailure(state::ReexecuteOn(fx.engine, fx.head, other_cert, c.Body()), ErrorCode::VERIFY_FAILED,
                   state::REEXECUTE_HEADER_MISMATCH, "another parent certificate");
}

BOOST_AUTO_TEST_SUITE_END()
