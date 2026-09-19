// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_runtime.h>

#include <flowmesh/auth.h>
#include <flowmesh/agreement_wire.h>
#include <hash.h>
#include <test/util/flowmesh.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {

node::FlowMeshRelayResult LegacyRelayResult()
{
    return {.no_peer_reason = node::FlowMeshDeliveryAdmission::LEGACY_UNTRACKED};
}

uint256 Filled(const unsigned char value)
{
    uint256 out;
    std::fill(out.begin(), out.end(), value);
    return out;
}

bls::SecretKey Key(const uint32_t index, const unsigned char salt)
{
    std::array<unsigned char, 32> ikm{};
    for (size_t i{0}; i < ikm.size(); ++i) {
        ikm[i] = static_cast<unsigned char>(salt + index * 17 + i * 11);
    }
    const auto key{bls::SecretKey::FromIKM(ikm)};
    BOOST_REQUIRE(key);
    return *key;
}

struct SeatFixture {
    std::vector<bls::SecretKey> secrets;
    flowmesh::ActiveFnBlsSeatSet seats;
};

SeatFixture Seats(const uint256& domain, const flowmesh::MarketId& market,
                  const size_t count, const uint64_t epoch,
                  const uint64_t anchor_height, const uint256& anchor_hash,
                  const unsigned char salt)
{
    struct Entry {
        bls::SecretKey secret;
        flowmesh::BlsSeatBinding binding;
        flowmesh::SeatId id;
    };
    std::vector<Entry> entries;
    for (size_t i{0}; i < count; ++i) {
        const bls::SecretKey secret{Key(i, salt)};
        flowmesh::BlsSeatBinding binding;
        binding.outpoint = COutPoint{
            Txid::FromUint256(Filled(static_cast<unsigned char>(salt + i + 40))),
            static_cast<uint32_t>(salt + i)};
        binding.public_key = secret.GetPublicKey().Compressed();
        binding.proof_of_possession = secret.SignPoP().Compressed();
        entries.push_back({secret, binding,
                           flowmesh::ComputeFlowMeshSeatId(domain,
                                                           binding.outpoint)});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.id < b.id ||
               (a.id == b.id && a.binding.outpoint < b.binding.outpoint);
    });
    SeatFixture out;
    std::vector<flowmesh::BlsSeatBinding> bindings;
    for (const Entry& entry : entries) {
        out.secrets.push_back(entry.secret);
        bindings.push_back(entry.binding);
    }
    flowmesh::BlsSeatSetCheck check;
    const auto seats{flowmesh::BuildActiveFnBlsSeatSet(
        domain, market, epoch, anchor_height, anchor_hash, bindings, check)};
    BOOST_REQUIRE(seats);
    BOOST_REQUIRE(check == flowmesh::BlsSeatSetCheck::OK);
    out.seats = *seats;
    return out;
}

class RuntimeChain final : public node::FlowMeshRuntimeChain
{
public:
    void SetReconciled(const bool reconciled) { m_reconciled = reconciled; }

    uint64_t DeliveryGeneration() const override { return m_delivery_generation.load(); }

    void ReconcileOnceWhileChecking(const flowmesh::AnchorRef& anchor)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_reconcile_once_anchor = anchor;
    }

    int32_t TipHeight() const override
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        return m_tip_height;
    }

    bool Acceptable(const flowmesh::AnchorRef& anchor) const override
    {
        if (!m_reconciled.load()) return false;
        {
            std::lock_guard<std::mutex> lock{m_mutex};
            if (m_reconcile_once_anchor && *m_reconcile_once_anchor == anchor) {
                m_reconcile_once_anchor.reset();
                ++m_delivery_generation;
                // One in-handler check sees reconciliation. It has already
                // finished when the caller next samples the live gate.
                return false;
            }
        }
        const int32_t tip_height{TipHeight()};
        return StillCanonical(anchor) && anchor.height <= tip_height &&
               tip_height - anchor.height >=
                   flowmesh::FLOWMESH_PRODUCTION_MIN_ANCHOR_DEPTH;
    }

    bool StillCanonical(const flowmesh::AnchorRef& anchor) const override
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        const auto it{m_canonical.find(anchor.height)};
        return it != m_canonical.end() && it->second == anchor.hash;
    }

    flowmesh::AnchorRef Current() const override { return m_current; }

    std::optional<flowmesh::ActiveFnBlsSeatSet> SeatSet(
        const uint256& domain, const flowmesh::MarketId& market,
        const uint64_t epoch, const uint256& set_hash) const override
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        const auto it{m_sets.find({market, epoch})};
        if (domain != m_domain || it == m_sets.end() ||
            it->second.set_hash != set_hash) {
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<flowmesh::ActiveFnBlsSeatSet> SeatSetForSequence(
        const uint256& domain, const flowmesh::MarketId& market,
        const uint64_t sequence) const override
    {
        (void)sequence;
        std::lock_guard<std::mutex> lock{m_mutex};
        if (domain != m_domain) return std::nullopt;
        for (const auto& [key, seats] : m_sets) {
            if (key.first == market) return seats;
        }
        return std::nullopt;
    }

    node::FlowMeshSeatTransition SeatTransition(
        const uint256& domain, const flowmesh::MarketId& market,
        const flowmesh::ActiveFnBlsSeatSet& current) const override
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        if (!m_reconciled.load() || domain != m_domain || market != current.market_id) {
            return {node::FlowMeshSeatTransitionKind::PAUSED, std::nullopt};
        }
        if (m_pause_after_checks) {
            if (*m_pause_after_checks == 0) {
                return {node::FlowMeshSeatTransitionKind::PAUSED, std::nullopt};
            }
            --*m_pause_after_checks;
        }
        const auto it{m_transitions.find(market)};
        return it == m_transitions.end() ? node::FlowMeshSeatTransition{}
                                         : it->second;
    }

    std::optional<node::FlowMeshRuntimeConnectedHandoff>
    ConnectedHandoffCheckpoint(
        const uint256& domain, const flowmesh::MarketId& market,
        const uint256& microblock_hash) const override
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        const auto it{m_connected_handoffs.find(microblock_hash)};
        if (domain != m_domain || it == m_connected_handoffs.end() ||
            it->second.core.market_id != market) {
            return std::nullopt;
        }
        return it->second;
    }

    void Add(const flowmesh::ActiveFnBlsSeatSet& seats)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_sets[{seats.market_id, seats.epoch}] = seats;
    }

    void SetTransition(const flowmesh::MarketId& market,
                       const node::FlowMeshSeatTransitionKind kind)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_transitions[market] = {kind, std::nullopt};
    }

    void PauseAfterTransitionChecks(const size_t checks)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_pause_after_checks = checks;
    }

    void SetHandoff(const flowmesh::MarketId& market,
                    const flowmesh::ActiveFnBlsSeatSet& next)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_transitions[market] = {
            node::FlowMeshSeatTransitionKind::HANDOFF, next};
    }

    void SetTipHeight(const int32_t height)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_tip_height = height;
    }

    void AddCanonical(const flowmesh::AnchorRef& anchor)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_canonical[anchor.height] = anchor.hash;
    }

    void ConnectHandoff(
        const uint256& microblock_hash,
        node::FlowMeshRuntimeConnectedHandoff connected)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_canonical[connected.connection.height] =
            connected.connection.block_hash;
        m_connected_handoffs.insert_or_assign(microblock_hash,
                                               std::move(connected));
    }

    void DisconnectHandoff(const uint256& microblock_hash,
                           const int32_t height,
                           const uint256& replacement_block)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_connected_handoffs.erase(microblock_hash);
        m_canonical[height] = replacement_block;
    }

    uint256 m_domain;
    flowmesh::AnchorRef m_current{200, Filled(0x74)};
    std::map<int32_t, uint256> m_canonical{{100, Filled(0x71)},
                                           {200, Filled(0x74)}};

private:
    std::atomic<bool> m_reconciled{true};
    mutable std::atomic<uint64_t> m_delivery_generation{0};
    mutable std::optional<flowmesh::AnchorRef> m_reconcile_once_anchor;
    mutable std::mutex m_mutex;
    int32_t m_tip_height{260};
    mutable std::optional<size_t> m_pause_after_checks;
    std::map<std::pair<flowmesh::MarketId, uint64_t>,
             flowmesh::ActiveFnBlsSeatSet> m_sets;
    std::map<flowmesh::MarketId, node::FlowMeshSeatTransition> m_transitions;
    std::map<uint256, node::FlowMeshRuntimeConnectedHandoff>
        m_connected_handoffs;
};

class RuntimeKeys final : public node::FlowMeshRuntimeKeyProvider
{
public:
    std::vector<bls::SecretKey> LocalSeatKeys(
        const flowmesh::MarketId& market,
        const flowmesh::ActiveFnBlsSeatSet& seats) const override
    {
        if (m_visibility_chain && !m_visibility_chain->Acceptable(m_visibility_chain->Current())) return {};
        (void)seats;
        const auto it{m_keys.find(market)};
        return it == m_keys.end() ? std::vector<bls::SecretKey>{}
                                  : it->second;
    }

    std::map<flowmesh::MarketId, std::vector<bls::SecretKey>> m_keys;
    // Optional production-service behavior: configured keys are masked while
    // B3 reconciliation is in progress, without actually disarming them.
    RuntimeChain* m_visibility_chain{nullptr};
};

class RuntimeSeatSource final : public node::ProductionSeatSetSource
{
public:
    std::optional<flowmesh::ActiveFnBlsSeatSet> GetSeatSet(
        const uint256& domain, const flowmesh::MarketId& market,
        const uint64_t epoch, const uint256& seat_set_hash) const override
    {
        if (domain != m_domain || market != m_seats.market_id ||
            epoch != m_seats.epoch || seat_set_hash != m_seats.set_hash) {
            return std::nullopt;
        }
        return m_seats;
    }

    uint256 m_domain;
    flowmesh::ActiveFnBlsSeatSet m_seats;
};

class FixedClock final : public node::FlowMeshRuntimeClock
{
public:
    flowmesh::WireClock::time_point Now() const override { return m_now; }
    flowmesh::WireClock::time_point m_now{
        flowmesh::WireClock::time_point{std::chrono::seconds{100}}};
};

class MapDeposits final : public flowmesh::DepositVerifier
{
public:
    std::optional<flowmesh::DepositInfo> GetDeposit(
        const COutPoint& outpoint,
        const flowmesh::AnchorRef& anchor) const override
    {
        if (anchor != required_anchor && (!additional_anchor || anchor != *additional_anchor)) return std::nullopt;
        const auto it{entries.find(outpoint)};
        return it == entries.end()
                   ? std::nullopt
                   : std::optional<flowmesh::DepositInfo>{it->second};
    }

    std::optional<std::vector<flowmesh::WithdrawalSettlementFactV1>>
    GetWithdrawalSettlements(
        const std::optional<flowmesh::AnchorRef>&,
        const flowmesh::AnchorRef&) const override
    {
        return std::vector<flowmesh::WithdrawalSettlementFactV1>{};
    }

    std::optional<CAmount> GetWithdrawalCapacity(
        const modern::AssetId&, const flowmesh::AnchorRef& anchor) const override
    {
        return anchor == required_anchor
                   ? std::optional<CAmount>{MAX_MONEY}
                   : std::nullopt;
    }

    flowmesh::AnchorRef required_anchor;
    std::optional<flowmesh::AnchorRef> additional_anchor;
    std::map<COutPoint, flowmesh::DepositInfo> entries;
};

flowmesh::Action Deposit(const COutPoint& outpoint)
{
    flowmesh::Action action;
    action.type = static_cast<uint8_t>(flowmesh::ActionType::DEPOSIT);
    action.outpoint = outpoint;
    return action;
}

flowmesh::BlsMicroblockCertificate Certify(
    const flowmesh::ProductionEntryCore& entry, const SeatFixture& fixture)
{
    std::vector<flowmesh::IndexedBlsSignature> signatures;
    const auto context{flowmesh::ProductionCertificateContext(entry)};
    for (uint32_t i{0};
         i < flowmesh::FlowMeshBlsThreshold(fixture.seats.Size()); ++i) {
        const auto signature{flowmesh::SignBlsMicroblockCertificate(
            fixture.secrets[i], context, fixture.seats)};
        BOOST_REQUIRE(signature);
        signatures.push_back({i, *signature});
    }
    flowmesh::BlsMicroblockCertificate certificate;
    BOOST_REQUIRE(flowmesh::AssembleProductionEntryCertificate(
                      entry, fixture.seats, signatures, certificate) ==
                  flowmesh::BlsCertificateAssemblyCheck::OK);
    return certificate;
}

class RuntimeNetwork
{
public:
    explicit RuntimeNetwork(size_t count = 4) : m_nodes(count, nullptr), m_online(count, false), m_get_requests(count, 0) {}
    using Filter = std::function<bool(size_t, size_t, const flowmesh::WireMessage&)>;
    void SetFilter(Filter filter)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_filter = std::move(filter);
    }

    void PartitionHalves(const bool partitioned)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_partitioned = partitioned;
    }

    void IgnoreHellos()
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_ignore_hellos = true;
    }

    size_t GetRequests(const size_t from)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        return m_get_requests.at(from);
    }

    void Set(const size_t id, node::FlowMeshRuntime* runtime,
             const bool online)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_nodes[id] = runtime;
        m_online[id] = online;
    }

    void Relay(const size_t from, node::FlowMeshRuntimeRelay relay)
    {
        std::vector<std::pair<size_t, node::FlowMeshRuntime*>> targets;
        Filter filter;
        {
            std::lock_guard<std::mutex> lock{m_mutex};
            filter = m_filter;
            if (relay.message.kind == flowmesh::WireMessageKind::GET) ++m_get_requests.at(from);
            if (m_ignore_hellos && relay.message.kind == flowmesh::WireMessageKind::HELLO) return;
            if (relay.peer) {
                const size_t target{static_cast<size_t>(*relay.peer)};
                if (target < m_nodes.size() && m_online[target] &&
                    m_nodes[target] != nullptr) {
                    targets.emplace_back(target, m_nodes[target]);
                }
            } else {
                for (size_t i{0}; i < m_nodes.size(); ++i) {
                    if (i == from || !m_online[i] || m_nodes[i] == nullptr ||
                        (relay.exclude_peer &&
                         *relay.exclude_peer == static_cast<int64_t>(i))) {
                        continue;
                    }
                    targets.emplace_back(i, m_nodes[i]);
                }
            }
            if (m_partitioned) {
                std::erase_if(targets, [from](const auto& target) {
                    return (from < 2) != (target.first < 2);
                });
            }
        }
        for (const auto& [id, runtime] : targets) {
            if (filter && !filter(from, id, relay.message)) continue;
            runtime->EnqueueWireMessage(static_cast<int64_t>(from),
                                        relay.message);
        }
    }

private:
    std::mutex m_mutex;
    std::vector<node::FlowMeshRuntime*> m_nodes;
    std::vector<bool> m_online;
    bool m_ignore_hellos{false};
    bool m_partitioned{false};
    Filter m_filter;
    std::vector<size_t> m_get_requests;
};

/**
 * Three-node star used to prove proposal retry recovers one lost vote.
 * Directional loss also suppresses forwarded non-proposer votes from the
 * hub to leaves, so neither voter can assemble a side certificate. Node 2's
 * first own broadcast vote is dropped; its later targeted reply must be the
 * exact cached payload. Transport sender is not necessarily signing seat.
 */
class RetryAttestationNetwork
{
public:
    void Set(const size_t id, node::FlowMeshRuntime* runtime)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_nodes.at(id) = runtime;
    }

    void Relay(const size_t from, node::FlowMeshRuntimeRelay relay)
    {
        std::vector<std::pair<size_t, node::FlowMeshRuntime*>> targets;
        {
            std::lock_guard<std::mutex> lock{m_mutex};
            if (relay.peer) {
                const size_t target{static_cast<size_t>(*relay.peer)};
                if (target < m_nodes.size() && m_nodes[target] != nullptr) {
                    targets.emplace_back(target, m_nodes[target]);
                }
            } else {
                for (size_t i{0}; i < m_nodes.size(); ++i) {
                    if (i == from || m_nodes[i] == nullptr ||
                        (relay.exclude_peer &&
                         *relay.exclude_peer == static_cast<int64_t>(i))) {
                        continue;
                    }
                    targets.emplace_back(i, m_nodes[i]);
                }
            }
            // Every message obeys physical star edges and origin exclusion,
            // including proposals now forwarded by a leaf. No leaf-to-leaf
            // shortcut may manufacture an extra cached-reply opportunity.
            std::erase_if(targets, [&](const auto& target) {
                return target.first == from || (from != 0 && target.first != 0) ||
                       (relay.exclude_peer && *relay.exclude_peer ==
                            static_cast<flowmesh::WirePeerId>(target.first));
            });
            if (targets.empty()) return;
            if (relay.message.kind == flowmesh::WireMessageKind::ATTESTATION) {
                const auto vote{flowmesh::DecodeProductionAttestationPayload(relay.message.payload)};
                if (!vote) return;
                // Keep the intended lost-vote experiment isolated even now
                // that the hub correctly gossips already verified shares.
                if (from == 0 && vote->seat_index != 0) return;
                if (from == 2 && vote->seat_index == 2) {
                    if (!m_dropped_first) {
                        m_dropped_first = relay.message.payload;
                        return;
                    }
                    if (relay.peer == 0) m_targeted_retry = relay.message.payload;
                }
            }
        }
        for (const auto& [id, runtime] : targets) {
            (void)id;
            runtime->EnqueueWireMessage(static_cast<int64_t>(from),
                                        relay.message);
        }
    }

    bool DroppedFirstAttestation() const
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        return m_dropped_first.has_value();
    }

    bool RetriedExactCachedAttestation() const
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        return m_dropped_first && m_targeted_retry &&
               *m_dropped_first == *m_targeted_retry;
    }

private:
    mutable std::mutex m_mutex;
    std::array<node::FlowMeshRuntime*, 3> m_nodes{};
    std::optional<std::vector<unsigned char>> m_dropped_first;
    std::optional<std::vector<unsigned char>> m_targeted_retry;
};

bool WaitUntil(const std::function<bool()>& predicate)
{
    for (size_t i{0}; i < 500; ++i) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
}

node::FlowMeshRuntimeMarketConfig MarketConfig(
    const uint256& domain, const flowmesh::MarketId& market,
    const uint256& treasury, const flowmesh::ActiveFnBlsSeatSet& seats,
    const flowmesh::FlowMeshState& state,
    node::FlowMeshProductionStore& store,
    const flowmesh::DepositVerifier* deposits)
{
    return node::FlowMeshRuntimeMarketConfig{
        .domain = domain,
        .market_id = market,
        .treasury_owner_commitment = treasury,
        .active_seats = seats,
        .state = state,
        .store = &store,
        .deposits = deposits,
    };
}

/** Real runtime/real BLS/real durable stores, with only the existing mock
 * canonical-chain oracle, deterministic clock, and in-memory transport. */
template <size_t Count = 4>
class PreagreementRuntimeHarness
{
public:
    explicit PreagreementRuntimeHarness(fs::path directory) : path{std::move(directory)}
    {
        for (auto& chain : chains) {
            chain.m_domain = domain;
            chain.Add(seats.seats);
            chain.SetTipHeight(chain.Current().height + flowmesh::FLOWMESH_PRODUCTION_MIN_ANCHOR_DEPTH);
        }
        deposits.required_anchor = chains[0].Current();
        deposits.entries.emplace(outpoint, flowmesh::DepositInfo{asset, 250, account});
        network.IgnoreHellos();
        network.SetFilter([this](size_t, size_t, const flowmesh::WireMessage& wire) {
            if (wire.kind != flowmesh::WireMessageKind::AGREEMENT) return true;
            const auto message{flowmesh::DecodeAgreementMessage(wire.payload)};
            if (!message) return false;
            if (message->stage == flowmesh::AgreementStage::PREPARE) ++prepares;
            if (message->stage == flowmesh::AgreementStage::COMMIT) ++commits;
            if (message->stage == flowmesh::AgreementStage::VIEW_CHANGE) ++view_changes;
            if (message->stage == flowmesh::AgreementStage::DECISION) ++decisions;
            return !block_commits || (message->stage != flowmesh::AgreementStage::COMMIT &&
                                      message->stage != flowmesh::AgreementStage::DECISION);
        });
        std::string error;
        try {
            for (size_t i{0}; i < runtimes.size(); ++i) {
                keys[i].m_keys[market] = {seats.secrets[i]};
                stores[i] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
                    .path = StorePath(i), .cache_bytes = size_t{1} << 20}, true);
                BOOST_REQUIRE_MESSAGE(stores[i]->OpenForMarket(domain, market, seats.seats, initial.Root(), error), error);
                BOOST_REQUIRE_MESSAGE(StartNode(i, AgreementPath(i), std::nullopt, error), error);
            }
        } catch (...) {
            for (size_t i{0}; i < runtimes.size(); ++i) network.Set(i, nullptr, false);
            for (const auto& runtime : runtimes) if (runtime) runtime->Stop();
            throw;
        }
    }

    ~PreagreementRuntimeHarness()
    {
        StopAll();
    }

    void StopAll()
    {
        for (size_t i{0}; i < runtimes.size(); ++i) network.Set(i, nullptr, false);
        for (const auto& runtime : runtimes) if (runtime) runtime->Stop();
    }

    fs::path StorePath(size_t i) const { return path / fs::PathFromString("operator_" + std::to_string(i)); }
    fs::path AgreementPath(size_t i) const { return StorePath(i) / "agreement"; }

    bool StartNode(size_t i, const fs::path& journal, std::optional<bool> allow_override, std::string& error)
    {
        auto& chain{chains[i]};
        std::optional<node::FlowMeshProductionStore::Marker> marker;
        if (!stores[i]->ReadMarker(marker, error) || !marker) return false;
        auto restored{initial};
        if (marker->next_sequence) {
            RuntimeSeatSource authority;
            authority.m_domain = domain;
            authority.m_seats = seats.seats;
            uint256 last;
            if (!stores[i]->Replay(restored, last, authority,
                {chain.TipHeight(), std::nullopt, &chain}, treasury, &deposits, error)) return false;
        }
        auto market_config{MarketConfig(domain, market, treasury, seats.seats, restored, *stores[i], &deposits)};
        market_config.next_sequence = marker->next_sequence;
        market_config.next_effect_index = marker->next_effect_index;
        market_config.last_microblock_hash = marker->last_microblock_hash;
        market_config.preagreement = true;
        market_config.agreement_path = journal;
        market_config.agreement_identity = marker->agreement_identity;
        market_config.agreement_allow_create = allow_override.value_or(!marker->agreement_bootstrap_complete);
        node::FlowMeshRuntimeConfig config;
        config.chain = &chain;
        config.keys = &keys[i];
        config.clock = &clocks[i];
        config.round_timeout = std::chrono::seconds{10};
        config.relay = [this, i](node::FlowMeshRuntimeRelay relay) {
            if (block_commits && (relay.message.kind == flowmesh::WireMessageKind::ATTESTATION ||
                                 relay.message.kind == flowmesh::WireMessageKind::PROPOSAL)) ++premature_v1;
            network.Relay(i, std::move(relay));
            // This transport admits an in-memory legacy delivery, not a claim
            // of peer verification. No socket-completion scheduler is used.
            node::FlowMeshRelayResult result;
            for (size_t j{0}; j < Count; ++j) if (i != j) {
                result.peers.push_back({static_cast<int64_t>(j), node::FlowMeshDeliveryAdmission::LEGACY_UNTRACKED, {}});
            }
            return result;
        };
        auto runtime{std::make_unique<node::FlowMeshRuntime>(config,
            std::vector<node::FlowMeshRuntimeMarketConfig>{std::move(market_config)})};
        if (!runtime->Start(error)) return false;
        runtimes[i] = std::move(runtime);
        network.Set(i, runtimes[i].get(), true);
        return true;
    }

    void StopNode(size_t i)
    {
        network.Set(i, nullptr, false);
        if (runtimes[i]) runtimes[i]->Stop();
        runtimes[i].reset();
    }

    void Drain()
    {
        for (size_t pass{0}; pass < 4; ++pass) for (const auto& runtime : runtimes) {
            if (runtime) BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{5}));
        }
    }

    void Tick(const std::chrono::milliseconds advance = std::chrono::milliseconds{0})
    {
        Drain(); // FixedClock is intentionally changed only while workers idle.
        for (auto& clock : clocks) clock.m_now += advance;
        for (const auto& runtime : runtimes) if (runtime) runtime->NotifyTick();
        Drain();
    }

    bool AllAt(uint64_t sequence) const
    {
        for (const auto& runtime : runtimes) {
            if (!runtime) continue;
            const auto status{runtime->MarketStatus(market)};
            if (!status || status->next_sequence != sequence || status->halt != node::FlowMeshRuntimeHalt::NONE) return false;
        }
        return true;
    }

    void Reach(uint64_t sequence)
    {
        for (size_t attempt{0}; attempt < 8 && !AllAt(sequence); ++attempt) Tick(std::chrono::seconds{2});
        BOOST_REQUIRE(AllAt(sequence));
    }

    const fs::path path;
    const uint256 domain{Filled(0x2e)};
    const modern::AssetId asset{Filled(0x4e)};
    const flowmesh::MarketId market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const flowmesh::VaultId vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x6e)};
    const flowmesh::AccountId account{Filled(0xae)};
    const COutPoint outpoint{Txid::FromUint256(Filled(0x8e)), 0};
    const SeatFixture seats{Seats(domain, market, Count, 0, 100, Filled(0x71), 143)};
    const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(), flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    std::array<RuntimeChain, Count> chains;
    MapDeposits deposits;
    std::array<FixedClock, Count> clocks;
    std::array<RuntimeKeys, Count> keys;
    std::array<std::unique_ptr<node::FlowMeshProductionStore>, Count> stores;
    std::atomic<bool> block_commits{true};
    std::atomic<size_t> prepares{0}, commits{0}, decisions{0}, view_changes{0}, premature_v1{0};
    RuntimeNetwork network{Count};
    std::array<std::unique_ptr<node::FlowMeshRuntime>, Count> runtimes;
};

/** One real signer in a four-seat committee: a proposal and a durable vote,
 * but never enough signatures to certify merely because a socket was written. */
class DeliveryRuntimeFixture
{
public:
    explicit DeliveryRuntimeFixture(const fs::path& path,
        const node::FlowMeshDeliveryAdmission admission, const bool proposer = true)
        : mode{admission}, store{DBParams{.path = path,
              .cache_bytes = size_t{1} << 20, .wipe_data = true}}
    {
        chain.m_domain = domain;
        chain.Add(seats.seats);
        keys.m_keys[market] = {seats.secrets[proposer ? 0 : 1]};
        std::string error;
        BOOST_REQUIRE_MESSAGE(store.OpenForMarket(
            domain, market, seats.seats, initial.Root(), error), error);
        node::FlowMeshRuntimeConfig config;
        config.chain = &chain;
        config.keys = &keys;
        config.clock = &clock;
        config.round_timeout = std::chrono::hours{1};
        config.relay = [this](node::FlowMeshRuntimeRelay relay) {
            if (relay.message.kind == flowmesh::WireMessageKind::GET) {
                std::lock_guard<std::mutex> guard{mutex};
                requests.push_back(relay);
            }
            if (relay.message.kind != flowmesh::WireMessageKind::PROPOSAL &&
                relay.message.kind != flowmesh::WireMessageKind::ATTESTATION &&
                relay.message.kind != flowmesh::WireMessageKind::CERTIFICATE) {
                return LegacyRelayResult();
            }
            {
                std::lock_guard<std::mutex> guard{mutex};
                sent.push_back(std::move(relay));
            }
            node::FlowMeshRelayResult result;
            result.no_peer_reason = mode.load();
            if (result.no_peer_reason == node::FlowMeshDeliveryAdmission::ADMITTED) {
                result.peers.push_back({PEER, node::FlowMeshDeliveryAdmission::ADMITTED, {}});
            }
            return result;
        };
        config.cancel_delivery = [this](const uint64_t id) {
            std::lock_guard<std::mutex> guard{mutex};
            cancelled.push_back(id);
        };
        runtime = std::make_unique<node::FlowMeshRuntime>(config,
            std::vector<node::FlowMeshRuntimeMarketConfig>{MarketConfig(
                domain, market, Filled(0x6b), seats.seats, initial, store, nullptr)});
        BOOST_REQUIRE_MESSAGE(runtime->Start(error), error);
        Tick();
        BOOST_REQUIRE_EQUAL(Sent().size(), proposer ? 2U : 0U);
    }

    void Tick(const std::chrono::milliseconds elapsed = std::chrono::milliseconds{0})
    {
        BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
        clock.m_now += elapsed;
        runtime->NotifyTick();
        BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
    }

    std::vector<node::FlowMeshRuntimeRelay> Sent()
    {
        std::lock_guard<std::mutex> guard{mutex};
        return sent;
    }

    std::vector<uint64_t> Cancelled()
    {
        std::lock_guard<std::mutex> guard{mutex};
        return cancelled;
    }

    std::vector<node::FlowMeshRuntimeRelay> Requests()
    {
        std::lock_guard<std::mutex> guard{mutex};
        return requests;
    }

    node::FlowMeshRuntimeDeliverySnapshot Snapshot()
    {
        const auto snapshots{runtime->DeliverySnapshots(market)};
        BOOST_REQUIRE_EQUAL(snapshots.size(), 1U);
        return snapshots.front();
    }

    void Complete(const node::FlowMeshRuntimeRelay& relay,
        const node::FlowMeshDeliveryOutcome outcome,
        const flowmesh::WirePeerId peer = PEER)
    {
        BOOST_REQUIRE(runtime->NotifyDeliveryEvent({relay.delivery_id, peer, outcome, {}}));
        BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
    }

    void CheckLock(const std::vector<node::FlowMeshRuntimeRelay>& original)
    {
        const auto proposal{std::find_if(original.begin(), original.end(), [](const auto& item) {
            return item.message.kind == flowmesh::WireMessageKind::PROPOSAL;
        })};
        BOOST_REQUIRE(proposal != original.end());
        const auto decoded{flowmesh::DecodeProductionProposalPayload(proposal->message.payload)};
        BOOST_REQUIRE(decoded);
        std::optional<uint256> locked;
        std::string error;
        BOOST_REQUIRE(store.ReadLock({seats.seats.epoch, 0}, locked, error));
        BOOST_REQUIRE(locked);
        BOOST_CHECK(*locked == decoded->entry.GetHash());
        std::optional<node::StoredLockedProductionCandidate> retained;
        BOOST_REQUIRE(store.ReadLockedCandidate({seats.seats.epoch, 0}, retained, error));
        BOOST_REQUIRE(retained);
        BOOST_CHECK(retained->entry.GetHash() == *locked);
        BOOST_CHECK_EQUAL(runtime->MarketStatus(market)->next_sequence, 0U);
        BOOST_CHECK(runtime->MarketStatus(market)->halt == node::FlowMeshRuntimeHalt::NONE);
    }

    static constexpr flowmesh::WirePeerId PEER{-2};
    const uint256 domain{Filled(0x2b)};
    const modern::AssetId asset{Filled(0x4b)};
    const flowmesh::MarketId market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const flowmesh::VaultId vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const SeatFixture seats{Seats(domain, market, 4, 7, 100, Filled(0x71), 131)};
    const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(),
                                        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    RuntimeChain chain;
    RuntimeKeys keys;
    FixedClock clock;
    std::atomic<node::FlowMeshDeliveryAdmission> mode;
    node::FlowMeshProductionStore store;
    std::mutex mutex;
    std::vector<node::FlowMeshRuntimeRelay> sent;
    std::vector<node::FlowMeshRuntimeRelay> requests;
    std::vector<uint64_t> cancelled;
    // Destroy the worker before any dependency or callback capture.
    std::unique_ptr<node::FlowMeshRuntime> runtime;
};

