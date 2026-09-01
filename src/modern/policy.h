// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_POLICY_H
#define B3COIN_MODERN_POLICY_H

#include <coins.h>
#include <consensus/amount.h>
#include <consensus/era.h>
#include <consensus/fn_params.h>
#include <consensus/params.h>
#include <crypto/bls.h>
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <script/script.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <span>
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
 * Each non-base policy has an explicit feature gate. A2 alone does not
 * activate the DEX vault or FlowMesh: FN-v2 pre-binding opens at A2 only when
 * a complete, anchor-depth-separated A3 schedule is pinned, and full
 * FlowMesh rules still wait for A3. No generic VM is added; unknown or
 * unactivated (type, version) pairs are invalid.
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
 *    accounted (modern/asset.h). Active contextually for FN from H+1 and for
 *    generic colored assets from A2.
 *  - DEX_VAULT: FlowMesh custody. v2: the commitment is the non-null VaultId
 *    and semantic params are {kind, shard, [account]}. A vault has no private
 *    key. The B3A1 wire carrier prefixes VaultId to those semantic params and
 *    activates only with full FlowMesh at A3; A2 seat pre-binding does not
 *    activate it. Spending authorization is specified separately.
 *  - STAKE: locked native B3 carrying a validator binding (modern/stake.h
 *    defines the v1 on-chain carrier). v1: the commitment is the owner
 *    binding (SHA256 of the owner script suffix, the OWNER scheme) and
 *    params are the 32-byte validator key plus 2 zero reserved bytes.
 *    Weight aggregates per validator key, never per output. Active from
 *    the modern era's first block: STAKE creation during the temporary-PoW
 *    corridor is what prepares the initial validator registry.
 *  - FN: FN Coin (modern/fn.h,
 *    doc/design/b3-legacy-fn-issuance-proposal.md; corrected owner
 *    specification 2026-08-18) — the ONE global chain-scoped
 *    fungible-but-indivisible colored asset, NEVER the native asset:
 *    v1 carries the non-native global FN_ASSET_ID (modern/fn.h
 *    FnAssetId, enforced by the FN layers — this layer can only pin
 *    non-native), a whole-unit amount in [1, MAX_FN_EVER_ISSUED], the
 *    modern ownership-policy commitment as the commitment (one party,
 *    a threshold group or an organization alike). v1 has canonically EMPTY
 *    params and is an ordinary FN owner output. v2 is an active FlowMesh seat:
 *    amount exactly 1 and params exactly one canonical, non-infinity 48-byte
 *    BLS public key. Historical units are created by the H+1 FN Genesis event;
 *    modern PoD issuance uses the branch-local A1 counter. No PoDId lives in
 *    an FN output. FN transfer/extinguishment is gated by the complete sealed
 *    Genesis configuration and the H+1 boundary; v2 pre-binding opens at A2
 *    only when the complete A3 runway is pinned.
 */
enum class PolicyType : uint16_t {
    LEGACY_LOCK = 0,
    OWNER = 1,
    BURN = 2,
    DEX_VAULT = 3,
    STAKE = 4,
    FN = 5,
    //! Owner-frozen numbers 2026-08-23 (never renumbered/reused). Zero-value
    //! METADATA cells — never entering the spendable UTXO set — whose large
    //! evidence lives in the Modern Payload Area, never in policy_params:
    //!   FINALITY_CERT        coinbase-only finality certificate cell
    //!                        (commitment = hash of the certificate payload,
    //!                        params = epoch u64 || height u64),
    //!   FINALITY_KEY         validator BLS-binding cell (commitment =
    //!                        validator_key, params = bls_pubkey || seq),
    //!   MODERN_PAYLOAD_ROOT  coinbase-only commitment to the block's MPA
    //!                        sections (commitment = payload_root, no params),
    //!   BRIDGE_RECORD        transaction-signature binding for one exact
    //!                        type-10 MPA record (commitment = tagged hash of
    //!                        its canonical frame, no params).
    //! These numbers are append-only. Activation remains contextual and
    //! fail-closed; a claiming output is invalid, never reinterpreted.
    FINALITY_CERT = 6,
    FINALITY_KEY = 7,
    MODERN_PAYLOAD_ROOT = 8,
    BRIDGE_RECORD = 9,
};

