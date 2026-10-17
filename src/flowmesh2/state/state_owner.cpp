// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/state/state_owner.h>

#include <flowmesh2/core/state_root.h>
#include <flowmesh2/storage/journal.h>

#include <utility>

namespace flowmesh2::state {

namespace {

constexpr Error Invalid(std::string_view detail) { return Error{ErrorCode::INVALID_INPUT, detail}; }
constexpr Error Stale() { return Error{ErrorCode::STALE_PARENT, PARENT_NOT_HEAD}; }

} // namespace

StateOwner::StateOwner(engine::Engine engine, std::shared_ptr<const HeadView> genesis, uint64_t commit_journal_instance)
    : m_engine{std::move(engine)}, m_commit_journal_instance{commit_journal_instance}, m_head{std::move(genesis)}
{
}

Result<std::unique_ptr<StateOwner>> StateOwner::Create(const ProfileParams& profile, const HeadView& genesis,
                                                       uint64_t commit_journal_instance)
{
    Result<engine::Engine> engine{engine::Engine::Create(profile)};
    if (!engine.has_value()) return engine.error();
    const SlotHeaderV2& header{genesis.Header()};
    if (header.height != 0 || !header.parent_hash.IsNull() || !header.parent_cert_hash.IsNull() || !genesis.CertHash().IsNull()) {
        return Invalid(OWNER_NOT_GENESIS);
    }
    if (header.profile_hash != ProfileHash(profile) || !(genesis.State().Profile() == profile)) return Invalid(OWNER_PROFILE_MISMATCH);
    if (header.seat_set_root != genesis.State().seat_set_root) return Invalid(OWNER_SEAT_SET_MISMATCH);
    const Result<StateRootsV0> roots{genesis.State().engine.Commit()};
    if (!roots.has_value()) return roots.error();
    if (roots->Root() != header.state_root) return Error{ErrorCode::VERIFY_FAILED, OWNER_STATE_ROOT_MISMATCH};
    std::unique_ptr<StateOwner> owner{
        new StateOwner{std::move(engine).value(), std::make_shared<const HeadView>(genesis), commit_journal_instance}};
    return Ok(std::move(owner));
}

std::shared_ptr<const HeadView> StateOwner::Head() const
{
    LOCK(m_mutex);
    return m_head;
}

Result<Candidate> StateOwner::Preview(const ParentRef& parent, const SlotInputs& inputs) const
{
    if (parent.kind == ParentKind::CANDIDATE) {
        // Pipelining: building on an uncommitted candidate (plan section 13: deferred until a measured need).
        return NotImplemented("M4-AGR-10"); // TODO(rebirth:M4-AGR-10)
    }
    if (parent.kind != ParentKind::COMMITTED) return Invalid(PARENT_KIND_UNKNOWN);
    const std::shared_ptr<const HeadView> head{Head()}; // the only work under the lock: a pointer copy
    if (parent.height != head->Header().height || parent.header_hash != head->HeaderHash()) return Stale();
    // Executes without the lock. The engine copies the whole state (REAL-REPLACE M2-LED-10, in BuildCandidate).
    return BuildCandidate(m_engine, *head, inputs);
}

Result<Candidate> StateOwner::Reexecute(const SlotHeaderV2& header, const SlotBody& body) const
{
    const std::shared_ptr<const HeadView> head{Head()};
    return ReexecuteOn(m_engine, *head, header, body);
}

Result<AppliedSlot> StateOwner::Apply(Candidate&& candidate, const DurableDecision& decision, const AuthorityVersion& current)
{
    const SlotHeaderV2& header{candidate.Header()};
    const std::shared_ptr<const HeadView> head{Head()};
    // A candidate always has a height >= 1 (ValidateSlotInputs gives parent height + 1).
    if (header.height == 0 || header.height - 1 != head->Header().height || header.parent_hash != head->HeaderHash()) {
        return Stale();
    }
    if (decision.JournalInstance() != m_commit_journal_instance) return Error{ErrorCode::VERIFY_FAILED, storage::TOKEN_OTHER_LOG};
    if (decision.Slot() != header.height || decision.Digest() != header.Hash()) {
        return Error{ErrorCode::VERIFY_FAILED, DECISION_OTHER_HEADER};
    }
    if (!(header.Authority() == current)) return Error{ErrorCode::AUTHORITY_CHANGED, AUTHORITY_SUPERSEDED};
    Result<HeadView> next{HeadView::Make(header, decision.Binding().cert_hash, candidate.Next())};
    if (!next.has_value()) return next.error();
    auto installed{std::make_shared<const HeadView>(std::move(next).value())};
    {
        LOCK(m_mutex);
        if (m_head != head) return Stale(); // another Apply moved the head after the checks
        m_head = installed;
    }
    ApplyReceipt receipt{decision.JournalInstance(), decision.Seq(), header.height, decision.Digest(), installed->Header().state_root};
    return Ok(AppliedSlot{std::move(installed), std::move(receipt)});
}

Result<void> StateOwner::Restore(const HeadView& /*base*/, std::span<const storage::JournalRecord> /*records*/)
{
    // Snapshot plus exactly-once replay of the DECISION records after it (plan section 3.7).
    return NotImplemented("M4-STO-03"); // TODO(rebirth:M4-STO-03)
}

} // namespace flowmesh2::state
