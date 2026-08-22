// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_ASSET_H
#define B3COIN_MODERN_ASSET_H

#include <consensus/amount.h>
#include <consensus/params.h>
#include <hash.h>
#include <modern/chain_domain.h>
#include <modern/creation_action.h>
#include <modern/policy.h>
#include <modern/proof.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <streams.h>
#include <uint256.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace modern {

/**
 * Native coloured-asset model — simple v1 (owner rulings 2026-08-22;
 * test-only activation until the asset activation height).
 *
 *     issue once  →  AssetId  →  immutable genesis record  →  ordinary
 *     outputs carry only AssetId + amount + per-output Policy
 *
 * GENESIS RECORD (the asset-wide rules, stated ONCE, in the issuance
 * transaction's creation action, never repeated on outputs):
 *   - max_supply    : the HARD CAP of units that can ever exist, in every
 *                     issuance mode; in v1's only mode the genesis mints
 *                     exactly this, in one transaction;
 *   - decimals      : display contract, committed for immutability;
 *                     consensus only bounds it (units stay integers);
 *   - issuance_mode : HOW supply comes into existence. v1 accepts only
 *                     GENESIS_FIXED (0): everything minted at genesis, no
 *                     later mint can ever exist, so the cap is enforced by
 *                     construction (inputs == outputs + burns forever
 *                     after) with no registry and no counters.
 *   - mode_params   : the mode's own parameters, bounded; EMPTY in v1.
 *                     ROOM FOR EXPANSION (owner direction 2026-08-22): a
 *                     future POW_MINED mode (PoW-minable colored assets)
 *                     carries its mining schedule here, AUTHORITY_MINT a
 *                     key, BRIDGE_BACKED its origin binding — all inside
 *                     this same record layout and the same AssetId
 *                     preimage, activated by a later rule set that
 *                     recognizes the mode; v1 rejects every non-zero mode.
 *
 * ASSET IDENTITY — unified with the FN asset id convention (genuine
 * tagged hash, chain-bound, parameter-bound):
 *
 *     AssetId = TaggedHash("B3/ASSET/V1")
 *               || ModernChainDomain || issuance_outpoint || H(genesis record)
 *
 * The outpoint is the transition's FIRST input (spendable exactly once,
 * so an asset is minted exactly once); the chain domain makes the same
 * issuance on any other chain or fork a different asset; the genesis
 * commitment makes the rules part of the identity — nobody can ever
 * reissue "the same asset" with different supply or decimals. This
 * supersedes the earlier untagged, domain-free, rule-free derivation
 * (which never left the test-only layer).
 *
 * CONSERVATION is exact per asset:
 *  - native B3 may run a deficit — that deficit is the fee, and fees are
 *    payable in native B3 only; native surplus is invalid (native minting
 *    is the consensus reward domain, never asset issuance);
 *  - every other asset must satisfy inputs == live outputs + explicit
 *    burns, to the unit. Losing value implicitly is invalid; destroying
 *    value happens only through visible BURN policy outputs;
 *  - the ONLY permitted surplus is the genesis mint: a transition carrying
 *    exactly one valid asset-issuance action may — and must — mint
 *    exactly the declared max_supply of exactly the id derived from its
 *    own first input and that record. Everything else is an unauthorized
 *    mint. Transitions without the action (including every v1-envelope
 *    transition) can mint nothing.
 *
 * Reissuance, bridge minting, PoW minting, and programmable schemes do
 * not exist at this stage (V2 research).
 */

//! Issuance modes. Only GENESIS_FIXED is accepted in v1; the others are
//! RESERVED numbers (append-only) so future modes need no format change.
inline constexpr uint8_t ASSET_ISSUANCE_MODE_GENESIS_FIXED{0};
inline constexpr uint8_t ASSET_ISSUANCE_MODE_AUTHORITY_MINT{1}; // RESERVED (V2)
inline constexpr uint8_t ASSET_ISSUANCE_MODE_POW_MINED{2};      // RESERVED (V2): PoW-minable assets
inline constexpr uint8_t ASSET_ISSUANCE_MODE_BRIDGE_BACKED{3};  // RESERVED (V2)
inline constexpr uint8_t ASSET_MAX_DECIMALS{18};
//! Bound on a mode's parameter blob (enforced before allocation); v1
//! requires it empty, future modes get up to this much room.
inline constexpr size_t MAX_ASSET_MODE_PARAMS{256};
//! Serialized size of a v1 record (empty mode_params): 8 + 1 + 1 + 1.
inline constexpr size_t ASSET_GENESIS_V1_SIZE{11};

