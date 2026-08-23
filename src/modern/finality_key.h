// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_MODERN_FINALITY_KEY_H
#define B3COIN_MODERN_FINALITY_KEY_H

#include <crypto/bls.h>
#include <modern/finality_types.h>
#include <pubkey.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace modern {

/**
 * FINALITY_KEY — validator BLS-binding semantics (plan Commit 4; owner
 * rulings 2026-08-23). Frozen model:
 *
 *   cell (policy 7, metadata, zero value, never a coin):
 *       commitment = validator_key (the BIP340 x-only identity key)
 *       params     = bls_pubkey[48] || seq u32 BE
 *   evidence (MPA record type 5, 244 bytes):
 *       validator_key[32] || bls_pubkey[48] || seq u32 || bip340_sig[64] || pop[96]
 *
 * Rules (CheckFinalityKeyTransition):
 *   - evidence fields must equal the cell's commitment/params;
 *   - bip340_sig = Schnorr by validator_key over
 *       TaggedHash("B3/FINALITY/BIND/V1", domain || validator_key || bls_pubkey || seq)
 *     — REQUIRED for every transition (bind, rotate, revoke);
 *   - bls_pubkey != 0: the key must decode canonically (crypto/bls rules) and
 *     `pop` must be a valid proof of possession for it;
 *   - bls_pubkey == 0 (REVOCATION): the 96-byte pop field must be all zero
 *     and no BLS verification is attempted;
 *   - sequence: the first binding of a validator uses seq = 0; every later
 *     bind/rotate/revoke uses exactly previous seq + 1; duplicates, gaps and
 *     overflow (previous == UINT32_MAX) fail closed;
 *   - one active validator identity per nonzero BLS key: a key currently
 *     bound (non-revoked) to another validator cannot be bound again; a
 *     validator re-binding its own current key is allowed.
 *
 * Effect timing is NOT decided here: a binding becomes effective for finality
 * / block eligibility only through the next validator snapshot (Commit 9
 * reads the binding index at snapshot heights; an already-snapshotted set is
 * immutable — see node/finality_binding_index.h).
 *
 * This header is a pure checker. It is NOT wired into transaction validation
 * yet: the evidence carrier (MPA) arrives in Commit 5, and until then no
 * production transaction can present evidence, so every FINALITY_KEY cell
 * remains invalid (policy 7 inactive, metadata_cell.h). No temporary carrier
 * is provided by design.
 */

using ValidatorKeyBytes = std::array<unsigned char, 32>;
using BlsPubkeyBytes = std::array<unsigned char, BLS_PUBKEY_SIZE>;

inline bool IsZeroBlsKey(std::span<const unsigned char> pk)
{
    for (unsigned char c : pk) if (c != 0) return false;
    return true;
}

//! The derived binding state of one validator key.
struct BindingRecord {
    BlsPubkeyBytes bls_pubkey{};  // all-zero = revoked
    uint32_t seq{0};
    int height{0};                // block height of the transition that set it
    bool IsRevoked() const { return IsZeroBlsKey(bls_pubkey); }
    friend bool operator==(const BindingRecord& a, const BindingRecord& b)
    {
        return a.bls_pubkey == b.bls_pubkey && a.seq == b.seq && a.height == b.height;
    }
};

enum class FinalityKeyCheck {
    OK,
    EVIDENCE_VALIDATOR_MISMATCH, //!< evidence.validator_key != cell commitment
    EVIDENCE_PUBKEY_MISMATCH,    //!< evidence.bls_pubkey != params.bls_pubkey
    EVIDENCE_SEQ_MISMATCH,       //!< evidence.seq != params.seq
    BAD_FIRST_SEQ,               //!< no previous binding and seq != 0
    BAD_SEQ,                     //!< previous exists and seq != previous + 1
    SEQ_OVERFLOW,                //!< previous seq is UINT32_MAX: no further transition
    POP_MUST_BE_ZERO,            //!< revocation with a nonzero pop field
    BAD_BIP340_SIGNATURE,        //!< identity authorization invalid
    BAD_BLS_PUBKEY,              //!< nonzero key does not decode canonically
    BAD_POP,                     //!< nonzero key with invalid (or zero) proof of possession
    BLS_KEY_IN_USE,              //!< nonzero key actively bound to another validator
};

