// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_ASSET_VALIDATION_H
#define B3COIN_MODERN_ASSET_VALIDATION_H

#include <coins.h>
#include <consensus/amount.h>
#include <consensus/era.h>
#include <consensus/params.h>
#include <modern/asset.h>
#include <modern/asset_output.h>
#include <modern/chain_domain.h>
#include <modern/fn.h>
#include <modern/fn_pod.h>
#include <primitives/transaction.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace modern {

//! Owner-ratified simple-v1 issuance charge, paid once in native B3.
inline constexpr CAmount ASSET_ISSUANCE_TREASURY_FEE{1'000 * KILO_COIN};

inline std::optional<AssetId> ConfiguredFnAssetId(const Consensus::Params& params)
{
    if (!params.legacy_final_hash) return std::nullopt;
    const auto domain{ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)};
    if (!domain) return std::nullopt;
    return FnAssetId(*domain);
}

//! View an ordinary native CTxOut through the existing ModernOutput model.
inline ModernOutput ViewNativeOutput(const CTxOut& out)
{
    ModernOutput view;
    view.asset = NativeAsset();
    view.amount = out.nValue;
    view.policy_type = static_cast<uint16_t>(PolicyType::OWNER);
    view.policy_version = POLICY_VERSION_V1;
    view.policy_commitment = LegacyLockCommitment(out.scriptPubKey);
    return view;
}

/**
 * Parse and context-check one CTxOut. Ordinary outputs become native B3;
 * B3A1 claims fail closed unless their exact policy is active at `height`.
 */
inline std::optional<ModernOutput> ViewAssetAwareOutput(const CTxOut& out,
                                                        const int height,
                                                        const Consensus::Params& params,
                                                        std::string& error)
{
    if (!ClaimsAssetOutput(out)) return ViewNativeOutput(out);

    const auto parsed{ParseAssetOutput(out, error)};
    if (!parsed) return std::nullopt;
    const auto fn_asset{ConfiguredFnAssetId(params)};
    const bool fn_id{fn_asset && parsed->asset == *fn_asset};
    const auto policy{static_cast<PolicyType>(parsed->policy_type)};

    switch (policy) {
    case PolicyType::FN:
        if (!Consensus::FnRulesActive(height, params)) {
            error = "FN output is not active";
            return std::nullopt;
        }
        if (!fn_id) {
            error = "FN policy carries the wrong chain-scoped asset id";
            return std::nullopt;
        }
        break;
    case PolicyType::OWNER:
        if (!Consensus::AssetRulesActive(height, params)) {
            error = "colored-asset owner output is not active";
            return std::nullopt;
        }
        if (fn_id) {
            error = "FN asset must use the FN policy";
            return std::nullopt;
        }
        break;
    case PolicyType::BURN:
        if (fn_id) {
            if (!Consensus::FnRulesActive(height, params)) {
                error = "FN extinguishment is not active";
                return std::nullopt;
            }
        } else if (!Consensus::AssetRulesActive(height, params)) {
            error = "colored-asset burn is not active";
            return std::nullopt;
        }
        break;
    case PolicyType::DEX_VAULT:
        // A2 accepts keyless bootstrap deposits so the market can be 30-deep
        // at A3. Spending them still requires an A3 type-9 proof.
        if (!Consensus::FlowMeshVaultPreparationRulesActive(height, params)) {
            error = "DEX vault output is not active";
            return std::nullopt;
        }
        break;
    default:
        // Keep the contextual switch explicit so a future parser expansion
        // cannot activate another policy accidentally.
        error = "policy is not valid in the B3A1 carrier";
        return std::nullopt;
    }

    if (CheckPolicyOutput(*parsed, height, params) != PolicyOutputCheck::OK) {
        error = "asset policy output failed contextual validation";
        return std::nullopt;
    }
    return parsed;
}

/**
 * Interpret a spent coin using the namespace that existed when it was
 * created.  Bytes in the sealed legacy UTXO set can coincidentally begin
 * with B3A1, but they remain ordinary native-B3 outputs forever.  Only
 * coins created after H may opt into the modern asset carrier.
 */
inline std::optional<ModernOutput> ViewAssetAwareCoin(const Coin& coin,
                                                      const int spend_height,
                                                      const Consensus::Params& params,
                                                      std::string& error)
{
    if (const auto H{Consensus::LegacyFinalHeight(params)}; H && coin.nHeight <= *H) {
        return ViewLegacyCoin(coin);
    }
    return ViewAssetAwareOutput(coin.out, spend_height, params, error);
}

