// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <node/flowmesh_observed_time_log.h>

#include <dbwrapper.h>
#include <logging.h>
#include <serialize.h>
#include <util/threadnames.h>
#include <util/time.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace node {
namespace {

constexpr uint8_t KEY_META{'M'};
constexpr uint8_t KEY_ROW{'r'};
constexpr uint8_t KEY_CANDLE{'c'};
constexpr int32_t META_VERSION{1};
constexpr auto SWEEP_INTERVAL{std::chrono::minutes{1}};

int64_t CandleRetentionMs(const flowmesh::CandleInterval interval)
{
    constexpr int64_t day{86'400'000};
    switch (interval) {
    case flowmesh::CandleInterval::M1: return 14 * day;
    case flowmesh::CandleInterval::M5: return 60 * day;
    case flowmesh::CandleInterval::M15: return 180 * day;
    case flowmesh::CandleInterval::H1: return 730 * day;
    case flowmesh::CandleInterval::H4: return 1826 * day;
    case flowmesh::CandleInterval::D1: return 3652 * day;
    }
    return 14 * day;
}

struct MetaKey {
    uint8_t prefix{KEY_META};
    uint256 market;
    SERIALIZE_METHODS(MetaKey, obj) { READWRITE(obj.prefix, obj.market); }
};

struct MetaValue {
    int32_t version{META_VERSION};
    uint256 domain;
    SERIALIZE_METHODS(MetaValue, obj) { READWRITE(obj.version, obj.domain); }
};

struct RowKey {
    uint8_t prefix{KEY_ROW};
    uint256 market;
    uint64_t sequence{0};
    SERIALIZE_METHODS(RowKey, obj) { READWRITE(obj.prefix, obj.market, Using<BigEndianFormatter<8>>(obj.sequence)); }
};

struct RowValue {
    uint256 hash;
    uint8_t mode{0};
    int64_t ms{0};
    uint8_t flags{0};
    uint8_t delay{0};
    SERIALIZE_METHODS(RowValue, obj) { READWRITE(obj.hash, obj.mode, obj.ms, obj.flags, obj.delay); }
};

struct CandleKey {
    uint8_t prefix{KEY_CANDLE};
    uint256 market;
    uint8_t interval{0};
    uint64_t open_ms{0};
    SERIALIZE_METHODS(CandleKey, obj)
    {
        READWRITE(obj.prefix, obj.market, obj.interval, Using<BigEndianFormatter<8>>(obj.open_ms));
    }
};

struct CandleValue {
    uint64_t first_sequence{0}, last_sequence{0};
    uint32_t entries{0}, clearings{0};
    int64_t open{0}, high{0}, low{0}, close{0};
    uint64_t base_low{0}, base_high{0}, quote_low{0}, quote_high{0};
    uint8_t flags{0};
    SERIALIZE_METHODS(CandleValue, obj)
    {
        READWRITE(obj.first_sequence, obj.last_sequence, obj.entries, obj.clearings, obj.open, obj.high, obj.low,
                  obj.close, obj.base_low, obj.base_high, obj.quote_low, obj.quote_high, obj.flags);
    }
};

RowValue ToRow(const flowmesh::ObservedStamp& stamp)
{
    return {stamp.hash, static_cast<uint8_t>(stamp.mode), stamp.ms, stamp.flags, static_cast<uint8_t>(stamp.delay)};
}

std::optional<flowmesh::ObservedStamp> FromRow(const uint64_t sequence, const RowValue& row)
{
    if (row.mode < static_cast<uint8_t>(flowmesh::ObservedMode::LIVE) ||
        row.mode > static_cast<uint8_t>(flowmesh::ObservedMode::UNTIMED) ||
        row.delay > static_cast<uint8_t>(flowmesh::ObservedDelay::CLOCK_BACK) || !flowmesh::ObservedTimeInRange(row.ms)) return std::nullopt;
    flowmesh::ObservedStamp out;
    out.sequence = sequence;
    out.hash = row.hash;
    out.mode = static_cast<flowmesh::ObservedMode>(row.mode);
    out.ms = row.ms;
    out.flags = row.flags;
    out.delay = static_cast<flowmesh::ObservedDelay>(row.delay);
    return out;
}

CandleValue ToValue(const flowmesh::ObservedCandle& candle)
{
    const auto split = [](const util::Unsigned128& value) {
        return std::pair{static_cast<uint64_t>(value), static_cast<uint64_t>(value >> 64)};
    };
    const auto [base_low, base_high]{split(candle.base_volume)};
    const auto [quote_low, quote_high]{split(candle.quote_volume)};
    return {candle.first_sequence, candle.last_sequence, candle.entries, candle.clearings, candle.open, candle.high,
            candle.low, candle.close, base_low, base_high, quote_low, quote_high, candle.flags};
}

std::optional<flowmesh::ObservedCandle> FromValue(const uint64_t key_open_ms, const flowmesh::CandleInterval interval, const CandleValue& value)
{
    // A key the rollup cannot have written (out of range or unaligned) is
    // skipped, never cast into a negative or far-future open.
    if (key_open_ms >= static_cast<uint64_t>(flowmesh::OBSERVED_MS_LIMIT)) return std::nullopt;
    const int64_t open_ms{static_cast<int64_t>(key_open_ms)};
    if (open_ms % flowmesh::CandleIntervalMs(interval) != 0) return std::nullopt;
    if (value.last_sequence < value.first_sequence || value.clearings > value.entries ||
        (value.clearings > 0 && (value.low > value.high || value.open < value.low || value.open > value.high ||
                                 value.close < value.low || value.close > value.high))) return std::nullopt;
    flowmesh::ObservedCandle out;
    out.open_ms = open_ms;
    out.first_sequence = value.first_sequence;
    out.last_sequence = value.last_sequence;
    out.entries = value.entries;
    out.clearings = value.clearings;
    out.open = value.open;
    out.high = value.high;
    out.low = value.low;
    out.close = value.close;
    out.base_volume = (util::Unsigned128{value.base_high} << 64) | value.base_low;
    out.quote_volume = (util::Unsigned128{value.quote_high} << 64) | value.quote_low;
    out.flags = value.flags;
    return out;
}

int64_t WallNowMs() { return TicksSinceEpoch<std::chrono::milliseconds>(SystemClock::now()); }

} // namespace

