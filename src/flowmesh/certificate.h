// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_CERTIFICATE_H
#define B3COIN_FLOWMESH_CERTIFICATE_H

#include <hash.h>
#include <key.h>
#include <pubkey.h>
#include <serialize.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace flowmesh {

/**
 * Microblock certificates: finality is enough eligible FN validator
 * attestations over ONE exact microblock hash. A certificate is a
 * SEPARATE object from the microblock — attestations accumulate over an
 * already-fixed identity and never enter it.
 *
 * Cryptography: BIP340 Schnorr over the repository's existing secp256k1
 * primitives. No aggregation; at seat-set scale (<= 1000) individual
 * verification is cheap and BLS is deliberately not introduced.
 *
 * FAULT MODEL (explicit; the numbers are an OWNER DECISION): assume at
 * most `f` of the `k` active seats are Byzantine. Certificate uniqueness
 * per sequence requires that any two certificates share at least one
 * HONEST attester: with threshold `t`, two certificates overlap in at
 * least 2t - k attesters, so uniqueness needs 2t - k > f, i.e.
 *
 *     t >= floor((k + f) / 2) + 1     (MinCertificateThreshold)
 *
 * combined with the honest-attester lock rule of recovery.h (an honest
 * seat never attests two different hashes at one sequence). Liveness
 * additionally requires t <= k - f (a certificate must be formable
 * without the faulty seats). The owner chooses f — and thereby t; the
 * code exposes the relation instead of hiding a number.
 */

//! Attestation digest: what a validator actually signs. Bound to the
//! FlowMesh domain and the sequence so an attestation can never be
//! replayed across domains or positions.
inline uint256 AttestationDigest(const uint256& domain, const uint64_t sequence,
                                 const uint256& microblock_hash)
{
    HashWriter h;
    h << std::string{"b3/flowmesh/attest/v1"} << domain << sequence << microblock_hash;
    return h.GetHash();
}

struct Attestation {
    XOnlyPubKey validator;
    std::array<unsigned char, 64> sig{};

    SERIALIZE_METHODS(Attestation, obj) { READWRITE(obj.validator, obj.sig); }
};

//! Sign an attestation with a seat's operator key (proposer/validator
//! tooling and tests).
inline std::optional<Attestation> SignAttestation(const CKey& key, const uint256& domain,
                                                  const uint64_t sequence,
                                                  const uint256& microblock_hash)
{
    Attestation a;
    a.validator = XOnlyPubKey{key.GetPubKey()};
    const uint256 digest{AttestationDigest(domain, sequence, microblock_hash)};
    if (!key.SignSchnorr(digest, a.sig, nullptr, uint256::ZERO)) return std::nullopt;
    return a;
}

inline bool VerifyAttestation(const Attestation& a, const uint256& domain,
                              const uint64_t sequence, const uint256& microblock_hash)
{
    if (!a.validator.IsFullyValid()) return false;
    return a.validator.VerifySchnorr(AttestationDigest(domain, sequence, microblock_hash), a.sig);
}

struct MicroblockCertificate {
    uint256 microblock_hash;
    uint64_t sequence{0};
    //! Canonical: strictly ascending by validator key, no duplicates.
    std::vector<Attestation> attestations;

    SERIALIZE_METHODS(MicroblockCertificate, obj)
    {
        READWRITE(obj.microblock_hash, obj.sequence, obj.attestations);
    }
};

//! Smallest threshold giving certificate uniqueness under at most `f`
//! Byzantine seats out of `k` (see the fault-model note above). Returns
//! nullopt when no threshold can be both safe and live (k <= 3f is the
//! classic bound: liveness needs t <= k - f).
inline std::optional<uint64_t> MinCertificateThreshold(const uint64_t k, const uint64_t f)
{
    if (k == 0 || f >= k) return std::nullopt;
    const uint64_t t{(k + f) / 2 + 1};
    if (t > k - f) return std::nullopt; // no t is both safe (2t-k > f) and live (t <= k-f)
    return t;
}

enum class CertificateCheck : uint8_t {
    OK = 0,
    EMPTY = 1,
    NON_CANONICAL = 2,   // unsorted or duplicate validators
    NOT_A_SEAT = 3,      // an attester outside the active seat set
    BAD_SIGNATURE = 4,
    BELOW_THRESHOLD = 5,
};

/**
 * Verify a certificate against the active seat set and the threshold.
 * The seat set and threshold come from the caller: seats derive from
 * anchored B3 state (FN seat lifecycle: OWNER DECISION), the threshold
 * from the owner-chosen fault model.
 */
inline CertificateCheck CheckCertificate(const MicroblockCertificate& cert, const uint256& domain,
                                         const std::set<XOnlyPubKey>& seats,
                                         const uint64_t threshold)
{
    if (cert.attestations.empty()) return CertificateCheck::EMPTY;
    for (size_t i{0}; i < cert.attestations.size(); ++i) {
        if (i > 0 && !(cert.attestations[i - 1].validator < cert.attestations[i].validator)) {
            return CertificateCheck::NON_CANONICAL;
        }
        if (seats.count(cert.attestations[i].validator) == 0) {
            return CertificateCheck::NOT_A_SEAT;
        }
        if (!VerifyAttestation(cert.attestations[i], domain, cert.sequence,
                               cert.microblock_hash)) {
            return CertificateCheck::BAD_SIGNATURE;
        }
    }
    if (cert.attestations.size() < threshold) return CertificateCheck::BELOW_THRESHOLD;
    return CertificateCheck::OK;
}

//! Assemble the canonical certificate from gathered attestations for one
//! (sequence, hash): sorts by validator key and drops exact-duplicate
//! keys deterministically (first in sorted order wins).
inline MicroblockCertificate AssembleCertificate(const uint256& microblock_hash,
                                                 const uint64_t sequence,
                                                 std::vector<Attestation> attestations)
{
    MicroblockCertificate cert;
    cert.microblock_hash = microblock_hash;
    cert.sequence = sequence;
    std::stable_sort(attestations.begin(), attestations.end(),
                     [](const Attestation& a, const Attestation& b) {
                         return a.validator < b.validator;
                     });
    for (const Attestation& a : attestations) {
        if (!cert.attestations.empty() && cert.attestations.back().validator == a.validator) {
            continue;
        }
        cert.attestations.push_back(a);
    }
    return cert;
}

/**
 * Equivocation evidence: one seat attesting two DIFFERENT microblock
 * hashes at one sequence. Detectable and recordable by construction;
 * penalties are deliberately NOT defined here (owner-level economics,
 * excluded from v1 by standing rule).
 */
struct AttestationEquivocation {
    uint64_t sequence{0};
    Attestation first;
    Attestation second;
};

inline std::optional<AttestationEquivocation> DetectEquivocation(
    const uint256& domain, const uint64_t sequence, const uint256& hash_a, const Attestation& a,
    const uint256& hash_b, const Attestation& b)
{
    if (!(a.validator == b.validator)) return std::nullopt;
    if (hash_a == hash_b) return std::nullopt;
    if (!VerifyAttestation(a, domain, sequence, hash_a)) return std::nullopt;
    if (!VerifyAttestation(b, domain, sequence, hash_b)) return std::nullopt;
    return AttestationEquivocation{sequence, a, b};
}

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_CERTIFICATE_H
