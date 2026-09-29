// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/auth.h>
#include <flowmesh/client_evidence.h>
#include <node/flowmesh_runtime.h>
#include <random.h>
#include <test/util/flowmesh.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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

using WaitClock = std::chrono::steady_clock;
using namespace std::chrono_literals;

flowmesh::ClientEvent ActionEvent(unsigned char market, unsigned char action, flowmesh::ClientEventKind kind)
{
    flowmesh::ClientEvent event;
    event.market_id = Filled(market);
    event.action_id = Filled(action);
    event.kind = kind;
    return event;
}

/** The wait's interrupt predicate doubles as a probe: its first evaluation
 * happens under the log mutex before the waiter sleeps, so any Append after
 * AwaitEvaluations(1) is observed as a wakeup, not as the initial status. */
struct WaitProbe {
    std::atomic<size_t> evaluations{0};
    std::atomic<bool> interrupt{false};
    std::function<bool()> Predicate()
    {
        return [this] { ++evaluations; return interrupt.load(); };
    }
    void AwaitEvaluations(size_t count)
    {
        const auto deadline{WaitClock::now() + 5s};
        while (evaluations.load() < count && WaitClock::now() < deadline) std::this_thread::sleep_for(1ms);
        BOOST_REQUIRE_GE(evaluations.load(), count);
    }
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

BOOST_AUTO_TEST_CASE(action_wait_returns_on_certified_inclusion_without_polling)
{
    using Kind = flowmesh::ClientEventKind;
    flowmesh::ClientEventLog log{Filled(1)};
    log.Append(ActionEvent(2, 3, Kind::QUEUE_ADMITTED));
    WaitProbe probe;
    const auto start{WaitClock::now()};
    auto waiter{std::async(std::launch::async, [&] {
        return log.WaitActionStatus(Filled(2), Filled(3), start + 5s, probe.Predicate());
    })};
    probe.AwaitEvaluations(1);
    std::this_thread::sleep_for(50ms);
    log.Append(ActionEvent(2, 3, Kind::POOL_ADMITTED)); // Relevant but nonterminal.
    // A commit appends its head and inclusion events in one batch.
    flowmesh::ClientEvent head;
    head.market_id = Filled(2);
    head.kind = Kind::CERTIFIED_HEAD;
    std::vector<flowmesh::ClientEvent> committed{head, ActionEvent(2, 3, Kind::CERTIFIED_INCLUDED)};
    log.Append(std::move(committed));
    const auto result{waiter.get()};
    BOOST_CHECK(result.result == flowmesh::ClientWaitResult::TERMINAL);
    BOOST_REQUIRE(result.status);
    BOOST_CHECK(result.status->kind == Kind::CERTIFIED_INCLUDED);
    BOOST_CHECK_EQUAL(result.status->event_id, 4U);
    BOOST_CHECK(WaitClock::now() - start < 2s);
    BOOST_CHECK_EQUAL(std::string{flowmesh::ClientWaitResultName(result.result)}, "terminal");
    // The batch keeps event order and consecutive, resumable ids.
    const auto page{log.Read(flowmesh::ClientEventCursor{Filled(1), 0}, {}, {})};
    BOOST_REQUIRE_EQUAL(page.events.size(), 4U);
    BOOST_CHECK(page.events[2].kind == Kind::CERTIFIED_HEAD && page.events[2].event_id == 3);
    BOOST_CHECK(page.events[3].kind == Kind::CERTIFIED_INCLUDED && page.events[3].event_id == 4);
}

BOOST_AUTO_TEST_CASE(action_wait_times_out_with_current_nonterminal_status)
{
    flowmesh::ClientEventLog log{Filled(1)};
    log.Append(ActionEvent(2, 3, flowmesh::ClientEventKind::POOL_ADMITTED));
    WaitProbe probe;
    const auto start{WaitClock::now()};
    const auto result{log.WaitActionStatus(Filled(2), Filled(3), start + 100ms, probe.Predicate())};
    BOOST_CHECK(WaitClock::now() - start >= 100ms);
    BOOST_CHECK(result.result == flowmesh::ClientWaitResult::TIMEOUT);
    BOOST_REQUIRE(result.status);
    BOOST_CHECK(result.status->kind == flowmesh::ClientEventKind::POOL_ADMITTED);
    // A never-recorded action is unknown, never a refusal, after its wait.
    const auto unknown{log.WaitActionStatus(Filled(2), Filled(9), WaitClock::now() + 20ms, {})};
    BOOST_CHECK(unknown.result == flowmesh::ClientWaitResult::TIMEOUT);
    BOOST_CHECK(!unknown.status);
}

BOOST_AUTO_TEST_CASE(action_wait_ignores_unrelated_events)
{
    using Kind = flowmesh::ClientEventKind;
    flowmesh::ClientEventLog log{Filled(1)};
    log.Append(ActionEvent(2, 3, Kind::QUEUE_ADMITTED));
    WaitProbe probe;
    const auto start{WaitClock::now()};
    auto waiter{std::async(std::launch::async, [&] {
        return log.WaitActionStatus(Filled(2), Filled(3), start + 400ms, probe.Predicate());
    })};
    probe.AwaitEvaluations(1);
    // Other actions, and the same ActionId in another market, never end it.
    for (int i{0}; i < 20; ++i) {
        log.Append(ActionEvent(2, 4, Kind::CERTIFIED_INCLUDED));
        log.Append(ActionEvent(5, 3, Kind::CERTIFIED_INCLUDED));
        std::this_thread::sleep_for(2ms);
    }
    const auto result{waiter.get()};
    BOOST_CHECK(result.result == flowmesh::ClientWaitResult::TIMEOUT);
    BOOST_REQUIRE(result.status);
    BOOST_CHECK(result.status->kind == Kind::QUEUE_ADMITTED);
    BOOST_CHECK(result.status->market_id == Filled(2));
    // It was woken by unrelated appends and went back to sleep.
    BOOST_CHECK_GT(probe.evaluations.load(), 1U);
    BOOST_CHECK(WaitClock::now() - start >= 400ms);
}

BOOST_AUTO_TEST_CASE(action_wait_stale_refusal_then_retry_is_not_terminal_but_fresh_refusal_is)
{
    using Kind = flowmesh::ClientEventKind;
    flowmesh::ClientEventLog log{Filled(1)};
    log.Append(ActionEvent(2, 3, Kind::QUEUE_ADMITTED));
    log.Append(ActionEvent(2, 3, Kind::POOL_REFUSED));
    const auto fresh{log.WaitActionStatus(Filled(2), Filled(3), WaitClock::now() + 5s, {})};
    BOOST_CHECK(fresh.result == flowmesh::ClientWaitResult::TERMINAL);
    BOOST_REQUIRE(fresh.status);
    BOOST_CHECK(fresh.status->kind == Kind::POOL_REFUSED);

    // An exact retry is newer than the old refusal: keep waiting for it.
    log.Append(ActionEvent(2, 3, Kind::QUEUE_ADMITTED));
    const auto stale{log.WaitActionStatus(Filled(2), Filled(3), WaitClock::now() + 100ms, {})};
    BOOST_CHECK(stale.result == flowmesh::ClientWaitResult::TIMEOUT);
    BOOST_REQUIRE(stale.status);
    BOOST_CHECK(stale.status->kind == Kind::POOL_REFUSED);

    // The retry's own refusal is the newest event and ends the wait.
    WaitProbe probe;
    auto waiter{std::async(std::launch::async, [&] {
        return log.WaitActionStatus(Filled(2), Filled(3), WaitClock::now() + 5s, probe.Predicate());
    })};
    probe.AwaitEvaluations(1);
    log.Append(ActionEvent(2, 3, Kind::POOL_REFUSED));
    const auto refused{waiter.get()};
    BOOST_CHECK(refused.result == flowmesh::ClientWaitResult::TERMINAL);
    BOOST_REQUIRE(refused.status);
    BOOST_CHECK(refused.status->kind == Kind::POOL_REFUSED);

    // Admission outranks a later refusal of another attempt: not terminal.
    log.Append(ActionEvent(2, 6, Kind::POOL_ADMITTED));
    log.Append(ActionEvent(2, 6, Kind::POOL_REFUSED));
    const auto admitted{log.WaitActionStatus(Filled(2), Filled(6), WaitClock::now() + 100ms, {})};
    BOOST_CHECK(admitted.result == flowmesh::ClientWaitResult::TIMEOUT);
    BOOST_REQUIRE(admitted.status);
    BOOST_CHECK(admitted.status->kind == Kind::POOL_ADMITTED);
}

BOOST_AUTO_TEST_CASE(action_wait_already_certified_returns_immediately)
{
    using Kind = flowmesh::ClientEventKind;
    flowmesh::ClientEventLog log{Filled(1)};
    log.Append(ActionEvent(2, 3, Kind::CERTIFIED_INCLUDED));
    log.Append(ActionEvent(2, 3, Kind::QUEUE_ADMITTED)); // Late observation.
    WaitProbe probe;
    const auto start{WaitClock::now()};
    const auto result{log.WaitActionStatus(Filled(2), Filled(3), start + 5s, probe.Predicate())};
    BOOST_CHECK(WaitClock::now() - start < 1s);
    BOOST_CHECK(result.result == flowmesh::ClientWaitResult::TERMINAL);
    BOOST_REQUIRE(result.status);
    BOOST_CHECK(result.status->kind == Kind::CERTIFIED_INCLUDED);
    BOOST_CHECK_EQUAL(probe.evaluations.load(), 0U);
    const auto null_action{log.WaitActionStatus(Filled(2), uint256{}, WaitClock::now() + 5s, {})};
    BOOST_CHECK(!null_action.status);
    BOOST_CHECK(WaitClock::now() - start < 1s);
}

namespace {
flowmesh::ClientActionNotice Notice(unsigned char market, unsigned char action, flowmesh::ActionReasonCode code)
{
    flowmesh::ClientActionNotice notice;
    notice.market_id = Filled(market);
    notice.action_id = Filled(action);
    notice.code = code;
    return notice;
}
} // namespace

BOOST_AUTO_TEST_CASE(notices_do_not_change_v1_pages_ids_or_gaps)
{
    using Kind = flowmesh::ClientEventKind;
    flowmesh::ClientEventLog with{Filled(1)};
    flowmesh::ClientEventLog without{Filled(1)};
    for (auto* log : {&with, &without}) {
        log->Append(ActionEvent(2, 3, Kind::QUEUE_ADMITTED));
        log->Append(ActionEvent(2, 3, Kind::POOL_ADMITTED));
    }
    // A full notice ring and then some: never an event id or ring slot.
    std::vector<flowmesh::ClientActionNotice> notices;
    for (size_t i{0}; i < flowmesh::CLIENT_NOTICE_CAPACITY + 10; ++i) {
        notices.push_back(Notice(2, 3, flowmesh::ActionReasonCode::DROPPED_SEQUENCE_CONSUMED));
    }
    with.AppendNotices(std::move(notices));
    for (auto* log : {&with, &without}) log->Append(ActionEvent(2, 4, Kind::CERTIFIED_INCLUDED));
    const auto a{with.Read(flowmesh::ClientEventCursor{Filled(1), 0}, {}, {})};
    const auto b{without.Read(flowmesh::ClientEventCursor{Filled(1), 0}, {}, {})};
    BOOST_CHECK_EQUAL(a.cursor.event_id, b.cursor.event_id);
    BOOST_CHECK_EQUAL(a.latest_event_id, 3U);
    BOOST_CHECK_EQUAL(a.oldest_event_id, b.oldest_event_id);
    BOOST_CHECK_EQUAL(a.gap, b.gap);
    BOOST_REQUIRE_EQUAL(a.events.size(), b.events.size());
    for (size_t i{0}; i < a.events.size(); ++i) {
        BOOST_CHECK_EQUAL(a.events[i].event_id, b.events[i].event_id);
        BOOST_CHECK(a.events[i].kind == b.events[i].kind);
    }
    BOOST_CHECK(with.ActionStatus(Filled(2), Filled(3))->kind == Kind::POOL_ADMITTED);
    BOOST_CHECK(with.Cursor().event_id == without.Cursor().event_id);
    // The newest notice records the log position it was appended at.
    const auto newest{with.NewestNotice(Filled(2), Filled(3))};
    BOOST_REQUIRE(newest);
    BOOST_CHECK_EQUAL(newest->after_event_id, 2U);
    BOOST_CHECK(!with.NewestNotice(Filled(2), Filled(4)));
    BOOST_CHECK(!with.NewestNotice(Filled(5), Filled(3)));
}

BOOST_AUTO_TEST_CASE(v1_wait_is_not_woken_or_ended_by_notices)
{
    using Kind = flowmesh::ClientEventKind;
    flowmesh::ClientEventLog log{Filled(1)};
    log.Append(ActionEvent(2, 3, Kind::QUEUE_ADMITTED));
    log.AppendNotices({Notice(2, 3, flowmesh::ActionReasonCode::STALE_SEQUENCE)});
    WaitProbe probe;
    const auto start{WaitClock::now()};
    auto waiter{std::async(std::launch::async, [&] {
        return log.WaitActionStatus(Filled(2), Filled(3), start + 300ms, probe.Predicate());
    })};
    probe.AwaitEvaluations(1);
    for (int i{0}; i < 10; ++i) {
        log.AppendNotices({Notice(2, 3, flowmesh::ActionReasonCode::STALE_SEQUENCE)});
        std::this_thread::sleep_for(2ms);
    }
    const auto result{waiter.get()};
    BOOST_CHECK(result.result == flowmesh::ClientWaitResult::TIMEOUT);
    BOOST_REQUIRE(result.status);
    BOOST_CHECK(result.status->kind == Kind::QUEUE_ADMITTED);
    BOOST_CHECK(!result.notice);
    // No notice waiter existed, so no notice woke it: one evaluation before
    // sleeping, one when the deadline passed.
    BOOST_CHECK_LE(probe.evaluations.load(), 2U);
    BOOST_CHECK(WaitClock::now() - start >= 300ms);
}

BOOST_AUTO_TEST_CASE(notice_wait_terminal_only_when_newer_than_events)
{
    using Kind = flowmesh::ClientEventKind;
    using Code = flowmesh::ActionReasonCode;
    flowmesh::ClientEventLog log{Filled(1)};
    // An older refusal notice followed by an exact retry's admission is not
    // terminal: the notice is older than the newest event.
    log.AppendNotices({Notice(2, 3, Code::STALE_SEQUENCE)});
    log.Append(ActionEvent(2, 3, Kind::QUEUE_ADMITTED));
    const auto older{log.WaitActionStatus(Filled(2), Filled(3), WaitClock::now() + 50ms, {}, true)};
    BOOST_CHECK(older.result == flowmesh::ClientWaitResult::TIMEOUT);
    BOOST_REQUIRE(older.notice);
    // A new notice arriving during the wait wakes and ends it.
    WaitProbe probe;
    auto waiter{std::async(std::launch::async, [&] {
        return log.WaitActionStatus(Filled(2), Filled(3), WaitClock::now() + 5s, probe.Predicate(), true);
    })};
    probe.AwaitEvaluations(1);
    log.AppendNotices({Notice(2, 3, Code::DROPPED_SEQUENCE_CONSUMED)});
    const auto dropped{waiter.get()};
    BOOST_CHECK(dropped.result == flowmesh::ClientWaitResult::TERMINAL);
    BOOST_REQUIRE(dropped.notice);
    BOOST_CHECK(dropped.notice->code == Code::DROPPED_SEQUENCE_CONSUMED);
    BOOST_REQUIRE(dropped.status);
    BOOST_CHECK(dropped.status->kind == Kind::QUEUE_ADMITTED);
    // A notice alone (no event at all) ends it; a non-ending code does not.
    log.AppendNotices({Notice(2, 7, Code::STALE_SEQUENCE_UNATTRIBUTED)});
    BOOST_CHECK(log.WaitActionStatus(Filled(2), Filled(7), WaitClock::now() + 5s, {}, true).result ==
                flowmesh::ClientWaitResult::TERMINAL);
    log.AppendNotices({Notice(2, 8, Code::FAR_FUTURE_SEQUENCE)});
    BOOST_CHECK(log.WaitActionStatus(Filled(2), Filled(8), WaitClock::now() + 50ms, {}, true).result ==
                flowmesh::ClientWaitResult::TIMEOUT);
    // Certified inclusion stays terminal whatever the notices say.
    log.Append(ActionEvent(2, 9, Kind::CERTIFIED_INCLUDED));
    log.AppendNotices({Notice(2, 9, Code::FAR_FUTURE_SEQUENCE)});
    BOOST_CHECK(log.WaitActionStatus(Filled(2), Filled(9), WaitClock::now() + 5s, {}, true).result ==
                flowmesh::ClientWaitResult::TERMINAL);
    // A refusal event appended BEFORE its notice keeps the notice newer.
    log.Append(ActionEvent(2, 10, Kind::POOL_ADMITTED));
    log.Append(ActionEvent(2, 10, Kind::POOL_REFUSED));
    log.AppendNotices({Notice(2, 10, Code::STALE_SEQUENCE)});
    const auto refused{log.WaitActionStatus(Filled(2), Filled(10), WaitClock::now() + 5s, {}, true)};
    BOOST_CHECK(refused.result == flowmesh::ClientWaitResult::TERMINAL);
    BOOST_CHECK(refused.status->kind == Kind::POOL_ADMITTED);
    // The same state without notices keeps the v1 answer (not terminal).
    BOOST_CHECK(log.WaitActionStatus(Filled(2), Filled(10), WaitClock::now() + 50ms, {}).result ==
                flowmesh::ClientWaitResult::TIMEOUT);
}

BOOST_AUTO_TEST_CASE(newest_notice_if_newer_than_every_event)
{
    using Kind = flowmesh::ClientEventKind;
    flowmesh::ClientEventLog log{Filled(1)};
    log.AppendNotices({Notice(2, 3, flowmesh::ActionReasonCode::STALE_SEQUENCE)});
    BOOST_CHECK(log.NewestNoticeIfNewer(Filled(2), Filled(3))); // no event at all
    log.Append(ActionEvent(2, 3, Kind::QUEUE_ADMITTED));
    BOOST_CHECK(!log.NewestNoticeIfNewer(Filled(2), Filled(3))); // an exact retry is newer
    BOOST_CHECK(log.NewestNotice(Filled(2), Filled(3)));
    log.AppendNotices({Notice(2, 3, flowmesh::ActionReasonCode::DROPPED_SEQUENCE_CONSUMED)});
    const auto newer{log.NewestNoticeIfNewer(Filled(2), Filled(3))};
    BOOST_REQUIRE(newer);
    BOOST_CHECK(newer->code == flowmesh::ActionReasonCode::DROPPED_SEQUENCE_CONSUMED);
    BOOST_CHECK(!log.NewestNoticeIfNewer(Filled(2), Filled(4)));
}

BOOST_AUTO_TEST_CASE(notice_ring_bounded_and_reset_with_instance)
{
    flowmesh::ClientEventLog log{Filled(1)};
    std::vector<flowmesh::ClientActionNotice> notices;
    for (size_t i{0}; i < flowmesh::CLIENT_NOTICE_CAPACITY + 1; ++i) {
        notices.push_back(Notice(2, static_cast<unsigned char>(i % 200), flowmesh::ActionReasonCode::STALE_SEQUENCE));
        notices.back().account_sequence = i;
    }
    log.AppendNotices(std::move(notices));
    // The oldest notice was evicted; the newest for action 0 remains.
    const auto newest{log.NewestNotice(Filled(2), Filled(0))};
    BOOST_REQUIRE(newest);
    BOOST_CHECK_EQUAL(newest->account_sequence, 2000U);
    size_t action_zero{0};
    for (size_t i{1}; i <= flowmesh::CLIENT_NOTICE_CAPACITY; ++i) action_zero += i % 200 == 0;
    BOOST_CHECK_GT(action_zero, 0U);
    log.Reset(Filled(9));
    BOOST_CHECK(!log.NewestNotice(Filled(2), Filled(0)));
    BOOST_CHECK(!log.NewestNotice(Filled(2), Filled(1)));
    BOOST_CHECK(flowmesh::ClientNoticeEndsWait(flowmesh::ActionReasonCode::DEPOSIT_ALREADY_CREDITED));
    BOOST_CHECK(!flowmesh::ClientNoticeEndsWait(flowmesh::ActionReasonCode::FAR_FUTURE_SEQUENCE));
    BOOST_CHECK(!flowmesh::ClientNoticeEndsWait(flowmesh::ActionReasonCode::SUBMIT_LANE_FULL));
}

BOOST_AUTO_TEST_CASE(public_notices_never_evict_node_notices)
{
    using Code = flowmesh::ActionReasonCode;
    using Origin = flowmesh::ClientNoticeOrigin;
    flowmesh::ClientEventLog log{Filled(1)};
    auto node{Notice(2, 3, Code::DROPPED_SEQUENCE_CONSUMED)};
    node.origin = Origin::NODE;
    log.AppendNotices({node});
    // Public traffic fills and turns over its own ring several times. It
    // comes from 96 identities (IPv6 /64s) of 64 notices each: more than
    // the 32 whose shares fill the ring, so the ring itself overflows.
    const auto flooded = [](const uint32_t n) {
        uint256 id{Filled(0xee)};
        std::memcpy(id.begin(), &n, sizeof(n));
        return id;
    };
    uint32_t n{0};
    for (int round{0}; round < 3; ++round) {
        std::vector<flowmesh::ClientActionNotice> flood;
        for (size_t i{0}; i < flowmesh::CLIENT_NOTICE_CAPACITY; ++i, ++n) {
            flood.push_back(Notice(2, 0, Code::DEPOSIT_FACTS_UNAVAILABLE));
            flood.back().action_id = flooded(n);
            flood.back().submitter = strprintf("ipv6/64:%016x", n / flowmesh::CLIENT_NOTICE_PER_SUBMITTER);
        }
        log.AppendNotices(std::move(flood));
    }
    BOOST_CHECK(!log.NewestNotice(Filled(2), flooded(0)));
    BOOST_CHECK(!log.NewestNotice(Filled(2), flooded(n - flowmesh::CLIENT_NOTICE_CAPACITY - 1)));
    BOOST_CHECK(log.NewestNotice(Filled(2), flooded(n - flowmesh::CLIENT_NOTICE_CAPACITY)));
    BOOST_CHECK(log.NewestNotice(Filled(2), flooded(n - 1)));
    const auto kept{log.NewestNotice(Filled(2), Filled(3))};
    BOOST_REQUIRE(kept);
    BOOST_CHECK(kept->code == Code::DROPPED_SEQUENCE_CONSUMED);
    BOOST_CHECK(kept->origin == Origin::NODE);
    // The node ring is bounded on its own, oldest first.
    std::vector<flowmesh::ClientActionNotice> own;
    for (size_t i{0}; i < flowmesh::CLIENT_NOTICE_CAPACITY; ++i) {
        own.push_back(Notice(2, 250, Code::STALE_SEQUENCE));
        own.back().origin = Origin::NODE;
    }
    log.AppendNotices(std::move(own));
    BOOST_CHECK(!log.NewestNotice(Filled(2), Filled(3)));
    // The newest notice of an action is the last appended, whichever ring
    // holds it.
    auto first{Notice(2, 9, Code::STALE_SEQUENCE)};
    first.origin = Origin::NODE;
    log.AppendNotices({first, Notice(2, 9, Code::FAR_FUTURE_SEQUENCE)});
    BOOST_CHECK(log.NewestNotice(Filled(2), Filled(9))->code == Code::FAR_FUTURE_SEQUENCE);
    auto last{Notice(2, 9, Code::DROPPED_SEQUENCE_CONSUMED)};
    last.origin = Origin::NODE;
    log.AppendNotices({last});
    BOOST_CHECK(log.NewestNotice(Filled(2), Filled(9))->code == Code::DROPPED_SEQUENCE_CONSUMED);
    BOOST_CHECK(log.NewestNoticeIfNewer(Filled(2), Filled(9))->code == Code::DROPPED_SEQUENCE_CONSUMED);
    // A new instance clears both rings.
    log.Reset(Filled(8));
    BOOST_CHECK(!log.NewestNotice(Filled(2), Filled(9)));
    BOOST_CHECK(!log.NewestNotice(Filled(2), Filled(250)));
    BOOST_CHECK(!log.NewestNotice(Filled(2), flooded(n - 1)));
}

BOOST_AUTO_TEST_CASE(identity_shares_apply_only_once_full)
{
    flowmesh::IdentityShares shares{8, 2};
    uint64_t serial{0};
    // One identity alone uses the whole capacity.
    for (int i{0}; i < 8; ++i) BOOST_CHECK(!shares.Insert(++serial, "a"));
    BOOST_CHECK_EQUAL(shares.Size(), 8U);
    // Full, an identity at or over its share replaces its own oldest.
    BOOST_CHECK_EQUAL(shares.Insert(++serial, "a").value_or(0), 1U);
    // An identity within its share takes the oldest of the one over it...
    BOOST_CHECK_EQUAL(shares.Insert(++serial, "b").value_or(0), 2U);
    BOOST_CHECK_EQUAL(shares.Insert(++serial, "b").value_or(0), 3U);
    // ...and, at its share, replaces its own.
    BOOST_CHECK_EQUAL(shares.Insert(++serial, "b").value_or(0), 10U);
    for (const auto* identity : {"c", "c", "d", "d"}) BOOST_CHECK(shares.Insert(++serial, identity));
    // Four identities at their share (8 / 2): nobody is over it, so a fifth
    // identity's entry takes the oldest overall.
    BOOST_CHECK(!shares.Identity(7));
    BOOST_CHECK_EQUAL(*shares.Identity(8), "a");
    BOOST_CHECK_EQUAL(shares.Insert(++serial, "e").value_or(0), 8U);
    BOOST_CHECK(!shares.Identity(8));
    BOOST_CHECK_EQUAL(shares.Size(), 8U);
    // A forgotten entry frees its place.
    shares.Erase(13);
    shares.Erase(13);
    BOOST_CHECK(!shares.Identity(13));
    BOOST_CHECK_EQUAL(shares.Size(), 7U);
    BOOST_CHECK(!shares.Insert(++serial, "e"));
    BOOST_CHECK_EQUAL(*shares.Identity(serial), "e");
    shares.Clear();
    BOOST_CHECK_EQUAL(shares.Size(), 0U);
    BOOST_CHECK(!shares.Identity(serial));
}

BOOST_AUTO_TEST_CASE(identity_shares_match_their_rules_over_random_use)
{
    // A direct model of the documented rules, against the indexed one:
    // every insert's dropped serial and every recorded identity agree.
    for (const auto& [capacity, share] : {std::pair<size_t, size_t>{8, 2}, {12, 3}, {5, 1}, {16, 16}}) {
        FastRandomContext rng{/*fDeterministic=*/true};
        flowmesh::IdentityShares shares{capacity, share};
        std::map<uint64_t, std::string> model; // oldest first
        uint64_t serial{0};
        const auto held = [&](const std::string& identity) {
            return static_cast<size_t>(std::count_if(model.begin(), model.end(), [&](const auto& e) { return e.second == identity; }));
        };
        for (int step{0}; step < 4000; ++step) {
            if (!model.empty() && rng.randrange(4) == 0) {
                // Forget a recorded serial, the oldest of its identity or not.
                auto it{model.begin()};
                std::advance(it, rng.randrange(model.size()));
                shares.Erase(it->first);
                model.erase(it);
            } else {
                const std::string identity{static_cast<char>('a' + rng.randrange(6))};
                const size_t before{held(identity)};
                model.emplace(++serial, identity);
                std::optional<uint64_t> expected;
                if (model.size() > capacity) {
                    const auto own_oldest{std::find_if(model.begin(), model.end(), [&](const auto& e) { return e.second == identity; })};
                    const auto over{std::find_if(model.begin(), model.end(), [&](const auto& e) { return held(e.second) > share; })};
                    expected = before >= share ? own_oldest->first : over != model.end() ? over->first : model.begin()->first;
                    model.erase(*expected);
                }
                BOOST_CHECK(shares.Insert(serial, identity) == expected);
            }
            BOOST_REQUIRE_EQUAL(shares.Size(), model.size());
        }
        for (uint64_t n{1}; n <= serial; ++n) {
            const auto* identity{shares.Identity(n)};
            const auto it{model.find(n)};
            BOOST_CHECK_EQUAL(identity != nullptr, it != model.end());
            if (identity && it != model.end()) BOOST_CHECK_EQUAL(*identity, it->second);
        }
    }
}

namespace {
//! Notices about distinct actions, for floods of the public ring.
struct NoticeFlood {
    flowmesh::ClientEventLog& log;
    uint32_t next{0};
    static uint256 Id(const uint32_t n)
    {
        uint256 out{Filled(0xee)};
        std::memcpy(out.begin(), &n, sizeof(n));
        return out;
    }
    void operator()(const std::string& submitter, const size_t count)
    {
        std::vector<flowmesh::ClientActionNotice> notices;
        for (size_t i{0}; i < count; ++i) {
            notices.push_back(Notice(2, 0, flowmesh::ActionReasonCode::FAR_FUTURE_SEQUENCE));
            notices.back().action_id = Id(next++);
            notices.back().submitter = submitter;
        }
        log.AppendNotices(std::move(notices));
    }
    bool Kept(const uint32_t n) const { return log.NewestNotice(Filled(2), Id(n)).has_value(); }
};
} // namespace

BOOST_AUTO_TEST_CASE(one_identity_alone_keeps_a_whole_public_notice_ring)
{
    using Code = flowmesh::ActionReasonCode;
    constexpr size_t CAPACITY{flowmesh::CLIENT_NOTICE_CAPACITY};
    constexpr size_t SHARE{flowmesh::CLIENT_NOTICE_PER_SUBMITTER};
    flowmesh::ClientEventLog log{Filled(1)};
    NoticeFlood flood{log};
    // One client alone, or every client behind one address, /64 or load
    // balancer, keeps a whole ring of notices.
    auto proof{Notice(2, 3, Code::DROPPED_SEQUENCE_CONSUMED)};
    proof.submitter = "198.51.100.7";
    log.AppendNotices({proof});
    flood("198.51.100.7", CAPACITY - 1);
    BOOST_CHECK(log.NewestNotice(Filled(2), Filled(3)));
    BOOST_CHECK(flood.Kept(0));
    // Full, its own oldest makes room for its next.
    flood("198.51.100.7", 1);
    BOOST_CHECK(!log.NewestNotice(Filled(2), Filled(3)));
    BOOST_CHECK(flood.Kept(0));
    // Another identity's first notices, up to its share, take the full
    // identity's oldest; past its share, it replaces its own.
    flood("203.0.113.9", SHARE);
    BOOST_CHECK(!flood.Kept(SHARE - 1));
    BOOST_CHECK(flood.Kept(SHARE));
    const uint32_t other_first{static_cast<uint32_t>(CAPACITY)};
    BOOST_CHECK(flood.Kept(other_first));
    flood("203.0.113.9", 1);
    BOOST_CHECK(!flood.Kept(other_first));
    BOOST_CHECK(flood.Kept(SHARE));
    BOOST_CHECK(flood.Kept(flood.next - 1));
}

BOOST_AUTO_TEST_CASE(one_identity_never_turns_over_the_public_notice_ring)
{
    using Code = flowmesh::ActionReasonCode;
    constexpr size_t CAPACITY{flowmesh::CLIENT_NOTICE_CAPACITY};
    constexpr size_t SHARE{flowmesh::CLIENT_NOTICE_PER_SUBMITTER};
    flowmesh::ClientEventLog log{Filled(1)};
    NoticeFlood flood{log};
    // A remote client's not-executed proof.
    auto proof{Notice(2, 3, Code::DROPPED_SEQUENCE_CONSUMED)};
    proof.submitter = "198.51.100.7";
    log.AppendNotices({proof});

    // One identity sends three whole rings. It fills the ring, then only
    // replaces its own notices: it keeps its newest CAPACITY - 1.
    flood("203.0.113.9", 3 * CAPACITY);
    BOOST_CHECK(log.NewestNotice(Filled(2), Filled(3)));
    BOOST_CHECK(!flood.Kept(2 * CAPACITY));
    BOOST_CHECK(flood.Kept(2 * CAPACITY + 1));
    BOOST_CHECK(flood.Kept(3 * CAPACITY - 1));
    // Notices of unknown identity count as one identity; so does each IPv6
    // /64. Each takes the flooding identity's oldest while within its share,
    // then replaces its own.
    flood("", CAPACITY);
    const uint32_t first_ipv6{flood.next};
    for (int prefix{0}; prefix < 29; ++prefix) flood(strprintf("ipv6/64:20010db8%08x", prefix), 2 * SHARE);
    const auto still{log.NewestNotice(Filled(2), Filled(3))};
    BOOST_REQUIRE(still);
    BOOST_CHECK(still->code == Code::DROPPED_SEQUENCE_CONSUMED);
    BOOST_CHECK_EQUAL(still->submitter, "198.51.100.7");
    BOOST_CHECK(!flood.Kept(first_ipv6 + SHARE - 1));
    BOOST_CHECK(flood.Kept(first_ipv6 + SHARE));
    BOOST_CHECK(flood.Kept(3 * CAPACITY - 1));

    // The ring stays bounded. The flooding identity is left with its share
    // after 63 more notices of a 32nd other identity; with every identity
    // then within its share, the next notice takes the oldest overall, here
    // the proof.
    const uint32_t before_more{flood.next};
    const std::string last{"ipv6/64:20010db900000000"};
    flood(last, SHARE - 1);
    BOOST_CHECK(log.NewestNotice(Filled(2), Filled(3)));
    BOOST_CHECK(flood.Kept(3 * CAPACITY - SHARE));
    BOOST_CHECK(!flood.Kept(3 * CAPACITY - SHARE - 1));
    flood(last, 1);
    BOOST_CHECK(!log.NewestNotice(Filled(2), Filled(3)));
    BOOST_CHECK(flood.Kept(before_more));
    BOOST_CHECK(flood.Kept(flood.next - 1));
    // A new instance clears the ring and every share.
    log.Reset(Filled(8));
    BOOST_CHECK(!flood.Kept(flood.next - 1));
    log.AppendNotices({proof});
    flood("198.51.100.7", CAPACITY - 1);
    BOOST_CHECK(log.NewestNotice(Filled(2), Filled(3)));
}

BOOST_AUTO_TEST_CASE(action_wait_interrupt_and_reset_wake_waiters)
{
    using Kind = flowmesh::ClientEventKind;
    flowmesh::ClientEventLog log{Filled(1)};
    log.Append(ActionEvent(2, 3, Kind::QUEUE_ADMITTED));
    {
        WaitProbe probe;
        const auto start{WaitClock::now()};
        auto waiter{std::async(std::launch::async, [&] {
            return log.WaitActionStatus(Filled(2), Filled(3), start + 5s, probe.Predicate());
        })};
        probe.AwaitEvaluations(1);
        log.WakeWaiters();
        const auto result{waiter.get()};
        BOOST_CHECK(result.result == flowmesh::ClientWaitResult::INTERRUPTED);
        BOOST_REQUIRE(result.status);
        BOOST_CHECK(result.status->kind == Kind::QUEUE_ADMITTED);
        BOOST_CHECK(WaitClock::now() - start < 2s);
    }
    {
        // An earlier wakeup does not interrupt a later wait.
        const auto later{log.WaitActionStatus(Filled(2), Filled(3), WaitClock::now() + 50ms, {})};
        BOOST_CHECK(later.result == flowmesh::ClientWaitResult::TIMEOUT);
    }
    {
        // A caller interrupted before it sleeps returns at once.
        WaitProbe probe;
        probe.interrupt = true;
        const auto start{WaitClock::now()};
        const auto result{log.WaitActionStatus(Filled(2), Filled(3), start + 5s, probe.Predicate())};
        BOOST_CHECK(result.result == flowmesh::ClientWaitResult::INTERRUPTED);
        BOOST_CHECK(WaitClock::now() - start < 1s);
    }
    {
        WaitProbe probe;
        const auto start{WaitClock::now()};
        auto waiter{std::async(std::launch::async, [&] {
            return log.WaitActionStatus(Filled(2), Filled(3), start + 5s, probe.Predicate());
        })};
        probe.AwaitEvaluations(1);
        log.Reset(Filled(7));
        const auto result{waiter.get()};
        BOOST_CHECK(result.result == flowmesh::ClientWaitResult::RESTARTED);
        BOOST_CHECK(!result.status); // The new instance has not seen the action.
        BOOST_CHECK(WaitClock::now() - start < 2s);
    }
}

namespace {
flowmesh::ClientEvent AccountEvent(unsigned char market, unsigned char account, flowmesh::ClientEventKind kind)
{
    flowmesh::ClientEvent event;
    event.market_id = Filled(market);
    event.account_id = Filled(account);
    event.action_id = Filled(static_cast<unsigned char>(account + 100));
    event.kind = kind;
    return event;
}
} // namespace

BOOST_AUTO_TEST_CASE(event_watch_returns_at_once_for_pending_events_and_gaps)
{
    using Kind = flowmesh::ClientEventKind;
    using Watch = flowmesh::ClientWatchResult;
    flowmesh::ClientEventLog log{Filled(1)};
    log.Append(AccountEvent(2, 4, Kind::POOL_ADMITTED));
    const auto current{log.Cursor()};
    const auto start{WaitClock::now()};
    // Nothing new after the current cursor: a watch has to wait.
    BOOST_CHECK(log.EventsPending(current, Filled(2), Filled(4)) == Watch::TIMEOUT);
    // Another instance, a cursor in the future: gaps, answered at once.
    BOOST_CHECK(log.WaitEvents({Filled(9), 1}, Filled(2), Filled(4), start + 5s, {}) == Watch::GAP);
    BOOST_CHECK(log.WaitEvents({Filled(1), 7}, Filled(2), Filled(4), start + 5s, {}) == Watch::GAP);
    // An event after the cursor that the filter keeps.
    BOOST_CHECK(log.WaitEvents({Filled(1), 0}, Filled(2), Filled(4), start + 5s, {}) == Watch::READY);
    // ... but not one the filter drops.
    BOOST_CHECK(log.EventsPending({Filled(1), 0}, Filled(2), Filled(5)) == Watch::TIMEOUT);
    BOOST_CHECK(log.EventsPending({Filled(1), 0}, Filled(3), std::nullopt) == Watch::TIMEOUT);
    // A certified head is relevant to every account of its market.
    log.Append(AccountEvent(2, 0, Kind::CERTIFIED_HEAD));
    BOOST_CHECK(log.WaitEvents(current, Filled(2), Filled(5), start + 5s, {}) == Watch::READY);
    // An evicted cursor.
    for (size_t i{0}; i < flowmesh::CLIENT_EVENT_CAPACITY; ++i) log.Append(AccountEvent(3, 4, Kind::POOL_ADMITTED));
    BOOST_CHECK(log.WaitEvents(current, Filled(2), Filled(5), start + 5s, {}) == Watch::GAP);
    BOOST_CHECK(WaitClock::now() - start < 1s);
    BOOST_CHECK_EQUAL(log.WatchersForTest(), 0U);
    BOOST_CHECK_EQUAL(std::string{flowmesh::ClientWatchResultName(Watch::READY)}, "ready");
    BOOST_CHECK_EQUAL(std::string{flowmesh::ClientWatchResultName(Watch::BUSY)}, "busy");
}

BOOST_AUTO_TEST_CASE(event_watch_pending_agrees_with_read)
{
    using Kind = flowmesh::ClientEventKind;
    using Watch = flowmesh::ClientWatchResult;
    FastRandomContext rng{/*fDeterministic=*/true};
    constexpr std::array KINDS{Kind::QUEUE_ADMITTED, Kind::POOL_ADMITTED, Kind::POOL_REFUSED,
                               Kind::CERTIFIED_INCLUDED, Kind::CERTIFIED_HEAD};
    for (int round{0}; round < 40; ++round) {
        flowmesh::ClientEventLog log{Filled(1)};
        const auto events{rng.randrange(flowmesh::CLIENT_EVENT_CAPACITY + 40)};
        for (uint64_t i{0}; i < events; ++i) {
            log.Append(AccountEvent(static_cast<unsigned char>(2 + rng.randrange(2)),
                                    static_cast<unsigned char>(4 + rng.randrange(2)), KINDS[rng.randrange(KINDS.size())]));
        }
        const auto last{log.Cursor().event_id};
        for (int probe{0}; probe < 60; ++probe) {
            const flowmesh::ClientEventCursor cursor{rng.randrange(8) == 0 ? Filled(9) : Filled(1),
                                                     rng.randrange(last + 3)};
            const std::optional<uint256> market{rng.randbool() ? std::optional{Filled(static_cast<unsigned char>(2 + rng.randrange(3)))} : std::nullopt};
            const std::optional<uint256> account{rng.randbool() ? std::optional{Filled(static_cast<unsigned char>(4 + rng.randrange(3)))} : std::nullopt};
            const auto page{log.Read(cursor, market, account)};
            const auto pending{log.EventsPending(cursor, market, account)};
            BOOST_CHECK_EQUAL(pending == Watch::GAP, page.gap);
            BOOST_CHECK_EQUAL(pending == Watch::READY, !page.gap && !page.events.empty());
            BOOST_CHECK_EQUAL(pending == Watch::TIMEOUT, !page.gap && page.events.empty());
        }
    }
}

BOOST_AUTO_TEST_CASE(event_watch_wakes_only_on_relevant_appends)
{
    using Kind = flowmesh::ClientEventKind;
    for (const bool head : {true, false}) {
        flowmesh::ClientEventLog log{Filled(1)};
        log.Append(AccountEvent(2, 4, Kind::POOL_ADMITTED));
        const auto cursor{log.Cursor()};
        WaitProbe probe;
        const auto start{WaitClock::now()};
        auto watcher{std::async(std::launch::async, [&] {
            return log.WaitEvents(cursor, Filled(2), Filled(4), start + 5s, probe.Predicate());
        })};
        probe.AwaitEvaluations(1);
        // Another market's events, and another account's non-head events in
        // this market, neither end nor even wake it.
        for (int i{0}; i < 20; ++i) {
            log.Append(AccountEvent(3, 4, Kind::CERTIFIED_HEAD));
            log.Append(std::vector{AccountEvent(2, 5, Kind::POOL_ADMITTED), AccountEvent(2, 5, Kind::CERTIFIED_INCLUDED)});
        }
        std::this_thread::sleep_for(50ms);
        BOOST_CHECK(watcher.wait_for(0s) == std::future_status::timeout);
        BOOST_CHECK_LE(probe.evaluations.load(), 2U); // At most one spurious wakeup.
        BOOST_CHECK_EQUAL(log.WatchersForTest(), 1U);
        // Its market's certified head, or its own account's event, does.
        if (head) log.Append(AccountEvent(2, 0, Kind::CERTIFIED_HEAD));
        else log.Append(AccountEvent(2, 4, Kind::CERTIFIED_INCLUDED));
        BOOST_CHECK(watcher.get() == flowmesh::ClientWatchResult::READY);
        BOOST_CHECK(WaitClock::now() - start < 3s);
        BOOST_CHECK_EQUAL(log.WatchersForTest(), 0U);
        // The page it now reads holds exactly that event.
        const auto page{log.Read(cursor, Filled(2), Filled(4))};
        BOOST_REQUIRE_EQUAL(page.events.size(), 1U);
        BOOST_CHECK(page.events[0].kind == (head ? Kind::CERTIFIED_HEAD : Kind::CERTIFIED_INCLUDED));
    }
}

BOOST_AUTO_TEST_CASE(event_watch_detects_cursor_eviction_while_waiting)
{
    using Kind = flowmesh::ClientEventKind;
    flowmesh::ClientEventLog log{Filled(1)};
    log.Append(AccountEvent(2, 4, Kind::POOL_ADMITTED));
    const auto cursor{log.Cursor()};
    WaitProbe probe;
    auto watcher{std::async(std::launch::async, [&] {
        return log.WaitEvents(cursor, Filled(2), Filled(4), WaitClock::now() + 5s, probe.Predicate());
    })};
    probe.AwaitEvaluations(1);
    // Unrelated events only, until the ring no longer retains the cursor.
    for (size_t i{0}; i < flowmesh::CLIENT_EVENT_CAPACITY; ++i) log.Append(AccountEvent(3, 4, Kind::POOL_ADMITTED));
    BOOST_CHECK(watcher.wait_for(50ms) == std::future_status::timeout);
    log.Append(AccountEvent(3, 4, Kind::POOL_ADMITTED));
    BOOST_CHECK(watcher.get() == flowmesh::ClientWatchResult::GAP);
    BOOST_CHECK(log.Read(cursor, Filled(2), Filled(4)).gap);
    // A single batch larger than the ring, too.
    const auto second{log.Cursor()};
    auto batched{std::async(std::launch::async, [&] {
        return log.WaitEvents(second, Filled(2), Filled(4), WaitClock::now() + 5s, {});
    })};
    while (log.WatchersForTest() == 0) std::this_thread::sleep_for(1ms);
    log.Append(std::vector<flowmesh::ClientEvent>(flowmesh::CLIENT_EVENT_CAPACITY + 2, AccountEvent(3, 4, Kind::POOL_ADMITTED)));
    BOOST_CHECK(batched.get() == flowmesh::ClientWatchResult::GAP);
}

BOOST_AUTO_TEST_CASE(event_watch_times_out_without_events)
{
    flowmesh::ClientEventLog log{Filled(1)};
    const auto start{WaitClock::now()};
    BOOST_CHECK(log.WaitEvents(log.Cursor(), Filled(2), std::nullopt, start + 100ms, {}) == flowmesh::ClientWatchResult::TIMEOUT);
    BOOST_CHECK(WaitClock::now() - start >= 100ms);
    BOOST_CHECK(WaitClock::now() - start < 2s);
    BOOST_CHECK_EQUAL(log.WatchersForTest(), 0U);
    // A deadline already passed returns without registering.
    BOOST_CHECK(log.WaitEvents(log.Cursor(), Filled(2), std::nullopt, start, {}) == flowmesh::ClientWatchResult::TIMEOUT);
}

BOOST_AUTO_TEST_CASE(event_watch_interrupt_wakewaiters_and_reset_end_waits)
{
    using Watch = flowmesh::ClientWatchResult;
    flowmesh::ClientEventLog log{Filled(1)};
    {
        WaitProbe probe;
        const auto start{WaitClock::now()};
        auto watcher{std::async(std::launch::async, [&] {
            return log.WaitEvents(log.Cursor(), Filled(2), std::nullopt, start + 5s, probe.Predicate());
        })};
        probe.AwaitEvaluations(1);
        log.WakeWaiters();
        BOOST_CHECK(watcher.get() == Watch::INTERRUPTED);
        BOOST_CHECK(WaitClock::now() - start < 2s);
    }
    {
        // An earlier wakeup does not end a later watch.
        BOOST_CHECK(log.WaitEvents(log.Cursor(), Filled(2), std::nullopt, WaitClock::now() + 50ms, {}) == Watch::TIMEOUT);
        // A caller interrupted before it sleeps returns at once.
        WaitProbe probe;
        probe.interrupt = true;
        const auto start{WaitClock::now()};
        BOOST_CHECK(log.WaitEvents(log.Cursor(), Filled(2), std::nullopt, start + 5s, probe.Predicate()) == Watch::INTERRUPTED);
        BOOST_CHECK(WaitClock::now() - start < 1s);
    }
    {
        WaitProbe probe;
        const auto start{WaitClock::now()};
        auto watcher{std::async(std::launch::async, [&] {
            return log.WaitEvents(log.Cursor(), Filled(2), std::nullopt, start + 5s, probe.Predicate());
        })};
        probe.AwaitEvaluations(1);
        log.Reset(Filled(7));
        BOOST_CHECK(watcher.get() == Watch::RESTARTED);
        BOOST_CHECK(WaitClock::now() - start < 2s);
    }
    BOOST_CHECK_EQUAL(log.WatchersForTest(), 0U);
}

BOOST_AUTO_TEST_CASE(event_watch_registration_cap_returns_busy)
{
    flowmesh::ClientEventLog log{Filled(1)};
    const auto cursor{log.Cursor()};
    std::vector<std::future<flowmesh::ClientWatchResult>> watchers;
    for (size_t i{0}; i < flowmesh::CLIENT_EVENT_MAX_WATCHERS; ++i) {
        watchers.push_back(std::async(std::launch::async, [&] {
            return log.WaitEvents(cursor, Filled(2), std::nullopt, WaitClock::now() + 10s, {});
        }));
    }
    const auto deadline{WaitClock::now() + 5s};
    while (log.WatchersForTest() < flowmesh::CLIENT_EVENT_MAX_WATCHERS && WaitClock::now() < deadline) std::this_thread::sleep_for(1ms);
    BOOST_REQUIRE_EQUAL(log.WatchersForTest(), flowmesh::CLIENT_EVENT_MAX_WATCHERS);
    const auto start{WaitClock::now()};
    BOOST_CHECK(log.WaitEvents(cursor, Filled(2), std::nullopt, start + 5s, {}) == flowmesh::ClientWatchResult::BUSY);
    BOOST_CHECK(WaitClock::now() - start < 1s);
    // An already answerable watch never needs a registration.
    log.Append(AccountEvent(3, 4, flowmesh::ClientEventKind::POOL_ADMITTED));
    BOOST_CHECK(log.WaitEvents(cursor, Filled(3), std::nullopt, start + 5s, {}) == flowmesh::ClientWatchResult::READY);
    // One relevant append ends every registered watch.
    log.Append(AccountEvent(2, 4, flowmesh::ClientEventKind::CERTIFIED_HEAD));
    for (auto& watcher : watchers) BOOST_CHECK(watcher.get() == flowmesh::ClientWatchResult::READY);
    BOOST_CHECK_EQUAL(log.WatchersForTest(), 0U);
}

BOOST_AUTO_TEST_CASE(event_watch_does_not_disturb_action_waits)
{
    using Kind = flowmesh::ClientEventKind;
    flowmesh::ClientEventLog log{Filled(1)};
    log.Append(ActionEvent(2, 3, Kind::QUEUE_ADMITTED));
    const auto cursor{log.Cursor()};
    const auto start{WaitClock::now()};
    auto action{std::async(std::launch::async, [&] {
        return log.WaitActionStatus(Filled(2), Filled(3), start + 300ms, {});
    })};
    auto watcher{std::async(std::launch::async, [&] {
        return log.WaitEvents(cursor, Filled(2), std::nullopt, start + 5s, {});
    })};
    while (log.WatchersForTest() == 0) std::this_thread::sleep_for(1ms);
    // Relevant to the watch, not to the action.
    log.Append(ActionEvent(2, 4, Kind::CERTIFIED_INCLUDED));
    BOOST_CHECK(watcher.get() == flowmesh::ClientWatchResult::READY);
    const auto waited{action.get()};
    BOOST_CHECK(waited.result == flowmesh::ClientWaitResult::TIMEOUT);
    BOOST_REQUIRE(waited.status);
    BOOST_CHECK(waited.status->kind == Kind::QUEUE_ADMITTED);
    BOOST_CHECK(WaitClock::now() - start >= 300ms);
    // With a watch registered, the action's inclusion still ends its wait.
    const auto next{log.Cursor()};
    auto second{std::async(std::launch::async, [&] {
        return log.WaitEvents(next, Filled(5), std::nullopt, WaitClock::now() + 300ms, {});
    })};
    WaitProbe probe;
    auto terminal{std::async(std::launch::async, [&] {
        return log.WaitActionStatus(Filled(2), Filled(3), WaitClock::now() + 5s, probe.Predicate());
    })};
    probe.AwaitEvaluations(1);
    log.Append(ActionEvent(2, 3, Kind::CERTIFIED_INCLUDED));
    BOOST_CHECK(terminal.get().result == flowmesh::ClientWaitResult::TERMINAL);
    BOOST_CHECK(second.get() == flowmesh::ClientWatchResult::TIMEOUT);
}

BOOST_AUTO_TEST_CASE(runtime_certified_action_status_recovers_after_restart_without_resubmission)
{
    EvidenceFixture f;
    EvidenceChain chain{f};
    NoSeatKeys keys;
    node::SteadyFlowMeshRuntimeClock clock;
    node::FlowMeshProductionStore store{DBParams{.path = m_path_root / "client-evidence", .cache_bytes = 1 << 20}};
    std::string error;
    BOOST_REQUIRE(store.OpenForMarket(f.pins.domain, f.pins.market_id, f.seats, f.state.Root(), error));
    node::FlowMeshRuntimeConfig config;
    config.chain = &chain;
    config.keys = &keys;
    config.clock = &clock;
    std::atomic<size_t> action_relays{0}, signing_relays{0};
    config.relay = [&](node::FlowMeshRuntimeRelay relay) {
        if (relay.message.kind == flowmesh::WireMessageKind::ACTION) ++action_relays;
        if (relay.message.kind == flowmesh::WireMessageKind::PROPOSAL ||
            relay.message.kind == flowmesh::WireMessageKind::ATTESTATION) ++signing_relays;
        return node::FlowMeshRelayResult{};
    };
    node::FlowMeshRuntimeMarketConfig market{.domain = f.pins.domain, .market_id = f.pins.market_id,
                                             .treasury_owner_commitment = Filled(9), .active_seats = f.seats,
                                             .state = f.state};
    market.store = &store;
    node::FlowMeshRuntime runtime{config, {market}};
    BOOST_REQUIRE_MESSAGE(runtime.Start(error), error);
    BOOST_CHECK(!runtime.ClientSnapshot(f.pins.market_id, error));
    flowmesh::Action action;
    action.signer = f.account;
    action.sequence = 7;
    action.type = static_cast<uint8_t>(flowmesh::ActionType::CANCEL_BID);
    BOOST_REQUIRE(flowmesh::SignAction(f.account_key, f.pins.domain, f.pins.execution_config_id, action));
    const auto original_bytes{flowmesh::EncodeProductionActionPayload(action)};
    BOOST_REQUIRE(original_bytes);
    const auto original_id{action.Id()};
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

    // A response must retain its own authenticated context even when the next
    // certificate is published before the caller consumes that response.
    const auto paired_before{runtime.ClientSnapshotView(f.pins.market_id, f.account, error)};
    BOOST_REQUIRE_MESSAGE(paired_before, error);
    const auto history_in_context = [](const flowmesh::MarketData& reported, uint64_t authenticated_next) {
        uint64_t previous{authenticated_next};
        for (const auto& row : reported.history.entries) {
            // This is the stale/out-of-order rejection boundary in the remote
            // client's ReportedHistory; endpoint history is not a state proof.
            if (row.sequence >= authenticated_next || row.sequence >= previous) return false;
            previous = row.sequence;
        }
        return true;
    };
    const auto check_paired_context = [&](const node::FlowMeshClientSnapshotView& paired) {
        const auto authenticated{f.Verify(paired.evidence, error)};
        BOOST_REQUIRE_MESSAGE(authenticated, error);
        const auto derived{flowmesh::ClientMarketData(f.pins, *authenticated, f.account, {}, error)};
        BOOST_REQUIRE_MESSAGE(derived, error);
        const auto& reported{paired.reported};
        BOOST_CHECK(reported.domain == derived->domain);
        BOOST_CHECK(reported.market_id == derived->market_id);
        BOOST_CHECK(reported.base_asset_id == derived->base_asset_id);
        BOOST_CHECK(reported.execution_config_id == derived->execution_config_id);
        BOOST_CHECK(reported.snapshot.certified);
        BOOST_CHECK_EQUAL(reported.snapshot.next_microblock_sequence, derived->snapshot.next_microblock_sequence);
        BOOST_CHECK(reported.snapshot.last_microblock_hash == derived->snapshot.last_microblock_hash);
        BOOST_CHECK(reported.snapshot.state_root == derived->snapshot.state_root);
        BOOST_REQUIRE(reported.account && derived->account);
        BOOST_CHECK(reported.account->account_id == f.account);
        BOOST_CHECK_EQUAL(reported.account->next_sequence, derived->account->next_sequence);
        BOOST_CHECK_EQUAL(reported.account->base_available, derived->account->base_available);
        BOOST_CHECK_EQUAL(reported.account->base_reserved, derived->account->base_reserved);
        BOOST_CHECK_EQUAL(reported.account->b3_available_atoms, derived->account->b3_available_atoms);
        BOOST_CHECK_EQUAL(reported.account->b3_reserved_atoms, derived->account->b3_reserved_atoms);
        BOOST_REQUIRE(reported.history.available);
        BOOST_REQUIRE(!reported.history.entries.empty());
        BOOST_CHECK(history_in_context(reported, derived->snapshot.next_microblock_sequence));
        BOOST_CHECK_EQUAL(reported.history.entries.front().sequence, authenticated->certified.entry.sequence);
        BOOST_CHECK(reported.history.entries.front().microblock_hash == authenticated->certified.entry.GetHash());
    };
    check_paired_context(*paired_before);
    BOOST_CHECK(paired_before->evidence.certified_payload == *payload);
    BOOST_CHECK_EQUAL(paired_before->reported.snapshot.next_microblock_sequence, 1U);
    BOOST_CHECK_EQUAL(paired_before->reported.account->next_sequence, 8U);

    // Move the certified head past the target instruction. Recovery must find
    // its old certificate, not attribute it to the latest whole-state proof.
    auto later{action};
    later.sequence = 8;
    later.type = static_cast<uint8_t>(flowmesh::ActionType::SUBMIT_BID);
    later.curve = {{6, 1}, {7, 0}};
    BOOST_REQUIRE(flowmesh::SignAction(f.account_key, f.pins.domain, f.pins.execution_config_id, later));
    const std::array<flowmesh::Action, 1> later_actions{later};
    const auto next{flowmesh::BuildProductionExecutionEntry(built->next_state, f.pins.domain, f.pins.market_id,
        f.seats, gate, 1, built->entry.effect_count, built->entry.GetHash(), f.anchor,
        {130, f.anchor, &chain}, Filled(9), later_actions, nullptr, check)};
    BOOST_REQUIRE(next);
    const flowmesh::ProductionCertifiedEnvelope later_certified{next->entry, f.Certify(next->entry)};
    const auto later_payload{flowmesh::EncodeProductionCertifiedPayload(later_certified, 4)};
    BOOST_REQUIRE(later_payload);
    BOOST_REQUIRE(runtime.EnqueueWireMessage(7, {flowmesh::WireMessageKind::CERTIFICATE,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.pins.market_id, 0, 1}, *later_payload}) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{5}));

    // Deterministic negative control for the former split endpoint assembly:
    // retain certificate/state at sequence 0, then fetch history after sequence
    // 1 commits. Both reads are individually valid, but this mixed response
    // crosses the client's authenticated-next-sequence rejection boundary.
    const auto split_later_history{runtime.MarketData(f.pins.market_id, f.account, {}, error)};
    BOOST_REQUIRE_MESSAGE(split_later_history, error);
    const auto old_derived{flowmesh::ClientMarketData(f.pins, *verified, f.account, {}, error)};
    BOOST_REQUIRE_MESSAGE(old_derived, error);
    BOOST_CHECK_EQUAL(old_derived->snapshot.next_microblock_sequence, 1U);
    BOOST_CHECK_EQUAL(split_later_history->snapshot.next_microblock_sequence, 2U);
    BOOST_REQUIRE(!split_later_history->history.entries.empty());
    BOOST_CHECK_EQUAL(split_later_history->history.entries.front().sequence, 1U);
    BOOST_CHECK(!history_in_context(*split_later_history, old_derived->snapshot.next_microblock_sequence));
    BOOST_CHECK(split_later_history->snapshot.last_microblock_hash != old_derived->snapshot.last_microblock_hash);
    BOOST_CHECK(split_later_history->snapshot.state_root != old_derived->snapshot.state_root);

    const auto paired_after{runtime.ClientSnapshotView(f.pins.market_id, f.account, error)};
    BOOST_REQUIRE_MESSAGE(paired_after, error);
    check_paired_context(*paired_after);
    BOOST_CHECK(paired_after->evidence.certified_payload == *later_payload);
    BOOST_CHECK_EQUAL(paired_after->reported.snapshot.next_microblock_sequence, 2U);
    BOOST_CHECK_EQUAL(paired_after->reported.account->next_sequence, 9U);
    // Earlier exported objects neither borrow mutable runtime state nor get
    // relabelled with the new head: they still authenticate their original view.
    check_paired_context(*paired_before);
    BOOST_CHECK(paired_before->evidence.certified_payload == snapshot->certified_payload);
    BOOST_CHECK(paired_before->evidence.state_bytes == snapshot->state_bytes);
    BOOST_CHECK(paired_before->reported.snapshot.last_microblock_hash == built->entry.GetHash());
    BOOST_CHECK(paired_before->reported.snapshot.state_root == built->next_state.Root());
    BOOST_CHECK_EQUAL(paired_before->reported.account->next_sequence, 8U);

    const auto cursor{snapshot->cursor};
    runtime.Stop();
    const auto action_relays_before{action_relays.load()};
    const auto signing_relays_before{signing_relays.load()};
    market.state = next->next_state;
    market.next_sequence = 2;
    market.next_effect_index = next->entry.effect_start + next->entry.effect_count;
    market.last_microblock_hash = next->entry.GetHash();
    node::FlowMeshRuntime restarted{config, {market}};
    BOOST_REQUIRE_MESSAGE(restarted.Start(error), error);
    const auto restored{restarted.ClientSnapshot(f.pins.market_id, error)};
    BOOST_REQUIRE(restored);
    BOOST_CHECK(restored->certified_payload == *later_payload);
    BOOST_CHECK(restarted.ClientEvents(cursor, f.pins.market_id, f.account).gap);
    // The old proof is durably available even before the action lookup repair.
    const auto old_payload{restarted.ClientCertifiedEntry(f.pins.market_id, 0, error)};
    BOOST_REQUIRE_MESSAGE(old_payload, error);
    BOOST_CHECK(*old_payload == *payload);
    const auto old_state{flowmesh::EncodeClientState(built->next_state, error)};
    BOOST_REQUIRE(old_state);
    BOOST_REQUIRE(f.Verify({*old_payload, *old_state, restored->cursor}, error));
    const auto recovered{restarted.ClientActionStatus(f.pins.market_id, original_id)};
    BOOST_CHECK_MESSAGE(recovered, "Stored certificate is available, but restarted action lookup lost certified inclusion");
    if (recovered) {
        BOOST_CHECK(recovered->kind == flowmesh::ClientEventKind::CERTIFIED_INCLUDED);
        BOOST_CHECK(recovered->action_id == original_id && recovered->account_id == f.account);
        BOOST_CHECK_EQUAL(recovered->account_sequence, 7U);
        BOOST_CHECK_EQUAL(recovered->microblock_sequence, 0U);
        BOOST_CHECK(recovered->microblock_hash == built->entry.GetHash());
    }
    BOOST_CHECK(!restarted.ClientActionStatus(f.pins.market_id, Filled(123)));
    BOOST_CHECK(!restarted.ClientActionStatus(Filled(124), original_id));
    BOOST_CHECK(!restarted.ClientActionStatus(f.pins.market_id, bad.Id()));
    BOOST_CHECK(flowmesh::EncodeProductionActionPayload(action) == original_bytes);
    BOOST_CHECK(action.Id() == original_id && action.sequence == 7);
    restarted.Stop();
    BOOST_CHECK_EQUAL(action_relays.load(), action_relays_before);
    BOOST_CHECK_EQUAL(signing_relays.load(), signing_relays_before);
}

