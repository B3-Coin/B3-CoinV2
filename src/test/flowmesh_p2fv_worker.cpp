// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
// TEST ONLY: fixed N=4, one executed batch, views 0..2. JSON stdin/stdout is
// driven by a test scheduler; it is NOT a transport or latency qualification.
#include <test/flowmesh_p2fv_proof.h>
#include <test/flowmesh_fastpath_probe_fixture.h>
#include <dbwrapper.h>
#include <flowmesh/auth.h>
#include <hash.h>
#include <streams.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/translation.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <unistd.h>

const TranslateFn G_TRANSLATION_FUN{nullptr};

namespace {
using namespace p2fv;
constexpr size_t MAX_LINE{1024 * 1024};
constexpr size_t MAX_STATE{8 * 1024 * 1024};
constexpr size_t MAX_BODY_BYTES{64 * 1024};
constexpr size_t MAX_PENDING{64};
constexpr uint32_t STORE_VERSION{1};

void Need(bool yes, const char* reason) { if (!yes) throw Invalid(reason); }
template <typename T> Bytes Serialize(const T& v)
{
    Bytes b; VectorWriter w{b, 0}; w << v; return b;
}
std::string Text(const UniValue& v, const char* key)
{
    Need(v.isObject() && v.exists(key) && v[key].isStr(), "MISSING_OR_WRONG_STRING");
    return v[key].get_str();
}
int64_t Number(const UniValue& v, const char* key)
{
    Need(v.isObject() && v.exists(key) && v[key].isNum(), "MISSING_OR_WRONG_NUMBER");
    return v[key].getInt<int64_t>();
}
bool Flag(const UniValue& v, const char* key)
{
    Need(v.isObject() && v.exists(key) && v[key].isBool(), "MISSING_OR_WRONG_FLAG");
    return v[key].get_bool();
}
const UniValue& Array(const UniValue& v, const char* key, size_t limit)
{
    Need(v.isObject() && v.exists(key) && v[key].isArray() && v[key].size() <= limit,
         "MISSING_OR_BOUNDED_ARRAY");
    return v[key];
}
Bytes HexBytes(const std::string& s, size_t limit)
{
    Need(s.size() <= 2 * limit && s.size() % 2 == 0 && (s.empty() || IsHex(s)), "HEX_BYTE_BOUND");
    return ParseHex(s);
}
uint256 HashText(const std::string& s)
{
    Need(s.size() == 64 && IsHex(s), "HASH_ENCODING");
    const auto h{uint256::FromHex(s)}; Need(h.has_value(), "HASH_ENCODING"); return *h;
}
std::string ProofText(const std::optional<Packet>& p) { return p ? HexStr(Encode(*p)) : ""; }
std::optional<Packet> ProofRead(const UniValue& v, const char* key)
{
    const auto s{Text(v, key)};
    if (s.empty()) return std::nullopt;
    return Decode(HexBytes(s, MAX_PROOF_BYTES));
}
Packet RequiredProof(const UniValue& v, const char* key)
{
    const auto p{ProofRead(v, key)}; Need(p.has_value(), "MISSING_PROOF"); return *p;
}
Packet Unsigned(Packet p) { p.signature = {}; return p; }

struct Body {
    Bytes request, entry, receipt;
    uint256 Value() const
    {
        HashWriter h;
        h << std::string{"B3/TEST-ONLY/P2FV-WORKER/BODY/1"} << request << entry << receipt;
        return h.GetHash();
    }
    UniValue Json() const
    {
        UniValue v{UniValue::VOBJ}; v.pushKV("value", Value().GetHex());
        v.pushKV("request", HexStr(request)); v.pushKV("entry", HexStr(entry));
        v.pushKV("receipt", HexStr(receipt)); return v;
    }
    static Body Parse(const UniValue& v)
    {
        Body b{HexBytes(Text(v, "request"), MAX_BODY_BYTES),
               HexBytes(Text(v, "entry"), MAX_BODY_BYTES),
               HexBytes(Text(v, "receipt"), MAX_BODY_BYTES)};
        if (v.exists("value")) Need(HashText(Text(v, "value")) == b.Value(), "BODY_HASH_MISMATCH");
        return b;
    }
    friend bool operator==(const Body&, const Body&) = default;
};

std::array<Body, 2> Generate(const fastprobe::Fixture& f)
{
    fastprobe::Engine engine{f};
    const auto request{fastprobe::MakeRequest(f, 0)};
    flowmesh::Action action; SpanReader reader{request}; reader >> action;
    Need(reader.empty(), "GENERATOR_REQUEST");
    std::array<Body, 2> out;
    for (size_t i{0}; i < out.size(); ++i) {
        std::array<unsigned char, 64> sig{};
        Need(f.buyer_key.SignSchnorr(flowmesh::ActionSignatureDigest(f.domain, f.ConfigId(), action),
                                   sig, nullptr, uint256{static_cast<uint8_t>(i + 1)}), "GENERATOR_SIGN");
        const XOnlyPubKey key{f.buyer_key.GetPubKey()};
        action.credential.assign(key.data(), key.data() + 32);
        action.credential.insert(action.credential.end(), sig.begin(), sig.end());
        auto x{engine.Execute(Serialize(action))};
        out[i] = {x.request_bytes, x.entry_bytes, x.receipt_bytes};
    }
    Need(out[0].request != out[1].request && out[0].entry == out[1].entry &&
         out[0].receipt == out[1].receipt && out[0].Value() != out[1].Value(), "GENERATOR_DISTINCT_VALUES");
    if (out[1].Value() < out[0].Value()) std::swap(out[0], out[1]);
    return out;
}
uint256 Instance(const fastprobe::Fixture& f)
{
    fastprobe::Engine e{f}; HashWriter h;
    h << std::string{"B3/TEST-ONLY/P2FV-WORKER/INSTANCE/1"} << f.domain << f.market
      << f.ConfigId() << f.seats.set_hash << e.Head() << e.State().Root();
    return h.GetHash();
}
std::vector<Member> Members(const fastprobe::Fixture& f)
{
    std::vector<Member> out;
    for (const auto& key : f.seat_keys) out.push_back({key.GetPublicKey().Compressed(), key.SignPoP().Compressed()});
    return out;
}

struct Guard {
    int32_t pre_view{0}, view{0};
    bool changing{false}, body_available{false}, fenced{false};
    std::optional<Packet> proposal, new_view, prepared, highest, v0, decision;
    UniValue Json() const
    {
        UniValue v{UniValue::VOBJ}; v.pushKV("pre_view", pre_view); v.pushKV("view", view);
        v.pushKV("changing", changing); v.pushKV("body_available", body_available); v.pushKV("fenced", fenced);
        v.pushKV("proposal", ProofText(proposal)); v.pushKV("new_view", ProofText(new_view));
        v.pushKV("prepared", ProofText(prepared)); v.pushKV("highest", ProofText(highest));
        v.pushKV("v0", ProofText(v0)); v.pushKV("decision", ProofText(decision)); return v;
    }
    static Guard Parse(const UniValue& v)
    {
        const auto pre{Number(v, "pre_view")}, view{Number(v, "view")};
        Need(pre >= 0 && pre <= MAX_VIEW && view >= 0 && view <= MAX_VIEW, "GUARD_VIEW_BOUND");
        return {static_cast<int32_t>(pre), static_cast<int32_t>(view), Flag(v, "changing"),
                Flag(v, "body_available"), Flag(v, "fenced"), ProofRead(v, "proposal"),
                ProofRead(v, "new_view"), ProofRead(v, "prepared"), ProofRead(v, "highest"),
                ProofRead(v, "v0"), ProofRead(v, "decision")};
    }
};
struct Record {
    Packet packet;
    Guard guard;
    bool leader{false};
    UniValue Json() const
    {
        UniValue v{UniValue::VOBJ}; v.pushKV("packet", HexStr(Encode(packet)));
        v.pushKV("guard", guard.Json()); v.pushKV("leader", leader); return v;
    }
    static Record Parse(const UniValue& v)
    {
        Need(v.exists("guard"), "MISSING_GUARD");
        return {RequiredProof(v, "packet"), Guard::Parse(v["guard"]), Flag(v, "leader")};
    }
};
using Slot = std::pair<Kind, int32_t>;
using VoteKey = std::tuple<Kind, int32_t, uint256>;

// The serialized JSON blob is itself length bounded before allocation. There
// are exactly two database records, identity and state; neither may disappear.
struct Blob {
    std::string text;
    template <class S> void Serialize(S& s) const { s << text; }
    template <class S> void Unserialize(S& s)
    {
        const auto n{ReadCompactSize(s)}; Need(n <= MAX_STATE, "STORE_BYTE_BOUND");
        text.resize(n); if (n) s.read(std::as_writable_bytes(std::span{text}));
    }
};

class Worker {
    fastprobe::Fixture m_fixture;
    Context m_context;
    std::array<Body, 2> m_universe;
    fastprobe::Engine m_base, m_engine;
    CDBWrapper m_db;
    int32_t m_seat;
    bool m_byzantine;
    bool m_stopped_store_tool;
    int32_t m_view{0};
    bool m_changing{false}, m_fenced{false};
    std::string m_reason, m_cut;
    std::map<uint256, Body> m_bodies;
    std::map<uint256, std::unique_ptr<fastprobe::Execution>> m_execution;
    std::map<int32_t, Packet> m_accepted, m_new_views;
    std::optional<Packet> m_highest, m_v0, m_decision;
    std::map<Slot, Record> m_signed;
    std::optional<Record> m_intent;
    std::map<VoteKey, std::map<int32_t, Packet>> m_votes;
    std::map<int32_t, std::map<int32_t, Packet>> m_reports;
    std::vector<Packet> m_pending, m_byzantine_signed;
    std::set<std::tuple<Kind, int32_t, uint256>> m_sent;
    std::optional<uint256> m_applied;
    uint32_t m_apply_count{0};
    Bytes m_snapshot;
    uint64_t m_now{0}, m_deadline{3}, m_last_retry{0};
    UniValue m_events{UniValue::VARR};
    bool m_storage_usable{true};

