// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2014 The B3Coin developers
// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <legacy/consensus.h>

#include <arith_uint256.h>
#include <chain.h>
#include <coins.h>
#include <hash.h>
#include <legacy/pos.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <script/solver.h>

#include <algorithm>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace legacy {
namespace {

const CBlockIndex* LastBlockOfType(const CBlockIndex* pindex, const bool proof_of_stake)
{
    while (pindex && pindex->pprev && pindex->m_legacy_proof_of_stake != proof_of_stake) {
        pindex = pindex->pprev;
    }
    return pindex;
}

int64_t SelectionIntervalSection(const int section)
{
    return STAKE_MODIFIER_INTERVAL * 63 /
           (63 + ((63 - section) * (STAKE_MODIFIER_INTERVAL_RATIO - 1)));
}

int64_t SelectionInterval()
{
    int64_t interval{0};
    for (int section{0}; section < 64; ++section) {
        interval += SelectionIntervalSection(section);
    }
    return interval;
}

const CBlockIndex* CandidateAtHeight(const CBlockIndex* tip, const int height)
{
    return tip && height >= 0 && height <= tip->nHeight ? tip->GetAncestor(height) : nullptr;
}

std::optional<uint64_t> KernelStakeModifier(const CBlockIndex* pindex_prev,
                                            const CBlockIndex* pindex_from)
{
    if (!pindex_prev || !pindex_from || pindex_from->nHeight > pindex_prev->nHeight) {
        return std::nullopt;
    }

    int64_t modifier_time{pindex_from->GetBlockTime()};
    const int64_t selection_stop{pindex_from->GetBlockTime() + SelectionInterval()};
    const CBlockIndex* pindex{pindex_from};

    while (modifier_time < selection_stop) {
        if (pindex->nHeight >= pindex_prev->nHeight) {
            return std::nullopt;
        }
        pindex = CandidateAtHeight(pindex_prev, pindex->nHeight + 1);
        if (!pindex) return std::nullopt;
        if (pindex->m_legacy_stake_modifier_generated) {
            modifier_time = pindex->GetBlockTime();
        }
    }
    return pindex->m_legacy_stake_modifier;
}

std::optional<uint64_t> ToUint64(const arith_uint256& value)
{
    if (value.bits() > 64) return std::nullopt;
    return value.GetLow64();
}

} // namespace

bool IsActive(const Consensus::Params& params, const int height)
{
    return params.legacy_b3coin &&
           (!params.hard_fork_height || height < *params.hard_fork_height);
}

uint32_t GetNextTargetRequired(const CBlockIndex* pindex_last, const bool proof_of_stake,
                               const Consensus::Params& params)
{
    const arith_uint256 target_limit{UintToArith256(params.powLimit)};
    if (!pindex_last) return target_limit.GetCompact();

    const CBlockIndex* pindex_prev{LastBlockOfType(pindex_last, proof_of_stake)};
    if (!pindex_prev || !pindex_prev->pprev) return target_limit.GetCompact();
    const CBlockIndex* pindex_prev_prev{LastBlockOfType(pindex_prev->pprev, proof_of_stake)};
    if (!pindex_prev_prev || !pindex_prev_prev->pprev) return target_limit.GetCompact();

    const int64_t target_spacing{proof_of_stake ?
        STAKE_TARGET_SPACING :
        std::min<int64_t>(TARGET_SPACING_WORK_MAX,
                          int64_t{STAKE_TARGET_SPACING} *
                              (1 + pindex_last->nHeight - pindex_prev->nHeight))};
    int64_t actual_spacing{pindex_prev->GetBlockTime() - pindex_prev_prev->GetBlockTime()};
    if (actual_spacing < 0) {
        actual_spacing = 1;
    } else if (actual_spacing > TARGET_TIMESPAN) {
        actual_spacing = TARGET_TIMESPAN;
    }
    actual_spacing = std::min(actual_spacing, target_spacing * 10);

    bool negative{false};
    bool overflow{false};
    arith_uint256 target;
    target.SetCompact(pindex_prev->nBits, &negative, &overflow);
    if (negative || overflow || target == 0) return target_limit.GetCompact();

    const int64_t interval{TARGET_TIMESPAN / target_spacing};
    const uint32_t numerator{static_cast<uint32_t>((interval - 1) * target_spacing + actual_spacing + actual_spacing)};
    const uint32_t denominator{static_cast<uint32_t>((interval + 1) * target_spacing)};
    target *= numerator;
    target /= arith_uint256{denominator};
    if (target == 0 || target > target_limit) target = target_limit;
    return target.GetCompact();
}

