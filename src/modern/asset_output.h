// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_ASSET_OUTPUT_H
#define B3COIN_MODERN_ASSET_OUTPUT_H

#include <consensus/amount.h>
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <modern/policy.h>
#include <primitives/transaction.h>
#include <script/script.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace modern {

/**
 * Canonical CTxOut carrier for spendable policy assets.
 *
 * An OWNER/FN output is:
 *
 *     PUSH(payload) OP_DROP <non-empty owner script...>
 *
 * and an explicit burn is the same Modern output envelope with no owner:
 *
 *     PUSH(payload) OP_DROP OP_FALSE
 *
 * In both forms nValue is zero and:
 *
 *     payload = "B3A1" || asset[32] || amount_u64_be
 *               || policy_type_u16_be || policy_version_u16_be
 *               || policy_params[0..80]
 *
 * OWNER-v1 and FN-v1 require empty params. FN-v2 carries exactly one
 * canonical, non-infinity 48-byte BLS public key for an active FlowMesh seat.
 * DEX_VAULT-v2 is keyless and appends exactly OP_FALSE. Its raw wire params
 * are VaultId[32] || {kind, shard, [account]}, while the parsed ModernOutput
 * separates VaultId into policy_commitment and exposes only the semantic
 * suffix in policy_params. A native vault carries the same positive amount
 * in both the payload and nValue; a non-native vault has nValue zero.
 * OWNER and FN derive policy_commitment as SHA256 of the exact owner suffix,
 * which is a script rather than an address type: ordinary bare-script
 * semantics allow single-key, multisig and other owner arrangements. BURN has
 * no owner suffix, has the null commitment, and never enters the modern UTXO
 * set. It is not an OP_RETURN/data-carrier output: its BURN policy supplies the
 * semantics.
 * Historical FN genesis separately
 * constructs its ruled byte-exact P2PKH recipient scripts; that is not a
 * restriction on this shared carrier.
 *
 * This file defines only the carrier and its structural checks.  It does
 * not activate asset policies or install transaction/block validation.
 */
inline constexpr std::array<unsigned char, 4> ASSET_OUTPUT_MAGIC{'B', '3', 'A', '1'};
inline constexpr size_t ASSET_OUTPUT_HEADER_SIZE{ASSET_OUTPUT_MAGIC.size() + 32 + 8 + 2 + 2};
inline constexpr size_t ASSET_OUTPUT_MIN_PAYLOAD_SIZE{ASSET_OUTPUT_HEADER_SIZE};
inline constexpr size_t ASSET_OUTPUT_MAX_PAYLOAD_SIZE{ASSET_OUTPUT_HEADER_SIZE +
                                                       MAX_POLICY_PARAMS_SIZE};
static_assert(ASSET_OUTPUT_HEADER_SIZE == 48);
static_assert(ASSET_OUTPUT_MAX_PAYLOAD_SIZE == 128);
static_assert(ASSET_OUTPUT_MAX_PAYLOAD_SIZE <= MAX_SCRIPT_ELEMENT_SIZE);

inline constexpr bool IsAssetOwnerPolicyType(const uint16_t policy_type)
{
    return policy_type == static_cast<uint16_t>(PolicyType::OWNER) ||
           policy_type == static_cast<uint16_t>(PolicyType::FN);
}

inline bool IsAssetOwnerPolicyShape(const uint16_t policy_type,
                                    const uint16_t policy_version,
                                    const std::vector<unsigned char>& policy_params)
{
    if (policy_type == static_cast<uint16_t>(PolicyType::OWNER)) {
        return policy_version == POLICY_VERSION_V1 && policy_params.empty();
    }
    if (policy_type != static_cast<uint16_t>(PolicyType::FN)) return false;
    if (policy_version == POLICY_VERSION_V1) return policy_params.empty();
    return policy_version == FN_SEAT_POLICY_VERSION_V2 &&
           policy_params.size() == FN_SEAT_POLICY_PARAMS_SIZE &&
           bls::PublicKey::Decode(policy_params).has_value();
}

