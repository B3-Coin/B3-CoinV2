// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2014 The B3Coin developers
// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <legacy/consensus.h>

#include <arith_uint256.h>
#include <chain.h>
#include <coins.h>
#include <consensus/era.h>
#include <hash.h>
#include <legacy/pos.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <script/solver.h>
#include <util/strencodings.h>

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
    return Consensus::GetB3Era(height, params) == Consensus::B3Era::LEGACY;
}

void InitializeGenesisBlockIndex(CBlockIndex& index, const uint256& proof_hash)
{
    index.m_legacy_proof_of_stake = false;
    index.m_legacy_stake_modifier_generated = true;
    index.m_legacy_stake_modifier = 0;
    index.m_legacy_hash_proof = proof_hash;
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
        // The historical uint256 type ordered hashes as numeric little-endian
        // integers. Core's uint256 orders raw bytes lexicographically, which
        // is different and would select a different stake modifier.
        return UintToArith256(a->GetBlockHash()) < UintToArith256(b->GetBlockHash());
    });

    uint64_t next_modifier{0};
    int64_t selection_stop{selection_start};
    std::set<uint256> selected;
    for (int round{0}; round < std::min<int>(64, candidates.size()); ++round) {
        selection_stop += SelectionIntervalSection(round);
        const CBlockIndex* best{nullptr};
        arith_uint256 best_hash;
        for (const CBlockIndex* candidate : candidates) {
            if (best && candidate->GetBlockTime() > selection_stop) break;
            if (selected.contains(candidate->GetBlockHash())) continue;
            HashWriter writer;
            writer << candidate->m_legacy_hash_proof << stake_modifier;
            arith_uint256 selection_hash{UintToArith256(writer.GetHash())};
            if (candidate->m_legacy_proof_of_stake) selection_hash >>= 32;
            if (!best || selection_hash < best_hash) {
                best = candidate;
                best_hash = selection_hash;
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

CAmount GetFNCollateral(const int height, const Consensus::Params& params)
{
    // Historical Proof-of-Disintegration collateral tiers, verbatim from
    // fn-activity.h. The test override exists ONLY so synthetic regtest
    // chains can afford an authentic disintegration; the mainnet schedule
    // is consensus history and never changes.
    if (params.legacy_fn_collateral_test_override) {
        return *params.legacy_fn_collateral_test_override;
    }
    if (height > 105'000) return 15'000'000 * COIN;
    if (height > 85'000) return 20'000'000 * COIN;
    return 25'000'000 * COIN;
}

CAmount GetLegacyTransactionFee(const CAmount value_in, const CAmount value_out,
                                const bool is_coinstake, const int height,
                                const Consensus::Params& params)
{
    // Proof of Disintegration, exactly as the historical client accounted
    // it: when a transaction's input/output gap reaches the collateral, the
    // collateral portion is NOT a fee -- it is destroyed, unclaimable by
    // the block producer, and leaves the spendable supply permanently.
    if (is_coinstake) return 0;
    const CAmount raw_fee{value_in - value_out};
    const CAmount collateral{GetFNCollateral(height, params)};
    return raw_fee >= collateral ? raw_fee - collateral : raw_fee;
}

bool IsRepairWindowHeight(const int height)
{
    // Verbatim from the final client's ConnectBlock: the whole coinstake
    // reward-cap check is skipped for 77447..77505 inclusive.
    return height > 77'446 && height < 77'506;
}

CScript SuperblockPayeeScript(const std::vector<unsigned char>& pubkey)
{
    // superlockPayee.SetDestination(superblockPubkey.GetID()): the P2PKH
    // script of the superblock key's hash.
    const CPubKey key{pubkey};
    return CScript() << OP_DUP << OP_HASH160 << ToByteVector(key.GetID()) << OP_EQUALVERIFY << OP_CHECKSIG;
}

bool StakeDestinationMatches(const CScript& script_pub_key, const uint160& key_id)
{
    // The 0.8-era client compared CTxDestination equality after its
    // ExtractDestination, whose Solver folded both pay-to-pubkey-hash and
    // pay-to-pubkey into the key's address. Reproduce exactly that fold.
    std::vector<std::vector<unsigned char>> solutions;
    switch (Solver(script_pub_key, solutions)) {
    case TxoutType::PUBKEYHASH:
        return uint160{solutions[0]} == key_id;
    case TxoutType::PUBKEY:
        return CPubKey{solutions[0]}.GetID() == CKeyID{key_id};
    default:
        return false;
    }
}

const uint160& RestrictedStakeKeyId()
{
    // Base58 payload of ShJsVNBQMa2M7cfCVPzRMt8nVZxHitBp7v (version byte 63).
    static const uint160 key_id{[] {
        uint160 id;
        const auto bytes{ParseHex("db8ca2a4493aaed6b7d2f30acb4467b823e0b0a5")};
        std::copy(bytes.begin(), bytes.end(), id.begin());
        return id;
    }()};
    return key_id;
}

bool StakeDestinationIsRestricted(const CScript& script_pub_key)
{
    return StakeDestinationMatches(script_pub_key, RestrictedStakeKeyId());
}

bool CheckpointAllows(const Consensus::Params& params, const int height, const uint256& hash)
{
    const auto it{params.legacy_checkpoints.find(height)};
    if (it == params.legacy_checkpoints.end()) return true;
    return hash == it->second;
}

bool ReorgDepthExceeded(const Consensus::Params& params, const int block_height, const int active_tip_height)
{
    if (params.legacy_checkpoint_span <= 0) return false;
    // Mirrors AutoSelectSyncCheckpoint + CheckSync: the sync checkpoint sits at
    // active_tip_height - span (clamped at genesis), and a block at or below it
    // is refused. For a linear active chain this is exactly that height.
    return block_height <= active_tip_height - params.legacy_checkpoint_span;
}

} // namespace legacy
