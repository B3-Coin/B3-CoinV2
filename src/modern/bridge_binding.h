// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_BRIDGE_BINDING_H
#define B3COIN_MODERN_BRIDGE_BINDING_H

#include <crypto/common.h>
#include <hash.h>
#include <modern/creation_action.h>
#include <modern/metadata_cell.h>
#include <modern/policy.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace modern {

/**
 * A type-10 MPA record is outside the legacy txid and signature serialization.
 * Its mandatory zero-valued BRIDGE_RECORD metadata output places a tagged hash
 * of the exact canonical record frame inside the ordinary transaction outputs.
 * For managed withdrawals consensus additionally requires SIGHASH_ALL (or the
 * equivalent Schnorr default), so every owner authorization binds every bridge
 * field, including the Ethereum recipient, without OP_RETURN.
 */
inline constexpr const char* BRIDGE_RECORD_COMMITMENT_TAG{"B3/BRIDGE/RECORD/V1"};

inline std::optional<uint256> BridgeRecordCommitmentV1(const CMpaRecord& record)
{
    if (record.payload_type != CREATION_ACTION_BRIDGE ||
        record.payload_version != POLICY_VERSION_V1) {
        return std::nullopt;
    }
    unsigned char frame[4];
    WriteBE16(frame, record.payload_type);
    WriteBE16(frame + 2, record.payload_version);
    HashWriter writer{TaggedHash(BRIDGE_RECORD_COMMITMENT_TAG)};
    writer << std::span<const unsigned char>(frame, sizeof(frame));
    writer << std::span<const unsigned char>(record.payload);
    return writer.GetSHA256();
}

//! Exact zero-valued output required beside every type-10 MPA record.
inline std::optional<CTxOut> MakeBridgeBindingOutput(const CMpaRecord& record)
{
    const auto commitment{BridgeRecordCommitmentV1(record)};
    if (!commitment) return std::nullopt;
    const auto script{MakeMetadataCellScript(
        static_cast<uint16_t>(PolicyType::BRIDGE_RECORD), POLICY_VERSION_V1,
        *commitment, {})};
    if (!script) return std::nullopt;
    return CTxOut{0, *script};
}

inline size_t BridgeBindingOutputCount(const CTransaction& tx)
{
    size_t count{0};
    for (const CTxOut& out : tx.vout) {
        const auto cell{ParseMetadataCell(out.scriptPubKey)};
        if (cell && cell->policy_type ==
                        static_cast<uint16_t>(PolicyType::BRIDGE_RECORD)) {
            ++count;
        }
    }
    return count;
}

//! Exact one-to-one binding between a type-10 record and its signed output.
inline bool CheckBridgeRecordBinding(const CTransaction& tx,
                                     const CMpaRecord& record,
                                     std::string& error)
{
    const auto expected{MakeBridgeBindingOutput(record)};
    if (!expected) {
        error = "bridge-binding-invalid-record";
        return false;
    }
    size_t count{0};
    bool exact{false};
    for (const CTxOut& out : tx.vout) {
        const auto cell{ParseMetadataCell(out.scriptPubKey)};
        if (!cell || cell->policy_type !=
                         static_cast<uint16_t>(PolicyType::BRIDGE_RECORD)) {
            continue;
        }
        ++count;
        exact = exact || (out.nValue == expected->nValue &&
                          out.scriptPubKey == expected->scriptPubKey);
    }
    if (count == 0) {
        error = "bridge-binding-missing";
        return false;
    }
    if (count != 1) {
        error = "bridge-binding-multiple";
        return false;
    }
    if (!exact) {
        error = "bridge-binding-mismatch";
        return false;
    }
    return true;
}

} // namespace modern

#endif // B3COIN_MODERN_BRIDGE_BINDING_H
