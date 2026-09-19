// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_AGREEMENT_WIRE_H
#define B3COIN_FLOWMESH_AGREEMENT_WIRE_H

#include <flowmesh/preagreement.h>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace flowmesh {

inline constexpr uint16_t AGREEMENT_WIRE_VERSION{1};
inline constexpr size_t AGREEMENT_MAX_BYTES{4U * 1024U * 1024U};
inline constexpr size_t AGREEMENT_MAX_PROOF_BYTES{1024U * 1024U};
inline constexpr uint32_t AGREEMENT_NO_SEAT{std::numeric_limits<uint32_t>::max()};

enum class AgreementStage : uint8_t {
    PROPOSAL = 1, PREPARE = 2, COMMIT = 3, VIEW_CHANGE = 4, DECISION = 5,
};

struct AgreementMessage {
    AgreementStage stage{AgreementStage::PROPOSAL};
    PreagreementContext context;
    uint32_t view{0};
    uint256 candidate;
    uint32_t seat_index{0};
    std::array<unsigned char, bls::SIGNATURE_SIZE> signature{};
    // Only PROPOSAL/DECISION carry exact canonical production entry bytes.
    // Private action evidence continues to use the existing ACTION channel.
    std::vector<unsigned char> entry_bytes;
    std::optional<PreagreementPreparedCertificate> prepared;
    std::optional<PreagreementNewViewProof> new_view;
    std::optional<PreagreementCommitCertificate> decision;
};

// Canonical, bounded structural codecs. They do not authenticate authority,
// execution, or local signing history. Zero outer signatures are permitted so
// the same format can retain a durable unsigned signing intent. A DECISION is
// quorum-authenticated and has AGREEMENT_NO_SEAT plus an all-zero outer sig.
std::optional<std::vector<unsigned char>> EncodeAgreementMessage(const AgreementMessage& message);
std::optional<AgreementMessage> DecodeAgreementMessage(std::span<const unsigned char> bytes);
std::optional<std::vector<unsigned char>> EncodePreagreementPrepared(const PreagreementPreparedCertificate& proof);
std::optional<PreagreementPreparedCertificate> DecodePreagreementPrepared(std::span<const unsigned char> bytes);
std::optional<std::vector<unsigned char>> EncodePreagreementViewChange(const PreagreementViewChange& proof);
std::optional<PreagreementViewChange> DecodePreagreementViewChange(std::span<const unsigned char> bytes);
std::optional<std::vector<unsigned char>> EncodePreagreementNewView(const PreagreementNewViewProof& proof);
std::optional<PreagreementNewViewProof> DecodePreagreementNewView(std::span<const unsigned char> bytes);
std::optional<std::vector<unsigned char>> EncodePreagreementCommit(const PreagreementCommitCertificate& proof);
std::optional<PreagreementCommitCertificate> DecodePreagreementCommit(std::span<const unsigned char> bytes);

// PREPARE/COMMIT/VIEW_CHANGE use the existing preliminary digests. PROPOSAL
// uses its own tag binding the exact entry bytes and complete new-view proof.
// DECISION has no outer digest/signature; use CheckPreagreementCommit instead.
std::optional<uint256> AgreementMessageDigest(const AgreementMessage& message);
// Checks the outer signature and, for proposals, the scheduled proposer. It
// does not replace CheckPreagreementPrepared/NewView/Commit for attachments,
// or comparison with the caller's independently pinned current context.
bool CheckAgreementMessageSignature(const AgreementMessage& message, const ActiveFnBlsSeatSet& seats);

} // namespace flowmesh
#endif // B3COIN_FLOWMESH_AGREEMENT_WIRE_H
