// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_CONSENSUS_BRIDGE_PARAMS_H
#define B3COIN_CONSENSUS_BRIDGE_PARAMS_H

#include <consensus/amount.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace Consensus {

using BridgeEthAddress = std::array<unsigned char, 20>;

inline constexpr uint8_t BRIDGE_ASSET_IDENTITY_VERSION_V1{1};
inline constexpr uint8_t BRIDGE_RECIPIENT_VERSION_P2PKH_V1{1};
inline constexpr uint32_t BRIDGE_ADAPTER_VERSION_DIRECT_TOKEN_V1{1};
inline constexpr unsigned ETHEREUM_SYNC_COMMITTEE_SIZE{512};
inline constexpr unsigned ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY{342};
inline constexpr uint64_t ETHEREUM_SLOTS_PER_EPOCH{32};

/**
 * Owner-ratified identity of the first bridge-backed B3 asset. Ethereum
 * addresses are stored in their ordinary left-to-right byte order.
 *
 * One raw USDT micro-unit maps to one raw bUSD micro-unit. Keeping both
 * decimal fields at six makes the 1:1 reserve accounting exact and avoids
 * any RPC/display denomination entering consensus arithmetic.
 */
inline constexpr uint64_t BUSD_ETHEREUM_CHAIN_ID{1};
inline constexpr uint8_t BUSD_ORIGIN_DECIMALS{6};
inline constexpr uint8_t BUSD_ASSET_DECIMALS{6};
inline constexpr BridgeEthAddress BUSD_ETHEREUM_VAULT{
    0x14, 0x3f, 0x20, 0x7e, 0x23, 0xe6, 0xae, 0xbd, 0x7e, 0x97,
    0x4b, 0xe9, 0x0a, 0xc6, 0xd4, 0x34, 0xf4, 0xc7, 0xbf, 0xb6,
};
inline constexpr BridgeEthAddress BUSD_ETHEREUM_USDT{
    0xda, 0xc1, 0x7f, 0x95, 0x8d, 0x2e, 0xe5, 0x23, 0xa2, 0x20,
    0x62, 0x06, 0x99, 0x45, 0x97, 0xc1, 0x3d, 0x83, 0x1e, 0xc7,
};
inline constexpr std::array<unsigned char, 32>
    ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT_BYTES{
        0x4b, 0x36, 0x3d, 0xb9, 0x4e, 0x28, 0x61, 0x20,
        0xd7, 0x6e, 0xb9, 0x05, 0x34, 0x0f, 0xdd, 0x4e,
        0x54, 0xbf, 0xe9, 0xf0, 0x6b, 0xf3, 0x3f, 0xf6,
        0xcf, 0x5a, 0xd2, 0x7f, 0x51, 0x1b, 0xfe, 0x95,
    };
inline constexpr uint256 ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT{
    std::span<const unsigned char>{
        ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT_BYTES}};
// Independently read from releaseAuthority() and eth_getCode at Ethereum
// block 25,877,643 (0xde867e...adffe6) through PublicNode and dRPC. The same
// EOA is rescueAuthority; the vault has no owner() or proxy interface.
inline constexpr BridgeEthAddress BUSD_ETHEREUM_MANAGED_AUTHORITY{
    0x76, 0xc7, 0xa2, 0x45, 0xd0, 0xd2, 0xe4, 0xcf, 0x92, 0x40,
    0x3a, 0xf0, 0x14, 0x48, 0x25, 0xdf, 0x1c, 0xc6, 0x14, 0xf1,
};
inline constexpr std::array<unsigned char, 32>
    BUSD_ETHEREUM_VAULT_RUNTIME_CODE_HASH_BYTES{
        0x1b, 0xe2, 0x20, 0xc1, 0x8e, 0xfa, 0x4e, 0x4c,
        0xda, 0x0b, 0xb1, 0xc9, 0x12, 0xc7, 0xc4, 0x13,
        0x46, 0xf5, 0xc0, 0x4d, 0x49, 0xa3, 0x6e, 0xc2,
        0xc6, 0x8f, 0x6d, 0xdc, 0xc5, 0x58, 0x62, 0x33,
    };
