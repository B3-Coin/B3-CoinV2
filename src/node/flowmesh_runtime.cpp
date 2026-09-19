// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_runtime.h>

#include <crypto/common.h>
#include <hash.h>
#include <random.h>
#include <serialize.h>
#include <streams.h>
#include <univalue.h>
#include <util/log.h>
#include <util/time.h>

#include <algorithm>
#include <limits>
#include <list>
#include <set>
#include <span>
#include <utility>

namespace node {
namespace {

constexpr flowmesh::WirePeerId LOCAL_ACTION_PEER{
    std::numeric_limits<flowmesh::WirePeerId>::min()};
constexpr size_t MAX_RUNTIME_CANDIDATES_PER_SEQUENCE{8};
constexpr size_t MAX_PENDING_MARKET_ADDITIONS{1'024};
constexpr size_t MAX_CATCHUP_COMMANDS{64};
constexpr size_t MAX_ACTIVE_CATCHUPS{16};
constexpr size_t MAX_CATCHUP_COOLDOWNS{256};
constexpr size_t MAX_CATCHUP_PROFILES{256};
constexpr size_t MAX_DISCOVERY_PEERS{256};
// At the service's 250 ms tick, leave half of the 16/s control budget
// available for catch-up requests/responses rather than starving them.
constexpr size_t MAX_ANNOUNCEMENTS_PER_TICK{2};
constexpr auto MARKET_ANNOUNCEMENT_INTERVAL{std::chrono::seconds{5}};
constexpr auto MARKET_ANNOUNCEMENT_MIN_INTERVAL{std::chrono::seconds{1}};
constexpr auto CATCHUP_TIMEOUT{std::chrono::seconds{5}};
constexpr auto CATCHUP_FAILURE_COOLDOWN{std::chrono::seconds{15}};
// A slow complete frame must still arrive inside the existing five-second
// request window. Reduce count 64->32->16->8->1 after timeouts; retain the
// full byte cap so even one maximum legal certificate is not excluded.
// This cannot recover a single certificate that itself takes >=5s to arrive.
constexpr auto CATCHUP_GROWTH_HEADROOM{
    std::chrono::duration_cast<std::chrono::milliseconds>(CATCHUP_TIMEOUT) / 2};
constexpr size_t MAX_DELIVERY_OBJECTS{8192};
constexpr size_t MAX_DELIVERY_BYTES{64 * 1024 * 1024};
constexpr size_t MAX_MARKET_DELIVERY_BYTES{16 * 1024 * 1024};
constexpr size_t MAX_DELIVERY_EVENTS{4096};
constexpr size_t MAX_DELIVERY_PEERS{64};
constexpr auto DELIVERY_REPEAT{std::chrono::seconds{1}};
constexpr auto AGREEMENT_FORWARD_REPEAT_MAX{std::chrono::seconds{32}};
constexpr auto DELIVERY_COMPLETION_TIMEOUT{std::chrono::seconds{5}};
// Finite memory-only retention during a transient chain/seat transition.
// Exact verified sources remain available for regeneration after reopening.
constexpr auto DELIVERY_PAUSE_MAX_AGE{std::chrono::seconds{60}};
constexpr uint64_t MAX_RUNTIME_TRACE_BYTES{64 * 1024 * 1024};
std::atomic<uint64_t> g_runtime_trace_bytes{0};
std::atomic<uint64_t> g_runtime_trace_dropped{0};

void CountObservation(uint64_t& count)
{
    if (count != std::numeric_limits<uint64_t>::max()) ++count;
}

void CountObservation(std::atomic<uint64_t>& count)
{
    auto value{count.load(std::memory_order_relaxed)};
    while (value != std::numeric_limits<uint64_t>::max() &&
           !count.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) {}
}

std::string CatchupDetails(const uint16_t requested, const uint32_t requested_bytes,
                           const size_t received, const size_t received_bytes,
                           const int64_t elapsed_ms)
{
    return "requested=" + std::to_string(requested) +
           " requested_bytes=" + std::to_string(requested_bytes) +
           " received=" + std::to_string(received) +
           " received_bytes=" + std::to_string(received_bytes) +
           " elapsed_ms=" + std::to_string(elapsed_ms);
}

class RuntimeActionPool
{
public:
    using Verifier = std::function<bool(const flowmesh::Action&)>;

    void SetVerifier(Verifier verifier) { m_verifier = std::move(verifier); }

    bool Add(const flowmesh::Action& action,
             const flowmesh::WirePeerId origin,
             const flowmesh::WireClock::time_point now)
    {
        if (!action.ShapeIsCanonical() || !m_verifier || !m_verifier(action)) {
            return false;
        }
        const uint256 id{action.Id()};
        if (m_actions.count(id) != 0) return false;
        const size_t bytes{static_cast<size_t>(::GetSerializeSize(action))};
        if (m_actions.size() >= flowmesh::FLOWMESH_ACTION_POOL_PER_MARKET_COUNT ||
            bytes > flowmesh::FLOWMESH_ACTION_POOL_PER_MARKET_BYTES ||
            m_bytes > flowmesh::FLOWMESH_ACTION_POOL_PER_MARKET_BYTES - bytes) {
            return false;
        }
        if (origin != LOCAL_ACTION_PEER) {
            const Usage& usage{m_peer_usage[origin]};
            if (usage.count >=
                    flowmesh::FLOWMESH_ACTION_POOL_PER_PEER_MARKET_COUNT ||
                bytes > flowmesh::FLOWMESH_ACTION_POOL_PER_PEER_MARKET_BYTES ||
                usage.bytes >
                    flowmesh::FLOWMESH_ACTION_POOL_PER_PEER_MARKET_BYTES -
                        bytes) {
                return false;
            }
        }
        if (action.IsDeposit()) {
            if (m_deposits.count(action.outpoint) != 0) return false;
        } else if (m_signed.count({action.signer, action.sequence}) != 0) {
            return false;
        }
        m_actions.emplace(id, StoredAction{action});
        m_origins.emplace(id, origin);
        m_bytes += bytes;
        if (origin != LOCAL_ACTION_PEER) {
            Usage& usage{m_peer_usage[origin]};
            ++usage.count;
            usage.bytes += bytes;
        }
        if (action.IsDeposit()) {
            m_deposits.emplace(action.outpoint, id);
        } else {
            m_signed.emplace(std::make_pair(action.signer, action.sequence), id);
        }
        if (origin == LOCAL_ACTION_PEER) {
            m_local_retries.push_back(id);
            m_actions.at(id).local_retry = LocalRetry{
                now + FlowMeshLocalActionRelayBudget::REPEAT_DELAY,
                std::prev(m_local_retries.end())};
        }
        return true;
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

    void Prune(const flowmesh::FlowMeshState& state)
    {
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
            Drop(it->second);
            it = m_signed.erase(it);
        }
    }

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
        std::optional<flowmesh::WireClock::time_point> last_forward_attempt{};
        std::optional<PendingForward> forward{};
        std::optional<LocalRetry> local_retry{};
    };

    static bool StillPending(const flowmesh::Action& action,
                             const flowmesh::FlowMeshState& state)
    {
        return action.IsDeposit() ? !state.DepositConsumed(action.outpoint)
                                  : action.sequence >= state.NextSequence(action.signer);
    }

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
            if (origin->second != LOCAL_ACTION_PEER) {
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
};

bool SameSeatIdentity(const flowmesh::ActiveFnBlsSeatSet& a,
                      const flowmesh::ActiveFnBlsSeatSet& b)
{
    return a.epoch == b.epoch && a.market_id == b.market_id &&
           a.anchor_height == b.anchor_height &&
           a.anchor_hash == b.anchor_hash && a.set_hash == b.set_hash;
}

std::optional<flowmesh::AnchorRef> SeatAnchor(
    const flowmesh::ActiveFnBlsSeatSet& seats)
{
    if (seats.anchor_height >
            static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
        seats.anchor_hash.IsNull()) {
        return std::nullopt;
    }
    return flowmesh::AnchorRef{static_cast<int32_t>(seats.anchor_height),
                               seats.anchor_hash};
}

flowmesh::WireHeader HeaderFor(const flowmesh::ProductionEntryCore& entry)
{
    return flowmesh::WireHeader{flowmesh::FLOWMESH_WIRE_VERSION_V1,
                                entry.market_id, entry.epoch,
                                entry.sequence};
}

} // namespace

struct FlowMeshRuntime::Market {
    struct Candidate {
        struct ProposalProof {
            uint32_t round, seat;
            std::array<unsigned char, bls::SIGNATURE_SIZE> signature;
        };
        flowmesh::ProductionEntryCore entry;
        flowmesh::FlowMeshState next_state;
        std::optional<flowmesh::ActiveFnBlsSeatSet> next_seats;
        std::vector<flowmesh::Action> evidence;
        // Shared across rounds: churn cannot refill an identity's cooldown.
        // These contain no payloads and die with the bounded candidate map.
        std::optional<flowmesh::WireClock::time_point> proposal_forward_attempt{};
        std::map<uint32_t, std::optional<flowmesh::WireClock::time_point>>
            attestation_forward_attempts{};
        // Compact proof only: the entry already lives above. This lets a
        // bounded retry queue regenerate identical bytes without re-signing.
        std::optional<ProposalProof> relay_proof{};
        std::set<uint32_t> local_signers{};
        bool certificate_formed{false};
    };
    struct EvidenceRetry {
        uint256 candidate_hash;
        size_t cursor{0};
        flowmesh::WireClock::time_point next_sweep{};
    };

    uint256 domain;
    flowmesh::MarketId market_id;
    uint256 treasury_owner_commitment;
    flowmesh::ActiveFnBlsSeatSet seats;
    flowmesh::FlowMeshState state;
    uint64_t next_sequence{0};
    uint64_t next_effect_index{0};
    uint256 last_hash;
    uint256 certified_state_root;
    std::optional<flowmesh::ProductionCertifiedEnvelope> client_head;
    size_t client_head_seat_count{0};
    flowmesh::ClientEventLog& client_events;
    std::optional<int64_t> local_observed_at;
    FlowMeshProductionStore* store{nullptr};
    const flowmesh::DepositVerifier* deposits{nullptr};
    FlowMeshRuntimeChain* chain{nullptr};
    FlowMeshRuntimeKeyProvider* keys{nullptr};
    FlowMeshRuntimeClock* clock{nullptr};
    FlowMeshRuntimeRelayFn* relay{nullptr};
    FlowMeshRuntimeDeliverySnapshot delivery;
    size_t delivery_regeneration_position{0};
    std::chrono::milliseconds round_timeout;
    std::unique_ptr<FlowMeshAgreement> agreement;
    std::optional<flowmesh::PreagreementContext> agreement_context;
    flowmesh::WireClock::time_point next_agreement_retry{};
    // Only locally eligible, active work spends this view's timeout. A pause
    // preserves the remaining budget; idle slots start their next work fresh.
    std::optional<flowmesh::WireClock::time_point> agreement_deadline;
    flowmesh::WireClock::duration agreement_timeout_remaining;
    // Independent of legacy relay budgets: a single bounded new-view proof
    // must fit, while each market has a bounded four-MiB scheduling burst.
    FlowMeshCommitteeRelayBudget agreement_budget{
        128, flowmesh::AGREEMENT_MAX_BYTES + flowmesh::FLOWMESH_WIRE_HEADER_SIZE};
    // Exact forwarded payloads of the active slot. A repeat of the same bytes
    // adds nothing at a peer that received them, so its forwarding interval
    // doubles; the first forward and the sender's durable retry are unchanged.
    struct AgreementForward {
        flowmesh::WireClock::time_point next;
        std::chrono::milliseconds delay;
    };
    std::map<uint256, AgreementForward> agreement_forwarded;

    RuntimeActionPool pool;
    bool ready{true};
    bool paused{false};
    uint32_t round{0};
    flowmesh::WireClock::time_point round_started{};
    //! Suppress repeated diagnostics while the same local scheduling gate waits.
    std::string proposal_wait_reason;
    uint64_t proposal_wait_sequence{0};
    uint32_t proposal_wait_round{0};
    bool pending_handoff{false};
    FlowMeshRuntimeHalt halt{FlowMeshRuntimeHalt::NONE};
    std::string error;
    std::optional<flowmesh::AnchorRef> previous_anchor;
    std::map<std::pair<int32_t, uint256>, uint64_t> committed_anchors;
    std::map<uint256, Candidate> candidates;
    // Decoded/authenticated durable evidence is not permission to sign. The
    // service installs markets while its live chain gate is still closed.
    std::optional<StoredLockedProductionCandidate> pending_candidate_restore;
    std::map<uint256, std::map<uint32_t, flowmesh::IndexedBlsSignature>>
        attestations;
    std::map<uint32_t, uint256> attested_hash_by_seat;
    std::optional<flowmesh::WireClock::time_point> last_announcement;
    flowmesh::WireClock::time_point next_announcement{};
    flowmesh::MarketDataRuntimeDiagnostics diagnostics;
    std::optional<EvidenceRetry> evidence_retry;
    bool evidence_retry_eligible{false};
    FlowMeshCommitteeRelayBudget committee_relay_budget{
        FlowMeshCommitteeRelayBudget::MARKET_MESSAGES,
        FlowMeshCommitteeRelayBudget::MARKET_BYTES};
    FlowMeshDuplicateActionRelayBudget duplicate_action_relay_budget;
    FlowMeshLocalActionRelayBudget local_action_relay_budget{
        FlowMeshLocalActionRelayBudget::MARKET_MESSAGES,
        FlowMeshLocalActionRelayBudget::MARKET_BYTES};

