// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/agreement/pbft.h>

namespace flowmesh2::agreement {

Result<void> PbftSafetyRules::MayPrepare(const Proposal& /*proposal*/)
{
    return NotImplemented("M4-AGR-01"); // TODO(rebirth:M4-AGR-01)
}

Result<void> PbftSafetyRules::MayCommit(const Proposal& /*proposal*/, const PreparedCert& /*prepared*/)
{
    return NotImplemented("M4-AGR-01"); // TODO(rebirth:M4-AGR-01)
}

Result<void> PbftSafetyRules::OnViewChange(const ViewChange& /*report*/)
{
    return NotImplemented("M4-AGR-01"); // TODO(rebirth:M4-AGR-01)
}

Result<std::unique_ptr<Agreement>> PbftAgreement::Create(const ProfileParams& /*profile*/, const PbftConfig& /*config*/,
                                                         const PbftContext& /*context*/)
{
    return NotImplemented("M4-AGR-02"); // TODO(rebirth:M4-AGR-02)
}

} // namespace flowmesh2::agreement
