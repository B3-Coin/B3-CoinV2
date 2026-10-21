// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** Agreement message codecs and structural checks (slice R10).
 *
 * Goldens come from an independent stdlib Python script written from the
 * documented layouts (scratch evidence R10/golden.py); the vectors here are
 * built from the same labels: LabelHash(label) = SHA256(label).
 */

#include <boost/test/unit_test.hpp>

#include <crypto/sha256.h>
#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/storage_fixtures.h>
#include <streams.h>
#include <util/strencodings.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::agreement;
using namespace flowmesh2::test;

namespace {

uint256 MsgHash(std::string_view name) { return LabelHash("fm2/test/msg/" + std::string{name}); }

storage::VoteSignature Filled(unsigned char byte)
{
    storage::VoteSignature sig;
    sig.fill(byte);
    return sig;
}

//! The golden header (golden.py header(height)).
SlotHeaderV2 GoldenHeader(uint64_t height = 7)
{
    SlotHeaderV2 h;
    h.domain = MsgHash("domain");
    h.profile_hash = MsgHash("profile");
    h.authority_epoch = 3;
    h.height = height;
    h.parent_hash = MsgHash("parent");
    h.parent_cert_hash = MsgHash("parent-cert");
    h.anchor = Anchor{129, MsgHash("anchor")};
    h.facts_from = FactCursor{130, 0};
    h.facts_to = FactCursor{130, 2};
    h.seat_set_root = MsgHash("seats");
    h.facts_root = MsgHash("facts");
    h.actions_root = MsgHash("actions");
    h.outcomes_root = MsgHash("outcomes");
    h.state_root = MsgHash("state");
    h.action_count = 2;
    h.outcome_count = 3;
    return h;
}

SlotCertificate Cert(const uint256& header_hash, std::vector<unsigned char> bitmap, unsigned char fill)
{
    SlotCertificate cert;
    cert.header_hash = header_hash;
    cert.bitmap = std::move(bitmap);
    cert.aggregate.fill(fill);
    return cert;
}

SlotBody GoldenBody()
{
    SlotBody body;
    body.facts = {Bytes("fact-0")};
    body.originals = {Bytes("original-a"), Bytes("original-bc")};
    return body;
}

Proposal GoldenProposal()
{
    return Proposal{3, 1, GoldenHeader(), Cert(MsgHash("parent"), {0x01}, 0x5a), GoldenBody()};
}

Vote GoldenVote()
{
    Vote v;
    v.phase = storage::VotePhase::PREPARE;
    v.epoch = 3;
    v.view = 1;
    v.slot = 7;
    v.header_hash = MsgHash("header");
    v.seat = 5;
    for (size_t i{0}; i < v.signature.size(); ++i) v.signature[i] = static_cast<unsigned char>(i);
    v.inclusion = {ShortId{1, 2, 3, 4, 5, 6}, ShortId{0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6}};
    return v;
}

ViewChange GoldenViewChange()
{
    return ViewChange{3, 7, 2, 1, PreparedCert{1, Cert(MsgHash("header"), {0x0b}, 0x11)}, Filled(0x22)};
}

NewView GoldenNewView()
{
    return NewView{3, 7, 2, 2, MsgHash("header"), {ViewChange{3, 7, 2, 0, std::nullopt, Filled(0x33)}, GoldenViewChange()}, Filled(0x44)};
}

Vote CommitVote(std::string_view header, unsigned char fill)
{
    Vote v;
    v.phase = storage::VotePhase::COMMIT;
    v.epoch = 3;
    v.view = 0;
    v.slot = 7;
    v.header_hash = MsgHash(header);
    v.seat = 2;
    v.signature = Filled(fill);
    return v;
}

Evidence GoldenDoubleVote() { return Evidence{DoubleVote{CommitVote("x", 0x55), CommitVote("y", 0x66)}}; }

Evidence GoldenConflictingCertificates()
{
    return Evidence{ConflictingCertificates{CertifiedHeader{GoldenHeader(7), Cert(MsgHash("c1"), {0x03}, 0x77)},
                                            CertifiedHeader{GoldenHeader(8), Cert(MsgHash("c2"), {0x07}, 0x88)}}};
}

std::string Sha256Hex(const std::vector<unsigned char>& bytes)
{
    unsigned char out[CSHA256::OUTPUT_SIZE];
    CSHA256{}.Write(bytes.data(), bytes.size()).Finalize(out);
    return HexStr(out);
}

std::vector<unsigned char> Encode(const Message& message, const ProfileParams& profile = RegtestProfile())
{
    Result<std::vector<unsigned char>> bytes{EncodeMessage(message, profile)};
    RequireOk(bytes, "EncodeMessage");
    return std::move(bytes).value();
}

void CheckGolden(const Message& message, size_t size, std::string_view sha256)
{
    const std::vector<unsigned char> bytes{Encode(message)};
    BOOST_CHECK_EQUAL(bytes.size(), size);
    BOOST_CHECK_EQUAL(Sha256Hex(bytes), sha256);
}

void CheckRoundTrip(const Message& message, std::string_view what)
{
    const std::vector<unsigned char> bytes{Encode(message)};
    Result<Message> decoded{DecodeMessage(bytes, RegtestProfile())};
    RequireOk(decoded, what);
    BOOST_CHECK_MESSAGE(*decoded == message, std::string{what} + ": round trip changed the message");
    BOOST_CHECK(TypeOf(*decoded) == TypeOf(message));
}

void ExpectDecode(const std::vector<unsigned char>& bytes, ErrorCode code, std::string_view what)
{
    RequireCode(DecodeMessage(bytes, RegtestProfile()), code, what);
}

//! A consistent proposal: the parent certificate names the parent and the body matches the header.
Proposal ConsistentProposal()
{
    Proposal p;
    p.body = GoldenBody();
    p.header = GoldenHeader();
    p.parent_cert = Cert(p.header.parent_hash, {0x01}, 0x5a);
    p.header.parent_cert_hash = p.parent_cert->Hash();
    p.header.actions_root = p.body.ActionsRoot();
    p.header.facts_root = p.body.FactsRoot();
    p.header.action_count = 2;
    p.epoch = p.header.authority_epoch;
    p.view = 1;
    return p;
}

void CheckAccepted(const Result<void>& result, std::string_view what)
{
    BOOST_CHECK_MESSAGE(result.has_value(), std::string{what} + (result.has_value() ? std::string{} : ": " + std::string{result.error().detail}));
}

} // namespace

