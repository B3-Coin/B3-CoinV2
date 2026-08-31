// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_WALLET_RPC_FLOWMESH_H
#define BITCOIN_WALLET_RPC_FLOWMESH_H

class UniValue;
namespace interfaces {
struct FlowMeshVaultOperation;
}

namespace wallet {

//! Stable JSON projection shared by the discovery RPC and its focused test.
UniValue FlowMeshVaultOperationToJSON(
    const interfaces::FlowMeshVaultOperation& operation);

} // namespace wallet

#endif // BITCOIN_WALLET_RPC_FLOWMESH_H
