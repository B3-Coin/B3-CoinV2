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

/**
 * Wallet-side admission result for creating a keyless FlowMesh vault output.
 * Consensus deliberately permits the first colored market deposit during the
 * A2 runway, but ordinary user deposits must fail closed until that market's
 * runtime is live and unpaused.
 */
enum class FlowMeshDepositAdmission {
    USER_DEPOSIT,
    MARKET_BOOTSTRAP,
    RULES_INACTIVE,
    MARKET_NOT_ESTABLISHED,
    RUNTIME_UNAVAILABLE,
    MARKET_PAUSED,
    BOOTSTRAP_REQUIRES_BASE_ASSET,
    BOOTSTRAP_MARKET_ALREADY_ESTABLISHED,
};

FlowMeshDepositAdmission CheckFlowMeshDepositAdmission(
    bool market_bootstrap, bool flowmesh_rules_active,
    bool market_established, bool base_asset_deposit,
    bool runtime_ready, bool runtime_paused);

//! Stable JSON projection shared by the discovery RPC and its focused test.
UniValue FlowMeshVaultOperationToJSON(
    const interfaces::FlowMeshVaultOperation& operation);

} // namespace wallet

#endif // BITCOIN_WALLET_RPC_FLOWMESH_H
