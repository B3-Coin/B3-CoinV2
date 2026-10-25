// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_LEDGER_PERSISTENT_TREE_H
#define B3_FLOWMESH2_LEDGER_PERSISTENT_TREE_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

/** Ordered containers whose copies share structure (M2-LED-10).
 *
 * PersistentMap<K, V> and PersistentSet<K> are AVL trees of reference-counted
 * nodes. Copying one copies a pointer, so the engine's candidate copy of the
 * parent state costs O(1) instead of O(state). The first change of a key
 * after a copy clones the O(log n) nodes on its path; later changes of that
 * path are made in place. Diff walks two trees and skips every subtree they
 * share, so the keys that changed between a parent and its candidate cost
 * O(changed x log n) to find, however the candidate was produced.
 *
 * Ownership. Every node records the token of the tree that created it, and a
 * tree writes only nodes that carry its own token. A copy gives both trees
 * fresh tokens (the source's token is a mutable atomic, so several threads
 * may copy one const source at once), so neither ever writes a node the other
 * can see. Any number of threads may read shared nodes; a tree itself is a
 * plain value written by one thread at a time, as std::map is.
 *
 * Differences from std::map: no erase (the ledger removes no account and no
 * nullifier); iteration is ascending and forward only; a pointer to a value
 * stays valid until the tree is assigned or destroyed, but one obtained for
 * writing must not be written after the tree is copied (the copy shares the
 * node from then on, and the next write through the tree clones it).
 * Deterministic: no clock, I/O or randomness, and no token reaches an output.
 */
namespace flowmesh2::ledger {

namespace detail {

//! Tree tokens, unique within the process; 0 is never one.
inline std::atomic<uint64_t> g_next_tree_token{1};
inline uint64_t NewTreeToken() { return g_next_tree_token.fetch_add(1, std::memory_order_relaxed); }

template <class K, class V>
struct MapPolicy {
    using value_type = std::pair<const K, V>;
    static const K& KeyOf(const value_type& value) { return value.first; }
};

template <class K>
struct SetPolicy {
    using value_type = K;
    static const K& KeyOf(const value_type& value) { return value; }
};

} // namespace detail

template <class K, class Policy>
class PersistentTree
{
public:
    using key_type = K;
    using value_type = typename Policy::value_type;
    using size_type = size_t;
    using difference_type = std::ptrdiff_t;
    //! An AVL tree of fewer than 2^64 nodes is less than 93 high.
    static constexpr size_t MAX_HEIGHT{96};

private:
    struct Node {
        template <class... Args>
        explicit Node(uint64_t token_in, Args&&... args) : value(std::forward<Args>(args)...), token{token_in}
        {
        }
        Node(const Node& other, uint64_t token_in)
            : value(other.value), left{other.left}, right{other.right}, size{other.size}, height{other.height}, token{token_in}
        {
        }

        value_type value;
        std::shared_ptr<Node> left;
        std::shared_ptr<Node> right;
        size_t size{1};
        uint8_t height{1};
        uint64_t token;
    };
    using NodePtr = std::shared_ptr<Node>;

public:
    class const_iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = PersistentTree::value_type;
        using difference_type = std::ptrdiff_t;
        using pointer = const value_type*;
        using reference = const value_type&;

        const_iterator() = default;
        reference operator*() const { return m_path[m_depth - 1]->value; }
        pointer operator->() const { return &m_path[m_depth - 1]->value; }
        const_iterator& operator++()
        {
            const Node* done{m_path[--m_depth]};
            for (const Node* next{done->right.get()}; next != nullptr; next = next->left.get()) m_path[m_depth++] = next;
            return *this;
        }
        const_iterator operator++(int)
        {
            const_iterator was{*this};
            ++*this;
            return was;
        }
        friend bool operator==(const const_iterator& a, const const_iterator& b) { return a.Top() == b.Top(); }

    private:
        friend class PersistentTree;
        const Node* Top() const { return m_depth == 0 ? nullptr : m_path[m_depth - 1]; }
        void Push(const Node* node) { m_path[m_depth++] = node; }

        //! The current node on top, below it every ancestor whose left subtree holds it.
        std::array<const Node*, MAX_HEIGHT> m_path{};
        size_t m_depth{0};
    };
    using iterator = const_iterator;

