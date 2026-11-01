// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/derived_state.h>

#include <hash.h>
#include <serialize.h>
#include <streams.h>

#include <ios>
#include <string>
#include <utility>

namespace node {

namespace {
constexpr uint8_t RECORD_FORMAT{1};
constexpr uint8_t DB_RECORD{'r'};
//! Real build ids are a version and a commit hash.
constexpr size_t MAX_BUILD_ID_SIZE{256};

std::pair<uint8_t, uint8_t> RecordKey(const DerivedStateId id)
{
    return {DB_RECORD, static_cast<uint8_t>(id)};
}
} // namespace

std::string_view DerivedStateCheckName(const DerivedStateCheck check)
{
    switch (check) {
    case DerivedStateCheck::OK: return "ok";
    case DerivedStateCheck::MALFORMED: return "malformed";
    case DerivedStateCheck::BAD_CHECKSUM: return "bad checksum";
    case DerivedStateCheck::OTHER_FORMAT: return "other format";
    case DerivedStateCheck::OTHER_BUILD: return "written by another build";
    case DerivedStateCheck::OTHER_PARAMS: return "written under other consensus parameters";
    }
    return "unknown";
}

std::vector<unsigned char> EncodeDerivedStateRecord(const DerivedStateRecord& record, const std::string_view build_id)
{
    std::vector<unsigned char> out;
    VectorWriter w{out, 0};
    w << RECORD_FORMAT << std::string{build_id} << record.params_digest << record.block_hash
      << int32_t{record.height} << record.payload;
    const uint256 checksum{Hash(out)};
    w << checksum;
    return out;
}

DerivedStateCheck DecodeDerivedStateRecord(const std::span<const unsigned char> bytes, const std::string_view build_id,
                                           const uint256& params_digest, DerivedStateRecord& record)
{
    if (bytes.empty()) return DerivedStateCheck::MALFORMED;
    if (bytes[0] != RECORD_FORMAT) return DerivedStateCheck::OTHER_FORMAT;
    std::string writer;
    DerivedStateRecord decoded;
    try {
        SpanReader r{bytes};
        uint8_t format;
        int32_t height;
        uint256 checksum;
        r >> format >> LIMITED_STRING(writer, MAX_BUILD_ID_SIZE) >> decoded.params_digest >> decoded.block_hash
          >> height >> decoded.payload >> checksum;
        if (!r.empty()) return DerivedStateCheck::MALFORMED;
        if (checksum != Hash(bytes.first(bytes.size() - uint256::size()))) return DerivedStateCheck::BAD_CHECKSUM;
        decoded.height = height;
    } catch (const std::ios_base::failure&) {
        return DerivedStateCheck::MALFORMED;
    }
    if (writer != build_id) return DerivedStateCheck::OTHER_BUILD;
    if (decoded.params_digest != params_digest) return DerivedStateCheck::OTHER_PARAMS;
    record = std::move(decoded);
    return DerivedStateCheck::OK;
}

bool DerivedStateWriteDue(const bool forced, const std::optional<int> written_height, const bool written_on_chain,
                          const int tip_height)
{
    if (forced || !written_height || !written_on_chain) return true;
    return tip_height - *written_height >= DERIVED_STATE_WRITE_INTERVAL;
}

DerivedStateStore::DerivedStateStore(const DBParams& params) : m_db{params} {}

std::optional<std::vector<unsigned char>> DerivedStateStore::Read(const DerivedStateId id) const
{
    std::vector<unsigned char> record;
    if (!m_db.Read(RecordKey(id), record)) return std::nullopt;
    return record;
}

void DerivedStateStore::Write(const DerivedStateId id, const std::vector<unsigned char>& record)
{
    m_db.Write(RecordKey(id), record, /*fSync=*/true);
}

void DerivedStateStore::Erase(const DerivedStateId id)
{
    m_db.Erase(RecordKey(id), /*fSync=*/true);
}

} // namespace node
