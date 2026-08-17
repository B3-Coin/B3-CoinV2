// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_FN_H
#define B3COIN_MODERN_FN_H

#include <consensus/amount.h>
#include <hash.h>
#include <modern/policy.h>
#include <modern/proof.h>
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
 * FN Coin v1 — the native-modern FN objects (doc/design/b3-fn-pod.md §8,
 * owner rulings 2026-08-17).
 *
 * CONSENSUS-INACTIVE BY CONSTRUCTION: FN v1 is not an activated policy on
 * any network (modern/policy.h IsActivatedPolicy), and nothing in block
 * validation, mempool, wallet, RPC, mining, rewards, supply accounting or
 * claimed[pod_id] state calls this header. It defines only the canonical
 * data model, byte-stable serialization, strict parsing and the
 * claim-intent digest. The only permitted caller besides tests is the
 * OFFLINE capacity report (node/fn_pod.cpp -podreport), which measures —
 * never validates.
 *
 * Era separation (owner ruling): legacy history supplies PoD eligibility,
 * the PoDId, the historical funding scripts and fresh P2PKH/P2PK proofs
 * from the historical funding keys. Modern consensus supplies the claim
 * action, FN Coin creation, the modern B3 value, the OWNER v1 policy
 * commitment (one party, a threshold group or an organization — control
 * is entirely the modern OWNER mechanism), and later transfer, rewards,
 * extinguishment and claimed[pod_id]. The historical signatures authorize
 * the creation of ONE modern FN Coin; afterwards the legacy side has no
 * further authority. No legacy destination script exists anywhere in the
 * modern objects.
 */

//! FN v1 policy identity, pinned. The numeric values are consensus-stable.
inline constexpr uint16_t FN_POLICY_TYPE{static_cast<uint16_t>(PolicyType::FN)};
static_assert(FN_POLICY_TYPE == 5, "PolicyType::FN is pinned to 5");
inline constexpr uint16_t FN_POLICY_VERSION_V1{POLICY_VERSION_V1};

//! FN v1 params are exactly the 32-byte PoDId.
inline constexpr size_t FN_POD_ID_SIZE{32};

//! Creation-action registry. Type 1 is the first and only registered
//! action: the FN claim.
inline constexpr uint16_t CREATION_ACTION_FN_CLAIM{1};
inline constexpr uint16_t FN_CLAIM_ACTION_VERSION_V1{1};

/**
 * A typed, versioned OUTPUT-BOUND creation action — a STANDALONE
 * canonical codec in this stage (owner ruling 2026-08-17). Input proofs
 * authorize spending a previous output; a creation action authorizes
 * CREATING one of the transition's own outputs. Modern-transition v1's
 * wire form is FROZEN and carries no creation actions: carrying them
 * inside the segregated proof area requires a future VERSIONED
 * modern-transition extension, which will make the action collection
 * part of ProofAreaCommitment and FullTransitionId. FN cannot activate
 * until that versioned integration is implemented. Until then this
 * struct is exercised by tests and offline tooling only.
 */
struct CreationAction {
    uint16_t action_type{0};
    uint16_t action_version{0};
    std::vector<unsigned char> payload{};

    SERIALIZE_METHODS(CreationAction, obj)
    {
        READWRITE(obj.action_type, obj.action_version, obj.payload);
    }

    friend bool operator==(const CreationAction& a, const CreationAction& b)
    {
        return a.action_type == b.action_type && a.action_version == b.action_version &&
               a.payload == b.payload;
    }
};

//! Creation-action payloads share the segregated proof area's bound.
inline constexpr size_t MAX_CREATION_ACTION_PAYLOAD{MAX_TRANSITION_PROOF_SIZE};

// ---- The FN v1 output ---------------------------------------------------

/**
 * Typed view of one FN v1 ModernOutput:
 *
 *     asset             = native B3
 *     amount            = the underlying modern B3 value
 *     policy_type       = FN (5), policy_version = 1
 *     policy_commitment = the modern OWNER v1 owner binding
 *     policy_params     = the 32-byte PoDId (raw txid bytes, internal
 *                         hash order)
 *
 * FN v1 is a wrapper around modern OWNER authorization: the color and
 * PoDId live in the FN policy; spend control is the committed OWNER
 * mechanism, so shared/threshold/organizational ownership needs nothing
 * FN-specific. Future FN spend validation delegates owner authorization
 * to OWNER and then enforces transfer-vs-extinguishment (§10.2).
 */
struct FnOutputView {
    CAmount amount{0};
    uint256 owner_commitment{};
    Txid pod_id{};

    friend bool operator==(const FnOutputView& a, const FnOutputView& b)
    {
        return a.amount == b.amount && a.owner_commitment == b.owner_commitment &&
               a.pod_id == b.pod_id;
    }
};

//! Build the canonical FN v1 ModernOutput. Returns std::nullopt when the
//! view violates v1 (zero PoDId, null owner commitment, amount outside
//! MoneyRange).
inline std::optional<ModernOutput> MakeFnOutput(const FnOutputView& view)
{
    if (view.pod_id.ToUint256().IsNull()) return std::nullopt;
    if (view.owner_commitment.IsNull()) return std::nullopt;
    if (view.amount < 0 || view.amount > MAX_MONEY) return std::nullopt;
    ModernOutput out;
    out.asset = NativeAsset();
    out.amount = view.amount;
    out.policy_type = FN_POLICY_TYPE;
    out.policy_version = FN_POLICY_VERSION_V1;
    out.policy_commitment = view.owner_commitment;
    const uint256 raw{view.pod_id.ToUint256()};
    out.policy_params.assign(raw.begin(), raw.end());
    return out;
}