    PersistentTree() : m_token{detail::NewTreeToken()} {}
    PersistentTree(const PersistentTree& other) : m_root{other.m_root}, m_token{detail::NewTreeToken()} { other.Share(); }
    PersistentTree(PersistentTree&& other) noexcept
        : m_root{std::move(other.m_root)}, m_token{other.m_token.load(std::memory_order_relaxed)}
    {
        other.Share();
    }
    PersistentTree& operator=(const PersistentTree& other)
    {
        if (this != &other) {
            m_root = other.m_root;
            Share();
            other.Share();
        }
        return *this;
    }
    PersistentTree& operator=(PersistentTree&& other) noexcept
    {
        if (this != &other) {
            m_root = std::move(other.m_root);
            m_token.store(other.m_token.load(std::memory_order_relaxed), std::memory_order_relaxed);
            other.Share();
        }
        return *this;
    }
    ~PersistentTree() = default;

    // ---- Reads ----------------------------------------------------------------

    size_t size() const { return Size(m_root.get()); }
    bool empty() const { return !m_root; }

    const_iterator begin() const
    {
        const_iterator it;
        for (const Node* node{m_root.get()}; node != nullptr; node = node->left.get()) it.Push(node);
        return it;
    }
    const_iterator end() const { return const_iterator{}; }

    //! The first value whose key is not below `key`.
    const_iterator lower_bound(const K& key) const
    {
        const_iterator it;
        for (const Node* node{m_root.get()}; node != nullptr;) {
            if (KeyOf(node) < key) {
                node = node->right.get();
            } else {
                it.Push(node);
                node = node->left.get();
            }
        }
        return it;
    }
    const_iterator find(const K& key) const
    {
        const const_iterator it{lower_bound(key)};
        return it != end() && !(key < KeyOf(it.Top())) ? it : end();
    }
    bool contains(const K& key) const { return FindNode(key) != nullptr; }
    size_t count(const K& key) const { return contains(key) ? 1 : 0; }

    //! The value of `key`, or nullptr.
    const value_type* FindValue(const K& key) const
    {
        const Node* node{FindNode(key)};
        return node == nullptr ? nullptr : &node->value;
    }

    //! The number of keys below `key`: the position `key` has or would have.
    size_t Rank(const K& key) const
    {
        size_t rank{0};
        for (const Node* node{m_root.get()}; node != nullptr;) {
            if (KeyOf(node) < key) {
                rank += Size(node->left.get()) + 1;
                node = node->right.get();
            } else {
                node = node->left.get();
            }
        }
        return rank;
    }

    //! The height of the tree (0 when empty).
    size_t Height() const { return Height(m_root.get()); }

    /** True iff the tree is a valid AVL tree: keys strictly ascending, every
     * node's size and height right, and no balance outside [-1, 1]. O(n), for
     * tests.
     */
    bool CheckShape() const
    {
        const K* last{nullptr};
        return CheckShape(m_root.get(), last);
    }

