// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_CONSENSUS_HARDFORK_H
#define BITCOIN_CONSENSUS_HARDFORK_H

#include <consensus/era.h>
#include <consensus/params.h>

namespace Consensus {

/**
 * True once the block at `height` is governed by post-fork (MODERN)
 * consensus. Thin convenience wrapper over the central era selector in
 * consensus/era.h; new code should call GetB3Era() directly.
 */
constexpr bool IsHardForkActive(const Params& params, int height)
{
    return GetB3Era(height, params) == B3Era::MODERN;
}

} // namespace Consensus

#endif // BITCOIN_CONSENSUS_HARDFORK_H
