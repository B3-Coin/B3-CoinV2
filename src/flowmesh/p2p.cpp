// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/p2p.h>

#include <crypto/common.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace flowmesh {
namespace {

void AppendU16(std::vector<unsigned char>& out, uint16_t value)
{
    const size_t pos{out.size()};
    out.resize(pos + 2);
    WriteBE16(out.data() + pos, value);
}

void AppendU32(std::vector<unsigned char>& out, uint32_t value)
{
    const size_t pos{out.size()};
    out.resize(pos + 4);
    WriteBE32(out.data() + pos, value);
}

void AppendU64(std::vector<unsigned char>& out, uint64_t value)
{
    const size_t pos{out.size()};
    out.resize(pos + 8);
    WriteBE64(out.data() + pos, value);
}

bool PayloadShapeValid(const WireMessageKind kind,
                       const std::span<const unsigned char> payload)
{
    switch (kind) {
    case WireMessageKind::HELLO:
        return !payload.empty() && payload.size() <= FLOWMESH_HELLO_MAX_BYTES;
    case WireMessageKind::ACTION:
        return !payload.empty() && payload.size() <= FLOWMESH_ACTION_MAX_BYTES;
    case WireMessageKind::PROPOSAL:
        return !payload.empty() && payload.size() <= FLOWMESH_PROPOSAL_MAX_BYTES;
    case WireMessageKind::ATTESTATION:
        return payload.size() == FLOWMESH_ATTESTATION_BYTES;
    case WireMessageKind::CERTIFICATE:
        return !payload.empty() && payload.size() <= FLOWMESH_CERTIFICATE_MAX_BYTES;
    case WireMessageKind::GET: {
        uint16_t count{0};
        uint32_t bytes{0};
        return DecodeCatchupRequest(payload, count, bytes);
    }
    case WireMessageKind::ENTRIES: {
        // Framing runs on the B3 message-processing thread. Validate the
        // complete length/count structure without allocating entry vectors;
        // the FlowMesh worker performs the allocating decode after enqueue.
        if (payload.size() < 2 || payload.size() > FLOWMESH_CATCHUP_MAX_BYTES) {
            return false;
        }
        const uint16_t count{ReadBE16(payload.data())};
        if (count == 0 || count > FLOWMESH_CATCHUP_MAX_ENTRIES) return false;
        size_t cursor{2};
        for (uint16_t i{0}; i < count; ++i) {
            if (cursor > payload.size() || payload.size() - cursor < 4) return false;
            const uint32_t size{ReadBE32(payload.data() + cursor)};
            cursor += 4;
            if (size == 0 || size > FLOWMESH_PROPOSAL_MAX_BYTES ||
                cursor > payload.size() || payload.size() - cursor < size) {
                return false;
            }
            cursor += size;
        }
        return cursor == payload.size();
    }
    }
    return false;
}

} // namespace

std::optional<WireMessageKind> WireKindForCommand(const std::string_view command)
{
    if (command == "fmhello") return WireMessageKind::HELLO;
    if (command == "fmaction") return WireMessageKind::ACTION;
    if (command == "fmprop") return WireMessageKind::PROPOSAL;
    if (command == "fmattest") return WireMessageKind::ATTESTATION;
    if (command == "fmcert") return WireMessageKind::CERTIFICATE;
    if (command == "fmget") return WireMessageKind::GET;
    if (command == "fmentries") return WireMessageKind::ENTRIES;
    return std::nullopt;
}

std::string_view WireCommand(const WireMessageKind kind)
{
    switch (kind) {
    case WireMessageKind::HELLO: return "fmhello";
    case WireMessageKind::ACTION: return "fmaction";
    case WireMessageKind::PROPOSAL: return "fmprop";
    case WireMessageKind::ATTESTATION: return "fmattest";
    case WireMessageKind::CERTIFICATE: return "fmcert";
    case WireMessageKind::GET: return "fmget";
    case WireMessageKind::ENTRIES: return "fmentries";
    }
    return {};
}

