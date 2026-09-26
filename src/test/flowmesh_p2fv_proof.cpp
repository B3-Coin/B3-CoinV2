// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <test/flowmesh_p2fv_proof.h>

#include <hash.h>
#include <streams.h>

#include <algorithm>
#include <map>
#include <utility>

namespace p2fv {
namespace {

inline constexpr uint8_t CODEC_VERSION{1};
inline constexpr char SIGNING_TAG[]{"B3/TEST-ONLY/P2FV-PROOF/SIGN/1"};
inline constexpr char MEMBERSHIP_TAG[]{"B3/TEST-ONLY/P2FV-PROOF/MEMBERS/1"};

void Require(bool condition, const char* reason)
{
    if (!condition) throw Invalid(reason);
}

bool KnownKind(Kind kind)
{
    return static_cast<uint8_t>(kind) <= static_cast<uint8_t>(Kind::PROPOSE);
}

bool ZeroSignature(const Packet& packet)
{
    return std::all_of(packet.signature.begin(), packet.signature.end(),
                       [](unsigned char value) { return value == 0; });
}

void Bound(const Packet& packet, size_t depth, size_t& nodes)
{
    Require(depth <= MAX_TREE_DEPTH && ++nodes <= MAX_TREE_NODES, "PROOF_TREE_BOUND");
    Require(KnownKind(packet.kind) && packet.items.size() <= MAX_CHILDREN, "PROOF_KIND_OR_CHILD_BOUND");
    for (const auto& child : packet.items) Bound(child, depth + 1, nodes);
}

void WritePacket(VectorWriter& writer, const Packet& packet, bool omit_signature)
{
    writer << static_cast<uint8_t>(packet.kind) << packet.view << packet.sender << packet.instance;
    writer << static_cast<uint8_t>(packet.value.has_value());
    if (packet.value) writer << *packet.value;
    writer << static_cast<uint8_t>(packet.items.size());
    for (const auto& child : packet.items) WritePacket(writer, child, false);
    if (omit_signature) {
        const std::array<unsigned char, bls::SIGNATURE_SIZE> zero{};
        writer << zero;
    } else {
        writer << packet.signature;
    }
}

Bytes EncodeImpl(const Packet& packet, bool omit_signature)
{
    size_t nodes{0};
    Bound(packet, 0, nodes);
    Bytes bytes;
    VectorWriter writer{bytes, 0};
    writer << CODEC_VERSION;
    WritePacket(writer, packet, omit_signature);
    Require(bytes.size() <= MAX_PROOF_BYTES, "PROOF_BYTE_BOUND");
    return bytes;
}

Packet ReadPacket(SpanReader& reader, size_t depth, size_t& nodes)
{
    Require(depth <= MAX_TREE_DEPTH && ++nodes <= MAX_TREE_NODES, "PROOF_TREE_BOUND");
    Packet packet;
    uint8_t kind, has_value, children;
    reader >> kind >> packet.view >> packet.sender >> packet.instance >> has_value;
    packet.kind = static_cast<Kind>(kind);
    Require(KnownKind(packet.kind) && has_value <= 1, "PROOF_KIND_OR_OPTIONAL_TAG");
    if (has_value) {
        uint256 value;
        reader >> value;
        packet.value = value;
    }
    reader >> children;
    Require(children <= MAX_CHILDREN && nodes + children <= MAX_TREE_NODES, "PROOF_CHILD_BOUND");
    Require(depth < MAX_TREE_DEPTH || children == 0, "PROOF_TREE_BOUND");
    packet.items.reserve(children);
    for (uint8_t i{0}; i < children; ++i) packet.items.push_back(ReadPacket(reader, depth + 1, nodes));
    reader >> packet.signature;
    return packet;
}

void CheckTree(const Context& context, const Packet& packet)
{
    size_t nodes{0};
    Bound(packet, 0, nodes);
    const auto visit = [&](const auto& self, const Packet& p) -> void {
        Require(p.instance == context.Instance(), "NESTED_INSTANCE_OR_TYPE");
        if (!IsSigned(p.kind)) Require(ZeroSignature(p), "UNSIGNED_WRAPPER_SIGNATURE");
        if (p.kind == Kind::EMPTY) {
            Require(p.view == -1 && p.sender == -1 && !p.value && p.items.empty(), "NONCANONICAL_EMPTY");
        }
        for (const auto& child : p.items) self(self, child);
    };
    visit(visit, packet);
}

bool IsEmpty(const Context& context, const Packet& packet)
{
    if (packet.kind != Kind::EMPTY) return false;
    CheckTree(context, packet);
    return true;
}

void Authentic(const Context& context, const Packet& packet, Kind expected)
{
    CheckTree(context, packet);
    Require(packet.kind == expected && IsSigned(packet.kind) && packet.sender >= 0 &&
                packet.sender < static_cast<int32_t>(N) && packet.view >= 0 && packet.view <= MAX_VIEW,
            "AUTH_OR_CONTEXT");
    const auto signature{bls::Signature::Decode(packet.signature)};
    Require(signature.has_value(), "MALFORMED_SIGNATURE");
    const auto digest{SigningDigest(context, packet)};
    Require(bls::Verify(context.Key(packet.sender).Key(),
                        std::span<const unsigned char>{digest.begin(), 32}, *signature), "BAD_SIGNATURE");
}

void VerifyV0(const Context& context, const Packet& packet, int32_t expected_sender)
{
    CheckTree(context, packet);
    Require(packet.kind == Kind::V0 && packet.items.size() == 2, "REPORT_V0_SHAPE");
    const auto& own{packet.items[0]};
    const auto& leader{packet.items[1]};
    VerifyVote(context, own, Kind::PREPARE);
    VerifyVote(context, leader, Kind::PREPARE);
    Require(own.view == 0 && leader.view == 0 && own.sender == expected_sender &&
                leader.sender == 0 && own.value == leader.value && packet.view == 0 &&
                packet.sender == expected_sender && packet.value == own.value, "REPORT_V0_LINK");
}

void VerifyEq(const Context& context, const Packet& packet)
{
    CheckTree(context, packet);
    Require(packet.kind == Kind::EQ && packet.items.size() == 2 && packet.view == 0 &&
                packet.sender == -1 && !packet.value, "EQUIVOCATION_SHAPE");
    for (const auto& vote : packet.items) {
        VerifyVote(context, vote, Kind::PREPARE);
        Require(vote.sender == 0 && vote.view == 0, "EQUIVOCATION_CONTEXT");
    }
    Require(packet.items[0].value != packet.items[1].value, "NOT_EQUIVOCATION");
}

} // namespace

Context::Context(const uint256& instance, std::span<const Member> members) : m_instance{instance}
{
    Require(!instance.IsNull() && members.size() == N, "MEMBERSHIP_OR_INSTANCE");
    std::set<std::array<unsigned char, bls::PUBKEY_SIZE>> distinct;
    HashWriter writer;
    writer << std::string{MEMBERSHIP_TAG} << static_cast<uint32_t>(N);
    for (const auto& member : members) {
        Require(distinct.insert(member.public_key).second, "DUPLICATE_MEMBER_KEY");
        const auto key{bls::PublicKey::Decode(member.public_key)};
        const auto pop{bls::Signature::Decode(member.proof_of_possession)};
        Require(key.has_value() && pop.has_value(), "MEMBERSHIP_POINT_DECODE");
        const auto verified{bls::VerifiedPublicKey::FromPoP(*key, *pop)};
        Require(verified.has_value(), "MEMBERSHIP_POP");
        m_keys.push_back(*verified);
        writer << member.public_key << member.proof_of_possession;
    }
    m_membership_hash = writer.GetHash();
}

const bls::VerifiedPublicKey& Context::Key(size_t seat) const
{
    Require(seat < m_keys.size(), "SEAT_OUT_OF_RANGE");
    return m_keys[seat];
}

bool IsSigned(Kind kind)
{
    return kind == Kind::PREPARE || kind == Kind::COMMIT || kind == Kind::REPORT || kind == Kind::NEW_VIEW;
}

const char* KindName(Kind kind)
{
    switch (kind) {
    case Kind::EMPTY: return "EMPTY";
    case Kind::PREPARE: return "PREPARE";
    case Kind::COMMIT: return "COMMIT";
    case Kind::PC: return "PC";
    case Kind::FAST: return "FAST";
    case Kind::SLOW: return "SLOW";
    case Kind::V0: return "V0";
    case Kind::EQ: return "EQ";
    case Kind::REPORT: return "REPORT";
    case Kind::NEW_VIEW: return "NEW_VIEW";
    case Kind::PROPOSE: return "PROPOSE";
    }
    return "UNKNOWN";
}

Packet Make(const Context& context, Kind kind, int32_t view, int32_t sender,
            std::optional<uint256> value, std::vector<Packet> items)
{
    return {kind, view, sender, value, std::move(items), context.Instance(), {}};
}

Packet Empty(const Context& context) { return Make(context, Kind::EMPTY); }
Bytes Encode(const Packet& packet) { return EncodeImpl(packet, false); }

Packet Decode(std::span<const unsigned char> bytes)
{
    Require(!bytes.empty() && bytes.size() <= MAX_PROOF_BYTES, "PROOF_BYTE_BOUND");
    try {
        SpanReader reader{bytes};
        uint8_t version;
        reader >> version;
        Require(version == CODEC_VERSION, "PROOF_CODEC_VERSION");
        size_t nodes{0};
        auto packet{ReadPacket(reader, 0, nodes)};
        Require(reader.empty(), "PROOF_TRAILING_BYTES");
        return packet;
    } catch (const std::ios_base::failure&) {
        throw Invalid("PROOF_TRUNCATED_OR_MALFORMED");
    }
}

uint256 SigningDigest(const Context& context, const Packet& packet)
{
    const auto encoded{EncodeImpl(packet, true)};
    HashWriter writer;
    writer << std::string{SIGNING_TAG} << context.MembershipHash() << context.Instance() << encoded;
    return writer.GetHash();
}

Packet Sign(const Context& context, const bls::SecretKey& key, Packet packet)
{
    Require(IsSigned(packet.kind) && packet.instance == context.Instance() &&
                packet.sender >= 0 && packet.sender < static_cast<int32_t>(N) &&
                packet.view >= 0 && packet.view <= MAX_VIEW, "SIGN_CONTEXT");
    Require(key.GetPublicKey() == context.Key(packet.sender).Key(), "SIGN_WRONG_KEY");
    const auto digest{SigningDigest(context, packet)};
    packet.signature = key.Sign(std::span<const unsigned char>{digest.begin(), 32}).Compressed();
    return packet;
}

void VerifyVote(const Context& context, const Packet& packet, Kind expected)
{
    Require(expected == Kind::PREPARE || expected == Kind::COMMIT, "VOTE_EXPECTED_KIND");
    Authentic(context, packet, expected);
    Require(packet.items.empty() && packet.value.has_value(), "VOTE_SHAPE");
}

void VerifyCertificate(const Context& context, const Packet& packet)
{
    CheckTree(context, packet);
    Require((packet.kind == Kind::PC || packet.kind == Kind::FAST || packet.kind == Kind::SLOW) &&
                !packet.items.empty() && packet.value && packet.view >= 0 && packet.view <= MAX_VIEW,
            "CERTIFICATE_SHAPE");
    const auto vote_kind{packet.kind == Kind::SLOW ? Kind::COMMIT : Kind::PREPARE};
    std::set<int32_t> senders;
    for (const auto& vote : packet.items) {
        VerifyVote(context, vote, vote_kind);
        Require(vote.view == packet.view && vote.value == packet.value &&
                    senders.insert(vote.sender).second, "CERTIFICATE_MATCH_OR_DUPLICATE");
    }
    Require(senders.size() >= (packet.kind == Kind::FAST ? FAST_QUORUM : Q), "CERTIFICATE_QUORUM");
    if (packet.kind == Kind::FAST) Require(packet.view == 0 && senders.contains(0), "FAST_CONTEXT");
    if (packet.kind == Kind::SLOW) Require(packet.view != 0, "NO_SLOW_VIEW_ZERO_WHEN_FAST_EQUALS_Q");
}

void VerifyReport(const Context& context, const Packet& packet)
{
    Authentic(context, packet, Kind::REPORT);
    Require(packet.view >= 1 && !packet.value && packet.items.size() == 3, "REPORT_SHAPE");
    if (!IsEmpty(context, packet.items[0])) VerifyV0(context, packet.items[0], packet.sender);
    if (!IsEmpty(context, packet.items[1])) {
        const auto& pc{packet.items[1]};
        VerifyCertificate(context, pc);
        Require(pc.kind == Kind::PC && pc.view < packet.view, "REPORT_PC_CONTEXT");
    }
    if (!IsEmpty(context, packet.items[2])) {
        const auto& decision{packet.items[2]};
        VerifyCertificate(context, decision);
        Require((decision.kind == Kind::FAST || decision.kind == Kind::SLOW) &&
                    decision.view < packet.view, "REPORT_DECISION_CONTEXT");
    }
}

Choice Choices(const Context& context, int32_t view,
               std::span<const Packet> reports, const Packet& equivocation)
{
    Require(view >= 1 && view <= MAX_VIEW && reports.size() == Q, "REPORT_SET");
    std::set<int32_t> senders;
    std::set<uint256> originals;
    for (const auto& report : reports) {
        VerifyReport(context, report);
        Require(report.view == view, "REPORT_TARGET");
        Require(senders.insert(report.sender).second, "REPORT_SET");
        if (report.items[0].kind != Kind::EMPTY) originals.insert(*report.items[0].items[1].value);
    }
    if (!IsEmpty(context, equivocation)) {
        VerifyEq(context, equivocation);
        for (const auto& vote : equivocation.items) originals.insert(*vote.value);
    }
    const bool equivocated{originals.size() > 1};
    // Exact MESSAGE_MODEL test clarification: exclude old leader globally,
    // BEFORE decision and highest-PC branches, not just the fallback branch.
    Require(!equivocated || !senders.contains(0), "EXCLUDE_EQUIVOCATING_OLD_LEADER");
    for (const auto& report : reports) {
        Require(report.items[2].kind == Kind::EMPTY, "APPLY_DECISION_INSTEAD");
    }
    int32_t highest{-1};
    std::set<uint256> highest_values;
    for (const auto& report : reports) {
        const auto& pc{report.items[1]};
        if (pc.kind == Kind::EMPTY) continue;
        if (pc.view > highest) { highest = pc.view; highest_values.clear(); }
        if (pc.view == highest) highest_values.insert(*pc.value);
    }
    if (highest >= 0) {
        Require(highest_values.size() == 1, "CONFLICTING_HIGHEST_PC");
        return {false, std::move(highest_values)};
    }
    if (equivocated) {
        std::map<uint256, size_t> counts;
        for (const auto& report : reports) {
            if (report.items[0].kind == Kind::V0) ++counts[*report.items[0].items[0].value];
        }
        for (const auto& [value, count] : counts) {
            if (count >= 2) return {false, {value}}; // f+t=2 for fixed N=4.
        }
        return {true, {}};
    }
    return {originals.empty(), std::move(originals)};
}

void VerifyNewView(const Context& context, const Packet& packet)
{
    Authentic(context, packet, Kind::NEW_VIEW);
    Require(packet.view >= 1 && packet.sender == packet.view % static_cast<int32_t>(N) &&
                packet.items.size() == Q + 1 && packet.value.has_value(), "NEW_VIEW_SHAPE");
    const std::span<const Packet> reports{packet.items.data() + 1, Q};
    Require(Choices(context, packet.view, reports, packet.items[0]).Allows(*packet.value), "NEW_VIEW_CHOICE");
}

void VerifyProposal(const Context& context, const Packet& packet)
{
    CheckTree(context, packet);
    Require(packet.kind == Kind::PROPOSE && packet.items.size() == 2, "PROPOSAL_SHAPE");
    const auto& leader{packet.items[0]};
    const auto& nv{packet.items[1]};
    VerifyVote(context, leader, Kind::PREPARE);
    Require(packet.view == leader.view && packet.value == leader.value &&
                packet.sender == packet.view % static_cast<int32_t>(N) && packet.sender == leader.sender,
            "PROPOSAL_LEADER_OR_VALUE");
    if (packet.view == 0) {
        Require(IsEmpty(context, nv), "VIEW_ZERO_NEW_VIEW");
    } else {
        VerifyNewView(context, nv);
        Require(nv.view == packet.view && nv.value == packet.value, "PROPOSAL_NEW_VIEW_LINK");
    }
}

void Verify(const Context& context, const Packet& packet)
{
    switch (packet.kind) {
    case Kind::EMPTY: Require(IsEmpty(context, packet), "NONCANONICAL_EMPTY"); return;
    case Kind::PREPARE: case Kind::COMMIT: VerifyVote(context, packet, packet.kind); return;
    case Kind::PC: case Kind::FAST: case Kind::SLOW: VerifyCertificate(context, packet); return;
    case Kind::V0: VerifyV0(context, packet, packet.sender); return;
    case Kind::EQ: VerifyEq(context, packet); return;
    case Kind::REPORT: VerifyReport(context, packet); return;
    case Kind::NEW_VIEW: VerifyNewView(context, packet); return;
    case Kind::PROPOSE: VerifyProposal(context, packet); return;
    }
    throw Invalid("UNKNOWN_PROOF_KIND");
}

size_t SelfCheck(const Context& context, std::span<const bls::SecretKey> keys)
{
    Require(keys.size() == N, "SELFCHECK_KEYS");
    size_t checks{0};
    const auto pass = [&](bool condition) { Require(condition, "SELFCHECK_ASSERTION"); ++checks; };
    const auto rejected = [&](const auto& operation) {
        bool failed{false};
        try { operation(); } catch (const Invalid&) { failed = true; }
        pass(failed);
    };
    const uint256 x{1}, y{2};
    const auto vote = [&](Kind kind, int32_t view, int32_t sender, uint256 value) {
        return Sign(context, keys[sender], Make(context, kind, view, sender, value));
    };
    const auto cert = [&](Kind kind, int32_t view, uint256 value, std::vector<int32_t> seats) {
        std::vector<Packet> votes;
        for (const auto seat : seats) votes.push_back(vote(kind == Kind::SLOW ? Kind::COMMIT : Kind::PREPARE,
                                                         view, seat, value));
        return Make(context, kind, view, -1, value, std::move(votes));
    };
    const auto report = [&](int32_t view, int32_t sender, Packet original, Packet pc, Packet decision) {
        return Sign(context, keys[sender], Make(context, Kind::REPORT, view, sender, std::nullopt,
                                                {std::move(original), std::move(pc), std::move(decision)}));
    };
    const auto original = [&](int32_t sender, uint256 value) {
        return Make(context, Kind::V0, 0, sender, value,
                    {vote(Kind::PREPARE, 0, sender, value), vote(Kind::PREPARE, 0, 0, value)});
    };
    const auto empty{Empty(context)};
    const auto fast{cert(Kind::FAST, 0, x, {0, 1, 2})};
    VerifyCertificate(context, fast); ++checks;
    pass(Decode(Encode(fast)) == fast);
    VerifyCertificate(context, cert(Kind::FAST, 0, x, {0, 1, 2, 3})); ++checks;
    VerifyCertificate(context, cert(Kind::PC, 0, x, {1, 2, 3})); ++checks;
    VerifyCertificate(context, cert(Kind::SLOW, 1, x, {1, 2, 3})); ++checks;
    rejected([&] { VerifyCertificate(context, cert(Kind::FAST, 0, x, {1, 2, 3})); });
    rejected([&] { VerifyCertificate(context, cert(Kind::FAST, 1, x, {0, 1, 2})); });
    rejected([&] { VerifyCertificate(context, cert(Kind::SLOW, 0, x, {0, 1, 2})); });
    rejected([&] { VerifyCertificate(context, cert(Kind::PC, 0, x, {0, 1})); });
    auto duplicate{fast}; duplicate.items[2] = duplicate.items[1];
    rejected([&] { VerifyCertificate(context, duplicate); });
    auto changed{fast}; changed.items[0].value = y;
    rejected([&] { VerifyCertificate(context, changed); });
    changed = fast; changed.items[0].signature[5] ^= 1;
    rejected([&] { VerifyCertificate(context, changed); });
    changed = fast; changed.items[0].instance.begin()[0] ^= 1;
    rejected([&] { VerifyCertificate(context, changed); });
    auto bad_empty{empty}; bad_empty.view = 0;
    rejected([&] { Verify(context, bad_empty); });
    const auto pc1{cert(Kind::PC, 1, x, {1, 2, 3})};
    rejected([&] { VerifyReport(context, report(1, 1, empty, pc1, empty)); });
    VerifyReport(context, report(2, 1, empty, pc1, empty)); ++checks;
    VerifyReport(context, report(1, 0, original(0, x), empty, empty)); ++checks;
    auto bad_v0{original(1, x)}; bad_v0.sender = 2;
    rejected([&] { VerifyReport(context, report(1, 1, bad_v0, empty, empty)); });
    auto foreign_empty{empty}; foreign_empty.instance.begin()[0] ^= 1;
    rejected([&] { VerifyReport(context, report(1, 1, foreign_empty, empty, empty)); });
    const auto eq{Make(context, Kind::EQ, 0, -1, std::nullopt,
                       {vote(Kind::PREPARE, 0, 0, x), vote(Kind::PREPARE, 0, 0, y)})};
    std::vector<Packet> reports{
        report(1, 1, original(1, x), empty, empty),
        report(1, 2, original(2, x), empty, empty),
        report(1, 3, original(3, y), empty, empty)};
    pass(Choices(context, 1, reports, eq).values == std::set<uint256>{x});
    auto nv{Sign(context, keys[1], Make(context, Kind::NEW_VIEW, 1, 1, x,
                                         {eq, reports[0], reports[1], reports[2]}))};
    VerifyNewView(context, nv); ++checks;
    rejected([&] { VerifyNewView(context, Sign(context, keys[1],
        Make(context, Kind::NEW_VIEW, 1, 1, y, {eq, reports[0], reports[1], reports[2]}))); });
    const auto proposal{Make(context, Kind::PROPOSE, 1, 1, x,
                            {vote(Kind::PREPARE, 1, 1, x), nv})};
    VerifyProposal(context, proposal); ++checks;
    pass(Decode(Encode(proposal)) == proposal);
    auto nested_changed{nv}; nested_changed.items[1] = report(1, 1, empty, empty, empty);
    rejected([&] { VerifyNewView(context, nested_changed); });
    reports[0] = report(1, 0, empty, cert(Kind::PC, 0, x, {0, 1, 2}), empty);
    rejected([&] { Choices(context, 1, reports, eq); });
    reports[0] = report(1, 1, empty, empty, fast);
    VerifyReport(context, reports[0]); ++checks;
    rejected([&] { Choices(context, 1, reports, empty); });
    reports = {report(2, 1, empty, pc1, empty),
               report(2, 2, empty, cert(Kind::PC, 1, y, {0, 2, 3}), empty),
               report(2, 3, empty, empty, empty)};
    rejected([&] { Choices(context, 2, reports, empty); });
    reports = {report(1, 1, empty, empty, empty), report(1, 2, empty, empty, empty),
               report(1, 3, empty, empty, empty)};
    pass(Choices(context, 1, reports, empty).unrestricted);
    pass(Choices(context, 1, reports, eq).unrestricted);
    reports[0] = report(1, 1, original(1, x), empty, empty);
    pass(Choices(context, 1, reports, empty).values == std::set<uint256>{x});
    pass(Choices(context, 1, reports, eq).unrestricted); // Carried EQ alone changes the rule.
    reports[0] = report(1, 0, empty, cert(Kind::PC, 0, x, {0, 1, 2}), empty);
    rejected([&] { Choices(context, 1, reports, eq); }); // EQ source report may be absent.
    reports = {report(2, 1, empty, pc1, empty),
               report(2, 2, empty, cert(Kind::PC, 0, y, {0, 2, 3}), empty),
               report(2, 3, empty, empty, empty)};
    pass(Choices(context, 2, reports, empty).values == std::set<uint256>{x});
    VerifyVote(context, vote(Kind::PREPARE, 0, 0, uint256{}), Kind::PREPARE); ++checks;
    changed = fast; changed.signature[0] = 1;
    rejected([&] { VerifyCertificate(context, changed); });
    auto encoded{Encode(fast)}; encoded.push_back(0);
    rejected([&] { Decode(encoded); });
    encoded = Encode(fast); encoded.pop_back();
    rejected([&] { Decode(encoded); });
    encoded = Encode(fast); encoded[0] = 255;
    rejected([&] { Decode(encoded); });
    Packet deep{empty};
    for (size_t i{0}; i <= MAX_TREE_DEPTH; ++i) deep = Make(context, Kind::V0, 0, 0, x, {deep});
    rejected([&] { Encode(deep); });
    auto wide{empty}; wide.items.assign(MAX_CHILDREN + 1, empty);
    rejected([&] { Encode(wide); });
    std::vector<Member> members;
    for (const auto& key : keys) members.push_back({key.GetPublicKey().Compressed(), key.SignPoP().Compressed()});
    auto duplicate_members{members}; duplicate_members[3] = duplicate_members[2];
    rejected([&] { Context invalid{context.Instance(), duplicate_members}; });
    auto wrong_pop{members}; wrong_pop[3].proof_of_possession = members[2].proof_of_possession;
    rejected([&] { Context invalid{context.Instance(), wrong_pop}; });
    std::swap(members[0], members[1]);
    const Context other_membership{context.Instance(), members};
    rejected([&] { VerifyCertificate(other_membership, fast); });
    rejected([&] { Sign(context, keys[1], Make(context, Kind::PREPARE, 0, 0, x)); });
    return checks;
}

} // namespace p2fv
