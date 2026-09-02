// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! Mocked P2P tests for the separation of legacy and modern B3 peer
//! capabilities. All peers are in-process mocks; no live peers are
//! contacted.

#include <consensus/era.h>
#include <chain.h>
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
#include <validationinterface.h>

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

std::unique_ptr<CNode> MakeNode(
    NodeId id,
    ConnectionType connection_type = ConnectionType::OUTBOUND_FULL_RELAY)
{
    return std::make_unique<CNode>(id,
                                   /*sock=*/nullptr,
                                   CAddress{ip(0xa0b0c001 + id), NODE_NONE},
                                   /*nKeyedNetGroupIn=*/0,
                                   /*nLocalHostNonceIn=*/0,
                                   CAddress{},
                                   /*addrNameIn=*/"",
                                   connection_type,
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

BOOST_AUTO_TEST_CASE(empty_inventory_does_not_declare_sync_complete)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    ConnmanTestMsg& connman = static_cast<ConnmanTestMsg&>(*m_node.connman);
    connman.SetPeerConnectTimeout(99999s);
    PeerManager& peerman = *m_node.peerman;

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

    // Peer 0 claims the window and sends its getblocks.
    for (auto& node : nodes) BOOST_CHECK(peerman.SendMessages(*node));
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*nodes[0]), NetMsgType::GETBLOCKS), 1U);
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*nodes[1]), NetMsgType::GETBLOCKS), 0U);

    // Peer 0 answers with an empty inventory. The synthetic chain is nowhere
    // near any pinned target, so this is peer-local exhaustion: it must not
    // permanently end legacy synchronization.
    BOOST_REQUIRE(connman.ReceiveMsgFrom(*nodes[0], NetMsg::Make(NetMsgType::INV, std::vector<CInv>{})));
    nodes[0]->fPauseSend = false; // ReceiveMsgFrom routes through the send path
    connman.ProcessMessagesOnce(*nodes[0]);
    BOOST_CHECK(!nodes[0]->fDisconnect); // exhaustion is not misbehavior

    // The window is released, so the other peer continues synchronization
    // immediately -- no disconnect and no waiting for the progress lease.
    BOOST_CHECK(peerman.SendMessages(*nodes[1]));
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*nodes[1]), NetMsgType::GETBLOCKS), 1U);

    // And the exhausted peer does not immediately re-poll while another peer
    // holds the window.
    BOOST_CHECK(peerman.SendMessages(*nodes[0]));
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*nodes[0]), NetMsgType::GETBLOCKS), 0U);

    // Exhaust the second peer too. Both are now unproductive, so neither may
    // retake the window: the per-peer retry bar must hold them both off. If
    // exhaustion were tracked in a single shared slot, the second exhaustion
    // would clear the first peer's bar and the two would hand the window back
    // and forth, one getblocks per round trip, indefinitely.
    BOOST_REQUIRE(connman.ReceiveMsgFrom(*nodes[1], NetMsg::Make(NetMsgType::INV, std::vector<CInv>{})));
    nodes[1]->fPauseSend = false;
    connman.ProcessMessagesOnce(*nodes[1]);
    for (auto& node : nodes) {
        BOOST_CHECK(peerman.SendMessages(*node));
        BOOST_CHECK(!node->fDisconnect);
    }
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*nodes[0]), NetMsgType::GETBLOCKS), 0U);
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*nodes[1]), NetMsgType::GETBLOCKS), 0U);

    for (auto& node : nodes) peerman.FinalizeNode(*node);
}

