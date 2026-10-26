// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/sim/scenario.h>

#include <flowmesh2/admission/admission.h>
#include <flowmesh2/admission/pool.h>
#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/codes.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/engine/book.h>
#include <flowmesh2/net/client_api.h>
#include <flowmesh2/read/read_service.h>
#include <flowmesh2/sim/cluster.h>
#include <flowmesh2/sim/scheduler.h>
#include <flowmesh2/sim/sim_client.h>
#include <flowmesh2/sim/sim_node.h>
#include <flowmesh2/sim/trace.h>
#include <flowmesh2/state/replay.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/memory_journal.h>
#include <flowmesh2/storage/record.h>
#include <flowmesh2/storage/signer_log.h>
#include <flowmesh2/verify/b3verify.h>
#include <util/overflow.h>
#include <util/strencodings.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <tuple>
#include <set>
#include <utility>
#include <variant>

namespace flowmesh2::sim {

namespace {

constexpr std::array<std::string_view, 21> ALL_CHECKS{"P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8", "N1",  "N2",  "N3",
                                                      "N4", "N5", "N6", "N7", "N8", "N9", "N10", "N11", "N12", "N13"};
constexpr std::array<std::string_view, 1> SUITE_CHECKS{"P6"};
constexpr std::array<Variant, 3> ALL_VARIANTS{Variant::WALKING_SKELETON, Variant::STORAGE_FAULT, Variant::AUTHORITY_FAULT};

/** P1: the canonical body order of slots 2, 3 and 4 by outbox label. Pinned
 * from the first run of the outline (every seed gives these; the order is
 * the engine's CanonicalOrder over the parent header hash and the height,
 * O-4b, O-50). A change of any header field before slot 4 may change them.
 * Self-pinned, so a regression check here (O-87); M3-ENG-05's independent
 * checker recomputes it from RunReport::replay_batch
 * (contrib/flowmesh2/checker/fm2_check.py --skeleton).
 */
constexpr std::array<std::string_view, 3> GOLDEN_CANONICAL_ORDER{"a,b", "c,d", "e,f"};

constexpr uint64_t JITTER_MS{8};
constexpr uint64_t STEP_EVENT_LIMIT{10'000};
constexpr int MAX_COLLECT_ATTEMPTS{64};

std::string ErrorText(const Error& error) { return std::string{ToString(error.code)} + " " + std::string{error.detail}; }

std::string Quoted(std::string_view text) { return '"' + std::string{text} + '"'; }

std::string HexList(std::vector<std::vector<unsigned char>> items)
{
    std::string out{"["};
    for (const auto& item : items) out += (out.size() > 1 ? "," : "") + Quoted(HexStr(item));
    return out + "]";
}

std::string CursorJson(const FactCursor& c) { return "[" + Quoted(std::to_string(c.height)) + "," + Quoted(std::to_string(c.index)) + "]"; }

std::string AnchorJson(const Anchor& a)
{
    return "{\"hash\":" + Quoted(HexStr(a.hash)) + ",\"height\":" + Quoted(std::to_string(a.height)) + "}";
}

//! RunReport::replay_batch of the node's committed slots.
std::string ReplayBatch(const SimNode& node, const std::map<ActionId, std::string>& labels)
{
    std::string out{"{\"format\":\"fm2-trace-batch/1\""};
    out += ",\"domain\":" + Quoted(HexStr(node.Domain()));
    out += ",\"authority_epoch\":" + Quoted(std::to_string(node.GenesisAuthority().epoch));
    out += ",\"seat_set_root\":" + Quoted(HexStr(node.GenesisAuthority().seat_set_root));
    out += ",\"genesis_anchor\":" + AnchorJson(SimAnchor(SIM_BASE_HEIGHT));
    out += ",\"facts_start\":" + CursorJson(FactCursor{SIM_BASE_HEIGHT + 1, 0});
    out += ",\"markets\":[";
    for (size_t i{0}; i < node.Markets().size(); ++i) {
        const MarketDescriptor& m{node.Markets()[i]};
        out += std::string{i ? "," : ""} + "{\"base\":" + Quoted(HexStr(m.base.id)) + ",\"quote\":" + Quoted(HexStr(m.quote.id)) +
               ",\"lot\":" + Quoted(std::to_string(m.base_atoms_per_lot)) + ",\"tick\":" + Quoted(std::to_string(m.quote_atoms_per_tick)) + "}";
    }
    out += "],\"slots\":[";
    for (size_t i{0}; i < node.Sealed().size(); ++i) {
        const SealedSlot& slot{node.Sealed()[i]};
        const SlotHeaderV2& header{slot.head->Header()};
        std::vector<std::vector<unsigned char>> originals{slot.decision.body.originals};
        std::sort(originals.begin(), originals.end());
        out += std::string{i ? "," : ""} + "{\"anchor\":" + AnchorJson(header.anchor) + ",\"cert_hash\":" + Quoted(HexStr(slot.head->CertHash())) +
               ",\"facts\":" + HexList(slot.decision.body.facts) + ",\"facts_from\":" + CursorJson(header.facts_from) +
               ",\"facts_to\":" + CursorJson(header.facts_to) + ",\"originals\":" + HexList(std::move(originals)) + "}";
    }
    out += "],\"labels\":{";
    bool first{true};
    for (const auto& [id, label] : labels) {
        out += std::string{first ? "" : ","} + Quoted(HexStr(id.id)) + ":" + Quoted(label);
        first = false;
    }
    return out + "}}";
}

//! The results of the checks of one run (or of a suite).
class Checks
{
public:
    void Expect(std::string_view id, bool ok, std::string_view what)
    {
        CheckResult& result{m_results[std::string{id}]};
        if (!result.ran) {
            result.ran = true;
            result.passed = true;
        }
        if (!ok && result.passed) {
            result.passed = false;
            result.detail = std::string{what};
        }
    }
    template <class T>
    void ExpectOk(std::string_view id, const Result<T>& result, std::string_view what)
    {
        if (result.has_value()) {
            Expect(id, true, what);
        } else {
            Expect(id, false, std::string{what} + ": " + ErrorText(result.error()));
        }
    }
    const CheckMap& Results() const { return m_results; }

private:
    CheckMap m_results;
};

//! One submission of a step and what came back.
struct Submission {
    std::string client;
    std::string label;
    std::vector<unsigned char> frame;
    std::optional<net::RequestId> id;
    std::optional<admission::AdmissionReply> reply;
    std::optional<Error> error; //!< a refused Submit or an Error reply
};

//! One SealSlot the script made: the height and the result it expects ("OK" or an ErrorCode name).
struct ExpectedWindow {
    uint64_t height{0};
    std::string result;
    friend bool operator==(const ExpectedWindow&, const ExpectedWindow&) = default;
};

using Balances = std::map<AssetId, std::pair<Atoms, Atoms>>; //!< asset -> (available, reserved)

Balances BalancesOf(const AccountLeafV0& leaf)
{
    Balances out;
    for (const AssetBalance& balance : leaf.balances) out[balance.asset] = {balance.available, balance.reserved};
    return out;
}

std::pair<Atoms, Atoms> BalanceOf(const AccountLeafV0& leaf, const AssetId& asset)
{
    for (const AssetBalance& balance : leaf.balances) {
        if (balance.asset == asset) return {balance.available, balance.reserved};
    }
    return {0, 0};
}

//! The (available, reserved) deltas of `account` in `asset` in `outcome`.
std::pair<int64_t, int64_t> DeltaOf(const OutcomeV2& outcome, const AccountId& account, const AssetId& asset)
{
    for (const BalanceDelta& delta : outcome.deltas) {
        if (delta.account == account && delta.asset == asset) return {delta.available_delta, delta.reserved_delta};
    }
    return {0, 0};
}

bool Contains(const std::vector<uint64_t>& values, uint64_t value) { return std::find(values.begin(), values.end(), value) != values.end(); }

class Driver
{
public:
    Driver(uint64_t seed, Variant variant) : m_seed{seed}, m_variant{variant}, m_sched{seed} {}

    RunReport Run();

private:
    // ---- Harness ----------------------------------------------------------------
    void NoteHarness(std::string what)
    {
        if (m_harness.empty()) m_harness = std::move(what);
    }
    template <class T>
    bool Got(const Result<T>& result, std::string_view what)
    {
        if (result.has_value()) return true;
        NoteHarness(std::string{what} + ": " + ErrorText(result.error()));
        return false;
    }
    bool Fail(std::string what)
    {
        NoteHarness(std::move(what));
        return false;
    }

    // ---- Script pieces ------------------------------------------------------------
    bool Setup();
    bool ScriptBase();
    bool ScriptStorageFault();
    bool ScriptAuthorityFault();
    //! Slots 1 and 2 and the admission of a, b, c and d, shared by the fault variants.
    bool FaultPrelude();
    void CheckTrace();
    void FillReport(RunReport& report);

    SimClient& Client(const std::string& name) { return m_clients.at(name); }
    //! Record `bytes` (the outbox entry `label`) for the canonical-order labels.
    bool Remember(const Result<std::vector<unsigned char>>& bytes, const std::string& label);
    std::optional<ActionId> IdOf(const std::string& label) const;
    std::optional<ActionV2> ActionOf(const std::string& client, const std::string& label);

    //! Submit every frame at seeded jitter, run until idle, return them with their replies.
    std::optional<std::vector<Submission>> Step(std::vector<Submission> subs);
    //! One frame alone.
    std::optional<Submission> One(const std::string& client, const std::string& label, std::vector<unsigned char> frame);
    void SubmitEvent(size_t index);
    void CollectEvent(size_t index, int attempt);

    std::optional<uint64_t> HeadFor(const std::string& client);
    uint64_t Sigs() { return m_node->Admitter().Stats().sig_verifications; }
    Result<SealReport> Seal(uint64_t height, std::string expected);
    //! P2, P4 and P8 after slot `height`; false on a harness error.
    bool AfterSlot(uint64_t height);
    const SealedSlot* SealedAt(uint64_t height) const;
    //! The labels of slot `height`'s originals in body order.
    std::string OrderOf(uint64_t height) const;
    //! P1 code check of a reply.
    static bool Is(const Submission& sub, AdmissionCode code) { return sub.reply.has_value() && sub.reply->code == code; }

    const uint64_t m_seed;
    const Variant m_variant;
    Checks m_checks;
    Scheduler m_sched;
    TraceRecorder m_trace;
    std::unique_ptr<SimCluster> m_cluster;
    SimNode* m_node{nullptr};
    std::map<std::string, SimClient> m_clients;
    std::map<ActionId, std::string> m_labels;
    std::map<std::string, ActionId> m_ids;
    std::vector<DepositFact> m_facts;
    std::map<uint64_t, std::map<std::string, AccountLeafV0>> m_leaves; //!< height -> client -> verified leaf
    std::vector<ExpectedWindow> m_windows;
    std::vector<Submission> m_step;
    std::string m_harness;
};

bool Driver::Remember(const Result<std::vector<unsigned char>>& bytes, const std::string& label)
{
    if (!Got(bytes, "sign " + label)) return false;
    Result<ActionV2> action{ActionV2::Decode(*bytes, m_node->Profile())};
    if (!Got(action, "decode " + label)) return false;
    m_labels[action->Id()] = label;
    m_ids[label] = action->Id();
    return true;
}

std::optional<ActionId> Driver::IdOf(const std::string& label) const
{
    const auto it{m_ids.find(label)};
    if (it == m_ids.end()) return std::nullopt;
    return it->second;
}

std::optional<ActionV2> Driver::ActionOf(const std::string& client, const std::string& label)
{
    Result<ActionV2> action{Client(client).Action(label)};
    if (!Got(action, "outbox " + label)) return std::nullopt;
    return std::move(action).value();
}

bool Driver::Setup()
{
    SimNodeConfig config;
    Result<AccountId> alice{SimClient::AccountOf("alice")};
    if (!Got(alice, "AccountOf alice")) return false;
    switch (m_variant) {
    case Variant::WALKING_SKELETON:
        config.faults.withhold_from_proposals.insert({*alice, 5}); // w (14.2 slot 4)
        break;
    case Variant::STORAGE_FAULT:
        config.faults.fail_commit_barrier_at = 3; // N10
        break;
    case Variant::AUTHORITY_FAULT:
        config.faults.bump_authority_at = 3; // N11
        break;
    }
    Result<std::unique_ptr<SimCluster>> cluster{SimCluster::Create(1, config, m_trace)};
    if (!Got(cluster, "SimCluster::Create")) return false;
    m_cluster = std::move(cluster).value();
    m_node = &m_cluster->Node(0);
    const std::vector<MarketDescriptor> markets{m_node->Markets().begin(), m_node->Markets().end()};
    for (const std::string name : {"alice", "bob", "carol"}) {
        Result<SimClient> client{SimClient::Create(name, m_node->Profile(), m_node->Domain(), m_node->Seats(), markets)};
        if (!Got(client, "SimClient::Create " + name)) return false;
        m_clients.emplace(name, std::move(client).value());
    }
    // Block 130: D1 alice +1e9 rUSD (T1:0), D2 bob +1e10 B3 (T2:0), D3 bob +5e5 TOK (T3:0).
    const std::array<std::tuple<std::string, AssetId, Atoms, std::string>, 3> deposits{{
        {"alice", SimAsset("rUSD"), 1'000'000'000, "T1"},
        {"bob", AssetId::Native(), 10'000'000'000, "T2"},
        {"bob", SimAsset("TOK"), 500'000, "T3"},
    }};
    std::vector<Fact> facts;
    for (uint32_t i{0}; i < deposits.size(); ++i) {
        const auto& [owner, asset, amount, tx] = deposits[i];
        DepositFact fact;
        fact.cursor = FactCursor{SIM_BASE_HEIGHT + 1, i};
        fact.outpoint = Outpoint{SimLabelHash("fm2/sim/" + tx), 0};
        fact.account = Client(owner).Account();
        fact.asset = asset;
        fact.amount = amount;
        m_facts.push_back(fact);
        facts.push_back(Fact{fact});
    }
    return Got(m_node->Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 1), facts), "AppendBlock 130");
}

std::optional<std::vector<Submission>> Driver::Step(std::vector<Submission> subs)
{
    m_step = std::move(subs);
    for (size_t i{0}; i < m_step.size(); ++i) {
        const Result<uint64_t> jitter{m_sched.Jitter(JITTER_MS)};
        if (!Got(jitter, "Jitter")) return std::nullopt;
        if (!Got(m_sched.After(1 + *jitter, [this, i] { SubmitEvent(i); }), "schedule a submission")) return std::nullopt;
    }
    if (!Got(m_sched.RunUntilIdle(STEP_EVENT_LIMIT), "RunUntilIdle")) return std::nullopt;
    if (!m_harness.empty()) return std::nullopt;
    for (const Submission& sub : m_step) {
        if (!sub.reply.has_value() && !sub.error.has_value()) {
            Fail("no reply for " + sub.client + "/" + sub.label);
            return std::nullopt;
        }
    }
    std::vector<Submission> out{std::move(m_step)};
    m_step.clear();
    return out;
}

void Driver::SubmitEvent(size_t index)
{
    Submission& sub{m_step[index]};
    m_trace.Record("client.submit", sub.client + "," + sub.label + ",t=" + std::to_string(m_sched.NowMs()));
    const Result<net::RequestId> id{m_node->Api().Submit(sub.frame)};
    if (!id.has_value()) {
        sub.error = id.error();
        m_trace.Record("client.refused", sub.client + "," + sub.label + "," + std::string{ToString(id.error().code)});
        return;
    }
    sub.id = *id;
    const bool pump{Got(m_sched.After(1,
                                      [this] {
                                          const Result<net::PumpReport> pumped{m_node->Pump(1)};
                                          if (Got(pumped, "Pump")) {
                                              m_trace.Record("node.pump", "answered=" + std::to_string(pumped->answered) +
                                                                              ",t=" + std::to_string(m_sched.NowMs()));
                                          }
                                      }),
                        "schedule a pump")};
    if (pump) (void)Got(m_sched.After(2, [this, index] { CollectEvent(index, 0); }), "schedule a collect");
}

void Driver::CollectEvent(size_t index, int attempt)
{
    Submission& sub{m_step[index]};
    std::optional<net::ApiReply> reply{m_node->Api().TakeReply(*sub.id)};
    if (!reply.has_value()) {
        if (attempt >= MAX_COLLECT_ATTEMPTS) {
            NoteHarness("request of " + sub.client + "/" + sub.label + " never answered");
            return;
        }
        (void)Got(m_sched.After(1, [this, index, attempt] { CollectEvent(index, attempt + 1); }), "schedule a collect");
        return;
    }
    if (const auto* admitted{std::get_if<admission::AdmissionReply>(&reply->body)}) {
        sub.reply = *admitted;
        m_trace.Record("client.reply", sub.client + "," + sub.label + "," + std::string{ToString(admitted->code)} + "," +
                                           std::string{admitted->reason});
    } else if (const auto* error{std::get_if<Error>(&reply->body)}) {
        sub.error = *error;
        m_trace.Record("client.reply_error", sub.client + "," + sub.label + "," + std::string{ToString(error->code)});
    } else {
        NoteHarness("the reply of a submitted action is not an admission reply");
    }
}

std::optional<Submission> Driver::One(const std::string& client, const std::string& label, std::vector<unsigned char> frame)
{
    std::optional<std::vector<Submission>> done{Step({Submission{client, label, std::move(frame), {}, {}, {}}})};
    if (!done.has_value()) return std::nullopt;
    return std::move(done->front());
}

std::optional<uint64_t> Driver::HeadFor(const std::string& client)
{
    SimClient& c{Client(client)};
    const Result<read::AccountReply> reply{m_node->Api().GetState(c.Account())};
    if (!Got(reply, "GetState for " + client)) return std::nullopt;
    if (!Got(c.VerifyHead(reply->head), "verify the head for " + client)) return std::nullopt;
    return reply->head.envelope.header.height;
}

Result<SealReport> Driver::Seal(uint64_t height, std::string expected)
{
    m_windows.push_back(ExpectedWindow{height, std::move(expected)});
    return m_node->SealSlot();
}

const SealedSlot* Driver::SealedAt(uint64_t height) const
{
    for (const SealedSlot& slot : m_node->Sealed()) {
        if (slot.height == height) return &slot;
    }
    return nullptr;
}

std::string Driver::OrderOf(uint64_t height) const
{
    const SealedSlot* slot{SealedAt(height)};
    if (slot == nullptr) return "<not sealed>";
    std::string out;
    for (const std::vector<unsigned char>& original : slot->decision.body.originals) {
        const Result<ActionV2> action{ActionV2::Decode(original, m_node->Profile())};
        std::string label{"?"};
        if (action.has_value()) {
            const auto it{m_labels.find(action->Id())};
            if (it != m_labels.end()) label = it->second;
        }
        if (!out.empty()) out += ',';
        out += label;
    }
    return out;
}

bool Driver::AfterSlot(uint64_t height)
{
    // P2: the published head's certificate verifies against the trusted seat set, bitmap 0x01.
    const Result<read::HeadReply> head{m_node->Read().Head()};
    if (!Got(head, "ReadService::Head")) return false;
    m_checks.ExpectOk("P2", Client("alice").VerifyHead(head->head), "certificate of slot " + std::to_string(height));
    m_checks.Expect("P2", head->head.envelope.cert.bitmap == std::vector<unsigned char>{0x01},
                    "bitmap of slot " + std::to_string(height) + " is not 0x01");
    m_checks.Expect("P2", head->head.envelope.header.height == height, "published head is not slot " + std::to_string(height));
    const SealedSlot* sealed{SealedAt(height)};
    m_checks.Expect("P2", sealed != nullptr, "slot " + std::to_string(height) + " was not sealed");
    if (sealed != nullptr) {
        m_checks.ExpectOk("P2", verify::VerifyCertificate(sealed->head->Header(), sealed->decision.certificate, m_node->Seats()),
                          "decided certificate of slot " + std::to_string(height));
    }

    // P4: every account reply comes from one lease and verifies under its head.
    for (const std::string name : {"alice", "bob"}) {
        SimClient& client{Client(name)};
        const Result<read::AccountReply> reply{m_node->Api().GetState(client.Account())};
        if (!Got(reply, "GetState " + name)) return false;
        const Result<AccountLeafV0> leaf{client.VerifyAccount(*reply, client.Account())};
        m_checks.ExpectOk("P4", leaf, name + " leaf at slot " + std::to_string(height));
        m_checks.Expect("P4", reply->head.envelope.header.height == height, name + " reply head is not slot " + std::to_string(height));
        m_checks.Expect("P4", reply->head.commit.id == reply->head.envelope.header.Hash(), name + " reply commit is not its header");
        m_checks.Expect("P4", reply->head.label == read::ReadLabel::CERTIFIED, name + " reply is not CERTIFIED");
        if (reply->witness.has_value()) {
            m_checks.Expect("P4", reply->witness->roots.Root() == reply->head.envelope.header.state_root,
                            name + " leaf roots are not the head's state root");
        }
        if (leaf.has_value()) m_leaves[height][name] = *leaf;
    }

    // P8: an independent per-slot tally equals CheckConservation.
    const std::shared_ptr<const state::HeadView> owner_head{m_node->Owner().Head()};
    const ledger::LedgerState& ledger{owner_head->State().engine.ledger};
    m_checks.ExpectOk("P8", ledger.CheckConservation(), "CheckConservation at slot " + std::to_string(height));
    std::map<AssetId, Atoms> held;
    bool overflow{false};
    for (const auto& [account, state] : ledger.Accounts()) {
        for (const auto& [asset, balance] : state.balances) {
            const std::optional<Atoms> sum{CheckedAdd(balance.available, balance.reserved)};
            const std::optional<Atoms> total{sum.has_value() ? CheckedAdd(held[asset], *sum) : std::nullopt};
            if (!total.has_value()) {
                overflow = true;
                continue;
            }
            held[asset] = *total;
        }
    }
    std::map<AssetId, Atoms> credited; // the script's own deposits consumed up to this slot
    for (const DepositFact& fact : m_facts) {
        if (fact.cursor < owner_head->Header().facts_to) credited[fact.asset] += fact.amount;
    }
    m_checks.Expect("P8", !overflow, "tally overflow at slot " + std::to_string(height));
    for (const auto& [asset, totals] : ledger.Totals()) {
        const Atoms with_held{held[asset] + totals.pending + totals.fees_held};
        m_checks.Expect("P8", with_held == totals.deposits - totals.effects,
                        "tally is not deposits - effects at slot " + std::to_string(height));
        m_checks.Expect("P8", with_held == credited[asset], "tally is not the script's deposits at slot " + std::to_string(height));
    }
    m_checks.Expect("P8", held.size() == credited.size(), "tally names other assets than the deposits at slot " + std::to_string(height));
    if (height >= 1) {
        m_checks.Expect("P8", held[SimAsset("rUSD")] == 1'000'000'000, "rUSD is not 1,000,000,000 at slot " + std::to_string(height));
        m_checks.Expect("P8", held[AssetId::Native()] == 10'000'000'000, "B3 is not 10,000,000,000 at slot " + std::to_string(height));
        m_checks.Expect("P8", held[SimAsset("TOK")] == 500'000, "TOK is not 500,000 at slot " + std::to_string(height));
    }
    return true;
}

bool Driver::ScriptBase()
{
    const AssetId rusd{SimAsset("rUSD")};
    const MarketDescriptor m1{SimMarketM1()};
    const MarketDescriptor m2{SimMarketM2()};
    const AccountId seat_pool{SeatPoolAccount(m_node->Profile())};
    const AccountId treasury{TreasuryAccount(m_node->Profile())};
    SimClient& alice{Client("alice")};
    SimClient& bob{Client("bob")};
    SimClient& carol{Client("carol")};

    // ---- Slot 1: three deposits ------------------------------------------------
    if (!Got(Seal(1, "OK"), "SealSlot 1")) return false;
    if (!AfterSlot(1)) return false;
    if (const SealedSlot* s1{SealedAt(1)}) {
        m_checks.Expect("P5", s1->outcomes.size() == 3, "slot 1 does not have 3 outcomes");
        for (size_t i{0}; i < s1->outcomes.size() && i < m_facts.size(); ++i) {
            const OutcomeV2& outcome{s1->outcomes[i]};
            const DepositFact& fact{m_facts[i]};
            const Result<uint256> fact_id{FactId(Fact{fact})};
            m_checks.Expect("P5", outcome.source == OutcomeSource::FACT && outcome.code == OutcomeCode::DEPOSIT_CREDITED,
                            "slot 1 outcome is not DEPOSIT_CREDITED");
            m_checks.Expect("P5", fact_id.has_value() && outcome.subject == *fact_id, "slot 1 outcome is not its fact's");
            m_checks.Expect("P5", outcome.deltas == std::vector<BalanceDelta>{BalanceDelta{fact.account, fact.asset, fact.amount, 0}},
                            "slot 1 deposit deltas are not the fact's credit");
        }
    }

    // ---- Slot-2 window -----------------------------------------------------------
    std::optional<uint64_t> h{HeadFor("alice")};
    if (!h) return false;
    if (!Remember(bob.Place("a", 1, m1, Side::SELL, TimeInForce::GTC, 250, 5'000, *h), "a")) return false;
    if (!Remember(alice.Place("b", 1, m2, Side::BUY, TimeInForce::GTC, 600, 100'000, *h), "b")) return false;
    {
        // a and b are independent (two accounts): the seed permutes them.
        std::optional<std::vector<Submission>> step{Step({Submission{"bob", "a", *bob.Bytes("a"), {}, {}, {}},
                                                          Submission{"alice", "b", *alice.Bytes("b"), {}, {}, {}}})};
        if (!step) return false;
        m_checks.Expect("P1", Is((*step)[0], AdmissionCode::ADMITTED), "a is not ADMITTED");
        m_checks.Expect("P1", Is((*step)[1], AdmissionCode::ADMITTED), "b is not ADMITTED");
    }
    {
        // N8: carol has no deposit, so no account: NO_ACCOUNT before any signature check.
        if (!Remember(carol.Place("n8", 1, m1, Side::BUY, TimeInForce::GTC, 250, 1, *h), "n8")) return false;
        const uint64_t sigs{Sigs()};
        std::optional<Submission> n8{One("carol", "n8", *carol.Bytes("n8"))};
        if (!n8) return false;
        m_checks.Expect("N8", Is(*n8, AdmissionCode::NO_ACCOUNT), "carol's place is not NO_ACCOUNT");
        m_checks.Expect("N8", Sigs() == sigs, "NO_ACCOUNT checked a signature");
    }
    {
        // N12: another magic's domain: WRONG_DOMAIN before any signature check.
        const uint256 foreign{DomainHash(SimForeignNetwork(), FM_VERSION)};
        m_checks.Expect("N12", foreign != m_node->Domain(), "the foreign domain is this domain");
        if (!Remember(alice.Place("n12", 7, m1, Side::BUY, TimeInForce::GTC, 250, 1, *h, 16, foreign), "n12")) return false;
        const uint64_t sigs{Sigs()};
        std::optional<Submission> n12{One("alice", "n12", *alice.Bytes("n12"))};
        if (!n12) return false;
        m_checks.Expect("N12", Is(*n12, AdmissionCode::WRONG_DOMAIN), "the foreign-domain place is not WRONG_DOMAIN");
        m_checks.Expect("N12", Sigs() == sigs, "WRONG_DOMAIN checked a signature");
    }

    // ---- Slot 2: a and b rest ----------------------------------------------------
    if (!Got(Seal(2, "OK"), "SealSlot 2")) return false;
    if (!AfterSlot(2)) return false;
    m_checks.Expect("P1", OrderOf(2) == GOLDEN_CANONICAL_ORDER[0], "slot 2 canonical order is " + OrderOf(2));
    if (const SealedSlot* s2{SealedAt(2)}) {
        std::map<uint256, OutcomeCode> codes;
        for (const OutcomeV2& outcome : s2->outcomes) codes[outcome.subject] = outcome.code;
        m_checks.Expect("P5", codes[IdOf("a")->id] == OutcomeCode::RESTING, "a is not RESTING");
        m_checks.Expect("P5", codes[IdOf("b")->id] == OutcomeCode::RESTING, "b is not RESTING");
    }
    m_checks.Expect("P5", BalanceOf(m_leaves[2]["bob"], AssetId::Native()) == std::pair<Atoms, Atoms>{5'000'000'000, 5'000'000'000},
                    "a does not reserve 5e9 B3");
    m_checks.Expect("P5", BalanceOf(m_leaves[2]["alice"], rusd) == std::pair<Atoms, Atoms>{400'000'000, 600'000'000},
                    "b does not reserve 600,000,000 rUSD");

    // ---- Slot-3 window -----------------------------------------------------------
    h = HeadFor("alice");
    if (!h) return false;
    if (!Remember(alice.Place("c", 2, m1, Side::BUY, TimeInForce::IOC, 260, 2'000, *h), "c")) return false;
    const std::vector<unsigned char> c_bytes{*alice.Bytes("c")};
    const ActionId c_id{*IdOf("c")};
    {
        // N1: c with sig[0] ^= 1 first: BAD_SIG after exactly one signature check.
        const Result<std::vector<unsigned char>> tampered{alice.TamperedSignature("c")};
        if (!Got(tampered, "TamperedSignature")) return false;
        const uint64_t sigs{Sigs()};
        std::optional<Submission> bad{One("alice", "c_bad_sig", *tampered)};
        if (!bad) return false;
        m_checks.Expect("N1", Is(*bad, AdmissionCode::BAD_SIG), "c with a flipped signature is not BAD_SIG");
        m_checks.Expect("N1", Sigs() == sigs + 1, "BAD_SIG did not check exactly one signature");
    }
    {
        // The valid c is ADMITTED afterwards, so nonce 2 was not consumed (N1).
        const uint64_t sigs{Sigs()};
        std::optional<Submission> c{One("alice", "c", c_bytes)};
        if (!c) return false;
        m_checks.Expect("N1", Is(*c, AdmissionCode::ADMITTED), "the valid c is not ADMITTED after BAD_SIG");
        m_checks.Expect("P1", Is(*c, AdmissionCode::ADMITTED), "c is not ADMITTED");
        m_checks.Expect("N1", Sigs() == sigs + 1, "admitting c did not check its signature");
    }
    {
        // N4, pending: the exact bytes again give DUPLICATE{PendingRef}, no signature check.
        const uint64_t sigs{Sigs()};
        std::optional<Submission> again{One("alice", "c", c_bytes)};
        if (!again) return false;
        m_checks.Expect("N4", Is(*again, AdmissionCode::DUPLICATE), "the resent pending c is not DUPLICATE");
        m_checks.Expect("N4", again->reply.has_value() && again->reply->pending.has_value() && again->reply->pending->id == c_id &&
                                  !again->reply->included.has_value(),
                        "the pending DUPLICATE does not carry c's PendingRef");
        m_checks.Expect("N4", Sigs() == sigs, "the pending DUPLICATE checked a signature");
    }
    if (!Remember(alice.Place("d", 3, m2, Side::BUY, TimeInForce::GTC, 600, 100'000, *h), "d")) return false;
    {
        std::optional<Submission> d{One("alice", "d", *alice.Bytes("d"))};
        if (!d) return false;
        m_checks.Expect("P1", Is(*d, AdmissionCode::ADMITTED), "d is not ADMITTED");
    }

    // ---- Slot 3: c fills against a, d is refused, fees are distributed ------------
    if (!Got(Seal(3, "OK"), "SealSlot 3")) return false;
    if (!AfterSlot(3)) return false;
    m_checks.Expect("P1", OrderOf(3) == GOLDEN_CANONICAL_ORDER[1], "slot 3 canonical order is " + OrderOf(3));
    if (const SealedSlot* s3{SealedAt(3)}) {
        bool fees{false};
        for (const OutcomeV2& outcome : s3->outcomes) {
            if (outcome.code != OutcomeCode::FEES_DISTRIBUTED) continue;
            fees = true;
            m_checks.Expect("P5", DeltaOf(outcome, seat_pool, rusd) == std::pair<int64_t, int64_t>{40, 0}, "seat pool fee is not +40");
            m_checks.Expect("P5", DeltaOf(outcome, treasury, rusd) == std::pair<int64_t, int64_t>{10, 0}, "treasury fee is not +10");
        }
        m_checks.Expect("P5", fees, "slot 3 has no FEES_DISTRIBUTED outcome");
    }
    // P5: alice verifies c's inclusion, the fill and her balance transition.
    std::optional<OutcomeV2> c_outcome;
    std::optional<OutcomeV2> d_outcome;
    std::optional<uint32_t> c_index;
    {
        const std::optional<ActionV2> c_action{ActionOf("alice", "c")};
        const std::optional<ActionV2> d_action{ActionOf("alice", "d")};
        if (!c_action || !d_action) return false;
        const Result<net::ActionStatusReply> gc{m_node->Api().GetAction(*c_action)};
        const Result<net::ActionStatusReply> gd{m_node->Api().GetAction(*d_action)};
        if (!Got(gc, "GetAction c") || !Got(gd, "GetAction d")) return false;
        m_checks.Expect("P5", gc->status == net::ApiActionStatus::INCLUDED && gc->included.has_value(), "c is not INCLUDED");
        m_checks.Expect("N7", gd->status == net::ApiActionStatus::INCLUDED && gd->included.has_value(), "d is not INCLUDED");
        if (gc->included.has_value()) {
            m_checks.Expect("P5", gc->included->ref.height == 3, "c is not included in slot 3");
            const Result<OutcomeV2> verified{alice.VerifyIncluded(*gc->included, c_id)};
            m_checks.ExpectOk("P5", verified, "alice verifies c's certificate and outcome");
            if (verified.has_value()) {
                c_outcome = *verified;
                c_index = gc->included->ref.index;
            }
        }
        if (gd->included.has_value()) {
            const Result<OutcomeV2> verified{alice.VerifyIncluded(*gd->included, *IdOf("d"))};
            m_checks.ExpectOk("N7", verified, "alice verifies d's outcome");
            if (verified.has_value()) d_outcome = *verified;
        }
    }
    if (c_outcome.has_value()) {
        m_checks.Expect("P5", c_outcome->code == OutcomeCode::FILLED, "c is not FILLED");
        m_checks.Expect("P5", c_outcome->fills.size() == 1, "c does not have exactly one fill");
        m_checks.ExpectOk("P5", alice.VerifyFills(*c_outcome), "VerifyFill of c's fill");
        if (c_outcome->fills.size() == 1) {
            const Fill& fill{c_outcome->fills[0]};
            m_checks.Expect("P5", fill.maker == OrderId::FromPlace(*IdOf("a")) && fill.taker == OrderId::FromPlace(c_id),
                            "c's fill is not against a");
            m_checks.Expect("P5", fill.lots == 2'000 && fill.price_ticks == 250, "c's fill is not 2,000 lots at 250");
            m_checks.Expect("P5", fill.quote_atoms == 500'000, "c's quote is not 500,000");
            m_checks.Expect("P5", fill.seller_fee == 50, "Bob's seller fee is not 50");
        }
    }
    if (d_outcome.has_value()) {
        // N7: certified REJ_INSUFFICIENT_AVAILABLE, no money moves, nonce 3 consumed.
        m_checks.Expect("N7", d_outcome->code == OutcomeCode::REJ_INSUFFICIENT_AVAILABLE, "d is not REJ_INSUFFICIENT_AVAILABLE");
        m_checks.Expect("N7", d_outcome->deltas.empty() && d_outcome->fills.empty(), "d moved money");
    }
    m_checks.Expect("N7", Contains(m_leaves[3]["alice"].used_nonces, 3), "nonce 3 is not used at slot 3");
    m_checks.Expect("N7", BalanceOf(m_leaves[3]["alice"], rusd).second == 600'000'000, "d reserved rUSD");
    if (c_outcome.has_value() && d_outcome.has_value()) {
        m_checks.ExpectOk("P5", verify::VerifyBalanceTransition(m_leaves[2]["alice"], m_leaves[3]["alice"], std::vector{*c_outcome, *d_outcome}),
                          "alice's balance transition from slot 2 to slot 3");
    }
    m_checks.Expect("P5", BalanceOf(m_leaves[2]["alice"], rusd).first == 400'000'000 && BalanceOf(m_leaves[3]["alice"], rusd).first == 399'500'000,
                    "alice's rUSD is not 400,000,000 -> 399,500,000");
    m_checks.Expect("P5", BalanceOf(m_leaves[2]["alice"], AssetId::Native()).first == 0 &&
                              BalanceOf(m_leaves[3]["alice"], AssetId::Native()).first == 2'000'000'000,
                    "alice's B3 is not 0 -> 2,000,000,000");
    {
        // P5: bob verifies his maker side through OrderFills(OrderId(a)) at head 3.
        const Result<read::OrderFillsReply> fills{m_node->Read().OrderFills(OrderId::FromPlace(*IdOf("a")))};
        if (!Got(fills, "OrderFills a")) return false;
        m_checks.ExpectOk("P5", bob.VerifyHead(fills->head), "bob verifies the OrderFills head");
        m_checks.Expect("P5", fills->head.envelope.header.height == 3, "OrderFills head is not slot 3");
        m_checks.Expect("P5", fills->status == read::LookupStatus::FOUND && fills->groups.size() == 1, "a has not one fill group");
        if (fills->groups.size() == 1) {
            const read::OrderFillGroup& group{fills->groups[0]};
            m_checks.ExpectOk("P5", verify::VerifyOutcome(fills->head.envelope.header, group.outcome.outcome, group.outcome.proof),
                              "bob verifies the maker outcome");
            m_checks.ExpectOk("P5", bob.VerifyFills(group.outcome.outcome), "bob verifies the fill");
            m_checks.Expect("P5", group.outcome.outcome.subject == c_id.id, "the maker fill is not in c's outcome");
            m_checks.Expect("P5", group.fills.size() == 1 && group.fills[0].role == read::FillRole::MAKER, "a is not the maker");
            m_checks.Expect("P5", DeltaOf(group.outcome.outcome, bob.Account(), rusd) == std::pair<int64_t, int64_t>{499'950, 0},
                            "bob's rUSD delta is not +499,950");
            m_checks.Expect("P5",
                            DeltaOf(group.outcome.outcome, bob.Account(), AssetId::Native()) == std::pair<int64_t, int64_t>{0, -2'000'000'000},
                            "bob's B3 reserved delta is not -2,000,000,000");
            m_checks.ExpectOk("P5",
                              verify::VerifyBalanceTransition(m_leaves[2]["bob"], m_leaves[3]["bob"], std::vector{group.outcome.outcome}),
                              "bob's balance transition from slot 2 to slot 3");
        }
    }
    {
        // N4, included: the exact bytes give DUPLICATE{IncludedRef{3, i}} with the certified outcome, no signature check.
        const uint64_t sigs{Sigs()};
        std::optional<Submission> again{One("alice", "c", c_bytes)};
        if (!again) return false;
        m_checks.Expect("N4", Is(*again, AdmissionCode::DUPLICATE), "the resent included c is not DUPLICATE");
        const bool ref_ok{again->reply.has_value() && again->reply->included.has_value() && again->reply->included->height == 3 &&
                          c_index.has_value() && again->reply->included->index == *c_index};
        m_checks.Expect("N4", ref_ok, "the included DUPLICATE is not IncludedRef{3, i}");
        const bool has_outcome{again->reply.has_value() && again->reply->outcome.has_value()};
        m_checks.Expect("N4", has_outcome, "the included DUPLICATE has no outcome");
        if (has_outcome) {
            m_checks.Expect("N4", c_outcome.has_value() && again->reply->outcome->outcome.outcome == *c_outcome,
                            "the DUPLICATE outcome is not the certified one");
            m_checks.ExpectOk("N4", alice.VerifyIncluded(*again->reply->outcome, c_id), "the DUPLICATE outcome verifies under header 3");
            m_checks.Expect("N4", again->reply->outcome->envelope.header.height == 3, "the DUPLICATE outcome is not under header 3");
        }
        m_checks.Expect("N4", Sigs() == sigs, "the included DUPLICATE checked a signature");
    }
    {
        // N5: another action with the used nonce 2: NONCE_USED after its signature is verified.
        if (!Remember(alice.Place("n5", 2, m1, Side::BUY, TimeInForce::GTC, 100, 1, 3), "n5")) return false;
        const uint64_t sigs{Sigs()};
        std::optional<Submission> n5{One("alice", "n5", *alice.Bytes("n5"))};
        if (!n5) return false;
        m_checks.Expect("N5", Is(*n5, AdmissionCode::NONCE_USED), "the second nonce-2 action is not NONCE_USED");
        m_checks.Expect("N5", Sigs() == sigs + 1, "NONCE_USED did not come after one signature check");
    }

    // ---- Slot-4 window -----------------------------------------------------------
    h = HeadFor("alice");
    if (!h) return false;
    if (!Remember(bob.Transfer("e", 2, alice.Account(), rusd, 100'000, *h), "e")) return false;
    if (!Remember(alice.Cancel("f", 4, OrderId::FromPlace(*IdOf("b")), *h), "f")) return false;
    {
        std::optional<std::vector<Submission>> step{Step({Submission{"bob", "e", *bob.Bytes("e"), {}, {}, {}},
                                                          Submission{"alice", "f", *alice.Bytes("f"), {}, {}, {}}})};
        if (!step) return false;
        m_checks.Expect("P1", Is((*step)[0], AdmissionCode::ADMITTED), "e is not ADMITTED");
        m_checks.Expect("P1", Is((*step)[1], AdmissionCode::ADMITTED), "f is not ADMITTED");
    }
    {
        // N6: after e's reply, e' (TRANSFER 1 atom, n=2) is NONCE_PENDING after its signature check.
        if (!Remember(bob.Transfer("e_prime", 2, alice.Account(), rusd, 1, *h), "e_prime")) return false;
        const uint64_t sigs{Sigs()};
        std::optional<Submission> e_prime{One("bob", "e_prime", *bob.Bytes("e_prime"))};
        if (!e_prime) return false;
        m_checks.Expect("N6", Is(*e_prime, AdmissionCode::NONCE_PENDING), "e' is not NONCE_PENDING");
        m_checks.Expect("N6", Sigs() == sigs + 1, "NONCE_PENDING did not come after one signature check");
    }
    {
        // w: ref_slot 3, max_delay 1, admitted, then withheld by the fault plan.
        if (!Remember(alice.Place("w", 5, m1, Side::BUY, TimeInForce::GTC, 1, 1, 3, 1), "w")) return false;
        std::optional<Submission> w{One("alice", "w", *alice.Bytes("w"))};
        if (!w) return false;
        m_checks.Expect("P1", Is(*w, AdmissionCode::ADMITTED), "w is not ADMITTED");
    }
    {
        // N9: WITHDRAW (n=6) is a typed refusal, before any signature check.
        if (!Remember(alice.Withdraw("n9", 6, rusd, 1, {0x00, 0x14, 0x01, 0x02}, *h), "n9")) return false;
        const uint64_t sigs{Sigs()};
        std::optional<Submission> n9{One("alice", "n9", *alice.Bytes("n9"))};
        if (!n9) return false;
        // TODO(rebirth:M2-LED-05) WITHDRAW is refused at admission until the withdrawal path exists.
        m_checks.Expect("N9", Is(*n9, AdmissionCode::UNSUPPORTED_KIND), "WITHDRAW is not UNSUPPORTED_KIND");
        m_checks.Expect("N9", n9->reply.has_value() && n9->reply->reason == "M2-LED-05", "the refusal does not name M2-LED-05");
        m_checks.Expect("N9", Sigs() == sigs, "the typed refusal checked a signature");
    }

    // ---- Slot 4: e and f; w is withheld --------------------------------------------
    if (!Got(Seal(4, "OK"), "SealSlot 4")) return false;
    if (!AfterSlot(4)) return false;
    m_checks.Expect("P1", OrderOf(4) == GOLDEN_CANONICAL_ORDER[2], "slot 4 canonical order is " + OrderOf(4));
    if (const SealedSlot* s4{SealedAt(4)}) {
        std::map<uint256, OutcomeCode> codes;
        for (const OutcomeV2& outcome : s4->outcomes) codes[outcome.subject] = outcome.code;
        m_checks.Expect("P5", codes[IdOf("e")->id] == OutcomeCode::TRANSFERRED, "e is not TRANSFERRED");
        m_checks.Expect("P5", codes[IdOf("f")->id] == OutcomeCode::CANCELLED, "f is not CANCELLED");
        m_checks.Expect("N6", codes.contains(IdOf("e")->id) && !codes.contains(IdOf("e_prime")->id), "slot 4 does not include exactly e");
        m_checks.Expect("N6", s4->decision.body.originals.size() == 2, "slot 4 does not hold exactly e and f");
        m_checks.Expect("N4", !codes.contains(c_id.id), "c was included again in slot 4");
        m_checks.Expect("P5", !codes.contains(IdOf("w")->id), "w was not withheld from slot 4");
    }
    m_checks.Expect("P5", BalanceOf(m_leaves[4]["alice"], rusd) == std::pair<Atoms, Atoms>{999'600'000, 0},
                    "f did not release 6e8 or e did not credit alice");
    m_checks.Expect("N4", BalanceOf(m_leaves[4]["alice"], AssetId::Native()) == BalanceOf(m_leaves[3]["alice"], AssetId::Native()),
                    "alice's B3 changed after the resend (a second fill)");
    if (const std::optional<admission::PoolEntryStatus> c_status{m_node->Pool().Status(c_id)}) {
        m_checks.Expect("N4", c_status->status == admission::PoolStatus::INCLUDED, "c is not INCLUDED in the pool");
    } else {
        m_checks.Expect("N4", false, "the pool does not know c");
    }
    if (const std::optional<admission::PoolEntryStatus> w_status{m_node->Pool().Status(*IdOf("w"))}) {
        m_checks.Expect("P5", w_status->status == admission::PoolStatus::PENDING, "w is not PENDING after slot 4");
    } else {
        m_checks.Expect("P5", false, "the pool does not know w after slot 4");
    }
    {
        const std::optional<ActionV2> e_prime{ActionOf("bob", "e_prime")};
        if (!e_prime) return false;
        const Result<net::ActionStatusReply> got{m_node->Api().GetAction(*e_prime)};
        if (!Got(got, "GetAction e'")) return false;
        m_checks.Expect("N6", got->status != net::ApiActionStatus::INCLUDED, "e' is INCLUDED");
    }
    {
        // N13: slot 3's decision and candidate again: STALE_PARENT, head and state unchanged.
        const std::shared_ptr<const state::HeadView> before{m_node->Owner().Head()};
        const Result<std::shared_ptr<const state::HeadView>> again{m_node->ReapplySealed(3)};
        m_checks.Expect("N13", !again.has_value() && again.error().code == ErrorCode::STALE_PARENT, "re-applying slot 3 is not STALE_PARENT");
        const std::shared_ptr<const state::HeadView> after{m_node->Owner().Head()};
        m_checks.Expect("N13", after.get() == before.get() && after->HeaderHash() == before->HeaderHash() &&
                                   after->State() == before->State(),
                        "re-applying slot 3 changed the head");
        m_checks.Expect("N13", m_node->Read().PublishedHeight() == std::optional<uint64_t>{4}, "the read head moved");
    }
    {
        // N2: bob's PLACE n=3, ref_slot 1, max_delay 2 is EXPIRED with no signature check.
        if (!Remember(bob.Place("n2", 3, m1, Side::SELL, TimeInForce::GTC, 300, 1, 1, 2), "n2")) return false;
        const uint64_t sigs{Sigs()};
        std::optional<Submission> n2{One("bob", "n2", *bob.Bytes("n2"))};
        if (!n2) return false;
        m_checks.Expect("N2", Is(*n2, AdmissionCode::EXPIRED), "bob's late place is not EXPIRED");
        m_checks.Expect("N2", Sigs() == sigs, "EXPIRED checked a signature");
    }

    // ---- Slot 5: heartbeat; w expires ---------------------------------------------
    if (!Got(Seal(5, "OK"), "SealSlot 5")) return false;
    if (!AfterSlot(5)) return false;
    if (const SealedSlot* s5{SealedAt(5)}) {
        m_checks.Expect("P5", s5->decision.body.originals.empty() && s5->outcomes.empty(), "slot 5 is not a heartbeat");
    }
    if (const std::optional<admission::PoolEntryStatus> w_status{m_node->Pool().Status(*IdOf("w"))}) {
        m_checks.Expect("N3", w_status->status == admission::PoolStatus::EXPIRED, "the pool did not give w a terminal EXPIRED");
    } else {
        m_checks.Expect("N3", false, "the pool does not know w after slot 5");
    }
    {
        // N3: GetAction(w) = EXPIRED with a proof from one lease, and VerifyExpired accepts it.
        const std::optional<ActionV2> w{ActionOf("alice", "w")};
        if (!w) return false;
        const Result<net::ActionStatusReply> got{m_node->Api().GetAction(*w)};
        if (!Got(got, "GetAction w")) return false;
        m_checks.Expect("N3", got->status == net::ApiActionStatus::EXPIRED, "w is not EXPIRED");
        m_checks.ExpectOk("N3", alice.VerifyExpiredReply("w", *got), "VerifyExpired(w, header 5, cert 5, leaf 5, proof)");
        if (got->head.has_value() && got->expiry.has_value()) {
            const std::vector<uint64_t>& used{got->expiry->leaf.used_nonces};
            m_checks.Expect("N3", got->head->envelope.header.height == 5 && 5 > w->ref_slot + w->max_delay, "the proof is not at 5 > 3 + 1");
            m_checks.Expect("N3", !Contains(used, 5) && used.size() == 4 && used.size() < m_node->Profile().nonce_window,
                            "nonce 5 is used or the window is full");
        }
    }

    // ---- Final state (14.3): R1 client-verified leaves, R2 independent recomputation --
    const std::map<AccountId, Balances> expected{
        {alice.Account(), Balances{{rusd, {999'600'000, 0}}, {AssetId::Native(), {2'000'000'000, 0}}}},
        {bob.Account(), Balances{{AssetId::Native(), {5'000'000'000, 3'000'000'000}}, {rusd, {399'950, 0}}, {SimAsset("TOK"), {500'000, 0}}}},
        {seat_pool, Balances{{rusd, {40, 0}}}},
        {treasury, Balances{{rusd, {10, 0}}}},
    };
    std::map<AccountId, AccountLeafV0> r1;
    for (const auto& [account, balances] : expected) {
        const Result<read::AccountReply> reply{m_node->Api().GetState(account)};
        if (!Got(reply, "GetState final")) return false;
        const Result<AccountLeafV0> leaf{alice.VerifyAccount(*reply, account)};
        m_checks.ExpectOk("P5", leaf, "client-verified final leaf");
        if (!leaf.has_value()) continue;
        r1[account] = *leaf;
        m_checks.Expect("P5", BalancesOf(*leaf) == balances, "a final client-verified balance is not section 14.3's");
    }
    m_checks.Expect("P5", r1[alice.Account()].used_nonces == std::vector<uint64_t>{1, 2, 3, 4}, "alice's used nonces are not {1,2,3,4}");
    m_checks.Expect("P5", r1[bob.Account()].used_nonces == std::vector<uint64_t>{1, 2}, "bob's used nonces are not {1,2}");
    m_checks.Expect("N2", r1[bob.Account()].used_nonces == std::vector<uint64_t>{1, 2}, "bob's nonce 3 was consumed");
    m_checks.Expect("N9", !Contains(r1[alice.Account()].used_nonces, 6), "alice's nonce 6 was consumed");
    // R2: the deposits of the script plus every certified outcome delta of slots 1-5.
    std::map<std::pair<AccountId, AssetId>, std::pair<Atoms, Atoms>> tally;
    for (const SealedSlot& slot : m_node->Sealed()) {
        for (const OutcomeV2& outcome : slot.outcomes) {
            for (const BalanceDelta& delta : outcome.deltas) {
                std::pair<Atoms, Atoms>& entry{tally[{delta.account, delta.asset}]};
                entry.first += delta.available_delta;
                entry.second += delta.reserved_delta;
            }
        }
    }
    Atoms deposited{0};
    for (const DepositFact& fact : m_facts) deposited += fact.amount;
    Atoms credited{0};
    if (const SealedSlot* s1{SealedAt(1)}) {
        for (const OutcomeV2& outcome : s1->outcomes) {
            for (const BalanceDelta& delta : outcome.deltas) credited += delta.available_delta;
        }
    }
    m_checks.Expect("P5", credited == deposited, "the deposit outcomes do not credit the facts");
    std::map<AccountId, Balances> r2;
    for (const auto& [key, value] : tally) {
        if (value.first != 0 || value.second != 0) r2[key.first][key.second] = value;
    }
    m_checks.Expect("P5", r2 == expected, "the independent recomputation is not section 14.3's");
    for (const auto& [account, leaf] : r1) {
        m_checks.Expect("P5", BalancesOf(leaf) == r2[account], "R1 differs from R2");
    }
    {
        // Books: M1 asks hold OrderId(a), 3,000 lots at 250; M2 is empty.
        const std::shared_ptr<const state::HeadView> head{m_node->Owner().Head()};
        const engine::OrderBook* book1{head->State().engine.books.Find(m1.Id())};
        const engine::OrderBook* book2{head->State().engine.books.Find(m2.Id())};
        m_checks.Expect("P5", book1 != nullptr && book2 != nullptr, "a market has no book");
        if (book1 != nullptr && book2 != nullptr) {
            const std::vector<engine::RestingOrder> orders{book1->Orders()};
            m_checks.Expect("P5", orders.size() == 1 && orders[0].id == OrderId::FromPlace(*IdOf("a")) && orders[0].side == Side::SELL &&
                                      orders[0].price_ticks == 250 && orders[0].remaining_lots == 3'000,
                            "M1 asks are not a: 3,000 lots at 250");
            m_checks.Expect("P5", book2->Orders().empty(), "M2 is not empty");
        }
    }

    // ---- P7: replay of the commit journal ---------------------------------------------
    {
        const Result<std::vector<storage::JournalRecord>> records{m_node->CommitJournal().ReadAll()};
        if (!Got(records, "commit journal ReadAll")) return false;
        const auto check_replay = [&](const Result<state::ReplayResult>& replay, uint64_t duplicates, std::string_view what) {
            m_checks.ExpectOk("P7", replay, what);
            if (!replay.has_value()) return;
            m_checks.Expect("P7", replay->slots.size() == m_node->Sealed().size(), std::string{what} + ": slot count");
            for (size_t i{0}; i < replay->slots.size() && i < m_node->Sealed().size(); ++i) {
                const SealedSlot& sealed{m_node->Sealed()[i]};
                m_checks.Expect("P7", replay->slots[i].header == sealed.head->Header() && replay->slots[i].cert_hash == sealed.head->CertHash(),
                                std::string{what} + ": header " + std::to_string(i + 1));
            }
            m_checks.Expect("P7", replay->head.HeaderHash() == m_node->Owner().Head()->HeaderHash() &&
                                      replay->head.Header().state_root == m_node->Owner().Head()->Header().state_root,
                            std::string{what} + ": final head and root");
            m_checks.Expect("P7", replay->duplicate_decisions == duplicates, std::string{what} + ": duplicate count");
        };
        check_replay(state::ReplayDecisions(m_node->Genesis(), *records), 0, "replay of the commit journal");
        // The same journal with slot 3's DECISION written twice: slot 3 is applied once.
        Result<std::unique_ptr<storage::MemoryJournal>> copy{storage::MemoryJournal::Create(storage::JournalRole::COMMIT, 99)};
        if (!Got(copy, "MemoryJournal copy")) return false;
        std::optional<uint64_t> first_seq;
        for (const storage::JournalRecord& record : *records) {
            if (!Got((*copy)->Append(record.type, record.payload), "copy a record")) return false;
            if (record.type != storage::RecordType::DECISION) continue;
            const Result<storage::DecisionRecord> decision{storage::DecisionRecord::Decode(record.payload, m_node->Profile())};
            if (!Got(decision, "decode a DECISION")) return false;
            if (decision->header.height != 3) continue;
            first_seq = (*copy)->Status().appended_seq;
            if (!Got((*copy)->Append(record.type, record.payload), "duplicate slot 3's DECISION")) return false;
        }
        if (!Got((*copy)->Barrier(), "copy barrier")) return false;
        const Result<std::vector<storage::JournalRecord>> doubled{(*copy)->ReadAll()};
        if (!Got(doubled, "copy ReadAll")) return false;
        m_checks.Expect("P7", doubled->size() == records->size() + 1, "the copy does not hold one more record");
        const Result<state::ReplayResult> replay{state::ReplayDecisions(m_node->Genesis(), *doubled)};
        check_replay(replay, 1, "replay with slot 3's DECISION twice");
        if (replay.has_value() && replay->slots.size() >= 3) {
            m_checks.Expect("P7", first_seq.has_value() && replay->slots[2].decision_seq == *first_seq,
                            "slot 3 was not applied from its first DECISION");
        }
    }
    return true;
}

bool Driver::FaultPrelude()
{
    const MarketDescriptor m1{SimMarketM1()};
    const MarketDescriptor m2{SimMarketM2()};
    SimClient& alice{Client("alice")};
    SimClient& bob{Client("bob")};
    if (!Got(Seal(1, "OK"), "SealSlot 1")) return false;
    if (!AfterSlot(1)) return false;
    std::optional<uint64_t> h{HeadFor("alice")};
    if (!h) return false;
    if (!Remember(bob.Place("a", 1, m1, Side::SELL, TimeInForce::GTC, 250, 5'000, *h), "a")) return false;
    if (!Remember(alice.Place("b", 1, m2, Side::BUY, TimeInForce::GTC, 600, 100'000, *h), "b")) return false;
    std::optional<std::vector<Submission>> step{
        Step({Submission{"bob", "a", *bob.Bytes("a"), {}, {}, {}}, Submission{"alice", "b", *alice.Bytes("b"), {}, {}, {}}})};
    if (!step) return false;
    if (!Is((*step)[0], AdmissionCode::ADMITTED) || !Is((*step)[1], AdmissionCode::ADMITTED)) return Fail("a or b not admitted");
    if (!Got(Seal(2, "OK"), "SealSlot 2")) return false;
    if (!AfterSlot(2)) return false;
    h = HeadFor("alice");
    if (!h) return false;
    if (!Remember(alice.Place("c", 2, m1, Side::BUY, TimeInForce::IOC, 260, 2'000, *h), "c")) return false;
    if (!Remember(alice.Place("d", 3, m2, Side::BUY, TimeInForce::GTC, 600, 100'000, *h), "d")) return false;
    for (const std::string label : {"c", "d"}) {
        std::optional<Submission> sub{One("alice", label, *alice.Bytes(label))};
        if (!sub) return false;
        if (!Is(*sub, AdmissionCode::ADMITTED)) return Fail(label + " not admitted");
    }
    return Remember(bob.Transfer("e", 2, alice.Account(), SimAsset("rUSD"), 100'000, *h), "e");
}

bool Driver::ScriptStorageFault()
{
    if (!FaultPrelude()) return false;
    // N10: B-DECISION of slot 3 fails.
    const Result<SealReport> sealed{Seal(3, "STORAGE_FATAL")};
    m_checks.Expect("N10", !sealed.has_value() && sealed.error().code == ErrorCode::STORAGE_FATAL, "SealSlot 3 is not STORAGE_FATAL");
    m_checks.Expect("N10", m_node->CommitJournal().IsFatal(), "the commit journal is not fatal (sticky)");
    {
        // The failed barrier came right after slot 3's DECISION append.
        std::vector<std::string> window;
        for (const TraceEvent& event : m_trace.Events()) {
            if (event.slot == 3) window.push_back(event.Step());
        }
        const std::vector<std::string> tail{"commit.append:DECISION", "commit.barrier.start", "commit.barrier.end:fail", "node.fence",
                                            "slot.end"};
        m_checks.Expect("N10", window.size() >= tail.size() && std::equal(tail.begin(), tail.end(), window.end() - tail.size()),
                        "the failed barrier is not the one after slot 3's DECISION");
    }
    const Result<std::vector<storage::JournalRecord>> records{m_node->CommitJournal().ReadAll()};
    if (!Got(records, "commit journal ReadAll")) return false;
    bool slot3{false};
    for (const storage::JournalRecord& record : *records) {
        if (record.type == storage::RecordType::APPLIED) {
            const Result<storage::AppliedRecord> applied{storage::AppliedRecord::Decode(record.payload)};
            slot3 = slot3 || !applied.has_value() || applied->slot == 3;
        }
        if (record.type == storage::RecordType::DECISION) {
            const Result<storage::DecisionRecord> decision{storage::DecisionRecord::Decode(record.payload, m_node->Profile())};
            slot3 = slot3 || !decision.has_value() || decision->header.height == 3;
        }
    }
    m_checks.Expect("N10", !slot3, "a durable DECISION or APPLIED of slot 3 exists");
    m_checks.Expect("N10", m_node->Read().PublishedHeight() == std::optional<uint64_t>{2}, "the read head is not 2");
    m_checks.Expect("N10", m_node->Owner().Head()->Header().height == 2, "the state owner applied slot 3");
    {
        const std::optional<ActionV2> c{ActionOf("alice", "c")};
        if (!c) return false;
        const Result<net::ActionStatusReply> got{m_node->Api().GetAction(*c)};
        m_checks.Expect("N10", got.has_value() && got->status == net::ApiActionStatus::PENDING, "GetAction(c) is not PENDING");
        m_checks.Expect("N10", got.has_value() && got->head.has_value() && got->head->envelope.header.height == 2,
                        "GetAction(c) does not read head 2");
    }
    m_checks.Expect("N10", m_node->Fenced(), "admission is not fenced");
    {
        std::optional<Submission> e{One("bob", "e", *Client("bob").Bytes("e"))};
        if (!e) return false;
        m_checks.Expect("N10", Is(*e, AdmissionCode::HALTED), "a new submit is not HALTED");
    }
    {
        // The next SealSlot is FENCED and writes no new ACCEPTED_PROPOSAL or INTENT.
        const storage::JournalStatus before{m_node->SignerJournal().Status()};
        const Result<SealReport> again{Seal(3, "FENCED")};
        m_checks.Expect("N10", !again.has_value() && again.error().code == ErrorCode::FENCED,
                        "the next SealSlot is not FENCED: " + (again.has_value() ? std::string{"Ok"} : ErrorText(again.error())));
        m_checks.Expect("N10", m_node->SignerJournal().Status() == before, "the fenced SealSlot wrote a signer record");
    }
    return true;
}

bool Driver::ScriptAuthorityFault()
{
    if (!FaultPrelude()) return false;
    // N11: the authority is bumped between Preview and Propose of slot 3.
    const storage::JournalStatus signer_before{m_node->SignerJournal().Status()};
    const storage::JournalStatus commit_before{m_node->CommitJournal().Status()};
    const Result<SealReport> sealed{Seal(3, "AUTHORITY_CHANGED")};
    m_checks.Expect("N11", !sealed.has_value() && sealed.error().code == ErrorCode::AUTHORITY_CHANGED, "SealSlot 3 is not AUTHORITY_CHANGED");
    m_checks.Expect("N11", m_node->SignerJournal().Status() == signer_before, "a signer record was written");
    m_checks.Expect("N11", m_node->CommitJournal().Status() == commit_before, "a commit record was written");
    m_checks.Expect("N11", m_node->Chain().CurrentAuthority().epoch == 1, "the authority was not bumped");
    m_checks.Expect("N11", m_node->Owner().Head()->Header().height == 2 && m_node->Read().PublishedHeight() == std::optional<uint64_t>{2},
                    "the head moved");
    m_checks.Expect("N11", m_node->Fenced(), "admission is not fenced");
    {
        std::vector<std::string> window;
        for (const TraceEvent& event : m_trace.Events()) {
            if (event.slot == 3) window.push_back(event.Step());
        }
        const std::vector<std::string> tail{"state.preview", "fault.bump_authority", "authority.check", "node.fence", "slot.end"};
        m_checks.Expect("N11", window.size() >= tail.size() && std::equal(tail.begin(), tail.end(), window.end() - tail.size()),
                        "the bump is not between Preview and the refused check 1");
    }
    std::optional<Submission> e{One("bob", "e", *Client("bob").Bytes("e"))};
    if (!e) return false;
    m_checks.Expect("N11", Is(*e, AdmissionCode::HALTED), "a new submit is not HALTED");
    {
        // The fenced node seals nothing: the next SealSlot is FENCED and writes no record (O-89).
        const Result<SealReport> again{Seal(3, "FENCED")};
        m_checks.Expect("N11", !again.has_value() && again.error().code == ErrorCode::FENCED,
                        "the next SealSlot is not FENCED: " + (again.has_value() ? std::string{"Ok"} : ErrorText(again.error())));
        m_checks.Expect("N11", m_node->SignerJournal().Status() == signer_before, "the fenced SealSlot wrote a signer record");
        m_checks.Expect("N11", m_node->CommitJournal().Status() == commit_before, "the fenced SealSlot wrote a commit record");
    }
    return true;
}

//! The P3 order of a slot with `result` (ARCHITECTURE.md 14.4 P3, plus the poll, take and preview before it).
std::vector<std::string> ExpectedSteps(std::string_view result)
{
    // FENCED: a fenced node seals nothing, so the window is empty (SimNode step 0, O-89).
    if (result == "FENCED") return {"slot.begin", "slot.end"};
    const std::vector<std::string> head{"slot.begin", "chain.poll", "pool.take", "state.preview"};
    std::vector<std::string> steps{head};
    if (result == "OK" || result == "STORAGE_FATAL") {
        for (const char* step : {"authority.check", "signer.append:ACCEPTED_PROPOSAL", "signer.append:INTENT", "signer.barrier.start",
                                 "signer.barrier.end:ok", "authority.check", "bls.sign", "signer.append:SIGNED_VOTE",
                                 "signer.barrier.start", "signer.barrier.end:ok", "commit.append:DECISION", "commit.barrier.start"}) {
            steps.emplace_back(step);
        }
    }
    if (result == "OK") {
        for (const char* step : {"commit.barrier.end:ok", "authority.check", "state.apply", "commit.append:APPLIED",
                                 "commit.barrier.start", "commit.barrier.end:ok", "read.publish", "pool.on_committed"}) {
            steps.emplace_back(step);
        }
    } else if (result == "STORAGE_FATAL") {
        steps.emplace_back("commit.barrier.end:fail");
        steps.emplace_back("node.fence");
    } else if (result == "AUTHORITY_CHANGED") {
        steps.emplace_back("fault.bump_authority");
        steps.emplace_back("authority.check");
        steps.emplace_back("node.fence");
    }
    steps.emplace_back("slot.end");
    return steps;
}

void Driver::CheckTrace()
{
    // P3: every SealSlot window has exactly the barrier-map order, and each
    // append's durable record (or its absence after a failed barrier) belongs to the window's slot.
    std::map<storage::JournalRole, std::map<uint64_t, storage::JournalRecord>> durable;
    for (const auto& [role, journal] : {std::pair{storage::JournalRole::SIGNER, &m_node->SignerJournal()},
                                        std::pair{storage::JournalRole::COMMIT, &m_node->CommitJournal()}}) {
        const Result<std::vector<storage::JournalRecord>> records{journal->ReadAll()};
        m_checks.ExpectOk("P3", records, "ReadAll of a journal");
        if (!records.has_value()) return;
        for (const storage::JournalRecord& record : *records) durable[role][record.seq] = record;
    }
    const auto slot_of = [&](const storage::JournalRecord& record) -> std::optional<uint64_t> {
        switch (record.type) {
        case storage::RecordType::ACCEPTED_PROPOSAL: {
            const Result<storage::AcceptedProposalRecord> r{storage::AcceptedProposalRecord::Decode(record.payload, m_node->Profile())};
            if (r.has_value() && r->key.slot == r->header.height) return r->key.slot;
            return std::nullopt;
        }
        case storage::RecordType::INTENT: {
            const Result<storage::IntentRecord> r{storage::IntentRecord::Decode(record.payload)};
            return r.has_value() ? std::optional<uint64_t>{r->key.slot} : std::nullopt;
        }
        case storage::RecordType::SIGNED_VOTE: {
            const Result<storage::SignedVoteRecord> r{storage::SignedVoteRecord::Decode(record.payload)};
            return r.has_value() ? std::optional<uint64_t>{r->key.slot} : std::nullopt;
        }
        case storage::RecordType::DECISION: {
            const Result<storage::DecisionRecord> r{storage::DecisionRecord::Decode(record.payload, m_node->Profile())};
            return r.has_value() ? std::optional<uint64_t>{r->header.height} : std::nullopt;
        }
        case storage::RecordType::APPLIED: {
            const Result<storage::AppliedRecord> r{storage::AppliedRecord::Decode(record.payload)};
            return r.has_value() ? std::optional<uint64_t>{r->slot} : std::nullopt;
        }
        default:
            return std::nullopt;
        }
    };

    std::vector<ExpectedWindow> seen;
    std::vector<std::string> steps;
    std::vector<TraceEvent> window_events;
    bool in_window{false};
    size_t publications{0};
    for (const TraceEvent& event : m_trace.Events()) {
        if (event.kind == "slot.begin") {
            m_checks.Expect("P3", !in_window, "a slot window began inside another");
            in_window = true;
            steps.clear();
            window_events.clear();
        }
        if (!in_window) {
            // Outside SealSlot no journal, signing or publication event may occur.
            const bool sealing{event.kind.find(".append") != std::string::npos || event.kind.find(".barrier") != std::string::npos ||
                               event.kind == "bls.sign" || event.kind == "read.publish" || event.kind == "state.apply"};
            m_checks.Expect("P3", !sealing, "sealing event " + event.Step() + " outside a slot window");
            continue;
        }
        steps.push_back(event.Step());
        window_events.push_back(event);
        if (event.kind != "slot.end") continue;
        in_window = false;
        const std::string result{event.detail.substr(event.detail.find(',') + 1)};
        seen.push_back(ExpectedWindow{event.slot, result});
        const std::vector<std::string> expected{ExpectedSteps(result)};
        m_checks.Expect("P3", steps == expected, "slot " + std::to_string(event.slot) + " (" + result + ") steps differ from the barrier map");
        // Records: appended before a successful barrier of its role are durable and of this slot; before a failed one, absent.
        std::map<storage::JournalRole, std::vector<const TraceEvent*>> unbarriered;
        for (const TraceEvent& e : window_events) {
            const storage::JournalRole role{e.kind.starts_with("signer.") ? storage::JournalRole::SIGNER : storage::JournalRole::COMMIT};
            if (e.kind.ends_with(".append")) {
                unbarriered[role].push_back(&e);
            } else if (e.kind.ends_with(".barrier.end")) {
                for (const TraceEvent* append : unbarriered[role]) {
                    const auto it{durable[role].find(append->seq)};
                    if (e.ok) {
                        const bool ok{it != durable[role].end() && it->second.type == append->record && slot_of(it->second) == event.slot};
                        m_checks.Expect("P3", ok, "record " + append->Line() + " is not a durable record of its slot");
                    } else {
                        m_checks.Expect("P3", it == durable[role].end(), "record " + append->Line() + " survived its failed barrier");
                    }
                }
                unbarriered[role].clear();
            } else if (e.kind == "read.publish") {
                ++publications;
            }
        }
        for (const auto& [role, appends] : unbarriered) {
            m_checks.Expect("P3", appends.empty(), "an append without a barrier in slot " + std::to_string(event.slot));
        }
    }
    m_checks.Expect("P3", !in_window, "the last slot window never ended");
    m_checks.Expect("P3", seen == m_windows, "the slot windows are not the script's SealSlot calls");
    size_t committed{0};
    for (const ExpectedWindow& window : m_windows) committed += window.result == "OK" ? 1 : 0;
    m_checks.Expect("P3", publications == committed && m_node->Sealed().size() == committed, "not exactly one publication per committed slot");
}

void Driver::FillReport(RunReport& report)
{
    report.variant = m_variant;
    report.seed = m_seed;
    report.harness_error = m_harness;
    report.checks = m_checks.Results();
    for (const std::string_view id : ManifestOf(m_variant)) {
        if (!report.checks.contains(id)) report.checks.emplace(std::string{id}, CheckResult{false, false, "not run"});
    }
    if (m_node != nullptr) {
        for (const SealedSlot& slot : m_node->Sealed()) {
            report.headers.push_back(slot.head->HeaderHash());
            report.canonical_order.push_back(OrderOf(slot.height));
        }
        report.final_state_root = m_node->Owner().Head()->Header().state_root;
        report.replay_batch = ReplayBatch(*m_node, m_labels);
    }
    report.trace_digest = m_trace.Digest();
    report.trace_events = m_trace.Size();
}

RunReport Driver::Run()
{
    RunReport report;
    if (Setup()) {
        bool done{false};
        switch (m_variant) {
        case Variant::WALKING_SKELETON:
            done = ScriptBase();
            break;
        case Variant::STORAGE_FAULT:
            done = ScriptStorageFault();
            break;
        case Variant::AUTHORITY_FAULT:
            done = ScriptAuthorityFault();
            break;
        }
        if (!done && m_harness.empty()) NoteHarness("the script stopped without a reason");
        CheckTrace();
    }
    FillReport(report);
    return report;
}

// ---- JSON ---------------------------------------------------------------------------

//! A minimal JSON value whose objects are written with sorted keys.
class Json
{
public:
    static Json Object() { return Json{Kind::OBJECT}; }
    static Json Array() { return Json{Kind::ARRAY}; }
    static Json String(std::string value)
    {
        Json json{Kind::STRING};
        json.m_string = std::move(value);
        return json;
    }
    static Json Number(uint64_t value)
    {
        Json json{Kind::NUMBER};
        json.m_number = value;
        return json;
    }
    static Json Bool(bool value)
    {
        Json json{Kind::BOOL};
        json.m_bool = value;
        return json;
    }
    Json& Set(std::string key, Json value)
    {
        for (size_t i{0}; i < m_keys.size(); ++i) {
            if (m_keys[i] == key) {
                m_items[i] = std::move(value);
                return *this;
            }
        }
        m_keys.push_back(std::move(key));
        m_items.push_back(std::move(value));
        return *this;
    }
    Json& Push(Json value)
    {
        m_items.push_back(std::move(value));
        return *this;
    }
    void Write(std::string& out, size_t indent) const
    {
        const std::string pad(indent + 2, ' ');
        switch (m_kind) {
        case Kind::BOOL:
            out += m_bool ? "true" : "false";
            return;
        case Kind::NUMBER:
            out += std::to_string(m_number);
            return;
        case Kind::STRING:
            WriteString(out, m_string);
            return;
        case Kind::ARRAY:
            if (m_items.empty()) {
                out += "[]";
                return;
            }
            out += "[\n";
            for (size_t i{0}; i < m_items.size(); ++i) {
                out += pad;
                m_items[i].Write(out, indent + 2);
                out += i + 1 < m_items.size() ? ",\n" : "\n";
            }
            out += std::string(indent, ' ') + "]";
            return;
        case Kind::OBJECT: {
            if (m_items.empty()) {
                out += "{}";
                return;
            }
            std::vector<size_t> order(m_keys.size());
            for (size_t i{0}; i < order.size(); ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return m_keys[a] < m_keys[b]; });
            out += "{\n";
            for (size_t n{0}; n < order.size(); ++n) {
                out += pad;
                WriteString(out, m_keys[order[n]]);
                out += ": ";
                m_items[order[n]].Write(out, indent + 2);
                out += n + 1 < order.size() ? ",\n" : "\n";
            }
            out += std::string(indent, ' ') + "}";
            return;
        }
        }
    }

private:
    enum class Kind : uint8_t { BOOL, NUMBER, STRING, ARRAY, OBJECT };
    explicit Json(Kind kind) : m_kind{kind} {}

    static void WriteString(std::string& out, const std::string& value)
    {
        static constexpr char HEX[]{"0123456789abcdef"};
        out += '"';
        for (const char c : value) {
            const unsigned char u{static_cast<unsigned char>(c)};
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (u < 0x20 || u >= 0x7f) {
                out += "\\u00";
                out += HEX[u >> 4];
                out += HEX[u & 0xf];
            } else {
                out += c;
            }
        }
        out += '"';
    }

    Kind m_kind;
    bool m_bool{false};
    uint64_t m_number{0};
    std::string m_string;
    std::vector<std::string> m_keys;
    std::vector<Json> m_items;
};

Json ChecksJson(const CheckMap& checks)
{
    Json out{Json::Object()};
    for (const auto& [id, result] : checks) {
        out.Set(id, Json::Object()
                        .Set("detail", Json::String(result.detail))
                        .Set("passed", Json::Bool(result.passed))
                        .Set("ran", Json::Bool(result.ran)));
    }
    return out;
}

} // namespace

std::string_view ToString(Variant variant)
{
    switch (variant) {
    case Variant::WALKING_SKELETON:
        return "walking_skeleton";
    case Variant::STORAGE_FAULT:
        return "walking_skeleton_storage_fault";
    case Variant::AUTHORITY_FAULT:
        return "walking_skeleton_authority_fault";
    }
    return "UNKNOWN";
}

std::optional<Variant> VariantFromName(std::string_view name)
{
    for (const Variant variant : ALL_VARIANTS) {
        if (ToString(variant) == name) return variant;
    }
    return std::nullopt;
}

std::span<const Variant> AllVariants() { return ALL_VARIANTS; }
std::span<const std::string_view> AllChecks() { return ALL_CHECKS; }
std::span<const std::string_view> SuiteChecks() { return SUITE_CHECKS; }

std::vector<std::string_view> ManifestOf(Variant variant)
{
    switch (variant) {
    case Variant::WALKING_SKELETON:
        return {"P1", "P2", "P3", "P4", "P5", "P7", "P8", "N1", "N2", "N3", "N4", "N5", "N6", "N7", "N8", "N9", "N12", "N13"};
    case Variant::STORAGE_FAULT:
        return {"P2", "P3", "P4", "P8", "N10"};
    case Variant::AUTHORITY_FAULT:
        return {"P2", "P3", "P4", "P8", "N11"};
    }
    return std::vector<std::string_view>{}; // an unregistered variant runs nothing
}

bool RunReport::Passed() const
{
    if (!harness_error.empty()) return false;
    for (const std::string_view id : ManifestOf(variant)) {
        const auto it{checks.find(id)};
        if (it == checks.end() || !it->second.ran || !it->second.passed) return false;
    }
    for (const auto& [id, result] : checks) {
        if (result.ran && !result.passed) return false;
    }
    return true;
}

RunReport RunWalkingSkeleton(uint64_t seed, Variant variant)
{
    Driver driver{seed, variant};
    return driver.Run();
}

CheckResult EvaluateP6(std::span<const RunReport> first, std::span<const RunReport> second, bool need_distinct)
{
    Checks p6;
    if (first.empty()) return CheckResult{false, false, "no run"};
    p6.Expect("P6", first.size() == second.size(), "not every seed ran twice");
    std::set<uint256> digests;
    for (size_t i{0}; i < first.size(); ++i) {
        const RunReport& run{first[i]};
        const std::string where{std::string{ToString(run.variant)} + " seed " + std::to_string(run.seed)};
        if (i < second.size()) {
            const RunReport& again{second[i]};
            p6.Expect("P6", again.seed == run.seed && again.variant == run.variant, where + ": the second run is of another seed");
            p6.Expect("P6", run.trace_digest == again.trace_digest, where + ": the same seed twice gives another trace digest");
            p6.Expect("P6", run.headers == again.headers && run.final_state_root == again.final_state_root,
                      where + ": the same seed twice gives other headers or root");
        }
        p6.Expect("P6", run.headers == first[0].headers, where + ": header hashes differ from seed " + std::to_string(first[0].seed));
        p6.Expect("P6", run.final_state_root == first[0].final_state_root,
                  where + ": final state root differs from seed " + std::to_string(first[0].seed));
        digests.insert(run.trace_digest);
    }
    if (need_distinct && first.size() >= 8) {
        p6.Expect("P6", digests.size() >= 2, "eight or more seeds gave one trace digest: the seed permutes nothing");
    }
    return p6.Results().at("P6");
}

SuiteReport RunSuite(std::span<const Variant> variants, uint64_t seed_from, uint64_t seed_to)
{
    SuiteReport report;
    report.variants.assign(variants.begin(), variants.end());
    report.seed_from = seed_from;
    report.seed_to = seed_to;
    Checks suite;
    bool all_ran{!variants.empty() && seed_from <= seed_to};
    for (const Variant variant : variants) {
        std::vector<RunReport> first;
        std::vector<RunReport> second;
        for (uint64_t seed{seed_from}; seed_from <= seed_to && seed <= seed_to; ++seed) {
            first.push_back(RunWalkingSkeleton(seed, variant));
            second.push_back(RunWalkingSkeleton(seed, variant));
            if (seed == std::numeric_limits<uint64_t>::max()) break;
        }
        std::set<uint256> digests;
        for (const RunReport& run : first) digests.insert(run.trace_digest);
        report.distinct_trace_digests[std::string{ToString(variant)}] = digests.size();
        const CheckResult p6{EvaluateP6(first, second, variant == Variant::WALKING_SKELETON)};
        suite.Expect("P6", p6.ran && p6.passed, p6.detail);
        for (RunReport& run : first) report.runs.push_back(std::move(run));
    }
    report.suite = suite.Results();
    for (const std::string_view id : SuiteChecks()) {
        if (!report.suite.contains(id)) report.suite.emplace(std::string{id}, CheckResult{false, false, "not run"});
    }

    // The required manifest: every selected variant's checks and the suite's.
    std::set<std::string, std::less<>> required;
    for (const Variant variant : variants) {
        for (const std::string_view id : ManifestOf(variant)) required.emplace(id);
    }
    for (const std::string_view id : SuiteChecks()) required.emplace(id);
    bool ok{all_ran};
    for (const std::string& id : required) {
        bool ran{false};
        bool failed{false};
        const auto note = [&](const CheckMap& checks) {
            const auto it{checks.find(id)};
            if (it == checks.end() || !it->second.ran) return;
            ran = true;
            failed = failed || !it->second.passed;
        };
        for (const RunReport& run : report.runs) note(run.checks);
        note(report.suite);
        const std::string verdict{!ran ? "not_run" : failed ? "fail" : "pass"};
        report.manifest[id] = verdict;
        ok = ok && verdict == "pass";
    }
    for (const RunReport& run : report.runs) ok = ok && run.Passed();
    for (const auto& [id, result] : report.suite) ok = ok && result.ran && result.passed;
    report.ok = ok;
    return report;
}

std::string SuiteJson(const SuiteReport& report)
{
    Json runs{Json::Array()};
    for (const RunReport& run : report.runs) {
        Json headers{Json::Array()};
        for (const uint256& header : run.headers) headers.Push(Json::String(header.GetHex()));
        Json order{Json::Array()};
        for (const std::string& slot : run.canonical_order) order.Push(Json::String(slot));
        runs.Push(Json::Object()
                      .Set("canonical_order", std::move(order))
                      .Set("checks", ChecksJson(run.checks))
                      .Set("final_state_root", Json::String(run.final_state_root.GetHex()))
                      .Set("harness_error", Json::String(run.harness_error))
                      .Set("headers", std::move(headers))
                      .Set("passed", Json::Bool(run.Passed()))
                      .Set("seed", Json::Number(run.seed))
                      .Set("trace_digest", Json::String(run.trace_digest.GetHex()))
                      .Set("trace_events", Json::Number(run.trace_events))
                      .Set("variant", Json::String(std::string{ToString(run.variant)})));
    }
    Json manifest{Json::Object()};
    for (const auto& [id, verdict] : report.manifest) manifest.Set(id, Json::String(verdict));
    Json distinct{Json::Object()};
    for (const auto& [name, count] : report.distinct_trace_digests) distinct.Set(name, Json::Number(count));
    Json variants{Json::Array()};
    for (const Variant variant : report.variants) variants.Push(Json::String(std::string{ToString(variant)}));
    Json root{Json::Object()};
    root.Set("distinct_trace_digests", std::move(distinct))
        .Set("format", Json::String("fm2-sim/1"))
        .Set("label", Json::String("PROVISIONAL REGTEST walking skeleton; not a latency or milestone claim"))
        .Set("manifest", std::move(manifest))
        .Set("ok", Json::Bool(report.ok))
        .Set("runs", std::move(runs))
        .Set("seeds", Json::Object().Set("from", Json::Number(report.seed_from)).Set("to", Json::Number(report.seed_to)))
        .Set("suite", ChecksJson(report.suite))
        .Set("variants", std::move(variants));
    std::string out;
    root.Write(out, 0);
    out += '\n';
    return out;
}

} // namespace flowmesh2::sim
