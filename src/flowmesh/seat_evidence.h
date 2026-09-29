// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_SEAT_EVIDENCE_H
#define B3COIN_FLOWMESH_SEAT_EVIDENCE_H

#include <crypto/bls.h>
#include <flowmesh/bls_certificate.h>
#include <flowmesh/market.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace flowmesh {

/**
 * Self-verifiable evidence that FlowMesh seats signed two conflicting
 * statements about one microblock sequence.
 *
 * Information only. Nothing reads it to decide consensus, admission, relay,
 * signing, verification, a halt, a seat's eligibility or a penalty; there are
 * no penalties. Each record carries every signed field, so anyone holding the
 * B3 chain can check it with CheckSeatEvidence against the anchored seat set
 * each statement names, without trusting the node that recorded it.
 */
enum class SeatEvidenceKind : uint8_t {
    //! One seat's BLS attestations for two different microblocks at one
    //! (epoch, sequence), under one seat set.
    ATTESTATION_EQUIVOCATION = 1,
    //! Two valid threshold certificates for two different microblocks at one
    //! sequence. Under one seat set, every seat whose bit is set in both
    //! signer bitmaps signed both.
    CERTIFICATE_CONFLICT = 2,
};
const char* SeatEvidenceKindName(SeatEvidenceKind kind);

/** One signed statement: the certificate context it signs (with the record's
 * domain and market) and either one seat's signature (an attestation) or a
 * threshold certificate's signer bitmap and aggregate signature. */
struct SeatEvidenceStatement {
    uint64_t seat_epoch{0};
    uint256 seat_set_hash;
    uint64_t sequence{0};
    uint256 microblock_hash;
    //! ATTESTATION_EQUIVOCATION only: the signing seat's index in the set.
    uint32_t seat_index{0};
    //! CERTIFICATE_CONFLICT only: the LSB-first signer bitmap.
    std::vector<unsigned char> signer_bitmap;
    std::array<unsigned char, FLOWMESH_BLS_SIGNATURE_SIZE> signature{};

    friend bool operator==(const SeatEvidenceStatement&, const SeatEvidenceStatement&) = default;
};

struct SeatEvidence {
    SeatEvidenceKind kind{SeatEvidenceKind::ATTESTATION_EQUIVOCATION};
    uint256 domain;
    MarketId market_id;
    //! The statement the recording node held first (its earlier vote or its
    //! durable certified entry).
    SeatEvidenceStatement first;
    //! The conflicting statement it received afterwards.
    SeatEvidenceStatement second;
    //! ATTESTATION_EQUIVOCATION only (and then required): the BLS public key
    //! of the seat at seat_index in the named set, so an operator can match
    //! it against its own keys. Not signed; CheckSeatEvidence requires it to
    //! be that seat's key. Not part of Id().
    std::optional<std::array<unsigned char, bls::PUBKEY_SIZE>> seat_public_key;

    //! The exact context `statement` signs under this record's domain/market.
    BlsCertificateContext Context(const SeatEvidenceStatement& statement) const;
    //! Order-independent identity (tagged hash of the kind, domain, market and
    //! both encoded statements in byte order); deduplicates records.
    uint256 Id() const;

    friend bool operator==(const SeatEvidence&, const SeatEvidence&) = default;
};

//! Encoded record version.
inline constexpr uint8_t SEAT_EVIDENCE_VERSION{1};
//! No record can encode larger: two statements at the largest seat set.
inline constexpr size_t SEAT_EVIDENCE_MAX_BYTES{
    2 + 32 + 32 + 2 * (8 + 32 + 8 + 32 + 4 + 3 + FLOWMESH_MAX_SIGNER_BITMAP_BYTES + FLOWMESH_BLS_SIGNATURE_SIZE) +
    bls::PUBKEY_SIZE};

//! Canonical bytes; nullopt for a record whose shape is not canonical.
std::optional<std::vector<unsigned char>> EncodeSeatEvidence(const SeatEvidence& evidence);
//! Strict decode: exact length, known version and kind, canonical shape.
std::optional<SeatEvidence> DecodeSeatEvidence(std::span<const unsigned char> bytes);

enum class SeatEvidenceCheck : uint8_t {
    OK = 0,
    MALFORMED,
    //! The statements do not conflict (another sequence, the same microblock,
    //! or, for an equivocation, another seat, epoch or seat set).
    NOT_CONFLICTING,
    //! A supplied seat set is not the anchored set the statement names.
    WRONG_SEAT_SET,
    //! seat_public_key is not the key of the named seat.
    WRONG_SEAT_KEY,
    BAD_FIRST_SIGNATURE,
    BAD_SECOND_SIGNATURE,
};
const char* SeatEvidenceCheckName(SeatEvidenceCheck check);

/** Expensive steps of CheckSeatEvidence that its caller has already completed
 * itself, with an OK result, for these exact inputs, so the check need not
 * repeat them. Set a flag only for work this node did; the default repeats
 * everything. The cheap checks (shape, the conflict, each set's market, epoch
 * and hash, and the seat key) always run. */
struct SeatEvidencePrechecked {
    //! The supplied set passed CheckActiveFnBlsSeatSet under the record's domain.
    bool first_seats{false};
    bool second_seats{false};
    //! The statement's signature verified over Context(statement) under the
    //! set the statement names.
    bool first_signature{false};
    bool second_signature{false};
};

//! Expensive steps one CheckSeatEvidence call started.
struct SeatEvidenceWork {
    //! Seat-set validations, including the one inside each certificate check.
    uint64_t seat_set_checks{0};
    //! BLS verifications: one seat's signature or one aggregate certificate.
    uint64_t signature_checks{0};
};

/** Check `evidence` against the seat set of each statement. The caller
 * supplies those sets from its own B3 chain, never from the record; each must
 * be the complete valid set for (market, epoch, seat_set_hash) it names.
 * `prechecked` skips only the steps it names; `work`, when given, counts the
 * expensive steps this call started. */
SeatEvidenceCheck CheckSeatEvidence(const SeatEvidence& evidence,
                                    const ActiveFnBlsSeatSet& first_seats,
                                    const ActiveFnBlsSeatSet& second_seats,
                                    const SeatEvidencePrechecked& prechecked = {},
                                    SeatEvidenceWork* work = nullptr);

//! A seat's attestation as one statement.
SeatEvidenceStatement SeatAttestationStatement(const BlsCertificateContext& context, uint32_t seat_index,
                                               const bls::Signature& signature);
//! A threshold certificate as one statement.
SeatEvidenceStatement SeatCertificateStatement(const BlsMicroblockCertificate& certificate,
                                               const uint256& seat_set_hash);

//! Seats whose bit is set in both certificates of a CERTIFICATE_CONFLICT under
//! one seat set (each of them signed both); empty for anything else.
std::vector<uint32_t> SeatEvidenceDoubleSigners(const SeatEvidence& evidence);

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_SEAT_EVIDENCE_H