void CheckExactDeliveryRetry(const std::vector<node::FlowMeshRuntimeRelay>& original,
                            const std::vector<node::FlowMeshRuntimeRelay>& all,
                            const size_t offset)
{
    BOOST_REQUIRE_EQUAL(original.size(), 2U);
    BOOST_REQUIRE_EQUAL(all.size(), offset + original.size());
    for (const auto& before : original) {
        const auto after{std::find_if(all.begin() + offset, all.end(), [&](const auto& item) {
            return item.message.kind == before.message.kind;
        })};
        BOOST_REQUIRE(after != all.end());
        BOOST_CHECK_NE(before.delivery_id, 0U);
        BOOST_CHECK_GT(after->delivery_id, before.delivery_id);
        BOOST_CHECK(after->peer == before.peer);
        BOOST_CHECK(after->exclude_peer == before.exclude_peer);
        flowmesh::WireCheck check;
        const auto before_bytes{flowmesh::EncodeWireMessage(before.message, check)};
        const auto after_bytes{flowmesh::EncodeWireMessage(after->message, check)};
        BOOST_REQUIRE(before_bytes);
        BOOST_REQUIRE(after_bytes);
        // Covers the signed proposal, the exact BLS attestation, and envelope.
        BOOST_CHECK(*before_bytes == *after_bytes);
    }
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_runtime_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(delivery_trace_matches_real_returning_seat_only_after_remote_acceptance)
{
    using Kind = flowmesh::WireMessageKind;
    DeliveryRuntimeFixture proposer{m_args.GetDataDirBase() / "flowmesh_trace_proposer",
        node::FlowMeshDeliveryAdmission::ADMITTED};
    DeliveryRuntimeFixture returning{m_args.GetDataDirBase() / "flowmesh_trace_returning",
        node::FlowMeshDeliveryAdmission::ADMITTED, false};
    const auto originals{proposer.Sent()};
    const auto proposal{std::find_if(originals.begin(), originals.end(), [](const auto& relay) {
        return relay.message.kind == Kind::PROPOSAL;
    })};
    BOOST_REQUIRE(proposal != originals.end());
    BOOST_REQUIRE(returning.runtime->EnqueueWireMessage(
        DeliveryRuntimeFixture::PEER, proposal->message) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(returning.runtime->WaitForIdle(std::chrono::seconds{2}));
    const auto emitted{returning.Sent()};
    const auto vote{std::find_if(emitted.begin(), emitted.end(), [](const auto& relay) {
        return relay.message.kind == Kind::ATTESTATION;
    })};
    BOOST_REQUIRE(vote != emitted.end());
    const auto decoded{flowmesh::DecodeProductionAttestationPayload(vote->message.payload)};
    BOOST_REQUIRE(decoded);
    BOOST_REQUIRE_EQUAL(decoded->seat_index, 1U);

    const auto returning_trace{returning.Snapshot()};
    const auto signed_vote{std::find_if(returning_trace.events.begin(), returning_trace.events.end(), [](const auto& event) {
        return event.stage == "attestation_signed";
    })};
    BOOST_REQUIRE(signed_vote != returning_trace.events.end());
    BOOST_REQUIRE(signed_vote->seat_index);
    BOOST_CHECK_EQUAL(*signed_vote->seat_index, 1U);
    BOOST_CHECK(signed_vote->bls_key_hash == Hash(returning.seats.seats.members[1].key.Key().Compressed()));
    BOOST_CHECK(signed_vote->signature_hash == Hash(decoded->signature.Compressed()));
    BOOST_CHECK_EQUAL(signed_vote->epoch, returning.seats.seats.epoch);
    BOOST_CHECK(signed_vote->seat_set_hash == returning.seats.seats.set_hash);
    const auto durable{std::find_if(returning_trace.events.begin(), returning_trace.events.end(), [](const auto& event) {
        return event.stage == "candidate_durable";
    })};
    BOOST_REQUIRE(durable != returning_trace.events.end());
    BOOST_CHECK_LT(durable->event_id, signed_vote->event_id);

    // A valid signature attached to the wrong claimed seat is not an accepted
    // vote, even though its transport framing and payload are well formed.
    auto wrong_seat{*decoded};
    wrong_seat.seat_index = 3;
    const auto wrong_payload{flowmesh::EncodeProductionAttestationPayload(wrong_seat)};
    BOOST_REQUIRE(wrong_payload);
    auto wrong_wire{vote->message};
    wrong_wire.payload = *wrong_payload;
    BOOST_REQUIRE(proposer.runtime->EnqueueWireMessage(
        DeliveryRuntimeFixture::PEER, wrong_wire) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(proposer.runtime->WaitForIdle(std::chrono::seconds{2}));
    const auto rejected{proposer.Snapshot()};
    BOOST_CHECK(std::none_of(rejected.events.begin(), rejected.events.end(), [](const auto& event) {
        return event.stage == "attestation_verified";
    }));
    BOOST_CHECK(std::any_of(rejected.events.begin(), rejected.events.end(), [](const auto& event) {
        return event.stage == "attestation_refused" && event.reason == "signature_matches_no_candidate";
    }));
    for (unsigned duplicate{0}; duplicate != 2; ++duplicate) {
        BOOST_REQUIRE(proposer.runtime->EnqueueWireMessage(
            DeliveryRuntimeFixture::PEER, vote->message) == flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(proposer.runtime->WaitForIdle(std::chrono::seconds{2}));
    }
    const auto accepted{proposer.Snapshot()};
    BOOST_CHECK_EQUAL(std::count_if(accepted.events.begin(), accepted.events.end(), [](const auto& event) {
        return event.stage == "attestation_verified";
    }), 1);
    const auto remote{std::find_if(accepted.events.begin(), accepted.events.end(), [](const auto& event) {
        return event.stage == "attestation_verified";
    })};
    BOOST_REQUIRE(remote != accepted.events.end());
    BOOST_CHECK(remote->seat_index == signed_vote->seat_index);
    BOOST_CHECK(remote->signature_hash == signed_vote->signature_hash);
    BOOST_CHECK(remote->bls_key_hash == signed_vote->bls_key_hash);
    BOOST_CHECK(remote->object_id == signed_vote->object_id);
    BOOST_CHECK(remote->seat_set_hash == signed_vote->seat_set_hash);
    BOOST_CHECK_EQUAL(remote->epoch, signed_vote->epoch);
    BOOST_CHECK(remote->peer == DeliveryRuntimeFixture::PEER);
    // Two actual seats are still below the unchanged three-of-four threshold.
    BOOST_CHECK_EQUAL(accepted.durably_applied, 0U);
    BOOST_CHECK_EQUAL(accepted.certificate_formed, 0U);
}

BOOST_AUTO_TEST_CASE(delivery_trace_cursor_accounts_for_bounded_ring_eviction)
{
    DeliveryRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_trace_ring",
        node::FlowMeshDeliveryAdmission::FULL};
    const auto initial{f.Snapshot()};
    BOOST_CHECK_GT(initial.last_event_id, 0U);
    for (unsigned tick{0}; tick != 20; ++tick) f.Tick(std::chrono::seconds{1});
    const auto snapshot{f.Snapshot()};
    BOOST_REQUIRE_EQUAL(snapshot.events.size(), 128U);
    BOOST_CHECK_GT(snapshot.events_dropped, 0U);
    BOOST_CHECK_EQUAL(snapshot.last_event_id, snapshot.events_dropped + snapshot.events.size());
    BOOST_CHECK_EQUAL(snapshot.events.back().event_id, snapshot.last_event_id);
    uint64_t previous{snapshot.events.front().event_id - 1};
    for (const auto& event : snapshot.events) {
        BOOST_CHECK_EQUAL(event.event_id, ++previous);
        BOOST_CHECK_LE(event.monotonic_us, snapshot.sampled_monotonic_us);
        BOOST_CHECK_EQUAL(event.epoch, f.seats.seats.epoch);
        BOOST_CHECK(event.seat_set_hash == f.seats.seats.set_hash);
    }
    BOOST_CHECK_LE(snapshot.trace_bytes, uint64_t{16 * 1024 * 1024});
    BOOST_CHECK_LE(snapshot.trace_global_bytes, uint64_t{64 * 1024 * 1024});
}

BOOST_AUTO_TEST_CASE(critical_delivery_retries_refusal_pause_and_socket_write_exactly)
{
    using Admission = node::FlowMeshDeliveryAdmission;
    using Outcome = node::FlowMeshDeliveryOutcome;
    DeliveryRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_delivery_retry", Admission::RECONCILING};
    const auto original{f.Sent()};
    const auto initial_admitted{f.Snapshot().admitted}; // untracked control traffic is counted too
    BOOST_CHECK_EQUAL(f.Snapshot().pending_objects, 2U);
    BOOST_CHECK_EQUAL(f.Snapshot().refused, 2U);
    f.CheckLock(original);

    f.Tick();
    f.Tick(std::chrono::milliseconds{999});
    BOOST_CHECK_EQUAL(f.Sent().size(), 2U);
    f.mode = Admission::FULL;
    f.Tick(std::chrono::milliseconds{1});
    CheckExactDeliveryRetry(original, f.Sent(), 2);
    BOOST_CHECK_EQUAL(f.Snapshot().refused, 4U);

    // Transport readiness and the consensus reconciliation gate are separate.
    f.mode = Admission::ADMITTED;
    f.chain.SetReconciled(false);
    f.Tick(std::chrono::seconds{1});
    BOOST_CHECK_EQUAL(f.Sent().size(), 4U);
    BOOST_CHECK_EQUAL(f.Snapshot().pending_objects, 2U);
    f.chain.SetReconciled(true);
    f.Tick();
    auto sent{f.Sent()};
    CheckExactDeliveryRetry(original, sent, 4);
    BOOST_CHECK_EQUAL(f.Snapshot().admitted, initial_admitted + 2);

    for (size_t i{4}; i < sent.size(); ++i) f.Complete(sent[i], Outcome::SOCKET_WRITTEN);
    BOOST_CHECK_EQUAL(f.Snapshot().socket_written, 2U);
    BOOST_CHECK_EQUAL(f.Snapshot().pending_objects, 2U);
    BOOST_CHECK_EQUAL(f.Snapshot().certificate_formed, 0U);
    BOOST_CHECK_EQUAL(f.Snapshot().durably_applied, 0U);
    f.Tick(std::chrono::milliseconds{999});
    BOOST_CHECK_EQUAL(f.Sent().size(), 6U);
    f.Tick(std::chrono::milliseconds{1});
    sent = f.Sent();
    CheckExactDeliveryRetry(original, sent, 6);
    f.CheckLock(original);

    // Disarming cancels outstanding sends without deleting the signing lock.
    f.keys.m_keys.clear(); // the preceding Tick waited for the worker to idle
    f.Tick(std::chrono::seconds{1});
    BOOST_CHECK_EQUAL(f.Sent().size(), 8U);
    BOOST_CHECK_EQUAL(f.Snapshot().pending_objects, 0U);
    BOOST_CHECK_EQUAL(f.Snapshot().pending_bytes, 0U);
    f.CheckLock(original);
    f.runtime->Stop();
    BOOST_CHECK_EQUAL(f.Snapshot().pending_objects, 0U);
    BOOST_CHECK_EQUAL(f.Snapshot().pending_bytes, 0U);
    const auto cancelled{f.Cancelled()};
    for (size_t i{6}; i < sent.size(); ++i) {
        BOOST_CHECK(std::find(cancelled.begin(), cancelled.end(), sent[i].delivery_id) != cancelled.end());
    }
    f.CheckLock(original);
    BOOST_CHECK(!f.runtime->NotifyDeliveryEvent({sent.back().delivery_id,
        DeliveryRuntimeFixture::PEER, Outcome::SOCKET_WRITTEN, {}}));
}

BOOST_AUTO_TEST_CASE(critical_delivery_timeout_changes_attempt_id_and_ignores_late_completion)
{
    using Outcome = node::FlowMeshDeliveryOutcome;
    DeliveryRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_delivery_timeout",
                             node::FlowMeshDeliveryAdmission::ADMITTED};
    const auto original{f.Sent()};
    f.Tick(std::chrono::milliseconds{4999});
    BOOST_CHECK_EQUAL(f.Sent().size(), 2U);
    BOOST_CHECK_EQUAL(f.Snapshot().completion_timeouts, 0U);
    f.Tick(std::chrono::milliseconds{1});
    const auto retried{f.Sent()};
    CheckExactDeliveryRetry(original, retried, 2);
    BOOST_CHECK_EQUAL(f.Snapshot().completion_timeouts, 2U);
    const auto cancelled{f.Cancelled()};
    for (const auto& relay : original) {
        BOOST_CHECK(std::find(cancelled.begin(), cancelled.end(), relay.delivery_id) != cancelled.end());
        f.Complete(relay, Outcome::SOCKET_WRITTEN);
        f.Complete(relay, Outcome::DISCONNECTED);
    }
    BOOST_CHECK_EQUAL(f.Snapshot().socket_written, 0U);
    BOOST_CHECK_EQUAL(f.Snapshot().refused, 0U);
    // Even a current attempt may only be completed by its admitted peer.
    f.Complete(retried.back(), Outcome::SOCKET_WRITTEN, -3);
    BOOST_CHECK_EQUAL(f.Snapshot().socket_written, 0U);
    for (size_t i{2}; i < retried.size(); ++i) f.Complete(retried[i], Outcome::SOCKET_WRITTEN);
    BOOST_CHECK_EQUAL(f.Snapshot().socket_written, 2U);
    BOOST_CHECK_EQUAL(f.Snapshot().pending_objects, 2U);
    f.CheckLock(original);
    f.runtime->Stop();
    const auto stopped{f.Cancelled()};
    for (size_t i{2}; i < retried.size(); ++i) {
        BOOST_CHECK(std::find(stopped.begin(), stopped.end(), retried[i].delivery_id) != stopped.end());
    }
    BOOST_CHECK_EQUAL(f.Snapshot().pending_objects, 0U);
    BOOST_CHECK_EQUAL(f.Snapshot().pending_bytes, 0U);
    f.CheckLock(original);
}

BOOST_AUTO_TEST_CASE(critical_proposal_waits_for_reconciliation_then_validates_and_votes)
{
    using Admission = node::FlowMeshDeliveryAdmission;
    DeliveryRuntimeFixture producer{m_args.GetDataDirBase() / "flowmesh_deferred_producer", Admission::ADMITTED};
    const auto original{producer.Sent()};
    const auto proposal{std::find_if(original.begin(), original.end(), [](const auto& relay) {
        return relay.message.kind == flowmesh::WireMessageKind::PROPOSAL;
    })};
    BOOST_REQUIRE(proposal != original.end());
    DeliveryRuntimeFixture recipient{m_args.GetDataDirBase() / "flowmesh_deferred_recipient", Admission::ADMITTED, false};
    recipient.chain.SetReconciled(false);
    BOOST_REQUIRE(recipient.runtime->EnqueueWireMessage(DeliveryRuntimeFixture::PEER,
        proposal->message) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(recipient.runtime->WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(recipient.Snapshot().receive_deferred, 1U);
    BOOST_CHECK_EQUAL(recipient.Snapshot().deferred_objects, 1U);
    BOOST_CHECK_GT(recipient.Snapshot().deferred_bytes, 0U);
    BOOST_CHECK(recipient.Sent().empty());
    std::optional<uint256> locked;
    std::string error;
    BOOST_REQUIRE(recipient.store.ReadLock({recipient.seats.seats.epoch, 0}, locked, error));
    BOOST_CHECK(!locked);
    recipient.Tick(std::chrono::seconds{1});
    BOOST_CHECK_EQUAL(recipient.Snapshot().deferred_objects, 1U);
    BOOST_CHECK(recipient.Sent().empty());

    recipient.chain.SetReconciled(true);
    recipient.Tick(std::chrono::milliseconds{250});
    BOOST_CHECK_EQUAL(recipient.Snapshot().deferred_objects, 0U);
    BOOST_CHECK_EQUAL(recipient.Snapshot().deferred_bytes, 0U);
    const auto sent{recipient.Sent()};
    const auto vote{std::find_if(sent.begin(), sent.end(), [](const auto& relay) {
        return relay.message.kind == flowmesh::WireMessageKind::ATTESTATION;
    })};
    BOOST_REQUIRE(vote != sent.end());
    const auto decoded{flowmesh::DecodeProductionAttestationPayload(vote->message.payload)};
    BOOST_REQUIRE(decoded);
    BOOST_CHECK_EQUAL(decoded->seat_index, 1U);
    recipient.CheckLock(original);
    BOOST_CHECK_EQUAL(recipient.Snapshot().certificate_formed, 0U);
    BOOST_CHECK_EQUAL(recipient.Snapshot().durably_applied, 0U);
}

BOOST_AUTO_TEST_CASE(critical_proposal_generation_change_survives_reopened_gate)
{
    using Admission = node::FlowMeshDeliveryAdmission;
    DeliveryRuntimeFixture producer{m_args.GetDataDirBase() / "flowmesh_generation_producer", Admission::ADMITTED};
    const auto original{producer.Sent()};
    const auto proposal{std::find_if(original.begin(), original.end(), [](const auto& relay) {
        return relay.message.kind == flowmesh::WireMessageKind::PROPOSAL;
    })};
    BOOST_REQUIRE(proposal != original.end());
    const auto decoded{flowmesh::DecodeProductionProposalPayload(proposal->message.payload)};
    BOOST_REQUIRE(decoded);
    DeliveryRuntimeFixture recipient{m_args.GetDataDirBase() / "flowmesh_generation_recipient", Admission::ADMITTED, false};
    BOOST_REQUIRE(!(decoded->entry.anchor == recipient.chain.Current()));
    recipient.chain.ReconcileOnceWhileChecking(decoded->entry.anchor);
    BOOST_REQUIRE(recipient.runtime->EnqueueWireMessage(DeliveryRuntimeFixture::PEER,
        proposal->message) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(recipient.runtime->WaitForIdle(std::chrono::seconds{2}));

    // The outer current-anchor gate passed; the proposal-anchor check inside
    // HandleProposal failed once. A final boolean gate check alone loses it.
    BOOST_CHECK_EQUAL(recipient.chain.DeliveryGeneration(), 1U);
    BOOST_CHECK(recipient.chain.Acceptable(recipient.chain.Current()));
    BOOST_CHECK(recipient.chain.Acceptable(decoded->entry.anchor));
    BOOST_CHECK_EQUAL(recipient.Snapshot().receive_deferred, 1U);
    BOOST_CHECK_EQUAL(recipient.Snapshot().deferred_objects, 1U);
    BOOST_CHECK(recipient.Sent().empty());
    std::optional<uint256> locked;
    std::string error;
    BOOST_REQUIRE(recipient.store.ReadLock({recipient.seats.seats.epoch, 0}, locked, error));
    BOOST_CHECK(!locked);

    recipient.Tick(std::chrono::milliseconds{250});
    BOOST_CHECK_EQUAL(recipient.Snapshot().deferred_objects, 0U);
    BOOST_CHECK_EQUAL(recipient.Snapshot().deferred_bytes, 0U);
    BOOST_CHECK_GT(recipient.Snapshot().verified, 0U);
    const auto sent{recipient.Sent()};
    const auto vote{std::find_if(sent.begin(), sent.end(), [](const auto& relay) {
        return relay.message.kind == flowmesh::WireMessageKind::ATTESTATION;
    })};
    BOOST_REQUIRE(vote != sent.end());
    const auto attestation{flowmesh::DecodeProductionAttestationPayload(vote->message.payload)};
    BOOST_REQUIRE(attestation);
    BOOST_CHECK_EQUAL(attestation->seat_index, 1U);
    recipient.CheckLock(original);
    BOOST_CHECK_EQUAL(recipient.Snapshot().certificate_formed, 0U);
    BOOST_CHECK_EQUAL(recipient.Snapshot().durably_applied, 0U);
}

BOOST_AUTO_TEST_CASE(critical_durable_certificate_retries_after_candidate_cleanup_and_disarm)
{
    DeliveryRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_delivery_durable_certificate",
                             node::FlowMeshDeliveryAdmission::FULL};
    BOOST_CHECK_EQUAL(f.Snapshot().pending_objects, 2U);
    f.keys.m_keys[f.market] = f.seats.secrets; // fixture is idle
    f.Tick(std::chrono::seconds{1});
    const auto status{f.runtime->MarketStatus(f.market)};
    BOOST_REQUIRE(status);
    BOOST_REQUIRE_EQUAL(status->next_sequence, 1U);
    BOOST_CHECK_EQUAL(f.Snapshot().certificate_formed, 1U);
    BOOST_CHECK_EQUAL(f.Snapshot().durably_applied, 1U);
    BOOST_CHECK_EQUAL(f.Snapshot().pending_objects, 1U);
    BOOST_CHECK_GE(f.Snapshot().cancelled, 2U);
    const auto sent{f.Sent()};
    const auto certificate{std::find_if(sent.begin(), sent.end(), [](const auto& relay) {
        return relay.message.kind == flowmesh::WireMessageKind::CERTIFICATE;
    })};
    BOOST_REQUIRE(certificate != sent.end());
    const uint256 durable_hash{status->last_microblock_hash};
    std::string error;
    std::optional<node::StoredProductionEntry> stored;
    BOOST_REQUIRE(f.store.ReadEntry(0, f.seats.seats, stored, error));
    BOOST_REQUIRE(stored);
    BOOST_CHECK(stored->entry.GetHash() == durable_hash);
    std::optional<node::StoredLockedProductionCandidate> retained;
    BOOST_REQUIRE(f.store.ReadLockedCandidate({f.seats.seats.epoch, 0}, retained, error));
    BOOST_CHECK(!retained);

    // A durable certificate remains relayable after candidate/vote caches are
    // cleared, with no armed secret key and no new local consensus action.
    f.keys.m_keys.clear();
    f.Tick(std::chrono::seconds{1});
    const auto retried{f.Sent()};
    BOOST_REQUIRE_EQUAL(retried.size(), sent.size() + 1);
    BOOST_CHECK(retried.back().message.kind == flowmesh::WireMessageKind::CERTIFICATE);
    BOOST_CHECK_GT(retried.back().delivery_id, certificate->delivery_id);
    flowmesh::WireCheck check;
    const auto original_bytes{flowmesh::EncodeWireMessage(certificate->message, check)};
    const auto retry_bytes{flowmesh::EncodeWireMessage(retried.back().message, check)};
    BOOST_REQUIRE(original_bytes);
    BOOST_REQUIRE(retry_bytes);
    BOOST_CHECK(*original_bytes == *retry_bytes);
    BOOST_CHECK_EQUAL(f.Snapshot().pending_objects, 1U);
    BOOST_CHECK_EQUAL(f.Snapshot().certificate_formed, 1U);
    BOOST_CHECK_EQUAL(f.Snapshot().durably_applied, 1U);
    BOOST_CHECK(f.runtime->MarketStatus(f.market)->last_microblock_hash == durable_hash);
    f.runtime->Stop();
    BOOST_CHECK_EQUAL(f.Snapshot().pending_objects, 0U);
    const auto cancelled{f.Cancelled()};
    BOOST_CHECK(std::find(cancelled.begin(), cancelled.end(), retried.back().delivery_id) != cancelled.end());
}

BOOST_AUTO_TEST_CASE(critical_caller_retry_respects_new_local_lock_without_periodic_cleanup)
{
    using Admission = node::FlowMeshDeliveryAdmission;
    DeliveryRuntimeFixture producer{m_args.GetDataDirBase() / "flowmesh_lock_retry_producer", Admission::FULL};
    const auto original{producer.Sent()};
    const auto proposal_a{std::find_if(original.begin(), original.end(), [](const auto& relay) {
        return relay.message.kind == flowmesh::WireMessageKind::PROPOSAL;
    })};
    BOOST_REQUIRE(proposal_a != original.end());
    const auto decoded_a{flowmesh::DecodeProductionProposalPayload(proposal_a->message.payload)};
    BOOST_REQUIRE(decoded_a);
    DeliveryRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_lock_retry_observer", Admission::FULL, false};
    f.keys.m_keys.clear(); // no signature or lock while observing competing candidates
    flowmesh::ProductionEpochGate gate{f.domain, f.market, f.seats.seats};
    flowmesh::ProductionEntryCheck check;
    const auto candidate_b{flowmesh::BuildProductionExecutionEntry(
        f.initial, f.domain, f.market, f.seats.seats, gate, 0, 0, {},
        f.chain.Current(), {f.chain.TipHeight(), std::nullopt, &f.chain},
        Filled(0x6b), {}, nullptr, check)};
    BOOST_REQUIRE(candidate_b);
    BOOST_REQUIRE(candidate_b->entry.GetHash() != decoded_a->entry.GetHash());
    flowmesh::ProductionProposalEnvelope envelope_b;
    envelope_b.entry = candidate_b->entry;
    envelope_b.round = 1;
    envelope_b.proposer_seat_index = 1;
    const auto digest_b{flowmesh::ProductionProposalDigest(envelope_b.entry, envelope_b.round)};
    envelope_b.proposer_signature = f.seats.secrets[1]
        .Sign(std::span<const unsigned char>{digest_b.begin(), 32}).Compressed();
    auto proposal_b{proposal_a->message};
    const auto proposal_payload{flowmesh::EncodeProductionProposalPayload(envelope_b)};
    BOOST_REQUIRE(proposal_payload);
    proposal_b.payload = *proposal_payload;
    const auto signature_b{flowmesh::SignBlsMicroblockCertificate(f.seats.secrets[3],
        flowmesh::ProductionCertificateContext(candidate_b->entry), f.seats.seats)};
    BOOST_REQUIRE(signature_b);
    const auto vote_payload{flowmesh::EncodeProductionAttestationPayload({3, *signature_b})};
    BOOST_REQUIRE(vote_payload);
    const flowmesh::WireMessage vote_b{flowmesh::WireMessageKind::ATTESTATION,
                                      proposal_b.header, *vote_payload};
    const auto receive = [&](const flowmesh::WireMessage& message) {
        BOOST_REQUIRE(f.runtime->EnqueueWireMessage(DeliveryRuntimeFixture::PEER,
            message) == flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
    };
    receive(proposal_a->message);
    receive(proposal_b);
    receive(vote_b);
    std::string error;
    const auto observed{f.runtime->MarketData(f.market, std::nullopt, {}, error)};
    BOOST_REQUIRE(observed);
    BOOST_CHECK_EQUAL(observed->snapshot.runtime.candidate_count, 2U);
    BOOST_CHECK(!observed->snapshot.runtime.local_locked_candidate);
    const auto before_lock{f.Sent()};
    BOOST_REQUIRE(std::any_of(before_lock.begin(), before_lock.end(), [&](const auto& relay) {
        return relay.message.kind == flowmesh::WireMessageKind::ATTESTATION &&
               relay.message.payload == vote_b.payload;
    }));

    f.keys.m_keys[f.market] = {f.seats.secrets[2]};
    receive(proposal_a->message);
    f.CheckLock(original);
    const auto after_lock{f.Sent()};
    const auto local_vote_count = [](const auto& relays) {
        return std::count_if(relays.begin(), relays.end(), [](const auto& relay) {
            if (relay.message.kind != flowmesh::WireMessageKind::ATTESTATION) return false;
            const auto vote{flowmesh::DecodeProductionAttestationPayload(relay.message.payload)};
            return vote && vote->seat_index == 2;
        });
    };
    BOOST_REQUIRE_EQUAL(local_vote_count(after_lock), 1);
    // No NotifyTick: the incoming cached-vote path must perform its own
    // relevance check instead of depending on periodic retirement of B.
    f.clock.m_now += node::FlowMeshCommitteeRelayBudget::REPEAT_DELAY;
    receive(vote_b);
    const auto after_retry{f.Sent()};
    BOOST_CHECK_EQUAL(after_retry.size(), after_lock.size());
    BOOST_CHECK_EQUAL(local_vote_count(after_retry), 1);
    f.CheckLock(original);
    // B's old outgoing item has now been retired. A new-key admission of
    // those same cached bytes must enforce the identical A-lock restriction.
    f.clock.m_now += node::FlowMeshCommitteeRelayBudget::REPEAT_DELAY;
    receive(vote_b);
    BOOST_CHECK_EQUAL(f.Sent().size(), after_lock.size());
    BOOST_CHECK_EQUAL(local_vote_count(f.Sent()), 1);
    f.CheckLock(original);
    BOOST_CHECK_EQUAL(f.Snapshot().certificate_formed, 0U);
    BOOST_CHECK_EQUAL(f.Snapshot().durably_applied, 0U);
}

BOOST_AUTO_TEST_CASE(critical_paused_retention_expires_and_regenerates_exact_verified_objects)
{
    using Admission = node::FlowMeshDeliveryAdmission;
    DeliveryRuntimeFixture producer{m_args.GetDataDirBase() / "flowmesh_pause_expiry_producer", Admission::FULL};
    const auto produced{producer.Sent()};
    const auto proposal{std::find_if(produced.begin(), produced.end(), [](const auto& relay) {
        return relay.message.kind == flowmesh::WireMessageKind::PROPOSAL;
    })};
    BOOST_REQUIRE(proposal != produced.end());
    for (const bool reconciliation : {true, false}) {
        DeliveryRuntimeFixture f{m_args.GetDataDirBase() / fs::PathFromString(
            reconciliation ? "flowmesh_reconciliation_expiry" : "flowmesh_transition_expiry"),
            Admission::ADMITTED, false};
        BOOST_REQUIRE(f.runtime->EnqueueWireMessage(DeliveryRuntimeFixture::PEER,
            proposal->message) == flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
        const auto original{f.Sent()};
        BOOST_REQUIRE_EQUAL(original.size(), 2U);
        f.CheckLock(produced);
        f.keys.m_visibility_chain = &f.chain;
        if (reconciliation) f.chain.SetReconciled(false);
        else f.chain.SetTransition(f.market, node::FlowMeshSeatTransitionKind::PAUSED);
        f.Tick(std::chrono::seconds{1}); // starts the pause-retention interval
        BOOST_CHECK_EQUAL(f.Snapshot().pending_objects, 2U);
        f.Tick(std::chrono::milliseconds{59999});
        BOOST_CHECK_EQUAL(f.Snapshot().pending_objects, 2U);
        BOOST_CHECK_EQUAL(f.Sent().size(), 2U);
        f.Tick(std::chrono::milliseconds{1});
        BOOST_CHECK_EQUAL(f.Snapshot().pending_objects, 0U);
        BOOST_CHECK_EQUAL(f.Snapshot().pending_bytes, 0U);
        BOOST_CHECK_EQUAL(f.Sent().size(), 2U);
        f.CheckLock(produced);
        const auto cancelled{f.Cancelled()};
        for (const auto& relay : original) {
            BOOST_CHECK(std::find(cancelled.begin(), cancelled.end(), relay.delivery_id) != cancelled.end());
        }

        if (reconciliation) f.chain.SetReconciled(true);
        else f.chain.SetTransition(f.market, node::FlowMeshSeatTransitionKind::CONTINUE);
        f.Tick(std::chrono::milliseconds{250});
        const auto regenerated{f.Sent()};
        BOOST_REQUIRE_EQUAL(regenerated.size(), 4U);
        for (const auto& before : original) {
            const auto after{std::find_if(regenerated.begin() + 2, regenerated.end(), [&](const auto& relay) {
                return relay.message.kind == before.message.kind;
            })};
            BOOST_REQUIRE(after != regenerated.end());
            BOOST_CHECK_GT(after->delivery_id, before.delivery_id);
            flowmesh::WireCheck check;
            const auto original_bytes{flowmesh::EncodeWireMessage(before.message, check)};
            const auto regenerated_bytes{flowmesh::EncodeWireMessage(after->message, check)};
            BOOST_REQUIRE(original_bytes);
            BOOST_REQUIRE(regenerated_bytes);
            BOOST_CHECK(*original_bytes == *regenerated_bytes);
        }
        // This node owns seat1, not the scheduled seat0. With no incoming
        // proposal, Tick cannot create a new proposal or attestation; these
        // two frames were rebuilt solely from already-verified compact data.
        BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->round, 0U);
        BOOST_CHECK_EQUAL(f.Snapshot().pending_objects, 2U);
        BOOST_CHECK_EQUAL(f.Snapshot().certificate_formed, 0U);
        BOOST_CHECK_EQUAL(f.Snapshot().durably_applied, 0U);
        f.CheckLock(produced);
    }
}

BOOST_AUTO_TEST_CASE(critical_permanent_halt_retires_outgoing_certificate_not_durable_history)
{
    DeliveryRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_halted_delivery",
                             node::FlowMeshDeliveryAdmission::FULL};
    f.keys.m_keys[f.market] = f.seats.secrets;
    f.Tick(std::chrono::seconds{1});
    BOOST_REQUIRE_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 1U);
    const auto original_hash{f.runtime->MarketStatus(f.market)->last_microblock_hash};
    BOOST_REQUIRE_EQUAL(f.Snapshot().pending_objects, 1U);
    const auto sent{f.Sent()};
    flowmesh::ProductionEpochGate gate{f.domain, f.market, f.seats.seats};
    flowmesh::ProductionEntryCheck check;
    const auto conflict{flowmesh::BuildProductionExecutionEntry(
        f.initial, f.domain, f.market, f.seats.seats, gate, 0, 0, {},
        f.chain.Current(), {f.chain.TipHeight(), std::nullopt, &f.chain},
        Filled(0x6b), {}, nullptr, check)};
    BOOST_REQUIRE(conflict);
    BOOST_REQUIRE(conflict->entry.GetHash() != original_hash);
    const auto payload{flowmesh::EncodeProductionCertifiedPayload(
        {conflict->entry, Certify(conflict->entry, f.seats)}, f.seats.seats.Size())};
    BOOST_REQUIRE(payload);
    BOOST_REQUIRE(f.runtime->EnqueueWireMessage(DeliveryRuntimeFixture::PEER,
        {flowmesh::WireMessageKind::CERTIFICATE,
         {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 0}, *payload}) ==
        flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
    BOOST_REQUIRE(f.runtime->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::CERTIFICATE_CONFLICT);
    f.Tick();
    BOOST_CHECK_EQUAL(f.Snapshot().pending_objects, 0U);
    BOOST_CHECK_EQUAL(f.Snapshot().pending_bytes, 0U);
    BOOST_CHECK_EQUAL(f.Sent().size(), sent.size());
    BOOST_CHECK(f.runtime->MarketStatus(f.market)->last_microblock_hash == original_hash);
    std::optional<node::StoredProductionEntry> stored;
    std::string error;
    BOOST_REQUIRE(f.store.ReadEntry(0, f.seats.seats, stored, error));
    BOOST_REQUIRE(stored);
    BOOST_CHECK(stored->entry.GetHash() == original_hash);
    BOOST_CHECK_EQUAL(f.Snapshot().durably_applied, 1U);
}

BOOST_AUTO_TEST_CASE(catchup_timeout_reduces_page_count_without_relaxing_deadline_or_verification)
{
    DeliveryRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_adaptive_catchup",
                             node::FlowMeshDeliveryAdmission::FULL, false};
    f.keys.m_keys.clear(); // A catch-up observer never acquires a signing seat.
    const auto peer{DeliveryRuntimeFixture::PEER};
    const auto request = [&] {
        BOOST_REQUIRE(f.runtime->RequestCatchup(peer, f.market));
        BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
    };
    const auto check_request = [&](const uint16_t expected_count, const uint64_t expected_sequence) {
        const auto requests{f.Requests()};
        BOOST_REQUIRE(!requests.empty());
        uint16_t maximum_entries{0};
        uint32_t maximum_bytes{0};
        BOOST_REQUIRE(flowmesh::DecodeCatchupRequest(requests.back().message.payload,
            maximum_entries, maximum_bytes));
        BOOST_CHECK_EQUAL(maximum_entries, expected_count);
        BOOST_CHECK_EQUAL(maximum_bytes, flowmesh::FLOWMESH_CATCHUP_MAX_BYTES);
        BOOST_CHECK_EQUAL(requests.back().message.header.sequence, expected_sequence);
        BOOST_CHECK(requests.back().peer == peer);
    };
    flowmesh::ProductionEpochGate gate{f.domain, f.market, f.seats.seats};
    flowmesh::ProductionEntryCheck check;
    const auto genesis{flowmesh::BuildProductionExecutionEntry(
        f.initial, f.domain, f.market, f.seats.seats, gate, 0, 0, {},
        {100, Filled(0x71)}, {f.chain.TipHeight(), std::nullopt, &f.chain},
        Filled(0x6b), {}, nullptr, check)};
    BOOST_REQUIRE(genesis);
    const auto certified{flowmesh::EncodeProductionCertifiedPayload(
        {genesis->entry, Certify(genesis->entry, f.seats)}, f.seats.seats.Size())};
    BOOST_REQUIRE(certified);
    const auto receive_page = [&](const std::vector<std::vector<unsigned char>>& entries) {
        const auto payload{flowmesh::EncodeCatchupEntries(entries)};
        BOOST_REQUIRE(payload);
        BOOST_REQUIRE(f.runtime->EnqueueWireMessage(peer,
            {flowmesh::WireMessageKind::ENTRIES,
             {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 0}, *payload}) ==
            flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
    };
    const auto untouched = [&] {
        BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 0U);
        BOOST_CHECK(f.runtime->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);
        BOOST_CHECK(f.runtime->StateSnapshot(f.market)->Root() == f.initial.Root());
        std::optional<uint256> locked;
        std::string error;
        BOOST_REQUIRE(f.store.ReadLock({f.seats.seats.epoch, 0}, locked, error));
        BOOST_CHECK(!locked);
    };
    request();
    check_request(64, 0);
    f.clock.m_now += std::chrono::milliseconds{4999};
    request();
    BOOST_REQUIRE_EQUAL(f.Requests().size(), 1U); // one outstanding request
    BOOST_CHECK_EQUAL(f.Snapshot().catchup_timeouts, 0U);
    f.clock.m_now += std::chrono::milliseconds{1};
    // No Tick: a complete valid response reaching its five-second deadline
    // must be refused by HandleEntries itself, not rescued by late upkeep.
    receive_page({*certified});
    untouched();
    BOOST_CHECK_EQUAL(f.Snapshot().catchup_timeouts, 1U);
    f.clock.m_now += std::chrono::milliseconds{9999};
    request();
    BOOST_CHECK_EQUAL(f.Requests().size(), 1U); // original fifteen-second cooldown
    f.clock.m_now += std::chrono::milliseconds{1};
    request();
    check_request(32, 0);
    for (const uint16_t smaller : {16, 8, 1}) {
        f.Tick(std::chrono::seconds{5});
        receive_page({*certified}); // no outstanding request: never applies
        untouched();
        f.Tick(std::chrono::seconds{10});
        request();
        check_request(smaller, 0);
    }
    BOOST_CHECK_EQUAL(f.Snapshot().catchup_timeouts, 4U);

    // Adapting count never enlarges the active bounds or skips certificate
    // verification. Neither an oversized nor an invalid page earns growth.
    receive_page({*certified, *certified});
    untouched();
    f.Tick(std::chrono::seconds{15});
    request();
    check_request(1, 0);
    auto invalid{*certified};
    invalid.back() ^= 1;
    receive_page({invalid});
    untouched();
    f.Tick(std::chrono::seconds{15});
    request();
    check_request(1, 0);
    receive_page({*certified}); // immediate, fully applied one-entry page
    BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 1U);
    BOOST_CHECK(f.runtime->MarketStatus(f.market)->last_microblock_hash == genesis->entry.GetHash());
    BOOST_CHECK(f.runtime->MarketStatus(f.market)->observer_only);
    // A full successful page schedules its normal continuation, now doubled.
    check_request(2, 1);
    std::optional<node::StoredProductionEntry> stored;
    std::string error;
    BOOST_REQUIRE(f.store.ReadEntry(0, f.seats.seats, stored, error));
    BOOST_REQUIRE(stored);
    BOOST_CHECK(flowmesh::CheckProductionEntryCertificate(stored->entry,
        f.seats.seats, stored->certificate) == flowmesh::BlsCertificateCheck::OK);
    const auto data{f.runtime->MarketData(f.market, std::nullopt, {}, error)};
    BOOST_REQUIRE(data);
    BOOST_CHECK_EQUAL(data->snapshot.quorum_required, 3U);
    std::optional<uint256> locked;
    BOOST_REQUIRE(f.store.ReadLock({f.seats.seats.epoch, 0}, locked, error));
    BOOST_CHECK(!locked);

    f.runtime->FlowMeshPeerDisconnected(peer);
    BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
    request();
    check_request(64, 1); // disconnect clears the local adaptive profile
}

BOOST_AUTO_TEST_CASE(evidence_retry_budget_bounds_count_bytes_and_clock)
{
    node::FlowMeshEvidenceRetryBudget budget;
    flowmesh::WireClock::time_point now{};
    BOOST_CHECK(!budget.Consume(1));
    BOOST_REQUIRE(budget.Begin(now));
    const size_t maximum_frame{flowmesh::FLOWMESH_ACTION_MAX_BYTES + flowmesh::FLOWMESH_WIRE_HEADER_SIZE};
    for (size_t i{0}; i < 7; ++i) BOOST_CHECK(budget.Consume(maximum_frame));
    BOOST_CHECK(!budget.Consume(maximum_frame)); // bytes, not message count
    BOOST_CHECK(budget.Consume(node::FlowMeshEvidenceRetryBudget::MAX_BYTES - 7 * maximum_frame));
    BOOST_CHECK(budget.Full());
    BOOST_CHECK(!budget.Begin(now));
    BOOST_CHECK(!budget.Begin(now + std::chrono::milliseconds{249}));
    now += std::chrono::milliseconds{250};
    BOOST_REQUIRE(budget.Begin(now));
    for (size_t i{0}; i < 8; ++i) BOOST_CHECK(budget.Consume(1));
    BOOST_CHECK(!budget.Consume(1)); // count, not byte capacity
    now += std::chrono::hours{1};
    BOOST_REQUIRE(budget.Begin(now)); // no accumulation after a long gap
    for (size_t i{0}; i < 8; ++i) BOOST_CHECK(budget.Consume(1));
    BOOST_CHECK(!budget.Consume(1));
    BOOST_CHECK(!budget.Begin(now - std::chrono::seconds{1}));
}

BOOST_AUTO_TEST_CASE(committee_forwarding_budget_bounds_count_bytes_and_clock)
{
    using Budget = node::FlowMeshCommitteeRelayBudget;
    flowmesh::WireClock::time_point now{};
    Budget global{Budget::GLOBAL_MESSAGES, Budget::GLOBAL_BYTES};
    Budget market{Budget::MARKET_MESSAGES, Budget::MARKET_BYTES};
    const size_t maximum_proposal{flowmesh::FLOWMESH_PROPOSAL_MAX_BYTES +
                                  flowmesh::FLOWMESH_WIRE_HEADER_SIZE};
    BOOST_REQUIRE(global.Available(now, maximum_proposal));
    BOOST_REQUIRE(market.Available(now, maximum_proposal));
    global.Charge(maximum_proposal);
    market.Charge(maximum_proposal);
    BOOST_CHECK(!global.Available(now, maximum_proposal));
    BOOST_CHECK(!market.Available(now, maximum_proposal));
    BOOST_CHECK(!market.Available(now + std::chrono::milliseconds{249}, maximum_proposal));
    now += Budget::INTERVAL;
    // Four independent markets share the same global frame cap. Neither
    // multiple markets nor an unchanged/backward clock refills it.
    for (size_t group{0}; group < 4; ++group) {
        Budget next_market{Budget::MARKET_MESSAGES, Budget::MARKET_BYTES};
        for (size_t i{0}; i < Budget::MARKET_MESSAGES; ++i) {
            BOOST_REQUIRE(global.Available(now, 50));
            BOOST_REQUIRE(next_market.Available(now, 50));
            global.Charge(50);
            next_market.Charge(50);
        }
        BOOST_CHECK(!next_market.Available(now, 50));
    }
    BOOST_CHECK(!global.Available(now, 50));
    BOOST_CHECK(!global.Available(now - Budget::INTERVAL, 50));
    now += std::chrono::hours{1};
    for (size_t i{0}; i < Budget::GLOBAL_MESSAGES; ++i) {
        BOOST_REQUIRE(global.Available(now, 50));
        global.Charge(50);
    }
    BOOST_CHECK(!global.Available(now, 50)); // no idle-time accumulation
}

BOOST_AUTO_TEST_CASE(committee_forwarding_line_recovers_exact_dropped_vote)
{
    const uint256 domain{Filled(0x1d)};
    const modern::AssetId asset{Filled(0x3d)};
    const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const auto vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x5d)};
    const SeatFixture seats{Seats(domain, market, 4, 7, 100, Filled(0x71), 124)};
    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(seats.seats);
    const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(),
                                        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    std::array<FixedClock, 4> clocks;
    std::array<RuntimeKeys, 4> keys;
    // Topology: seat0 -- observer -- seat1 -- seat2. Seat3 is offline.
    // Every certificate needs all three live seats; no fully connected mesh.
    keys[0].m_keys[market] = {seats.secrets[0]};
    keys[2].m_keys[market] = {seats.secrets[1]};
    keys[3].m_keys[market] = {seats.secrets[2]};
    std::array<std::unique_ptr<node::FlowMeshProductionStore>, 4> stores;
    std::array<std::unique_ptr<node::FlowMeshRuntime>, 4> runtimes;
    RuntimeNetwork network;
    struct Seen { size_t from; size_t to; flowmesh::WireMessage wire; };
    std::mutex seen_mutex;
    std::vector<Seen> seen;
    std::optional<std::vector<unsigned char>> dropped_vote;
    std::optional<std::vector<unsigned char>> retried_vote;
    struct StopBeforeCapturedState {
        std::array<std::unique_ptr<node::FlowMeshRuntime>, 4>& runtimes;
        ~StopBeforeCapturedState()
        {
            for (const auto& runtime : runtimes) if (runtime) runtime->Stop();
        }
    } stop_before_captured_state{runtimes};
    network.SetFilter([&](const size_t from, const size_t to, const auto& wire) {
        if (!(from + 1 == to || to + 1 == from)) return false;
        std::lock_guard<std::mutex> guard{seen_mutex};
        if (seen.size() < 512) seen.push_back({from, to, wire});
        if (wire.kind == flowmesh::WireMessageKind::ATTESTATION) {
            const auto vote{flowmesh::DecodeProductionAttestationPayload(wire.payload)};
            if (!vote) return false;
            // Only the endpoint proposer can collect all three votes. This
            // prevents a side certificate from masking lost-vote recovery.
            if (vote->seat_index == 0 || to == 3) return false;
            if (from == 3 && to == 2 && vote->seat_index == 2) {
                if (!dropped_vote) {
                    dropped_vote = wire.payload;
                    return false;
                }
                retried_vote = wire.payload;
            }
        }
        return true;
    });
    std::string error;
    for (size_t i{0}; i < runtimes.size(); ++i) {
        stores[i] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
            .path = m_args.GetDataDirBase() / fs::PathFromString(
                        "flowmesh_runtime_committee_line_" + std::to_string(i)),
            .cache_bytes = size_t{1} << 20, .wipe_data = true});
        BOOST_REQUIRE_MESSAGE(stores[i]->OpenForMarket(
            domain, market, seats.seats, initial.Root(), error), error);
        node::FlowMeshRuntimeConfig config;
        config.chain = &chain;
        config.keys = &keys[i];
        config.clock = &clocks[i];
        config.round_timeout = std::chrono::hours{1};
        config.relay = [&network, i](node::FlowMeshRuntimeRelay relay) {
            network.Relay(i, std::move(relay));
            return LegacyRelayResult();
        };
        runtimes[i] = std::make_unique<node::FlowMeshRuntime>(config,
            std::vector<node::FlowMeshRuntimeMarketConfig>{MarketConfig(
                domain, market, treasury, seats.seats, initial, *stores[i], nullptr)});
        network.Set(i, runtimes[i].get(), true);
        BOOST_REQUIRE_MESSAGE(runtimes[i]->Start(error), error);
    }
    const auto drain = [&] {
        for (size_t pass{0}; pass < 4; ++pass) {
            for (const auto& runtime : runtimes) {
                BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
            }
        }
    };
    runtimes[0]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        std::lock_guard<std::mutex> guard{seen_mutex};
        return dropped_vote.has_value();
    }));
    drain();
    flowmesh::WireMessage original;
    size_t forwarded_before;
    {
        std::lock_guard<std::mutex> guard{seen_mutex};
        const auto found{std::find_if(seen.begin(), seen.end(), [](const auto& item) {
            return item.from == 0 && item.wire.kind == flowmesh::WireMessageKind::PROPOSAL;
        })};
        BOOST_REQUIRE(found != seen.end());
        original = found->wire;
        forwarded_before = std::count_if(seen.begin(), seen.end(), [](const auto& item) {
            return item.from == 1 && item.wire.kind == flowmesh::WireMessageKind::PROPOSAL;
        });
        BOOST_CHECK_EQUAL(forwarded_before, 1U);
    }
    for (const auto& runtime : runtimes) {
        BOOST_CHECK_EQUAL(runtime->MarketStatus(market)->next_sequence, 0U);
        BOOST_CHECK(runtime->MarketStatus(market)->halt == node::FlowMeshRuntimeHalt::NONE);
    }
    const auto proposal{flowmesh::DecodeProductionProposalPayload(original.payload)};
    BOOST_REQUIRE(proposal);
    const auto locked_hash{proposal->entry.GetHash()};
    for (const size_t i : {size_t{0}, size_t{2}, size_t{3}}) {
        std::optional<uint256> lock;
        BOOST_REQUIRE(stores[i]->ReadLock({seats.seats.epoch, 0}, lock, error));
        BOOST_REQUIRE(lock == locked_hash);
    }

    // Identical loop copies do not amplify at an unchanged clock. A forged
    // proposal and a well-formed vote signed by the wrong seat do not relay.
    for (size_t i{0}; i < 8; ++i) {
        BOOST_REQUIRE(runtimes[1]->EnqueueWireMessage(i % 2 ? 2 : 0, original) ==
                      flowmesh::QueueResult::ACCEPTED);
    }
    auto forged{*proposal};
    forged.proposer_signature[0] ^= 1;
    auto invalid_proposal{original};
    invalid_proposal.payload = *flowmesh::EncodeProductionProposalPayload(forged);
    BOOST_REQUIRE(runtimes[1]->EnqueueWireMessage(0, invalid_proposal) ==
                  flowmesh::QueueResult::ACCEPTED);
    const auto wrong_signature{flowmesh::SignBlsMicroblockCertificate(
        seats.secrets[3], flowmesh::ProductionCertificateContext(proposal->entry), seats.seats)};
    BOOST_REQUIRE(wrong_signature);
    flowmesh::WireMessage invalid_vote;
    invalid_vote.kind = flowmesh::WireMessageKind::ATTESTATION;
    invalid_vote.header = original.header;
    invalid_vote.payload = *flowmesh::EncodeProductionAttestationPayload({2, *wrong_signature});
    const auto before_bad{runtimes[1]->MarketData(market, std::nullopt, {}, error)};
    BOOST_REQUIRE(before_bad);
    BOOST_REQUIRE(runtimes[1]->EnqueueWireMessage(2, invalid_vote) ==
                  flowmesh::QueueResult::ACCEPTED);
    drain();
    const auto after_bad{runtimes[1]->MarketData(market, std::nullopt, {}, error)};
    BOOST_REQUIRE(after_bad);
    BOOST_CHECK_EQUAL(before_bad->snapshot.runtime.max_verified_attestations,
                      after_bad->snapshot.runtime.max_verified_attestations);
    {
        std::lock_guard<std::mutex> guard{seen_mutex};
        BOOST_CHECK_EQUAL(std::count_if(seen.begin(), seen.end(), [](const auto& item) {
            return item.from == 1 && item.wire.kind == flowmesh::WireMessageKind::PROPOSAL;
        }), forwarded_before);
        BOOST_CHECK(std::none_of(seen.begin(), seen.end(), [&](const auto& item) {
            return item.from == 1 && item.wire.kind == flowmesh::WireMessageKind::ATTESTATION &&
                   item.wire.payload == invalid_vote.payload;
        }));
    }
    for (auto& clock : clocks) clock.m_now += std::chrono::milliseconds{999};
    runtimes[0]->NotifyTick();
    drain();
    BOOST_CHECK_EQUAL(runtimes[0]->MarketStatus(market)->next_sequence, 0U);
    for (auto& clock : clocks) clock.m_now += std::chrono::milliseconds{1};
    // Same-round proposal retry crosses the observer again. The distant
    // signer replies with its exact cached vote; two intermediaries verify
    // and relay it before the endpoint can form a certificate.
    runtimes[0]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        return std::all_of(runtimes.begin(), runtimes.end(), [&](const auto& runtime) {
            return runtime->MarketStatus(market)->next_sequence == 1;
        });
    }));
    drain();
    {
        std::lock_guard<std::mutex> guard{seen_mutex};
        BOOST_REQUIRE(retried_vote);
        BOOST_CHECK(*retried_vote == *dropped_vote);
        for (const size_t from : {size_t{1}, size_t{2}}) {
            BOOST_CHECK(std::any_of(seen.begin(), seen.end(), [&](const auto& item) {
                return item.from == from && item.to + 1 == from &&
                       item.wire.kind == flowmesh::WireMessageKind::ATTESTATION &&
                       item.wire.payload == *dropped_vote;
            }));
        }
        BOOST_CHECK(std::all_of(seen.begin(), seen.end(), [&](const auto& item) {
            return item.wire.kind != flowmesh::WireMessageKind::PROPOSAL ||
                   item.wire.payload == original.payload;
        }));
        BOOST_CHECK_LT(seen.size(), 100U); // bounded copies, no relay storm
    }
    for (size_t i{0}; i < runtimes.size(); ++i) {
        BOOST_CHECK(runtimes[i]->MarketStatus(market)->last_microblock_hash == locked_hash);
        BOOST_CHECK_EQUAL(runtimes[i]->MarketStatus(market)->round, 0U);
        BOOST_CHECK(runtimes[i]->MarketStatus(market)->halt == node::FlowMeshRuntimeHalt::NONE);
        runtimes[i]->Stop();
    }
}

