// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_BRIDGE_MPT_H
#define B3COIN_BRIDGE_MPT_H

#include <bridge/rlp.h>
#include <crypto/keccak256.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/** Ethereum Merkle-Patricia trie INCLUSION proof verification for the gated
 *  type-10 consensus bridge.
 *
 *  Target use: proving one receipt against a finalized execution header's
 *  receiptsRoot (key = rlp(tx_index)). The verifier is deliberately
 *  inclusion-only — the deposit mint leg never needs exclusion proofs —
 *  and strict: every proof node must be consumed, every hash link must
 *  match, and the walk must end exactly on a value with the key exhausted.
 */
namespace bridge {

namespace mpt_detail {

inline uint256 KeccakOf(std::span<const unsigned char> data)
{
    uint256 out;
    Keccak256().Write(data).Finalize(out);
    return out;
}

//! Expand a byte string key into nibbles (big-endian nibble order).
inline std::vector<unsigned char> ToNibbles(std::span<const unsigned char> key)
{
    std::vector<unsigned char> out;
    out.reserve(key.size() * 2);
    for (unsigned char b : key) {
        out.push_back(b >> 4);
        out.push_back(b & 0x0f);
    }
    return out;
}

//! Decode a hex-prefix encoded path. Returns nibbles; sets is_leaf.
inline std::optional<std::vector<unsigned char>> DecodeHexPrefix(std::span<const unsigned char> hp,
                                                                 bool& is_leaf)
{
    if (hp.empty()) return std::nullopt;
    const unsigned char flags{static_cast<unsigned char>(hp[0] >> 4)};
    if (flags > 3) return std::nullopt;
    is_leaf = (flags & 2) != 0;
    const bool odd{(flags & 1) != 0};
    std::vector<unsigned char> out;
    if (odd) {
        out.push_back(hp[0] & 0x0f);
    } else if ((hp[0] & 0x0f) != 0) {
        return std::nullopt; // even path: low nibble of the prefix byte must be zero
    }
    for (size_t i = 1; i < hp.size(); ++i) {
        out.push_back(hp[i] >> 4);
        out.push_back(hp[i] & 0x0f);
    }
    return out;
}

} // namespace mpt_detail

/** Verify an inclusion proof.
 *
 *  root        keccak root of the trie (e.g. header receiptsRoot)
 *  key         the exact key bytes (receipts: rlp(tx_index))
 *  proof       trie nodes ordered from the root, each the full RLP node encoding
 *
 *  Returns the value bytes on success (copied out), std::nullopt on ANY
 *  malformation, mismatch, unconsumed proof node, or absence of the key.
 */
inline std::optional<std::vector<unsigned char>> VerifyMptProof(
    const uint256& root,
    std::span<const unsigned char> key,
    const std::vector<std::vector<unsigned char>>& proof)
{
    using namespace mpt_detail;
    if (proof.empty()) return std::nullopt;

    const std::vector<unsigned char> nib{ToNibbles(key)};
    size_t key_pos{0};
    size_t proof_pos{0};

    // The node we are standing on, as raw encoded bytes. Starts at proof[0],
    // which must hash to the root.
    if (KeccakOf(proof[0]) != root) return std::nullopt;
    std::span<const unsigned char> node_bytes{proof[0]};
    ++proof_pos;

    while (true) {
        const auto node{RlpDecode(node_bytes)};
        if (!node || !node->is_list) return std::nullopt;
        const auto items{RlpChildren(*node)};
        if (!items) return std::nullopt;

        // Resolve a child reference: 32-byte hash -> next proof node;
        // inline list -> embedded node; anything else is invalid mid-walk.
        auto descend = [&](const RlpItem& child) -> bool {
            if (!child.is_list && child.payload.size() == 32) {
                if (proof_pos >= proof.size()) return false;
                const uint256 want{uint256(std::span<const unsigned char>{child.payload})};
                if (KeccakOf(proof[proof_pos]) != want) return false;
                node_bytes = proof[proof_pos];
                ++proof_pos;
                return true;
            }
            if (child.is_list) {
                // Inline node: its full encoding must be < 32 bytes.
                const auto enc{RlpEncodedSpan(child, node_bytes)};
                if (enc.size() >= 32) return false;
                node_bytes = enc;
                return true;
            }
            return false;
        };

        if (items->size() == 17) { // branch node
            if (key_pos == nib.size()) {
                const RlpItem& val{(*items)[16]};
                if (val.is_list || val.payload.empty()) return std::nullopt;
                if (proof_pos != proof.size()) return std::nullopt;
                return std::vector<unsigned char>(val.payload.begin(), val.payload.end());
            }
            const unsigned char branch{nib[key_pos]};
            ++key_pos;
            const RlpItem& child{(*items)[branch]};
            if (!child.is_list && child.payload.empty()) return std::nullopt; // absent child
            if (!descend(child)) return std::nullopt;
            continue;
        }
        if (items->size() == 2) { // leaf or extension
            const RlpItem& hp{(*items)[0]};
            if (hp.is_list) return std::nullopt;
            bool is_leaf{false};
            const auto path{DecodeHexPrefix(hp.payload, is_leaf)};
            if (!path) return std::nullopt;
            if (path->size() > nib.size() - key_pos) return std::nullopt;
            for (size_t i = 0; i < path->size(); ++i) {
                if ((*path)[i] != nib[key_pos + i]) return std::nullopt;
            }
            key_pos += path->size();
            if (is_leaf) {
                if (key_pos != nib.size()) return std::nullopt;
                const RlpItem& val{(*items)[1]};
                if (val.is_list || val.payload.empty()) return std::nullopt;
                if (proof_pos != proof.size()) return std::nullopt;
                return std::vector<unsigned char>(val.payload.begin(), val.payload.end());
            }
            if (path->empty()) return std::nullopt; // extension must consume nibbles
            if (!descend((*items)[1])) return std::nullopt;
            continue;
        }
        return std::nullopt;
    }
}

} // namespace bridge

#endif // B3COIN_BRIDGE_MPT_H
