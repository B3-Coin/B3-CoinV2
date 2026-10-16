// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_TEST_UTIL_STATE_FIXTURES_H
#define B3_FLOWMESH2_TEST_UTIL_STATE_FIXTURES_H

#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/engine/engine.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/slot_fixtures.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** TEST fixtures of the state owner suites (slice R9). Synthetic values only:
 * the TEST network and keys of test/util/slot_fixtures.h, the seat set root
 * LabelHash("fm2/test/seatset") and label anchors. The genesis head equals
 * the R7 SlotFixture's genesis, so engine-level and state-level results can
 * be compared directly.
 */
namespace flowmesh2::test {

inline uint256 TestSeatSetRoot() { return LabelHash("fm2/test/seatset"); }

//! The authority of the state tests at `epoch`.
inline AuthorityVersion TestAuthority(const ProfileParams& profile, uint64_t epoch = 0)
{
    return AuthorityVersion{epoch, TestSeatSetRoot(), ProfileHash(profile)};
}

//! The genesis head of the state tests (the SlotFixture genesis: anchor 129, facts from (130, 0)).
inline state::HeadView TestGenesisHead(const ProfileParams& profile = RegtestProfile(),
                                       const std::vector<MarketDescriptor>& markets = {MarketM1(), MarketM2()})
{
    Result<state::HeadView> head{state::GenesisHead(profile, markets, TestDomain(), TestAuthority(profile), TestAnchor(129),
                                                    FactCursor{130, 0})};
    RequireOk(head, "GenesisHead");
    return std::move(head).value();
}

//! Requires an error with exactly `code` and `detail`, reading error() only on failure.
template <class T>
void RequireFailure(const Result<T>& result, ErrorCode code, std::string_view detail, std::string_view what)
{
    if (result.has_value()) BOOST_FAIL(std::string{what} + " unexpectedly succeeded");
    BOOST_CHECK_MESSAGE(result.error().code == code && result.error().detail == detail,
                        std::string{what} + ": got " + std::string{ToString(result.error().code)} + " '" +
                            std::string{result.error().detail} + "', want " + std::string{ToString(code)} + " '" +
                            std::string{detail} + "'");
}

/** A head the tests advance by hand with BuildCandidate (no state owner):
 * `actions` signs actions and builds deposit bytes, and its header and
 * state follow `head`, so ref_slot defaults to the head height.
 */
struct CandidateFixture {
    SlotFixture actions;
    engine::Engine engine;
    AuthorityVersion authority;
    state::HeadView head;

    explicit CandidateFixture(const ProfileParams& profile = RegtestProfile())
        : actions{profile},
          engine{engine::Engine::Create(profile).value()},
          authority{TestAuthority(profile)},
          head{TestGenesisHead(profile)}
    {
        BOOST_REQUIRE(head.Header() == actions.header);
    }

    //! Inputs that follow the head: facts_from = its facts_to, facts_to = the next deposit cursor.
    state::SlotInputs Inputs(std::vector<std::vector<unsigned char>> facts, std::vector<std::vector<unsigned char>> originals) const
    {
        state::SlotInputs inputs;
        inputs.authority = authority;
        inputs.anchor = TestAnchor(actions.anchor_height);
        inputs.facts_from = head.Header().facts_to;
        inputs.facts_to = FactCursor{actions.anchor_height, actions.next_fact_index};
        inputs.body.facts = std::move(facts);
        inputs.body.originals = std::move(originals);
        return inputs;
    }

    Result<state::Candidate> TryBuild(const state::SlotInputs& inputs) const { return state::BuildCandidate(engine, head, inputs); }

    state::Candidate Build(const state::SlotInputs& inputs) const
    {
        Result<state::Candidate> candidate{TryBuild(inputs)};
        RequireOk(candidate, "BuildCandidate");
        return std::move(candidate).value();
    }

    //! Make `candidate` the head, as decided by a certificate with hash `cert_hash`.
    void Advance(const state::Candidate& candidate, const uint256& cert_hash)
    {
        Result<state::HeadView> next{state::HeadView::Make(candidate.Header(), cert_hash, candidate.Next())};
        RequireOk(next, "HeadView::Make");
        head = std::move(next).value();
        actions.header = head.Header();
        actions.state = head.State().engine;
    }

    //! Deposits for alice (rUSD), bob (B3 and TOK) and carol (rUSD), as in the walking skeleton plus carol.
    std::vector<std::vector<unsigned char>> FundingFacts()
    {
        return {actions.DepositBytes(KeyAccount("alice"), TestAsset("rUSD"), 1'000'000'000, "fund/alice"),
                actions.DepositBytes(KeyAccount("bob"), AssetId::Native(), 10'000'000'000, "fund/bob/b3"),
                actions.DepositBytes(KeyAccount("bob"), TestAsset("TOK"), 500'000, "fund/bob/tok"),
                actions.DepositBytes(KeyAccount("carol"), TestAsset("rUSD"), 1'000'000'000, "fund/carol")};
    }
};

} // namespace flowmesh2::test

#endif // B3_FLOWMESH2_TEST_UTIL_STATE_FIXTURES_H