BOOST_AUTO_TEST_CASE(cached_proposer_vote_reaches_late_observer_without_spam)
{
    const uint256 domain{Filled(0x2d)};
    const modern::AssetId asset{Filled(0x4d)};
    const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const auto vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x6d)};
    const SeatFixture seats{Seats(domain, market, 4, 7, 100, Filled(0x71), 132)};
    const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(),
                                        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(seats.seats);
    std::array<FixedClock, 4> clocks;
    std::array<RuntimeKeys, 4> keys;
    keys[0].m_keys[market] = {seats.secrets[0]};
    keys[2].m_keys[market] = {seats.secrets[1]};
    keys[3].m_keys[market] = {seats.secrets[2]};
    std::array<std::unique_ptr<node::FlowMeshProductionStore>, 4> stores;
    RuntimeNetwork network;
    std::array<std::unique_ptr<node::FlowMeshRuntime>, 4> runtimes;
    std::mutex seen_mutex;
    std::optional<flowmesh::WireMessage> original_proposal;
    std::vector<std::vector<unsigned char>> proposer_votes;
    size_t observer_certificates{0};
    struct StopBeforeCapturedState {
        decltype(runtimes)& all;
        ~StopBeforeCapturedState() { for (const auto& runtime : all) if (runtime) runtime->Stop(); }
    } stop{runtimes};
    // The late observer is the only possible certificate assembler:
    // seat0 -- observer -- seat1 -- seat2. The endpoints cannot acquire all
    // three shares and hide whether the observer received seat0's lost vote.
    network.SetFilter([](size_t from, size_t to, const auto& wire) {
        if (!(from + 1 == to || to + 1 == from)) return false;
        if (wire.kind == flowmesh::WireMessageKind::ATTESTATION) {
            const auto vote{flowmesh::DecodeProductionAttestationPayload(wire.payload)};
            if (!vote || (to == 0 && vote->seat_index != 0) ||
                (to >= 2 && vote->seat_index == 0)) return false;
        }
        return true;
    });
    std::string error;
    for (size_t i{0}; i < runtimes.size(); ++i) {
        stores[i] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
            .path = m_args.GetDataDirBase() / fs::PathFromString(
                "flowmesh_cached_proposer_vote_" + std::to_string(i)),
            .cache_bytes = size_t{1} << 20, .wipe_data = true});
        BOOST_REQUIRE_MESSAGE(stores[i]->OpenForMarket(
            domain, market, seats.seats, initial.Root(), error), error);
        node::FlowMeshRuntimeConfig config;
        config.chain = &chain;
        config.keys = &keys[i];
        config.clock = &clocks[i];
        config.round_timeout = std::chrono::hours{1};
        config.relay = [&, i](node::FlowMeshRuntimeRelay relay) {
            {
                std::lock_guard<std::mutex> guard{seen_mutex};
                if (i == 0 && relay.message.kind == flowmesh::WireMessageKind::PROPOSAL &&
                    !original_proposal) original_proposal = relay.message;
                if (i == 0 && relay.message.kind == flowmesh::WireMessageKind::ATTESTATION) {
                    const auto vote{flowmesh::DecodeProductionAttestationPayload(relay.message.payload)};
                    if (vote && vote->seat_index == 0) proposer_votes.push_back(relay.message.payload);
                }
                if (i == 1 && relay.message.kind == flowmesh::WireMessageKind::CERTIFICATE) {
                    ++observer_certificates;
                }
            }
            network.Relay(i, std::move(relay));
            return LegacyRelayResult();
        };
        runtimes[i] = std::make_unique<node::FlowMeshRuntime>(config,
            std::vector<node::FlowMeshRuntimeMarketConfig>{MarketConfig(
                domain, market, treasury, seats.seats, initial, *stores[i], nullptr)});
        network.Set(i, runtimes[i].get(), i == 0);
        BOOST_REQUIRE_MESSAGE(runtimes[i]->Start(error), error);
    }
    const auto drain = [&] {
        for (size_t pass{0}; pass < 4; ++pass) {
            for (const auto& runtime : runtimes) {
                BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
            }
        }
    };
    const auto vote_count = [&] {
        std::lock_guard<std::mutex> guard{seen_mutex};
        return proposer_votes.size();
    };
    // The first proposal and vote are produced with no connected recipients.
    runtimes[0]->NotifyTick();
    drain();
    BOOST_REQUIRE_EQUAL(vote_count(), 1U);
    flowmesh::WireMessage proposal_wire;
    std::vector<unsigned char> original_vote;
    {
        std::lock_guard<std::mutex> guard{seen_mutex};
        BOOST_REQUIRE(original_proposal);
        proposal_wire = *original_proposal;
        original_vote = proposer_votes.front();
    }
    const auto proposal{flowmesh::DecodeProductionProposalPayload(proposal_wire.payload)};
    BOOST_REQUIRE(proposal);
    const auto locked_hash{proposal->entry.GetHash()};
    const auto check_proposer_lock = [&] {
        std::optional<uint256> locked;
        BOOST_REQUIRE(stores[0]->ReadLock({seats.seats.epoch, 0}, locked, error));
        BOOST_REQUIRE(locked);
        BOOST_CHECK(*locked == locked_hash);
    };
    check_proposer_lock();

    // A new peer cannot bypass the shared one-second exact retry interval.
    // Neither the retained proposal nor its cached vote is due yet.
    network.Set(1, runtimes[1].get(), true);
    runtimes[0]->FlowMeshPeerConnected(1);
    drain();
    const auto learned{runtimes[1]->MarketData(market, std::nullopt, {}, error)};
    BOOST_REQUIRE(learned);
    BOOST_CHECK(learned->snapshot.observer_only);
    BOOST_CHECK_EQUAL(learned->snapshot.runtime.candidate_count, 0U);
    BOOST_CHECK_EQUAL(learned->snapshot.runtime.max_verified_attestations, 0U);
    BOOST_CHECK_EQUAL(vote_count(), 1U);
    for (auto& clock : clocks) {
        clock.m_now += node::FlowMeshCommitteeRelayBudget::REPEAT_DELAY - std::chrono::milliseconds{1};
    }
    runtimes[0]->NotifyTick();
    drain();
    BOOST_CHECK_EQUAL(vote_count(), 1U);
    for (auto& clock : clocks) clock.m_now += std::chrono::milliseconds{1};
    runtimes[0]->NotifyTick();
    drain();
    BOOST_REQUIRE_MESSAGE(vote_count() == 2,
        "a due local proposal retry must rebroadcast its exact cached attestation");
    {
        std::lock_guard<std::mutex> guard{seen_mutex};
        BOOST_CHECK(proposer_votes.back() == original_vote);
    }
    auto received{runtimes[1]->MarketData(market, std::nullopt, {}, error)};
    BOOST_REQUIRE(received);
    BOOST_REQUIRE_EQUAL(received->snapshot.runtime.candidate_count, 1U);
    size_t expected_proposer_votes{2};
    // Votes have higher queue priority than proposals. If the first due vote
    // arrived before its candidate, one further exact retry must recover it.
    if (received->snapshot.runtime.max_verified_attestations == 0) {
        for (auto& clock : clocks) clock.m_now += node::FlowMeshCommitteeRelayBudget::REPEAT_DELAY;
        runtimes[0]->NotifyTick();
        drain();
        ++expected_proposer_votes;
        BOOST_REQUIRE_EQUAL(vote_count(), expected_proposer_votes);
        received = runtimes[1]->MarketData(market, std::nullopt, {}, error);
        BOOST_REQUIRE(received);
    }
    BOOST_CHECK_EQUAL(received->snapshot.runtime.max_verified_attestations, 1U);
    BOOST_CHECK_EQUAL(received->snapshot.next_microblock_sequence, 0U);
    check_proposer_lock();

    // Repeated local ticks within that interval do not amplify the cached vote.
    for (size_t i{0}; i < 4; ++i) {
        runtimes[0]->NotifyTick();
        BOOST_REQUIRE(runtimes[0]->WaitForIdle(std::chrono::seconds{2}));
    }
    drain();
    BOOST_CHECK_EQUAL(vote_count(), expected_proposer_votes);
    network.Set(2, runtimes[2].get(), true);
    network.Set(3, runtimes[3].get(), true);
    for (auto& clock : clocks) clock.m_now += node::FlowMeshCommitteeRelayBudget::REPEAT_DELAY;
    runtimes[0]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        return std::all_of(runtimes.begin(), runtimes.end(), [&](const auto& runtime) {
            const auto status{runtime->MarketStatus(market)};
            return status && status->next_sequence == 1 &&
                   status->halt == node::FlowMeshRuntimeHalt::NONE;
        });
    }));
    drain();
    {
        std::lock_guard<std::mutex> guard{seen_mutex};
        BOOST_CHECK_GT(observer_certificates, 0U);
        BOOST_CHECK_EQUAL(proposer_votes.size(), expected_proposer_votes + 1);
        BOOST_CHECK(std::all_of(proposer_votes.begin(), proposer_votes.end(),
            [&](const auto& vote) { return vote == original_vote; }));
    }
    check_proposer_lock();
    for (size_t i{0}; i < runtimes.size(); ++i) {
        BOOST_CHECK(runtimes[i]->MarketStatus(market)->last_microblock_hash == locked_hash);
        std::optional<node::StoredProductionEntry> committed;
        BOOST_REQUIRE(stores[i]->ReadEntry(0, seats.seats, committed, error));
        BOOST_REQUIRE(committed);
        BOOST_CHECK(flowmesh::CheckProductionEntryCertificate(
            committed->entry, seats.seats, committed->certificate) ==
            flowmesh::BlsCertificateCheck::OK);
    }
}

BOOST_AUTO_TEST_CASE(cached_proposer_vote_retries_cover_all_owned_seats)
{
    const uint256 domain{Filled(0x2e)};
    const modern::AssetId asset{Filled(0x4e)};
    const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const auto vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x6e)};
    const SeatFixture seats{Seats(domain, market, 16, 7, 100, Filled(0x71), 133)};
    const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(),
                                        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(seats.seats);
    FixedClock clock;
    RuntimeKeys keys;
    keys.m_keys[market] = std::vector<bls::SecretKey>{
        seats.secrets.begin(), seats.secrets.begin() + 10};
    node::FlowMeshProductionStore store{DBParams{
        .path = m_args.GetDataDirBase() / "flowmesh_cached_vote_fairness",
        .cache_bytes = size_t{1} << 20, .wipe_data = true}};
    std::string error;
    BOOST_REQUIRE_MESSAGE(store.OpenForMarket(
        domain, market, seats.seats, initial.Root(), error), error);
    std::mutex votes_mutex;
    std::vector<std::vector<unsigned char>> votes;
    size_t critical_relays{0};
    node::FlowMeshRuntimeConfig config;
    config.chain = &chain;
    config.keys = &keys;
    config.clock = &clock;
    config.round_timeout = std::chrono::hours{1};
    config.relay = [&](node::FlowMeshRuntimeRelay relay) {
        if (relay.message.kind == flowmesh::WireMessageKind::PROPOSAL ||
            relay.message.kind == flowmesh::WireMessageKind::ATTESTATION) {
            std::lock_guard<std::mutex> guard{votes_mutex};
            ++critical_relays;
            if (relay.message.kind == flowmesh::WireMessageKind::ATTESTATION) {
                votes.push_back(relay.message.payload);
            }
        }
        return LegacyRelayResult();
    };
    node::FlowMeshRuntime runtime{config, {MarketConfig(
        domain, market, treasury, seats.seats, initial, store, nullptr)}};
    BOOST_REQUIRE_MESSAGE(runtime.Start(error), error);
    runtime.NotifyTick();
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    std::map<uint32_t, std::vector<unsigned char>> originals;
    {
        std::lock_guard<std::mutex> guard{votes_mutex};
        BOOST_REQUIRE_EQUAL(votes.size(), 10U);
        for (const auto& payload : votes) {
            const auto vote{flowmesh::DecodeProductionAttestationPayload(payload)};
            BOOST_REQUIRE(vote);
            BOOST_REQUIRE(originals.emplace(vote->seat_index, payload).second);
        }
    }
    const auto first{runtime.MarketData(market, std::nullopt, {}, error)};
    BOOST_REQUIRE(first);
    BOOST_REQUIRE_EQUAL(first->snapshot.quorum_required, 11U);
    BOOST_REQUIRE_EQUAL(first->snapshot.runtime.max_verified_attestations, 10U);
    BOOST_REQUIRE(first->snapshot.runtime.local_locked_candidate);
    const auto locked_hash{*first->snapshot.runtime.local_locked_candidate};
    std::set<uint32_t> replayed;
    for (size_t tick{0}; tick < 2; ++tick) {
        size_t begin, critical_before;
        {
            std::lock_guard<std::mutex> guard{votes_mutex};
            begin = votes.size();
            critical_before = critical_relays;
        }
        clock.m_now += node::FlowMeshCommitteeRelayBudget::REPEAT_DELAY;
        runtime.NotifyTick();
        BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
        std::lock_guard<std::mutex> guard{votes_mutex};
        // The proposal and cached shares now use the same eight-object
        // retry budget. Fairness still requires every owned seat to reappear.
        BOOST_REQUIRE_EQUAL(critical_relays - critical_before,
                            node::FlowMeshCommitteeRelayBudget::MARKET_MESSAGES);
        BOOST_REQUIRE_LE(votes.size() - begin,
                         node::FlowMeshCommitteeRelayBudget::MARKET_MESSAGES);
        for (size_t i{begin}; i < votes.size(); ++i) {
            const auto vote{flowmesh::DecodeProductionAttestationPayload(votes[i])};
            BOOST_REQUIRE(vote);
            const auto original{originals.find(vote->seat_index)};
            BOOST_REQUIRE(original != originals.end());
            BOOST_CHECK(votes[i] == original->second);
            replayed.insert(vote->seat_index);
        }
    }
    // Ten owned seats exceed the eight-share retry budget. Always starting
    // at seat zero would starve the last two at every eligible local tick.
    BOOST_CHECK_EQUAL(replayed.size(), originals.size());
    BOOST_CHECK_EQUAL(runtime.MarketStatus(market)->next_sequence, 0U);
    BOOST_CHECK(runtime.MarketStatus(market)->halt == node::FlowMeshRuntimeHalt::NONE);
    std::optional<uint256> locked;
    BOOST_REQUIRE(store.ReadLock({seats.seats.epoch, 0}, locked, error));
    BOOST_REQUIRE(locked);
    BOOST_CHECK(*locked == locked_hash);

    // Disarming removes signing eligibility even though valid cached votes
    // and their permanent lock remain. The provider is changed only at idle.
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    const auto votes_before_disarm{votes.size()};
    const auto critical_before_disarm{critical_relays};
    keys.m_keys.clear();
    clock.m_now += node::FlowMeshCommitteeRelayBudget::REPEAT_DELAY;
    runtime.NotifyTick();
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    {
        std::lock_guard<std::mutex> guard{votes_mutex};
        BOOST_CHECK_EQUAL(votes.size(), votes_before_disarm);
        BOOST_CHECK_EQUAL(critical_relays, critical_before_disarm);
    }
    BOOST_CHECK(runtime.MarketStatus(market)->observer_only);
    BOOST_CHECK_EQUAL(runtime.MarketStatus(market)->next_sequence, 0U);
    BOOST_REQUIRE(store.ReadLock({seats.seats.epoch, 0}, locked, error));
    BOOST_CHECK(locked == locked_hash);
    runtime.Stop();
}

BOOST_AUTO_TEST_CASE(cached_delivery_two_markets_do_not_starve_budget_denied_seats)
{
    const uint256 domain{Filled(0x2f)};
    const std::array<modern::AssetId, 2> assets{Filled(0x4f), Filled(0x50)};
    const std::array<flowmesh::MarketId, 2> markets{
        *flowmesh::ComputeFlowMeshMarketId(domain, assets[0]),
        *flowmesh::ComputeFlowMeshMarketId(domain, assets[1])};
    const std::array<SeatFixture, 2> seats{
        Seats(domain, markets[0], 16, 7, 100, Filled(0x71), 134),
        Seats(domain, markets[1], 16, 7, 100, Filled(0x71), 135)};
    RuntimeChain chain;
    chain.m_domain = domain;
    RuntimeKeys keys;
    FixedClock clock;
    std::array<std::unique_ptr<node::FlowMeshProductionStore>, 2> stores;
    std::vector<node::FlowMeshRuntimeMarketConfig> market_configs;
    std::string error;
    for (size_t i{0}; i < markets.size(); ++i) {
        chain.Add(seats[i].seats);
        keys.m_keys[markets[i]] = std::vector<bls::SecretKey>{
            seats[i].secrets.begin(), seats[i].secrets.begin() + 10};
        const flowmesh::FlowMeshState initial{
            *flowmesh::ComputeFlowMeshVaultId(domain, markets[i]), assets[i],
            modern::NativeAsset(), flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
        stores[i] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
            .path = m_args.GetDataDirBase() / fs::PathFromString(
                "flowmesh_two_market_delivery_" + std::to_string(i)),
            .cache_bytes = size_t{1} << 20, .wipe_data = true});
        BOOST_REQUIRE_MESSAGE(stores[i]->OpenForMarket(
            domain, markets[i], seats[i].seats, initial.Root(), error), error);
        market_configs.push_back(MarketConfig(domain, markets[i], Filled(0x6f),
            seats[i].seats, initial, *stores[i], nullptr));
    }
    std::mutex relay_mutex;
    std::map<flowmesh::MarketId, std::vector<flowmesh::WireMessage>> relays;
    node::FlowMeshRuntimeConfig config;
    config.chain = &chain;
    config.keys = &keys;
    config.clock = &clock;
    config.round_timeout = std::chrono::hours{1};
    config.relay = [&](node::FlowMeshRuntimeRelay relay) {
        if (relay.message.kind == flowmesh::WireMessageKind::PROPOSAL ||
            relay.message.kind == flowmesh::WireMessageKind::ATTESTATION) {
            std::lock_guard<std::mutex> guard{relay_mutex};
            relays[relay.message.header.market_id].push_back(std::move(relay.message));
        }
        return LegacyRelayResult();
    };
    node::FlowMeshRuntime runtime{config, market_configs};
    BOOST_REQUIRE_MESSAGE(runtime.Start(error), error);
    runtime.NotifyTick();
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    std::map<flowmesh::MarketId, std::map<uint32_t, std::vector<unsigned char>>> originals;
    std::map<flowmesh::MarketId, std::set<uint32_t>> replayed;
    std::array<uint256, 2> locked_hashes;
    for (size_t i{0}; i < markets.size(); ++i) {
        const auto data{runtime.MarketData(markets[i], std::nullopt, {}, error)};
        BOOST_REQUIRE(data);
        BOOST_REQUIRE_EQUAL(data->snapshot.quorum_required, 11U);
        BOOST_REQUIRE_EQUAL(data->snapshot.runtime.max_verified_attestations, 10U);
        BOOST_REQUIRE(data->snapshot.runtime.local_locked_candidate);
        locked_hashes[i] = *data->snapshot.runtime.local_locked_candidate;
        std::lock_guard<std::mutex> guard{relay_mutex};
        BOOST_REQUIRE_EQUAL(relays[markets[i]].size(), 11U); // proposal plus ten shares
        for (const auto& wire : relays[markets[i]]) {
            if (wire.kind != flowmesh::WireMessageKind::ATTESTATION) continue;
            const auto vote{flowmesh::DecodeProductionAttestationPayload(wire.payload)};
            BOOST_REQUIRE(vote);
            BOOST_REQUIRE(originals[markets[i]].emplace(vote->seat_index, wire.payload).second);
        }
        BOOST_REQUIRE_EQUAL(originals[markets[i]].size(), 10U);
    }
    for (size_t round{0}; round < 2; ++round) {
        std::map<flowmesh::MarketId, size_t> before;
        {
            std::lock_guard<std::mutex> guard{relay_mutex};
            for (const auto& market : markets) before[market] = relays[market].size();
        }
        clock.m_now += node::FlowMeshCommitteeRelayBudget::REPEAT_DELAY;
        runtime.NotifyTick();
        BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
        std::lock_guard<std::mutex> guard{relay_mutex};
        size_t total{0};
        for (const auto& market : markets) {
            const auto count{relays[market].size() - before[market]};
            BOOST_REQUIRE_EQUAL(count, node::FlowMeshCommitteeRelayBudget::MARKET_MESSAGES);
            total += count;
            for (size_t i{before[market]}; i < relays[market].size(); ++i) {
                const auto& wire{relays[market][i]};
                if (wire.kind != flowmesh::WireMessageKind::ATTESTATION) continue;
                const auto vote{flowmesh::DecodeProductionAttestationPayload(wire.payload)};
                BOOST_REQUIRE(vote);
                const auto original{originals[market].find(vote->seat_index)};
                BOOST_REQUIRE(original != originals[market].end());
                BOOST_CHECK(wire.payload == original->second);
                replayed[market].insert(vote->seat_index);
            }
        }
        BOOST_CHECK_LE(total, node::FlowMeshCommitteeRelayBudget::GLOBAL_MESSAGES);
    }
    for (size_t i{0}; i < markets.size(); ++i) {
        BOOST_CHECK_EQUAL(replayed[markets[i]].size(), 10U);
        BOOST_CHECK_EQUAL(runtime.MarketStatus(markets[i])->next_sequence, 0U);
        BOOST_CHECK(runtime.MarketStatus(markets[i])->halt == node::FlowMeshRuntimeHalt::NONE);
        std::optional<uint256> locked;
        BOOST_REQUIRE(stores[i]->ReadLock({seats[i].seats.epoch, 0}, locked, error));
        BOOST_REQUIRE(locked);
        BOOST_CHECK(*locked == locked_hashes[i]);
    }
}

BOOST_AUTO_TEST_CASE(committee_forwarding_paces_verified_repeats_and_denied_budget)
{
    const uint256 domain{Filled(0x1e)};
    const modern::AssetId asset{Filled(0x3e)};
    const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const auto vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x5e)};
    const SeatFixture seats{Seats(domain, market, 20, 7, 100, Filled(0x71), 125)};
    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(seats.seats);
    FixedClock clock;
    RuntimeKeys observer;
    const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(),
                                        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    node::FlowMeshProductionStore store{DBParams{
        .path = m_args.GetDataDirBase() / "flowmesh_runtime_committee_pacing",
        .cache_bytes = size_t{1} << 20, .wipe_data = true}};
    std::string error;
    BOOST_REQUIRE(store.OpenForMarket(domain, market, seats.seats, initial.Root(), error));
    flowmesh::ProductionEpochGate gate{domain, market, seats.seats};
    flowmesh::ProductionEntryCheck check;
    const auto built{flowmesh::BuildProductionExecutionEntry(
        initial, domain, market, seats.seats, gate, 0, 0, {},
        {100, Filled(0x71)}, {chain.TipHeight(), std::nullopt, &chain},
        treasury, {}, nullptr, check)};
    BOOST_REQUIRE(built);
    flowmesh::ProductionProposalEnvelope proposal;
    proposal.entry = built->entry;
    const auto proposal_wire = [&](const uint32_t round) {
        proposal.round = round;
        proposal.proposer_seat_index = flowmesh::ProductionProposerSeatIndex(0, round, seats.seats.Size());
        const auto digest{flowmesh::ProductionProposalDigest(proposal.entry, round)};
        proposal.proposer_signature = seats.secrets[proposal.proposer_seat_index]
            .Sign(std::span<const unsigned char>{digest.begin(), 32}).Compressed();
        flowmesh::WireMessage wire;
        wire.kind = flowmesh::WireMessageKind::PROPOSAL;
        wire.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1, market, seats.seats.epoch, 0};
        wire.payload = *flowmesh::EncodeProductionProposalPayload(proposal);
        return wire;
    };
    std::mutex relay_mutex;
    std::vector<node::FlowMeshRuntimeRelay> relays;
    node::FlowMeshRuntimeConfig config;
    config.chain = &chain;
    config.keys = &observer;
    config.clock = &clock;
    config.round_timeout = std::chrono::hours{1};
    config.relay = [&](node::FlowMeshRuntimeRelay relay) {
        if (relay.message.kind != flowmesh::WireMessageKind::PROPOSAL &&
            relay.message.kind != flowmesh::WireMessageKind::ATTESTATION) return LegacyRelayResult();
        std::lock_guard<std::mutex> guard{relay_mutex};
        relays.push_back(std::move(relay));
        return LegacyRelayResult();
    };
    node::FlowMeshRuntime runtime{config, {MarketConfig(
        domain, market, treasury, seats.seats, initial, store, nullptr)}};
    BOOST_REQUIRE(runtime.Start(error));
    const auto submit = [&](const flowmesh::WireMessage& wire) {
        BOOST_REQUIRE(runtime.EnqueueWireMessage(77, wire) == flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    };
    const auto count = [&] {
        std::lock_guard<std::mutex> guard{relay_mutex};
        return relays.size();
    };
    submit(proposal_wire(0));
    BOOST_REQUIRE_EQUAL(count(), 1U);
    std::vector<flowmesh::WireMessage> votes;
    for (uint32_t i{0}; i < 8; ++i) {
        const auto signature{flowmesh::SignBlsMicroblockCertificate(
            seats.secrets[i], flowmesh::ProductionCertificateContext(built->entry), seats.seats)};
        BOOST_REQUIRE(signature);
        flowmesh::WireMessage vote;
        vote.kind = flowmesh::WireMessageKind::ATTESTATION;
        vote.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1, market, seats.seats.epoch, 0};
        vote.payload = *flowmesh::EncodeProductionAttestationPayload({i, *signature});
        votes.push_back(vote);
        submit(vote);
    }
    BOOST_CHECK_EQUAL(count(), 8U); // proposal + seven votes; eighth vote denied
    BOOST_CHECK_EQUAL(runtime.MarketData(market, std::nullopt, {}, error)->snapshot.runtime.max_verified_attestations, 8U);
    submit(proposal_wire(1)); // valid next round does not refill identity cooldown
    BOOST_CHECK_EQUAL(runtime.MarketStatus(market)->round, 1U);
    BOOST_CHECK_EQUAL(count(), 8U);
    clock.m_now += std::chrono::milliseconds{250};
    submit(votes.back()); // batch refill does not erase denied-attempt cooldown
    BOOST_CHECK_EQUAL(count(), 8U);
    clock.m_now -= std::chrono::milliseconds{1};
    submit(votes.front()); // a backward clock also grants no opportunity
    BOOST_CHECK_EQUAL(count(), 8U);
    clock.m_now += std::chrono::milliseconds{751};
    submit(votes.back());
    BOOST_CHECK_EQUAL(count(), 9U);
    for (size_t i{0}; i < 12; ++i) submit(votes.back());
    BOOST_CHECK_EQUAL(count(), 9U); // exact same-payload cycles are paced
    submit(votes.front());
    submit(proposal_wire(1));
    BOOST_CHECK_EQUAL(count(), 11U); // exact known vote and round1 payload retry
    {
        std::lock_guard<std::mutex> guard{relay_mutex};
        BOOST_CHECK(relays[8].message.payload == votes.back().payload);
        BOOST_CHECK(relays[9].message.payload == votes.front().payload);
        for (const auto& relay : relays) {
            BOOST_CHECK(!relay.peer);
            BOOST_CHECK(relay.exclude_peer == 77);
        }
    }
    // A known seat's vote for a second valid candidate is equivocation, not
    // a fresh relay identity. No additional verified vote or forwarding.
    const auto other{flowmesh::BuildProductionExecutionEntry(
        initial, domain, market, seats.seats, gate, 0, 0, {},
        chain.m_current, {chain.TipHeight(), std::nullopt, &chain},
        treasury, {}, nullptr, check)};
    BOOST_REQUIRE(other);
    proposal.entry = other->entry;
    submit(proposal_wire(1));
    BOOST_REQUIRE_EQUAL(runtime.MarketData(market, std::nullopt, {}, error)->snapshot.runtime.candidate_count, 2U);
    const auto other_signature{flowmesh::SignBlsMicroblockCertificate(
        seats.secrets[0], flowmesh::ProductionCertificateContext(other->entry), seats.seats)};
    BOOST_REQUIRE(other_signature);
    auto conflicting{votes.front()};
    conflicting.payload = *flowmesh::EncodeProductionAttestationPayload({0, *other_signature});
    const auto before_conflict{count()};
    submit(conflicting);
    BOOST_CHECK_EQUAL(count(), before_conflict);
    BOOST_CHECK_EQUAL(runtime.MarketData(market, std::nullopt, {}, error)->snapshot.runtime.max_verified_attestations, 8U);
    BOOST_CHECK_EQUAL(runtime.MarketStatus(market)->next_sequence, 0U);
    runtime.Stop();
}

