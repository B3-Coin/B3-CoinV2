// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_CONSENSUS_ERA_H
#define BITCOIN_CONSENSUS_ERA_H

#include <consensus/flowmesh_params.h>
#include <consensus/params.h>

#include <limits>
#include <optional>

namespace Consensus {

/**
 * The two consensus eras of the continuous B3 chain.
 *
 * The chain keeps one genesis and one uninterrupted history. Blocks at
 * height <= LEGACY_FINAL_HEIGHT (H) belong to the LEGACY era and are
 * replayed under the preserved historical rules; blocks at height > H
 * belong to the MODERN era and receive full modern validation. A
 * reorganization crossing H is permanently prohibited once the boundary
 * is configured.
 *
 * Chains that never had a legacy B3Coin history (Bitcoin test chains,
 * plain regtest) are MODERN at every height.
 */
enum class B3Era {
    LEGACY,
    MODERN,
};

/**
 * Return the consensus era governing the block at `height`.
 *
 * `Params::hard_fork_height` stores the first MODERN height, i.e.
 * LEGACY_FINAL_HEIGHT + 1. While it is unset, every height of a
 * legacy-B3Coin chain remains LEGACY.
 *
 * This is the single source of truth for era selection. Use it only
 * where the block height (and therefore chain context) is already
 * unambiguous — for example against a connected CBlockIndex, or a
 * height derived from a known parent. It must NOT be used to choose a
 * wire decoder for a not-yet-decoded block: at that point no trusted
 * height exists. See doc/design/b3-era-architecture.md for the
 * decoder-selection problem.
 */
constexpr B3Era GetB3Era(int height, const Params& params)
{
    if (params.legacy_b3coin &&
        (!params.hard_fork_height || height < *params.hard_fork_height)) {
        return B3Era::LEGACY;
    }
    return B3Era::MODERN;
}

/**
 * The three block-production consensus phases of the continuous B3 chain.
 *
 * Distinct from B3Era, which selects the block/transaction FORMAT (codec and
 * hash domain). The MODERN era spans two production phases: the bounded
 * temporary-PoW transition corridor immediately after H, and modern PoS from
 * the first post-corridor height onward.
 *
 *     height <= H                 LEGACY_POS      legacy codec, legacy PoS
 *     H+1 .. H+corridor_length    TRANSITION_POW  modern codec, B3 scrypt PoW
 *     height > H+corridor_length  MODERN_POS      modern codec, modern PoS
 *
 * Chains that never had a legacy B3Coin history are MODERN_POS at every
 * height and never have a corridor. While no boundary is configured on a
 * legacy-B3 chain, every height is LEGACY_POS (live legacy operation).
 */
enum class ConsensusPhase {
    LEGACY_POS,
    TRANSITION_POW,
    MODERN_POS,
};

/**
 * LEGACY_FINAL_HEIGHT (H): the last legacy height, when a boundary is
 * configured on a legacy-B3 chain.
 */
constexpr std::optional<int> LegacyFinalHeight(const Params& params)
{
    if (!params.legacy_b3coin || !params.hard_fork_height) return std::nullopt;
    return *params.hard_fork_height - 1;
}

/**
 * True once the legacy boundary is PINNED: both the final height H and the
 * exact boundary hash X are consensus constants of this chain.
 *
 * From that moment every height <= H is attested history: the node
 * reconstructs it mechanically through the trusted replay engine, and the
 * preserved live legacy rule set (PoW, kernel, difficulty, signatures,
 * historical timestamp semantics, rewards) is never used to re-judge it.
 * While the boundary is unpinned the chain is in live legacy operation and
 * the full preserved rule set applies.
 *
 * Distinct from Chainstate::LegacyBoundaryActive(), which additionally
 * requires X to be connected on the active chain and gates the
 * cross-boundary reorganization prohibition.
 */
constexpr bool LegacyBoundaryPinned(const Params& params)
{
    return LegacyFinalHeight(params).has_value() && params.legacy_final_hash.has_value();
}

/**
 * The X-distribution PAUSE state (owner ruling 2026-08-23): the final legacy
 * height H is configured but the exact boundary hash X is not yet pinned.
 * A node in this state accepts the chain through H and FAILS CLOSED at H+1:
 * no block above H is admitted and none is produced, because nothing modern
 * can be validated without X (the modern chain domain, the corridor anchor
 * and the trusted replay all derive from it). The mandatory follow-up
 * release pins X and the corridor resumes from it. A blank-X node must
 * never enter the corridor.
 */
constexpr bool LegacyBoundaryHeightOnly(const Params& params)
{
    return LegacyFinalHeight(params).has_value() && !params.legacy_final_hash.has_value();
}

/**
 * The final temporary-PoW corridor height (H + transition_pow_length), when
 * a boundary is configured and the chain has a corridor.
 */
constexpr std::optional<int> TransitionPowFinalHeight(const Params& params)
{
    const std::optional<int> final_height{LegacyFinalHeight(params)};
    if (!final_height || params.transition_pow_length <= 0) return std::nullopt;
    return *final_height + params.transition_pow_length;
}

/**
 * First height of the modern-PoS phase: M = H + 1 + corridor length (the
 * height at which GetConsensusPhase first returns MODERN_POS). Epoch 0 of
 * the finality gadget starts here (F = M). nullopt while H is unpinned.
 */
constexpr std::optional<int> ModernPosStartHeight(const Params& params)
{
    const std::optional<int> final_height{LegacyFinalHeight(params)};
    if (!final_height) return std::nullopt;
    return *final_height + 1 + params.transition_pow_length;
}

/**
 * F = M activation (plan Commit 18): the modern-era OBJECT rules -- metadata
 * cells (frozen policies 6 FINALITY_CERT / 7 FINALITY_KEY / 8
 * MODERN_PAYLOAD_ROOT) and the Modern Payload Area types 4/5 -- are live
 * exactly when the X-pin release configuration is complete: H set, X pinned,
 * and the Modern-PoS rule set present. STAKE and FINALITY_KEY begin at H+1
 * (the corridor) so Set_0 = Snapshot(M-1) exists at M; the era gates in
 * validation keep every rule out of legacy blocks; certificates additionally
 * cannot be valid before M (no epoch state exists below it). A shipped
 * network without the pinned parameters is fail-closed everywhere this
 * predicate is consulted; mainnet pins them in the transition release while
 * the other shipped networks remain unconfigured. Known-but-inactive MPA
 * types 1..3 stay invalid regardless.
 */
constexpr bool ModernObjectRulesActive(const Params& params)
{
    return params.legacy_b3coin && params.hard_fork_height.has_value() &&
           params.legacy_final_hash.has_value() && params.modern_pos.has_value();
}

/**
 * Whether the transition release carries a complete historical FN Genesis
 * configuration. This is a cheap shape predicate only; the manifest/root
 * equality is checked by the FN Genesis validation layer before any block can
 * use it.
 */
constexpr bool FnGenesisConfigured(const Params& params)
{
    return LegacyBoundaryPinned(params) && params.hard_fork_height.has_value() &&
           params.fn_genesis_rights_root.has_value() &&
           !params.fn_genesis_manifest.empty();
}

/** FN owner outputs and transfers exist from their mandatory H+1 genesis. */
constexpr bool FnRulesActive(const int height, const Params& params)
{
    return FnGenesisConfigured(params) && height >= *params.hard_fork_height;
}

/**
 * Whether the two post-M feature heights form the one release schedule that
 * was ratified for FN PoD and simple-v1 assets. Both pins are required, FN PoD
 * starts strictly after modern PoS has begun, and colored assets cannot start
 * before FN PoD. An incomplete or contradictory schedule fails closed for
 * both features.
 */
constexpr bool FnAssetActivationScheduleConfigured(const Params& params)
{
    const std::optional<int> modern_start{ModernPosStartHeight(params)};
    return ModernObjectRulesActive(params) && modern_start.has_value() &&
           params.fn_pod_activation_height.has_value() &&
           params.asset_activation_height.has_value() &&
           *params.fn_pod_activation_height > *modern_start &&
           *params.asset_activation_height >= *params.fn_pod_activation_height;
}

/** Permissionless post-genesis PoD creation has its own explicit height. */
constexpr bool FnPodRulesActive(const int height, const Params& params)
{
    return FnRulesActive(height, params) &&
           FnAssetActivationScheduleConfigured(params) &&
           height >= *params.fn_pod_activation_height;
}

/** Simple-v1 colored assets activate only at their separately pinned height. */
constexpr bool AssetRulesActive(const int height, const Params& params)
{
    if (params.test_only_asset_policies_active) return true;
    return FnAssetActivationScheduleConfigured(params) &&
           height >= *params.asset_activation_height;
}

/**
 * Whether the transition release contains a complete A1/A2/A3 FlowMesh
 * schedule. Seat pre-binding opens at A2, while full FlowMesh service starts
 * at A3 after at least one anchor-depth runway. Leaving A3 unset or pinning an
 * insufficient runway fails both gates closed without changing A1/A2 asset
 * semantics.
 */
constexpr bool FlowMeshSeatBindingScheduleConfigured(const Params& params)
{
    return FnAssetActivationScheduleConfigured(params) &&
           params.flowmesh_activation_height.has_value() &&
           *params.asset_activation_height <=
               std::numeric_limits<int>::max() - FLOWMESH_ANCHOR_DEPTH &&
           *params.flowmesh_activation_height >=
               *params.asset_activation_height + FLOWMESH_ANCHOR_DEPTH;
}

/** FN-v2 seat outputs and type-7 evidence pre-bind from A2. */
constexpr bool FlowMeshSeatBindingRulesActive(const int height, const Params& params)
{
    return FlowMeshSeatBindingScheduleConfigured(params) &&
           height >= *params.asset_activation_height;
}

/**
 * Keyless market bootstrap deposits share the A2 preparation runway. They
 * cannot be swept, traded, checkpointed or withdrawn until full A3 rules,
 * but allowing the first colored deposit here makes its market identity 30
 * blocks deep when A3 opens.
 */
constexpr bool FlowMeshVaultPreparationRulesActive(const int height,
                                                   const Params& params)
{
    return FlowMeshSeatBindingRulesActive(height, params);
}

/** Trading, vault spends and checkpoint rules remain inactive until A3. */
constexpr bool FlowMeshRulesActive(const int height, const Params& params)
{
    return FlowMeshSeatBindingScheduleConfigured(params) &&
           height >= *params.flowmesh_activation_height;
}

/**
 * Bridge records have their own independently complete activation envelope.
 * FlowMesh A3 never turns the bridge on by itself: every proof, cap,
 * withdrawal-mode and origin-chain pin must be present, and the bridge's
 * explicit activation may not precede A3.  An approval_last_height limits
 * new mints in the bridge admission layer; it deliberately does not disable
 * light-client maintenance or already-backed withdrawal requests.
 */
inline bool BridgeRulesActive(const int height, const Params& params)
{
    if (!FlowMeshRulesActive(height, params) || !params.busd_bridge ||
        !BridgeMintParamsReady(*params.busd_bridge) ||
        !params.busd_bridge->activation_height) {
        return false;
    }
    return *params.busd_bridge->activation_height >=
               *params.flowmesh_activation_height &&
           height >= *params.busd_bridge->activation_height;
}

/**
 * Return the block-production consensus phase governing the block at
 * `height`. Single source of truth for phase selection; like GetB3Era it
 * must only be consulted where the height is already unambiguous, and never
 * to choose a wire decoder.
 */
constexpr ConsensusPhase GetConsensusPhase(const int height, const Params& params)
{
    if (GetB3Era(height, params) == B3Era::LEGACY) return ConsensusPhase::LEGACY_POS;
    if (params.legacy_b3coin && params.hard_fork_height &&
        height < *params.hard_fork_height + params.transition_pow_length) {
        return ConsensusPhase::TRANSITION_POW;
    }
    return ConsensusPhase::MODERN_POS;
}

/**
 * A disconnect of the block at `disconnect_height` would cross the
 * finalized legacy boundary. Reorganizations that disconnect H or anything
 * below it are permanently prohibited; forks entirely above H may still
 * reorganize.
 */
constexpr bool DisconnectCrossesLegacyBoundary(const Params& params, const int disconnect_height)
{
    const std::optional<int> final_height{LegacyFinalHeight(params)};
    return final_height.has_value() && disconnect_height <= *final_height;
}

/**
 * Reorganizing the active chain onto a branch that forks at `fork_height`
 * would cross the finalized legacy boundary.
 *
 * Activating such a branch disconnects every block above the fork point, so
 * the lowest block it would disconnect sits at `fork_height + 1`. A branch
 * that forks at or below H - 1 therefore requires disconnecting H or lower
 * and can never become the active chain.
 */
constexpr bool ReorgFromForkCrossesLegacyBoundary(const Params& params, const int fork_height)
{
    return DisconnectCrossesLegacyBoundary(params, fork_height + 1);
}

} // namespace Consensus

#endif // BITCOIN_CONSENSUS_ERA_H
