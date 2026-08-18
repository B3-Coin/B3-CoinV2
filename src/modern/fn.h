// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_FN_H
#define B3COIN_MODERN_FN_H

#include <consensus/amount.h>
#include <hash.h>
#include <modern/creation_action.h>
#include <modern/policy.h>
#include <pubkey.h>
#include <primitives/transaction_identifier.h>
#include <uint256.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace modern {

/**
 * FN Coin v1 — the modern FN Coin data model (owner rulings 2026-08-17
 * and the corrective ruling 2026-08-18;
 * doc/design/b3-legacy-fn-issuance-proposal.md).
 *
 * THE LOCKED MODEL: FN Coin is ONE global, chain-scoped,
 * fungible-but-indivisible modern colored asset — decimals = 0, at most
 * MAX_FN_EVER_ISSUED units ever. Every FN unit is a unit of the same
 * FnAssetId; there is NOT one asset per PoD, and the PoDId is never FN
 * identity — it is only the one-time issuance receipt/nullifier
 * (`issued[pod_id]`, future consensus state). After issuance an FN unit
 * is ordinary fungible FN Coin; its historical origin is irrelevant to
 * transfer semantics. Ownership is the modern ownership-policy
 * commitment (one party, threshold or shared — ownership structure and
 * divisibility are separate concepts: 2-of-3 over amount 1 is one
 * jointly controlled unit, never thirds).
 *
 * THE COMMON PoD INVARIANT (both eras): valid PoD → the native B3
 * sacrifice is permanent → authorized issuance of exactly +1
 * FN_ASSET_ID. HISTORICAL FN: the legacy PoD destroyed native B3
 * historically; a modern historical issuance verifies that sealed fact
 * and creates exactly 1 FN Coin. MODERN FN (future work): a modern PoD
 * destroys native B3 in the modern transaction itself and its validated
 * issuance creates exactly 1 FN Coin. The eras differ ONLY in where the
 * PoD evidence originates and when the B3 destruction occurred; in
 * neither era does FN Coin replace, refund, denominate, or recreate the
 * destroyed B3.
 *
 * CONSENSUS-INACTIVE BY CONSTRUCTION: FN v1 is not an activated policy
 * on any network (modern/policy.h IsActivatedPolicy), and nothing in
 * block validation, mempool, wallet, RPC, mining, rewards or supply
 * accounting calls this header. It defines only the canonical data
 * model, byte-stable serialization, strict parsing and pure model
 * helpers. The only permitted caller besides tests is the OFFLINE
 * capacity report (node/fn_pod.cpp -podreport), which measures — never
 * validates.
 *
 * SUPERSEDED CONTENT KEPT AS RESERVED RECORD: the funding-key
 * authorization records, FnClaimActionV1 and the claim-intent digest
 * below belong to the abandoned funding-signature user-claim design
 * (superseded by the archival-builder / stateless-proof issuance model,
 * conflict register C-R4). Their registered creation-action type (1, 1)
 * and codec bytes are preserved so old bytes never acquire new meaning;
 * they are UNSUPPORTED for FN issuance and the issuance path rejects
 * them (modern/legacy_fn_issuance.h carries the live action, type 2).
 */

//! FN v1 policy identity, pinned. The numeric values are consensus-stable.
inline constexpr uint16_t FN_POLICY_TYPE{static_cast<uint16_t>(PolicyType::FN)};
static_assert(FN_POLICY_TYPE == 5, "PolicyType::FN is pinned to 5");
inline constexpr uint16_t FN_POLICY_VERSION_V1{POLICY_VERSION_V1};

//! Hard cap on FN units EVER issued (legacy + modern issuance together;
//! doc/design/b3-fn-pod.md §11.1 alias MAX_FN_EVER_CREATED, owner
//! selection D-1). `issued_total` is monotonic: extinguishment reduces
//! live supply but NEVER reopens issuance capacity.
inline constexpr uint32_t MAX_FN_EVER_ISSUED{1000};

