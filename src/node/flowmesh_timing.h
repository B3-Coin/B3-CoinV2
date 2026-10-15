// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_NODE_FLOWMESH_TIMING_H
#define BITCOIN_NODE_FLOWMESH_TIMING_H

#include <kernel/lock_timing.h>
#include <univalue.h>
#include <uint256.h>
#include <cstdint>
#include <string>

namespace node {
// Default-off, bounded, regtest-RPC-controlled diagnostics. Never protocol state.
bool FlowMeshTimingRecording() noexcept;
void FlowMeshTimingEmit(const char* stream, const UniValue& event) noexcept;
UniValue FlowMeshTimingControl(const std::string& command);
//! Install only for regtest. The callbacks have static lifetime and no captures.
kernel::LockTimingCallbacks FlowMeshChainLockTimingCallbacks() noexcept;

class FlowMeshTimingSpan {
    bool m_enabled;
    uint64_t m_id{0}, m_parent{0};
    UniValue m_row{UniValue::VOBJ};
    uint64_t m_lock_acquired_us{0}, m_lock_releasing_us{0};
    bool m_lock_acquired{false}, m_lock_releasing{false};
    void LockAcquired() noexcept;
    void LockReleasing() noexcept;
    friend class FlowMeshLockHoldObservation;
public:
    explicit FlowMeshTimingSpan(const char* stage) noexcept;
    ~FlowMeshTimingSpan();
    FlowMeshTimingSpan(const FlowMeshTimingSpan&) = delete;
    FlowMeshTimingSpan& operator=(const FlowMeshTimingSpan&) = delete;
    bool Enabled() const noexcept { return m_enabled; }
    void Mark(const char* field) noexcept;
    void Field(const char* field, const uint256& value) noexcept;
    void Field(const char* field, uint64_t value) noexcept;
    void Field(const char* field, const std::string& value) noexcept;
};

/** Observation only: construct immediately after the actual lock guard and
 * destroy before it. The parent span must outlive that guard, so encoding and
 * emission happen after unlock, including early returns. This object does not
 * acquire a lock or establish an ownership/safety contract. */
class FlowMeshLockHoldObservation {
    FlowMeshTimingSpan& m_span;
public:
    explicit FlowMeshLockHoldObservation(FlowMeshTimingSpan& span) noexcept : m_span{span}
    {
        m_span.LockAcquired();
    }
    ~FlowMeshLockHoldObservation() { m_span.LockReleasing(); }
    FlowMeshLockHoldObservation(const FlowMeshLockHoldObservation&) = delete;
    FlowMeshLockHoldObservation& operator=(const FlowMeshLockHoldObservation&) = delete;
};
} // namespace node
#endif
