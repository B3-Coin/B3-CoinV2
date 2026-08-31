// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_production_store.h>

#include <node/flowmesh_store.h>
#include <test/util/flowmesh.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <variant>
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
    BOOST_REQUIRE(key.has_value());
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
        flowmesh::SeatId seat_id;
    };
    std::vector<Entry> entries;
    for (size_t i{0}; i < count; ++i) {
        const bls::SecretKey secret{Key(i, salt)};
        flowmesh::BlsSeatBinding binding;
        binding.outpoint = COutPoint{
            Txid::FromUint256(Filled(static_cast<unsigned char>(salt + i + 30))),
            static_cast<uint32_t>(salt + i)};
        binding.public_key = secret.GetPublicKey().Compressed();
        binding.proof_of_possession = secret.SignPoP().Compressed();
        entries.push_back({secret, binding,
                           flowmesh::ComputeFlowMeshSeatId(domain,
                                                           binding.outpoint)});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.seat_id < b.seat_id ||
               (a.seat_id == b.seat_id &&
                a.binding.outpoint < b.binding.outpoint);
    });
    std::vector<flowmesh::BlsSeatBinding> bindings;
    SeatFixture out;
    for (const Entry& entry : entries) {
        out.secrets.push_back(entry.secret);
        bindings.push_back(entry.binding);
    }
    flowmesh::BlsSeatSetCheck check;
    const auto seats{flowmesh::BuildActiveFnBlsSeatSet(
        domain, market, epoch, anchor_height, anchor_hash, bindings, check)};
    BOOST_REQUIRE(seats.has_value());
    BOOST_REQUIRE(check == flowmesh::BlsSeatSetCheck::OK);
    out.seats = *seats;
    return out;
}

class TestAnchorPolicy final : public flowmesh::AnchorPolicy
{
public:
    explicit TestAnchorPolicy(const int32_t tip) : m_tip{tip} {}
    void Add(const flowmesh::AnchorRef& anchor)
    {
        m_canonical[anchor.height] = anchor.hash;
    }
    bool Acceptable(const flowmesh::AnchorRef& anchor) const override
    {
        return StillCanonical(anchor) && anchor.height <= m_tip &&
               m_tip - anchor.height >=
                   flowmesh::FLOWMESH_PRODUCTION_MIN_ANCHOR_DEPTH;
    }
    bool StillCanonical(const flowmesh::AnchorRef& anchor) const override
    {
        const auto it{m_canonical.find(anchor.height)};
        return it != m_canonical.end() && it->second == anchor.hash;
    }
    flowmesh::AnchorRef Current() const override
    {
        return {m_tip, Filled(0xfe)};
    }

private:
    int32_t m_tip;
    std::map<int32_t, uint256> m_canonical;
};

class SeatSource final : public node::ProductionSeatSetSource
{
public:
    void Add(const flowmesh::ActiveFnBlsSeatSet& seats)
    {
        m_sets[{seats.epoch, seats.set_hash}] = seats;
    }
    std::optional<flowmesh::ActiveFnBlsSeatSet> GetSeatSet(
        const uint256& domain, const flowmesh::MarketId& market_id,
        const uint64_t epoch, const uint256& set_hash) const override
    {
        if (domain != m_domain || market_id != m_market) return std::nullopt;
        const auto it{m_sets.find({epoch, set_hash})};
        return it == m_sets.end()
                   ? std::nullopt
                   : std::optional<flowmesh::ActiveFnBlsSeatSet>{it->second};
    }

    uint256 m_domain;
    flowmesh::MarketId m_market;

private:
    std::map<std::pair<uint64_t, uint256>,
             flowmesh::ActiveFnBlsSeatSet> m_sets;
};

class SettlementFacts final : public flowmesh::DepositVerifier
{
public:
    flowmesh::AnchorRef request_previous;
    flowmesh::AnchorRef request_anchor;
    flowmesh::AnchorRef settlement_anchor;
    std::vector<flowmesh::WithdrawalSettlementFactV1> settlements;

    std::optional<flowmesh::DepositInfo> GetDeposit(
        const COutPoint&, const flowmesh::AnchorRef&) const override
    {
        return std::nullopt;
    }

    std::optional<CAmount> GetWithdrawalCapacity(
        const modern::AssetId&, const flowmesh::AnchorRef& anchor) const override
    {
        return anchor == request_anchor
                   ? std::optional<CAmount>{MAX_MONEY}
                   : std::nullopt;
    }

    std::optional<std::vector<flowmesh::WithdrawalSettlementFactV1>>
    GetWithdrawalSettlements(
        const std::optional<flowmesh::AnchorRef>& after,
        const flowmesh::AnchorRef& through) const override
    {
        if (after == std::optional<flowmesh::AnchorRef>{request_previous} &&
            through == request_anchor) {
            return std::vector<flowmesh::WithdrawalSettlementFactV1>{};
        }
        if (after == std::optional<flowmesh::AnchorRef>{request_anchor} &&
            through == settlement_anchor) {
            return settlements;
        }
        return std::nullopt;
    }
};

class EmptyChainFacts final : public flowmesh::DepositVerifier
{
public:
    std::optional<flowmesh::DepositInfo> GetDeposit(
        const COutPoint&, const flowmesh::AnchorRef&) const override
    {
        return std::nullopt;
    }

    std::optional<CAmount> GetWithdrawalCapacity(
        const modern::AssetId&, const flowmesh::AnchorRef&) const override
    {
        return MAX_MONEY;
    }

    std::optional<std::vector<flowmesh::WithdrawalSettlementFactV1>>
    GetWithdrawalSettlements(
        const std::optional<flowmesh::AnchorRef>&,
        const flowmesh::AnchorRef&) const override
    {
        return std::vector<flowmesh::WithdrawalSettlementFactV1>{};
    }
};

flowmesh::Action Bid(const flowmesh::AccountId& account,
                     const uint64_t sequence, const CAmount price,
                     const CAmount amount)
{
    flowmesh::Action action;
    action.signer = account;
    action.sequence = sequence;
    action.type = static_cast<uint8_t>(flowmesh::ActionType::SUBMIT_BID);
    action.curve = *flowmesh::MakeLimitBidCurve(price, amount);
    return action;
}

flowmesh::Action Ask(const flowmesh::AccountId& account,
                     const uint64_t sequence, const CAmount price,
                     const CAmount amount)
{
    flowmesh::Action action;
    action.signer = account;
    action.sequence = sequence;
    action.type = static_cast<uint8_t>(flowmesh::ActionType::SUBMIT_ASK);
    action.curve = *flowmesh::MakeLimitAskCurve(price, amount);
    return action;
}

flowmesh::Action Withdraw(const flowmesh::AccountId& account,
                          const uint64_t sequence,
                          const modern::AssetId& asset,
                          const CAmount amount,
                          const uint256& destination)
{
    flowmesh::Action action;
    action.signer = account;
    action.sequence = sequence;
    action.type = static_cast<uint8_t>(flowmesh::ActionType::WITHDRAW);
    action.asset = asset;
    action.amount = amount;
    action.destination = destination;
    return action;
}

flowmesh::BlsMicroblockCertificate Certify(
    const flowmesh::ProductionEntryCore& entry, const SeatFixture& fixture)
{
    std::vector<flowmesh::IndexedBlsSignature> partials;
    const auto context{flowmesh::ProductionCertificateContext(entry)};
    for (uint32_t i{0};
         i < flowmesh::FlowMeshBlsThreshold(fixture.seats.Size()); ++i) {
        const auto signature{flowmesh::SignBlsMicroblockCertificate(
            fixture.secrets[i], context, fixture.seats)};
        BOOST_REQUIRE(signature.has_value());
        partials.push_back({i, *signature});
    }
    flowmesh::BlsMicroblockCertificate certificate;
    BOOST_REQUIRE(flowmesh::AssembleProductionEntryCertificate(
                      entry, fixture.seats, partials, certificate) ==
                  flowmesh::BlsCertificateAssemblyCheck::OK);
    return certificate;
}

