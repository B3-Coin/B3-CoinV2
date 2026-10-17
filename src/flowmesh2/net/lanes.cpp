// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/net/lanes.h>

#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/core/action.h>
#include <util/check.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace flowmesh2::net {

namespace {

//! 0-based index of a registered lane.
size_t IndexOf(Lane lane) { return static_cast<size_t>(lane) - 1; }

//! a * b, saturated at SIZE_MAX.
size_t SaturatingMul(size_t a, size_t b)
{
    if (a != 0 && b > std::numeric_limits<size_t>::max() / a) return std::numeric_limits<size_t>::max();
    return a * b;
}

//! a + b, saturated at SIZE_MAX.
size_t SaturatingAdd(size_t a, size_t b)
{
    if (b > std::numeric_limits<size_t>::max() - a) return std::numeric_limits<size_t>::max();
    return a + b;
}

//! A lane of `messages` messages of up to `message_bytes`, with a byte limit that passes LimitsValid.
LaneLimits LaneOf(size_t messages, size_t message_bytes)
{
    LaneLimits limits{messages, SaturatingMul(messages, message_bytes), message_bytes};
    // ReservedShare(B) >= m and B - ReservedShare(B) >= m hold for B >= 10 * m + 10.
    limits.max_payload_bytes = std::max(limits.max_payload_bytes, SaturatingAdd(SaturatingMul(10, message_bytes), 10));
    return limits;
}

} // namespace

bool IsKnown(Lane lane)
{
    switch (lane) {
    case Lane::CRITICAL:
    case Lane::PROPOSAL:
    case Lane::ACTION:
    case Lane::BULK:
        return true;
    }
    return false;
}

std::string_view ToString(Lane lane)
{
    switch (lane) {
    case Lane::CRITICAL: return "CRITICAL";
    case Lane::PROPOSAL: return "PROPOSAL";
    case Lane::ACTION: return "ACTION";
    case Lane::BULK: return "BULK";
    }
    return "UNKNOWN";
}

bool IsKnown(TrafficClass traffic)
{
    switch (traffic) {
    case TrafficClass::ORDINARY:
    case TrafficClass::CANCEL:
    case TrafficClass::STATUS:
    case TrafficClass::HEALTH:
        return true;
    }
    return false;
}

bool IsReserved(TrafficClass traffic)
{
    return traffic == TrafficClass::CANCEL || traffic == TrafficClass::STATUS || traffic == TrafficClass::HEALTH;
}

std::string_view ToString(TrafficClass traffic)
{
    switch (traffic) {
    case TrafficClass::ORDINARY: return "ORDINARY";
    case TrafficClass::CANCEL: return "CANCEL";
    case TrafficClass::STATUS: return "STATUS";
    case TrafficClass::HEALTH: return "HEALTH";
    }
    return "UNKNOWN";
}

size_t ReservedShare(size_t limit) { return limit / 10 + (limit % 10 != 0 ? 1 : 0); }

bool LimitsValid(const LaneLimits& limits)
{
    if (limits.max_message_bytes == 0) return false;
    const size_t reserved_messages{ReservedShare(limits.max_messages)};
    const size_t reserved_bytes{ReservedShare(limits.max_payload_bytes)};
    // ReservedShare(x) <= x, so the subtractions below cannot wrap.
    return reserved_messages >= 1 && limits.max_messages - reserved_messages >= 1 && reserved_bytes >= limits.max_message_bytes &&
           limits.max_payload_bytes - reserved_bytes >= limits.max_message_bytes;
}

const LaneLimits& LaneSetLimits::Of(Lane lane) const
{
    Assert(IsKnown(lane));
    return lanes[IndexOf(lane)];
}

LaneLimits& LaneSetLimits::Of(Lane lane)
{
    Assert(IsKnown(lane));
    return lanes[IndexOf(lane)];
}

