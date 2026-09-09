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
#include <functional>
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

    void SetCatchupPageLimit(const size_t limit)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_catchup_page_limit = limit;
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
            if (relay.message.kind == flowmesh::WireMessageKind::ENTRIES) {
                auto entries{flowmesh::DecodeCatchupEntries(relay.message.payload)};
                if (entries && entries->size() > m_catchup_page_limit) {
                    entries->resize(m_catchup_page_limit);
                    relay.message.payload = *flowmesh::EncodeCatchupEntries(*entries);
                }
            }
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
    std::array<node::FlowMeshRuntime*, 4> m_nodes{};
    std::array<bool, 4> m_online{};
    size_t m_catchup_page_limit{flowmesh::FLOWMESH_CATCHUP_MAX_ENTRIES};
    bool m_ignore_hellos{false};
    bool m_partitioned{false};
    Filter m_filter;
    std::array<size_t, 4> m_get_requests{};
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
            [&network, i](node::FlowMeshRuntimeRelay relay) { network.Relay(i, std::move(relay)); },
            std::chrono::hours{1}};
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
            if (!voter_bytes) voter_bytes = message.payload;
            else exact_vote &= *voter_bytes == message.payload;
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
        BOOST_CHECK_EQUAL(proposer_action_attempts, 2U);
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
        BOOST_CHECK_EQUAL(proposer_action_attempts, 2U);
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
            if (relay.message.kind != flowmesh::WireMessageKind::ACTION) return;
            std::lock_guard<std::mutex> lock{relay_mutex};
            ++attempts;
            if (!suppress) delivered.push_back(std::move(relay.message));
        }, std::chrono::hours{1}};
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
    // Retained evidence now uses the bounded scheduler. A proposal can be
    // dequeued ahead of its action frames; retry normally after their arrival.
    for (const size_t i : {size_t{1}, size_t{0}, size_t{2}}) {
        BOOST_REQUIRE(runtimes[i]->WaitForIdle(std::chrono::seconds{2}));
    }
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
    const auto rejected{runtimes[0]->MarketData(market, std::nullopt, {}, error)};
    BOOST_REQUIRE_MESSAGE(rejected, error);
    BOOST_CHECK_EQUAL(rejected->snapshot.runtime.proposals_rejected_round, 1U);
    BOOST_CHECK_EQUAL(rejected->snapshot.runtime.round, 0U);
    BOOST_CHECK(!rejected->snapshot.runtime.local_locked_candidate);

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

BOOST_AUTO_TEST_CASE(honest_divergent_round_candidates_preserve_permanent_locks_and_halt)
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
    // C's authentic next-round proposal is round-admissible at A/B, but its
    // different hash cannot replace their durable X locks. The safe halt is
    // the current protocol contract, not a recovery mechanism to bypass.
    runtimes[2]->NotifyTick();
    BOOST_REQUIRE(WaitUntil([&] {
        return runtimes[0]->MarketStatus(market)->halt == node::FlowMeshRuntimeHalt::SIGNING_CONFLICT &&
               runtimes[1]->MarketStatus(market)->halt == node::FlowMeshRuntimeHalt::SIGNING_CONFLICT;
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

BOOST_AUTO_TEST_CASE(idle_legacy_peer_discovery_catches_up_new_market_and_byte_limited_pages)
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
    std::array<std::unique_ptr<node::FlowMeshRuntime>, 2> runtimes;
    // Simulate a response limited by bytes rather than by the 64-entry cap.
    network.SetCatchupPageLimit(1);
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
    BOOST_CHECK_GE(requests_after_sync, 3U); // initial + partial page + tip probe
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

    // Expiring the final unanswered probe must not restart blind polling.
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
    for (auto& runtime : runtimes) runtime->Stop();
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
    runtime.Stop();
}

BOOST_AUTO_TEST_SUITE_END()
