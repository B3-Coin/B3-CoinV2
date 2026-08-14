// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_POLICY_H
#define B3COIN_MODERN_POLICY_H

#include <coins.h>
#include <consensus/amount.h>
#include <consensus/era.h>
#include <consensus/params.h>
#include <crypto/sha256.h>
#include <script/script.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <vector>

namespace modern {

/**
 * Versioned B3 Policy Output primitives (modern era only).
 *
 * A ModernOutput is the generic typed output of the modern B3 era
 * (doc/design/b3-architecture-contract.md, FUTURE ASSET AND DEX
 * ARCHITECTURE): where the coin's value lives (asset, amount) and which
 * policy governs spending it (type, version, commitment, small bounded
 * parameters). It is its own primitive — never new fields on CTxIn or
 * CTxOut, and never any change to pre-H data: historical UTXOs remain
 * byte-identical on disk and in history, and are merely *viewed* through
 * this model when spent after the boundary.
 *
 * Only two policies exist at this stage; everything else — DEX vault,
 * staking, bridge, asset issuance — arrives strictly through explicit
 * later activation, and no generic VM will ever be added. Unknown or
 * unactivated (type, version) pairs are invalid, never ignored.
 */

//! Stable 32-byte asset identifier.
using AssetId = uint256;

//! The native B3 coin: the all-zero asset id, permanently reserved.
inline const AssetId& NativeAsset()
{
    static const AssetId native{};
    return native;
}

//! Consensus-visible policy parameters are small by design.
inline constexpr size_t MAX_POLICY_PARAMS_SIZE{80};

/**
 * Policy types. Values are consensus-stable; never renumber.
 *
 *  - LEGACY_LOCK: pre-H value under its original legacy locking script.
 *    v1: asset must be native B3, params must be empty, and the commitment
 *    is SHA256(scriptPubKey) of the historical script, which remains the
 *    enforceable spending condition.
 *  - OWNER: value owned by a modern owner commitment. v1: params must be
 *    empty; the commitment is an opaque 32-byte owner binding.
 *  - BURN: value explicitly and provably destroyed. v1: params empty,
 *    commitment must be all-zero; a burn output is unspendable by
 *    definition and exists so supply reduction is visible and exactly
 *    accounted (modern/asset.h). Part of the coloured-asset policy set,
 *    activated for tests only until the asset rules ship.
 *  - DEX_VAULT: DEX custody (modern/vault.h). v1: the commitment is the
 *    approved vault identity (non-zero) and params are exactly a 2-byte
 *    shard id, so custody spreads over many parallel UTXOs. A vault has
 *    no private key: spending is authorized only by finalized withdrawal
 *    receipts. Same test-only activation as the asset policy set.
 */
enum class PolicyType : uint16_t {
    LEGACY_LOCK = 0,
    OWNER = 1,
    BURN = 2,
    DEX_VAULT = 3,
};

//! DEX_VAULT v1 params: exactly a little-endian 2-byte shard id.
inline constexpr size_t VAULT_SHARD_PARAMS_SIZE{2};

//! First and, at this stage, only version of either policy.
inline constexpr uint16_t POLICY_VERSION_V1{1};

struct ModernOutput {
    AssetId asset{};
    CAmount amount{0};
    uint16_t policy_type{0};
    uint16_t policy_version{0};
    uint256 policy_commitment{};
    std::vector<unsigned char> policy_params{};

    SERIALIZE_METHODS(ModernOutput, obj)
    {
        READWRITE(obj.asset, obj.amount, obj.policy_type, obj.policy_version,
                  obj.policy_commitment, obj.policy_params);
    }