BOOST_AUTO_TEST_CASE(committee_forwarding_fresh_pause_prevents_local_vote)
{
    const uint256 domain{Filled(0x1f)};
    const modern::AssetId asset{Filled(0x3f)};
    const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const auto vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x5f)};
    const SeatFixture seats{Seats(domain, market, 4, 7, 100, Filled(0x71), 126)};
    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(seats.seats);
    FixedClock clock;
    RuntimeKeys keys;
    keys.m_keys[market] = {seats.secrets[1]};
    const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(),
                                        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    node::FlowMeshProductionStore store{DBParams{
        .path = m_args.GetDataDirBase() / "flowmesh_runtime_committee_fresh_pause",
        .cache_bytes = size_t{1} << 20, .wipe_data = true}};
    std::string error;
    BOOST_REQUIRE(store.OpenForMarket(domain, market, seats.seats, initial.Root(), error));
    flowmesh::ProductionEpochGate gate{domain, market, seats.seats};
    flowmesh::ProductionEntryCheck check;
    const auto built{flowmesh::BuildProductionExecutionEntry(
        initial, domain, market, seats.seats, gate, 0, 0, {},
        {100, Filled(0x71)}, {chain.TipHeight(), std::nullopt, &chain},
        treasury, {}, nullptr, check)};
    BOOST_REQUIRE(built);
    flowmesh::ProductionProposalEnvelope proposal;
    proposal.entry = built->entry;
    const auto digest{flowmesh::ProductionProposalDigest(proposal.entry, 0)};
    proposal.proposer_signature = seats.secrets[0]
        .Sign(std::span<const unsigned char>{digest.begin(), 32}).Compressed();
    flowmesh::WireMessage wire;
    wire.kind = flowmesh::WireMessageKind::PROPOSAL;
    wire.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1, market, seats.seats.epoch, 0};
    wire.payload = *flowmesh::EncodeProductionProposalPayload(proposal);
    std::mutex relay_mutex;
    size_t committee_relays{0};
    node::FlowMeshRuntimeConfig config;
    config.chain = &chain;
    config.keys = &keys;
    config.clock = &clock;
    config.relay = [&](node::FlowMeshRuntimeRelay relay) {
        if (relay.message.kind == flowmesh::WireMessageKind::PROPOSAL ||
            relay.message.kind == flowmesh::WireMessageKind::ATTESTATION) {
            std::lock_guard<std::mutex> guard{relay_mutex};
            ++committee_relays;
        }
        return LegacyRelayResult();
    };
    node::FlowMeshRuntime runtime{config, {MarketConfig(
        domain, market, treasury, seats.seats, initial, store, nullptr)}};
    BOOST_REQUIRE(runtime.Start(error));
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    // Existing proposal policy and candidate evaluation pass. Only the new
    // third check observes PAUSED, after the durable retain but before signing.
    chain.PauseAfterTransitionChecks(2);
    BOOST_REQUIRE(runtime.EnqueueWireMessage(0, wire) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    const auto data{runtime.MarketData(market, std::nullopt, {}, error)};
    BOOST_REQUIRE(data);
    BOOST_CHECK(data->snapshot.runtime.local_locked_candidate == proposal.entry.GetHash());
    BOOST_CHECK_EQUAL(data->snapshot.runtime.max_verified_attestations, 0U);
    BOOST_CHECK(runtime.MarketStatus(market)->paused);
    BOOST_CHECK_EQUAL(runtime.MarketStatus(market)->next_sequence, 0U);
    {
        std::lock_guard<std::mutex> guard{relay_mutex};
        BOOST_CHECK_EQUAL(committee_relays, 0U);
    }
    runtime.Stop();
}

BOOST_AUTO_TEST_CASE(duplicate_action_forwarding_recovers_missing_tail_through_informed_middle)
{
    const uint256 domain{Filled(0x2a)};
    const modern::AssetId asset{Filled(0x4a)};
    const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const auto vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x6a)};
    const SeatFixture seats{Seats(domain, market, 4, 7, 100, Filled(0x71), 128)};
    CKey account_key;
    account_key.MakeNewKey(true);
    const auto account{flowmesh::AccountForKey(XOnlyPubKey{account_key.GetPubKey()})};
    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(seats.seats);
    FixedClock clock;
    MapDeposits deposits;
    deposits.required_anchor = chain.m_current;
    const COutPoint outpoint{Txid::FromUint256(Filled(0x9a)), 0};
    deposits.entries.emplace(outpoint, flowmesh::DepositInfo{asset, 100, account});
    const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(),
                                        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    std::array<RuntimeKeys, 3> keys;
    std::array<std::unique_ptr<node::FlowMeshProductionStore>, 3> stores;
    RuntimeNetwork network;
    std::mutex seen_mutex;
    bool drop_tail{true};
    std::vector<std::vector<unsigned char>> middle_forwards;
    std::array<std::unique_ptr<node::FlowMeshRuntime>, 3> runtimes;
    network.SetFilter([&](const size_t from, const size_t to, const auto& wire) {
        if (!(from + 1 == to || to + 1 == from)) return false;
        if (wire.kind != flowmesh::WireMessageKind::ACTION || wire.header.sequence != 2) return true;
        const auto action{flowmesh::DecodeProductionActionPayload(wire.payload)};
        if (!action) return false;
        std::lock_guard<std::mutex> guard{seen_mutex};
        if (from == 1 && to == 0) {
            middle_forwards.push_back(wire.payload);
            if (drop_tail && action->sequence == 8) return false;
        }
        return true;
    });
    std::string error;
    for (size_t i{0}; i < runtimes.size(); ++i) {
        keys[i].m_keys[market] = {seats.secrets[i]};
        stores[i] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
            .path = m_args.GetDataDirBase() / fs::PathFromString(
                        "flowmesh_runtime_duplicate_tail_" + std::to_string(i)),
            .cache_bytes = size_t{1} << 20, .wipe_data = true});
        BOOST_REQUIRE(stores[i]->OpenForMarket(domain, market, seats.seats, initial.Root(), error));
        node::FlowMeshRuntimeConfig config{&chain, &keys[i], &clock,
            [&network, i](node::FlowMeshRuntimeRelay relay) {
                network.Relay(i, std::move(relay));
                return LegacyRelayResult();
            },
            std::chrono::hours{1}, std::chrono::seconds{60}, {}};
        runtimes[i] = std::make_unique<node::FlowMeshRuntime>(config,
            std::vector<node::FlowMeshRuntimeMarketConfig>{MarketConfig(
                domain, market, treasury, seats.seats, initial, *stores[i], &deposits)});
        network.Set(i, runtimes[i].get(), true);
        BOOST_REQUIRE(runtimes[i]->Start(error));
    }
    const auto drain = [&] {
        for (size_t pass{0}; pass < 3; ++pass) {
            for (const auto& runtime : runtimes) BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
        }
    };
    const auto all_at = [&](const uint64_t sequence) {
        return std::all_of(runtimes.begin(), runtimes.end(), [&](const auto& runtime) {
            return runtime->MarketStatus(market)->next_sequence == sequence;
        });
    };
    runtimes[0]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] { return all_at(1); }));
    BOOST_REQUIRE(runtimes[1]->SubmitLocalAction(market, Deposit(outpoint)) == flowmesh::QueueResult::ACCEPTED);
    drain();
    runtimes[1]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] { return all_at(2); }));
    drain();
    std::vector<flowmesh::Action> actions;
    std::vector<std::vector<unsigned char>> payloads;
    for (uint64_t sequence{0}; sequence < 9; ++sequence) {
        flowmesh::Action action;
        action.signer = account;
        action.sequence = sequence;
        action.type = static_cast<uint8_t>(flowmesh::ActionType::SUBMIT_ASK);
        action.curve = {{1, 10}};
        BOOST_REQUIRE(flowmesh::SignAction(account_key, domain, initial.ConfigId(), action));
        actions.push_back(action);
        payloads.push_back(*flowmesh::EncodeProductionActionPayload(action));
        BOOST_REQUIRE(runtimes[2]->SubmitLocalAction(market, action) == flowmesh::QueueResult::ACCEPTED);
        drain();
    }
    BOOST_CHECK_EQUAL(runtimes[0]->MarketStatus(market)->pending_actions, 8U);
    BOOST_CHECK_EQUAL(runtimes[1]->MarketStatus(market)->pending_actions, 9U);
    runtimes[2]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        return runtimes[2]->MarketData(market, std::nullopt, {}, error)->snapshot.runtime.max_verified_attestations == 2;
    }));
    drain();
    BOOST_CHECK_GT(runtimes[0]->MarketData(market, std::nullopt, {}, error)->snapshot.runtime.proposals_missing_evidence, 0U);
    BOOST_CHECK(!runtimes[0]->MarketData(market, std::nullopt, {}, error)->snapshot.runtime.local_locked_candidate);
    std::optional<uint256> locked;
    BOOST_REQUIRE(stores[2]->ReadLock({seats.seats.epoch, 2}, locked, error));
    BOOST_REQUIRE(locked);
    const auto wire_for = [&](const std::vector<unsigned char>& payload, const uint64_t sequence = 2) {
        return flowmesh::WireMessage{flowmesh::WireMessageKind::ACTION,
            {flowmesh::FLOWMESH_WIRE_VERSION_V1, market, seats.seats.epoch, sequence}, payload};
    };
    auto bad_credential{actions.back()};
    bad_credential.credential.back() ^= 1;
    BOOST_REQUIRE(runtimes[1]->EnqueueWireMessage(2,
        wire_for(*flowmesh::EncodeProductionActionPayload(bad_credential))) == flowmesh::QueueResult::ACCEPTED);
    auto alternate_credential{actions.back()};
    std::array<unsigned char, 64> alternate_signature;
    BOOST_REQUIRE(account_key.SignSchnorr(
        flowmesh::ActionSignatureDigest(domain, initial.ConfigId(), alternate_credential),
        alternate_signature, nullptr, Filled(0xb4)));
    std::copy(alternate_signature.begin(), alternate_signature.end(),
              alternate_credential.credential.begin() + 32);
    const flowmesh::SchnorrActionAuthenticator authenticator{domain, initial.ConfigId()};
    BOOST_REQUIRE(authenticator.Authenticate(alternate_credential));
    BOOST_REQUIRE(alternate_credential.Id() == actions.back().Id());
    const auto alternate_payload{*flowmesh::EncodeProductionActionPayload(alternate_credential)};
    BOOST_REQUIRE(alternate_payload != payloads.back());
    BOOST_REQUIRE(runtimes[1]->EnqueueWireMessage(2,
        wire_for(alternate_payload)) == flowmesh::QueueResult::ACCEPTED);
    auto conflicting{actions.back()};
    conflicting.curve = {{2, 10}};
    conflicting.credential.clear();
    BOOST_REQUIRE(flowmesh::SignAction(account_key, domain, initial.ConfigId(), conflicting));
    BOOST_REQUIRE(runtimes[1]->EnqueueWireMessage(2,
        wire_for(*flowmesh::EncodeProductionActionPayload(conflicting))) == flowmesh::QueueResult::ACCEPTED);
    drain();
    {
        std::lock_guard<std::mutex> guard{seen_mutex};
        middle_forwards.clear();
        drop_tail = false;
    }
    runtimes[1]->NotifyTick();
    drain();
    {
        std::lock_guard<std::mutex> guard{seen_mutex};
        BOOST_CHECK(middle_forwards.empty()); // invalid/changed duplicates admitted no obligation
    }
    // The proposer retains nine exact actions, but its sweep is eight then
    // one. Queue both chunks at the already-informed middle before draining.
    clock.m_now += std::chrono::milliseconds{250};
    runtimes[2]->NotifyTick();
    drain();
    clock.m_now += std::chrono::milliseconds{250};
    runtimes[2]->NotifyTick();
    drain();
    for (size_t batch{0}; batch < 3; ++batch) {
        if (batch != 0) clock.m_now += std::chrono::milliseconds{250};
        runtimes[1]->NotifyTick();
        drain();
        std::lock_guard<std::mutex> guard{seen_mutex};
        BOOST_CHECK_EQUAL(middle_forwards.size(), std::min(size_t{9}, (batch + 1) * 4));
    }
    {
        std::lock_guard<std::mutex> guard{seen_mutex};
        BOOST_CHECK(middle_forwards == payloads); // fair FIFO, including missing ninth action
    }
    BOOST_CHECK_EQUAL(runtimes[0]->MarketStatus(market)->pending_actions, 9U);
    clock.m_now += node::FlowMeshCommitteeRelayBudget::REPEAT_DELAY;
    runtimes[2]->NotifyTick(); // normal proposal retry after credentials reached endpoint
    BOOST_REQUIRE(WaitUntil([&] { return all_at(3); }));
    drain();
    for (const auto& runtime : runtimes) {
        BOOST_CHECK(runtime->MarketStatus(market)->last_microblock_hash == *locked);
        BOOST_CHECK_EQUAL(runtime->StateSnapshot(market)->NextSequence(account), 9U);
        BOOST_CHECK_EQUAL(runtime->MarketStatus(market)->pending_actions, 0U);
    }
    std::optional<node::StoredProductionEntry> committed;
    BOOST_REQUIRE(stores[2]->ReadEntry(2, seats.seats, committed, error));
    BOOST_REQUIRE(committed);
    BOOST_CHECK_EQUAL(committed->entry.actions.size(), 9U);
    // Cleared obligations do not recur, and the old unsigned header is not
    // rewritten to a fresh head. No action is executed a second time.
    BOOST_REQUIRE(runtimes[1]->EnqueueWireMessage(2, wire_for(payloads.back())) == flowmesh::QueueResult::ACCEPTED);
    drain();
    clock.m_now += std::chrono::seconds{2};
    runtimes[1]->NotifyTick();
    drain();
    {
        std::lock_guard<std::mutex> guard{seen_mutex};
        BOOST_CHECK_EQUAL(middle_forwards.size(), 9U);
    }
    BOOST_CHECK(all_at(3));
    for (auto& runtime : runtimes) runtime->Stop();
}

BOOST_AUTO_TEST_CASE(duplicate_action_forwarding_fifo_market_fairness_and_cleanup)
{
    const uint256 domain{Filled(0x2b)};
    const uint256 treasury{Filled(0x6b)};
    RuntimeChain chain;
    chain.m_domain = domain;
    FixedClock clock;
    RuntimeKeys observer;
    MapDeposits deposits;
    deposits.required_anchor = chain.m_current;
    std::vector<std::unique_ptr<node::FlowMeshProductionStore>> stores;
    std::vector<node::FlowMeshRuntimeMarketConfig> configs;
    std::vector<flowmesh::WireMessage> originals;
    std::vector<flowmesh::MarketId> markets;
    std::string error;
    for (size_t m{0}; m < 2; ++m) {
        const modern::AssetId asset{Filled(static_cast<unsigned char>(0x4b + m))};
        const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
        markets.push_back(market);
        const auto vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
        const auto seats{Seats(domain, market, 4, 7, 100, Filled(0x71), 129 + m)};
        chain.Add(seats.seats);
        const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(), flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
        stores.push_back(std::make_unique<node::FlowMeshProductionStore>(DBParams{
            .path = m_args.GetDataDirBase() / fs::PathFromString("flowmesh_runtime_duplicate_fair_" + std::to_string(m)),
            .cache_bytes = size_t{1} << 20, .wipe_data = true}));
        BOOST_REQUIRE(stores.back()->OpenForMarket(domain, market, seats.seats, initial.Root(), error));
        configs.push_back(MarketConfig(domain, market, treasury, seats.seats, initial, *stores.back(), &deposits));
        for (size_t a{0}; a < 6; ++a) {
            const COutPoint outpoint{Txid::FromUint256(Filled(static_cast<unsigned char>(0xa0 + m * 6 + a))), 0};
            deposits.entries.emplace(outpoint, flowmesh::DepositInfo{asset, 1, Filled(0x7b)});
            originals.push_back({flowmesh::WireMessageKind::ACTION,
                {flowmesh::FLOWMESH_WIRE_VERSION_V1, market, seats.seats.epoch, 0},
                *flowmesh::EncodeProductionActionPayload(Deposit(outpoint))});
        }
    }
    std::mutex relay_mutex;
    std::vector<node::FlowMeshRuntimeRelay> relays;
    node::FlowMeshRuntimeConfig config{&chain, &observer, &clock,
        [&](node::FlowMeshRuntimeRelay relay) {
            if (relay.message.kind != flowmesh::WireMessageKind::ACTION) return LegacyRelayResult();
            std::lock_guard<std::mutex> guard{relay_mutex};
            relays.push_back(std::move(relay));
            return LegacyRelayResult();
        }, std::chrono::hours{1}, std::chrono::seconds{60}, {}};
    node::FlowMeshRuntime runtime{config, configs};
    BOOST_REQUIRE(runtime.Start(error));
    const auto submit = [&](const flowmesh::WireMessage& wire, const int64_t peer) {
        BOOST_REQUIRE(runtime.EnqueueWireMessage(peer, wire) == flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    };
    const auto tick = [&] {
        runtime.NotifyTick();
        BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    };
    for (const auto& wire : originals) submit(wire, 77); // normal initial relay
    {
        std::lock_guard<std::mutex> guard{relay_mutex};
        BOOST_REQUIRE_EQUAL(relays.size(), 12U);
        relays.clear();
    }
    for (const auto& wire : originals) {
        submit(wire, 77);
        submit(wire, 88); // coalesce, retain first origin/FIFO position
    }
    tick();
    flowmesh::MarketId first;
    {
        std::lock_guard<std::mutex> guard{relay_mutex};
        BOOST_REQUIRE_EQUAL(relays.size(), 4U);
        first = relays.front().message.header.market_id;
        for (const auto& relay : relays) BOOST_CHECK(relay.message.header.market_id == first);
    }
    // No repeat-driven or unchanged-clock refill; a completed obligation is
    // not recreated within its per-ID cooldown.
    for (const auto& wire : originals) submit(wire, 99);
    tick();
    {
        std::lock_guard<std::mutex> guard{relay_mutex};
        BOOST_CHECK_EQUAL(relays.size(), 4U);
    }
    clock.m_now += std::chrono::milliseconds{250};
    tick();
    {
        std::lock_guard<std::mutex> guard{relay_mutex};
        BOOST_REQUIRE_EQUAL(relays.size(), 8U);
        for (size_t i{4}; i < 8; ++i) BOOST_CHECK(relays[i].message.header.market_id != first);
    }
    clock.m_now += std::chrono::milliseconds{250};
    tick();
    {
        std::lock_guard<std::mutex> guard{relay_mutex};
        BOOST_REQUIRE_EQUAL(relays.size(), 12U);
        for (const auto& original : originals) {
            BOOST_CHECK_EQUAL(std::count_if(relays.begin(), relays.end(), [&](const auto& relay) {
                return relay.message.header == original.header && relay.message.payload == original.payload;
            }), 1);
        }
        for (const auto& relay : relays) BOOST_CHECK(relay.exclude_peer == 77);
    }
    clock.m_now += std::chrono::seconds{1};
    tick(); // no autonomous re-enqueue after all obligations completed
    for (const auto& original : originals) submit(original, 77);
    for (const auto& market : markets) chain.SetTransition(market, node::FlowMeshSeatTransitionKind::PAUSED);
    tick();
    for (const auto& market : markets) chain.SetTransition(market, node::FlowMeshSeatTransitionKind::CONTINUE);
    clock.m_now += std::chrono::seconds{1};
    tick();
    {
        std::lock_guard<std::mutex> guard{relay_mutex};
        BOOST_CHECK_EQUAL(relays.size(), 12U); // pause cleared every deferred obligation
    }
    for (const auto& original : originals) submit(original, 77);
    chain.AddCanonical({100, Filled(0xee)}); // invalidates both markets' seat anchors
    tick();
    for (const auto& market : markets) {
        BOOST_CHECK(runtime.MarketStatus(market)->halt == node::FlowMeshRuntimeHalt::ANCHOR_INVALIDATED);
    }
    clock.m_now += std::chrono::seconds{1};
    tick();
    {
        std::lock_guard<std::mutex> guard{relay_mutex};
        BOOST_CHECK_EQUAL(relays.size(), 12U); // halted obligations cannot drain later
    }
    runtime.Stop();
    // Byte and count caps remain independent, including nominal maximum wire
    // ACTION size; no credit accumulates after a long idle or backward clock.
    node::FlowMeshDuplicateActionRelayBudget budget;
    const size_t maximum{flowmesh::FLOWMESH_ACTION_MAX_BYTES + flowmesh::FLOWMESH_WIRE_HEADER_SIZE};
    for (size_t i{0}; i < 3; ++i) {
        BOOST_REQUIRE(budget.Available(clock.m_now, maximum));
        budget.Charge(maximum);
    }
    BOOST_CHECK(!budget.Available(clock.m_now, maximum));
    BOOST_CHECK(!budget.Available(clock.m_now - std::chrono::seconds{1}, maximum));
    clock.m_now += std::chrono::hours{1};
    for (size_t i{0}; i < 4; ++i) {
        BOOST_REQUIRE(budget.Available(clock.m_now, 100));
        budget.Charge(100);
    }
    BOOST_CHECK(!budget.Available(clock.m_now, 100));
}

BOOST_AUTO_TEST_CASE(dropped_action_evidence_retries_exact_locked_candidate)
{
    const uint256 domain{Filled(0x1c)};
    const modern::AssetId asset{Filled(0x3c)};
    const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const auto vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x5c)};
    const SeatFixture seats{Seats(domain, market, 4, 7, 100, Filled(0x71), 124)};
    CKey account_key;
    account_key.MakeNewKey(true);
    const auto account{flowmesh::AccountForKey(XOnlyPubKey{account_key.GetPubKey()})};
    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(seats.seats);
    FixedClock clock;
    MapDeposits deposits;
    deposits.required_anchor = chain.m_current;
    const COutPoint outpoint{Txid::FromUint256(Filled(0x8d)), 0};
    deposits.entries.emplace(outpoint, flowmesh::DepositInfo{asset, 100, account});
    const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(),
                                        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    std::array<RuntimeKeys, 3> keys;
    std::array<std::unique_ptr<node::FlowMeshProductionStore>, 3> stores;
    RuntimeNetwork network;
    std::array<std::unique_ptr<node::FlowMeshRuntime>, 3> runtimes;
    std::string error;
    for (size_t i{0}; i < runtimes.size(); ++i) {
        keys[i].m_keys[market] = {seats.secrets[i]};
        stores[i] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
            .path = m_args.GetDataDirBase() / fs::PathFromString(
                        "flowmesh_runtime_evidence_loss_" + std::to_string(i)),
            .cache_bytes = size_t{1} << 20, .wipe_data = true});
        BOOST_REQUIRE_MESSAGE(stores[i]->OpenForMarket(
            domain, market, seats.seats, initial.Root(), error), error);
        node::FlowMeshRuntimeConfig config{&chain, &keys[i], &clock,
            [&network, i](node::FlowMeshRuntimeRelay relay) {
                network.Relay(i, std::move(relay));
                return LegacyRelayResult();
            },
            std::chrono::hours{1}, std::chrono::seconds{60}, {}};
        runtimes[i] = std::make_unique<node::FlowMeshRuntime>(config,
            std::vector<node::FlowMeshRuntimeMarketConfig>{MarketConfig(
                domain, market, treasury, seats.seats, initial, *stores[i], &deposits)});
        network.Set(i, runtimes[i].get(), true);
        BOOST_REQUIRE_MESSAGE(runtimes[i]->Start(error), error);
    }
    const auto all_at = [&](uint64_t sequence) {
        return std::all_of(runtimes.begin(), runtimes.end(), [&](const auto& runtime) {
            return runtime->MarketStatus(market)->next_sequence == sequence;
        });
    };
    runtimes[0]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] { return all_at(1); }));
    BOOST_REQUIRE(runtimes[1]->SubmitLocalAction(market, Deposit(outpoint)) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtimes[1]->WaitForIdle(std::chrono::seconds{2}));
    runtimes[1]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] { return all_at(2); }));
    for (const auto& runtime : runtimes) BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));

    flowmesh::Action ask;
    ask.signer = account;
    ask.type = static_cast<uint8_t>(flowmesh::ActionType::SUBMIT_ASK);
    for (CAmount i{1}; i <= static_cast<CAmount>(flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS); ++i) {
        ask.curve.push_back({i, i * 10});
    }
    BOOST_REQUIRE(flowmesh::SignAction(account_key, domain, initial.ConfigId(), ask));
    const auto original_payload{flowmesh::EncodeProductionActionPayload(ask)};
    BOOST_REQUIRE(original_payload);
    std::mutex filter_mutex;
    bool deliver_evidence{false};
    bool exact_evidence{true};
    bool exact_candidate{true};
    bool exact_vote{true};
    size_t proposer_action_attempts{0};
    size_t voter_replays{0};
    std::optional<std::vector<unsigned char>> candidate_bytes;
    std::optional<std::vector<unsigned char>> voter_bytes;
    struct StopBeforeCapturedState {
        std::array<std::unique_ptr<node::FlowMeshRuntime>, 3>& runtimes;
        ~StopBeforeCapturedState() { for (auto& runtime : runtimes) runtime->Stop(); }
    } stop_before_captured_state{runtimes};
    network.SetFilter([&](size_t from, size_t to, const flowmesh::WireMessage& message) {
        if (message.header.sequence != 2) return true;
        std::lock_guard<std::mutex> lock{filter_mutex};
        if (message.kind == flowmesh::WireMessageKind::ACTION) {
            exact_evidence &= message.payload == *original_payload;
            if (from == 2 && to == 0) ++proposer_action_attempts;
            if (to == 0 && !deliver_evidence) return false; // drop every gossip route
        }
        if (message.kind == flowmesh::WireMessageKind::PROPOSAL && from == 2 && to == 1) {
            const auto proposal{flowmesh::DecodeProductionProposalPayload(message.payload)};
            const auto bytes{proposal ? flowmesh::EncodeProductionEntry(proposal->entry) : std::nullopt};
            if (!bytes) exact_candidate = false;
            else if (!candidate_bytes) candidate_bytes = bytes;
            else exact_candidate &= *candidate_bytes == *bytes;
        }
        if (message.kind == flowmesh::WireMessageKind::ATTESTATION && from == 1 && to == 2) {
            const auto vote{flowmesh::DecodeProductionAttestationPayload(message.payload)};
            if (!vote) exact_vote = false;
            // A peer can now forward another seat's vote. Compare the exact
            // cached payload by signing identity, not transport sender.
            else if (vote->seat_index == 1) {
                if (!voter_bytes) voter_bytes = message.payload;
                else {
                    exact_vote &= *voter_bytes == message.payload;
                    ++voter_replays;
                }
            }
        }
        return true;
    });
    BOOST_REQUIRE(runtimes[2]->SubmitLocalAction(market, ask) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] {
        return runtimes[1]->MarketStatus(market)->pending_actions == 1 &&
               runtimes[2]->MarketStatus(market)->pending_actions == 1;
    }));
    runtimes[2]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        const auto data{runtimes[2]->MarketData(market, std::nullopt, {}, error)};
        return data && data->snapshot.runtime.max_verified_attestations == 2;
    }));
    for (const auto& runtime : runtimes) BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
    const auto missing{runtimes[0]->MarketData(market, std::nullopt, {}, error)};
    BOOST_REQUIRE_MESSAGE(missing, error);
    BOOST_CHECK_GT(missing->snapshot.runtime.proposals_missing_evidence, 0U);
    BOOST_CHECK(!missing->snapshot.runtime.local_locked_candidate);
    BOOST_CHECK_EQUAL(missing->snapshot.quorum_required, 3U);
    const flowmesh::ProductionSignPosition position{seats.seats.epoch, 2};
    std::optional<uint256> locked;
    BOOST_REQUIRE(stores[2]->ReadLock(position, locked, error));
    BOOST_REQUIRE(locked);
    // Proposal retries alone, without elapsed retry budget or credentials,
    // cannot supply the third vote.
    for (size_t i{0}; i < 2; ++i) {
        runtimes[2]->NotifyTick();
        for (const auto& runtime : runtimes) BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
    }
    BOOST_CHECK(all_at(2));
    {
        std::lock_guard<std::mutex> lock{filter_mutex};
        BOOST_CHECK_EQUAL(proposer_action_attempts, 1U);
        deliver_evidence = true;
    }
    clock.m_now += std::chrono::milliseconds{250};
    runtimes[2]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        const auto status{runtimes[0]->MarketStatus(market)};
        return status->pending_actions == 1 || status->next_sequence == 3;
    }));
    for (const auto& runtime : runtimes) BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
    clock.m_now += node::FlowMeshCommitteeRelayBudget::REPEAT_DELAY;
    runtimes[2]->NotifyTick(); // normal proposal retry after queued evidence
    BOOST_REQUIRE(WaitUntil([&] { return all_at(3); }));
    for (const auto& runtime : runtimes) BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
    std::optional<node::StoredProductionEntry> committed;
    BOOST_REQUIRE(stores[2]->ReadEntry(2, seats.seats, committed, error));
    BOOST_REQUIRE(committed);
    BOOST_CHECK(committed->entry.GetHash() == *locked);
    {
        std::lock_guard<std::mutex> lock{filter_mutex};
        BOOST_REQUIRE(candidate_bytes);
        BOOST_CHECK(*flowmesh::EncodeProductionEntry(committed->entry) == *candidate_bytes);
        BOOST_CHECK(exact_evidence && exact_candidate && exact_vote);
        BOOST_REQUIRE(voter_bytes);
        BOOST_CHECK_GT(voter_replays, 0U);
        // Initial evidence, the 250ms recovery, and the eligible evidence
        // sweep during the later one-second proposal retry.
        BOOST_CHECK_EQUAL(proposer_action_attempts, 3U);
    }
    for (size_t i{0}; i < runtimes.size(); ++i) {
        std::optional<uint256> same_lock;
        BOOST_REQUIRE(stores[i]->ReadLock(position, same_lock, error));
        BOOST_CHECK(same_lock == locked);
        BOOST_CHECK(runtimes[i]->MarketStatus(market)->last_microblock_hash == *locked);
        BOOST_CHECK_EQUAL(runtimes[i]->StateSnapshot(market)->NextSequence(account), 1U);
    }
    flowmesh::WireMessage delayed{flowmesh::WireMessageKind::ACTION,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, market, seats.seats.epoch, 2}, *original_payload};
    BOOST_REQUIRE(runtimes[0]->EnqueueWireMessage(2, std::move(delayed)) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtimes[0]->WaitForIdle(std::chrono::seconds{2}));
    clock.m_now += std::chrono::seconds{2};
    runtimes[2]->NotifyTick();
    BOOST_REQUIRE(runtimes[2]->WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK(all_at(3));
    {
        std::lock_guard<std::mutex> lock{filter_mutex};
        BOOST_CHECK_EQUAL(proposer_action_attempts, 3U);
    }
    network.SetFilter({});
    for (auto& runtime : runtimes) runtime->Stop();
}

