// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2014 The B3Coin developers
// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <legacy/pos.h>

#include <hash.h>

namespace legacy::pos {

uint256 ComputeKernelHash(const KernelInput& input)
{
    HashWriter writer;
    writer << input.stake_modifier
           << input.source_block_time
           << input.source_transaction_offset
           << input.source_transaction_time
           << input.source_output_index
           << input.stake_time;
    return writer.GetHash();
}

std::optional<KernelResult> EvaluateKernel(const KernelInput& input,
                                           uint32_t bits,
                                           int64_t value)
{
    if (value <= 0 || input.stake_time < input.source_transaction_time) return std::nullopt;

    const int64_t age{static_cast<int64_t>(input.stake_time) -
                      static_cast<int64_t>(input.source_transaction_time) -
                      Params::MIN_AGE};
    if (age <= 0) return std::nullopt;

    arith_uint256 coin_day_weight_wide{static_cast<uint64_t>(value)};
    coin_day_weight_wide *= static_cast<uint32_t>(age);
    coin_day_weight_wide /= arith_uint256{Params::COIN};
    coin_day_weight_wide /= arith_uint256{24 * 60 * 60};
    if (coin_day_weight_wide.bits() > 64) return std::nullopt;
    const uint64_t coin_day_weight{coin_day_weight_wide.GetLow64()};

    arith_uint256 target;
    bool negative{false};
    bool overflow{false};
    target.SetCompact(bits, &negative, &overflow);
    if (negative || overflow || target == 0 || coin_day_weight == 0) return std::nullopt;
    const arith_uint256 max_target{~arith_uint256{0}};
    const arith_uint256 weight{coin_day_weight};
    if (target > max_target / weight) {
        target = max_target;
    } else {
        target *= weight;
    }

    return KernelResult{
        .proof_hash = ComputeKernelHash(input),
        .weighted_target = target,
        .coin_day_weight = coin_day_weight,
    };
}

bool KernelResult::IsValid() const
{
    return UintToArith256(proof_hash) <= weighted_target;
}

bool CheckCoinStakeTimestamp(uint32_t block_time, uint32_t transaction_time)
{
    return block_time == transaction_time;
}

} // namespace legacy::pos
