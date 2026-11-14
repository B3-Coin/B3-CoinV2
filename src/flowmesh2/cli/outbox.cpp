// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/cli/outbox.h>

#include <flowmesh2/core/codec.h>
#include <flowmesh2/storage/record.h>
#include <serialize.h>
#include <streams.h>

#include <algorithm>
#include <system_error>
#include <utility>

namespace flowmesh2::cli {

namespace {

struct ActionRecord {
    std::string label;
    std::vector<unsigned char> bytes;
    SERIALIZE_METHODS(ActionRecord, obj) { READWRITE(obj.label, obj.bytes); }
};

struct ResultRecord {
    std::string label;
    uint8_t outcome{0};
    uint64_t height{0};
    uint32_t index{0};
    std::string detail;
    SERIALIZE_METHODS(ResultRecord, obj) { READWRITE(obj.label, obj.outcome, obj.height, obj.index, obj.detail); }
};

template <typename T>
Result<T> DecodeRecord(std::span<const unsigned char> payload)
{
    Result<T> decoded{DecodeExact<T>(payload, OUTBOX_MAX_PAYLOAD_BYTES)};
    if (!decoded.has_value()) return Error{ErrorCode::DECODE, OUTBOX_RECORD};
    return decoded;
}

} // namespace

bool IsKnown(OutboxOutcome outcome)
{
    return outcome == OutboxOutcome::INCLUDED || outcome == OutboxOutcome::EXPIRED || outcome == OutboxOutcome::FAILED;
}

std::string_view ToString(OutboxOutcome outcome)
{
    switch (outcome) {
    case OutboxOutcome::INCLUDED: return "INCLUDED";
    case OutboxOutcome::EXPIRED: return "EXPIRED";
    case OutboxOutcome::FAILED: return "FAILED";
    }
    return "UNKNOWN";
}

std::vector<unsigned char> EncodeOutboxAction(std::string_view label, std::span<const unsigned char> bytes)
{
    return EncodeBytes(ActionRecord{std::string{label}, std::vector<unsigned char>{bytes.begin(), bytes.end()}});
}

std::vector<unsigned char> EncodeOutboxResult(std::string_view label, const OutboxResult& result)
{
    return EncodeBytes(ResultRecord{std::string{label}, static_cast<uint8_t>(result.outcome), result.height, result.index, result.detail});
}

Outbox::Outbox(std::unique_ptr<storage::FileJournal> journal, ProfileParams profile) : m_journal{std::move(journal)}, m_profile{profile} {}

Result<std::unique_ptr<Outbox>> Outbox::Open(const fs::path& dir, const ProfileParams& profile)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) return Error{ErrorCode::STORAGE_FATAL, OUTBOX_DIR};
    // An outbox exists once its first segment does; a stray file in a fresh directory is not one (CreateNew still needs
    // the directory empty and says so).
    const bool existing{fs::exists(dir / fs::u8path(std::string{storage::FIRST_SEGMENT_NAME}))};
    Result<std::unique_ptr<storage::FileJournal>> journal{
        existing ? storage::FileJournal::OpenExisting(dir, storage::JournalRole::OUTBOX, OUTBOX_INSTANCE)
                 : storage::FileJournal::CreateNew(dir, storage::JournalRole::OUTBOX, OUTBOX_INSTANCE, OUTBOX_SEGMENT_BYTES)};
    if (!journal.has_value()) return journal.error();
    std::unique_ptr<Outbox> outbox{new Outbox{std::move(*journal), profile}};
    if (const Result<void> loaded{outbox->Load()}; !loaded.has_value()) return loaded.error();
    return Ok(std::move(outbox));
}

