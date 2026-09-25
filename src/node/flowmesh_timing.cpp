// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <node/flowmesh_timing.h>
#include <logging.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace node {
namespace {
constexpr size_t MAX_EVENTS{32768}, MAX_BYTES{16 * 1024 * 1024};
std::atomic<bool> recording{false};
std::atomic<uint64_t> next_span{0};
std::atomic<uint64_t> errors{0};
std::mutex mutex;
std::vector<std::string> rows;
size_t bytes{0};
uint64_t generation{0}, start_us{0}, stop_us{0}, dropped{0};
thread_local uint64_t current_span{0};
uint64_t Now() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
bool FlowMeshTimingRecording() noexcept { return recording.load(std::memory_order_relaxed); }

void FlowMeshTimingEmit(const char* stream, const UniValue& event) noexcept
{
    try {
        if (LogAcceptCategory(BCLog::BENCH, BCLog::Level::Debug))
            LogDebug(BCLog::BENCH, "%s %s\n", stream, event.write());
        if (!FlowMeshTimingRecording()) return;
        UniValue wrapper{UniValue::VOBJ};
        wrapper.pushKV("stream", stream);
        wrapper.pushKV("event", event);
        auto encoded{wrapper.write()};
        // No disk I/O, no eviction, and no logger while BENCH is disabled.
        // This is serialized-byte accounting, not total allocator/heap usage.
        std::lock_guard lock{mutex};
        if (!recording.load(std::memory_order_relaxed)) return;
        if (rows.size() >= MAX_EVENTS || encoded.size() > MAX_BYTES - bytes) {
            ++dropped;
            return;
        }
        bytes += encoded.size();
        rows.push_back(std::move(encoded));
    } catch (...) { errors.fetch_add(1, std::memory_order_relaxed); }
}

UniValue FlowMeshTimingControl(const std::string& command)
{
    std::lock_guard lock{mutex};
    if (command == "start") {
        if (recording.load()) throw std::runtime_error("Timing capture already active; stop and preserve it first");
        rows.clear(); bytes = 0; dropped = 0; errors.store(0);
        ++generation; start_us = Now(); stop_us = 0;
        recording.store(true);
    } else if (command == "stop") {
        recording.store(false);
        if (!stop_us) stop_us = Now();
    } else if (command != "read") {
        throw std::runtime_error("Use start, stop or read");
    }
    UniValue result{UniValue::VOBJ}, events{UniValue::VARR};
    result.pushKV("enabled", recording.load());
    result.pushKV("generation", generation);
    result.pushKV("start_us", start_us); result.pushKV("stop_us", stop_us);
    result.pushKV("dropped", dropped + errors.load());
    result.pushKV("bytes", uint64_t{bytes});
    result.pushKV("max_bytes", uint64_t{MAX_BYTES});
    result.pushKV("max_events", uint64_t{MAX_EVENTS});
    // Freeze first. No large read/copy can hold a lock needed by active writers.
    if (command == "read" && recording.load())
        throw std::runtime_error("Stop capture before reading events");
    if (command != "start") {
        for (const auto& encoded : rows) {
            UniValue event;
            if (!event.read(encoded)) throw std::runtime_error("Malformed diagnostic record");
            events.push_back(std::move(event));
        }
    }
    result.pushKV("events", std::move(events));
    return result;
}

FlowMeshTimingSpan::FlowMeshTimingSpan(const char* stage) noexcept
    : m_enabled{FlowMeshTimingRecording()}
{
    if (!m_enabled) return;
    try {
        m_id = ++next_span; m_parent = current_span;
        m_row.pushKV("stage", stage);
        m_row.pushKV("started_us", Now());
        m_row.pushKV("span_id", m_id); m_row.pushKV("parent_span_id", m_parent);
        m_row.pushKV("thread_id", uint64_t{std::hash<std::thread::id>{}(std::this_thread::get_id())});
        current_span = m_id;
    } catch (...) { m_enabled = false; ++errors; }
}
FlowMeshTimingSpan::~FlowMeshTimingSpan()
{
    if (!m_enabled) return;
    current_span = m_parent;
    Mark("monotonic_us");
    FlowMeshTimingEmit("FlowMeshNativeSpan", m_row);
}
void FlowMeshTimingSpan::Mark(const char* field) noexcept { if (m_enabled) Field(field, Now()); }
void FlowMeshTimingSpan::Field(const char* field, uint64_t value) noexcept
{
    if (m_enabled) try { m_row.pushKV(field, value); } catch (...) { ++errors; }
}
void FlowMeshTimingSpan::Field(const char* field, const uint256& value) noexcept
{
    if (m_enabled) try { m_row.pushKV(field, value.GetHex()); } catch (...) { ++errors; }
}
void FlowMeshTimingSpan::Field(const char* field, const std::string& value) noexcept
{
    if (m_enabled) try { m_row.pushKV(field, value.substr(0, 128)); } catch (...) { ++errors; }
}
} // namespace node
