// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <flowmesh2/agreement/regtest_pbft.h>
#include <flowmesh2/agreement/control_messages.h>
#include <flowmesh2/agreement/control_outbound.h>
#include <flowmesh2/agreement/outbound.h>

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <optional>
#include <utility>

namespace flowmesh2::agreement {
namespace {
// TODO(rebirth:M4-AGR-13) regtest-only PBFT for 4..7 seats: quorum, bitmap,
// leader rule and report counts follow the seat set (O-98); the general
// agreement of M4-AGR-02 replaces it.
constexpr size_t MAX_BODIES{16};
constexpr size_t MAX_BODY_CACHE_BYTES{16 * 1024 * 1024};
constexpr size_t MAX_PEER_OUTBOX{16};
constexpr size_t MAX_PEER_OUTBOX_BYTES{16 * 1024 * 1024};
constexpr uint64_t VIEW_WINDOW{8};
constexpr uint64_t RETRY_MS{10};
constexpr size_t MAX_VERIFIED_CERTIFICATES{64};
constexpr size_t MAX_VERIFIED_VOTES{256};
Error Refuse(ErrorCode code, std::string_view detail) { return Error{code, detail}; }

Result<void> VerifyCert(const SlotCertificate& cert, const SlotHeaderV2& header, const SeatSet& seats, const uint256& digest)
{
    if (cert.header_hash != header.Hash()) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_CERT_HEADER");
    if (const auto format{CertificateDigest(header, cert)}; !format.has_value()) return format.error();
    auto indices{cert.SignerIndices(seats.Size())};
    if (!indices.has_value()) return indices.error();
    if (indices->size() < seats.Quorum()) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_CERT_QUORUM");
    std::vector<bls::VerifiedPublicKey> keys;
    for (auto seat : *indices) keys.push_back(seats.Keys()[seat]);
    const auto signature{bls::Signature::Decode(cert.aggregate)};
    if (!signature || !bls::FastAggregateVerify(keys, std::span{digest.data(), digest.size()}, *signature)) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_CERT_SIGNATURE");
    return Ok();
}

class FullViewAgreement final : public Agreement
{
public:
    FullViewAgreement(ProfileParams profile, PbftConfig config, PbftContext context, SeatSet seats)
        : m_profile{std::move(profile)}, m_config{std::move(config)}, m_context{context}, m_seats{std::move(seats)},
          m_control{*context.control_log}, m_parent_certificate{m_config.parent_certificate}, m_outbox(m_seats.Size()) {}

    Result<void> Initialize()
    {
        if (auto ready{Ready()}; !ready.has_value()) return ready.error();
        const auto head{m_context.owner.Head()};
        if (head->Header().height == UINT64_MAX) return Refuse(ErrorCode::FENCED, "PBFT_SEQUENCE_BOUND");
        m_slot = head->Header().height + 1;
        if (const auto restored{m_control.ViewAt(m_slot)}) {
            if (restored->parent_hash != head->Header().Hash()) return Refuse(ErrorCode::FENCED, "PBFT_CONTROL_PARENT");
            m_view = restored->view;
            m_changing = restored->changing;
            if (restored->accepted) {
                auto decoded{DecodeMessage(restored->accepted->message, m_profile)};
                if (!decoded.has_value()) return decoded.error();
                const auto* view{std::get_if<ControlNewView>(&*decoded)};
                if (!view || view->key != restored->accepted->key || view->key.view != m_view) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_RESTORED_NEW_VIEW_BINDING");
                if (auto proof{VerifyControlNewView(*view, head->Header(), m_seats, m_profile)}; !proof.has_value()) return proof.error();
                const auto own_entry{m_control.IntentAt(Key(m_view, storage::ControlPhase::VIEW_CHANGE))};
                if (!own_entry) return Refuse(ErrorCode::FENCED, "PBFT_RESTORED_ACTIVE_WITHOUT_REPORT");
                if (auto truthful{RestoreReport(*own_entry)}; !truthful.has_value()) return truthful.error();
                m_new_view = *view;
                if (!view->selected_proposal) return Refuse(ErrorCode::FENCED, "PBFT_RESTORED_SELECTED_BODY");
                if (auto cached{Cache(*view->selected_proposal)}; !cached.has_value()) return cached.error();
                m_offer = view->selected;
                m_have_work = true;
            }
        }
        // Recover the exact accepted body, not a replacement instruction.
        auto accepted{m_context.signer_log.AcceptedAt({0, m_slot, m_view}, m_profile)};
        if (!accepted.has_value()) return accepted.error();
        if (*accepted) {
            if (m_view && (m_changing || !m_new_view || m_new_view->selected != (*accepted)->header.Hash())) return Refuse(ErrorCode::FENCED, "PBFT_RESTORED_ACCEPTANCE_NOT_SELECTED");
            Proposal proposal{0, m_view, (*accepted)->header, m_parent_certificate, (*accepted)->body};
            if (auto body{Cache(proposal)}; !body.has_value()) return body.error();
            auto [it, inserted]{m_states.emplace(m_view, ViewState{proposal.header.Hash(), {}, {}})};
            it->second.accepted = true;
            m_offer = proposal.header.Hash();
            m_have_work = true;
        }
        if (m_changing) {
            const auto key{Key(m_view, storage::ControlPhase::VIEW_CHANGE)};
            if (const auto record{m_control.IntentAt(key)}) {
                auto report{RestoreReport(*record)};
                if (!report.has_value()) return report.error();
                m_own_report = *report;
                m_have_work = true;
            } else return Refuse(ErrorCode::FENCED, "PBFT_RESTORED_CHANGING_WITHOUT_REPORT");
            // A leader can crash after its complete immutable NEW_VIEW
            // intent but before signing or accepting that bundle. Its
            // selected body must survive without a client resubmission.
            if (const auto pending{m_control.IntentAt(Key(m_view, storage::ControlPhase::NEW_VIEW))}) {
                auto decoded{DecodeMessage(pending->message, m_profile)};
                if (!decoded.has_value()) return decoded.error();
                const auto* view{std::get_if<ControlNewView>(&*decoded)};
                if (!view || !view->selected_proposal || view->key != pending->key) return Refuse(ErrorCode::FENCED, "PBFT_RESTORED_NEW_VIEW_INTENT");
                if (auto proof{VerifyControlNewViewEvidence(*view, head->Header(), m_seats, m_profile)}; !proof.has_value()) return proof.error();
                auto authenticated{*view};
                // Authenticate the complete proof, including the leader
                // signature when already issued. An unsigned intent cannot
                // grant ACTIVE or ordinary voting permission.
                if (const auto signed_record{m_control.SignedAt(pending->key)}) {
                    auto durable{m_control.Confirm(*signed_record)};
                    if (!durable.has_value()) return durable.error();
                    authenticated.signature = durable->Binding().signature;
                    if (auto valid{VerifyControlNewView(authenticated, head->Header(), m_seats, m_profile)}; !valid.has_value()) return valid.error();
                }
                for (const auto& report : view->reports) {
                    if (auto valid{VerifyControlReport(report, head->Header(), m_seats, m_profile)}; !valid.has_value()) return valid.error();
                    m_reports[m_view].insert_or_assign(report.key.seat, report);
                }
                if (auto cached{Cache(*view->selected_proposal)}; !cached.has_value()) return cached.error();
                m_offer = view->selected; m_have_work = true;
            }
        }
        // Detect partial control-history rollback against independently
        // retained ordinary-vote and prepared-certificate journals. This is
        // cross-file consistency, not a coherent rollback freshness witness.
        if (const auto prepared{m_context.commit_log.HighestDurablePreparedViewAt(0, m_slot)}; prepared && *prepared > m_view) {
            return Refuse(ErrorCode::FENCED, "PBFT_PREPARED_AHEAD_OF_CONTROL");
        }
        for (uint64_t view : m_context.signer_log.DurableAcceptedViewsAt(0, m_slot)) {
            if (view > m_view) return Refuse(ErrorCode::FENCED, "PBFT_ORDINARY_VIEW_AHEAD_OF_CONTROL");
            if (!view) continue;
            const auto accepted_view{m_control.AcceptedAt(m_slot, view)};
            const auto report{m_control.IntentAt(Key(view, storage::ControlPhase::VIEW_CHANGE))};
            if (!accepted_view || !report) return Refuse(ErrorCode::FENCED, "PBFT_ORDINARY_VIEW_CONTROL_HISTORY_MISSING");
            if (auto truthful{RestoreReport(*report)}; !truthful.has_value()) return truthful.error();
            auto decoded{DecodeMessage(accepted_view->message, m_profile)};
            if (!decoded.has_value()) return decoded.error();
            const auto* proof{std::get_if<ControlNewView>(&*decoded)};
            if (!proof || proof->key != accepted_view->key || proof->key.view != view) return Refuse(ErrorCode::FENCED, "PBFT_ORDINARY_NEW_VIEW_BINDING");
            if (auto verified{VerifyControlNewView(*proof, head->Header(), m_seats, m_profile)}; !verified.has_value()) return verified.error();
            auto ordinary{m_context.signer_log.AcceptedAt({0, m_slot, view}, m_profile)};
            if (!ordinary.has_value()) return ordinary.error();
            if (!*ordinary || !proof->selected_proposal || proof->selected_proposal->header != (*ordinary)->header ||
                proof->selected_proposal->body != (*ordinary)->body) return Refuse(ErrorCode::FENCED, "PBFT_ORDINARY_NEW_VIEW_SELECTION");
        }
        return Ok();
    }

