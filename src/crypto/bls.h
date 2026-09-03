// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_CRYPTO_BLS_H
#define B3COIN_CRYPTO_BLS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/**
 * B3 consensus BLS wrapper — the ONLY BLS surface consensus code may use.
 *
 * The scheme is frozen HERE, in B3 consensus, not inherited from library
 * defaults (owner ruling 2026-08-23, implementation plan Commit 2):
 *
 *   curve            BLS12-381
 *   public key       G1, compressed, exactly 48 bytes
 *   signature        G2, compressed, exactly 96 bytes
 *   secret key       32-byte big-endian scalar, 0 < sk < r
 *   hash-to-curve    RFC 9380 hash_to_curve to G2 (XMD:SHA-256, SSWU, RO)
 *   signature DST    BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_   (SIG_DST)
 *   PoP DST          BLS_POP_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_   (POP_DST)
 *   PoP message      the 48 compressed public-key bytes
 *   decode           canonical only: the bytes must re-encode to themselves;
 *                    the point must lie in the prime-order subgroup (G1/G2
 *                    membership); the point at infinity is REJECTED for both
 *                    public keys and signatures
 *   verification     single: e(pk, H(m)) == e(g1, sig)
 *                    aggregate: FastAggregateVerify ONLY — one identical
 *                    32-byte digest for every signer, aggregate public key
 *                    must not be infinity, and ONLY public keys whose binding
 *                    proof of possession has already been verified may be
 *                    aggregated (enforced by the VerifiedPublicKey type)
 *
 * The header exposes no blst types; the implementation keeps blst private.
 * Nothing here is activated by itself: it is a primitive for later commits.
 */
namespace bls {

inline constexpr size_t PUBKEY_SIZE{48};
inline constexpr size_t SIGNATURE_SIZE{96};
inline constexpr size_t SECRET_SIZE{32};
inline constexpr size_t DIGEST_SIZE{32};

inline constexpr std::string_view SIG_DST{"BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_"};
inline constexpr std::string_view POP_DST{"BLS_POP_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_"};

class SecretKey;

//! A canonical, subgroup-checked, non-infinity G1 public key.
class PublicKey
{
public:
    //! Canonical decode of exactly 48 compressed bytes (see header comment).
    static std::optional<PublicKey> Decode(std::span<const unsigned char> bytes);
    const std::array<unsigned char, PUBKEY_SIZE>& Compressed() const { return m_bytes; }
    //! EIP-2537 G1 encoding: two 64-byte, zero-left-padded field elements.
    std::array<unsigned char, 128> Eip2537Uncompressed() const;
    friend bool operator==(const PublicKey& a, const PublicKey& b) { return a.m_bytes == b.m_bytes; }
    friend bool operator<(const PublicKey& a, const PublicKey& b) { return a.m_bytes < b.m_bytes; }

private:
    friend class SecretKey;
    friend struct Impl;
    PublicKey() = default;
    std::array<unsigned char, PUBKEY_SIZE> m_bytes{};
    //! Opaque decoded affine point (two 384-bit field elements).
    alignas(8) std::array<uint64_t, 12> m_point{};
};

//! A canonical, subgroup-checked, non-infinity G2 signature (or PoP).
class Signature
{
public:
    static std::optional<Signature> Decode(std::span<const unsigned char> bytes);
    const std::array<unsigned char, SIGNATURE_SIZE>& Compressed() const { return m_bytes; }
    //! EIP-2537 G2 encoding: four 64-byte, zero-left-padded field elements.
    std::array<unsigned char, 256> Eip2537Uncompressed() const;
    friend bool operator==(const Signature& a, const Signature& b) { return a.m_bytes == b.m_bytes; }

private:
    friend class SecretKey;
    friend struct Impl;
    Signature() = default;
    std::array<unsigned char, SIGNATURE_SIZE> m_bytes{};
    //! Opaque decoded affine point (two Fp2 elements = four field elements).
    alignas(8) std::array<uint64_t, 24> m_point{};
};

//! Secret scalar; zeroed on destruction. Signing only — consensus never holds one.
class SecretKey
{
public:
    //! RFC 9380 / IETF BLS KeyGen from input keying material (>= 32 bytes).
    static std::optional<SecretKey> FromIKM(std::span<const unsigned char> ikm);
    //! Exactly 32 big-endian bytes, rejected unless 0 < sk < r.
    static std::optional<SecretKey> FromBytes(std::span<const unsigned char> bytes);
    ~SecretKey();
    SecretKey(const SecretKey&) = default;
    SecretKey& operator=(const SecretKey&) = default;

    std::array<unsigned char, SECRET_SIZE> Bytes() const { return m_bytes; }
    PublicKey GetPublicKey() const;
    //! Sign a 32-byte digest under SIG_DST.
    Signature Sign(std::span<const unsigned char> digest) const;
    //! Proof of possession: sign this key's compressed public key under POP_DST.
    Signature SignPoP() const;

private:
    SecretKey() = default;
    std::array<unsigned char, SECRET_SIZE> m_bytes{};
};

//! Single-signature verification of a 32-byte digest under SIG_DST.
bool Verify(const PublicKey& pk, std::span<const unsigned char> digest, const Signature& sig);

//! Proof-of-possession verification: `pop` signs pk's compressed bytes under POP_DST.
bool VerifyPoP(const PublicKey& pk, const Signature& pop);

/**
 * A public key whose proof of possession HAS BEEN verified — the only kind
 * that may enter an aggregate. There is no public constructor.
 *
 *  - FromPoP(pk, pop): verifies the PoP; the ONLY path for any key that
 *    arrives from a transaction, a block being validated, a P2P message, an
 *    RPC/wallet input, or any other external source.
 *  - TrustedFromValidatedChain(pk) — INVARIANT (owner, 2026-08-23): permitted
 *    ONLY when reconstructing consensus state from blocks that already passed
 *    full validation, i.e. for a key whose binding PoP was verified by
 *    consensus when its block connected (index rebuild after restart/reorg,
 *    reindex replay of validated blocks). It is NEVER a bypass around
 *    FromPoP for network or user input; a key that has not passed PoP
 *    validation in consensus must never be wrapped by it. Reviewers: every
 *    call site must cite the validated-chain provenance in a comment.
 */
class VerifiedPublicKey
{
public:
    static std::optional<VerifiedPublicKey> FromPoP(const PublicKey& pk, const Signature& pop);
    static VerifiedPublicKey TrustedFromValidatedChain(const PublicKey& pk) { return VerifiedPublicKey{pk}; }
    const PublicKey& Key() const { return m_pk; }

private:
    explicit VerifiedPublicKey(const PublicKey& pk) : m_pk{pk} {}
    PublicKey m_pk;
};

/**
 * FastAggregateVerify: every key signed the SAME 32-byte digest under
 * SIG_DST and `sig` is the aggregate of those signatures. Requires a
 * non-empty key set; the aggregate public key must not be infinity.
 */
bool FastAggregateVerify(std::span<const VerifiedPublicKey> keys, std::span<const unsigned char> digest,
                         const Signature& sig);

//! Aggregate (sum) signatures. Non-empty; an infinity result is rejected.
std::optional<Signature> AggregateSignatures(std::span<const Signature> sigs);

//! Aggregate (sum) PoP-verified public keys, e.g. a validator set's
//! aggregate_pubkey. Non-empty; an infinity result is rejected.
std::optional<PublicKey> AggregatePublicKeys(std::span<const VerifiedPublicKey> keys);

} // namespace bls

#endif // B3COIN_CRYPTO_BLS_H
