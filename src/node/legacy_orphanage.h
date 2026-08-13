// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_NODE_LEGACY_ORPHANAGE_H
#define BITCOIN_NODE_LEGACY_ORPHANAGE_H

#include <net.h> // For NodeId
#include <primitives/block.h>
#include <uint256.h>
#include <util/time.h>

#include <cstddef>
#include <map>
#include <memory>
#include <vector>

namespace node {

/**
 * Bounded holding area for structurally-valid legacy blocks that arrived
 * before their parent. The historical client kept a byte-capped orphan cache
 * for exactly this case; this is its bounded, deterministic counterpart.
 *
 * Every dimension of memory use is capped: total entry count, total bytes,
 * per-peer entry count and per-peer bytes, and a hard expiry. Entries carry
 * no chain weight and take part in no validation decision: they are only
 * candidates for reprocessing when their parent connects. Eviction order is
 * deterministic (expired first, then oldest by insertion), never random.
 */
class LegacyBlockOrphanage
{
public:
    //! Bounds. A legacy block is at most 5 MB on the wire
    //! (legacy::MAX_BLOCK_SIZE), so the per-peer byte cap admits at least one
    //! maximal block and the global byte cap roughly three.
    static constexpr size_t MAX_ORPHAN_COUNT{64};
    static constexpr size_t MAX_ORPHAN_BYTES{16 * 1024 * 1024};
    static constexpr size_t MAX_PEER_ORPHAN_COUNT{16};
    static constexpr size_t MAX_PEER_ORPHAN_BYTES{6 * 1024 * 1024};
    static constexpr auto ORPHAN_EXPIRY{10min};

    struct Entry {
        std::shared_ptr<const CBlock> block;
        NodeId from{-1};
        size_t bytes{0};
        NodeClock::time_point added{};
    };

    /**
     * Add a block that is missing its parent. Returns false without storing
     * when the hash is already present, when the block alone exceeds the
     * per-peer byte cap, or when the providing peer is at one of its caps
     * (a peer cannot evict other peers' entries to make room for its own).
     * Global caps are enforced by evicting expired entries first, then the
     * oldest entries, before insertion.
     */
    bool Add(const uint256& hash, const uint256& parent_hash,
             std::shared_ptr<const CBlock> block, NodeId from, size_t bytes,
             NodeClock::time_point now);

    /** Remove and return all stored children of the given parent. */
    std::vector<Entry> TakeChildrenOf(const uint256& parent_hash);

    /** Drop every entry older than ORPHAN_EXPIRY. */
    void Expire(NodeClock::time_point now);

    bool Contains(const uint256& hash) const { return m_orphans.count(hash) > 0; }
    size_t Count() const { return m_orphans.size(); }
    size_t Bytes() const { return m_total_bytes; }
    size_t PeerCount(NodeId peer) const;
    size_t PeerBytes(NodeId peer) const;

private:
    void EraseByHash(const uint256& hash);
    //! Evict the entry with the earliest insertion time. No-op when empty.
    void EvictOldest();

    //! Keyed by the block's own (marker) hash.
    std::map<uint256, Entry> m_orphans;
    //! parent hash -> child hash, for TakeChildrenOf.
    std::multimap<uint256, uint256> m_by_parent;
    size_t m_total_bytes{0};
};

} // namespace node

#endif // BITCOIN_NODE_LEGACY_ORPHANAGE_H
