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
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
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

BOOST_AUTO_TEST_CASE(action_wait_timeout_forgets_status_evicted_by_unrelated_append)
{
    using Kind = flowmesh::ClientEventKind;
    for (const auto kind : {Kind::QUEUE_ADMITTED, Kind::POOL_ADMITTED}) {
        flowmesh::ClientEventLog log{Filled(1)};
        log.Append(ActionEvent(2, 3, kind)); // The watched action is oldest.
        for (size_t i{1}; i < flowmesh::CLIENT_EVENT_CAPACITY; ++i) {
            log.Append(ActionEvent(2, 4, Kind::CERTIFIED_INCLUDED));
        }
        const auto before{log.ActionStatus(Filled(2), Filled(3))};
        BOOST_REQUIRE(before);
        BOOST_CHECK_EQUAL(before->event_id, 1U);
        BOOST_CHECK_EQUAL(log.Cursor().event_id, flowmesh::CLIENT_EVENT_CAPACITY);

        // Signal from the first predicate evaluation under the log mutex:
        // the status has already been sampled, and Append cannot acquire
        // that mutex until the waiter releases it to sleep. No timing sleep
        // is used to arrange the eviction after the initial status lookup.
        std::promise<void> sampled;
        auto sampled_future{sampled.get_future()};
        std::atomic<bool> signalled{false};
        auto waiter{std::async(std::launch::async, [&] {
            return log.WaitActionStatus(Filled(2), Filled(3), WaitClock::now() + 1s, [&] {
                if (!signalled.exchange(true)) sampled.set_value();
                return false;
            });
        })};
        BOOST_REQUIRE(sampled_future.wait_for(5s) == std::future_status::ready);
        BOOST_REQUIRE(waiter.wait_for(0ms) == std::future_status::timeout);
        log.Append(ActionEvent(2, 4, Kind::CERTIFIED_INCLUDED));
        // One unrelated append evicts the target without creating a gap
        // after the waiter's last-seen cursor. Ordinary status is now unknown.
        const auto current{log.ActionStatus(Filled(2), Filled(3))};
        BOOST_CHECK(!current);
        const auto result{waiter.get()};
        BOOST_CHECK(result.result == flowmesh::ClientWaitResult::TIMEOUT);
        BOOST_CHECK_EQUAL(result.status.has_value(), current.has_value());
        BOOST_CHECK(!result.status);
        const auto page{log.Read({}, {}, {})};
        BOOST_CHECK_EQUAL(page.oldest_event_id, 2U);
        BOOST_CHECK_EQUAL(page.latest_event_id - page.oldest_event_id + 1,
                          flowmesh::CLIENT_EVENT_CAPACITY);
    }
}

BOOST_AUTO_TEST_CASE(action_wait_eviction_rechecks_retained_refusal_priority)
{
    using Kind = flowmesh::ClientEventKind;
    flowmesh::ClientEventLog log{Filled(1)};
    log.Append(ActionEvent(2, 3, Kind::POOL_ADMITTED));
    log.Append(ActionEvent(2, 3, Kind::POOL_REFUSED));
    for (size_t i{2}; i < flowmesh::CLIENT_EVENT_CAPACITY; ++i) {
        log.Append(ActionEvent(2, 4, Kind::CERTIFIED_INCLUDED));
    }
    // Admission outranks a later refusal of another attempt while retained.
    const auto before{log.ActionStatus(Filled(2), Filled(3))};
    BOOST_REQUIRE(before);
    BOOST_CHECK(before->kind == Kind::POOL_ADMITTED);

    std::promise<void> sampled;
    auto sampled_future{sampled.get_future()};
    std::atomic<bool> signalled{false};
    auto waiter{std::async(std::launch::async, [&] {
        return log.WaitActionStatus(Filled(2), Filled(3), WaitClock::now() + 5s, [&] {
            if (!signalled.exchange(true)) sampled.set_value();
            return false;
        });
    })};
    BOOST_REQUIRE(sampled_future.wait_for(5s) == std::future_status::ready);
    BOOST_REQUIRE(waiter.wait_for(0ms) == std::future_status::timeout);
    log.Append(ActionEvent(2, 4, Kind::CERTIFIED_INCLUDED));
    // Only the older admission expired. The retained refusal is now both
    // the ordinary status and this action's newest event, so it is terminal.
    const auto current{log.ActionStatus(Filled(2), Filled(3))};
    BOOST_REQUIRE(current);
    BOOST_CHECK_EQUAL(current->event_id, 2U);
    BOOST_CHECK(current->kind == Kind::POOL_REFUSED);
    BOOST_CHECK(waiter.wait_for(1s) == std::future_status::ready);
    const auto result{waiter.get()};
    BOOST_CHECK(result.result == flowmesh::ClientWaitResult::TERMINAL);
    BOOST_REQUIRE(result.status);
    BOOST_CHECK_EQUAL(result.status->event_id, current->event_id);
    BOOST_CHECK(result.status->kind == current->kind);
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

BOOST_AUTO_TEST_SUITE_END()