BOOST_AUTO_TEST_SUITE(messages_tests)

BOOST_AUTO_TEST_CASE(registries_pinned)
{
    BOOST_CHECK_EQUAL(MESSAGE_VERSION, 1);
    BOOST_CHECK_EQUAL(static_cast<int>(MessageType::PROPOSAL), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(MessageType::VOTE), 2);
    BOOST_CHECK_EQUAL(static_cast<int>(MessageType::VIEW_CHANGE), 3);
    BOOST_CHECK_EQUAL(static_cast<int>(MessageType::NEW_VIEW), 4);
    BOOST_CHECK_EQUAL(static_cast<int>(MessageType::EVIDENCE), 5);
    BOOST_CHECK_EQUAL(static_cast<int>(MessageType::CERTIFIED_PROPOSAL), 6);
    BOOST_CHECK_EQUAL(static_cast<int>(MessageType::CONTROL_REPORT), 7);
    BOOST_CHECK_EQUAL(static_cast<int>(MessageType::CONTROL_NEW_VIEW), 8);
    BOOST_CHECK_EQUAL(static_cast<int>(MessageType::OFFER), 9);
    BOOST_CHECK_EQUAL(ToString(MessageType::PROPOSAL), "PROPOSAL");
    BOOST_CHECK_EQUAL(ToString(MessageType::VOTE), "VOTE");
    BOOST_CHECK_EQUAL(ToString(MessageType::VIEW_CHANGE), "VIEW_CHANGE");
    BOOST_CHECK_EQUAL(ToString(MessageType::NEW_VIEW), "NEW_VIEW");
    BOOST_CHECK_EQUAL(ToString(MessageType::EVIDENCE), "EVIDENCE");
    BOOST_CHECK_EQUAL(static_cast<int>(EvidenceKind::DOUBLE_VOTE), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(EvidenceKind::CONFLICTING_CERTIFICATES), 2);
    BOOST_CHECK_EQUAL(ToString(EvidenceKind::DOUBLE_VOTE), "DOUBLE_VOTE");
    BOOST_CHECK_EQUAL(ToString(EvidenceKind::CONFLICTING_CERTIFICATES), "CONFLICTING_CERTIFICATES");
    for (int value : {0, 10, 255}) {
        BOOST_CHECK(!IsKnown(static_cast<MessageType>(value)));
        BOOST_CHECK_EQUAL(ToString(static_cast<MessageType>(value)), "UNKNOWN");
    }
    for (int value : {0, 3, 255}) {
        BOOST_CHECK(!IsKnown(static_cast<EvidenceKind>(value)));
        BOOST_CHECK_EQUAL(ToString(static_cast<EvidenceKind>(value)), "UNKNOWN");
    }
    BOOST_CHECK_EQUAL(SHORT_ID_BYTES, 6U);
    BOOST_CHECK_EQUAL(MAX_VOTE_INCLUSION_IDS, 32U);
    BOOST_CHECK_EQUAL(MAX_NEW_VIEW_REPORTS, MAX_SEATS);
    BOOST_CHECK(TypeOf(GoldenProposal()) == MessageType::PROPOSAL);
    BOOST_CHECK(TypeOf(GoldenVote()) == MessageType::VOTE);
    BOOST_CHECK(TypeOf(GoldenViewChange()) == MessageType::VIEW_CHANGE);
    BOOST_CHECK(TypeOf(GoldenNewView()) == MessageType::NEW_VIEW);
    BOOST_CHECK(TypeOf(GoldenDoubleVote()) == MessageType::EVIDENCE);
    BOOST_CHECK(GoldenDoubleVote().Kind() == EvidenceKind::DOUBLE_VOTE);
    BOOST_CHECK(GoldenConflictingCertificates().Kind() == EvidenceKind::CONFLICTING_CERTIFICATES);
}

