// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

//! The node's display-only observed-time log and its separate database.

#include <node/flowmesh_observed_time_log.h>

#include <node/flowmesh_client.h>

#include <dbwrapper.h>
#include <serialize.h>
#include <test/util/setup_common.h>
#include <util/fs.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
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

//! The store's own row and candle layouts (flowmesh_observed_time_log.cpp),
//! to write what it never writes itself.
struct RawRowKey {
    uint8_t prefix{'r'};
    uint256 market;
    uint64_t sequence{0};
    SERIALIZE_METHODS(RawRowKey, obj) { READWRITE(obj.prefix, obj.market, Using<BigEndianFormatter<8>>(obj.sequence)); }
};
struct RawRowValue {
    uint256 hash;
    uint8_t mode{0};
    int64_t ms{0};
    uint8_t flags{0};
    uint8_t delay{0};
    SERIALIZE_METHODS(RawRowValue, obj) { READWRITE(obj.hash, obj.mode, obj.ms, obj.flags, obj.delay); }
};
struct RawCandleKey {
    uint8_t prefix{'c'};
    uint256 market;
    uint8_t interval{0};
    uint64_t open_ms{0};
    SERIALIZE_METHODS(RawCandleKey, obj) { READWRITE(obj.prefix, obj.market, obj.interval, Using<BigEndianFormatter<8>>(obj.open_ms)); }
};
struct RawCandleValue {
    uint64_t first_sequence{0}, last_sequence{0};
    uint32_t entries{0}, clearings{0};
    int64_t open{0}, high{0}, low{0}, close{0};
    uint64_t base_low{0}, base_high{0}, quote_low{0}, quote_high{0};
    uint8_t flags{0};
    SERIALIZE_METHODS(RawCandleValue, obj)
    {
        READWRITE(obj.first_sequence, obj.last_sequence, obj.entries, obj.clearings, obj.open, obj.high, obj.low,
                  obj.close, obj.base_low, obj.base_high, obj.quote_low, obj.quote_high, obj.flags);
    }
};

std::vector<std::pair<uint64_t, uint256>> Known(const uint64_t from, const uint64_t to)
{
    std::vector<std::pair<uint64_t, uint256>> out;
    for (uint64_t i{from}; i < to; ++i) out.emplace_back(i, EntryHash(i));
    return out;
}

const flowmesh::MarketId OTHER_MARKET{uint256{"00000000000000000000000000000000000000000000000000000000000000e2"}};

