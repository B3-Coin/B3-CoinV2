// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/certificate.h>

#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/hashes.h>
#include <streams.h>

#include <utility>

namespace flowmesh2 {

static_assert(GetSizeOfCompactSize(MAX_BITMAP_BYTES) == 1, "MAX_ENCODED_BYTES assumes a one-byte bitmap length");

namespace {

bool SeatCountValid(size_t seat_count) { return seat_count >= 1 && seat_count <= MAX_SEATS; }

bool BitSet(const std::vector<unsigned char>& bitmap, size_t index)
{
    return (bitmap[index / 8] >> (index % 8)) & 1U;
}

} // namespace

std::vector<unsigned char> SlotCertificate::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter writer{out, 0};
    writer << header_hash;
    WriteCompactSize(writer, bitmap.size());
    writer.write(MakeByteSpan(bitmap));
    writer << aggregate;
    return out;
}

Result<SlotCertificate> SlotCertificate::Decode(std::span<const unsigned char> bytes)
{
    return DecodeExactWith(bytes, MAX_ENCODED_BYTES, [](SpanReader& reader) {
        SlotCertificate cert;
        reader >> cert.header_hash;
        ReadCappedVector(reader, cert.bitmap, MAX_BITMAP_BYTES);
        reader >> cert.aggregate;
        return cert;
    });
}

uint256 SlotCertificate::Hash() const { return Tagged(HashTag::CERT, Encode()); }

Result<std::vector<uint32_t>> SlotCertificate::SignerIndices(size_t seat_count) const
{
    if (!SeatCountValid(seat_count)) return Error{ErrorCode::INVALID_INPUT, "seat count outside 1..MAX_SEATS"};
    if (bitmap.size() != BitmapBytesFor(seat_count)) {
        return Error{ErrorCode::INVALID_INPUT, "bitmap length does not match the seat count"};
    }
    std::vector<uint32_t> signers;
    for (size_t index{0}; index < bitmap.size() * 8; ++index) {
        if (!BitSet(bitmap, index)) continue;
        if (index >= seat_count) return Error{ErrorCode::INVALID_INPUT, "bitmap marks a seat at or above the seat count"};
        signers.push_back(static_cast<uint32_t>(index));
    }
    return Ok(std::move(signers));
}

Result<std::vector<unsigned char>> SlotCertificate::MakeBitmap(size_t seat_count, std::span<const uint32_t> signers)
{
    if (!SeatCountValid(seat_count)) return Error{ErrorCode::INVALID_INPUT, "seat count outside 1..MAX_SEATS"};
    std::vector<unsigned char> bitmap(BitmapBytesFor(seat_count), 0);
    for (const uint32_t signer : signers) {
        if (signer >= seat_count) return Error{ErrorCode::INVALID_INPUT, "signer index at or above the seat count"};
        if (BitSet(bitmap, signer)) return Error{ErrorCode::INVALID_INPUT, "signer listed twice"};
        bitmap[signer / 8] |= static_cast<unsigned char>(1U << (signer % 8));
    }
    return Ok(std::move(bitmap));
}

} // namespace flowmesh2
