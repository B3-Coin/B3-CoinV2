// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/storage/record.h>

#include <crc32c/crc32c.h>
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <flowmesh2/core/hashes.h>

#include <algorithm>
#include <array>
#include <utility>

namespace flowmesh2::storage {

namespace {

constexpr std::array<RecordTypeInfo, 11> RECORD_TYPES{{
    {RecordType::INTENT, "INTENT"},
    {RecordType::SIGNED_VOTE, "SIGNED_VOTE"},
    {RecordType::DECISION, "DECISION"},
    {RecordType::APPLIED, "APPLIED"},
    {RecordType::SNAPSHOT_MARK, "SNAPSHOT_MARK"},
    {RecordType::ACCEPTED_PROPOSAL, "ACCEPTED_PROPOSAL"},
    {RecordType::VIEW_STATE, "VIEW_STATE"},
    {RecordType::NEW_VIEW_ACCEPTED, "NEW_VIEW_ACCEPTED"},
    {RecordType::PREPARED_QC, "PREPARED_QC"},
    {RecordType::CONTROL_INTENT, "CONTROL_INTENT"},
    {RecordType::SIGNED_CONTROL, "SIGNED_CONTROL"},
}};

bool AllZero(std::span<const unsigned char> bytes)
{
    return std::all_of(bytes.begin(), bytes.end(), [](unsigned char b) { return b == 0; });
}

} // namespace

std::span<const RecordTypeInfo> RecordTypeRegistry() { return RECORD_TYPES; }

bool IsKnown(RecordType type)
{
    return std::any_of(RECORD_TYPES.begin(), RECORD_TYPES.end(), [&](const RecordTypeInfo& info) { return info.type == type; });
}

std::string_view ToString(RecordType type)
{
    for (const auto& info : RECORD_TYPES) {
        if (info.type == type) return info.name;
    }
    return "UNKNOWN";
}

bool IsKnown(JournalRole role) { return role == JournalRole::SIGNER || role == JournalRole::COMMIT; }

std::string_view ToString(JournalRole role)
{
    switch (role) {
    case JournalRole::SIGNER: return "SIGNER";
    case JournalRole::COMMIT: return "COMMIT";
    }
    return "UNKNOWN";
}

bool RoleAccepts(JournalRole role, RecordType type)
{
    switch (role) {
    case JournalRole::SIGNER:
        return type == RecordType::INTENT || type == RecordType::SIGNED_VOTE || type == RecordType::ACCEPTED_PROPOSAL ||
               type == RecordType::VIEW_STATE || type == RecordType::NEW_VIEW_ACCEPTED ||
               type == RecordType::CONTROL_INTENT || type == RecordType::SIGNED_CONTROL;
    case JournalRole::COMMIT:
        return type == RecordType::DECISION || type == RecordType::APPLIED || type == RecordType::SNAPSHOT_MARK ||
               type == RecordType::PREPARED_QC;
    }
    return false;
}

uint256 JournalGenesisChain(JournalRole role, uint64_t instance_id)
{
    std::array<unsigned char, 9> message{};
    message[0] = static_cast<unsigned char>(role);
    WriteLE64(message.data() + 1, instance_id);
    return Tagged(HashTag::JOURNAL, message);
}

uint256 NextChain(const uint256& prev, RecordType type, std::span<const unsigned char> payload)
{
    const unsigned char type_byte{static_cast<unsigned char>(type)};
    uint256 out;
    CSHA256{}
        .Write(prev.data(), prev.size())
        .Write(&type_byte, 1)
        .Write(payload.data(), payload.size())
        .Finalize(out.data());
    return out;
}

uint32_t RecordCrc(RecordType type, std::span<const unsigned char> payload, const uint256& chain)
{
    const uint8_t type_byte{static_cast<uint8_t>(type)};
    uint32_t crc{crc32c::Extend(0, &type_byte, 1)};
    crc = crc32c::Extend(crc, payload.data(), payload.size());
    return crc32c::Extend(crc, chain.data(), chain.size());
}

Result<EncodedRecord> EncodeRecord(const uint256& prev, RecordType type, std::span<const unsigned char> payload)
{
    if (!IsKnown(type)) return Error{ErrorCode::INVALID_INPUT, RECORD_TYPE};
    if (payload.size() > MAX_RECORD_PAYLOAD_BYTES) return Error{ErrorCode::OVERSIZE, RECORD_LENGTH};
    EncodedRecord out;
    out.chain = NextChain(prev, type, payload);
    out.bytes.resize(RECORD_OVERHEAD_BYTES + payload.size());
    unsigned char* p{out.bytes.data()};
    WriteLE32(p, static_cast<uint32_t>(payload.size()));
    WriteLE32(p + 4, RecordCrc(type, payload, out.chain));
    p[8] = static_cast<unsigned char>(type);
    std::copy(payload.begin(), payload.end(), p + RECORD_HEADER_BYTES);
    std::copy(out.chain.begin(), out.chain.end(), p + RECORD_HEADER_BYTES + payload.size());
    return Ok(std::move(out));
}

Result<ScanResult> ScanRecords(std::span<const unsigned char> bytes, JournalRole role, uint64_t instance_id)
{
    if (!IsKnown(role)) return Error{ErrorCode::INVALID_INPUT, "unknown journal role"};
    ScanResult out;
    out.last_chain = JournalGenesisChain(role, instance_id);
    size_t pos{0};
    while (pos < bytes.size()) {
        const std::span<const unsigned char> rest{bytes.subspan(pos)};
        if (rest.size() < RECORD_HEADER_BYTES) {
            if (AllZero(rest)) break; // zero tail shorter than a header
            return Error{ErrorCode::DECODE, RECORD_LENGTH};
        }
        const uint32_t len{ReadLE32(rest.data())};
        const uint32_t crc{ReadLE32(rest.data() + 4)};
        const uint8_t type_byte{rest[8]};
        if (len == 0 && crc == 0 && type_byte == 0) {
            if (!AllZero(rest)) return Error{ErrorCode::DECODE, RECORD_TAIL};
            break;
        }
        if (len > MAX_RECORD_PAYLOAD_BYTES || rest.size() - RECORD_HEADER_BYTES < size_t{len} + RECORD_CHAIN_BYTES) {
            return Error{ErrorCode::DECODE, RECORD_LENGTH};
        }
        const RecordType type{static_cast<RecordType>(type_byte)};
        const std::span<const unsigned char> payload{rest.subspan(RECORD_HEADER_BYTES, len)};
        uint256 chain;
        std::copy_n(rest.begin() + RECORD_HEADER_BYTES + len, RECORD_CHAIN_BYTES, chain.begin());
        if (RecordCrc(type, payload, chain) != crc) return Error{ErrorCode::VERIFY_FAILED, RECORD_CRC};
        if (!IsKnown(type)) return Error{ErrorCode::DECODE, RECORD_TYPE};
        if (!RoleAccepts(role, type)) return Error{ErrorCode::ORDER_VIOLATION, RECORD_ROLE};
        if (NextChain(out.last_chain, type, payload) != chain) return Error{ErrorCode::VERIFY_FAILED, RECORD_CHAIN};
        out.records.push_back(JournalRecord{out.records.size() + 1, type, std::vector<unsigned char>(payload.begin(), payload.end()), chain});
        out.last_chain = chain;
        pos += RECORD_OVERHEAD_BYTES + len;
        out.end_offset = pos;
    }
    return Ok(std::move(out));
}

} // namespace flowmesh2::storage
