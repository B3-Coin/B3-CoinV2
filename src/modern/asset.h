// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_ASSET_H
#define B3COIN_MODERN_ASSET_H

#include <consensus/amount.h>
#include <consensus/params.h>
#include <hash.h>
#include <modern/policy.h>
#include <modern/proof.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <map>
#include <string>
#include <vector>

namespace modern {

/**
 * Native coloured-asset conservation (modern era, test-only activation).
 *
 * Issuance identity is deterministic and structural: the AssetId of a new
 * asset is the tagged hash of the FIRST input's prevout of its issuance
 * transition. An outpoint can be spent exactly once, so an asset can be
 * minted exactly once — fixed supply and the impossibility of reissuance
 * are properties of the id itself, not registry state. Reissuance, bridge
 * minting and PoW minting do not exist at this stage.
 *
 * Conservation is exact per asset:
 *  - native B3 may run a deficit — that deficit is the fee, and fees are
 *    payable in native B3 only; native surplus is invalid (native minting
 *    is the consensus reward domain, never asset issuance);
 *  - every other asset must satisfy inputs == live outputs + explicit
 *    burns, to the unit. Losing value implicitly is invalid; destroying
 *    value happens only through visible BURN policy outputs;
 *  - a surplus in exactly the issuance asset of this transition is the
 *    mint; any other surplus is unauthorized.
 */

//! Deterministic issuance identity of the asset defined by spending
//! `defining_prevout` as the first input of its issuance transition.
inline AssetId IssuanceAssetId(const COutPoint& defining_prevout)
{
    HashWriter h;
    h << std::string{"b3/asset/v1"} << defining_prevout;
    return h.GetHash();
}

enum class AssetCheck {
    OK,
    NOT_ACTIVE,
    STRUCTURE_INVALID,
    POLICY_INVALID,
    AMOUNT_OVERFLOW,
    UNAUTHORIZED_MINT,
    CONSERVATION_MISMATCH,
};

/**
 * Check exact per-asset conservation of a transition at a connected
 * modern height. `prev_outputs[i]` is the coin spent by `inputs[i]`.
 * Fail-closed: NOT_ACTIVE until the asset rules are activated (test-only
 * for now).
 */
inline AssetCheck CheckAssetConservation(const std::vector<ModernOutput>& prev_outputs,
                                         const ModernTransition& t, const int height,
                                         const Consensus::Params& params)
{
    if (!params.test_only_asset_policies_active) return AssetCheck::NOT_ACTIVE;
    if (t.inputs.empty() || prev_outputs.size() != t.inputs.size()) {
        return AssetCheck::STRUCTURE_INVALID;
    }

    // Overflow-safe per-asset accumulation. Every individual amount has
    // already been bounded to [0, MAX_MONEY] by CheckPolicyOutput; sums are
    // guarded against exceeding MAX_MONEY.
    const auto accumulate{[](std::map<AssetId, CAmount>& sums, const AssetId& asset,
                             const CAmount amount) {
        CAmount& sum{sums[asset]};
        if (amount < 0 || amount > MAX_MONEY - sum) return false;
        sum += amount;
        return true;
    }};

    std::map<AssetId, CAmount> in_sums;
    for (const ModernOutput& prev : prev_outputs) {
        if (CheckPolicyOutput(prev, height, params) != PolicyOutputCheck::OK) {
            return AssetCheck::POLICY_INVALID;
        }
        if (!accumulate(in_sums, prev.asset, prev.amount)) return AssetCheck::AMOUNT_OVERFLOW;
    }

    std::map<AssetId, CAmount> live_sums;
    std::map<AssetId, CAmount> burn_sums;
    for (const ModernOutput& out : t.outputs) {
        if (CheckPolicyOutput(out, height, params) != PolicyOutputCheck::OK) {
            return AssetCheck::POLICY_INVALID;
        }
        auto& sums{out.policy_type == static_cast<uint16_t>(PolicyType::BURN) ? burn_sums
                                                                              : live_sums};
        if (!accumulate(sums, out.asset, out.amount)) return AssetCheck::AMOUNT_OVERFLOW;
    }

    // The only asset this transition may mint: the one defined by its
    // first input. Fixed supply is whatever this single transition mints.
    const AssetId issuance_id{IssuanceAssetId(t.inputs[0].prevout)};

    std::map<AssetId, CAmount> all_assets;
    for (const auto& [asset, sum] : in_sums) all_assets.try_emplace(asset, 0);
    for (const auto& [asset, sum] : live_sums) all_assets.try_emplace(asset, 0);
    for (const auto& [asset, sum] : burn_sums) all_assets.try_emplace(asset, 0);

    for (const auto& [asset, unused] : all_assets) {
        const CAmount in{in_sums.count(asset) ? in_sums.at(asset) : 0};
        const CAmount live{live_sums.count(asset) ? live_sums.at(asset) : 0};
        const CAmount burned{burn_sums.count(asset) ? burn_sums.at(asset) : 0};
        if (live > MAX_MONEY - burned) return AssetCheck::AMOUNT_OVERFLOW;
        const CAmount out_total{live + burned};

        if (asset == NativeAsset()) {
            // Deficit is the fee — native B3 only. Surplus can never be
            // asset issuance.
            if (out_total > in) return AssetCheck::CONSERVATION_MISMATCH;
            continue;
        }
        if (out_total == in) continue; // exact transfer/burn conservation
        if (out_total > in) {
            // A mint: authorized only for this transition's issuance asset.
            if (asset != issuance_id) return AssetCheck::UNAUTHORIZED_MINT;
            continue;
        }
        // out_total < in: value vanished without an explicit burn.
        return AssetCheck::CONSERVATION_MISMATCH;
    }
    return AssetCheck::OK;
}

} // namespace modern

#endif // B3COIN_MODERN_ASSET_H