BOOST_AUTO_TEST_CASE(exhausted_peer_repolls_after_its_retry_bar_expires)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    ConnmanTestMsg& connman = static_cast<ConnmanTestMsg&>(*m_node.connman);
    connman.SetPeerConnectTimeout(99999s);
    PeerManager& peerman = *m_node.peerman;

    const auto t0{std::chrono::seconds{1'700'000'000}};
    SetMockTime(t0);

    // A single legacy peer: after it exhausts, only it can continue, so this
    // isolates the re-poll behaviour that the exhaustion reset enables.
    auto node{MakeNode(0)};
    connman.Handshake(*node,
                      /*successfully_connected=*/true,
                      /*remote_services=*/ServiceFlags(NODE_NETWORK),
                      /*local_services=*/ServiceFlags(NODE_NETWORK),
                      /*version=*/legacy::P2P_PROTOCOL_VERSION,
                      /*relay_txs=*/true);
    BOOST_REQUIRE(!node->fDisconnect);

    const auto exhaust{[&]() EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex) {
        BOOST_REQUIRE(connman.ReceiveMsgFrom(*node, NetMsg::Make(NetMsgType::INV, std::vector<CInv>{})));
        node->fPauseSend = false;
        connman.ProcessMessagesOnce(*node);
    }};

    // Claim, then exhaust: the window is released and this peer is barred for
    // one lease period.
    BOOST_CHECK(peerman.SendMessages(*node));
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*node), NetMsgType::GETBLOCKS), 1U);
    exhaust();
    BOOST_CHECK(!node->fDisconnect);

    // Still inside the bar: no re-poll.
    SetMockTime(t0 + 119s);
    BOOST_CHECK(peerman.SendMessages(*node));
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*node), NetMsgType::GETBLOCKS), 0U);

    // Past the bar: the peer reclaims the window and asks again. This is what
    // makes exhaustion temporary rather than a permanent completion latch --
    // without clearing the exhausted flag on reclaim, no getblocks is sent and
    // synchronization stays dead.
    SetMockTime(t0 + 121s);
    BOOST_CHECK(peerman.SendMessages(*node));
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*node), NetMsgType::GETBLOCKS), 1U);
    BOOST_CHECK(!node->fDisconnect);

    // And it is a poll, not a one-shot: exhaust and wait again, and it asks a
    // third time.
    exhaust();
    SetMockTime(t0 + 242s);
    BOOST_CHECK(peerman.SendMessages(*node));
    BOOST_CHECK_EQUAL(CountType(DrainSentMessages(*node), NetMsgType::GETBLOCKS), 1U);

    peerman.FinalizeNode(*node);
    SetMockTime(0s);
}

BOOST_AUTO_TEST_CASE(legacy_peer_services_are_outbound_eligible)
{
    PeerManager& peerman = *m_node.peerman;

    // A historical B3 peer advertises NODE_NETWORK only: the legacy era has no
    // witness data, so NODE_WITNESS is not a capability it can ever offer.
    // Outbound selection must still consider it eligible, or every legacy peer
    // becomes unreachable once its real services are recorded in addrman.
    BOOST_CHECK(peerman.HasAllDesirableServiceFlags(ServiceFlags(NODE_NETWORK)));
    BOOST_CHECK(!(peerman.GetDesirableServiceFlags(ServiceFlags(NODE_NETWORK)) & NODE_WITNESS));

    // A peer offering nothing useful is still undesirable.
    BOOST_CHECK(!peerman.HasAllDesirableServiceFlags(ServiceFlags(NODE_NONE)));

    // A peer that also advertises witness remains eligible.
    BOOST_CHECK(peerman.HasAllDesirableServiceFlags(ServiceFlags(NODE_NETWORK | NODE_WITNESS)));
}