/**
 * The ONE global FN Coin asset identity (owner ruling 2026-08-18):
 *
 *     FN_ASSET_ID = TaggedHash("B3/FN/ASSET/V1") << ModernChainDomain
 *
 * Chain-scoped and deterministic — the domain input is the fail-closed
 * ModernChainDomain (genesis || X), so the id exists only once H/X
 * exist, is identical on every node of one chain, and differs across
 * chains. "PoD A → +1, PoD B → +1": all units of the SAME asset. It
 * cannot equal NativeAsset() (the all-zero id) short of a SHA256
 * preimage. The MAINNET value is pinned only after mainnet H/X — and
 * with it the mainnet domain — is frozen; tests pin the derivation
 * (tag, preimage, byte order) on synthetic vectors.
 */
inline AssetId FnAssetId(const uint256& chain_domain)
{
    HashWriter writer{TaggedHash("B3/FN/ASSET/V1")};
    writer << chain_domain;
    return writer.GetSHA256();
}

// The generic CreationAction frame, the (type, version) registry and
// the decode bounds live in the NEUTRAL layer, modern/creation_action.h
// — this header adds only the FN-specific payload rules on top. The
// v2-envelope carriage lives in modern/proof.h (versioned; v1 frozen).

// ---- The FN v1 output ---------------------------------------------------

/**
 * Typed view of one FN v1 ModernOutput (corrected model, owner ruling
 * 2026-08-18):
 *
 *     asset             = FN_ASSET_ID   (what asset this is — never
 *                                        native B3, never per-PoD)
 *     amount            = whole integer FN units (decimals = 0)
 *     policy_type       = FN (5), policy_version = 1
 *     policy_commitment = the modern ownership-policy commitment
 *                         (who controls the units)
 *     policy_params     = EMPTY. Canonical for v1: no FN-v1 lifecycle
 *                         parameters are approved, so none exist —
 *                         opaque bytes that future code might
 *                         reinterpret are not accepted. Any future
 *                         lifecycle state requires a new explicitly
 *                         defined FN policy version.
 *
 * No PoDId lives here: an FN output carries no issuance origin, is
 * never bound to a disintegration, and transfers copy nothing
 * historical. FN v1 wraps modern ownership authorization: the color is
 * the asset id, control is the committed ownership policy, so
 * shared/threshold/organizational ownership needs nothing FN-specific
 * (one commitment controlling amount 1 is one jointly controlled whole
 * unit, never fractions). STATUS: the commitment is a REPRESENTATION
 * only — actual signature/threshold authorization is NOT implemented
 * (VerifyTransitionProof today requires only a nonempty payload) and is
 * a mandatory pre-activation item.
 */
struct FnOutputView {
    CAmount amount{0}; // whole FN units
    uint256 owner_commitment{};

    friend bool operator==(const FnOutputView& a, const FnOutputView& b)
    {
        return a.amount == b.amount && a.owner_commitment == b.owner_commitment;
    }
};

//! Build the canonical FN v1 ModernOutput for the given chain's FN
//! asset. Returns std::nullopt when the view violates v1 (null owner
//! commitment, unit count outside [1, MAX_FN_EVER_ISSUED] — a zero FN
//! balance is represented by NO output, never by a zero-amount output)
//! or the asset id is the native asset.
inline std::optional<ModernOutput> MakeFnOutput(const FnOutputView& view,
                                                const AssetId& fn_asset_id)
{
    if (fn_asset_id == NativeAsset()) return std::nullopt;
    if (view.owner_commitment.IsNull()) return std::nullopt;
    if (view.amount < 1 || view.amount > static_cast<CAmount>(MAX_FN_EVER_ISSUED)) {
        return std::nullopt;
    }
    ModernOutput out;
    out.asset = fn_asset_id;
    out.amount = view.amount;
    out.policy_type = FN_POLICY_TYPE;
    out.policy_version = FN_POLICY_VERSION_V1;
    out.policy_commitment = view.owner_commitment;
    // policy_params: canonically EMPTY for v1 (default-constructed).
    return out;
}

//! Whether the output claims the FN v1 policy identity at all.
inline bool IsFnPolicyOutput(const ModernOutput& out)
{
    return out.policy_type == FN_POLICY_TYPE && out.policy_version == FN_POLICY_VERSION_V1;
}

