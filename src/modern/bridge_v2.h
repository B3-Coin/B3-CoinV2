// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_BRIDGE_V2_H
#define B3COIN_MODERN_BRIDGE_V2_H

#include <consensus/bridge_params.h>
#include <consensus/params.h>
#include <hash.h>
#include <modern/chain_domain.h>
#include <modern/policy.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace modern {

/**
 * Bridge V2 identities (doc/design/b3-bridge-v2-node-changes.md section 3.2).
 *
 * - The bUSD V2 AssetId is the normative cross-language preimage: Vault V2
 *   computes the same value from address(this) (B3TypesV2.assetId), and the
 *   shared vector file contracts/test/vectors/bridge_v2_vectors.json pins it.
 * - The vault entry id is what kind 3 and kind 6 records name in their
 *   registry_id field; a successor vault or code hash yields another id.
 * - The registry id V2 is the approval identity over every pin except V and
 *   W2, followed by the light-client extension list's hash; records never
 *   name it (and this slice's value is provisional, see BridgeRegistryIdV2).
 *
 * All three are TaggedHash(tag) over the chain's modern domain followed by
 * Bitcoin serialization (little-endian integers, raw 20-byte addresses and
 * raw 32-byte hashes), as BridgeAssetIdV1/BridgeRegistryIdV1 do.
 */
inline constexpr const char* BRIDGE_ASSET_V2_TAG{"B3/BRIDGE/ASSET/V2"};
inline constexpr const char* BRIDGE_VAULT_V2_TAG{"B3/BRIDGE/VAULT/V2"};
inline constexpr const char* BRIDGE_REGISTRY_V2_TAG{"B3/BRIDGE/REGISTRY/V2"};
inline constexpr const char* BRIDGE_LIGHT_CLIENT_EXTENSIONS_V2_TAG{"B3/BRIDGE/LC-EXTENSIONS/V2"};

//! The AssetId message after the doubled tag hash: domain, version byte 2,
//! LE64 origin chain id, vault, token and both decimals (83 bytes; with the
//! two tag hashes, Solidity's 147-byte ASSET_ID_PREIMAGE_LENGTH).
inline constexpr size_t BRIDGE_ASSET_ID_V2_MESSAGE_SIZE{32 + 1 + 8 + 20 + 20 + 1 + 1};
static_assert(BRIDGE_ASSET_ID_V2_MESSAGE_SIZE + 64 == 147);

template <typename Stream>
void SerializeBridgeAssetIdV2Message(Stream& stream, const uint256& chain_domain,
                                     const Consensus::BridgeV2VaultEntry& entry)
{
    stream << chain_domain << Consensus::BRIDGE_ASSET_IDENTITY_VERSION_V2
           << entry.origin_chain_id << entry.vault_address
           << entry.token_address << entry.origin_decimals
           << entry.asset_decimals;
}

/** bUSD V2 AssetId: bound to the chain domain, origin chain, Vault V2 and token. */
inline AssetId BridgeAssetIdV2(const uint256& chain_domain,
                               const Consensus::BridgeV2VaultEntry& entry)
{
    HashWriter writer{TaggedHash(BRIDGE_ASSET_V2_TAG)};
    SerializeBridgeAssetIdV2Message(writer, chain_domain, entry);
    return writer.GetSHA256();
}

/**
 * Vault entry id, the registry_id that V2 mint and burn records name.
 * Deviation from node changes section 3.2 (solve-and-report): the optional
 * vault code hash (OI-3 leaves it unpinned on regtest fixtures) is preceded
 * by a presence byte, 1 with the 32-byte hash or 0 without it, so an unset
 * pin can never collide with any hash value.
 */
inline uint256 BridgeVaultEntryIdV2(const uint256& chain_domain,
                                    const Consensus::BridgeV2VaultEntry& entry)
{
    HashWriter writer{TaggedHash(BRIDGE_VAULT_V2_TAG)};
    writer << chain_domain << entry.origin_chain_id << entry.vault_address
           << entry.token_address << entry.origin_decimals
           << entry.asset_decimals
           << static_cast<uint8_t>(entry.vault_runtime_code_hash.has_value());
    if (entry.vault_runtime_code_hash) writer << *entry.vault_runtime_code_hash;
    writer << entry.proof_kind;
    return writer.GetSHA256();
}

/**
 * Hash of the append-only light-client extension list (node changes
 * sections 3.1 and 12.3): TaggedHash(tag) over a u32 entry count followed by
 * the entries. The list is empty until a fork-extension hard fork; the entry
 * encoding (B3 height, fork version, fork epoch, new horizon) is fixed when
 * the first extension lands, so only the empty list is defined here.
 */
