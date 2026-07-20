// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_CONSENSUS_HARDFORK_H
#define BITCOIN_CONSENSUS_HARDFORK_H

#include <consensus/params.h>

namespace Consensus {

enum class Era {
    LEGACY,
    POST_HARD_FORK,
};

/** Return the consensus era governing a block at height. */
constexpr Era GetEra(const Params& params, int height)
{
    if (height >= 0 && params.hard_fork_height && height >= *params.hard_fork_height) {
        return Era::POST_HARD_FORK;
    }
    return Era::LEGACY;
}

constexpr bool IsHardForkActive(const Params& params, int height)
{
    return GetEra(params, height) == Era::POST_HARD_FORK;
}

} // namespace Consensus

#endif // BITCOIN_CONSENSUS_HARDFORK_H
