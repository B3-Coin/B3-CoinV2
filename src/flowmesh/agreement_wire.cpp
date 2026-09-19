// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/agreement_wire.h>

#include <crypto/common.h>
#include <flowmesh/production_engine.h>
#include <hash.h>

#include <algorithm>
#include <array>
#include <utility>

namespace flowmesh {
namespace {

struct Budget {
    size_t signatures{0};
    bool Add(size_t count)
    {
        if (count > PREAGREEMENT_MAX_PROOF_SIGNATURES - signatures) return false;
        signatures += count;
        return true;
    }
};

struct Writer {
    std::vector<unsigned char> bytes;
    size_t limit;
    bool Raw(std::span<const unsigned char> value)
    {
        if (value.size() > limit - bytes.size()) return false;
        bytes.insert(bytes.end(), value.begin(), value.end());
        return true;
    }
    bool U8(uint8_t value) { return Raw({&value, 1}); }
    bool U16(uint16_t value) { std::array<unsigned char, 2> b{}; WriteBE16(b.data(), value); return Raw(b); }
    bool U32(uint32_t value) { std::array<unsigned char, 4> b{}; WriteBE32(b.data(), value); return Raw(b); }
    bool U64(uint64_t value) { std::array<unsigned char, 8> b{}; WriteBE64(b.data(), value); return Raw(b); }
    bool Hash(const uint256& value) { return Raw({value.begin(), 32}); }
};

struct Reader {
    std::span<const unsigned char> bytes;
    size_t cursor{0};
    size_t Remaining() const { return bytes.size() - cursor; }
    bool Raw(std::span<unsigned char> value)
    {
        if (value.size() > Remaining()) return false;
        std::copy_n(bytes.begin() + cursor, value.size(), value.begin());
        cursor += value.size();
        return true;
    }
    bool U8(uint8_t& value) { return Raw({&value, 1}); }
    bool U16(uint16_t& value) { std::array<unsigned char, 2> b{}; if (!Raw(b)) return false; value = ReadBE16(b.data()); return true; }
    bool U32(uint32_t& value) { std::array<unsigned char, 4> b{}; if (!Raw(b)) return false; value = ReadBE32(b.data()); return true; }
    bool U64(uint64_t& value) { std::array<unsigned char, 8> b{}; if (!Raw(b)) return false; value = ReadBE64(b.data()); return true; }
    bool Hash(uint256& value) { return Raw({value.begin(), 32}); }
};

bool ContextShape(const PreagreementContext& c)
{
    return !c.domain.IsNull() && !c.market_id.IsNull() && !c.seat_set_hash.IsNull() &&
        !c.previous_state_root.IsNull() && !c.execution_config_id.IsNull() &&
        (c.sequence == 0 ? c.parent_hash.IsNull() : !c.parent_hash.IsNull());
}

bool PutContext(Writer& w, const PreagreementContext& c)
{
    return ContextShape(c) && w.Hash(c.domain) && w.Hash(c.market_id) && w.U64(c.epoch) &&
        w.Hash(c.seat_set_hash) && w.U64(c.sequence) && w.Hash(c.parent_hash) &&
        w.Hash(c.previous_state_root) && w.Hash(c.execution_config_id);
}

bool GetContext(Reader& r, PreagreementContext& c)
{
    return r.Hash(c.domain) && r.Hash(c.market_id) && r.U64(c.epoch) &&
        r.Hash(c.seat_set_hash) && r.U64(c.sequence) && r.Hash(c.parent_hash) &&
        r.Hash(c.previous_state_root) && r.Hash(c.execution_config_id) && ContextShape(c);
}

bool PutVotes(Writer& w, const std::vector<IndexedBlsSignature>& votes, Budget& budget)
{
    if (votes.empty() || !budget.Add(votes.size()) || !w.U32(votes.size())) return false;
    for (size_t i{0}; i < votes.size(); ++i) {
        const auto& vote{votes[i]};
        const auto bytes{vote.signature.Compressed()};
        if (vote.seat_index >= FLOWMESH_MAX_ACTIVE_FN_SEATS ||
            (i && votes[i - 1].seat_index >= vote.seat_index) ||
            !bls::Signature::Decode(bytes) || !w.U32(vote.seat_index) || !w.Raw(bytes)) return false;
    }
    return true;
}

bool GetVotes(Reader& r, std::vector<IndexedBlsSignature>& votes, Budget& budget)
{
    uint32_t count{0};
    if (!r.U32(count) || count == 0 || count > r.Remaining() / (4 + bls::SIGNATURE_SIZE) ||
        !budget.Add(count)) return false;
    votes.reserve(count); // Count and full byte lower bound checked before allocation.
    for (uint32_t i{0}; i < count; ++i) {
        uint32_t seat{0};
        std::array<unsigned char, bls::SIGNATURE_SIZE> bytes{};
        if (!r.U32(seat) || seat >= FLOWMESH_MAX_ACTIVE_FN_SEATS ||
            (i && votes.back().seat_index >= seat) || !r.Raw(bytes)) return false;
        const auto signature{bls::Signature::Decode(bytes)};
        if (!signature) return false;
        votes.push_back({seat, *signature});
    }
    return true;
}

bool PutPrepared(Writer& w, const PreagreementPreparedCertificate& p, Budget& b)
{
    return !p.candidate.IsNull() && w.U32(p.view) && w.Hash(p.candidate) && PutVotes(w, p.votes, b);
}
bool GetPrepared(Reader& r, PreagreementPreparedCertificate& p, Budget& b)
{
    return r.U32(p.view) && r.Hash(p.candidate) && !p.candidate.IsNull() && GetVotes(r, p.votes, b);
}

template <typename T, typename Put>
bool PutOptional(Writer& w, const std::optional<T>& value, Budget& b, Put put)
{
    return w.U8(value.has_value()) && (!value || put(w, *value, b));
}
template <typename T, typename Get>
bool GetOptional(Reader& r, std::optional<T>& value, Budget& b, Get get)
{
    uint8_t present{0};
    if (!r.U8(present) || present > 1) return false;
    if (present) { value.emplace(); return get(r, *value, b); }
    return true;
}

bool PutReport(Writer& w, const PreagreementViewChange& p, Budget& b)
{
    return p.view != 0 && p.seat_index < FLOWMESH_MAX_ACTIVE_FN_SEATS &&
        (!p.prepared || p.prepared->view < p.view) && b.Add(1) &&
        w.U32(p.view) && w.U32(p.seat_index) && PutOptional(w, p.prepared, b, PutPrepared) && w.Raw(p.signature);
}
bool GetReport(Reader& r, PreagreementViewChange& p, Budget& b)
{
    return b.Add(1) && r.U32(p.view) && p.view != 0 && r.U32(p.seat_index) &&
        p.seat_index < FLOWMESH_MAX_ACTIVE_FN_SEATS && GetOptional(r, p.prepared, b, GetPrepared) &&
        (!p.prepared || p.prepared->view < p.view) && r.Raw(p.signature);
}

bool PutNewView(Writer& w, const PreagreementNewViewProof& p, Budget& b)
{
    if (p.view == 0 || p.candidate.IsNull() || p.reports.empty() ||
        p.reports.size() > PREAGREEMENT_MAX_PROOF_SIGNATURES - b.signatures ||
        !w.U32(p.view) || !w.Hash(p.candidate) || !w.U32(p.reports.size())) return false;
    for (size_t i{0}; i < p.reports.size(); ++i) {
        if (p.reports[i].view != p.view || (i && p.reports[i - 1].seat_index >= p.reports[i].seat_index) ||
            !PutReport(w, p.reports[i], b)) return false;
    }
    return true;
}
bool GetNewView(Reader& r, PreagreementNewViewProof& p, Budget& b)
{
    uint32_t count{0};
    if (!r.U32(p.view) || p.view == 0 || !r.Hash(p.candidate) || p.candidate.IsNull() ||
        !r.U32(count) || count == 0 || count > r.Remaining() / (9 + bls::SIGNATURE_SIZE) ||
        count > PREAGREEMENT_MAX_PROOF_SIGNATURES - b.signatures) return false;
    p.reports.reserve(count);
    for (uint32_t i{0}; i < count; ++i) {
        PreagreementViewChange report;
        if (!GetReport(r, report, b) || report.view != p.view ||
            (i && p.reports.back().seat_index >= report.seat_index)) return false;
        p.reports.push_back(std::move(report));
    }
    return true;
}

bool CommitShape(const PreagreementCommitCertificate& p)
{
    return p.prepared.view == 0 ? !p.new_view :
        p.new_view && p.new_view->view == p.prepared.view && p.new_view->candidate == p.prepared.candidate;
}
bool PutCommit(Writer& w, const PreagreementCommitCertificate& p, Budget& b)
{
    return CommitShape(p) && PutPrepared(w, p.prepared, b) && PutVotes(w, p.commits, b) &&
        PutOptional(w, p.new_view, b, PutNewView);
}
bool GetCommit(Reader& r, PreagreementCommitCertificate& p, Budget& b)
{
    return GetPrepared(r, p.prepared, b) && GetVotes(r, p.commits, b) &&
        GetOptional(r, p.new_view, b, GetNewView) && CommitShape(p);
}

bool EntryMatches(const AgreementMessage& m)
{
    if (m.entry_bytes.empty() || m.entry_bytes.size() > FLOWMESH_V1_MAX_MICROBLOCK_BYTES) return false;
    const auto entry{DecodeProductionEntry(m.entry_bytes)};
    if (!entry || entry->GetHash() != m.candidate || entry->domain != m.context.domain ||
        entry->market_id != m.context.market_id || entry->epoch != m.context.epoch ||
        entry->seat_set_hash != m.context.seat_set_hash || entry->sequence != m.context.sequence ||
        entry->parent_hash != m.context.parent_hash || entry->previous_state_root != m.context.previous_state_root) return false;
    const auto canonical{EncodeProductionEntry(*entry)};
    return canonical && *canonical == m.entry_bytes;
}

bool MessageShape(const AgreementMessage& m)
{
    if (!ContextShape(m.context)) return false;
    if (m.stage != AgreementStage::DECISION && m.seat_index >= FLOWMESH_MAX_ACTIVE_FN_SEATS) return false;
    const auto prepared_matches = [&] { return !m.prepared ||
        (m.prepared->view == m.view && m.prepared->candidate == m.candidate); };
    const auto new_view_matches = [&] { return !m.new_view ||
        (m.view != 0 && m.new_view->view == m.view && m.new_view->candidate == m.candidate); };
    switch (m.stage) {
    case AgreementStage::PROPOSAL:
        return !m.prepared && !m.decision && (m.view == 0 ? !m.new_view : m.new_view.has_value()) &&
            new_view_matches() && EntryMatches(m);
    case AgreementStage::PREPARE:
        return !m.candidate.IsNull() && m.entry_bytes.empty() && !m.prepared && !m.new_view && !m.decision;
    case AgreementStage::COMMIT:
        return !m.candidate.IsNull() && m.entry_bytes.empty() && !m.decision && prepared_matches() &&
            new_view_matches() && (!m.new_view || m.prepared.has_value());
    case AgreementStage::VIEW_CHANGE:
        return m.view != 0 && m.entry_bytes.empty() && !m.new_view && !m.decision &&
            (m.prepared ? m.prepared->view < m.view && m.candidate == m.prepared->candidate : m.candidate.IsNull());
    case AgreementStage::DECISION:
        return m.seat_index == AGREEMENT_NO_SEAT &&
            std::all_of(m.signature.begin(), m.signature.end(), [](unsigned char c) { return c == 0; }) &&
            !m.prepared && !m.new_view && m.decision && m.decision->prepared.view == m.view &&
            m.decision->prepared.candidate == m.candidate && EntryMatches(m);
    }
    return false;
}

bool PutEntry(Writer& w, const std::vector<unsigned char>& entry)
{
    return entry.size() <= FLOWMESH_V1_MAX_MICROBLOCK_BYTES && w.U32(entry.size()) && w.Raw(entry);
}
bool GetEntry(Reader& r, std::vector<unsigned char>& entry)
{
    uint32_t size{0};
    if (!r.U32(size) || size == 0 || size > FLOWMESH_V1_MAX_MICROBLOCK_BYTES || size > r.Remaining()) return false;
    entry.assign(r.bytes.begin() + r.cursor, r.bytes.begin() + r.cursor + size);
    r.cursor += size;
    return true;
}

template <typename T, typename Put>
std::optional<std::vector<unsigned char>> EncodeProof(const T& p, Put put)
{
    Writer w{{}, AGREEMENT_MAX_PROOF_BYTES};
    Budget b;
    if (!w.U16(AGREEMENT_WIRE_VERSION) || !put(w, p, b)) return std::nullopt;
    return std::move(w.bytes);
}
template <typename T, typename Get>
std::optional<T> DecodeProof(std::span<const unsigned char> bytes, Get get)
{
    if (bytes.size() > AGREEMENT_MAX_PROOF_BYTES) return std::nullopt;
    Reader r{bytes};
    uint16_t version{0};
    Budget b;
    T p;
    if (!r.U16(version) || version != AGREEMENT_WIRE_VERSION || !get(r, p, b) || r.Remaining()) return std::nullopt;
    return p;
}

} // namespace

std::optional<std::vector<unsigned char>> EncodePreagreementPrepared(const PreagreementPreparedCertificate& p) { return EncodeProof(p, PutPrepared); }
std::optional<PreagreementPreparedCertificate> DecodePreagreementPrepared(std::span<const unsigned char> b) { return DecodeProof<PreagreementPreparedCertificate>(b, GetPrepared); }
std::optional<std::vector<unsigned char>> EncodePreagreementViewChange(const PreagreementViewChange& p) { return EncodeProof(p, PutReport); }
std::optional<PreagreementViewChange> DecodePreagreementViewChange(std::span<const unsigned char> b) { return DecodeProof<PreagreementViewChange>(b, GetReport); }
std::optional<std::vector<unsigned char>> EncodePreagreementNewView(const PreagreementNewViewProof& p) { return EncodeProof(p, PutNewView); }
std::optional<PreagreementNewViewProof> DecodePreagreementNewView(std::span<const unsigned char> b) { return DecodeProof<PreagreementNewViewProof>(b, GetNewView); }
std::optional<std::vector<unsigned char>> EncodePreagreementCommit(const PreagreementCommitCertificate& p) { return EncodeProof(p, PutCommit); }
std::optional<PreagreementCommitCertificate> DecodePreagreementCommit(std::span<const unsigned char> b) { return DecodeProof<PreagreementCommitCertificate>(b, GetCommit); }

std::optional<std::vector<unsigned char>> EncodeAgreementMessage(const AgreementMessage& m)
{
    if (!MessageShape(m)) return std::nullopt;
    Writer w{{}, AGREEMENT_MAX_BYTES};
    Budget b;
    if (!w.U16(AGREEMENT_WIRE_VERSION) || !w.U8(static_cast<uint8_t>(m.stage)) || !PutContext(w, m.context) ||
        !w.U32(m.view) || !w.Hash(m.candidate) || !w.U32(m.seat_index) || !w.Raw(m.signature)) return std::nullopt;
    bool ok{false};
    switch (m.stage) {
    case AgreementStage::PROPOSAL: ok = b.Add(1) && PutEntry(w, m.entry_bytes) && PutOptional(w, m.new_view, b, PutNewView); break;
    case AgreementStage::PREPARE: ok = b.Add(1); break;
    case AgreementStage::COMMIT: ok = b.Add(1) && PutOptional(w, m.prepared, b, PutPrepared) && PutOptional(w, m.new_view, b, PutNewView); break;
    case AgreementStage::VIEW_CHANGE: ok = b.Add(1) && PutOptional(w, m.prepared, b, PutPrepared); break;
    case AgreementStage::DECISION: ok = PutEntry(w, m.entry_bytes) && PutCommit(w, *m.decision, b); break;
    }
    return ok ? std::optional{std::move(w.bytes)} : std::nullopt;
}

std::optional<AgreementMessage> DecodeAgreementMessage(std::span<const unsigned char> bytes)
{
    if (bytes.size() > AGREEMENT_MAX_BYTES) return std::nullopt;
    Reader r{bytes};
    Budget b;
    uint16_t version{0};
    uint8_t stage{0};
    AgreementMessage m;
    if (!r.U16(version) || version != AGREEMENT_WIRE_VERSION || !r.U8(stage) || stage < 1 || stage > 5 ||
        !GetContext(r, m.context) || !r.U32(m.view) || !r.Hash(m.candidate) || !r.U32(m.seat_index) || !r.Raw(m.signature)) return std::nullopt;
    m.stage = static_cast<AgreementStage>(stage);
    bool ok{false};
    switch (m.stage) {
    case AgreementStage::PROPOSAL: ok = b.Add(1) && GetEntry(r, m.entry_bytes) && GetOptional(r, m.new_view, b, GetNewView); break;
    case AgreementStage::PREPARE: ok = b.Add(1); break;
    case AgreementStage::COMMIT: ok = b.Add(1) && GetOptional(r, m.prepared, b, GetPrepared) && GetOptional(r, m.new_view, b, GetNewView); break;
    case AgreementStage::VIEW_CHANGE: ok = b.Add(1) && GetOptional(r, m.prepared, b, GetPrepared); break;
    case AgreementStage::DECISION: m.decision.emplace(); ok = GetEntry(r, m.entry_bytes) && GetCommit(r, *m.decision, b); break;
    }
    if (!ok || r.Remaining() || !MessageShape(m)) return std::nullopt;
    return m;
}

std::optional<uint256> AgreementMessageDigest(const AgreementMessage& m)
{
    if (!MessageShape(m)) return std::nullopt;
    // Direct callers (including durable intent construction) receive the same
    // canonical shape/budget protection as a message decoded from the wire.
    if (m.stage != AgreementStage::PROPOSAL && !EncodeAgreementMessage(m)) return std::nullopt;
    switch (m.stage) {
    case AgreementStage::PREPARE:
        return PreagreementVoteDigest(m.context, PreagreementPhase::PREPARE, m.view, m.candidate, m.seat_index);
    case AgreementStage::COMMIT:
        return PreagreementVoteDigest(m.context, PreagreementPhase::COMMIT, m.view, m.candidate, m.seat_index);
    case AgreementStage::VIEW_CHANGE:
        return PreagreementViewChangeDigest(m.context, {m.view, m.seat_index, m.prepared, m.signature});
    case AgreementStage::PROPOSAL: {
        AgreementMessage unsigned_message{m};
        unsigned_message.signature.fill(0);
        const auto bytes{EncodeAgreementMessage(unsigned_message)};
        if (!bytes) return std::nullopt;
        HashWriter writer{TaggedHash("B3/FLOWMESH/PREAGREE/PROPOSAL/V1")};
        writer << std::span<const unsigned char>{*bytes};
        return writer.GetSHA256();
    }
    case AgreementStage::DECISION: return std::nullopt;
    }
    return std::nullopt;
}

bool CheckAgreementMessageSignature(const AgreementMessage& m, const ActiveFnBlsSeatSet& seats)
{
    if (m.stage == AgreementStage::DECISION || m.seat_index >= seats.Size() ||
        m.context.market_id != seats.market_id || m.context.epoch != seats.epoch ||
        m.context.seat_set_hash != seats.set_hash || CheckActiveFnBlsSeatSet(m.context.domain, seats) != BlsSeatSetCheck::OK ||
        (m.stage == AgreementStage::PROPOSAL && m.seat_index != ProductionProposerSeatIndex(m.context.sequence, m.view, seats.Size()))) return false;
    const auto digest{AgreementMessageDigest(m)};
    const auto signature{bls::Signature::Decode(m.signature)};
    return digest && signature && bls::Verify(seats.members[m.seat_index].key.Key(),
        std::span<const unsigned char>{digest->begin(), 32}, *signature);
}

} // namespace flowmesh
