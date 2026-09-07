// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/licenses/mit/.
#ifndef B3COIN_NODE_FINALITY_SIGNING_POLICY_H
#define B3COIN_NODE_FINALITY_SIGNING_POLICY_H

#include <util/chaintype.h>

#include <algorithm>
#include <cstdint>
#include <optional>

namespace node {

//! Local production policy only; never a certificate-validity parameter.
inline constexpr int MAINNET_FINALITY_SIGNING_DEPTH{20};

struct FinalitySigningWindow {
    int first_checkpoint;
    int last_checkpoint;
};

/** Optional extra wait before creating a NEW signature. This cannot release
 * an ancestry lock, lower a watermark, or alter received-vote validation. */
struct FinalitySigningPolicy {
    //! Zero preserves consensus/scaled-fixture timing unless explicitly set.
    int minimum_depth{0};

    static constexpr FinalitySigningPolicy ForNetwork(const ChainType network)
    {
        return {network == ChainType::MAIN ? MAINNET_FINALITY_SIGNING_DEPTH : 0};
    }

    constexpr int EffectiveDepth(const int consensus_depth) const
    {
        return std::max(consensus_depth, minimum_depth);
    }

    /** Scheduled checkpoints above the caller's combined last-vote,
     * ancestry-lock and finalized watermark. Empty means wait, never rewind.
     * Wide intermediates also keep near-limit heights from wrapping. */
    std::optional<FinalitySigningWindow> Checkpoints(
        const int tip_height, const int consensus_depth,
        const int modern_start, const int checkpoint_interval,
        const int watermark) const
    {
        if (tip_height < 0 || consensus_depth < 0 || minimum_depth < 0 ||
            modern_start < 0 || checkpoint_interval <= 0) return std::nullopt;
        const int64_t upper{int64_t{tip_height} - EffectiveDepth(consensus_depth)};
        int64_t first{std::max(int64_t{modern_start}, int64_t{watermark} + 1)};
        if (first > upper) return std::nullopt;
        const int64_t remainder{(first - modern_start) % checkpoint_interval};
        if (remainder != 0) first += checkpoint_interval - remainder;
        if (first > upper) return std::nullopt;
        const int64_t last{upper - (upper - modern_start) % checkpoint_interval};
        return FinalitySigningWindow{static_cast<int>(first), static_cast<int>(last)};
    }
};

} // namespace node

#endif // B3COIN_NODE_FINALITY_SIGNING_POLICY_H