//! The OWNER/FN v1 policy commitment for an exact owner-script suffix.
inline uint256 AssetOwnerCommitment(const CScript& owner_script)
{
    uint256 commitment;
    CSHA256().Write(owner_script.data(), owner_script.size()).Finalize(commitment.begin());
    return commitment;
}

namespace asset_output_detail {

inline std::vector<unsigned char> Payload(const ModernOutput& out)
{
    std::vector<unsigned char> payload;
    payload.reserve(ASSET_OUTPUT_HEADER_SIZE + out.policy_params.size());
    payload.insert(payload.end(), ASSET_OUTPUT_MAGIC.begin(), ASSET_OUTPUT_MAGIC.end());
    payload.insert(payload.end(), out.asset.begin(), out.asset.end());
    unsigned char fields[12];
    WriteBE64(fields, static_cast<uint64_t>(out.amount));
    WriteBE16(fields + 8, out.policy_type);
    WriteBE16(fields + 10, out.policy_version);
    payload.insert(payload.end(), fields, fields + sizeof(fields));
    payload.insert(payload.end(), out.policy_params.begin(), out.policy_params.end());
    return payload;
}

inline std::vector<unsigned char> DexVaultPayload(const ModernOutput& out)
{
    ModernOutput wire{out};
    wire.policy_params.clear();
    wire.policy_params.reserve(out.policy_commitment.size() + out.policy_params.size());
    wire.policy_params.insert(wire.policy_params.end(), out.policy_commitment.begin(),
                              out.policy_commitment.end());
    wire.policy_params.insert(wire.policy_params.end(), out.policy_params.begin(),
                              out.policy_params.end());
    return Payload(wire);
}

//! Locate the data bytes of a push without requiring the complete push to
//! be present.  This lets a truncated/non-minimal B3A1 push claim the
//! namespace and fail closed instead of becoming an ordinary output.
inline std::optional<size_t> PushDataOffset(const CScript& script, const size_t opcode_pos,
                                            uint64_t& claimed_size)
{
    if (opcode_pos >= script.size()) return std::nullopt;
    const uint8_t opcode{script[opcode_pos]};
    if (opcode >= 1 && opcode <= 75) {
        claimed_size = opcode;
        return opcode_pos + 1;
    }
    if (opcode == OP_PUSHDATA1) {
        if (opcode_pos + 2 > script.size()) return std::nullopt;
        claimed_size = script[opcode_pos + 1];
        return opcode_pos + 2;
    }
    if (opcode == OP_PUSHDATA2) {
        if (opcode_pos + 3 > script.size()) return std::nullopt;
        claimed_size = ReadLE16(script.data() + opcode_pos + 1);
        return opcode_pos + 3;
    }
    if (opcode == OP_PUSHDATA4) {
        if (opcode_pos + 5 > script.size()) return std::nullopt;
        claimed_size = ReadLE32(script.data() + opcode_pos + 1);
        return opcode_pos + 5;
    }
    return std::nullopt;
}

inline bool ClaimsPayloadAt(const CScript& script, const size_t opcode_pos)
{
    uint64_t claimed_size{0};
    const auto data_pos{PushDataOffset(script, opcode_pos, claimed_size)};
    if (!data_pos || claimed_size < ASSET_OUTPUT_MAGIC.size() ||
        *data_pos > script.size() || script.size() - *data_pos < ASSET_OUTPUT_MAGIC.size()) {
        return false;
    }
    return std::equal(ASSET_OUTPUT_MAGIC.begin(), ASSET_OUTPUT_MAGIC.end(),
                      script.begin() + *data_pos);
}

inline bool HasMinimalPush(const CScript& script, const CScript::const_iterator push_begin,
                           const CScript::const_iterator push_end,
                           const std::vector<unsigned char>& payload)
{
    const CScript minimal{CScript() << payload};
    return static_cast<size_t>(push_end - push_begin) == minimal.size() &&
           std::equal(minimal.begin(), minimal.end(), push_begin);
}

} // namespace asset_output_detail

/**
 * Whether a script claims the B3A1 asset-output namespace.  The canonical
 * carrier begins with the payload push.  The abandoned OP_RETURN-first shape
 * is also recognized as a claim solely so it fails closed; it is never a valid
 * asset output. Claiming is deliberately wider than validity: any push
 * encoding and even a push truncated after the four magic bytes still claims.
 */
