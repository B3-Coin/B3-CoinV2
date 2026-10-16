// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/chain/b3d_adapter.h>

#include <util/chaintype.h>

namespace flowmesh2::chain {

Result<std::unique_ptr<ChainAdapter>> B3dTrustedFactsAdapter::Create(const NetworkId& network, const B3dAdapterConfig& /*config*/)
{
    // Trusted facts are a regtest-only mode (plan 3.3, Security M4).
    if (network.chain != ChainType::REGTEST) return Error{ErrorCode::UNSUPPORTED_NETWORK, "trusted facts are REGTEST only"};
    return NotImplemented("MB1-ADP-06"); // TODO(rebirth:MB1-ADP-06)
}

Result<std::unique_ptr<ChainAdapter>> B3dFinalizedAdapter::Create(const NetworkId& /*network*/, const B3dAdapterConfig& /*config*/)
{
    return NotImplemented("M7-ADP-01"); // TODO(rebirth:M7-ADP-01)
}

} // namespace flowmesh2::chain