Result<void> Outbox::Load()
{
    const Result<std::vector<storage::JournalRecord>> records{m_journal->ReadAll()};
    if (!records.has_value()) return records.error();
    for (const storage::JournalRecord& record : *records) {
        if (record.type == storage::RecordType::OUTBOX_ACTION) {
            const Result<ActionRecord> decoded{DecodeRecord<ActionRecord>(record.payload)};
            if (!decoded.has_value()) return decoded.error();
            Result<ActionV2> action{ActionV2::Decode(decoded->bytes, m_profile)};
            if (!action.has_value()) return Error{ErrorCode::DECODE, OUTBOX_RECORD};
            m_entries.push_back(OutboxEntry{decoded->label, decoded->bytes, std::move(*action), std::nullopt});
        } else if (record.type == storage::RecordType::OUTBOX_RESULT) {
            const Result<ResultRecord> decoded{DecodeRecord<ResultRecord>(record.payload)};
            if (!decoded.has_value()) return decoded.error();
            const auto entry{std::find_if(m_entries.begin(), m_entries.end(), [&](const OutboxEntry& e) { return e.label == decoded->label; })};
            if (entry == m_entries.end()) return Error{ErrorCode::ORDER_VIOLATION, OUTBOX_RESULT_ORPHAN};
            if (!IsKnown(static_cast<OutboxOutcome>(decoded->outcome))) return Error{ErrorCode::DECODE, OUTBOX_RECORD};
            entry->result = OutboxResult{static_cast<OutboxOutcome>(decoded->outcome), decoded->height, decoded->index, decoded->detail};
        } else {
            return Error{ErrorCode::DECODE, OUTBOX_RECORD}; // the journal's role check keeps other types out; belt and braces
        }
    }
    return Ok();
}

Result<void> Outbox::Add(const std::string& label, std::span<const unsigned char> bytes)
{
    if (label.empty()) return Error{ErrorCode::INVALID_INPUT, OUTBOX_LABEL_EMPTY};
    if (Find(label).has_value()) return Error{ErrorCode::INVALID_INPUT, OUTBOX_LABEL_TAKEN};
    Result<ActionV2> action{ActionV2::Decode(bytes, m_profile)};
    if (!action.has_value()) return action.error();
    if (const Result<uint64_t> seq{m_journal->Append(storage::RecordType::OUTBOX_ACTION, EncodeOutboxAction(label, bytes))}; !seq.has_value()) {
        return seq.error();
    }
    if (const Result<uint64_t> durable{m_journal->Barrier()}; !durable.has_value()) return durable.error();
    m_entries.push_back(OutboxEntry{label, std::vector<unsigned char>{bytes.begin(), bytes.end()}, std::move(*action), std::nullopt});
    return Ok();
}

Result<void> Outbox::Record(const std::string& label, const OutboxResult& result)
{
    const auto entry{std::find_if(m_entries.begin(), m_entries.end(), [&](const OutboxEntry& e) { return e.label == label; })};
    if (entry == m_entries.end()) return Error{ErrorCode::INVALID_INPUT, OUTBOX_LABEL_UNKNOWN};
    if (entry->result.has_value()) return Error{ErrorCode::ORDER_VIOLATION, OUTBOX_RESULT_TWICE};
    if (!IsKnown(result.outcome)) return Error{ErrorCode::INVALID_INPUT, OUTBOX_RECORD};
    if (const Result<uint64_t> seq{m_journal->Append(storage::RecordType::OUTBOX_RESULT, EncodeOutboxResult(label, result))}; !seq.has_value()) {
        return seq.error();
    }
    if (const Result<uint64_t> durable{m_journal->Barrier()}; !durable.has_value()) return durable.error();
    entry->result = result;
    return Ok();
}

Result<OutboxEntry> Outbox::Find(std::string_view label) const
{
    for (const OutboxEntry& entry : m_entries) {
        if (entry.label == label) return Ok(entry);
    }
    return Error{ErrorCode::INVALID_INPUT, OUTBOX_LABEL_UNKNOWN};
}

std::optional<uint64_t> Outbox::HighestNonce(const AccountId& account) const
{
    std::optional<uint64_t> highest;
    for (const OutboxEntry& entry : m_entries) {
        if (entry.action.Account() == account && (!highest.has_value() || entry.action.nonce > *highest)) highest = entry.action.nonce;
    }
    return highest;
}

} // namespace flowmesh2::cli