    Market(const FlowMeshRuntimeMarketConfig& config,
           FlowMeshRuntimeConfig& runtime_config,
           flowmesh::ClientEventLog& events)
        : domain{config.domain}, market_id{config.market_id},
          treasury_owner_commitment{config.treasury_owner_commitment},
          seats{config.active_seats}, state{config.state},
          next_sequence{config.next_sequence},
          next_effect_index{config.next_effect_index},
          last_hash{config.last_microblock_hash},
          certified_state_root{config.state.Root()}, client_events{events}, store{config.store},
          deposits{config.deposits}, chain{runtime_config.chain},
          keys{runtime_config.keys}, clock{runtime_config.clock},
          relay{&runtime_config.relay},
          round_timeout{runtime_config.round_timeout},
          agreement_timeout_remaining{runtime_config.round_timeout},
          ready{config.readiness == FlowMeshRuntimeMarketReadiness::READY},
          paused{!ready},
          round_started{runtime_config.clock->Now()}
    {
        delivery.market_id = market_id;
    }
};

namespace {

template <typename Market>
void SuspendAgreementTimeout(Market& market)
{
    if (!market.agreement_deadline) return;
    market.agreement_timeout_remaining = std::max(
        flowmesh::WireClock::duration::zero(), *market.agreement_deadline - market.clock->Now());
    market.agreement_deadline.reset();
}

template <typename Market>
void ResetAgreementTimeout(Market& market)
{
    market.agreement_deadline.reset();
    market.agreement_timeout_remaining = market.round_timeout;
}

uint64_t TraceTime(const flowmesh::WireClock::time_point time)
{
    return static_cast<uint64_t>(std::max<int64_t>(0,
        std::chrono::duration_cast<std::chrono::microseconds>(
            time.time_since_epoch()).count()));
}

uint64_t TraceNow(const FlowMeshRuntimeClock& clock)
{
    return TraceTime(clock.Now());
}

struct TraceContext {
    std::optional<uint32_t> round, seat_index;
    std::optional<uint256> related_object_id, bls_key_hash, signature_hash, wire_hash;
    std::optional<uint64_t> delivery_id;
    std::optional<uint64_t> observed_monotonic_us, epoch;
    std::optional<uint256> seat_set_hash;
};

TraceContext EntryTrace(const flowmesh::ProductionEntryCore& entry)
{
    TraceContext trace;
    trace.epoch = entry.epoch;
    trace.seat_set_hash = entry.seat_set_hash;
    return trace;
}

TraceContext WireTrace(const flowmesh::WireMessage& message)
{
    TraceContext trace;
    if (util::log::ShouldLog(BCLog::BENCH, BCLog::Level::Debug)) {
        flowmesh::WireCheck check;
        if (const auto encoded{flowmesh::EncodeWireMessage(message, check)}) trace.wire_hash = Hash(*encoded);
    }
    return trace;
}

template <typename Market, typename Signature>
TraceContext SeatTrace(const Market& market, uint32_t seat, const Signature& signature,
                       std::optional<uint32_t> round = std::nullopt)
{
    TraceContext trace;
    trace.round = round;
    trace.seat_index = seat;
    if (seat < market.seats.Size()) trace.bls_key_hash = Hash(market.seats.members[seat].key.Key().Compressed());
    trace.signature_hash = Hash(signature);
    return trace;
}

template <typename Market>
void DeliveryEvent(Market& market, const char* stage,
                   const flowmesh::WireMessageKind kind, const uint256& object,
                   const uint64_t sequence,
                   const std::optional<flowmesh::WirePeerId> peer = std::nullopt,
                   const std::string& reason = {},
                   const TraceContext& trace = {})
{
    auto& out{market.delivery};
    if (!object.IsNull()) out.last_target_hash = object;
    if (out.events.size() == 128) {
        out.events.erase(out.events.begin());
        CountObservation(out.events_dropped);
    }
    CountObservation(out.last_event_id);
    FlowMeshRuntimeEvent event;
    event.monotonic_us = trace.observed_monotonic_us ? *trace.observed_monotonic_us : TraceNow(*market.clock);
    event.stage = stage;
    event.kind = kind;
    event.sequence = sequence;
    event.object_id = object;
    event.peer = peer;
    event.reason = reason.substr(0, 160);
    event.event_id = out.last_event_id;
    event.epoch = trace.epoch.value_or(market.seats.epoch);
    event.seat_set_hash = trace.seat_set_hash.value_or(market.seats.set_hash);
    event.round = trace.round;
    event.seat_index = trace.seat_index;
    event.related_object_id = trace.related_object_id;
    event.bls_key_hash = trace.bls_key_hash;
    event.signature_hash = trace.signature_hash;
    event.wire_hash = trace.wire_hash;
    event.delivery_id = trace.delivery_id;
    out.events.push_back(event);
    if (util::log::ShouldLog(BCLog::BENCH, BCLog::Level::Debug)) {
        constexpr uint64_t MAX_TRACE_BYTES{16 * 1024 * 1024};
        const auto dropped = [&] {
            CountObservation(out.trace_events_dropped);
            CountObservation(g_runtime_trace_dropped);
        };
        if (out.trace_bytes >= MAX_TRACE_BYTES ||
            g_runtime_trace_bytes.load(std::memory_order_relaxed) >= MAX_RUNTIME_TRACE_BYTES) {
            dropped();
            return;
        }
        UniValue row{UniValue::VOBJ};
        row.pushKV("market_id", market.market_id.GetHex());
        row.pushKV("event_id", event.event_id);
        row.pushKV("monotonic_us", event.monotonic_us);
        row.pushKV("stage", event.stage);
        row.pushKV("kind", std::string{flowmesh::WireCommand(kind)});
        row.pushKV("sequence", sequence);
        row.pushKV("object_id", object.GetHex());
        row.pushKV("epoch", event.epoch);
        row.pushKV("seat_set_hash", event.seat_set_hash.GetHex());
        if (peer) row.pushKV("peer", *peer);
        if (event.round) row.pushKV("round", *event.round);
        if (event.seat_index) row.pushKV("seat_index", *event.seat_index);
        if (event.related_object_id) row.pushKV("related_object_id", event.related_object_id->GetHex());
        if (event.bls_key_hash) row.pushKV("bls_key_hash", event.bls_key_hash->GetHex());
        if (event.signature_hash) row.pushKV("signature_hash", event.signature_hash->GetHex());
        if (event.wire_hash) row.pushKV("wire_hash", event.wire_hash->GetHex());
        if (event.delivery_id) row.pushKV("delivery_id", *event.delivery_id);
        row.pushKV("reason", event.reason);
        const auto line{row.write()};
        // Include a bounded allowance for the logger's prefix and newline.
        const uint64_t bytes{line.size() + 256};
        if (bytes > MAX_TRACE_BYTES - out.trace_bytes) {
            out.trace_bytes = MAX_TRACE_BYTES;
            dropped();
            return;
        }
        auto global_bytes{g_runtime_trace_bytes.load(std::memory_order_relaxed)};
        do {
            if (bytes > MAX_RUNTIME_TRACE_BYTES - global_bytes) {
                dropped();
                return;
            }
        } while (!g_runtime_trace_bytes.compare_exchange_weak(
            global_bytes, global_bytes + bytes, std::memory_order_relaxed));
        out.trace_bytes += bytes;
        LogDebug(BCLog::BENCH, "FlowMeshTrace %s\n", line);
    }
}

template <typename Market>
TraceContext SchedulingContext(const Market& market)
{
    TraceContext trace;
    trace.round = market.round;
    if (market.ready && market.seats.Size() != 0) {
        trace.seat_index = flowmesh::ProductionProposerSeatIndex(
            market.next_sequence, market.round, market.seats.Size());
    }
    return trace;
}

template <typename Market>
std::string RoundTiming(const Market& market)
{
    return " start_us=" + std::to_string(TraceTime(market.round_started)) +
           " deadline_us=" + std::to_string(TraceTime(market.round_started + market.round_timeout));
}

template <typename Market>
void RoundEntered(Market& market, const char* reason)
{
    market.proposal_wait_reason.clear();
    DeliveryEvent(market, "round_entered", flowmesh::WireMessageKind::PROPOSAL,
                  {}, market.next_sequence, std::nullopt,
                  reason + RoundTiming(market), SchedulingContext(market));
}

template <typename Market>
void ProposalWait(Market& market, const char* reason, const bool include_idle = false)
{
    // Ordinary empty markets stay quiet. Required chain/settlement barriers
    // are actionable even without queued user actions; still deduplicate them.
    if ((!include_idle && market.pool.Size() == 0) ||
        (market.proposal_wait_reason == reason &&
         market.proposal_wait_sequence == market.next_sequence &&
         market.proposal_wait_round == market.round)) return;
    market.proposal_wait_reason = reason;
    market.proposal_wait_sequence = market.next_sequence;
    market.proposal_wait_round = market.round;
    DeliveryEvent(market, "proposer_wait", flowmesh::WireMessageKind::PROPOSAL,
                  {}, market.next_sequence, std::nullopt,
                  reason + RoundTiming(market), SchedulingContext(market));
}

template <typename Market>
void TraceActions(Market& market, const char* stage, const flowmesh::ProductionEntryCore& entry)
{
    // Per-action correlation can be 1024 events for one candidate; avoid
    // expanding the normal observation path unless explicitly requested.
    if (!util::log::ShouldLog(BCLog::BENCH, BCLog::Level::Debug)) return;
    TraceContext trace{EntryTrace(entry)};
    trace.related_object_id = entry.GetHash();
    for (const auto& action : entry.actions) {
        DeliveryEvent(market, stage, flowmesh::WireMessageKind::ACTION,
                      action.Id(), entry.sequence, std::nullopt, {}, trace);
    }
}

uint256 DeliveryKey(const flowmesh::MarketId& market,
                    const flowmesh::WireMessageKind kind, const uint256& object,
                    const uint32_t seat, const std::optional<flowmesh::WirePeerId> peer)
{
    HashWriter writer;
    writer << market << static_cast<uint8_t>(kind) << object
           << (kind == flowmesh::WireMessageKind::ATTESTATION ? seat : uint32_t{0})
           << peer.has_value() << peer.value_or(0);
    return writer.GetHash();
}

template <typename Market>
void HaltMarket(Market& market, const FlowMeshRuntimeHalt halt,
                std::string error)
{
    if (market.halt != FlowMeshRuntimeHalt::NONE) return;
    market.halt = halt;
    market.pool.ClearDuplicateForwards();
    market.error = std::move(error);
}

template <typename Market>
flowmesh::ProductionAnchorContext AnchorContext(const Market& market)
{
    return flowmesh::ProductionAnchorContext{
        market.chain->TipHeight(), market.previous_anchor, market.chain};
}

template <typename Market>
std::vector<std::pair<uint32_t, bls::SecretKey>> LocalSeatKeys(
    const Market& market)
{
    std::vector<std::pair<uint32_t, bls::SecretKey>> out;
    std::set<uint32_t> indices;
    for (const bls::SecretKey& key :
         market.keys->LocalSeatKeys(market.market_id, market.seats)) {
        const bls::PublicKey public_key{key.GetPublicKey()};
        const auto member{std::find_if(
            market.seats.members.begin(), market.seats.members.end(),
            [&](const flowmesh::ActiveFnBlsSeat& candidate) {
                return candidate.key.Key() == public_key;
            })};
        if (member == market.seats.members.end()) continue;
        const uint32_t index{static_cast<uint32_t>(std::distance(
            market.seats.members.begin(), member))};
        if (indices.insert(index).second) out.emplace_back(index, key);
    }
    return out;
}

template <typename Market>
bool RecheckAnchors(Market& market)
{
    if (market.halt != FlowMeshRuntimeHalt::NONE || !market.ready) return false;
    const auto seat_anchor{SeatAnchor(market.seats)};
    if (!seat_anchor || !market.chain->StillCanonical(*seat_anchor)) {
        HaltMarket(market, FlowMeshRuntimeHalt::ANCHOR_INVALIDATED,
                   "active FlowMesh seat anchor is no longer canonical");
        return false;
    }
    for (const auto& [key, sequence] : market.committed_anchors) {
        (void)sequence;
        const flowmesh::AnchorRef anchor{key.first, key.second};
        if (!market.chain->StillCanonical(anchor)) {
            HaltMarket(market, FlowMeshRuntimeHalt::ANCHOR_INVALIDATED,
                       "a committed FlowMesh anchor is no longer canonical");
            return false;
        }
    }
    return true;
}

template <typename Market>
std::optional<FlowMeshSeatTransition> CurrentSeatTransition(Market& market)
{
    if (!market.ready || !RecheckAnchors(market)) return std::nullopt;
    FlowMeshSeatTransition transition{market.chain->SeatTransition(
        market.domain, market.market_id, market.seats)};
    switch (transition.kind) {
    case FlowMeshSeatTransitionKind::CONTINUE:
        if (transition.next_seats) break;
        market.paused = false;
        return transition;
    case FlowMeshSeatTransitionKind::PAUSED:
        if (transition.next_seats) break;
        market.paused = true;
        return transition;
    case FlowMeshSeatTransitionKind::HANDOFF:
        if (transition.next_seats &&
            flowmesh::CheckActiveFnBlsSeatSet(
                market.domain, *transition.next_seats) ==
                flowmesh::BlsSeatSetCheck::OK &&
            transition.next_seats->market_id == market.market_id &&
            market.seats.epoch != std::numeric_limits<uint64_t>::max() &&
            transition.next_seats->epoch == market.seats.epoch + 1 &&
            !SameSeatIdentity(*transition.next_seats, market.seats)) {
            market.paused = false;
            return transition;
        }
        break;
    }
    HaltMarket(market, FlowMeshRuntimeHalt::ANCHOR_INVALIDATED,
               "canonical FlowMesh seat-transition result is malformed");
    return std::nullopt;
}

template <typename Market>
bool AuthenticateAction(const Market& market, const flowmesh::Action& action)
{
    if (static_cast<flowmesh::ActionType>(action.type) ==
        flowmesh::ActionType::CLAIM_SEAT_REWARD) {
        const auto historical{market.chain->HistoricalRewardSeat(
            market.domain, market.market_id, action.signer)};
        return historical && flowmesh::CheckProductionActionCredential(
                                 action, market.domain, market.state.ConfigId(),
                                 market.market_id, &historical->seats,
                                 &historical->member);
    }
    return flowmesh::CheckProductionActionCredential(
        action, market.domain, market.state.ConfigId(), market.market_id);
}

template <typename Market>
bool AuthenticateCandidateEvidence(
    const Market& market, const flowmesh::ProductionEntryCore& entry,
    const std::span<const flowmesh::Action> evidence)
{
    if (entry.actions.size() != evidence.size()) return false;
    for (size_t i{0}; i < evidence.size(); ++i) {
        flowmesh::Action stripped{evidence[i]};
        stripped.credential.clear();
        const auto semantic_bytes{
            flowmesh::EncodeProductionActionPayload(entry.actions[i])};
        const auto stripped_bytes{
            flowmesh::EncodeProductionActionPayload(stripped)};
        if (!semantic_bytes || !stripped_bytes ||
            *semantic_bytes != *stripped_bytes ||
            !AuthenticateAction(market, evidence[i])) {
            return false;
        }
    }
    return true;
}

template <typename Market>
bool ReconcileConnectedHandoff(Market& market)
{
    if (!market.pending_handoff) return true;
    if (!market.ready || market.next_sequence == 0) {
        HaltMarket(market, FlowMeshRuntimeHalt::STORE_FAILURE,
                   "pending FlowMesh handoff has no durable entry");
        return false;
    }
    std::optional<StoredProductionEntry> stored;
    std::string error;
    if (!market.store->ReadEntry(market.next_sequence - 1, market.seats,
                                 stored, error) ||
        !stored || stored->entry.kind != static_cast<uint8_t>(
            flowmesh::ProductionEntryKind::EPOCH_HANDOFF) ||
        stored->entry.GetHash() != market.last_hash) {
        HaltMarket(market, FlowMeshRuntimeHalt::STORE_FAILURE,
                   error.empty() ? "pending FlowMesh handoff is unreadable"
                                 : std::move(error));
        return false;
    }
    const auto core{market.chain->ConnectedHandoffCheckpoint(
        market.domain, market.market_id, market.last_hash)};
    if (!core) return true;
    const int32_t tip_height{market.chain->TipHeight()};
    if (!FlowMeshHandoffConnectionMature(core->connection, tip_height)) {
        return true;
    }
    const auto next{market.chain->SeatSet(
        market.domain, market.market_id, stored->entry.next_epoch,
        stored->entry.next_seat_set_hash)};
    if (!next || flowmesh::CheckActiveFnBlsSeatSet(market.domain, *next) !=
                     flowmesh::BlsSeatSetCheck::OK) {
        HaltMarket(market, FlowMeshRuntimeHalt::ANCHOR_INVALIDATED,
                   "connected FlowMesh handoff has no canonical next set");
        return false;
    }
    const modern::FlowMeshCheckpointRecordV1 checkpoint{core->core,
                                                        stored->certificate};
    if (!market.store->MarkHandoffCheckpointConnected(
            checkpoint, market.seats, *next, core->connection, tip_height,
            error)) {
        HaltMarket(market, FlowMeshRuntimeHalt::STORE_FAILURE,
                   error.empty()
                       ? "connected FlowMesh handoff cannot advance the store"
                       : std::move(error));
        return false;
    }
    market.seats = *next;
    market.pending_handoff = false;
    market.paused = false;
    market.round = 0;
    market.round_started = market.clock->Now();
    RoundEntered(market, "handoff_connected");
    return RecheckAnchors(market);
}

template <typename Market>
bool RefreshMarker(Market& market)
{
    if (!RecheckAnchors(market)) return false;
    std::optional<FlowMeshProductionStore::Marker> marker;
    std::string error;
    if (!market.store->ReadMarker(marker, error) || !marker) {
        HaltMarket(market, FlowMeshRuntimeHalt::STORE_FAILURE,
                   error.empty() ? "FlowMesh production marker is unavailable"
                                 : std::move(error));
        return false;
    }
    if (marker->domain != market.domain ||
        marker->market_id != market.market_id ||
        marker->next_sequence != market.next_sequence ||
        marker->next_effect_index != market.next_effect_index ||
        marker->last_microblock_hash != market.last_hash ||
        marker->state_root != market.state.Root()) {
        HaltMarket(market, FlowMeshRuntimeHalt::STORE_FAILURE,
                   "FlowMesh runtime state disagrees with its durable marker");
        return false;
    }
    if (market.pending_handoff &&
        marker->current_epoch == market.seats.epoch &&
        marker->current_seat_set_hash == market.seats.set_hash) {
        if (!ReconcileConnectedHandoff(market)) return false;
        if (!market.store->ReadMarker(marker, error) || !marker) {
            HaltMarket(market, FlowMeshRuntimeHalt::STORE_FAILURE,
                       error.empty()
                           ? "FlowMesh marker is unavailable after handoff"
                           : std::move(error));
            return false;
        }
    }
    if (marker->current_epoch != market.seats.epoch ||
        marker->current_seat_set_hash != market.seats.set_hash) {
        const auto next{market.chain->SeatSet(
            market.domain, market.market_id, marker->current_epoch,
            marker->current_seat_set_hash)};
        const auto anchor{next ? SeatAnchor(*next) : std::nullopt};
        if (!next || !anchor || marker->current_anchor != *anchor ||
            flowmesh::CheckActiveFnBlsSeatSet(market.domain, *next) !=
                flowmesh::BlsSeatSetCheck::OK) {
            HaltMarket(market, FlowMeshRuntimeHalt::ANCHOR_INVALIDATED,
                       "connected handoff cannot resolve its anchored seat set");
            return false;
        }
        market.seats = *next;
        market.pending_handoff = false;
        market.round = 0;
        market.round_started = market.clock->Now();
        RoundEntered(market, "marker_seat_set_changed");
    }
    return RecheckAnchors(market);
}

template <typename Market>
std::unique_ptr<typename Market::Candidate> EvaluateCandidate(
    Market& market, const flowmesh::ProductionEntryCore& entry,
    const std::vector<flowmesh::Action>* authenticated_evidence)
{
    if (market.halt != FlowMeshRuntimeHalt::NONE || market.pending_handoff ||
        entry.domain != market.domain || entry.market_id != market.market_id ||
        entry.epoch != market.seats.epoch ||
        entry.seat_set_hash != market.seats.set_hash ||
        entry.sequence != market.next_sequence ||
        entry.parent_hash != market.last_hash ||
        entry.previous_state_root != market.state.Root() ||
        !flowmesh::ProductionWireHeaderMatches(HeaderFor(entry), entry) ||
        !market.chain->Acceptable(entry.anchor) || !RecheckAnchors(market)) {
        return nullptr;
    }
    const auto transition{CurrentSeatTransition(market)};
    if (!transition) return nullptr;
    if (authenticated_evidence != nullptr &&
        !AuthenticateCandidateEvidence(market, entry,
                                       *authenticated_evidence)) {
        return nullptr;
    }

    const auto anchors{AnchorContext(market)};
    if (entry.kind == static_cast<uint8_t>(
                          flowmesh::ProductionEntryKind::EXECUTION)) {
        if (transition->kind != FlowMeshSeatTransitionKind::CONTINUE) {
            return nullptr;
        }
        flowmesh::ProductionEpochGate gate{market.domain, market.market_id,
                                           market.seats};
        flowmesh::ProductionEntryCheck check;
        DeliveryEvent(market, "execution_started", flowmesh::WireMessageKind::PROPOSAL,
                      entry.GetHash(), entry.sequence, std::nullopt, "evaluate_candidate");
        const auto executed{flowmesh::ExecuteProductionEntry(
            market.state, entry, market.domain, market.market_id, market.seats,
            gate, market.next_sequence, market.next_effect_index,
            market.last_hash, anchors,
            market.treasury_owner_commitment, market.deposits, check)};
        DeliveryEvent(market, executed ? "execution_completed" : "execution_failed",
                      flowmesh::WireMessageKind::PROPOSAL, entry.GetHash(), entry.sequence,
                      std::nullopt, flowmesh::ProductionEntryCheckName(check));
        if (!executed) return nullptr;
        return std::make_unique<typename Market::Candidate>(
            typename Market::Candidate{entry, executed->next_state,
                                       std::nullopt,
                                       authenticated_evidence
                                           ? *authenticated_evidence
                                           : std::vector<flowmesh::Action>{}});
    }
    if (entry.kind != static_cast<uint8_t>(
                          flowmesh::ProductionEntryKind::EPOCH_HANDOFF)) {
        return nullptr;
    }
    if (transition->kind != FlowMeshSeatTransitionKind::HANDOFF ||
        !transition->next_seats ||
        transition->next_seats->epoch != entry.next_epoch ||
        transition->next_seats->set_hash != entry.next_seat_set_hash) {
        return nullptr;
    }
    const auto& next{*transition->next_seats};
    flowmesh::ProductionEntryCheck check;
    const auto expected{flowmesh::BuildProductionHandoffEntry(
        market.state, market.domain, market.market_id, market.seats, next,
        market.next_sequence, market.next_effect_index, market.last_hash,
        entry.anchor, anchors, check)};
    if (!expected || expected->GetHash() != entry.GetHash()) return nullptr;
    return std::make_unique<typename Market::Candidate>(
        typename Market::Candidate{entry, market.state, next,
                                   authenticated_evidence
                                       ? *authenticated_evidence
                                       : std::vector<flowmesh::Action>{}});
}

template <typename Market>
flowmesh::PreagreementContext AgreementContext(const Market& market)
{
    return {market.domain, market.market_id, market.seats.epoch,
            market.seats.set_hash, market.next_sequence, market.last_hash,
            market.state.Root(), market.state.ConfigId()};
}

bool SameAgreementContext(const flowmesh::PreagreementContext& a,
                          const flowmesh::PreagreementContext& b)
{
    return a.domain == b.domain && a.market_id == b.market_id &&
        a.epoch == b.epoch && a.seat_set_hash == b.seat_set_hash &&
        a.sequence == b.sequence && a.parent_hash == b.parent_hash &&
        a.previous_state_root == b.previous_state_root &&
        a.execution_config_id == b.execution_config_id;
}

// Public authentication evidence, never wallet keys. Lengths are checked
// before allocation and every restored action is independently authenticated.
std::optional<std::vector<flowmesh::Action>> DecodeAgreementEvidence(
    std::span<const unsigned char> bytes)
{
    if (bytes.size() > flowmesh::FLOWMESH_ACTION_POOL_PER_MARKET_BYTES) return std::nullopt;
    try {
        DataStream stream{bytes};
        const auto count{ReadCompactSize(stream)};
        if (count > flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_ACTIONS) return std::nullopt;
        std::vector<flowmesh::Action> out;
        for (size_t i{0}; i < count; ++i) {
            const auto size{ReadCompactSize(stream)};
            if (!size || size > stream.size() ||
                size > flowmesh::PayloadLimitForWireKind(flowmesh::WireMessageKind::ACTION)) return std::nullopt;
            std::vector<unsigned char> payload(size);
            stream.read(std::as_writable_bytes(std::span{payload}));
            auto action{flowmesh::DecodeProductionActionPayload(payload)};
            if (!action) return std::nullopt;
            out.push_back(std::move(*action));
        }
        if (!stream.empty()) return std::nullopt;
        return out;
    } catch (const std::exception&) { return std::nullopt; }
}

template <typename Market>
std::optional<std::vector<unsigned char>> ValidateAgreementCandidate(
    Market& market, std::span<const unsigned char> bytes,
    std::optional<std::span<const unsigned char>> restored)
{
    if (!market.chain->Acceptable(market.chain->Current())) return std::nullopt;
    const auto entry{flowmesh::DecodeProductionEntry(bytes)};
    if (!entry) return std::nullopt;
    auto evidence{restored ? DecodeAgreementEvidence(*restored)
                          : market.pool.EvidenceFor(entry->actions)};
    // Locally constructed candidates already carry the selected exact actions.
    if (!restored) {
        const auto cached{market.candidates.find(entry->GetHash())};
        if (cached != market.candidates.end()) evidence = cached->second.evidence;
    }
    if (!evidence) return std::nullopt;
    auto candidate{EvaluateCandidate(market, *entry, &*evidence)};
    if (!candidate) return std::nullopt;
    if (!market.candidates.contains(entry->GetHash()) &&
        market.candidates.size() >= MAX_RUNTIME_CANDIDATES_PER_SEQUENCE) return std::nullopt;
    DataStream encoded;
    WriteCompactSize(encoded, evidence->size());
    for (const auto& action : *evidence) {
        const auto payload{flowmesh::EncodeProductionActionPayload(action)};
        if (!payload || payload->size() > flowmesh::FLOWMESH_ACTION_POOL_PER_MARKET_BYTES - 9 ||
            encoded.size() > flowmesh::FLOWMESH_ACTION_POOL_PER_MARKET_BYTES - payload->size() - 9) return std::nullopt;
        WriteCompactSize(encoded, payload->size());
        encoded.write(std::as_bytes(std::span{*payload}));
    }
    const auto hash{entry->GetHash()};
    market.candidates.try_emplace(hash, std::move(*candidate));
    if (!evidence->empty() && (!market.evidence_retry || market.evidence_retry->candidate_hash != hash)) {
        market.evidence_retry = typename Market::EvidenceRetry{hash, 0, market.clock->Now()};
    }
    market.evidence_retry_eligible = true;
    return std::vector<unsigned char>{UCharCast(encoded.data()), UCharCast(encoded.data()) + encoded.size()};
}

template <typename Market>
bool RestoreRetainedCandidate(Market& market)
{
    if (!market.pending_candidate_restore) return true;
    if (market.halt != FlowMeshRuntimeHalt::NONE) return false;
    market.paused = true;
    // Do not evaluate a saved candidate through the live production gate
    // until checkpoint/index reconciliation has finished. This also prevents
    // messages queued during startup from signing ahead of restoration.
    if (!market.chain->Acceptable(market.chain->Current())) return false;

    const auto& retained{*market.pending_candidate_restore};
    auto candidate{EvaluateCandidate(market, retained.entry,
                                     &retained.evidence)};
    if (!candidate || candidate->entry.GetHash() != retained.entry.GetHash()) {
        // A concurrent B3 reconciliation is a wait, not evidence that the
        // retained record is invalid. Never clear or replace its disk lock.
        if (market.halt == FlowMeshRuntimeHalt::NONE &&
            !market.chain->Acceptable(market.chain->Current())) {
            market.paused = true;
            return false;
        }
        HaltMarket(market, FlowMeshRuntimeHalt::SIGNING_CONFLICT,
                   "FlowMesh runtime cannot re-execute its retained signing candidate after chain reconciliation; signing remains blocked");
        market.paused = true;
        return false;
    }
    if (!candidate->evidence.empty()) {
        market.evidence_retry = typename Market::EvidenceRetry{
            candidate->entry.GetHash(), 0, market.clock->Now()};
    }
    market.diagnostics.local_locked_candidate = candidate->entry.GetHash();
    market.candidates.emplace(candidate->entry.GetHash(), std::move(*candidate));
    market.pending_candidate_restore.reset();
    return true;
}

template <typename Market>
bool RetainCandidateBeforeSigning(Market& market,
                                  const typename Market::Candidate& candidate)
{
    DeliveryEvent(market, "candidate_persist_started", flowmesh::WireMessageKind::PROPOSAL,
                  candidate.entry.GetHash(), candidate.entry.sequence);
    const auto result{market.store->LockCandidate(candidate.entry,
                                                   candidate.evidence)};
    if (result == flowmesh::ProductionLockResult::LOCKED ||
        result == flowmesh::ProductionLockResult::ALREADY_LOCKED_SAME) {
        market.diagnostics.local_locked_candidate = candidate.entry.GetHash();
        DeliveryEvent(market, "candidate_durable", flowmesh::WireMessageKind::PROPOSAL,
                      candidate.entry.GetHash(), candidate.entry.sequence, std::nullopt,
                      result == flowmesh::ProductionLockResult::LOCKED ? "new_lock" : "already_locked_same");
        if (!market.evidence_retry && !candidate.evidence.empty()) {
            market.evidence_retry = typename Market::EvidenceRetry{
                candidate.entry.GetHash(), 0,
                market.clock->Now() + FlowMeshEvidenceRetryBudget::INTERVAL};
        }
        return true;
    }
    DeliveryEvent(market, "candidate_persist_failed", flowmesh::WireMessageKind::PROPOSAL,
                  candidate.entry.GetHash(), candidate.entry.sequence, std::nullopt,
                  result == flowmesh::ProductionLockResult::CONFLICT ? "lock_conflict" : "storage_failure");
    HaltMarket(market, FlowMeshRuntimeHalt::SIGNING_CONFLICT,
               result == flowmesh::ProductionLockResult::CONFLICT
                   ? "permanent FlowMesh candidate lock conflicts"
                   : "permanent FlowMesh candidate retention failed");
    return false;
}

template <typename Market>
flowmesh::ClientEvent MakeClientActionEvent(Market& market, const flowmesh::ClientEventKind kind,
                                           const flowmesh::Action& action, std::string reason,
                                           const flowmesh::ProductionEntryCore* certified)
{
    flowmesh::ClientEvent event;
    event.market_id = market.market_id;
    event.kind = kind;
    event.action_id = action.Id();
    event.account_id = action.signer;
    event.account_sequence = action.sequence;
    event.action_type = action.type;
    event.microblock_sequence = certified ? certified->sequence : market.next_sequence;
    if (certified) {
        event.microblock_hash = certified->GetHash();
    } else if (const auto bytes{flowmesh::EncodeProductionActionPayload(action)}) {
        event.signed_action_hash = Hash(*bytes);
    }
    event.reason = std::move(reason);
    return event;
}

template <typename Market>
void ClientActionEvent(Market& market, const flowmesh::ClientEventKind kind,
                       const flowmesh::Action& action, std::string reason = {},
                       const flowmesh::ProductionEntryCore* certified = nullptr)
{
    market.client_events.Append(MakeClientActionEvent(market, kind, action, std::move(reason), certified));
}

template <typename Market>
bool CertifiedAnchorReconciliationInterrupted(
    Market& market, const flowmesh::ProductionEntryCore& entry,
    const uint64_t delivery_generation)
{
    if (market.halt != FlowMeshRuntimeHalt::NONE ||
        (delivery_generation == market.chain->DeliveryGeneration() &&
         market.chain->Acceptable(market.chain->Current()))) return false;
    if (!RecheckAnchors(market)) return false;
    if (!market.chain->StillCanonical(entry.anchor) ||
        (entry.kind == static_cast<uint8_t>(flowmesh::ProductionEntryKind::EPOCH_HANDOFF) &&
         !market.chain->StillCanonical(entry.next_anchor))) {
        HaltMarket(market, FlowMeshRuntimeHalt::ANCHOR_INVALIDATED,
                   "certified anchor became non-canonical during validation");
        return false;
    }
    return true;
}

template <typename Market>
bool DeferCertifiedAnchorRejection(
    Market& market, const flowmesh::ProductionEntryCore& entry,
    const std::optional<flowmesh::ProductionEntryCheck> validation_failure,
    const uint64_t delivery_generation)
{
    // Only a typed validation refusal known to precede every store write can
    // be retried. Never reinterpret a disk error or uncertain append as a
    // temporary chain gate, even when reconciliation overlaps that failure.
    if (validation_failure != flowmesh::ProductionEntryCheck::BAD_ANCHOR ||
        !CertifiedAnchorReconciliationInterrupted(market, entry, delivery_generation)) return false;
    // Direct received certificates are retained by ProcessMessage's gate
    // check; HandleEntries retains catch-up certificates on this exact result.
    // Locally assembled certificates retain their candidate and votes.
    // Both paths re-execute the same exact entry after reconciliation.
    DeliveryEvent(market, "publication_deferred", flowmesh::WireMessageKind::CERTIFICATE,
                  entry.GetHash(), entry.sequence, std::nullopt,
                  "anchor validation interrupted by chain reconciliation", EntryTrace(entry));
    return true;
}

template <typename Market>
bool CommitCertified(Market& market,
                     const flowmesh::ProductionCertifiedEnvelope& certified,
                     typename Market::Candidate& candidate,
                     bool* reconciliation_deferred = nullptr)
{
    if (reconciliation_deferred) *reconciliation_deferred = false;
    const uint64_t delivery_generation{market.chain->DeliveryGeneration()};
    if (!RecheckAnchors(market) ||
        !market.chain->Acceptable(certified.entry.anchor)) {
        if (!market.chain->StillCanonical(certified.entry.anchor)) {
            HaltMarket(market, FlowMeshRuntimeHalt::ANCHOR_INVALIDATED,
                       "candidate anchor became non-canonical before commit");
        }
        if (reconciliation_deferred &&
            CertifiedAnchorReconciliationInterrupted(market, certified.entry, delivery_generation)) {
            *reconciliation_deferred = true;
            DeliveryEvent(market, "certificate_validation_deferred", flowmesh::WireMessageKind::CERTIFICATE,
                          certified.entry.GetHash(), certified.entry.sequence, std::nullopt,
                          "verified certificate interrupted before append", EntryTrace(certified.entry));
        }
        return false;
    }
    std::string error;
    std::optional<flowmesh::ProductionEntryCheck> validation_failure;
    DeliveryEvent(market, "publication_started", flowmesh::WireMessageKind::CERTIFICATE,
                  certified.entry.GetHash(), certified.entry.sequence, std::nullopt, {}, EntryTrace(certified.entry));
    if (certified.entry.kind == static_cast<uint8_t>(
                                    flowmesh::ProductionEntryKind::EXECUTION)) {
        flowmesh::FlowMeshState persisted{market.state};
        if (!market.store->AppendExecution(
                certified.entry, certified.certificate, market.seats,
                market.state, AnchorContext(market),
                market.treasury_owner_commitment, market.deposits, persisted,
                error, &validation_failure) ||
            persisted.Root() != candidate.next_state.Root()) {
            if (DeferCertifiedAnchorRejection(market, certified.entry,
                                              validation_failure, delivery_generation)) {
                if (reconciliation_deferred) *reconciliation_deferred = true;
                return false;
            }
            DeliveryEvent(market, "publication_failed", flowmesh::WireMessageKind::CERTIFICATE,
                          certified.entry.GetHash(), certified.entry.sequence, std::nullopt, error, EntryTrace(certified.entry));
            HaltMarket(market, FlowMeshRuntimeHalt::STORE_FAILURE,
                       error.empty()
                           ? "durable execution result disagrees with independent execution"
                           : std::move(error));
            return false;
        }
        market.state = std::move(persisted);
    } else {
        if (!candidate.next_seats ||
            !market.store->AppendHandoff(
                certified.entry, certified.certificate, market.seats,
                *candidate.next_seats, market.state, AnchorContext(market),
                error, &validation_failure)) {
            if (DeferCertifiedAnchorRejection(market, certified.entry,
                                              validation_failure, delivery_generation)) {
                if (reconciliation_deferred) *reconciliation_deferred = true;
                return false;
            }
            DeliveryEvent(market, "publication_failed", flowmesh::WireMessageKind::CERTIFICATE,
                          certified.entry.GetHash(), certified.entry.sequence, std::nullopt, error, EntryTrace(certified.entry));
            HaltMarket(market, FlowMeshRuntimeHalt::STORE_FAILURE,
                       error.empty() ? "durable handoff append failed"
                                     : std::move(error));
            return false;
        }
        market.pending_handoff = true;
    }

    market.last_hash = certified.entry.GetHash();
    CountObservation(market.delivery.durably_applied);
    DeliveryEvent(market, "durably_applied", flowmesh::WireMessageKind::CERTIFICATE,
                  market.last_hash, certified.entry.sequence, std::nullopt, {}, EntryTrace(certified.entry));
    TraceActions(market, "action_certified", certified.entry);
    market.certified_state_root = certified.entry.state_root;
    market.client_head = certified;
    market.client_head_seat_count = market.seats.Size();
    flowmesh::ClientEvent head_event;
    head_event.market_id = market.market_id;
    head_event.kind = flowmesh::ClientEventKind::CERTIFIED_HEAD;
    head_event.microblock_sequence = certified.entry.sequence;
    head_event.microblock_hash = market.last_hash;
    market.client_events.Append(std::move(head_event));
    for (const auto& action : certified.entry.actions) {
        ClientActionEvent(market, flowmesh::ClientEventKind::CERTIFIED_INCLUDED,
                          action, "semantic inclusion only; execution outcome not established by this event",
                          &certified.entry);
    }
    market.local_observed_at = GetTime();
    ++market.next_sequence;
    market.pool.ClearDuplicateForwards(); // never rewrite a queued retry to the new head
    market.next_effect_index += certified.entry.effect_count;
    market.previous_anchor = certified.entry.anchor;
    market.committed_anchors[{certified.entry.anchor.height,
                              certified.entry.anchor.hash}] =
        certified.entry.sequence;
    market.pool.DropCertified(certified.entry.actions);
    market.pool.Prune(market.state);
    market.candidates.clear();
    market.attestations.clear();
    market.attested_hash_by_seat.clear();
    market.evidence_retry.reset();
    market.evidence_retry_eligible = false;
    const auto last_message_observed_at{market.diagnostics.last_message_observed_at};
    market.diagnostics = {};
    market.diagnostics.last_message_observed_at = last_message_observed_at;
    market.round = 0;
    market.round_started = market.clock->Now();
    RoundEntered(market, "certificate_committed");
    if (market.pending_handoff && !ReconcileConnectedHandoff(market)) {
        return false;
    }
    return true;
}

} // namespace

const char* FlowMeshRuntimeHaltName(const FlowMeshRuntimeHalt halt)
{
    switch (halt) {
    case FlowMeshRuntimeHalt::NONE: return "none";
    case FlowMeshRuntimeHalt::INVALID_CONFIG: return "invalid-config";
    case FlowMeshRuntimeHalt::STORE_FAILURE: return "store-failure";
    case FlowMeshRuntimeHalt::SIGNING_CONFLICT: return "signing-conflict";
    case FlowMeshRuntimeHalt::CERTIFICATE_CONFLICT:
        return "certificate-conflict";
    case FlowMeshRuntimeHalt::ANCHOR_INVALIDATED: return "anchor-invalidated";
    }
    return "unknown";
}

struct FlowMeshRuntime::PendingDelivery {
    uint256 key, object;
    FlowMeshRuntimeRelay relay;
    flowmesh::AnchorRef anchor;
    uint32_t seat{0};
    bool local_key_required{false};
    flowmesh::WireClock::time_point next_attempt{};
    std::optional<flowmesh::WireClock::time_point> paused_since;
    std::map<flowmesh::WirePeerId, flowmesh::WireClock::time_point> outstanding;
};

void FlowMeshRuntime::EraseDelivery(const uint64_t id, const char* reason)
{
    const auto it{m_deliveries.find(id)};
    if (it == m_deliveries.end()) return;
    const auto& pending{*it->second};
    auto& market{*m_markets.at(pending.relay.message.header.market_id)};
    const auto bytes{pending.relay.message.MemoryUsage()};
    m_delivery_bytes -= bytes;
    market.delivery.pending_bytes -= bytes;
    --market.delivery.pending_objects;
    CountObservation(market.delivery.cancelled);
    market.delivery.current_reason = reason;
    DeliveryEvent(market, reason, pending.relay.message.kind, pending.object,
                  pending.relay.message.header.sequence, pending.relay.peer, reason);
    m_delivery_keys.erase(pending.key);
    m_deliveries.erase(it);
    if (m_config.cancel_delivery) m_config.cancel_delivery(id);
}

void FlowMeshRuntime::AttemptDelivery(PendingDelivery& pending, const bool retry)
{
    auto& market{*m_markets.at(pending.relay.message.header.market_id)};
    const auto now{m_config.clock->Now()};
    pending.next_attempt = now + DELIVERY_REPEAT;
    if (retry) CountObservation(market.delivery.retried);
    auto trace{WireTrace(pending.relay.message)};
    trace.delivery_id = pending.relay.delivery_id;
    DeliveryEvent(market, "relay_attempt", pending.relay.message.kind, pending.object,
                  pending.relay.message.header.sequence, pending.relay.peer, {}, trace);
    FlowMeshRelayResult result;
    try {
        result = m_config.relay(pending.relay);
    } catch (const std::exception&) {
        result.no_peer_reason = FlowMeshDeliveryAdmission::DISCONNECTED;
        result.reason = "relay_callback_exception";
    }
    const auto observe = [&](const FlowMeshDeliveryAdmission admission,
                             const std::optional<flowmesh::WirePeerId> peer,
                             const std::string& reason) {
        market.delivery.current_reason = reason.empty()
            ? FlowMeshDeliveryAdmissionName(admission) : reason.substr(0, 160);
        if (admission == FlowMeshDeliveryAdmission::ADMITTED && peer &&
            pending.outstanding.size() < MAX_DELIVERY_PEERS) {
            pending.outstanding.insert_or_assign(*peer, now + DELIVERY_COMPLETION_TIMEOUT);
            CountObservation(market.delivery.admitted);
            DeliveryEvent(market, "admitted", pending.relay.message.kind,
                          pending.object, pending.relay.message.header.sequence, peer, market.delivery.current_reason);
        } else if (admission == FlowMeshDeliveryAdmission::LEGACY_UNTRACKED) {
            CountObservation(market.delivery.admitted);
            DeliveryEvent(market, "legacy_untracked", pending.relay.message.kind,
                          pending.object, pending.relay.message.header.sequence, peer, market.delivery.current_reason);
        } else {
            CountObservation(market.delivery.refused);
            DeliveryEvent(market, "refused", pending.relay.message.kind,
                          pending.object, pending.relay.message.header.sequence, peer, market.delivery.current_reason);
        }
    };
    if (result.peers.empty()) observe(result.no_peer_reason, std::nullopt, result.reason);
    else {
        for (size_t i{0}; i < std::min(result.peers.size(), MAX_DELIVERY_PEERS); ++i) {
            observe(result.peers[i].admission, result.peers[i].peer, result.peers[i].reason);
        }
        if (result.peers.size() > MAX_DELIVERY_PEERS) {
            DeliveryEvent(market, "admission_observations_truncated", pending.relay.message.kind,
                          pending.object, pending.relay.message.header.sequence,
                          std::nullopt, "runtime_tracks_at_most_64_targets_per_attempt");
        }
        // A dual route may have legacy peers while its independent route has
        // no peers. The service preserves that explicit branch refusal here.
        if (result.no_peer_reason != FlowMeshDeliveryAdmission::ADMITTED &&
            result.no_peer_reason != FlowMeshDeliveryAdmission::LEGACY_UNTRACKED) {
            observe(result.no_peer_reason, std::nullopt, result.reason);
        }
    }
}

void FlowMeshRuntime::RelayMessage(
    Market& market, flowmesh::WireMessage message,
    const std::optional<flowmesh::WirePeerId> peer,
    const std::optional<flowmesh::WirePeerId> exclude,
    const bool replay_budget_charged)
{
    using Kind = flowmesh::WireMessageKind;
    if (message.kind != Kind::PROPOSAL && message.kind != Kind::ATTESTATION &&
        message.kind != Kind::CERTIFICATE) {
        // ACTION and catch-up retain their existing independent bounded retry
        // owners. A control send is not an observed remote admission.
        const auto kind{message.kind};
        const auto sequence{message.header.sequence};
        auto object{Hash(message.payload)};
        if (kind == Kind::ACTION) {
            const auto action{flowmesh::DecodeProductionActionPayload(message.payload)};
            if (action) object = action->Id();
        }
        CountObservation(market.delivery.created);
        DeliveryEvent(market, "created", kind, object, sequence, peer, {}, WireTrace(message));
        const auto result{m_config.relay(FlowMeshRuntimeRelay{peer, exclude, std::move(message)})};
        const auto observe = [&](FlowMeshDeliveryAdmission admission,
                                 std::optional<flowmesh::WirePeerId> target,
                                 const std::string& detail) {
            market.delivery.current_reason = detail.empty()
                ? FlowMeshDeliveryAdmissionName(admission) : detail.substr(0, 160);
            const bool admitted{admission == FlowMeshDeliveryAdmission::ADMITTED ||
                admission == FlowMeshDeliveryAdmission::LEGACY_UNTRACKED};
            CountObservation(admitted ? market.delivery.admitted : market.delivery.refused);
            DeliveryEvent(market, admitted ? "untracked_admission" : "refused",
                          kind, object, sequence, target, market.delivery.current_reason);
        };
        for (size_t i{0}; i < std::min(result.peers.size(), MAX_DELIVERY_PEERS); ++i) {
            observe(result.peers[i].admission, result.peers[i].peer, result.peers[i].reason);
        }
        if (result.peers.empty() || (result.no_peer_reason != FlowMeshDeliveryAdmission::ADMITTED &&
            result.no_peer_reason != FlowMeshDeliveryAdmission::LEGACY_UNTRACKED)) {
            observe(result.no_peer_reason, std::nullopt, result.reason);
        }
        return;
    }
    uint256 object;
    flowmesh::AnchorRef anchor;
    uint32_t seat{0};
    if (message.kind == Kind::PROPOSAL) {
        const auto proposal{flowmesh::DecodeProductionProposalPayload(message.payload)};
        if (!proposal) return;
        object = proposal->entry.GetHash();
        anchor = proposal->entry.anchor;
        seat = proposal->proposer_seat_index;
    } else if (message.kind == Kind::ATTESTATION) {
        const auto vote{flowmesh::DecodeProductionAttestationPayload(message.payload)};
        if (!vote) return;
        seat = vote->seat_index;
        const auto hash{market.attested_hash_by_seat.find(seat)};
        if (hash == market.attested_hash_by_seat.end()) return;
        const auto candidate{market.candidates.find(hash->second)};
        if (candidate == market.candidates.end()) return;
        object = hash->second;
        anchor = candidate->second.entry.anchor;
    } else {
        // Only HandleCertificate, after durable application, publishes here.
        object = market.last_hash;
        if (message.header.sequence + 1 != market.next_sequence || !market.previous_anchor) return;
        anchor = *market.previous_anchor;
        // Durable advancement supersedes this market's older gossip. Cancel
        // queued unsent copies before admitting the new certified head.
        std::vector<uint64_t> obsolete;
        for (const auto& [id, old] : m_deliveries) {
            if (old->relay.message.header.market_id == market.market_id &&
                old->relay.message.header.sequence <= message.header.sequence &&
                (old->object != object || old->relay.message.kind != Kind::CERTIFICATE)) obsolete.push_back(id);
        }
        for (const auto id : obsolete) EraseDelivery(id, "superseded_by_durable_head");
    }
    const uint256 key{DeliveryKey(market.market_id, message.kind, object, seat, peer)};
    const auto existing{m_delivery_keys.find(key)};
    if (existing != m_delivery_keys.end()) {
        // Equivalent certificates may have different valid signer subsets.
        // Retention coalesces storage, not an already-due caller-authorized
        // replay. Incoming verified repeats must propagate without depending
        // on a separate Tick on every intermediate node.
        const uint64_t id{existing->second};
        if (!RecheckDelivery(id)) return;
        auto& pending{*m_deliveries.at(id)};
        const auto now{m_config.clock->Now()};
        if (!pending.outstanding.empty() || now < pending.next_attempt) return;
        const size_t bytes{pending.relay.message.MemoryUsage()};
        if (!replay_budget_charged) {
            if (!m_delivery_retry_budget.Available(now, bytes) ||
                !market.committee_relay_budget.Available(now, bytes)) return;
            m_delivery_retry_budget.Charge(bytes);
            market.committee_relay_budget.Charge(bytes);
        }
        // Routing metadata is not signed. Exclude this newly verified source
        // while retaining the first exact checked header/payload.
        pending.relay.exclude_peer = exclude;
        RestartDelivery(id);
        return;
    }
    const size_t bytes{message.MemoryUsage()};
    if (m_deliveries.size() >= MAX_DELIVERY_OBJECTS ||
        bytes > MAX_DELIVERY_BYTES - m_delivery_bytes ||
        bytes > MAX_MARKET_DELIVERY_BYTES - market.delivery.pending_bytes ||
        m_next_delivery_id == std::numeric_limits<uint64_t>::max()) {
        CountObservation(market.delivery.retention_refused);
        m_delivery_retention_waiting = true;
        market.delivery.current_reason = "runtime_retention_full";
        DeliveryEvent(market, "retention_refused", message.kind, object, message.header.sequence, peer, market.delivery.current_reason);
        // Never claim a send. Candidate/vote caches and durable certified
        // history remain intact; normal bounded gossip/catch-up can retry.
        return;
    }
    auto pending{std::make_unique<PendingDelivery>()};
    pending->key = key;
    pending->object = object;
    pending->anchor = anchor;
    pending->seat = seat;
    if (message.kind != Kind::CERTIFICATE) {
        pending->local_key_required = market.candidates.at(object).local_signers.contains(seat);
    }
    const auto id{m_next_delivery_id++};
    pending->relay = {peer, exclude, std::move(message), id};
    m_delivery_keys.emplace(key, id);
    m_delivery_bytes += bytes;
    market.delivery.pending_bytes += bytes;
    ++market.delivery.pending_objects;
    CountObservation(market.delivery.created);
    DeliveryEvent(market, "created", pending->relay.message.kind, object,
                  pending->relay.message.header.sequence, peer, {}, WireTrace(pending->relay.message));
    auto* object_ptr{pending.get()};
    m_deliveries.emplace(id, std::move(pending));
    if (RecheckDelivery(id)) AttemptDelivery(*object_ptr, false);
}

void FlowMeshRuntime::RestartDelivery(const uint64_t id)
{
    auto& pending{*m_deliveries.at(id)};
    if (m_next_delivery_id == std::numeric_limits<uint64_t>::max()) {
        m_markets.at(pending.relay.message.header.market_id)->delivery.current_reason = "delivery_id_exhausted";
        return;
    }
    // A late completion from an old attempt can never complete its successor.
    const uint64_t next_id{m_next_delivery_id++};
    pending.relay.delivery_id = next_id;
    m_delivery_keys[pending.key] = next_id;
    auto owned{m_deliveries.extract(id)};
    owned.key() = next_id;
    m_deliveries.insert(std::move(owned));
    AttemptDelivery(pending, true);
}

bool FlowMeshRuntime::RecheckDelivery(const uint64_t id)
{
    const auto found{m_deliveries.find(id)};
    if (found == m_deliveries.end()) return false;
    auto& pending{*found->second};
    auto& market{*m_markets.at(pending.relay.message.header.market_id)};
    const bool certificate{pending.relay.message.kind == flowmesh::WireMessageKind::CERTIFICATE};
    if (!market.chain->StillCanonical(pending.anchor) ||
        (certificate ? pending.object != market.last_hash
                     : pending.relay.message.header.sequence != market.next_sequence ||
                       pending.relay.message.header.epoch != market.seats.epoch ||
                       !market.candidates.contains(pending.object) ||
                       (market.diagnostics.local_locked_candidate &&
                        *market.diagnostics.local_locked_candidate != pending.object))) {
        EraseDelivery(id, "obsolete");
        return false;
    }
    if (market.halt != FlowMeshRuntimeHalt::NONE) {
        EraseDelivery(id, "market_halted");
        return false;
    }
    const auto pause = [&](const char* reason) {
        const auto now{m_config.clock->Now()};
        if (!pending.paused_since) pending.paused_since = now;
        if (!pending.outstanding.empty() && m_config.cancel_delivery) m_config.cancel_delivery(id);
        pending.outstanding.clear();
        if (now >= *pending.paused_since + DELIVERY_PAUSE_MAX_AGE) {
            EraseDelivery(id, "pause_expired");
            return false;
        }
        market.delivery.current_reason = reason;
        return false;
    };
    if (!market.chain->Acceptable(market.chain->Current())) return pause("chain_context_paused");
    if (!certificate && pending.local_key_required) {
        const auto generation{market.chain->DeliveryGeneration()};
        const auto keys{LocalSeatKeys(market)};
        if (std::none_of(keys.begin(), keys.end(), [&](const auto& item) {
            return item.first == pending.seat;
        })) {
            // The service deliberately hides keys during reconciliation.
            // An empty list in that window is not a wallet-disarm signal.
            if (generation != market.chain->DeliveryGeneration() ||
                !market.chain->Acceptable(market.chain->Current())) return pause("chain_context_paused");
            EraseDelivery(id, "local_signer_disarmed");
            return false;
        }
    }
    if (!certificate) {
        const auto transition{CurrentSeatTransition(market)};
        const auto& entry{market.candidates.at(pending.object).entry};
        const bool handoff{entry.kind == static_cast<uint8_t>(flowmesh::ProductionEntryKind::EPOCH_HANDOFF)};
        if (!transition || !market.chain->Acceptable(pending.anchor) ||
            (!handoff && transition->kind != FlowMeshSeatTransitionKind::CONTINUE) ||
            (handoff && (transition->kind != FlowMeshSeatTransitionKind::HANDOFF ||
             !transition->next_seats || transition->next_seats->set_hash != entry.next_seat_set_hash))) {
            return pause("chain_context_paused");
        }
    }
    pending.paused_since.reset();
    return true;
}

void FlowMeshRuntime::RetryDeliveries()
{
    const auto now{m_config.clock->Now()};
    // Bounded work per tick, with a rotating cursor independent of key order.
    auto it{m_deliveries.upper_bound(m_delivery_cursor)};
    const size_t count{std::min<size_t>(m_deliveries.size(), 64)};
    uint64_t last_scanned{m_delivery_cursor};
    std::optional<uint64_t> last_serviced;
    std::optional<uint64_t> first_budget_denied;
    for (size_t scanned{0}; scanned < count && !m_deliveries.empty(); ++scanned) {
        if (it == m_deliveries.end()) it = m_deliveries.begin();
        const uint64_t id{it->first};
        ++it;
        last_scanned = id;
        if (!RecheckDelivery(id)) continue;
        auto& pending{*m_deliveries.at(id)};
        auto& market{*m_markets.at(pending.relay.message.header.market_id)};
        if (!pending.outstanding.empty()) {
            const bool expired{std::any_of(pending.outstanding.begin(), pending.outstanding.end(),
                [&](const auto& target) { return now >= target.second; })};
            if (!expired) continue;
            if (m_config.cancel_delivery) m_config.cancel_delivery(id);
            pending.outstanding.clear();
            CountObservation(market.delivery.completion_timeouts);
            market.delivery.current_reason = "completion_timeout";
            DeliveryEvent(market, "completion_timeout", pending.relay.message.kind,
                          pending.object, pending.relay.message.header.sequence,
                          pending.relay.peer, market.delivery.current_reason);
        }
        if (m_delivery_retention_waiting && now >= pending.next_attempt) {
            // Compact verified sources remain owned by candidates/store.
            // Rotate one completed/expired slot when another object waits.
            EraseDelivery(id, "retention_rotation");
            m_delivery_retention_waiting = false;
            continue;
        }
        const size_t bytes{pending.relay.message.MemoryUsage()};
        if (now < pending.next_attempt) continue;
        if (!m_delivery_retry_budget.Available(now, bytes) ||
            !market.committee_relay_budget.Available(now, bytes)) {
            if (!first_budget_denied) first_budget_denied = id;
            continue;
        }
        m_delivery_retry_budget.Charge(bytes);
        market.committee_relay_budget.Charge(bytes);
        RestartDelivery(id);
        last_serviced = id;
    }
    // Successful retries move to new IDs at the map's tail. Resume at the
    // first budget-denied object even if a later market managed to send;
    // otherwise that market's sends could repeatedly skip the earlier tail.
    // IDs start at one. Without a denial, keep scanning past idle/paused work.
    // This is bounded object fairness, not a per-market latency guarantee:
    // a large market backlog can still take multiple bounded passes to drain.
    m_delivery_cursor = first_budget_denied ? *first_budget_denied - 1
                                            : last_serviced.value_or(last_scanned);
}

void FlowMeshRuntime::DeferMessage(Market& market, const flowmesh::QueuedWireMessage& queued)
{
    HashWriter writer;
    writer << static_cast<uint8_t>(queued.message.kind) << queued.message.header.market_id
           << queued.message.header.epoch << queued.message.header.sequence << queued.message.payload;
    const auto key{writer.GetHash()};
    if (m_deferred_messages.contains(key)) return;
    const size_t bytes{queued.message.MemoryUsage()};
    const auto peer_count{std::count_if(m_deferred_messages.begin(), m_deferred_messages.end(),
        [&](const auto& item) { return item.second.peer == queued.peer; })};
    if (m_deferred_messages.size() >= 128 || bytes > 16 * 1024 * 1024 - m_deferred_bytes ||
        market.delivery.deferred_objects >= 16 || peer_count >= 8) {
        CountObservation(market.delivery.receive_refused);
        market.delivery.current_reason = "receive_retention_full";
        DeliveryEvent(market, "receive_discarded", queued.message.kind, key,
                      queued.message.header.sequence, queued.peer, market.delivery.current_reason);
        if (m_receive_recovery.size() < 128) m_receive_recovery.emplace(queued.peer, market.market_id);
        return;
    }
    m_deferred_messages.emplace(key, queued);
    m_deferred_deadlines.emplace(key, m_config.clock->Now() + std::chrono::seconds{60});
    m_deferred_bytes += bytes;
    market.delivery.deferred_bytes += bytes;
    ++market.delivery.deferred_objects;
    CountObservation(market.delivery.receive_deferred);
    market.delivery.current_reason = "b3_reconciling_after_admission";
    DeliveryEvent(market, "receive_deferred", queued.message.kind, key,
                  queued.message.header.sequence, queued.peer, market.delivery.current_reason);
}

void FlowMeshRuntime::RetryDeferredMessages()
{
    const auto now{m_config.clock->Now()};
    if (now < m_next_deferred_retry) return;
    m_next_deferred_retry = now + std::chrono::milliseconds{250};
    auto it{m_deferred_messages.upper_bound(m_deferred_cursor)};
    for (size_t scanned{0}, maximum{std::min<size_t>(m_deferred_messages.size(), 8)};
         scanned < maximum && !m_deferred_messages.empty(); ++scanned) {
        if (it == m_deferred_messages.end()) it = m_deferred_messages.begin();
        const auto current{it++};
        m_deferred_cursor = current->first;
        const auto& queued{current->second};
        auto& market{*m_markets.at(queued.message.header.market_id)};
        const bool expired{now >= m_deferred_deadlines.at(current->first)};
        if (!expired && !market.chain->Acceptable(market.chain->Current())) continue;
        const bool obsolete{queued.message.kind != flowmesh::WireMessageKind::CERTIFICATE &&
                            queued.message.header.sequence < market.next_sequence};
        if (!expired && !obsolete && EnqueueWireMessage(queued.peer, queued.message) != flowmesh::QueueResult::ACCEPTED) {
            market.delivery.current_reason = "receive_retry_queue_full";
            DeliveryEvent(market, "receive_retry_refused", queued.message.kind, current->first,
                          queued.message.header.sequence, queued.peer, market.delivery.current_reason);
            continue;
        }
        const size_t bytes{queued.message.MemoryUsage()};
        market.delivery.deferred_bytes -= bytes;
        --market.delivery.deferred_objects;
        m_deferred_bytes -= bytes;
        if (expired) {
            CountObservation(market.delivery.receive_refused);
            if (m_receive_recovery.size() < 128) m_receive_recovery.emplace(queued.peer, market.market_id);
        }
        DeliveryEvent(market, expired ? "receive_expired" : obsolete ? "receive_obsolete" : "receive_requeued", queued.message.kind,
                      current->first, queued.message.header.sequence, queued.peer);
        m_deferred_deadlines.erase(current->first);
        m_deferred_messages.erase(current);
    }
    auto recovery{m_receive_recovery_cursor ? m_receive_recovery.upper_bound(*m_receive_recovery_cursor)
                                            : m_receive_recovery.begin()};
    for (size_t attempts{0}, maximum{std::min<size_t>(m_receive_recovery.size(), 8)};
         attempts < maximum && !m_receive_recovery.empty(); ++attempts) {
        if (recovery == m_receive_recovery.end()) recovery = m_receive_recovery.begin();
        const auto current{recovery++};
        m_receive_recovery_cursor = *current;
        auto& market{*m_markets.at(current->second)};
        if (market.chain->Acceptable(market.chain->Current()) && TryRequestCatchup(market, current->first)) {
            m_receive_recovery.erase(current);
        }
    }
}

void FlowMeshRuntime::RegenerateDeliveries()
{
    const auto now{m_config.clock->Now()};
    if (m_markets.empty() || now < m_next_delivery_regeneration) return;
    m_next_delivery_regeneration = now + std::chrono::milliseconds{250};
    using Kind = flowmesh::WireMessageKind;
    auto market_it{m_markets.upper_bound(m_delivery_regeneration_cursor)};
    size_t remaining{32};
    for (size_t scanned{0}; scanned < std::min<size_t>(m_markets.size(), 4) && remaining; ++scanned) {
        if (market_it == m_markets.end()) market_it = m_markets.begin();
        auto& market{*market_it->second};
        m_delivery_regeneration_cursor = market_it++->first;
        if (!market.ready || market.halt != FlowMeshRuntimeHalt::NONE ||
            !market.chain->Acceptable(market.chain->Current())) continue;
        // This is a bounded list of references, not duplicated signed bodies.
        // At most eight candidates and one verified vote per committee seat.
        std::vector<std::pair<uint256, int64_t>> sources;
        if (market.next_sequence != 0) sources.emplace_back(market.last_hash, -2);
        for (const auto& [hash, candidate] : market.candidates) {
            if (candidate.relay_proof) sources.emplace_back(hash, -1);
            const auto votes{market.attestations.find(hash)};
            if (votes != market.attestations.end()) {
                for (const auto& [seat, vote] : votes->second) sources.emplace_back(hash, seat);
            }
        }
        if (sources.empty()) continue;
        const auto local_keys{LocalSeatKeys(market)};
        const auto transition{CurrentSeatTransition(market)};
        for (size_t checked{0}; checked < std::min<size_t>(sources.size(), 8) && remaining; ++checked, --remaining) {
            market.delivery_regeneration_position %= sources.size();
            const auto [hash, index]{sources[market.delivery_regeneration_position++]};
            const Kind kind{index == -2 ? Kind::CERTIFICATE : index == -1 ? Kind::PROPOSAL : Kind::ATTESTATION};
            const auto key{DeliveryKey(market.market_id, kind, hash, index < 0 ? 0 : static_cast<uint32_t>(index), std::nullopt)};
            if (m_delivery_keys.contains(key)) continue;
            flowmesh::WireMessage wire;
            wire.kind = kind;
            std::optional<std::vector<unsigned char>> payload;
            if (kind == Kind::CERTIFICATE) {
                const auto seats{market.chain->SeatSetForSequence(market.domain, market.market_id, market.next_sequence - 1)};
                std::optional<StoredProductionEntry> stored;
                std::string error;
                if (!seats || !market.store->ReadEntry(market.next_sequence - 1, *seats, stored, error) ||
                    !stored || stored->entry.GetHash() != market.last_hash) {
                    market.delivery.current_reason = "durable_retry_source_unavailable";
                    DeliveryEvent(market, "regeneration_blocked", kind, hash, market.next_sequence - 1,
                                  std::nullopt, market.delivery.current_reason);
                    continue;
                }
                if (!market.chain->StillCanonical(stored->entry.anchor)) continue;
                wire.header = HeaderFor(stored->entry);
                payload = flowmesh::EncodeProductionCertifiedPayload({stored->entry, stored->certificate}, seats->Size());
            } else {
                const auto& candidate{market.candidates.at(hash)};
                const auto& entry{candidate.entry};
                const bool handoff{entry.kind == static_cast<uint8_t>(flowmesh::ProductionEntryKind::EPOCH_HANDOFF)};
                if (!transition || market.pending_handoff || !market.chain->Acceptable(entry.anchor) ||
                    entry.sequence != market.next_sequence || entry.epoch != market.seats.epoch ||
                    (market.diagnostics.local_locked_candidate && *market.diagnostics.local_locked_candidate != hash) ||
                    (!handoff && transition->kind != FlowMeshSeatTransitionKind::CONTINUE) ||
                    (handoff && (transition->kind != FlowMeshSeatTransitionKind::HANDOFF ||
                     !transition->next_seats || transition->next_seats->set_hash != entry.next_seat_set_hash))) continue;
                const uint32_t seat{kind == Kind::PROPOSAL ? candidate.relay_proof->seat : static_cast<uint32_t>(index)};
                if (candidate.local_signers.contains(seat) && std::none_of(local_keys.begin(), local_keys.end(),
                    [&](const auto& item) { return item.first == seat; })) continue;
                wire.header = HeaderFor(entry);
                if (kind == Kind::PROPOSAL) {
                    const auto& proof{*candidate.relay_proof};
                    payload = flowmesh::EncodeProductionProposalPayload({entry, proof.round, proof.seat, proof.signature});
                } else {
                    payload = flowmesh::EncodeProductionAttestationPayload(market.attestations.at(hash).at(seat));
                }
            }
            if (!payload) continue;
            wire.payload = std::move(*payload);
            const auto bytes{wire.MemoryUsage()};
            if (!m_delivery_retry_budget.Available(now, bytes) || !market.committee_relay_budget.Available(now, bytes)) continue;
            m_delivery_retry_budget.Charge(bytes);
            market.committee_relay_budget.Charge(bytes);
            CountObservation(market.delivery.retried);
            DeliveryEvent(market, "regenerated_exact", kind, hash, wire.header.sequence);
            RelayMessage(market, std::move(wire), std::nullopt, std::nullopt);
        }
    }
}

bool FlowMeshRuntime::NotifyDeliveryEvent(const FlowMeshDeliveryEvent& event)
{
    std::lock_guard<std::mutex> lock{m_queue_mutex};
    if (!m_started || m_stopping || event.delivery_id == 0) return false;
    if (m_delivery_events.size() >= MAX_DELIVERY_EVENTS) {
        ++m_delivery_event_overflows;
        return false;
    }
    auto bounded{event};
    bounded.reason.resize(std::min<size_t>(bounded.reason.size(), 160));
    m_delivery_events.push_back(std::move(bounded));
    m_work_cv.notify_one();
    return true;
}

void FlowMeshRuntime::ProcessDeliveryEvent(const FlowMeshDeliveryEvent& event)
{
    std::lock_guard<std::mutex> lock{m_market_mutex};
    const auto it{m_deliveries.find(event.delivery_id)};
    if (it == m_deliveries.end()) return; // cancelled/replaced local identity
    auto& pending{*it->second};
    if (pending.outstanding.erase(event.peer) == 0) return;
    auto& market{*m_markets.at(pending.relay.message.header.market_id)};
    if (event.outcome == FlowMeshDeliveryOutcome::SOCKET_WRITTEN) {
        CountObservation(market.delivery.socket_written);
    } else {
        CountObservation(market.delivery.refused);
    }
    market.delivery.current_reason = event.reason.empty()
        ? FlowMeshDeliveryOutcomeName(event.outcome) : event.reason.substr(0, 160);
    DeliveryEvent(market, FlowMeshDeliveryOutcomeName(event.outcome),
                  pending.relay.message.kind, pending.object,
                  pending.relay.message.header.sequence, event.peer, market.delivery.current_reason);
    // Written is not remotely verified: retain while semantically relevant.
}

std::vector<FlowMeshRuntimeDeliverySnapshot> FlowMeshRuntime::DeliverySnapshots(
    const std::optional<flowmesh::MarketId> market_id) const
{
    // Start() takes the queue lock before the market lock; never nest them
    // in the opposite order here.
    std::map<flowmesh::MarketId, uint64_t> coalesced;
    {
        std::lock_guard<std::mutex> queue_lock{m_queue_mutex};
        for (const auto& [id, quiet] : m_agreement_quiet) coalesced.emplace(id, quiet.coalesced);
    }
    std::lock_guard<std::mutex> lock{m_market_mutex};
    std::vector<FlowMeshRuntimeDeliverySnapshot> out;
    for (const auto& [id, market] : m_markets) {
        if (market_id && *market_id != id) continue;
        out.push_back(market->delivery);
        out.back().event_queue_overflows = m_delivery_event_overflows.load();
        if (const auto count{coalesced.find(id)}; count != coalesced.end()) {
            out.back().agreement_duplicates_coalesced = count->second;
        }
        out.back().sampled_monotonic_us = TraceNow(*market->clock);
        out.back().trace_global_bytes = g_runtime_trace_bytes.load(std::memory_order_relaxed);
        out.back().trace_global_events_dropped = g_runtime_trace_dropped.load(std::memory_order_relaxed);
    }
    return out;
}

FlowMeshRuntime::FlowMeshRuntime(
    FlowMeshRuntimeConfig config,
    std::vector<FlowMeshRuntimeMarketConfig> markets)
    : m_config{std::move(config)}, m_market_configs{std::move(markets)}
{
}

FlowMeshRuntime::~FlowMeshRuntime()
{
    Stop();
}

bool FlowMeshRuntime::InitializeMarkets(std::string& error)
{
    if (m_config.chain == nullptr || m_config.keys == nullptr ||
        m_config.clock == nullptr || !m_config.relay ||
        m_config.round_timeout <= std::chrono::milliseconds{0} ||
        m_config.legacy_probe_interval < std::chrono::seconds{60}) {
        error = "FlowMesh runtime dependencies or timing policy are incomplete";
        return false;
    }
    m_markets.clear();
    for (const FlowMeshRuntimeMarketConfig& config : m_market_configs) {
        if (!InitializeMarket(config, error)) {
            m_markets.clear();
            return false;
        }
    }
    return true;
}

bool FlowMeshRuntime::InitializeMarket(
    const FlowMeshRuntimeMarketConfig& config, std::string& error)
{
    if (config.domain.IsNull() || config.market_id.IsNull() ||
        config.treasury_owner_commitment.IsNull() ||
        m_markets.count(config.market_id) != 0) {
        error = "FlowMesh runtime market configuration is malformed";
        return false;
    }
    const auto vault{
        flowmesh::ComputeFlowMeshVaultId(config.domain, config.market_id)};
    if (!vault || config.state.LedgerView().VaultCommitment() != *vault) {
        error = "FlowMesh runtime state is bound to a different vault";
        return false;
    }
    if (config.readiness ==
        FlowMeshRuntimeMarketReadiness::INSUFFICIENT_SEATS) {
        if (config.store != nullptr || !config.active_seats.members.empty() ||
            config.next_sequence != 0 || config.next_effect_index != 0 ||
            !config.last_microblock_hash.IsNull()) {
            error = "paused FlowMesh market has initialized signing state";
            return false;
        }
        m_markets.emplace(config.market_id,
                          std::make_unique<Market>(config, m_config, m_client_events));
        return true;
    }
    if (config.store == nullptr ||
        config.active_seats.market_id != config.market_id ||
        flowmesh::CheckActiveFnBlsSeatSet(config.domain,
                                          config.active_seats) !=
            flowmesh::BlsSeatSetCheck::OK) {
        error = "ready FlowMesh market has no canonical seat/store state";
        return false;
    }
    const auto canonical{m_config.chain->SeatSet(
        config.domain, config.market_id, config.active_seats.epoch,
        config.active_seats.set_hash)};
    if (!canonical || !SameSeatIdentity(*canonical, config.active_seats)) {
        error = "FlowMesh runtime active seat set is not the canonical snapshot";
        return false;
    }
    const auto active_anchor{SeatAnchor(config.active_seats)};
    if (!active_anchor || !m_config.chain->StillCanonical(*active_anchor)) {
        error = "FlowMesh runtime active seat anchor is not canonical";
        return false;
    }

    std::optional<FlowMeshProductionStore::Marker> marker;
    if (!config.store->ReadMarker(marker, error) || !marker ||
        marker->domain != config.domain ||
        marker->market_id != config.market_id ||
        marker->current_epoch != config.active_seats.epoch ||
        marker->current_seat_set_hash != config.active_seats.set_hash ||
        marker->current_anchor != *active_anchor ||
        marker->next_sequence != config.next_sequence ||
        marker->next_effect_index != config.next_effect_index ||
        marker->last_microblock_hash != config.last_microblock_hash ||
        marker->state_root != config.state.Root()) {
        if (error.empty()) {
            error = "FlowMesh runtime configuration disagrees with its store marker";
        }
        return false;
    }

    auto market{std::make_unique<Market>(config, m_config, m_client_events)};
    // Rebuild only bounded inclusion lookup hints during the existing verified
    // history replay. Never resubmit actions or scan history on status requests.
    // Publish after the WHOLE market validates: AddMarket may run on a live
    // service, and a failed initialization must not leak partial lookup events.
    std::deque<flowmesh::ClientEvent> restored_client_actions;
    uint256 previous_hash;
    uint64_t expected_effect_start{0};
    for (uint64_t sequence{0}; sequence < config.next_sequence; ++sequence) {
        const auto historical_seats{m_config.chain->SeatSetForSequence(
            config.domain, config.market_id, sequence)};
        std::optional<StoredProductionEntry> stored;
        if (!historical_seats ||
            !config.store->ReadEntry(sequence, *historical_seats, stored,
                                     error) ||
            !stored || stored->entry.sequence != sequence ||
            stored->entry.parent_hash != previous_hash ||
            stored->entry.effect_start != expected_effect_start ||
            stored->entry.effect_count >
                std::numeric_limits<uint64_t>::max() - expected_effect_start) {
            if (error.empty()) {
                error = "FlowMesh runtime cannot restore durable anchor history";
            }
            return false;
        }
        expected_effect_start += stored->entry.effect_count;
        previous_hash = stored->entry.GetHash();
        market->previous_anchor = stored->entry.anchor;
        market->committed_anchors[{stored->entry.anchor.height,
                                   stored->entry.anchor.hash}] = sequence;
        for (const auto& action : stored->entry.actions) {
            if (restored_client_actions.size() == flowmesh::CLIENT_EVENT_CAPACITY) restored_client_actions.pop_front();
            restored_client_actions.push_back(MakeClientActionEvent(*market,
                flowmesh::ClientEventKind::CERTIFIED_INCLUDED, action,
                "restored semantic inclusion only; execution outcome not established by this event", &stored->entry));
        }
        if (sequence + 1 == config.next_sequence) {
            market->client_head = flowmesh::ProductionCertifiedEnvelope{
                stored->entry, stored->certificate};
            market->client_head_seat_count = historical_seats->Size();
        }
        if (sequence + 1 == config.next_sequence &&
            stored->entry.kind == static_cast<uint8_t>(
                flowmesh::ProductionEntryKind::EPOCH_HANDOFF) &&
            marker->current_epoch == stored->entry.epoch) {
            market->pending_handoff = true;
        }
    }
    if (previous_hash != config.last_microblock_hash ||
        expected_effect_start != config.next_effect_index ||
        !RecheckAnchors(*market)) {
        error = market->error.empty()
                    ? "FlowMesh runtime durable history has the wrong head"
                    : market->error;
        return false;
    }
    Market* market_ptr{market.get()};
    market->pool.SetVerifier([market_ptr](const flowmesh::Action& action) {
        return AuthenticateAction(*market_ptr, action);
    });

    std::optional<StoredLockedProductionCandidate> retained;
    if (!config.store->ReadLockedCandidate(
            {config.active_seats.epoch, config.next_sequence}, retained,
            error)) {
        if (error.empty()) {
            error = "FlowMesh runtime cannot read its retained signing candidate";
        }
        return false;
    }
    if (retained) {
        for (const flowmesh::Action& action : retained->evidence) {
            if (!market->pool.Add(action, LOCAL_ACTION_PEER, market->clock->Now())) {
                error = "FlowMesh runtime retained action evidence no longer authenticates";
                return false;
            }
        }
        market->diagnostics.local_locked_candidate = retained->entry.GetHash();
        market->pending_candidate_restore = std::move(retained);
        // A service-start reconciliation gate is expected here. Keep the
        // market installed but paused, and retry from the first live tick or
        // message. Already-live runtimes retain immediate verification.
        if (!RestoreRetainedCandidate(*market) &&
            market->halt != FlowMeshRuntimeHalt::NONE) {
            error = market->error;
            return false;
        }
    }
    if (!InitializeAgreement(*market, config, error)) return false;
    RoundEntered(*market, "market_initialized");
    for (auto& event : restored_client_actions) m_client_events.Append(std::move(event));
    m_markets.emplace(config.market_id, std::move(market));
    return true;
}

bool FlowMeshRuntime::InitializeAgreement(Market& market,
    const FlowMeshRuntimeMarketConfig& config, std::string& error)
{
    if (config.preagreement != market.store->PreagreementEnabled()) {
        error = "FlowMesh runtime/store agreement mode mismatch";
        return false;
    }
    if (!config.preagreement) return true;
    if (config.agreement_path.empty() || config.agreement_identity.IsNull()) {
        error = "FlowMesh agreement requires an explicit durable journal identity/path";
        return false;
    }
    std::optional<FlowMeshProductionStore::Marker> marker;
    if (!market.store->ReadMarker(marker, error) || !marker ||
        marker->agreement_identity != config.agreement_identity ||
        config.agreement_allow_create == marker->agreement_bootstrap_complete) {
        error = "FlowMesh agreement bootstrap permission does not match its durable production marker";
        return false;
    }
    auto context{AgreementContext(market)};
    // A handoff append is not seat activation. Do not open a next-slot journal
    // against the outgoing roster while the B3 inclusion is still maturing.
    if (market.pending_handoff && market.client_head) {
        const auto& entry{market.client_head->entry};
        context.sequence = entry.sequence;
        context.parent_hash = entry.parent_hash;
        context.previous_state_root = entry.previous_state_root;
    }
    FlowMeshAgreementCallbacks callbacks;
    callbacks.validate_candidate = [&market](std::span<const unsigned char> entry,
        std::optional<std::span<const unsigned char>> evidence) {
        return ValidateAgreementCandidate(market, entry, evidence);
    };
    callbacks.local_keys = [&market] {
        std::vector<bls::SecretKey> keys;
        if (market.halt != FlowMeshRuntimeHalt::NONE || market.pending_handoff ||
            !market.chain->Acceptable(market.chain->Current()) || !RecheckAnchors(market)) return keys;
        const auto transition{CurrentSeatTransition(market)};
        if (!transition || transition->kind == FlowMeshSeatTransitionKind::PAUSED) return keys;
        for (const auto& [seat, key] : LocalSeatKeys(market)) { (void)seat; keys.push_back(key); }
        return keys;
    };
    callbacks.seat_set = [&market](const flowmesh::PreagreementContext& c) {
        return market.chain->SeatSet(c.domain, c.market_id, c.epoch, c.seat_set_hash);
    };
    callbacks.leader = [&market](uint32_t view) -> std::optional<uint32_t> {
        if (!market.seats.Size()) return std::nullopt;
        return flowmesh::ProductionProposerSeatIndex(market.next_sequence, view, market.seats.Size());
    };
    callbacks.publish = [this, &market](const flowmesh::AgreementMessage& message) {
        return PublishAgreement(market, message);
    };
    try {
        market.agreement = std::make_unique<FlowMeshAgreement>(
            DBParams{.path = config.agreement_path, .cache_bytes = 1 << 20}, std::move(callbacks));
        if (!market.agreement->Open(context, market.seats, config.agreement_identity,
                                    config.agreement_allow_create, error)) return false;
        if (!market.store->MarkAgreementBootstrapComplete(config.agreement_identity, error)) return false;
        market.agreement_context = context;
        market.round = market.agreement->View();
        if (market.diagnostics.local_locked_candidate &&
            market.agreement->DecidedCandidate() != market.diagnostics.local_locked_candidate) {
            error = "permanent V1 signing lock lacks its exact durable agreement decision";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        error = std::string{"FlowMesh agreement initialization failed: "} + e.what();
        return false;
    }
}

bool FlowMeshRuntime::RefreshAgreement(Market& market)
{
    if (!market.agreement) return true;
    if (market.ready) {
        // This is the worker's certified production position, never a peer's
        // advertised header. Retire known older payloads even while a handoff
        // or reconciliation pauses opening the next agreement slot.
        std::lock_guard<std::mutex> lock{m_queue_mutex};
        auto& quiet{m_agreement_quiet[market.market_id]};
        quiet.sequence = std::max(quiet.sequence, market.next_sequence);
    }
    if (market.halt != FlowMeshRuntimeHalt::NONE || market.pending_handoff ||
        !market.chain->Acceptable(market.chain->Current())) {
        SuspendAgreementTimeout(market);
        return false;
    }
    if (market.agreement->Halted()) {
        HaltMarket(market, FlowMeshRuntimeHalt::SIGNING_CONFLICT, market.agreement->LastError());
        return false;
    }
    const auto context{AgreementContext(market)};
    if (!market.agreement_context || !SameAgreementContext(*market.agreement_context, context)) {
        std::string error;
        if (!market.agreement->Advance(context, market.seats, error)) {
            HaltMarket(market, FlowMeshRuntimeHalt::SIGNING_CONFLICT, std::move(error));
            return false;
        }
        market.agreement_context = context;
        market.agreement_forwarded.clear();
        market.next_agreement_retry = {};
        ResetAgreementTimeout(market);
    }
    if (market.round != market.agreement->View()) {
        market.round = market.agreement->View();
        market.round_started = market.clock->Now();
        market.agreement_timeout_remaining = market.round_timeout;
        if (market.agreement_deadline) market.agreement_deadline = market.round_started + market.round_timeout;
        RoundEntered(market, "authenticated_agreement_view");
    }
    return true;
}

bool FlowMeshRuntime::PublishAgreement(Market& market, const flowmesh::AgreementMessage& message,
                                       const std::optional<flowmesh::WirePeerId> exclude)
{
    const auto refuse = [&](const std::string& reason) {
        CountObservation(market.delivery.refused);
        DeliveryEvent(market, "agreement_refused", flowmesh::WireMessageKind::AGREEMENT,
                      message.candidate, message.context.sequence, std::nullopt, reason);
        return false;
    };
    if (!market.agreement || !market.agreement_context ||
        !SameAgreementContext(message.context, *market.agreement_context) ||
        !SameAgreementContext(message.context, AgreementContext(market))) return refuse("stale_agreement_context");
    if (market.halt != FlowMeshRuntimeHalt::NONE || market.pending_handoff ||
        !market.chain->Acceptable(market.chain->Current())) return refuse("market_unavailable_or_reconciling");
    const auto payload{flowmesh::EncodeAgreementMessage(message)};
    if (!payload) return refuse("agreement_encoding_limit");
    const size_t bytes{payload->size() + flowmesh::FLOWMESH_WIRE_HEADER_SIZE};
    const auto now{market.clock->Now()};
    if (!market.agreement_budget.Available(now, bytes) ||
        !m_agreement_relay_budget.Available(now, bytes)) return refuse("agreement_relay_budget");
    market.agreement_budget.Charge(bytes);
    m_agreement_relay_budget.Charge(bytes);
    flowmesh::WireMessage wire;
    wire.kind = flowmesh::WireMessageKind::AGREEMENT;
    wire.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1, market.market_id,
                   message.context.epoch, message.context.sequence};
    wire.payload = *payload;
    CountObservation(market.delivery.created);
    DeliveryEvent(market, "agreement_delivery_attempt", wire.kind, message.candidate,
                  message.context.sequence, std::nullopt,
                  "stage=" + std::to_string(static_cast<unsigned>(message.stage)) +
                      " view=" + std::to_string(message.view), WireTrace(wire));
    if (!m_config.relay) return refuse("relay_unavailable");
    FlowMeshRelayResult result;
    try {
        result = m_config.relay({std::nullopt, exclude, std::move(wire)});
    } catch (const std::exception&) { return refuse("relay_callback_exception_retryable"); }
    bool admitted{false};
    for (size_t i{0}; i < std::min(result.peers.size(), MAX_DELIVERY_PEERS); ++i) {
        const auto& peer{result.peers[i]};
        const bool accepted{peer.admission == FlowMeshDeliveryAdmission::ADMITTED ||
                             peer.admission == FlowMeshDeliveryAdmission::LEGACY_UNTRACKED};
        admitted |= accepted;
        DeliveryEvent(market, accepted ? "agreement_admitted" : "agreement_refused",
                      flowmesh::WireMessageKind::AGREEMENT, message.candidate,
                      message.context.sequence, peer.peer, peer.reason.empty()
                          ? FlowMeshDeliveryAdmissionName(peer.admission) : peer.reason);
    }
    if (result.peers.empty()) return refuse(result.reason.empty()
        ? FlowMeshDeliveryAdmissionName(result.no_peer_reason) : result.reason);
    // Admission is not peer receipt. The agreement journal still owns retries.
    return admitted;
}

void FlowMeshRuntime::HandleAgreement(Market& market, flowmesh::WirePeerId peer,
                                    const flowmesh::WireMessage& wire)
{
    if (!market.agreement || !RefreshAgreement(market)) return;
    const auto message{flowmesh::DecodeAgreementMessage(wire.payload)};
    if (!message || wire.header.version != flowmesh::FLOWMESH_WIRE_VERSION_V1 ||
        wire.header.market_id != message->context.market_id ||
        wire.header.epoch != message->context.epoch ||
        wire.header.sequence != message->context.sequence) return;
    std::string error;
    if (!market.agreement->Receive(*message, error)) {
        DeliveryEvent(market, "agreement_refused", wire.kind, message->candidate,
                      message->context.sequence, peer, error);
        RefreshAgreement(market);
        return;
    }
    DeliveryEvent(market, "agreement_verified", wire.kind, message->candidate,
                  message->context.sequence, peer,
                  "stage=" + std::to_string(static_cast<unsigned>(message->stage)));
    // Authenticated remote traffic must cross a star relay too. Memory is
    // bounded; refusal leaves the sender's durable retry obligation intact.
    const auto id{Hash(wire.payload)};
    const auto now{market.clock->Now()};
    if (market.agreement_forwarded.size() >= 4096) {
        // This is a transient forwarding cooldown, not a signing journal.
        // Expired hashes must not permanently prevent new valid messages
        // from crossing a star relay while a slot is recovering.
        std::erase_if(market.agreement_forwarded,
                      [&](const auto& item) { return now >= item.second.next; });
    }
    // New bytes (including a COMMIT repeated with newly attached proof) cross
    // at once. Identical bytes were echoed by every receiver once per second
    // for the life of the slot; four operators then offered each other more
    // exact duplicates than the per-peer committee admission refills, and the
    // next view's first delivery waited behind them until its round expired.
    // Recovery forwarding is retained at a doubling interval, and the peer
    // that supplied these bytes is never sent them back.
    auto found{market.agreement_forwarded.find(id)};
    auto quiet_until{now + DELIVERY_REPEAT};
    if (found == market.agreement_forwarded.end()) {
        if (market.agreement_forwarded.size() < 4096 && PublishAgreement(market, *message, peer)) {
            market.agreement_forwarded.emplace(id, Market::AgreementForward{now + DELIVERY_REPEAT, DELIVERY_REPEAT});
        }
    } else if (now >= found->second.next) {
        if (PublishAgreement(market, *message, peer)) {
            found->second.delay = std::min<std::chrono::milliseconds>(
                found->second.delay * 2, AGREEMENT_FORWARD_REPEAT_MAX);
            found->second.next = now + found->second.delay;
        }
        quiet_until = std::max(quiet_until, found->second.next);
    } else {
        quiet_until = found->second.next;
    }
    // Receive() succeeded: the engine holds everything these bytes carry.
    // Until the next recovery forward is due, identical bytes are consumed
    // at admission instead of spending the sender's committee admission.
    QuietAgreementPayload(wire.header, id, quiet_until);
    RefreshAgreement(market);
    FinalizeAgreement(market);
}

void FlowMeshRuntime::FinalizeAgreement(Market& market)
{
    if (!market.agreement || !RefreshAgreement(market)) return;
    const auto hash{market.agreement->DecidedCandidate()};
    if (!hash) return;
    const auto bytes{market.agreement->CandidateBytes(*hash)};
    const auto blob{market.agreement->RestoreCandidateBlob(*hash)};
    if (!bytes || !blob || !ValidateAgreementCandidate(market, *bytes,
        std::span<const unsigned char>{*blob})) return;
    auto& candidate{market.candidates.at(*hash)};
    const auto local_keys{LocalSeatKeys(market)};
    if (!local_keys.empty() && !RetainCandidateBeforeSigning(market, candidate)) return;
    flowmesh::ProductionSigningGuard guard{*market.store};
    for (const auto& [seat, key] : local_keys) {
        auto& votes{market.attestations[*hash]};
        auto vote{votes.find(seat)};
        if (vote == votes.end()) {
            flowmesh::ProductionLockResult lock;
            const auto signed_vote{flowmesh::SignProductionEntryAttestation(
                key, seat, candidate.entry, market.seats, guard, lock)};
            if (!signed_vote) {
                HaltMarket(market, FlowMeshRuntimeHalt::SIGNING_CONFLICT,
                           "durable agreement decision could not acquire its unchanged V1 signing lock");
                return;
            }
            vote = votes.emplace(seat, *signed_vote).first;
            candidate.local_signers.insert(seat);
            market.attested_hash_by_seat.emplace(seat, *hash);
            DeliveryEvent(market, "attestation_signed", flowmesh::WireMessageKind::ATTESTATION,
                          *hash, market.next_sequence, LOCAL_ACTION_PEER, "after_durable_commit_decision",
                          SeatTrace(market, seat, vote->second.signature.Compressed()));
        }
        auto& last{candidate.attestation_forward_attempts[seat]};
        if (last && market.clock->Now() < *last + DELIVERY_REPEAT) continue;
        const auto payload{flowmesh::EncodeProductionAttestationPayload(vote->second)};
        if (!payload) continue;
        flowmesh::WireMessage wire{flowmesh::WireMessageKind::ATTESTATION,
                                   HeaderFor(candidate.entry), *payload};
        last = market.clock->Now();
        RelayMessage(market, std::move(wire), std::nullopt, std::nullopt);
    }
    MaybeCertify(market, *hash);
}

bool FlowMeshRuntime::Start(std::string& error)
{
    std::lock_guard<std::mutex> queue_lock{m_queue_mutex};
    if (m_started) return true;
    {
        std::lock_guard<std::mutex> market_lock{m_market_mutex};
        m_client_events.Reset(GetRandHash());
        if (!InitializeMarkets(error)) return false;
        m_admitted_markets.clear();
        m_probe_markets.clear();
        m_peer_probe_cursors.clear();
        m_evidence_retry_budget = {};
        m_evidence_retry_cursor.SetNull();
        m_delivery_regeneration_cursor.SetNull();
        m_next_delivery_regeneration = {};
        m_delivery_retry_budget.Reset();
        for (const auto& [market_id, market] : m_markets) {
            if (market->ready) {
                m_admitted_markets.insert(market_id);
                m_probe_markets.push_back(market_id);
            }
        }
    }
    m_stopping = false;
    m_started = true;
    m_worker = std::thread{&FlowMeshRuntime::WorkerLoop, this};
    return true;
}

void FlowMeshRuntime::Stop()
{
    std::deque<AddMarketCommand> abandoned;
    {
        std::lock_guard<std::mutex> lock{m_queue_mutex};
        if (!m_started) return;
        m_stopping = true;
        abandoned.swap(m_add_market_commands);
        m_work_cv.notify_all();
    }
    for (AddMarketCommand& command : abandoned) {
        command.completion->set_value(
            {false, "FlowMesh runtime stopped before adding the market"});
    }
    if (m_worker.joinable()) m_worker.join();
    {
        std::lock_guard<std::mutex> market_lock{m_market_mutex};
        while (!m_deliveries.empty()) EraseDelivery(m_deliveries.begin()->first, "stopped");
        m_deferred_messages.clear();
        m_deferred_deadlines.clear();
        m_deferred_bytes = 0;
        m_receive_recovery.clear();
        m_receive_recovery_cursor.reset();
        for (const auto& [key, request] : m_pending_catchup) {
            (void)request;
            m_catchup_tracker.Cancel(key.first, key.second);
        }
        m_pending_catchup.clear();
        m_catchup_cooldowns.clear();
        m_catchup_profiles.clear();
        for (auto& [id, market] : m_markets) {
            market->delivery.deferred_objects = 0;
            market->delivery.deferred_bytes = 0;
        }
    }
    std::lock_guard<std::mutex> lock{m_queue_mutex};
    m_delivery_events.clear();
    m_started = false;
    m_processing = false;
    m_idle_cv.notify_all();
}

flowmesh::QueueResult FlowMeshRuntime::EnqueueWireMessage(
    const flowmesh::WirePeerId peer, flowmesh::WireMessage message)
{
    if (message.kind == flowmesh::WireMessageKind::AGREEMENT &&
        (message.payload.empty() || message.payload.size() > flowmesh::FLOWMESH_AGREEMENT_MAX_BYTES)) {
        return flowmesh::QueueResult::MALFORMED;
    }
    // Hash outside the queue lock; a bounded frame is at most four MiB.
    const std::optional<uint256> agreement_payload{
        message.kind == flowmesh::WireMessageKind::AGREEMENT
            ? std::optional<uint256>{Hash(message.payload)} : std::nullopt};
    std::lock_guard<std::mutex> lock{m_queue_mutex};
    if (!m_started || m_stopping) return flowmesh::QueueResult::STOPPED;
    // Reject caller-selected market ids before BoundedWireQueue allocates a
    // per-peer/market token bucket. Otherwise a peer can evade throttling and
    // grow the bucket map indefinitely by sending valid frames for random
    // market ids. Keep this queue-owned set separate from m_markets so relay
    // callbacks between runtimes cannot create a cross-runtime lock cycle.
    if (m_admitted_markets.count(message.header.market_id) == 0) {
        return flowmesh::QueueResult::MARKET_LIMIT;
    }
    // The agreement engine already verified and retained these exact bytes.
    // Another copy can add no vote or proof, but it used to spend one of the
    // sender's eight committee admissions per second and, once refused, hold
    // the independent network's critical channel in front of the next view's
    // first proposal. Consume it here. A copy is let through again whenever
    // its recovery forward is due, and different bytes (such as a COMMIT
    // repeated with newly attached proof) are never matched.
    if (agreement_payload) {
        const auto quiet{m_agreement_quiet.find(message.header.market_id)};
        if (quiet != m_agreement_quiet.end()) {
            const auto known{quiet->second.payloads.find(*agreement_payload)};
            if (known != quiet->second.payloads.end()) {
                // A payload hash authenticates no caller-selected envelope.
                // Exact bytes under a changed version/epoch/sequence are a
                // malformed frame, not an already verified duplicate.
                if (message.header != known->second.header) return flowmesh::QueueResult::MALFORMED;
                if (known->second.header.sequence < quiet->second.sequence ||
                    m_config.clock->Now() < known->second.until) {
                    ++quiet->second.coalesced;
                    return flowmesh::QueueResult::ACCEPTED;
                }
            }
        }
    }
    const auto result{m_queue.Push(peer, std::move(message),
                                   m_config.clock->Now())};
    if (result == flowmesh::QueueResult::ACCEPTED) m_work_cv.notify_one();
    return result;
}

void FlowMeshRuntime::QuietAgreementPayload(const flowmesh::WireHeader& header,
    const uint256& payload_hash, const flowmesh::WireClock::time_point until)
{
    std::lock_guard<std::mutex> lock{m_queue_mutex};
    auto& quiet{m_agreement_quiet[header.market_id]};
    const auto [it, inserted]{quiet.payloads.insert_or_assign(payload_hash, AgreementQuiet::Payload{header, until})};
    (void)it;
    if (!inserted) return;
    quiet.order.push_back(payload_hash);
    // Forgetting the oldest hash only means its next copy is verified again.
    if (quiet.order.size() > AgreementQuiet::MAX_PAYLOADS) {
        quiet.payloads.erase(quiet.order.front());
        quiet.order.pop_front();
    }
}

void FlowMeshRuntime::FlowMeshPeerConnected(const flowmesh::WirePeerId peer)
{
    std::lock_guard<std::mutex> lock{m_queue_mutex};
    if (!m_started || m_stopping || peer == LOCAL_ACTION_PEER) return;
    if (m_discovery_peers.size() < MAX_DISCOVERY_PEERS) {
        m_discovery_peers.insert(peer);
    }
    // Coalesce concurrent handshakes. Announcements are still rate-limited
    // on the worker, so connection churn cannot create an unbounded queue.
    m_discovery_refresh = true;
    if (!m_tick_pending) m_tick_requested_us = TraceNow(*m_config.clock);
    m_tick_pending = true;
    m_work_cv.notify_one();
}

void FlowMeshRuntime::FlowMeshPeerDisconnected(
    const flowmesh::WirePeerId peer)
{
    std::lock_guard<std::mutex> lock{m_queue_mutex};
    m_queue.RemovePeer(peer);
    m_discovery_peers.erase(peer);
    std::erase_if(m_catchup_commands, [&](const CatchupCommand& command) {
        return command.peer == peer;
    });
    m_removed_peers.push_back(peer);
    m_work_cv.notify_one();
}

void FlowMeshRuntime::NotifyTick()
{
    std::lock_guard<std::mutex> lock{m_queue_mutex};
    if (!m_started || m_stopping) return;
    if (!m_tick_pending) m_tick_requested_us = TraceNow(*m_config.clock);
    m_tick_pending = true;
    m_work_cv.notify_one();
}

bool FlowMeshRuntime::RequestCatchup(
    const flowmesh::WirePeerId peer,
    const flowmesh::MarketId& market_id)
{
    std::lock_guard<std::mutex> lock{m_queue_mutex};
    if (!m_started || m_stopping || market_id.IsNull()) return false;
    if (m_catchup_commands.size() >= MAX_CATCHUP_COMMANDS) return false;
    if (std::any_of(m_catchup_commands.begin(), m_catchup_commands.end(),
                    [&](const CatchupCommand& command) {
                        return command.peer == peer && command.market_id == market_id;
                    })) return true;
    m_catchup_commands.push_back({peer, market_id});
    m_work_cv.notify_one();
    return true;
}

bool FlowMeshRuntime::AddMarket(FlowMeshRuntimeMarketConfig market,
                                std::string& error)
{
    auto completion{
        std::make_shared<std::promise<std::pair<bool, std::string>>>()};
    auto result{completion->get_future()};
    {
        std::lock_guard<std::mutex> lock{m_queue_mutex};
        if (!m_started || m_stopping) {
            error = "FlowMesh runtime is not running";
            return false;
        }
        if (m_add_market_commands.size() >= MAX_PENDING_MARKET_ADDITIONS) {
            error = "FlowMesh market-discovery queue is full";
            return false;
        }
        m_add_market_commands.push_back(
            AddMarketCommand{std::move(market), completion});
        m_work_cv.notify_one();
    }
    auto [ok, result_error]{result.get()};
    error = std::move(result_error);
    return ok;
}

std::vector<flowmesh::MarketId> FlowMeshRuntime::MarketIds() const
{
    std::lock_guard<std::mutex> lock{m_market_mutex};
    std::vector<flowmesh::MarketId> out;
    out.reserve(m_markets.size());
    for (const auto& [market_id, market] : m_markets) {
        (void)market;
        out.push_back(market_id);
    }
    return out;
}

flowmesh::QueueResult FlowMeshRuntime::SubmitLocalAction(
    const flowmesh::MarketId& market_id, const flowmesh::Action& action)
{
    const auto payload{flowmesh::EncodeProductionActionPayload(action)};
    if (!payload) return flowmesh::QueueResult::MALFORMED;
    flowmesh::WireMessage message;
    TraceContext trace;
    {
        std::lock_guard<std::mutex> lock{m_market_mutex};
        const auto it{m_markets.find(market_id)};
        if (it == m_markets.end() || !it->second->ready ||
            it->second->halt != FlowMeshRuntimeHalt::NONE) {
            return flowmesh::QueueResult::MARKET_LIMIT;
        }
        message.kind = flowmesh::WireMessageKind::ACTION;
        message.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1, market_id,
                          it->second->seats.epoch,
                          it->second->next_sequence};
        message.payload = *payload;
        trace.epoch = message.header.epoch;
        trace.seat_set_hash = it->second->seats.set_hash;
    }
    trace.wire_hash = WireTrace(message).wire_hash;
    const uint64_t sequence{message.header.sequence};
    const auto result{EnqueueWireMessage(LOCAL_ACTION_PEER, std::move(message))};
    trace.observed_monotonic_us = TraceNow(*m_config.clock);
    // Preserve submission's original lock scope. This is the observed push
    // result, not an atomic queue timestamp: worker events may precede this
    // append. Cursors order appends; monotonic_us records the observation.
    {
        std::lock_guard<std::mutex> lock{m_market_mutex};
        const auto it{m_markets.find(market_id)};
        if (it != m_markets.end()) DeliveryEvent(*it->second, result == flowmesh::QueueResult::ACCEPTED
                          ? "local_action_admitted" : "local_action_refused",
                      flowmesh::WireMessageKind::ACTION, action.Id(), sequence,
                      LOCAL_ACTION_PEER, std::to_string(static_cast<unsigned>(result)), trace);
        if (it != m_markets.end() && result == flowmesh::QueueResult::ACCEPTED) {
            // Queue observation can follow pool/certification events because
            // submission does not hold the market lock across enqueue.
            ClientActionEvent(*it->second, flowmesh::ClientEventKind::QUEUE_ADMITTED,
                              action, "bounded runtime queue only; pool admission is pending");
        }
    }
    return result;
}

std::optional<FlowMeshRuntimeMarketStatus> FlowMeshRuntime::MarketStatus(
    const flowmesh::MarketId& market_id) const
{
    std::lock_guard<std::mutex> lock{m_market_mutex};
    const auto it{m_markets.find(market_id)};
    if (it == m_markets.end()) return std::nullopt;
    const Market& market{*it->second};
    return FlowMeshRuntimeMarketStatus{
        market.seats.epoch, market.next_sequence, market.next_effect_index,
        market.round,
        market.last_hash, market.state.Root(), market.pool.Size(),
        !market.ready || LocalSeatKeys(market).empty(), market.paused,
        market.pending_handoff, market.halt, market.error};
}

std::optional<flowmesh::FlowMeshState> FlowMeshRuntime::StateSnapshot(
    const flowmesh::MarketId& market_id) const
{
    std::lock_guard<std::mutex> lock{m_market_mutex};
    const auto it{m_markets.find(market_id)};
    return it == m_markets.end()
               ? std::nullopt
               : std::optional<flowmesh::FlowMeshState>{it->second->state};
}

std::optional<flowmesh::MarketData> FlowMeshRuntime::MarketData(
    const flowmesh::MarketId& market_id,
    const std::optional<flowmesh::AccountId>& account,
    const flowmesh::MarketDataQuery& query, std::string& error) const
{
    if (query.limit == 0 || query.limit > flowmesh::MARKET_DATA_MAX_HISTORY ||
        query.curve_limit == 0 || query.curve_limit > flowmesh::MARKET_DATA_MAX_CURVES ||
        (query.curve_cursor && !query.expected_head) ||
        (query.known_head && (query.curve_cursor || query.before_sequence))) {
        error = "Invalid bounded market-data query or pagination snapshot";
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock{m_market_mutex};
    const auto it{m_markets.find(market_id)};
    if (it == m_markets.end()) {
        error = "Unknown FlowMesh market";
        return std::nullopt;
    }
    const Market& market{*it->second};
    if (query.expected_head && *query.expected_head != market.last_hash) {
        error = "FlowMesh certified head changed; restart market-data pagination";
        return std::nullopt;
    }
    flowmesh::MarketData out;
    out.domain = market.domain;
    out.market_id = market_id;
    out.base_asset_id = market.state.BaseAsset();
    out.execution_config_id = market.state.ConfigId();
    auto& snapshot{out.snapshot};
    snapshot.certified = market.next_sequence != 0;
    snapshot.next_microblock_sequence = market.next_sequence;
    snapshot.last_microblock_hash = market.last_hash;
    snapshot.state_root = market.certified_state_root;
    snapshot.epoch = market.seats.epoch;
    if (market.previous_anchor) {
        snapshot.anchor_height = market.previous_anchor->height;
        snapshot.anchor_hash = market.previous_anchor->hash;
    }
    snapshot.active_seats = market.seats.Size();
    snapshot.quorum_required = flowmesh::FlowMeshBlsThreshold(market.seats.Size());
    snapshot.running = true;
    snapshot.paused = market.paused;
    snapshot.observer_only = !market.ready || LocalSeatKeys(market).empty();
    snapshot.pending_handoff = market.pending_handoff;
    snapshot.halt = FlowMeshRuntimeHaltName(market.halt);
    snapshot.error = market.error;
    snapshot.pending_actions = market.pool.Size();
    snapshot.local_observed_at = market.local_observed_at;
    snapshot.runtime = market.diagnostics;
    snapshot.runtime.round = market.round;
    snapshot.runtime.candidate_count = market.candidates.size();
    for (const auto& [hash, attestations] : market.attestations) {
        (void)hash;
        snapshot.runtime.max_verified_attestations = std::max(
            snapshot.runtime.max_verified_attestations, attestations.size());
    }
    for (const auto& [key, pending] : m_pending_catchup) {
        (void)pending;
        snapshot.runtime.active_catchup_requests += key.second == market_id;
    }
    out.unchanged = query.known_head && *query.known_head == market.last_hash;
    if (out.unchanged || !snapshot.certified) return out;

    out.liquidity = market.state.ReadCurves(query.curve_limit, query.curve_cursor);
    if (account) {
        const auto& ledger{market.state.LedgerView()};
        out.account = flowmesh::MarketAccountData{
            *account, market.state.NextSequence(*account),
            ledger.Available(*account, out.base_asset_id),
            ledger.Reserved(*account, out.base_asset_id),
            ledger.Available(*account, modern::NativeAsset()),
            ledger.Reserved(*account, modern::NativeAsset()),
            market.state.AccountCurves(*account)};
    }
    if (market.store) {
        out.history = market.store->ReadMarketHistory(
            std::min(query.before_sequence.value_or(market.next_sequence), market.next_sequence),
            query.limit);
    }
    return out;
}

std::optional<flowmesh::ClientStateEvidence> FlowMeshRuntime::ClientSnapshot(
    const flowmesh::MarketId& market_id, std::string& error) const
{
    std::lock_guard lock{m_market_mutex};
    const auto it{m_markets.find(market_id)};
    if (it == m_markets.end() || !it->second->client_head) {
        error = "FlowMesh client snapshot has no certified head";
        return std::nullopt;
    }
    const auto& market{*it->second};
    const auto& head{*market.client_head};
    if (head.entry.GetHash() != market.last_hash ||
        head.entry.state_root != market.certified_state_root) {
        error = "FlowMesh client snapshot head is inconsistent";
        return std::nullopt;
    }
    auto state{flowmesh::EncodeClientState(market.state, error)};
    auto payload{flowmesh::EncodeProductionCertifiedPayload(head, market.client_head_seat_count)};
    if (!state || !payload) {
        if (error.empty()) error = "FlowMesh client certificate cannot be encoded";
        return std::nullopt;
    }
    return flowmesh::ClientStateEvidence{std::move(*payload), std::move(*state), m_client_events.Cursor()};
}

std::optional<std::vector<unsigned char>> FlowMeshRuntime::ClientCertifiedEntry(
    const flowmesh::MarketId& market_id, const uint64_t sequence, std::string& error) const
{
    std::lock_guard lock{m_market_mutex};
    const auto it{m_markets.find(market_id)};
    if (it == m_markets.end() || !it->second->store || sequence >= it->second->next_sequence) {
        error = "FlowMesh certified entry is unavailable";
        return std::nullopt;
    }
    const auto& market{*it->second};
    if (market.client_head && market.client_head->entry.sequence == sequence) {
        return flowmesh::EncodeProductionCertifiedPayload(*market.client_head, market.client_head_seat_count);
    }
    const auto seats{market.chain->SeatSetForSequence(market.domain, market_id, sequence)};
    std::optional<StoredProductionEntry> stored;
    if (!seats || !market.store->ReadEntry(sequence, *seats, stored, error) || !stored) {
        if (error.empty()) error = "FlowMesh certified entry authority is unavailable";
        return std::nullopt;
    }
    return flowmesh::EncodeProductionCertifiedPayload({stored->entry, stored->certificate}, seats->Size());
}

flowmesh::ClientEventPage FlowMeshRuntime::ClientEvents(
    const std::optional<flowmesh::ClientEventCursor>& after,
    const std::optional<flowmesh::MarketId>& market,
    const std::optional<flowmesh::AccountId>& account, const size_t limit) const
{
    return m_client_events.Read(after, market, account, limit);
}

std::optional<flowmesh::ClientEvent> FlowMeshRuntime::ClientActionStatus(
    const flowmesh::MarketId& market_id, const uint256& action_id) const
{
    return m_client_events.ActionStatus(market_id, action_id);
}

bool FlowMeshRuntime::WaitForIdle(const std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock{m_queue_mutex};
    return m_idle_cv.wait_for(lock, timeout, [&] {
        return m_queue.Empty() && m_removed_peers.empty() &&
               m_catchup_commands.empty() && m_add_market_commands.empty() &&
               m_delivery_events.empty() && !m_tick_pending && !m_processing;
    });
}

void FlowMeshRuntime::WorkerLoop()
{
    while (true) {
        std::optional<flowmesh::QueuedWireMessage> message;
        std::optional<flowmesh::WirePeerId> removed;
        std::optional<CatchupCommand> catchup;
        std::optional<AddMarketCommand> add_market;
        std::optional<FlowMeshDeliveryEvent> delivery;
        bool tick{false};
        uint64_t tick_requested_us{0}, dequeued_us{0};
        {
            std::unique_lock<std::mutex> lock{m_queue_mutex};
            m_work_cv.wait(lock, [&] {
                return m_stopping || !m_queue.Empty() ||
                       !m_removed_peers.empty() ||
                       !m_catchup_commands.empty() ||
                       !m_add_market_commands.empty() || !m_delivery_events.empty() || m_tick_pending;
            });
            if (m_stopping) break;
            // At most one completion alongside one normal work item. A busy
            // completion stream cannot starve incoming consensus messages.
            if (!m_delivery_events.empty()) {
                delivery = std::move(m_delivery_events.front());
                m_delivery_events.pop_front();
            }
            if (!m_removed_peers.empty()) {
                removed = m_removed_peers.front();
                m_removed_peers.pop_front();
            } else if (!m_add_market_commands.empty()) {
                add_market = std::move(m_add_market_commands.front());
                m_add_market_commands.pop_front();
            } else if (!m_catchup_commands.empty()) {
                catchup = m_catchup_commands.front();
                m_catchup_commands.pop_front();
            } else if (m_tick_pending) {
                tick = true;
                tick_requested_us = m_tick_requested_us;
                m_tick_pending = false;
            } else {
                message = m_queue.Pop();
            }
            dequeued_us = TraceNow(*m_config.clock);
            m_processing = true;
        }

        if (delivery) ProcessDeliveryEvent(*delivery);
        if (removed) {
            RemovePeerOnWorker(*removed);
        } else if (add_market) {
            ProcessAddMarketCommand(std::move(*add_market));
        } else if (catchup) {
            ProcessCatchupCommand(*catchup);
        } else if (tick) {
            ProcessTick(tick_requested_us, dequeued_us);
        } else if (message) {
            ProcessMessage(*message, dequeued_us);
        }

        {
            std::lock_guard<std::mutex> lock{m_queue_mutex};
            m_processing = false;
            if (m_queue.Empty() && m_removed_peers.empty() &&
                m_catchup_commands.empty() && m_add_market_commands.empty() &&
                m_delivery_events.empty() && !m_tick_pending) {
                m_idle_cv.notify_all();
            }
        }
    }
}

void FlowMeshRuntime::ProcessAddMarketCommand(AddMarketCommand command)
{
    bool ok{false};
    std::string error;
    {
        std::lock_guard<std::mutex> lock{m_market_mutex};
        std::unique_ptr<Market> paused;
        const auto existing{m_markets.find(command.market.market_id)};
        if (existing != m_markets.end()) {
            if (existing->second->ready ||
                command.market.readiness !=
                    FlowMeshRuntimeMarketReadiness::READY) {
                error = "FlowMesh market is already configured";
            } else {
                paused = std::move(existing->second);
                m_markets.erase(existing);
            }
        }
        if (error.empty()) {
            ok = InitializeMarket(command.market, error);
            if (!ok && paused) {
                m_markets.emplace(paused->market_id, std::move(paused));
            }
        }
        if (ok) {
            const auto old{std::find_if(
                m_market_configs.begin(), m_market_configs.end(),
                [&](const FlowMeshRuntimeMarketConfig& item) {
                    return item.market_id == command.market.market_id;
                })};
            if (old == m_market_configs.end()) {
                m_market_configs.push_back(command.market);
            } else {
                *old = command.market;
            }
            if (command.market.readiness == FlowMeshRuntimeMarketReadiness::READY) {
                m_probe_markets.push_back(command.market.market_id);
            }
        }
    }
    if (ok) {
        std::lock_guard<std::mutex> lock{m_queue_mutex};
        m_admitted_markets.insert(command.market.market_id);
        if (!m_tick_pending) m_tick_requested_us = TraceNow(*m_config.clock);
        m_tick_pending = true;
        m_work_cv.notify_one();
    }
    command.completion->set_value({ok, std::move(error)});
}

void FlowMeshRuntime::RemovePeerOnWorker(const flowmesh::WirePeerId peer)
{
    std::lock_guard<std::mutex> lock{m_market_mutex};
    std::vector<uint64_t> direct;
    for (auto& [id, delivery] : m_deliveries) {
        if (delivery->outstanding.erase(peer) != 0) {
            auto& market{*m_markets.at(delivery->relay.message.header.market_id)};
            CountObservation(market.delivery.refused);
            market.delivery.current_reason = "peer_disconnected_before_socket_completion_observed; receipt_unknown";
            DeliveryEvent(market, "peer_disconnected", delivery->relay.message.kind,
                          delivery->object, delivery->relay.message.header.sequence,
                          peer, market.delivery.current_reason);
        }
        if (delivery->relay.peer == peer) direct.push_back(id);
    }
    for (const auto id : direct) EraseDelivery(id, "peer_disconnected");
    for (auto it{m_deferred_messages.begin()}; it != m_deferred_messages.end();) {
        if (it->second.peer != peer) { ++it; continue; }
        auto& market{*m_markets.at(it->second.message.header.market_id)};
        const auto bytes{it->second.message.MemoryUsage()};
        m_deferred_bytes -= bytes;
        market.delivery.deferred_bytes -= bytes;
        --market.delivery.deferred_objects;
        CountObservation(market.delivery.receive_refused);
        DeliveryEvent(market, "receive_discarded", it->second.message.kind,
                      it->first, it->second.message.header.sequence, peer, "peer_disconnected");
        m_deferred_deadlines.erase(it->first);
        it = m_deferred_messages.erase(it);
    }
    std::erase_if(m_receive_recovery, [&](const auto& item) { return item.first == peer; });
    m_peer_probe_cursors.erase(peer);
    m_catchup_tracker.RemovePeer(peer);
    std::erase_if(m_catchup_profiles, [&](const auto& item) { return item.first.first == peer; });
    for (auto it{m_catchup_cooldowns.begin()}; it != m_catchup_cooldowns.end();) {
        if (it->first.first == peer) it = m_catchup_cooldowns.erase(it);
        else ++it;
    }
    for (auto it{m_pending_catchup.begin()};
         it != m_pending_catchup.end();) {
        if (it->first.first == peer) {
            it = m_pending_catchup.erase(it);
        } else {
            ++it;
        }
    }
}

void FlowMeshRuntime::ProcessMessage(
    const flowmesh::QueuedWireMessage& queued, const uint64_t dequeued_us)
{
    const auto lock_requested_us{TraceNow(*m_config.clock)};
    std::lock_guard<std::mutex> lock{m_market_mutex};
    const auto locked_us{TraceNow(*m_config.clock)};
    const auto it{m_markets.find(queued.message.header.market_id)};
    if (it == m_markets.end()) return;
    Market& market{*it->second};
    if (!market.ready) return;
    DeliveryEvent(market, "message_processing", queued.message.kind, {},
                  queued.message.header.sequence, queued.peer,
                  "dequeue_us=" + std::to_string(dequeued_us) +
                      " lock_request_us=" + std::to_string(lock_requested_us) +
                      " locked_us=" + std::to_string(locked_us), WireTrace(queued.message));
    const uint64_t delivery_generation{market.chain->DeliveryGeneration()};
    const bool critical{queued.message.kind == flowmesh::WireMessageKind::PROPOSAL ||
        queued.message.kind == flowmesh::WireMessageKind::ATTESTATION ||
        queued.message.kind == flowmesh::WireMessageKind::AGREEMENT ||
        queued.message.kind == flowmesh::WireMessageKind::CERTIFICATE};
    if (critical && !market.chain->Acceptable(market.chain->Current())) {
        DeferMessage(market, queued);
        return;
    }
    if (market.halt != FlowMeshRuntimeHalt::NONE &&
        queued.message.kind != flowmesh::WireMessageKind::GET) {
        return;
    }
    if (queued.message.kind != flowmesh::WireMessageKind::GET &&
        !RestoreRetainedCandidate(market)) {
        if (critical && (delivery_generation != market.chain->DeliveryGeneration() ||
            !market.chain->Acceptable(market.chain->Current()))) DeferMessage(market, queued);
        return;
    }
    market.diagnostics.last_message_observed_at = GetTime();
    switch (queued.message.kind) {
    case flowmesh::WireMessageKind::AGREEMENT:
        HandleAgreement(market, queued.peer, queued.message);
        break;
    case flowmesh::WireMessageKind::ACTION:
        HandleAction(market, queued.peer, queued.message);
        break;
    case flowmesh::WireMessageKind::PROPOSAL:
        HandleProposal(market, queued.peer, queued.message);
        break;
    case flowmesh::WireMessageKind::ATTESTATION:
        HandleAttestation(market, queued.peer, queued.message);
        break;
    case flowmesh::WireMessageKind::CERTIFICATE:
        HandleCertificate(market, queued.peer, queued.message);
        break;
    case flowmesh::WireMessageKind::GET:
        HandleGet(market, queued.peer, queued.message);
        break;
    case flowmesh::WireMessageKind::ENTRIES:
        HandleEntries(market, queued.peer, queued.message);
        break;
    case flowmesh::WireMessageKind::HELLO:
        HandleHello(market, queued.peer, queued.message);
        break;
    }
    // The gate can close after admission or even during a handler. Retrying
    // the same bytes is safe: the normal handlers revalidate/deduplicate them.
    if (critical && (delivery_generation != market.chain->DeliveryGeneration() ||
        !market.chain->Acceptable(market.chain->Current()))) DeferMessage(market, queued);
}

void FlowMeshRuntime::ProcessTick(const uint64_t requested_us, const uint64_t dequeued_us)
{
    const auto lock_requested_us{TraceNow(*m_config.clock)};
    std::lock_guard<std::mutex> lock{m_market_mutex};
    const auto locked_us{TraceNow(*m_config.clock)};
    for (auto& [id, market_ptr] : m_markets) {
        (void)id;
        Market& market{*market_ptr};
        if (!market.ready || (market.pool.Size() == 0 &&
            market.clock->Now() < market.round_started + market.round_timeout)) continue;
        auto trace{SchedulingContext(market)};
        trace.observed_monotonic_us = locked_us;
        DeliveryEvent(market, "tick_market_lock_acquired", flowmesh::WireMessageKind::PROPOSAL,
                      {}, market.next_sequence, std::nullopt,
                      "request_us=" + std::to_string(requested_us) +
                          " dequeue_us=" + std::to_string(dequeued_us) +
                          " lock_request_us=" + std::to_string(lock_requested_us) +
                          " locked_us=" + std::to_string(locked_us), trace);
    }
    bool refresh{false};
    std::vector<flowmesh::WirePeerId> peers;
    {
        std::lock_guard<std::mutex> queue_lock{m_queue_mutex};
        refresh = std::exchange(m_discovery_refresh, false);
        peers.assign(m_discovery_peers.begin(), m_discovery_peers.end());
    }
    const auto now{m_config.clock->Now()};
    for (auto it{m_pending_catchup.begin()}; it != m_pending_catchup.end();) {
        if (now >= it->second.deadline) {
            const auto key{it->first};
            ++it;
            ExpireCatchup(key.first, key.second);
        } else {
            ++it;
        }
    }
    for (auto it{m_catchup_cooldowns.begin()}; it != m_catchup_cooldowns.end();) {
        if (now >= it->second) it = m_catchup_cooldowns.erase(it);
        else ++it;
    }
    RetryDeferredMessages();
    RetryDeliveries(); // old exact objects get a fair budget before fresh scheduling
    RegenerateDeliveries();
    AnnounceMarkets(refresh);
    ProbeLegacyPeers(peers);
    for (auto& [market_id, market_ptr] : m_markets) {
        (void)market_id;
        Market& market{*market_ptr};
        market.evidence_retry_eligible = false;
        if (!RestoreRetainedCandidate(market) || !RefreshMarker(market) || !RefreshAgreement(market)) {
            SuspendAgreementTimeout(market);
            continue;
        }
        const auto now{market.clock->Now()};
        if (market.agreement) {
            std::string error;
            const auto transition{CurrentSeatTransition(market)};
            const bool has_work{market.next_sequence == 0 || market.pool.Size() != 0 ||
                                !market.candidates.empty() ||
                                (transition && transition->kind == FlowMeshSeatTransitionKind::HANDOFF)};
            if (!has_work || market.agreement->DecidedCandidate()) {
                ResetAgreementTimeout(market);
            } else if (!transition || transition->kind == FlowMeshSeatTransitionKind::PAUSED ||
                       LocalSeatKeys(market).empty()) {
                SuspendAgreementTimeout(market);
            } else if (!market.agreement_deadline) {
                market.agreement_deadline = now + market.agreement_timeout_remaining;
                market.round_started = *market.agreement_deadline - market.round_timeout;
            }
            if (market.agreement_deadline && now >= *market.agreement_deadline) {
                if (!market.agreement->Timeout(error)) {
                    RefreshAgreement(market);
                    continue;
                }
                RefreshAgreement(market);
            }
            if (now >= market.next_agreement_retry) {
                market.next_agreement_retry = now + DELIVERY_REPEAT;
                if (!market.agreement->Retry(error)) {
                    RefreshAgreement(market);
                    continue;
                }
                RefreshAgreement(market);
            }
            FinalizeAgreement(market);
            MaybePropose(market);
            continue;
        }
        if (!market.pending_handoff &&
            now >= market.round_started + market.round_timeout) {
            if (market.round == std::numeric_limits<uint32_t>::max()) {
                HaltMarket(market, FlowMeshRuntimeHalt::SIGNING_CONFLICT,
                           "FlowMesh proposer round space is exhausted");
                continue;
            }
            TraceContext trace;
            trace.round = market.round;
            const auto elapsed{std::chrono::duration_cast<std::chrono::microseconds>(now - market.round_started).count()};
            DeliveryEvent(market, "round_timeout", flowmesh::WireMessageKind::PROPOSAL,
                          {}, market.next_sequence, std::nullopt,
                          "elapsed_us=" + std::to_string(elapsed) +
                              " timeout_ms=" + std::to_string(market.round_timeout.count()) +
                              RoundTiming(market), trace);
            ++market.round;
            market.round_started = now;
            RoundEntered(market, "local_timeout");
        }
        MaybePropose(market);
    }
    RetryRetainedEvidence();
    RetryLocalActions();
    ForwardPendingDuplicateActions();
}

void FlowMeshRuntime::RetryLocalActions()
{
    if (m_markets.empty()) return;
    const auto now{m_config.clock->Now()};
    auto it{m_markets.upper_bound(m_local_action_relay_cursor)};
    size_t actions_scanned{0};
    for (size_t markets_scanned{0};
         markets_scanned < std::min(m_markets.size(), FlowMeshLocalActionRelayBudget::MAX_MARKETS_SCANNED) &&
         actions_scanned < FlowMeshLocalActionRelayBudget::MAX_ACTIONS_SCANNED;
         ++markets_scanned) {
        // Leave the next market first in line if this batch is exhausted.
        if (!m_local_action_relay_budget.Available(
                now, flowmesh::FLOWMESH_WIRE_HEADER_SIZE + 1)) return;
        if (it == m_markets.end()) it = m_markets.begin();
        Market& market{*it->second};
        m_local_action_relay_cursor = it->first;
        ++it;
        if (!market.ready || market.halt != FlowMeshRuntimeHalt::NONE ||
            market.pending_handoff || market.pending_candidate_restore) continue;
        // An active locked candidate already has a dedicated evidence retry.
        // Its unselected local tail becomes eligible after certification; do
        // not spend two retry budgets on the same retained candidate.
        if (market.evidence_retry) continue;
        const auto transition{CurrentSeatTransition(market)};
        const auto anchor{market.chain->Current()};
        if (!transition || transition->kind != FlowMeshSeatTransitionKind::CONTINUE ||
            !market.chain->Acceptable(anchor)) continue;
        const flowmesh::WireHeader header{flowmesh::FLOWMESH_WIRE_VERSION_V1,
            market.market_id, market.seats.epoch, market.next_sequence};
        while (actions_scanned < FlowMeshLocalActionRelayBudget::MAX_ACTIONS_SCANNED) {
            if (!m_local_action_relay_budget.Available(
                    now, flowmesh::FLOWMESH_WIRE_HEADER_SIZE + 1)) return;
            if (!market.local_action_relay_budget.Available(
                    now, flowmesh::FLOWMESH_WIRE_HEADER_SIZE + 1)) break;
            auto message{market.pool.PrepareLocalRetry(
                header, market.state, market.deposits, anchor, market.clock->Now(), actions_scanned)};
            if (!message) break;
            const size_t bytes{flowmesh::FLOWMESH_WIRE_HEADER_SIZE + message->payload.size()};
            // A due front remains queued if its complete frame does not fit.
            if (!m_local_action_relay_budget.Available(now, bytes)) return;
            if (!market.local_action_relay_budget.Available(now, bytes)) break;
            market.pool.CompleteLocalRetry(market.clock->Now());
            m_local_action_relay_budget.Charge(bytes);
            market.local_action_relay_budget.Charge(bytes);
            // Charge before the external callback, even if a simultaneous
            // service reconciliation or unavailable peer suppresses delivery.
            RelayMessage(market, std::move(*message), std::nullopt, std::nullopt);
        }
    }
}

void FlowMeshRuntime::ForwardPendingDuplicateActions()
{
    if (m_markets.empty()) return;
    const auto now{m_config.clock->Now()};
    auto it{m_markets.upper_bound(m_duplicate_action_relay_cursor)};
    size_t actions_scanned{0};
    for (size_t markets_scanned{0};
         markets_scanned < std::min(m_markets.size(), FlowMeshDuplicateActionRelayBudget::MAX_MARKETS_SCANNED) &&
         actions_scanned < FlowMeshDuplicateActionRelayBudget::MAX_ACTIONS_SCANNED;
         ++markets_scanned) {
        // Do not move past the next market when the previous one consumed
        // the entire global batch: it must lead the next batch fairly.
        if (!m_duplicate_action_relay_budget.Available(
                now, flowmesh::FLOWMESH_WIRE_HEADER_SIZE + 1)) return;
        if (it == m_markets.end()) it = m_markets.begin();
        Market& market{*it->second};
        m_duplicate_action_relay_cursor = it->first;
        ++it;
        if (!market.ready || market.halt != FlowMeshRuntimeHalt::NONE || market.pending_handoff) {
            market.pool.ClearDuplicateForwards();
            continue;
        }
        const auto transition{CurrentSeatTransition(market)};
        if (!transition || transition->kind != FlowMeshSeatTransitionKind::CONTINUE) {
            market.pool.ClearDuplicateForwards();
            continue;
        }
        const flowmesh::WireHeader expected{flowmesh::FLOWMESH_WIRE_VERSION_V1,
            market.market_id, market.seats.epoch, market.next_sequence};
        while (market.pool.HasPendingForwards() &&
               actions_scanned < FlowMeshDuplicateActionRelayBudget::MAX_ACTIONS_SCANNED) {
            ++actions_scanned;
            flowmesh::WirePeerId exclude_peer;
            auto message{market.pool.PrepareDuplicateForward(expected, market.state, exclude_peer)};
            if (!message) continue;
            const size_t bytes{flowmesh::FLOWMESH_WIRE_HEADER_SIZE + message->payload.size()};
            // Preserve FIFO position on exhausted budget; a later tick
            // completes this already received obligation without re-enqueue.
            if (!m_duplicate_action_relay_budget.Available(now, bytes)) return;
            if (!market.duplicate_action_relay_budget.Available(now, bytes)) break;
            market.pool.CompleteDuplicateForward(now);
            m_duplicate_action_relay_budget.Charge(bytes);
            market.duplicate_action_relay_budget.Charge(bytes);
            RelayMessage(market, std::move(*message), std::nullopt, exclude_peer);
        }
    }
}

void FlowMeshRuntime::RetryRetainedEvidence()
{
    const auto now{m_config.clock->Now()};
    if (m_markets.empty() || !m_evidence_retry_budget.Begin(now)) return;
    auto it{m_markets.upper_bound(m_evidence_retry_cursor)};
    for (size_t scanned{0};
         scanned < std::min(m_markets.size(), FlowMeshEvidenceRetryBudget::MAX_MARKETS_SCANNED) &&
         !m_evidence_retry_budget.Full(); ++scanned) {
        if (it == m_markets.end()) it = m_markets.begin();
        Market& market{*it->second};
        m_evidence_retry_cursor = it->first;
        ++it;
        if (!market.evidence_retry_eligible || !market.evidence_retry ||
            market.halt != FlowMeshRuntimeHalt::NONE || market.pending_handoff ||
            now < market.evidence_retry->next_sweep) continue;
        auto& retry{*market.evidence_retry};
        const auto found{market.candidates.find(retry.candidate_hash)};
        if (found == market.candidates.end()) continue;
        const auto& candidate{found->second};
        if (candidate.entry.epoch != market.seats.epoch ||
            candidate.entry.sequence != market.next_sequence) continue;
        while (retry.cursor < candidate.evidence.size() && !m_evidence_retry_budget.Full()) {
            const auto payload{flowmesh::EncodeProductionActionPayload(candidate.evidence[retry.cursor])};
            if (!payload) {
                HaltMarket(market, FlowMeshRuntimeHalt::STORE_FAILURE,
                           "retained FlowMesh action evidence is not encodable");
                break;
            }
            if (!m_evidence_retry_budget.Consume(flowmesh::FLOWMESH_WIRE_HEADER_SIZE + payload->size())) break;
            flowmesh::WireMessage evidence;
            evidence.kind = flowmesh::WireMessageKind::ACTION;
            evidence.header = HeaderFor(candidate.entry);
            evidence.payload = *payload;
            // Charge attempted delivery even when service reconciliation
            // suppresses relay. Only a later paced sweep may retry it.
            RelayMessage(market, std::move(evidence), std::nullopt, std::nullopt);
            ++retry.cursor;
        }
        if (retry.cursor == candidate.evidence.size()) {
            retry.cursor = 0;
            retry.next_sweep = now + FlowMeshEvidenceRetryBudget::SWEEP_DELAY;
        }
    }
}

void FlowMeshRuntime::AnnounceMarkets(const bool refresh)
{
    if (m_markets.empty()) return;
    const auto now{m_config.clock->Now()};
    if (refresh) {
        for (auto& [id, market] : m_markets) {
            (void)id;
            market->next_announcement = std::min(
                market->next_announcement,
                market->last_announcement
                    ? *market->last_announcement + MARKET_ANNOUNCEMENT_MIN_INTERVAL
                    : now);
        }
    }
    if (now < m_next_announcement_batch) return;
    // Rotate the starting market so a large registry cannot starve its tail.
    auto it{m_markets.upper_bound(m_announcement_cursor)};
    size_t sent{0};
    for (size_t scanned{0}; scanned < m_markets.size() &&
                           sent < MAX_ANNOUNCEMENTS_PER_TICK; ++scanned) {
        if (it == m_markets.end()) it = m_markets.begin();
        Market& market{*it->second};
        m_announcement_cursor = it->first;
        ++it;
        if (!market.ready || market.halt != FlowMeshRuntimeHalt::NONE ||
            now < market.next_announcement) continue;
        flowmesh::WireMessage hello;
        hello.kind = flowmesh::WireMessageKind::HELLO;
        hello.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1, market.market_id,
                        market.seats.epoch, market.next_sequence};
        hello.payload = flowmesh::EncodeMarketHello({market.domain, market.last_hash});
        RelayMessage(market, std::move(hello), std::nullopt, std::nullopt);
        market.last_announcement = now;
        market.next_announcement = now + MARKET_ANNOUNCEMENT_INTERVAL;
        ++sent;
    }
    if (sent != 0) m_next_announcement_batch = now + std::chrono::milliseconds{250};
}

void FlowMeshRuntime::ProcessCatchupCommand(
    const CatchupCommand& command)
{
    std::lock_guard<std::mutex> lock{m_market_mutex};
    const auto it{m_markets.find(command.market_id)};
    if (it == m_markets.end()) return;
    TryRequestCatchup(*it->second, command.peer);
}

void FlowMeshRuntime::ProbeLegacyPeers(const std::vector<flowmesh::WirePeerId>& peers)
{
    for (const auto peer : peers) m_peer_probe_cursors.try_emplace(peer);
    const auto now{m_config.clock->Now()};
    if (m_peer_probe_cursors.empty() || m_probe_markets.empty() ||
        now < m_next_legacy_probe) return;
    auto it{m_peer_probe_cursors.upper_bound(m_probe_peer_cursor)};
    for (size_t scanned{0}; scanned < std::min(size_t{8}, m_peer_probe_cursors.size()); ++scanned) {
        if (it == m_peer_probe_cursors.end()) it = m_peer_probe_cursors.begin();
        const auto peer{it->first};
        LegacyPeerProbe& probe{it->second};
        ++it;
        m_probe_peer_cursor = peer;
        if (probe.market_cursor >= m_probe_markets.size()) {
            if (now < probe.next_sweep) continue;
            probe.market_cursor = 0;
        }
        const auto advance = [&] {
            ++probe.market_cursor;
            if (probe.market_cursor >= m_probe_markets.size()) {
                probe.next_sweep = now + m_config.legacy_probe_interval;
            }
        };
        const auto market{m_markets.find(m_probe_markets[probe.market_cursor])};
        if (market == m_markets.end() || !market->second->ready ||
            market->second->halt != FlowMeshRuntimeHalt::NONE) {
            advance();
            continue;
        }
        // Do not spend a probe while local reconciliation suppresses relay.
        // Remote reconciliation can also silently discard a request, so a
        // completed sweep earns another bounded attempt after the backoff.
        // Neither the retry nor a peer hint waives certificate verification.
        if (!market->second->chain->Acceptable(market->second->chain->Current())) continue;
        const auto key{std::make_pair(peer, market->first)};
        if (m_pending_catchup.contains(key) || m_catchup_cooldowns.contains(key)) {
            advance(); // a hint/proposal already initiated this discovery
            continue;
        }
        if (TryRequestCatchup(*market->second, peer)) {
            advance();
            // At most one blind compatibility probe per second globally.
            m_next_legacy_probe = now + std::chrono::seconds{1};
            return;
        }
    }
}

void FlowMeshRuntime::ExpireCatchup(const flowmesh::WirePeerId peer,
                                  const flowmesh::MarketId& market_id)
{
    const auto key{std::make_pair(peer, market_id)};
    const auto pending{m_pending_catchup.find(key)};
    if (pending == m_pending_catchup.end()) return;
    const auto now{m_config.clock->Now()};
    if (now < pending->second.deadline) return;
    const auto request{pending->second};
    const uint16_t next_count{static_cast<uint16_t>(
        request.max_entries > 8 ? request.max_entries / 2 : 1)};
    const auto profile{m_catchup_profiles.find(key)};
    if (profile != m_catchup_profiles.end()) {
        profile->second.max_entries = next_count;
        profile->second.last_used = now;
    }
    const auto market{m_markets.find(market_id)};
    if (market != m_markets.end()) {
        CountObservation(market->second->delivery.catchup_timeouts);
        const auto elapsed{std::chrono::duration_cast<std::chrono::milliseconds>(now - request.started).count()};
        const auto reason{CatchupDetails(request.max_entries, request.max_bytes, 0, 0, elapsed) +
                          " next_count=" + std::to_string(next_count)};
        market->second->delivery.current_reason = "catchup_timeout; " + reason;
        DeliveryEvent(*market->second, "catchup_timeout", flowmesh::WireMessageKind::GET,
                      market->second->last_hash, request.from_sequence, peer, reason);
    }
    m_catchup_tracker.Cancel(peer, market_id);
    m_pending_catchup.erase(pending);
}

bool FlowMeshRuntime::TryRequestCatchup(Market& market, const flowmesh::WirePeerId peer)
{
    if (!market.ready || market.halt != FlowMeshRuntimeHalt::NONE) return false;
    const auto key{std::make_pair(peer, market.market_id)};
    if (m_pending_catchup.count(key) != 0) return false;
    const auto now{m_config.clock->Now()};
    const auto cooldown{m_catchup_cooldowns.find(key)};
    if (cooldown != m_catchup_cooldowns.end() && now < cooldown->second) return false;
    if (m_pending_catchup.size() >= MAX_ACTIVE_CATCHUPS ||
        (cooldown == m_catchup_cooldowns.end() &&
         m_catchup_cooldowns.size() >= MAX_CATCHUP_COOLDOWNS)) return false;
    size_t peer_requests{0};
    size_t market_requests{0};
    for (const auto& [pending_key, request] : m_pending_catchup) {
        (void)request;
        peer_requests += pending_key.first == peer;
        market_requests += pending_key.second == market.market_id;
    }
    if (peer_requests >= 2 || market_requests >= 2) return false;
    auto profile{m_catchup_profiles.find(key)};
    if (profile == m_catchup_profiles.end()) {
        if (m_catchup_profiles.size() >= MAX_CATCHUP_PROFILES) {
            auto oldest{m_catchup_profiles.end()};
            for (auto it{m_catchup_profiles.begin()}; it != m_catchup_profiles.end(); ++it) {
                if (m_pending_catchup.contains(it->first)) continue;
                if (oldest == m_catchup_profiles.end() ||
                    it->second.last_used < oldest->second.last_used) oldest = it;
            }
            if (oldest == m_catchup_profiles.end()) return false;
            m_catchup_profiles.erase(oldest);
        }
        profile = m_catchup_profiles.emplace(key, CatchupProfile{}).first;
    }
    profile->second.last_used = now;
    const uint16_t maximum_entries{profile->second.max_entries};
    constexpr uint32_t MAX_BYTES{
        static_cast<uint32_t>(flowmesh::FLOWMESH_CATCHUP_MAX_BYTES)};
    const auto payload{flowmesh::EncodeCatchupRequest(maximum_entries, MAX_BYTES)};
    if (!payload) return false;
    if (!m_catchup_tracker.Begin(peer, market.market_id,
                                 market.next_sequence, maximum_entries,
                                 MAX_BYTES)) {
        return false;
    }
    m_pending_catchup.emplace(
        key, PendingCatchup{market.seats.epoch, market.next_sequence,
                            maximum_entries, MAX_BYTES, now, now + CATCHUP_TIMEOUT});
    CountObservation(market.delivery.catchup_started);
    DeliveryEvent(market, "catchup_started", flowmesh::WireMessageKind::GET,
                  market.last_hash, market.next_sequence, peer,
                  CatchupDetails(maximum_entries, MAX_BYTES, 0, 0, 0));
    m_catchup_cooldowns.insert_or_assign(key, now + CATCHUP_FAILURE_COOLDOWN);
    flowmesh::WireMessage request;
    request.kind = flowmesh::WireMessageKind::GET;
    request.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1,
                      market.market_id, market.seats.epoch,
                      market.next_sequence};
    request.payload = *payload;
    RelayMessage(market, std::move(request), peer, std::nullopt);
    return true;
}