//! DEX_VAULT v1 params: exactly a little-endian 2-byte shard id.
//! DEX_VAULT v2 params (owner ruling 2026-08-22): {kind u8, shard u16 LE,
//! [flowmesh_account_id 32]}. Two kinds under ONE keyless vault policy:
//!   USER_DEPOSIT      — carries the FlowMesh account id; may credit that
//!                       account exactly once (35 bytes);
//!   VAULT_POOL_CHANGE — no beneficiary; withdrawal change and pooled
//!                       custody; can NEVER create a FlowMesh balance (3 bytes).
//! The v1 shard-only form (2 bytes) is retired: it is no longer an
//! activated policy version (no output of it ever existed on mainnet).
inline constexpr uint8_t VAULT_KIND_USER_DEPOSIT{1};
inline constexpr uint8_t VAULT_KIND_POOL_CHANGE{2};
inline constexpr size_t VAULT_POOL_CHANGE_PARAMS_SIZE{3};
inline constexpr size_t VAULT_USER_DEPOSIT_PARAMS_SIZE{35};
inline constexpr uint16_t DEX_VAULT_POLICY_VERSION_V2{2};
inline constexpr const char* FLOWMESH_VAULT_SHARD_TAG{"B3/FLOWMESH/SHARD/V1"};
//! Retired v1 shard-only params size, kept only so old tests/docs read.
inline constexpr size_t VAULT_SHARD_PARAMS_SIZE{2};

//! STAKE v1 params: the 32-byte validator key plus 2 reserved bytes that
//! must be zero (mirrors the script carrier's payload after its magic —
//! modern/stake.h STAKE_VALIDATOR_KEY_SIZE/STAKE_RESERVED_SIZE).
inline constexpr size_t STAKE_PARAMS_KEY_SIZE{32};
inline constexpr size_t STAKE_PARAMS_SIZE{34};

//! First and, at this stage, only version of either policy.
inline constexpr uint16_t POLICY_VERSION_V1{1};
//! FN policy v2 is the FlowMesh active-seat form.
inline constexpr uint16_t FN_SEAT_POLICY_VERSION_V2{2};
inline constexpr size_t FN_SEAT_POLICY_PARAMS_SIZE{bls::PUBKEY_SIZE};

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
 * and OWNER v1 are always active; BURN v1 follows the colored-asset/FN
 * feature gates. DEX_VAULT v2 has a separate A2 preparation gate (with an
 * isolated test hook for the policy model): outputs may bootstrap and mature
 * before A3, while every keyless spend remains proof-gated by full FlowMesh
 * activation. Anything else — including future versions — is invalid.
 */
