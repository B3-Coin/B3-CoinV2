// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_RECENT_WINDOW_H
#define B3COIN_NODE_FLOWMESH_RECENT_WINDOW_H

#include <flowmesh/batch.h>
#include <flowmesh/production_wire.h>
#include <node/flowmesh_production_store.h>
#include <uint256.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace node {

/** Per-market bounds of the recent certified window. Memory only. */
struct FlowMeshRecentWindowLimits {
    //! Certified envelopes (payloads), newest first kept.
    size_t payload_entries{64};
    size_t payload_bytes{16U << 20};
    //! Store re-execution records (outcome evidence), newest first kept.
    size_t outcome_entries{1024};
    size_t outcome_bytes{8U << 20};
};
//! Across all markets of one runtime. The newest payload of each market is
//! its certified head, which the runtime holds anyway: it is shared, never
//! copied, and never counted here.
inline constexpr size_t FLOWMESH_RECENT_PAYLOAD_GLOBAL_BYTES{64U << 20};
inline constexpr size_t FLOWMESH_RECENT_OUTCOME_GLOBAL_BYTES{32U << 20};

/** Which certified action consumed one account sequence, from this node's
 * own re-execution record of a retained entry. */
struct FlowMeshRecentConsumer {
    uint256 action_id;
    uint64_t microblock_sequence{0};
    uint256 microblock_hash;
};

/**
 * Bounded memory window of this runtime's most recent certified entries of
 * one market, filled only from its own durable commits (and the restart
 * replay of them). Worker-owned under the market mutex; readers copy shared
 * pointers under that mutex and encode outside it. Never a source of state,
 * never read from the store, never an input to admission of remote frames,
 * proposals, agreement or signing.
 */
class FlowMeshRecentCertifiedWindow
{
public:
    struct Item {
        uint64_t sequence{0};
        uint64_t epoch{0};
        uint256 hash;
        //! Payload part: present for the newest payload_entries items.
        std::shared_ptr<const flowmesh::ProductionCertifiedEnvelope> envelope;
        size_t seat_count{0};
        size_t payload_bytes{0};
        //! (ActionId, body index), sorted; present iff envelope.
        std::vector<std::pair<uint256, uint32_t>> ids;
        //! Outcome part: the store's own re-execution record, if retained.
        std::shared_ptr<const ProductionOutcomeRecord> outcome;
    };

    //! O(actions) conservative memory estimate of one retained envelope.
    static size_t EstimatePayloadBytes(const flowmesh::ProductionCertifiedEnvelope& certified)
    {
        size_t bytes{512 + certified.certificate.signer_bitmap.size()};
        for (const auto& action : certified.entry.actions) {
            bytes += sizeof(flowmesh::Action) + 16 * action.curve.size() + action.credential.size();
        }
        return bytes;
    }

