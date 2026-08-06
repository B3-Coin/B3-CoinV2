// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2014 The B3Coin developers
// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_LEGACY_CONSENSUS_H
#define B3COIN_LEGACY_CONSENSUS_H

#include <consensus/amount.h>
#include <consensus/params.h>
#include <uint256.h>

#include <cstdint>
#include <optional>

class CBlock;
class CBlockIndex;
class CCoinsViewCache;
class CTransaction;

namespace legacy {

/** Consensus constants frozen from the existing B3Coin chain. */
inline constexpr uint32_t MAX_BLOCK_SIZE{5'000'000};
inline constexpr uint32_t MAX_BLOCK_SIGOPS{MAX_BLOCK_SIZE / 100};
inline constexpr uint32_t COINBASE_MATURITY{30};
inline constexpr uint32_t STAKE_MIN_CONFIRMATIONS{10};
inline constexpr uint32_t STAKE_MIN_AGE{60 * 60};
inline constexpr uint32_t STAKE_MODIFIER_INTERVAL{15 * 60};
inline constexpr uint32_t STAKE_MODIFIER_INTERVAL_RATIO{3};
inline constexpr uint32_t STAKE_TARGET_SPACING{360};
inline constexpr uint32_t TARGET_TIMESPAN{STAKE_TARGET_SPACING * 20};
inline constexpr uint32_t TARGET_SPACING_WORK_MAX{STAKE_TARGET_SPACING * 3};
inline constexpr uint32_t MAX_FUTURE_BLOCK_TIME{10 * 60};
inline constexpr uint32_t MAX_FUTURE_COINBASE_TIME_POW{100'000};
inline constexpr CAmount CENT{COIN / 100};
inline constexpr CAmount COIN_YEAR_REWARD{CENT};
/**
 * Historical fee accounting treated a 2.5M B3 shortfall as a special burn.
 * It is retained solely to reproduce existing blocks; it does not enable any
 * Fundamental Node service, payment, vote, or validation mechanism.
 */
inline constexpr CAmount LEGACY_FUNDAMENTALNODE_BURN{2'500'000 * COIN};

/** True when a height must use the preserved B3Coin consensus rules. */
bool IsActive(const Consensus::Params& params, int height);

/** Old hybrid PoW/PoS per-block target adjustment. */
uint32_t GetNextTargetRequired(const CBlockIndex* pindex_last, bool proof_of_stake,
                               const Consensus::Params& params);

/** Validate the old post-transaction signature on proof-of-stake blocks. */
bool CheckBlockSignature(const CBlock& block);

struct StakeProof {
    uint256 hash;
    uint64_t coin_day_weight;
};

/**
 * Validate the old Peercoin-v1 stake kernel. Script validation remains in the
 * normal Core validation path so every coinstake input is checked uniformly.
 */
std::optional<StakeProof> CheckStakeKernel(const CBlockIndex* pindex_prev,
                                           const CTransaction& tx,
                                           const CCoinsViewCache& view,
                                           uint32_t bits);

/** Compute coin age in old-chain coin-days for a coinstake transaction. */
std::optional<uint64_t> GetCoinAge(const CTransaction& tx,
                                   const CBlockIndex* pindex_prev,
                                   const CCoinsViewCache& view);

/** Rebuild the Peercoin-v1 stake modifier for a newly-connected block. */
bool ComputeNextStakeModifier(const CBlockIndex* pindex_prev,
                              uint64_t& stake_modifier,
                              bool& generated);

/** Legacy issuance rules, with Fundamental Node logic intentionally omitted. */
CAmount GetProofOfWorkReward(CAmount fees, int height,
                             const Consensus::Params& params);
CAmount GetProofOfStakeReward(const CBlockIndex* pindex_prev,
                              uint64_t coin_age, CAmount fees);

/** Return the fee amount the legacy block-reward calculation counted. */
CAmount GetLegacyTransactionFee(CAmount value_in, CAmount value_out, bool is_coinstake);

/**
 * Heights where the legacy client deliberately bypassed its normal coinstake
 * reward cap. They remain a narrow historical compatibility exception only;
 * no Fundamental Node mechanism is enabled by this function.
 */
bool IsHistoricalStakeRewardCapException(int height);

} // namespace legacy

#endif // B3COIN_LEGACY_CONSENSUS_H
