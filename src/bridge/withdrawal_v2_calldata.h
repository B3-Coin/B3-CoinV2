// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_BRIDGE_WITHDRAWAL_V2_CALLDATA_H
#define B3COIN_BRIDGE_WITHDRAWAL_V2_CALLDATA_H

#include <bridge/bootstrap_proof.h>
#include <bridge/ethereum_calldata.h>
#include <modern/finality_v2.h>
#include <modern/withdrawal_tree.h>
#include <modern/withdrawal_v2.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bridge {

/**
 * B3VaultV2.release(WithdrawalV2 w, bytes32[32] path) with
 * WithdrawalV2 = (uint64 withdrawalId, address recipient, uint256 amount,
 * uint64 b3Height) (contracts spec section 6.4). The tuple and therefore the
 * selector equal V1's B3StakerBridge.release; the vault, token, destination
 * chain and asset id are the vault's own immutables, bound by the leaf.
 */
inline constexpr std::string_view VAULT_V2_RELEASE_SIGNATURE{
    "release((uint64,address,uint256,uint64),bytes32[32])"};
//! selector || 4 static tuple words || 32 path words.
inline constexpr size_t VAULT_V2_RELEASE_CALLDATA_SIZE{
    4 + (4 + modern::WITHDRAWAL_TREE_DEPTH) * 32};
static_assert(VAULT_V2_RELEASE_CALLDATA_SIZE == 1156);

/** Fully public artifacts needed to call B3VaultV2.release. */
struct WithdrawalV2RelayArtifacts {
    modern::BridgeWithdrawalV2 withdrawal{};
    std::array<unsigned char, modern::WITHDRAWAL_LEAF_V2_PREIMAGE_SIZE> leaf_preimage{};
    uint256 leaf{};
    uint256 root{};
    std::array<uint256, modern::WITHDRAWAL_TREE_DEPTH> path{};
    std::vector<unsigned char> release_calldata{};
};

/**
 * Construct the ordered depth-32 path of one leaf V2 in a cumulative prefix
 * and encode B3VaultV2.release. `withdrawals` must be the exact consecutive
 * prefix (ids 0..n-1) committed by `expected_root`, the V2 withdrawal root a
 * fresh-lineage certificate signed. The path walk is the same level loop as
 * BuildWithdrawalRelayArtifacts (modern::FinalityHistoryPath), and the leaf
 * is folded back up the path as the vault does before anything is returned.
 */
inline std::optional<WithdrawalV2RelayArtifacts> BuildWithdrawalV2RelayArtifacts(
    const std::span<const modern::BridgeWithdrawalV2> withdrawals,
    const uint64_t withdrawal_id,
    const uint256& expected_root,
    std::string& error)
{
    auto fail{[&](std::string message)
                  -> std::optional<WithdrawalV2RelayArtifacts> {
        error = std::move(message);
        return std::nullopt;
    }};
    if (withdrawal_id >= withdrawals.size()) {
        return fail("withdrawal is not present in the finalized prefix");
    }
    if (withdrawals.size() > modern::MAX_WITHDRAWAL_LEAVES) {
        return fail("withdrawal prefix exceeds the depth-32 tree capacity");
    }
    std::vector<uint256> leaves;
    leaves.reserve(withdrawals.size());
    for (size_t i{0}; i < withdrawals.size(); ++i) {
        if (withdrawals[i].withdrawal_id != i) {
            return fail("withdrawal prefix is not consecutive");
        }
        const auto leaf{modern::BridgeWithdrawalLeafV2(withdrawals[i])};
        if (!leaf) return fail("withdrawal prefix contains an invalid leaf");
        leaves.push_back(*leaf);
    }

    uint256 root;
    const auto path{modern::FinalityHistoryPath(leaves, withdrawal_id, root)};
    if (!path || root != expected_root) {
        return fail("withdrawal prefix does not reproduce the finalized root");
    }

    WithdrawalV2RelayArtifacts out;
    out.withdrawal = withdrawals[withdrawal_id];
    out.leaf_preimage = *modern::EncodeBridgeWithdrawalV2(out.withdrawal);
    out.leaf = leaves[withdrawal_id];
    out.root = expected_root;
    out.path = *path;
    if (modern::FoldFinalityHistoryPath(out.leaf, withdrawal_id, out.path) !=
        expected_root) {
        return fail("withdrawal path does not fold to the finalized root");
    }

    using namespace bootstrap_detail;
    const auto selector{calldata_detail::Selector(VAULT_V2_RELEASE_SIGNATURE)};
    out.release_calldata.reserve(VAULT_V2_RELEASE_CALLDATA_SIZE);
    Append(out.release_calldata, selector);
    Append(out.release_calldata, Word(out.withdrawal.withdrawal_id));
    Append(out.release_calldata,
           calldata_detail::AddressWord(out.withdrawal.recipient));
    Append(out.release_calldata,
           Word(static_cast<uint64_t>(out.withdrawal.amount)));
    Append(out.release_calldata, Word(out.withdrawal.b3_height));
    for (const uint256& sibling : out.path) {
        Append(out.release_calldata, Word(sibling));
    }
    error.clear();
    return out;
}

} // namespace bridge

#endif // B3COIN_BRIDGE_WITHDRAWAL_V2_CALLDATA_H
