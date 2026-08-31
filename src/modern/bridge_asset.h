// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_BRIDGE_ASSET_H
#define B3COIN_MODERN_BRIDGE_ASSET_H

#include <consensus/bridge_params.h>
#include <consensus/params.h>
#include <hash.h>
#include <modern/chain_domain.h>
#include <modern/policy.h>
#include <uint256.h>

#include <optional>

namespace modern {

/**
 * Stable, chain-bound bridge asset identity. Adapter/verifier upgrades are
 * deliberately excluded: they authorize a registry interval but cannot turn
 * the same reserve asset into a different ticker or balance namespace.
 */
inline AssetId BridgeAssetIdV1(const uint256& chain_domain,
                               const Consensus::BridgeAssetIdentityV1& identity)
{
    HashWriter writer{TaggedHash("B3/BRIDGE/ASSET/V1")};
    writer << chain_domain << identity.version << identity.origin_chain_id
           << identity.vault_address << identity.token_address
           << identity.origin_decimals << identity.asset_decimals;
    return writer.GetSHA256();
}

/**
 * Identity of one approved registry interval. Unlike the asset id, this binds
 * the adapter and admission rules, so replacing any of them creates a new,
 * auditable approval identity without renaming users' bUSD balances.
 */
inline std::optional<uint256> BridgeRegistryIdV1(
    const uint256& chain_domain, const Consensus::BridgeAssetParams& params)
{
    if (!Consensus::BridgeRegistryPinsValid(params)) return std::nullopt;
    HashWriter writer{TaggedHash("B3/BRIDGE/REGISTRY/V1")};
    writer << chain_domain << BridgeAssetIdV1(chain_domain, params.asset)
           << *params.implementation_or_adapter << *params.adapter_version
           << *params.recipient_encoding_version << *params.activation_height;
    writer << static_cast<uint8_t>(params.approval_last_height.has_value());
    if (params.approval_last_height) writer << *params.approval_last_height;
    return writer.GetSHA256();
}

inline std::optional<AssetId> ConfiguredBridgeAssetId(const Consensus::Params& params)
{
    if (!params.busd_bridge || !Consensus::BridgeAssetIdentityValid(params.busd_bridge->asset) ||
        !params.legacy_final_hash) {
        return std::nullopt;
    }
    const auto domain{ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)};
    if (!domain) return std::nullopt;
    return BridgeAssetIdV1(*domain, params.busd_bridge->asset);
}

inline std::optional<uint256> ConfiguredBridgeRegistryId(const Consensus::Params& params)
{
    if (!params.busd_bridge || !Consensus::BridgeRegistryPinsValid(*params.busd_bridge) ||
        !params.legacy_final_hash) {
        return std::nullopt;
    }
    const auto domain{ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)};
    if (!domain) return std::nullopt;
    return BridgeRegistryIdV1(*domain, *params.busd_bridge);
}

} // namespace modern

#endif // B3COIN_MODERN_BRIDGE_ASSET_H