WirePriority PriorityForWireKind(const WireMessageKind kind)
{
    switch (kind) {
    case WireMessageKind::ATTESTATION:
    case WireMessageKind::CERTIFICATE:
        return WirePriority::CERTIFICATE_OR_ATTESTATION;
    case WireMessageKind::PROPOSAL:
        return WirePriority::PROPOSAL;
    case WireMessageKind::ACTION:
        return WirePriority::ACTION;
    case WireMessageKind::HELLO:
    case WireMessageKind::GET:
    case WireMessageKind::ENTRIES:
        return WirePriority::CATCHUP;
    }
    return WirePriority::CATCHUP;
}

size_t PayloadLimitForWireKind(const WireMessageKind kind)
{
    switch (kind) {
    case WireMessageKind::HELLO: return FLOWMESH_HELLO_MAX_BYTES;
    case WireMessageKind::ACTION: return FLOWMESH_ACTION_MAX_BYTES;
    case WireMessageKind::PROPOSAL: return FLOWMESH_PROPOSAL_MAX_BYTES;
    case WireMessageKind::ATTESTATION: return FLOWMESH_ATTESTATION_BYTES;
    case WireMessageKind::CERTIFICATE: return FLOWMESH_CERTIFICATE_MAX_BYTES;
    case WireMessageKind::GET: return FLOWMESH_GET_BYTES;
    case WireMessageKind::ENTRIES: return FLOWMESH_CATCHUP_MAX_BYTES;
    }
    return 0;
}

const char* WireCheckName(const WireCheck check)
{
    switch (check) {
    case WireCheck::OK: return "ok";
    case WireCheck::UNKNOWN_COMMAND: return "unknown-command";
    case WireCheck::BAD_VERSION: return "bad-version";
    case WireCheck::NULL_MARKET: return "null-market";
    case WireCheck::WRONG_LENGTH: return "wrong-length";
    case WireCheck::TOO_LARGE: return "too-large";
    case WireCheck::BAD_CATCHUP_REQUEST: return "bad-catchup-request";
    case WireCheck::BAD_CATCHUP_RESPONSE: return "bad-catchup-response";
    }
    return "unknown";
}

std::optional<std::vector<unsigned char>> EncodeWireMessage(
    const WireMessage& message, WireCheck& check)
{
    check = WireCheck::OK;
    if (message.header.version != FLOWMESH_WIRE_VERSION_V1) {
        check = WireCheck::BAD_VERSION;
        return std::nullopt;
    }
    if (message.header.market_id.IsNull()) {
        check = WireCheck::NULL_MARKET;
        return std::nullopt;
    }
    if (message.payload.size() > PayloadLimitForWireKind(message.kind)) {
        check = WireCheck::TOO_LARGE;
        return std::nullopt;
    }
    if (!PayloadShapeValid(message.kind, message.payload)) {
        check = message.kind == WireMessageKind::GET
                    ? WireCheck::BAD_CATCHUP_REQUEST
                    : message.kind == WireMessageKind::ENTRIES
                          ? WireCheck::BAD_CATCHUP_RESPONSE
                          : WireCheck::WRONG_LENGTH;
        return std::nullopt;
    }
    std::vector<unsigned char> out;
    out.reserve(FLOWMESH_WIRE_HEADER_SIZE + message.payload.size());
    AppendU16(out, message.header.version);
    out.insert(out.end(), message.header.market_id.begin(),
               message.header.market_id.end());
    AppendU64(out, message.header.epoch);
    AppendU64(out, message.header.sequence);
    out.insert(out.end(), message.payload.begin(), message.payload.end());
    return out;
}