FlowMeshObservedTimeLog::FlowMeshObservedTimeLog(uint256 domain, flowmesh::MarketId market, Clocks clocks)
    : m_domain{domain}, m_market{market},
      m_clocks{clocks.wall_ms ? std::move(clocks.wall_ms) : std::function<int64_t()>{WallNowMs},
               clocks.steady ? std::move(clocks.steady)
                             : std::function<std::chrono::steady_clock::time_point()>{[] { return std::chrono::steady_clock::now(); }}}
{
}

flowmesh::ObservedStamp FlowMeshObservedTimeLog::Apply(State& state, const Raw& raw)
{
    flowmesh::ObservedStamp stamp;
    stamp.sequence = raw.sequence;
    stamp.hash = raw.hash;
    stamp.delay = raw.delay;
    stamp.mode = raw.delay == flowmesh::ObservedDelay::NONE ? flowmesh::ObservedMode::LIVE : flowmesh::ObservedMode::DELAYED;
    stamp.flags = raw.flags;
    const int64_t wall{raw.wall};
    const auto steady{raw.steady};
    if (state.ring.empty() || state.ring.back().sequence < raw.sequence) {
        if (!state.previous) {
            stamp.flags |= flowmesh::OBSERVED_RESTART;
        } else {
            const int64_t wall_delta{wall - state.previous->first};
            const int64_t steady_delta{std::chrono::duration_cast<std::chrono::milliseconds>(steady - state.previous->second).count()};
            if (wall_delta - steady_delta > flowmesh::OBSERVED_CLOCK_SLEW_MS ||
                steady_delta - wall_delta > flowmesh::OBSERVED_CLOCK_SLEW_MS) stamp.flags |= flowmesh::OBSERVED_CLOCK_STEP;
        }
        state.previous = std::pair{wall, steady};
        if (wall >= flowmesh::OBSERVED_MS_LIMIT) {
            // A clock at or past the display bound (10000-01-01) is
            // broken, not a time: untimed, flagged suspect and never the
            // monotonic floor, so later stamps are timed again as soon
            // as it is back below the bound. CLOCK_BACK is reused
            // deliberately: the delay vocabulary stays unchanged for a
            // state no working clock reaches.
            stamp.mode = flowmesh::ObservedMode::UNTIMED;
            stamp.delay = flowmesh::ObservedDelay::CLOCK_BACK;
            stamp.ms = 0;
            stamp.flags |= flowmesh::OBSERVED_CLOCK_SUSPECT;
        } else if (state.last_ms && wall < *state.last_ms) {
            if (*state.last_ms - wall <= flowmesh::OBSERVED_CLOCK_SLEW_MS) {
                stamp.ms = *state.last_ms;
                stamp.flags |= flowmesh::OBSERVED_CLOCK_CLAMPED;
            } else {
                // Never pinned at the old value: no time until the clock
                // passes it again.
                stamp.mode = flowmesh::ObservedMode::UNTIMED;
                stamp.delay = flowmesh::ObservedDelay::CLOCK_BACK;
                stamp.ms = 0;
                stamp.flags |= flowmesh::OBSERVED_CLOCK_STEP;
            }
        } else {
            stamp.ms = std::max<int64_t>(wall, 1);
        }
        if (stamp.mode != flowmesh::ObservedMode::UNTIMED) state.last_ms = stamp.ms;
        flowmesh::ObservedRollup::Changes changes;
        state.rollup.Add(stamp, raw.tick, &changes);
        state.ring.push_back(stamp);
        if (state.ring.size() > RING) state.ring.pop_front();
        if (!state.recorded_since) state.recorded_since = raw.sequence;
        state.pending.rows.push_back(stamp);
        if (state.pending.rows.size() > PENDING_ROWS_MAX) {
            state.pending.rows.pop_front();
            ++state.rows_not_persisted;
        }
        for (const auto& [interval, candle] : changes) {
            state.pending.candles[{static_cast<uint8_t>(interval), candle.open_ms}] = candle;
        }
    }
    return stamp;
}

