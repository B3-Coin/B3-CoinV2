// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_MODERN_PAYLOAD_ROOT_H
#define B3COIN_MODERN_PAYLOAD_ROOT_H

#include <consensus/merkle.h>
#include <consensus/params.h>
#include <crypto/common.h>
#include <hash.h>
#include <modern/metadata_cell.h>
#include <modern/policy.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace modern {

/**
 * MODERN_PAYLOAD_ROOT — the B3-native (Path B) commitment of every Modern
 * Payload Area in a block into the ordinary block hash (plan Commit 7;
 * doc/design/b3-modern-payload-area.md §2–§4).
 *
 * Construction, for every transaction i in exact block order (i = 0 is the
 * coinbase):
 *
 *   section_hash[i] = TaggedHash("B3/MPA/SECTION/V1", canonical MPA section bytes of tx i)
 *                     if tx i carries an MPA, else 0x00..00 (32 zero bytes)
 *   leaf[i]         = TaggedHash("B3/MPA/LEAF/V1", index u32 BIG-ENDIAN || section_hash[i])
 *   payload_root    = ComputeMerkleRoot([leaf[0..n-1]])   -- the existing block
 *                     Merkle algorithm (odd levels duplicate the last node)
 *
 * The index is written explicitly as 4 big-endian bytes (never the host
 * serializer's endianness), like every other frozen finality layout. Leaves
 * carry position + section hash only: no txid, wtxid, ptxid, output or
 * commitment enters a leaf, so payload_root never depends on anything that
 * depends on payload_root (the coinbase txid and ptxid are downstream of the
 * root cell) — acyclic even when the coinbase itself carries an MPA.
 *
 * Cell (policy 8, metadata, zero value, never a coin): coinbase-only,
 * commitment = payload_root, params EMPTY. Consensus (CheckBlockPayloadRoot):
 * no MPA in the block => no root cell anywhere; at least one MPA => exactly one
 * valid root cell in the coinbase. Missing, duplicate, wrong root, non-coinbase
 * placement, non-empty params, wrong value/type/version => block invalid.
 * There is no "empty payload root" encoding. Activation: policy 8 and the MPA
 * stay fail-closed on real networks (metadata_cell.h / mpa.h); this rule is
 * consistent with that automatically (no MPA can exist, no cell may exist).
 */

inline constexpr const char* MPA_SECTION_TAG{"B3/MPA/SECTION/V1"};
inline constexpr const char* MPA_LEAF_TAG{"B3/MPA/LEAF/V1"};

//! The canonical MPA section bytes of a transaction (the flag-0x02 section
//! exactly as serialized under TX_MODERN); empty for a transaction without MPA.
inline std::vector<unsigned char> CanonicalMpaSectionBytes(const CTransaction& tx)
{
    if (!tx.HasMpa()) return {};
    DataStream ss;
    SerializeMpaSection(ss, tx.mpa);
    const auto s{ss.str()};
    return std::vector<unsigned char>(s.begin(), s.end());
}

//! section_hash[i]
inline uint256 MpaSectionHash(const CTransaction& tx)
{
    if (!tx.HasMpa()) return uint256{};
    HashWriter w{TaggedHash(MPA_SECTION_TAG)};
    w << std::span<const unsigned char>(CanonicalMpaSectionBytes(tx));
    return w.GetSHA256();
}

//! leaf[i] from an explicit index and section hash.
inline uint256 PayloadLeaf(const uint32_t index, const uint256& section_hash)
{
    unsigned char pre[4 + 32];
    WriteBE32(pre, index);
    std::copy(section_hash.begin(), section_hash.end(), pre + 4);
    HashWriter w{TaggedHash(MPA_LEAF_TAG)};
    w << std::span<const unsigned char>(pre, sizeof(pre));
    return w.GetSHA256();
}

inline std::vector<uint256> PayloadLeaves(const CBlock& block)
{
    std::vector<uint256> leaves;
    leaves.reserve(block.vtx.size());
    for (size_t i = 0; i < block.vtx.size(); ++i) {
        leaves.push_back(PayloadLeaf(static_cast<uint32_t>(i), MpaSectionHash(*block.vtx[i])));
    }
    return leaves;
}

//! payload_root over the block's transactions in block order.
inline uint256 ComputePayloadRoot(const CBlock& block)
{
    return ComputeMerkleRoot(PayloadLeaves(block));
}

inline bool BlockHasAnyMpa(const CBlock& block)
{
    for (const auto& tx : block.vtx) {
        if (tx->HasMpa()) return true;
    }
    return false;
}

//! The canonical MODERN_PAYLOAD_ROOT cell for a root.
inline CScript MakePayloadRootCellScript(const uint256& payload_root)
{
    const auto script{MakeMetadataCellScript(static_cast<uint16_t>(PolicyType::MODERN_PAYLOAD_ROOT), POLICY_VERSION_V1,
                                             payload_root, {})};
    return *script; // empty params always fit
}

//! Is this output a (well-formed) MODERN_PAYLOAD_ROOT cell? Returns its commitment.
inline std::optional<uint256> ParsePayloadRootCell(const CTxOut& out)
{
    const auto cell{ParseMetadataCell(out.scriptPubKey)};
    if (!cell || cell->policy_type != static_cast<uint16_t>(PolicyType::MODERN_PAYLOAD_ROOT)) return std::nullopt;
    return cell->commitment;
}

/**
 * Block-level consensus rule (modern era). Cheap: one pass over outputs and,
 * only when an MPA exists, one Merkle computation over n leaves.
 */
inline bool CheckBlockPayloadRoot(const CBlock& block, std::string& error)
{
    if (block.vtx.empty()) {
        error = "payload-root-no-coinbase";
        return false;
    }
    size_t coinbase_cells{0};
    std::optional<uint256> committed;
    for (size_t t = 0; t < block.vtx.size(); ++t) {
        const CTransaction& tx{*block.vtx[t]};
        for (const CTxOut& out : tx.vout) {
            const auto cell{ParseMetadataCell(out.scriptPubKey)};
            if (!cell || cell->policy_type != static_cast<uint16_t>(PolicyType::MODERN_PAYLOAD_ROOT)) continue;
            if (t != 0) {
                error = "payload-root-not-in-coinbase";
                return false;
            }
            if (cell->policy_version != POLICY_VERSION_V1) {
                error = "payload-root-version";
                return false;
            }
            if (!cell->params.empty()) {
                error = "payload-root-params";
                return false;
            }
            if (out.nValue != 0) {
                error = "payload-root-value";
                return false;
            }
            ++coinbase_cells;
            committed = cell->commitment;
        }
    }
    const bool has_mpa{BlockHasAnyMpa(block)};
    if (!has_mpa) {
        if (coinbase_cells != 0) {
            error = "payload-root-without-mpa";
            return false;
        }
        return true;
    }
    if (coinbase_cells == 0) {
        error = "payload-root-missing";
        return false;
    }
    if (coinbase_cells > 1) {
        error = "payload-root-duplicate";
        return false;
    }
    if (*committed != ComputePayloadRoot(block)) {
        error = "payload-root-mismatch";
        return false;
    }
    return true;
}

} // namespace modern

#endif // B3COIN_MODERN_PAYLOAD_ROOT_H