//! Strict parse of an FN-claiming ModernOutput against the chain's FN
//! asset id. Returns std::nullopt with `error` set when a claiming
//! output violates any v1 rule. NOTE: the amount bound is STRUCTURAL
//! representability only — an output holding up to MAX_FN_EVER_ISSUED
//! units is well-formed, but no transaction may CREATE units because
//! its amount parses: minting is authorized exclusively by validated
//! issuance actions (+1 each), and totals are governed by the FN
//! conservation equation, never by this parser.
inline std::optional<FnOutputView> ParseFnOutput(const ModernOutput& out,
                                                 const AssetId& fn_asset_id, std::string& error)
{
    if (!IsFnPolicyOutput(out)) {
        error = "not an FN v1 output";
        return std::nullopt;
    }
    if (out.asset == NativeAsset()) {
        error = "FN output must not carry the native asset";
        return std::nullopt;
    }
    if (out.asset != fn_asset_id) {
        error = "FN output asset is not the chain's FN asset id";
        return std::nullopt;
    }
    if (out.amount < 1 || out.amount > static_cast<CAmount>(MAX_FN_EVER_ISSUED)) {
        error = "FN unit count outside [1, MAX_FN_EVER_ISSUED]";
        return std::nullopt;
    }
    if (out.policy_commitment.IsNull()) {
        error = "FN owner commitment is null";
        return std::nullopt;
    }
    if (!out.policy_params.empty()) {
        error = "FN v1 params must be empty";
        return std::nullopt;
    }
    FnOutputView view;
    view.amount = out.amount;
    view.owner_commitment = out.policy_commitment;
    return view;
}

// ---- Pure FN supply / conservation model (inactive) --------------------

/**
 * Pure model of the future issuance-cap and live-supply rules (owner
 * ruling 2026-08-18). NOT wired anywhere: no persistent counters exist
 * in this commit; consensus state arrives with FN activation.
 *
 *     fn_issued_total : monotonic, capped by MAX_FN_EVER_ISSUED
 *     fn_live_supply  : reduced by extinguishment, which NEVER reopens
 *                       issuance capacity
 *
 * Every helper validates the model invariant
 * `live_supply <= issued_total <= MAX_FN_EVER_ISSUED` BEFORE acting —
 * a malformed state is rejected, never operated on.
 */
struct FnSupplyModel {
    uint32_t issued_total{0};
    uint32_t live_supply{0};
};

//! The structural invariant every well-formed model satisfies.
inline bool FnSupplyModelValid(const FnSupplyModel& model)
{
    return model.live_supply <= model.issued_total &&
           model.issued_total <= MAX_FN_EVER_ISSUED;
}

//! One fresh, successfully validated legacy/modern FN issuance
//! authorizes exactly +1 unit — and only below the ever-issued cap.
//! Rejects malformed model state outright.
inline bool FnAuthorizeIssuance(FnSupplyModel& model)
{
    if (!FnSupplyModelValid(model)) return false;
    if (model.issued_total >= MAX_FN_EVER_ISSUED) return false;
    ++model.issued_total;
    ++model.live_supply;
    return true;
}

//! Extinguishment destroys live units; issued_total is untouched, so
//! capacity never reopens. Rejects malformed model state outright.
inline bool FnExtinguish(FnSupplyModel& model, const uint32_t units)
{
    if (!FnSupplyModelValid(model)) return false;
    if (units > model.live_supply) return false;
    model.live_supply -= units;
    return true;
}

//! The future FN conservation equation (recorded and testable here,
//! deliberately NOT wired into the production asset checker):
//!     Σ FN input units + validated fresh issuances
//!         == Σ FN live output units + Σ FN extinguished units
//! Overflow-safe: each side is guarded BEFORE the addition, so a
//! wraparound (e.g. UINT64_MAX + 1) can never fabricate a balance.
inline bool CheckFnUnitConservation(const uint64_t input_units, const uint64_t fresh_issuances,
                                    const uint64_t output_units,
                                    const uint64_t extinguished_units)
{
    if (input_units > std::numeric_limits<uint64_t>::max() - fresh_issuances) return false;
    if (output_units > std::numeric_limits<uint64_t>::max() - extinguished_units) return false;
    return input_units + fresh_issuances == output_units + extinguished_units;
}