    Result<void> Propose(const state::Candidate& candidate, const AuthorityVersion& authority) override
    {
        if (auto refreshed{Refresh()}; !refreshed.has_value()) return refreshed.error();
        if (auto ready{Ready()}; !ready.has_value()) return ready.error();
        if (candidate.Header().Authority() != authority) return Refuse(ErrorCode::INVALID_INPUT, "PBFT_PROPOSE_AUTHORITY");
        Proposal offered{0, m_view, candidate.Header(), m_parent_certificate, candidate.Body()};
        if (auto valid{Executable(offered)}; !valid.has_value()) return valid.error();
        if (auto cached{Cache(offered)}; !cached.has_value()) return cached.error();
        if (!m_offer) m_offer = offered.header.Hash();
        m_have_work = true;
        if (auto queued{Broadcast(Message{OfferedProposal{offered}})}; !queued.has_value()) return queued.error();
        if (!m_changing && Leader(m_view) == m_config.self_seat && !m_states.contains(m_view)) {
            if (auto started{StartProposal(offered)}; !started.has_value()) return started.error();
        }
        return Flush();
    }

    Result<void> OnMessage(uint32_t from, std::span<const unsigned char> bytes) override
    {
        if (auto refreshed{Refresh()}; !refreshed.has_value()) return refreshed.error();
        if (auto ready{Ready()}; !ready.has_value()) return ready.error();
        if (from >= m_seats.Size()) return Refuse(ErrorCode::INVALID_INPUT, "PBFT_PEER_SEAT");
        auto decoded{DecodeMessage(bytes, m_profile)};
        if (!decoded.has_value()) return decoded.error();
        Result<void> result{Ok()};
        if (const auto* offer{std::get_if<OfferedProposal>(&*decoded)}) result = OnOffer(offer->proposal);
        else if (const auto* proposal{std::get_if<Proposal>(&*decoded)}) {
            if (from != LeaderOf(proposal->header.height, proposal->view)) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_PROPOSAL_LEADER");
            result = AcceptProposal(*proposal);
            if (result.has_value() && proposal->header.height == m_slot && proposal->view == m_view && !m_changing) result = MakeVote(storage::VotePhase::PREPARE);
        } else if (const auto* vote{std::get_if<Vote>(&*decoded)}) {
            if (from != vote->seat) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_VOTE_SESSION");
            result = OnVote(*vote);
        } else if (const auto* final{std::get_if<CertifiedProposal>(&*decoded)}) result = Decide(*final);
        else if (const auto* report{std::get_if<ControlReport>(&*decoded)}) {
            if (from != report->key.seat) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_REPORT_SESSION");
            if (report->key.slot >= m_slot) result = OnReport(*report);
        } else if (const auto* view{std::get_if<ControlNewView>(&*decoded)}) {
            if (from != view->key.seat) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_NEW_VIEW_SESSION");
            if (view->key.slot >= m_slot) result = AcceptNewView(*view);
        } else result = Refuse(ErrorCode::UNSUPPORTED, "PBFT_LEGACY_CONTROL_UNSUPPORTED");
        if (!result.has_value()) return result.error();
        return Flush();
    }

    Result<void> OnTimeout(uint64_t now_ms) override
    {
        if (auto refreshed{Refresh()}; !refreshed.has_value()) return refreshed.error();
        if (auto ready{Ready()}; !ready.has_value()) return ready.error();
        if (m_now && now_ms < *m_now) return Refuse(ErrorCode::INVALID_INPUT, "PBFT_TIME_BACKWARDS");
        m_now = now_ms;
        if (auto flushed{Flush()}; !flushed.has_value()) return flushed.error();
        if (m_final || !m_have_work) return Ok();
        if (m_pending_new_view) {
            auto pending{*m_pending_new_view};
            auto accepted{AcceptNewView(pending)};
            if (!accepted.has_value() && accepted.error().code != ErrorCode::NEED_DATA && accepted.error().code != ErrorCode::BUSY) return accepted.error();
        }
        if (m_changing && m_own_report) {
            if (auto own{PublishReport(*m_own_report)}; !own.has_value() && own.error().code != ErrorCode::BUSY) return own.error();
        }
        // ACTIVE pending work has a timer. CHANGING has a timer only after
        // q exact same-target reports, never from queued requests or restart.
        const bool timed{!m_changing || Reports(m_view).size() >= m_seats.Quorum()};
        if (timed && !m_timer_start) m_timer_start = now_ms;
        if (!timed) m_timer_start.reset();
        if (timed && m_timer_start && now_ms - *m_timer_start >= Timeout()) {
            if (auto entered{EnterView(m_view + 1)}; !entered.has_value()) return entered.error();
        }
        if (!m_last_retry || now_ms - *m_last_retry >= RETRY_MS) {
            if (auto retried{Retry()}; !retried.has_value() && retried.error().code != ErrorCode::BUSY && retried.error().code != ErrorCode::NEED_DATA) return retried.error();
            m_last_retry = now_ms;
        }
        return Flush();
    }

