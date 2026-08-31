// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_BLS_CERTIFICATE_H
#define B3COIN_FLOWMESH_BLS_CERTIFICATE_H

#include <crypto/bls.h>
#include <flowmesh/seat_id.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace flowmesh {

/**
 * Standalone production BLS certificate primitive for FlowMesh.
 *
 * Integration supplies the complete active FN-v2 seat snapshot for the
 * certificate epoch. It must never supply a sampled committee. Members are in
 * the frozen `(SeatId, outpoint)` order; that order defines LSB-first bitmap
 * indices. This layer deliberately does not rewire the older Schnorr
 * certificate, sync, or storage code.
 */
inline constexpr size_t FLOWMESH_MIN_ACTIVE_FN_SEATS{4};
inline constexpr size_t FLOWMESH_MAX_ACTIVE_FN_SEATS{5000};
inline constexpr size_t FLOWMESH_MAX_SIGNER_BITMAP_BYTES{(FLOWMESH_MAX_ACTIVE_FN_SEATS + 7) / 8};
inline constexpr size_t FLOWMESH_BLS_SIGNATURE_SIZE{bls::SIGNATURE_SIZE};

inline constexpr const char* FLOWMESH_BLS_CERTIFICATE_TAG{"B3/FLOWMESH/CERT/V1"};

//! Frozen threshold: floor(2*k/3)+1. Callers must first validate k is 4..5000.
inline constexpr size_t FlowMeshBlsThreshold(const size_t k) { return (2 * k) / 3 + 1; }

//! Exact LSB-first bitmap width for k active FN seats.
inline constexpr size_t FlowMeshSignerBitmapBytes(const size_t k) { return (k + 7) / 8; }

bool FlowMeshSignerBit(std::span<const unsigned char> bitmap, size_t seat_index);
bool IsWellFormedFlowMeshSignerBitmap(std::span<const unsigned char> bitmap, size_t seat_count);

struct BlsSeatBinding {
    COutPoint outpoint;
    std::array<unsigned char, bls::PUBKEY_SIZE> public_key{};
    std::array<unsigned char, bls::SIGNATURE_SIZE> proof_of_possession{};
};

struct ActiveFnBlsSeat {
    SeatId seat_id;
    COutPoint outpoint;
    bls::VerifiedPublicKey key;
};

/**
 * Canonical view of ALL active FN-v2 seats for one anchored epoch.
 * `set_hash` is always recomputed from domain, market, epoch, anchor and every
 * ordered member. It is retained as the signed-context value, never accepted
 * as a caller-selected commitment.
 */
struct ActiveFnBlsSeatSet {
    uint64_t epoch{0};
    uint256 market_id;
    uint64_t anchor_height{0};
    uint256 anchor_hash;
    uint256 set_hash;
    std::vector<ActiveFnBlsSeat> members;

    size_t Size() const { return members.size(); }
};

enum class BlsSeatSetCheck : uint8_t {
    OK = 0,
    TOO_SMALL,
    TOO_LARGE,
    BAD_SEAT_ID,
    NON_CANONICAL_MEMBERS,
    DUPLICATE_KEYS,
    BAD_PUBLIC_KEY,
    BAD_PROOF_OF_POSSESSION,
    BAD_SET_HASH,
};

const char* BlsSeatSetCheckName(BlsSeatSetCheck check);
BlsSeatSetCheck CheckActiveFnBlsSeatSet(const uint256& domain,
                                        const ActiveFnBlsSeatSet& set);

/**
 * Build a set from untrusted on-chain key+PoP bindings. Input must already be
 * in strict `(SeatId, outpoint)` order and is never silently reordered.
 * Counts, order, and duplicate key bytes are rejected before BLS work; the
 * anchored set hash is computed internally after verification.
 */
std::optional<ActiveFnBlsSeatSet> BuildActiveFnBlsSeatSet(
    const uint256& domain, const uint256& market_id, uint64_t epoch,
    uint64_t anchor_height, const uint256& anchor_hash,
    std::span<const BlsSeatBinding> bindings, BlsSeatSetCheck& result);

//! Every field is signed; only epoch/sequence/hash travel in the certificate.
struct BlsCertificateContext {
    uint256 domain;
    uint256 market_id;
    uint64_t seat_epoch{0};
    uint256 seat_set_hash;
    uint64_t sequence{0};
    uint256 microblock_hash;
};

/**
 * Tagged SHA-256 over:
 *   domain || market_id || seat_epoch_be || seat_set_hash ||
 *   sequence_be || microblock_hash
 */