BOOST_AUTO_TEST_CASE(golden_bytes)
{
    BOOST_CHECK_EQUAL(HexStr(Encode(GoldenVote())),
                      "01020103000000000000000100000000000000070000000000000088630025f72c276f8408edb267b6a1a0c0a520539fd66f708d"
                      "e9c83adb5d8faa05000000000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728"
                      "292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c"
                      "5d5e5f02010203040506a1a2a3a4a5a6");
    BOOST_CHECK_EQUAL(HexStr(Encode(GoldenViewChange())),
                      "0103030000000000000007000000000000000200000000000000010000000101000000000000008288630025f72c276f8408edb2"
                      "67b6a1a0c0a520539fd66f708de9c83adb5d8faa010b111111111111111111111111111111111111111111111111111111111111"
                      "11111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111"
                      "11111111111111111111111111112222222222222222222222222222222222222222222222222222222222222222222222222222"
                      "22222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222"
                      "222222222222");
    CheckGolden(GoldenVote(), 172, "20809f60647c06f946729542d5db1c6d5bef157aa486ba89e44be76ea87d06c3");
    CheckGolden(GoldenViewChange(), 266, "201e195ed36973edd76d44077353f244db4ee876fd7fa8ebbba2c883153ebb35");
    CheckGolden(GoldenProposal(), 547, "b317b72dcb9413befc9918ae40dc3360d3ac7b0daf712776888e71677e529fcc");
    Proposal no_parent{GoldenProposal()};
    no_parent.parent_cert.reset();
    CheckGolden(no_parent, 417, "e9dc39903ac9f45454009334fa500fffbd306288b8cf9cace7170143858af9f6");
    CheckGolden(GoldenNewView(), 552, "247ee7dd80a3875e8d362b3e5b2850c280acb852c516199d50de5ad8ead65203");
    CheckGolden(GoldenDoubleVote(), 321, "3f0d5b20675cc96a22c0bd58603052e34982dc11987a838f4b47d02e6235519c");
    CheckGolden(GoldenConflictingCertificates(), 997, "40d72f2ecbaf118444bc77907eb39bdf39b0476769c8fe0c557278ade16b08f1");
}