    std::optional<Decision> TakeDecision() override
    {
        std::optional<Decision> taken;
        if (m_decision) { taken.emplace(std::move(*m_decision)); m_decision.reset(); }
        return taken;
    }
    uint64_t CurrentView() const override
    {
        const auto next{m_context.owner.Head()->Header().height + 1};
        if (next == m_slot) return m_view;
        const auto stored{m_control.ViewAt(next)};
        return stored ? stored->view : 0;
    }
    bool ChangingView() const override
    {
        const auto next{m_context.owner.Head()->Header().height + 1};
        if (next == m_slot) return m_changing;
        const auto stored{m_control.ViewAt(next)};
        return stored && stored->changing;
    }
    Result<std::optional<ControlNewView>> PendingObservationNewView(const std::optional<Proposal>& available = std::nullopt) const override
    {
        if (!m_changing || Leader(m_view) != m_config.self_seat || m_final) return Ok(std::optional<ControlNewView>{});
        const auto key{Key(m_view, storage::ControlPhase::NEW_VIEW)};
        if (const auto existing{m_control.IntentAt(key)}) {
            const auto decoded{DecodeMessage(existing->message, m_profile)};
            if (!decoded.has_value()) return decoded.error();
            const auto* view{std::get_if<ControlNewView>(&*decoded)};
            if (!view || view->key != key) return Refuse(ErrorCode::FENCED, "PBFT_OBSERVATION_INTENT_BINDING");
            if (auto valid{VerifyControlNewViewEvidence(*view, m_context.owner.Head()->Header(), m_seats, m_profile)}; !valid.has_value()) return valid.error();
            return Ok(std::optional<ControlNewView>{*view});
        }
        const auto found{m_reports.find(m_view)};
        if (found == m_reports.end() || found->second.size() < m_seats.Quorum()) return Ok(std::optional<ControlNewView>{});
        std::vector<ControlReport> reports;
        std::optional<ControlPrepared> highest;
        std::optional<CertifiedHeader> decision;
        for (const auto& [seat, report] : found->second) {
            (void)seat;
            if (auto valid{VerifyControlReport(report, m_context.owner.Head()->Header(), m_seats, m_profile)}; !valid.has_value()) return valid.error();
            reports.push_back(report);
            if (report.decision) decision = report.decision;
            if (report.prepared && (!highest || report.prepared->view > highest->view)) highest = report.prepared;
            if (reports.size() == m_seats.Quorum()) break;
        }
        const auto selected{decision ? std::optional{decision->header.Hash()} : highest ? std::optional{highest->header.Hash()} : m_offer};
        if (!selected) return Ok(std::optional<ControlNewView>{});
        auto body{Body(*selected, decision ? *decision->cert.view : highest ? highest->view : m_view)};
        if (!body.has_value()) return body.error();
        if (available && available->header.Hash() == *selected) body = Ok(std::optional<Proposal>{*available});
        if (!*body) return Ok(std::optional<ControlNewView>{});
        auto proposal{**body}; proposal.view = m_view;
        ControlNewView view{key, *selected, reports, {}, {}}; view.selected_proposal = proposal;
        if (auto proof{VerifyControlNewViewEvidence(view, m_context.owner.Head()->Header(), m_seats, m_profile)}; !proof.has_value()) return proof.error();
        if (auto execution{ValueExecutable(proposal)}; !execution.has_value()) return execution.error();
        return Ok(std::optional<ControlNewView>{std::move(view)});
    }
    Result<void> ResumeObservationNewView(const ControlNewView& view) override
    {
        if (view.key.seat != m_config.self_seat || Leader(view.key.view) != m_config.self_seat ||
            view.key.slot != m_slot || view.key.view != m_view) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_OBSERVATION_LEADER_INSTANCE");
        if (auto proof{VerifyControlNewViewEvidence(view, m_context.owner.Head()->Header(), m_seats, m_profile)}; !proof.has_value()) return proof.error();
        auto transcript{ControlIntent(view, m_profile)};
        if (!transcript.has_value()) return transcript.error();
        if (const auto known{m_control.AcceptedAt(m_slot, m_view)}) {
            const auto decoded{DecodeMessage(known->message, m_profile)};
            if (!decoded.has_value()) return decoded.error();
            const auto* accepted{std::get_if<ControlNewView>(&*decoded)};
            if (!accepted) return Refuse(ErrorCode::FENCED, "PBFT_OBSERVATION_ACCEPTED_BINDING");
            const auto previous{ControlIntent(*accepted, m_profile)};
            if (!previous.has_value()) return previous.error();
            if (*previous != *transcript) return Refuse(ErrorCode::EQUIVOCATION_REFUSED, "PBFT_OBSERVATION_ACCEPTED_CONFLICT");
            return Ok();
        }
        if (!m_changing) return Refuse(ErrorCode::ORDER_VIOLATION, "PBFT_OBSERVATION_NOT_CHANGING");
        if (const auto previous{m_control.IntentAt(view.key)}; previous && *previous != *transcript) return Refuse(ErrorCode::EQUIVOCATION_REFUSED, "PBFT_OBSERVATION_INTENT_CONFLICT");
        if (auto valid{Executable(*view.selected_proposal)}; !valid.has_value()) return valid.error();
        if (auto cached{Cache(*view.selected_proposal)}; !cached.has_value()) return cached.error();
        if (auto published{PublishNewView(view, *transcript)}; !published.has_value()) return published.error();
        return Flush();
    }

private:
    struct ViewState {
        uint256 hash;
        std::map<uint32_t, Vote> prepares;
        std::map<uint32_t, Vote> commits;
        bool prepare_sent{false};
        bool commit_sent{false};
        bool accepted{false};
    };
    struct Frame {
        std::vector<unsigned char> bytes;
        std::optional<OutboundVote> vote;
        std::optional<OutboundControl> control;
        uint256 header_hash;
    };
    struct VerifiedCertificate {
        SlotHeaderV2 header;
        uint256 digest;
        std::vector<unsigned char> bytes;
    };
    struct VerifiedVote {
        uint32_t seat;
        uint256 digest;
        storage::VoteSignature signature;
    };
    Result<void> VerifyCachedCertificate(const SlotCertificate& cert, const SlotHeaderV2& header, const uint256& digest) const
    {
        // The owned profile and verified roster are immutable for this
        // controller. Cache only successful pure cryptography for the exact
        // canonical certificate, complete header and phase/view-bound digest.
        // Structural checks still run on every call; source/execution/journal
        // checks remain at each original call site outside this helper.
        if (cert.header_hash != header.Hash()) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_CERT_HEADER");
        if (auto format{CertificateDigest(header, cert)}; !format.has_value()) return format.error();
        const auto indices{cert.SignerIndices(m_seats.Size())};
        if (!indices.has_value()) return indices.error();
        if (indices->size() < m_seats.Quorum()) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_CERT_QUORUM");
        const auto bytes{cert.Encode()};
        if (std::any_of(m_verified_certificates.begin(), m_verified_certificates.end(), [&](const auto& known) {
                return known.digest == digest && known.header == header && known.bytes == bytes;
            })) return Ok();
        if (auto verified{VerifyCert(cert, header, m_seats, digest)}; !verified.has_value()) return verified.error();
        if (m_verified_certificates.size() == MAX_VERIFIED_CERTIFICATES) m_verified_certificates.pop_front();
        m_verified_certificates.push_back({header, digest, bytes});
        return Ok();
    }
    Result<void> VerifyCachedVote(const Vote& vote, const uint256& digest)
    {
        // CheckVote, session seat, inclusion and instance/body checks run
        // before this helper. A different seat, digest or signature always
        // takes the full decode/verification path, including after eviction.
        if (std::any_of(m_verified_votes.begin(), m_verified_votes.end(), [&](const auto& known) {
                return known.seat == vote.seat && known.digest == digest && known.signature == vote.signature;
            })) return Ok();
        const auto signature{bls::Signature::Decode(vote.signature)};
        if (!signature || !bls::Verify(m_seats.Keys()[vote.seat].Key(), std::span{digest.data(), digest.size()}, *signature))
            return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_VOTE_SIGNATURE");
        if (m_verified_votes.size() == MAX_VERIFIED_VOTES) m_verified_votes.pop_front();
        m_verified_votes.push_back({vote.seat, digest, vote.signature});
        return Ok();
    }
    Result<void> Ready() const
    {
        if (m_context.signer_log.IsFatal() || m_context.commit_log.IsFatal() || m_control.IsFatal()) return Refuse(ErrorCode::FENCED, "PBFT_STORAGE_FENCED");
        if (m_halted) return Refuse(ErrorCode::FENCED, "PBFT_SAFETY_HALT");
        return Ok();
    }
    //! (slot + view) mod k, without overflow; the control log checks the same rule.
    uint32_t LeaderOf(uint64_t slot, uint64_t view) const
    {
        const uint64_t k{m_seats.Size()};
        return static_cast<uint32_t>((slot % k + view % k) % k);
    }
    uint32_t Leader(uint64_t view) const { return LeaderOf(m_slot, view); }
    //! f + 1 = k - quorum + 1: enough reports of a higher view that one is honest.
    size_t OneHonest() const { return m_seats.Size() - m_seats.Quorum() + 1; }
    uint64_t Timeout() const
    {
        uint64_t timeout{m_config.view_timeout_floor_ms};
        for (uint64_t i{0}; i < m_view && timeout < 60'000; ++i) timeout = std::min<uint64_t>(60'000, timeout * 2);
        return timeout;
    }
    storage::ControlKey Key(uint64_t view, storage::ControlPhase phase) const
    {
        const auto parent{m_context.owner.Head()};
        return {parent->Header().domain, parent->Header().profile_hash, m_seats.Root(), 0, m_slot, parent->Header().Hash(), m_config.self_seat, view, phase};
    }
    const std::map<uint32_t, ControlReport>& Reports(uint64_t view) const
    {
        static const std::map<uint32_t, ControlReport> empty;
        const auto it{m_reports.find(view)};
        return it == m_reports.end() ? empty : it->second;
    }
    Result<void> Refresh()
    {
        const auto head{m_context.owner.Head()};
        if (head->Header().height + 1 == m_slot) return Ok();
        if (head->Header().height != m_slot || !m_final || head->Header().Hash() != m_final->proposal.header.Hash()) return Refuse(ErrorCode::FENCED, "PBFT_HEAD_CHANGED_OUTSIDE_DECISION");
        if (m_decision) return Refuse(ErrorCode::ORDER_VIOLATION, "PBFT_DECISION_NOT_TAKEN");
        m_view = 0; m_changing = false; m_have_work = false;
        m_states.clear(); m_observed_votes.clear(); m_bodies.clear(); m_body_bytes = 0; m_reports.clear(); m_offer.reset(); m_new_view.reset(); m_known_decision.reset();
        m_own_report.reset(); m_pending_new_view.reset(); m_final.reset(); m_timer_start.reset(); m_last_retry.reset();
        m_verified_certificates.clear(); m_verified_votes.clear();
        // Retain pending exact frames until the transport accepts them.
        return Initialize();
    }

