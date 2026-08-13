// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! Mocked P2P tests for the separation of legacy and modern B3 peer
//! capabilities. All peers are in-process mocks; no live peers are
//! contacted.

#include <crypto/common.h>
#include <legacy/consensus.h>
#include <net.h>
#include <net_processing.h>
#include <netaddress.h>
#include <protocol.h>
#include <serialize.h>
#include <streams.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <span>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(legacy_net_tests, TestingSetup)

namespace {

CService ip(uint32_t i)
{
    struct in_addr s;
    s.s_addr = i;
    return CService(CNetAddr(s), Params().GetDefaultPort());
}

std::unique_ptr<CNode> MakeNode(NodeId id)
{
    return std::make_unique<CNode>(id,
                                   /*sock=*/nullptr,
                                   CAddress{ip(0xa0b0c001 + id), NODE_NONE},
                                   /*nKeyedNetGroupIn=*/0,
                                   /*nLocalHostNonceIn=*/0,
                                   CAddress{},
                                   /*addrNameIn=*/"",
                                   ConnectionType::OUTBOUND_FULL_RELAY,
                                   /*inbound_onion=*/false,
                                   /*network_key=*/0);
}

struct SentMsg {
    std::string type;
    std::vector<uint8_t> payload;
};

//! Drain and decode every message our node has produced for this mocked
//! peer: bytes already handed to the transport plus still-queued messages.
std::vector<SentMsg> DrainSentMessages(CNode& node)
{
    std::vector<SentMsg> out;
    LOCK(node.cs_vSend);
    std::vector<uint8_t> raw;
    while (true) {
        const auto& [to_send, _more, _type] = node.m_transport->GetBytesToSend(false);
        if (to_send.empty()) break;
        raw.insert(raw.end(), to_send.begin(), to_send.end());
        node.m_transport->MarkBytesSent(to_send.size());
    }
    size_t off{0};
    while (off + 24 <= raw.size()) { // v1 header: magic(4) command(12) len(4) checksum(4)
        std::string type(reinterpret_cast<const char*>(raw.data() + off + 4), 12);
        type.resize(strnlen(type.c_str(), 12));
        const uint32_t len{ReadLE32(raw.data() + off + 16)};
        if (off + 24 + len > raw.size()) break;
        out.push_back({type, {raw.begin() + off + 24, raw.begin() + off + 24 + len}});
        off += 24 + len;
    }
    for (const CSerializedNetMsg& msg : node.vSendMsg) {
        out.push_back({msg.m_type, {msg.data.begin(), msg.data.end()}});
    }
    node.vSendMsg.clear();
    node.m_send_memusage = 0;
    return out;
}

size_t CountType(const std::vector<SentMsg>& msgs, const std::string& type)
{
    size_t count{0};
    for (const SentMsg& msg : msgs) {
        if (msg.type == type) ++count;
    }
    return count;
}

} // namespace

BOOST_AUTO_TEST_CASE(legacy_phase_advertises_the_legacy_version_banner)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    PeerManager& peerman = *m_node.peerman;

    auto node{MakeNode(0)};
    peerman.InitializeNode(*node, ServiceFlags(NODE_NETWORK));
    BOOST_CHECK(peerman.SendMessages(*node));

    const auto msgs{DrainSentMessages(*node)};
    bool found{false};
    for (const SentMsg& msg : msgs) {
        if (msg.type != NetMsgType::VERSION) continue;
        SpanReader reader{std::as_bytes(std::span{msg.payload})};
        int32_t advertised{0};
        reader >> advertised;
        BOOST_CHECK_EQUAL(advertised, legacy::P2P_PROTOCOL_VERSION);
        found = true;
    }
    BOOST_CHECK(found);
    peerman.FinalizeNode(*node);
}

BOOST_AUTO_TEST_CASE(one_owner_deterministic_failover_no_duplicate_download)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    ConnmanTestMsg& connman = static_cast<ConnmanTestMsg&>(*m_node.connman);
    connman.SetPeerConnectTimeout(99999s);
    PeerManager& peerman = *m_node.peerman;

    std::vector<std::unique_ptr<CNode>> nodes;
    for (NodeId id{0}; id < 3; ++id) {
        nodes.push_back(MakeNode(id));
        // Historical-protocol handshake: version 80008, NODE_NETWORK only —
        // a real old client never advertises modern service bits.
        connman.Handshake(*nodes.back(),
                          /*successfully_connected=*/true,
                          /*remote_services=*/ServiceFlags(NODE_NETWORK),
                          /*local_services=*/ServiceFlags(NODE_NETWORK),
                          /*version=*/legacy::P2P_PROTOCOL_VERSION,
                          /*relay_txs=*/true);
        BOOST_REQUIRE(!nodes.back()->fDisconnect);
        // Classification caps the negotiated feature range.
        BOOST_CHECK_EQUAL(nodes.back()->GetCommonVersion(), legacy::P2P_COMPATIBILITY_VERSION);
    }

    for (auto& node : nodes) BOOST_CHECK(peerman.SendMessages(*node));

    // Exactly one peer owns the historical download window: one getblocks in
    // total, from the first eligible peer — no duplicate full-history
    // download across the others, and no silent headers-sync redirect.
    const auto sent0{DrainSentMessages(*nodes[0])};
    const auto sent1{DrainSentMessages(*nodes[1])};
    const auto sent2{DrainSentMessages(*nodes[2])};
    BOOST_CHECK_EQUAL(CountType(sent0, NetMsgType::GETBLOCKS), 1U);
    BOOST_CHECK_EQUAL(CountType(sent1, NetMsgType::GETBLOCKS), 0U);
    BOOST_CHECK_EQUAL(CountType(sent2, NetMsgType::GETBLOCKS), 0U);
    BOOST_CHECK_EQUAL(CountType(sent1, NetMsgType::GETHEADERS), 0U);
    BOOST_CHECK_EQUAL(CountType(sent2, NetMsgType::GETHEADERS), 0U);

    // Owner disconnect clears the owned window state and fails over
    // deterministically to the next eligible peer.
    peerman.FinalizeNode(*nodes[0]);
    for (size_t i{1}; i < nodes.size(); ++i) BOOST_CHECK(peerman.SendMessages(*nodes[i]));
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*nodes[1]), NetMsgType::GETBLOCKS), 1U);
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*nodes[2]), NetMsgType::GETBLOCKS), 0U);

    // And again: the last remaining peer takes over.
    peerman.FinalizeNode(*nodes[1]);
    BOOST_CHECK(peerman.SendMessages(*nodes[2]));
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*nodes[2]), NetMsgType::GETBLOCKS), 1U);

    peerman.FinalizeNode(*nodes[2]);
}

