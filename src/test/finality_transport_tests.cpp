// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <addrman.h>
#include <crypto/common.h>
#include <net.h>
#include <net_processing.h>
#include <netgroup.h>
#include <node/finality_transport.h>
#include <protocol.h>
#include <streams.h>
#include <test/util/finality_fixture.h>
#include <test/util/net.h>
#include <test/util/time.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <span>

using namespace std::chrono_literals;

namespace {

node::FinalitySig Candidate(uint32_t index = 0)
{
    node::FinalitySig sig;
    sig.height = 100;
    sig.index = index;
    sig.signature[0] = 1;
    return sig;
}

node::FinalitySig MakeSig(const node::FinalityTracker::State& state, const CChain& chain,
                      const uint256& domain, const Consensus::Params& params,
                      const uint64_t height, const uint32_t index, const bls::SecretKey& key)
{
    node::FinalitySig sig;
    sig.epoch = state.epoch;
    sig.height = height;
    sig.index = index;
    const auto fb{node::FinalitySignaturePool::ExpectedFinalizedBlock(sig.epoch, height, state, chain, params)};
    BOOST_REQUIRE(fb);
    const uint256 digest{modern::FinalityDigest(domain, *fb)};
    sig.signature = key.Sign(std::span<const unsigned char>(digest.begin(), 32)).Compressed();
    return sig;
}

/** Drain actual mocked wire messages, not an alternate transport model. */
std::vector<node::FinalitySig> DrainSignatures(CNode& peer)
{
    LOCK(peer.cs_vSend);
    std::vector<node::FinalitySig> out;
    const auto decode = [&](const std::string& type, const std::span<const uint8_t> payload) {
        if (type != NetMsgType::FINSIG) return;
        SpanReader reader{std::as_bytes(payload)};
        node::FinalitySig sig;
        reader >> sig;
        BOOST_CHECK(reader.empty());
        out.push_back(sig);
    };
    std::vector<uint8_t> wire;
    while (true) {
        const auto& [bytes, more, type]{peer.m_transport->GetBytesToSend(false)};
        if (bytes.empty()) break;
        wire.insert(wire.end(), bytes.begin(), bytes.end());
        peer.m_transport->MarkBytesSent(bytes.size());
    }
    size_t offset{0};
    while (offset + 24 <= wire.size()) {
        std::string type(reinterpret_cast<const char*>(wire.data() + offset + 4), 12);
        type.resize(strnlen(type.c_str(), 12));
        const uint32_t size{ReadLE32(wire.data() + offset + 16)};
        BOOST_REQUIRE_LE(offset + 24 + size, wire.size());
        decode(type, std::span<const uint8_t>{wire}.subspan(offset + 24, size));
        offset += 24 + size;
    }
    for (const CSerializedNetMsg& msg : peer.vSendMsg) {
        decode(msg.m_type, std::span<const uint8_t>{msg.data});
    }
    peer.vSendMsg.clear();
    peer.m_send_memusage = 0;
    // The real socket writer recomputes this after draining its queues.
    // Without that step an in-process peer stays permanently backpressured.
    peer.fPauseSend = false;
    return out;
}

struct FinalityTransportNetSetup : b3test::FinalityChainFixture {
    SteadyClockContext transport_clock;
    std::set<NodeId> live;

    void SetupNetwork()
    {
        // The finality fixture intentionally has no network. Construct only
        // in-process mocks after the synthetic chain has reached modern PoS.
        m_node.netgroupman = std::make_unique<NetGroupManager>(NetGroupManager::NoAsmap());
        m_node.addrman = std::make_unique<AddrMan>(*m_node.netgroupman, false, 0);
        m_node.connman = std::make_unique<ConnmanTestMsg>(0x1337, 0x1337, *m_node.addrman,
                                                        *m_node.netgroupman, Params());
        PeerManager::Options opts;
        opts.deterministic_rng = true;
        m_node.peerman = PeerManager::make(*m_node.connman, *m_node.addrman, nullptr,
                                           *m_node.chainman, *m_node.mempool, *m_node.warnings, opts);
        CConnman::Options options;
        options.m_msgproc = m_node.peerman.get();
        // Options defaults to zero, which marks a mocked peer paused after
        // even one ping. Use a realistic bounded buffer; no sockets are run.
        options.nSendBufferMaxSize = 1024 * 1024;
        m_node.connman->Init(options);
    }

