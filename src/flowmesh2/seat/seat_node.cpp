// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/seat/seat_node.h>

#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/core/facts.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace flowmesh2::seat {

namespace {

//! The error codes that fence the node before the head moves (O-90).
bool FencesNode(ErrorCode code)
{
    switch (code) {
    case ErrorCode::STORAGE_FATAL:
    case ErrorCode::FENCED:
    case ErrorCode::AUTHORITY_CHANGED:
    case ErrorCode::EQUIVOCATION_REFUSED:
    case ErrorCode::INVARIANT:
    case ErrorCode::ORDER_VIOLATION:
        return true;
    default:
        return false;
    }
}

} // namespace

SeatNode::SeatNode(const SeatNodeConfig& config, const SeatNodeParts& parts, const SeatSet& seats, const state::HeadView& genesis)
    : m_config{config},
      m_hooks{parts.hooks != nullptr ? parts.hooks : &m_no_hooks},
      m_chain{parts.chain},
      m_signer_journal{parts.signer_journal},
      m_commit_journal{parts.commit_journal},
      m_budget{parts.budget},
      m_mesh{parts.mesh},
      m_sign_observer{parts.sign_observer},
      m_factory{parts.agreement},
      m_seats{seats},
      m_genesis{genesis}
{
}

SeatNode::~SeatNode() = default;

Result<std::unique_ptr<SeatNode>> SeatNode::Create(const SeatNodeConfig& config, const SeatNodeParts& parts, const SeatSet& seats,
                                                   const state::HeadView& genesis, bool restart)
{
    if (parts.chain == nullptr || parts.signer_journal == nullptr || parts.commit_journal == nullptr || parts.budget == nullptr) {
        return Error{ErrorCode::INVALID_INPUT, SEAT_PARTS_MISSING};
    }
    if (config.seat_count == 0 || config.self_seat >= config.seat_count) return Error{ErrorCode::INVALID_INPUT, SEAT_RANGE};
    if (config.seat_count != seats.Size()) return Error{ErrorCode::INVALID_INPUT, SEAT_SET_SIZE};
    if (config.seat_count > 1 && parts.mesh == nullptr) return Error{ErrorCode::INVALID_INPUT, SEAT_MESH_MISSING};
    if (config.seat_count == 1 && !parts.agreement) return Error{ErrorCode::INVALID_INPUT, SEAT_NO_AGREEMENT};
    std::unique_ptr<SeatNode> node{new SeatNode{config, parts, seats, genesis}};
    if (const Result<void> built{node->BuildComponents(restart)}; !built.has_value()) return built.error();
    return Ok(std::move(node));
}

