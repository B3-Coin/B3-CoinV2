// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/governance/governance.h>

namespace flowmesh2::governance {

Result<MarketDescriptor> Governance::ApplyListing(const ListingFact& /*fact*/)
{
    return NotImplemented("M7-GOV-01"); // TODO(rebirth:M7-GOV-01)
}

Result<void> Governance::ApplyRemoval(const RemovalRequest& /*removal*/)
{
    return NotImplemented("M7-GOV-02"); // TODO(rebirth:M7-GOV-02)
}

Result<ProfileParams> Governance::ParamsAt(uint64_t /*height*/) const
{
    return NotImplemented("M7-GOV-03"); // TODO(rebirth:M7-GOV-03)
}

} // namespace flowmesh2::governance