BOOST_AUTO_TEST_CASE(runtime_public_already_credited_deposit_is_answered_in_the_reply_only)
{
    EvidenceFixture f;
    // An outpoint credited before any entry this node retains: which action
    // credited it is unknown here.
    const COutPoint credited{Txid::FromUint256(Filled(0x44)), 3};
    BOOST_REQUIRE(flowmesh::test_only::StateFunding::CreditDeposit(f.state, credited, f.account, f.pins.base_asset, 5));
    EvidenceChain chain{f};
    NoSeatKeys keys;
    node::SteadyFlowMeshRuntimeClock clock;
    node::FlowMeshProductionStore store{DBParams{.path = m_path_root / "credited-deposit", .cache_bytes = 1 << 20}};
    std::string error;
    BOOST_REQUIRE(store.OpenForMarket(f.pins.domain, f.pins.market_id, f.seats, f.state.Root(), error));
    node::FlowMeshRuntimeConfig config;
    config.chain = &chain;
    config.keys = &keys;
    config.clock = &clock;
    config.relay = [](node::FlowMeshRuntimeRelay) { return node::FlowMeshRelayResult{}; };
    node::FlowMeshRuntimeMarketConfig market{.domain = f.pins.domain, .market_id = f.pins.market_id,
                                             .treasury_owner_commitment = Filled(9), .active_seats = f.seats,
                                             .state = f.state};
    market.store = &store;
    node::FlowMeshRuntime runtime{config, {market}};
    BOOST_REQUIRE_MESSAGE(runtime.Start(error), error);
    flowmesh::Action deposit;
    deposit.type = static_cast<uint8_t>(flowmesh::ActionType::DEPOSIT);
    deposit.outpoint = credited;
    // A public deposit carries no credential and credited outpoints are
    // public, so anyone could replay this one: the answer is bound to the
    // submit reply, as for a deposit refused for missing facts.
    const auto replayed{runtime.SubmitLocalActionChecked(f.pins.market_id, deposit)};
    BOOST_CHECK(!replayed.queued);
    BOOST_CHECK(replayed.admission.kind == node::FlowMeshLocalAdmission::Kind::UNKNOWN_OUTCOME);
    BOOST_CHECK(replayed.admission.code == flowmesh::ActionReasonCode::DEPOSIT_ALREADY_CREDITED);
    BOOST_CHECK(!runtime.NewestClientNotice(f.pins.market_id, deposit.Id()));
    // This node's own deposit still gets its notice.
    const auto own{runtime.SubmitLocalActionChecked(f.pins.market_id, deposit, flowmesh::ClientNoticeOrigin::NODE)};
    BOOST_CHECK(own.admission.code == flowmesh::ActionReasonCode::DEPOSIT_ALREADY_CREDITED);
    const auto notice{runtime.NewestClientNotice(f.pins.market_id, deposit.Id())};
    BOOST_REQUIRE(notice);
    BOOST_CHECK(notice->code == flowmesh::ActionReasonCode::DEPOSIT_ALREADY_CREDITED);
    BOOST_CHECK(notice->origin == flowmesh::ClientNoticeOrigin::NODE);
    runtime.Stop();
}