Result<void> SeatNode::BuildComponents(bool restart)
{
    // Logs: fresh over empty journals, rebuilt from history on a restart (M4-STO-01).
    Result<storage::SignerLog> signer_log{restart ? storage::SignerLog::Recover(*m_signer_journal, m_config.profile)
                                                  : storage::SignerLog::Create(*m_signer_journal)};
    if (!signer_log.has_value()) return signer_log.error();
    m_signer_log = std::make_unique<storage::SignerLog>(std::move(signer_log).value());
    Result<storage::CommitLog> commit_log{restart ? storage::CommitLog::Recover(*m_commit_journal, m_config.profile)
                                                  : storage::CommitLog::Create(*m_commit_journal)};
    if (!commit_log.has_value()) return commit_log.error();
    m_commit_log = std::make_unique<storage::CommitLog>(std::move(commit_log).value());
    // Recorded evidence (M4-AGR-07): a restarted node that recorded any stays halted.
    Result<agreement::EvidenceStore> evidence{agreement::EvidenceStore::Create(m_config.profile, m_genesis.Header().domain, *m_chain, *m_commit_log)};
    if (!evidence.has_value()) return evidence.error();
    m_evidence = std::make_unique<agreement::EvidenceStore>(std::move(evidence).value());

    // The signer of this seat.
    Result<storage::SeatSigner> signer{
        storage::SeatSigner::FromIKM(std::span<const unsigned char>{m_config.ikm.data(), m_config.ikm.size()}, *m_signer_log)};
    if (!signer.has_value()) return signer.error();
    m_signer = std::make_unique<storage::SeatSigner>(std::move(signer).value());
    m_signer->SetSignObserver(m_sign_observer);
    if (!(m_seats.Keys()[m_config.self_seat].Key() == m_signer->PublicKey())) return Error{ErrorCode::INVALID_INPUT, SEAT_BAD_KEY};

    // State: the genesis, or the committed head replayed from the commit journal on a restart.
    Result<std::unique_ptr<state::StateOwner>> owner{state::StateOwner::Create(m_config.profile, m_genesis, m_commit_journal->InstanceId())};
    if (!owner.has_value()) return owner.error();
    m_owner = std::move(owner).value();
    if (restart) {
        Result<std::vector<storage::JournalRecord>> records{m_commit_journal->ReadAll()};
        if (!records.has_value()) return records.error();
        if (const Result<void> restored{m_owner->Restore(m_genesis, *records)}; !restored.has_value()) return restored.error();
    }
    const std::shared_ptr<const state::HeadView> head{m_owner->Head()};

    // Reads, the pool, the budget, admission, the lanes and the API.
    const read::ReadLimits limits{8, read::CertifiedEnvelope::MAX_ENCODED_BYTES, 8, m_config.profile.max_delay_slots};
    Result<std::unique_ptr<read::ReadService>> read{read::ReadService::Create(limits, m_config.profile)};
    if (!read.has_value()) return read.error();
    m_read = std::move(read).value();
    // A restart's pool starts at the first slot the pool and the read service retain; RebuildRetained brings both to
    // the committed head (M4-STO-03's pool rebuild, O-71).
    const admission::PoolLimits pool_limits{admission::PoolLimits::FromProfile(m_config.profile)};
    const uint64_t committed{head->Header().height};
    const uint64_t window{std::max<uint64_t>({pool_limits.retention_slots, limits.outcome_index_slots, limits.view_slots})};
    const uint64_t base{restart ? committed - std::min(committed, window) : committed};
    Result<std::unique_ptr<admission::ActionPool>> pool{admission::ActionPool::Create(pool_limits, m_config.profile, base)};
    if (!pool.has_value()) return pool.error();
    m_pool = std::move(pool).value();
    Result<std::unique_ptr<admission::Admission>> admission{
        admission::Admission::Create(m_config.profile, *m_owner, *m_read, *m_pool, *m_budget)};
    if (!admission.has_value()) return admission.error();
    m_admission = std::move(admission).value();
    Result<std::unique_ptr<net::LaneSet>> lanes{net::LaneSet::Create(net::ProvisionalLaneLimits(m_config.profile))};
    if (!lanes.has_value()) return lanes.error();
    m_lanes = std::move(lanes).value();
    Result<std::unique_ptr<net::StreamHub>> streams{net::StreamHub::Create(m_config.streams, m_config.stream_epoch)};
    if (!streams.has_value()) return streams.error();
    m_streams = std::move(streams).value();
    Result<std::unique_ptr<net::InProcessApi>> api{net::InProcessApi::Create(m_config.profile, *m_lanes, *m_admission, *m_pool, *m_read,
                                                                             net::ApiLimits{m_config.max_replies}, m_streams.get())};
    if (!api.has_value()) return api.error();
    m_api = std::move(api).value();

    // A restart's decided-but-unapplied tail (review 2026-11-16, defect 8; the first half of M4-STO-03): every DECISION
    // after the committed head is re-executed and applied under the current authority before the agreement starts. A
    // refusal keeps the head where it is and fences the node, which then starts and serves reads (O-89: that height is
    // never re-decided with another header); a storage failure is the restart's error.
    if (restart) {
        if (const Result<void> rebuilt{RebuildRetained(base)}; !rebuilt.has_value()) return rebuilt.error();
        if (const Result<void> tail{ApplyUnappliedTail()}; !tail.has_value()) {
            Hooks().Record("restart.tail", std::string{ToString(tail.error().code)} + "," + std::string{tail.error().detail});
            if (tail.error().code == ErrorCode::STORAGE_FATAL || tail.error().code == ErrorCode::INVARIANT) return tail.error();
        }
    }

    // The agreement: the caller's k = 1 certifier, or PBFT over the mesh.
    m_certifier.reset();
    m_pbft.reset();
    m_proposed.reset();
    m_proposed_ids.clear();
    if (m_config.seat_count == 1) {
        const AgreementParts agreement_parts{m_config.profile, *m_chain, m_seats, *m_signer, *m_signer_log, *m_commit_log};
        Result<std::unique_ptr<agreement::Agreement>> certifier{m_factory(agreement_parts)};
        if (!certifier.has_value()) return certifier.error();
        m_certifier = std::move(certifier).value();
        return Ok();
    }
    m_rules = std::make_unique<agreement::PbftSafetyRules>();
    agreement::PbftConfig config{m_config.pbft};
    config.self_seat = m_config.self_seat;
    config.start_ms = m_now_ms;
    const agreement::PbftContext context{*m_chain, *m_owner, *m_signer, *m_signer_log, *m_commit_log, *m_rules, *m_mesh, *m_mesh, *m_evidence};
    Result<std::unique_ptr<agreement::PbftAgreement>> pbft{agreement::PbftAgreement::Create(m_config.profile, config, context)};
    if (!pbft.has_value()) return pbft.error();
    m_pbft = std::move(pbft).value();
    return Ok();
}

void SeatNode::Fence(const Error& error)
{
    if (!m_admission->Fenced()) Hooks().Record("node.fence", std::string{ToString(error.code)});
    m_admission->Fence();
}

Error SeatNode::FenceOn(const Error& error)
{
    if (FencesNode(error.code)) Fence(error);
    return error;
}

Error SeatNode::FenceAfterApply(const Error& error)
{
    // The head already moved: the pool, the read service or the commit log
    // would disagree with it on every later call, so the node stops (O-90).
    Fence(error);
    return error;
}

Result<SealReport> SeatNode::SealSlot()
{
    const std::shared_ptr<const state::HeadView> head{m_owner->Head()};
    const uint64_t height{head->Header().height + 1};
    Hooks().BeginSlot(height);
    Result<SealReport> sealed{SealNext(head, height)};
    Hooks().EndSlot(height, sealed.has_value() ? std::string_view{"OK"} : ToString(sealed.error().code));
    return sealed;
}

Result<SealReport> SeatNode::SealNext(const std::shared_ptr<const state::HeadView>& head, uint64_t height)
{
    if (m_config.seat_count != 1) return Error{ErrorCode::INVALID_INPUT, SEAT_MULTI_SEAT};
    Result<std::pair<state::Candidate, std::vector<ActionId>>> prepared{Prepare(head, height)};
    if (!prepared.has_value()) return prepared.error();
    state::Candidate& candidate{prepared->first};

    // 5. Agreement: checks 1 and 2, B-INTENT, sign, B-VOTE, B-DECISION.
    if (const Result<void> proposed{m_certifier->Propose(candidate, candidate.Header().Authority())}; !proposed.has_value()) {
        return FenceOn(proposed.error());
    }
    std::optional<agreement::Decision> decision{m_certifier->TakeDecision()};
    if (!decision.has_value()) return FenceOn(Error{ErrorCode::INVARIANT, SEAT_NO_DECISION});

    // The hooks' step after the decision is durable (the simulator's authority bump, which check 3 refuses, O-89; or its
    // crash, which fences the node with the DECISION durable and nothing applied: a restart recovers that).
    if (const Result<void> after{Hooks().AfterDecision(height)}; !after.has_value()) return FenceOn(after.error());
    return Finish(height, std::move(candidate), std::move(*decision), std::move(prepared->second));
}