inline bool ClaimsAssetOutput(const CScript& script)
{
    if (asset_output_detail::ClaimsPayloadAt(script, 0)) return true;
    return !script.empty() && script[0] == OP_RETURN &&
           asset_output_detail::ClaimsPayloadAt(script, 1);
}

inline bool ClaimsAssetOutput(const CTxOut& out)
{
    return ClaimsAssetOutput(out.scriptPubKey);
}

/**
 * Strictly parse the canonical policy-bearing carrier into its ModernOutput
 * view. OWNER/FN append their owner script; BURN and DEX_VAULT append exactly
 * OP_FALSE. BURN has no spendable UTXO, while DEX_VAULT is a keyless but
 * spendable typed UTXO. No asset policy uses OP_RETURN.
 * This is structural validation only: policy activation, FN identity and
 * per-transaction conservation remain the responsibility of their layers.
 */
inline std::optional<ModernOutput> ParseAssetOutput(const CTxOut& txout, std::string& error)
{
    if (!ClaimsAssetOutput(txout.scriptPubKey)) {
        error = "not a B3A1 asset output";
        return std::nullopt;
    }
    const CScript& script{txout.scriptPubKey};
    if (!script.empty() && script[0] == OP_RETURN) {
        error = "OP_RETURN is not an asset output carrier";
        return std::nullopt;
    }
    CScript::const_iterator pc{script.begin()};
    opcodetype opcode;
    std::vector<unsigned char> ignored;

    const CScript::const_iterator push_begin{pc};
    std::vector<unsigned char> payload;
    if (!script.GetOp(pc, opcode, payload) || opcode > OP_PUSHDATA4) {
        error = "asset payload malformed";
        return std::nullopt;
    }
    if (!asset_output_detail::HasMinimalPush(script, push_begin, pc, payload)) {
        error = "asset payload not minimally encoded";
        return std::nullopt;
    }
    if (payload.size() < ASSET_OUTPUT_MIN_PAYLOAD_SIZE ||
        payload.size() > ASSET_OUTPUT_MAX_PAYLOAD_SIZE ||
        !std::equal(ASSET_OUTPUT_MAGIC.begin(), ASSET_OUTPUT_MAGIC.end(), payload.begin())) {
        error = "asset payload has the wrong size or magic";
        return std::nullopt;
    }

    ModernOutput out;
    std::copy(payload.begin() + ASSET_OUTPUT_MAGIC.size(),
              payload.begin() + ASSET_OUTPUT_MAGIC.size() + out.asset.size(), out.asset.begin());
    const uint64_t encoded_amount{ReadBE64(payload.data() + 36)};
    out.policy_type = ReadBE16(payload.data() + 44);
    out.policy_version = ReadBE16(payload.data() + 46);
    if (encoded_amount < 1 || encoded_amount > static_cast<uint64_t>(MAX_MONEY)) {
        error = "asset amount outside [1, MAX_MONEY]";
        return std::nullopt;
    }
    out.amount = static_cast<CAmount>(encoded_amount);

    const bool dex_vault{out.policy_type == static_cast<uint16_t>(PolicyType::DEX_VAULT)};
    if (dex_vault) {
        if (out.policy_version != DEX_VAULT_POLICY_VERSION_V2) {
            error = "asset DEX_VAULT policy requires v2";
            return std::nullopt;
        }
        const size_t raw_params_size{payload.size() - ASSET_OUTPUT_HEADER_SIZE};
        if (raw_params_size != 32 + VAULT_POOL_CHANGE_PARAMS_SIZE &&
            raw_params_size != 32 + VAULT_USER_DEPOSIT_PARAMS_SIZE) {
            error = "asset DEX_VAULT policy has the wrong wire params size";
            return std::nullopt;
        }
        std::copy(payload.begin() + ASSET_OUTPUT_HEADER_SIZE,
                  payload.begin() + ASSET_OUTPUT_HEADER_SIZE + 32,
                  out.policy_commitment.begin());
        out.policy_params.assign(payload.begin() + ASSET_OUTPUT_HEADER_SIZE + 32,
                                 payload.end());
        if (!CheckVaultParams(out.policy_commitment, out.policy_params)) {
            error = "asset DEX_VAULT policy has invalid vault params";
            return std::nullopt;
        }
        if (out.asset == NativeAsset()) {
            if (txout.nValue != out.amount) {
                error = "native DEX_VAULT value must equal its encoded amount";
                return std::nullopt;
            }
        } else if (txout.nValue != 0) {
            error = "non-native DEX_VAULT must have zero native value";
            return std::nullopt;
        }
    } else {
        out.policy_params.assign(payload.begin() + ASSET_OUTPUT_HEADER_SIZE, payload.end());
        if (txout.nValue != 0) {
            error = "asset output must have zero native value";
            return std::nullopt;
        }
        if (out.asset == NativeAsset()) {
            error = "asset output must carry a non-native asset";
            return std::nullopt;
        }
    }
    // Preserve the v1 diagnostics while admitting only the one frozen v2
    // extension (FN seats). OWNER and every v1 form keep their original
    // empty-params contract.
    if (out.policy_type == static_cast<uint16_t>(PolicyType::OWNER)) {
        if (out.policy_version != POLICY_VERSION_V1) {
            error = "asset output policy version is not v1";
            return std::nullopt;
        }
        if (!out.policy_params.empty()) {
            error = "asset output v1 policy params must be empty";
            return std::nullopt;
        }
    } else if (out.policy_type == static_cast<uint16_t>(PolicyType::FN) &&
               out.policy_version == POLICY_VERSION_V1 &&
               !out.policy_params.empty()) {
        error = "asset output v1 policy params must be empty";
        return std::nullopt;
    }
    if (!script.GetOp(pc, opcode, ignored) || opcode != OP_DROP) {
        error = "asset payload not followed by OP_DROP";
        return std::nullopt;
    }

    if (out.policy_type == static_cast<uint16_t>(PolicyType::BURN)) {
        if (out.policy_version != POLICY_VERSION_V1 || !out.policy_params.empty()) {
            error = "asset BURN policy requires v1 with empty params";
            return std::nullopt;
        }
        if (!script.GetOp(pc, opcode, ignored) || opcode != OP_FALSE ||
            pc != script.end()) {
            error = "asset BURN policy requires the exact OP_FALSE terminator";
            return std::nullopt;
        }
        // Default-constructed policy_commitment is the canonical null burn
        // commitment; it is deliberately absent from the wire payload.
        return out;
    }

    if (dex_vault) {
        if (!script.GetOp(pc, opcode, ignored) || opcode != OP_FALSE ||
            pc != script.end()) {
            error = "asset DEX_VAULT policy requires the exact OP_FALSE terminator";
            return std::nullopt;
        }
        return out;
    }

    if (!IsAssetOwnerPolicyShape(out.policy_type, out.policy_version,
                                 out.policy_params)) {
        error = "spendable asset carrier has an invalid OWNER/FN policy shape";
        return std::nullopt;
    }
    const CScript owner_script{pc, script.end()};
    if (owner_script.empty()) {
        error = "asset owner suffix is empty";
        return std::nullopt;
    }
    // A carrier is a single ownership envelope, never another carrier.  Apart
    // from being redundant, nesting would make different consumers disagree
    // about whether one or several prefixes should be removed before script
    // execution.
    if (ClaimsAssetOutput(owner_script) || ClaimsB3PolicyCarrier(owner_script)) {
        error = "asset owner suffix cannot be another B3 policy carrier";
        return std::nullopt;
    }
    out.policy_commitment = AssetOwnerCommitment(owner_script);
    return out;
}

