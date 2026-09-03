// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/production_wire.h>

#include <crypto/common.h>
#include <streams.h>

#include <algorithm>
#include <exception>
#include <limits>

namespace flowmesh {

std::optional<std::vector<unsigned char>> EncodeProductionActionPayload(
    const Action& action)
{
    if (!action.ShapeIsCanonical()) return std::nullopt;
    std::vector<unsigned char> out;
    VectorWriter writer{out, 0};
    writer << action;
    if (out.empty() || out.size() > FLOWMESH_ACTION_MAX_BYTES) {
        return std::nullopt;
    }
    return out;
}

std::optional<Action> DecodeProductionActionPayload(
    const std::span<const unsigned char> payload)
{
    if (payload.empty() || payload.size() > FLOWMESH_ACTION_MAX_BYTES) {
        return std::nullopt;
    }
    try {
        SpanReader reader{payload};
        Action out;
        reader >> out;
        if (!reader.empty() || !out.ShapeIsCanonical()) return std::nullopt;
        const auto canonical{EncodeProductionActionPayload(out)};
        if (!canonical || !std::equal(canonical->begin(), canonical->end(),
                                      payload.begin(), payload.end())) {
            return std::nullopt;
        }
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<std::vector<unsigned char>> EncodeProductionProposalPayload(
    const ProductionProposalEnvelope& proposal)
{
    const auto entry{EncodeProductionEntry(proposal.entry)};
    if (!entry || entry->size() > FLOWMESH_PROPOSAL_MAX_BYTES -
                                   FLOWMESH_PROPOSAL_PAYLOAD_PREFIX_SIZE) {
        return std::nullopt;
    }
    std::vector<unsigned char> out(
        FLOWMESH_PROPOSAL_PAYLOAD_PREFIX_SIZE + entry->size());
    WriteBE32(out.data(), proposal.round);
    WriteBE32(out.data() + 4, proposal.proposer_seat_index);
    std::copy(proposal.proposer_signature.begin(),
              proposal.proposer_signature.end(), out.begin() + 8);
    std::copy(entry->begin(), entry->end(),
              out.begin() + FLOWMESH_PROPOSAL_PAYLOAD_PREFIX_SIZE);
    return out;
}

std::optional<ProductionProposalEnvelope> DecodeProductionProposalPayload(
    const std::span<const unsigned char> payload)
{
    if (payload.size() <= FLOWMESH_PROPOSAL_PAYLOAD_PREFIX_SIZE ||
        payload.size() > FLOWMESH_PROPOSAL_MAX_BYTES) {
        return std::nullopt;
    }
    const auto entry{DecodeProductionEntry(
        payload.subspan(FLOWMESH_PROPOSAL_PAYLOAD_PREFIX_SIZE))};
    if (!entry) return std::nullopt;
    ProductionProposalEnvelope out;
    out.entry = *entry;
    out.round = ReadBE32(payload.data());
    out.proposer_seat_index = ReadBE32(payload.data() + 4);
    std::copy(payload.begin() + 8,
              payload.begin() + FLOWMESH_PROPOSAL_PAYLOAD_PREFIX_SIZE,
              out.proposer_signature.begin());
    const auto canonical{EncodeProductionProposalPayload(out)};
    if (!canonical || !std::equal(canonical->begin(), canonical->end(),
                                  payload.begin(), payload.end())) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::vector<unsigned char>> EncodeProductionAttestationPayload(
    const IndexedBlsSignature& attestation)
{
    if (attestation.seat_index >= FLOWMESH_MAX_ACTIVE_FN_SEATS) {
        return std::nullopt;
    }
    std::vector<unsigned char> out(FLOWMESH_ATTESTATION_BYTES);
    WriteBE32(out.data(), attestation.seat_index);
    const auto signature{attestation.signature.Compressed()};
    std::copy(signature.begin(), signature.end(), out.begin() + 4);
    return out;
}

std::optional<IndexedBlsSignature> DecodeProductionAttestationPayload(
    const std::span<const unsigned char> payload)
{
    if (payload.size() != FLOWMESH_ATTESTATION_BYTES) return std::nullopt;
    const uint32_t seat_index{ReadBE32(payload.data())};
    if (seat_index >= FLOWMESH_MAX_ACTIVE_FN_SEATS) return std::nullopt;
    std::array<unsigned char, bls::SIGNATURE_SIZE> bytes{};
    std::copy(payload.begin() + 4, payload.end(), bytes.begin());
    const auto signature{bls::Signature::Decode(bytes)};
    if (!signature) return std::nullopt;
    return IndexedBlsSignature{seat_index, *signature};
}

std::optional<std::vector<unsigned char>> EncodeProductionCertifiedPayload(
    const ProductionCertifiedEnvelope& certified, const size_t seat_count)
{
    const auto entry{EncodeProductionEntry(certified.entry)};
    const auto certificate{
        EncodeBlsMicroblockCertificate(certified.certificate, seat_count)};
    if (!entry || !certificate ||
        entry->size() > std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    const size_t total{FLOWMESH_CERTIFIED_PAYLOAD_PREFIX_SIZE + entry->size() +
                       certificate->size()};
    if (total > FLOWMESH_CERTIFICATE_MAX_BYTES) return std::nullopt;
    std::vector<unsigned char> out(total);
    WriteBE32(out.data(), static_cast<uint32_t>(entry->size()));
    std::copy(entry->begin(), entry->end(),
              out.begin() + FLOWMESH_CERTIFIED_PAYLOAD_PREFIX_SIZE);
    std::copy(certificate->begin(), certificate->end(),
              out.begin() + FLOWMESH_CERTIFIED_PAYLOAD_PREFIX_SIZE +
                  entry->size());
    return out;
}

std::optional<ProductionCertifiedEnvelope> DecodeProductionCertifiedPayload(
    const std::span<const unsigned char> payload, const size_t seat_count)
{
    if (payload.size() <= FLOWMESH_CERTIFIED_PAYLOAD_PREFIX_SIZE ||
        payload.size() > FLOWMESH_CERTIFICATE_MAX_BYTES ||
        seat_count < FLOWMESH_MIN_ACTIVE_FN_SEATS ||
        seat_count > FLOWMESH_MAX_ACTIVE_FN_SEATS) {
        return std::nullopt;
    }
    const uint32_t entry_size{ReadBE32(payload.data())};
    const size_t certificate_size{FLOWMESH_BLS_CERTIFICATE_FIXED_SIZE +
                                  FlowMeshSignerBitmapBytes(seat_count)};
    if (entry_size == 0 || entry_size > FLOWMESH_V1_MAX_MICROBLOCK_BYTES ||
        payload.size() != FLOWMESH_CERTIFIED_PAYLOAD_PREFIX_SIZE +
                              entry_size + certificate_size) {
        return std::nullopt;
    }
    const auto entry{DecodeProductionEntry(payload.subspan(
        FLOWMESH_CERTIFIED_PAYLOAD_PREFIX_SIZE, entry_size))};
    const auto certificate{DecodeBlsMicroblockCertificate(
        payload.subspan(FLOWMESH_CERTIFIED_PAYLOAD_PREFIX_SIZE + entry_size),
        seat_count)};
    if (!entry || !certificate) return std::nullopt;
    ProductionCertifiedEnvelope out{*entry, *certificate};
    if (out.certificate.seat_epoch != out.entry.epoch ||
        out.certificate.sequence != out.entry.sequence ||
        out.certificate.microblock_hash != out.entry.GetHash()) {
        return std::nullopt;
    }
    const auto canonical{EncodeProductionCertifiedPayload(out, seat_count)};
    if (!canonical || !std::equal(canonical->begin(), canonical->end(),
                                  payload.begin(), payload.end())) {
        return std::nullopt;
    }
    return out;
}

bool ProductionWireHeaderMatches(const WireHeader& header,
                                 const ProductionEntryCore& entry)
{
    return header.version == FLOWMESH_WIRE_VERSION_V1 &&
           !header.market_id.IsNull() && header.market_id == entry.market_id &&
           header.epoch == entry.epoch && header.sequence == entry.sequence;
}

} // namespace flowmesh