std::optional<WireMessage> DecodeWireMessage(const WireMessageKind kind,
                                             const std::span<const unsigned char> bytes,
                                             WireCheck& check)
{
    check = WireCheck::OK;
    const size_t limit{PayloadLimitForWireKind(kind)};
    if (bytes.size() < FLOWMESH_WIRE_HEADER_SIZE) {
        check = WireCheck::WRONG_LENGTH;
        return std::nullopt;
    }
    if (bytes.size() - FLOWMESH_WIRE_HEADER_SIZE > limit) {
        check = WireCheck::TOO_LARGE;
        return std::nullopt;
    }
    WireMessage out;
    out.kind = kind;
    out.header.version = ReadBE16(bytes.data());
    if (out.header.version != FLOWMESH_WIRE_VERSION_V1) {
        check = WireCheck::BAD_VERSION;
        return std::nullopt;
    }
    std::copy(bytes.begin() + 2, bytes.begin() + 34, out.header.market_id.begin());
    if (out.header.market_id.IsNull()) {
        check = WireCheck::NULL_MARKET;
        return std::nullopt;
    }
    out.header.epoch = ReadBE64(bytes.data() + 34);
    out.header.sequence = ReadBE64(bytes.data() + 42);
    const auto payload{bytes.subspan(FLOWMESH_WIRE_HEADER_SIZE)};
    if (!PayloadShapeValid(kind, payload)) {
        check = kind == WireMessageKind::GET
                    ? WireCheck::BAD_CATCHUP_REQUEST
                    : kind == WireMessageKind::ENTRIES
                          ? WireCheck::BAD_CATCHUP_RESPONSE
                          : WireCheck::WRONG_LENGTH;
        return std::nullopt;
    }
    out.payload.assign(payload.begin(), payload.end());
    return out;
}

std::optional<std::vector<unsigned char>> EncodeCatchupEntries(
    const std::span<const std::vector<unsigned char>> entries)
{
    if (entries.empty() || entries.size() > FLOWMESH_CATCHUP_MAX_ENTRIES) {
        return std::nullopt;
    }
    size_t total{2};
    for (const auto& entry : entries) {
        if (entry.empty() || entry.size() > FLOWMESH_PROPOSAL_MAX_BYTES ||
            entry.size() > std::numeric_limits<uint32_t>::max() ||
            total > FLOWMESH_CATCHUP_MAX_BYTES - 4 - entry.size()) {
            return std::nullopt;
        }
        total += 4 + entry.size();
    }
    std::vector<unsigned char> out;
    out.reserve(total);
    AppendU16(out, static_cast<uint16_t>(entries.size()));
    for (const auto& entry : entries) {
        AppendU32(out, static_cast<uint32_t>(entry.size()));
        out.insert(out.end(), entry.begin(), entry.end());
    }
    return out;
}

std::optional<std::vector<std::vector<unsigned char>>> DecodeCatchupEntries(
    const std::span<const unsigned char> payload)
{
    if (payload.size() < 2 || payload.size() > FLOWMESH_CATCHUP_MAX_BYTES) {
        return std::nullopt;
    }
    const uint16_t count{ReadBE16(payload.data())};
    if (count == 0 || count > FLOWMESH_CATCHUP_MAX_ENTRIES) return std::nullopt;
    size_t cursor{2};
    std::vector<std::vector<unsigned char>> out;
    out.reserve(count);
    for (uint16_t i{0}; i < count; ++i) {
        if (cursor > payload.size() || payload.size() - cursor < 4) {
            return std::nullopt;
        }
        const uint32_t size{ReadBE32(payload.data() + cursor)};
        cursor += 4;
        if (size == 0 || size > FLOWMESH_PROPOSAL_MAX_BYTES ||
            cursor > payload.size() || payload.size() - cursor < size) {
            return std::nullopt;
        }
        out.emplace_back(payload.begin() + cursor, payload.begin() + cursor + size);
        cursor += size;
    }
    if (cursor != payload.size()) return std::nullopt;
    return out;
}

std::optional<std::vector<unsigned char>> EncodeCatchupRequest(
    const uint16_t max_entries, const uint32_t max_bytes)
{
    if (max_entries == 0 || max_entries > FLOWMESH_CATCHUP_MAX_ENTRIES ||
        max_bytes == 0 || max_bytes > FLOWMESH_CATCHUP_MAX_BYTES) {
        return std::nullopt;
    }
    std::vector<unsigned char> out;
    out.reserve(FLOWMESH_GET_BYTES);
    AppendU16(out, max_entries);
    AppendU32(out, max_bytes);
    return out;
}

