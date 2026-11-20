// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/net/sync.h>

#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/storage/commit_log.h>

#include <algorithm>
#include <cstring>

namespace flowmesh2::net {

namespace {

void PutU8(std::vector<unsigned char>& out, uint8_t v) { out.push_back(v); }
void PutU32(std::vector<unsigned char>& out, uint32_t v)
{
    for (int i{0}; i < 4; ++i) out.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xff));
}
void PutU64(std::vector<unsigned char>& out, uint64_t v)
{
    for (int i{0}; i < 8; ++i) out.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xff));
}
void PutHash(std::vector<unsigned char>& out, const uint256& h) { out.insert(out.end(), h.begin(), h.end()); }
void PutBytes(std::vector<unsigned char>& out, std::span<const unsigned char> bytes)
{
    PutU32(out, static_cast<uint32_t>(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

class Reader
{
public:
    explicit Reader(std::span<const unsigned char> bytes) : m_bytes{bytes} {}
    bool Need(size_t n) const { return m_pos + n <= m_bytes.size(); }
    uint8_t U8()
    {
        return m_bytes[m_pos++];
    }
    uint32_t U32()
    {
        uint32_t v{0};
        for (int i{0}; i < 4; ++i) v |= static_cast<uint32_t>(m_bytes[m_pos++]) << (8 * i);
        return v;
    }
    uint64_t U64()
    {
        uint64_t v{0};
        for (int i{0}; i < 8; ++i) v |= static_cast<uint64_t>(m_bytes[m_pos++]) << (8 * i);
        return v;
    }
    uint256 Hash()
    {
        uint256 h;
        std::memcpy(h.data(), m_bytes.data() + m_pos, 32);
        m_pos += 32;
        return h;
    }
    std::vector<unsigned char> Bytes(size_t len)
    {
        std::vector<unsigned char> out{m_bytes.begin() + static_cast<std::ptrdiff_t>(m_pos),
                                       m_bytes.begin() + static_cast<std::ptrdiff_t>(m_pos + len)};
        m_pos += len;
        return out;
    }
    bool Done() const { return m_pos == m_bytes.size(); }

private:
    std::span<const unsigned char> m_bytes;
    size_t m_pos{0};
};

Error Decode(std::string_view detail) { return Error{ErrorCode::DECODE, detail}; }

} // namespace

bool IsKnown(SyncKind kind)
{
    switch (kind) {
    case SyncKind::DECISIONS_REQUEST:
    case SyncKind::DECISIONS_REPLY:
    case SyncKind::PROPOSAL_REQUEST:
    case SyncKind::PROPOSAL_REPLY:
        return true;
    }
    return false;
}

std::string_view ToString(SyncKind kind)
{
    switch (kind) {
    case SyncKind::DECISIONS_REQUEST: return "DECISIONS_REQUEST";
    case SyncKind::DECISIONS_REPLY: return "DECISIONS_REPLY";
    case SyncKind::PROPOSAL_REQUEST: return "PROPOSAL_REQUEST";
    case SyncKind::PROPOSAL_REPLY: return "PROPOSAL_REPLY";
    }
    return "UNKNOWN";
}

SyncKind KindOf(const SyncMessage& message)
{
    return std::visit(
        [](auto&& m) -> SyncKind {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, DecisionsRequest>) return SyncKind::DECISIONS_REQUEST;
            if constexpr (std::is_same_v<T, DecisionsReply>) return SyncKind::DECISIONS_REPLY;
            if constexpr (std::is_same_v<T, ProposalRequest>) return SyncKind::PROPOSAL_REQUEST;
            return SyncKind::PROPOSAL_REPLY;
        },
        message);
}

size_t MaxSyncRecordBytes(const ProfileParams& profile) { return storage::DecisionRecord::MaxEncodedSize(profile); }

size_t MaxBulkFrameBytes(const ProfileParams& profile)
{
    // version, kind, slot, hash, length prefix, then the longest byte string of any kind.
    return 2 + 8 + 32 + 4 + std::max(MaxSyncRecordBytes(profile), agreement::MaxMessageBytes(profile));
}

Result<std::vector<unsigned char>> EncodeSync(const SyncMessage& message, const ProfileParams& profile)
{
    std::vector<unsigned char> out;
    PutU8(out, SYNC_VERSION);
    PutU8(out, static_cast<uint8_t>(KindOf(message)));
    if (const auto* r{std::get_if<DecisionsRequest>(&message)}) {
        if (r->max == 0 || r->max > MAX_SYNC_DECISIONS) return Error{ErrorCode::INVALID_INPUT, SYNC_MAX_RANGE};
        PutU64(out, r->from_slot);
        PutU8(out, r->max);
    } else if (const auto* r{std::get_if<DecisionsReply>(&message)}) {
        if (r->record.size() > MaxSyncRecordBytes(profile)) return Error{ErrorCode::OVERSIZE, SYNC_OVERSIZE};
        PutU64(out, r->slot);
        PutBytes(out, r->record);
    } else if (const auto* r{std::get_if<ProposalRequest>(&message)}) {
        PutU64(out, r->slot);
        PutHash(out, r->header_hash);
    } else {
        const auto& reply{std::get<ProposalReply>(message)};
        if (reply.proposal.size() > agreement::MaxMessageBytes(profile)) return Error{ErrorCode::OVERSIZE, SYNC_OVERSIZE};
        PutU64(out, reply.slot);
        PutHash(out, reply.header_hash);
        PutBytes(out, reply.proposal);
    }
    return Ok(std::move(out));
}

Result<SyncMessage> DecodeSync(std::span<const unsigned char> bytes, const ProfileParams& profile)
{
    if (bytes.size() > MaxBulkFrameBytes(profile)) return Error{ErrorCode::OVERSIZE, SYNC_OVERSIZE};
    Reader in{bytes};
    if (!in.Need(2)) return Decode(SYNC_TRUNCATED);
    if (in.U8() != SYNC_VERSION) return Decode(SYNC_VERSION_UNKNOWN);
    const auto kind{static_cast<SyncKind>(in.U8())};
    if (!IsKnown(kind)) return Decode(SYNC_KIND_UNKNOWN);
    SyncMessage out;
    switch (kind) {
    case SyncKind::DECISIONS_REQUEST: {
        if (!in.Need(9)) return Decode(SYNC_TRUNCATED);
        DecisionsRequest r;
        r.from_slot = in.U64();
        r.max = in.U8();
        if (r.max == 0 || r.max > MAX_SYNC_DECISIONS) return Error{ErrorCode::INVALID_INPUT, SYNC_MAX_RANGE};
        out = r;
        break;
    }
    case SyncKind::DECISIONS_REPLY: {
        if (!in.Need(12)) return Decode(SYNC_TRUNCATED);
        DecisionsReply r;
        r.slot = in.U64();
        const uint32_t len{in.U32()};
        if (len > MaxSyncRecordBytes(profile)) return Error{ErrorCode::OVERSIZE, SYNC_OVERSIZE};
        if (!in.Need(len)) return Decode(SYNC_TRUNCATED);
        r.record = in.Bytes(len);
        out = std::move(r);
        break;
    }
    case SyncKind::PROPOSAL_REQUEST: {
        if (!in.Need(40)) return Decode(SYNC_TRUNCATED);
        ProposalRequest r;
        r.slot = in.U64();
        r.header_hash = in.Hash();
        out = r;
        break;
    }
    case SyncKind::PROPOSAL_REPLY: {
        if (!in.Need(44)) return Decode(SYNC_TRUNCATED);
        ProposalReply r;
        r.slot = in.U64();
        r.header_hash = in.Hash();
        const uint32_t len{in.U32()};
        if (len > agreement::MaxMessageBytes(profile)) return Error{ErrorCode::OVERSIZE, SYNC_OVERSIZE};
        if (!in.Need(len)) return Decode(SYNC_TRUNCATED);
        r.proposal = in.Bytes(len);
        out = std::move(r);
        break;
    }
    }
    if (!in.Done()) return Decode(SYNC_TRAILING);
    return Ok(std::move(out));
}

} // namespace flowmesh2::net
