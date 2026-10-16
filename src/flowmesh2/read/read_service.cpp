// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/read/read_service.h>

#include <flowmesh2/core/nonce_rule.h>

#include <utility>

namespace flowmesh2::read {

std::string_view ToString(ReadLabel label)
{
    switch (label) {
    case ReadLabel::CERTIFIED:
        return "CERTIFIED";
    }
    return "UNKNOWN";
}

// ---- ReadHandle -------------------------------------------------------------------

ReadHandle::ReadHandle(const ReadService& service, ReadLease lease, ReplyHead head)
    : m_service{&service}, m_lease{std::move(lease)}, m_head{std::move(head)}
{
}

Result<HeadReply> ReadHandle::Head() const { return Ok(HeadReply{m_head}); }

Result<AccountReply> ReadHandle::Account(const AccountId& account) const
{
    AccountReply reply;
    reply.head = m_head;
    const std::shared_ptr<const SlotView> view{m_service->FindView(m_head.commit.id)};
    if (!view) {
        reply.reason = UNKNOWN_VIEW_EVICTED;
        return Ok(std::move(reply));
    }
    reply.witness = view->Account(account);
    if (!reply.witness) {
        reply.reason = UNKNOWN_NO_ACCOUNT;
        return Ok(std::move(reply));
    }
    reply.status = LookupStatus::FOUND;
    return Ok(std::move(reply));
}

Result<ActionReply> ReadHandle::Action(const ActionV2& original) const
{
    ActionReply reply;
    reply.head = m_head;
    const SlotHeaderV2& header{m_head.envelope.header};
    // INCLUDED: the outcome index, restricted to slots this lease can see.
    if (const auto included{m_service->FindIncluded(original.Id(), header.height)}) {
        reply.status = ActionStatus::INCLUDED;
        reply.included = *included;
        return Ok(std::move(reply));
    }
    const std::shared_ptr<const SlotView> view{m_service->FindView(m_head.commit.id)};
    if (!view) {
        reply.reason = UNKNOWN_VIEW_EVICTED;
        return Ok(std::move(reply));
    }
    // EXPIRED only with the proof b3verify's VerifyExpired accepts, from this lease's view.
    const ProfileParams& profile{m_service->Profile()};
    if (original.domain == header.domain && original.MaxDelayValid(profile) &&
        ExpiredAt(original.ref_slot, original.max_delay, header.height)) {
        std::optional<AccountWitness> witness{view->Account(original.Account())};
        if (witness && NonceAcceptable(witness->leaf.used_nonces, original.nonce, profile.nonce_window)) {
            reply.status = ActionStatus::EXPIRED;
            reply.expiry = std::move(witness);
            return Ok(std::move(reply));
        }
    }
    reply.reason = UNKNOWN_ACTION;
    return Ok(std::move(reply));
}

Result<OrderFillsReply> ReadHandle::OrderFills(const OrderId& order) const
{
    OrderFillsReply reply;
    reply.head = m_head;
    const std::shared_ptr<const SlotView> view{m_service->FindView(m_head.commit.id)};
    if (!view) {
        reply.reason = UNKNOWN_VIEW_EVICTED;
        return Ok(std::move(reply));
    }
    // FillsOf lists the fills in outcome, then fill order, so one outcome's fills are adjacent.
    for (const FillRef& ref : view->FillsOf(order)) {
        if (reply.groups.empty() || reply.groups.back().outcome.index != ref.outcome_index) {
            Result<OutcomeWitness> witness{view->Outcome(ref.outcome_index)};
            if (!witness.has_value()) return Error{ErrorCode::INVARIANT, witness.error().detail};
            reply.groups.push_back(OrderFillGroup{std::move(witness).value(), {}});
        }
        reply.groups.back().fills.push_back(ref);
    }
    if (reply.groups.empty()) {
        reply.reason = UNKNOWN_NO_FILLS;
        return Ok(std::move(reply));
    }
    reply.status = LookupStatus::FOUND;
    return Ok(std::move(reply));
}

// ---- ReadService ------------------------------------------------------------------

ReadService::ReadService(const ReadLimits& limits, const ProfileParams& profile)
    : m_limits{limits}, m_profile{profile}, m_profile_hash{ProfileHash(profile)}, m_slot{limits.max_readers, limits.max_envelope_bytes}
{
}

Result<std::unique_ptr<ReadService>> ReadService::Create(const ReadLimits& limits, const ProfileParams& profile)
{
    if (Result<void> valid{Validate(profile)}; !valid.has_value()) return valid.error();
    if (limits.max_readers == 0 || limits.max_envelope_bytes == 0 || limits.view_slots == 0) {
        return Error{ErrorCode::INVALID_INPUT, READ_LIMIT_ZERO};
    }
    if (limits.outcome_index_slots < profile.max_delay_slots) return Error{ErrorCode::INVALID_INPUT, READ_OUTCOME_INDEX_SHORT};
    // The C1 constructor throws only for a zero limit, which is refused above.
    return Ok(std::unique_ptr<ReadService>{new ReadService{limits, profile}});
}

Result<void> ReadService::Publish(const DurableApplied& applied, std::shared_ptr<const SlotView> view)
{
    if (!view) return Error{ErrorCode::INVALID_INPUT, READ_NULL_VIEW};
    const SlotHeaderV2& header{view->Header()};
    const uint256& hash{view->HeaderHash()};
    if (applied.Slot() != header.height || applied.Digest() != hash || applied.Binding().state_root != header.state_root) {
        return Error{ErrorCode::VERIFY_FAILED, READ_TOKEN_MISMATCH};
    }
    if (header.profile_hash != m_profile_hash) return Error{ErrorCode::INVALID_INPUT, READ_PROFILE_MISMATCH};
    ReadEnvelope envelope{ScopeOf(header), CommitOf(header), view->Envelope().Encode()};
    if (envelope.payload.size() > m_limits.max_envelope_bytes) return Error{ErrorCode::OVERSIZE, READ_ENVELOPE_OVERSIZE};

    // The slot's action outcomes with their proofs, built before any lock.
    std::vector<std::pair<ActionId, std::shared_ptr<const IncludedOutcome>>> included;
    for (uint32_t i{0}; i < view->Outcomes().size(); ++i) {
        const OutcomeV2& outcome{view->Outcomes()[i]};
        if (outcome.source != OutcomeSource::ACTION) continue;
        Result<OutcomeWitness> witness{view->Outcome(i)};
        if (!witness.has_value()) return Error{ErrorCode::INVARIANT, witness.error().detail};
        included.emplace_back(ActionId{outcome.subject},
                              std::make_shared<const IncludedOutcome>(
                                  IncludedOutcome{IncludedRef{header.height, i}, view->Envelope(), std::move(witness).value()}));
    }

    {
        LOCK(m_archive_mutex);
        if (m_publishing) return Error{ErrorCode::ORDER_VIOLATION, READ_CONCURRENT_PUBLISH};
        if (m_last && (header.height != m_last->first + 1 || header.parent_hash != m_last->second)) {
            return Error{ErrorCode::ORDER_VIOLATION, READ_NOT_SUCCESSOR};
        }
        m_publishing = true;
        m_views.emplace(hash, view);
        m_view_order.push_back(hash);
        std::vector<ActionId> ids;
        ids.reserve(included.size());
        for (const auto& [id, record] : included) {
            // An action executes at most once on one chain (its nonce is consumed).
            if (m_included.emplace(id, record).second) ids.push_back(id);
        }
        m_included_order.emplace_back(header.height, std::move(ids));
    }

    // C1 publication: memory visibility only, after the view is findable.
    const ReadPublishResult published{m_slot.Publish(envelope)};

    // Objects released after the lock: evicted or rolled-back views and records.
    std::vector<std::shared_ptr<const SlotView>> retired_views;
    std::vector<std::shared_ptr<const IncludedOutcome>> retired_records;
    {
        LOCK(m_archive_mutex);
        m_publishing = false;
        if (published != ReadPublishResult::PUBLISHED) {
            // Roll back this slot's insertions (OVERSIZE cannot happen: checked above).
            const auto view_it{m_views.find(hash)};
            if (view_it != m_views.end()) {
                retired_views.push_back(std::move(view_it->second));
                m_views.erase(view_it);
            }
            m_view_order.pop_back();
            for (const ActionId& id : m_included_order.back().second) {
                const auto it{m_included.find(id)};
                if (it == m_included.end()) continue;
                retired_records.push_back(std::move(it->second));
                m_included.erase(it);
            }
            m_included_order.pop_back();
            return Error{ErrorCode::INVARIANT, READ_SLOT_EXHAUSTED};
        }
        m_last = std::make_pair(header.height, hash);
        while (m_view_order.size() > m_limits.view_slots) {
            const auto it{m_views.find(m_view_order.front())};
            if (it != m_views.end()) {
                retired_views.push_back(std::move(it->second));
                m_views.erase(it);
            }
            m_view_order.pop_front();
        }
        // Keep the actions of slots s with height - s < outcome_index_slots.
        while (!m_included_order.empty() && header.height - m_included_order.front().first >= m_limits.outcome_index_slots) {
            for (const ActionId& id : m_included_order.front().second) {
                const auto it{m_included.find(id)};
                if (it == m_included.end()) continue;
                retired_records.push_back(std::move(it->second));
                m_included.erase(it);
            }
            m_included_order.pop_front();
        }
    }
    return Ok();
}

Result<ReadHandle> ReadService::Acquire() const
{
    std::optional<ReadLease> lease{m_slot.Acquire()};
    if (!lease) {
        LOCK(m_archive_mutex);
        if (!m_last) return Error{ErrorCode::NEED_DATA, READ_NOTHING_PUBLISHED};
        return Error{ErrorCode::BUSY, READ_NO_PERMIT};
    }
    const PublishedEnvelope& published{lease->View()};
    Result<CertifiedEnvelope> envelope{CertifiedEnvelope::Decode(published.envelope.payload)};
    if (!envelope.has_value() || envelope->header.Hash() != published.envelope.commit.id ||
        !(ScopeOf(envelope->header) == published.envelope.scope) || !(CommitOf(envelope->header) == published.envelope.commit)) {
        return Error{ErrorCode::INVARIANT, READ_ENVELOPE_CORRUPT};
    }
    ReplyHead head;
    head.scope = published.envelope.scope;
    head.commit = published.envelope.commit;
    head.publication = published.publication;
    head.envelope = std::move(envelope).value();
    head.label = ReadLabel::CERTIFIED;
    return Ok(ReadHandle{*this, std::move(*lease), std::move(head)});
}

Result<HeadReply> ReadService::Head() const
{
    Result<ReadHandle> handle{Acquire()};
    if (!handle.has_value()) return handle.error();
    return handle->Head();
}

Result<AccountReply> ReadService::Account(const AccountId& account) const
{
    Result<ReadHandle> handle{Acquire()};
    if (!handle.has_value()) return handle.error();
    return handle->Account(account);
}

Result<ActionReply> ReadService::Action(const ActionV2& original) const
{
    Result<ReadHandle> handle{Acquire()};
    if (!handle.has_value()) return handle.error();
    return handle->Action(original);
}

Result<OrderFillsReply> ReadService::OrderFills(const OrderId& order) const
{
    Result<ReadHandle> handle{Acquire()};
    if (!handle.has_value()) return handle.error();
    return handle->OrderFills(order);
}

size_t ReadService::ViewCount() const
{
    LOCK(m_archive_mutex);
    return m_views.size();
}

size_t ReadService::IndexedActions() const
{
    LOCK(m_archive_mutex);
    return m_included.size();
}

std::optional<uint64_t> ReadService::PublishedHeight() const
{
    LOCK(m_archive_mutex);
    if (!m_last) return std::nullopt;
    return m_last->first;
}

std::shared_ptr<const SlotView> ReadService::FindView(const uint256& id) const
{
    LOCK(m_archive_mutex);
    const auto it{m_views.find(id)};
    if (it == m_views.end()) return nullptr;
    return it->second;
}

std::shared_ptr<const IncludedOutcome> ReadService::FindIncluded(const ActionId& id, uint64_t max_height) const
{
    LOCK(m_archive_mutex);
    const auto it{m_included.find(id)};
    if (it == m_included.end() || it->second->ref.height > max_height) return nullptr;
    return it->second;
}

} // namespace flowmesh2::read