// ========================================================================
// SUPERSEDED RECORD from here to the end of this header: the funding-key
// authorization records, FnClaimActionV1 and the claim-intent digest of
// the abandoned funding-signature user-claim design. Codec history only
// — see the header comment above for the full supersession contract.
// Do not extend, do not reinterpret.
// ========================================================================

// ---- Historical funding-key authorization records (SUPERSEDED) ---------
// These bytes prove control of LEGACY funding keys and are legitimately
// legacy-shaped (P2PKH / P2PK key forms); everything they authorize is
// modern. Strict form checks only — no verification against PodRecords
// happens anywhere in this header.

//! Authorization form discriminator (one byte on the wire).
enum class FnAuthForm : uint8_t {
    P2PKH = 0x01, //!< carries pubkey + signature
    P2PK = 0x02,  //!< carries signature only; the key is the funding
                  //!< script's embedded key (owner ruling 2026-08-17)
};

/**
 * One funding-script authorization. funding_script_index refers to the
 * canonically ordered (lexicographic by script bytes) distinct funding
 * scripts of the PoD; records appear in ascending index order, exactly
 * one per index starting at 0, so on the wire index_i == i always. The
 * signature is a bare strict-DER, low-S ECDSA signature over the
 * claim-intent digest — no sighash byte.
 */
struct FnAuthorization {
    uint32_t funding_script_index{0};
    FnAuthForm form{FnAuthForm::P2PKH};
    std::vector<unsigned char> pubkey;    //!< P2PKH only; empty for P2PK
    std::vector<unsigned char> signature; //!< bare strict-DER, low-S

    friend bool operator==(const FnAuthorization& a, const FnAuthorization& b)
    {
        return a.funding_script_index == b.funding_script_index && a.form == b.form &&
               a.pubkey == b.pubkey && a.signature == b.signature;
    }
};

namespace detail {

//! Bare strict-DER bounds (no sighash byte): 30 len 02 rlen r 02 slen s.
inline constexpr size_t MIN_BARE_DER_SIG{8};
inline constexpr size_t MAX_BARE_DER_SIG{72};

/**
 * Strict DER check for a BARE signature (no trailing sighash byte) —
 * modeled on the interpreter's IsValidSignatureEncoding with the
 * sighash-byte handling removed.
 */
inline bool IsStrictBareDer(const std::vector<unsigned char>& sig)
{
    if (sig.size() < MIN_BARE_DER_SIG || sig.size() > MAX_BARE_DER_SIG) return false;
    if (sig[0] != 0x30) return false;
    if (sig[1] != sig.size() - 2) return false;
    const size_t len_r{sig[3]};
    if (5 + len_r >= sig.size()) return false;
    const size_t len_s{sig[5 + len_r]};
    if (len_r + len_s + 6 != sig.size()) return false;
    if (sig[2] != 0x02) return false;
    if (len_r == 0) return false;
    if (sig[4] & 0x80) return false;
    if (len_r > 1 && sig[4] == 0x00 && !(sig[5] & 0x80)) return false;
    if (sig[len_r + 4] != 0x02) return false;
    if (len_s == 0) return false;
    if (sig[len_r + 6] & 0x80) return false;
    if (len_s > 1 && sig[len_r + 6] == 0x00 && !(sig[len_r + 7] & 0x80)) return false;
    return true;
}

inline bool CheckSignatureForm(const std::vector<unsigned char>& sig, std::string& error)
{
    if (!IsStrictBareDer(sig)) {
        error = "signature is not strict bare DER";
        return false;
    }
    if (!CPubKey::CheckLowS(sig)) {
        error = "signature is not low-S";
        return false;
    }
    return true;
}

inline bool CheckPubKeyForm(const std::vector<unsigned char>& pubkey, std::string& error)
{
    if (pubkey.size() != CPubKey::COMPRESSED_SIZE && pubkey.size() != CPubKey::SIZE) {
        error = "public key has an invalid length";
        return false;
    }
    const CPubKey key{pubkey};
    if (!key.IsFullyValid()) {
        error = "public key is not a valid curve point";
        return false;
    }
    return true;
}

// ReadCompact / WriteCompact / CompactSizeLen are shared with the
// neutral layer (modern/creation_action.h, same detail namespace).

} // namespace detail