BOOST_AUTO_TEST_CASE(round_trip_every_type)
{
    CheckRoundTrip(GoldenProposal(), "proposal");
    Proposal no_parent{GoldenProposal()};
    no_parent.parent_cert.reset();
    CheckRoundTrip(no_parent, "proposal without a parent certificate");
    Proposal empty_body{GoldenProposal()};
    empty_body.body = SlotBody{};
    CheckRoundTrip(empty_body, "proposal with an empty body");

    CheckRoundTrip(GoldenVote(), "prepare vote");
    Vote no_ids{GoldenVote()};
    no_ids.inclusion.clear();
    CheckRoundTrip(no_ids, "vote without inclusion IDs");
    Vote full{GoldenVote()};
    full.inclusion.clear();
    for (unsigned char i{0}; i < MAX_VOTE_INCLUSION_IDS; ++i) full.inclusion.push_back(ShortId{0, 0, 0, 0, 0, i});
    CheckRoundTrip(full, "vote with 32 inclusion IDs");
    Vote top_seat{GoldenVote()};
    top_seat.seat = MAX_SEATS - 1;
    CheckRoundTrip(top_seat, "vote of the highest seat index");

    CheckRoundTrip(GoldenViewChange(), "view change with a preparation");
    ViewChange bare{GoldenViewChange()};
    bare.prepared.reset();
    CheckRoundTrip(bare, "view change without a preparation");

    CheckRoundTrip(GoldenNewView(), "new view");
    NewView no_reports{GoldenNewView()};
    no_reports.reports.clear();
    CheckRoundTrip(no_reports, "new view without reports (decodes; CheckNewView refuses it)");

    CheckRoundTrip(GoldenDoubleVote(), "double vote evidence");
    CheckRoundTrip(GoldenConflictingCertificates(), "conflicting certificate evidence");
}

BOOST_AUTO_TEST_CASE(caps_refused_before_allocation)
{
    const ProfileParams profile{RegtestProfile()};
    // 33 short IDs: the encoder refuses, and so does the decoder on hand-made bytes.
    Vote over{GoldenVote()};
    over.inclusion.assign(MAX_VOTE_INCLUSION_IDS + 1, ShortId{});
    RequireCode(EncodeMessage(over, profile), ErrorCode::OVERSIZE, "encode 33 inclusion IDs");
    std::vector<unsigned char> bytes{Encode(GoldenVote())};
    const size_t count_pos{2 + 157};
    BOOST_REQUIRE_EQUAL(bytes[count_pos], 2);
    bytes[count_pos] = MAX_VOTE_INCLUSION_IDS + 1;
    for (int i{0}; i < 31; ++i) bytes.insert(bytes.end(), SHORT_ID_BYTES, 0);
    ExpectDecode(bytes, ErrorCode::OVERSIZE, "decode 33 inclusion IDs");
    // The count itself is refused, before any ID is read: a count of 33 with
    // only two IDs behind it is OVERSIZE (refused at the count), not DECODE
    // (cut input), and its length is within the vote maximum.
    std::vector<unsigned char> count_only{Encode(GoldenVote())};
    count_only[count_pos] = MAX_VOTE_INCLUSION_IDS + 1;
    ExpectDecode(count_only, ErrorCode::OVERSIZE, "a count of 33 refused before the IDs");

    // A NEW_VIEW announcing 2^32 - 1 reports is refused at the count.
    std::vector<unsigned char> nv{Encode(GoldenNewView())};
    const size_t reports_pos{2 + 8 + 8 + 8 + 4 + 32};
    BOOST_REQUIRE_EQUAL(nv[reports_pos], 2);
    std::vector<unsigned char> huge{nv.begin(), nv.begin() + reports_pos};
    for (unsigned char b : {0xfe, 0xff, 0xff, 0xff, 0xff}) huge.push_back(b);
    huge.insert(huge.end(), nv.begin() + reports_pos + 1, nv.end());
    ExpectDecode(huge, ErrorCode::OVERSIZE, "new view with 2^32 - 1 reports");
    NewView too_many{GoldenNewView()};
    too_many.reports.assign(MAX_NEW_VIEW_REPORTS + 1, ViewChange{3, 7, 2, 0, std::nullopt, Filled(0)});
    RequireCode(EncodeMessage(too_many, profile), ErrorCode::OVERSIZE, "encode MAX_SEATS + 1 reports");

    // A certificate longer than its maximum encoding.
    Proposal long_cert{GoldenProposal()};
    long_cert.parent_cert->bitmap.assign(MAX_BITMAP_BYTES + 1, 0);
    RequireCode(EncodeMessage(long_cert, profile), ErrorCode::OVERSIZE, "parent certificate bitmap of 129 bytes");

    // A body outside the profile's caps.
    Proposal big_body{GoldenProposal()};
    big_body.body.originals.assign(profile.max_actions_per_slot + 1, Bytes("a"));
    RequireCode(EncodeMessage(big_body, profile), ErrorCode::OVERSIZE, "1,025 originals");

    // Input above MaxMessageBytes, and a payload above its type's maximum.
    std::vector<unsigned char> giant(MaxMessageBytes(profile) + 1, 0);
    giant[0] = MESSAGE_VERSION;
    giant[1] = static_cast<unsigned char>(MessageType::PROPOSAL);
    ExpectDecode(giant, ErrorCode::OVERSIZE, "above MaxMessageBytes");
    std::vector<unsigned char> long_vote{Encode(GoldenVote())};
    long_vote.resize(long_vote.size() + 400, 0);
    ExpectDecode(long_vote, ErrorCode::OVERSIZE, "vote payload above the vote maximum");
    BOOST_CHECK_GT(MaxMessageBytes(profile), SlotBody::MaxEncodedSize(profile));
}