void FlowMeshRuntime::HandleHello(
    Market& market, const flowmesh::WirePeerId peer,
    const flowmesh::WireMessage& message)
{
    const auto hello{flowmesh::DecodeMarketHello(message.payload)};
    if (!hello || hello->domain != market.domain || peer == LOCAL_ACTION_PEER ||
        (message.header.sequence == 0) != hello->last_microblock_hash.IsNull()) return;
    // A hint neither changes the active committee nor establishes finality.
    // Only the existing certificate-verification/re-execution path can move
    // our state. Do not pause signing or report a trusted remote tip here.
    if (message.header.sequence > market.next_sequence) {
        RequestCatchup(peer, market.market_id);
    } else if (message.header.sequence < market.next_sequence) {
        // Help a newly connected/discovered market even when production has
        // stopped. Responses are coalesced into the bounded announcement tick.
        const auto now{market.clock->Now()};
        market.next_announcement = std::min(
            market.next_announcement,
            market.last_announcement
                ? *market.last_announcement + MARKET_ANNOUNCEMENT_MIN_INTERVAL
                : now);
        NotifyTick();
    }
}

void FlowMeshRuntime::HandleAction(
    Market& market, const flowmesh::WirePeerId peer,
    const flowmesh::WireMessage& message)
{
    if (message.header.version != flowmesh::FLOWMESH_WIRE_VERSION_V1 ||
        message.header.market_id != market.market_id ||
        message.header.epoch != market.seats.epoch ||
        message.header.sequence != market.next_sequence) {
        return;
    }
    const auto action{flowmesh::DecodeProductionActionPayload(message.payload)};
    if (!action) return;
    if (!market.pool.Add(*action, peer, market.clock->Now())) {
        const bool already_admitted{market.pool.ContainsExact(*action)};
        ClientActionEvent(market, already_admitted ? flowmesh::ClientEventKind::POOL_ADMITTED
                                                   : flowmesh::ClientEventKind::POOL_REFUSED,
                          *action, already_admitted ? "exact signed action already present in pool"
                              : "pool refused this attempt (authentication, conflicting sequence or capacity); not an execution result");
        if (peer != LOCAL_ACTION_PEER) {
            market.pool.QueueDuplicateForward(*action, message, peer,
                                               market.state, market.clock->Now());
        }
        return;
    }
    CountObservation(market.delivery.verified);
    ClientActionEvent(market, flowmesh::ClientEventKind::POOL_ADMITTED, *action,
                      "authenticated action added to pool; not yet certified");
    DeliveryEvent(market, "action_verified", flowmesh::WireMessageKind::ACTION,
                  action->Id(), market.next_sequence, peer);
    DeliveryEvent(market, "action_round_observed", flowmesh::WireMessageKind::ACTION,
                  action->Id(), market.next_sequence, peer,
                  "pool_admission" + RoundTiming(market), SchedulingContext(market));
    RelayMessage(market, message, std::nullopt,
                 peer == LOCAL_ACTION_PEER
                     ? std::nullopt
                     : std::optional<flowmesh::WirePeerId>{peer});
}

