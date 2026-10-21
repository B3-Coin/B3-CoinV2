// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/agreement/messages.h>

#include <flowmesh2/core/codec.h>
#include <flowmesh2/storage/commit_log.h>
#include <serialize.h>
#include <streams.h>

#include <algorithm>
#include <ios>
#include <type_traits>
#include <utility>

namespace flowmesh2::agreement {

bool IsKnown(MessageType type)
{
    switch (type) {
    case MessageType::PROPOSAL:
    case MessageType::VOTE:
    case MessageType::VIEW_CHANGE:
    case MessageType::NEW_VIEW:
    case MessageType::EVIDENCE:
    case MessageType::CERTIFIED_PROPOSAL:
    case MessageType::CONTROL_REPORT:
    case MessageType::CONTROL_NEW_VIEW:
    case MessageType::OFFER:
        return true;
    }
    return false;
}

bool IsKnown(EvidenceKind kind)
{
    switch (kind) {
    case EvidenceKind::DOUBLE_VOTE:
    case EvidenceKind::CONFLICTING_CERTIFICATES:
        return true;
    }
    return false;
}

std::string_view ToString(MessageType type)
{
    switch (type) {
    case MessageType::PROPOSAL: return "PROPOSAL";
    case MessageType::VOTE: return "VOTE";
    case MessageType::VIEW_CHANGE: return "VIEW_CHANGE";
    case MessageType::NEW_VIEW: return "NEW_VIEW";
    case MessageType::EVIDENCE: return "EVIDENCE";
    case MessageType::CERTIFIED_PROPOSAL: return "CERTIFIED_PROPOSAL";
    case MessageType::CONTROL_REPORT: return "CONTROL_REPORT";
    case MessageType::CONTROL_NEW_VIEW: return "CONTROL_NEW_VIEW";
    case MessageType::OFFER: return "OFFER";
    }
    return "UNKNOWN";
}

std::string_view ToString(EvidenceKind kind)
{
    switch (kind) {
    case EvidenceKind::DOUBLE_VOTE: return "DOUBLE_VOTE";
    case EvidenceKind::CONFLICTING_CERTIFICATES: return "CONFLICTING_CERTIFICATES";
    }
    return "UNKNOWN";
}

EvidenceKind Evidence::Kind() const
{
    return std::holds_alternative<DoubleVote>(content) ? EvidenceKind::DOUBLE_VOTE : EvidenceKind::CONFLICTING_CERTIFICATES;
}

MessageType TypeOf(const Message& message)
{
    return std::visit(
        [](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, Proposal>) {
                return MessageType::PROPOSAL;
            } else if constexpr (std::is_same_v<T, Vote>) {
                return MessageType::VOTE;
            } else if constexpr (std::is_same_v<T, ViewChange>) {
                return MessageType::VIEW_CHANGE;
            } else if constexpr (std::is_same_v<T, NewView>) {
                return MessageType::NEW_VIEW;
            } else if constexpr (std::is_same_v<T, CertifiedProposal>) {
                return MessageType::CERTIFIED_PROPOSAL;
            } else if constexpr (std::is_same_v<T, ControlReport>) {
                return MessageType::CONTROL_REPORT;
            } else if constexpr (std::is_same_v<T, ControlNewView>) {
                return MessageType::CONTROL_NEW_VIEW;
            } else if constexpr (std::is_same_v<T, OfferedProposal>) {
                return MessageType::OFFER;
            } else {
                static_assert(std::is_same_v<T, Evidence>, "every Message alternative has a type");
                return MessageType::EVIDENCE;
            }
        },
        message);
}