BOOST_AUTO_TEST_CASE(decode_refuses_malformed_bytes)
{
    ExpectDecode({}, ErrorCode::DECODE, "empty");
    ExpectDecode({MESSAGE_VERSION}, ErrorCode::DECODE, "envelope cut");
    const std::vector<unsigned char> vote{Encode(GoldenVote())};
    {
        std::vector<unsigned char> b{vote};
        b[0] = 2;
        RequireRefusal(DecodeMessage(b, RegtestProfile()), ErrorCode::DECODE, MESSAGE_VERSION_UNKNOWN, "version 2");
        for (unsigned char type : {0, 10, 255}) {
            b = vote;
            b[1] = type;
            RequireRefusal(DecodeMessage(b, RegtestProfile()), ErrorCode::DECODE, MESSAGE_TYPE_UNKNOWN, "unknown type");
        }
    }
    for (unsigned char phase : {0, 3}) {
        std::vector<unsigned char> b{vote};
        b[2] = phase;
        ExpectDecode(b, ErrorCode::DECODE, "unknown vote phase");
    }
    {
        // Seat index MAX_SEATS (1,024 = 0x0400) at offset 2 + 1 + 24 + 32.
        std::vector<unsigned char> b{vote};
        b[2 + 1 + 24 + 32] = 0x00;
        b[2 + 1 + 24 + 32 + 1] = 0x04;
        ExpectDecode(b, ErrorCode::DECODE, "seat at MAX_SEATS");
        Vote bad_seat{GoldenVote()};
        bad_seat.seat = MAX_SEATS;
        RequireCode(EncodeMessage(bad_seat, RegtestProfile()), ErrorCode::DECODE, "encode a seat at MAX_SEATS");
    }
    {
        std::vector<unsigned char> b{vote};
        b.push_back(0);
        ExpectDecode(b, ErrorCode::DECODE, "trailing byte");
        for (size_t cut{2}; cut < vote.size(); ++cut) {
            ExpectDecode(std::vector<unsigned char>{vote.begin(), vote.begin() + cut}, ErrorCode::DECODE, "truncated vote");
        }
    }
    {
        // The count 2 written as a non-canonical 3-byte CompactSize.
        std::vector<unsigned char> b{vote.begin(), vote.begin() + 2 + 157};
        for (unsigned char x : {0xfd, 0x02, 0x00}) b.push_back(x);
        b.insert(b.end(), vote.begin() + 2 + 157 + 1, vote.end());
        ExpectDecode(b, ErrorCode::DECODE, "non-canonical CompactSize");
    }
    {
        std::vector<unsigned char> b{Encode(GoldenViewChange())};
        b[2 + 28] = 2;
        ExpectDecode(b, ErrorCode::DECODE, "has_prepared 2");
    }
    for (const Evidence& evidence : {GoldenDoubleVote(), GoldenConflictingCertificates()}) {
        // An unknown kind in front of a payload of either kind: no kind decodes another's payload.
        std::vector<unsigned char> b{Encode(evidence)};
        for (unsigned char kind : {0, 3, 255}) {
            b[2] = kind;
            ExpectDecode(b, ErrorCode::DECODE, "unknown evidence kind");
        }
    }
    {
        // A nested view-change report one byte longer than its content.
        NewView nv{GoldenNewView()};
        nv.reports.resize(1);
        std::vector<unsigned char> b{Encode(nv)};
        const size_t len_pos{2 + 8 + 8 + 8 + 4 + 32 + 1};
        BOOST_REQUIRE_EQUAL(b[len_pos], 8 + 8 + 8 + 4 + 1 + 96);
        b[len_pos] += 1;
        b.insert(b.begin() + len_pos + 1 + b[len_pos] - 1, 0);
        ExpectDecode(b, ErrorCode::DECODE, "nested report with a trailing byte");
    }
    {
        // A proposal whose header has an unknown version.
        std::vector<unsigned char> b{Encode(GoldenProposal())};
        b[2 + 16] = 2;
        ExpectDecode(b, ErrorCode::DECODE, "header version 2");
    }
}