inline bool IsActivatedPolicy(const uint16_t policy_type, const uint16_t policy_version,
                              const bool assets_active = false,
                              const bool fn_active = false,
                              const bool dex_vault_active = false,
                              const bool flowmesh_active = false)
{
    // DEX_VAULT lives at v2 (kind/shard/account params, owner ruling
    // 2026-08-22); its v1 shard-only form is retired. Every other policy is
    // v1 only.
    if (policy_type == static_cast<uint16_t>(PolicyType::DEX_VAULT)) {
        return policy_version == DEX_VAULT_POLICY_VERSION_V2 && dex_vault_active;
    }
    if (policy_type == static_cast<uint16_t>(PolicyType::FN) &&
        policy_version == FN_SEAT_POLICY_VERSION_V2) {
        return fn_active && flowmesh_active;
    }
    if (policy_version != POLICY_VERSION_V1) return false;
    if (policy_type == static_cast<uint16_t>(PolicyType::LEGACY_LOCK) ||
        policy_type == static_cast<uint16_t>(PolicyType::OWNER) ||
        policy_type == static_cast<uint16_t>(PolicyType::STAKE)) {
        return true;
    }
    if (policy_type == static_cast<uint16_t>(PolicyType::BURN)) {
        // FN extinguishment is valid as soon as FN itself is live; generic
        // colored-asset burns wait for the colored-asset activation height.
        // The contextual asset layer restricts a pre-asset-activation BURN
        // to the one chain-scoped FN asset id.
        return assets_active || fn_active;
    }
    if (policy_type == static_cast<uint16_t>(PolicyType::FN)) {
        return fn_active;
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
//! Parsed DEX_VAULT v2 params. `account` is set only for USER_DEPOSIT.
struct VaultParams {
    uint8_t kind{0};
    uint16_t shard{0};
    std::optional<uint256> account;
};

//! Strict parse of DEX_VAULT v2 params: exactly {kind, shard LE} for
//! VAULT_POOL_CHANGE (3 bytes) or {kind, shard LE, account} for
//! USER_DEPOSIT (35 bytes, non-null account). Anything else is invalid.
inline std::optional<VaultParams> ParseVaultParams(const std::vector<unsigned char>& params)
{
    if (params.size() != VAULT_POOL_CHANGE_PARAMS_SIZE &&
        params.size() != VAULT_USER_DEPOSIT_PARAMS_SIZE) {
        return std::nullopt;
    }
    VaultParams out;
    out.kind = params[0];
    out.shard = static_cast<uint16_t>(params[1] | (params[2] << 8));
    if (out.shard >= 256) return std::nullopt;
    if (out.kind == VAULT_KIND_POOL_CHANGE) {
        if (params.size() != VAULT_POOL_CHANGE_PARAMS_SIZE) return std::nullopt;
        return out;
    }
    if (out.kind == VAULT_KIND_USER_DEPOSIT) {
        if (params.size() != VAULT_USER_DEPOSIT_PARAMS_SIZE) return std::nullopt;
        uint256 account;
        std::copy(params.begin() + 3, params.end(), account.begin());
        if (account.IsNull()) return std::nullopt;
        out.account = account;
        return out;
    }
    return std::nullopt;
}

//! Deterministic USER_DEPOSIT shard. The hash input is the exact raw
//! VaultId followed by the exact raw account id; uint256 serialization is
//! deliberately avoided so no length or type prefix can enter the digest.
inline uint16_t FlowMeshUserDepositShard(const uint256& vault_id, const uint256& account)
{
    HashWriter writer{TaggedHash(FLOWMESH_VAULT_SHARD_TAG)};
    writer << std::span<const unsigned char>{vault_id.begin(), vault_id.size()}
           << std::span<const unsigned char>{account.begin(), account.size()};
    const uint256 digest{writer.GetSHA256()};
    return ReadLE16(digest.begin()) % 256;
}

//! Validate both the exact semantic v2 grammar and the owner-ratified shard
//! binding. Pool change may name any of the 256 shards; a user deposit has
//! exactly one valid shard for its (VaultId, account) pair.
inline bool CheckVaultParams(const uint256& vault_id,
                             const std::vector<unsigned char>& params)
{
    if (vault_id.IsNull()) return false;
    const auto parsed{ParseVaultParams(params)};
    if (!parsed) return false;
    if (parsed->kind != VAULT_KIND_USER_DEPOSIT) return true;
    return parsed->account &&
           parsed->shard == FlowMeshUserDepositShard(vault_id, *parsed->account);
}

//! Build DEX_VAULT v2 params for either kind.
inline std::vector<unsigned char> MakeVaultParams(const uint8_t kind, const uint16_t shard,
                                                  const uint256& account = uint256{})
{
    std::vector<unsigned char> params{kind, static_cast<unsigned char>(shard & 0xff),
                                      static_cast<unsigned char>(shard >> 8)};
    if (kind == VAULT_KIND_USER_DEPOSIT) params.insert(params.end(), account.begin(), account.end());
    return params;
}

inline PolicyOutputCheck CheckPolicyOutput(const ModernOutput& out, const int height,
                                           const Consensus::Params& params)
{
    if (Consensus::GetB3Era(height, params) != Consensus::B3Era::MODERN) {
        return PolicyOutputCheck::NOT_MODERN_ERA;
    }
    const bool dex_vault_active{
        params.test_only_asset_policies_active ||
        Consensus::FlowMeshVaultPreparationRulesActive(height, params)};
    if (!IsActivatedPolicy(out.policy_type, out.policy_version,
                           Consensus::AssetRulesActive(height, params),
                           Consensus::FnRulesActive(height, params),
                           dex_vault_active,
                           Consensus::FlowMeshSeatBindingRulesActive(height, params))) {
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
        // v2: the commitment names the approved vault; params carry
        // {kind, shard, [account]} — USER_DEPOSIT with a 32-byte FlowMesh
        // account id, or VAULT_POOL_CHANGE with no beneficiary.
        if (!CheckVaultParams(out.policy_commitment, out.policy_params)) {
            return PolicyOutputCheck::BAD_POLICY_PARAMS;
        }
        break;
    case PolicyType::STAKE: {
        // Locked NATIVE B3 carrying a validator binding: a POSITIVE
        // amount (zero-amount stake would be a free registry entry — the
        // standalone stake model requires amount > 0 and so does this
        // view), the owner binding (non-null) as the commitment, and
        // params of exactly the 32-byte validator key plus 2 zero
        // reserved bytes, with the key not all-zero (an all-zero "key"
        // can never be valid under any eventual key scheme and would
        // aggregate dead weight). Previously this case was missing and
        // an activated STAKE output fell through the switch unchecked.
        if (out.asset != NativeAsset()) return PolicyOutputCheck::BAD_ASSET;
        if (out.amount == 0) return PolicyOutputCheck::BAD_AMOUNT;
        if (out.policy_commitment.IsNull()) return PolicyOutputCheck::BAD_POLICY_PARAMS;
        if (out.policy_params.size() != STAKE_PARAMS_SIZE) {
            return PolicyOutputCheck::BAD_POLICY_PARAMS;
        }
        bool key_nonzero{false};
        for (size_t i{0}; i < STAKE_PARAMS_KEY_SIZE; ++i) {
            key_nonzero = key_nonzero || out.policy_params[i] != 0x00;
        }
        if (!key_nonzero) return PolicyOutputCheck::BAD_POLICY_PARAMS;
        for (size_t i{STAKE_PARAMS_KEY_SIZE}; i < STAKE_PARAMS_SIZE; ++i) {
            if (out.policy_params[i] != 0x00) return PolicyOutputCheck::BAD_POLICY_PARAMS;
        }
        break;
    }
    case PolicyType::FN:
        // Contextually active only after the complete sealed FN configuration
        // and its height gate. FN Coin is the ONE global
        // chain-scoped colored asset — never the native asset (the exact
        // FnAssetId needs the chain domain and is enforced by the FN
        // layers, modern/fn.h) — with whole-unit amounts, the modern
        // ownership-policy commitment, and canonically EMPTY params:
        // PoDId is an issuance nullifier, never output identity, and no
        // opaque future-reinterpretable bytes are accepted.
        if (out.asset == NativeAsset()) return PolicyOutputCheck::BAD_ASSET;
        if (out.policy_version == FN_SEAT_POLICY_VERSION_V2) {
            if (out.amount != 1) return PolicyOutputCheck::BAD_AMOUNT;
        } else if (out.amount < 1 || out.amount > Consensus::MAX_FN_EVER_ISSUED) {
            return PolicyOutputCheck::BAD_AMOUNT;
        }
        if (out.policy_commitment.IsNull()) return PolicyOutputCheck::BAD_POLICY_PARAMS;
        if (out.policy_version == POLICY_VERSION_V1) {
            if (!out.policy_params.empty()) return PolicyOutputCheck::BAD_POLICY_PARAMS;
        } else {
            if (out.policy_version != FN_SEAT_POLICY_VERSION_V2 ||
                out.policy_params.size() != FN_SEAT_POLICY_PARAMS_SIZE ||
                !bls::PublicKey::Decode(out.policy_params)) {
                return PolicyOutputCheck::BAD_POLICY_PARAMS;
            }
        }
        break;
    case PolicyType::FINALITY_CERT:
    case PolicyType::FINALITY_KEY:
    case PolicyType::MODERN_PAYLOAD_ROOT:
    case PolicyType::BRIDGE_RECORD:
        // Metadata cells never use the spendable B3A1 carrier handled here.
        // Kept explicit so the switch stays exhaustive and no future default
        // can accidentally accept them.
        return PolicyOutputCheck::UNKNOWN_POLICY;
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