bool DecodeCatchupRequest(const std::span<const unsigned char> payload,
                          uint16_t& max_entries, uint32_t& max_bytes)
{
    if (payload.size() != FLOWMESH_GET_BYTES) return false;
    max_entries = ReadBE16(payload.data());
    max_bytes = ReadBE32(payload.data() + 2);
    return max_entries > 0 && max_entries <= FLOWMESH_CATCHUP_MAX_ENTRIES &&
           max_bytes > 0 && max_bytes <= FLOWMESH_CATCHUP_MAX_BYTES;
}

bool BoundedWireQueue::ConsumeTokens(const WirePeerId peer,
                                     const WireMessage& message,
                                     const WireClock::time_point now)
{
    TokenBucket& bucket{m_tokens[{peer, message.header.market_id}]};
    if (!bucket.initialized) {
        bucket.updated = now;
        bucket.initialized = true;
    } else if (now > bucket.updated) {
        const double seconds{
            std::chrono::duration<double>(now - bucket.updated).count()};
        bucket.count_tokens = std::min(FLOWMESH_ACTION_TOKEN_COUNT_BURST,
                                       bucket.count_tokens +
                                           seconds * FLOWMESH_ACTION_TOKEN_COUNT_RATE);
        bucket.byte_tokens = std::min(FLOWMESH_ACTION_TOKEN_BYTE_BURST,
                                      bucket.byte_tokens +
                                          seconds * FLOWMESH_ACTION_TOKEN_BYTE_RATE);
        bucket.committee_tokens = std::min(
            FLOWMESH_COMMITTEE_TOKEN_BURST,
            bucket.committee_tokens + seconds * FLOWMESH_COMMITTEE_TOKEN_RATE);
        bucket.updated = now;
    }
    if (message.kind == WireMessageKind::ACTION) {
        if (bucket.count_tokens < 1.0 ||
            bucket.byte_tokens < static_cast<double>(message.MemoryUsage())) {
            return false;
        }
        bucket.count_tokens -= 1.0;
        bucket.byte_tokens -= static_cast<double>(message.MemoryUsage());
    } else if (message.kind == WireMessageKind::PROPOSAL ||
               message.kind == WireMessageKind::ATTESTATION ||
               message.kind == WireMessageKind::CERTIFICATE) {
        if (bucket.committee_tokens < 1.0) return false;
        bucket.committee_tokens -= 1.0;
    } else {
        // HELLO/GET/ENTRIES are cheap but otherwise permit a tiny-frame queue
        // flood. Limit them per peer (not per caller-selected market id).
        ControlTokenBucket& control{m_control_tokens[peer]};
        if (!control.initialized) {
            control.updated = now;
            control.initialized = true;
        } else if (now > control.updated) {
            const double seconds{
                std::chrono::duration<double>(now - control.updated).count()};
            control.tokens = std::min(FLOWMESH_CONTROL_TOKEN_BURST,
                                      control.tokens +
                                          seconds * FLOWMESH_CONTROL_TOKEN_RATE);
            control.updated = now;
        }
        if (control.tokens < 1.0) return false;
        control.tokens -= 1.0;
    }
    return true;
}

void BoundedWireQueue::AccountAdd(const QueuedWireMessage& item)
{
    const size_t bytes{item.message.MemoryUsage()};
    ++m_size;
    m_bytes += bytes;
    m_peer_bytes[item.peer] += bytes;
    ++m_peer_count[item.peer];
    if (item.message.kind == WireMessageKind::ACTION) {
        Usage& market{m_market_actions[item.message.header.market_id]};
        ++market.count;
        market.bytes += bytes;
        Usage& peer_market{
            m_peer_market_actions[{item.peer, item.message.header.market_id}]};
        ++peer_market.count;
        peer_market.bytes += bytes;
    }
}

