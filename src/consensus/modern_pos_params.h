// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_CONSENSUS_MODERN_POS_PARAMS_H
#define B3COIN_CONSENSUS_MODERN_POS_PARAMS_H

#include <cstdint>
#include <vector>
#include <optional>

namespace Consensus {

/**
 * Modern PoS V1 parameter block — THE single configurable home of every
 * modern-PoS number (doc/design/b3-modern-pos-spec.md §9).
 *
 * Ratification state (owner rulings 2026-08-21): the timing values —
 * block interval, round length, f0, and the fixed x2 relaxation — are
 * RATIFIED as the V1 numbers. The remaining fields stay
 * REVISABLE_BEFORE_MAINNET provisionals, and the reorganization horizon D
 * is an owner decision with deliberately no default. Real chainparams
 * still never construct this block — while
 * Params::modern_pos is unset, modern-PoS validation and production FAIL
 * CLOSED (`no-modern-pos-rules`), exactly like the corridor's unset
 * difficulty. A guard test pins that no shipped network configures it.
 *
 * The V1 mechanism these numbers feed (frozen, not revisable here):
 *
 *   digest = TaggedHash("B3/MODERN/POS/ELIG/V1",
 *                       ModernChainDomain || seed || height || round || key)
 *   eligible  iff  digest < MAX256 * f0 * 2^round * w / W
 *   nTime     = parent.nTime + block_interval + round * round_seconds  EXACTLY
 *
 * There is no difficulty retarget: the w/W normalization IS the difficulty.
 * nBits is a fixed enforced sentinel; nNonce must be 0.
 */
struct ModernPosParams {
    //! RATIFIED 2026-08-21: forced minimum block spacing (seconds).
    int64_t block_interval_seconds{60};
    //! RATIFIED 2026-08-21: recovery-round length (seconds). Each empty
    //! round doubles every validator's eligibility (f(r) = f0 * 2^r; the
    //! doubling factor is likewise ratified as fixed x2 in V1).
    int64_t round_seconds{30};
    //! RATIFIED 2026-08-21: f0 as a rational. Expected eligible
    //! validators in round 0 ~= f0 * (online stake fraction).
    uint32_t f0_num{1};
    uint32_t f0_den{1};
    //! REVISABLE_BEFORE_MAINNET: the constant nBits every modern-PoS block
    //! must carry. A dead field kept only for header-layout compatibility;
    //! its GetBlockProof() constant makes nChainWork a height counter for
    //! modern chains (bookkeeping, not work).
    uint32_t sentinel_bits{0x207fffff};
    //! REVISABLE_BEFORE_MAINNET: clock-skew allowance. Because timestamps
    //! are exact, this doubles as the pacing gate: a round's block cannot be
    //! accepted before its forced timestamp minus this bound.
    int64_t max_future_seconds{120};
    //! OD-2 RULED 2026-08-26: reward = R0, the INITIAL per-block subsidy
    //! (fees ride on top), halving every `halving_interval` blocks from M:
    //!   subsidy(h) = reward >> ((h - M) / halving_interval)
    //! R0 itself = floor(S_H * 1% / 525,600) with S_H measured at H — it is
    //! pinned in the X-pin release; 0 (the shipped default) = fees only,
    //! so nothing mints by omission.
    int64_t reward{0};
    //! Halving interval in modern-PoS blocks. RULED: 525,600 (one year at
    //! the 60 s interval). 0 = no halving (flat `reward` forever).
    int64_t halving_interval{0};
    //! OD-2 treasury split: this percentage of each block's SUBSIDY (never
    //! the fees) must pay to `treasury_script` in the coinbase. RULED:
    //! 10, to the single owner treasury address. 0 or an empty script
    //! disables enforcement (and the miner pays no treasury output).
    uint32_t treasury_percent{0};
    std::vector<unsigned char> treasury_script{};
    //! Modern reorganization horizon D: a reorg deeper than this many
    //! modern-PoS blocks is refused without peer penalty. RATIFIED
    //! 2026-08-21: 1440 — one day of history at the ratified 60-second
    //! interval becomes final for online nodes. Fixtures may override with
    //! small scaffolding values; unset disables the horizon.
    std::optional<int> reorg_horizon{1440};
    //! RATIFIED 2026-08-23 (ruling M7, frozen constants): BLS finality gadget
    //! schedule. E = validator-set epoch length in modern-PoS blocks;
    //! checkpoints every `checkpoint_interval` blocks, signed once
    //! `checkpoint_depth` deep; a certificate-gated epoch may extend by at
    //! most `max_epoch_extension` blocks before the finality lineage is
    //! declared broken; `min_finality_set` is the chain BOOTSTRAP floor
    //! only (never a bridge security threshold). Fixtures may scale these
    //! down exactly as they scale reorg_horizon; real chainparams ship
    //! the whole block unset, so nothing here is live before the X-pin
    //! release pins it. Declarations only at this stage: no rule in the
    //! tree reads them yet (implementation plan, Commit 1).
    int finality_epoch_blocks{1440};
    int checkpoint_interval{10};
    int checkpoint_depth{12};
    int max_epoch_extension{7 * 1440};
    int min_finality_set{4};

    //! Structural sanity of a configured block (not economics).
    constexpr bool Valid() const
    {
        return block_interval_seconds > 0 && round_seconds > 0 && f0_num > 0 && f0_den > 0 &&
               max_future_seconds >= 0 && reward >= 0 && halving_interval >= 0 &&
               treasury_percent <= 100 && treasury_script.size() <= 128 &&
               (treasury_percent == 0 || !treasury_script.empty()) &&
               (!reorg_horizon || *reorg_horizon > 0) &&
               finality_epoch_blocks > 0 && checkpoint_interval > 0 &&
               checkpoint_interval <= finality_epoch_blocks &&
               checkpoint_depth >= 0 && checkpoint_depth < finality_epoch_blocks &&
               max_epoch_extension >= finality_epoch_blocks && min_finality_set >= 1;
    }
};

/** OD-2 subsidy schedule (owner ruling 2026-08-26): the per-block subsidy
 *  at modern height `height`, where `m_height` is the first modern-PoS
 *  height M. Pure integer arithmetic; shifts of 63+ are zero. */
constexpr int64_t ModernBlockSubsidy(const int height, const int m_height,
                                     const ModernPosParams& params)
{
    if (height < m_height || params.reward <= 0) return 0;
    if (params.halving_interval <= 0) return params.reward;
    const int64_t halvings{(static_cast<int64_t>(height) - m_height) / params.halving_interval};
    if (halvings >= 63) return 0;
    return params.reward >> halvings;
}

/** The treasury share of a subsidy (never of fees): floor(subsidy * pct / 100). */
constexpr int64_t ModernTreasuryShare(const int64_t subsidy, const ModernPosParams& params)
{
    if (subsidy <= 0 || params.treasury_percent == 0 || params.treasury_script.empty()) return 0;
    return subsidy / 100 * params.treasury_percent +
           (subsidy % 100) * params.treasury_percent / 100;
}


} // namespace Consensus

#endif // B3COIN_CONSENSUS_MODERN_POS_PARAMS_H
