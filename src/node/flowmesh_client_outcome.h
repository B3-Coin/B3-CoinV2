// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_CLIENT_OUTCOME_H
#define B3COIN_NODE_FLOWMESH_CLIENT_OUTCOME_H

#include <flowmesh/action_reason.h>
#include <flowmesh/client_evidence.h>
#include <flowmesh/outcome_evidence.h>
#include <interfaces/chain.h>
#include <node/flowmesh_client_join.h>
#include <node/flowmesh_production_store.h>

#include <univalue.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace node {

//! Latency and outcome capabilities are hints from a status object, never
//! evidence: only a JSON true advertises verifiable outcomes.
inline bool FlowMeshAdvertisedActionOutcome(const UniValue& status)
{
    return status.isObject() && status.exists("action_outcome_v1") && status["action_outcome_v1"].isTrue();
}

//! Receipt form of a verified derivation over certified entry (M, hash).
inline interfaces::FlowMeshActionOutcome FlowMeshReceiptOutcome(const flowmesh::DerivedActionOutcome& derived,
                                                                const uint64_t sequence, const uint256& hash,
                                                                std::string evidence)
{
    interfaces::FlowMeshActionOutcome out;
    switch (derived.status) {
    case flowmesh::ActionOutcomeStatus::APPLIED: out.status = "applied"; break;
    case flowmesh::ActionOutcomeStatus::REJECTED: out.status = "rejected"; break;
    case flowmesh::ActionOutcomeStatus::INCLUDED_NOT_EXECUTED: out.status = "included_not_executed"; break;
    }
    out.final = derived.final;
    out.reason_code = std::string{flowmesh::ActionReasonName(derived.code)};
    out.evidence = std::move(evidence);
    out.microblock_sequence = sequence;
    out.microblock_hash = hash;
    if (derived.fill) {
        const auto& fill{*derived.fill};
        out.fill = interfaces::FlowMeshActionOutcome::Fill{
            fill.side == flowmesh::ClearingEngine::Side::BID ? "bid" : "ask", fill.cleared, fill.superseded_in_entry,
            fill.quantity, fill.price, fill.quote_atoms, fill.fee_atoms, fill.net_quote_atoms};
    }
    if (derived.withdrawal) {
        out.withdrawal = interfaces::FlowMeshActionOutcome::Withdrawal{derived.withdrawal->receipt_id,
                                                                        derived.withdrawal->asset,
                                                                        derived.withdrawal->amount};
    }
    if (derived.deposit) {
        out.deposit = interfaces::FlowMeshActionOutcome::Deposit{derived.deposit->deposit_outpoint,
                                                                  derived.deposit->asset, derived.deposit->amount};
    }
    return out;
}

//! Receipt form of a proven non-execution: a different certified action X
//! consumed the sequence in entry (M, hash).
inline interfaces::FlowMeshActionOutcome FlowMeshNotExecutedOutcome(const uint256& consumer, const uint64_t sequence,
                                                                    const uint256& hash)
{
    interfaces::FlowMeshActionOutcome out;
    out.status = "not_executed";
    out.final = true;
    out.reason_code = std::string{flowmesh::ActionReasonName(flowmesh::ActionReasonCode::NOT_EXECUTED_SEQUENCE_CONSUMED)};
    out.evidence = "certified_sequence_consumer";
    out.microblock_sequence = sequence;
    out.microblock_hash = hash;
    out.consumed_by = interfaces::FlowMeshActionOutcome::ConsumedBy{consumer, sequence, hash};
    return out;
}

//! A verified outcome survives only when it is consistent with its receipt:
//! an execution outcome of the verified including entry, or a proven
//! non-execution (state "rejected") naming its consumer. Anything else is
//! stripped and outcome_verified cleared. Never throws.
inline void FlowMeshStripInconsistentOutcome(interfaces::FlowMeshActionReceipt& receipt)
{
    const auto& outcome{receipt.outcome};
    const bool consistent{receipt.outcome_verified && outcome &&
        (((outcome->status == "applied" || outcome->status == "rejected" || outcome->status == "included_not_executed") &&
          receipt.state == "certified_inclusion" && receipt.certificate_verified &&
          outcome->microblock_hash == receipt.microblock_hash) ||
         (outcome->status == "not_executed" && receipt.state == "rejected" && outcome->consumed_by))};
    if (!consistent) {
        receipt.outcome.reset();
        receipt.outcome_verified = false;
    }
}