void FlowMeshRuntime::MaybePropose(Market& market)
{
    if (market.halt != FlowMeshRuntimeHalt::NONE || !market.ready ||
        market.pending_handoff || !RecheckAnchors(market)) {
        ProposalWait(market, market.pending_handoff ? "pending_handoff" : "market_not_ready");
        return;
    }
    if (market.agreement) {
        if (!RefreshAgreement(market)) return;
        if (market.agreement->DecidedCandidate()) {
            FinalizeAgreement(market);
            return;
        }
    }
    const auto transition{CurrentSeatTransition(market)};
    if (!transition ||
        transition->kind == FlowMeshSeatTransitionKind::PAUSED) {
        ProposalWait(market, transition && !transition->pause_reason.empty()
                                 ? transition->pause_reason.c_str() : "seat_transition_paused",
                     /*include_idle=*/true);
        return;
    }
    const auto local_keys{LocalSeatKeys(market)};
    const uint32_t proposer_index{flowmesh::ProductionProposerSeatIndex(
        market.next_sequence, market.round, market.seats.Size())};
    const auto proposer{std::find_if(
        local_keys.begin(), local_keys.end(), [&](const auto& item) {
            return item.first == proposer_index;
        })};
    if (proposer == local_keys.end()) {
        ProposalWait(market, "proposer_not_local");
        return;
    }

    std::optional<uint256> locked_hash;
    std::string error;
    if (!market.store->ReadLock(
            flowmesh::ProductionSignPosition{market.seats.epoch,
                                             market.next_sequence},
            locked_hash, error)) {
        HaltMarket(market, FlowMeshRuntimeHalt::STORE_FAILURE,
                   error.empty() ? "FlowMesh signing journal is unreadable"
                                 : std::move(error));
        return;
    }

    if (market.agreement && !locked_hash) {
        // Reuse exact, independently revalidated candidates. A changing B3 tip
        // must not manufacture a different proposal on each refresh.
        if (const auto required{market.agreement->RequiredCandidateHash()}) {
            const auto bytes{market.agreement->CandidateBytes(*required)};
            const auto blob{market.agreement->RestoreCandidateBlob(*required)};
            if (!bytes || !blob || !ValidateAgreementCandidate(market, *bytes,
                std::span<const unsigned char>{*blob})) return;
            locked_hash = required;
        } else if (!market.candidates.empty()) {
            locked_hash = market.candidates.begin()->first;
        }
    }
    Market::Candidate* candidate{nullptr};
    if (locked_hash) {
        const auto it{market.candidates.find(*locked_hash)};
        if (it == market.candidates.end()) {
            ProposalWait(market, "locked_candidate_unavailable");
            return;
        }
        candidate = &it->second;
    } else {
        // Each dynamic market pins one deterministic epoch-zero seat anchor.
        // Its first production entry must use that same anchor so the first
        // type-8 checkpoint has one canonical bootstrap snapshot even when
        // user activity begins later. Subsequent entries advance normally.
        const std::optional<flowmesh::AnchorRef> bootstrap_anchor{
            market.next_sequence == 0 ? SeatAnchor(market.seats)
                                      : std::nullopt};
        flowmesh::AnchorRef anchor{
            bootstrap_anchor ? *bootstrap_anchor : market.chain->Current()};
        if (!bootstrap_anchor && market.deposits != nullptr) {
            const auto settlement_plan{
                market.deposits->PlanWithdrawalSettlements(
                    market.previous_anchor, anchor)};
            if (!settlement_plan) {
                ProposalWait(market, "settlement_plan_unavailable");
                return;
            }
            anchor = settlement_plan->anchor;
        }
        if (!market.chain->Acceptable(anchor)) {
            ProposalWait(market, "anchor_unavailable");
            return;
        }

        std::optional<flowmesh::ProductionEntryCore> entry;
        std::optional<flowmesh::ActiveFnBlsSeatSet> next_seats;
        flowmesh::ProductionEntryCheck check;
        if (transition->kind == FlowMeshSeatTransitionKind::HANDOFF) {
            const auto& next{*transition->next_seats};
            entry = flowmesh::BuildProductionHandoffEntry(
                market.state, market.domain, market.market_id, market.seats,
                next, market.next_sequence, market.next_effect_index,
                market.last_hash, anchor, AnchorContext(market), check);
            next_seats = next;
        } else {
            // Sequence zero is an explicit empty genesis execution. It is
            // certified and checkpointed before user effects are admitted.
            flowmesh::ProductionEpochGate gate{market.domain,
                                               market.market_id,
                                               market.seats};
            std::vector<flowmesh::Action> selected_actions{
                market.next_sequence == 0
                    ? std::vector<flowmesh::Action>{}
                    : market.pool.Select(
                          market.state,
                          flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_ACTIONS)};
            TraceContext build_trace;
            build_trace.round = market.round;
            DeliveryEvent(market, "execution_started", flowmesh::WireMessageKind::PROPOSAL,
                          {}, market.next_sequence, std::nullopt, "build_proposal", build_trace);
            auto built{flowmesh::BuildProductionExecutionEntry(
                market.state, market.domain, market.market_id, market.seats,
                gate, market.next_sequence, market.next_effect_index,
                market.last_hash, anchor, AnchorContext(market),
                market.treasury_owner_commitment,
                selected_actions,
                market.deposits, check)};
            DeliveryEvent(market, built ? "execution_completed" : "execution_failed",
                          flowmesh::WireMessageKind::PROPOSAL,
                          built ? built->entry.GetHash() : uint256{}, market.next_sequence,
                          std::nullopt, flowmesh::ProductionEntryCheckName(check), build_trace);
            // A connected type-9 withdrawal always wins the next slot. The
            // engine reports the collision with queued user actions; retry
            // once with the required dedicated empty-action settlement.
            if (!built && !selected_actions.empty() &&
                check == flowmesh::ProductionEntryCheck::CHAIN_SETTLEMENT_MISMATCH) {
                selected_actions.clear();
                DeliveryEvent(market, "execution_started", flowmesh::WireMessageKind::PROPOSAL,
                              {}, market.next_sequence, std::nullopt, "build_settlement", build_trace);
                built = flowmesh::BuildProductionExecutionEntry(
                    market.state, market.domain, market.market_id,
                    market.seats, gate, market.next_sequence,
                    market.next_effect_index, market.last_hash, anchor,
                    AnchorContext(market), market.treasury_owner_commitment,
                    selected_actions, market.deposits, check);
                DeliveryEvent(market, built ? "execution_completed" : "execution_failed",
                              flowmesh::WireMessageKind::PROPOSAL,
                              built ? built->entry.GetHash() : uint256{}, market.next_sequence,
                              std::nullopt, flowmesh::ProductionEntryCheckName(check), build_trace);
            }
            // Do not manufacture empty traffic. Sequence zero and a nonempty
            // chain-derived settlement are the only actionless executions.
            if (built && market.next_sequence != 0 &&
                built->entry.actions.empty() && built->settlements.empty()) {
                return;
            }
            if (built) {
                entry = built->entry;
                TraceActions(market, "action_selected", *entry);
                const uint256 hash{entry->GetHash()};
                auto [it, inserted]{market.candidates.emplace(
                    hash, Market::Candidate{*entry, built->next_state,
                                            std::nullopt,
                                            std::move(selected_actions)})};
                (void)inserted;
                candidate = &it->second;
            }
        }
        if (!entry) return;
        if (next_seats) {
            const uint256 hash{entry->GetHash()};
            auto [it, inserted]{market.candidates.emplace(
                hash, Market::Candidate{*entry, market.state, *next_seats,
                                        {}})};
            (void)inserted;
            candidate = &it->second;
        }
    }
    if (candidate == nullptr) return;

    if (market.agreement) {
        const auto bytes{flowmesh::EncodeProductionEntry(candidate->entry)};
        if (!bytes) return;
        std::string agreement_error;
        if (!market.agreement->SubmitCandidate(*bytes, agreement_error)) {
            DeliveryEvent(market, "agreement_candidate_wait", flowmesh::WireMessageKind::AGREEMENT,
                          candidate->entry.GetHash(), market.next_sequence, std::nullopt, agreement_error);
        }
        RefreshAgreement(market);
        FinalizeAgreement(market);
        return;
    }

    if (!RetainCandidateBeforeSigning(market, *candidate)) return;

    // Eligibility is sampled only through the normal proposer gates on this
    // tick. The separate bounded scheduler never chooses/signs a candidate.
    market.evidence_retry_eligible =
        transition->kind == FlowMeshSeatTransitionKind::CONTINUE &&
        market.chain->Acceptable(candidate->entry.anchor);

    flowmesh::ProductionSigningGuard guard{*market.store};
    flowmesh::ProductionProposalCheck check;
    TraceContext sign_trace;
    sign_trace.round = market.round;
    sign_trace.seat_index = proposer_index;
    DeliveryEvent(market, "proposal_signing_started", flowmesh::WireMessageKind::PROPOSAL,
                  candidate->entry.GetHash(), candidate->entry.sequence, LOCAL_ACTION_PEER, {}, sign_trace);
    const auto proposal{flowmesh::SignProductionProposal(
        proposer->second, candidate->entry, market.round, market.seats,
        guard, check)};
    if (!proposal) {
        DeliveryEvent(market, "proposal_signing_failed", flowmesh::WireMessageKind::PROPOSAL,
                      candidate->entry.GetHash(), candidate->entry.sequence, LOCAL_ACTION_PEER,
                      "proposal_check=" + std::to_string(static_cast<unsigned>(check)), sign_trace);
        if (check == flowmesh::ProductionProposalCheck::LOCK_CONFLICT ||
            check == flowmesh::ProductionProposalCheck::LOCK_STORAGE_FAILURE) {
            HaltMarket(market, FlowMeshRuntimeHalt::SIGNING_CONFLICT,
                       check == flowmesh::ProductionProposalCheck::LOCK_CONFLICT
                           ? "permanent FlowMesh proposal lock conflicts"
                           : "permanent FlowMesh proposal lock failed");
        }
        return;
    }
    DeliveryEvent(market, "proposal_signed", flowmesh::WireMessageKind::PROPOSAL,
                  candidate->entry.GetHash(), candidate->entry.sequence, LOCAL_ACTION_PEER, {},
                  SeatTrace(market, proposal->proposer_seat_index, proposal->proposer_signature, proposal->round));
    const auto payload{flowmesh::EncodeProductionProposalPayload(*proposal)};
    if (!payload) return;
    flowmesh::WireMessage wire;
    wire.kind = flowmesh::WireMessageKind::PROPOSAL;
    wire.header = HeaderFor(proposal->entry);
    wire.payload = *payload;
    // Local originals already broadcast normally; their returning copies
    // must not create a second broadcast through the forwarding path.
    candidate->proposal_forward_attempt = market.clock->Now();
    candidate->local_signers.insert(proposal->proposer_seat_index);
    if (!candidate->relay_proof) candidate->relay_proof = Market::Candidate::ProposalProof{
        proposal->round, proposal->proposer_seat_index, proposal->proposer_signature};
    RelayMessage(market, wire, std::nullopt, std::nullopt);
    HandleProposal(market, LOCAL_ACTION_PEER, wire);
}

