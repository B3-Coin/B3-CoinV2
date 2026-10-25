// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_LEDGER_SMT_TREE_H
#define B3_FLOWMESH2_LEDGER_SMT_TREE_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/core/smt.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace flowmesh2::ledger {

/** A compact sparse Merkle tree (core/smt.h) kept in memory (M2-LED-02).
 *
 * Immutable shared nodes, each with its subtree's hash and leaf count: a
 * copy is one pointer, and Set or Erase rebuilds only the nodes on the
 * path, about log2(n) hashes, so copies taken before a change keep their
 * root. The shape is always the canonical one of core/smt.h (CheckShape),
 * so Root() equals SmtRoot of the leaves and Prove() equals SmtProve.
 *
 * Memory: a leaf node holds its path, value, hash and count; an inner node
 * its hash, count and two children. About 2n nodes for n leaves.
 * Deterministic; no lock, clock or I/O. A tree may be read from several
 * threads; a write needs the only reference to the object written.
 */
class SmtTree
{
public:
    SmtTree() = default;

    //! The tree of `leaves` (strictly ascending path order; INVALID_INPUT otherwise).
    static Result<SmtTree> Build(std::span<const SmtLeaf> leaves);

    uint256 Root() const;
    size_t Size() const;
    //! The value at `path`, if a leaf has it.
    std::optional<uint256> Find(const uint256& path) const;
    //! Insert or replace the leaf of `path`.
    void Set(const uint256& path, const uint256& value);
    //! Remove the leaf of `path`; false if there was none.
    bool Erase(const uint256& path);
    //! The membership proof of `path`, or its absence proof (equal to SmtProve).
    SmtProof Prove(const uint256& path) const;
    //! True iff every node is canonical and its hash and count are its children's.
    bool CheckShape() const;

    //! The node type, defined in smt_tree.cpp; public only so the helpers there can name it.
    struct Node;
    using NodePtr = std::shared_ptr<const Node>;

private:
    explicit SmtTree(NodePtr root) : m_root{std::move(root)} {}

    NodePtr m_root;
};

} // namespace flowmesh2::ledger

#endif // B3_FLOWMESH2_LEDGER_SMT_TREE_H