namespace {

// ---- sizes -------------------------------------------------------------

constexpr size_t SIG_BYTES{bls::SIGNATURE_SIZE};
constexpr size_t CERT_MAX{SlotCertificate::MAX_ENCODED_BYTES};
//! phase, epoch, view, slot, header_hash, seat, signature.
constexpr size_t VOTE_FIXED_BYTES{1 + 8 + 8 + 8 + 32 + 4 + SIG_BYTES};
constexpr size_t VOTE_MAX_BYTES{VOTE_FIXED_BYTES + 1 + MAX_VOTE_INCLUSION_IDS * SHORT_ID_BYTES};
//! epoch, slot, new_view, seat, has_prepared, [view, cert], signature.
constexpr size_t VIEW_CHANGE_MAX_BYTES{8 + 8 + 8 + 4 + 1 + 8 + 3 + CERT_MAX + SIG_BYTES};
constexpr size_t NEW_VIEW_MAX_BYTES{8 + 8 + 8 + 4 + 32 + 3 + MAX_NEW_VIEW_REPORTS * (3 + VIEW_CHANGE_MAX_BYTES) + SIG_BYTES};
constexpr size_t CERTIFIED_HEADER_MAX_BYTES{SLOT_HEADER_BYTES + 3 + CERT_MAX};
constexpr size_t CONTROL_REPORT_MAX_BYTES{storage::CONTROL_KEY_BYTES + 8 + 1 + 8 + CERTIFIED_HEADER_MAX_BYTES + 1 + CERTIFIED_HEADER_MAX_BYTES + SIG_BYTES};
constexpr size_t CONTROL_NEW_VIEW_MAX_BYTES{storage::CONTROL_KEY_BYTES + 32 + 1 + MAX_CONTROL_REPORTS * (3 + CONTROL_REPORT_MAX_BYTES) + SIG_BYTES};
constexpr size_t EVIDENCE_MAX_BYTES{1 + std::max(2 * (3 + VOTE_MAX_BYTES), 2 * CERTIFIED_HEADER_MAX_BYTES)};
static_assert(CERT_MAX <= 0xffff && VIEW_CHANGE_MAX_BYTES <= 0xffff && VOTE_MAX_BYTES <= 0xffff,
              "every nested length fits a 3-byte CompactSize");

size_t ProposalMaxBytes(const ProfileParams& profile)
{
    return 8 + 8 + SLOT_HEADER_BYTES + 3 + CERT_MAX + SlotBody::MaxEncodedSize(profile);
}

//! Longest payload of one message type.
size_t MaxPayloadBytes(MessageType type, const ProfileParams& profile)
{
    switch (type) {
    case MessageType::PROPOSAL: return ProposalMaxBytes(profile);
    case MessageType::VOTE: return VOTE_MAX_BYTES;
    case MessageType::VIEW_CHANGE: return VIEW_CHANGE_MAX_BYTES;
    case MessageType::NEW_VIEW: return NEW_VIEW_MAX_BYTES;
    case MessageType::EVIDENCE: return EVIDENCE_MAX_BYTES;
    case MessageType::CERTIFIED_PROPOSAL: return 3 + CERT_MAX + ProposalMaxBytes(profile);
    case MessageType::CONTROL_REPORT: return CONTROL_REPORT_MAX_BYTES;
    case MessageType::CONTROL_NEW_VIEW: return CONTROL_NEW_VIEW_MAX_BYTES + 1 + 5 + ProposalMaxBytes(profile);
    case MessageType::OFFER: return ProposalMaxBytes(profile);
    }
    return 0;
}

// ---- encoding ------------------------------------------------------------

void PutCompactSize(std::vector<unsigned char>& out, uint64_t n)
{
    VectorWriter writer{out, out.size()};
    WriteCompactSize(writer, n);
}

template <class... Args>
void Put(std::vector<unsigned char>& out, const Args&... args)
{
    VectorWriter{out, out.size(), args...};
}

void PutBytes(std::vector<unsigned char>& out, std::span<const unsigned char> bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); }

void PutLengthPrefixed(std::vector<unsigned char>& out, std::span<const unsigned char> bytes)
{
    PutCompactSize(out, bytes.size());
    PutBytes(out, bytes);
}

void EncodePayload(std::vector<unsigned char>& out, const Proposal& p)
{
    Put(out, p.epoch, p.view);
    PutBytes(out, p.header.Encode());
    if (p.parent_cert.has_value()) {
        PutLengthPrefixed(out, p.parent_cert->Encode());
    } else {
        PutCompactSize(out, 0);
    }
    PutBytes(out, p.body.Encode());
}

