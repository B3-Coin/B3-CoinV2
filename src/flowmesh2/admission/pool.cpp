// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/admission/pool.h>

#include <flowmesh2/core/merkle.h>
#include <flowmesh2/core/nonce_rule.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/ledger/nonce_window.h>

#include <algorithm>
#include <set>

namespace flowmesh2::admission {

PoolLimits PoolLimits::FromProfile(const ProfileParams& profile)
{
    return PoolLimits{profile.pool_capacity, profile.outcome_index_slots, profile.pool_capacity};
}

bool IsKnown(PoolStatus status)
{
    switch (status) {
    case PoolStatus::PENDING:
    case PoolStatus::IN_PROPOSAL:
    case PoolStatus::INCLUDED:
    case PoolStatus::NONCE_USED:
    case PoolStatus::NONCE_TOO_LOW:
    case PoolStatus::EXPIRED:
        return true;
    }
    return false;
}

bool IsTerminal(PoolStatus status)
{
    switch (status) {
    case PoolStatus::INCLUDED:
    case PoolStatus::NONCE_USED:
    case PoolStatus::NONCE_TOO_LOW:
    case PoolStatus::EXPIRED:
        return true;
    case PoolStatus::PENDING:
    case PoolStatus::IN_PROPOSAL:
        return false;
    }
    return false;
}

std::string_view ToString(PoolStatus status)
{
    switch (status) {
    case PoolStatus::PENDING: return "PENDING";
    case PoolStatus::IN_PROPOSAL: return "IN_PROPOSAL";
    case PoolStatus::INCLUDED: return "INCLUDED";
    case PoolStatus::NONCE_USED: return "NONCE_USED";
    case PoolStatus::NONCE_TOO_LOW: return "NONCE_TOO_LOW";
    case PoolStatus::EXPIRED: return "EXPIRED";
    }
    return "UNKNOWN";
}

std::string_view ToString(InsertResult result)
{
    switch (result) {
    case InsertResult::INSERTED: return "INSERTED";
    case InsertResult::DUPLICATE: return "DUPLICATE";
    case InsertResult::NONCE_PENDING: return "NONCE_PENDING";
    case InsertResult::POOL_FULL: return "POOL_FULL";
    case InsertResult::STALE_SNAPSHOT: return "STALE_SNAPSHOT";
    }
    return "UNKNOWN";
}

Result<std::unique_ptr<ActionPool>> ActionPool::Create(const PoolLimits& limits, const ProfileParams& profile, uint64_t head_height)
{
    const Result<void> valid{Validate(profile)};
    if (!valid.has_value()) return valid.error();
    if (limits.capacity == 0 || limits.retention_slots == 0 || limits.terminal_capacity == 0) {
        return Error{ErrorCode::INVALID_INPUT, POOL_LIMIT_ZERO};
    }
    if (limits.retention_slots < profile.max_delay_slots) return Error{ErrorCode::INVALID_INPUT, POOL_RETENTION_SHORT};
    return Ok(std::unique_ptr<ActionPool>{new ActionPool{limits, profile, head_height}});
}

ActionPool::ActionPool(const PoolLimits& limits, const ProfileParams& profile, uint64_t head_height)
    : m_limits{limits}, m_profile{profile}, m_profile_hash{ProfileHash(profile)}, m_head_height{head_height}
{
}

void ActionPool::EraseLive(std::map<ActionId, Entry>::iterator it)
{
    const ActionV2& action{it->second.action->Action()};
    m_by_seq.erase(it->second.ref.seq);
    m_by_nonce.erase({it->second.action->Account(), action.nonce});
    m_live.erase(it);
}

InsertReply ActionPool::Insert(const VerifiedAction& action, uint64_t snapshot_height)
{
    LOCK(m_mutex);
    if (snapshot_height < m_head_height) return InsertReply{InsertResult::STALE_SNAPSHOT, std::nullopt, std::nullopt};
    const ActionId& id{action.Id()};
    if (const auto live{m_live.find(id)}; live != m_live.end()) {
        return InsertReply{InsertResult::DUPLICATE, live->second.ref, std::nullopt};
    }
    const auto terminal{m_terminal.find(id)};
    if (terminal != m_terminal.end() && terminal->second.status == PoolStatus::INCLUDED) {
        return InsertReply{InsertResult::DUPLICATE, terminal->second.pending, terminal->second.included};
    }
    const std::pair<AccountId, uint64_t> nonce_key{action.Account(), action.Action().nonce};
    if (m_by_nonce.contains(nonce_key)) return InsertReply{InsertResult::NONCE_PENDING, std::nullopt, std::nullopt};
    if (m_live.size() >= m_limits.capacity) return InsertReply{InsertResult::POOL_FULL, std::nullopt, std::nullopt};

    if (terminal != m_terminal.end()) {
        // A NONCE_USED, NONCE_TOO_LOW or EXPIRED record of a re-admitted action; its
        // order entries become stale and are skipped at eviction.
        m_terminal.erase(terminal);
        --m_other_count;
    }
    const PendingRef ref{id, m_next_seq++, snapshot_height};
    Entry entry{std::make_shared<const VerifiedAction>(action), action.Action().Encode(), ref, PoolStatus::PENDING, 0};
    m_by_seq.emplace(ref.seq, id);
    m_by_nonce.emplace(nonce_key, id);
    m_live.emplace(id, std::move(entry));
    return InsertReply{InsertResult::INSERTED, ref, std::nullopt};
}

std::optional<KnownAction> ActionPool::FindAuthenticated(const ActionId& id, std::span<const unsigned char, ACTION_SIG_BYTES> sig) const
{
    LOCK(m_mutex);
    if (const auto live{m_live.find(id)}; live != m_live.end()) {
        const auto& held{live->second.action->Action().sig};
        if (!std::equal(held.begin(), held.end(), sig.begin())) return std::nullopt;
        return KnownAction{live->second.status, live->second.ref, std::nullopt};
    }
    if (const auto terminal{m_terminal.find(id)}; terminal != m_terminal.end() && terminal->second.status == PoolStatus::INCLUDED) {
        const auto& held{terminal->second.sig};
        if (!std::equal(held.begin(), held.end(), sig.begin())) return std::nullopt;
        return KnownAction{PoolStatus::INCLUDED, terminal->second.pending, terminal->second.included};
    }
    return std::nullopt;
}

std::optional<KnownAction> ActionPool::FindById(const ActionId& id) const
{
    LOCK(m_mutex);
    if (const auto live{m_live.find(id)}; live != m_live.end()) {
        return KnownAction{live->second.status, live->second.ref, std::nullopt};
    }
    if (const auto terminal{m_terminal.find(id)}; terminal != m_terminal.end() && terminal->second.status == PoolStatus::INCLUDED) {
        return KnownAction{PoolStatus::INCLUDED, terminal->second.pending, terminal->second.included};
    }
    return std::nullopt;
}

Result<TakenSlot> ActionPool::TakeForSlot(uint64_t next_height, size_t max, const WithholdFilter& withhold)
{
    // Candidates are copied out under the lock (shared pointers to immutable
    // actions), the filter runs without it, and the taken ones are marked
    // under the lock again; an entry that left the pool in between is skipped.
    std::vector<std::pair<ActionId, std::shared_ptr<const VerifiedAction>>> candidates;
    {
        LOCK(m_mutex);
        if (m_head_height == UINT64_MAX || next_height != m_head_height + 1) {
            return Error{ErrorCode::ORDER_VIOLATION, POOL_TAKE_HEIGHT};
        }
        candidates.reserve(m_by_seq.size());
        for (const auto& [seq, id] : m_by_seq) {
            const Entry& entry{m_live.at(id)};
            const ActionV2& action{entry.action->Action()};
            if (!InExpiryWindow(action.ref_slot, action.max_delay, next_height)) continue;
            candidates.emplace_back(id, entry.action);
        }
    }
    const size_t limit{std::min<size_t>(max, m_profile.max_actions_per_slot)};
    std::vector<ActionId> chosen;
    for (const auto& [id, action] : candidates) {
        if (chosen.size() >= limit) break;
        if (withhold && withhold(action->Action())) continue;
        chosen.push_back(id);
    }
    TakenSlot taken;
    taken.height = next_height;
    LOCK(m_mutex);
    if (next_height != m_head_height + 1) return Error{ErrorCode::ORDER_VIOLATION, POOL_TAKE_HEIGHT};
    for (const ActionId& id : chosen) {
        const auto it{m_live.find(id)};
        if (it == m_live.end()) continue;
        it->second.status = PoolStatus::IN_PROPOSAL;
        it->second.proposal_height = next_height;
        taken.ids.push_back(id);
        taken.originals.push_back(it->second.original);
    }
    return Ok(std::move(taken));
}

Result<CommitReport> ActionPool::OnCommitted(const state::HeadView& head, std::span<const OutcomeV2> outcomes, const SlotBody& body)
{
    const SlotHeaderV2& header{head.Header()};
    // Checks that need no pool state, done before taking the lock.
    if (header.profile_hash != m_profile_hash) return Error{ErrorCode::INVALID_INPUT, POOL_PROFILE_MISMATCH};
    std::vector<uint256> outcome_leaves;
    outcome_leaves.reserve(outcomes.size());
    for (const OutcomeV2& outcome : outcomes) outcome_leaves.push_back(outcome.LeafHash());
    if (outcomes.size() != header.outcome_count || MerkleRoot(outcome_leaves) != header.outcomes_root) {
        return Error{ErrorCode::INVALID_INPUT, POOL_OUTCOMES_MISMATCH};
    }
    if (body.originals.size() != header.action_count || body.ActionsRoot() != header.actions_root) {
        return Error{ErrorCode::INVALID_INPUT, POOL_BODY_MISMATCH};
    }
    // Every original has exactly one ACTION outcome and every ACTION outcome an original.
    std::map<ActionId, uint32_t> outcome_of;
    for (size_t i{0}; i < outcomes.size(); ++i) {
        if (outcomes[i].source != OutcomeSource::ACTION) continue;
        if (!outcome_of.emplace(ActionId{outcomes[i].subject}, static_cast<uint32_t>(i)).second) {
            return Error{ErrorCode::INVALID_INPUT, POOL_OUTCOME_MISSING};
        }
    }
    struct Included {
        ActionId id;
        std::array<unsigned char, ACTION_SIG_BYTES> sig;
        uint32_t index;
    };
    std::vector<Included> included;
    included.reserve(body.originals.size());
    for (const auto& bytes : body.originals) {
        const Result<ActionV2> action{ActionV2::Decode(bytes, m_profile)};
        if (!action.has_value()) return Error{ErrorCode::INVALID_INPUT, POOL_ORIGINAL_MALFORMED};
        const ActionId id{action->Id()};
        const auto found{outcome_of.find(id)};
        if (found == outcome_of.end()) return Error{ErrorCode::INVALID_INPUT, POOL_OUTCOME_MISSING};
        included.push_back(Included{id, action->sig, found->second});
    }
    if (included.size() != outcome_of.size()) return Error{ErrorCode::INVALID_INPUT, POOL_OUTCOME_MISSING};
    const uint64_t height{header.height};
    const ledger::LedgerState& ledger{head.State().engine.ledger};

    LOCK(m_mutex);
    if (m_head_height == UINT64_MAX || height != m_head_height + 1) return Error{ErrorCode::ORDER_VIOLATION, POOL_NOT_SUCCESSOR};

    // Classify every live entry the slot did not include, in admission order
    // (so terminal records are written, and evicted, oldest first), before
    // changing anything.
    std::set<ActionId> included_ids;
    for (const Included& inc : included) included_ids.insert(inc.id);
    std::vector<std::pair<ActionId, PoolStatus>> verdicts; // terminal status, or PENDING to return / keep
    verdicts.reserve(m_live.size());
    for (const auto& [seq, id] : m_by_seq) {
        if (included_ids.contains(id)) continue;
        const Entry& entry{m_live.at(id)};
        const ActionV2& action{entry.action->Action()};
        const ledger::AccountState* account{ledger.Find(entry.action->Account())};
        if (account == nullptr) return Error{ErrorCode::INVARIANT, POOL_ACCOUNT_MISSING};
        switch (account->nonces.Classify(action.nonce)) {
        case NonceVerdict::USED: verdicts.emplace_back(id, PoolStatus::NONCE_USED); continue;
        case NonceVerdict::TOO_LOW: verdicts.emplace_back(id, PoolStatus::NONCE_TOO_LOW); continue;
        case NonceVerdict::ACCEPTABLE: break;
        }
        if (ExpiredAt(action.ref_slot, action.max_delay, height)) {
            verdicts.emplace_back(id, PoolStatus::EXPIRED);
        } else {
            verdicts.emplace_back(id, PoolStatus::PENDING);
        }
    }

    // From here on nothing can fail.
    CommitReport report;
    report.height = height;
    for (const Included& inc : included) {
        Terminal record;
        record.status = PoolStatus::INCLUDED;
        record.included = read::IncludedRef{height, inc.index};
        record.sig = inc.sig;
        record.height = height;
        if (const auto live{m_live.find(inc.id)}; live != m_live.end()) {
            record.pending = live->second.ref;
            EraseLive(live);
            ++report.included;
        }
        if (const auto old{m_terminal.find(inc.id)}; old != m_terminal.end()) {
            if (old->second.status != PoolStatus::INCLUDED) --m_other_count;
            m_terminal.erase(old);
        }
        m_terminal.emplace(inc.id, std::move(record));
        m_included_order.emplace_back(height, inc.id);
        ++report.recorded;
    }
    for (const auto& [id, verdict] : verdicts) {
        const auto live{m_live.find(id)};
        if (verdict == PoolStatus::PENDING) {
            if (live->second.status == PoolStatus::IN_PROPOSAL) {
                live->second.status = PoolStatus::PENDING;
                live->second.proposal_height = 0;
                ++report.returned;
            }
            continue;
        }
        Terminal record;
        record.status = verdict;
        record.pending = live->second.ref;
        record.height = height;
        EraseLive(live);
        // Live entries and terminal records are disjoint (Insert removes the
        // record of a re-admitted action), so there is no record to replace.
        m_terminal.emplace(id, std::move(record));
        m_other_order.emplace_back(height, id);
        ++m_other_count;
        if (verdict == PoolStatus::NONCE_USED) ++report.nonce_used;
        if (verdict == PoolStatus::NONCE_TOO_LOW) ++report.nonce_too_low;
        if (verdict == PoolStatus::EXPIRED) ++report.expired;
    }
    m_head_height = height;

    // Retention: keep a record of slot s while height - s < retention_slots.
    const auto retained = [&](uint64_t record_height) { return height - record_height < m_limits.retention_slots; };
    while (!m_included_order.empty() && !retained(m_included_order.front().first)) {
        const auto [record_height, id] = m_included_order.front();
        m_included_order.pop_front();
        const auto it{m_terminal.find(id)};
        if (it != m_terminal.end() && it->second.status == PoolStatus::INCLUDED && it->second.height == record_height) {
            m_terminal.erase(it);
            ++report.evicted;
        }
    }
    while (!m_other_order.empty() && (!retained(m_other_order.front().first) || m_other_count > m_limits.terminal_capacity)) {
        const auto [record_height, id] = m_other_order.front();
        m_other_order.pop_front();
        const auto it{m_terminal.find(id)};
        if (it != m_terminal.end() && it->second.status != PoolStatus::INCLUDED && it->second.height == record_height) {
            m_terminal.erase(it);
            --m_other_count;
            ++report.evicted;
        }
    }
    return Ok(report);
}

std::optional<PoolEntryStatus> ActionPool::Status(const ActionId& id) const
{
    LOCK(m_mutex);
    if (const auto live{m_live.find(id)}; live != m_live.end()) {
        return PoolEntryStatus{live->second.status, live->second.ref, std::nullopt, live->second.proposal_height};
    }
    if (const auto terminal{m_terminal.find(id)}; terminal != m_terminal.end()) {
        return PoolEntryStatus{terminal->second.status, terminal->second.pending, terminal->second.included, terminal->second.height};
    }
    return std::nullopt;
}

size_t ActionPool::Size() const
{
    LOCK(m_mutex);
    return m_live.size();
}

uint64_t ActionPool::HeadHeight() const
{
    LOCK(m_mutex);
    return m_head_height;
}

size_t ActionPool::IncludedRecords() const
{
    LOCK(m_mutex);
    return m_terminal.size() - m_other_count;
}

size_t ActionPool::TerminalRecords() const
{
    LOCK(m_mutex);
    return m_other_count;
}

} // namespace flowmesh2::admission