inline const char* FinalityKeyCheckName(const FinalityKeyCheck c)
{
    switch (c) {
    case FinalityKeyCheck::OK: return "ok";
    case FinalityKeyCheck::EVIDENCE_VALIDATOR_MISMATCH: return "evidence-validator-mismatch";
    case FinalityKeyCheck::EVIDENCE_PUBKEY_MISMATCH: return "evidence-pubkey-mismatch";
    case FinalityKeyCheck::EVIDENCE_SEQ_MISMATCH: return "evidence-seq-mismatch";
    case FinalityKeyCheck::BAD_FIRST_SEQ: return "bad-first-seq";
    case FinalityKeyCheck::BAD_SEQ: return "bad-seq";
    case FinalityKeyCheck::SEQ_OVERFLOW: return "seq-overflow";
    case FinalityKeyCheck::POP_MUST_BE_ZERO: return "pop-must-be-zero";
    case FinalityKeyCheck::BAD_BIP340_SIGNATURE: return "bad-bip340-signature";
    case FinalityKeyCheck::BAD_BLS_PUBKEY: return "bad-bls-pubkey";
    case FinalityKeyCheck::BAD_POP: return "bad-pop";
    case FinalityKeyCheck::BLS_KEY_IN_USE: return "bls-key-in-use";
    }
    return "unknown";
}

//! Lookup of the validator currently (actively, non-revoked) holding a BLS key.
using BlsKeyOwnerLookup = std::function<std::optional<ValidatorKeyBytes>(const BlsPubkeyBytes&)>;

/**
 * Validate one FINALITY_KEY transition. Cheap structural and sequence rules
 * run before any cryptography; BIP340 before BLS; the one-owner rule last.
 *
 * @param chain_domain   ModernChainDomain (a bind digest is chain-bound)
 * @param cell_commitment the cell's commitment = validator_key
 * @param params         the cell's params (bls_pubkey || seq)
 * @param evidence       the 244-byte evidence record
 * @param previous       the validator's current binding, if any
 * @param owner_of       active owner of a nonzero BLS key (for the one-owner rule)
 */
inline FinalityKeyCheck CheckFinalityKeyTransition(const uint256& chain_domain, const uint256& cell_commitment,
                                                   const FinalityKeyParams& params, const FinalityKeyEvidence& evidence,
                                                   const std::optional<BindingRecord>& previous,
                                                   const BlsKeyOwnerLookup& owner_of)
{
    // 1. evidence <-> cell consistency
    if (!std::equal(evidence.validator_key.begin(), evidence.validator_key.end(), cell_commitment.begin())) {
        return FinalityKeyCheck::EVIDENCE_VALIDATOR_MISMATCH;
    }
    if (evidence.bls_pubkey != params.bls_pubkey) return FinalityKeyCheck::EVIDENCE_PUBKEY_MISMATCH;
    if (evidence.seq != params.seq) return FinalityKeyCheck::EVIDENCE_SEQ_MISMATCH;
    // 2. sequence rule
    if (!previous) {
        if (params.seq != 0) return FinalityKeyCheck::BAD_FIRST_SEQ;
    } else {
        if (previous->seq == UINT32_MAX) return FinalityKeyCheck::SEQ_OVERFLOW;
        if (params.seq != previous->seq + 1) return FinalityKeyCheck::BAD_SEQ;
    }
    // 3. revocation shape
    const bool revocation{IsZeroBlsKey(params.bls_pubkey)};
    if (revocation && !IsZeroBlsKey(evidence.pop)) return FinalityKeyCheck::POP_MUST_BE_ZERO;
    // 4. identity authorization (BIP340 by validator_key over the bind digest)
    const uint256 digest{FinalityBindDigest(chain_domain, evidence.validator_key, evidence.bls_pubkey, evidence.seq)};
    const XOnlyPubKey identity{std::span<const unsigned char>(evidence.validator_key)};
    if (!identity.VerifySchnorr(digest, evidence.bip340_sig)) return FinalityKeyCheck::BAD_BIP340_SIGNATURE;
    if (revocation) return FinalityKeyCheck::OK; // no BLS verification for a revocation
    // 5. nonzero key: canonical decode + proof of possession
    const auto pk{bls::PublicKey::Decode(evidence.bls_pubkey)};
    if (!pk) return FinalityKeyCheck::BAD_BLS_PUBKEY;
    const auto pop{bls::Signature::Decode(evidence.pop)};
    if (!pop || !bls::VerifyPoP(*pk, *pop)) return FinalityKeyCheck::BAD_POP;
    // 6. one active validator identity per nonzero BLS key
    if (const auto owner{owner_of(params.bls_pubkey)}; owner.has_value()) {
        if (!std::equal(owner->begin(), owner->end(), cell_commitment.begin())) {
            return FinalityKeyCheck::BLS_KEY_IN_USE;
        }
    }
    return FinalityKeyCheck::OK;
}

} // namespace modern

#endif // B3COIN_MODERN_FINALITY_KEY_H
