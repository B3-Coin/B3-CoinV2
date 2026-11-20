// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/storage/signer_log.h>

#include <flowmesh2/core/codec.h>
#include <serialize.h>
#include <streams.h>

#include <utility>

namespace flowmesh2::storage {

namespace {

constexpr size_t PROPOSAL_KEY_BYTES{24};

} // namespace

std::vector<unsigned char> AcceptedProposalRecord::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter{out, 0, key};
    const std::vector<unsigned char> header_bytes{header.Encode()};
    out.insert(out.end(), header_bytes.begin(), header_bytes.end());
    const std::vector<unsigned char> body_bytes{body.Encode()};
    out.insert(out.end(), body_bytes.begin(), body_bytes.end());
    return out;
}

size_t AcceptedProposalRecord::MaxEncodedSize(const ProfileParams& profile)
{
    return PROPOSAL_KEY_BYTES + SLOT_HEADER_BYTES + SlotBody::MaxEncodedSize(profile);
}

Result<AcceptedProposalRecord> AcceptedProposalRecord::Decode(std::span<const unsigned char> bytes, const ProfileParams& profile)
{
    if (bytes.size() > MaxEncodedSize(profile)) return Error{ErrorCode::OVERSIZE, "proposal record longer than its maximum encoding"};
    if (bytes.size() < PROPOSAL_KEY_BYTES + SLOT_HEADER_BYTES) return Error{ErrorCode::DECODE, "proposal record too short"};
    Result<ProposalKey> key{DecodeExact<ProposalKey>(bytes.first(PROPOSAL_KEY_BYTES), PROPOSAL_KEY_BYTES)};
    if (!key.has_value()) return key.error();
    Result<SlotHeaderV2> header{SlotHeaderV2::Decode(bytes.subspan(PROPOSAL_KEY_BYTES, SLOT_HEADER_BYTES))};
    if (!header.has_value()) return header.error();
    Result<SlotBody> body{SlotBody::Decode(bytes.subspan(PROPOSAL_KEY_BYTES + SLOT_HEADER_BYTES), profile)};
    if (!body.has_value()) return body.error();
    return Ok(AcceptedProposalRecord{*key, std::move(header).value(), std::move(body).value()});
}

std::vector<unsigned char> IntentRecord::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter{out, 0, key, digest, proposal_seq};
    return out;
}

Result<IntentRecord> IntentRecord::Decode(std::span<const unsigned char> bytes)
{
    return DecodeExactWith(bytes, INTENT_RECORD_BYTES, [](SpanReader& reader) {
        IntentRecord record;
        reader >> record.key >> record.digest >> record.proposal_seq;
        return record;
    });
}

std::vector<unsigned char> ViewStateRecord::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter{out, 0, epoch, slot, view};
    return out;
}

Result<ViewStateRecord> ViewStateRecord::Decode(std::span<const unsigned char> bytes)
{
    return DecodeExactWith(bytes, VIEW_STATE_RECORD_BYTES, [](SpanReader& reader) {
        ViewStateRecord record;
        reader >> record.epoch >> record.slot >> record.view;
        return record;
    });
}

std::vector<unsigned char> NewViewAcceptedRecord::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter writer{out, 0, epoch, slot, view};
    WriteCompactSize(writer, message.size());
    writer.write(MakeByteSpan(message));
    return out;
}

Result<NewViewAcceptedRecord> NewViewAcceptedRecord::Decode(std::span<const unsigned char> bytes)
{
    const size_t max_bytes{VIEW_STATE_RECORD_BYTES + GetSizeOfCompactSize(MAX_NEW_VIEW_MESSAGE_BYTES) + MAX_NEW_VIEW_MESSAGE_BYTES};
    return DecodeExactWith(bytes, max_bytes, [](SpanReader& reader) {
        NewViewAcceptedRecord record;
        reader >> record.epoch >> record.slot >> record.view;
        ReadCappedVector(reader, record.message, MAX_NEW_VIEW_MESSAGE_BYTES);
        return record;
    });
}

std::vector<unsigned char> SignedVoteRecord::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter{out, 0, key, digest, signature, intent_seq};
    return out;
}

Result<SignedVoteRecord> SignedVoteRecord::Decode(std::span<const unsigned char> bytes)
{
    return DecodeExactWith(bytes, SIGNED_VOTE_RECORD_BYTES, [](SpanReader& reader) {
        SignedVoteRecord record;
        reader >> record.key >> record.digest >> record.signature >> record.intent_seq;
        return record;
    });
}

