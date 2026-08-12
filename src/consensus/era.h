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
 * LEGACY_FINAL_HEIGHT (H): the last legacy height, when a boundary is
 * configured on a legacy-B3 chain.
 */
constexpr std::optional<int> LegacyFinalHeight(const Params& params)
{
    if (!params.legacy_b3coin || !params.hard_fork_height) return std::nullopt;
    return *params.hard_fork_height - 1;
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
