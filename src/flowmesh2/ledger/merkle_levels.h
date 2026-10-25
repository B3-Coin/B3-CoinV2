// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_LEDGER_MERKLE_LEVELS_H
#define B3_FLOWMESH2_LEDGER_MERKLE_LEVELS_H

#include <flowmesh2/core/merkle.h>
#include <flowmesh2/core/result.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

/** The levels of one commitment v0 component tree, kept between slots
 * (M2-LED-10).
 *
 * The tree is core/merkle.h's: level 0 holds MerkleLeafNode of each value,
 * each next level pairs the one below left to right and promotes an unpaired
 * last node, and Root() is MerkleBindRoot(n, top), or MerkleRoot({}) when
 * empty. So Root() == MerkleRoot(values) always, and Prove(i) ==
 * MerkleProve(values, i).
 *
 * Levels live in fixed chunks of CHUNK nodes that copies share. Apply never
 * writes a chunk it did not make itself, so a copy costs one pointer per
 * chunk and this object never changes after it is made; any number of
 * threads may read it. Cost of Apply: for a changed value, one leaf hash and
 * one hash per level; for an inserted value at position p, every node right
 * of p is rewritten, about n - p hashes in all, because v0's sorted leaves
 * shift (the sparse Merkle trees of M2-LED-02 remove that term).
 *
 * Deterministic: no clock, I/O or randomness.
 */
namespace flowmesh2::ledger {

class MerkleLevels
{
public:
    static constexpr size_t CHUNK{128};

    //! One change of the value sequence. `position` is in the sequence after
    //! the change: `value` replaces the value there (insert false) or is put
    //! in front of the value that was there (insert true).
    struct Edit {
        uint64_t position{0};
        uint256 value;
        bool insert{false};
    };

    //! The empty tree.
    MerkleLevels() = default;

    //! The levels of `values` (one Apply that inserts every value).
    static MerkleLevels Build(std::span<const uint256> values);

    //! The number of values.
    uint64_t Size() const { return m_size; }
    //! MerkleRoot of the values.
    uint256 Root() const;
    //! MerkleProve(values, index); INVALID_INPUT for index >= Size().
    Result<MerkleProof> Prove(uint64_t index) const;

    /** The levels of the sequence after `edits`, which are ascending and
     * distinct by position, each position below the new size (Size() plus
     * the inserts). INVALID_INPUT otherwise. This object is unchanged.
     */
    Result<MerkleLevels> Apply(std::span<const Edit> edits) const;

private:
    using Chunk = std::array<uint256, CHUNK>;
    struct Level {
        std::vector<std::shared_ptr<Chunk>> chunks;
        uint64_t size{0};

        const uint256& At(uint64_t index) const { return (*chunks[index / CHUNK])[index % CHUNK]; }
    };

    //! levels[0] holds the leaf nodes; the last level holds one node.
    std::vector<Level> m_levels;
    uint64_t m_size{0};
};

} // namespace flowmesh2::ledger

#endif // B3_FLOWMESH2_LEDGER_MERKLE_LEVELS_H