//! Bounded, immutable asset-wide rules, fixed at issuance.
struct AssetGenesisV1 {
    uint64_t max_supply{0};
    uint8_t decimals{0};
    uint8_t issuance_mode{ASSET_ISSUANCE_MODE_GENESIS_FIXED};
    std::vector<unsigned char> mode_params{};

    SERIALIZE_METHODS(AssetGenesisV1, obj)
    {
        READWRITE(obj.max_supply, obj.decimals, obj.issuance_mode, obj.mode_params);
    }
    friend bool operator==(const AssetGenesisV1& a, const AssetGenesisV1& b)
    {
        return a.max_supply == b.max_supply && a.decimals == b.decimals &&
               a.issuance_mode == b.issuance_mode && a.mode_params == b.mode_params;
    }
};

//! v1 validity: supply in [1, MAX_MONEY], decimals bounded, GENESIS_FIXED
//! with no mode parameters. Any reserved mode is refused here until the
//! rule set that defines it exists.
inline bool AssetGenesisValid(const AssetGenesisV1& g)
{
    return g.max_supply >= 1 && g.max_supply <= static_cast<uint64_t>(MAX_MONEY) &&
           g.decimals <= ASSET_MAX_DECIMALS &&
           g.issuance_mode == ASSET_ISSUANCE_MODE_GENESIS_FIXED && g.mode_params.empty();
}

//! The commitment to the genesis record that the AssetId binds.
inline uint256 AssetGenesisCommitment(const AssetGenesisV1& g)
{
    HashWriter writer{TaggedHash("B3/ASSET/GENESIS/V1")};
    writer << g;
    return writer.GetSHA256();
}

/**
 * The chain-scoped, rule-bound asset identity. Pure function of its
 * inputs; the caller supplies a PINNED chain domain (ModernChainDomain
 * fails closed while X is unset, so no asset id can exist before H/X).
 */
inline AssetId AssetIdV1(const uint256& chain_domain, const COutPoint& issuance_outpoint,
                         const uint256& genesis_commitment)
{
    HashWriter writer{TaggedHash("B3/ASSET/V1")};
    writer << chain_domain << issuance_outpoint << genesis_commitment;
    return writer.GetSHA256();
}

//! Build the issuance creation action (type 3, v1) carrying the record.
inline CreationAction MakeAssetIssuanceAction(const AssetGenesisV1& g)
{
    DataStream ds;
    ds << g;
    CreationAction action;
    action.action_type = CREATION_ACTION_ASSET_ISSUANCE;
    action.action_version = ASSET_ISSUANCE_ACTION_VERSION_V1;
    action.payload.assign(UCharCast(ds.data()), UCharCast(ds.data()) + ds.size());
    return action;
}

//! Strict decode: the record and nothing more (bounded before allocation,
//! full consumption required). Mode validity is AssetGenesisValid's job.
inline bool DecodeAssetIssuanceAction(const CreationAction& action, AssetGenesisV1& out,
                                      std::string& error)
{
    if (action.action_type != CREATION_ACTION_ASSET_ISSUANCE ||
        action.action_version != ASSET_ISSUANCE_ACTION_VERSION_V1) {
        error = "not an asset issuance action";
        return false;
    }
    // 8 + 1 + 1 + compact-size(<= 3 bytes for the bounded blob) + blob.
    if (action.payload.size() < ASSET_GENESIS_V1_SIZE ||
        action.payload.size() > ASSET_GENESIS_V1_SIZE + 2 + MAX_ASSET_MODE_PARAMS) {
        error = "asset genesis record has the wrong size";
        return false;
    }
    try {
        SpanReader reader{action.payload};
        // Field by field: the blob's length is bounded BEFORE any
        // allocation (the generic vector deserializer would otherwise
        // reserve a multi-megabyte chunk for a claimed huge length).
        reader >> out.max_supply >> out.decimals >> out.issuance_mode;
        const uint64_t params_size{ReadCompactSize(reader, /*range_check=*/true)};
        if (params_size > MAX_ASSET_MODE_PARAMS) {
            error = "asset genesis mode parameters exceed the bound";
            return false;
        }
        if (params_size > reader.size()) {
            error = "asset genesis record is malformed";
            return false;
        }
        out.mode_params.resize(params_size);
        if (params_size > 0) reader.read(MakeWritableByteSpan(out.mode_params));
        if (!reader.empty()) {
            error = "trailing bytes after the asset genesis record";
            return false;
        }
    } catch (const std::exception&) {
        error = "asset genesis record is malformed";
        return false;
    }
    return true;
}

