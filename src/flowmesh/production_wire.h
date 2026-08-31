// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_PRODUCTION_WIRE_H
#define B3COIN_FLOWMESH_PRODUCTION_WIRE_H

#include <flowmesh/p2p.h>
#include <flowmesh/production_engine.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace flowmesh {

/** Exact payload formats carried after the common 50-byte FlowMesh header. */
inline constexpr size_t FLOWMESH_PROPOSAL_PAYLOAD_PREFIX_SIZE{4 + 4 + 96};
inline constexpr size_t FLOWMESH_CERTIFIED_PAYLOAD_PREFIX_SIZE{4};

struct ProductionCertifiedEnvelope {
    ProductionEntryCore entry;
    BlsMicroblockCertificate certificate;
};

std::optional<std::vector<unsigned char>> EncodeProductionActionPayload(
    const Action& action);
std::optional<Action> DecodeProductionActionPayload(
    std::span<const unsigned char> payload);

std::optional<std::vector<unsigned char>> EncodeProductionProposalPayload(
    const ProductionProposalEnvelope& proposal);
std::optional<ProductionProposalEnvelope> DecodeProductionProposalPayload(
    std::span<const unsigned char> payload);

std::optional<std::vector<unsigned char>> EncodeProductionAttestationPayload(
    const IndexedBlsSignature& attestation);
std::optional<IndexedBlsSignature> DecodeProductionAttestationPayload(
    std::span<const unsigned char> payload);

/**
 * u32BE entry_size || exact ProductionEntryCore bytes || exact BLS certificate
 * bytes. The certificate width is inferred from the supplied anchored seat
 * count; no caller-controlled bitmap length exists.
 */
std::optional<std::vector<unsigned char>> EncodeProductionCertifiedPayload(
    const ProductionCertifiedEnvelope& certified, size_t seat_count);
std::optional<ProductionCertifiedEnvelope> DecodeProductionCertifiedPayload(
    std::span<const unsigned char> payload, size_t seat_count);

/** Common header and inner production identity must agree byte-for-byte. */
bool ProductionWireHeaderMatches(const WireHeader& header,
                                 const ProductionEntryCore& entry);

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_PRODUCTION_WIRE_H