Result<std::pair<state::Candidate, std::vector<ActionId>>> SeatNode::Prepare(const std::shared_ptr<const state::HeadView>& head, uint64_t height)
{
    // 0. A fenced node seals nothing: no poll, take, preview, signature or write (O-89).
    if (m_admission->Fenced()) return Error{ErrorCode::FENCED, SEAT_FENCED};

    // 1. Facts.
    const FactCursor from{head->Header().facts_to};
    Result<chain::PollReply> polled{m_chain->Poll(from, MAX_SLOT_FACTS)};
    if (!polled.has_value()) return FenceOn(polled.error());
    Hooks().Record("chain.poll", "from=" + std::to_string(from.height) + ":" + std::to_string(from.index) + "," +
                                     std::string{chain::ToString(polled->Status())});
    switch (polled->Status()) {
    case chain::PollStatus::FACTS:
        break;
    case chain::PollStatus::BUSY:
        return Error{ErrorCode::BUSY, SEAT_POLL_BUSY};
    case chain::PollStatus::NEED_DATA:
        return Error{ErrorCode::NEED_DATA, SEAT_POLL_NEED_DATA};
    case chain::PollStatus::AUTHORITY_CHANGED:
        return FenceOn(Error{ErrorCode::AUTHORITY_CHANGED, SEAT_POLL_AUTHORITY});
    }
    const chain::FactBatch& batch{*polled->Batch()};

    // 2. Actions, minus the withheld ones (the filter runs without the pool lock).
    const admission::WithholdFilter withhold{[this](const ActionV2& action) { return Hooks().Withhold(action); }};
    Result<admission::TakenSlot> taken{m_pool->TakeForSlot(height, m_config.profile.max_actions_per_slot, withhold)};
    if (!taken.has_value()) return FenceOn(taken.error());
    Hooks().Record("pool.take", "slot=" + std::to_string(height) + ",n=" + std::to_string(taken->originals.size()));

    // 3. Preview on the committed head.
    state::SlotInputs inputs;
    inputs.authority = batch.authority;
    inputs.anchor = batch.anchor;
    inputs.facts_from = batch.from;
    inputs.facts_to = batch.to;
    for (const Fact& fact : batch.facts) {
        Result<std::vector<unsigned char>> bytes{EncodeFact(fact)};
        if (!bytes.has_value()) return FenceOn(bytes.error());
        inputs.body.facts.push_back(std::move(bytes).value());
    }
    inputs.body.originals = taken->originals;
    // An engine error that actions cause (O-34) leaves them out (M4-ADM-03); one a fact causes is returned unfenced and
    // the slot is retried.
    Result<state::Candidate> candidate{m_owner->Preview(state::ParentRef::Committed(*head), inputs)};
    if (!candidate.has_value() && !taken->ids.empty() && !FencesNode(candidate.error().code) &&
        candidate.error().code != ErrorCode::STALE_PARENT) {
        const Error failed{candidate.error()};
        candidate = LeaveOutBreakingActions(*head, height, inputs, *taken, failed);
    }
    if (!candidate.has_value()) return FenceOn(candidate.error());
    Hooks().Record("state.preview", "slot=" + std::to_string(height) + ",header=" + candidate->Header().Hash().GetHex());

    // 4. The hooks' step between Preview and Propose (the simulator's authority bump and fault mapping check).
    if (const Result<void> after{Hooks().AfterPreview(height)}; !after.has_value()) return FenceOn(after.error());
    return Ok(std::pair{std::move(candidate).value(), taken->ids});
}

Result<state::Candidate> SeatNode::LeaveOutBreakingActions(const state::HeadView& head, uint64_t height, state::SlotInputs& inputs,
                                                           admission::TakenSlot& taken, const Error& failed)
{
    const state::ParentRef parent{state::ParentRef::Committed(head)};
    size_t previews{0};
    // The slot with `originals` (admission order; the engine's result does not depend on body order).
    const auto preview = [&](std::vector<std::vector<unsigned char>> originals) {
        ++previews;
        inputs.body.originals = std::move(originals);
        return m_owner->Preview(parent, inputs);
    };
    const auto first = [&](size_t n) {
        return preview({taken.originals.begin(), taken.originals.begin() + static_cast<std::ptrdiff_t>(n)});
    };
    // An error that ends the search: it fences the node, or the head moved.
    const auto ends = [](const Error& error) { return FencesNode(error.code) || error.code == ErrorCode::STALE_PARENT; };
    const auto stop = [&](const Error& error) {
        inputs.body.originals = taken.originals;
        return Result<state::Candidate>{error};
    };
    const auto trace = [&](std::string_view step, const ActionId& id, const Error& error) {
        Hooks().Record(step, "slot=" + std::to_string(height) + ",action=" + id.id.GetHex() + "," + std::string{ToString(error.code)} +
                                 "," + std::string{error.detail});
    };
    // Without actions the slot must have a candidate, or a fact (not an action) breaks it: nothing is left out.
    Result<state::Candidate> valid{first(0)};
    if (!valid.has_value()) return stop(failed);
    // Invariant: the first `lo` taken actions give the candidate `valid`, and all of them (more than lo) give `last`.
    size_t lo{0};
    Error last{failed};
    while (previews < MAX_SLOT_PREVIEWS) {
        // The boundary: the first lo give a candidate, the first hi none. Exponential search from lo, then bisection.
        size_t hi{taken.originals.size()};
        for (size_t step{1}; lo + step < hi && previews < MAX_SLOT_PREVIEWS; step *= 2) {
            Result<state::Candidate> probe{first(lo + step)};
            if (!probe.has_value()) {
                if (ends(probe.error())) return stop(probe.error());
                hi = lo + step;
                break;
            }
            lo += step;
            valid = std::move(probe);
        }
        while (hi - lo > 1 && previews < MAX_SLOT_PREVIEWS) {
            const size_t mid{lo + (hi - lo) / 2};
            Result<state::Candidate> probe{first(mid)};
            if (probe.has_value()) {
                lo = mid;
                valid = std::move(probe);
            } else if (ends(probe.error())) {
                return stop(probe.error());
            } else {
                hi = mid;
            }
        }
        if (hi - lo > 1) break; // the budget ran out before the boundary was found
        // Action lo breaks the slot after the first lo. Alone (for lo = 0 that is the probe just made) it either gives no
        // candidate, so it is the cause and terminal; or it needs the actions before it, so it only waits for a later slot.
        bool alone_breaks{lo == 0};
        if (lo > 0) {
            if (previews >= MAX_SLOT_PREVIEWS) break;
            Result<state::Candidate> alone{preview({taken.originals[lo]})};
            if (!alone.has_value() && ends(alone.error())) return stop(alone.error());
            alone_breaks = !alone.has_value();
        }
        const ActionId id{taken.ids[lo]};
        if (alone_breaks) {
            if (const Result<void> out{m_pool->Exclude(height, id)}; !out.has_value()) return stop(out.error());
            trace("pool.exclude", id, last);
        } else {
            trace("pool.defer", id, last);
        }
        taken.ids.erase(taken.ids.begin() + static_cast<std::ptrdiff_t>(lo));
        taken.originals.erase(taken.originals.begin() + static_cast<std::ptrdiff_t>(lo));
        if (taken.originals.size() == lo || previews >= MAX_SLOT_PREVIEWS) break;
        Result<state::Candidate> again{first(taken.originals.size())};
        if (again.has_value()) return again;
        if (ends(again.error())) return stop(again.error());
        last = again.error();
    }
    // The slot is the first lo, whose candidate was seen; the rest stay in the pool (IN_PROPOSAL until the slot commits,
    // then PENDING) and a later slot takes them again.
    for (size_t i{lo}; i < taken.ids.size(); ++i) trace("pool.defer", taken.ids[i], last);
    taken.ids.resize(lo);
    taken.originals.resize(lo);
    inputs.body.originals = taken.originals;
    return valid;
}