enum class AssetCheck {
    OK,
    NOT_ACTIVE,
    STRUCTURE_INVALID,
    POLICY_INVALID,
    AMOUNT_OVERFLOW,
    UNAUTHORIZED_MINT,
    CONSERVATION_MISMATCH,
    //! An issuance action that is malformed, duplicated, out of bounds,
    //! unpinned-chain, or not honored by exactly its declared mint.
    ISSUANCE_INVALID,
};

/**
 * Core conservation check over explicit inputs/outputs/actions.
 * `prev_outputs[i]` is the coin spent by `inputs[i]`. Fail-closed:
 * NOT_ACTIVE until the asset rules are activated (test-only for now).
 */
inline AssetCheck CheckAssetConservation(const std::vector<ModernOutput>& prev_outputs,
                                         const std::vector<ModernInput>& inputs,
                                         const std::vector<ModernOutput>& outputs,
                                         const std::vector<CreationAction>& actions,
                                         const int height, const Consensus::Params& params)
{
    if (!params.test_only_asset_policies_active) return AssetCheck::NOT_ACTIVE;
    if (inputs.empty() || prev_outputs.size() != inputs.size()) {
        return AssetCheck::STRUCTURE_INVALID;
    }

    // At most one genesis per transition; decode it strictly and bind the
    // asset id to this chain, this first input, and this record.
    std::optional<AssetId> issuance_id;
    uint64_t declared_supply{0};
    for (const CreationAction& action : actions) {
        if (action.action_type != CREATION_ACTION_ASSET_ISSUANCE) continue;
        if (issuance_id) return AssetCheck::ISSUANCE_INVALID;
        AssetGenesisV1 genesis;
        std::string error;
        if (!DecodeAssetIssuanceAction(action, genesis, error) || !AssetGenesisValid(genesis)) {
            return AssetCheck::ISSUANCE_INVALID;
        }
        const std::optional<uint256> domain{ModernChainDomain(
            params.hashGenesisBlock, params.legacy_final_hash.value_or(uint256{}))};
        if (!domain) return AssetCheck::ISSUANCE_INVALID; // no pinned chain, no asset
        issuance_id = AssetIdV1(*domain, inputs[0].prevout, AssetGenesisCommitment(genesis));
        if (*issuance_id == NativeAsset()) return AssetCheck::ISSUANCE_INVALID;
        declared_supply = genesis.max_supply;
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
    for (const ModernOutput& out : outputs) {
        if (CheckPolicyOutput(out, height, params) != PolicyOutputCheck::OK) {
            return AssetCheck::POLICY_INVALID;
        }
        auto& sums{out.policy_type == static_cast<uint16_t>(PolicyType::BURN) ? burn_sums
                                                                              : live_sums};
        if (!accumulate(sums, out.asset, out.amount)) return AssetCheck::AMOUNT_OVERFLOW;
    }

    std::map<AssetId, CAmount> all_assets;
    for (const auto& [asset, sum] : in_sums) all_assets.try_emplace(asset, 0);
    for (const auto& [asset, sum] : live_sums) all_assets.try_emplace(asset, 0);
    for (const auto& [asset, sum] : burn_sums) all_assets.try_emplace(asset, 0);

    bool genesis_minted{false};
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
            // A mint: authorized only as THIS transition's declared genesis,
            // and only for exactly the declared supply.
            if (!issuance_id || asset != *issuance_id) return AssetCheck::UNAUTHORIZED_MINT;
            if (in != 0 || static_cast<uint64_t>(out_total) != declared_supply) {
                return AssetCheck::ISSUANCE_INVALID;
            }
            genesis_minted = true;
            continue;
        }
        // out_total < in: value vanished without an explicit burn.
        return AssetCheck::CONSERVATION_MISMATCH;
    }
    // A declared genesis must actually mint its supply.
    if (issuance_id && !genesis_minted) return AssetCheck::ISSUANCE_INVALID;
    return AssetCheck::OK;
}

//! v1-envelope transitions carry no creation actions: they can transfer
//! and burn but never issue.
inline AssetCheck CheckAssetConservation(const std::vector<ModernOutput>& prev_outputs,
                                         const ModernTransition& t, const int height,
                                         const Consensus::Params& params)
{
    return CheckAssetConservation(prev_outputs, t.inputs, t.outputs, {}, height, params);
}

//! v2-envelope transitions may carry the genesis action.
inline AssetCheck CheckAssetConservation(const std::vector<ModernOutput>& prev_outputs,
                                         const ModernTransitionV2& t, const int height,
                                         const Consensus::Params& params)
{
    return CheckAssetConservation(prev_outputs, t.inputs, t.outputs, t.creation_actions, height,
                                  params);
}

} // namespace modern

#endif // B3COIN_MODERN_ASSET_H
