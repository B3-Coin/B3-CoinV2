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

std::vector<unsigned char> PreparedQcRecord::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter writer{out, 0};
    writer << epoch << slot << view << header_hash;
    const auto header_bytes{header.Encode()};
    out.insert(out.end(), header_bytes.begin(), header_bytes.end());
    VectorWriter qc_writer{out, out.size()};
    WriteCappedVector(qc_writer, qc, SlotCertificate::MAX_ENCODED_BYTES);
    const auto body_bytes{body.Encode()};
    out.insert(out.end(), body_bytes.begin(), body_bytes.end());
    return out;
}

size_t PreparedQcRecord::MaxEncodedSize(const ProfileParams& profile)
{
    return 56 + DecisionRecord::MaxEncodedSize(profile);
}

Result<PreparedQcRecord> PreparedQcRecord::Decode(std::span<const unsigned char> bytes, const ProfileParams& profile)
{
    if (bytes.size() > MaxEncodedSize(profile)) return Error{ErrorCode::OVERSIZE, "PBFT_PREPARED_SIZE"};
    if (bytes.size() < 56) return Error{ErrorCode::DECODE, "PBFT_PREPARED_PREFIX"};
    PreparedQcRecord record;
    SpanReader reader{bytes.first(56)};
    reader >> record.epoch >> record.slot >> record.view >> record.header_hash;
    auto decision{DecisionRecord::Decode(bytes.subspan(56), profile)};
    if (!decision.has_value()) return decision.error();
    record.qc = decision->certificate.Encode();
    record.header = std::move(decision->header);
    record.body = std::move(decision->body);
    return Ok(std::move(record));
}

namespace {
Result<void> CheckPreparedRecord(const PreparedQcRecord& record)
{
    if (record.slot == 0 || record.header_hash.IsNull()) return Error{ErrorCode::INVALID_INPUT, "PBFT_PREPARED_KEY"};
    if (record.header.authority_epoch != record.epoch || record.header.height != record.slot || record.header.Hash() != record.header_hash) {
        return Error{ErrorCode::VERIFY_FAILED, "PBFT_PREPARED_KEY_BINDING"};
    }
    if (!BodyMatchesHeader(record.header, record.body)) return Error{ErrorCode::VERIFY_FAILED, BODY_HEADER_MISMATCH};
    const auto certificate{SlotCertificate::Decode(record.qc)};
    if (!certificate.has_value()) return certificate.error();
    if (certificate->header_hash != record.header_hash) return Error{ErrorCode::VERIFY_FAILED, "PBFT_PREPARED_HEADER"};
    if (const auto format{CertificateDigest(record.header, *certificate)}; !format.has_value()) return format.error();
    if (record.header.commitment_version == COMMITMENT_VERSION_VIEW_BOUND && certificate->view != record.view) {
        return Error{ErrorCode::VERIFY_FAILED, "PBFT_PREPARED_VIEW"};
    }
    if (certificate->Encode() != record.qc) return Error{ErrorCode::DECODE, "PBFT_PREPARED_CANONICAL"};
    return Ok();
}
} // namespace

Result<CommitLog> CommitLog::Create(Journal& journal)
{
    return CreateImpl(journal, nullptr);
}

Result<CommitLog> CommitLog::Create(Journal& journal, const ProfileParams& profile)
{
    return CreateImpl(journal, &profile);
}