//! Serialize one authorization record. Returns std::nullopt for a record
//! that violates its form rules.
inline std::optional<std::vector<unsigned char>> EncodeFnAuthorization(const FnAuthorization& auth)
{
    std::string error;
    if (auth.form == FnAuthForm::P2PKH) {
        if (!detail::CheckPubKeyForm(auth.pubkey, error)) return std::nullopt;
    } else if (auth.form == FnAuthForm::P2PK) {
        if (!auth.pubkey.empty()) return std::nullopt; // key derives from the funding script
    } else {
        return std::nullopt;
    }
    if (!detail::CheckSignatureForm(auth.signature, error)) return std::nullopt;

    std::vector<unsigned char> out;
    detail::WriteCompact(out, auth.funding_script_index);
    out.push_back(static_cast<unsigned char>(auth.form));
    if (auth.form == FnAuthForm::P2PKH) {
        detail::WriteCompact(out, auth.pubkey.size());
        out.insert(out.end(), auth.pubkey.begin(), auth.pubkey.end());
    }
    detail::WriteCompact(out, auth.signature.size());
    out.insert(out.end(), auth.signature.begin(), auth.signature.end());
    return out;
}

//! Strict parse of one authorization record: bounded reads (every length
//! checked against the remaining input before allocation), canonical
//! compact sizes, known form, exact pubkey lengths with a fully valid
//! key (P2PKH only), bare strict-DER low-S signature, no trailing bytes.
inline bool DecodeFnAuthorization(const std::span<const unsigned char> data, FnAuthorization& out,
                                  std::string& error)
{
    size_t cursor{0};
    uint64_t index{0};
    if (!detail::ReadCompact(data, cursor, index)) {
        error = "truncated or non-canonical funding_script_index";
        return false;
    }
    if (index > std::numeric_limits<uint32_t>::max()) {
        error = "funding_script_index out of range";
        return false;
    }
    if (cursor >= data.size()) {
        error = "truncated form byte";
        return false;
    }
    const uint8_t form_byte{data[cursor++]};
    if (form_byte != static_cast<uint8_t>(FnAuthForm::P2PKH) &&
        form_byte != static_cast<uint8_t>(FnAuthForm::P2PK)) {
        error = "unknown authorization form";
        return false;
    }
    const FnAuthForm form{static_cast<FnAuthForm>(form_byte)};

    std::vector<unsigned char> pubkey;
    if (form == FnAuthForm::P2PKH) {
        uint64_t len{0};
        if (!detail::ReadCompact(data, cursor, len)) {
            error = "truncated pubkey length";
            return false;
        }
        if (len != CPubKey::COMPRESSED_SIZE && len != CPubKey::SIZE) {
            error = "public key has an invalid length";
            return false;
        }
        if (data.size() - cursor < len) {
            error = "truncated public key";
            return false;
        }
        pubkey.assign(data.begin() + cursor, data.begin() + cursor + len);
        cursor += len;
        if (!detail::CheckPubKeyForm(pubkey, error)) return false;
    }

    uint64_t sig_len{0};
    if (!detail::ReadCompact(data, cursor, sig_len)) {
        error = "truncated signature length";
        return false;
    }
    if (sig_len < detail::MIN_BARE_DER_SIG || sig_len > detail::MAX_BARE_DER_SIG) {
        error = "signature length out of range";
        return false;
    }
    if (data.size() - cursor < sig_len) {
        error = "truncated signature";
        return false;
    }
    std::vector<unsigned char> sig(data.begin() + cursor, data.begin() + cursor + sig_len);
    cursor += sig_len;
    if (!detail::CheckSignatureForm(sig, error)) return false;

    if (cursor != data.size()) {
        error = "trailing bytes after authorization";
        return false;
    }
    out.funding_script_index = static_cast<uint32_t>(index);
    out.form = form;
    out.pubkey = std::move(pubkey);
    out.signature = std::move(sig);
    return true;
}

// ---- The FN claim creation action --------------------------------------