    friend bool operator==(const ModernOutput& a, const ModernOutput& b)
    {
        return a.asset == b.asset && a.amount == b.amount &&
               a.policy_type == b.policy_type && a.policy_version == b.policy_version &&
               a.policy_commitment == b.policy_commitment && a.policy_params == b.policy_params;
    }
};

/**
 * Whether a (type, version) pair is an activated policy. LEGACY_LOCK v1
 * and OWNER v1 are always active; BURN v1 and DEX_VAULT v1 only when the
 * coloured-asset policy set is active (assets_active). Anything else —
 * including future versions of these types — is unactivated and therefore
 * invalid. assets_active is sourced from Params::test_only_asset_policies_active
 * and is false in production, so the asset policies stay invalid until they
 * ship.
 */
inline bool IsActivatedPolicy(const uint16_t policy_type, const uint16_t policy_version,
                              const bool assets_active = false)
{
    if (policy_version != POLICY_VERSION_V1) return false;
    if (policy_type == static_cast<uint16_t>(PolicyType::LEGACY_LOCK) ||
        policy_type == static_cast<uint16_t>(PolicyType::OWNER)) {
        return true;
    }
    if (policy_type == static_cast<uint16_t>(PolicyType::BURN) ||
        policy_type == static_cast<uint16_t>(PolicyType::DEX_VAULT)) {
        return assets_active;
    }
    return false;
}

enum class PolicyOutputCheck {
    OK,
    NOT_MODERN_ERA,
    UNKNOWN_POLICY,
    BAD_AMOUNT,
    PARAMS_TOO_LARGE,
    BAD_POLICY_PARAMS,
    BAD_ASSET,
};

/**
 * Context-checked validity of a policy output at a connected height.
 * Policy outputs exist only in the modern era; in the legacy era they are
 * invalid outright.
 */
inline PolicyOutputCheck CheckPolicyOutput(const ModernOutput& out, const int height,
                                           const Consensus::Params& params)
{
    if (Consensus::GetB3Era(height, params) != Consensus::B3Era::MODERN) {
        return PolicyOutputCheck::NOT_MODERN_ERA;
    }
    if (!IsActivatedPolicy(out.policy_type, out.policy_version, params.test_only_asset_policies_active)) {
        return PolicyOutputCheck::UNKNOWN_POLICY;
    }
    // Per-asset supply rules arrive with issuance; until then every amount
    // is bounded by the native monetary range.
    if (out.amount < 0 || out.amount > MAX_MONEY) {
        return PolicyOutputCheck::BAD_AMOUNT;
    }
    if (out.policy_params.size() > MAX_POLICY_PARAMS_SIZE) {
        return PolicyOutputCheck::PARAMS_TOO_LARGE;
    }
    switch (static_cast<PolicyType>(out.policy_type)) {
    case PolicyType::LEGACY_LOCK:
        // Pre-H value is native B3 under its historical script; v1 carries
        // no parameters.
        if (out.asset != NativeAsset()) return PolicyOutputCheck::BAD_ASSET;
        if (!out.policy_params.empty()) return PolicyOutputCheck::BAD_POLICY_PARAMS;
        break;
    case PolicyType::OWNER:
        if (!out.policy_params.empty()) return PolicyOutputCheck::BAD_POLICY_PARAMS;
        break;
    case PolicyType::BURN:
        // Canonical v1 burn: no parameters, all-zero commitment.
        if (!out.policy_params.empty()) return PolicyOutputCheck::BAD_POLICY_PARAMS;
        if (!out.policy_commitment.IsNull()) return PolicyOutputCheck::BAD_POLICY_PARAMS;
        break;
    case PolicyType::DEX_VAULT:
        // The commitment names the approved vault; params carry the shard.
        if (out.policy_commitment.IsNull()) return PolicyOutputCheck::BAD_POLICY_PARAMS;
        if (out.policy_params.size() != VAULT_SHARD_PARAMS_SIZE) {
            return PolicyOutputCheck::BAD_POLICY_PARAMS;
        }
        break;
    }
    return PolicyOutputCheck::OK;
}

//! SHA256 of a legacy locking script: the LEGACY_LOCK v1 commitment.
inline uint256 LegacyLockCommitment(const CScript& script_pub_key)
{
    uint256 commitment;
    CSHA256()
        .Write(script_pub_key.data(), script_pub_key.size())
        .Finalize(commitment.begin());
    return commitment;
}

/**
 * View a pre-H UTXO through the modern model: native B3 for the coin's
 * full amount under LEGACY_LOCK v1, committed to its historical locking
 * script. A pure projection — the stored Coin, its bytes and its history
 * are never modified, and the original script remains the enforceable
 * spending condition when the output is eventually spent after H.
 */
inline ModernOutput ViewLegacyCoin(const Coin& coin)
{
    ModernOutput out;
    out.asset = NativeAsset();
    out.amount = coin.out.nValue;
    out.policy_type = static_cast<uint16_t>(PolicyType::LEGACY_LOCK);
    out.policy_version = POLICY_VERSION_V1;
    out.policy_commitment = LegacyLockCommitment(coin.out.scriptPubKey);
    return out;
}

} // namespace modern

#endif // B3COIN_MODERN_POLICY_H