BOOST_AUTO_TEST_CASE(runtime_wait_client_action_status_wakes_on_commit)
{
    EvidenceFixture f;
    EvidenceChain chain{f};
    NoSeatKeys keys;
    node::SteadyFlowMeshRuntimeClock clock;
    node::FlowMeshProductionStore store{DBParams{.path = m_path_root / "client-wait", .cache_bytes = 1 << 20}};
    std::string error;
    BOOST_REQUIRE(store.OpenForMarket(f.pins.domain, f.pins.market_id, f.seats, f.state.Root(), error));
    node::FlowMeshRuntimeConfig config;
    config.chain = &chain;
    config.keys = &keys;
    config.clock = &clock;
    config.relay = [](node::FlowMeshRuntimeRelay) { return node::FlowMeshRelayResult{}; };
    node::FlowMeshRuntimeMarketConfig market{.domain = f.pins.domain, .market_id = f.pins.market_id,
        .treasury_owner_commitment = Filled(9), .active_seats = f.seats, .state = f.state, .store = &store};
    node::FlowMeshRuntime runtime{config, {market}};
    // Nothing can be waited for before the runtime starts.
    BOOST_CHECK(runtime.WaitClientActionStatus(f.pins.market_id, Filled(8), WaitClock::now() + 5s, {}).result ==
                flowmesh::ClientWaitResult::INTERRUPTED);
    BOOST_REQUIRE_MESSAGE(runtime.Start(error), error);
    flowmesh::Action action;
    action.signer = f.account;
    action.sequence = 7;
    action.type = static_cast<uint8_t>(flowmesh::ActionType::CANCEL_BID);
    BOOST_REQUIRE(flowmesh::SignAction(f.account_key, f.pins.domain, f.pins.execution_config_id, action));
    BOOST_REQUIRE(runtime.SubmitLocalAction(f.pins.market_id, action) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{5}));
    BOOST_REQUIRE(runtime.ClientActionStatus(f.pins.market_id, action.Id())->kind == flowmesh::ClientEventKind::POOL_ADMITTED);

    flowmesh::ProductionEntryCheck check;
    flowmesh::ProductionEpochGate gate{f.pins.domain, f.pins.market_id, f.seats};
    const std::array<flowmesh::Action, 1> actions{action};
    const auto built{flowmesh::BuildProductionExecutionEntry(f.state, f.pins.domain, f.pins.market_id,
        f.seats, gate, 0, 0, {}, f.anchor, {130, {}, &chain}, Filled(9), actions, nullptr, check)};
    BOOST_REQUIRE(built);
    const flowmesh::ProductionCertifiedEnvelope certified{built->entry, f.Certify(built->entry)};
    const auto payload{flowmesh::EncodeProductionCertifiedPayload(certified, 4)};
    BOOST_REQUIRE(payload);

    // The woken waiter reads the payload exactly as the public 'action' read
    // does. It must find the durable head, never "unavailable".
    WaitProbe probe;
    const auto start{WaitClock::now()};
    auto waiter{std::async(std::launch::async, [&] {
        const auto result{runtime.WaitClientActionStatus(f.pins.market_id, action.Id(), start + 5s, probe.Predicate())};
        std::string entry_error;
        auto entry{result.status ? runtime.ClientCertifiedEntry(f.pins.market_id, result.status->microblock_sequence, entry_error)
                                 : std::nullopt};
        return std::pair{result, std::move(entry)};
    })};
    probe.AwaitEvaluations(1);
    BOOST_REQUIRE(runtime.EnqueueWireMessage(7, {flowmesh::WireMessageKind::CERTIFICATE,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.pins.market_id, 0, 0}, *payload}) == flowmesh::QueueResult::ACCEPTED);
    const auto [result, entry]{waiter.get()};
    BOOST_CHECK(result.result == flowmesh::ClientWaitResult::TERMINAL);
    BOOST_REQUIRE(result.status);
    BOOST_CHECK(result.status->kind == flowmesh::ClientEventKind::CERTIFIED_INCLUDED);
    BOOST_CHECK_EQUAL(result.status->microblock_sequence, 0U);
    BOOST_CHECK(result.status->microblock_hash == built->entry.GetHash());
    BOOST_REQUIRE(entry);
    BOOST_CHECK(*entry == *payload);
    BOOST_CHECK(WaitClock::now() - start < 4s);

    // Stop interrupts a current wait promptly, and every later one at once.
    WaitProbe pending;
    const auto stop_start{WaitClock::now()};
    auto stopped{std::async(std::launch::async, [&] {
        return runtime.WaitClientActionStatus(f.pins.market_id, Filled(8), stop_start + 5s, pending.Predicate());
    })};
    pending.AwaitEvaluations(1);
    runtime.Stop();
    BOOST_CHECK(stopped.get().result == flowmesh::ClientWaitResult::INTERRUPTED);
    BOOST_CHECK(runtime.WaitClientActionStatus(f.pins.market_id, action.Id(), WaitClock::now() + 5s, {}).result ==
                flowmesh::ClientWaitResult::TERMINAL); // Already terminal: returned before any wait.
    BOOST_CHECK(runtime.WaitClientActionStatus(f.pins.market_id, Filled(8), WaitClock::now() + 5s, {}).result ==
                flowmesh::ClientWaitResult::INTERRUPTED);
    BOOST_CHECK(WaitClock::now() - stop_start < 2s);
}

