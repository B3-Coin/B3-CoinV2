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
inline constexpr uint64_t BUSD_ETHEREUM_ORIGIN_DEPLOYMENT_BLOCK{25'898'729};
inline constexpr BridgeEthAddress BUSD_ETHEREUM_VAULT{
    0x07, 0x78, 0x39, 0xb1, 0x2c, 0xeb, 0xfb, 0xf1, 0x63, 0xac,
    0xae, 0xac, 0x3a, 0x59, 0xa0, 0x15, 0xd1, 0x00, 0xc6, 0x4b,
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
// Historical managed-v1 authority retained only as an audit constant. It is
// not an authority of the production keyless vault pinned below.
inline constexpr BridgeEthAddress BUSD_ETHEREUM_MANAGED_AUTHORITY{
    0x76, 0xc7, 0xa2, 0x45, 0xd0, 0xd2, 0xe4, 0xcf, 0x92, 0x40,
    0x3a, 0xf0, 0x14, 0x48, 0x25, 0xdf, 0x1c, 0xc6, 0x14, 0xf1,
};
inline constexpr std::array<unsigned char, 32>
    BUSD_ETHEREUM_VAULT_RUNTIME_CODE_HASH_BYTES{
        0xdb, 0x26, 0x77, 0x12, 0x88, 0x75, 0x68, 0xbf,
        0xfd, 0x39, 0x4e, 0x46, 0x53, 0x8b, 0xdd, 0xba,
        0x01, 0xda, 0x11, 0xce, 0xfc, 0x38, 0xe3, 0x2b,
        0x24, 0x28, 0xc0, 0x09, 0x11, 0x23, 0x7f, 0x8d,
    };
inline constexpr uint256 BUSD_ETHEREUM_VAULT_RUNTIME_CODE_HASH{
    std::span<const unsigned char>{
        BUSD_ETHEREUM_VAULT_RUNTIME_CODE_HASH_BYTES}};
inline constexpr std::array<unsigned char, 32>
    BUSD_ETHEREUM_USDT_RUNTIME_CODE_HASH_BYTES{
        0xb4, 0x4f, 0xb4, 0xe9, 0x49, 0xd0, 0xf7, 0x8f,
        0x87, 0xf7, 0x9e, 0xe4, 0x64, 0x28, 0xf2, 0x3a,
        0x2a, 0x57, 0x13, 0xce, 0x6f, 0xc6, 0xe0, 0xbe,
        0xb3, 0xdd, 0xa7, 0x8c, 0x2a, 0xc1, 0xea, 0x55,
    };
inline constexpr uint256 BUSD_ETHEREUM_USDT_RUNTIME_CODE_HASH{
    std::span<const unsigned char>{
        BUSD_ETHEREUM_USDT_RUNTIME_CODE_HASH_BYTES}};
inline constexpr BridgeEthAddress BUSD_ETHEREUM_VERIFIER{
    0xe7, 0x2b, 0x3f, 0xe7, 0x3f, 0x0d, 0x42, 0xa6, 0xe9, 0x64,
    0xd3, 0x3e, 0x7b, 0xb1, 0xcc, 0x2e, 0xa7, 0xa3, 0xf6, 0x90,
};
inline constexpr std::array<unsigned char, 32>
    BUSD_ETHEREUM_VERIFIER_RUNTIME_CODE_HASH_BYTES{
        0xaf, 0xdb, 0xa8, 0xbe, 0xfb, 0x1a, 0xac, 0xc8,
        0x32, 0xbf, 0xf4, 0xe0, 0x8d, 0xcd, 0x92, 0xe6,
        0x64, 0x5a, 0x01, 0x2e, 0xa8, 0xa8, 0x08, 0x8b,
        0x0f, 0x28, 0x11, 0xd9, 0x16, 0x02, 0x29, 0x02,
    };
inline constexpr uint256 BUSD_ETHEREUM_VERIFIER_RUNTIME_CODE_HASH{
    std::span<const unsigned char>{
        BUSD_ETHEREUM_VERIFIER_RUNTIME_CODE_HASH_BYTES}};
inline constexpr std::array<unsigned char, 32>
    BUSD_ETHEREUM_BOOTSTRAP_VALIDATOR_SET_HASH_BYTES{
        0x7a, 0x0b, 0x8a, 0xac, 0xa4, 0xe7, 0x78, 0xdf,
        0x11, 0x4a, 0xd1, 0x3d, 0xcb, 0xb8, 0xcf, 0xdb,
        0xb0, 0xc8, 0xcd, 0xf4, 0x57, 0x60, 0xa5, 0x64,
        0xa2, 0xac, 0x6c, 0x39, 0xdd, 0x6b, 0x23, 0x27,
    };
inline constexpr uint256 BUSD_ETHEREUM_BOOTSTRAP_VALIDATOR_SET_HASH{
    std::span<const unsigned char>{
        BUSD_ETHEREUM_BOOTSTRAP_VALIDATOR_SET_HASH_BYTES}};
inline constexpr std::array<unsigned char, 32>
    ETHEREUM_MAINNET_BRIDGE_CHECKPOINT_ROOT_BYTES{
        0xf6, 0x74, 0x47, 0x74, 0xa1, 0xbc, 0xfe, 0x91,
        0x0c, 0x64, 0x3e, 0x44, 0x7c, 0xd0, 0x9f, 0xe8,
        0x44, 0x3c, 0xc2, 0xed, 0xc2, 0x5d, 0x9a, 0xe6,
        0x51, 0x55, 0xb3, 0xcb, 0xbe, 0xf3, 0xb6, 0x46,
    };
inline constexpr uint256 ETHEREUM_MAINNET_BRIDGE_CHECKPOINT_ROOT{
    std::span<const unsigned char>{
        ETHEREUM_MAINNET_BRIDGE_CHECKPOINT_ROOT_BYTES}};
inline constexpr uint64_t ETHEREUM_MAINNET_BRIDGE_CHECKPOINT_SLOT{15'136'512};

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
/**
 * Cross-language decentralized-withdrawal V1 rules commitment. This is the
 * existing C++/Solidity leaf vector for the exact 128-byte big-endian layout:
 * id=0, chain=1, asset bytes 00..1f, token bytes 20..33, recipient bytes
 * 40..53, amount=1,000,000 and B3 height=815,000.
 */
inline constexpr std::array<unsigned char, 32>
    DECENTRALIZED_WITHDRAWAL_RULES_COMMITMENT_V1_BYTES{
        0xf9, 0x6e, 0xe3, 0x73, 0x21, 0xb1, 0x91, 0xd9,
        0xba, 0x3e, 0x57, 0x3f, 0xd7, 0x73, 0x9a, 0xb8,
        0xa1, 0x63, 0x03, 0x38, 0x24, 0xa1, 0xc5, 0x34,
        0x04, 0x5b, 0xd1, 0x68, 0xc3, 0xc8, 0x8b, 0x44,
    };
inline constexpr uint256 DECENTRALIZED_WITHDRAWAL_RULES_COMMITMENT_V1{
    std::span<const unsigned char>{
        DECENTRALIZED_WITHDRAWAL_RULES_COMMITMENT_V1_BYTES}};
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
               withdrawal_rules_commitment ==
                   DECENTRALIZED_WITHDRAWAL_RULES_COMMITMENT_V1 &&
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

// ---------------------------------------------------------------- bridge V2
//
// Height-activated bridge V2 regime (doc/design/b3-bridge-v2-node-changes.md
// sections 3.1-3.3): one fresh Ethereum vault entry minting bUSD V2, a fresh
// light-client store and the Verifier V2 pins. BridgeAssetParams above stays
// byte-for-byte unchanged, so every V1 identity keeps validating history.
// Unset on every shipped network; only the regtest pin file's v2 section
// populates it until a reviewed release pins every value (stage S7).

//! Serialization version byte of the bUSD V2 AssetId preimage (node changes
//! section 3.2): BridgeAssetIdV1's field order with version 2.
inline constexpr uint8_t BRIDGE_ASSET_IDENTITY_VERSION_V2{2};
//! Vault entry proof kind 1: Ethereum receipt proofs through the finalized
//! light client, minted by record kind 3 (node changes section 3.3).
inline constexpr uint8_t BRIDGE_V2_PROOF_KIND_ETHEREUM_RECEIPT{1};
//! B-14: 10,000 bUSD (raw, six decimals) per block, per mint epoch and per
//! deposit (Vault V2 MAX_DEPOSIT_RAW).
inline constexpr CAmount BRIDGE_V2_DEFAULT_CAP_RAW{10'000'000'000};

/** One Vault V2 entry (node changes section 3.3). */
struct BridgeV2VaultEntry {
    uint64_t origin_chain_id{0};
    BridgeEthAddress vault_address{};
    BridgeEthAddress token_address{};
    uint8_t origin_decimals{BUSD_ORIGIN_DECIMALS};
    uint8_t asset_decimals{BUSD_ASSET_DECIMALS};
    // OI-3: the vault's extcodehash commits to blockHash(R_f - 1) through
    // its verifier and lineage immutables, so a regtest fixture cannot know
    // it before the first block >= A. Required before any non-regtest
    // activation: BridgeV2Params::Valid() refuses origin chain 1 without it
    // (stage S7 pins it).
    std::optional<uint256> vault_runtime_code_hash{};
    uint256 token_runtime_code_hash{};
    uint8_t proof_kind{BRIDGE_V2_PROOF_KIND_ETHEREUM_RECEIPT};
    // First Ethereum block that may hold vault events; the relayer's first
    // log scan. A lower bound is safe; zero is an incomplete pin.
    uint64_t origin_deployment_block{0};
    CAmount max_deposit_raw{0};
    BridgeMintCaps mint_caps{};

    bool Valid() const
    {
        return origin_chain_id != 0 && !BridgeAddressIsNull(vault_address) &&
               !BridgeAddressIsNull(token_address) &&
               vault_address != token_address &&
               origin_decimals == BUSD_ORIGIN_DECIMALS &&
               asset_decimals == BUSD_ASSET_DECIMALS &&
               (!vault_runtime_code_hash ||
                !vault_runtime_code_hash->IsNull()) &&
               !token_runtime_code_hash.IsNull() &&
               proof_kind == BRIDGE_V2_PROOF_KIND_ETHEREUM_RECEIPT &&
               origin_deployment_block > 0 && mint_caps.Valid() &&
               max_deposit_raw > 0 &&
               max_deposit_raw <= mint_caps.max_per_block;
    }
};

//! Which vault entry of the envelope a rule reads: the launch entry v, or
//! the B-23 successor v' a migration hard fork appends (node changes
//! sections 3.1 and 7.6). The value is the entry's index in the registry.
enum class BridgeV2EntryRole : uint8_t {
    LAUNCH = 0,
    SUCCESSOR = 1,
};

/**
 * B-23 successor vault entry v' (node changes sections 3.2 and 7.6;
 * contracts spec sections 6.1 and 6.4). `entry` is v' itself: v's origin
 * chain, token, decimals, token code hash, proof kind and mint caps, with
 * its own vault address, optional vault code hash (OI-3), deployment block
 * and MAX_DEPOSIT_RAW. `predecessor` is v's vault address and `asset_id` v's
 * bUSD V2 AssetId as raw EVM bytes (Vault V2 `B3_ASSET_ID()`), the two
 * constructor arguments v' checks against v; B3 checks `asset_id` against
 * its own recomputation (modern::bridge_v2_detail::ConfiguredDomain), so v'
 * mints and burns the same asset. From `migration_height` (h_m > V, >= W2)
 * v retires and v' takes deposits and burns; h_m, like V and W2, is not part
 * of the registry id.
 */
struct BridgeV2SuccessorEntry {
    BridgeV2VaultEntry entry{};
    BridgeEthAddress predecessor{};
    uint256 asset_id{};
    int32_t migration_height{-1};
};

/**
 * Verifier V2 pins mirrored by B3 (node changes section 3.1). The member and
 * weight bounds are the verifier's immutable bridge-authorizing interval, in
 * the units of BridgeDecentralizedWithdrawalPins. The code hash carries the
 * vault's OI-3 caveat.
 *
 * `max_epoch_lag_seconds` mirrors the verifier's immutable MAX_EPOCH_LAG,
 * the window after a rotation in which it still accepts a root (its set
 * expiry). The burn gate's C1 impairment and E2 rotation age read the B3
 * side's set_expiry_seconds, so BridgeV2Params::Valid() requires the two to
 * be equal whenever the mirror is pinned: a verifier deployed with a
 * shorter window would otherwise see B3 keep burns open after it can no
 * longer accept a covering root. The value is a deployment parameter known
 * before the fresh lineage starts (unlike the code hash, OI-3), so the
 * regtest pin file may carry it (`verifier_max_epoch_lag`); Ethereum
 * mainnet requires it (a stage S7 pin gate). It equals a registry-id field
 * (set_expiry) and so adds nothing to BridgeRegistryIdV2.
 */
struct BridgeV2VerifierPins {
    BridgeEthAddress address{};
    std::optional<uint256> runtime_code_hash{};
    uint32_t min_bridge_validators{0};
    uint32_t max_bridge_validators{0};
    uint64_t min_bridge_total_weight{0};
    std::optional<int64_t> max_epoch_lag_seconds{};

    bool Valid() const
    {
        return !BridgeAddressIsNull(address) &&
               (!runtime_code_hash || !runtime_code_hash->IsNull()) &&
               min_bridge_validators >= MIN_BRIDGE_VALIDATORS_V1 &&
               max_bridge_validators >= min_bridge_validators &&
               max_bridge_validators <= MAX_PROVEN_BRIDGE_VALIDATORS_V1 &&
               min_bridge_total_weight > 0 &&
               (!max_epoch_lag_seconds || *max_epoch_lag_seconds > 0);
    }
};

/**
 * Burn liveness gate thresholds (node changes sections 8.4 and 8.6; B-13).
 * The defaults are profile M, the mainnet values: `set_expiry_seconds`,
 * `release_margin_seconds` and `burn_margin_seconds` mirror the verifier's
 * windows, so E2 closes burns once a rotation is older than
 * RotationAgeLimit() (30 d - 7 d - 1 d = 1,900,800 s). Observations older
 * than `observation_max_age_seconds` fail E1, E7 and E8; `max_epoch_gap`
 * bounds E3 and `max_finality_lag_blocks` C3. `wind_down_margin_ppm` is
 * margin_v of node changes section 7.2 check 3 while the observed USDT
 * balance is below `locked` (zero margin otherwise).
 *
 * `mempool_close_margin_seconds` is policy only (the mempool refuses a burn
 * whose gate closes within it); every other field is consensus, and only
 * the consensus fields enter the registry id V2.
 */
struct BridgeV2GateParams {
    int64_t set_expiry_seconds{2'592'000};
    int64_t release_margin_seconds{604'800};
    int64_t burn_margin_seconds{86'400};
    int64_t observation_max_age_seconds{172'800};
    uint32_t max_epoch_gap{2};
    int32_t max_finality_lag_blocks{90};
    uint32_t wind_down_margin_ppm{100'000};
    int64_t mempool_close_margin_seconds{3'600}; // policy only

    //! E2's bound: set_expiry - release_margin - burn_margin. Positive
    //! whenever Valid().
    int64_t RotationAgeLimit() const
    {
        return set_expiry_seconds - release_margin_seconds - burn_margin_seconds;
    }

    //! Every time value positive, ppm at most 1e6, a positive lag, and
    //! set_expiry > release_margin + burn_margin (node changes section 3.4
    //! assertion 5), evaluated without overflow.
    bool Valid() const
    {
        return set_expiry_seconds > 0 && release_margin_seconds > 0 &&
               burn_margin_seconds > 0 && observation_max_age_seconds > 0 &&
               mempool_close_margin_seconds > 0 &&
               wind_down_margin_ppm <= 1'000'000 &&
               max_finality_lag_blocks > 0 &&
               release_margin_seconds < set_expiry_seconds &&
               burn_margin_seconds < set_expiry_seconds - release_margin_seconds;
    }

    /**
     * Regtest profile R (regtest plan section 3.3 item 2): every time value
     * divided by 36, a mainnet day to one 2,400 s regtest epoch, and
     * max_finality_lag_blocks 30 (about three regtest t_fin). The epoch gap
     * and the wind-down margin are not times and keep profile M's values.
     */
    static BridgeV2GateParams RegtestProfileR()
    {
        BridgeV2GateParams profile;
        profile.set_expiry_seconds /= 36;           // 72,000
        profile.release_margin_seconds /= 36;       // 16,800
        profile.burn_margin_seconds /= 36;          // 2,400
        profile.observation_max_age_seconds /= 36;  // 4,800
        profile.mempool_close_margin_seconds /= 36; // 100
        profile.max_finality_lag_blocks = 30;
        return profile;
    }

    friend bool operator==(const BridgeV2GateParams&,
                           const BridgeV2GateParams&) = default;
};

/**
 * Deposit forwarder F_d pins (node changes section 3.1; contracts spec
 * section 7.2): the B3DepositForwarderFactory address and its
 * FORWARDER_INIT_CODE_HASH (raw EVM bytes, from the DeployV2 manifest).
 * Derivation only (modern/bridge_forwarder.h), never a mint condition.
 */
struct BridgeV2DepositForwarderPins {
    BridgeEthAddress factory{};
    uint256 init_code_hash{};

    bool Valid() const
    {
        return !BridgeAddressIsNull(factory) && !init_code_hash.IsNull();
    }

    friend bool operator==(const BridgeV2DepositForwarderPins&,
                           const BridgeV2DepositForwarderPins&) = default;
};

/**
 * Exit forwarder F_x pins (node changes sections 3.1 and 7.2; contracts spec
 * section 8): the B3OftExitFactory address, its FORWARDER_INIT_CODE_HASH and
 * its immutable DST_EID (LayerZero endpoint id, Arbitrum 30110). A burn's
 * exit block is checked against them.
 */
struct BridgeV2ExitForwarderPins {
    BridgeEthAddress factory{};
    uint256 init_code_hash{};
    uint32_t dst_eid{0};

    bool Valid() const
    {
        return !BridgeAddressIsNull(factory) && !init_code_hash.IsNull() &&
               dst_eid != 0;
    }

    friend bool operator==(const BridgeV2ExitForwarderPins&,
                           const BridgeV2ExitForwarderPins&) = default;
};

/**
 * Complete bridge V2 envelope. V (`activation_height`) is the first height
 * of V2 records and of the V2 withdrawal tree and equals the verifier's
 * BRIDGE_ACTIVATION_HEIGHT; W2 (`burn_height`) >= V is the first height of
 * V2 burns (B-12 default W2 = V). Neither enters the registry id.
 *
 * `vault` is the launch entry; `successor` is the optional B-23 successor
 * v' (BridgeV2SuccessorEntry), unset until a migration is pinned.
 * `deposit_forwarder` and `exit_forwarder` are the optional route (b) and
 * (d3) factory pins (ForwardersValid()); both enter the registry id.
 */
struct BridgeV2Params {
    int32_t activation_height{-1};
    int32_t burn_height{-1};
    EthereumLightClientPins light_client{};
    BridgeV2VerifierPins verifier{};
    BridgeV2VaultEntry vault{};
    BridgeV2GateParams gate{};
    std::optional<BridgeV2SuccessorEntry> successor{};
    // Route (b) and (d3) forwarder factories; unset on every shipped network.
    std::optional<BridgeV2DepositForwarderPins> deposit_forwarder{};
    std::optional<BridgeV2ExitForwarderPins> exit_forwarder{};
    // Owner ruling R10 (OI-8): the first height at which a kind 6 burn is
    // script-checked under SCRIPT_VERIFY_BRIDGE_SIGHASH_ALL_OUTPUTS, so a
    // relayer may add the native fee input. A pin of its own, at or above
    // V: pinning the rest of the envelope never turns the sponsored sighash
    // rule on. Unset by default and on every shipped network; only the
    // regtest pin file's v2 section sets it.
    std::optional<int32_t> sponsored_sighash_height{};

    //! The entry of `role`; nullptr for SUCCESSOR when none is pinned.
    const BridgeV2VaultEntry* Entry(BridgeV2EntryRole role) const
    {
        switch (role) {
        case BridgeV2EntryRole::LAUNCH:
            return &vault;
        case BridgeV2EntryRole::SUCCESSOR:
            return successor ? &successor->entry : nullptr;
        }
        return nullptr;
    }

    /**
     * The successor's own rules, given a launch envelope that is otherwise
     * Valid(): a complete entry on v's chain and token with v's decimals,
     * token code hash, proof kind and caps (the mint epoch counters are
     * keyed by the shared AssetId); a vault address of its own; v as the
     * predecessor; a non-null AssetId; h_m > V and h_m >= W2. On Ethereum
     * mainnet v' needs its vault code hash (OI-3 leaves it unpinned only
     * off mainnet). True without a successor.
     */
    bool SuccessorValid() const
    {
        if (!successor) return true;
        const BridgeV2SuccessorEntry& s{*successor};
        const BridgeV2VaultEntry& next{s.entry};
        if (!next.Valid() || next.origin_chain_id != vault.origin_chain_id ||
            next.token_address != vault.token_address ||
            next.origin_decimals != vault.origin_decimals ||
            next.asset_decimals != vault.asset_decimals ||
            next.token_runtime_code_hash != vault.token_runtime_code_hash ||
            next.proof_kind != vault.proof_kind ||
            next.mint_caps.max_per_block != vault.mint_caps.max_per_block ||
            next.mint_caps.max_per_epoch != vault.mint_caps.max_per_epoch ||
            next.mint_caps.epoch_length_blocks !=
                vault.mint_caps.epoch_length_blocks) {
            return false;
        }
        if (next.vault_address == vault.vault_address ||
            next.vault_address == vault.token_address ||
            next.vault_address == verifier.address ||
            s.predecessor != vault.vault_address || s.asset_id.IsNull()) {
            return false;
        }
        if (s.migration_height <= activation_height ||
            s.migration_height < burn_height) {
            return false;
        }
        return next.origin_chain_id != BUSD_ETHEREUM_CHAIN_ID ||
               next.vault_runtime_code_hash.has_value();
    }

    /**
     * The forwarder pins' own rules (node changes section 3.1): each pinned
     * factory is complete (a non-null address and init-code hash; the exit
     * pin also a non-zero dst_eid) and is a contract of its own, distinct
     * from the launch vault, the token, the verifier, a pinned successor
     * vault and the other factory. True when neither is pinned, so every
     * forwarder-free envelope keeps its verdict.
     */
    bool ForwardersValid() const
    {
        const auto is_bridge_contract{[&](const BridgeEthAddress& address) {
            return address == vault.vault_address ||
                   address == vault.token_address ||
                   address == verifier.address ||
                   (successor && address == successor->entry.vault_address);
        }};
        if (deposit_forwarder && (!deposit_forwarder->Valid() ||
                                  is_bridge_contract(deposit_forwarder->factory))) {
            return false;
        }
        if (exit_forwarder && (!exit_forwarder->Valid() ||
                               is_bridge_contract(exit_forwarder->factory))) {
            return false;
        }
        return !(deposit_forwarder && exit_forwarder &&
                 deposit_forwarder->factory == exit_forwarder->factory);
    }

    bool Valid() const
    {
        if (activation_height < 1 || burn_height < activation_height ||
            !light_client.Valid() || !vault.Valid() || !verifier.Valid() ||
            !gate.Valid() ||
            verifier.address == vault.vault_address ||
            verifier.address == vault.token_address) {
            return false;
        }
        // The gate's set expiry must be the verifier's own window
        // (BridgeV2VerifierPins::max_epoch_lag_seconds).
        if (verifier.max_epoch_lag_seconds &&
            *verifier.max_epoch_lag_seconds != gate.set_expiry_seconds) {
            return false;
        }
        // R10's sponsored sighash rule starts at or after V.
        if (sponsored_sighash_height &&
            *sponsored_sighash_height < activation_height) {
            return false;
        }
        // As BridgeMintParamsReady: an Ethereum-mainnet entry is exactly
        // USDT under the mainnet beacon root, so a testnet light client
        // copied with chain id 1 fails closed. Mainnet also needs both
        // code-hash pins (node changes section 3.4 assertion 1) and the
        // MAX_EPOCH_LAG mirror; only a non-mainnet fixture may leave the
        // OI-3 hashes and the mirror unset.
        if (vault.origin_chain_id == BUSD_ETHEREUM_CHAIN_ID &&
            (vault.token_address != BUSD_ETHEREUM_USDT ||
             light_client.genesis_validators_root !=
                 ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT ||
             !vault.vault_runtime_code_hash || !verifier.runtime_code_hash ||
             !verifier.max_epoch_lag_seconds)) {
            return false;
        }
        return SuccessorValid() && ForwardersValid();
    }
};

} // namespace Consensus

#endif // B3COIN_CONSENSUS_BRIDGE_PARAMS_H