void BoundedWireQueue::AccountRemove(const QueuedWireMessage& item)
{
    const size_t bytes{item.message.MemoryUsage()};
    --m_size;
    m_bytes -= bytes;
    auto peer_it{m_peer_bytes.find(item.peer)};
    if (peer_it != m_peer_bytes.end()) {
        peer_it->second -= bytes;
        if (peer_it->second == 0) m_peer_bytes.erase(peer_it);
    }
    auto peer_count_it{m_peer_count.find(item.peer)};
    if (peer_count_it != m_peer_count.end()) {
        --peer_count_it->second;
        if (peer_count_it->second == 0) m_peer_count.erase(peer_count_it);
    }
    if (item.message.kind == WireMessageKind::ACTION) {
        auto market_it{m_market_actions.find(item.message.header.market_id)};
        if (market_it != m_market_actions.end()) {
            --market_it->second.count;
            market_it->second.bytes -= bytes;
            if (market_it->second.count == 0) m_market_actions.erase(market_it);
        }
        auto peer_market_it{
            m_peer_market_actions.find({item.peer, item.message.header.market_id})};
        if (peer_market_it != m_peer_market_actions.end()) {
            --peer_market_it->second.count;
            peer_market_it->second.bytes -= bytes;
            if (peer_market_it->second.count == 0) {
                m_peer_market_actions.erase(peer_market_it);
            }
        }
    }
}

bool BoundedWireQueue::EvictOneLowerPriority(
    const WirePriority higher_than, const std::optional<WirePeerId> only_peer)
{
    const size_t first{static_cast<size_t>(higher_than) + 1};
    for (size_t lane{m_lanes.size()}; lane-- > first;) {
        auto& queue{m_lanes[lane]};
        auto it{only_peer
                    ? std::find_if(queue.rbegin(), queue.rend(),
                                   [&](const QueuedWireMessage& item) {
                                       return item.peer == *only_peer;
                                   })
                    : queue.rbegin()};
        if (it == queue.rend()) continue;
        auto erase_it{std::next(it).base()};
        AccountRemove(*erase_it);
        queue.erase(erase_it);
        return true;
    }
    return false;
}

QueueResult BoundedWireQueue::Push(const WirePeerId peer, WireMessage message,
                                   const WireClock::time_point now)
{
    WireCheck check;
    if (!EncodeWireMessage(message, check)) return QueueResult::MALFORMED;
    if (!ConsumeTokens(peer, message, now)) return QueueResult::RATE_LIMITED;

    const size_t bytes{message.MemoryUsage()};
    const WirePriority priority{PriorityForWireKind(message.kind)};
    // A single oversized FlowMesh frame can never consume a peer's complete
    // isolated budget. This check also keeps the subtraction tests below from
    // wrapping size_t.
    if (bytes > FLOWMESH_QUEUE_PER_PEER_BYTES) return QueueResult::PEER_LIMIT;
    if (bytes > FLOWMESH_QUEUE_GLOBAL_BYTES) return QueueResult::GLOBAL_LIMIT;
    if (message.kind == WireMessageKind::ACTION) {
        const auto market_it{m_market_actions.find(message.header.market_id)};
        const Usage market{market_it == m_market_actions.end() ? Usage{}
                                                               : market_it->second};
        if (market.count >= FLOWMESH_ACTION_POOL_PER_MARKET_COUNT ||
            market.bytes > FLOWMESH_ACTION_POOL_PER_MARKET_BYTES - bytes) {
            return QueueResult::MARKET_LIMIT;
        }
        const auto peer_market_it{
            m_peer_market_actions.find({peer, message.header.market_id})};
        const Usage peer_market{peer_market_it == m_peer_market_actions.end()
                                    ? Usage{}
                                    : peer_market_it->second};
        if (peer_market.count >= FLOWMESH_ACTION_POOL_PER_PEER_MARKET_COUNT ||
            peer_market.bytes > FLOWMESH_ACTION_POOL_PER_PEER_MARKET_BYTES - bytes) {
            return QueueResult::PEER_LIMIT;
        }
    }

    while (PeerCount(peer) >= FLOWMESH_QUEUE_PER_PEER_ITEMS ||
           PeerBytes(peer) > FLOWMESH_QUEUE_PER_PEER_BYTES - bytes) {
        if (!EvictOneLowerPriority(priority, peer)) return QueueResult::PEER_LIMIT;
    }
    while (m_size >= FLOWMESH_QUEUE_GLOBAL_ITEMS ||
           m_bytes > FLOWMESH_QUEUE_GLOBAL_BYTES - bytes) {
        if (!EvictOneLowerPriority(priority)) return QueueResult::GLOBAL_LIMIT;
    }
    QueuedWireMessage item{peer, std::move(message)};
    AccountAdd(item);
    m_lanes[static_cast<size_t>(priority)].push_back(std::move(item));
    return QueueResult::ACCEPTED;
}

