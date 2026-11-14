// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/net/stream_hub.h>

#include <flowmesh2/engine/book.h>
#include <flowmesh2/engine/engine.h>

#include <algorithm>
#include <utility>

namespace flowmesh2::net {

namespace {

std::vector<BookLevel> LadderLevels(const engine::OrderBook::Ladder& ladder, bool best_is_highest)
{
    std::vector<BookLevel> levels;
    for (const auto& [price, level] : ladder) {
        uint64_t lots{0};
        for (const engine::RestingOrder& order : level) lots += order.remaining_lots;
        levels.push_back(BookLevel{price, lots, static_cast<uint32_t>(level.size())});
    }
    if (best_is_highest) std::reverse(levels.begin(), levels.end());
    return levels;
}

} // namespace

std::string_view ToString(StreamEventKind kind)
{
    switch (kind) {
    case StreamEventKind::SNAPSHOT: return "snapshot";
    case StreamEventKind::DELTA: return "delta";
    }
    return "unknown";
}

Result<std::unique_ptr<StreamHub>> StreamHub::Create(const StreamHubLimits& limits, uint64_t stream_epoch)
{
    if (limits.max_subscriptions == 0 || limits.max_queued_events == 0 || limits.ring_slots == 0) {
        return Error{ErrorCode::INVALID_INPUT, STREAM_LIMIT_ZERO};
    }
    return Ok(std::unique_ptr<StreamHub>{new StreamHub{limits, stream_epoch}});
}

size_t StreamHub::Subscriptions() const
{
    LOCK(m_mutex);
    return m_subscriptions.size();
}

uint64_t StreamHub::PublishedHeight() const
{
    LOCK(m_mutex);
    return m_ring.empty() ? 0 : m_ring.back().height;
}

Result<StreamId> StreamHub::Subscribe(const StreamRequest& request)
{
    switch (request.channel) {
    case StreamChannel::ACCOUNT:
        if (!request.account.has_value()) return Error{ErrorCode::INVALID_INPUT, STREAM_FILTER};
        break;
    case StreamChannel::BOOK:
    case StreamChannel::TRADES:
        if (!request.market.has_value()) return Error{ErrorCode::INVALID_INPUT, STREAM_FILTER};
        break;
    case StreamChannel::SLOTS:
        break;
    default:
        return Error{ErrorCode::INVALID_INPUT, STREAM_CHANNEL_UNKNOWN};
    }
    LOCK(m_mutex);
    if (m_subscriptions.size() >= m_limits.max_subscriptions) return Error{ErrorCode::BUSY, STREAM_LIMIT};
    if (request.market.has_value() && !m_ring.empty() && m_ring.back().head->State().engine.books.Find(*request.market) == nullptr) {
        return Error{ErrorCode::INVALID_INPUT, STREAM_UNKNOWN_MARKET};
    }
    const StreamId id{m_next_id++};
    Subscription& sub{m_subscriptions[id.value]};
    sub.request = request;
    if (m_ring.empty()) return Ok(id); // the SNAPSHOT comes with the first publication
    // Resume after a gap: replay the slots after from.head_slot when the ring holds them, else a fresh SNAPSHOT.
    const StreamCursor* from{request.from.has_value() ? &*request.from : nullptr};
    const bool can_replay{from != nullptr && from->stream_epoch == m_epoch && from->head_slot + 1 >= m_ring.front().height &&
                          from->head_slot <= m_ring.back().height};
    if (can_replay) {
        // The subscriber saw from.head_slot: seed its view of that slot without emitting, then the later slots as DELTAs.
        for (const Published& published : m_ring) {
            if (published.height == from->head_slot) (void)BodyFor(sub, published, /*snapshot=*/true);
        }
        sub.started = true;
        for (const Published& published : m_ring) {
            if (published.height > from->head_slot) Deliver(sub, published, /*snapshot=*/false);
        }
        return Ok(id);
    }
    Deliver(sub, m_ring.back(), /*snapshot=*/true);
    return Ok(id);
}

Result<void> StreamHub::Unsubscribe(StreamId id)
{
    LOCK(m_mutex);
    if (m_subscriptions.erase(id.value) == 0) return Error{ErrorCode::INVALID_INPUT, STREAM_UNKNOWN};
    return Ok();
}

Result<std::vector<StreamEvent>> StreamHub::TakeEvents(StreamId id, size_t max)
{
    LOCK(m_mutex);
    const auto it{m_subscriptions.find(id.value)};
    if (it == m_subscriptions.end()) return Error{ErrorCode::INVALID_INPUT, STREAM_UNKNOWN};
    if (it->second.gap) {
        m_subscriptions.erase(it);
        return Error{ErrorCode::NEED_DATA, STREAM_GAP};
    }
    std::vector<StreamEvent> out;
    std::deque<StreamEvent>& queue{it->second.queue};
    while (!queue.empty() && out.size() < max) {
        out.push_back(std::move(queue.front()));
        queue.pop_front();
    }
    return Ok(std::move(out));
}

void StreamHub::Publish(std::shared_ptr<const state::HeadView> head, std::shared_ptr<const read::SlotView> view)
{
    if (!head || !view) return;
    LOCK(m_mutex);
    Published published{head->Header().height, std::move(head), std::move(view)};
    for (auto& [id, sub] : m_subscriptions) {
        if (sub.gap) continue;
        Deliver(sub, published, /*snapshot=*/!sub.started);
    }
    m_ring.push_back(std::move(published));
    while (m_ring.size() > m_limits.ring_slots) m_ring.pop_front();
}

void StreamHub::Deliver(Subscription& sub, const Published& published, bool snapshot)
{
    std::optional<StreamBody> body{BodyFor(sub, published, snapshot)};
    if (!body.has_value()) return;
    Emit(sub, published, snapshot ? StreamEventKind::SNAPSHOT : StreamEventKind::DELTA, std::move(*body));
}

void StreamHub::Emit(Subscription& sub, const Published& published, StreamEventKind kind, StreamBody body)
{
    sub.started = true;
    if (sub.queue.size() >= m_limits.max_queued_events) {
        sub.gap = true;
        sub.queue.clear();
        return;
    }
    StreamEvent event;
    event.cursor = StreamCursor{m_epoch, sub.next_seq++, published.height};
    event.kind = kind;
    event.body = std::move(body);
    sub.queue.push_back(std::move(event));
}

std::optional<StreamBody> StreamHub::BodyFor(Subscription& sub, const Published& published, bool snapshot) const
{
    const StreamRequest& request{sub.request};
    switch (request.channel) {
    case StreamChannel::SLOTS: {
        SlotEvent event;
        event.height = published.height;
        event.header_hash = published.view->HeaderHash();
        event.state_root = published.view->Header().state_root;
        event.action_count = published.view->Header().action_count;
        event.outcome_count = published.view->Header().outcome_count;
        event.envelope = published.view->Envelope().Encode();
        return StreamBody{std::move(event)};
    }
    case StreamChannel::ACCOUNT: {
        AccountEvent event;
        event.account = *request.account;
        const std::optional<read::AccountWitness> witness{published.view->Account(*request.account)};
        if (witness.has_value()) event.leaf = witness->leaf;
        if (!snapshot) {
            for (const OutcomeV2& outcome : published.view->Outcomes()) {
                for (const BalanceDelta& delta : outcome.deltas) {
                    if (delta.account == *request.account) event.deltas.push_back(delta);
                }
            }
            if (event.deltas.empty() && event.leaf == sub.last_leaf) return std::nullopt;
        }
        sub.last_leaf = event.leaf;
        return StreamBody{std::move(event)};
    }
    case StreamChannel::BOOK: {
        const engine::OrderBook* book{published.head->State().engine.books.Find(*request.market)};
        if (book == nullptr) return std::nullopt;
        BookEvent event;
        event.market = *request.market;
        event.bids = LadderLevels(book->LadderOf(Side::BUY), /*best_is_highest=*/true);
        event.asks = LadderLevels(book->LadderOf(Side::SELL), /*best_is_highest=*/false);
        if (!snapshot && event.bids == sub.last_bids && event.asks == sub.last_asks) return std::nullopt;
        sub.last_bids = event.bids;
        sub.last_asks = event.asks;
        return StreamBody{std::move(event)};
    }
    case StreamChannel::TRADES: {
        TradesEvent event;
        event.market = *request.market;
        if (!snapshot) {
            for (const OutcomeV2& outcome : published.view->Outcomes()) {
                for (const Fill& fill : outcome.fills) {
                    if (fill.market == *request.market) event.fills.push_back(fill);
                }
            }
            if (event.fills.empty()) return std::nullopt;
        }
        return StreamBody{std::move(event)};
    }
    }
    return std::nullopt;
}

} // namespace flowmesh2::net