flowmesh::ObservedStamp FlowMeshObservedTimeLog::Record(const uint64_t sequence, const uint256& hash,
                                                        const flowmesh::ObservedDelay delay,
                                                        const std::optional<flowmesh::ClearingTick>& tick) noexcept
{
    try {
        Raw raw;
        raw.sequence = sequence;
        raw.hash = hash;
        raw.delay = delay;
        raw.tick = tick;
        // Clamped so the step check cannot overflow on any clock value.
        raw.wall = std::clamp<int64_t>(m_clocks.wall_ms(), -flowmesh::OBSERVED_MS_LIMIT, flowmesh::OBSERVED_MS_LIMIT);
        raw.steady = m_clocks.steady();
        if (m_clock_suspect.load(std::memory_order_relaxed)) raw.flags |= flowmesh::OBSERVED_CLOCK_SUSPECT;
        if (!tick || tick->sequence != sequence || tick->microblock_hash != hash) raw.flags |= flowmesh::OBSERVED_NO_CLEARING;
        flowmesh::ObservedStamp stamp;
        {
            std::lock_guard lock{m_mutex};
            if (m_loading) {
                // Stamped by FinishLoad, as if the history had loaded first.
                if (m_early.size() >= RING) {
                    m_early.pop_front();
                    ++m_early_dropped;
                }
                m_early.push_back(std::move(raw));
                stamp.sequence = sequence;
                stamp.hash = hash;
                stamp.mode = flowmesh::ObservedMode::UNTIMED;
                stamp.delay = delay;
            } else {
                stamp = Apply(m_state, raw);
            }
        }
        m_generation.fetch_add(1, std::memory_order_acq_rel);
        return stamp;
    } catch (...) {
        m_record_failures.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
}

std::vector<flowmesh::ObservedStamp> FlowMeshObservedTimeLog::Lookup(const std::vector<std::pair<uint64_t, uint256>>& entries) const
{
    std::vector<flowmesh::ObservedStamp> out;
    std::lock_guard lock{m_mutex};
    // While loading, the retained history is unknown: none, never a part.
    if (m_loading) return out;
    const auto& ring{m_state.ring};
    for (const auto& [sequence, hash] : entries) {
        const auto it{std::lower_bound(ring.begin(), ring.end(), sequence,
                                       [](const auto& stamp, const uint64_t value) { return stamp.sequence < value; })};
        if (it != ring.end() && it->sequence == sequence && it->hash == hash) out.push_back(*it);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.sequence < b.sequence; });
    out.erase(std::unique(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.sequence == b.sequence; }), out.end());
    return out;
}

flowmesh::ObservedCandlePage FlowMeshObservedTimeLog::Candles(const flowmesh::CandleInterval interval,
                                                              const std::optional<int64_t> before_ms, const size_t limit) const
{
    std::lock_guard lock{m_mutex};
    if (m_loading) {
        // Never an available page without the history: a reader would hold
        // it as the whole window.
        flowmesh::ObservedCandlePage out;
        out.interval = interval;
        out.unavailable_reason = LOADING_REASON;
        return out;
    }
    return m_state.rollup.Page(interval, before_ms, limit);
}

std::optional<int64_t> FlowMeshObservedTimeLog::OldestMemoryOpen(const flowmesh::CandleInterval interval) const
{
    std::lock_guard lock{m_mutex};
    if (m_loading) return std::nullopt;
    const auto& candles{m_state.rollup.Candles(interval)};
    return candles.empty() ? std::nullopt : std::optional{candles.front().open_ms};
}