std::optional<QueuedWireMessage> BoundedWireQueue::Pop()
{
    for (auto& lane : m_lanes) {
        if (lane.empty()) continue;
        QueuedWireMessage out{std::move(lane.front())};
        lane.pop_front();
        AccountRemove(out);
        return out;
    }
    return std::nullopt;
}

void BoundedWireQueue::RemovePeer(const WirePeerId peer)
{
    for (auto& lane : m_lanes) {
        for (auto it{lane.begin()}; it != lane.end();) {
            if (it->peer != peer) {
                ++it;
                continue;
            }
            AccountRemove(*it);
            it = lane.erase(it);
        }
    }
    for (auto it{m_tokens.begin()}; it != m_tokens.end();) {
        if (it->first.first == peer) {
            it = m_tokens.erase(it);
        } else {
            ++it;
        }
    }
    m_control_tokens.erase(peer);
}

size_t BoundedWireQueue::PeerBytes(const WirePeerId peer) const
{
    const auto it{m_peer_bytes.find(peer)};
    return it == m_peer_bytes.end() ? 0 : it->second;
}

size_t BoundedWireQueue::PeerCount(const WirePeerId peer) const
{
    const auto it{m_peer_count.find(peer)};
    return it == m_peer_count.end() ? 0 : it->second;
}

size_t BoundedWireQueue::MarketActionCount(const MarketId& market) const
{
    const auto it{m_market_actions.find(market)};
    return it == m_market_actions.end() ? 0 : it->second.count;
}

bool CatchupRequestTracker::Begin(const WirePeerId peer, const MarketId& market,
                                  const uint64_t from_sequence,
                                  const uint16_t max_entries,
                                  const uint32_t max_bytes)
{
    if (market.IsNull() || max_entries == 0 ||
        max_entries > FLOWMESH_CATCHUP_MAX_ENTRIES || max_bytes == 0 ||
        max_bytes > FLOWMESH_CATCHUP_MAX_BYTES) {
        return false;
    }
    return m_requests.emplace(std::make_pair(peer, market),
                              Request{from_sequence, max_entries, max_bytes})
        .second;
}

bool CatchupRequestTracker::AcceptResponse(const WirePeerId peer,
                                           const MarketId& market,
                                           const uint64_t from_sequence,
                                           const size_t entry_count,
                                           const size_t response_bytes)
{
    const auto key{std::make_pair(peer, market)};
    const auto it{m_requests.find(key)};
    if (it == m_requests.end() || it->second.from_sequence != from_sequence ||
        entry_count == 0 || entry_count > it->second.max_entries ||
        response_bytes == 0 || response_bytes > it->second.max_bytes) {
        return false;
    }
    m_requests.erase(it);
    return true;
}

void CatchupRequestTracker::RemovePeer(const WirePeerId peer)
{
    for (auto it{m_requests.begin()}; it != m_requests.end();) {
        if (it->first.first == peer) {
            it = m_requests.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace flowmesh
