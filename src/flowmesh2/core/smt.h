// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_SMT_H
#define B3_FLOWMESH2_CORE_SMT_H

#include <flowmesh2/core/result.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/** Compact sparse Merkle trees of commitment v1 (M2-LED-02; plan section
 * 3.7; PROVISIONAL REGTEST, ARCHITECTURE.md O-96).
 *
 * Every key of a component is mapped to a 256-bit path, and the tree is the
 * binary trie of the paths, read from the first bit (the most significant
 * bit of byte 0) down. A subtree is hashed by what it holds:
 *   no leaf          SMT_EMPTY, the 32 zero bytes
 *   exactly one leaf SmtLeafHash(path, value) = Tagged(SMT_LEAF, path || value),
 *                    at whatever depth the subtree sits
 *   two or more      SmtNodeHash(left, right) = Tagged(SMT_NODE, left || right)
 * so a leaf sits at the shortest prefix no other path shares, the root is a
 * function of the leaf set alone, and a tree of n hashed paths is about
 * log2(n) deep. Leaf and node hashes are domain-separated, and a leaf hash
 * binds the whole path, so a proof cannot move a leaf to another depth.
 *
 * Paths: SmtKeyPath(component, key) = Tagged(SMT_KEY, u8 component || key),
 * so nobody chooses a path's bits and a component's keys never collide with
 * another's.
 *
 * Proofs (SmtProof) are typed. They name the depth the walk along a path
 * stopped at, the non-empty siblings on the way (a bitmap marks which of the
 * `depth` siblings are present) and the terminal: EMPTY, or the one LEAF of
 * the subtree there. A membership proof ends at the path's own leaf; an
 * absence proof ends at EMPTY or at a leaf of another path with the same
 * first `depth` bits. A proof over n hashed paths is about
 * 3 + 32 log2(n) + 65 bytes: about 768 bytes at 2^21 leaves.
 *
 * Deterministic: no I/O, clock or allocation beyond the vectors returned.
 */
namespace flowmesh2 {

//! Path length in bits: the deepest a leaf can sit.
inline constexpr size_t SMT_DEPTH{256};

//! The hash of an empty subtree.
inline const uint256 SMT_EMPTY{};

//! The components of commitment v1 (the byte in SmtKeyPath). APPEND-ONLY; zero is never a value.
enum class SmtComponent : uint8_t {
    ACCOUNTS = 1,
    ORDERS = 2,
    MARKETS = 3,
    NULLIFIERS = 4,
    TOTALS = 5,
    WITHDRAWALS = 6,
};

//! Tagged(SMT_KEY, u8 component || key).
uint256 SmtKeyPath(SmtComponent component, std::span<const unsigned char> key);
//! Bit `depth` of `path`, from the most significant bit of byte 0. depth < SMT_DEPTH.
bool SmtBit(const uint256& path, size_t depth);
//! The number of leading bits `a` and `b` share (SMT_DEPTH if equal).
size_t SmtCommonPrefix(const uint256& a, const uint256& b);
//! Tagged(SMT_LEAF, path || value).
uint256 SmtLeafHash(const uint256& path, const uint256& value);
//! Tagged(SMT_NODE, left || right).
uint256 SmtNodeHash(const uint256& left, const uint256& right);

struct SmtLeaf {
    uint256 path;
    uint256 value;
    friend bool operator==(const SmtLeaf&, const SmtLeaf&) = default;
};

//! True iff `a` sorts before `b` in tree order (bytewise, which is bit order).
bool SmtPathLess(const uint256& a, const uint256& b);

/** The root of `leaves`, which are in strictly ascending path order
 * (SmtPathLess); INVALID_INPUT otherwise. The reference construction:
 * O(n log n), for vectors and tests; the ledger keeps its trees.
 */
Result<uint256> SmtRoot(std::span<const SmtLeaf> leaves);

//! What a proof's walk ended at. APPEND-ONLY; zero is never a value.
enum class SmtTerminal : uint8_t {
    EMPTY = 1, //!< an empty subtree: the path is absent
    LEAF = 2,  //!< the subtree's one leaf: the path's own (membership) or another's (absence)
};

/** A typed proof along one path (see the file comment).
 *
 * Encoding, at most MAX_ENCODED_BYTES: u16 LE depth (0..256) || bitmap of
 * ceil(depth / 8) bytes, bit i (most significant first) set iff the sibling
 * at depth i is non-empty || the non-empty siblings, 32 bytes each, root
 * first || u8 terminal || for LEAF, the leaf's path and value. Decode is
 * exact and refuses anything else with DECODE: a depth above 256, a bitmap
 * bit at or above depth, a present sibling equal to SMT_EMPTY, a missing
 * sibling at depth - 1 (the canonical tree has none: a single leaf or an
 * empty side would have collapsed into its parent), an unknown terminal and
 * trailing bytes.
 */
struct SmtProof {
    uint16_t depth{0};
    std::vector<unsigned char> bitmap;  //!< ceil(depth / 8) bytes
    std::vector<uint256> siblings;      //!< the non-empty siblings, root first
    SmtTerminal terminal{SmtTerminal::EMPTY};
    SmtLeaf leaf;                       //!< for LEAF only; zero for EMPTY

    static constexpr size_t MAX_ENCODED_BYTES{2 + SMT_DEPTH / 8 + SMT_DEPTH * 32 + 1 + 64};
    friend bool operator==(const SmtProof&, const SmtProof&) = default;

    //! The sibling at `depth_i` < depth: a listed sibling or SMT_EMPTY.
    uint256 SiblingAt(size_t depth_i) const;
    std::vector<unsigned char> Encode() const;
    static Result<SmtProof> Decode(std::span<const unsigned char> bytes);
};

/** The proof along `path` in the tree of `leaves` (strictly ascending path
 * order, INVALID_INPUT otherwise): membership if `path` is a leaf, else
 * absence. The reference construction, for vectors and tests.
 */
Result<SmtProof> SmtProve(std::span<const SmtLeaf> leaves, const uint256& path);

/** The root a proof gives: the terminal's hash folded up the path's bits
 * with the siblings. Nullopt for a proof that cannot be canonical: a LEAF
 * terminal whose path does not share the first `depth` bits with `path`, or
 * a depth above 256, a bitmap that is not ceil(depth / 8) bytes or does not
 * match the sibling count.
 */
std::optional<uint256> SmtFoldRoot(const uint256& path, const SmtProof& proof);

//! True iff `proof` shows `path` holding `value` under `root`.
bool SmtVerifyMember(const uint256& root, const uint256& path, const uint256& value, const SmtProof& proof);
//! True iff `proof` shows that no leaf of `root` has `path`.
bool SmtVerifyAbsent(const uint256& root, const uint256& path, const SmtProof& proof);

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_SMT_H