FlowMeshObservedTimeLog::Summary FlowMeshObservedTimeLog::GetSummary() const
{
    Summary out;
    {
        std::lock_guard lock{m_mutex};
        out.loading = m_loading;
        if (!m_loading) out.recorded_since = m_state.recorded_since;
        out.rows_not_persisted = m_state.rows_not_persisted + m_early_dropped;
    }
    out.persisted = m_persisted.load(std::memory_order_relaxed);
    out.clock_suspect = m_clock_suspect.load(std::memory_order_relaxed);
    out.record_failures = m_record_failures.load(std::memory_order_relaxed);
    return out;
}

FlowMeshObservedTimeLog::Pending FlowMeshObservedTimeLog::TakePending()
{
    Pending out;
    std::lock_guard lock{m_mutex};
    std::swap(out, m_state.pending);
    return out;
}

FlowMeshObservedTimeLog::State FlowMeshObservedTimeLog::Reloaded(
    std::vector<flowmesh::ObservedStamp> rows,
    std::array<std::vector<flowmesh::ObservedCandle>, flowmesh::CANDLE_INTERVALS.size()> candles,
    const std::optional<uint64_t> recorded_since)
{
    State out;
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.sequence < b.sequence; });
    if (rows.size() > RING) rows.erase(rows.begin(), rows.end() - RING);
    out.ring.assign(rows.begin(), rows.end());
    for (const auto interval : flowmesh::CANDLE_INTERVALS) out.rollup.Restore(interval, std::move(candles[static_cast<size_t>(interval)]));
    std::optional<int64_t> last_live;
    for (auto it{out.ring.rbegin()}; it != out.ring.rend(); ++it) {
        if (!out.last_ms && it->mode != flowmesh::ObservedMode::UNTIMED) out.last_ms = it->ms;
        if (it->mode == flowmesh::ObservedMode::LIVE) {
            // Only a run of consecutive sequences ending at the newest row
            // bounds what follows.
            const bool contiguous{it->sequence + static_cast<uint64_t>(std::distance(out.ring.rbegin(), it)) == out.ring.back().sequence};
            if (contiguous) last_live = it->ms;
            break;
        }
    }
    out.rollup.RestoreLast(out.ring.empty() ? std::nullopt : std::optional{out.ring.back().sequence}, last_live);
    out.recorded_since = recorded_since ? recorded_since : out.ring.empty() ? std::nullopt : std::optional{out.ring.front().sequence};
    return out;
}

void FlowMeshObservedTimeLog::Load(std::vector<flowmesh::ObservedStamp> rows,
                                   std::array<std::vector<flowmesh::ObservedCandle>, flowmesh::CANDLE_INTERVALS.size()> candles,
                                   const std::optional<uint64_t> recorded_since)
{
    BeginLoad();
    FinishLoad(std::move(rows), std::move(candles), recorded_since);
}

void FlowMeshObservedTimeLog::BeginLoad() noexcept
{
    std::lock_guard lock{m_mutex};
    m_loading = true;
}

bool FlowMeshObservedTimeLog::FinishLoad(std::vector<flowmesh::ObservedStamp> rows,
                                         std::array<std::vector<flowmesh::ObservedCandle>, flowmesh::CANDLE_INTERVALS.size()> candles,
                                         const std::optional<uint64_t> recorded_since) noexcept
{
    // Entries taken out of m_early and not yet installed (lost on failure).
    uint64_t taken{0};
    try {
        State state{Reloaded(std::move(rows), std::move(candles), recorded_since)};
        // Replayed off the mutex, so Record (the runtime worker, under its
        // market lock) never waits for this work. Each round swaps the
        // buffer out in O(1); rounds repeat while entries keep arriving, so
        // what is left for the mutex is a handful (RING at most).
        constexpr size_t UNDER_LOCK_MAX{64};
        constexpr int ROUNDS{8};
        for (int round{0}; round < ROUNDS; ++round) {
            std::deque<Raw> batch;
            {
                std::lock_guard lock{m_mutex};
                if (round > 0 && m_early.size() <= UNDER_LOCK_MAX) break;
                batch.swap(m_early);
            }
            taken += batch.size();
            for (const auto& raw : batch) (void)Apply(state, raw);
        }
        {
            std::lock_guard lock{m_mutex};
            // Only what was recorded during the replay above.
            for (const auto& raw : m_early) (void)Apply(state, raw);
            state.rows_not_persisted += m_state.rows_not_persisted + m_early_dropped;
            // The replaced state is freed below, after the mutex.
            std::swap(m_state, state);
            m_early.clear();
            m_early_dropped = 0;
            m_loading = false;
        }
        m_generation.fetch_add(1, std::memory_order_acq_rel);
        return true;
    } catch (...) {
        try {
            std::lock_guard lock{m_mutex};
            State fresh;
            fresh.rows_not_persisted = m_state.rows_not_persisted + m_early_dropped + taken + m_early.size();
            std::swap(m_state, fresh);
            m_early.clear();
            m_early_dropped = 0;
            m_loading = false;
        } catch (...) {
        }
        m_record_failures.fetch_add(1, std::memory_order_relaxed);
        m_generation.fetch_add(1, std::memory_order_acq_rel);
        return false;
    }
}