    Result<void> CheckFacts(const Proposal& proposal)
    {
        auto poll{m_context.adapter.Poll(proposal.header.facts_from, std::max<size_t>(1, proposal.body.facts.size()))};
        if (!poll.has_value()) return poll.error();
        if (poll->Status() != chain::PollStatus::FACTS) return Refuse(ErrorCode::NEED_DATA, "PBFT_FACT_EVIDENCE");
        const auto& batch{*poll->Batch()};
        if (batch.authority != proposal.header.Authority() || batch.from != proposal.header.facts_from || batch.to != proposal.header.facts_to ||
            batch.facts.size() != proposal.body.facts.size()) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_FACT_BATCH");
        for (size_t i{0}; i < batch.facts.size(); ++i) {
            auto bytes{EncodeFact(batch.facts[i])};
            if (!bytes.has_value()) return bytes.error();
            if (*bytes != proposal.body.facts[i]) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_FACT_BYTES");
        }
        return Ok();
    }
    Result<void> ValueExecutable(const Proposal& proposal) const
    {
        if (auto structural{CheckProposal(proposal)}; !structural.has_value()) return structural.error();
        const auto parent{m_context.owner.Head()};
        if (proposal.epoch != 0 || proposal.header.height != m_slot || proposal.header.parent_hash != parent->Header().Hash()) return Refuse(ErrorCode::STALE_PARENT, "PBFT_PARENT_NOT_HEAD");
        if (proposal.header.Authority() != m_context.adapter.CurrentAuthority() || proposal.header.seat_set_root != m_seats.Root() ||
            proposal.header.profile_hash != ProfileHash(m_profile)) return Refuse(ErrorCode::AUTHORITY_CHANGED, "PBFT_CONFIGURATION");
        if (proposal.parent_cert) {
            const auto digest{CertificateDigest(parent->Header(), *proposal.parent_cert)};
            if (!digest.has_value()) return digest.error();
            if (auto cert{VerifyCachedCertificate(*proposal.parent_cert, parent->Header(), *digest)}; !cert.has_value()) return cert.error();
        }
        if (auto execution{m_context.owner.Reexecute(proposal.header, proposal.body)}; !execution.has_value()) return execution.error();
        return Ok();
    }
    Result<void> SourceExecutable(const Proposal& proposal)
    {
        if (auto anchor{m_context.adapter.VerifyAnchor(proposal.header.anchor, m_context.owner.Head()->Header().anchor)}; !anchor.has_value()) return anchor.error();
        return CheckFacts(proposal);
    }
    Result<void> Executable(const Proposal& proposal)
    {
        if (auto value{ValueExecutable(proposal)}; !value.has_value()) return value.error();
        return SourceExecutable(proposal);
    }
    Result<void> Cache(const Proposal& proposal)
    {
        const auto hash{proposal.header.Hash()};
        if (const auto it{m_bodies.find(hash)}; it != m_bodies.end()) {
            if (it->second.body != proposal.body) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_BODY_CONFLICT");
            return Ok();
        }
        const auto size{proposal.body.Encode().size()};
        while (m_bodies.size() >= MAX_BODIES || size > MAX_BODY_CACHE_BYTES - m_body_bytes) {
            auto disposable{std::find_if(m_bodies.begin(), m_bodies.end(), [&](const auto& item) {
                const auto accepted{m_states.find(m_view)};
                return (accepted == m_states.end() || !accepted->second.accepted || accepted->second.hash != item.first) &&
                       (!m_offer || *m_offer != item.first) &&
                       (!m_pending_new_view || m_pending_new_view->selected != item.first);
            })};
            if (disposable == m_bodies.end()) return Refuse(ErrorCode::BUSY, "PBFT_BODY_CACHE_FULL");
            if (m_offer && *m_offer == disposable->first) m_offer.reset();
            m_body_bytes -= disposable->second.body.Encode().size();
            m_bodies.erase(disposable);
        }
        m_bodies.emplace(hash, proposal); m_body_bytes += size;
        return Ok();
    }
    Result<std::optional<Proposal>> Body(const uint256& hash, uint64_t view) const
    {
        if (const auto it{m_bodies.find(hash)}; it != m_bodies.end()) return Ok(std::optional{it->second});
        auto accepted{m_context.signer_log.AcceptedAt({0, m_slot, view}, m_profile)};
        if (!accepted.has_value()) return accepted.error();
        if (*accepted && (*accepted)->header.Hash() == hash) return Ok(std::optional{Proposal{0, view, (*accepted)->header, m_parent_certificate, (*accepted)->body}});
        if (const auto prepared{m_context.commit_log.PreparedAt(0, m_slot, view)}; prepared && prepared->header_hash == hash) {
            return Ok(std::optional{Proposal{0, view, prepared->header, m_parent_certificate, prepared->body}});
        }
        return Ok(std::optional<Proposal>{});
    }
    Result<void> OnOffer(const Proposal& proposal)
    {
        if (proposal.header.height < m_slot) return Ok();
        if (auto valid{Executable(proposal)}; !valid.has_value()) return valid.error();
        if (auto cached{Cache(proposal)}; !cached.has_value()) return cached.error();
        if (!m_offer) m_offer = proposal.header.Hash();
        m_have_work = true;
        if (m_pending_new_view) {
            auto pending{*m_pending_new_view};
            auto accepted{AcceptNewView(pending)};
            if (!accepted.has_value() && accepted.error().code != ErrorCode::NEED_DATA) return accepted.error();
        }
        if (auto learned{TryKnownDecision()}; !learned.has_value()) return learned.error();
        if (m_final) return Ok();
        if (m_changing) return MaybeNewView();
        if (!m_changing && Leader(m_view) == m_config.self_seat && !m_states.contains(m_view)) {
            auto offered{proposal}; offered.view = m_view;
            return StartProposal(offered);
        }
        return Ok();
    }
    Result<void> AcceptProposal(const Proposal& proposal)
    {
        if (const auto known{m_context.commit_log.DecisionAt(proposal.header.height)}; known) {
            if (known->header_hash != proposal.header.Hash()) return Refuse(ErrorCode::EQUIVOCATION_REFUSED, "PBFT_HEIGHT_DECIDED");
            return Ok();
        }
        if (proposal.view != m_view || m_changing) return Refuse(ErrorCode::ORDER_VIOLATION, "PBFT_VIEW_NOT_ACTIVE");
        if (m_view && (!m_new_view || m_new_view->selected != proposal.header.Hash())) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_NEW_VIEW_SELECTION");
        if (const auto existing{m_states.find(m_view)}; existing != m_states.end() && existing->second.hash != proposal.header.Hash()) return Refuse(ErrorCode::EQUIVOCATION_REFUSED, "PBFT_PROPOSAL_CONFLICT");
        if (auto valid{Executable(proposal)}; !valid.has_value()) return valid.error();
        if (auto cached{Cache(proposal)}; !cached.has_value()) return cached.error();
        const auto [it, inserted]{m_states.try_emplace(m_view, ViewState{proposal.header.Hash(), {}, {}})};
        if (!inserted && it->second.hash != proposal.header.Hash()) return Refuse(ErrorCode::EQUIVOCATION_REFUSED, "PBFT_PROPOSAL_CONFLICT");
        it->second.accepted = true;
        if (!m_offer) m_offer = proposal.header.Hash();
        if (const auto observed{m_observed_votes.find(std::pair{m_view, proposal.header.Hash()})}; observed != m_observed_votes.end()) {
            it->second.prepares = observed->second.prepares;
            it->second.commits = observed->second.commits;
            m_observed_votes.erase(observed);
        }
        m_have_work = true;
        return Ok();
    }
    Result<void> StartProposal(Proposal proposal)
    {
        proposal.view = m_view;
        if (Leader(m_view) != m_config.self_seat) return Refuse(ErrorCode::INVALID_INPUT, "PBFT_NOT_LEADER");
        if (auto accepted{AcceptProposal(proposal)}; !accepted.has_value()) return accepted.error();
        if (auto sent{Broadcast(Message{proposal})}; !sent.has_value()) return sent.error();
        return MakeVote(storage::VotePhase::PREPARE);
    }

