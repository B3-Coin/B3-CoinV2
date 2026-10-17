// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NET_LANES_H
#define B3_FLOWMESH2_NET_LANES_H

#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/mailbox.h>
#include <sync.h>
#include <uint256.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/** Service lanes: the bounded inbound queues of one node (slice R13;
 * ARCHITECTURE.md sections 3 and 4; plan sections 3.2 and 3.8; SEC-3,
 * SEC-4, PERF-12).
 *
 * A LaneSet is four C1 BoundedMailboxes, one per Lane. PopNext serves them
 * in strict priority order CRITICAL > PROPOSAL > ACTION > BULK (SEC-3:
 * certificates > proposals > actions > catch-up); inside a lane the order
 * is FIFO (the C1 mailbox). Every message carries a TrafficClass. CANCEL,
 * STATUS and HEALTH are the RESERVED classes (plan 3.2: "cancel, status
 * and health have reserved capacity (10% of each lane)"): ORDINARY traffic
 * may fill at most max_messages - ReservedShare(max_messages) messages and
 * max_payload_bytes - ReservedShare(max_payload_bytes) payload bytes of a
 * lane, so while the ordinary share of a lane is full, the reserved
 * classes still find at least ReservedShare(max_messages) messages and
 * ReservedShare(max_payload_bytes) bytes free (less what reserved traffic
 * itself already holds). Reserved traffic may use the whole lane.
 *
 * TryPush answers definitely (DUR-6):
 *   - Ok: queued; the LaneSet holds one copy of the payload.
 *   - INVALID_INPUT LANE_UNKNOWN / LANE_CLASS_UNKNOWN: unregistered lane or class.
 *   - FENCED LANE_CLOSED: Close was called.
 *   - OVERSIZE LANE_MESSAGE_OVERSIZE: payload above max_message_bytes.
 *   - BUSY LANE_FULL: max_messages or max_payload_bytes would be exceeded.
 *   - BUSY LANE_ORDINARY_FULL: an ORDINARY message would exceed the ordinary share.
 *   - INVARIANT LANE_ACCOUNTING: the mailbox refused what the accounting admitted.
 * Every refusal is "not queued": the lane holds no copy and its counters
 * are unchanged once the call returns. BUSY means "try later", never "lost".
 *
 * Accounting. The LaneSet counts, per lane, the queued messages and payload
 * bytes of every class and of the ORDINARY class under one Mutex; a push
 * reserves its counts, releases the lock, pushes into the mailbox (which
 * takes the mailbox's own mutex) and rolls the counts back if the mailbox
 * refuses; a pop takes the message from the mailbox, releases that lock,
 * then takes the LaneSet mutex to subtract. So no code here holds two
 * mutexes at once (ARCHITECTURE.md section 4), and the counts never
 * understate what is queued: every limit above holds at every instant.
 * Internally each mailbox payload is u8 TrafficClass || payload; the
 * mailboxes are private, so the header is always valid.
 *
 * Threads: any number of pushers and poppers. Close is sticky; queued
 * messages stay poppable after it (C1 semantics).
 *
 * PROVISIONAL: strict priority can starve BULK under sustained higher
 * traffic; the research proposes deficit round-robin below CRITICAL, and
 * protocol-derived budgets (M4-NET-04) replace the provisional limits.
 */
namespace flowmesh2::net {

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view LANE_LIMITS_INVALID{"lane limits leave no ordinary or no reserved share"}; //!< (INVALID_INPUT)
inline constexpr std::string_view LANE_UNKNOWN{"unregistered lane"};                                           //!< (INVALID_INPUT)
inline constexpr std::string_view LANE_CLASS_UNKNOWN{"unregistered traffic class"};                            //!< (INVALID_INPUT)
inline constexpr std::string_view LANE_CLOSED{"the lanes are closed"};                                         //!< (FENCED)
inline constexpr std::string_view LANE_MESSAGE_OVERSIZE{"message above the lane's max_message_bytes"};         //!< (OVERSIZE)
inline constexpr std::string_view LANE_FULL{"the lane is full"};                                               //!< (BUSY)
inline constexpr std::string_view LANE_ORDINARY_FULL{"the lane's ordinary share is full; its reserved share is not"}; //!< (BUSY)
inline constexpr std::string_view LANE_ACCOUNTING{"the mailbox refused a message the lane accounting admitted"};      //!< (INVARIANT)

//! The lanes of a node. APPEND-ONLY; zero is never a value.
enum class Lane : uint8_t {
    CRITICAL = 1, //!< votes, certificates, view changes, evidence
    PROPOSAL = 2, //!< slot proposals
    ACTION = 3,   //!< client actions, their status queries and health probes
    BULK = 4,     //!< catch-up and snapshots
};
inline constexpr size_t LANE_COUNT{4};
//! Every lane, highest priority first: the order PopNext serves them in.
inline constexpr std::array<Lane, LANE_COUNT> LANES_BY_PRIORITY{Lane::CRITICAL, Lane::PROPOSAL, Lane::ACTION, Lane::BULK};
bool IsKnown(Lane lane);
//! Registry name; "UNKNOWN" for an unregistered value.
std::string_view ToString(Lane lane);

//! What a message is for. APPEND-ONLY; zero is never a value.
enum class TrafficClass : uint8_t {
    ORDINARY = 1, //!< everything that is not reserved
    CANCEL = 2,   //!< reserved: a cancel (plan 3.8: "cancels always have the reserved 10% lane")
    STATUS = 3,   //!< reserved: a status query
    HEALTH = 4,   //!< reserved: a health probe
};
bool IsKnown(TrafficClass traffic);
//! True for CANCEL, STATUS and HEALTH.
bool IsReserved(TrafficClass traffic);
std::string_view ToString(TrafficClass traffic);

//! ceil(limit / 10) without overflow: the reserved share of a limit.
size_t ReservedShare(size_t limit);

//! The limits of one lane. Counts are of the caller's payload bytes.
struct LaneLimits {
    size_t max_messages{0};      //!< queued messages of every class
    size_t max_payload_bytes{0}; //!< queued payload bytes of every class
    size_t max_message_bytes{0}; //!< payload bytes of one message
    friend bool operator==(const LaneLimits&, const LaneLimits&) = default;
};

/** True iff `limits` leave both shares usable: at least one message, and at
 * least max_message_bytes (>= 1) bytes, in the ordinary share AND in the
 * reserved share. So max_messages >= 2 and max_payload_bytes >= 10 *
 * max_message_bytes, roughly.
 */
bool LimitsValid(const LaneLimits& limits);

struct LaneSetLimits {
    std::array<LaneLimits, LANE_COUNT> lanes{}; //!< indexed by the lane integer minus 1
    //! The limits of a registered lane (Assert on an unregistered one).
    const LaneLimits& Of(Lane lane) const;
    LaneLimits& Of(Lane lane);
};

/** PROVISIONAL limits for `profile` (replaced by M4-NET-04's protocol-derived
 * budgets): CRITICAL 4,096 messages of up to agreement::MaxMessageBytes;
 * PROPOSAL 16 of up to MaxMessageBytes; ACTION profile.pool_capacity
 * messages (at least 20) of up to ActionV2::MaxEncodedSize + 1 bytes (one
 * request-kind byte, net/in_process_api.h); BULK 64 of up to
 * MaxMessageBytes. Every lane's byte limit is max_messages *
 * max_message_bytes, raised to 10 * max_message_bytes + 10 where needed so
 * LimitsValid holds. These are bounds, not allocations.
 */
LaneSetLimits ProvisionalLaneLimits(const ProfileParams& profile);

//! One popped message.
struct LaneMessage {
    uint256 request_id;
    Lane lane{Lane::BULK};
    TrafficClass traffic{TrafficClass::ORDINARY};
    std::vector<unsigned char> payload;
};

//! What one lane holds (a coherent observation of the accounting, not a reservation).
struct LaneStats {
    size_t messages{0};
    size_t payload_bytes{0};
    size_t ordinary_messages{0};
    size_t ordinary_bytes{0};
    bool closed{false};
    friend bool operator==(const LaneStats&, const LaneStats&) = default;
};

class LaneSet
{
public:
    //! INVALID_INPUT LANE_LIMITS_INVALID unless every lane's limits pass LimitsValid.
    static Result<std::unique_ptr<LaneSet>> Create(const LaneSetLimits& limits);

    LaneSet(const LaneSet&) = delete;
    LaneSet& operator=(const LaneSet&) = delete;

    //! Queue one copy of `payload` on `lane`. See the file comment for the answers.
    Result<void> TryPush(Lane lane, TrafficClass traffic, const uint256& request_id, std::span<const unsigned char> payload)
        EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The oldest message of `lane`; nullopt if it is empty or `lane` is unregistered.
    std::optional<LaneMessage> TryPop(Lane lane) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The oldest message of the highest-priority non-empty lane (LANES_BY_PRIORITY).
    std::optional<LaneMessage> PopNext() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Refuse every later push (FENCED). Sticky; queued messages stay poppable.
    void Close();
    bool Closed() const { return m_closed.load(); }

    //! The accounting of `lane` (all zero for an unregistered lane).
    LaneStats Stats(Lane lane) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! What the C1 mailbox of `lane` itself reports (tests compare it with Stats).
    MailboxStats MailboxStatsOf(Lane lane) const;
    const LaneSetLimits& Limits() const { return m_limits; }

private:
    explicit LaneSet(const LaneSetLimits& limits);

    struct Accounting {
        size_t messages{0};
        size_t bytes{0};
        size_t ordinary_messages{0};
        size_t ordinary_bytes{0};
    };

    //! Subtract one popped message from `lane`'s counts.
    void Release(size_t index, TrafficClass traffic, size_t bytes) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    const LaneSetLimits m_limits;
    std::array<std::unique_ptr<BoundedMailbox>, LANE_COUNT> m_mailboxes;
    std::atomic<bool> m_closed{false};
    mutable Mutex m_mutex;
    std::array<Accounting, LANE_COUNT> m_accounting GUARDED_BY(m_mutex){};
};

} // namespace flowmesh2::net

#endif // B3_FLOWMESH2_NET_LANES_H
