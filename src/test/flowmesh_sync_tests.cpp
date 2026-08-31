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
#include <test/util/asset.h>

#include <flowmesh/auth.h>
#include <flowmesh/certificate.h>
#include <flowmesh/microblock.h>
#include <flowmesh/pool.h>
#include <flowmesh/recovery.h>
#include <flowmesh/state.h>
#include <key.h>
#include <node/flowmesh_anchor.h>
#include <node/flowmesh_store.h>
#include <test/util/flowmesh.h>
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
    modern::test_only::SyntheticAssetId(COutPoint{
        Txid::FromUint256(uint256{"0000000000000000000000000000000000000000000000000000000000000011"}),
        0}),
    modern::NativeAsset(), 8)};

modern::AssetId BaseX()
{
    return modern::test_only::SyntheticAssetId(COutPoint{
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
    //! Simulates the owner-configured burial/finality requirement: an
    //! anchor may remain canonical yet stop satisfying full
    //! acceptability (e.g. a competing branch reduced its burial).
    bool sufficiently_buried{true};

    bool Acceptable(const AnchorRef& anchor) const override
    {
        return accept_all && sufficiently_buried && anchor == current &&
               StillCanonical(anchor);
    }
    //! When set, exactly this hash is treated as reorged-away while
    //! everything else stays canonical (models a targeted B3 reorg).
    uint256 orphaned_hash;

    bool StillCanonical(const AnchorRef& anchor) const override
    {
        if (anchor.IsNull()) return true;
        if (!orphaned_hash.IsNull() && anchor.hash == orphaned_hash) return false;
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

    std::optional<CAmount> GetWithdrawalCapacity(
        const modern::AssetId&, const AnchorRef&) const override
    {
        return MAX_MONEY;
    }


    std::optional<std::vector<flowmesh::WithdrawalSettlementFactV1>>
    GetWithdrawalSettlements(const std::optional<AnchorRef>&,
                             const AnchorRef&) const override
    {
        return std::vector<flowmesh::WithdrawalSettlementFactV1>{};
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

//! Test funding shortcut over the test-only bridge.
inline bool Fund(flowmesh::FlowMeshState& state, const flowmesh::AccountId& account,
                 const modern::AssetId& asset, const CAmount amount)
{
    return flowmesh::test_only::StateFunding::Fund(state, account, asset, amount);
}

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
        BOOST_REQUIRE_MESSAGE(node::StartValidator(reopened, std::move(config), VAULT, BaseX(),
                                                   Quote(), 8, runtime, error),
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
    BOOST_REQUIRE_MESSAGE(node::StartValidator(reopened, std::move(config), VAULT, BaseX(),
                                               Quote(), 8, runtime, error),
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
    // A must actually certify (3 honest-side attestations reach t=3);
    // without this the test could pass vacuously with nothing certified.
    BOOST_REQUIRE(!certificates.empty());
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
    // ...the committed anchor is orphaned before another seat signs:
    // the proposal is refused (the full-acceptability gate fails), and
    // the committed-dependency recheck halts the node.
    const size_t signer{(p + 1) % 3};
    net.anchors.still_canonical = false;
    BOOST_CHECK(!net.nodes[signer]->HandleProposal(*proposal).has_value());
    BOOST_CHECK(!net.nodes[signer]->RecheckCommittedAnchors());
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
    const auto [tip_height, tip_hash]{WITH_LOCK(cs_main, return (std::pair{
        m_node.chainman->ActiveChain().Height(),
        m_node.chainman->ActiveChain().Tip()->GetBlockHash()}))};
    const AnchorRef current{policy.Current()};
    BOOST_CHECK_EQUAL(current.height, tip_height - 6);
    BOOST_CHECK(policy.Acceptable(current));
    BOOST_CHECK(policy.StillCanonical(current));
    const AnchorRef tip{tip_height, tip_hash};
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

    const int height_before{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height())};
    CreateAndProcessBlock({}, CScript{} << OP_TRUE);
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height()), height_before + 1);
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
    // The split must actually exist for this test to mean anything:
    // two honest locks on A plus exactly one honest lock on B.
    BOOST_REQUIRE_EQUAL(atts.size(), 3U);

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
    const int height_before{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height())};
    CreateAndProcessBlock({}, CScript{} << OP_TRUE);
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height()), height_before + 1);
}