    Result<void> MakeVote(storage::VotePhase phase)
    {
        if (auto ready{Ready()}; !ready.has_value()) return ready.error();
        if (m_changing || m_final) return Refuse(ErrorCode::ORDER_VIOLATION, "PBFT_VIEW_NOT_ACTIVE");
        const auto it{m_states.find(m_view)};
        if (it == m_states.end() || !it->second.accepted) return Refuse(ErrorCode::ORDER_VIOLATION, "PBFT_NO_PROPOSAL");
        auto& state{it->second};
        bool& sent{phase == storage::VotePhase::PREPARE ? state.prepare_sent : state.commit_sent};
        if (sent) return Ok();
        if (phase == storage::VotePhase::COMMIT && !m_context.commit_log.PreparedAt(0, m_slot, m_view)) return Refuse(ErrorCode::ORDER_VIOLATION, "PBFT_NO_DURABLE_PREPARATION");
        auto found{Body(state.hash, m_view)};
        if (!found.has_value()) return found.error();
        if (!*found) return Ok(); // no signing permission until the exact body returns
        auto proposal{**found}; proposal.view = m_view;
        if (auto valid{Executable(proposal)}; !valid.has_value()) {
            if (valid.error().code == ErrorCode::NEED_DATA || valid.error().code == ErrorCode::BUSY) return Ok();
            return valid.error();
        }
        if (auto active{m_control.AllowsVote(proposal.header.domain, 0, m_slot, m_view)}; !active.has_value()) return active.error();
        const storage::VoteKey key{proposal.header.domain, 0, m_slot, m_view, phase};
        const auto retained{m_context.signer_log.SignedAt(key)};
        std::optional<OutboundVote> outbound;
        if (retained) {
            auto durable{m_context.signer_log.Confirm(*retained)};
            if (!durable.has_value()) return durable.error();
            outbound = OutboundVote::From(*durable);
        } else {
            auto accepted{m_context.signer_log.AppendAcceptedProposal({0, m_slot, m_view}, proposal.header, proposal.body)};
            if (!accepted.has_value()) return accepted.error();
            const auto digest{phase == storage::VotePhase::PREPARE ? PbftPrepareDigest(proposal.header, m_view) : VersionedCommitDigest(proposal.header, m_view)};
            auto intent{m_context.signer_log.AppendIntent(key, digest, *accepted)};
            if (!intent.has_value()) return intent.error();
            if (auto barrier{m_context.signer_log.Barrier()}; !barrier.has_value()) return barrier.error();
            auto durable{m_context.signer_log.Confirm(*intent)};
            if (!durable.has_value()) return durable.error();
            if (auto authority{m_context.adapter.CheckAuthority(proposal.header.Authority())}; !authority.has_value()) return authority.error();
            if (auto facts{CheckFacts(proposal)}; !facts.has_value()) {
                // A durable but unsigned intent can be resumed. Missing source
                // evidence never causes a first signature or a replacement.
                if (facts.error().code == ErrorCode::NEED_DATA || facts.error().code == ErrorCode::BUSY) return Ok();
                return facts.error();
            }
            auto signed_vote{m_context.signer.SignAndPersist(*durable, m_context.signer_log)};
            if (!signed_vote.has_value()) return signed_vote.error();
            outbound = OutboundVote::From(*signed_vote);
        }
        Vote vote{phase, 0, m_view, m_slot, state.hash, m_config.self_seat, outbound->Signature(), {}};
        if (auto queued{Broadcast(Message{vote}, outbound)}; !queued.has_value()) return queued.error();
        sent = true;
        return OnVote(vote);
    }

    Result<SlotCertificate> Aggregate(const Proposal& proposal, const std::map<uint32_t, Vote>& votes, const uint256& digest)
    {
        std::vector<uint32_t> indices;
        std::vector<bls::Signature> signatures;
        for (const auto& [seat, vote] : votes) {
            indices.push_back(seat);
            const auto signature{bls::Signature::Decode(vote.signature)};
            if (!signature) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_SHARE_DECODE");
            signatures.push_back(*signature);
        }
        const auto aggregate{bls::AggregateSignatures(signatures)};
        if (!aggregate) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_AGGREGATE");
        SlotCertificate cert{proposal.header.Hash(), SlotCertificate::MakeBitmap(m_seats.Size(), indices).value(), aggregate->Compressed(), votes.begin()->second.view};
        if (auto valid{VerifyCachedCertificate(cert, proposal.header, digest)}; !valid.has_value()) return valid.error();
        return Ok(std::move(cert));
    }
    Result<void> SavePrepared(const ControlPrepared& prepared, const Proposal& proposal)
    {
        if (prepared.header != proposal.header || prepared.certificate.view != prepared.view) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_PREPARED_VIEW_OR_HEADER");
        if (auto valid{VerifyCachedCertificate(prepared.certificate, proposal.header, PbftPrepareDigest(proposal.header, prepared.view))}; !valid.has_value()) return valid.error();
        if (auto valid{Executable(proposal)}; !valid.has_value()) return valid.error();
        auto record{m_context.commit_log.RecordVerifiedPreparedQc({0, m_slot, prepared.view, proposal.header.Hash(), prepared.certificate.Encode(), proposal.header, proposal.body})};
        if (!record.has_value()) return record.error();
        if (auto barrier{m_context.commit_log.Barrier()}; !barrier.has_value()) return barrier.error();
        return Ok();
    }
    Result<void> OnVote(const Vote& vote)
    {
        if (auto valid{CheckVote(vote)}; !valid.has_value()) return valid.error();
        if (!vote.inclusion.empty()) return Refuse(ErrorCode::UNSUPPORTED, "PBFT_INCLUSION_NOT_SIGNED");
        if (const auto decided{m_context.commit_log.DecisionAt(vote.slot)}; decided && decided->header_hash == vote.header_hash) return Ok();
        if (vote.epoch != 0 || vote.slot != m_slot || vote.seat >= m_seats.Size() || vote.view > storage::MAX_CONTROL_VIEW) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_VOTE_INSTANCE");
        // Naked future votes never grant entry to their view.
        if (vote.view > m_view) return Refuse(ErrorCode::NEED_DATA, "PBFT_FUTURE_VOTE");
        auto body{Body(vote.header_hash, vote.view)};
        if (!body.has_value()) return body.error();
        if (!*body) return Refuse(ErrorCode::NEED_DATA, "PBFT_VOTE_WITHOUT_BODY");
        auto proposal{**body}; proposal.view = vote.view;
        const auto digest{vote.phase == storage::VotePhase::PREPARE ? PbftPrepareDigest(proposal.header, vote.view) : VersionedCommitDigest(proposal.header, vote.view)};
        if (auto signature{VerifyCachedVote(vote, digest)}; !signature.has_value()) return signature.error();
        ViewState* bucket{nullptr};
        const auto accepted{m_states.find(vote.view)};
        if (accepted != m_states.end() && accepted->second.hash == vote.header_hash) bucket = &accepted->second;
        else {
            const auto key{std::pair{vote.view, vote.header_hash}};
            if (!m_observed_votes.contains(key) && m_observed_votes.size() >= 128) return Refuse(ErrorCode::BUSY, "PBFT_RECEIVED_VOTE_BOUND");
            bucket = &m_observed_votes.try_emplace(key, ViewState{vote.header_hash, {}, {}}).first->second;
        }
        auto& votes{vote.phase == storage::VotePhase::PREPARE ? bucket->prepares : bucket->commits};
        if (const auto old{votes.find(vote.seat)}; old != votes.end() && old->second != vote) return Refuse(ErrorCode::EQUIVOCATION_REFUSED, "PBFT_DOUBLE_VOTE");
        votes.emplace(vote.seat, vote);
        if (vote.phase == storage::VotePhase::PREPARE && votes.size() >= m_seats.Quorum()) {
            auto qc{Aggregate(proposal, votes, PbftPrepareDigest(proposal.header, vote.view))};
            if (!qc.has_value()) return qc.error();
            if (auto saved{SavePrepared({vote.view, proposal.header, *qc}, proposal)}; !saved.has_value()) {
                if (saved.error().code == ErrorCode::NEED_DATA || saved.error().code == ErrorCode::BUSY) return Ok();
                return saved.error();
            }
            // Late preparation is retained but never rewrites an issued report
            // or manufactures permission to vote in an abandoned view.
            if (vote.view == m_view && !m_changing && bucket->accepted && !bucket->commit_sent) return MakeVote(storage::VotePhase::COMMIT);
        }
        if (vote.phase == storage::VotePhase::COMMIT && votes.size() >= m_seats.Quorum() && !m_final) {
            auto cert{Aggregate(proposal, votes, VersionedCommitDigest(proposal.header, vote.view))};
            if (!cert.has_value()) return cert.error();
            CertifiedProposal final{proposal, *cert};
            if (auto decided{Decide(final)}; !decided.has_value()) return decided.error();
            return Broadcast(Message{final});
        }
        return Ok();
    }

