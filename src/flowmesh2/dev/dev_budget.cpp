// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/dev/dev_budget.h>

#include <util/chaintype.h>

namespace flowmesh2::dev {

Result<std::unique_ptr<UnlimitedDevBudget>> UnlimitedDevBudget::Create(const ProfileParams& profile)
{
    if (profile.chain != ChainType::REGTEST) return Error{ErrorCode::UNSUPPORTED_NETWORK, DEV_BUDGET_NETWORK};
    const Result<void> valid{Validate(profile)};
    if (!valid.has_value()) return valid.error();
    return Ok(std::unique_ptr<UnlimitedDevBudget>{new UnlimitedDevBudget{}});
}

Result<admission::BudgetVerdict> UnlimitedDevBudget::Check(const admission::BudgetRequest&)
{
    ++m_checks;
    return Ok(admission::BudgetVerdict::ALLOW);
}

void UnlimitedDevBudget::Settle(const admission::BudgetRequest&, bool admitted)
{
    if (admitted) {
        ++m_admitted;
    } else {
        ++m_refused;
    }
}

} // namespace flowmesh2::dev