void EncodePayload(std::vector<unsigned char>& out, const Vote& v)
{
    Put(out, static_cast<uint8_t>(v.phase), v.epoch, v.view, v.slot, v.header_hash, v.seat);
    PutBytes(out, v.signature);
    PutCompactSize(out, v.inclusion.size());
    for (const ShortId& id : v.inclusion) PutBytes(out, id);
}

void EncodePayload(std::vector<unsigned char>& out, const ViewChange& vc)
{
    Put(out, vc.epoch, vc.slot, vc.new_view, vc.seat, static_cast<uint8_t>(vc.prepared.has_value() ? 1 : 0));
    if (vc.prepared.has_value()) {
        Put(out, vc.prepared->view);
        PutLengthPrefixed(out, vc.prepared->cert.Encode());
    }
    PutBytes(out, vc.signature);
}

void EncodePayload(std::vector<unsigned char>& out, const NewView& nv)
{
    Put(out, nv.epoch, nv.slot, nv.view, nv.seat, nv.selected);
    PutCompactSize(out, nv.reports.size());
    for (const ViewChange& report : nv.reports) {
        std::vector<unsigned char> bytes;
        EncodePayload(bytes, report);
        PutLengthPrefixed(out, bytes);
    }
    PutBytes(out, nv.signature);
}

void EncodeCertifiedHeader(std::vector<unsigned char>& out, const CertifiedHeader& ch)
{
    PutBytes(out, ch.header.Encode());
    PutLengthPrefixed(out, ch.cert.Encode());
}

void EncodePayload(std::vector<unsigned char>& out, const Evidence& e)
{
    Put(out, static_cast<uint8_t>(e.Kind()));
    if (const auto* dv{std::get_if<DoubleVote>(&e.content)}) {
        for (const Vote* vote : {&dv->first, &dv->second}) {
            std::vector<unsigned char> bytes;
            EncodePayload(bytes, *vote);
            PutLengthPrefixed(out, bytes);
        }
    } else {
        const auto& cc{std::get<ConflictingCertificates>(e.content)};
        EncodeCertifiedHeader(out, cc.first);
        EncodeCertifiedHeader(out, cc.second);
    }
}

void EncodePayload(std::vector<unsigned char>& out, const CertifiedProposal& final)
{
    PutLengthPrefixed(out, final.certificate.Encode());
    EncodePayload(out, final.proposal);
}

void EncodePayload(std::vector<unsigned char>& out, const ControlReport& report)
{
    Put(out, report.key, report.prepared_watermark, static_cast<uint8_t>(report.prepared ? 1 : 0));
    if (report.prepared) {
        Put(out, report.prepared->view);
        EncodeCertifiedHeader(out, CertifiedHeader{report.prepared->header, report.prepared->certificate});
    }
    Put(out, static_cast<uint8_t>(report.decision ? 1 : 0));
    if (report.decision) EncodeCertifiedHeader(out, *report.decision);
    PutBytes(out, report.signature);
}
void EncodePayload(std::vector<unsigned char>& out, const ControlNewView& view)
{
    Put(out, view.key, view.selected);
    PutCompactSize(out, view.reports.size());
    for (const auto& report : view.reports) {
        std::vector<unsigned char> bytes;
        EncodePayload(bytes, report);
        PutLengthPrefixed(out, bytes);
    }
    Put(out, static_cast<uint8_t>(view.selected_proposal ? 1 : 0));
    if (view.selected_proposal) {
        std::vector<unsigned char> body;
        EncodePayload(body, *view.selected_proposal);
        PutLengthPrefixed(out, body);
    }
    PutBytes(out, view.signature);
}
void EncodePayload(std::vector<unsigned char>& out, const OfferedProposal& offer) { EncodePayload(out, offer.proposal); }

// ---- decoding ------------------------------------------------------------

//! Thrown inside the decoders to return a nested decoder's typed error.
struct NestedError {
    Error error;
};

template <class T>
T Unwrap(Result<T>&& result)
{
    if (!result.has_value()) throw NestedError{result.error()};
    return std::move(result).value();
}

//! A reader that can also hand out sub-spans of its input without copying.
class Parser
{
public:
    explicit Parser(std::span<const unsigned char> bytes) : m_bytes{bytes}, m_reader{bytes} {}