    Result<ControlReport> RestoreReport(const storage::ControlIntentRecord& intent)
    {
        auto decoded{DecodeMessage(intent.message, m_profile)};
        if (!decoded.has_value()) return decoded.error();
        const auto* report{std::get_if<ControlReport>(&*decoded)};
        if (!report || report->key != intent.key || report->prepared_watermark > m_context.commit_log.DurableSeq()) return Refuse(ErrorCode::FENCED, "PBFT_REPORT_REPLAY_BINDING");
        const auto expected{m_context.commit_log.HighestPreparedAt(0, m_slot, report->key.view, report->prepared_watermark)};
        if (expected.has_value() != report->prepared.has_value()) return Refuse(ErrorCode::FENCED, "PBFT_REPORT_REPLAY_HIGHEST");
        if (expected && (report->prepared->view != expected->view || report->prepared->header != expected->header ||
                         report->prepared->certificate.Encode() != expected->qc)) return Refuse(ErrorCode::FENCED, "PBFT_REPORT_REPLAY_HIGHEST");
        auto restored{*report};
        if (auto evidence{VerifyControlReportEvidence(restored, m_context.owner.Head()->Header(), m_seats, m_profile)}; !evidence.has_value()) return evidence.error();
        if (const auto signed_record{m_control.SignedAt(intent.key)}) {
            auto durable{m_control.Confirm(*signed_record)};
            if (!durable.has_value()) return durable.error();
            restored.signature = durable->Binding().signature;
            if (auto valid{VerifyControlReport(restored, m_context.owner.Head()->Header(), m_seats, m_profile)}; !valid.has_value()) return valid.error();
        }
        return Ok(std::move(restored));
    }

    Result<void> PublishReport(ControlReport report)
    {
        if (auto evidence{VerifyControlReportEvidence(report, m_context.owner.Head()->Header(), m_seats, m_profile)}; !evidence.has_value()) return evidence.error();
        auto intent_record{ControlIntent(report, m_profile)};
        if (!intent_record.has_value()) return intent_record.error();
        auto pending{m_control.AppendIntent(*intent_record)};
        if (!pending.has_value()) return pending.error();
        if (auto barrier{m_control.Barrier()}; !barrier.has_value()) return barrier.error();
        auto intent{m_control.Confirm(*pending)};
        if (!intent.has_value()) return intent.error();
        auto signed_record{m_context.signer.SignControlAndPersist(*intent, m_control)};
        if (!signed_record.has_value()) return signed_record.error();
        const auto outbound{OutboundControl::From(*signed_record)};
        report.signature = outbound.Signature();
        if (auto valid{VerifyControlReport(report, m_context.owner.Head()->Header(), m_seats, m_profile)}; !valid.has_value()) return valid.error();
        m_own_report = report;
        m_reports[report.key.view].insert_or_assign(m_config.self_seat, report);
        if (auto queued{Broadcast(Message{report}, std::nullopt, outbound)}; !queued.has_value()) return queued.error();
        // Prepared reports authenticate the value and view. Its exact durable
        // body is offered separately so a replacement leader can reexecute it.
        if (report.prepared) {
            auto body{Body(report.prepared->header.Hash(), report.prepared->view)};
            if (!body.has_value()) return body.error();
            if (!*body) return Refuse(ErrorCode::FENCED, "PBFT_DURABLE_PREPARED_BODY_MISSING");
            if (auto offered{Broadcast(Message{OfferedProposal{**body}})}; !offered.has_value()) return offered.error();
        }
        if (report.decision && (!report.prepared || report.prepared->header.Hash() != report.decision->header.Hash())) {
            auto body{Body(report.decision->header.Hash(), *report.decision->cert.view)};
            if (!body.has_value()) return body.error();
            if (*body) if (auto offered{Broadcast(Message{OfferedProposal{**body}})}; !offered.has_value()) return offered.error();
        }
        return MaybeNewView();
    }

    Result<void> EnterView(uint64_t target)
    {
        if (m_final || target < m_view) return Ok();
        if (target > storage::MAX_CONTROL_VIEW) return Refuse(ErrorCode::FENCED, "PBFT_VIEW_BOUND");
        if (target == m_view && m_own_report) return Ok();
        const auto key{Key(target, storage::ControlPhase::VIEW_CHANGE)};
        ControlReport report;
        if (const auto previous{m_control.IntentAt(key)}) {
            auto restored{RestoreReport(*previous)};
            if (!restored.has_value()) return restored.error();
            report = *restored;
        } else {
            report.key = key;
            report.prepared_watermark = m_context.commit_log.DurableSeq();
            if (const auto highest{m_context.commit_log.HighestPreparedAt(0, m_slot, target, report.prepared_watermark)}) {
                auto cert{SlotCertificate::Decode(highest->qc)};
                if (!cert.has_value()) return cert.error();
                report.prepared = ControlPrepared{highest->view, highest->header, *cert};
            }
            report.decision = m_known_decision;
        }
        // The durable record is the atomic view entry AND exact report intent.
        auto transcript{ControlIntent(report, m_profile)};
        if (!transcript.has_value()) return transcript.error();
        auto pending{m_control.AppendIntent(*transcript)};
        if (!pending.has_value()) return pending.error();
        m_view = target; m_changing = true; m_new_view.reset(); m_timer_start.reset();
        if (auto barrier{m_control.Barrier()}; !barrier.has_value()) return barrier.error();
        m_own_report = report;
        return PublishReport(report);
    }

    Result<void> OnReport(const ControlReport& report)
    {
        if (auto valid{VerifyControlReport(report, m_context.owner.Head()->Header(), m_seats, m_profile)}; !valid.has_value()) return valid.error();
        if (report.key.view < m_view) return Ok();
        if (report.key.view > m_view + VIEW_WINDOW) return Refuse(ErrorCode::BUSY, "PBFT_VIEW_DISCOVERY_BOUND");
        auto& reports{m_reports[report.key.view]};
        if (const auto old{reports.find(report.key.seat)}; old != reports.end() && old->second != report) return Refuse(ErrorCode::EQUIVOCATION_REFUSED, "PBFT_REPORT_EQUIVOCATION");
        reports.emplace(report.key.seat, report);
        m_have_work = true;
        if (report.decision) {
            m_known_decision = report.decision;
            if (auto learned{TryKnownDecision()}; !learned.has_value()) return learned.error();
            if (m_final) return Ok();
        }
        // A single arbitrary target never advances view state. f+1 reports
        // for ONE same target may synchronize a lagging honest replica.
        if (report.key.view > m_view && reports.size() >= OneHonest()) return EnterView(report.key.view);
        if (report.key.view == m_view && m_changing) return MaybeNewView();
        return Ok();
    }

