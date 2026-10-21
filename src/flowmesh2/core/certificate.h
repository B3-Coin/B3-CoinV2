// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_CERTIFICATE_H
#define B3_FLOWMESH2_CORE_CERTIFICATE_H

#include <crypto/bls.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/** Slot certificate (slice R4; ARCHITECTURE.md section 9).
 *
 * A certificate says: the seats marked in `bitmap` signed the header whose
 * Hash() is `header_hash`, and `aggregate` is the BLS aggregate of their
 * signatures over CertificateDigest(header, certificate): the frozen digest
 * for commitments 0/2, or the exact view-bound digest for commitment 3. This file holds the data
 * type only. Checking it (FastAggregateVerify over CommitDigest with the
 * PoP-verified keys of the marked seats, at least Quorum() of them) is
 * b3verify's VerifyCertificate (R11); the k = 1 dev certifier (R10) makes
 * them in the outline. A BLS multisignature certificate is NOT unique: every
 * quorum subset gives another bitmap and aggregate (O-4b).
 *
 * Bitmap: LSB-first. Seat i is bit (i % 8) of byte i / 8, so seat 0 alone is
 * the single byte 0x01. For a set of k seats the bitmap has exactly
 * ceil(k / 8) bytes and every bit at an index >= k is zero.
 *
 * Encoding: header_hash (32) | CompactSize-capped bitmap (at most
 * MAX_SEATS / 8 bytes) | aggregate (96, compressed G2) | optional u64 view.
 * The exact base length is legacy; exactly eight additional bytes carry a
 * view (including zero). Other trailing lengths refuse. The header version
 * decides whether that view must be absent or present; CertificateDigest
 * enforces it before any signature can be accepted. PREPARE proof containers
 * additionally require this metadata to match their signed prepare view.
 * Decode is exact:
 * a longer bitmap or input is OVERSIZE, malformed or trailing bytes DECODE.
 * Decode does not parse the aggregate as a curve point; b3verify does
 * (bls::Signature::Decode). Encode() is total.
 *
 * Hash() = Tagged(CERT, Encode()): the next header's parent_cert_hash.
 */
namespace flowmesh2 {

struct SlotHeaderV2;

inline constexpr size_t MAX_BITMAP_BYTES{(MAX_SEATS + 7) / 8};

//! ceil(seat_count / 8): the bitmap length for a set of seat_count seats.
constexpr size_t BitmapBytesFor(size_t seat_count) { return (seat_count + 7) / 8; }

struct SlotCertificate {
    uint256 header_hash;
    std::vector<unsigned char> bitmap;
    std::array<unsigned char, bls::SIGNATURE_SIZE> aggregate{};
    //! Absent for legacy commitments 0/2. Commitment 3 requires presence,
    //! including view zero. PREPARE certificates bind the same explicit view.
    std::optional<uint64_t> view{};

    friend bool operator==(const SlotCertificate&, const SlotCertificate&) = default;

    static constexpr size_t MAX_LEGACY_ENCODED_BYTES{32 + 1 + MAX_BITMAP_BYTES + bls::SIGNATURE_SIZE};
    static constexpr size_t MAX_ENCODED_BYTES{MAX_LEGACY_ENCODED_BYTES + 8};

    std::vector<unsigned char> Encode() const;
    static Result<SlotCertificate> Decode(std::span<const unsigned char> bytes);
    //! Tagged(CERT, Encode()).
    uint256 Hash() const;

    /** The marked seat indices, ascending. INVALID_INPUT if seat_count is 0
     * or above MAX_SEATS, if the bitmap is not exactly
     * BitmapBytesFor(seat_count) bytes, or if any bit at an index >= seat_count
     * is set.
     */
    Result<std::vector<uint32_t>> SignerIndices(size_t seat_count) const;

    /** The bitmap marking `signers` in a set of seat_count seats. INVALID_INPUT
     * if seat_count is 0 or above MAX_SEATS, or if a signer is >= seat_count or
     * listed twice.
     */
    static Result<std::vector<unsigned char>> MakeBitmap(size_t seat_count, std::span<const uint32_t> signers);
};

//! Exact certificate/header version binding and the appropriate COMMIT digest.
//! This checks the absent/present view format; BLS/quorum/header-hash checks are separate.
Result<uint256> CertificateDigest(const SlotHeaderV2& header, const SlotCertificate& certificate);

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_CERTIFICATE_H
