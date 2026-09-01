// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_MODERN_MPA_H
#define B3COIN_MODERN_MPA_H

#include <bridge/proof.h>
#include <consensus/consensus.h>
#include <consensus/era.h>
#include <consensus/params.h>
#include <modern/creation_action.h>
#include <modern/bridge_binding.h>
#include <modern/finality_key.h>
#include <modern/finality_types.h>
#include <modern/flowmesh_checkpoint.h>
#include <modern/flowmesh_seat.h>
#include <modern/flowmesh_vault_proof.h>
#include <modern/fn_pod.h>
#include <modern/metadata_cell.h>
#include <modern/payload_cost.h>
#include <modern/policy.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace modern {

/**
 * Modern Payload Area — consensus layer over the wire codec in
 * primitives/transaction.h (plan Commit 5).
 *
 * REGISTRY (typed, versioned, frozen numbers; never renumbered):
 *   1  FN claim (RESERVED/SUPERSEDED)      known, NOT activated
 *   2  legacy FN issuance                  known, NOT activated
 *   3  asset issuance                      known; height-gated
 *   4  FINALITY_CERTIFICATE                known; X-pin-gated
 *   5  FINALITY_KEY_EVIDENCE               known; X-pin-gated
 *   6  modern FN PoD creation              known; height-gated
 *   7  FlowMesh FN-seat pre-binding        known; A2 height-gated
 *   8  FlowMesh checkpoint                 known; A3 height-gated
 *   9  FlowMesh vault effect proof         known; A3 height-gated
 *  10  Ethereum bridge record              known; bridge-height-gated
 * Every other (type, version) is UNKNOWN. Unknown -> invalid; known but not
 * activated -> invalid; known and activated -> parsed with that type's exact
 * grammar. There is no generic/user-data record type.
 *
 * STRUCTURE (CheckTransactionMpa):
 *   - an MPA may appear only in the Modern activation context
 *     (Consensus::ModernObjectRulesActive -- the X-pin configuration; production is
 *     fail-closed) — parser recognition never implies activation;
 *   - records must be in strictly increasing canonical order
 *     (payload_type, payload_version, payload bytes lexicographically), which
 *     makes the encoding of a record set unique and forbids duplicates;
 *   - every record's (type, version) must be ACTIVE and its payload must
 *     satisfy the type's size/grammar rule.
 *
 * FINALITY_KEY BINDING (MatchFinalityKeyPairs): exact one-to-one matching of
 * FINALITY_KEY cells and FINALITY_KEY_EVIDENCE records by the deterministic
 * key (validator_key, bls_pubkey, seq) — the cell's commitment and params
 * versus the record's fields. No positional guessing. Duplicate cells with the
 * same key, duplicate evidence, orphan evidence, cells without evidence, and
 * any field mismatch are invalid. Matched pairs are then passed through
 * modern::CheckFinalityKeyTransition (Commit 4) by the caller, in block order,
 * against the derived binding index plus an in-block overlay.
 */

//! Frozen record type numbers (see creation_action.h for 1-10).
inline constexpr uint16_t MPA_TYPE_FINALITY_CERTIFICATE{CREATION_ACTION_FINALITY_CERTIFICATE};
inline constexpr uint16_t MPA_TYPE_FINALITY_KEY_EVIDENCE{CREATION_ACTION_FINALITY_KEY_EVIDENCE};
inline constexpr uint16_t MPA_TYPE_FLOWMESH_CHECKPOINT{CREATION_ACTION_FLOWMESH_CHECKPOINT};
inline constexpr uint16_t MPA_TYPE_FLOWMESH_VAULT_PROOF{CREATION_ACTION_FLOWMESH_VAULT_PROOF};
inline constexpr uint16_t MPA_TYPE_BRIDGE{CREATION_ACTION_BRIDGE};
inline constexpr uint16_t MPA_VERSION_V1{1};

enum class PayloadTypeStatus { UNKNOWN, INACTIVE, ACTIVE };

