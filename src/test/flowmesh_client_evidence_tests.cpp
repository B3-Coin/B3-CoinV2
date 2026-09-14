// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/auth.h>
#include <flowmesh/client_evidence.h>
#include <node/flowmesh_runtime.h>
#include <test/util/flowmesh.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>

namespace {

uint256 Filled(const unsigned char n)
{
    uint256 out;
    std::fill(out.begin(), out.end(), n);
    return out;
}

struct EvidenceFixture {
    flowmesh::ClientEvidencePins pins;
    flowmesh::ActiveFnBlsSeatSet seats;
    std::vector<bls::SecretKey> keys;
    CKey account_key;
    flowmesh::AccountId account;
    flowmesh::AnchorRef anchor{100, Filled(3)};
    flowmesh::FlowMeshState state;
    flowmesh::ProductionCertifiedEnvelope certified;

    EvidenceFixture()
        : pins{Filled(1), *flowmesh::ComputeFlowMeshMarketId(Filled(1), Filled(2)), Filled(2), {}, {}},
          state{*flowmesh::ComputeFlowMeshVaultId(pins.domain, pins.market_id), pins.base_asset, modern::NativeAsset()}
    {
        pins.vault_id = state.LedgerView().VaultCommitment();
        pins.execution_config_id = state.ConfigId();
        std::array<unsigned char, 32> secret{};
        secret.back() = 1;
        account_key.Set(secret.begin(), secret.end(), true);
        account = flowmesh::AccountForKey(XOnlyPubKey{account_key.GetPubKey()});
        BOOST_REQUIRE(flowmesh::test_only::StateFunding::Fund(state, account, modern::NativeAsset(), 100000));
        BOOST_REQUIRE(flowmesh::test_only::StateFunding::Fund(state, account, pins.base_asset, 200));
        BOOST_REQUIRE(state.SubmitCurve(account, flowmesh::ClearingEngine::Side::BID, {{7, 10}, {8, 0}}));
        flowmesh::test_only::StateFunding::SetNextSequence(state, account, 7);
        struct Member { bls::SecretKey key; flowmesh::BlsSeatBinding binding; uint256 id; };
        std::vector<Member> members;
        for (unsigned char i{1}; i <= 4; ++i) {
            std::array<unsigned char, 32> seed{};
            seed.fill(i);
            const auto key{bls::SecretKey::FromIKM(seed)};
            BOOST_REQUIRE(key);
            flowmesh::BlsSeatBinding binding{COutPoint{Txid::FromUint256(Filled(i + 10)), i},
                key->GetPublicKey().Compressed(), key->SignPoP().Compressed()};
            members.push_back({*key, binding, flowmesh::ComputeFlowMeshSeatId(pins.domain, binding.outpoint)});
        }
        std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        std::vector<flowmesh::BlsSeatBinding> bindings;
        for (const auto& member : members) { keys.push_back(member.key); bindings.push_back(member.binding); }
        flowmesh::BlsSeatSetCheck check;
        const auto set{flowmesh::BuildActiveFnBlsSeatSet(pins.domain, pins.market_id, 0,
            anchor.height, anchor.hash, bindings, check)};
        BOOST_REQUIRE(set);
        seats = *set;
        auto& entry{certified.entry};
        entry.domain = pins.domain;
        entry.market_id = pins.market_id;
        entry.seat_set_hash = seats.set_hash;
        entry.anchor = anchor;
        entry.previous_state_root = Filled(4);
        entry.actions_root = flowmesh::ComputeProductionActionsRoot(entry.actions);
        entry.result_root = Filled(5);
        entry.state_root = state.Root();
        entry.effect_root = modern::EmptyFlowMeshEffectRoot(0);
        certified.certificate = Certify(entry);
    }

    flowmesh::BlsMicroblockCertificate Certify(const flowmesh::ProductionEntryCore& entry) const
    {
        std::vector<flowmesh::IndexedBlsSignature> signatures;
        for (uint32_t i{0}; i < 3; ++i) {
            const auto sig{flowmesh::SignBlsMicroblockCertificate(keys[i], flowmesh::ProductionCertificateContext(entry), seats)};
            BOOST_REQUIRE(sig);
            signatures.push_back({i, *sig});
        }
        flowmesh::BlsMicroblockCertificate out;
        BOOST_REQUIRE(flowmesh::AssembleProductionEntryCertificate(entry, seats, signatures, out) ==
                      flowmesh::BlsCertificateAssemblyCheck::OK);
        return out;
    }

    flowmesh::ClientStateEvidence Evidence() const
    {
        std::string error;
        const auto bytes{flowmesh::EncodeClientState(state, error)};
        const auto payload{flowmesh::EncodeProductionCertifiedPayload(certified, seats.Size())};
        BOOST_REQUIRE(bytes);
        BOOST_REQUIRE(payload);
        return {*payload, *bytes, {Filled(99), 12}};
    }

