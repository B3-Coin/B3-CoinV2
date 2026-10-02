// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <node/flowmesh_observed_time_log.h>

#include <leveldb/env.h>
#include <leveldb/helpers/memenv/memenv.h>
#include <leveldb/status.h>
#include <test/util/setup_common.h>
#include <util/fs.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

using flowmesh::CandleInterval;
using flowmesh::ObservedDelay;
using node::FlowMeshObservedTimeLog;
using node::FlowMeshObservedTimeStore;

constexpr int64_t T0{1790769600000};
const uint256 TEST_DOMAIN{uint256{"00000000000000000000000000000000000000000000000000000000000000d0"}};
const flowmesh::MarketId MARKET{uint256{"00000000000000000000000000000000000000000000000000000000000000e1"}};

bool IsTable(const std::string& name) { return name.ends_with(".ldb") || name.ends_with(".sst"); }
bool IsManifest(const std::string& name) { return name.find("/MANIFEST-") != std::string::npos; }

/** Models the loss of bytes not protected by a successful Sync, using real
 * LevelDB files generated in a memory Env. This is a storage-contract test,
 * not an emulation of machine power loss or hardware caches. It retains the
 * state of closed and deleted WALs, and holds actual background callbacks so
 * log rotation cannot race the assertions. */
class DurabilityEnv final : public leveldb::EnvWrapper
{
public:
    struct FileState {
        std::string name;
        size_t written{0};
        size_t synced{0};
        bool alive{true};
    };

    explicit DurabilityEnv(leveldb::Env* backing) : EnvWrapper(backing) {}

    leveldb::Status NewWritableFile(const std::string& name, leveldb::WritableFile** result) override
    {
        std::lock_guard lock{m_mutex};
        if (m_frozen) return FrozenError();
        leveldb::WritableFile* file{nullptr};
        auto status{target()->NewWritableFile(name, &file)};
        if (status.ok()) {
            auto state{std::make_shared<FileState>()};
            state->name = name;
            m_files[name] = state;
            *result = new TrackedFile{*this, file, std::move(state)};
        }
        return status;
    }

    leveldb::Status NewAppendableFile(const std::string& name, leveldb::WritableFile** result) override
    {
        std::lock_guard lock{m_mutex};
        if (m_frozen) return FrozenError();
        leveldb::WritableFile* file{nullptr};
        auto status{target()->NewAppendableFile(name, &file)};
        if (status.ok()) {
            auto& state{m_files[name]};
            if (!state) {
                state = std::make_shared<FileState>();
                state->name = name;
                uint64_t size{0};
                if (target()->GetFileSize(name, &size).ok()) state->written = size;
            }
            state->alive = true;
            *result = new TrackedFile{*this, file, state};
        }
        return status;
    }

    leveldb::Status DeleteFile(const std::string& name) override
    {
        std::lock_guard lock{m_mutex};
        if (m_frozen) return FrozenError();
        auto status{target()->DeleteFile(name)};
        if (status.ok()) {
            if (auto it{m_files.find(name)}; it != m_files.end()) it->second->alive = false;
        }
        return status;
    }

    leveldb::Status RenameFile(const std::string& from, const std::string& to) override
    {
        std::lock_guard lock{m_mutex};
        if (m_frozen) return FrozenError();
        auto status{target()->RenameFile(from, to)};
        if (status.ok()) {
            if (auto it{m_files.find(from)}; it != m_files.end()) {
                auto state{it->second};
                m_files.erase(it);
                state->name = to;
                m_files[to] = std::move(state);
            }
        }
        return status;
    }

    void Schedule(void (*callback)(void*), void* argument) override
    {
        {
            std::lock_guard lock{m_mutex};
            if (m_hold_background) {
                m_scheduled.emplace_back(callback, argument);
                return;
            }
        }
        target()->Schedule(callback, argument);
    }

    void ReleaseBackground()
    {
        std::vector<std::pair<void (*)(void*), void*>> scheduled;
        {
            std::lock_guard lock{m_mutex};
            m_hold_background = false;
            scheduled.swap(m_scheduled);
        }
        for (const auto& [callback, argument] : scheduled) target()->Schedule(callback, argument);
    }

    size_t PendingBackground() const
    {
        std::lock_guard lock{m_mutex};
        return m_scheduled.size();
    }