BOOST_AUTO_TEST_CASE(check_proposal_clauses)
{
    CheckAccepted(CheckProposal(ConsistentProposal()), "consistent proposal");
    {
        // Height 1: the parent is the genesis head, which has no certificate.
        Proposal p{ConsistentProposal()};
        p.header.height = 1;
        p.header.parent_cert_hash.SetNull();
        p.parent_cert.reset();
        CheckAccepted(CheckProposal(p), "first slot without a parent certificate");
    }
    Proposal p{ConsistentProposal()};
    p.header.height = 0;
    RequireRefusal(CheckProposal(p), ErrorCode::INVALID_INPUT, PROPOSAL_GENESIS, "genesis height");
    p = ConsistentProposal();
    p.epoch += 1;
    RequireRefusal(CheckProposal(p), ErrorCode::INVALID_INPUT, PROPOSAL_EPOCH, "epoch");
    p = ConsistentProposal();
    p.parent_cert.reset();
    RequireRefusal(CheckProposal(p), ErrorCode::INVALID_INPUT, PROPOSAL_PARENT_CERT, "missing parent certificate");
    p = ConsistentProposal();
    p.header.parent_cert_hash.SetNull();
    RequireRefusal(CheckProposal(p), ErrorCode::INVALID_INPUT, PROPOSAL_PARENT_CERT, "unexpected parent certificate");
    p = ConsistentProposal();
    p.parent_cert->aggregate[0] ^= 1;
    RequireRefusal(CheckProposal(p), ErrorCode::INVALID_INPUT, PROPOSAL_PARENT_CERT, "another certificate of the parent");
    p = ConsistentProposal();
    p.parent_cert->header_hash = MsgHash("other");
    p.header.parent_cert_hash = p.parent_cert->Hash();
    RequireRefusal(CheckProposal(p), ErrorCode::INVALID_INPUT, PROPOSAL_PARENT_CERT, "certificate of another header");
    p = ConsistentProposal();
    p.body.originals.pop_back();
    RequireRefusal(CheckProposal(p), ErrorCode::INVALID_INPUT, PROPOSAL_BODY, "body without one original");
    p = ConsistentProposal();
    p.body.facts.clear();
    RequireRefusal(CheckProposal(p), ErrorCode::INVALID_INPUT, PROPOSAL_BODY, "body without its fact");
}

