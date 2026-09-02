// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <crypto/bls.h>

#include <support/cleanse.h>

#include <blst.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace bls {

// The opaque storage in the public types must exactly mirror blst's affine
// point layouts so that decoded points are cached without exposing blst.
static_assert(sizeof(blst_p1_affine) == sizeof(uint64_t) * 12, "blst_p1_affine layout");
static_assert(sizeof(blst_p2_affine) == sizeof(uint64_t) * 24, "blst_p2_affine layout");
static_assert(alignof(blst_p1_affine) <= 8 && alignof(blst_p2_affine) <= 8, "point alignment");

struct Impl {
    static const blst_p1_affine* P1(const PublicKey& pk) { return reinterpret_cast<const blst_p1_affine*>(pk.m_point.data()); }
    static const blst_p2_affine* P2(const Signature& s) { return reinterpret_cast<const blst_p2_affine*>(s.m_point.data()); }

    //! Canonical G1 decode: uncompress, re-compress must match, in G1, not infinity.
    static std::optional<PublicKey> DecodeP1(std::span<const unsigned char> bytes)
    {
        if (bytes.size() != PUBKEY_SIZE) return std::nullopt;
        blst_p1_affine aff;
        if (blst_p1_uncompress(&aff, bytes.data()) != BLST_SUCCESS) return std::nullopt;
        if (blst_p1_affine_is_inf(&aff)) return std::nullopt;
        if (!blst_p1_affine_in_g1(&aff)) return std::nullopt;
        unsigned char again[PUBKEY_SIZE];
        blst_p1_affine_compress(again, &aff);
        if (std::memcmp(again, bytes.data(), PUBKEY_SIZE) != 0) return std::nullopt; // non-canonical encoding
        PublicKey pk;
        std::copy(bytes.begin(), bytes.end(), pk.m_bytes.begin());
        std::memcpy(pk.m_point.data(), &aff, sizeof(aff));
        return pk;
    }

    //! Canonical G2 decode with the same rules.
    static std::optional<Signature> DecodeP2(std::span<const unsigned char> bytes)
    {
        if (bytes.size() != SIGNATURE_SIZE) return std::nullopt;
        blst_p2_affine aff;
        if (blst_p2_uncompress(&aff, bytes.data()) != BLST_SUCCESS) return std::nullopt;
        if (blst_p2_affine_is_inf(&aff)) return std::nullopt;
        if (!blst_p2_affine_in_g2(&aff)) return std::nullopt;
        unsigned char again[SIGNATURE_SIZE];
        blst_p2_affine_compress(again, &aff);
        if (std::memcmp(again, bytes.data(), SIGNATURE_SIZE) != 0) return std::nullopt;
        Signature s;
        std::copy(bytes.begin(), bytes.end(), s.m_bytes.begin());
        std::memcpy(s.m_point.data(), &aff, sizeof(aff));
        return s;
    }

    static std::optional<Signature> FromP2(const blst_p2& p)
    {
        blst_p2_affine aff;
        blst_p2_to_affine(&aff, &p);
        unsigned char bytes[SIGNATURE_SIZE];
        blst_p2_affine_compress(bytes, &aff);
        return DecodeP2(bytes); // applies every rule, incl. infinity rejection
    }

    static std::optional<PublicKey> FromP1(const blst_p1& p)
    {
        blst_p1_affine aff;
        blst_p1_to_affine(&aff, &p);
        unsigned char bytes[PUBKEY_SIZE];
        blst_p1_affine_compress(bytes, &aff);
        return DecodeP1(bytes);
    }

    static bool CoreVerify(const blst_p1_affine* pk, const blst_p2_affine* sig, std::span<const unsigned char> msg,
                           std::string_view dst)
    {
        return blst_core_verify_pk_in_g1(pk, sig, /*hash_or_encode=*/true, msg.data(), msg.size(),
                                         reinterpret_cast<const unsigned char*>(dst.data()), dst.size(),
                                         nullptr, 0) == BLST_SUCCESS;
    }
};

std::optional<PublicKey> PublicKey::Decode(std::span<const unsigned char> bytes) { return Impl::DecodeP1(bytes); }
std::optional<Signature> Signature::Decode(std::span<const unsigned char> bytes) { return Impl::DecodeP2(bytes); }

std::array<unsigned char, 128> PublicKey::Eip2537Uncompressed() const
{
    // blst serializes x || y as two canonical 48-byte big-endian field
    // elements. EIP-2537 places each element in a 64-byte slot.
    std::array<unsigned char, 96> serialized{};
    blst_p1_affine_serialize(serialized.data(), Impl::P1(*this));
    std::array<unsigned char, 128> out{};
    std::copy_n(serialized.begin(), 48, out.begin() + 16);
    std::copy_n(serialized.begin() + 48, 48, out.begin() + 80);
    return out;
}

std::array<unsigned char, 256> Signature::Eip2537Uncompressed() const
{
    // blst serializes Fp2 as x.c1 || x.c0 || y.c1 || y.c0. EIP-2537's
    // precompile input is x.c0 || x.c1 || y.c0 || y.c1, with every field
    // element left-padded to 64 bytes.
    std::array<unsigned char, 192> serialized{};
    blst_p2_affine_serialize(serialized.data(), Impl::P2(*this));
    std::array<unsigned char, 256> out{};
    static constexpr std::array<size_t, 4> EIP_ORDER{1, 0, 3, 2};
    for (size_t limb{0}; limb < EIP_ORDER.size(); ++limb) {
        std::copy_n(serialized.begin() + EIP_ORDER[limb] * 48, 48,
                    out.begin() + limb * 64 + 16);
    }
    return out;
}

