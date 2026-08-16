// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_STAKE_REGISTRY_H
#define B3COIN_NODE_STAKE_REGISTRY_H

#include <consensus/amount.h>
#include <consensus/params.h>
#include <modern/stake.h>
#include <node/utxo_commitment.h>

#include <array>
#include <cstddef>
#include <map>
#include <vector>

class CCoinsView;

namespace node {

using ValidatorKey = std::array<unsigned char, modern::STAKE_VALIDATOR_KEY_SIZE>;

/**
 * The derived validator registry: consensus weight aggregated per validator
 * key over every MATURE modern-era STAKE output. The aggregation rule is
 * LOCKED design: one validator identity has exactly the weight of the SUM of
 * its qualifying principal, however it is split across outputs — never one
 * lottery ticket per UTXO. Derived state, recomputable from the UTXO set;
 * modern PoS will consume it, nothing consults it during the corridor.
 */
struct StakeRegistry {
    std::map<ValidatorKey, CAmount> weights;
    size_t mature_outputs{0};
    size_t immature_outputs{0};
    CAmount total_weight{0};
};

/**
 * Derive the registry from enumerated UTXO entries at `current_height`.
 * Qualification: the coin is a MODERN-era output (created above the final
 * legacy height H — a pre-H script that happens to match the STAKE shape is
 * an ordinary legacy output, never stake), parses as a valid v1 STAKE
 * output, and is mature per STAKE_ACTIVATION_DEPTH. Immature qualifying
 * outputs are counted but carry no weight.
 */
StakeRegistry DeriveStakeRegistry(const std::vector<UtxoEntry>& entries, int current_height,
                                  const Consensus::Params& params);

//! Convenience overload over a cursor-supporting view (e.g. CCoinsViewDB).
StakeRegistry DeriveStakeRegistry(const CCoinsView& view, int current_height,
                                  const Consensus::Params& params);

} // namespace node

#endif // B3COIN_NODE_STAKE_REGISTRY_H