    //! Sorted (ActionId, body index) lookup table for one entry body.
    static std::vector<std::pair<uint256, uint32_t>> IndexIds(std::vector<std::pair<uint256, uint32_t>> ids)
    {
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    /**
     * Append the newest item. A sequence gap clears the window first (fail
     * closed: lookups never span a missing entry). Then own items are evicted
     * oldest first until every bound holds: the payload bounds never evict
     * the newest item's envelope (the head), the outcome bounds may.
     * `*_others` are the other markets' counted bytes of this runtime.
     */
    void Push(Item item, const FlowMeshRecentWindowLimits& limits, const size_t payload_others,
              const size_t outcome_others)
    {
        if (!m_items.empty() && m_items.back().sequence + 1 != item.sequence) Clear();
        if (!item.envelope) item.ids.clear();
        if (item.envelope) {
            ++m_payload_count;
            m_payload_bytes += item.payload_bytes;
        }
        if (item.outcome) {
            ++m_outcome_count;
            m_outcome_bytes += item.outcome->charged_bytes;
        }
        m_items.push_back(std::move(item));
        for (auto& old : m_items) {
            if (&old == &m_items.back()) break;
            if (!old.envelope) continue;
            if (m_payload_count <= limits.payload_entries && PayloadExtraBytes() <= limits.payload_bytes &&
                payload_others + PayloadExtraBytes() <= FLOWMESH_RECENT_PAYLOAD_GLOBAL_BYTES) break;
            DropPayload(old);
        }
        for (auto& old : m_items) {
            if (!old.outcome) continue;
            if (m_outcome_count <= limits.outcome_entries && m_outcome_bytes <= limits.outcome_bytes &&
                outcome_others + m_outcome_bytes <= FLOWMESH_RECENT_OUTCOME_GLOBAL_BYTES) break;
            --m_outcome_count;
            m_outcome_bytes -= old.outcome->charged_bytes;
            old.outcome.reset();
        }
        while (!m_items.empty() && !m_items.front().envelope && !m_items.front().outcome) m_items.pop_front();
    }

    void Clear()
    {
        m_items.clear();
        m_payload_count = m_payload_bytes = m_outcome_count = m_outcome_bytes = 0;
    }

    //! Counted payload bytes: every retained envelope but the newest.
    size_t PayloadExtraBytes() const
    {
        if (m_items.empty() || !m_items.back().envelope) return m_payload_bytes;
        return m_payload_bytes - m_items.back().payload_bytes;
    }
    size_t OutcomeBytes() const { return m_outcome_bytes; }
    size_t PayloadCount() const { return m_payload_count; }
    size_t OutcomeCount() const { return m_outcome_count; }
    size_t Size() const { return m_items.size(); }
    const std::deque<Item>& Items() const { return m_items; }

    const Item* Find(const uint64_t sequence) const
    {
        if (m_items.empty() || sequence < m_items.front().sequence || sequence > m_items.back().sequence) return nullptr;
        const Item& item{m_items[sequence - m_items.front().sequence]};
        return item.sequence == sequence ? &item : nullptr;
    }

    /**
     * The retained certified entry that includes `action_id`. Of several, the
     * one whose record shows a decisive disposition (applied, or a signed
     * action rejected by state) wins, since a later inclusion of the same
     * action can only be a non-executing one; otherwise the newest.
     */
    std::optional<std::pair<uint64_t, uint256>> Inclusion(const uint256& action_id) const
    {
        std::optional<std::pair<uint64_t, uint256>> newest;
        for (auto it{m_items.rbegin()}; it != m_items.rend(); ++it) {
            const auto index{BodyIndex(*it, action_id)};
            if (!index) continue;
            if (!newest) newest.emplace(it->sequence, it->hash);
            if (Decisive(*it, action_id, it->envelope->entry.actions[*index].IsDeposit())) {
                return std::pair{it->sequence, it->hash};
            }
        }
        return newest;
    }

    /**
     * The certified action that consumed (signer, sequence) inside a retained
     * payload whose re-execution record is retained and shows the consumer
     * applied or rejected by state. Without such a record: no attribution.
     */
    std::optional<FlowMeshRecentConsumer> FindConsumer(const flowmesh::AccountId& signer, const uint64_t sequence) const
    {
        for (auto it{m_items.rbegin()}; it != m_items.rend(); ++it) {
            if (!it->envelope || !it->outcome || it->outcome->too_large) continue;
            const auto& actions{it->envelope->entry.actions};
            const auto first{std::lower_bound(actions.begin(), actions.end(), std::pair{signer, sequence},
                [](const flowmesh::Action& action, const std::pair<flowmesh::AccountId, uint64_t>& key) {
                    if (action.IsDeposit()) return true;
                    return action.signer < key.first || (action.signer == key.first && action.sequence < key.second);
                })};
            for (auto candidate{first}; candidate != actions.end() && !candidate->IsDeposit() &&
                                        candidate->signer == signer && candidate->sequence == sequence; ++candidate) {
                const uint256 id{candidate->Id()};
                if (Decisive(*it, id, false)) return FlowMeshRecentConsumer{id, it->sequence, it->hash};
            }
        }
        return std::nullopt;
    }

    static std::optional<uint32_t> BodyIndex(const Item& item, const uint256& action_id)
    {
        if (!item.envelope) return std::nullopt;
        const auto found{std::lower_bound(item.ids.begin(), item.ids.end(), std::pair{action_id, uint32_t{0}})};
        if (found == item.ids.end() || found->first != action_id ||
            found->second >= item.envelope->entry.actions.size()) {
            return std::nullopt;
        }
        return found->second;
    }

private:
    //! Applied, or (for a signed action) rejected by state: each consumes the
    //! action's sequence or outpoint, which happens at most once.
    static bool Decisive(const Item& item, const uint256& id, const bool deposit)
    {
        if (!item.outcome || item.outcome->too_large) return false;
        const auto& p{item.outcome->preimage};
        if (std::find(p.applied.begin(), p.applied.end(), id) != p.applied.end()) return true;
        return !deposit && std::binary_search(p.rejected.begin(), p.rejected.end(),
                                              std::pair{id, flowmesh::ActionReject::REJECTED_BY_STATE});
    }

    void DropPayload(Item& item)
    {
        --m_payload_count;
        m_payload_bytes -= item.payload_bytes;
        item.envelope.reset();
        item.ids.clear();
        item.ids.shrink_to_fit();
        item.payload_bytes = 0;
    }

    std::deque<Item> m_items;
    size_t m_payload_count{0};
    size_t m_payload_bytes{0};
    size_t m_outcome_count{0};
    size_t m_outcome_bytes{0};
};

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_RECENT_WINDOW_H
