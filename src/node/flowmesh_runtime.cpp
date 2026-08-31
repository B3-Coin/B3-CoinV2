// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_runtime.h>

#include <crypto/common.h>
#include <serialize.h>

#include <algorithm>
#include <limits>
#include <set>
#include <span>
#include <utility>

namespace node {
namespace {

constexpr flowmesh::WirePeerId LOCAL_ACTION_PEER{
    std::numeric_limits<flowmesh::WirePeerId>::min()};
constexpr size_t MAX_RUNTIME_CANDIDATES_PER_SEQUENCE{8};
constexpr size_t MAX_PENDING_MARKET_ADDITIONS{1'024};

class RuntimeActionPool
{
public:
    using Verifier = std::function<bool(const flowmesh::Action&)>;

    void SetVerifier(Verifier verifier) { m_verifier = std::move(verifier); }

    bool Add(const flowmesh::Action& action,
             const flowmesh::WirePeerId origin)
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
        m_actions.emplace(id, action);
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
        return true;
    }

    std::optional<std::vector<flowmesh::Action>> EvidenceFor(
        const std::span<const flowmesh::Action> semantic_actions) const
    {
        std::vector<flowmesh::Action> out;
        out.reserve(semantic_actions.size());
        for (const flowmesh::Action& semantic : semantic_actions) {
            const auto it{m_actions.find(semantic.Id())};
            if (it == m_actions.end()) return std::nullopt;
            flowmesh::Action stripped{it->second};
            stripped.credential.clear();
            const auto semantic_bytes{
                flowmesh::EncodeProductionActionPayload(semantic)};
            const auto stripped_bytes{
                flowmesh::EncodeProductionActionPayload(stripped)};
            if (!semantic_bytes || !stripped_bytes ||
                *semantic_bytes != *stripped_bytes) {
                return std::nullopt;
            }
            out.push_back(it->second);
        }
        return out;
    }

    size_t Size() const { return m_actions.size(); }

    std::vector<flowmesh::Action> Select(
        const flowmesh::FlowMeshState& state, const size_t maximum) const
    {
        std::vector<flowmesh::Action> out;
        out.reserve(std::min(maximum, m_actions.size()));
        for (const auto& [outpoint, id] : m_deposits) {
            if (out.size() >= maximum) return out;
            if (!state.DepositConsumed(outpoint)) out.push_back(m_actions.at(id));
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
            out.push_back(m_actions.at(id));
            ++expected;
        }
        return out;
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
    void Drop(const uint256& id)
    {
        const auto it{m_actions.find(id)};
        if (it == m_actions.end()) return;
        const size_t bytes{static_cast<size_t>(::GetSerializeSize(it->second))};
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
        m_actions.erase(it);
    }

    struct Usage {
        size_t count{0};
        size_t bytes{0};
    };

    Verifier m_verifier;
    std::map<uint256, flowmesh::Action> m_actions;
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
        flowmesh::ProductionEntryCore entry;
        flowmesh::FlowMeshState next_state;
        std::optional<flowmesh::ActiveFnBlsSeatSet> next_seats;
        std::vector<flowmesh::Action> evidence;
        bool reannounce_evidence{false};
    };

    uint256 domain;
    flowmesh::MarketId market_id;
    uint256 treasury_owner_commitment;
    flowmesh::ActiveFnBlsSeatSet seats;
    flowmesh::FlowMeshState state;
    uint64_t next_sequence{0};
    uint64_t next_effect_index{0};
    uint256 last_hash;
    FlowMeshProductionStore* store{nullptr};
    const flowmesh::DepositVerifier* deposits{nullptr};
    FlowMeshRuntimeChain* chain{nullptr};
    FlowMeshRuntimeKeyProvider* keys{nullptr};
    FlowMeshRuntimeClock* clock{nullptr};
    FlowMeshRuntimeRelayFn* relay{nullptr};
    std::chrono::milliseconds round_timeout;

    RuntimeActionPool pool;
    bool ready{true};
    bool paused{false};
    uint32_t round{0};
    flowmesh::WireClock::time_point round_started{};
    bool pending_handoff{false};
    FlowMeshRuntimeHalt halt{FlowMeshRuntimeHalt::NONE};
    std::string error;
    std::optional<flowmesh::AnchorRef> previous_anchor;
    std::map<std::pair<int32_t, uint256>, uint64_t> committed_anchors;
    std::map<uint256, Candidate> candidates;
    std::map<uint256, std::map<uint32_t, flowmesh::IndexedBlsSignature>>
        attestations;
    std::map<uint32_t, uint256> attested_hash_by_seat;

    Market(const FlowMeshRuntimeMarketConfig& config,
           FlowMeshRuntimeConfig& runtime_config)
        : domain{config.domain}, market_id{config.market_id},
          treasury_owner_commitment{config.treasury_owner_commitment},
          seats{config.active_seats}, state{config.state},
          next_sequence{config.next_sequence},
          next_effect_index{config.next_effect_index},
          last_hash{config.last_microblock_hash}, store{config.store},
          deposits{config.deposits}, chain{runtime_config.chain},
          keys{runtime_config.keys}, clock{runtime_config.clock},
          relay{&runtime_config.relay},
          round_timeout{runtime_config.round_timeout},
          ready{config.readiness == FlowMeshRuntimeMarketReadiness::READY},
          paused{!ready},
          round_started{runtime_config.clock->Now()}
    {
    }
};

namespace {

template <typename Market>
void HaltMarket(Market& market, const FlowMeshRuntimeHalt halt,
                std::string error)
{
    if (market.halt != FlowMeshRuntimeHalt::NONE) return;
    market.halt = halt;
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
void RelayMessage(Market& market, flowmesh::WireMessage message,
                  const std::optional<flowmesh::WirePeerId> peer,
                  const std::optional<flowmesh::WirePeerId> exclude)
{
    if (!market.relay || !*market.relay) return;
    (*market.relay)(FlowMeshRuntimeRelay{peer, exclude, std::move(message)});
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
        const auto executed{flowmesh::ExecuteProductionEntry(
            market.state, entry, market.domain, market.market_id, market.seats,
            gate, market.next_sequence, market.next_effect_index,
            market.last_hash, anchors,
            market.treasury_owner_commitment, market.deposits, check)};
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
bool RetainCandidateBeforeSigning(Market& market,
                                  const typename Market::Candidate& candidate)
{
    const auto result{market.store->LockCandidate(candidate.entry,
                                                   candidate.evidence)};
    if (result == flowmesh::ProductionLockResult::LOCKED ||
        result == flowmesh::ProductionLockResult::ALREADY_LOCKED_SAME) {
        return true;
    }
    HaltMarket(market, FlowMeshRuntimeHalt::SIGNING_CONFLICT,
               result == flowmesh::ProductionLockResult::CONFLICT
                   ? "permanent FlowMesh candidate lock conflicts"
                   : "permanent FlowMesh candidate retention failed");
    return false;
}

template <typename Market>
bool CommitCertified(Market& market,
                     const flowmesh::ProductionCertifiedEnvelope& certified,
                     typename Market::Candidate& candidate)
{
    if (!RecheckAnchors(market) ||
        !market.chain->Acceptable(certified.entry.anchor)) {
        if (!market.chain->StillCanonical(certified.entry.anchor)) {
            HaltMarket(market, FlowMeshRuntimeHalt::ANCHOR_INVALIDATED,
                       "candidate anchor became non-canonical before commit");
        }
        return false;
    }
    std::string error;
    if (certified.entry.kind == static_cast<uint8_t>(
                                    flowmesh::ProductionEntryKind::EXECUTION)) {
        flowmesh::FlowMeshState persisted{market.state};
        if (!market.store->AppendExecution(
                certified.entry, certified.certificate, market.seats,
                market.state, AnchorContext(market),
                market.treasury_owner_commitment, market.deposits, persisted,
                error) ||
            persisted.Root() != candidate.next_state.Root()) {
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
                error)) {
            HaltMarket(market, FlowMeshRuntimeHalt::STORE_FAILURE,
                       error.empty() ? "durable handoff append failed"
                                     : std::move(error));
            return false;
        }
        market.pending_handoff = true;
    }

    market.last_hash = certified.entry.GetHash();
    ++market.next_sequence;
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
    market.round = 0;
    market.round_started = market.clock->Now();
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
        m_config.round_timeout <= std::chrono::milliseconds{0}) {
        error = "FlowMesh runtime dependencies or round policy are incomplete";
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
                          std::make_unique<Market>(config, m_config));
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

    auto market{std::make_unique<Market>(config, m_config)};
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
            if (!market->pool.Add(action, LOCAL_ACTION_PEER)) {
                error = "FlowMesh runtime retained action evidence no longer authenticates";
                return false;
            }
        }
        auto candidate{EvaluateCandidate(*market, retained->entry,
                                         &retained->evidence)};
        if (!candidate || candidate->entry.GetHash() !=
                              retained->entry.GetHash()) {
            error = market->error.empty()
                        ? "FlowMesh runtime cannot re-execute its retained signing candidate"
                        : market->error;
            return false;
        }
        candidate->reannounce_evidence = true;
        market->candidates.emplace(candidate->entry.GetHash(),
                                   std::move(*candidate));
    }
    m_markets.emplace(config.market_id, std::move(market));
    return true;
}

bool FlowMeshRuntime::Start(std::string& error)
{
    std::lock_guard<std::mutex> queue_lock{m_queue_mutex};
    if (m_started) return true;
    {
        std::lock_guard<std::mutex> market_lock{m_market_mutex};
        if (!InitializeMarkets(error)) return false;
        m_admitted_markets.clear();
        for (const auto& [market_id, market] : m_markets) {
            if (market->ready) m_admitted_markets.insert(market_id);
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
    std::lock_guard<std::mutex> lock{m_queue_mutex};
    m_started = false;
    m_processing = false;
    m_idle_cv.notify_all();
}

flowmesh::QueueResult FlowMeshRuntime::EnqueueWireMessage(
    const flowmesh::WirePeerId peer, flowmesh::WireMessage message)
{
    std::lock_guard<std::mutex> lock{m_queue_mutex};
    if (!m_started || m_stopping) return flowmesh::QueueResult::GLOBAL_LIMIT;
    // Reject caller-selected market ids before BoundedWireQueue allocates a
    // per-peer/market token bucket. Otherwise a peer can evade throttling and
    // grow the bucket map indefinitely by sending valid frames for random
    // market ids. Keep this queue-owned set separate from m_markets so relay
    // callbacks between runtimes cannot create a cross-runtime lock cycle.
    if (m_admitted_markets.count(message.header.market_id) == 0) {
        return flowmesh::QueueResult::MARKET_LIMIT;
    }
    const auto result{m_queue.Push(peer, std::move(message),
                                   m_config.clock->Now())};
    if (result == flowmesh::QueueResult::ACCEPTED) m_work_cv.notify_one();
    return result;
}

void FlowMeshRuntime::FlowMeshPeerDisconnected(
    const flowmesh::WirePeerId peer)
{
    std::lock_guard<std::mutex> lock{m_queue_mutex};
    m_queue.RemovePeer(peer);
    m_removed_peers.push_back(peer);
    m_work_cv.notify_one();
}

void FlowMeshRuntime::NotifyTick()
{
    std::lock_guard<std::mutex> lock{m_queue_mutex};
    if (!m_started || m_stopping) return;
    m_tick_pending = true;
    m_work_cv.notify_one();
}

bool FlowMeshRuntime::RequestCatchup(
    const flowmesh::WirePeerId peer,
    const flowmesh::MarketId& market_id)
{
    std::lock_guard<std::mutex> lock{m_queue_mutex};
    if (!m_started || m_stopping || market_id.IsNull()) return false;
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
    }
    return EnqueueWireMessage(LOCAL_ACTION_PEER, std::move(message));
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

bool FlowMeshRuntime::WaitForIdle(const std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock{m_queue_mutex};
    return m_idle_cv.wait_for(lock, timeout, [&] {
        return m_queue.Empty() && m_removed_peers.empty() &&
               m_catchup_commands.empty() && m_add_market_commands.empty() &&
               !m_tick_pending && !m_processing;
    });
}

void FlowMeshRuntime::WorkerLoop()
{
    while (true) {
        std::optional<flowmesh::QueuedWireMessage> message;
        std::optional<flowmesh::WirePeerId> removed;
        std::optional<CatchupCommand> catchup;
        std::optional<AddMarketCommand> add_market;
        bool tick{false};
        {
            std::unique_lock<std::mutex> lock{m_queue_mutex};
            m_work_cv.wait(lock, [&] {
                return m_stopping || !m_queue.Empty() ||
                       !m_removed_peers.empty() ||
                       !m_catchup_commands.empty() ||
                       !m_add_market_commands.empty() || m_tick_pending;
            });
            if (m_stopping) break;
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
                m_tick_pending = false;
            } else {
                message = m_queue.Pop();
            }
            m_processing = true;
        }

        if (removed) {
            RemovePeerOnWorker(*removed);
        } else if (add_market) {
            ProcessAddMarketCommand(std::move(*add_market));
        } else if (catchup) {
            ProcessCatchupCommand(*catchup);
        } else if (tick) {
            ProcessTick();
        } else if (message) {
            ProcessMessage(*message);
        }

        {
            std::lock_guard<std::mutex> lock{m_queue_mutex};
            m_processing = false;
            if (m_queue.Empty() && m_removed_peers.empty() &&
                m_catchup_commands.empty() && m_add_market_commands.empty() &&
                !m_tick_pending) {
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
        }
    }
    if (ok) {
        std::lock_guard<std::mutex> lock{m_queue_mutex};
        m_admitted_markets.insert(command.market.market_id);
    }
    command.completion->set_value({ok, std::move(error)});
}

void FlowMeshRuntime::RemovePeerOnWorker(const flowmesh::WirePeerId peer)
{
    m_catchup_tracker.RemovePeer(peer);
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
    const flowmesh::QueuedWireMessage& queued)
{
    std::lock_guard<std::mutex> lock{m_market_mutex};
    const auto it{m_markets.find(queued.message.header.market_id)};
    if (it == m_markets.end()) return;
    Market& market{*it->second};
    if (!market.ready) return;
    if (market.halt != FlowMeshRuntimeHalt::NONE &&
        queued.message.kind != flowmesh::WireMessageKind::GET) {
        return;
    }
    switch (queued.message.kind) {
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
        break; // subscription negotiation belongs to the NodeContext/net glue
    }
}

void FlowMeshRuntime::ProcessTick()
{
    std::lock_guard<std::mutex> lock{m_market_mutex};
    for (auto& [market_id, market_ptr] : m_markets) {
        (void)market_id;
        Market& market{*market_ptr};
        if (!RefreshMarker(market)) continue;
        const auto now{market.clock->Now()};
        if (!market.pending_handoff &&
            now >= market.round_started + market.round_timeout) {
            if (market.round == std::numeric_limits<uint32_t>::max()) {
                HaltMarket(market, FlowMeshRuntimeHalt::SIGNING_CONFLICT,
                           "FlowMesh proposer round space is exhausted");
                continue;
            }
            ++market.round;
            market.round_started = now;
        }
        MaybePropose(market);
    }
}

void FlowMeshRuntime::ProcessCatchupCommand(
    const CatchupCommand& command)
{
    std::lock_guard<std::mutex> lock{m_market_mutex};
    const auto it{m_markets.find(command.market_id)};
    if (it == m_markets.end()) return;
    Market& market{*it->second};
    if (!market.ready || market.halt != FlowMeshRuntimeHalt::NONE) return;
    const auto key{std::make_pair(command.peer, command.market_id)};
    if (m_pending_catchup.count(key) != 0) return;
    constexpr uint16_t MAX_ENTRIES{
        static_cast<uint16_t>(flowmesh::FLOWMESH_CATCHUP_MAX_ENTRIES)};
    constexpr uint32_t MAX_BYTES{
        static_cast<uint32_t>(flowmesh::FLOWMESH_CATCHUP_MAX_BYTES)};
    if (!m_catchup_tracker.Begin(command.peer, command.market_id,
                                 market.next_sequence, MAX_ENTRIES,
                                 MAX_BYTES)) {
        return;
    }
    const auto payload{flowmesh::EncodeCatchupRequest(MAX_ENTRIES, MAX_BYTES)};
    if (!payload) return;
    m_pending_catchup.emplace(
        key, PendingCatchup{market.seats.epoch, market.next_sequence,
                            MAX_ENTRIES, MAX_BYTES});
    flowmesh::WireMessage request;
    request.kind = flowmesh::WireMessageKind::GET;
    request.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1,
                      market.market_id, market.seats.epoch,
                      market.next_sequence};
    request.payload = *payload;
    RelayMessage(market, std::move(request), command.peer, std::nullopt);
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
    if (!action || !market.pool.Add(*action, peer)) return;
    RelayMessage(market, message, std::nullopt,
                 peer == LOCAL_ACTION_PEER
                     ? std::nullopt
                     : std::optional<flowmesh::WirePeerId>{peer});
}

void FlowMeshRuntime::MaybePropose(Market& market)
{
    if (market.halt != FlowMeshRuntimeHalt::NONE || !market.ready ||
        market.pending_handoff || !RecheckAnchors(market)) {
        return;
    }
    const auto transition{CurrentSeatTransition(market)};
    if (!transition ||
        transition->kind == FlowMeshSeatTransitionKind::PAUSED) {
        return;
    }
    const auto local_keys{LocalSeatKeys(market)};
    const uint32_t proposer_index{flowmesh::ProductionProposerSeatIndex(
        market.next_sequence, market.round, market.seats.Size())};
    const auto proposer{std::find_if(
        local_keys.begin(), local_keys.end(), [&](const auto& item) {
            return item.first == proposer_index;
        })};
    if (proposer == local_keys.end()) return;

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

    Market::Candidate* candidate{nullptr};
    if (locked_hash) {
        const auto it{market.candidates.find(*locked_hash)};
        if (it == market.candidates.end()) return;
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
            if (!settlement_plan) return;
            anchor = settlement_plan->anchor;
        }
        if (!market.chain->Acceptable(anchor)) return;

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
            auto built{flowmesh::BuildProductionExecutionEntry(
                market.state, market.domain, market.market_id, market.seats,
                gate, market.next_sequence, market.next_effect_index,
                market.last_hash, anchor, AnchorContext(market),
                market.treasury_owner_commitment,
                selected_actions,
                market.deposits, check)};
            // A connected type-9 withdrawal always wins the next slot. The
            // engine reports the collision with queued user actions; retry
            // once with the required dedicated empty-action settlement.
            if (!built && !selected_actions.empty() &&
                check == flowmesh::ProductionEntryCheck::CHAIN_SETTLEMENT_MISMATCH) {
                selected_actions.clear();
                built = flowmesh::BuildProductionExecutionEntry(
                    market.state, market.domain, market.market_id,
                    market.seats, gate, market.next_sequence,
                    market.next_effect_index, market.last_hash, anchor,
                    AnchorContext(market), market.treasury_owner_commitment,
                    selected_actions, market.deposits, check);
            }
            // Do not manufacture empty traffic. Sequence zero and a nonempty
            // chain-derived settlement are the only actionless executions.
            if (built && market.next_sequence != 0 &&
                built->entry.actions.empty() && built->settlements.empty()) {
                return;
            }
            if (built) {
                entry = built->entry;
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

    if (!RetainCandidateBeforeSigning(market, *candidate)) return;

    if (candidate->reannounce_evidence) {
        for (const flowmesh::Action& action : candidate->evidence) {
            const auto payload{flowmesh::EncodeProductionActionPayload(action)};
            if (!payload) {
                HaltMarket(market, FlowMeshRuntimeHalt::STORE_FAILURE,
                           "retained FlowMesh action evidence is not encodable");
                return;
            }
            flowmesh::WireMessage evidence;
            evidence.kind = flowmesh::WireMessageKind::ACTION;
            evidence.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1,
                               market.market_id, market.seats.epoch,
                               market.next_sequence};
            evidence.payload = *payload;
            RelayMessage(market, std::move(evidence), std::nullopt,
                         std::nullopt);
        }
        candidate->reannounce_evidence = false;
    }

    flowmesh::ProductionSigningGuard guard{*market.store};
    flowmesh::ProductionProposalCheck check;
    const auto proposal{flowmesh::SignProductionProposal(
        proposer->second, candidate->entry, market.round, market.seats,
        guard, check)};
    if (!proposal) {
        if (check == flowmesh::ProductionProposalCheck::LOCK_CONFLICT ||
            check == flowmesh::ProductionProposalCheck::LOCK_STORAGE_FAILURE) {
            HaltMarket(market, FlowMeshRuntimeHalt::SIGNING_CONFLICT,
                       check == flowmesh::ProductionProposalCheck::LOCK_CONFLICT
                           ? "permanent FlowMesh proposal lock conflicts"
                           : "permanent FlowMesh proposal lock failed");
        }
        return;
    }
    const auto payload{flowmesh::EncodeProductionProposalPayload(*proposal)};
    if (!payload) return;
    flowmesh::WireMessage wire;
    wire.kind = flowmesh::WireMessageKind::PROPOSAL;
    wire.header = HeaderFor(proposal->entry);
    wire.payload = *payload;
    RelayMessage(market, wire, std::nullopt, std::nullopt);
    HandleProposal(market, LOCAL_ACTION_PEER, wire);
}

void FlowMeshRuntime::HandleProposal(
    Market& market, const flowmesh::WirePeerId peer,
    const flowmesh::WireMessage& message)
{
    const auto proposal{
        flowmesh::DecodeProductionProposalPayload(message.payload)};
    if (!proposal ||
        !flowmesh::ProductionWireHeaderMatches(message.header,
                                               proposal->entry)) {
        return;
    }

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
            RequestCatchup(peer, market.market_id);
        }
        return;
    }

