// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! FlowMesh certified-log node layer: multi-node propose/attest/certify
//! convergence with signed proposer envelopes, durable-before-live
//! commit ordering, write-ahead lock journaling with restart safety,
//! replacement-proposer recovery of a locked candidate, catch-up,
//! anchor revalidation of certified history under B3 reorgs, snapshot
//! revalidation, the withdrawal request lifecycle, and FlowMesh-outage
//! isolation from B3 block production.

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

#include <map>
#include <memory>
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

//! Ephemeral in-memory journal: provides the WRITE-AHEAD interface for
//! message-flow tests. Durability itself is exercised by the
//! store-backed journal in the restart tests below.
class MemJournal final : public flowmesh::LockJournal
{
public:
    bool fail{false};
    std::map<uint64_t, uint256> locks;

    bool WriteLock(const uint64_t sequence, const uint256& hash) override
    {
        if (fail) return false;
        locks[sequence] = hash;
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

//! A commit sink that can be told to fail: the durable append refused.
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
    flowmesh::SchnorrActionAuthenticator auth{MESH_DOMAIN};
    FixedAnchors anchors;
    MapDeposits deposits;
    CKey alice_key{MakeKey(0xa1)};
    CKey bob_key{MakeKey(0xb2)};
    flowmesh::AccountId alice{flowmesh::AccountForKey(Xonly(alice_key))};
    flowmesh::AccountId bob{flowmesh::AccountForKey(Xonly(bob_key))};
    std::vector<std::unique_ptr<MemJournal>> journals;
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
            nodes.push_back(MakeNode(keys[i], nullptr, journals.back().get()));
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
        return std::make_unique<MeshNode>(
            std::move(config),
            state ? std::move(*state) : FlowMeshState{VAULT, BaseX(), Quote()}, last_hash,
            locks);
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
        BOOST_REQUIRE(flowmesh::SignAction(key, MESH_DOMAIN, a));
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

    // Microblock 1: a withdrawal intent — which creates a REQUEST, never
    // anything redeemable (lifecycle: REQUESTED, then derived
    // MICROBLOCK_CERTIFIED once this entry certifies; the B3_FINAL /
    // REDEEMABLE stage is gated on the unresolved owner decision).
    Action withdraw;
    withdraw.signer = net.alice;
    withdraw.sequence = 1;
    withdraw.type = static_cast<uint8_t>(ActionType::WITHDRAW);
    withdraw.asset = BaseX();
    withdraw.amount = 10;
    withdraw.destination =
        uint256{"00000000000000000000000000000000000000000000000000000000000000d1"};
    BOOST_REQUIRE(flowmesh::SignAction(net.alice_key, MESH_DOMAIN, withdraw));
    const CertifiedEntry e1{net.RunRound({withdraw})};
    BOOST_CHECK_EQUAL(e1.mb.sequence, 1U);
    BOOST_CHECK(e1.mb.parent_hash == e0.mb.GetHash());
    // The certified state holds exactly one pending withdrawal REQUEST.
    BOOST_REQUIRE_EQUAL(e1.mb.actions.size(), 1U);

    const uint256 root{net.nodes[0]->State().Root()};
    for (auto& node : net.nodes) {
        BOOST_CHECK_EQUAL(node->Sequence(), 2U);
        BOOST_CHECK_EQUAL(node->State().Root().GetHex(), root.GetHex());
        BOOST_CHECK(node->Evidence().empty());
        BOOST_CHECK(!node->Halted());
        BOOST_CHECK(node->State().ledger.SolvencyHolds());
    }
    BOOST_CHECK_EQUAL(net.observer->State().Root().GetHex(), root.GetHex());

    BOOST_CHECK_EQUAL(net.nodes[0]->State().ledger.Available(net.bob, Quote()), 500'000);
    BOOST_CHECK_EQUAL(net.nodes[0]->State().ledger.Available(net.alice, BaseX()), 0);
}

BOOST_AUTO_TEST_CASE(commit_is_durable_before_live_and_storage_failure_halts)
{
    // Codex defect 8 regression: a certified transition must not become
    // live state when its durable record failed; the node fail-stops.
    MeshNet net{3, 0};
    FailableSink sink;
    MemJournal journal;
    net.nodes[0] = net.MakeNode(net.keys[0], &sink, &journal);

    net.RunRound({net.Deposit(Outpoint(0x0a, 0))});
    BOOST_REQUIRE_EQUAL(sink.committed.size(), 1U); // durable first, then live
    BOOST_CHECK_EQUAL(net.nodes[0]->Sequence(), 1U);

    // Storage starts failing: the next certified entry must NOT advance
    // node 0's live tip, and the node halts instead of continuing.
    sink.fail = true;
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
    BOOST_REQUIRE(entry.has_value()); // a sinkless node still certified
    BOOST_CHECK(!net.nodes[0]->HandleCertified(*entry));
    BOOST_CHECK_EQUAL(net.nodes[0]->Sequence(), 1U); // live tip did NOT advance
    BOOST_CHECK(net.nodes[0]->Halted());
    BOOST_CHECK(net.nodes[0]->Halt() == MeshHalt::PERSIST_FAILED);
    BOOST_CHECK(!net.nodes[0]->TryPropose().has_value());
    BOOST_CHECK(!net.nodes[0]->SubmitAction(net.SignedOrder(net.bob_key, 0, false, 1, 1)));
}

BOOST_AUTO_TEST_CASE(lock_journal_failure_prevents_signing)
{
    // Codex defect 7 regression (durable safety): if the write-ahead
    // lock cannot be journaled, the validator must NOT sign.
    MeshNet net{3, 0};
    const size_t p{net.ProposerIndex(0, 0)};
    const auto proposal{net.nodes[p]->TryPropose()};
    BOOST_REQUIRE(proposal.has_value());

    const size_t other{(p + 1) % 3};
    net.journals[other]->fail = true;
    BOOST_CHECK(!net.nodes[other]->HandleProposal(*proposal).has_value());
    BOOST_CHECK(net.nodes[other]->Halted());
    BOOST_CHECK(net.nodes[other]->Halt() == MeshHalt::LOCK_JOURNAL_FAILED);

    // A seat configured WITHOUT any journal is an invalid config: it
    // never participates at all.
    auto no_journal{net.MakeNode(net.keys[p], nullptr, /*journal=*/nullptr)};
    BOOST_CHECK(no_journal->Halted());
    BOOST_CHECK(no_journal->Halt() == MeshHalt::INVALID_CONFIG);
    // Threshold zero is likewise refused up front.
    {
        MeshNode::Config config;
        config.domain = MESH_DOMAIN;
        config.seats = net.seats;
        config.threshold = 0;
        config.schedule = net.schedule.get();
        config.auth = &net.auth;
        config.anchors = &net.anchors;
        const MeshNode invalid{std::move(config), FlowMeshState{VAULT, BaseX(), Quote()}};
        BOOST_CHECK(invalid.Halt() == MeshHalt::INVALID_CONFIG);
    }
}

BOOST_AUTO_TEST_CASE(replacement_proposer_continues_a_locked_candidate)
{
    // Codex defect 7 regression (replacement proposal): proposer
    // identity lives in the ENVELOPE, not the candidate hash, so a
    // later round's proposer re-proposes the SAME locked candidate and
    // locked validators re-attest it — recovery cannot deadlock merely
    // because authorship changed.
    MeshNet net{3, 0}; // t = 2
    const size_t p0{net.ProposerIndex(0, 0)};
    BOOST_REQUIRE(net.nodes[p0]->SubmitAction(net.Deposit(Outpoint(0x0a, 0))));
    const auto proposal_a{net.nodes[p0]->TryPropose()};
    BOOST_REQUIRE(proposal_a.has_value());

    // Round 0: only seat p1 sees the proposal and attests (1 < t —
    // stalled); p1 is now locked on candidate A and holds it.
    const size_t p1{net.ProposerIndex(0, 1)};
    const auto att_p1{net.nodes[p1]->HandleProposal(*proposal_a)};
    BOOST_REQUIRE(att_p1.has_value());

    // Everyone times into round 1, whose scheduled proposer is p1: it
    // re-proposes the SAME candidate hash under its own envelope.
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

    // Deterministic reconstruction, with anchors revalidated.
    {
        FlowMeshState state{VAULT, BaseX(), Quote()};
        uint256 last_hash;
        BOOST_REQUIRE_MESSAGE(store->Replay(state, last_hash, net.auth, &net.deposits,
                                            net.seats, net.threshold, &net.anchors, error),
                              error);
        BOOST_CHECK_EQUAL(state.Root().GetHex(), live_root.GetHex());
        BOOST_CHECK_EQUAL(last_hash.GetHex(), live_hash.GetHex());
    }
    // Replay under a different quorum fails closed (defect 9: history is
    // verified with the quorum it was certified under, recorded in the
    // marker; re-judging under another is the owner's seat-lifecycle
    // decision, not an implicit behavior).
    {
        FlowMeshState state{VAULT, BaseX(), Quote()};
        uint256 last_hash;
        BOOST_CHECK(!store->Replay(state, last_hash, net.auth, &net.deposits,
                                   std::set<XOnlyPubKey>{Xonly(net.keys[0])}, 1, nullptr,
                                   error));
    }
    // Replay refuses history whose anchors left the canonical chain
    // (defect 10: certified history cannot silently remain accepted).
    {
        net.anchors.still_canonical = false;
        FlowMeshState state{VAULT, BaseX(), Quote()};
        uint256 last_hash;
        BOOST_CHECK(!store->Replay(state, last_hash, net.auth, &net.deposits, net.seats,
                                   net.threshold, &net.anchors, error));
        net.anchors.still_canonical = true;
    }

    // Certificate-verified snapshot accelerates reconstruction; a wrong
    // state is refused at write time.
    BOOST_REQUIRE_MESSAGE(store->WriteSnapshot(1, mid_state, error), error);
    BOOST_CHECK(!store->WriteSnapshot(2, mid_state, error));
    {
        FlowMeshState state{VAULT, BaseX(), Quote()};
        uint256 last_hash;
        BOOST_REQUIRE_MESSAGE(store->ReplayFromBestSnapshot(state, last_hash, net.auth,
                                                            &net.deposits, net.seats,
                                                            net.threshold, &net.anchors, error),
                              error);
        BOOST_CHECK_EQUAL(state.Root().GetHex(), live_root.GetHex());
        BOOST_CHECK_EQUAL(last_hash.GetHex(), live_hash.GetHex());
    }
    // Snapshots cannot bypass anchor validity: with the snapshot-tip
    // anchor off-chain, reconstruction refuses rather than trusting the
    // snapshot (defect 10 / snapshot revalidation).
    {
        net.anchors.still_canonical = false;
        FlowMeshState state{VAULT, BaseX(), Quote()};
        uint256 last_hash;
        BOOST_CHECK(!store->ReplayFromBestSnapshot(state, last_hash, net.auth, &net.deposits,
                                                   net.seats, net.threshold, &net.anchors,
                                                   error));
        net.anchors.still_canonical = true;
    }

    // "Restart": close everything, reopen, reconstruct, and serve
    // catch-up from the durable log.
    net.nodes[0] = net.MakeNode(net.keys[0]); // detach sink+journal
    store.reset();
    {
        node::FlowMeshStore reopened{
            DBParams{.path = store_path, .cache_bytes = size_t{1} << 20}};
        BOOST_REQUIRE(reopened.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
        FlowMeshState state{VAULT, BaseX(), Quote()};
        uint256 last_hash;
        BOOST_REQUIRE_MESSAGE(reopened.ReplayFromBestSnapshot(state, last_hash, net.auth,
                                                              &net.deposits, net.seats,
                                                              net.threshold, &net.anchors,
                                                              error),
                              error);
        BOOST_CHECK_EQUAL(state.Root().GetHex(), live_root.GetHex());

        // Log discipline: re-appending an old entry and opening under a
        // foreign domain or quorum both fail closed.
        const auto e0{reopened.ReadEntry(0)};
        BOOST_REQUIRE(e0.has_value());
        BOOST_CHECK(!reopened.Append(*e0, error));
        BOOST_CHECK(!reopened.OpenForDomain(uint256::ONE, net.seats, net.threshold, error));
        BOOST_CHECK(!reopened.OpenForDomain(MESH_DOMAIN, net.seats, net.threshold - 1, error));

        // A restored node serves history it did not commit in this
        // process lifetime, via the durable catch-up source.
        node::StoreCatchupSource source{reopened};
        auto restored{net.MakeNode(std::nullopt, nullptr, nullptr, &source, last_hash,
                                   /*locks=*/{}, state)};
        BOOST_CHECK_EQUAL(restored->Sequence(), 2U);
        const auto served{restored->HandleCatchupRequest(flowmesh::CatchupRequest{0})};
        BOOST_REQUIRE_EQUAL(served.size(), 2U);
        auto late{net.MakeNode(std::nullopt)};
        BOOST_CHECK_EQUAL(late->HandleCatchupResponse(served), 2U);
        BOOST_CHECK_EQUAL(late->State().Root().GetHex(), live_root.GetHex());
    }
}

BOOST_AUTO_TEST_CASE(restart_cannot_sign_a_conflicting_candidate)
{
    // Codex defect 7/9 regression: the journaled lock survives restart,
    // so the restarted validator refuses to sign a DIFFERENT candidate
    // at its protected sequence — even though its in-memory guard is
    // fresh.
    const fs::path store_path{m_args.GetDataDirBase() / "flowmesh_locks"};
    MeshNet net{3, 0};
    auto store{std::make_unique<node::FlowMeshStore>(
        DBParams{.path = store_path, .cache_bytes = size_t{1} << 20, .wipe_data = true})};
    std::string error;
    BOOST_REQUIRE(store->OpenForDomain(MESH_DOMAIN, net.seats, net.threshold, error));
    node::StoreLockJournal journal{*store};
    const size_t p0{net.ProposerIndex(0, 0)};
    const size_t voter{(p0 + 1) % 3};
    net.nodes[voter] = net.MakeNode(net.keys[voter], nullptr, &journal);

    // The voter attests candidate A (lock journaled write-ahead).
    BOOST_REQUIRE(net.nodes[p0]->SubmitAction(net.Deposit(Outpoint(0x0a, 0))));
    const auto proposal_a{net.nodes[p0]->TryPropose()};
    BOOST_REQUIRE(proposal_a.has_value());
    BOOST_REQUIRE(net.nodes[voter]->HandleProposal(*proposal_a).has_value());

    // Crash + restart the voter: fresh node, locks restored from disk.
    std::map<uint64_t, uint256> locks;
    BOOST_REQUIRE(store->ReadLocks(locks, error));
    BOOST_REQUIRE_EQUAL(locks.size(), 1U);
    BOOST_CHECK(locks.at(0) == proposal_a->mb.GetHash());
    net.nodes[voter] = net.MakeNode(net.keys[voter], nullptr, &journal, nullptr, uint256{},
                                    locks);

    // A conflicting candidate at the same sequence/round must be
    // refused by the restarted voter...
    auto fresh_p0{net.MakeNode(net.keys[p0], nullptr, net.journals[p0].get())};
    const auto conflicting{fresh_p0->TryPropose()}; // empty pool: different candidate
    BOOST_REQUIRE(conflicting.has_value());
    BOOST_REQUIRE(conflicting->mb.GetHash() != proposal_a->mb.GetHash());
    BOOST_CHECK(!net.nodes[voter]->HandleProposal(*conflicting).has_value());
    // ...while re-attesting the locked candidate A remains possible.
    BOOST_CHECK(net.nodes[voter]->HandleProposal(*proposal_a).has_value());
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

    // Other seats time into round 1; p1 proposes a DIFFERENT candidate
    // (its own pool is empty). p0 stays locked on A and refuses B, but B
    // reaches t without it.
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
    // The Byzantine proposer signs a second, different candidate too.
    MemJournal shadow_journal;
    auto shadow{net.MakeNode(net.keys[p0], nullptr, &shadow_journal)};
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

BOOST_AUTO_TEST_CASE(anchors_gate_proposals_and_committed_history)
{
    // Proposals anchored elsewhere are refused before execution.
    MeshNet net{3, 0};
    const size_t p0{net.ProposerIndex(0, 0)};
    const auto proposal{net.nodes[p0]->TryPropose()};
    BOOST_REQUIRE(proposal.has_value());
    ProposalMsg bad{*proposal};
    bad.mb.anchor = AnchorRef{9, uint256::ONE};
    for (auto& node : net.nodes) BOOST_CHECK(!node->HandleProposal(bad).has_value());

    // Codex defect 10 regression: an anchor relied on by COMMITTED
    // history that stops being canonical halts FlowMesh progression
    // (fail-safe; deep-reorg treatment is an owner decision).
    net.RunRound({net.Deposit(Outpoint(0x0a, 0))});
    net.anchors.still_canonical = false;
    BOOST_CHECK(!net.nodes[0]->RecheckCommittedAnchors());
    BOOST_CHECK(net.nodes[0]->Halted());
    BOOST_CHECK(net.nodes[0]->Halt() == MeshHalt::ANCHOR_INVALIDATED);
    BOOST_CHECK(!net.nodes[0]->TryPropose().has_value());
    // A certified entry whose anchor is off-chain is refused by a
    // still-running node (catch-up revalidates anchors).
    const size_t p_next{net.ProposerIndex(1, 0)};
    net.anchors.still_canonical = true;
    const auto next_proposal{net.nodes[p_next]->TryPropose()};
    BOOST_REQUIRE(next_proposal.has_value());
    std::vector<AttestationMsg> atts;
    for (auto& node : net.nodes) {
        if (const auto att{node->HandleProposal(*next_proposal)}) atts.push_back(*att);
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
    net.anchors.still_canonical = false;
    BOOST_CHECK(!net.observer->HandleCertified(*entry));
    net.anchors.still_canonical = true;

    // Chain-backed policy over the regtest fixture chain.
    const node::ChainAnchorPolicy policy{*Assert(m_node.chainman), /*min_depth=*/6};
    const int tip_height{m_node.chainman->ActiveChain().Height()};
    const AnchorRef current{policy.Current()};
    BOOST_CHECK_EQUAL(current.height, tip_height - 6);
    BOOST_CHECK(policy.Acceptable(current));
    BOOST_CHECK(policy.StillCanonical(current));
    const AnchorRef tip{tip_height, m_node.chainman->ActiveChain().Tip()->GetBlockHash()};
    BOOST_CHECK(!policy.Acceptable(tip));    // not buried deep enough for proposals
    BOOST_CHECK(policy.StillCanonical(tip)); // but certainly canonical
    BOOST_CHECK(!policy.Acceptable(AnchorRef{current.height, uint256::ONE}));
    BOOST_CHECK(!policy.StillCanonical(AnchorRef{current.height, uint256::ONE}));
    BOOST_CHECK(!policy.StillCanonical(AnchorRef{current.height - 1, current.hash}));
    BOOST_CHECK(policy.StillCanonical(AnchorRef{})); // null anchor: no B3 reliance
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
    BOOST_CHECK(!policy.StillCanonical(before)); // committed history using it must halt
    const AnchorRef after{policy.Current()};
    BOOST_CHECK(after.height < before.height);
    BOOST_CHECK(policy.Acceptable(after));
}

BOOST_AUTO_TEST_CASE(pool_admission_is_atomic)
{
    // Codex defect 12 regression: Add() can never report success (or
    // fail) while leaving an unreachable byte-counted entry behind — a
    // per-(signer, sequence) conflict is refused BEFORE any accounting.
    MeshNet net{3, 0};
    flowmesh::ActionPool pool;
    const Action first{net.SignedOrder(net.alice_key, 0, true, 40'000, 2)};
    Action conflicting{net.SignedOrder(net.alice_key, 0, true, 41'000, 3)}; // same (signer, seq)
    BOOST_REQUIRE(pool.Add(first));
    const size_t size_after{pool.Size()};
    const size_t bytes_after{pool.Bytes()};
    BOOST_CHECK(!pool.Add(conflicting));
    BOOST_CHECK_EQUAL(pool.Size(), size_after);
    BOOST_CHECK_EQUAL(pool.Bytes(), bytes_after); // no orphaned byte accounting
    BOOST_CHECK(!pool.Add(first));                // exact duplicate refused
    BOOST_CHECK_EQUAL(pool.Bytes(), bytes_after);
    // The first-admitted intent is what a proposer selects.
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
    BOOST_CHECK(net.nodes[p0]->State().ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_SUITE_END()