BOOST_AUTO_TEST_CASE(runtime_wait_client_events_wakes_on_commit_and_stop)
{
    using Watch = flowmesh::ClientWatchResult;
    EvidenceFixture f;
    EvidenceChain chain{f};
    NoSeatKeys keys;
    node::SteadyFlowMeshRuntimeClock clock;
    node::FlowMeshProductionStore store{DBParams{.path = m_path_root / "client-watch", .cache_bytes = 1 << 20}};
    std::string error;
    BOOST_REQUIRE(store.OpenForMarket(f.pins.domain, f.pins.market_id, f.seats, f.state.Root(), error));
    node::FlowMeshRuntimeConfig config;
    config.chain = &chain;
    config.keys = &keys;
    config.clock = &clock;
    config.relay = [](node::FlowMeshRuntimeRelay) { return node::FlowMeshRelayResult{}; };
    node::FlowMeshRuntimeMarketConfig market{.domain = f.pins.domain, .market_id = f.pins.market_id,
        .treasury_owner_commitment = Filled(9), .active_seats = f.seats, .state = f.state, .store = &store};
    node::FlowMeshRuntime runtime{config, {market}};
    // Before Start, a watch that would sleep is interrupted at once.
    const auto start{WaitClock::now()};
    BOOST_CHECK(runtime.WaitClientEvents(runtime.ClientEventCursor(), f.pins.market_id, std::nullopt,
                                         start + 5s, {}) == Watch::INTERRUPTED);
    BOOST_CHECK(WaitClock::now() - start < 1s);
    BOOST_REQUIRE_MESSAGE(runtime.Start(error), error);
    flowmesh::Action action;
    action.signer = f.account;
    action.sequence = 7;
    action.type = static_cast<uint8_t>(flowmesh::ActionType::CANCEL_BID);
    BOOST_REQUIRE(flowmesh::SignAction(f.account_key, f.pins.domain, f.pins.execution_config_id, action));
    BOOST_REQUIRE(runtime.SubmitLocalAction(f.pins.market_id, action) == flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(runtime.WaitForIdle(std::chrono::seconds{5}));
    const auto cursor{runtime.ClientEventCursor()};
    BOOST_CHECK(runtime.ClientEventsPending(cursor, f.pins.market_id, f.account) == Watch::TIMEOUT);
    BOOST_CHECK(runtime.ClientEventsPending({cursor.instance_id, 0}, f.pins.market_id, f.account) == Watch::READY);
    BOOST_CHECK(runtime.ClientEventsPending({Filled(77), 0}, f.pins.market_id, f.account) == Watch::GAP);

    flowmesh::ProductionEntryCheck check;
    flowmesh::ProductionEpochGate gate{f.pins.domain, f.pins.market_id, f.seats};
    const std::array<flowmesh::Action, 1> actions{action};
    const auto built{flowmesh::BuildProductionExecutionEntry(f.state, f.pins.domain, f.pins.market_id,
        f.seats, gate, 0, 0, {}, f.anchor, {130, {}, &chain}, Filled(9), actions, nullptr, check)};
    BOOST_REQUIRE(built);
    const flowmesh::ProductionCertifiedEnvelope certified{built->entry, f.Certify(built->entry)};
    const auto payload{flowmesh::EncodeProductionCertifiedPayload(certified, 4)};
    BOOST_REQUIRE(payload);

    // A watch of another market is never woken by this commit.
    auto other{std::async(std::launch::async, [&] {
        return runtime.WaitClientEvents(cursor, Filled(55), std::nullopt, WaitClock::now() + 400ms, {});
    })};
    WaitProbe probe;
    auto watcher{std::async(std::launch::async, [&] {
        return runtime.WaitClientEvents(cursor, f.pins.market_id, f.account, WaitClock::now() + 5s, probe.Predicate());
    })};
    probe.AwaitEvaluations(1);
    const auto committed{WaitClock::now()};
    BOOST_REQUIRE(runtime.EnqueueWireMessage(7, {flowmesh::WireMessageKind::CERTIFICATE,
        {flowmesh::FLOWMESH_WIRE_VERSION_V1, f.pins.market_id, 0, 0}, *payload}) == flowmesh::QueueResult::ACCEPTED);
    BOOST_CHECK(watcher.get() == Watch::READY);
    BOOST_CHECK(WaitClock::now() - committed < 4s);
    BOOST_CHECK(other.get() == Watch::TIMEOUT);
    // The page it reads next begins with the commit's certified head.
    const auto page{runtime.ClientEvents(cursor, f.pins.market_id, f.account)};
    BOOST_REQUIRE(!page.gap && !page.events.empty());
    BOOST_CHECK(page.events.front().kind == flowmesh::ClientEventKind::CERTIFIED_HEAD);
    BOOST_CHECK(page.events.front().microblock_hash == built->entry.GetHash());

    // A status-only read has the same head and runtime status, and nothing else.
    const auto full{runtime.MarketData(f.pins.market_id, f.account, {}, error)};
    flowmesh::MarketDataQuery status;
    status.status_only = true;
    const auto brief{runtime.MarketData(f.pins.market_id, f.account, status, error)};
    BOOST_REQUIRE(full && brief);
    BOOST_CHECK(full->snapshot.certified && brief->snapshot.certified);
    BOOST_CHECK_EQUAL(brief->snapshot.next_microblock_sequence, full->snapshot.next_microblock_sequence);
    BOOST_CHECK(brief->snapshot.last_microblock_hash == full->snapshot.last_microblock_hash);
    BOOST_CHECK(brief->snapshot.state_root == full->snapshot.state_root);
    BOOST_CHECK_EQUAL(brief->snapshot.epoch, full->snapshot.epoch);
    BOOST_CHECK_EQUAL(brief->snapshot.paused, full->snapshot.paused);
    BOOST_CHECK_EQUAL(brief->snapshot.halt, full->snapshot.halt);
    BOOST_CHECK_EQUAL(brief->snapshot.pending_actions, full->snapshot.pending_actions);
    BOOST_CHECK(!brief->unchanged);
    BOOST_CHECK(full->account && full->history.available && !full->history.entries.empty());
    BOOST_CHECK(brief->liquidity.curves.empty());
    BOOST_CHECK(!brief->account);
    BOOST_CHECK(!brief->history.available && brief->history.entries.empty());
    // It is never combined with pagination.
    for (int shape{0}; shape < 3; ++shape) {
        auto paged{status};
        if (shape == 0) paged.before_sequence = 1;
        if (shape == 1) paged.expected_head = full->snapshot.last_microblock_hash;
        if (shape == 2) { paged.expected_head = full->snapshot.last_microblock_hash; paged.curve_cursor = flowmesh::ClearingEngine::CurveKey{flowmesh::ClearingEngine::Side::BID, f.account}; }
        error.clear();
        BOOST_CHECK(!runtime.MarketData(f.pins.market_id, f.account, paged, error));
        BOOST_CHECK(!error.empty());
    }

    // Stop interrupts a current watch promptly, and every later one at once.
    WaitProbe pending;
    const auto stop_start{WaitClock::now()};
    const auto head_cursor{runtime.ClientEventCursor()};
    auto stopped{std::async(std::launch::async, [&] {
        return runtime.WaitClientEvents(head_cursor, f.pins.market_id, std::nullopt, stop_start + 5s, pending.Predicate());
    })};
    pending.AwaitEvaluations(1);
    runtime.Stop();
    BOOST_CHECK(stopped.get() == Watch::INTERRUPTED);
    BOOST_CHECK(runtime.WaitClientEvents(head_cursor, f.pins.market_id, std::nullopt, WaitClock::now() + 5s, {}) ==
                Watch::INTERRUPTED);
    BOOST_CHECK(WaitClock::now() - stop_start < 2s);
}

BOOST_AUTO_TEST_SUITE_END()
