// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_runtime.h>

#include <flowmesh/auth.h>
#include <test/util/flowmesh.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

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
    int32_t TipHeight() const override
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        return m_tip_height;
    }

    bool Acceptable(const flowmesh::AnchorRef& anchor) const override
    {
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
        if (domain != m_domain || market != current.market_id) {
            return {node::FlowMeshSeatTransitionKind::PAUSED, std::nullopt};
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
    mutable std::mutex m_mutex;
    int32_t m_tip_height{260};
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
        (void)seats;
        const auto it{m_keys.find(market)};
        return it == m_keys.end() ? std::vector<bls::SecretKey>{}
                                  : it->second;
    }

    std::map<flowmesh::MarketId, std::vector<bls::SecretKey>> m_keys;
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
        if (!(anchor == required_anchor)) return std::nullopt;
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
        {
            std::lock_guard<std::mutex> lock{m_mutex};
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
        }
        for (const auto& [id, runtime] : targets) {
            (void)id;
            runtime->EnqueueWireMessage(static_cast<int64_t>(from),
                                        relay.message);
        }
    }

private:
    std::mutex m_mutex;
    std::array<node::FlowMeshRuntime*, 4> m_nodes{};
    std::array<bool, 4> m_online{};
};

/**
 * Three-node star used to prove proposal retry recovers one lost vote.
 * Voter attestations travel only to node 0, so neither voter can assemble a
 * certificate on the side. Node 2's first broadcast vote is deliberately
 * dropped; its later targeted reply must be the exact cached payload.
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
            if (relay.message.kind ==
                    flowmesh::WireMessageKind::ATTESTATION &&
                from != 0) {
                // New votes retain normal broadcast semantics at the runtime
                // boundary, but this star has only the proposer as a peer.
                const size_t target{
                    relay.peer ? static_cast<size_t>(*relay.peer) : 0};
                if (target != 0 || m_nodes[0] == nullptr) return;
                if (from == 2 && !m_dropped_first) {
                    m_dropped_first = relay.message.payload;
                    return;
                }
                if (from == 2 && relay.peer) {
                    m_targeted_retry = relay.message.payload;
                }
                targets.emplace_back(0, m_nodes[0]);
            } else if (relay.peer) {
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

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_runtime_tests, BasicTestingSetup)

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

    // Retrying the permanently locked proposal must not sign again. Each
    // voter returns its cached vote directly to node 0; the previously lost
    // node-2 payload is byte-identical and restores certification liveness.
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
    network.Set(3, runtimes[3].get(), true);

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

    BOOST_REQUIRE(runtimes[3]->RequestCatchup(0, market_a));
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
    restarted_config.relay = [&network](node::FlowMeshRuntimeRelay relay) {
        network.Relay(1, std::move(relay));
    };
    runtimes[1] = std::make_unique<node::FlowMeshRuntime>(
        std::move(restarted_config),
        std::vector<node::FlowMeshRuntimeMarketConfig>{restarted_market});
    network.Set(1, runtimes[1].get(), true);
    BOOST_REQUIRE_MESSAGE(runtimes[1]->Start(error), error);

    // The restarted proposer re-authenticates/re-executes the exact retained
    // candidate, re-gossips its evidence, and may sign only the same hash.
    network.Set(0, runtimes[0].get(), true);
    network.Set(2, runtimes[2].get(), true);
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
        };
        runtimes[i] = std::make_unique<node::FlowMeshRuntime>(
            std::move(config),
            std::vector<node::FlowMeshRuntimeMarketConfig>{MarketConfig(
                domain, market, treasury, seats.seats, initial, *stores[i],
                &deposits)});
        network.Set(i, runtimes[i].get(), true);
        BOOST_REQUIRE_MESSAGE(runtimes[i]->Start(error), error);
    }

    // Even a correctly signed proposal cannot pull a receiver forward by two
    // rounds or create a durable lock there.
    flowmesh::ProductionEpochGate gate{domain, market, seats.seats};
    flowmesh::ProductionEntryCheck entry_check;
    const flowmesh::AnchorRef bootstrap_anchor{
        static_cast<int32_t>(seats.seats.anchor_height),
        seats.seats.anchor_hash};
    const std::vector<flowmesh::Action> no_actions;
    const auto genesis{flowmesh::BuildProductionExecutionEntry(
        initial, domain, market, seats.seats, gate, /*sequence=*/0,
        /*effect_start=*/0, uint256{}, bootstrap_anchor,
        {chain.TipHeight(), std::nullopt, &chain}, treasury, no_actions,
        &deposits, entry_check)};
    BOOST_REQUIRE_MESSAGE(genesis,
                          flowmesh::ProductionEntryCheckName(entry_check));
    flowmesh::ProductionProposalEnvelope too_far;
    too_far.entry = genesis->entry;
    too_far.round = 2;
    too_far.proposer_seat_index = flowmesh::ProductionProposerSeatIndex(
        too_far.entry.sequence, too_far.round, seats.seats.Size());
    BOOST_REQUIRE_EQUAL(too_far.proposer_seat_index, 2U);
    const uint256 too_far_digest{
        flowmesh::ProductionProposalDigest(too_far.entry, too_far.round)};
    too_far.proposer_signature =
        seats.secrets[too_far.proposer_seat_index]
            .Sign(std::span<const unsigned char>{too_far_digest.begin(), 32})
            .Compressed();
    const auto too_far_payload{
        flowmesh::EncodeProductionProposalPayload(too_far)};
    BOOST_REQUIRE(too_far_payload.has_value());
    flowmesh::WireMessage too_far_wire;
    too_far_wire.kind = flowmesh::WireMessageKind::PROPOSAL;
    too_far_wire.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1, market,
                           seats.seats.epoch, 0};
    too_far_wire.payload = *too_far_payload;
    BOOST_REQUIRE(runtimes[0]->EnqueueWireMessage(2, std::move(too_far_wire)) ==
                  flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtimes[0]->WaitForIdle(std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(runtimes[0]->MarketStatus(market)->round, 0U);
    std::optional<uint256> premature_lock;
    BOOST_REQUIRE(stores[0]->ReadLock(
        flowmesh::ProductionSignPosition{seats.seats.epoch, 0},
        premature_lock, error));
    BOOST_CHECK(!premature_lock.has_value());

    // A/B now time out before C/D, recreating the adjacent 2+2 split that
    // exact receiver-round admission could never heal.
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
    config.relay = [](node::FlowMeshRuntimeRelay) {};
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

BOOST_AUTO_TEST_SUITE_END()