BOOST_AUTO_TEST_CASE(stalled_sync_owner_is_reassigned_after_the_progress_lease)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    ConnmanTestMsg& connman = static_cast<ConnmanTestMsg&>(*m_node.connman);
    connman.SetPeerConnectTimeout(99999s);
    PeerManager& peerman = *m_node.peerman;

    const auto t0{std::chrono::seconds{1'700'000'000}};
    SetMockTime(t0);

    std::vector<std::unique_ptr<CNode>> nodes;
    for (NodeId id{0}; id < 2; ++id) {
        nodes.push_back(MakeNode(id));
        connman.Handshake(*nodes.back(),
                          /*successfully_connected=*/true,
                          /*remote_services=*/ServiceFlags(NODE_NETWORK),
                          /*local_services=*/ServiceFlags(NODE_NETWORK),
                          /*version=*/legacy::P2P_PROTOCOL_VERSION,
                          /*relay_txs=*/true);
        BOOST_REQUIRE(!nodes.back()->fDisconnect);
    }

    // Peer 0 claims the window; peer 1 does not.
    for (auto& node : nodes) BOOST_CHECK(peerman.SendMessages(*node));
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*nodes[0]), NetMsgType::GETBLOCKS), 1U);
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*nodes[1]), NetMsgType::GETBLOCKS), 0U);

    // No block ever connects, so the owner makes no forward progress. Before
    // the lease expires ownership is unchanged: peer 1 still gets nothing, and
    // no disconnect is used to move the window.
    SetMockTime(t0 + 119s);
    for (auto& node : nodes) BOOST_CHECK(peerman.SendMessages(*node));
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*nodes[1]), NetMsgType::GETBLOCKS), 0U);
    BOOST_CHECK(!nodes[0]->fDisconnect);
    (void)DrainSentMessages(*nodes[0]);

    // Past the lease, the stalled owner is released (not banned) and peer 1
    // takes over on its own SendMessages pass.
    SetMockTime(t0 + 121s);
    BOOST_CHECK(peerman.SendMessages(*nodes[1]));
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*nodes[1]), NetMsgType::GETBLOCKS), 1U);
    BOOST_CHECK(!nodes[0]->fDisconnect);

    // The just-stalled peer 0 is in cooldown and cannot immediately reclaim.
    BOOST_CHECK(peerman.SendMessages(*nodes[0]));
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*nodes[0]), NetMsgType::GETBLOCKS), 0U);

    for (auto& node : nodes) peerman.FinalizeNode(*node);
    SetMockTime(0s);
}

BOOST_AUTO_TEST_CASE(modern_capability_peer_does_not_own_the_legacy_window)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    ConnmanTestMsg& connman = static_cast<ConnmanTestMsg&>(*m_node.connman);
    connman.SetPeerConnectTimeout(99999s);
    PeerManager& peerman = *m_node.peerman;

    // A peer with a Core-line version is modern-capable: normal feature
    // negotiation, and it is never handed the historical download window.
    auto modern{MakeNode(0)};
    connman.Handshake(*modern,
                      /*successfully_connected=*/true,
                      /*remote_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*local_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*version=*/PROTOCOL_VERSION,
                      /*relay_txs=*/true);
    BOOST_REQUIRE(!modern->fDisconnect);
    BOOST_CHECK_EQUAL(modern->GetCommonVersion(), PROTOCOL_VERSION);
    BOOST_CHECK(peerman.SendMessages(*modern));
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*modern), NetMsgType::GETBLOCKS), 0U);

    // A legacy-capable peer arriving afterwards claims the window.
    auto old_client{MakeNode(1)};
    connman.Handshake(*old_client,
                      /*successfully_connected=*/true,
                      /*remote_services=*/ServiceFlags(NODE_NETWORK),
                      /*local_services=*/ServiceFlags(NODE_NETWORK),
                      /*version=*/legacy::P2P_PROTOCOL_VERSION,
                      /*relay_txs=*/true);
    BOOST_REQUIRE(!old_client->fDisconnect);
    BOOST_CHECK(peerman.SendMessages(*old_client));
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*old_client), NetMsgType::GETBLOCKS), 1U);

    peerman.FinalizeNode(*modern);
    peerman.FinalizeNode(*old_client);
}

BOOST_AUTO_TEST_SUITE_END()
