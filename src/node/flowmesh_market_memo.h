// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_NODE_FLOWMESH_MARKET_MEMO_H
#define BITCOIN_NODE_FLOWMESH_MARKET_MEMO_H

#include <flowmesh/market.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace node {

/** Everything the settlement-requirement answer depends on: the durable head
 * (whose entry fixes the previous anchor), the observed anchor it is planned
 * through, and the local delivery generation, which moves whenever B3
 * reconciliation starts or ends. */
struct FlowMeshSettlementRequirementKey {
    flowmesh::MarketId market_id;
    uint256 domain;
    uint64_t next_sequence{0};
    uint256 last_microblock_hash;
    int32_t anchor_height{0};
    uint256 anchor_hash;
    uint64_t delivery_generation{0};
    bool operator==(const FlowMeshSettlementRequirementKey&) const = default;
};

/** The identity of one durable head entry and the seat set its certificate
 * was verified against. The log is append-only, so the entry named by one
 * exact head never changes; a rollback moves the head and so the key. */
struct FlowMeshHeadEntryKey {
    flowmesh::MarketId market_id;
    uint256 domain;
    uint64_t next_sequence{0};
    uint256 last_microblock_hash;
    uint256 seat_set_hash;
    bool operator==(const FlowMeshHeadEntryKey&) const = default;
};

/** One retained answer per market for one exact observation key.
 *
 * Local scheduling state only, never a protocol or durable fact. A lookup
 * hits only for a key identical in every field; a different key for the same
 * market recomputes and replaces that market's answer, and markets never
 * evict each other. The memo therefore holds at most one answer per market
 * it has been given. A null result is never retained, and a computed answer
 * is retained only when the caller's `still_current` check holds after the
 * computation, so an observation that moved while the answer was derived is
 * recomputed on the next call. The mutex is a leaf: `compute` and
 * `still_current` run without it. */
template <typename Key, typename Value>
class FlowMeshMarketMemo
{
public:
    template <typename Compute, typename StillCurrent>
    std::optional<Value> Get(const Key& key, Compute&& compute,
                             StillCurrent&& still_current)
    {
        {
            std::lock_guard<std::mutex> lock{m_mutex};
            const auto it{m_answers.find(key.market_id)};
            if (it != m_answers.end() && it->second.first == key) {
                return it->second.second;
            }
        }
        std::optional<Value> computed{compute()};
        if (computed && still_current()) {
            std::lock_guard<std::mutex> lock{m_mutex};
            m_answers.insert_or_assign(key.market_id,
                                       std::pair<Key, Value>{key, *computed});
        }
        return computed;
    }

    size_t Size() const
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        return m_answers.size();
    }

private:
    mutable std::mutex m_mutex;
    //! Guarded by m_mutex.
    std::map<flowmesh::MarketId, std::pair<Key, Value>> m_answers;
};

} // namespace node

#endif // BITCOIN_NODE_FLOWMESH_MARKET_MEMO_H
