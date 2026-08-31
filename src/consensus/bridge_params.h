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
inline constexpr unsigned ETHEREUM_SYNC_COMMITTEE_SIZE{512};
inline constexpr unsigned ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY{342};

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
            fork_schedule_valid_through_epoch < fork_schedule.back().activation_epoch) {
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

/**
 * Transition-v1 withdrawal mode accepted by the owner. The authority is the
 * exact immutable releaseAuthority read from the deployed vault, never a
 * configured substitute. The runtime-code hash binds the inspected vault
 * implementation, while the version plus commitment binds the off-chain
 * operational rules under which that authority may release reserves.
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
 * Future decentralized withdrawal mode. The verifier address and code hash
 * bind what the vault delegates to; the two roots bind the B3-side set genesis
 * and cross-language withdrawal rules; the economic floor prevents a
 * four-seat bootstrap set from controlling the vault; and max_epoch_lag makes
 * a stale verifier halt safely.
 */
struct BridgeDecentralizedWithdrawalPins {
    BridgeEthAddress ethereum_verifier_address{};
    uint256 ethereum_verifier_code_hash{};
    uint256 b3_genesis_validator_set_root{};
    uint32_t withdrawal_rules_version{0};
    uint256 withdrawal_rules_commitment{};
    CAmount min_b3_validator_stake{0};
    uint32_t max_epoch_lag{0};

    bool Valid() const
    {
        return !BridgeAddressIsNull(ethereum_verifier_address) &&
               !ethereum_verifier_code_hash.IsNull() &&
               !b3_genesis_validator_set_root.IsNull() &&
               withdrawal_rules_version ==
                   DECENTRALIZED_WITHDRAWAL_RULES_VERSION_V1 &&
               !withdrawal_rules_commitment.IsNull() &&
               min_b3_validator_stake > 0 &&
               min_b3_validator_stake <= MAX_MONEY && max_epoch_lag > 0;
    }
};

/**
 * Complete parameter envelope for one bridge-backed asset. The immutable
 * `asset` fields are sufficient to derive its chain-bound AssetId. Every
 * optional below is a separate fail-closed production gate and remains unset
 * until its exact value has been reviewed and ratified.
 */
struct BridgeAssetParams {
    BridgeAssetIdentityV1 asset{};
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
           params.implementation_or_adapter &&
           !params.implementation_or_adapter->IsNull() &&
           params.adapter_version && *params.adapter_version != 0 &&
           params.recipient_encoding_version &&
           *params.recipient_encoding_version == BRIDGE_RECIPIENT_VERSION_P2PKH_V1 &&
           params.activation_height && *params.activation_height >= 0 &&
           (!params.approval_last_height ||
            *params.approval_last_height >= *params.activation_height);
}

/** True only when every production gate represented by this foundation exists. */
inline bool BridgeMintParamsReady(const BridgeAssetParams& params)
{
    if (!BridgeRegistryPinsValid(params) || !params.mint_caps ||
        !params.mint_caps->Valid() || !params.light_client ||
        !params.light_client->Valid() || !params.withdrawal_mode) {
        return false;
    }
    switch (*params.withdrawal_mode) {
    case BridgeWithdrawalMode::MANAGED_V1:
        return params.managed_withdrawal && params.managed_withdrawal->Valid() &&
               !params.decentralized_withdrawal;
    case BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1:
        return params.decentralized_withdrawal &&
               params.decentralized_withdrawal->Valid() &&
               !params.managed_withdrawal;
    }
    return false;
}

} // namespace Consensus

#endif // B3COIN_CONSENSUS_BRIDGE_PARAMS_H
