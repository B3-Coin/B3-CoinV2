// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_CONSENSUS_MODERN_POS_PARAMS_H
#define B3COIN_CONSENSUS_MODERN_POS_PARAMS_H

#include <cstdint>
#include <optional>

namespace Consensus {

/**
 * Modern PoS V1 parameter block — THE single configurable home of every
 * modern-PoS number (doc/design/b3-modern-pos-spec.md §9).
 *
 * REVISABLE_BEFORE_MAINNET: every default below is a PROVISIONAL regtest
 * scaffolding value under the frozen V1 mechanism. None is ratified mainnet
 * policy. Real chainparams never construct this block — while
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
    //! REVISABLE_BEFORE_MAINNET: forced minimum block spacing (seconds).
    int64_t block_interval_seconds{60};
    //! REVISABLE_BEFORE_MAINNET: recovery-round length (seconds). Each empty
    //! round doubles every validator's eligibility (f(r) = f0 * 2^r; the
    //! doubling factor is fixed in V1).
    int64_t round_seconds{30};
    //! REVISABLE_BEFORE_MAINNET: f0 as a rational. Expected eligible
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
    //! REVISABLE_BEFORE_MAINNET: per-block subsidy on top of fees, bounded
    //! by the unconditional modern coinbase cap. 0 = fees only. The real
    //! schedule is an owner economics decision (OD-2).
    int64_t reward{0};
    //! Modern reorganization horizon D: a reorg deeper than this many
    //! modern-PoS blocks is refused without peer penalty. OWNER DECISION —
    //! deliberately no default; unset disables the horizon (V1 regtest
    //! fixtures set a scaffolding value).
    std::optional<int> reorg_horizon;

    //! Structural sanity of a configured block (not economics).
    constexpr bool Valid() const
    {
        return block_interval_seconds > 0 && round_seconds > 0 && f0_num > 0 && f0_den > 0 &&
               max_future_seconds >= 0 && reward >= 0 &&
               (!reorg_horizon || *reorg_horizon > 0);
    }
};

} // namespace Consensus

#endif // B3COIN_CONSENSUS_MODERN_POS_PARAMS_H