//! Registry lookup. Historical types 1/2 are permanently inactive; the live
//! creation declarations are independently height-gated.
inline PayloadTypeStatus GetPayloadTypeStatus(const uint16_t type, const uint16_t version,
                                              const Consensus::Params& params,
                                              const std::optional<int> height = std::nullopt)
{
    if (version != MPA_VERSION_V1) return PayloadTypeStatus::UNKNOWN;
    switch (type) {
    case CREATION_ACTION_FN_CLAIM:
    case CREATION_ACTION_LEGACY_FN_ISSUANCE:
        // Permanently reserved/superseded. Recognition never gives these
        // proof-carrying historical claim formats a production path.
        return PayloadTypeStatus::INACTIVE;
    case CREATION_ACTION_ASSET_ISSUANCE:
        return height && Consensus::AssetRulesActive(*height, params)
                   ? PayloadTypeStatus::ACTIVE
                   : PayloadTypeStatus::INACTIVE;
    case CREATION_ACTION_MODERN_FN_POD:
        return height && Consensus::FnPodRulesActive(*height, params)
                   ? PayloadTypeStatus::ACTIVE
                   : PayloadTypeStatus::INACTIVE;
    case CREATION_ACTION_FLOWMESH_SEAT_BINDING:
        return height && Consensus::FlowMeshSeatBindingRulesActive(*height, params)
                   ? PayloadTypeStatus::ACTIVE
                   : PayloadTypeStatus::INACTIVE;
    case MPA_TYPE_FLOWMESH_CHECKPOINT:
    case MPA_TYPE_FLOWMESH_VAULT_PROOF:
        return height && Consensus::FlowMeshRulesActive(*height, params)
                   ? PayloadTypeStatus::ACTIVE
                   : PayloadTypeStatus::INACTIVE;
    case MPA_TYPE_BRIDGE:
        return height && Consensus::BridgeRulesActive(*height, params)
                   ? PayloadTypeStatus::ACTIVE
                   : PayloadTypeStatus::INACTIVE;
    case MPA_TYPE_FINALITY_CERTIFICATE:
    case MPA_TYPE_FINALITY_KEY_EVIDENCE:
        // Live from the X-pin configuration (F = M plumbing); every real
        // network today ships without it, so production stays fail-closed.
        return Consensus::ModernObjectRulesActive(params) ? PayloadTypeStatus::ACTIVE : PayloadTypeStatus::INACTIVE;
    default:
        return PayloadTypeStatus::UNKNOWN;
    }
}

//! Per-type payload size rule (exact or maximum), for known types.
inline bool PayloadSizeAllowed(const uint16_t type, const size_t size)
{
    switch (type) {
    case CREATION_ACTION_FN_CLAIM:
    case CREATION_ACTION_LEGACY_FN_ISSUANCE:
    case CREATION_ACTION_ASSET_ISSUANCE:
        return size <= MAX_CREATION_ACTION_PAYLOAD;
    case MPA_TYPE_FINALITY_CERTIFICATE:
        return size <= FINALITY_CERTIFICATE_RECORD_MAX;
    case MPA_TYPE_FINALITY_KEY_EVIDENCE:
        return size == FINALITY_KEY_EVIDENCE_SIZE;
    case CREATION_ACTION_MODERN_FN_POD:
        return size == MODERN_FN_POD_ACTION_V1_SIZE;
    case CREATION_ACTION_FLOWMESH_SEAT_BINDING:
        return size == FLOWMESH_SEAT_BINDING_ACTION_V1_SIZE;
    case MPA_TYPE_FLOWMESH_CHECKPOINT:
        return size >= FLOWMESH_CHECKPOINT_RECORD_MIN_SIZE &&
               size <= FLOWMESH_CHECKPOINT_RECORD_MAX_SIZE;
    case MPA_TYPE_FLOWMESH_VAULT_PROOF:
        return size >= FLOWMESH_VAULT_PROOF_V1_WIRE_MIN_SIZE &&
               size <= FLOWMESH_VAULT_PROOF_RECORD_MAX_SIZE;
    case MPA_TYPE_BRIDGE:
        return size >= 2 && size <= bridge::MAX_BRIDGE_RECORD_SIZE;
    default:
        return false;
    }
}

//! Canonical order: strictly increasing (type, version, payload).
inline bool MpaRecordLess(const CMpaRecord& a, const CMpaRecord& b)
{
    return std::tie(a.payload_type, a.payload_version, a.payload) < std::tie(b.payload_type, b.payload_version, b.payload);
}

