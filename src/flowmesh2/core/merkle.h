// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_MERKLE_H
#define B3_FLOWMESH2_CORE_MERKLE_H

#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/result.h>
#include <serialize.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/** Merkle trees of the FlowMesh V2 clean core (slice R3).
 *
 * Shape: RFC 6962 (RFC 9162 section 2.1) with tagged hashes instead of the
 * 0x00/0x01 prefixes. For n leaf values D[0..n):
 *   MTH({})   = Tagged(MERKLE_EMPTY, "")
 *   MTH({d})  = Tagged(MERKLE_LEAF, d)
 *   MTH(D[n]) = Tagged(MERKLE_NODE, MTH(D[0:k]) || MTH(D[k:n])), k the largest
 *               power of two below n.
 * Nothing is ever duplicated, so the Bitcoin flaw CVE-2012-2459 (two leaf
 * lists with one root, consensus/merkle.cpp) cannot occur, and leaf and inner
 * nodes are domain-separated.
 *
 * Committed root = Tagged(MERKLE_ROOT, u64 LE n || MTH(D[n])). Like an RFC
 * 6962 signed tree head, it binds the leaf count, so a proof cannot claim
 * another tree size for the same root (an RFC 9162 inclusion check alone
 * accepts some wrong sizes, e.g. index 0 of 4 leaves verified as a tree of 3).
 *
 * The leaf values are 32-byte hashes that callers make with their own tag
 * (for example ACTION_LEAF), so a leaf value is never parsed as a node.
 * Deterministic: no I/O, clock or allocation beyond the vectors returned.
 */
namespace flowmesh2 {

//! A proof never needs more than 64 path entries (n < 2^64).
inline constexpr size_t MAX_MERKLE_PATH{64};

//! Committed root of `leaves` (see file comment).
uint256 MerkleRoot(std::span<const uint256> leaves);

/** Inclusion proof of leaf `index` in a tree of `size` leaves. `path` lists
 * the sibling subtree hashes from the leaf level up (RFC 9162 order).
 * Serialization: u64 LE index, u64 LE size, CompactSize-capped path.
 */
struct MerkleProof {
    uint64_t index{0};
    uint64_t size{0};
    std::vector<uint256> path;

    friend bool operator==(const MerkleProof&, const MerkleProof&) = default;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << index << size;
        WriteCappedVector(s, path, MAX_MERKLE_PATH);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> index >> size;
        ReadCappedVector(s, path, MAX_MERKLE_PATH);
    }
};

//! Proof for `leaves[index]`; INVALID_INPUT if index >= leaves.size().
Result<MerkleProof> MerkleProve(std::span<const uint256> leaves, uint64_t index);

/** True iff `proof` shows `leaf` at proof.index in the tree whose committed
 * root is `root` and whose size is proof.size (RFC 9162 section 2.1.3.2, then
 * the MERKLE_ROOT binding). False for index >= size or more than
 * MAX_MERKLE_PATH entries.
 */
bool MerkleVerify(const uint256& leaf, const MerkleProof& proof, const uint256& root);

/** A Merkle tree that keeps every level (slice R11), so that one O(n) build
 * pays for O(log n) proofs. It is the tree of MerkleRoot and MerkleProve
 * (the same leaf, node, root and empty hashes and the same promotion of an
 * unpaired last node): Root() == MerkleRoot(leaves) and Prove(i) ==
 * MerkleProve(leaves, i) for every i (merkle_tests checks both for sizes
 * 0..70). The read views (read/views.h) keep one per committed tree.
 * Immutable after Build; memory about 2n hashes. Deterministic, no I/O.
 */
class MerkleTree
{
public:
    static MerkleTree Build(std::span<const uint256> leaves);

    //! The number of leaves.
    uint64_t Size() const { return m_size; }
    //! The committed root, MerkleRoot of the leaves.
    const uint256& Root() const { return m_root; }
    //! MerkleProve(leaves, index) from the stored levels; INVALID_INPUT for index >= Size().
    Result<MerkleProof> Prove(uint64_t index) const;

private:
    MerkleTree(std::vector<std::vector<uint256>> levels, uint64_t size, const uint256& root);

    //! levels[0] holds the leaf nodes; each next level pairs the previous
    //! one (an unpaired last node is promoted); the last level has one node.
    std::vector<std::vector<uint256>> m_levels;
    uint64_t m_size{0};
    uint256 m_root;
};

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_MERKLE_H