inline std::optional<ModernOutput> ParseAssetOutput(const CTxOut& txout)
{
    std::string error;
    return ParseAssetOutput(txout, error);
}

//! Build a canonical OWNER/FN carrier from a complete policy view.  The
//! supplied commitment must equal SHA256(owner_script), since commitment is
//! derived rather than serialized in this carrier.
inline std::optional<CTxOut> MakeAssetOwnerOutput(const ModernOutput& out,
                                                  const CScript& owner_script)
{
    if (out.asset == NativeAsset() || out.amount < 1 || out.amount > MAX_MONEY ||
        !IsAssetOwnerPolicyShape(out.policy_type, out.policy_version, out.policy_params) ||
        owner_script.empty() ||
        ClaimsAssetOutput(owner_script) || ClaimsB3PolicyCarrier(owner_script) ||
        out.policy_commitment != AssetOwnerCommitment(owner_script)) {
        return std::nullopt;
    }
    const std::vector<unsigned char> payload{asset_output_detail::Payload(out)};
    CScript script;
    script << payload << OP_DROP;
    script.insert(script.end(), owner_script.begin(), owner_script.end());
    return CTxOut{0, std::move(script)};
}

//! Convenience builder that derives the owner commitment.
inline std::optional<CTxOut> MakeAssetOwnerOutput(const AssetId& asset, const CAmount amount,
                                                  const uint16_t policy_type,
                                                  const CScript& owner_script)
{
    ModernOutput out;
    out.asset = asset;
    out.amount = amount;
    out.policy_type = policy_type;
    out.policy_version = POLICY_VERSION_V1;
    out.policy_commitment = AssetOwnerCommitment(owner_script);
    return MakeAssetOwnerOutput(out, owner_script);
}