    const bool current_or_next_round{
        proposal->round == market.round ||
        (market.round != std::numeric_limits<uint32_t>::max() &&
         proposal->round == market.round + 1)};
    if (proposal->entry.sequence != market.next_sequence ||
        !current_or_next_round ||
        flowmesh::CheckProductionProposal(
            *proposal, market.domain, market.market_id, market.seats.epoch,
            proposal->round, market.seats) !=
            flowmesh::ProductionProposalCheck::OK) {
        return;
    }
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
        return;
    }
    const uint256 hash{proposal->entry.GetHash()};
    auto candidate_it{market.candidates.find(hash)};
    if (candidate_it == market.candidates.end()) {
        if (market.candidates.size() >= MAX_RUNTIME_CANDIDATES_PER_SEQUENCE) {
            return;
        }
        const auto evidence{market.pool.EvidenceFor(proposal->entry.actions)};
        if (!evidence) return;
        auto candidate{EvaluateCandidate(market, proposal->entry, &*evidence)};
        if (!candidate) return;
        candidate_it = market.candidates.emplace(hash,
                                                  std::move(*candidate)).first;
    }

    // Round timeouts are local policy, so independently installed markets can
    // be one round apart. A fully validated proposal from the authenticated
    // next-round proposer safely reunites them. Never accept a larger jump:
    // an otherwise valid Byzantine proposer must not exhaust the round space.
    if (proposal->round > market.round) {
        market.round = proposal->round;
        market.round_started = market.clock->Now();
    }

    const auto local_keys{LocalSeatKeys(market)};
    if (!local_keys.empty() &&
        !RetainCandidateBeforeSigning(market, candidate_it->second)) {
        return;
    }
    flowmesh::ProductionSigningGuard guard{*market.store};
    for (const auto& [seat_index, key] : local_keys) {
        if (market.attestations[hash].count(seat_index) != 0) continue;
        flowmesh::ProductionLockResult lock;
        const auto attestation{flowmesh::SignProductionEntryAttestation(
            key, seat_index, candidate_it->second.entry, market.seats, guard,
            lock)};
        if (!attestation) {
            if (lock == flowmesh::ProductionLockResult::CONFLICT ||
                lock == flowmesh::ProductionLockResult::STORAGE_FAILURE) {
                HaltMarket(market, FlowMeshRuntimeHalt::SIGNING_CONFLICT,
                           lock == flowmesh::ProductionLockResult::CONFLICT
                               ? "permanent FlowMesh attestation lock conflicts"
                               : "permanent FlowMesh attestation lock failed");
            }
            return;
        }
        market.attestations[hash].emplace(seat_index, *attestation);
        market.attested_hash_by_seat.emplace(seat_index, hash);
        const auto payload{
            flowmesh::EncodeProductionAttestationPayload(*attestation)};
        if (!payload) continue;
        flowmesh::WireMessage wire;
        wire.kind = flowmesh::WireMessageKind::ATTESTATION;
        wire.header = HeaderFor(candidate_it->second.entry);
        wire.payload = *payload;
        RelayMessage(market, std::move(wire), std::nullopt, std::nullopt);
    }
    MaybeCertify(market, hash);
}

