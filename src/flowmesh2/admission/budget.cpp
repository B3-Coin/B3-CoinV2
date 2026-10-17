// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/admission/budget.h>

namespace flowmesh2::admission {

std::string_view ToString(BudgetVerdict verdict)
{
    switch (verdict) {
    case BudgetVerdict::ALLOW: return "ALLOW";
    case BudgetVerdict::DENY: return "DENY";
    }
    return "UNKNOWN";
}

Result<std::unique_ptr<CreditBudget>> CreditBudget::Create(const CreditBudgetParams&, const ProfileParams&)
{
    // TODO(rebirth:M4-ADM-02) account credits: initial credits, credits per traded Q, the reserved cancel lane.
    return NotImplemented("M4-ADM-02");
}

Result<BudgetVerdict> CreditBudget::Check(const BudgetRequest&)
{
    return NotImplemented("M4-ADM-02"); // TODO(rebirth:M4-ADM-02) unreachable until Create makes an instance
}

void CreditBudget::Settle(const BudgetRequest&, bool) {}

} // namespace flowmesh2::admission
