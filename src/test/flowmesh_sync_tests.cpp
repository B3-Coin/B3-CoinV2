// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! FlowMesh certified-log node layer: multi-node propose/attest/certify
//! convergence with signed proposer envelopes and out-of-identity
//! evidence, durable-before-live commit ordering, compare-and-set lock
//! journaling with automatic restart restore (StartValidator),
//! guard-before-cache proposal admission, anchor revalidation before
//! signing/commit and across replay/snapshots, replacement-proposer
//! recovery, catch-up, and FlowMesh-outage isolation from B3.

#include <flowmesh/sync.h>

#include <flowmesh/auth.h>
#include <flowmesh/certificate.h>
#include <flowmesh/microblock.h>
#include <flowmesh/pool.h>
#include <flowmesh/recovery.h>
#include <flowmesh/state.h>
#include <key.h>
#include <node/flowmesh_anchor.h>
#include <node/flowmesh_store.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <map>
#include <memory>
#include <thread>
#include <optional>
#include <set>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(flowmesh_sync_tests, TestChain100Setup)

namespace {

using flowmesh::Action;
using flowmesh::ActionType;
using flowmesh::AnchorRef;
using flowmesh::AttestationMsg;
using flowmesh::CertifiedEntry;
using flowmesh::DepositInfo;
using flowmesh::FlowMeshState;
using flowmesh::MeshHalt;
using flowmesh::MeshNode;
using flowmesh::ProposalMsg;

const uint256 VAULT{uint256{"00000000000000000000000000000000000000000000000000000000000000f1"}};
const uint256 MESH_DOMAIN{
    uint256{"00000000000000000000000000000000000000000000000000000000000000dd"}};
const uint256 MESH_CONFIG{flowmesh::ComputeExecutionConfigId(
    uint256{"00000000000000000000000000000000000000000000000000000000000000f1"},
    modern::IssuanceAssetId(COutPoint{
        Txid::FromUint256(uint256{"0000000000000000000000000000000000000000000000000000000000000011"}),
        0}),
    modern::NativeAsset(), 8)};

modern::AssetId BaseX()
{
    return modern::IssuanceAssetId(COutPoint{
        Txid::FromUint256(uint256{"0000000000000000000000000000000000000000000000000000000000000011"}),
        0});
}
const modern::AssetId& Quote() { return modern::NativeAsset(); }

CKey MakeKey(const unsigned char seed)
{
    std::vector<unsigned char> data(32, seed);
    data[31] = 1;
    CKey key;
    key.Set(data.begin(), data.end(), /*fCompressedIn=*/true);
    BOOST_REQUIRE(key.IsValid());
    return key;
}

XOnlyPubKey Xonly(const CKey& key) { return XOnlyPubKey{key.GetPubKey()}; }

COutPoint Outpoint(const unsigned char tag, const uint32_t n)
{
    std::vector<unsigned char> data(32, tag);
    return COutPoint{Txid::FromUint256(uint256{data}), n};
}

class FixedAnchors final : public flowmesh::AnchorPolicy
{
public:
    AnchorRef current{7, uint256{"00000000000000000000000000000000000000000000000000000000000000aa"}};
    bool accept_all{true};
    bool still_canonical{true};

    bool Acceptable(const AnchorRef& anchor) const override
    {
        return accept_all && anchor == current;
    }
    bool StillCanonical(const AnchorRef& anchor) const override
    {
        if (anchor.IsNull()) return true;
        return still_canonical;
    }
    AnchorRef Current() const override { return current; }
};

class MapDeposits final : public flowmesh::DepositVerifier
{
public:
    std::map<COutPoint, DepositInfo> entries;

    std::optional<DepositInfo> GetDeposit(const COutPoint& outpoint,
                                          const AnchorRef&) const override
    {
        const auto it{entries.find(outpoint)};
        if (it == entries.end()) return std::nullopt;
        return it->second;
    }
};

//! Ephemeral in-memory journal implementing the compare-and-set
//! contract, with a fail switch. Durability itself is exercised by the
//! store-backed journal below.
class MemJournal final : public flowmesh::LockJournal
{
public:
    bool fail{false};
    std::map<uint64_t, uint256> locks;

    bool WriteLock(const uint64_t sequence, const uint256& hash) override
    {
        if (fail) return false;
        const auto it{locks.find(sequence)};
        if (it != locks.end()) return it->second == hash; // CAS: refuse different
        locks.emplace(sequence, hash);
        return true;
    }
    bool ClearLocksThrough(const uint64_t sequence) override
    {
        if (fail) return false;
        for (auto it{locks.begin()}; it != locks.end();) {
            it = it->first <= sequence ? locks.erase(it) : std::next(it);
        }
        return true;
    }
};

//! An in-memory commit sink that can be told to fail (a durable-store
//! stand-in for message-flow tests).
class FailableSink final : public flowmesh::CommitSink
{
public:
    bool fail{false};
    std::vector<CertifiedEntry> committed;

    bool OnCommit(const CertifiedEntry& entry) override
    {
        if (fail) return false;
        committed.push_back(entry);
        return true;
    }
};

struct MeshNet {
    std::vector<CKey> keys;
    std::set<XOnlyPubKey> seats;
    uint64_t threshold{0};
    std::unique_ptr<flowmesh::RoundRobinSchedule> schedule;
    flowmesh::SchnorrActionAuthenticator auth{MESH_DOMAIN, MESH_CONFIG};
    FixedAnchors anchors;
    MapDeposits deposits;
    CKey alice_key{MakeKey(0xa1)};
    CKey bob_key{MakeKey(0xb2)};
    flowmesh::AccountId alice{flowmesh::AccountForKey(Xonly(alice_key))};
    flowmesh::AccountId bob{flowmesh::AccountForKey(Xonly(bob_key))};
    std::vector<std::unique_ptr<MemJournal>> journals;
    std::vector<std::unique_ptr<FailableSink>> sinks;
    std::vector<std::unique_ptr<MeshNode>> nodes; // one per seat
    std::unique_ptr<MeshNode> observer;           // no seat key