inline std::vector<CreationAction> AssetCreationActions(const CTransaction& tx)
{
    std::vector<CreationAction> actions;
    for (const CMpaRecord& record : tx.mpa) {
        if (record.payload_type != CREATION_ACTION_ASSET_ISSUANCE) continue;
        actions.push_back(CreationAction{record.payload_type, record.payload_version,
                                         record.payload});
    }
    return actions;
}

inline bool HasAssetCreationAction(const CTransaction& tx)
{
    return std::any_of(tx.mpa.begin(), tx.mpa.end(), [](const CMpaRecord& record) {
        return record.payload_type == CREATION_ACTION_ASSET_ISSUANCE;
    });
}

struct AssetTransactionContext {
    //! Branch-local number of successful modern FN creations through the
    //! parent block. Required only when this transaction declares FN PoD.
    std::optional<uint32_t> fn_pod_issued_before{};
    //! Native input/output gap already checked by CheckTxInputs.
    CAmount native_input_output_gap{0};
    /**
     * Exact non-native surplus authorized by an independently verified
     * bridge deposit.  The bridge state machine is responsible for proof,
     * recipient, registry, replay and cap checks before populating this
     * field; asset conservation merely enforces that the transaction creates
     * this amount and no other surplus.
     */
    std::optional<AuthorizedAssetMint> bridge_mint{};
};

struct AssetTransactionEffects {
    uint32_t fn_pod_creations{0};
    CAmount fn_pod_disintegration{0};
};

inline bool PaysAssetIssuanceTreasuryFee(const CTransaction& tx,
                                         const Consensus::Params& params)
{
    if (!params.modern_pos || params.modern_pos->treasury_script.empty()) return false;
    const CScript treasury{params.modern_pos->treasury_script.begin(),
                           params.modern_pos->treasury_script.end()};
    CAmount paid{0};
    for (const CTxOut& out : tx.vout) {
        if (out.scriptPubKey != treasury) continue;
        if (out.nValue < 0 || paid > MAX_MONEY - out.nValue) return false;
        paid += out.nValue;
    }
    return paid >= ASSET_ISSUANCE_TREASURY_FEE;
}

/**
 * Production transaction projection and conservation check. `prev_coins`
 * must match tx.vin exactly and is supplied from the active UTXO view before
 * any input is spent. Coinbase is handled by the FN Genesis block rule.
 */