    ConnmanTestMsg& Connman() { return static_cast<ConnmanTestMsg&>(*m_node.connman); }

    CNode& Connect(const NodeId id, const bool finish_handshake = true)
        EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
    {
        struct in_addr address;
        address.s_addr = 0xa0b0c001 + id;
        auto* peer = new CNode(id, nullptr, CAddress{CService{CNetAddr{address}, Params().GetDefaultPort()}, NODE_NONE},
                               0, 0, CAddress{}, "", ConnectionType::OUTBOUND_FULL_RELAY, false, 0);
        Connman().AddTestNode(*peer);
        live.insert(id);
        Connman().Handshake(*peer, finish_handshake, ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                            ServiceFlags(NODE_NETWORK | NODE_WITNESS), B3_MODERN_PROTOCOL_VERSION, true);
        BOOST_REQUIRE(!peer->fDisconnect);
        return *peer;
    }

    void Receive(CNode& peer, const node::FinalitySig& sig)
        EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
    {
        BOOST_REQUIRE(Connman().ReceiveMsgFrom(peer, NetMsg::Make(NetMsgType::FINSIG, sig)));
        peer.fPauseSend = false;
        Connman().ProcessMessagesOnce(peer);
    }

    void Disconnect(CNode& peer)
    {
        peer.fDisconnect = true;
        m_node.peerman->FinalizeNode(peer);
        live.erase(peer.GetId());
    }

