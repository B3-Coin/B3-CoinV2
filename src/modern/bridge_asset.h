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
 * Identity of one complete approved registry interval. Unlike the asset id,
 * this commits every mint, light-client, and withdrawal safety pin. Replacing
 * any of them therefore creates a new auditable approval identity without
 * renaming users' bUSD balances.
 */
inline std::optional<uint256> BridgeRegistryIdV1(
    const uint256& chain_domain, const Consensus::BridgeAssetParams& params)
{
    if (!Consensus::BridgeMintParamsReady(params)) return std::nullopt;
    HashWriter writer{TaggedHash("B3/BRIDGE/REGISTRY/V1")};
    writer << chain_domain << BridgeAssetIdV1(chain_domain, params.asset)
           << *params.origin_deployment_block
           << *params.vault_runtime_code_hash
           << *params.implementation_or_adapter << *params.adapter_version
           << *params.recipient_encoding_version << *params.activation_height;
    writer << static_cast<uint8_t>(params.approval_last_height.has_value());
    if (params.approval_last_height) writer << *params.approval_last_height;

    const auto& caps{*params.mint_caps};
    writer << caps.max_per_block << caps.max_per_epoch
           << caps.epoch_length_blocks;

    const auto& light{*params.light_client};
    writer << light.trusted_checkpoint_root << light.trusted_checkpoint_slot
           << light.genesis_validators_root
           << static_cast<uint32_t>(light.fork_schedule.size());
    for (const auto& fork : light.fork_schedule) {
        writer << fork.activation_epoch << fork.fork_version;
    }
    writer << light.fork_schedule_valid_through_epoch << light.electra_epoch
           << static_cast<uint32_t>(light.min_sync_committee_participants)
           << light.max_sync_lag_slots;

    writer << static_cast<uint8_t>(*params.withdrawal_mode);
    if (*params.withdrawal_mode ==
        Consensus::BridgeWithdrawalMode::MANAGED_V1) {
        const auto& withdrawal{*params.managed_withdrawal};
        writer << withdrawal.authority_address
               << withdrawal.vault_runtime_code_hash
               << withdrawal.withdrawal_rules_version
               << withdrawal.withdrawal_rules_commitment;
    } else {
        const auto& withdrawal{*params.decentralized_withdrawal};
        writer << withdrawal.ethereum_verifier_address
               << withdrawal.ethereum_verifier_code_hash
               << withdrawal.bootstrap_validator_set_hash
               << withdrawal.withdrawal_rules_version
               << withdrawal.withdrawal_rules_commitment
               << withdrawal.min_bridge_validators
               << withdrawal.max_bridge_validators
               << withdrawal.min_bridge_total_weight
               << withdrawal.max_epoch_lag;
    }
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

/**
 * Wallet/display identity for the current keyless bridge only. The mainnet
 * tree may retain an incomplete historical managed-vault identity as a
 * fail-closed audit record; it must not make that excluded asset look like
 * the production bUSD ticker. Recognition begins only when the complete
 * decentralized parameter envelope is present. This is metadata, not a
 * height-activation predicate.
 */
inline std::optional<AssetId> ConfiguredDecentralizedBridgeAssetId(
    const Consensus::Params& params)
{
    if (!params.busd_bridge ||
        !Consensus::BridgeMintParamsReady(*params.busd_bridge) ||
        !params.busd_bridge->withdrawal_mode ||
        *params.busd_bridge->withdrawal_mode !=
            Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1) {
        return std::nullopt;
    }
    return ConfiguredBridgeAssetId(params);
}

inline std::optional<uint256> ConfiguredBridgeRegistryId(const Consensus::Params& params)
{
    if (!params.busd_bridge || !Consensus::BridgeMintParamsReady(*params.busd_bridge) ||
        !params.legacy_final_hash) {
        return std::nullopt;
    }
    const auto domain{ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)};
    if (!domain) return std::nullopt;
    return BridgeRegistryIdV1(*domain, *params.busd_bridge);
}

} // namespace modern

#endif // B3COIN_MODERN_BRIDGE_ASSET_H
