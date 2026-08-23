// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_PROOF_H
#define B3COIN_MODERN_PROOF_H

#include <crypto/sha256.h>
#include <hash.h>
#include <modern/creation_action.h>
#include <modern/policy.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <streams.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace modern {

/**
 * Typed segregated TransitionProofs (doc/design/b3-architecture-contract.md,
 * FUTURE ASSET AND DEX ARCHITECTURE).
 *
 * A ModernInput carries exactly the contract's three fields: the prevout
 * identifying WHICH coin is spent, the sequence, and a ProofRef into the
 * transaction's segregated proof area. The TransitionProof establishes WHY
 * the proposed transition is authorized. Proof material never lives inside
 * the input — it is segregated, excluded from the transition id, and
 * pinned by its own unambiguous commitment. No DEX-specific field exists
 * here or may ever be added.
 */
struct ModernInput {
    COutPoint prevout;
    uint32_t sequence{0xFFFFFFFF};
    //! ProofRef: index into the segregated proof area. v1 is canonical —
    //! input i must reference proof i — so proof data can be neither
    //! reordered nor left dangling.
    uint32_t proof_index{0};

    SERIALIZE_METHODS(ModernInput, obj) { READWRITE(obj.prevout, obj.sequence, obj.proof_index); }
};

//! Upper bound for one proof's payload. Generous for v1 (a legacy script
//! reveal plus its unlocking script); oversized proofs are invalid.
inline constexpr size_t MAX_TRANSITION_PROOF_SIZE{4000};

/**
 * A typed, versioned proof. `proof_type`/`proof_version` must match the
 * activated policy of the previous output being spent — verification is
 * dispatched by that previous output, never by the proof's own claim
 * alone.
 *
 *  - LEGACY_LOCK v1 payload: serialized {reveal script, unlock script}.
 *    The reveal must hash (SHA256) to the previous output's commitment,
 *    proving the proof targets the historical locking script. Script
 *    EXECUTION (signatures over the modern sighash) is deliberately not
 *    performed here: the modern sighash domain is not yet defined, and
 *    inventing one is out of scope. Structural and binding checks only.
 *  - OWNER v1 payload: the owner's authorization blob (non-empty,
 *    bounded). Its cryptographic verification likewise awaits the modern
 *    sighash definition; structure and dispatch are enforced now.
 */
struct TransitionProof {
    uint16_t proof_type{0};
    uint16_t proof_version{0};
    std::vector<unsigned char> payload{};

    SERIALIZE_METHODS(TransitionProof, obj)
    {
        READWRITE(obj.proof_type, obj.proof_version, obj.payload);
    }
};

/**
 * The spend side of a modern transition at the primitive stage: inputs,
 * policy outputs, and the segregated proof area. The future modern
 * transaction codec embeds this; nothing here touches CTxIn/CTxOut or any
 * pre-H data.
 *
 * v1 wire form is FROZEN as-is and carries no creation actions. The
 * versioned v2 envelope defined at the end of this header carries the
 * creation-action collection (the generic frame lives in
 * modern/creation_action.h; the FN payload rules in modern/fn.h).
 * No production outer transaction/block codec selects either form yet;
 * FN cannot activate until an outer context selects the v2 envelope
 * through its own reviewed change.
 */
struct ModernTransition {
    std::vector<ModernInput> inputs;
    std::vector<ModernOutput> outputs;
    //! Segregated proof area: one proof per input (canonical v1).
    std::vector<TransitionProof> proofs;

    SERIALIZE_METHODS(ModernTransition, obj)
    {
        READWRITE(obj.inputs, obj.outputs, obj.proofs);
    }

    //! Serialize only the id domain: inputs and outputs, never proofs.
    template <typename Stream>
    void SerializeForId(Stream& s) const
    {
        s << inputs << outputs;
    }
};

//! The transition id: commits to prevouts, sequences, proof refs and
//! outputs — and to no proof bytes at all.
inline uint256 TransitionId(const ModernTransition& t)
{
    HashWriter h;
    t.SerializeForId(h);
    return h.GetHash();
}

//! Unambiguous commitment to the segregated proof area alone.
inline uint256 ProofAreaCommitment(const ModernTransition& t)
{
    HashWriter h;
    h << t.proofs;
    return h.GetHash();
}

//! Full id: the transition id domain plus every proof byte. Together with
//! TransitionId this pins the whole transition without ambiguity.
inline uint256 FullTransitionId(const ModernTransition& t)
{
    HashWriter h;
    h << t;
    return h.GetHash();
}

