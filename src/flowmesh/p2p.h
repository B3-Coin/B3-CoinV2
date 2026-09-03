// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_P2P_H
#define B3COIN_FLOWMESH_P2P_H

#include <flowmesh/market.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace flowmesh {

/**
 * FlowMesh uses its own bounded application queues over an already-established
 * B3 P2P connection. The B3 message processor performs only the cheap framing
 * checks below and enqueues a bounded object; BLS verification and execution
 * are always performed by the FlowMesh worker.
 */
inline constexpr uint16_t FLOWMESH_WIRE_VERSION_V1{1};
inline constexpr size_t FLOWMESH_WIRE_HEADER_SIZE{2 + 32 + 8 + 8};
inline constexpr size_t FLOWMESH_ACTION_MAX_BYTES{4 * 1024};
inline constexpr size_t FLOWMESH_PROPOSAL_MAX_BYTES{2 * 1024 * 1024};
inline constexpr size_t FLOWMESH_CERTIFICATE_MAX_BYTES{
    FLOWMESH_PROPOSAL_MAX_BYTES + 1024};
inline constexpr size_t FLOWMESH_HELLO_MAX_BYTES{64 * 1024};
inline constexpr size_t FLOWMESH_CATCHUP_MAX_ENTRIES{64};
inline constexpr size_t FLOWMESH_CATCHUP_MAX_BYTES{4 * 1024 * 1024};
inline constexpr size_t FLOWMESH_ATTESTATION_BYTES{4 + 96};
inline constexpr size_t FLOWMESH_GET_BYTES{2 + 4};

// The queue must be able to hold one maximum legal catch-up response. Smaller
// production proposals/certificates therefore fit by construction too.
inline constexpr size_t FLOWMESH_QUEUE_PER_PEER_BYTES{
    FLOWMESH_CATCHUP_MAX_BYTES + FLOWMESH_WIRE_HEADER_SIZE};
