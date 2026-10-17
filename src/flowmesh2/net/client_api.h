// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NET_CLIENT_API_H
#define B3_FLOWMESH2_NET_CLIENT_API_H

#include <flowmesh2/admission/admission.h>
#include <flowmesh2/admission/pool.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/net/lanes.h>
#include <flowmesh2/read/read_service.h>
#include <uint256.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

/** The client API contract (slice R13; ARCHITECTURE.md sections 3 and 10;
 * plan section 3.8; [CC] boundaries 1, 5 and 7; UX-7, UX-10, DUR-4..DUR-6,
 * PERF-12).
 *
 * Every transport implements this one interface: the in-process API
 * (net/in_process_api.h, real in the outline) and HTTPS + SSE
 * (net/https_sse_api.h, the stub M5-API-01). Four kinds of answer are kept
 * apart and never merged (plan 3.8; ARCHITECTURE.md section 3):
 *
 *  1. QUEUE ACCEPTANCE. Submit, SubmitStatus and SubmitHealth either queue
 *     the request and return its RequestId, or refuse it definitely with
 *     BUSY (a lane is full: NOT queued, try later), OVERSIZE (above the
 *     lane's message cap: not queued) or FENCED (the API is closed). A
 *     RequestId names one queued request; it is not an ActionId and says
 *     nothing about admission.
 *  2. THE REPLY of a queued request: exactly one ApiReply per RequestId,
 *     taken once with TakeReply. For a submitted action it is the
 *     AdmissionReply (ADMITTED means "in this node's pool", never "will
 *     execute"; admission/admission.h) or the Error admission returned.
 *  3. INCLUSION AND EXECUTION: only certified data, through GetAction and
 *     GetState (one read lease per answer, read/read_service.h), which the
 *     client verifies with b3verify against its own trusted seat set.
 *  4. STREAMS: Subscribe (the stub M5-API-02).
 *
 * GetAction(original) answers, from ONE read lease and then the pool:
 *  - INCLUDED: the lease's read service holds the including slot's
 *    envelope, the outcome and its proof (read::IncludedOutcome);
 *  - EXPIRED: the lease's own view proves it (read::ActionReply::expiry,
 *    the witness verify::VerifyExpired accepts with the lease's header and
 *    certificate); never from the pool's terminal status alone;
 *  - PENDING: this node's pool holds it live (PENDING or IN_PROPOSAL);
 *  - UNKNOWN otherwise, with a static reason (for example the pool made it
 *    terminal but the lease proves nothing, or it was included after the
 *    lease's head).
 * An EXPIRED proof is final (no later slot can include the action), so it
 * is answered before PENDING: the pool learns of a slot only after the read
 * service published it (ARCHITECTURE.md section 3), so for one publication
 * the pool may still hold an action the lease already proves expired
 * (O-75).
 *
 * Client rules (plan 3.8, DUR-4): store the exact bytes before sending,
 * never re-sign; a BUSY or a timeout means "unknown", and resending the
 * exact bytes is safe (admission answers DUPLICATE).
 */
