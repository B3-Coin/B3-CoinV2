// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_OUTCOME_EVIDENCE_H
#define B3COIN_FLOWMESH_OUTCOME_EVIDENCE_H

#include <consensus/amount.h>
#include <flowmesh/action_reason.h>
#include <flowmesh/batch.h>
#include <flowmesh/fee_allocation.h>
#include <flowmesh/production_engine.h>
#include <modern/flowmesh_checkpoint.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace flowmesh {

/**
 * Outcome evidence for one certified EXECUTION entry.
 *
 * A certified entry commits to what executing it did only through
 * result_root = ComputeProductionExecutionResultRoot(execresult/v1
 * commitment, effects). The preimage below is exactly the data hashed by
 * "b3/flowmesh/execresult/v1" (batch.h), in the same order. A client that
 * already verified the entry's BLS certificate under its own B3 authority can
 * recompute result_root from this preimage and the effects and thereby learn,
 * with the same trust as the certificate, what happened to each action.
 *
 * Nothing here changes a commitment, a signed format or execution: batch.h
 * and every production commitment are untouched. This is a separate, pure,
 * local API codec; a divergence from batch.h fails CLOSED (no outcome).
 */
inline constexpr uint8_t OUTCOME_EVIDENCE_VERSION_V1{1};
//! Local API bound, never consensus. Larger results stay "inclusion verified".
inline constexpr size_t FLOWMESH_OUTCOME_EVIDENCE_MAX_BYTES{1U << 20};

struct ExecutionOutcomePreimageV1 {
    uint64_t slot{0};
    std::vector<uint256> applied;
    std::vector<std::pair<uint256, ActionReject>> rejected;
    uint256 request_root;
    bool cleared{false};
    CAmount price{0};
    CAmount volume{0};
    CAmount imbalance{0};
    //! Map order (strictly ascending account), as the executor hashes them.
    std::vector<std::pair<AccountId, CAmount>> bid_fill;
    std::vector<std::pair<AccountId, CAmount>> ask_fill;
    CAmount matched_b3_quote_notional{0};
    CAmount fee_total{0};
    CAmount treasury_fee{0};
    CAmount seat_fee{0};
    std::vector<SellerFeeShare> seller_fees;
    std::vector<SeatFeeShare> seat_rewards;

    friend bool operator==(const ExecutionOutcomePreimageV1&, const ExecutionOutcomePreimageV1&) = default;
};

ExecutionOutcomePreimageV1 MakeExecutionOutcomePreimageV1(const BatchResult& result);
//! Moves the vectors out of `result` (the append path never copies them).
ExecutionOutcomePreimageV1 MakeExecutionOutcomePreimageV1(BatchResult&& result);

//! Byte-for-byte the execresult/v1 hash of BatchExecutor::ExecuteSlot.
uint256 ComputeExecResultCommitmentV1(const ExecutionOutcomePreimageV1& preimage);

struct ExecutionOutcomeEvidenceV1 {
    uint64_t entry_sequence{0};
    uint256 entry_hash;
    ExecutionOutcomePreimageV1 preimage;
    std::vector<modern::FlowMeshEffectV1> effects;
};

//! Strict, versioned, single encoding; nullopt above the byte bound.
std::optional<std::vector<unsigned char>> EncodeExecutionOutcomeEvidenceV1(
    uint64_t entry_sequence, const uint256& entry_hash,
    const ExecutionOutcomePreimageV1& preimage,
    std::span<const modern::FlowMeshEffectV1> effects);
std::optional<std::vector<unsigned char>> EncodeExecutionOutcomeEvidenceV1(
    const ExecutionOutcomeEvidenceV1& evidence);
//! Every count is checked against the remaining bytes before allocation, the
//! input must be fully consumed and at most FLOWMESH_OUTCOME_EVIDENCE_MAX_BYTES.
std::optional<ExecutionOutcomeEvidenceV1> DecodeExecutionOutcomeEvidenceV1(
    std::span<const unsigned char> bytes);

enum class OutcomeEvidenceCheck : uint8_t {
    OK = 0,
    WRONG_ENTRY,
    RESULT_ROOT_MISMATCH,
    EFFECT_ROOT_MISMATCH,
    BODY_MISMATCH,
};

/**
 * Binds evidence to an entry whose certificate the CALLER already verified.
 * OK only when the entry is an EXECUTION entry with this exact sequence and
 * hash, the preimage and effects hash to its result_root, the effects to its
 * effect root and count, and every body action is disposed of exactly once.
 */
OutcomeEvidenceCheck VerifyExecutionOutcomeEvidence(const ProductionEntryCore& entry,
                                                    const ExecutionOutcomeEvidenceV1& evidence);

enum class ActionOutcomeStatus : uint8_t {
    APPLIED,
    REJECTED,
    INCLUDED_NOT_EXECUTED,
};

struct ActionFill {
    ClearingEngine::Side side{ClearingEngine::Side::BID};
    bool cleared{false};
    //! A later SUBMIT or CANCEL on the same side in the same entry replaced
    //! this curve before clearing; it filled nothing.
    bool superseded_in_entry{false};
    CAmount quantity{0};
    CAmount price{0};
    //! BID: quote paid. ASK: gross proceeds.
    CAmount quote_atoms{0};
    CAmount fee_atoms{0};
    CAmount net_quote_atoms{0};
};

struct DerivedActionOutcome {
    ActionOutcomeStatus status{ActionOutcomeStatus::REJECTED};
    bool final{false};
    ActionReasonCode code{ActionReasonCode::NONE};
    std::optional<ActionFill> fill;
    std::optional<modern::FlowMeshWithdrawalReceiptV1> withdrawal;
    std::optional<modern::FlowMeshDepositAcceptanceV1> deposit;
};

/**
 * What the committed result says about `action`. The caller must have
 * checked VerifyExecutionOutcomeEvidence(entry, evidence) == OK. nullopt
 * (fail closed) when the action is not in the body or the committed data is
 * inconsistent with the executor's rules.
 */
std::optional<DerivedActionOutcome> DeriveActionOutcome(const ProductionEntryCore& entry,
                                                        const ExecutionOutcomeEvidenceV1& evidence,
                                                        const Action& action);

/**
 * True ONLY when `consumer_id` differs from action.Id(), is in `entry`, is a
 * signed action with the same signer and account sequence as `action`, and
 * its committed disposition consumed that sequence (APPLIED or
 * REJECTED_BY_STATE). A sequence is consumed exactly once and never reopened
 * (batch.h, state.h), and had `action` been in the same entry the pair would
 * have been EQUIVOCATION, consuming nothing. So `action` can never execute.
 * The caller must have verified the entry's certificate and the evidence.
 */
bool ProvesSequenceConsumedByOther(const ProductionEntryCore& entry,
                                   const ExecutionOutcomeEvidenceV1& evidence,
                                   const Action& action, const uint256& consumer_id);
//! The same proof for a signed action known only by its id, signer and
//! account sequence.
bool ProvesSequenceConsumedByOther(const ProductionEntryCore& entry,
                                   const ExecutionOutcomeEvidenceV1& evidence,
                                   const uint256& action_id, const AccountId& signer, uint64_t sequence,
                                   const uint256& consumer_id);

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_OUTCOME_EVIDENCE_H
