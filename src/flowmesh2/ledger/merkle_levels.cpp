// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/ledger/merkle_levels.h>

#include <utility>

namespace flowmesh2::ledger {

namespace {

//! Node counts per level for `size` leaves: size, ceil(size / 2), ... down to 1.
std::vector<uint64_t> LevelSizes(uint64_t size)
{
    std::vector<uint64_t> sizes;
    if (size == 0) return sizes;
    sizes.push_back(size);
    while (sizes.back() > 1) sizes.push_back(sizes.back() / 2 + sizes.back() % 2);
    return sizes;
}

} // namespace

MerkleLevels MerkleLevels::Build(std::span<const uint256> values)
{
    std::vector<Edit> edits;
    edits.reserve(values.size());
    for (size_t i{0}; i < values.size(); ++i) edits.push_back(Edit{i, values[i], true});
    // Ascending distinct positions below the new size: Apply cannot refuse them.
    Result<MerkleLevels> built{MerkleLevels{}.Apply(edits)};
    return std::move(built).value();
}

uint256 MerkleLevels::Root() const
{
    if (m_size == 0) return MerkleRoot({});
    return MerkleBindRoot(m_size, m_levels.back().At(0));
}

Result<MerkleProof> MerkleLevels::Prove(uint64_t index) const
{
    if (index >= m_size) return Error{ErrorCode::INVALID_INPUT, "Merkle leaf index outside the tree"};
    MerkleProof proof;
    proof.index = index;
    proof.size = m_size;
    uint64_t position{index};
    for (size_t level{0}; level + 1 < m_levels.size(); ++level) {
        const uint64_t sibling{position ^ 1};
        if (sibling < m_levels[level].size) proof.path.push_back(m_levels[level].At(sibling));
        position >>= 1;
    }
    return Ok(std::move(proof));
}

Result<MerkleLevels> MerkleLevels::Apply(std::span<const Edit> edits) const
{
    if (edits.empty()) return Ok(*this);
    uint64_t inserts{0};
    for (const Edit& edit : edits) inserts += edit.insert ? 1 : 0;
    const uint64_t size{m_size + inserts};
    for (size_t i{0}; i < edits.size(); ++i) {
        if (edits[i].position >= size || (i > 0 && edits[i - 1].position >= edits[i].position)) {
            return Error{ErrorCode::INVALID_INPUT, "Merkle edits out of order or outside the tree"};
        }
    }
    const std::vector<uint64_t> sizes{LevelSizes(size)};

    // A level being written: chunks shared with this object until first
    // written, then a fresh copy that only this call can see.
    struct Writer {
        Level level;
        std::vector<bool> fresh;

        Writer(const Level* old, uint64_t size)
        {
            level.size = size;
            const size_t count{static_cast<size_t>((size + CHUNK - 1) / CHUNK)};
            level.chunks.resize(count);
            fresh.assign(count, false);
            if (old != nullptr) {
                for (size_t c{0}; c < count && c < old->chunks.size(); ++c) level.chunks[c] = old->chunks[c];
            }
        }
        void Set(uint64_t index, const uint256& node)
        {
            const size_t c{static_cast<size_t>(index / CHUNK)};
            if (!fresh[c]) {
                level.chunks[c] = level.chunks[c] ? std::make_shared<Chunk>(*level.chunks[c]) : std::make_shared<Chunk>();
                fresh[c] = true;
            }
            (*level.chunks[c])[index % CHUNK] = node;
        }
    };

    // Level 0. Values left of the first insert keep their positions; from the
    // first insert on, every position is rewritten from the edits and the old
    // leaves, which have moved right by the inserts before them.
    uint64_t first_insert{size};
    for (const Edit& edit : edits) {
        if (edit.insert) {
            first_insert = edit.position;
            break;
        }
    }
    std::vector<uint64_t> dirty; // changed positions left of `rewritten`, ascending
    Writer leaves{m_levels.empty() ? nullptr : &m_levels[0], size};
    size_t e{0};
    for (; e < edits.size() && edits[e].position < first_insert; ++e) {
        leaves.Set(edits[e].position, MerkleLeafNode(edits[e].value));
        dirty.push_back(edits[e].position);
    }
    uint64_t shifted{0};
    for (uint64_t i{first_insert}; i < size; ++i) {
        if (e < edits.size() && edits[e].position == i) {
            leaves.Set(i, MerkleLeafNode(edits[e].value));
            shifted += edits[e].insert ? 1 : 0;
            ++e;
        } else {
            leaves.Set(i, m_levels[0].At(i - shifted));
        }
    }

    MerkleLevels out;
    out.m_size = size;
    out.m_levels.reserve(sizes.size());
    out.m_levels.push_back(std::move(leaves.level));

    // Each level up: recompute the parents of the dirty nodes and every node
    // from the rewritten suffix on. A node left of the suffix covers leaves
    // left of the first insert only, a full subtree that the old tree has too.
    uint64_t rewritten{first_insert};
    for (size_t j{1}; j < sizes.size(); ++j) {
        const Level& below{out.m_levels[j - 1]};
        Writer up{j < m_levels.size() ? &m_levels[j] : nullptr, sizes[j]};
        const auto node_at{[&below](uint64_t i) {
            return 2 * i + 1 < below.size ? MerkleInnerNode(below.At(2 * i), below.At(2 * i + 1)) : below.At(2 * i);
        }};
        rewritten /= 2;
        std::vector<uint64_t> next;
        next.reserve(dirty.size());
        for (const uint64_t d : dirty) {
            const uint64_t parent{d / 2};
            if (parent >= rewritten || (!next.empty() && next.back() == parent)) continue;
            up.Set(parent, node_at(parent));
            next.push_back(parent);
        }
        for (uint64_t i{rewritten}; i < sizes[j]; ++i) up.Set(i, node_at(i));
        dirty = std::move(next);
        out.m_levels.push_back(std::move(up.level));
    }
    return Ok(std::move(out));
}

} // namespace flowmesh2::ledger
