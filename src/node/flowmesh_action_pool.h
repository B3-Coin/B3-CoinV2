// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_ACTION_POOL_H
#define B3COIN_NODE_FLOWMESH_ACTION_POOL_H

#include <flowmesh/action_reason.h>
#include <flowmesh/batch.h>
#include <flowmesh/deposit.h>
#include <flowmesh/p2p.h>
#include <flowmesh/production_wire.h>
#include <flowmesh/state.h>
#include <node/flowmesh_runtime.h>
#include <serialize.h>
#include <uint256.h>

#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace node {

//! Pool origin of this node's own API/RPC submissions and restored evidence.
//! Never a real peer id: exempt from per-peer quotas, owner of local retries.
inline constexpr flowmesh::WirePeerId FLOWMESH_LOCAL_ACTION_PEER{
    std::numeric_limits<flowmesh::WirePeerId>::min()};

/** Pool admission context. Only this node's own API/RPC submissions are
 * api_origin; retained candidate evidence restored at start is not. */
struct FlowMeshPoolAddContext {
    bool api_origin{false};
    bool priority_eligible{false};
};

/**
 * Per-market pending-action pool of the production runtime. Worker-owned:
 * the runtime calls it only under its market mutex. Admission authenticates
 * through the installed verifier and enforces the per-market and per-peer
 * count/byte bounds of flowmesh/p2p.h; selection is proposer-local policy.
 */
class FlowMeshActionPool
{
public:
    using Verifier = std::function<bool(const flowmesh::Action&)>;

    using AddContext = FlowMeshPoolAddContext;

    void SetVerifier(Verifier verifier) { m_verifier = std::move(verifier); }

    /** Same checks in the same order as ever; the result names the first one
     * that refused (ADMITTED on success). */
    flowmesh::ActionReasonCode Add(const flowmesh::Action& action,
                                   const flowmesh::WirePeerId origin,
                                   const flowmesh::WireClock::time_point now,
                                   const AddContext context = {})
    {
        using Code = flowmesh::ActionReasonCode;
        if (!action.ShapeIsCanonical()) return Code::MALFORMED_SHAPE;
        if (!m_verifier || !m_verifier(action)) return Code::UNAUTHENTICATED;
        const uint256 id{action.Id()};
        if (m_actions.count(id) != 0) return Code::DUPLICATE_PENDING;
        const size_t bytes{static_cast<size_t>(::GetSerializeSize(action))};
        if (m_actions.size() >= flowmesh::FLOWMESH_ACTION_POOL_PER_MARKET_COUNT ||
            bytes > flowmesh::FLOWMESH_ACTION_POOL_PER_MARKET_BYTES ||
            m_bytes > flowmesh::FLOWMESH_ACTION_POOL_PER_MARKET_BYTES - bytes) {
            return Code::POOL_FULL;
        }
        if (origin != FLOWMESH_LOCAL_ACTION_PEER) {
            const Usage& usage{m_peer_usage[origin]};
            if (usage.count >=
                    flowmesh::FLOWMESH_ACTION_POOL_PER_PEER_MARKET_COUNT ||
                bytes > flowmesh::FLOWMESH_ACTION_POOL_PER_PEER_MARKET_BYTES ||
                usage.bytes >
                    flowmesh::FLOWMESH_ACTION_POOL_PER_PEER_MARKET_BYTES -
                        bytes) {
                return Code::PEER_QUOTA;
            }
        }
        if (action.IsDeposit()) {
            if (m_deposits.count(action.outpoint) != 0) return Code::DEPOSIT_CONFLICT;
        } else if (m_signed.count({action.signer, action.sequence}) != 0) {
            return Code::SEQUENCE_CONFLICT;
        }
        m_actions.emplace(id, StoredAction{action});
        m_actions.at(id).api_origin = context.api_origin;
        m_origins.emplace(id, origin);
        m_bytes += bytes;
        if (origin != FLOWMESH_LOCAL_ACTION_PEER) {
            Usage& usage{m_peer_usage[origin]};
            ++usage.count;
            usage.bytes += bytes;
        }
        if (action.IsDeposit()) {
            m_deposits.emplace(action.outpoint, id);
        } else {
            m_signed.emplace(std::make_pair(action.signer, action.sequence), id);
        }
        if (origin == FLOWMESH_LOCAL_ACTION_PEER) {
            m_local_retries.push_back(id);
            m_actions.at(id).local_retry = LocalRetry{
                now + FlowMeshLocalActionRelayBudget::REPEAT_DELAY,
                std::prev(m_local_retries.end())};
        }
        return Code::ADMITTED;
    }