void FlowMeshRuntime::HandleProposal(
    Market& market, const flowmesh::WirePeerId peer,
    const flowmesh::WireMessage& message)
{
    if (market.agreement) {
        DeliveryEvent(market, "proposal_refused", message.kind, {},
                      message.header.sequence, peer, "preagreement_market_requires_commit_decision");
        return;
    }
    const auto proposal{
        flowmesh::DecodeProductionProposalPayload(message.payload)};
    if (!proposal ||
        !flowmesh::ProductionWireHeaderMatches(message.header,
                                               proposal->entry)) {
        return;
    }
    auto proposal_trace{WireTrace(message)};
    proposal_trace.round = proposal->round;
    DeliveryEvent(market, "proposal_received", flowmesh::WireMessageKind::PROPOSAL,
                  proposal->entry.GetHash(), proposal->entry.sequence, peer, "not_yet_authenticated", proposal_trace);

    if (proposal->entry.sequence > market.next_sequence) {
        // A stale validator cannot wait for a future certificate to discover
        // that it is behind: the remaining up-to-date validators may need its
        // vote to form that certificate. Only an authenticated proposal from
        // the currently anchored committee earns one bounded catch-up request.
        // Use the proposal's signed round here because a lagging node's local
        // timeout round is not meaningful for a later sequence.
        if (peer != LOCAL_ACTION_PEER &&
            flowmesh::CheckProductionProposal(
                *proposal, market.domain, market.market_id, market.seats.epoch,
                proposal->round, market.seats) ==
                flowmesh::ProductionProposalCheck::OK) {
            DeliveryEvent(market, "proposal_authenticated", flowmesh::WireMessageKind::PROPOSAL,
                          proposal->entry.GetHash(), proposal->entry.sequence, peer, "future_sequence_catchup",
                          SeatTrace(market, proposal->proposer_seat_index, proposal->proposer_signature, proposal->round));
            RequestCatchup(peer, market.market_id);
        }
        return;
    }

    if (proposal->entry.sequence != market.next_sequence) {
        DeliveryEvent(market, "proposal_refused", flowmesh::WireMessageKind::PROPOSAL,
                      proposal->entry.GetHash(), proposal->entry.sequence, peer,
                      "sequence_already_applied", proposal_trace);
        return;
    }
    // A round selects and authenticates a proposer; it is not part of the
    // certified entry or attestation digest. Local timers can differ after
    // reconnect/restart, so they must not prevent checking the same entry.
    // Validate against the envelope's signed round, never an untrusted hint.
    if (flowmesh::CheckProductionProposal(
            *proposal, market.domain, market.market_id, market.seats.epoch,
            proposal->round, market.seats) !=
            flowmesh::ProductionProposalCheck::OK) {
        DeliveryEvent(market, "proposal_refused", flowmesh::WireMessageKind::PROPOSAL,
                      proposal->entry.GetHash(), proposal->entry.sequence, peer,
                      "proposal_authentication", proposal_trace);
        return;
    }
    DeliveryEvent(market, "proposal_authenticated", flowmesh::WireMessageKind::PROPOSAL,
                  proposal->entry.GetHash(), proposal->entry.sequence, peer, {},
                  SeatTrace(market, proposal->proposer_seat_index, proposal->proposer_signature, proposal->round));
    // Re-read newest anchor/set policy immediately before any local vote.
    // A previously executed candidate cannot authorize stale-set signing.
    const auto transition{CurrentSeatTransition(market)};
    const bool execution{
        proposal->entry.kind == static_cast<uint8_t>(
                                    flowmesh::ProductionEntryKind::EXECUTION)};
    const bool handoff{
        proposal->entry.kind == static_cast<uint8_t>(
                                  flowmesh::ProductionEntryKind::EPOCH_HANDOFF)};
    if (!transition || !market.chain->Acceptable(proposal->entry.anchor) ||
        (execution && transition->kind !=
                          FlowMeshSeatTransitionKind::CONTINUE) ||
        (handoff &&
         (transition->kind != FlowMeshSeatTransitionKind::HANDOFF ||
          !transition->next_seats ||
          transition->next_seats->epoch != proposal->entry.next_epoch ||
          transition->next_seats->set_hash !=
              proposal->entry.next_seat_set_hash))) {
        DeliveryEvent(market, "proposal_waiting", flowmesh::WireMessageKind::PROPOSAL,
                      proposal->entry.GetHash(), proposal->entry.sequence, peer, "chain_or_seat_transition", proposal_trace);
        return;
    }
    const uint256 hash{proposal->entry.GetHash()};
    const flowmesh::ProductionSignPosition position{market.seats.epoch,
                                                    market.next_sequence};
    std::optional<StoredLockedProductionCandidate> retained;
    std::optional<uint256> locked_hash;
    std::string lock_error;
    if (!market.store->ReadLockedCandidate(position, retained, lock_error) ||
        !market.store->ReadLock(position, locked_hash, lock_error) ||
        retained.has_value() != locked_hash.has_value() ||
        (retained && retained->entry.GetHash() != *locked_hash) ||
        (market.diagnostics.local_locked_candidate &&
         market.diagnostics.local_locked_candidate != locked_hash) ||
        (locked_hash && market.candidates.count(*locked_hash) == 0)) {
        HaltMarket(market, FlowMeshRuntimeHalt::STORE_FAILURE,
                   lock_error.empty()
                       ? "FlowMesh pending lock and retained candidate disagree"
                       : std::move(lock_error));
        return;
    }
    if (locked_hash && *locked_hash != hash) {
        DeliveryEvent(market, "proposal_refused", flowmesh::WireMessageKind::PROPOSAL,
                      hash, proposal->entry.sequence, peer, "different_durable_candidate", proposal_trace);
        if (peer == LOCAL_ACTION_PEER) {
            HaltMarket(market, FlowMeshRuntimeHalt::SIGNING_CONFLICT,
                       "local FlowMesh proposal conflicts with its retained candidate");
        } else {
            // A competing proposal is not a conflicting certificate. Keep
            // the exact durable lock and continue retrying that candidate;
            // a remote proposer must not stop our worker with another hash.
            CountObservation(market.diagnostics.proposals_conflicting_lock);
        }
        return;
    }
    auto candidate_it{market.candidates.find(hash)};
    if (candidate_it == market.candidates.end()) {
        if (market.candidates.size() >= MAX_RUNTIME_CANDIDATES_PER_SEQUENCE) {
            DeliveryEvent(market, "proposal_refused", flowmesh::WireMessageKind::PROPOSAL,
                          hash, proposal->entry.sequence, peer, "candidate_limit", proposal_trace);
            return;
        }
        const auto evidence{market.pool.EvidenceFor(proposal->entry.actions)};
        if (!evidence) {
            CountObservation(market.diagnostics.proposals_missing_evidence);
            DeliveryEvent(market, "proposal_waiting", flowmesh::WireMessageKind::PROPOSAL,
                          hash, proposal->entry.sequence, peer, "missing_action_evidence", proposal_trace);
            return;
        }
        auto candidate{EvaluateCandidate(market, proposal->entry, &*evidence)};
        if (!candidate) return;
        candidate_it = market.candidates.emplace(hash,
                                                  std::move(*candidate)).first;
        TraceActions(market, "action_in_proposal", proposal->entry);
    }

    if (proposal->round != market.round) {
        CountObservation(market.diagnostics.proposals_verified_different_round);
    }
    CountObservation(market.delivery.verified);
    DeliveryEvent(market, "proposal_verified", flowmesh::WireMessageKind::PROPOSAL,
                  hash, market.next_sequence, peer, {}, proposal_trace);
    if (!candidate_it->second.relay_proof) candidate_it->second.relay_proof = Market::Candidate::ProposalProof{
        proposal->round, proposal->proposer_seat_index, proposal->proposer_signature};
    // Preserve the existing adjacent-round timer nudge for mixed-version
    // peers. Distant rounds may carry a valid entry, but cannot set our
    // scheduling clock or remotely exhaust its finite round space.
    if (market.round != std::numeric_limits<uint32_t>::max() &&
        proposal->round == market.round + 1) {
        market.round = proposal->round;
        market.round_started = market.clock->Now();
        RoundEntered(market, "adjacent_authenticated_proposal");
    }

    auto local_keys{LocalSeatKeys(market)};
    if (peer == LOCAL_ACTION_PEER) {
        // When a wallet owns more seats than fit in one relay budget, serve
        // the oldest retry first rather than starving higher seat indices.
        const auto last_attempt = [&](const uint32_t seat) {
            const auto& attempts{candidate_it->second.attestation_forward_attempts};
            const auto it{attempts.find(seat)};
            return it == attempts.end()
                ? std::optional<flowmesh::WireClock::time_point>{}
                : it->second;
        };
        std::stable_sort(local_keys.begin(), local_keys.end(),
                        [&](const auto& a, const auto& b) {
                            return last_attempt(a.first) < last_attempt(b.first);
                        });
    }
    if (!local_keys.empty() &&
        !RetainCandidateBeforeSigning(market, candidate_it->second)) {
        return;
    }
    // Send the fully checked proposal onward before our own vote. Transport
    // priority may still reorder delivery, so incoming repeats get paced
    // opportunities rather than once-ever deduplication.
    if (!ForwardCommitteeMessage(market, candidate_it->second.entry, peer, message,
                                 candidate_it->second.proposal_forward_attempt)) return;
    flowmesh::ProductionSigningGuard guard{*market.store};
    for (const auto& [seat_index, key] : local_keys) {
        auto& attestations{market.attestations[hash]};
        const auto cached{attestations.find(seat_index)};
        if (cached != attestations.end()) {
            // Replay the exact vote; do not sign again. A locally retried
            // proposal also needs its vote resent: a peer may have joined
            // after the original broadcast, or reconciliation may have
            // suppressed that send. Pace local broadcasts with the same
            // per-seat and shared limits as committee forwarding.
            const auto payload{
                flowmesh::EncodeProductionAttestationPayload(cached->second)};
            if (!payload) continue;
            flowmesh::WireMessage wire;
            wire.kind = flowmesh::WireMessageKind::ATTESTATION;
            wire.header = HeaderFor(candidate_it->second.entry);
            wire.payload = *payload;
            if (peer == LOCAL_ACTION_PEER) {
                const auto now{market.clock->Now()};
                auto& last_attempt{
                    candidate_it->second.attestation_forward_attempts[seat_index]};
                if (last_attempt &&
                    now < *last_attempt + FlowMeshCommitteeRelayBudget::REPEAT_DELAY) {
                    continue;
                }
                const size_t bytes{
                    flowmesh::FLOWMESH_WIRE_HEADER_SIZE + wire.payload.size()};
                if (!m_committee_relay_budget.Available(now, bytes) ||
                    !market.committee_relay_budget.Available(now, bytes)) {
                    continue;
                }
                // Keep budget-denied local votes eligible for a later tick.
                // Charge before the external gate, even if it drops the send.
                last_attempt = now;
                m_committee_relay_budget.Charge(bytes);
                market.committee_relay_budget.Charge(bytes);
                RelayMessage(market, std::move(wire), std::nullopt, std::nullopt, true);
            } else {
                // Preserve the direct response to an authenticated remote
                // proposal; the synthetic local peer is never a destination.
                RelayMessage(market, std::move(wire), peer, std::nullopt);
            }
            continue;
        }
        flowmesh::ProductionLockResult lock;
        TraceContext sign_trace;
        sign_trace.seat_index = seat_index;
        DeliveryEvent(market, "attestation_signing_started", flowmesh::WireMessageKind::ATTESTATION,
                      hash, market.next_sequence, LOCAL_ACTION_PEER, {}, sign_trace);
        const auto attestation{flowmesh::SignProductionEntryAttestation(
            key, seat_index, candidate_it->second.entry, market.seats, guard,
            lock)};
        if (!attestation) {
            DeliveryEvent(market, "attestation_signing_failed", flowmesh::WireMessageKind::ATTESTATION,
                          hash, market.next_sequence, LOCAL_ACTION_PEER,
                          "lock_result=" + std::to_string(static_cast<unsigned>(lock)), sign_trace);
            if (lock == flowmesh::ProductionLockResult::CONFLICT ||
                lock == flowmesh::ProductionLockResult::STORAGE_FAILURE) {
                HaltMarket(market, FlowMeshRuntimeHalt::SIGNING_CONFLICT,
                           lock == flowmesh::ProductionLockResult::CONFLICT
                               ? "permanent FlowMesh attestation lock conflicts"
                               : "permanent FlowMesh attestation lock failed");
            }
            return;
        }
        attestations.emplace(seat_index, *attestation);
        candidate_it->second.local_signers.insert(seat_index);
        market.attested_hash_by_seat.emplace(seat_index, hash);
        DeliveryEvent(market, "attestation_signed", flowmesh::WireMessageKind::ATTESTATION,
                      hash, market.next_sequence, LOCAL_ACTION_PEER, {},
                      SeatTrace(market, seat_index, attestation->signature.Compressed()));
        const auto payload{
            flowmesh::EncodeProductionAttestationPayload(*attestation)};
        if (!payload) continue;
        flowmesh::WireMessage wire;
        wire.kind = flowmesh::WireMessageKind::ATTESTATION;
        wire.header = HeaderFor(candidate_it->second.entry);
        wire.payload = *payload;
        candidate_it->second.attestation_forward_attempts[seat_index] =
            market.clock->Now();
        RelayMessage(market, std::move(wire), std::nullopt, std::nullopt);
    }
    MaybeCertify(market, hash);
}

