// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

//! The node's display-only observed-time log and its separate database.

#include <node/flowmesh_observed_time_log.h>

#include <node/flowmesh_client.h>

#include <test/util/setup_common.h>
#include <util/fs.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace {

using flowmesh::CandleInterval;
using flowmesh::ObservedDelay;
using flowmesh::ObservedMode;
using node::FlowMeshObservedTimeLog;
using node::FlowMeshObservedTimeStore;

constexpr int64_t T0{1790769600000};

uint256 EntryHash(const uint64_t sequence, const unsigned char salt = 1)
{
    uint256 out;
    out.data()[0] = static_cast<unsigned char>(sequence & 0xff);
    out.data()[1] = static_cast<unsigned char>((sequence >> 8) & 0xff);
    out.data()[31] = salt;
    return out;
}

flowmesh::ClearingTick Tick(const uint64_t sequence, const CAmount price = 10, const CAmount volume = 1)
{
    flowmesh::ClearingTick out;
    out.sequence = sequence;
    out.microblock_hash = EntryHash(sequence);
    out.cleared = true;
    out.price = price;
    out.volume = volume;
    return out;
}

//! Hand-driven wall and steady clocks.
struct FakeClocks {
    std::atomic<int64_t> wall{T0};
    std::atomic<int64_t> steady_ms{1'000'000};
    FlowMeshObservedTimeLog::Clocks Get()
    {
        return {[this] { return wall.load(); },
                [this] { return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds{steady_ms.load()}; }};
    }
    void Advance(const int64_t ms)
    {
        wall += ms;
        steady_ms += ms;
    }
};

const uint256 TEST_DOMAIN{uint256{"00000000000000000000000000000000000000000000000000000000000000d0"}};
const flowmesh::MarketId MARKET{uint256{"00000000000000000000000000000000000000000000000000000000000000e1"}};

std::shared_ptr<FlowMeshObservedTimeLog> MakeLog(FakeClocks& clocks)
{
    return std::make_shared<FlowMeshObservedTimeLog>(TEST_DOMAIN, MARKET, clocks.Get());
}

FlowMeshObservedTimeStore::Options Options(const fs::path& path)
{
    FlowMeshObservedTimeStore::Options out;
    out.path = path;
    out.flush_interval = std::chrono::milliseconds{20};
    return out;
}

