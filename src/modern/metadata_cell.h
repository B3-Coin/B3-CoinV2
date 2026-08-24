// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_MODERN_METADATA_CELL_H
#define B3COIN_MODERN_METADATA_CELL_H

#include <consensus/era.h>
#include <consensus/params.h>
#include <crypto/common.h>
#include <modern/policy.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace modern {

/**
 * METADATA CELLS — the v1 on-chain carrier of the zero-value Modern policy
 * cells that are consensus-committed but NEVER enter the spendable UTXO
 * set (owner rulings 2026-08-23; implementation plan Commit 3).
 *
 * Applies to exactly the frozen metadata policy types:
 *   6 FINALITY_CERT, 7 FINALITY_KEY, 8 MODERN_PAYLOAD_ROOT.
 *
 * Carrier (one byte representation per logical cell):
 *
 *     PUSH(payload) OP_DROP OP_FALSE
 *     payload = "B3MC" || policy_type u16 BE || policy_version u16 BE
 *               || commitment[32] || params[0..MAX_POLICY_PARAMS_SIZE]
 *
 * Recognition is deterministic on the magic: ANY first push whose data
 * begins with "B3MC" — under any push encoding, any length — CLAIMS to be a
 * metadata cell, and a claiming output that violates any rule below is
 * INVALID, never silently reinterpreted as an ordinary output (the STAKE
 * carrier's discipline). A well-formed cell:
 *   - the push is the minimal encoding of a 40..120-byte payload,
 *   - the script is exactly that push, OP_DROP, OP_FALSE — nothing else,
 *   - policy_type is one of 6/7/8, params <= 80 bytes (the permanent
 *     ModernOutput bound; the cell IS a ModernOutput on the wire),
 *   - the output value is exactly 0.
 *
 * Semantics:
 *   - NOT OP_RETURN: the script is not `IsUnspendable()`; it is unspendable
 *     because it evaluates to false AND because consensus never adds it to
 *     the UTXO set (AddCoins skips it in the modern era; DisconnectBlock
 *     skips it symmetrically). No data-carrier semantics exist: the payload
 *     is a typed policy cell, and its params are bounded typed state; large
 *     evidence travels in the Modern Payload Area, never here.
 *   - ACTIVATION = the F = M plumbing (Consensus::ModernObjectRulesActive):
 *     cells are live exactly when H, X and the Modern-PoS rule set are all
 *     pinned. Every real network today ships without them, so every claiming
 *     output stays invalid there (fail closed) until the X-pin release.
 *   - The legacy era (height <= H) is untouched: the exclusion is applied
 *     only where Consensus::GetB3Era() == MODERN.
 */

inline constexpr std::array<unsigned char, 4> METADATA_CELL_MAGIC{'B', '3', 'M', 'C'};
inline constexpr size_t METADATA_CELL_HEADER_SIZE{4 + 2 + 2 + 32};
inline constexpr size_t METADATA_CELL_MIN_PAYLOAD{METADATA_CELL_HEADER_SIZE};
inline constexpr size_t METADATA_CELL_MAX_PAYLOAD{METADATA_CELL_HEADER_SIZE + MAX_POLICY_PARAMS_SIZE};
static_assert(METADATA_CELL_MAX_PAYLOAD == 120);
static_assert(METADATA_CELL_MAX_PAYLOAD <= MAX_SCRIPT_ELEMENT_SIZE);

struct MetadataCell {
    uint16_t policy_type{0};
    uint16_t policy_version{0};
    uint256 commitment{};
    std::vector<unsigned char> params{};
};

inline constexpr bool IsMetadataCellPolicyType(const uint16_t policy_type)
{
    return policy_type == static_cast<uint16_t>(PolicyType::FINALITY_CERT) ||
           policy_type == static_cast<uint16_t>(PolicyType::FINALITY_KEY) ||
           policy_type == static_cast<uint16_t>(PolicyType::MODERN_PAYLOAD_ROOT);
}

//! Activation of the metadata carrier for (type, version). FALSE on every
//! real network at this stage; fixtures may switch it on to exercise the
//! UTXO-exclusion and undo machinery. Later commits replace this with the
//! real per-type activation rules.
inline bool IsMetadataCellActive(const uint16_t policy_type, const uint16_t policy_version,
                                 const Consensus::Params& params)
{
    return Consensus::ModernObjectRulesActive(params) && IsMetadataCellPolicyType(policy_type) &&
           policy_version == POLICY_VERSION_V1;
}