Result<SignerLog> SignerLog::Create(Journal& journal)
{
    if (journal.Role() != JournalRole::SIGNER) return Error{ErrorCode::ORDER_VIOLATION, LOG_ROLE};
    const JournalStatus status{journal.Status()};
    if (status.fatal) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (status.appended_seq != 0) return Error{ErrorCode::ORDER_VIOLATION, LOG_HAS_HISTORY};
    return Ok(SignerLog{journal});
}

Result<SignerLog> SignerLog::Recover(Journal& journal, const ProfileParams& profile)
{
    if (journal.Role() != JournalRole::SIGNER) return Error{ErrorCode::ORDER_VIOLATION, LOG_ROLE};
    if (journal.IsFatal()) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    Result<std::vector<JournalRecord>> records{journal.ReadAll()};
    if (!records.has_value()) return records.error();
    SignerLog log{journal};
    const Error inconsistent{ErrorCode::VERIFY_FAILED, RECOVER_INCONSISTENT};
    for (const JournalRecord& record : *records) {
        switch (record.type) {
        case RecordType::ACCEPTED_PROPOSAL: {
            Result<AcceptedProposalRecord> decoded{AcceptedProposalRecord::Decode(record.payload, profile)};
            if (!decoded.has_value()) return decoded.error();
            if (!BodyMatchesHeader(decoded->header, decoded->body) || decoded->header.authority_epoch != decoded->key.epoch ||
                decoded->header.height != decoded->key.slot) {
                return inconsistent;
            }
            const uint256 header_hash{decoded->header.Hash()};
            const auto [it, inserted]{log.m_proposals.emplace(
                decoded->key, ProposalEntry{header_hash, decoded->header.domain, decoded->header.CommitDigest(), record.seq})};
            if (!inserted) return inconsistent; // the log never writes a key twice
            break;
        }
        case RecordType::INTENT: {
            Result<IntentRecord> decoded{IntentRecord::Decode(record.payload)};
            if (!decoded.has_value()) return decoded.error();
            if (IsVotePhase(decoded->key.phase)) {
                const auto proposal{log.m_proposals.find(decoded->key.Proposal())};
                if (proposal == log.m_proposals.end() || proposal->second.seq != decoded->proposal_seq ||
                    proposal->second.domain != decoded->key.domain ||
                    (decoded->key.phase == VotePhase::COMMIT && decoded->digest != proposal->second.commit_digest)) {
                    return inconsistent;
                }
            } else if (decoded->proposal_seq != 0) {
                return inconsistent; // a statement intent names no proposal
            }
            const auto [it, inserted]{log.m_votes.emplace(decoded->key, VoteEntry{decoded->digest, record.seq, std::nullopt, 0})};
            if (!inserted) return inconsistent; // one intent record per VoteKey
            break;
        }
        case RecordType::SIGNED_VOTE: {
            Result<SignedVoteRecord> decoded{SignedVoteRecord::Decode(record.payload)};
            if (!decoded.has_value()) return decoded.error();
            const auto vote{log.m_votes.find(decoded->key)};
            if (vote == log.m_votes.end() || vote->second.digest != decoded->digest || vote->second.intent_seq != decoded->intent_seq ||
                vote->second.signature.has_value()) {
                return inconsistent;
            }
            vote->second.signature = decoded->signature;
            vote->second.signed_seq = record.seq;
            break;
        }
        case RecordType::VIEW_STATE: {
            Result<ViewStateRecord> decoded{ViewStateRecord::Decode(record.payload)};
            if (!decoded.has_value()) return decoded.error();
            if (log.m_view_state.has_value()) {
                const bool forward{decoded->slot > log.m_view_state->slot ||
                                   (decoded->slot == log.m_view_state->slot && decoded->view > log.m_view_state->view)};
                if (!forward) return inconsistent; // the log never writes a view state backwards or twice
            }
            log.m_view_state = *decoded;
            log.m_view_state_seq = record.seq;
            break;
        }
        case RecordType::NEW_VIEW_ACCEPTED: {
            Result<NewViewAcceptedRecord> decoded{NewViewAcceptedRecord::Decode(record.payload)};
            if (!decoded.has_value()) return decoded.error();
            if (decoded->message.empty()) return inconsistent;
            const auto [it, inserted]{log.m_new_views.emplace(ProposalKey{decoded->epoch, decoded->slot, decoded->view},
                                                              NewViewEntry{std::move(decoded->message), record.seq})};
            if (!inserted) return inconsistent; // one accepted NEW_VIEW per view
            break;
        }
        case RecordType::DECISION:
        case RecordType::APPLIED:
        case RecordType::SNAPSHOT_MARK:
        case RecordType::PREPARED_QC:
        case RecordType::EVIDENCE:
        case RecordType::OUTBOX_ACTION:
        case RecordType::OUTBOX_RESULT:
            return Error{ErrorCode::ORDER_VIOLATION, RECORD_ROLE};
        }
    }
    return Ok(std::move(log));
}