uint256 FlowMeshBlsCertificateDigest(const BlsCertificateContext& context);

struct BlsMicroblockCertificate {
    uint64_t seat_epoch{0};
    uint64_t sequence{0};
    uint256 microblock_hash;
    std::vector<unsigned char> signer_bitmap;
    std::array<unsigned char, FLOWMESH_BLS_SIGNATURE_SIZE> aggregate_signature{};

    friend bool operator==(const BlsMicroblockCertificate& a, const BlsMicroblockCertificate& b)
    {
        return a.seat_epoch == b.seat_epoch && a.sequence == b.sequence &&
               a.microblock_hash == b.microblock_hash &&
               a.signer_bitmap == b.signer_bitmap &&
               a.aggregate_signature == b.aggregate_signature;
    }
};

/**
 * Exact v1 codec (integers fixed-width big-endian):
 *
 *   seat_epoch[8] || sequence[8] || microblock_hash[32] ||
 *   signer_bitmap[ceil(k/8)] || aggregate_signature[96]
 *
 * There is no CompactSize or alternate representation. Decoding checks the
 * 4..5000 bound, exact total length, bitmap width, and zero high bits before
 * allocation. Signature validity is semantic and is checked only after the
 * cheap context/bitmap/threshold gates.
 */
inline constexpr size_t FLOWMESH_BLS_CERTIFICATE_PREFIX_SIZE{8 + 8 + 32};
inline constexpr size_t FLOWMESH_BLS_CERTIFICATE_FIXED_SIZE{
    FLOWMESH_BLS_CERTIFICATE_PREFIX_SIZE + FLOWMESH_BLS_SIGNATURE_SIZE};
inline constexpr size_t FLOWMESH_BLS_CERTIFICATE_MAX_SIZE{
    FLOWMESH_BLS_CERTIFICATE_FIXED_SIZE + FLOWMESH_MAX_SIGNER_BITMAP_BYTES};

std::optional<std::vector<unsigned char>> EncodeBlsMicroblockCertificate(
    const BlsMicroblockCertificate& certificate, size_t seat_count);
std::optional<BlsMicroblockCertificate> DecodeBlsMicroblockCertificate(
    std::span<const unsigned char> bytes, size_t seat_count);

enum class BlsCertificateCheck : uint8_t {
    OK = 0,
    INVALID_SEAT_SET,
    WRONG_SEAT_EPOCH,
    WRONG_SEAT_SET,
    WRONG_SEQUENCE,
    WRONG_MICROBLOCK_HASH,
    MALFORMED_BITMAP,
    NO_SIGNERS,
    BELOW_THRESHOLD,
    MALFORMED_SIGNATURE,
    BAD_SIGNATURE,
};

const char* BlsCertificateCheckName(BlsCertificateCheck check);

//! Cheap set/context/bitmap/quorum checks precede all BLS signature work.
BlsCertificateCheck CheckBlsMicroblockCertificate(
    const BlsMicroblockCertificate& certificate,
    const BlsCertificateContext& expected_context,
    const ActiveFnBlsSeatSet& active_seats);

//! Wallet/node signing helper; refuses a non-seat key or mismatched context.
std::optional<bls::Signature> SignBlsMicroblockCertificate(
    const bls::SecretKey& key, const BlsCertificateContext& context,
    const ActiveFnBlsSeatSet& active_seats);

struct IndexedBlsSignature {
    uint32_t seat_index{0};
    bls::Signature signature;
};

enum class BlsCertificateAssemblyCheck : uint8_t {
    OK = 0,
    INVALID_SEAT_SET,
    WRONG_SEAT_EPOCH,
    WRONG_SEAT_SET,
    EMPTY,
    TOO_MANY_SIGNATURES,
    SEAT_INDEX_OUT_OF_RANGE,
    DUPLICATE_SEAT_INDEX,
    BELOW_THRESHOLD,
    BAD_PARTIAL_SIGNATURE,
    AGGREGATION_FAILED,
};

const char* BlsCertificateAssemblyCheckName(BlsCertificateAssemblyCheck check);

/**
 * Canonical aggregation helper. Partials may arrive in any order; indices are
 * sorted, duplicates rejected, every partial verified against its indexed
 * PoP-verified seat, and one 96-byte aggregate signature emitted.
 */
std::optional<BlsMicroblockCertificate> AssembleBlsMicroblockCertificate(
    const BlsCertificateContext& context, const ActiveFnBlsSeatSet& active_seats,
    std::span<const IndexedBlsSignature> partials,
    BlsCertificateAssemblyCheck& result);

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_BLS_CERTIFICATE_H
