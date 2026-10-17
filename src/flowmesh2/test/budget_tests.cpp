// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** Admission budgets (slice R12; admission/budget.h, dev/dev_budget.h):
 * the verdict registry, the REGTEST-only unlimited dev budget, and the
 * credit budget's target parameters (its stub is in stub_inventory,
 * M4-ADM-02).
 */

#include <boost/test/unit_test.hpp>

#include <flowmesh2/admission/budget.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/dev/dev_budget.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/state_fixtures.h>

#include <memory>
#include <thread>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;
using admission::BudgetRequest;
using admission::BudgetVerdict;

BOOST_AUTO_TEST_SUITE(budget_tests)

BOOST_AUTO_TEST_CASE(verdict_registry_and_credit_parameters)
{
    BOOST_CHECK_EQUAL(static_cast<int>(BudgetVerdict::ALLOW), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(BudgetVerdict::DENY), 2);
    BOOST_CHECK_EQUAL(admission::ToString(BudgetVerdict::ALLOW), "ALLOW");
    BOOST_CHECK_EQUAL(admission::ToString(BudgetVerdict::DENY), "DENY");
    BOOST_CHECK_EQUAL(admission::ToString(static_cast<BudgetVerdict>(0)), "UNKNOWN");
    BOOST_CHECK_EQUAL(admission::ToString(static_cast<BudgetVerdict>(3)), "UNKNOWN");
    // The plan's PROPOSED HL-like defaults (section 3.8; SEC-4 OPEN).
    const admission::CreditBudgetParams params;
    BOOST_CHECK_EQUAL(params.initial_credits, 10'000U);
    BOOST_CHECK_EQUAL(params.credits_per_quote_unit, 1U);
    BOOST_CHECK_EQUAL(params.cancel_lane_percent, 10U);
}

BOOST_AUTO_TEST_CASE(unlimited_dev_budget_is_regtest_only)
{
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4, ChainType::SIGNET}) {
        ProfileParams profile{RegtestProfile()};
        profile.chain = chain;
        RequireFailure(dev::UnlimitedDevBudget::Create(profile), ErrorCode::UNSUPPORTED_NETWORK, dev::DEV_BUDGET_NETWORK,
                       "non-REGTEST chain");
    }
    ProfileParams zero_cap{RegtestProfile()};
    zero_cap.pool_capacity = 0;
    RequireError(dev::UnlimitedDevBudget::Create(zero_cap), ErrorCode::INVALID_INPUT, "invalid profile");
    ProfileParams stub{RegtestProfile()};
    stub.futures_enabled = true;
    RequireStub(dev::UnlimitedDevBudget::Create(stub), "M9-FUT-01");
    RequireOk(dev::UnlimitedDevBudget::Create(RegtestProfile()), "REGTEST");
}

BOOST_AUTO_TEST_CASE(unlimited_dev_budget_allows_and_counts)
{
    std::unique_ptr<dev::UnlimitedDevBudget> budget{dev::UnlimitedDevBudget::Create(RegtestProfile()).value()};
    admission::BudgetPolicy& policy{*budget};
    const BudgetRequest request{KeyAccount("alice"), ActionKind::PLACE, 200, 3, 7};
    for (int i{0}; i < 5; ++i) {
        const Result<BudgetVerdict> verdict{policy.Check(request)};
        RequireOk(verdict, "Check");
        BOOST_CHECK(*verdict == BudgetVerdict::ALLOW);
    }
    // Any request, also one no admission would make.
    const Result<BudgetVerdict> odd{policy.Check(BudgetRequest{})};
    RequireOk(odd, "Check of an empty request");
    BOOST_CHECK(*odd == BudgetVerdict::ALLOW);
    policy.Settle(request, true);
    policy.Settle(request, false);
    policy.Settle(request, false);
    BOOST_CHECK_EQUAL(budget->Checks(), 6U);
    BOOST_CHECK_EQUAL(budget->Admitted(), 1U);
    BOOST_CHECK_EQUAL(budget->Refused(), 2U);

    // Admission threads call it concurrently.
    std::vector<std::thread> threads;
    for (int t{0}; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i{0}; i < 1'000; ++i) {
                (void)policy.Check(request);
                policy.Settle(request, i % 2 == 0);
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    BOOST_CHECK_EQUAL(budget->Checks(), 4'006U);
    BOOST_CHECK_EQUAL(budget->Admitted(), 2'001U);
    BOOST_CHECK_EQUAL(budget->Refused(), 2'002U);
}

BOOST_AUTO_TEST_SUITE_END()
