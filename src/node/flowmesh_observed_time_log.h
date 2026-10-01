// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef B3COIN_NODE_FLOWMESH_OBSERVED_TIME_LOG_H
#define B3COIN_NODE_FLOWMESH_OBSERVED_TIME_LOG_H

#include <flowmesh/market.h>
#include <flowmesh/market_data.h>
#include <flowmesh/observed_time.h>
#include <uint256.h>
#include <util/fs.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class CDBWrapper;

namespace node {

/** Operator-observed time of one market (doc/design/flowmesh-chart-time.md).
 *
 * Display only. Nothing that signs, admits, relays, executes, verifies or
 * decides anything reads this log, and nothing here can halt a market: a
 * failure only leaves a gap in the times. Record runs on the runtime worker
 * right after a durable commit; it is memory only and never throws, so a
 * slow or broken disk never reaches the commit path. Its own mutex is a
 * leaf (taken inside the runtime's market lock, never around another lock).
 *
 * Clocks: a backward wall-clock step up to OBSERVED_CLOCK_SLEW_MS is held at
 * the previous stamp and flagged clamped; a larger one records entries
 * UNTIMED until the wall clock passes the previous stamp again, so no time
 * is ever pinned or invented. Wall and steady clocks disagreeing by more
 * than the slew since the previous stamp flags a clock step.
 *
 * History: between BeginLoad and FinishLoad (the store reading this
 * market's retained history on its own thread) Record keeps each entry's
 * clock readings only, and every read is unavailable (LOADING_REASON),
 * never partial. FinishLoad then stamps those entries exactly as if the
 * history had loaded before them. */
class FlowMeshObservedTimeLog
{
public:
    struct Clocks {
        //! Unix milliseconds; default the system clock.
        std::function<int64_t()> wall_ms;
        //! Default std::chrono::steady_clock.
        std::function<std::chrono::steady_clock::time_point()> steady;
    };
    //! Stamps kept in memory for replies; rows persisted per market.
    static constexpr size_t RING{4096};
    //! Unflushed rows. Beyond it the oldest are dropped from persistence
    //! only (a gap after a restart), never from memory.
    static constexpr size_t PENDING_ROWS_MAX{8192};
    //! Unflushed candle snapshots of `interval`: CandleMemoryWindow + 1
    //! (4742 for the six). Only a flush stalled for longer than the window
    //! reaches it; the oldest snapshot, which can no longer change, is then
    //! dropped from persistence only and counted (candles_not_persisted),
    //! never from memory. The stored candle of that bucket stays as it was
    //! at the last successful flush, or is absent, as after a crash; the
    //! sequence gap to the next stored candle shows it.
    static size_t PendingCandlesMax(flowmesh::CandleInterval interval) { return flowmesh::CandleMemoryWindow(interval) + 1; }
    //! Why every read is unavailable while the history loads.
    static constexpr const char* LOADING_REASON{"observed_time_loading"};

    FlowMeshObservedTimeLog(uint256 domain, flowmesh::MarketId market, Clocks clocks = {});

    const uint256& Domain() const { return m_domain; }
    const flowmesh::MarketId& MarketId() const { return m_market; }

    /** One durable commit, in sequence order. `delay` NONE records it LIVE.
     * `tick` is the entry's clearing; one of another entry is ignored and
     * the entry counts without a price. Memory only and bounded (at most
     * PendingCandlesMax snapshots of each interval wait to be persisted, a
     * map); never throws.
     * While the history loads it keeps only this call's clock readings (at
     * most RING entries; beyond that the oldest are dropped, a gap) and
     * returns a provisional UNTIMED stamp; FinishLoad stamps the entry. */
    flowmesh::ObservedStamp Record(uint64_t sequence, const uint256& hash, flowmesh::ObservedDelay delay,
                                   const std::optional<flowmesh::ClearingTick>& tick) noexcept;

    //! The stamps of exactly these entries whose hash matches, ascending.
    //! None while the history loads.
    std::vector<flowmesh::ObservedStamp> Lookup(const std::vector<std::pair<uint64_t, uint256>>& entries) const;
    //! A page of the memory window (ObservedRollup::Page). Unavailable
    //! (LOADING_REASON) while the history loads.
    flowmesh::ObservedCandlePage Candles(flowmesh::CandleInterval interval, std::optional<int64_t> before_ms, size_t limit) const;
    //! The oldest candle held in memory for `interval`, if any. None while
    //! the history loads.
    std::optional<int64_t> OldestMemoryOpen(flowmesh::CandleInterval interval) const;

    struct Summary {
        //! Unset while the history loads.
        std::optional<uint64_t> recorded_since;
        bool persisted{true};
        bool clock_suspect{false};
        uint64_t rows_not_persisted{0};
        //! Internal (never serialized): candle snapshots dropped beyond
        //! PendingCandlesMax.
        uint64_t candles_not_persisted{0};
        uint64_t record_failures{0};
        //! Internal (never serialized): the history is loading, so every
        //! read is unavailable with LOADING_REASON.
        bool loading{false};
    };
    Summary GetSummary() const;
    //! Set from outside (the B3 tip's time); flags later stamps.
    void SetClockSuspect(bool suspect) noexcept { m_clock_suspect.store(suspect, std::memory_order_relaxed); }
    //! Increases with every Record and when FinishLoad installs the history.
    uint64_t Generation() const noexcept { return m_generation.load(std::memory_order_acquire); }

