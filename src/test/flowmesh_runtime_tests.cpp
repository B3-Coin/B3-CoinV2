// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_runtime.h>

#include <flowmesh/auth.h>
#include <flowmesh/agreement_wire.h>
#include <flowmesh/market_view.h>
#include <flowmesh/outcome_evidence.h>
#include <hash.h>
#include <kernel/cs_main.h>
#include <logging.h>
#include <node/flowmesh_action_pool.h>
#include <node/flowmesh_client.h>
#include <node/flowmesh_observed_time_log.h>
#include <test/util/flowmesh.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace node {
// Test translation-unit only; no runtime callback, option or wire privilege.
class FlowMeshRuntimeTestAccess {
public:
    struct PublicationStats {
        size_t records, bytes, waiting;
        uint64_t inspected, encoded_input, attempted;
    };
    static PublicationStats Publications(FlowMeshRuntime& runtime)
    {
        std::lock_guard lock{runtime.m_market_mutex};
        size_t waiting{0};
        for (const auto& [id, source] : runtime.m_local_agreement_publications) {
            (void)id;
            waiting += source.waiting_chain_open;
        }
        return {runtime.m_local_agreement_publications.size(), runtime.m_local_agreement_publication_bytes,
            waiting, runtime.m_local_agreement_publications_inspected,
            runtime.m_local_agreement_publications_encoded_input, runtime.m_local_agreement_publications_attempted};
    }
    // Synthetic PRIVATE callback/cache controls, not a new wire privilege or
    // proof that an arbitrary test message was signed by this engine.
    static void CachePublication(FlowMeshRuntime& runtime, const flowmesh::AgreementMessage& message)
    {
        std::lock_guard lock{runtime.m_market_mutex};
        runtime.RememberLocalAgreementPublication(*runtime.m_markets.at(message.context.market_id), message);
    }
    static bool PublishPublication(FlowMeshRuntime& runtime, const flowmesh::AgreementMessage& message,
                                   const bool local_signature = false)
    {
        std::lock_guard lock{runtime.m_market_mutex};
        return runtime.PublishAgreement(*runtime.m_markets.at(message.context.market_id), message,
                                        std::nullopt, local_signature);
    }
    static void ExhaustAgreementRelayBudget(FlowMeshRuntime& runtime)
    {
        std::lock_guard lock{runtime.m_market_mutex};
        const auto now{runtime.m_config.clock->Now()};
        while (runtime.m_agreement_relay_budget.Available(now, 1)) runtime.m_agreement_relay_budget.Charge(1);
    }
    static constexpr size_t PublicationRecords() { return FlowMeshRuntime::AGREEMENT_RESUME_RECORDS; }
    static constexpr size_t PublicationMarketRecords() { return FlowMeshRuntime::AGREEMENT_RESUME_MARKET_RECORDS; }
    static constexpr size_t PublicationBytes() { return FlowMeshRuntime::AGREEMENT_RESUME_BYTES; }
    static constexpr size_t PublicationMarketBytes() { return FlowMeshRuntime::AGREEMENT_RESUME_MARKET_BYTES; }
    static constexpr size_t PublicationInspect() { return FlowMeshRuntime::AGREEMENT_RESUME_INSPECT; }
    static std::pair<size_t, size_t> MarketPublications(FlowMeshRuntime& runtime,
                                                       const flowmesh::MarketId& market)
    {
        std::lock_guard lock{runtime.m_market_mutex};
        size_t count{0}, bytes{0};
        for (const auto& [id, source] : runtime.m_local_agreement_publications) {
            (void)id;
            if (source.context.market_id != market) continue;
            ++count;
            bytes += source.payload.size() + flowmesh::FLOWMESH_WIRE_HEADER_SIZE;
        }
        return {count, bytes};
    }
    static uint64_t Reserve(FlowMeshRuntime& runtime, const flowmesh::WireMessage& wire,
                            const uint256& domain, const uint256& config, const std::string& submitter = "127.0.0.1")
    {
#ifdef FLOWMESH_LOCAL_RETENTION_BASELINE_TEST
        return 1; // Old source has no reservation; the captured queue body is identical.
#else
        return runtime.ReserveLocalAction(wire, domain, config, flowmesh::ClientNoticeOrigin::PUBLIC,
                                          submitter, runtime.m_config.clock->Now() + std::chrono::seconds{60});
#endif
    }
    static flowmesh::QueueResult Enqueue(FlowMeshRuntime& runtime, const uint64_t id, const flowmesh::WireMessage& wire)
    {
#ifdef FLOWMESH_LOCAL_RETENTION_BASELINE_TEST
        return runtime.EnqueueWireMessage(FLOWMESH_LOCAL_ACTION_PEER, wire);
#else
        return runtime.EnqueueReservedLocalAction(id);
#endif
    }
#ifndef FLOWMESH_LOCAL_RETENTION_BASELINE_TEST
    static std::pair<size_t, size_t> Sources(FlowMeshRuntime& runtime)
    {
        std::lock_guard lock{runtime.m_market_mutex};
        return {runtime.m_local_action_sources.size(), runtime.m_local_action_source_bytes};
    }
    static flowmesh::QueueResult Inject(FlowMeshRuntime& runtime, flowmesh::WirePeerId peer,
                                       flowmesh::WireMessage wire, uint64_t id, bool counted = true)
    {
        return runtime.Enqueue(peer, std::move(wire), counted, id);
    }
    static void DropQueueCopy(FlowMeshRuntime& runtime, uint64_t id)
    {
        std::lock_guard market{runtime.m_market_mutex};
        auto& source{runtime.m_local_action_sources.at(id)};
        std::lock_guard queue{runtime.m_queue_mutex};
        source.admitted = true;
        BOOST_REQUIRE(runtime.m_queue.Push(FLOWMESH_LOCAL_ACTION_PEER, source.queued.message,
            runtime.m_config.clock->Now(), source.queued.counted, id) == flowmesh::QueueResult::ACCEPTED);
        runtime.m_queue.RemovePeer(FLOWMESH_LOCAL_ACTION_PEER); // priority/drop semantics: source survives
    }
    static void ExhaustTokens(FlowMeshRuntime& runtime)
    {
        std::lock_guard lock{runtime.m_market_mutex};
        runtime.m_next_local_action_source = std::numeric_limits<uint64_t>::max();
    }
    static auto Deadline(FlowMeshRuntime& runtime, uint64_t id)
    {
        std::lock_guard lock{runtime.m_market_mutex};
        return runtime.m_local_action_sources.at(id).deadline;
    }
    static uint64_t ReserveUntil(FlowMeshRuntime& runtime, const flowmesh::WireMessage& wire,
                                 const uint256& domain, const uint256& config,
                                 flowmesh::WireClock::time_point deadline)
    {
        return runtime.ReserveLocalAction(wire, domain, config, flowmesh::ClientNoticeOrigin::PUBLIC,
                                          "127.0.0.1", deadline);
    }
    static bool Stopping(FlowMeshRuntime& runtime)
    {
        std::lock_guard lock{runtime.m_queue_mutex};
        return runtime.m_stopping;
    }
#endif
};
} // namespace node

namespace {

// Apple's C++20 library here does not yet provide std::jthread. Keep the
// exception-safe join guarantee, including failure to construct a sibling.
class JoiningTestThread {
    std::function<void()> m_before_join;
    std::thread m_thread;
public:
    template <typename Fn>
    explicit JoiningTestThread(Fn&& fn, std::function<void()> before_join = {})
        : m_before_join{std::move(before_join)}, m_thread{std::forward<Fn>(fn)} {}
    ~JoiningTestThread()
    {
        if (m_before_join) m_before_join();
        join();
    }
    void join() { if (m_thread.joinable()) m_thread.join(); }
};

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

    // A reconciliation that started and finished while the worker was idle:
    // the gate is open again but every generation-stamped observation is old.
    void BumpDeliveryGeneration() { m_delivery_generation += 2; }

    void ReconcileOnceWhileChecking(const flowmesh::AnchorRef& anchor,
                                   const size_t successful_checks = 0,
                                   const bool reopen = true,
                                   const bool invalidate = false)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_reconcile_once_anchor = anchor;
        m_reconcile_after_checks = successful_checks;
        m_reconcile_reopen = reopen;
        m_reconcile_invalidate = invalidate;
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
                if (m_reconcile_after_checks != 0) {
                    --m_reconcile_after_checks;
                } else {
                    m_reconcile_once_anchor.reset();
                    ++m_delivery_generation;
                    // The gate may have reopened before the caller samples
                    // it, or remain closed until the test completes the tip.
                    if (!m_reconcile_reopen) m_reconciled = false;
                    if (m_reconcile_invalidate) m_canonical[anchor.height] = Filled(0xfa);
                    return false;
                }
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

    void SetPausedTransition(const flowmesh::MarketId& market, std::string reason)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_transitions[market] = {node::FlowMeshSeatTransitionKind::PAUSED, std::nullopt, std::move(reason)};
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
    mutable std::map<int32_t, uint256> m_canonical{{100, Filled(0x71)},
                                           {200, Filled(0x74)}};

private:
    mutable std::atomic<bool> m_reconciled{true};
    mutable std::atomic<uint64_t> m_delivery_generation{0};
    mutable std::optional<flowmesh::AnchorRef> m_reconcile_once_anchor;
    mutable size_t m_reconcile_after_checks{0};
    bool m_reconcile_reopen{true};
    bool m_reconcile_invalidate{false};
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
        if (after_get) after_get();
        return it == m_keys.end() ? std::vector<bls::SecretKey>{}
                                  : it->second;
    }

    std::map<flowmesh::MarketId, std::vector<bls::SecretKey>> m_keys;
    // Optional production-service behavior: configured keys are masked while
    // B3 reconciliation is in progress, without actually disarming them.
    RuntimeChain* m_visibility_chain{nullptr};
    //! Test-only, installed/removed while all workers are idle. The native
    //! engine still performs its own validation and durable signing writes.
    std::function<void()> after_get;
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
        if (on_get) on_get();
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
    //! Run first by every GetDeposit call, on the calling thread. Set and
    //! reset only while no runtime is working.
    std::function<void()> on_get;
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

/** Deterministic model of one independent-network (FMN2) critical channel.
 *
 * Every directed link is a FIFO of at most 1,024 items. The receiver offers
 * the head frame to the real runtime admission queue; a refusal retains that
 * exact frame, backs off from 100 ms to 1 s and reads nothing queued behind
 * it, as flowmesh_net.cpp does. Thirty seconds of refusal drops the link's
 * queue like the production disconnect. Local queueing here is never treated
 * as receipt: only an ACCEPTED runtime admission counts as delivered. */
class Fmn2LinkModel
{
public:
    static constexpr size_t PEER_QUEUE_ITEMS{1024};
    static constexpr auto RETRY_INITIAL{std::chrono::milliseconds{100}};
    static constexpr auto RETRY_MAX{std::chrono::milliseconds{1000}};
    static constexpr auto RETRY_TIMEOUT{std::chrono::seconds{30}};
    using Time = flowmesh::WireClock::time_point;

    explicit Fmn2LinkModel(size_t count) : m_count{count}, m_links(count * count) {}

    node::FlowMeshRelayResult Offer(const size_t from, const node::FlowMeshRuntimeRelay& relay, const Time now)
    {
        node::FlowMeshRelayResult result;
        std::lock_guard<std::mutex> lock{m_mutex};
        if (m_partitioned) return result; // No connected peer: nothing is queued or admitted.
        const bool agreement{relay.message.kind == flowmesh::WireMessageKind::AGREEMENT};
        if (agreement) {
            ++agreement_publications;
            max_publications_per_message = std::max(max_publications_per_message,
                ++m_publications[Hash(relay.message.payload)]);
        }
        for (size_t to{0}; to < m_count; ++to) {
            if (to == from || (relay.peer && *relay.peer != static_cast<int64_t>(to)) ||
                (relay.exclude_peer && *relay.exclude_peer == static_cast<int64_t>(to))) continue;
            auto& link{m_links[from * m_count + to]};
            if (link.queue.size() >= PEER_QUEUE_ITEMS) {
                ++queue_refusals;
                result.peers.push_back({static_cast<int64_t>(to), node::FlowMeshDeliveryAdmission::FULL,
                                        "critical per-peer item limit"});
                continue;
            }
            link.queue.push_back({relay.message, now});
            max_queue_depth = std::max(max_queue_depth, link.queue.size());
            result.peers.push_back({static_cast<int64_t>(to), node::FlowMeshDeliveryAdmission::LEGACY_UNTRACKED, {}});
        }
        return result;
    }

    // A disconnect discards every locally queued frame, as production does.
    void Partition(const bool partitioned)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_partitioned = partitioned;
        if (partitioned) for (auto& link : m_links) link = Link{};
    }

    template <typename Runtimes>
    bool Pump(const Time now, const Runtimes& runtimes)
    {
        bool progress{false};
        for (size_t from{0}; from < m_count; ++from) for (size_t to{0}; to < m_count; ++to) {
            auto& link{m_links[from * m_count + to]};
            for (;;) {
                std::optional<Item> head;
                {
                    std::lock_guard<std::mutex> lock{m_mutex};
                    if (link.queue.empty() || now < link.next_retry) break;
                    head = link.queue.front();
                }
                const auto result{runtimes[to] ? runtimes[to]->EnqueueWireMessage(static_cast<int64_t>(from), head->message)
                                               : flowmesh::QueueResult::STOPPED};
                std::lock_guard<std::mutex> lock{m_mutex};
                const bool agreement{head->message.kind == flowmesh::WireMessageKind::AGREEMENT};
                if (result == flowmesh::QueueResult::ACCEPTED || result == flowmesh::QueueResult::MALFORMED) {
                    if (agreement && result == flowmesh::QueueResult::ACCEPTED) {
                        ++agreement_deliveries;
                        if (delivered.size() < 16) delivered.push_back({from, to, head->message});
                        max_agreement_wait = std::max(max_agreement_wait,
                            std::chrono::duration_cast<std::chrono::milliseconds>(now - head->offered));
                    }
                    link.queue.pop_front();
                    link.pending = false;
                    link.delay = RETRY_INITIAL;
                    link.next_retry = {};
                    progress = true;
                    continue;
                }
                if (result == flowmesh::QueueResult::RATE_LIMITED) ++rate_limited;
                if (!link.pending) { link.pending = true; link.pending_since = now; link.delay = RETRY_INITIAL; }
                if (now - link.pending_since >= RETRY_TIMEOUT) {
                    ++retry_timeouts;
                    link.queue.clear();
                    link.pending = false;
                    break;
                }
                link.next_retry = now + link.delay;
                link.delay = std::min<std::chrono::milliseconds>(RETRY_MAX, link.delay * 2);
                break;
            }
        }
        return progress;
    }

    // Read only while every runtime worker is idle.
    size_t agreement_publications{0}, agreement_deliveries{0}, max_publications_per_message{0};
    size_t rate_limited{0}, queue_refusals{0}, retry_timeouts{0}, max_queue_depth{0};
    std::chrono::milliseconds max_agreement_wait{0};
    struct Delivered { size_t from, to; flowmesh::WireMessage message; };
    std::vector<Delivered> delivered; // First few admitted AGREEMENT frames.

private:
    struct Item { flowmesh::WireMessage message; Time offered; };
    struct Link {
        std::deque<Item> queue;
        bool pending{false};
        Time pending_since{}, next_retry{};
        std::chrono::milliseconds delay{RETRY_INITIAL};
    };
    const size_t m_count;
    std::mutex m_mutex;
    bool m_partitioned{false};
    std::vector<Link> m_links;
    std::map<uint256, size_t> m_publications;
};

/** Real runtime/real BLS/real durable stores, with only the existing mock
 * canonical-chain oracle, deterministic clock, and in-memory transport. */
template <size_t Count = 4>
class PreagreementRuntimeHarness
{
public:
    explicit PreagreementRuntimeHarness(fs::path directory,
        const std::chrono::milliseconds timeout = std::chrono::seconds{10},
        const size_t deposit_count = 1, const bool fmn2_links = false)
        : path{std::move(directory)}, round_timeout{timeout}, use_links{fmn2_links}
    {
        for (auto& chain : chains) {
            chain.m_domain = domain;
            chain.Add(seats.seats);
            chain.SetTipHeight(chain.Current().height + flowmesh::FLOWMESH_PRODUCTION_MIN_ANCHOR_DEPTH);
        }
        deposits.required_anchor = chains[0].Current();
        for (uint32_t n{0}; n < deposit_count; ++n) {
            deposits.entries.emplace(COutPoint{outpoint.hash, n}, flowmesh::DepositInfo{asset, 250, account});
        }
        network.IgnoreHellos();
        network.SetFilter([this](size_t, size_t, const flowmesh::WireMessage& wire) { return Observe(wire); });
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

    bool Observe(const flowmesh::WireMessage& wire)
    {
        if (wire.kind != flowmesh::WireMessageKind::AGREEMENT) return true;
        const auto message{flowmesh::DecodeAgreementMessage(wire.payload)};
        if (!message) return false;
        if (message->stage == flowmesh::AgreementStage::PREPARE) ++prepares;
        if (message->stage == flowmesh::AgreementStage::COMMIT) ++commits;
        if (message->stage == flowmesh::AgreementStage::VIEW_CHANGE) ++view_changes;
        if (message->stage == flowmesh::AgreementStage::DECISION) ++decisions;
        return !block_commits || (message->stage != flowmesh::AgreementStage::COMMIT &&
                                  message->stage != flowmesh::AgreementStage::DECISION);
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
        config.round_timeout = round_timeout;
        config.recent_window = recent_window;
        config.relay = [this, i](node::FlowMeshRuntimeRelay relay) {
            if (block_commits && (relay.message.kind == flowmesh::WireMessageKind::ATTESTATION ||
                                 relay.message.kind == flowmesh::WireMessageKind::PROPOSAL)) ++premature_v1;
            if (use_links) {
                if (relay.message.kind == flowmesh::WireMessageKind::HELLO || !Observe(relay.message)) return LegacyRelayResult();
                return links.Offer(i, relay, clocks[i].Now());
            }
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
        if (after_drain) after_drain();
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

    // FMN2-link mode: advance every clock together, let each blocked channel
    // retry, and schedule the production 250 ms runtime tick.
    void Step(const std::chrono::milliseconds advance = std::chrono::milliseconds{50})
    {
        Drain();
        for (auto& clock : clocks) clock.m_now += advance;
        elapsed += advance;
        const auto deliver = [this] {
            for (size_t pass{0}; pass < 64 && links.Pump(clocks[0].Now(), runtimes); ++pass) Drain();
        };
        deliver();
        if (elapsed.count() % 250 == 0) {
            for (const auto& runtime : runtimes) if (runtime) runtime->NotifyTick();
            Drain();
            deliver();
        }
    }

    bool StepUntil(uint64_t sequence, const std::chrono::milliseconds limit)
    {
        const auto start{elapsed};
        while (!AllAt(sequence) && elapsed - start < limit) Step();
        return AllAt(sequence);
    }

    const fs::path path;
    const std::chrono::milliseconds round_timeout;
    const bool use_links;
    std::chrono::milliseconds elapsed{0};
    // Optional observer run while every worker is idle, e.g. to read each
    // bounded delivery-event ring before it can wrap.
    std::function<void()> after_drain;
    // Applied by StartNode; tests restart a node to lower it.
    node::FlowMeshRecentWindowLimits recent_window{};
    Fmn2LinkModel links{Count};
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

// Observer-only native stores/engines. Private cache controls below isolate
// resource and scheduling invariants; they do not pretend these synthetic
// messages were issued by each engine. The end-to-end test uses actual Sign.
class AgreementResumeCacheFixture
{
public:
    explicit AgreementResumeCacheFixture(const fs::path& path, const size_t count = 1)
    {
        chain.m_domain = domain;
        std::vector<node::FlowMeshRuntimeMarketConfig> configs;
        std::string error;
        for (size_t i{0}; i < count; ++i) {
            const modern::AssetId asset{Filled(static_cast<unsigned char>(0x50 + i))};
            const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
            auto seats{Seats(domain, market, 4, 0, 100, Filled(0x71), static_cast<unsigned char>(150 + 8 * i))};
            const flowmesh::FlowMeshState initial{*flowmesh::ComputeFlowMeshVaultId(domain, market), asset,
                                                  modern::NativeAsset(), flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
            chain.Add(seats.seats);
            const auto store_path{path / fs::PathFromString("market_" + std::to_string(i))};
            stores.push_back(std::make_unique<node::FlowMeshProductionStore>(DBParams{
                .path = store_path, .cache_bytes = size_t{1} << 20, .wipe_data = true}, true));
            BOOST_REQUIRE_MESSAGE(stores.back()->OpenForMarket(domain, market, seats.seats, initial.Root(), error), error);
            std::optional<node::FlowMeshProductionStore::Marker> marker;
            BOOST_REQUIRE(stores.back()->ReadMarker(marker, error));
            BOOST_REQUIRE(marker);
            auto config{MarketConfig(domain, market, Filled(0x6c), seats.seats, initial, *stores.back(), nullptr)};
            config.preagreement = true;
            config.agreement_path = store_path / "agreement";
            config.agreement_identity = marker->agreement_identity;
            config.agreement_allow_create = true;
            configs.push_back(std::move(config));
            contexts.push_back({domain, market, 0, seats.seats.set_hash, 0, {}, initial.Root(), initial.ConfigId()});
            seat_fixtures.push_back(std::move(seats));
        }
        node::FlowMeshRuntimeConfig config;
        config.chain = &chain;
        config.keys = &keys;
        config.clock = &clock;
        config.round_timeout = std::chrono::hours{1};
        config.relay = [this](node::FlowMeshRuntimeRelay relay) {
            if (relay.message.kind != flowmesh::WireMessageKind::AGREEMENT) return LegacyRelayResult();
            sent.push_back(relay.message.payload); // Read only after WaitForIdle.
            if (std::exchange(reclose, false)) chain.SetReconciled(false);
            node::FlowMeshRelayResult result;
            result.peers.push_back({9, mode, {}});
            return result;
        };
        runtime = std::make_unique<node::FlowMeshRuntime>(config, std::move(configs));
        BOOST_REQUIRE_MESSAGE(runtime->Start(error), error);
        Tick(); // Arm ordinary one-second retries with no local keys/signed records.
    }
    ~AgreementResumeCacheFixture() { runtime->Stop(); }
    void Tick(const std::chrono::milliseconds advance = std::chrono::milliseconds{0})
    {
        BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{5}));
        clock.m_now += advance;
        runtime->NotifyTick();
        BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{5}));
    }
    flowmesh::AgreementMessage Message(const size_t market, const uint32_t view) const
    {
        flowmesh::AgreementMessage message;
        message.context = contexts.at(market);
        message.stage = flowmesh::AgreementStage::VIEW_CHANGE;
        message.view = view;
        const auto digest{flowmesh::AgreementMessageDigest(message)};
        BOOST_REQUIRE(digest);
        message.signature = seat_fixtures.at(market).secrets[0].Sign(
            std::span<const unsigned char>{digest->begin(), 32}).Compressed();
        return message;
    }
    RuntimeChain chain;
    RuntimeKeys keys;
    FixedClock clock;
    const uint256 domain{Filled(0x2c)};
    std::vector<std::unique_ptr<node::FlowMeshProductionStore>> stores;
    std::vector<flowmesh::PreagreementContext> contexts;
    std::vector<SeatFixture> seat_fixtures;
    node::FlowMeshDeliveryAdmission mode{node::FlowMeshDeliveryAdmission::ADMITTED};
    bool reclose{false};
    std::vector<std::vector<unsigned char>> sent;
    std::unique_ptr<node::FlowMeshRuntime> runtime;
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

/** Counts one runtime's entry executions from its bounded delivery-event
 * ring. Sample after every drain; an event evicted before it was sampled
 * fails the test instead of silently undercounting. */
class ExecutionLog
{
public:
    void Sample(const node::FlowMeshRuntime& runtime, const flowmesh::MarketId& market)
    {
        const auto snapshots{runtime.DeliverySnapshots(market)};
        BOOST_REQUIRE_EQUAL(snapshots.size(), 1U);
        for (const auto& event : snapshots.front().events) {
            if (event.event_id <= m_last) continue;
            BOOST_REQUIRE_EQUAL(event.event_id, m_last + 1);
            m_last = event.event_id;
            ++m_stages[{event.sequence, event.stage}];
            if (event.stage == "execution_started") ++m_started[{event.sequence, event.reason}];
        }
    }

    size_t Executions(const uint64_t sequence, const std::string& reason) const
    {
        const auto it{m_started.find({sequence, reason})};
        return it == m_started.end() ? 0 : it->second;
    }

    size_t Events(const uint64_t sequence, const std::string& stage) const
    {
        const auto it{m_stages.find({sequence, stage})};
        return it == m_stages.end() ? 0 : it->second;
    }

private:
    uint64_t m_last{0};
    std::map<std::pair<uint64_t, std::string>, size_t> m_started;
    std::map<std::pair<uint64_t, std::string>, size_t> m_stages;
};

class ScopedBenchLogging
{
    const BCLog::CategoryMask mask{LogInstance().GetCategoryMask()};
    const decltype(LogInstance().CategoryLevels()) levels{LogInstance().CategoryLevels()};
public:
    ScopedBenchLogging()
    {
        auto enabled{levels};
        enabled[BCLog::BENCH] = BCLog::Level::Debug;
        LogInstance().SetCategoryLogLevel(enabled);
        LogInstance().EnableCategory(BCLog::BENCH);
    }
    ~ScopedBenchLogging()
    {
        LogInstance().SetCategoryLogLevel(levels);
        LogInstance().DisableCategory(BCLog::ALL);
        LogInstance().EnableCategory(BCLog::LogFlags{mask});
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_runtime_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(idle_market_reports_required_chain_pause_without_refresh_spam)
{
    DeliveryRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_idle_pause_trace",
        node::FlowMeshDeliveryAdmission::ADMITTED, false};
    f.chain.SetTransition(f.seats.seats.market_id, node::FlowMeshSeatTransitionKind::PAUSED);
    for (unsigned i{0}; i != 3; ++i) f.Tick(std::chrono::milliseconds{100});
    const auto snapshot{f.Snapshot()};
    BOOST_CHECK_EQUAL(std::count_if(snapshot.events.begin(), snapshot.events.end(), [](const auto& event) {
        return event.stage == "proposer_wait" && event.reason.starts_with("seat_transition_paused");
    }), 1);
    BOOST_CHECK(f.Sent().empty());
}

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

BOOST_AUTO_TEST_CASE(bench_spans_preserve_operational_delivery_history)
{
    ScopedBenchLogging logging;
    BOOST_REQUIRE(LogAcceptCategory(BCLog::BENCH, BCLog::Level::Debug));
    DeliveryRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_bench_history",
        node::FlowMeshDeliveryAdmission::ADMITTED};
    // Preserve initial admission evidence, then remove local signing
    // eligibility while the worker is idle. No repeated V1 proposal work is
    // expected from the diagnostic-only ticks below.
    f.keys.m_keys.clear();
    f.Tick();
    const auto initial{f.Snapshot()};
    BOOST_REQUIRE(std::any_of(initial.events.begin(), initial.events.end(), [](const auto& event) {
        return event.stage == "admitted";
    }));
    // The clock never advances: these ticks have no eligible retry/timer work,
    // but each ends a BENCH market-processing span. More than a whole ring of
    // diagnostics must leave operational evidence and its byte budget intact.
    for (unsigned i{0}; i < 256; ++i) f.Tick();
    const auto after{f.Snapshot()};
    BOOST_CHECK_EQUAL(after.last_event_id, initial.last_event_id);
    BOOST_CHECK_EQUAL(after.events_dropped, initial.events_dropped);
    BOOST_CHECK_EQUAL(after.trace_bytes, initial.trace_bytes);
    BOOST_CHECK_EQUAL(after.trace_events_dropped, initial.trace_events_dropped);
    BOOST_CHECK_EQUAL(after.trace_global_bytes, initial.trace_global_bytes);
    BOOST_CHECK_EQUAL(after.trace_global_events_dropped, initial.trace_global_events_dropped);
    BOOST_CHECK_EQUAL(after.admitted, initial.admitted);
    BOOST_CHECK_EQUAL(after.refused, initial.refused);
    BOOST_CHECK(after.last_target_hash == initial.last_target_hash);
    BOOST_REQUIRE_EQUAL(after.events.size(), initial.events.size());
    for (size_t i{0}; i < after.events.size(); ++i) {
        BOOST_CHECK_EQUAL(after.events[i].event_id, initial.events[i].event_id);
        BOOST_CHECK_EQUAL(after.events[i].stage, initial.events[i].stage);
        BOOST_CHECK(after.events[i].object_id == initial.events[i].object_id);
    }
    flowmesh::WireMessage hello;
    hello.kind = flowmesh::WireMessageKind::HELLO;
    hello.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 0};
    hello.payload = flowmesh::EncodeMarketHello({f.domain, {}});
    for (const bool enabled : {false, true}) {
        if (enabled) LogInstance().EnableCategory(BCLog::BENCH);
        else LogInstance().DisableCategory(BCLog::BENCH);
        const auto before{f.Snapshot()};
        BOOST_REQUIRE(f.runtime->EnqueueWireMessage(DeliveryRuntimeFixture::PEER, hello) == flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
        const auto processed{f.Snapshot()};
        // The original message_processing event is the only operational
        // observation; diagnostic completion/gate events have their own log.
        BOOST_CHECK_EQUAL(processed.last_event_id, before.last_event_id + 1);
        BOOST_CHECK_EQUAL(processed.events.back().stage, "message_processing");
        BOOST_CHECK_EQUAL(processed.events_dropped, before.events_dropped);
        BOOST_CHECK_EQUAL(processed.refused, before.refused);
    }
}

BOOST_AUTO_TEST_CASE(bench_agreement_spans_are_not_operational_events)
{
    ScopedBenchLogging logging;
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_bench_history"};
    f.Tick();
    BOOST_REQUIRE_GT(f.prepares.load(), 0U);
    BOOST_REQUIRE_GT(f.commits.load(), 0U);
    for (const auto& runtime : f.runtimes) {
        const auto snapshots{runtime->DeliverySnapshots(f.market)};
        BOOST_REQUIRE_EQUAL(snapshots.size(), 1U);
        BOOST_CHECK_LE(snapshots.front().events.size(), 128U);
        for (const auto& event : snapshots.front().events) {
            BOOST_CHECK(event.stage != "agreement_span");
            BOOST_CHECK(event.stage != "message_chain_gate");
            BOOST_CHECK(event.stage != "agreement_finalization_completed");
        }
    }
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

BOOST_AUTO_TEST_CASE(deferred_critical_messages_requeue_immediately_when_gate_reopens)
{
    using Admission = node::FlowMeshDeliveryAdmission;
    DeliveryRuntimeFixture producer{m_args.GetDataDirBase() / "flowmesh_reopen_producer", Admission::ADMITTED};
    const auto original{producer.Sent()};
    const auto proposal{std::find_if(original.begin(), original.end(), [](const auto& relay) {
        return relay.message.kind == flowmesh::WireMessageKind::PROPOSAL;
    })};
    BOOST_REQUIRE(proposal != original.end());
    const auto decoded{flowmesh::DecodeProductionProposalPayload(proposal->message.payload)};
    BOOST_REQUIRE(decoded);
    // Each recipient's construction tick ran a retry pass at this clock time,
    // which used to pace the reopen tick out for 250 ms. Closed-gate passes
    // restarted that pacing as well, and a generation change whose gate had
    // already reopened was never treated as a reopen.
    for (const std::string gate : {"closed", "closed_passes", "generation"}) {
        DeliveryRuntimeFixture recipient{m_args.GetDataDirBase() / fs::PathFromString("flowmesh_reopen_" + gate),
                                         Admission::ADMITTED, false};
        if (gate == "generation") {
            BOOST_REQUIRE(!(decoded->entry.anchor == recipient.chain.Current()));
            recipient.chain.ReconcileOnceWhileChecking(decoded->entry.anchor);
        } else {
            recipient.chain.SetReconciled(false);
        }
        BOOST_REQUIRE(recipient.runtime->EnqueueWireMessage(DeliveryRuntimeFixture::PEER,
            proposal->message) == flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(recipient.runtime->WaitForIdle(std::chrono::seconds{2}));
        BOOST_CHECK_EQUAL(recipient.Snapshot().deferred_objects, 1U);
        BOOST_CHECK(recipient.Sent().empty());
        if (gate == "closed_passes") {
            for (int i{0}; i < 3; ++i) recipient.Tick(std::chrono::milliseconds{100});
            BOOST_CHECK_EQUAL(recipient.Snapshot().deferred_objects, 1U);
            BOOST_CHECK(recipient.Sent().empty());
        }
        if (gate != "generation") recipient.chain.SetReconciled(true);
        recipient.Tick(); // the reopen wake-up itself; the clock does not move
        BOOST_CHECK_EQUAL(recipient.Snapshot().deferred_objects, 0U);
        BOOST_CHECK_EQUAL(recipient.Snapshot().deferred_bytes, 0U);
        const auto sent{recipient.Sent()};
        BOOST_CHECK(std::any_of(sent.begin(), sent.end(), [](const auto& relay) {
            return relay.message.kind == flowmesh::WireMessageKind::ATTESTATION;
        }));
        recipient.CheckLock(original);
    }
}

BOOST_AUTO_TEST_CASE(deferred_reopen_pass_is_bounded_to_32_items)
{
    // The per-market (16) and per-peer (8) deferral caps need three markets
    // and five peers to hold more input than one reopen pass may requeue.
    const uint256 domain{Filled(0x2c)};
    RuntimeChain chain;
    chain.m_domain = domain;
    RuntimeKeys keys; // no local seat: nothing is proposed or signed
    FixedClock clock;
    std::vector<flowmesh::MarketId> markets;
    std::vector<std::unique_ptr<node::FlowMeshProductionStore>> stores;
    std::vector<node::FlowMeshRuntimeMarketConfig> configs;
    for (unsigned char i{0}; i < 3; ++i) {
        const modern::AssetId asset{Filled(static_cast<unsigned char>(0x50 + i))};
        const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
        const auto seats{Seats(domain, market, 4, 7, 100, Filled(0x71), static_cast<unsigned char>(150 + 8 * i))};
        const flowmesh::FlowMeshState initial{*flowmesh::ComputeFlowMeshVaultId(domain, market), asset,
                                              modern::NativeAsset(), flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
        chain.Add(seats.seats);
        stores.push_back(std::make_unique<node::FlowMeshProductionStore>(DBParams{
            .path = m_args.GetDataDirBase() / fs::PathFromString("flowmesh_reopen_budget_" + std::to_string(i)),
            .cache_bytes = size_t{1} << 20, .wipe_data = true}));
        std::string error;
        BOOST_REQUIRE_MESSAGE(stores.back()->OpenForMarket(domain, market, seats.seats, initial.Root(), error), error);
        configs.push_back(MarketConfig(domain, market, Filled(0x6c), seats.seats, initial, *stores.back(), nullptr));
        markets.push_back(market);
    }
    node::FlowMeshRuntimeConfig config;
    config.chain = &chain;
    config.keys = &keys;
    config.clock = &clock;
    config.round_timeout = std::chrono::hours{1};
    config.relay = [](node::FlowMeshRuntimeRelay) { return LegacyRelayResult(); };
    node::FlowMeshRuntime runtime{config, configs};
    std::string error;
    BOOST_REQUIRE_MESSAGE(runtime.Start(error), error);
    const auto tick = [&](const std::chrono::milliseconds elapsed) {
        BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
        clock.m_now += elapsed;
        runtime.NotifyTick();
        BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    };
    const auto deferred = [&] {
        size_t total{0};
        for (const auto& market : markets) {
            const auto snapshots{runtime.DeliverySnapshots(market)};
            BOOST_REQUIRE_EQUAL(snapshots.size(), 1U);
            total += snapshots.front().deferred_objects;
        }
        return total;
    };
    const auto requeued = [&] {
        size_t total{0};
        for (const auto& market : markets) {
            const auto snapshots{runtime.DeliverySnapshots(market)};
            BOOST_REQUIRE_EQUAL(snapshots.size(), 1U);
            total += std::count_if(snapshots.front().events.begin(), snapshots.front().events.end(),
                                   [](const auto& event) { return event.stage == "receive_requeued"; });
        }
        return total;
    };
    tick(std::chrono::milliseconds{0});

    // Distinct unverified certificates: never obsolete, dropped once handled.
    chain.SetReconciled(false);
    for (uint64_t k{0}; k < 34; ++k) {
        flowmesh::WireMessage junk;
        junk.kind = flowmesh::WireMessageKind::CERTIFICATE;
        junk.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1, markets[k < 12 ? 0 : k < 24 ? 1 : 2], 7, k};
        junk.payload = {static_cast<unsigned char>(k), 0x42};
        BOOST_REQUIRE(runtime.EnqueueWireMessage(-2 - static_cast<flowmesh::WirePeerId>(k % 5), junk) ==
                      flowmesh::QueueResult::ACCEPTED);
    }
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(deferred(), 34U);

    chain.SetReconciled(true);
    tick(std::chrono::milliseconds{0});
    BOOST_CHECK_EQUAL(requeued(), 32U);
    BOOST_CHECK_EQUAL(deferred(), 2U);
    // The remainder is paced again from the reopen pass.
    tick(std::chrono::milliseconds{0});
    tick(std::chrono::milliseconds{100});
    BOOST_CHECK_EQUAL(deferred(), 2U);
    // A reconciliation that both started and ended between two ticks moves
    // only the generation; that is a reopen too.
    chain.BumpDeliveryGeneration();
    tick(std::chrono::milliseconds{0});
    BOOST_CHECK_EQUAL(deferred(), 0U);
    BOOST_CHECK_EQUAL(requeued(), 34U);
    for (const auto& market : markets) {
        BOOST_CHECK(runtime.MarketStatus(market)->halt == node::FlowMeshRuntimeHalt::NONE);
        BOOST_CHECK_EQUAL(runtime.MarketStatus(market)->next_sequence, 0U);
    }
    runtime.Stop();
}

BOOST_AUTO_TEST_CASE(critical_certificate_append_retries_reconciliation_without_losing_lock)
{
    for (const bool reopen : {false, true}) {
        DeliveryRuntimeFixture f{m_args.GetDataDirBase() /
            (reopen ? "flowmesh_append_reopened" : "flowmesh_append_paused"),
            node::FlowMeshDeliveryAdmission::ADMITTED};
        const auto original{f.Sent()};
        const auto proposal{std::find_if(original.begin(), original.end(), [](const auto& relay) {
            return relay.message.kind == flowmesh::WireMessageKind::PROPOSAL;
        })};
        BOOST_REQUIRE(proposal != original.end());
        const auto decoded{flowmesh::DecodeProductionProposalPayload(proposal->message.payload)};
        BOOST_REQUIRE(decoded);
        const auto certificate{Certify(decoded->entry, f.seats)};
        const auto payload{flowmesh::EncodeProductionCertifiedPayload(
            {decoded->entry, certificate}, f.seats.seats.Size())};
        BOOST_REQUIRE(payload);
        const flowmesh::WireMessage message{flowmesh::WireMessageKind::CERTIFICATE,
            proposal->message.header, *payload};

        // The candidate already exists. Its first anchor check is the commit
        // precheck; reject the second, inside the real store's re-execution.
        // The outer current-anchor admission uses a different anchor.
        BOOST_REQUIRE(!(decoded->entry.anchor == f.chain.Current()));
        f.chain.ReconcileOnceWhileChecking(decoded->entry.anchor, 1, reopen);
        BOOST_REQUIRE(f.runtime->EnqueueWireMessage(DeliveryRuntimeFixture::PEER, message) ==
                      flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
        BOOST_CHECK_EQUAL(f.chain.DeliveryGeneration(), 1U);
        BOOST_CHECK(f.runtime->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);
        BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 0U);
        BOOST_CHECK_EQUAL(f.Snapshot().durably_applied, 0U);
        BOOST_CHECK_EQUAL(f.Snapshot().deferred_objects, 1U);
        const auto paused{f.Snapshot()};
        BOOST_CHECK(std::any_of(paused.events.begin(), paused.events.end(), [](const auto& event) {
            return event.stage == "publication_deferred";
        }));
        f.CheckLock(original);
        std::string error;
        std::optional<node::FlowMeshProductionStore::Marker> marker;
        BOOST_REQUIRE(f.store.ReadMarker(marker, error));
        BOOST_REQUIRE(marker);
        BOOST_CHECK_EQUAL(marker->next_sequence, 0U);
        std::optional<node::StoredProductionEntry> stored;
        // ReadEntry rejects an index at/above the durable head; an absent
        // future entry is not a successful optional lookup in this API.
        BOOST_REQUIRE(!f.store.ReadEntry(0, f.seats.seats, stored, error));
        BOOST_CHECK_EQUAL(error, "FlowMesh v3 entry request is outside the active database");
        BOOST_CHECK(!stored);
        error.clear();

        f.chain.SetReconciled(true);
        f.Tick(std::chrono::milliseconds{250});
        BOOST_CHECK(f.runtime->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);
        BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 1U);
        BOOST_CHECK_EQUAL(f.Snapshot().durably_applied, 1U);
        BOOST_CHECK_EQUAL(f.Snapshot().deferred_objects, 0U);
        BOOST_REQUIRE(f.store.ReadEntry(0, f.seats.seats, stored, error));
        BOOST_REQUIRE(stored);
        BOOST_CHECK(stored->entry.GetHash() == decoded->entry.GetHash());
        BOOST_CHECK(flowmesh::CheckProductionEntryCertificate(stored->entry, f.seats.seats,
                                                              stored->certificate) ==
                    flowmesh::BlsCertificateCheck::OK);
        std::optional<uint256> locked;
        BOOST_REQUIRE(f.store.ReadLock({f.seats.seats.epoch, 0}, locked, error));
        BOOST_REQUIRE(locked);
        BOOST_CHECK(*locked == decoded->entry.GetHash());
        // A later exact certificate replay never appends the entry twice.
        BOOST_REQUIRE(f.runtime->EnqueueWireMessage(DeliveryRuntimeFixture::PEER, message) ==
                      flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
        BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 1U);
        BOOST_CHECK_EQUAL(f.Snapshot().durably_applied, 1U);
    }
}

BOOST_AUTO_TEST_CASE(catchup_first_certificate_append_retries_reconciliation_without_new_gossip)
{
    for (const bool reopen : {false, true}) {
        DeliveryRuntimeFixture f{m_args.GetDataDirBase() /
            (reopen ? "flowmesh_catchup_append_reopened" : "flowmesh_catchup_append_paused"),
            node::FlowMeshDeliveryAdmission::ADMITTED};
        const auto original{f.Sent()};
        const auto proposal{std::find_if(original.begin(), original.end(), [](const auto& relay) {
            return relay.message.kind == flowmesh::WireMessageKind::PROPOSAL;
        })};
        BOOST_REQUIRE(proposal != original.end());
        const auto decoded{flowmesh::DecodeProductionProposalPayload(proposal->message.payload)};
        BOOST_REQUIRE(decoded);
        flowmesh::ProductionEpochGate gate{f.domain, f.market, f.seats.seats};
        flowmesh::ProductionEntryCheck check;
        const auto genesis{flowmesh::ExecuteProductionEntry(
            f.initial, decoded->entry, f.domain, f.market, f.seats.seats, gate,
            0, 0, {}, {f.chain.TipHeight(), std::nullopt, &f.chain},
            Filled(0x6b), nullptr, check)};
        BOOST_REQUIRE(genesis);
        const auto next{flowmesh::BuildProductionExecutionEntry(
            genesis->next_state, f.domain, f.market, f.seats.seats, gate,
            1, decoded->entry.effect_count, decoded->entry.GetHash(), f.chain.Current(),
            {f.chain.TipHeight(), decoded->entry.anchor, &f.chain}, Filled(0x6b), {}, nullptr, check)};
        BOOST_REQUIRE(next);
        const auto first_payload{flowmesh::EncodeProductionCertifiedPayload(
            {decoded->entry, Certify(decoded->entry, f.seats)}, f.seats.seats.Size())};
        const auto next_payload{flowmesh::EncodeProductionCertifiedPayload(
            {next->entry, Certify(next->entry, f.seats)}, f.seats.seats.Size())};
        BOOST_REQUIRE(first_payload);
        BOOST_REQUIRE(next_payload);
        const auto page{flowmesh::EncodeCatchupEntries(
            std::vector<std::vector<unsigned char>>{*first_payload, *next_payload})};
        BOOST_REQUIRE(page);
        f.runtime->RequestCatchup(DeliveryRuntimeFixture::PEER, f.market);
        BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
        BOOST_REQUIRE_EQUAL(f.Requests().size(), 1U);
        const auto request{f.Requests().front()};

        f.chain.ReconcileOnceWhileChecking(decoded->entry.anchor, 1, reopen);
        BOOST_REQUIRE(f.runtime->EnqueueWireMessage(DeliveryRuntimeFixture::PEER,
            {flowmesh::WireMessageKind::ENTRIES, request.message.header, *page}) == flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
        BOOST_CHECK_EQUAL(f.chain.DeliveryGeneration(), 1U);
        BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 0U);
        BOOST_CHECK(f.runtime->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);
        BOOST_CHECK_EQUAL(f.Snapshot().catchup_partial_pages, 1U);
        BOOST_CHECK_EQUAL(f.Snapshot().deferred_objects, 1U);
        f.CheckLock(original);

        // No hello, new attestation, reconnect or external RequestCatchup can
        // rescue this page. Its exact first certificate must survive locally.
        f.chain.SetReconciled(true);
        f.Tick(std::chrono::milliseconds{250});
        BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 1U);
        BOOST_CHECK_EQUAL(f.Snapshot().durably_applied, 1U);
        BOOST_CHECK_EQUAL(f.Snapshot().deferred_objects, 0U);
        BOOST_CHECK_EQUAL(f.Requests().size(), 1U);
        f.Tick(std::chrono::milliseconds{14749});
        BOOST_CHECK_EQUAL(f.Requests().size(), 1U); // Existing 15-second cooldown is preserved.
        f.Tick(std::chrono::milliseconds{250}); // Next bounded recovery pass after the cooldown.
        BOOST_REQUIRE_EQUAL(f.Requests().size(), 2U);
        const auto retry{f.Requests().back()};
        BOOST_CHECK_EQUAL(retry.message.header.sequence, 1U);
        BOOST_CHECK(retry.message.payload == request.message.payload); // Same bounded request limits.
        const auto remainder{flowmesh::EncodeCatchupEntries(
            std::vector<std::vector<unsigned char>>{*next_payload})};
        BOOST_REQUIRE(remainder);
        BOOST_REQUIRE(f.runtime->EnqueueWireMessage(DeliveryRuntimeFixture::PEER,
            {flowmesh::WireMessageKind::ENTRIES, retry.message.header, *remainder}) == flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
        BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 2U);
        BOOST_CHECK_EQUAL(f.Snapshot().durably_applied, 2U);
        BOOST_CHECK(f.runtime->MarketStatus(f.market)->last_microblock_hash == next->entry.GetHash());
        BOOST_CHECK(f.runtime->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);
    }
}

BOOST_AUTO_TEST_CASE(catchup_first_certificate_early_checks_retain_retry_ownership)
{
    DeliveryRuntimeFixture producer{m_args.GetDataDirBase() / "flowmesh_catchup_early_producer",
                                   node::FlowMeshDeliveryAdmission::ADMITTED};
    const auto original{producer.Sent()};
    const auto proposal{std::find_if(original.begin(), original.end(), [](const auto& relay) {
        return relay.message.kind == flowmesh::WireMessageKind::PROPOSAL;
    })};
    BOOST_REQUIRE(proposal != original.end());
    const auto decoded{flowmesh::DecodeProductionProposalPayload(proposal->message.payload)};
    BOOST_REQUIRE(decoded);
    flowmesh::ProductionEpochGate gate{producer.domain, producer.market, producer.seats.seats};
    flowmesh::ProductionEntryCheck check;
    const auto genesis{flowmesh::ExecuteProductionEntry(
        producer.initial, decoded->entry, producer.domain, producer.market, producer.seats.seats, gate,
        0, 0, {}, {producer.chain.TipHeight(), std::nullopt, &producer.chain},
        Filled(0x6b), nullptr, check)};
    BOOST_REQUIRE(genesis);
    const auto next{flowmesh::BuildProductionExecutionEntry(
        genesis->next_state, producer.domain, producer.market, producer.seats.seats, gate,
        1, decoded->entry.effect_count, decoded->entry.GetHash(), producer.chain.Current(),
        {producer.chain.TipHeight(), decoded->entry.anchor, &producer.chain}, Filled(0x6b), {}, nullptr, check)};
    BOOST_REQUIRE(next);
    const auto first_payload{flowmesh::EncodeProductionCertifiedPayload(
        {decoded->entry, Certify(decoded->entry, producer.seats)}, producer.seats.seats.Size())};
    const auto next_payload{flowmesh::EncodeProductionCertifiedPayload(
        {next->entry, Certify(next->entry, producer.seats)}, producer.seats.seats.Size())};
    BOOST_REQUIRE(first_payload);
    BOOST_REQUIRE(next_payload);
    const auto page{flowmesh::EncodeCatchupEntries(
        std::vector<std::vector<unsigned char>>{*first_payload, *next_payload})};
    BOOST_REQUIRE(page);
    for (const bool cached : {false, true}) for (const bool reopen : {false, true}) {
        DeliveryRuntimeFixture f{m_args.GetDataDirBase() / fs::PathFromString(
            "flowmesh_catchup_early_" + std::to_string(cached) + "_" + std::to_string(reopen)),
            node::FlowMeshDeliveryAdmission::ADMITTED, cached};
        f.runtime->RequestCatchup(DeliveryRuntimeFixture::PEER, f.market);
        BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
        BOOST_REQUIRE_EQUAL(f.Requests().size(), 1U);
        const auto request{f.Requests().front()};
        BOOST_REQUIRE(!(decoded->entry.anchor == f.chain.Current()));
        // An absent candidate hits EvaluateCandidate; an already cached one
        // hits CommitCertified's precheck. Neither reaches the append API's
        // typed rejection, and the gate can reopen before a final sample.
        f.chain.ReconcileOnceWhileChecking(decoded->entry.anchor, 0, reopen);
        BOOST_REQUIRE(f.runtime->EnqueueWireMessage(DeliveryRuntimeFixture::PEER,
            {flowmesh::WireMessageKind::ENTRIES, request.message.header, *page}) == flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
        BOOST_CHECK_EQUAL(f.chain.DeliveryGeneration(), 1U);
        BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 0U);
        BOOST_CHECK(f.runtime->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);
        const auto interrupted{f.Snapshot()};
        BOOST_CHECK_EQUAL(interrupted.durably_applied, 0U);
        BOOST_CHECK_EQUAL(interrupted.deferred_objects, 1U);
        BOOST_CHECK_EQUAL(interrupted.deferred_bytes,
                          first_payload->size() + flowmesh::FLOWMESH_WIRE_HEADER_SIZE);
        BOOST_CHECK(std::any_of(interrupted.events.begin(), interrupted.events.end(), [](const auto& event) {
            return event.stage == "certificate_verified";
        }));
        BOOST_CHECK(std::none_of(interrupted.events.begin(), interrupted.events.end(), [](const auto& event) {
            return event.stage == "publication_started";
        }));
        std::string error;
        std::optional<uint256> locked;
        BOOST_REQUIRE(f.store.ReadLock({f.seats.seats.epoch, 0}, locked, error));
        BOOST_CHECK_EQUAL(locked.has_value(), cached);
        if (locked) BOOST_CHECK(*locked == decoded->entry.GetHash());
        std::optional<node::FlowMeshProductionStore::Marker> marker;
        BOOST_REQUIRE(f.store.ReadMarker(marker, error));
        BOOST_REQUIRE(marker);
        BOOST_CHECK_EQUAL(marker->next_sequence, 0U);

        // No new certificate, hello, vote or reconnect provides recovery.
        f.chain.SetReconciled(true);
        f.Tick(std::chrono::milliseconds{250});
        BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 1U);
        BOOST_CHECK_EQUAL(f.Snapshot().durably_applied, 1U);
        BOOST_CHECK_EQUAL(f.Snapshot().deferred_objects, 0U);
        std::optional<node::StoredProductionEntry> stored;
        BOOST_REQUIRE(f.store.ReadEntry(0, f.seats.seats, stored, error));
        BOOST_REQUIRE(stored);
        BOOST_CHECK(flowmesh::EncodeProductionEntry(stored->entry) ==
                    flowmesh::EncodeProductionEntry(decoded->entry));
        BOOST_CHECK(flowmesh::CheckProductionEntryCertificate(stored->entry, f.seats.seats,
                                                              stored->certificate) == flowmesh::BlsCertificateCheck::OK);
        f.Tick(std::chrono::milliseconds{14749});
        BOOST_CHECK_EQUAL(f.Requests().size(), 1U);
        f.Tick(std::chrono::milliseconds{250});
        BOOST_REQUIRE_EQUAL(f.Requests().size(), 2U);
        const auto retry{f.Requests().back()};
        BOOST_CHECK_EQUAL(retry.message.header.sequence, 1U);
        BOOST_CHECK(retry.message.payload == request.message.payload);
        const auto remainder{flowmesh::EncodeCatchupEntries(
            std::vector<std::vector<unsigned char>>{*next_payload})};
        BOOST_REQUIRE(remainder);
        BOOST_REQUIRE(f.runtime->EnqueueWireMessage(DeliveryRuntimeFixture::PEER,
            {flowmesh::WireMessageKind::ENTRIES, retry.message.header, *remainder}) == flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
        BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 2U);
        BOOST_CHECK_EQUAL(f.Snapshot().durably_applied, 2U);
        BOOST_CHECK(f.runtime->MarketStatus(f.market)->last_microblock_hash == next->entry.GetHash());
        BOOST_REQUIRE(f.runtime->EnqueueWireMessage(DeliveryRuntimeFixture::PEER,
            {flowmesh::WireMessageKind::CERTIFICATE, proposal->message.header, *first_payload}) == flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
        BOOST_CHECK_EQUAL(f.Snapshot().durably_applied, 2U);
        BOOST_CHECK(f.runtime->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);
    }
}

BOOST_AUTO_TEST_CASE(catchup_invalid_certificate_during_closed_gate_has_no_retry_owner)
{
    DeliveryRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_catchup_early_invalid",
                            node::FlowMeshDeliveryAdmission::ADMITTED};
    const auto original{f.Sent()};
    const auto proposal{std::find_if(original.begin(), original.end(), [](const auto& relay) {
        return relay.message.kind == flowmesh::WireMessageKind::PROPOSAL;
    })};
    BOOST_REQUIRE(proposal != original.end());
    const auto decoded{flowmesh::DecodeProductionProposalPayload(proposal->message.payload)};
    BOOST_REQUIRE(decoded);
    auto payload{flowmesh::EncodeProductionCertifiedPayload(
        {decoded->entry, Certify(decoded->entry, f.seats)}, f.seats.seats.Size())};
    BOOST_REQUIRE(payload);
    payload->back() ^= 1;
    const auto page{flowmesh::EncodeCatchupEntries(std::vector<std::vector<unsigned char>>{*payload})};
    BOOST_REQUIRE(page);
    f.runtime->RequestCatchup(DeliveryRuntimeFixture::PEER, f.market);
    BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
    BOOST_REQUIRE_EQUAL(f.Requests().size(), 1U);
    const auto request{f.Requests().front()};
    f.chain.SetReconciled(false);
    BOOST_REQUIRE(f.runtime->EnqueueWireMessage(DeliveryRuntimeFixture::PEER,
        {flowmesh::WireMessageKind::ENTRIES, request.message.header, *page}) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
    const auto refused{f.Snapshot()};
    BOOST_CHECK_EQUAL(refused.receive_deferred, 0U);
    BOOST_CHECK_EQUAL(refused.deferred_objects, 0U);
    BOOST_CHECK_EQUAL(refused.durably_applied, 0U);
    BOOST_CHECK(std::none_of(refused.events.begin(), refused.events.end(), [](const auto& event) {
        return event.stage == "certificate_verified" || event.stage == "certificate_validation_deferred";
    }));
    f.chain.SetReconciled(true);
    f.Tick(std::chrono::seconds{16});
    BOOST_CHECK_EQUAL(f.Requests().size(), 1U);
    BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 0U);
    BOOST_CHECK(f.runtime->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);
    f.CheckLock(original);
}

BOOST_AUTO_TEST_CASE(catchup_conflicting_decision_during_closed_gate_has_no_retry_owner)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "flowmesh_catchup_early_conflict"};
    f.block_commits = false;
    // Permit preliminary agreement, then hold final V1 shares so its durable
    // decision and permanent lock exist before the certified append.
    f.network.SetFilter([](size_t, size_t, const flowmesh::WireMessage& wire) {
        return wire.kind == flowmesh::WireMessageKind::AGREEMENT;
    });
    f.Tick();
    BOOST_REQUIRE(f.AllAt(0));
    std::string error;
    std::optional<node::StoredLockedProductionCandidate> locked;
    BOOST_REQUIRE(f.stores[0]->ReadLockedCandidate({0, 0}, locked, error));
    BOOST_REQUIRE(locked);
    auto conflicting{locked->entry};
    conflicting.anchor = f.chains[0].Current();
    BOOST_REQUIRE(conflicting.GetHash() != locked->entry.GetHash());
    const auto payload{flowmesh::EncodeProductionCertifiedPayload(
        {conflicting, Certify(conflicting, f.seats)}, f.seats.seats.Size())};
    BOOST_REQUIRE(payload);
    const auto page{flowmesh::EncodeCatchupEntries(std::vector<std::vector<unsigned char>>{*payload})};
    BOOST_REQUIRE(page);
    f.runtimes[0]->RequestCatchup(1, f.market);
    f.Drain();
    f.chains[0].SetReconciled(false);
    BOOST_REQUIRE(f.runtimes[0]->EnqueueWireMessage(1,
        {flowmesh::WireMessageKind::ENTRIES,
         {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, 0, 0}, *page}) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    BOOST_CHECK(f.runtimes[0]->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::CERTIFICATE_CONFLICT);
    BOOST_CHECK_EQUAL(f.runtimes[0]->MarketStatus(f.market)->next_sequence, 0U);
    const auto snapshot{f.runtimes[0]->DeliverySnapshots(f.market).front()};
    BOOST_CHECK_EQUAL(snapshot.receive_deferred, 0U);
    BOOST_CHECK_EQUAL(snapshot.deferred_objects, 0U);
    std::optional<uint256> retained;
    BOOST_REQUIRE(f.stores[0]->ReadLock({0, 0}, retained, error));
    BOOST_CHECK(retained == locked->entry.GetHash());
}

BOOST_AUTO_TEST_CASE(catchup_invalid_certificate_does_not_earn_reconciliation_retry)
{
    DeliveryRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_catchup_append_invalid",
                            node::FlowMeshDeliveryAdmission::ADMITTED};
    const auto original{f.Sent()};
    const auto proposal{std::find_if(original.begin(), original.end(), [](const auto& relay) {
        return relay.message.kind == flowmesh::WireMessageKind::PROPOSAL;
    })};
    BOOST_REQUIRE(proposal != original.end());
    const auto decoded{flowmesh::DecodeProductionProposalPayload(proposal->message.payload)};
    BOOST_REQUIRE(decoded);
    auto payload{flowmesh::EncodeProductionCertifiedPayload(
        {decoded->entry, Certify(decoded->entry, f.seats)}, f.seats.seats.Size())};
    BOOST_REQUIRE(payload);
    payload->back() ^= 1; // Framed but not a valid certificate for this entry.
    const auto page{flowmesh::EncodeCatchupEntries(
        std::vector<std::vector<unsigned char>>{*payload})};
    BOOST_REQUIRE(page);
    f.runtime->RequestCatchup(DeliveryRuntimeFixture::PEER, f.market);
    BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
    BOOST_REQUIRE_EQUAL(f.Requests().size(), 1U);
    BOOST_REQUIRE(f.runtime->EnqueueWireMessage(DeliveryRuntimeFixture::PEER,
        {flowmesh::WireMessageKind::ENTRIES, f.Requests().front().message.header, *page}) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
    f.Tick(std::chrono::seconds{16});
    BOOST_CHECK_EQUAL(f.Requests().size(), 1U);
    BOOST_CHECK_EQUAL(f.Snapshot().deferred_objects, 0U);
    BOOST_CHECK_EQUAL(f.Snapshot().durably_applied, 0U);
    BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 0U);
    BOOST_CHECK(f.runtime->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);
}

BOOST_AUTO_TEST_CASE(critical_certificate_append_reconciliation_does_not_mask_invalid_anchor)
{
    DeliveryRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_append_invalid_anchor",
                            node::FlowMeshDeliveryAdmission::ADMITTED};
    const auto original{f.Sent()};
    const auto proposal{std::find_if(original.begin(), original.end(), [](const auto& relay) {
        return relay.message.kind == flowmesh::WireMessageKind::PROPOSAL;
    })};
    BOOST_REQUIRE(proposal != original.end());
    const auto decoded{flowmesh::DecodeProductionProposalPayload(proposal->message.payload)};
    BOOST_REQUIRE(decoded);
    const auto payload{flowmesh::EncodeProductionCertifiedPayload(
        {decoded->entry, Certify(decoded->entry, f.seats)}, f.seats.seats.Size())};
    BOOST_REQUIRE(payload);
    f.chain.ReconcileOnceWhileChecking(decoded->entry.anchor, 1, true, true);
    BOOST_REQUIRE(f.runtime->EnqueueWireMessage(DeliveryRuntimeFixture::PEER,
        {flowmesh::WireMessageKind::CERTIFICATE, proposal->message.header, *payload}) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(f.chain.DeliveryGeneration(), 1U);
    BOOST_CHECK(f.runtime->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::ANCHOR_INVALIDATED);
    BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 0U);
    BOOST_CHECK_EQUAL(f.Snapshot().durably_applied, 0U);
    std::string error;
    std::optional<uint256> locked;
    BOOST_REQUIRE(f.store.ReadLock({f.seats.seats.epoch, 0}, locked, error));
    BOOST_REQUIRE(locked);
    BOOST_CHECK(*locked == decoded->entry.GetHash());
}

BOOST_AUTO_TEST_CASE(critical_certificate_append_store_failure_still_halts)
{
    DeliveryRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_append_store_failure",
                            node::FlowMeshDeliveryAdmission::ADMITTED};
    const auto before_state{f.runtime->StateView(f.market)};
    BOOST_REQUIRE(before_state);
    const auto original{f.Sent()};
    const auto proposal{std::find_if(original.begin(), original.end(), [](const auto& relay) {
        return relay.message.kind == flowmesh::WireMessageKind::PROPOSAL;
    })};
    BOOST_REQUIRE(proposal != original.end());
    const auto decoded{flowmesh::DecodeProductionProposalPayload(proposal->message.payload)};
    BOOST_REQUIRE(decoded);
    const auto payload{flowmesh::EncodeProductionCertifiedPayload(
        {decoded->entry, Certify(decoded->entry, f.seats)}, f.seats.seats.Size())};
    BOOST_REQUIRE(payload);
    // A failed strict reopen leaves this real store unavailable. The runtime
    // still holds its independently validated candidate, so append must stop.
    std::string error;
    BOOST_REQUIRE(!f.store.OpenForMarket(Filled(0xfe), f.market, f.seats.seats,
                                         f.initial.Root(), error));
    BOOST_REQUIRE(f.runtime->EnqueueWireMessage(DeliveryRuntimeFixture::PEER,
        {flowmesh::WireMessageKind::CERTIFICATE, proposal->message.header, *payload}) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK(f.runtime->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::STORE_FAILURE);
    BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 0U);
    BOOST_CHECK_EQUAL(f.Snapshot().durably_applied, 0U);
    BOOST_CHECK_EQUAL(f.Snapshot().deferred_objects, 0U);
    BOOST_CHECK(f.runtime->StateView(f.market) == before_state);
    const auto data{f.runtime->MarketData(f.market, std::nullopt, {}, error)};
    BOOST_REQUIRE(data);
    BOOST_CHECK(!data->snapshot.certified);
    BOOST_CHECK(data->snapshot.state_root == before_state->Root());
    BOOST_CHECK(!f.runtime->ClientSnapshot(f.market, error));
    const auto events{f.runtime->ClientEvents(std::nullopt, f.market, std::nullopt)};
    BOOST_CHECK(std::none_of(events.events.begin(), events.events.end(), [](const auto& event) {
        return event.kind == flowmesh::ClientEventKind::CERTIFIED_HEAD ||
               event.kind == flowmesh::ClientEventKind::CERTIFIED_INCLUDED;
    }));
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

BOOST_AUTO_TEST_CASE(critical_paused_delivery_resends_immediately_after_reopen)
{
    using Outcome = node::FlowMeshDeliveryOutcome;
    for (const bool reconciliation : {true, false}) {
        DeliveryRuntimeFixture f{m_args.GetDataDirBase() / fs::PathFromString(
            reconciliation ? "flowmesh_resume_reconciliation" : "flowmesh_resume_transition"),
            node::FlowMeshDeliveryAdmission::ADMITTED};
        const auto pause = [&](const bool paused) {
            if (reconciliation) f.chain.SetReconciled(!paused);
            else f.chain.SetTransition(f.market, paused ? node::FlowMeshSeatTransitionKind::PAUSED
                                                        : node::FlowMeshSeatTransitionKind::CONTINUE);
        };
        // The proposal and the local vote were admitted to the peer and have
        // not completed; the pause cancels those possibly unsent copies.
        const auto original{f.Sent()};
        BOOST_REQUIRE_EQUAL(original.size(), 2U);
        pause(true);
        f.Tick(std::chrono::milliseconds{100});
        const auto cancelled{f.Cancelled()};
        for (const auto& relay : original) {
            BOOST_CHECK(std::find(cancelled.begin(), cancelled.end(), relay.delivery_id) != cancelled.end());
        }
        BOOST_CHECK_EQUAL(f.Sent().size(), 2U);
        // Resumed 200 ms after the attempt, not after its one-second repeat.
        pause(false);
        f.Tick(std::chrono::milliseconds{100});
        auto sent{f.Sent()};
        CheckExactDeliveryRetry(original, sent, 2);
        BOOST_CHECK_EQUAL(f.Snapshot().pending_objects, 2U);

        // A pause that cancelled nothing, because the peer already took the
        // bytes, keeps the normal repeat delay.
        for (size_t i{2}; i < sent.size(); ++i) f.Complete(sent[i], Outcome::SOCKET_WRITTEN);
        pause(true);
        f.Tick(std::chrono::milliseconds{100});
        pause(false);
        f.Tick(std::chrono::milliseconds{100});
        BOOST_CHECK_EQUAL(f.Sent().size(), 4U);
        f.Tick(std::chrono::milliseconds{799});
        BOOST_CHECK_EQUAL(f.Sent().size(), 4U);
        f.Tick(std::chrono::milliseconds{1});
        const std::vector<node::FlowMeshRuntimeRelay> resumed(sent.begin() + 2, sent.end());
        CheckExactDeliveryRetry(resumed, f.Sent(), 4);
        f.CheckLock(original);
    }
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

BOOST_AUTO_TEST_CASE(durable_duplicate_certificate_is_not_verified_again_but_conflicts_halt)
{
    DeliveryRuntimeFixture producer{m_args.GetDataDirBase() / "flowmesh_duplicate_certificate_producer",
                                   node::FlowMeshDeliveryAdmission::ADMITTED};
    const auto original{producer.Sent()};
    const auto proposal{std::find_if(original.begin(), original.end(), [](const auto& relay) {
        return relay.message.kind == flowmesh::WireMessageKind::PROPOSAL;
    })};
    BOOST_REQUIRE(proposal != original.end());
    const auto decoded{flowmesh::DecodeProductionProposalPayload(proposal->message.payload)};
    BOOST_REQUIRE(decoded);
    flowmesh::ProductionEpochGate gate{producer.domain, producer.market, producer.seats.seats};
    flowmesh::ProductionEntryCheck check;
    const auto genesis{flowmesh::ExecuteProductionEntry(
        producer.initial, decoded->entry, producer.domain, producer.market, producer.seats.seats, gate,
        0, 0, {}, {producer.chain.TipHeight(), std::nullopt, &producer.chain},
        Filled(0x6b), nullptr, check)};
    BOOST_REQUIRE(genesis);
    const auto next{flowmesh::BuildProductionExecutionEntry(
        genesis->next_state, producer.domain, producer.market, producer.seats.seats, gate,
        1, decoded->entry.effect_count, decoded->entry.GetHash(), producer.chain.Current(),
        {producer.chain.TipHeight(), decoded->entry.anchor, &producer.chain}, Filled(0x6b), {}, nullptr, check)};
    BOOST_REQUIRE(next);
    // Seats 0-2 sign the applied copies; seats 1-3 form a different, equally
    // valid certificate for the same entry.
    const auto payload = [&](const flowmesh::ProductionEntryCore& entry, const std::vector<uint32_t>& signers) {
        std::vector<flowmesh::IndexedBlsSignature> signatures;
        const auto context{flowmesh::ProductionCertificateContext(entry)};
        for (const uint32_t seat : signers) {
            const auto signature{flowmesh::SignBlsMicroblockCertificate(
                producer.seats.secrets[seat], context, producer.seats.seats)};
            BOOST_REQUIRE(signature);
            signatures.push_back({seat, *signature});
        }
        flowmesh::BlsMicroblockCertificate certificate;
        BOOST_REQUIRE(flowmesh::AssembleProductionEntryCertificate(
                          entry, producer.seats.seats, signatures, certificate) ==
                      flowmesh::BlsCertificateAssemblyCheck::OK);
        const auto encoded{flowmesh::EncodeProductionCertifiedPayload(
            {entry, certificate}, producer.seats.seats.Size())};
        BOOST_REQUIRE(encoded);
        return *encoded;
    };
    DeliveryRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_duplicate_certificate",
                             node::FlowMeshDeliveryAdmission::ADMITTED, false};
    const auto send = [&](const uint64_t sequence, const std::vector<unsigned char>& bytes) {
        BOOST_REQUIRE(f.runtime->EnqueueWireMessage(DeliveryRuntimeFixture::PEER,
            {flowmesh::WireMessageKind::CERTIFICATE,
             {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, sequence}, bytes}) ==
            flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
    };
    const auto duplicates = [&] {
        const auto events{f.Snapshot().events};
        return std::count_if(events.begin(), events.end(), [](const auto& event) {
            return event.stage == "certificate_duplicate";
        });
    };
    send(0, payload(decoded->entry, {0, 1, 2}));
    send(1, payload(next->entry, {0, 1, 2}));
    BOOST_REQUIRE_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 2U);
    BOOST_REQUIRE_EQUAL(f.Snapshot().durably_applied, 2U);
    const auto verified{f.Snapshot().verified};
    const auto relayed{f.Sent().size()};
    const auto consumed{f.Snapshot().certified_slot_frames_consumed};

    // The durable head and an older durable entry, each once under another
    // valid signer subset and once with a corrupted aggregate: the exact
    // entry is already durable here, so no copy is verified or relayed.
    // Their entry bytes and headers match this runtime's own appends, so
    // admission consumes them before the worker's duplicate check (which
    // still covers copies admission has no record of, e.g. after a restart).
    for (const auto& [sequence, entry] : {std::pair{uint64_t{1}, next->entry}, std::pair{uint64_t{0}, decoded->entry}}) {
        send(sequence, payload(entry, {1, 2, 3}));
        auto corrupted{payload(entry, {0, 1, 2})};
        corrupted.back() ^= 1;
        send(sequence, corrupted);
    }
    BOOST_CHECK_EQUAL(f.Snapshot().certified_slot_frames_consumed, consumed + 4);
    BOOST_CHECK_EQUAL(duplicates(), 0);
    BOOST_CHECK_EQUAL(f.Snapshot().verified, verified);
    BOOST_CHECK_EQUAL(f.Sent().size(), relayed);
    BOOST_CHECK(f.runtime->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);
    BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 2U);
    BOOST_CHECK_EQUAL(f.Snapshot().durably_applied, 2U);

    // A validly certified different entry at a durable sequence is still
    // verified and still halts the market.
    auto conflicting{decoded->entry};
    conflicting.state_root = Filled(0x99);
    BOOST_REQUIRE(conflicting.GetHash() != decoded->entry.GetHash());
    send(0, payload(conflicting, {1, 2, 3}));
    BOOST_CHECK_EQUAL(f.Snapshot().verified, verified + 1);
    BOOST_CHECK(f.runtime->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::CERTIFICATE_CONFLICT);
    BOOST_CHECK_EQUAL(f.Snapshot().durably_applied, 2U);
    BOOST_CHECK_EQUAL(f.Snapshot().certified_slot_frames_consumed, consumed + 4);
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

BOOST_AUTO_TEST_CASE(market_data_reads_ticks_and_copies_curves_on_an_unchanged_head)
{
    DeliveryRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_ticks_unchanged_head",
                             node::FlowMeshDeliveryAdmission::FULL, false};
    f.keys.m_keys.clear(); // An observer applies the certified entry below.
    const auto peer{DeliveryRuntimeFixture::PEER};
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
    BOOST_REQUIRE(f.runtime->RequestCatchup(peer, f.market));
    BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
    const std::vector<std::vector<unsigned char>> entries{*certified};
    const auto payload{flowmesh::EncodeCatchupEntries(entries)};
    BOOST_REQUIRE(payload);
    BOOST_REQUIRE(f.runtime->EnqueueWireMessage(peer,
        {flowmesh::WireMessageKind::ENTRIES,
         {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 0}, *payload}) ==
        flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
    BOOST_REQUIRE_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 1U);

    std::string error;
    // Without the option nothing is read.
    const auto plain{f.runtime->MarketData(f.market, std::nullopt, {}, error)};
    BOOST_REQUIRE(plain);
    BOOST_CHECK(!plain->ticks);
    // A light known_head poll omits the heavy data but still reads ticks.
    flowmesh::MarketDataQuery query;
    query.known_head = genesis->entry.GetHash();
    query.ticks = flowmesh::ClearingTickQuery{};
    const auto light{f.runtime->MarketData(f.market, std::nullopt, query, error)};
    BOOST_REQUIRE(light);
    BOOST_CHECK(light->unchanged);
    BOOST_CHECK(light->history.entries.empty() && light->liquidity.curves.empty());
    BOOST_REQUIRE(light->ticks && light->ticks->available);
    BOOST_REQUIRE_EQUAL(light->ticks->ticks.size(), 1U);
    BOOST_CHECK_EQUAL(light->ticks->ticks[0].sequence, 0U);
    BOOST_CHECK(light->ticks->ticks[0].microblock_hash == genesis->entry.GetHash());
    BOOST_CHECK(light->ticks->ticks[0].kind == flowmesh::ClearingTick::Kind::EXECUTION);
    BOOST_CHECK(!light->ticks->ticks[0].cleared);
    query.ticks->after_sequence = 0;
    const auto current{f.runtime->MarketData(f.market, std::nullopt, query, error)};
    BOOST_REQUIRE(current && current->ticks);
    BOOST_CHECK(current->ticks->available && current->ticks->ticks.empty());
    // Bounds are the local read API's: refused, never clamped.
    for (const size_t limit : {size_t{0}, flowmesh::MARKET_DATA_MAX_TICKS + 1}) {
        query.ticks->limit = limit;
        BOOST_CHECK(!f.runtime->MarketData(f.market, std::nullopt, query, error));
    }

    // Display views: the standing curves are copied once per head (a page
    // per market-lock hold), also for a light poll; nothing is computed there.
    BOOST_CHECK(!plain->book && !light->book);
    BOOST_CHECK_EQUAL(f.runtime->BookCapturesForTest(), 0U);
    // Book levels copy the curves only for a changed head, or when asked for
    // an unchanged one: a light poll that only wants them copies nothing.
    {
        flowmesh::MarketDataQuery levels;
        levels.known_head = genesis->entry.GetHash();
        levels.levels = flowmesh::BookLevelsQuery{};
        const auto quiet{f.runtime->MarketData(f.market, std::nullopt, levels, error)};
        BOOST_REQUIRE(quiet && quiet->unchanged);
        BOOST_CHECK(!quiet->book);
        BOOST_CHECK_EQUAL(f.runtime->BookCapturesForTest(), 0U);
        auto none{*quiet};
        flowmesh::ComputeMarketViews(none, levels);
        BOOST_CHECK(!none.levels);
        levels.levels->include_unchanged = true;
        const auto asked{f.runtime->MarketData(f.market, std::nullopt, levels, error)};
        BOOST_REQUIRE(asked && asked->book);
        auto computed_levels{*asked};
        flowmesh::ComputeMarketViews(computed_levels, levels);
        BOOST_REQUIRE(computed_levels.levels);
        BOOST_CHECK(computed_levels.levels->available);
        BOOST_CHECK(computed_levels.levels->head == genesis->entry.GetHash());
        BOOST_CHECK(computed_levels.levels->standing.head == genesis->entry.GetHash());
        BOOST_CHECK(computed_levels.levels->bids.levels.empty() && computed_levels.levels->asks.levels.empty());
        // A changed head (no known_head) always carries them.
        levels.known_head.reset();
        levels.levels->include_unchanged = false;
        const auto full{f.runtime->MarketData(f.market, std::nullopt, levels, error)};
        BOOST_REQUIRE(full && full->book);
        BOOST_CHECK(full->book == asked->book); // One copy per head.
        // Refused, never clamped: status_only, depth and grouping bounds.
        levels.status_only = true;
        BOOST_CHECK(!f.runtime->MarketData(f.market, std::nullopt, levels, error));
        levels.status_only = false;
        for (const size_t depth : {size_t{0}, flowmesh::MARKET_DATA_MAX_LEVELS + 1}) {
            levels.levels->depth = depth;
            BOOST_CHECK(!f.runtime->MarketData(f.market, std::nullopt, levels, error));
        }
        levels.levels->depth = 1;
        levels.levels->grouping.exact = false;
        levels.levels->grouping.mantissa = 3;
        BOOST_CHECK(!f.runtime->MarketData(f.market, std::nullopt, levels, error));
        // Drop this head's copy again for the checks below.
        f.Tick(node::FlowMeshRuntime::BOOK_CAPTURE_IDLE);
        BOOST_REQUIRE(f.runtime->MarketData(f.market, std::nullopt, {}, error));
        BOOST_CHECK_EQUAL(f.runtime->BookCapturesForTest(), 0U);
    }
    flowmesh::MarketDataQuery views;
    views.known_head = genesis->entry.GetHash();
    views.indicative = flowmesh::IndicativeQuery{};
    const auto first{f.runtime->MarketData(f.market, std::nullopt, views, error)};
    BOOST_REQUIRE(first && first->book);
    BOOST_CHECK(first->unchanged && !first->indicative);
    BOOST_CHECK(first->book->head == genesis->entry.GetHash());
    BOOST_CHECK(first->book->curves.empty());
    const auto again{f.runtime->MarketData(f.market, std::nullopt, views, error)};
    BOOST_REQUIRE(again);
    BOOST_CHECK(again->book == first->book);
    BOOST_CHECK_EQUAL(f.runtime->BookCapturesForTest(), 1U);
    auto computed{*again};
    flowmesh::ComputeMarketViews(computed, views);
    BOOST_CHECK(!computed.book);
    BOOST_REQUIRE(computed.indicative);
    BOOST_CHECK(computed.indicative->available && !computed.indicative->cleared);
    BOOST_CHECK(computed.indicative->head == genesis->entry.GetHash());

    // A new certified head replaces the copy.
    const auto next{flowmesh::BuildProductionExecutionEntry(
        genesis->next_state, f.domain, f.market, f.seats.seats, gate,
        1, genesis->entry.effect_count, genesis->entry.GetHash(), f.chain.Current(),
        {f.chain.TipHeight(), genesis->entry.anchor, &f.chain}, Filled(0x6b), {}, nullptr, check)};
    BOOST_REQUIRE(next);
    const auto next_payload{flowmesh::EncodeProductionCertifiedPayload(
        {next->entry, Certify(next->entry, f.seats)}, f.seats.seats.Size())};
    BOOST_REQUIRE(next_payload);
    f.Tick(std::chrono::seconds{16});
    BOOST_REQUIRE(f.runtime->RequestCatchup(peer, f.market));
    BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
    const auto next_page{flowmesh::EncodeCatchupEntries(std::vector<std::vector<unsigned char>>{*next_payload})};
    BOOST_REQUIRE(next_page);
    BOOST_REQUIRE(f.runtime->EnqueueWireMessage(peer,
        {flowmesh::WireMessageKind::ENTRIES, f.Requests().back().message.header, *next_page}) ==
        flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
    BOOST_REQUIRE_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 2U);
    // Publication drops the current cache slot for the old head immediately;
    // the caller's retained immutable first->book remains valid.
    BOOST_CHECK_EQUAL(f.runtime->BookCapturesForTest(), 0U);
    BOOST_REQUIRE(f.runtime->MarketData(f.market, std::nullopt, {}, error));
    BOOST_CHECK_EQUAL(f.runtime->BookCapturesForTest(), 0U);
    const auto moved{f.runtime->MarketData(f.market, std::nullopt, views, error)};
    BOOST_REQUIRE(moved && moved->book);
    BOOST_CHECK(!moved->unchanged);
    BOOST_CHECK(moved->book != first->book);
    BOOST_CHECK(moved->book->head == next->entry.GetHash());
    BOOST_CHECK(moved->book->unavailable_reason.empty());
    BOOST_CHECK_EQUAL(f.runtime->BookCapturesForTest(), 1U);
    // ... and one no view has used for BOOK_CAPTURE_IDLE.
    f.Tick(node::FlowMeshRuntime::BOOK_CAPTURE_IDLE - std::chrono::seconds{1});
    BOOST_REQUIRE(f.runtime->MarketData(f.market, std::nullopt, {}, error));
    BOOST_CHECK_EQUAL(f.runtime->BookCapturesForTest(), 1U);
    f.Tick(std::chrono::seconds{1});
    BOOST_REQUIRE(f.runtime->MarketData(f.market, std::nullopt, {}, error));
    BOOST_CHECK_EQUAL(f.runtime->BookCapturesForTest(), 0U);
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
    // This node's own API refuses a deposit without anchored facts, so the
    // unknown deposit arrives the way it still can: relayed by a peer.
    {
        const auto head{runtimes[0]->MarketStatus(market_a)};
        BOOST_REQUIRE(head);
        BOOST_REQUIRE(runtimes[0]->EnqueueWireMessage(99, {flowmesh::WireMessageKind::ACTION,
            {flowmesh::FLOWMESH_WIRE_VERSION_V1, market_a, head->epoch, head->next_sequence},
            *flowmesh::EncodeProductionActionPayload(Deposit(rejected_deposit))}) ==
                      flowmesh::QueueResult::ACCEPTED);
    }
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
    // (A relayed deposit: this node's own API refuses fact-less deposits.)
    {
        const auto head{runtime.MarketStatus(market)};
        BOOST_REQUIRE(head);
        BOOST_REQUIRE(runtime.EnqueueWireMessage(99, {flowmesh::WireMessageKind::ACTION,
            {flowmesh::FLOWMESH_WIRE_VERSION_V1, market, head->epoch, head->next_sequence},
            *flowmesh::EncodeProductionActionPayload(Deposit(COutPoint{Txid::FromUint256(Filled(0x91)), 0}))}) ==
                      flowmesh::QueueResult::ACCEPTED);
    }
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

/** One observer runtime and a peer that serves verified certificates. Every
 * health check advances the fixed clock past the 250 ms publish interval. */
class HealthRuntimeFixture
{
public:
    explicit HealthRuntimeFixture(const fs::path& path,
                                  std::shared_ptr<node::FlowMeshObservedTimeLog> observed = nullptr,
                                  const size_t entry_count = 3,
                                  const node::FlowMeshRecentWindowLimits recent = {})
        : store{DBParams{.path = path, .cache_bytes = size_t{1} << 20, .wipe_data = true}}
    {
        chain.m_domain = domain;
        chain.Add(seats.seats);
        std::string error;
        BOOST_REQUIRE_MESSAGE(store.OpenForMarket(domain, market, seats.seats, initial.Root(), error), error);
        node::FlowMeshRuntimeConfig config;
        config.chain = &chain;
        config.keys = &keys;
        config.clock = &clock;
        config.recent_window = recent;
        config.relay = [this](node::FlowMeshRuntimeRelay relay) {
            std::unique_lock<std::mutex> lock{relay_mutex};
            if (relay.message.kind == flowmesh::WireMessageKind::GET) {
                gets.push_back(relay);
            }
            if ((relay.message.kind == flowmesh::WireMessageKind::GET && block_gets) ||
                (relay.message.kind == flowmesh::WireMessageKind::CERTIFICATE && block_certificates)) {
                relay_blocked = true;
                relay_cv.notify_all();
                relay_cv.wait(lock, [&] { return !block_gets && !block_certificates; });
            }
            return LegacyRelayResult();
        };
        auto market_config{MarketConfig(domain, market, treasury, seats.seats, initial, store, nullptr)};
        market_config.observed = std::move(observed);
        runtime = std::make_unique<node::FlowMeshRuntime>(config, std::vector<node::FlowMeshRuntimeMarketConfig>{
            std::move(market_config)});
        flowmesh::ProductionEpochGate gate{domain, market, seats.seats};
        flowmesh::ProductionEntryCheck check;
        std::optional<flowmesh::FlowMeshState> state{initial};
        uint256 parent;
        std::optional<flowmesh::AnchorRef> previous;
        for (uint64_t sequence{0}; sequence < entry_count; ++sequence) {
            const auto built{flowmesh::BuildProductionExecutionEntry(
                *state, domain, market, seats.seats, gate, sequence, 0, parent, {100, Filled(0x71)},
                {chain.TipHeight(), previous, &chain}, treasury, {}, nullptr, check)};
            BOOST_REQUIRE_MESSAGE(built, flowmesh::ProductionEntryCheckName(check));
            entries.push_back(built->entry);
            const auto payload{flowmesh::EncodeProductionCertifiedPayload(
                {built->entry, Certify(built->entry, seats)}, seats.seats.Size())};
            BOOST_REQUIRE(payload);
            payloads.push_back(*payload);
            state = built->next_state;
            parent = built->entry.GetHash();
            previous = built->entry.anchor;
        }
    }
    ~HealthRuntimeFixture()
    {
        {
            std::lock_guard<std::mutex> lock{relay_mutex};
            block_gets = false;
            block_certificates = false;
        }
        relay_cv.notify_all();
        runtime->Stop();
    }

    //! Settle the worker, then publish a fresh observation.
    std::shared_ptr<const node::FlowMeshRuntimeHealthSnapshot> Health()
    {
        BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
        clock.m_now += std::chrono::milliseconds{250};
        runtime->NotifyTick();
        BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
        const auto health{runtime->HealthSnapshot()};
        BOOST_REQUIRE(health);
        BOOST_REQUIRE(health->published_at == clock.m_now);
        BOOST_REQUIRE_EQUAL(health->markets.size(), 1U);
        return health;
    }
    const node::FlowMeshLagSummary& Lag(const std::shared_ptr<const node::FlowMeshRuntimeHealthSnapshot>& health)
    {
        return health->markets.front().lag;
    }
    void Send(const flowmesh::WirePeerId peer, flowmesh::WireMessage message)
    {
        BOOST_REQUIRE(runtime->EnqueueWireMessage(peer, std::move(message)) == flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(runtime->WaitForIdle(std::chrono::seconds{2}));
    }
    void Hello(const flowmesh::WirePeerId peer, const uint64_t next)
    {
        Send(peer, {flowmesh::WireMessageKind::HELLO, {flowmesh::FLOWMESH_WIRE_VERSION_V1, market, seats.seats.epoch, next},
                    flowmesh::EncodeMarketHello({domain, next == 0 ? uint256{} : Filled(0x7a)})});
    }
    void Certificate(const flowmesh::WirePeerId peer, const uint64_t sequence)
    {
        Send(peer, {flowmesh::WireMessageKind::CERTIFICATE,
                    {flowmesh::FLOWMESH_WIRE_VERSION_V1, market, seats.seats.epoch, sequence}, payloads.at(sequence)});
    }
    //! A catch-up reply to the pending request from `from`.
    void Entries(const flowmesh::WirePeerId peer, const uint64_t from, std::vector<std::vector<unsigned char>> page)
    {
        const auto encoded{flowmesh::EncodeCatchupEntries(page)};
        BOOST_REQUIRE(encoded);
        Send(peer, {flowmesh::WireMessageKind::ENTRIES, {flowmesh::FLOWMESH_WIRE_VERSION_V1, market, seats.seats.epoch, from},
                    *encoded});
    }
    size_t Gets(const flowmesh::WirePeerId peer)
    {
        std::lock_guard<std::mutex> lock{relay_mutex};
        return std::count_if(gets.begin(), gets.end(), [&](const auto& relay) { return relay.peer == peer; });
    }

    const uint256 domain{Filled(0x1c)};
    const modern::AssetId asset{Filled(0x3c)};
    const flowmesh::MarketId market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const flowmesh::VaultId vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x5c)};
    const SeatFixture seats{Seats(domain, market, 4, 7, 100, Filled(0x71), 140)};
    const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(), flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    RuntimeChain chain;
    RuntimeKeys keys; // Observer.
    FixedClock clock;
    node::FlowMeshProductionStore store;
    std::vector<flowmesh::ProductionEntryCore> entries;
    std::vector<std::vector<unsigned char>> payloads;
    std::mutex relay_mutex;
    std::condition_variable relay_cv;
    bool block_gets{false}, block_certificates{false}, relay_blocked{false};
    std::vector<node::FlowMeshRuntimeRelay> gets;
    std::unique_ptr<node::FlowMeshRuntime> runtime;
};

BOOST_AUTO_TEST_CASE(health_snapshot_publishes_head_pause_reason_peers_and_halt)
{
    HealthRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_health_snapshot"};
    BOOST_CHECK(!f.runtime->HealthSnapshot());
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);
    // Start publishes at once: a missing observation always means no Start.
    const auto started{f.runtime->HealthSnapshot()};
    BOOST_REQUIRE(started);
    BOOST_CHECK(started->published_at == f.clock.m_now);
    BOOST_CHECK_EQUAL(started->connected_peers, 0U);
    BOOST_CHECK(started->no_peers_since == f.clock.m_now);
    BOOST_REQUIRE_EQUAL(started->markets.size(), 1U);
    const auto& row{started->markets.front()};
    BOOST_CHECK(row.market_id == f.market);
    BOOST_CHECK(row.installed);
    BOOST_CHECK_EQUAL(row.epoch, 7U);
    BOOST_CHECK_EQUAL(row.next_sequence, 0U);
    BOOST_CHECK_EQUAL(row.active_seats, 4U);
    BOOST_CHECK_EQUAL(row.quorum_required, 3U);
    BOOST_CHECK(!row.halted);
    BOOST_CHECK_EQUAL(row.halt, "none");
    BOOST_CHECK(!row.head_advanced_at);
    BOOST_CHECK(!row.lag.behind_since);

    // Ticks inside the publish interval do not copy the rows again.
    for (int i{0}; i < 10; ++i) f.runtime->NotifyTick();
    BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK(f.runtime->HealthSnapshot() == started);

    f.chain.SetPausedTransition(f.market, "handoff_checkpoints_pending");
    auto health{f.Health()};
    BOOST_CHECK(health->markets.front().paused);
    BOOST_CHECK_EQUAL(health->markets.front().pause_reason, "handoff_checkpoints_pending");
    f.chain.SetTransition(f.market, node::FlowMeshSeatTransitionKind::CONTINUE);
    health = f.Health();
    BOOST_CHECK(!health->markets.front().paused);
    BOOST_CHECK_EQUAL(health->markets.front().pause_reason, "");

    f.runtime->FlowMeshPeerConnected(5);
    health = f.Health();
    BOOST_CHECK_EQUAL(health->connected_peers, 1U);
    BOOST_CHECK(!health->no_peers_since);
    f.runtime->FlowMeshPeerDisconnected(5);
    const auto disconnected{f.clock.m_now};
    health = f.Health();
    BOOST_CHECK_EQUAL(health->connected_peers, 0U);
    BOOST_CHECK(health->no_peers_since == disconnected);

    // A verified certificate committed at our next sequence advances the
    // head, observed on the local steady clock.
    f.Certificate(9, 0);
    health = f.Health();
    BOOST_CHECK_EQUAL(health->markets.front().next_sequence, 1U);
    BOOST_CHECK(health->markets.front().last_hash == f.entries[0].GetHash());
    BOOST_CHECK(health->markets.front().head_advanced_at == f.clock.m_now - std::chrono::milliseconds{250});
    BOOST_CHECK(health->markets.front().local_observed_at);

    // A conflicting, validly certified entry at a durable sequence halts.
    auto conflicting{f.entries[0]};
    conflicting.state_root = Filled(0x99);
    const auto conflict{flowmesh::EncodeProductionCertifiedPayload(
        {conflicting, Certify(conflicting, f.seats)}, f.seats.seats.Size())};
    BOOST_REQUIRE(conflict);
    f.Send(9, {flowmesh::WireMessageKind::CERTIFICATE,
               {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 0}, *conflict});
    health = f.Health();
    BOOST_CHECK(health->markets.front().halted);
    BOOST_CHECK_EQUAL(health->markets.front().halt, "certificate-conflict");
    BOOST_CHECK(!health->markets.front().error.empty());
}

BOOST_AUTO_TEST_CASE(health_lag_is_proven_only_by_verified_certificates_and_speculative_pages)
{
    HealthRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_health_lag"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);

    // An unverified HELLO far ahead is shown as a hint only, and the catch-up
    // it triggers is unchanged (one GET).
    f.Hello(8, 999);
    auto health{f.Health()};
    BOOST_CHECK(!f.Lag(health).behind_since);
    BOOST_CHECK(!f.Lag(health).proven_target);
    BOOST_CHECK(!f.Lag(health).speculative);
    BOOST_CHECK_EQUAL(f.Lag(health).peers_hinting_ahead, 1U);
    BOOST_CHECK_EQUAL(*f.Lag(health).peer_hint_next_sequence, 999U);
    BOOST_CHECK_EQUAL(f.Gets(8), 1U);

    // A BLS-verified certificate for sequence 1 while we are at 0 proves lag.
    f.Certificate(9, 1);
    health = f.Health();
    const auto behind_since{f.clock.m_now - std::chrono::milliseconds{250}};
    BOOST_CHECK(f.Lag(health).behind_since == behind_since);
    BOOST_CHECK(f.Lag(health).proven_since == behind_since);
    BOOST_CHECK_EQUAL(*f.Lag(health).proven_target, 2U);
    BOOST_CHECK_EQUAL(f.Gets(9), 1U);
    // A catch-up page of one entry with tail slack advances the head but not
    // past the proven target; the episode keeps its start.
    f.Entries(9, 0, {f.payloads[0]});
    health = f.Health();
    BOOST_CHECK_EQUAL(health->markets.front().next_sequence, 1U);
    BOOST_CHECK(f.Lag(health).behind_since == behind_since);
    BOOST_CHECK_EQUAL(*f.Lag(health).proven_target, 2U);
    BOOST_CHECK(!f.Lag(health).speculative);
    // No announcement at or below our head, from any peer, clears it.
    f.Hello(9, 1);
    f.Hello(8, 1);
    health = f.Health();
    BOOST_CHECK(f.Lag(health).behind_since == behind_since);
    // Committing the proven sequence ends it.
    f.Certificate(9, 1);
    health = f.Health();
    BOOST_CHECK_EQUAL(health->markets.front().next_sequence, 2U);
    BOOST_CHECK(!f.Lag(health).behind_since);
    BOOST_CHECK(!f.Lag(health).proven_target);

    // A page whose valid prefix is followed by an invalid entry has no tail
    // slack: speculative lag, which only its serving peer's announcement at
    // or below our head (or a timeout) ends.
    f.Hello(10, 5);
    BOOST_REQUIRE_EQUAL(f.Gets(10), 1U);
    f.Entries(10, 2, {f.payloads[2], {0x42}});
    health = f.Health();
    BOOST_CHECK_EQUAL(health->markets.front().next_sequence, 3U);
    BOOST_CHECK(f.Lag(health).speculative);
    BOOST_CHECK(f.Lag(health).behind_since);
    BOOST_CHECK(!f.Lag(health).proven_since);
    BOOST_CHECK(!f.Lag(health).proven_target);
    f.clock.m_now += std::chrono::seconds{1}; // Refill the control budget.
    f.Hello(11, 3);
    health = f.Health();
    BOOST_CHECK(f.Lag(health).speculative);
    f.Hello(10, 3);
    health = f.Health();
    BOOST_CHECK(!f.Lag(health).speculative);
    BOOST_CHECK(!f.Lag(health).behind_since);
    // Peer 8's hint ended with its announcement at our head. A new hint
    // expires when it is not repeated.
    BOOST_CHECK_EQUAL(f.Lag(health).peers_hinting_ahead, 0U);
    f.Hello(12, 50);
    BOOST_CHECK_EQUAL(f.Lag(f.Health()).peers_hinting_ahead, 1U);
    f.clock.m_now += node::FlowMeshLagTracker::HINT_TTL;
    BOOST_CHECK_EQUAL(f.Lag(f.Health()).peers_hinting_ahead, 0U);
    BOOST_CHECK(!f.Lag(f.Health()).behind_since);
}

//! Hand-driven wall and steady clocks for an observed-time log.
struct ObservedTestClock {
    std::atomic<int64_t> wall_ms{1790769600000};
    std::atomic<int64_t> steady_ms{1'000'000};
    node::FlowMeshObservedTimeLog::Clocks Get()
    {
        return {[this] { return wall_ms.load(); },
                [this] { return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds{steady_ms.load()}; }};
    }
};

std::shared_ptr<node::FlowMeshObservedTimeLog> ObservedLogFor(ObservedTestClock& clock)
{
    const uint256 domain{Filled(0x1c)};
    return std::make_shared<node::FlowMeshObservedTimeLog>(
        domain, *flowmesh::ComputeFlowMeshMarketId(domain, modern::AssetId{Filled(0x3c)}), clock.Get());
}

//! Health() ticks until the observed-time warm-up (4 s at the default 2 s
//! round timeout) has passed with a connected peer.
void WarmObservedTime(HealthRuntimeFixture& f, ObservedTestClock& clock)
{
    f.runtime->FlowMeshPeerConnected(5);
    for (int i{0}; i < 24; ++i) {
        f.Health();
        clock.wall_ms += 250;
        clock.steady_ms += 250;
    }
}

flowmesh::ObservedStamp ObservedAt(const node::FlowMeshObservedTimeLog& log, const HealthRuntimeFixture& f, const uint64_t sequence)
{
    const auto rows{log.Lookup({{sequence, f.entries.at(sequence).GetHash()}})};
    BOOST_REQUIRE_EQUAL(rows.size(), 1U);
    return rows.front();
}

BOOST_AUTO_TEST_CASE(observed_time_is_recorded_after_commit_with_its_liveness)
{
    ObservedTestClock clock;
    const auto log{ObservedLogFor(clock)};
    HealthRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_observed_live", log, 5};
    // The entries' own anchor is the current acceptable one.
    f.chain.m_current = f.entries.front().anchor;
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);

    // Right after start nothing is warm: an old certificate relayed now must
    // not be stamped live. It is recorded, with only an upper bound.
    f.Certificate(9, 0);
    auto stamp{ObservedAt(*log, f, 0)};
    BOOST_CHECK(stamp.mode == flowmesh::ObservedMode::DELAYED);
    BOOST_CHECK(stamp.delay == flowmesh::ObservedDelay::WARMUP);
    BOOST_CHECK(stamp.flags & flowmesh::OBSERVED_RESTART);
    BOOST_CHECK(f.Health()->markets.front().local_observed_at);

    // Caught up with a peer for the warm-up: live, at this node's clock.
    WarmObservedTime(f, clock);
    const int64_t applied{clock.wall_ms.load()};
    f.Certificate(9, 1);
    stamp = ObservedAt(*log, f, 1);
    BOOST_CHECK(stamp.mode == flowmesh::ObservedMode::LIVE);
    BOOST_CHECK_EQUAL(stamp.ms, applied);
    // The stamp carries the entry's own clearing (none here).
    BOOST_CHECK(!(stamp.flags & flowmesh::OBSERVED_NO_CLEARING));

    // A verified certificate for a later sequence proves this node behind;
    // the catch-up page that follows is recorded late.
    f.Certificate(9, 3);
    BOOST_REQUIRE_GE(f.Gets(9), 1U);
    f.Entries(9, 2, {f.payloads[2]});
    stamp = ObservedAt(*log, f, 2);
    BOOST_CHECK(stamp.mode == flowmesh::ObservedMode::DELAYED);
    BOOST_CHECK(stamp.delay == flowmesh::ObservedDelay::CATCHUP);
    // The held certificate is for the very next entry: committing it, this
    // node is level again, and a lag shorter than a round kept the warm-up.
    f.Certificate(9, 3);
    BOOST_CHECK(ObservedAt(*log, f, 3).mode == flowmesh::ObservedMode::LIVE);
    // A peer level with the entry being committed is not ahead of it.
    f.Hello(8, 5);
    f.Certificate(9, 4);
    BOOST_CHECK(ObservedAt(*log, f, 4).mode == flowmesh::ObservedMode::LIVE);
    // Candles hold only what was applied live (or late inside one bucket).
    const auto page{log->Candles(flowmesh::CandleInterval::H1, std::nullopt, 10)};
    BOOST_REQUIRE_EQUAL(page.candles.size(), 1U);
    BOOST_CHECK_GE(page.candles.front().entries, 2U);
}

BOOST_AUTO_TEST_CASE(observed_time_warmup_starts_within_a_tick_of_catching_up)
{
    // The warm-up (4 s at the default 2 s round timeout) ends one period and
    // at most a tick after the node catches up, not up to a chain-gate
    // sample (1 s) later: here the node catches up between two samples,
    // once by its chain gate opening (as after a start) and once by a peer
    // connecting while the gate is open.
    const auto warmup{flowmesh::ObservedWarmupFor(std::chrono::milliseconds{2000}) + std::chrono::milliseconds{250}};
    for (const bool by_gate : {true, false}) {
        BOOST_TEST_CONTEXT("by_gate=" << by_gate) {
            ObservedTestClock clock;
            const auto log{ObservedLogFor(clock)};
            HealthRuntimeFixture f{m_args.GetDataDirBase() / (by_gate ? "flowmesh_observed_warmup_gate" : "flowmesh_observed_warmup_peer"), log, 3};
            f.chain.m_current = f.entries.front().anchor;
            std::string error;
            BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);
            if (by_gate) {
                f.runtime->FlowMeshPeerConnected(5);
                f.chain.SetReconciled(false);
            }
            // A tick samples the chain gate; this observer is not caught up.
            f.Health();
            if (by_gate) {
                f.chain.SetReconciled(true);
            } else {
                f.runtime->FlowMeshPeerConnected(5);
            }
            const auto caught_up{f.clock.m_now};
            while (f.clock.m_now < caught_up + warmup) f.Health();
            f.Certificate(9, 0);
            const auto stamp{ObservedAt(*log, f, 0)};
            BOOST_CHECK(stamp.mode == flowmesh::ObservedMode::LIVE);
            BOOST_CHECK_MESSAGE(stamp.delay == flowmesh::ObservedDelay::NONE, flowmesh::ObservedDelayName(stamp.delay));
        }
    }
}

BOOST_AUTO_TEST_CASE(observed_time_ignores_an_unverified_peer_hint)
{
    ObservedTestClock clock;
    const auto log{ObservedLogFor(clock)};
    HealthRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_observed_hint", log, 4};
    f.chain.m_current = f.entries.front().anchor;
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);
    // One peer announces a head far ahead and keeps it alive (a hint lasts
    // HINT_TTL). Unverified, it is shown as a hint but neither holds back
    // the warm-up nor delays a stamp: any one peer could otherwise keep this
    // node's times delayed for as long as it keeps saying so.
    f.Hello(8, 999);
    WarmObservedTime(f, clock);
    const auto health{f.Health()};
    BOOST_CHECK_EQUAL(f.Lag(health).peers_hinting_ahead, 1U);
    BOOST_CHECK_EQUAL(f.Lag(health).peer_hint_next_sequence.value_or(0), 999U);
    BOOST_CHECK(!f.Lag(health).proven_target);
    for (const uint64_t sequence : {0U, 1U}) {
        f.Certificate(9, sequence);
        const auto stamp{ObservedAt(*log, f, sequence)};
        BOOST_CHECK(stamp.mode == flowmesh::ObservedMode::LIVE);
        BOOST_CHECK_MESSAGE(stamp.delay == flowmesh::ObservedDelay::NONE, flowmesh::ObservedDelayName(stamp.delay));
    }
    BOOST_CHECK_EQUAL(f.Lag(f.Health()).peers_hinting_ahead, 1U);
    // A verified certificate beyond the head still proves the lag: the
    // catch-up that follows is recorded late.
    f.Certificate(9, 3);
    BOOST_CHECK(f.Lag(f.Health()).proven_target);
    BOOST_REQUIRE_GE(f.Gets(9), 1U);
    f.Entries(9, 2, {f.payloads[2]});
    BOOST_CHECK(ObservedAt(*log, f, 2).mode == flowmesh::ObservedMode::DELAYED);
}

BOOST_AUTO_TEST_CASE(observed_time_marks_a_reconciliation_deferred_certificate_late)
{
    ObservedTestClock clock;
    const auto log{ObservedLogFor(clock)};
    HealthRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_observed_deferred", log, 3};
    f.chain.m_current = f.entries.front().anchor;
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);
    WarmObservedTime(f, clock);

    // The certificate waits for B3 reconciliation, then commits: late.
    f.chain.SetReconciled(false);
    f.Certificate(9, 0);
    BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 0U);
    f.chain.SetReconciled(true);
    f.Health();
    f.Health();
    BOOST_REQUIRE_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 1U);
    const auto stamp{ObservedAt(*log, f, 0)};
    BOOST_CHECK(stamp.mode == flowmesh::ObservedMode::DELAYED);
    BOOST_CHECK(stamp.delay == flowmesh::ObservedDelay::DEFERRED);
    // A brief closure does not restart the warm-up: the next one is live.
    f.Certificate(9, 1);
    BOOST_CHECK(ObservedAt(*log, f, 1).mode == flowmesh::ObservedMode::LIVE);
}

BOOST_AUTO_TEST_CASE(observed_time_in_market_data_only_when_requested)
{
    ObservedTestClock clock;
    const auto log{ObservedLogFor(clock)};
    HealthRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_observed_market_data", log, 3};
    f.chain.m_current = f.entries.front().anchor;
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);
    WarmObservedTime(f, clock);
    f.Certificate(9, 0);
    clock.wall_ms += 400;
    f.Certificate(9, 1);

    flowmesh::MarketDataQuery query;
    query.ticks = flowmesh::ClearingTickQuery{std::nullopt, 16};
    const auto plain{f.runtime->MarketData(f.market, std::nullopt, query, error)};
    BOOST_REQUIRE_MESSAGE(plain, error);
    BOOST_CHECK(!plain->observed);
    const auto plain_json{node::FlowMeshClientMarketDataJson(*plain).write()};
    BOOST_CHECK(plain_json.find("observed_time") == std::string::npos);

    query.observed_time = true;
    auto timed{f.runtime->MarketData(f.market, std::nullopt, query, error)};
    BOOST_REQUIRE_MESSAGE(timed, error);
    BOOST_REQUIRE(timed->observed);
    const auto& section{*timed->observed};
    BOOST_CHECK(section.available);
    BOOST_CHECK_EQUAL(section.source, "this_node");
    BOOST_CHECK_EQUAL(section.recorded_since_sequence.value_or(99), 0U);
    // Exactly the reply's entries (head, history and ticks), once each.
    BOOST_REQUIRE_EQUAL(section.rows.size(), 2U);
    BOOST_CHECK_EQUAL(section.rows[0].sequence, 0U);
    BOOST_CHECK(section.rows[0].hash == f.entries[0].GetHash());
    BOOST_CHECK(section.rows[1].mode == flowmesh::ObservedMode::LIVE);
    BOOST_CHECK_EQUAL(section.rows[1].ms - section.rows[0].ms, 400);
    // Everything else in the reply is byte for byte the same.
    const auto timed_json{node::FlowMeshClientMarketDataJson(*timed)};
    BOOST_CHECK(timed_json["observed_time"]["rows"][1]["observed_ms"].getInt<int64_t>() == section.rows[1].ms);
    BOOST_CHECK_EQUAL(timed_json["observed_time"]["certified"].get_bool(), false);
    BOOST_CHECK_EQUAL(timed_json["observed_time"]["source_kind"].get_str(), "operator_observed");
    // This node's own section carries its storage detail (operator only).
    BOOST_REQUIRE(section.storage);
    BOOST_CHECK(section.storage->persisted);
    BOOST_CHECK_EQUAL(section.storage->rows_not_persisted, 0U);
    BOOST_CHECK_EQUAL(section.storage->candles_not_persisted, 0U);
    BOOST_CHECK(timed_json["observed_time"]["storage"]["persisted"].get_bool());
    timed->observed.reset();
    BOOST_CHECK_EQUAL(node::FlowMeshClientMarketDataJson(*timed).write(), plain_json);

    // An unchanged light poll still carries the head's time and its ticks'.
    flowmesh::MarketDataQuery light;
    light.known_head = f.entries[1].GetHash();
    light.observed_time = true;
    const auto unchanged{f.runtime->MarketData(f.market, std::nullopt, light, error)};
    BOOST_REQUIRE_MESSAGE(unchanged, error);
    BOOST_CHECK(unchanged->unchanged);
    BOOST_REQUIRE(unchanged->observed);
    BOOST_REQUIRE_EQUAL(unchanged->observed->rows.size(), 1U);
    BOOST_CHECK_EQUAL(unchanged->observed->rows[0].sequence, 1U);
    BOOST_CHECK(node::FlowMeshClientMarketDataJson(*unchanged).exists("observed_time"));

    // A status-only read never carries it.
    flowmesh::MarketDataQuery status;
    status.status_only = true;
    status.observed_time = true;
    BOOST_CHECK(!f.runtime->MarketData(f.market, std::nullopt, status, error));
}

BOOST_AUTO_TEST_CASE(observed_time_throwing_clock_never_affects_the_commit)
{
    auto log{std::make_shared<node::FlowMeshObservedTimeLog>(
        Filled(0x1c), *flowmesh::ComputeFlowMeshMarketId(Filled(0x1c), modern::AssetId{Filled(0x3c)}),
        node::FlowMeshObservedTimeLog::Clocks{[]() -> int64_t { throw std::runtime_error{"clock"}; }, {}})};
    HealthRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_observed_throw", log, 3};
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);
    f.Certificate(9, 0);
    f.Certificate(9, 1);
    BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 2U);
    BOOST_CHECK(f.runtime->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);
    BOOST_CHECK_EQUAL(log->GetSummary().record_failures, 2U);
    BOOST_CHECK(f.Health()->markets.front().local_observed_at);
}

BOOST_AUTO_TEST_CASE(health_snapshot_includes_an_added_market_before_add_returns)
{
    const uint256 domain{Filled(0x1d)};
    const modern::AssetId asset{Filled(0x3d)};
    const auto market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    const auto vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const SeatFixture seats{Seats(domain, market, 4, 7, 100, Filled(0x71), 150)};
    RuntimeChain chain;
    chain.m_domain = domain;
    chain.Add(seats.seats);
    RuntimeKeys keys;
    FixedClock clock;
    const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(), flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    node::FlowMeshProductionStore store{DBParams{
        .path = m_args.GetDataDirBase() / "flowmesh_health_added_market",
        .cache_bytes = size_t{1} << 20, .wipe_data = true}};
    std::string error;
    BOOST_REQUIRE_MESSAGE(store.OpenForMarket(domain, market, seats.seats, initial.Root(), error), error);
    node::FlowMeshRuntimeConfig config;
    config.chain = &chain;
    config.keys = &keys;
    config.clock = &clock;
    config.relay = [](node::FlowMeshRuntimeRelay) { return LegacyRelayResult(); };
    node::FlowMeshRuntime runtime{config, {}};
    BOOST_REQUIRE_MESSAGE(runtime.Start(error), error);
    BOOST_REQUIRE(runtime.HealthSnapshot());
    BOOST_CHECK(runtime.HealthSnapshot()->markets.empty());
    // No tick and no clock movement: AddMarket itself publishes.
    BOOST_REQUIRE_MESSAGE(runtime.AddMarket(MarketConfig(domain, market, Filled(0x5d), seats.seats, initial, store, nullptr),
                                            error), error);
    const auto health{runtime.HealthSnapshot()};
    BOOST_REQUIRE_EQUAL(health->markets.size(), 1U);
    BOOST_CHECK(health->markets.front().market_id == market);
    BOOST_CHECK(health->markets.front().installed);
    runtime.Stop();
}

BOOST_AUTO_TEST_CASE(health_snapshot_is_readable_while_the_worker_is_blocked)
{
    HealthRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_health_blocked"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);
    const auto before{f.Health()};
    {
        std::lock_guard<std::mutex> lock{f.relay_mutex};
        f.block_gets = true;
    }
    // The HELLO's catch-up request blocks the worker inside the relay
    // callback, holding the market lock every ordinary read waits for.
    BOOST_REQUIRE(f.runtime->EnqueueWireMessage(8, {flowmesh::WireMessageKind::HELLO,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 4},
        flowmesh::EncodeMarketHello({f.domain, Filled(0x7a)})}) == flowmesh::QueueResult::ACCEPTED);
    {
        std::unique_lock<std::mutex> lock{f.relay_mutex};
        BOOST_REQUIRE(f.relay_cv.wait_for(lock, std::chrono::seconds{5}, [&] { return f.relay_blocked; }));
    }
    const auto start{std::chrono::steady_clock::now()};
    const auto health{f.runtime->HealthSnapshot()};
    BOOST_CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds{100});
    BOOST_REQUIRE(health);
    // The observation's age, not a blocked read, reports the stalled worker.
    BOOST_CHECK(health->published_at == before->published_at);
    {
        std::lock_guard<std::mutex> lock{f.relay_mutex};
        f.block_gets = false;
    }
    f.relay_cv.notify_all();
    BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
}

BOOST_AUTO_TEST_CASE(health_snapshot_is_readable_while_chain_lock_is_held)
{
    HealthRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_health_chain_blocked"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);
    const auto before{f.Health()};

    std::promise<void> held;
    auto held_future{held.get_future()};
    std::promise<void> read_completed;
    auto read_completed_future{read_completed.get_future()};
    bool observed_before_release{false};
    JoiningTestThread chain_owner{[&] {
        LOCK(::cs_main);
        const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{3}};
        held.set_value();
        // Observe completion WHILE the real chain lock is held. Merely
        // observing a ready future on the caller could run after release.
        observed_before_release = read_completed_future.wait_until(deadline) == std::future_status::ready;
        std::this_thread::sleep_until(deadline);
    }};
    held_future.wait();
    std::shared_ptr<const node::FlowMeshRuntimeHealthSnapshot> health;
    JoiningTestThread reader{[&] {
        health = f.runtime->HealthSnapshot();
        read_completed.set_value();
    }};
    // RAII also joins if construction of the second thread throws.
    chain_owner.join();
    reader.join();
    BOOST_CHECK(observed_before_release);
    BOOST_REQUIRE(health);
    BOOST_CHECK(health == before);
    BOOST_CHECK(health->published_at == before->published_at);
}

BOOST_AUTO_TEST_CASE(certified_reads_do_not_wait_for_a_blocked_market_worker)
{
    HealthRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_certified_read_blocked"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);
    f.Certificate(8, 0);
    const auto before{f.runtime->ClientSnapshot(f.market, error)};
    BOOST_REQUIRE_MESSAGE(before, error);
    {
        std::lock_guard lock{f.relay_mutex};
        f.block_gets = true;
    }
    BOOST_REQUIRE(f.runtime->EnqueueWireMessage(8, {flowmesh::WireMessageKind::HELLO,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 4},
        flowmesh::EncodeMarketHello({f.domain, Filled(0x7a)})}) == flowmesh::QueueResult::ACCEPTED);
    {
        std::unique_lock lock{f.relay_mutex};
        BOOST_REQUIRE(f.relay_cv.wait_for(lock, std::chrono::seconds{5}, [&] { return f.relay_blocked; }));
    }
    // The GET relay holds m_market_mutex. All readers below must answer from
    // one already-durable version, without consulting the blocked worker.
    auto reader{std::async(std::launch::async, [&] {
        std::string read_error;
        const auto status{f.runtime->MarketStatus(f.market)};
        const auto state{f.runtime->StateSnapshot(f.market)};
        flowmesh::MarketDataQuery query;
        query.ticks = flowmesh::ClearingTickQuery{};
        query.indicative = flowmesh::IndicativeQuery{};
        const auto data{f.runtime->MarketData(f.market, std::nullopt, query, read_error)};
        const auto snapshot{f.runtime->ClientSnapshotView(f.market, std::nullopt, read_error)};
        const auto payload{f.runtime->ClientCertifiedEntry(f.market, 0, read_error)};
        return status && state && data && snapshot && payload &&
            status->next_sequence == 1 && status->last_microblock_hash == f.entries[0].GetHash() &&
            status->state_root == state->Root() && data->snapshot.state_root == state->Root() &&
            data->snapshot.next_microblock_sequence == 1 &&
            snapshot->reported.snapshot.last_microblock_hash == f.entries[0].GetHash() &&
            snapshot->evidence.certified_payload == before->certified_payload &&
            snapshot->evidence.state_bytes == before->state_bytes && *payload == before->certified_payload;
    })};
    const bool answered_while_blocked{reader.wait_for(std::chrono::seconds{1}) == std::future_status::ready};
    {
        std::lock_guard lock{f.relay_mutex};
        f.block_gets = false;
    }
    f.relay_cv.notify_all();
    const bool coherent{reader.get()};
    BOOST_CHECK_MESSAGE(answered_while_blocked, "certified read waited for the worker's market lock");
    BOOST_CHECK(coherent);
    BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
}

BOOST_AUTO_TEST_CASE(seat_key_observation_publishes_without_a_tick_or_certified_state_change)
{
    HealthRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_seat_key_observation"};
    const auto refresh = [&] {
#ifndef FLOWMESH_SEAT_OBSERVATION_BASELINE_TEST
        f.runtime->RefreshSeatKeyObservation();
#endif
        // The test-only baseline adapter deliberately omits the new
        // notification: the preceding implementation only scheduled a tick.
        // The immediate observer assertion below must fail on that control.
    };
    refresh(); // Before Start: no view or key-provider observation is created.
    BOOST_CHECK(!f.runtime->MarketStatus(f.market));
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);
    f.Certificate(8, 0);
    BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
    const auto before{f.runtime->MarketStatus(f.market)};
    const auto state{f.runtime->StateView(f.market)};
    const auto evidence{f.runtime->ClientSnapshot(f.market, error)};
    const auto cursor{f.runtime->ClientEventCursor()};
    const auto clock{f.clock.Now()};
    const auto health{f.runtime->HealthSnapshot()};
    BOOST_REQUIRE(before && state && evidence);
    BOOST_REQUIRE(before->observer_only);

    // The provider changes only while idle. No queued work, NotifyTick,
    // clock advance or new signature can hide stale observer metadata.
    f.keys.m_keys[f.market] = {f.seats.secrets[1]};
    refresh();
    BOOST_REQUIRE(!f.runtime->MarketStatus(f.market)->observer_only);
    const auto armed_data{f.runtime->MarketData(f.market, std::nullopt, {}, error)};
    BOOST_REQUIRE_MESSAGE(armed_data, error);
    BOOST_CHECK(!armed_data->snapshot.observer_only);
    f.keys.m_keys.clear();
    refresh();
    BOOST_CHECK(f.runtime->MarketStatus(f.market)->observer_only);
    f.keys.m_keys[f.market] = {Key(42, 91)}; // Armed is not membership.
    refresh();
    BOOST_CHECK(f.runtime->MarketStatus(f.market)->observer_only);

    f.keys.m_keys[f.market] = {f.seats.secrets[1]};
    f.keys.m_visibility_chain = &f.chain;
    f.chain.SetReconciled(false);
    refresh();
    BOOST_CHECK(f.runtime->MarketStatus(f.market)->observer_only);
    f.chain.SetReconciled(true);
    refresh();
    BOOST_CHECK(!f.runtime->MarketStatus(f.market)->observer_only);

    const auto after{f.runtime->MarketStatus(f.market)};
    const auto after_evidence{f.runtime->ClientSnapshot(f.market, error)};
    BOOST_REQUIRE(after && after_evidence);
    BOOST_CHECK_EQUAL(after->epoch, before->epoch);
    BOOST_CHECK_EQUAL(after->round, before->round);
    BOOST_CHECK_EQUAL(after->next_sequence, before->next_sequence);
    BOOST_CHECK_EQUAL(after->next_effect_index, before->next_effect_index);
    BOOST_CHECK_EQUAL(after->pending_actions, before->pending_actions);
    BOOST_CHECK(after->last_microblock_hash == before->last_microblock_hash);
    BOOST_CHECK(after->state_root == before->state_root);
    BOOST_CHECK(after->halt == before->halt);
    BOOST_CHECK_EQUAL(after->paused, before->paused);
    BOOST_CHECK(f.runtime->StateView(f.market) == state);
    BOOST_CHECK(after_evidence->certified_payload == evidence->certified_payload);
    BOOST_CHECK(after_evidence->state_bytes == evidence->state_bytes);
    BOOST_CHECK(f.runtime->ClientEventCursor().instance_id == cursor.instance_id);
    BOOST_CHECK_EQUAL(f.runtime->ClientEventCursor().event_id, cursor.event_id);
    BOOST_CHECK(f.clock.Now() == clock);
    BOOST_CHECK(f.runtime->HealthSnapshot() == health);
    std::optional<uint256> lock;
    BOOST_REQUIRE(f.store.ReadLock({f.seats.seats.epoch, before->next_sequence}, lock, error));
    BOOST_CHECK(!lock); // Updating display capability cannot issue a vote.

    f.runtime->Stop();
    f.keys.m_keys.clear();
    refresh(); // Closed runtime: no new view or provider sampling.
    BOOST_CHECK(!f.runtime->MarketStatus(f.market)->observer_only);
    BOOST_CHECK(f.runtime->StateView(f.market) == state);
}

BOOST_AUTO_TEST_CASE(certified_read_versions_are_immutable_and_survive_rejected_certificate_and_restart)
{
    HealthRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_certified_read_versions"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);
    f.Certificate(8, 0);
    const auto first{f.runtime->StateView(f.market)};
    BOOST_REQUIRE(first);
    const auto first_root{first->Root()};
    const auto first_bytes{flowmesh::EncodeClientState(*first, error)};
    const auto first_evidence{f.runtime->ClientSnapshot(f.market, error)};
    BOOST_REQUIRE(first_bytes && first_evidence);

    auto invalid{f.payloads[1]};
    invalid.back() ^= 1;
    f.Send(8, {flowmesh::WireMessageKind::CERTIFICATE,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 1}, invalid});
    BOOST_CHECK(f.runtime->StateView(f.market) == first);
    const auto after_invalid{f.runtime->ClientSnapshot(f.market, error)};
    BOOST_REQUIRE(after_invalid);
    BOOST_CHECK(after_invalid->certified_payload == first_evidence->certified_payload);

    f.Certificate(8, 1);
    const auto second{f.runtime->StateView(f.market)};
    const auto second_evidence{f.runtime->ClientSnapshot(f.market, error)};
    BOOST_REQUIRE(second && second_evidence);
    BOOST_CHECK(second != first);
    BOOST_CHECK(first->Root() == first_root);
    BOOST_CHECK(flowmesh::EncodeClientState(*first, error) == first_bytes);
    BOOST_CHECK(second->Root() == f.entries[1].state_root);
    f.runtime->Stop();
    node::FlowMeshRuntimeConfig config;
    config.chain = &f.chain; config.keys = &f.keys; config.clock = &f.clock;
    config.relay = [](node::FlowMeshRuntimeRelay) { return LegacyRelayResult(); };
    auto restored{MarketConfig(f.domain, f.market, f.treasury, f.seats.seats, *second, f.store, nullptr)};
    restored.next_sequence = 2;
    restored.next_effect_index = f.entries[1].effect_start + f.entries[1].effect_count;
    restored.last_microblock_hash = f.entries[1].GetHash();
    f.runtime = std::make_unique<node::FlowMeshRuntime>(config,
        std::vector<node::FlowMeshRuntimeMarketConfig>{std::move(restored)});
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);
    const auto reopened{f.runtime->ClientSnapshot(f.market, error)};
    BOOST_REQUIRE_MESSAGE(reopened, error);
    BOOST_CHECK(reopened->certified_payload == second_evidence->certified_payload);
    BOOST_CHECK(reopened->state_bytes == second_evidence->state_bytes);
    BOOST_CHECK(first->Root() == first_root);
}

BOOST_AUTO_TEST_CASE(certified_read_publication_precedes_events_and_never_mixes_heads)
{
    HealthRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_certified_read_events", nullptr, 12};
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);
    f.Certificate(8, 0);
    std::atomic<bool> stop{false}, coherent{true};
    std::atomic<size_t> reads{0};
    JoiningTestThread reader{[&] {
        const flowmesh::ClientEvidencePins pins{f.domain, f.market, f.asset, f.vault, f.initial.ConfigId()};
        while (!stop.load()) {
            std::string read_error;
            const auto events{f.runtime->ClientEvents(std::nullopt, f.market, std::nullopt)};
            const auto view{f.runtime->ClientSnapshotView(f.market, std::nullopt, read_error)};
            if (!view) { coherent = false; break; }
            const auto verified{flowmesh::VerifyClientStateEvidence(pins, view->evidence, f.seats.seats,
                [&](const flowmesh::AnchorRef& anchor) { return f.chain.StillCanonical(anchor); }, read_error)};
            if (!verified || verified->certified.entry.GetHash() != view->reported.snapshot.last_microblock_hash ||
                verified->state.Root() != view->reported.snapshot.state_root ||
                verified->certified.entry.sequence + 1 != view->reported.snapshot.next_microblock_sequence) {
                coherent = false; break;
            }
            for (const auto& event : events.events) {
                if (event.kind == flowmesh::ClientEventKind::CERTIFIED_HEAD &&
                    event.microblock_sequence >= view->reported.snapshot.next_microblock_sequence) coherent = false;
            }
            ++reads;
        }
    }, [&] { stop = true; }};
    BOOST_REQUIRE(WaitUntil([&] { return reads.load() > 0 || !coherent.load(); }));
    // No client request is needed to move these observer replicas: each
    // entry is an independently checked prebuilt quorum certificate.
    for (uint64_t sequence{1}; sequence < 12; ++sequence) f.Certificate(8, sequence);
    stop = true;
    reader.join();
    BOOST_CHECK(coherent.load());
    BOOST_CHECK_GT(reads.load(), 0U);
}

BOOST_AUTO_TEST_CASE(certified_read_public_history_refusal_preserves_exact_durable_entry)
{
    node::FlowMeshRecentWindowLimits recent;
    recent.payload_entries = 1;
    recent.outcome_entries = 0;
    HealthRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_certified_read_history", nullptr, 3, recent};
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);
    f.Certificate(8, 0);
    std::weak_ptr<const flowmesh::FlowMeshState> old_state{f.runtime->StateView(f.market)};
    BOOST_CHECK(!old_state.expired());
    f.Certificate(8, 1);
    // Retired views do not form an unbounded chain. After this unit ends,
    // only a caller that deliberately retained the old state can pin it.
    BOOST_CHECK(old_state.expired());
    const auto reads_before{f.runtime->ClientEntryStoreReadsForTest()};
    BOOST_CHECK(!f.runtime->ClientCertifiedEntry(f.market, 0, error, false));
    BOOST_CHECK_EQUAL(error, "FlowMesh certified entry history unavailable: history_busy");
    BOOST_CHECK_EQUAL(f.runtime->ClientEntryStoreReadsForTest(), reads_before);
    error.clear();
    const auto current{f.runtime->ClientCertifiedEntry(f.market, 1, error, false)};
    BOOST_REQUIRE_MESSAGE(current, error);
    BOOST_CHECK(*current == f.payloads[1]);
    BOOST_CHECK_EQUAL(f.runtime->ClientEntryStoreReadsForTest(), reads_before);
    // The public refusal neither deletes history nor invents nonexecution.
    // The unchanged internal historical path still returns exact bytes.
    const auto historical{f.runtime->ClientCertifiedEntry(f.market, 0, error)};
    BOOST_REQUIRE_MESSAGE(historical, error);
    BOOST_CHECK(*historical == f.payloads[0]);
    BOOST_CHECK_EQUAL(f.runtime->ClientEntryStoreReadsForTest(), reads_before + 1);
}

BOOST_AUTO_TEST_CASE(idempotent_start_does_not_drain_a_busy_writers_retired_views)
{
    HealthRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_read_retirement_start"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);
    f.Certificate(8, 0);
    std::weak_ptr<const flowmesh::FlowMeshState> old_state{f.runtime->StateView(f.market)};
    {
        std::lock_guard lock{f.relay_mutex};
        f.block_certificates = true;
    }
    BOOST_REQUIRE(f.runtime->EnqueueWireMessage(8, {flowmesh::WireMessageKind::CERTIFICATE,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 1},
        f.payloads[1]}) == flowmesh::QueueResult::ACCEPTED);
    {
        std::unique_lock lock{f.relay_mutex};
        BOOST_REQUIRE(f.relay_cv.wait_for(lock, std::chrono::seconds{5}, [&] { return f.relay_blocked; }));
    }
    // The new version was durably published, but its worker is still holding
    // m_market_mutex in the relay. Its retired handles remain its obligation.
    const bool retained_before_start{!old_state.expired()};
    const bool started_again{f.runtime->Start(error)};
    const bool retained_after_start{!old_state.expired()};
    {
        std::lock_guard lock{f.relay_mutex};
        f.block_certificates = false;
    }
    f.relay_cv.notify_all();
    BOOST_REQUIRE(f.runtime->WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK(retained_before_start);
    BOOST_CHECK_MESSAGE(started_again, error);
    BOOST_CHECK_MESSAGE(retained_after_start, "idempotent Start drained a busy writer's retired view");
    BOOST_CHECK(old_state.expired());
}

BOOST_AUTO_TEST_CASE(seat_misbehaviour_evidence_is_recorded_restored_and_changes_nothing)
{
    HealthRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_seat_evidence"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);
    auto health{f.Health()};
    BOOST_CHECK_EQUAL(health->markets.front().seat_evidence_recorded, 0U);
    BOOST_REQUIRE(health->markets.front().seat_evidence);
    BOOST_CHECK(health->markets.front().seat_evidence->empty());

    const auto proposal = [&](const flowmesh::ProductionEntryCore& entry, const uint32_t round) {
        flowmesh::ProductionProposalEnvelope envelope;
        envelope.entry = entry;
        envelope.round = round;
        envelope.proposer_seat_index = flowmesh::ProductionProposerSeatIndex(0, round, f.seats.seats.Size());
        const auto digest{flowmesh::ProductionProposalDigest(entry, round)};
        envelope.proposer_signature = f.seats.secrets[envelope.proposer_seat_index]
            .Sign(std::span<const unsigned char>{digest.begin(), 32}).Compressed();
        return flowmesh::WireMessage{flowmesh::WireMessageKind::PROPOSAL,
                                     {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 0},
                                     *flowmesh::EncodeProductionProposalPayload(envelope)};
    };
    const auto vote = [&](const flowmesh::ProductionEntryCore& entry, const uint32_t seat) {
        const auto signature{flowmesh::SignBlsMicroblockCertificate(
            f.seats.secrets[seat], flowmesh::ProductionCertificateContext(entry), f.seats.seats)};
        BOOST_REQUIRE(signature);
        return flowmesh::WireMessage{flowmesh::WireMessageKind::ATTESTATION,
                                     {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 0},
                                     *flowmesh::EncodeProductionAttestationPayload({seat, *signature})};
    };
    // Two valid candidates for sequence 0, in rounds 0 and 1.
    flowmesh::ProductionEpochGate gate{f.domain, f.market, f.seats.seats};
    flowmesh::ProductionEntryCheck check;
    const auto other{flowmesh::BuildProductionExecutionEntry(
        f.initial, f.domain, f.market, f.seats.seats, gate, 0, 0, {}, f.chain.m_current,
        {f.chain.TipHeight(), std::nullopt, &f.chain}, f.treasury, {}, nullptr, check)};
    BOOST_REQUIRE_MESSAGE(other, flowmesh::ProductionEntryCheckName(check));
    BOOST_REQUIRE(other->entry.GetHash() != f.entries[0].GetHash());
    f.Send(9, proposal(f.entries[0], 0));
    f.Send(9, vote(f.entries[0], 2));
    f.Send(9, proposal(other->entry, 1));
    // Seat 2 signs the second candidate too: refused as before, and recorded.
    f.Send(9, vote(other->entry, 2));
    health = f.Health();
    {
        const auto& row{health->markets.front()};
        BOOST_CHECK_EQUAL(row.seat_evidence_recorded, 1U);
        BOOST_CHECK_EQUAL(row.seat_evidence_unrecorded, 0U);
        BOOST_REQUIRE_EQUAL(row.seat_evidence->size(), 1U);
        const auto& evidence{row.seat_evidence->front()};
        BOOST_CHECK(evidence.kind == flowmesh::SeatEvidenceKind::ATTESTATION_EQUIVOCATION);
        BOOST_CHECK_EQUAL(evidence.first.seat_index, 2U);
        BOOST_CHECK(evidence.seat_public_key == f.seats.seats.members[2].key.Key().Compressed());
        BOOST_CHECK(evidence.first.microblock_hash == f.entries[0].GetHash());
        BOOST_CHECK(evidence.second.microblock_hash == other->entry.GetHash());
        BOOST_CHECK(flowmesh::CheckSeatEvidence(evidence, f.seats.seats, f.seats.seats) ==
                    flowmesh::SeatEvidenceCheck::OK);
        // Nothing else changed: no halt, no commit.
        BOOST_CHECK(!row.halted);
        BOOST_CHECK_EQUAL(row.next_sequence, 0U);
    }
    // The same equivocation again adds no record.
    f.Send(9, vote(other->entry, 2));
    BOOST_CHECK_EQUAL(f.Health()->markets.front().seat_evidence_recorded, 1U);
    const auto recorded{health->markets.front().seat_evidence->front()};

    // A restarted runtime restores the record from the market's store.
    f.runtime->Stop();
    node::FlowMeshRuntimeConfig config;
    config.chain = &f.chain;
    config.keys = &f.keys;
    config.clock = &f.clock;
    config.relay = [](node::FlowMeshRuntimeRelay) { return LegacyRelayResult(); };
    f.runtime = std::make_unique<node::FlowMeshRuntime>(config, std::vector<node::FlowMeshRuntimeMarketConfig>{
        MarketConfig(f.domain, f.market, f.treasury, f.seats.seats, f.initial, f.store, nullptr)});
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);
    health = f.Health();
    BOOST_CHECK_EQUAL(health->markets.front().seat_evidence_recorded, 1U);
    BOOST_REQUIRE_EQUAL(health->markets.front().seat_evidence->size(), 1U);
    BOOST_CHECK(health->markets.front().seat_evidence->front() == recorded);

    // Two valid certificates at one sequence: the existing halt, now with its
    // evidence kept, newest first.
    f.Certificate(9, 0);
    auto conflicting{f.entries[0]};
    conflicting.state_root = Filled(0x99);
    const auto conflict{flowmesh::EncodeProductionCertifiedPayload(
        {conflicting, Certify(conflicting, f.seats)}, f.seats.seats.Size())};
    BOOST_REQUIRE(conflict);
    f.Send(9, {flowmesh::WireMessageKind::CERTIFICATE,
               {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 0}, *conflict});
    f.Send(9, {flowmesh::WireMessageKind::CERTIFICATE,
               {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 0}, *conflict});
    health = f.Health();
    const auto& row{health->markets.front()};
    BOOST_CHECK_EQUAL(row.halt, "certificate-conflict");
    BOOST_CHECK_EQUAL(row.seat_evidence_recorded, 2U);
    BOOST_REQUIRE_EQUAL(row.seat_evidence->size(), 2U);
    const auto& evidence{row.seat_evidence->front()};
    BOOST_CHECK(evidence.kind == flowmesh::SeatEvidenceKind::CERTIFICATE_CONFLICT);
    BOOST_CHECK(evidence.first.microblock_hash == f.entries[0].GetHash());
    BOOST_CHECK(evidence.second.microblock_hash == conflicting.GetHash());
    BOOST_CHECK(flowmesh::CheckSeatEvidence(evidence, f.seats.seats, f.seats.seats) ==
                flowmesh::SeatEvidenceCheck::OK);
    // Both certificates carry seats 0, 1 and 2: each of them signed both.
    BOOST_CHECK((flowmesh::SeatEvidenceDoubleSigners(evidence) == std::vector<uint32_t>{0, 1, 2}));
    BOOST_CHECK(row.seat_evidence->back() == recorded);
}

BOOST_AUTO_TEST_CASE(seat_misbehaviour_flood_costs_bounded_verification_work)
{
    HealthRuntimeFixture f{m_args.GetDataDirBase() / "flowmesh_seat_evidence_flood"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);
    // Each frame from its own connection: the flood is bounded here by the
    // recorder, not by any sender's committee-frame budget.
    flowmesh::WirePeerId next_peer{100};
    const auto send = [&](flowmesh::WireMessage message) { f.Send(next_peer++, std::move(message)); };
    const auto header = [&](const uint64_t sequence) {
        return flowmesh::WireHeader{flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, sequence};
    };
    const auto proposal = [&](const flowmesh::ProductionEntryCore& entry, const uint32_t round) {
        flowmesh::ProductionProposalEnvelope envelope;
        envelope.entry = entry;
        envelope.round = round;
        envelope.proposer_seat_index = flowmesh::ProductionProposerSeatIndex(entry.sequence, round, f.seats.seats.Size());
        const auto digest{flowmesh::ProductionProposalDigest(entry, round)};
        envelope.proposer_signature = f.seats.secrets[envelope.proposer_seat_index]
            .Sign(std::span<const unsigned char>{digest.begin(), 32}).Compressed();
        return flowmesh::WireMessage{flowmesh::WireMessageKind::PROPOSAL, header(entry.sequence),
                                     *flowmesh::EncodeProductionProposalPayload(envelope)};
    };
    const auto vote = [&](const flowmesh::ProductionEntryCore& entry, const uint32_t seat) {
        const auto signature{flowmesh::SignBlsMicroblockCertificate(
            f.seats.secrets[seat], flowmesh::ProductionCertificateContext(entry), f.seats.seats)};
        BOOST_REQUIRE(signature);
        return flowmesh::WireMessage{flowmesh::WireMessageKind::ATTESTATION, header(entry.sequence),
                                     *flowmesh::EncodeProductionAttestationPayload({seat, *signature})};
    };
    // Beside each certified entry, a valid second candidate at its sequence
    // that differs only in its anchor.
    std::vector<flowmesh::ProductionEntryCore> others;
    {
        flowmesh::ProductionEpochGate gate{f.domain, f.market, f.seats.seats};
        flowmesh::ProductionEntryCheck check;
        std::optional<flowmesh::FlowMeshState> state{f.initial};
        uint256 parent;
        std::optional<flowmesh::AnchorRef> previous;
        for (uint64_t sequence{0}; sequence < f.entries.size(); ++sequence) {
            const auto other{flowmesh::BuildProductionExecutionEntry(
                *state, f.domain, f.market, f.seats.seats, gate, sequence, 0, parent, f.chain.m_current,
                {f.chain.TipHeight(), previous, &f.chain}, f.treasury, {}, nullptr, check)};
            BOOST_REQUIRE_MESSAGE(other, flowmesh::ProductionEntryCheckName(check));
            BOOST_REQUIRE(other->entry.GetHash() != f.entries[sequence].GetHash());
            others.push_back(other->entry);
            const auto same{flowmesh::BuildProductionExecutionEntry(
                *state, f.domain, f.market, f.seats.seats, gate, sequence, 0, parent, {100, Filled(0x71)},
                {f.chain.TipHeight(), previous, &f.chain}, f.treasury, {}, nullptr, check)};
            BOOST_REQUIRE(same && same->entry.GetHash() == f.entries[sequence].GetHash());
            state = same->next_state;
            parent = same->entry.GetHash();
            previous = same->entry.anchor;
        }
    }
    // Two candidates at the slot, two seats' votes on each: no quorum, and
    // then `seats` sign the other candidate as well (each one detection).
    const auto equivocate = [&](const uint64_t sequence, const std::vector<uint32_t>& seats) {
        for (const uint32_t seat : seats) {
            send(vote(seat < 2 ? others[sequence] : f.entries[sequence], seat));
        }
    };
    const auto open_slot = [&](const uint64_t sequence) {
        send(proposal(f.entries[sequence], 0));
        send(proposal(others[sequence], 1));
        for (const uint32_t seat : {0U, 1U}) send(vote(f.entries[sequence], seat));
        for (const uint32_t seat : {2U, 3U}) send(vote(others[sequence], seat));
    };
    const auto work = [&] { return f.runtime->SeatEvidenceWorkForTest(); };

    // One seat's equivocation, then a flood of duplicates and repeats of it.
    open_slot(0);
    equivocate(0, {2});
    for (int i{0}; i < 32; ++i) {
        send(vote(f.entries[0], 2));
        send(vote(others[0], 2));
    }
    auto health{f.Health()};
    BOOST_CHECK_EQUAL(health->markets.front().seat_evidence_recorded, 1U);
    BOOST_CHECK_EQUAL(health->markets.front().seat_evidence_unrecorded, 0U);
    // Checked once, without verifying again the seat set or the two
    // signatures this node verified when it received the votes.
    BOOST_CHECK_EQUAL(work().checks, 1U);
    BOOST_CHECK_EQUAL(work().chain_lookups, 0U);
    BOOST_CHECK_EQUAL(work().seat_set_checks, 0U);
    BOOST_CHECK_EQUAL(work().signature_checks, 0U);
    BOOST_CHECK(flowmesh::CheckSeatEvidence(health->markets.front().seat_evidence->front(), f.seats.seats,
                                            f.seats.seats) == flowmesh::SeatEvidenceCheck::OK);

    // More equivocations than the pace allows in one minute: they stop one
    // check short of it, which is kept for a certificate conflict.
    equivocate(0, {0, 1, 3});
    f.Certificate(9, 0);
    open_slot(1);
    equivocate(1, {0, 1, 2, 3});
    BOOST_CHECK_EQUAL(work().checks, 7U);
    f.Certificate(9, 1);
    open_slot(2);
    equivocate(2, {0, 1});
    health = f.Health();
    {
        const auto& row{health->markets.front()};
        BOOST_CHECK_EQUAL(row.next_sequence, 2U);
        BOOST_CHECK(!row.halted);
        BOOST_CHECK_EQUAL(row.seat_evidence_recorded, 7U);
        BOOST_CHECK_EQUAL(row.seat_evidence->size(), 7U);
        // Paced out before any check: counted, never verified or written.
        BOOST_CHECK_EQUAL(row.seat_evidence_unrecorded, 3U);
    }
    BOOST_CHECK_EQUAL(work().checks, 7U);
    BOOST_CHECK_EQUAL(work().seat_set_checks + work().signature_checks + work().chain_lookups, 0U);

    // A new minute checks and records again.
    f.clock.m_now += std::chrono::minutes{1};
    equivocate(2, {2, 3});
    health = f.Health();
    BOOST_CHECK_EQUAL(health->markets.front().seat_evidence_recorded, 9U);
    BOOST_CHECK_EQUAL(health->markets.front().seat_evidence_unrecorded, 3U);
    BOOST_CHECK_EQUAL(work().checks, 9U);
    BOOST_CHECK_EQUAL(work().seat_set_checks + work().signature_checks + work().chain_lookups, 0U);
    for (const auto& evidence : *health->markets.front().seat_evidence) {
        BOOST_CHECK(flowmesh::CheckSeatEvidence(evidence, f.seats.seats, f.seats.seats) ==
                    flowmesh::SeatEvidenceCheck::OK);
    }

    // A certificate conflict re-verifies only the certificate read from the
    // store; the received one verified just before the halt.
    auto conflicting{f.entries[0]};
    conflicting.state_root = Filled(0x99);
    const auto conflict{flowmesh::EncodeProductionCertifiedPayload(
        {conflicting, Certify(conflicting, f.seats)}, f.seats.seats.Size())};
    BOOST_REQUIRE(conflict);
    send({flowmesh::WireMessageKind::CERTIFICATE, header(0), *conflict});
    health = f.Health();
    BOOST_CHECK_EQUAL(health->markets.front().halt, "certificate-conflict");
    BOOST_CHECK_EQUAL(health->markets.front().seat_evidence_recorded, 10U);
    BOOST_CHECK_EQUAL(work().checks, 10U);
    BOOST_CHECK_EQUAL(work().chain_lookups, 0U);
    BOOST_CHECK_EQUAL(work().seat_set_checks, 1U);
    BOOST_CHECK_EQUAL(work().signature_checks, 1U);
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

// Qualification Run03/Run04: a withdrawal that followed other certified slots
// never reached a COMMIT certificate. Exact duplicates of already certified
// slots had spent each receiver's per-peer committee admission (burst 32,
// 8 messages/second) and held the FMN2 channel, so the next view-zero
// proposal was admitted after its two-second round had already expired.
// Limits, round timeout and quorum here are the unchanged production values.
BOOST_AUTO_TEST_CASE(preagreement_paced_slots_survive_fmn2_committee_admission)
{
    constexpr uint32_t SLOTS{12};
    constexpr auto CADENCE{std::chrono::milliseconds{1250}};
    constexpr auto OBSERVATION{std::chrono::seconds{10}};
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_fmn2_admission",
                                 std::chrono::seconds{2}, SLOTS, true};
    f.block_commits = false;
    BOOST_REQUIRE(f.StepUntil(1, OBSERVATION));
    for (uint32_t slot{0}; slot < SLOTS; ++slot) {
        const auto submitted{f.elapsed};
        for (const auto& runtime : f.runtimes) {
            BOOST_REQUIRE(runtime->SubmitLocalAction(f.market, Deposit(COutPoint{f.outpoint.hash, slot})) ==
                          flowmesh::QueueResult::ACCEPTED);
        }
        const bool certified{f.StepUntil(slot + 2, OBSERVATION)};
        f.Drain();
        BOOST_TEST_MESSAGE("slot " << slot + 1 << " certified=" << certified
            << " ms=" << (f.elapsed - submitted).count()
            << " view_changes=" << f.view_changes.load()
            << " agreement_publications=" << f.links.agreement_publications
            << " agreement_deliveries=" << f.links.agreement_deliveries
            << " max_publications_per_message=" << f.links.max_publications_per_message
            << " rate_limited=" << f.links.rate_limited
            << " max_agreement_wait_ms=" << f.links.max_agreement_wait.count()
            << " max_queue_depth=" << f.links.max_queue_depth
            << " queue_refusals=" << f.links.queue_refusals);
        // Every replica durably applied this slot; local queueing is not used
        // as evidence of delivery or certification.
        BOOST_REQUIRE_MESSAGE(certified, "slot " << slot + 1 << " was not certified by every replica within the observation window");
        while (f.elapsed - submitted < CADENCE) f.Step();
    }
    f.Drain();
    // No round may expire merely because exact duplicates were queued first.
    BOOST_CHECK_EQUAL(f.view_changes.load(), 0U);
    BOOST_CHECK_LT(f.links.max_agreement_wait.count(), std::chrono::milliseconds{f.round_timeout}.count());
    BOOST_CHECK_EQUAL(f.links.queue_refusals, 0U);
    BOOST_CHECK_EQUAL(f.links.retry_timeouts, 0U);
    for (size_t i{0}; i < 4; ++i) {
        const auto state{f.runtimes[i]->StateSnapshot(f.market)};
        BOOST_REQUIRE(state);
        BOOST_CHECK_EQUAL(state->LedgerView().Available(f.account, f.asset), 250 * int64_t{SLOTS});
    }
}

// Run04's failing slot: all four operators left view zero before its proposal
// was admitted, and no later view completed. Force that state with a complete
// disconnect longer than one round, reconnect, and require the justified later
// view to decide, certify and be durably applied by every replica.
BOOST_AUTO_TEST_CASE(preagreement_view_change_recovers_under_fmn2_committee_admission)
{
    constexpr auto OBSERVATION{std::chrono::seconds{10}};
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_fmn2_view_change",
                                 std::chrono::seconds{2}, 3, true};
    f.block_commits = false;
    BOOST_REQUIRE(f.StepUntil(1, OBSERVATION));
    const auto submit = [&](uint32_t n) {
        for (const auto& runtime : f.runtimes) {
            BOOST_REQUIRE(runtime->SubmitLocalAction(f.market, Deposit(COutPoint{f.outpoint.hash, n})) ==
                          flowmesh::QueueResult::ACCEPTED);
        }
    };
    submit(0);
    BOOST_REQUIRE(f.StepUntil(2, OBSERVATION));

    f.links.Partition(true);
    submit(1);
    const auto disconnected{f.elapsed};
    while (f.elapsed - disconnected < std::chrono::milliseconds{2500}) f.Step();
    f.Drain();
    BOOST_REQUIRE(f.AllAt(2));
    for (const auto& runtime : f.runtimes) BOOST_CHECK_GE(runtime->MarketStatus(f.market)->round, 1U);
    BOOST_REQUIRE_GT(f.view_changes.load(), 0U);

    f.links.Partition(false);
    const auto reconnected{f.elapsed};
    const bool certified{f.StepUntil(3, OBSERVATION)};
    f.Drain();
    BOOST_TEST_MESSAGE("view-change slot certified=" << certified
        << " ms_after_reconnect=" << (f.elapsed - reconnected).count()
        << " view_changes=" << f.view_changes.load()
        << " agreement_publications=" << f.links.agreement_publications
        << " max_publications_per_message=" << f.links.max_publications_per_message
        << " rate_limited=" << f.links.rate_limited
        << " max_agreement_wait_ms=" << f.links.max_agreement_wait.count()
        << " max_queue_depth=" << f.links.max_queue_depth);
    BOOST_REQUIRE_MESSAGE(certified, "justified later view was not certified by every replica after reconnect");
    // The slot after a view change must be ordinary again.
    submit(2);
    const auto next{f.elapsed};
    BOOST_REQUIRE(f.StepUntil(4, OBSERVATION));
    BOOST_CHECK_LT((f.elapsed - next).count(), std::chrono::milliseconds{f.round_timeout}.count());
    BOOST_CHECK_LT(f.links.max_agreement_wait.count(), std::chrono::milliseconds{f.round_timeout}.count());
    BOOST_CHECK_EQUAL(f.links.queue_refusals, 0U);
    const auto agreed{f.runtimes[0]->MarketStatus(f.market)->last_microblock_hash};
    for (size_t i{0}; i < 4; ++i) {
        BOOST_CHECK(f.runtimes[i]->MarketStatus(f.market)->last_microblock_hash == agreed);
        BOOST_CHECK(f.runtimes[i]->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);
        const auto state{f.runtimes[i]->StateSnapshot(f.market)};
        BOOST_REQUIRE(state);
        BOOST_CHECK_EQUAL(state->LedgerView().Available(f.account, f.asset), 750);
    }
}

// Only identical, already verified bytes are consumed at admission, and only
// until their recovery forward is due. They spend no committee admission;
// changed bytes and later recovery copies still take the ordinary path.
BOOST_AUTO_TEST_CASE(preagreement_exact_verified_duplicates_do_not_spend_committee_admission)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_fmn2_duplicates",
                                 std::chrono::seconds{2}, 1, true};
    for (size_t step{0}; step < 40 && f.links.delivered.empty(); ++step) f.Step();
    f.Drain();
    BOOST_REQUIRE(!f.links.delivered.empty());
    const auto sample{f.links.delivered.front()};
    auto& receiver{*f.runtimes[sample.to]};
    const auto coalesced = [&] {
        const auto snapshots{receiver.DeliverySnapshots(f.market)};
        BOOST_REQUIRE_EQUAL(snapshots.size(), 1U);
        return snapshots.front().agreement_duplicates_coalesced;
    };
    const auto before{coalesced()};
    // More exact copies than the unchanged 32-message committee burst.
    for (size_t n{0}; n < 40; ++n) {
        BOOST_REQUIRE(receiver.EnqueueWireMessage(static_cast<int64_t>(sample.from), sample.message) ==
                      flowmesh::QueueResult::ACCEPTED);
    }
    BOOST_CHECK_EQUAL(coalesced(), before + 40);

    // One changed byte is different evidence: never matched here.
    auto changed{sample.message};
    changed.payload.back() ^= 1;
    (void)receiver.EnqueueWireMessage(static_cast<int64_t>(sample.from), changed);
    f.Drain();
    BOOST_CHECK_EQUAL(coalesced(), before + 40);

    // When the recovery forward is due the same bytes are processed again.
    f.Drain();
    for (auto& clock : f.clocks) clock.m_now += std::chrono::seconds{2};
    BOOST_REQUIRE(receiver.EnqueueWireMessage(static_cast<int64_t>(sample.from), sample.message) ==
                  flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    BOOST_CHECK_EQUAL(coalesced(), before + 40);
}

BOOST_AUTO_TEST_CASE(preagreement_verified_duplicate_requires_exact_wire_header)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_duplicate_header",
                                 std::chrono::seconds{2}, 1, true};
    for (size_t step{0}; step < 40 && f.links.delivered.empty(); ++step) f.Step();
    f.Drain();
    BOOST_REQUIRE(!f.links.delivered.empty());
    const auto sample{f.links.delivered.front()};
    auto& receiver{*f.runtimes[sample.to]};
    const auto before{receiver.DeliverySnapshots(f.market).front().agreement_duplicates_coalesced};
    for (size_t field{0}; field < 3; ++field) {
        auto changed{sample.message};
        if (field == 0) ++changed.header.version;
        if (field == 1) ++changed.header.epoch;
        if (field == 2) ++changed.header.sequence;
        BOOST_CHECK(receiver.EnqueueWireMessage(static_cast<int64_t>(sample.from), changed) ==
                    flowmesh::QueueResult::MALFORMED);
    }
    BOOST_CHECK_EQUAL(receiver.DeliverySnapshots(f.market).front().agreement_duplicates_coalesced, before);
    BOOST_REQUIRE(receiver.EnqueueWireMessage(static_cast<int64_t>(sample.from), sample.message) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_CHECK_EQUAL(receiver.DeliverySnapshots(f.market).front().agreement_duplicates_coalesced, before + 1);
}

// Every validator star-relays each vote and certificate, so an applied slot
// keeps arriving from each peer after this node appended it. The worker can
// only drop those frames. They must not spend the sender's committee
// admission: otherwise the sender's next-slot proposal and votes are refused
// and held behind them. Frames for the current slot, and any older
// certificate that is not byte-identical to an append made here, still spend
// the unchanged committee tokens and take the verified path.
BOOST_AUTO_TEST_CASE(certified_slot_committee_frames_do_not_spend_committee_admission)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "certified_slot_committee_frames"};
    f.block_commits = false;
    f.Reach(1);
    f.Drain();
    std::string error;
    std::optional<node::StoredProductionEntry> stored;
    BOOST_REQUIRE(f.stores[0]->ReadEntry(0, f.seats.seats, stored, error));
    BOOST_REQUIRE(stored);
    // Any valid signer subset: admission matches the entry bytes and header.
    const auto certificate{flowmesh::EncodeProductionCertifiedPayload(
        {stored->entry, stored->certificate}, f.seats.seats.Size())};
    BOOST_REQUIRE(certificate);
    const flowmesh::WireHeader applied{flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, stored->entry.epoch, 0};
    const flowmesh::WireHeader current{flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, stored->entry.epoch, 1};
    const uint256 digest{Filled(0x5c)};
    const auto vote{flowmesh::EncodeProductionAttestationPayload(
        {1, f.seats.secrets[1].Sign(std::span<const unsigned char>{digest.begin(), 32})})};
    BOOST_REQUIRE(vote);
    // Correctly framed (format 1, a valid stage) but never a verified payload.
    std::vector<unsigned char> agreement(flowmesh::FLOWMESH_AGREEMENT_MIN_BYTES, 0x5c);
    agreement[0] = 0;
    agreement[1] = 1;
    agreement[2] = 2;

    auto& receiver{*f.runtimes[0]};
    constexpr flowmesh::WirePeerId sender{1};
    const auto snapshot = [&] {
        const auto snapshots{f.runtimes[0]->DeliverySnapshots(f.market)};
        BOOST_REQUIRE_EQUAL(snapshots.size(), 1U);
        return snapshots.front();
    };
    // Let the sender's bucket refill completely after the slot's own traffic,
    // then freeze the clock: three times the unchanged 32-message burst.
    f.clocks[0].m_now += std::chrono::seconds{5};
    const auto before{snapshot()};
    for (size_t n{0}; n < 32; ++n) {
        BOOST_REQUIRE(receiver.EnqueueWireMessage(sender, {flowmesh::WireMessageKind::ATTESTATION, applied, *vote}) ==
                      flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(receiver.EnqueueWireMessage(sender, {flowmesh::WireMessageKind::AGREEMENT, applied, agreement}) ==
                      flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(receiver.EnqueueWireMessage(sender, {flowmesh::WireMessageKind::CERTIFICATE, applied, *certificate}) ==
                      flowmesh::QueueResult::ACCEPTED);
    }
    f.Drain();
    auto after{snapshot()};
    BOOST_CHECK_EQUAL(after.certified_slot_frames_consumed, before.certified_slot_frames_consumed + 96);
    // Nothing reached the worker: no certificate was verified again.
    BOOST_CHECK_EQUAL(after.verified, before.verified);
    BOOST_CHECK(receiver.MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);
    // A badly framed frame for the applied slot is still refused as such.
    BOOST_CHECK(receiver.EnqueueWireMessage(sender, {flowmesh::WireMessageKind::ATTESTATION, applied,
                    std::vector<unsigned char>(vote->begin(), vote->end() - 1)}) == flowmesh::QueueResult::MALFORMED);
    BOOST_CHECK(receiver.EnqueueWireMessage(sender, {flowmesh::WireMessageKind::AGREEMENT, applied,
                    std::vector<unsigned char>(agreement.begin(), agreement.end() - 1)}) == flowmesh::QueueResult::MALFORMED);
    BOOST_CHECK_EQUAL(snapshot().certified_slot_frames_consumed, before.certified_slot_frames_consumed + 96);

    // The sender's whole burst is still available for the next slot, and it
    // still bounds everything admission cannot prove settled: the current
    // slot, a changed header, and different entry bytes at the applied
    // sequence (the only way a conflicting certificate can arrive).
    auto relabelled{applied};
    ++relabelled.epoch;
    auto different{*certificate};
    different.back() ^= 1;                                               // same entry bytes, other aggregate
    auto conflicting{*certificate};
    conflicting[flowmesh::FLOWMESH_CERTIFIED_PAYLOAD_PREFIX_SIZE + 8] ^= 1; // other entry bytes
    const std::vector<flowmesh::WireMessage> charged{
        {flowmesh::WireMessageKind::ATTESTATION, current, *vote},
        {flowmesh::WireMessageKind::AGREEMENT, current, agreement},
        {flowmesh::WireMessageKind::CERTIFICATE, relabelled, *certificate},
        {flowmesh::WireMessageKind::CERTIFICATE, applied, conflicting}};
    size_t admitted{0}, limited{0};
    for (size_t n{0}; n < 40; ++n) {
        const auto result{receiver.EnqueueWireMessage(sender, charged[n % charged.size()])};
        admitted += result == flowmesh::QueueResult::ACCEPTED;
        limited += result == flowmesh::QueueResult::RATE_LIMITED;
    }
    BOOST_CHECK_EQUAL(admitted, 32U);
    BOOST_CHECK_EQUAL(limited, 8U);
    BOOST_REQUIRE(receiver.EnqueueWireMessage(sender, {flowmesh::WireMessageKind::CERTIFICATE, applied, different}) ==
                  flowmesh::QueueResult::ACCEPTED); // settled entry bytes: consumed even with no token left
    f.Drain();
    after = snapshot();
    BOOST_CHECK_EQUAL(after.certified_slot_frames_consumed, before.certified_slot_frames_consumed + 97);
    BOOST_CHECK(receiver.MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);

    // A restarted runtime has appended nothing yet in its lifetime, so the
    // same exact copy takes the verified path again; the worker still
    // recognises the durable head without verifying it a second time.
    f.StopNode(0);
    f.stores[0].reset();
    f.stores[0] = std::make_unique<node::FlowMeshProductionStore>(DBParams{
        .path = f.StorePath(0), .cache_bytes = size_t{1} << 20}, true);
    BOOST_REQUIRE_MESSAGE(f.stores[0]->OpenForMarket(f.domain, f.market,
        f.seats.seats, f.initial.Root(), error), error);
    BOOST_REQUIRE_MESSAGE(f.StartNode(0, f.AgreementPath(0), std::nullopt, error), error);
    BOOST_REQUIRE(f.AllAt(1));
    BOOST_REQUIRE(f.runtimes[0]->EnqueueWireMessage(sender, {flowmesh::WireMessageKind::CERTIFICATE, applied, *certificate}) ==
                  flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    const auto restarted{snapshot()};
    BOOST_CHECK_EQUAL(restarted.certified_slot_frames_consumed, 0U);
    BOOST_CHECK(std::any_of(restarted.events.begin(), restarted.events.end(), [](const auto& event) {
        return event.stage == "certificate_duplicate";
    }));
    BOOST_CHECK(f.runtimes[0]->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);
}

// The worker's durable-duplicate check covers older appends of this
// lifetime (recent_durable), not only its head, without verifying the copy
// again. Admission consumes copies of recorded appends before they reach the
// worker, so this needs a copy admitted before its append was recorded:
// runtime 0's worker is held while two certificates for slots it has not yet
// applied, and then a repeat of the first, are queued behind it.
BOOST_AUTO_TEST_CASE(worker_recognises_an_older_durable_certificate_without_verifying)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "worker_older_durable_certificate",
                                 std::chrono::seconds{10}, 3};
    f.block_commits = false;
    f.Reach(1);

    // Runtime 0 neither hears nor is heard while the others apply two slots.
    std::mutex mutex;
    std::condition_variable cv;
    bool hold{false}, held{false}, release{false};
    f.network.SetFilter([&](size_t from, size_t to, const flowmesh::WireMessage& wire) {
        {
            std::unique_lock<std::mutex> lock{mutex};
            if (hold && from == 0 && wire.kind == flowmesh::WireMessageKind::ACTION) {
                hold = false;
                held = true;
                cv.notify_all();
                cv.wait(lock, [&] { return release; });
            }
        }
        if (from == 0 || to == 0) return false;
        return f.Observe(wire);
    });
    const auto released = [&] {
        {
            std::lock_guard<std::mutex> lock{mutex};
            release = true;
        }
        cv.notify_all();
    };
    struct Release {
        std::function<void()> run;
        ~Release() { run(); }
    } release_on_exit{released};
    const auto others_at = [&](const uint64_t sequence) {
        for (size_t i{1}; i < f.runtimes.size(); ++i) {
            const auto status{f.runtimes[i]->MarketStatus(f.market)};
            if (!status || status->next_sequence != sequence ||
                status->halt != node::FlowMeshRuntimeHalt::NONE) return false;
        }
        return true;
    };
    for (uint32_t n{0}; n < 2; ++n) {
        for (size_t i{1}; i < f.runtimes.size(); ++i) {
            BOOST_REQUIRE(f.runtimes[i]->SubmitLocalAction(f.market, Deposit(COutPoint{f.outpoint.hash, n})) ==
                          flowmesh::QueueResult::ACCEPTED);
        }
        for (size_t attempt{0}; attempt < 16 && !others_at(n + 2); ++attempt) f.Tick(std::chrono::seconds{2});
        BOOST_REQUIRE(others_at(n + 2));
    }
    auto& runtime{*f.runtimes[0]};
    BOOST_REQUIRE_EQUAL(runtime.MarketStatus(f.market)->next_sequence, 1U);
    const auto certificate = [&](const uint64_t sequence) {
        std::string error;
        std::optional<node::StoredProductionEntry> stored;
        BOOST_REQUIRE_MESSAGE(f.stores[1]->ReadEntry(sequence, f.seats.seats, stored, error), error);
        BOOST_REQUIRE(stored);
        const auto payload{flowmesh::EncodeProductionCertifiedPayload(
            {stored->entry, stored->certificate}, f.seats.seats.Size())};
        BOOST_REQUIRE(payload);
        return flowmesh::WireMessage{flowmesh::WireMessageKind::CERTIFICATE,
            {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, stored->entry.epoch, sequence}, *payload};
    };
    const auto first{certificate(1)};
    const auto second{certificate(2)};
    const auto snapshot = [&] {
        const auto snapshots{runtime.DeliverySnapshots(f.market)};
        BOOST_REQUIRE_EQUAL(snapshots.size(), 1U);
        return snapshots.front();
    };
    const auto before{snapshot()};

    // Hold runtime 0's worker in its relay of a new peer action. Nothing is
    // recorded for admission yet, so all three frames are queued.
    {
        std::lock_guard<std::mutex> lock{mutex};
        hold = true;
    }
    const auto action{flowmesh::EncodeProductionActionPayload(Deposit(COutPoint{f.outpoint.hash, 2}))};
    BOOST_REQUIRE(action);
    BOOST_REQUIRE(runtime.EnqueueWireMessage(9, {flowmesh::WireMessageKind::ACTION,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 1}, *action}) ==
                  flowmesh::QueueResult::ACCEPTED);
    {
        std::unique_lock<std::mutex> lock{mutex};
        BOOST_REQUIRE(cv.wait_for(lock, std::chrono::seconds{10}, [&] { return held; }));
    }
    constexpr flowmesh::WirePeerId sender{7};
    for (const auto* message : {&first, &second, &first}) {
        BOOST_REQUIRE(runtime.EnqueueWireMessage(sender, *message) == flowmesh::QueueResult::ACCEPTED);
    }
    released();
    f.Drain();

    const auto status{runtime.MarketStatus(f.market)};
    BOOST_REQUIRE(status);
    BOOST_CHECK_EQUAL(status->next_sequence, 3U);
    BOOST_CHECK(status->halt == node::FlowMeshRuntimeHalt::NONE);
    const auto after{snapshot()};
    // The action and the two new certificates were verified; the repeat of
    // the first, now older than the head, was recognised without it.
    BOOST_CHECK_EQUAL(after.verified, before.verified + 3);
    BOOST_CHECK_EQUAL(after.certified_slot_frames_consumed, before.certified_slot_frames_consumed);
    const auto count = [&](const std::string& stage, const uint64_t sequence) {
        return std::count_if(after.events.begin(), after.events.end(), [&](const auto& event) {
            return event.stage == stage && event.sequence == sequence;
        });
    };
    BOOST_CHECK_EQUAL(count("certificate_verified", 1), 1);
    BOOST_CHECK_EQUAL(count("certificate_verified", 2), 1);
    BOOST_CHECK_EQUAL(count("certificate_duplicate", 1), 1);
    // Recorded now: a further copy is consumed at admission.
    BOOST_REQUIRE(runtime.EnqueueWireMessage(sender, first) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    BOOST_CHECK_EQUAL(snapshot().certified_slot_frames_consumed, before.certified_slot_frames_consumed + 1);
}

// Consumed frames spend no committee token, but a flood of them is still
// refused: both admission consumptions (exact verified agreement repeats and
// frames for an already appended slot) draw on the sender's separate
// settled-frame budget, 256 at once and 64 per second per peer and market.
// The committee budget and other peers are unaffected.
BOOST_AUTO_TEST_CASE(settled_slot_frames_spend_a_separate_bounded_budget)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "settled_slot_budget",
                                 std::chrono::seconds{2}, 1, true};
    f.block_commits = false;
    BOOST_REQUIRE(f.StepUntil(1, std::chrono::seconds{10}));
    f.Tick(); // every worker has retired slot 0's verified agreement bytes
    BOOST_REQUIRE(!f.links.delivered.empty());
    const auto sample{f.links.delivered.front()};
    BOOST_REQUIRE_EQUAL(sample.message.header.sequence, 0U);
    auto& receiver{*f.runtimes[sample.to]};
    const auto sender{static_cast<flowmesh::WirePeerId>(sample.from)};
    const auto snapshot = [&] {
        const auto snapshots{receiver.DeliverySnapshots(f.market)};
        BOOST_REQUIRE_EQUAL(snapshots.size(), 1U);
        return snapshots.front();
    };
    const flowmesh::WireHeader applied{sample.message.header};
    flowmesh::WireHeader current{applied};
    current.sequence = 1;
    const uint256 digest{Filled(0x5d)};
    const auto vote{flowmesh::EncodeProductionAttestationPayload(
        {1, f.seats.secrets[1].Sign(std::span<const unsigned char>{digest.begin(), 32})})};
    BOOST_REQUIRE(vote);
    const flowmesh::WireMessage settled_vote{flowmesh::WireMessageKind::ATTESTATION, applied, *vote};
    // Let the sender's budgets refill after the slot's own traffic, then
    // freeze the receiver's clock.
    f.clocks[sample.to].m_now += std::chrono::seconds{5};
    const auto before{snapshot()};
    for (size_t n{0}; n < 200; ++n) {
        BOOST_REQUIRE(receiver.EnqueueWireMessage(sender, sample.message) == flowmesh::QueueResult::ACCEPTED);
    }
    for (size_t n{0}; n < 56; ++n) {
        BOOST_REQUIRE(receiver.EnqueueWireMessage(sender, settled_vote) == flowmesh::QueueResult::ACCEPTED);
    }
    BOOST_CHECK(receiver.EnqueueWireMessage(sender, sample.message) == flowmesh::QueueResult::RATE_LIMITED);
    BOOST_CHECK(receiver.EnqueueWireMessage(sender, settled_vote) == flowmesh::QueueResult::RATE_LIMITED);
    // The framing verdict still comes first.
    BOOST_CHECK(receiver.EnqueueWireMessage(sender, {flowmesh::WireMessageKind::ATTESTATION, applied,
                    std::vector<unsigned char>(vote->begin(), vote->end() - 1)}) == flowmesh::QueueResult::MALFORMED);
    // The committee budget is separate: the current slot is still admitted.
    BOOST_CHECK(receiver.EnqueueWireMessage(sender, {flowmesh::WireMessageKind::ATTESTATION, current, *vote}) ==
                flowmesh::QueueResult::ACCEPTED);
    // Another peer has a settled-frame budget of its own.
    BOOST_CHECK(receiver.EnqueueWireMessage(sender + 100, settled_vote) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    auto after{snapshot()};
    BOOST_CHECK_EQUAL(after.agreement_duplicates_coalesced, before.agreement_duplicates_coalesced + 200);
    BOOST_CHECK_EQUAL(after.certified_slot_frames_consumed, before.certified_slot_frames_consumed + 57);
    BOOST_CHECK_EQUAL(after.settled_frames_rate_limited, before.settled_frames_rate_limited + 2);

    // One second refills 64 frames' worth.
    f.clocks[sample.to].m_now += std::chrono::seconds{1};
    for (size_t n{0}; n < 64; ++n) {
        BOOST_REQUIRE(receiver.EnqueueWireMessage(sender, settled_vote) == flowmesh::QueueResult::ACCEPTED);
    }
    BOOST_CHECK(receiver.EnqueueWireMessage(sender, settled_vote) == flowmesh::QueueResult::RATE_LIMITED);
    f.Drain();
    after = snapshot();
    BOOST_CHECK_EQUAL(after.certified_slot_frames_consumed, before.certified_slot_frames_consumed + 57 + 64);
    BOOST_CHECK_EQUAL(after.settled_frames_rate_limited, before.settled_frames_rate_limited + 3);
    BOOST_CHECK(receiver.MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);
}

// Admission hashes a certificate's entry bytes, or an agreement payload the
// quiet record may hold, only while the sender still has a settled or a
// committee token that could admit the frame. A frame that neither budget
// could admit is refused RATE_LIMITED unhashed (a refused FMN2 frame is
// offered again on every retry); otherwise results are the hashed path's.
BOOST_AUTO_TEST_CASE(admission_hashes_no_frame_that_no_budget_could_admit)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "admission_hash_budget",
                                 std::chrono::seconds{2}, 1, true};
    f.block_commits = false;
    BOOST_REQUIRE(f.StepUntil(1, std::chrono::seconds{10}));
    f.Tick(); // every worker has retired slot 0's verified agreement bytes
    BOOST_REQUIRE(!f.links.delivered.empty());
    const auto sample{f.links.delivered.front()};
    BOOST_REQUIRE_EQUAL(sample.message.header.sequence, 0U);
    auto& receiver{*f.runtimes[sample.to]};
    const auto sender{static_cast<flowmesh::WirePeerId>(sample.from)};
    const auto snapshot = [&] {
        const auto snapshots{receiver.DeliverySnapshots(f.market)};
        BOOST_REQUIRE_EQUAL(snapshots.size(), 1U);
        return snapshots.front();
    };
    const auto enqueue = [&](const flowmesh::WireMessage& message) {
        return receiver.EnqueueWireMessage(sender, message);
    };
    std::string error;
    std::optional<node::StoredProductionEntry> stored;
    BOOST_REQUIRE(f.stores[sample.to]->ReadEntry(0, f.seats.seats, stored, error));
    BOOST_REQUIRE(stored);
    const auto payload{flowmesh::EncodeProductionCertifiedPayload(
        {stored->entry, stored->certificate}, f.seats.seats.Size())};
    BOOST_REQUIRE(payload);
    const flowmesh::WireHeader applied{flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, stored->entry.epoch, 0};
    flowmesh::WireHeader current{applied};
    current.sequence = 1;
    const flowmesh::WireMessage certificate{flowmesh::WireMessageKind::CERTIFICATE, applied, *payload};
    auto conflicting{certificate}; // other entry bytes under the recorded header
    conflicting.payload[flowmesh::FLOWMESH_CERTIFIED_PAYLOAD_PREFIX_SIZE + 8] ^= 1;
    auto relabelled{sample.message}; // verified agreement bytes under another header
    ++relabelled.header.epoch;
    auto misframed{sample.message};
    misframed.payload[0] ^= 0xff;
    const uint256 digest{Filled(0x5e)};
    const auto vote{flowmesh::EncodeProductionAttestationPayload(
        {1, f.seats.secrets[1].Sign(std::span<const unsigned char>{digest.begin(), 32})})};
    BOOST_REQUIRE(vote);
    const flowmesh::WireMessage settled_vote{flowmesh::WireMessageKind::ATTESTATION, applied, *vote};
    const flowmesh::WireMessage current_vote{flowmesh::WireMessageKind::ATTESTATION, current, *vote};

    // Let the sender's budgets refill after the slot's own traffic, then
    // freeze the receiver's clock.
    f.clocks[sample.to].m_now += std::chrono::seconds{5};
    const auto before{snapshot()};
    const auto hashes{receiver.AdmissionHashCountForTest()};
    // With tokens left each is hashed, as before: exact copies are consumed
    // and known bytes under another header are malformed.
    BOOST_CHECK(enqueue(certificate) == flowmesh::QueueResult::ACCEPTED);
    BOOST_CHECK(enqueue(sample.message) == flowmesh::QueueResult::ACCEPTED);
    BOOST_CHECK(enqueue(relabelled) == flowmesh::QueueResult::MALFORMED);
    BOOST_CHECK_EQUAL(receiver.AdmissionHashCountForTest(), hashes + 3);
    // Spend the rest of the settled budget on frames that need no hash.
    size_t consumed{0};
    while (consumed < 300 && enqueue(settled_vote) == flowmesh::QueueResult::ACCEPTED) ++consumed;
    BOOST_CHECK_EQUAL(consumed, 254U);
    // The committee budget can still admit a certificate, so it is hashed:
    // other entry bytes take the committee path, and an exact copy is
    // refused by the spent settled budget, exactly as before.
    BOOST_CHECK(enqueue(conflicting) == flowmesh::QueueResult::ACCEPTED);
    BOOST_CHECK(enqueue(certificate) == flowmesh::QueueResult::RATE_LIMITED);
    BOOST_CHECK_EQUAL(receiver.AdmissionHashCountForTest(), hashes + 5);
    // Spend the rest of the committee budget.
    size_t queued{0};
    while (queued < 40 && enqueue(current_vote) == flowmesh::QueueResult::ACCEPTED) ++queued;
    BOOST_CHECK_EQUAL(queued, 31U);

    // Now neither budget could admit any of these: refused unhashed. The
    // framing verdict still comes first.
    BOOST_CHECK(enqueue(certificate) == flowmesh::QueueResult::RATE_LIMITED);
    BOOST_CHECK(enqueue(conflicting) == flowmesh::QueueResult::RATE_LIMITED);
    BOOST_CHECK(enqueue(sample.message) == flowmesh::QueueResult::RATE_LIMITED);
    BOOST_CHECK(enqueue(relabelled) == flowmesh::QueueResult::RATE_LIMITED);
    BOOST_CHECK(enqueue(misframed) == flowmesh::QueueResult::MALFORMED);
    BOOST_CHECK_EQUAL(receiver.AdmissionHashCountForTest(), hashes + 5);
    f.Drain();
    auto after{snapshot()};
    BOOST_CHECK_EQUAL(after.agreement_duplicates_coalesced, before.agreement_duplicates_coalesced + 1);
    BOOST_CHECK_EQUAL(after.certified_slot_frames_consumed, before.certified_slot_frames_consumed + 1 + 254);
    // One vote and one certificate refused by the settled budget after
    // hashing, and four refused before hashing.
    BOOST_CHECK_EQUAL(after.settled_frames_rate_limited, before.settled_frames_rate_limited + 6);

    // Once a token is back the refused frames are judged as before; the
    // relabelled copy is found malformed on its retry.
    f.clocks[sample.to].m_now += std::chrono::seconds{1};
    BOOST_CHECK(enqueue(relabelled) == flowmesh::QueueResult::MALFORMED);
    BOOST_CHECK(enqueue(certificate) == flowmesh::QueueResult::ACCEPTED);
    BOOST_CHECK_EQUAL(receiver.AdmissionHashCountForTest(), hashes + 7);
    f.Drain();
    BOOST_CHECK(receiver.MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);
}

BOOST_AUTO_TEST_CASE(preagreement_certified_duplicate_stays_quiet_after_backoff_expires)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_duplicate_old_slot",
                                 std::chrono::seconds{2}, 1, true};
    f.block_commits = false;
    BOOST_REQUIRE(f.StepUntil(1, std::chrono::seconds{10}));
    f.Drain();
    BOOST_REQUIRE(!f.links.delivered.empty());
    const auto sample{f.links.delivered.front()};
    BOOST_REQUIRE_EQUAL(sample.message.header.sequence, 0U);
    auto& receiver{*f.runtimes[sample.to]};
    const auto before{receiver.DeliverySnapshots(f.market).front().agreement_duplicates_coalesced};
    for (auto& chain : f.chains) chain.SetReconciled(false);
    f.Tick();
    for (size_t sweep{0}; sweep < 2; ++sweep) {
        // Older-slot verification cannot succeed again after advancement. Its
        // elapsed recovery deadline must not repeatedly reopen committee work.
        for (auto& clock : f.clocks) clock.m_now += std::chrono::seconds{60};
        for (size_t n{0}; n < 40; ++n) {
            BOOST_REQUIRE(receiver.EnqueueWireMessage(static_cast<int64_t>(sample.from), sample.message) ==
                          flowmesh::QueueResult::ACCEPTED);
        }
        f.Drain();
        BOOST_CHECK_EQUAL(receiver.DeliverySnapshots(f.market).front().agreement_duplicates_coalesced,
                          before + 40 * (sweep + 1));
    }
    // Different bytes for the applied slot are consumed at admission too (the
    // worker could only refuse them), and the next slot retains the same
    // unmodified admission budget and full quorum.
    auto changed{sample.message};
    changed.payload.back() ^= 1;
    BOOST_REQUIRE(receiver.EnqueueWireMessage(static_cast<int64_t>(sample.from), changed) ==
                  flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    for (auto& chain : f.chains) chain.SetReconciled(true);
    for (const auto& runtime : f.runtimes) {
        BOOST_REQUIRE(runtime->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    }
    BOOST_REQUIRE(f.StepUntil(2, std::chrono::seconds{10}));
    for (const auto& runtime : f.runtimes) {
        BOOST_CHECK_EQUAL(runtime->StateSnapshot(f.market)->LedgerView().Available(f.account, f.asset), 250);
        BOOST_CHECK(runtime->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);
    }
}

BOOST_AUTO_TEST_CASE(preagreement_old_view_evidence_recovers_while_duplicate_is_quiet)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_old_view_evidence"};
    f.block_commits = false;
    f.Reach(1);
    f.Drain();
    std::mutex observed_mutex;
    std::optional<flowmesh::WireMessage> proposal;
    size_t target_proposals{0}, target_echoes{0};
    struct StopBeforeObservations {
        PreagreementRuntimeHarness<4>& fixture;
        ~StopBeforeObservations() { fixture.StopAll(); }
    } stop_before_observations{f};
    // All traffic is held after genesis. Observe exact proposals only; no
    // hidden network delivery is allowed to provide the missing deposit.
    f.network.SetFilter([&](size_t from, size_t to, const flowmesh::WireMessage& wire) {
        if (wire.kind == flowmesh::WireMessageKind::AGREEMENT && wire.header.sequence == 1) {
            const auto message{flowmesh::DecodeAgreementMessage(wire.payload)};
            if (message && message->stage == flowmesh::AgreementStage::PROPOSAL && message->view == 0) {
                std::lock_guard<std::mutex> lock{observed_mutex};
                if (from == 1) proposal = wire;
                if (from == 3) { ++target_proposals; if (to == 1) ++target_echoes; }
            }
        }
        return false;
    });
    BOOST_REQUIRE(f.runtimes[1]->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    f.Tick();
    flowmesh::WireMessage exact;
    {
        std::lock_guard<std::mutex> lock{observed_mutex};
        BOOST_REQUIRE(proposal);
        exact = *proposal;
    }
    const auto decoded{flowmesh::DecodeAgreementMessage(exact.payload)};
    BOOST_REQUIRE(decoded);
    // Two authenticated reports justify entering view one. Operator three
    // is a follower there and cannot manufacture the hidden candidate itself.
    for (uint32_t seat : {0U, 2U}) {
        flowmesh::AgreementMessage report;
        report.stage = flowmesh::AgreementStage::VIEW_CHANGE;
        report.context = decoded->context;
        report.view = 1;
        report.seat_index = seat;
        const auto digest{flowmesh::AgreementMessageDigest(report)};
        BOOST_REQUIRE(digest);
        report.signature = f.seats.secrets[seat].Sign(
            std::span<const unsigned char>{digest->begin(), 32}).Compressed();
        const auto payload{flowmesh::EncodeAgreementMessage(report)};
        BOOST_REQUIRE(payload);
        BOOST_REQUIRE(f.runtimes[3]->EnqueueWireMessage(seat,
            {flowmesh::WireMessageKind::AGREEMENT, exact.header, *payload}) == flowmesh::QueueResult::ACCEPTED);
        f.Drain();
    }
    BOOST_REQUIRE_EQUAL(f.runtimes[3]->MarketStatus(f.market)->round, 1U);
    for (const auto delay : {0, 1, 2, 4}) {
        f.clocks[3].m_now += std::chrono::seconds{delay};
        BOOST_REQUIRE(f.runtimes[3]->EnqueueWireMessage(1, exact) == flowmesh::QueueResult::ACCEPTED);
        f.Drain();
    }
    size_t before;
    {
        std::lock_guard<std::mutex> lock{observed_mutex};
        before = target_proposals;
        BOOST_CHECK_GT(before, 0U);
        BOOST_CHECK_EQUAL(target_echoes, 0U); // every received forward excludes its source
    }
    const auto quiet_before{f.runtimes[3]->DeliverySnapshots(f.market).front().agreement_duplicates_coalesced};
    BOOST_REQUIRE(f.runtimes[3]->EnqueueWireMessage(1, exact) == flowmesh::QueueResult::ACCEPTED);
    BOOST_CHECK_EQUAL(f.runtimes[3]->DeliverySnapshots(f.market).front().agreement_duplicates_coalesced,
                      quiet_before + 1);
    // Fresh action admission wakes the worker. Close the gate before that
    // event so its already-due evidence retry cannot run during the setup.
    f.chains[3].SetReconciled(false);
    BOOST_REQUIRE(f.runtimes[3]->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    f.runtimes[3]->NotifyTick();
    f.Drain();
    {
        std::lock_guard<std::mutex> lock{observed_mutex};
        BOOST_CHECK_EQUAL(target_proposals, before);
    }
    f.chains[3].SetReconciled(true);
    f.clocks[3].m_now += std::chrono::milliseconds{250};
    f.runtimes[3]->NotifyTick();
    f.Drain();
    {
        std::lock_guard<std::mutex> lock{observed_mutex};
        // Recovery retains the exact old proposal for durable retry, well
        // before the eight-second ingress/forward cooldown can expire.
        BOOST_CHECK_GT(target_proposals, before);
    }
    BOOST_CHECK_EQUAL(f.runtimes[3]->MarketStatus(f.market)->round, 1U);
    BOOST_CHECK_EQUAL(f.runtimes[3]->MarketStatus(f.market)->next_sequence, 1U);
    std::optional<uint256> locked;
    std::string error;
    BOOST_REQUIRE(f.stores[3]->ReadLock({0, 1}, locked, error));
    BOOST_CHECK(!locked);
    BOOST_CHECK(f.runtimes[3]->MarketStatus(f.market)->halt == node::FlowMeshRuntimeHalt::NONE);
}

BOOST_AUTO_TEST_CASE(preagreement_star_retries_exact_proposal_after_asymmetric_loss)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_star_recovery"};
    f.block_commits = false;
    f.Reach(1);
    f.Drain();
    f.StopNode(2); // three online seats still require the full three-seat quorum
    std::atomic<bool> healed{false};
    std::atomic<size_t> blocked{0}, recovered{0};
    std::mutex observed_mutex;
    std::optional<std::vector<unsigned char>> original;
    struct StopBeforeObservations {
        PreagreementRuntimeHarness<4>& fixture;
        ~StopBeforeObservations() { fixture.StopAll(); }
    } stop_before_observations{f};
    f.network.SetFilter([&](size_t from, size_t to, const flowmesh::WireMessage& wire) {
        if (from != 0 && to != 0) return false; // physical star, no leaf shortcut
        if (wire.kind == flowmesh::WireMessageKind::AGREEMENT && wire.header.sequence == 1) {
            const auto message{flowmesh::DecodeAgreementMessage(wire.payload)};
            if (message && message->stage == flowmesh::AgreementStage::PROPOSAL && message->view == 0) {
                std::lock_guard<std::mutex> lock{observed_mutex};
                if (from == 1) original = wire.payload;
                if (from == 0 && to == 3) {
                    if (!healed) { ++blocked; return false; }
                    if (original && wire.payload == *original) ++recovered;
                }
            }
        }
        return true;
    });
    BOOST_REQUIRE(f.runtimes[1]->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    f.Tick();
    BOOST_REQUIRE_GT(blocked.load(), 0U);
    BOOST_REQUIRE(f.AllAt(1));
    healed = true;
    for (size_t attempt{0}; attempt < 8 && !f.AllAt(2); ++attempt) f.Tick(std::chrono::seconds{1});
    BOOST_REQUIRE_GT(recovered.load(), 0U);
    BOOST_REQUIRE(f.AllAt(2));
    const auto head{f.runtimes[0]->MarketStatus(f.market)->last_microblock_hash};
    for (const auto& runtime : f.runtimes) if (runtime) {
        BOOST_CHECK(runtime->MarketStatus(f.market)->last_microblock_hash == head);
        BOOST_CHECK_EQUAL(runtime->StateSnapshot(f.market)->LedgerView().Available(f.account, f.asset), 250);
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

BOOST_AUTO_TEST_CASE(preagreement_runtime_unavailable_signers_preserve_timeout_and_history)
{
    // Each gate can remove local signing eligibility without authorizing a
    // new view. Keep an already prepared deposit and the certified genesis.
    for (const std::string gate : {"paused", "keyless", "reconciling"}) {
        BOOST_TEST_CONTEXT(gate) {
            PreagreementRuntimeHarness f{m_args.GetDataDirBase() /
                fs::PathFromString("preagreement_timeout_" + gate), std::chrono::seconds{2}};
            f.block_commits = false;
            f.Reach(1);
            f.Drain();
            const uint256 genesis{f.runtimes[0]->MarketStatus(f.market)->last_microblock_hash};
            f.block_commits = true;
            const auto commits_before{f.commits.load()};
            for (const auto& runtime : f.runtimes) {
                BOOST_REQUIRE(runtime->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
            }
            f.Tick();
            BOOST_REQUIRE_GT(f.commits.load(), commits_before);
            BOOST_REQUIRE(f.AllAt(1));
            const auto view_changes_before{f.view_changes.load()};
            for (size_t i{0}; i < f.runtimes.size(); ++i) {
                if (gate == "paused") f.chains[i].SetTransition(f.market, node::FlowMeshSeatTransitionKind::PAUSED);
                if (gate == "keyless") f.keys[i].m_keys.clear();
                if (gate == "reconciling") f.chains[i].SetReconciled(false);
            }
            f.Tick(); // Observe the gate closing before advancing the clock.
            for (size_t interval{0}; interval < 130; ++interval) f.Tick(std::chrono::seconds{3});
            BOOST_CHECK(f.AllAt(1));
            BOOST_CHECK_EQUAL(f.view_changes.load(), view_changes_before);
            std::string error;
            for (size_t i{0}; i < f.runtimes.size(); ++i) {
                BOOST_CHECK_EQUAL(f.runtimes[i]->MarketStatus(f.market)->round, 0U);
                std::optional<uint256> lock;
                BOOST_REQUIRE(f.stores[i]->ReadLock({0, 1}, lock, error));
                BOOST_CHECK(!lock);
                std::optional<node::StoredProductionEntry> stored;
                BOOST_REQUIRE(f.stores[i]->ReadEntry(0, f.seats.seats, stored, error));
                BOOST_REQUIRE(stored);
                BOOST_CHECK(stored->entry.GetHash() == genesis);
                if (gate == "paused") f.chains[i].SetTransition(f.market, node::FlowMeshSeatTransitionKind::CONTINUE);
                if (gate == "keyless") f.keys[i].m_keys[f.market] = {f.seats.secrets[i]};
                if (gate == "reconciling") f.chains[i].SetReconciled(true);
            }
            f.block_commits = false;
            f.Tick();
            f.Reach(2);
            const uint256 deposit{f.runtimes[0]->MarketStatus(f.market)->last_microblock_hash};
            for (size_t i{0}; i < f.runtimes.size(); ++i) {
                std::optional<node::StoredProductionEntry> stored;
                BOOST_REQUIRE(f.stores[i]->ReadEntry(1, f.seats.seats, stored, error));
                BOOST_REQUIRE(stored);
                BOOST_CHECK(stored->entry.GetHash() == deposit);
                BOOST_CHECK(stored->entry.parent_hash == genesis);
                BOOST_CHECK_EQUAL(f.runtimes[i]->StateSnapshot(f.market)->LedgerView().Available(f.account, f.asset), 250);
            }
            // Reopen the same journal and certified history; no bootstrap or
            // view reset is needed to recover from the signing pause.
            f.Drain();
            f.StopNode(0);
            BOOST_REQUIRE_MESSAGE(f.StartNode(0, f.AgreementPath(0), std::nullopt, error), error);
            BOOST_CHECK(f.AllAt(2));
            BOOST_CHECK(f.runtimes[0]->MarketStatus(f.market)->last_microblock_hash == deposit);
        }
    }
}

BOOST_AUTO_TEST_CASE(preagreement_runtime_idle_work_starts_fresh_timeout)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_idle_timeout", std::chrono::seconds{2}};
    f.block_commits = false;
    f.Reach(1);
    f.Tick();
    f.Tick(std::chrono::seconds{30});
    BOOST_REQUIRE(f.AllAt(1));
    const auto view_changes_before{f.view_changes.load()};
    f.block_commits = true;
    for (const auto& runtime : f.runtimes) {
        BOOST_REQUIRE(runtime->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    }
    f.Tick();
    BOOST_REQUIRE(f.AllAt(1));
    BOOST_REQUIRE_GT(f.commits.load(), 0U);
    for (const auto& runtime : f.runtimes) BOOST_CHECK_EQUAL(runtime->MarketStatus(f.market)->round, 0U);
    f.Tick(std::chrono::milliseconds{1999});
    BOOST_CHECK_EQUAL(f.view_changes.load(), view_changes_before);
    for (const auto& runtime : f.runtimes) BOOST_CHECK_EQUAL(runtime->MarketStatus(f.market)->round, 0U);
    f.Tick(std::chrono::milliseconds{1});
    BOOST_CHECK_GT(f.view_changes.load(), view_changes_before);
}

BOOST_AUTO_TEST_CASE(preagreement_runtime_missing_leader_timeout_survives_short_pauses)
{
    for (const bool interrupt : {false, true}) {
        BOOST_TEST_CONTEXT("short pauses=" << interrupt) {
            PreagreementRuntimeHarness f{m_args.GetDataDirBase() /
                fs::PathFromString(interrupt ? "preagreement_pulsed_timeout" : "preagreement_missing_leader_timeout"),
                std::chrono::seconds{2}};
            f.StopNode(0); // View-zero genesis proposer is genuinely absent.
            f.block_commits = false;
            f.Tick();
            for (size_t pulse{0}; pulse < 4; ++pulse) {
                f.Tick(std::chrono::milliseconds{400});
                BOOST_REQUIRE(f.AllAt(0));
                if (interrupt) {
                    for (size_t i{1}; i < f.runtimes.size(); ++i) f.chains[i].SetReconciled(false);
                    f.Tick();
                    f.Tick(std::chrono::milliseconds{100});
                    for (size_t i{1}; i < f.runtimes.size(); ++i) f.chains[i].SetReconciled(true);
                    f.Tick();
                }
            }
            f.Tick(std::chrono::milliseconds{399});
            BOOST_REQUIRE(f.AllAt(0));
            BOOST_CHECK_EQUAL(f.view_changes.load(), 0U);
            for (size_t i{1}; i < f.runtimes.size(); ++i) BOOST_CHECK_EQUAL(f.runtimes[i]->MarketStatus(f.market)->round, 0U);
            f.Tick(std::chrono::milliseconds{1});
            BOOST_CHECK_GT(f.view_changes.load(), 0U);
            f.Reach(1); // Remaining three seats form the unchanged quorum.
        }
    }
}

BOOST_AUTO_TEST_CASE(preagreement_runtime_keyless_observer_accepts_quorum)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_observer_timeout", std::chrono::seconds{2}};
    f.Drain();
    f.keys[3].m_keys.clear();
    f.block_commits = false;
    f.Tick();
    f.Reach(1);
    const uint256 genesis{f.runtimes[0]->MarketStatus(f.market)->last_microblock_hash};
    BOOST_CHECK(f.runtimes[3]->MarketStatus(f.market)->last_microblock_hash == genesis);
    BOOST_CHECK_EQUAL(f.view_changes.load(), 0U);
}

BOOST_AUTO_TEST_CASE(preagreement_action_admission_wakes_proposal_without_periodic_tick)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_action_wakeup", std::chrono::seconds{2}};
    f.block_commits = false;
    f.Reach(1);
    f.Tick(std::chrono::seconds{30}); // Idle time is not active proposal time.
    BOOST_REQUIRE(f.AllAt(1));
    const auto submitted_at{f.clocks[0].Now()};
    const auto decisions_before{f.decisions.load()};

    // Seat zero is not sequence one's proposer. Its fresh ACTION must wake
    // the remote proposer too; there is no ticker in this harness and no
    // NotifyTick/clock advance between submission and durable certification.
    BOOST_REQUIRE(f.runtimes[0]->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] { return f.AllAt(2); }));
    f.Drain();
    BOOST_CHECK_GT(f.decisions.load(), decisions_before);
    BOOST_CHECK_EQUAL(f.view_changes.load(), 0U);
    std::string error;
    for (size_t i{0}; i < f.runtimes.size(); ++i) {
        BOOST_CHECK(f.clocks[i].Now() == submitted_at);
        std::optional<node::StoredProductionEntry> stored;
        BOOST_REQUIRE(f.stores[i]->ReadEntry(1, f.seats.seats, stored, error));
        BOOST_REQUIRE(stored);
        BOOST_REQUIRE_EQUAL(stored->entry.actions.size(), 1U);
        BOOST_CHECK(stored->entry.actions.front().Id() == Deposit(f.outpoint).Id());
        BOOST_CHECK_EQUAL(f.runtimes[i]->StateSnapshot(f.market)->LedgerView().Available(f.account, f.asset), 250);
    }

    // Neither the wake-up nor ordinary idle maintenance manufactures an
    // actionless entry after the admitted action has been consumed.
    const auto decisions_after{f.decisions.load()};
    f.Tick();
    f.Tick(std::chrono::seconds{30});
    BOOST_CHECK(f.AllAt(2));
    BOOST_CHECK_EQUAL(f.decisions.load(), decisions_after);
    BOOST_CHECK_EQUAL(f.view_changes.load(), 0U);
}

BOOST_AUTO_TEST_CASE(preagreement_local_queued_action_survives_certified_head_advance)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_local_queued_head_advance"};
    f.block_commits = false;
    f.Reach(1);
    f.Drain();
    CKey account_key;
    account_key.MakeNewKey(true);
    const auto account{flowmesh::AccountForKey(XOnlyPubKey{account_key.GetPubKey()})};
    f.deposits.entries.at(f.outpoint) = {f.asset, 250, account};
    flowmesh::Action tail;
    tail.signer = account;
    tail.type = static_cast<uint8_t>(flowmesh::ActionType::SUBMIT_ASK);
    tail.curve = {{10, 5}};
    BOOST_REQUIRE(flowmesh::SignAction(account_key, f.domain, f.initial.ConfigId(), tail));
    const auto payload{flowmesh::EncodeProductionActionPayload(tail)};
    BOOST_REQUIRE(payload);
    const auto submitted_at{f.clocks[0].Now()};
    // This is exactly SubmitLocalAction's encoded queue item at head one.
    // Use its existing queue seam so the test can hold the worker's market
    // lock without blocking a second SubmitLocalAction's snapshot acquisition.
    const auto local_peer{std::numeric_limits<flowmesh::WirePeerId>::min()};
    const flowmesh::WireMessage queued{flowmesh::WireMessageKind::ACTION,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 1}, *payload};
    std::mutex barrier_mutex;
    std::condition_variable barrier_cv;
    bool entered{false}, released{false}, barrier_timed_out{false};
    std::atomic<bool> certificate_queued{false};
    std::vector<flowmesh::WireMessage> tail_relays;
    struct ReleaseBeforeStop {
        PreagreementRuntimeHarness<>& fixture;
        std::mutex& mutex;
        std::condition_variable& cv;
        bool& released;
        ~ReleaseBeforeStop()
        {
            { std::lock_guard lock{mutex}; released = true; }
            cv.notify_all();
            fixture.StopAll(); // callbacks finish before captured locals die
        }
    } release_before_stop{f, barrier_mutex, barrier_cv, released};
    f.network.SetFilter([&](size_t from, size_t to, const flowmesh::WireMessage& wire) {
        if (from == 0 && wire.kind == flowmesh::WireMessageKind::GET) {
            std::unique_lock lock{barrier_mutex};
            entered = true;
            barrier_cv.notify_all();
            barrier_timed_out = !barrier_cv.wait_for(lock, std::chrono::seconds{15}, [&] { return released; });
            return false; // this request is only the deterministic worker barrier
        }
        if (to == 0 && wire.kind == flowmesh::WireMessageKind::CERTIFICATE && wire.header.sequence == 1) {
            // Publish the synchronization flag only AFTER real queue admission.
            const auto result{f.runtimes[0]->EnqueueWireMessage(static_cast<flowmesh::WirePeerId>(from), wire)};
            if (result == flowmesh::QueueResult::ACCEPTED) certificate_queued = true;
            return false; // do not enqueue a second copy through RuntimeNetwork
        }
        if (from == 0 && wire.kind == flowmesh::WireMessageKind::ACTION && wire.payload == *payload) {
            std::lock_guard lock{barrier_mutex};
            tail_relays.push_back(wire);
        }
        return f.Observe(wire);
    });
    BOOST_REQUIRE(f.runtimes[0]->RequestCatchup(1, f.market));
    {
        std::unique_lock lock{barrier_mutex};
        BOOST_REQUIRE(barrier_cv.wait_for(lock, std::chrono::seconds{5}, [&] { return entered; }));
    }
    BOOST_REQUIRE(f.runtimes[0]->EnqueueWireMessage(local_peer, queued) == flowmesh::QueueResult::ACCEPTED);
    // The other three real seats form the unchanged quorum while node zero's
    // worker is held. Its already-queued local tail has not reached the pool.
    BOOST_REQUIRE(f.runtimes[1]->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] {
        return certificate_queued && std::all_of(f.runtimes.begin() + 1, f.runtimes.end(), [&](const auto& runtime) {
            return runtime->MarketStatus(f.market)->next_sequence == 2;
        });
    }));
    {
        std::lock_guard lock{barrier_mutex};
        BOOST_REQUIRE(!barrier_timed_out);
        released = true;
    }
    barrier_cv.notify_all();
    f.Drain();
    // The existing certificate-priority lane advances the durable head before
    // dequeuing the earlier ACTION lane item. Before the repair, this exact
    // assertion fails: HandleAction silently discards its now-old wrapper.
    const auto events{f.runtimes[0]->ClientEvents(std::nullopt, f.market, account).events};
    const auto admitted{std::find_if(events.begin(), events.end(), [&](const auto& event) {
        return event.kind == flowmesh::ClientEventKind::POOL_ADMITTED && event.action_id == tail.Id();
    })};
    BOOST_REQUIRE_MESSAGE(admitted != events.end(), "queued local tail lost before pool admission after certified head advanced");
    BOOST_CHECK_EQUAL(admitted->microblock_sequence, 2U);
    BOOST_CHECK(admitted->signed_action_hash == Hash(*payload));
    BOOST_REQUIRE(WaitUntil([&] { return f.AllAt(3); }));
    f.Drain(); // no new client action, manual tick or clock advance was needed
    {
        std::lock_guard lock{barrier_mutex};
        BOOST_REQUIRE(!tail_relays.empty());
        for (const auto& wire : tail_relays) {
            BOOST_CHECK(wire.payload == *payload);
            BOOST_CHECK(wire.header == flowmesh::WireHeader(flowmesh::FLOWMESH_WIRE_VERSION_V1,
                f.market, f.seats.seats.epoch, 2));
        }
    }
    std::string error;
    for (size_t i{0}; i < f.runtimes.size(); ++i) {
        BOOST_CHECK(f.clocks[i].Now() == submitted_at);
        std::optional<node::StoredProductionEntry> stored;
        BOOST_REQUIRE(f.stores[i]->ReadEntry(2, f.seats.seats, stored, error));
        BOOST_REQUIRE(stored);
        BOOST_REQUIRE_EQUAL(stored->entry.actions.size(), 1U);
        BOOST_CHECK(stored->entry.actions.front().Id() == tail.Id());
        BOOST_CHECK(flowmesh::CheckProductionEntryCertificate(stored->entry, f.seats.seats, stored->certificate) ==
                    flowmesh::BlsCertificateCheck::OK);
        BOOST_CHECK_EQUAL(f.runtimes[i]->StateSnapshot(f.market)->NextSequence(account), 1U);
    }
    const auto head{f.runtimes[0]->MarketStatus(f.market)->last_microblock_hash};
    auto replay_header{queued.header};
    replay_header.sequence = 2; // one-head check: terminal tail and consumed older deposit
    for (const auto& replay : {flowmesh::WireMessage{flowmesh::WireMessageKind::ACTION, replay_header, *payload},
            flowmesh::WireMessage{flowmesh::WireMessageKind::ACTION, replay_header,
                *flowmesh::EncodeProductionActionPayload(Deposit(f.outpoint))}}) {
        const auto action{flowmesh::DecodeProductionActionPayload(replay.payload)};
        BOOST_REQUIRE(action);
        const auto before{f.runtimes[0]->ClientEventCursor()};
        BOOST_REQUIRE(f.runtimes[0]->EnqueueWireMessage(local_peer, replay) == flowmesh::QueueResult::ACCEPTED);
        f.Drain();
        // A local resend of an already-executed action is answered by its
        // certified inclusion: no refusal event that could later read as
        // "rejected" once the inclusion event leaves the ring, no admission.
        const auto attempts{f.runtimes[0]->ClientEvents(before, f.market, std::nullopt).events};
        BOOST_CHECK(std::none_of(attempts.begin(), attempts.end(), [&](const auto& event) {
            return event.action_id == action->Id() && (event.kind == flowmesh::ClientEventKind::POOL_REFUSED ||
                                                       event.kind == flowmesh::ClientEventKind::POOL_ADMITTED);
        }));
        const auto status{f.runtimes[0]->ClientActionStatus(f.market, action->Id())};
        BOOST_REQUIRE(status);
        BOOST_CHECK(status->kind == flowmesh::ClientEventKind::CERTIFIED_INCLUDED);
        BOOST_CHECK_EQUAL(f.runtimes[0]->MarketStatus(f.market)->pending_actions, 0U);
        BOOST_CHECK(f.runtimes[0]->MarketStatus(f.market)->last_microblock_hash == head);
    }

    // A fresh, valid instruction has no known terminal disposition. A
    // wrapper more than two durable heads old is beyond the relay window:
    // refuse explicitly instead of guessing or scanning storage; the caller
    // must use existing exact-action recovery.
    f.block_commits = true;
    auto older_tail{tail};
    older_tail.sequence = 1;
    older_tail.type = static_cast<uint8_t>(flowmesh::ActionType::CANCEL_ASK);
    older_tail.curve.clear();
    BOOST_REQUIRE(flowmesh::SignAction(account_key, f.domain, f.initial.ConfigId(), older_tail));
    const auto older_payload{flowmesh::EncodeProductionActionPayload(older_tail)};
    BOOST_REQUIRE(older_payload);
    auto three_behind{queued.header};
    three_behind.sequence = 0;
    BOOST_REQUIRE(f.runtimes[0]->EnqueueWireMessage(local_peer,
        {flowmesh::WireMessageKind::ACTION, three_behind, *older_payload}) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    const auto older_events{f.runtimes[0]->ClientEvents(std::nullopt, f.market, account).events};
    BOOST_CHECK(std::none_of(older_events.begin(), older_events.end(), [&](const auto& event) {
        return event.kind == flowmesh::ClientEventKind::POOL_ADMITTED && event.action_id == older_tail.Id();
    }));
    BOOST_CHECK(std::any_of(older_events.begin(), older_events.end(), [&](const auto& event) {
        return event.kind == flowmesh::ClientEventKind::POOL_REFUSED && event.action_id == older_tail.Id() &&
               event.reason.find("complete retained certified-head interval") != std::string::npos;
    }));
    BOOST_CHECK_EQUAL(f.runtimes[0]->MarketStatus(f.market)->pending_actions, 0U);
    BOOST_CHECK(f.runtimes[0]->MarketStatus(f.market)->last_microblock_hash == head);

    // Its wrapper TWO durable heads old is proven over the recent window
    // (both retained bodies exclude it, it is still pending) and relayed with
    // this node's head; the signed payload is unchanged.
    const auto restamped_before{f.runtimes[0]->DeliverySnapshots(f.market).front().action_admission.header_restamped_behind};
    BOOST_REQUIRE(f.runtimes[0]->EnqueueWireMessage(local_peer,
        {flowmesh::WireMessageKind::ACTION, queued.header, *older_payload}) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    BOOST_CHECK_EQUAL(f.runtimes[0]->MarketStatus(f.market)->pending_actions, 1U);
    BOOST_CHECK_EQUAL(f.runtimes[0]->DeliverySnapshots(f.market).front().action_admission.header_restamped_behind,
                      restamped_before + 1);
}

BOOST_AUTO_TEST_CASE(preagreement_queued_certified_but_rejected_deposit_is_terminal)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_queued_rejected_deposit"};
    f.block_commits = false;
    f.Reach(1);
    f.Drain();
    // Both deposits have positive, bounded anchored facts. The second cannot
    // credit an already-full vault, so certification does NOT consume it.
    f.deposits.entries.at(f.outpoint).amount = MAX_MONEY;
    const COutPoint rejected_outpoint{f.outpoint.hash, 1};
    f.deposits.entries.emplace(rejected_outpoint, flowmesh::DepositInfo{f.asset, 1, f.account});
    BOOST_REQUIRE(f.runtimes[1]->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] { return f.AllAt(2); }));
    f.Drain();
    const auto action{Deposit(rejected_outpoint)};
    const auto payload{flowmesh::EncodeProductionActionPayload(action)};
    BOOST_REQUIRE(payload);
    const auto local_peer{std::numeric_limits<flowmesh::WirePeerId>::min()};
    const flowmesh::WireMessage queued{flowmesh::WireMessageKind::ACTION,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 2}, *payload};
    // This is a controlled certified-recovery edge, not a claim that the
    // current honest leader selects this rejected body without a view change.
    // Use the existing production builder and actual quorum BLS signatures.
    std::string error;
    std::optional<node::StoredProductionEntry> previous;
    BOOST_REQUIRE(f.stores[0]->ReadEntry(1, f.seats.seats, previous, error));
    BOOST_REQUIRE(previous);
    const auto before{f.runtimes[0]->StateSnapshot(f.market)};
    const auto before_status{f.runtimes[0]->MarketStatus(f.market)};
    BOOST_REQUIRE(before);
    BOOST_REQUIRE(before_status);
    flowmesh::ProductionEpochGate gate{f.domain, f.market, f.seats.seats};
    flowmesh::ProductionEntryCheck check;
    const std::array actions{action};
    const auto built{flowmesh::BuildProductionExecutionEntry(
        *before, f.domain, f.market, f.seats.seats, gate, 2,
        before_status->next_effect_index, before_status->last_microblock_hash,
        f.chains[0].Current(), {f.chains[0].TipHeight(), previous->entry.anchor, &f.chains[0]},
        f.treasury, actions, &f.deposits, check)};
    BOOST_REQUIRE(built);
    BOOST_CHECK(!built->next_state.DepositConsumed(rejected_outpoint));
    const auto certified_payload{flowmesh::EncodeProductionCertifiedPayload(
        {built->entry, Certify(built->entry, f.seats)}, f.seats.seats.Size())};
    BOOST_REQUIRE(certified_payload);
    f.block_commits = true;
    std::mutex barrier_mutex;
    std::condition_variable barrier_cv;
    bool entered{false}, released{false}, barrier_timed_out{false};
    struct ReleaseBeforeStop {
        PreagreementRuntimeHarness<>& fixture;
        std::mutex& mutex;
        std::condition_variable& cv;
        bool& released;
        ~ReleaseBeforeStop()
        {
            { std::lock_guard lock{mutex}; released = true; }
            cv.notify_all();
            fixture.StopAll();
        }
    } release_before_stop{f, barrier_mutex, barrier_cv, released};
    f.network.SetFilter([&](size_t from, size_t, const flowmesh::WireMessage& wire) {
        if (from == 0 && wire.kind == flowmesh::WireMessageKind::GET) {
            std::unique_lock lock{barrier_mutex};
            entered = true;
            barrier_cv.notify_all();
            barrier_timed_out = !barrier_cv.wait_for(lock, std::chrono::seconds{15}, [&] { return released; });
            return false;
        }
        return f.Observe(wire);
    });
    BOOST_REQUIRE(f.runtimes[0]->RequestCatchup(1, f.market));
    {
        std::unique_lock lock{barrier_mutex};
        BOOST_REQUIRE(barrier_cv.wait_for(lock, std::chrono::seconds{5}, [&] { return entered; }));
    }
    BOOST_REQUIRE(f.runtimes[0]->EnqueueWireMessage(local_peer, queued) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(f.runtimes[0]->EnqueueWireMessage(1,
        {flowmesh::WireMessageKind::CERTIFICATE, queued.header, *certified_payload}) == flowmesh::QueueResult::ACCEPTED);
    // An erroneous re-admission stays observable without permitting another
    // certificate or turning the regression into an unbounded retry loop.
    {
        std::lock_guard lock{barrier_mutex};
        BOOST_REQUIRE(!barrier_timed_out);
        released = true;
    }
    barrier_cv.notify_all();
    f.Drain();
    BOOST_REQUIRE_EQUAL(f.runtimes[0]->MarketStatus(f.market)->next_sequence, 3U);
    const auto state{f.runtimes[0]->StateSnapshot(f.market)};
    BOOST_REQUIRE(state);
    BOOST_CHECK(!state->DepositConsumed(rejected_outpoint));
    BOOST_CHECK_EQUAL(state->LedgerView().Available(f.account, f.asset), MAX_MONEY);
    std::optional<node::StoredProductionEntry> stored;
    BOOST_REQUIRE(f.stores[0]->ReadEntry(2, f.seats.seats, stored, error));
    BOOST_REQUIRE(stored);
    BOOST_REQUIRE_EQUAL(stored->entry.actions.size(), 1U);
    BOOST_CHECK(stored->entry.actions.front().Id() == action.Id());
    BOOST_CHECK(flowmesh::CheckProductionEntryCertificate(stored->entry, f.seats.seats, stored->certificate) ==
                flowmesh::BlsCertificateCheck::OK);
    const auto events{f.runtimes[0]->ClientEvents(std::nullopt, f.market, std::nullopt).events};
    BOOST_CHECK_MESSAGE(std::none_of(events.begin(), events.end(), [&](const auto& event) {
        return event.kind == flowmesh::ClientEventKind::POOL_ADMITTED && event.action_id == action.Id() &&
               event.microblock_sequence == 3;
    }), "certified but unapplied queued deposit was incorrectly re-admitted at the new head");
    BOOST_CHECK(std::any_of(events.begin(), events.end(), [&](const auto& event) {
        return event.kind == flowmesh::ClientEventKind::POOL_REFUSED && event.action_id == action.Id();
    }));
    BOOST_CHECK_EQUAL(f.runtimes[0]->MarketStatus(f.market)->pending_actions, 0U);
    const auto status{f.runtimes[0]->ClientActionStatus(f.market, action.Id())};
    BOOST_REQUIRE(status);
    BOOST_CHECK(status->kind == flowmesh::ClientEventKind::CERTIFIED_INCLUDED);
}

BOOST_AUTO_TEST_CASE(preagreement_stale_local_action_revalidation_preserves_remote_and_authority_bounds)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_stale_local_action_bounds"};
    f.block_commits = false;
    f.Reach(1);
    f.Drain();
    f.block_commits = true; // no accidental admission can finish an execution
    CKey account_key;
    account_key.MakeNewKey(true);
    const auto account{flowmesh::AccountForKey(XOnlyPubKey{account_key.GetPubKey()})};
    flowmesh::Action action;
    action.signer = account;
    action.type = static_cast<uint8_t>(flowmesh::ActionType::CANCEL_ASK);
    BOOST_REQUIRE(flowmesh::SignAction(account_key, f.domain, f.initial.ConfigId(), action));
    const auto payload{flowmesh::EncodeProductionActionPayload(action)};
    BOOST_REQUIRE(payload);
    const auto local_peer{std::numeric_limits<flowmesh::WirePeerId>::min()};
    const flowmesh::WireHeader stale{flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 0};
    const uint256 head{f.runtimes[0]->MarketStatus(f.market)->last_microblock_hash};
    const auto refuse = [&](flowmesh::WirePeerId peer, flowmesh::WireMessage wire, bool observable) {
        const auto decoded{flowmesh::DecodeProductionActionPayload(wire.payload)};
        BOOST_REQUIRE(decoded);
        const auto before{f.runtimes[0]->ClientEvents(std::nullopt, f.market, std::nullopt).cursor};
        BOOST_REQUIRE(f.runtimes[0]->EnqueueWireMessage(peer, wire) == flowmesh::QueueResult::ACCEPTED);
        f.Drain();
        BOOST_CHECK_EQUAL(f.runtimes[0]->MarketStatus(f.market)->pending_actions, 0U);
        BOOST_CHECK(f.runtimes[0]->MarketStatus(f.market)->last_microblock_hash == head);
        const auto events{f.runtimes[0]->ClientEvents(before, f.market, std::nullopt).events};
        BOOST_CHECK(std::none_of(events.begin(), events.end(), [](const auto& event) {
            return event.kind == flowmesh::ClientEventKind::POOL_ADMITTED;
        }));
        if (observable) BOOST_CHECK(std::any_of(events.begin(), events.end(), [&](const auto& event) {
            return event.kind == flowmesh::ClientEventKind::POOL_REFUSED && event.action_id == decoded->Id();
        }));
    };
    // Remote frames outside the two-slot relay window stay refused and are
    // only counted (no client event).
    auto remote_wrong_epoch{stale};
    ++remote_wrong_epoch.epoch;
    refuse(2, {flowmesh::WireMessageKind::ACTION, remote_wrong_epoch, *payload}, false);
    auto remote_far{stale};
    remote_far.sequence = 1 + 3;
    refuse(2, {flowmesh::WireMessageKind::ACTION, remote_far, *payload}, false);
    const auto remote_counters{f.runtimes[0]->DeliverySnapshots(f.market).front().action_admission};
    BOOST_CHECK_EQUAL(remote_counters.header_refused_identity, 1U);
    BOOST_CHECK_EQUAL(remote_counters.header_refused_far, 1U);
    auto wrong_epoch{stale};
    ++wrong_epoch.epoch;
    refuse(local_peer, {flowmesh::WireMessageKind::ACTION, wrong_epoch, *payload}, true);
    auto future{stale};
    future.sequence = 2;
    refuse(local_peer, {flowmesh::WireMessageKind::ACTION, future, *payload}, true);
    auto forged{action};
    BOOST_REQUIRE(!forged.credential.empty());
    forged.credential.back() ^= 1;
    refuse(local_peer, {flowmesh::WireMessageKind::ACTION, stale,
                       *flowmesh::EncodeProductionActionPayload(forged)}, true);
    const auto missing{Deposit(COutPoint{f.outpoint.hash, 99})};
    refuse(local_peer, {flowmesh::WireMessageKind::ACTION, stale,
                       *flowmesh::EncodeProductionActionPayload(missing)}, true);
    f.chains[0].SetReconciled(false);
    refuse(local_peer, {flowmesh::WireMessageKind::ACTION, stale, *payload}, true);
    f.chains[0].SetReconciled(true);
    f.chains[0].SetTransition(f.market, node::FlowMeshSeatTransitionKind::PAUSED);
    refuse(local_peer, {flowmesh::WireMessageKind::ACTION, stale, *payload}, true);

    // A remote sender one slot behind is admitted once the retained interval
    // proves it: remote frames never had the authority gate (an equal-header
    // remote frame is admitted while paused too), and add no chain call.
    BOOST_REQUIRE(f.runtimes[0]->EnqueueWireMessage(2, {flowmesh::WireMessageKind::ACTION, stale, *payload}) ==
                  flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    BOOST_CHECK_EQUAL(f.runtimes[0]->MarketStatus(f.market)->pending_actions, 1U);
    BOOST_CHECK_EQUAL(f.runtimes[0]->DeliverySnapshots(f.market).front().action_admission.header_restamped_behind, 1U);

    // Local identity does not bypass the existing bounded wire admission.
    flowmesh::BoundedWireQueue queue;
    const flowmesh::WireMessage wire{flowmesh::WireMessageKind::ACTION, stale, *payload};
    const auto limit{static_cast<size_t>(flowmesh::FLOWMESH_ACTION_TOKEN_COUNT_BURST)};
    for (size_t i{0}; i < limit; ++i) BOOST_REQUIRE(queue.Push(local_peer, wire, f.clocks[0].Now()) == flowmesh::QueueResult::ACCEPTED);
    BOOST_CHECK(queue.Push(local_peer, wire, f.clocks[0].Now()) == flowmesh::QueueResult::RATE_LIMITED);
    BOOST_CHECK_EQUAL(queue.Size(), limit);
    for (size_t i{0}; i < limit; ++i) {
        const auto item{queue.Pop()};
        BOOST_REQUIRE(item);
        BOOST_CHECK(item->message.payload == *payload);
    }
    BOOST_CHECK(queue.Empty());
}

BOOST_AUTO_TEST_CASE(preagreement_duplicate_actions_do_not_schedule_proposal_ticks)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_duplicate_wakeup", std::chrono::seconds{2}};
    f.block_commits = false;
    f.Reach(1);
    f.Tick(std::chrono::seconds{30});
    f.block_commits = true;
    const auto commits_before{f.commits.load()};
    BOOST_REQUIRE(f.runtimes[0]->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] { return f.commits.load() > commits_before; }));
    f.Drain();
    BOOST_REQUIRE(f.AllAt(1));
    const auto committed_votes{f.commits.load()};
    const auto cursor{f.runtimes[0]->DeliverySnapshots(f.market).front().last_event_id};
    const auto payload{flowmesh::EncodeProductionActionPayload(Deposit(f.outpoint))};
    BOOST_REQUIRE(payload);
    const flowmesh::WireMessage duplicate{flowmesh::WireMessageKind::ACTION,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 1}, *payload};
    for (size_t copy{0}; copy < 8; ++copy) {
        BOOST_REQUIRE(f.runtimes[0]->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(f.runtimes[0]->EnqueueWireMessage(1, duplicate) == flowmesh::QueueResult::ACCEPTED);
    }
    f.Drain();
    const auto snapshot{f.runtimes[0]->DeliverySnapshots(f.market).front()};
    BOOST_REQUIRE(!snapshot.events.empty());
    BOOST_REQUIRE_LE(snapshot.events.front().event_id, cursor + 1); // No new event escaped the bounded ring.
    BOOST_CHECK(std::none_of(snapshot.events.begin(), snapshot.events.end(), [&](const auto& event) {
        return event.event_id > cursor && event.stage == "tick_market_lock_acquired";
    }));
    BOOST_CHECK_EQUAL(f.commits.load(), committed_votes);
    BOOST_CHECK_EQUAL(f.view_changes.load(), 0U);
    BOOST_CHECK_EQUAL(f.runtimes[0]->MarketStatus(f.market)->pending_actions, 1U);
    BOOST_CHECK(f.AllAt(1));
}

BOOST_AUTO_TEST_CASE(preagreement_action_wakeup_respects_reconciliation_pause)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_paused_wakeup", std::chrono::seconds{2}};
    f.block_commits = false;
    f.Reach(1);
    f.Tick(std::chrono::seconds{30});
    const auto prepares_before{f.prepares.load()};
    for (auto& chain : f.chains) chain.SetReconciled(false);
    // Submit directly to the proposer while its production gate is closed.
    BOOST_REQUIRE(f.runtimes[1]->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    BOOST_CHECK(f.AllAt(1));
    BOOST_CHECK_EQUAL(f.prepares.load(), prepares_before);
    BOOST_CHECK_EQUAL(f.view_changes.load(), 0U);
    std::string error;
    for (size_t i{0}; i < f.runtimes.size(); ++i) {
        std::optional<uint256> lock;
        BOOST_REQUIRE(f.stores[i]->ReadLock({0, 1}, lock, error));
        BOOST_CHECK(!lock);
        BOOST_CHECK_EQUAL(f.runtimes[i]->MarketStatus(f.market)->round, 0U);
    }
    // Recovery still uses the existing periodic path and the exact admitted
    // action. The admission event does not add a separate recovery timer.
    for (auto& chain : f.chains) chain.SetReconciled(true);
    f.Tick(std::chrono::milliseconds{250});
    f.Reach(2);
    for (const auto& runtime : f.runtimes) {
        BOOST_CHECK_EQUAL(runtime->StateSnapshot(f.market)->LedgerView().Available(f.account, f.asset), 250);
    }
}

BOOST_AUTO_TEST_CASE(preagreement_action_wakeup_arms_initial_timeout_without_periodic_tick)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_wakeup_timeout", std::chrono::seconds{2}};
    f.block_commits = false;
    f.Reach(1);
    f.Tick(std::chrono::seconds{30});
    f.StopNode(1); // Sequence one's initial proposer is absent.
    BOOST_REQUIRE(f.runtimes[0]->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    f.Drain(); // No manual tick: admission starts each eligible local clock.
    BOOST_REQUIRE(f.AllAt(1));
    f.Tick(std::chrono::milliseconds{1999});
    BOOST_CHECK_EQUAL(f.view_changes.load(), 0U);
    f.Tick(std::chrono::milliseconds{1});
    BOOST_CHECK_GT(f.view_changes.load(), 0U);
    f.Reach(2);
}

// The agreement engine re-validates the candidate for every message, retry and
// finalization of a slot. A follower executes each entry once; the leader
// executes it twice, once to build it and once to verify it independently
// before its first agreement signature. Nothing else executes it again while
// the stamped execution context is unchanged. The FMN2 link model delivers in
// bounded waves, so each node's 128-event ring is read before it can wrap.
BOOST_AUTO_TEST_CASE(preagreement_candidate_executes_once_per_unchanged_context)
{
    std::array<ExecutionLog, 4> logs;
    // A long round keeps every retry below inside view zero.
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_execution_once",
                                 std::chrono::seconds{60}, 1, true};
    f.after_drain = [&] {
        for (size_t i{0}; i < f.runtimes.size(); ++i) logs[i].Sample(*f.runtimes[i], f.market);
    };
    const auto leader = [&](const uint64_t sequence) -> size_t {
        return flowmesh::ProductionProposerSeatIndex(sequence, 0, f.seats.seats.Size());
    };
    // An idle leader may build (and discard) an empty entry on a tick; only
    // the build of the proposed entry is fixed, so builds are compared across
    // the slot rather than counted from the start.
    const auto builds = [&](const uint64_t sequence) {
        return logs[leader(sequence)].Executions(sequence, "build_proposal");
    };
    const auto check = [&](const uint64_t sequence, const size_t proposed_builds) {
        BOOST_CHECK_GE(proposed_builds, 1U);
        BOOST_CHECK_EQUAL(builds(sequence), proposed_builds);
        for (size_t i{0}; i < f.runtimes.size(); ++i) {
            BOOST_TEST_CONTEXT("node " << i << " sequence " << sequence) {
                if (i != leader(sequence)) BOOST_CHECK_EQUAL(logs[i].Executions(sequence, "build_proposal"), 0U);
                BOOST_CHECK_EQUAL(logs[i].Executions(sequence, "evaluate_candidate"), 1U);
            }
        }
    };
    const auto steps = [&](const size_t count) { for (size_t i{0}; i < count; ++i) f.Step(); };

    // Genesis: proposal and prepares, with every COMMIT held so the slot keeps
    // re-validating through four seconds of retries and re-sent votes.
    steps(80);
    BOOST_REQUIRE_GT(f.prepares.load(), 0U);
    BOOST_REQUIRE(f.AllAt(0));
    BOOST_CHECK_EQUAL(builds(0), 1U);
    check(0, 1);
    f.block_commits = false;
    BOOST_REQUIRE(f.StepUntil(1, std::chrono::seconds{10}));
    steps(20);
    BOOST_REQUIRE_GT(f.decisions.load(), 0U);
    check(0, 1);

    // An action-bearing slot re-authenticates its evidence on every call but
    // still executes the entry once per node, plus the leader's build.
    f.block_commits = true;
    for (const auto& runtime : f.runtimes) {
        BOOST_REQUIRE(runtime->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    }
    steps(10);
    BOOST_REQUIRE_GT(logs[leader(1)].Executions(1, "evaluate_candidate"), 0U);
    const size_t proposed{builds(1)};
    steps(70);
    BOOST_REQUIRE(f.AllAt(1));
    check(1, proposed);
    f.block_commits = false;
    BOOST_REQUIRE(f.StepUntil(2, std::chrono::seconds{10}));
    steps(20);
    check(1, proposed);
    for (const auto& runtime : f.runtimes) {
        BOOST_CHECK_EQUAL(runtime->StateSnapshot(f.market)->LedgerView().Available(f.account, f.asset), 250);
    }
}

// Every context change that execution depends on forces exactly one fresh
// execution, after which the refreshed result is reused again.
BOOST_AUTO_TEST_CASE(preagreement_candidate_reexecutes_after_generation_or_tip_change)
{
    std::array<ExecutionLog, 4> logs;
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_execution_context",
                                 std::chrono::seconds{60}, 1, true};
    f.after_drain = [&] {
        for (size_t i{0}; i < f.runtimes.size(); ++i) logs[i].Sample(*f.runtimes[i], f.market);
    };
    const auto evaluated = [&](const size_t i) { return logs[i].Executions(0, "evaluate_candidate"); };
    const auto steps = [&](const size_t count) { for (size_t i{0}; i < count; ++i) f.Step(); };
    steps(40);
    BOOST_REQUIRE(f.AllAt(0));
    for (size_t i{0}; i < f.runtimes.size(); ++i) BOOST_REQUIRE_EQUAL(evaluated(i), 1U);

    // A reconciliation that started and ended while node 1 was idle.
    f.chains[1].BumpDeliveryGeneration();
    steps(60);
    BOOST_CHECK_EQUAL(evaluated(0), 1U);
    BOOST_CHECK_EQUAL(evaluated(1), 2U);
    BOOST_CHECK_EQUAL(evaluated(2), 1U);
    BOOST_CHECK_EQUAL(evaluated(3), 1U);

    // A new B3 tip on node 2.
    f.chains[2].SetTipHeight(f.chains[2].TipHeight() + 1);
    steps(60);
    BOOST_CHECK_EQUAL(evaluated(1), 2U);
    BOOST_CHECK_EQUAL(evaluated(2), 2U);

    // Both at once on node 3 still cost one execution.
    f.chains[3].BumpDeliveryGeneration();
    f.chains[3].SetTipHeight(f.chains[3].TipHeight() + 1);
    steps(60);
    BOOST_CHECK_EQUAL(evaluated(0), 1U);
    BOOST_CHECK_EQUAL(evaluated(1), 2U);
    BOOST_CHECK_EQUAL(evaluated(2), 2U);
    BOOST_CHECK_EQUAL(evaluated(3), 2U);

    // The refreshed candidates decide, certify and apply the same entry.
    f.block_commits = false;
    BOOST_REQUIRE(f.StepUntil(1, std::chrono::seconds{10}));
    steps(20);
    for (size_t i{0}; i < f.runtimes.size(); ++i) {
        BOOST_CHECK_EQUAL(evaluated(i), i == 0 ? 1U : 2U);
        BOOST_CHECK(f.runtimes[i]->MarketStatus(f.market)->last_microblock_hash ==
                    f.runtimes[0]->MarketStatus(f.market)->last_microblock_hash);
    }
}

// A retained, already executed candidate must not be locked or attested while
// the seat transition is PAUSED, even when its execution context is unchanged
// and a complete COMMIT quorum and DECISION arrive. The transition kind is a
// live gate, not part of the reusable execution result.
BOOST_AUTO_TEST_CASE(preagreement_decided_candidate_not_attested_while_transition_paused)
{
    std::array<std::atomic<size_t>, 4> attestations{};
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_paused_reuse", std::chrono::seconds{60}};
    f.network.SetFilter([&](const size_t from, size_t, const flowmesh::WireMessage& wire) {
        if (wire.kind == flowmesh::WireMessageKind::ATTESTATION) ++attestations.at(from);
        return f.Observe(wire);
    });
    std::string error;
    // Every node executes and retains the genesis candidate under CONTINUE.
    f.Tick();
    BOOST_REQUIRE_GT(f.commits.load(), 0U);
    BOOST_REQUIRE(f.AllAt(0));

    // Two seats pause. The other two can decide but cannot certify alone.
    const std::array<size_t, 2> paused{2, 3};
    for (const size_t i : paused) f.chains[i].SetTransition(f.market, node::FlowMeshSeatTransitionKind::PAUSED);
    f.block_commits = false;
    for (size_t pass{0}; pass < 4; ++pass) f.Tick(std::chrono::seconds{1});
    BOOST_REQUIRE_GT(f.decisions.load(), 0U);
    BOOST_REQUIRE(f.AllAt(0));
    for (size_t i{0}; i < f.runtimes.size(); ++i) {
        BOOST_TEST_CONTEXT("node " << i) {
            const bool is_paused{i == paused[0] || i == paused[1]};
            std::optional<uint256> lock;
            BOOST_REQUIRE(f.stores[i]->ReadLock({0, 0}, lock, error));
            BOOST_CHECK_EQUAL(lock.has_value(), !is_paused);
            BOOST_CHECK_EQUAL(attestations[i].load() > 0, !is_paused);
        }
    }

    // Once the transition continues, the same retained candidate completes.
    for (const size_t i : paused) f.chains[i].SetTransition(f.market, node::FlowMeshSeatTransitionKind::CONTINUE);
    f.Reach(1);
    const auto genesis{f.runtimes[0]->MarketStatus(f.market)->last_microblock_hash};
    for (size_t i{0}; i < f.runtimes.size(); ++i) {
        std::optional<uint256> lock;
        BOOST_REQUIRE(f.stores[i]->ReadLock({0, 0}, lock, error));
        BOOST_REQUIRE(lock);
        BOOST_CHECK(*lock == genesis);
        BOOST_CHECK(f.runtimes[i]->MarketStatus(f.market)->last_microblock_hash == genesis);
    }
    for (const size_t i : paused) BOOST_CHECK_GT(attestations[i].load(), 0U);
}

// A certificate that leaves admitted work queued wakes only the next slot's
// proposer, which proposes at once instead of on its next periodic tick.
// Every other node keeps its periodic schedule.
BOOST_AUTO_TEST_CASE(preagreement_certified_slot_with_queued_work_proposes_without_periodic_tick)
{
    std::array<ExecutionLog, 4> logs;
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "preagreement_certified_wakeup",
                                 std::chrono::seconds{60}, 2, true};
    f.after_drain = [&] {
        for (size_t i{0}; i < f.runtimes.size(); ++i) logs[i].Sample(*f.runtimes[i], f.market);
    };
    f.block_commits = false;
    BOOST_REQUIRE(f.StepUntil(1, std::chrono::seconds{10}));
    for (size_t i{0}; i < 20; ++i) f.Step();

    // Slot one holds its first deposit at COMMIT while a second one queues.
    f.block_commits = true;
    for (const auto& runtime : f.runtimes) {
        BOOST_REQUIRE(runtime->SubmitLocalAction(f.market, Deposit(COutPoint{f.outpoint.hash, 0})) ==
                      flowmesh::QueueResult::ACCEPTED);
    }
    for (size_t i{0}; i < 20; ++i) f.Step();
    BOOST_REQUIRE(f.AllAt(1));
    for (const auto& runtime : f.runtimes) {
        BOOST_REQUIRE(runtime->SubmitLocalAction(f.market, Deposit(COutPoint{f.outpoint.hash, 1})) ==
                      flowmesh::QueueResult::ACCEPTED);
    }
    f.Drain();

    // One explicit tick lets every node re-send its held COMMIT. After that,
    // only message delivery runs: no periodic tick reaches any runtime.
    f.block_commits = false;
    f.Tick(std::chrono::seconds{1});
    for (size_t wave{0}; wave < 200 && !f.AllAt(3); ++wave) {
        for (auto& clock : f.clocks) clock.m_now += std::chrono::milliseconds{50};
        f.links.Pump(f.clocks[0].Now(), f.runtimes);
        f.Drain();
    }
    BOOST_REQUIRE(f.AllAt(3));
    const size_t proposer{flowmesh::ProductionProposerSeatIndex(2, 0, f.seats.seats.Size())};
    for (size_t i{0}; i < f.runtimes.size(); ++i) {
        BOOST_TEST_CONTEXT("node " << i) {
            // A tick with queued work records the lock acquisition for the
            // slot it runs in; only the proposer ran one during slot two.
            BOOST_CHECK_EQUAL(logs[i].Events(2, "tick_market_lock_acquired") > 0, i == proposer);
            BOOST_CHECK_EQUAL(logs[i].Executions(2, "build_proposal") > 0, i == proposer);
            BOOST_CHECK_EQUAL(f.runtimes[i]->StateSnapshot(f.market)->LedgerView().Available(f.account, f.asset), 500);
        }
    }
}

BOOST_AUTO_TEST_CASE(recent_window_published_before_inclusion_events)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "recent_window_published"};
    f.block_commits = false;
    f.Reach(1);
    f.Drain();
    const auto deposit{Deposit(f.outpoint)};
    std::promise<flowmesh::ClientActionWait> woken;
    auto result{woken.get_future()};
    // The waiter reads the window the moment CERTIFIED_INCLUDED wakes it.
    std::thread waiter{[&] {
        auto wait{f.runtimes[0]->WaitClientActionStatus(f.market, deposit.Id(),
            std::chrono::steady_clock::now() + std::chrono::seconds{20}, {})};
        woken.set_value(std::move(wait));
    }};
    BOOST_REQUIRE(f.runtimes[1]->SubmitLocalAction(f.market, deposit) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] { return f.AllAt(2); }));
    waiter.join();
    const auto wait{result.get()};
    BOOST_REQUIRE(wait.result == flowmesh::ClientWaitResult::TERMINAL);
    BOOST_REQUIRE(wait.status && wait.status->kind == flowmesh::ClientEventKind::CERTIFIED_INCLUDED);
    const auto inclusion{f.runtimes[0]->ClientRecentInclusion(f.market, deposit.Id())};
    BOOST_REQUIRE(inclusion);
    BOOST_CHECK_EQUAL(inclusion->first, wait.status->microblock_sequence);
    BOOST_CHECK(inclusion->second == wait.status->microblock_hash);
    const auto parts{f.runtimes[0]->ClientOutcomeParts(f.market, inclusion->first, inclusion->second, true)};
    BOOST_REQUIRE(parts.found);
    BOOST_REQUIRE(parts.envelope);
    BOOST_REQUIRE(parts.outcome);
    BOOST_CHECK(!parts.outcome->too_large);
    const flowmesh::ExecutionOutcomeEvidenceV1 evidence{parts.outcome->sequence, parts.outcome->entry_hash,
                                                        parts.outcome->preimage, parts.outcome->effects};
    BOOST_REQUIRE(flowmesh::VerifyExecutionOutcomeEvidence(parts.envelope->entry, evidence) ==
                  flowmesh::OutcomeEvidenceCheck::OK);
    const auto outcome{flowmesh::DeriveActionOutcome(parts.envelope->entry, evidence, deposit)};
    BOOST_REQUIRE(outcome);
    BOOST_CHECK(outcome->status == flowmesh::ActionOutcomeStatus::APPLIED);
    BOOST_REQUIRE(outcome->deposit);
    BOOST_CHECK(outcome->deposit->deposit_outpoint == f.outpoint);
    // A wrong hash or an unknown sequence finds nothing.
    BOOST_CHECK(!f.runtimes[0]->ClientOutcomeParts(f.market, inclusion->first, uint256{}, true).found);
    BOOST_CHECK(!f.runtimes[0]->ClientOutcomeParts(f.market, 99, inclusion->second, true).found);
    // Without a payload request only the record is shared.
    const auto record_only{f.runtimes[0]->ClientOutcomeParts(f.market, inclusion->first, inclusion->second, false)};
    BOOST_CHECK(record_only.found && record_only.outcome && !record_only.envelope);
}

BOOST_AUTO_TEST_CASE(certified_entry_from_window_is_byte_identical_to_store_path)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "recent_window_entry_bytes"};
    f.block_commits = false;
    f.Reach(1);
    BOOST_REQUIRE(f.runtimes[1]->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] { return f.AllAt(2); }));
    f.Drain();
    std::string error;
    const auto reads_before{f.runtimes[0]->ClientEntryStoreReadsForTest()};
    for (uint64_t sequence{0}; sequence < 2; ++sequence) {
        const auto served{f.runtimes[0]->ClientCertifiedEntry(f.market, sequence, error)};
        BOOST_REQUIRE_MESSAGE(served, error);
        std::optional<node::StoredProductionEntry> stored;
        BOOST_REQUIRE(f.stores[0]->ReadEntry(sequence, f.seats.seats, stored, error));
        BOOST_REQUIRE(stored);
        const auto expected{flowmesh::EncodeProductionCertifiedPayload({stored->entry, stored->certificate},
                                                                       f.seats.seats.Size())};
        BOOST_REQUIRE(expected);
        BOOST_CHECK(*served == *expected);
    }
    // certified_entry_window_reads_never_touch_store: both sequences (the
    // head and one older entry) were served from memory.
    BOOST_CHECK_EQUAL(f.runtimes[0]->ClientEntryStoreReadsForTest(), reads_before);
    BOOST_CHECK(!f.runtimes[0]->ClientCertifiedEntry(f.market, 2, error));
}

BOOST_AUTO_TEST_CASE(recent_window_rebuilt_after_restart_with_replay_outcomes)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "recent_window_restart"};
    f.block_commits = false;
    f.Reach(1);
    const auto deposit{Deposit(f.outpoint)};
    BOOST_REQUIRE(f.runtimes[1]->SubmitLocalAction(f.market, deposit) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] { return f.AllAt(2); }));
    f.Drain();
    const auto before{f.runtimes[0]->ClientRecentInclusion(f.market, deposit.Id())};
    BOOST_REQUIRE(before);
    f.StopNode(0);
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.StartNode(0, f.AgreementPath(0), std::nullopt, error), error);
    f.Drain();
    const auto after{f.runtimes[0]->ClientRecentInclusion(f.market, deposit.Id())};
    BOOST_REQUIRE(after);
    BOOST_CHECK(*after == *before);
    const auto parts{f.runtimes[0]->ClientOutcomeParts(f.market, after->first, after->second, true)};
    BOOST_REQUIRE(parts.found && parts.envelope && parts.outcome);
    const flowmesh::ExecutionOutcomeEvidenceV1 evidence{parts.outcome->sequence, parts.outcome->entry_hash,
                                                        parts.outcome->preimage, parts.outcome->effects};
    BOOST_CHECK(flowmesh::VerifyExecutionOutcomeEvidence(parts.envelope->entry, evidence) ==
                flowmesh::OutcomeEvidenceCheck::OK);
    // The genesis entry is retained too, with its own record.
    const auto genesis{f.runtimes[0]->ClientOutcomeParts(f.market, 0, parts.envelope->entry.parent_hash, false)};
    BOOST_CHECK(genesis.found && genesis.outcome);
    const auto reads_before{f.runtimes[0]->ClientEntryStoreReadsForTest()};
    BOOST_CHECK(f.runtimes[0]->ClientCertifiedEntry(f.market, 0, error));
    BOOST_CHECK_EQUAL(f.runtimes[0]->ClientEntryStoreReadsForTest(), reads_before);
}

BOOST_AUTO_TEST_CASE(pool_events_carry_memory_only_reason_codes_with_unchanged_texts)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "pool_event_reason_codes"};
    f.block_commits = false;
    f.Reach(1);
    f.Drain();
    f.block_commits = true; // nothing below may certify
    CKey key;
    key.MakeNewKey(true);
    flowmesh::Action action;
    action.signer = flowmesh::AccountForKey(XOnlyPubKey{key.GetPubKey()});
    action.type = static_cast<uint8_t>(flowmesh::ActionType::CANCEL_ASK);
    BOOST_REQUIRE(flowmesh::SignAction(key, f.domain, f.initial.ConfigId(), action));
    auto forged{action};
    forged.sequence = 1;
    BOOST_REQUIRE(flowmesh::SignAction(key, f.domain, f.initial.ConfigId(), forged));
    forged.credential.back() ^= 1;
    const auto before{f.runtimes[0]->ClientEvents(std::nullopt, f.market, std::nullopt).cursor};
    BOOST_REQUIRE(f.runtimes[0]->SubmitLocalAction(f.market, action) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(f.runtimes[0]->SubmitLocalAction(f.market, action) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(f.runtimes[0]->SubmitLocalAction(f.market, forged) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    const auto events{f.runtimes[0]->ClientEvents(before, f.market, std::nullopt).events};
    const auto find = [&](const uint256& id, flowmesh::ClientEventKind kind, const std::string& text) {
        return std::find_if(events.begin(), events.end(), [&](const auto& event) {
            return event.action_id == id && event.kind == kind && event.reason == text;
        });
    };
    const auto admitted{find(action.Id(), flowmesh::ClientEventKind::POOL_ADMITTED,
                             "authenticated action added to pool; not yet certified")};
    BOOST_REQUIRE(admitted != events.end());
    BOOST_CHECK(admitted->reason_code == flowmesh::ActionReasonCode::ADMITTED);
    const auto duplicate{find(action.Id(), flowmesh::ClientEventKind::POOL_ADMITTED,
                              "exact signed action already present in pool")};
    BOOST_REQUIRE(duplicate != events.end());
    BOOST_CHECK(duplicate->reason_code == flowmesh::ActionReasonCode::DUPLICATE_PENDING);
    const auto refused{find(forged.Id(), flowmesh::ClientEventKind::POOL_REFUSED,
        "pool refused this attempt (authentication, conflicting sequence or capacity); not an execution result")};
    BOOST_REQUIRE(refused != events.end());
    BOOST_CHECK(refused->reason_code == flowmesh::ActionReasonCode::UNAUTHENTICATED);
    // Only the one valid action is pooled.
    BOOST_CHECK_EQUAL(f.runtimes[0]->MarketStatus(f.market)->pending_actions, 1U);
}

namespace {
//! Tick the harness until `done` holds (bounded).
template <typename Harness>
bool Settle(Harness& f, const std::function<bool()>& done)
{
    for (size_t attempt{0}; attempt < 16 && !done(); ++attempt) f.Tick(std::chrono::seconds{2});
    return done();
}

flowmesh::Action SignedAction(const CKey& key, const uint256& domain, const uint256& config, uint64_t sequence,
                              flowmesh::ActionType type)
{
    flowmesh::Action action;
    action.signer = flowmesh::AccountForKey(XOnlyPubKey{key.GetPubKey()});
    action.sequence = sequence;
    action.type = static_cast<uint8_t>(type);
    if (type == flowmesh::ActionType::WITHDRAW) {
        action.asset = modern::NativeAsset();
        action.amount = 1;
        action.destination = Filled(0xd9);
    }
    BOOST_REQUIRE(flowmesh::SignAction(key, domain, config, action));
    return action;
}
} // namespace

BOOST_AUTO_TEST_CASE(local_submit_answers_from_certified_state_and_notices_only_authenticated)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "local_submit_certified_state"};
    f.block_commits = false;
    f.Reach(1);
    CKey key;
    key.MakeNewKey(true);
    const auto config{f.initial.ConfigId()};
    const auto consumer{SignedAction(key, f.domain, config, 0, flowmesh::ActionType::CANCEL_ASK)};
    BOOST_REQUIRE(f.runtimes[1]->SubmitLocalAction(f.market, consumer) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(f.runtimes[1]->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(Settle(f, [&] {
        const auto state{f.runtimes[0]->StateSnapshot(f.market)};
        return state && state->DepositConsumed(f.outpoint) && state->NextSequence(consumer.signer) == 1;
    }));
    f.Drain();
    f.block_commits = true; // nothing below may certify
    auto& runtime{*f.runtimes[0]};
    const auto inclusion{runtime.ClientRecentInclusion(f.market, consumer.Id())};
    BOOST_REQUIRE(inclusion);
    const auto events_before{runtime.ClientEventCursor()};
    const auto pending_before{runtime.MarketStatus(f.market)->pending_actions};

    // Resubmitting the executed action answers with its certified inclusion.
    const auto again{runtime.SubmitLocalActionChecked(f.market, consumer)};
    BOOST_CHECK(!again.queued);
    BOOST_CHECK(again.admission.kind == node::FlowMeshLocalAdmission::Kind::ALREADY_INCLUDED);
    BOOST_REQUIRE(again.admission.included);
    BOOST_CHECK(*again.admission.included == *inclusion);
    BOOST_CHECK(runtime.SubmitLocalAction(f.market, consumer) == flowmesh::QueueResult::ACCEPTED);
    const auto deposit_again{runtime.SubmitLocalActionChecked(f.market, Deposit(f.outpoint))};
    BOOST_CHECK(deposit_again.admission.kind == node::FlowMeshLocalAdmission::Kind::ALREADY_INCLUDED);
    // An inclusion is answered from retained certified data: no notice, so a
    // replay of public certified bytes never churns the notice ring.
    BOOST_CHECK(!runtime.NewestClientNotice(f.market, consumer.Id()));
    BOOST_CHECK(!runtime.NewestClientNotice(f.market, Deposit(f.outpoint).Id()));

    // A different action for the consumed sequence is refused, naming X.
    const auto loser{SignedAction(key, f.domain, config, 0, flowmesh::ActionType::CANCEL_BID)};
    const auto refused{runtime.SubmitLocalActionChecked(f.market, loser)};
    BOOST_CHECK(!refused.queued);
    BOOST_CHECK(refused.admission.kind == node::FlowMeshLocalAdmission::Kind::REFUSE);
    BOOST_CHECK(refused.admission.code == flowmesh::ActionReasonCode::STALE_SEQUENCE);
    BOOST_REQUIRE(refused.admission.consumer);
    BOOST_CHECK(refused.admission.consumer->action_id == consumer.Id());
    BOOST_CHECK_EQUAL(refused.admission.consumer->microblock_sequence, inclusion->first);
    BOOST_CHECK(refused.admission.consumer->microblock_hash == inclusion->second);
    const auto notice{runtime.NewestClientNotice(f.market, loser.Id())};
    BOOST_REQUIRE(notice);
    BOOST_CHECK(notice->code == flowmesh::ActionReasonCode::STALE_SEQUENCE);
    BOOST_REQUIRE(notice->consumed_by);
    BOOST_CHECK(notice->consumed_by->action_id == consumer.Id());
    BOOST_CHECK_EQUAL(notice->next_account_sequence, 1U);

    // notice_only_for_authenticated_attempts: a forged stale attempt gets
    // the same decision but no notice.
    auto forged{SignedAction(key, f.domain, config, 0, flowmesh::ActionType::WITHDRAW)};
    forged.credential.back() ^= 1;
    BOOST_CHECK(runtime.SubmitLocalActionChecked(f.market, forged).admission.kind ==
                node::FlowMeshLocalAdmission::Kind::REFUSE);
    BOOST_CHECK(!runtime.NewestClientNotice(f.market, forged.Id()));

    // The local window: next+64 queues, next+65 is refused.
    const auto edge{SignedAction(key, f.domain, config, 1 + 64, flowmesh::ActionType::CANCEL_ASK)};
    const auto edge_result{runtime.SubmitLocalActionChecked(f.market, edge)};
    BOOST_CHECK(edge_result.queued && *edge_result.queued == flowmesh::QueueResult::ACCEPTED);
    const auto far{SignedAction(key, f.domain, config, 1 + 65, flowmesh::ActionType::CANCEL_ASK)};
    const auto far_result{runtime.SubmitLocalActionChecked(f.market, far)};
    BOOST_CHECK(!far_result.queued);
    BOOST_CHECK(far_result.admission.code == flowmesh::ActionReasonCode::FAR_FUTURE_SEQUENCE);
    f.Drain();

    // None of the answered attempts appended a v1 event or reached the pool;
    // only the queued next+64 action did.
    const auto events{runtime.ClientEvents(events_before, f.market, std::nullopt).events};
    for (const auto& event : events) {
        BOOST_CHECK(event.action_id == edge.Id());
    }
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, pending_before + 1);
}

BOOST_AUTO_TEST_CASE(local_queued_stale_frame_refused_by_worker_with_consumer_then_notice)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "local_worker_recheck"};
    f.block_commits = false;
    f.Reach(1);
    CKey key;
    key.MakeNewKey(true);
    const auto config{f.initial.ConfigId()};
    const auto consumer{SignedAction(key, f.domain, config, 0, flowmesh::ActionType::CANCEL_ASK)};
    BOOST_REQUIRE(f.runtimes[1]->SubmitLocalAction(f.market, consumer) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] { return f.AllAt(2); }));
    f.Drain();
    f.block_commits = true;
    auto& runtime{*f.runtimes[0]};
    // A local frame queued before the consumer certified reaches the worker
    // after it: the worker's authoritative recheck refuses it by name.
    const auto loser{SignedAction(key, f.domain, config, 0, flowmesh::ActionType::CANCEL_BID)};
    const auto payload{flowmesh::EncodeProductionActionPayload(loser)};
    const auto local_peer{std::numeric_limits<flowmesh::WirePeerId>::min()};
    const auto before{runtime.ClientEventCursor()};
    BOOST_REQUIRE(runtime.EnqueueWireMessage(local_peer, {flowmesh::WireMessageKind::ACTION,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 1}, *payload}) ==
                  flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    const auto events{runtime.ClientEvents(before, f.market, std::nullopt).events};
    BOOST_REQUIRE_EQUAL(events.size(), 1U);
    BOOST_CHECK(events.front().kind == flowmesh::ClientEventKind::POOL_REFUSED);
    BOOST_CHECK_EQUAL(events.front().reason, "account sequence already used by another certified action; this attempt was not admitted");
    BOOST_CHECK(events.front().reason_code == flowmesh::ActionReasonCode::STALE_SEQUENCE);
    BOOST_REQUIRE(events.front().consumed_by);
    BOOST_CHECK(events.front().consumed_by->action_id == consumer.Id());
    const auto notice{runtime.NewestClientNotice(f.market, loser.Id())};
    BOOST_REQUIRE(notice);
    BOOST_CHECK_GE(notice->after_event_id, events.front().event_id); // the event first, then the notice
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 0U);
}

BOOST_AUTO_TEST_CASE(local_frame_with_stale_header_answers_pool_and_inclusion_before_refusing)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "local_stale_header"};
    f.block_commits = false;
    f.Reach(1);
    CKey key;
    key.MakeNewKey(true);
    const auto config{f.initial.ConfigId()};
    // Three certified entries: the first one's local frame below is then more
    // than two slots behind as well as in another epoch.
    const auto certified{SignedAction(key, f.domain, config, 0, flowmesh::ActionType::CANCEL_ASK)};
    for (uint64_t sequence{0}; sequence < 3; ++sequence) {
        const auto action{sequence == 0 ? certified : SignedAction(key, f.domain, config, sequence, flowmesh::ActionType::CANCEL_ASK)};
        BOOST_REQUIRE(f.runtimes[1]->SubmitLocalAction(f.market, action) == flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(Settle(f, [&] { return f.AllAt(2 + sequence); }));
    }
    f.Drain();
    f.block_commits = true;
    auto& runtime{*f.runtimes[0]};
    const auto head{runtime.MarketStatus(f.market)};
    BOOST_REQUIRE_EQUAL(head->next_sequence, 4U);
    BOOST_REQUIRE(runtime.ClientRecentInclusion(f.market, certified.Id()));
    // Relayed by a peer and pooled here.
    const auto pooled{SignedAction(key, f.domain, config, 5, flowmesh::ActionType::CANCEL_BID)};
    BOOST_REQUIRE(runtime.EnqueueWireMessage(3, {flowmesh::WireMessageKind::ACTION,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, head->epoch, head->next_sequence},
        *flowmesh::EncodeProductionActionPayload(pooled)}) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    BOOST_REQUIRE_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 1U);
    const auto fresh{SignedAction(key, f.domain, config, 6, flowmesh::ActionType::CANCEL_BID)};
    // Local frames whose headers went stale while they were queued.
    const auto local_peer{std::numeric_limits<flowmesh::WirePeerId>::min()};
    const auto send = [&](const flowmesh::Action& action, const flowmesh::WireHeader& header) {
        const auto cursor{runtime.ClientEventCursor()};
        BOOST_REQUIRE(runtime.EnqueueWireMessage(local_peer, {flowmesh::WireMessageKind::ACTION, header,
            *flowmesh::EncodeProductionActionPayload(action)}) == flowmesh::QueueResult::ACCEPTED);
        f.Drain();
        return runtime.ClientEvents(cursor, f.market, std::nullopt).events;
    };
    const flowmesh::WireHeader other_epoch{flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, head->epoch + 1, head->next_sequence};
    const flowmesh::WireHeader far_behind{flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, head->epoch, 1};
    for (const auto& header : {other_epoch, far_behind}) {
        // An already certified action is answered by its retained inclusion:
        // nothing is appended, never a refusal that would read "rejected".
        BOOST_CHECK(send(certified, header).empty());
        // Exact pooled bytes keep the duplicate answer, whatever the header.
        const auto duplicate{send(pooled, header)};
        BOOST_REQUIRE_EQUAL(duplicate.size(), 1U);
        BOOST_CHECK(duplicate.front().action_id == pooled.Id());
        BOOST_CHECK(duplicate.front().kind == flowmesh::ClientEventKind::POOL_ADMITTED);
        BOOST_CHECK_EQUAL(duplicate.front().reason, "exact signed action already present in pool");
        BOOST_CHECK(duplicate.front().reason_code == flowmesh::ActionReasonCode::DUPLICATE_PENDING);
    }
    // A frame that would still be queued meets the header checks, with their
    // existing texts.
    const auto refused{send(fresh, other_epoch)};
    BOOST_REQUIRE_EQUAL(refused.size(), 1U);
    BOOST_CHECK(refused.front().kind == flowmesh::ClientEventKind::POOL_REFUSED);
    BOOST_CHECK_EQUAL(refused.front().reason, "local queued action header is not current authority; this attempt was not admitted");
    BOOST_CHECK(refused.front().reason_code == flowmesh::ActionReasonCode::HEADER_NOT_CURRENT);
    const auto behind{send(fresh, far_behind)};
    BOOST_REQUIRE_EQUAL(behind.size(), 1U);
    BOOST_CHECK(behind.front().kind == flowmesh::ClientEventKind::POOL_REFUSED);
    BOOST_CHECK_EQUAL(behind.front().reason, "local queued action lacks a complete retained certified-head interval; check exact-action status before retry");
    BOOST_CHECK(behind.front().reason_code == flowmesh::ActionReasonCode::HEAD_INTERVAL_UNAVAILABLE);
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 1U);
}

BOOST_AUTO_TEST_CASE(local_stale_without_retained_consumer_is_unknown_not_rejected)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "local_stale_unattributed", std::chrono::seconds{10}, 3};
    f.block_commits = false;
    f.recent_window.payload_entries = 1;
    f.recent_window.outcome_entries = 1;
    f.StopNode(0);
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.StartNode(0, f.AgreementPath(0), std::nullopt, error), error);
    f.Reach(1);
    CKey key;
    key.MakeNewKey(true);
    const auto config{f.initial.ConfigId()};
    const auto consumer{SignedAction(key, f.domain, config, 0, flowmesh::ActionType::CANCEL_ASK)};
    BOOST_REQUIRE(f.runtimes[1]->SubmitLocalAction(f.market, consumer) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] { return f.AllAt(2); }));
    // Two later entries push the consumer out of node zero's small window.
    for (uint32_t n{1}; n <= 2; ++n) {
        BOOST_REQUIRE(f.runtimes[1]->SubmitLocalAction(f.market, Deposit(COutPoint{f.outpoint.hash, n})) ==
                      flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(Settle(f, [&] { return f.AllAt(2 + n); }));
    }
    f.Drain();
    f.block_commits = true;
    auto& runtime{*f.runtimes[0]};
    BOOST_CHECK(!runtime.ClientRecentInclusion(f.market, consumer.Id()));
    const auto loser{SignedAction(key, f.domain, config, 0, flowmesh::ActionType::CANCEL_BID)};
    const auto before{runtime.ClientEventCursor()};
    const auto decision{runtime.SubmitLocalActionChecked(f.market, loser)};
    BOOST_CHECK(!decision.queued);
    BOOST_CHECK(decision.admission.kind == node::FlowMeshLocalAdmission::Kind::UNKNOWN_OUTCOME);
    BOOST_CHECK(decision.admission.code == flowmesh::ActionReasonCode::STALE_SEQUENCE_UNATTRIBUTED);
    BOOST_CHECK(runtime.ClientEvents(before, f.market, std::nullopt).events.empty()); // no v1 event
    BOOST_REQUIRE(runtime.NewestClientNotice(f.market, loser.Id()));
    // The executed action itself is still answered from the event ring.
    BOOST_CHECK(runtime.SubmitLocalActionChecked(f.market, consumer).admission.kind ==
                node::FlowMeshLocalAdmission::Kind::ALREADY_INCLUDED);
}

BOOST_AUTO_TEST_CASE(remote_frames_have_no_local_sequence_window)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "remote_no_local_window"};
    f.block_commits = false;
    f.Reach(1);
    f.Drain();
    f.block_commits = true;
    CKey key;
    key.MakeNewKey(true);
    const auto config{f.initial.ConfigId()};
    // remote_frame_at_next_plus_1023_still_admitted: a relayed action far
    // beyond the local window is pooled exactly as before.
    const auto far{SignedAction(key, f.domain, config, 1023, flowmesh::ActionType::CANCEL_ASK)};
    const flowmesh::WireHeader header{flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 1};
    BOOST_REQUIRE(f.runtimes[0]->EnqueueWireMessage(2, {flowmesh::WireMessageKind::ACTION, header,
        *flowmesh::EncodeProductionActionPayload(far)}) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    BOOST_CHECK_EQUAL(f.runtimes[0]->MarketStatus(f.market)->pending_actions, 1U);
    BOOST_CHECK(!f.runtimes[0]->NewestClientNotice(f.market, far.Id()));

    // A contiguous run far beyond 64, relayed to every node, is proposed,
    // validated by followers and certified: proposal evidence has no window.
    // Commits stay blocked while the run is relayed at one head.
    constexpr uint64_t RUN{100};
    for (uint64_t n{0}; n < RUN; ++n) {
        const auto action{SignedAction(key, f.domain, config, n, flowmesh::ActionType::CANCEL_ASK)};
        const auto payload{flowmesh::EncodeProductionActionPayload(action)};
        for (size_t i{0}; i < f.runtimes.size(); ++i) {
            const auto status{f.runtimes[i]->MarketStatus(f.market)};
            BOOST_REQUIRE(f.runtimes[i]->EnqueueWireMessage(static_cast<flowmesh::WirePeerId>(10 + n % 8), {
                flowmesh::WireMessageKind::ACTION,
                {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, status->next_sequence},
                *payload}) == flowmesh::QueueResult::ACCEPTED);
        }
    }
    f.Drain();
    for (const auto& runtime : f.runtimes) BOOST_CHECK_EQUAL(runtime->MarketStatus(f.market)->pending_actions, RUN + 1);
    f.block_commits = false;
    const bool settled{Settle(f, [&] {
        for (const auto& runtime : f.runtimes) {
            const auto state{runtime->StateSnapshot(f.market)};
            if (!state || state->NextSequence(far.signer) != RUN) return false;
        }
        return true;
    })};
    BOOST_REQUIRE(settled);
}

BOOST_AUTO_TEST_CASE(prune_publishes_local_drop_notice_with_consumer_from_same_entry)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "local_drop_notice"};
    f.block_commits = false;
    f.Reach(1);
    f.Drain();
    f.block_commits = true;
    // The slot's proposer P certifies X; node L holds only its own A.
    const uint32_t proposer_seat{flowmesh::ProductionProposerSeatIndex(1, 0, f.seats.seats.Size())};
    size_t proposer{0};
    for (size_t i{0}; i < f.runtimes.size(); ++i) {
        if (f.seats.secrets[i].GetPublicKey() == f.seats.seats.members[proposer_seat].key.Key()) proposer = i;
    }
    const size_t local{(proposer + 1) % f.runtimes.size()};
    f.network.SetFilter([&](size_t from, size_t, const flowmesh::WireMessage& wire) {
        if (from == local && wire.kind == flowmesh::WireMessageKind::ACTION) return false;
        return f.Observe(wire);
    });
    CKey key;
    key.MakeNewKey(true);
    const auto config{f.initial.ConfigId()};
    const auto queued{SignedAction(key, f.domain, config, 0, flowmesh::ActionType::CANCEL_BID)};
    const auto consumer{SignedAction(key, f.domain, config, 0, flowmesh::ActionType::CANCEL_ASK)};
    const auto remote_only{SignedAction(key, f.domain, config, 0, flowmesh::ActionType::WITHDRAW)};
    // Submitted by this node's own wallet: its drop notice keeps that origin.
    const auto submitted{f.runtimes[local]->SubmitLocalActionChecked(f.market, queued, flowmesh::ClientNoticeOrigin::NODE)};
    BOOST_REQUIRE(submitted.queued && *submitted.queued == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    BOOST_REQUIRE_EQUAL(f.runtimes[local]->MarketStatus(f.market)->pending_actions, 1U);
    BOOST_REQUIRE(f.runtimes[proposer]->SubmitLocalAction(f.market, consumer) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    f.block_commits = false;
    BOOST_REQUIRE(Settle(f, [&] { return f.AllAt(2); }));
    f.Drain();
    const auto state{f.runtimes[local]->StateSnapshot(f.market)};
    BOOST_REQUIRE_EQUAL(state->NextSequence(consumer.signer), 1U);
    BOOST_CHECK_EQUAL(f.runtimes[local]->MarketStatus(f.market)->pending_actions, 0U);
    const auto inclusion{f.runtimes[local]->ClientRecentInclusion(f.market, consumer.Id())};
    BOOST_REQUIRE(inclusion);
    const auto notice{f.runtimes[local]->NewestClientNotice(f.market, queued.Id())};
    BOOST_REQUIRE(notice);
    BOOST_CHECK(notice->code == flowmesh::ActionReasonCode::DROPPED_SEQUENCE_CONSUMED);
    BOOST_CHECK(notice->origin == flowmesh::ClientNoticeOrigin::NODE);
    BOOST_CHECK_EQUAL(notice->account_sequence, 0U);
    BOOST_CHECK_EQUAL(notice->next_account_sequence, 1U);
    BOOST_CHECK(notice->signed_action_hash == Hash(*flowmesh::EncodeProductionActionPayload(queued)));
    BOOST_REQUIRE(notice->consumed_by);
    BOOST_CHECK(notice->consumed_by->action_id == consumer.Id());
    BOOST_CHECK_EQUAL(notice->consumed_by->microblock_sequence, inclusion->first);
    BOOST_CHECK(notice->consumed_by->microblock_hash == inclusion->second);
    // The notice is newer than every v1 event of the queued action, so an
    // opt-in wait ends on it; a v1 wait does not.
    const auto opt_in{f.runtimes[local]->WaitClientActionStatus(f.market, queued.Id(),
        std::chrono::steady_clock::now() + std::chrono::seconds{5}, {}, true)};
    BOOST_CHECK(opt_in.result == flowmesh::ClientWaitResult::TERMINAL);
    // remote_origin_prune_emits_nothing: no other node reports anything.
    for (size_t i{0}; i < f.runtimes.size(); ++i) {
        if (i == local) continue;
        BOOST_CHECK(!f.runtimes[i]->NewestClientNotice(f.market, queued.Id()));
        BOOST_CHECK(!f.runtimes[i]->NewestClientNotice(f.market, remote_only.Id()));
    }
}

BOOST_AUTO_TEST_CASE(public_notice_flood_from_other_identities_keeps_a_remote_clients_proofs)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "notice_submitter_share"};
    f.block_commits = false;
    f.Reach(1);
    f.Drain();
    f.block_commits = true;
    // The slot's proposer certifies X; node L holds only the client's A.
    const uint32_t proposer_seat{flowmesh::ProductionProposerSeatIndex(1, 0, f.seats.seats.Size())};
    size_t proposer{0};
    for (size_t i{0}; i < f.runtimes.size(); ++i) {
        if (f.seats.secrets[i].GetPublicKey() == f.seats.seats.members[proposer_seat].key.Key()) proposer = i;
    }
    const size_t local{(proposer + 1) % f.runtimes.size()};
    f.network.SetFilter([&](size_t from, size_t, const flowmesh::WireMessage& wire) {
        if (from == local && wire.kind == flowmesh::WireMessageKind::ACTION) return false;
        return f.Observe(wire);
    });
    auto& runtime{*f.runtimes[local]};
    const std::string client{"198.51.100.7"};
    CKey key;
    key.MakeNewKey(true);
    const auto config{f.initial.ConfigId()};
    const auto queued{SignedAction(key, f.domain, config, 0, flowmesh::ActionType::CANCEL_BID)};
    const auto consumer{SignedAction(key, f.domain, config, 0, flowmesh::ActionType::CANCEL_ASK)};
    // A remote client queues A through this node's public API.
    const auto submitted{runtime.SubmitLocalActionChecked(f.market, queued, flowmesh::ClientNoticeOrigin::PUBLIC, client)};
    BOOST_REQUIRE(submitted.queued && *submitted.queued == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    BOOST_REQUIRE_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 1U);
    // Another identity then submits 1,200 open-sequence actions. Those the
    // local queue takes are pooled, and their submitter is remembered while
    // they stay pooled; the rest are forgotten when the queue refuses them.
    const std::string churner{"203.0.113.9"};
    for (int k{0}; k < 20; ++k) {
        CKey churn_key;
        churn_key.MakeNewKey(true);
        for (uint64_t n{0}; n < 60; ++n) {
            const auto churn{SignedAction(churn_key, f.domain, config, n, flowmesh::ActionType::CANCEL_ASK)};
            BOOST_REQUIRE(runtime.SubmitLocalActionChecked(f.market, churn, flowmesh::ClientNoticeOrigin::PUBLIC, churner)
                              .admission.kind == node::FlowMeshLocalAdmission::Kind::ENQUEUE);
        }
    }
    f.Drain();
    BOOST_REQUIRE(f.runtimes[proposer]->SubmitLocalAction(f.market, consumer) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    f.block_commits = false;
    BOOST_REQUIRE(Settle(f, [&] { return f.AllAt(2); }));
    f.Drain();
    // A's pool-drop notice is kept for the client that queued it.
    const auto dropped{runtime.NewestClientNotice(f.market, queued.Id())};
    BOOST_REQUIRE(dropped);
    BOOST_CHECK(dropped->code == flowmesh::ActionReasonCode::DROPPED_SEQUENCE_CONSUMED);
    BOOST_CHECK(dropped->origin == flowmesh::ClientNoticeOrigin::PUBLIC);
    BOOST_CHECK_EQUAL(dropped->submitter, client);
    BOOST_REQUIRE(dropped->consumed_by);
    // The same client's late attempt at the used sequence.
    const auto late{SignedAction(key, f.domain, config, 0, flowmesh::ActionType::WITHDRAW)};
    BOOST_REQUIRE(runtime.SubmitLocalActionChecked(f.market, late, flowmesh::ClientNoticeOrigin::PUBLIC, client)
                      .admission.code == flowmesh::ActionReasonCode::STALE_SEQUENCE);

    // Other identities flood: the same one sends more than a whole ring of
    // authenticated attempts far ahead, from a fresh key; 24 IPv6 /64s send
    // 96 each; a third sends made-up public deposits, which earn no notice.
    CKey flood;
    flood.MakeNewKey(true);
    uint64_t sequence{1000};
    const auto attempt = [&](const std::string& who) {
        const auto far{SignedAction(flood, f.domain, config, sequence++, flowmesh::ActionType::CANCEL_ASK)};
        BOOST_REQUIRE(runtime.SubmitLocalActionChecked(f.market, far, flowmesh::ClientNoticeOrigin::PUBLIC, who)
                          .admission.code == flowmesh::ActionReasonCode::FAR_FUTURE_SEQUENCE);
    };
    for (size_t n{0}; n < flowmesh::CLIENT_NOTICE_CAPACITY + 64; ++n) attempt("203.0.113.9");
    for (int prefix{0}; prefix < 24; ++prefix) {
        for (int n{0}; n < 96; ++n) attempt(strprintf("ipv6/64:20010db8%08x", prefix));
    }
    for (uint32_t n{0}; n < 256; ++n) {
        const auto made_up{Deposit(COutPoint{f.outpoint.hash, 5000 + n})};
        BOOST_REQUIRE(runtime.SubmitLocalActionChecked(f.market, made_up, flowmesh::ClientNoticeOrigin::PUBLIC,
                                                       "203.0.113.10").admission.code ==
                      flowmesh::ActionReasonCode::DEPOSIT_FACTS_UNAVAILABLE);
        BOOST_CHECK(!runtime.NewestClientNotice(f.market, made_up.Id()));
    }
    // Each flooding identity keeps only its newest share.
    BOOST_CHECK(!runtime.NewestClientNotice(
        f.market, SignedAction(flood, f.domain, config, 1000, flowmesh::ActionType::CANCEL_ASK).Id()));
    BOOST_CHECK(runtime.NewestClientNotice(
        f.market, SignedAction(flood, f.domain, config, sequence - 1, flowmesh::ActionType::CANCEL_ASK).Id()));
    // Both of the client's proofs survive, and the opt-in 'action' reply's
    // lookup still finds them.
    for (const auto& [action, code] : {std::pair{queued.Id(), flowmesh::ActionReasonCode::DROPPED_SEQUENCE_CONSUMED},
                                       std::pair{late.Id(), flowmesh::ActionReasonCode::STALE_SEQUENCE}}) {
        const auto kept{runtime.NewestClientNoticeIfNewer(f.market, action)};
        BOOST_REQUIRE(kept);
        BOOST_CHECK(kept->code == code);
        BOOST_CHECK_EQUAL(kept->submitter, client);
        BOOST_REQUIRE(kept->consumed_by);
        BOOST_CHECK(kept->consumed_by->action_id == consumer.Id());
    }
}

BOOST_AUTO_TEST_CASE(public_submitters_are_remembered_only_while_the_action_is_live)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "notice_submitter_lifetime"};
    f.block_commits = false;
    f.Reach(1);
    f.Drain();
    f.block_commits = true;
    // The slot's proposer certifies X; node L holds only the client's A.
    const uint32_t proposer_seat{flowmesh::ProductionProposerSeatIndex(1, 0, f.seats.seats.Size())};
    size_t proposer{0};
    for (size_t i{0}; i < f.runtimes.size(); ++i) {
        if (f.seats.secrets[i].GetPublicKey() == f.seats.seats.members[proposer_seat].key.Key()) proposer = i;
    }
    const size_t local{(proposer + 1) % f.runtimes.size()};
    f.network.SetFilter([&](size_t from, size_t, const flowmesh::WireMessage& wire) {
        if (from == local && wire.kind == flowmesh::WireMessageKind::ACTION) return false;
        return f.Observe(wire);
    });
    auto& runtime{*f.runtimes[local]};
    using Origin = flowmesh::ClientNoticeOrigin;
    const std::string client{"198.51.100.7"};
    CKey key;
    key.MakeNewKey(true);
    const auto config{f.initial.ConfigId()};
    const auto queued{SignedAction(key, f.domain, config, 0, flowmesh::ActionType::CANCEL_BID)};
    const auto consumer{SignedAction(key, f.domain, config, 0, flowmesh::ActionType::CANCEL_ASK)};
    // A remote client queues A through this node's public API; the same
    // bytes from another identity later keep A the client's.
    for (const auto& who : {client, std::string{"203.0.113.50"}}) {
        const auto submitted{runtime.SubmitLocalActionChecked(f.market, queued, Origin::PUBLIC, who)};
        BOOST_REQUIRE(submitted.queued && *submitted.queued == flowmesh::QueueResult::ACCEPTED);
        f.Drain();
        BOOST_REQUIRE_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 1U);
        BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 1U);
    }

    // A peer's action holds another signer's sequence 0 in this pool.
    CKey holder_key;
    holder_key.MakeNewKey(true);
    const auto holder{SignedAction(holder_key, f.domain, config, 0, flowmesh::ActionType::CANCEL_ASK)};
    const flowmesh::WireHeader header{flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 1};
    BOOST_REQUIRE(runtime.EnqueueWireMessage(3, {flowmesh::WireMessageKind::ACTION, header,
        *flowmesh::EncodeProductionActionPayload(holder)}) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    BOOST_REQUIRE_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 2U);
    // 20 identities each submit more than their share (64) of different
    // withdrawals at that sequence, 1,440 in all: more than e9facb6's
    // 1,024-entry FIFO held, though under the 2,048-action bound
    // (public_submitters_reach_the_attribution_bound reaches that). Each is
    // open when submitted, so each is counted; then its life here ends. The
    // first ones the local queue takes and the worker's pool refuses (the
    // sequence is held); past the queue's burst, the queue refuses them.
    uint64_t amount{0};
    const auto withdrawal = [&] {
        flowmesh::Action action;
        action.signer = holder.signer;
        action.sequence = 0;
        action.type = static_cast<uint8_t>(flowmesh::ActionType::WITHDRAW);
        action.asset = modern::NativeAsset();
        action.amount = ++amount;
        action.destination = Filled(0xd9);
        BOOST_REQUIRE(flowmesh::SignAction(holder_key, f.domain, config, action));
        return action;
    };
    size_t queued_frames{0};
    for (int identity{0}; identity < 20; ++identity) {
        const auto who{strprintf("ipv6/64:20010db8%08x", identity)};
        for (size_t n{0}; n < flowmesh::CLIENT_NOTICE_PER_SUBMITTER + 8; ++n) {
            const auto submitted{runtime.SubmitLocalActionChecked(f.market, withdrawal(), Origin::PUBLIC, who)};
            BOOST_REQUIRE(submitted.admission.kind == node::FlowMeshLocalAdmission::Kind::ENQUEUE);
            queued_frames += submitted.queued && *submitted.queued == flowmesh::QueueResult::ACCEPTED;
        }
    }
    f.Drain();
    // Both paths ran: some reached the worker, some the queue refused.
    BOOST_CHECK_GT(queued_frames, 0U);
    BOOST_CHECK_LT(queued_frames, 20 * (flowmesh::CLIENT_NOTICE_PER_SUBMITTER + 8));
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 2U);
    // Only the live action is remembered.
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 1U);
    // A made-up public deposit is refused before it is queued; nothing stays.
    BOOST_CHECK(runtime.SubmitLocalActionChecked(f.market, Deposit(COutPoint{f.outpoint.hash, 4242}), Origin::PUBLIC,
                                                 "203.0.113.10").admission.code ==
                flowmesh::ActionReasonCode::DEPOSIT_FACTS_UNAVAILABLE);
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 1U);

    BOOST_REQUIRE(f.runtimes[proposer]->SubmitLocalAction(f.market, consumer) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    f.block_commits = false;
    BOOST_REQUIRE(Settle(f, [&] { return f.AllAt(2); }));
    f.Drain();
    // A's pool-drop notice still names the client that queued it, and the
    // drop ends A's life here.
    const auto dropped{runtime.NewestClientNotice(f.market, queued.Id())};
    BOOST_REQUIRE(dropped);
    BOOST_CHECK(dropped->code == flowmesh::ActionReasonCode::DROPPED_SEQUENCE_CONSUMED);
    BOOST_CHECK(dropped->origin == Origin::PUBLIC);
    BOOST_CHECK_EQUAL(dropped->submitter, client);
    BOOST_REQUIRE(dropped->consumed_by);
    BOOST_CHECK(dropped->consumed_by->action_id == consumer.Id());
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 0U);
}

BOOST_AUTO_TEST_CASE(public_submitters_reach_the_attribution_bound)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "notice_submitter_bound"};
    f.block_commits = false;
    f.Reach(1);
    f.Drain();
    f.block_commits = true;
    // Only node L holds these actions, and nothing is certified.
    const uint32_t proposer_seat{flowmesh::ProductionProposerSeatIndex(1, 0, f.seats.seats.Size())};
    size_t proposer{0};
    for (size_t i{0}; i < f.runtimes.size(); ++i) {
        if (f.seats.secrets[i].GetPublicKey() == f.seats.seats.members[proposer_seat].key.Key()) proposer = i;
    }
    const size_t local{(proposer + 1) % f.runtimes.size()};
    f.network.SetFilter([&](size_t from, size_t, const flowmesh::WireMessage& wire) {
        if (from == local && wire.kind == flowmesh::WireMessageKind::ACTION) return false;
        return f.Observe(wire);
    });
    auto& runtime{*f.runtimes[local]};
    const auto config{f.initial.ConfigId()};
    // The runtime's bound (PUBLIC_SUBMITTER_ACTIONS) is the ring's capacity:
    // 2,048 live actions, a share of 64, so 32 identities fit in full.
    constexpr size_t BOUND{flowmesh::CLIENT_NOTICE_CAPACITY}, SHARE{flowmesh::CLIENT_NOTICE_PER_SUBMITTER};
    static_assert(BOUND == 2048 && SHARE == 64);
    constexpr size_t FULL{BOUND / SHARE};
    // Every action is pooled. The local queue admits a burst of 256 and then
    // 64 a second of this node's clock, so time moves between batches.
    size_t queued{0};
    const auto submit = [&](const flowmesh::Action& action, const std::string& who) {
        const auto submitted{runtime.SubmitLocalActionChecked(f.market, action, flowmesh::ClientNoticeOrigin::PUBLIC, who)};
        BOOST_REQUIRE(submitted.admission.kind == node::FlowMeshLocalAdmission::Kind::ENQUEUE);
        BOOST_REQUIRE(submitted.queued && *submitted.queued == flowmesh::QueueResult::ACCEPTED);
        if (++queued % 128 == 0) {
            f.Drain();
            f.clocks[local].m_now += std::chrono::seconds{2};
        }
        return action.Id();
    };
    // Sequences first..first+count-1 of one account, within its window.
    const auto actions = [&](const CKey& key, const std::string& who, uint64_t first, size_t count) {
        std::vector<uint256> ids;
        for (uint64_t n{first}; n < first + count; ++n) {
            ids.push_back(submit(SignedAction(key, f.domain, config, n, flowmesh::ActionType::CANCEL_BID), who));
        }
        return ids;
    };
    const auto identity = [](size_t i) { return strprintf("ipv6/64:20010db8%08x", i); };
    const auto submitter = [&](const uint256& id) { return runtime.PublicSubmitterForTest(f.market, id).value_or("(none)"); };
    const std::string client{"198.51.100.7"};
    CKey client_key;
    client_key.MakeNewKey(true);
    const auto proof{actions(client_key, client, 0, 1).front()};
    // 31 other identities each fill their share: 32 identities in all.
    std::vector<std::vector<uint256>> held;
    for (size_t i{0}; i + 1 < FULL; ++i) {
        CKey key;
        key.MakeNewKey(true);
        held.push_back(actions(key, identity(i), 0, SHARE));
    }
    f.Drain();
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 1 + (FULL - 1) * SHARE);
    BOOST_CHECK_EQUAL(submitter(proof), client);
    // A 33rd identity: all but its last action fill the bound exactly, and
    // everyone keeps their attributions.
    CKey last_key;
    last_key.MakeNewKey(true);
    auto last{actions(last_key, identity(FULL - 1), 0, SHARE - 1)};
    f.Drain();
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), BOUND);
    BOOST_CHECK_EQUAL(submitter(proof), client);
    // Its last: every identity is within its share, so the oldest
    // attribution overall, the client's, is forgotten. Only the attribution:
    // the pool still holds the client's action.
    last.push_back(actions(last_key, identity(FULL - 1), SHARE - 1, 1).front());
    f.Drain();
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), BOUND);
    BOOST_CHECK_EQUAL(submitter(proof), "(none)");
    BOOST_CHECK_EQUAL(submitter(last.back()), identity(FULL - 1));
    BOOST_CHECK_EQUAL(submitter(held.front().front()), identity(0));
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 1 + FULL * SHARE);
    // An identity at its share replaces only its own oldest.
    CKey more_key;
    more_key.MakeNewKey(true);
    const auto more{actions(more_key, identity(0), 0, 1).front()};
    f.Drain();
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), BOUND);
    BOOST_CHECK_EQUAL(submitter(more), identity(0));
    BOOST_CHECK_EQUAL(submitter(held[0][0]), "(none)");
    BOOST_CHECK_EQUAL(submitter(held[0][1]), identity(0));
    BOOST_CHECK_EQUAL(submitter(held[1][0]), identity(1));
    BOOST_CHECK_EQUAL(submitter(last.front()), identity(FULL - 1));
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 2 + FULL * SHARE);
}

BOOST_AUTO_TEST_CASE(a_certified_public_action_ends_its_submitters_attribution)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "notice_submitter_certified"};
    f.block_commits = false;
    f.Reach(1);
    f.Drain();
    f.block_commits = true;
    auto& runtime{*f.runtimes[0]};
    const std::string client{"198.51.100.7"};
    CKey key;
    key.MakeNewKey(true);
    const auto action{SignedAction(key, f.domain, f.initial.ConfigId(), 0, flowmesh::ActionType::CANCEL_BID)};
    const auto submitted{runtime.SubmitLocalActionChecked(f.market, action, flowmesh::ClientNoticeOrigin::PUBLIC, client)};
    BOOST_REQUIRE(submitted.queued && *submitted.queued == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    // Its frame was handled while the pool holds it: the attribution stays.
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 1U);
    BOOST_CHECK_EQUAL(runtime.PublicSubmitterForTest(f.market, action.Id()).value_or("(none)"), client);
    f.block_commits = false;
    BOOST_REQUIRE(Settle(f, [&] { return f.AllAt(2); }));
    f.Drain();
    // Certified: the pool drops it on commit, and its attribution with it.
    const auto status{runtime.ClientActionStatus(f.market, action.Id())};
    BOOST_REQUIRE(status);
    BOOST_CHECK(status->kind == flowmesh::ClientEventKind::CERTIFIED_INCLUDED);
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 0U);
    BOOST_CHECK(!runtime.PublicSubmitterForTest(f.market, action.Id()));
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 0U);
}

BOOST_AUTO_TEST_CASE(only_a_counted_frame_settles_its_public_submitter)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "notice_submitter_counted"};
    f.block_commits = false;
    f.Reach(1);
    f.Drain();
    f.block_commits = true;
    auto& runtime{*f.runtimes[0]};
    const std::string client{"198.51.100.7"};
    const auto deposit{Deposit(f.outpoint)};
    // The client's public deposit counts its frame, then checks its anchored
    // facts on the submitting thread before queueing the frame: hold it there.
    std::promise<void> entered, release;
    const auto released{release.get_future().share()};
    std::atomic<bool> armed{true};
    f.deposits.on_get = [&] {
        if (!armed.exchange(false)) return;
        entered.set_value();
        released.wait();
    };
    node::FlowMeshLocalSubmitResult submitted;
    std::thread submitter{[&] {
        submitted = runtime.SubmitLocalActionChecked(f.market, deposit, flowmesh::ClientNoticeOrigin::PUBLIC, client);
    }};
    entered.get_future().wait();
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 1U);
    // Meanwhile a local frame of the same action that nobody counted, as this
    // node's own would be, is handled and refused: its header is not current
    // authority, so the pool does not take it.
    const flowmesh::WireHeader stale{flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch + 1, 1};
    BOOST_REQUIRE(runtime.EnqueueWireMessage(node::FLOWMESH_LOCAL_ACTION_PEER, {flowmesh::WireMessageKind::ACTION, stale,
        *flowmesh::EncodeProductionActionPayload(deposit)}) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 0U);
    // It settled nothing: the client's counted frame is still to come.
    BOOST_CHECK_EQUAL(runtime.PublicSubmitterForTest(f.market, deposit.Id()).value_or("(none)"), client);
    release.set_value();
    submitter.join();
    f.Drain();
    f.deposits.on_get = nullptr;
    BOOST_REQUIRE(submitted.queued && *submitted.queued == flowmesh::QueueResult::ACCEPTED);
    // The counted frame pooled the deposit and settled itself; the pool
    // holds the action, so its attribution stays.
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 1U);
    BOOST_CHECK_EQUAL(runtime.PublicSubmitterForTest(f.market, deposit.Id()).value_or("(none)"), client);
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 1U);
}

BOOST_AUTO_TEST_CASE(public_submission_notices_never_evict_node_notices)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "notice_origin"};
    f.block_commits = false;
    f.Reach(1);
    CKey key;
    key.MakeNewKey(true);
    const auto config{f.initial.ConfigId()};
    const auto consumer{SignedAction(key, f.domain, config, 0, flowmesh::ActionType::CANCEL_ASK)};
    BOOST_REQUIRE(f.runtimes[1]->SubmitLocalAction(f.market, consumer) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] { return f.AllAt(2); }));
    f.Drain();
    f.block_commits = true;
    auto& runtime{*f.runtimes[0]};
    // This node's wallet learns its action can never execute.
    const auto loser{SignedAction(key, f.domain, config, 0, flowmesh::ActionType::CANCEL_BID)};
    BOOST_CHECK(runtime.SubmitLocalActionChecked(f.market, loser, flowmesh::ClientNoticeOrigin::NODE).admission.code ==
                flowmesh::ActionReasonCode::STALE_SEQUENCE);
    // More than a whole ring of public notices: authenticated attempts far
    // ahead of the certified sequence, from 33 public identities (IPv6 /64s)
    // of 64 each. The 33rd identity's notices overflow the public ring
    // itself, oldest first; its per-identity shares alone never would.
    CKey flood;
    flood.MakeNewKey(true);
    const auto far = [&](const uint32_t n) {
        return SignedAction(flood, f.domain, config, 1000 + n, flowmesh::ActionType::CANCEL_ASK);
    };
    constexpr uint32_t FLOOD{flowmesh::CLIENT_NOTICE_CAPACITY + flowmesh::CLIENT_NOTICE_PER_SUBMITTER};
    for (uint32_t n{0}; n < FLOOD; ++n) {
        const auto who{strprintf("ipv6/64:%016x", n / flowmesh::CLIENT_NOTICE_PER_SUBMITTER)};
        BOOST_REQUIRE(runtime.SubmitLocalActionChecked(f.market, far(n), flowmesh::ClientNoticeOrigin::PUBLIC, who)
                          .admission.code == flowmesh::ActionReasonCode::FAR_FUTURE_SEQUENCE);
    }
    BOOST_CHECK(!runtime.NewestClientNotice(f.market, far(0).Id())); // evicted
    BOOST_CHECK(!runtime.NewestClientNotice(f.market, far(FLOOD - flowmesh::CLIENT_NOTICE_CAPACITY - 1).Id()));
    BOOST_CHECK(runtime.NewestClientNotice(f.market, far(FLOOD - flowmesh::CLIENT_NOTICE_CAPACITY).Id()));
    BOOST_CHECK(runtime.NewestClientNotice(f.market, far(FLOOD - 1).Id()));
    const auto kept{runtime.NewestClientNotice(f.market, loser.Id())};
    BOOST_REQUIRE(kept);
    BOOST_CHECK(kept->code == flowmesh::ActionReasonCode::STALE_SEQUENCE);
    BOOST_CHECK(kept->origin == flowmesh::ClientNoticeOrigin::NODE);
    BOOST_REQUIRE(kept->consumed_by);
    BOOST_CHECK(kept->consumed_by->action_id == consumer.Id());
}

BOOST_AUTO_TEST_CASE(local_factless_deposit_refused_not_relayed_but_remote_still_admitted)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "local_factless_deposit"};
    f.block_commits = false;
    f.Reach(1);
    f.Drain();
    f.block_commits = true;
    auto& runtime{*f.runtimes[0]};
    const auto factless{Deposit(COutPoint{f.outpoint.hash, 77})};
    const auto before{runtime.ClientEventCursor()};
    const auto refused{runtime.SubmitLocalActionChecked(f.market, factless)};
    BOOST_CHECK(!refused.queued);
    BOOST_CHECK(refused.admission.kind == node::FlowMeshLocalAdmission::Kind::REFUSE);
    BOOST_CHECK(refused.admission.code == flowmesh::ActionReasonCode::DEPOSIT_FACTS_UNAVAILABLE);
    BOOST_CHECK_EQUAL(node::FlowMeshLocalAdmissionText(refused.admission),
                      "FlowMesh deposit has no anchored facts at the current B3 anchor; this attempt was not queued");
    f.Drain();
    // Bound to the reply: no v1 event another attempt could inherit, no pool
    // entry, no relay. A public deposit carries no credential, so its
    // refusal earns no notice either; this node's own deposit's does.
    BOOST_CHECK(runtime.ClientEvents(before, f.market, std::nullopt).events.empty());
    for (const auto& node : f.runtimes) BOOST_CHECK_EQUAL(node->MarketStatus(f.market)->pending_actions, 0U);
    BOOST_CHECK(!runtime.NewestClientNotice(f.market, factless.Id()));
    const auto own{runtime.SubmitLocalActionChecked(f.market, factless, flowmesh::ClientNoticeOrigin::NODE)};
    BOOST_CHECK(own.admission.code == flowmesh::ActionReasonCode::DEPOSIT_FACTS_UNAVAILABLE);
    const auto notice{runtime.NewestClientNotice(f.market, factless.Id())};
    BOOST_REQUIRE(notice);
    BOOST_CHECK(notice->code == flowmesh::ActionReasonCode::DEPOSIT_FACTS_UNAVAILABLE);
    BOOST_CHECK(notice->origin == flowmesh::ClientNoticeOrigin::NODE);
    // A deposit with anchored facts is queued as before.
    const auto with_facts{runtime.SubmitLocalActionChecked(f.market, Deposit(f.outpoint))};
    BOOST_CHECK(with_facts.queued && *with_facts.queued == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 1U);
    // remote_factless_deposit_still_admitted_for_old_proposer: a relayed
    // fact-less deposit is pooled exactly as before.
    const flowmesh::WireHeader header{flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 1};
    BOOST_REQUIRE(f.runtimes[1]->EnqueueWireMessage(3, {flowmesh::WireMessageKind::ACTION, header,
        *flowmesh::EncodeProductionActionPayload(factless)}) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    BOOST_CHECK_EQUAL(f.runtimes[1]->MarketStatus(f.market)->pending_actions, 2U);
}

BOOST_AUTO_TEST_CASE(local_submit_of_pooled_bytes_keeps_duplicate_answer_before_local_refusals)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "local_submit_pooled"};
    f.block_commits = false;
    f.Reach(1);
    f.Drain();
    f.block_commits = true;
    auto& runtime{*f.runtimes[0]};
    CKey key;
    key.MakeNewKey(true);
    // Relayed by a peer, which has neither the local sequence window nor the
    // deposit facts check: both are pooled here.
    const auto far{SignedAction(key, f.domain, f.initial.ConfigId(), 1023, flowmesh::ActionType::CANCEL_ASK)};
    const auto factless{Deposit(COutPoint{f.outpoint.hash, 91})};
    const flowmesh::WireHeader header{flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 1};
    for (const auto& action : {far, factless}) {
        BOOST_REQUIRE(runtime.EnqueueWireMessage(3, {flowmesh::WireMessageKind::ACTION, header,
            *flowmesh::EncodeProductionActionPayload(action)}) == flowmesh::QueueResult::ACCEPTED);
    }
    f.Drain();
    BOOST_REQUIRE_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 2U);
    // The same exact bytes through this node's API: a pooled action may still
    // execute, so neither the sequence window nor the facts check refuses it
    // (either would read "rejected"); it is queued and gets today's answer.
    for (const auto& action : {far, factless}) {
        const auto before{runtime.ClientEventCursor()};
        const auto submitted{runtime.SubmitLocalActionChecked(f.market, action)};
        BOOST_CHECK(submitted.admission.kind == node::FlowMeshLocalAdmission::Kind::ENQUEUE);
        BOOST_REQUIRE(submitted.queued);
        BOOST_CHECK(*submitted.queued == flowmesh::QueueResult::ACCEPTED);
        f.Drain();
        const auto events{runtime.ClientEvents(before, f.market, std::nullopt).events};
        BOOST_REQUIRE(!events.empty());
        for (const auto& event : events) {
            BOOST_CHECK(event.action_id == action.Id());
            BOOST_CHECK(event.kind != flowmesh::ClientEventKind::POOL_REFUSED);
        }
        // The queue observation may follow the worker's answer.
        BOOST_CHECK(std::any_of(events.begin(), events.end(), [](const auto& event) {
            return event.kind == flowmesh::ClientEventKind::POOL_ADMITTED &&
                   event.reason == "exact signed action already present in pool";
        }));
        BOOST_CHECK(!runtime.NewestClientNotice(f.market, action.Id()));
    }
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 2U);
    // Not pooled, the local refusals still apply.
    const auto other{SignedAction(key, f.domain, f.initial.ConfigId(), 1024, flowmesh::ActionType::CANCEL_ASK)};
    BOOST_CHECK(runtime.SubmitLocalActionChecked(f.market, other).admission.code ==
                flowmesh::ActionReasonCode::FAR_FUTURE_SEQUENCE);
    BOOST_CHECK(runtime.SubmitLocalActionChecked(f.market, Deposit(COutPoint{f.outpoint.hash, 92})).admission.code ==
                flowmesh::ActionReasonCode::DEPOSIT_FACTS_UNAVAILABLE);
}

BOOST_AUTO_TEST_CASE(action_relay_window_accepts_two_slots_with_proof_and_restamps)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "action_relay_window", std::chrono::seconds{10}, 2};
    f.block_commits = false;
    f.Reach(1);
    // Entry 1 includes a deposit that is NOT credited (no anchored facts),
    // so its outpoint stays open: "still pending" alone would re-admit it.
    const auto uncredited{Deposit(COutPoint{f.outpoint.hash, 55})};
    {
        const auto head{f.runtimes[1]->MarketStatus(f.market)};
        BOOST_REQUIRE(f.runtimes[1]->EnqueueWireMessage(99, {flowmesh::WireMessageKind::ACTION,
            {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, head->epoch, head->next_sequence},
            *flowmesh::EncodeProductionActionPayload(uncredited)}) == flowmesh::QueueResult::ACCEPTED);
    }
    BOOST_REQUIRE(Settle(f, [&] { return f.AllAt(2); }));
    f.Drain();
    f.block_commits = true;
    auto& runtime{*f.runtimes[0]};
    BOOST_REQUIRE(runtime.ClientRecentInclusion(f.market, uncredited.Id()));
    BOOST_REQUIRE(!runtime.StateSnapshot(f.market)->DepositConsumed(uncredited.outpoint));
    std::vector<flowmesh::WireMessage> relayed;
    std::mutex relayed_mutex;
    f.network.SetFilter([&](size_t from, size_t, const flowmesh::WireMessage& wire) {
        if (from == 0 && wire.kind == flowmesh::WireMessageKind::ACTION) {
            std::lock_guard lock{relayed_mutex};
            relayed.push_back(wire);
        }
        return f.Observe(wire);
    });
    const auto send = [&](const flowmesh::Action& action, uint64_t sequence) {
        BOOST_REQUIRE(runtime.EnqueueWireMessage(7, {flowmesh::WireMessageKind::ACTION,
            {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, sequence},
            *flowmesh::EncodeProductionActionPayload(action)}) == flowmesh::QueueResult::ACCEPTED);
        f.Drain();
    };
    const auto counters = [&] { return runtime.DeliverySnapshots(f.market).front().action_admission; };
    // action_relay_window_refuses_action_in_retained_interval: the retained
    // entry since the sender's head included it: refused, counted only.
    send(uncredited, 1);
    BOOST_CHECK_EQUAL(counters().header_refused_unproven, 1U);
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 0U);
    // A fresh deposit one slot behind is proven and restamped.
    const auto fresh{Deposit(COutPoint{f.outpoint.hash, 1})};
    send(fresh, 1);
    BOOST_CHECK_EQUAL(counters().header_restamped_behind, 1U);
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 1U);
    // action_relay_window_accepts_ahead_on_lagging_seat: a sender two slots
    // ahead is accepted; three ahead is refused.
    const auto ahead{Deposit(COutPoint{f.outpoint.hash, 2})};
    send(ahead, 4);
    BOOST_CHECK_EQUAL(counters().header_accepted_ahead, 1U);
    send(Deposit(COutPoint{f.outpoint.hash, 3}), 5);
    BOOST_CHECK_EQUAL(counters().header_refused_far, 1U);
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 2U);
    // Relays and duplicate forwards always carry this node's own head; the
    // payload is untouched.
    {
        std::lock_guard lock{relayed_mutex};
        BOOST_REQUIRE_GE(relayed.size(), 2U);
        for (const auto& wire : relayed) BOOST_CHECK_EQUAL(wire.header.sequence, 2U);
        BOOST_CHECK(relayed.front().payload == *flowmesh::EncodeProductionActionPayload(fresh));
    }

    // action_relay_window_fails_closed_after_eviction: with a one-entry
    // window the interval two slots back is no longer retained.
    f.recent_window.payload_entries = 1;
    f.StopNode(0);
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.StartNode(0, f.AgreementPath(0), std::nullopt, error), error);
    f.Drain();
    auto& restarted{*f.runtimes[0]};
    const auto pending{restarted.MarketStatus(f.market)->pending_actions};
    BOOST_REQUIRE(restarted.EnqueueWireMessage(7, {flowmesh::WireMessageKind::ACTION,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 0},
        *flowmesh::EncodeProductionActionPayload(Deposit(COutPoint{f.outpoint.hash, 8}))}) ==
                  flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    BOOST_CHECK_EQUAL(restarted.DeliverySnapshots(f.market).front().action_admission.header_refused_unproven, 1U);
    BOOST_CHECK_EQUAL(restarted.MarketStatus(f.market)->pending_actions, pending);
}

BOOST_AUTO_TEST_CASE(accepted_local_original_waits_for_authority_then_resumes_without_client_resend)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "accepted_local_authority_resume"};
    f.block_commits = false;
    f.Reach(1); f.Drain();
    CKey key; key.MakeNewKey(true);
    const auto action{SignedAction(key, f.domain, f.initial.ConfigId(), 0, flowmesh::ActionType::CANCEL_ASK)};
    const auto payload{flowmesh::EncodeProductionActionPayload(action)};
    BOOST_REQUIRE(payload);
    const flowmesh::WireMessage original{flowmesh::WireMessageKind::ACTION,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 1}, *payload};
    auto& runtime{*f.runtimes[0]};
    // Capture/reserve and enqueue are the same private production stages.
    // The baseline test adapter uses only the old public queue seam, honestly
    // reproducing its original wrapper; this is not an RPC end-to-end test.
    const auto id{node::FlowMeshRuntimeTestAccess::Reserve(runtime, original, f.domain, f.initial.ConfigId())};
    BOOST_REQUIRE(id);
    BOOST_REQUIRE(f.runtimes[1]->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] { return f.AllAt(2); }));
    f.Drain();
    f.chains[0].SetReconciled(false);
    const auto cursor{runtime.ClientEventCursor()};
    BOOST_REQUIRE(node::FlowMeshRuntimeTestAccess::Enqueue(runtime, id, original) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    const auto interrupted{runtime.ClientEvents(cursor, f.market, action.signer).events};
    BOOST_REQUIRE_MESSAGE(std::none_of(interrupted.begin(), interrupted.end(), [&](const auto& event) {
        return event.action_id == action.Id() && event.kind == flowmesh::ClientEventKind::POOL_REFUSED;
    }), "accepted original was terminally refused solely because reconciliation was temporarily unavailable");
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 0U);
    BOOST_CHECK_EQUAL(runtime.DeliverySnapshots(f.market).front().deferred_objects, 1U);
    const auto clock{f.clocks[0].Now()};
    for (size_t n{0}; n < 4; ++n) f.Tick(); // ordinary maintenance, no clock/timing-policy change
    BOOST_CHECK_EQUAL(runtime.DeliverySnapshots(f.market).front().deferred_objects, 1U);
    BOOST_CHECK(f.clocks[0].Now() == clock);
    f.chains[0].SetReconciled(true);
    f.Tick(); // existing reopen edge, never a new client instruction
    BOOST_REQUIRE(WaitUntil([&] { return f.AllAt(3); }));
    f.Drain();
    BOOST_CHECK_EQUAL(runtime.DeliverySnapshots(f.market).front().deferred_objects, 0U);
    BOOST_CHECK_EQUAL(runtime.StateSnapshot(f.market)->NextSequence(action.signer), 1U);
    std::optional<node::StoredProductionEntry> stored;
    std::string error;
    BOOST_REQUIRE(runtime.ClientRecentInclusion(f.market, action.Id()));
    BOOST_REQUIRE(f.stores[0]->ReadEntry(2, f.seats.seats, stored, error));
    BOOST_REQUIRE(stored);
    BOOST_REQUIRE_EQUAL(stored->entry.actions.size(), 1U);
    BOOST_CHECK(stored->entry.actions.front().Id() == action.Id());
    const auto completed{runtime.ClientEvents(cursor, f.market, action.signer).events};
    BOOST_CHECK(std::any_of(completed.begin(), completed.end(), [&](const auto& event) {
        return event.action_id == action.Id() && event.kind == flowmesh::ClientEventKind::POOL_ADMITTED && event.signed_action_hash == Hash(*payload);
    }));
}

#ifndef FLOWMESH_LOCAL_RETENTION_BASELINE_TEST
BOOST_AUTO_TEST_CASE(local_source_reservation_bounds_preserve_critical_capacity_and_original_expiry)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "local_source_capacity"};
    f.block_commits = false; f.Reach(1); f.Drain(); f.block_commits = true;
    auto& runtime{*f.runtimes[0]};
    CKey key; key.MakeNewKey(true);
    const auto action{SignedAction(key, f.domain, f.initial.ConfigId(), 0, flowmesh::ActionType::CANCEL_ASK)};
    const flowmesh::WireMessage wire{flowmesh::WireMessageKind::ACTION,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 1}, *flowmesh::EncodeProductionActionPayload(action)};
    std::vector<uint64_t> ids;
    for (size_t n{0}; n < 8; ++n) {
        const auto id{node::FlowMeshRuntimeTestAccess::Reserve(runtime, wire, f.domain, f.initial.ConfigId())};
        BOOST_REQUIRE(id); ids.push_back(id);
    }
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Reserve(runtime, wire, f.domain, f.initial.ConfigId()), 0U);
    const auto reserved{node::FlowMeshRuntimeTestAccess::Sources(runtime)};
    BOOST_CHECK_EQUAL(reserved.first, 8U);
    BOOST_CHECK_LE(reserved.second, 8 * (flowmesh::FLOWMESH_ACTION_MAX_BYTES + flowmesh::FLOWMESH_WIRE_HEADER_SIZE + 128));
    f.chains[0].SetReconciled(false);
    // Maximum legal critical frames retain their existing independent space.
    // The fourth would exceed the shared encoded-byte bound once the promised
    // local sources are included, so it is safely refused, not allowed to steal.
    for (size_t n{0}; n < 4; ++n) {
        flowmesh::WireMessage critical{flowmesh::WireMessageKind::AGREEMENT, wire.header,
            std::vector<unsigned char>(flowmesh::FLOWMESH_AGREEMENT_MAX_BYTES, static_cast<unsigned char>(n + 1))};
        critical.payload[0] = 0;
        critical.payload[1] = 1;
        critical.payload[2] = 1; // Existing cheap version/stage framing, not canonical signed evidence.
        critical.header.sequence += n;
        flowmesh::WireCheck framing;
        BOOST_REQUIRE(flowmesh::CheckWireMessage(critical, framing));
        BOOST_REQUIRE(runtime.EnqueueWireMessage(20 + n, std::move(critical)) == flowmesh::QueueResult::ACCEPTED);
        f.Drain();
    }
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Sources(runtime).first, 8U);
    BOOST_CHECK_EQUAL(runtime.DeliverySnapshots(f.market).front().deferred_objects, 3U);
    BOOST_CHECK_LE(runtime.DeliverySnapshots(f.market).front().deferred_bytes + reserved.second, 16 * 1024 * 1024);
    // Never queued reservations expire too. No new original is signed, and
    // repeated closed-gate maintenance cannot restart the original60s window.
    f.Tick(std::chrono::seconds{59});
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Sources(runtime).first, 8U);
    f.Tick(std::chrono::seconds{2});
    for (size_t n{0}; n < 4; ++n) f.Tick(); // bounded two-source closed passes
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Sources(runtime).first, 0U);
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 0U);
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 0U);
}

BOOST_AUTO_TEST_CASE(local_source_survives_queue_drop_and_cannot_be_claimed_by_remote_alias)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "local_source_eviction"};
    f.block_commits = false; f.Reach(1); f.Drain();
    auto& runtime{*f.runtimes[0]};
    CKey key; key.MakeNewKey(true);
    const auto action{SignedAction(key, f.domain, f.initial.ConfigId(), 0, flowmesh::ActionType::CANCEL_ASK)};
    const flowmesh::WireMessage wire{flowmesh::WireMessageKind::ACTION,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 1}, *flowmesh::EncodeProductionActionPayload(action)};
    const auto id{node::FlowMeshRuntimeTestAccess::Reserve(runtime, wire, f.domain, f.initial.ConfigId())};
    BOOST_REQUIRE(id);
    BOOST_CHECK(node::FlowMeshRuntimeTestAccess::Inject(runtime, 23, wire, id) == flowmesh::QueueResult::MALFORMED);
    auto alias{wire}; alias.payload.back() ^= 1; // Same semantic ActionId, different signed bytes.
    BOOST_REQUIRE(node::FlowMeshRuntimeTestAccess::Inject(runtime, node::FLOWMESH_LOCAL_ACTION_PEER, alias, id) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Sources(runtime).first, 1U);
    node::FlowMeshRuntimeTestAccess::DropQueueCopy(runtime, id);
    f.Tick(std::chrono::milliseconds{250});
    BOOST_REQUIRE(WaitUntil([&] { return f.AllAt(2); }));
    f.Drain();
    BOOST_CHECK_EQUAL(runtime.StateSnapshot(f.market)->NextSequence(action.signer), 1U);
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Sources(runtime).first, 0U);
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 0U);
    // A delayed duplicate of the now-settled token is inert, even with the
    // correct wrapper and counted=true; no second accounting/count operation.
    BOOST_REQUIRE(node::FlowMeshRuntimeTestAccess::Inject(runtime, node::FLOWMESH_LOCAL_ACTION_PEER, wire, id) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    BOOST_CHECK_EQUAL(runtime.StateSnapshot(f.market)->NextSequence(action.signer), 1U);
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 0U);
    node::FlowMeshRuntimeTestAccess::ExhaustTokens(runtime);
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Reserve(runtime, wire, f.domain, f.initial.ConfigId()), 0U);
}

BOOST_AUTO_TEST_CASE(local_source_open_chain_seat_pause_preserves_retry_pacing_and_expiry)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "local_source_seat_pause"};
    f.block_commits = false; f.Reach(1); f.Drain();
    auto& runtime{*f.runtimes[0]};
    CKey key; key.MakeNewKey(true);
    const auto action{SignedAction(key, f.domain, f.initial.ConfigId(), 0, flowmesh::ActionType::CANCEL_ASK)};
    const flowmesh::WireMessage wire{flowmesh::WireMessageKind::ACTION,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 1}, *flowmesh::EncodeProductionActionPayload(action)};
    const auto id{node::FlowMeshRuntimeTestAccess::Reserve(runtime, wire, f.domain, f.initial.ConfigId())};
    BOOST_REQUIRE(id);
    BOOST_REQUIRE(f.runtimes[1]->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] { return f.AllAt(2); }));
    f.Drain();
    f.chains[0].SetTransition(f.market, node::FlowMeshSeatTransitionKind::PAUSED);
    BOOST_REQUIRE(node::FlowMeshRuntimeTestAccess::Enqueue(runtime, id, wire) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    const auto deadline{node::FlowMeshRuntimeTestAccess::Deadline(runtime, id)};
    uint64_t last_event{0};
    size_t action_processing{0};
    const auto sample = [&] {
        const auto events{runtime.DeliverySnapshots(f.market).front().events};
        for (const auto& event : events) {
            if (event.event_id <= last_event) continue;
            last_event = event.event_id;
            if (event.stage == "message_processing" && event.kind == flowmesh::WireMessageKind::ACTION &&
                event.sequence == 1) ++action_processing;
        }
    };
    sample();
    // One ordinary pass may observe the existing open/generation edge, but a
    // PAUSED seat decision must not invent that edge again at the same clock.
    f.Tick(); sample();
    const auto first_pass{action_processing};
    for (size_t n{0}; n < 12; ++n) { f.Tick(); sample(); }
    BOOST_CHECK_EQUAL(action_processing, first_pass);
    BOOST_CHECK(node::FlowMeshRuntimeTestAccess::Deadline(runtime, id) == deadline);
    f.Tick(std::chrono::milliseconds{250}); sample();
    BOOST_CHECK_EQUAL(action_processing, first_pass + 1);
    BOOST_CHECK(node::FlowMeshRuntimeTestAccess::Deadline(runtime, id) == deadline);
    f.chains[0].SetTransition(f.market, node::FlowMeshSeatTransitionKind::CONTINUE);
    f.Tick(std::chrono::milliseconds{250});
    BOOST_REQUIRE(WaitUntil([&] { return f.AllAt(3); }));
    f.Drain();
    BOOST_CHECK_EQUAL(runtime.StateSnapshot(f.market)->NextSequence(action.signer), 1U);
}

BOOST_AUTO_TEST_CASE(local_source_stop_start_stale_tokens_and_delayed_preflight_fail_closed)
{
    HealthRuntimeFixture f{m_args.GetDataDirBase() / "local_source_stop_start"};
    // Restart the same object before any append, so its original verified
    // configuration still matches the unchanged durable marker. This tests
    // token lifecycle in the ordinary mode, not a substitute for preagreement
    // bootstrap/replay. A completed preagreement creation capability cannot
    // be reused by simply restarting an originally creating configuration.
    auto& runtime{*f.runtime};
    std::string error;
    BOOST_REQUIRE_MESSAGE(runtime.Start(error), error);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    CKey key; key.MakeNewKey(true);
    const auto action{SignedAction(key, f.domain, f.initial.ConfigId(), 0, flowmesh::ActionType::CANCEL_ASK)};
    const flowmesh::WireMessage wire{flowmesh::WireMessageKind::ACTION,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 0}, *flowmesh::EncodeProductionActionPayload(action)};
    const auto old{node::FlowMeshRuntimeTestAccess::Reserve(runtime, wire, f.domain, f.initial.ConfigId())};
    BOOST_REQUIRE(old);
    runtime.Stop();
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Sources(runtime).first, 0U);
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 0U);
    BOOST_REQUIRE_MESSAGE(runtime.Start(error), error);
    const auto current{node::FlowMeshRuntimeTestAccess::Reserve(runtime, wire, f.domain, f.initial.ConfigId())};
    BOOST_REQUIRE_GT(current, old);
    BOOST_REQUIRE(node::FlowMeshRuntimeTestAccess::Inject(runtime, node::FLOWMESH_LOCAL_ACTION_PEER, wire, old) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Sources(runtime).first, 1U);
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 0U);
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 1U);
    // Expired original preflight/capture cannot be made new by reservation.
    const auto original_deadline{f.clock.Now() + std::chrono::seconds{60}};
    f.clock.m_now += std::chrono::seconds{61};
    runtime.NotifyTick();
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::ReserveUntil(runtime, wire, f.domain,
        f.initial.ConfigId(), original_deadline), 0U);
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Sources(runtime).first, 0U);
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 0U);
}

BOOST_AUTO_TEST_CASE(local_action_producer_reserves_before_acceptance_and_rejects_forgery_while_closed)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "local_source_producer"};
    f.block_commits = false; f.Reach(1); f.Drain();
    auto& runtime{*f.runtimes[0]};
    f.deposits.on_get = [] { throw std::runtime_error{"synthetic preflight facts failure"}; };
    BOOST_CHECK_THROW(runtime.SubmitLocalActionChecked(f.market, Deposit(f.outpoint),
        flowmesh::ClientNoticeOrigin::PUBLIC, "throwing-preflight"), std::runtime_error);
    f.deposits.on_get = nullptr;
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Sources(runtime).first, 0U);
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 0U);
    const auto missing{runtime.SubmitLocalActionChecked(f.market, Deposit(COutPoint{f.outpoint.hash, 77}),
        flowmesh::ClientNoticeOrigin::PUBLIC, "factless-preflight")};
    BOOST_CHECK(missing.admission.code == flowmesh::ActionReasonCode::DEPOSIT_FACTS_UNAVAILABLE);
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Sources(runtime).first, 0U);
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 0U);
    CKey key; key.MakeNewKey(true);
    const auto action{SignedAction(key, f.domain, f.initial.ConfigId(), 0, flowmesh::ActionType::CANCEL_ASK)};
    const auto original{flowmesh::EncodeProductionActionPayload(action)};
    const flowmesh::WireMessage wire{flowmesh::WireMessageKind::ACTION,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 1}, *original};
    const auto id{node::FlowMeshRuntimeTestAccess::Reserve(runtime, wire, f.domain, f.initial.ConfigId(), "original")};
    BOOST_REQUIRE(id);
    BOOST_REQUIRE(f.runtimes[1]->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] { return f.AllAt(2); }));
    f.Drain(); f.block_commits = true;
    // Only the existing behind-header authority check waits. Equal-header
    // pool admission remains unchanged, even while signing authority is shut.
    f.chains[0].SetReconciled(false);
    BOOST_CHECK(node::FlowMeshRuntimeTestAccess::Enqueue(runtime, id, wire) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Sources(runtime).first, 1U);
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 1U);
    auto forged{action}; forged.credential.back() ^= 1;
    const auto invalid{runtime.SubmitLocalActionChecked(f.market, forged, flowmesh::ClientNoticeOrigin::PUBLIC, "forger")};
    BOOST_REQUIRE(invalid.queued);
    BOOST_CHECK(*invalid.queued == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    // Same ActionId does not make a different credential the exact original,
    // and it never earns a retained shelter while authority is unavailable.
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Sources(runtime).first, 1U);
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 1U);
    BOOST_CHECK(runtime.PublicSubmitterForTest(f.market, action.Id()) == std::optional<std::string>{"original"});
    const auto cursor{runtime.ClientEventCursor()};
    f.Tick(std::chrono::seconds{61});
    for (size_t n{0}; n < 4; ++n) f.Tick();
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Sources(runtime).first, 0U);
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 0U);
    const auto expired{runtime.ClientEvents(cursor, f.market, action.signer).events};
    BOOST_CHECK(std::none_of(expired.begin(), expired.end(), [](const auto& event) {
        return event.kind == flowmesh::ClientEventKind::POOL_REFUSED;
    }));
    BOOST_CHECK(*flowmesh::EncodeProductionActionPayload(action) == *original);
}

BOOST_AUTO_TEST_CASE(local_source_start_keeps_idempotent_fast_path_during_stop_join)
{
    HealthRuntimeFixture f{m_args.GetDataDirBase() / "local_source_lifecycle"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(f.runtime->Start(error), error);
    f.Certificate(8, 0);
    {
        std::lock_guard lock{f.relay_mutex};
        f.block_certificates = true;
    }
    const auto unblock = [&] {
        { std::lock_guard lock{f.relay_mutex}; f.block_certificates = false; }
        f.relay_cv.notify_all();
    };
    BOOST_REQUIRE(f.runtime->EnqueueWireMessage(8, {flowmesh::WireMessageKind::CERTIFICATE,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 1}, f.payloads[1]}) ==
                  flowmesh::QueueResult::ACCEPTED);
    bool blocked{false};
    {
        std::unique_lock lock{f.relay_mutex};
        blocked = f.relay_cv.wait_for(lock, std::chrono::seconds{5}, [&] { return f.relay_blocked; });
    }
    if (!blocked) { unblock(); BOOST_FAIL("certificate worker did not reach controlled relay boundary"); }
    JoiningTestThread stop{[&] { f.runtime->Stop(); }, unblock};
    const bool stopping{WaitUntil([&] { return node::FlowMeshRuntimeTestAccess::Stopping(*f.runtime); })};
    std::promise<bool> started;
    auto observed{started.get_future()};
    JoiningTestThread start{[&] {
        try {
            std::string start_error;
            started.set_value(f.runtime->Start(start_error));
        } catch (...) { started.set_exception(std::current_exception()); }
    }, unblock};
    const bool returned_without_worker{observed.wait_for(std::chrono::seconds{2}) == std::future_status::ready};
    // Release controlled work before any assertion can unwind or join.
    unblock();
    start.join(); stop.join();
    BOOST_CHECK(stopping);
    BOOST_CHECK(returned_without_worker);
    BOOST_CHECK(observed.get());
    BOOST_CHECK_EQUAL(f.runtime->MarketStatus(f.market)->next_sequence, 2U);
}

BOOST_AUTO_TEST_CASE(local_source_malformed_reward_credentials_do_not_shelter_behind_closed_authority)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "local_source_reward_syntax"};
    f.block_commits = false; f.Reach(1); f.Drain(); f.block_commits = true;
    auto& runtime{*f.runtimes[0]};
    f.chains[0].SetReconciled(false);
    flowmesh::Action claim;
    claim.type = static_cast<uint8_t>(flowmesh::ActionType::CLAIM_SEAT_REWARD);
    claim.signer = Filled(0x95);
    claim.asset = modern::NativeAsset();
    claim.amount = 1;
    claim.destination = Filled(0x96);
    BOOST_REQUIRE(claim.ShapeIsCanonical());
    // All three fit the original transport shape but are independently
    // impossible reward signatures, even with historical facts unavailable.
    for (const size_t length : {size_t{0}, flowmesh::FLOWMESH_SEAT_REWARD_CREDENTIAL_SIZE - 1,
                                flowmesh::FLOWMESH_SEAT_REWARD_CREDENTIAL_SIZE}) {
        claim.credential.assign(length, 0);
        BOOST_REQUIRE(claim.ShapeIsCanonical());
        const auto submit{runtime.SubmitLocalActionChecked(f.market, claim,
            flowmesh::ClientNoticeOrigin::PUBLIC, "malformed-claim")};
        BOOST_REQUIRE(submit.queued);
        BOOST_REQUIRE(*submit.queued == flowmesh::QueueResult::ACCEPTED); // Queue-only, not authentication.
        f.Drain();
        BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Sources(runtime).first, 0U);
        BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 0U);
        BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 0U);
    }
    // Correct encoding alone is not membership/authentication. This unknown
    // claim may wait only within the same bounded source/expiry policy; it
    // cannot enter the pool until the ordinary historical check succeeds.
    const auto signature{f.seats.secrets[0].Sign(std::span<const unsigned char>{f.domain.begin(), 32}).Compressed()};
    claim.credential.assign(signature.begin(), signature.end());
    BOOST_REQUIRE(bls::Signature::Decode(claim.credential));
    f.chains[0].SetReconciled(true);
    const flowmesh::WireMessage original{flowmesh::WireMessageKind::ACTION,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.market, f.seats.seats.epoch, 1}, *flowmesh::EncodeProductionActionPayload(claim)};
    const auto source{node::FlowMeshRuntimeTestAccess::Reserve(runtime, original, f.domain, f.initial.ConfigId(), "unknown-claim")};
    BOOST_REQUIRE(source);
    f.block_commits = false;
    BOOST_REQUIRE(f.runtimes[1]->SubmitLocalAction(f.market, Deposit(f.outpoint)) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(WaitUntil([&] { return f.AllAt(2); }));
    f.Drain(); f.block_commits = true;
    f.chains[0].SetReconciled(false);
    BOOST_REQUIRE(node::FlowMeshRuntimeTestAccess::Enqueue(runtime, source, original) == flowmesh::QueueResult::ACCEPTED);
    f.Drain();
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Sources(runtime).first, 1U);
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 0U);
    f.chains[0].SetReconciled(true);
    f.Tick();
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Sources(runtime).first, 0U);
    BOOST_CHECK_EQUAL(runtime.PublicSubmittersForTest(f.market), 0U);
    BOOST_CHECK_EQUAL(runtime.MarketStatus(f.market)->pending_actions, 0U);
}
#endif

BOOST_AUTO_TEST_CASE(preagreement_local_signed_publications_resume_exactly_without_clock_advance)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() /
        "agreement_publication_reopen", std::chrono::seconds{10}};
    f.block_commits = false;
    f.Drain();
    const auto now{f.clocks[0].Now()};
    std::atomic<size_t> key_calls{0};
    // On the first ordinary tick: timeout eligibility, initial empty Retry,
    // MaybePropose, then SubmitCandidate's Pump. CandidateReady is already
    // validated/cached at that final key lookup; the next Current check is
    // PublishAgreement, after Sign's intent and exact signature syncs.
    f.keys[0].after_get = [&] {
        if (++key_calls == 4) {
            f.chains[0].ReconcileOnceWhileChecking(f.chains[0].Current(), 0, false);
        }
    };
    std::mutex observed_mutex;
    std::map<flowmesh::AgreementStage, std::vector<unsigned char>> originals;
    struct StopBeforeObservations {
        PreagreementRuntimeHarness<>& fixture;
        ~StopBeforeObservations() { fixture.StopAll(); }
    } stop_before_observations{f};
    f.network.SetFilter([&](size_t from, size_t, const flowmesh::WireMessage& wire) {
        if (from == 0 && wire.kind == flowmesh::WireMessageKind::AGREEMENT) {
            const auto message{flowmesh::DecodeAgreementMessage(wire.payload)};
            if (message && message->view == 0 && message->context.sequence == 0 &&
                message->seat_index == 0 && (message->stage == flowmesh::AgreementStage::PROPOSAL ||
                    message->stage == flowmesh::AgreementStage::PREPARE)) {
                std::lock_guard lock{observed_mutex};
                const auto [it, inserted]{originals.emplace(message->stage, wire.payload)};
                if (!inserted) BOOST_CHECK(it->second == wire.payload);
            }
        }
        return f.Observe(wire);
    });
    f.Tick();
    BOOST_REQUIRE_GE(key_calls.load(), 4U);
    BOOST_REQUIRE(!f.chains[0].Acceptable(f.chains[0].Current()));
    BOOST_REQUIRE(f.AllAt(0));
    const auto blocked{f.runtimes[0]->DeliverySnapshots(f.market).front()};
    const auto refused{std::count_if(blocked.events.begin(), blocked.events.end(), [](const auto& event) {
        return event.stage == "agreement_refused" && event.reason == "market_unavailable_or_reconciling";
    })};
    BOOST_REQUIRE_EQUAL(refused, 2); // Both durable proposal and PREPARE hit publication, not an earlier gate.
    {
        std::lock_guard lock{observed_mutex};
        BOOST_REQUIRE(originals.empty());
    }
    f.keys[0].after_get = {};
    f.chains[0].SetReconciled(true);
    f.Tick(); // The normal one-second retry is not due; no new request/view/timer.
    {
        std::lock_guard lock{observed_mutex};
        BOOST_REQUIRE_EQUAL(originals.size(), 2U);
        const auto proposal{flowmesh::DecodeAgreementMessage(originals.at(flowmesh::AgreementStage::PROPOSAL))};
        const auto prepare{flowmesh::DecodeAgreementMessage(originals.at(flowmesh::AgreementStage::PREPARE))};
        BOOST_REQUIRE(proposal);
        BOOST_REQUIRE(prepare);
        BOOST_CHECK(flowmesh::CheckAgreementMessageSignature(*proposal, f.seats.seats));
        BOOST_CHECK(flowmesh::CheckAgreementMessageSignature(*prepare, f.seats.seats));
        BOOST_CHECK(proposal->candidate == prepare->candidate);
        // Rebuild the expected original bytes, not a newly selected candidate.
        flowmesh::ProductionEpochGate gate{f.domain, f.market, f.seats.seats};
        flowmesh::ProductionEntryCheck check;
        const auto built{flowmesh::BuildProductionExecutionEntry(f.initial, f.domain, f.market,
            f.seats.seats, gate, 0, 0, {}, {100, Filled(0x71)},
            {f.chains[0].TipHeight(), std::nullopt, &f.chains[0]}, f.treasury, {}, &f.deposits, check)};
        BOOST_REQUIRE(built);
        BOOST_CHECK(std::optional<std::vector<unsigned char>>{proposal->entry_bytes} ==
                    flowmesh::EncodeProductionEntry(built->entry));
        auto expected{*prepare};
        expected.signature.fill(0);
        const auto digest{flowmesh::AgreementMessageDigest(expected)};
        BOOST_REQUIRE(digest);
        expected.signature = f.seats.secrets[0].Sign(
            std::span<const unsigned char>{digest->begin(), 32}).Compressed();
        BOOST_CHECK(flowmesh::EncodeAgreementMessage(expected) ==
                    std::optional<std::vector<unsigned char>>{originals.at(flowmesh::AgreementStage::PREPARE)});
    }
    BOOST_CHECK(f.AllAt(1));
    for (const auto& clock : f.clocks) BOOST_CHECK(clock.Now() == now);
    for (const auto& runtime : f.runtimes) BOOST_CHECK_EQUAL(runtime->MarketStatus(f.market)->round, 0U);
}

BOOST_AUTO_TEST_CASE(local_agreement_publication_cache_bounds_and_peer_origin)
{
    AgreementResumeCacheFixture f{m_args.GetDataDirBase() / "agreement_resume_count", 5};
    auto& runtime{*f.runtime};
    f.chain.SetReconciled(false);
    const auto message{f.Message(0, 1)};
    BOOST_CHECK(!node::FlowMeshRuntimeTestAccess::PublishPublication(runtime, message));
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Publications(runtime).records, 0U);
    const auto payload{flowmesh::EncodeAgreementMessage(message)};
    BOOST_REQUIRE(payload);
    BOOST_REQUIRE(runtime.EnqueueWireMessage(7, {flowmesh::WireMessageKind::AGREEMENT,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, message.context.market_id, 0, 0}, *payload}) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{5}));
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Publications(runtime).records, 0U);
    for (size_t market{0}; market < f.contexts.size(); ++market) {
        for (uint32_t view{1}; view <= 17; ++view) {
            BOOST_CHECK(!node::FlowMeshRuntimeTestAccess::PublishPublication(runtime, f.Message(market, view), true));
            const auto stats{node::FlowMeshRuntimeTestAccess::Publications(runtime)};
            BOOST_CHECK_LE(stats.records, node::FlowMeshRuntimeTestAccess::PublicationRecords());
            BOOST_CHECK_LE(stats.bytes, node::FlowMeshRuntimeTestAccess::PublicationBytes());
            const auto local{node::FlowMeshRuntimeTestAccess::MarketPublications(runtime, f.contexts[market].market_id)};
            BOOST_CHECK_LE(local.first, node::FlowMeshRuntimeTestAccess::PublicationMarketRecords());
            BOOST_CHECK_LE(local.second, node::FlowMeshRuntimeTestAccess::PublicationMarketBytes());
        }
    }
    const auto full{node::FlowMeshRuntimeTestAccess::Publications(runtime)};
    BOOST_CHECK_EQUAL(full.records, 64U);
    BOOST_CHECK_EQUAL(full.waiting, 64U);
    for (size_t n{0}; n < 100; ++n) {
        BOOST_CHECK(!node::FlowMeshRuntimeTestAccess::PublishPublication(runtime, message, true));
    }
    const auto duplicate{node::FlowMeshRuntimeTestAccess::Publications(runtime)};
    BOOST_CHECK_EQUAL(duplicate.records, full.records);
    BOOST_CHECK_EQUAL(duplicate.bytes, full.bytes);
    for (const auto& context : f.contexts) {
        BOOST_CHECK(runtime.MarketStatus(context.market_id)->halt == node::FlowMeshRuntimeHalt::NONE);
    }
    runtime.Stop();
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Publications(runtime).records, 0U);
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Publications(runtime).bytes, 0U);
}

BOOST_AUTO_TEST_CASE(local_agreement_publication_cache_accounts_large_encoded_copies)
{
    AgreementResumeCacheFixture f{m_args.GetDataDirBase() / "agreement_resume_bytes"};
    flowmesh::ProductionEntryCore entry;
    const auto& context{f.contexts[0]};
    entry.domain = context.domain;
    entry.market_id = context.market_id;
    entry.seat_set_hash = context.seat_set_hash;
    entry.previous_state_root = context.previous_state_root;
    entry.state_root = context.previous_state_root;
    entry.anchor = {100, Filled(0x71)};
    entry.effect_root = modern::EmptyFlowMeshEffectRoot(0);
    for (uint64_t i{0}; i < flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_ACTIONS; ++i) {
        flowmesh::Action action;
        action.signer = Filled(0x12);
        action.sequence = i;
        action.type = static_cast<uint8_t>(flowmesh::ActionType::SUBMIT_BID);
        for (CAmount price{1}; price <= static_cast<CAmount>(flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS); ++price) {
            action.curve.push_back({price, 1});
        }
        entry.actions.push_back(std::move(action));
    }
    entry.actions_root = flowmesh::ComputeProductionActionsRoot(entry.actions);
    size_t framed{0};
    // Structurally canonical large bodies test PRIVATE copy accounting only,
    // not executable balances or engine-issued conflicting proposals.
    for (unsigned char i{1}; i <= 20; ++i) {
        entry.result_root = Filled(i);
        const auto encoded{flowmesh::EncodeProductionEntry(entry)};
        BOOST_REQUIRE(encoded);
        flowmesh::AgreementMessage message;
        message.context = context;
        message.entry_bytes = *encoded;
        message.candidate = entry.GetHash();
        const auto payload{flowmesh::EncodeAgreementMessage(message)};
        BOOST_REQUIRE(payload);
        BOOST_REQUIRE(flowmesh::DecodeAgreementMessage(*payload));
        framed = payload->size() + flowmesh::FLOWMESH_WIRE_HEADER_SIZE;
        node::FlowMeshRuntimeTestAccess::CachePublication(*f.runtime, message);
    }
    const auto stats{node::FlowMeshRuntimeTestAccess::Publications(*f.runtime)};
    BOOST_REQUIRE_GT(stats.records, 0U);
    BOOST_CHECK_LT(stats.records, node::FlowMeshRuntimeTestAccess::PublicationMarketRecords());
    BOOST_CHECK_EQUAL(stats.records, node::FlowMeshRuntimeTestAccess::PublicationMarketBytes() / framed);
    BOOST_CHECK_EQUAL(stats.bytes, stats.records * framed);
    BOOST_CHECK_LE(stats.bytes, node::FlowMeshRuntimeTestAccess::PublicationMarketBytes());
    BOOST_CHECK_GE(node::FlowMeshRuntimeTestAccess::PublicationMarketBytes(),
                   flowmesh::AGREEMENT_MAX_BYTES + flowmesh::FLOWMESH_WIRE_HEADER_SIZE);
}

BOOST_AUTO_TEST_CASE(local_agreement_publication_retry_work_is_global_and_paced)
{
    AgreementResumeCacheFixture f{m_args.GetDataDirBase() / "agreement_resume_work", 4};
    f.mode = node::FlowMeshDeliveryAdmission::FULL;
    f.chain.SetReconciled(false);
    for (size_t market{0}; market < f.contexts.size(); ++market) {
        for (uint32_t view{1}; view <= 16; ++view) {
            BOOST_CHECK(!node::FlowMeshRuntimeTestAccess::PublishPublication(*f.runtime, f.Message(market, view), true));
        }
    }
    f.Tick();
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Publications(*f.runtime).inspected, 0U);
    f.chain.SetReconciled(true);
    f.Tick();
    const auto first{node::FlowMeshRuntimeTestAccess::Publications(*f.runtime)};
    BOOST_CHECK_EQUAL(first.inspected, 8U);
    BOOST_CHECK_EQUAL(first.attempted, 8U);
    BOOST_CHECK_LE(first.encoded_input, node::FlowMeshRuntimeTestAccess::PublicationBytes());
    for (size_t n{0}; n < 32; ++n) f.Tick();
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Publications(*f.runtime).inspected, first.inspected);
    f.Tick(std::chrono::milliseconds{249});
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Publications(*f.runtime).inspected, first.inspected);
    for (size_t pass{1}; pass < 8; ++pass) {
        const auto before{node::FlowMeshRuntimeTestAccess::Publications(*f.runtime)};
        f.Tick(pass == 1 ? std::chrono::milliseconds{1} : std::chrono::milliseconds{250});
        const auto after{node::FlowMeshRuntimeTestAccess::Publications(*f.runtime)};
        BOOST_CHECK_EQUAL(after.inspected - before.inspected, 8U);
        BOOST_CHECK_LE(after.encoded_input - before.encoded_input, node::FlowMeshRuntimeTestAccess::PublicationBytes());
    }
    std::set<uint256> exact_objects;
    for (const auto& payload : f.sent) exact_objects.insert(Hash(payload));
    BOOST_CHECK_EQUAL(exact_objects.size(), 64U); // Cursor did not favor one market or hash prefix.
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Publications(*f.runtime).records, 64U);
}

BOOST_AUTO_TEST_CASE(local_agreement_publication_reclose_and_budget_do_not_reset_pacing)
{
    AgreementResumeCacheFixture f{m_args.GetDataDirBase() / "agreement_resume_reclose"};
    f.chain.SetReconciled(false);
    for (uint32_t view{1}; view <= 3; ++view) {
        BOOST_CHECK(!node::FlowMeshRuntimeTestAccess::PublishPublication(*f.runtime, f.Message(0, view), true));
    }
    f.chain.SetReconciled(true);
    node::FlowMeshRuntimeTestAccess::ExhaustAgreementRelayBudget(*f.runtime);
    f.Tick();
    const auto budget_denied{node::FlowMeshRuntimeTestAccess::Publications(*f.runtime)};
    BOOST_CHECK_EQUAL(budget_denied.records, 3U);
    BOOST_CHECK_EQUAL(budget_denied.waiting, 0U);
    BOOST_CHECK(f.sent.empty());
    f.mode = node::FlowMeshDeliveryAdmission::FULL;
    f.reclose = true;
    f.Tick(std::chrono::milliseconds{250});
    BOOST_REQUIRE(!f.chain.Acceptable(f.chain.Current()));
    const auto closed{node::FlowMeshRuntimeTestAccess::Publications(*f.runtime)};
    BOOST_CHECK_EQUAL(closed.records, 3U);
    BOOST_CHECK_EQUAL(closed.waiting, 2U);
    BOOST_REQUIRE_EQUAL(f.sent.size(), 1U);
    const auto exact{f.sent.front()};
    f.chain.SetReconciled(true);
    f.mode = node::FlowMeshDeliveryAdmission::ADMITTED;
    for (size_t n{0}; n < 8; ++n) f.Tick();
    BOOST_CHECK_EQUAL(f.sent.size(), 1U); // Real reclose/reopen did not refill/bypass pace.
    f.Tick(std::chrono::milliseconds{250});
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Publications(*f.runtime).records, 0U);
    BOOST_CHECK_EQUAL(std::count(f.sent.begin() + 1, f.sent.end(), exact), 1);
    const auto done{node::FlowMeshRuntimeTestAccess::Publications(*f.runtime)};
    f.Tick();
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Publications(*f.runtime).attempted, done.attempted);
}

BOOST_AUTO_TEST_CASE(local_agreement_publication_normal_success_and_context_refusal_retire_copies)
{
    AgreementResumeCacheFixture f{m_args.GetDataDirBase() / "agreement_resume_context"};
    auto message{f.Message(0, 1)};
    f.chain.SetTransition(f.contexts[0].market_id, node::FlowMeshSeatTransitionKind::PAUSED);
    f.Tick();
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Publications(*f.runtime).records, 0U);
    // Seat pause/keyless alone is not a cache source. Existing publication
    // rules still allow replay of a signature already issued while eligible.
    BOOST_CHECK(node::FlowMeshRuntimeTestAccess::PublishPublication(*f.runtime, message, true));
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Publications(*f.runtime).records, 0U);
    f.chain.SetReconciled(false);
    BOOST_CHECK(!node::FlowMeshRuntimeTestAccess::PublishPublication(*f.runtime, message, true));
    BOOST_REQUIRE_EQUAL(node::FlowMeshRuntimeTestAccess::Publications(*f.runtime).records, 1U);
    f.chain.SetReconciled(true);
    BOOST_CHECK(node::FlowMeshRuntimeTestAccess::PublishPublication(*f.runtime, message, true));
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Publications(*f.runtime).records, 0U);
    const auto before{node::FlowMeshRuntimeTestAccess::Publications(*f.runtime)};
    f.Tick();
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Publications(*f.runtime).attempted, before.attempted);
    // PRIVATE cache injection isolates stale binding rejection; public source
    // admission would already reject this different execution configuration.
    message.context.execution_config_id = Filled(0x98);
    node::FlowMeshRuntimeTestAccess::CachePublication(*f.runtime, message);
    const auto sends{f.sent.size()};
    f.Tick(std::chrono::milliseconds{250});
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Publications(*f.runtime).records, 0U);
    BOOST_CHECK_EQUAL(f.sent.size(), sends);
    BOOST_CHECK(f.runtime->MarketStatus(f.contexts[0].market_id)->halt == node::FlowMeshRuntimeHalt::NONE);
    // A newly detected permanent seat-anchor failure is refreshed before the
    // fast pass. Discard the disposable copy, never its signing obligation.
    node::FlowMeshRuntimeTestAccess::CachePublication(*f.runtime, f.Message(0, 2));
    f.chain.AddCanonical({100, Filled(0xfa)});
    f.Tick(std::chrono::milliseconds{250});
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Publications(*f.runtime).records, 0U);
    BOOST_CHECK_EQUAL(f.sent.size(), sends);
    BOOST_CHECK(f.runtime->MarketStatus(f.contexts[0].market_id)->halt == node::FlowMeshRuntimeHalt::ANCHOR_INVALIDATED);
}

BOOST_AUTO_TEST_CASE(local_agreement_publication_restart_retains_exact_journal_fallback)
{
    PreagreementRuntimeHarness f{m_args.GetDataDirBase() / "agreement_resume_restart"};
    f.block_commits = false;
    f.Drain();
    std::atomic<size_t> key_calls{0};
    f.keys[0].after_get = [&] {
        if (++key_calls == 4) f.chains[0].ReconcileOnceWhileChecking(f.chains[0].Current(), 0, false);
    };
    f.Tick();
    BOOST_REQUIRE(!f.chains[0].Acceptable(f.chains[0].Current()));
    BOOST_REQUIRE_EQUAL(node::FlowMeshRuntimeTestAccess::Publications(*f.runtimes[0]).records, 2U);
    f.keys[0].after_get = {};
    f.StopNode(0); // Discards only disposable memory; preserved signer journal.
    std::string error;
    std::optional<node::FlowMeshProductionStore::Marker> marker;
    BOOST_REQUIRE(f.stores[0]->ReadMarker(marker, error));
    BOOST_REQUIRE(marker);
    const flowmesh::PreagreementContext context{f.domain, f.market, 0, f.seats.seats.set_hash,
        0, {}, f.initial.Root(), f.initial.ConfigId()};
    std::set<std::vector<unsigned char>> expected;
    {
        node::FlowMeshAgreementCallbacks callbacks;
        callbacks.local_keys = [] { return std::vector<bls::SecretKey>{}; };
        callbacks.publish = [&](const auto& message) {
            const auto encoded{flowmesh::EncodeAgreementMessage(message)};
            BOOST_REQUIRE(encoded);
            expected.insert(*encoded);
            return true;
        };
        node::FlowMeshAgreement journal{DBParams{.path = f.AgreementPath(0), .cache_bytes = 1 << 20}, std::move(callbacks)};
        BOOST_REQUIRE_MESSAGE(journal.Open(context, f.seats.seats, marker->agreement_identity, false, error), error);
        BOOST_REQUIRE(journal.Retry(error)); // Keyless, no candidate validation: exact old records only.
    }
    BOOST_REQUIRE_EQUAL(expected.size(), 2U);
    f.chains[0].SetReconciled(true);
    std::mutex mutex;
    std::set<std::vector<unsigned char>> replayed;
    struct StopBeforeObservations {
        PreagreementRuntimeHarness<>& fixture;
        ~StopBeforeObservations() { fixture.StopAll(); }
    } stop_before_observations{f};
    f.network.SetFilter([&](size_t from, size_t, const flowmesh::WireMessage& wire) {
        if (from == 0 && wire.kind == flowmesh::WireMessageKind::AGREEMENT && expected.contains(wire.payload)) {
            std::lock_guard lock{mutex};
            replayed.insert(wire.payload);
        }
        return f.Observe(wire);
    });
    BOOST_REQUIRE_MESSAGE(f.StartNode(0, f.AgreementPath(0), std::nullopt, error), error);
    BOOST_CHECK_EQUAL(node::FlowMeshRuntimeTestAccess::Publications(*f.runtimes[0]).records, 0U);
    f.Tick(); // Ordinary retry restored from the journal; optimization need not survive restart.
    {
        std::lock_guard lock{mutex};
        BOOST_CHECK(replayed == expected);
    }
    BOOST_CHECK(f.AllAt(1));
}

BOOST_AUTO_TEST_SUITE_END()