inline constexpr uint256 BUSD_ETHEREUM_VAULT_RUNTIME_CODE_HASH{
    std::span<const unsigned char>{
        BUSD_ETHEREUM_VAULT_RUNTIME_CODE_HASH_BYTES}};

inline bool BridgeAddressIsNull(const BridgeEthAddress& address)
{
    return std::all_of(address.begin(), address.end(),
                       [](const unsigned char byte) { return byte == 0; });
}

/** Stable origin identity. Security adapter upgrades do not rename an asset. */
struct BridgeAssetIdentityV1 {
    uint8_t version{BRIDGE_ASSET_IDENTITY_VERSION_V1};
    uint64_t origin_chain_id{0};
    BridgeEthAddress vault_address{};
    BridgeEthAddress token_address{};
    uint8_t origin_decimals{0};
    uint8_t asset_decimals{0};

    friend bool operator==(const BridgeAssetIdentityV1&,
                           const BridgeAssetIdentityV1&) = default;
};

inline constexpr BridgeAssetIdentityV1 ETHEREUM_MAINNET_BUSD_IDENTITY{
    BRIDGE_ASSET_IDENTITY_VERSION_V1,
    BUSD_ETHEREUM_CHAIN_ID,
    BUSD_ETHEREUM_VAULT,
    BUSD_ETHEREUM_USDT,
    BUSD_ORIGIN_DECIMALS,
    BUSD_ASSET_DECIMALS,
};

inline bool BridgeAssetIdentityValid(const BridgeAssetIdentityV1& identity)
{
    return identity.version == BRIDGE_ASSET_IDENTITY_VERSION_V1 &&
           identity.origin_chain_id != 0 &&
           !BridgeAddressIsNull(identity.vault_address) &&
           !BridgeAddressIsNull(identity.token_address) &&
           identity.vault_address != identity.token_address &&
           identity.origin_decimals <= 18 && identity.asset_decimals <= 18;
}

struct EthereumForkVersionPin {
    uint64_t activation_epoch{0};
    std::array<unsigned char, 4> fork_version{};

    friend bool operator==(const EthereumForkVersionPin&,
                           const EthereumForkVersionPin&) = default;
};

/**
 * Consensus trust/configuration pins for the finalized Ethereum light client.
 * `fork_schedule_valid_through_epoch` is an explicit stop line: stage-4
 * validation must fail closed beyond it instead of reusing the last known
 * signing domain across an unknown Ethereum fork.
 */
struct EthereumLightClientPins {
    uint256 trusted_checkpoint_root{};
    uint64_t trusted_checkpoint_slot{0};
    uint256 genesis_validators_root{};
    std::vector<EthereumForkVersionPin> fork_schedule{};
    uint64_t fork_schedule_valid_through_epoch{0};
    uint64_t electra_epoch{std::numeric_limits<uint64_t>::max()};
    unsigned min_sync_committee_participants{0};
    uint64_t max_sync_lag_slots{0};

    bool Valid() const
    {
        if (trusted_checkpoint_root.IsNull() || trusted_checkpoint_slot == 0 ||
            genesis_validators_root.IsNull() || fork_schedule.empty() ||
            min_sync_committee_participants < ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY ||
            min_sync_committee_participants > ETHEREUM_SYNC_COMMITTEE_SIZE ||
            max_sync_lag_slots == 0 || fork_schedule.front().activation_epoch != 0 ||
            fork_schedule_valid_through_epoch < fork_schedule.back().activation_epoch ||
            trusted_checkpoint_slot / ETHEREUM_SLOTS_PER_EPOCH >
                fork_schedule_valid_through_epoch) {
            return false;
        }
        for (size_t i{1}; i < fork_schedule.size(); ++i) {
            if (fork_schedule[i - 1].activation_epoch >=
                fork_schedule[i].activation_epoch) {
                return false;
            }
        }
        return electra_epoch == std::numeric_limits<uint64_t>::max() ||
               electra_epoch <= fork_schedule_valid_through_epoch;
    }
};

/** Caps are denominated in raw bUSD units (six decimals). */
struct BridgeMintCaps {
    CAmount max_per_block{0};
    CAmount max_per_epoch{0};
    uint32_t epoch_length_blocks{0};