FlowMeshObservedTimeStore::FlowMeshObservedTimeStore(Options options) : m_options{std::move(options)} {}

FlowMeshObservedTimeStore::~FlowMeshObservedTimeStore() { Stop(); }

bool FlowMeshObservedTimeStore::Open(std::string& error)
{
    std::lock_guard flush{m_flush_mutex};
    {
        std::lock_guard lock{m_mutex};
        if (m_db) return true;
    }
    // Built and checked without m_mutex (readers never wait for this disk
    // work), then published under it.
    DBParams params{.path = m_options.path, .cache_bytes = m_options.cache_bytes, .memory_only = m_options.memory_only};
    for (const bool wipe : {false, true}) {
        try {
            params.wipe_data = wipe;
            auto db{std::make_unique<CDBWrapper>(params)};
            // Display data only: a database this binary cannot read is
            // replaced, never trusted and never fatal.
            std::unique_ptr<CDBIterator> it{db->NewIterator()};
            it->SeekToFirst();
            if (!it->StatusOK()) throw dbwrapper_error{"unreadable observed-time database"};
            it.reset();
            {
                std::lock_guard lock{m_mutex};
                m_db = std::move(db);
            }
            if (wipe) LogWarning("FlowMesh observed-time database was unreadable and has been recreated empty\n");
            return true;
        } catch (const std::exception& e) {
            error = e.what();
        }
    }
    LogWarning("FlowMesh observed time is kept in memory only: %s\n", error);
    return false;
}

bool FlowMeshObservedTimeStore::IsOpen() const
{
    std::lock_guard lock{m_mutex};
    return m_db != nullptr;
}

void FlowMeshObservedTimeStore::WipeMarket(CDBWrapper& db, const flowmesh::MarketId& market)
{
    CDBBatch batch{db};
    size_t erased{0};
    std::unique_ptr<CDBIterator> it{db.NewIterator()};
    for (it->Seek(RowKey{KEY_ROW, market, 0}); it->Valid(); it->Next()) {
        RowKey key;
        if (!it->GetKey(key) || key.prefix != KEY_ROW || key.market != market) break;
        batch.Erase(key);
        ++erased;
    }
    for (it->Seek(CandleKey{KEY_CANDLE, market, 0, 0}); it->Valid(); it->Next()) {
        CandleKey key;
        if (!it->GetKey(key) || key.prefix != KEY_CANDLE || key.market != market) break;
        batch.Erase(key);
        ++erased;
    }
    batch.Write(MetaKey{KEY_META, market}, MetaValue{});
    db.WriteBatch(batch, true);
    LogWarning("FlowMesh market %s: observed times did not match this node's certified log and were cleared (%u records)\n",
               market.GetHex(), static_cast<unsigned>(erased));
}

void FlowMeshObservedTimeStore::Attach(const std::shared_ptr<FlowMeshObservedTimeLog>& log, const uint64_t store_next_sequence,
                                       const std::vector<std::pair<uint64_t, uint256>>& known) noexcept
{
    if (!log) return;
    try {
        // Queued only: the caller (a market start inside the B3
        // reconciliation gate) never waits for this database or a flush.
        log->BeginLoad();
        {
            std::lock_guard lock{m_mutex};
            m_attached.push_back(Attached{log, std::nullopt, {}, LoadJob{store_next_sequence, known}, true});
            m_load_requested = true;
        }
        m_cv.notify_all();
    } catch (...) {
        log->FinishLoad({}, {}, std::nullopt);
        log->SetPersisted(false);
    }
}