BOOST_AUTO_TEST_CASE(check_vote_and_view_change_clauses)
{
    CheckAccepted(CheckVote(GoldenVote()), "prepare vote with inclusion IDs");
    CheckAccepted(CheckVote(CommitVote("x", 1)), "commit vote");
    Vote v{GoldenVote()};
    v.slot = 0;
    RequireRefusal(CheckVote(v), ErrorCode::INVALID_INPUT, VOTE_GENESIS, "vote for slot 0");
    v = GoldenVote();
    std::swap(v.inclusion[0], v.inclusion[1]);
    RequireRefusal(CheckVote(v), ErrorCode::INVALID_INPUT, VOTE_INCLUSION_ORDER, "descending IDs");
    v = GoldenVote();
    v.inclusion[1] = v.inclusion[0];
    RequireRefusal(CheckVote(v), ErrorCode::INVALID_INPUT, VOTE_INCLUSION_ORDER, "repeated ID");
    v = GoldenVote();
    v.phase = storage::VotePhase::COMMIT;
    RequireRefusal(CheckVote(v), ErrorCode::INVALID_INPUT, VOTE_INCLUSION_PHASE, "inclusion IDs on a commit vote");

    CheckAccepted(CheckViewChange(GoldenViewChange()), "view change");
    ViewChange vc{GoldenViewChange()};
    vc.new_view = 0;
    vc.prepared.reset();
    RequireRefusal(CheckViewChange(vc), ErrorCode::INVALID_INPUT, VIEW_CHANGE_TARGET, "target view 0");
    vc = GoldenViewChange();
    vc.prepared->view = vc.new_view;
    RequireRefusal(CheckViewChange(vc), ErrorCode::INVALID_INPUT, VIEW_CHANGE_PREPARED, "preparation from the target view");
}

BOOST_AUTO_TEST_CASE(check_new_view_clauses)
{
    CheckAccepted(CheckNewView(GoldenNewView()), "new view");
    NewView nothing_prepared{GoldenNewView()};
    nothing_prepared.reports[1].prepared.reset();
    nothing_prepared.selected = MsgHash("any offered value");
    CheckAccepted(CheckNewView(nothing_prepared), "no preparation: the leader picks the value");

    NewView nv{GoldenNewView()};
    nv.view = 0;
    RequireRefusal(CheckNewView(nv), ErrorCode::INVALID_INPUT, NEW_VIEW_TARGET, "view 0");
    nv = GoldenNewView();
    nv.reports.clear();
    RequireRefusal(CheckNewView(nv), ErrorCode::INVALID_INPUT, NEW_VIEW_EMPTY, "no reports");
    nv = GoldenNewView();
    nv.reports[1].prepared->view = 2;
    RequireRefusal(CheckNewView(nv), ErrorCode::INVALID_INPUT, VIEW_CHANGE_PREPARED, "a report fails CheckViewChange");
    for (int field{0}; field < 3; ++field) {
        nv = GoldenNewView();
        if (field == 0) nv.reports[0].epoch += 1;
        if (field == 1) nv.reports[0].slot += 1;
        if (field == 2) nv.reports[0].new_view += 1;
        RequireRefusal(CheckNewView(nv), ErrorCode::INVALID_INPUT, NEW_VIEW_REPORT_MISMATCH, "report of another epoch, slot or view");
    }
    nv = GoldenNewView();
    std::swap(nv.reports[0], nv.reports[1]);
    RequireRefusal(CheckNewView(nv), ErrorCode::INVALID_INPUT, NEW_VIEW_REPORT_ORDER, "seats descending");
    nv = GoldenNewView();
    nv.reports[0].seat = nv.reports[1].seat;
    RequireRefusal(CheckNewView(nv), ErrorCode::INVALID_INPUT, NEW_VIEW_REPORT_ORDER, "one seat twice");
    nv = GoldenNewView();
    nv.reports[0].prepared = PreparedCert{1, Cert(MsgHash("other header"), {0x0b}, 0x12)};
    RequireRefusal(CheckNewView(nv), ErrorCode::INVALID_INPUT, NEW_VIEW_PREPARED_CONFLICT, "two headers prepared in view 1");
    nv = GoldenNewView();
    nv.reports[0].prepared = PreparedCert{0, Cert(MsgHash("older header"), {0x0b}, 0x12)};
    CheckAccepted(CheckNewView(nv), "an older preparation of another header is superseded");
    nv.selected = MsgHash("older header");
    RequireRefusal(CheckNewView(nv), ErrorCode::INVALID_INPUT, NEW_VIEW_SELECTED, "selected the older preparation");
    nv = GoldenNewView();
    nv.selected = MsgHash("not prepared");
    RequireRefusal(CheckNewView(nv), ErrorCode::INVALID_INPUT, NEW_VIEW_SELECTED, "selected a header nobody prepared");
}