bool CheckBlockSignature(const CBlock& block)
{
    if (block.IsProofOfWork()) return block.vchBlockSig.empty();
    if (block.vchBlockSig.empty() || block.vtx.size() < 2 || block.vtx[1]->vout.size() < 2) return false;

    const CScript& script{block.vtx[1]->vout[1].scriptPubKey};
    std::vector<std::vector<unsigned char>> solutions;
    if (Solver(script, solutions) == TxoutType::PUBKEY && solutions.size() == 1) {
        return CPubKey{solutions[0]}.Verify(block.GetLegacyB3Hash(), block.vchBlockSig);
    }

    CScript::const_iterator pc{script.begin()};
    opcodetype opcode;
    std::vector<unsigned char> pushed;
    if (!script.GetOp(pc, opcode, pushed) || opcode != OP_RETURN ||
        !script.GetOp(pc, opcode, pushed) || !CPubKey::ValidSize(pushed)) {
        return false;
    }
    return CPubKey{pushed}.Verify(block.GetLegacyB3Hash(), block.vchBlockSig);
}

std::optional<StakeProof> CheckStakeKernel(const CBlockIndex* pindex_prev,
                                           const CTransaction& tx,
                                           const CCoinsViewCache& view,
                                           const uint32_t bits)
{
    if (!tx.IsCoinStake() || tx.vin.empty() || !pindex_prev) return std::nullopt;

    const CTxIn& txin{tx.vin.front()};
    const Coin& coin{view.AccessCoin(txin.prevout)};
    if (coin.IsSpent() || txin.prevout.n >= std::numeric_limits<uint32_t>::max()) return std::nullopt;
    if (pindex_prev->nHeight - static_cast<int>(coin.nHeight) <
        static_cast<int>(STAKE_MIN_CONFIRMATIONS - 1)) {
        return std::nullopt;
    }

    const CBlockIndex* source{pindex_prev->GetAncestor(coin.nHeight)};
    if (!source || tx.nTime < coin.nTime) return std::nullopt;
    const auto modifier{KernelStakeModifier(pindex_prev, source)};
    if (!modifier) return std::nullopt;

    const pos::KernelInput kernel{
        .stake_modifier = *modifier,
        .source_block_time = source->nTime,
        .source_transaction_offset = coin.nTxOffset,
        .source_transaction_time = coin.nTime,
        .source_output_index = txin.prevout.n,
        .stake_time = tx.nTime,
    };
    const auto result{pos::EvaluateKernel(kernel, bits, coin.out.nValue)};
    if (!result || !result->IsValid()) return std::nullopt;
    return StakeProof{.hash = result->proof_hash, .coin_day_weight = result->coin_day_weight};
}

std::optional<uint64_t> GetCoinAge(const CTransaction& tx,
                                   const CBlockIndex* pindex_prev,
                                   const CCoinsViewCache& view)
{
    if (!pindex_prev) return std::nullopt;
    arith_uint256 cent_seconds;
    for (const CTxIn& txin : tx.vin) {
        const Coin& coin{view.AccessCoin(txin.prevout)};
        if (coin.IsSpent() || tx.nTime < coin.nTime || coin.nHeight > static_cast<uint32_t>(pindex_prev->nHeight)) {
            return std::nullopt;
        }
        const CBlockIndex* source{pindex_prev->GetAncestor(coin.nHeight)};
        if (!source) return std::nullopt;
        if (source->GetBlockTime() + STAKE_MIN_AGE > tx.nTime) continue;

        arith_uint256 value{static_cast<uint64_t>(coin.out.nValue)};
        value *= static_cast<uint32_t>(tx.nTime - coin.nTime);
        value /= arith_uint256{static_cast<uint64_t>(CENT)};
        cent_seconds += value;
    }
    cent_seconds *= static_cast<uint32_t>(CENT);
    cent_seconds /= arith_uint256{static_cast<uint64_t>(COIN)};
    cent_seconds /= arith_uint256{24 * 60 * 60};
    return ToUint64(cent_seconds);
}

