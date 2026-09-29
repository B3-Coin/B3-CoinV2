// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_ACTION_REASON_H
#define B3COIN_FLOWMESH_ACTION_REASON_H

#include <flowmesh/batch.h>

#include <cstdint>
#include <string_view>

namespace flowmesh {

/**
 * Closed, append-only registry of reasons attached to one FlowMesh action.
 *
 * Numbers and names are public API: never renumbered or renamed, only
 * appended. JSON carries the NAME; numbers exist for tests and logs. A client
 * that meets an unknown name treats it as "unrecognised reason; outcome
 * unchanged" and never as terminal.
 *
 * Nothing here is consensus, signed data or an execution rule. Observations
 * describe one attempt at one node; only the OUTCOME_* classes describe what
 * a certified entry did, and only when the caller verified that entry.
 */
enum class ActionReasonCode : uint16_t {
    NONE = 0,
    // Observations about one attempt at one node. Never execution outcomes.
    ADMITTED = 1,
    DUPLICATE_PENDING = 2,
    MALFORMED_SHAPE = 3,
    UNAUTHENTICATED = 4,
    DUPLICATE_ID_OTHER_CREDENTIAL = 5,
    SEQUENCE_CONFLICT = 6,
    DEPOSIT_CONFLICT = 7,
    POOL_FULL = 8,
    PEER_QUOTA = 9,
    //! A DIFFERENT certified action consumed this account sequence, according
    //! to this node's own certified store.
    STALE_SEQUENCE = 10,
    //! Local API policy only: more than 64 beyond the certified next sequence.
    FAR_FUTURE_SEQUENCE = 11,
    HEADER_NOT_CURRENT = 12,
    HEAD_INTERVAL_UNAVAILABLE = 13,
    //! The exact ActionId is already in a certified batch; not queued again.
    ALREADY_INCLUDED = 14,
    AUTHORITY_UNAVAILABLE = 15,
    DEPOSIT_FACTS_UNAVAILABLE = 16,
    //! Credited (so executed); its batch is no longer retained at this node.
    DEPOSIT_ALREADY_CREDITED = 17,
    QUEUE_UNAVAILABLE = 18,
    STALE_SEQUENCE_UNATTRIBUTED = 19,
    SUBMIT_LANE_FULL = 20,
    QUEUE_RATE_LIMITED = 21,
    // Pool removals of API-origin actions (notices only).
    DROPPED_SEQUENCE_CONSUMED = 32,
    // 33 DROPPED_DEPOSIT_CREDITED, 34 DROPPED_EXPIRED and 35 DROPPED_EVICTED
    // are RESERVED: never emitted, never reused.
    // Certified execution outcomes: 1:1 with the COMMITTED ActionReject.
    APPLIED = 64,
    REJECTED_BY_STATE = 65,
    INCLUDED_BAD_SEQUENCE = 66,
    INCLUDED_EQUIVOCATION = 67,
    INCLUDED_MALFORMED = 68,
    DEPOSIT_NOT_CREDITED = 69,
    //! Client-proven terminal: a different certified action consumed the
    //! sequence, so this action can never execute.
    NOT_EXECUTED_SEQUENCE_CONSUMED = 96,
    // Outcome evidence availability. Inclusion is unaffected by any of them.
    OUTCOME_EVIDENCE_EXPIRED = 112,
    OUTCOME_EVIDENCE_TOO_LARGE = 113,
    OUTCOME_EVIDENCE_INVALID = 114,
    OUTCOME_NOT_OFFERED = 115,
    OUTCOME_EVIDENCE_BUDGET = 116,
};

enum class ActionReasonClass : uint8_t {
    OBSERVATION,       // one attempt at one node; never terminal
    DROP,              // pool removal notice; never terminal by itself
    OUTCOME_FINAL,     // verified execution outcome that can never change
    OUTCOME_NOT_FINAL, // verified inclusion whose action may still execute
    PROVEN_TERMINAL,   // proven never to execute
    EVIDENCE,          // outcome proof availability; outcome unchanged
};

struct ActionReasonInfo {
    ActionReasonCode code;
    std::string_view name;
    std::string_view ui_text;
    ActionReasonClass cls;
};

inline constexpr ActionReasonInfo ACTION_REASONS[]{
    {ActionReasonCode::ADMITTED, "ADMITTED", "Queued for the next batch; not yet certified.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::DUPLICATE_PENDING, "DUPLICATE_PENDING", "Already queued; this resend changed nothing.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::MALFORMED_SHAPE, "MALFORMED_SHAPE", "Request is malformed for this market; it was not queued.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::UNAUTHENTICATED, "UNAUTHENTICATED", "Signature does not match this market and key; it was not queued.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::DUPLICATE_ID_OTHER_CREDENTIAL, "DUPLICATE_ID_OTHER_CREDENTIAL", "The same request is already queued with a different signature encoding.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::SEQUENCE_CONFLICT, "SEQUENCE_CONFLICT", "Another request with this account sequence is already queued.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::DEPOSIT_CONFLICT, "DEPOSIT_CONFLICT", "This deposit is already queued.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::POOL_FULL, "POOL_FULL", "Market queue is full; not queued. Resending the same request later is safe.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::PEER_QUOTA, "PEER_QUOTA", "Relay quota for this peer reached; not queued by this node.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::STALE_SEQUENCE, "STALE_SEQUENCE", "The account sequence was used by another certified request; this request can never execute.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::FAR_FUTURE_SEQUENCE, "FAR_FUTURE_SEQUENCE", "Account sequence is more than 64 ahead of the next one; not queued.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::HEADER_NOT_CURRENT, "HEADER_NOT_CURRENT", "Validator authority changed while queuing; not queued. Resend the same request.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::HEAD_INTERVAL_UNAVAILABLE, "HEAD_INTERVAL_UNAVAILABLE", "The endpoint could not re-check this request after a new batch; check status first.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::ALREADY_INCLUDED, "ALREADY_INCLUDED", "Already included in a certified batch; not queued again.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::AUTHORITY_UNAVAILABLE, "AUTHORITY_UNAVAILABLE", "Validator authority is changing; not queued. Resend the same request later.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::DEPOSIT_FACTS_UNAVAILABLE, "DEPOSIT_FACTS_UNAVAILABLE", "Deposit is not yet visible at the anchored B3 height; not queued.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::DEPOSIT_ALREADY_CREDITED, "DEPOSIT_ALREADY_CREDITED", "Deposit already credited; not queued again.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::QUEUE_UNAVAILABLE, "QUEUE_UNAVAILABLE", "Endpoint queue unavailable (full, stopped or reconciling); not queued.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::STALE_SEQUENCE_UNATTRIBUTED, "STALE_SEQUENCE_UNATTRIBUTED", "The account sequence is already used; this node no longer knows by which request. Outcome unknown.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::SUBMIT_LANE_FULL, "SUBMIT_LANE_FULL", "Queue space is reserved for cancels and withdrawals; order not queued. Resend later.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::QUEUE_RATE_LIMITED, "QUEUE_RATE_LIMITED", "Endpoint queue is rate limited; not queued. Resending the same request later is safe.", ActionReasonClass::OBSERVATION},
    {ActionReasonCode::DROPPED_SEQUENCE_CONSUMED, "DROPPED_SEQUENCE_CONSUMED", "Removed from the queue: its account sequence was used by another certified request.", ActionReasonClass::DROP},
    {ActionReasonCode::APPLIED, "APPLIED", "Executed in a certified batch.", ActionReasonClass::OUTCOME_FINAL},
    {ActionReasonCode::REJECTED_BY_STATE, "REJECTED_BY_STATE", "Refused by certified state; the account sequence was used.", ActionReasonClass::OUTCOME_FINAL},
    {ActionReasonCode::INCLUDED_BAD_SEQUENCE, "INCLUDED_BAD_SEQUENCE", "Included but not executed: its sequence was not next. It may still execute; resend only these exact bytes.", ActionReasonClass::OUTCOME_NOT_FINAL},
    {ActionReasonCode::INCLUDED_EQUIVOCATION, "INCLUDED_EQUIVOCATION", "Included but not executed: two requests used this sequence in one batch. Not final.", ActionReasonClass::OUTCOME_NOT_FINAL},
    {ActionReasonCode::INCLUDED_MALFORMED, "INCLUDED_MALFORMED", "Included but invalid; it can never execute.", ActionReasonClass::OUTCOME_FINAL},
    {ActionReasonCode::DEPOSIT_NOT_CREDITED, "DEPOSIT_NOT_CREDITED", "Deposit not credited in this batch; it may be credited later.", ActionReasonClass::OUTCOME_NOT_FINAL},
    {ActionReasonCode::NOT_EXECUTED_SEQUENCE_CONSUMED, "NOT_EXECUTED_SEQUENCE_CONSUMED", "Never executes: its account sequence was used by another certified request.", ActionReasonClass::PROVEN_TERMINAL},
    {ActionReasonCode::OUTCOME_EVIDENCE_EXPIRED, "OUTCOME_EVIDENCE_EXPIRED", "Included; the endpoint no longer holds the outcome proof.", ActionReasonClass::EVIDENCE},
    {ActionReasonCode::OUTCOME_EVIDENCE_TOO_LARGE, "OUTCOME_EVIDENCE_TOO_LARGE", "Included; the outcome proof is too large to verify remotely.", ActionReasonClass::EVIDENCE},
    {ActionReasonCode::OUTCOME_EVIDENCE_INVALID, "OUTCOME_EVIDENCE_INVALID", "Included; the endpoint's outcome proof failed verification.", ActionReasonClass::EVIDENCE},
    {ActionReasonCode::OUTCOME_NOT_OFFERED, "OUTCOME_NOT_OFFERED", "Included; this endpoint does not offer outcome proofs.", ActionReasonClass::EVIDENCE},
    {ActionReasonCode::OUTCOME_EVIDENCE_BUDGET, "OUTCOME_EVIDENCE_BUDGET", "Included; the endpoint's request budget did not cover the outcome proof.", ActionReasonClass::EVIDENCE},
};

//! nullptr for NONE and for any number or name outside the registry.
inline const ActionReasonInfo* ActionReasonLookup(const ActionReasonCode code)
{
    for (const auto& info : ACTION_REASONS) {
        if (info.code == code) return &info;
    }
    return nullptr;
}

inline const ActionReasonInfo* ActionReasonLookup(const std::string_view name)
{
    for (const auto& info : ACTION_REASONS) {
        if (info.name == name) return &info;
    }
    return nullptr;
}

//! Empty for NONE or an unregistered number.
inline std::string_view ActionReasonName(const ActionReasonCode code)
{
    const auto* info{ActionReasonLookup(code)};
    return info ? info->name : std::string_view{};
}

//! Terminal only for a verified final outcome or a proven non-execution. An
//! unknown name, an observation, a drop or an evidence code never is.
inline bool ActionReasonIsTerminal(const std::string_view name)
{
    const auto* info{ActionReasonLookup(name)};
    return info && (info->cls == ActionReasonClass::OUTCOME_FINAL ||
                    info->cls == ActionReasonClass::PROVEN_TERMINAL);
}

/**
 * REJECTED_BY_STATE text depends only on the action type. The committed data
 * carries no finer cause; finer sub-codes would change result_commitment and
 * need an owner ruling.
 */
inline std::string_view RejectedByStateText(const ActionType type)
{
    switch (type) {
    case ActionType::SUBMIT_BID:
    case ActionType::SUBMIT_ASK:
        return "Not executed: funds could not be reserved or the curve was invalid; sequence used.";
    case ActionType::CANCEL_BID:
    case ActionType::CANCEL_ASK:
        return "Nothing to cancel on that side; sequence used.";
    case ActionType::WITHDRAW:
        return "Withdrawal refused: insufficient available balance or vault capacity; sequence used.";
    case ActionType::CLAIM_SEAT_REWARD:
        return "Reward claim refused by state; sequence used.";
    case ActionType::DEPOSIT:
        return "Deposit not credited in this batch; it may be credited later.";
    case ActionType::SPOT_TO_FUTURES:
    case ActionType::FUTURES_TO_SPOT:
        return "Reserved action type; it can never execute.";
    }
    return "Refused by certified state.";
}

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_ACTION_REASON_H