    /** Call visit(before_value, after_value) in ascending key order for every
     * key whose value differs between the trees; before_value is nullptr for a
     * key only `after` has and after_value nullptr for one only `before` has.
     * Shared subtrees are skipped. The visitor returns false to stop early.
     */
    template <class Visit>
    static void Diff(const PersistentTree& before, const PersistentTree& after, Visit&& visit)
    {
        struct Item {
            const Node* node;
            bool whole; //!< the subtree rooted at node, or node alone
        };
        std::vector<Item> a;
        std::vector<Item> b;
        a.reserve(2 * MAX_HEIGHT);
        b.reserve(2 * MAX_HEIGHT);
        if (before.m_root) a.push_back({before.m_root.get(), true});
        if (after.m_root) b.push_back({after.m_root.get(), true});
        // Replace a subtree on top by its right subtree, its root alone and its left subtree.
        const auto expand{[](std::vector<Item>& items) {
            const Node* node{items.back().node};
            items.pop_back();
            if (node->right) items.push_back({node->right.get(), true});
            items.push_back({node, false});
            if (node->left) items.push_back({node->left.get(), true});
        }};
        while (!a.empty() || !b.empty()) {
            if (!a.empty() && !b.empty()) {
                const Item& x{a.back()};
                const Item& y{b.back()};
                if (x.whole && y.whole && x.node == y.node) {
                    a.pop_back();
                    b.pop_back();
                } else if (x.whole && (!y.whole || x.node->height >= y.node->height)) {
                    expand(a);
                } else if (y.whole) {
                    expand(b);
                } else if (KeyOf(x.node) < KeyOf(y.node)) {
                    if (!visit(&x.node->value, static_cast<const value_type*>(nullptr))) return;
                    a.pop_back();
                } else if (KeyOf(y.node) < KeyOf(x.node)) {
                    if (!visit(static_cast<const value_type*>(nullptr), &y.node->value)) return;
                    b.pop_back();
                } else {
                    if (!(x.node->value == y.node->value) && !visit(&x.node->value, &y.node->value)) return;
                    a.pop_back();
                    b.pop_back();
                }
            } else if (!a.empty()) {
                if (a.back().whole) {
                    expand(a);
                } else {
                    if (!visit(&a.back().node->value, static_cast<const value_type*>(nullptr))) return;
                    a.pop_back();
                }
            } else if (b.back().whole) {
                expand(b);
            } else {
                if (!visit(static_cast<const value_type*>(nullptr), &b.back().node->value)) return;
                b.pop_back();
            }
        }
    }

    //! Same keys and values, ascending; O(1) for trees that share their root.
    friend bool operator==(const PersistentTree& a, const PersistentTree& b)
    {
        if (a.m_root == b.m_root) return true;
        if (a.size() != b.size()) return false;
        bool same{true};
        Diff(a, b, [&same](const value_type*, const value_type*) {
            same = false;
            return false;
        });
        return same;
    }

protected:
    //! The value of `key` for writing, or nullptr; clones the shared nodes on its path.
    value_type* MutableValue(const K& key)
    {
        if (!contains(key)) return nullptr;
        const uint64_t token{Token()};
        NodePtr* slot{&m_root};
        while (true) {
            Node& node{Own(*slot, token)};
            if (key < KeyOf(&node)) {
                slot = &node.left;
            } else if (KeyOf(&node) < key) {
                slot = &node.right;
            } else {
                return &node.value;
            }
        }
    }

    //! Insert value_type(args...) under `key`, which must be absent; the new value.
    template <class... Args>
    value_type* InsertAbsent(const K& key, Args&&... args)
    {
        value_type* made{nullptr};
        m_root = InsertAt(std::move(m_root), key, Token(), made, std::forward<Args>(args)...);
        return made;
    }

private:
    static const K& KeyOf(const Node* node) { return Policy::KeyOf(node->value); }
    static size_t Size(const Node* node) { return node == nullptr ? 0 : node->size; }
    static size_t Height(const Node* node) { return node == nullptr ? 0 : node->height; }

    uint64_t Token() const { return m_token.load(std::memory_order_relaxed); }
    //! A new token: no node made before this call can be written by this tree any more.
    void Share() const { m_token.store(detail::NewTreeToken(), std::memory_order_relaxed); }

    const Node* FindNode(const K& key) const
    {
        for (const Node* node{m_root.get()}; node != nullptr;) {
            if (key < KeyOf(node)) {
                node = node->left.get();
            } else if (KeyOf(node) < key) {
                node = node->right.get();
            } else {
                return node;
            }
        }
        return nullptr;
    }

    //! `slot`'s node, cloned first unless this tree made it since its last copy.
    static Node& Own(NodePtr& slot, uint64_t token)
    {
        if (slot->token != token) slot = std::make_shared<Node>(*slot, token);
        return *slot;
    }

    static void Update(Node& node)
    {
        node.size = 1 + Size(node.left.get()) + Size(node.right.get());
        node.height = static_cast<uint8_t>(1 + std::max(Height(node.left.get()), Height(node.right.get())));
    }