inline constexpr size_t FLOWMESH_QUEUE_GLOBAL_BYTES{64 * 1024 * 1024};
inline constexpr size_t FLOWMESH_QUEUE_PER_PEER_ITEMS{4'096};
inline constexpr size_t FLOWMESH_QUEUE_GLOBAL_ITEMS{65'536};
inline constexpr size_t FLOWMESH_ACTION_POOL_PER_MARKET_COUNT{65'536};
inline constexpr size_t FLOWMESH_ACTION_POOL_PER_MARKET_BYTES{16 * 1024 * 1024};
inline constexpr size_t FLOWMESH_ACTION_POOL_PER_PEER_MARKET_COUNT{256};
inline constexpr size_t FLOWMESH_ACTION_POOL_PER_PEER_MARKET_BYTES{1024 * 1024};

inline constexpr double FLOWMESH_ACTION_TOKEN_COUNT_BURST{256.0};
inline constexpr double FLOWMESH_ACTION_TOKEN_BYTE_BURST{1024.0 * 1024.0};
inline constexpr double FLOWMESH_ACTION_TOKEN_COUNT_RATE{64.0};
inline constexpr double FLOWMESH_ACTION_TOKEN_BYTE_RATE{256.0 * 1024.0};
inline constexpr double FLOWMESH_COMMITTEE_TOKEN_BURST{32.0};
inline constexpr double FLOWMESH_COMMITTEE_TOKEN_RATE{8.0};
inline constexpr double FLOWMESH_CONTROL_TOKEN_BURST{64.0};
inline constexpr double FLOWMESH_CONTROL_TOKEN_RATE{16.0};

enum class WireMessageKind : uint8_t {
    HELLO = 0,
    ACTION,
    PROPOSAL,
    ATTESTATION,
    CERTIFICATE,
    GET,
    ENTRIES,
};

enum class WirePriority : uint8_t {
    CERTIFICATE_OR_ATTESTATION = 0,
    PROPOSAL = 1,
    ACTION = 2,
    CATCHUP = 3,
};

struct WireHeader {
    uint16_t version{FLOWMESH_WIRE_VERSION_V1};
    MarketId market_id;
    uint64_t epoch{0};
    uint64_t sequence{0};

    friend bool operator==(const WireHeader&, const WireHeader&) = default;
};

struct WireMessage {
    WireMessageKind kind{WireMessageKind::HELLO};
    WireHeader header;
    std::vector<unsigned char> payload;

    size_t MemoryUsage() const { return FLOWMESH_WIRE_HEADER_SIZE + payload.size(); }
    friend bool operator==(const WireMessage&, const WireMessage&) = default;
};

enum class WireCheck : uint8_t {
    OK = 0,
    UNKNOWN_COMMAND,
    BAD_VERSION,
    NULL_MARKET,
    WRONG_LENGTH,
    TOO_LARGE,
    BAD_CATCHUP_REQUEST,
    BAD_CATCHUP_RESPONSE,
};

std::optional<WireMessageKind> WireKindForCommand(std::string_view command);
std::string_view WireCommand(WireMessageKind kind);
WirePriority PriorityForWireKind(WireMessageKind kind);
size_t PayloadLimitForWireKind(WireMessageKind kind);
const char* WireCheckName(WireCheck check);

std::optional<std::vector<unsigned char>> EncodeWireMessage(const WireMessage& message,
                                                            WireCheck& check);
std::optional<WireMessage> DecodeWireMessage(WireMessageKind kind,
                                             std::span<const unsigned char> bytes,
                                             WireCheck& check);

/** fmentries payload: u16 count, then count * (u32 size || exact entry). */
std::optional<std::vector<unsigned char>> EncodeCatchupEntries(
    std::span<const std::vector<unsigned char>> entries);
std::optional<std::vector<std::vector<unsigned char>>> DecodeCatchupEntries(
    std::span<const unsigned char> payload);

/** fmget payload: u16 maximum entry count || u32 maximum response bytes. */
std::optional<std::vector<unsigned char>> EncodeCatchupRequest(uint16_t max_entries,
                                                               uint32_t max_bytes);
bool DecodeCatchupRequest(std::span<const unsigned char> payload, uint16_t& max_entries,
                          uint32_t& max_bytes);

using WirePeerId = int64_t;
using WireClock = std::chrono::steady_clock;

enum class QueueResult : uint8_t {
    ACCEPTED = 0,
    MALFORMED,
    RATE_LIMITED,
    PEER_LIMIT,
    MARKET_LIMIT,
    GLOBAL_LIMIT,
};

struct QueuedWireMessage {
    WirePeerId peer{0};
    WireMessage message;
};

/**
 * Four independent FIFO priority lanes. Admission is bounded globally, per
 * peer, per market, and per peer/market. High-priority votes/certificates may
 * evict queued lower-priority FlowMesh work, but no FlowMesh admission path
 * touches or waits for B3's validation queues.
 *
 * This object is intentionally not internally synchronized. The production
 * runtime owns it on one queue mutex and the worker pops outside cs_main.
 */
class BoundedWireQueue
{
public:
    QueueResult Push(WirePeerId peer, WireMessage message,
                     WireClock::time_point now = WireClock::now());
    std::optional<QueuedWireMessage> Pop();
    void RemovePeer(WirePeerId peer);

    size_t Size() const { return m_size; }
    size_t Bytes() const { return m_bytes; }
    size_t PeerBytes(WirePeerId peer) const;
    size_t PeerCount(WirePeerId peer) const;
    size_t MarketActionCount(const MarketId& market) const;
    bool Empty() const { return m_size == 0; }

private:
    struct Usage {
        size_t count{0};
        size_t bytes{0};
    };
    struct TokenBucket {
        double count_tokens{FLOWMESH_ACTION_TOKEN_COUNT_BURST};
        double byte_tokens{FLOWMESH_ACTION_TOKEN_BYTE_BURST};
        double committee_tokens{FLOWMESH_COMMITTEE_TOKEN_BURST};
        WireClock::time_point updated{};
        bool initialized{false};
    };
    struct ControlTokenBucket {
        double tokens{FLOWMESH_CONTROL_TOKEN_BURST};
        WireClock::time_point updated{};
        bool initialized{false};
    };

    using PeerMarket = std::pair<WirePeerId, MarketId>;

    bool ConsumeTokens(WirePeerId peer, const WireMessage& message,
                       WireClock::time_point now);
    bool EvictOneLowerPriority(WirePriority higher_than,
                               std::optional<WirePeerId> only_peer = std::nullopt);
    void AccountAdd(const QueuedWireMessage& item);
    void AccountRemove(const QueuedWireMessage& item);

    std::array<std::deque<QueuedWireMessage>, 4> m_lanes;
    std::map<WirePeerId, size_t> m_peer_bytes;
    std::map<WirePeerId, size_t> m_peer_count;
    std::map<MarketId, Usage> m_market_actions;
    std::map<PeerMarket, Usage> m_peer_market_actions;
    std::map<PeerMarket, TokenBucket> m_tokens;
    std::map<WirePeerId, ControlTokenBucket> m_control_tokens;
    size_t m_size{0};
    size_t m_bytes{0};
};

/** One outstanding bounded catch-up request is allowed per peer/market. */
class CatchupRequestTracker
{
public:
    bool Begin(WirePeerId peer, const MarketId& market, uint64_t from_sequence,
               uint16_t max_entries, uint32_t max_bytes);
    bool AcceptResponse(WirePeerId peer, const MarketId& market,
                        uint64_t from_sequence, size_t entry_count,
                        size_t response_bytes);
    void RemovePeer(WirePeerId peer);
    size_t Size() const { return m_requests.size(); }

private:
    struct Request {
        uint64_t from_sequence{0};
        uint16_t max_entries{0};
        uint32_t max_bytes{0};
    };
    std::map<std::pair<WirePeerId, MarketId>, Request> m_requests;
};

/** Asynchronous handoff from B3's cheap P2P parser to the FlowMesh worker. */
class WireMessageSink
{
public:
    virtual ~WireMessageSink() = default;
    virtual QueueResult EnqueueWireMessage(WirePeerId peer,
                                           WireMessage message) = 0;
    virtual void FlowMeshPeerDisconnected(WirePeerId peer) = 0;
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_P2P_H