    template <class T>
    T Read()
    {
        T value{};
        m_reader >> value;
        return value;
    }
    //! A canonical CompactSize count, refused above `cap` before any allocation.
    uint64_t Count(size_t cap)
    {
        const uint64_t count{ReadCompactSize(m_reader, /*range_check=*/false)};
        if (count > cap) throw CapExceeded{"count exceeds its cap"};
        return count;
    }
    std::span<const unsigned char> Take(size_t n)
    {
        const size_t pos{m_bytes.size() - m_reader.size()};
        m_reader.ignore(n);
        return m_bytes.subspan(pos, n);
    }
    //! A CompactSize length (at most `cap`) and that many bytes.
    std::span<const unsigned char> TakeLengthPrefixed(size_t cap) { return Take(static_cast<size_t>(Count(cap))); }
    std::span<const unsigned char> Rest() { return Take(m_reader.size()); }
    void Finish() const
    {
        if (!m_reader.empty()) throw std::ios_base::failure("trailing bytes");
    }

private:
    std::span<const unsigned char> m_bytes;
    SpanReader m_reader;
};

storage::VoteSignature ReadSignature(Parser& parser)
{
    storage::VoteSignature signature{};
    const std::span<const unsigned char> bytes{parser.Take(SIG_BYTES)};
    std::copy(bytes.begin(), bytes.end(), signature.begin());
    return signature;
}

uint32_t ReadSeat(Parser& parser)
{
    const uint32_t seat{parser.Read<uint32_t>()};
    if (seat >= MAX_SEATS) throw std::ios_base::failure("seat index at or above MAX_SEATS");
    return seat;
}

SlotCertificate ReadCertificate(Parser& parser)
{
    return Unwrap(SlotCertificate::Decode(parser.TakeLengthPrefixed(CERT_MAX)));
}

Proposal DecodeProposal(Parser& parser, const ProfileParams& profile)
{
    Proposal p;
    p.epoch = parser.Read<uint64_t>();
    p.view = parser.Read<uint64_t>();
    p.header = Unwrap(SlotHeaderV2::Decode(parser.Take(SLOT_HEADER_BYTES)));
    const std::span<const unsigned char> cert{parser.TakeLengthPrefixed(CERT_MAX)};
    if (!cert.empty()) p.parent_cert = Unwrap(SlotCertificate::Decode(cert));
    p.body = Unwrap(SlotBody::Decode(parser.Rest(), profile));
    return p;
}

Vote DecodeVote(Parser& parser)
{
    Vote v;
    const uint8_t phase{parser.Read<uint8_t>()};
    v.phase = static_cast<storage::VotePhase>(phase);
    if (!storage::IsKnown(v.phase)) throw std::ios_base::failure("unknown vote phase");
    v.epoch = parser.Read<uint64_t>();
    v.view = parser.Read<uint64_t>();
    v.slot = parser.Read<uint64_t>();
    v.header_hash = parser.Read<uint256>();
    v.seat = ReadSeat(parser);
    v.signature = ReadSignature(parser);
    const uint64_t count{parser.Count(MAX_VOTE_INCLUSION_IDS)};
    v.inclusion.reserve(static_cast<size_t>(count));
    for (uint64_t i{0}; i < count; ++i) {
        ShortId id{};
        const std::span<const unsigned char> bytes{parser.Take(SHORT_ID_BYTES)};
        std::copy(bytes.begin(), bytes.end(), id.begin());
        v.inclusion.push_back(id);
    }
    return v;
}

ViewChange DecodeViewChange(Parser& parser)
{
    ViewChange vc;
    vc.epoch = parser.Read<uint64_t>();
    vc.slot = parser.Read<uint64_t>();
    vc.new_view = parser.Read<uint64_t>();
    vc.seat = ReadSeat(parser);
    const uint8_t has_prepared{parser.Read<uint8_t>()};
    if (has_prepared > 1) throw std::ios_base::failure("has_prepared is not 0 or 1");
    if (has_prepared == 1) {
        PreparedCert prepared;
        prepared.view = parser.Read<uint64_t>();
        prepared.cert = ReadCertificate(parser);
        vc.prepared = std::move(prepared);
    }
    vc.signature = ReadSignature(parser);
    return vc;
}