    Result<void> MaybeNewView()
    {
        if (!m_changing || Leader(m_view) != m_config.self_seat || m_final) return Ok();
        // An existing immutable leader intent wins before consulting later
        // reports or replacement offers. Its complete body is independently
        // checked again immediately before the first signature or resend.
        const auto key{Key(m_view, storage::ControlPhase::NEW_VIEW)};
        if (const auto existing{m_control.IntentAt(key)}) {
            auto decoded{DecodeMessage(existing->message, m_profile)};
            if (!decoded.has_value()) return decoded.error();
            const auto* previous{std::get_if<ControlNewView>(&*decoded)};
            if (!previous || previous->key != existing->key || previous->key != key) return Refuse(ErrorCode::FENCED, "PBFT_NEW_VIEW_REPLAY_TYPE");
            return PublishNewView(*previous, *existing);
        }
        if (Reports(m_view).size() < m_seats.Quorum()) return Ok();
        std::vector<ControlReport> reports;
        for (const auto& [seat, report] : Reports(m_view)) { reports.push_back(report); if (reports.size() == m_seats.Quorum()) break; }
        std::optional<ControlPrepared> highest;
        for (const auto& report : reports) {
            if (auto verified{VerifyControlReport(report, m_context.owner.Head()->Header(), m_seats, m_profile)}; !verified.has_value()) return verified.error();
            if (report.decision) { m_known_decision = report.decision; return TryKnownDecision(); }
            if (report.prepared) {
                if (!highest || report.prepared->view > highest->view) highest = report.prepared;
                else if (report.prepared->view == highest->view && report.prepared->header.Hash() != highest->header.Hash()) {
                    m_halted = true; return Refuse(ErrorCode::FENCED, "PBFT_CONFLICTING_PREPARATIONS");
                }
            }
        }
        const auto selected{highest ? std::optional{highest->header.Hash()} : m_offer};
        if (!selected) return Ok(); // wait for an available, independently valid body
        auto found{Body(*selected, highest ? highest->view : m_view)};
        if (!found.has_value()) return found.error();
        if (!*found) return Ok();
        auto proposal{**found}; proposal.view = m_view;
        ControlNewView view{Key(m_view, storage::ControlPhase::NEW_VIEW), *selected, reports, {}, {}};
        view.selected_proposal = proposal; // exact body becomes part of the durable NV intent
        if (auto proof{VerifyControlNewViewEvidence(view, m_context.owner.Head()->Header(), m_seats, m_profile)}; !proof.has_value()) return proof.error();
        if (auto value{ValueExecutable(proposal)}; !value.has_value()) return value.error();
        if (auto source{SourceExecutable(proposal)}; !source.has_value()) {
            if (source.error().code == ErrorCode::NEED_DATA || source.error().code == ErrorCode::BUSY || source.error().code == ErrorCode::VERIFY_FAILED) return Ok();
            return source.error();
        }
        auto transcript{ControlIntent(view, m_profile)};
        if (!transcript.has_value()) return transcript.error();
        return PublishNewView(std::move(view), *transcript);
    }

    Result<void> PublishNewView(ControlNewView view, const storage::ControlIntentRecord& transcript)
    {
        if (auto evidence{VerifyControlNewViewEvidence(view, m_context.owner.Head()->Header(), m_seats, m_profile)}; !evidence.has_value()) return evidence.error();
        auto canonical{ControlIntent(view, m_profile)};
        if (!canonical.has_value()) return canonical.error();
        if (*canonical != transcript) return Refuse(ErrorCode::FENCED, "PBFT_NEW_VIEW_INTENT_BINDING");
        if (auto value{ValueExecutable(*view.selected_proposal)}; !value.has_value()) return value.error();
        if (auto source{SourceExecutable(*view.selected_proposal)}; !source.has_value()) {
            if (source.error().code == ErrorCode::NEED_DATA || source.error().code == ErrorCode::BUSY || source.error().code == ErrorCode::VERIFY_FAILED) return Ok();
            return source.error();
        }
        auto pending{m_control.AppendIntent(transcript)};
        if (!pending.has_value()) return pending.error();
        if (auto barrier{m_control.Barrier()}; !barrier.has_value()) return barrier.error();
        auto intent{m_control.Confirm(*pending)};
        if (!intent.has_value()) return intent.error();
        if (auto authority{m_context.adapter.CheckAuthority(view.selected_proposal->header.Authority())}; !authority.has_value()) return authority.error();
        if (auto facts{CheckFacts(*view.selected_proposal)}; !facts.has_value()) {
            if (facts.error().code == ErrorCode::NEED_DATA || facts.error().code == ErrorCode::BUSY) return Ok();
            return facts.error();
        }
        auto signed_record{m_context.signer.SignControlAndPersist(*intent, m_control)};
        if (!signed_record.has_value()) return signed_record.error();
        const auto outbound{OutboundControl::From(*signed_record)};
        view.signature = outbound.Signature();
        if (auto valid{VerifyControlNewView(view, m_context.owner.Head()->Header(), m_seats, m_profile)}; !valid.has_value()) return valid.error();
        if (auto queued{Broadcast(Message{view}, std::nullopt, outbound)}; !queued.has_value()) return queued.error();
        return AcceptNewView(view);
    }

    Result<void> AcceptNewView(const ControlNewView& view)
    {
        auto selected{VerifyControlNewView(view, m_context.owner.Head()->Header(), m_seats, m_profile)};
        if (!selected.has_value()) return selected.error();
        if (view.key.view < m_view) return Ok();
        if (m_new_view && view.key.view == m_view) {
            if (*m_new_view != view) return Refuse(ErrorCode::EQUIVOCATION_REFUSED, "PBFT_ACCEPTED_NEW_VIEW_CONFLICT");
            return Ok();
        }
        // The complete authenticated quorum proof fences abandoned rounds
        // even while the selected body's independent source is unavailable.
        // Report discovery stays window-bounded; a full q proof permits a
        // long-offline replica to join the proved bounded target directly.
        if (view.key.view > m_view) {
            if (auto entered{EnterView(view.key.view)}; !entered.has_value()) return entered.error();
        }
        if (selected->decision) {
            // The complete selected body travels in the authenticated bundle;
            // a lagging receiver may have never seen its separate OFFER.
            if (auto value{ValueExecutable(*view.selected_proposal)}; !value.has_value()) return value.error();
            if (auto source{SourceExecutable(*view.selected_proposal)}; !source.has_value()) {
                if (source.error().code == ErrorCode::NEED_DATA || source.error().code == ErrorCode::BUSY || source.error().code == ErrorCode::VERIFY_FAILED) { m_pending_new_view = view; return Ok(); }
                return source.error();
            }
            if (auto cached{Cache(*view.selected_proposal)}; !cached.has_value()) return cached.error();
            m_known_decision = selected->decision;
            return TryKnownDecision();
        }
        m_pending_new_view = view;
        if (!view.selected_proposal) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_SELECTED_BODY_MISSING");
        auto proposal{*view.selected_proposal}; proposal.view = view.key.view;
        if (auto value{ValueExecutable(proposal)}; !value.has_value()) return value.error();
        if (auto source{SourceExecutable(proposal)}; !source.has_value()) {
            if (source.error().code == ErrorCode::NEED_DATA || source.error().code == ErrorCode::BUSY || source.error().code == ErrorCode::VERIFY_FAILED) return Ok();
            return source.error();
        }
        if (selected->highest) {
            if (auto saved{SavePrepared(*selected->highest, proposal)}; !saved.has_value()) return saved.error();
        }
        if (auto cached{Cache(proposal)}; !cached.has_value()) return cached.error();
        auto bytes{EncodeMessage(Message{view}, m_profile)};
        if (!bytes.has_value()) return bytes.error();
        auto accepted{m_control.AcceptNewView({view.key, *bytes})};
        if (!accepted.has_value()) return accepted.error();
        if (auto barrier{m_control.Barrier()}; !barrier.has_value()) return barrier.error();
        m_view = view.key.view; m_changing = false; m_new_view = view; m_pending_new_view.reset();
        // The last observed tick may predate a long idle period. Arm this
        // freshly accepted view from the next actual tick, as for new offers,
        // rather than treating stale clock state as its receive timestamp.
        m_timer_start.reset(); m_have_work = true;
        if (Leader(m_view) == m_config.self_seat) return StartProposal(proposal);
        return Ok();
    }

    Result<void> TryKnownDecision()
    {
        if (!m_known_decision) return Ok();
        const auto view{m_known_decision->cert.view};
        if (!view) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_DECISION_VIEW_MISSING");
        auto found{Body(m_known_decision->header.Hash(), *view)};
        if (!found.has_value()) return found.error();
        if (!*found) return Ok();
        auto proposal{**found}; proposal.view = *view;
        return Decide({proposal, m_known_decision->cert});
    }
    Result<void> Decide(const CertifiedProposal& final)
    {
        if (auto ready{Ready()}; !ready.has_value()) return ready.error();
        const auto& header{final.proposal.header};
        if (final.certificate.view != final.proposal.view) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_DECISION_VIEW");
        auto digest{CertificateDigest(header, final.certificate)};
        if (!digest.has_value()) return digest.error();
        if (auto cert{VerifyCachedCertificate(final.certificate, header, *digest)}; !cert.has_value()) return cert.error();
        if (const auto known{m_context.commit_log.DecisionAt(header.height)}) {
            if (known->header_hash != header.Hash()) { m_halted = true; return Refuse(ErrorCode::FENCED, "PBFT_CONFLICTING_DECISION"); }
            return Ok();
        }
        if (auto valid{Executable(final.proposal)}; !valid.has_value()) return valid.error();
        auto pending{m_context.commit_log.AppendDecision(header, final.certificate, final.proposal.body)};
        if (!pending.has_value()) return pending.error();
        if (auto barrier{m_context.commit_log.Barrier()}; !barrier.has_value()) return barrier.error();
        auto durable{m_context.commit_log.Confirm(*pending)};
        if (!durable.has_value()) return durable.error();
        m_decision.emplace(Decision{std::move(*durable), final.certificate, final.proposal.body, header});
        m_final = final; m_parent_certificate = final.certificate; m_timer_start.reset();
        return Ok();
    }

