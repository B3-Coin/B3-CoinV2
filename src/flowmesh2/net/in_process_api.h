// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NET_IN_PROCESS_API_H
#define B3_FLOWMESH2_NET_IN_PROCESS_API_H

#include <flowmesh2/admission/admission.h>
#include <flowmesh2/admission/pool.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/net/client_api.h>
#include <flowmesh2/net/lanes.h>
#include <flowmesh2/read/read_service.h>
#include <sync.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/** The in-process client API (slice R13; net/client_api.h is the contract).
 *
 * Request path. Submit, SubmitStatus and SubmitHealth give the request the
 * next RequestId, record it QUEUED, and push u8 RequestKind || body onto the
 * ACTION lane of `lanes` with its TrafficClass:
 *   SUBMIT_ACTION  body = the exact frame; CANCEL if the frame decodes
 *                  exactly as a CANCEL or CANCEL_ALL, else ORDINARY
 *   ACTION_STATUS  body = original.Encode(); STATUS
 *   HEALTH         empty body; HEALTH
 * A refused push (BUSY, OVERSIZE, FENCED; net/lanes.h) erases the record
 * again, so a refused request leaves no queued copy and no record, and has
 * no RequestId. The ACTION lane of `lanes` belongs to this API: nothing
 * else may push to it (Pump refuses a message it did not issue, INVARIANT
 * API_FOREIGN_REQUEST).
 *
 * Pump(max) is the server side: it answers at most `max` requests, oldest
 * first. For each it first reserves room for one reply (at most
 * max_replies replies may be PROCESSING or waiting for TakeReply; at that
 * bound Pump stops with REPLIES_FULL and the requests stay queued), pops
 * one message of the ACTION lane, marks the request PROCESSING, answers it
 * WITHOUT any lock (SUBMIT_ACTION: Admission::Admit, A0..A10;
 * ACTION_STATUS: GetAction; HEALTH: the counters), then stores the reply
 * and marks it REPLIED. TakeReply hands it out once and forgets the
 * request. So every RequestId gets exactly one reply, and the memory this
 * API holds is bounded by the ACTION lane's limits plus max_replies.
 *
 * Request states (RequestState, append-only): QUEUED (in the lane),
 * PROCESSING (popped, being answered), REPLIED (waiting for TakeReply).
 *
 * Locks. One Mutex (ARCHITECTURE.md section 4) guards the request map and
 * the reply reservation count. It is held for map operations only: never
 * across a lane push or pop, admission, a read or a pool call, so it nests
 * no other mutex. Submit*, TakeReply, GetAction, GetState and Pump may be
 * called from any number of threads.
 */
namespace flowmesh2::net {

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view API_PROFILE_MISMATCH{"an API component has another profile"};                    //!< (INVALID_INPUT)
inline constexpr std::string_view API_LIMIT_ZERO{"zero API limit"};                                               //!< (INVALID_INPUT)
inline constexpr std::string_view API_ACTION_LANE_SMALL{"the ACTION lane cannot carry the largest action frame"}; //!< (INVALID_INPUT)
inline constexpr std::string_view API_FOREIGN_REQUEST{"a message on the ACTION lane that this API did not queue"}; //!< (INVARIANT)
inline constexpr std::string_view API_REQUEST_KIND{"a queued request of an unregistered kind"};                    //!< (INVARIANT)
inline constexpr std::string_view API_REQUEST_ID_REUSED{"a request id was issued twice"};                         //!< (INVARIANT)

struct ApiLimits {
    size_t max_replies{0}; //!< replies PROCESSING or waiting for TakeReply
};

//! The state of a request this API knows. APPEND-ONLY; zero is never a value.
enum class RequestState : uint8_t {
    QUEUED = 1,
    PROCESSING = 2,
    REPLIED = 3,
};
std::string_view ToString(RequestState state);

//! Why Pump stopped. APPEND-ONLY; zero is never a value.
enum class PumpStop : uint8_t {
    EMPTY = 1,        //!< the ACTION lane was empty
    REPLIES_FULL = 2, //!< max_replies replies were waiting; take some
    MAX_REACHED = 3,  //!< `max` requests were answered
};

struct PumpReport {
    size_t answered{0};
    PumpStop stop{PumpStop::EMPTY};
};

class InProcessApi final : public ClientApi
{
public:
    /** An API over `lanes` (its ACTION lane), `admission`, `pool` and `read`,
     * which must outlive it. Validate(profile) first (its errors), then
     * INVALID_INPUT: API_PROFILE_MISMATCH unless admission, pool and read
     * use `profile`; API_LIMIT_ZERO for max_replies 0;
     * API_ACTION_LANE_SMALL if the ACTION lane's max_message_bytes is below
     * ActionV2::MaxEncodedSize(profile) + 1.
     */
    static Result<std::unique_ptr<InProcessApi>> Create(const ProfileParams& profile, LaneSet& lanes, admission::Admission& admission,
                                                        const admission::ActionPool& pool, const read::ReadService& read,
                                                        const ApiLimits& limits);

    InProcessApi(const InProcessApi&) = delete;
    InProcessApi& operator=(const InProcessApi&) = delete;

    // ClientApi.
    Result<RequestId> Submit(std::span<const unsigned char> frame) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Result<RequestId> SubmitStatus(const ActionV2& original) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Result<RequestId> SubmitHealth() override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::optional<ApiReply> TakeReply(RequestId id) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Result<ActionStatusReply> GetAction(const ActionV2& original) const override;
    Result<read::AccountReply> GetState(const AccountId& account) const override;
    //! Stub: NotImplemented(M5-API-02).
    Result<StreamId> Subscribe(const StreamRequest& request) override;

    //! Answer at most `max` queued requests (see the file comment). INVARIANT API_FOREIGN_REQUEST or API_REQUEST_KIND.
    Result<PumpReport> Pump(size_t max) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! The state of `id`; nullopt once its reply was taken, and for an id this API never issued.
    std::optional<RequestState> State(RequestId id) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Requests QUEUED, PROCESSING or REPLIED.
    size_t OpenRequests() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The TrafficClass Submit would give `frame`.
    TrafficClass ClassOf(std::span<const unsigned char> frame) const;

    const ProfileParams& Profile() const { return m_profile; }
    const ApiLimits& Limits() const { return m_limits; }

private:
    InProcessApi(const ProfileParams& profile, LaneSet& lanes, admission::Admission& admission, const admission::ActionPool& pool,
                 const read::ReadService& read, const ApiLimits& limits);

    struct Request {
        RequestState state{RequestState::QUEUED};
        std::optional<ApiReply> reply; //!< iff REPLIED
    };

    //! Record, then push one request; erase the record again if the push is refused.
    Result<RequestId> Enqueue(RequestKind kind, TrafficClass traffic, std::span<const unsigned char> body)
        EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The reply of one popped request (no lock held).
    ApiReply Answer(RequestId id, RequestKind kind, std::span<const unsigned char> body);
    HealthReply Health() const;

    const ProfileParams m_profile;
    LaneSet& m_lanes;
    admission::Admission& m_admission;
    const admission::ActionPool& m_pool;
    const read::ReadService& m_read;
    const ApiLimits m_limits;

    std::atomic<uint64_t> m_next_id{1};
    mutable Mutex m_mutex;
    std::map<RequestId, Request> m_requests GUARDED_BY(m_mutex);
    //! Requests PROCESSING or REPLIED (Pump reserves one before it pops).
    size_t m_reply_reservations GUARDED_BY(m_mutex){0};
};

} // namespace flowmesh2::net

#endif // B3_FLOWMESH2_NET_IN_PROCESS_API_H