void FlowMeshRuntime::HandleAttestation(
    Market& market, const flowmesh::WirePeerId peer,
    const flowmesh::WireMessage& message)
{
    if (message.header.market_id != market.market_id ||
        message.header.epoch != market.seats.epoch ||
        message.header.sequence != market.next_sequence) {
        return;
    }
    const auto attestation{
        flowmesh::DecodeProductionAttestationPayload(message.payload)};
    if (!attestation || attestation->seat_index >= market.seats.Size()) return;
    auto receive_trace{WireTrace(message)};
    receive_trace.seat_index = attestation->seat_index;
    receive_trace.signature_hash = Hash(attestation->signature.Compressed());
    DeliveryEvent(market, "attestation_received", flowmesh::WireMessageKind::ATTESTATION,
                  {}, market.next_sequence, peer, "not_yet_authenticated", receive_trace);
    if (market.candidates.empty()) {
        CountObservation(market.diagnostics.attestations_without_candidate);
        DeliveryEvent(market, "attestation_waiting", flowmesh::WireMessageKind::ATTESTATION,
                      {}, market.next_sequence, peer, "no_candidate", receive_trace);
        return;
    }

    std::optional<uint256> matching_hash;
    DeliveryEvent(market, "attestation_verification_started", flowmesh::WireMessageKind::ATTESTATION,
                  {}, market.next_sequence, peer, {}, receive_trace);
    for (const auto& [hash, candidate] : market.candidates) {
        const uint256 digest{flowmesh::FlowMeshBlsCertificateDigest(
            flowmesh::ProductionCertificateContext(candidate.entry))};
        if (!bls::Verify(
                market.seats.members[attestation->seat_index].key.Key(),
                std::span<const unsigned char>{digest.begin(), 32},
                attestation->signature)) {
            continue;
        }
        if (matching_hash) return;
        matching_hash = hash;
    }
    if (!matching_hash) {
        DeliveryEvent(market, "attestation_refused", flowmesh::WireMessageKind::ATTESTATION,
                      {}, market.next_sequence, peer, "signature_matches_no_candidate", receive_trace);
        return;
    }
    DeliveryEvent(market, "attestation_signature_verified", flowmesh::WireMessageKind::ATTESTATION,
                  *matching_hash, market.next_sequence, peer, "not_yet_accepted", receive_trace);
    const auto prior{
        market.attested_hash_by_seat.find(attestation->seat_index)};
    if (prior != market.attested_hash_by_seat.end() &&
        prior->second != *matching_hash) {
        DeliveryEvent(market, "attestation_refused", flowmesh::WireMessageKind::ATTESTATION,
                      *matching_hash, market.next_sequence, peer, "seat_already_attested_other_candidate", receive_trace);
        return;
    }
    auto& by_seat{market.attestations[*matching_hash]};
    const auto existing{by_seat.find(attestation->seat_index)};
    if (existing == by_seat.end()) {
        by_seat.emplace(attestation->seat_index, *attestation);
        market.attested_hash_by_seat.emplace(attestation->seat_index,
                                             *matching_hash);
        CountObservation(market.delivery.verified);
        DeliveryEvent(market, "attestation_verified", flowmesh::WireMessageKind::ATTESTATION,
                      *matching_hash, market.next_sequence, peer, "new_active_seat_vote_accepted",
                      SeatTrace(market, attestation->seat_index, attestation->signature.Compressed()));
    } else if (existing->second.signature.Compressed() !=
               attestation->signature.Compressed()) {
        return;
    }
    auto& candidate{market.candidates.at(*matching_hash)};
    if (!ForwardCommitteeMessage(
        market, candidate.entry, peer, message,
        candidate.attestation_forward_attempts[attestation->seat_index])) return;
    MaybeCertify(market, *matching_hash);
}