std::vector<std::pair<uint64_t, uint256>> Known(const uint64_t from, const uint64_t to)
{
    std::vector<std::pair<uint64_t, uint256>> out;
    for (uint64_t i{from}; i < to; ++i) out.emplace_back(i, EntryHash(i));
    return out;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_observed_time_log_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(monotonic_clamp_and_flags)
{
    FakeClocks clocks;
    const auto log{MakeLog(clocks)};
    auto stamp{log->Record(0, EntryHash(0), ObservedDelay::NONE, Tick(0))};
    BOOST_CHECK(stamp.mode == ObservedMode::LIVE);
    BOOST_CHECK_EQUAL(stamp.ms, T0);
    // The first stamp of a process is a seam; a matching tick prices it.
    BOOST_CHECK(stamp.flags & flowmesh::OBSERVED_RESTART);
    BOOST_CHECK(!(stamp.flags & flowmesh::OBSERVED_NO_CLEARING));
    clocks.Advance(200);
    stamp = log->Record(1, EntryHash(1), ObservedDelay::NONE, std::nullopt);
    BOOST_CHECK_EQUAL(stamp.ms, T0 + 200);
    BOOST_CHECK_EQUAL(stamp.flags, flowmesh::OBSERVED_NO_CLEARING);
    // The wall clock steps back 500 ms (inside the slew): held and flagged.
    clocks.wall -= 700;
    clocks.steady_ms += 200;
    stamp = log->Record(2, EntryHash(2), ObservedDelay::NONE, Tick(2));
    BOOST_CHECK(stamp.mode == ObservedMode::LIVE);
    BOOST_CHECK_EQUAL(stamp.ms, T0 + 200);
    BOOST_CHECK(stamp.flags & flowmesh::OBSERVED_CLOCK_CLAMPED);
    // It steps back an hour: no time at all, never the old value.
    clocks.wall -= 3'600'000;
    clocks.steady_ms += 100;
    stamp = log->Record(3, EntryHash(3), ObservedDelay::NONE, Tick(3));
    BOOST_CHECK(stamp.mode == ObservedMode::UNTIMED);
    BOOST_CHECK(stamp.delay == ObservedDelay::CLOCK_BACK);
    BOOST_CHECK_EQUAL(stamp.ms, 0);
    BOOST_CHECK(stamp.flags & flowmesh::OBSERVED_CLOCK_STEP);
    clocks.Advance(1000);
    stamp = log->Record(4, EntryHash(4), ObservedDelay::NONE, Tick(4));
    BOOST_CHECK(stamp.mode == ObservedMode::UNTIMED);
    // Once the clock passes the last stamp again, times resume.
    clocks.wall = T0 + 5'000;
    clocks.steady_ms += 100;
    stamp = log->Record(5, EntryHash(5), ObservedDelay::NONE, Tick(5));
    BOOST_CHECK(stamp.mode == ObservedMode::LIVE);
    BOOST_CHECK_EQUAL(stamp.ms, T0 + 5'000);
    BOOST_CHECK(stamp.flags & flowmesh::OBSERVED_CLOCK_STEP);
    // A forward jump the steady clock did not see is a step, and a seam.
    clocks.wall += 3'600'000;
    clocks.steady_ms += 250;
    stamp = log->Record(6, EntryHash(6), ObservedDelay::NONE, Tick(6));
    BOOST_CHECK(stamp.mode == ObservedMode::LIVE);
    BOOST_CHECK(stamp.flags & flowmesh::OBSERVED_CLOCK_STEP);
    BOOST_CHECK(log->Candles(CandleInterval::M1, std::nullopt, 10).candles.back().flags & flowmesh::CANDLE_SEAM_BEFORE);
    // A late entry keeps its reason and only an upper bound.
    clocks.Advance(100);
    stamp = log->Record(7, EntryHash(7), ObservedDelay::CATCHUP, Tick(7));
    BOOST_CHECK(stamp.mode == ObservedMode::DELAYED);
    BOOST_CHECK(stamp.delay == ObservedDelay::CATCHUP);
    log->SetClockSuspect(true);
    clocks.Advance(100);
    BOOST_CHECK(log->Record(8, EntryHash(8), ObservedDelay::NONE, Tick(8)).flags & flowmesh::OBSERVED_CLOCK_SUSPECT);
    // Lookup returns exactly the matching entries.
    const auto rows{log->Lookup({{8, EntryHash(8)}, {7, EntryHash(7)}, {6, EntryHash(6, 2)}, {99, EntryHash(99)}})};
    BOOST_REQUIRE_EQUAL(rows.size(), 2U);
    BOOST_CHECK_EQUAL(rows[0].sequence, 7U);
    BOOST_CHECK_EQUAL(rows[1].sequence, 8U);
    BOOST_CHECK_EQUAL(log->Generation(), 9U);
}

BOOST_AUTO_TEST_CASE(persist_restart_reload_rows_and_candles)
{
    const auto path{m_path_root / "observed_reload"};
    FakeClocks clocks;
    {
        FlowMeshObservedTimeStore store{Options(path)};
        std::string error;
        BOOST_REQUIRE(store.Open(error));
        const auto log{MakeLog(clocks)};
        store.Attach(log, 0, {});
        for (uint64_t i{0}; i < 30; ++i) {
            log->Record(i, EntryHash(i), ObservedDelay::NONE, Tick(i, 100 + static_cast<CAmount>(i)));
            clocks.Advance(10'000);
        }
        store.Stop();
        BOOST_CHECK(log->GetSummary().persisted);
    }
    FlowMeshObservedTimeStore store{Options(path)};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    store.Attach(log, 30, Known(0, 30));
    BOOST_CHECK_EQUAL(log->GetSummary().recorded_since.value_or(99), 0U);
    const auto rows{log->Lookup(Known(0, 30))};
    BOOST_CHECK_EQUAL(rows.size(), 30U);
    const auto page{log->Candles(CandleInterval::M1, std::nullopt, 100)};
    BOOST_REQUIRE_EQUAL(page.candles.size(), 5U);
    BOOST_CHECK_EQUAL(page.candles.front().open, 100);
    BOOST_CHECK_EQUAL(page.candles.back().last_sequence, 29U);
    // The next entry continues the run: after a restart it is a seam.
    const auto stamp{log->Record(30, EntryHash(30), ObservedDelay::NONE, Tick(30))};
    BOOST_CHECK(stamp.flags & flowmesh::OBSERVED_RESTART);
    const auto minutes{log->Candles(CandleInterval::M1, std::nullopt, 100).candles};
    BOOST_CHECK_EQUAL(minutes.back().last_sequence, 30U);
    // Disk paging returns the same stored candles.
    bool older{true};
    const auto disk{store.ReadCandles(MARKET, CandleInterval::M1, T0, T0 + 10 * 60'000, 100, &older)};
    BOOST_CHECK_EQUAL(disk.size(), 5U);
    BOOST_CHECK(!older);
}

BOOST_AUTO_TEST_CASE(unflushed_tail_is_a_gap_not_an_error)
{
    const auto path{m_path_root / "observed_tail"};
    FakeClocks clocks;
    {
        FlowMeshObservedTimeStore store{Options(path)};
        std::string error;
        BOOST_REQUIRE(store.Open(error));
        const auto log{MakeLog(clocks)};
        store.Attach(log, 0, {});
        for (uint64_t i{0}; i < 10; ++i) {
            log->Record(i, EntryHash(i), ObservedDelay::NONE, Tick(i));
            clocks.Advance(1'000);
        }
        store.FlushNow(false);
        for (uint64_t i{10}; i < 15; ++i) {
            log->Record(i, EntryHash(i), ObservedDelay::NONE, Tick(i));
            clocks.Advance(1'000);
        }
        // A crash: the tail never reaches the database.
        (void)log->TakePending();
        store.Stop();
    }
    FlowMeshObservedTimeStore store{Options(path)};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    store.Attach(log, 15, Known(0, 15));
    BOOST_CHECK_EQUAL(log->Lookup(Known(0, 15)).size(), 10U);
    clocks.Advance(120'000);
    log->Record(15, EntryHash(15), ObservedDelay::NONE, Tick(15));
    const auto minutes{log->Candles(CandleInterval::M1, std::nullopt, 100).candles};
    BOOST_REQUIRE_GE(minutes.size(), 2U);
    // Entries 10..14 have no time: the candles are not contiguous.
    BOOST_CHECK_EQUAL(minutes[minutes.size() - 2].last_sequence, 9U);
    BOOST_CHECK_EQUAL(minutes.back().first_sequence, 15U);
}

BOOST_AUTO_TEST_CASE(corrupt_database_is_wiped_and_recreated)
{
    const auto path{m_path_root / "observed_corrupt"};
    fs::create_directories(path);
    {
        std::ofstream current{fs::PathToString(path / "CURRENT")};
        current << "MANIFEST-999999\n";
    }
    FlowMeshObservedTimeStore store{Options(path)};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    FakeClocks clocks;
    const auto log{MakeLog(clocks)};
    store.Attach(log, 5, {});
    BOOST_CHECK(log->Lookup(Known(0, 5)).empty());
    log->Record(5, EntryHash(5), ObservedDelay::NONE, Tick(5));
    store.Stop();
    BOOST_CHECK(log->GetSummary().persisted);
}

BOOST_AUTO_TEST_CASE(rows_beyond_store_head_dropped_and_mismatch_wipes)
{
    const auto path{m_path_root / "observed_mismatch"};
    FakeClocks clocks;
    {
        FlowMeshObservedTimeStore store{Options(path)};
        std::string error;
        BOOST_REQUIRE(store.Open(error));
        const auto log{MakeLog(clocks)};
        store.Attach(log, 0, {});
        for (uint64_t i{0}; i < 20; ++i) {
            log->Record(i, EntryHash(i), ObservedDelay::NONE, Tick(i));
            clocks.Advance(1'000);
        }
        store.Stop();
    }
    {
        // The store only holds 12 entries: rows 12.. were never certified here.
        FlowMeshObservedTimeStore store{Options(path)};
        std::string error;
        BOOST_REQUIRE(store.Open(error));
        const auto log{MakeLog(clocks)};
        store.Attach(log, 12, Known(0, 12));
        BOOST_CHECK_EQUAL(log->Lookup(Known(0, 20)).size(), 12U);
        for (const auto& candle : log->Candles(CandleInterval::H1, std::nullopt, 10).candles) BOOST_CHECK_LT(candle.last_sequence, 12U);
        // Memory filtering alone is insufficient: the persisted bucket
        // originally held sequence 19, and older-page reads use the DB.
        bool older{true};
        const auto disk{store.ReadCandles(MARKET, CandleInterval::H1, T0, T0 + 3'600'000, 10, &older)};
        BOOST_CHECK(disk.empty());
        BOOST_CHECK(!older);
        for (const auto interval : flowmesh::CANDLE_INTERVALS) {
            for (const auto& candle : store.ReadCandles(MARKET, interval, 0, T0 + 86'400'000, 100)) {
                BOOST_CHECK_LT(candle.last_sequence, 12U);
            }
        }
        store.Stop();
    }
    FlowMeshObservedTimeStore store{Options(path)};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    auto known{Known(0, 12)};
    known[5].second = EntryHash(5, 7);
    store.Attach(log, 12, known);
    BOOST_CHECK(log->Lookup(Known(0, 12)).empty());
    BOOST_CHECK(log->Candles(CandleInterval::M1, std::nullopt, 10).candles.empty());
    BOOST_CHECK(!log->GetSummary().recorded_since);
}

BOOST_AUTO_TEST_CASE(pending_overflow_drops_rows_from_persistence_only)
{
    FakeClocks clocks;
    const auto log{MakeLog(clocks)};
    const uint64_t total{FlowMeshObservedTimeLog::PENDING_ROWS_MAX + 10};
    for (uint64_t i{0}; i < total; ++i) {
        log->Record(i, EntryHash(i), ObservedDelay::NONE, Tick(i));
        clocks.Advance(10);
    }
    BOOST_CHECK_EQUAL(log->GetSummary().rows_not_persisted, 10U);
    const auto pending{log->TakePending()};
    BOOST_CHECK_EQUAL(pending.rows.size(), FlowMeshObservedTimeLog::PENDING_ROWS_MAX);
    BOOST_CHECK_EQUAL(pending.rows.front().sequence, 10U);
    // Memory keeps the newest ring.
    BOOST_CHECK_EQUAL(log->Lookup({{total - 1, EntryHash(total - 1)}}).size(), 1U);
    BOOST_CHECK(log->TakePending().Empty());
}

BOOST_AUTO_TEST_CASE(flush_failure_sets_persisted_false)
{
    FakeClocks clocks;
    FlowMeshObservedTimeStore store{Options(m_path_root / "observed_fail")};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    store.Attach(log, 0, {});
    store.SetFlushHookForTest([] { throw std::runtime_error{"disk"}; });
    log->Record(0, EntryHash(0), ObservedDelay::NONE, Tick(0));
    store.FlushNow(false);
    BOOST_CHECK(!log->GetSummary().persisted);
    // Recording carries on in memory.
    clocks.Advance(10);
    BOOST_CHECK(log->Record(1, EntryHash(1), ObservedDelay::NONE, Tick(1)).mode == ObservedMode::LIVE);
    store.SetFlushHookForTest({});
    store.FlushNow(false);
    BOOST_CHECK(log->GetSummary().persisted);
}

BOOST_AUTO_TEST_CASE(stop_syncs_an_already_flushed_tail)
{
    FakeClocks clocks;
    FlowMeshObservedTimeStore store{Options(m_path_root / "observed_final_sync")};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    store.Attach(log, 0, {});
    log->Record(0, EntryHash(0), ObservedDelay::NONE, Tick(0));
    store.FlushNow(false);
    BOOST_CHECK(log->TakePending().Empty());
    unsigned writes{0};
    store.SetFlushHookForTest([&] { ++writes; });
    store.Stop();
    BOOST_CHECK_EQUAL(writes, 1U);
    BOOST_CHECK(log->GetSummary().persisted);
    // A failure of that final sync is visible even with no new rows.
    store.SetFlushHookForTest([] { throw std::runtime_error{"sync"}; });
    store.Stop();
    BOOST_CHECK(!log->GetSummary().persisted);
    store.SetFlushHookForTest({});
}

BOOST_AUTO_TEST_CASE(blocked_flush_never_blocks_record_or_reads)
{
    FakeClocks clocks;
    FlowMeshObservedTimeStore store{Options(m_path_root / "observed_blocked")};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    store.Attach(log, 0, {});
    std::mutex mutex;
    std::condition_variable cv;
    bool entered{false}, release{false};
    store.SetFlushHookForTest([&] {
        std::unique_lock lock{mutex};
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    });
    log->Record(0, EntryHash(0), ObservedDelay::NONE, Tick(0));
    store.Start();
    {
        std::unique_lock lock{mutex};
        BOOST_REQUIRE(cv.wait_for(lock, std::chrono::seconds{10}, [&] { return entered; }));
    }
    // The flush thread is stalled inside its disk write. Record (the
    // runtime's commit path) and reads still complete at once.
    const auto started{std::chrono::steady_clock::now()};
    for (uint64_t i{1}; i < 200; ++i) {
        clocks.Advance(5);
        BOOST_CHECK(log->Record(i, EntryHash(i), ObservedDelay::NONE, Tick(i)).mode == ObservedMode::LIVE);
        BOOST_CHECK(!log->Candles(CandleInterval::M1, std::nullopt, 10).candles.empty());
    }
    BOOST_CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds{5});
    {
        std::lock_guard lock{mutex};
        release = true;
    }
    cv.notify_all();
    store.SetFlushHookForTest({});
    store.Stop();
    BOOST_CHECK(log->GetSummary().persisted);
}

BOOST_AUTO_TEST_CASE(async_attach_during_blocked_flush_preserves_new_rows)
{
    const auto path{m_path_root / "observed_async_attach"};
    const flowmesh::MarketId second_market{uint256{"00000000000000000000000000000000000000000000000000000000000000e2"}};
    FakeClocks clocks;
    // A previous process retained one historical row for the new market.
    {
        FlowMeshObservedTimeStore seed{Options(path)};
        std::string error;
        BOOST_REQUIRE(seed.Open(error));
        const auto history{std::make_shared<FlowMeshObservedTimeLog>(TEST_DOMAIN, second_market, clocks.Get())};
        seed.Attach(history, 0, {});
        history->Record(0, EntryHash(0), ObservedDelay::NONE, Tick(0));
        seed.Stop();
    }
    FlowMeshObservedTimeStore store{Options(path)};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    store.Attach(log, 0, {});
    std::mutex mutex;
    std::condition_variable cv;
    bool entered{false}, release{false};
    store.SetFlushHookForTest([&] {
        std::unique_lock lock{mutex};
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    });
    log->Record(0, EntryHash(0), ObservedDelay::NONE, Tick(0));
    store.Start();
    {
        std::unique_lock lock{mutex};
        BOOST_REQUIRE(cv.wait_for(lock, std::chrono::seconds{10}, [&] { return entered; }));
    }
    const auto fresh{std::make_shared<FlowMeshObservedTimeLog>(TEST_DOMAIN, second_market, clocks.Get())};
    auto registration{std::async(std::launch::async, [&] { store.AttachAsync(fresh, 1, Known(0, 1)); })};
    const bool registered{registration.wait_for(std::chrono::seconds{1}) == std::future_status::ready};
    BOOST_CHECK(registered);
    // A stalled old-market flush cannot prevent the new runtime recording.
    clocks.Advance(61'000);
    fresh->Record(1, EntryHash(1), ObservedDelay::NONE, Tick(1));
    BOOST_CHECK_EQUAL(fresh->Lookup(Known(0, 2)).size(), 1U);
    if (registered) {
        BOOST_CHECK(!fresh->GetSummary().persisted);
        BOOST_CHECK(store.ReadCandles(second_market, CandleInterval::M1, 0, T0 + 120'000, 100).empty());
    }
    {
        std::lock_guard lock{mutex};
        release = true;
    }
    cv.notify_all();
    registration.get();
    store.SetFlushHookForTest({});
    // Stop also drains an attachment not yet serviced by the worker.
    store.Stop();
    const auto rows{fresh->Lookup(Known(0, 2))};
    BOOST_REQUIRE_EQUAL(rows.size(), 1U);
    BOOST_CHECK_EQUAL(rows.front().sequence, 1U);
    BOOST_CHECK(fresh->GetSummary().persisted);
    BOOST_CHECK(fresh->TakePending().Empty());
    const auto disk{store.ReadCandles(second_market, CandleInterval::M1, T0, T0 + 120'000, 100)};
    BOOST_REQUIRE_EQUAL(disk.size(), 2U);
    BOOST_CHECK_EQUAL(disk.front().last_sequence, 0U);
    BOOST_CHECK_EQUAL(disk.back().last_sequence, 1U);
}

BOOST_AUTO_TEST_CASE(async_attachments_coalesce_per_market)
{
    FakeClocks clocks;
    FlowMeshObservedTimeStore store{Options(m_path_root / "observed_async_coalesce")};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    std::vector<std::shared_ptr<FlowMeshObservedTimeLog>> logs;
    for (CAmount price{10}; price < 20; ++price) {
        const auto log{MakeLog(clocks)};
        store.AttachAsync(log, 0, {});
        log->Record(0, EntryHash(0), ObservedDelay::NONE, Tick(0, price));
        logs.push_back(log);
    }
    unsigned writes{0};
    store.SetFlushHookForTest([&] { ++writes; });
    store.FlushNow(false);
    // Ten queued replacements retain only the latest handle for this market.
    BOOST_CHECK_EQUAL(writes, 1U);
    const auto disk{store.ReadCandles(MARKET, CandleInterval::M1, T0, T0 + 60'000, 10)};
    BOOST_REQUIRE_EQUAL(disk.size(), 1U);
    BOOST_CHECK_EQUAL(disk.front().close, 19);
    BOOST_CHECK(logs.back()->TakePending().Empty());
    BOOST_CHECK(!logs.front()->TakePending().Empty());
    store.SetFlushHookForTest({});
}

BOOST_AUTO_TEST_CASE(candle_page_json_exact_volumes_and_labels)
{
    FakeClocks clocks;
    const auto log{MakeLog(clocks)};
    log->Record(0, EntryHash(0), ObservedDelay::NONE, std::nullopt);
    clocks.Advance(61'000);
    log->Record(1, EntryHash(1), ObservedDelay::NONE, Tick(1, MAX_MONEY, MAX_MONEY));
    auto page{log->Candles(CandleInterval::M1, std::nullopt, 10)};
    page.source = "this_node";
    const auto json{node::FlowMeshObservedCandlePageJson(page)};
    BOOST_CHECK_EQUAL(json["source_kind"].get_str(), "operator_observed");
    BOOST_CHECK(!json["certified"].get_bool());
    BOOST_CHECK_EQUAL(json["interval"].get_str(), "1m");
    BOOST_CHECK_EQUAL(json["interval_ms"].getInt<int64_t>(), 60'000);
    const auto& candles{json["candles"]};
    BOOST_REQUIRE_EQUAL(candles.size(), 2U);
    // No prices without a clearing.
    BOOST_CHECK(!candles[0].exists("open"));
    BOOST_CHECK_EQUAL(candles[0]["base_volume"].get_str(), "0");
    BOOST_CHECK_EQUAL(candles[1]["open"].getInt<int64_t>(), MAX_MONEY);
    BOOST_CHECK_EQUAL(candles[1]["base_volume"].get_str(), "662200000000000000");
    // MAX_MONEY squared needs more than 64 bits and is exact.
    BOOST_CHECK_EQUAL(candles[1]["quote_volume"].get_str(), "438508840000000000000000000000000000");
    BOOST_CHECK_EQUAL(candles[1]["flags"].size(), 0U);
    BOOST_CHECK_EQUAL(candles[0]["flags"][0].get_str(), "seam_before");
}

BOOST_AUTO_TEST_SUITE_END()