Result<void> SeatNode::RebuildRetained(uint64_t base)
{
    const std::shared_ptr<const state::HeadView> restored{m_owner->Head()};
    const uint64_t committed{restored->Header().height};
    if (committed == base) return Ok(); // nothing committed yet
    Result<std::unique_ptr<state::StateOwner>> created{state::StateOwner::Create(m_config.profile, m_genesis, m_commit_journal->InstanceId())};
    if (!created.has_value()) return created.error();
    state::StateOwner& replayer{**created};
    for (uint64_t height{1}; height <= committed; ++height) {
        const std::optional<std::vector<unsigned char>> bytes{m_commit_log->DecisionRecordAt(height)};
        if (!bytes.has_value()) return Error{ErrorCode::INVARIANT, SEAT_REBUILD_GAP};
        Result<storage::DecisionRecord> record{storage::DecisionRecord::Decode(*bytes, m_config.profile)};
        if (!record.has_value()) return record.error();
        Result<state::Candidate> candidate{replayer.Reexecute(record->header, record->body)};
        if (!candidate.has_value()) return candidate.error();
        const std::vector<OutcomeV2> outcomes{candidate->Outcomes()};
        // The DECISION record is durable: AppendDecision names it again (idempotent), as in ApplyUnappliedTail.
        Result<storage::PendingDecision> pending{m_commit_log->AppendDecision(record->header, record->certificate, record->body)};
        if (!pending.has_value()) return pending.error();
        Result<DurableDecision> decision{m_commit_log->Confirm(*pending)};
        if (!decision.has_value()) return decision.error();
        // Its APPLIED record is durable too (height <= the committed head), so check 3 passed under the authority the
        // header names: applying under that authority on the replayer gives the slot's receipt.
        Result<state::AppliedSlot> applied{replayer.Apply(std::move(candidate).value(), *decision, record->header.Authority())};
        if (!applied.has_value()) return applied.error();
        if (height <= base) continue;
        // AppendApplied names the durable APPLIED record again (idempotent); Confirm gives the slot's DurableApplied.
        Result<storage::PendingApplied> pending_applied{m_commit_log->AppendApplied(applied->receipt)};
        if (!pending_applied.has_value()) return pending_applied.error();
        Result<DurableApplied> token{m_commit_log->Confirm(*pending_applied)};
        if (!token.has_value()) return token.error();
        Result<std::shared_ptr<const read::SlotView>> view{read::SlotView::Build(*applied->head, record->certificate, outcomes)};
        if (!view.has_value()) return view.error();
        if (const Result<void> published{m_read->Publish(*token, *view)}; !published.has_value()) return published.error();
        Result<admission::CommitReport> report{m_pool->OnCommitted(*token, *applied->head, outcomes, record->body)};
        if (!report.has_value()) return report.error();
    }
    const std::shared_ptr<const state::HeadView> replayed{replayer.Head()};
    if (replayed->HeaderHash() != restored->HeaderHash() || replayed->CertHash() != restored->CertHash() ||
        !(replayed->State() == restored->State())) {
        return Error{ErrorCode::INVARIANT, SEAT_REBUILD_HEAD};
    }
    Hooks().Record("restart.rebuild", "from=" + std::to_string(base + 1) + ",to=" + std::to_string(committed));
    return Ok();
}

Result<void> SeatNode::ApplyUnappliedTail()
{
    for (;;) {
        const std::shared_ptr<const state::HeadView> head{m_owner->Head()};
        const uint64_t height{head->Header().height + 1};
        const std::optional<std::vector<unsigned char>> bytes{m_commit_log->DecisionRecordAt(height)};
        if (!bytes.has_value()) return Ok(); // the tail ends where the decisions end
        Result<storage::DecisionRecord> record{storage::DecisionRecord::Decode(*bytes, m_config.profile)};
        if (!record.has_value()) return FenceOn(record.error());
        if (record->header.parent_hash != head->HeaderHash()) return FenceOn(Error{ErrorCode::STALE_PARENT, SEAT_SYNC_PARENT});
        Result<state::Candidate> candidate{m_owner->Reexecute(record->header, record->body)};
        if (!candidate.has_value()) return FenceOn(candidate.error());
        // The decision token: the DECISION record is durable already, and AppendDecision names it again (idempotent).
        Result<storage::PendingDecision> pending{m_commit_log->AppendDecision(record->header, record->certificate, record->body)};
        if (!pending.has_value()) return FenceOn(pending.error());
        Result<DurableDecision> durable{m_commit_log->Confirm(*pending)};
        if (!durable.has_value()) return FenceOn(durable.error());
        agreement::Decision decision{std::move(durable).value(), record->certificate, record->body, record->header};
        Hooks().BeginSlot(height);
        Hooks().Record("restart.apply", "slot=" + std::to_string(height) + ",header=" + record->header.Hash().GetHex());
        Result<SealReport> finished{Finish(height, std::move(candidate).value(), std::move(decision), {})};
        Hooks().EndSlot(height, finished.has_value() ? std::string_view{"OK"} : ToString(finished.error().code));
        if (!finished.has_value()) return finished.error();
    }
}