    //! Persistence plumbing for FlowMeshObservedTimeStore.
    struct Pending {
        //! A deque: dropping the oldest on overflow is O(1).
        std::deque<flowmesh::ObservedStamp> rows;
        //! At most PendingCandlesMax(interval) for each interval.
        std::map<std::pair<uint8_t, int64_t>, flowmesh::ObservedCandle> candles;
        //! How many of `candles` each interval holds.
        std::array<size_t, flowmesh::CANDLE_INTERVALS.size()> candle_counts{};
        bool Empty() const { return rows.empty() && candles.empty(); }
    };
    Pending TakePending();
    void SetPersisted(bool persisted) noexcept { m_persisted.store(persisted, std::memory_order_relaxed); }
    //! Replaces memory with reloaded data. `rows` ascending (the newest are
    //! kept), `candles` ascending per interval. BeginLoad then FinishLoad.
    void Load(std::vector<flowmesh::ObservedStamp> rows,
              std::array<std::vector<flowmesh::ObservedCandle>, flowmesh::CANDLE_INTERVALS.size()> candles,
              std::optional<uint64_t> recorded_since);
    /** The history is being read: from now until FinishLoad, Record keeps
     * clock readings only and every read is unavailable. For a fresh log
     * (FlowMeshObservedTimeStore::Attach is always given one). */
    void BeginLoad() noexcept;
    /** Replaces memory with the reloaded data (as Load), then applies every
     * Record since BeginLoad in order with its own clock readings: the
     * result is exactly the synchronous Load followed by those Records,
     * except that entries dropped beyond RING are a gap. The work is done
     * off the mutex (Record never waits for it); under it only the entries
     * recorded meanwhile are applied and the state is swapped in. Never
     * throws: a failure leaves the log empty and recording, and returns
     * false (the history was not installed). */
    bool FinishLoad(std::vector<flowmesh::ObservedStamp> rows,
                    std::array<std::vector<flowmesh::ObservedCandle>, flowmesh::CANDLE_INTERVALS.size()> candles,
                    std::optional<uint64_t> recorded_since) noexcept;

private:
    //! One Record's inputs and clock readings.
    struct Raw {
        uint64_t sequence{0};
        uint256 hash;
        flowmesh::ObservedDelay delay{flowmesh::ObservedDelay::NONE};
        std::optional<flowmesh::ClearingTick> tick;
        //! Clamped to +-OBSERVED_MS_LIMIT.
        int64_t wall{0};
        std::chrono::steady_clock::time_point steady{};
        //! OBSERVED_CLOCK_SUSPECT and OBSERVED_NO_CLEARING, read at Record.
        uint8_t flags{0};
    };
    //! Everything Record changes.
    struct State {
        std::deque<flowmesh::ObservedStamp> ring;
        flowmesh::ObservedRollup rollup;
        Pending pending;
        std::optional<uint64_t> recorded_since;
        //! The newest timed stamp (LIVE or DELAYED), kept monotonic.
        std::optional<int64_t> last_ms;
        //! This process's previous Record: raw wall and steady readings.
        std::optional<std::pair<int64_t, std::chrono::steady_clock::time_point>> previous;
        uint64_t rows_not_persisted{0};
        uint64_t candles_not_persisted{0};
    };
    //! The body of Record, on the live state or on one being reloaded.
    static flowmesh::ObservedStamp Apply(State& state, const Raw& raw);
    //! The state Load installs, before any Record.
    static State Reloaded(std::vector<flowmesh::ObservedStamp> rows,
                          std::array<std::vector<flowmesh::ObservedCandle>, flowmesh::CANDLE_INTERVALS.size()> candles,
                          std::optional<uint64_t> recorded_since);