SecretKey::~SecretKey() { memory_cleanse(m_bytes.data(), m_bytes.size()); }

std::optional<SecretKey> SecretKey::FromIKM(std::span<const unsigned char> ikm)
{
    if (ikm.size() < 32) return std::nullopt;
    blst_scalar sk;
    blst_keygen(&sk, ikm.data(), ikm.size(), nullptr, 0);
    if (!blst_sk_check(&sk)) return std::nullopt;
    SecretKey out;
    blst_bendian_from_scalar(out.m_bytes.data(), &sk);
    memory_cleanse(&sk, sizeof(sk));
    return out;
}

std::optional<SecretKey> SecretKey::FromBytes(std::span<const unsigned char> bytes)
{
    if (bytes.size() != SECRET_SIZE) return std::nullopt;
    blst_scalar sk;
    blst_scalar_from_bendian(&sk, bytes.data());
    if (!blst_sk_check(&sk)) { memory_cleanse(&sk, sizeof(sk)); return std::nullopt; } // zero or >= r
    SecretKey out;
    std::copy(bytes.begin(), bytes.end(), out.m_bytes.begin());
    memory_cleanse(&sk, sizeof(sk));
    return out;
}

PublicKey SecretKey::GetPublicKey() const
{
    blst_scalar sk;
    blst_scalar_from_bendian(&sk, m_bytes.data());
    blst_p1 pk;
    blst_sk_to_pk_in_g1(&pk, &sk);
    memory_cleanse(&sk, sizeof(sk));
    auto out{Impl::FromP1(pk)};
    // A checked secret key (0 < sk < r) always yields a canonical non-infinity G1 point.
    return *out;
}

static Signature SignWithDst(const std::array<unsigned char, SECRET_SIZE>& sk_bytes, std::span<const unsigned char> msg,
                             std::string_view dst)
{
    blst_scalar sk;
    blst_scalar_from_bendian(&sk, sk_bytes.data());
    blst_p2 h;
    blst_hash_to_g2(&h, msg.data(), msg.size(), reinterpret_cast<const unsigned char*>(dst.data()), dst.size(), nullptr, 0);
    blst_p2 sig;
    blst_sign_pk_in_g1(&sig, &h, &sk);
    memory_cleanse(&sk, sizeof(sk));
    // sk * H(m) is infinity only if H(m) is (negligible); FromP2 rejects that case,
    // and a std::optional deref on it would be a programming error surfaced loudly.
    return *Impl::FromP2(sig);
}

Signature SecretKey::Sign(std::span<const unsigned char> digest) const
{
    return SignWithDst(m_bytes, digest, SIG_DST);
}

Signature SecretKey::SignPoP() const
{
    const PublicKey pk{GetPublicKey()};
    return SignWithDst(m_bytes, pk.Compressed(), POP_DST);
}

bool Verify(const PublicKey& pk, std::span<const unsigned char> digest, const Signature& sig)
{
    if (digest.size() != DIGEST_SIZE) return false;
    return Impl::CoreVerify(Impl::P1(pk), Impl::P2(sig), digest, SIG_DST);
}

bool VerifyPoP(const PublicKey& pk, const Signature& pop)
{
    return Impl::CoreVerify(Impl::P1(pk), Impl::P2(pop), pk.Compressed(), POP_DST);
}

std::optional<VerifiedPublicKey> VerifiedPublicKey::FromPoP(const PublicKey& pk, const Signature& pop)
{
    if (!VerifyPoP(pk, pop)) return std::nullopt;
    return VerifiedPublicKey{pk};
}

bool FastAggregateVerify(std::span<const VerifiedPublicKey> keys, std::span<const unsigned char> digest,
                         const Signature& sig)
{
    if (keys.empty() || digest.size() != DIGEST_SIZE) return false;
    std::vector<const blst_p1_affine*> pts;
    pts.reserve(keys.size());
    for (const auto& k : keys) pts.push_back(Impl::P1(k.Key()));
    blst_p1 sum;
    blst_p1s_add(&sum, pts.data(), pts.size());
    blst_p1_affine agg;
    blst_p1_to_affine(&agg, &sum);
    if (blst_p1_affine_is_inf(&agg)) return false;
    return Impl::CoreVerify(&agg, Impl::P2(sig), digest, SIG_DST);
}

std::optional<PublicKey> AggregatePublicKeys(std::span<const VerifiedPublicKey> keys)
{
    if (keys.empty()) return std::nullopt;
    std::vector<const blst_p1_affine*> pts;
    pts.reserve(keys.size());
    for (const auto& k : keys) pts.push_back(Impl::P1(k.Key()));
    blst_p1 sum;
    blst_p1s_add(&sum, pts.data(), pts.size());
    return Impl::FromP1(sum); // canonical re-decode; infinity rejected
}

std::optional<Signature> AggregateSignatures(std::span<const Signature> sigs)
{
    if (sigs.empty()) return std::nullopt;
    std::vector<const blst_p2_affine*> pts;
    pts.reserve(sigs.size());
    for (const auto& s : sigs) pts.push_back(Impl::P2(s));
    blst_p2 sum;
    blst_p2s_add(&sum, pts.data(), pts.size());
    return Impl::FromP2(sum); // canonical re-decode; infinity rejected
}

} // namespace bls