Result<SealReport> SeatNode::Finish(uint64_t height, state::Candidate candidate, agreement::Decision decision, std::vector<ActionId> taken)
{
    // 6. Apply under the authority read now (check 3).
    const AuthorityVersion current{m_chain->CurrentAuthority()};
    state::Candidate kept{candidate};
    std::vector<OutcomeV2> outcomes{candidate.Outcomes()};
    Result<state::AppliedSlot> applied{m_owner->Apply(std::move(candidate), decision.decision, current)};
    if (!applied.has_value()) return FenceOn(applied.error());
    const std::shared_ptr<const state::HeadView> next{applied->head};
    Hooks().Record("state.apply", "slot=" + std::to_string(height) + ",root=" + next->Header().state_root.GetHex());

    // 7. B-APPLIED.
    // From here on the head has moved: every error fences the node (O-90).
    Result<storage::PendingApplied> pending{m_commit_log->AppendApplied(applied->receipt)};
    if (!pending.has_value()) return FenceAfterApply(pending.error());
    if (const Result<uint64_t> barrier{m_commit_log->Barrier()}; !barrier.has_value()) return FenceAfterApply(barrier.error());
    Result<DurableApplied> token{m_commit_log->Confirm(*pending)};
    if (!token.has_value()) return FenceAfterApply(token.error());

    // 8. Publication, after B-APPLIED.
    Result<std::shared_ptr<const read::SlotView>> view{read::SlotView::Build(*next, decision.certificate, outcomes)};
    if (!view.has_value()) return FenceAfterApply(view.error());
    if (const Result<void> published{m_read->Publish(*token, *view)}; !published.has_value()) return FenceAfterApply(published.error());
    Hooks().Record("read.publish", "slot=" + std::to_string(height));
    m_streams->Publish(next, *view);

    // 9. The pool learns of the slot last.
    Result<admission::CommitReport> report{m_pool->OnCommitted(*token, *next, outcomes, decision.body)};
    if (!report.has_value()) return FenceAfterApply(report.error());
    Hooks().Record("pool.on_committed", "slot=" + std::to_string(height) + ",included=" + std::to_string(report->included) +
                                            ",recorded=" + std::to_string(report->recorded) + ",nonce_used=" +
                                            std::to_string(report->nonce_used) + ",nonce_too_low=" +
                                            std::to_string(report->nonce_too_low) + ",expired=" + std::to_string(report->expired) +
                                            ",returned=" + std::to_string(report->returned));

    SealReport out{height, next->HeaderHash(), next->Header().action_count, next->Header().outcome_count};
    Hooks().OnSealed(SealedSlot{height, next, std::move(kept), std::move(decision), std::move(outcomes), std::move(view).value(), *report,
                                std::move(taken)});
    return Ok(out);
}

// ---- Multi-seat (k > 1) --------------------------------------------------------------

bool SeatNode::IsLeader() { return m_pbft != nullptr && m_pbft->IsLeader(); }

std::optional<agreement::PbftStatus> SeatNode::AgreementStatus()
{
    if (m_pbft == nullptr) return std::nullopt;
    return m_pbft->Status();
}

Result<uint64_t> SeatNode::ProposeSlot()
{
    if (m_pbft == nullptr) return Error{ErrorCode::INVALID_INPUT, SEAT_MULTI_SEAT};
    if (!m_pbft->IsLeader()) return Error{ErrorCode::INVALID_INPUT, SEAT_NOT_LEADER};
    // The view holds this seat's proposal already (resumed after a restart, ARCHITECTURE.md 17.3 item 15(a), or proposed
    // before): the retry timer resends it. Prepare would take pool actions for a candidate Propose then refuses as
    // ORDER_VIOLATION PBFT_PROPOSAL_PENDING, which fences the node.
    if (m_pbft->Status().proposal_accepted) return Error{ErrorCode::BUSY, SEAT_PROPOSAL_IN_FLIGHT};
    // A NEW_VIEW selected a header: re-propose it when this seat holds it, with the parent certificate the header binds; fetch
    // it (Tick) when not. A fresh candidate would be refused in either case (PBFT_NEW_VIEW_SELECTED; O-101).
    if (const std::optional<uint256> selected{m_pbft->SelectedHeader()}; selected.has_value()) {
        const std::optional<agreement::Proposal> known{m_pbft->KnownProposal(*selected)};
        if (!known.has_value()) return Error{ErrorCode::NEED_DATA, SEAT_SELECTED_MISSING};
        Hooks().Record("slot.repropose", "slot=" + std::to_string(known->header.height) + ",seat=" + std::to_string(m_config.self_seat));
        if (const Result<void> proposed{m_pbft->ProposeSelected(known->header, known->body, known->parent_cert)}; !proposed.has_value()) {
            return FenceOn(proposed.error());
        }
        return Ok(known->header.height);
    }
    const std::shared_ptr<const state::HeadView> head{m_owner->Head()};
    const uint64_t height{head->Header().height + 1};
    Hooks().Record("slot.propose", "slot=" + std::to_string(height) + ",seat=" + std::to_string(m_config.self_seat));
    Result<std::pair<state::Candidate, std::vector<ActionId>>> prepared{Prepare(head, height)};
    if (!prepared.has_value()) return prepared.error();
    if (const Result<void> proposed{m_pbft->Propose(prepared->first, prepared->first.Header().Authority())}; !proposed.has_value()) {
        return FenceOn(proposed.error());
    }
    m_proposed = std::move(prepared->first);
    m_proposed_ids = std::move(prepared->second);
    return Ok(height);
}

