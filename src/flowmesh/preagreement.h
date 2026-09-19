// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_PREAGREEMENT_H
#define B3COIN_FLOWMESH_PREAGREEMENT_H

#include <flowmesh/bls_certificate.h>
#include <flowmesh/market.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace flowmesh {

/** EXPERIMENTAL STATELESS PROOF VERIFIER.
 *
 * No local vote/signing API, durable journal, wire codec, view-change state
 * machine, runtime activation, or permission to emit a V1 signature is exposed
 * by this header. Those obligations belong to the separate agreement engine
 * and its explicitly opted-in fresh-market runtime integration.
 * Successful verification authenticates the supplied pre-agreement evidence;
 * it does not prove correct execution, anchor validity, local signing safety,
 * a complete BFT implementation, or liveness. In particular it must not unlock
 * an existing production journal. Callers must pin context/seat authority from
 * local validated state, not accept an endpoint's chosen context.
 * This stateless verifier cannot establish freshness relative to a local view
 * or enforce that a sender truthfully reports its highest prepared evidence.
 */
struct PreagreementContext {
    uint256 domain;
    MarketId market_id;
    uint64_t epoch{0};
    uint256 seat_set_hash;
    uint64_t sequence{0};
    uint256 parent_hash;
    uint256 previous_state_root;
    uint256 execution_config_id;
};

enum class PreagreementPhase : uint8_t { PREPARE = 1, COMMIT = 2 };

struct PreagreementPreparedCertificate {
    uint32_t view{0};
    uint256 candidate;
    // Exactly the unchanged FlowMesh quorum, in strictly increasing seat order.
    std::vector<IndexedBlsSignature> votes;
};

struct PreagreementViewChange {
    uint32_t view{0};
    uint32_t seat_index{0};
    // Highest prepared certificate reported by this seat. The verifier cannot
    // establish that a sender has not hidden a higher certificate.
    std::optional<PreagreementPreparedCertificate> prepared;
    std::array<unsigned char, bls::SIGNATURE_SIZE> signature{};
};

struct PreagreementNewViewProof {
    uint32_t view{0};
    uint256 candidate;
    // Exactly quorum distinct authenticated reports; no recursive new-view
    // chains. Prior prepared certificates are authenticated quorum evidence.
    std::vector<PreagreementViewChange> reports;
};

struct PreagreementCommitCertificate {
    PreagreementPreparedCertificate prepared;
    std::vector<IndexedBlsSignature> commits;
    // Forbidden at view zero; mandatory and matching at every later view.
    std::optional<PreagreementNewViewProof> new_view;
};

// Local experimental verifier resource limit, not a B3 consensus limit or a
// different quorum. Oversized complete proofs are refused before BLS work.
inline constexpr size_t PREAGREEMENT_MAX_PROOF_SIGNATURES{4096};

enum class PreagreementCheck : uint8_t {
    OK,
    INVALID_CONTEXT,
    INVALID_SEAT_SET,
    INVALID_CANDIDATE,
    WRONG_QUORUM,
    NON_CANONICAL_SIGNERS,
    BAD_SIGNATURE,
    INVALID_VIEW,
    MISSING_NEW_VIEW,
    WRONG_NEW_VIEW,
    CONFLICTING_PREPARED,
    UNSAFE_NEW_VIEW_CANDIDATE,
    PROOF_TOO_LARGE,
};

const char* PreagreementCheckName(PreagreementCheck check);

// Domain-separated digest helpers only. They do not sign or authorize signing.
std::optional<uint256> PreagreementVoteDigest(
    const PreagreementContext& context, PreagreementPhase phase,
    uint32_t view, const uint256& candidate, uint32_t seat_index);
std::optional<uint256> PreagreementViewChangeDigest(
    const PreagreementContext& context, const PreagreementViewChange& report);

PreagreementCheck CheckPreagreementPrepared(
    const PreagreementContext& context, const ActiveFnBlsSeatSet& seats,
    const PreagreementPreparedCertificate& certificate);
PreagreementCheck CheckPreagreementNewView(
    const PreagreementContext& context, const ActiveFnBlsSeatSet& seats,
    const PreagreementNewViewProof& proof);
PreagreementCheck CheckPreagreementCommit(
    const PreagreementContext& context, const ActiveFnBlsSeatSet& seats,
    const PreagreementCommitCertificate& certificate);

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_PREAGREEMENT_H
