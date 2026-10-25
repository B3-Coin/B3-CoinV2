// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/smt.h>

#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/hashes.h>
#include <hash.h>
#include <streams.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <ios>
#include <utility>

namespace flowmesh2 {
namespace {

bool BitmapBit(std::span<const unsigned char> bitmap, size_t i) { return ((bitmap[i / 8] >> (7 - i % 8)) & 1) != 0; }

//! The first leaf of `leaves` (all sharing `depth` bits) whose bit `depth` is 1.
size_t SplitAt(std::span<const SmtLeaf> leaves, size_t depth)
{
    const auto it{std::partition_point(leaves.begin(), leaves.end(), [depth](const SmtLeaf& leaf) { return !SmtBit(leaf.path, depth); })};
    return static_cast<size_t>(it - leaves.begin());
}

//! The hash of the subtree holding `leaves`, which share their first `depth` bits.
uint256 SubtreeHash(std::span<const SmtLeaf> leaves, size_t depth)
{
    if (leaves.empty()) return SMT_EMPTY;
    if (leaves.size() == 1) return SmtLeafHash(leaves.front().path, leaves.front().value);
    const size_t split{SplitAt(leaves, depth)};
    return SmtNodeHash(SubtreeHash(leaves.first(split), depth + 1), SubtreeHash(leaves.subspan(split), depth + 1));
}

bool StrictlyAscending(std::span<const SmtLeaf> leaves)
{
    for (size_t i{1}; i < leaves.size(); ++i) {
        if (!SmtPathLess(leaves[i - 1].path, leaves[i].path)) return false;
    }
    return true;
}

constexpr Error Unordered() { return Error{ErrorCode::INVALID_INPUT, "SMT leaves are not in strictly ascending path order"}; }

} // namespace

uint256 SmtKeyPath(SmtComponent component, std::span<const unsigned char> key)
{
    HashWriter writer{TaggedWriter(HashTag::SMT_KEY)};
    writer << static_cast<uint8_t>(component);
    writer.write(std::as_bytes(key));
    return writer.GetSHA256();
}

bool SmtBit(const uint256& path, size_t depth) { return ((path.data()[depth / 8] >> (7 - depth % 8)) & 1) != 0; }

size_t SmtCommonPrefix(const uint256& a, const uint256& b)
{
    for (size_t byte{0}; byte < 32; ++byte) {
        const unsigned diff{static_cast<unsigned>(a.data()[byte] ^ b.data()[byte])};
        if (diff != 0) return byte * 8 + static_cast<size_t>(std::countl_zero(static_cast<uint8_t>(diff)));
    }
    return SMT_DEPTH;
}

uint256 SmtLeafHash(const uint256& path, const uint256& value)
{
    HashWriter writer{TaggedWriter(HashTag::SMT_LEAF)};
    writer << path << value;
    return writer.GetSHA256();
}

uint256 SmtNodeHash(const uint256& left, const uint256& right)
{
    HashWriter writer{TaggedWriter(HashTag::SMT_NODE)};
    writer << left << right;
    return writer.GetSHA256();
}

bool SmtPathLess(const uint256& a, const uint256& b) { return std::memcmp(a.data(), b.data(), 32) < 0; }

Result<uint256> SmtRoot(std::span<const SmtLeaf> leaves)
{
    if (!StrictlyAscending(leaves)) return Unordered();
    return Ok(SubtreeHash(leaves, 0));
}

uint256 SmtProof::SiblingAt(size_t depth_i) const
{
    if (!BitmapBit(bitmap, depth_i)) return SMT_EMPTY;
    size_t index{0};
    for (size_t i{0}; i < depth_i; ++i) index += BitmapBit(bitmap, i) ? 1 : 0;
    return siblings[index];
}

std::vector<unsigned char> SmtProof::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter writer{out, 0};
    writer << depth;
    writer.write(std::as_bytes(std::span{bitmap}));
    for (const uint256& sibling : siblings) writer << sibling;
    writer << static_cast<uint8_t>(terminal);
    if (terminal == SmtTerminal::LEAF) writer << leaf.path << leaf.value;
    return out;
}