    // Only local admission creates a retry obligation. The list owns IDs,
    // never another copy of the action, and Drop removes its entry atomically.
    // Unlike received duplicate envelopes, this local obligation survives a
    // certified-head change. The signed action/outpoint is never rewritten.
    std::optional<flowmesh::WireMessage> PrepareLocalRetry(
        const flowmesh::WireHeader& header, const flowmesh::FlowMeshState& state,
        const flowmesh::DepositVerifier* deposits, const flowmesh::AnchorRef& anchor,
        const flowmesh::WireClock::time_point now, size_t& scanned)
    {
        const size_t maximum{m_local_retries.size()};
        for (size_t visited{0}; visited < maximum && !m_local_retries.empty() &&
             scanned < FlowMeshLocalActionRelayBudget::MAX_ACTIONS_SCANNED; ++visited) {
            ++scanned;
            auto& stored{m_actions.at(m_local_retries.front())};
            if (!StillPending(stored.action, state) || !m_verifier ||
                !stored.action.ShapeIsCanonical()) {
                RemoveLocalRetry(stored);
                continue;
            }
            if (now < stored.local_retry->next_attempt) {
                m_local_retries.splice(m_local_retries.end(), m_local_retries,
                                       m_local_retries.begin());
                continue;
            }
            if (!m_verifier(stored.action)) {
                RemoveLocalRetry(stored);
                continue;
            }
            if (stored.action.IsDeposit() &&
                (deposits == nullptr || !deposits->GetDeposit(stored.action.outpoint, anchor))) {
                // Missing anchored facts are not authorization to advertise a
                // deposit, but can become available later. Pace rechecks too.
                CompleteLocalRetry(now);
                continue;
            }
            const auto payload{flowmesh::EncodeProductionActionPayload(stored.action)};
            if (!payload) {
                RemoveLocalRetry(stored);
                continue;
            }
            return flowmesh::WireMessage{flowmesh::WireMessageKind::ACTION,
                                         header, *payload};
        }
        return std::nullopt;
    }

    void CompleteLocalRetry(const flowmesh::WireClock::time_point now)
    {
        auto& stored{m_actions.at(m_local_retries.front())};
        stored.local_retry->next_attempt = now + FlowMeshLocalActionRelayBudget::REPEAT_DELAY;
        m_local_retries.splice(m_local_retries.end(), m_local_retries,
                               m_local_retries.begin());
    }

    std::optional<std::vector<flowmesh::Action>> EvidenceFor(
        const std::span<const flowmesh::Action> semantic_actions) const
    {
        std::vector<flowmesh::Action> out;
        out.reserve(semantic_actions.size());
        for (const flowmesh::Action& semantic : semantic_actions) {
            const auto it{m_actions.find(semantic.Id())};
            if (it == m_actions.end()) return std::nullopt;
            flowmesh::Action stripped{it->second.action};
            stripped.credential.clear();
            const auto semantic_bytes{
                flowmesh::EncodeProductionActionPayload(semantic)};
            const auto stripped_bytes{
                flowmesh::EncodeProductionActionPayload(stripped)};
            if (!semantic_bytes || !stripped_bytes ||
                *semantic_bytes != *stripped_bytes) {
                return std::nullopt;
            }
            out.push_back(it->second.action);
        }
        return out;
    }

    size_t Size() const { return m_actions.size(); }

    bool ContainsExact(const flowmesh::Action& action) const
    {
        const auto it{m_actions.find(action.Id())};
        if (it == m_actions.end()) return false;
        return flowmesh::EncodeProductionActionPayload(action) ==
               flowmesh::EncodeProductionActionPayload(it->second.action);
    }

