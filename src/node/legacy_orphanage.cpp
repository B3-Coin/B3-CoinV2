// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/legacy_orphanage.h>

#include <hash.h>
#include <legacy/codec.h>

#include <algorithm>

namespace node {

namespace {

uint256 WireBodyDigest(const CBlock& block)
{
    // Unknown-parent B3 blocks are decoded and later written through this
    // marker-aware codec. Hash the same complete canonical form so no
    // hash-external field (signature, witness, or MPA) is omitted from the
    // orphan variant identity.
    return (HashWriter{} << legacy::TX_LEGACY(block)).GetHash();
}

} // namespace

bool LegacyBlockOrphanage::Add(const uint256& hash, const uint256& parent_hash,
                               std::shared_ptr<const CBlock> block, NodeId from,
                               size_t bytes, NodeClock::time_point now)
{
    if (bytes > MAX_PEER_ORPHAN_BYTES) return false;

    Expire(now);
    const VariantKey key{hash, WireBodyDigest(*block)};
    if (m_orphans.count(key) > 0) return false;

    // A peer at one of its own caps is refused outright: making room would
    // mean evicting other peers' entries on its behalf.
    if (PeerCount(from) >= MAX_PEER_ORPHAN_COUNT || PeerBytes(from) + bytes > MAX_PEER_ORPHAN_BYTES) {
        return false;
    }

    // Enforce the global caps deterministically: oldest entries go first.
    while (m_orphans.size() >= MAX_ORPHAN_COUNT || m_total_bytes + bytes > MAX_ORPHAN_BYTES) {
        EvictOldest();
    }

    m_by_parent.emplace(parent_hash, key);
    m_total_bytes += bytes;
    m_orphans.emplace(key, Entry{std::move(block), from, bytes, now});
    return true;
}

std::vector<LegacyBlockOrphanage::Entry> LegacyBlockOrphanage::TakeChildrenOf(
    const uint256& parent_hash, NodeClock::time_point now)
{
    Expire(now);

    std::vector<Entry> out;
    auto [begin, end] = m_by_parent.equal_range(parent_hash);
    for (auto it{begin}; it != end; ++it) {
        auto entry_it{m_orphans.find(it->second)};
        if (entry_it != m_orphans.end()) {
            m_total_bytes -= entry_it->second.bytes;
            out.push_back(std::move(entry_it->second));
            m_orphans.erase(entry_it);
        }
    }
    m_by_parent.erase(begin, end);
    return out;
}

void LegacyBlockOrphanage::Expire(NodeClock::time_point now)
{
    for (auto it{m_orphans.begin()}; it != m_orphans.end();) {
        if (now - it->second.added > ORPHAN_EXPIRY) {
            const VariantKey key{it->first};
            ++it;
            EraseByKey(key);
        } else {
            ++it;
        }
    }
}

size_t LegacyBlockOrphanage::PeerCount(NodeId peer) const
{
    return std::count_if(m_orphans.begin(), m_orphans.end(),
                         [peer](const auto& e) { return e.second.from == peer; });
}

size_t LegacyBlockOrphanage::PeerBytes(NodeId peer) const
{
    size_t bytes{0};
    for (const auto& [_, entry] : m_orphans) {
        if (entry.from == peer) bytes += entry.bytes;
    }
    return bytes;
}

bool LegacyBlockOrphanage::Contains(const uint256& hash) const
{
    const auto it{m_orphans.lower_bound(VariantKey{hash, {}})};
    return it != m_orphans.end() && it->first.first == hash;
}

void LegacyBlockOrphanage::EraseByKey(const VariantKey& key)
{
    auto it{m_orphans.find(key)};
    if (it == m_orphans.end()) return;
    m_total_bytes -= it->second.bytes;
    m_orphans.erase(it);
    for (auto pit{m_by_parent.begin()}; pit != m_by_parent.end(); ++pit) {
        if (pit->second == key) {
            m_by_parent.erase(pit);
            break;
        }
    }
}

void LegacyBlockOrphanage::EvictOldest()
{
    if (m_orphans.empty()) return;
    auto oldest{m_orphans.begin()};
    for (auto it{m_orphans.begin()}; it != m_orphans.end(); ++it) {
        if (it->second.added < oldest->second.added) oldest = it;
    }
    EraseByKey(oldest->first);
}

} // namespace node
