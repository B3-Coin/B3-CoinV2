// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_TEST_UTIL_KEYS_H
#define B3_FLOWMESH2_TEST_UTIL_KEYS_H

#include <crypto/bls.h>
#include <crypto/sha256.h>
#include <key.h>
#include <pubkey.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <optional>
#include <span>
#include <string_view>

/** Deterministic TEST keys for test_flowmesh2 (slice R4).
 *
 * Every key is derived from a label: the secret (BIP340) or the input keying
 * material (BLS, RFC 9380 KeyGen through bls::SecretKey::FromIKM) is
 * SHA256(label). These are TEST keys only; no real network value or key is
 * ever derived here. CKey comes from bitcoin_common, which only the test
 * executable links (the fm2 libraries never do). Signing with CKey needs the
 * ECC_Context global fixture of test/main.cpp.
 */
namespace flowmesh2::test {

//! SHA256(label), as b32(label) in the Python golden scripts.
inline uint256 LabelHash(std::string_view label)
{
    uint256 out;
    CSHA256{}.Write(reinterpret_cast<const unsigned char*>(label.data()), label.size()).Finalize(out.data());
    return out;
}

//! Compressed secp256k1 key whose secret is SHA256(label).
inline CKey KeyFromLabel(std::string_view label)
{
    const uint256 secret{LabelHash(label)};
    CKey key;
    key.Set(secret.begin(), secret.end(), /*fCompressedIn=*/true);
    BOOST_REQUIRE_MESSAGE(key.IsValid(), "SHA256(label) is not a valid secp256k1 secret");
    return key;
}

//! The x-only public key of KeyFromLabel(label).
inline XOnlyPubKey XOnlyFromLabel(std::string_view label) { return XOnlyPubKey{KeyFromLabel(label).GetPubKey()}; }

//! BIP340 signature of `digest` with `aux` as the auxiliary randomness.
inline std::array<unsigned char, 64> SignDigest(const CKey& key, const uint256& digest, const uint256& aux)
{
    std::array<unsigned char, 64> sig{};
    BOOST_REQUIRE(key.SignSchnorr(digest, sig, /*merkle_root=*/nullptr, aux));
    return sig;
}

//! BLS secret key from IKM = SHA256(label).
inline bls::SecretKey BlsKeyFromLabel(std::string_view label)
{
    const uint256 ikm{LabelHash(label)};
    std::optional<bls::SecretKey> key{bls::SecretKey::FromIKM(std::span<const unsigned char>{ikm.begin(), ikm.end()})};
    BOOST_REQUIRE(key.has_value());
    return *key;
}

} // namespace flowmesh2::test

#endif // B3_FLOWMESH2_TEST_UTIL_KEYS_H