//! Whether the output claims the FN v1 policy identity at all.
inline bool IsFnPolicyOutput(const ModernOutput& out)
{
    return out.policy_type == FN_POLICY_TYPE && out.policy_version == FN_POLICY_VERSION_V1;
}

//! Strict parse of an FN-claiming ModernOutput. Returns std::nullopt with
//! `error` set when a claiming output violates any v1 rule; must only be
//! called when IsFnPolicyOutput() is true.
inline std::optional<FnOutputView> ParseFnOutput(const ModernOutput& out, std::string& error)
{
    if (!IsFnPolicyOutput(out)) {
        error = "not an FN v1 output";
        return std::nullopt;
    }
    if (out.asset != NativeAsset()) {
        error = "FN output asset must be native B3";
        return std::nullopt;
    }
    if (out.amount < 0 || out.amount > MAX_MONEY) {
        error = "FN output amount outside MoneyRange";
        return std::nullopt;
    }
    if (out.policy_commitment.IsNull()) {
        error = "FN owner commitment is null";
        return std::nullopt;
    }
    if (out.policy_params.size() != FN_POD_ID_SIZE) {
        error = "FN params must be exactly the 32-byte PoDId";
        return std::nullopt;
    }
    uint256 raw;
    std::copy(out.policy_params.begin(), out.policy_params.end(), raw.begin());
    if (raw.IsNull()) {
        error = "PoDId is zero";
        return std::nullopt;
    }
    FnOutputView view;
    view.amount = out.amount;
    view.owner_commitment = out.policy_commitment;
    view.pod_id = Txid::FromUint256(raw);
    return view;
}

// ---- Historical funding-key authorization records ----------------------
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

//! Bounded canonical compact-size read from a cursor over `data`.
inline bool ReadCompact(std::span<const unsigned char> data, size_t& cursor, uint64_t& out)
{
    if (cursor >= data.size()) return false;
    const uint8_t first{data[cursor++]};
    if (first < 253) {
        out = first;
        return true;
    }
    size_t width{0};
    uint64_t min{0};
    if (first == 253) {
        width = 2;
        min = 253;
    } else if (first == 254) {
        width = 4;
        min = 0x10000;
    } else {
        width = 8;
        min = 0x100000000;
    }
    if (data.size() - cursor < width) return false;
    uint64_t value{0};
    for (size_t i{0}; i < width; ++i) {
        value |= uint64_t{data[cursor + i]} << (8 * i);
    }
    cursor += width;
    if (value < min) return false; // non-canonical
    out = value;
    return true;
}

inline void WriteCompact(std::vector<unsigned char>& out, uint64_t value)
{
    if (value < 253) {
        out.push_back(static_cast<unsigned char>(value));
    } else if (value <= 0xffff) {
        out.push_back(253);
        out.push_back(value & 0xff);
        out.push_back((value >> 8) & 0xff);
    } else if (value <= 0xffffffff) {
        out.push_back(254);
        for (int i{0}; i < 4; ++i) out.push_back((value >> (8 * i)) & 0xff);
    } else {
        out.push_back(255);
        for (int i{0}; i < 8; ++i) out.push_back((value >> (8 * i)) & 0xff);
    }
}

inline size_t CompactSizeLen(const uint64_t value)
{
    if (value < 253) return 1;
    if (value <= 0xffff) return 3;
    if (value <= 0xffffffff) return 5;
    return 9;
}

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

//! Exact worst-case serialized FN claim-action payload for a PoD with
//! `n_scripts` distinct funding scripts: every record its P2PKH worst
//! (uncompressed key, 72-byte DER). The single source of truth for the
//! offline capacity report.
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
 * Structural rules for a transition's creation actions against its
 * outputs (owner ruling 2026-08-17) — syntactic only, NOT wired into any
 * validation path in this commit:
 *
 *   - actions sorted by ascending referenced output index, no duplicates;
 *   - every referenced index exists and the output is FN v1;
 *   - exactly one claim action per FN v1 output — no FN output without
 *     an action, no action without an FN output.
 */
inline bool CheckFnCreationActions(const std::vector<CreationAction>& actions,
                                   const std::vector<ModernOutput>& outputs, std::string& error)
{
    std::set<uint32_t> claimed_outputs;
    uint64_t last_index{0};
    bool first{true};
    for (const CreationAction& action : actions) {
        FnClaimActionV1 claim;
        if (!DecodeFnClaimAction(action, claim, error)) return false;
        if (!first && claim.fn_output_index <= last_index) {
            error = "creation actions not in ascending output-index order";
            return false;
        }
        first = false;
        last_index = claim.fn_output_index;
        if (claim.fn_output_index >= outputs.size()) {
            error = "creation action references a nonexistent output";
            return false;
        }
        const ModernOutput& target{outputs[claim.fn_output_index]};
        if (!IsFnPolicyOutput(target)) {
            error = "creation action references a non-FN output";
            return false;
        }
        std::string parse_error;
        if (!ParseFnOutput(target, parse_error)) {
            error = "creation action references a malformed FN output: " + parse_error;
            return false;
        }
        claimed_outputs.insert(claim.fn_output_index);
    }
    for (uint32_t n{0}; n < outputs.size(); ++n) {
        if (IsFnPolicyOutput(outputs[n]) && !claimed_outputs.contains(n)) {
            error = "FN output has no creation action";
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
