// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_DEV_DEV_BUDGET_H
#define B3_FLOWMESH2_DEV_DEV_BUDGET_H

#ifndef FM2_DEV_COMPONENTS
#error "dev/dev_budget.h is a REGTEST-only dev component: only the fm2_dev target (FM2_DEV_COMPONENTS) and its users may include it"
#endif

#include <flowmesh2/admission/budget.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>

/** UnlimitedDevBudget: the REGTEST budget that allows every admission
 * attempt (slice R12). DEV ONLY: it lives in fm2_dev, compiles only with
 * FM2_DEV_COMPONENTS, and guard rule 3 keeps its symbols out of fm2_core
 * and fm2_verify. With it, admission's anti-DoS before the signature check
 * is only "the account exists and the nonce and window fit" (A4..A6); a
 * real budget is CreditBudget (M4-ADM-02).
 *
 * Create(profile) refuses a profile of any chain but REGTEST
 * (UNSUPPORTED_NETWORK, DEV_BUDGET_NETWORK), then a profile Validate
 * refuses (its error). Check always answers ALLOW; Settle counts. The
 * counters are atomics, so admission threads may call it concurrently; it
 * takes no lock.
 */
namespace flowmesh2::dev {

inline constexpr std::string_view DEV_BUDGET_NETWORK{"the unlimited dev budget runs on REGTEST only"}; //!< (UNSUPPORTED_NETWORK)

class UnlimitedDevBudget final : public admission::BudgetPolicy
{
public:
    static Result<std::unique_ptr<UnlimitedDevBudget>> Create(const ProfileParams& profile);

    //! ALLOW, always; counts the call.
    Result<admission::BudgetVerdict> Check(const admission::BudgetRequest& request) override;
    //! Counts an admitted or refused attempt.
    void Settle(const admission::BudgetRequest& request, bool admitted) override;

    uint64_t Checks() const { return m_checks.load(); }
    uint64_t Admitted() const { return m_admitted.load(); }
    uint64_t Refused() const { return m_refused.load(); }

private:
    UnlimitedDevBudget() = default;

    std::atomic<uint64_t> m_checks{0};
    std::atomic<uint64_t> m_admitted{0};
    std::atomic<uint64_t> m_refused{0};
};

} // namespace flowmesh2::dev

#endif // B3_FLOWMESH2_DEV_DEV_BUDGET_H