void FlowMeshObservedTimeStore::RunLoad(Attached& item, CDBWrapper* const db) noexcept
{
    // Caller holds m_flush_mutex.
    auto& log{*item.log};
    // Set once the disk holds only what may be served (the erasures and
    // wipes below are done), before the log serves its history: a reader
    // that finds the log loaded never finds its disk candles withheld.
    const auto disk_settled = [&]() noexcept {
        try {
            std::lock_guard lock{m_mutex};
            item.loading = false;
        } catch (...) {
        }
    };
    bool finished{false};
    try {
        const LoadJob job{std::move(*item.load)};
        item.load.reset();
        const uint64_t store_next_sequence{job.store_next_sequence};
        const auto& known{job.known};
        if (!db) {
            disk_settled();
            finished = true;
            log.FinishLoad({}, {}, std::nullopt);
            log.SetPersisted(false);
            return;
        }
        const uint256 market{log.MarketId()};
        try {
            MetaValue meta;
            const bool has_meta{db->Read(MetaKey{KEY_META, market}, meta)};
            if (has_meta && (meta.version != META_VERSION || meta.domain != log.Domain())) {
                WipeMarket(*db, market);
            }
            db->Write(MetaKey{KEY_META, market}, MetaValue{META_VERSION, log.Domain()}, true);
            // Rows the production store never certified here are erased.
            std::unique_ptr<CDBIterator> it{db->NewIterator()};
            CDBBatch erase{*db};
            bool erased{false};
            for (it->Seek(RowKey{KEY_ROW, market, store_next_sequence}); it->Valid(); it->Next()) {
                RowKey key;
                if (!it->GetKey(key) || key.prefix != KEY_ROW || key.market != market) break;
                erase.Erase(key);
                erased = true;
            }
            // A candle cannot be truncated without the original clearings.
            // Drop the whole optional bucket when it contains an entry
            // beyond the accepted head, including older intervals that a
            // later disk page could otherwise return after this reload.
            for (it->Seek(CandleKey{KEY_CANDLE, market, 0, 0}); it->Valid(); it->Next()) {
                CandleKey key;
                if (!it->GetKey(key) || key.prefix != KEY_CANDLE || key.market != market) break;
                CandleValue value;
                if (!it->GetValue(value) || value.last_sequence >= store_next_sequence) {
                    erase.Erase(key);
                    erased = true;
                }
            }
            if (erased) db->WriteBatch(erase, true);
            // The newest retained rows; a hash the store contradicts wipes.
            const uint64_t from{store_next_sequence > FlowMeshObservedTimeLog::RING ? store_next_sequence - FlowMeshObservedTimeLog::RING : 0};
            std::vector<flowmesh::ObservedStamp> rows;
            std::map<uint64_t, uint256> expected{known.begin(), known.end()};
            bool mismatch{false};
            for (it->Seek(RowKey{KEY_ROW, market, from}); it->Valid(); it->Next()) {
                RowKey key;
                if (!it->GetKey(key) || key.prefix != KEY_ROW || key.market != market || key.sequence >= store_next_sequence) break;
                RowValue value;
                if (!it->GetValue(value)) continue;
                const auto stamp{FromRow(key.sequence, value)};
                if (!stamp) continue;
                if (const auto want{expected.find(key.sequence)}; want != expected.end() && want->second != stamp->hash) {
                    mismatch = true;
                    break;
                }
                rows.push_back(*stamp);
            }
            if (mismatch) {
                WipeMarket(*db, market);
                rows.clear();
            }
            it->Seek(RowKey{KEY_ROW, market, 0});
            if (it->Valid()) {
                RowKey key;
                if (it->GetKey(key) && key.prefix == KEY_ROW && key.market == market) item.oldest_row = key.sequence;
            }
            // The memory window of every interval, ending at the newest time.
            std::optional<int64_t> newest;
            for (auto row{rows.rbegin()}; row != rows.rend() && !newest; ++row) {
                if (row->mode != flowmesh::ObservedMode::UNTIMED) newest = row->ms;
            }
            std::array<std::vector<flowmesh::ObservedCandle>, flowmesh::CANDLE_INTERVALS.size()> candles;
            for (const auto interval : flowmesh::CANDLE_INTERVALS) {
                if (!newest || mismatch) break;
                const int64_t step{flowmesh::CandleIntervalMs(interval)};
                const int64_t window{static_cast<int64_t>(flowmesh::CandleMemoryWindow(interval)) * step};
                const int64_t start{std::max<int64_t>(0, flowmesh::CandleOpenMs(*newest, interval) - window)};
                auto& out{candles[static_cast<size_t>(interval)]};
                for (it->Seek(CandleKey{KEY_CANDLE, market, static_cast<uint8_t>(interval), static_cast<uint64_t>(start)}); it->Valid(); it->Next()) {
                    CandleKey key;
                    if (!it->GetKey(key) || key.prefix != KEY_CANDLE || key.market != market ||
                        key.interval != static_cast<uint8_t>(interval)) break;
                    CandleValue value;
                    if (!it->GetValue(value)) continue;
                    auto candle{FromValue(key.open_ms, interval, value)};
                    // Never a candle holding an entry the store does not have.
                    if (!candle || candle->last_sequence >= store_next_sequence) continue;
                    out.push_back(*candle);
                }
            }
            const auto recorded_since{mismatch ? std::nullopt : item.oldest_row};
            if (mismatch) item.oldest_row.reset();
            it.reset();
            disk_settled();
            finished = true;
            // A history FinishLoad failed to install (it then leaves the log
            // empty) is not what the disk holds: not persisted, as before
            // the load moved to this thread.
            log.SetPersisted(log.FinishLoad(std::move(rows), std::move(candles), recorded_since));
        } catch (const std::exception& e) {
            LogWarning("FlowMesh market %s: observed times could not be read (%s); recording starts empty\n",
                       market.GetHex(), e.what());
            disk_settled();
            finished = true;
            log.FinishLoad({}, {}, std::nullopt);
            log.SetPersisted(false);
        }
    } catch (...) {
        item.load.reset();
        disk_settled();
        if (!finished) log.FinishLoad({}, {}, std::nullopt);
        log.SetPersisted(false);
    }
}