//! A nested payload: exactly the length-prefixed bytes, decoded by `decode`.
template <class Decode>
auto DecodeNested(Parser& parser, size_t cap, Decode&& decode)
{
    Parser inner{parser.TakeLengthPrefixed(cap)};
    auto value{decode(inner)};
    inner.Finish();
    return value;
}

NewView DecodeNewView(Parser& parser)
{
    NewView nv;
    nv.epoch = parser.Read<uint64_t>();
    nv.slot = parser.Read<uint64_t>();
    nv.view = parser.Read<uint64_t>();
    nv.seat = ReadSeat(parser);
    nv.selected = parser.Read<uint256>();
    const uint64_t count{parser.Count(MAX_NEW_VIEW_REPORTS)};
    nv.reports.reserve(static_cast<size_t>(count));
    for (uint64_t i{0}; i < count; ++i) {
        nv.reports.push_back(DecodeNested(parser, VIEW_CHANGE_MAX_BYTES, [](Parser& p) { return DecodeViewChange(p); }));
    }
    nv.signature = ReadSignature(parser);
    return nv;
}

CertifiedHeader DecodeCertifiedHeader(Parser& parser)
{
    CertifiedHeader ch;
    ch.header = Unwrap(SlotHeaderV2::Decode(parser.Take(SLOT_HEADER_BYTES)));
    ch.cert = ReadCertificate(parser);
    return ch;
}

Evidence DecodeEvidence(Parser& parser)
{
    const auto kind{static_cast<EvidenceKind>(parser.Read<uint8_t>())};
    if (!IsKnown(kind)) throw std::ios_base::failure("unknown evidence kind");
    if (kind == EvidenceKind::DOUBLE_VOTE) {
        DoubleVote dv;
        dv.first = DecodeNested(parser, VOTE_MAX_BYTES, [](Parser& p) { return DecodeVote(p); });
        dv.second = DecodeNested(parser, VOTE_MAX_BYTES, [](Parser& p) { return DecodeVote(p); });
        return Evidence{std::move(dv)};
    }
    ConflictingCertificates cc;
    cc.first = DecodeCertifiedHeader(parser);
    cc.second = DecodeCertifiedHeader(parser);
    return Evidence{std::move(cc)};
}

ControlReport DecodeControlReport(Parser& parser)
{
    ControlReport report;
    report.key = parser.Read<storage::ControlKey>();
    report.prepared_watermark = parser.Read<uint64_t>();
    const auto has_prepared{parser.Read<uint8_t>()};
    if (has_prepared > 1) throw std::ios_base::failure("control prepared flag");
    if (has_prepared) {
        const auto view{parser.Read<uint64_t>()};
        auto certified{DecodeCertifiedHeader(parser)};
        report.prepared = ControlPrepared{view, std::move(certified.header), std::move(certified.cert)};
    }
    const auto has_decision{parser.Read<uint8_t>()};
    if (has_decision > 1) throw std::ios_base::failure("control decision flag");
    if (has_decision) report.decision = DecodeCertifiedHeader(parser);
    report.signature = ReadSignature(parser);
    return report;
}
ControlNewView DecodeControlNewView(Parser& parser, const ProfileParams& profile)
{
    ControlNewView view;
    view.key = parser.Read<storage::ControlKey>();
    view.selected = parser.Read<uint256>();
    const auto count{parser.Count(MAX_CONTROL_REPORTS)};
    for (uint64_t i{0}; i < count; ++i) view.reports.push_back(DecodeNested(parser, CONTROL_REPORT_MAX_BYTES, DecodeControlReport));
    const auto has_body{parser.Read<uint8_t>()};
    if (has_body > 1) throw std::ios_base::failure("control selected proposal flag");
    if (has_body) view.selected_proposal = DecodeNested(parser, ProposalMaxBytes(profile), [&](Parser& inner) { return DecodeProposal(inner, profile); });
    view.signature = ReadSignature(parser);
    return view;
}