bool FlowMeshRuntime::ForwardCommitteeMessage(
    Market& market, const flowmesh::ProductionEntryCore& entry,
    const flowmesh::WirePeerId peer, const flowmesh::WireMessage& message,
    std::optional<flowmesh::WireClock::time_point>& last_attempt)
{
    if (!market.ready || market.pending_handoff ||
        market.halt != FlowMeshRuntimeHalt::NONE) return false;
    const auto transition{CurrentSeatTransition(market)};
    if (!transition || !market.chain->Acceptable(entry.anchor)) return false;
    const bool execution{entry.kind == static_cast<uint8_t>(
        flowmesh::ProductionEntryKind::EXECUTION)};
    if (execution ? transition->kind != FlowMeshSeatTransitionKind::CONTINUE
                  : (transition->kind != FlowMeshSeatTransitionKind::HANDOFF ||
                     !transition->next_seats ||
                     transition->next_seats->epoch != entry.next_epoch ||
                     transition->next_seats->set_hash != entry.next_seat_set_hash)) {
        return false;
    }
    if (peer == LOCAL_ACTION_PEER) return true;
    const auto now{market.clock->Now()};
    if (last_attempt &&
        now < *last_attempt + FlowMeshCommitteeRelayBudget::REPEAT_DELAY) return true;
    // Even denied attempts wait: untrusted repetitions and round churn may
    // not spin against a depleted budget. No delayed payload queue is kept.
    last_attempt = now;
    const size_t bytes{flowmesh::FLOWMESH_WIRE_HEADER_SIZE + message.payload.size()};
    if (!m_committee_relay_budget.Available(now, bytes) ||
        !market.committee_relay_budget.Available(now, bytes)) return true;
    m_committee_relay_budget.Charge(bytes);
    market.committee_relay_budget.Charge(bytes);
    // Charge before the external gate/callback, including suppressed sends.
    RelayMessage(market, message, std::nullopt, peer, true);
    return true;
}

