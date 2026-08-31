// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/bls_certificate.h>

#include <crypto/common.h>
#include <hash.h>
#include <modern/finality_types.h>

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace flowmesh {

namespace {

bool ValidSeatCount(const size_t count)
{
    return count >= FLOWMESH_MIN_ACTIVE_FN_SEATS && count <= FLOWMESH_MAX_ACTIVE_FN_SEATS;
}

bool MemberBefore(const ActiveFnBlsSeat& a, const ActiveFnBlsSeat& b)
{
    return a.seat_id < b.seat_id || (a.seat_id == b.seat_id && a.outpoint < b.outpoint);
}

} // namespace

bool FlowMeshSignerBit(const std::span<const unsigned char> bitmap, const size_t seat_index)
{
    return modern::SignerBit(bitmap, seat_index);
}

bool IsWellFormedFlowMeshSignerBitmap(const std::span<const unsigned char> bitmap,
                                      const size_t seat_count)
{
    if (!ValidSeatCount(seat_count)) return false;
    // Reuse the frozen LSB-first/high-bit rule, after applying FlowMesh's
    // stricter 5,000-seat bound.
    return modern::IsWellFormedSignerBitmap(bitmap, seat_count);
}

const char* BlsSeatSetCheckName(const BlsSeatSetCheck check)
{
    switch (check) {
    case BlsSeatSetCheck::OK: return "ok";
    case BlsSeatSetCheck::TOO_SMALL: return "too-small";
    case BlsSeatSetCheck::TOO_LARGE: return "too-large";
    case BlsSeatSetCheck::BAD_SEAT_ID: return "bad-seat-id";
    case BlsSeatSetCheck::NON_CANONICAL_MEMBERS: return "non-canonical-members";
    case BlsSeatSetCheck::DUPLICATE_KEYS: return "duplicate-keys";
    case BlsSeatSetCheck::BAD_PUBLIC_KEY: return "bad-public-key";
    case BlsSeatSetCheck::BAD_PROOF_OF_POSSESSION: return "bad-proof-of-possession";
    case BlsSeatSetCheck::BAD_SET_HASH: return "bad-set-hash";
    }
    return "unknown";
}

BlsSeatSetCheck CheckActiveFnBlsSeatSet(const uint256& domain,
                                        const ActiveFnBlsSeatSet& set)
{
    if (set.members.size() < FLOWMESH_MIN_ACTIVE_FN_SEATS) return BlsSeatSetCheck::TOO_SMALL;
    if (set.members.size() > FLOWMESH_MAX_ACTIVE_FN_SEATS) return BlsSeatSetCheck::TOO_LARGE;

    std::set<std::array<unsigned char, bls::PUBKEY_SIZE>> unique_keys;
    std::vector<FlowMeshSeatSetMember> committed_members;
    committed_members.reserve(set.members.size());
    for (size_t i{0}; i < set.members.size(); ++i) {
        const ActiveFnBlsSeat& member{set.members[i]};
        if (member.seat_id != ComputeFlowMeshSeatId(domain, member.outpoint)) {
            return BlsSeatSetCheck::BAD_SEAT_ID;
        }
        if (i > 0 && !MemberBefore(set.members[i - 1], member)) {
            return BlsSeatSetCheck::NON_CANONICAL_MEMBERS;
        }
        const auto key{member.key.Key().Compressed()};
        if (!unique_keys.insert(key).second) {
            return BlsSeatSetCheck::DUPLICATE_KEYS;
        }
        committed_members.push_back({member.seat_id, member.outpoint, key});
    }
    const auto expected_hash{ComputeFlowMeshSeatSetHash(
        domain, set.market_id, set.epoch, set.anchor_height, set.anchor_hash,
        committed_members)};
    if (!expected_hash || *expected_hash != set.set_hash) {
        return BlsSeatSetCheck::BAD_SET_HASH;
    }
    return BlsSeatSetCheck::OK;
}

std::optional<ActiveFnBlsSeatSet> BuildActiveFnBlsSeatSet(
    const uint256& domain, const uint256& market_id, const uint64_t epoch,
    const uint64_t anchor_height, const uint256& anchor_hash,
    const std::span<const BlsSeatBinding> bindings, BlsSeatSetCheck& result)
{
    if (bindings.size() < FLOWMESH_MIN_ACTIVE_FN_SEATS) {
        result = BlsSeatSetCheck::TOO_SMALL;
        return std::nullopt;
    }
    if (bindings.size() > FLOWMESH_MAX_ACTIVE_FN_SEATS) {
        result = BlsSeatSetCheck::TOO_LARGE;
        return std::nullopt;
    }

    std::vector<SeatId> seat_ids;
    seat_ids.reserve(bindings.size());
    std::set<std::array<unsigned char, bls::PUBKEY_SIZE>> unique_keys;
    for (size_t i{0}; i < bindings.size(); ++i) {
        seat_ids.push_back(ComputeFlowMeshSeatId(domain, bindings[i].outpoint));
        if (i > 0) {
            const bool ordered{seat_ids[i - 1] < seat_ids[i] ||
                               (seat_ids[i - 1] == seat_ids[i] &&
                                bindings[i - 1].outpoint < bindings[i].outpoint)};
            if (!ordered) {
                result = BlsSeatSetCheck::NON_CANONICAL_MEMBERS;
                return std::nullopt;
            }
        }
        if (!unique_keys.insert(bindings[i].public_key).second) {
            result = BlsSeatSetCheck::DUPLICATE_KEYS;
            return std::nullopt;
        }
    }

    std::vector<FlowMeshSeatSetMember> committed_members;
    committed_members.reserve(bindings.size());
    for (size_t i{0}; i < bindings.size(); ++i) {
        committed_members.push_back(
            {seat_ids[i], bindings[i].outpoint, bindings[i].public_key});
    }
    const auto set_hash{ComputeFlowMeshSeatSetHash(
        domain, market_id, epoch, anchor_height, anchor_hash,
        committed_members)};
    if (!set_hash) {
        result = BlsSeatSetCheck::BAD_SET_HASH;
        return std::nullopt;
    }

    ActiveFnBlsSeatSet out;
    out.epoch = epoch;
    out.market_id = market_id;
    out.anchor_height = anchor_height;
    out.anchor_hash = anchor_hash;
    out.set_hash = *set_hash;
    out.members.reserve(bindings.size());
    for (size_t i{0}; i < bindings.size(); ++i) {
        const auto public_key{bls::PublicKey::Decode(bindings[i].public_key)};
        if (!public_key) {
            result = BlsSeatSetCheck::BAD_PUBLIC_KEY;
            return std::nullopt;
        }
        const auto pop{bls::Signature::Decode(bindings[i].proof_of_possession)};
        if (!pop) {
            result = BlsSeatSetCheck::BAD_PROOF_OF_POSSESSION;
            return std::nullopt;
        }
        const auto verified{bls::VerifiedPublicKey::FromPoP(*public_key, *pop)};
        if (!verified) {
            result = BlsSeatSetCheck::BAD_PROOF_OF_POSSESSION;
            return std::nullopt;
        }
        out.members.push_back(ActiveFnBlsSeat{seat_ids[i], bindings[i].outpoint, *verified});
    }
    result = BlsSeatSetCheck::OK;
    return out;
}

uint256 FlowMeshBlsCertificateDigest(const BlsCertificateContext& context)
{
    HashWriter writer{TaggedHash(FLOWMESH_BLS_CERTIFICATE_TAG)};
    std::array<unsigned char, 16> numbers{};
    WriteBE64(numbers.data(), context.seat_epoch);
    WriteBE64(numbers.data() + 8, context.sequence);
    writer << std::span<const unsigned char>{context.domain.begin(), 32};
    writer << std::span<const unsigned char>{context.market_id.begin(), 32};
    writer << std::span<const unsigned char>{numbers.data(), 8};
    writer << std::span<const unsigned char>{context.seat_set_hash.begin(), 32};
    writer << std::span<const unsigned char>{numbers.data() + 8, 8};
    writer << std::span<const unsigned char>{context.microblock_hash.begin(), 32};
    return writer.GetSHA256();
}

std::optional<std::vector<unsigned char>> EncodeBlsMicroblockCertificate(
    const BlsMicroblockCertificate& certificate, const size_t seat_count)
{
    if (!IsWellFormedFlowMeshSignerBitmap(certificate.signer_bitmap, seat_count)) {
        return std::nullopt;
    }
    std::vector<unsigned char> out(
        FLOWMESH_BLS_CERTIFICATE_FIXED_SIZE + FlowMeshSignerBitmapBytes(seat_count));
    WriteBE64(out.data(), certificate.seat_epoch);
    WriteBE64(out.data() + 8, certificate.sequence);
    std::copy(certificate.microblock_hash.begin(), certificate.microblock_hash.end(),
              out.begin() + 16);
    auto bitmap_begin{out.begin() + FLOWMESH_BLS_CERTIFICATE_PREFIX_SIZE};
    std::copy(certificate.signer_bitmap.begin(), certificate.signer_bitmap.end(), bitmap_begin);
    std::copy(certificate.aggregate_signature.begin(), certificate.aggregate_signature.end(),
              bitmap_begin + certificate.signer_bitmap.size());
    return out;
}

std::optional<BlsMicroblockCertificate> DecodeBlsMicroblockCertificate(
    const std::span<const unsigned char> bytes, const size_t seat_count)
{
    if (!ValidSeatCount(seat_count)) return std::nullopt;
    const size_t bitmap_size{FlowMeshSignerBitmapBytes(seat_count)};
    if (bytes.size() != FLOWMESH_BLS_CERTIFICATE_FIXED_SIZE + bitmap_size ||
        bytes.size() > FLOWMESH_BLS_CERTIFICATE_MAX_SIZE) {
        return std::nullopt;
    }

    BlsMicroblockCertificate out;
    out.seat_epoch = ReadBE64(bytes.data());
    out.sequence = ReadBE64(bytes.data() + 8);
    std::copy(bytes.begin() + 16, bytes.begin() + FLOWMESH_BLS_CERTIFICATE_PREFIX_SIZE,
              out.microblock_hash.begin());
    size_t offset{FLOWMESH_BLS_CERTIFICATE_PREFIX_SIZE};
    out.signer_bitmap.assign(bytes.begin() + offset, bytes.begin() + offset + bitmap_size);
    if (!IsWellFormedFlowMeshSignerBitmap(out.signer_bitmap, seat_count)) return std::nullopt;
    offset += bitmap_size;
    std::copy(bytes.begin() + offset, bytes.end(), out.aggregate_signature.begin());
    return out;
}

const char* BlsCertificateCheckName(const BlsCertificateCheck check)
{
    switch (check) {
    case BlsCertificateCheck::OK: return "ok";
    case BlsCertificateCheck::INVALID_SEAT_SET: return "invalid-seat-set";
    case BlsCertificateCheck::WRONG_SEAT_EPOCH: return "wrong-seat-epoch";
    case BlsCertificateCheck::WRONG_SEAT_SET: return "wrong-seat-set";
    case BlsCertificateCheck::WRONG_SEQUENCE: return "wrong-sequence";
    case BlsCertificateCheck::WRONG_MICROBLOCK_HASH: return "wrong-microblock-hash";
    case BlsCertificateCheck::MALFORMED_BITMAP: return "malformed-bitmap";
    case BlsCertificateCheck::NO_SIGNERS: return "no-signers";
    case BlsCertificateCheck::BELOW_THRESHOLD: return "below-threshold";
    case BlsCertificateCheck::MALFORMED_SIGNATURE: return "malformed-signature";
    case BlsCertificateCheck::BAD_SIGNATURE: return "bad-signature";
    }
    return "unknown";
}

BlsCertificateCheck CheckBlsMicroblockCertificate(
    const BlsMicroblockCertificate& certificate,
    const BlsCertificateContext& expected_context,
    const ActiveFnBlsSeatSet& active_seats)
{
    if (CheckActiveFnBlsSeatSet(expected_context.domain, active_seats) != BlsSeatSetCheck::OK) {
        return BlsCertificateCheck::INVALID_SEAT_SET;
    }
    if (certificate.seat_epoch != expected_context.seat_epoch ||
        active_seats.epoch != expected_context.seat_epoch) {
        return BlsCertificateCheck::WRONG_SEAT_EPOCH;
    }
    if (active_seats.market_id != expected_context.market_id ||
        active_seats.set_hash != expected_context.seat_set_hash) {
        return BlsCertificateCheck::WRONG_SEAT_SET;
    }
    if (certificate.sequence != expected_context.sequence) return BlsCertificateCheck::WRONG_SEQUENCE;
    if (certificate.microblock_hash != expected_context.microblock_hash) {
        return BlsCertificateCheck::WRONG_MICROBLOCK_HASH;
    }
    if (!IsWellFormedFlowMeshSignerBitmap(certificate.signer_bitmap, active_seats.Size())) {
        return BlsCertificateCheck::MALFORMED_BITMAP;
    }

    std::vector<bls::VerifiedPublicKey> signers;
    signers.reserve(active_seats.Size());
    for (size_t i{0}; i < active_seats.Size(); ++i) {
        if (FlowMeshSignerBit(certificate.signer_bitmap, i)) {
            signers.push_back(active_seats.members[i].key);
        }
    }
    if (signers.empty()) return BlsCertificateCheck::NO_SIGNERS;
    if (signers.size() < FlowMeshBlsThreshold(active_seats.Size())) {
        return BlsCertificateCheck::BELOW_THRESHOLD;
    }

    const auto signature{bls::Signature::Decode(certificate.aggregate_signature)};
    if (!signature) return BlsCertificateCheck::MALFORMED_SIGNATURE;
    const uint256 digest{FlowMeshBlsCertificateDigest(expected_context)};
    if (!bls::FastAggregateVerify(signers, std::span<const unsigned char>{digest.begin(), 32}, *signature)) {
        return BlsCertificateCheck::BAD_SIGNATURE;
    }
    return BlsCertificateCheck::OK;
}

std::optional<bls::Signature> SignBlsMicroblockCertificate(
    const bls::SecretKey& key, const BlsCertificateContext& context,
    const ActiveFnBlsSeatSet& active_seats)
{
    if (CheckActiveFnBlsSeatSet(context.domain, active_seats) != BlsSeatSetCheck::OK ||
        context.seat_epoch != active_seats.epoch ||
        context.market_id != active_seats.market_id ||
        context.seat_set_hash != active_seats.set_hash) {
        return std::nullopt;
    }
    const bls::PublicKey public_key{key.GetPublicKey()};
    const auto seat{std::find_if(active_seats.members.begin(), active_seats.members.end(),
                                 [&](const ActiveFnBlsSeat& member) {
                                     return member.key.Key() == public_key;
                                 })};
    if (seat == active_seats.members.end()) return std::nullopt;
    const uint256 digest{FlowMeshBlsCertificateDigest(context)};
    return key.Sign(std::span<const unsigned char>{digest.begin(), 32});
}

const char* BlsCertificateAssemblyCheckName(const BlsCertificateAssemblyCheck check)
{
    switch (check) {
    case BlsCertificateAssemblyCheck::OK: return "ok";
    case BlsCertificateAssemblyCheck::INVALID_SEAT_SET: return "invalid-seat-set";
    case BlsCertificateAssemblyCheck::WRONG_SEAT_EPOCH: return "wrong-seat-epoch";
    case BlsCertificateAssemblyCheck::WRONG_SEAT_SET: return "wrong-seat-set";
    case BlsCertificateAssemblyCheck::EMPTY: return "empty";
    case BlsCertificateAssemblyCheck::TOO_MANY_SIGNATURES: return "too-many-signatures";
    case BlsCertificateAssemblyCheck::SEAT_INDEX_OUT_OF_RANGE: return "seat-index-out-of-range";
    case BlsCertificateAssemblyCheck::DUPLICATE_SEAT_INDEX: return "duplicate-seat-index";
    case BlsCertificateAssemblyCheck::BELOW_THRESHOLD: return "below-threshold";
    case BlsCertificateAssemblyCheck::BAD_PARTIAL_SIGNATURE: return "bad-partial-signature";
    case BlsCertificateAssemblyCheck::AGGREGATION_FAILED: return "aggregation-failed";
    }
    return "unknown";
}

std::optional<BlsMicroblockCertificate> AssembleBlsMicroblockCertificate(
    const BlsCertificateContext& context, const ActiveFnBlsSeatSet& active_seats,
    const std::span<const IndexedBlsSignature> partials,
    BlsCertificateAssemblyCheck& result)
{
    if (CheckActiveFnBlsSeatSet(context.domain, active_seats) != BlsSeatSetCheck::OK) {
        result = BlsCertificateAssemblyCheck::INVALID_SEAT_SET;
        return std::nullopt;
    }
    if (context.seat_epoch != active_seats.epoch) {
        result = BlsCertificateAssemblyCheck::WRONG_SEAT_EPOCH;
        return std::nullopt;
    }
    if (context.market_id != active_seats.market_id ||
        context.seat_set_hash != active_seats.set_hash) {
        result = BlsCertificateAssemblyCheck::WRONG_SEAT_SET;
        return std::nullopt;
    }
    if (partials.empty()) {
        result = BlsCertificateAssemblyCheck::EMPTY;
        return std::nullopt;
    }
    if (partials.size() > active_seats.Size()) {
        result = BlsCertificateAssemblyCheck::TOO_MANY_SIGNATURES;
        return std::nullopt;
    }

    std::vector<IndexedBlsSignature> ordered{partials.begin(), partials.end()};
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        return a.seat_index < b.seat_index;
    });
    for (size_t i{0}; i < ordered.size(); ++i) {
        if (ordered[i].seat_index >= active_seats.Size()) {
            result = BlsCertificateAssemblyCheck::SEAT_INDEX_OUT_OF_RANGE;
            return std::nullopt;
        }
        if (i > 0 && ordered[i - 1].seat_index == ordered[i].seat_index) {
            result = BlsCertificateAssemblyCheck::DUPLICATE_SEAT_INDEX;
            return std::nullopt;
        }
    }
    if (ordered.size() < FlowMeshBlsThreshold(active_seats.Size())) {
        result = BlsCertificateAssemblyCheck::BELOW_THRESHOLD;
        return std::nullopt;
    }

    const uint256 digest{FlowMeshBlsCertificateDigest(context)};
    std::vector<bls::Signature> signatures;
    signatures.reserve(ordered.size());
    for (const IndexedBlsSignature& partial : ordered) {
        if (!bls::Verify(active_seats.members[partial.seat_index].key.Key(),
                         std::span<const unsigned char>{digest.begin(), 32}, partial.signature)) {
            result = BlsCertificateAssemblyCheck::BAD_PARTIAL_SIGNATURE;
            return std::nullopt;
        }
        signatures.push_back(partial.signature);
    }
    const auto aggregate{bls::AggregateSignatures(signatures)};
    if (!aggregate) {
        result = BlsCertificateAssemblyCheck::AGGREGATION_FAILED;
        return std::nullopt;
    }

    BlsMicroblockCertificate certificate;
    certificate.seat_epoch = context.seat_epoch;
    certificate.sequence = context.sequence;
    certificate.microblock_hash = context.microblock_hash;
    certificate.signer_bitmap.assign(FlowMeshSignerBitmapBytes(active_seats.Size()), 0);
    for (const IndexedBlsSignature& partial : ordered) {
        certificate.signer_bitmap[partial.seat_index / 8] |=
            static_cast<unsigned char>(1u << (partial.seat_index % 8));
    }
    certificate.aggregate_signature = aggregate->Compressed();
    result = BlsCertificateAssemblyCheck::OK;
    return certificate;
}

} // namespace flowmesh
