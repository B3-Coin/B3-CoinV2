// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_ADMISSION_BUDGET_H
#define B3_FLOWMESH2_ADMISSION_BUDGET_H

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

/** Admission budgets (slice R12; ARCHITECTURE.md section 10.1 step A7; plan
 * section 3.8, PLAN-9, SEC-3, SEC-4, D13).
 *
 * A BudgetPolicy decides at step A7, after the cheap checks (account
 * exists, nonce window, expiry) and BEFORE the BIP340 signature check,
 * whether one more admission attempt for an account may spend a signature
 * verification and a pool slot. It is anti-DoS policy, never consensus: a
 * budget refusal is the admission code BUDGET ("not admissible now"), and
 * no certified data depends on it.
 *
 * Contract for implementations:
 * - Check(request) answers ALLOW or DENY. It must be cheap and must not
 *   block or call back into admission, the pool, the state owner or the
 *   read service; an Error is propagated by Admission::Admit unchanged (a
 *   stub's NotImplemented, for example).
 * - Settle(request, admitted) is called exactly once for every Check that
 *   answered ALLOW, after admission decided: admitted = true iff the reply
 *   is ADMITTED (also false when Admit returns an Error after the check).
 *   A policy that charges per attempt does it in Check; one that charges
 *   per admitted action does it in Settle.
 * - Both may be called concurrently from several admission threads; an
 *   implementation synchronises itself (Bitcoin Mutex or atomics) and holds
 *   no lock across a call out.
 *
 * Implementations in the outline:
 * - dev::UnlimitedDevBudget (dev/dev_budget.h, fm2_dev): REGTEST only,
 *   always ALLOW; counts calls for tests.
 * - CreditBudget (below): the plan's HL-like account credits (10,000
 *   initial credits per account, 1 credit per 1 Q of traded notional, a
 *   reserved 10% cancel lane up to the account's open orders). Stub:
 *   NotImplemented(M4-ADM-02); the economics are BLOCKED-ON-RULING (SEC-4,
 *   D13).
 */
namespace flowmesh2::admission {

//! What one admission attempt asks the budget for.
struct BudgetRequest {
    AccountId account;
    ActionKind kind{ActionKind::PLACE};
    size_t frame_bytes{0};    //!< exact original bytes
    uint64_t head_height{0};  //!< the snapshot the attempt was checked against (A4..A6)
    uint32_t open_orders{0};  //!< the account's open orders at that snapshot (the cancel lane needs it)
    friend bool operator==(const BudgetRequest&, const BudgetRequest&) = default;
};

//! A budget's answer. APPEND-ONLY; zero is never a value.
enum class BudgetVerdict : uint8_t {
    ALLOW = 1,
    DENY = 2, //!< admission answers BUDGET
};
std::string_view ToString(BudgetVerdict verdict);

class BudgetPolicy
{
public:
    virtual ~BudgetPolicy() = default;
    //! Step A7 (see the file comment).
    virtual Result<BudgetVerdict> Check(const BudgetRequest& request) = 0;
    //! Exactly once per ALLOW (see the file comment).
    virtual void Settle(const BudgetRequest& request, bool admitted) = 0;
};

//! Parameters of the credit budget (target shape; PROVISIONAL, SEC-4).
struct CreditBudgetParams {
    uint64_t initial_credits{10'000};      //!< per account
    uint64_t credits_per_quote_unit{1};    //!< earned per 1 Q of traded notional
    uint32_t cancel_lane_percent{10};      //!< reserved for cancels, up to the account's open orders
    friend bool operator==(const CreditBudgetParams&, const CreditBudgetParams&) = default;
};

/** The account credit budget of plan section 3.8. Outline stub: Create
 * returns NotImplemented(M4-ADM-02), so no instance exists; the methods are
 * the stub's target shape and return NotImplemented(M4-ADM-02) too.
 */
class CreditBudget final : public BudgetPolicy
{
public:
    static Result<std::unique_ptr<CreditBudget>> Create(const CreditBudgetParams& params, const ProfileParams& profile);

    Result<BudgetVerdict> Check(const BudgetRequest& request) override;
    void Settle(const BudgetRequest& request, bool admitted) override;

private:
    CreditBudget() = default;
};

} // namespace flowmesh2::admission

#endif // B3_FLOWMESH2_ADMISSION_BUDGET_H
