// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/bridge/bridge_manager.h>

namespace flowmesh2::bridge {

Result<DepositFact> BridgeManagerV2::OnFinalizedMint(const FinalizedMint& /*mint*/)
{
    return NotImplemented("M8-BRG-01"); // TODO(rebirth:M8-BRG-01)
}

Result<WithdrawalPacket> BridgeManagerV2::BuildWithdrawalPacket(const WithdrawalRequest& /*request*/)
{
    return NotImplemented("M8-BRG-02"); // TODO(rebirth:M8-BRG-02)
}

Result<void> BridgeManagerV2::CheckReleaseLimits(const WithdrawalRequest& /*request*/, const ReleaseWindow& /*window*/,
                                                 const ReleaseLimits& /*limits*/) const
{
    return NotImplemented("M8-BRG-03"); // TODO(rebirth:M8-BRG-03)
}

} // namespace flowmesh2::bridge