BOOST_AUTO_TEST_CASE(modern_archival_peer_owns_legacy_window_and_renegotiates_at_h)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    ConnmanTestMsg& connman = static_cast<ConnmanTestMsg&>(*m_node.connman);
    connman.SetPeerConnectTimeout(99999s);
    PeerManager& peerman = *m_node.peerman;

    // A fresh pre-H node may have only a post-H upgraded archival peer. The
    // local 80008 banner makes this mixed connection use the legacy common
    // mode, and the archival peer must still be allowed to provide ordered
    // full-block history through getblocks/inv/getdata.
    CNode* modern{MakeNode(0).release()};
    connman.Handshake(*modern,
                      /*successfully_connected=*/true,
                      /*remote_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*local_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*version=*/PROTOCOL_VERSION,
                      /*relay_txs=*/true);
    BOOST_REQUIRE(!modern->fDisconnect);
    BOOST_CHECK_EQUAL(modern->GetCommonVersion(),
                      legacy::P2P_COMPATIBILITY_VERSION);
    connman.AddTestNode(*modern);
    BOOST_CHECK(peerman.SendMessages(*modern));
    BOOST_CHECK_EQUAL(
        CountType(DrainSentMessages(*modern), NetMsgType::GETBLOCKS), 1U);

    // A block inventory from that mixed-version peer follows the historical
    // ordered full-block path rather than being discarded as a modern headers
    // announcement.
    BOOST_REQUIRE(connman.ReceiveMsgFrom(
        *modern, NetMsg::Make(NetMsgType::INV,
                              std::vector<CInv>{{MSG_BLOCK, uint256::ONE}})));
    modern->fPauseSend = false;
    connman.ProcessMessagesOnce(*modern);
    BOOST_CHECK_EQUAL(
        CountType(DrainSentMessages(*modern), NetMsgType::GETDATA), 1U);

    // Also cover the connection-thread race: this outbound socket has sent
    // 80008 but has not completed VERSION/VERACK, so ForEachNode will not see
    // it during the tip callback. Its next message pass must still close it.
    auto pending{MakeNode(1)};
    peerman.InitializeNode(*pending, ServiceFlags(NODE_NETWORK));
    BOOST_CHECK(peerman.SendMessages(*pending));
    BOOST_CHECK_EQUAL(
        CountType(DrainSentMessages(*pending), NetMsgType::VERSION), 1U);
    BOOST_CHECK(!pending->fSuccessfullyConnected);

    // VERSION cannot change on an open socket. Once our tip reaches H, every
    // connection on which we sent 80008 is closed without punishment so the
    // upgraded pair immediately renegotiates the modern protocol.
    m_node.validation_signals->RegisterValidationInterface(&peerman);
    CBlockIndex boundary;
    uint256 boundary_hash{uint256::ONE};
    boundary.phashBlock = &boundary_hash;
    boundary.nHeight = *Consensus::LegacyFinalHeight(Params().GetConsensus());
    m_node.validation_signals->ActiveTipChange(boundary, /*is_ibd=*/false);
    m_node.validation_signals->UnregisterValidationInterface(&peerman);
    BOOST_CHECK(modern->fDisconnect);
    BOOST_CHECK(peerman.SendMessages(*pending));
    BOOST_CHECK(pending->fDisconnect);
    peerman.FinalizeNode(*pending);

    auto renegotiated{MakeNode(2)};
    peerman.InitializeNode(*renegotiated, ServiceFlags(NODE_NETWORK | NODE_WITNESS));
    BOOST_CHECK(peerman.SendMessages(*renegotiated));
    bool found_modern_version{false};
    for (const SentMsg& msg : DrainSentMessages(*renegotiated)) {
        if (msg.type != NetMsgType::VERSION) continue;
        SpanReader reader{std::as_bytes(std::span{msg.payload})};
        int32_t advertised{0};
        reader >> advertised;
        BOOST_CHECK_EQUAL(advertised, PROTOCOL_VERSION);
        found_modern_version = true;
    }
    BOOST_CHECK(found_modern_version);
    peerman.FinalizeNode(*renegotiated);

    // A lagging historical node now initiates an inbound connection to this
    // post-H archival node. The old client rejects any VERSION below 80006,
    // so replying with our normal 70016 banner would make archival bootstrap
    // impossible despite all later getblocks compatibility code.
    auto historical_inbound{MakeNode(3, ConnectionType::INBOUND)};
    peerman.InitializeNode(*historical_inbound,
                           ServiceFlags(NODE_NETWORK | NODE_WITNESS));
    BOOST_REQUIRE(connman.ReceiveMsgFrom(
        *historical_inbound,
        NetMsg::Make(NetMsgType::VERSION,
                     int32_t{legacy::P2P_PROTOCOL_VERSION},
                     Using<CustomUintFormatter<8>>(
                         ServiceFlags{NODE_NETWORK}),
                     int64_t{}, int64_t{}, CNetAddr::V1(CService{}),
                     int64_t{}, CNetAddr::V1(CService{}), uint64_t{1},
                     std::string{}, int32_t{}, true)));
    historical_inbound->fPauseSend = false;
    connman.ProcessMessagesOnce(*historical_inbound);

    bool found_legacy_reply{false};
    for (const SentMsg& msg : DrainSentMessages(*historical_inbound)) {
        if (msg.type != NetMsgType::VERSION) continue;
        SpanReader reader{std::as_bytes(std::span{msg.payload})};
        int32_t advertised{0};
        reader >> advertised;
        BOOST_CHECK_GE(advertised, 80'006);
        BOOST_CHECK_EQUAL(advertised, legacy::P2P_PROTOCOL_VERSION);
        found_legacy_reply = true;
    }
    BOOST_CHECK(found_legacy_reply);
    BOOST_CHECK_EQUAL(historical_inbound->GetCommonVersion(),
                      legacy::P2P_COMPATIBILITY_VERSION);

    // This 80008 was a post-H compatibility reply, not a stale pre-H banner:
    // the boundary-renegotiation guard must keep it open through VERACK.
    BOOST_CHECK(peerman.SendMessages(*historical_inbound));
    BOOST_CHECK(!historical_inbound->fDisconnect);
    BOOST_REQUIRE(connman.ReceiveMsgFrom(
        *historical_inbound, NetMsg::Make(NetMsgType::VERACK)));
    historical_inbound->fPauseSend = false;
    connman.ProcessMessagesOnce(*historical_inbound);
    BOOST_CHECK(peerman.SendMessages(*historical_inbound));
    BOOST_CHECK(historical_inbound->fSuccessfullyConnected);
    BOOST_CHECK(!historical_inbound->fDisconnect);
    peerman.FinalizeNode(*historical_inbound);

    peerman.FinalizeNode(*modern);
    connman.ClearTestNodes();
}

BOOST_AUTO_TEST_SUITE_END()