Message DecodePayload(MessageType type, Parser& parser, const ProfileParams& profile)
{
    switch (type) {
    case MessageType::PROPOSAL: return DecodeProposal(parser, profile);
    case MessageType::VOTE: return DecodeVote(parser);
    case MessageType::VIEW_CHANGE: return DecodeViewChange(parser);
    case MessageType::NEW_VIEW: return DecodeNewView(parser);
    case MessageType::EVIDENCE: return DecodeEvidence(parser);
    case MessageType::CONTROL_REPORT: return DecodeControlReport(parser);
    case MessageType::CONTROL_NEW_VIEW: return DecodeControlNewView(parser, profile);
    case MessageType::OFFER: return OfferedProposal{DecodeProposal(parser, profile)};
    case MessageType::CERTIFIED_PROPOSAL: {
        SlotCertificate certificate{ReadCertificate(parser)};
        return CertifiedProposal{DecodeProposal(parser, profile), std::move(certificate)};
    }
    }
    throw std::ios_base::failure("unknown message type"); // unreachable: the type was checked
}

Error Invalid(std::string_view detail) { return Error{ErrorCode::INVALID_INPUT, detail}; }

} // namespace

size_t MaxMessageBytes(const ProfileParams& profile)
{
    size_t longest{0};
    for (const MessageType type : {MessageType::PROPOSAL, MessageType::VOTE, MessageType::VIEW_CHANGE, MessageType::NEW_VIEW,
                                   MessageType::EVIDENCE, MessageType::CERTIFIED_PROPOSAL, MessageType::CONTROL_REPORT, MessageType::CONTROL_NEW_VIEW, MessageType::OFFER}) {
        longest = std::max(longest, MaxPayloadBytes(type, profile));
    }
    return 2 + longest;
}

Result<Message> DecodeMessage(std::span<const unsigned char> bytes, const ProfileParams& profile)
{
    if (bytes.size() > MaxMessageBytes(profile)) return Error{ErrorCode::OVERSIZE, "message longer than its maximum encoding"};
    if (bytes.size() < 2) return Error{ErrorCode::DECODE, "message shorter than its envelope"};
    if (bytes[0] != MESSAGE_VERSION) return Error{ErrorCode::DECODE, MESSAGE_VERSION_UNKNOWN};
    const auto type{static_cast<MessageType>(bytes[1])};
    if (!IsKnown(type)) return Error{ErrorCode::DECODE, MESSAGE_TYPE_UNKNOWN};
    if (bytes.size() - 2 > MaxPayloadBytes(type, profile)) return Error{ErrorCode::OVERSIZE, "payload longer than its type's maximum"};
    try {
        Parser parser{bytes.subspan(2)};
        Message message{DecodePayload(type, parser, profile)};
        parser.Finish();
        return Ok(std::move(message));
    } catch (const NestedError& nested) {
        return nested.error;
    } catch (const CapExceeded&) {
        return Error{ErrorCode::OVERSIZE, "count exceeds its cap"};
    } catch (const std::ios_base::failure&) {
        return Error{ErrorCode::DECODE, "malformed or non-canonical message"};
    }
}

Result<std::vector<unsigned char>> EncodeMessage(const Message& message, const ProfileParams& profile)
{
    std::vector<unsigned char> out{MESSAGE_VERSION, static_cast<unsigned char>(TypeOf(message))};
    std::visit([&](const auto& m) { EncodePayload(out, m); }, message);
    // Exactly the bytes the decoder accepts: refuse what it refuses, and
    // never return bytes that decode to another message.
    Result<Message> decoded{DecodeMessage(out, profile)};
    if (!decoded.has_value()) return decoded.error();
    if (!(*decoded == message)) return Error{ErrorCode::INVARIANT, MESSAGE_NOT_CANONICAL};
    return Ok(std::move(out));
}

