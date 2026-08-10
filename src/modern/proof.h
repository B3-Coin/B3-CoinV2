// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_PROOF_H
#define B3COIN_MODERN_PROOF_H

#include <crypto/sha256.h>
#include <hash.h>
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
                                        const TransitionProof& proof)
{
    if (!IsActivatedPolicy(prev_output.policy_type, prev_output.policy_version)) {
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
                                         const ModernTransition& t)
{
    if (t.proofs.size() != t.inputs.size() || prev_outputs.size() != t.inputs.size()) {
        return ProofCheck::PROOF_COUNT_MISMATCH;
    }
    for (size_t i{0}; i < t.inputs.size(); ++i) {
        if (t.inputs[i].proof_index != i) return ProofCheck::PROOF_REF_NONCANONICAL;
        const ProofCheck result{VerifyTransitionProof(prev_outputs[i], t.proofs[i])};
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

} // namespace modern

#endif // B3COIN_MODERN_PROOF_H