    // Rotations take and return owned nodes and own every node they relink.
    static NodePtr RotateRight(NodePtr node, uint64_t token)
    {
        NodePtr left{std::move(node->left)};
        Own(left, token);
        node->left = std::move(left->right);
        Update(*node);
        left->right = std::move(node);
        Update(*left);
        return left;
    }
    static NodePtr RotateLeft(NodePtr node, uint64_t token)
    {
        NodePtr right{std::move(node->right)};
        Own(right, token);
        node->right = std::move(right->left);
        Update(*node);
        right->left = std::move(node);
        Update(*right);
        return right;
    }
    static NodePtr Rebalance(NodePtr node, uint64_t token)
    {
        Update(*node);
        const size_t left{Height(node->left.get())};
        const size_t right{Height(node->right.get())};
        if (left > right + 1) {
            Own(node->left, token);
            if (Height(node->left->left.get()) < Height(node->left->right.get())) {
                node->left = RotateLeft(std::move(node->left), token);
            }
            return RotateRight(std::move(node), token);
        }
        if (right > left + 1) {
            Own(node->right, token);
            if (Height(node->right->right.get()) < Height(node->right->left.get())) {
                node->right = RotateRight(std::move(node->right), token);
            }
            return RotateLeft(std::move(node), token);
        }
        return node;
    }

    template <class... Args>
    static NodePtr InsertAt(NodePtr node, const K& key, uint64_t token, value_type*& made, Args&&... args)
    {
        if (!node) {
            NodePtr leaf{std::make_shared<Node>(token, std::forward<Args>(args)...)};
            made = &leaf->value;
            return leaf;
        }
        Own(node, token);
        if (key < KeyOf(node.get())) {
            node->left = InsertAt(std::move(node->left), key, token, made, std::forward<Args>(args)...);
        } else {
            node->right = InsertAt(std::move(node->right), key, token, made, std::forward<Args>(args)...);
        }
        return Rebalance(std::move(node), token);
    }

    static bool CheckShape(const Node* node, const K*& last)
    {
        if (node == nullptr) return true;
        if (!CheckShape(node->left.get(), last)) return false;
        if (last != nullptr && !(*last < KeyOf(node))) return false;
        last = &KeyOf(node);
        if (!CheckShape(node->right.get(), last)) return false;
        const size_t left{Height(node->left.get())};
        const size_t right{Height(node->right.get())};
        return node->size == 1 + Size(node->left.get()) + Size(node->right.get()) &&
               node->height == 1 + std::max(left, right) && left <= right + 1 && right <= left + 1;
    }

    NodePtr m_root;
    mutable std::atomic<uint64_t> m_token;
};

//! An ordered map with shared copies (see the file comment).
template <class K, class V>
class PersistentMap : public PersistentTree<K, detail::MapPolicy<K, V>>
{
    using Base = PersistentTree<K, detail::MapPolicy<K, V>>;

public:
    using mapped_type = V;
    using typename Base::value_type;

    //! The value of `key`, or nullptr.
    const V* Find(const K& key) const
    {
        const value_type* found{this->FindValue(key)};
        return found == nullptr ? nullptr : &found->second;
    }
    //! The value of `key` for writing, or nullptr (clones its shared path).
    V* FindMutable(const K& key)
    {
        value_type* found{this->MutableValue(key)};
        return found == nullptr ? nullptr : &found->second;
    }
    const V& at(const K& key) const
    {
        const V* found{Find(key)};
        if (found == nullptr) throw std::out_of_range("PersistentMap::at");
        return *found;
    }
    V& at(const K& key)
    {
        V* found{FindMutable(key)};
        if (found == nullptr) throw std::out_of_range("PersistentMap::at");
        return *found;
    }
    //! The value of `key` for writing and false, or a new V(args...) and true.
    template <class... Args>
    std::pair<V*, bool> TryEmplace(const K& key, Args&&... args)
    {
        if (V* found{FindMutable(key)}) return {found, false};
        value_type* made{this->InsertAbsent(key, std::piecewise_construct, std::forward_as_tuple(key),
                                            std::forward_as_tuple(std::forward<Args>(args)...))};
        return {&made->second, true};
    }
};

//! An ordered set with shared copies (see the file comment).
template <class K>
class PersistentSet : public PersistentTree<K, detail::SetPolicy<K>>
{
public:
    //! Insert `key`; false if it was there already.
    bool Insert(const K& key)
    {
        if (this->contains(key)) return false;
        this->InsertAbsent(key, key);
        return true;
    }
};

} // namespace flowmesh2::ledger

#endif // B3_FLOWMESH2_LEDGER_PERSISTENT_TREE_H
