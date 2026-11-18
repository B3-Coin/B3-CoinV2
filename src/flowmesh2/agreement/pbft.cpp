// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/agreement/pbft.h>

#include <crypto/bls.h>
#include <flowmesh2/agreement/digests.h>
#include <flowmesh2/agreement/outbound.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/storage/tokens.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <utility>
#include <variant>

namespace flowmesh2::agreement {

namespace {

constexpr Error Invalid(std::string_view detail) { return Error{ErrorCode::INVALID_INPUT, detail}; }
constexpr Error Order(std::string_view detail) { return Error{ErrorCode::ORDER_VIOLATION, detail}; }

FactCursor FactCursorOf(const Fact& fact)
{
    return std::visit([](const auto& f) { return f.cursor; }, fact);
}

} // namespace

// ---- PbftSafetyRules -----------------------------------------------------------

void PbftSafetyRules::SetView(uint64_t epoch, uint64_t slot, uint64_t view, bool active, const uint256& selected)
{
    m_epoch = epoch;
    m_slot = slot;
    m_view = view;
    m_active = active;
    m_selected = selected;
}

void PbftSafetyRules::SetAccepted(const std::optional<uint256>& header_hash) { m_accepted = header_hash; }

Result<void> PbftSafetyRules::MayPrepare(const Proposal& proposal)
{
    if (const Result<void> shape{CheckProposal(proposal)}; !shape.has_value()) return shape.error();
    if (proposal.header.height != m_slot) return Invalid(PBFT_MESSAGE_SLOT);
    if (proposal.epoch != m_epoch) return Invalid(PBFT_EPOCH);
    if (proposal.view != m_view || !m_active) return Order(PBFT_VIEW_STALE);
    const uint256 hash{proposal.header.Hash()};
    // One proposal per (epoch, slot, view): the same again is fine, another is a double proposal.
    if (m_accepted.has_value() && *m_accepted != hash) return Error{ErrorCode::EQUIVOCATION_REFUSED, PBFT_DOUBLE_VOTE};
    // At view > 0 only the header the view's NEW_VIEW selected may be prepared (if it selected one).
    if (m_view > 0 && !m_selected.IsNull() && m_selected != hash) return Invalid(PBFT_NEW_VIEW_SELECTED);
    return Ok();
}

Result<void> PbftSafetyRules::MayCommit(const Proposal& proposal, const PreparedCert& prepared)
{
    if (!m_accepted.has_value()) return Order(PBFT_NO_PROPOSAL);
    const uint256 hash{proposal.header.Hash()};
    if (*m_accepted != hash || proposal.view != m_view || !m_active) return Order(PBFT_VIEW_STALE);
    if (prepared.view != m_view || prepared.cert.header_hash != hash) return Invalid(PBFT_PREPARED_MISMATCH);
    return Ok();
}

Result<void> PbftSafetyRules::OnViewChange(const ViewChange& report)
{
    if (const Result<void> shape{CheckViewChange(report)}; !shape.has_value()) return shape.error();
    if (report.slot != m_slot) return Invalid(PBFT_MESSAGE_SLOT);
    if (report.epoch != m_epoch) return Invalid(PBFT_EPOCH);
    return Ok();
}

// ---- PbftAgreement: construction -------------------------------------------------

Result<std::unique_ptr<PbftAgreement>> PbftAgreement::Create(const ProfileParams& profile, const PbftConfig& config, const PbftContext& context)
{
    if (const Result<void> valid{Validate(profile)}; !valid.has_value()) return valid.error();
    const AuthorityVersion authority{context.adapter.CurrentAuthority()};
    Result<SeatSet> seats{context.adapter.SeatSetAt(authority.epoch)};
    if (!seats.has_value()) return seats.error();
    if (config.self_seat >= seats->Size()) return Invalid(PBFT_SELF_SEAT);
    if (!(seats->Keys()[config.self_seat].Key() == context.signer.PublicKey())) return Invalid(PBFT_SELF_KEY);
    const std::shared_ptr<const state::HeadView> head{context.owner.Head()};
    std::optional<SlotCertificate> parent_cert;
    if (head->Header().height > 0) {
        parent_cert = context.commit_log.CertificateAt(head->Header().height);
        if (!parent_cert.has_value() || parent_cert->header_hash != head->HeaderHash()) {
            return Error{ErrorCode::INVARIANT, "the commit log holds no certificate for the head"};
        }
    }
    std::unique_ptr<PbftAgreement> pbft{new PbftAgreement{profile, config, context, std::move(seats).value(), std::move(parent_cert)}};
    pbft->StartSlot(*head);
    if (const Result<void> resumed{pbft->ResumeViewState(*head)}; !resumed.has_value()) return resumed.error();
    return Ok(std::move(pbft));
}

PbftAgreement::PbftAgreement(const ProfileParams& profile, const PbftConfig& config, const PbftContext& context, SeatSet seats,
                             std::optional<SlotCertificate> parent_cert)
    : m_profile{profile},
      m_config{config},
      m_adapter{context.adapter},
      m_owner{context.owner},
      m_signer{context.signer},
      m_signer_log{context.signer_log},
      m_commit_log{context.commit_log},
      m_safety{context.safety},
      m_sink{context.sink},
      m_votes{context.votes},
      m_evidence{context.evidence},
      m_seats{std::move(seats)},
      m_domain{context.owner.Head()->Header().domain},
      m_now_ms{config.start_ms},
      m_parent_cert{std::move(parent_cert)}
{
}

bool PbftAgreement::Fenced() const { return m_halted || m_evidence.Halted() || m_signer_log.IsFatal() || m_commit_log.IsFatal(); }

uint64_t PbftAgreement::TimeoutMs(uint64_t view) const
{
    const uint64_t factor{view >= 3 ? 8 : (uint64_t{1} << view)};
    return m_config.view_timeout_floor_ms * factor;
}

Result<uint32_t> PbftAgreement::LeaderOf(uint64_t view) const
{
    const LeaderSchedule schedule{m_seats.Size()};
    return schedule.LeaderForView(AuthorityVersion{m_slot.epoch, m_seats.Root(), ProfileHash(m_profile)}, m_slot.slot, view, SlowSet{});
}

void PbftAgreement::Tell()
{
    if (auto* rules{dynamic_cast<PbftSafetyRules*>(&m_safety)}) {
        rules->SetView(m_slot.epoch, m_slot.slot, m_slot.view, !m_slot.changing, m_slot.selected);
        rules->SetAccepted(m_slot.accepted.has_value() ? std::optional<uint256>{m_slot.accepted->header.Hash()} : std::nullopt);
    }
}

void PbftAgreement::StartSlot(const state::HeadView& head)
{
    m_slot = Slot{};
    m_slot.slot = head.Header().height + 1;
    m_slot.epoch = m_adapter.CurrentAuthority().epoch;
    m_slot.deadline_ms = m_now_ms + m_config.idle_grace_ms + TimeoutMs(0);
    m_slot.timer_running = true;
    m_slot.last_retry_ms = m_now_ms;
    for (auto it{m_claims.begin()}; it != m_claims.end();) {
        it = it->second <= m_slot.slot ? m_claims.erase(it) : std::next(it);
    }
    Tell();
}

void PbftAgreement::TightenDeadline()
{
    if (!m_slot.timer_running) return;
    const uint64_t soon{m_now_ms + TimeoutMs(m_slot.view)};
    if (soon < m_slot.deadline_ms) m_slot.deadline_ms = soon;
}

void PbftAgreement::OnLocalWork()
{
    if (Fenced()) return;
    (void)SyncHead();
    TightenDeadline();
}

void PbftAgreement::NoteClaim(uint32_t from, uint64_t slot)
{
    if (slot <= m_slot.slot) return;
    uint64_t& claim{m_claims[from]};
    claim = std::max(claim, slot);
}

std::optional<uint64_t> PbftAgreement::AheadHint() const
{
    std::vector<uint64_t> claims;
    for (const auto& [seat, slot] : m_claims) {
        if (slot > m_slot.slot) claims.push_back(slot);
    }
    const size_t need{m_seats.Size() - m_seats.Quorum() + 1}; // f + 1
    if (claims.size() < need) return std::nullopt;
    std::sort(claims.begin(), claims.end(), std::greater<>{});
    return claims[need - 1];
}

void PbftAgreement::ResetClaim(uint32_t seat) { m_claims.erase(seat); }

std::optional<Proposal> PbftAgreement::KnownProposal(const uint256& header_hash)
{
    (void)SyncHead();
    if (const auto it{m_slot.known_proposals.find(header_hash)}; it != m_slot.known_proposals.end()) return it->second;
    if (m_slot.accepted.has_value() && m_slot.accepted->header.Hash() == header_hash) return m_slot.accepted;
    return std::nullopt;
}

Result<void> PbftAgreement::ProposeSelected(const SlotHeaderV2& header, const SlotBody& body, const std::optional<SlotCertificate>& parent_cert)
{
    if (Fenced()) return Error{ErrorCode::FENCED, PBFT_FENCED};
    if (const Result<void> synced{SyncHead()}; !synced.has_value()) return synced.error();
    const uint256 hash{header.Hash()};
    if (m_slot.selected.IsNull() || m_slot.selected != hash) return Invalid(PBFT_NEW_VIEW_SELECTED);
    if (header.height != m_slot.slot) return Invalid(PBFT_CANDIDATE_SLOT);
    const std::shared_ptr<const state::HeadView> head{m_owner.Head()};
    if (header.parent_hash != head->HeaderHash()) return Error{ErrorCode::STALE_PARENT, PBFT_PARENT};
    if (parent_cert.has_value()) {
        // The selection's own parent certificate: another valid quorum over the same head is as good as this seat's.
        if (parent_cert->header_hash != head->HeaderHash()) return Error{ErrorCode::VERIFY_FAILED, PBFT_PARENT_CERT};
        if (const Result<void> ok{CheckCertificate(*parent_cert, head->Header().CommitDigest(), m_seats, PBFT_PARENT_CERT)}; !ok.has_value()) {
            return ok.error();
        }
        m_parent_cert = *parent_cert;
    }
    Proposal fetched;
    fetched.epoch = m_slot.epoch;
    fetched.view = m_slot.view;
    fetched.header = header;
    fetched.body = body;
    if (const Result<void> facts{CheckFacts(fetched)}; !facts.has_value()) return facts.error();
    // Execute-then-propose: the body must reproduce the header exactly before this seat puts its name on it.
    const Result<state::Candidate> candidate{m_owner.Reexecute(header, body)};
    if (!candidate.has_value()) return candidate.error();
    return ProposeHeaderBody(header, body, header.Authority());
}

Result<void> PbftAgreement::ResumeViewState(const state::HeadView& head)
{
    (void)head;
    // The slot's highest durable preparation of this epoch, read whatever the view. View 0 writes no VIEW_STATE, and the
    // early return below used to come first: a seat that prepared in view 0 and restarted forgot its PreparedQC, so its
    // next report carried none although it may have signed COMMIT for that header. A preparation of another epoch is
    // not this instance's (its digest names that epoch) and is never carried.
    for (const storage::PreparedQcRecord& prepared : m_commit_log.PreparedAt(m_slot.slot)) {
        if (prepared.epoch != m_slot.epoch) continue;
        if (m_slot.prepared.has_value() && prepared.view <= m_slot.prepared->view) continue;
        const Result<SlotCertificate> cert{SlotCertificate::Decode(prepared.qc)};
        if (cert.has_value()) m_slot.prepared = PreparedCert{prepared.view, *cert};
    }
    const std::optional<storage::ViewStateRecord> state{m_signer_log.ViewState()};
    if (!state.has_value() || state->slot != m_slot.slot || state->epoch != m_slot.epoch) return Ok();
    m_slot.view = state->view;
    const std::optional<std::vector<unsigned char>> new_view{m_signer_log.NewViewAccepted(state->epoch, state->slot, state->view)};
    if (state->view == 0 || new_view.has_value()) {
        m_slot.changing = false;
        if (new_view.has_value()) {
            Result<Message> decoded{DecodeMessage(*new_view, m_profile)};
            if (decoded.has_value()) {
                if (const auto* nv{std::get_if<NewView>(&*decoded)}) m_slot.selected = nv->selected;
            }
        }
        Tell();
        return Ok();
    }
    // Durable VIEW_STATE without its NEW_VIEW: the seat was CHANGING. Its report, if signed, is in the signer log as
    // the digest it signed and the signature. The report is rebuilt from the durable preparations of this slot and
    // must reproduce that digest exactly: a preparation learnt after the signature never rewrites a signed report
    // (PROTOCOL.md), and the highest one is not necessarily the one signed. Rebuilt with its statement, the retry
    // timer resends it like any report (review of 2026-11-16, R5: a restarted seat never resent its report, and the
    // report it rebuilt from the highest preparation could no longer match its signature).
    m_slot.changing = true;
    // CHANGING waits for q reports before its timer runs (EnterChanging): the slot's timer that StartSlot armed would
    // otherwise move a restarted seat to the next view after one timeout, alone.
    m_slot.timer_running = false;
    Tell();
    const storage::VoteKey key{m_domain, m_slot.epoch, m_slot.slot, m_slot.view, storage::VotePhase::VIEW_CHANGE};
    const std::optional<storage::SignedVoteRecord> signed_report{m_signer_log.SignedVote(key)};
    if (!signed_report.has_value()) {
        // The crash came between the view entry and the report's signature (B-INTENT, Sign, B-VOTE): the report is
        // signed and sent now, as EnterChanging would have (an INTENT already written names the same digest, since
        // nothing durable changed in between, and the intent is idempotent). Without a report of its own the seat
        // would wait for a quorum it never asks for (review workflow of 2026-11-17 on 17.3 item 11).
        return SignReport(m_slot.view);
    }
    ViewChange report;
    report.epoch = m_slot.epoch;
    report.slot = m_slot.slot;
    report.new_view = m_slot.view;
    report.seat = m_config.self_seat;
    report.signature = signed_report->signature;
    // The candidates: no preparation, then every durable preparation below the target.
    std::vector<std::optional<PreparedCert>> candidates{std::nullopt};
    for (const storage::PreparedQcRecord& prepared : m_commit_log.PreparedAt(m_slot.slot)) {
        if (prepared.view >= m_slot.view) continue;
        const Result<SlotCertificate> cert{SlotCertificate::Decode(prepared.qc)};
        if (cert.has_value()) candidates.emplace_back(PreparedCert{prepared.view, *cert});
    }
    bool matched{false};
    for (const std::optional<PreparedCert>& candidate : candidates) {
        report.prepared = candidate;
        if (ViewChangeDigest(m_domain, report) == signed_report->digest) {
            matched = true;
            break;
        }
    }
    // The signer log signed a digest no durable preparation reproduces: the two logs disagree about the past.
    if (!matched) return Error{ErrorCode::VERIFY_FAILED, PBFT_REPORT_RECOVERY};
    Result<std::vector<unsigned char>> bytes{EncodeMessage(Message{report}, m_profile)};
    if (!bytes.has_value()) return bytes.error();
    m_slot.own_report = std::move(bytes).value();
    m_slot.own_report_statement = OutboundVote::Recovered(*signed_report);
    m_slot.reports[m_slot.view].emplace(m_config.self_seat, report);
    return Ok();
}

Result<void> PbftAgreement::SyncHead()
{
    const std::shared_ptr<const state::HeadView> head{m_owner.Head()};
    if (head->Header().height + 1 == m_slot.slot) return Ok();
    // The head moved (a decision was applied): the next slot starts at view 0.
    if (m_decision.has_value()) return Order(PBFT_DECISION_NOT_TAKEN);
    if (head->Header().height > 0 && (!m_parent_cert.has_value() || m_parent_cert->header_hash != head->HeaderHash())) {
        const std::optional<SlotCertificate> cert{m_commit_log.CertificateAt(head->Header().height)};
        if (!cert.has_value()) return Error{ErrorCode::INVARIANT, "the commit log holds no certificate for the head"};
        m_parent_cert = *cert;
    }
    StartSlot(*head);
    return ResumeViewState(*head);
}

// ---- Status --------------------------------------------------------------------

PbftStatus PbftAgreement::Status()
{
    (void)SyncHead();
    PbftStatus status;
    status.slot = m_slot.slot;
    status.epoch = m_slot.epoch;
    status.view = m_slot.view;
    status.changing = m_slot.changing;
    status.leader = IsLeader();
    status.proposal_accepted = m_slot.accepted.has_value();
    status.prepared = m_slot.prepared.has_value();
    status.decided = m_decision.has_value();
    status.halted = m_halted || m_evidence.Halted();
    status.prepare_votes = static_cast<uint32_t>(m_slot.prepares.size());
    status.commit_votes = static_cast<uint32_t>(m_slot.commits.size());
    status.selected_missing = !m_slot.selected.IsNull() && !m_slot.known_proposals.contains(m_slot.selected);
    status.ahead_hint = AheadHint().value_or(0);
    for (const auto& [target, by_seat] : m_slot.reports) status.reports_held += static_cast<uint32_t>(by_seat.size());
    return status;
}

bool PbftAgreement::IsLeader()
{
    (void)SyncHead();
    if (m_slot.changing) return false;
    const Result<uint32_t> leader{LeaderOf(m_slot.view)};
    return leader.has_value() && *leader == m_config.self_seat;
}

std::optional<uint256> PbftAgreement::SelectedHeader()
{
    (void)SyncHead();
    if (m_slot.selected.IsNull()) return std::nullopt;
    return m_slot.selected;
}

// ---- Signing helpers ------------------------------------------------------------

Result<OutboundVote> PbftAgreement::SignVote(const storage::VoteKey& key, const uint256& digest, const storage::PendingProposal& proposal,
                                             const AuthorityVersion& authority)
{
    Result<storage::PendingIntent> intent{m_signer_log.AppendIntent(key, digest, proposal)};
    if (!intent.has_value()) return intent.error();
    // B-INTENT, authority check 2 (the INTENT stays durable, nothing is signed if the authority moved), Sign, B-VOTE.
    Result<DurableSignedVote> durable_vote{
        m_signer.SignDurably(*intent, [this, &authority]() -> Result<void> { return m_adapter.CheckAuthority(authority); })};
    if (!durable_vote.has_value()) return durable_vote.error();
    return Ok(OutboundVote::From(*durable_vote));
}

Result<OutboundVote> PbftAgreement::SignStatement(const storage::VoteKey& key, const uint256& digest)
{
    Result<storage::PendingIntent> intent{m_signer_log.AppendStatementIntent(key, digest)};
    if (!intent.has_value()) return intent.error();
    Result<DurableSignedVote> durable_vote{m_signer.SignDurably(*intent, {})};
    if (!durable_vote.has_value()) return durable_vote.error();
    return Ok(OutboundVote::From(*durable_vote));
}

Result<void> PbftAgreement::VerifySeatSignature(uint32_t seat, const uint256& digest, const storage::VoteSignature& signature,
                                                std::string_view detail) const
{
    if (seat >= m_seats.Size()) return Invalid(PBFT_VOTE_SEAT);
    const std::optional<bls::Signature> decoded{bls::Signature::Decode(signature)};
    if (!decoded.has_value() ||
        !bls::Verify(m_seats.Keys()[seat].Key(), std::span<const unsigned char>{digest.data(), digest.size()}, *decoded)) {
        return Error{ErrorCode::VERIFY_FAILED, detail};
    }
    return Ok();
}

Result<SlotCertificate> PbftAgreement::AggregateVotes(const std::map<uint32_t, Vote>& votes, const uint256& header_hash,
                                                      const uint256& digest) const
{
    std::vector<uint32_t> signers;
    std::vector<bls::Signature> shares;
    for (const auto& [seat, vote] : votes) {
        if (vote.header_hash != header_hash) continue;
        const std::optional<bls::Signature> share{bls::Signature::Decode(vote.signature)};
        if (!share.has_value()) return Error{ErrorCode::INVARIANT, PBFT_CERTIFICATE};
        signers.push_back(seat);
        shares.push_back(*share);
    }
    Result<std::vector<unsigned char>> bitmap{SlotCertificate::MakeBitmap(m_seats.Size(), signers)};
    if (!bitmap.has_value()) return bitmap.error();
    const std::optional<bls::Signature> aggregate{bls::AggregateSignatures(shares)};
    if (!aggregate.has_value()) return Error{ErrorCode::INVARIANT, PBFT_CERTIFICATE};
    SlotCertificate cert;
    cert.header_hash = header_hash;
    cert.bitmap = std::move(bitmap).value();
    cert.aggregate = aggregate->Compressed();
    if (const Result<void> verified{VerifyCertificate(cert, digest)}; !verified.has_value()) return verified.error();
    return Ok(std::move(cert));
}

Result<void> CheckCertificate(const SlotCertificate& cert, const uint256& digest, const SeatSet& seats, std::string_view detail)
{
    const Result<std::vector<uint32_t>> signers{cert.SignerIndices(seats.Size())};
    if (!signers.has_value()) return signers.error();
    if (signers->size() < seats.Quorum()) return Error{ErrorCode::VERIFY_FAILED, detail};
    std::vector<bls::VerifiedPublicKey> keys;
    for (const uint32_t seat : *signers) keys.push_back(seats.Keys()[seat]);
    const std::optional<bls::Signature> aggregate{bls::Signature::Decode(cert.aggregate)};
    if (!aggregate.has_value() || !bls::FastAggregateVerify(keys, std::span<const unsigned char>{digest.data(), digest.size()}, *aggregate)) {
        return Error{ErrorCode::VERIFY_FAILED, detail};
    }
    return Ok();
}

Result<void> PbftAgreement::VerifyCertificate(const SlotCertificate& cert, const uint256& digest) const
{
    return CheckCertificate(cert, digest, m_seats, PBFT_PREPARED_CERT);
}

Result<void> PbftAgreement::SendToAll(std::span<const unsigned char> message)
{
    for (uint32_t seat{0}; seat < m_seats.Size(); ++seat) {
        if (seat == m_config.self_seat) continue;
        const Result<void> sent{m_sink.Send(seat, message)};
        // BUSY: the retry timer resends; any other refusal is a bug in the frame.
        if (!sent.has_value() && sent.error().code != ErrorCode::BUSY) return sent.error();
    }
    return Ok();
}

Result<void> PbftAgreement::SendVoteToAll(const OutboundVote& vote, const uint256& header_hash)
{
    for (uint32_t seat{0}; seat < m_seats.Size(); ++seat) {
        if (seat == m_config.self_seat) continue;
        const Result<void> sent{m_votes.SendVote(seat, vote, header_hash)};
        if (!sent.has_value() && sent.error().code != ErrorCode::BUSY) return sent.error();
    }
    return Ok();
}

Result<void> PbftAgreement::SendStatementToAll(const OutboundVote& statement, std::span<const unsigned char> message)
{
    for (uint32_t seat{0}; seat < m_seats.Size(); ++seat) {
        if (seat == m_config.self_seat) continue;
        const Result<void> sent{m_votes.SendStatement(seat, statement, message)};
        if (!sent.has_value() && sent.error().code != ErrorCode::BUSY) return sent.error();
    }
    return Ok();
}

// ---- The leader -----------------------------------------------------------------

Result<void> PbftAgreement::Propose(const state::Candidate& candidate, const AuthorityVersion& authority)
{
    if (Fenced()) return Error{ErrorCode::FENCED, PBFT_FENCED};
    if (const Result<void> synced{SyncHead()}; !synced.has_value()) return synced.error();
    const SlotHeaderV2& header{candidate.Header()};
    if (header.height != m_slot.slot) return Invalid(PBFT_CANDIDATE_SLOT);
    if (!(header.Authority() == authority) || authority.epoch != m_slot.epoch) return Invalid(PBFT_CANDIDATE_AUTHORITY);
    if (header.seat_set_root != m_seats.Root()) return Invalid(PBFT_SEAT_SET);
    return ProposeHeaderBody(header, candidate.Body(), authority);
}

Result<void> PbftAgreement::ProposeHeaderBody(const SlotHeaderV2& header, const SlotBody& body, const AuthorityVersion& authority)
{
    if (m_slot.changing) return Invalid(PBFT_NOT_LEADER);
    const Result<uint32_t> leader{LeaderOf(m_slot.view)};
    if (!leader.has_value()) return leader.error();
    if (*leader != m_config.self_seat) return Invalid(PBFT_NOT_LEADER);
    const uint256 hash{header.Hash()};
    if (m_slot.accepted.has_value()) {
        if (m_slot.accepted->header.Hash() == hash) return Ok(); // the same proposal again: idempotent
        return Order(PBFT_PROPOSAL_PENDING);
    }
    if (m_slot.view > 0 && !m_slot.selected.IsNull() && m_slot.selected != hash) return Invalid(PBFT_NEW_VIEW_SELECTED);
    if (const std::optional<storage::DecisionRef> decided{m_commit_log.DecisionAt(header.height)};
        decided.has_value() && decided->header_hash != hash) {
        return Error{ErrorCode::EQUIVOCATION_REFUSED, PBFT_HEIGHT_DECIDED};
    }
    Proposal proposal;
    proposal.epoch = m_slot.epoch;
    proposal.view = m_slot.view;
    proposal.header = header;
    proposal.parent_cert = m_parent_cert;
    proposal.body = body;
    if (const Result<void> shape{CheckProposal(proposal)}; !shape.has_value()) return shape.error();

    // Authority check 1: before ACCEPTED_PROPOSAL; nothing written on refusal.
    if (Result<void> current{m_adapter.CheckAuthority(authority)}; !current.has_value()) return current.error();
    Result<storage::PendingProposal> pending{
        m_signer_log.AppendAcceptedProposal(storage::ProposalKey{m_slot.epoch, m_slot.slot, m_slot.view}, header, body)};
    if (!pending.has_value()) return pending.error();
    // ACCEPTED_PROPOSAL is durable before the proposal leaves (review of 2026-11-16, R4): a leader that crashed between
    // the send and its own B-INTENT came back without the proposal it had put on the wire, and the view was lost. The
    // leader's PREPARE barriers right after, so this costs nothing on the happy path.
    if (Result<uint64_t> barrier{m_signer_log.Barrier()}; !barrier.has_value()) return barrier.error();
    Result<std::vector<unsigned char>> bytes{EncodeMessage(Message{proposal}, m_profile)};
    if (!bytes.has_value()) return bytes.error();
    m_slot.accepted = proposal;
    m_slot.accepted_pending = *pending;
    m_slot.known_proposals.emplace(hash, proposal);
    m_slot.own_proposal = *bytes;
    TightenDeadline();
    Tell();
    if (const Result<void> sent{SendToAll(*bytes)}; !sent.has_value()) return sent.error();
    // The leader prepares its own proposal like a follower (it executed it already).
    return VotePrepare(proposal);
}

// ---- The follower -----------------------------------------------------------------

Result<void> PbftAgreement::CheckFacts(const Proposal& proposal)
{
    const std::shared_ptr<const state::HeadView> head{m_owner.Head()};
    const FactCursor from{head->Header().facts_to};
    if (proposal.header.facts_from != from) return Error{ErrorCode::STALE_PARENT, PBFT_PARENT};
    Result<chain::PollReply> reply{m_adapter.Poll(from, MAX_SLOT_FACTS)};
    if (!reply.has_value()) return reply.error();
    switch (reply->Status()) {
    case chain::PollStatus::BUSY: return Error{ErrorCode::BUSY, "the chain adapter answered BUSY"};
    case chain::PollStatus::NEED_DATA: return Error{ErrorCode::NEED_DATA, "the chain adapter has no data at the cursor"};
    case chain::PollStatus::AUTHORITY_CHANGED: return Error{ErrorCode::AUTHORITY_CHANGED, chain::CHAIN_AUTHORITY_CHANGED};
    case chain::PollStatus::FACTS: break;
    }
    const chain::FactBatch& batch{*reply->Batch()};
    if (batch.to < proposal.header.facts_to) return Error{ErrorCode::NEED_DATA, "this seat's chain is behind the proposal's facts"};
    // Exactly the facts in [facts_from, facts_to), in order, byte for byte.
    size_t i{0};
    for (const Fact& fact : batch.facts) {
        if (FactCursorOf(fact) >= proposal.header.facts_to) break;
        if (i >= proposal.body.facts.size()) return Error{ErrorCode::VERIFY_FAILED, PBFT_FACTS_MISMATCH};
        const Result<std::vector<unsigned char>> bytes{EncodeFact(fact)};
        if (!bytes.has_value()) return bytes.error();
        if (*bytes != proposal.body.facts[i]) return Error{ErrorCode::VERIFY_FAILED, PBFT_FACTS_MISMATCH};
        ++i;
    }
    if (i != proposal.body.facts.size()) return Error{ErrorCode::VERIFY_FAILED, PBFT_FACTS_MISMATCH};
    return Ok();
}

Result<void> PbftAgreement::OnProposal(uint32_t from, Proposal proposal)
{
    if (const Result<void> shape{CheckProposal(proposal)}; !shape.has_value()) return shape.error();
    if (proposal.header.height != m_slot.slot) return Ok(); // another slot: stale or ahead of us; catch-up is not this path
    if (proposal.epoch != m_slot.epoch) return Invalid(PBFT_EPOCH);
    const Result<uint32_t> leader{LeaderOf(proposal.view)};
    if (!leader.has_value()) return leader.error();
    if (*leader != from) return Invalid(PBFT_WRONG_LEADER);
    if (proposal.view < m_slot.view) return Ok(); // an abandoned view
    if (proposal.view > m_slot.view || m_slot.changing) {
        // Its NEW_VIEW (or the quorum that leads there) has not arrived yet: one proposal waits.
        TightenDeadline();
        m_slot.pending_proposal = std::pair{from, std::move(proposal)};
        return Ok();
    }
    const uint256 hash{proposal.header.Hash()};
    if (m_slot.accepted.has_value()) {
        if (m_slot.accepted->header.Hash() == hash) return Ok(); // a retransmission
        return Error{ErrorCode::EQUIVOCATION_REFUSED, PBFT_DOUBLE_VOTE};
    }
    const std::shared_ptr<const state::HeadView> head{m_owner.Head()};
    if (proposal.header.parent_hash != head->HeaderHash()) return Error{ErrorCode::STALE_PARENT, PBFT_PARENT};
    // The proposer's parent certificate: not necessarily this seat's (O-4b), but it must certify the head (O-98).
    if (head->Header().height > 0) {
        if (!proposal.parent_cert.has_value()) return Error{ErrorCode::STALE_PARENT, PBFT_PARENT};
        if (const Result<void> ok{VerifyCertificate(*proposal.parent_cert, head->Header().CommitDigest())}; !ok.has_value()) return ok.error();
    }
    if (const Result<void> anchor{m_adapter.VerifyAnchor(proposal.header.anchor, head->Header().anchor)}; !anchor.has_value()) {
        return anchor.error();
    }
    if (const Result<void> facts{CheckFacts(proposal)}; !facts.has_value()) return facts.error();
    if (const Result<void> may{m_safety.MayPrepare(proposal)}; !may.has_value()) return may.error();
    // Execute-then-vote: the header and the canonical body must be reproduced exactly.
    const Result<state::Candidate> candidate{m_owner.Reexecute(proposal.header, proposal.body)};
    if (!candidate.has_value()) return candidate.error();
    // Authority check 1 for the follower too, then ACCEPTED_PROPOSAL.
    const AuthorityVersion authority{proposal.header.Authority()};
    if (Result<void> current{m_adapter.CheckAuthority(authority)}; !current.has_value()) return current.error();
    Result<storage::PendingProposal> pending{
        m_signer_log.AppendAcceptedProposal(storage::ProposalKey{m_slot.epoch, m_slot.slot, m_slot.view}, proposal.header, proposal.body)};
    if (!pending.has_value()) return pending.error();
    m_slot.accepted = proposal;
    m_slot.accepted_pending = *pending;
    m_slot.known_proposals.emplace(hash, proposal);
    TightenDeadline();
    Tell();
    if (const Result<void> voted{VotePrepare(proposal)}; !voted.has_value()) return voted.error();
    return ReplayEarlyVotes();
}

Result<void> PbftAgreement::VotePrepare(const Proposal& proposal)
{
    if (m_slot.own_prepare.has_value()) return Ok();
    const uint256 hash{proposal.header.Hash()};
    const storage::VoteKey key{m_domain, m_slot.epoch, m_slot.slot, m_slot.view, storage::VotePhase::PREPARE};
    Result<OutboundVote> vote{SignVote(key, PrepareDigest(m_domain, m_slot.epoch, m_slot.slot, m_slot.view, hash), *m_slot.accepted_pending, proposal.header.Authority())};
    if (!vote.has_value()) return vote.error();
    m_slot.own_prepare = *vote;
    Vote own;
    own.phase = storage::VotePhase::PREPARE;
    own.epoch = m_slot.epoch;
    own.view = m_slot.view;
    own.slot = m_slot.slot;
    own.header_hash = hash;
    own.seat = m_config.self_seat;
    own.signature = vote->Signature();
    m_slot.prepares[m_config.self_seat] = own;
    if (const Result<void> sent{SendVoteToAll(*vote, hash)}; !sent.has_value()) return sent.error();
    return CountPrepare(own);
}

Result<void> PbftAgreement::OnVote(uint32_t from, const Vote& vote)
{
    if (const Result<void> shape{CheckVote(vote)}; !shape.has_value()) return shape.error();
    if (vote.slot != m_slot.slot) return Ok(); // another slot
    if (vote.epoch != m_slot.epoch) return Invalid(PBFT_EPOCH);
    if (vote.seat != from) return Invalid(PBFT_VOTE_SEAT);
    if (!m_slot.accepted.has_value()) {
        // We cannot check a vote against a header we do not hold yet: it waits for the proposal.
        if (m_slot.early_votes.size() < MAX_EARLY_VOTES) m_slot.early_votes.push_back(vote);
        return Ok();
    }
    const uint256 accepted{m_slot.accepted->header.Hash()};
    if (vote.phase == storage::VotePhase::PREPARE) {
        if (vote.view != m_slot.view) return Ok(); // another view's preparation
        // A vote already counted costs no BLS work (review of 2026-11-16, R6); a conflicting one is verified before it is
        // called a double vote (evidence names two signatures of the seat).
        const auto known{m_slot.prepares.find(vote.seat)};
        if (known != m_slot.prepares.end() && known->second.header_hash == vote.header_hash) return Ok(); // a retransmission
        if (const Result<void> ok{VerifySeatSignature(vote.seat, PrepareDigest(m_domain, m_slot.epoch, m_slot.slot, vote.view, vote.header_hash), vote.signature,
                                                     PBFT_VOTE_SIGNATURE)};
            !ok.has_value()) {
            return ok.error();
        }
        if (known != m_slot.prepares.end()) return OnDoubleVote(known->second, vote);
        if (vote.header_hash != accepted) return Ok(); // a valid vote for a header we did not accept: not ours to count
        m_slot.prepares.emplace(vote.seat, vote);
        return CountPrepare(vote);
    }
    if (vote.phase != storage::VotePhase::COMMIT) return Invalid(storage::VOTE_PHASE);
    if (vote.header_hash != accepted) return Ok();
    if (m_slot.commits.contains(vote.seat)) return Ok(); // a retransmission (commits hold the accepted header only): no BLS work (R6)
    if (const Result<void> ok{VerifySeatSignature(vote.seat, m_slot.accepted->header.CommitDigest(), vote.signature, PBFT_VOTE_SIGNATURE)};
        !ok.has_value()) {
        return ok.error();
    }
    m_slot.commits.emplace(vote.seat, vote);
    return CountCommit(vote);
}

Result<void> PbftAgreement::ReplayEarlyVotes()
{
    std::vector<Vote> early{std::move(m_slot.early_votes)};
    m_slot.early_votes.clear();
    for (const Vote& vote : early) {
        // A bad early vote is dropped, not an error of the proposal that just arrived.
        (void)OnVote(vote.seat, vote);
        if (m_decision.has_value()) break;
    }
    return Ok();
}

Result<void> PbftAgreement::CountPrepare(const Vote& /*vote*/)
{
    // A preparation carried from an earlier view does not count: the COMMIT needs a PreparedQC of this view.
    const bool prepared_here{m_slot.prepared.has_value() && m_slot.prepared->view == m_slot.view &&
                             m_slot.accepted.has_value() && m_slot.prepared->cert.header_hash == m_slot.accepted->header.Hash()};
    if (prepared_here || m_decision.has_value() || !m_slot.accepted.has_value()) return Ok();
    size_t count{0};
    const uint256 hash{m_slot.accepted->header.Hash()};
    for (const auto& [seat, vote] : m_slot.prepares) {
        if (vote.header_hash == hash && vote.view == m_slot.view) ++count;
    }
    if (count < m_seats.Quorum()) return Ok();
    Result<SlotCertificate> cert{AggregateVotes(m_slot.prepares, hash, PrepareDigest(m_domain, m_slot.epoch, m_slot.slot, m_slot.view, hash))};
    if (!cert.has_value()) return cert.error();
    const PreparedCert prepared{m_slot.view, *cert};
    // The proof is durable before the COMMIT intent (PROTOCOL.md).
    if (Result<uint64_t> seq{m_commit_log.AppendPreparedQc(storage::PreparedQcRecord{m_slot.epoch, m_slot.slot, m_slot.view, hash, cert->Encode()})};
        !seq.has_value()) {
        return seq.error();
    }
    if (Result<uint64_t> barrier{m_commit_log.Barrier()}; !barrier.has_value()) return barrier.error();
    m_slot.prepared = prepared;
    return VoteCommit();
}

Result<void> PbftAgreement::VoteCommit()
{
    if (m_slot.commit_voted || !m_slot.accepted.has_value() || !m_slot.prepared.has_value() || m_slot.prepared->view != m_slot.view) return Ok();
    const Proposal& proposal{*m_slot.accepted};
    if (const Result<void> may{m_safety.MayCommit(proposal, *m_slot.prepared)}; !may.has_value()) return may.error();
    const uint256 hash{proposal.header.Hash()};
    const storage::VoteKey key{m_domain, m_slot.epoch, m_slot.slot, m_slot.view, storage::VotePhase::COMMIT};
    Result<OutboundVote> vote{SignVote(key, proposal.header.CommitDigest(), *m_slot.accepted_pending, proposal.header.Authority())};
    if (!vote.has_value()) return vote.error();
    m_slot.commit_voted = true;
    m_slot.own_commit = *vote;
    Vote own;
    own.phase = storage::VotePhase::COMMIT;
    own.epoch = m_slot.epoch;
    own.view = m_slot.view;
    own.slot = m_slot.slot;
    own.header_hash = hash;
    own.seat = m_config.self_seat;
    own.signature = vote->Signature();
    m_slot.commits[m_config.self_seat] = own;
    if (const Result<void> sent{SendVoteToAll(*vote, hash)}; !sent.has_value()) return sent.error();
    return CountCommit(own);
}

Result<void> PbftAgreement::CountCommit(const Vote& /*vote*/)
{
    if (m_decision.has_value() || !m_slot.accepted.has_value()) return Ok();
    const uint256 hash{m_slot.accepted->header.Hash()};
    size_t count{0};
    for (const auto& [seat, vote] : m_slot.commits) {
        if (vote.header_hash == hash) ++count;
    }
    if (count < m_seats.Quorum()) return Ok();
    return Decide();
}

Result<void> PbftAgreement::Decide()
{
    const Proposal& proposal{*m_slot.accepted};
    const uint256 hash{proposal.header.Hash()};
    Result<SlotCertificate> cert{AggregateVotes(m_slot.commits, hash, proposal.header.CommitDigest())};
    if (!cert.has_value()) return cert.error();
    Result<storage::PendingDecision> decision{m_commit_log.AppendDecision(proposal.header, *cert, proposal.body)};
    if (!decision.has_value()) return decision.error();
    if (Result<uint64_t> barrier{m_commit_log.Barrier()}; !barrier.has_value()) return barrier.error();
    Result<DurableDecision> durable{m_commit_log.Confirm(*decision)};
    if (!durable.has_value()) return durable.error();
    m_parent_cert = *cert;
    m_decision.emplace(Decision{std::move(durable).value(), std::move(cert).value(), proposal.body, proposal.header});
    m_slot.timer_running = false;
    return Ok();
}

// ---- Evidence (M4-AGR-07) ------------------------------------------------------------

Result<void> PbftAgreement::OnDoubleVote(const Vote& first, const Vote& second)
{
    // Both votes verified against the seat's key; one encoding per conflict (ascending header hash).
    const bool ordered{first.header_hash < second.header_hash};
    const Evidence evidence{DoubleVote{ordered ? first : second, ordered ? second : first}};
    if (const Result<void> recorded{m_evidence.Record(evidence)}; !recorded.has_value()) return recorded.error();
    m_halted = true;
    Result<std::vector<unsigned char>> bytes{EncodeMessage(Message{evidence}, m_profile)};
    if (bytes.has_value()) (void)SendToAll(*bytes); // best effort: the evidence is durable here whatever the mesh says
    return Error{ErrorCode::EQUIVOCATION_REFUSED, PBFT_DOUBLE_VOTE};
}

Result<void> PbftAgreement::OnEvidence(const Evidence& evidence)
{
    if (const Result<void> recorded{m_evidence.Record(evidence)}; !recorded.has_value()) return recorded.error();
    m_halted = true;
    return Ok();
}

// ---- Messages and time -------------------------------------------------------------

Result<void> PbftAgreement::OnMessage(uint32_t from_seat, std::span<const unsigned char> bytes)
{
    if (Fenced()) return Error{ErrorCode::FENCED, PBFT_FENCED};
    if (const Result<void> synced{SyncHead()}; !synced.has_value()) return synced.error();
    if (from_seat >= m_seats.Size() || from_seat == m_config.self_seat) return Invalid(PBFT_VOTE_SEAT);
    Result<Message> decoded{DecodeMessage(bytes, m_profile)};
    if (!decoded.has_value()) return decoded.error();
    // The slot the peer is working on, for the catch-up hint (O-101); never trusted further.
    std::visit(
        [&](const auto& message) {
            using T = std::decay_t<decltype(message)>;
            if constexpr (std::is_same_v<T, Proposal>) {
                m_last_message_slot = message.header.height;
                NoteClaim(from_seat, message.header.height);
            } else if constexpr (std::is_same_v<T, Vote> || std::is_same_v<T, ViewChange> || std::is_same_v<T, NewView>) {
                m_last_message_slot = message.slot;
                NoteClaim(from_seat, message.slot);
            } else {
                m_last_message_slot.reset();
            }
        },
        *decoded);
    return std::visit(
        [&](auto&& message) -> Result<void> {
            using T = std::decay_t<decltype(message)>;
            if constexpr (std::is_same_v<T, Proposal>) {
                return OnProposal(from_seat, std::move(message));
            } else if constexpr (std::is_same_v<T, Vote>) {
                return OnVote(from_seat, message);
            } else if constexpr (std::is_same_v<T, ViewChange>) {
                return OnViewChangeReport(from_seat, message);
            } else if constexpr (std::is_same_v<T, NewView>) {
                return OnNewView(from_seat, std::move(message), bytes);
            } else {
                return OnEvidence(message);
            }
        },
        std::move(*decoded));
}

Result<void> PbftAgreement::OnTimeout(uint64_t now_ms)
{
    if (now_ms < m_now_ms) return Invalid("time moved backwards");
    m_now_ms = now_ms;
    if (Fenced()) return Error{ErrorCode::FENCED, PBFT_FENCED};
    if (const Result<void> synced{SyncHead()}; !synced.has_value()) return synced.error();
    if (m_decision.has_value()) return Ok();
    if (m_slot.timer_running && now_ms >= m_slot.deadline_ms) {
        // The view made no progress: move on. CHANGING waits for a quorum of reports before its timer runs.
        return EnterChanging(m_slot.view + 1);
    }
    if (now_ms - m_slot.last_retry_ms >= m_config.retry_ms) {
        m_slot.last_retry_ms = now_ms;
        return Retransmit();
    }
    return Ok();
}

Result<void> PbftAgreement::Retransmit()
{
    if (m_slot.changing) {
        if (m_slot.own_report.has_value() && m_slot.own_report_statement.has_value()) {
            return SendStatementToAll(*m_slot.own_report_statement, *m_slot.own_report);
        }
        return Ok();
    }
    if (m_slot.own_new_view.has_value() && m_slot.own_new_view_statement.has_value()) {
        if (const Result<void> sent{SendStatementToAll(*m_slot.own_new_view_statement, *m_slot.own_new_view)}; !sent.has_value()) return sent.error();
    }
    if (m_slot.own_proposal.has_value()) {
        if (const Result<void> sent{SendToAll(*m_slot.own_proposal)}; !sent.has_value()) return sent.error();
    }
    if (m_slot.accepted.has_value()) {
        const uint256 hash{m_slot.accepted->header.Hash()};
        if (m_slot.own_prepare.has_value()) {
            if (const Result<void> sent{SendVoteToAll(*m_slot.own_prepare, hash)}; !sent.has_value()) return sent.error();
        }
        if (m_slot.own_commit.has_value()) {
            if (const Result<void> sent{SendVoteToAll(*m_slot.own_commit, hash)}; !sent.has_value()) return sent.error();
        }
    }
    return Ok();
}

std::optional<Decision> PbftAgreement::TakeDecision()
{
    std::optional<Decision> taken;
    if (m_decision.has_value()) {
        taken.emplace(std::move(*m_decision));
        m_decision.reset();
    }
    return taken;
}

// ---- View change ------------------------------------------------------------------

Result<void> PbftAgreement::EnterChanging(uint64_t target)
{
    if (target <= m_slot.view && m_slot.changing) return Ok();
    if (target < m_slot.view) return Ok();
    // VIEW_STATE is durable before the report's intent (PROTOCOL.md: view entry and report intent commit together).
    if (Result<uint64_t> seq{m_signer_log.AppendViewState(storage::ViewStateRecord{m_slot.epoch, m_slot.slot, target})}; !seq.has_value()) {
        return seq.error();
    }
    if (Result<uint64_t> barrier{m_signer_log.Barrier()}; !barrier.has_value()) return barrier.error();
    m_slot.view = target;
    m_slot.changing = true;
    std::erase_if(m_slot.reports, [target](const auto& by_target) { return by_target.first < target; }); // old targets (R6)
    m_slot.selected.SetNull();
    m_slot.accepted.reset();
    m_slot.accepted_pending.reset();
    m_slot.prepares.clear();
    m_slot.commits.clear();
    m_slot.own_prepare.reset();
    m_slot.own_commit.reset();
    m_slot.commit_voted = false;
    m_slot.own_proposal.reset();
    m_slot.own_new_view.reset();
    m_slot.own_new_view_statement.reset();
    m_slot.new_view_built = false;
    m_slot.timer_running = false; // runs again once q reports for the target are in
    Tell();
    return SignReport(target);
}

Result<void> PbftAgreement::SignReport(uint64_t target)
{
    // The report: the highest durable preparation below the target.
    ViewChange report;
    report.epoch = m_slot.epoch;
    report.slot = m_slot.slot;
    report.new_view = target;
    report.seat = m_config.self_seat;
    if (m_slot.prepared.has_value() && m_slot.prepared->view < target) report.prepared = m_slot.prepared;
    const storage::VoteKey key{m_domain, m_slot.epoch, m_slot.slot, target, storage::VotePhase::VIEW_CHANGE};
    Result<OutboundVote> statement{SignStatement(key, ViewChangeDigest(m_domain, report))};
    if (!statement.has_value()) return statement.error();
    report.signature = statement->Signature();
    Result<std::vector<unsigned char>> bytes{EncodeMessage(Message{report}, m_profile)};
    if (!bytes.has_value()) return bytes.error();
    m_slot.own_report = *bytes;
    m_slot.own_report_statement = *statement;
    m_slot.reports[target][m_config.self_seat] = report;
    if (const Result<void> sent{SendStatementToAll(*statement, *bytes)}; !sent.has_value()) return sent.error();
    // Reports that arrived before we moved may already make a quorum.
    return OnViewChangeReport(m_config.self_seat, report);
}

Result<void> PbftAgreement::OnViewChangeReport(uint32_t from, const ViewChange& report)
{
    if (const Result<void> may{m_safety.OnViewChange(report)}; !may.has_value()) return may.error();
    if (report.seat != from) return Invalid(PBFT_VOTE_SEAT);
    if (report.new_view < m_slot.view) return Ok(); // an old target
    if (from != m_config.self_seat) {
        // Bounded storage, and no BLS work for a report already held (review of 2026-11-16, R6): one report per seat per
        // target, and above the current view one report per seat, its highest target (a seat that moved on supersedes its
        // lower reports, as PBFT view changes do), so at most 2k - 1 reports are ever kept whatever targets a seat names.
        if (const auto target{m_slot.reports.find(report.new_view)}; target != m_slot.reports.end() && target->second.contains(report.seat)) {
            return Ok(); // one report per seat per target
        }
        if (report.new_view > m_slot.view) {
            for (const auto& [target, by_seat] : m_slot.reports) {
                if (target > report.new_view && by_seat.contains(report.seat)) return Ok(); // the seat reported a higher target already
            }
        }
        if (const Result<void> ok{VerifySeatSignature(report.seat, ViewChangeDigest(m_domain, report), report.signature, PBFT_REPORT_SIGNATURE)};
            !ok.has_value()) {
            return ok.error();
        }
        if (report.prepared.has_value()) {
            if (const Result<void> ok{VerifyCertificate(report.prepared->cert, PrepareDigest(m_domain, m_slot.epoch, m_slot.slot, report.prepared->view,
                                                                                                       report.prepared->cert.header_hash))};
                !ok.has_value()) {
                return ok.error();
            }
            // Never downgrade: a higher preparation we learn of is kept (and written) for our own reports.
            if (!m_slot.prepared.has_value() || report.prepared->view > m_slot.prepared->view) {
                if (Result<uint64_t> seq{m_commit_log.AppendPreparedQc(storage::PreparedQcRecord{
                        m_slot.epoch, m_slot.slot, report.prepared->view, report.prepared->cert.header_hash, report.prepared->cert.Encode()})};
                    !seq.has_value()) {
                    return seq.error();
                }
                if (Result<uint64_t> barrier{m_commit_log.Barrier()}; !barrier.has_value()) return barrier.error();
                m_slot.prepared = report.prepared;
            }
        }
        if (report.new_view > m_slot.view) {
            // The seat's lower reports above the current view are superseded.
            for (auto it{m_slot.reports.begin()}; it != m_slot.reports.end();) {
                if (it->first > m_slot.view && it->first < report.new_view) {
                    it->second.erase(report.seat);
                    if (it->second.empty()) {
                        it = m_slot.reports.erase(it);
                        continue;
                    }
                }
                ++it;
            }
        }
        m_slot.reports[report.new_view].emplace(report.seat, report);
    }
    const auto& reports{m_slot.reports[report.new_view]};
    const size_t quorum{m_seats.Quorum()};
    const size_t sync{m_seats.Size() - quorum + 1}; // f + 1
    // f + 1 reports for a higher target synchronise us into it.
    if (report.new_view > m_slot.view && reports.size() >= sync) {
        return EnterChanging(report.new_view);
    }
    if (report.new_view == m_slot.view && m_slot.changing && reports.size() >= quorum) {
        if (!m_slot.timer_running) {
            m_slot.timer_running = true;
            m_slot.deadline_ms = m_now_ms + TimeoutMs(m_slot.view);
        }
        const Result<uint32_t> leader{LeaderOf(m_slot.view)};
        if (!leader.has_value()) return leader.error();
        if (*leader == m_config.self_seat && !m_slot.new_view_built) return BuildNewView(m_slot.view);
    }
    return Ok();
}

Result<void> PbftAgreement::BuildNewView(uint64_t target)
{
    const auto& reports{m_slot.reports[target]};
    NewView message;
    message.epoch = m_slot.epoch;
    message.slot = m_slot.slot;
    message.view = target;
    message.seat = m_config.self_seat;
    std::optional<PreparedCert> highest;
    for (const auto& [seat, report] : reports) {
        message.reports.push_back(report);
        if (report.prepared.has_value() && (!highest.has_value() || report.prepared->view > highest->view)) highest = report.prepared;
    }
    if (highest.has_value()) message.selected = highest->cert.header_hash;
    if (const Result<void> shape{CheckNewView(message)}; !shape.has_value()) return shape.error();
    const storage::VoteKey key{m_domain, m_slot.epoch, m_slot.slot, target, storage::VotePhase::NEW_VIEW};
    Result<OutboundVote> statement{SignStatement(key, NewViewDigest(m_domain, message))};
    if (!statement.has_value()) return statement.error();
    message.signature = statement->Signature();
    Result<std::vector<unsigned char>> bytes{EncodeMessage(Message{message}, m_profile)};
    if (!bytes.has_value()) return bytes.error();
    m_slot.new_view_built = true;
    if (const Result<void> accepted{AcceptNewView(message, *bytes)}; !accepted.has_value()) return accepted.error();
    m_slot.own_new_view = *bytes;
    m_slot.own_new_view_statement = *statement;
    if (const Result<void> sent{SendStatementToAll(*statement, *bytes)}; !sent.has_value()) return sent.error();
    // Re-propose the selected header if this seat holds its body (with the certificate that header binds, verified when the
    // proposal was accepted); otherwise the caller fetches and proposes it (ProposeSelected).
    if (!message.selected.IsNull()) {
        if (const auto it{m_slot.known_proposals.find(message.selected)}; it != m_slot.known_proposals.end()) {
            const Proposal known{it->second};
            if (known.parent_cert.has_value()) m_parent_cert = *known.parent_cert;
            return ProposeHeaderBody(known.header, known.body, known.header.Authority());
        }
    }
    return Ok();
}

Result<void> PbftAgreement::OnNewView(uint32_t from, NewView message, std::span<const unsigned char> bytes)
{
    if (const Result<void> shape{CheckNewView(message)}; !shape.has_value()) return shape.error();
    if (message.slot != m_slot.slot) return Ok();
    if (message.epoch != m_slot.epoch) return Invalid(PBFT_EPOCH);
    if (message.seat != from) return Invalid(PBFT_VOTE_SEAT);
    const Result<uint32_t> leader{LeaderOf(message.view)};
    if (!leader.has_value()) return leader.error();
    if (*leader != from) return Invalid(PBFT_WRONG_LEADER);
    if (message.view < m_slot.view) return Ok();
    if (message.view == m_slot.view && !m_slot.changing) return Ok(); // already in it
    if (message.reports.size() < m_seats.Quorum()) return Invalid(PBFT_NEW_VIEW_QUORUM);
    if (const Result<void> ok{VerifySeatSignature(message.seat, NewViewDigest(m_domain, message), message.signature, PBFT_REPORT_SIGNATURE)};
        !ok.has_value()) {
        return ok.error();
    }
    for (const ViewChange& report : message.reports) {
        if (const Result<void> ok{VerifySeatSignature(report.seat, ViewChangeDigest(m_domain, report), report.signature, PBFT_REPORT_SIGNATURE)};
            !ok.has_value()) {
            return ok.error();
        }
        if (report.prepared.has_value()) {
            if (const Result<void> ok{VerifyCertificate(report.prepared->cert, PrepareDigest(m_domain, m_slot.epoch, m_slot.slot, report.prepared->view,
                                                                                                       report.prepared->cert.header_hash))};
                !ok.has_value()) {
                return ok.error();
            }
        }
    }
    return AcceptNewView(message, bytes);
}

Result<void> PbftAgreement::AcceptNewView(const NewView& message, std::span<const unsigned char> bytes)
{
    if (Result<uint64_t> seq{m_signer_log.AppendViewState(storage::ViewStateRecord{m_slot.epoch, m_slot.slot, message.view})}; !seq.has_value()) {
        return seq.error();
    }
    if (Result<uint64_t> seq{m_signer_log.AppendNewViewAccepted(
            storage::NewViewAcceptedRecord{m_slot.epoch, m_slot.slot, message.view, std::vector<unsigned char>{bytes.begin(), bytes.end()}})};
        !seq.has_value()) {
        return seq.error();
    }
    if (Result<uint64_t> barrier{m_signer_log.Barrier()}; !barrier.has_value()) return barrier.error();
    m_slot.view = message.view;
    m_slot.changing = false;
    m_slot.selected = message.selected;
    m_slot.accepted.reset();
    m_slot.accepted_pending.reset();
    m_slot.prepares.clear();
    m_slot.commits.clear();
    m_slot.own_prepare.reset();
    m_slot.own_commit.reset();
    m_slot.commit_voted = false;
    m_slot.own_proposal.reset();
    m_slot.deadline_ms = m_now_ms + TimeoutMs(m_slot.view);
    m_slot.timer_running = true;
    Tell();
    // A proposal of this view that arrived early is handled now.
    if (m_slot.pending_proposal.has_value() && m_slot.pending_proposal->second.view == m_slot.view) {
        std::pair<uint32_t, Proposal> pending{std::move(*m_slot.pending_proposal)};
        m_slot.pending_proposal.reset();
        return OnProposal(pending.first, std::move(pending.second));
    }
    return Ok();
}

} // namespace flowmesh2::agreement