    const uint256 m_domain;
    const flowmesh::MarketId m_market;
    const Clocks m_clocks;
    mutable std::mutex m_mutex;
    State m_state;
    //! Between BeginLoad and FinishLoad.
    bool m_loading{false};
    //! Records while loading, oldest first, at most RING.
    std::deque<Raw> m_early;
    //! Records dropped from m_early (a gap), counted as not persisted.
    uint64_t m_early_dropped{0};
    std::atomic<uint64_t> m_record_failures{0};
    std::atomic<bool> m_persisted{true};
    std::atomic<bool> m_clock_suspect{false};
    std::atomic<uint64_t> m_generation{0};
};

/** The one display-only database of every market's observed time, at
 * <flowmeshdatadir>/observed_time, outside every production store (whose
 * fail-closed checks it can never affect). Keys are market-prefixed.
 *
 * Nothing on a caller's thread waits for its disk: Attach only queues a
 * market, and the flush thread loads (reads, erases, wipes) each queued
 * market before it writes any row of it. Its mutex is held only for
 * in-memory bookkeeping, never across disk work.
 *
 * Only its own flush thread writes it: at most once per interval, unsynced,
 * one batch per market (rows and candles together, so a crash loses a
 * consistent tail), and every Stop ends with one synced write. It is never
 * written by the FlowMesh tick or worker thread, so a stalled disk delays
 * only the flush.
 * A corrupt database is wiped and recreated; one that cannot be opened
 * leaves every log memory only. Retention: 262,144 rows per market; candles
 * 1m 14 days, 5m 60 days, 15m 180 days, 1h 2 years, 4h 5 years, 1d 10 years. */
class FlowMeshObservedTimeStore
{
public:
    struct Options {
        fs::path path;
        bool memory_only{false};
        size_t cache_bytes{size_t{1} << 20};
        std::chrono::milliseconds flush_interval{1000};
    };
    static constexpr uint64_t ROW_RETENTION{262'144};
    //! Erases per retention sweep of one market (bounds one flush's work).
    static constexpr size_t SWEEP_ERASE_MAX{4096};

    explicit FlowMeshObservedTimeStore(Options options);
    ~FlowMeshObservedTimeStore();
    FlowMeshObservedTimeStore(const FlowMeshObservedTimeStore&) = delete;
    FlowMeshObservedTimeStore& operator=(const FlowMeshObservedTimeStore&) = delete;

    //! Opens the database, wiping and recreating a corrupt one. False (and
    //! memory-only logs) only when even that fails. Never throws. The flush
    //! thread calls it when it starts, before its first flush or load.
    bool Open(std::string& error);
    bool IsOpen() const;
    /** Queues `log` (a fresh one) and returns at once: no disk access and
     * no wait for a flush. The flush thread then loads its retained rows
     * and candles before it writes any row of it, and flushes it from then
     * on. Rows at or after `store_next_sequence` (never certified here) are
     * erased, together with every candle holding any such row; a row whose
     * hash differs from `known` (sequence, hash) of the production store
     * wipes the market's times; so does a meta record of another domain.
     * Until the load ends, the log's reads are unavailable
     * (FlowMeshObservedTimeLog::LOADING_REASON) and ReadCandles serves none
     * of the market's disk candles. Never throws. */
    void Attach(const std::shared_ptr<FlowMeshObservedTimeLog>& log, uint64_t store_next_sequence,
                const std::vector<std::pair<uint64_t, uint256>>& known) noexcept;
    //! Starts the flush thread, which opens the database first.
    void Start();
    //! Stops the flush thread, writes every pending log, then syncs the
    //! database even when previous unsynced passes left no pending rows.
    void Stop();
    //! One flush pass on the calling thread (the flush thread's body).
    void FlushNow(bool sync) noexcept;
    //! Disk candles of one market with open in [from_ms, before_ms), at most
    //! `limit`, ascending; `older` is set when one exists before from_ms.
    //! None while that market's load is queued. Never holds the store mutex
    //! across the read.
    std::vector<flowmesh::ObservedCandle> ReadCandles(const flowmesh::MarketId& market, flowmesh::CandleInterval interval,
                                                      int64_t from_ms, int64_t before_ms, size_t limit, bool* older = nullptr) const;
    //! Tests only: runs inside every flush, before its batch is written,
    //! and before Stop's final synced write.
    void SetFlushHookForTest(std::function<void()> hook);

private:
    //! What Attach queues for the flush thread.
    struct LoadJob {
        uint64_t store_next_sequence{0};
        std::vector<std::pair<uint64_t, uint256>> known;
    };
    struct Attached {
        std::shared_ptr<FlowMeshObservedTimeLog> log;
        //! These three belong to the holder of m_flush_mutex once the item
        //! is published.
        std::optional<uint64_t> oldest_row;
        std::chrono::steady_clock::time_point next_sweep{};
        std::optional<LoadJob> load;
        //! Under m_mutex: set until the load's disk work is done, and no
        //! disk candle of the market is served meanwhile.
        bool loading{false};
    };
    void FlushLoop();
    //! One queued load (the body Attach ran before queueing). Caller holds
    //! m_flush_mutex.
    void RunLoad(Attached& item, CDBWrapper* db) noexcept;
    //! Caller holds m_flush_mutex.
    static void WipeMarket(CDBWrapper& db, const flowmesh::MarketId& market);

    const Options m_options;
    //! Published once, under both mutexes (Open); read under either. Freed
    //! only with the store, so a reader may use it after dropping m_mutex.
    std::unique_ptr<CDBWrapper> m_db;
    mutable std::mutex m_mutex;
    //! A list: a flush pass keeps pointers to its items across the pass
    //! while Attach appends.
    std::list<Attached> m_attached;
    std::function<void()> m_hook;
    //! A load is queued (wakes the flush thread).
    bool m_load_requested{false};
    //! Serializes Open, loads and flush passes (the thread's and Stop's).
    //! Never taken by Attach or ReadCandles.
    std::mutex m_flush_mutex;
    std::condition_variable m_cv;
    bool m_stopping{false};
    std::thread m_thread;
};

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_OBSERVED_TIME_LOG_H