/**
 * FN claim action v1: authorizes the creation of exactly ONE FN v1
 * output of this transition. Wire form (CreationAction payload,
 * action_type = CREATION_ACTION_FN_CLAIM, action_version = 1):
 *
 *     compactSize(fn_output_index)
 *     compactSize(n_authorizations)            (n >= 1)
 *     n × { compactSize(record_len) record }   (records ascending,
 *                                               index_i == i)
 *
 * The PoDId is NOT repeated here — it lives in the referenced FN
 * output's params; one object, one source of truth.
 */
struct FnClaimActionV1 {
    uint32_t fn_output_index{0};
    std::vector<FnAuthorization> authorizations;

    friend bool operator==(const FnClaimActionV1& a, const FnClaimActionV1& b)
    {
        return a.fn_output_index == b.fn_output_index && a.authorizations == b.authorizations;
    }
};

//! Serialize an FN claim action into its CreationAction. Returns
//! std::nullopt when the action violates v1 (no authorizations,
//! mis-indexed records, malformed record, payload above the bound).
inline std::optional<CreationAction> EncodeFnClaimAction(const FnClaimActionV1& action)
{
    if (action.authorizations.empty()) return std::nullopt;
    std::vector<unsigned char> payload;
    detail::WriteCompact(payload, action.fn_output_index);
    detail::WriteCompact(payload, action.authorizations.size());
    for (size_t i{0}; i < action.authorizations.size(); ++i) {
        if (action.authorizations[i].funding_script_index != i) return std::nullopt;
        const auto record{EncodeFnAuthorization(action.authorizations[i])};
        if (!record) return std::nullopt;
        detail::WriteCompact(payload, record->size());
        payload.insert(payload.end(), record->begin(), record->end());
    }
    if (payload.size() > MAX_CREATION_ACTION_PAYLOAD) return std::nullopt;
    CreationAction out;
    out.action_type = CREATION_ACTION_FN_CLAIM;
    out.action_version = FN_CLAIM_ACTION_VERSION_V1;
    out.payload = std::move(payload);
    return out;
}

//! Strict parse of an FN claim CreationAction: exact type/version, the
//! payload bound, canonical compact sizes, bounded reads, ascending
//! contiguous record indexes, full payload consumption.
inline bool DecodeFnClaimAction(const CreationAction& action, FnClaimActionV1& out,
                                std::string& error)
{
    if (action.action_type != CREATION_ACTION_FN_CLAIM ||
        action.action_version != FN_CLAIM_ACTION_VERSION_V1) {
        error = "unknown creation-action type or version";
        return false;
    }
    if (action.payload.size() > MAX_CREATION_ACTION_PAYLOAD) {
        error = "creation-action payload exceeds the proof-area bound";
        return false;
    }
    const std::span<const unsigned char> data{action.payload};
    size_t cursor{0};
    uint64_t output_index{0};
    if (!detail::ReadCompact(data, cursor, output_index)) {
        error = "truncated fn_output_index";
        return false;
    }
    if (output_index > std::numeric_limits<uint32_t>::max()) {
        error = "fn_output_index out of range";
        return false;
    }
    uint64_t count{0};
    if (!detail::ReadCompact(data, cursor, count)) {
        error = "truncated authorization count";
        return false;
    }
    if (count == 0) {
        error = "claim action carries no authorizations";
        return false;
    }
    // Bound BEFORE any allocation: each record occupies at least its
    // 1-byte length prefix plus one byte.
    if (count > data.size() - cursor) {
        error = "authorization count exceeds the payload";
        return false;
    }
    std::vector<FnAuthorization> auths;
    auths.reserve(count);
    for (uint64_t i{0}; i < count; ++i) {
        uint64_t len{0};
        if (!detail::ReadCompact(data, cursor, len)) {
            error = "truncated authorization length";
            return false;
        }
        if (len == 0 || data.size() - cursor < len) {
            error = "truncated authorization record";
            return false;
        }
        FnAuthorization auth;
        if (!DecodeFnAuthorization(data.subspan(cursor, len), auth, error)) return false;
        cursor += len;
        if (auth.funding_script_index != i) {
            error = "authorization records are duplicated, omitted or out of order";
            return false;
        }
        auths.push_back(std::move(auth));
    }
    if (cursor != data.size()) {
        error = "trailing bytes after the claim action";
        return false;
    }
    out.fn_output_index = static_cast<uint32_t>(output_index);
    out.authorizations = std::move(auths);
    return true;
}