Result<void> SeatNode::OnFrame(const net::InboundFrame& frame)
{
    if (m_pbft == nullptr) return Error{ErrorCode::INVALID_INPUT, SEAT_MULTI_SEAT};
    if (frame.lane == net::Lane::BULK) return OnBulk(frame);
    if (frame.lane != net::Lane::CRITICAL && frame.lane != net::Lane::PROPOSAL) return Ok();
    const Result<void> handled{m_pbft->OnMessage(frame.from_seat, frame.bytes)};
    // A message about a slot this seat has decided means the peer is behind: push it the decision (O-105 (10)). The agreement
    // ignores such a message itself, and a peer stuck in a view change for that slot may have nobody else to learn from.
    if (const std::optional<uint64_t> slot{m_pbft->LastMessageSlot()}; slot.has_value() && *slot <= m_owner->Head()->Header().height) {
        (void)PushDecision(frame.from_seat, *slot);
    }
    if (!handled.has_value()) {
        if (handled.error().code == ErrorCode::STORAGE_FATAL || handled.error().code == ErrorCode::FENCED) Fence(handled.error());
        Hooks().Record("pbft.refuse", "seat=" + std::to_string(m_config.self_seat) + ",from=" + std::to_string(frame.from_seat) + "," +
                                          std::string{ToString(handled.error().code)} + "," + std::string{handled.error().detail});
        return handled.error();
    }
    return Ok();
}

Result<void> SeatNode::Tick(uint64_t now_ms)
{
    if (m_pbft == nullptr) return Error{ErrorCode::INVALID_INPUT, SEAT_MULTI_SEAT};
    m_now_ms = std::max(m_now_ms, now_ms);
    const Result<void> ticked{m_pbft->OnTimeout(now_ms)};
    if (!ticked.has_value()) {
        if (ticked.error().code == ErrorCode::STORAGE_FATAL || ticked.error().code == ErrorCode::FENCED) Fence(ticked.error());
        return ticked;
    }
    // The catch-up driver (O-101): one request in flight, retried with a doubling period.
    const agreement::PbftStatus status{m_pbft->Status()};
    if (status.halted || m_admission->Fenced()) return Ok();
    const uint64_t next{m_owner->Head()->Header().height + 1};
    const bool behind{status.ahead_hint > next};
    const bool needs_proposal{status.leader && status.selected_missing};
    if (!behind && !needs_proposal) {
        m_sync.pending_peer.reset();
        return Ok();
    }
    if (m_sync.progressed) {
        m_sync.progressed = false;
        if (m_sync.pending_peer.has_value()) m_sync.sent_ms = m_now_ms; // the request in flight is still answering
    }
    if (m_sync.pending_peer.has_value() && m_now_ms - m_sync.sent_ms < m_sync.backoff_ms) return Ok();
    if (m_sync.pending_peer.has_value()) m_sync.backoff_ms = std::min(m_sync.backoff_ms * 2, m_config.sync_backoff_max_ms);
    if (m_sync.backoff_ms == 0) m_sync.backoff_ms = m_config.sync_backoff_ms;
    const uint32_t peer{NextPeer()};
    m_sync.pending_peer = peer;
    m_sync.pending_slot = next;
    m_sync.sent_ms = m_now_ms;
    if (behind) {
        m_sync.pending_proposal = false;
        Hooks().Record("sync.request", "slot=" + std::to_string(next) + ",to=" + std::to_string(peer) + ",hint=" + std::to_string(status.ahead_hint));
        m_sync.pending_max = net::MAX_SYNC_DECISIONS;
        return SendSync(peer, net::DecisionsRequest{next, net::MAX_SYNC_DECISIONS});
    }
    m_sync.pending_proposal = true;
    const uint256 selected{m_pbft->SelectedHeader().value_or(uint256{})};
    Hooks().Record("sync.request", "proposal=" + selected.GetHex() + ",slot=" + std::to_string(next) + ",to=" + std::to_string(peer));
    return SendSync(peer, net::ProposalRequest{next, selected});
}

void SeatNode::OnLocalWork()
{
    if (m_pbft != nullptr) m_pbft->OnLocalWork();
}

uint32_t SeatNode::NextPeer()
{
    for (uint32_t tries{0}; tries < m_config.seat_count; ++tries) {
        const uint32_t peer{m_sync.next_peer};
        m_sync.next_peer = (m_sync.next_peer + 1) % m_config.seat_count;
        if (peer != m_config.self_seat) return peer;
    }
    return (m_config.self_seat + 1) % m_config.seat_count;
}

Result<void> SeatNode::SendSync(uint32_t seat, const net::SyncMessage& message)
{
    Result<std::vector<unsigned char>> bytes{net::EncodeSync(message, m_config.profile)};
    if (!bytes.has_value()) return bytes.error();
    const Result<void> sent{m_mesh->Send(seat, net::Lane::BULK, *bytes)};
    // BUSY: the lane to that peer is full; the retry period asks again (or another peer).
    if (!sent.has_value() && sent.error().code != ErrorCode::BUSY) return sent.error();
    return Ok();
}

// ---- Catch-up (O-101) --------------------------------------------------------------

Result<void> SeatNode::PushDecision(uint32_t peer, uint64_t slot)
{
    std::pair<uint64_t, uint64_t>& last{m_sync.pushed[peer]};
    if (last.first == slot && last.second != 0 && m_now_ms - last.second < m_config.sync_backoff_ms) return Ok();
    const std::optional<std::vector<unsigned char>> record{m_commit_log->DecisionRecordAt(slot)};
    if (!record.has_value()) return Ok(); // this seat does not hold it either: the peer's own request path remains
    last = std::pair<uint64_t, uint64_t>{slot, m_now_ms == 0 ? 1 : m_now_ms};
    Hooks().Record("sync.push", "slot=" + std::to_string(slot) + ",to=" + std::to_string(peer));
    return SendSync(peer, net::DecisionsReply{slot, *record});
}

