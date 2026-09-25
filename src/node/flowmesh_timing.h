// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_NODE_FLOWMESH_TIMING_H
#define BITCOIN_NODE_FLOWMESH_TIMING_H

#include <univalue.h>
#include <uint256.h>
#include <cstdint>
#include <string>

namespace node {
// Default-off, bounded, regtest-RPC-controlled diagnostics. Never protocol state.
bool FlowMeshTimingRecording() noexcept;
void FlowMeshTimingEmit(const char* stream, const UniValue& event) noexcept;
UniValue FlowMeshTimingControl(const std::string& command);

class FlowMeshTimingSpan {
    bool m_enabled;
    uint64_t m_id{0}, m_parent{0};
    UniValue m_row{UniValue::VOBJ};
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
} // namespace node
#endif