bool ComputeNextStakeModifier(const CBlockIndex* pindex_prev,
                              uint64_t& stake_modifier,
                              bool& generated)
{
    stake_modifier = 0;
    generated = false;
    if (!pindex_prev) {
        generated = true;
        return true;
    }

    const CBlockIndex* last_modifier{pindex_prev};
    while (last_modifier && last_modifier->pprev && !last_modifier->m_legacy_stake_modifier_generated) {
        last_modifier = last_modifier->pprev;
    }
    if (!last_modifier || !last_modifier->m_legacy_stake_modifier_generated) return false;
    stake_modifier = last_modifier->m_legacy_stake_modifier;
    if (last_modifier->GetBlockTime() / STAKE_MODIFIER_INTERVAL >=
        pindex_prev->GetBlockTime() / STAKE_MODIFIER_INTERVAL) {
        return true;
    }

    const int64_t selection_start{
        (pindex_prev->GetBlockTime() / STAKE_MODIFIER_INTERVAL) * STAKE_MODIFIER_INTERVAL - SelectionInterval()};
    std::vector<const CBlockIndex*> candidates;
    for (const CBlockIndex* index{pindex_prev}; index && index->GetBlockTime() >= selection_start; index = index->pprev) {
        candidates.push_back(index);
    }
    std::reverse(candidates.begin(), candidates.end());
    std::sort(candidates.begin(), candidates.end(), [](const CBlockIndex* a, const CBlockIndex* b) {
        if (a->GetBlockTime() != b->GetBlockTime()) return a->GetBlockTime() < b->GetBlockTime();
        return a->GetBlockHash() < b->GetBlockHash();
    });

    uint64_t next_modifier{0};
    int64_t selection_stop{selection_start};
    std::set<uint256> selected;
    for (int round{0}; round < std::min<int>(64, candidates.size()); ++round) {
        selection_stop += SelectionIntervalSection(round);
        const CBlockIndex* best{nullptr};
        uint256 best_hash;
        for (const CBlockIndex* candidate : candidates) {
            if (best && candidate->GetBlockTime() > selection_stop) break;
            if (selected.contains(candidate->GetBlockHash())) continue;
            HashWriter writer;
            writer << candidate->m_legacy_hash_proof << stake_modifier;
            arith_uint256 selection_hash{UintToArith256(writer.GetHash())};
            if (candidate->m_legacy_proof_of_stake) selection_hash >>= 32;
            const uint256 comparison{ArithToUint256(selection_hash)};
            if (!best || comparison < best_hash) {
                best = candidate;
                best_hash = comparison;
            }
        }
        if (!best) return false;
        next_modifier |= (best->GetBlockHash().GetUint64(0) & 1ULL) << round;
        selected.insert(best->GetBlockHash());
    }

    stake_modifier = next_modifier;
    generated = true;
    return true;
}

CAmount GetProofOfWorkReward(const CAmount fees, const int height,
                             const Consensus::Params& params)
{
    CAmount subsidy{10 * COIN};
    if (height == 1) subsidy = 260'000 * COIN;
    if (height > params.legacy_last_pow_block) return fees;
    return subsidy + fees;
}

CAmount GetProofOfStakeReward(const CBlockIndex* pindex_prev,
                              const uint64_t coin_age, const CAmount fees)
{
    if (!pindex_prev) return fees + COIN;
    arith_uint256 subsidy{coin_age};
    subsidy *= static_cast<uint32_t>(COIN_YEAR_REWARD);
    subsidy *= 33;
    subsidy /= arith_uint256{365 * 33 + 8};

    uint32_t multiplier{8};
    if (pindex_prev->nHeight >= 1'000'000) multiplier = 5;
    else if (pindex_prev->nHeight >= 110'000) multiplier = 20;
    else if (pindex_prev->nHeight >= 80'000) multiplier = 100;
    else if (pindex_prev->nHeight >= 60'000) multiplier = 10'000;
    else if (pindex_prev->nHeight >= 50'000) multiplier = 1'000;
    else if (pindex_prev->nHeight >= 40'000) multiplier = 100;
    else if (pindex_prev->nHeight >= 30'000) multiplier = 50;
    else if (pindex_prev->nHeight >= 20'000) multiplier = 25;
    else if (pindex_prev->nHeight >= 10'000) multiplier = 15;
    subsidy *= multiplier;

    const auto value{ToUint64(subsidy)};
    if (!value || *value > static_cast<uint64_t>(std::numeric_limits<CAmount>::max())) {
        return std::numeric_limits<CAmount>::max();
    }
    const CAmount base{static_cast<CAmount>(*value)};
    if (base > std::numeric_limits<CAmount>::max() - fees - COIN) {
        return std::numeric_limits<CAmount>::max();
    }
    return base + fees + COIN;
}

CAmount GetLegacyTransactionFee(const CAmount value_in, const CAmount value_out,
                                const bool is_coinstake)
{
    if (is_coinstake) return 0;
    const CAmount raw_fee{value_in - value_out};
    return raw_fee >= LEGACY_FUNDAMENTALNODE_BURN ? raw_fee - LEGACY_FUNDAMENTALNODE_BURN : raw_fee;
}

bool IsHistoricalStakeRewardCapException(const int height)
{
    // The original B3Coin validator exempted this short repair interval and
    // one historical superblock from its usual cap. This preserves existing
    // chain history without reintroducing Fundamental Node consensus.
    return (height > 77'446 && height < 77'506) || height == 107'488;
}

} // namespace legacy
