// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_STAKE_REGISTRY_H
#define B3COIN_NODE_STAKE_REGISTRY_H

#include <consensus/amount.h>
#include <consensus/params.h>
#include <modern/stake.h>
#include <node/utxo_commitment.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <map>
#include <vector>

class CCoinsView;

namespace node {

using ValidatorKey = std::array<unsigned char, modern::STAKE_VALIDATOR_KEY_SIZE>;

/**
 * One qualifying STAKE output attributed to its validator: full identity and
 * enough information for the later lifecycle — activation (creation height
 * plus state at the evaluation height), exits and cutoff classification
 * (outpoint), and future attribution needs (owner commitment = SHA256 of the
 * owner script suffix, the OWNER binding scheme). No slashing, penalties or
 * finality semantics exist here — only the information they would need.
 */
struct StakeOutputRecord {
    COutPoint outpoint{};
    CAmount amount{0};
    uint256 owner_commitment{};
    int creation_height{0};
    //! Activation state at the evaluation height per STAKE_ACTIVATION_DEPTH:
    //! true = ACTIVE (contributes weight), false = PENDING (attributed,
    //! weightless).
    bool active{false};
};

/**
 * Everything the registry knows about one validator identity: the opaque
 * 32-byte key, per-output attribution, and the aggregated weight — the SUM
 * of ACTIVE principal, per the locked per-validator aggregation rule.
 * Aggregation is a derived view over the records, never the stored form.
 */
struct ValidatorRecord {
    ValidatorKey key{};
    std::vector<StakeOutputRecord> outputs;
    CAmount total_weight{0};
};

/**
 * The derived validator registry at an evaluation height. Derived state,
 * recomputable from the UTXO set (hence reorg-safe by recomputation);
 * modern PoS will consume it, nothing consults it during the corridor.
 * A validator whose outputs are all PENDING appears with zero weight —
 * attribution is retained either way.
 */
struct StakeRegistry {
    std::map<ValidatorKey, ValidatorRecord> validators;
    size_t mature_outputs{0};
    size_t immature_outputs{0};
    CAmount total_weight{0};

    //! The aggregation view: validator key -> ACTIVE weight (zero-weight
    //! validators included).
    std::map<ValidatorKey, CAmount> WeightView() const
    {
        std::map<ValidatorKey, CAmount> view;
        for (const auto& [key, record] : validators) view.emplace(key, record.total_weight);
        return view;
    }
};

/**
 * Derive the registry from enumerated UTXO entries at `current_height`.
 * Qualification: the coin is a MODERN-era output (created above the final
 * legacy height H — a pre-H script that happens to match the STAKE shape is
 * an ordinary legacy output, never stake) and parses as a valid v1 STAKE
 * output. ACTIVE vs PENDING follows STAKE_ACTIVATION_DEPTH at the
 * evaluation height.
 */
StakeRegistry DeriveStakeRegistry(const std::vector<UtxoEntry>& entries, int current_height,
                                  const Consensus::Params& params);

//! Convenience overload over a cursor-supporting view (e.g. CCoinsViewDB).
StakeRegistry DeriveStakeRegistry(const CCoinsView& view, int current_height,
                                  const Consensus::Params& params);

} // namespace node

#endif // B3COIN_NODE_STAKE_REGISTRY_H