BOOST_AUTO_TEST_CASE(evidence_retry_rotates_markets_and_preserves_pacing_gates)
{
    const uint256 domain{Filled(0x1d)};
    const uint256 treasury{Filled(0x5d)};
    CKey account_key;
    account_key.MakeNewKey(true);
    const auto account{flowmesh::AccountForKey(XOnlyPubKey{account_key.GetPubKey()})};
    RuntimeChain chain;
    chain.m_domain = domain;
    FixedClock clock;
    RuntimeKeys keys;
    std::vector<SeatFixture> seats;
    std::vector<flowmesh::MarketId> markets;
    std::vector<std::unique_ptr<node::FlowMeshProductionStore>> stores;
    std::vector<node::FlowMeshRuntimeMarketConfig> market_configs;
    std::string error;
    for (size_t i{0}; i < 2; ++i) {
        const modern::AssetId asset{Filled(static_cast<unsigned char>(0x3d + i))};
        const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
        markets.push_back(market);
        seats.push_back(Seats(domain, market, 4, 7, 100, Filled(0x71), static_cast<unsigned char>(125 + i)));
        chain.Add(seats.back().seats);
        keys.m_keys[market] = seats.back().secrets;
        flowmesh::FlowMeshState state{*flowmesh::ComputeFlowMeshVaultId(domain, market),
            asset, modern::NativeAsset(), flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
        BOOST_REQUIRE(flowmesh::test_only::StateFunding::Fund(state, account, asset, 1000));
        stores.push_back(std::make_unique<node::FlowMeshProductionStore>(DBParams{
            .path = m_args.GetDataDirBase() / fs::PathFromString("flowmesh_evidence_pacing_" + std::to_string(i)),
            .cache_bytes = size_t{1} << 20, .wipe_data = true}));
        BOOST_REQUIRE_MESSAGE(stores.back()->OpenForMarket(domain, market, seats.back().seats, state.Root(), error), error);
        market_configs.push_back(MarketConfig(domain, market, treasury, seats.back().seats, state, *stores.back(), nullptr));
    }
    std::mutex relay_mutex;
    std::vector<flowmesh::WireMessage> delivered;
    size_t attempts{0};
    bool suppress{false};
    node::FlowMeshRuntimeConfig config{&chain, &keys, &clock,
        [&](node::FlowMeshRuntimeRelay relay) {
            if (relay.message.kind != flowmesh::WireMessageKind::ACTION) return LegacyRelayResult();
            std::lock_guard<std::mutex> lock{relay_mutex};
            ++attempts;
            if (!suppress) delivered.push_back(std::move(relay.message));
            return LegacyRelayResult();
        }, std::chrono::hours{1}, std::chrono::seconds{60}, {}};
    node::FlowMeshRuntime runtime{config, market_configs};
    BOOST_REQUIRE_MESSAGE(runtime.Start(error), error);
    runtime.NotifyTick(); // all four local keys certify both genesis entries
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    for (size_t i{0}; i < markets.size(); ++i) {
        BOOST_REQUIRE_EQUAL(runtime.MarketStatus(markets[i])->next_sequence, 1U);
        keys.m_keys[markets[i]] = {seats[i].secrets[1]};
        for (uint64_t sequence{0}; sequence < 20; ++sequence) {
            flowmesh::Action ask;
            ask.signer = account;
            ask.sequence = sequence;
            ask.type = static_cast<uint8_t>(flowmesh::ActionType::SUBMIT_ASK);
            for (CAmount j{1}; j <= static_cast<CAmount>(flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS); ++j) {
                ask.curve.push_back({j, j * 10});
            }
            BOOST_REQUIRE(flowmesh::SignAction(account_key, domain, market_configs[i].state.ConfigId(), ask));
            BOOST_REQUIRE(runtime.SubmitLocalAction(markets[i], ask) == flowmesh::QueueResult::ACCEPTED);
        }
    }
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    {
        std::lock_guard<std::mutex> lock{relay_mutex};
        BOOST_REQUIRE_EQUAL(delivered.size(), 40U);
        delivered.clear();
        attempts = 0;
    }
    runtime.NotifyTick(); // each market locks a 20-action candidate, no quorum
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    for (const auto& market : markets) {
        BOOST_REQUIRE(runtime.MarketData(market, std::nullopt, {}, error)->snapshot.runtime.local_locked_candidate);
    }
    const auto tick = [&](std::chrono::milliseconds elapsed) {
        clock.m_now += elapsed;
        runtime.NotifyTick();
        BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    };
    std::map<flowmesh::MarketId, size_t> first_sweep;
    for (size_t batch{0}; batch < 5; ++batch) {
        size_t before;
        {
            std::lock_guard<std::mutex> lock{relay_mutex};
            before = delivered.size();
        }
        tick(std::chrono::milliseconds{250});
        {
            std::lock_guard<std::mutex> lock{relay_mutex};
            BOOST_CHECK_EQUAL(delivered.size() - before, 8U);
            size_t bytes{0};
            for (size_t i{before}; i < delivered.size(); ++i) {
                bytes += delivered[i].MemoryUsage();
                ++first_sweep[delivered[i].header.market_id];
                const auto action{flowmesh::DecodeProductionActionPayload(delivered[i].payload)};
                BOOST_REQUIRE(action);
                BOOST_CHECK_EQUAL(action->curve.size(), flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS);
            }
            BOOST_CHECK_LE(bytes, node::FlowMeshEvidenceRetryBudget::MAX_BYTES);
        }
        tick(std::chrono::milliseconds{0}); // no refill from callback/tick churn
        {
            std::lock_guard<std::mutex> lock{relay_mutex};
            BOOST_CHECK_EQUAL(delivered.size(), before + 8);
        }
        if (batch == 1) {
            BOOST_CHECK_EQUAL(first_sweep[markets[0]], 8U);
            BOOST_CHECK_EQUAL(first_sweep[markets[1]], 8U);
        }
    }
    BOOST_CHECK_EQUAL(first_sweep[markets[0]], 20U);
    BOOST_CHECK_EQUAL(first_sweep[markets[1]], 20U);
    tick(std::chrono::milliseconds{750}); // complete sweep waits a full second
    {
        std::lock_guard<std::mutex> lock{relay_mutex};
        BOOST_CHECK_EQUAL(attempts, 40U);
        suppress = true;
    }
    tick(std::chrono::milliseconds{250}); // model service reconciliation dropping relay
    {
        std::lock_guard<std::mutex> lock{relay_mutex};
        BOOST_CHECK_EQUAL(attempts, 48U);
        BOOST_CHECK_EQUAL(delivered.size(), 40U);
    }
    tick(std::chrono::milliseconds{0});
    {
        std::lock_guard<std::mutex> lock{relay_mutex};
        BOOST_CHECK_EQUAL(attempts, 48U);
        suppress = false;
    }
    // A suppressed send consumes the chunk, but later complete sweeps revisit
    // the exact payloads. No immediate retry spin and no cursor starvation.
    for (size_t i{0}; i < 8; ++i) tick(std::chrono::milliseconds{250});
    {
        std::lock_guard<std::mutex> lock{relay_mutex};
        const auto first{delivered.front()};
        BOOST_CHECK_GE(std::count_if(delivered.begin(), delivered.end(), [&](const auto& message) {
            return message.header == first.header && message.payload == first.payload;
        }), 2);
    }
    // Paused markets and missing local proposer keys must not keep retrying.
    chain.SetTransition(markets[0], node::FlowMeshSeatTransitionKind::PAUSED);
    {
        std::lock_guard<std::mutex> lock{relay_mutex};
        delivered.clear();
    }
    tick(std::chrono::milliseconds{1000});
    {
        std::lock_guard<std::mutex> lock{relay_mutex};
        BOOST_CHECK(std::none_of(delivered.begin(), delivered.end(), [&](const auto& message) {
            return message.header.market_id == markets[0];
        }));
        delivered.clear();
    }
    keys.m_keys.clear();
    tick(std::chrono::milliseconds{1000});
    {
        std::lock_guard<std::mutex> lock{relay_mutex};
        BOOST_CHECK(delivered.empty());
    }
    for (size_t i{0}; i < markets.size(); ++i) keys.m_keys[markets[i]] = {seats[i].secrets[0]};
    tick(std::chrono::milliseconds{1000}); // a local non-proposer key is not enough
    {
        std::lock_guard<std::mutex> lock{relay_mutex};
        BOOST_CHECK(delivered.empty());
    }
    for (size_t i{0}; i < markets.size(); ++i) keys.m_keys[markets[i]] = {seats[i].secrets[1]};
    chain.AddCanonical({100, Filled(0xee)}); // invalidates the shared seat anchor
    tick(std::chrono::milliseconds{1000});
    for (const auto& market : markets) {
        BOOST_CHECK(runtime.MarketStatus(market)->halt == node::FlowMeshRuntimeHalt::ANCHOR_INVALIDATED);
        BOOST_CHECK_EQUAL(runtime.MarketStatus(market)->next_sequence, 1U);
    }
    {
        std::lock_guard<std::mutex> lock{relay_mutex};
        BOOST_CHECK(delivered.empty());
    }
    runtime.Stop();
}

BOOST_AUTO_TEST_CASE(repeated_proposal_replays_cached_attestation)
{
    const uint256 domain{Filled(0x19)};
    const modern::AssetId asset{Filled(0x39)};
    const flowmesh::MarketId market{
        *flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const flowmesh::VaultId vault{
        *flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x59)};
    const SeatFixture seats{
        Seats(domain, market, 4, 7, 100, Filled(0x71), 121)};

    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(seats.seats);
    FixedClock clock;
    flowmesh::FlowMeshState initial{
        vault, asset, modern::NativeAsset(),
        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};

    std::array<std::unique_ptr<node::FlowMeshProductionStore>, 3> stores;
    std::array<RuntimeKeys, 3> keys;
    std::string error;
    for (size_t i{0}; i < stores.size(); ++i) {
        stores[i] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
            .path = m_args.GetDataDirBase() / fs::PathFromString(
                        "flowmesh_runtime_attestation_retry_" +
                        std::to_string(i)),
            .cache_bytes = size_t{1} << 20, .wipe_data = true});
        BOOST_REQUIRE_MESSAGE(stores[i]->OpenForMarket(
                                  domain, market, seats.seats,
                                  initial.Root(), error),
                              error);
        keys[i].m_keys[market] = {seats.secrets[i]};
    }

    RetryAttestationNetwork network;
    std::array<std::unique_ptr<node::FlowMeshRuntime>, 3> runtimes;
    for (size_t i{0}; i < runtimes.size(); ++i) {
        node::FlowMeshRuntimeConfig config;
        config.chain = &chain;
        config.keys = &keys[i];
        config.clock = &clock;
        config.round_timeout = std::chrono::hours{1};
        config.relay = [&network, i](node::FlowMeshRuntimeRelay relay) {
            network.Relay(i, std::move(relay));
            return LegacyRelayResult();
        };
        runtimes[i] = std::make_unique<node::FlowMeshRuntime>(
            std::move(config),
            std::vector<node::FlowMeshRuntimeMarketConfig>{MarketConfig(
                domain, market, treasury, seats.seats, initial,
                *stores[i], nullptr)});
        network.Set(i, runtimes[i].get());
        BOOST_REQUIRE_MESSAGE(runtimes[i]->Start(error), error);
    }

    // Node 0 is the sequence-zero proposer. Its own vote and node 1's vote
    // are insufficient for the 3-of-4 threshold after node 2's first vote is
    // dropped.
    runtimes[0]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        return network.DroppedFirstAttestation();
    }));
    for (const auto& runtime : runtimes) {
        BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
        BOOST_REQUIRE_EQUAL(runtime->MarketStatus(market)->next_sequence, 0U);
    }
    const auto stalled{runtimes[0]->MarketData(market, std::nullopt, {}, error)};
    BOOST_REQUIRE_MESSAGE(stalled, error);
    BOOST_CHECK_EQUAL(stalled->snapshot.runtime.candidate_count, 1U);
    BOOST_CHECK_EQUAL(stalled->snapshot.runtime.max_verified_attestations, 2U);
    BOOST_CHECK_EQUAL(stalled->snapshot.quorum_required, 3U);
    BOOST_CHECK(stalled->snapshot.runtime.local_locked_candidate.has_value());
    BOOST_CHECK(stalled->snapshot.runtime.last_message_observed_at.has_value());

    // Retrying the permanently locked proposal must not sign again. Each
    // voter returns its cached vote directly to node 0; the previously lost
    // node-2 payload is byte-identical and restores certification liveness.
    clock.m_now += node::FlowMeshCommitteeRelayBudget::REPEAT_DELAY;
    runtimes[0]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        if (!network.RetriedExactCachedAttestation()) return false;
        for (const auto& runtime : runtimes) {
            const auto status{runtime->MarketStatus(market)};
            if (!status || status->next_sequence != 1 ||
                status->halt != node::FlowMeshRuntimeHalt::NONE) {
                return false;
            }
        }
        return true;
    }));

    const auto progressed{runtimes[0]->MarketData(market, std::nullopt, {}, error)};
    BOOST_REQUIRE_MESSAGE(progressed, error);
    BOOST_CHECK_EQUAL(progressed->snapshot.runtime.candidate_count, 0U);
    BOOST_CHECK_EQUAL(progressed->snapshot.runtime.max_verified_attestations, 0U);
    BOOST_CHECK(!progressed->snapshot.runtime.local_locked_candidate);
    BOOST_CHECK(progressed->snapshot.local_observed_at.has_value());

    for (auto& runtime : runtimes) runtime->Stop();
}

BOOST_AUTO_TEST_CASE(four_node_commit_pause_dynamic_catchup_and_isolated_halt)
{
    const uint256 domain{Filled(0x11)};
    const modern::AssetId asset_a{Filled(0x31)};
    const modern::AssetId asset_b{Filled(0x32)};
    const flowmesh::MarketId market_a{
        *flowmesh::ComputeFlowMeshMarketId(domain, asset_a)};
    const flowmesh::MarketId market_b{
        *flowmesh::ComputeFlowMeshMarketId(domain, asset_b)};
    const flowmesh::VaultId vault_a{
        *flowmesh::ComputeFlowMeshVaultId(domain, market_a)};
    const flowmesh::VaultId vault_b{
        *flowmesh::ComputeFlowMeshVaultId(domain, market_b)};
    const uint256 treasury{Filled(0x51)};
    const flowmesh::AccountId account{Filled(0x61)};
    const SeatFixture seats_a{
        Seats(domain, market_a, 4, 7, 100, Filled(0x71), 1)};
    const SeatFixture seats_b{
        Seats(domain, market_b, 4, 7, 100, Filled(0x71), 41)};

    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(seats_a.seats);
    chain.Add(seats_b.seats);
    FixedClock clock;
    MapDeposits deposits;
    deposits.required_anchor = chain.m_current;
    const COutPoint deposit0{Txid::FromUint256(Filled(0x81)), 0};
    const COutPoint deposit1{Txid::FromUint256(Filled(0x82)), 1};
    const COutPoint conflicting_deposit{Txid::FromUint256(Filled(0x83)), 2};
    const COutPoint rejected_deposit{Txid::FromUint256(Filled(0x84)), 3};
    deposits.entries.emplace(deposit0,
                             flowmesh::DepositInfo{asset_a, 100, account});
    deposits.entries.emplace(deposit1,
                             flowmesh::DepositInfo{asset_a, 200, account});
    deposits.entries.emplace(conflicting_deposit,
                             flowmesh::DepositInfo{asset_a, 300, account});

    flowmesh::FlowMeshState initial_a{
        vault_a, asset_a, modern::NativeAsset(),
        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    flowmesh::FlowMeshState initial_b{
        vault_b, asset_b, modern::NativeAsset(),
        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};

    std::array<std::unique_ptr<node::FlowMeshProductionStore>, 4> stores_a;
    std::string error;
    for (size_t i{0}; i < stores_a.size(); ++i) {
        stores_a[i] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
            .path = m_args.GetDataDirBase() / fs::PathFromString(
                        "flowmesh_runtime_a_" + std::to_string(i)),
            .cache_bytes = size_t{1} << 20, .wipe_data = true});
        BOOST_REQUIRE_MESSAGE(stores_a[i]->OpenForMarket(
                                  domain, market_a, seats_a.seats,
                                  initial_a.Root(), error),
                              error);
    }
    node::FlowMeshProductionStore store_b{DBParams{
        .path = m_args.GetDataDirBase() / "flowmesh_runtime_b",
        .cache_bytes = size_t{1} << 20, .wipe_data = true}};
    BOOST_REQUIRE_MESSAGE(store_b.OpenForMarket(
                              domain, market_b, seats_b.seats,
                              initial_b.Root(), error),
                          error);

    std::array<RuntimeKeys, 4> keys;
    for (size_t i{0}; i < 3; ++i) {
        keys[i].m_keys[market_a] = {seats_a.secrets[i]};
    }
    keys[0].m_keys[market_b] = {seats_b.secrets[0]};

    RuntimeNetwork network;
    std::array<std::unique_ptr<node::FlowMeshRuntime>, 4> runtimes;
    for (size_t i{0}; i < runtimes.size(); ++i) {
        node::FlowMeshRuntimeConfig config;
        config.chain = &chain;
        config.keys = &keys[i];
        config.clock = &clock;
        config.round_timeout = std::chrono::hours{1};
        config.relay = [&network, i](node::FlowMeshRuntimeRelay relay) {
            network.Relay(i, std::move(relay));
            return LegacyRelayResult();
        };
        std::vector<node::FlowMeshRuntimeMarketConfig> markets;
        if (i < 3) {
            markets.push_back(MarketConfig(domain, market_a, treasury,
                                           seats_a.seats, initial_a,
                                           *stores_a[i], &deposits));
        }
        if (i == 0) {
            markets.push_back(MarketConfig(domain, market_b, treasury,
                                           seats_b.seats, initial_b, store_b,
                                           nullptr));
        }
        runtimes[i] = std::make_unique<node::FlowMeshRuntime>(
            std::move(config), std::move(markets));
        network.Set(i, runtimes[i].get(), i < 3);
        BOOST_REQUIRE_MESSAGE(runtimes[i]->Start(error), error);
    }

    // Unknown caller-selected market ids are rejected before the P2P queue
    // creates per-peer/market rate-limit state. A long-lived peer therefore
    // cannot bypass throttling or grow memory with random ids.
    for (size_t i{0}; i < 100'000; ++i) {
        flowmesh::WireMessage unknown;
        unknown.kind = flowmesh::WireMessageKind::ACTION;
        unknown.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1,
                          Filled(static_cast<unsigned char>(i % 251 + 1)), 7,
                          static_cast<uint64_t>(i)};
        unknown.header.market_id.begin()[0] =
            static_cast<unsigned char>((i / 251) & 0xff);
        unknown.payload = {0x42};
        BOOST_REQUIRE(runtimes[0]->EnqueueWireMessage(99, std::move(unknown)) ==
                      flowmesh::QueueResult::MARKET_LIMIT);
    }
    BOOST_CHECK_EQUAL(runtimes[0]->MarketIds().size(), 2U);

    // Every market first certifies an empty sequence-zero entry pinned to
    // its seat anchor. The service publishes this genesis checkpoint before
    // admitting effect-generating wallet actions.
    runtimes[0]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        for (size_t i{0}; i < 3; ++i) {
            const auto status{runtimes[i]->MarketStatus(market_a)};
            if (!status || status->next_sequence != 1 ||
                status->next_effect_index != 0 ||
                status->halt != node::FlowMeshRuntimeHalt::NONE) {
                return false;
            }
        }
        return true;
    }));

    BOOST_REQUIRE(runtimes[0]->SubmitLocalAction(
                      market_a, Deposit(deposit0)) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] {
        for (size_t i{0}; i < 3; ++i) {
            const auto status{runtimes[i]->MarketStatus(market_a)};
            if (!status || status->pending_actions != 1) return false;
        }
        return true;
    }));
    runtimes[1]->NotifyTick(); // sequence 1 proposer is ordered seat 1
    BOOST_REQUIRE(WaitUntil([&] {
        for (size_t i{0}; i < 3; ++i) {
            const auto status{runtimes[i]->MarketStatus(market_a)};
            if (!status || status->next_sequence != 2 ||
                status->next_effect_index != 1 ||
                status->halt != node::FlowMeshRuntimeHalt::NONE) {
                return false;
            }
        }
        return true;
    }));

    BOOST_REQUIRE(runtimes[1]->SubmitLocalAction(
                      market_a, Deposit(deposit1)) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] {
        for (size_t i{0}; i < 3; ++i) {
            const auto status{runtimes[i]->MarketStatus(market_a)};
            if (!status || status->pending_actions != 1) return false;
        }
        return true;
    }));
    chain.SetTransition(market_a, node::FlowMeshSeatTransitionKind::PAUSED);
    for (size_t i{0}; i < 3; ++i) runtimes[i]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        for (size_t i{0}; i < 3; ++i) {
            const auto status{runtimes[i]->MarketStatus(market_a)};
            if (!status || !status->paused || status->next_sequence != 2) {
                return false;
            }
        }
        return true;
    }));
    chain.SetTransition(market_a, node::FlowMeshSeatTransitionKind::CONTINUE);
    runtimes[2]->NotifyTick(); // sequence 2 proposer is ordered seat 2
    BOOST_REQUIRE(WaitUntil([&] {
        for (size_t i{0}; i < 3; ++i) {
            const auto status{runtimes[i]->MarketStatus(market_a)};
            if (!status || status->next_sequence != 3 ||
                status->next_effect_index != 2 || status->paused) {
                return false;
            }
        }
        return true;
    }));

    // A certified rejection is terminal for that exact action. In
    // particular, an unknown deposit must leave the pool after one slot or
    // it could generate an unbounded stream of zero-effect certificates.
    BOOST_REQUIRE(runtimes[0]->SubmitLocalAction(
                      market_a, Deposit(rejected_deposit)) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] {
        for (size_t i{0}; i < 3; ++i) {
            const auto status{runtimes[i]->MarketStatus(market_a)};
            if (!status || status->pending_actions != 1) return false;
        }
        return true;
    }));
    clock.m_now += std::chrono::hours{2};
    for (size_t i{0}; i < 3; ++i) runtimes[i]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        for (size_t i{0}; i < 3; ++i) {
            const auto status{runtimes[i]->MarketStatus(market_a)};
            if (!status || status->next_sequence != 4 ||
                status->next_effect_index != 2 ||
                status->pending_actions != 0 ||
                status->halt != node::FlowMeshRuntimeHalt::NONE) {
                return false;
            }
        }
        return true;
    }));
    for (size_t i{0}; i < 3; ++i) runtimes[i]->NotifyTick();
    for (size_t i{0}; i < 3; ++i) {
        BOOST_REQUIRE(runtimes[i]->WaitForIdle(std::chrono::seconds{2}));
        BOOST_CHECK_EQUAL(runtimes[i]->MarketStatus(market_a)->next_sequence,
                          4U);
    }

    BOOST_REQUIRE_MESSAGE(runtimes[3]->AddMarket(
                              MarketConfig(domain, market_a, treasury,
                                           seats_a.seats, initial_a,
                                           *stores_a[3], &deposits),
                              error),
                          error);
    BOOST_CHECK_EQUAL(runtimes[3]->MarketIds().size(), 1U);
    BOOST_REQUIRE(runtimes[3]->MarketStatus(market_a));
    BOOST_CHECK(runtimes[3]->MarketStatus(market_a)->observer_only);
    // Drain the new-market tick while this node is still offline. Its first
    // unsolicited response must remain untrusted even with discovery enabled.
    BOOST_REQUIRE(runtimes[3]->WaitForIdle(std::chrono::seconds{2}));

    std::optional<node::StoredProductionEntry> stored0;
    BOOST_REQUIRE(stores_a[0]->ReadEntry(0, seats_a.seats, stored0, error));
    BOOST_REQUIRE(stored0);
    const auto certified0{flowmesh::EncodeProductionCertifiedPayload(
        flowmesh::ProductionCertifiedEnvelope{stored0->entry,
                                              stored0->certificate},
        seats_a.seats.Size())};
    BOOST_REQUIRE(certified0);
    const auto unsolicited_payload{
        flowmesh::EncodeCatchupEntries(std::vector<std::vector<unsigned char>>{
            *certified0})};
    BOOST_REQUIRE(unsolicited_payload);
    flowmesh::WireMessage unsolicited;
    unsolicited.kind = flowmesh::WireMessageKind::ENTRIES;
    unsolicited.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1, market_a, 7, 0};
    unsolicited.payload = *unsolicited_payload;
    BOOST_REQUIRE(runtimes[3]->EnqueueWireMessage(0, unsolicited) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtimes[3]->WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(runtimes[3]->MarketStatus(market_a)->next_sequence, 0U);

    network.Set(3, runtimes[3].get(), true);
    BOOST_REQUIRE(runtimes[0]->WaitForIdle(std::chrono::seconds{2}));
    clock.m_now += std::chrono::seconds{6};
    runtimes[0]->FlowMeshPeerConnected(3);
    BOOST_REQUIRE(WaitUntil([&] {
        const auto status{runtimes[3]->MarketStatus(market_a)};
        return status && status->next_sequence == 4 &&
               status->next_effect_index == 2 &&
               status->halt == node::FlowMeshRuntimeHalt::NONE;
    }));

    // A valid certificate with a mismatched common header is ignored.
    flowmesh::WireMessage wrong_header;
    wrong_header.kind = flowmesh::WireMessageKind::CERTIFICATE;
    wrong_header.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1, market_a, 7, 1};
    wrong_header.payload = *certified0;
    BOOST_REQUIRE(runtimes[0]->EnqueueWireMessage(55, wrong_header) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtimes[0]->WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK(runtimes[0]->MarketStatus(market_a)->halt ==
                node::FlowMeshRuntimeHalt::NONE);

    flowmesh::ProductionEpochGate gate{domain, market_a, seats_a.seats};
    flowmesh::ProductionEntryCheck entry_check;
    const std::vector<flowmesh::Action> conflicting_actions{
        Deposit(conflicting_deposit)};
    const auto conflicting{flowmesh::BuildProductionExecutionEntry(
        initial_a, domain, market_a, seats_a.seats, gate, 0, 0, uint256{},
        chain.m_current, {260, std::nullopt, &chain}, treasury,
        conflicting_actions, &deposits, entry_check)};
    BOOST_REQUIRE_MESSAGE(conflicting,
                          flowmesh::ProductionEntryCheckName(entry_check));
    const flowmesh::ProductionCertifiedEnvelope conflict_envelope{
        conflicting->entry, Certify(conflicting->entry, seats_a)};
    const auto conflict_payload{flowmesh::EncodeProductionCertifiedPayload(
        conflict_envelope, seats_a.seats.Size())};
    BOOST_REQUIRE(conflict_payload);
    flowmesh::WireMessage conflict;
    conflict.kind = flowmesh::WireMessageKind::CERTIFICATE;
    conflict.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1, market_a, 7, 0};
    conflict.payload = *conflict_payload;
    BOOST_REQUIRE(runtimes[0]->EnqueueWireMessage(56, conflict) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] {
        const auto status{runtimes[0]->MarketStatus(market_a)};
        return status &&
               status->halt ==
                   node::FlowMeshRuntimeHalt::CERTIFICATE_CONFLICT;
    }));
    BOOST_REQUIRE(runtimes[0]->MarketStatus(market_b));
    BOOST_CHECK(runtimes[0]->MarketStatus(market_b)->halt ==
                node::FlowMeshRuntimeHalt::NONE);

    for (auto& runtime : runtimes) runtime->Stop();
}

BOOST_AUTO_TEST_CASE(action_bearing_signing_lock_resumes_after_restart)
{
    const uint256 domain{Filled(0x15)};
    const modern::AssetId asset{Filled(0x35)};
    const flowmesh::MarketId market{
        *flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const flowmesh::VaultId vault{
        *flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x55)};
    const SeatFixture seats{
        Seats(domain, market, 4, 7, 100, Filled(0x71), 81)};

    CKey account_key;
    account_key.MakeNewKey(/*fCompressedIn=*/true);
    const flowmesh::AccountId account{
        flowmesh::AccountForKey(XOnlyPubKey{account_key.GetPubKey()})};
    flowmesh::FlowMeshState initial{
        vault, asset, modern::NativeAsset(),
        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    BOOST_REQUIRE(flowmesh::test_only::StateFunding::Fund(
        initial, account, modern::NativeAsset(), 100));

    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(seats.seats);
    FixedClock clock;
    std::array<RuntimeKeys, 3> keys;
    for (size_t i{0}; i < keys.size(); ++i) {
        keys[i].m_keys[market] = {seats.secrets[i]};
    }

    const fs::path base_path{
        m_args.GetDataDirBase() / "flowmesh_runtime_locked_restart"};
    std::array<fs::path, 3> paths;
    std::array<std::unique_ptr<node::FlowMeshProductionStore>, 3> stores;
    RuntimeNetwork network;
    std::array<std::unique_ptr<node::FlowMeshRuntime>, 3> runtimes;
    std::string error;
    for (size_t i{0}; i < stores.size(); ++i) {
        paths[i] = base_path / fs::PathFromString(std::to_string(i));
        stores[i] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
            .path = paths[i], .cache_bytes = size_t{1} << 20,
            .wipe_data = true});
        BOOST_REQUIRE_MESSAGE(stores[i]->OpenForMarket(
                                  domain, market, seats.seats, initial.Root(),
                                  error),
                              error);
        node::FlowMeshRuntimeConfig config;
        config.chain = &chain;
        config.keys = &keys[i];
        config.clock = &clock;
        config.round_timeout = std::chrono::hours{1};
        config.relay = [&network, i](node::FlowMeshRuntimeRelay relay) {
            network.Relay(i, std::move(relay));
            return LegacyRelayResult();
        };
        runtimes[i] = std::make_unique<node::FlowMeshRuntime>(
            std::move(config),
            std::vector<node::FlowMeshRuntimeMarketConfig>{MarketConfig(
                domain, market, treasury, seats.seats, initial, *stores[i],
                nullptr)});
        network.Set(i, runtimes[i].get(), true);
        BOOST_REQUIRE_MESSAGE(runtimes[i]->Start(error), error);
    }

    // First establish the mandatory empty market-genesis entry.
    runtimes[0]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        for (const auto& runtime : runtimes) {
            const auto status{runtime->MarketStatus(market)};
            if (!status || status->next_sequence != 1 ||
                status->halt != node::FlowMeshRuntimeHalt::NONE) {
                return false;
            }
        }
        return true;
    }));

    // Sequence one belongs to seat one. Isolate it after action admission so
    // it durably locks an action-bearing candidate but cannot reach quorum.
    network.Set(0, runtimes[0].get(), false);
    network.Set(2, runtimes[2].get(), false);
    BOOST_REQUIRE_EQUAL(flowmesh::ProductionProposerSeatIndex(
                            1, 0, seats.seats.Size()),
                        1U);
    BOOST_CHECK(keys[1].m_keys.at(market).front().GetPublicKey() ==
                seats.seats.members[1].key.Key());
    flowmesh::Action withdrawal;
    withdrawal.signer = account;
    withdrawal.type = static_cast<uint8_t>(flowmesh::ActionType::WITHDRAW);
    withdrawal.asset = modern::NativeAsset();
    withdrawal.amount = 10;
    withdrawal.destination = Filled(0xa5);
    BOOST_REQUIRE(flowmesh::SignAction(account_key, domain,
                                       initial.ConfigId(), withdrawal));
    BOOST_REQUIRE(runtimes[1]->SubmitLocalAction(market, withdrawal) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] {
        const auto status{runtimes[1]->MarketStatus(market)};
        return status && status->pending_actions == 1;
    }));
    runtimes[1]->NotifyTick();
    BOOST_REQUIRE(runtimes[1]->WaitForIdle(std::chrono::seconds{2}));
    const auto isolated_status{runtimes[1]->MarketStatus(market)};
    BOOST_REQUIRE(isolated_status.has_value());
    BOOST_CHECK_EQUAL(isolated_status->next_sequence, 1U);
    BOOST_CHECK_EQUAL(isolated_status->pending_actions, 1U);
    BOOST_CHECK_EQUAL(isolated_status->round, 0U);
    BOOST_CHECK(!isolated_status->observer_only);
    BOOST_CHECK(isolated_status->halt == node::FlowMeshRuntimeHalt::NONE);

    const flowmesh::ProductionSignPosition position{seats.seats.epoch, 1};
    std::optional<uint256> locked_hash;
    BOOST_REQUIRE(stores[1]->ReadLock(position, locked_hash, error));
    BOOST_REQUIRE(locked_hash.has_value());
    std::optional<node::StoredLockedProductionCandidate> retained;
    BOOST_REQUIRE(stores[1]->ReadLockedCandidate(position, retained, error));
    BOOST_REQUIRE(retained.has_value());
    BOOST_REQUIRE_EQUAL(retained->evidence.size(), 1U);
    BOOST_CHECK(retained->evidence.front().credential ==
                withdrawal.credential);
    BOOST_CHECK(retained->entry.GetHash() == *locked_hash);

    // Destroy both runtime and DB handle, then reopen from only durable state.
    network.Set(1, runtimes[1].get(), false);
    runtimes[1]->Stop();
    runtimes[1].reset();
    stores[1].reset();
    stores[1] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
        .path = paths[1], .cache_bytes = size_t{1} << 20});
    BOOST_REQUIRE_MESSAGE(stores[1]->OpenForMarket(
                              domain, market, seats.seats, initial.Root(),
                              error),
                          error);
    RuntimeSeatSource source;
    source.m_domain = domain;
    source.m_seats = seats.seats;
    flowmesh::FlowMeshState replayed{initial};
    uint256 replayed_hash;
    BOOST_REQUIRE_MESSAGE(stores[1]->Replay(
                              replayed, replayed_hash, source,
                              {chain.TipHeight(), std::nullopt, &chain},
                              treasury, nullptr, error),
                          error);

    auto restarted_market{MarketConfig(domain, market, treasury, seats.seats,
                                       replayed, *stores[1], nullptr)};
    restarted_market.next_sequence = 1;
    restarted_market.last_microblock_hash = replayed_hash;
    node::FlowMeshRuntimeConfig restarted_config;
    restarted_config.chain = &chain;
    restarted_config.keys = &keys[1];
    restarted_config.clock = &clock;
    restarted_config.round_timeout = std::chrono::hours{1};
    std::atomic<size_t> restarted_signing_messages{0};
    restarted_config.relay = [&network, &restarted_signing_messages](node::FlowMeshRuntimeRelay relay) {
        if (relay.message.kind == flowmesh::WireMessageKind::PROPOSAL ||
            relay.message.kind == flowmesh::WireMessageKind::ATTESTATION ||
            relay.message.kind == flowmesh::WireMessageKind::CERTIFICATE) {
            ++restarted_signing_messages;
        }
        network.Relay(1, std::move(relay));
        return LegacyRelayResult();
    };
    runtimes[1] = std::make_unique<node::FlowMeshRuntime>(
        std::move(restarted_config),
        std::vector<node::FlowMeshRuntimeMarketConfig>{restarted_market});
    network.Set(1, runtimes[1].get(), true);
    // Match the production service: the durable store is restored before
    // chain/index reconciliation opens the live anchor and transition gate.
    chain.SetReconciled(false);
    BOOST_REQUIRE_MESSAGE(runtimes[1]->Start(error), error);
    const auto paused_status{runtimes[1]->MarketStatus(market)};
    BOOST_REQUIRE(paused_status);
    BOOST_CHECK(paused_status->paused);
    BOOST_CHECK(paused_status->halt == node::FlowMeshRuntimeHalt::NONE);
    BOOST_CHECK_EQUAL(paused_status->next_sequence, 1U);
    BOOST_CHECK(paused_status->state_root == replayed.Root());
    runtimes[1]->NotifyTick();
    BOOST_REQUIRE(runtimes[1]->SubmitLocalAction(market, withdrawal) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtimes[1]->WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(restarted_signing_messages.load(), 0U);
    BOOST_CHECK(runtimes[1]->MarketStatus(market)->paused);
    const uint256 expected_lock{*locked_hash};
    BOOST_REQUIRE(stores[1]->ReadLock(position, locked_hash, error));
    BOOST_REQUIRE(locked_hash);
    BOOST_CHECK(*locked_hash == expected_lock);
    retained.reset();
    BOOST_REQUIRE(stores[1]->ReadLockedCandidate(position, retained, error));
    BOOST_REQUIRE(retained);
    BOOST_CHECK(retained->entry.GetHash() == expected_lock);
    BOOST_REQUIRE_EQUAL(retained->evidence.size(), 1U);
    BOOST_CHECK(retained->evidence.front().credential == withdrawal.credential);
    for (const auto& runtime : runtimes) {
        BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
    }
    // The closed-gate tick still advances the bounded relay scheduler. Model
    // the next real tick instead of expecting a retry at a frozen timestamp.
    clock.m_now += node::FlowMeshEvidenceRetryBudget::INTERVAL;
    chain.SetReconciled(true);

    // The restarted proposer re-authenticates/re-executes the exact retained
    // candidate, re-gossips its evidence, and may sign only the same hash.
    network.Set(0, runtimes[0].get(), true);
    network.Set(2, runtimes[2].get(), true);
    runtimes[1]->NotifyTick();
    // Retained evidence now uses the bounded scheduler. A proposal can be
    // dequeued ahead of its action frames; retry normally after their arrival.
    for (const size_t i : {size_t{1}, size_t{0}, size_t{2}}) {
        BOOST_REQUIRE(runtimes[i]->WaitForIdle(std::chrono::seconds{2}));
    }
    clock.m_now += node::FlowMeshCommitteeRelayBudget::REPEAT_DELAY;
    runtimes[1]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        for (const auto& runtime : runtimes) {
            const auto status{runtime->MarketStatus(market)};
            if (!status || status->next_sequence != 2 ||
                status->halt != node::FlowMeshRuntimeHalt::NONE) {
                return false;
            }
        }
        return true;
    }));
    BOOST_REQUIRE(runtimes[1]->WaitForIdle(std::chrono::seconds{2}));
    retained.reset();
    BOOST_REQUIRE(stores[1]->ReadLockedCandidate(position, retained, error));
    BOOST_CHECK(!retained.has_value());
    BOOST_REQUIRE(stores[1]->ReadLock(position, locked_hash, error));
    BOOST_REQUIRE(locked_hash.has_value());

    for (auto& runtime : runtimes) runtime->Stop();
}

