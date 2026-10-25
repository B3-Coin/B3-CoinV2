// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/ledger/smt_tree.h>

#include <utility>
#include <vector>

namespace flowmesh2::ledger {

struct SmtTree::Node {
    uint256 hash;
    uint64_t count{0}; //!< leaves in the subtree; 1 for a leaf node, at least 2 for an inner node
    SmtLeaf leaf;      //!< a leaf node's path and value
    NodePtr left;      //!< an inner node's children (either may be null, never both)
    NodePtr right;
};

namespace {

using Node = SmtTree::Node;
using NodePtr = std::shared_ptr<const Node>;

uint256 HashOf(const NodePtr& node) { return node ? node->hash : SMT_EMPTY; }
uint64_t CountOf(const NodePtr& node) { return node ? node->count : 0; }

NodePtr MakeLeaf(const SmtLeaf& leaf)
{
    auto node{std::make_shared<Node>()};
    node->hash = SmtLeafHash(leaf.path, leaf.value);
    node->count = 1;
    node->leaf = leaf;
    return node;
}

//! The node over `left` and `right`, which hold at least two leaves together.
NodePtr MakeInner(NodePtr left, NodePtr right)
{
    auto node{std::make_shared<Node>()};
    node->hash = SmtNodeHash(HashOf(left), HashOf(right));
    node->count = CountOf(left) + CountOf(right);
    node->left = std::move(left);
    node->right = std::move(right);
    return node;
}

//! The canonical subtree at `depth` over the two leaf nodes `a` and `b` of different paths.
NodePtr Join(const NodePtr& a, const NodePtr& b, size_t depth)
{
    const bool a_right{SmtBit(a->leaf.path, depth)};
    if (a_right == SmtBit(b->leaf.path, depth)) {
        NodePtr below{Join(a, b, depth + 1)};
        return a_right ? MakeInner(nullptr, std::move(below)) : MakeInner(std::move(below), nullptr);
    }
    return a_right ? MakeInner(b, a) : MakeInner(a, b);
}

NodePtr SetAt(const NodePtr& node, size_t depth, const SmtLeaf& leaf)
{
    if (!node) return MakeLeaf(leaf);
    if (node->count == 1) {
        if (node->leaf.path == leaf.path) return node->leaf.value == leaf.value ? node : MakeLeaf(leaf);
        return Join(node, MakeLeaf(leaf), depth);
    }
    if (SmtBit(leaf.path, depth)) {
        NodePtr right{SetAt(node->right, depth + 1, leaf)};
        return right == node->right ? node : MakeInner(node->left, std::move(right));
    }
    NodePtr left{SetAt(node->left, depth + 1, leaf)};
    return left == node->left ? node : MakeInner(std::move(left), node->right);
}

NodePtr EraseAt(const NodePtr& node, size_t depth, const uint256& path, bool& erased)
{
    if (!node) return node;
    if (node->count == 1) {
        if (node->leaf.path != path) return node;
        erased = true;
        return nullptr;
    }
    const bool go_right{SmtBit(path, depth)};
    NodePtr child{EraseAt(go_right ? node->right : node->left, depth + 1, path, erased)};
    if (!erased) return node;
    const NodePtr& other{go_right ? node->left : node->right};
    // One leaf left in this subtree: it moves up to the shortest unique prefix.
    if (!child && other && other->count == 1) return other;
    if (child && !other && child->count == 1) return child;
    return go_right ? MakeInner(other, std::move(child)) : MakeInner(std::move(child), other);
}

//! The subtree of `leaves` (strictly ascending, sharing `depth` bits).
NodePtr BuildAt(std::span<const SmtLeaf> leaves, size_t depth)
{
    if (leaves.empty()) return nullptr;
    if (leaves.size() == 1) return MakeLeaf(leaves.front());
    size_t split{0};
    while (split < leaves.size() && !SmtBit(leaves[split].path, depth)) ++split;
    return MakeInner(BuildAt(leaves.first(split), depth + 1), BuildAt(leaves.subspan(split), depth + 1));
}

bool ShapeAt(const NodePtr& node, size_t depth, const uint256& route)
{
    if (!node) return true;
    if (node->count == 1) {
        return !node->left && !node->right && SmtCommonPrefix(node->leaf.path, route) >= depth &&
               node->hash == SmtLeafHash(node->leaf.path, node->leaf.value);
    }
    if (depth >= SMT_DEPTH || node->count != CountOf(node->left) + CountOf(node->right) || node->count < 2) return false;
    // A child with one leaf beside an empty one would have collapsed into this node.
    if (!node->left && node->right->count == 1) return false;
    if (!node->right && node->left->count == 1) return false;
    if (node->hash != SmtNodeHash(HashOf(node->left), HashOf(node->right))) return false;
    uint256 right_route{route};
    right_route.data()[depth / 8] |= static_cast<unsigned char>(0x80 >> (depth % 8));
    uint256 left_route{route};
    left_route.data()[depth / 8] &= static_cast<unsigned char>(~(0x80 >> (depth % 8)));
    return ShapeAt(node->left, depth + 1, left_route) && ShapeAt(node->right, depth + 1, right_route);
}

} // namespace

Result<SmtTree> SmtTree::Build(std::span<const SmtLeaf> leaves)
{
    for (size_t i{1}; i < leaves.size(); ++i) {
        if (!SmtPathLess(leaves[i - 1].path, leaves[i].path)) {
            return Error{ErrorCode::INVALID_INPUT, "SMT leaves are not in strictly ascending path order"};
        }
    }
    return Ok(SmtTree{BuildAt(leaves, 0)});
}

uint256 SmtTree::Root() const { return HashOf(m_root); }
size_t SmtTree::Size() const { return static_cast<size_t>(CountOf(m_root)); }

std::optional<uint256> SmtTree::Find(const uint256& path) const
{
    const Node* node{m_root.get()};
    for (size_t depth{0}; node != nullptr && node->count > 1; ++depth) {
        node = SmtBit(path, depth) ? node->right.get() : node->left.get();
    }
    if (node == nullptr || node->leaf.path != path) return std::nullopt;
    return node->leaf.value;
}

void SmtTree::Set(const uint256& path, const uint256& value) { m_root = SetAt(m_root, 0, SmtLeaf{path, value}); }

bool SmtTree::Erase(const uint256& path)
{
    bool erased{false};
    m_root = EraseAt(m_root, 0, path, erased);
    return erased;
}

SmtProof SmtTree::Prove(const uint256& path) const
{
    SmtProof proof;
    std::vector<uint256> all;
    const Node* node{m_root.get()};
    while (node != nullptr && node->count > 1) {
        const bool go_right{SmtBit(path, all.size())};
        all.push_back(HashOf(go_right ? node->left : node->right));
        node = go_right ? node->right.get() : node->left.get();
    }
    proof.depth = static_cast<uint16_t>(all.size());
    proof.bitmap.assign((all.size() + 7) / 8, 0);
    for (size_t i{0}; i < all.size(); ++i) {
        if (all[i] == SMT_EMPTY) continue;
        proof.bitmap[i / 8] |= static_cast<unsigned char>(0x80 >> (i % 8));
        proof.siblings.push_back(all[i]);
    }
    if (node == nullptr) {
        proof.terminal = SmtTerminal::EMPTY;
    } else {
        proof.terminal = SmtTerminal::LEAF;
        proof.leaf = node->leaf;
    }
    return proof;
}

bool SmtTree::CheckShape() const
{
    if (m_root && m_root->count > 1 && ((!m_root->left && m_root->right->count == 1) || (!m_root->right && m_root->left->count == 1))) {
        return false;
    }
    return ShapeAt(m_root, 0, uint256{});
}

} // namespace flowmesh2::ledger