//! Evidence over one retained record (copies its vectors).
inline flowmesh::ExecutionOutcomeEvidenceV1 FlowMeshRecordEvidence(const ProductionOutcomeRecord& record)
{
    return {record.sequence, record.entry_hash, record.preimage, record.effects};
}

/** Whether `evidence` (endpoint bytes) proves that `action` can never
 * execute because `consumer` names a DIFFERENT certified action that used
 * its account sequence. `entry` must be the consumer's entry whose
 * certificate the caller already verified under local B3 authority. True
 * only when the entry is exactly the named microblock, the evidence decodes
 * and hashes to its result root, and ProvesSequenceConsumedByOther holds.
 * Never throws; any doubt is false. */
inline bool FlowMeshProvesNotExecuted(const flowmesh::ProductionEntryCore& entry, std::span<const unsigned char> evidence,
                                      const flowmesh::Action& action, const flowmesh::ClientConsumerRef& consumer) noexcept
{
    try {
        if (action.IsDeposit() || entry.sequence != consumer.microblock_sequence ||
            entry.GetHash() != consumer.microblock_hash) return false;
        const auto decoded{flowmesh::DecodeExecutionOutcomeEvidenceV1(evidence)};
        return decoded && flowmesh::VerifyExecutionOutcomeEvidence(entry, *decoded) == flowmesh::OutcomeEvidenceCheck::OK &&
               flowmesh::ProvesSequenceConsumedByOther(entry, *decoded, action, consumer.action_id);
    } catch (...) {
        return false;
    }
}

//! A retained action whose non-execution this process has proven: its
//! durable marker and a verified not_executed outcome naming that consumer.
inline bool FlowMeshProvenNotExecuted(const interfaces::FlowMeshActionReceipt& receipt,
                                      const std::optional<flowmesh::ClientConsumerRef>& marker)
{
    return marker && receipt.state == "rejected" && receipt.outcome_verified && receipt.outcome &&
           receipt.outcome->status == "not_executed" && receipt.outcome->consumed_by &&
           receipt.outcome->consumed_by->action_id == marker->action_id &&
           receipt.outcome->consumed_by->microblock_sequence == marker->microblock_sequence &&
           receipt.outcome->consumed_by->microblock_hash == marker->microblock_hash;
}

/** How a retained own signed action counts for a signing preflight's join.
 * A proven non-execution is reflected in any state at or after its
 * consumer's microblock; a retained marker whose proof is not renewed yet
 * joins only once the cached state consumed its sequence. Both come before a
 * "rejected" receipt: a proven non-execution reads "rejected", yet unlike a
 * first-attempt refusal its sequence WAS consumed (by the consumer). */
inline FlowMeshJoinOwnAction FlowMeshJoinOwnActionFor(const interfaces::FlowMeshActionReceipt& receipt,
                                                      const bool previously_certified,
                                                      const std::optional<flowmesh::ClientConsumerRef>& not_executed,
                                                      const uint64_t sequence)
{
    using Kind = FlowMeshJoinOwnAction::Kind;
    if (receipt.certificate_verified && receipt.state == "certified_inclusion") {
        return {Kind::CERTIFIED, receipt.microblock_sequence, sequence};
    }
    if (FlowMeshProvenNotExecuted(receipt, not_executed)) return {Kind::CERTIFIED, not_executed->microblock_sequence, sequence};
    // Certified before a restart, or its proof is being renewed: where it
    // was included is not known here. It may join only if the cached state
    // already consumed its sequence.
    if (previously_certified || not_executed) return {Kind::CERTIFIED, std::nullopt, sequence};
    // Send and QueryAction keep a refusal after any possible earlier delivery
    // as unknown, so this is definite.
    if (receipt.state == "rejected") return {Kind::DEFINITE_REJECTED, std::nullopt, sequence};
    return {Kind::UNRESOLVED, std::nullopt, sequence};
}

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_CLIENT_OUTCOME_H
