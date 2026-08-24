// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_BRIDGE_RLP_H
#define B3COIN_BRIDGE_RLP_H

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/** Strict RLP (Recursive Length Prefix) decoding for the Ethereum bridge
 *  mint leg (bridge proposal stage 2; test-only/header-only per the staged
 *  build order — nothing here is reachable from consensus).
 *
 *  The decoder is CANONICAL-STRICT: any encoding a conforming Ethereum
 *  encoder would never emit (non-minimal length-of-length, a long form
 *  where the short form fits, a single byte < 0x80 wrapped in a string
 *  header) is rejected. Proof verification must never accept two byte
 *  strings for one logical item.
 *
 *  Items are spans over the caller's buffer — zero-copy; the caller keeps
 *  the underlying bytes alive.
 */
namespace bridge {

struct RlpItem {
    bool is_list{false};
    //! Payload only (list body or string bytes), header stripped.
    std::span<const unsigned char> payload{};
};

namespace rlp_detail {
//! Decode the item starting at in[0]. On success sets consumed to the full
//! encoded size (header + payload) and returns the item.
inline std::optional<RlpItem> DecodeOne(std::span<const unsigned char> in, size_t& consumed)
{
    if (in.empty()) return std::nullopt;
    const unsigned char b0{in[0]};
    // Single byte 0x00..0x7f encodes itself.
    if (b0 <= 0x7f) {
        consumed = 1;
        return RlpItem{false, in.subspan(0, 1)};
    }
    auto read_len = [&](size_t lenlen, size_t& out_len) -> bool {
        if (in.size() < 1 + lenlen) return false;
        if (in[1] == 0x00) return false; // leading zero: non-canonical
        uint64_t v{0};
        for (size_t i = 0; i < lenlen; ++i) {
            if (v > (UINT64_MAX >> 8)) return false;
            v = (v << 8) | in[1 + i];
        }
        if (v < 56) return false; // long form where short form fits
        if (v > in.size() - 1 - lenlen) return false;
        out_len = static_cast<size_t>(v);
        return true;
    };
    if (b0 <= 0xb7) { // short string, 0..55 bytes
        const size_t len{static_cast<size_t>(b0 - 0x80)};
        if (in.size() < 1 + len) return std::nullopt;
        if (len == 1 && in[1] <= 0x7f) return std::nullopt; // must self-encode
        consumed = 1 + len;
        return RlpItem{false, in.subspan(1, len)};
    }
    if (b0 <= 0xbf) { // long string
        const size_t lenlen{static_cast<size_t>(b0 - 0xb7)};
        size_t len;
        if (!read_len(lenlen, len)) return std::nullopt;
        consumed = 1 + lenlen + len;
        return RlpItem{false, in.subspan(1 + lenlen, len)};
    }
    if (b0 <= 0xf7) { // short list, payload 0..55 bytes
        const size_t len{static_cast<size_t>(b0 - 0xc0)};
        if (in.size() < 1 + len) return std::nullopt;
        consumed = 1 + len;
        return RlpItem{true, in.subspan(1, len)};
    }
    // long list
    const size_t lenlen{static_cast<size_t>(b0 - 0xf7)};
    size_t len;
    if (!read_len(lenlen, len)) return std::nullopt;
    consumed = 1 + lenlen + len;
    return RlpItem{true, in.subspan(1 + lenlen, len)};
}
} // namespace rlp_detail

//! Decode a buffer that must contain EXACTLY one RLP item.
inline std::optional<RlpItem> RlpDecode(std::span<const unsigned char> in)
{
    size_t consumed{0};
    auto item{rlp_detail::DecodeOne(in, consumed)};
    if (!item || consumed != in.size()) return std::nullopt;
    return item;
}

//! Split a list item's payload into its child items (strict; empty list ok).
inline std::optional<std::vector<RlpItem>> RlpChildren(const RlpItem& list)
{
    if (!list.is_list) return std::nullopt;
    std::vector<RlpItem> out;
    std::span<const unsigned char> rest{list.payload};
    while (!rest.empty()) {
        size_t consumed{0};
        auto child{rlp_detail::DecodeOne(rest, consumed)};
        if (!child) return std::nullopt;
        out.push_back(*child);
        rest = rest.subspan(consumed);
    }
    return out;
}

//! The full encoding (header + payload) of a decoded child, reconstructed
//! from its payload span position inside the parent buffer. Needed by the
//! MPT walker to hash inline nodes exactly as encoded.
inline std::span<const unsigned char> RlpEncodedSpan(const RlpItem& item,
                                                     std::span<const unsigned char> parent)
{
    // The item's payload lies inside parent; the header directly precedes it.
    const unsigned char* pay{item.payload.data()};
    const unsigned char* base{parent.data()};
    size_t start{static_cast<size_t>(pay - base)};
    // Header length: recompute from payload size / first byte.
    size_t header{1};
    const size_t n{item.payload.size()};
    if (!item.is_list && n == 1 && item.payload[0] <= 0x7f) {
        header = 0;
    } else if (n >= 56) {
        size_t len{n}, lenlen{0};
        while (len) { ++lenlen; len >>= 8; }
        header = 1 + lenlen;
    }
    return parent.subspan(start - header, header + n);
}

//! Minimal encoder (test/key construction: receipts-trie keys are rlp(index)).
inline std::vector<unsigned char> RlpEncodeBytes(std::span<const unsigned char> data)
{
    std::vector<unsigned char> out;
    const size_t n{data.size()};
    if (n == 1 && data[0] <= 0x7f) {
        out.push_back(data[0]);
        return out;
    }
    if (n <= 55) {
        out.push_back(static_cast<unsigned char>(0x80 + n));
    } else {
        std::vector<unsigned char> lenbytes;
        size_t len{n};
        while (len) { lenbytes.insert(lenbytes.begin(), static_cast<unsigned char>(len & 0xff)); len >>= 8; }
        out.push_back(static_cast<unsigned char>(0xb7 + lenbytes.size()));
        out.insert(out.end(), lenbytes.begin(), lenbytes.end());
    }
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

//! Canonical RLP of an unsigned integer (big-endian, no leading zeros; 0 = empty string).
inline std::vector<unsigned char> RlpEncodeUint64(uint64_t v)
{
    std::vector<unsigned char> be;
    while (v) { be.insert(be.begin(), static_cast<unsigned char>(v & 0xff)); v >>= 8; }
    return RlpEncodeBytes(be);
}

} // namespace bridge

#endif // B3COIN_BRIDGE_RLP_H