BOOST_AUTO_TEST_CASE(authenticator_binding_is_verified_by_construction)
{
    // Codex finding 1: a Market-B (or wrong-domain) authenticator
    // inside a Market-A node is refused EXPLICITLY at construction —
    // never left to downstream accidents — and store replay applies
    // the same binding check.
    MeshNet net{3, 0};
    const uint256 config_b{flowmesh::ComputeExecutionConfigId(VAULT, BaseX(), uint256::ONE, 8)};
    const flowmesh::SchnorrActionAuthenticator auth_b{MESH_DOMAIN, config_b};
    const uint256 other_domain{
        uint256{"00000000000000000000000000000000000000000000000000000000000000de"}};
    const flowmesh::SchnorrActionAuthenticator auth_wrong_domain{other_domain, MESH_CONFIG};

    const auto observer_with{[&](const flowmesh::ActionAuthenticator* auth) {
        MeshNode::Config config;
        config.domain = MESH_DOMAIN;
        config.market_config_id = MESH_CONFIG;
        config.seats = net.seats;
        config.threshold = net.threshold;
        config.schedule = net.schedule.get();
        config.auth = auth;
        config.anchors = &net.anchors;
        return MeshNode{std::move(config), FlowMeshState{VAULT, BaseX(), Quote()}};
    }};
    BOOST_CHECK(observer_with(&auth_b).Halt() == MeshHalt::INVALID_CONFIG);
    BOOST_CHECK(observer_with(&auth_wrong_domain).Halt() == MeshHalt::INVALID_CONFIG);
    BOOST_CHECK(!observer_with(&net.auth).Halted()); // correct binding is accepted

    // Store replay refuses a wrong-binding authenticator outright.
    const fs::path store_path{m_args.GetDataDirBase() / "flowmesh_authbind"};
    std::string error;
    node::FlowMeshStore store{
        DBParams{.path = store_path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
    BOOST_REQUIRE(store.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
    FlowMeshState state{VAULT, BaseX(), Quote()};
    uint256 last_hash;
    BOOST_CHECK(!store.Replay(state, last_hash, auth_b, &net.deposits, net.seats,
                              net.threshold, nullptr, error, nullptr));
    BOOST_CHECK(!store.Replay(state, last_hash, auth_wrong_domain, &net.deposits, net.seats,
                              net.threshold, nullptr, error, nullptr));
    BOOST_CHECK(store.Replay(state, last_hash, net.auth, &net.deposits, net.seats,
                             net.threshold, nullptr, error, nullptr)); // empty log, bound ok
}

BOOST_AUTO_TEST_CASE(production_startup_builds_canonical_empty_genesis)
{
    // Codex finding 2: StartValidator no longer accepts a caller-built
    // state at all (fabricated balances/custody/nonces are impossible
    // at the API level); it derives the config id and starts from the
    // canonical empty state. An out-of-cap curve bound is refused.
    MeshNet net{3, 0};
    std::string error;
    const fs::path store_path{m_args.GetDataDirBase() / "flowmesh_genesis"};
    node::FlowMeshStore store{
        DBParams{.path = store_path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
    MeshNode::Config config;
    config.domain = MESH_DOMAIN;
    config.market_config_id = uint256::ONE; // caller lies: derived internally, ignored
    config.seats = net.seats;
    config.threshold = net.threshold;
    config.schedule = net.schedule.get();
    config.auth = &net.auth;
    config.deposits = &net.deposits;
    config.anchors = &net.anchors;
    config.seat_key = net.keys[0];
    node::ValidatorRuntime runtime;
    BOOST_REQUIRE_MESSAGE(node::StartValidator(store, config, VAULT, BaseX(), Quote(), 8,
                                               runtime, error),
                          error);
    BOOST_CHECK_EQUAL(runtime.mesh_node->Sequence(), 0U);
    BOOST_CHECK_EQUAL(runtime.mesh_node->State().LedgerView().Custody(Quote()), 0);
    BOOST_CHECK_EQUAL(runtime.mesh_node->State().LedgerView().Custody(BaseX()), 0);
    BOOST_CHECK(runtime.mesh_node->State().ConfigId() == MESH_CONFIG);

    // Curve bound outside the hard protocol cap: refused.
    node::ValidatorRuntime refused;
    BOOST_CHECK(!node::StartValidator(store, config, VAULT, BaseX(), Quote(),
                                      flowmesh::HARD_MAX_CURVE_POINTS + 1, refused, error));
}

BOOST_AUTO_TEST_CASE(store_namespace_freshness_lock_bounds_and_tip_overrun)
{
    // Codex finding 4 regressions.
    MeshNet net{3, 0};
    std::string error;
    const fs::path path{m_args.GetDataDirBase() / "flowmesh_hardening2"};

    // (A) Non-FlowMesh database metadata does NOT defeat freshness...
    {
        CDBWrapper raw{DBParams{.path = path, .cache_bytes = size_t{1} << 20,
                                .wipe_data = true}};
        raw.Write(std::make_pair(uint8_t{0x0e}, uint8_t{0x01}), uint8_t{0x77}, true);
    }
    {
        node::FlowMeshStore store{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
        BOOST_CHECK(store.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
    }
    // ...but a FlowMesh-namespace key without a marker is fail-closed.
    {
        CDBWrapper raw{DBParams{.path = path, .cache_bytes = size_t{1} << 20,
                                .wipe_data = true}};
        raw.Write(std::make_pair(uint8_t{'e'}, uint64_t{0}), uint8_t{0x01}, true);
    }
    {
        node::FlowMeshStore store{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
        BOOST_CHECK(!store.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
    }

    // (B) A malformed SHORT 'l'-prefixed key fails the namespace scan.
    {
        CDBWrapper raw{DBParams{.path = path, .cache_bytes = size_t{1} << 20,
                                .wipe_data = true}};
        raw.Write(uint8_t{'l'}, uint256::ONE, true); // one-byte key: malformed
    }
    {
        node::FlowMeshStore store{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
        std::map<uint64_t, uint256> locks;
        BOOST_CHECK(!store.ReadLocks(locks, error));
        BOOST_CHECK(!store.WriteLock(0, uint256::ONE)); // journal unusable: never sign blind
    }

    // (C) The journal bound refuses entry MAX+1 BEFORE writing.
    {
        node::FlowMeshStore store{DBParams{.path = path, .cache_bytes = size_t{1} << 20,
                                           .wipe_data = true},
                                  /*max_lock_entries=*/2};
        BOOST_REQUIRE(store.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
        BOOST_CHECK(store.WriteLock(0, uint256::ONE));
        BOOST_CHECK(store.WriteLock(1, uint256::ONE));
        BOOST_CHECK(!store.WriteLock(2, uint256::ONE)); // bound reached: refuse to sign
        BOOST_CHECK(store.ClearLocksThrough(0));
        BOOST_CHECK(store.WriteLock(2, uint256::ONE)); // capacity freed
    }

    // (D) Entries beyond the authoritative tip are detected, not
    // silently ignored.
    {
        node::FlowMeshStore store{DBParams{.path = path, .cache_bytes = size_t{1} << 20,
                                           .wipe_data = true}};
        BOOST_REQUIRE(store.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
    }
    {
        MeshNet fresh{3, 0};
        FlowMeshState state{VAULT, BaseX(), Quote()};
        flowmesh::BatchExecutor exec{state};
        const auto r{exec.ExecuteSlot({}, fresh.anchors.current)};
        BOOST_REQUIRE(r.has_value());
        flowmesh::MicroblockCore mb;
        mb.domain = MESH_DOMAIN;
        mb.anchor = fresh.anchors.current;
        mb.prev_state_root = FlowMeshState{VAULT, BaseX(), Quote()}.Root();
        mb.actions_root = flowmesh::MicroblockCore::ComputeActionsRoot({});
        mb.result_commitment = r->result_commitment;
        mb.resulting_state_root = r->state_root;
        flowmesh::CertifiedEntry beyond{mb, {}, {}};
        CDBWrapper raw{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
        raw.Write(std::make_pair(uint8_t{'e'}, uint64_t{0}), beyond, true); // beyond tip 0
    }
    {
        node::FlowMeshStore store{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
        BOOST_CHECK(!store.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
    }
}

BOOST_AUTO_TEST_CASE(anchor_burial_loss_blocks_signing_and_commit)
{
    // Codex finding 5: the final pre-sign / pre-commit check is the
    // FULL acceptability policy — an anchor that remains canonical but
    // loses its required burial must not be signed against or
    // committed; canonicality loss still halts.
    MeshNet net{3, 0};
    const size_t p{net.ProposerIndex(0, 0)};
    const auto proposal{net.nodes[p]->TryPropose()};
    BOOST_REQUIRE(proposal.has_value());

    // Burial lost between proposal receipt and signing: refuse (no
    // halt — the anchor is still canonical, so this is retriable).
    const size_t signer{(p + 1) % 3};
    net.anchors.sufficiently_buried = false;
    BOOST_CHECK(!net.nodes[signer]->HandleProposal(*proposal).has_value());
    BOOST_CHECK(!net.nodes[signer]->Halted());
    net.anchors.sufficiently_buried = true;
    BOOST_CHECK(net.nodes[signer]->HandleProposal(*proposal).has_value()); // buried again: signs

    // Burial lost just before the certificate completes: commit refused
    // (no halt), live tip unmoved.
    const size_t committer{(p + 2) % 3};
    std::vector<AttestationMsg> atts;
    if (const auto a{net.nodes[p]->HandleProposal(*proposal)}) atts.push_back(*a);
    if (const auto a{net.nodes[committer]->HandleProposal(*proposal)}) atts.push_back(*a);
    BOOST_REQUIRE_EQUAL(atts.size(), 2U);
    BOOST_REQUIRE(net.nodes[committer]->HandleAttestation(atts[0]) == std::nullopt);
    net.anchors.sufficiently_buried = false;
    BOOST_CHECK(net.nodes[committer]->HandleAttestation(atts[1]) == std::nullopt);
    BOOST_CHECK_EQUAL(net.nodes[committer]->Sequence(), 0U);
    BOOST_CHECK(!net.nodes[committer]->Halted());
    net.anchors.sufficiently_buried = true;
}

BOOST_AUTO_TEST_CASE(decoded_book_state_must_reconcile_with_the_ledger)
{
    // Codex finding 7: a snapshot decode cannot construct impossible
    // fill/reservation state or book/ledger divergence, and the curve
    // bound has a hard protocol cap.
    BOOST_CHECK_THROW(
        (FlowMeshState{VAULT, BaseX(), Quote(), flowmesh::HARD_MAX_CURVE_POINTS + 1}),
        std::invalid_argument);
    BOOST_CHECK_THROW((FlowMeshState{VAULT, BaseX(), Quote(), 0}), std::invalid_argument);

    // Craft state streams by serializing a standalone ledger/book pair
    // and appending empty sequence/deposit maps, then decoding into a
    // properly configured state.
    const auto decode{[&](const flowmesh::Ledger& ledger,
                          const flowmesh::ClearingEngine& book) {
        DataStream s;
        s << ledger << book;
        WriteCompactSize(s, 0); // next_seq
        WriteCompactSize(s, 0); // consumed deposits
        FlowMeshState target{VAULT, BaseX(), Quote()};
        s >> target;
        return target;
    }};

    // Consistent state round-trips…
    {
        MeshNet net{3, 0};
        FlowMeshState source{VAULT, BaseX(), Quote()};
        Fund(source, net.alice, Quote(), 1'000);
        BOOST_REQUIRE(source.SubmitCurve(net.alice, flowmesh::ClearingEngine::Side::BID,
                                         {{10, 10}, {20, 0}}));
        DataStream s;
        s << source;
        FlowMeshState target{VAULT, BaseX(), Quote()};
        BOOST_CHECK_NO_THROW(s >> target);
        BOOST_CHECK_EQUAL(target.Root().GetHex(), source.Root().GetHex());
    }
    // …but an orphan ledger reservation with NO backing curve is
    // impossible state:
    {
        MeshNet net{3, 0};
        flowmesh::Ledger ledger{VAULT};
        BOOST_REQUIRE(ledger.Deposit(net.alice, Quote(), 1'000));
        BOOST_REQUIRE(ledger.Reserve(net.alice, Quote(), 400)); // no curve backs this
        const flowmesh::ClearingEngine book{BaseX(), Quote()};
        BOOST_CHECK_THROW(decode(ledger, book), std::ios_base::failure);
    }
}

BOOST_AUTO_TEST_CASE(self_audit_fixes_market_config_store_and_decode)
{
    MeshNet net{3, 0};
    std::string error;

    // base == quote markets are unconstructible (engine, state, startup).
    BOOST_CHECK_THROW((FlowMeshState{VAULT, BaseX(), BaseX()}), std::invalid_argument);
    {
        const fs::path store_path{m_args.GetDataDirBase() / "flowmesh_samequote"};
        node::FlowMeshStore store{
            DBParams{.path = store_path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
        MeshNode::Config config;
        config.domain = MESH_DOMAIN;
        config.seats = net.seats;
        config.threshold = net.threshold;
        config.schedule = net.schedule.get();
        config.auth = &net.auth;
        config.anchors = &net.anchors;
        config.seat_key = net.keys[0];
        node::ValidatorRuntime runtime;
        BOOST_CHECK(!node::StartValidator(store, config, VAULT, BaseX(), BaseX(), 8, runtime,
                                          error));
        BOOST_CHECK(!runtime.mesh_node); // failure never hands back a half-built runtime
    }

    // A negative anchor depth fails loudly instead of warping semantics.
    BOOST_CHECK_THROW((node::ChainAnchorPolicy{*Assert(m_node.chainman), -1}),
                      std::invalid_argument);

    // One signing validator per store: a second StartValidator claim fails.
    {
        const fs::path store_path{m_args.GetDataDirBase() / "flowmesh_singleclaim"};
        node::FlowMeshStore store{
            DBParams{.path = store_path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
        MeshNode::Config config;
        config.domain = MESH_DOMAIN;
        config.seats = net.seats;
        config.threshold = net.threshold;
        config.schedule = net.schedule.get();
        config.auth = &net.auth;
        config.deposits = &net.deposits;
        config.anchors = &net.anchors;
        config.seat_key = net.keys[0];
        node::ValidatorRuntime first;
        BOOST_REQUIRE_MESSAGE(node::StartValidator(store, config, VAULT, BaseX(), Quote(), 8,
                                                   first, error),
                              error);
        node::ValidatorRuntime second;
        BOOST_CHECK(!node::StartValidator(store, config, VAULT, BaseX(), Quote(), 8, second,
                                          error));
        BOOST_CHECK(!second.mesh_node);
    }

    // A stray entry at tip+2 (not just tip) is detected at open.
    {
        const fs::path store_path{m_args.GetDataDirBase() / "flowmesh_straytail"};
        {
            node::FlowMeshStore store{
                DBParams{.path = store_path, .cache_bytes = size_t{1} << 20,
                         .wipe_data = true}};
            BOOST_REQUIRE(store.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
        }
        {
            CDBWrapper raw{DBParams{.path = store_path, .cache_bytes = size_t{1} << 20}};
            raw.Write(std::make_pair(uint8_t{'e'}, uint64_t{2}), uint8_t{0x01}, true);
        }
        node::FlowMeshStore reopened{
            DBParams{.path = store_path, .cache_bytes = size_t{1} << 20}};
        BOOST_CHECK(!reopened.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
    }

    // An idempotent lock rewrite still succeeds at journal capacity —
    // a validator can always re-sign its locked hash after restart.
    {
        const fs::path store_path{m_args.GetDataDirBase() / "flowmesh_capidem"};
        node::FlowMeshStore store{DBParams{.path = store_path, .cache_bytes = size_t{1} << 20,
                                           .wipe_data = true},
                                  /*max_lock_entries=*/2};
        BOOST_REQUIRE(store.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
        BOOST_CHECK(store.WriteLock(0, uint256::ONE));
        BOOST_CHECK(store.WriteLock(1, uint256::ONE));
        BOOST_CHECK(store.WriteLock(1, uint256::ONE)); // idempotent at capacity: allowed
        BOOST_CHECK(!store.WriteLock(2, uint256::ONE)); // new entry at capacity: refused
    }

    // A snapshot stream carrying a DIFFERENT vault is refused at decode
    // (the codec now enforces the whole configuration, not just the
    // market pair), independent of the store's certified-root gate.
    {
        const uint256 other_vault{uint256::ONE};
        FlowMeshState foreign{other_vault, BaseX(), Quote()};
        DataStream stream;
        stream << foreign;
        FlowMeshState target{VAULT, BaseX(), Quote()};
        BOOST_CHECK_THROW(stream >> target, std::ios_base::failure);
    }

    // CheckDecodedAccounting branch coverage: impossible fill, impossible
    // reservation, and a curve/ledger reservation mismatch all throw.
    {
        const auto craft{[&](const CAmount filled, const CAmount reserved,
                             const CAmount ledger_reserved) {
            flowmesh::Ledger ledger{VAULT};
            BOOST_REQUIRE(ledger.Deposit(net.alice, Quote(), 10'000));
            if (ledger_reserved > 0) {
                BOOST_REQUIRE(ledger.Reserve(net.alice, Quote(), ledger_reserved));
            }
            DataStream stream;
            stream << ledger;
            // Hand-build the book stream: one BID curve {(10,10),(20,0)}
            // with the given filled/reserved accounting fields.
            stream << BaseX() << Quote() << uint64_t{8};
            WriteCompactSize(stream, 1);
            stream << uint8_t{0} << net.alice; // Side::BID, account
            WriteCompactSize(stream, 2);
            stream << flowmesh::ClearingEngine::Breakpoint{10, 10}
                   << flowmesh::ClearingEngine::Breakpoint{20, 0};
            stream << filled << reserved;
            WriteCompactSize(stream, 0); // next_seq
            WriteCompactSize(stream, 0); // consumed deposits
            FlowMeshState target{VAULT, BaseX(), Quote()};
            stream >> target;
        }};
        // REACHABLE residuals only (Codex blocker 1): an unfilled bid
        // must carry EXACTLY its submission-time reservation (worst =
        // (10-0)*(20-1) = 190 here); after `filled` lots the residual
        // may lie only within [worst - MaxPrefixSpend, worst] — each
        // filled lot spends between 0 (a zero-price clear) and its
        // per-lot bound (19 here).
        BOOST_CHECK_NO_THROW(craft(0, 190, 190));                      // exact at filled == 0
        BOOST_CHECK_THROW(craft(0, 90, 90), std::ios_base::failure);   // unreachable under-reserve
        BOOST_CHECK_THROW(craft(0, 189, 189), std::ios_base::failure); // even one tick short
        BOOST_CHECK_NO_THROW(craft(1, 171, 171));                      // band floor: 190 - 19
        BOOST_CHECK_NO_THROW(craft(1, 190, 190));                      // zero-price fill: no spend
        BOOST_CHECK_THROW(craft(1, 170, 170), std::ios_base::failure); // below the reachable band
        BOOST_CHECK_THROW(craft(11, 190, 190), std::ios_base::failure); // filled > max qty
        BOOST_CHECK_THROW(craft(0, 191, 191), std::ios_base::failure); // reserved > worst case
        BOOST_CHECK_THROW(craft(0, 190, 180), std::ios_base::failure); // book != ledger
    }

    // The proposal envelope wire codec: strict round trip, and oversized
    // embedded evidence refused before allocation.
    {
        const size_t p{net.ProposerIndex(0, 0)};
        BOOST_REQUIRE(net.nodes[p]->SubmitAction(net.Deposit(Outpoint(0x0a, 0))));
        const auto proposal{net.nodes[p]->TryPropose()};
        BOOST_REQUIRE(proposal.has_value());
        DataStream wire;
        wire << *proposal;
        ProposalMsg decoded;
        wire >> decoded;
        BOOST_CHECK(wire.empty());
        BOOST_CHECK(decoded.mb.GetHash() == proposal->mb.GetHash());
        BOOST_CHECK(decoded.credentials == proposal->credentials);
        BOOST_CHECK(flowmesh::VerifyProposal(decoded, MESH_DOMAIN));

        DataStream bad;
        bad << proposal->mb << proposal->round << proposal->proposer
            << proposal->proposer_sig;
        WriteCompactSize(bad, flowmesh::MAX_MICROBLOCK_ACTIONS + 1);
        ProposalMsg refuse;
        BOOST_CHECK_THROW(bad >> refuse, std::ios_base::failure);
    }

    // Halted-node handlers are inert across the board.
    {
        MeshNet fresh{3, 0};
        fresh.RunRound({fresh.Deposit(Outpoint(0x0a, 0))});
        const size_t p{fresh.ProposerIndex(1, 0)};
        fresh.sinks[p]->fail = true;
        // Drive node p to a PERSIST_FAILED halt via a certified entry.
        const size_t other{(p + 1) % 3};
        BOOST_REQUIRE(fresh.nodes[other]->SubmitAction(
            fresh.SignedOrder(fresh.alice_key, 0, true, 40'000, 2)));
        const auto proposal{fresh.nodes[p]->TryPropose()};
        BOOST_REQUIRE(proposal.has_value());
        std::vector<AttestationMsg> atts;
        for (auto& node : fresh.nodes) {
            if (const auto att{node->HandleProposal(*proposal)}) atts.push_back(*att);
        }
        std::optional<CertifiedEntry> entry;
        for (const AttestationMsg& att : atts) {
            for (auto& node : fresh.nodes) {
                if (const auto e{node->HandleAttestation(att)}) {
                    if (!entry) entry = e;
                }
            }
        }
        BOOST_REQUIRE(entry.has_value());
        BOOST_REQUIRE(!fresh.nodes[p]->HandleCertified(*entry));
        BOOST_REQUIRE(fresh.nodes[p]->Halted());
        const uint256 root_at_halt{fresh.nodes[p]->State().Root()};
        BOOST_CHECK(!fresh.nodes[p]->HandleProposal(*proposal).has_value());
        for (const AttestationMsg& att : atts) {
            BOOST_CHECK(!fresh.nodes[p]->HandleAttestation(att).has_value());
        }
        BOOST_CHECK(!fresh.nodes[p]->HandleCertified(*entry));
        fresh.nodes[p]->NoteTimeout(); // no effect while halted
        BOOST_CHECK_EQUAL(fresh.nodes[p]->CurrentRound(), 0U);
        BOOST_CHECK(fresh.nodes[p]->HandleCatchupRequest(flowmesh::CatchupRequest{0}).empty());
        BOOST_CHECK_EQUAL(fresh.nodes[p]->State().Root().GetHex(), root_at_halt.GetHex());
    }
}

BOOST_AUTO_TEST_CASE(codex_followup_boundaries)
{
    MeshNet net{3, 0};
    std::string error;

    // 1) Signing nodes are neither copyable nor movable: a stale clone
    // could otherwise sign a conflicting candidate after the original
    // cleared its durable lock.
    static_assert(!std::is_copy_constructible_v<MeshNode>);
    static_assert(!std::is_copy_assignable_v<MeshNode>);
    static_assert(!std::is_move_constructible_v<MeshNode>);
    static_assert(!std::is_move_assignable_v<MeshNode>);

    // 3a) A malformed SHORT 'e'-prefixed key fails the strict open-time
    // namespace validation.
    {
        const fs::path path{m_args.GetDataDirBase() / "flowmesh_shorte"};
        {
            node::FlowMeshStore store{
                DBParams{.path = path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
            BOOST_REQUIRE(store.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
        }
        {
            CDBWrapper raw{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
            raw.Write(uint8_t{'e'}, uint8_t{0x01}, true); // one-byte key: malformed
        }
        node::FlowMeshStore reopened{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
        BOOST_CHECK(!reopened.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
    }

    // 3b) The idempotent WriteLock path no longer succeeds over a
    // corrupt journal: EVERY lock key AND value is decoded before any
    // CAS decision.
    {
        const fs::path path{m_args.GetDataDirBase() / "flowmesh_idemcorrupt"};
        {
            node::FlowMeshStore store{
                DBParams{.path = path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
            BOOST_REQUIRE(store.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
            BOOST_REQUIRE(store.WriteLock(0, uint256::ONE));
        }
        {
            CDBWrapper raw{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
            raw.Write(std::make_pair(uint8_t{'l'}, uint64_t{5}), uint8_t{0x42}, true);
        }
        node::FlowMeshStore reopened{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
        BOOST_CHECK(!reopened.WriteLock(0, uint256::ONE)); // idempotent rewrite refused
    }

    // 3c) The journal bound is a REAL hard limit: zero and huge
    // configured values are refused at construction.
    {
        const fs::path path{m_args.GetDataDirBase() / "flowmesh_lockbound"};
        BOOST_CHECK_THROW((node::FlowMeshStore{DBParams{.path = path,
                                                        .cache_bytes = size_t{1} << 20,
                                                        .wipe_data = true},
                                               /*max_lock_entries=*/0}),
                          std::invalid_argument);
        BOOST_CHECK_THROW((node::FlowMeshStore{DBParams{.path = path,
                                                        .cache_bytes = size_t{1} << 20,
                                                        .wipe_data = true},
                                               /*max_lock_entries=*/5000}),
                          std::invalid_argument);
    }

    // 4) Unreachable decoded accounting: a retained EXHAUSTED curve and
    // an ASK whose reservation is not the exact residual both throw.
    {
        const auto craft_ask{[&](const CAmount filled, const CAmount reserved,
                                 const CAmount ledger_reserved) {
            flowmesh::Ledger ledger{VAULT};
            BOOST_REQUIRE(ledger.Deposit(net.alice, BaseX(), 1'000));
            if (ledger_reserved > 0) {
                BOOST_REQUIRE(ledger.Reserve(net.alice, BaseX(), ledger_reserved));
            }
            DataStream stream;
            stream << ledger;
            stream << BaseX() << Quote() << uint64_t{8};
            WriteCompactSize(stream, 1);
            stream << uint8_t{1} << net.alice; // Side::ASK
            WriteCompactSize(stream, 2);
            stream << flowmesh::ClearingEngine::Breakpoint{9, 0}
                   << flowmesh::ClearingEngine::Breakpoint{10, 10};
            stream << filled << reserved;
            WriteCompactSize(stream, 0);
            WriteCompactSize(stream, 0);
            FlowMeshState target{VAULT, BaseX(), Quote()};
            stream >> target;
        }};
        BOOST_CHECK_NO_THROW(craft_ask(4, 6, 6));                       // exact residual
        BOOST_CHECK_THROW(craft_ask(0, 5, 5), std::ios_base::failure);  // needs 10, reserves 5
        BOOST_CHECK_THROW(craft_ask(10, 0, 0), std::ios_base::failure); // exhausted, retained
    }

    // 5) Committed-anchor recheck runs BEFORE execution/cache: with the
    // committed history orphaned, a proposal carrying a perfectly
    // acceptable NEW anchor is refused (and the node halts) before any
    // candidate work — observers included.
    {
        MeshNet fresh{3, 0};
        const AnchorRef old_anchor{fresh.anchors.current};
        fresh.RunRound({fresh.Deposit(Outpoint(0x0a, 0))}); // history at old_anchor
        // The chain reorganizes: a NEW anchor becomes current and fully
        // acceptable; the committed one is orphaned.
        fresh.anchors.current =
            AnchorRef{8, uint256{"00000000000000000000000000000000000000000000000000000000000000ab"}};
        fresh.anchors.orphaned_hash = old_anchor.hash;

        // Craft a valid round-0 proposal for sequence 1 at the NEW anchor.
        const size_t p{fresh.ProposerIndex(1, 0)};
        FlowMeshState next{fresh.nodes[p]->State()};
        flowmesh::BatchResult result;
        std::vector<std::vector<unsigned char>> credentials;
        const auto mb{flowmesh::BuildMicroblock(fresh.nodes[p]->State(), MESH_DOMAIN,
                                                fresh.nodes[p]->LastHash(),
                                                fresh.anchors.current, {}, &fresh.deposits,
                                                next, result, credentials)};
        BOOST_REQUIRE(mb.has_value());
        ProposalMsg proposal;
        proposal.mb = *mb;
        proposal.round = 0;
        proposal.credentials = credentials;
        BOOST_REQUIRE(flowmesh::SignProposal(fresh.keys[p], MESH_DOMAIN, proposal));

        const size_t seat{(p + 1) % 3};
        BOOST_CHECK(!fresh.nodes[seat]->HandleProposal(proposal).has_value());
        BOOST_CHECK(fresh.nodes[seat]->Halted());
        BOOST_CHECK(fresh.nodes[seat]->Halt() == MeshHalt::ANCHOR_INVALIDATED);
        BOOST_CHECK(!fresh.observer->HandleProposal(proposal).has_value());
        BOOST_CHECK(fresh.observer->Halted()); // observers recheck too
    }

    // 6) An invalid startup must not mutate a fresh store: a
    // wrong-binding authenticator fails BEFORE the domain marker is
    // written.
    {
        const fs::path path{m_args.GetDataDirBase() / "flowmesh_freshmarker"};
        node::FlowMeshStore store{
            DBParams{.path = path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
        const flowmesh::SchnorrActionAuthenticator wrong{MESH_DOMAIN, uint256::ONE};
        MeshNode::Config config;
        config.domain = MESH_DOMAIN;
        config.seats = net.seats;
        config.threshold = net.threshold;
        config.schedule = net.schedule.get();
        config.auth = &wrong;
        config.anchors = &net.anchors;
        config.seat_key = net.keys[0];
        node::ValidatorRuntime runtime;
        BOOST_CHECK(!node::StartValidator(store, config, VAULT, BaseX(), Quote(), 8, runtime,
                                          error));
        std::optional<node::FlowMeshStore::Marker> marker;
        BOOST_REQUIRE(store.ReadMarker(marker, error));
        BOOST_CHECK(!marker.has_value()); // still a fresh, unmutated store
    }
}

BOOST_AUTO_TEST_CASE(codex_final_blockers)
{
    MeshNet net{3, 0};
    std::string error;

    // Blocker 2: anchors are rechecked immediately before EVERY
    // signature. Proposal signing: burial lost between eligibility and
    // signing -> TryPropose refuses (no halt; canonical anchor).
    {
        const size_t p{net.ProposerIndex(0, 0)};
        net.anchors.sufficiently_buried = false;
        BOOST_CHECK(!net.nodes[p]->TryPropose().has_value());
        BOOST_CHECK(!net.nodes[p]->Halted());
        net.anchors.sufficiently_buried = true;
        BOOST_CHECK(net.nodes[p]->TryPropose().has_value()); // buried again: proposes
    }

    // Blocker 3: startup is store-neutral on EVERY failure path — a
    // fresh store that fails at the role claim (all validation passed)
    // still carries no marker; only a fully successful startup writes.
    {
        const fs::path path{m_args.GetDataDirBase() / "flowmesh_neutral"};
        node::FlowMeshStore store{
            DBParams{.path = path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
        BOOST_REQUIRE(store.ClaimValidatorRole()); // pre-claim: startup must fail late
        MeshNode::Config config;
        config.domain = MESH_DOMAIN;
        config.seats = net.seats;
        config.threshold = net.threshold;
        config.schedule = net.schedule.get();
        config.auth = &net.auth;
        config.deposits = &net.deposits;
        config.anchors = &net.anchors;
        config.seat_key = net.keys[0];
        node::ValidatorRuntime runtime;
        BOOST_CHECK(!node::StartValidator(store, config, VAULT, BaseX(), Quote(), 8, runtime,
                                          error));
        std::optional<node::FlowMeshStore::Marker> marker;
        BOOST_REQUIRE(store.ReadMarker(marker, error));
        BOOST_CHECK(!marker.has_value()); // fresh store byte-identical after the failure
        // Releasing the stale claim lets a correct startup succeed and
        // perform the single marker write.
        store.ReleaseValidatorRole();
        BOOST_REQUIRE_MESSAGE(node::StartValidator(store, config, VAULT, BaseX(), Quote(), 8,
                                                   runtime, error),
                              error);
        BOOST_REQUIRE(store.ReadMarker(marker, error));
        BOOST_CHECK(marker.has_value());
    }
}


//! Owner ruling 2026-08-22 (certificate finality): a valid threshold
//! certificate finalizes a sequence; a SECOND valid certificate over a
//! DIFFERENT microblock for an already-committed sequence is recorded as
//! evidence and fail-safe halts the node — never silently ignored. A
//! duplicate of the committed entry stays harmless; an invalid conflicting
//! certificate is junk, not evidence.
BOOST_AUTO_TEST_CASE(conflicting_valid_certificate_is_evidence_and_halts)
{
    MeshNet net{3, /*f=*/0};
    const CertifiedEntry e0{net.RunRound({})};
    const uint64_t s{e0.mb.sequence};
    for (auto& node : net.nodes) BOOST_CHECK(!node->Halted());

    // A duplicate of the committed entry: harmless, still not halted.
    BOOST_CHECK(net.nodes[0]->HandleCertified(e0));
    BOOST_CHECK(!net.nodes[0]->Halted());
    BOOST_CHECK(net.nodes[0]->ConflictEvidence().empty());

    // A different microblock at the same sequence, certified by the full
    // seat set: a genuine conflict.
    CertifiedEntry conflict{e0};
    conflict.mb.result_commitment = uint256::ONE; // any content difference
    const uint256 other_hash{conflict.mb.GetHash()};
    BOOST_REQUIRE(other_hash != e0.mb.GetHash());
    std::vector<flowmesh::Attestation> atts;
    for (const CKey& key : net.keys) {
        const auto a{flowmesh::SignAttestation(key, MESH_DOMAIN, s, other_hash)};
        BOOST_REQUIRE(a.has_value());
        atts.push_back(*a);
    }
    conflict.cert = flowmesh::AssembleCertificate(other_hash, s, atts);

    // Same conflicting bytes but with a broken certificate: junk, ignored.
    CertifiedEntry junk{conflict};
    junk.cert.attestations.clear();
    BOOST_CHECK(!net.nodes[1]->HandleCertified(junk));
    BOOST_CHECK(!net.nodes[1]->Halted());
    BOOST_CHECK(net.nodes[1]->ConflictEvidence().empty());

    // The valid conflict: evidence recorded, fail-safe halt.
    BOOST_CHECK(!net.nodes[0]->HandleCertified(conflict));
    BOOST_CHECK(net.nodes[0]->Halted());
    BOOST_REQUIRE_EQUAL(net.nodes[0]->ConflictEvidence().size(), 1U);
    BOOST_CHECK_EQUAL(net.nodes[0]->ConflictEvidence()[0].sequence, s);
    BOOST_CHECK(net.nodes[0]->ConflictEvidence()[0].committed_hash == e0.mb.GetHash());
    BOOST_CHECK(net.nodes[0]->ConflictEvidence()[0].conflicting.microblock_hash == other_hash);
    // Halted nodes refuse further work.
    BOOST_CHECK(!net.nodes[0]->HandleCertified(e0));
}

BOOST_AUTO_TEST_SUITE_END()