BOOST_AUTO_TEST_CASE(future_proposal_proactively_catches_up_stale_validator)
{
    const uint256 domain{Filled(0x16)};
    const modern::AssetId asset{Filled(0x36)};
    const flowmesh::MarketId market{
        *flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const flowmesh::VaultId vault{
        *flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x56)};
    const flowmesh::AccountId account{Filled(0x66)};
    const SeatFixture seats{
        Seats(domain, market, 4, 7, 100, Filled(0x71), 101)};

    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(seats.seats);
    FixedClock clock;
    MapDeposits deposits;
    deposits.required_anchor = chain.m_current;
    const COutPoint deposit_outpoint{
        Txid::FromUint256(Filled(0x86)), 0};
    deposits.entries.emplace(
        deposit_outpoint, flowmesh::DepositInfo{asset, 250, account});
    flowmesh::FlowMeshState initial{
        vault, asset, modern::NativeAsset(),
        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};

    std::array<RuntimeKeys, 4> keys;
    std::array<std::unique_ptr<node::FlowMeshProductionStore>, 4> stores;
    RuntimeNetwork network;
    std::array<std::unique_ptr<node::FlowMeshRuntime>, 4> runtimes;
    std::string error;
    for (size_t i{0}; i < runtimes.size(); ++i) {
        keys[i].m_keys[market] = {seats.secrets[i]};
        stores[i] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
            .path = m_args.GetDataDirBase() / fs::PathFromString(
                        "flowmesh_runtime_proactive_catchup_" +
                        std::to_string(i)),
            .cache_bytes = size_t{1} << 20,
            .wipe_data = true});
        BOOST_REQUIRE_MESSAGE(stores[i]->OpenForMarket(
                                  domain, market, seats.seats, initial.Root(),
                                  error),
                              error);
        node::FlowMeshRuntimeConfig config;
        config.chain = &chain;
        config.keys = &keys[i];
        config.clock = &clock;
        config.round_timeout = std::chrono::hours{1};
        config.relay = [&network, i](node::FlowMeshRuntimeRelay relay) {
            network.Relay(i, std::move(relay));
            return LegacyRelayResult();
        };
        runtimes[i] = std::make_unique<node::FlowMeshRuntime>(
            std::move(config),
            std::vector<node::FlowMeshRuntimeMarketConfig>{MarketConfig(
                domain, market, treasury, seats.seats, initial, *stores[i],
                &deposits)});
        // D starts offline while A/B/C are exactly the three-seat threshold.
        network.Set(i, runtimes[i].get(), i < 3);
        BOOST_REQUIRE_MESSAGE(runtimes[i]->Start(error), error);
    }

    runtimes[0]->NotifyTick(); // A proposes sequence-zero market genesis.
    BOOST_REQUIRE(WaitUntil([&] {
        for (size_t i{0}; i < 3; ++i) {
            const auto status{runtimes[i]->MarketStatus(market)};
            if (!status || status->next_sequence != 1 ||
                status->halt != node::FlowMeshRuntimeHalt::NONE) {
                return false;
            }
        }
        const auto stale{runtimes[3]->MarketStatus(market)};
        return stale && stale->next_sequence == 0;
    }));

    std::optional<node::StoredProductionEntry> genesis;
    BOOST_REQUIRE(stores[1]->ReadEntry(0, seats.seats, genesis, error));
    BOOST_REQUIRE(genesis.has_value());
    const auto state_after_genesis{runtimes[1]->StateSnapshot(market)};
    BOOST_REQUIRE(state_after_genesis.has_value());

    // A leaves. B/C alone are below threshold; D returns one entry stale.
    network.Set(0, runtimes[0].get(), false);
    network.Set(3, runtimes[3].get(), true);

    const flowmesh::Action deposit{Deposit(deposit_outpoint)};
    flowmesh::ProductionEpochGate gate{domain, market, seats.seats};
    flowmesh::ProductionEntryCheck entry_check;
    const std::vector<flowmesh::Action> actions{deposit};
    const auto future{flowmesh::BuildProductionExecutionEntry(
        *state_after_genesis, domain, market, seats.seats, gate,
        /*sequence=*/1, /*effect_start=*/0, genesis->entry.GetHash(),
        chain.m_current,
        {chain.TipHeight(), genesis->entry.anchor, &chain}, treasury, actions,
        &deposits, entry_check)};
    BOOST_REQUIRE_MESSAGE(future,
                          flowmesh::ProductionEntryCheckName(entry_check));

    flowmesh::ProductionProposalEnvelope proposal;
    proposal.entry = future->entry;
    proposal.round = 0;
    proposal.proposer_seat_index = flowmesh::ProductionProposerSeatIndex(
        proposal.entry.sequence, proposal.round, seats.seats.Size());
    BOOST_REQUIRE_EQUAL(proposal.proposer_seat_index, 1U);
    const uint256 digest{
        flowmesh::ProductionProposalDigest(proposal.entry, proposal.round)};
    proposal.proposer_signature =
        seats.secrets[proposal.proposer_seat_index]
            .Sign(std::span<const unsigned char>{digest.begin(), 32})
            .Compressed();
    const auto proposal_payload{
        flowmesh::EncodeProductionProposalPayload(proposal)};
    BOOST_REQUIRE(proposal_payload.has_value());
    flowmesh::WireMessage future_proposal;
    future_proposal.kind = flowmesh::WireMessageKind::PROPOSAL;
    future_proposal.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1, market,
                              seats.seats.epoch, proposal.entry.sequence};
    future_proposal.payload = *proposal_payload;

    // A merely well-shaped future frame is insufficient: an invalid proposer
    // signature must not make D send a catch-up request.
    auto forged{proposal};
    forged.proposer_signature[0] ^= 1;
    const auto forged_payload{
        flowmesh::EncodeProductionProposalPayload(forged)};
    BOOST_REQUIRE(forged_payload.has_value());
    flowmesh::WireMessage forged_wire{future_proposal};
    forged_wire.payload = *forged_payload;
    BOOST_REQUIRE(runtimes[3]->EnqueueWireMessage(1, std::move(forged_wire)) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtimes[3]->WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(runtimes[3]->MarketStatus(market)->next_sequence, 0U);

    // B's authenticated future proposal is enough to discover the lag. D asks
    // B for the bounded missing range and installs sequence zero without first
    // requiring the impossible sequence-one certificate.
    BOOST_REQUIRE(runtimes[3]->EnqueueWireMessage(1, future_proposal) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] {
        const auto status{runtimes[3]->MarketStatus(market)};
        return status && status->next_sequence == 1 &&
               status->halt == node::FlowMeshRuntimeHalt::NONE;
    }));

    // Local diagnostic counters describe observations without promoting a
    // pre-candidate signature to a verified vote or creating a signing lock.
    const auto early_signature{flowmesh::SignBlsMicroblockCertificate(
        seats.secrets[2], flowmesh::ProductionCertificateContext(future->entry),
        seats.seats)};
    BOOST_REQUIRE(early_signature);
    const auto early_payload{flowmesh::EncodeProductionAttestationPayload(
        flowmesh::IndexedBlsSignature{2, *early_signature})};
    BOOST_REQUIRE(early_payload);
    flowmesh::WireMessage early_vote;
    early_vote.kind = flowmesh::WireMessageKind::ATTESTATION;
    early_vote.header = future_proposal.header;
    early_vote.payload = *early_payload;
    BOOST_REQUIRE(runtimes[3]->EnqueueWireMessage(2, std::move(early_vote)) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtimes[3]->EnqueueWireMessage(1, future_proposal) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtimes[3]->WaitForIdle(std::chrono::seconds{2}));
    const auto missing{runtimes[3]->MarketData(market, std::nullopt, {}, error)};
    BOOST_REQUIRE_MESSAGE(missing, error);
    BOOST_CHECK_EQUAL(missing->snapshot.runtime.proposals_missing_evidence, 1U);
    BOOST_CHECK_EQUAL(missing->snapshot.runtime.attestations_without_candidate, 1U);
    BOOST_CHECK_EQUAL(missing->snapshot.runtime.max_verified_attestations, 0U);
    BOOST_CHECK_EQUAL(missing->snapshot.runtime.candidate_count, 0U);
    BOOST_CHECK(!missing->snapshot.runtime.local_locked_candidate);
    BOOST_CHECK(missing->snapshot.runtime.last_message_observed_at.has_value());

    // With D caught up, the same B/C/D set now receives the action and regains
    // its three-seat threshold even though A remains offline.
    BOOST_REQUIRE(runtimes[1]->SubmitLocalAction(market, deposit) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] {
        for (const size_t i : {size_t{1}, size_t{2}, size_t{3}}) {
            const auto status{runtimes[i]->MarketStatus(market)};
            if (!status || status->pending_actions != 1) return false;
        }
        return true;
    }));
    runtimes[1]->NotifyTick(); // sequence one proposer is ordered seat one.
    BOOST_REQUIRE(WaitUntil([&] {
        for (const size_t i : {size_t{1}, size_t{2}, size_t{3}}) {
            const auto status{runtimes[i]->MarketStatus(market)};
            if (!status || status->next_sequence != 2 ||
                status->halt != node::FlowMeshRuntimeHalt::NONE) {
                return false;
            }
        }
        return true;
    }));

    for (auto& runtime : runtimes) runtime->Stop();
}

BOOST_AUTO_TEST_CASE(independent_rounds_recover_quorum_through_observer)
{
    const uint256 domain{Filled(0x28)};
    const modern::AssetId asset{Filled(0x48)};
    const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const auto vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x68)};
    const SeatFixture seats{Seats(domain, market, 4, 7, 100, Filled(0x71), 128)};
    const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(),
                                        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(seats.seats);
    MapDeposits deposits;
    deposits.required_anchor = chain.m_current;
    const flowmesh::AccountId account{Filled(0x88)};
    const COutPoint deposit_outpoint{Txid::FromUint256(Filled(0xa8)), 0};
    deposits.entries.emplace(deposit_outpoint,
                             flowmesh::DepositInfo{asset, 250, account});
    std::array<FixedClock, 4> clocks;
    std::array<RuntimeKeys, 4> keys;
    std::array<std::unique_ptr<node::FlowMeshProductionStore>, 4> stores;
    RuntimeNetwork network;
    std::array<std::unique_ptr<node::FlowMeshRuntime>, 4> runtimes;
    std::atomic<size_t> observer_proposals{0};
    // Workers must stop before any state captured by the relay filter dies,
    // including when the pre-fix implementation fails the convergence check.
    struct StopRuntimes {
        decltype(runtimes)& all;
        ~StopRuntimes() { for (const auto& runtime : all) if (runtime) runtime->Stop(); }
    } stop{runtimes};
    network.SetFilter([](size_t, size_t, const auto&) { return false; });
    std::string error;
    for (size_t i{0}; i < runtimes.size(); ++i) {
        stores[i] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
            .path = m_args.GetDataDirBase() / fs::PathFromString(
                "flowmesh_independent_rounds_" + std::to_string(i)),
            .cache_bytes = size_t{1} << 20, .wipe_data = true});
        BOOST_REQUIRE_MESSAGE(stores[i]->OpenForMarket(
            domain, market, seats.seats, initial.Root(), error), error);
        node::FlowMeshRuntimeConfig config;
        config.chain = &chain;
        config.keys = &keys[i];
        config.clock = &clocks[i];
        config.round_timeout = std::chrono::seconds{1};
        config.relay = [&network, i](node::FlowMeshRuntimeRelay relay) {
            network.Relay(i, std::move(relay));
            return LegacyRelayResult();
        };
        runtimes[i] = std::make_unique<node::FlowMeshRuntime>(config,
            std::vector<node::FlowMeshRuntimeMarketConfig>{MarketConfig(
                domain, market, treasury, seats.seats, initial, *stores[i], &deposits)});
        network.Set(i, runtimes[i].get(), true);
        BOOST_REQUIRE_MESSAGE(runtimes[i]->Start(error), error);
    }

    // Different observer/startup durations leave the same sequence at widely
    // separated rounds. Advance each real worker independently while unarmed:
    // no preexisting candidate or signing conflict can explain a later stall.
    const std::array<uint32_t, 4> rounds{4, 7, 2, 0};
    for (size_t i{0}; i < runtimes.size(); ++i) {
        for (uint32_t round{0}; round < rounds[i]; ++round) {
            clocks[i].m_now += std::chrono::seconds{1};
            runtimes[i]->NotifyTick();
            BOOST_REQUIRE(runtimes[i]->WaitForIdle(std::chrono::seconds{2}));
        }
        BOOST_REQUIRE_EQUAL(runtimes[i]->MarketStatus(market)->round, rounds[i]);
        BOOST_REQUIRE_EQUAL(runtimes[i]->MarketStatus(market)->next_sequence, 0U);
        std::optional<uint256> locked;
        BOOST_REQUIRE(stores[i]->ReadLock({seats.seats.epoch, 0}, locked, error));
        BOOST_REQUIRE(!locked);
    }
    // The provider maps are changed only with all workers idle. Three of four
    // seats are online and every route between the signers crosses the keyless
    // observer: seat0 -- observer -- seat1 -- seat2.
    keys[0].m_keys[market] = {seats.secrets[0]};
    keys[2].m_keys[market] = {seats.secrets[1]};
    keys[3].m_keys[market] = {seats.secrets[2]};
    const RuntimeNetwork::Filter line_filter{[&](size_t from, size_t to, const auto& wire) {
        if (!(from + 1 == to || to + 1 == from)) return false;
        if (from == 1 && wire.kind == flowmesh::WireMessageKind::PROPOSAL) {
            ++observer_proposals;
        }
        return true;
    }};
    network.SetFilter(line_filter);
    BOOST_REQUIRE_EQUAL(flowmesh::ProductionProposerSeatIndex(
        0, rounds[0], seats.seats.Size()), 0U);
    runtimes[0]->NotifyTick();
    BOOST_REQUIRE_MESSAGE(WaitUntil([&] {
        for (const auto& runtime : runtimes) {
            const auto status{runtime->MarketStatus(market)};
            if (!status || status->next_sequence != 1 ||
                status->halt != node::FlowMeshRuntimeHalt::NONE) return false;
        }
        return true;
    }), "same-sequence proposals must recover quorum across independent local rounds");
    BOOST_CHECK_GT(observer_proposals.load(), 0U);
    BOOST_CHECK(runtimes[1]->MarketStatus(market)->observer_only);
    const auto committed_hash{runtimes[0]->MarketStatus(market)->last_microblock_hash};
    for (size_t i{0}; i < runtimes.size(); ++i) {
        BOOST_REQUIRE(runtimes[i]->WaitForIdle(std::chrono::seconds{2}));
        BOOST_CHECK(runtimes[i]->MarketStatus(market)->last_microblock_hash == committed_hash);
        std::optional<node::StoredProductionEntry> committed;
        BOOST_REQUIRE(stores[i]->ReadEntry(0, seats.seats, committed, error));
        BOOST_REQUIRE(committed);
        BOOST_CHECK(flowmesh::CheckProductionEntryCertificate(
            committed->entry, seats.seats, committed->certificate) ==
            flowmesh::BlsCertificateCheck::OK);
        if (i != 1) {
            std::optional<uint256> locked;
            BOOST_REQUIRE(stores[i]->ReadLock({seats.seats.epoch, 0}, locked, error));
            BOOST_REQUIRE(locked);
            BOOST_CHECK(*locked == committed_hash);
        }
    }

    // Repeat at a non-genesis position with real authenticated action
    // evidence. Empty genesis alone cannot exercise deposit propagation,
    // candidate execution, or the resulting ledger credit across the observer.
    const auto drain = [&] {
        for (size_t pass{0}; pass < 4; ++pass) {
            for (const auto& runtime : runtimes) {
                BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
            }
        }
    };
    drain();
    for (auto& provider : keys) provider.m_keys.clear();
    network.SetFilter([](size_t, size_t, const auto&) { return false; });
    const std::array<uint32_t, 4> action_rounds{3, 7, 1, 5};
    for (size_t i{0}; i < runtimes.size(); ++i) {
        BOOST_REQUIRE_EQUAL(runtimes[i]->MarketStatus(market)->round, 0U);
        for (uint32_t round{0}; round < action_rounds[i]; ++round) {
            clocks[i].m_now += std::chrono::seconds{1};
            runtimes[i]->NotifyTick();
            BOOST_REQUIRE(runtimes[i]->WaitForIdle(std::chrono::seconds{2}));
        }
        BOOST_REQUIRE_EQUAL(runtimes[i]->MarketStatus(market)->round, action_rounds[i]);
        BOOST_REQUIRE_EQUAL(runtimes[i]->MarketStatus(market)->next_sequence, 1U);
        BOOST_CHECK_EQUAL(runtimes[i]->StateSnapshot(market)->LedgerView().Available(account, asset), 0);
        std::optional<uint256> locked;
        BOOST_REQUIRE(stores[i]->ReadLock({seats.seats.epoch, 1}, locked, error));
        BOOST_REQUIRE(!locked);
    }
    network.SetFilter(line_filter);
    BOOST_REQUIRE(runtimes[0]->SubmitLocalAction(market, Deposit(deposit_outpoint)) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] {
        return std::all_of(runtimes.begin(), runtimes.end(), [&](const auto& runtime) {
            return runtime->MarketStatus(market)->pending_actions == 1;
        });
    }));
    drain();
    keys[0].m_keys[market] = {seats.secrets[0]};
    keys[2].m_keys[market] = {seats.secrets[1]};
    keys[3].m_keys[market] = {seats.secrets[2]};
    const auto forwarded_before{observer_proposals.load()};
    BOOST_REQUIRE_EQUAL(flowmesh::ProductionProposerSeatIndex(
        1, action_rounds[0], seats.seats.Size()), 0U);
    runtimes[0]->NotifyTick();
    BOOST_REQUIRE_MESSAGE(WaitUntil([&] {
        return std::all_of(runtimes.begin(), runtimes.end(), [&](const auto& runtime) {
            const auto status{runtime->MarketStatus(market)};
            return status && status->next_sequence == 2 &&
                   status->halt == node::FlowMeshRuntimeHalt::NONE;
        });
    }), "action-bearing proposals must recover quorum across independent local rounds");
    drain();
    BOOST_CHECK_GT(observer_proposals.load(), forwarded_before);
    BOOST_CHECK(runtimes[1]->MarketStatus(market)->observer_only);
    const auto deposit_hash{runtimes[0]->MarketStatus(market)->last_microblock_hash};
    const auto deposit_root{runtimes[0]->StateSnapshot(market)->Root()};
    BOOST_CHECK(deposit_hash != committed_hash);
    for (size_t i{0}; i < runtimes.size(); ++i) {
        BOOST_CHECK(runtimes[i]->MarketStatus(market)->last_microblock_hash == deposit_hash);
        const auto state{runtimes[i]->StateSnapshot(market)};
        BOOST_REQUIRE(state);
        BOOST_CHECK(state->Root() == deposit_root);
        BOOST_CHECK_EQUAL(state->LedgerView().Available(account, asset), 250);
        BOOST_CHECK_EQUAL(state->LedgerView().Reserved(account, asset), 0);
        std::optional<node::StoredProductionEntry> committed;
        BOOST_REQUIRE(stores[i]->ReadEntry(1, seats.seats, committed, error));
        BOOST_REQUIRE(committed);
        BOOST_CHECK(committed->entry.parent_hash == committed_hash);
        BOOST_REQUIRE_EQUAL(committed->entry.actions.size(), 1U);
        BOOST_CHECK(committed->entry.actions.front().outpoint == deposit_outpoint);
        BOOST_CHECK(flowmesh::CheckProductionEntryCertificate(
            committed->entry, seats.seats, committed->certificate) ==
            flowmesh::BlsCertificateCheck::OK);
        if (i != 1) {
            std::optional<uint256> locked;
            BOOST_REQUIRE(stores[i]->ReadLock({seats.seats.epoch, 1}, locked, error));
            BOOST_REQUIRE(locked);
            BOOST_CHECK(*locked == deposit_hash);
        }
    }
}

BOOST_AUTO_TEST_CASE(normal_and_restarted_proposer_round_deadlines)
{
    // Injected time measures scheduling decisions only, not network or disk
    // throughput. Run the no-restart control independently of the fault case.
    for (const bool restart_proposer : {false, true}) {
        const std::string scenario{restart_proposer ? "restarted_proposer" : "normal"};
        const uint256 domain{Filled(0x2a)};
        const modern::AssetId asset{Filled(0x4a)};
        const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
        const auto vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
        const uint256 treasury{Filled(0x6a)};
        const SeatFixture seats{Seats(domain, market, 4, 7, 100, Filled(0x71), 130)};
        const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(),
                                            flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
        RuntimeChain chain;
        chain.m_domain = domain;
        chain.Add(seats.seats);
        MapDeposits deposits;
        deposits.required_anchor = chain.m_current;
        const flowmesh::AccountId account{Filled(0x8a)};
        const COutPoint outpoint{Txid::FromUint256(Filled(0xaa)), 0};
        deposits.entries.emplace(outpoint, flowmesh::DepositInfo{asset, 250, account});
        std::array<FixedClock, 4> clocks;
        std::array<RuntimeKeys, 4> keys;
        std::array<std::unique_ptr<node::FlowMeshProductionStore>, 4> stores;
        RuntimeNetwork network;
        network.IgnoreHellos();
        std::array<std::unique_ptr<node::FlowMeshRuntime>, 4> runtimes;
        struct StopRuntimes {
            decltype(runtimes)& all;
            ~StopRuntimes() { for (const auto& runtime : all) if (runtime) runtime->Stop(); }
        } stop{runtimes};
        std::string error;
        const auto runtime_config = [&](const size_t i) {
            node::FlowMeshRuntimeConfig config;
            config.chain = &chain;
            config.keys = &keys[i];
            config.clock = &clocks[i];
            config.round_timeout = std::chrono::seconds{2};
            config.relay = [&network, i](node::FlowMeshRuntimeRelay relay) {
                network.Relay(i, std::move(relay));
                return LegacyRelayResult();
            };
            return config;
        };
        const auto drain = [&] {
            for (size_t pass{0}; pass < 4; ++pass) {
                for (const auto& runtime : runtimes) {
                    BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
                }
            }
        };
        const auto set_time = [&](const int64_t micros) {
            drain();
            for (auto& clock : clocks) {
                clock.m_now = flowmesh::WireClock::time_point{std::chrono::microseconds{micros}};
            }
        };
        const auto tick_all = [&] {
            for (const auto& runtime : runtimes) runtime->NotifyTick();
            drain();
        };
        const auto observe_rounds = [&](const char* phase, const uint64_t sequence) {
            for (size_t i{0}; i < runtimes.size(); ++i) {
                const auto snapshots{runtimes[i]->DeliverySnapshots()};
                BOOST_REQUIRE_EQUAL(snapshots.size(), 1U);
                const auto& events{snapshots.front().events};
                const auto entered{std::find_if(events.rbegin(), events.rend(), [&](const auto& event) {
                    return event.stage == "round_entered" && event.sequence == sequence;
                })};
                BOOST_REQUIRE(entered != events.rend());
                BOOST_REQUIRE(entered->round);
                BOOST_REQUIRE(entered->seat_index);
                BOOST_CHECK(entered->reason.find(" start_us=") != std::string::npos);
                BOOST_CHECK(entered->reason.find(" deadline_us=") != std::string::npos);
                BOOST_TEST_MESSAGE("scheduler_event scenario=" << scenario << " node=" << i
                    << " phase=" << phase << " stage=" << entered->stage
                    << " event_us=" << entered->monotonic_us << " round=" << *entered->round
                    << " proposer=" << *entered->seat_index << " reason=" << entered->reason);
            }
        };
        for (size_t i{0}; i < runtimes.size(); ++i) {
            keys[i].m_keys[market] = {seats.secrets[i]};
            stores[i] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
                .path = m_args.GetDataDirBase() / fs::PathFromString(
                    "flowmesh_round_deadline_" + scenario + "_" + std::to_string(i)),
                .cache_bytes = size_t{1} << 20, .wipe_data = true});
            BOOST_REQUIRE_MESSAGE(stores[i]->OpenForMarket(
                domain, market, seats.seats, initial.Root(), error), error);
            runtimes[i] = std::make_unique<node::FlowMeshRuntime>(runtime_config(i),
                std::vector<node::FlowMeshRuntimeMarketConfig>{MarketConfig(
                    domain, market, treasury, seats.seats, initial, *stores[i], &deposits)});
            network.Set(i, runtimes[i].get(), true);
            BOOST_REQUIRE_MESSAGE(runtimes[i]->Start(error), error);
        }
        runtimes[0]->NotifyTick();
        BOOST_REQUIRE(WaitUntil([&] {
            return std::all_of(runtimes.begin(), runtimes.end(), [&](const auto& runtime) {
                return runtime->MarketStatus(market)->next_sequence == 1;
            });
        }));
        drain();
        observe_rounds("previous_certificate", 1);
        for (auto& provider : keys) provider.m_keys.clear();
        // CommitCertified set round zero at 100s on every node. A later
        // restart replaces only node0's local timer, just as Run5's reindex
        // happened after sequence six had already committed everywhere.
        if (restart_proposer) {
            set_time(101100000);
            const auto state{*runtimes[0]->StateSnapshot(market)};
            const auto status{*runtimes[0]->MarketStatus(market)};
            network.Set(0, nullptr, false);
            runtimes[0]->Stop();
            auto restored{MarketConfig(domain, market, treasury, seats.seats,
                                      state, *stores[0], &deposits)};
            restored.next_sequence = status.next_sequence;
            restored.next_effect_index = status.next_effect_index;
            restored.last_microblock_hash = status.last_microblock_hash;
            runtimes[0] = std::make_unique<node::FlowMeshRuntime>(
                runtime_config(0), std::vector<node::FlowMeshRuntimeMarketConfig>{restored});
            network.Set(0, runtimes[0].get(), true);
            BOOST_REQUIRE_MESSAGE(runtimes[0]->Start(error), error);
        }
        set_time(102224000);
        tick_all();
        for (size_t i{0}; i < runtimes.size(); ++i) {
            BOOST_CHECK_EQUAL(runtimes[i]->MarketStatus(market)->round,
                              restart_proposer && i == 0 ? 0U : 1U);
        }
        observe_rounds("before_action", 1);
        // Sequence1/round1 and Run5 sequence7/round11 both select seat2.
        const std::array<size_t, 4> local_seats{2, 1, 0, 3};
        for (size_t i{0}; i < keys.size(); ++i) {
            keys[i].m_keys[market] = {seats.secrets[local_seats[i]]};
        }
        BOOST_REQUIRE(runtimes[0]->SubmitLocalAction(market, Deposit(outpoint)) ==
                      flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(WaitUntil([&] {
            return std::all_of(runtimes.begin(), runtimes.end(), [&](const auto& runtime) {
                return runtime->MarketStatus(market)->pending_actions == 1;
            });
        }));
        tick_all();
        if (restart_proposer) {
            for (const int64_t now : {102500000LL, 102900000LL, 103099999LL}) {
                set_time(now);
                tick_all();
                BOOST_CHECK_EQUAL(runtimes[0]->MarketStatus(market)->round, 0U);
                for (const auto& runtime : runtimes) {
                    BOOST_CHECK_EQUAL(runtime->MarketStatus(market)->next_sequence, 1U);
                }
                const auto snapshots{runtimes[0]->DeliverySnapshots()};
                const auto& events{snapshots.front().events};
                const auto tick{std::find_if(events.rbegin(), events.rend(), [](const auto& event) {
                    return event.stage == "tick_market_lock_acquired";
                })};
                BOOST_REQUIRE(tick != events.rend());
                BOOST_CHECK_EQUAL(tick->monotonic_us, static_cast<uint64_t>(now));
                BOOST_CHECK(tick->reason.find("request_us=" + std::to_string(now)) != std::string::npos);
                BOOST_CHECK(tick->reason.find("dequeue_us=" + std::to_string(now)) != std::string::npos);
                BOOST_CHECK(tick->reason.find("lock_request_us=" + std::to_string(now)) != std::string::npos);
                BOOST_CHECK(tick->reason.find("locked_us=" + std::to_string(now)) != std::string::npos);
                BOOST_TEST_MESSAGE("scheduler_event scenario=" << scenario << " node=0"
                    << " phase=before_deadline stage=" << tick->stage
                    << " event_us=" << tick->monotonic_us << " round=" << *tick->round
                    << " reason=" << tick->reason);
            }
            set_time(103100000);
            tick_all();
        }
        BOOST_REQUIRE(WaitUntil([&] {
            return std::all_of(runtimes.begin(), runtimes.end(), [&](const auto& runtime) {
                return runtime->MarketStatus(market)->next_sequence == 2;
            });
        }));
        drain();
        observe_rounds("new_certificate", 2);
        const uint64_t expected_execution{restart_proposer ? 103100000U : 102224000U};
        const auto certified_hash{runtimes[0]->MarketStatus(market)->last_microblock_hash};
        for (size_t i{0}; i < runtimes.size(); ++i) {
            const auto snapshots{runtimes[i]->DeliverySnapshots()};
            BOOST_REQUIRE_EQUAL(snapshots.size(), 1U);
            const auto& events{snapshots.front().events};
            const auto admitted{std::find_if(events.begin(), events.end(), [&](const auto& event) {
                return event.stage == "action_verified" && event.object_id == Deposit(outpoint).Id();
            })};
            const auto applied{std::find_if(events.begin(), events.end(), [](const auto& event) {
                return event.stage == "durably_applied" && event.sequence == 1;
            })};
            const auto action_round{std::find_if(events.begin(), events.end(), [](const auto& event) {
                return event.stage == "action_round_observed" && event.sequence == 1;
            })};
            BOOST_REQUIRE(admitted != events.end());
            BOOST_REQUIRE(applied != events.end());
            BOOST_REQUIRE(action_round != events.end());
            BOOST_REQUIRE(action_round->round);
            BOOST_REQUIRE(action_round->seat_index);
            BOOST_CHECK_EQUAL(*action_round->round, restart_proposer && i == 0 ? 0U : 1U);
            BOOST_CHECK_EQUAL(*action_round->seat_index, restart_proposer && i == 0 ? 1U : 2U);
            BOOST_CHECK_EQUAL(admitted->monotonic_us, 102224000U);
            BOOST_CHECK_EQUAL(applied->monotonic_us, expected_execution);
            BOOST_CHECK(runtimes[i]->MarketStatus(market)->last_microblock_hash == certified_hash);
            BOOST_CHECK_EQUAL(runtimes[i]->StateSnapshot(market)->LedgerView().Available(account, asset), 250);
            BOOST_CHECK_EQUAL(runtimes[i]->MarketStatus(market)->round, 0U);
            std::optional<node::StoredProductionEntry> entry;
            BOOST_REQUIRE(stores[i]->ReadEntry(1, seats.seats, entry, error));
            BOOST_REQUIRE(entry);
            BOOST_CHECK(flowmesh::CheckProductionEntryCertificate(
                entry->entry, seats.seats, entry->certificate) == flowmesh::BlsCertificateCheck::OK);
            BOOST_TEST_MESSAGE("scheduler_scenario=" << scenario << " node=" << i
                << " admitted_us=" << admitted->monotonic_us
                << " applied_us=" << applied->monotonic_us
                << " virtual_wait_us=" << applied->monotonic_us - admitted->monotonic_us);
        }
    }
}

BOOST_AUTO_TEST_CASE(remote_round_cannot_drive_timer_or_bypass_proposal_validation)
{
    const uint256 domain{Filled(0x29)};
    const modern::AssetId asset{Filled(0x49)};
    const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const auto vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x69)};
    const SeatFixture seats{Seats(domain, market, 4, 7, 100, Filled(0x71), 129)};
    const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(),
                                        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(seats.seats);
    FixedClock clock;
    RuntimeKeys keys;
    keys.m_keys[market] = {seats.secrets[0]};
    node::FlowMeshProductionStore store{DBParams{
        .path = m_args.GetDataDirBase() / "flowmesh_remote_round_safety",
        .cache_bytes = size_t{1} << 20, .wipe_data = true}};
    std::string error;
    BOOST_REQUIRE_MESSAGE(store.OpenForMarket(
        domain, market, seats.seats, initial.Root(), error), error);
    std::atomic<size_t> votes{0};
    std::mutex vote_payload_mutex;
    std::vector<std::vector<unsigned char>> vote_payloads;
    node::FlowMeshRuntimeConfig config;
    config.chain = &chain;
    config.keys = &keys;
    config.clock = &clock;
    config.round_timeout = std::chrono::seconds{1};
    config.relay = [&](node::FlowMeshRuntimeRelay relay) {
        if (relay.message.kind == flowmesh::WireMessageKind::ATTESTATION) {
            std::lock_guard<std::mutex> guard{vote_payload_mutex};
            vote_payloads.push_back(relay.message.payload);
            ++votes;
        }
        return LegacyRelayResult();
    };
    node::FlowMeshRuntime runtime{config, {MarketConfig(
        domain, market, treasury, seats.seats, initial, store, nullptr)}};
    BOOST_REQUIRE_MESSAGE(runtime.Start(error), error);
    flowmesh::ProductionEpochGate gate{domain, market, seats.seats};
    flowmesh::ProductionEntryCheck check;
    const auto genesis{flowmesh::BuildProductionExecutionEntry(
        initial, domain, market, seats.seats, gate, 0, 0, {},
        {100, Filled(0x71)}, {chain.TipHeight(), std::nullopt, &chain},
        treasury, {}, nullptr, check)};
    BOOST_REQUIRE_MESSAGE(genesis, flowmesh::ProductionEntryCheckName(check));
    const auto proposal_wire = [&](const flowmesh::ProductionEntryCore& entry,
                                   const bool valid_signature = true) {
        flowmesh::ProductionProposalEnvelope proposal;
        proposal.entry = entry;
        proposal.round = std::numeric_limits<uint32_t>::max();
        proposal.proposer_seat_index = flowmesh::ProductionProposerSeatIndex(
            entry.sequence, proposal.round, seats.seats.Size());
        const auto digest{flowmesh::ProductionProposalDigest(entry, proposal.round)};
        const size_t signer{valid_signature ? proposal.proposer_seat_index
                                            : (proposal.proposer_seat_index + 1) % seats.seats.Size()};
        proposal.proposer_signature = seats.secrets[signer]
            .Sign(std::span<const unsigned char>{digest.begin(), 32}).Compressed();
        const auto payload{flowmesh::EncodeProductionProposalPayload(proposal)};
        BOOST_REQUIRE(payload);
        return flowmesh::WireMessage{flowmesh::WireMessageKind::PROPOSAL,
            {flowmesh::FLOWMESH_WIRE_VERSION_V1, market, seats.seats.epoch, 0}, *payload};
    };
    const auto assert_untouched = [&] {
        BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
        BOOST_CHECK_EQUAL(votes.load(), 0U);
        BOOST_CHECK_EQUAL(runtime.MarketStatus(market)->round, 0U);
        BOOST_CHECK_EQUAL(runtime.MarketStatus(market)->next_sequence, 0U);
        BOOST_CHECK(runtime.MarketStatus(market)->halt == node::FlowMeshRuntimeHalt::NONE);
        std::optional<uint256> locked;
        BOOST_REQUIRE(store.ReadLock({seats.seats.epoch, 0}, locked, error));
        BOOST_CHECK(!locked);
    };
    BOOST_REQUIRE(runtime.EnqueueWireMessage(3, proposal_wire(genesis->entry, false)) ==
                  flowmesh::QueueResult::ACCEPTED);
    assert_untouched();
    auto invalid{genesis->entry};
    invalid.domain = Filled(0xe1);
    BOOST_REQUIRE(runtime.EnqueueWireMessage(3, proposal_wire(invalid)) ==
                  flowmesh::QueueResult::ACCEPTED);
    assert_untouched();
    invalid = genesis->entry;
    invalid.anchor.hash = Filled(0xe2);
    BOOST_REQUIRE(runtime.EnqueueWireMessage(3, proposal_wire(invalid)) ==
                  flowmesh::QueueResult::ACCEPTED);
    assert_untouched();
    invalid = genesis->entry;
    invalid.state_root = Filled(0xe3);
    BOOST_REQUIRE(runtime.EnqueueWireMessage(3, proposal_wire(invalid)) ==
                  flowmesh::QueueResult::ACCEPTED);
    assert_untouched();

    // A fully checked proposal can earn our one durable vote even at the
    // maximum signed round. Its round must never become the local timer.
    BOOST_REQUIRE(runtime.EnqueueWireMessage(3, proposal_wire(genesis->entry)) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(votes.load(), 1U);
    BOOST_CHECK_EQUAL(runtime.MarketStatus(market)->round, 0U);
    std::optional<uint256> locked;
    BOOST_REQUIRE(store.ReadLock({seats.seats.epoch, 0}, locked, error));
    BOOST_REQUIRE(locked);
    BOOST_CHECK(*locked == genesis->entry.GetHash());
    clock.m_now += std::chrono::seconds{1};
    runtime.NotifyTick();
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(runtime.MarketStatus(market)->round, 1U);
    BOOST_CHECK_EQUAL(runtime.MarketStatus(market)->next_sequence, 0U); // One vote is not quorum.
    BOOST_CHECK(runtime.MarketStatus(market)->halt == node::FlowMeshRuntimeHalt::NONE);
    BOOST_REQUIRE_EQUAL(votes.load(), 2U); // due replay, not a second distinct vote
    {
        std::lock_guard<std::mutex> guard{vote_payload_mutex};
        BOOST_REQUIRE_EQUAL(vote_payloads.size(), 2U);
        BOOST_CHECK(vote_payloads[0] == vote_payloads[1]);
    }
    BOOST_REQUIRE(store.ReadLock({seats.seats.epoch, 0}, locked, error));
    BOOST_REQUIRE(locked);
    BOOST_CHECK(*locked == genesis->entry.GetHash());
    runtime.Stop();
}

BOOST_AUTO_TEST_CASE(authenticated_future_round_reunites_split_validators)
{
    const uint256 domain{Filled(0x17)};
    const modern::AssetId asset{Filled(0x37)};
    const flowmesh::MarketId market{
        *flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const flowmesh::VaultId vault{
        *flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x57)};
    const SeatFixture seats{
        Seats(domain, market, 4, 7, 100, Filled(0x71), 121)};

    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(seats.seats);
    MapDeposits deposits;
    deposits.required_anchor = chain.m_current;
    flowmesh::FlowMeshState initial{
        vault, asset, modern::NativeAsset(),
        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};

    std::array<FixedClock, 4> clocks;
    std::array<RuntimeKeys, 4> keys;
    std::array<std::unique_ptr<node::FlowMeshProductionStore>, 4> stores;
    RuntimeNetwork network;
    std::array<std::unique_ptr<node::FlowMeshRuntime>, 4> runtimes;
    std::string error;
    for (size_t i{0}; i < runtimes.size(); ++i) {
        keys[i].m_keys[market] = {seats.secrets[i]};
        stores[i] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
            .path = m_args.GetDataDirBase() / fs::PathFromString(
                        "flowmesh_runtime_round_split_" +
                        std::to_string(i)),
            .cache_bytes = size_t{1} << 20,
            .wipe_data = true});
        BOOST_REQUIRE_MESSAGE(stores[i]->OpenForMarket(
                                  domain, market, seats.seats, initial.Root(),
                                  error),
                              error);
        node::FlowMeshRuntimeConfig config;
        config.chain = &chain;
        config.keys = &keys[i];
        config.clock = &clocks[i];
        config.round_timeout = std::chrono::seconds{1};
        config.relay = [&network, i](node::FlowMeshRuntimeRelay relay) {
            network.Relay(i, std::move(relay));
            return LegacyRelayResult();
        };
        runtimes[i] = std::make_unique<node::FlowMeshRuntime>(
            std::move(config),
            std::vector<node::FlowMeshRuntimeMarketConfig>{MarketConfig(
                domain, market, treasury, seats.seats, initial, *stores[i],
                &deposits)});
        network.Set(i, runtimes[i].get(), true);
        BOOST_REQUIRE_MESSAGE(runtimes[i]->Start(error), error);
    }

    // A/B now time out before C/D, recreating the adjacent 2+2 split that
    // exact receiver-round admission could never heal. Preserve this adjacent
    // timer nudge while the independent-round test covers larger differences.
    clocks[0].m_now += std::chrono::seconds{2};
    clocks[1].m_now += std::chrono::seconds{2};
    runtimes[0]->NotifyTick();
    BOOST_REQUIRE(runtimes[0]->WaitForIdle(std::chrono::seconds{2}));
    BOOST_REQUIRE_EQUAL(runtimes[0]->MarketStatus(market)->round, 1U);
    runtimes[1]->NotifyTick(); // Sequence-zero round-one proposer is B.

    BOOST_REQUIRE(WaitUntil([&] {
        for (const auto& runtime : runtimes) {
            const auto status{runtime->MarketStatus(market)};
            if (!status || status->next_sequence != 1 ||
                status->halt != node::FlowMeshRuntimeHalt::NONE) {
                return false;
            }
        }
        return true;
    }));

    std::optional<uint256> expected_lock;
    for (size_t i{0}; i < stores.size(); ++i) {
        std::optional<uint256> locked_hash;
        BOOST_REQUIRE(stores[i]->ReadLock(
            flowmesh::ProductionSignPosition{seats.seats.epoch, 0},
            locked_hash, error));
        BOOST_REQUIRE(locked_hash.has_value());
        if (!expected_lock) {
            expected_lock = locked_hash;
        } else {
            BOOST_CHECK(*locked_hash == *expected_lock);
        }
    }

    for (auto& runtime : runtimes) runtime->Stop();
}

