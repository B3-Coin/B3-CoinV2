// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/seat_evidence.h>

#include <hash.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>

#include <algorithm>
#include <exception>

namespace flowmesh {
namespace {

constexpr const char* SEAT_EVIDENCE_ID_TAG{"B3/FLOWMESH/SEAT-EVIDENCE/V1"};

bool KnownKind(const uint8_t kind)
{
    return kind == static_cast<uint8_t>(SeatEvidenceKind::ATTESTATION_EQUIVOCATION) ||
           kind == static_cast<uint8_t>(SeatEvidenceKind::CERTIFICATE_CONFLICT);
}

//! The per-kind statement shape; no cryptographic check.
bool StatementShape(const SeatEvidenceKind kind, const SeatEvidenceStatement& statement)
{
    if (kind == SeatEvidenceKind::ATTESTATION_EQUIVOCATION) return statement.signer_bitmap.empty();
    return statement.seat_index == 0 && !statement.signer_bitmap.empty() &&
           statement.signer_bitmap.size() <= FLOWMESH_MAX_SIGNER_BITMAP_BYTES;
}

bool Shape(const SeatEvidence& evidence)
{
    return KnownKind(static_cast<uint8_t>(evidence.kind)) && !evidence.domain.IsNull() &&
           !evidence.market_id.IsNull() && StatementShape(evidence.kind, evidence.first) &&
           StatementShape(evidence.kind, evidence.second) &&
           evidence.seat_public_key.has_value() == (evidence.kind == SeatEvidenceKind::ATTESTATION_EQUIVOCATION);
}

template <typename Stream>
void WriteStatement(Stream& s, const SeatEvidenceStatement& statement)
{
    s << statement.seat_epoch << statement.seat_set_hash << statement.sequence << statement.microblock_hash
      << statement.seat_index << statement.signer_bitmap << statement.signature;
}

template <typename Stream>
void ReadStatement(Stream& s, SeatEvidenceStatement& statement)
{
    s >> statement.seat_epoch >> statement.seat_set_hash >> statement.sequence >> statement.microblock_hash >>
        statement.seat_index;
    // Bound the bitmap before reading it.
    const uint64_t size{ReadCompactSize(s)};
    if (size > FLOWMESH_MAX_SIGNER_BITMAP_BYTES) throw std::ios_base::failure("seat evidence bitmap too large");
    statement.signer_bitmap.resize(size);
    s >> std::span{statement.signer_bitmap};
    s >> statement.signature;
}

std::vector<unsigned char> StatementBytes(const SeatEvidenceStatement& statement)
{
    DataStream s;
    WriteStatement(s, statement);
    return {UCharCast(s.data()), UCharCast(s.data()) + s.size()};
}

BlsMicroblockCertificate Certificate(const SeatEvidenceStatement& statement)
{
    BlsMicroblockCertificate out;
    out.seat_epoch = statement.seat_epoch;
    out.sequence = statement.sequence;
    out.microblock_hash = statement.microblock_hash;
    out.signer_bitmap = statement.signer_bitmap;
    out.aggregate_signature = statement.signature;
    return out;
}

bool SeatSetMatches(const SeatEvidence& evidence, const SeatEvidenceStatement& statement,
                    const ActiveFnBlsSeatSet& seats)
{
    return seats.market_id == evidence.market_id && seats.epoch == statement.seat_epoch &&
           seats.set_hash == statement.seat_set_hash &&
           CheckActiveFnBlsSeatSet(evidence.domain, seats) == BlsSeatSetCheck::OK;
}

bool StatementSignatureValid(const SeatEvidence& evidence, const SeatEvidenceStatement& statement,
                             const ActiveFnBlsSeatSet& seats)
{
    const auto context{evidence.Context(statement)};
    if (evidence.kind == SeatEvidenceKind::CERTIFICATE_CONFLICT) {
        return CheckBlsMicroblockCertificate(Certificate(statement), context, seats) == BlsCertificateCheck::OK;
    }
    if (statement.seat_index >= seats.Size()) return false;
    const auto signature{bls::Signature::Decode(statement.signature)};
    if (!signature) return false;
    const uint256 digest{FlowMeshBlsCertificateDigest(context)};
    return bls::Verify(seats.members[statement.seat_index].key.Key(),
                       std::span<const unsigned char>{digest.begin(), 32}, *signature);
}

} // namespace

const char* SeatEvidenceKindName(const SeatEvidenceKind kind)
{
    switch (kind) {
    case SeatEvidenceKind::ATTESTATION_EQUIVOCATION: return "attestation_equivocation";
    case SeatEvidenceKind::CERTIFICATE_CONFLICT: return "certificate_conflict";
    }
    return "unknown";
}

const char* SeatEvidenceCheckName(const SeatEvidenceCheck check)
{
    switch (check) {
    case SeatEvidenceCheck::OK: return "ok";
    case SeatEvidenceCheck::MALFORMED: return "malformed";
    case SeatEvidenceCheck::NOT_CONFLICTING: return "not_conflicting";
    case SeatEvidenceCheck::WRONG_SEAT_SET: return "wrong_seat_set";
    case SeatEvidenceCheck::WRONG_SEAT_KEY: return "wrong_seat_key";
    case SeatEvidenceCheck::BAD_FIRST_SIGNATURE: return "bad_first_signature";
    case SeatEvidenceCheck::BAD_SECOND_SIGNATURE: return "bad_second_signature";
    }
    return "unknown";
}

BlsCertificateContext SeatEvidence::Context(const SeatEvidenceStatement& statement) const
{
    return BlsCertificateContext{domain, market_id, statement.seat_epoch, statement.seat_set_hash,
                                 statement.sequence, statement.microblock_hash};
}

uint256 SeatEvidence::Id() const
{
    auto a{StatementBytes(first)};
    auto b{StatementBytes(second)};
    if (b < a) std::swap(a, b);
    HashWriter writer{TaggedHash(SEAT_EVIDENCE_ID_TAG)};
    writer << static_cast<uint8_t>(kind) << domain << market_id << a << b;
    return writer.GetSHA256();
}

std::optional<std::vector<unsigned char>> EncodeSeatEvidence(const SeatEvidence& evidence)
{
    if (!Shape(evidence)) return std::nullopt;
    DataStream s;
    s << SEAT_EVIDENCE_VERSION << static_cast<uint8_t>(evidence.kind) << evidence.domain << evidence.market_id;
    WriteStatement(s, evidence.first);
    WriteStatement(s, evidence.second);
    if (evidence.seat_public_key) s << *evidence.seat_public_key;
    if (s.size() > SEAT_EVIDENCE_MAX_BYTES) return std::nullopt;
    return std::vector<unsigned char>{UCharCast(s.data()), UCharCast(s.data()) + s.size()};
}

std::optional<SeatEvidence> DecodeSeatEvidence(const std::span<const unsigned char> bytes)
{
    if (bytes.empty() || bytes.size() > SEAT_EVIDENCE_MAX_BYTES) return std::nullopt;
    try {
        DataStream s{bytes};
        uint8_t version{0};
        uint8_t kind{0};
        s >> version >> kind;
        if (version != SEAT_EVIDENCE_VERSION || !KnownKind(kind)) return std::nullopt;
        SeatEvidence out;
        out.kind = static_cast<SeatEvidenceKind>(kind);
        s >> out.domain >> out.market_id;
        ReadStatement(s, out.first);
        ReadStatement(s, out.second);
        if (out.kind == SeatEvidenceKind::ATTESTATION_EQUIVOCATION) {
            out.seat_public_key.emplace();
            s >> *out.seat_public_key;
        }
        if (!s.empty() || !Shape(out)) return std::nullopt;
        // Canonical: the bytes re-encode exactly (CompactSize included).
        const auto again{EncodeSeatEvidence(out)};
        if (!again || !std::equal(again->begin(), again->end(), bytes.begin(), bytes.end())) return std::nullopt;
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

SeatEvidenceCheck CheckSeatEvidence(const SeatEvidence& evidence, const ActiveFnBlsSeatSet& first_seats,
                                    const ActiveFnBlsSeatSet& second_seats)
{
    if (!Shape(evidence)) return SeatEvidenceCheck::MALFORMED;
    const auto& a{evidence.first};
    const auto& b{evidence.second};
    if (a.sequence != b.sequence || a.microblock_hash == b.microblock_hash) return SeatEvidenceCheck::NOT_CONFLICTING;
    if (evidence.kind == SeatEvidenceKind::ATTESTATION_EQUIVOCATION &&
        (a.seat_index != b.seat_index || a.seat_epoch != b.seat_epoch || a.seat_set_hash != b.seat_set_hash)) {
        return SeatEvidenceCheck::NOT_CONFLICTING;
    }
    if (!SeatSetMatches(evidence, a, first_seats) || !SeatSetMatches(evidence, b, second_seats)) {
        return SeatEvidenceCheck::WRONG_SEAT_SET;
    }
    if (evidence.kind == SeatEvidenceKind::ATTESTATION_EQUIVOCATION &&
        (a.seat_index >= first_seats.Size() ||
         *evidence.seat_public_key != first_seats.members[a.seat_index].key.Key().Compressed())) {
        return SeatEvidenceCheck::WRONG_SEAT_KEY;
    }
    if (!StatementSignatureValid(evidence, a, first_seats)) return SeatEvidenceCheck::BAD_FIRST_SIGNATURE;
    if (!StatementSignatureValid(evidence, b, second_seats)) return SeatEvidenceCheck::BAD_SECOND_SIGNATURE;
    return SeatEvidenceCheck::OK;
}

SeatEvidenceStatement SeatAttestationStatement(const BlsCertificateContext& context, const uint32_t seat_index,
                                               const bls::Signature& signature)
{
    SeatEvidenceStatement out;
    out.seat_epoch = context.seat_epoch;
    out.seat_set_hash = context.seat_set_hash;
    out.sequence = context.sequence;
    out.microblock_hash = context.microblock_hash;
    out.seat_index = seat_index;
    out.signature = signature.Compressed();
    return out;
}

SeatEvidenceStatement SeatCertificateStatement(const BlsMicroblockCertificate& certificate,
                                               const uint256& seat_set_hash)
{
    SeatEvidenceStatement out;
    out.seat_epoch = certificate.seat_epoch;
    out.seat_set_hash = seat_set_hash;
    out.sequence = certificate.sequence;
    out.microblock_hash = certificate.microblock_hash;
    out.signer_bitmap = certificate.signer_bitmap;
    out.signature = certificate.aggregate_signature;
    return out;
}

std::vector<uint32_t> SeatEvidenceDoubleSigners(const SeatEvidence& evidence)
{
    std::vector<uint32_t> out;
    const auto& a{evidence.first};
    const auto& b{evidence.second};
    if (evidence.kind != SeatEvidenceKind::CERTIFICATE_CONFLICT || a.seat_epoch != b.seat_epoch ||
        a.seat_set_hash != b.seat_set_hash || a.signer_bitmap.size() != b.signer_bitmap.size()) {
        return out;
    }
    for (size_t byte{0}; byte < a.signer_bitmap.size(); ++byte) {
        const unsigned both{static_cast<unsigned>(a.signer_bitmap[byte] & b.signer_bitmap[byte])};
        for (unsigned bit{0}; bit < 8; ++bit) {
            if (both & (1U << bit)) out.push_back(static_cast<uint32_t>(byte * 8 + bit));
        }
    }
    return out;
}

} // namespace flowmesh