inline std::optional<CTxOut> MakeAssetOwnerOutput(const AssetId& asset, const CAmount amount,
                                                  const PolicyType policy_type,
                                                  const CScript& owner_script)
{
    return MakeAssetOwnerOutput(asset, amount, static_cast<uint16_t>(policy_type), owner_script);
}

inline std::optional<CTxOut> MakeAssetOwnerOutput(const AssetId& asset, const CAmount amount,
                                                  const CScript& owner_script,
                                                  const PolicyType policy_type = PolicyType::OWNER)
{
    return MakeAssetOwnerOutput(asset, amount, policy_type, owner_script);
}

//! Build a canonical explicit BURN Modern output. The policy itself marks the
//! destruction; OP_FALSE makes the envelope fail script evaluation, and the
//! modern UTXO update path omits it entirely. No OP_RETURN is involved.
inline std::optional<CTxOut> MakeAssetBurnOutput(const ModernOutput& out)
{
    if (out.asset == NativeAsset() || out.amount < 1 || out.amount > MAX_MONEY ||
        out.policy_type != static_cast<uint16_t>(PolicyType::BURN) ||
        out.policy_version != POLICY_VERSION_V1 || !out.policy_commitment.IsNull() ||
        !out.policy_params.empty()) {
        return std::nullopt;
    }
    return CTxOut{0, CScript() << asset_output_detail::Payload(out)
                               << OP_DROP << OP_FALSE};
}

inline std::optional<CTxOut> MakeAssetBurnOutput(const AssetId& asset, const CAmount amount)
{
    ModernOutput out;
    out.asset = asset;
    out.amount = amount;
    out.policy_type = static_cast<uint16_t>(PolicyType::BURN);
    out.policy_version = POLICY_VERSION_V1;
    return MakeAssetBurnOutput(out);
}

//! Build the production DEX_VAULT-v2 carrier. `out.policy_params` is the
//! semantic {kind, shard, [account]} form; the builder prefixes the non-null
//! policy_commitment/VaultId only on the wire.
inline std::optional<CTxOut> MakeDexVaultOutput(const ModernOutput& out)
{
    if (out.amount < 1 || out.amount > MAX_MONEY ||
        out.policy_type != static_cast<uint16_t>(PolicyType::DEX_VAULT) ||
        out.policy_version != DEX_VAULT_POLICY_VERSION_V2 ||
        !CheckVaultParams(out.policy_commitment, out.policy_params)) {
        return std::nullopt;
    }
    const CAmount native_value{out.asset == NativeAsset() ? out.amount : 0};
    return CTxOut{native_value, CScript() << asset_output_detail::DexVaultPayload(out)
                                          << OP_DROP << OP_FALSE};
}