    Result<void> Retry()
    {
        if (m_final) return Broadcast(Message{*m_final});
        if (m_known_decision) {
            if (auto learned{TryKnownDecision()}; !learned.has_value()) return learned.error();
            if (m_final) return Broadcast(Message{*m_final});
        }
        if (m_offer) {
            auto body{Body(*m_offer, m_view)};
            if (!body.has_value()) return body.error();
            if (*body) {
                if (auto offered{Broadcast(Message{OfferedProposal{**body}})}; !offered.has_value()) return offered.error();
            }
        }
        if (m_changing) {
            if (auto next{MaybeNewView()}; !next.has_value()) return next.error();
        } else if (const auto state{m_states.find(m_view)}; state != m_states.end()) {
            if (m_new_view && Leader(m_view) == m_config.self_seat) {
                const auto retained{m_control.SignedAt(Key(m_view, storage::ControlPhase::NEW_VIEW))};
                if (!retained) return Refuse(ErrorCode::FENCED, "PBFT_ACTIVE_LEADER_NEW_VIEW_SIGNATURE_MISSING");
                auto durable{m_control.Confirm(*retained)};
                if (!durable.has_value()) return durable.error();
                if (auto queued{Broadcast(Message{*m_new_view}, std::nullopt, OutboundControl::From(*durable))}; !queued.has_value()) return queued.error();
            }
            auto body{Body(state->second.hash, m_view)};
            if (!body.has_value()) return body.error();
            if (*body && Leader(m_view) == m_config.self_seat) {
                auto proposal{**body}; proposal.view = m_view;
                if (auto queued{Broadcast(Message{proposal})}; !queued.has_value()) return queued.error();
            }
            // Restore/retransmit exact signed objects. No additional signing
            // occurs for an already durable vote.
            for (auto phase : {storage::VotePhase::PREPARE, storage::VotePhase::COMMIT}) {
                const storage::VoteKey key{m_context.owner.Head()->Header().domain, 0, m_slot, m_view, phase};
                if (const auto retained{m_context.signer_log.SignedAt(key)}) {
                    auto durable{m_context.signer_log.Confirm(*retained)};
                    if (!durable.has_value()) return durable.error();
                    const auto outbound{OutboundVote::From(*durable)};
                    Vote vote{phase, 0, m_view, m_slot, state->second.hash, m_config.self_seat, outbound.Signature(), {}};
                    if (auto queued{Broadcast(Message{vote}, outbound)}; !queued.has_value()) return queued.error();
                    if (auto own{OnVote(vote)}; !own.has_value()) return own.error();
                } else if (phase == storage::VotePhase::PREPARE || m_context.commit_log.PreparedAt(0, m_slot, m_view)) {
                    if (auto made{MakeVote(phase)}; !made.has_value()) {
                        if (made.error().code != ErrorCode::NEED_DATA && made.error().code != ErrorCode::BUSY) return made.error();
                    }
                }
            }
        } else if (m_new_view && Leader(m_view) == m_config.self_seat) {
            auto body{Body(m_new_view->selected, m_view)};
            if (!body.has_value()) return body.error();
            if (*body) return StartProposal(**body);
        }
        return Ok();
    }

    Result<void> Broadcast(const Message& message, std::optional<OutboundVote> vote = std::nullopt,
                            std::optional<OutboundControl> control = std::nullopt)
    {
        auto bytes{EncodeMessage(message, m_profile)};
        if (!bytes.has_value()) return bytes.error();
        if (bytes->size() > MAX_PEER_OUTBOX_BYTES) return Refuse(ErrorCode::OVERSIZE, "PBFT_OUTBOX_FRAME_BOUND");
        uint256 hash;
        if (const auto* v{std::get_if<Vote>(&message)}) hash = v->header_hash;
        for (uint32_t seat{0}; seat < m_seats.Size(); ++seat) {
            if (seat == m_config.self_seat) continue;
            auto& queue{m_outbox[seat]};
            // Retry of an exact immutable object occupies one slot, even
            // while this peer refuses every transmission. A slow peer never
            // stops another peer's delivery or ordinary consensus progress.
            if (std::any_of(queue.begin(), queue.end(), [&](const auto& frame) { return frame.bytes == *bytes; })) continue;
            size_t queued_bytes{0};
            for (const auto& frame : queue) queued_bytes += frame.bytes.size();
            while (queue.size() >= MAX_PEER_OUTBOX || queued_bytes > MAX_PEER_OUTBOX_BYTES - bytes->size()) {
                queued_bytes -= queue.front().bytes.size();
                queue.pop_front();
            }
            // This is a bounded delivery cache, never the sole durable copy
            // of signed votes or controls. Current protocol state regenerates
            // them on timers. Historical catchup needs its own receipt path.
            queue.push_back({*bytes, vote, control, hash});
        }
        return Ok();
    }
    Result<void> Flush()
    {
        for (uint32_t seat{0}; seat < m_seats.Size(); ++seat) {
            auto& queue{m_outbox[seat]};
            while (!queue.empty()) {
                const auto& frame{queue.front()};
                auto sent{frame.vote ? m_context.sink.SendDurableVote(seat, *frame.vote, frame.header_hash, frame.bytes)
                                    : frame.control ? m_context.sink.SendDurableControl(seat, *frame.control, frame.bytes)
                                                    : m_context.sink.Send(seat, frame.bytes)};
                if (!sent.has_value()) {
                    if (sent.error().code != ErrorCode::BUSY) return sent.error();
                    break; // preserve this peer's order; continue other peers
                }
                queue.pop_front();
            }
        }
        return Ok();
    }

    ProfileParams m_profile;
    PbftConfig m_config;
    PbftContext m_context;
    SeatSet m_seats;
    storage::ControlLog& m_control;
    std::optional<SlotCertificate> m_parent_certificate;
    uint64_t m_slot{0};
    uint64_t m_view{0};
    bool m_changing{false};
    bool m_have_work{false};
    bool m_halted{false};
    size_t m_body_bytes{0};
    std::map<uint256, Proposal> m_bodies;
    mutable std::deque<VerifiedCertificate> m_verified_certificates;
    std::deque<VerifiedVote> m_verified_votes;
    std::map<uint64_t, ViewState> m_states;
    std::map<std::pair<uint64_t, uint256>, ViewState> m_observed_votes;
    std::map<uint64_t, std::map<uint32_t, ControlReport>> m_reports;
    std::optional<uint256> m_offer;
    std::optional<ControlReport> m_own_report;
    std::optional<ControlNewView> m_new_view;
    std::optional<ControlNewView> m_pending_new_view;
    std::optional<CertifiedHeader> m_known_decision;
    std::optional<CertifiedProposal> m_final;
    std::optional<Decision> m_decision;
    std::vector<std::deque<Frame>> m_outbox; //!< one queue per seat
    std::optional<uint64_t> m_now;
    std::optional<uint64_t> m_timer_start;
    std::optional<uint64_t> m_last_retry;
};
} // namespace

Result<std::unique_ptr<Agreement>> MakeRegtestFullViewAgreement(const ProfileParams& profile, const PbftConfig& config,
                                                               const PbftContext& context, SeatSet seats)
{
    if (!context.control_log || !context.signer.BoundToControlLog(*context.control_log)) return Refuse(ErrorCode::INVALID_INPUT, "PBFT_CONTROL_CAPABILITY_REQUIRED");
    const auto& control{context.control_log->Config()};
    const auto parent{context.owner.Head()};
    if (control.domain != parent->Header().domain || control.profile_hash != ProfileHash(profile) || control.seat_set_root != seats.Root() ||
        control.epoch != 0 || control.self_seat != config.self_seat || control.seat_count != seats.Size() || !profile.view_bound_commits ||
        parent->Header().commitment_version != COMMITMENT_VERSION_VIEW_BOUND) {
        return Refuse(ErrorCode::INVALID_INPUT, "PBFT_CONTROL_CONFIG");
    }
    auto agreement{std::make_unique<FullViewAgreement>(profile, config, context, std::move(seats))};
    if (auto initialized{agreement->Initialize()}; !initialized.has_value()) return initialized.error();
    return Ok(std::unique_ptr<Agreement>{std::move(agreement)});
}

} // namespace flowmesh2::agreement