//! Canonical carrier for a cell; params must be <= MAX_POLICY_PARAMS_SIZE.
inline std::optional<CScript> MakeMetadataCellScript(const uint16_t policy_type, const uint16_t policy_version,
                                                     const uint256& commitment,
                                                     std::span<const unsigned char> params)
{
    if (params.size() > MAX_POLICY_PARAMS_SIZE) return std::nullopt;
    std::vector<unsigned char> payload;
    payload.reserve(METADATA_CELL_HEADER_SIZE + params.size());
    payload.insert(payload.end(), METADATA_CELL_MAGIC.begin(), METADATA_CELL_MAGIC.end());
    unsigned char tv[4];
    WriteBE16(tv, policy_type);
    WriteBE16(tv + 2, policy_version);
    payload.insert(payload.end(), tv, tv + 4);
    payload.insert(payload.end(), commitment.begin(), commitment.end());
    payload.insert(payload.end(), params.begin(), params.end());
    return CScript() << payload << OP_DROP << OP_FALSE; // CScript << vector = minimal push
}

//! Does the first push of the script begin with the metadata magic (any
//! push encoding, any length)? A claim, not a validity check.
inline bool ClaimsMetadataCell(const CScript& script)
{
    CScript::const_iterator pc{script.begin()};
    opcodetype op;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, op, data)) return false;
    if (op > OP_PUSHDATA4) return false; // not a data push
    if (data.size() < METADATA_CELL_MAGIC.size()) return false;
    return std::equal(METADATA_CELL_MAGIC.begin(), METADATA_CELL_MAGIC.end(), data.begin());
}

//! Strict parse: exactly PUSH(payload) OP_DROP OP_FALSE with a minimal push
//! of a 40..120-byte payload and a metadata policy type. Nullopt otherwise.
inline std::optional<MetadataCell> ParseMetadataCell(const CScript& script)
{
    CScript::const_iterator pc{script.begin()};
    opcodetype op;
    std::vector<unsigned char> payload;
    const CScript::const_iterator push_begin{pc};
    if (!script.GetOp(pc, op, payload)) return std::nullopt;
    if (op > OP_PUSHDATA4) return std::nullopt;
    if (payload.size() < METADATA_CELL_MIN_PAYLOAD || payload.size() > METADATA_CELL_MAX_PAYLOAD) return std::nullopt;
    if (!std::equal(METADATA_CELL_MAGIC.begin(), METADATA_CELL_MAGIC.end(), payload.begin())) return std::nullopt;
    // Minimal push encoding: the bytes consumed must equal the minimal form.
    const CScript minimal{CScript() << payload};
    if (static_cast<size_t>(pc - push_begin) != minimal.size() ||
        !std::equal(minimal.begin(), minimal.end(), push_begin)) {
        return std::nullopt;
    }
    std::vector<unsigned char> rest;
    if (!script.GetOp(pc, op, rest) || op != OP_DROP) return std::nullopt;
    if (!script.GetOp(pc, op, rest) || op != OP_FALSE) return std::nullopt;
    if (pc != script.end()) return std::nullopt; // nothing may follow
    MetadataCell cell;
    cell.policy_type = ReadBE16(payload.data() + 4);
    cell.policy_version = ReadBE16(payload.data() + 6);
    if (!IsMetadataCellPolicyType(cell.policy_type)) return std::nullopt;
    std::copy(payload.begin() + 8, payload.begin() + 40, cell.commitment.begin());
    cell.params.assign(payload.begin() + METADATA_CELL_HEADER_SIZE, payload.end());
    return cell;
}

//! A well-formed metadata cell (carrier grammar only; not activation).
inline bool IsMetadataCell(const CScript& script)
{
    return ParseMetadataCell(script).has_value();
}

/**
 * Transaction-level consensus rule (modern era): every output that CLAIMS
 * to be a metadata cell must be well-formed, zero-valued, of a metadata
 * policy type, and of an ACTIVATED (type, version); otherwise the
 * transaction is invalid. Outputs that do not claim are untouched.
 */
inline bool CheckMetadataCellOutputs(const CTransaction& tx, const Consensus::Params& params, std::string& error)
{
    for (size_t i = 0; i < tx.vout.size(); ++i) {
        const CTxOut& out{tx.vout[i]};
        if (!ClaimsMetadataCell(out.scriptPubKey)) continue;
        const auto cell{ParseMetadataCell(out.scriptPubKey)};
        if (!cell) {
            error = "malformed metadata cell at output " + std::to_string(i);
            return false;
        }
        if (out.nValue != 0) {
            error = "metadata cell must be zero-valued at output " + std::to_string(i);
            return false;
        }
        if (!IsMetadataCellActive(cell->policy_type, cell->policy_version, params)) {
            error = "inactive metadata policy " + std::to_string(cell->policy_type) + " v" +
                    std::to_string(cell->policy_version) + " at output " + std::to_string(i);
            return false;
        }
    }
    return true;
}

} // namespace modern

#endif // B3COIN_MODERN_METADATA_CELL_H