//! SUPERSEDED MEASUREMENT: exact worst-case serialized payload of the
//! ABANDONED type-1 claim action for a PoD with `n_scripts` distinct
//! funding scripts (every record its P2PKH worst: uncompressed key,
//! 72-byte DER). Feeds only the superseded, NON-AUTHORITATIVE
//! -podreport payload figures (node/fn_pod.h PodCapacityReport); says
//! nothing about the live type-2 issuance carrier, whose real encoded
//! sizes remain unmeasured future work.
inline size_t WorstCaseFnClaimActionPayload(const size_t n_scripts)
{
    size_t payload{detail::CompactSizeLen(std::numeric_limits<uint32_t>::max())}; // output index
    payload += detail::CompactSizeLen(n_scripts);
    for (size_t i{0}; i < n_scripts; ++i) {
        const size_t record{detail::CompactSizeLen(i) + 1 /*form*/ + 1 + CPubKey::SIZE +
                            1 + detail::MAX_BARE_DER_SIG};
        payload += detail::CompactSizeLen(record) + record;
    }
    return payload;
}

/**
 * The FN semantic gate over a transition's creation actions under the
 * CORRECTED model. Type (1, 1) — the abandoned claim action — is codec
 * history only: the generic framing layer may still decode its bytes,
 * but NO current FN semantic checker accepts it, so this function
 * REJECTS every type-1 action outright. Deep validation of the live
 * issuance action (type 2) is chain-contextual and lives in
 * modern/legacy_fn_issuance.h VerifyLegacyFnIssuanceAction — not here.
 */
inline bool CheckFnCreationActions(const std::vector<CreationAction>& actions,
                                   const std::vector<ModernOutput>& outputs, std::string& error)
{
    (void)outputs;
    for (const CreationAction& action : actions) {
        if (action.action_type == CREATION_ACTION_FN_CLAIM) {
            error = "superseded FN claim action is not accepted";
            return false;
        }
    }
    return true;
}

// ---- Claim-intent digest (§8.3, revised 2026-08-17) --------------------

//! The contract's immutable anti-replay network identifier:
//! TaggedHash("B3/MODERN/CHAIN", genesis || X), both as their 32 raw
//! internal-order (header-serialization) bytes. Pure function of its
//! arguments — no defaults, no globals; fails closed (nullopt) when
//! either hash is null, so a call site with an unset X cannot obtain a
//! domain.
inline std::optional<uint256> ModernChainDomain(const uint256& genesis_hash,
                                                const uint256& final_legacy_hash)
{
    if (genesis_hash.IsNull() || final_legacy_hash.IsNull()) return std::nullopt;
    HashWriter writer{TaggedHash("B3/MODERN/CHAIN")};
    writer << genesis_hash << final_legacy_hash;
    return writer.GetSHA256();
}

/**
 * The claim-intent digest every funding-key authorization signs:
 *
 *     TaggedHash("B3/FN/CLAIM/V1",
 *                ModernChainDomain(32)
 *                || PoDId(32, internal order)
 *                || underlying_B3_value (int64, 8 bytes little-endian)
 *                || OWNER_v1_policy_commitment(32))
 *
 * All fixed-width; no CScript appears. It binds the right to one exact
 * FN Coin — PoDId, modern B3 value and modern owner — on one exact
 * network. This is a claim-INTENT digest, not a transaction sighash:
 * copying it can only recreate the same PoDId with the same value to
 * the same owner commitment, and the later claimed[pod_id] validation
 * prevents a second successful issuance. Fails closed (nullopt) on an
 * amount outside MoneyRange.
 */
inline std::optional<uint256> FnClaimDigest(const uint256& chain_domain, const Txid& pod_id,
                                            const CAmount value,
                                            const uint256& owner_commitment)
{
    if (value < 0 || value > MAX_MONEY) return std::nullopt;
    HashWriter writer{TaggedHash("B3/FN/CLAIM/V1")};
    writer << chain_domain << pod_id;
    writer << static_cast<int64_t>(value); // 8 bytes, little-endian
    writer << owner_commitment;
    return writer.GetSHA256();
}

} // namespace modern

#endif // B3COIN_MODERN_FN_H
