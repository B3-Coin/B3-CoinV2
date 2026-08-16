// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/stake_registry.h>

#include <consensus/era.h>

#include <string>

namespace node {

StakeRegistry DeriveStakeRegistry(const std::vector<UtxoEntry>& entries, const int current_height,
                                  const Consensus::Params& params)
{
    StakeRegistry registry;
    const std::optional<int> final_height{Consensus::LegacyFinalHeight(params)};
    for (const UtxoEntry& entry : entries) {
        // Modern-era outputs only: a pre-H (or boundary-less chain) output
        // is never stake regardless of its script shape.
        if (!final_height || static_cast<int>(entry.coin.nHeight) <= *final_height) continue;
        if (!modern::ClaimsStakeMagic(entry.coin.out.scriptPubKey)) continue;
        std::string error;
        const auto view{modern::ParseStakeOutput(entry.coin.out, error)};
        // A connected chain cannot contain an invalid claiming output
        // (ContextualCheckBlock enforces it); stay total anyway.
        if (!view) continue;
        if (!modern::IsStakeMature(static_cast<int>(entry.coin.nHeight), current_height)) {
            ++registry.immature_outputs;
            continue;
        }
        ++registry.mature_outputs;
        registry.weights[view->validator_key] += view->amount;
        registry.total_weight += view->amount;
    }
    return registry;
}

StakeRegistry DeriveStakeRegistry(const CCoinsView& view, const int current_height,
                                  const Consensus::Params& params)
{
    return DeriveStakeRegistry(EnumerateUtxos(view), current_height, params);
}

} // namespace node