Result<PendingProposal> SignerLog::AppendAcceptedProposal(const ProposalKey& key, const SlotHeaderV2& header, const SlotBody& body)
{
    if (m_journal->IsFatal()) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (header.authority_epoch != key.epoch || header.height != key.slot) {
        return Error{ErrorCode::INVALID_INPUT, PROPOSAL_KEY_MISMATCH};
    }
    if (!BodyMatchesHeader(header, body)) return Error{ErrorCode::VERIFY_FAILED, BODY_HEADER_MISMATCH};
    const uint256 header_hash{header.Hash()};
    const ProposalRef ref{key, header_hash};
    if (const auto it{m_proposals.find(key)}; it != m_proposals.end()) {
        if (it->second.header_hash != header_hash) return Error{ErrorCode::EQUIVOCATION_REFUSED, PROPOSAL_CONFLICT};
        return Ok(PendingProposal{m_journal->InstanceId(), it->second.seq, ref});
    }
    const std::vector<unsigned char> payload{AcceptedProposalRecord{key, header, body}.Encode()};
    Result<uint64_t> seq{m_journal->Append(RecordType::ACCEPTED_PROPOSAL, payload)};
    if (!seq.has_value()) return seq.error();
    m_proposals.emplace(key, ProposalEntry{header_hash, header.domain, header.CommitDigest(), *seq});
    return Ok(PendingProposal{m_journal->InstanceId(), *seq, ref});
}

Result<PendingIntent> SignerLog::AppendStatementIntent(const VoteKey& key, const uint256& digest)
{
    if (m_journal->IsFatal()) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (!IsKnown(key.phase) || IsVotePhase(key.phase)) return Error{ErrorCode::INVALID_INPUT, VOTE_PHASE};
    const VoteRef ref{key, digest};
    if (const auto it{m_votes.find(key)}; it != m_votes.end()) {
        if (it->second.digest != digest) return Error{ErrorCode::EQUIVOCATION_REFUSED, VOTE_EQUIVOCATION};
        return Ok(PendingIntent{m_journal->InstanceId(), it->second.intent_seq, ref});
    }
    const std::vector<unsigned char> payload{IntentRecord{key, digest, 0}.Encode()};
    Result<uint64_t> seq{m_journal->Append(RecordType::INTENT, payload)};
    if (!seq.has_value()) return seq.error();
    m_votes.emplace(key, VoteEntry{digest, *seq, std::nullopt, 0});
    return Ok(PendingIntent{m_journal->InstanceId(), *seq, ref});
}

std::optional<VoteSignature> SignerLog::SignedStatement(const VoteKey& key) const
{
    const auto it{m_votes.find(key)};
    if (it == m_votes.end()) return std::nullopt;
    return it->second.signature;
}

std::optional<SignedVoteRecord> SignerLog::SignedVote(const VoteKey& key) const
{
    const auto it{m_votes.find(key)};
    if (it == m_votes.end() || !it->second.signature.has_value()) return std::nullopt;
    return SignedVoteRecord{key, it->second.digest, *it->second.signature, it->second.intent_seq};
}

Result<PendingIntent> SignerLog::AppendIntent(const VoteKey& key, const uint256& digest, const PendingProposal& proposal)
{
    if (m_journal->IsFatal()) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (!IsVotePhase(key.phase)) return Error{ErrorCode::INVALID_INPUT, VOTE_PHASE};
    if (proposal.JournalInstance() != m_journal->InstanceId()) return Error{ErrorCode::VERIFY_FAILED, TOKEN_OTHER_LOG};
    const auto proposal_it{m_proposals.find(proposal.Key().key)};
    if (proposal_it == m_proposals.end() || proposal_it->second.seq != proposal.Seq() ||
        proposal_it->second.header_hash != proposal.Key().header_hash) {
        return Error{ErrorCode::VERIFY_FAILED, TOKEN_OTHER_LOG};
    }
    const ProposalEntry& accepted{proposal_it->second};
    if (key.Proposal() != proposal.Key().key || key.domain != accepted.domain) {
        return Error{ErrorCode::INVALID_INPUT, INTENT_PROPOSAL_MISMATCH};
    }
    if (key.phase == VotePhase::COMMIT && digest != accepted.commit_digest) {
        return Error{ErrorCode::INVALID_INPUT, INTENT_DIGEST_MISMATCH};
    }
    const VoteRef ref{key, digest};
    if (const auto it{m_votes.find(key)}; it != m_votes.end()) {
        // One digest per VoteKey, forever (section 7.2).
        if (it->second.digest != digest) return Error{ErrorCode::EQUIVOCATION_REFUSED, VOTE_EQUIVOCATION};
        return Ok(PendingIntent{m_journal->InstanceId(), it->second.intent_seq, ref});
    }
    const std::vector<unsigned char> payload{IntentRecord{key, digest, proposal.Seq()}.Encode()};
    Result<uint64_t> seq{m_journal->Append(RecordType::INTENT, payload)};
    if (!seq.has_value()) return seq.error();
    m_votes.emplace(key, VoteEntry{digest, *seq, std::nullopt, 0});
    return Ok(PendingIntent{m_journal->InstanceId(), *seq, ref});
}

