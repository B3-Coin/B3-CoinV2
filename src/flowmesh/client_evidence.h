// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_CLIENT_EVIDENCE_H
#define B3COIN_FLOWMESH_CLIENT_EVIDENCE_H

#include <flowmesh/action_reason.h>
#include <flowmesh/market_data.h>
#include <flowmesh/production_wire.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace flowmesh {

// Public-client resource policy, never a consensus/state codec limit.
inline constexpr size_t CLIENT_STATE_MAX_BYTES{8 * 1024 * 1024};
inline constexpr size_t CLIENT_EVENT_CAPACITY{2048};
inline constexpr size_t CLIENT_EVENT_PAGE_MAX{256};
inline constexpr size_t CLIENT_NOTICE_CAPACITY{2048};
//! Concurrent event watches (ClientEventLog::WaitEvents) per log. Defence in
//! depth only: every caller bounds its own watchers far below this.
inline constexpr size_t CLIENT_EVENT_MAX_WATCHERS{64};

struct ClientEvidencePins {
    uint256 domain;
    MarketId market_id;
    AssetId base_asset;
    VaultId vault_id;
    uint256 execution_config_id;
};

bool CheckClientEvidencePins(const ClientEvidencePins& pins);

struct ClientEventCursor {
    uint256 instance_id;
    uint64_t event_id{0};
};

struct ClientStateEvidence {
    std::vector<unsigned char> certified_payload;
    std::vector<unsigned char> state_bytes;
    // Local delivery position only, not covered by the certificate.
    ClientEventCursor cursor;
};

struct VerifiedClientState {
    ProductionCertifiedEnvelope certified;
    FlowMeshState state;
    size_t active_seats{0};
};

std::optional<std::vector<unsigned char>> EncodeClientState(
    const FlowMeshState& state, std::string& error);

/** Verify the existing quorum certificate and whole-state commitment only.
 * The caller supplies locally established epoch authority and anchor policy.
 * This does NOT establish latest-head freshness, ancestry, B3 settlement or
 * a fresh independently executed result. No executor or signing is invoked. */
std::optional<VerifiedClientState> VerifyClientStateEvidence(
    const ClientEvidencePins& pins, const ClientStateEvidence& evidence,
    const ActiveFnBlsSeatSet& anchored_seats,
    const std::function<bool(const AnchorRef&)>& canonical_anchor,
    std::string& error);

/** Account/book values come exclusively from the authenticated state. Local
 * running/paused status and endpoint-reported history are not authenticated. */
std::optional<MarketData> ClientMarketData(
    const ClientEvidencePins& pins, const VerifiedClientState& verified,
    const std::optional<AccountId>& account, const MarketDataQuery& query,
    std::string& error);

enum class ClientEventKind : uint8_t {
    QUEUE_ADMITTED,
    POOL_ADMITTED,
    POOL_REFUSED,
    CERTIFIED_INCLUDED,
    CERTIFIED_HEAD,
};
const char* ClientEventKindName(ClientEventKind kind);

//! The certified action that consumed an account sequence: which action, in
//! which certified microblock. Memory only.
struct ClientConsumerRef {
    uint256 action_id;
    uint64_t microblock_sequence{0};
    uint256 microblock_hash;

    friend bool operator==(const ClientConsumerRef&, const ClientConsumerRef&) = default;
};

struct ClientEvent {
    uint64_t event_id{0};
    MarketId market_id;
    ClientEventKind kind{ClientEventKind::QUEUE_ADMITTED};
    uint256 action_id;
    AccountId account_id;
    uint64_t account_sequence{0};
    uint8_t action_type{0};
    // SHA256d of the exact signed action payload, absent for credential-free
    // certified bodies. This is transport correlation, NOT a new ActionId.
    uint256 signed_action_hash;
    uint64_t microblock_sequence{0};
    uint256 microblock_hash;
    std::string reason;
    // Memory-only annotations, never serialized in any v1 reply or page.
    ActionReasonCode reason_code{ActionReasonCode::NONE};
    std::optional<ClientConsumerRef> consumed_by;
};

/**
 * A local observation about one API-origin action, kept beside (never inside)
 * the v1 event ring: notices take no event id, no ring capacity and never
 * appear in a v1 page, status or gap. They name what this node decided about
 * one attempt (refused, answered from certified state, dropped from its pool)
 * and, when known, the certified action that consumed the sequence. A notice
 * is an endpoint claim, never evidence and never an execution outcome.
 */