    bool Valid() const
    {
        return max_per_block > 0 && max_per_block <= MAX_MONEY &&
               max_per_epoch >= max_per_block && max_per_epoch <= MAX_MONEY &&
               epoch_length_blocks > 0;
    }
};

enum class BridgeWithdrawalMode : uint8_t {
    MANAGED_V1 = 1,
    DECENTRALIZED_VERIFIER_V1 = 2,
};

inline constexpr uint32_t MANAGED_WITHDRAWAL_RULES_VERSION_V1{1};
inline constexpr uint32_t DECENTRALIZED_WITHDRAWAL_RULES_VERSION_V1{1};
// The deployment verifier deliberately permits only the gas-benchmarked
// bridge-authorizing subset of B3's larger (8,192-member) finality-set shape.
inline constexpr uint32_t MIN_BRIDGE_VALIDATORS_V1{4};
inline constexpr uint32_t MAX_PROVEN_BRIDGE_VALIDATORS_V1{64};

/**
 * Historical managed-v1 withdrawal envelope. The authority is the exact
 * immutable releaseAuthority read from that vault, never a configured
 * substitute. Production decentralized deployments must not populate this
 * mutually exclusive mode.
 */
struct BridgeManagedWithdrawalPins {
    BridgeEthAddress authority_address{};
    uint256 vault_runtime_code_hash{};
    uint32_t withdrawal_rules_version{0};
    uint256 withdrawal_rules_commitment{};

    bool Valid() const
    {
        return !BridgeAddressIsNull(authority_address) &&
               !vault_runtime_code_hash.IsNull() &&
               withdrawal_rules_version == MANAGED_WITHDRAWAL_RULES_VERSION_V1 &&
               !withdrawal_rules_commitment.IsNull();
    }
};

/**
 * Decentralized withdrawal mode. The verifier address and code hash bind what
 * the vault delegates to. The bootstrap-set hash is the deployment-time
 * four-key trust root: it is known before M, while canonical Set_0 is accepted
 * later only through the verifier's one-time 3-of-4 initialization. The rules
 * commitment binds the cross-language withdrawal layout, the economic floor
 * prevents the synthetic bootstrap set from authorizing withdrawals, and
 * max_epoch_lag makes a stale verifier halt safely.
 */
struct BridgeDecentralizedWithdrawalPins {
    BridgeEthAddress ethereum_verifier_address{};
    uint256 ethereum_verifier_code_hash{};
    uint256 bootstrap_validator_set_hash{};
    uint32_t withdrawal_rules_version{0};
    uint256 withdrawal_rules_commitment{};
    // Exact deployment-time bounds mirrored from B3FinalityVerifier. A B3
    // set outside this interval remains valid for chain finality but cannot
    // safely authorize an Ethereum withdrawal root in this deployment.
    uint32_t min_bridge_validators{0};
    uint32_t max_bridge_validators{0};
    // Same unit as ValidatorSetHeader::total_weight and the verifier's
    // MIN_BRIDGE_TOTAL_WEIGHT: whole modern B3, not legacy/base units.
    uint64_t min_bridge_total_weight{0};
    uint32_t max_epoch_lag{0};

    bool Valid() const
    {
        return !BridgeAddressIsNull(ethereum_verifier_address) &&
               !ethereum_verifier_code_hash.IsNull() &&
               !bootstrap_validator_set_hash.IsNull() &&
               withdrawal_rules_version ==
                   DECENTRALIZED_WITHDRAWAL_RULES_VERSION_V1 &&
               !withdrawal_rules_commitment.IsNull() &&
               min_bridge_validators >= MIN_BRIDGE_VALIDATORS_V1 &&
               max_bridge_validators >= min_bridge_validators &&
               max_bridge_validators <= MAX_PROVEN_BRIDGE_VALIDATORS_V1 &&
               min_bridge_total_weight > 0 && max_epoch_lag > 0;
    }
};

/**
 * Complete parameter envelope for one bridge-backed asset. The immutable
 * `asset` fields are sufficient to derive its chain-bound AssetId. Every
 * optional below is a separate fail-closed inbound/contract pin and remains
 * unset until its exact value has been reviewed and ratified. The independent
 * outbound burn height W lives in Params so a later release can enable it
 * without changing this registry interval's identity.
 */