void FlowMeshRuntime::MaybeCertify(Market& market,
                                   const uint256& candidate_hash)
{
    if (market.agreement && (!RefreshAgreement(market) ||
        market.agreement->DecidedCandidate() != candidate_hash)) return;
    const auto candidate{market.candidates.find(candidate_hash)};
    const auto signatures{market.attestations.find(candidate_hash)};
    if (candidate == market.candidates.end() ||
        signatures == market.attestations.end() ||
        signatures->second.size() <
            flowmesh::FlowMeshBlsThreshold(market.seats.Size())) {
        return;
    }
    std::vector<flowmesh::IndexedBlsSignature> partials;
    partials.reserve(signatures->second.size());
    for (const auto& [index, signature] : signatures->second) {
        (void)index;
        partials.push_back(signature);
    }
    flowmesh::BlsMicroblockCertificate certificate;
    DeliveryEvent(market, "certificate_assembly_started", flowmesh::WireMessageKind::CERTIFICATE,
                  candidate_hash, market.next_sequence, std::nullopt,
                  "votes=" + std::to_string(partials.size()) +
                      " required=" + std::to_string(flowmesh::FlowMeshBlsThreshold(market.seats.Size())));
    if (flowmesh::AssembleProductionEntryCertificate(
            candidate->second.entry, market.seats, partials, certificate) !=
        flowmesh::BlsCertificateAssemblyCheck::OK) {
        DeliveryEvent(market, "certificate_assembly_failed", flowmesh::WireMessageKind::CERTIFICATE,
                      candidate_hash, market.next_sequence);
        return;
    }
    const flowmesh::ProductionCertifiedEnvelope certified{
        candidate->second.entry, certificate};
    if (!candidate->second.certificate_formed) {
        candidate->second.certificate_formed = true;
        CountObservation(market.delivery.certificate_formed);
        DeliveryEvent(market, "certificate_formed", flowmesh::WireMessageKind::CERTIFICATE,
                      candidate_hash, market.next_sequence);
    }
    const auto payload{flowmesh::EncodeProductionCertifiedPayload(
        certified, market.seats.Size())};
    if (!payload) return;
    flowmesh::WireMessage message;
    message.kind = flowmesh::WireMessageKind::CERTIFICATE;
    message.header = HeaderFor(certified.entry);
    message.payload = *payload;
    HandleCertificate(market, LOCAL_ACTION_PEER, message);
}

void FlowMeshRuntime::HandleCertificate(
    Market& market, const flowmesh::WirePeerId peer,
    const flowmesh::WireMessage& message, const bool from_catchup,
    bool* reconciliation_deferred)
{
    if (reconciliation_deferred) *reconciliation_deferred = false;
    const uint64_t delivery_generation{market.chain->DeliveryGeneration()};
    if (message.payload.size() <=
        flowmesh::FLOWMESH_CERTIFIED_PAYLOAD_PREFIX_SIZE) {
        return;
    }
    const uint32_t entry_size{ReadBE32(message.payload.data())};
    if (entry_size == 0 ||
        entry_size > flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_BYTES ||
        message.payload.size() <
            flowmesh::FLOWMESH_CERTIFIED_PAYLOAD_PREFIX_SIZE + entry_size) {
        return;
    }
    const auto entry{flowmesh::DecodeProductionEntry(std::span{
        message.payload.data() +
            flowmesh::FLOWMESH_CERTIFIED_PAYLOAD_PREFIX_SIZE,
        static_cast<size_t>(entry_size)})};
    if (!entry || entry->domain != market.domain ||
        entry->market_id != market.market_id ||
        !flowmesh::ProductionWireHeaderMatches(message.header, *entry)) {
        return;
    }

    std::optional<flowmesh::ActiveFnBlsSeatSet> certified_seats;
    if (entry->epoch == market.seats.epoch &&
        entry->seat_set_hash == market.seats.set_hash) {
        certified_seats = market.seats;
    } else {
        certified_seats = market.chain->SeatSet(
            market.domain, market.market_id, entry->epoch,
            entry->seat_set_hash);
    }
    if (!certified_seats ||
        flowmesh::CheckActiveFnBlsSeatSet(market.domain, *certified_seats) !=
            flowmesh::BlsSeatSetCheck::OK) {
        return;
    }
    const auto certified{flowmesh::DecodeProductionCertifiedPayload(
        message.payload, certified_seats->Size())};
    if (!certified ||
        flowmesh::CheckProductionEntryCertificate(
            certified->entry, *certified_seats, certified->certificate) !=
            flowmesh::BlsCertificateCheck::OK) {
        return;
    }

    CountObservation(market.delivery.verified);
    DeliveryEvent(market, "certificate_verified", flowmesh::WireMessageKind::CERTIFICATE,
                  certified->entry.GetHash(), certified->entry.sequence, peer, {}, EntryTrace(certified->entry));
    if (certified->entry.sequence < market.next_sequence) {
        std::optional<StoredProductionEntry> stored;
        std::string error;
        if (!market.store->ReadEntry(certified->entry.sequence,
                                     *certified_seats, stored, error) ||
            !stored) {
            HaltMarket(market, FlowMeshRuntimeHalt::STORE_FAILURE,
                       error.empty() ? "durable certified entry is unreadable"
                                     : std::move(error));
            return;
        }
        if (stored->entry.GetHash() != certified->entry.GetHash()) {
            HaltMarket(market, FlowMeshRuntimeHalt::CERTIFICATE_CONFLICT,
                       "two valid FlowMesh certificates conflict at one sequence");
        }
        return;
    }
    if (certified->entry.sequence > market.next_sequence) {
        if (!from_catchup && peer != LOCAL_ACTION_PEER) {
            RequestCatchup(peer, market.market_id);
        }
        return;
    }
    if (!SameSeatIdentity(*certified_seats, market.seats) ||
        market.pending_handoff || market.halt != FlowMeshRuntimeHalt::NONE) {
        return;
    }

    // The full certificate has been verified, but its execution is not yet
    // established. Catch-up pages already consumed their request and need an
    // explicit owner for this exact retry if a chain gate interrupts here.
    // Only pre-append paths use this classification; storage/conflict failures
    // below retain their existing permanent failure behavior.
    const auto defer_candidate_validation = [&] {
        if (reconciliation_deferred &&
            certified->entry.parent_hash == market.last_hash &&
            certified->entry.previous_state_root == market.state.Root() &&
            CertifiedAnchorReconciliationInterrupted(market, certified->entry, delivery_generation)) {
            *reconciliation_deferred = true;
            DeliveryEvent(market, "certificate_validation_deferred", flowmesh::WireMessageKind::CERTIFICATE,
                          certified->entry.GetHash(), certified->entry.sequence, peer,
                          "verified certificate awaits execution after reconciliation", EntryTrace(certified->entry));
        }
    };
    const uint256 hash{certified->entry.GetHash()};
    if (market.agreement) {
        // Reconciliation may block RefreshAgreement, but cannot make an
        // already durable decision at this exact slot retryable as a different
        // hash. An older slot's decision is irrelevant to this certificate.
        const auto& context{market.agreement->Context()};
        if (context.epoch == certified->entry.epoch && context.sequence == certified->entry.sequence) {
            const auto decided{market.agreement->DecidedCandidate()};
            if (decided && *decided != hash) {
                HaltMarket(market, FlowMeshRuntimeHalt::CERTIFICATE_CONFLICT,
                           "valid V1 certificate contradicts the durable agreement decision");
                return;
            }
        }
        if (!RefreshAgreement(market)) { defer_candidate_validation(); return; }
        const auto decided{market.agreement->DecidedCandidate()};
        if (decided && *decided != hash) {
            HaltMarket(market, FlowMeshRuntimeHalt::CERTIFICATE_CONFLICT,
                       "valid V1 certificate contradicts the durable agreement decision");
            return;
        }
    }
    auto candidate_it{market.candidates.find(hash)};
    if (candidate_it == market.candidates.end()) {
        auto candidate{EvaluateCandidate(market, certified->entry, nullptr)};
        if (!candidate) { defer_candidate_validation(); return; }
        candidate_it = market.candidates.emplace(hash,
                                                  std::move(*candidate)).first;
    }
    if (!CommitCertified(market, *certified, candidate_it->second, reconciliation_deferred)) return;
    if (!from_catchup) {
        RelayMessage(
            market, message, std::nullopt,
            peer == LOCAL_ACTION_PEER
                ? std::nullopt
                : std::optional<flowmesh::WirePeerId>{peer});
    }
}

void FlowMeshRuntime::HandleGet(
    Market& market, const flowmesh::WirePeerId peer,
    const flowmesh::WireMessage& message)
{
    uint16_t maximum_entries{0};
    uint32_t maximum_bytes{0};
    if (message.header.market_id != market.market_id ||
        message.header.sequence > market.next_sequence ||
        !flowmesh::DecodeCatchupRequest(message.payload, maximum_entries,
                                        maximum_bytes)) {
        return;
    }
    if (message.header.sequence < market.next_sequence) {
        const auto starting_seats{market.chain->SeatSetForSequence(
            market.domain, market.market_id, message.header.sequence)};
        if (!starting_seats || starting_seats->epoch != message.header.epoch) {
            return;
        }
    } else if (message.header.epoch != market.seats.epoch) {
        return;
    }

    std::vector<std::vector<unsigned char>> entries;
    size_t encoded_bytes{2};
    for (uint64_t sequence{message.header.sequence};
         sequence < market.next_sequence && entries.size() < maximum_entries;
         ++sequence) {
        const auto seats{market.chain->SeatSetForSequence(
            market.domain, market.market_id, sequence)};
        std::optional<StoredProductionEntry> stored;
        std::string error;
        if (!seats || !market.store->ReadEntry(sequence, *seats, stored, error) ||
            !stored) {
            HaltMarket(market, FlowMeshRuntimeHalt::STORE_FAILURE,
                       error.empty() ? "catch-up history is unavailable"
                                     : std::move(error));
            return;
        }
        const auto encoded{flowmesh::EncodeProductionCertifiedPayload(
            flowmesh::ProductionCertifiedEnvelope{stored->entry,
                                                  stored->certificate},
            seats->Size())};
        if (!encoded || encoded_bytes > maximum_bytes ||
            maximum_bytes - encoded_bytes < 4 ||
            encoded->size() > maximum_bytes - encoded_bytes - 4) {
            break;
        }
        encoded_bytes += 4 + encoded->size();
        entries.push_back(*encoded);
    }
    if (entries.empty()) return;
    const auto payload{flowmesh::EncodeCatchupEntries(entries)};
    if (!payload || payload->size() > maximum_bytes) return;
    flowmesh::WireMessage response;
    response.kind = flowmesh::WireMessageKind::ENTRIES;
    response.header = message.header;
    response.payload = *payload;
    RelayMessage(market, std::move(response), peer, std::nullopt);
}

void FlowMeshRuntime::HandleEntries(
    Market& market, const flowmesh::WirePeerId peer,
    const flowmesh::WireMessage& message)
{
    const auto key{std::make_pair(peer, market.market_id)};
    const auto pending{m_pending_catchup.find(key)};
    const size_t received_count{message.payload.size() >= 2 ? ReadBE16(message.payload.data()) : 0U};
    const auto now{m_config.clock->Now()};
    const auto refuse = [&](const char* stage, const std::string& details) {
        CountObservation(market.delivery.catchup_replies_refused);
        market.delivery.current_reason = std::string{stage} + "; " + details;
        DeliveryEvent(market, stage, flowmesh::WireMessageKind::ENTRIES,
                      market.last_hash, message.header.sequence, peer, details);
    };
    if (pending == m_pending_catchup.end()) {
        refuse("catchup_no_matching_request",
               CatchupDetails(0, 0, received_count, message.payload.size(), -1));
        return; // unsolicited or late; this is not an invalid-signature claim
    }
    const auto request{pending->second};
    const auto elapsed{std::chrono::duration_cast<std::chrono::milliseconds>(now - request.started).count()};
    const auto details{CatchupDetails(request.max_entries, request.max_bytes,
                                     received_count, message.payload.size(), elapsed)};
    if (now >= request.deadline) {
        ExpireCatchup(peer, market.market_id);
        refuse("catchup_expired_reply", details);
        return;
    }
    if (message.header.market_id != market.market_id ||
        message.header.epoch != pending->second.epoch ||
        message.header.sequence != pending->second.from_sequence) {
        refuse("catchup_request_mismatch", details);
        return; // leave the actual requested page outstanding
    }
    const auto entries{flowmesh::DecodeCatchupEntries(message.payload)};
    if (!entries || !m_catchup_tracker.AcceptResponse(
                        peer, market.market_id, message.header.sequence,
                        entries ? entries->size() : 0,
                        message.payload.size())) {
        refuse(entries ? "catchup_invalid_bounds" : "catchup_malformed_page", details);
        m_catchup_tracker.Cancel(peer, market.market_id);
        m_pending_catchup.erase(pending);
        return;
    }
    m_pending_catchup.erase(pending);

    uint64_t expected{message.header.sequence};
    for (const std::vector<unsigned char>& payload : *entries) {
        if (payload.size() <=
            flowmesh::FLOWMESH_CERTIFIED_PAYLOAD_PREFIX_SIZE) {
            break;
        }
        const uint32_t entry_size{ReadBE32(payload.data())};
        if (entry_size == 0 ||
            entry_size > flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_BYTES ||
            payload.size() <
                flowmesh::FLOWMESH_CERTIFIED_PAYLOAD_PREFIX_SIZE + entry_size) {
            break;
        }
        const auto entry{flowmesh::DecodeProductionEntry(std::span{
            payload.data() + flowmesh::FLOWMESH_CERTIFIED_PAYLOAD_PREFIX_SIZE,
            static_cast<size_t>(entry_size)})};
        if (!entry || entry->domain != market.domain ||
            entry->market_id != market.market_id ||
            entry->sequence != expected) {
            break;
        }
        flowmesh::WireMessage certified;
        certified.kind = flowmesh::WireMessageKind::CERTIFICATE;
        certified.header = HeaderFor(*entry);
        certified.payload = payload;
        bool reconciliation_deferred{false};
        HandleCertificate(market, peer, certified,
                          /*from_catchup=*/true, &reconciliation_deferred);
        if (reconciliation_deferred) {
            // ENTRIES is not a critical ProcessMessage kind, and its request
            // was consumed before applying the page. Keep this exact verified
            // certificate and a bounded catch-up obligation for the remaining
            // page, even when its first entry made no progress. Only a verified
            // certificate interrupted before any append can earn this retry;
            // its complete execution is still checked again on the retry.
            DeferMessage(market, {peer, certified});
            if (m_receive_recovery.size() < 128) m_receive_recovery.emplace(peer, market.market_id);
        }
        if (market.halt != FlowMeshRuntimeHalt::NONE ||
            market.next_sequence != expected + 1) {
            break;
        }
        ++expected;
    }
    const auto applied{expected - message.header.sequence};
    const bool fully_applied{applied == entries->size() && market.halt == FlowMeshRuntimeHalt::NONE};
    const auto finished{m_config.clock->Now()};
    const auto apply_details{details + " applied=" + std::to_string(applied)};
    if (fully_applied) {
        const auto profile{m_catchup_profiles.find(key)};
        // Only a full count-limited page with generous observed headroom can
        // grow. A working slow-link count stays stable instead of oscillating
        // between a successful small page and a timed-out double-size page.
        if (profile != m_catchup_profiles.end() &&
            entries->size() == request.max_entries &&
            finished - request.started <= CATCHUP_GROWTH_HEADROOM) {
            profile->second.max_entries = static_cast<uint16_t>(std::min<size_t>(
                flowmesh::FLOWMESH_CATCHUP_MAX_ENTRIES, request.max_entries * 2));
            profile->second.last_used = finished;
        }
        DeliveryEvent(market, "catchup_page_verified", flowmesh::WireMessageKind::ENTRIES,
                      market.last_hash, message.header.sequence, peer, apply_details);
    } else {
        CountObservation(market.delivery.catchup_partial_pages);
        market.delivery.current_reason = "catchup_not_fully_applied; " + apply_details;
        DeliveryEvent(market, "catchup_not_fully_applied", flowmesh::WireMessageKind::ENTRIES,
                      market.last_hash, message.header.sequence, peer, apply_details);
    }
    if (market.halt == FlowMeshRuntimeHalt::NONE &&
        expected > message.header.sequence) {
        m_catchup_cooldowns.erase(key);
        // Do not create an unanswered tip probe (and its failure cooldown)
        // when an honest greedy sender had room for any further legal entry.
        // This is only local scheduling: no remote tip becomes trusted.
        // Count- or byte-limited pages, including partial application, retain
        // the existing follow-up; a short count alone cannot imply a tail.
        const bool tail_slack{FlowMeshCatchupPageHasTailSlack(
                request.max_entries, request.max_bytes, entries->size(),
                message.payload.size(), applied)};
        if (!tail_slack) {
            RequestCatchup(peer, market.market_id);
        } else {
            CountObservation(market.delivery.catchup_completed);
            DeliveryEvent(market, "catchup_page_applied_with_tail_slack", flowmesh::WireMessageKind::ENTRIES,
                          market.last_hash, market.next_sequence, peer, apply_details);
        }
    }
}

} // namespace node
