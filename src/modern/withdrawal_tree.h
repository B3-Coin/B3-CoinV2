// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_WITHDRAWAL_TREE_H
#define B3COIN_MODERN_WITHDRAWAL_TREE_H

#include <consensus/amount.h>
#include <consensus/bridge_params.h>
#include <crypto/common.h>
#include <crypto/keccak256.h>
#include <modern/policy.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace modern {

/**
 * Ethereum-facing cumulative withdrawal tree (cross-chain finality v1, §6).
 *
 * All integers are fixed-width big-endian. The amount is encoded as a
 * 256-bit integer even though B3 consensus currently bounds it to CAmount.
 * Hashes and addresses retain their canonical raw byte order. Leaves occupy
 * indices equal to their zero-based, strictly sequential withdrawal ids.
 */
inline constexpr unsigned WITHDRAWAL_TREE_DEPTH{32};
inline constexpr uint64_t MAX_WITHDRAWAL_LEAVES{uint64_t{1} << WITHDRAWAL_TREE_DEPTH};
inline constexpr size_t WITHDRAWAL_LEAF_PREIMAGE_SIZE{8 + 8 + 32 + 20 + 20 + 32 + 8};
static_assert(WITHDRAWAL_LEAF_PREIMAGE_SIZE == 128);

struct BridgeWithdrawalV1 {
    uint64_t withdrawal_id{0};
    uint64_t origin_chain_id{0};
    AssetId asset_id{};
    Consensus::BridgeEthAddress origin_token{};
    Consensus::BridgeEthAddress recipient{};
    CAmount amount{0};
    uint64_t b3_height{0};

    friend bool operator==(const BridgeWithdrawalV1&,
                           const BridgeWithdrawalV1&) = default;
};

inline uint256 WithdrawalKeccak(const std::span<const unsigned char> bytes)
{
    uint256 out;
    Keccak256().Write(bytes).Finalize(out);
    return out;
}

inline uint256 WithdrawalNodeHash(const uint256& left, const uint256& right)
{
    std::array<unsigned char, 64> preimage{};
    std::copy(left.begin(), left.end(), preimage.begin());
    std::copy(right.begin(), right.end(), preimage.begin() + 32);
    return WithdrawalKeccak(preimage);
}

inline std::optional<std::array<unsigned char, WITHDRAWAL_LEAF_PREIMAGE_SIZE>>
EncodeBridgeWithdrawalV1(const BridgeWithdrawalV1& withdrawal)
{
    if (withdrawal.withdrawal_id >= MAX_WITHDRAWAL_LEAVES ||
        withdrawal.origin_chain_id == 0 || withdrawal.asset_id.IsNull() ||
        Consensus::BridgeAddressIsNull(withdrawal.origin_token) ||
        Consensus::BridgeAddressIsNull(withdrawal.recipient) ||
        withdrawal.amount <= 0 || withdrawal.amount > MAX_MONEY) {
        return std::nullopt;
    }

    std::array<unsigned char, WITHDRAWAL_LEAF_PREIMAGE_SIZE> out{};
    WriteBE64(out.data(), withdrawal.withdrawal_id);
    WriteBE64(out.data() + 8, withdrawal.origin_chain_id);
    std::copy(withdrawal.asset_id.begin(), withdrawal.asset_id.end(),
              out.begin() + 16);
    std::copy(withdrawal.origin_token.begin(), withdrawal.origin_token.end(),
              out.begin() + 48);
    std::copy(withdrawal.recipient.begin(), withdrawal.recipient.end(),
              out.begin() + 68);
    // Solidity's uint256 amount: the high 24 bytes remain zero.
    WriteBE64(out.data() + 112, static_cast<uint64_t>(withdrawal.amount));
    WriteBE64(out.data() + 120, withdrawal.b3_height);
    return out;
}

inline std::optional<uint256> BridgeWithdrawalLeafV1(
    const BridgeWithdrawalV1& withdrawal)
{
    const auto encoded{EncodeBridgeWithdrawalV1(withdrawal)};
    if (!encoded) return std::nullopt;
    return WithdrawalKeccak(*encoded);
}

/**
 * Incremental fixed-depth ordered Merkle tree. `frontier[level]` is the
 * completed left subtree waiting for a right sibling when the corresponding
 * bit of `count` is set. This is the same append algorithm used by Ethereum
 * deposit-style accumulators; it never sorts pairs.
 */
struct WithdrawalTreeState {
    uint64_t count{0};
    std::array<uint256, WITHDRAWAL_TREE_DEPTH> frontier{};
    uint256 root{};

    WithdrawalTreeState();

    friend bool operator==(const WithdrawalTreeState&,
                           const WithdrawalTreeState&) = default;
};

inline const std::array<uint256, WITHDRAWAL_TREE_DEPTH + 1>&
WithdrawalZeroHashes()
{
    static const std::array<uint256, WITHDRAWAL_TREE_DEPTH + 1> zeros{[] {
        std::array<uint256, WITHDRAWAL_TREE_DEPTH + 1> value{};
        for (unsigned level{0}; level < WITHDRAWAL_TREE_DEPTH; ++level) {
            value[level + 1] = WithdrawalNodeHash(value[level], value[level]);
        }
        return value;
    }()};
    return zeros;
}

inline WithdrawalTreeState::WithdrawalTreeState()
    : root{WithdrawalZeroHashes()[WITHDRAWAL_TREE_DEPTH]}
{
}

inline bool AppendWithdrawalLeaf(WithdrawalTreeState& state,
                                 const uint256& leaf)
{
    if (state.count >= MAX_WITHDRAWAL_LEAVES || leaf.IsNull()) return false;
    uint256 node{leaf};
    const uint64_t index{state.count};
    const auto& zeros{WithdrawalZeroHashes()};
    for (unsigned level{0}; level < WITHDRAWAL_TREE_DEPTH; ++level) {
        if (((index >> level) & 1U) == 0) {
            state.frontier[level] = node;
            node = WithdrawalNodeHash(node, zeros[level]);
        } else {
            node = WithdrawalNodeHash(state.frontier[level], node);
        }
    }
    ++state.count;
    state.root = node;
    return true;
}

inline std::optional<uint256> AppendBridgeWithdrawal(
    WithdrawalTreeState& state, const BridgeWithdrawalV1& withdrawal)
{
    if (withdrawal.withdrawal_id != state.count) return std::nullopt;
    const auto leaf{BridgeWithdrawalLeafV1(withdrawal)};
    if (!leaf || !AppendWithdrawalLeaf(state, *leaf)) return std::nullopt;
    return leaf;
}

} // namespace modern

#endif // B3COIN_MODERN_WITHDRAWAL_TREE_H