Result<void> CheckProposal(const Proposal& proposal)
{
    const SlotHeaderV2& header{proposal.header};
    if (header.height == 0) return Invalid(PROPOSAL_GENESIS);
    if (proposal.epoch != header.authority_epoch) return Invalid(PROPOSAL_EPOCH);
    if (proposal.parent_cert.has_value() != !header.parent_cert_hash.IsNull()) return Invalid(PROPOSAL_PARENT_CERT);
    if (proposal.parent_cert.has_value() &&
        (proposal.parent_cert->header_hash != header.parent_hash ||
         ((header.commitment_version == COMMITMENT_VERSION_WITHDRAWALS || header.commitment_version == COMMITMENT_VERSION_VIEW_BOUND)
              ? header.parent_cert_hash != header.parent_hash
              : proposal.parent_cert->Hash() != header.parent_cert_hash))) {
        return Invalid(PROPOSAL_PARENT_CERT);
    }
    if (!storage::BodyMatchesHeader(header, proposal.body)) return Invalid(PROPOSAL_BODY);
    return Ok();
}

Result<void> CheckVote(const Vote& vote)
{
    if (vote.slot == 0) return Invalid(VOTE_GENESIS);
    if (!vote.inclusion.empty() && vote.phase != storage::VotePhase::PREPARE) return Invalid(VOTE_INCLUSION_PHASE);
    for (size_t i{1}; i < vote.inclusion.size(); ++i) {
        if (!(vote.inclusion[i - 1] < vote.inclusion[i])) return Invalid(VOTE_INCLUSION_ORDER);
    }
    return Ok();
}

Result<void> CheckViewChange(const ViewChange& report)
{
    if (report.new_view == 0) return Invalid(VIEW_CHANGE_TARGET);
    if (report.prepared.has_value() && report.prepared->view >= report.new_view) return Invalid(VIEW_CHANGE_PREPARED);
    return Ok();
}

Result<void> CheckNewView(const NewView& new_view)
{
    if (new_view.view == 0) return Invalid(NEW_VIEW_TARGET);
    if (new_view.reports.empty()) return Invalid(NEW_VIEW_EMPTY);
    const PreparedCert* highest{nullptr};
    for (size_t i{0}; i < new_view.reports.size(); ++i) {
        const ViewChange& report{new_view.reports[i]};
        if (Result<void> ok{CheckViewChange(report)}; !ok.has_value()) return ok.error();
        if (report.epoch != new_view.epoch || report.slot != new_view.slot || report.new_view != new_view.view) {
            return Invalid(NEW_VIEW_REPORT_MISMATCH);
        }
        if (i > 0 && !(new_view.reports[i - 1].seat < report.seat)) return Invalid(NEW_VIEW_REPORT_ORDER);
        if (!report.prepared.has_value()) continue;
        const PreparedCert& prepared{*report.prepared};
        if (highest == nullptr || prepared.view > highest->view) {
            highest = &prepared;
        } else if (prepared.view == highest->view && prepared.cert.header_hash != highest->cert.header_hash) {
            return Invalid(NEW_VIEW_PREPARED_CONFLICT);
        }
    }
    if (highest != nullptr && new_view.selected != highest->cert.header_hash) return Invalid(NEW_VIEW_SELECTED);
    return Ok();
}

Result<void> CheckEvidence(const Evidence& evidence)
{
    if (const auto* dv{std::get_if<DoubleVote>(&evidence.content)}) {
        const Vote& a{dv->first};
        const Vote& b{dv->second};
        if (a.seat != b.seat || a.phase != b.phase || a.epoch != b.epoch || a.view != b.view || a.slot != b.slot ||
            a.header_hash == b.header_hash) {
            return Invalid(EVIDENCE_NOT_CONFLICTING);
        }
        if (!(a.header_hash < b.header_hash)) return Invalid(EVIDENCE_ORDER);
        return Ok();
    }
    const auto& cc{std::get<ConflictingCertificates>(evidence.content)};
    const uint256 first_hash{cc.first.header.Hash()};
    const uint256 second_hash{cc.second.header.Hash()};
    if (cc.first.cert.header_hash != first_hash || cc.second.cert.header_hash != second_hash) return Invalid(EVIDENCE_CERT_HEADER);
    if (cc.first.header.domain != cc.second.header.domain || cc.first.header.height != cc.second.header.height ||
        first_hash == second_hash) {
        return Invalid(EVIDENCE_NOT_CONFLICTING);
    }
    if (!(first_hash < second_hash)) return Invalid(EVIDENCE_ORDER);
    return Ok();
}

} // namespace flowmesh2::agreement