//! Three recorded entries a minute apart from T0 (1m candles at T0, +1 and
//! +2 minutes), then what the store never writes itself: 1m bucket starts
//! at the display bound, at 2^63 and at 2^64 - 1 (negative as int64), one
//! not aligned, one at 2^63 for OTHER_MARKET, and entry 2's row stamped at
//! the bound.
void WriteOutOfBoundTimes(const fs::path& path, FakeClocks& clocks)
{
    {
        FlowMeshObservedTimeStore store{Options(path)};
        std::string error;
        BOOST_REQUIRE(store.Open(error));
        const auto log{MakeLog(clocks)};
        store.Attach(log, 0, {});
        for (uint64_t i{0}; i < 3; ++i) {
            log->Record(i, EntryHash(i), ObservedDelay::NONE, Tick(i));
            clocks.Advance(60'000);
        }
        store.Stop();
    }
    CDBWrapper db{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
    const RawCandleValue value{1, 1, 1, 1, 10, 10, 10, 10, 1, 0, 10, 0, 0};
    CDBBatch batch{db};
    for (const uint64_t open : {static_cast<uint64_t>(flowmesh::OBSERVED_MS_LIMIT), uint64_t{1} << 63,
                                std::numeric_limits<uint64_t>::max(), static_cast<uint64_t>(T0 + 30'000)}) {
        batch.Write(RawCandleKey{'c', MARKET, 0, open}, value);
    }
    batch.Write(RawCandleKey{'c', OTHER_MARKET, 0, uint64_t{1} << 63}, value);
    batch.Write(RawRowKey{'r', MARKET, 2}, RawRowValue{EntryHash(2), static_cast<uint8_t>(ObservedMode::LIVE), flowmesh::OBSERVED_MS_LIMIT, 0, 0});
    db.WriteBatch(batch, true);
}

//! One Record: its clock change first, then its inputs.
struct ScriptStep {
    int64_t wall{1'000}, steady{1'000};
    ObservedDelay delay{ObservedDelay::NONE};
    bool priced{true};
    bool suspect{false};
    //! Sequences left out before this entry.
    uint64_t skip{0};
};

//! `count` entries: a delayed one, a clamp inside the slew, an unpriced one,
//! missing sequences, an hour back (untimed), suspect, a forward step, then
//! ordinary entries a second apart.
std::vector<ScriptStep> Script(const size_t count)
{
    std::vector<ScriptStep> out{
        {1'000, 1'000},
        {2'000, 2'000, ObservedDelay::CATCHUP},
        {-400, 100},
        {5'000, 5'000, ObservedDelay::NONE, false},
        {5'000, 5'000, ObservedDelay::NONE, true, false, 3},
        {-3'600'000, 100},
        {1'000, 1'000, ObservedDelay::NONE, true, true},
        {3'700'000, 1'000},
    };
    out.resize(count);
    return out;
}

//! Plays `script` from `sequence`; the first `unrecorded` steps move the
//! clocks without a Record. Returns the last stamp Record returned.
std::optional<flowmesh::ObservedStamp> Play(FlowMeshObservedTimeLog& log, FakeClocks& clocks, const std::vector<ScriptStep>& script,
                                            uint64_t& sequence, const size_t unrecorded = 0)
{
    std::optional<flowmesh::ObservedStamp> last;
    for (size_t i{0}; i < script.size(); ++i) {
        const auto& step{script[i]};
        clocks.wall += step.wall;
        clocks.steady_ms += step.steady;
        log.SetClockSuspect(step.suspect);
        sequence += step.skip;
        if (i >= unrecorded) {
            last = log.Record(sequence, EntryHash(sequence), step.delay,
                              step.priced ? std::optional{Tick(sequence, 150 + static_cast<CAmount>(sequence % 7))} : std::nullopt);
        }
        ++sequence;
    }
    return last;
}

//! Everything a reader or the flush can take from a log.
struct LogView {
    std::vector<flowmesh::ObservedStamp> rows;
    std::array<std::vector<flowmesh::ObservedCandle>, flowmesh::CANDLE_INTERVALS.size()> candles;
    std::array<std::optional<int64_t>, flowmesh::CANDLE_INTERVALS.size()> oldest;
    std::optional<uint64_t> recorded_since;
    uint64_t rows_not_persisted{0};
    std::deque<flowmesh::ObservedStamp> pending_rows;
    std::map<std::pair<uint8_t, int64_t>, flowmesh::ObservedCandle> pending_candles;
};

LogView View(FlowMeshObservedTimeLog& log, const uint64_t end)
{
    LogView out;
    out.rows = log.Lookup(Known(0, end));
    for (const auto interval : flowmesh::CANDLE_INTERVALS) {
        const auto page{log.Candles(interval, std::nullopt, flowmesh::OBSERVED_CANDLE_PAGE_MAX)};
        BOOST_CHECK(page.available);
        out.candles[static_cast<size_t>(interval)] = page.candles;
        out.oldest[static_cast<size_t>(interval)] = log.OldestMemoryOpen(interval);
    }
    const auto summary{log.GetSummary()};
    BOOST_CHECK(!summary.loading);
    out.recorded_since = summary.recorded_since;
    out.rows_not_persisted = summary.rows_not_persisted;
    auto pending{log.TakePending()};
    out.pending_rows = std::move(pending.rows);
    out.pending_candles = std::move(pending.candles);
    return out;
}

void CheckSameView(const LogView& got, const LogView& want, const std::string& name, const uint64_t extra_not_persisted = 0)
{
    BOOST_CHECK_MESSAGE(got.rows == want.rows, name << ": stamps differ (" << got.rows.size() << " vs " << want.rows.size() << ")");
    BOOST_CHECK_MESSAGE(got.candles == want.candles, name << ": candles differ");
    BOOST_CHECK_MESSAGE(got.oldest == want.oldest, name << ": oldest memory candles differ");
    BOOST_CHECK_MESSAGE(got.recorded_since == want.recorded_since, name << ": recorded_since differs");
    BOOST_CHECK_MESSAGE(got.rows_not_persisted == want.rows_not_persisted + extra_not_persisted,
                        name << ": rows_not_persisted " << got.rows_not_persisted << " vs " << want.rows_not_persisted);
    BOOST_CHECK_MESSAGE(got.pending_rows == want.pending_rows, name << ": pending rows differ");
    BOOST_CHECK_MESSAGE(got.pending_candles == want.pending_candles, name << ": pending candles differ");
}

//! A market's retained history, as the store reads it: 40 entries seven
//! seconds apart from T0, then the clocks at its end.
struct History {
    std::vector<flowmesh::ObservedStamp> rows;
    std::array<std::vector<flowmesh::ObservedCandle>, flowmesh::CANDLE_INTERVALS.size()> candles;
    FakeClocks clocks;
};

void MakeHistory(History& out)
{
    const auto source{MakeLog(out.clocks)};
    for (uint64_t i{0}; i < 40; ++i) {
        source->Record(i, EntryHash(i), ObservedDelay::NONE, Tick(i, 100 + static_cast<CAmount>(i)));
        out.clocks.Advance(7'000);
    }
    out.rows = source->Lookup(Known(0, 40));
    for (const auto interval : flowmesh::CANDLE_INTERVALS) {
        out.candles[static_cast<size_t>(interval)] = source->Candles(interval, std::nullopt, flowmesh::OBSERVED_CANDLE_PAGE_MAX).candles;
    }
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

BOOST_AUTO_TEST_CASE(far_future_wall_clock_is_untimed_and_never_the_floor)
{
    FakeClocks clocks;
    clocks.wall = flowmesh::OBSERVED_MS_LIMIT;
    const auto log{MakeLog(clocks)};
    // A clock at or past 10000-01-01 is no time: untimed and flagged.
    auto stamp{log->Record(0, EntryHash(0), ObservedDelay::NONE, Tick(0))};
    BOOST_REQUIRE(stamp.mode == ObservedMode::UNTIMED);
    BOOST_CHECK(stamp.delay == ObservedDelay::CLOCK_BACK);
    BOOST_CHECK_EQUAL(stamp.ms, 0);
    BOOST_CHECK(stamp.flags & flowmesh::OBSERVED_CLOCK_SUSPECT);
    BOOST_CHECK(log->Candles(CandleInterval::M1, std::nullopt, 10).candles.empty());
    // It never became the monotonic floor: once corrected, the next entry
    // has a time again (a step from the previous stamp).
    clocks.wall = T0;
    clocks.steady_ms += 1000;
    stamp = log->Record(1, EntryHash(1), ObservedDelay::NONE, Tick(1));
    BOOST_REQUIRE(stamp.mode == ObservedMode::LIVE);
    BOOST_CHECK_EQUAL(stamp.ms, T0);
    BOOST_CHECK(stamp.flags & flowmesh::OBSERVED_CLOCK_STEP);
    const auto minutes{log->Candles(CandleInterval::M1, std::nullopt, 10).candles};
    BOOST_REQUIRE_EQUAL(minutes.size(), 1U);
    BOOST_CHECK_EQUAL(minutes.back().open_ms, T0);
    BOOST_CHECK(minutes.back().flags & flowmesh::CANDLE_PARTIAL);
    // Neither extreme clock overflows the step check; both are untimed.
    uint64_t sequence{2};
    for (const int64_t wall : {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()}) {
        clocks.wall = wall;
        clocks.steady_ms += 1000;
        stamp = log->Record(sequence, EntryHash(sequence), ObservedDelay::NONE, Tick(sequence));
        ++sequence;
        BOOST_CHECK(stamp.mode == ObservedMode::UNTIMED);
        BOOST_CHECK_EQUAL(stamp.ms, 0);
    }
    clocks.wall = T0 + 5'000;
    clocks.steady_ms += 1000;
    stamp = log->Record(sequence, EntryHash(sequence), ObservedDelay::NONE, Tick(sequence));
    BOOST_CHECK(stamp.mode == ObservedMode::LIVE);
    BOOST_CHECK_EQUAL(stamp.ms, T0 + 5'000);
}

BOOST_AUTO_TEST_CASE(stored_times_past_the_display_bound_are_never_loaded)
{
    const auto path{m_path_root / "observed_bound_load"};
    FakeClocks clocks;
    WriteOutOfBoundTimes(path, clocks);
    FlowMeshObservedTimeStore store{Options(path)};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    store.Attach(log, 3, Known(0, 3));
    // The row at the bound is not read back, and none of those candles is.
    BOOST_CHECK_EQUAL(log->Lookup(Known(0, 3)).size(), 2U);
    const auto page{log->Candles(CandleInterval::M1, std::nullopt, 500)};
    BOOST_CHECK_EQUAL(page.candles.size(), 3U);
    for (const auto& candle : page.candles) BOOST_CHECK(flowmesh::ObservedTimeInRange(candle.open_ms) && candle.open_ms % 60'000 == 0);
    BOOST_CHECK_EQUAL(page.before_ms, T0 + 180'000);
    // The recorder's floor is its last real time: the next entry is live.
    clocks.Advance(60'000);
    const auto stamp{log->Record(3, EntryHash(3), ObservedDelay::NONE, Tick(3))};
    BOOST_CHECK(stamp.mode == ObservedMode::LIVE);
    BOOST_CHECK_EQUAL(stamp.ms, clocks.wall.load());
    store.Stop();
}

BOOST_AUTO_TEST_CASE(stored_keys_past_the_display_bound_end_the_disk_scan)
{
    const auto path{m_path_root / "observed_bound_scan"};
    FakeClocks clocks;
    WriteOutOfBoundTimes(path, clocks);
    FlowMeshObservedTimeStore store{Options(path)};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    // Keys are compared unsigned: a key at or past 2^63 ends the scan and is
    // never an older candle; one at the bound or unaligned is skipped.
    bool older{true};
    const auto disk{store.ReadCandles(MARKET, CandleInterval::M1, 0, std::numeric_limits<int64_t>::max(), 500, &older)};
    BOOST_CHECK(!older);
    BOOST_CHECK_EQUAL(disk.size(), 3U);
    for (const auto& candle : disk) BOOST_CHECK(flowmesh::ObservedTimeInRange(candle.open_ms) && candle.open_ms % 60'000 == 0);
    older = true;
    BOOST_CHECK(store.ReadCandles(OTHER_MARKET, CandleInterval::M1, 0, flowmesh::OBSERVED_MS_LIMIT, 500, &older).empty());
    BOOST_CHECK(!older);
}

// A log given its history after entries were recorded ends exactly as one
// loaded before them with the same clock readings (the same stamps, memory
// candles, candles and rows to persist, recorded_since), whether the clock
// ran on or stood an hour behind the reloaded stamps. Beyond RING waiting
// entries the oldest are a gap, counted as not persisted. Until then every
// read is unavailable, never partial.
BOOST_AUTO_TEST_CASE(a_log_loaded_after_its_entries_equals_one_loaded_before_them)
{
    History history;
    MakeHistory(history);
    for (const int64_t restart : {int64_t{0}, int64_t{-3'600'000}}) {
        for (const size_t count : {size_t{0}, size_t{1}, size_t{13}, FlowMeshObservedTimeLog::RING, FlowMeshObservedTimeLog::RING + 1}) {
            const auto name{"restart " + std::to_string(restart) + ", " + std::to_string(count) + " entries"};
            const auto script{Script(count)};
            const size_t dropped{count > FlowMeshObservedTimeLog::RING ? count - FlowMeshObservedTimeLog::RING : 0};

            // Reference: loaded first, then the entries (those that will be
            // dropped move the clocks only).
            FakeClocks reference_clocks;
            reference_clocks.wall = history.clocks.wall + restart;
            reference_clocks.steady_ms = history.clocks.steady_ms.load();
            const auto reference{MakeLog(reference_clocks)};
            reference->Load(history.rows, history.candles, std::nullopt);
            uint64_t reference_sequence{40};
            Play(*reference, reference_clocks, script, reference_sequence, dropped);

            FakeClocks clocks;
            clocks.wall = history.clocks.wall + restart;
            clocks.steady_ms = history.clocks.steady_ms.load();
            const auto log{MakeLog(clocks)};
            log->BeginLoad();
            uint64_t sequence{40};
            // Stamped later: what Record returns meanwhile is provisional.
            const auto provisional{Play(*log, clocks, script, sequence)};
            BOOST_CHECK(!provisional || provisional->mode == ObservedMode::UNTIMED);
            const auto summary{log->GetSummary()};
            BOOST_CHECK(summary.loading);
            BOOST_CHECK(!summary.recorded_since);
            BOOST_CHECK(log->Lookup(Known(0, sequence)).empty());
            const auto page{log->Candles(CandleInterval::M1, std::nullopt, 300)};
            BOOST_CHECK(!page.available);
            BOOST_CHECK_EQUAL(page.unavailable_reason, FlowMeshObservedTimeLog::LOADING_REASON);
            BOOST_CHECK(page.candles.empty());
            BOOST_CHECK(!log->OldestMemoryOpen(CandleInterval::M1));
            BOOST_CHECK(log->TakePending().Empty());
            log->FinishLoad(history.rows, history.candles, std::nullopt);

            BOOST_CHECK_EQUAL(sequence, reference_sequence);
            CheckSameView(View(*log, sequence), View(*reference, reference_sequence), name, dropped);
        }
    }
}

// FinishLoad replays the waiting entries off the mutex while more arrive:
// the result is still exactly the load followed by every entry in order.
BOOST_AUTO_TEST_CASE(a_load_finishing_while_entries_arrive_equals_one_loaded_before_them)
{
    History history;
    MakeHistory(history);
    const auto script{Script(3'000)};
    FakeClocks reference_clocks;
    reference_clocks.wall = history.clocks.wall.load();
    reference_clocks.steady_ms = history.clocks.steady_ms.load();
    const auto reference{MakeLog(reference_clocks)};
    reference->Load(history.rows, history.candles, std::nullopt);
    uint64_t reference_sequence{40};
    Play(*reference, reference_clocks, script, reference_sequence);
    const auto want{View(*reference, reference_sequence)};

    for (int run{0}; run < 5; ++run) {
        FakeClocks clocks;
        clocks.wall = history.clocks.wall.load();
        clocks.steady_ms = history.clocks.steady_ms.load();
        const auto log{MakeLog(clocks)};
        log->BeginLoad();
        uint64_t sequence{40};
        // Fewer than RING entries in all, so none is dropped however the
        // two threads interleave.
        std::thread recorder{[&] { Play(*log, clocks, script, sequence); }};
        std::this_thread::sleep_for(std::chrono::microseconds{200 * run});
        log->FinishLoad(history.rows, history.candles, std::nullopt);
        recorder.join();
        BOOST_CHECK_EQUAL(sequence, reference_sequence);
        CheckSameView(View(*log, sequence), want, "run " + std::to_string(run));
    }
}

BOOST_AUTO_TEST_SUITE_END()
