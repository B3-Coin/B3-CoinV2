// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/dev/dev_certifier.h>

#include <crypto/bls.h>
#include <flowmesh2/agreement/outbound.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/storage/tokens.h>
#include <util/chaintype.h>

#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace flowmesh2::dev {

Result<std::unique_ptr<DevSingleSeatCertifier>> DevSingleSeatCertifier::Create(const ProfileParams& profile, chain::ChainAdapter& adapter,
                                                                               const SeatSet& seats, storage::SeatSigner& signer,
                                                                               storage::SignerLog& signer_log,
                                                                               storage::CommitLog& commit_log)
{
    if (Result<void> valid{Validate(profile)}; !valid.has_value()) return valid.error();
    if (adapter.Network().chain != ChainType::REGTEST) return Error{ErrorCode::UNSUPPORTED_NETWORK, DEV_NETWORK};
    if (seats.Size() != 1) return Error{ErrorCode::INVALID_INPUT, DEV_SEAT_COUNT};
    if (!(seats.Keys().front().Key() == signer.PublicKey())) return Error{ErrorCode::INVALID_INPUT, DEV_SEAT_KEY};
    return Ok(std::unique_ptr<DevSingleSeatCertifier>{new DevSingleSeatCertifier{adapter, seats, signer, signer_log, commit_log}});
}

DevSingleSeatCertifier::DevSingleSeatCertifier(chain::ChainAdapter& adapter, const SeatSet& seats, storage::SeatSigner& signer,
                                               storage::SignerLog& signer_log, storage::CommitLog& commit_log)
    : m_adapter{adapter}, m_seats{seats}, m_signer{signer}, m_signer_log{signer_log}, m_commit_log{commit_log}
{
}

Result<void> DevSingleSeatCertifier::Propose(const state::Candidate& candidate, const AuthorityVersion& authority)
{
    // 0. Preconditions; nothing is written before authority check 1.
    if (m_signer_log.IsFatal() || m_commit_log.IsFatal()) return Error{ErrorCode::FENCED, DEV_FENCED};
    if (m_decision.has_value()) return Error{ErrorCode::ORDER_VIOLATION, DEV_DECISION_PENDING};
    const SlotHeaderV2& header{candidate.Header()};
    if (!(header.Authority() == authority)) return Error{ErrorCode::INVALID_INPUT, DEV_CANDIDATE_AUTHORITY};
    if (header.seat_set_root != m_seats.Root()) return Error{ErrorCode::INVALID_INPUT, DEV_SEAT_SET};

    // 1. Authority check 1: before ACCEPTED_PROPOSAL; nothing written on refusal.
    if (Result<void> current{m_adapter.CheckAuthority(authority)}; !current.has_value()) return current.error();

    // 2-5. ACCEPTED_PROPOSAL and INTENT, then B-INTENT.
    const uint64_t view{0};
    Result<storage::PendingProposal> proposal{
        m_signer_log.AppendAcceptedProposal(storage::ProposalKey{header.authority_epoch, header.height, view}, header, candidate.Body())};
    if (!proposal.has_value()) return proposal.error();
    const storage::VoteKey key{header.domain, header.authority_epoch, header.height, view, storage::VotePhase::COMMIT};
    const uint256 digest{header.CommitDigest()};
    Result<storage::PendingIntent> intent{m_signer_log.AppendIntent(key, digest, *proposal)};
    if (!intent.has_value()) return intent.error();
    if (Result<uint64_t> barrier{m_signer_log.Barrier()}; !barrier.has_value()) return barrier.error();
    Result<DurableIntent> durable_intent{m_signer_log.Confirm(*intent)};
    if (!durable_intent.has_value()) return durable_intent.error();

    // 6. Authority check 2: immediately before the signature. The INTENT
    // stays durable; no signature is made.
    if (Result<void> current{m_adapter.CheckAuthority(authority)}; !current.has_value()) return current.error();

    // 7-10. Sign, SIGNED_VOTE, B-VOTE; only a durable vote may enter a certificate.
    Result<storage::VoteSignature> signature{m_signer.Sign(*durable_intent)};
    if (!signature.has_value()) return signature.error();
    Result<storage::PendingSignedVote> signed_vote{m_signer_log.AppendSignedVote(*durable_intent, *signature)};
    if (!signed_vote.has_value()) return signed_vote.error();
    if (Result<uint64_t> barrier{m_signer_log.Barrier()}; !barrier.has_value()) return barrier.error();
    Result<DurableSignedVote> durable_vote{m_signer_log.Confirm(*signed_vote)};
    if (!durable_vote.has_value()) return durable_vote.error();
    const agreement::OutboundVote vote{agreement::OutboundVote::From(*durable_vote)};

    // 11. The certificate of one seat: bitmap 0x01, the aggregate of its one
    // signature, checked like b3verify checks it.
    const std::array<uint32_t, 1> signers{0};
    Result<std::vector<unsigned char>> bitmap{SlotCertificate::MakeBitmap(m_seats.Size(), signers)};
    if (!bitmap.has_value()) return bitmap.error();
    const std::optional<bls::Signature> share{bls::Signature::Decode(vote.Signature())};
    if (!share.has_value()) return Error{ErrorCode::INVARIANT, DEV_CERTIFICATE};
    const std::array<bls::Signature, 1> shares{*share};
    const std::optional<bls::Signature> aggregate{bls::AggregateSignatures(shares)};
    if (!aggregate.has_value() ||
        !bls::FastAggregateVerify(m_seats.Keys(), std::span<const unsigned char>{digest.data(), digest.size()}, *aggregate)) {
        return Error{ErrorCode::INVARIANT, DEV_CERTIFICATE};
    }
    SlotCertificate certificate;
    certificate.header_hash = header.Hash();
    certificate.bitmap = std::move(bitmap).value();
    certificate.aggregate = aggregate->Compressed();

    // 12-14. DECISION{header, certificate, body}, then B-DECISION.
    Result<storage::PendingDecision> decision{m_commit_log.AppendDecision(header, certificate, candidate.Body())};
    if (!decision.has_value()) return decision.error();
    if (Result<uint64_t> barrier{m_commit_log.Barrier()}; !barrier.has_value()) return barrier.error();
    Result<DurableDecision> durable_decision{m_commit_log.Confirm(*decision)};
    if (!durable_decision.has_value()) return durable_decision.error();

    m_decision.emplace(agreement::Decision{std::move(durable_decision).value(), std::move(certificate), candidate.Body()});
    return Ok();
}

Result<void> DevSingleSeatCertifier::OnMessage(uint32_t /*from_seat*/, std::span<const unsigned char> /*bytes*/)
{
    // One seat receives nothing; the multi-seat message path is PBFT's.
    return Unsupported("M4-AGR-02"); // TODO(rebirth:M4-AGR-02)
}

Result<void> DevSingleSeatCertifier::OnTimeout(uint64_t /*now_ms*/)
{
    // Propose decides or fails synchronously: there is never pending work.
    return Ok();
}

std::optional<agreement::Decision> DevSingleSeatCertifier::TakeDecision()
{
    std::optional<agreement::Decision> taken;
    if (m_decision.has_value()) {
        taken.emplace(std::move(*m_decision));
        m_decision.reset();
    }
    return taken;
}

} // namespace flowmesh2::dev
