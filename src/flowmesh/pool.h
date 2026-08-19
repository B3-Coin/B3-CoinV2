// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_POOL_H
#define B3COIN_FLOWMESH_POOL_H

#include <flowmesh/batch.h>
#include <flowmesh/state.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <map>
#include <vector>

namespace flowmesh {

/**
 * Pending-action pool: the FlowMesh analog of a mempool, and like a
 * mempool it is LOCAL POLICY, not consensus — nothing about certified
 * state depends on what any node's pool holds. It is deliberately
 * simple and deterministic anyway: canonical storage order, explicit
 * bounds, admit-or-refuse (no fancy eviction).
 *
 * Admission requires canonical shape and bounds only; authentication
 * happens at execution (and a proposer may pre-filter). Batch selection
 * is deterministic: unconsumed deposits in outpoint order, then per
 * signer the contiguous sequence run starting at the state's next
 * sequence — actions a microblock could actually apply.
 */
class ActionPool
{
public:
    explicit ActionPool(const size_t max_actions = 65536, const size_t max_bytes = 16 << 20)
        : m_max_actions{max_actions}, m_max_bytes{max_bytes}
    {
    }

    size_t Size() const { return m_by_id.size(); }
    size_t Bytes() const { return m_bytes; }

    //! Admit one action. Refuses malformed shapes, duplicates,
    //! per-(signer, sequence) conflicts, and anything past the bounds.
    //! ATOMIC: every admission precondition — including secondary-index
    //! availability — is checked BEFORE any container or byte-count
    //! mutation, so Add can never report success (or fail) while leaving
    //! an unreachable, byte-counted entry behind.
    bool Add(const Action& action)
    {
        if (!action.ShapeIsCanonical()) return false;
        const uint256 id{action.Id()};
        if (m_by_id.count(id) > 0) return false;
        const size_t sz{static_cast<size_t>(::GetSerializeSize(action))};
        if (m_by_id.size() + 1 > m_max_actions || m_bytes + sz > m_max_bytes) return false;
        const bool is_deposit{static_cast<ActionType>(action.type) == ActionType::DEPOSIT};
        if (is_deposit) {
            if (m_deposits.count(action.outpoint) > 0) return false;
        } else {
            // First-seen wins per (signer, sequence); an equivocating
            // second intent is refused here (pool policy) and judged at
            // consensus level if it arrives in a microblock anyway.
            if (m_signed.count({action.signer, action.sequence}) > 0) return false;
        }

        m_by_id.emplace(id, action);
        m_bytes += sz;
        if (is_deposit) {
            m_deposits.emplace(action.outpoint, id);
        } else {
            m_signed.emplace(std::make_pair(action.signer, action.sequence), id);
        }
        return true;
    }

    /**
     * Deterministic proposer selection against `state`: every pool
     * deposit whose outpoint is not yet consumed (outpoint order), then
     * for each signer (account order) the contiguous run of sequences
     * starting at the state's next sequence. At most `max_actions`.
     */
    std::vector<Action> SelectBatch(const FlowMeshState& state, const size_t max_actions) const
    {
        std::vector<Action> out;
        for (const auto& [outpoint, id] : m_deposits) {
            if (out.size() >= max_actions) return out;
            if (state.consumed_deposits.count(outpoint) > 0) continue;
            out.push_back(m_by_id.at(id));
        }
        const AccountId* current_signer{nullptr};
        uint64_t expected{0};
        for (const auto& [key, id] : m_signed) {
            if (out.size() >= max_actions) return out;
            const auto& [signer, sequence]{key};
            if (current_signer == nullptr || !(*current_signer == signer)) {
                current_signer = &key.first;
                expected = state.NextSequence(signer);
            }
            if (sequence < expected) continue;  // stale; pruned on commit
            if (sequence > expected) continue;  // gap: not yet applicable
            out.push_back(m_by_id.at(id));
            ++expected;
        }
        return out;
    }

    /**
     * Drop everything a committed state makes moot: consumed deposits,
     * and every signed action whose sequence is now below its signer's
     * next sequence (applied, state-rejected, or superseded).
     */
    void PruneCommitted(const FlowMeshState& state)
    {
        for (auto it{m_deposits.begin()}; it != m_deposits.end();) {
            if (state.consumed_deposits.count(it->first) > 0) {
                Drop(it->second);
                it = m_deposits.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it{m_signed.begin()}; it != m_signed.end();) {
            const auto& [signer, sequence]{it->first};
            if (sequence < state.NextSequence(signer)) {
                Drop(it->second);
                it = m_signed.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    void Drop(const uint256& id)
    {
        const auto it{m_by_id.find(id)};
        if (it == m_by_id.end()) return;
        m_bytes -= static_cast<size_t>(::GetSerializeSize(it->second));
        m_by_id.erase(it);
    }

    const size_t m_max_actions;
    const size_t m_max_bytes;
    size_t m_bytes{0};
    std::map<uint256, Action> m_by_id;
    std::map<COutPoint, uint256> m_deposits;
    std::map<std::pair<AccountId, uint64_t>, uint256> m_signed;
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_POOL_H
