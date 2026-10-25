// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/merkle.h>

#include <flowmesh2/core/hashes.h>
#include <hash.h>

#include <utility>

namespace flowmesh2 {
namespace {

uint256 LeafNode(const uint256& value)
{
    HashWriter writer{TaggedWriter(HashTag::MERKLE_LEAF)};
    writer << value;
    return writer.GetSHA256();
}

uint256 InnerNode(const uint256& left, const uint256& right)
{
    HashWriter writer{TaggedWriter(HashTag::MERKLE_NODE)};
    writer << left << right;
    return writer.GetSHA256();
}

uint256 BindRoot(uint64_t size, const uint256& tree_hash)
{
    HashWriter writer{TaggedWriter(HashTag::MERKLE_ROOT)};
    writer << size << tree_hash;
    return writer.GetSHA256();
}

std::vector<uint256> LeafLevel(std::span<const uint256> leaves)
{
    std::vector<uint256> level;
    level.reserve(leaves.size());
    for (const auto& leaf : leaves) level.push_back(LeafNode(leaf));
    return level;
}

//! Replace `level` by the next level up. Pairs are hashed left to right; an
//! unpaired last node is promoted unchanged. Repeating this until one node is
//! left gives exactly the RFC 6962 split at the largest power of two.
void NextLevel(std::vector<uint256>& level)
{
    size_t out{0};
    size_t i{0};
    for (; i + 1 < level.size(); i += 2) level[out++] = InnerNode(level[i], level[i + 1]);
    if (i < level.size()) level[out++] = level[i];
    level.resize(out);
}

} // namespace

uint256 MerkleLeafNode(const uint256& value) { return LeafNode(value); }
uint256 MerkleInnerNode(const uint256& left, const uint256& right) { return InnerNode(left, right); }
uint256 MerkleBindRoot(uint64_t size, const uint256& tree_hash) { return BindRoot(size, tree_hash); }

uint256 MerkleRoot(std::span<const uint256> leaves)
{
    if (leaves.empty()) return BindRoot(0, Tagged(HashTag::MERKLE_EMPTY, {}));
    std::vector<uint256> level{LeafLevel(leaves)};
    while (level.size() > 1) NextLevel(level);
    return BindRoot(leaves.size(), level.front());
}

Result<MerkleProof> MerkleProve(std::span<const uint256> leaves, uint64_t index)
{
    if (index >= leaves.size()) return Error{ErrorCode::INVALID_INPUT, "Merkle leaf index outside the tree"};
    MerkleProof proof;
    proof.index = index;
    proof.size = leaves.size();
    std::vector<uint256> level{LeafLevel(leaves)};
    uint64_t position{index};
    while (level.size() > 1) {
        const uint64_t sibling{position ^ 1};
        // No sibling: the node is the unpaired last one and is promoted.
        if (sibling < level.size()) proof.path.push_back(level[sibling]);
        NextLevel(level);
        position >>= 1;
    }
    return Ok(std::move(proof));
}

bool MerkleVerify(const uint256& leaf, const MerkleProof& proof, const uint256& root)
{
    if (proof.index >= proof.size || proof.path.size() > MAX_MERKLE_PATH) return false;
    // RFC 9162 section 2.1.3.2.
    uint64_t fn{proof.index};
    uint64_t sn{proof.size - 1};
    uint256 r{LeafNode(leaf)};
    for (const auto& p : proof.path) {
        if (sn == 0) return false;
        if ((fn & 1) != 0 || fn == sn) {
            r = InnerNode(p, r);
            while ((fn & 1) == 0 && fn != 0) {
                fn >>= 1;
                sn >>= 1;
            }
        } else {
            r = InnerNode(r, p);
        }
        fn >>= 1;
        sn >>= 1;
    }
    return sn == 0 && BindRoot(proof.size, r) == root;
}

MerkleTree::MerkleTree(std::vector<std::vector<uint256>> levels, uint64_t size, const uint256& root)
    : m_levels{std::move(levels)}, m_size{size}, m_root{root}
{
}

MerkleTree MerkleTree::Build(std::span<const uint256> leaves)
{
    if (leaves.empty()) return MerkleTree{{}, 0, MerkleRoot(leaves)};
    std::vector<std::vector<uint256>> levels;
    levels.push_back(LeafLevel(leaves));
    while (levels.back().size() > 1) {
        std::vector<uint256> next{levels.back()};
        NextLevel(next);
        levels.push_back(std::move(next));
    }
    const uint256 root{BindRoot(leaves.size(), levels.back().front())};
    return MerkleTree{std::move(levels), leaves.size(), root};
}

Result<MerkleProof> MerkleTree::Prove(uint64_t index) const
{
    if (index >= m_size) return Error{ErrorCode::INVALID_INPUT, "Merkle leaf index outside the tree"};
    MerkleProof proof;
    proof.index = index;
    proof.size = m_size;
    uint64_t position{index};
    // Every level below the single top node gives at most one sibling; an
    // unpaired last node has none (it was promoted), as in MerkleProve.
    for (size_t level{0}; level + 1 < m_levels.size(); ++level) {
        const uint64_t sibling{position ^ 1};
        if (sibling < m_levels[level].size()) proof.path.push_back(m_levels[level][sibling]);
        position >>= 1;
    }
    return Ok(std::move(proof));
}

} // namespace flowmesh2