BOOST_AUTO_TEST_CASE(baseline_same_deposit_adjacent_anchors_split_nine_seat_quorum)
{
    // Baseline reproducer, not a recovery requirement: this test deliberately
    // asserts the existing liveness failure while preserving signing safety.
    // Three operators own three seats each. Two isolated operators permanently
    // lock the same deposit under consecutive canonical production anchors;
    // even all three remaining votes cannot bring either initial group to 7/9.
    const uint256 domain{Filled(0x2d)};
    const modern::AssetId asset{Filled(0x4d)};
    const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const auto vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x6d)};
    const SeatFixture seats{Seats(domain, market, 9, 7, 100, Filled(0x71), 133)};
    BOOST_REQUIRE_EQUAL(flowmesh::FlowMeshBlsThreshold(seats.seats.Size()), 7U);
    const std::array<flowmesh::AnchorRef, 2> anchors{{
        {200, Filled(0x74)}, {201, Filled(0x75)}}};
    const COutPoint outpoint{Txid::FromUint256(Filled(0x8d)), 0};
    const flowmesh::Action action{Deposit(outpoint)};

    // The deposit is already confirmed at both anchors. Neither missing
    // evidence nor different deposit facts can account for the split.
    class AdjacentAnchorDeposits final : public flowmesh::DepositVerifier {
    public:
        std::optional<flowmesh::DepositInfo> GetDeposit(
            const COutPoint& outpoint, const flowmesh::AnchorRef& anchor) const override
        {
            if (const auto earlier{left.GetDeposit(outpoint, anchor)}) return earlier;
            return right.GetDeposit(outpoint, anchor);
        }
        std::optional<std::vector<flowmesh::WithdrawalSettlementFactV1>>
        GetWithdrawalSettlements(const std::optional<flowmesh::AnchorRef>&,
                                 const flowmesh::AnchorRef&) const override
        {
            return std::vector<flowmesh::WithdrawalSettlementFactV1>{};
        }
        MapDeposits left;
        MapDeposits right;
    } deposits;
    deposits.left.required_anchor = anchors[0];
    deposits.right.required_anchor = anchors[1];
    for (auto* facts : {&deposits.left, &deposits.right}) {
        facts->entries.emplace(outpoint, flowmesh::DepositInfo{asset, 250, Filled(0xad)});
    }
    const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(),
                                        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    std::array<RuntimeChain, 3> chains;
    std::array<FixedClock, 3> clocks;
    std::array<RuntimeKeys, 3> keys;
    std::array<std::unique_ptr<node::FlowMeshProductionStore>, 3> stores;
    RuntimeNetwork network;
    std::array<std::unique_ptr<node::FlowMeshRuntime>, 3> runtimes;
    struct StopRuntimes {
        decltype(runtimes)& all;
        ~StopRuntimes() { for (const auto& runtime : all) if (runtime) runtime->Stop(); }
    } stop{runtimes};
    std::string error;
    for (size_t i{0}; i < runtimes.size(); ++i) {
        chains[i].m_domain = domain;
        chains[i].Add(seats.seats);
        for (const auto& anchor : anchors) {
            chains[i].AddCanonical(anchor);
        }
        chains[i].m_current = anchors[i == 0 ? 0 : 1];
        chains[i].SetTipHeight(chains[i].m_current.height + flowmesh::FLOWMESH_PRODUCTION_MIN_ANCHOR_DEPTH);
        BOOST_REQUIRE(chains[i].Acceptable(chains[i].Current()));
        for (size_t seat{i * 3}; seat < (i + 1) * 3; ++seat) {
            keys[i].m_keys[market].push_back(seats.secrets[seat]);
        }
        stores[i] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
            .path = m_args.GetDataDirBase() / fs::PathFromString(
                "flowmesh_adjacent_anchor_split_" + std::to_string(i)),
            .cache_bytes = size_t{1} << 20, .wipe_data = true});
        BOOST_REQUIRE_MESSAGE(stores[i]->OpenForMarket(
            domain, market, seats.seats, initial.Root(), error), error);
        node::FlowMeshRuntimeConfig config;
        config.chain = &chains[i];
        config.keys = &keys[i];
        config.clock = &clocks[i];
        config.round_timeout = std::chrono::seconds{1};
        config.relay = [&network, i](node::FlowMeshRuntimeRelay relay) {
            network.Relay(i, std::move(relay));
            return LegacyRelayResult();
        };
        runtimes[i] = std::make_unique<node::FlowMeshRuntime>(config,
            std::vector<node::FlowMeshRuntimeMarketConfig>{MarketConfig(
                domain, market, treasury, seats.seats, initial, *stores[i], &deposits)});
        network.Set(i, runtimes[i].get(), true);
        BOOST_REQUIRE_MESSAGE(runtimes[i]->Start(error), error);
    }
    const auto drain = [&] {
        for (size_t pass{0}; pass < runtimes.size(); ++pass) {
            for (const auto& runtime : runtimes) {
                BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
            }
        }
    };
    runtimes[0]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        return std::all_of(runtimes.begin(), runtimes.end(), [&](const auto& runtime) {
            return runtime->MarketStatus(market)->next_sequence == 1;
        });
    }));
    drain();
    const auto certified_head{runtimes[0]->MarketStatus(market)->last_microblock_hash};
    network.SetFilter([](size_t, size_t, const auto&) { return false; });
    for (const auto& runtime : runtimes) {
        BOOST_REQUIRE(runtime->SubmitLocalAction(market, action) == flowmesh::QueueResult::ACCEPTED);
    }
    drain();
    for (const auto& runtime : runtimes) {
        BOOST_REQUIRE_EQUAL(runtime->MarketStatus(market)->pending_actions, 1U);
    }

    runtimes[0]->NotifyTick(); // Sequence 1, round 0: seat 1 locks anchor 200.
    drain();
    for (size_t round{0}; round < 2; ++round) {
        clocks[1].m_now += std::chrono::seconds{1};
        runtimes[1]->NotifyTick(); // Round 2: seat 3 locks anchor 201.
        drain();
    }
    const flowmesh::ProductionSignPosition position{seats.seats.epoch, 1};
    std::array<node::StoredLockedProductionCandidate, 2> candidates;
    std::array<uint256, 2> locked_hashes;
    for (size_t i{0}; i < candidates.size(); ++i) {
        std::optional<node::StoredLockedProductionCandidate> retained;
        BOOST_REQUIRE_MESSAGE(stores[i]->ReadLockedCandidate(position, retained, error), error);
        BOOST_REQUIRE(retained);
        candidates[i] = *retained;
        locked_hashes[i] = retained->entry.GetHash();
        BOOST_CHECK(retained->entry.anchor == anchors[i]);
        BOOST_REQUIRE_EQUAL(retained->entry.actions.size(), 1U);
        BOOST_CHECK(retained->entry.actions.front().outpoint == outpoint);
        const auto data{runtimes[i]->MarketData(market, std::nullopt, {}, error)};
        BOOST_REQUIRE_MESSAGE(data, error);
        BOOST_CHECK_EQUAL(data->snapshot.runtime.max_verified_attestations, 3U);
        BOOST_CHECK_EQUAL(data->snapshot.quorum_required, 7U);
        BOOST_CHECK(data->snapshot.runtime.local_locked_candidate == locked_hashes[i]);
    }
    BOOST_CHECK(locked_hashes[0] != locked_hashes[1]);
    auto normalized{candidates[1].entry};
    normalized.anchor = candidates[0].entry.anchor;
    const auto left_bytes{flowmesh::EncodeProductionEntry(candidates[0].entry)};
    const auto normalized_bytes{flowmesh::EncodeProductionEntry(normalized)};
    BOOST_REQUIRE(left_bytes);
    BOOST_REQUIRE(normalized_bytes);
    BOOST_CHECK(*left_bytes == *normalized_bytes); // Only the production anchor differs.
    std::optional<uint256> remaining_lock;
    BOOST_REQUIRE(stores[2]->ReadLock(position, remaining_lock, error));
    BOOST_REQUIRE(!remaining_lock);

    // Advance A's mock canonical tip by one block before reconnecting. Both
    // anchors are now deep, but the newer Current() must not replace its lock.
    drain();
    chains[0].SetTipHeight(anchors[1].height + flowmesh::FLOWMESH_PRODUCTION_MIN_ANCHOR_DEPTH);
    chains[0].m_current = anchors[1];
    for (const auto& chain : chains) {
        for (const auto& anchor : anchors) BOOST_REQUIRE(chain.Acceptable(anchor));
    }

    // Release A to the three uncommitted seats first, deterministically. This
    // gives A every remaining vote (6), while B retains its three honest votes.
    network.SetFilter([](size_t from, size_t to, const auto&) { return from != 1 && to != 1; });
    for (auto& clock : clocks) clock.m_now += node::FlowMeshCommitteeRelayBudget::REPEAT_DELAY;
    runtimes[0]->NotifyTick(); // Round 1: seat 2 retries the exact retained A.
    BOOST_REQUIRE(WaitUntil([&] {
        for (const size_t i : {size_t{0}, size_t{2}}) {
            const auto data{runtimes[i]->MarketData(market, std::nullopt, {}, error)};
            if (!data || data->snapshot.runtime.max_verified_attestations != 6) return false;
        }
        return true;
    }));
    drain();
    BOOST_REQUIRE(stores[2]->ReadLock(position, remaining_lock, error));
    BOOST_REQUIRE(remaining_lock);
    BOOST_CHECK(*remaining_lock == locked_hashes[0]);

    // Heal every link, then exercise a complete proposer rotation and paced
    // retries. A future recovery test must require progress; this baseline
    // instead proves both alternatives arrived and the durable split persists.
    network.SetFilter({});
    for (size_t round{0}; round < seats.seats.Size(); ++round) {
        for (auto& clock : clocks) clock.m_now += node::FlowMeshCommitteeRelayBudget::REPEAT_DELAY;
        for (const auto& runtime : runtimes) {
            runtime->NotifyTick();
            drain();
        }
    }
    for (size_t i{0}; i < runtimes.size(); ++i) {
        std::optional<uint256> locked;
        BOOST_REQUIRE(stores[i]->ReadLock(position, locked, error));
        BOOST_REQUIRE(locked);
        BOOST_CHECK(*locked == locked_hashes[i == 1 ? 1 : 0]);
        const auto status{runtimes[i]->MarketStatus(market)};
        BOOST_REQUIRE(status);
        BOOST_CHECK_EQUAL(status->next_sequence, 1U);
        BOOST_CHECK(status->last_microblock_hash == certified_head);
        BOOST_CHECK(status->halt == node::FlowMeshRuntimeHalt::NONE);
        const auto data{runtimes[i]->MarketData(market, std::nullopt, {}, error)};
        BOOST_REQUIRE_MESSAGE(data, error);
        BOOST_CHECK_GT(data->snapshot.runtime.proposals_conflicting_lock, 0U);
        BOOST_CHECK_EQUAL(data->snapshot.runtime.max_verified_attestations, i == 1 ? 3U : 6U);
        BOOST_CHECK_EQUAL(data->snapshot.quorum_required, 7U);
        BOOST_CHECK_LT(data->snapshot.runtime.max_verified_attestations, data->snapshot.quorum_required);
    }
}

BOOST_AUTO_TEST_CASE(honest_divergent_round_candidates_preserve_permanent_locks_without_remote_halt)
{
    const uint256 domain{Filled(0x1b)};
    const modern::AssetId asset{Filled(0x3b)};
    const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const auto vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x5b)};
    const SeatFixture seats{Seats(domain, market, 4, 7, 100, Filled(0x71), 123)};
    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(seats.seats);
    MapDeposits deposits;
    deposits.required_anchor = chain.m_current;
    const COutPoint left_outpoint{Txid::FromUint256(Filled(0x8b)), 0};
    const COutPoint right_outpoint{Txid::FromUint256(Filled(0x8c)), 0};
    deposits.entries.emplace(left_outpoint, flowmesh::DepositInfo{asset, 250, Filled(0x6b)});
    deposits.entries.emplace(right_outpoint, flowmesh::DepositInfo{asset, 125, Filled(0x6c)});
    const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(),
                                        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    std::array<FixedClock, 4> clocks;
    std::array<RuntimeKeys, 4> keys;
    std::array<std::unique_ptr<node::FlowMeshProductionStore>, 4> stores;
    RuntimeNetwork network;
    std::array<std::unique_ptr<node::FlowMeshRuntime>, 4> runtimes;
    std::string error;
    for (size_t i{0}; i < runtimes.size(); ++i) {
        keys[i].m_keys[market] = {seats.secrets[i]};
        stores[i] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
            .path = m_args.GetDataDirBase() / fs::PathFromString(
                        "flowmesh_runtime_honest_lock_split_" + std::to_string(i)),
            .cache_bytes = size_t{1} << 20, .wipe_data = true});
        BOOST_REQUIRE_MESSAGE(stores[i]->OpenForMarket(
            domain, market, seats.seats, initial.Root(), error), error);
        node::FlowMeshRuntimeConfig config;
        config.chain = &chain;
        config.keys = &keys[i];
        config.clock = &clocks[i];
        config.round_timeout = std::chrono::seconds{1};
        config.relay = [&network, i](node::FlowMeshRuntimeRelay relay) {
            network.Relay(i, std::move(relay));
            return LegacyRelayResult();
        };
        runtimes[i] = std::make_unique<node::FlowMeshRuntime>(config,
            std::vector<node::FlowMeshRuntimeMarketConfig>{MarketConfig(
                domain, market, treasury, seats.seats, initial, *stores[i], &deposits)});
        network.Set(i, runtimes[i].get(), true);
        BOOST_REQUIRE_MESSAGE(runtimes[i]->Start(error), error);
    }
    runtimes[0]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        return std::all_of(runtimes.begin(), runtimes.end(), [&](const auto& runtime) {
            return runtime->MarketStatus(market)->next_sequence == 1;
        });
    }));
    for (const auto& runtime : runtimes) {
        BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
    }
    const auto certified_head{runtimes[0]->MarketStatus(market)->last_microblock_hash};

    // No equivocation, invalid anchor, or Byzantine signer: only different
    // delivered action pools and a delayed cross-half network at one position.
    network.PartitionHalves(true);
    const flowmesh::Action left_action{Deposit(left_outpoint)};
    const flowmesh::Action right_action{Deposit(right_outpoint)};
    BOOST_REQUIRE(runtimes[1]->SubmitLocalAction(market, left_action) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtimes[2]->SubmitLocalAction(market, right_action) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] {
        return std::all_of(runtimes.begin(), runtimes.end(), [&](const auto& runtime) {
            return runtime->MarketStatus(market)->pending_actions == 1;
        });
    }));
    runtimes[1]->NotifyTick(); // seq1/round0 proposer B; A/B lock X.
    BOOST_REQUIRE(WaitUntil([&] {
        const auto data{runtimes[1]->MarketData(market, std::nullopt, {}, error)};
        return data && data->snapshot.runtime.max_verified_attestations == 2;
    }));
    for (const auto& runtime : runtimes) {
        BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
    }
    clocks[2].m_now += std::chrono::seconds{2};
    clocks[3].m_now += std::chrono::seconds{2};
    runtimes[2]->NotifyTick(); // seq1/round1 proposer C; C/D lock Y.
    BOOST_REQUIRE(WaitUntil([&] {
        const auto data{runtimes[2]->MarketData(market, std::nullopt, {}, error)};
        return data && data->snapshot.runtime.max_verified_attestations == 2;
    }));
    for (const auto& runtime : runtimes) {
        BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
    }

    const flowmesh::ProductionSignPosition position{seats.seats.epoch, 1};
    std::array<uint256, 4> locked_hashes;
    for (size_t i{0}; i < runtimes.size(); ++i) {
        std::optional<uint256> locked;
        BOOST_REQUIRE(stores[i]->ReadLock(position, locked, error));
        BOOST_REQUIRE(locked);
        locked_hashes[i] = *locked;
        const auto data{runtimes[i]->MarketData(market, std::nullopt, {}, error)};
        BOOST_REQUIRE_MESSAGE(data, error);
        BOOST_CHECK_EQUAL(data->snapshot.quorum_required, 3U);
        BOOST_CHECK_EQUAL(data->snapshot.next_microblock_sequence, 1U);
        BOOST_CHECK(data->snapshot.last_microblock_hash == certified_head);
        BOOST_CHECK(data->snapshot.runtime.local_locked_candidate == locked);
        BOOST_CHECK(runtimes[i]->MarketStatus(market)->halt == node::FlowMeshRuntimeHalt::NONE);
    }
    BOOST_CHECK(locked_hashes[0] == locked_hashes[1]);
    BOOST_CHECK(locked_hashes[2] == locked_hashes[3]);
    BOOST_CHECK(locked_hashes[0] != locked_hashes[2]);
    for (const size_t proposer : {size_t{1}, size_t{2}}) {
        const auto data{runtimes[proposer]->MarketData(market, std::nullopt, {}, error)};
        BOOST_REQUIRE_MESSAGE(data, error);
        BOOST_CHECK_EQUAL(data->snapshot.runtime.max_verified_attestations, 2U);
        BOOST_CHECK_LT(data->snapshot.runtime.max_verified_attestations,
                       data->snapshot.quorum_required);
    }
    BOOST_CHECK_EQUAL(runtimes[1]->MarketStatus(market)->round, 0U);
    BOOST_CHECK_EQUAL(runtimes[2]->MarketStatus(market)->round, 1U);

    // Deliver the delayed credentials first. Receiving the other action does
    // not replace a locked candidate. All nodes can now execute either body.
    network.PartitionHalves(false);
    for (size_t i{0}; i < runtimes.size(); ++i) {
        flowmesh::WireMessage delayed;
        delayed.kind = flowmesh::WireMessageKind::ACTION;
        delayed.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1, market, seats.seats.epoch, 1};
        const auto payload{flowmesh::EncodeProductionActionPayload(
            i < 2 ? right_action : left_action)};
        BOOST_REQUIRE(payload);
        delayed.payload = *payload;
        BOOST_REQUIRE(runtimes[i]->EnqueueWireMessage(i < 2 ? 2 : 1, std::move(delayed)) ==
                      flowmesh::QueueResult::ACCEPTED);
    }
    BOOST_REQUIRE(WaitUntil([&] {
        return std::all_of(runtimes.begin(), runtimes.end(), [&](const auto& runtime) {
            return runtime->MarketStatus(market)->pending_actions == 2;
        });
    }));
    // C's authentic proposal cannot replace the durable X locks at A/B.
    // Ignore the competing body without letting a remote proposal halt the
    // worker; no round change can turn either two-vote candidate into quorum.
    for (const auto& runtime : runtimes) BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
    for (auto& clock : clocks) clock.m_now += node::FlowMeshCommitteeRelayBudget::REPEAT_DELAY;
    runtimes[2]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        const auto a{runtimes[0]->MarketData(market, std::nullopt, {}, error)};
        const auto b{runtimes[1]->MarketData(market, std::nullopt, {}, error)};
        return a && b && a->snapshot.runtime.proposals_conflicting_lock > 0 &&
               b->snapshot.runtime.proposals_conflicting_lock > 0;
    }));
    runtimes[1]->NotifyTick(); // The exact retained X candidate can still be retried.
    BOOST_REQUIRE(WaitUntil([&] {
        const auto c{runtimes[2]->MarketData(market, std::nullopt, {}, error)};
        const auto d{runtimes[3]->MarketData(market, std::nullopt, {}, error)};
        return c && d && c->snapshot.runtime.proposals_conflicting_lock > 0 &&
               d->snapshot.runtime.proposals_conflicting_lock > 0;
    }));
    for (size_t i{0}; i < runtimes.size(); ++i) {
        BOOST_REQUIRE(runtimes[i]->WaitForIdle(std::chrono::seconds{2}));
        std::optional<uint256> locked;
        BOOST_REQUIRE(stores[i]->ReadLock(position, locked, error));
        BOOST_REQUIRE(locked);
        BOOST_CHECK(*locked == locked_hashes[i]);
        const auto status{runtimes[i]->MarketStatus(market)};
        BOOST_CHECK_EQUAL(status->next_sequence, 1U);
        BOOST_CHECK(status->last_microblock_hash == certified_head);
        BOOST_CHECK(status->halt == node::FlowMeshRuntimeHalt::NONE);
        const auto data{runtimes[i]->MarketData(market, std::nullopt, {}, error)};
        BOOST_REQUIRE(data);
        BOOST_CHECK_EQUAL(data->snapshot.runtime.max_verified_attestations, 2U);
        BOOST_CHECK_EQUAL(data->snapshot.quorum_required, 3U);
    }
    for (auto& runtime : runtimes) runtime->Stop();
}

BOOST_AUTO_TEST_CASE(incoming_committee_waits_for_mature_handoff_publication)
{
    const uint256 domain{Filled(0x12)};
    const modern::AssetId asset{Filled(0x33)};
    const flowmesh::MarketId market{
        *flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const flowmesh::VaultId vault{
        *flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x52)};
    const SeatFixture outgoing{
        Seats(domain, market, 4, 7, 100, Filled(0x71), 3)};
    const SeatFixture incoming{
        Seats(domain, market, 5, 8, 220, Filled(0x72), 43)};
    flowmesh::FlowMeshState state{
        vault, asset, modern::NativeAsset(),
        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};

    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(outgoing.seats);
    FixedClock clock;
    RuntimeKeys keys;
    keys.m_keys[market] = outgoing.secrets;
    keys.m_keys[market].insert(keys.m_keys[market].end(),
                               incoming.secrets.begin(),
                               incoming.secrets.end());
    node::FlowMeshProductionStore store{DBParams{
        .path = m_args.GetDataDirBase() /
                "flowmesh_runtime_handoff_maturity",
        .cache_bytes = size_t{1} << 20,
        .wipe_data = true}};
    std::string error;
    BOOST_REQUIRE_MESSAGE(store.OpenForMarket(
                              domain, market, outgoing.seats, state.Root(),
                              error),
                          error);

    node::FlowMeshRuntimeConfig config;
    config.chain = &chain;
    config.keys = &keys;
    config.clock = &clock;
    config.relay = [](node::FlowMeshRuntimeRelay) { return LegacyRelayResult(); };
    config.round_timeout = std::chrono::hours{1};
    node::FlowMeshRuntime runtime{
        std::move(config),
        {MarketConfig(domain, market, treasury, outgoing.seats, state, store,
                      nullptr)}};
    BOOST_REQUIRE_MESSAGE(runtime.Start(error), error);

    // Establish and publish sequence-zero genesis before the membership
    // change, matching the production service's ordering rule.
    runtime.NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        const auto status{runtime.MarketStatus(market)};
        return status && status->next_sequence == 1;
    }));
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    std::optional<node::StoredProductionEntry> genesis;
    BOOST_REQUIRE(store.ReadEntry(0, outgoing.seats, genesis, error));
    BOOST_REQUIRE(genesis.has_value());
    const auto genesis_checkpoint{flowmesh::BuildProductionCheckpointRecord(
        genesis->entry, genesis->certificate, outgoing.seats, {})};
    BOOST_REQUIRE(genesis_checkpoint.has_value());
    const auto genesis_id{
        modern::FlowMeshCheckpointIdV1(genesis_checkpoint->core)};
    BOOST_REQUIRE(genesis_id.has_value());
    BOOST_REQUIRE(store.MarkExecutionCheckpointConnected(
        *genesis_checkpoint, outgoing.seats, {230, Filled(0xc0)}, error));

    chain.Add(incoming.seats);
    chain.AddCanonical({220, Filled(0x72)});
    chain.SetHandoff(market, incoming.seats);
    runtime.NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        const auto status{runtime.MarketStatus(market)};
        return status && status->next_sequence == 2 &&
               status->pending_handoff && status->epoch == outgoing.seats.epoch;
    }));
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));

    std::optional<node::StoredProductionEntry> handoff;
    BOOST_REQUIRE(store.ReadEntry(1, outgoing.seats, handoff, error));
    BOOST_REQUIRE(handoff.has_value());
    BOOST_REQUIRE(handoff->entry.kind == static_cast<uint8_t>(
        flowmesh::ProductionEntryKind::EPOCH_HANDOFF));
    const auto handoff_checkpoint{flowmesh::BuildProductionCheckpointRecord(
        handoff->entry, handoff->certificate, outgoing.seats, *genesis_id)};
    BOOST_REQUIRE(handoff_checkpoint.has_value());
    const uint256 handoff_hash{handoff->entry.GetHash()};

    // All incoming keys are armed, and work may queue, but a first-connect
    // publication remains owned by the outgoing marker and creates no signing
    // lock for the incoming epoch.
    BOOST_REQUIRE(runtime.SubmitLocalAction(
                      market,
                      Deposit(COutPoint{Txid::FromUint256(Filled(0x91)), 0})) ==
                  flowmesh::QueueResult::ACCEPTED);
    const node::ProductionB3Connection shallow{261, Filled(0xc1)};
    chain.SetTipHeight(261);
    chain.ConnectHandoff(
        handoff_hash,
        node::FlowMeshRuntimeConnectedHandoff{handoff_checkpoint->core,
                                              shallow});
    runtime.NotifyTick();
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    auto status{runtime.MarketStatus(market)};
    BOOST_REQUIRE(status.has_value());
    BOOST_CHECK(status->pending_handoff);
    BOOST_CHECK_EQUAL(status->epoch, outgoing.seats.epoch);
    BOOST_CHECK_EQUAL(status->next_sequence, 2U);
    std::optional<uint256> incoming_lock;
    BOOST_REQUIRE(store.ReadLock({incoming.seats.epoch, 2}, incoming_lock,
                                 error));
    BOOST_CHECK(!incoming_lock.has_value());

    std::vector<int32_t> connected_heights;
    BOOST_REQUIRE(store.ConnectedB3Heights(connected_heights, error));
    BOOST_REQUIRE_EQUAL(connected_heights.size(), 1U);
    BOOST_CHECK_EQUAL(connected_heights.front(), 230);

    // A shallow reorg merely removes an observation the store never adopted.
    // The same certified handoff can be republished and is still held pending
    // through depth 29.
    chain.DisconnectHandoff(handoff_hash, shallow.height, Filled(0xcf));
    runtime.NotifyTick();
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    const node::ProductionB3Connection republished{262, Filled(0xc2)};
    chain.SetTipHeight(291);
    chain.ConnectHandoff(
        handoff_hash,
        node::FlowMeshRuntimeConnectedHandoff{handoff_checkpoint->core,
                                              republished});
    runtime.NotifyTick();
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    status = runtime.MarketStatus(market);
    BOOST_REQUIRE(status.has_value());
    BOOST_CHECK(status->pending_handoff);
    BOOST_CHECK_EQUAL(status->epoch, outgoing.seats.epoch);
    BOOST_REQUIRE(store.ReadLock({incoming.seats.epoch, 2}, incoming_lock,
                                 error));
    BOOST_CHECK(!incoming_lock.has_value());

    // At exactly 30 blocks, the one atomic store transition activates the
    // incoming set. No incoming signing was possible before this boundary.
    chain.SetTransition(market,
                        node::FlowMeshSeatTransitionKind::CONTINUE);
    chain.SetTipHeight(292);
    runtime.NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        const auto current{runtime.MarketStatus(market)};
        return current && current->epoch == incoming.seats.epoch &&
               !current->pending_handoff;
    }));
    BOOST_REQUIRE(store.ConnectedB3Heights(connected_heights, error));
    BOOST_REQUIRE_EQUAL(connected_heights.size(), 2U);
    BOOST_CHECK_EQUAL(connected_heights.back(), republished.height);

    runtime.Stop();
}

BOOST_AUTO_TEST_CASE(catchup_tail_slack_requires_full_application_and_count_byte_room)
{
    constexpr size_t framed_maximum{4 + flowmesh::FLOWMESH_CERTIFICATE_MAX_BYTES};
    constexpr size_t response_bytes{1000};
    const auto tail = [&](const size_t requested_count, const size_t budget,
                          const size_t returned_count, const size_t applied_count) {
        return node::FlowMeshCatchupPageHasTailSlack(
            requested_count, budget, returned_count, response_bytes, applied_count);
    };
    BOOST_CHECK(!tail(64, response_bytes + framed_maximum - 1, 1, 1));
    BOOST_CHECK(tail(64, response_bytes + framed_maximum, 1, 1));
    BOOST_CHECK(tail(64, response_bytes + framed_maximum + 1, 1, 1));
    BOOST_CHECK(!tail(1, flowmesh::FLOWMESH_CATCHUP_MAX_BYTES, 1, 1)); // count limited
    BOOST_CHECK(!tail(64, flowmesh::FLOWMESH_CATCHUP_MAX_BYTES, 64, 64));
    BOOST_CHECK(!tail(64, flowmesh::FLOWMESH_CATCHUP_MAX_BYTES, 2, 1)); // invalid tail
    BOOST_CHECK(!tail(64, flowmesh::FLOWMESH_CATCHUP_MAX_BYTES, 1, 0));
    BOOST_CHECK(!tail(64, flowmesh::FLOWMESH_CATCHUP_MAX_BYTES, 0, 0));
    BOOST_CHECK(!tail(64, response_bytes - 1, 1, 1)); // no subtraction underflow
    BOOST_CHECK(!tail(0, flowmesh::FLOWMESH_CATCHUP_MAX_BYTES, 1, 1));
}