inline std::optional<CTxOut> MakeDexVaultOutput(const AssetId& asset,
                                                const CAmount amount,
                                                const uint256& vault_id,
                                                const uint8_t kind,
                                                const uint16_t shard,
                                                const uint256& account = uint256{})
{
    ModernOutput out;
    out.asset = asset;
    out.amount = amount;
    out.policy_type = static_cast<uint16_t>(PolicyType::DEX_VAULT);
    out.policy_version = DEX_VAULT_POLICY_VERSION_V2;
    out.policy_commitment = vault_id;
    out.policy_params = MakeVaultParams(kind, shard, account);
    return MakeDexVaultOutput(out);
}

//! Recover the exact owner suffix from a valid OWNER/FN carrier.
inline std::optional<CScript> AssetOwnerScript(const CScript& script)
{
    if (script.empty()) return std::nullopt;
    const auto parsed{ParseAssetOutput(CTxOut{0, script})};
    if (!parsed || !IsAssetOwnerPolicyType(parsed->policy_type)) return std::nullopt;
    CScript::const_iterator pc{script.begin()};
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, opcode, data) || !script.GetOp(pc, opcode, data) || opcode != OP_DROP) {
        return std::nullopt;
    }
    return CScript{pc, script.end()};
}

inline std::optional<CScript> AssetOwnerScript(const CTxOut& out)
{
    if (out.nValue != 0) return std::nullopt;
    return AssetOwnerScript(out.scriptPubKey);
}

inline bool IsAssetOwnerOutput(const CTxOut& out)
{
    const auto parsed{ParseAssetOutput(out)};
    return parsed && IsAssetOwnerPolicyType(parsed->policy_type);
}

inline bool IsAssetBurnOutput(const CTxOut& out)
{
    const auto parsed{ParseAssetOutput(out)};
    return parsed && parsed->policy_type == static_cast<uint16_t>(PolicyType::BURN);
}

inline bool IsDexVaultOutput(const CTxOut& out)
{
    const auto parsed{ParseAssetOutput(out)};
    return parsed && parsed->policy_type == static_cast<uint16_t>(PolicyType::DEX_VAULT);
}

//! Script-only DEX recognition for Solver. Native vault validity depends on
//! nValue, which Solver does not receive, so derive the sole admissible value
//! from the already committed B3A1 amount and then run the full parser.
inline bool IsDexVaultScript(const CScript& script)
{
    CScript::const_iterator pc{script.begin()};
    opcodetype opcode;
    std::vector<unsigned char> payload;
    if (!script.GetOp(pc, opcode, payload) || opcode > OP_PUSHDATA4 ||
        payload.size() < ASSET_OUTPUT_HEADER_SIZE ||
        !std::equal(ASSET_OUTPUT_MAGIC.begin(), ASSET_OUTPUT_MAGIC.end(), payload.begin())) {
        return false;
    }
    const bool native{std::all_of(payload.begin() + 4, payload.begin() + 36,
                                  [](const unsigned char byte) { return byte == 0; })};
    CAmount native_value{0};
    if (native) {
        const uint64_t amount{ReadBE64(payload.data() + 36)};
        if (amount > static_cast<uint64_t>(MAX_MONEY)) return false;
        native_value = static_cast<CAmount>(amount);
    }
    return IsDexVaultOutput(CTxOut{native_value, script});
}

//! A single-output fail-closed structural check.  Non-claiming outputs are
//! outside this carrier and pass unchanged.
inline bool CheckAssetOutput(const CTxOut& out, std::string& error)
{
    if (!ClaimsAssetOutput(out)) return true;
    return ParseAssetOutput(out, error).has_value();
}

//! Transaction-level structural helper.  It performs no activation check.
inline bool CheckAssetOutputs(const CTransaction& tx, std::string& error)
{
    for (size_t i{0}; i < tx.vout.size(); ++i) {
        std::string parse_error;
        if (!CheckAssetOutput(tx.vout[i], parse_error)) {
            error = "malformed asset output at output " + std::to_string(i) + ": " + parse_error;
            return false;
        }
    }
    return true;
}

} // namespace modern

#endif // B3COIN_MODERN_ASSET_OUTPUT_H
