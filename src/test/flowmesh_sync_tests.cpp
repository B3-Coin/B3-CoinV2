// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! FlowMesh certified-log node layer: multi-node propose/attest/certify
//! convergence, durable log append + deterministic replay (restart),
//! catch-up for lagging nodes, vote-split recovery through rounds with
//! the lock rule, proposer equivocation contained by the fault-model
//! threshold, chain-backed anchor gating, and FlowMesh-outage isolation
//! from B3 block production.

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

    bool Acceptable(const AnchorRef& anchor) const override
    {
        return accept_all && anchor == current;
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

//! A small FN network of MeshNodes with shared seats/threshold/schedule
//! and a manual message plane, so tests control delivery exactly.
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

        for (size_t i{0}; i < n_seats; ++i) nodes.push_back(MakeNode(keys[i]));
        observer = MakeNode(std::nullopt);
    }

    std::unique_ptr<MeshNode> MakeNode(std::optional<CKey> seat_key,
                                       flowmesh::CommitSink* sink = nullptr) const
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
        config.seat_key = std::move(seat_key);
        return std::make_unique<MeshNode>(std::move(config),
                                          FlowMeshState{VAULT, BaseX(), Quote()});
    }

    //! Index of the seat scheduled to propose (sequence, round).
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

    //! Full happy-path round: scheduled proposer proposes from its pool,
    //! everyone re-executes and attests, attestations broadcast until a
    //! certificate forms, and the certificate reaches every node.
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
        BOOST_REQUIRE(observer->HandleProposal(*proposal) == std::nullopt); // observers track only

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

    // Microblock 0: deposits plus a crossed pair -> a real clearing.
    const CertifiedEntry e0{net.RunRound(
        {net.Deposit(Outpoint(0x0a, 0)), net.Deposit(Outpoint(0x0b, 1)),
         net.SignedOrder(net.alice_key, 0, /*buy=*/true, 50'000, 10),
         net.SignedOrder(net.bob_key, 0, /*buy=*/false, 50'000, 10)})};
    BOOST_CHECK_EQUAL(e0.mb.sequence, 0U);

    // Microblock 1: a withdrawal intent.
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

    // Every node (and the keyless observer) landed on one state.
    const uint256 root{net.nodes[0]->State().Root()};
    for (auto& node : net.nodes) {
        BOOST_CHECK_EQUAL(node->Sequence(), 2U);
        BOOST_CHECK_EQUAL(node->State().Root().GetHex(), root.GetHex());
        BOOST_CHECK(node->Evidence().empty());
        BOOST_CHECK(node->State().ledger.SolvencyHolds());
    }
    BOOST_CHECK_EQUAL(net.observer->State().Root().GetHex(), root.GetHex());

    // The trade actually settled and the withdrawal receipt is pending.
    BOOST_CHECK_EQUAL(net.nodes[0]->State().ledger.Available(net.bob, Quote()), 500'000);
    BOOST_CHECK_EQUAL(net.nodes[0]->State().ledger.Available(net.alice, BaseX()), 0); // withdrawn
}

BOOST_AUTO_TEST_CASE(store_appends_replays_and_survives_restart)
{
    const fs::path store_path{m_args.GetDataDirBase() / "flowmesh_log"};
    MeshNet net{3, 0};

    // Seat 0 persists everything it commits.
    auto store{std::make_unique<node::FlowMeshStore>(
        DBParams{.path = store_path, .cache_bytes = size_t{1} << 20, .wipe_data = true})};
    std::string error;
    BOOST_REQUIRE_MESSAGE(store->OpenForDomain(MESH_DOMAIN, error), error);
    node::StoreCommitSink sink{*store};
    net.nodes[0] = net.MakeNode(net.keys[0], &sink);

    net.RunRound({net.Deposit(Outpoint(0x0a, 0)), net.Deposit(Outpoint(0x0b, 1)),
                  net.SignedOrder(net.alice_key, 0, true, 50'000, 10),
                  net.SignedOrder(net.bob_key, 0, false, 50'000, 10)});
    net.RunRound({net.SignedOrder(net.alice_key, 1, true, 40'000, 2)});
    BOOST_REQUIRE(!sink.LastError().has_value());

    const uint256 live_root{net.nodes[0]->State().Root()};
    const uint256 live_hash{net.nodes[0]->LastHash()};

    // Deterministic reconstruction from the same store handle, then
    // close it (the on-disk lock must clear before "restarting").
    {
        FlowMeshState state{VAULT, BaseX(), Quote()};
        uint256 last_hash;
        BOOST_REQUIRE_MESSAGE(store->Replay(state, last_hash, net.auth, &net.deposits, net.seats,
                                            net.threshold, error),
                              error);
        BOOST_CHECK_EQUAL(state.Root().GetHex(), live_root.GetHex());
        BOOST_CHECK_EQUAL(last_hash.GetHex(), live_hash.GetHex());
    }
    net.nodes[0] = net.MakeNode(net.keys[0]); // detach the sink before closing
    store.reset();

    // "Restart": a brand-new handle on the same on-disk log.
    {
        node::FlowMeshStore reopened{
            DBParams{.path = store_path, .cache_bytes = size_t{1} << 20}};
        BOOST_REQUIRE_MESSAGE(reopened.OpenForDomain(MESH_DOMAIN, error), error);
        FlowMeshState state{VAULT, BaseX(), Quote()};
        uint256 last_hash;
        BOOST_REQUIRE_MESSAGE(reopened.Replay(state, last_hash, net.auth, &net.deposits,
                                              net.seats, net.threshold, error),
                              error);
        BOOST_CHECK_EQUAL(state.Root().GetHex(), live_root.GetHex());

        // The log refuses forks and gaps: re-appending an old entry
        // fails closed; so does opening under a different domain.
        const auto e0{reopened.ReadEntry(0)};
        BOOST_REQUIRE(e0.has_value());
        BOOST_CHECK(!reopened.Append(*e0, error));
        BOOST_CHECK(!reopened.OpenForDomain(uint256::ONE, error));
    }

    // Replay under an insufficient seat set fails closed: certificates
    // must re-verify at rest, not just at commit time.
    {
        node::FlowMeshStore reopened{
            DBParams{.path = store_path, .cache_bytes = size_t{1} << 20}};
        BOOST_REQUIRE(reopened.OpenForDomain(MESH_DOMAIN, error));
        FlowMeshState state{VAULT, BaseX(), Quote()};
        uint256 last_hash;
        BOOST_CHECK(!reopened.Replay(state, last_hash, net.auth, &net.deposits,
                                     std::set<XOnlyPubKey>{}, net.threshold, error));
    }
}

BOOST_AUTO_TEST_CASE(lagging_node_catches_up_from_history)
{
    MeshNet net{3, 0};
    const CertifiedEntry e0{net.RunRound({net.Deposit(Outpoint(0x0a, 0))})};
    const CertifiedEntry e1{net.RunRound({net.SignedOrder(net.alice_key, 0, true, 40'000, 2)})};

    // A fresh observer hears only the newest certificate: it cannot use
    // it (missing parent), requests history, and replays to the tip.
    auto late{net.MakeNode(std::nullopt)};
    BOOST_CHECK(!late->HandleCertified(e1));
    const auto request{late->MaybeRequestCatchup(e1.mb.sequence)};
    BOOST_REQUIRE(request.has_value());
    BOOST_CHECK_EQUAL(request->from_sequence, 0U);
    const auto history{net.nodes[0]->HandleCatchupRequest(*request)};
    BOOST_REQUIRE_EQUAL(history.size(), 2U);
    BOOST_CHECK_EQUAL(late->HandleCatchupResponse(history), 2U);
    BOOST_CHECK_EQUAL(late->State().Root().GetHex(), net.nodes[0]->State().Root().GetHex());
    BOOST_CHECK(late->LastHash() == net.nodes[0]->LastHash());
}

BOOST_AUTO_TEST_CASE(vote_split_recovers_in_the_next_round_without_double_finality)
{
    MeshNet net{3, 0}; // t = 2

    // Round 0: the scheduled proposer's block A reaches ONLY the
    // proposer itself (1 attestation < t): a stalled round.
    const size_t p0{net.ProposerIndex(0, 0)};
    BOOST_REQUIRE(net.nodes[p0]->SubmitAction(net.Deposit(Outpoint(0x0a, 0))));
    const auto proposal_a{net.nodes[p0]->TryPropose()};
    BOOST_REQUIRE(proposal_a.has_value());
    const auto att_a{net.nodes[p0]->HandleProposal(*proposal_a)};
    BOOST_REQUIRE(att_a.has_value());
    for (auto& node : net.nodes) BOOST_CHECK(!node->HandleAttestation(*att_a).has_value());

    // The other seats time out into round 1; its proposer proposes B
    // (empty block). They attest B; the round-0 proposer stays locked on
    // A and refuses to attest B — but B still reaches t without it.
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
            BOOST_CHECK(!att.has_value()); // wrong round for p0, and locked on A
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

    // The locked proposer ACCEPTS the certified B: the lock disciplines
    // attesting, never certificate acceptance.
    BOOST_REQUIRE(net.nodes[p0]->HandleCertified(*entry));
    for (auto& node : net.nodes) {
        BOOST_CHECK_EQUAL(node->Sequence(), 1U);
        BOOST_CHECK(node->LastHash() == proposal_b->mb.GetHash());
        BOOST_CHECK(node->Evidence().empty()); // an honest split is not equivocation
    }
}

BOOST_AUTO_TEST_CASE(equivocating_proposer_cannot_double_certify)
{
    // k=4, f=1 -> t=3: any two certificates would share an honest seat.
    MeshNet net{4, 1};
    const size_t p0{net.ProposerIndex(0, 0)};

    // The Byzantine proposer builds two different round-0 blocks.
    BOOST_REQUIRE(net.nodes[p0]->SubmitAction(net.Deposit(Outpoint(0x0a, 0))));
    const auto proposal_a{net.nodes[p0]->TryPropose()};
    BOOST_REQUIRE(proposal_a.has_value());
    auto empty_node{net.MakeNode(net.keys[p0])}; // same seat, empty pool
    const auto proposal_b{empty_node->TryPropose()};
    BOOST_REQUIRE(proposal_b.has_value());
    BOOST_CHECK(proposal_a->mb.GetHash() != proposal_b->mb.GetHash());

    // Honest seats split: two see A first, one sees B first. The
    // equivocator double-signs both (forged second attestation).
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
    // Only A can certify: B gathered one honest seat plus a double-sign
    // that every node discards as equivocation evidence.
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

BOOST_AUTO_TEST_CASE(anchors_gate_proposals_and_follow_the_chain)
{
    // Mock policy: a proposal anchored anywhere else is refused outright.
    MeshNet net{3, 0};
    const size_t p0{net.ProposerIndex(0, 0)};
    const auto proposal{net.nodes[p0]->TryPropose()};
    BOOST_REQUIRE(proposal.has_value());
    ProposalMsg bad{*proposal};
    bad.mb.anchor = AnchorRef{9, uint256::ONE};
    // Tampering the anchor also breaks nothing else: it is simply not
    // acceptable, before any execution happens.
    for (auto& node : net.nodes) BOOST_CHECK(!node->HandleProposal(bad).has_value());

    // Real chain-backed policy over the regtest fixture chain.
    const node::ChainAnchorPolicy policy{*Assert(m_node.chainman), /*min_depth=*/6};
    const int tip_height{m_node.chainman->ActiveChain().Height()};
    const AnchorRef current{policy.Current()};
    BOOST_CHECK_EQUAL(current.height, tip_height - 6);
    BOOST_CHECK(policy.Acceptable(current));
    const AnchorRef tip{tip_height, m_node.chainman->ActiveChain().Tip()->GetBlockHash()};
    BOOST_CHECK(!policy.Acceptable(tip));          // not buried deep enough
    BOOST_CHECK(!policy.Acceptable(AnchorRef{current.height, uint256::ONE})); // unknown hash
    BOOST_CHECK(!policy.Acceptable(AnchorRef{current.height - 1, current.hash})); // wrong height
}

BOOST_AUTO_TEST_CASE(flowmesh_outage_never_stalls_b3)
{
    MeshNet net{3, 0};

    // Outage: the proposer proposes, but no other seat ever answers —
    // no certificate can form and FlowMesh simply stops progressing.
    const size_t p0{net.ProposerIndex(0, 0)};
    const auto proposal{net.nodes[p0]->TryPropose()};
    BOOST_REQUIRE(proposal.has_value());
    const auto own_att{net.nodes[p0]->HandleProposal(*proposal)};
    BOOST_REQUIRE(own_att.has_value());
    BOOST_CHECK(!net.nodes[p0]->HandleAttestation(*own_att).has_value());
    BOOST_CHECK_EQUAL(net.nodes[p0]->Sequence(), 0U);

    // B3 keeps producing and connecting blocks regardless.
    const int height_before{m_node.chainman->ActiveChain().Height()};
    CreateAndProcessBlock({}, CScript{} << OP_TRUE);
    BOOST_CHECK_EQUAL(m_node.chainman->ActiveChain().Height(), height_before + 1);
    BOOST_CHECK_EQUAL(net.nodes[p0]->Sequence(), 0U); // still stalled; funds untouched
    BOOST_CHECK(net.nodes[p0]->State().ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_CASE(b3_reorg_invalidates_previously_acceptable_anchors)
{
    // A base-chain reorganization deeper than the finality depth is the
    // one event that can strip acceptability from an anchored position —
    // exactly the residual risk the OD-6 depth prices. Deposits judged
    // at an anchor become unjudgeable when the anchor leaves the active
    // chain, and new proposals must move to the reorganized chain.
    const node::ChainAnchorPolicy policy{*Assert(m_node.chainman), /*min_depth=*/2};
    const AnchorRef before{policy.Current()};
    BOOST_REQUIRE(policy.Acceptable(before));

    // Invalidate the anchored block: everything from it up is
    // disconnected, so the anchor is no longer on the active chain.
    BlockValidationState state;
    CBlockIndex* anchored{WITH_LOCK(
        cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(before.hash))};
    BOOST_REQUIRE(anchored != nullptr);
    m_node.chainman->ActiveChainstate().InvalidateBlock(state, anchored);
    BOOST_REQUIRE(state.IsValid());

    BOOST_CHECK(!policy.Acceptable(before));
    const AnchorRef after{policy.Current()};
    BOOST_CHECK(after.height < before.height);
    BOOST_CHECK(policy.Acceptable(after));
}

BOOST_AUTO_TEST_SUITE_END()