    explicit MeshNet(const size_t n_seats, const uint64_t f)
    {
        std::vector<XOnlyPubKey> seat_keys;
        for (size_t i{0}; i < n_seats; ++i) {
            keys.push_back(MakeKey(static_cast<unsigned char>(0x10 + i)));
            seat_keys.push_back(Xonly(keys.back()));
            seats.insert(seat_keys.back());
        }
        const auto t{flowmesh::MinCertificateThreshold(n_seats, f)};
        BOOST_REQUIRE(t.has_value());
        threshold = *t;
        schedule = std::make_unique<flowmesh::RoundRobinSchedule>(seat_keys);

        deposits.entries[Outpoint(0x0a, 0)] = {Quote(), 600'000, alice};
        deposits.entries[Outpoint(0x0b, 1)] = {BaseX(), 10, bob};

        for (size_t i{0}; i < n_seats; ++i) {
            journals.push_back(std::make_unique<MemJournal>());
            sinks.push_back(std::make_unique<FailableSink>());
            nodes.push_back(MakeNode(keys[i], sinks.back().get(), journals.back().get()));
        }
        observer = MakeNode(std::nullopt);
    }

    std::unique_ptr<MeshNode> MakeNode(std::optional<CKey> seat_key,
                                       flowmesh::CommitSink* sink = nullptr,
                                       flowmesh::LockJournal* journal = nullptr,
                                       const flowmesh::CatchupSource* history = nullptr,
                                       const uint256& last_hash = {},
                                       const std::map<uint64_t, uint256>& locks = {},
                                       std::optional<FlowMeshState> state = std::nullopt) const
    {
        MeshNode::Config config;
        config.domain = MESH_DOMAIN;
        config.market_config_id = MESH_CONFIG;
        config.seats = seats;
        config.threshold = threshold;
        config.schedule = schedule.get();
        config.auth = &auth;
        config.deposits = &deposits;
        config.anchors = &anchors;
        config.sink = sink;
        config.history = history;
        config.seat_key = std::move(seat_key);
        config.lock_journal = journal;
        FlowMeshState genesis{state ? std::move(*state)
                                    : FlowMeshState{VAULT, BaseX(), Quote()}};
        if (config.seat_key.has_value()) {
            // TEST BRIDGE: unit tests construct signing nodes with
            // in-memory sinks/journals; production signing nodes exist
            // only via node::StartValidator.
            return flowmesh::test_only::SigningBridge::UnsafeMake(std::move(config),
                                                                  std::move(genesis),
                                                                  last_hash, locks);
        }
        return std::make_unique<MeshNode>(std::move(config), std::move(genesis), last_hash);
    }

    size_t ProposerIndex(const uint64_t sequence, const uint32_t round) const
    {
        const auto scheduled{schedule->ProposerAt(sequence, round)};
        BOOST_REQUIRE(scheduled.has_value());
        for (size_t i{0}; i < keys.size(); ++i) {
            if (Xonly(keys[i]) == *scheduled) return i;
        }
        BOOST_REQUIRE(false);
        return 0;
    }

    Action SignedOrder(const CKey& key, const uint64_t seq, const bool buy, const CAmount price,
                       const CAmount qty) const
    {
        Action a;
        a.signer = flowmesh::AccountForKey(Xonly(key));
        a.sequence = seq;
        a.type = static_cast<uint8_t>(buy ? ActionType::SUBMIT_BID : ActionType::SUBMIT_ASK);
        const auto curve{buy ? flowmesh::MakeLimitBidCurve(price, qty)
                             : flowmesh::MakeLimitAskCurve(price, qty)};
        BOOST_REQUIRE(curve.has_value());
        a.curve = *curve;
        BOOST_REQUIRE(flowmesh::SignAction(key, MESH_DOMAIN, MESH_CONFIG, a));
        return a;
    }

    Action Deposit(const COutPoint& outpoint) const
    {
        Action a;
        a.type = static_cast<uint8_t>(ActionType::DEPOSIT);
        a.outpoint = outpoint;
        return a;
    }

    CertifiedEntry RunRound(const std::vector<Action>& actions)
    {
        const uint64_t seq{nodes[0]->Sequence()};
        const size_t p{ProposerIndex(seq, 0)};
        for (const Action& a : actions) BOOST_REQUIRE(nodes[p]->SubmitAction(a));
        const std::optional<ProposalMsg> proposal{nodes[p]->TryPropose()};
        BOOST_REQUIRE(proposal.has_value());

        std::vector<AttestationMsg> atts;
        for (auto& node : nodes) {
            if (const auto att{node->HandleProposal(*proposal)}) atts.push_back(*att);
        }
        BOOST_REQUIRE(observer->HandleProposal(*proposal) == std::nullopt);

        std::optional<CertifiedEntry> entry;
        for (const AttestationMsg& att : atts) {
            for (auto& node : nodes) {
                if (const auto e{node->HandleAttestation(att)}) {
                    if (!entry) entry = e;
                }
            }
        }
        BOOST_REQUIRE(entry.has_value());
        for (auto& node : nodes) BOOST_REQUIRE(node->HandleCertified(*entry));
        BOOST_REQUIRE(observer->HandleCertified(*entry));
        return *entry;
    }
};

} // namespace

BOOST_AUTO_TEST_CASE(three_nodes_certify_and_converge)
{
    MeshNet net{3, /*f=*/0};

    const CertifiedEntry e0{net.RunRound(
        {net.Deposit(Outpoint(0x0a, 0)), net.Deposit(Outpoint(0x0b, 1)),
         net.SignedOrder(net.alice_key, 0, /*buy=*/true, 50'000, 10),
         net.SignedOrder(net.bob_key, 0, /*buy=*/false, 50'000, 10)})};
    BOOST_CHECK_EQUAL(e0.mb.sequence, 0U);
    for (const Action& body_action : e0.mb.actions) {
        BOOST_CHECK(body_action.credential.empty()); // evidence never enters the body
    }

    // Microblock 1: a withdrawal intent — a REQUEST, never redeemable
    // (B3 authorization is an unresolved owner decision).
    Action withdraw;
    withdraw.signer = net.alice;
    withdraw.sequence = 1;
    withdraw.type = static_cast<uint8_t>(ActionType::WITHDRAW);
    withdraw.asset = BaseX();
    withdraw.amount = 10;
    withdraw.destination =
        uint256{"00000000000000000000000000000000000000000000000000000000000000d1"};
    BOOST_REQUIRE(flowmesh::SignAction(net.alice_key, MESH_DOMAIN, MESH_CONFIG, withdraw));
    const CertifiedEntry e1{net.RunRound({withdraw})};
    BOOST_CHECK_EQUAL(e1.mb.sequence, 1U);
    BOOST_CHECK(e1.mb.parent_hash == e0.mb.GetHash());

    const uint256 root{net.nodes[0]->State().Root()};
    for (auto& node : net.nodes) {
        BOOST_CHECK_EQUAL(node->Sequence(), 2U);
        BOOST_CHECK_EQUAL(node->State().Root().GetHex(), root.GetHex());
        BOOST_CHECK(node->Evidence().empty());
        BOOST_CHECK(!node->Halted());
        BOOST_CHECK(node->State().LedgerView().SolvencyHolds());
    }
    BOOST_CHECK_EQUAL(net.observer->State().Root().GetHex(), root.GetHex());

    BOOST_CHECK_EQUAL(net.nodes[0]->State().LedgerView().Available(net.bob, Quote()), 500'000);
    BOOST_CHECK_EQUAL(net.nodes[0]->State().LedgerView().Available(net.alice, BaseX()), 0);
}

BOOST_AUTO_TEST_CASE(commit_is_durable_before_live_and_storage_failure_halts)
{
    MeshNet net{3, 0};

    net.RunRound({net.Deposit(Outpoint(0x0a, 0))});
    BOOST_REQUIRE_EQUAL(net.sinks[0]->committed.size(), 1U); // durable first, then live
    BOOST_CHECK_EQUAL(net.nodes[0]->Sequence(), 1U);

    // Storage starts failing on node 0: the next certified entry must
    // NOT advance its live tip; the node fail-stops.
    net.sinks[0]->fail = true;
    const size_t p{net.ProposerIndex(1, 0)};
    BOOST_REQUIRE(net.nodes[p]->SubmitAction(net.SignedOrder(net.alice_key, 0, true, 40'000, 2)));
    const auto proposal{net.nodes[p]->TryPropose()};
    BOOST_REQUIRE(proposal.has_value());
    std::vector<AttestationMsg> atts;
    for (auto& node : net.nodes) {
        if (const auto att{node->HandleProposal(*proposal)}) atts.push_back(*att);
    }
    std::optional<CertifiedEntry> entry;
    for (const AttestationMsg& att : atts) {
        for (auto& node : net.nodes) {
            if (const auto e{node->HandleAttestation(att)}) {
                if (!entry) entry = e;
            }
        }
    }
    BOOST_REQUIRE(entry.has_value()); // another node certified
    BOOST_CHECK(!net.nodes[0]->HandleCertified(*entry));
    BOOST_CHECK_EQUAL(net.nodes[0]->Sequence(), 1U); // live tip did NOT advance
    BOOST_CHECK(net.nodes[0]->Halted());
    BOOST_CHECK(net.nodes[0]->Halt() == MeshHalt::PERSIST_FAILED);
    BOOST_CHECK(!net.nodes[0]->TryPropose().has_value());
    BOOST_CHECK(!net.nodes[0]->SubmitAction(net.SignedOrder(net.bob_key, 0, false, 1, 1)));
}

BOOST_AUTO_TEST_CASE(signing_validators_require_full_durable_state)
{
    // Codex re-audit item 7: a seat without a durable sink OR without a
    // lock journal is an invalid signing configuration.
    MeshNet net{3, 0};
    MemJournal journal;
    FailableSink sink;
    auto no_sink{net.MakeNode(net.keys[0], nullptr, &journal)};
    BOOST_CHECK(no_sink->Halted());
    BOOST_CHECK(no_sink->Halt() == MeshHalt::INVALID_CONFIG);
    auto no_journal{net.MakeNode(net.keys[0], &sink, nullptr)};
    BOOST_CHECK(no_journal->Halted());
    BOOST_CHECK(no_journal->Halt() == MeshHalt::INVALID_CONFIG);
    auto observer{net.MakeNode(std::nullopt)}; // observers need neither
    BOOST_CHECK(!observer->Halted());

    // Lock-journal failure prevents signing.
    const size_t p{net.ProposerIndex(0, 0)};
    const auto proposal{net.nodes[p]->TryPropose()};
    BOOST_REQUIRE(proposal.has_value());
    const size_t other{(p + 1) % 3};
    net.journals[other]->fail = true;
    BOOST_CHECK(!net.nodes[other]->HandleProposal(*proposal).has_value());
    BOOST_CHECK(net.nodes[other]->Halted());
    BOOST_CHECK(net.nodes[other]->Halt() == MeshHalt::LOCK_JOURNAL_FAILED);

    // Threshold zero is refused up front.
    {
        MeshNode::Config config;
        config.domain = MESH_DOMAIN;
        config.market_config_id = MESH_CONFIG;
        config.seats = net.seats;
        config.threshold = 0;
        config.schedule = net.schedule.get();
        config.auth = &net.auth;
        config.anchors = &net.anchors;
        const MeshNode invalid{std::move(config), FlowMeshState{VAULT, BaseX(), Quote()}};
        BOOST_CHECK(invalid.Halt() == MeshHalt::INVALID_CONFIG);
    }
}

BOOST_AUTO_TEST_CASE(guards_run_before_candidate_cache)
{
    // Codex re-audit item 9: wrong-round / wrong-proposer / forged
    // proposals are refused BEFORE execution or caching, so spam cannot
    // exhaust the bounded candidate cache and shadow a legit proposal.
    MeshNet net{3, 0};
    const size_t p0{net.ProposerIndex(0, 0)};
    const size_t p1{net.ProposerIndex(0, 1)};
    size_t victim{0};
    while (victim == p0 || victim == p1) ++victim;

    // Build a legitimate round-0 proposal for later.
    BOOST_REQUIRE(net.nodes[p0]->SubmitAction(net.Deposit(Outpoint(0x0a, 0))));
    const auto legit{net.nodes[p0]->TryPropose()};
    BOOST_REQUIRE(legit.has_value());

    // Spam: 20 distinct FUTURE-ROUND proposals authored by the correct
    // round-1 proposer, plus forged-signature variants. None may cache.
    MemJournal spam_journal;
    FailableSink spam_sink;
    for (int i{0}; i < 20; ++i) {
        auto spammer{net.MakeNode(net.keys[p1], &spam_sink, &spam_journal)};
        BOOST_REQUIRE(spammer->SubmitAction(
            net.SignedOrder(net.alice_key, 0, true, 10'000 + i * 1'000, 2)));
        spammer->NoteTimeout(); // round 1: p1 is scheduled there
        auto spam{spammer->TryPropose()};
        BOOST_REQUIRE(spam.has_value());
        BOOST_CHECK(!net.nodes[victim]->HandleProposal(*spam).has_value()); // wrong round here
        ProposalMsg forged{*spam};
        forged.round = 0;
        forged.proposer = Xonly(net.keys[p0]); // claims round-0 proposer, wrong signature
        BOOST_CHECK(!net.nodes[victim]->HandleProposal(forged).has_value());
    }
    // The legit proposal still admits, executes and attests: the cache
    // was never consumed by the spam.
    BOOST_CHECK(net.nodes[victim]->HandleProposal(*legit).has_value());
}

BOOST_AUTO_TEST_CASE(replacement_proposer_continues_a_locked_candidate)
{
    MeshNet net{3, 0}; // t = 2
    const size_t p0{net.ProposerIndex(0, 0)};
    BOOST_REQUIRE(net.nodes[p0]->SubmitAction(net.Deposit(Outpoint(0x0a, 0))));
    const auto proposal_a{net.nodes[p0]->TryPropose()};
    BOOST_REQUIRE(proposal_a.has_value());

    const size_t p1{net.ProposerIndex(0, 1)};
    const auto att_p1{net.nodes[p1]->HandleProposal(*proposal_a)};
    BOOST_REQUIRE(att_p1.has_value());

    for (auto& node : net.nodes) node->NoteTimeout();
    const auto proposal_b{net.nodes[p1]->TryPropose()};
    BOOST_REQUIRE(proposal_b.has_value());
    BOOST_CHECK(proposal_b->mb.GetHash() == proposal_a->mb.GetHash()); // same candidate
    BOOST_CHECK(!(proposal_b->proposer == proposal_a->proposer));      // new author

    std::vector<AttestationMsg> atts{*att_p1};
    for (auto& node : net.nodes) {
        if (const auto att{node->HandleProposal(*proposal_b)}) atts.push_back(*att);
    }
    std::optional<CertifiedEntry> entry;
    for (const AttestationMsg& att : atts) {
        for (auto& node : net.nodes) {
            if (const auto e{node->HandleAttestation(att)}) {
                if (!entry) entry = e;
            }
        }
    }
    BOOST_REQUIRE(entry.has_value());
    BOOST_CHECK(entry->mb.GetHash() == proposal_a->mb.GetHash());
    for (auto& node : net.nodes) {
        BOOST_CHECK(node->HandleCertified(*entry));
        BOOST_CHECK_EQUAL(node->Sequence(), 1U);
        BOOST_CHECK(node->Evidence().empty());
    }
}

BOOST_AUTO_TEST_CASE(lock_journal_is_compare_and_set_across_restart)
{
    // Codex re-audit item 6: no lock -> write; same lock -> idempotent;
    // different lock -> refuse. Across reopen.
    const fs::path store_path{m_args.GetDataDirBase() / "flowmesh_cas"};
    MeshNet net{3, 0};
    std::string error;
    const uint256 h1{uint256::ONE};
    const uint256 h2{uint256{"0000000000000000000000000000000000000000000000000000000000000002"}};
    {
        node::FlowMeshStore store{
            DBParams{.path = store_path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
        BOOST_REQUIRE(store.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
        BOOST_CHECK(store.WriteLock(0, h1));
        BOOST_CHECK(store.WriteLock(0, h1));  // idempotent
        BOOST_CHECK(!store.WriteLock(0, h2)); // conflicting: refused
        std::map<uint64_t, uint256> locks;
        BOOST_REQUIRE(store.ReadLocks(locks, error));
        BOOST_CHECK(locks.at(0) == h1);
    }
    {
        node::FlowMeshStore reopened{
            DBParams{.path = store_path, .cache_bytes = size_t{1} << 20}};
        BOOST_REQUIRE(reopened.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
        BOOST_CHECK(!reopened.WriteLock(0, h2)); // still refused after restart
        BOOST_CHECK(reopened.WriteLock(0, h1));
        BOOST_CHECK(reopened.ClearLocksThrough(0));
        BOOST_CHECK(reopened.WriteLock(0, h2)); // cleared lock frees the slot
    }
}

BOOST_AUTO_TEST_CASE(store_appends_replays_snapshots_and_survives_restart)
{
    const fs::path store_path{m_args.GetDataDirBase() / "flowmesh_log"};
    MeshNet net{3, 0};

    auto store{std::make_unique<node::FlowMeshStore>(
        DBParams{.path = store_path, .cache_bytes = size_t{1} << 20, .wipe_data = true})};
    std::string error;
    BOOST_REQUIRE_MESSAGE(store->OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error),
                          error);
    node::StoreCommitSink sink{*store};
    node::StoreLockJournal journal{*store};
    net.nodes[0] = net.MakeNode(net.keys[0], &sink, &journal);

    net.RunRound({net.Deposit(Outpoint(0x0a, 0)), net.Deposit(Outpoint(0x0b, 1)),
                  net.SignedOrder(net.alice_key, 0, true, 50'000, 10),
                  net.SignedOrder(net.bob_key, 0, false, 50'000, 10)});
    const FlowMeshState mid_state{net.nodes[0]->State()}; // after entries [0, 1)
    net.RunRound({net.SignedOrder(net.alice_key, 1, true, 40'000, 2)});
    BOOST_REQUIRE(!sink.LastError().has_value());
    const uint256 live_root{net.nodes[0]->State().Root()};
    const uint256 live_hash{net.nodes[0]->LastHash()};

    // Reconstruction with anchors revalidated and dependencies reported.
    {
        FlowMeshState state{VAULT, BaseX(), Quote()};
        uint256 last_hash;
        std::map<std::pair<int32_t, uint256>, uint64_t> anchors_out;
        BOOST_REQUIRE_MESSAGE(store->Replay(state, last_hash, net.auth, &net.deposits,
                                            net.seats, net.threshold, &net.anchors, error,
                                            &anchors_out),
                              error);
        BOOST_CHECK_EQUAL(state.Root().GetHex(), live_root.GetHex());
        BOOST_CHECK_EQUAL(last_hash.GetHex(), live_hash.GetHex());
        BOOST_CHECK_EQUAL(anchors_out.size(), 1U); // both entries share the fixed anchor
    }
    // Replay under a different quorum fails closed.
    {
        FlowMeshState state{VAULT, BaseX(), Quote()};
        uint256 last_hash;
        BOOST_CHECK(!store->Replay(state, last_hash, net.auth, &net.deposits,
                                   std::set<XOnlyPubKey>{Xonly(net.keys[0])}, 1, nullptr,
                                   error));
    }
    // Replay refuses history whose anchors left the canonical chain.
    {
        net.anchors.still_canonical = false;
        FlowMeshState state{VAULT, BaseX(), Quote()};
        uint256 last_hash;
        BOOST_CHECK(!store->Replay(state, last_hash, net.auth, &net.deposits, net.seats,
                                   net.threshold, &net.anchors, error, nullptr));
        net.anchors.still_canonical = true;
    }

    // Certificate-verified snapshot; wrong state refused at write time.
    BOOST_REQUIRE_MESSAGE(store->WriteSnapshot(1, mid_state, error), error);
    BOOST_CHECK(!store->WriteSnapshot(2, mid_state, error));
    {
        FlowMeshState state{VAULT, BaseX(), Quote()};
        uint256 last_hash;
        BOOST_REQUIRE_MESSAGE(store->ReplayFromBestSnapshot(state, last_hash, net.auth,
                                                            &net.deposits, net.seats,
                                                            net.threshold, &net.anchors, error,
                                                            nullptr),
                              error);
        BOOST_CHECK_EQUAL(state.Root().GetHex(), live_root.GetHex());
        BOOST_CHECK_EQUAL(last_hash.GetHex(), live_hash.GetHex());
    }
    // Codex re-audit item 12: a snapshot cannot bypass anchor validity
    // for the SKIPPED PREFIX — with the prefix anchor off-chain, the
    // snapshot is rejected AND the verified-replay fallback also
    // refuses, so reconstruction fails safe rather than accepting
    // state derived from an orphaned B3 anchor.
    {
        net.anchors.still_canonical = false;
        FlowMeshState state{VAULT, BaseX(), Quote()};
        uint256 last_hash;
        BOOST_CHECK(!store->ReplayFromBestSnapshot(state, last_hash, net.auth, &net.deposits,
                                                   net.seats, net.threshold, &net.anchors,
                                                   error, nullptr));
        net.anchors.still_canonical = true;
    }

    // "Restart" through the PRODUCTION lifecycle: StartValidator wires
    // sink/journal/history and restores tip, state, locks and anchor
    // dependencies automatically.
    net.nodes[0] = net.MakeNode(net.keys[0], net.sinks[0].get(), net.journals[0].get());
    store.reset();
    {
        node::FlowMeshStore reopened{
            DBParams{.path = store_path, .cache_bytes = size_t{1} << 20}};
        MeshNode::Config config;
        config.domain = MESH_DOMAIN;
        config.market_config_id = MESH_CONFIG;
        config.seats = net.seats;
        config.threshold = net.threshold;
        config.schedule = net.schedule.get();
        config.auth = &net.auth;
        config.deposits = &net.deposits;
        config.anchors = &net.anchors;
        config.seat_key = net.keys[0];
        node::ValidatorRuntime runtime;
        BOOST_REQUIRE_MESSAGE(node::StartValidator(reopened, std::move(config),
                                                   FlowMeshState{VAULT, BaseX(), Quote()},
                                                   runtime, error),
                              error);
        BOOST_CHECK_EQUAL(runtime.mesh_node->Sequence(), 2U);
        BOOST_CHECK_EQUAL(runtime.mesh_node->State().Root().GetHex(), live_root.GetHex());
        BOOST_CHECK(runtime.mesh_node->LastHash() == live_hash);
        // Committed-anchor dependencies were restored: a later B3 reorg
        // of that anchor halts the restarted node.
        BOOST_CHECK_EQUAL(runtime.mesh_node->CommittedAnchors().size(), 1U);
        net.anchors.still_canonical = false;
        BOOST_CHECK(!runtime.mesh_node->RecheckCommittedAnchors());
        BOOST_CHECK(runtime.mesh_node->Halt() == MeshHalt::ANCHOR_INVALIDATED);
        net.anchors.still_canonical = true;

        // Log discipline: re-append/foreign-domain/foreign-quorum fail.
        const auto e0{reopened.ReadEntry(0)};
        BOOST_REQUIRE(e0.has_value());
        BOOST_CHECK(!reopened.Append(*e0, error));
        BOOST_CHECK(!reopened.OpenForDomain(uint256::ONE, net.seats, net.threshold, error));
        BOOST_CHECK(!reopened.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold - 1, error));

        // Catch-up serves from the durable log after restart.
        node::StoreCatchupSource source{reopened};
        auto restored{net.MakeNode(std::nullopt, nullptr, nullptr, &source, live_hash,
                                   /*locks=*/{}, net.nodes[0]->State())};
        const auto served{restored->HandleCatchupRequest(flowmesh::CatchupRequest{0})};
        BOOST_REQUIRE_EQUAL(served.size(), 2U);
        auto late{net.MakeNode(std::nullopt)};
        BOOST_CHECK_EQUAL(late->HandleCatchupResponse(served), 2U);
        BOOST_CHECK_EQUAL(late->State().Root().GetHex(), live_root.GetHex());
    }
}

BOOST_AUTO_TEST_CASE(restart_cannot_sign_a_conflicting_candidate)
{
    // The journaled lock is restored AUTOMATICALLY by StartValidator: a
    // restarted validator refuses to sign a different candidate at its
    // protected sequence.
    const fs::path store_path{m_args.GetDataDirBase() / "flowmesh_locks"};
    MeshNet net{3, 0};
    std::string error;
    auto store{std::make_unique<node::FlowMeshStore>(
        DBParams{.path = store_path, .cache_bytes = size_t{1} << 20, .wipe_data = true})};
    BOOST_REQUIRE(store->OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
    node::StoreCommitSink sink{*store};
    node::StoreLockJournal journal{*store};
    const size_t p0{net.ProposerIndex(0, 0)};
    const size_t voter{(p0 + 1) % 3};
    net.nodes[voter] = net.MakeNode(net.keys[voter], &sink, &journal);

    BOOST_REQUIRE(net.nodes[p0]->SubmitAction(net.Deposit(Outpoint(0x0a, 0))));
    const auto proposal_a{net.nodes[p0]->TryPropose()};
    BOOST_REQUIRE(proposal_a.has_value());
    BOOST_REQUIRE(net.nodes[voter]->HandleProposal(*proposal_a).has_value());

    // Crash + restart the voter through the production lifecycle.
    net.nodes[voter] = net.MakeNode(std::nullopt); // drop the crashed instance
    store.reset();
    node::FlowMeshStore reopened{DBParams{.path = store_path, .cache_bytes = size_t{1} << 20}};
    MeshNode::Config config;
    config.domain = MESH_DOMAIN;
    config.market_config_id = MESH_CONFIG;
    config.seats = net.seats;
    config.threshold = net.threshold;
    config.schedule = net.schedule.get();
    config.auth = &net.auth;
    config.deposits = &net.deposits;
    config.anchors = &net.anchors;
    config.seat_key = net.keys[voter];
    node::ValidatorRuntime runtime;
    BOOST_REQUIRE_MESSAGE(node::StartValidator(reopened, std::move(config),
                                               FlowMeshState{VAULT, BaseX(), Quote()}, runtime,
                                               error),
                          error);

    // A conflicting candidate at the protected sequence is refused...
    MemJournal j2;
    FailableSink s2;
    auto fresh_p0{net.MakeNode(net.keys[p0], &s2, &j2)};
    const auto conflicting{fresh_p0->TryPropose()}; // empty pool: different candidate
    BOOST_REQUIRE(conflicting.has_value());
    BOOST_REQUIRE(conflicting->mb.GetHash() != proposal_a->mb.GetHash());
    BOOST_CHECK(!runtime.mesh_node->HandleProposal(*conflicting).has_value());
    // ...while re-attesting the locked candidate remains possible.
    BOOST_CHECK(runtime.mesh_node->HandleProposal(*proposal_a).has_value());
}

BOOST_AUTO_TEST_CASE(vote_split_recovers_without_double_finality)
{
    MeshNet net{3, 0}; // t = 2

    const size_t p0{net.ProposerIndex(0, 0)};
    BOOST_REQUIRE(net.nodes[p0]->SubmitAction(net.Deposit(Outpoint(0x0a, 0))));
    const auto proposal_a{net.nodes[p0]->TryPropose()};
    BOOST_REQUIRE(proposal_a.has_value());
    const auto att_a{net.nodes[p0]->HandleProposal(*proposal_a)};
    BOOST_REQUIRE(att_a.has_value());
    for (auto& node : net.nodes) BOOST_CHECK(!node->HandleAttestation(*att_a).has_value());

    const size_t p1{net.ProposerIndex(0, 1)};
    BOOST_REQUIRE(p1 != p0);
    for (size_t i{0}; i < net.nodes.size(); ++i) {
        if (i != p0) net.nodes[i]->NoteTimeout();
    }
    const auto proposal_b{net.nodes[p1]->TryPropose()};
    BOOST_REQUIRE(proposal_b.has_value());
    BOOST_CHECK(proposal_b->mb.GetHash() != proposal_a->mb.GetHash());

    std::vector<AttestationMsg> atts_b;
    for (size_t i{0}; i < net.nodes.size(); ++i) {
        const auto att{net.nodes[i]->HandleProposal(*proposal_b)};
        if (i == p0) {
            BOOST_CHECK(!att.has_value());
        } else if (att) {
            atts_b.push_back(*att);
        }
    }
    BOOST_REQUIRE_EQUAL(atts_b.size(), 2U);

    std::optional<CertifiedEntry> entry;
    for (const AttestationMsg& att : atts_b) {
        for (auto& node : net.nodes) {
            if (const auto e{node->HandleAttestation(att)}) {
                if (!entry) entry = e;
            }
        }
    }
    BOOST_REQUIRE(entry.has_value());
    BOOST_CHECK(entry->mb.GetHash() == proposal_b->mb.GetHash());

    BOOST_REQUIRE(net.nodes[p0]->HandleCertified(*entry));
    for (auto& node : net.nodes) {
        BOOST_CHECK_EQUAL(node->Sequence(), 1U);
        BOOST_CHECK(node->LastHash() == proposal_b->mb.GetHash());
        BOOST_CHECK(node->Evidence().empty());
    }
}

BOOST_AUTO_TEST_CASE(equivocating_proposer_cannot_double_certify)
{
    MeshNet net{4, 1}; // t = 3: any two certificates share an honest seat
    const size_t p0{net.ProposerIndex(0, 0)};

    BOOST_REQUIRE(net.nodes[p0]->SubmitAction(net.Deposit(Outpoint(0x0a, 0))));
    const auto proposal_a{net.nodes[p0]->TryPropose()};
    BOOST_REQUIRE(proposal_a.has_value());
    MemJournal shadow_journal;
    FailableSink shadow_sink;
    auto shadow{net.MakeNode(net.keys[p0], &shadow_sink, &shadow_journal)};
    const auto proposal_b{shadow->TryPropose()}; // empty pool: different candidate
    BOOST_REQUIRE(proposal_b.has_value());
    BOOST_CHECK(proposal_a->mb.GetHash() != proposal_b->mb.GetHash());

    std::vector<size_t> honest;
    for (size_t i{0}; i < net.nodes.size(); ++i) {
        if (i != p0) honest.push_back(i);
    }
    std::vector<AttestationMsg> atts;
    if (const auto a{net.nodes[p0]->HandleProposal(*proposal_a)}) atts.push_back(*a);
    if (const auto a{net.nodes[honest[0]]->HandleProposal(*proposal_a)}) atts.push_back(*a);
    if (const auto a{net.nodes[honest[1]]->HandleProposal(*proposal_a)}) atts.push_back(*a);
    if (const auto a{net.nodes[honest[2]]->HandleProposal(*proposal_b)}) atts.push_back(*a);
    const auto double_sign{flowmesh::SignAttestation(net.keys[p0], MESH_DOMAIN, 0,
                                                     proposal_b->mb.GetHash())};
    BOOST_REQUIRE(double_sign.has_value());
    atts.push_back(AttestationMsg{0, proposal_b->mb.GetHash(), *double_sign});

    std::vector<CertifiedEntry> certificates;
    for (const AttestationMsg& att : atts) {
        for (auto& node : net.nodes) {
            if (const auto e{node->HandleAttestation(att)}) certificates.push_back(*e);
        }
    }
    for (const CertifiedEntry& cert : certificates) {
        BOOST_CHECK(cert.mb.GetHash() == proposal_a->mb.GetHash());
    }
    bool evidence_seen{false};
    for (auto& node : net.nodes) {
        evidence_seen = evidence_seen || !node->Evidence().empty();
        BOOST_CHECK(node->LastHash() == proposal_a->mb.GetHash() || node->Sequence() == 0);
    }
    BOOST_CHECK(evidence_seen);
}

BOOST_AUTO_TEST_CASE(anchors_are_rechecked_before_signing_and_before_commit)
{
    // Codex re-audit item 10: a B3 reorg BETWEEN proposal receipt and
    // signing, or between attestation gathering and commit, must halt
    // FlowMesh progression (B3 untouched).
    MeshNet net{3, 0};
    net.RunRound({net.Deposit(Outpoint(0x0a, 0))}); // committed history w/ anchor

    // Before SIGNING: proposal built while the anchor was canonical...
    const size_t p{net.ProposerIndex(1, 0)};
    const auto proposal{net.nodes[p]->TryPropose()};
    BOOST_REQUIRE(proposal.has_value());
    // ...the committed anchor is orphaned before another seat signs.
    const size_t signer{(p + 1) % 3};
    net.anchors.still_canonical = false;
    BOOST_CHECK(!net.nodes[signer]->HandleProposal(*proposal).has_value());
    BOOST_CHECK(net.nodes[signer]->Halted());
    BOOST_CHECK(net.nodes[signer]->Halt() == MeshHalt::ANCHOR_INVALIDATED);
    net.anchors.still_canonical = true;

    // Before COMMIT: attestations gather while canonical, the reorg
    // lands just before the certificate completes on the third node.
    const size_t committer{(p + 2) % 3};
    std::vector<AttestationMsg> atts;
    if (const auto a{net.nodes[p]->HandleProposal(*proposal)}) atts.push_back(*a);
    if (const auto a{net.nodes[committer]->HandleProposal(*proposal)}) atts.push_back(*a);
    BOOST_REQUIRE_EQUAL(atts.size(), 2U); // t = 2 reachable
    BOOST_REQUIRE(net.nodes[committer]->HandleAttestation(atts[0]) == std::nullopt);
    net.anchors.still_canonical = false;
    BOOST_CHECK(net.nodes[committer]->HandleAttestation(atts[1]) == std::nullopt);
    BOOST_CHECK_EQUAL(net.nodes[committer]->Sequence(), 1U); // live tip did NOT advance
    BOOST_CHECK(net.nodes[committer]->Halted());
    BOOST_CHECK(net.nodes[committer]->Halt() == MeshHalt::ANCHOR_INVALIDATED);
    net.anchors.still_canonical = true;

    // Chain-backed policy sanity over the regtest fixture chain.
    const node::ChainAnchorPolicy policy{*Assert(m_node.chainman), /*min_depth=*/6};
    const int tip_height{m_node.chainman->ActiveChain().Height()};
    const AnchorRef current{policy.Current()};
    BOOST_CHECK_EQUAL(current.height, tip_height - 6);
    BOOST_CHECK(policy.Acceptable(current));
    BOOST_CHECK(policy.StillCanonical(current));
    const AnchorRef tip{tip_height, m_node.chainman->ActiveChain().Tip()->GetBlockHash()};
    BOOST_CHECK(!policy.Acceptable(tip));
    BOOST_CHECK(policy.StillCanonical(tip));
    BOOST_CHECK(!policy.StillCanonical(AnchorRef{current.height, uint256::ONE}));
    BOOST_CHECK(policy.StillCanonical(AnchorRef{}));
}

BOOST_AUTO_TEST_CASE(b3_reorg_invalidates_previously_acceptable_anchors)
{
    const node::ChainAnchorPolicy policy{*Assert(m_node.chainman), /*min_depth=*/2};
    const AnchorRef before{policy.Current()};
    BOOST_REQUIRE(policy.Acceptable(before));
    BOOST_REQUIRE(policy.StillCanonical(before));

    BlockValidationState state;
    CBlockIndex* anchored{WITH_LOCK(
        cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(before.hash))};
    BOOST_REQUIRE(anchored != nullptr);
    m_node.chainman->ActiveChainstate().InvalidateBlock(state, anchored);
    BOOST_REQUIRE(state.IsValid());

    BOOST_CHECK(!policy.Acceptable(before));
    BOOST_CHECK(!policy.StillCanonical(before));
    const AnchorRef after{policy.Current()};
    BOOST_CHECK(after.height < before.height);
    BOOST_CHECK(policy.Acceptable(after));
}

BOOST_AUTO_TEST_CASE(pool_admission_is_atomic)
{
    MeshNet net{3, 0};
    flowmesh::ActionPool pool{&net.auth};
    const Action first{net.SignedOrder(net.alice_key, 0, true, 40'000, 2)};
    const Action conflicting{net.SignedOrder(net.alice_key, 0, true, 41'000, 3)};
    BOOST_REQUIRE(pool.Add(first));
    const size_t size_after{pool.Size()};
    const size_t bytes_after{pool.Bytes()};
    BOOST_CHECK(!pool.Add(conflicting));
    BOOST_CHECK_EQUAL(pool.Size(), size_after);
    BOOST_CHECK_EQUAL(pool.Bytes(), bytes_after);
    BOOST_CHECK(!pool.Add(first)); // exact duplicate refused
    BOOST_CHECK_EQUAL(pool.Bytes(), bytes_after);
    FlowMeshState state{VAULT, BaseX(), Quote()};
    const auto batch{pool.SelectBatch(state, 16)};
    BOOST_REQUIRE_EQUAL(batch.size(), 1U);
    BOOST_CHECK(batch[0].Id() == first.Id());
}

BOOST_AUTO_TEST_CASE(flowmesh_outage_never_stalls_b3)
{
    MeshNet net{3, 0};

    const size_t p0{net.ProposerIndex(0, 0)};
    const auto proposal{net.nodes[p0]->TryPropose()};
    BOOST_REQUIRE(proposal.has_value());
    const auto own_att{net.nodes[p0]->HandleProposal(*proposal)};
    BOOST_REQUIRE(own_att.has_value());
    BOOST_CHECK(!net.nodes[p0]->HandleAttestation(*own_att).has_value());
    BOOST_CHECK_EQUAL(net.nodes[p0]->Sequence(), 0U);

    const int height_before{m_node.chainman->ActiveChain().Height()};
    CreateAndProcessBlock({}, CScript{} << OP_TRUE);
    BOOST_CHECK_EQUAL(m_node.chainman->ActiveChain().Height(), height_before + 1);
    BOOST_CHECK_EQUAL(net.nodes[p0]->Sequence(), 0U);
    BOOST_CHECK(net.nodes[p0]->State().LedgerView().SolvencyHolds());
}

BOOST_AUTO_TEST_CASE(public_construction_cannot_obtain_signing_capability)
{
    // Codex item 3: the public MeshNode constructor is observer-only —
    // signing material is stripped and the instance is refused; no
    // restore maps are accepted publicly. Signing nodes exist only via
    // node::StartValidator (production) or the loud test bridge.
    MeshNet net{3, 0};
    MemJournal journal;
    FailableSink sink;
    MeshNode::Config config;
    config.domain = MESH_DOMAIN;
    config.market_config_id = MESH_CONFIG;
    config.seats = net.seats;
    config.threshold = net.threshold;
    config.schedule = net.schedule.get();
    config.auth = &net.auth;
    config.anchors = &net.anchors;
    config.sink = &sink;
    config.seat_key = net.keys[0];
    config.lock_journal = &journal;
    MeshNode smuggled{std::move(config), FlowMeshState{VAULT, BaseX(), Quote()}};
    BOOST_CHECK(smuggled.Halted());
    BOOST_CHECK(smuggled.Halt() == MeshHalt::INVALID_CONFIG);
    BOOST_CHECK(!smuggled.TryPropose().has_value());
    // A config-id mismatch is likewise refused (wrong-market node).
    MeshNode::Config wrong;
    wrong.domain = MESH_DOMAIN;
    wrong.market_config_id = uint256::ONE;
    wrong.seats = net.seats;
    wrong.threshold = net.threshold;
    wrong.schedule = net.schedule.get();
    wrong.auth = &net.auth;
    wrong.anchors = &net.anchors;
    const MeshNode mismatched{std::move(wrong), FlowMeshState{VAULT, BaseX(), Quote()}};
    BOOST_CHECK(mismatched.Halt() == MeshHalt::INVALID_CONFIG);
}

BOOST_AUTO_TEST_CASE(store_freshness_cas_and_corruption_discipline)
{
    // Codex items 4/6 regressions.
    MeshNet net{3, 0};
    std::string error;
    const fs::path path{m_args.GetDataDirBase() / "flowmesh_hardening"};

    // (a) Missing marker with NONEMPTY namespaces is inconsistent
    // storage, never "fresh".
    {
        CDBWrapper raw{DBParams{.path = path, .cache_bytes = size_t{1} << 20,
                                .wipe_data = true}};
        raw.Write(std::make_pair(uint8_t{'l'}, uint64_t{3}), uint256::ONE, /*fSync=*/true);
    }
    {
        node::FlowMeshStore store{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
        BOOST_CHECK(!store.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
    }

    // (b) Serialized conflicting CAS under concurrency: exactly one
    // hash wins and persists; the loser is refused, never overwrites.
    const uint256 h1{uint256::ONE};
    const uint256 h2{uint256{"0000000000000000000000000000000000000000000000000000000000000002"}};
    {
        node::FlowMeshStore store{DBParams{.path = path, .cache_bytes = size_t{1} << 20,
                                           .wipe_data = true}};
        BOOST_REQUIRE(store.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
        std::atomic<int> wins_h1{0}, wins_h2{0};
        std::thread a{[&] {
            for (int i{0}; i < 50; ++i) {
                if (store.WriteLock(7, h1)) ++wins_h1;
            }
        }};
        std::thread b{[&] {
            for (int i{0}; i < 50; ++i) {
                if (store.WriteLock(7, h2)) ++wins_h2;
            }
        }};
        a.join();
        b.join();
        // One side won the slot; the other side never succeeded.
        BOOST_CHECK((wins_h1 == 0) != (wins_h2 == 0));
        std::map<uint64_t, uint256> locks;
        BOOST_REQUIRE(store.ReadLocks(locks, error));
        BOOST_REQUIRE_EQUAL(locks.size(), 1U);
        BOOST_CHECK(locks.at(7) == (wins_h1 > 0 ? h1 : h2));
    }

    // (c) A storage ERROR is never "key missing": a lock slot holding
    // undecodable bytes REFUSES a write instead of being overwritten,
    // and the namespace scan reports corruption. A malformed short
    // 'l'-prefixed key likewise fails the scan.
    {
        node::FlowMeshStore store{DBParams{.path = path, .cache_bytes = size_t{1} << 20,
                                           .wipe_data = true}};
        BOOST_REQUIRE(store.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
        {
            CDBWrapper raw{DBParams{.path = m_args.GetDataDirBase() / "flowmesh_raw",
                                    .cache_bytes = size_t{1} << 20, .wipe_data = true}};
        }
    }
    {
        // Corrupt lock VALUE (short bytes) written raw.
        CDBWrapper raw{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
        raw.Write(std::make_pair(uint8_t{'l'}, uint64_t{9}), uint8_t{0x42}, /*fSync=*/true);
    }
    {
        node::FlowMeshStore store{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
        BOOST_CHECK(!store.WriteLock(9, h1)); // ERROR != missing: refuse, never replace
        std::map<uint64_t, uint256> locks;
        BOOST_CHECK(!store.ReadLocks(locks, error)); // corrupt namespace reported
    }
}

BOOST_AUTO_TEST_CASE(snapshot_rejects_invalid_historical_certificate_or_evidence)
{
    // Codex item 6: a valid TIP certificate is not proof of the skipped
    // prefix. Corrupting entry 0's admission evidence (which leaves the
    // microblock hash, parent chain AND its certificate fully valid)
    // must reject the snapshot; the verified-replay fallback then also
    // refuses, so reconstruction fails safe.
    const fs::path store_path{m_args.GetDataDirBase() / "flowmesh_snapprefix"};
    MeshNet net{3, 0};
    std::string error;
    auto store{std::make_unique<node::FlowMeshStore>(
        DBParams{.path = store_path, .cache_bytes = size_t{1} << 20, .wipe_data = true})};
    BOOST_REQUIRE(store->OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
    node::StoreCommitSink sink{*store};
    node::StoreLockJournal journal{*store};
    net.nodes[0] = net.MakeNode(net.keys[0], &sink, &journal);

    // Entry 0 carries a SIGNED action (so it has evidence to corrupt).
    net.RunRound({net.Deposit(Outpoint(0x0a, 0)),
                  net.SignedOrder(net.alice_key, 0, true, 40'000, 2)});
    const FlowMeshState mid_state{net.nodes[0]->State()};
    net.RunRound({net.SignedOrder(net.alice_key, 1, true, 41'000, 2)});
    BOOST_REQUIRE_MESSAGE(store->WriteSnapshot(2, net.nodes[0]->State(), error), error);
    (void)mid_state;

    // Corrupt entry 0's evidence in place (hash/cert unchanged). Close
    // the store first: LevelDB holds an exclusive lock.
    auto e0{store->ReadEntry(0)};
    BOOST_REQUIRE(e0.has_value());
    BOOST_REQUIRE(!e0->credentials.empty());
    auto e0_evidence{*e0};
    e0_evidence.credentials[0][40] ^= 0x01;
    auto e0_cert{*e0};
    e0_cert.cert.attestations[0].sig[7] ^= 0x01;
    net.nodes[0] = net.MakeNode(std::nullopt); // detach sink/journal
    store.reset();
    {
        CDBWrapper raw{DBParams{.path = store_path, .cache_bytes = size_t{1} << 20}};
        raw.Write(std::make_pair(uint8_t{'e'}, uint64_t{0}), e0_evidence, /*fSync=*/true);
    }
    FlowMeshState state{VAULT, BaseX(), Quote()};
    uint256 last_hash;
    {
        node::FlowMeshStore reopened{
            DBParams{.path = store_path, .cache_bytes = size_t{1} << 20}};
        BOOST_CHECK(!reopened.ReplayFromBestSnapshot(state, last_hash, net.auth, &net.deposits,
                                                     net.seats, net.threshold, &net.anchors,
                                                     error, nullptr));
    }

    // A corrupted historical CERTIFICATE is equally fatal.
    {
        CDBWrapper raw{DBParams{.path = store_path, .cache_bytes = size_t{1} << 20}};
        raw.Write(std::make_pair(uint8_t{'e'}, uint64_t{0}), e0_cert, /*fSync=*/true);
    }
    {
        node::FlowMeshStore reopened{
            DBParams{.path = store_path, .cache_bytes = size_t{1} << 20}};
        BOOST_CHECK(!reopened.ReplayFromBestSnapshot(state, last_hash, net.auth, &net.deposits,
                                                     net.seats, net.threshold, &net.anchors,
                                                     error, nullptr));
    }
}

BOOST_AUTO_TEST_CASE(known_split_lock_case_safely_does_not_finalize)
{
    // Codex item 8: k=4, f=1, t=3; honest locks split 2/1 and the
    // Byzantine seat withholds. NO certificate may form (safety), the
    // honest nodes neither halt nor equivocate, and B3 continues. No
    // unlock rule exists — resolving this is an OWNER DECISION.
    MeshNet net{4, 1}; // t = 3
    const size_t p0{net.ProposerIndex(0, 0)};
    const size_t p1{net.ProposerIndex(0, 1)};
    std::vector<size_t> honest;
    for (size_t i{0}; i < net.nodes.size(); ++i) {
        if (i != p0) honest.push_back(i); // p0 plays the withholding Byzantine seat
    }

    // Round 0: candidate A reaches two honest seats, which lock on it.
    BOOST_REQUIRE(net.nodes[p0]->SubmitAction(net.Deposit(Outpoint(0x0a, 0))));
    const auto proposal_a{net.nodes[p0]->TryPropose()};
    BOOST_REQUIRE(proposal_a.has_value());
    std::vector<AttestationMsg> atts;
    size_t locked_a{0};
    for (const size_t i : honest) {
        if (locked_a == 2) break;
        if (const auto att{net.nodes[i]->HandleProposal(*proposal_a)}) {
            atts.push_back(*att);
            ++locked_a;
        }
    }
    BOOST_REQUIRE_EQUAL(locked_a, 2U);

    // Round 1: a different candidate B locks the remaining honest seat.
    for (const size_t i : honest) net.nodes[i]->NoteTimeout();
    size_t p1_honest{p1};
    if (p1 == p0) p1_honest = honest[0]; // ensure an honest round-1 proposer path
    MemJournal j2;
    FailableSink s2;
    auto round1_proposer{net.MakeNode(net.keys[net.ProposerIndex(0, 1)], &s2, &j2)};
    round1_proposer->NoteTimeout();
    const auto proposal_b{round1_proposer->TryPropose()};
    BOOST_REQUIRE(proposal_b.has_value());
    BOOST_CHECK(proposal_b->mb.GetHash() != proposal_a->mb.GetHash());
    for (const size_t i : honest) {
        if (const auto att{net.nodes[i]->HandleProposal(*proposal_b)}) atts.push_back(*att);
    }
    (void)p1_honest;

    // Deliver every attestation everywhere: with locks split 2/1 and
    // the Byzantine seat silent, neither hash can reach t = 3.
    std::vector<CertifiedEntry> certificates;
    for (const AttestationMsg& att : atts) {
        for (auto& node : net.nodes) {
            if (const auto e{node->HandleAttestation(att)}) certificates.push_back(*e);
        }
    }
    BOOST_CHECK(certificates.empty()); // SAFE non-finalization
    for (const size_t i : honest) {
        BOOST_CHECK_EQUAL(net.nodes[i]->Sequence(), 0U);
        BOOST_CHECK(!net.nodes[i]->Halted());       // stalled, not broken
        BOOST_CHECK(net.nodes[i]->Evidence().empty()); // honest split != equivocation
    }
    // B3 continues regardless.
    const int height_before{m_node.chainman->ActiveChain().Height()};
    CreateAndProcessBlock({}, CScript{} << OP_TRUE);
    BOOST_CHECK_EQUAL(m_node.chainman->ActiveChain().Height(), height_before + 1);
}

BOOST_AUTO_TEST_SUITE_END()
