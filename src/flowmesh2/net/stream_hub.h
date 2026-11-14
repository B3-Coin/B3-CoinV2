// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NET_STREAM_HUB_H
#define B3_FLOWMESH2_NET_STREAM_HUB_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/net/client_api.h>
#include <flowmesh2/net/stream_types.h>
#include <flowmesh2/read/views.h>
#include <flowmesh2/state/candidate.h>
#include <sync.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

/** The stream hub (M5-API-02, stage 2 step 4; O-103): the node publishes
 * every committed slot (the head and its SlotView, after ReadService::Publish)
 * and subscribers take events through the ClientApi (InProcessApi::Subscribe,
 * TakeEvents, Unsubscribe; the HTTPS layer renders them as SSE).
 *
 * - Subscribe(request): the channel's filter must be present (ACCOUNT needs
 *   an account, BOOK and TRADES a market the head knows: STREAM_FILTER,
 *   STREAM_UNKNOWN_MARKET); at most max_subscriptions (BUSY STREAM_LIMIT).
 *   Without `from`, or with a `from` of another stream_epoch or older than
 *   the ring, the first event is a SNAPSHOT of the latest published slot
 *   (nothing yet before the first publication; the SNAPSHOT comes with it);
 *   with a `from` the hub still holds, the slots after from.head_slot are
 *   replayed as DELTAs, each slot exactly once.
 * - Publish(head, view): one DELTA per subscription whose channel the slot
 *   touched (SLOTS always; ACCOUNT when the slot's deltas name the account or
 *   its leaf changed; BOOK when the market's ladder changed; TRADES when the
 *   slot filled in the market). A queue above max_queued_events is dropped:
 *   the next TakeEvents answers NEED_DATA STREAM_GAP once and forgets the
 *   subscription (the client resubscribes from its last cursor).
 * - TakeEvents(id, max): the oldest queued events, in order.
 *
 * The ring keeps the last ring_slots publications for replay. One Mutex;
 * Publish runs on the sealing thread, the rest on any thread; nothing is
 * held across I/O.
 */
namespace flowmesh2::net {

inline constexpr std::string_view STREAM_UNKNOWN{"no such subscription"};                                        //!< (INVALID_INPUT)
inline constexpr std::string_view STREAM_GAP{"the subscription fell behind and was dropped: resubscribe from the last cursor"}; //!< (NEED_DATA)
inline constexpr std::string_view STREAM_LIMIT{"too many subscriptions"};                                        //!< (BUSY)
inline constexpr std::string_view STREAM_FILTER{"the channel needs its account or market"};                      //!< (INVALID_INPUT)
inline constexpr std::string_view STREAM_UNKNOWN_MARKET{"no such market at the head"};                           //!< (INVALID_INPUT)
inline constexpr std::string_view STREAM_CHANNEL_UNKNOWN{"unregistered stream channel"};                         //!< (INVALID_INPUT)
inline constexpr std::string_view STREAM_LIMIT_ZERO{"zero stream limit"};                                        //!< (INVALID_INPUT)
inline constexpr std::string_view STREAM_NO_HUB{"this API has no stream hub"};                                   //!< (INVALID_INPUT)

struct StreamHubLimits {
    size_t max_subscriptions{256};  //!< PROVISIONAL
    size_t max_queued_events{1'024}; //!< per subscription, PROVISIONAL
    size_t ring_slots{16};          //!< publications kept for replay
};

class StreamHub
{
public:
    //! INVALID_INPUT STREAM_LIMIT_ZERO for a zero limit.
    static Result<std::unique_ptr<StreamHub>> Create(const StreamHubLimits& limits, uint64_t stream_epoch);
    StreamHub(const StreamHub&) = delete;
    StreamHub& operator=(const StreamHub&) = delete;

    void Publish(std::shared_ptr<const state::HeadView> head, std::shared_ptr<const read::SlotView> view) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Result<StreamId> Subscribe(const StreamRequest& request) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Result<std::vector<StreamEvent>> TakeEvents(StreamId id, size_t max) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Result<void> Unsubscribe(StreamId id) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    uint64_t StreamEpoch() const { return m_epoch; }
    size_t Subscriptions() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The latest published height (0 before the first publication).
    uint64_t PublishedHeight() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    const StreamHubLimits& Limits() const { return m_limits; }

private:
    StreamHub(const StreamHubLimits& limits, uint64_t stream_epoch) : m_limits{limits}, m_epoch{stream_epoch} {}

    struct Published {
        uint64_t height{0};
        std::shared_ptr<const state::HeadView> head;
        std::shared_ptr<const read::SlotView> view;
    };
    struct Subscription {
        StreamRequest request;
        uint64_t next_seq{1};
        std::deque<StreamEvent> queue;
        bool gap{false};
        std::vector<BookLevel> last_bids, last_asks; //!< BOOK: what the subscriber last saw
        std::optional<AccountLeafV0> last_leaf;      //!< ACCOUNT: what the subscriber last saw
        bool started{false};                          //!< a SNAPSHOT (or a replay) went out
    };

    //! One event for `sub` from `published`; nullopt when the slot did not touch its channel.
    std::optional<StreamBody> BodyFor(Subscription& sub, const Published& published, bool snapshot) const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    void Emit(Subscription& sub, const Published& published, StreamEventKind kind, StreamBody body) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    void Deliver(Subscription& sub, const Published& published, bool snapshot) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    const StreamHubLimits m_limits;
    const uint64_t m_epoch;
    mutable Mutex m_mutex;
    std::deque<Published> m_ring GUARDED_BY(m_mutex);
    std::map<uint64_t, Subscription> m_subscriptions GUARDED_BY(m_mutex);
    uint64_t m_next_id GUARDED_BY(m_mutex){1};
};

} // namespace flowmesh2::net

#endif // B3_FLOWMESH2_NET_STREAM_HUB_H