modern::FlowMeshCheckpointRecordV1 Checkpoint(
    const flowmesh::ProductionEntryCore& entry,
    const flowmesh::BlsMicroblockCertificate& certificate,
    const flowmesh::ActiveFnBlsSeatSet& active_seats,
    const uint256& previous_checkpoint)
{
    const auto out{flowmesh::BuildProductionCheckpointRecord(
        entry, certificate, active_seats, previous_checkpoint)};
    BOOST_REQUIRE(out.has_value());
    return *out;
}

struct Scenario {
    uint256 domain{Filled(0x11)};
    modern::AssetId asset{Filled(0x31)};
    flowmesh::MarketId market{*flowmesh::ComputeFlowMeshMarketId(domain, asset)};
    flowmesh::VaultId vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    uint256 treasury{Filled(0x51)};
    flowmesh::AccountId buyer{Filled(0x61)};
    flowmesh::AccountId seller{Filled(0x62)};
    flowmesh::AccountId withdrawer{Filled(0x63)};
    SeatFixture seats0{Seats(domain, market, 4, 7, 100, Filled(0x71), 1)};
    SeatFixture seats1{Seats(domain, market, 5, 8, 220, Filled(0x72), 41)};
    TestAnchorPolicy anchors{300};
    EmptyChainFacts chain_facts;
    flowmesh::FlowMeshState state0{vault, asset, modern::NativeAsset(),
                                   flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    flowmesh::BuiltProductionExecution execution0;
    flowmesh::ProductionEntryCore handoff1;
    flowmesh::BuiltProductionExecution execution2;
    flowmesh::BlsMicroblockCertificate certificate0;
    flowmesh::BlsMicroblockCertificate certificate1;
    flowmesh::BlsMicroblockCertificate certificate2;

    Scenario()
        : execution0{BuildFirst()}, handoff1{BuildHandoff()},
          execution2{BuildNext()}
    {
        certificate0 = Certify(execution0.entry, seats0);
        certificate1 = Certify(handoff1, seats0);
        certificate2 = Certify(execution2.entry, seats1);
    }

private:
    flowmesh::BuiltProductionExecution BuildFirst()
    {
        const flowmesh::AnchorRef set_anchor{100, Filled(0x71)};
        const flowmesh::AnchorRef previous_anchor{190, Filled(0x73)};
        const flowmesh::AnchorRef entry_anchor{200, Filled(0x74)};
        for (const auto& anchor : {set_anchor, previous_anchor, entry_anchor,
                                  flowmesh::AnchorRef{210, Filled(0x75)},
                                  flowmesh::AnchorRef{220, Filled(0x72)},
                                  flowmesh::AnchorRef{230, Filled(0x76)}}) {
            anchors.Add(anchor);
        }
        BOOST_REQUIRE(flowmesh::test_only::StateFunding::Fund(
            state0, buyer, modern::NativeAsset(), 20'000));
        BOOST_REQUIRE(flowmesh::test_only::StateFunding::Fund(
            state0, seller, asset, 200));
        BOOST_REQUIRE(flowmesh::test_only::StateFunding::Fund(
            state0, withdrawer, asset, 50));
        const std::vector<flowmesh::Action> actions{
            Ask(seller, 0, 100, 200), Bid(buyer, 0, 100, 200),
            Withdraw(withdrawer, 0, asset, 25, Filled(0x64))};
        flowmesh::ProductionEpochGate gate{domain, market, seats0.seats};
        flowmesh::ProductionEntryCheck check;
        const auto built{flowmesh::BuildProductionExecutionEntry(
            state0, domain, market, seats0.seats, gate, 0, 0, uint256{},
            entry_anchor, {300, previous_anchor, &anchors}, treasury, actions,
            &chain_facts, check)};
        BOOST_REQUIRE_MESSAGE(built.has_value(),
                              flowmesh::ProductionEntryCheckName(check));
        return *built;
    }

    flowmesh::ProductionEntryCore BuildHandoff()
    {
        flowmesh::ProductionEntryCheck check;
        const auto handoff{flowmesh::BuildProductionHandoffEntry(
            execution0.next_state, domain, market, seats0.seats, seats1.seats,
            1, execution0.entry.effect_start + execution0.entry.effect_count,
            execution0.entry.GetHash(), {210, Filled(0x75)},
            {300, flowmesh::AnchorRef{200, Filled(0x74)}, &anchors}, check)};
        BOOST_REQUIRE_MESSAGE(handoff.has_value(),
                              flowmesh::ProductionEntryCheckName(check));
        return *handoff;
    }

    flowmesh::BuiltProductionExecution BuildNext()
    {
        flowmesh::ProductionEpochGate gate{domain, market, seats1.seats};
        flowmesh::ProductionEntryCheck check;
        const std::vector<flowmesh::Action> actions;
        const auto built{flowmesh::BuildProductionExecutionEntry(
            execution0.next_state, domain, market, seats1.seats, gate, 2,
            handoff1.effect_start + handoff1.effect_count,
            handoff1.GetHash(), {230, Filled(0x76)},
            {300, flowmesh::AnchorRef{210, Filled(0x75)}, &anchors}, treasury,
            actions, &chain_facts, check)};
        BOOST_REQUIRE_MESSAGE(built.has_value(),
                              flowmesh::ProductionEntryCheckName(check));
        return *built;
    }
};

struct TestLockKey {
    uint8_t prefix{'l'};
    uint64_t epoch{0};
    uint64_t sequence{0};
    SERIALIZE_METHODS(TestLockKey, obj)
    {
        READWRITE(obj.prefix, Using<BigEndianFormatter<8>>(obj.epoch),
                  Using<BigEndianFormatter<8>>(obj.sequence));
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_production_store_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(atomic_execution_handoff_connection_and_epoch_replay)
{
    Scenario scenario;
    const fs::path path{m_args.GetDataDirBase() / "flowmesh_production_v3"};
    std::string error;
    const uint256 initial_root{scenario.state0.Root()};
    const auto execution_checkpoint{
        Checkpoint(scenario.execution0.entry, scenario.certificate0,
                   scenario.seats0.seats, {})};
    const auto execution_checkpoint_id{
        modern::FlowMeshCheckpointIdV1(execution_checkpoint.core)};
    BOOST_REQUIRE(execution_checkpoint_id.has_value());
    const auto handoff_checkpoint{Checkpoint(
        scenario.handoff1, scenario.certificate1, scenario.seats0.seats,
        *execution_checkpoint_id)};

    {
        node::FlowMeshProductionStore store{DBParams{
            .path = path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
        BOOST_REQUIRE_MESSAGE(store.OpenForMarket(
                                  scenario.domain, scenario.market,
                                  scenario.seats0.seats, initial_root, error),
                              error);
        flowmesh::FlowMeshState committed{scenario.state0};
        BOOST_REQUIRE_MESSAGE(store.AppendExecution(
                                  scenario.execution0.entry,
                                  scenario.certificate0, scenario.seats0.seats,
                                  scenario.state0,
                                  {300, flowmesh::AnchorRef{190, Filled(0x73)},
                                   &scenario.anchors},
                                  scenario.treasury, &scenario.chain_facts,
                                  committed, error),
                              error);
        BOOST_CHECK(committed.Root() == scenario.execution0.next_state.Root());
        std::optional<node::ProductionCheckpointCandidate> candidate;
        BOOST_REQUIRE_MESSAGE(store.NextCheckpointCandidate(
                                  scenario.seats0.seats, candidate, error),
                              error);
        BOOST_REQUIRE(candidate.has_value());
        BOOST_CHECK(candidate->stored.entry.GetHash() ==
                    scenario.execution0.entry.GetHash());
        BOOST_CHECK(candidate->stored.effects == scenario.execution0.effects);
        BOOST_CHECK(candidate->previous_checkpoint_id.IsNull());
        BOOST_REQUIRE_MESSAGE(store.MarkExecutionCheckpointConnected(
                                  execution_checkpoint, scenario.seats0.seats,
                                  {240, Filled(0xa0)},
                                  error),
                              error);
        BOOST_REQUIRE_MESSAGE(store.AppendHandoff(
                                  scenario.handoff1, scenario.certificate1,
                                  scenario.seats0.seats, scenario.seats1.seats,
                                  committed,
                                  {300, flowmesh::AnchorRef{200, Filled(0x74)},
                                   &scenario.anchors},
                                  error),
                              error);
        BOOST_REQUIRE_MESSAGE(store.NextCheckpointCandidate(
                                  scenario.seats0.seats, candidate, error),
                              error);
        BOOST_REQUIRE(candidate.has_value());
        BOOST_CHECK(candidate->stored.entry.GetHash() ==
                    scenario.handoff1.GetHash());
        BOOST_CHECK(candidate->stored.effects.empty());
        BOOST_CHECK(candidate->previous_checkpoint_id ==
                    *execution_checkpoint_id);

        std::optional<node::FlowMeshProductionStore::Marker> marker;
        BOOST_REQUIRE(store.ReadMarker(marker, error));
        BOOST_REQUIRE(marker.has_value());
        BOOST_CHECK_EQUAL(marker->current_epoch, 7U);
        BOOST_CHECK(marker->current_seat_set_hash == scenario.seats0.seats.set_hash);

        flowmesh::FlowMeshState refused{committed};
        BOOST_CHECK(!store.AppendExecution(
            scenario.execution2.entry, scenario.certificate2,
            scenario.seats1.seats, committed,
            {300, flowmesh::AnchorRef{210, Filled(0x75)}, &scenario.anchors},
            scenario.treasury, &scenario.chain_facts, refused, error));

        auto wrong_checkpoint{handoff_checkpoint};
        wrong_checkpoint.core.previous_checkpoint_id = Filled(0xa1);
        BOOST_CHECK(!store.MarkHandoffCheckpointConnected(
            wrong_checkpoint, scenario.seats0.seats, scenario.seats1.seats,
            {241, Filled(0xa1)}, 300,
            error));
        BOOST_REQUIRE_MESSAGE(store.MarkHandoffCheckpointConnected(
                                  handoff_checkpoint, scenario.seats0.seats,
                                  scenario.seats1.seats,
                                  {241, Filled(0xa1)}, 300, error),
                              error);
        BOOST_REQUIRE(store.ReadMarker(marker, error));
        BOOST_REQUIRE(marker.has_value());
        BOOST_CHECK_EQUAL(marker->current_epoch, 8U);
        BOOST_CHECK((marker->current_anchor ==
                     flowmesh::AnchorRef{220, Filled(0x72)}));
        BOOST_CHECK(marker->current_seat_set_hash == scenario.seats1.seats.set_hash);
        BOOST_CHECK_EQUAL(marker->next_effect_index,
                          scenario.execution0.effects.size());

        BOOST_REQUIRE_MESSAGE(store.AppendExecution(
                                  scenario.execution2.entry,
                                  scenario.certificate2, scenario.seats1.seats,
                                  committed,
                                  {300, flowmesh::AnchorRef{210, Filled(0x75)},
                                   &scenario.anchors},
                                  scenario.treasury, &scenario.chain_facts,
                                  refused, error),
                              error);
        BOOST_CHECK(refused.Root() == scenario.execution2.next_state.Root());
        BOOST_REQUIRE_MESSAGE(store.NextCheckpointCandidate(
                                  scenario.seats1.seats, candidate, error),
                              error);
        BOOST_CHECK(!candidate);
    }

    node::FlowMeshProductionStore reopened{
        DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
    BOOST_REQUIRE_MESSAGE(reopened.OpenForMarket(
                              scenario.domain, scenario.market,
                              scenario.seats0.seats, initial_root, error),
                          error);
    BOOST_CHECK(reopened.LockOnce({8, 3}, Filled(0xee)) ==
                flowmesh::ProductionLockResult::STORAGE_FAILURE);
    SeatSource source;
    source.m_domain = scenario.domain;
    source.m_market = scenario.market;
    source.Add(scenario.seats0.seats);
    source.Add(scenario.seats1.seats);
    flowmesh::FlowMeshState replayed{scenario.state0};
    uint256 last_hash;
    BOOST_REQUIRE_MESSAGE(reopened.Replay(
                              replayed, last_hash, source,
                              {300, flowmesh::AnchorRef{190, Filled(0x73)},
                               &scenario.anchors},
                              scenario.treasury, &scenario.chain_facts, error),
                          error);
    BOOST_CHECK(replayed.Root() == scenario.execution2.next_state.Root());
    BOOST_CHECK(last_hash == scenario.execution2.entry.GetHash());
    BOOST_CHECK(reopened.LockOnce({8, 3}, Filled(0xee)) ==
                flowmesh::ProductionLockResult::STORAGE_FAILURE);
}

BOOST_AUTO_TEST_CASE(genesis_and_ordinary_checkpoint_reorgs_republish_and_restart)
{
    Scenario scenario;
    const fs::path path{
        m_args.GetDataDirBase() / "flowmesh_production_checkpoint_reorg"};
    const uint256 block0{Filled(0xa0)};
    const uint256 replacement0{Filled(0xb0)};
    const uint256 block1{Filled(0xa1)};
    const uint256 replacement1{Filled(0xb1)};
    std::string error;

    flowmesh::FlowMeshState state1{scenario.state0};
    flowmesh::FlowMeshState state2{scenario.state0};
    modern::FlowMeshCheckpointRecordV1 checkpoint1;
    {
        node::FlowMeshProductionStore store{DBParams{
            .path = path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
        BOOST_REQUIRE_MESSAGE(store.OpenForMarket(
                                  scenario.domain, scenario.market,
                                  scenario.seats0.seats, scenario.state0.Root(),
                                  error),
                              error);
        BOOST_REQUIRE_MESSAGE(store.AppendExecution(
                                  scenario.execution0.entry,
                                  scenario.certificate0, scenario.seats0.seats,
                                  scenario.state0,
                                  {300, flowmesh::AnchorRef{190, Filled(0x73)},
                                   &scenario.anchors},
                                  scenario.treasury, &scenario.chain_facts,
                                  state1, error),
                              error);
        const auto checkpoint0{Checkpoint(
            scenario.execution0.entry, scenario.certificate0,
            scenario.seats0.seats, {})};
        const auto checkpoint0_id{
            modern::FlowMeshCheckpointIdV1(checkpoint0.core)};
        BOOST_REQUIRE(checkpoint0_id.has_value());
        BOOST_REQUIRE_MESSAGE(store.MarkExecutionCheckpointConnected(
                                  checkpoint0, scenario.seats0.seats,
                                  {240, block0}, error),
                              error);

        // Sequence zero is a mandatory checkpoint. Removing its exact B3
        // inclusion rewinds the head to null and makes the same certified
        // genesis entry publishable on the replacement fork.
        bool rolled_back{false};
        BOOST_REQUIRE_MESSAGE(store.ReconcileCheckpointConnections(
                                  {{240, replacement0}}, rolled_back, error),
                              error);
        BOOST_CHECK(rolled_back);
        std::optional<node::FlowMeshProductionStore::Marker> marker;
        BOOST_REQUIRE(store.ReadMarker(marker, error));
        BOOST_REQUIRE(marker.has_value());
        BOOST_CHECK(marker->last_b3_checkpoint.IsNull());
        std::optional<node::ProductionCheckpointCandidate> candidate;
        BOOST_REQUIRE(store.NextCheckpointCandidate(
            scenario.seats0.seats, candidate, error));
        BOOST_REQUIRE(candidate.has_value());
        BOOST_CHECK_EQUAL(candidate->stored.entry.sequence, 0U);
        BOOST_REQUIRE_MESSAGE(store.MarkExecutionCheckpointConnected(
                                  checkpoint0, scenario.seats0.seats,
                                  {240, replacement0}, error),
                              error);

        // Add a later, effect-bearing ordinary execution so its checkpoint is
        // independently reversible without changing the active committee.
        flowmesh::ProductionEpochGate gate{
            scenario.domain, scenario.market, scenario.seats0.seats};
        flowmesh::ProductionEntryCheck check;
        const std::vector<flowmesh::Action> actions{Withdraw(
            scenario.withdrawer, 1, scenario.asset, 10, Filled(0x65))};
        const auto second{flowmesh::BuildProductionExecutionEntry(
            state1, scenario.domain, scenario.market, scenario.seats0.seats,
            gate, 1,
            scenario.execution0.entry.effect_start +
                scenario.execution0.entry.effect_count,
            scenario.execution0.entry.GetHash(), {210, Filled(0x75)},
            {300, flowmesh::AnchorRef{200, Filled(0x74)}, &scenario.anchors},
            scenario.treasury, actions, &scenario.chain_facts, check)};
        BOOST_REQUIRE_MESSAGE(second.has_value(),
                              flowmesh::ProductionEntryCheckName(check));
        const auto certificate1{Certify(second->entry, scenario.seats0)};
        BOOST_REQUIRE_MESSAGE(store.AppendExecution(
                                  second->entry, certificate1,
                                  scenario.seats0.seats, state1,
                                  {300, flowmesh::AnchorRef{200, Filled(0x74)},
                                   &scenario.anchors},
                                  scenario.treasury, &scenario.chain_facts,
                                  state2, error),
                              error);
        checkpoint1 = Checkpoint(second->entry, certificate1,
                                 scenario.seats0.seats, *checkpoint0_id);
        const auto checkpoint1_id{
            modern::FlowMeshCheckpointIdV1(checkpoint1.core)};
        BOOST_REQUIRE(checkpoint1_id.has_value());
        BOOST_REQUIRE_MESSAGE(store.MarkExecutionCheckpointConnected(
                                  checkpoint1, scenario.seats0.seats,
                                  {241, block1}, error),
                              error);

        error.clear();
        rolled_back = false;
        BOOST_REQUIRE_MESSAGE(store.ReconcileCheckpointConnections(
                                  {{240, replacement0}, {241, replacement1}},
                                  rolled_back, error),
                              error);
        BOOST_CHECK(rolled_back);
        BOOST_REQUIRE(store.ReadMarker(marker, error));
        BOOST_REQUIRE(marker.has_value());
        BOOST_CHECK(marker->last_b3_checkpoint == *checkpoint0_id);
        BOOST_CHECK_EQUAL(marker->current_epoch, scenario.seats0.seats.epoch);
        BOOST_REQUIRE(store.NextCheckpointCandidate(
            scenario.seats0.seats, candidate, error));
        BOOST_REQUIRE(candidate.has_value());
        BOOST_CHECK_EQUAL(candidate->stored.entry.sequence, 1U);
        BOOST_REQUIRE_MESSAGE(store.MarkExecutionCheckpointConnected(
                                  checkpoint1, scenario.seats0.seats,
                                  {241, replacement1}, error),
                              error);
        BOOST_REQUIRE(store.ReadMarker(marker, error));
        BOOST_REQUIRE(marker.has_value());
        BOOST_CHECK(marker->last_b3_checkpoint == *checkpoint1_id);
    }

    node::FlowMeshProductionStore reopened{
        DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
    BOOST_REQUIRE_MESSAGE(reopened.OpenForMarket(
                              scenario.domain, scenario.market,
                              scenario.seats0.seats, scenario.state0.Root(),
                              error),
                          error);
    SeatSource source;
    source.m_domain = scenario.domain;
    source.m_market = scenario.market;
    source.Add(scenario.seats0.seats);
    flowmesh::FlowMeshState replayed{scenario.state0};
    uint256 last_hash;
    BOOST_REQUIRE_MESSAGE(reopened.Replay(
                              replayed, last_hash, source,
                              {300, flowmesh::AnchorRef{190, Filled(0x73)},
                               &scenario.anchors},
                              scenario.treasury, &scenario.chain_facts, error),
                          error);
    BOOST_CHECK(replayed.Root() == state2.Root());
    BOOST_CHECK(last_hash == checkpoint1.core.microblock_hash);
}

BOOST_AUTO_TEST_CASE(handoff_reorg_restores_outgoing_committee_across_restart)
{
    Scenario scenario;
    const fs::path path{
        m_args.GetDataDirBase() / "flowmesh_production_handoff_reorg"};
    const uint256 block0{Filled(0xc0)};
    const uint256 block1{Filled(0xc1)};
    const uint256 replacement1{Filled(0xd1)};
    std::string error;
    modern::FlowMeshCheckpointRecordV1 handoff_checkpoint;
    modern::FlowMeshCheckpointId checkpoint0_id;

    {
        node::FlowMeshProductionStore store{DBParams{
            .path = path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
        BOOST_REQUIRE(store.OpenForMarket(
            scenario.domain, scenario.market, scenario.seats0.seats,
            scenario.state0.Root(), error));
        flowmesh::FlowMeshState state1{scenario.state0};
        BOOST_REQUIRE(store.AppendExecution(
            scenario.execution0.entry, scenario.certificate0,
            scenario.seats0.seats, scenario.state0,
            {300, flowmesh::AnchorRef{190, Filled(0x73)}, &scenario.anchors},
            scenario.treasury, &scenario.chain_facts, state1, error));
        const auto checkpoint0{Checkpoint(
            scenario.execution0.entry, scenario.certificate0,
            scenario.seats0.seats, {})};
        const auto id{modern::FlowMeshCheckpointIdV1(checkpoint0.core)};
        BOOST_REQUIRE(id.has_value());
        checkpoint0_id = *id;
        BOOST_REQUIRE(store.MarkExecutionCheckpointConnected(
            checkpoint0, scenario.seats0.seats, {240, block0}, error));
        BOOST_REQUIRE(store.AppendHandoff(
            scenario.handoff1, scenario.certificate1, scenario.seats0.seats,
            scenario.seats1.seats, state1,
            {300, flowmesh::AnchorRef{200, Filled(0x74)}, &scenario.anchors},
            error));
        handoff_checkpoint = Checkpoint(
            scenario.handoff1, scenario.certificate1, scenario.seats0.seats,
            checkpoint0_id);
        BOOST_REQUIRE(store.MarkHandoffCheckpointConnected(
            handoff_checkpoint, scenario.seats0.seats, scenario.seats1.seats,
            {241, block1}, 300, error));

        bool rolled_back{false};
        BOOST_REQUIRE_MESSAGE(store.ReconcileCheckpointConnections(
                                  {{240, block0}, {241, replacement1}},
                                  rolled_back, error),
                              error);
        BOOST_CHECK(rolled_back);
        std::optional<node::FlowMeshProductionStore::Marker> marker;
        BOOST_REQUIRE(store.ReadMarker(marker, error));
        BOOST_REQUIRE(marker.has_value());
        BOOST_CHECK_EQUAL(marker->current_epoch, scenario.seats0.seats.epoch);
        BOOST_CHECK((marker->current_anchor ==
                     flowmesh::AnchorRef{100, Filled(0x71)}));
        BOOST_CHECK(marker->current_seat_set_hash ==
                    scenario.seats0.seats.set_hash);
        BOOST_CHECK(marker->last_b3_checkpoint == checkpoint0_id);
    }

    // Startup sees the already-rolled-back outgoing marker, replays the
    // unconnected handoff as a pending tip, and can accept the same certified
    // checkpoint on a replacement B3 block.
    node::FlowMeshProductionStore reopened{
        DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
    BOOST_REQUIRE_MESSAGE(reopened.OpenForMarket(
                              scenario.domain, scenario.market,
                              scenario.seats0.seats, scenario.state0.Root(),
                              error),
                          error);
    SeatSource source;
    source.m_domain = scenario.domain;
    source.m_market = scenario.market;
    source.Add(scenario.seats0.seats);
    source.Add(scenario.seats1.seats);
    flowmesh::FlowMeshState replayed{scenario.state0};
    uint256 last_hash;
    BOOST_REQUIRE_MESSAGE(reopened.Replay(
                              replayed, last_hash, source,
                              {300, flowmesh::AnchorRef{190, Filled(0x73)},
                               &scenario.anchors},
                              scenario.treasury, &scenario.chain_facts, error),
                          error);
    std::optional<node::ProductionCheckpointCandidate> candidate;
    BOOST_REQUIRE(reopened.NextCheckpointCandidate(
        scenario.seats0.seats, candidate, error));
    BOOST_REQUIRE(candidate.has_value());
    BOOST_CHECK_EQUAL(candidate->stored.entry.sequence, 1U);
    BOOST_REQUIRE_MESSAGE(reopened.MarkHandoffCheckpointConnected(
                              handoff_checkpoint, scenario.seats0.seats,
                              scenario.seats1.seats, {242, replacement1}, 300,
                              error),
                          error);
    std::optional<node::FlowMeshProductionStore::Marker> marker;
    BOOST_REQUIRE(reopened.ReadMarker(marker, error));
    BOOST_REQUIRE(marker.has_value());
    BOOST_CHECK_EQUAL(marker->current_epoch, scenario.seats1.seats.epoch);
    BOOST_CHECK(marker->current_seat_set_hash == scenario.seats1.seats.set_hash);
}

BOOST_AUTO_TEST_CASE(handoff_waits_for_depth_and_republishes_after_shallow_reorg)
{
    Scenario scenario;
    const fs::path path{
        m_args.GetDataDirBase() / "flowmesh_production_handoff_maturity"};
    const uint256 genesis_block{Filled(0xd0)};
    const uint256 shallow_block{Filled(0xd1)};
    const uint256 republished_block{Filled(0xd2)};
    std::string error;
    node::FlowMeshProductionStore store{DBParams{
        .path = path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
    BOOST_REQUIRE(store.OpenForMarket(
        scenario.domain, scenario.market, scenario.seats0.seats,
        scenario.state0.Root(), error));

    flowmesh::FlowMeshState state1{scenario.state0};
    BOOST_REQUIRE(store.AppendExecution(
        scenario.execution0.entry, scenario.certificate0,
        scenario.seats0.seats, scenario.state0,
        {300, flowmesh::AnchorRef{190, Filled(0x73)}, &scenario.anchors},
        scenario.treasury, &scenario.chain_facts, state1, error));
    const auto genesis_checkpoint{Checkpoint(
        scenario.execution0.entry, scenario.certificate0,
        scenario.seats0.seats, {})};
    const auto genesis_id{
        modern::FlowMeshCheckpointIdV1(genesis_checkpoint.core)};
    BOOST_REQUIRE(genesis_id.has_value());
    BOOST_REQUIRE(store.MarkExecutionCheckpointConnected(
        genesis_checkpoint, scenario.seats0.seats, {240, genesis_block},
        error));
    BOOST_REQUIRE(store.AppendHandoff(
        scenario.handoff1, scenario.certificate1, scenario.seats0.seats,
        scenario.seats1.seats, state1,
        {300, flowmesh::AnchorRef{200, Filled(0x74)}, &scenario.anchors},
        error));
    const auto handoff_checkpoint{Checkpoint(
        scenario.handoff1, scenario.certificate1, scenario.seats0.seats,
        *genesis_id)};

    const node::ProductionB3Connection shallow{270, shallow_block};
    BOOST_CHECK(!node::FlowMeshHandoffConnectionMature(shallow, 299));
    BOOST_CHECK(node::FlowMeshHandoffConnectionMature(shallow, 300));
    BOOST_CHECK(!store.MarkHandoffCheckpointConnected(
        handoff_checkpoint, scenario.seats0.seats, scenario.seats1.seats,
        shallow, 299, error));
    BOOST_CHECK(error.find("not deep enough") != std::string::npos);

    // The shallow publication was never written as a connection, so its
    // disappearance cannot require an epoch rollback. The outgoing marker
    // remains authoritative and the incoming committee cannot append.
    std::optional<node::FlowMeshProductionStore::Marker> marker;
    BOOST_REQUIRE(store.ReadMarker(marker, error));
    BOOST_REQUIRE(marker.has_value());
    BOOST_CHECK_EQUAL(marker->current_epoch, scenario.seats0.seats.epoch);
    std::vector<int32_t> heights;
    BOOST_REQUIRE(store.ConnectedB3Heights(heights, error));
    BOOST_REQUIRE_EQUAL(heights.size(), 1U);
    BOOST_CHECK_EQUAL(heights.front(), 240);
    flowmesh::FlowMeshState refused{state1};
    BOOST_CHECK(!store.AppendExecution(
        scenario.execution2.entry, scenario.certificate2,
        scenario.seats1.seats, state1,
        {300, flowmesh::AnchorRef{210, Filled(0x75)}, &scenario.anchors},
        scenario.treasury, &scenario.chain_facts, refused, error));

    // The byte-identical handoff can be published in a replacement block. It
    // remains pending at depth 29 and activates exactly at depth 30.
    const node::ProductionB3Connection republished{271, republished_block};
    BOOST_CHECK(!store.MarkHandoffCheckpointConnected(
        handoff_checkpoint, scenario.seats0.seats, scenario.seats1.seats,
        republished, 300, error));
    BOOST_REQUIRE_MESSAGE(store.MarkHandoffCheckpointConnected(
                              handoff_checkpoint, scenario.seats0.seats,
                              scenario.seats1.seats, republished, 301, error),
                          error);
    BOOST_REQUIRE(store.ReadMarker(marker, error));
    BOOST_REQUIRE(marker.has_value());
    BOOST_CHECK_EQUAL(marker->current_epoch, scenario.seats1.seats.epoch);
    BOOST_CHECK(marker->current_seat_set_hash ==
                scenario.seats1.seats.set_hash);
}

BOOST_AUTO_TEST_CASE(handoff_reorg_after_incoming_entry_fails_closed)
{
    Scenario scenario;
    const fs::path path{
        m_args.GetDataDirBase() / "flowmesh_production_unsafe_handoff_reorg"};
    const uint256 block0{Filled(0xe0)};
    const uint256 block1{Filled(0xe1)};
    std::string error;
    node::FlowMeshProductionStore store{DBParams{
        .path = path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
    BOOST_REQUIRE(store.OpenForMarket(
        scenario.domain, scenario.market, scenario.seats0.seats,
        scenario.state0.Root(), error));
    flowmesh::FlowMeshState state1{scenario.state0};
    BOOST_REQUIRE(store.AppendExecution(
        scenario.execution0.entry, scenario.certificate0,
        scenario.seats0.seats, scenario.state0,
        {300, flowmesh::AnchorRef{190, Filled(0x73)}, &scenario.anchors},
        scenario.treasury, &scenario.chain_facts, state1, error));
    const auto checkpoint0{Checkpoint(
        scenario.execution0.entry, scenario.certificate0,
        scenario.seats0.seats, {})};
    const auto checkpoint0_id{modern::FlowMeshCheckpointIdV1(checkpoint0.core)};
    BOOST_REQUIRE(checkpoint0_id.has_value());
    BOOST_REQUIRE(store.MarkExecutionCheckpointConnected(
        checkpoint0, scenario.seats0.seats, {240, block0}, error));
    BOOST_REQUIRE(store.AppendHandoff(
        scenario.handoff1, scenario.certificate1, scenario.seats0.seats,
        scenario.seats1.seats, state1,
        {300, flowmesh::AnchorRef{200, Filled(0x74)}, &scenario.anchors},
        error));
    const auto handoff_checkpoint{Checkpoint(
        scenario.handoff1, scenario.certificate1, scenario.seats0.seats,
        *checkpoint0_id)};
    BOOST_REQUIRE(store.MarkHandoffCheckpointConnected(
        handoff_checkpoint, scenario.seats0.seats, scenario.seats1.seats,
        {241, block1}, 300, error));
    flowmesh::FlowMeshState state2{state1};
    BOOST_REQUIRE(store.AppendExecution(
        scenario.execution2.entry, scenario.certificate2,
        scenario.seats1.seats, state1,
        {300, flowmesh::AnchorRef{210, Filled(0x75)}, &scenario.anchors},
        scenario.treasury, &scenario.chain_facts, state2, error));

    bool rolled_back{false};
    error.clear();
    BOOST_CHECK(!store.ReconcileCheckpointConnections(
        {{240, block0}, {241, Filled(0xf1)}}, rolled_back, error));
    BOOST_CHECK(!rolled_back);
    BOOST_CHECK(error.find("cannot safely roll back a handoff") !=
                std::string::npos);
    BOOST_CHECK(store.LockOnce({8, 3}, Filled(0xf2)) ==
                flowmesh::ProductionLockResult::STORAGE_FAILURE);
}

BOOST_AUTO_TEST_CASE(zero_effect_market_genesis_is_mandatory_then_empty_entries_skip)
{
    Scenario scenario;
    const fs::path path{
        m_args.GetDataDirBase() / "flowmesh_production_v3_empty_genesis"};
    flowmesh::ProductionEpochGate gate{
        scenario.domain, scenario.market, scenario.seats0.seats};
    const std::vector<flowmesh::Action> no_actions;
    flowmesh::ProductionEntryCheck check;
    const auto first{flowmesh::BuildProductionExecutionEntry(
        scenario.state0, scenario.domain, scenario.market,
        scenario.seats0.seats, gate, 0, 0, uint256{},
        {200, Filled(0x74)},
        {300, flowmesh::AnchorRef{190, Filled(0x73)}, &scenario.anchors},
        scenario.treasury, no_actions, &scenario.chain_facts, check)};
    BOOST_REQUIRE_MESSAGE(first.has_value(),
                          flowmesh::ProductionEntryCheckName(check));
    BOOST_REQUIRE(first->effects.empty());
    const auto first_certificate{Certify(first->entry, scenario.seats0)};

    node::FlowMeshProductionStore store{DBParams{
        .path = path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
    std::string error;
    BOOST_REQUIRE_MESSAGE(store.OpenForMarket(
                              scenario.domain, scenario.market,
                              scenario.seats0.seats, scenario.state0.Root(),
                              error),
                          error);
    flowmesh::FlowMeshState state1{scenario.state0};
    BOOST_REQUIRE_MESSAGE(store.AppendExecution(
                              first->entry, first_certificate,
                              scenario.seats0.seats, scenario.state0,
                              {300, flowmesh::AnchorRef{190, Filled(0x73)},
                               &scenario.anchors},
                              scenario.treasury, &scenario.chain_facts,
                              state1, error),
                          error);
    std::optional<node::ProductionCheckpointCandidate> candidate;
    BOOST_REQUIRE_MESSAGE(store.NextCheckpointCandidate(
                              scenario.seats0.seats, candidate, error),
                          error);
    BOOST_REQUIRE(candidate.has_value());
    BOOST_CHECK_EQUAL(candidate->stored.entry.sequence, 0U);
    BOOST_CHECK(candidate->stored.effects.empty());

    const auto checkpoint{Checkpoint(first->entry, first_certificate,
                                     scenario.seats0.seats, {})};
    BOOST_REQUIRE_MESSAGE(store.MarkExecutionCheckpointConnected(
                              checkpoint, scenario.seats0.seats,
                              {240, Filled(0xa0)}, error),
                          error);

    const auto second{flowmesh::BuildProductionExecutionEntry(
        state1, scenario.domain, scenario.market, scenario.seats0.seats, gate,
        1, 0, first->entry.GetHash(), {210, Filled(0x75)},
        {300, flowmesh::AnchorRef{200, Filled(0x74)}, &scenario.anchors},
        scenario.treasury, no_actions, &scenario.chain_facts, check)};
    BOOST_REQUIRE_MESSAGE(second.has_value(),
                          flowmesh::ProductionEntryCheckName(check));
    const auto second_certificate{Certify(second->entry, scenario.seats0)};
    flowmesh::FlowMeshState state2{state1};
    BOOST_REQUIRE_MESSAGE(store.AppendExecution(
                              second->entry, second_certificate,
                              scenario.seats0.seats, state1,
                              {300, flowmesh::AnchorRef{200, Filled(0x74)},
                               &scenario.anchors},
                              scenario.treasury, &scenario.chain_facts,
                              state2, error),
                          error);
    BOOST_REQUIRE_MESSAGE(store.NextCheckpointCandidate(
                              scenario.seats0.seats, candidate, error),
                          error);
    BOOST_CHECK(!candidate);
}

BOOST_AUTO_TEST_CASE(settlement_only_execution_is_durable_mandatory_and_replayable)
{
    Scenario scenario;
    const fs::path path{
        m_args.GetDataDirBase() / "flowmesh_production_v3_settlement"};
    const flowmesh::AnchorRef request_previous{190, Filled(0x73)};
    const flowmesh::AnchorRef request_anchor{200, Filled(0x74)};
    const flowmesh::AnchorRef settlement_anchor{210, Filled(0x75)};

    const auto request_checkpoint{
        Checkpoint(scenario.execution0.entry, scenario.certificate0,
                   scenario.seats0.seats, {})};
    const auto request_checkpoint_id{
        modern::FlowMeshCheckpointIdV1(request_checkpoint.core)};
    BOOST_REQUIRE(request_checkpoint_id.has_value());
    const modern::FlowMeshWithdrawalReceiptV1* receipt{nullptr};
    for (const auto& effect : scenario.execution0.effects) {
        if (const auto* candidate{
                std::get_if<modern::FlowMeshWithdrawalReceiptV1>(&effect)}) {
            receipt = candidate;
            break;
        }
    }
    BOOST_REQUIRE(receipt != nullptr);

    SettlementFacts facts;
    facts.request_previous = request_previous;
    facts.request_anchor = request_anchor;
    facts.settlement_anchor = settlement_anchor;
    facts.settlements = {flowmesh::WithdrawalSettlementFactV1{
        *receipt, *request_checkpoint_id,
        Txid::FromUint256(Filled(0xb4)), 205, Filled(0xb5)}};

    flowmesh::ProductionEpochGate gate{
        scenario.domain, scenario.market, scenario.seats0.seats};
    flowmesh::ProductionEntryCheck check;
    const std::vector<flowmesh::Action> no_actions;
    const uint64_t next_effect_index{
        scenario.execution0.entry.effect_start +
        scenario.execution0.entry.effect_count};
    const auto settlement{flowmesh::BuildProductionExecutionEntry(
        scenario.execution0.next_state, scenario.domain, scenario.market,
        scenario.seats0.seats, gate, 1, next_effect_index,
        scenario.execution0.entry.GetHash(), settlement_anchor,
        {300, request_anchor, &scenario.anchors}, scenario.treasury,
        no_actions, &facts, check)};
    BOOST_REQUIRE_MESSAGE(settlement.has_value(),
                          flowmesh::ProductionEntryCheckName(check));
    BOOST_CHECK(settlement->effects.empty());
    BOOST_REQUIRE_EQUAL(settlement->settlements.size(), 1U);
    BOOST_CHECK(settlement->settlements == facts.settlements);
    const auto settlement_certificate{Certify(settlement->entry,
                                              scenario.seats0)};

    {
        node::FlowMeshProductionStore store{DBParams{
            .path = path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
        std::string error;
        BOOST_REQUIRE_MESSAGE(store.OpenForMarket(
                                  scenario.domain, scenario.market,
                                  scenario.seats0.seats, scenario.state0.Root(),
                                  error),
                              error);
        flowmesh::FlowMeshState request_state{scenario.state0};
        BOOST_REQUIRE_MESSAGE(store.AppendExecution(
                                  scenario.execution0.entry,
                                  scenario.certificate0, scenario.seats0.seats,
                                  scenario.state0,
                                  {300, request_previous, &scenario.anchors},
                                  scenario.treasury, &facts, request_state,
                                  error),
                              error);
        BOOST_REQUIRE_MESSAGE(store.MarkExecutionCheckpointConnected(
                                  request_checkpoint, scenario.seats0.seats,
                                  {240, Filled(0xa0)},
                                  error),
                              error);

        flowmesh::FlowMeshState settled_state{request_state};
        BOOST_REQUIRE_MESSAGE(store.AppendExecution(
                                  settlement->entry, settlement_certificate,
                                  scenario.seats0.seats, request_state,
                                  {300, request_anchor, &scenario.anchors},
                                  scenario.treasury, &facts, settled_state,
                                  error),
                              error);
        BOOST_CHECK(settled_state.Root() == settlement->next_state.Root());

        std::optional<node::ProductionCheckpointCandidate> candidate;
        BOOST_REQUIRE_MESSAGE(store.NextCheckpointCandidate(
                                  scenario.seats0.seats, candidate, error),
                              error);
        BOOST_REQUIRE(candidate.has_value());
        BOOST_CHECK_EQUAL(candidate->stored.entry.sequence, 1U);
        BOOST_CHECK(candidate->stored.effects.empty());
        BOOST_CHECK(candidate->stored.settlements == facts.settlements);
        BOOST_CHECK(candidate->previous_checkpoint_id ==
                    *request_checkpoint_id);

        std::optional<node::StoredProductionEntry> stored;
        BOOST_REQUIRE_MESSAGE(store.ReadEntry(
                                  1, scenario.seats0.seats, stored, error),
                              error);
        BOOST_REQUIRE(stored.has_value());
        BOOST_CHECK(stored->settlements == facts.settlements);
    }

    node::FlowMeshProductionStore reopened{
        DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
    std::string error;
    BOOST_REQUIRE_MESSAGE(reopened.OpenForMarket(
                              scenario.domain, scenario.market,
                              scenario.seats0.seats, scenario.state0.Root(),
                              error),
                          error);
    SeatSource source;
    source.m_domain = scenario.domain;
    source.m_market = scenario.market;
    source.Add(scenario.seats0.seats);
    flowmesh::FlowMeshState replayed{scenario.state0};
    uint256 last_hash;
    BOOST_REQUIRE_MESSAGE(reopened.Replay(
                              replayed, last_hash, source,
                              {300, request_previous, &scenario.anchors},
                              scenario.treasury, &facts, error),
                          error);
    BOOST_CHECK(replayed.Root() == settlement->next_state.Root());
    BOOST_CHECK(last_hash == settlement->entry.GetHash());
    std::optional<node::ProductionCheckpointCandidate> candidate;
    BOOST_REQUIRE_MESSAGE(reopened.NextCheckpointCandidate(
                              scenario.seats0.seats, candidate, error),
                          error);
    BOOST_REQUIRE(candidate.has_value());
    BOOST_CHECK_EQUAL(candidate->stored.entry.sequence, 1U);
    BOOST_CHECK(candidate->stored.settlements == facts.settlements);
}

BOOST_AUTO_TEST_CASE(permanent_epoch_sequence_lock_survives_restart)
{
    Scenario scenario;
    const fs::path path{m_args.GetDataDirBase() / "flowmesh_production_locks"};
    std::string error;
    const flowmesh::ProductionSignPosition position{7, 0};
    const uint256 hash1{scenario.execution0.entry.GetHash()};
    const uint256 hash2{Filled(0xb2)};
    std::vector<flowmesh::Action> evidence{scenario.execution0.entry.actions};
    for (flowmesh::Action& action : evidence) {
        if (!action.IsDeposit()) action.credential = {0x01};
    }
    {
        node::FlowMeshProductionStore store{DBParams{
            .path = path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
        BOOST_REQUIRE(store.OpenForMarket(
            scenario.domain, scenario.market, scenario.seats0.seats,
            scenario.state0.Root(), error));
        // The production store refuses a hash-only first lock: the exact
        // candidate and its authenticated evidence must become durable in
        // the same synchronous batch as the permanent lock.
        BOOST_CHECK(store.LockOnce(position, hash1) ==
                    flowmesh::ProductionLockResult::STORAGE_FAILURE);
        BOOST_CHECK(store.LockCandidate(scenario.execution0.entry, evidence) ==
                    flowmesh::ProductionLockResult::LOCKED);
        BOOST_CHECK(store.LockOnce(position, hash1) ==
                    flowmesh::ProductionLockResult::ALREADY_LOCKED_SAME);
        BOOST_CHECK(store.LockOnce(position, hash2) ==
                    flowmesh::ProductionLockResult::CONFLICT);
        BOOST_CHECK(store.LockOnce({8, 0}, hash1) ==
                    flowmesh::ProductionLockResult::STORAGE_FAILURE);
        BOOST_CHECK(store.LockOnce({7, 1}, hash1) ==
                    flowmesh::ProductionLockResult::STORAGE_FAILURE);
        std::optional<node::StoredLockedProductionCandidate> retained;
        BOOST_REQUIRE(store.ReadLockedCandidate(position, retained, error));
        BOOST_REQUIRE(retained.has_value());
        BOOST_CHECK(retained->entry.GetHash() == hash1);
        BOOST_CHECK_EQUAL(retained->evidence.size(), evidence.size());
    }
    node::FlowMeshProductionStore reopened{
        DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
    BOOST_REQUIRE(reopened.OpenForMarket(
        scenario.domain, scenario.market, scenario.seats0.seats,
        scenario.state0.Root(), error));
    std::optional<uint256> restored;
    BOOST_REQUIRE(reopened.ReadLock(position, restored, error));
    BOOST_REQUIRE(restored.has_value());
    BOOST_CHECK(*restored == hash1);
    std::optional<node::StoredLockedProductionCandidate> retained;
    BOOST_REQUIRE(reopened.ReadLockedCandidate(position, retained, error));
    BOOST_REQUIRE(retained.has_value());
    BOOST_CHECK(retained->entry.GetHash() == hash1);
    BOOST_CHECK(reopened.LockOnce(position, hash2) ==
                flowmesh::ProductionLockResult::CONFLICT);

    flowmesh::FlowMeshState committed{scenario.state0};
    BOOST_REQUIRE_MESSAGE(reopened.AppendExecution(
                              scenario.execution0.entry,
                              scenario.certificate0, scenario.seats0.seats,
                              scenario.state0,
                              {300,
                               flowmesh::AnchorRef{190, Filled(0x73)},
                               &scenario.anchors},
                              scenario.treasury, &scenario.chain_facts,
                              committed, error),
                          error);
    retained.reset();
    BOOST_REQUIRE(reopened.ReadLockedCandidate(position, retained, error));
    BOOST_CHECK(!retained.has_value());
    BOOST_REQUIRE(reopened.ReadLock(position, restored, error));
    BOOST_REQUIRE(restored.has_value());
    BOOST_CHECK(*restored == hash1);
}

BOOST_AUTO_TEST_CASE(lock_without_retained_candidate_fails_closed_on_restart)
{
    Scenario scenario;
    const fs::path path{
        m_args.GetDataDirBase() / "flowmesh_production_missing_candidate"};
    std::string error;
    std::vector<flowmesh::Action> evidence{scenario.execution0.entry.actions};
    for (flowmesh::Action& action : evidence) {
        if (!action.IsDeposit()) action.credential = {0x01};
    }
    {
        node::FlowMeshProductionStore store{DBParams{
            .path = path, .cache_bytes = size_t{1} << 20,
            .wipe_data = true}};
        BOOST_REQUIRE(store.OpenForMarket(
            scenario.domain, scenario.market, scenario.seats0.seats,
            scenario.state0.Root(), error));
        BOOST_REQUIRE(store.LockCandidate(scenario.execution0.entry,
                                          evidence) ==
                      flowmesh::ProductionLockResult::LOCKED);
    }
    {
        CDBWrapper raw{
            DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
        raw.Erase(TestLockKey{'p', 7, 0}, /*fSync=*/true);
    }
    node::FlowMeshProductionStore reopened{
        DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
    BOOST_CHECK(!reopened.OpenForMarket(
        scenario.domain, scenario.market, scenario.seats0.seats,
        scenario.state0.Root(), error));
    BOOST_CHECK(error.find("no exact candidate") != std::string::npos);
    BOOST_CHECK(reopened.LockOnce({7, 0},
                                  scenario.execution0.entry.GetHash()) ==
                flowmesh::ProductionLockResult::STORAGE_FAILURE);
}

BOOST_AUTO_TEST_CASE(append_rejects_wrong_sequence_parent_state_set_and_certificate)
{
    Scenario scenario;
    const fs::path path{m_args.GetDataDirBase() / "flowmesh_production_append"};
    std::string error;
    node::FlowMeshProductionStore store{DBParams{
        .path = path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
    BOOST_REQUIRE(store.OpenForMarket(
        scenario.domain, scenario.market, scenario.seats0.seats,
        scenario.state0.Root(), error));
    flowmesh::FlowMeshState output{scenario.state0};
    const flowmesh::ProductionAnchorContext anchors{
        300, flowmesh::AnchorRef{190, Filled(0x73)}, &scenario.anchors};

    auto wrong{scenario.execution0.entry};
    wrong.sequence = 1;
    wrong.parent_hash = Filled(0x91);
    BOOST_CHECK(!store.AppendExecution(
        wrong, scenario.certificate0, scenario.seats0.seats, scenario.state0,
        anchors, scenario.treasury, &scenario.chain_facts, output, error));

    wrong = scenario.execution0.entry;
    wrong.parent_hash = Filled(0x92);
    BOOST_CHECK(!store.AppendExecution(
        wrong, scenario.certificate0, scenario.seats0.seats, scenario.state0,
        anchors, scenario.treasury, &scenario.chain_facts, output, error));

    wrong = scenario.execution0.entry;
    wrong.previous_state_root = Filled(0x93);
    BOOST_CHECK(!store.AppendExecution(
        wrong, scenario.certificate0, scenario.seats0.seats, scenario.state0,
        anchors, scenario.treasury, &scenario.chain_facts, output, error));

    auto bad_certificate{scenario.certificate0};
    bad_certificate.aggregate_signature[0] ^= 1;
    BOOST_CHECK(!store.AppendExecution(
        scenario.execution0.entry, bad_certificate, scenario.seats0.seats,
        scenario.state0, anchors, scenario.treasury, &scenario.chain_facts,
        output, error));
    BOOST_CHECK(!store.AppendExecution(
        scenario.execution0.entry, scenario.certificate0,
        scenario.seats1.seats, scenario.state0, anchors, scenario.treasury,
        &scenario.chain_facts, output, error));

    BOOST_REQUIRE_MESSAGE(store.AppendExecution(
                              scenario.execution0.entry,
                              scenario.certificate0, scenario.seats0.seats,
                              scenario.state0, anchors, scenario.treasury,
                              &scenario.chain_facts, output, error),
                          error);
    BOOST_CHECK(output.Root() == scenario.execution0.next_state.Root());
}

BOOST_AUTO_TEST_CASE(namespace_corruption_and_v2_marker_fail_closed)
{
    Scenario scenario;
    const fs::path path{m_args.GetDataDirBase() / "flowmesh_production_corrupt"};
    std::string error;
    {
        CDBWrapper raw{DBParams{.path = path, .cache_bytes = size_t{1} << 20,
                                .wipe_data = true}};
        node::FlowMeshStore::Marker old_marker;
        old_marker.domain = scenario.domain;
        raw.Write(uint8_t{'m'}, old_marker, /*fSync=*/true);
    }
    {
        node::FlowMeshProductionStore store{
            DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
        BOOST_CHECK(!store.OpenForMarket(
            scenario.domain, scenario.market, scenario.seats0.seats,
            scenario.state0.Root(), error));
    }
    {
        CDBWrapper raw{DBParams{.path = path, .cache_bytes = size_t{1} << 20,
                                .wipe_data = true}};
        raw.Write(std::make_pair(uint8_t{'e'}, uint64_t{0}), uint8_t{1}, true);
    }
    {
        node::FlowMeshProductionStore store{
            DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
        BOOST_CHECK(!store.OpenForMarket(
            scenario.domain, scenario.market, scenario.seats0.seats,
            scenario.state0.Root(), error));
    }
    {
        node::FlowMeshProductionStore store{DBParams{
            .path = path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
        BOOST_REQUIRE(store.OpenForMarket(
            scenario.domain, scenario.market, scenario.seats0.seats,
            scenario.state0.Root(), error));
    }
    {
        CDBWrapper raw{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
        node::FlowMeshProductionStore::Marker marker;
        BOOST_REQUIRE(raw.Read(uint8_t{'m'}, marker));
        marker.next_sequence = 1;
        marker.last_microblock_hash = Filled(0xc1);
        raw.Write(uint8_t{'m'}, marker, true);
    }
    {
        node::FlowMeshProductionStore store{
            DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
        BOOST_CHECK(!store.OpenForMarket(
            scenario.domain, scenario.market, scenario.seats0.seats,
            scenario.state0.Root(), error));
    }
}

BOOST_AUTO_TEST_CASE(malformed_lock_namespace_blocks_reopen_and_signing)
{
    Scenario scenario;
    const fs::path path{m_args.GetDataDirBase() / "flowmesh_production_badlock"};
    std::string error;
    {
        node::FlowMeshProductionStore store{DBParams{
            .path = path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
        BOOST_REQUIRE(store.OpenForMarket(
            scenario.domain, scenario.market, scenario.seats0.seats,
            scenario.state0.Root(), error));
    }
    {
        CDBWrapper raw{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
        raw.Write(TestLockKey{'l', 7, 0}, uint8_t{0x42}, true);
    }
    node::FlowMeshProductionStore reopened{
        DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
    BOOST_CHECK(!reopened.OpenForMarket(
        scenario.domain, scenario.market, scenario.seats0.seats,
        scenario.state0.Root(), error));
    BOOST_CHECK(reopened.LockOnce({7, 0}, Filled(0xd1)) ==
                flowmesh::ProductionLockResult::STORAGE_FAILURE);
}

BOOST_AUTO_TEST_SUITE_END()