    ~FinalityTransportNetSetup()
    {
        if (!m_node.peerman) return;
        LOCK(NetEventsInterface::g_msgproc_mutex);
        for (CNode* peer : Connman().TestNodes()) {
            if (live.count(peer->GetId())) m_node.peerman->FinalizeNode(*peer);
        }
        Connman().ClearTestNodes();
        m_node.peerman.reset();
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(finality_transport_tests)

BOOST_AUTO_TEST_CASE(pending_variants_budget_expiry_and_branch_pruning)
{
    node::PendingFinalitySignatures queue;
    const auto valid = [](const auto&) { return true; };
    const auto budget = [](int64_t) { return true; };
    const uint256 digest{1};
    const auto good{Candidate()};
    auto bad{good};
    bad.signature[1] = 1;
    BOOST_CHECK(queue.Add(bad, 1, digest, 0us));
    BOOST_CHECK(queue.Add(good, 1, digest, 0us));
    BOOST_CHECK(!queue.Add(good, 2, digest, 1s));
    BOOST_CHECK_EQUAL(queue.PeerSize(1), 2U);
    BOOST_CHECK(queue.TakeReady(99, 1s, valid, budget).empty());
    BOOST_CHECK(queue.TakeReady(100, 1s, valid, [](int64_t) { return false; }).empty());
    BOOST_CHECK_EQUAL(queue.Size(), 2U);
    const auto ready{queue.TakeReady(100, 2s, valid, budget)};
    BOOST_REQUIRE_EQUAL(ready.size(), 2U);
    BOOST_CHECK(std::any_of(ready.begin(), ready.end(), [&](const auto& e) { return e.sig == good; }));
    BOOST_CHECK_EQUAL(queue.Size(), 0U);
    BOOST_CHECK_EQUAL(queue.PeerSize(1), 0U);

    BOOST_CHECK(queue.Add(good, 1, digest, 3s));
    BOOST_CHECK(queue.TakeReady(100, 4s, [](const auto&) { return false; }, budget).empty());
    BOOST_CHECK_EQUAL(queue.Size(), 0U); // fork, finalized, or disconnected
    BOOST_CHECK(queue.Add(good, 1, digest, 5s));
    BOOST_CHECK(!queue.Add(good, 2, digest, 300s)); // does not refresh expiry
    BOOST_CHECK(queue.TakeReady(100, 305s, valid, budget).empty());
    BOOST_CHECK_EQUAL(queue.Size(), 0U);
}

BOOST_AUTO_TEST_CASE(pending_caps_and_retry_fairness)
{
    node::PendingFinalitySignatures queue;
    const uint256 digest{1};
    for (uint32_t i{0}; i < node::PendingFinalitySignatures::MAX_PEER_ENTRIES; ++i) {
        BOOST_REQUIRE(queue.Add(Candidate(i), 1, digest, 0us));
    }
    BOOST_CHECK(!queue.Add(Candidate(9000), 1, digest, 0us));
    BOOST_CHECK(queue.Add(Candidate(9000), 2, digest, 0us));
    const auto ready{queue.TakeReady(100, 1s, [](const auto&) { return true; }, [](int64_t) { return true; })};
    BOOST_CHECK_LE(ready.size(), node::PendingFinalitySignatures::MAX_RETRY_BATCH);
    BOOST_CHECK_EQUAL(std::count_if(ready.begin(), ready.end(), [](const auto& e) { return e.peer == 1; }),
                      node::PendingFinalitySignatures::MAX_RETRY_PER_PEER);
    BOOST_CHECK_EQUAL(std::count_if(ready.begin(), ready.end(), [](const auto& e) { return e.peer == 2; }), 1);
    queue.Expire(300s);
    BOOST_CHECK_EQUAL(queue.Size(), 0U);

    // A burst of variants at one claimed index cannot consume a whole retry
    // batch ahead of an unrelated validator's candidate.
    for (uint32_t i{0}; i < 100; ++i) {
        auto sig{Candidate()};
        sig.signature[1] = i;
        BOOST_REQUIRE(queue.Add(sig, 1, digest, 301s));
    }
    BOOST_CHECK(queue.Add(Candidate(1), 1, digest, 301s));
    const auto fair{queue.TakeReady(100, 302s, [](const auto&) { return true; }, [](int64_t) { return true; })};
    BOOST_CHECK_EQUAL(fair.size(), 3U);
    BOOST_CHECK_EQUAL(std::count_if(fair.begin(), fair.end(), [](const auto& e) { return e.sig.index == 1; }), 1);
}

BOOST_AUTO_TEST_CASE(relay_batches_are_bounded_and_reconnect_starts_fresh)
{
    node::FinalityRelayCursor cursor;
    BOOST_CHECK(cursor.Take(5, 0us) == std::make_pair(size_t{0}, size_t{2}));
    BOOST_CHECK_EQUAL(cursor.Take(5, 1s).second, 0U);
    BOOST_CHECK(cursor.Take(5, 2s) == std::make_pair(size_t{2}, size_t{2}));
    BOOST_CHECK(cursor.Take(5, 4s) == std::make_pair(size_t{4}, size_t{1}));
    BOOST_CHECK_EQUAL(cursor.Take(5, 33s).second, 0U);
    BOOST_CHECK_EQUAL(cursor.Take(5, 34s).second, 2U);
    node::FinalityRelayCursor reconnected;
    BOOST_CHECK(reconnected.Take(5, 5s) == std::make_pair(size_t{0}, size_t{2}));
    BOOST_CHECK_EQUAL(reconnected.Take(0, 7s).second, 0U); // pool pruned
    node::FinalityRelayCursor hub58;
    BOOST_CHECK_EQUAL(hub58.Take(5, 0s, 58).second, 2U);
    BOOST_CHECK_EQUAL(hub58.Take(5, 1s, 58).second, 0U);
    BOOST_CHECK_EQUAL(hub58.Take(5, 2s, 58).second, 2U);
    node::FinalityRelayCursor hub125;
    BOOST_CHECK_EQUAL(hub125.Take(5, 0s, 125).second, 2U);
    BOOST_CHECK_EQUAL(hub125.Take(5, 3s, 125).second, 0U);
    BOOST_CHECK_EQUAL(hub125.Take(5, 4s, 125).second, 2U);
}

BOOST_FIXTURE_TEST_CASE(early_messages_retry_then_verified_votes_replay_on_loss_and_reconnect, FinalityTransportNetSetup)
{
    PrepareFinalityChain();
    ProduceTo(m_M + 11, m_vk_a); // checkpoint M+10 is only one block deep
    SetupNetwork();
    LOCK(NetEventsInterface::g_msgproc_mutex);
    CNode& source{Connect(0)};
    CNode& receiver{Connect(1)};
    DrainSignatures(source);
    DrainSignatures(receiver);
    const Consensus::Params& params{m_node.chainman->GetConsensus()};
    node::FinalitySig a, b;
    {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        const auto& state{Finality().Current()};
        a = MakeSig(state, chain, m_domain, params, m_M + 10, *state.current->IndexOf(m_vk_a), m_bls_a);
        b = MakeSig(state, chain, m_domain, params, m_M + 10, *state.current->IndexOf(m_vk_b), m_bls_b);
        BOOST_CHECK(node::FinalityTransportDigest(a, state, chain, params));
        auto malformed{a};
        malformed.epoch = state.epoch + 1;
        BOOST_CHECK(!node::FinalityTransportDigest(malformed, state, chain, params));
        malformed = a;
        malformed.index = state.current->Size();
        BOOST_CHECK(!node::FinalityTransportDigest(malformed, state, chain, params));
        malformed = a;
        malformed.height = std::numeric_limits<uint64_t>::max();
        BOOST_CHECK(!node::FinalityTransportDigest(malformed, state, chain, params));
    }
    auto poison{a};
    poison.signature.fill(0);
    Receive(source, poison);
    Receive(source, a);
    Receive(source, b);
    BOOST_CHECK(DrainSignatures(receiver).empty());
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return m_node.chainman->ActiveChainstate().FinalitySignatures().SignatureCount(a.epoch, a.height)), 0U);