BOOST_AUTO_TEST_CASE(idle_legacy_discovery_stops_small_pages_and_fresh_ahead_recovers)
{
    const uint256 domain{Filled(0x19)};
    const modern::AssetId asset{Filled(0x39)};
    const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const auto vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x59)};
    const SeatFixture seats{Seats(domain, market, 4, 7, 100, Filled(0x71), 119)};
    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(seats.seats);
    FixedClock clock;
    MapDeposits deposits;
    deposits.required_anchor = chain.m_current;
    const COutPoint outpoint{Txid::FromUint256(Filled(0x89)), 0};
    deposits.entries.emplace(outpoint, flowmesh::DepositInfo{asset, 250, Filled(0x69)});
    const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(),
                                        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    std::array<RuntimeKeys, 2> keys;
    keys[0].m_keys[market] = seats.secrets;
    std::array<std::unique_ptr<node::FlowMeshProductionStore>, 2> stores;
    RuntimeNetwork network;
    std::mutex pages_mutex;
    std::vector<flowmesh::WireMessage> captured_pages;
    std::array<std::unique_ptr<node::FlowMeshRuntime>, 2> runtimes;
    // All remote peers behave like the old version: they never process or
    // advertise fmhello, but their existing certified-history handler works.
    network.IgnoreHellos();
    std::string error;
    for (size_t i{0}; i < 2; ++i) {
        stores[i] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
            .path = m_args.GetDataDirBase() / fs::PathFromString(
                        "flowmesh_idle_discovery_" + std::to_string(i)),
            .cache_bytes = size_t{1} << 20, .wipe_data = true});
        BOOST_REQUIRE_MESSAGE(stores[i]->OpenForMarket(
            domain, market, seats.seats, initial.Root(), error), error);
        node::FlowMeshRuntimeConfig config;
        config.chain = &chain;
        config.keys = &keys[i];
        config.clock = &clock;
        config.round_timeout = std::chrono::hours{1};
        config.relay = [&network, i](node::FlowMeshRuntimeRelay relay) {
            network.Relay(i, std::move(relay));
            return LegacyRelayResult();
        };
        std::vector<node::FlowMeshRuntimeMarketConfig> markets;
        if (i == 0) markets.push_back(MarketConfig(
            domain, market, treasury, seats.seats, initial, *stores[i], &deposits));
        runtimes[i] = std::make_unique<node::FlowMeshRuntime>(config, markets);
        network.Set(i, runtimes[i].get(), i == 0);
        BOOST_REQUIRE_MESSAGE(runtimes[i]->Start(error), error);
    }
    runtimes[0]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        return runtimes[0]->MarketStatus(market)->next_sequence == 1;
    }));
    BOOST_REQUIRE(runtimes[0]->SubmitLocalAction(market, Deposit(outpoint)) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtimes[0]->WaitForIdle(std::chrono::seconds{2}));
    runtimes[0]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        return runtimes[0]->MarketStatus(market)->next_sequence == 2;
    }));
    BOOST_REQUIRE(runtimes[0]->WaitForIdle(std::chrono::seconds{2}));
    const auto certified_tip{runtimes[0]->MarketStatus(market)->last_microblock_hash};

    // No more orders, deposits, B3 blocks, proposals or new certificates.
    // The observer discovers its market after the producer has become idle.
    clock.m_now += std::chrono::seconds{6};
    network.Set(1, runtimes[1].get(), true);
    runtimes[1]->FlowMeshPeerConnected(0);
    BOOST_REQUIRE(runtimes[1]->WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(network.GetRequests(1), 0U); // no market registered yet
    BOOST_REQUIRE_MESSAGE(runtimes[1]->AddMarket(MarketConfig(
        domain, market, treasury, seats.seats, initial, *stores[1], &deposits), error), error);
    BOOST_REQUIRE(WaitUntil([&] {
        const auto status{runtimes[1]->MarketStatus(market)};
        return status && status->next_sequence == 2 &&
               status->last_microblock_hash == certified_tip;
    }));
    BOOST_CHECK(runtimes[1]->MarketStatus(market)->observer_only);
    BOOST_CHECK_EQUAL(runtimes[0]->MarketStatus(market)->next_sequence, 2U);
    BOOST_CHECK(runtimes[1]->StateSnapshot(market)->Root() ==
                runtimes[0]->StateSnapshot(market)->Root());
    BOOST_REQUIRE(runtimes[0]->WaitForIdle(std::chrono::seconds{2}));
    BOOST_REQUIRE(runtimes[1]->WaitForIdle(std::chrono::seconds{2}));
    const auto requests_after_sync{network.GetRequests(1)};
    BOOST_CHECK_EQUAL(requests_after_sync, 1U); // small verified page; no speculative tip probe
    const auto data{runtimes[1]->MarketData(market, std::nullopt, {}, error)};
    BOOST_REQUIRE_MESSAGE(data, error);
    BOOST_CHECK(data->snapshot.certified);
    BOOST_CHECK(data->snapshot.last_microblock_hash == certified_tip);
    BOOST_CHECK(data->snapshot.state_root == runtimes[1]->MarketStatus(market)->state_root);
    flowmesh::MarketDataQuery query;
    query.known_head = certified_tip;
    const auto unchanged{runtimes[1]->MarketData(market, std::nullopt, query, error)};
    BOOST_REQUIRE_MESSAGE(unchanged, error);
    BOOST_CHECK(unchanged->unchanged);
    BOOST_CHECK(unchanged->history.entries.empty());
    BOOST_CHECK(unchanged->liquidity.curves.empty());
    BOOST_CHECK(unchanged->snapshot.last_microblock_hash == certified_tip);
    query = {};
    query.expected_head = Filled(0xfa);
    BOOST_CHECK(!runtimes[1]->MarketData(market, std::nullopt, query, error));
    query = {};
    query.limit = 101;
    BOOST_CHECK(!runtimes[1]->MarketData(market, std::nullopt, query, error));

    // Omitting the unnecessary tip probe must not restart blind polling.
    clock.m_now += std::chrono::seconds{30};
    runtimes[1]->NotifyTick();
    BOOST_REQUIRE(runtimes[1]->WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(network.GetRequests(1), requests_after_sync);

    // The observer misses one more certificate while disconnected. A fresh
    // connection scan recovers it while the producer is idle again.
    network.Set(1, runtimes[1].get(), false);
    runtimes[1]->FlowMeshPeerDisconnected(0);
    BOOST_REQUIRE(runtimes[1]->WaitForIdle(std::chrono::seconds{2}));
    const COutPoint second_outpoint{Txid::FromUint256(Filled(0x8a)), 1};
    deposits.entries.emplace(second_outpoint,
                             flowmesh::DepositInfo{asset, 125, Filled(0x69)});
    BOOST_REQUIRE(runtimes[0]->SubmitLocalAction(market, Deposit(second_outpoint)) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtimes[0]->WaitForIdle(std::chrono::seconds{2}));
    runtimes[0]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        return runtimes[0]->MarketStatus(market)->next_sequence == 3;
    }));
    BOOST_REQUIRE(runtimes[0]->WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(runtimes[1]->MarketStatus(market)->next_sequence, 2U);
    network.Set(1, runtimes[1].get(), true);
    runtimes[1]->FlowMeshPeerConnected(0);
    BOOST_REQUIRE(WaitUntil([&] {
        return runtimes[1]->MarketStatus(market)->next_sequence == 3;
    }));
    BOOST_CHECK(runtimes[1]->MarketStatus(market)->last_microblock_hash ==
                runtimes[0]->MarketStatus(market)->last_microblock_hash);
    BOOST_REQUIRE(runtimes[0]->WaitForIdle(std::chrono::seconds{2}));
    BOOST_REQUIRE(runtimes[1]->WaitForIdle(std::chrono::seconds{2}));
    const auto requests_after_reconnect{network.GetRequests(1)};
    BOOST_CHECK_EQUAL(requests_after_reconnect, requests_after_sync + 1);

    // Miss two certificates without a reconnect or any clock advancement.
    // A cryptographically verified future certificate must immediately earn
    // catch-up: the prior small page installed no unanswered-tip cooldown.
    network.Set(1, runtimes[1].get(), false);
    for (size_t i{0}; i < 2; ++i) {
        const COutPoint next_outpoint{Txid::FromUint256(Filled(0x8b + i)), 0};
        deposits.entries.emplace(next_outpoint,
                                 flowmesh::DepositInfo{asset, 1, Filled(0x69)});
        BOOST_REQUIRE(runtimes[0]->SubmitLocalAction(market, Deposit(next_outpoint)) ==
                      flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(runtimes[0]->WaitForIdle(std::chrono::seconds{2}));
        runtimes[0]->NotifyTick();
        BOOST_REQUIRE(WaitUntil([&] {
            return runtimes[0]->MarketStatus(market)->next_sequence == 4 + i;
        }));
        BOOST_REQUIRE(runtimes[0]->WaitForIdle(std::chrono::seconds{2}));
    }
    BOOST_CHECK_EQUAL(runtimes[1]->MarketStatus(market)->next_sequence, 3U);
    std::optional<node::StoredProductionEntry> future;
    BOOST_REQUIRE(stores[0]->ReadEntry(4, seats.seats, future, error));
    BOOST_REQUIRE(future);
    flowmesh::WireMessage future_certificate{flowmesh::WireMessageKind::CERTIFICATE,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, market, seats.seats.epoch, 4},
        *flowmesh::EncodeProductionCertifiedPayload({future->entry, future->certificate}, seats.seats.Size())};
    network.Set(1, runtimes[1].get(), true);
    BOOST_REQUIRE(runtimes[1]->EnqueueWireMessage(0, future_certificate) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] { return runtimes[1]->MarketStatus(market)->next_sequence == 5; }));
    BOOST_REQUIRE(runtimes[0]->WaitForIdle(std::chrono::seconds{2}));
    BOOST_REQUIRE(runtimes[1]->WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(network.GetRequests(1), requests_after_reconnect + 1);
    BOOST_CHECK(runtimes[1]->StateSnapshot(market)->Root() == runtimes[0]->StateSnapshot(market)->Root());

    // Exercise the real greedy sender, not a transport that silently truncates
    // a tiny response to a 4 MiB request. These replies have no pending request
    // at the observer, so they cannot change its state.
    network.SetFilter([&](size_t from, size_t to, const flowmesh::WireMessage& message) {
        if (from == 0 && to == 1 && message.kind == flowmesh::WireMessageKind::ENTRIES) {
            std::lock_guard<std::mutex> lock{pages_mutex};
            captured_pages.push_back(message);
        }
        return true;
    });
    std::optional<node::StoredProductionEntry> first_entry;
    BOOST_REQUIRE(stores[0]->ReadEntry(0, seats.seats, first_entry, error));
    BOOST_REQUIRE(first_entry);
    const auto first_payload{flowmesh::EncodeProductionCertifiedPayload(
        {first_entry->entry, first_entry->certificate}, seats.seats.Size())};
    BOOST_REQUIRE(first_payload);
    const uint32_t one_entry_bytes{static_cast<uint32_t>(2 + 4 + first_payload->size())};
    for (const auto& [maximum_count, maximum_bytes] :
         std::vector<std::pair<uint16_t, uint32_t>>{{1, flowmesh::FLOWMESH_CATCHUP_MAX_BYTES},
                                                   {64, one_entry_bytes}}) {
        flowmesh::WireMessage request{flowmesh::WireMessageKind::GET,
            {flowmesh::FLOWMESH_WIRE_VERSION_V1, market, seats.seats.epoch, 0},
            *flowmesh::EncodeCatchupRequest(maximum_count, maximum_bytes)};
        BOOST_REQUIRE(runtimes[0]->EnqueueWireMessage(1, request) == flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(runtimes[0]->WaitForIdle(std::chrono::seconds{2}));
        BOOST_REQUIRE(runtimes[1]->WaitForIdle(std::chrono::seconds{2}));
        std::lock_guard<std::mutex> lock{pages_mutex};
        BOOST_REQUIRE_EQUAL(captured_pages.size(), 1U);
        const auto page{flowmesh::DecodeCatchupEntries(captured_pages.front().payload)};
        BOOST_REQUIRE(page);
        BOOST_REQUIRE_EQUAL(page->size(), 1U);
        BOOST_CHECK(page->front() == *first_payload);
        BOOST_CHECK_EQUAL(captured_pages.front().payload.size(), one_entry_bytes);
        BOOST_CHECK(!node::FlowMeshCatchupPageHasTailSlack(
            maximum_count, maximum_bytes, page->size(), captured_pages.front().payload.size(), page->size()));
        captured_pages.clear();
    }
    BOOST_CHECK_EQUAL(runtimes[1]->MarketStatus(market)->next_sequence, 5U);
    for (auto& runtime : runtimes) runtime->Stop();
}

BOOST_AUTO_TEST_CASE(legacy_discovery_retries_silent_peers_after_backoff_and_verifies_history)
{
    const uint256 domain{Filled(0x1b)};
    const modern::AssetId asset{Filled(0x3b)};
    const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const auto vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x5b)};
    const SeatFixture seats{Seats(domain, market, 4, 7, 100, Filled(0x71), 121)};
    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(seats.seats);
    RuntimeKeys keys; // Discovery cannot create a local signing seat.
    FixedClock clock;
    const auto started{clock.m_now};
    const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(),
                                        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    node::FlowMeshProductionStore store{DBParams{
        .path = m_args.GetDataDirBase() / "flowmesh_legacy_retry",
        .cache_bytes = size_t{1} << 20, .wipe_data = true}};
    std::string error;
    BOOST_REQUIRE_MESSAGE(store.OpenForMarket(domain, market, seats.seats,
                                              initial.Root(), error), error);
    std::mutex relay_mutex;
    std::vector<flowmesh::WirePeerId> requested_peers;
    node::FlowMeshRuntimeConfig config;
    config.chain = &chain;
    config.keys = &keys;
    config.clock = &clock;
    config.round_timeout = std::chrono::hours{1};
    config.relay = [&](node::FlowMeshRuntimeRelay relay) {
        // Both legacy peers ignore HELLO and initially have nothing to send,
        // as when their service is still reconciling the B3 tip.
        if (relay.message.kind != flowmesh::WireMessageKind::GET) return LegacyRelayResult();
        std::lock_guard<std::mutex> lock{relay_mutex};
        if (relay.peer) requested_peers.push_back(*relay.peer);
        return LegacyRelayResult();
    };
    node::FlowMeshRuntime runtime{config, {MarketConfig(
        domain, market, treasury, seats.seats, initial, store, nullptr)}};
    const auto requests = [&] {
        std::lock_guard<std::mutex> lock{relay_mutex};
        return requested_peers;
    };
    BOOST_REQUIRE_MESSAGE(runtime.Start(error), error);
    runtime.FlowMeshPeerConnected(10);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    runtime.FlowMeshPeerConnected(11);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_REQUIRE_EQUAL(requests().size(), 1U); // One probe per second globally.
    BOOST_CHECK_EQUAL(requests()[0], 10);
    clock.m_now = started + std::chrono::seconds{1};
    runtime.NotifyTick();
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_REQUIRE_EQUAL(requests().size(), 2U);
    BOOST_CHECK_EQUAL(requests()[1], 11); // The second peer is not starved.

    // Request deadlines and ordinary failure cooldowns expire well before
    // discovery retries; neither same-clock ticks nor reconnect notifications
    // for an existing connection refill its sweep budget.
    clock.m_now = started + std::chrono::seconds{59};
    runtime.FlowMeshPeerConnected(10);
    for (int i{0}; i < 20; ++i) runtime.NotifyTick();
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(requests().size(), 2U);
    BOOST_CHECK_EQUAL(runtime.MarketStatus(market)->next_sequence, 0U);
    clock.m_now = started + std::chrono::seconds{60};
    runtime.NotifyTick();
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_REQUIRE_EQUAL(requests().size(), 3U);
    BOOST_CHECK_EQUAL(requests()[2], 10);
    for (int i{0}; i < 20; ++i) runtime.NotifyTick();
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(requests().size(), 3U);
    clock.m_now = started + std::chrono::seconds{61};
    runtime.NotifyTick();
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_REQUIRE_EQUAL(requests().size(), 4U);
    BOOST_CHECK_EQUAL(requests()[3], 11);

    // The retry does not trust a remote head: only the normal certificate
    // verification and deterministic execution may install this response.
    flowmesh::ProductionEpochGate gate{domain, market, seats.seats};
    flowmesh::ProductionEntryCheck check;
    const auto genesis{flowmesh::BuildProductionExecutionEntry(
        initial, domain, market, seats.seats, gate, 0, 0, {},
        {100, Filled(0x71)}, {chain.TipHeight(), std::nullopt, &chain},
        treasury, {}, nullptr, check)};
    BOOST_REQUIRE(genesis);
    const auto certificate{Certify(genesis->entry, seats)};
    const auto certified{flowmesh::EncodeProductionCertifiedPayload(
        {genesis->entry, certificate}, seats.seats.Size())};
    BOOST_REQUIRE(certified);
    const auto payload{flowmesh::EncodeCatchupEntries(
        std::vector<std::vector<unsigned char>>{*certified})};
    BOOST_REQUIRE(payload);
    flowmesh::WireMessage response{flowmesh::WireMessageKind::ENTRIES,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, market, seats.seats.epoch, 0},
        *payload};
    BOOST_REQUIRE(runtime.EnqueueWireMessage(10, response) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(runtime.MarketStatus(market)->next_sequence, 1U);
    BOOST_CHECK(runtime.MarketStatus(market)->last_microblock_hash == genesis->entry.GetHash());
    BOOST_CHECK(runtime.MarketStatus(market)->halt == node::FlowMeshRuntimeHalt::NONE);
    BOOST_CHECK(runtime.MarketStatus(market)->observer_only);
    std::optional<uint256> lock;
    BOOST_REQUIRE(store.ReadLock({seats.seats.epoch, 0}, lock, error));
    BOOST_CHECK(!lock);
    BOOST_CHECK_EQUAL(requests().size(), 4U); // No speculative short-page GET.
    runtime.Stop();
}

BOOST_AUTO_TEST_CASE(discovery_hints_are_untrusted_and_failed_catchup_is_bounded)
{
    const uint256 domain{Filled(0x1a)};
    const modern::AssetId asset{Filled(0x3a)};
    const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const auto vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const SeatFixture seats{Seats(domain, market, 4, 7, 100, Filled(0x71), 120)};
    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(seats.seats);
    RuntimeKeys keys; // observer; hints must never activate a signer
    FixedClock clock;
    const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(),
                                        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    node::FlowMeshProductionStore store{DBParams{
        .path = m_args.GetDataDirBase() / "flowmesh_hint_limits",
        .cache_bytes = size_t{1} << 20, .wipe_data = true}};
    std::string error;
    BOOST_REQUIRE_MESSAGE(store.OpenForMarket(domain, market, seats.seats,
                                              initial.Root(), error), error);
    std::mutex relay_mutex;
    std::vector<node::FlowMeshRuntimeRelay> relayed;
    node::FlowMeshRuntimeConfig config;
    config.chain = &chain;
    config.keys = &keys;
    config.clock = &clock;
    config.relay = [&](node::FlowMeshRuntimeRelay relay) {
        std::lock_guard<std::mutex> lock{relay_mutex};
        relayed.push_back(std::move(relay));
        return LegacyRelayResult();
    };
    node::FlowMeshRuntime runtime{config, {MarketConfig(
        domain, market, Filled(0x5a), seats.seats, initial, store, nullptr)}};
    const auto count = [&](const flowmesh::WireMessageKind kind) {
        std::lock_guard<std::mutex> lock{relay_mutex};
        return std::count_if(relayed.begin(), relayed.end(), [&](const auto& relay) {
            return relay.message.kind == kind;
        });
    };
    BOOST_REQUIRE_MESSAGE(runtime.Start(error), error);
    runtime.NotifyTick();
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(count(flowmesh::WireMessageKind::HELLO), 1);
    for (int i{0}; i < 20; ++i) runtime.NotifyTick();
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(count(flowmesh::WireMessageKind::HELLO), 1);

    flowmesh::WireMessage hello;
    hello.kind = flowmesh::WireMessageKind::HELLO;
    hello.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1, market, 999, 999};
    hello.payload = flowmesh::EncodeMarketHello({Filled(0x2a), Filled(0x7a)});
    BOOST_REQUIRE(runtime.EnqueueWireMessage(8, hello) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(count(flowmesh::WireMessageKind::GET), 0);
    hello.payload = flowmesh::EncodeMarketHello({domain, Filled(0x7a)});
    BOOST_REQUIRE(runtime.EnqueueWireMessage(8, hello) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(count(flowmesh::WireMessageKind::GET), 1);
    for (int i{0}; i < 20; ++i) (void)runtime.EnqueueWireMessage(8, hello);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(count(flowmesh::WireMessageKind::GET), 1);
    BOOST_CHECK_EQUAL(runtime.MarketStatus(market)->next_sequence, 0U);
    BOOST_CHECK_EQUAL(runtime.MarketStatus(market)->epoch, 7U);
    BOOST_CHECK(runtime.StateSnapshot(market)->Root() == initial.Root());

    // A framed response with no certificate is not progress and earns no
    // immediate retry. It does not corrupt the certified state or journal.
    flowmesh::WireMessage bogus;
    bogus.kind = flowmesh::WireMessageKind::ENTRIES;
    bogus.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1, market, 7, 0};
    bogus.payload = *flowmesh::EncodeCatchupEntries(
        std::vector<std::vector<unsigned char>>{{0x42}});
    // Refill the separate control token bucket before testing retry policy.
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    clock.m_now += std::chrono::seconds{1};
    BOOST_REQUIRE(runtime.EnqueueWireMessage(8, bogus) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_REQUIRE(runtime.EnqueueWireMessage(8, hello) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(count(flowmesh::WireMessageKind::GET), 1);
    clock.m_now += std::chrono::seconds{15};
    runtime.NotifyTick();
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_REQUIRE(runtime.EnqueueWireMessage(8, hello) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(count(flowmesh::WireMessageKind::GET), 2);

    // This request receives no response. Its deadline expires, and the
    // same peer can be retried after the bounded cooldown instead of wedging.
    clock.m_now += std::chrono::seconds{16};
    runtime.NotifyTick();
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_REQUIRE(runtime.EnqueueWireMessage(8, hello) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(count(flowmesh::WireMessageKind::GET), 3);
    BOOST_CHECK_EQUAL(runtime.MarketStatus(market)->next_sequence, 0U);
    BOOST_CHECK(runtime.MarketStatus(market)->halt == node::FlowMeshRuntimeHalt::NONE);
    std::optional<uint256> lock;
    BOOST_REQUIRE(store.ReadLock({7, 0}, lock, error));
    BOOST_CHECK(!lock);

    // A valid prefix followed by an invalid entry must not satisfy the
    // whole-page tail condition, even with ample count/byte slack. Preserve
    // the existing progress-follow-up behavior from the first missing entry.
    flowmesh::ProductionEpochGate gate{domain, market, seats.seats};
    flowmesh::ProductionEntryCheck entry_check;
    const auto genesis{flowmesh::BuildProductionExecutionEntry(
        initial, domain, market, seats.seats, gate, 0, 0, {},
        {100, Filled(0x71)}, {chain.TipHeight(), std::nullopt, &chain},
        Filled(0x5a), {}, nullptr, entry_check)};
    BOOST_REQUIRE(genesis);
    const auto certificate{Certify(genesis->entry, seats)};
    const auto valid_payload{flowmesh::EncodeProductionCertifiedPayload(
        {genesis->entry, certificate}, seats.seats.Size())};
    BOOST_REQUIRE(valid_payload);
    const auto partial_payload{flowmesh::EncodeCatchupEntries(
        std::vector<std::vector<unsigned char>>{*valid_payload, {0x42}})};
    BOOST_REQUIRE(partial_payload);
    flowmesh::WireMessage partial{flowmesh::WireMessageKind::ENTRIES,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, market, seats.seats.epoch, 0},
        *partial_payload};
    BOOST_REQUIRE(runtime.EnqueueWireMessage(8, partial) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(runtime.MarketStatus(market)->next_sequence, 1U);
    BOOST_CHECK(runtime.MarketStatus(market)->last_microblock_hash == genesis->entry.GetHash());
    BOOST_CHECK(runtime.MarketStatus(market)->halt == node::FlowMeshRuntimeHalt::NONE);
    BOOST_CHECK_EQUAL(count(flowmesh::WireMessageKind::GET), 4);
    {
        std::lock_guard<std::mutex> guard{relay_mutex};
        const auto last_get{std::find_if(relayed.rbegin(), relayed.rend(), [](const auto& relay) {
            return relay.message.kind == flowmesh::WireMessageKind::GET;
        })};
        BOOST_REQUIRE(last_get != relayed.rend());
        BOOST_CHECK_EQUAL(last_get->message.header.sequence, 1U);
    }
    clock.m_now += std::chrono::seconds{6};
    runtime.NotifyTick();
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_REQUIRE(runtime.EnqueueWireMessage(8, hello) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(count(flowmesh::WireMessageKind::GET), 4); // failure cooldown unchanged
    runtime.Stop();
}

BOOST_AUTO_TEST_CASE(preagreement_runtime_commit_gate_genesis_deposit_and_exact_restart)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_runtime_commit"};
    std::string error;
    f.Tick();
    BOOST_REQUIRE_GT(f.prepares.load(), 0U);
    BOOST_REQUIRE_GT(f.commits.load(), 0U);
    BOOST_CHECK_EQUAL(f.decisions.load(), 0U);
    BOOST_CHECK_EQUAL(f.premature_v1.load(), 0U);
    BOOST_REQUIRE(f.AllAt(0));
    // PREPARE quorums and durable local COMMIT votes are insufficient: no V1
    // permanent lock exists while all cross-operator COMMIT delivery is held.
    for (const auto& store : f.stores) {
        std::optional<uint256> locked;
        BOOST_REQUIRE(store->ReadLock({0, 0}, locked, error));
        BOOST_CHECK(!locked);
        std::optional<node::FlowMeshProductionStore::Marker> marker;
        BOOST_REQUIRE(store->ReadMarker(marker, error));
        BOOST_REQUIRE(marker);
        BOOST_CHECK(marker->agreement_bootstrap_complete);
    }
    f.block_commits = false;
    f.Reach(1);
    BOOST_REQUIRE_GT(f.decisions.load(), 0U);
    const uint256 genesis{f.runtimes[0]->MarketStatus(f.market)->last_microblock_hash};
    for (size_t i{0}; i < 4; ++i) {
        std::optional<node::StoredProductionEntry> stored;
        BOOST_REQUIRE(f.stores[i]->ReadEntry(0, f.seats.seats, stored, error));
        BOOST_REQUIRE(stored);
        BOOST_CHECK(stored->entry.GetHash() == genesis);
        BOOST_CHECK(stored->entry.actions.empty());
        BOOST_REQUIRE(f.runtimes[i]->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    }
    f.Drain();
    f.Reach(2);
    const uint256 head{f.runtimes[0]->MarketStatus(f.market)->last_microblock_hash};
    for (size_t i{0}; i < 4; ++i) {
        std::optional<node::StoredProductionEntry> stored;
        BOOST_REQUIRE(f.stores[i]->ReadEntry(1, f.seats.seats, stored, error));
        BOOST_REQUIRE(stored);
        BOOST_CHECK(stored->entry.GetHash() == head);
        BOOST_REQUIRE_EQUAL(stored->entry.actions.size(), 1U);
        BOOST_CHECK(stored->entry.actions.front().IsDeposit());
        const auto state{f.runtimes[i]->StateSnapshot(f.market)};
        BOOST_REQUIRE(state);
        BOOST_CHECK_EQUAL(state->LedgerView().Available(f.account, f.asset), 250);
    }
    f.Drain();
    std::optional<node::FlowMeshProductionStore::Marker> before;
    BOOST_REQUIRE(f.stores[0]->ReadMarker(before, error));
    BOOST_REQUIRE(before);
    std::optional<uint256> lock_before;
    BOOST_REQUIRE(f.stores[0]->ReadLock({0, 1}, lock_before, error));
    f.StopNode(0);
    f.stores[0].reset();
    f.stores[0] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
        .path = f.StorePath(0), .cache_bytes = size_t{1} << 20}, true);
    BOOST_REQUIRE_MESSAGE(f.stores[0]->OpenForMarket(f.domain, f.market,
        f.seats.seats, f.initial.Root(), error), error);
    BOOST_REQUIRE_MESSAGE(f.StartNode(0, f.AgreementPath(0), std::nullopt, error), error);
    BOOST_CHECK(f.AllAt(2));
    BOOST_CHECK(f.runtimes[0]->MarketStatus(f.market)->last_microblock_hash == head);
    BOOST_CHECK_EQUAL(f.runtimes[0]->StateSnapshot(f.market)->LedgerView().Available(f.account, f.asset), 250);
    std::optional<node::FlowMeshProductionStore::Marker> after;
    BOOST_REQUIRE(f.stores[0]->ReadMarker(after, error));
    BOOST_REQUIRE(after);
    BOOST_CHECK(after->agreement_identity == before->agreement_identity);
    BOOST_CHECK(after->agreement_bootstrap_complete);
    std::optional<uint256> lock_after;
    BOOST_REQUIRE(f.stores[0]->ReadLock({0, 1}, lock_after, error));
    BOOST_CHECK(lock_after == lock_before);

    f.Drain();
    f.StopNode(0);
    BOOST_CHECK(!f.StartNode(0, f.StorePath(0) / "missing_agreement", std::nullopt, error));
    BOOST_CHECK(error.find("missing agreement journal") != std::string::npos);
    // A caller cannot regain bootstrap permission merely by setting a config
    // boolean after the production marker has completed its durable handshake.
    BOOST_CHECK(!f.StartNode(0, f.StorePath(0) / "forged_bootstrap", true, error));
    BOOST_CHECK(error.find("bootstrap permission") != std::string::npos);
    BOOST_REQUIRE_MESSAGE(f.StartNode(0, f.AgreementPath(0), std::nullopt, error), error);
    f.Drain();
    f.StopNode(0);
    f.stores[0].reset();
    node::FlowMeshProductionStore legacy{DBParams{
        .path = f.StorePath(0), .cache_bytes = size_t{1} << 20}};
    BOOST_CHECK(!legacy.OpenForMarket(f.domain, f.market, f.seats.seats, f.initial.Root(), error));
}

BOOST_AUTO_TEST_CASE(preagreement_runtime_divergent_preliminary_views_heal_without_v1_locks)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_runtime_views"};
    std::string error;
    f.network.PartitionHalves(true);
    f.Tick(); // View-zero proposal/prepares reach only the left two seats.
    BOOST_REQUIRE(f.AllAt(0));
    BOOST_REQUIRE_GT(f.prepares.load(), 0U);
    f.Drain();
    for (size_t i{2}; i < 4; ++i) f.clocks[i].m_now += std::chrono::seconds{11};
    for (size_t i{2}; i < 4; ++i) f.runtimes[i]->NotifyTick();
    f.Drain();
    BOOST_CHECK_EQUAL(f.runtimes[0]->MarketStatus(f.market)->round, 0U);
    BOOST_CHECK_EQUAL(f.runtimes[2]->MarketStatus(f.market)->round, 1U);
    BOOST_REQUIRE_GT(f.view_changes.load(), 0U);
    for (const auto& store : f.stores) {
        std::optional<uint256> locked;
        BOOST_REQUIRE(store->ReadLock({0, 0}, locked, error));
        BOOST_CHECK(!locked);
    }
    BOOST_CHECK_EQUAL(f.premature_v1.load(), 0U);
    f.block_commits = false;
    f.network.PartitionHalves(false);
    f.Reach(1);
    BOOST_REQUIRE_GT(f.decisions.load(), 0U);
    const auto agreed{f.runtimes[0]->MarketStatus(f.market)->last_microblock_hash};
    for (const auto& runtime : f.runtimes) {
        BOOST_CHECK(runtime->MarketStatus(f.market)->last_microblock_hash == agreed);
        BOOST_CHECK(runtime->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);
    }
}

BOOST_AUTO_TEST_CASE(preagreement_nine_runtimes_same_deposit_adjacent_anchors_heal)
{
    // Nine distinct operators/stores/workers, not three operators holding
    // three seats each. Chain answers and delivery remain isolated test mocks.
    PreagreementRuntimeHarness<9> f{m_args.GetDataDirBase() / "preagreement_nine_anchors"};
    std::string error;
    BOOST_REQUIRE_EQUAL(flowmesh::FlowMeshBlsThreshold(f.seats.seats.Size()), 7U);
    f.block_commits = false;
    f.Reach(1);
    f.Drain();
    const auto genesis{f.runtimes[0]->MarketStatus(f.market)->last_microblock_hash};
    const flowmesh::AnchorRef earlier{200, Filled(0x74)}, later{201, Filled(0x75)};
    f.deposits.additional_anchor = later;
    for (size_t i{0}; i < 9; ++i) {
        f.chains[i].AddCanonical(later);
        f.chains[i].SetTipHeight(later.height + flowmesh::FLOWMESH_PRODUCTION_MIN_ANCHOR_DEPTH);
        f.chains[i].m_current = i < 3 ? earlier : later;
    }
    std::mutex observed_mutex;
    std::map<uint32_t, flowmesh::ProductionEntryCore> proposals;
    std::atomic<bool> healed{false};
    std::atomic<size_t> early_final_votes{0};
    struct StopBeforeCapturedObservations {
        PreagreementRuntimeHarness<9>& fixture;
        ~StopBeforeCapturedObservations() { fixture.StopAll(); }
    } stop_before_observations{f};
    f.network.SetFilter([&](size_t from, size_t to, const flowmesh::WireMessage& wire) {
        if (wire.header.sequence == 1 && wire.kind == flowmesh::WireMessageKind::ATTESTATION && !healed) ++early_final_votes;
        if (wire.kind == flowmesh::WireMessageKind::AGREEMENT) {
            const auto message{flowmesh::DecodeAgreementMessage(wire.payload)};
            if (!message) return false;
            if (message->context.sequence == 1 && message->stage == flowmesh::AgreementStage::PROPOSAL) {
                const auto entry{flowmesh::DecodeProductionEntry(message->entry_bytes)};
                if (entry) { std::lock_guard<std::mutex> lock{observed_mutex}; proposals.try_emplace(message->view, *entry); }
            }
            // After healing, old messages are deliberately still delayed.
            // View two must succeed through its own seven-report proof.
            if (healed && message->context.sequence == 1 && message->view < 2) return false;
        }
        return healed || from / 3 == to / 3;
    });
    for (const auto& runtime : f.runtimes) {
        BOOST_REQUIRE(runtime->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    }
    f.Tick(); // Seat 1 proposes the earlier anchor in view zero to its partition.
    {
        std::lock_guard<std::mutex> lock{observed_mutex};
        BOOST_REQUIRE(proposals.contains(0));
        BOOST_CHECK(proposals.at(0).anchor == earlier);
    }
    // The right-hand partitions independently time out. Seat 3 builds the
    // adjacent-anchor candidate, but cannot publish it without seven reports.
    for (size_t round{0}; round < 2; ++round) {
        f.Drain();
        for (size_t i{3}; i < 9; ++i) f.clocks[i].m_now += std::chrono::seconds{11};
        for (size_t i{3}; i < 9; ++i) f.runtimes[i]->NotifyTick();
        f.Drain();
    }
    BOOST_REQUIRE(f.AllAt(1));
    BOOST_CHECK_EQUAL(f.runtimes[3]->MarketStatus(f.market)->round, 2U);
    const auto candidate_data{f.runtimes[3]->MarketData(f.market, std::nullopt, {}, error)};
    BOOST_REQUIRE(candidate_data);
    BOOST_REQUIRE_GT(candidate_data->snapshot.runtime.candidate_count, 0U);
    for (const auto& store : f.stores) {
        std::optional<uint256> locked;
        BOOST_REQUIRE(store->ReadLock({0, 1}, locked, error));
        BOOST_CHECK(!locked);
    }
    BOOST_CHECK_EQUAL(early_final_votes.load(), 0U);
    healed = true;
    f.Reach(2);
    f.Drain();
    flowmesh::ProductionEntryCore agreed;
    {
        std::lock_guard<std::mutex> lock{observed_mutex};
        BOOST_REQUIRE(proposals.contains(2));
        agreed = proposals.at(2);
        BOOST_CHECK(agreed.anchor == later);
        auto normalized{agreed}; normalized.anchor = earlier;
        BOOST_CHECK(flowmesh::EncodeProductionEntry(normalized) == flowmesh::EncodeProductionEntry(proposals.at(0)));
        BOOST_CHECK(agreed.GetHash() != proposals.at(0).GetHash());
    }
    for (size_t i{0}; i < 9; ++i) {
        std::optional<node::StoredProductionEntry> stored;
        BOOST_REQUIRE(f.stores[i]->ReadEntry(1, f.seats.seats, stored, error));
        BOOST_REQUIRE(stored);
        BOOST_CHECK(stored->entry.GetHash() == agreed.GetHash());
        BOOST_CHECK(stored->entry.parent_hash == genesis);
        BOOST_REQUIRE_EQUAL(stored->entry.actions.size(), 1U);
        BOOST_CHECK(stored->entry.actions.front().outpoint == f.outpoint);
        BOOST_CHECK_EQUAL(f.runtimes[i]->StateSnapshot(f.market)->LedgerView().Available(f.account, f.asset), 250);
        std::optional<uint256> locked;
        BOOST_REQUIRE(f.stores[i]->ReadLock({0, 1}, locked, error));
        BOOST_REQUIRE(locked);
        BOOST_CHECK(*locked == agreed.GetHash());
    }
    // Reopen every generated operator's original two stores, never a second
    // signer. Replay must preserve the certified entry and credit exactly once.
    f.network.SetFilter({});
    for (size_t i{0}; i < 9; ++i) f.StopNode(i);
    for (size_t i{0}; i < 9; ++i) {
        f.stores[i].reset();
        f.stores[i] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
            .path = f.StorePath(i), .cache_bytes = size_t{1} << 20}, true);
        BOOST_REQUIRE(f.stores[i]->OpenForMarket(f.domain, f.market, f.seats.seats, f.initial.Root(), error));
        BOOST_REQUIRE_MESSAGE(f.StartNode(i, f.AgreementPath(i), std::nullopt, error), error);
        BOOST_CHECK(f.runtimes[i]->MarketStatus(f.market)->last_microblock_hash == agreed.GetHash());
        BOOST_CHECK_EQUAL(f.runtimes[i]->StateSnapshot(f.market)->LedgerView().Available(f.account, f.asset), 250);
    }
    f.Tick();
    BOOST_CHECK(f.AllAt(2));
}

BOOST_AUTO_TEST_CASE(preagreement_nine_runtimes_keep_seven_vote_threshold)
{
    PreagreementRuntimeHarness<9> f{m_args.GetDataDirBase() / "preagreement_nine_availability"};
    f.block_commits = false;
    f.StopNode(7); f.StopNode(8);
    f.Reach(1); // Seven online seats certify; two remain stopped.
    for (size_t i{0}; i < 7; ++i) {
        BOOST_REQUIRE(f.runtimes[i]->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    }
    f.Reach(2);
    f.Drain();
    f.StopNode(6);
    const COutPoint second{Txid::FromUint256(Filled(0x9e)), 0};
    f.deposits.entries.emplace(second, flowmesh::DepositInfo{f.asset, 125, f.account});
    for (size_t i{0}; i < 6; ++i) {
        BOOST_REQUIRE(f.runtimes[i]->SubmitLocalAction(f.market, Deposit(second)) == flowmesh::QueueResult::ACCEPTED);
    }
    for (size_t attempt{0}; attempt < 3; ++attempt) f.Tick(std::chrono::seconds{2});
    BOOST_CHECK(f.AllAt(2));
    std::string error;
    for (size_t i{0}; i < 6; ++i) {
        std::optional<uint256> locked;
        BOOST_REQUIRE(f.stores[i]->ReadLock({0, 2}, locked, error));
        BOOST_CHECK(!locked);
        BOOST_CHECK_EQUAL(f.runtimes[i]->StateSnapshot(f.market)->LedgerView().Available(f.account, f.asset), 250);
    }
}

BOOST_AUTO_TEST_CASE(preagreement_prepared_anchor_reorg_preserves_history_without_final_vote)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_prepared_reorg"};
    f.block_commits = false;
    f.Reach(1);
    f.Drain();
    f.block_commits = true;
    const auto commits_before{f.commits.load()};
    for (const auto& runtime : f.runtimes) {
        BOOST_REQUIRE(runtime->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    }
    f.Tick();
    BOOST_REQUIRE_GT(f.commits.load(), commits_before); // Prepared quorum, not a complete decision.
    BOOST_REQUIRE(f.AllAt(1));
    f.Drain();
    // Temporarily unavailable chain data is not permission to sign through
    // reconciliation. Existing exact messages remain retryable.
    for (auto& chain : f.chains) chain.SetReconciled(false);
    f.block_commits = false;
    f.Tick(std::chrono::seconds{2});
    BOOST_CHECK(f.AllAt(1));
    f.Drain();
    const flowmesh::AnchorRef later{201, Filled(0x75)};
    f.deposits.additional_anchor = later;
    for (auto& chain : f.chains) {
        chain.AddCanonical({200, Filled(0x99)}); // Replace the prepared candidate's anchor.
        chain.AddCanonical(later);
        chain.SetTipHeight(231);
        chain.m_current = later;
        chain.SetReconciled(true);
    }
    for (size_t round{0}; round < 2; ++round) f.Tick(std::chrono::seconds{11});
    BOOST_CHECK(f.AllAt(1));
    std::string error;
    const auto check_unchanged = [&] {
        for (size_t i{0}; i < 4; ++i) {
            std::optional<uint256> lock;
            BOOST_REQUIRE(f.stores[i]->ReadLock({0, 1}, lock, error));
            BOOST_CHECK(!lock);
            BOOST_CHECK_EQUAL(f.runtimes[i]->StateSnapshot(f.market)->LedgerView().Available(f.account, f.asset), 0);
        }
    };
    check_unchanged();
    for (size_t i{0}; i < 4; ++i) f.StopNode(i);
    for (size_t i{0}; i < 4; ++i) {
        f.stores[i].reset();
        f.stores[i] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
            .path = f.StorePath(i), .cache_bytes = size_t{1} << 20}, true);
        BOOST_REQUIRE(f.stores[i]->OpenForMarket(f.domain, f.market, f.seats.seats, f.initial.Root(), error));
        BOOST_REQUIRE_MESSAGE(f.StartNode(i, f.AgreementPath(i), std::nullopt, error), error);
    }
    f.Tick();
    BOOST_CHECK(f.AllAt(1));
    check_unchanged();
    // This is NOT recovery onto the replacement fork. It only proves that
    // preserved decisions/evidence remain usable if the original anchor is
    // canonical again. An invalid prepared anchor cannot simply be forgotten.
    f.Drain();
    for (auto& chain : f.chains) chain.AddCanonical({200, Filled(0x74)});
    f.Reach(2);
    for (size_t i{0}; i < 4; ++i) {
        std::optional<node::StoredProductionEntry> stored;
        BOOST_REQUIRE(f.stores[i]->ReadEntry(1, f.seats.seats, stored, error));
        BOOST_REQUIRE(stored);
        BOOST_CHECK(stored->entry.anchor == (flowmesh::AnchorRef{200, Filled(0x74)}));
        BOOST_CHECK_EQUAL(f.runtimes[i]->StateSnapshot(f.market)->LedgerView().Available(f.account, f.asset), 250);
    }
}

BOOST_AUTO_TEST_SUITE_END()
