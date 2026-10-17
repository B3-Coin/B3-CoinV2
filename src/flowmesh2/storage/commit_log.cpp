// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/storage/commit_log.h>

#include <flowmesh2/core/codec.h>
#include <serialize.h>
#include <streams.h>

#include <ios>
#include <utility>

namespace flowmesh2::storage {

bool BodyMatchesHeader(const SlotHeaderV2& header, const SlotBody& body)
{
    return body.ActionsRoot() == header.actions_root && body.FactsRoot() == header.facts_root &&
           body.originals.size() == header.action_count;
}

std::vector<unsigned char> DecisionRecord::Encode() const
{
    std::vector<unsigned char> out{header.Encode()};
    const std::vector<unsigned char> cert{certificate.Encode()};
    {
        VectorWriter writer{out, out.size()};
        WriteCompactSize(writer, cert.size());
    }
    out.insert(out.end(), cert.begin(), cert.end());
    const std::vector<unsigned char> body_bytes{body.Encode()};
    out.insert(out.end(), body_bytes.begin(), body_bytes.end());
    return out;
}

size_t DecisionRecord::MaxEncodedSize(const ProfileParams& profile)
{
    return SLOT_HEADER_BYTES + GetSizeOfCompactSize(SlotCertificate::MAX_ENCODED_BYTES) + SlotCertificate::MAX_ENCODED_BYTES +
           SlotBody::MaxEncodedSize(profile);
}

Result<DecisionRecord> DecisionRecord::Decode(std::span<const unsigned char> bytes, const ProfileParams& profile)
{
    if (bytes.size() > MaxEncodedSize(profile)) return Error{ErrorCode::OVERSIZE, "decision record longer than its maximum encoding"};
    if (bytes.size() < SLOT_HEADER_BYTES) return Error{ErrorCode::DECODE, "decision record shorter than a header"};
    Result<SlotHeaderV2> header{SlotHeaderV2::Decode(bytes.first(SLOT_HEADER_BYTES))};
    if (!header.has_value()) return header.error();

    const std::span<const unsigned char> rest{bytes.subspan(SLOT_HEADER_BYTES)};
    uint64_t cert_len{0};
    size_t prefix{0};
    try {
        SpanReader reader{rest};
        cert_len = ReadCompactSize(reader, /*range_check=*/false);
        prefix = rest.size() - reader.size();
    } catch (const std::ios_base::failure&) {
        return Error{ErrorCode::DECODE, "malformed certificate length"};
    }
    if (cert_len > SlotCertificate::MAX_ENCODED_BYTES) return Error{ErrorCode::OVERSIZE, "certificate longer than its maximum encoding"};
    if (rest.size() - prefix < cert_len) return Error{ErrorCode::DECODE, "decision record cut inside the certificate"};
    Result<SlotCertificate> certificate{SlotCertificate::Decode(rest.subspan(prefix, static_cast<size_t>(cert_len)))};
    if (!certificate.has_value()) return certificate.error();
    Result<SlotBody> body{SlotBody::Decode(rest.subspan(prefix + static_cast<size_t>(cert_len)), profile)};
    if (!body.has_value()) return body.error();
    return Ok(DecisionRecord{std::move(header).value(), std::move(certificate).value(), std::move(body).value()});
}

std::vector<unsigned char> AppliedRecord::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter{out, 0, slot, header_hash, state_root};
    return out;
}

Result<AppliedRecord> AppliedRecord::Decode(std::span<const unsigned char> bytes)
{
    return DecodeExactWith(bytes, APPLIED_RECORD_BYTES, [](SpanReader& reader) {
        AppliedRecord record;
        reader >> record.slot >> record.header_hash >> record.state_root;
        return record;
    });
}

Result<CommitLog> CommitLog::Create(Journal& journal)
{
    if (journal.Role() != JournalRole::COMMIT) return Error{ErrorCode::ORDER_VIOLATION, LOG_ROLE};
    const JournalStatus status{journal.Status()};
    if (status.fatal) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (status.appended_seq != 0) {
        // Rebuilding the decision index from a journal with history is recovery.
        return NotImplemented("M4-STO-01"); // TODO(rebirth:M4-STO-01)
    }
    return Ok(CommitLog{journal});
}