    ProduceTo(m_M + 13, m_vk_a); // unchanged consensus depth is now satisfied
    SetMockTime(GetTime() + 2);
    transport_clock += 2s;
    BOOST_CHECK(m_node.peerman->SendMessages(receiver));
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return m_node.chainman->ActiveChainstate().FinalitySignatures().SignatureCount(a.epoch, a.height)), 2U);
    const auto first{DrainSignatures(receiver)};
    BOOST_CHECK(std::find(first.begin(), first.end(), a) != first.end());
    BOOST_CHECK(std::find(first.begin(), first.end(), b) != first.end());
    BOOST_CHECK(std::find(first.begin(), first.end(), poison) == first.end());

    // Drop every first-delivery byte. A clock-driven retry sends the EXACT
    // already-verified messages without invoking a validator signer again.
    SetMockTime(GetTime() + 30);
    transport_clock += 30s;
    receiver.fPauseSend = true;
    BOOST_CHECK(m_node.peerman->SendMessages(receiver));
    BOOST_CHECK(DrainSignatures(receiver).empty()); // respects backpressure
    // Draining models the socket becoming writable. The blocked pass must
    // not advance the replay cursor or postpone its already-due batch.
    BOOST_CHECK(m_node.peerman->SendMessages(receiver));
    const auto retry{DrainSignatures(receiver)};
    BOOST_REQUIRE_EQUAL(retry.size(), 2U);
    BOOST_CHECK(std::find(retry.begin(), retry.end(), a) != retry.end());
    BOOST_CHECK(std::find(retry.begin(), retry.end(), b) != retry.end());
    BOOST_CHECK(m_node.peerman->SendMessages(receiver));
    BOOST_CHECK(DrainSignatures(receiver).empty()); // no send-loop flood

    Disconnect(receiver);
    CNode& reconnect{Connect(2, false)};
    BOOST_REQUIRE(Connman().ReceiveMsgFrom(reconnect, NetMsg::Make(NetMsgType::VERACK)));
    reconnect.fPauseSend = false;
    Connman().ProcessMessagesOnce(reconnect);
    BOOST_CHECK(m_node.peerman->SendMessages(reconnect));
    const auto restored{DrainSignatures(reconnect)};
    BOOST_REQUIRE_EQUAL(restored.size(), 2U);
    BOOST_CHECK(std::find(restored.begin(), restored.end(), a) != restored.end());
    BOOST_CHECK(std::find(restored.begin(), restored.end(), b) != restored.end());
}

BOOST_AUTO_TEST_SUITE_END()