    std::vector<flowmesh::Action> Select(
        const flowmesh::FlowMeshState& state, const size_t maximum) const
    {
        std::vector<flowmesh::Action> out;
        out.reserve(std::min(maximum, m_actions.size()));
        for (const auto& [outpoint, id] : m_deposits) {
            if (out.size() >= maximum) return out;
            if (!state.DepositConsumed(outpoint)) out.push_back(m_actions.at(id).action);
        }
        const flowmesh::AccountId* signer_before{nullptr};
        uint64_t expected{0};
        for (const auto& [key, id] : m_signed) {
            if (out.size() >= maximum) return out;
            const auto& [signer, sequence]{key};
            if (signer_before == nullptr || *signer_before != signer) {
                signer_before = &key.first;
                expected = state.NextSequence(signer);
            }
            if (sequence != expected) continue;
            out.push_back(m_actions.at(id).action);
            ++expected;
        }
        return out;
    }

    // Admit an obligation only for an incoming exact authenticated duplicate.
    // Action::Id deliberately excludes credentials, so compare full bytes.
    bool QueueDuplicateForward(const flowmesh::Action& action,
                               const flowmesh::WireMessage& message,
                               const flowmesh::WirePeerId peer,
                               const flowmesh::FlowMeshState& state,
                               const flowmesh::WireClock::time_point now)
    {
        const auto found{m_actions.find(action.Id())};
        if (found == m_actions.end() || !action.ShapeIsCanonical() ||
            !m_verifier || !m_verifier(action) || !StillPending(action, state)) return false;
        auto& stored{found->second};
        const auto bytes{flowmesh::EncodeProductionActionPayload(stored.action)};
        if (!bytes || *bytes != message.payload) return false;
        if (stored.forward) return true; // coalesce without changing FIFO order/origin
        if (stored.last_forward_attempt &&
            now < *stored.last_forward_attempt + FlowMeshDuplicateActionRelayBudget::REPEAT_DELAY) return false;
        m_pending_forwards.push_back(found->first);
        stored.forward = PendingForward{message.header, peer,
                                        std::prev(m_pending_forwards.end())};
        return true;
    }

    bool HasPendingForwards() const { return !m_pending_forwards.empty(); }

    // Invalid/stale obligations are removed. A valid front remains in place
    // until its complete frame fits the shared budget, preventing tail loss.
    std::optional<flowmesh::WireMessage> PrepareDuplicateForward(
        const flowmesh::WireHeader& expected,
        const flowmesh::FlowMeshState& state,
        flowmesh::WirePeerId& exclude_peer)
    {
        if (m_pending_forwards.empty()) return std::nullopt;
        auto& stored{m_actions.at(m_pending_forwards.front())};
        if (!(stored.forward->header == expected) ||
            !StillPending(stored.action, state) || !m_verifier ||
            !m_verifier(stored.action)) {
            RemoveForward(stored);
            return std::nullopt;
        }
        const auto payload{flowmesh::EncodeProductionActionPayload(stored.action)};
        if (!payload) {
            RemoveForward(stored);
            return std::nullopt;
        }
        exclude_peer = stored.forward->peer;
        return flowmesh::WireMessage{flowmesh::WireMessageKind::ACTION,
                                     stored.forward->header, *payload};
    }

    void CompleteDuplicateForward(const flowmesh::WireClock::time_point now)
    {
        auto& stored{m_actions.at(m_pending_forwards.front())};
        stored.last_forward_attempt = now;
        RemoveForward(stored);
    }

    void ClearDuplicateForwards()
    {
        while (!m_pending_forwards.empty()) {
            RemoveForward(m_actions.at(m_pending_forwards.front()));
        }
    }

    /** Drops every consumed deposit and every stale sequence, as always.
     * Returns up to `report_max` dropped API-origin SIGNED actions, in drop
     * order; further such drops are only counted (UnreportedDrops). */
    std::vector<flowmesh::Action> Prune(const flowmesh::FlowMeshState& state, const size_t report_max = 0)
    {
        std::vector<flowmesh::Action> reported;
        for (auto it{m_deposits.begin()}; it != m_deposits.end();) {
            if (!state.DepositConsumed(it->first)) {
                ++it;
                continue;
            }
            Drop(it->second);
            it = m_deposits.erase(it);
        }
        for (auto it{m_signed.begin()}; it != m_signed.end();) {
            if (it->first.second >= state.NextSequence(it->first.first)) {
                ++it;
                continue;
            }
            if (const auto found{m_actions.find(it->second)};
                found != m_actions.end() && found->second.api_origin) {
                if (reported.size() < report_max) {
                    reported.push_back(found->second.action);
                } else if (m_unreported_drops != std::numeric_limits<uint64_t>::max()) {
                    ++m_unreported_drops;
                }
            }
            Drop(it->second);
            it = m_signed.erase(it);
        }
        return reported;
    }