LaneSetLimits ProvisionalLaneLimits(const ProfileParams& profile)
{
    const size_t agreement_bytes{agreement::MaxMessageBytes(profile)};
    LaneSetLimits limits;
    limits.Of(Lane::CRITICAL) = LaneOf(4'096, agreement_bytes);
    limits.Of(Lane::PROPOSAL) = LaneOf(16, agreement_bytes);
    limits.Of(Lane::ACTION) = LaneOf(std::max<size_t>(profile.pool_capacity, 20), ActionV2::MaxEncodedSize(profile) + 1);
    limits.Of(Lane::BULK) = LaneOf(64, agreement_bytes);
    return limits;
}

Result<std::unique_ptr<LaneSet>> LaneSet::Create(const LaneSetLimits& limits)
{
    for (const LaneLimits& lane : limits.lanes) {
        if (!LimitsValid(lane)) return Error{ErrorCode::INVALID_INPUT, LANE_LIMITS_INVALID};
    }
    return Ok(std::unique_ptr<LaneSet>{new LaneSet{limits}});
}

LaneSet::LaneSet(const LaneSetLimits& limits) : m_limits{limits}
{
    for (size_t i{0}; i < LANE_COUNT; ++i) {
        const LaneLimits& lane{limits.lanes[i]};
        // A backstop only: the accounting admits at most max_messages messages
        // and max_payload_bytes caller bytes, plus one class byte per message.
        m_mailboxes[i] = std::make_unique<BoundedMailbox>(
            MailboxLimits{lane.max_messages, SaturatingAdd(lane.max_payload_bytes, lane.max_messages)});
    }
}

Result<void> LaneSet::TryPush(Lane lane, TrafficClass traffic, const uint256& request_id, std::span<const unsigned char> payload)
{
    if (!IsKnown(lane)) return Error{ErrorCode::INVALID_INPUT, LANE_UNKNOWN};
    if (!IsKnown(traffic)) return Error{ErrorCode::INVALID_INPUT, LANE_CLASS_UNKNOWN};
    if (m_closed.load()) return Error{ErrorCode::FENCED, LANE_CLOSED};
    const size_t index{IndexOf(lane)};
    const LaneLimits& limits{m_limits.lanes[index]};
    const size_t bytes{payload.size()};
    if (bytes > limits.max_message_bytes) return Error{ErrorCode::OVERSIZE, LANE_MESSAGE_OVERSIZE};
    const bool ordinary{traffic == TrafficClass::ORDINARY};

    // Reserve the counts. Every subtraction is of a value at most its minuend.
    {
        LOCK(m_mutex);
        Accounting& a{m_accounting[index]};
        if (a.messages >= limits.max_messages || bytes > limits.max_payload_bytes - a.bytes) {
            return Error{ErrorCode::BUSY, LANE_FULL};
        }
        if (ordinary) {
            const size_t share_messages{limits.max_messages - ReservedShare(limits.max_messages)};
            const size_t share_bytes{limits.max_payload_bytes - ReservedShare(limits.max_payload_bytes)};
            if (a.ordinary_messages >= share_messages || bytes > share_bytes - std::min(share_bytes, a.ordinary_bytes)) {
                return Error{ErrorCode::BUSY, LANE_ORDINARY_FULL};
            }
            ++a.ordinary_messages;
            a.ordinary_bytes += bytes;
        }
        ++a.messages;
        a.bytes += bytes;
    }

    MailboxMessage message;
    message.request_id = request_id;
    message.payload.reserve(bytes + 1);
    message.payload.push_back(static_cast<unsigned char>(traffic));
    message.payload.insert(message.payload.end(), payload.begin(), payload.end());
    const MailboxPushResult pushed{m_mailboxes[index]->TryPush(std::move(message))};
    if (pushed == MailboxPushResult::ACCEPTED) return Ok();

    Release(index, traffic, bytes);
    if (pushed == MailboxPushResult::CLOSED) return Error{ErrorCode::FENCED, LANE_CLOSED};
    return Error{ErrorCode::INVARIANT, LANE_ACCOUNTING};
}

void LaneSet::Release(size_t index, TrafficClass traffic, size_t bytes)
{
    LOCK(m_mutex);
    Accounting& a{m_accounting[index]};
    Assert(a.messages >= 1 && a.bytes >= bytes);
    --a.messages;
    a.bytes -= bytes;
    if (traffic == TrafficClass::ORDINARY) {
        Assert(a.ordinary_messages >= 1 && a.ordinary_bytes >= bytes);
        --a.ordinary_messages;
        a.ordinary_bytes -= bytes;
    }
}

std::optional<LaneMessage> LaneSet::TryPop(Lane lane)
{
    if (!IsKnown(lane)) return std::nullopt;
    const size_t index{IndexOf(lane)};
    std::optional<MailboxMessage> popped{m_mailboxes[index]->TryPop()};
    if (!popped) return std::nullopt;
    // Only TryPush fills the private mailboxes, so the class byte is always there and registered.
    Assert(!popped->payload.empty());
    const TrafficClass traffic{static_cast<TrafficClass>(popped->payload.front())};
    Assert(IsKnown(traffic));
    LaneMessage message;
    message.request_id = popped->request_id;
    message.lane = lane;
    message.traffic = traffic;
    message.payload.assign(popped->payload.begin() + 1, popped->payload.end());
    Release(index, traffic, message.payload.size());
    return message;
}

std::optional<LaneMessage> LaneSet::PopNext()
{
    for (const Lane lane : LANES_BY_PRIORITY) {
        if (std::optional<LaneMessage> message{TryPop(lane)}) return message;
    }
    return std::nullopt;
}

void LaneSet::Close()
{
    m_closed.store(true);
    for (const std::unique_ptr<BoundedMailbox>& mailbox : m_mailboxes) mailbox->Close();
}

LaneStats LaneSet::Stats(Lane lane) const
{
    if (!IsKnown(lane)) return LaneStats{};
    LaneStats stats;
    {
        LOCK(m_mutex);
        const Accounting& a{m_accounting[IndexOf(lane)]};
        stats.messages = a.messages;
        stats.payload_bytes = a.bytes;
        stats.ordinary_messages = a.ordinary_messages;
        stats.ordinary_bytes = a.ordinary_bytes;
    }
    stats.closed = m_closed.load();
    return stats;
}

MailboxStats LaneSet::MailboxStatsOf(Lane lane) const
{
    if (!IsKnown(lane)) return MailboxStats{0, 0, m_closed.load()};
    return m_mailboxes[IndexOf(lane)]->Stats();
}

} // namespace flowmesh2::net
