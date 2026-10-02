// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

//! The node's display-only observed-time log and its separate database.

#include <node/flowmesh_observed_time_log.h>

#include <node/flowmesh_client.h>
#include <node/flowmesh_runtime.h>

#include <dbwrapper.h>
#include <serialize.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <util/fs.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

//! `count` entries `step_ms` apart from the clocks' time, loaded and
//! flushed through a store at `path` and stopped cleanly.
void Populate(const fs::path& path, FakeClocks& clocks, const uint64_t count, const int64_t step_ms)
{
    FlowMeshObservedTimeStore store{Options(path)};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    store.Attach(log, 0, {});
    store.FlushNow(false);
    for (uint64_t i{0}; i < count; ++i) {
        log->Record(i, EntryHash(i), ObservedDelay::NONE, Tick(i, 100 + static_cast<CAmount>(i)));
        clocks.Advance(step_ms);
    }
    store.Stop();
}

//! What a store run leaves: the log's stamps, recorded_since and memory
//! candles, the stored candles, and what a restart reloads.
struct StoreRun {
    std::vector<flowmesh::ObservedStamp> rows;
    std::optional<uint64_t> recorded_since;
    std::array<std::vector<flowmesh::ObservedCandle>, flowmesh::CANDLE_INTERVALS.size()> memory, disk, reloaded;
    std::vector<flowmesh::ObservedStamp> reloaded_rows;
};

