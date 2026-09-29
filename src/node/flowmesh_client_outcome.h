// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_CLIENT_OUTCOME_H
#define B3COIN_NODE_FLOWMESH_CLIENT_OUTCOME_H

#include <flowmesh/action_reason.h>
#include <flowmesh/outcome_evidence.h>
#include <interfaces/chain.h>
#include <node/flowmesh_production_store.h>

#include <optional>
#include <string>
#include <utility>

namespace node {

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

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_CLIENT_OUTCOME_H