    uint256 Identity() const
    {
        HashWriter h; h << std::string{"B3/TEST-ONLY/P2FV-WORKER/STORE/1"}
                        << STORE_VERSION << m_context.Instance() << m_context.MembershipHash()
                        << m_seat << m_byzantine; return h.GetHash();
    }
    bool KnownValue(const uint256& value) const
    {
        return std::any_of(m_universe.begin(), m_universe.end(), [&](const Body& b) { return b.Value() == value; });
    }
    void KnownTree(const Packet& p) const
    {
        if (p.value) Need(KnownValue(*p.value), "VALUE_OUTSIDE_FIXED_TWO_BODY_PROFILE");
        for (const auto& child : p.items) KnownTree(child);
    }
    void VerifyPacket(const Packet& p) const { Verify(m_context, p); KnownTree(p); }
    void CheckLocalEvidence(const Packet& p, bool allow_intent = false) const
    {
        if (IsSigned(p.kind) && p.sender == m_seat) {
            const auto it{m_signed.find({p.kind, p.view})};
            const bool retained{it != m_signed.end() && it->second.packet == p};
            const bool adversarial{m_byzantine && std::find(m_byzantine_signed.begin(), m_byzantine_signed.end(), p) != m_byzantine_signed.end()};
            const bool planned{allow_intent && m_intent && m_intent->packet == p &&
                p.signature == std::array<unsigned char, bls::SIGNATURE_SIZE>{}};
            Need(retained || adversarial || planned, "LOCAL_PROOF_WITHOUT_EXACT_SIGNING_RECORD");
        }
        for (const auto& child : p.items) CheckLocalEvidence(child, allow_intent);
    }
    UniValue State() const
    {
        UniValue v{UniValue::VOBJ}; v.pushKV("version", STORE_VERSION);
        v.pushKV("view", m_view); v.pushKV("changing", m_changing); v.pushKV("fenced", m_fenced);
        v.pushKV("reason", m_reason); v.pushKV("highest", ProofText(m_highest));
        v.pushKV("v0", ProofText(m_v0)); v.pushKV("decision", ProofText(m_decision));
        v.pushKV("applied", m_applied ? m_applied->GetHex() : ""); v.pushKV("apply_count", m_apply_count);
        v.pushKV("snapshot", HexStr(m_snapshot));
        UniValue bodies{UniValue::VARR}, signed_rows{UniValue::VARR}, accepted{UniValue::VARR},
                 nvs{UniValue::VARR}, votes{UniValue::VARR}, reports{UniValue::VARR},
                 pending{UniValue::VARR}, byz{UniValue::VARR};
        for (const auto& [_, b] : m_bodies) bodies.push_back(b.Json());
        for (const auto& [_, r] : m_signed) signed_rows.push_back(r.Json());
        for (const auto& [_, p] : m_accepted) accepted.push_back(HexStr(Encode(p)));
        for (const auto& [_, p] : m_new_views) nvs.push_back(HexStr(Encode(p)));
        for (const auto& [_, by_seat] : m_votes) for (const auto& [__, p] : by_seat) votes.push_back(HexStr(Encode(p)));
        for (const auto& [_, by_seat] : m_reports) for (const auto& [__, p] : by_seat) reports.push_back(HexStr(Encode(p)));
        for (const auto& p : m_pending) pending.push_back(HexStr(Encode(p)));
        for (const auto& p : m_byzantine_signed) byz.push_back(HexStr(Encode(p)));
        v.pushKV("bodies", std::move(bodies)); v.pushKV("signed", std::move(signed_rows));
        v.pushKV("accepted", std::move(accepted)); v.pushKV("new_views", std::move(nvs));
        v.pushKV("votes", std::move(votes)); v.pushKV("reports", std::move(reports));
        v.pushKV("pending", std::move(pending)); v.pushKV("byzantine_signed", std::move(byz));
        v.pushKV("intent", m_intent ? m_intent->Json() : UniValue{}); return v;
    }
    void Persist()
    {
        try {
            Need(m_storage_usable, "STORE_ALREADY_FAILED");
            if (m_cut == "fail_signed_state_serialize" && !m_intent && !m_signed.empty()) {
                m_cut.clear(); throw Invalid("INJECTED_SIGNED_STATE_SERIALIZATION_FAILURE");
            }
            const auto json{State().write()}; Need(json.size() <= MAX_STATE, "STATE_BYTE_BOUND");
            CDBBatch batch{m_db}; batch.Write(std::string{"identity"}, Identity());
            batch.Write(std::string{"state"}, Blob{json}); m_db.WriteBatch(batch, true);
        } catch (...) { m_storage_usable = false; m_fenced = true; m_reason = "PERSISTENCE_FAILED"; throw; }
    }
    void Fence(const std::string& reason)
    {
        m_fenced = true; m_reason = reason; if (m_storage_usable) Persist();
    }
    void Cut(const char* point)
    {
        if (m_cut != point) return;
        // Publication is the actual flushed JSON response, never an in-memory
        // event. At this cut the scheduler can observe the exact durable vote.
        if (m_cut == "after_publish") {
            auto response{Response(true)}; response.pushKV("partial", true);
            std::cout << response.write() << std::endl;
        }
        _exit(73);
    }
    void NeedBody(const uint256& value)
    {
        UniValue e{UniValue::VOBJ}; e.pushKV("need", value.GetHex()); m_events.push_back(std::move(e));
    }
    bool RequireBody(const Packet& p)
    {
        Need(p.value.has_value(), "MESSAGE_WITHOUT_VALUE");
        if (m_bodies.contains(*p.value)) return true;
        if (std::find(m_pending.begin(), m_pending.end(), p) == m_pending.end()) {
            Need(m_pending.size() < MAX_PENDING, "PENDING_BOUND"); m_pending.push_back(p); Persist();
        }
        NeedBody(*p.value); return false;
    }
    void Issued(const Packet& p)
    {
        if (IsSigned(p.kind) && p.sender == m_seat) {
            const auto it{m_signed.find({p.kind, p.view})};
            if (it != m_signed.end() && it->second.packet == p) {
                UniValue e{UniValue::VOBJ}; e.pushKV("issued", HexStr(Encode(p)));
                e.pushKV("guard", it->second.guard.Json()); m_events.push_back(std::move(e));
            } else {
                Need(m_byzantine && std::find(m_byzantine_signed.begin(), m_byzantine_signed.end(), p) != m_byzantine_signed.end(),
                     "PUBLICATION_WITHOUT_DURABLE_LOCAL_SIGNATURE");
                UniValue e{UniValue::VOBJ}; e.pushKV("issued", HexStr(Encode(p))); e.pushKV("byzantine", true);
                m_events.push_back(std::move(e));
            }
        }
        for (const auto& child : p.items) Issued(child);
    }
    void Emit(const Packet& p)
    {
        VerifyPacket(p); Issued(p);
        UniValue e{UniValue::VOBJ}; e.pushKV("proof", HexStr(Encode(p))); m_events.push_back(std::move(e));
        Need(m_events.size() <= 1024, "EVENT_BOUND");
    }
    Guard MakeGuard(std::optional<Packet> proposal = std::nullopt,
                    std::optional<Packet> prepared = std::nullopt,
                    std::optional<int32_t> target = std::nullopt) const
    {
        const int32_t view{target.value_or(m_view)};
        const auto nv{m_new_views.find(view)};
        return {m_view, view, target.has_value() || m_changing,
                proposal && proposal->value && m_bodies.contains(*proposal->value), m_fenced,
                proposal, nv == m_new_views.end() ? std::nullopt : std::optional<Packet>{nv->second},
                prepared, m_highest, m_v0, m_decision};
    }
    void ValidateGuard(const Record& r, bool intent) const
    {
        const auto& p{r.packet}; const auto& g{r.guard};
        Need(IsSigned(p.kind) && p.sender == m_seat && p.instance == m_context.Instance() &&
             p.view >= 0 && p.view <= MAX_VIEW && !g.fenced && !g.decision,
             "SIGN_GUARD_AUTHORITY");
        KnownTree(p);
        if (!intent) VerifyPacket(p);
        else Need(p.signature == std::array<unsigned char, bls::SIGNATURE_SIZE>{}, "INTENT_ALREADY_SIGNED");
        if (g.highest) { VerifyPacket(*g.highest); Need(g.highest->kind == Kind::PC, "GUARD_HIGHEST_NOT_PC"); }
        if (g.v0) { VerifyPacket(*g.v0); Need(g.v0->kind == Kind::V0 && g.v0->sender == m_seat, "GUARD_V0"); }
        if (g.new_view) { VerifyNewView(m_context, *g.new_view); Need(g.new_view->view == g.view, "GUARD_NEW_VIEW_TARGET"); }
        const auto retained_report = [&](int32_t view) {
            const auto it{m_signed.find({Kind::REPORT, view})};
            if (it == m_signed.end()) return false;
            const auto& report{it->second.packet}; VerifyReport(m_context, report);
            Need(report.view == view && report.sender == m_seat, "GUARD_REPORT_AUTHORITY_CONTEXT");
            return true;
        };
        if (g.pre_view > 0) {
            bool installed{false};
            if (const auto it{m_new_views.find(g.pre_view)}; it != m_new_views.end()) {
                VerifyNewView(m_context, it->second);
                Need(it->second.view == g.pre_view, "GUARD_INSTALLED_VIEW_CONTEXT"); installed = true;
            }
            Need(retained_report(g.pre_view) || installed, "MISSING_PRE_VIEW_TRANSITION_AUTHORITY");
        }
        if (p.kind == Kind::PREPARE || p.kind == Kind::COMMIT) {
            Need(g.pre_view == p.view && g.view == p.view && !g.changing && g.proposal &&
                 g.body_available && p.value && m_bodies.contains(*p.value) &&
                 p.items.empty() && g.proposal->value == p.value && g.proposal->view == p.view, "VOTE_GUARD_CONTEXT");
            if (intent && r.leader) {
                Need(p.kind == Kind::PREPARE && m_seat == p.view % 4 && g.proposal->kind == Kind::PROPOSE &&
                     g.proposal->sender == m_seat && g.proposal->instance == m_context.Instance() &&
                     g.proposal->signature == std::array<unsigned char, bls::SIGNATURE_SIZE>{} &&
                     g.proposal->items.size() == 2 && g.proposal->items[0] == p, "LEADER_INTENT_PROPOSAL");
                if (p.view == 0) Verify(m_context, g.proposal->items[1]);
            } else VerifyProposal(m_context, *g.proposal);
            if (p.view == 0) {
                Need(!g.new_view && g.proposal->items[1] == Empty(m_context), "V0_HAS_NEW_VIEW");
            } else {
                Need(g.new_view && g.new_view->value == p.value && g.proposal->items[1] == *g.new_view,
                     "VOTE_MISSING_NEW_VIEW_JUSTIFICATION");
            }
            if (p.kind == Kind::COMMIT) {
                Need(p.view > 0 && g.prepared && g.prepared->kind == Kind::PC &&
                     g.prepared->view == p.view && g.prepared->value == p.value && !r.leader,
                     "COMMIT_GUARD_PC"); VerifyCertificate(m_context, *g.prepared);
                Need(g.highest && g.highest->view >= g.prepared->view &&
                     (g.highest->view != g.prepared->view || g.highest->value == g.prepared->value),
                     "COMMIT_GUARD_LOST_HIGHEST_PC");
            } else Need(!g.prepared, "PREPARE_WITH_PREPARED_GUARD");
            Need(r.leader == (p.kind == Kind::PREPARE && m_seat == p.view % 4), "LEADER_GUARD_FLAG");
        } else if (p.kind == Kind::REPORT) {
            Need(!r.leader && p.view == g.view && p.view > g.pre_view && g.changing &&
                 !g.proposal && !g.prepared && !g.body_available && !p.value && p.items.size() == 3 &&
                 p.items[0] == g.v0.value_or(Empty(m_context)) &&
                 p.items[1] == g.highest.value_or(Empty(m_context)) &&
                 p.items[2] == g.decision.value_or(Empty(m_context)), "REPORT_GUARD_OBLIGATIONS");
            Need(!g.highest || p.view > g.highest->view, "REPORT_MUST_ADVANCE_PAST_PC");
        } else {
            Need(p.kind == Kind::NEW_VIEW && !r.leader && p.view == g.view && g.pre_view == p.view &&
                 g.changing && m_seat == p.view % 4 && !g.proposal && !g.prepared && !g.body_available &&
                 p.value && m_bodies.contains(*p.value), "NEW_VIEW_GUARD_CONTEXT");
            Need(retained_report(p.view), "NEW_VIEW_WITHOUT_CHANGING_REPORT_AUTHORITY");
            Need(p.items.size() == 4 && Choices(m_context, p.view,
                std::span<const Packet>{p.items.data() + 1, 3}, p.items[0]).Allows(*p.value), "NEW_VIEW_GUARD_CHOICE");
        }
    }
    Packet FinishIntent()
    {
        Need(m_intent.has_value() && !m_fenced, "NO_AUTHORIZED_INTENT");
        Record r{*m_intent}; ValidateGuard(r, true);
        Need(r.guard.pre_view == m_view && r.guard.highest == m_highest && r.guard.v0 == m_v0 &&
             r.guard.decision == m_decision && !m_signed.contains({r.packet.kind, r.packet.view}), "INTENT_PRESTATE_CHANGED");
        const auto nv{m_new_views.find(r.guard.view)};
        Need(r.guard.new_view == (nv == m_new_views.end() ? std::nullopt : std::optional<Packet>{nv->second}),
             "INTENT_NEW_VIEW_CHANGED");
        if (r.packet.kind == Kind::PREPARE || r.packet.kind == Kind::COMMIT)
            Need(!m_changing, "INTENT_VOTE_AFTER_VIEW_CHANGE");
        if (r.packet.kind == Kind::NEW_VIEW) Need(m_changing, "INTENT_NEW_VIEW_NOT_CHANGING");
        Cut("before_compute");
        try {
            r.packet = Sign(m_context, m_fixture.seat_keys[m_seat], r.packet);
            Cut("after_compute_before_durable");
            if (r.leader) r.guard.proposal->items[0] = r.packet;
            ValidateGuard(r, false);
            if (r.packet.kind == Kind::PREPARE) {
                m_accepted.insert_or_assign(r.packet.view, *r.guard.proposal);
                if (r.packet.view == 0) m_v0 = Make(m_context, Kind::V0, 0, m_seat, r.packet.value,
                                                   {r.packet, r.guard.proposal->items[0]});
            } else if (r.packet.kind == Kind::REPORT) {
                m_view = r.packet.view; m_changing = true;
            }
            if (m_cut == "fail_signed_record_insert") {
                m_cut.clear(); throw std::bad_alloc{}; // Injection, not an actual-OOM qualification.
            }
            m_signed.emplace(Slot{r.packet.kind, r.packet.view}, r); m_intent.reset(); Persist();
        } catch (...) {
            const bool already_unusable{!m_storage_usable};
            m_fenced = true; m_storage_usable = false;
            if (!already_unusable) m_reason = "SIGNED_STAGING_FAILED";
            throw;
        }
        Cut("after_durable_before_publish");
        Emit(r.leader ? *r.guard.proposal : r.packet); Cut("after_publish"); return r.packet;
    }
    Packet SignLocal(Packet p, Guard guard, bool leader = false)
    {
        const auto old{m_signed.find({p.kind, p.view})};
        if (old != m_signed.end()) {
            Need(Unsigned(old->second.packet) == p, "DURABLE_SIGNING_SLOT_ALREADY_USED"); return old->second.packet;
        }
        Need(!m_fenced && !m_intent && m_signed.size() < 12, "SIGNING_FENCED_OR_BOUND");
        Record r{p, std::move(guard), leader}; ValidateGuard(r, true);
        m_intent = r; Persist(); Cut("after_intent"); return FinishIntent();
    }
    void Propose(const uint256& value)
    {
        if (m_seat != m_view % 4 || m_changing || m_decision || !m_bodies.contains(value) ||
            m_signed.contains({Kind::PREPARE, m_view})) return;
        const auto found{m_new_views.find(m_view)};
        const auto nv{found == m_new_views.end() ? Empty(m_context) : found->second};
        if (m_view) { VerifyNewView(m_context, nv); Need(nv.value == value, "LEADER_MUST_PROPOSE_NEW_VIEW_VALUE"); }
        const auto vote{Make(m_context, Kind::PREPARE, m_view, m_seat, value)};
        const auto proposal{Make(m_context, Kind::PROPOSE, m_view, m_seat, value, {vote, nv})};
        SignLocal(vote, MakeGuard(proposal), true);
    }
    void AddVote(const Packet& p)
    {
        auto& row{m_votes[{p.kind, p.view, *p.value}]};
        if (const auto it{row.find(p.sender)}; it != row.end()) {
            Need(it->second == p, "DIFFERENT_SIGNATURE_FOR_SAME_VOTE"); return;
        }
        row.emplace(p.sender, p); Persist();
    }
    std::vector<Packet> Votes(Kind kind, int32_t view, const uint256& value) const
    {
        std::vector<Packet> out;
        if (const auto it{m_votes.find({kind, view, value})}; it != m_votes.end())
            for (const auto& [_, p] : it->second) out.push_back(p);
        return out;
    }
    void Aggregate(int32_t view, const uint256& value)
    {
        const auto prepares{Votes(Kind::PREPARE, view, value)};
        const auto commits{Votes(Kind::COMMIT, view, value)};
        if (view == 0 && prepares.size() >= FAST_QUORUM &&
            std::any_of(prepares.begin(), prepares.end(), [](const Packet& p) { return p.sender == 0; }))
            Decide(Make(m_context, Kind::FAST, view, -1, value, prepares));
        if (prepares.size() >= Q) OnPC(Make(m_context, Kind::PC, view, -1, value, prepares));
        if (view > 0 && commits.size() >= Q) Decide(Make(m_context, Kind::SLOW, view, -1, value, commits));
    }
    void OnProposal(const Packet& p)
    {
        VerifyProposal(m_context, p); if (!RequireBody(p)) return;
        if (p.view > 0 && p.view >= m_view) InstallView(p.items[1], false);
        AddVote(p.items[0]);
        if (p.view != m_view || m_changing || m_decision) { Aggregate(p.view, *p.value); return; }
        const auto old{m_accepted.find(m_view)};
        Need(old == m_accepted.end() || old->second.value == p.value, "ACCEPTED_DIFFERENT_PROPOSAL");
        if (!m_signed.contains({Kind::PREPARE, m_view}))
            SignLocal(Make(m_context, Kind::PREPARE, m_view, m_seat, p.value), MakeGuard(p), m_seat == m_view % 4);
        Aggregate(p.view, *p.value);
    }
    void OnPC(const Packet& p)
    {
        VerifyCertificate(m_context, p); Need(p.kind == Kind::PC, "NOT_PC");
        if (!RequireBody(p)) return;
        if (m_highest && m_highest->view == p.view && m_highest->value != p.value) {
            Fence("CONFLICTING_PC_SAFETY_STOP"); throw Invalid(m_reason);
        }
        if (!m_highest || p.view > m_highest->view) { m_highest = p; Persist(); }
        if (m_sent.emplace(Kind::PC, p.view, *p.value).second) Emit(p);
        const auto accepted{m_accepted.find(m_view)};
        if (!m_changing && p.view == m_view && accepted != m_accepted.end() &&
            accepted->second.value == p.value && !m_decision && m_view > 0)
            SignLocal(Make(m_context, Kind::COMMIT, m_view, m_seat, p.value), MakeGuard(accepted->second, p));
    }
    void Apply()
    {
        if (!m_decision || !m_bodies.contains(*m_decision->value) || m_applied) return;
        Cut("during_apply_before_atomic_commit");
        const auto it{m_execution.find(*m_decision->value)};
        Need(it != m_execution.end() && it->second, "MISSING_VALIDATED_EXECUTION");
        m_snapshot = Serialize(it->second->next_state);
        m_applied = m_decision->value; m_apply_count = 1;
        Persist(); // decision + exact resulting state + count in one synced batch.
        m_engine.Apply(std::move(*it->second)); it->second.reset(); Cut("after_apply");
    }
    void Decide(const Packet& p)
    {
        VerifyCertificate(m_context, p); Need(p.kind == Kind::FAST || p.kind == Kind::SLOW, "NOT_DECISION");
        if (!RequireBody(p)) return;
        if (m_decision && m_decision->value != p.value) { Fence("CONFLICTING_DECISION"); throw Invalid(m_reason); }
        if (!m_decision) { m_decision = p; Persist(); Cut("after_decision_before_apply"); }
        Apply(); if (m_sent.emplace(Kind::FAST, -1, *p.value).second) Emit(*m_decision);
    }
    void ChangeView(int32_t target)
    {
        if (target <= m_view || target > MAX_VIEW || m_decision || m_fenced) return;
        if (m_highest) {
            VerifyCertificate(m_context, *m_highest); Need(m_highest->kind == Kind::PC, "HIGHEST_NOT_PC");
            target = std::max(target, m_highest->view + 1);
            if (target > MAX_VIEW) { m_reason = "TEST_VIEW_BOUND_REQUIRED_BY_RETAINED_PC"; Persist(); return; }
        }
        const auto report{Make(m_context, Kind::REPORT, target, m_seat, std::nullopt,
                              {m_v0.value_or(Empty(m_context)), m_highest.value_or(Empty(m_context)),
                               m_decision.value_or(Empty(m_context))})};
        SignLocal(report, MakeGuard(std::nullopt, std::nullopt, target)); m_deadline = m_now + 3;
    }
    void OnReport(const Packet& r)
    {
        VerifyReport(m_context, r);
        auto& reports{m_reports[r.view]};
        if (reports.emplace(r.sender, r).second) Persist();
        if (r.items[2].kind != Kind::EMPTY) Decide(r.items[2]);
        if (r.view > m_view && reports.size() >= 2) ChangeView(r.view);
        if (m_seat == r.view % 4 && r.view == m_view) MakeView(r.view);
    }
    void MakeView(int32_t view)
    {
        if (m_decision || m_signed.contains({Kind::NEW_VIEW, view}) || !m_changing) return;
        const auto found{m_reports.find(view)};
        if (found == m_reports.end() || found->second.size() < Q) return;
        std::map<uint256, Packet> originals;
        std::vector<Packet> reports;
        for (const auto& [_, r] : found->second) {
            reports.push_back(r);
            if (r.items[0].kind == Kind::V0) originals.emplace(*r.items[0].items[1].value, r.items[0].items[1]);
        }
        const auto eq{originals.size() > 1 ? Make(m_context, Kind::EQ, 0, -1, std::nullopt,
            {originals.begin()->second, originals.rbegin()->second}) : Empty(m_context)};
        for (size_t a{0}; a + 2 < reports.size(); ++a)
            for (size_t b{a + 1}; b + 1 < reports.size(); ++b)
                for (size_t c{b + 1}; c < reports.size(); ++c) {
                    const std::array selection{reports[a], reports[b], reports[c]};
                    Choice choice;
                    try { choice = Choices(m_context, view, selection, eq); } catch (const Invalid&) { continue; }
                    std::optional<uint256> chosen;
                    for (const auto& [value, _] : m_bodies) if (choice.Allows(value)) { chosen = value; break; }
                    if (!chosen) {
                        for (const auto& body : m_universe) if (choice.Allows(body.Value())) NeedBody(body.Value());
                        return;
                    }
                    const auto nv{Make(m_context, Kind::NEW_VIEW, view, m_seat, chosen,
                                       {eq, selection[0], selection[1], selection[2]})};
                    SignLocal(nv, MakeGuard()); return;
                }
    }
    void InstallView(const Packet& nv, bool drive = true)
    {
        VerifyNewView(m_context, nv); if (nv.view < m_view || m_decision) return;
        const auto old{m_new_views.find(nv.view)};
        Need(old == m_new_views.end() || old->second == nv, "DIFFERENT_NEW_VIEW_ALREADY_INSTALLED");
        if (!RequireBody(nv)) return;
        m_view = nv.view; m_changing = false; m_new_views.insert_or_assign(nv.view, nv); Persist();
        if (drive) Propose(*nv.value);
    }
    void Receive(const Packet& p)
    {
        if (m_fenced) return;
        VerifyPacket(p); CheckLocalEvidence(p);
        switch (p.kind) {
        case Kind::PROPOSE: OnProposal(p); break;
        case Kind::PREPARE: case Kind::COMMIT: AddVote(p); Aggregate(p.view, *p.value); break;
        case Kind::PC: OnPC(p); break;
        case Kind::FAST: case Kind::SLOW: Decide(p); break;
        case Kind::REPORT: OnReport(p); break;
        case Kind::NEW_VIEW: InstallView(p); break;
        default: throw Invalid("UNKNOWN_MESSAGE");
        }
    }
    void ValidateBody(const Body& body)
    {
        Need(KnownValue(body.Value()), "BODY_OUTSIDE_FIXED_TWO_BODY_PROFILE");
        auto x{std::make_unique<fastprobe::Execution>(m_base.Execute(body.request, body.entry))};
        Need(x->request_bytes == body.request && x->entry_bytes == body.entry && x->receipt_bytes == body.receipt &&
             x->entry_sequence == 1, "BODY_EXECUTION_MISMATCH");
        fastprobe::ValidateReceipt(m_fixture, body.request, body.entry, body.receipt);
        m_execution.insert_or_assign(body.Value(), std::move(x));
    }
    void BodyArrived(const Body& body)
    {
        if (m_fenced) return;
        const auto value{body.Value()};
        if (!m_bodies.contains(value)) {
            Need(m_bodies.size() < 2, "BODY_COUNT_BOUND"); ValidateBody(body); m_bodies.emplace(value, body); Persist();
        } else Need(m_bodies.at(value) == body, "CONFLICTING_BODY_BYTES");
        if (m_byzantine) return; // The model never drives an honest Node for a Byzantine seat.
        auto pending{std::move(m_pending)}; m_pending.clear(); Persist();
        std::sort(pending.begin(), pending.end(), [](const Packet& a, const Packet& b) {
            return std::tuple{a.view, std::string{KindName(a.kind)}, a.value} <
                   std::tuple{b.view, std::string{KindName(b.kind)}, b.value}; });
        for (const auto& p : pending) {
            try { Receive(p); } catch (const Invalid& e) { UniValue event{UniValue::VOBJ}; event.pushKV("rejected", e.what()); m_events.push_back(std::move(event)); }
        }
        if (m_decision) Apply();
        else if (m_view == 0) Propose(value);
        else if (m_new_views.contains(m_view)) Propose(*m_new_views.at(m_view).value);
        else if (m_changing && m_seat == m_view % 4) MakeView(m_view);
    }
    void Retransmit()
    {
        if (m_fenced || m_byzantine) return;
        if (m_decision) { Emit(*m_decision); return; }
        // Sort by the model's textual phase names, then view.
        std::vector<const Record*> records;
        for (const auto& [_, r] : m_signed) records.push_back(&r);
        std::sort(records.begin(), records.end(), [](const Record* a, const Record* b) {
            return std::pair{std::string{KindName(a->packet.kind)}, a->packet.view} <
                   std::pair{std::string{KindName(b->packet.kind)}, b->packet.view}; });
        for (const auto* r : records) Emit(r->leader ? m_accepted.at(r->packet.view) : r->packet);
        for (const auto& p : m_pending) NeedBody(*p.value);
    }
    void Timer(uint64_t now)
    {
        Need(now >= m_now && now <= 1000000, "TEST_CLOCK_BOUND_OR_ROLLBACK"); m_now = now;
        if (m_fenced || m_byzantine) return;
        if (m_now >= m_deadline && !m_bodies.empty() && !m_decision) {
            if (m_view < MAX_VIEW) ChangeView(m_view + 1);
            else { m_reason = "TEST_VIEW_BOUND_REACHED_WITH_OBLIGATIONS_PRESERVED"; Persist(); }
        }
        if (m_now > m_last_retry) {
            m_last_retry = m_now; Retransmit();
            if (m_changing && m_seat == m_view % 4) MakeView(m_view);
        }
    }
    void Load(const UniValue& v)
    {
        Need(Number(v, "version") == STORE_VERSION, "STORE_VERSION");
        const auto view{Number(v, "view")}; Need(view >= 0 && view <= MAX_VIEW, "STORE_VIEW_BOUND");
        m_view = view; m_changing = Flag(v, "changing"); m_fenced = Flag(v, "fenced"); m_reason = Text(v, "reason");
        m_highest = ProofRead(v, "highest"); m_v0 = ProofRead(v, "v0"); m_decision = ProofRead(v, "decision");
        const auto applied{Text(v, "applied")}; if (!applied.empty()) m_applied = HashText(applied);
        const auto count{Number(v, "apply_count")}; Need(count >= 0 && count <= 1, "STORE_APPLY_COUNT"); m_apply_count = count;
        m_snapshot = HexBytes(Text(v, "snapshot"), MAX_BODY_BYTES);
        for (const auto& row : Array(v, "bodies", 2).getValues()) {
            auto b{Body::Parse(row)}; Need(m_bodies.emplace(b.Value(), b).second, "DUPLICATE_BODY");
        }
        for (const auto& row : Array(v, "signed", 12).getValues()) {
            auto r{Record::Parse(row)};
            Need(m_signed.emplace(Slot{r.packet.kind, r.packet.view}, r).second, "DUPLICATE_SIGNING_SLOT");
        }
        auto packet_array = [&](const char* key, size_t limit, const auto& add) {
            for (const auto& row : Array(v, key, limit).getValues()) {
                Need(row.isStr(), "STORE_PACKET_TYPE"); add(Decode(HexBytes(row.get_str(), MAX_PROOF_BYTES)));
            }
        };
        packet_array("accepted", 3, [&](Packet p) { Need(m_accepted.emplace(p.view, p).second, "DUPLICATE_ACCEPTED"); });
        packet_array("new_views", 2, [&](Packet p) { Need(m_new_views.emplace(p.view, p).second, "DUPLICATE_NEW_VIEW"); });
        packet_array("votes", 48, [&](Packet p) {
            Need(p.value.has_value(), "STORED_VOTE_NO_VALUE");
            Need(m_votes[{p.kind, p.view, *p.value}].emplace(p.sender, p).second, "DUPLICATE_STORED_VOTE"); });
        packet_array("reports", 8, [&](Packet p) { Need(m_reports[p.view].emplace(p.sender, p).second, "DUPLICATE_REPORT"); });
        packet_array("pending", MAX_PENDING, [&](Packet p) {
            Need(std::find(m_pending.begin(), m_pending.end(), p) == m_pending.end(), "DUPLICATE_PENDING"); m_pending.push_back(p); });
        packet_array("byzantine_signed", 12, [&](Packet p) { m_byzantine_signed.push_back(p); });
        Need(v.exists("intent"), "MISSING_INTENT_FIELD"); if (!v["intent"].isNull()) m_intent = Record::Parse(v["intent"]);
    }
    void Revalidate()
    {
        for (const auto& [_, body] : m_bodies) ValidateBody(body);
        for (const auto& [view, p] : m_accepted) {
            VerifyPacket(p); Need(p.kind == Kind::PROPOSE && p.view == view && view <= m_view &&
                m_bodies.contains(*p.value) && m_signed.contains({Kind::PREPARE, view}), "ACCEPTED_WITHOUT_LOCAL_VOTE");
        }
        for (const auto& [view, p] : m_new_views) {
            VerifyPacket(p); Need(p.kind == Kind::NEW_VIEW && p.view == view && view <= m_view &&
                m_bodies.contains(*p.value), "STORED_NEW_VIEW_CONTEXT");
        }
        if (m_highest) { VerifyPacket(*m_highest); Need(m_highest->kind == Kind::PC && m_bodies.contains(*m_highest->value), "STORED_HIGHEST"); }
        if (m_v0) { VerifyPacket(*m_v0); Need(m_v0->kind == Kind::V0 && m_v0->sender == m_seat, "STORED_ORIGINAL"); }
        if (m_decision) { VerifyPacket(*m_decision); Need((m_decision->kind == Kind::FAST || m_decision->kind == Kind::SLOW) &&
            m_bodies.contains(*m_decision->value), "STORED_DECISION"); }
        for (const auto& [slot, r] : m_signed) {
            ValidateGuard(r, false); Need(r.packet.view <= m_view, "SIGNING_HISTORY_AHEAD_OF_VIEW");
            if (r.guard.highest) Need(m_highest && m_highest->view >= r.guard.highest->view &&
                (m_highest->view != r.guard.highest->view || m_highest->value == r.guard.highest->value), "LOST_HIGHEST_OBLIGATION");
            if (r.guard.v0) Need(m_v0 == r.guard.v0, "LOST_ORIGINAL_VOTE_OBLIGATION");
            if (r.packet.kind == Kind::PREPARE || r.packet.kind == Kind::COMMIT) {
                Need(m_accepted.contains(r.packet.view) && m_accepted.at(r.packet.view) == *r.guard.proposal, "LOST_PROPOSAL_JUSTIFICATION");
                if (r.packet.view > 0) Need(m_new_views.contains(r.packet.view) &&
                    m_new_views.at(r.packet.view) == *r.guard.new_view, "LOST_NEW_VIEW_JUSTIFICATION");
            }
        }
        const auto original{m_signed.find({Kind::PREPARE, 0})};
        if (original != m_signed.end()) {
            Need(m_accepted.contains(0) && m_v0 == Make(m_context, Kind::V0, 0, m_seat, original->second.packet.value,
                 {original->second.packet, m_accepted.at(0).items[0]}), "MISSING_OR_INCONSISTENT_ORIGINAL_VOTE");
        } else Need(!m_v0, "ORIGINAL_WITHOUT_SIGNING_RECORD");
        if (m_view == 0) Need(!m_changing, "CHANGING_VIEW_ZERO");
        else if (m_changing) Need(m_signed.contains({Kind::REPORT, m_view}), "CHANGING_WITHOUT_REPORT");
        else Need(m_new_views.contains(m_view), "ACTIVE_VIEW_WITHOUT_NEW_VIEW");
        for (const auto& [_, votes] : m_votes) for (const auto& [__, p] : votes) {
            VerifyPacket(p); Need(p.kind == Kind::PREPARE || p.kind == Kind::COMMIT, "STORED_NONVOTE");
        }
        for (const auto& [_, reports] : m_reports) for (const auto& [__, p] : reports) {
            VerifyPacket(p); Need(p.kind == Kind::REPORT, "STORED_NONREPORT");
        }
        for (const auto& p : m_pending) {
            VerifyPacket(p); Need(p.value.has_value() && (p.kind == Kind::PROPOSE || p.kind == Kind::PC ||
                p.kind == Kind::FAST || p.kind == Kind::SLOW || p.kind == Kind::NEW_VIEW), "STORED_PENDING_KIND");
        }
        for (const auto& p : m_byzantine_signed) {
            Need(m_byzantine && m_seat == 0 && p.sender == 0 && IsSigned(p.kind), "BYZANTINE_RECORD_IN_HONEST_STORE"); VerifyPacket(p);
        }
        Need(m_applied.has_value() == (m_apply_count == 1), "APPLICATION_COUNT_MISMATCH");
        if (m_applied) {
            Need(m_decision && m_decision->value == m_applied && m_execution.contains(*m_applied), "APPLICATION_WITHOUT_DECISION");
            auto& x{m_execution.at(*m_applied)};
            Need(Serialize(x->next_state) == m_snapshot, "APPLICATION_SNAPSHOT_MISMATCH");
            m_engine.Apply(std::move(*x)); x.reset();
        } else Need(m_snapshot == Serialize(m_engine.State()), "UNAPPLIED_SNAPSHOT_MISMATCH");
        if (m_intent) {
            ValidateGuard(*m_intent, true);
            Need(!m_signed.contains({m_intent->packet.kind, m_intent->packet.view}) &&
                 m_intent->guard.pre_view == m_view && m_intent->guard.highest == m_highest &&
                 m_intent->guard.v0 == m_v0 && m_intent->guard.decision == m_decision,
                 "INTENT_CONTEXT_MISMATCH");
        }
        // Local signatures embedded in retained remote proofs must also have
        // their exact durable signing record. A missing slot is never repaired
        // merely because its signature can be verified or recomputed.
        for (const auto& [_, p] : m_accepted) CheckLocalEvidence(p);
        for (const auto& [_, p] : m_new_views) CheckLocalEvidence(p);
        for (const auto& p : {m_highest, m_v0, m_decision}) if (p) CheckLocalEvidence(*p);
        for (const auto& [_, row] : m_votes) for (const auto& [__, p] : row) CheckLocalEvidence(p);
        for (const auto& [_, row] : m_reports) for (const auto& [__, p] : row) CheckLocalEvidence(p);
        for (const auto& p : m_pending) CheckLocalEvidence(p);
        const auto check_guard = [&](const Record& r, bool intent) {
            CheckLocalEvidence(r.packet, intent);
            for (const auto& p : {r.guard.proposal, r.guard.new_view, r.guard.prepared,
                                 r.guard.highest, r.guard.v0, r.guard.decision})
                if (p) CheckLocalEvidence(*p, intent);
        };
        for (const auto& [_, r] : m_signed) check_guard(r, false);
        if (m_intent) check_guard(*m_intent, true);
    }
    void Open()
    {
        try {
            if (m_db.IsEmpty()) {
                Need(!m_stopped_store_tool, "CORRUPTION_TOOL_REQUIRES_NONEMPTY_STORE");
                m_snapshot = Serialize(m_engine.State()); Persist(); return;
            }
            std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
            std::optional<uint256> identity; std::optional<Blob> state;
            for (it->SeekToFirst(); it->Valid(); it->Next()) {
                std::string key; Need(it->GetKeyExact(key), "STORE_KEY_DECODE");
                if (key == "identity") { uint256 v; Need(!identity && it->GetValueExact(v), "STORE_IDENTITY_DECODE"); identity = v; }
                else if (key == "state") { Blob v; Need(!state && it->GetValueExact(v), "STORE_STATE_DECODE"); state = std::move(v); }
                else throw Invalid("UNKNOWN_STORE_NAMESPACE");
            }
            Need(it->StatusOK() && identity && state && *identity == Identity(), "MISSING_OR_WRONG_STORE_IDENTITY");
            UniValue json; Need(json.read(state->text), "MALFORMED_STORE_JSON"); Load(json); Revalidate();
            if (!m_fenced && !m_stopped_store_tool) {
                if (m_intent) FinishIntent(); if (m_decision) Apply(); Retransmit();
            }
        } catch (const std::exception& e) {
            // Preserve malformed bytes for diagnosis. Never overwrite a failed
            // reopen with an apparently fresh or partially reconstructed state.
            m_fenced = true; m_storage_usable = false; m_reason = std::string{"RESTART_SAFE_REFUSAL:"} + e.what();
        }
    }
public:
    Worker(int32_t seat, const std::string& path, bool byzantine, uint64_t now,
           bool stopped_store_tool = false)
        : m_context{Instance(m_fixture), Members(m_fixture)}, m_universe{Generate(m_fixture)},
          m_base{m_fixture}, m_engine{m_fixture},
          m_db{DBParams{.path = fs::PathFromString(path), .cache_bytes = 1 << 20, .obfuscate = false}},
          m_seat{seat}, m_byzantine{byzantine}, m_stopped_store_tool{stopped_store_tool}
    {
        Need(seat >= 0 && seat < 4 && (!byzantine || seat == 0), "WORKER_SEAT_OR_BYZANTINE_SCOPE");
        Need(now <= 1000000, "STARTUP_CLOCK_BOUND");
        m_now = now; m_deadline = now + 3; m_last_retry = now; Open();
    }
    UniValue CorruptTestStore(const std::string& mode)
    {
        // Open already checked the exact generated fixture/seat identity,
        // every namespace, all signatures, and full execution/state replay.
        // This tool never resumes an intent, signs, publishes, or repairs.
        Need(m_stopped_store_tool && m_storage_usable && !m_fenced && !m_byzantine,
             "CORRUPTION_TOOL_REQUIRES_VALID_HONEST_TEST_STORE");
        if (mode == "drop-report1") {
            Need(m_signed.erase({Kind::REPORT, 1}) == 1, "CORRUPTION_TARGET_REPORT1_MISSING");
        } else if (mode == "drop-v0") {
            Need(m_v0.has_value(), "CORRUPTION_TARGET_V0_MISSING"); m_v0.reset();
        } else if (mode == "drop-commit-highest") {
            auto it{std::find_if(m_signed.begin(), m_signed.end(), [](const auto& item) {
                return item.first.first == Kind::COMMIT && item.second.guard.highest.has_value();
            })};
            Need(it != m_signed.end(), "CORRUPTION_TARGET_COMMIT_HIGHEST_MISSING");
            it->second.guard.highest.reset();
        } else if (mode == "bad-snapshot") {
            Need(!m_snapshot.empty(), "CORRUPTION_TARGET_SNAPSHOT_MISSING"); m_snapshot[0] ^= 1;
        } else throw Invalid("UNKNOWN_CORRUPTION_MODE");
        Persist();
        UniValue out{UniValue::VOBJ}; out.pushKV("corrupted", true); out.pushKV("mode", mode);
        out.pushKV("seat", m_seat); out.pushKV("instance", m_context.Instance().GetHex());
        return out;
    }
    UniValue Status() const
    {
        UniValue v{UniValue::VOBJ}; v.pushKV("seat", m_seat);
        v.pushKV("instance", m_context.Instance().GetHex()); v.pushKV("fenced", m_fenced);
        v.pushKV("reason", m_reason); v.pushKV("storage_usable", m_storage_usable);
        // A failed write or failed reopen makes every mutable field suspect.
        // Even a status proof can publish a signature whose durable write
        // failed; only immutable identifiers and the refusal survive here.
        if (!m_storage_usable) return v;
        v.pushKV("view", m_view);
        v.pushKV("changing", m_changing); v.pushKV("applied", m_applied ? UniValue{m_applied->GetHex()} : UniValue{});
        v.pushKV("apply_count", m_apply_count);
        v.pushKV("state_root", m_engine.State().Root().GetHex()); v.pushKV("signed_count", uint64_t{m_signed.size()});
        v.pushKV("body_count", uint64_t{m_bodies.size()}); v.pushKV("pending_count", uint64_t{m_pending.size()});
        v.pushKV("intent_pending", m_intent.has_value()); v.pushKV("decision", ProofText(m_decision));
        v.pushKV("highest", ProofText(m_highest)); v.pushKV("v0", ProofText(m_v0));
        return v;
    }
    UniValue Response(bool ok, const std::string& error = {}) const
    {
        UniValue r{UniValue::VOBJ}; r.pushKV("ok", ok);
        r.pushKV("events", m_storage_usable ? m_events : UniValue{UniValue::VARR});
        r.pushKV("status", Status());
        if (!error.empty()) r.pushKV("error", error); return r;
    }
    UniValue Ready()
    {
        auto r{Response(!m_fenced)}; r.pushKV("ready", true);
        r.pushKV("instance", m_context.Instance().GetHex()); return r;
    }
    UniValue Command(const UniValue& command)
    {
        m_events = UniValue{UniValue::VARR};
        try {
            const auto name{Text(command, "cmd")};
            if (name == "status" || name == "quit") return Response(true);
            Need(!m_fenced, "FENCED");
            if (name == "set-cut") {
                const auto point{Text(command, "point")};
                const std::set<std::string> allowed{"", "after_intent", "before_compute", "after_compute_before_durable",
                    "after_durable_before_publish", "after_publish", "after_decision_before_apply",
                    "during_apply_before_atomic_commit", "after_apply", "fail_signed_state_serialize",
                    "fail_signed_record_insert"};
                Need(allowed.contains(point), "UNKNOWN_CUT"); m_cut = point;
            } else if (name == "body") BodyArrived(Body::Parse(command));
            else if (name == "receive") { if (!m_byzantine) Receive(RequiredProof(command, "proof")); }
            else if (name == "tick") { const auto now{Number(command, "now")}; Need(now >= 0, "NEGATIVE_CLOCK"); Timer(now); }
            else if (name == "need") {
                const auto value{HashText(Text(command, "value"))};
                if (m_bodies.contains(value)) { UniValue e{UniValue::VOBJ}; e.pushKV("data", m_bodies.at(value).Json()); m_events.push_back(std::move(e)); }
            } else if (name == "sign-byzantine") {
                Need(m_byzantine && m_seat == 0 && m_byzantine_signed.size() < 12, "BYZANTINE_COMMAND_DISABLED_OR_BOUND");
                auto p{RequiredProof(command, "proof")}; Need(p.sender == 0 && IsSigned(p.kind), "BYZANTINE_SEAT_SCOPE");
                p = Sign(m_context, m_fixture.seat_keys[0], Unsigned(p)); VerifyPacket(p);
                if (std::find(m_byzantine_signed.begin(), m_byzantine_signed.end(), p) == m_byzantine_signed.end()) {
                    m_byzantine_signed.push_back(p); Persist();
                }
                Emit(p);
            } else throw Invalid("UNKNOWN_COMMAND");
            return Response(true);
        } catch (const dbwrapper_error& e) {
            m_fenced = true; m_storage_usable = false; m_reason = std::string{"STORAGE_SAFE_REFUSAL:"} + e.what(); return Response(false, m_reason);
        } catch (const std::exception& e) { return Response(false, e.what()); }
    }
};

void GenerateJson()
{
    fastprobe::Fixture f; Context context{Instance(f), Members(f)};
    UniValue out{UniValue::VOBJ}, bodies{UniValue::VARR}, members{UniValue::VARR};
    for (const auto& body : Generate(f)) bodies.push_back(body.Json());
    for (const auto& member : Members(f)) {
        UniValue row{UniValue::VOBJ}; row.pushKV("public_key", HexStr(member.public_key));
        row.pushKV("proof_of_possession", HexStr(member.proof_of_possession)); members.push_back(std::move(row));
    }
    out.pushKV("instance", context.Instance().GetHex()); out.pushKV("membership_hash", context.MembershipHash().GetHex());
    out.pushKV("bodies", std::move(bodies)); out.pushKV("members", std::move(members));
    std::cout << out.write() << std::endl;
}

bool ReadCommandLine(std::istream& input, std::string& line)
{
    line.clear();
    for (;;) {
        const auto next{input.get()};
        if (next == std::char_traits<char>::eof()) return !line.empty();
        if (next == '\n') return true;
        Need(line.size() < MAX_LINE, "COMMAND_LINE_BOUND");
        line.push_back(static_cast<char>(next));
    }
}
} // namespace