BOOST_AUTO_TEST_CASE(check_evidence_clauses)
{
    Evidence dv{GoldenDoubleVote()};
    auto& votes{std::get<DoubleVote>(dv.content)};
    if (!(votes.first.header_hash < votes.second.header_hash)) std::swap(votes.first, votes.second);
    CheckAccepted(CheckEvidence(dv), "double vote");
    {
        Evidence swapped{dv};
        auto& s{std::get<DoubleVote>(swapped.content)};
        std::swap(s.first, s.second);
        RequireRefusal(CheckEvidence(swapped), ErrorCode::INVALID_INPUT, EVIDENCE_ORDER, "double vote in descending order");
    }
    for (int field{0}; field < 6; ++field) {
        Evidence bad{dv};
        Vote& second{std::get<DoubleVote>(bad.content).second};
        if (field == 0) second.seat += 1;
        if (field == 1) second.phase = storage::VotePhase::PREPARE;
        if (field == 2) second.epoch += 1;
        if (field == 3) second.view += 1;
        if (field == 4) second.slot += 1;
        if (field == 5) second.header_hash = std::get<DoubleVote>(bad.content).first.header_hash;
        RequireRefusal(CheckEvidence(bad), ErrorCode::INVALID_INPUT, EVIDENCE_NOT_CONFLICTING, "votes that do not conflict");
    }

    // Two certified headers for one height.
    SlotHeaderV2 a{GoldenHeader(7)};
    SlotHeaderV2 b{GoldenHeader(7)};
    b.state_root = MsgHash("other state");
    if (!(a.Hash() < b.Hash())) std::swap(a, b);
    const Evidence cc{ConflictingCertificates{CertifiedHeader{a, Cert(a.Hash(), {0x01}, 1)}, CertifiedHeader{b, Cert(b.Hash(), {0x01}, 2)}}};
    CheckAccepted(CheckEvidence(cc), "conflicting certificates");
    {
        Evidence swapped{cc};
        auto& s{std::get<ConflictingCertificates>(swapped.content)};
        std::swap(s.first, s.second);
        RequireRefusal(CheckEvidence(swapped), ErrorCode::INVALID_INPUT, EVIDENCE_ORDER, "certificates in descending order");
    }
    {
        Evidence bad{cc};
        std::get<ConflictingCertificates>(bad.content).second.cert.header_hash = a.Hash();
        RequireRefusal(CheckEvidence(bad), ErrorCode::INVALID_INPUT, EVIDENCE_CERT_HEADER, "certificate of the other header");
    }
    {
        Evidence same{ConflictingCertificates{CertifiedHeader{a, Cert(a.Hash(), {0x01}, 1)}, CertifiedHeader{a, Cert(a.Hash(), {0x01}, 2)}}};
        RequireRefusal(CheckEvidence(same), ErrorCode::INVALID_INPUT, EVIDENCE_NOT_CONFLICTING, "two certificates of one header");
    }
    for (int field{0}; field < 2; ++field) {
        SlotHeaderV2 c{b};
        if (field == 0) c.height += 1;
        if (field == 1) c.domain = MsgHash("other domain");
        SlotHeaderV2 lo{a};
        SlotHeaderV2 hi{c};
        if (!(lo.Hash() < hi.Hash())) std::swap(lo, hi);
        const Evidence bad{ConflictingCertificates{CertifiedHeader{lo, Cert(lo.Hash(), {0x01}, 1)}, CertifiedHeader{hi, Cert(hi.Hash(), {0x01}, 2)}}};
        RequireRefusal(CheckEvidence(bad), ErrorCode::INVALID_INPUT, EVIDENCE_NOT_CONFLICTING, "headers of another height or domain");
    }
}

BOOST_AUTO_TEST_SUITE_END()