namespace flowmesh2::net {

//! One queued request of one API instance; assigned from 1 upwards, never reused.
struct RequestId {
    uint64_t value{0};
    friend bool operator==(const RequestId&, const RequestId&) = default;
    friend auto operator<=>(const RequestId&, const RequestId&) = default;
};
//! The RequestId as a C1 MailboxMessage request id: value little-endian in bytes 0..7, the rest zero.
uint256 ToMailboxId(RequestId id);
//! The inverse of ToMailboxId; nullopt for any other uint256.
std::optional<RequestId> RequestIdFromMailbox(const uint256& mailbox_id);

//! What a queued request asks for. APPEND-ONLY; zero is never a value.
enum class RequestKind : uint8_t {
    SUBMIT_ACTION = 1, //!< admit exact ActionV2 bytes (ORDINARY, or CANCEL for a cancel)
    ACTION_STATUS = 2, //!< GetAction of an original (STATUS)
    HEALTH = 3,        //!< a health probe (HEALTH)
};
bool IsKnown(RequestKind kind);
std::string_view ToString(RequestKind kind);

//! The status GetAction reports. APPEND-ONLY; zero is never a value.
enum class ApiActionStatus : uint8_t {
    INCLUDED = 1, //!< certified: included, with the outcome and its proof
    PENDING = 2,  //!< not certified: in this node's pool
    EXPIRED = 3,  //!< certified: provably never includable, with the proof
    UNKNOWN = 4,  //!< neither; see the reason
};
bool IsKnown(ApiActionStatus status);
std::string_view ToString(ApiActionStatus status);

// Static reasons of an UNKNOWN GetAction answer (besides the read service's read::UNKNOWN_* reasons).
inline constexpr std::string_view API_UNKNOWN_NOTHING_PUBLISHED{"nothing published yet and not in this node's pool"};
inline constexpr std::string_view API_UNKNOWN_INCLUDED_AFTER_LEASE{"included in a slot after the lease's head; ask again"};
inline constexpr std::string_view API_UNKNOWN_TERMINAL_UNPROVEN{"this node's pool dropped it, but the lease proves nothing"};

//! The answer of GetAction (see the file comment).
struct ActionStatusReply {
    ApiActionStatus status{ApiActionStatus::UNKNOWN};
    std::string_view reason;                           //!< static; empty unless UNKNOWN
    std::optional<read::ReplyHead> head;               //!< the one lease's head; absent iff nothing was published yet
    std::optional<read::IncludedOutcome> included;     //!< iff INCLUDED
    std::optional<read::AccountWitness> expiry;        //!< iff EXPIRED: the signer's leaf under head->envelope
    std::optional<admission::PoolEntryStatus> pending; //!< iff PENDING: what the pool holds
};

//! The answer of a HEALTH request. Local diagnostics, never certified.
struct HealthReply {
    bool fenced{false};                        //!< admission answers HALTED
    std::optional<uint64_t> published_height;  //!< the read service's last publication
    uint64_t pool_height{0};                   //!< the last head the pool committed
    size_t pool_live{0};                       //!< live pool entries
    LaneStats action_lane;                     //!< the ACTION lane's accounting
};

//! The one reply of a queued request.
struct ApiReply {
    RequestId id;
    RequestKind kind{RequestKind::SUBMIT_ACTION};
    //! SUBMIT_ACTION: AdmissionReply; ACTION_STATUS: ActionStatusReply; HEALTH: HealthReply; any kind: the Error it ended in.
    std::variant<admission::AdmissionReply, ActionStatusReply, HealthReply, Error> body{admission::AdmissionReply{}};
};

//! Stream channels (plan 3.8 GET /v2/stream). APPEND-ONLY; target shape for M5-API-02.
enum class StreamChannel : uint8_t {
    ACCOUNT = 1,
    BOOK = 2,
    TRADES = 3,
    SLOTS = 4,
};

//! A position in a stream: (stream_epoch, seq). Target shape for M5-API-02.
struct StreamCursor {
    uint64_t stream_epoch{0};
    uint64_t seq{0};
};

//! A subscription request: a snapshot, then deltas from `from` (or now). Target shape for M5-API-02.
struct StreamRequest {
    StreamChannel channel{StreamChannel::SLOTS};
    std::optional<AccountId> account; //!< ACCOUNT
    std::optional<MarketId> market;   //!< BOOK, TRADES
    std::optional<StreamCursor> from; //!< resubscribe after a gap
};

struct StreamId {
    uint64_t value{0};
    friend bool operator==(const StreamId&, const StreamId&) = default;
};

class ClientApi
{
public:
    virtual ~ClientApi() = default;

    //! Queue exact ActionV2 bytes for admission (see the file comment for the answers).
    virtual Result<RequestId> Submit(std::span<const unsigned char> frame) = 0;
    //! Queue a GetAction of `original` (STATUS class); the reply carries an ActionStatusReply.
    virtual Result<RequestId> SubmitStatus(const ActionV2& original) = 0;
    //! Queue a health probe (HEALTH class); the reply carries a HealthReply.
    virtual Result<RequestId> SubmitHealth() = 0;
    //! The reply of `id`, exactly once; nullopt while it is not answered yet, after it was taken, and for an unknown id.
    virtual std::optional<ApiReply> TakeReply(RequestId id) = 0;

    //! Certified status of an action (see the file comment). BUSY when every read permit is held; INVARIANT from the read service.
    virtual Result<ActionStatusReply> GetAction(const ActionV2& original) const = 0;
    //! The certified account leaf (read::ReadService::Account: one lease); BUSY, NEED_DATA or INVARIANT.
    virtual Result<read::AccountReply> GetState(const AccountId& account) const = 0;
    //! A snapshot-then-delta stream (UX-10). Stub M5-API-02 in the outline.
    virtual Result<StreamId> Subscribe(const StreamRequest& request) = 0;
};

} // namespace flowmesh2::net

#endif // B3_FLOWMESH2_NET_CLIENT_API_H
