// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_WITHDRAWAL_V2_H
#define B3COIN_MODERN_WITHDRAWAL_V2_H

#include <consensus/amount.h>
#include <consensus/bridge_params.h>
#include <crypto/common.h>
#include <modern/policy.h>
#include <modern/withdrawal_tree.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace modern {

/**
 * Leaf V2 (doc/design/b3-bridge-v2-contracts-spec.md section 6.4, normative;
 * node changes section 7.3). keccak256 of a 180-byte fixed-width big-endian
 * preimage:
 *
 *   [0,32)    LEAF_TAG_V2 = keccak256("B3/BRIDGE/WITHDRAWAL/V2")
 *   [32,64)   bUSD V2 AssetId (raw bytes; binds the chain domain)
 *   [64,72)   withdrawal id
 *   [72,80)   destination chain id (Vault V2's block.chainid)
 *   [80,100)  vault
 *   [100,120) token
 *   [120,140) recipient
 *   [140,172) amount as uint256 (high 24 bytes zero)
 *   [172,180) B3 height
 *
 * The tag and length keep a V2 leaf apart from V1's 128-byte leaves and from
 * 64-byte tree nodes. Leaf V2 shares V1's global id space (< 2^32) and its
 * depth-32 ordered tree, zero hashes and append algorithm; the V2 tree is a
 * separate instance that starts empty at V (its empty root is non-zero).
 */
inline constexpr std::string_view WITHDRAWAL_LEAF_V2_TAG_STRING{"B3/BRIDGE/WITHDRAWAL/V2"};
inline constexpr size_t WITHDRAWAL_LEAF_V2_PREIMAGE_SIZE{32 + 32 + 8 + 8 + 20 + 20 + 20 + 32 + 8};
static_assert(WITHDRAWAL_LEAF_V2_PREIMAGE_SIZE == 180);

inline const uint256& WithdrawalLeafTagV2()
{
    static const uint256 tag{WithdrawalKeccak(std::span<const unsigned char>{
        reinterpret_cast<const unsigned char*>(WITHDRAWAL_LEAF_V2_TAG_STRING.data()),
        WITHDRAWAL_LEAF_V2_TAG_STRING.size()})};
    return tag;
}

struct BridgeWithdrawalV2 {
    uint64_t withdrawal_id{0};
    uint64_t destination_chain_id{0};
    AssetId asset_id{};
    Consensus::BridgeEthAddress vault{};
    Consensus::BridgeEthAddress token{};
    Consensus::BridgeEthAddress recipient{};
    CAmount amount{0};
    uint64_t b3_height{0};

    friend bool operator==(const BridgeWithdrawalV2&,
                           const BridgeWithdrawalV2&) = default;
};

/** The 180-byte preimage; nullopt for a value Vault V2 could never release. */
inline std::optional<std::array<unsigned char, WITHDRAWAL_LEAF_V2_PREIMAGE_SIZE>>
EncodeBridgeWithdrawalV2(const BridgeWithdrawalV2& withdrawal)
{
    if (withdrawal.withdrawal_id >= MAX_WITHDRAWAL_LEAVES ||
        withdrawal.destination_chain_id == 0 || withdrawal.asset_id.IsNull() ||
        Consensus::BridgeAddressIsNull(withdrawal.vault) ||
        Consensus::BridgeAddressIsNull(withdrawal.token) ||
        Consensus::BridgeAddressIsNull(withdrawal.recipient) ||
        withdrawal.amount <= 0 || withdrawal.amount > MAX_MONEY) {
        return std::nullopt;
    }

    std::array<unsigned char, WITHDRAWAL_LEAF_V2_PREIMAGE_SIZE> out{};
    const uint256& tag{WithdrawalLeafTagV2()};
    std::copy(tag.begin(), tag.end(), out.begin());
    std::copy(withdrawal.asset_id.begin(), withdrawal.asset_id.end(),
              out.begin() + 32);
    WriteBE64(out.data() + 64, withdrawal.withdrawal_id);
    WriteBE64(out.data() + 72, withdrawal.destination_chain_id);
    std::copy(withdrawal.vault.begin(), withdrawal.vault.end(),
              out.begin() + 80);
    std::copy(withdrawal.token.begin(), withdrawal.token.end(),
              out.begin() + 100);
    std::copy(withdrawal.recipient.begin(), withdrawal.recipient.end(),
              out.begin() + 120);
    // Solidity's uint256 amount: bytes [140,164) remain zero.
    WriteBE64(out.data() + 164, static_cast<uint64_t>(withdrawal.amount));
    WriteBE64(out.data() + 172, withdrawal.b3_height);
    return out;
}

inline std::optional<uint256> BridgeWithdrawalLeafV2(
    const BridgeWithdrawalV2& withdrawal)
{
    const auto encoded{EncodeBridgeWithdrawalV2(withdrawal)};
    if (!encoded) return std::nullopt;
    return WithdrawalKeccak(*encoded);
}

/**
 * The B-23 migration (MIGRATE) and retired-vault sweep leaf (contracts spec
 * section 6.4; node changes section 7.6): an ordinary leaf V2 that the
 * retired vault v releases, naming v (its chain, vault and token) and paying
 * the successor vault v' `amount` of the shared bUSD V2 asset. No new tag,
 * function or length: v' counts the arrival through absorb(), and B3
 * credits it only from the absorbedTotal observed at v'.
 */
inline BridgeWithdrawalV2 MakeVaultSweepWithdrawalV2(
    const Consensus::BridgeV2VaultEntry& retired, const AssetId& asset,
    const Consensus::BridgeEthAddress& successor_vault, uint64_t id,
    CAmount amount, uint64_t b3_height)
{
    return BridgeWithdrawalV2{
        .withdrawal_id = id,
        .destination_chain_id = retired.origin_chain_id,
        .asset_id = asset,
        .vault = retired.vault_address,
        .token = retired.token_address,
        .recipient = successor_vault,
        .amount = amount,
        .b3_height = b3_height,
    };
}

/** Append one leaf V2 at index == its withdrawal id; returns the leaf. */
inline std::optional<uint256> AppendBridgeWithdrawalV2(
    WithdrawalTreeState& state, const BridgeWithdrawalV2& withdrawal)
{
    if (withdrawal.withdrawal_id != state.count) return std::nullopt;
    const auto leaf{BridgeWithdrawalLeafV2(withdrawal)};
    if (!leaf || !AppendWithdrawalLeaf(state, *leaf)) return std::nullopt;
    return leaf;
}

} // namespace modern

#endif // B3COIN_MODERN_WITHDRAWAL_V2_H
