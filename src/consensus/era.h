// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_CONSENSUS_ERA_H
#define BITCOIN_CONSENSUS_ERA_H

#include <consensus/params.h>

#include <optional>

namespace Consensus {

/**
 * The two consensus eras of the continuous B3 chain.
 *
 * The chain keeps one genesis and one uninterrupted history. Blocks at
 * height <= LEGACY_FINAL_HEIGHT (H) belong to the LEGACY era and are
 * replayed under the preserved historical rules; blocks at height > H
 * belong to the MODERN era and receive full modern validation. A
 * reorganization crossing H is permanently prohibited once the boundary
 * is configured.
 *
 * Chains that never had a legacy B3Coin history (Bitcoin test chains,
 * plain regtest) are MODERN at every height.
 */
enum class B3Era {
    LEGACY,
    MODERN,
};

/**
 * Return the consensus era governing the block at `height`.
 *
 * `Params::hard_fork_height` stores the first MODERN height, i.e.
 * LEGACY_FINAL_HEIGHT + 1. While it is unset, every height of a
 * legacy-B3Coin chain remains LEGACY.
 *
 * This is the single source of truth for era selection. Use it only
 * where the block height (and therefore chain context) is already
 * unambiguous — for example against a connected CBlockIndex, or a
 * height derived from a known parent. It must NOT be used to choose a
 * wire decoder for a not-yet-decoded block: at that point no trusted
 * height exists. See doc/design/b3-era-architecture.md for the
 * decoder-selection problem.
 */
constexpr B3Era GetB3Era(int height, const Params& params)
{
    if (params.legacy_b3coin &&
        (!params.hard_fork_height || height < *params.hard_fork_height)) {
        return B3Era::LEGACY;
    }
    return B3Era::MODERN;
}

/**
 * The three block-production consensus phases of the continuous B3 chain.
 *
 * Distinct from B3Era, which selects the block/transaction FORMAT (codec and
 * hash domain). The MODERN era spans two production phases: the bounded
 * temporary-PoW transition corridor immediately after H, and modern PoS from
 * the first post-corridor height onward.
 *
 *     height <= H                 LEGACY_POS      legacy codec, legacy PoS
 *     H+1 .. H+corridor_length    TRANSITION_POW  modern codec, B3 scrypt PoW
 *     height > H+corridor_length  MODERN_POS      modern codec, modern PoS
 *
 * Chains that never had a legacy B3Coin history are MODERN_POS at every
 * height and never have a corridor. While no boundary is configured on a
 * legacy-B3 chain, every height is LEGACY_POS (live legacy operation).
 */
enum class ConsensusPhase {
    LEGACY_POS,
    TRANSITION_POW,
    MODERN_POS,
};

/**
 * LEGACY_FINAL_HEIGHT (H): the last legacy height, when a boundary is
 * configured on a legacy-B3 chain.
 */
constexpr std::optional<int> LegacyFinalHeight(const Params& params)
{
    if (!params.legacy_b3coin || !params.hard_fork_height) return std::nullopt;
    return *params.hard_fork_height - 1;
}

/**
 * True once the legacy boundary is PINNED: both the final height H and the
 * exact boundary hash X are consensus constants of this chain.
 *
 * From that moment every height <= H is attested history: the node
 * reconstructs it mechanically through the trusted replay engine, and the
 * preserved live legacy rule set (PoW, kernel, difficulty, signatures,
 * historical timestamp semantics, rewards) is never used to re-judge it.
 * While the boundary is unpinned the chain is in live legacy operation and
 * the full preserved rule set applies.
 *
 * Distinct from Chainstate::LegacyBoundaryActive(), which additionally
 * requires X to be connected on the active chain and gates the
 * cross-boundary reorganization prohibition.
 */
constexpr bool LegacyBoundaryPinned(const Params& params)
{
    return LegacyFinalHeight(params).has_value() && params.legacy_final_hash.has_value();
}

/**
 * The final temporary-PoW corridor height (H + transition_pow_length), when
 * a boundary is configured and the chain has a corridor.
 */
constexpr std::optional<int> TransitionPowFinalHeight(const Params& params)
{
    const std::optional<int> final_height{LegacyFinalHeight(params)};
    if (!final_height || params.transition_pow_length <= 0) return std::nullopt;
    return *final_height + params.transition_pow_length;
}

/**
 * Return the block-production consensus phase governing the block at
 * `height`. Single source of truth for phase selection; like GetB3Era it
 * must only be consulted where the height is already unambiguous, and never
 * to choose a wire decoder.
 */
constexpr ConsensusPhase GetConsensusPhase(const int height, const Params& params)
{
    if (GetB3Era(height, params) == B3Era::LEGACY) return ConsensusPhase::LEGACY_POS;
    if (params.legacy_b3coin && params.hard_fork_height &&
        height < *params.hard_fork_height + params.transition_pow_length) {
        return ConsensusPhase::TRANSITION_POW;
    }
    return ConsensusPhase::MODERN_POS;
}

/**
 * A disconnect of the block at `disconnect_height` would cross the
 * finalized legacy boundary. Reorganizations that disconnect H or anything
 * below it are permanently prohibited; forks entirely above H may still
 * reorganize.
 */
constexpr bool DisconnectCrossesLegacyBoundary(const Params& params, const int disconnect_height)
{
    const std::optional<int> final_height{LegacyFinalHeight(params)};
    return final_height.has_value() && disconnect_height <= *final_height;
}

/**
 * Reorganizing the active chain onto a branch that forks at `fork_height`
 * would cross the finalized legacy boundary.
 *
 * Activating such a branch disconnects every block above the fork point, so
 * the lowest block it would disconnect sits at `fork_height + 1`. A branch
 * that forks at or below H - 1 therefore requires disconnecting H or lower
 * and can never become the active chain.
 */
constexpr bool ReorgFromForkCrossesLegacyBoundary(const Params& params, const int fork_height)
{
    return DisconnectCrossesLegacyBoundary(params, fork_height + 1);
}

} // namespace Consensus

#endif // BITCOIN_CONSENSUS_ERA_H
