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
        row.delay > static_cast<uint8_t>(flowmesh::ObservedDelay::CLOCK_BACK) || row.ms < 0) return std::nullopt;
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

std::optional<flowmesh::ObservedCandle> FromValue(const int64_t open_ms, const CandleValue& value)
{
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

flowmesh::ObservedStamp FlowMeshObservedTimeLog::Record(const uint64_t sequence, const uint256& hash,
                                                        const flowmesh::ObservedDelay delay,
                                                        const std::optional<flowmesh::ClearingTick>& tick) noexcept
{
    try {
        flowmesh::ObservedStamp stamp;
        stamp.sequence = sequence;
        stamp.hash = hash;
        stamp.delay = delay;
        stamp.mode = delay == flowmesh::ObservedDelay::NONE ? flowmesh::ObservedMode::LIVE : flowmesh::ObservedMode::DELAYED;
        const int64_t wall{m_clocks.wall_ms()};
        const auto steady{m_clocks.steady()};
        if (m_clock_suspect.load(std::memory_order_relaxed)) stamp.flags |= flowmesh::OBSERVED_CLOCK_SUSPECT;
        if (!tick || tick->sequence != sequence || tick->microblock_hash != hash) stamp.flags |= flowmesh::OBSERVED_NO_CLEARING;
        std::lock_guard lock{m_mutex};
        if (m_ring.empty() || m_ring.back().sequence < sequence) {
            if (!m_previous) {
                stamp.flags |= flowmesh::OBSERVED_RESTART;
            } else {
                const int64_t wall_delta{wall - m_previous->first};
                const int64_t steady_delta{std::chrono::duration_cast<std::chrono::milliseconds>(steady - m_previous->second).count()};
                if (wall_delta - steady_delta > flowmesh::OBSERVED_CLOCK_SLEW_MS ||
                    steady_delta - wall_delta > flowmesh::OBSERVED_CLOCK_SLEW_MS) stamp.flags |= flowmesh::OBSERVED_CLOCK_STEP;
            }
            m_previous = std::pair{wall, steady};
            if (m_last_ms && wall < *m_last_ms) {
                if (*m_last_ms - wall <= flowmesh::OBSERVED_CLOCK_SLEW_MS) {
                    stamp.ms = *m_last_ms;
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
            if (stamp.mode != flowmesh::ObservedMode::UNTIMED) m_last_ms = stamp.ms;
            flowmesh::ObservedRollup::Changes changes;
            m_rollup.Add(stamp, tick, &changes);
            m_ring.push_back(stamp);
            if (m_ring.size() > RING) m_ring.pop_front();
            if (!m_recorded_since) m_recorded_since = sequence;
            m_pending.rows.push_back(stamp);
            if (m_pending.rows.size() > PENDING_ROWS_MAX) {
                m_pending.rows.erase(m_pending.rows.begin());
                ++m_rows_not_persisted;
            }
            for (const auto& [interval, candle] : changes) {
                m_pending.candles[{static_cast<uint8_t>(interval), candle.open_ms}] = candle;
            }
            // A restore or disk flush may never succeed. Keep only the same
            // bounded interval windows as the in-memory rollup, not every
            // bucket ever recorded while storage was unavailable. Dropping
            // whole optional snapshots leaves a history gap after restart;
            // it never removes the current candle or any signing evidence.
            for (const auto interval : flowmesh::CANDLE_INTERVALS) {
                const auto& retained{m_rollup.Candles(interval)};
                if (retained.empty()) continue;
                const auto id{static_cast<uint8_t>(interval)};
                auto it{m_pending.candles.lower_bound({id, 0})};
                while (it != m_pending.candles.end() && it->first.first == id &&
                       it->first.second < retained.front().open_ms) {
                    it = m_pending.candles.erase(it);
                }
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
    for (const auto& [sequence, hash] : entries) {
        const auto it{std::lower_bound(m_ring.begin(), m_ring.end(), sequence,
                                       [](const auto& stamp, const uint64_t value) { return stamp.sequence < value; })};
        if (it != m_ring.end() && it->sequence == sequence && it->hash == hash) out.push_back(*it);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.sequence < b.sequence; });
    out.erase(std::unique(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.sequence == b.sequence; }), out.end());
    return out;
}

flowmesh::ObservedCandlePage FlowMeshObservedTimeLog::Candles(const flowmesh::CandleInterval interval,
                                                              const std::optional<int64_t> before_ms, const size_t limit) const
{
    std::lock_guard lock{m_mutex};
    return m_rollup.Page(interval, before_ms, limit);
}

std::optional<int64_t> FlowMeshObservedTimeLog::OldestMemoryOpen(const flowmesh::CandleInterval interval) const
{
    std::lock_guard lock{m_mutex};
    const auto& candles{m_rollup.Candles(interval)};
    return candles.empty() ? std::nullopt : std::optional{candles.front().open_ms};
}

FlowMeshObservedTimeLog::Summary FlowMeshObservedTimeLog::GetSummary() const
{
    Summary out;
    {
        std::lock_guard lock{m_mutex};
        out.recorded_since = m_recorded_since;
        out.rows_not_persisted = m_rows_not_persisted;
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
    std::swap(out, m_pending);
    return out;
}

void FlowMeshObservedTimeLog::Load(std::vector<flowmesh::ObservedStamp> rows,
                                   std::array<std::vector<flowmesh::ObservedCandle>, flowmesh::CANDLE_INTERVALS.size()> candles,
                                   const std::optional<uint64_t> recorded_since, const bool preserve_recorded)
{
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.sequence < b.sequence; });
    if (rows.size() > RING) rows.erase(rows.begin(), rows.end() - RING);
    std::lock_guard lock{m_mutex};
    if (preserve_recorded && (!m_ring.empty() || !m_pending.Empty())) return;
    m_ring.assign(rows.begin(), rows.end());
    m_rollup = flowmesh::ObservedRollup{};
    for (const auto interval : flowmesh::CANDLE_INTERVALS) m_rollup.Restore(interval, std::move(candles[static_cast<size_t>(interval)]));
    m_last_ms.reset();
    std::optional<int64_t> last_live;
    for (auto it{m_ring.rbegin()}; it != m_ring.rend(); ++it) {
        if (!m_last_ms && it->mode != flowmesh::ObservedMode::UNTIMED) m_last_ms = it->ms;
        if (it->mode == flowmesh::ObservedMode::LIVE) {
            // Only a run of consecutive sequences ending at the newest row
            // bounds what follows.
            const bool contiguous{it->sequence + static_cast<uint64_t>(std::distance(m_ring.rbegin(), it)) == m_ring.back().sequence};
            if (contiguous) last_live = it->ms;
            break;
        }
    }
    m_rollup.RestoreLast(m_ring.empty() ? std::nullopt : std::optional{m_ring.back().sequence}, last_live);
    m_recorded_since = recorded_since ? recorded_since : m_ring.empty() ? std::nullopt : std::optional{m_ring.front().sequence};
    m_pending = {};
    m_previous.reset();
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

void FlowMeshObservedTimeStore::WipeMarket(const flowmesh::MarketId& market)
{
    // Caller holds m_flush_mutex; never the registration mutex during I/O.
    CDBBatch batch{*m_db};
    size_t erased{0};
    std::unique_ptr<CDBIterator> it{m_db->NewIterator()};
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
    m_db->WriteBatch(batch, true);
    LogWarning("FlowMesh market %s: observed times did not match this node's certified log and were cleared (%u records)\n",
               market.GetHex(), static_cast<unsigned>(erased));
}

void FlowMeshObservedTimeStore::Attach(const std::shared_ptr<FlowMeshObservedTimeLog>& log, const uint64_t store_next_sequence,
                                       const std::vector<std::pair<uint64_t, uint256>>& known) noexcept
{
    if (!log) return;
    try {
        auto attached{std::make_shared<Attached>()};
        attached->log = log;
        attached->restore = RestoreRequest{store_next_sequence, known};
        std::lock_guard flush{m_flush_mutex};
        if (RestoreAttached(*attached, /*preserve_recorded=*/false)) attached->restore.reset();
        std::lock_guard lock{m_mutex};
        RegisterAttached(std::move(attached));
    } catch (...) {
        log->SetPersisted(false);
    }
}

void FlowMeshObservedTimeStore::AttachAsync(const std::shared_ptr<FlowMeshObservedTimeLog>& log, const uint64_t store_next_sequence,
                                            std::vector<std::pair<uint64_t, uint256>> known) noexcept
{
    if (!log) return;
    log->SetPersisted(false);
    try {
        auto attached{std::make_shared<Attached>()};
        attached->log = log;
        attached->restore = RestoreRequest{store_next_sequence, std::move(known)};
        {
            std::lock_guard lock{m_mutex};
            RegisterAttached(std::move(attached));
            m_restore_queued = true;
        }
        m_cv.notify_all();
    } catch (...) {
        log->SetPersisted(false);
    }
}

void FlowMeshObservedTimeStore::RegisterAttached(std::shared_ptr<Attached> attached)
{
    const auto previous{std::find_if(m_attached.begin(), m_attached.end(), [&](const auto& item) {
        return item->log->MarketId() == attached->log->MarketId();
    })};
    if (previous == m_attached.end()) m_attached.push_back(std::move(attached));
    else *previous = std::move(attached);
}

bool FlowMeshObservedTimeStore::RestoreAttached(Attached& attached, const bool preserve_recorded) noexcept
{
    const auto& log{attached.log};
    const uint256 market{log->MarketId()};
    try {
        const uint64_t store_next_sequence{attached.restore->next_sequence};
        const auto& known{attached.restore->known};
        if (!m_db) {
            log->SetPersisted(false);
            return false;
        }
        MetaValue meta;
        const bool has_meta{m_db->Read(MetaKey{KEY_META, market}, meta)};
        if (has_meta && (meta.version != META_VERSION || meta.domain != log->Domain())) {
            WipeMarket(market);
        }
        m_db->Write(MetaKey{KEY_META, market}, MetaValue{META_VERSION, log->Domain()}, true);
        // Rows the production store never certified here are erased.
        std::unique_ptr<CDBIterator> it{m_db->NewIterator()};
        CDBBatch erase{*m_db};
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
        if (erased) m_db->WriteBatch(erase, true);
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
            WipeMarket(market);
            rows.clear();
        }
        it->Seek(RowKey{KEY_ROW, market, 0});
        if (it->Valid()) {
            RowKey key;
            if (it->GetKey(key) && key.prefix == KEY_ROW && key.market == market) attached.oldest_row = key.sequence;
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
                auto candle{FromValue(static_cast<int64_t>(key.open_ms), value)};
                // Never a candle holding an entry the store does not have.
                if (!candle || candle->last_sequence >= store_next_sequence) continue;
                out.push_back(*candle);
            }
        }
        const auto recorded_since{mismatch ? std::nullopt : attached.oldest_row};
        if (mismatch) attached.oldest_row.reset();
        log->Load(std::move(rows), std::move(candles), recorded_since, preserve_recorded);
        log->SetPersisted(true);
        attached.restored.store(true, std::memory_order_release);
        return true;
    } catch (const std::exception& e) {
        LogWarning("FlowMesh market %s: observed times could not be read (%s); recording continues in memory\n",
                   market.GetHex(), e.what());
    } catch (...) {
    }
    log->SetPersisted(false);
    return false;
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
    // Every runtime writer has stopped before the service calls this.
    FlushNow(/*sync=*/true);
}

void FlowMeshObservedTimeStore::FlushLoop()
{
    util::ThreadRename("flowmesh-obstime");
    std::unique_lock lock{m_mutex};
    while (!m_stopping) {
        m_cv.wait_for(lock, m_options.flush_interval, [&] { return m_stopping || m_restore_queued; });
        if (m_stopping) break;
        m_restore_queued = false;
        lock.unlock();
        FlushNow(/*sync=*/false);
        lock.lock();
    }
}

void FlowMeshObservedTimeStore::FlushNow(const bool sync) noexcept
{
    try {
        std::lock_guard flush{m_flush_mutex};
        std::vector<std::shared_ptr<Attached>> attached;
        std::function<void()> hook;
        {
            std::lock_guard lock{m_mutex};
            attached = m_attached;
            hook = m_hook;
        }
        const auto now{std::chrono::steady_clock::now()};
        for (const auto& item : attached) {
            if (item->restore) {
                if (!sync && now < item->retry_restore) continue;
                if (!RestoreAttached(*item, /*preserve_recorded=*/true)) {
                    // Optional restore failures retry at a bounded cadence;
                    // recording continues in its bounded memory queue.
                    item->retry_restore = now + std::chrono::seconds{5};
                    continue;
                }
                item->restore.reset();
            }
            auto& log{*item->log};
            auto pending{log.TakePending()};
            if (pending.Empty() && now < item->next_sweep) continue;
            if (!m_db) continue;
            try {
                const uint256 market{log.MarketId()};
                CDBBatch batch{*m_db};
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
                    std::unique_ptr<CDBIterator> it{m_db->NewIterator()};
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
                // One final synced batch below covers these writes and
                // every previous unsynced pass, even with no pending rows.
                m_db->WriteBatch(batch, false);
                log.SetPersisted(true);
            } catch (const std::exception& e) {
                // Lost rows become a gap after a restart; the market is
                // never affected.
                LogDebug(BCLog::NET, "FlowMesh observed-time flush failed for market %s: %s\n", log.MarketId().GetHex(), e.what());
                log.SetPersisted(false);
            }
        }
        if (sync && m_db) {
            try {
                CDBBatch final_sync{*m_db};
                if (hook) hook();
                m_db->WriteBatch(final_sync, true);
            } catch (const std::exception& e) {
                LogDebug(BCLog::NET, "FlowMesh observed-time final sync failed: %s\n", e.what());
                for (const auto& item : attached) item->log->SetPersisted(false);
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
            const bool restored{std::any_of(m_attached.begin(), m_attached.end(), [&](const auto& item) {
                return item->log->MarketId() == market && item->restored.load(std::memory_order_acquire);
            })};
            if (!restored) return out;
            db = m_db.get();
        }
        if (!db || before_ms <= from_ms) return out;
        std::unique_ptr<CDBIterator> it{db->NewIterator()};
        const auto matches = [&](const CandleKey& key) {
            return key.prefix == KEY_CANDLE && key.market == market && key.interval == static_cast<uint8_t>(interval);
        };
        if (older) {
            it->Seek(CandleKey{KEY_CANDLE, market, static_cast<uint8_t>(interval), 0});
            CandleKey key;
            *older = it->Valid() && it->GetKey(key) && matches(key) && static_cast<int64_t>(key.open_ms) < from_ms;
        }
        for (it->Seek(CandleKey{KEY_CANDLE, market, static_cast<uint8_t>(interval), static_cast<uint64_t>(std::max<int64_t>(0, from_ms))});
             it->Valid() && out.size() < limit; it->Next()) {
            CandleKey key;
            if (!it->GetKey(key) || !matches(key) || static_cast<int64_t>(key.open_ms) >= before_ms) break;
            CandleValue value;
            if (!it->GetValue(value)) continue;
            if (auto candle{FromValue(static_cast<int64_t>(key.open_ms), value)}) out.push_back(*candle);
        }
    } catch (const std::exception&) {
        out.clear();
    }
    return out;
}

void FlowMeshObservedTimeStore::SetFlushHookForTest(std::function<void()> hook)
{
    std::lock_guard lock{m_mutex};
    m_hook = std::move(hook);
}

} // namespace node