Result<SmtProof> SmtProof::Decode(std::span<const unsigned char> bytes)
{
    return DecodeExactWith(bytes, MAX_ENCODED_BYTES, [](SpanReader& reader) {
        SmtProof proof;
        reader >> proof.depth;
        if (proof.depth > SMT_DEPTH) throw std::ios_base::failure("SmtProof depth above 256");
        proof.bitmap.resize((proof.depth + 7) / 8);
        reader.read(MakeWritableByteSpan(proof.bitmap));
        size_t present{0};
        for (size_t i{0}; i < proof.bitmap.size() * 8; ++i) {
            if (!BitmapBit(proof.bitmap, i)) continue;
            if (i >= proof.depth) throw std::ios_base::failure("SmtProof bitmap bit at or above depth");
            ++present;
        }
        if (proof.depth > 0 && !BitmapBit(proof.bitmap, proof.depth - 1)) {
            throw std::ios_base::failure("SmtProof has no sibling at depth - 1");
        }
        proof.siblings.resize(present);
        for (uint256& sibling : proof.siblings) {
            reader >> sibling;
            if (sibling == SMT_EMPTY) throw std::ios_base::failure("SmtProof lists an empty sibling");
        }
        uint8_t terminal{0};
        reader >> terminal;
        if (terminal == static_cast<uint8_t>(SmtTerminal::LEAF)) {
            proof.terminal = SmtTerminal::LEAF;
            reader >> proof.leaf.path >> proof.leaf.value;
        } else if (terminal == static_cast<uint8_t>(SmtTerminal::EMPTY)) {
            proof.terminal = SmtTerminal::EMPTY;
        } else {
            throw std::ios_base::failure("SmtProof terminal unknown");
        }
        return proof;
    });
}

Result<SmtProof> SmtProve(std::span<const SmtLeaf> leaves, const uint256& path)
{
    if (!StrictlyAscending(leaves)) return Unordered();
    SmtProof proof;
    std::vector<uint256> all;
    std::span<const SmtLeaf> here{leaves};
    size_t depth{0};
    while (here.size() > 1) {
        const size_t split{SplitAt(here, depth)};
        const std::span<const SmtLeaf> left{here.first(split)};
        const std::span<const SmtLeaf> right{here.subspan(split)};
        const bool go_right{SmtBit(path, depth)};
        all.push_back(SubtreeHash(go_right ? left : right, depth + 1));
        here = go_right ? right : left;
        ++depth;
    }
    proof.depth = static_cast<uint16_t>(depth);
    proof.bitmap.assign((depth + 7) / 8, 0);
    for (size_t i{0}; i < depth; ++i) {
        if (all[i] == SMT_EMPTY) continue;
        proof.bitmap[i / 8] |= static_cast<unsigned char>(0x80 >> (i % 8));
        proof.siblings.push_back(all[i]);
    }
    if (here.empty()) {
        proof.terminal = SmtTerminal::EMPTY;
    } else {
        proof.terminal = SmtTerminal::LEAF;
        proof.leaf = here.front();
    }
    return Ok(std::move(proof));
}

std::optional<uint256> SmtFoldRoot(const uint256& path, const SmtProof& proof)
{
    if (proof.depth > SMT_DEPTH || proof.bitmap.size() != (proof.depth + 7) / 8u) return std::nullopt;
    size_t present{0};
    for (size_t i{0}; i < proof.bitmap.size() * 8; ++i) {
        if (!BitmapBit(proof.bitmap, i)) continue;
        if (i >= proof.depth) return std::nullopt;
        ++present;
    }
    if (present != proof.siblings.size()) return std::nullopt;
    uint256 hash;
    if (proof.terminal == SmtTerminal::LEAF) {
        if (SmtCommonPrefix(proof.leaf.path, path) < proof.depth) return std::nullopt;
        hash = SmtLeafHash(proof.leaf.path, proof.leaf.value);
    } else if (proof.terminal == SmtTerminal::EMPTY) {
        hash = SMT_EMPTY;
    } else {
        return std::nullopt;
    }
    size_t next{proof.siblings.size()};
    for (size_t i{proof.depth}; i-- > 0;) {
        const uint256 sibling{BitmapBit(proof.bitmap, i) ? proof.siblings[--next] : SMT_EMPTY};
        hash = SmtBit(path, i) ? SmtNodeHash(sibling, hash) : SmtNodeHash(hash, sibling);
    }
    return hash;
}

bool SmtVerifyMember(const uint256& root, const uint256& path, const uint256& value, const SmtProof& proof)
{
    if (proof.terminal != SmtTerminal::LEAF || proof.leaf.path != path || proof.leaf.value != value) return false;
    const std::optional<uint256> folded{SmtFoldRoot(path, proof)};
    return folded.has_value() && *folded == root;
}

bool SmtVerifyAbsent(const uint256& root, const uint256& path, const SmtProof& proof)
{
    if (proof.terminal == SmtTerminal::LEAF && proof.leaf.path == path) return false;
    const std::optional<uint256> folded{SmtFoldRoot(path, proof)};
    return folded.has_value() && *folded == root;
}

} // namespace flowmesh2
