// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/bridge/checkpoint.h>

namespace flowmesh2::bridge {

Result<Checkpoint> CheckpointBuilder::Build(const state::HeadView& /*head*/, const SlotCertificate& /*cert*/,
                                            std::span<const WithdrawalPacket> /*packets*/)
{
    return NotImplemented("M8-CKP-01"); // TODO(rebirth:M8-CKP-01)
}

} // namespace flowmesh2::bridge