//! Does the transaction's MPA satisfy activation, canonical order, registry and
//! per-type size rules? (Structural only; binding semantics are separate.)
inline bool CheckTransactionMpa(const CTransaction& tx, const Consensus::Params& params,
                                const std::optional<int> height, std::string& error)
{
    const size_t bridge_bindings{BridgeBindingOutputCount(tx)};
    if (tx.mpa.empty()) {
        if (bridge_bindings != 0) {
            error = "mpa-orphan-bridge-binding";
            return false;
        }
        // FN-v2 without its mandatory type-7 record must still fail. A caller
        // without a height can never activate the A2 pre-binding gate and
        // therefore fails closed.
        return CheckFlowMeshSeatBindings(tx, height.value_or(std::numeric_limits<int>::min()),
                                         params, error);
    }
    if (!Consensus::ModernObjectRulesActive(params)) {
        error = "mpa-not-active";
        return false;
    }
    if (tx.mpa.size() > MAX_PAYLOAD_RECORDS_PER_TX) {
        error = "mpa-too-many-records";
        return false;
    }
    bool saw_flowmesh_vault_proof{false};
    bool saw_bridge_record{false};
    for (size_t i = 0; i < tx.mpa.size(); ++i) {
        const CMpaRecord& rec{tx.mpa[i]};
        if (i > 0 && !MpaRecordLess(tx.mpa[i - 1], rec)) {
            error = "mpa-record-order";
            return false;
        }
        if (rec.payload.size() > MAX_PAYLOAD_RECORD_SIZE) {
            error = "mpa-record-too-large";
            return false;
        }
        switch (GetPayloadTypeStatus(rec.payload_type, rec.payload_version, params, height)) {
        case PayloadTypeStatus::UNKNOWN:
            error = "mpa-unknown-type";
            return false;
        case PayloadTypeStatus::INACTIVE:
            error = "mpa-inactive-type";
            return false;
        case PayloadTypeStatus::ACTIVE:
            break;
        }
        if (!PayloadSizeAllowed(rec.payload_type, rec.payload.size())) {
            error = "mpa-bad-record-size";
            return false;
        }
        if (rec.payload_type == MPA_TYPE_FLOWMESH_VAULT_PROOF) {
            if (saw_flowmesh_vault_proof) {
                error = "mpa-multiple-flowmesh-vault-proofs";
                return false;
            }
            saw_flowmesh_vault_proof = true;
        }
        if (rec.payload_type == MPA_TYPE_BRIDGE) {
            if (saw_bridge_record) {
                error = "mpa-multiple-bridge-records";
                return false;
            }
            saw_bridge_record = true;
        }
    }
    // Type 10 consumes the full per-transaction verification budget and its
    // state transition is intentionally self-contained. Keeping it as the
    // sole MPA record prevents zero-cost creation declarations from being
    // coupled to a bridge proof in one consensus object.
    if (saw_bridge_record && tx.mpa.size() != 1) {
        error = "mpa-bridge-record-not-exclusive";
        return false;
    }
    if (!saw_bridge_record && bridge_bindings != 0) {
        error = "mpa-orphan-bridge-binding";
        return false;
    }
    if (saw_bridge_record &&
        !CheckBridgeRecordBinding(tx, tx.mpa.front(), error)) {
        return false;
    }
    // Frame-derived cost is enforced after every cheap outer shape/gate and
    // before any type-specific hash, signature, or proof work.
    if (!CheckTransactionPayloadCost(tx, error)) return false;
    for (const CMpaRecord& rec : tx.mpa) {
        // Type 8/9 have self-delimiting, frozen inner grammars. Parse them
        // after all outer count/size/activation/cost gates, before any BLS or
        // chain-index work. Type 8's exact bitmap width/high bits are checked
        // later against the anchored active-seat count.
        if (rec.payload_type == MPA_TYPE_FLOWMESH_CHECKPOINT &&
            !DecodeFlowMeshCheckpointEnvelopeV1(rec.payload)) {
            error = "mpa-malformed-flowmesh-checkpoint";
            return false;
        }
        if (rec.payload_type == MPA_TYPE_FLOWMESH_VAULT_PROOF &&
            !DecodeFlowMeshVaultProofV1(rec.payload)) {
            error = "mpa-malformed-flowmesh-vault-proof";
            return false;
        }
        if (rec.payload_type == MPA_TYPE_BRIDGE &&
            !bridge::DecodeBridgeMpaRecordV1(rec)) {
            error = "mpa-malformed-bridge-record";
            return false;
        }
    }
    return CheckFlowMeshSeatBindings(tx, height.value_or(std::numeric_limits<int>::min()),
                                     params, error);
}