//! 40 stored entries, a restart with the wall clock `back_ms` behind, then
//! Script(85) with the queued load run before step `load_at` (never:
//! loaded first, the reference; past the end: after every entry).
StoreRun RunWithLoadAt(const fs::path& path, const std::optional<size_t> load_at, const int64_t back_ms)
{
    FakeClocks clocks;
    Populate(path, clocks, 40, 7'000);
    clocks.wall -= back_ms;
    StoreRun out;
    uint64_t sequence{40};
    std::string error;
    {
        FlowMeshObservedTimeStore store{Options(path)};
        BOOST_REQUIRE(store.Open(error));
        const auto log{MakeLog(clocks)};
        store.Attach(log, 40, Known(0, 40));
        if (!load_at) store.FlushNow(false);
        const auto script{Script(85)};
        for (size_t i{0}; i < script.size(); ++i) {
            if (load_at && *load_at == i) store.FlushNow(false);
            (void)Play(*log, clocks, {script[i]}, sequence);
        }
        store.FlushNow(false);
        out.rows = log->Lookup(Known(0, sequence));
        out.recorded_since = log->GetSummary().recorded_since;
        for (const auto interval : flowmesh::CANDLE_INTERVALS) {
            out.memory[static_cast<size_t>(interval)] = log->Candles(interval, std::nullopt, flowmesh::OBSERVED_CANDLE_PAGE_MAX).candles;
        }
        store.Stop();
        for (const auto interval : flowmesh::CANDLE_INTERVALS) {
            out.disk[static_cast<size_t>(interval)] = store.ReadCandles(MARKET, interval, 0, T0 + 10 * 86'400'000LL, flowmesh::OBSERVED_CANDLE_PAGE_MAX);
        }
    }
    FlowMeshObservedTimeStore store{Options(path)};
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    store.Attach(log, sequence, Known(0, sequence));
    store.FlushNow(false);
    out.reloaded_rows = log->Lookup(Known(0, sequence));
    for (const auto interval : flowmesh::CANDLE_INTERVALS) {
        out.reloaded[static_cast<size_t>(interval)] = log->Candles(interval, std::nullopt, flowmesh::OBSERVED_CANDLE_PAGE_MAX).candles;
    }
    store.Stop();
    return out;
}

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

//! A key the store never writes, to pad the database ('A' sorts before the
//! meta keys, 'N' between them and the candles, 'd' between the candles and
//! the rows).
struct RawPadKey {
    uint8_t prefix{'A'};
    uint32_t index{0};
    SERIALIZE_METHODS(RawPadKey, obj) { READWRITE(obj.prefix, Using<BigEndianFormatter<4>>(obj.index)); }
};

//! `count` stored entries under MARKET, then padding so that its meta ('M')
//! and candle ('c') keys sit in data blocks of their own after a first block
//! Open reads, compacted into a table.
void WritePaddedTable(const fs::path& path, FakeClocks& clocks, const uint64_t count)
{
    Populate(path, clocks, count, 1'000);
    CDBWrapper db{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
    CDBBatch batch{db};
    const std::vector<unsigned char> value(200, 0x5a);
    for (const uint8_t pad : {uint8_t{'A'}, uint8_t{'N'}, uint8_t{'d'}}) {
        for (uint32_t i{0}; i < 400; ++i) batch.Write(RawPadKey{pad, i}, value);
    }
    db.WriteBatch(batch, true);
    db.CompactFull();
}

//! Flips one byte of the table block holding MARKET's first `prefix` key
//! (WritePaddedTable): every read of that block then fails its checksum
//! (Corruption), every time. Relies on the store's uncompressed tables and
//! checked reads; false when the layout no longer matches.
bool FlipBlock(const fs::path& path, const uint8_t prefix)
{
    std::vector<char> pattern{static_cast<char>(prefix)};
    for (const auto byte : MARKET) pattern.push_back(static_cast<char>(byte));
    bool flipped{false};
    for (const auto& entry : fs::directory_iterator(path)) {
        if (entry.path().extension() != ".ldb") continue;
        std::fstream file{entry.path(), std::ios::in | std::ios::out | std::ios::binary};
        std::vector<char> bytes{std::istreambuf_iterator<char>{file}, {}};
        const auto at{std::search(bytes.begin(), bytes.end(), pattern.begin(), pattern.end())};
        if (at == bytes.end()) continue;
        const auto offset{std::distance(bytes.begin(), at) + 5};
        const char byte{static_cast<char>(bytes[offset] ^ 0x40)};
        file.clear();
        file.seekp(offset);
        file.write(&byte, 1);
        flipped = true;
        break;
    }
    return flipped;
}

//! A real read error under MARKET: WritePaddedTable, then the block of its
//! `prefix` keys ('M' meta, 'c' candles) made unreadable (FlipBlock).
void WriteUnreadableBlock(const fs::path& path, FakeClocks& clocks, const uint64_t count, const uint8_t prefix)
{
    WritePaddedTable(path, clocks, count);
    BOOST_REQUIRE(FlipBlock(path, prefix));
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
    // The load is queued: run it (the flush thread's pass) before reading.
    store.FlushNow(false);
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
    store.FlushNow(false);
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
        store.FlushNow(false);
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
    {
        // Erased, not hidden: a reopened database never serves them either.
        FlowMeshObservedTimeStore store{Options(path)};
        std::string error;
        BOOST_REQUIRE(store.Open(error));
        const auto log{MakeLog(clocks)};
        store.Attach(log, 12, Known(0, 12));
        store.FlushNow(false);
        size_t served{0};
        for (const auto interval : flowmesh::CANDLE_INTERVALS) {
            for (const auto& candle : store.ReadCandles(MARKET, interval, 0, T0 + 86'400'000, 100)) {
                BOOST_CHECK_LT(candle.last_sequence, 12U);
                ++served;
            }
        }
        BOOST_CHECK_EQUAL(served, 0U);
        store.Stop();
    }
    FlowMeshObservedTimeStore store{Options(path)};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    auto known{Known(0, 12)};
    known[5].second = EntryHash(5, 7);
    store.Attach(log, 12, known);
    // Without the load, the checks below would pass without testing it.
    store.FlushNow(false);
    BOOST_CHECK(log->Lookup(Known(0, 12)).empty());
    BOOST_CHECK(log->Candles(CandleInterval::M1, std::nullopt, 10).candles.empty());
    BOOST_CHECK(!log->GetSummary().recorded_since);
}

// The one exception to "a stored bucket is continued, never replaced": a
// bucket holding an entry at or beyond the store's head is erased whole by
// the load (a candle cannot be cut back), so the entries recorded since
// begin it again. Its candle undercounts (the kept entries before the head
// are not in it) and is marked as a seam: seam_before when its first entry
// is the first after the restart, partial when an untimed one came first.
BOOST_AUTO_TEST_CASE(a_bucket_erased_by_the_load_restarts_as_a_seam)
{
    for (const bool clock_back : {false, true}) {
        BOOST_TEST_CONTEXT("clock_back=" << clock_back) {
            const auto path{m_path_root / (clock_back ? "observed_restarted_back" : "observed_restarted")};
            FakeClocks clocks;
            // Entries 0..19, a second apart: all in the 1m bucket at T0.
            Populate(path, clocks, 20, 1'000);
            FlowMeshObservedTimeStore store{Options(path)};
            std::string error;
            BOOST_REQUIRE(store.Open(error));
            const auto log{MakeLog(clocks)};
            store.Attach(log, 12, Known(0, 12));
            store.FlushNow(false);
            BOOST_REQUIRE(!log->GetSummary().loading);
            BOOST_CHECK_EQUAL(log->Lookup(Known(0, 12)).size(), 12U);
            BOOST_CHECK(log->Candles(CandleInterval::M1, std::nullopt, 10).candles.empty());
            uint64_t first{12};
            if (clock_back) {
                clocks.wall -= 3'600'000;
                BOOST_CHECK(log->Record(first, EntryHash(first), ObservedDelay::NONE, Tick(first)).mode == ObservedMode::UNTIMED);
                ++first;
                clocks.Advance(3'600'000);
            }
            clocks.Advance(1'000);
            BOOST_CHECK(log->Record(first, EntryHash(first), ObservedDelay::NONE, Tick(first)).mode == ObservedMode::LIVE);
            const auto page{log->Candles(CandleInterval::M1, std::nullopt, 10)};
            BOOST_REQUIRE_EQUAL(page.candles.size(), 1U);
            const auto& candle{page.candles.front()};
            BOOST_CHECK_EQUAL(candle.open_ms, T0);
            BOOST_CHECK_EQUAL(candle.first_sequence, first);
            BOOST_CHECK_EQUAL(candle.entries, 1U);
            BOOST_CHECK_EQUAL((candle.flags & flowmesh::CANDLE_SEAM_BEFORE) != 0, !clock_back);
            BOOST_CHECK_EQUAL((candle.flags & flowmesh::CANDLE_PARTIAL) != 0, clock_back);
            store.Stop();
        }
    }
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

// What a market's times lost from persistence is operator detail of this
// node's own market-data section (storage), with the persisted flag; a
// section without this node's log has none.
BOOST_AUTO_TEST_CASE(persistence_losses_are_reported_in_this_nodes_section)
{
    FakeClocks clocks;
    const auto log{MakeLog(clocks)};
    // A minute apart: both the row queue and the one-minute candles overflow.
    const uint64_t total{FlowMeshObservedTimeLog::PENDING_ROWS_MAX + 10};
    for (uint64_t i{0}; i < total; ++i) {
        log->Record(i, EntryHash(i), ObservedDelay::NONE, Tick(i));
        clocks.Advance(60'000);
    }
    log->SetPersisted(false);
    const auto summary{log->GetSummary()};
    BOOST_REQUIRE_EQUAL(summary.rows_not_persisted, 10U);
    BOOST_REQUIRE_GT(summary.candles_not_persisted, 0U);
    flowmesh::MarketData data;
    node::FillObservedTimes(data, log.get());
    BOOST_REQUIRE(data.observed);
    BOOST_CHECK(data.observed->available);
    BOOST_REQUIRE(data.observed->storage);
    BOOST_CHECK(!data.observed->storage->persisted);
    BOOST_CHECK_EQUAL(data.observed->storage->rows_not_persisted, 10U);
    BOOST_CHECK_EQUAL(data.observed->storage->candles_not_persisted, summary.candles_not_persisted);
    const auto json{node::FlowMeshClientMarketDataJson(data)};
    const auto& storage{json["observed_time"]["storage"]};
    BOOST_REQUIRE(storage.isObject());
    BOOST_CHECK(!storage["persisted"].get_bool());
    BOOST_CHECK_EQUAL(storage["rows_not_persisted"].getInt<uint64_t>(), 10U);
    BOOST_CHECK_EQUAL(storage["candles_not_persisted"].getInt<uint64_t>(), summary.candles_not_persisted);
    log->SetPersisted(true);
    data.observed.reset();
    node::FillObservedTimes(data, log.get());
    BOOST_REQUIRE(data.observed && data.observed->storage);
    BOOST_CHECK(data.observed->storage->persisted);
    data.observed.reset();
    node::FillObservedTimes(data, nullptr);
    BOOST_REQUIRE(data.observed);
    BOOST_CHECK(!data.observed->storage);
    BOOST_CHECK(!node::FlowMeshClientMarketDataJson(data)["observed_time"].exists("storage"));
}

// A flush that never drains (a stalled disk) for three days of one entry a
// minute: each interval keeps at most its memory window + 1 unflushed
// snapshots, the newest, and the drop is counted. Memory is untouched.
BOOST_AUTO_TEST_CASE(pending_candles_stay_within_each_memory_window_while_nothing_drains)
{
    FakeClocks clocks;
    const auto log{MakeLog(clocks)};
    const uint64_t total{3 * flowmesh::CandleMemoryWindow(CandleInterval::M1)};
    for (uint64_t i{0}; i < total; ++i) {
        log->Record(i, EntryHash(i), ObservedDelay::NONE, Tick(i));
        clocks.Advance(60'000);
    }
    // Only the one-minute candles outgrow their budget in three days.
    BOOST_CHECK_EQUAL(log->GetSummary().candles_not_persisted, total - FlowMeshObservedTimeLog::PendingCandlesMax(CandleInterval::M1));
    const auto pending{log->TakePending()};
    size_t bound{0};
    for (const auto interval : flowmesh::CANDLE_INTERVALS) {
        const auto id{static_cast<uint8_t>(interval)};
        const auto first{pending.candles.lower_bound({id, std::numeric_limits<int64_t>::min()})};
        const auto end{pending.candles.upper_bound({id, std::numeric_limits<int64_t>::max()})};
        BOOST_CHECK_LE(static_cast<size_t>(std::distance(first, end)), flowmesh::CandleMemoryWindow(interval) + 1);
        bound += flowmesh::CandleMemoryWindow(interval) + 1;
        BOOST_REQUIRE(first != end);
        BOOST_CHECK_EQUAL(first->second.open_ms, *log->OldestMemoryOpen(interval));
        BOOST_CHECK_EQUAL(std::prev(end)->second.last_sequence, total - 1);
    }
    BOOST_CHECK_LE(pending.candles.size(), bound);
    // Only persistence loses old buckets: the newest stamp and candle stay.
    BOOST_CHECK_EQUAL(log->Lookup({{total - 1, EntryHash(total - 1)}}).size(), 1U);
    log->Record(total, EntryHash(total), ObservedDelay::NONE, Tick(total));
    BOOST_CHECK_EQUAL(log->Candles(CandleInterval::M1, std::nullopt, 1).candles.back().last_sequence, total);
}

// The same bound with the store's own thread held inside its disk write
// for three days of entries: nothing drains, nothing grows past it.
BOOST_AUTO_TEST_CASE(a_stalled_flush_thread_never_grows_pending_candles)
{
    FakeClocks clocks;
    std::mutex mutex;
    std::condition_variable cv;
    bool entered{false}, release{false};
    const auto release_hook = [&] {
        {
            std::lock_guard lock{mutex};
            release = true;
        }
        cv.notify_all();
    };
    FlowMeshObservedTimeStore store{Options(m_path_root / "observed_pending_stall")};
    // Declared after the store: a failed check releases the thread before
    // the store's destructor joins it.
    const struct Releaser {
        std::function<void()> release;
        ~Releaser() { release(); }
    } releaser{release_hook};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    store.Attach(log, 0, {});
    store.FlushNow(false);
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
    const uint64_t total{3 * flowmesh::CandleMemoryWindow(CandleInterval::M1)};
    for (uint64_t i{1}; i <= total; ++i) {
        clocks.Advance(60'000);
        log->Record(i, EntryHash(i), ObservedDelay::NONE, Tick(i));
    }
    size_t bound{0};
    for (const auto interval : flowmesh::CANDLE_INTERVALS) bound += FlowMeshObservedTimeLog::PendingCandlesMax(interval);
    BOOST_CHECK_LE(log->TakePending().candles.size(), bound);
    BOOST_CHECK_GT(log->GetSummary().candles_not_persisted, 0U);
    release_hook();
    store.SetFlushHookForTest({});
    store.Stop();
}

// With the flush draining, no snapshot is ever dropped, even when the wall
// clock steps forward past the 1m window between two flushes: the last
// update of the candle that left the window is still flushed.
BOOST_AUTO_TEST_CASE(a_draining_flush_never_drops_a_candle_snapshot)
{
    FakeClocks clocks;
    const auto log{MakeLog(clocks)};
    log->Record(0, EntryHash(0), ObservedDelay::NONE, Tick(0));
    (void)log->TakePending();
    clocks.Advance(10'000);
    log->Record(1, EntryHash(1), ObservedDelay::NONE, Tick(1));
    clocks.wall += 2 * 86'400'000LL;
    clocks.Advance(1'000);
    log->Record(2, EntryHash(2), ObservedDelay::NONE, Tick(2));
    const auto pending{log->TakePending()};
    const auto minute{pending.candles.find({static_cast<uint8_t>(CandleInterval::M1), T0})};
    BOOST_REQUIRE(minute != pending.candles.end());
    BOOST_CHECK_EQUAL(minute->second.last_sequence, 1U);
    BOOST_CHECK_EQUAL(log->GetSummary().candles_not_persisted, 0U);
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

// A market's first failed write is a warning naming it; a broken disk fails
// every pass, so its later failures are debug only. Each market is warned
// once.
BOOST_AUTO_TEST_CASE(a_markets_first_failed_write_is_one_warning)
{
    FakeClocks clocks;
    FlowMeshObservedTimeStore store{Options(m_path_root / "observed_fail_warning")};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    const auto other{std::make_shared<FlowMeshObservedTimeLog>(TEST_DOMAIN, OTHER_MARKET, clocks.Get())};
    store.Attach(log, 0, {});
    store.Attach(other, 0, {});
    store.FlushNow(false);
    std::map<std::string, int> warnings;
    {
        DebugLogHelper counter{"observed times could not be written", [&](const std::string* line) {
            for (const auto& market : {MARKET, OTHER_MARKET}) {
                if (line && line->find(market.GetHex()) != std::string::npos) ++warnings[market.GetHex()];
            }
            return false;
        }};
        store.SetFlushHookForTest([] { throw std::runtime_error{"disk"}; });
        for (uint64_t i{0}; i < 3; ++i) {
            clocks.Advance(1'000);
            log->Record(i, EntryHash(i), ObservedDelay::NONE, Tick(i));
            other->Record(i, EntryHash(i), ObservedDelay::NONE, Tick(i));
            store.FlushNow(false);
            BOOST_CHECK(!log->GetSummary().persisted);
            BOOST_CHECK(!other->GetSummary().persisted);
        }
        store.SetFlushHookForTest({});
    }
    BOOST_CHECK_EQUAL(warnings[MARKET.GetHex()], 1);
    BOOST_CHECK_EQUAL(warnings[OTHER_MARKET.GetHex()], 1);
    log->Record(3, EntryHash(3), ObservedDelay::NONE, Tick(3));
    store.FlushNow(false);
    BOOST_CHECK(log->GetSummary().persisted);
    store.Stop();
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
    store.FlushNow(false);
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
    // Disk pages are served only for a market whose load succeeded, never
    // for one with no attached log here (never attached, or an Attach that
    // failed and left none).
    BOOST_CHECK(store.ReadCandles(MARKET, CandleInterval::M1, 0, std::numeric_limits<int64_t>::max(), 500).empty());
    BOOST_CHECK(store.ReadCandles(OTHER_MARKET, CandleInterval::M1, 0, flowmesh::OBSERVED_MS_LIMIT, 500).empty());
    // So both are attached and loaded first. No stored candle holds an
    // entry at or beyond either head: the load erases none of them.
    const auto log{MakeLog(clocks)};
    store.Attach(log, 3, Known(0, 3));
    const auto other{std::make_shared<FlowMeshObservedTimeLog>(TEST_DOMAIN, OTHER_MARKET, clocks.Get())};
    store.Attach(other, 2, {});
    store.FlushNow(false);
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

// A market start only queues its history: Attach never waits for the flush
// thread, even while that thread is stuck inside its disk write, and a
// candles read meanwhile answers at once. The queued market then records,
// is persisted and reloads.
BOOST_AUTO_TEST_CASE(attach_never_waits_for_a_stalled_flush)
{
    const auto path{m_path_root / "observed_attach_stall"};
    FakeClocks clocks;
    std::string error;
    {
        // Declared before the store, so they outlive it: its destructor's
        // Stop may still run the hook.
        std::mutex mutex;
        std::condition_variable cv;
        bool entered{false}, release{false};
        const auto release_hook = [&] {
            {
                std::lock_guard lock{mutex};
                release = true;
            }
            cv.notify_all();
        };
        FlowMeshObservedTimeStore store{Options(path)};
        // Released on every path before the store stops, so a failed
        // REQUIRE below never leaves Stop joining a thread held in the hook.
        const struct Releaser {
            std::function<void()> release;
            ~Releaser() { release(); }
        } releaser{release_hook};
        BOOST_REQUIRE(store.Open(error));
        const auto busy{MakeLog(clocks)};
        store.Attach(busy, 0, {});
        store.SetFlushHookForTest([&] {
            std::unique_lock lock{mutex};
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release; });
        });
        busy->Record(0, EntryHash(0), ObservedDelay::NONE, Tick(0));
        store.Start();
        {
            std::unique_lock lock{mutex};
            BOOST_REQUIRE(cv.wait_for(lock, std::chrono::seconds{10}, [&] { return entered; }));
        }
        // The flush thread holds its pass inside the disk write.
        const auto starting{std::make_shared<FlowMeshObservedTimeLog>(TEST_DOMAIN, OTHER_MARKET, clocks.Get())};
        auto attach{std::async(std::launch::async, [&] { store.Attach(starting, 0, {}); })};
        // Generous deadlines: a passing run never waits for them, and the
        // failure they catch blocks until the hook is released.
        const bool attached{attach.wait_for(std::chrono::seconds{30}) == std::future_status::ready};
        // A candles read of either market (the queued one's load waits for
        // that stalled disk) answers at once too.
        auto read{std::async(std::launch::async, [&] {
            (void)store.ReadCandles(OTHER_MARKET, CandleInterval::M1, 0, T0 + 3'600'000, 10);
            return store.ReadCandles(MARKET, CandleInterval::M1, 0, T0 + 3'600'000, 10);
        })};
        const bool read_done{read.wait_for(std::chrono::seconds{30}) == std::future_status::ready};
        release_hook();
        attach.get();
        (void)read.get();
        BOOST_CHECK_MESSAGE(attached, "Attach waited for the flush thread's disk write");
        BOOST_CHECK_MESSAGE(read_done, "a candles read waited for the flush thread's disk write");
        // Stamps are final once the queued load ran (serialized with the
        // flush thread's passes).
        store.FlushNow(false);
        BOOST_CHECK(starting->Record(0, EntryHash(0), ObservedDelay::NONE, Tick(0)).mode == ObservedMode::LIVE);
        store.SetFlushHookForTest({});
        store.Stop();
        BOOST_CHECK(starting->GetSummary().persisted);
    }
    FlowMeshObservedTimeStore reopened{Options(path)};
    BOOST_REQUIRE(reopened.Open(error));
    const auto reloaded{std::make_shared<FlowMeshObservedTimeLog>(TEST_DOMAIN, OTHER_MARKET, clocks.Get())};
    reopened.Attach(reloaded, 1, {{0, EntryHash(0)}});
    reopened.FlushNow(false);
    BOOST_CHECK_EQUAL(reloaded->Lookup({{0, EntryHash(0)}}).size(), 1U);
    reopened.Stop();
}

// Whenever the queued load runs (before the first new entry, after one, a
// few, or all of them), the store ends exactly as when the history loaded
// first: the same stamps, recorded_since, memory candles, stored candles
// and reloaded stamps and candles. With the clock an hour behind the
// stored stamps, new entries stay untimed until it passes them, as before.
BOOST_AUTO_TEST_CASE(a_queued_load_equals_the_synchronous_load_whenever_it_runs)
{
    for (const int64_t back : {int64_t{0}, int64_t{3'600'000}}) {
        const auto reference{RunWithLoadAt(m_path_root / fs::PathFromString("observed_equiv_ref_" + std::to_string(back)), std::nullopt, back)};
        BOOST_REQUIRE_EQUAL(reference.rows.size(), 40U + 85U);
        for (const size_t at : {size_t{0}, size_t{1}, size_t{7}, size_t{13}, size_t{85}}) {
            const auto name{"back " + std::to_string(back) + ", load at " + std::to_string(at)};
            const auto run{RunWithLoadAt(m_path_root / fs::PathFromString("observed_equiv_" + std::to_string(back) + "_" + std::to_string(at)), at, back)};
            BOOST_CHECK_MESSAGE(run.rows == reference.rows, name << ": stamps differ (" << run.rows.size() << " vs " << reference.rows.size() << ")");
            BOOST_CHECK_MESSAGE(run.recorded_since == reference.recorded_since, name << ": recorded_since differs");
            BOOST_CHECK_MESSAGE(run.memory == reference.memory, name << ": memory candles differ");
            BOOST_CHECK_MESSAGE(run.disk == reference.disk, name << ": stored candles differ");
            BOOST_CHECK_MESSAGE(run.reloaded_rows == reference.reloaded_rows, name << ": reloaded stamps differ");
            BOOST_CHECK_MESSAGE(run.reloaded == reference.reloaded, name << ": reloaded candles differ");
        }
    }
}

// More than RING entries commit before a slow load runs (the slow-disk case
// the queue exists for). The oldest is a gap, counted as not persisted, and
// the stored day and hour buckets keep the reloaded entries: they are
// continued, never replaced by a candle of new entries only.
BOOST_AUTO_TEST_CASE(more_entries_than_the_ring_before_a_load_never_replace_a_stored_bucket)
{
    const auto path{m_path_root / "observed_ring_overflow"};
    FakeClocks clocks;
    Populate(path, clocks, 28, 10'000);
    const uint64_t extra{FlowMeshObservedTimeLog::RING + 1};
    {
        FlowMeshObservedTimeStore store{Options(path)};
        std::string error;
        BOOST_REQUIRE(store.Open(error));
        const auto log{MakeLog(clocks)};
        store.Attach(log, 28, Known(0, 28));
        for (uint64_t i{28}; i < 28 + extra; ++i) {
            clocks.Advance(10);
            log->Record(i, EntryHash(i), ObservedDelay::NONE, Tick(i, 200));
        }
        store.FlushNow(false);
        clocks.Advance(10);
        log->Record(28 + extra, EntryHash(28 + extra), ObservedDelay::NONE, Tick(28 + extra, 200));
        BOOST_CHECK(log->Lookup(Known(28, 29)).empty());
        BOOST_CHECK_GE(log->GetSummary().rows_not_persisted, 1U);
        for (const auto interval : {CandleInterval::D1, CandleInterval::H1}) {
            const auto memory{log->Candles(interval, std::nullopt, 10).candles};
            BOOST_REQUIRE(!memory.empty());
            BOOST_CHECK_MESSAGE(memory.back().first_sequence == 0, "memory " << flowmesh::CandleIntervalName(interval)
                                                                             << " candle lost the reloaded entries: first_sequence="
                                                                             << memory.back().first_sequence);
        }
        store.Stop();
        for (const auto interval : {CandleInterval::D1, CandleInterval::H1}) {
            const auto disk{store.ReadCandles(MARKET, interval, 0, T0 + 86'400'000, 10)};
            BOOST_REQUIRE(!disk.empty());
            BOOST_CHECK_MESSAGE(disk.back().first_sequence == 0, "stored " << flowmesh::CandleIntervalName(interval)
                                                                           << " candle replaced by new entries only: first_sequence="
                                                                           << disk.back().first_sequence << " entries=" << disk.back().entries);
        }
    }
    // The dropped entry is a gap on disk; the next one is stored.
    CDBWrapper db{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
    BOOST_CHECK(db.Exists(RawRowKey{'r', MARKET, 27}));
    BOOST_CHECK(!db.Exists(RawRowKey{'r', MARKET, 28}));
    BOOST_CHECK(db.Exists(RawRowKey{'r', MARKET, 29}));
}

// Until the queued load ran, every read of the market is unavailable, never
// an available page lacking the history (a reader would hold it as the
// whole window): the log says why, and the store serves no disk candle.
BOOST_AUTO_TEST_CASE(reads_while_the_history_loads_are_unavailable_never_partial)
{
    const auto path{m_path_root / "observed_loading_reads"};
    FakeClocks clocks;
    Populate(path, clocks, 30, 10'000);
    FlowMeshObservedTimeStore store{Options(path)};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    store.Attach(log, 30, Known(0, 30));
    clocks.Advance(5'000);
    log->Record(30, EntryHash(30), ObservedDelay::NONE, Tick(30));
    const auto summary{log->GetSummary()};
    BOOST_CHECK(summary.loading);
    BOOST_CHECK(!summary.recorded_since);
    const auto page{log->Candles(CandleInterval::M1, std::nullopt, 300)};
    BOOST_CHECK(!page.available);
    BOOST_CHECK_EQUAL(page.unavailable_reason, FlowMeshObservedTimeLog::LOADING_REASON);
    BOOST_CHECK(page.candles.empty());
    BOOST_CHECK(log->Lookup(Known(0, 31)).empty());
    bool older{true};
    BOOST_CHECK(store.ReadCandles(MARKET, CandleInterval::M1, 0, T0 + 3'600'000, 100, &older).empty());
    BOOST_CHECK(!older);
    // The market-data section (RPC, HTTPS and the client snapshot) says so.
    flowmesh::MarketData data;
    data.history.entries.push_back({});
    data.history.entries.back().sequence = 3;
    data.history.entries.back().microblock_hash = EntryHash(3);
    node::FillObservedTimes(data, log.get());
    BOOST_REQUIRE(data.observed);
    BOOST_CHECK(!data.observed->available);
    BOOST_CHECK_EQUAL(data.observed->unavailable_reason, FlowMeshObservedTimeLog::LOADING_REASON);
    BOOST_CHECK(!data.observed->recorded_since_sequence);
    BOOST_CHECK(data.observed->rows.empty());
    // Its storage detail is there while it loads (nothing failed yet).
    BOOST_REQUIRE(data.observed->storage);
    BOOST_CHECK(data.observed->storage->persisted);
    store.FlushNow(false);
    BOOST_CHECK(!log->GetSummary().loading);
    data.observed.reset();
    node::FillObservedTimes(data, log.get());
    BOOST_REQUIRE(data.observed);
    BOOST_CHECK(data.observed->available);
    BOOST_CHECK_EQUAL(data.observed->recorded_since_sequence.value_or(99), 0U);
    BOOST_REQUIRE_EQUAL(data.observed->rows.size(), 1U);
    BOOST_CHECK_EQUAL(data.observed->rows.front().sequence, 3U);
    BOOST_CHECK_EQUAL(log->Lookup(Known(0, 31)).size(), 31U);
    BOOST_CHECK_EQUAL(log->GetSummary().recorded_since.value_or(99), 0U);
    const auto loaded{log->Candles(CandleInterval::M1, std::nullopt, 300)};
    BOOST_REQUIRE(loaded.available);
    BOOST_REQUIRE(!loaded.candles.empty());
    BOOST_CHECK_EQUAL(loaded.candles.front().first_sequence, 0U);
    BOOST_CHECK_EQUAL(loaded.candles.back().last_sequence, 30U);
    BOOST_CHECK(!store.ReadCandles(MARKET, CandleInterval::M1, 0, T0 + 3'600'000, 100).empty());
    store.Stop();
}

// The wall clock stands an hour behind the stored stamps after a restart and
// entries commit before the queued load runs. They stay untimed until the
// clock passes the reloaded stamps, as if loaded first: after the next
// restart no LIVE stamp precedes an older one (remote clients would flag it
// implausible).
BOOST_AUTO_TEST_CASE(a_backward_clock_across_a_restart_keeps_stamps_monotonic_whenever_the_load_runs)
{
    const auto path{m_path_root / "observed_backward_restart"};
    FakeClocks clocks;
    Populate(path, clocks, 30, 10'000);
    {
        FlowMeshObservedTimeStore store{Options(path)};
        std::string error;
        BOOST_REQUIRE(store.Open(error));
        const auto log{MakeLog(clocks)};
        store.Attach(log, 30, Known(0, 30));
        clocks.wall -= 3'600'000;
        for (uint64_t i{30}; i < 32; ++i) {
            clocks.Advance(1'000);
            log->Record(i, EntryHash(i), ObservedDelay::NONE, Tick(i));
        }
        store.FlushNow(false);
        for (const auto& row : log->Lookup(Known(30, 32))) BOOST_CHECK(row.mode == ObservedMode::UNTIMED);
        store.Stop();
    }
    FlowMeshObservedTimeStore store{Options(path)};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    store.Attach(log, 32, Known(0, 32));
    store.FlushNow(false);
    const auto rows{log->Lookup(Known(0, 32))};
    BOOST_CHECK_EQUAL(rows.size(), 32U);
    std::optional<int64_t> live;
    bool monotonic{true};
    for (const auto& row : rows) {
        if (row.mode != ObservedMode::LIVE) continue;
        if (live && row.ms < *live) monotonic = false;
        live = row.ms;
    }
    BOOST_CHECK_MESSAGE(monotonic, "a LIVE stamp precedes an older one after the restart");
    store.Stop();
}

// A queued history that contradicts the production store is wiped by its
// load: none of it is served before (the load may still wipe it) or after.
BOOST_AUTO_TEST_CASE(a_queued_contradicted_history_is_never_served_then_wiped)
{
    const auto path{m_path_root / "observed_queued_wipe"};
    FakeClocks clocks;
    Populate(path, clocks, 30, 10'000);
    FlowMeshObservedTimeStore store{Options(path)};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    auto known{Known(0, 30)};
    known[3].second = EntryHash(3, 9);
    store.Attach(log, 30, known);
    BOOST_CHECK(store.ReadCandles(MARKET, CandleInterval::M1, 0, T0 + 3'600'000, 100).empty());
    BOOST_CHECK(!log->Candles(CandleInterval::M1, std::nullopt, 100).available);
    store.FlushNow(false);
    BOOST_CHECK(store.ReadCandles(MARKET, CandleInterval::M1, 0, T0 + 3'600'000, 100).empty());
    BOOST_CHECK(log->Lookup(Known(0, 30)).empty());
    BOOST_CHECK(log->Candles(CandleInterval::M1, std::nullopt, 100).candles.empty());
    BOOST_CHECK(!log->GetSummary().recorded_since);
    store.Stop();
}

// Markets attached from several threads while entries are recorded, disk
// candles read and the flush thread runs every millisecond: every market
// loads, records every entry and is persisted.
BOOST_AUTO_TEST_CASE(concurrent_attach_record_read_and_flush_stress)
{
    FakeClocks clocks;
    FlowMeshObservedTimeStore::Options options{Options(m_path_root / "observed_stress")};
    options.flush_interval = std::chrono::milliseconds{1};
    FlowMeshObservedTimeStore store{options};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    store.Start();
    std::vector<std::shared_ptr<FlowMeshObservedTimeLog>> logs(32);
    std::atomic<size_t> attached{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> attachers;
    for (size_t t{0}; t < 4; ++t) {
        attachers.emplace_back([&, t] {
            for (size_t i{t}; i < logs.size(); i += 4) {
                uint256 id;
                id.data()[0] = static_cast<unsigned char>(0x80 + i);
                logs[i] = std::make_shared<FlowMeshObservedTimeLog>(TEST_DOMAIN, id, clocks.Get());
                store.Attach(logs[i], 0, {});
                ++attached;
                // Paces the attachers only; every check follows Stop.
                std::this_thread::sleep_for(std::chrono::microseconds{300});
            }
        });
    }
    std::thread reader{[&] {
        while (!stop) {
            for (unsigned char b{0x80}; b < 0xa0; ++b) {
                uint256 id;
                id.data()[0] = b;
                (void)store.ReadCandles(id, CandleInterval::M1, 0, T0 + 86'400'000, 50);
            }
        }
    }};
    for (auto& t : attachers) t.join();
    for (uint64_t sequence{0}; sequence < 300; ++sequence) {
        clocks.Advance(1'000);
        for (auto& log : logs) log->Record(sequence, EntryHash(sequence), ObservedDelay::NONE, Tick(sequence));
    }
    stop = true;
    reader.join();
    store.Stop();
    BOOST_CHECK_EQUAL(attached.load(), logs.size());
    for (auto& log : logs) {
        const auto summary{log->GetSummary()};
        BOOST_CHECK(!summary.loading);
        BOOST_CHECK(summary.persisted);
        BOOST_CHECK_EQUAL(log->Lookup(Known(0, 300)).size(), 300U);
    }
    uint256 first;
    first.data()[0] = 0x80;
    BOOST_CHECK(!store.ReadCandles(first, CandleInterval::M1, 0, T0 + 86'400'000, 50).empty());
}

// Start opens the database on the store's own thread, before its first
// flush or load: a market attached before it is loaded and persisted.
BOOST_AUTO_TEST_CASE(the_store_thread_opens_its_database)
{
    const auto path{m_path_root / "observed_thread_open"};
    FakeClocks clocks;
    {
        FlowMeshObservedTimeStore store{Options(path)};
        BOOST_CHECK(!store.IsOpen());
        const auto log{MakeLog(clocks)};
        store.Attach(log, 0, {});
        log->Record(0, EntryHash(0), ObservedDelay::NONE, Tick(0));
        store.Start();
        store.Stop();
        BOOST_CHECK(store.IsOpen());
        BOOST_CHECK(log->GetSummary().persisted);
        BOOST_CHECK_EQUAL(log->Lookup(Known(0, 1)).size(), 1U);
    }
    FlowMeshObservedTimeStore store{Options(path)};
    const auto log{MakeLog(clocks)};
    store.Attach(log, 1, Known(0, 1));
    store.Start();
    store.Stop();
    BOOST_CHECK_EQUAL(log->Lookup(Known(0, 1)).size(), 1U);
    BOOST_CHECK_EQUAL(log->GetSummary().recorded_since.value_or(99), 0U);
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

// A load that hits a real read error (the meta block fails its checksum on
// every read) never restores the market: its disk candles, which still
// hold entries beyond the head, are not served, and its new entries are
// not written beside them. Stop's attempt fails too: the market is then
// memory only (recording goes on), still never served or written.
BOOST_AUTO_TEST_CASE(a_load_that_cannot_read_its_history_never_serves_or_writes_it)
{
    const auto path{m_path_root / "observed_unreadable_meta"};
    FakeClocks clocks;
    WriteUnreadableBlock(path, clocks, 20, 'M');
    {
        FlowMeshObservedTimeStore store{Options(path)};
        std::string error;
        BOOST_REQUIRE(store.Open(error));
        const auto log{MakeLog(clocks)};
        store.Attach(log, 12, Known(0, 12));
        store.FlushNow(false);
        clocks.Advance(1'000);
        log->Record(12, EntryHash(12, 2), ObservedDelay::NONE, std::nullopt);
        store.FlushNow(false);
        BOOST_CHECK(!log->GetSummary().persisted);
        for (const auto interval : flowmesh::CANDLE_INTERVALS) {
            BOOST_CHECK(store.ReadCandles(MARKET, interval, 0, T0 + 86'400'000, 100).empty());
        }
        store.Stop();
        const auto summary{log->GetSummary()};
        BOOST_CHECK(!summary.loading);
        BOOST_CHECK(!summary.persisted);
        BOOST_CHECK_EQUAL(log->Lookup({{12, EntryHash(12, 2)}}).size(), 1U);
        for (const auto interval : flowmesh::CANDLE_INTERVALS) {
            BOOST_CHECK(store.ReadCandles(MARKET, interval, 0, T0 + 86'400'000, 100).empty());
        }
    }
    // The stored row is the old one: the new entry was never written.
    CDBWrapper db{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
    RawRowValue row;
    BOOST_REQUIRE(db.Read(RawRowKey{'r', MARKET, 12}, row));
    BOOST_CHECK(row.hash == EntryHash(12));
}

// A read error a scan meets (an unreadable candle block) fails the load as
// a thrown one does: it is not a load that found fewer candles.
BOOST_AUTO_TEST_CASE(a_read_error_during_the_load_is_not_a_load)
{
    const auto path{m_path_root / "observed_unreadable_candles"};
    FakeClocks clocks;
    WriteUnreadableBlock(path, clocks, 20, 'c');
    FlowMeshObservedTimeStore store{Options(path)};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    store.Attach(log, 20, Known(0, 20));
    store.FlushNow(false);
    BOOST_CHECK(log->GetSummary().loading);
    BOOST_CHECK(!log->GetSummary().persisted);
    store.Stop();
    BOOST_CHECK(!log->GetSummary().loading);
    BOOST_CHECK(!log->GetSummary().persisted);
    BOOST_CHECK(store.ReadCandles(MARKET, CandleInterval::M1, 0, T0 + 86'400'000, 100).empty());
}

// A wipe (here for a meta record of another domain) that meets an
// unreadable block writes nothing, since a key the block hid would survive
// it: the load fails instead and the stored rows stay as they were.
BOOST_AUTO_TEST_CASE(a_wipe_that_meets_a_read_error_writes_nothing)
{
    const auto path{m_path_root / "observed_unreadable_wipe"};
    FakeClocks clocks;
    WriteUnreadableBlock(path, clocks, 20, 'c');
    {
        FlowMeshObservedTimeStore store{Options(path)};
        std::string error;
        BOOST_REQUIRE(store.Open(error));
        const uint256 other_domain{uint256{"00000000000000000000000000000000000000000000000000000000000000d1"}};
        const auto log{std::make_shared<FlowMeshObservedTimeLog>(other_domain, MARKET, clocks.Get())};
        store.Attach(log, 20, {});
        store.FlushNow(false);
        BOOST_CHECK(log->GetSummary().loading);
        store.Stop();
        BOOST_CHECK(!log->GetSummary().loading);
        BOOST_CHECK(!log->GetSummary().persisted);
    }
    CDBWrapper db{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
    for (uint64_t i{0}; i < 20; ++i) BOOST_CHECK(db.Exists(RawRowKey{'r', MARKET, i}));
}

// A disk read of a loaded market that meets an error says so: no candles,
// `older` unset and `failed` set, for a thrown error and for a block found
// unreadable after the load alike. Never an empty page that reads as "no
// older history"; the next read that succeeds serves the page again.
BOOST_AUTO_TEST_CASE(a_failed_disk_read_is_reported_never_an_empty_history)
{
    const auto path{m_path_root / "observed_unreadable_read"};
    FakeClocks clocks;
    WritePaddedTable(path, clocks, 20);
    FlowMeshObservedTimeStore store{Options(path)};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    store.Attach(log, 20, Known(0, 20));
    store.FlushNow(false);
    BOOST_REQUIRE(!log->GetSummary().loading);
    bool older{false}, failed{false};
    // One 1m candle at T0 (entries 0..19); a window after it has it older.
    const auto read = [&](const int64_t from_ms) {
        older = !older;
        failed = !failed;
        return store.ReadCandles(MARKET, CandleInterval::M1, from_ms, T0 + 3'600'000, 100, &older, &failed);
    };
    BOOST_CHECK_EQUAL(read(0).size(), 1U);
    BOOST_CHECK(!older);
    BOOST_CHECK(!failed);
    BOOST_CHECK(read(T0 + 60'000).empty());
    BOOST_CHECK(older);
    BOOST_CHECK(!failed);
    store.SetReadHookForTest([] { throw dbwrapper_error{"injected read error"}; });
    for (const int64_t from : {int64_t{0}, T0 + 60'000}) {
        BOOST_CHECK(read(from).empty());
        BOOST_CHECK(!older);
        BOOST_CHECK(failed);
    }
    store.SetReadHookForTest({});
    BOOST_CHECK_EQUAL(read(0).size(), 1U);
    BOOST_CHECK(!failed);
    // The loaded market's candle block made unreadable: LevelDB's iterators
    // never cache a block, so the next read meets it.
    BOOST_REQUIRE(FlipBlock(path, 'c'));
    for (const int64_t from : {int64_t{0}, T0 + 60'000}) {
        BOOST_CHECK(read(from).empty());
        BOOST_CHECK(!older);
        BOOST_CHECK(failed);
    }
    store.Stop();
}

// A load that fails once at any stage leaves the market loading: its reads
// stay unavailable, none of its disk candles (which hold entries beyond
// the head until the erase) is served and nothing of it is written. The
// retry then loads it as a slow load would: the history, the entry that
// waited, the rejected candles erased.
BOOST_AUTO_TEST_CASE(a_failed_load_is_fenced_until_its_retry_succeeds)
{
    for (const std::string stage : {"meta", "erase", "rows", "candles"}) {
        const auto path{m_path_root / fs::PathFromString("observed_retry_" + stage)};
        FakeClocks clocks;
        Populate(path, clocks, 20, 1'000);
        auto options{Options(path)};
        options.load_retry = std::chrono::milliseconds{0};
        FlowMeshObservedTimeStore store{options};
        std::string error;
        BOOST_REQUIRE(store.Open(error));
        const auto log{MakeLog(clocks)};
        store.Attach(log, 12, Known(0, 12));
        unsigned reached{0}, writes{0};
        store.SetLoadHookForTest([&](const flowmesh::MarketId& market, const char* at) {
            BOOST_CHECK(market == MARKET);
            if (stage == at && reached++ == 0) throw std::runtime_error{"injected"};
        });
        store.SetFlushHookForTest([&] { ++writes; });
        store.FlushNow(false);
        BOOST_CHECK_EQUAL(reached, 1U);
        clocks.Advance(1'000);
        log->Record(12, EntryHash(12), ObservedDelay::NONE, Tick(12));
        BOOST_CHECK(log->GetSummary().loading);
        BOOST_CHECK(!log->GetSummary().persisted);
        BOOST_CHECK(!log->Candles(CandleInterval::M1, std::nullopt, 10).available);
        for (const auto interval : flowmesh::CANDLE_INTERVALS) {
            BOOST_CHECK(store.ReadCandles(MARKET, interval, 0, T0 + 86'400'000, 100).empty());
        }
        BOOST_CHECK_EQUAL(writes, 0U);
        store.FlushNow(false);
        BOOST_CHECK_EQUAL(reached, 2U);
        BOOST_CHECK(!log->GetSummary().loading);
        BOOST_CHECK(log->GetSummary().persisted);
        BOOST_CHECK_EQUAL(log->Lookup(Known(0, 13)).size(), 13U);
        BOOST_CHECK_EQUAL(writes, 1U);
        size_t served{0};
        for (const auto interval : flowmesh::CANDLE_INTERVALS) {
            for (const auto& candle : store.ReadCandles(MARKET, interval, 0, T0 + 86'400'000, 100)) {
                BOOST_CHECK_EQUAL(candle.first_sequence, 12U);
                BOOST_CHECK_EQUAL(candle.last_sequence, 12U);
                ++served;
            }
        }
        BOOST_CHECK_EQUAL(served, flowmesh::CANDLE_INTERVALS.size());
        store.SetLoadHookForTest({});
        store.SetFlushHookForTest({});
        store.Stop();
    }
}

// Every attempt fails: after load_attempts the market is memory only until
// restart. Its entries (those that waited, then new ones) are recorded and
// read from memory; nothing of it is served from disk or written, and what
// it would persist is dropped each pass, never accumulated.
BOOST_AUTO_TEST_CASE(a_load_given_up_keeps_the_market_in_memory_only)
{
    const auto path{m_path_root / "observed_given_up"};
    FakeClocks clocks;
    Populate(path, clocks, 20, 1'000);
    auto options{Options(path)};
    options.load_retry = std::chrono::milliseconds{0};
    FlowMeshObservedTimeStore store{options};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    store.Attach(log, 20, Known(0, 20));
    unsigned attempts{0}, writes{0};
    store.SetLoadHookForTest([&](const flowmesh::MarketId&, const char* at) {
        if (std::string_view{at} == "meta") {
            ++attempts;
            throw std::runtime_error{"injected"};
        }
    });
    store.SetFlushHookForTest([&] { ++writes; });
    for (uint64_t i{20}; i < 23; ++i) {
        clocks.Advance(1'000);
        log->Record(i, EntryHash(i), ObservedDelay::NONE, Tick(i));
        BOOST_CHECK(log->GetSummary().loading);
        store.FlushNow(false);
    }
    BOOST_CHECK_EQUAL(attempts, options.load_attempts);
    const auto summary{log->GetSummary()};
    BOOST_CHECK(!summary.loading);
    BOOST_CHECK(!summary.persisted);
    BOOST_CHECK_EQUAL(log->Lookup(Known(20, 23)).size(), 3U);
    // The market-data section serves memory only, from the first entry
    // recorded here, and says its times are not persisted.
    flowmesh::MarketData data;
    node::FillObservedTimes(data, log.get());
    BOOST_REQUIRE(data.observed);
    BOOST_CHECK(data.observed->available);
    BOOST_CHECK_EQUAL(data.observed->recorded_since_sequence.value_or(0), 20U);
    BOOST_REQUIRE(data.observed->storage);
    BOOST_CHECK(!data.observed->storage->persisted);
    clocks.Advance(1'000);
    log->Record(23, EntryHash(23), ObservedDelay::NONE, Tick(23));
    BOOST_CHECK_EQUAL(log->Candles(CandleInterval::M1, std::nullopt, 10).candles.back().last_sequence, 23U);
    store.FlushNow(false);
    BOOST_CHECK(log->TakePending().Empty());
    BOOST_CHECK_EQUAL(attempts, options.load_attempts);
    BOOST_CHECK_EQUAL(writes, 0U);
    BOOST_CHECK(store.ReadCandles(MARKET, CandleInterval::M1, 0, T0 + 86'400'000, 100).empty());
    store.Stop();
    // Only Stop's final synced write.
    BOOST_CHECK_EQUAL(writes, 1U);
    BOOST_CHECK_EQUAL(attempts, options.load_attempts);
    store.SetLoadHookForTest({});
    store.SetFlushHookForTest({});
}

// Stop attempts a load whose retry is not due yet: a disk that recovered
// gets the history and the entries that waited.
BOOST_AUTO_TEST_CASE(stop_attempts_a_queued_load_at_once)
{
    const auto path{m_path_root / "observed_stop_retry"};
    FakeClocks clocks;
    Populate(path, clocks, 20, 1'000);
    auto options{Options(path)};
    options.load_retry = std::chrono::hours{1};
    FlowMeshObservedTimeStore store{options};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    store.Attach(log, 20, Known(0, 20));
    unsigned reached{0};
    store.SetLoadHookForTest([&](const flowmesh::MarketId&, const char* at) {
        if (std::string_view{at} == "meta" && reached++ == 0) throw std::runtime_error{"injected"};
    });
    store.FlushNow(false);
    clocks.Advance(1'000);
    log->Record(20, EntryHash(20), ObservedDelay::NONE, Tick(20));
    store.FlushNow(false);
    BOOST_CHECK_EQUAL(reached, 1U);
    BOOST_CHECK(log->GetSummary().loading);
    store.Stop();
    BOOST_CHECK_EQUAL(reached, 2U);
    BOOST_CHECK(!log->GetSummary().loading);
    BOOST_CHECK(log->GetSummary().persisted);
    BOOST_CHECK_EQUAL(log->Lookup(Known(0, 21)).size(), 21U);
    const auto disk{store.ReadCandles(MARKET, CandleInterval::M1, T0, T0 + 60'000, 10)};
    BOOST_REQUIRE_EQUAL(disk.size(), 1U);
    BOOST_CHECK_EQUAL(disk.front().first_sequence, 0U);
    BOOST_CHECK_EQUAL(disk.front().last_sequence, 20U);
    store.SetLoadHookForTest({});
}

// Only a stopping store gives a failing load up after one attempt: a synced
// pass of a running store (FlushNow(true)) neither attempts a load before
// its retry is due nor gives a failed one up.
BOOST_AUTO_TEST_CASE(a_synced_pass_alone_never_gives_a_failing_load_up)
{
    const auto path{m_path_root / "observed_synced_retry"};
    FakeClocks clocks;
    Populate(path, clocks, 20, 1'000);
    auto options{Options(path)};
    options.load_retry = std::chrono::hours{1};
    FlowMeshObservedTimeStore store{options};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    store.Attach(log, 20, Known(0, 20));
    unsigned reached{0};
    store.SetLoadHookForTest([&](const flowmesh::MarketId&, const char* at) {
        if (std::string_view{at} == "meta" && reached++ == 0) throw std::runtime_error{"injected"};
    });
    store.FlushNow(true);
    BOOST_CHECK_EQUAL(reached, 1U);
    BOOST_CHECK(log->GetSummary().loading);
    // Its retry is an hour away: a second synced pass leaves it queued.
    store.FlushNow(true);
    BOOST_CHECK_EQUAL(reached, 1U);
    BOOST_CHECK(log->GetSummary().loading);
    BOOST_CHECK(store.ReadCandles(MARKET, CandleInterval::M1, T0, T0 + 60'000, 10).empty());
    // Stop attempts it at once, and the recovered disk loads.
    store.Stop();
    BOOST_CHECK_EQUAL(reached, 2U);
    BOOST_CHECK(!log->GetSummary().loading);
    BOOST_CHECK(log->GetSummary().persisted);
    BOOST_CHECK_EQUAL(store.ReadCandles(MARKET, CandleInterval::M1, T0, T0 + 60'000, 10).size(), 1U);
    store.SetLoadHookForTest({});
}

// A history read and checked but not installed (FinishLoad failing, as an
// allocation failure would) leaves the log empty and recording: the market
// is given up, so its fresh candles never overwrite the stored ones of the
// same buckets and the failure stays visible (never persisted again by a
// sweep).
BOOST_AUTO_TEST_CASE(a_load_that_cannot_install_its_history_is_given_up)
{
    const auto path{m_path_root / "observed_install_failure"};
    FakeClocks clocks;
    Populate(path, clocks, 20, 1'000);
    {
        FlowMeshObservedTimeStore store{Options(path)};
        std::string error;
        BOOST_REQUIRE(store.Open(error));
        const auto log{MakeLog(clocks)};
        store.Attach(log, 20, Known(0, 20));
        log->FailNextFinishLoadForTest();
        unsigned writes{0};
        store.SetFlushHookForTest([&] { ++writes; });
        store.FlushNow(false);
        BOOST_CHECK(!log->GetSummary().loading);
        BOOST_CHECK(!log->GetSummary().persisted);
        BOOST_CHECK_EQUAL(writes, 0U);
        for (const auto interval : flowmesh::CANDLE_INTERVALS) {
            BOOST_CHECK(store.ReadCandles(MARKET, interval, 0, T0 + 86'400'000, 100).empty());
        }
        // An entry in the same minute is recorded in memory, never written.
        log->Record(20, EntryHash(20), ObservedDelay::NONE, Tick(20));
        BOOST_CHECK_EQUAL(log->Candles(CandleInterval::M1, std::nullopt, 10).candles.back().last_sequence, 20U);
        store.FlushNow(false);
        BOOST_CHECK(!log->GetSummary().persisted);
        BOOST_CHECK_EQUAL(writes, 0U);
        store.SetFlushHookForTest({});
        store.Stop();
        BOOST_CHECK(!log->GetSummary().persisted);
    }
    // The stored candles still hold entries 0..19.
    FlowMeshObservedTimeStore store{Options(path)};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    const auto log{MakeLog(clocks)};
    store.Attach(log, 20, Known(0, 20));
    store.FlushNow(false);
    BOOST_CHECK(log->GetSummary().persisted);
    for (const auto interval : {CandleInterval::M1, CandleInterval::D1}) {
        const auto disk{store.ReadCandles(MARKET, interval, 0, T0 + 86'400'000, 100)};
        BOOST_REQUIRE_EQUAL(disk.size(), 1U);
        BOOST_CHECK_EQUAL(disk.front().first_sequence, 0U);
        BOOST_CHECK_EQUAL(disk.front().last_sequence, 19U);
        BOOST_CHECK_EQUAL(disk.front().entries, 20U);
    }
    store.Stop();
}

// A market whose load is given up never affects another: the other market
// loads, writes and is served as usual, and Stop's final synced write runs
// once for the store.
BOOST_AUTO_TEST_CASE(a_market_given_up_never_affects_another)
{
    const auto path{m_path_root / "observed_given_up_beside"};
    FakeClocks clocks;
    Populate(path, clocks, 20, 1'000);
    {
        auto options{Options(path)};
        options.load_retry = std::chrono::milliseconds{0};
        FlowMeshObservedTimeStore store{options};
        std::string error;
        BOOST_REQUIRE(store.Open(error));
        const auto log{MakeLog(clocks)};
        const auto other{std::make_shared<FlowMeshObservedTimeLog>(TEST_DOMAIN, OTHER_MARKET, clocks.Get())};
        unsigned attempts{0}, other_stages{0};
        store.SetLoadHookForTest([&](const flowmesh::MarketId& market, const char* at) {
            if (market != MARKET) {
                ++other_stages;
                return;
            }
            if (std::string_view{at} == "meta") {
                ++attempts;
                throw std::runtime_error{"injected"};
            }
        });
        store.Attach(log, 20, Known(0, 20));
        store.Attach(other, 0, {});
        for (uint64_t i{0}; i < 3; ++i) {
            clocks.Advance(1'000);
            log->Record(20 + i, EntryHash(20 + i), ObservedDelay::NONE, Tick(20 + i));
            other->Record(i, EntryHash(i), ObservedDelay::NONE, Tick(i));
            store.FlushNow(false);
        }
        BOOST_CHECK_EQUAL(attempts, options.load_attempts);
        // One load of the other market, every stage, once.
        BOOST_CHECK_EQUAL(other_stages, 4U);
        BOOST_CHECK(!log->GetSummary().loading);
        BOOST_CHECK(!log->GetSummary().persisted);
        BOOST_CHECK_EQUAL(log->Lookup(Known(20, 23)).size(), 3U);
        BOOST_CHECK(store.ReadCandles(MARKET, CandleInterval::M1, 0, T0 + 86'400'000, 100).empty());
        BOOST_CHECK(!other->GetSummary().loading);
        BOOST_CHECK(other->GetSummary().persisted);
        const auto disk{store.ReadCandles(OTHER_MARKET, CandleInterval::M1, 0, T0 + 86'400'000, 100)};
        BOOST_REQUIRE_EQUAL(disk.size(), 1U);
        BOOST_CHECK_EQUAL(disk.front().first_sequence, 0U);
        BOOST_CHECK_EQUAL(disk.front().last_sequence, 2U);
        unsigned writes{0};
        store.SetFlushHookForTest([&] { ++writes; });
        store.Stop();
        BOOST_CHECK_EQUAL(writes, 1U);
        BOOST_CHECK(other->GetSummary().persisted);
        BOOST_CHECK_EQUAL(attempts, options.load_attempts);
        store.SetFlushHookForTest({});
        store.SetLoadHookForTest({});
    }
    // The given-up market's rows are untouched and none of its new ones was
    // written; the other market's are on disk.
    CDBWrapper db{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
    RawRowValue row;
    BOOST_REQUIRE(db.Read(RawRowKey{'r', MARKET, 19}, row));
    BOOST_CHECK(row.hash == EntryHash(19));
    BOOST_CHECK(!db.Exists(RawRowKey{'r', MARKET, 20}));
    BOOST_REQUIRE(db.Read(RawRowKey{'r', OTHER_MARKET, 2}, row));
    BOOST_CHECK(row.hash == EntryHash(2));
}

BOOST_AUTO_TEST_SUITE_END()