    std::optional<flowmesh::VerifiedClientState> Verify(const flowmesh::ClientStateEvidence& evidence,
                                                       std::string& error) const
    {
        return flowmesh::VerifyClientStateEvidence(pins, evidence, seats,
            [&](const auto& candidate) { return candidate == anchor; }, error);
    }
};

class EvidenceChain final : public node::FlowMeshRuntimeChain {
public:
    explicit EvidenceChain(const EvidenceFixture& f) : fixture{f} {}
    int32_t TipHeight() const override { return 130; }
    bool Acceptable(const flowmesh::AnchorRef& anchor) const override { return StillCanonical(anchor); }
    bool StillCanonical(const flowmesh::AnchorRef& anchor) const override { return anchor == fixture.anchor; }
    flowmesh::AnchorRef Current() const override { return fixture.anchor; }
    std::optional<flowmesh::ActiveFnBlsSeatSet> SeatSet(const uint256& domain, const uint256& market,
        uint64_t epoch, const uint256& set) const override
    {
        if (domain != fixture.pins.domain || market != fixture.pins.market_id ||
            epoch != fixture.seats.epoch || set != fixture.seats.set_hash) return std::nullopt;
        return fixture.seats;
    }
    std::optional<flowmesh::ActiveFnBlsSeatSet> SeatSetForSequence(const uint256& domain,
        const uint256& market, uint64_t) const override
    {
        return SeatSet(domain, market, fixture.seats.epoch, fixture.seats.set_hash);
    }
private:
    const EvidenceFixture& fixture;
};

class NoSeatKeys final : public node::FlowMeshRuntimeKeyProvider {
    std::vector<bls::SecretKey> LocalSeatKeys(const uint256&,
        const flowmesh::ActiveFnBlsSeatSet&) const override { return {}; }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_client_evidence_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(whole_state_authenticates_account_and_book_not_a_supplied_row)
{
    EvidenceFixture f;
    std::string error;
    const auto verified{f.Verify(f.Evidence(), error)};
    BOOST_REQUIRE_MESSAGE(verified, error);
    auto data{flowmesh::ClientMarketData(f.pins, *verified, f.account, {}, error)};
    BOOST_REQUIRE(data && data->account);
    BOOST_CHECK_EQUAL(data->account->next_sequence, 7U);
    BOOST_CHECK_EQUAL(data->account->base_available, 200);
    BOOST_CHECK_EQUAL(data->account->b3_available_atoms, 99930);
    BOOST_CHECK_EQUAL(data->account->b3_reserved_atoms, 70);
    BOOST_REQUIRE_EQUAL(data->account->curves.size(), 1U);
    BOOST_CHECK_EQUAL(data->account->curves.front().points.front().price, 7);
    BOOST_CHECK(!data->snapshot.running); // Certificate proves no endpoint availability.
    BOOST_CHECK(!data->history.available);
    data->account->b3_available_atoms = 900000; // Untrusted adjacent endpoint row.
    const auto independently_derived{flowmesh::ClientMarketData(f.pins, *verified, f.account, {}, error)};
    BOOST_REQUIRE(independently_derived && independently_derived->account);
    BOOST_CHECK_EQUAL(independently_derived->account->b3_available_atoms, 99930);
}

BOOST_AUTO_TEST_CASE(state_tampering_trailing_data_wrong_vault_and_oversize_fail)
{
    EvidenceFixture f;
    const auto original{f.Evidence()};
    std::string error;
    auto changed{original};
    changed.state_bytes[0] ^= 1;
    BOOST_CHECK(!f.Verify(changed, error));
    changed = original;
    changed.state_bytes.push_back(0);
    BOOST_CHECK(!f.Verify(changed, error));
    changed.state_bytes.clear();
    BOOST_CHECK(!f.Verify(changed, error));
    changed.state_bytes.resize(flowmesh::CLIENT_STATE_MAX_BYTES + 1);
    BOOST_CHECK(!f.Verify(changed, error));
    changed = original;
    auto other{f.state};
    BOOST_REQUIRE(flowmesh::test_only::StateFunding::Fund(other, f.account, f.pins.base_asset, 1));
    changed.state_bytes = *flowmesh::EncodeClientState(other, error);
    BOOST_CHECK(!f.Verify(changed, error));
}

BOOST_AUTO_TEST_CASE(missing_forged_and_below_quorum_certificates_fail)
{
    EvidenceFixture f;
    const auto original{f.Evidence()};
    std::string error;
    auto changed{original};
    changed.certified_payload.clear();
    BOOST_CHECK(!f.Verify(changed, error));
    changed = original;
    changed.certified_payload.back() ^= 1;
    BOOST_CHECK(!f.Verify(changed, error));
    changed = original;
    changed.certified_payload.push_back(0);
    BOOST_CHECK(!f.Verify(changed, error));
    auto certificate{f.certified};
    certificate.certificate.signer_bitmap = {0x03};
    changed.certified_payload = *flowmesh::EncodeProductionCertifiedPayload(certificate, 4);
    BOOST_CHECK(!f.Verify(changed, error));
}

BOOST_AUTO_TEST_CASE(domain_market_config_and_anchor_authority_are_pinned)
{
    EvidenceFixture f;
    std::string error;
    const auto evidence{f.Evidence()};
    for (size_t i{0}; i < 5; ++i) {
        auto pins{f.pins};
        std::array<uint256*, 5> fields{&pins.domain, &pins.market_id, &pins.base_asset,
                                     &pins.vault_id, &pins.execution_config_id};
        *fields[i] = Filled(88);
        BOOST_CHECK(!flowmesh::VerifyClientStateEvidence(pins, evidence, f.seats,
            [](const auto&) { return true; }, error));
    }
    BOOST_CHECK(!flowmesh::VerifyClientStateEvidence(f.pins, evidence, f.seats,
        [](const auto&) { return false; }, error));
    BOOST_CHECK(!flowmesh::VerifyClientStateEvidence(f.pins, evidence, f.seats, {}, error));
    auto seats{f.seats};
    ++seats.epoch;
    BOOST_CHECK(!flowmesh::VerifyClientStateEvidence(f.pins, evidence, seats,
        [](const auto&) { return true; }, error));
}

BOOST_AUTO_TEST_CASE(event_cursor_expiry_restart_and_filters_never_silently_skip)
{
    flowmesh::ClientEventLog log{Filled(1)};
    const auto start{log.Cursor()};
    for (size_t i{0}; i < flowmesh::CLIENT_EVENT_CAPACITY + 3; ++i) {
        flowmesh::ClientEvent event;
        event.market_id = Filled(i % 2 ? 2 : 3);
        event.account_id = Filled(4);
        log.Append(event);
    }
    const auto expired{log.Read(start, {}, {})};
    BOOST_CHECK(expired.gap);
    BOOST_CHECK(expired.events.empty());
    BOOST_CHECK_EQUAL(expired.oldest_event_id, 4U);
    flowmesh::ClientEventCursor cursor{Filled(1), 3};
    size_t count{0};
    do {
        const auto page{log.Read(cursor, Filled(2), Filled(4), 17)};
        BOOST_CHECK(!page.gap);
        BOOST_REQUIRE_LE(page.events.size(), 17U);
        for (const auto& event : page.events) BOOST_CHECK(event.market_id == Filled(2));
        count += page.events.size();
        BOOST_CHECK_GE(page.cursor.event_id, cursor.event_id);
        cursor = page.cursor;
        if (!page.more) break;
    } while (true);
    BOOST_CHECK_EQUAL(count, 1024U);
    log.Reset(Filled(5));
    BOOST_CHECK(log.Read(cursor, {}, {}).gap);
    BOOST_CHECK(log.Read({}, {}, {}, flowmesh::CLIENT_EVENT_PAGE_MAX + 1).gap);
}

BOOST_AUTO_TEST_CASE(action_status_does_not_regress_when_queue_observation_arrives_late)
{
    flowmesh::ClientEventLog log{Filled(1)};
    flowmesh::ClientEvent event;
    event.market_id = Filled(2);
    event.action_id = Filled(3);
    event.kind = flowmesh::ClientEventKind::POOL_ADMITTED;
    log.Append(event);
    event.kind = flowmesh::ClientEventKind::QUEUE_ADMITTED;
    log.Append(event);
    BOOST_REQUIRE(log.ActionStatus(event.market_id, event.action_id));
    BOOST_CHECK(log.ActionStatus(event.market_id, event.action_id)->kind == flowmesh::ClientEventKind::POOL_ADMITTED);
    event.kind = flowmesh::ClientEventKind::CERTIFIED_INCLUDED;
    log.Append(event);
    event.kind = flowmesh::ClientEventKind::POOL_REFUSED;
    log.Append(event);
    BOOST_CHECK(log.ActionStatus(event.market_id, event.action_id)->kind == flowmesh::ClientEventKind::CERTIFIED_INCLUDED);
    BOOST_CHECK(!log.ActionStatus(event.market_id, Filled(9)));
}

BOOST_AUTO_TEST_CASE(runtime_pool_events_atomic_certified_snapshot_and_restart_unknown)
{
    EvidenceFixture f;
    EvidenceChain chain{f};
    NoSeatKeys keys;
    node::SteadyFlowMeshRuntimeClock clock;
    node::FlowMeshProductionStore store{DBParams{.path = m_path_root / "client-evidence", .cache_bytes = 1 << 20, .memory_only = true}};
    std::string error;
    BOOST_REQUIRE(store.OpenForMarket(f.pins.domain, f.pins.market_id, f.seats, f.state.Root(), error));
    node::FlowMeshRuntimeConfig config;
    config.chain = &chain;
    config.keys = &keys;
    config.clock = &clock;
    config.relay = [](node::FlowMeshRuntimeRelay) { return node::FlowMeshRelayResult{}; };
    node::FlowMeshRuntimeMarketConfig market{f.pins.domain, f.pins.market_id, Filled(9), f.seats, f.state};
    market.store = &store;
    node::FlowMeshRuntime runtime{config, {market}};
    BOOST_REQUIRE_MESSAGE(runtime.Start(error), error);
    BOOST_CHECK(!runtime.ClientSnapshot(f.pins.market_id, error));
    flowmesh::Action action;
    action.signer = f.account;
    action.sequence = 7;
    action.type = static_cast<uint8_t>(flowmesh::ActionType::CANCEL_BID);
    BOOST_REQUIRE(flowmesh::SignAction(f.account_key, f.pins.domain, f.pins.execution_config_id, action));
    BOOST_REQUIRE(runtime.SubmitLocalAction(f.pins.market_id, action) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{5}));
    const auto admitted{runtime.ClientActionStatus(f.pins.market_id, action.Id())};
    BOOST_REQUIRE(admitted);
    BOOST_CHECK(admitted->kind == flowmesh::ClientEventKind::POOL_ADMITTED);
    BOOST_CHECK(!admitted->signed_action_hash.IsNull());
    auto bad{action};
    ++bad.sequence;
    BOOST_REQUIRE(runtime.SubmitLocalAction(f.pins.market_id, bad) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{5}));
    const auto refused{runtime.ClientActionStatus(f.pins.market_id, bad.Id())};
    BOOST_REQUIRE(refused);
    BOOST_CHECK(refused->kind == flowmesh::ClientEventKind::POOL_REFUSED);