Result<void> SeatNode::OnBulk(const net::InboundFrame& frame)
{
    Result<net::SyncMessage> decoded{net::DecodeSync(frame.bytes, m_config.profile)};
    if (!decoded.has_value()) {
        Hooks().Record("sync.refuse", "from=" + std::to_string(frame.from_seat) + "," + std::string{ToString(decoded.error().code)} + "," + std::string{decoded.error().detail});
        return decoded.error();
    }
    if (const auto* request{std::get_if<net::DecisionsRequest>(&*decoded)}) {
        for (uint64_t slot{request->from_slot}; slot < request->from_slot + request->max; ++slot) {
            const std::optional<std::vector<unsigned char>> record{m_commit_log->DecisionRecordAt(slot)};
            if (const Result<void> sent{SendSync(frame.from_seat, net::DecisionsReply{slot, record.value_or(std::vector<unsigned char>{})})};
                !sent.has_value()) {
                return sent.error();
            }
            if (!record.has_value()) break;
        }
        return Ok();
    }
    if (const auto* reply{std::get_if<net::DecisionsReply>(&*decoded)}) {
        const uint64_t next{m_owner->Head()->Header().height + 1};
        // The request in flight asked for pending_max slots from pending_slot: each reply in that range answers it, and the
        // request stays pending until the last one (or until the retry period, when a peer stops short or stays silent).
        const bool solicited{m_sync.pending_peer.has_value() && *m_sync.pending_peer == frame.from_seat && !m_sync.pending_proposal &&
                             reply->slot >= m_sync.pending_slot && reply->slot < m_sync.pending_slot + m_sync.pending_max};
        const bool last_of_batch{solicited && reply->slot + 1 == m_sync.pending_slot + m_sync.pending_max};
        // A reply for a slot applied already (a push or an earlier reply got there first) is late and ignored, asked for or not.
        if (reply->slot < next) {
            if (last_of_batch) m_sync.pending_peer.reset();
            return Ok();
        }
        if (!solicited && (reply->slot != next || reply->record.empty())) {
            // A pushed decision for the slot after the head is welcome (O-105 (10)); anything else nobody asked for is refused.
            Hooks().Record("sync.refuse", "from=" + std::to_string(frame.from_seat) + ",unsolicited");
            return Error{ErrorCode::INVALID_INPUT, SEAT_SYNC_UNSOLICITED};
        }
        if (reply->record.empty()) {
            // The peer holds nothing from this slot on. The request stays pending: the retry period (doubling) holds the next
            // request back and the next peer gets it (review of 2026-11-16: an empty reply used to free the driver at once).
            m_pbft->ResetClaim(frame.from_seat);
            Hooks().Record("sync.reply", "from=" + std::to_string(frame.from_seat) + ",slot=" + std::to_string(reply->slot) + ",none");
            return Ok();
        }
        Result<storage::DecisionRecord> record{storage::DecisionRecord::Decode(reply->record, m_config.profile)};
        if (!record.has_value()) {
            Hooks().Record("sync.refuse", "from=" + std::to_string(frame.from_seat) + "," + std::string{ToString(record.error().code)} + "," + std::string{record.error().detail});
            return record.error();
        }
        Hooks().Record("sync.reply", "from=" + std::to_string(frame.from_seat) + ",slot=" + std::to_string(reply->slot) + (solicited ? "" : ",pushed"));
        const Result<void> applied{ApplyFetched(*record)};
        if (!applied.has_value()) {
            Hooks().Record("sync.refuse", "from=" + std::to_string(frame.from_seat) + "," + std::string{ToString(applied.error().code)} + "," +
                                              std::string{applied.error().detail});
            return applied.error();
        }
        m_sync.backoff_ms = m_config.sync_backoff_ms;
        if (last_of_batch) {
            m_sync.pending_peer.reset(); // the batch is complete: the driver asks for the next one at the next tick
        } else if (solicited) {
            // The batch is still arriving: its retry period runs from its latest record that applied, not from the send
            // (review workflow of 2026-11-18). Only a record that moved the head counts; the next Tick stamps it (OnFrame
            // does not see the clock), so the period is never shortened.
            if (m_owner->Head()->Header().height == reply->slot) m_sync.progressed = true;
        } else if (m_sync.pending_peer.has_value() && m_sync.pending_slot == reply->slot) {
            m_sync.pending_peer.reset(); // a push answered the pending request: it is moot
        }
        return Ok();
    }
    if (const auto* request{std::get_if<net::ProposalRequest>(&*decoded)}) {
        std::vector<unsigned char> proposal;
        if (request->slot == m_owner->Head()->Header().height + 1) {
            if (const std::optional<agreement::Proposal> known{m_pbft->KnownProposal(request->header_hash)}) {
                Result<std::vector<unsigned char>> bytes{agreement::EncodeMessage(agreement::Message{*known}, m_config.profile)};
                if (bytes.has_value()) proposal = std::move(bytes).value();
            }
        }
        return SendSync(frame.from_seat, net::ProposalReply{request->slot, request->header_hash, std::move(proposal)});
    }
    const auto& reply{std::get<net::ProposalReply>(*decoded)};
    if (!m_sync.pending_peer.has_value() || *m_sync.pending_peer != frame.from_seat || !m_sync.pending_proposal ||
        reply.slot != m_sync.pending_slot) {
        Hooks().Record("sync.refuse", "from=" + std::to_string(frame.from_seat) + ",unsolicited");
        return Error{ErrorCode::INVALID_INPUT, SEAT_SYNC_UNSOLICITED};
    }
    m_sync.pending_peer.reset();
    if (reply.proposal.empty()) {
        Hooks().Record("sync.reply", "from=" + std::to_string(frame.from_seat) + ",proposal=none");
        return Ok();
    }
    Result<agreement::Message> message{agreement::DecodeMessage(reply.proposal, m_config.profile)};
    if (!message.has_value()) return message.error();
    const auto* proposal{std::get_if<agreement::Proposal>(&*message)};
    if (proposal == nullptr || proposal->header.Hash() != reply.header_hash) return Error{ErrorCode::INVALID_INPUT, SEAT_SYNC_UNSOLICITED};
    Hooks().Record("sync.reply", "from=" + std::to_string(frame.from_seat) + ",proposal=" + reply.header_hash.GetHex());
    const Result<void> proposed{m_pbft->ProposeSelected(proposal->header, proposal->body, proposal->parent_cert)};
    if (!proposed.has_value()) {
        Hooks().Record("sync.refuse", "from=" + std::to_string(frame.from_seat) + "," + std::string{ToString(proposed.error().code)} + "," + std::string{proposed.error().detail});
        return FenceOn(proposed.error());
    }
    m_sync.backoff_ms = m_config.sync_backoff_ms;
    return Ok();
}

