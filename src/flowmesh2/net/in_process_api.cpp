// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/net/in_process_api.h>

#include <util/check.h>

#include <utility>

namespace flowmesh2::net {

std::string_view ToString(RequestState state)
{
    switch (state) {
    case RequestState::QUEUED: return "QUEUED";
    case RequestState::PROCESSING: return "PROCESSING";
    case RequestState::REPLIED: return "REPLIED";
    }
    return "UNKNOWN";
}

Result<std::unique_ptr<InProcessApi>> InProcessApi::Create(const ProfileParams& profile, LaneSet& lanes, admission::Admission& admission,
                                                           const admission::ActionPool& pool, const read::ReadService& read,
                                                           const ApiLimits& limits)
{
    const Result<void> valid{Validate(profile)};
    if (!valid.has_value()) return valid.error();
    const uint256 hash{ProfileHash(profile)};
    if (ProfileHash(admission.Profile()) != hash || ProfileHash(pool.Profile()) != hash || ProfileHash(read.Profile()) != hash) {
        return Error{ErrorCode::INVALID_INPUT, API_PROFILE_MISMATCH};
    }
    if (limits.max_replies == 0) return Error{ErrorCode::INVALID_INPUT, API_LIMIT_ZERO};
    if (lanes.Limits().Of(Lane::ACTION).max_message_bytes < ActionV2::MaxEncodedSize(profile) + 1) {
        return Error{ErrorCode::INVALID_INPUT, API_ACTION_LANE_SMALL};
    }
    return Ok(std::unique_ptr<InProcessApi>{new InProcessApi{profile, lanes, admission, pool, read, limits}});
}

InProcessApi::InProcessApi(const ProfileParams& profile, LaneSet& lanes, admission::Admission& admission,
                           const admission::ActionPool& pool, const read::ReadService& read, const ApiLimits& limits)
    : m_profile{profile}, m_lanes{lanes}, m_admission{admission}, m_pool{pool}, m_read{read}, m_limits{limits}
{
}

TrafficClass InProcessApi::ClassOf(std::span<const unsigned char> frame) const
{
    // Only a frame that decodes exactly as a cancel may use the reserved
    // share; anything else, malformed frames included, is ORDINARY and gets
    // its admission answer like any other.
    const Result<ActionV2> action{ActionV2::Decode(frame, m_profile)};
    if (action.has_value() && (action->kind == ActionKind::CANCEL || action->kind == ActionKind::CANCEL_ALL)) {
        return TrafficClass::CANCEL;
    }
    return TrafficClass::ORDINARY;
}

Result<RequestId> InProcessApi::Enqueue(RequestKind kind, TrafficClass traffic, std::span<const unsigned char> body)
{
    if (m_lanes.Closed()) return Error{ErrorCode::FENCED, LANE_CLOSED};
    // Check before adding the request-kind byte: no overflow or unbounded copy.
    if (body.size() >= m_lanes.Limits().Of(Lane::ACTION).max_message_bytes) {
        return Error{ErrorCode::OVERSIZE, LANE_MESSAGE_OVERSIZE};
    }
    // Construct the bounded payload before recording a request. A failed
    // allocation here (or map insertion below) leaves no request behind.
    std::vector<unsigned char> payload;
    payload.reserve(body.size() + 1);
    payload.push_back(static_cast<unsigned char>(kind));
    payload.insert(payload.end(), body.begin(), body.end());
    const RequestId id{m_next_id.fetch_add(1)};
    {
        LOCK(m_mutex);
        if (!m_requests.emplace(id, Request{}).second) return Error{ErrorCode::INVARIANT, API_REQUEST_ID_REUSED};
    }
    try {
        const Result<void> pushed{m_lanes.TryPush(Lane::ACTION, traffic, ToMailboxId(id), payload)};
        if (pushed.has_value()) return Ok(id);
        // Not queued: nothing of this request remains.
        LOCK(m_mutex);
        m_requests.erase(id);
        return pushed.error();
    } catch (...) {
        // LaneSet rolls back its own accounting if message allocation fails.
        // Do not leave a request that can never be popped or answered.
        LOCK(m_mutex);
        m_requests.erase(id);
        throw;
    }
}

Result<RequestId> InProcessApi::Submit(std::span<const unsigned char> frame)
{
    // Preserve the lane's closed-before-oversize refusal precedence.
    if (m_lanes.Closed()) return Error{ErrorCode::FENCED, LANE_CLOSED};
    // Refuse oversized input before ClassOf decodes it or Enqueue copies it.
    if (frame.size() > ActionV2::MaxEncodedSize(m_profile) ||
        frame.size() >= m_lanes.Limits().Of(Lane::ACTION).max_message_bytes) {
        return Error{ErrorCode::OVERSIZE, LANE_MESSAGE_OVERSIZE};
    }
    return Enqueue(RequestKind::SUBMIT_ACTION, ClassOf(frame), frame);
}

Result<RequestId> InProcessApi::SubmitStatus(const ActionV2& original)
{
    if (m_lanes.Closed()) return Error{ErrorCode::FENCED, LANE_CLOSED};
    // Encode is deliberately total, including over-cap payloads; bound it first.
    if (original.payload.size() > m_profile.max_action_payload_bytes) {
        return Error{ErrorCode::OVERSIZE, LANE_MESSAGE_OVERSIZE};
    }
    const std::vector<unsigned char> bytes{original.Encode()};
    return Enqueue(RequestKind::ACTION_STATUS, TrafficClass::STATUS, bytes);
}

Result<RequestId> InProcessApi::SubmitHealth() { return Enqueue(RequestKind::HEALTH, TrafficClass::HEALTH, {}); }

std::optional<ApiReply> InProcessApi::TakeReply(RequestId id)
{
    LOCK(m_mutex);
    const auto it{m_requests.find(id)};
    if (it == m_requests.end() || it->second.state != RequestState::REPLIED) return std::nullopt;
    std::optional<ApiReply> reply{std::move(it->second.reply)};
    m_requests.erase(it);
    Assert(m_reply_reservations >= 1);
    --m_reply_reservations;
    return reply;
}

Result<PumpReport> InProcessApi::Pump(size_t max)
{
    PumpReport report;
    while (true) {
        if (report.answered >= max) {
            report.stop = PumpStop::MAX_REACHED;
            return Ok(report);
        }
        {
            LOCK(m_mutex);
            if (m_reply_reservations >= m_limits.max_replies) {
                report.stop = PumpStop::REPLIES_FULL;
                return Ok(report);
            }
            ++m_reply_reservations;
        }
        std::optional<LaneMessage> message{m_lanes.TryPop(Lane::ACTION)};
        const std::optional<RequestId> id{message ? RequestIdFromMailbox(message->request_id) : std::nullopt};
        bool known{false};
        {
            LOCK(m_mutex);
            if (id) {
                const auto it{m_requests.find(*id)};
                if (it != m_requests.end() && it->second.state == RequestState::QUEUED) {
                    it->second.state = RequestState::PROCESSING;
                    known = true;
                }
            }
            if (!known) --m_reply_reservations;
        }
        if (!message) {
            report.stop = PumpStop::EMPTY;
            return Ok(report);
        }
        if (!known) return Error{ErrorCode::INVARIANT, API_FOREIGN_REQUEST};
        if (message->payload.empty() || !IsKnown(static_cast<RequestKind>(message->payload.front()))) {
            // Only Enqueue fills this lane, so this cannot happen; the request is answered with the error.
            ApiReply failed{*id, RequestKind::SUBMIT_ACTION, Error{ErrorCode::INVARIANT, API_REQUEST_KIND}};
            LOCK(m_mutex);
            Request& request{m_requests.at(*id)};
            request.state = RequestState::REPLIED;
            request.reply = std::move(failed);
            return Error{ErrorCode::INVARIANT, API_REQUEST_KIND};
        }
        const RequestKind kind{static_cast<RequestKind>(message->payload.front())};
        ApiReply reply{Answer(*id, kind, std::span<const unsigned char>{message->payload}.subspan(1))};
        {
            LOCK(m_mutex);
            Request& request{m_requests.at(*id)};
            request.state = RequestState::REPLIED;
            request.reply = std::move(reply);
        }
        ++report.answered;
    }
}

ApiReply InProcessApi::Answer(RequestId id, RequestKind kind, std::span<const unsigned char> body)
{
    ApiReply reply;
    reply.id = id;
    reply.kind = kind;
    switch (kind) {
    case RequestKind::SUBMIT_ACTION: {
        Result<admission::AdmissionReply> admitted{m_admission.Admit(body)};
        if (admitted.has_value()) {
            reply.body = std::move(admitted).value();
        } else {
            reply.body = admitted.error();
        }
        return reply;
    }
    case RequestKind::ACTION_STATUS: {
        Result<ActionV2> original{ActionV2::Decode(body, m_profile)};
        if (!original.has_value()) {
            reply.body = original.error();
            return reply;
        }
        Result<ActionStatusReply> status{GetAction(*original)};
        if (status.has_value()) {
            reply.body = std::move(status).value();
        } else {
            reply.body = status.error();
        }
        return reply;
    }
    case RequestKind::HEALTH:
        reply.body = Health();
        return reply;
    }
    reply.body = Error{ErrorCode::INVARIANT, API_REQUEST_KIND};
    return reply;
}

HealthReply InProcessApi::Health() const
{
    HealthReply health;
    health.fenced = m_admission.Fenced();
    health.published_height = m_read.PublishedHeight();
    health.pool_height = m_pool.HeadHeight();
    health.pool_live = m_pool.Size();
    health.action_lane = m_lanes.Stats(Lane::ACTION);
    return health;
}

Result<ActionStatusReply> InProcessApi::GetAction(const ActionV2& original) const
{
    // 1. One read lease answers INCLUDED or EXPIRED; nothing published yet is NEED_DATA.
    std::optional<read::ActionReply> lease_reply;
    {
        Result<read::ReadHandle> handle{m_read.Acquire()};
        if (handle.has_value()) {
            Result<read::ActionReply> from_lease{handle->Action(original)};
            if (!from_lease.has_value()) return from_lease.error();
            lease_reply = std::move(from_lease).value();
        } else if (handle.error().code != ErrorCode::NEED_DATA) {
            return handle.error(); // BUSY (every permit is held) or INVARIANT
        }
    }
    ActionStatusReply reply;
    if (lease_reply) {
        reply.head = lease_reply->head;
        if (lease_reply->status == read::ActionStatus::INCLUDED && lease_reply->included) {
            reply.status = ApiActionStatus::INCLUDED;
            reply.included = std::move(lease_reply->included);
            return Ok(std::move(reply));
        }
        // EXPIRED only with the witness from this same lease (the read service
        // answers EXPIRED only when its view proves it, read/read_service.h).
        if (lease_reply->status == read::ActionStatus::EXPIRED && lease_reply->expiry) {
            reply.status = ApiActionStatus::EXPIRED;
            reply.expiry = std::move(lease_reply->expiry);
            return Ok(std::move(reply));
        }
    }
    // 2. The pool: PENDING while it holds the action live.
    const std::optional<admission::PoolEntryStatus> pooled{m_pool.Status(original.Id())};
    if (pooled && (pooled->status == admission::PoolStatus::PENDING || pooled->status == admission::PoolStatus::IN_PROPOSAL)) {
        reply.status = ApiActionStatus::PENDING;
        reply.pending = pooled;
        return Ok(std::move(reply));
    }
    // 3. Nothing proven: UNKNOWN, never EXPIRED from the pool's terminal status alone.
    reply.status = ApiActionStatus::UNKNOWN;
    if (pooled && pooled->status == admission::PoolStatus::INCLUDED) {
        reply.reason = API_UNKNOWN_INCLUDED_AFTER_LEASE;
    } else if (pooled && admission::IsTerminal(pooled->status)) {
        reply.reason = API_UNKNOWN_TERMINAL_UNPROVEN;
    } else if (lease_reply) {
        reply.reason = lease_reply->reason.empty() ? read::UNKNOWN_ACTION : lease_reply->reason;
    } else {
        reply.reason = API_UNKNOWN_NOTHING_PUBLISHED;
    }
    return Ok(std::move(reply));
}

Result<read::AccountReply> InProcessApi::GetState(const AccountId& account) const { return m_read.Account(account); }

Result<StreamId> InProcessApi::Subscribe(const StreamRequest& /*request*/)
{
    return NotImplemented("M5-API-02"); // TODO(rebirth:M5-API-02)
}

std::optional<RequestState> InProcessApi::State(RequestId id) const
{
    LOCK(m_mutex);
    const auto it{m_requests.find(id)};
    if (it == m_requests.end()) return std::nullopt;
    return it->second.state;
}

size_t InProcessApi::OpenRequests() const
{
    LOCK(m_mutex);
    return m_requests.size();
}

} // namespace flowmesh2::net