Result<CommitLog> CommitLog::CreateImpl(Journal& journal, const ProfileParams* profile)
{
    if (journal.Role() != JournalRole::COMMIT) return Error{ErrorCode::ORDER_VIOLATION, LOG_ROLE};
    const JournalStatus status{journal.Status()};
    if (status.fatal) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (status.appended_seq != status.durable_seq) return Error{ErrorCode::ORDER_VIOLATION, LOG_PENDING_RECORDS};
    if (profile) {
        if (Result<void> valid{Validate(*profile)}; !valid.has_value()) return valid.error();
    }
    if (status.durable_seq == 0) return Ok(CommitLog{journal});
    if (!profile) return Error{ErrorCode::INVALID_INPUT, LOG_REPLAY_PROFILE_REQUIRED};
    Result<std::vector<JournalRecord>> records{journal.ReadAll()};
    if (!records.has_value()) return records.error();
    if (records->size() != status.durable_seq) return Error{ErrorCode::ORDER_VIOLATION, LOG_REPLAY_SEQUENCE};

    CommitLog log{journal};
    uint64_t expected_seq{0};
    for (const JournalRecord& record : *records) {
        if (record.seq != ++expected_seq) return Error{ErrorCode::ORDER_VIOLATION, LOG_REPLAY_SEQUENCE};
        switch (record.type) {
        case RecordType::DECISION: {
            Result<DecisionRecord> decision{DecisionRecord::Decode(record.payload, *profile)};
            if (!decision.has_value()) return decision.error();
            const SlotHeaderV2& header{decision->header};
            const uint256 header_hash{header.Hash()};
            if (decision->certificate.header_hash != header_hash) return Error{ErrorCode::VERIFY_FAILED, DECISION_CERT_HEADER};
            if (!BodyMatchesHeader(header, decision->body)) return Error{ErrorCode::VERIFY_FAILED, BODY_HEADER_MISMATCH};
            const auto [it, inserted]{log.m_decisions.emplace(
                header.height, DecisionEntry{header_hash, decision->certificate.Hash(), header.state_root, record.seq, std::nullopt})};
            if (!inserted && it->second.header_hash != header_hash) return Error{ErrorCode::EQUIVOCATION_REFUSED, DECISION_CONFLICT};
            break;
        }
        case RecordType::APPLIED: {
            Result<AppliedRecord> applied{AppliedRecord::Decode(record.payload)};
            if (!applied.has_value()) return applied.error();
            const auto it{log.m_decisions.find(applied->slot)};
            if (it == log.m_decisions.end() || it->second.header_hash != applied->header_hash) {
                return Error{ErrorCode::VERIFY_FAILED, APPLIED_DECISION_UNKNOWN};
            }
            DecisionEntry& entry{it->second};
            if (applied->state_root != entry.state_root) return Error{ErrorCode::VERIFY_FAILED, APPLIED_ROOT_MISMATCH};
            if (!entry.applied_seq) {
                if (log.m_last_applied_slot == UINT64_MAX || applied->slot != log.m_last_applied_slot + 1) {
                    return Error{ErrorCode::ORDER_VIOLATION, APPLIED_OUT_OF_ORDER};
                }
                entry.applied_seq = record.seq;
                log.m_last_applied_slot = applied->slot;
            }
            break;
        }
        case RecordType::SNAPSHOT_MARK:
            return NotImplemented("M4-STO-03"); // TODO(rebirth:M4-STO-03)
        case RecordType::PREPARED_QC: {
            auto prepared{PreparedQcRecord::Decode(record.payload, *profile)};
            if (!prepared.has_value()) return prepared.error();
            if (auto valid{CheckPreparedRecord(*prepared)}; !valid.has_value()) return valid.error();
            const auto key{std::tuple{prepared->epoch, prepared->slot, prepared->view}};
            const auto [it, inserted]{log.m_prepared.emplace(key, std::pair{*prepared, record.seq})};
            if (!inserted && it->second.first.header_hash != prepared->header_hash) {
                return Error{ErrorCode::EQUIVOCATION_REFUSED, "PBFT_PREPARED_CONFLICT"};
            }
            break;
        }
        default:
            if (!IsKnown(record.type)) return Error{ErrorCode::DECODE, RECORD_TYPE};
            return Error{ErrorCode::ORDER_VIOLATION, RECORD_ROLE};
        }
    }
    const JournalStatus after{journal.Status()};
    if (after.fatal) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (!(after == status)) return Error{ErrorCode::ORDER_VIOLATION, LOG_REPLAY_CHANGED};
    return Ok(std::move(log));
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

Result<uint64_t> CommitLog::RecordVerifiedPreparedQc(const PreparedQcRecord& record)
{
    if (m_journal->IsFatal()) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (auto valid{CheckPreparedRecord(record)}; !valid.has_value()) return valid.error();
    const auto key{std::tuple{record.epoch, record.slot, record.view}};
    if (const auto it{m_prepared.find(key)}; it != m_prepared.end()) {
        if (it->second.first.header_hash != record.header_hash) return Error{ErrorCode::EQUIVOCATION_REFUSED, "PBFT_PREPARED_CONFLICT"};
        return Ok(it->second.second);
    }
    auto seq{m_journal->Append(RecordType::PREPARED_QC, record.Encode())};
    if (!seq.has_value()) return seq.error();
    m_prepared.emplace(key, std::pair{record, *seq});
    return seq;
}

std::optional<PreparedQcRecord> CommitLog::PreparedAt(uint64_t epoch, uint64_t slot, uint64_t view) const
{
    const auto it{m_prepared.find(std::tuple{epoch, slot, view})};
    return it == m_prepared.end() ? std::nullopt : std::optional{it->second.first};
}

std::optional<uint64_t> CommitLog::HighestDurablePreparedViewAt(uint64_t epoch, uint64_t slot) const
{
    std::optional<uint64_t> highest;
    const auto durable{m_journal->DurableSeq()};
    for (const auto& [key, entry] : m_prepared) {
        if (std::get<0>(key) == epoch && std::get<1>(key) == slot && entry.second <= durable &&
            (!highest || *highest < std::get<2>(key))) highest = std::get<2>(key);
    }
    return highest;
}

std::optional<PreparedQcRecord> CommitLog::HighestPreparedAt(uint64_t epoch, uint64_t slot, uint64_t before_view, uint64_t watermark) const
{
    std::optional<PreparedQcRecord> highest;
    for (const auto& [key, entry] : m_prepared) {
        if (std::get<0>(key) == epoch && std::get<1>(key) == slot && std::get<2>(key) < before_view && entry.second <= watermark &&
            (!highest || highest->view < entry.first.view)) highest = entry.first;
    }
    return highest;
}

} // namespace flowmesh2::storage
