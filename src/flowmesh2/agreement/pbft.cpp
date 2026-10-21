// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/agreement/pbft.h>

#include <flowmesh2/agreement/outbound.h>
#include <flowmesh2/agreement/regtest_pbft.h>
#include <flowmesh2/core/hashes.h>
#include <util/chaintype.h>

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace flowmesh2::agreement {

// Durable view state / authenticated control-message signing is deliberately
// not enabled by the fixed-view regtest slice. No memory-only unlock exists.
Result<void> PbftSafetyRules::MayPrepare(const Proposal& /*proposal*/)
{
    return NotImplemented("M4-AGR-01"); // TODO(rebirth:M4-AGR-01)
}
Result<void> PbftSafetyRules::MayCommit(const Proposal& /*proposal*/, const PreparedCert& /*prepared*/)
{
    return NotImplemented("M4-AGR-01"); // TODO(rebirth:M4-AGR-01)
}
Result<void> PbftSafetyRules::OnViewChange(const ViewChange& /*report*/)
{
    return NotImplemented("M4-AGR-01"); // TODO(rebirth:M4-AGR-01)
}

uint256 PbftPrepareDigest(const SlotHeaderV2& header, uint64_t view)
{
    // Full header includes domain, deployment/profile, authority and sequence.
    // Inclusion lists are refused until their complete bytes can be signed.
    HashWriter writer{TaggedWriter(HashTag::PREPARE)};
    writer << uint8_t{1} << header << view << uint8_t{0};
    return writer.GetSHA256();
}

namespace {

Error Refuse(ErrorCode code, std::string_view detail) { return Error{code, detail}; }

Result<void> CheckCertificate(const SlotCertificate& cert, const SlotHeaderV2& header,
                              const SeatSet& seats, const uint256& digest)
{
    if (cert.header_hash != header.Hash()) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_CERT_HEADER");
    auto indices{cert.SignerIndices(seats.Size())};
    if (!indices.has_value()) return indices.error();
    if (indices->size() < seats.Quorum()) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_CERT_QUORUM");
    std::vector<bls::VerifiedPublicKey> keys;
    for (uint32_t i : *indices) keys.push_back(seats.Keys()[i]);
    const auto signature{bls::Signature::Decode(cert.aggregate)};
    if (!signature.has_value() || !bls::FastAggregateVerify(keys, std::span{digest.data(), digest.size()}, *signature)) {
        return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_CERT_SIGNATURE");
    }
    return Ok();
}

class FixedViewAgreement final : public Agreement
{
public:
    FixedViewAgreement(ProfileParams profile, PbftConfig config, PbftContext context, SeatSet seats)
        : m_profile{std::move(profile)}, m_config{config}, m_context{context}, m_seats{std::move(seats)},
          m_parent_certificate{config.parent_certificate} {}

    Result<void> Propose(const state::Candidate& candidate, const AuthorityVersion& authority) override
    {
        if (auto ready{Ready()}; !ready.has_value()) return ready.error();
        if (candidate.Header().Authority() != authority) return Refuse(ErrorCode::INVALID_INPUT, "PBFT_PROPOSE_AUTHORITY");
        if (Leader(candidate.Header().height) != m_config.self_seat) return Refuse(ErrorCode::INVALID_INPUT, "PBFT_NOT_LEADER");
        // The exact parent proof is carried as data; its v2 identity is the
        // certified parent value, independently of the valid quorum subset.
        Proposal proposal{candidate.Header().authority_epoch, 0, candidate.Header(), m_parent_certificate, candidate.Body()};
        if (candidate.Header().height > 1 && !m_parent_certificate) return Refuse(ErrorCode::NEED_DATA, "PBFT_PARENT_CERTIFICATE");
        if (auto accepted{AcceptProposal(proposal)}; !accepted.has_value()) return accepted.error();
        if (auto queued{Broadcast(Message{proposal})}; !queued.has_value()) return queued.error();
        if (auto vote{MakeVote(storage::VotePhase::PREPARE)}; !vote.has_value()) return vote.error();
        return Flush();
    }

    Result<void> OnMessage(uint32_t from, std::span<const unsigned char> bytes) override
    {
        if (m_context.signer_log.IsFatal() || m_context.commit_log.IsFatal()) return Ready();
        if (from >= m_seats.Size()) return Refuse(ErrorCode::INVALID_INPUT, "PBFT_PEER_SEAT");
        auto message{DecodeMessage(bytes, m_profile)};
        if (!message.has_value()) return message.error();
        // A timeout stops fresh voting; a valid already committed proof can
        // still finish the outstanding application without any new signature.
        if (m_fenced && !std::holds_alternative<CertifiedProposal>(*message)) return Ready();
        if (const auto* proposal{std::get_if<Proposal>(&*message)}) {
            if (from != Leader(proposal->header.height)) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_PROPOSAL_LEADER");
            if (const auto known{m_context.commit_log.DecisionAt(proposal->header.height)}; known) {
                if (known->header_hash != proposal->header.Hash()) return Refuse(ErrorCode::EQUIVOCATION_REFUSED, "PBFT_HEIGHT_DECIDED");
                return CheckProposal(*proposal); // exact old data, no second execution or vote
            }
            if (auto accepted{AcceptProposal(*proposal)}; !accepted.has_value()) return accepted.error();
            if (auto vote{MakeVote(storage::VotePhase::PREPARE)}; !vote.has_value()) return vote.error();
        } else if (const auto* vote{std::get_if<Vote>(&*message)}) {
            if (from != vote->seat) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_VOTE_SESSION");
            if (const auto known{m_context.commit_log.DecisionAt(vote->slot)}; known && known->header_hash == vote->header_hash) {
                return Ok(); // irrelevant duplicate traffic does not change a timer
            }
            if (auto accepted{AcceptVote(*vote)}; !accepted.has_value()) return accepted.error();
        } else if (const auto* final{std::get_if<CertifiedProposal>(&*message)}) {
            // The proof itself authenticates the value; a configured peer can
            // relay it. The leader emits one exact certificate in normal flow.
            if (auto accepted{AcceptDecision(*final)}; !accepted.has_value()) return accepted.error();
        } else {
            return Refuse(ErrorCode::UNSUPPORTED, "PBFT_VIEW_CHANGE_NOT_DURABLE");
        }
        return Flush();
    }

    Result<void> OnTimeout(uint64_t now_ms) override
    {
        if (m_context.signer_log.IsFatal() || m_context.commit_log.IsFatal()) return Ready();
        if (m_last_time && now_ms < *m_last_time) return Refuse(ErrorCode::INVALID_INPUT, "PBFT_TIME_BACKWARDS");
        m_last_time = now_ms;
        if (auto flushed{Flush()}; !flushed.has_value()) return flushed.error();
        if (m_fenced) return Refuse(ErrorCode::FENCED, "PBFT_TIMEOUT_NO_DURABLE_VIEW_CHANGE");
        if (m_proposal && !m_decision && !m_final) {
            if (!m_timer_start) m_timer_start = now_ms;
            if (now_ms - *m_timer_start >= m_config.view_timeout_floor_ms) {
                m_fenced = true;
                return Refuse(ErrorCode::FENCED, "PBFT_TIMEOUT_NO_DURABLE_VIEW_CHANGE");
            }
        }
        return Ready();
    }

    std::optional<Decision> TakeDecision() override
    {
        std::optional<Decision> taken;
        if (m_decision) {
            taken.emplace(std::move(*m_decision));
            m_decision.reset();
        }
        return taken;
    }

private:
    uint32_t Leader(uint64_t slot) const { return static_cast<uint32_t>(slot % m_seats.Size()); }
    Result<void> Ready() const
    {
        if (m_context.signer_log.IsFatal() || m_context.commit_log.IsFatal()) return Refuse(ErrorCode::FENCED, "PBFT_STORAGE_FENCED");
        if (m_fenced) return Refuse(ErrorCode::FENCED, "PBFT_TIMEOUT_NO_DURABLE_VIEW_CHANGE");
        return Ok();
    }

    Result<void> AcceptProposal(const Proposal& proposal)
    {
        if (proposal.view != 0) return Refuse(ErrorCode::UNSUPPORTED, "PBFT_VIEW_CHANGE_NOT_DURABLE");
        if (auto checked{CheckProposal(proposal)}; !checked.has_value()) return checked.error();
        if (m_decision && proposal.header.height != m_decision->decision.Slot()) {
            return Refuse(ErrorCode::ORDER_VIOLATION, "PBFT_DECISION_NOT_TAKEN");
        }
        const auto head{m_context.owner.Head()};
        if (proposal.header.height != head->Header().height + 1 || proposal.header.parent_hash != head->Header().Hash()) {
            return Refuse(ErrorCode::STALE_PARENT, "PBFT_PARENT_NOT_HEAD");
        }
        if (proposal.header.seat_set_root != m_seats.Root() || proposal.header.profile_hash != ProfileHash(m_profile)) {
            return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_CONFIGURATION");
        }
        if (proposal.epoch != 0) return Refuse(ErrorCode::UNSUPPORTED, "PBFT_STATIC_EPOCH_ONLY");
        if (m_proposal && m_proposal->header.height == proposal.header.height && m_proposal->header != proposal.header) {
            return Refuse(ErrorCode::EQUIVOCATION_REFUSED, "PBFT_PROPOSAL_CONFLICT");
        }
        if (const auto decided{m_context.commit_log.DecisionAt(proposal.header.height)};
            decided && decided->header_hash != proposal.header.Hash()) return Refuse(ErrorCode::EQUIVOCATION_REFUSED, "PBFT_HEIGHT_DECIDED");
        if (auto authority{m_context.adapter.CheckAuthority(proposal.header.Authority())}; !authority.has_value()) return authority.error();
        if (auto anchor{m_context.adapter.VerifyAnchor(proposal.header.anchor, head->Header().anchor)}; !anchor.has_value()) return anchor.error();
        if (proposal.parent_cert) {
            if (auto cert{CheckCertificate(*proposal.parent_cert, head->Header(), m_seats, head->Header().CommitDigest())}; !cert.has_value()) {
                return cert.error();
            }
        }
        auto candidate{m_context.owner.Reexecute(proposal.header, proposal.body)};
        if (!candidate.has_value()) return candidate.error();
        if (auto facts{CheckFacts(proposal)}; !facts.has_value()) return facts.error();
        if (!m_proposal || m_proposal->header.height != proposal.header.height) {
            m_proposal = proposal;
            m_prepares.clear();
            m_commits.clear();
            m_prepare_sent = false;
            m_commit_sent = false;
            m_final.reset();
            m_timer_start.reset();
        }
        return Ok();
    }

    Result<void> CheckFacts(const Proposal& proposal)
    {
        // The adapter is the operator's independently validated complete fact
        // stream. A leader's executable but fabricated credit is never enough.
        auto poll{m_context.adapter.Poll(proposal.header.facts_from, std::max<size_t>(1, proposal.body.facts.size()))};
        if (!poll.has_value()) return poll.error();
        if (poll->Status() != chain::PollStatus::FACTS) return Refuse(ErrorCode::NEED_DATA, "PBFT_FACT_EVIDENCE");
        const auto& batch{*poll->Batch()};
        if (batch.authority != proposal.header.Authority() || batch.from != proposal.header.facts_from ||
            batch.to != proposal.header.facts_to || batch.facts.size() != proposal.body.facts.size()) {
            return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_FACT_BATCH");
        }
        for (size_t i{0}; i < batch.facts.size(); ++i) {
            auto bytes{EncodeFact(batch.facts[i])};
            if (!bytes.has_value()) return bytes.error();
            if (*bytes != proposal.body.facts[i]) return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_FACT_BYTES");
        }
        return Ok();
    }

    Result<void> MakeVote(storage::VotePhase phase)
    {
        if (auto ready{Ready()}; !ready.has_value()) return ready.error();
        if (!m_proposal) return Refuse(ErrorCode::ORDER_VIOLATION, "PBFT_NO_PROPOSAL");
        bool& sent{phase == storage::VotePhase::PREPARE ? m_prepare_sent : m_commit_sent};
        if (sent) return Ok();
        const auto& proposal{*m_proposal};
        const auto& header{proposal.header};
        // Recheck the exact body and anchor immediately before writing a new
        // obligation, including restart/retry of an interrupted intent.
        if (auto current{m_context.adapter.CheckAuthority(header.Authority())}; !current.has_value()) return current.error();
        if (auto body{m_context.owner.Reexecute(header, proposal.body)}; !body.has_value()) return body.error();
        const auto head{m_context.owner.Head()};
        if (auto anchor{m_context.adapter.VerifyAnchor(header.anchor, head->Header().anchor)}; !anchor.has_value()) return anchor.error();
        if (auto facts{CheckFacts(proposal)}; !facts.has_value()) return facts.error();
        auto accepted{m_context.signer_log.AppendAcceptedProposal({header.authority_epoch, header.height, 0}, header, proposal.body)};
        if (!accepted.has_value()) return accepted.error();
        const uint256 digest{phase == storage::VotePhase::PREPARE ? PbftPrepareDigest(header, 0) : header.CommitDigest()};
        const storage::VoteKey key{header.domain, header.authority_epoch, header.height, 0, phase};
        auto intent{m_context.signer_log.AppendIntent(key, digest, *accepted)};
        if (!intent.has_value()) return intent.error();
        if (auto barrier{m_context.signer_log.Barrier()}; !barrier.has_value()) return barrier.error();
        auto durable{m_context.signer_log.Confirm(*intent)};
        if (!durable.has_value()) return durable.error();
        if (auto current{m_context.adapter.CheckAuthority(header.Authority())}; !current.has_value()) return current.error();
        if (auto facts{CheckFacts(proposal)}; !facts.has_value()) return facts.error();
        auto published{m_context.signer.SignAndPersist(*durable, m_context.signer_log)};
        if (!published.has_value()) return published.error();
        const OutboundVote outbound{OutboundVote::From(*published)};
        Vote vote{phase, header.authority_epoch, 0, header.height, header.Hash(), m_config.self_seat, outbound.Signature(), {}};
        if (auto queued{Broadcast(Message{vote}, outbound)}; !queued.has_value()) return queued.error();
        sent = true;
        return AcceptVote(vote);
    }

    Result<SlotCertificate> Aggregate(const std::map<uint32_t, Vote>& votes, const uint256& digest) const
    {
        std::vector<uint32_t> indices;
        std::vector<bls::Signature> shares;
        for (const auto& [seat, vote] : votes) {
            indices.push_back(seat);
            const auto share{bls::Signature::Decode(vote.signature)};
            if (!share) return Refuse(ErrorCode::INVARIANT, "PBFT_SHARE_DECODE");
            shares.push_back(*share);
        }
        auto bitmap{SlotCertificate::MakeBitmap(m_seats.Size(), indices)};
        if (!bitmap.has_value()) return bitmap.error();
        const auto aggregate{bls::AggregateSignatures(shares)};
        if (!aggregate) return Refuse(ErrorCode::INVARIANT, "PBFT_AGGREGATE");
        SlotCertificate cert{m_proposal->header.Hash(), std::move(*bitmap), aggregate->Compressed()};
        if (auto verified{CheckCertificate(cert, m_proposal->header, m_seats, digest)}; !verified.has_value()) return verified.error();
        return Ok(std::move(cert));
    }

    Result<void> AcceptVote(const Vote& vote)
    {
        if (auto valid{CheckVote(vote)}; !valid.has_value()) return valid.error();
        if (!vote.inclusion.empty()) return Refuse(ErrorCode::UNSUPPORTED, "PBFT_INCLUSION_NOT_SIGNED");
        if (!m_proposal) return Refuse(ErrorCode::NEED_DATA, "PBFT_VOTE_WITHOUT_BODY");
        const auto& header{m_proposal->header};
        if (vote.view != 0 || vote.epoch != header.authority_epoch || vote.slot != header.height || vote.header_hash != header.Hash()) {
            return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_VOTE_BINDING");
        }
        if (vote.seat >= m_seats.Size()) return Refuse(ErrorCode::INVALID_INPUT, "PBFT_VOTE_SEAT");
        const auto signature{bls::Signature::Decode(vote.signature)};
        const uint256 digest{vote.phase == storage::VotePhase::PREPARE ? PbftPrepareDigest(header, 0) : header.CommitDigest()};
        if (!signature || !bls::Verify(m_seats.Keys()[vote.seat].Key(), std::span{digest.data(), digest.size()}, *signature)) {
            return Refuse(ErrorCode::VERIFY_FAILED, "PBFT_VOTE_SIGNATURE");
        }
        auto& votes{vote.phase == storage::VotePhase::PREPARE ? m_prepares : m_commits};
        if (const auto it{votes.find(vote.seat)}; it != votes.end() && it->second != vote) {
            return Refuse(ErrorCode::EQUIVOCATION_REFUSED, "PBFT_VOTE_CONFLICT");
        }
        votes.emplace(vote.seat, vote);
        if (vote.phase == storage::VotePhase::PREPARE && votes.size() >= m_seats.Quorum() && !m_commit_sent) {
            auto prepared{Aggregate(votes, PbftPrepareDigest(header, 0))};
            if (!prepared.has_value()) return prepared.error();
            auto recorded{m_context.commit_log.RecordVerifiedPreparedQc({header.authority_epoch, header.height, 0, header.Hash(), prepared->Encode(), header, m_proposal->body})};
            if (!recorded.has_value()) return recorded.error();
            if (auto barrier{m_context.commit_log.Barrier()}; !barrier.has_value()) return barrier.error();
            return MakeVote(storage::VotePhase::COMMIT);
        }
        if (vote.phase == storage::VotePhase::COMMIT && m_commits.size() >= m_seats.Quorum() &&
            m_config.self_seat == Leader(header.height) && !m_final) {
            auto cert{Aggregate(m_commits, header.CommitDigest())};
            if (!cert.has_value()) return cert.error();
            CertifiedProposal final{*m_proposal, std::move(*cert)};
            if (auto accepted{AcceptDecision(final)}; !accepted.has_value()) return accepted.error();
            return Broadcast(Message{final});
        }
        return Ok();
    }

    Result<void> AcceptDecision(const CertifiedProposal& final)
    {
        if (m_context.signer_log.IsFatal() || m_context.commit_log.IsFatal()) return Ready();
        if (final.proposal.view != 0) return Refuse(ErrorCode::UNSUPPORTED, "PBFT_VIEW_CHANGE_NOT_DURABLE");
        const auto& header{final.proposal.header};
        if (auto cert{CheckCertificate(final.certificate, header, m_seats, header.CommitDigest())}; !cert.has_value()) return cert.error();
        if (const auto known{m_context.commit_log.DecisionAt(header.height)}; known) {
            if (known->header_hash != header.Hash()) return Refuse(ErrorCode::EQUIVOCATION_REFUSED, "PBFT_CONFLICTING_DECISION");
            // Proof subsets certify one value; never emit another application.
            return Ok();
        }
        if (auto accepted{AcceptProposal(final.proposal)}; !accepted.has_value()) return accepted.error();
        auto pending{m_context.commit_log.AppendDecision(header, final.certificate, final.proposal.body)};
        if (!pending.has_value()) return pending.error();
        if (auto barrier{m_context.commit_log.Barrier()}; !barrier.has_value()) return barrier.error();
        auto durable{m_context.commit_log.Confirm(*pending)};
        if (!durable.has_value()) return durable.error();
        m_decision.emplace(Decision{std::move(*durable), final.certificate, final.proposal.body, header});
        m_final = final;
        m_parent_certificate = final.certificate;
        return Ok();
    }

    Result<void> Broadcast(const Message& message, std::optional<OutboundVote> durable = std::nullopt)
    {
        auto bytes{EncodeMessage(message, m_profile)};
        if (!bytes.has_value()) return bytes.error();
        if (m_outbox.size() + m_seats.Size() - 1 > 24) return Refuse(ErrorCode::BUSY, "PBFT_OUTBOX_FULL");
        for (uint32_t seat{0}; seat < m_seats.Size(); ++seat) {
            if (seat != m_config.self_seat) m_outbox.push_back(OutboundFrame{seat, *bytes, durable, m_proposal->header.Hash()});
        }
        return Ok();
    }

    Result<void> Flush()
    {
        for (auto it{m_outbox.begin()}; it != m_outbox.end();) {
            auto sent{it->vote ? m_context.sink.SendDurableVote(it->seat, *it->vote, it->header_hash, it->bytes)
                               : m_context.sink.Send(it->seat, it->bytes)};
            if (!sent.has_value()) {
                if (sent.error().code != ErrorCode::BUSY) return sent.error();
                ++it;
            } else {
                it = m_outbox.erase(it);
            }
        }
        return Ok();
    }

    ProfileParams m_profile;
    PbftConfig m_config;
    PbftContext m_context;
    SeatSet m_seats;
    std::optional<Proposal> m_proposal;
    std::map<uint32_t, Vote> m_prepares;
    std::map<uint32_t, Vote> m_commits;
    std::optional<CertifiedProposal> m_final;
    std::optional<Decision> m_decision;
    std::optional<SlotCertificate> m_parent_certificate;
    struct OutboundFrame {
        uint32_t seat;
        std::vector<unsigned char> bytes;
        std::optional<OutboundVote> vote;
        uint256 header_hash;
    };
    std::deque<OutboundFrame> m_outbox;
    std::optional<uint64_t> m_timer_start;
    std::optional<uint64_t> m_last_time;
    bool m_prepare_sent{false};
    bool m_commit_sent{false};
    bool m_fenced{false};
};

} // namespace

Result<std::unique_ptr<Agreement>> PbftAgreement::Create(const ProfileParams& profile, const PbftConfig& config,
                                                         const PbftContext& context)
{
    if (!config.regtest_fixed_view && !config.regtest_full_view) return NotImplemented("M4-AGR-02"); // TODO(rebirth:M4-AGR-02)
    if (context.adapter.Network().chain != ChainType::REGTEST) return Refuse(ErrorCode::UNSUPPORTED_NETWORK, "PBFT_REGTEST_ONLY");
    if (auto valid{Validate(profile)}; !valid.has_value()) return valid.error();
    const auto expected{config.regtest_full_view ? (profile.source_range_commitment ? StableQNetworkRegtestProfile(ChainType::REGTEST)
        : StableQViewRegtestProfile(ChainType::REGTEST)) : StableQRegtestProfile(ChainType::REGTEST)};
    if (!expected.has_value()) return expected.error();
    if (profile != *expected) return Refuse(ErrorCode::INVALID_INPUT, "PBFT_STABLE_Q_PROFILE_ONLY");
    if (context.owner.Profile() != profile) return Refuse(ErrorCode::INVALID_INPUT, "PBFT_OWNER_PROFILE");
    const auto authority{context.adapter.CurrentAuthority()};
    if (authority.epoch != 0 || authority.profile_hash != ProfileHash(profile)) return Refuse(ErrorCode::INVALID_INPUT, "PBFT_STATIC_AUTHORITY");
    auto seats{context.adapter.SeatSetAt(authority.epoch)};
    if (!seats.has_value()) return seats.error();
    if (seats->Size() != 4 || seats->Quorum() != 3 || seats->Root() != authority.seat_set_root) return Refuse(ErrorCode::INVALID_INPUT, "PBFT_FOUR_SEATS");
    if (config.self_seat >= seats->Size() || seats->Keys()[config.self_seat].Key() != context.signer.PublicKey()) {
        return Refuse(ErrorCode::INVALID_INPUT, "PBFT_SELF_KEY");
    }
    if (config.view_timeout_floor_ms < 50 || config.view_timeout_floor_ms > 60'000) return Refuse(ErrorCode::INVALID_INPUT, "PBFT_TIMEOUT_BOUND");
    const auto parent{context.owner.Head()};
    if (parent->Header().height > 0) {
        if (!config.parent_certificate) return Refuse(ErrorCode::NEED_DATA, "PBFT_PARENT_CERTIFICATE");
        const auto digest{CertificateDigest(parent->Header(), *config.parent_certificate)};
        if (!digest.has_value()) return digest.error();
        if (auto checked{CheckCertificate(*config.parent_certificate, parent->Header(), *seats, *digest)}; !checked.has_value()) {
            return checked.error();
        }
    } else if (config.parent_certificate) {
        return Refuse(ErrorCode::INVALID_INPUT, "PBFT_GENESIS_CERTIFICATE");
    }
    if (config.regtest_full_view) return MakeRegtestFullViewAgreement(profile, config, context, std::move(*seats));
    return Ok(std::unique_ptr<Agreement>{new FixedViewAgreement{profile, config, context, std::move(*seats)}});
}

} // namespace flowmesh2::agreement
