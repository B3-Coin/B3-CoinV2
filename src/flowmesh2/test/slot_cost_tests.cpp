// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/engine/engine.h>
#include <flowmesh2/read/views.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/slot_fixtures.h>
#include <flowmesh2/test/util/state_fixtures.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;

namespace {

using Clock = std::chrono::steady_clock;

struct SlotCost {
    double best_ms{0};
    StateRootsV0 roots;
};

/** Fund the four signers and `untouched` other accounts the timed slot
 * never reads, commit that as the genesis header and run one empty slot,
 * so the head is an engine result, as every head after genesis is.
 */
void FundAndStart(SlotFixture& f, uint32_t untouched)
{
    for (const char* name : {"alice", "bob", "carol", "dave"}) {
        RequireOk(f.state.ledger.CreditDeposit(Deposit(KeyAccount(name), TestAsset("rUSD"), 1'000'000'000,
                                                       TestOutpoint(std::string{"cost/rUSD/"} + name, 0))),
                  "fund a signer");
        RequireOk(f.state.ledger.CreditDeposit(Deposit(KeyAccount(name), TestAsset("TOK"), 1'000'000,
                                                       TestOutpoint(std::string{"cost/TOK/"} + name, 0))),
                  "fund a signer");
    }
    for (uint32_t i{0}; i < untouched; ++i) {
        RequireOk(f.state.ledger.CreditDeposit(Deposit(TestAccount("cost/untouched/" + std::to_string(i)), TestAsset("rUSD"),
                                                       1'000 + i, TestOutpoint("cost/untouched", i))),
                  "fund an untouched account");
    }
    const Result<StateRootsV0> funded{f.state.Commit()};
    RequireOk(funded, "commit the funded genesis");
    const AuthorityVersion authority{0, LabelHash("fm2/test/seatset"), f.profile_hash};
    f.header = GenesisHeader(f.domain, authority, TestAnchor(f.anchor_height - 1), FactCursor{f.anchor_height, 0},
                             funded->Root(), funded->version)
                   .value();
    f.Apply(f.Inputs({}, {}));
}

//! The timed slot: four transfers and two resting orders by four signers.
engine::SlotInputs TimedInputs(SlotFixture& f)
{
    return f.Inputs({}, {
                            f.Transfer("alice", 1, KeyAccount("bob"), TestAsset("rUSD"), 10),
                            f.Transfer("bob", 1, KeyAccount("carol"), TestAsset("rUSD"), 10),
                            f.Transfer("carol", 1, KeyAccount("dave"), TestAsset("rUSD"), 10),
                            f.Transfer("dave", 1, KeyAccount("alice"), TestAsset("rUSD"), 10),
                            f.Place("alice", 2, MarketM2(), Side::BUY, TimeInForce::GTC, 90, 5),
                            f.Place("bob", 2, MarketM2(), Side::SELL, TimeInForce::GTC, 110, 5),
                        });
}

//! The fastest of `reps` runs of the timed slot on a head with `untouched` other accounts.
SlotCost TimeOneSlot(uint32_t untouched, int reps)
{
    SlotFixture f;
    FundAndStart(f, untouched);
    const engine::SlotInputs inputs{TimedInputs(f)};
    const engine::ValidatedSlot slot{f.Validate(inputs).value()};
    const engine::Engine engine{engine::Engine::Create(f.profile).value()};
    const engine::HeadView head{f.Head()};
    SlotCost cost{1e300, {}};
    for (int rep{0}; rep < reps; ++rep) {
        const auto start{Clock::now()};
        const Result<engine::SlotResult> result{engine.ApplySlot(head, slot)};
        const double ms{std::chrono::duration<double, std::milli>(Clock::now() - start).count()};
        if (!result.has_value()) BOOST_FAIL("the timed slot failed");
        if (rep > 0 && result->roots.Root() != cost.roots.Root()) BOOST_FAIL("one slot gave two roots");
        cost.roots = result->roots;
        cost.best_ms = std::min(cost.best_ms, ms);
    }
    return cost;
}

//! The fastest of `reps` builds of the timed slot's read view, on a head with `untouched` other accounts.
double TimePublish(uint32_t untouched, int reps)
{
    SlotFixture f;
    FundAndStart(f, untouched);
    const engine::SlotResult result{f.Apply(TimedInputs(f))};
    const SlotCertificate cert{TestCertificate(f.header)};
    const auto core{std::make_shared<const state::CoreState>(state::CoreState{f.state, f.header.seat_set_root})};
    const state::HeadView head{state::HeadView::Make(f.header, cert.Hash(), core).value()};
    double best{1e300};
    for (int rep{0}; rep < reps; ++rep) {
        const auto start{Clock::now()};
        const Result<std::shared_ptr<const read::SlotView>> view{read::SlotView::Build(head, cert, result.outcomes)};
        best = std::min(best, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        if (!view.has_value()) BOOST_FAIL("the view was refused: " << view.error().detail);
        if ((*view)->AccountCount() != f.state.ledger.Accounts().size()) BOOST_FAIL("the view lost accounts");
    }
    return best;
}

} // namespace

BOOST_AUTO_TEST_SUITE(slot_cost_tests)

BOOST_AUTO_TEST_CASE(a_slot_costs_the_same_with_ten_thousand_untouched_accounts_as_with_a_hundred)
{
    // M2-LED-10: a slot's work follows what it changes, not the state's size.
    // The skeleton copied, checked and hashed every account each slot, and
    // its 10^4-account slot took about 100 times the 10^2-account one. The
    // bound leaves room for the O(log n) paths and a noisy host.
    const SlotCost small{TimeOneSlot(100, 7)};
    const SlotCost large{TimeOneSlot(10'000, 7)};
    const double ratio{large.best_ms / small.best_ms};
    BOOST_TEST_MESSAGE("one slot, fastest of 7: " << small.best_ms << " ms at 10^2 accounts, " << large.best_ms
                                                 << " ms at 10^4, ratio " << ratio);
    BOOST_CHECK_LT(ratio, 5.0);
    BOOST_CHECK(small.roots.Root() != large.roots.Root());
}

BOOST_AUTO_TEST_CASE(publishing_a_view_costs_the_same_with_ten_thousand_untouched_accounts_as_with_a_hundred)
{
    // M2-LED-02: a publish shares the head's state and its sealed levels.
    // R11's SlotView copied every account leaf and rebuilt the accounts tree.
    const double small{TimePublish(100, 5)};
    const double large{TimePublish(10'000, 5)};
    const double ratio{large / small};
    BOOST_TEST_MESSAGE("one publish, fastest of 5: " << small << " ms at 10^2 accounts, " << large << " ms at 10^4, ratio " << ratio);
    BOOST_CHECK_LT(ratio, 5.0);
}

BOOST_AUTO_TEST_SUITE_END()