Result<PendingSignedVote> SignerLog::AppendSignedVote(const DurableIntent& intent, const VoteSignature& signature)
{
    if (m_journal->IsFatal()) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (intent.JournalInstance() != m_journal->InstanceId()) return Error{ErrorCode::VERIFY_FAILED, TOKEN_OTHER_LOG};
    if (!Knows(intent)) return Error{ErrorCode::VERIFY_FAILED, INTENT_UNKNOWN};
    VoteEntry& entry{m_votes.find(intent.Binding().key)->second};
    const SignedVoteRef ref{intent.Binding().key, intent.Digest(), signature};
    if (entry.signature.has_value()) {
        if (*entry.signature != signature) return Error{ErrorCode::EQUIVOCATION_REFUSED, SIGNED_VOTE_CONFLICT};
        return Ok(PendingSignedVote{m_journal->InstanceId(), entry.signed_seq, ref});
    }
    const std::vector<unsigned char> payload{SignedVoteRecord{intent.Binding().key, intent.Digest(), signature, intent.Seq()}.Encode()};
    Result<uint64_t> seq{m_journal->Append(RecordType::SIGNED_VOTE, payload)};
    if (!seq.has_value()) return seq.error();
    entry.signature = signature;
    entry.signed_seq = *seq;
    return Ok(PendingSignedVote{m_journal->InstanceId(), *seq, ref});
}

bool SignerLog::Knows(const DurableIntent& intent) const
{
    if (intent.JournalInstance() != m_journal->InstanceId()) return false;
    const auto it{m_votes.find(intent.Binding().key)};
    return it != m_votes.end() && it->second.intent_seq == intent.Seq() && it->second.digest == intent.Digest();
}

Result<PendingSignedVote> SignerLog::AppendSignedFolded(const VoteKey& /*key*/, const uint256& /*digest*/,
                                                        const PendingProposal& /*proposal*/, const VoteSignature& /*signature*/)
{
    return NotImplemented("M4-STO-05"); // TODO(rebirth:M4-STO-05)
}

Result<uint64_t> SignerLog::AppendViewState(const ViewStateRecord& record)
{
    if (m_journal->IsFatal()) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (m_view_state.has_value()) {
        if (*m_view_state == record) return Ok(m_view_state_seq);
        const bool forward{record.slot > m_view_state->slot || (record.slot == m_view_state->slot && record.view > m_view_state->view)};
        if (!forward) return Error{ErrorCode::ORDER_VIOLATION, VIEW_STATE_BACKWARDS};
    }
    Result<uint64_t> seq{m_journal->Append(RecordType::VIEW_STATE, record.Encode())};
    if (!seq.has_value()) return seq.error();
    m_view_state = record;
    m_view_state_seq = *seq;
    return seq;
}

Result<uint64_t> SignerLog::AppendNewViewAccepted(const NewViewAcceptedRecord& record)
{
    if (m_journal->IsFatal()) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (record.message.empty()) return Error{ErrorCode::INVALID_INPUT, NEW_VIEW_EMPTY_MESSAGE};
    if (record.message.size() > MAX_NEW_VIEW_MESSAGE_BYTES) return Error{ErrorCode::OVERSIZE, "new-view message above its cap"};
    const ProposalKey key{record.epoch, record.slot, record.view};
    if (const auto it{m_new_views.find(key)}; it != m_new_views.end()) {
        if (it->second.message != record.message) return Error{ErrorCode::EQUIVOCATION_REFUSED, NEW_VIEW_CONFLICT};
        return Ok(it->second.seq);
    }
    Result<uint64_t> seq{m_journal->Append(RecordType::NEW_VIEW_ACCEPTED, record.Encode())};
    if (!seq.has_value()) return seq.error();
    m_new_views.emplace(key, NewViewEntry{record.message, *seq});
    return seq;
}

std::optional<std::vector<unsigned char>> SignerLog::NewViewAccepted(uint64_t epoch, uint64_t slot, uint64_t view) const
{
    const auto it{m_new_views.find(ProposalKey{epoch, slot, view})};
    if (it == m_new_views.end()) return std::nullopt;
    return it->second.message;
}

} // namespace flowmesh2::storage