int main(int argc, char** argv)
{
    try {
        signal(SIGPIPE, SIG_IGN); ECC_Context ecc;
        Need(argc >= 2, "MODE_REQUIRED"); const std::string mode{argv[1]};
        if (mode == "generate") { Need(argc == 2, "GENERATE_ARGUMENTS"); GenerateJson(); return 0; }
        if (mode == "corrupt-test-store") {
            Need(argc == 5, "USAGE_corrupt-test-store_seat_dbpath_mode");
            const auto path{fs::PathFromString(argv[3])};
            Need(fs::is_directory(path) && fs::exists(path / "CURRENT"), "CORRUPTION_TOOL_REQUIRES_EXISTING_DATABASE");
            Worker worker{std::stoi(argv[2]), argv[3], false, 0, true};
            std::cout << worker.CorruptTestStore(argv[4]).write() << std::endl; return 0;
        }
        Need(mode == "worker" && argc >= 4 && argc <= 6, "USAGE_worker_seat_dbpath_--byzantine_--now=N");
        const auto seat{std::stoi(argv[2])}; bool byzantine{false}, supplied_now{false}; uint64_t now{0};
        for (int i{4}; i < argc; ++i) {
            const std::string option{argv[i]};
            if (option == "--byzantine") { Need(!byzantine, "DUPLICATE_BYZANTINE_OPTION"); byzantine = true; }
            else if (option.starts_with("--now=")) {
                const auto digits{option.substr(6)};
                Need(!supplied_now && !digits.empty() && digits.size() <= 7 &&
                    std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }),
                    "STARTUP_CLOCK_FORMAT");
                now = std::stoull(digits); supplied_now = true;
            } else throw Invalid("UNKNOWN_STARTUP_OPTION");
        }
        Worker worker{seat, argv[3], byzantine, now}; std::cout << worker.Ready().write() << std::endl;
        std::string line;
        while (ReadCommandLine(std::cin, line)) {
            UniValue command;
            Need(command.read(line) && command.isObject(), "COMMAND_JSON");
            const auto response{worker.Command(command)}; std::cout << response.write() << std::endl;
            if (command.exists("cmd") && command["cmd"].isStr() && command["cmd"].get_str() == "quit") break;
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << "P2FV_TEST_SAFE_STOP: " << e.what() << std::endl; return 1; }
}
