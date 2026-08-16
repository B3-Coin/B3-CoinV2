// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/stake_registry.h>

#include <consensus/era.h>
#include <crypto/sha256.h>

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

        StakeOutputRecord record;
        record.outpoint = entry.outpoint;
        record.amount = view->amount;
        record.creation_height = static_cast<int>(entry.coin.nHeight);
        record.active = modern::IsStakeMature(record.creation_height, current_height);
        CSHA256()
            .Write(view->owner_script.data(), view->owner_script.size())
            .Finalize(record.owner_commitment.begin());

        ValidatorRecord& validator{registry.validators[view->validator_key]};
        validator.key = view->validator_key;
        if (record.active) {
            ++registry.mature_outputs;
            validator.total_weight += record.amount;
            registry.total_weight += record.amount;
        } else {
            ++registry.immature_outputs;
        }
        validator.outputs.push_back(std::move(record));
    }
    return registry;
}

StakeRegistry DeriveStakeRegistry(const CCoinsView& view, const int current_height,
                                  const Consensus::Params& params)
{
    return DeriveStakeRegistry(EnumerateUtxos(view), current_height, params);
}

} // namespace node
