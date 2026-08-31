// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_FN_GENESIS_VALIDATION_H
#define B3COIN_MODERN_FN_GENESIS_VALIDATION_H

#include <consensus/era.h>
#include <consensus/params.h>
#include <modern/asset_output.h>
#include <modern/asset_validation.h>
#include <modern/chain_domain.h>
#include <modern/fn.h>
#include <modern/fn_genesis.h>
#include <primitives/block.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace modern {

inline bool HasFnGenesisConfigurationIntent(const Consensus::Params& params)
{
    return params.fn_genesis_rights_root.has_value() ||
           !params.fn_genesis_manifest.empty();
}

/**
 * Validate the transition release's self-contained historical rights pin.
 * The root is not trusted as a substitute for the list: the embedded rows
 * are recomputed and must match it byte-for-byte under the pinned chain
 * domain and the mandatory first-corridor height H+1.
 */
inline bool CheckFnGenesisConfiguration(const Consensus::Params& params,
                                        std::string& error)
{
    if (!HasFnGenesisConfigurationIntent(params)) {
        error = "FN Genesis is not configured";
        return false;
    }
    if (!Consensus::FnGenesisConfigured(params)) {
        error = "FN Genesis configuration is incomplete";
        return false;
    }
    if (params.fn_genesis_manifest.size() <
        Consensus::HISTORICAL_FN_PROVEN_FLOOR) {
        error = "FN Genesis manifest is below the proven historical floor";
        return false;
    }
    if (params.fn_genesis_manifest.size() > Consensus::MAX_FN_EVER_ISSUED) {
        error = "FN Genesis manifest exceeds the lifetime cap";
        return false;
    }
    if (params.transition_pow_length <= 0) {
        error = "FN Genesis requires the transition corridor";
        return false;
    }
    if (params.fn_genesis_manifest_version != FN_GENESIS_MANIFEST_VERSION_V1) {
        error = "unsupported FN Genesis manifest version";
        return false;
    }
    if (*params.hard_fork_height < 1) {
        error = "invalid FN Genesis height";
        return false;
    }

    const auto domain{ModernChainDomain(params.hashGenesisBlock,
                                         *params.legacy_final_hash)};
    if (!domain) {
        error = "FN Genesis chain domain is not pinned";
        return false;
    }
    const auto root{ComputeFnGenesisManifestRootV1(
        *domain, static_cast<uint32_t>(*params.hard_fork_height),
        params.fn_genesis_manifest, &error)};
    if (!root) return false;
    if (*root != *params.fn_genesis_rights_root) {
        error = "FN Genesis manifest does not match the pinned rights root";
        return false;
    }
    error.clear();
    return true;
}

inline std::optional<std::vector<CTxOut>> ExpectedFnGenesisOutputs(
    const Consensus::Params& params,
    std::string& error)
{
    if (!CheckFnGenesisConfiguration(params, error)) return std::nullopt;
    const auto fn_asset{ConfiguredFnAssetId(params)};
    if (!fn_asset) {
        error = "FN Genesis asset id is unavailable";
        return std::nullopt;
    }

    std::vector<CTxOut> outputs;
    outputs.reserve(params.fn_genesis_manifest.size());
    for (const Consensus::FnGenesisRight& right : params.fn_genesis_manifest) {
        const auto out{MakeAssetOwnerOutput(*fn_asset, 1, PolicyType::FN,
                                            FnGenesisRecipientScript(right))};
        if (!out) {
            error = "FN Genesis row cannot be encoded as an output";
            return std::nullopt;
        }
        outputs.push_back(*out);
    }
    error.clear();
    return outputs;
}

/**
 * Mandatory one-time block event. When configured, the first corridor
 * coinbase contains exactly one unit per manifest row, in manifest order.
 * Native fee payout outputs may coexist; no asset output may appear in any
 * other coinbase at any height.
 */
inline bool CheckFnGenesisBlock(const CBlock& block,
                                const int height,
                                const Consensus::Params& params,
                                std::string& error)
{
    if (block.vtx.empty() || !block.vtx.front()->IsCoinBase()) {
        error = "block has no coinbase";
        return false;
    }
    if (HasAssetCreationAction(*block.vtx.front()) ||
        HasModernFnPodDeclaration(*block.vtx.front())) {
        error = "user asset/FN creation declaration is forbidden in coinbase";
        return false;
    }

    const bool configured{HasFnGenesisConfigurationIntent(params)};
    const bool required{params.legacy_b3coin && params.fn_genesis_required};
    const bool genesis_height{(configured || required) && params.hard_fork_height &&
                              height == *params.hard_fork_height};
    std::vector<CTxOut> carried;
    for (const CTxOut& out : block.vtx.front()->vout) {
        if (!ClaimsAssetOutput(out)) continue;
        std::string parse_error;
        const auto parsed{ParseAssetOutput(out, parse_error)};
        if (!parsed) {
            error = "malformed asset claim in coinbase: " + parse_error;
            return false;
        }
        if (!genesis_height) {
            error = "asset output outside the FN Genesis coinbase";
            return false;
        }
        carried.push_back(out);
    }

    if (!genesis_height) {
        error.clear();
        return true;
    }
    const auto expected{ExpectedFnGenesisOutputs(params, error)};
    if (!expected) return false;
    if (carried != *expected) {
        error = "FN Genesis coinbase outputs do not match the pinned manifest";
        return false;
    }
    error.clear();
    return true;
}

} // namespace modern

#endif // B3COIN_MODERN_FN_GENESIS_VALIDATION_H
