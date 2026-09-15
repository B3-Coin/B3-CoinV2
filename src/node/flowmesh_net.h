// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef B3COIN_NODE_FLOWMESH_NET_H
#define B3COIN_NODE_FLOWMESH_NET_H

#include <flowmesh/p2p.h>
#include <node/flowmesh_delivery.h>
#include <util/fs.h>
#include <uint256.h>

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace node {
struct FlowMeshRuntimeRelay;

enum class FlowMeshNetReceiveTimeout { NONE, IDLE, FRAME };
//! Pure local policy: 30-second byte-idle timeout, five-minute whole-frame
//! deadline. Pending runtime admission has its separate bounded retry timer.
FlowMeshNetReceiveTimeout ClassifyFlowMeshNetReceiveTimeout(
    std::chrono::milliseconds idle_age, std::chrono::milliseconds frame_age,
    bool frame_active, bool admission_pending);

/** Opt-in transport policy, never a consensus or wallet configuration. */
struct FlowMeshNetConfig {
    uint256 domain;
    std::string bind_host{"127.0.0.1"};
    uint16_t port{5649};
    //! Compressed operator pubkey@numeric-address:port; an omitted pin is TOFU-free self-identity only.
    std::vector<std::string> peers;
    //! Independent network identity is stored here, never in a wallet/signing journal.
    fs::path datadir;
    std::string role{"observer"};
    bool enable_listen{true};
    size_t max_peers{16};
    //! Bounded local admission wait, not a consensus timeout (max 30 seconds).
    std::chrono::milliseconds ingress_retry_timeout{30000};
    //! Called outside transport locks. False records refused feedback; callers
    //! must retain their own bounded delivery deadline/relevance policy.
    std::function<bool(const FlowMeshDeliveryEvent&)> delivery_callback;
};

/** Local counters for critical, action and bulk traffic, respectively. */
struct FlowMeshNetTraffic {
    size_t queued_bytes{0}, queued_messages{0};
    uint64_t admitted{0}, rejected{0}, socket_written{0}, failed{0};
    uint64_t received{0}, ingress_retries{0}, sent_bytes{0}, received_bytes{0};
    uint64_t max_pass_work_us{0};
    uint64_t max_pass_bytes{0}, max_pass_operations{0};
};

struct FlowMeshNetPeer {
    flowmesh::WirePeerId id{-2};
    std::string address, operator_pubkey, role;
    bool inbound{false}, live{false}, actions{false}, bulk{false}, authenticated{false};
    size_t queued_bytes{0};
    uint64_t sent_messages{0}, received_messages{0}, dropped_messages{0};
    size_t pending_ingress_bytes{0};
    uint64_t ingress_retries{0};
    std::string last_ingress_retry;
    std::array<FlowMeshNetTraffic, 3> traffic;
};

/** Public connection diagnostics, not application delivery or quorum proofs. */
struct FlowMeshNetTargetChannel {
    std::string state{"queued"}, last_error;
    uint64_t attempts{0}, failures{0};
    int64_t retry_in_ms{0}, last_attempt_age_ms{-1};
    bool authenticated{false};
};
struct FlowMeshNetTarget {
    std::string address, operator_pubkey;
    bool runtime_added{false}, admitted_to_worker{false}, authenticated{false};
    std::array<FlowMeshNetTargetChannel, 3> channels;
};
struct FlowMeshNetConnectResult {
    bool accepted{false}, already_present{false};
    std::string address, operator_pubkey, error;
};

struct FlowMeshNetSnapshot {
    bool running{false}, listening{false};
    std::string operator_pubkey, bind_address, error;
    std::vector<FlowMeshNetPeer> peers;
    std::vector<FlowMeshNetTarget> targets;
    size_t pending_ingress_bytes{0}, outbox_queued_bytes{0};
    uint64_t ingress_retries{0}, ingress_discarded_messages{0}, egress_rejected_messages{0};
    std::string last_ingress_error, last_egress_error;
    uint64_t notification_refused{0};
    uint64_t trace_events_dropped{0}; //!< Process-lifetime optional BENCH trace-cap refusals.
    uint64_t receive_idle_timeouts{0}, receive_frame_timeouts{0};
    std::string last_disconnect_reason;
    std::array<FlowMeshNetTraffic, 3> traffic;
};

/**
 * Independent FMN2 three-channel authenticated TCP with separately reserved
 * critical, action and bulk sockets/queues. Inner application wire is unchanged.
 * Authentication proves operator-key possession, NOT FN eligibility. Existing
 * inner FlowMesh objects remain unchanged and the sink verifies consensus.
 * Relay is thread-safe and bounded (no network I/O). Opt-in BENCH diagnostics
 * perform synchronous debug logging outside the queue-admission lock. Snapshot
 * exposes public metadata only. Stop never waits while holding a chain lock.
 * Sink must outlive this service. Start/Stop must be externally serialized.
 */
class FlowMeshNetService {
public:
    FlowMeshNetService(FlowMeshNetConfig config, flowmesh::WireMessageSink& sink);
    ~FlowMeshNetService();
    FlowMeshNetService(const FlowMeshNetService&) = delete;
    FlowMeshNetService& operator=(const FlowMeshNetService&) = delete;
    bool Start(std::string& error);
    void Stop();
    //! Queue one pinned numeric endpoint for this service lifetime only. A
    //! successful return is bounded local admission, NOT TCP/authentication.
    //! Does not write configuration, touch wallet keys or restart the service.
    FlowMeshNetConnectResult AddPeer(const std::string& peer);
    //! Synchronous per-peer queue admission, never a claim of remote receipt.
    FlowMeshRelayResult Relay(const FlowMeshRuntimeRelay& relay);
    //! Retire this owner's delivery id. Partially written frames require a
    //! disconnect; their bytes are never spliced into another signed frame.
    void Cancel(uint64_t delivery_id);
    FlowMeshNetSnapshot Snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace node
#endif