    flowmesh::ProductionEntryCheck check;
    flowmesh::ProductionEpochGate gate{f.pins.domain, f.pins.market_id, f.seats};
    const std::array<flowmesh::Action, 1> actions{action};
    const auto built{flowmesh::BuildProductionExecutionEntry(f.state, f.pins.domain, f.pins.market_id,
        f.seats, gate, 0, 0, {}, f.anchor, {130, {}, &chain}, Filled(9), actions, nullptr, check)};
    BOOST_REQUIRE(built);
    const flowmesh::ProductionCertifiedEnvelope certified{built->entry, f.Certify(built->entry)};
    const auto payload{flowmesh::EncodeProductionCertifiedPayload(certified, 4)};
    BOOST_REQUIRE(payload);
    BOOST_REQUIRE(runtime.EnqueueWireMessage(7, {flowmesh::WireMessageKind::CERTIFICATE,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.pins.market_id, 0, 0}, *payload}) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{5}));
    const auto snapshot{runtime.ClientSnapshot(f.pins.market_id, error)};
    BOOST_REQUIRE_MESSAGE(snapshot, error);
    const auto verified{f.Verify(*snapshot, error)};
    BOOST_REQUIRE_MESSAGE(verified, error);
    BOOST_CHECK(verified->state.Root() == built->next_state.Root());
    BOOST_CHECK(snapshot->certified_payload == *payload);
    BOOST_CHECK(runtime.ClientActionStatus(f.pins.market_id, action.Id())->kind == flowmesh::ClientEventKind::CERTIFIED_INCLUDED);
    BOOST_CHECK(runtime.ClientCertifiedEntry(f.pins.market_id, 0, error) == payload);
    const auto cursor{snapshot->cursor};
    runtime.Stop();
    market.state = built->next_state;
    market.next_sequence = 1;
    market.next_effect_index = built->entry.effect_count;
    market.last_microblock_hash = built->entry.GetHash();
    node::FlowMeshRuntime restarted{config, {market}};
    BOOST_REQUIRE_MESSAGE(restarted.Start(error), error);
    const auto restored{restarted.ClientSnapshot(f.pins.market_id, error)};
    BOOST_REQUIRE(restored);
    BOOST_CHECK(restored->certified_payload == *payload);
    BOOST_CHECK(restarted.ClientEvents(cursor, f.pins.market_id, f.account).gap);
    BOOST_CHECK(!restarted.ClientActionStatus(f.pins.market_id, action.Id()));
    restarted.Stop();
}

BOOST_AUTO_TEST_SUITE_END()