    //! API-origin drops beyond a Prune report bound (observation only).
    uint64_t UnreportedDrops() const { return m_unreported_drops; }

    /**
     * A certified execution is a terminal disposition for every action in
     * its body, including a deposit rejected by the anchored chain-state
     * verifier. Remove those exact actions so an invalid deposit cannot be
     * proposed and certified forever while producing empty effects.
     */
    void DropCertified(std::span<const flowmesh::Action> actions)
    {
        for (const flowmesh::Action& action : actions) {
            const uint256 id{action.Id()};
            if (m_actions.count(id) == 0) continue;
            if (action.IsDeposit()) {
                const auto deposit{m_deposits.find(action.outpoint)};
                if (deposit != m_deposits.end() && deposit->second == id) {
                    m_deposits.erase(deposit);
                }
            } else {
                const auto key{std::make_pair(action.signer,
                                              action.sequence)};
                const auto signed_action{m_signed.find(key)};
                if (signed_action != m_signed.end() &&
                    signed_action->second == id) {
                    m_signed.erase(signed_action);
                }
            }
            Drop(id);
        }
    }

    // Shared by retained local retries and local queue items whose transport
    // header aged while a higher-priority certificate advanced this state.
    static bool StillPending(const flowmesh::Action& action,
                             const flowmesh::FlowMeshState& state)
    {
        return action.IsDeposit() ? !state.DepositConsumed(action.outpoint)
                                  : action.sequence >= state.NextSequence(action.signer);
    }

private:
    struct PendingForward {
        flowmesh::WireHeader header;
        flowmesh::WirePeerId peer;
        std::list<uint256>::iterator position;
    };
    struct LocalRetry {
        flowmesh::WireClock::time_point next_attempt;
        std::list<uint256>::iterator position;
    };
    struct StoredAction {
        flowmesh::Action action;
        //! Admitted from this node's own API/RPC (AddContext::api_origin).
        bool api_origin{false};
        std::optional<flowmesh::WireClock::time_point> last_forward_attempt{};
        std::optional<PendingForward> forward{};
        std::optional<LocalRetry> local_retry{};
    };

    void RemoveLocalRetry(StoredAction& stored)
    {
        if (!stored.local_retry) return;
        m_local_retries.erase(stored.local_retry->position);
        stored.local_retry.reset();
    }

    void RemoveForward(StoredAction& stored)
    {
        if (!stored.forward) return;
        m_pending_forwards.erase(stored.forward->position);
        stored.forward.reset();
    }

    void Drop(const uint256& id)
    {
        const auto it{m_actions.find(id)};
        if (it == m_actions.end()) return;
        const size_t bytes{static_cast<size_t>(::GetSerializeSize(it->second.action))};
        m_bytes -= bytes;
        const auto origin{m_origins.find(id)};
        if (origin != m_origins.end()) {
            if (origin->second != FLOWMESH_LOCAL_ACTION_PEER) {
                auto usage{m_peer_usage.find(origin->second)};
                if (usage != m_peer_usage.end()) {
                    --usage->second.count;
                    usage->second.bytes -= bytes;
                    if (usage->second.count == 0) m_peer_usage.erase(usage);
                }
            }
            m_origins.erase(origin);
        }
        RemoveForward(it->second);
        RemoveLocalRetry(it->second);
        m_actions.erase(it);
    }

    struct Usage {
        size_t count{0};
        size_t bytes{0};
    };

    Verifier m_verifier;
    std::map<uint256, StoredAction> m_actions;
    std::list<uint256> m_pending_forwards; // one ID, no payload, per stored action
    std::list<uint256> m_local_retries; // local origin only; one ID per stored action
    std::map<uint256, flowmesh::WirePeerId> m_origins;
    std::map<flowmesh::WirePeerId, Usage> m_peer_usage;
    std::map<COutPoint, uint256> m_deposits;
    std::map<std::pair<flowmesh::AccountId, uint64_t>, uint256> m_signed;
    size_t m_bytes{0};
    uint64_t m_unreported_drops{0};
};

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_ACTION_POOL_H