Result<void> SeatNode::ApplyFetched(const storage::DecisionRecord& record)
{
    const std::shared_ptr<const state::HeadView> head{m_owner->Head()};
    const uint64_t height{record.header.height};
    if (height != head->Header().height + 1) return Error{ErrorCode::STALE_PARENT, SEAT_SYNC_SLOT};
    const uint256 hash{record.header.Hash()};
    if (m_pbft != nullptr && m_pbft->Status().decided) return Ok(); // Complete applies the agreement's own decision
    if (const std::optional<storage::DecisionRef> local{m_commit_log->DecisionAt(height)}) {
        if (local->header_hash == hash) return Ok(); // decided here already; Complete (or the restart) applies it
        // Two certified headers for one height: evidence, then the halt (M4-AGR-07).
        if (const std::optional<std::vector<unsigned char>> mine{m_commit_log->DecisionRecordAt(height)}) {
            if (const Result<storage::DecisionRecord> ours{storage::DecisionRecord::Decode(*mine, m_config.profile)}) {
                agreement::CertifiedHeader a{ours->header, ours->certificate};
                agreement::CertifiedHeader b{record.header, record.certificate};
                if (!(a.header.Hash() < b.header.Hash())) std::swap(a, b);
                const Result<void> recorded{m_evidence->Record(agreement::Evidence{agreement::ConflictingCertificates{a, b}})};
                Hooks().Record("sync.evidence", "slot=" + std::to_string(height) + "," + (recorded.has_value() ? "recorded" : "refused"));
            }
        }
        return Error{ErrorCode::EQUIVOCATION_REFUSED, SEAT_SYNC_CONFLICT};
    }
    if (record.header.parent_hash != head->HeaderHash()) return Error{ErrorCode::STALE_PARENT, SEAT_SYNC_PARENT};
    if (record.certificate.header_hash != hash) return Error{ErrorCode::VERIFY_FAILED, SEAT_SYNC_CERT_HEADER};
    Result<SeatSet> seats{m_chain->SeatSetAt(record.header.authority_epoch)};
    if (!seats.has_value()) return seats.error();
    if (const Result<void> cert{agreement::CheckCertificate(record.certificate, record.header.CommitDigest(), *seats, SEAT_SYNC_CERT)};
        !cert.has_value()) {
        return cert.error();
    }
    // Execute, then make the decision durable, then steps 6 to 9 like any follower.
    Result<state::Candidate> candidate{m_owner->Reexecute(record.header, record.body)};
    if (!candidate.has_value()) return FenceOn(candidate.error());
    Result<storage::PendingDecision> pending{m_commit_log->AppendDecision(record.header, record.certificate, record.body)};
    if (!pending.has_value()) return FenceOn(pending.error());
    if (const Result<uint64_t> barrier{m_commit_log->Barrier()}; !barrier.has_value()) return FenceOn(barrier.error());
    Result<DurableDecision> durable{m_commit_log->Confirm(*pending)};
    if (!durable.has_value()) return FenceOn(durable.error());
    agreement::Decision decision{std::move(durable).value(), record.certificate, record.body, record.header};
    m_proposed.reset();
    m_proposed_ids.clear();
    Hooks().BeginSlot(height);
    Hooks().Record("sync.apply", "slot=" + std::to_string(height) + ",header=" + hash.GetHex());
    Result<SealReport> finished{Finish(height, std::move(candidate).value(), std::move(decision), {})};
    Hooks().EndSlot(height, finished.has_value() ? std::string_view{"OK"} : ToString(finished.error().code));
    if (!finished.has_value()) return finished.error();
    return Ok();
}

Result<std::optional<SealReport>> SeatNode::Complete()
{
    if (m_pbft == nullptr) return Error{ErrorCode::INVALID_INPUT, SEAT_MULTI_SEAT};
    std::optional<agreement::Decision> decision{m_pbft->TakeDecision()};
    if (!decision.has_value()) return Ok(std::optional<SealReport>{});
    const uint64_t height{decision->header.height};
    Hooks().BeginSlot(height);
    std::optional<state::Candidate> candidate;
    std::vector<ActionId> taken;
    if (m_proposed.has_value() && m_proposed->Header().Hash() == decision->header.Hash()) {
        candidate = std::move(*m_proposed);
        taken = std::move(m_proposed_ids);
    } else {
        // A follower (or a leader whose proposal lost to another view): execute the decided slot.
        Result<state::Candidate> reexecuted{m_owner->Reexecute(decision->header, decision->body)};
        if (!reexecuted.has_value()) {
            Hooks().EndSlot(height, ToString(reexecuted.error().code));
            return FenceOn(reexecuted.error());
        }
        candidate = std::move(reexecuted).value();
    }
    m_proposed.reset();
    m_proposed_ids.clear();
    Result<SealReport> finished{Finish(height, std::move(*candidate), std::move(*decision), std::move(taken))};
    Hooks().EndSlot(height, finished.has_value() ? std::string_view{"OK"} : ToString(finished.error().code));
    if (!finished.has_value()) return finished.error();
    return Ok(std::optional<SealReport>{*finished});
}

Result<void> SeatNode::Restart() { return BuildComponents(true); }

Result<net::PumpReport> SeatNode::Pump(size_t max) { return m_api->Pump(max); }

} // namespace flowmesh2::seat