    std::vector<FileState> Files() const
    {
        std::lock_guard lock{m_mutex};
        std::vector<FileState> files;
        for (const auto& [name, state] : m_files) files.push_back(*state);
        return files;
    }

    void FreezeWrites()
    {
        std::lock_guard lock{m_mutex};
        m_frozen = true;
    }

    //! Called after closing the DB: its destructor cannot make the captured
    //! state more durable, because FreezeWrites preceded the close.
    leveldb::Status LoseUnsyncedBytes()
    {
        std::lock_guard lock{m_mutex};
        for (const auto& [name, state] : m_files) {
            if (!state->alive || state->written == state->synced) continue;
            std::string contents;
            auto status{leveldb::ReadFileToString(target(), name, &contents)};
            if (!status.ok()) return status;
            contents.resize(std::min(contents.size(), state->synced));
            status = leveldb::WriteStringToFile(target(), contents, name);
            if (!status.ok()) return status;
            state->written = contents.size();
            state->synced = contents.size();
        }
        m_frozen = false;
        return leveldb::Status::OK();
    }

    void FailSync(const bool table, const bool manifest)
    {
        std::lock_guard lock{m_mutex};
        m_fail_table_sync = table;
        m_fail_manifest_sync = manifest;
    }

    unsigned FailedSyncs() const
    {
        std::lock_guard lock{m_mutex};
        return m_failed_syncs;
    }

private:
    static leveldb::Status FrozenError() { return leveldb::Status::IOError("modeled storage loss"); }

    class TrackedFile final : public leveldb::WritableFile
    {
    public:
        TrackedFile(DurabilityEnv& env, leveldb::WritableFile* file, std::shared_ptr<FileState> state)
            : m_env{env}, m_file{file}, m_state{std::move(state)} {}

        leveldb::Status Append(const leveldb::Slice& data) override
        {
            std::lock_guard lock{m_env.m_mutex};
            if (m_env.m_frozen) return FrozenError();
            auto status{m_file->Append(data)};
            if (status.ok()) m_state->written += data.size();
            return status;
        }

        leveldb::Status Close() override { return m_file->Close(); }

        leveldb::Status Flush() override
        {
            std::lock_guard lock{m_env.m_mutex};
            return m_env.m_frozen ? FrozenError() : m_file->Flush();
        }

        leveldb::Status Sync() override
        {
            std::lock_guard lock{m_env.m_mutex};
            if (m_env.m_frozen) return FrozenError();
            if ((m_env.m_fail_table_sync && IsTable(m_state->name)) ||
                (m_env.m_fail_manifest_sync && IsManifest(m_state->name))) {
                ++m_env.m_failed_syncs;
                return leveldb::Status::IOError("injected chart storage sync failure");
            }
            auto status{m_file->Sync()};
            if (status.ok()) m_state->synced = m_state->written;
            return status;
        }

        std::string GetName() const override { return m_file->GetName(); }

    private:
        DurabilityEnv& m_env;
        std::unique_ptr<leveldb::WritableFile> m_file;
        std::shared_ptr<FileState> m_state;
    };

    mutable std::mutex m_mutex;
    std::map<std::string, std::shared_ptr<FileState>> m_files;
    std::vector<std::pair<void (*)(void*), void*>> m_scheduled;
    bool m_hold_background{true};
    bool m_frozen{false};
    bool m_fail_table_sync{false};
    bool m_fail_manifest_sync{false};
    unsigned m_failed_syncs{0};
};

//! Release queued work before the store's destructor on an assertion failure.
struct ReleaseBackgroundOnExit {
    DurabilityEnv& env;
    ~ReleaseBackgroundOnExit() { env.ReleaseBackground(); }
};

struct Clocks {
    int64_t wall{T0};
    int64_t steady{1'000'000};

    FlowMeshObservedTimeLog::Clocks Get()
    {
        return {[this] { return wall; }, [this] {
                    return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds{steady};
                }};
    }

    void Advance() { ++wall; ++steady; }
};

uint256 EntryHash(const uint64_t sequence)
{
    uint256 hash;
    hash.data()[0] = static_cast<unsigned char>(sequence & 0xff);
    hash.data()[1] = static_cast<unsigned char>((sequence >> 8) & 0xff);
    hash.data()[31] = 1;
    return hash;
}

