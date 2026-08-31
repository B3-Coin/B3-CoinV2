// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_MODERN_PAYLOAD_COST_H
#define B3COIN_MODERN_PAYLOAD_COST_H

#include <consensus/consensus.h>
#include <modern/creation_action.h>
#include <modern/finality_types.h>
#include <primitives/block.h>
#include <primitives/transaction.h>

#include <cstdint>
#include <string>

namespace modern {

/**
 * Payload verification-cost accounting (frozen 2026-08-23; plan Commit 8).
 * Every (payload_type, payload_version) declares a DETERMINISTIC cost (1 unit
 * ~ 1 us on the reference machine); consensus bounds the sum per transaction
 * (MAX_TX_PAYLOAD_COST) and per block (MAX_BLOCK_PAYLOAD_COST), and both
 * checks run from the record frames alone -- BEFORE any cryptography -- so a
 * block can never force more BLS/BIP340 work than the budget. Relay prices the
 * same cost into the virtual size (policy/policy.h): vsize = max(weight/4,
 * cost x PAYLOAD_COST_TO_VBYTES), so CPU-heavy records are never cheaper to
 * relay than the CPU they consume.
 *
 * Declared costs (frozen): FINALITY_CERTIFICATE 2,000; FINALITY_KEY_EVIDENCE
 * 700; FLOWMESH_SEAT_BINDING 700; FLOWMESH_CHECKPOINT 6,000;
 * FLOWMESH_VAULT_PROOF 500. The type-8 cost admits at most two checkpoints
 * under the frozen 12,000-unit transaction budget. Retired types 1/2 and the
 * lightweight, non-cryptographic type 3/6 declarations carry cost 0; unknown
 * types are rejected by the registry before cost is consulted.
 */
inline constexpr int64_t FLOWMESH_SEAT_BINDING_VERIFY_COST{700};
inline constexpr int64_t FLOWMESH_CHECKPOINT_VERIFY_COST{6000};
inline constexpr int64_t FLOWMESH_VAULT_PROOF_VERIFY_COST{500};

inline int64_t PayloadRecordVerifyCost(const uint16_t type, const uint16_t version)
{
    if (version != 1) return 0;
    switch (type) {
    case CREATION_ACTION_FINALITY_CERTIFICATE: return FINALITY_CERTIFICATE_VERIFY_COST;
    case CREATION_ACTION_FINALITY_KEY_EVIDENCE: return FINALITY_KEY_EVIDENCE_VERIFY_COST;
    case CREATION_ACTION_FLOWMESH_SEAT_BINDING: return FLOWMESH_SEAT_BINDING_VERIFY_COST;
    case CREATION_ACTION_FLOWMESH_CHECKPOINT: return FLOWMESH_CHECKPOINT_VERIFY_COST;
    case CREATION_ACTION_FLOWMESH_VAULT_PROOF: return FLOWMESH_VAULT_PROOF_VERIFY_COST;
    default: return 0;
    }
}

inline int64_t PayloadVerifyCost(const CTransaction& tx)
{
    int64_t cost{0};
    for (const CMpaRecord& rec : tx.mpa) cost += PayloadRecordVerifyCost(rec.payload_type, rec.payload_version);
    return cost;
}

inline bool CheckTransactionPayloadCost(const CTransaction& tx, std::string& error)
{
    if (PayloadVerifyCost(tx) > MAX_TX_PAYLOAD_COST) {
        error = "bad-payload-cost";
        return false;
    }
    return true;
}

inline int64_t BlockPayloadVerifyCost(const CBlock& block)
{
    int64_t cost{0};
    for (const auto& tx : block.vtx) cost += PayloadVerifyCost(*tx);
    return cost;
}

inline bool CheckBlockPayloadCost(const CBlock& block, std::string& error)
{
    if (BlockPayloadVerifyCost(block) > MAX_BLOCK_PAYLOAD_COST) {
        error = "bad-block-payload-cost";
        return false;
    }
    return true;
}

} // namespace modern

#endif // B3COIN_MODERN_PAYLOAD_COST_H
