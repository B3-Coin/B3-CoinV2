// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_BRIDGE_EXEC_CHAIN_H
#define B3COIN_BRIDGE_EXEC_CHAIN_H

#include <bridge/rlp.h>
#include <crypto/keccak256.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/** Execution-header ancestry proofs (header-only, not reachable from
 *  consensus). The sync-committee light client proves ONE finalized
 *  execution header; a deposit usually sits in an EARLIER block. The gap is
 *  closed by the keccak parent-hash chain: full RLP execution block
 *  headers, newest first, where
 *
 *    keccak(headers[0]) == the PROVEN finalized block_hash
 *    keccak(headers[i+1]) == headers[i].parentHash
 *
 *  walking down to the deposit block, whose receiptsRoot then anchors the
 *  Merkle-Patricia receipt proof. Every link is a keccak preimage — nothing
 *  in the chain is trusted. This is the same object a stage-4 consensus
 *  deposit proof will carry.
 */
namespace bridge {

struct ExecAncestor {
    uint64_t block_number{0};
    uint256 block_hash{};
    uint256 receipts_root{};
};

namespace exec_detail {

//! Parse the fields we need from a full RLP execution block header:
//! [0]=parentHash, [5]=receiptsRoot, [8]=number (canonical big-endian).
inline std::optional<ExecAncestor> ParseHeader(std::span<const unsigned char> rlp_bytes,
                                               uint256& parent_out)
{
    const auto top{RlpDecode(rlp_bytes)};
    if (!top || !top->is_list) return std::nullopt;
    const auto f{RlpChildren(*top)};
    if (!f || f->size() < 16) return std::nullopt; // post-London headers have >= 16 fields
    const RlpItem& parent{(*f)[0]};
    const RlpItem& receipts{(*f)[5]};
    const RlpItem& number{(*f)[8]};
    if (parent.is_list || parent.payload.size() != 32) return std::nullopt;
    if (receipts.is_list || receipts.payload.size() != 32) return std::nullopt;
    if (number.is_list || number.payload.size() > 8) return std::nullopt;
    if (!number.payload.empty() && number.payload[0] == 0x00) return std::nullopt;
    ExecAncestor out;
    parent_out = uint256{std::span<const unsigned char>{parent.payload}};
    out.receipts_root = uint256{std::span<const unsigned char>{receipts.payload}};
    for (unsigned char b : number.payload) out.block_number = (out.block_number << 8) | b;
    uint256 h;
    Keccak256().Write(rlp_bytes).Finalize(h);
    out.block_hash = h;
    return out;
}

} // namespace exec_detail

/** Walk the parent-hash chain from the proven block hash down to
 *  `target_block`. `headers` are full RLP execution block headers ordered
 *  NEWEST FIRST; headers[0] must hash to `proven_block_hash` and the last
 *  header must be the target block. Returns the target's ancestor record
 *  (with its proven receipts_root) or nullopt on any break. */
inline std::optional<ExecAncestor> VerifyExecAncestry(
    const uint256& proven_block_hash, uint64_t target_block,
    const std::vector<std::vector<unsigned char>>& headers)
{
    if (headers.empty()) return std::nullopt;
    uint256 want{proven_block_hash};
    uint64_t want_number{0};
    bool have_number{false};
    for (size_t i = 0; i < headers.size(); ++i) {
        uint256 parent;
        const auto h{exec_detail::ParseHeader(headers[i], parent)};
        if (!h) return std::nullopt;
        if (h->block_hash != want) return std::nullopt;
        if (have_number && h->block_number != want_number) return std::nullopt;
        if (h->block_number == target_block) {
            return i + 1 == headers.size() ? h : std::nullopt; // no trailing junk
        }
        if (h->block_number < target_block) return std::nullopt; // walked past it
        want = parent;
        want_number = h->block_number - 1;
        have_number = true;
    }
    return std::nullopt; // chain ended before the target
}

} // namespace bridge

#endif // B3COIN_BRIDGE_EXEC_CHAIN_H