void FlowMeshRuntime::HandleAttestation(
    Market& market, const flowmesh::WirePeerId peer,
    const flowmesh::WireMessage& message)
{
    (void)peer;
    if (message.header.market_id != market.market_id ||
        message.header.epoch != market.seats.epoch ||
        message.header.sequence != market.next_sequence) {
        return;
    }
    const auto attestation{
        flowmesh::DecodeProductionAttestationPayload(message.payload)};
    if (!attestation || attestation->seat_index >= market.seats.Size()) return;

    std::optional<uint256> matching_hash;
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
    if (!matching_hash) return;
    const auto prior{
        market.attested_hash_by_seat.find(attestation->seat_index)};
    if (prior != market.attested_hash_by_seat.end() &&
        prior->second != *matching_hash) {
        return;
    }
    auto& by_seat{market.attestations[*matching_hash]};
    const auto existing{by_seat.find(attestation->seat_index)};
    if (existing != by_seat.end()) return;
    by_seat.emplace(attestation->seat_index, *attestation);
    market.attested_hash_by_seat.emplace(attestation->seat_index,
                                         *matching_hash);
    MaybeCertify(market, *matching_hash);
}

void FlowMeshRuntime::MaybeCertify(Market& market,
                                   const uint256& candidate_hash)
{
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
    if (flowmesh::AssembleProductionEntryCertificate(
            candidate->second.entry, market.seats, partials, certificate) !=
        flowmesh::BlsCertificateAssemblyCheck::OK) {
        return;
    }
    const flowmesh::ProductionCertifiedEnvelope certified{
        candidate->second.entry, certificate};
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
    const flowmesh::WireMessage& message, const bool from_catchup)
{
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
        market.pending_handoff) {
        return;
    }

    const uint256 hash{certified->entry.GetHash()};
    auto candidate_it{market.candidates.find(hash)};
    if (candidate_it == market.candidates.end()) {
        auto candidate{EvaluateCandidate(market, certified->entry, nullptr)};
        if (!candidate) return;
        candidate_it = market.candidates.emplace(hash,
                                                  std::move(*candidate)).first;
    }
    if (!CommitCertified(market, *certified, candidate_it->second)) return;
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
    if (pending == m_pending_catchup.end() ||
        message.header.market_id != market.market_id ||
        message.header.epoch != pending->second.epoch ||
        message.header.sequence != pending->second.from_sequence) {
        return; // unsolicited or for a different request
    }
    const auto entries{flowmesh::DecodeCatchupEntries(message.payload)};
    if (!entries || !m_catchup_tracker.AcceptResponse(
                        peer, market.market_id, message.header.sequence,
                        entries ? entries->size() : 0,
                        message.payload.size())) {
        RemovePeerOnWorker(peer);
        return;
    }
    const PendingCatchup request{pending->second};
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
        HandleCertificate(market, peer, certified,
                          /*from_catchup=*/true);
        if (market.halt != FlowMeshRuntimeHalt::NONE ||
            market.next_sequence != expected + 1) {
            break;
        }
        ++expected;
    }
    if (market.halt == FlowMeshRuntimeHalt::NONE &&
        entries->size() == request.max_entries) {
        RequestCatchup(peer, market.market_id);
    }
}

} // namespace node
