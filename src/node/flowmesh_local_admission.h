// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_LOCAL_ADMISSION_H
#define B3COIN_NODE_FLOWMESH_LOCAL_ADMISSION_H

#include <flowmesh/action_reason.h>
#include <flowmesh/batch.h>
#include <flowmesh/client_evidence.h>
#include <flowmesh/state.h>
#include <node/flowmesh_recent_window.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace node {

//! Local API policy only: never consensus, never applied to remote frames,
//! proposal evidence, agreement or certified entries. A signed action this
//! node's own API admits must be at most this far beyond the certified next
//! account sequence.
inline constexpr uint64_t FLOWMESH_LOCAL_MAX_SEQUENCE_AHEAD{64};

/**
 * What this node does with one of its OWN API/RPC submissions, decided from
 * its certified state, its recent certified window and its client event ring.
 *
 * - ENQUEUE: queue it as today.
 * - ALREADY_INCLUDED: this exact ActionId is in certified microblock
 *   `included`; the answer is that certified inclusion, never "rejected".
 * - REFUSE: this attempt is provably never admitted here (a DIFFERENT
 *   certified action consumed its sequence, or it is too far ahead).
 * - UNKNOWN_OUTCOME: the sequence or deposit is already used in certified
 *   state but this node no longer knows by which action: "unknown", never
 *   "rejected", and never queued again.
 */
struct FlowMeshLocalAdmission {
    enum class Kind : uint8_t { ENQUEUE, ALREADY_INCLUDED, REFUSE, UNKNOWN_OUTCOME };
    Kind kind{Kind::ENQUEUE};
    flowmesh::ActionReasonCode code{flowmesh::ActionReasonCode::NONE};
    //! REFUSE(STALE_SEQUENCE): the consuming action and its microblock.
    std::optional<flowmesh::ClientConsumerRef> consumer;
    //! ALREADY_INCLUDED: {microblock sequence, microblock hash}.
    std::optional<std::pair<uint64_t, uint256>> included;
    //! Certified NextSequence(signer) the decision used (0 for deposits).
    uint64_t next_account_sequence{0};
};

/**
 * Pure decision. `ring_status` is the event ring's current ActionStatus for
 * this ActionId. Inclusion is looked up before any stale test, so an
 * already-executed resubmission is answered with its inclusion. A signed
 * action at or above the next sequence is queued without an inclusion
 * check: an included-but-not-executed action whose sequence is still open
 * may be delivered again, as before.
 */
inline FlowMeshLocalAdmission FlowMeshDecideLocalAdmission(
    const flowmesh::FlowMeshState& state, const FlowMeshRecentCertifiedWindow& recent,
    const std::optional<flowmesh::ClientEvent>& ring_status, const flowmesh::Action& action)
{
    using Kind = FlowMeshLocalAdmission::Kind;
    using Code = flowmesh::ActionReasonCode;
    FlowMeshLocalAdmission out;
    const auto included = [&]() -> std::optional<std::pair<uint64_t, uint256>> {
        if (auto found{recent.Inclusion(action.Id())}) return found;
        if (ring_status && ring_status->kind == flowmesh::ClientEventKind::CERTIFIED_INCLUDED) {
            return std::pair{ring_status->microblock_sequence, ring_status->microblock_hash};
        }
        return std::nullopt;
    };
    if (action.IsDeposit()) {
        if (!state.DepositConsumed(action.outpoint)) return out;
        if ((out.included = included())) {
            out.kind = Kind::ALREADY_INCLUDED;
            out.code = Code::ALREADY_INCLUDED;
        } else {
            out.kind = Kind::UNKNOWN_OUTCOME;
            out.code = Code::DEPOSIT_ALREADY_CREDITED;
        }
        return out;
    }
    const uint64_t next{state.NextSequence(action.signer)};
    out.next_account_sequence = next;
    if (action.sequence >= next) {
        if (action.sequence - next > FLOWMESH_LOCAL_MAX_SEQUENCE_AHEAD) {
            out.kind = Kind::REFUSE;
            out.code = Code::FAR_FUTURE_SEQUENCE;
        }
        return out;
    }
    if ((out.included = included())) {
        out.kind = Kind::ALREADY_INCLUDED;
        out.code = Code::ALREADY_INCLUDED;
        return out;
    }
    if (const auto consumer{recent.FindConsumer(action.signer, action.sequence)}) {
        // The exact ActionId was not found, so the consumer is a different action.
        out.kind = Kind::REFUSE;
        out.code = Code::STALE_SEQUENCE;
        out.consumer = flowmesh::ClientConsumerRef{consumer->action_id, consumer->microblock_sequence,
                                                   consumer->microblock_hash};
        return out;
    }
    out.kind = Kind::UNKNOWN_OUTCOME;
    out.code = Code::STALE_SEQUENCE_UNATTRIBUTED;
    return out;
}

//! The receipt text of a non-ENQUEUE decision for this node's API caller.
inline std::string FlowMeshLocalAdmissionText(const FlowMeshLocalAdmission& admission)
{
    using Code = flowmesh::ActionReasonCode;
    switch (admission.code) {
    case Code::ALREADY_INCLUDED:
        return "Exact action already included in certified microblock #" +
               std::to_string(admission.included ? admission.included->first : 0) + "; not queued again";
    case Code::FAR_FUTURE_SEQUENCE:
        return "FlowMesh action account sequence is more than 64 ahead of the certified next sequence; not queued";
    case Code::STALE_SEQUENCE:
        return "FlowMesh action account sequence was already used by another certified action; this action can never execute";
    case Code::STALE_SEQUENCE_UNATTRIBUTED:
        return "FlowMesh action account sequence is already used in certified state; this node no longer retains which "
               "action used it; outcome unknown; not queued again";
    case Code::DEPOSIT_ALREADY_CREDITED:
        return "FlowMesh deposit is already credited in certified state; its batch is no longer retained here; not queued again";
    default:
        return {};
    }
}

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_LOCAL_ADMISSION_H
