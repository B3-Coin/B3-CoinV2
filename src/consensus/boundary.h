// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_CONSENSUS_BOUNDARY_H
#define B3COIN_CONSENSUS_BOUNDARY_H

#include <consensus/era.h>
#include <consensus/params.h>
#include <primitives/block.h>

namespace Consensus {

enum class BoundaryCheck {
    OK,
    //! The block at LEGACY_FINAL_HEIGHT does not hash to LEGACY_FINAL_HASH.
    WRONG_FINAL_HASH,
    //! The block at LEGACY_FINAL_HEIGHT + 1 does not reference
    //! LEGACY_FINAL_HASH as its parent.
    WRONG_FINAL_PARENT,
};

/**
 * Enforce the finalized legacy boundary identity on a header at a known
 * height: the exact hash X is required at H, and the first modern block at
 * H + 1 must reference X. Heights away from the boundary — or a chain with
 * no finalized boundary (hash unset) — are unconstrained here; codec
 * legality per era is enforced separately by HasExpectedB3BlockCodec.
 */
inline BoundaryCheck CheckLegacyBoundaryHeader(const CBlockHeader& header, const int height,
                                               const Params& params)
{
    const std::optional<int> final_height{LegacyFinalHeight(params)};
    if (!final_height || !params.legacy_final_hash) return BoundaryCheck::OK;

    if (height == *final_height && header.GetMarkerHash(params) != *params.legacy_final_hash) {
        return BoundaryCheck::WRONG_FINAL_HASH;
    }
    if (height == *final_height + 1 && header.hashPrevBlock != *params.legacy_final_hash) {
        return BoundaryCheck::WRONG_FINAL_PARENT;
    }
    return BoundaryCheck::OK;
}

/**
 * Return whether a post-legacy block has the exact identity pinned for its
 * height. This table is intentionally separate from legacy checkpoints: it
 * uses the modern block identity supplied by the caller and never applies to
 * an attested legacy height.
 */
inline bool ModernCheckpointAllows(const Params& params, const int height,
                                   const uint256& block_hash)
{
    if (GetB3Era(height, params) != B3Era::MODERN) return true;

    const auto checkpoint{params.modern_checkpoints.find(height)};
    return checkpoint == params.modern_checkpoints.end() ||
           checkpoint->second == block_hash;
}

} // namespace Consensus

#endif // B3COIN_CONSENSUS_BOUNDARY_H