struct ClientActionNotice {
    //! The event log's last event id when the notice was appended. A notice is
    //! newer than an event iff after_event_id >= event.event_id.
    uint64_t after_event_id{0};
    MarketId market_id;
    uint256 action_id;
    AccountId account_id;
    uint64_t account_sequence{0};
    uint8_t action_type{0};
    uint256 signed_action_hash;
    ActionReasonCode code{ActionReasonCode::NONE};
    //! Certified NextSequence(signer) this node's decision used.
    uint64_t next_account_sequence{0};
    std::optional<ClientConsumerRef> consumed_by;
};

//! Notice codes that end an opt-in wait: this node will not admit the
//! attempt, whatever its eventual outcome.
bool ClientNoticeEndsWait(ActionReasonCode code);

struct ClientEventPage {
    ClientEventCursor cursor;
    uint64_t oldest_event_id{0};
    uint64_t latest_event_id{0};
    bool gap{false};
    bool more{false};
    std::vector<ClientEvent> events;
};

enum class ClientWaitResult : uint8_t {
    TERMINAL,    // certified inclusion, or a refusal that is the newest event
    TIMEOUT,     // deadline reached; the status is the current, nonterminal one
    INTERRUPTED, // WakeWaiters() or the caller's interrupt predicate
    RESTARTED,   // Reset(): a new instance; the status is re-read from it
};
const char* ClientWaitResultName(ClientWaitResult result);

struct ClientActionWait {
    std::optional<ClientEvent> status;
    ClientWaitResult result{ClientWaitResult::TIMEOUT};
    //! Only for an include_notices wait: the action's newest notice at return.
    std::optional<ClientActionNotice> notice;
};

//! Why an event watch (ClientEventLog::WaitEvents) returned. A latency
//! observation only, never evidence: the caller reads the page itself.
enum class ClientWatchResult : uint8_t {
    READY,       // Read(after, market, account) would now return an event
    GAP,         // Read(after, ...) would now report a gap
    TIMEOUT,     // deadline reached with nothing new for this filter
    INTERRUPTED, // WakeWaiters() or the caller's interrupt predicate
    RESTARTED,   // Reset(): a new instance (Read then reports a gap)
    BUSY,        // CLIENT_EVENT_MAX_WATCHERS watches already registered
};
const char* ClientWatchResultName(ClientWatchResult result);

/** Memory-only, globally bounded per runtime. No callbacks or network work; a
 * condition variable wakes bounded long-poll observers, whose count and
 * lifetime the caller bounds. The only per-waiter state is the registration
 * of a current event watch (at most CLIENT_EVENT_MAX_WATCHERS).
 * An absent action status means unknown/expired, never a definite rejection.
 * Pool refusal is an observation of one attempt, not an execution outcome. */