enum class ProofCheck {
    OK,
    PROOF_COUNT_MISMATCH,
    PROOF_REF_NONCANONICAL,
    UNKNOWN_POLICY,
    TYPE_MISMATCH,
    OVERSIZED,
    MALFORMED,
    COMMITMENT_MISMATCH,
    UNSPENDABLE,
};

//! Upper bound of withdrawal receipts one vault proof may claim.
inline constexpr size_t MAX_VAULT_RECEIPTS_PER_PROOF{64};

/**
 * Strictly parse a DEX_VAULT v1 proof payload: a non-empty,
 * strictly-ascending (sorted, duplicate-free) list of receipt ids, fully
 * consuming the payload. Returns std::nullopt on any violation.
 */
inline std::optional<std::vector<uint256>> ParseVaultReceiptIds(
    const std::vector<unsigned char>& payload)
{
    std::vector<uint256> ids;
    try {
        SpanReader reader{std::as_bytes(std::span{payload})};
        reader >> ids;
        if (!reader.empty()) return std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (ids.empty() || ids.size() > MAX_VAULT_RECEIPTS_PER_PROOF) return std::nullopt;
    for (size_t i{1}; i < ids.size(); ++i) {
        if (!(ids[i - 1] < ids[i])) return std::nullopt;
    }
    return ids;
}

/**
 * Verify one proof against the previous output it spends. Dispatch is
 * selected by the PREVIOUS OUTPUT's policy: the proof's own type must
 * match it, and each policy defines its v1 payload rules.
 */
inline ProofCheck VerifyTransitionProof(const ModernOutput& prev_output,
                                        const TransitionProof& proof,
                                        const bool assets_active = false)
{
    if (!IsActivatedPolicy(prev_output.policy_type, prev_output.policy_version, assets_active)) {
        return ProofCheck::UNKNOWN_POLICY;
    }
    if (proof.proof_type != prev_output.policy_type ||
        proof.proof_version != prev_output.policy_version) {
        return ProofCheck::TYPE_MISMATCH;
    }
    if (proof.payload.size() > MAX_TRANSITION_PROOF_SIZE) {
        return ProofCheck::OVERSIZED;
    }

    switch (static_cast<PolicyType>(prev_output.policy_type)) {
    case PolicyType::LEGACY_LOCK: {
        std::vector<unsigned char> reveal;
        std::vector<unsigned char> unlock;
        try {
            SpanReader reader{std::as_bytes(std::span{proof.payload})};
            reader >> reveal >> unlock;
            if (!reader.empty()) return ProofCheck::MALFORMED; // trailing bytes
        } catch (const std::exception&) {
            return ProofCheck::MALFORMED;
        }
        uint256 commitment;
        CSHA256().Write(reveal.data(), reveal.size()).Finalize(commitment.begin());
        if (commitment != prev_output.policy_commitment) {
            return ProofCheck::COMMITMENT_MISMATCH;
        }
        return ProofCheck::OK;
    }
    case PolicyType::OWNER:
        if (proof.payload.empty()) return ProofCheck::MALFORMED;
        return ProofCheck::OK;
    case PolicyType::BURN:
        // Burned value is destroyed: no proof can ever spend it.
        return ProofCheck::UNSPENDABLE;
    case PolicyType::DEX_VAULT:
        // A vault has no private key. The only spendable form is a
        // canonical finalized-receipt list; its semantic verification
        // (finalization, destinations, change, conservation) lives in
        // modern/vault.h.
        if (!ParseVaultReceiptIds(proof.payload)) return ProofCheck::MALFORMED;
        return ProofCheck::OK;
    case PolicyType::STAKE:
        // DELIBERATELY UNSPENDABLE AT THIS LAYER for now: how locked
        // stake exits (unbonding/cooldown, and whether cancel remains an
        // ordinary script-carrier spend) is an OPEN modern-PoS decision
        // (corridor audit S-3), and corridor STAKE lives as a script
        // carrier spent under script rules, not as a ModernOutput. An
        // explicit case so the gap is a decision on record, not a
        // silent switch fall-through.
        return ProofCheck::UNKNOWN_POLICY;
    case PolicyType::FN:
        // UNREACHABLE until FN v1 is activated (IsActivatedPolicy fails
        // closed above). Spending an FN coin delegates owner
        // authorization to the modern OWNER v1 mechanism — the same
        // payload rules; transfer-vs-extinguishment semantics arrive
        // with the FN lifecycle validation commits.
        if (proof.payload.empty()) return ProofCheck::MALFORMED;
        return ProofCheck::OK;
    case PolicyType::FINALITY_CERT:
    case PolicyType::FINALITY_KEY:
    case PolicyType::MODERN_PAYLOAD_ROOT:
        // Metadata cells (declared 2026-08-23, not activated): zero-value,
        // never in the UTXO set, no spend path — there is no proof that can
        // ever spend one. UNREACHABLE while IsActivatedPolicy fails closed;
        // explicit so the switch stays exhaustive.
        return ProofCheck::UNSPENDABLE;
    }
    return ProofCheck::UNKNOWN_POLICY;
}

/**
 * Verify a transition's whole proof area against the previous outputs it
 * spends (`prev_outputs[i]` is the coin spent by `inputs[i]`). Enforces
 * the canonical segregated layout — one proof per input, referenced in
 * position — then dispatches each proof by its previous output's policy.
 */
inline ProofCheck VerifyTransitionProofs(const std::vector<ModernOutput>& prev_outputs,
                                         const ModernTransition& t,
                                         const bool assets_active = false)
{
    if (t.proofs.size() != t.inputs.size() || prev_outputs.size() != t.inputs.size()) {
        return ProofCheck::PROOF_COUNT_MISMATCH;
    }
    for (size_t i{0}; i < t.inputs.size(); ++i) {
        if (t.inputs[i].proof_index != i) return ProofCheck::PROOF_REF_NONCANONICAL;
        const ProofCheck result{VerifyTransitionProof(prev_outputs[i], t.proofs[i], assets_active)};
        if (result != ProofCheck::OK) return result;
    }
    return ProofCheck::OK;
}

//! Convenience builder for a LEGACY_LOCK v1 proof payload.
inline TransitionProof MakeLegacyLockProof(const std::vector<unsigned char>& reveal_script,
                                           const std::vector<unsigned char>& unlock_script)
{
    TransitionProof proof;
    proof.proof_type = static_cast<uint16_t>(PolicyType::LEGACY_LOCK);
    proof.proof_version = POLICY_VERSION_V1;
    VectorWriter writer{proof.payload, 0};
    writer << reveal_script << unlock_script;
    return proof;
}

// ======================================================================
// Versioned transition envelope — v2 (owner-approved 2026-08-17;
// bounds ruled 2026-08-17).
//
// Raw ModernTransition above remains the SOLE v1 format, byte-frozen.
// The envelope is a SEPARATE API for a SEPARATE context, accepting
// version 2 only (0, 1 and unknown values reject).
//
// COMPATIBILITY RULE (binding): raw-v1 bytes and v2-envelope bytes are
// NOT universally disjoint byte languages — a prefix such as 02 00 can
// also begin a valid raw-v1 object. Discrimination is therefore never
// done by inspection: THE FUTURE OUTER CONTEXT EXPLICITLY SELECTS
// either raw-v1 decoding or v2-envelope decoding; there is no sniffing,
// no fallback, no automatic reinterpretation; and each selected decoder
// requires exact input exhaustion. No production context selects either
// form yet — the active chain carries neither, and FN cannot activate
// until an outer context selects the v2 envelope through its own
// reviewed change.
//
// SINGULAR CODEC: EncodeTransitionEnvelope/DecodeTransitionEnvelope are
// the ONLY v2 wire codec. ModernTransitionV2 deliberately has no
// generic serialization methods, so no path can bypass the version,
// bound and canonicality checks. The canonical representation is the
// standard serialization grammar of each field, produced and consumed
// exclusively by these two functions; the v2 hash preimages below are
// built from the same canonical bytes.
// ======================================================================

//! The only version the envelope accepts.
inline constexpr uint16_t TRANSITION_VERSION_V2{2};

//! TEMPORARY DEFENSIVE PARSER CEILING (owner ruling 2026-08-17): caps
//! decode work while the codec is inactive. It is NOT the final
//! production consensus, relay or weight limit — that must be
//! reconciled with the future outer transaction/block codec before
//! activation.
inline constexpr size_t MAX_TRANSITION_ENVELOPE_SIZE{1'000'000};

static_assert(MAX_CREATION_ACTION_PAYLOAD == MAX_TRANSITION_PROOF_SIZE,
              "creation-action payloads share the proof-area bound");

//! The v2 transition body: the v1 economic fields plus the segregated
//! creation-action collection. NO generic serialization on purpose —
//! the envelope functions are the only wire codec, and equality where
//! needed is byte equality of encoded envelopes (or FullTransitionIdV2).
struct ModernTransitionV2 {
    std::vector<ModernInput> inputs;
    std::vector<ModernOutput> outputs;
    std::vector<TransitionProof> proofs;
    std::vector<CreationAction> creation_actions;
};

/**
 * Encode the envelope: uint16 version (LE, always 2) || inputs ||
 * outputs || proofs || action section — every field in the standard
 * serialization grammar, with the action-section bytes taken VERBATIM
 * from the strict section encoder. Returns std::nullopt when any
 * encode-side bound is violated: policy params above
 * MAX_POLICY_PARAMS_SIZE, a proof payload above
 * MAX_TRANSITION_PROOF_SIZE, the action bounds, or the envelope
 * ceiling.
 */
inline std::optional<std::vector<unsigned char>> EncodeTransitionEnvelope(
    const ModernTransitionV2& t)
{
    for (const ModernOutput& out : t.outputs) {
        if (out.policy_params.size() > MAX_POLICY_PARAMS_SIZE) return std::nullopt;
    }
    for (const TransitionProof& proof : t.proofs) {
        if (proof.payload.size() > MAX_TRANSITION_PROOF_SIZE) return std::nullopt;
    }
    const auto section{EncodeCreationActionSection(t.creation_actions)};
    if (!section) return std::nullopt;

    std::vector<unsigned char> out;
    VectorWriter writer{out, 0};
    writer << TRANSITION_VERSION_V2 << t.inputs << t.outputs << t.proofs;
    out.insert(out.end(), section->begin(), section->end()); // verbatim section bytes
    if (out.size() > MAX_TRANSITION_ENVELOPE_SIZE) return std::nullopt;
    return out;
}

/**
 * Strict, fully bounded envelope decode — STRUCTURAL only (FN semantic
 * validation is modern/fn.h's separate concern). Enforces, all BEFORE
 * the corresponding allocation: the envelope ceiling (before any
 * parsing); version exactly 2 (a wrong-endian 2 reads as 512 and
 * rejects as unknown); canonical compact sizes everywhere; count
 * feasibility against the remaining bytes for inputs, outputs, proofs
 * and actions; policy params <= MAX_POLICY_PARAMS_SIZE; proof payloads
 * <= MAX_TRANSITION_PROOF_SIZE; the action count/payload/aggregate
 * bounds; checked cursor arithmetic; and exact exhaustion — trailing
 * bytes reject.
 */
inline bool DecodeTransitionEnvelope(const std::span<const unsigned char> data,
                                     ModernTransitionV2& out, std::string& error)
{
    if (data.size() > MAX_TRANSITION_ENVELOPE_SIZE) {
        error = "transition envelope exceeds the size bound";
        return false;
    }
    size_t cursor{0};
    uint16_t version{0};
    if (!detail::ReadU16(data, cursor, version)) {
        error = "truncated transition version";
        return false;
    }
    if (version != TRANSITION_VERSION_V2) {
        error = "unsupported transition version";
        return false;
    }
    ModernTransitionV2 t;

    // ---- inputs: fixed 44-byte records.
    {
        uint64_t count{0};
        if (!detail::ReadCompact(data, cursor, count)) {
            error = "truncated or non-canonical input count";
            return false;
        }
        if (count > (data.size() - cursor) / 44) {
            error = "input count exceeds the available bytes";
            return false;
        }
        t.inputs.reserve(static_cast<size_t>(count));
        for (uint64_t i{0}; i < count; ++i) {
            ModernInput in;
            uint256 raw;
            uint32_t vout{0};
            if (!detail::ReadBytes(data, cursor, raw.begin(), 32) ||
                !detail::ReadU32(data, cursor, vout) ||
                !detail::ReadU32(data, cursor, in.sequence) ||
                !detail::ReadU32(data, cursor, in.proof_index)) {
                error = "truncated input";
                return false;
            }
            in.prevout = COutPoint{Txid::FromUint256(raw), vout};
            t.inputs.push_back(in);
        }
    }
    // ---- outputs: 32+8+2+2+32 fixed + bounded params (>= 77 bytes each).
    {
        uint64_t count{0};
        if (!detail::ReadCompact(data, cursor, count)) {
            error = "truncated or non-canonical output count";
            return false;
        }
        if (count > (data.size() - cursor) / 77) {
            error = "output count exceeds the available bytes";
            return false;
        }
        t.outputs.reserve(static_cast<size_t>(count));
        for (uint64_t i{0}; i < count; ++i) {
            ModernOutput o;
            uint64_t amount{0};
            if (!detail::ReadBytes(data, cursor, o.asset.begin(), 32) ||
                !detail::ReadU64(data, cursor, amount) ||
                !detail::ReadU16(data, cursor, o.policy_type) ||
                !detail::ReadU16(data, cursor, o.policy_version) ||
                !detail::ReadBytes(data, cursor, o.policy_commitment.begin(), 32)) {
                error = "truncated output";
                return false;
            }
            o.amount = static_cast<CAmount>(amount);
            uint64_t params_len{0};
            if (!detail::ReadCompact(data, cursor, params_len)) {
                error = "truncated or non-canonical params length";
                return false;
            }
            if (params_len > MAX_POLICY_PARAMS_SIZE) {
                error = "policy params exceed the bound";
                return false;
            }
            if (data.size() - cursor < params_len) {
                error = "truncated policy params";
                return false;
            }
            o.policy_params.assign(data.begin() + cursor, data.begin() + cursor + params_len);
            cursor += static_cast<size_t>(params_len);
            t.outputs.push_back(std::move(o));
        }
    }
    // ---- proofs: 2+2 header + bounded payload (>= 5 bytes each).
    {
        uint64_t count{0};
        if (!detail::ReadCompact(data, cursor, count)) {
            error = "truncated or non-canonical proof count";
            return false;
        }
        if (count > (data.size() - cursor) / 5) {
            error = "proof count exceeds the available bytes";
            return false;
        }
        t.proofs.reserve(static_cast<size_t>(count));
        for (uint64_t i{0}; i < count; ++i) {
            TransitionProof proof;
            if (!detail::ReadU16(data, cursor, proof.proof_type) ||
                !detail::ReadU16(data, cursor, proof.proof_version)) {
                error = "truncated proof header";
                return false;
            }
            uint64_t len{0};
            if (!detail::ReadCompact(data, cursor, len)) {
                error = "truncated or non-canonical proof payload length";
                return false;
            }
            if (len > MAX_TRANSITION_PROOF_SIZE) {
                error = "proof payload exceeds the bound";
                return false;
            }
            if (data.size() - cursor < len) {
                error = "truncated proof payload";
                return false;
            }
            proof.payload.assign(data.begin() + cursor, data.begin() + cursor + len);
            cursor += static_cast<size_t>(len);
            t.proofs.push_back(std::move(proof));
        }
    }
    // ---- creation actions: the strict section decoder, then exhaustion.
    if (!DecodeCreationActionSection(data, cursor, t.creation_actions, error)) return false;
    if (cursor != data.size()) {
        error = "trailing bytes after the transition envelope";
        return false;
    }
    out = std::move(t);
    return true;
}

//! v2 transition id: explicit v2 domain over the version and the
//! canonical inputs and outputs — blind to proofs AND creation actions.
//! Identical v1/v2 economic bodies never share an id (v1's untagged
//! double-SHA256 id is unchanged).
inline uint256 TransitionIdV2(const ModernTransitionV2& t)
{
    HashWriter h{TaggedHash("B3/MODERN/TX/ID/V2")};
    h << TRANSITION_VERSION_V2 << t.inputs << t.outputs;
    return h.GetSHA256();
}

//! v2 proof-area commitment: explicit v2 domain over the version plus
//! the canonically framed proofs and creation actions (the same
//! standard-grammar bytes the envelope carries).
inline uint256 ProofAreaCommitmentV2(const ModernTransitionV2& t)
{
    HashWriter h{TaggedHash("B3/MODERN/TX/PROOFAREA/V2")};
    h << TRANSITION_VERSION_V2 << t.proofs << t.creation_actions;
    return h.GetSHA256();
}

//! v2 full id: explicit v2 domain over the COMPLETE versioned envelope
//! bytes (version included) — the same canonical representation the
//! wire codec produces.
inline uint256 FullTransitionIdV2(const ModernTransitionV2& t)
{
    HashWriter h{TaggedHash("B3/MODERN/TX/FULL/V2")};
    h << TRANSITION_VERSION_V2 << t.inputs << t.outputs << t.proofs << t.creation_actions;
    return h.GetSHA256();
}

} // namespace modern

#endif // B3COIN_MODERN_PROOF_H
