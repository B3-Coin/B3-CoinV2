// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/agreement/evidence.h>

#include <crypto/bls.h>
#include <flowmesh2/agreement/digests.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/seat_set.h>

#include <algorithm>
#include <span>
#include <utility>
#include <variant>

namespace flowmesh2::agreement {

Result<EvidenceStore> EvidenceStore::Create(const ProfileParams& profile, const uint256& domain, const chain::ChainAdapter& adapter,
                                            storage::CommitLog& log)
{
    EvidenceStore store{profile, domain, adapter, log};
    for (const std::vector<unsigned char>& payload : log.EvidenceRecords()) {
        Result<Message> decoded{DecodeMessage(payload, profile)};
        if (!decoded.has_value()) return decoded.error();
        const auto* evidence{std::get_if<Evidence>(&*decoded)};
        if (evidence == nullptr) return Error{ErrorCode::DECODE, EVIDENCE_RECORD};
        if (const Result<void> shape{CheckEvidence(*evidence)}; !shape.has_value()) return shape.error();
        store.m_recorded.push_back(*evidence);
    }
    return Ok(std::move(store));
}

Result<void> EvidenceStore::Verify(const Evidence& evidence) const
{
    if (const auto* dv{std::get_if<DoubleVote>(&evidence.content)}) {
        if (dv->first.phase != storage::VotePhase::PREPARE) return Error{ErrorCode::INVALID_INPUT, EVIDENCE_UNVERIFIABLE};
        Result<SeatSet> seats{m_adapter->SeatSetAt(dv->first.epoch)};
        if (!seats.has_value()) return seats.error();
        for (const Vote* vote : {&dv->first, &dv->second}) {
            if (vote->seat >= seats->Size()) return Error{ErrorCode::INVALID_INPUT, EVIDENCE_SEAT};
            const uint256 digest{PrepareDigest(m_domain, vote->epoch, vote->slot, vote->view, vote->header_hash)};
            const std::optional<bls::Signature> signature{bls::Signature::Decode(vote->signature)};
            if (!signature.has_value() ||
                !bls::Verify(seats->Keys()[vote->seat].Key(), std::span<const unsigned char>{digest.data(), digest.size()}, *signature)) {
                return Error{ErrorCode::VERIFY_FAILED, EVIDENCE_SIGNATURE};
            }
        }
        return Ok();
    }
    const auto& cc{std::get<ConflictingCertificates>(evidence.content)};
    for (const CertifiedHeader* certified : {&cc.first, &cc.second}) {
        Result<SeatSet> seats{m_adapter->SeatSetAt(certified->header.authority_epoch)};
        if (!seats.has_value()) return seats.error();
        const Result<std::vector<uint32_t>> signers{certified->cert.SignerIndices(seats->Size())};
        if (!signers.has_value()) return signers.error();
        if (signers->size() < seats->Quorum()) return Error{ErrorCode::VERIFY_FAILED, EVIDENCE_CERTIFICATE};
        std::vector<bls::VerifiedPublicKey> keys;
        for (const uint32_t seat : *signers) keys.push_back(seats->Keys()[seat]);
        const uint256 digest{certified->header.CommitDigest()};
        const std::optional<bls::Signature> aggregate{bls::Signature::Decode(certified->cert.aggregate)};
        if (!aggregate.has_value() ||
            !bls::FastAggregateVerify(keys, std::span<const unsigned char>{digest.data(), digest.size()}, *aggregate)) {
            return Error{ErrorCode::VERIFY_FAILED, EVIDENCE_CERTIFICATE};
        }
    }
    return Ok();
}

Result<void> EvidenceStore::Record(const Evidence& evidence)
{
    if (const Result<void> shape{CheckEvidence(evidence)}; !shape.has_value()) return shape.error();
    if (const Result<void> proof{Verify(evidence)}; !proof.has_value()) return proof.error();
    if (std::find(m_recorded.begin(), m_recorded.end(), evidence) != m_recorded.end()) return Ok(); // idempotent
    Result<std::vector<unsigned char>> payload{EncodeMessage(Message{evidence}, m_profile)};
    if (!payload.has_value()) return payload.error();
    if (const Result<uint64_t> seq{m_log->AppendEvidence(*payload)}; !seq.has_value()) return seq.error();
    if (const Result<uint64_t> barrier{m_log->Barrier()}; !barrier.has_value()) return barrier.error();
    m_recorded.push_back(evidence);
    return Ok();
}

} // namespace flowmesh2::agreement