void FlowMeshObservedTimeStore::Start()
{
    std::lock_guard lock{m_mutex};
    if (m_thread.joinable()) return;
    m_stopping = false;
    m_thread = std::thread{[this] { FlushLoop(); }};
}

void FlowMeshObservedTimeStore::Stop()
{
    {
        std::lock_guard lock{m_mutex};
        m_stopping = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    // Every runtime writer has stopped before the service calls this. A
    // market still queued is loaded (and erased) before its rows are written.
    FlushNow(/*sync=*/true);
}

void FlowMeshObservedTimeStore::FlushLoop()
{
    util::ThreadRename("flowmesh-obstime");
    // Opened here, never on the service's start path (display data only:
    // a failure leaves every log in memory).
    if (!IsOpen()) {
        std::string error;
        Open(error);
    }
    std::unique_lock lock{m_mutex};
    while (!m_stopping) {
        // A queued market is loaded at once, not after the interval.
        m_cv.wait_for(lock, m_options.flush_interval, [&] { return m_stopping || m_load_requested; });
        if (m_stopping) break;
        lock.unlock();
        FlushNow(/*sync=*/false);
        lock.lock();
    }
}

void FlowMeshObservedTimeStore::FlushNow(const bool sync) noexcept
{
    try {
        std::lock_guard flush{m_flush_mutex};
        std::vector<Attached*> attached;
        std::function<void()> hook;
        CDBWrapper* db{nullptr};
        {
            std::lock_guard lock{m_mutex};
            for (auto& item : m_attached) attached.push_back(&item);
            hook = m_hook;
            db = m_db.get();
            m_load_requested = false;
        }
        // Queued markets first, on this thread: a market's erasures and
        // wipes always reach the disk before any of its new rows.
        for (auto* item : attached) {
            if (item->load) RunLoad(*item, db);
        }
        const auto now{std::chrono::steady_clock::now()};
        for (auto* item : attached) {
            auto& log{*item->log};
            auto pending{log.TakePending()};
            if (pending.Empty() && now < item->next_sweep) continue;
            if (!db) continue;
            try {
                const uint256 market{log.MarketId()};
                CDBBatch batch{*db};
                std::optional<uint64_t> newest_row;
                for (const auto& row : pending.rows) {
                    batch.Write(RowKey{KEY_ROW, market, row.sequence}, ToRow(row));
                    newest_row = row.sequence;
                    if (!item->oldest_row || row.sequence < *item->oldest_row) item->oldest_row = row.sequence;
                }
                std::array<std::optional<int64_t>, flowmesh::CANDLE_INTERVALS.size()> newest_open;
                for (const auto& [key, candle] : pending.candles) {
                    if (key.first >= flowmesh::CANDLE_INTERVALS.size()) continue;
                    batch.Write(CandleKey{KEY_CANDLE, market, key.first, static_cast<uint64_t>(std::max<int64_t>(0, key.second))}, ToValue(candle));
                    auto& open{newest_open[key.first]};
                    open = std::max(open.value_or(key.second), key.second);
                }
                if (now >= item->next_sweep) {
                    item->next_sweep = now + SWEEP_INTERVAL;
                    // Retention, a bounded amount per sweep.
                    std::unique_ptr<CDBIterator> it{db->NewIterator()};
                    size_t erased{0};
                    if (newest_row && *newest_row >= ROW_RETENTION && item->oldest_row && *item->oldest_row <= *newest_row - ROW_RETENTION) {
                        const uint64_t keep_from{*newest_row - ROW_RETENTION + 1};
                        for (it->Seek(RowKey{KEY_ROW, market, *item->oldest_row}); it->Valid() && erased < SWEEP_ERASE_MAX; it->Next()) {
                            RowKey key;
                            if (!it->GetKey(key) || key.prefix != KEY_ROW || key.market != market || key.sequence >= keep_from) break;
                            batch.Erase(key);
                            ++erased;
                            item->oldest_row = key.sequence + 1;
                        }
                    }
                    for (const auto interval : flowmesh::CANDLE_INTERVALS) {
                        const auto newest{newest_open[static_cast<size_t>(interval)]};
                        if (!newest || *newest <= CandleRetentionMs(interval)) continue;
                        const int64_t cutoff{*newest - CandleRetentionMs(interval)};
                        for (it->Seek(CandleKey{KEY_CANDLE, market, static_cast<uint8_t>(interval), 0}); it->Valid() && erased < SWEEP_ERASE_MAX; it->Next()) {
                            CandleKey key;
                            if (!it->GetKey(key) || key.prefix != KEY_CANDLE || key.market != market ||
                                key.interval != static_cast<uint8_t>(interval) || static_cast<int64_t>(key.open_ms) >= cutoff) break;
                            batch.Erase(key);
                            ++erased;
                        }
                    }
                }
                if (hook) hook();
                db->WriteBatch(batch, sync);
                log.SetPersisted(true);
            } catch (const std::exception& e) {
                // Lost rows become a gap after a restart; the market is
                // never affected.
                LogDebug(BCLog::NET, "FlowMesh observed-time flush failed for market %s: %s\n", log.MarketId().GetHex(), e.what());
                log.SetPersisted(false);
            }
        }
    } catch (...) {
    }
}

std::vector<flowmesh::ObservedCandle> FlowMeshObservedTimeStore::ReadCandles(const flowmesh::MarketId& market,
                                                                             const flowmesh::CandleInterval interval,
                                                                             const int64_t from_ms, const int64_t before_ms,
                                                                             const size_t limit, bool* older) const
{
    if (older) *older = false;
    std::vector<flowmesh::ObservedCandle> out;
    try {
        CDBWrapper* db{nullptr};
        {
            std::lock_guard lock{m_mutex};
            // A queued market's disk may still hold rows and candles its
            // load will erase or wipe: none is served until then (a gap).
            for (const auto& item : m_attached) {
                if (item.loading && item.log->MarketId() == market) return out;
            }
            db = m_db.get();
        }
        // Read without m_mutex: the database outlives every reader (it is
        // freed only with the store) and LevelDB reads are safe beside the
        // flush thread's writes.
        if (!db || before_ms <= from_ms) return out;
        std::unique_ptr<CDBIterator> it{db->NewIterator()};
        const auto matches = [&](const CandleKey& key) {
            return key.prefix == KEY_CANDLE && key.market == market && key.interval == static_cast<uint8_t>(interval);
        };
        // Keys are unsigned: compared as such, a corrupt key at or past 2^63
        // ends the scan instead of reading as a negative open.
        const uint64_t from{static_cast<uint64_t>(std::max<int64_t>(0, from_ms))};
        const uint64_t before{static_cast<uint64_t>(std::max<int64_t>(0, before_ms))};
        if (older) {
            it->Seek(CandleKey{KEY_CANDLE, market, static_cast<uint8_t>(interval), 0});
            CandleKey key;
            *older = it->Valid() && it->GetKey(key) && matches(key) && key.open_ms < from;
        }
        for (it->Seek(CandleKey{KEY_CANDLE, market, static_cast<uint8_t>(interval), from}); it->Valid() && out.size() < limit; it->Next()) {
            CandleKey key;
            if (!it->GetKey(key) || !matches(key) || key.open_ms >= before) break;
            CandleValue value;
            if (!it->GetValue(value)) continue;
            if (auto candle{FromValue(key.open_ms, interval, value)}) out.push_back(*candle);
        }
    } catch (const std::exception&) {
        out.clear();
        if (older) *older = false;
    }
    return out;
}

void FlowMeshObservedTimeStore::SetFlushHookForTest(std::function<void()> hook)
{
    std::lock_guard lock{m_mutex};
    m_hook = std::move(hook);
}

} // namespace node
