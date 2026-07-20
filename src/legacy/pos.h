// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2014 The B3Coin developers
// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_LEGACY_POS_H
#define B3COIN_LEGACY_POS_H

#include <arith_uint256.h>
#include <uint256.h>

#include <cstdint>
#include <optional>

namespace legacy::pos {

/** Frozen old-chain constants. These are not the post-fork PoS design. */
struct Params {
    static constexpr int64_t COIN{1'000'000};
    static constexpr uint32_t TARGET_SPACING{360};
    static constexpr uint32_t TARGET_TIMESPAN{360 * 20};
    static constexpr uint32_t MIN_CONFIRMATIONS{10};
    static constexpr uint32_t MIN_AGE{60 * 60};
    static constexpr uint32_t MODIFIER_INTERVAL{15 * 60};
    static constexpr uint32_t COINBASE_MATURITY{30};
};

struct KernelInput {
    uint64_t stake_modifier;
    uint32_t source_block_time;
    uint32_t source_transaction_offset;
    uint32_t source_transaction_time;
    uint32_t source_output_index;
    uint32_t stake_time;
};

struct KernelResult {
    uint256 proof_hash;
    arith_uint256 weighted_target;
    uint64_t coin_day_weight;

    bool IsValid() const;
};

uint256 ComputeKernelHash(const KernelInput& input);

/**
 * Evaluate the old-chain Peercoin-v1 stake kernel after its modifier has been
 * selected. Chain lookup, confirmations, and signature checks stay outside
 * this pure consensus calculation.
 */
std::optional<KernelResult> EvaluateKernel(const KernelInput& input,
                                           uint32_t bits,
                                           int64_t value);

bool CheckCoinStakeTimestamp(uint32_t block_time, uint32_t transaction_time);

} // namespace legacy::pos

#endif // B3COIN_LEGACY_POS_H