void Record(FlowMeshObservedTimeLog& log, Clocks& clocks, const uint64_t sequence)
{
    flowmesh::ClearingTick tick;
    tick.sequence = sequence;
    tick.microblock_hash = EntryHash(sequence);
    tick.cleared = true;
    tick.price = 10;
    tick.volume = 1;
    log.Record(sequence, EntryHash(sequence), ObservedDelay::NONE, tick);
    clocks.Advance();
}

FlowMeshObservedTimeStore::Options Options(const fs::path& path, DurabilityEnv& env)
{
    FlowMeshObservedTimeStore::Options options;
    options.path = path;
    options.cache_bytes = 256 << 10; // The wrapper's memtable is 64 KiB.
    options.env_for_test = &env;
    return options;
}

std::vector<std::pair<uint64_t, uint256>> Known(const uint64_t count)
{
    std::vector<std::pair<uint64_t, uint256>> known;
    for (uint64_t sequence{0}; sequence < count; ++sequence) known.emplace_back(sequence, EntryHash(sequence));
    return known;
}

void CheckSyncFailure(const fs::path& path, const bool table)
{
    std::unique_ptr<leveldb::Env> storage{leveldb::NewMemEnv(leveldb::Env::Default())};
    DurabilityEnv env{storage.get()};
    env.ReleaseBackground();
    FlowMeshObservedTimeStore store{Options(path, env)};
    std::string error;
    BOOST_REQUIRE(store.Open(error));
    Clocks clocks;
    const auto log{std::make_shared<FlowMeshObservedTimeLog>(TEST_DOMAIN, MARKET, clocks.Get())};
    store.Attach(log, 0, {});
    store.FlushNow(false);
    Record(*log, clocks, 0);
    store.FlushNow(false);
    BOOST_REQUIRE(log->GetSummary().persisted);
    env.FailSync(/*table=*/table, /*manifest=*/!table);
    store.Stop();
    BOOST_CHECK_GT(env.FailedSyncs(), 0U);
    // CompactRange returns void. Its stored table/manifest error must reach
    // the checked final write even though no chart rows remain pending.
    BOOST_CHECK(!log->GetSummary().persisted);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_observed_time_durability_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(stop_protects_rotated_wal_and_current_memtable)
{
    const auto path{m_path_root / "observed_rotated_wal"};
    std::unique_ptr<leveldb::Env> storage{leveldb::NewMemEnv(leveldb::Env::Default())};
    DurabilityEnv env{storage.get()};
    auto store{std::make_unique<FlowMeshObservedTimeStore>(Options(path, env))};
    const ReleaseBackgroundOnExit release_background{env};
    std::string error;
    BOOST_REQUIRE(store->Open(error));
    Clocks clocks;
    const auto log{std::make_shared<FlowMeshObservedTimeLog>(TEST_DOMAIN, MARKET, clocks.Get())};
    store->Attach(log, 0, {});
    store->FlushNow(false);

    constexpr uint64_t ROWS{2049};
    for (uint64_t sequence{0}; sequence < ROWS - 1; ++sequence) Record(*log, clocks, sequence);
    store->FlushNow(false);
    // One write exceeds the buffer; the next rotates it before writing its
    // own row/candles to the new WAL. Neither table job can run yet.
    Record(*log, clocks, ROWS - 1);
    store->FlushNow(false);
    const auto before{env.Files()};
    std::vector<DurabilityEnv::FileState> logs;
    for (const auto& file : before) {
        if (file.alive && file.name.ends_with(".log")) logs.push_back(file);
    }
    // These requirements establish the storage condition under test, rather
    // than relying on a flush hook's call count or a compaction race.
    BOOST_REQUIRE_EQUAL(logs.size(), 2U);
    BOOST_REQUIRE_GT(logs.front().written, logs.front().synced);
    BOOST_REQUIRE_GT(logs.back().written, logs.back().synced);
    BOOST_REQUIRE_EQUAL(env.PendingBackground(), 1U);
    BOOST_REQUIRE(std::none_of(before.begin(), before.end(), [](const auto& file) {
        return file.alive && IsTable(file.name);
    }));
    const auto expected_rows{log->Lookup(Known(ROWS))};
    BOOST_REQUIRE_EQUAL(expected_rows.size(), ROWS);
    const auto expected_candles{store->ReadCandles(MARKET, CandleInterval::M1, T0 - 1, T0 + 60'000, 8)};
    BOOST_REQUIRE_EQUAL(expected_candles.size(), 1U);
    BOOST_REQUIRE_EQUAL(expected_candles.front().entries, ROWS);

    auto stopped{std::async(std::launch::async, [&] { store->Stop(); })};
    const bool returned_before_background{stopped.wait_for(std::chrono::milliseconds{100}) == std::future_status::ready};
    BOOST_CHECK(!returned_before_background);
    // On the old empty-sync implementation, freeze at its premature return
    // before releasing the callback: cleanup must not repair the lost tail.
    if (returned_before_background) env.FreezeWrites();
    env.ReleaseBackground();
    BOOST_REQUIRE(stopped.wait_for(std::chrono::seconds{10}) == std::future_status::ready);
    stopped.get();
    BOOST_CHECK(log->GetSummary().persisted);
    env.FreezeWrites();
    const auto after{env.Files()};
    const auto older_log{std::find_if(after.begin(), after.end(), [&](const auto& file) {
        return file.name == logs.front().name;
    })};
    BOOST_REQUIRE(older_log != after.end());
    BOOST_CHECK(!older_log->alive);
    BOOST_CHECK(std::any_of(after.begin(), after.end(), [](const auto& file) {
        return file.alive && IsTable(file.name) && file.written > 0;
    }));
    for (const auto& file : after) {
        if (file.alive && (file.name.ends_with(".log") || IsTable(file.name) || IsManifest(file.name) || file.name.ends_with("/CURRENT"))) {
            BOOST_CHECK_EQUAL(file.written, file.synced);
        }
    }
    store.reset();
    BOOST_REQUIRE(env.LoseUnsyncedBytes().ok());

    FlowMeshObservedTimeStore reopened{Options(path, env)};
    BOOST_REQUIRE(reopened.Open(error));
    const auto recovered{std::make_shared<FlowMeshObservedTimeLog>(TEST_DOMAIN, MARKET, clocks.Get())};
    reopened.Attach(recovered, ROWS, Known(ROWS));
    reopened.FlushNow(false);
    const auto rows{recovered->Lookup(Known(ROWS))};
    BOOST_REQUIRE_EQUAL(rows.size(), expected_rows.size());
    for (size_t i{0}; i < rows.size(); ++i) {
        BOOST_CHECK_EQUAL(rows[i].sequence, expected_rows[i].sequence);
        BOOST_CHECK(rows[i].hash == expected_rows[i].hash);
        BOOST_CHECK_EQUAL(rows[i].ms, expected_rows[i].ms);
        BOOST_CHECK_EQUAL(rows[i].flags, expected_rows[i].flags);
        BOOST_CHECK(rows[i].mode == expected_rows[i].mode);
        BOOST_CHECK(rows[i].delay == expected_rows[i].delay);
    }
    const auto candles{reopened.ReadCandles(MARKET, CandleInterval::M1, T0 - 1, T0 + 60'000, 8)};
    BOOST_REQUIRE_EQUAL(candles.size(), expected_candles.size());
    BOOST_CHECK_EQUAL(candles.front().first_sequence, expected_candles.front().first_sequence);
    BOOST_CHECK_EQUAL(candles.front().last_sequence, expected_candles.front().last_sequence);
    BOOST_CHECK_EQUAL(candles.front().entries, expected_candles.front().entries);
    BOOST_CHECK_EQUAL(candles.front().clearings, expected_candles.front().clearings);
    BOOST_CHECK(candles.front().base_volume == expected_candles.front().base_volume);
    BOOST_CHECK(candles.front().quote_volume == expected_candles.front().quote_volume);
}

BOOST_AUTO_TEST_CASE(stop_reports_table_sync_failure)
{
    CheckSyncFailure(m_path_root / "observed_table_sync_failure", /*table=*/true);
}

BOOST_AUTO_TEST_CASE(stop_reports_manifest_sync_failure)
{
    CheckSyncFailure(m_path_root / "observed_manifest_sync_failure", /*table=*/false);
}

BOOST_AUTO_TEST_SUITE_END()