inline bool CheckAssetTransaction(const CTransaction& tx,
                                  const std::vector<Coin>& prev_coins,
                                  const int height,
                                  const Consensus::Params& params,
                                  const AssetTransactionContext& context,
                                  AssetTransactionEffects& effects,
                                  std::string& error)
{
    effects = {};
    if (tx.IsCoinBase()) {
        error = "asset transaction checker does not accept coinbase";
        return false;
    }
    if (prev_coins.size() != tx.vin.size()) {
        error = "asset previous-output view does not match inputs";
        return false;
    }

    const size_t fn_pod_declarations{CountModernFnPodDeclarations(tx)};
    bool has_asset{HasAssetCreationAction(tx) || fn_pod_declarations != 0 ||
                   context.bridge_mint.has_value()};
    std::vector<ModernOutput> prev_outputs;
    std::vector<ModernOutput> outputs;
    std::vector<ModernInput> inputs;
    prev_outputs.reserve(prev_coins.size());
    inputs.reserve(tx.vin.size());
    outputs.reserve(tx.vout.size());

    for (size_t i{0}; i < tx.vin.size(); ++i) {
        std::string view_error;
        auto view{ViewAssetAwareCoin(prev_coins[i], height, params, view_error)};
        if (!view) {
            error = "input " + std::to_string(i) + ": " + view_error;
            return false;
        }
        has_asset = has_asset || view->asset != NativeAsset();
        prev_outputs.push_back(std::move(*view));
        inputs.push_back(ModernInput{tx.vin[i].prevout, tx.vin[i].nSequence,
                                     static_cast<uint32_t>(i)});
    }
    for (size_t i{0}; i < tx.vout.size(); ++i) {
        std::string view_error;
        auto view{ViewAssetAwareOutput(tx.vout[i], height, params, view_error)};
        if (!view) {
            error = "output " + std::to_string(i) + ": " + view_error;
            return false;
        }
        has_asset = has_asset || view->asset != NativeAsset();
        outputs.push_back(std::move(*view));
    }

    if (!has_asset) return true;
    const std::vector<CreationAction> actions{AssetCreationActions(tx)};
    if (context.bridge_mint && !actions.empty()) {
        error = "a transaction cannot combine asset genesis and bridge minting";
        return false;
    }
    if (!actions.empty()) {
        if (!Consensus::AssetRulesActive(height, params)) {
            error = "asset issuance is not active";
            return false;
        }
        if (!PaysAssetIssuanceTreasuryFee(tx, params)) {
            error = "asset issuance does not pay 1,000 B3 to the treasury";
            return false;
        }
    }

    std::optional<AuthorizedAssetMint> authorized_mint{context.bridge_mint};
    if (fn_pod_declarations != 0) {
        if (authorized_mint) {
            error = "a transaction cannot combine FN PoD and bridge minting";
            return false;
        }
        if (fn_pod_declarations != 1) {
            error = "modern FN PoD transaction must have exactly one declaration";
            return false;
        }
        if (!Consensus::FnPodRulesActive(height, params)) {
            error = "modern FN PoD creation is not active";
            return false;
        }
        if (!context.fn_pod_issued_before) {
            error = "modern FN PoD counter is unavailable";
            return false;
        }
        const auto capacity{ModernFnCapacity(params)};
        if (!capacity) {
            error = "historical FN count exceeds the lifetime cap";
            return false;
        }
        if (*context.fn_pod_issued_before >= *capacity) {
            error = "FN lifetime issuance cap is exhausted";
            return false;
        }

        const CMpaRecord* declaration{nullptr};
        for (const CMpaRecord& record : tx.mpa) {
            if (record.payload_type == CREATION_ACTION_MODERN_FN_POD) {
                declaration = &record;
                break;
            }
        }
        if (!declaration) {
            error = "modern FN PoD declaration is missing";
            return false;
        }
        ModernFnPodActionV1 pod_action;
        if (!DecodeModernFnPodRecord(*declaration, pod_action, error)) return false;
        if (pod_action.created_before != *context.fn_pod_issued_before) {
            error = "modern FN PoD declaration names the wrong creation slot";
            return false;
        }
        if (pod_action.output_index >= outputs.size()) {
            error = "modern FN PoD output index is out of range";
            return false;
        }
        const auto fn_asset{ConfiguredFnAssetId(params)};
        if (!fn_asset) {
            error = "FN asset id is unavailable";
            return false;
        }
        const ModernOutput& created{outputs[pod_action.output_index]};
        if (created.asset != *fn_asset || created.amount != 1 ||
            created.policy_type != static_cast<uint16_t>(PolicyType::FN)) {
            error = "modern FN PoD declaration does not name an amount-1 FN owner output";
            return false;
        }
        const CAmount required{RequiredFnPodDisintegration(*context.fn_pod_issued_before)};
        if (context.native_input_output_gap < required) {
            error = "modern FN PoD accounting gap is below the required disintegration";
            return false;
        }
        authorized_mint = AuthorizedAssetMint{*fn_asset, 1};
        effects.fn_pod_creations = 1;
        effects.fn_pod_disintegration = required;
    }

    const AssetCheck check{CheckAssetConservation(prev_outputs, inputs, outputs, actions,
                                                   height, params, authorized_mint)};
    if (check != AssetCheck::OK) {
        error = "asset conservation failure (code " +
                std::to_string(static_cast<int>(check)) + ")";
        return false;
    }
    return true;
}

/** Compatibility helper for callers that do not carry branch-local FN state.
 * It remains fully useful for transfers and colored-asset genesis; a modern
 * FN PoD declaration fails closed because its counter is intentionally absent.
 */
inline bool CheckAssetTransaction(const CTransaction& tx,
                                  const std::vector<Coin>& prev_coins,
                                  const int height,
                                  const Consensus::Params& params,
                                  std::string& error)
{
    CAmount input_value{0};
    for (const Coin& coin : prev_coins) {
        if (coin.out.nValue < 0 || coin.out.nValue > MAX_MONEY - input_value) {
            error = "native input amount is out of range";
            return false;
        }
        input_value += coin.out.nValue;
    }
    const CAmount output_value{tx.GetValueOut()};
    if (output_value > input_value) {
        error = "native output amount exceeds inputs";
        return false;
    }
    AssetTransactionEffects ignored;
    return CheckAssetTransaction(tx, prev_coins, height, params,
                                 AssetTransactionContext{std::nullopt,
                                                         input_value - output_value,
                                                         std::nullopt},
                                 ignored, error);
}

} // namespace modern

#endif // B3COIN_MODERN_ASSET_VALIDATION_H