struct BridgeAssetParams {
    BridgeAssetIdentityV1 asset{};
    // First Ethereum block that may contain events from the pinned vault.
    // Zero is an incomplete-manifest stop marker, never a deployable origin.
    // Relayers must begin their initial log scan exactly here. Keeping this a
    // release pin prevents an operator typo from silently skipping deposits.
    std::optional<uint64_t> origin_deployment_block{};
    // Exact extcodehash of the immutable vault at asset.vault_address. An
    // address by itself is not a sufficient production pin: activation must
    // bind the reviewed bytecode that receives deposits and releases funds.
    std::optional<uint256> vault_runtime_code_hash{};
    std::optional<uint256> implementation_or_adapter{};
    std::optional<uint32_t> adapter_version{};
    std::optional<uint8_t> recipient_encoding_version{};
    std::optional<int32_t> activation_height{};
    std::optional<int32_t> approval_last_height{}; // inclusive; unset = no expiry
    std::optional<BridgeMintCaps> mint_caps{};
    std::optional<EthereumLightClientPins> light_client{};
    std::optional<BridgeWithdrawalMode> withdrawal_mode{};
    std::optional<BridgeManagedWithdrawalPins> managed_withdrawal{};
    std::optional<BridgeDecentralizedWithdrawalPins> decentralized_withdrawal{};
};

inline bool BridgeRegistryPinsValid(const BridgeAssetParams& params)
{
    return BridgeAssetIdentityValid(params.asset) &&
           params.origin_deployment_block &&
           *params.origin_deployment_block > 0 &&
           params.vault_runtime_code_hash &&
           !params.vault_runtime_code_hash->IsNull() &&
           params.implementation_or_adapter &&
           !params.implementation_or_adapter->IsNull() &&
           params.adapter_version &&
           *params.adapter_version == BRIDGE_ADAPTER_VERSION_DIRECT_TOKEN_V1 &&
           params.recipient_encoding_version &&
           *params.recipient_encoding_version == BRIDGE_RECIPIENT_VERSION_P2PKH_V1 &&
           params.activation_height && *params.activation_height >= 0 &&
           (!params.approval_last_height ||
            *params.approval_last_height >= *params.activation_height);
}

/** True only when every shared inbound/contract pin represented here exists. */
inline bool BridgeMintParamsReady(const BridgeAssetParams& params)
{
    if (!BridgeRegistryPinsValid(params) || !params.mint_caps ||
        !params.mint_caps->Valid() || !params.light_client ||
        !params.light_client->Valid() || !params.withdrawal_mode) {
        return false;
    }
    // The first production bridge is specifically Ethereum-mainnet USDT with
    // the same fixed six-decimal identity compiled into B3StakerBridge. The
    // beacon genesis root makes an accidental testnet light client fail closed
    // even if the numeric origin chain id was copied as 1.
    if (params.asset.origin_decimals != BUSD_ORIGIN_DECIMALS ||
        params.asset.asset_decimals != BUSD_ASSET_DECIMALS) {
        return false;
    }
    if (params.asset.origin_chain_id == BUSD_ETHEREUM_CHAIN_ID &&
        (params.asset.token_address != BUSD_ETHEREUM_USDT ||
         params.light_client->genesis_validators_root !=
             ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT)) {
        return false;
    }
    switch (*params.withdrawal_mode) {
    case BridgeWithdrawalMode::MANAGED_V1:
        return params.managed_withdrawal && params.managed_withdrawal->Valid() &&
               params.managed_withdrawal->vault_runtime_code_hash ==
                   *params.vault_runtime_code_hash &&
               !params.decentralized_withdrawal;
    case BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1:
        return params.decentralized_withdrawal &&
               params.decentralized_withdrawal->Valid() &&
               params.decentralized_withdrawal->ethereum_verifier_address !=
                   params.asset.vault_address &&
               params.decentralized_withdrawal->ethereum_verifier_address !=
                   params.asset.token_address &&
               !params.managed_withdrawal;
    }
    return false;
}

} // namespace Consensus

#endif // B3COIN_CONSENSUS_BRIDGE_PARAMS_H