class ClientEventLog {
public:
    explicit ClientEventLog(const uint256& instance_id);
    void Reset(const uint256& instance_id);
    void Append(ClientEvent event);
    /** Appends in order under one lock with one wakeup, so a commit's head
     * and inclusion events cost one notification and are never observed
     * partially applied. */
    void Append(std::vector<ClientEvent> events);
    ClientEventCursor Cursor() const;
    ClientEventPage Read(const std::optional<ClientEventCursor>& after,
                         const std::optional<MarketId>& market,
                         const std::optional<AccountId>& account,
                         size_t limit = CLIENT_EVENT_PAGE_MAX) const;
    std::optional<ClientEvent> ActionStatus(const MarketId& market,
                                          const uint256& action_id) const;
    /** Blocks until the exact action's ActionStatus is terminal, the bounded
     * deadline passes, WakeWaiters() or interrupted() ends the wait, or
     * Reset() starts a new instance. Terminal is CERTIFIED_INCLUDED, or
     * POOL_REFUSED only while that refusal is also the action's newest event:
     * a stale refusal followed by an exact retry keeps waiting. Returns the
     * same status ActionStatus would at return; never an execution outcome.
     * Sleeps without the log's mutex; only events for this action trigger a
     * full status scan. interrupted() runs under that mutex and must be a
     * nonblocking, lock-free read. */
    ClientActionWait WaitActionStatus(const MarketId& market, const uint256& action_id,
                                      std::chrono::steady_clock::time_point deadline,
                                      const std::function<bool()>& interrupted,
                                      bool include_notices = false) const;
    /** Ends every current wait with INTERRUPTED (shutdown, never a result). */
    void WakeWaiters() const;
    /** Nonblocking, O(events after the cursor): READY or GAP exactly when
     * Read(after, market, account) would now return an event or report a
     * gap, otherwise TIMEOUT. */
    ClientWatchResult EventsPending(const ClientEventCursor& after, const std::optional<MarketId>& market,
                                    const std::optional<AccountId>& account) const;
    /** Blocks until EventsPending(after, market, account) is no longer
     * TIMEOUT, the bounded deadline passes, WakeWaiters() or interrupted()
     * ends the wait, or Reset() starts a new instance; BUSY at once while
     * CLIENT_EVENT_MAX_WATCHERS watches exist. Returns no events: the caller
     * reads its page with Read(), exactly as an unwaited reader does.
     * The watcher sleeps on its own condition variable, which an append
     * signals only when it matches the watcher's filter or evicts its cursor,
     * and notifies after the log's mutex is released. interrupted() runs
     * under that mutex and must be a nonblocking, lock-free read. */
    ClientWatchResult WaitEvents(const ClientEventCursor& after, const std::optional<MarketId>& market,
                                 const std::optional<AccountId>& account,
                                 std::chrono::steady_clock::time_point deadline,
                                 const std::function<bool()>& interrupted) const;
    //! Test observation: current event watches.
    size_t WatchersForTest() const;
    /** Appends local notices in order under one lock, with at most one
     * wakeup and only while an include_notices wait exists. Bounded ring of
     * CLIENT_NOTICE_CAPACITY; never touches the v1 events. */
    void AppendNotices(std::vector<ClientActionNotice> notices);
    std::optional<ClientActionNotice> NewestNotice(const MarketId& market, const uint256& action_id) const;
    //! The newest notice only while it is newer than every retained event of
    //! the action (or the action has none), under one lock.
    std::optional<ClientActionNotice> NewestNoticeIfNewer(const MarketId& market, const uint256& action_id) const;

private:
    //! The one filter shared by Read() and event watches, so they never disagree.
    static bool Matches(const ClientEvent& event, const std::optional<MarketId>& market,
                        const std::optional<AccountId>& account)
    {
        return (!market || event.market_id == *market) &&
               (!account || event.kind == ClientEventKind::CERTIFIED_HEAD || event.account_id == *account);
    }
    //! One registered WaitEvents call. Shared, so that an appender can
    //! notify it after unlocking even when the watch has returned meanwhile.
    struct EventWatcher {
        EventWatcher(const ClientEventCursor& after_in, const std::optional<MarketId>& market_in,
                     const std::optional<AccountId>& account_in)
            : after{after_in}, market{market_in}, account{account_in} {}
        const ClientEventCursor after;
        const std::optional<MarketId> market;
        const std::optional<AccountId> account;
        std::condition_variable changed;
        //! Guarded by m_mutex.
        bool signalled{false};
    };
    using Signalled = std::vector<std::shared_ptr<EventWatcher>>;
    // Caller holds m_mutex.
    ClientWatchResult PendingLocked(const ClientEventCursor& after, const std::optional<MarketId>& market,
                                    const std::optional<AccountId>& account) const;
    // Caller holds m_mutex. Marks each unmarked watcher that an event with an
    // id above `appended_after` matches, or whose cursor the ring no longer
    // retains; without a bound, every watcher. The caller notifies `out`
    // after unlocking.
    void SignalWatchersLocked(std::optional<uint64_t> appended_after, Signalled& out) const;
    static void Notify(const Signalled& watchers);
    // Caller holds m_mutex. False only at the fail-closed event-id limit.
    bool AppendLocked(ClientEvent&& event);
    // Caller holds m_mutex. newest, when given, receives the kind of the
    // action's newest retained event (unchanged if there is none).
    // newest_id, when given, receives that event's id (unchanged if none).
    std::optional<ClientEvent> ActionStatusLocked(const MarketId& market,
                                                const uint256& action_id,
                                                ClientEventKind* newest,
                                                uint64_t* newest_id = nullptr) const;
    std::optional<ClientActionNotice> NewestNoticeLocked(const MarketId& market, const uint256& action_id) const;

    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    uint256 m_instance_id;
    uint64_t m_last_event_id{0};
    mutable uint64_t m_wake_epoch{0};
    // Current waiters; appends skip the notification when there are none.
    mutable size_t m_waiters{0};
    std::deque<ClientEvent> m_events;
    // Notices: a separate ring, their own change counter and waiter count.
    std::deque<ClientActionNotice> m_notices;
    uint64_t m_notice_changes{0};
    mutable size_t m_notice_waiters{0};
    // Current event watches; guarded by m_mutex, at most CLIENT_EVENT_MAX_WATCHERS.
    mutable std::vector<std::shared_ptr<EventWatcher>> m_watchers;
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_CLIENT_EVIDENCE_H