inline uint256 BridgeLightClientExtensionsHashV2()
{
    HashWriter writer{TaggedHash(BRIDGE_LIGHT_CLIENT_EXTENSIONS_V2_TAG)};
    writer << uint32_t{0};
    return writer.GetSHA256();
}

/**
 * Registry id V2 over every pin of a valid envelope except V and W2, then
 * the light-client extension list's hash (node changes section 3.2). The
 * light-client pins follow BridgeRegistryIdV1's order. nullopt unless the
 * envelope is Valid().
 *
 * Not yet an approval identity: this slice's envelope lacks several section
 * 3.1 pins, which the id gains (and so changes value) when they land: the
 * verifier's lineage id, genesis set hash, mirrored windows and
 * BRIDGE_ACTIVATION_HEIGHT mirror, the deposit and exit forwarder pins, the
 * gate thresholds (section 8.6) and, on regtest, the vault and verifier code
 * hashes (OI-3). Records never name it; nothing may treat today's value as
 * the S7 approval identity.
 */
inline std::optional<uint256> BridgeRegistryIdV2(
    const uint256& chain_domain, const Consensus::BridgeV2Params& params)
{
    if (!params.Valid()) return std::nullopt;
    const auto& entry{params.vault};
    HashWriter writer{TaggedHash(BRIDGE_REGISTRY_V2_TAG)};
    writer << chain_domain << BridgeVaultEntryIdV2(chain_domain, entry)
           << BridgeAssetIdV2(chain_domain, entry)
           << entry.origin_deployment_block << entry.token_runtime_code_hash
           << entry.max_deposit_raw << entry.mint_caps.max_per_block
           << entry.mint_caps.max_per_epoch
           << entry.mint_caps.epoch_length_blocks;

    const auto& light{params.light_client};
    writer << light.trusted_checkpoint_root << light.trusted_checkpoint_slot
           << light.genesis_validators_root
           << static_cast<uint32_t>(light.fork_schedule.size());
    for (const auto& fork : light.fork_schedule) {
        writer << fork.activation_epoch << fork.fork_version;
    }
    writer << light.fork_schedule_valid_through_epoch << light.electra_epoch
           << static_cast<uint32_t>(light.min_sync_committee_participants)
           << light.max_sync_lag_slots;

    const auto& verifier{params.verifier};
    writer << verifier.address
           << static_cast<uint8_t>(verifier.runtime_code_hash.has_value());
    if (verifier.runtime_code_hash) writer << *verifier.runtime_code_hash;
    writer << verifier.min_bridge_validators << verifier.max_bridge_validators
           << verifier.min_bridge_total_weight;
    writer << BridgeLightClientExtensionsHashV2();
    return writer.GetSHA256();
}

namespace bridge_v2_detail {

inline std::optional<uint256> ConfiguredDomain(const Consensus::Params& params)
{
    if (!params.bridge_v2 || !params.bridge_v2->Valid() ||
        !params.legacy_final_hash) {
        return std::nullopt;
    }
    return ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash);
}

} // namespace bridge_v2_detail

//! bUSD V2 AssetId of the configured entry; nullopt unless bridge_v2 is Valid().
inline std::optional<AssetId> ConfiguredBridgeV2AssetId(const Consensus::Params& params)
{
    const auto domain{bridge_v2_detail::ConfiguredDomain(params)};
    if (!domain) return std::nullopt;
    return BridgeAssetIdV2(*domain, params.bridge_v2->vault);
}

//! Vault entry id of the configured entry; nullopt unless bridge_v2 is Valid().
inline std::optional<uint256> ConfiguredBridgeV2EntryId(const Consensus::Params& params)
{
    const auto domain{bridge_v2_detail::ConfiguredDomain(params)};
    if (!domain) return std::nullopt;
    return BridgeVaultEntryIdV2(*domain, params.bridge_v2->vault);
}

//! Registry id V2 of the configured envelope; nullopt unless bridge_v2 is Valid().
inline std::optional<uint256> ConfiguredBridgeV2RegistryId(const Consensus::Params& params)
{
    const auto domain{bridge_v2_detail::ConfiguredDomain(params)};
    if (!domain) return std::nullopt;
    return BridgeRegistryIdV2(*domain, *params.bridge_v2);
}

} // namespace modern

#endif // B3COIN_MODERN_BRIDGE_V2_H