//! Compatibility wrapper for context-free callers and existing model tests.
//! Height-gated issuance stays inactive unless the caller supplies a height.
inline bool CheckTransactionMpa(const CTransaction& tx, const Consensus::Params& params,
                                std::string& error)
{
    return CheckTransactionMpa(tx, params, std::nullopt, error);
}

//! One matched FINALITY_KEY cell + evidence record.
struct FinalityKeyPair {
    size_t cell_index{0};    // tx.vout index of the cell
    size_t record_index{0};  // tx.mpa index of the evidence record
    uint256 commitment{};    // = validator_key
    FinalityKeyParams params{};
    FinalityKeyEvidence evidence{};
};

//! Deterministic matching key: validator_key || bls_pubkey || seq (BE).
using FinalityKeyMatchKey = std::array<unsigned char, 32 + FinalityKeyParams::SIZE>;

inline FinalityKeyMatchKey MakeFinalityKeyMatchKey(std::span<const unsigned char> validator_key,
                                                  const FinalityKeyParams& params)
{
    FinalityKeyMatchKey key{};
    std::copy(validator_key.begin(), validator_key.begin() + 32, key.begin());
    const auto p{params.Encode()};
    std::copy(p.begin(), p.end(), key.begin() + 32);
    return key;
}

/**
 * Exact one-to-one matching of FINALITY_KEY cells and FINALITY_KEY_EVIDENCE
 * records within one transaction. Fails closed on: malformed cell params,
 * malformed evidence, duplicate cells or duplicate evidence sharing a key,
 * a cell without evidence, evidence without a cell (orphan), field mismatch.
 * Transactions without either are trivially fine (empty output).
 */
inline bool MatchFinalityKeyPairs(const CTransaction& tx, std::vector<FinalityKeyPair>& out, std::string& error)
{
    out.clear();
    std::map<FinalityKeyMatchKey, FinalityKeyPair> cells;
    for (size_t i = 0; i < tx.vout.size(); ++i) {
        const auto cell{ParseMetadataCell(tx.vout[i].scriptPubKey)};
        if (!cell || cell->policy_type != static_cast<uint16_t>(PolicyType::FINALITY_KEY)) continue;
        if (cell->policy_version != POLICY_VERSION_V1) {
            error = "finality-key-cell-version";
            return false;
        }
        const auto params{FinalityKeyParams::Decode(cell->params)};
        if (!params) {
            error = "finality-key-cell-params";
            return false;
        }
        FinalityKeyPair pair;
        pair.cell_index = i;
        pair.commitment = cell->commitment;
        pair.params = *params;
        const auto key{MakeFinalityKeyMatchKey(std::span<const unsigned char>(cell->commitment.begin(), 32), *params)};
        if (!cells.emplace(key, pair).second) {
            error = "finality-key-duplicate-cell";
            return false;
        }
    }
    std::map<FinalityKeyMatchKey, size_t> records;
    for (size_t i = 0; i < tx.mpa.size(); ++i) {
        const CMpaRecord& rec{tx.mpa[i]};
        if (rec.payload_type != MPA_TYPE_FINALITY_KEY_EVIDENCE || rec.payload_version != MPA_VERSION_V1) continue;
        const auto ev{FinalityKeyEvidence::Decode(rec.payload)};
        if (!ev) {
            error = "finality-key-evidence-malformed";
            return false;
        }
        FinalityKeyParams p;
        p.bls_pubkey = ev->bls_pubkey;
        p.seq = ev->seq;
        const auto key{MakeFinalityKeyMatchKey(ev->validator_key, p)};
        if (!records.emplace(key, i).second) {
            error = "finality-key-duplicate-evidence";
            return false;
        }
        const auto cell_it{cells.find(key)};
        if (cell_it == cells.end()) {
            error = "finality-key-orphan-evidence";
            return false;
        }
        cell_it->second.record_index = i;
        cell_it->second.evidence = *ev;
    }
    if (records.size() != cells.size()) {
        error = "finality-key-cell-without-evidence";
        return false;
    }
    for (auto& [key, pair] : cells) out.push_back(pair);
    // out is in key order (deterministic); record_index/cell_index identify positions.
    return true;
}

} // namespace modern

#endif // B3COIN_MODERN_MPA_H