Result<PendingDecision> CommitLog::AppendDecision(const SlotHeaderV2& header, const SlotCertificate& certificate, const SlotBody& body)
{
    if (m_journal->IsFatal()) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    const uint256 header_hash{header.Hash()};
    if (certificate.header_hash != header_hash) return Error{ErrorCode::VERIFY_FAILED, DECISION_CERT_HEADER};
    if (!BodyMatchesHeader(header, body)) return Error{ErrorCode::VERIFY_FAILED, BODY_HEADER_MISMATCH};
    if (const auto it{m_decisions.find(header.height)}; it != m_decisions.end()) {
        if (it->second.header_hash != header_hash) return Error{ErrorCode::EQUIVOCATION_REFUSED, DECISION_CONFLICT};
        // Idempotent: the record keeps the first certificate, and so does the reference.
        return Ok(PendingDecision{m_journal->InstanceId(), it->second.seq,
                                  DecisionRef{header.height, header_hash, it->second.cert_hash}});
    }
    const uint256 cert_hash{certificate.Hash()};
    const std::vector<unsigned char> payload{DecisionRecord{header, certificate, body}.Encode()};
    Result<uint64_t> seq{m_journal->Append(RecordType::DECISION, payload)};
    if (!seq.has_value()) return seq.error();
    m_decisions.emplace(header.height, DecisionEntry{header_hash, cert_hash, header.state_root, *seq, std::nullopt});
    return Ok(PendingDecision{m_journal->InstanceId(), *seq, DecisionRef{header.height, header_hash, cert_hash}});
}

Result<PendingApplied> CommitLog::AppendApplied(const ApplyReceipt& receipt)
{
    if (m_journal->IsFatal()) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (receipt.JournalInstance() != m_journal->InstanceId()) return Error{ErrorCode::VERIFY_FAILED, TOKEN_OTHER_LOG};
    const auto it{m_decisions.find(receipt.Slot())};
    if (it == m_decisions.end() || it->second.seq != receipt.DecisionSeq() || it->second.header_hash != receipt.HeaderHash()) {
        return Error{ErrorCode::VERIFY_FAILED, APPLIED_DECISION_UNKNOWN};
    }
    DecisionEntry& entry{it->second};
    if (receipt.StateRoot() != entry.state_root) return Error{ErrorCode::VERIFY_FAILED, APPLIED_ROOT_MISMATCH};
    const AppliedRef ref{receipt.Slot(), entry.header_hash, entry.state_root};
    if (entry.applied_seq.has_value()) return Ok(PendingApplied{m_journal->InstanceId(), *entry.applied_seq, ref});
    if (m_last_applied_slot == UINT64_MAX || receipt.Slot() != m_last_applied_slot + 1) {
        return Error{ErrorCode::ORDER_VIOLATION, APPLIED_OUT_OF_ORDER};
    }
    const std::vector<unsigned char> payload{AppliedRecord{receipt.Slot(), entry.header_hash, entry.state_root}.Encode()};
    Result<uint64_t> seq{m_journal->Append(RecordType::APPLIED, payload)};
    if (!seq.has_value()) return seq.error();
    entry.applied_seq = *seq;
    m_last_applied_slot = receipt.Slot();
    return Ok(PendingApplied{m_journal->InstanceId(), *seq, ref});
}

std::optional<DecisionRef> CommitLog::DecisionAt(uint64_t slot) const
{
    const auto it{m_decisions.find(slot)};
    if (it == m_decisions.end()) return std::nullopt;
    return DecisionRef{slot, it->second.header_hash, it->second.cert_hash};
}

Result<uint64_t> CommitLog::AppendPreparedQc(const PreparedQcRecord& /*record*/)
{
    return NotImplemented("M4-STO-07"); // TODO(rebirth:M4-STO-07)
}

} // namespace flowmesh2::storage
