// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/client_evidence.h>

#include <streams.h>
#include <util/check.h>

#include <algorithm>
#include <limits>

namespace flowmesh {
namespace {

class BoundedStateWriter {
public:
    std::vector<unsigned char> bytes;
    void write(std::span<const std::byte> input)
    {
        if (input.empty()) return;
        if (input.size() > CLIENT_STATE_MAX_BYTES - bytes.size()) {
            throw std::ios_base::failure("FlowMesh client state exceeds 8 MiB");
        }
        const auto* begin{reinterpret_cast<const unsigned char*>(input.data())};
        bytes.insert(bytes.end(), begin, begin + input.size());
    }
    template <typename T> BoundedStateWriter& operator<<(const T& value)
    {
        ::Serialize(*this, value);
        return *this;
    }
};

int StatusPriority(const ClientEventKind kind)
{
    switch (kind) {
    case ClientEventKind::CERTIFIED_INCLUDED: return 3;
    case ClientEventKind::POOL_ADMITTED: return 2;
    case ClientEventKind::POOL_REFUSED: return 1;
    default: return 0;
    }
}

} // namespace

bool CheckClientEvidencePins(const ClientEvidencePins& pins)
{
    const auto market{ComputeFlowMeshMarketId(pins.domain, pins.base_asset)};
    const auto vault{market ? ComputeFlowMeshVaultId(pins.domain, *market) : std::nullopt};
    return market && vault && *market == pins.market_id && *vault == pins.vault_id &&
           pins.execution_config_id == ComputeExecutionConfigId(
               *vault, pins.base_asset, modern::NativeAsset(), FLOWMESH_V1_MAX_CURVE_POINTS);
}

std::optional<std::vector<unsigned char>> EncodeClientState(
    const FlowMeshState& state, std::string& error)
{
    try {
        BoundedStateWriter writer;
        writer << state;
        return std::move(writer.bytes);
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }
}

std::optional<VerifiedClientState> VerifyClientStateEvidence(
    const ClientEvidencePins& pins, const ClientStateEvidence& evidence,
    const ActiveFnBlsSeatSet& seats,
    const std::function<bool(const AnchorRef&)>& canonical_anchor,
    std::string& error)
{
    if (!CheckClientEvidencePins(pins)) {
        error = "FlowMesh client market/domain/configuration pins are invalid";
        return std::nullopt;
    }
    if (evidence.state_bytes.empty() || evidence.state_bytes.size() > CLIENT_STATE_MAX_BYTES ||
        evidence.certified_payload.empty() ||
        evidence.certified_payload.size() > FLOWMESH_V1_MAX_MICROBLOCK_BYTES +
            FLOWMESH_CERTIFIED_PAYLOAD_PREFIX_SIZE + FLOWMESH_BLS_CERTIFICATE_MAX_SIZE) {
        error = "FlowMesh client evidence is missing or exceeds its bound";
        return std::nullopt;
    }
    if (seats.market_id != pins.market_id ||
        CheckActiveFnBlsSeatSet(pins.domain, seats) != BlsSeatSetCheck::OK ||
        seats.anchor_height > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        error = "FlowMesh client anchored seat authority is invalid";
        return std::nullopt;
    }
    const auto certified{DecodeProductionCertifiedPayload(evidence.certified_payload, seats.Size())};
    if (!certified || certified->entry.domain != pins.domain ||
        certified->entry.market_id != pins.market_id ||
        certified->entry.epoch != seats.epoch || certified->entry.seat_set_hash != seats.set_hash) {
        error = "FlowMesh client certified entry does not match pinned context";
        return std::nullopt;
    }
    const AnchorRef seat_anchor{static_cast<int32_t>(seats.anchor_height), seats.anchor_hash};
    if (!canonical_anchor || !canonical_anchor(seat_anchor) ||
        !canonical_anchor(certified->entry.anchor) ||
        certified->entry.anchor.height < seat_anchor.height) {
        error = "FlowMesh client evidence anchor is not acceptable";
        return std::nullopt;
    }
    if (CheckProductionEntryCertificate(certified->entry, seats, certified->certificate) !=
        BlsCertificateCheck::OK) {
        error = "FlowMesh client quorum certificate is invalid";
        return std::nullopt;
    }
    try {
        FlowMeshState state{pins.vault_id, pins.base_asset, modern::NativeAsset(),
                            FLOWMESH_V1_MAX_CURVE_POINTS};
        SpanReader reader{evidence.state_bytes};
        reader >> state;
        if (!reader.empty() || state.Root() != certified->entry.state_root) {
            error = "FlowMesh client state does not match the certified root";
            return std::nullopt;
        }
        return VerifiedClientState{*certified, std::move(state), seats.Size()};
    } catch (const std::exception&) {
        error = "FlowMesh client state is malformed or has inconsistent accounting";
        return std::nullopt;
    }
}

std::optional<MarketData> ClientMarketData(
    const ClientEvidencePins& pins, const VerifiedClientState& verified,
    const std::optional<AccountId>& account, const MarketDataQuery& query,
    std::string& error)
{
    const auto& entry{verified.certified.entry};
    if (!CheckClientEvidencePins(pins) || verified.state.ConfigId() != pins.execution_config_id ||
        entry.domain != pins.domain || entry.market_id != pins.market_id ||
        entry.sequence == std::numeric_limits<uint64_t>::max() ||
        query.limit == 0 || query.limit > MARKET_DATA_MAX_HISTORY ||
        query.curve_limit == 0 || query.curve_limit > MARKET_DATA_MAX_CURVES ||
        (query.curve_cursor && !query.expected_head) ||
        (query.known_head && (query.curve_cursor || query.before_sequence)) ||
        (query.expected_head && *query.expected_head != entry.GetHash())) {
        error = "FlowMesh client snapshot/query context is invalid";
        return std::nullopt;
    }
    MarketData out;
    out.domain = pins.domain;
    out.market_id = pins.market_id;
    out.base_asset_id = pins.base_asset;
    out.execution_config_id = pins.execution_config_id;
    out.snapshot.certified = true;
    out.snapshot.next_microblock_sequence = entry.sequence + 1;
    out.snapshot.last_microblock_hash = entry.GetHash();
    out.snapshot.state_root = entry.state_root;
    out.snapshot.epoch = entry.epoch;
    out.snapshot.anchor_height = entry.anchor.height;
    out.snapshot.anchor_hash = entry.anchor.hash;
    out.snapshot.active_seats = verified.active_seats;
    out.snapshot.quorum_required = FlowMeshBlsThreshold(verified.active_seats);
    out.unchanged = query.known_head && *query.known_head == entry.GetHash();
    if (out.unchanged) return out;
    out.liquidity = verified.state.ReadCurves(query.curve_limit, query.curve_cursor);
    if (account) {
        const auto& ledger{verified.state.LedgerView()};
        out.account = MarketAccountData{*account, verified.state.NextSequence(*account),
            ledger.Available(*account, pins.base_asset), ledger.Reserved(*account, pins.base_asset),
            ledger.Available(*account, modern::NativeAsset()), ledger.Reserved(*account, modern::NativeAsset()),
            verified.state.AccountCurves(*account)};
    }
    return out;
}

const char* ClientEventKindName(const ClientEventKind kind)
{
    switch (kind) {
    case ClientEventKind::QUEUE_ADMITTED: return "queue_admitted";
    case ClientEventKind::POOL_ADMITTED: return "pool_admitted";
    case ClientEventKind::POOL_REFUSED: return "pool_refused";
    case ClientEventKind::CERTIFIED_INCLUDED: return "certified_included";
    case ClientEventKind::CERTIFIED_HEAD: return "certified_head";
    }
    return "unknown";
}

const char* ClientWaitResultName(const ClientWaitResult result)
{
    switch (result) {
    case ClientWaitResult::TERMINAL: return "terminal";
    case ClientWaitResult::TIMEOUT: return "timeout";
    case ClientWaitResult::INTERRUPTED: return "interrupted";
    case ClientWaitResult::RESTARTED: return "restarted";
    }
    return "unknown";
}

const char* ClientWatchResultName(const ClientWatchResult result)
{
    switch (result) {
    case ClientWatchResult::READY: return "ready";
    case ClientWatchResult::GAP: return "gap";
    case ClientWatchResult::TIMEOUT: return "timeout";
    case ClientWatchResult::INTERRUPTED: return "interrupted";
    case ClientWatchResult::RESTARTED: return "restarted";
    case ClientWatchResult::BUSY: return "busy";
    }
    return "unknown";
}

ClientEventLog::ClientEventLog(const uint256& instance_id) : m_instance_id{instance_id} {}

bool ClientNoticeEndsWait(const ActionReasonCode code)
{
    return code == ActionReasonCode::STALE_SEQUENCE || code == ActionReasonCode::DROPPED_SEQUENCE_CONSUMED ||
           code == ActionReasonCode::STALE_SEQUENCE_UNATTRIBUTED ||
           code == ActionReasonCode::DEPOSIT_ALREADY_CREDITED;
}

void ClientEventLog::Reset(const uint256& instance_id)
{
    Signalled signalled;
    {
        std::lock_guard lock{m_mutex};
        m_instance_id = instance_id;
        m_last_event_id = 0;
        m_events.clear();
        m_notices.clear();
        m_public_notices.clear();
        m_public_by_submitter.clear();
        ++m_notice_changes;
        SignalWatchersLocked(std::nullopt, signalled);
    }
    m_changed.notify_all();
    Notify(signalled);
}

void ClientEventLog::SignalWatchersLocked(const std::optional<uint64_t> appended_after, Signalled& out) const
{
    for (const auto& watcher : m_watchers) {
        if (watcher->signalled) continue;
        // Read()'s eviction rule: the cursor can no longer be resumed.
        bool wake{!appended_after ||
                  (!m_events.empty() && watcher->after.event_id < m_events.front().event_id - 1)};
        // Only the retained new events; an evicted new one also evicted the cursor.
        for (auto it{m_events.rbegin()}; !wake && it != m_events.rend() && it->event_id > *appended_after; ++it) {
            wake = Matches(*it, watcher->market, watcher->account);
        }
        if (!wake) continue;
        watcher->signalled = true;
        out.push_back(watcher);
    }
}

void ClientEventLog::Notify(const Signalled& watchers)
{
    // After unlocking: a woken watcher never blocks on the appender's lock.
    // The shared owner keeps a watcher that already returned alive.
    for (const auto& watcher : watchers) watcher->changed.notify_one();
}

void ClientEventLog::AppendNotices(std::vector<ClientActionNotice> notices)
{
    if (notices.empty()) return;
    bool notify{false};
    {
        std::lock_guard lock{m_mutex};
        for (auto& notice : notices) {
            notice.after_event_id = m_last_event_id;
            notice.serial = ++m_notice_serial;
            if (notice.origin == ClientNoticeOrigin::NODE) {
                notice.submitter.clear();
                m_notices.push_back(std::move(notice));
                if (m_notices.size() > CLIENT_NOTICE_CAPACITY) m_notices.pop_front();
                continue;
            }
            // A submitter at its share makes room from its own notices.
            auto& own{m_public_by_submitter[notice.submitter]};
            if (own.size() >= CLIENT_NOTICE_PER_SUBMITTER) {
                m_public_notices.erase(own.front());
                own.pop_front();
            }
            own.push_back(notice.serial);
            m_public_notices.emplace(notice.serial, std::move(notice));
            if (m_public_notices.size() > CLIENT_NOTICE_CAPACITY) {
                // The ring's oldest is its submitter's oldest.
                const auto oldest{m_public_notices.begin()};
                const auto owner{m_public_by_submitter.find(oldest->second.submitter)};
                owner->second.pop_front();
                if (owner->second.empty()) m_public_by_submitter.erase(owner);
                m_public_notices.erase(oldest);
            }
        }
        ++m_notice_changes;
        notify = m_notice_waiters != 0;
    }
    if (notify) m_changed.notify_all();
}

std::optional<ClientActionNotice> ClientEventLog::NewestNotice(const MarketId& market, const uint256& action_id) const
{
    std::lock_guard lock{m_mutex};
    return NewestNoticeLocked(market, action_id);
}

std::optional<ClientActionNotice> ClientEventLog::NewestNoticeIfNewer(const MarketId& market,
                                                                     const uint256& action_id) const
{
    std::lock_guard lock{m_mutex};
    auto notice{NewestNoticeLocked(market, action_id)};
    if (!notice) return notice;
    uint64_t newest_id{0};
    if (ActionStatusLocked(market, action_id, nullptr, &newest_id) && notice->after_event_id < newest_id) {
        return std::nullopt;
    }
    return notice;
}

std::optional<ClientActionNotice> ClientEventLog::NewestNoticeLocked(const MarketId& market,
                                                                     const uint256& action_id) const
{
    const ClientActionNotice* newest{nullptr};
    // Each ring newest first; the first match is that ring's newest.
    const auto newer = [&](const ClientActionNotice& notice) {
        if (notice.market_id != market || notice.action_id != action_id) return false;
        if (!newest || notice.serial > newest->serial) newest = &notice;
        return true;
    };
    for (auto it{m_notices.rbegin()}; it != m_notices.rend() && !newer(*it); ++it) {}
    for (auto it{m_public_notices.rbegin()}; it != m_public_notices.rend() && !newer(it->second); ++it) {}
    return newest ? std::optional{*newest} : std::nullopt;
}

bool ClientEventLog::AppendLocked(ClientEvent&& event)
{
    // Practically unreachable; fail closed rather than wrap a resumable cursor.
    if (m_last_event_id == std::numeric_limits<uint64_t>::max()) return false;
    event.event_id = ++m_last_event_id;
    event.reason.resize(std::min<size_t>(event.reason.size(), 160));
    m_events.push_back(std::move(event));
    if (m_events.size() > CLIENT_EVENT_CAPACITY) m_events.pop_front();
    return true;
}

void ClientEventLog::Append(ClientEvent event)
{
    bool notify{false};
    Signalled signalled;
    {
        std::lock_guard lock{m_mutex};
        const uint64_t before{m_last_event_id};
        const bool appended{AppendLocked(std::move(event))};
        notify = appended && m_waiters != 0;
        if (appended && !m_watchers.empty()) SignalWatchersLocked(before, signalled);
    }
    // After unlocking: a woken waiter never blocks on the appender's lock.
    if (notify) m_changed.notify_all();
    Notify(signalled);
}

void ClientEventLog::Append(std::vector<ClientEvent> events)
{
    bool appended{false};
    bool notify{false};
    Signalled signalled;
    {
        std::lock_guard lock{m_mutex};
        const uint64_t before{m_last_event_id};
        for (auto& event : events) appended = AppendLocked(std::move(event)) || appended;
        notify = appended && m_waiters != 0;
        if (appended && !m_watchers.empty()) SignalWatchersLocked(before, signalled);
    }
    if (notify) m_changed.notify_all();
    Notify(signalled);
}

ClientEventCursor ClientEventLog::Cursor() const
{
    std::lock_guard lock{m_mutex};
    return {m_instance_id, m_last_event_id};
}

ClientEventPage ClientEventLog::Read(const std::optional<ClientEventCursor>& after,
                                    const std::optional<MarketId>& market,
                                    const std::optional<AccountId>& account,
                                    const size_t limit) const
{
    std::lock_guard lock{m_mutex};
    ClientEventPage out;
    out.latest_event_id = m_last_event_id;
    out.oldest_event_id = m_events.empty() ? 0 : m_events.front().event_id;
    out.cursor = {m_instance_id, after ? after->event_id : 0};
    if (limit == 0 || limit > CLIENT_EVENT_PAGE_MAX ||
        (after && (after->instance_id != m_instance_id || after->event_id > m_last_event_id ||
                   (!m_events.empty() && after->event_id < m_events.front().event_id - 1)))) {
        out.gap = true;
        out.cursor.event_id = m_last_event_id;
        return out;
    }
    for (const auto& event : m_events) {
        if (event.event_id <= out.cursor.event_id) continue;
        if (out.events.size() == limit) { out.more = true; break; }
        out.cursor.event_id = event.event_id;
        if (!Matches(event, market, account)) continue;
        out.events.push_back(event);
    }
    if (!out.more) out.cursor.event_id = m_last_event_id;
    return out;
}

std::optional<ClientEvent> ClientEventLog::ActionStatus(const MarketId& market,
                                                      const uint256& action_id) const
{
    std::lock_guard lock{m_mutex};
    return ActionStatusLocked(market, action_id, nullptr);
}

std::optional<ClientEvent> ClientEventLog::ActionStatusLocked(const MarketId& market,
                                                            const uint256& action_id,
                                                            ClientEventKind* const newest,
                                                            uint64_t* const newest_id) const
{
    std::optional<ClientEvent> out;
    if (action_id.IsNull()) return out;
    bool first{true};
    for (auto it{m_events.rbegin()}; it != m_events.rend(); ++it) {
        if (it->market_id != market || it->action_id != action_id) continue;
        if (first && newest) *newest = it->kind;
        if (first && newest_id) *newest_id = it->event_id;
        first = false;
        if (!out || StatusPriority(it->kind) > StatusPriority(out->kind)) out = *it;
        if (out->kind == ClientEventKind::CERTIFIED_INCLUDED) break;
    }
    return out;
}

ClientActionWait ClientEventLog::WaitActionStatus(const MarketId& market, const uint256& action_id,
                                                  const std::chrono::steady_clock::time_point deadline,
                                                  const std::function<bool()>& interrupted,
                                                  const bool include_notices) const
{
    std::unique_lock lock{m_mutex};
    ClientEventKind newest{ClientEventKind::QUEUE_ADMITTED};
    uint64_t newest_id{0};
    ClientActionWait out{ActionStatusLocked(market, action_id, &newest, &newest_id)};
    const auto read_notice = [&] {
        if (include_notices) out.notice = NewestNoticeLocked(market, action_id);
    };
    read_notice();
    // Without notices this is exactly the v1 predicate. With them, a refusal
    // notice newer than every event of the action also ends the wait.
    const auto terminal = [&] {
        if (out.status && (out.status->kind == ClientEventKind::CERTIFIED_INCLUDED ||
                           (out.status->kind == ClientEventKind::POOL_REFUSED &&
                            newest == ClientEventKind::POOL_REFUSED))) {
            return true;
        }
        return include_notices && out.notice && ClientNoticeEndsWait(out.notice->code) &&
               (!out.status || out.notice->after_event_id >= newest_id);
    };
    if (terminal()) {
        out.result = ClientWaitResult::TERMINAL;
        return out;
    }
    if (action_id.IsNull()) return out;
    const uint256 instance{m_instance_id};
    const uint64_t wake{m_wake_epoch};
    uint64_t seen{m_last_event_id};
    uint64_t seen_notices{m_notice_changes};
    // Declared after lock, so it is destroyed, and uncounted, under m_mutex.
    struct Waiter {
        size_t& count;
        size_t* notice_count;
        Waiter(size_t& waiters, size_t* notice_waiters) : count{waiters}, notice_count{notice_waiters}
        {
            ++count;
            if (notice_count) ++*notice_count;
        }
        ~Waiter()
        {
            --count;
            if (notice_count) --*notice_count;
        }
    } waiter{m_waiters, include_notices ? &m_notice_waiters : nullptr};
    while (true) {
        if (!m_changed.wait_until(lock, deadline, [&] {
                return m_last_event_id != seen || m_instance_id != instance ||
                       m_wake_epoch != wake || (interrupted && interrupted()) ||
                       (include_notices && m_notice_changes != seen_notices);
            })) {
            return out; // TIMEOUT with the current nonterminal status.
        }
        if (m_instance_id != instance) {
            out.status = ActionStatusLocked(market, action_id, nullptr);
            read_notice();
            out.result = ClientWaitResult::RESTARTED;
            return out;
        }
        if (m_wake_epoch != wake || (interrupted && interrupted())) {
            out.result = ClientWaitResult::INTERRUPTED;
            return out;
        }
        // O(new events): unrelated appends never pay the full status scan.
        // An eviction gap rescans rather than miss an evicted relevant event.
        bool relevant{!m_events.empty() && m_events.front().event_id > seen + 1};
        for (auto it{m_events.rbegin()}; !relevant && it != m_events.rend() && it->event_id > seen; ++it) {
            relevant = it->market_id == market && it->action_id == action_id;
        }
        seen = m_last_event_id;
        if (include_notices && m_notice_changes != seen_notices) {
            seen_notices = m_notice_changes;
            relevant = true;
        }
        if (!relevant) continue;
        out.status = ActionStatusLocked(market, action_id, &newest, &newest_id);
        read_notice();
        if (terminal()) {
            out.result = ClientWaitResult::TERMINAL;
            return out;
        }
    }
}

void ClientEventLog::WakeWaiters() const
{
    Signalled signalled;
    {
        std::lock_guard lock{m_mutex};
        ++m_wake_epoch;
        SignalWatchersLocked(std::nullopt, signalled);
    }
    m_changed.notify_all();
    Notify(signalled);
}

ClientWatchResult ClientEventLog::PendingLocked(const ClientEventCursor& after, const std::optional<MarketId>& market,
                                                const std::optional<AccountId>& account) const
{
    // Read()'s gap rule: another instance, a future cursor or an evicted one.
    if (after.instance_id != m_instance_id || after.event_id > m_last_event_id ||
        (!m_events.empty() && after.event_id < m_events.front().event_id - 1)) {
        return ClientWatchResult::GAP;
    }
    for (auto it{m_events.rbegin()}; it != m_events.rend() && it->event_id > after.event_id; ++it) {
        if (Matches(*it, market, account)) return ClientWatchResult::READY;
    }
    return ClientWatchResult::TIMEOUT;
}

ClientWatchResult ClientEventLog::EventsPending(const ClientEventCursor& after, const std::optional<MarketId>& market,
                                                const std::optional<AccountId>& account) const
{
    std::lock_guard lock{m_mutex};
    return PendingLocked(after, market, account);
}

ClientWatchResult ClientEventLog::WaitEvents(const ClientEventCursor& after, const std::optional<MarketId>& market,
                                             const std::optional<AccountId>& account,
                                             const std::chrono::steady_clock::time_point deadline,
                                             const std::function<bool()>& interrupted) const
{
    std::unique_lock lock{m_mutex};
    // The pending check and the registration share the lock every append
    // signals under, so no append can fall between them.
    if (const auto now{PendingLocked(after, market, account)}; now != ClientWatchResult::TIMEOUT) return now;
    if (m_watchers.size() >= CLIENT_EVENT_MAX_WATCHERS) return ClientWatchResult::BUSY;
    const uint256 instance{m_instance_id};
    const uint64_t wake{m_wake_epoch};
    const auto self{std::make_shared<EventWatcher>(after, market, account)};
    m_watchers.push_back(self);
    // Declared after lock, so it is destroyed, and deregistered, under m_mutex.
    struct Deregister {
        std::vector<std::shared_ptr<EventWatcher>>& watchers;
        const EventWatcher* self;
        ~Deregister()
        {
            const auto it{std::find_if(watchers.begin(), watchers.end(),
                                       [&](const auto& watcher) { return watcher.get() == self; })};
            // Registered above under this same lock; only this call removes it.
            Assert(it != watchers.end());
            watchers.erase(it);
        }
    } deregister{m_watchers, self.get()};
    while (true) {
        if (!self->changed.wait_until(lock, deadline, [&] {
                return self->signalled || m_instance_id != instance || m_wake_epoch != wake ||
                       (interrupted && interrupted());
            })) {
            return ClientWatchResult::TIMEOUT;
        }
        if (m_instance_id != instance) return ClientWatchResult::RESTARTED;
        if (m_wake_epoch != wake || (interrupted && interrupted())) return ClientWatchResult::INTERRUPTED;
        if (const auto now{PendingLocked(after, market, account)}; now != ClientWatchResult::TIMEOUT) return now;
        // Defensive: an append signals only a READY or GAP condition.
        self->signalled = false;
    }
}

size_t ClientEventLog::WatchersForTest() const
{
    std::lock_guard lock{m_mutex};
    return m_watchers.size();
}

} // namespace flowmesh
