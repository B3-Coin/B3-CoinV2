// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! Mocked P2P tests for the separation of legacy and modern B3 peer
//! capabilities. All peers are in-process mocks; no live peers are
//! contacted.

#include <addrman.h>
#include <arith_uint256.h>
#include <banman.h>
#include <chain.h>
#include <consensus/block_codec.h>
#include <consensus/era.h>
#include <consensus/merkle.h>
#include <crypto/common.h>
#include <legacy/codec.h>
#include <legacy/consensus.h>
#include <modern/pos_v1.h>
#include <net.h>
#include <net_processing.h>
#include <netaddress.h>
#include <pow.h>
#include <protocol.h>
#include <serialize.h>
#include <streams.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

CService ip(uint32_t i)
{
    struct in_addr s;
    s.s_addr = i;
    return CService(CNetAddr(s), Params().GetDefaultPort());
}

uint256 BlockHashFromInt(uint64_t n)
{
    uint256 out;
    out.data()[0] = static_cast<unsigned char>(n & 0xff);
    out.data()[1] = static_cast<unsigned char>(n >> 8);
    return out;
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

struct ModernOrphanNetSetup : public TestingSetup {
    ModernOrphanNetSetup()
        : TestingSetup{ChainType::REGTEST,
                       {.extra_args = {"-b3modernregtest",
                                       "-b3corridorlength=2"}}}
    {
    }
};

CBlock BuildMarkerModernBlock(const uint256& prev_hash, int height,
                              uint32_t block_time, uint32_t bits,
                              CAmount coinbase_value = 0)
{
    CMutableTransaction coinbase;
    coinbase.version = 2;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig =
        CScript() << CScriptNum{height} << CScriptNum{7};
    coinbase.vout.emplace_back(coinbase_value, CScript() << OP_TRUE);

    CBlock block;
    block.nVersion =
        static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
    block.hashPrevBlock = prev_hash;
    block.nTime = block_time;
    block.nBits = bits;
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    block.hashMerkleRoot = BlockMerkleRoot(block);
    return block;
}

class BlockCheckedRecorder final : public CValidationInterface
{
public:
    explicit BlockCheckedRecorder(uint256 watched) : m_watched{watched} {}

    void BlockChecked(const std::shared_ptr<const CBlock>& block,
                      const BlockValidationState&) override
    {
        if (block->GetHash() == m_watched) ++m_seen;
    }

    int Seen() const { return m_seen.load(); }

private:
    const uint256 m_watched;
    std::atomic<int> m_seen{0};
};

void ProcessB3BlockMessage(ConnmanTestMsg& connman, CNode& node,
                           const CBlock& block)
    EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
{
    BOOST_REQUIRE(connman.ReceiveMsgFrom(
        node, NetMsg::Make(NetMsgType::BLOCK,
                          legacy::TX_LEGACY(block))));
    node.fPauseSend = false;
    connman.ProcessMessagesOnce(node);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(legacy_net_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(b3_protocol_version_namespaces_are_pinned)
{
    BOOST_CHECK_EQUAL(PROTOCOL_VERSION, 70'016);
    BOOST_CHECK_EQUAL(legacy::P2P_PROTOCOL_VERSION, 80'008);
    BOOST_CHECK_EQUAL(B3_MODERN_PROTOCOL_VERSION, 80'010);
    BOOST_CHECK_GT(B3_MODERN_PROTOCOL_VERSION,
                   legacy::P2P_PROTOCOL_VERSION);
}

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
    BOOST_CHECK(!connman.B3ModernSeedRescueRequested());
    m_node.validation_signals->RegisterValidationInterface(&peerman);
    CBlockIndex boundary;
    uint256 boundary_hash{uint256::ONE};
    boundary.phashBlock = &boundary_hash;
    boundary.nHeight = *Consensus::LegacyFinalHeight(Params().GetConsensus());
    m_node.validation_signals->ActiveTipChange(boundary, /*is_ibd=*/false);
    m_node.validation_signals->UnregisterValidationInterface(&peerman);
    BOOST_CHECK(connman.B3ModernSeedRescueRequested());
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
        BOOST_CHECK_EQUAL(advertised, B3_MODERN_PROTOCOL_VERSION);
        found_modern_version = true;
    }
    BOOST_CHECK(found_modern_version);
    peerman.FinalizeNode(*renegotiated);

    // B3_MODERN_PROTOCOL_VERSION (80010; 80009 before the v1.1.3
    // finality-recovery build) is B3's modern wire identity, not a claim that
    // Core has features beyond 70016. Two modern B3 peers therefore cap their
    // effective feature version at the inherited Core capability ceiling.
    auto modern_handshake{MakeNode(4)};
    connman.Handshake(*modern_handshake,
                      /*successfully_connected=*/true,
                      /*remote_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*local_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*version=*/B3_MODERN_PROTOCOL_VERSION,
                      /*relay_txs=*/true);
    BOOST_REQUIRE(!modern_handshake->fDisconnect);
    BOOST_CHECK_EQUAL(modern_handshake->GetCommonVersion(), PROTOCOL_VERSION);
    peerman.FinalizeNode(*modern_handshake);

    // The immediately preceding modern identity remains fully compatible.
    // It is above the sealed legacy range and negotiates the same inherited
    // Core feature ceiling, so the recovery banner does not partition 80009
    // wallets from 80010 wallets.
    auto previous_modern{MakeNode(42)};
    connman.Handshake(*previous_modern,
                      /*successfully_connected=*/true,
                      /*remote_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*local_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*version=*/80'009,
                      /*relay_txs=*/true);
    BOOST_REQUIRE(!previous_modern->fDisconnect);
    BOOST_CHECK_EQUAL(previous_modern->GetCommonVersion(), PROTOCOL_VERSION);
    peerman.FinalizeNode(*previous_modern);

    // A post-H automatic outbound modern connection must reject an old node's
    // 80008 reply. It cannot supply modern headers or blocks, and keeping it
    // would let obsolete addresses occupy every useful synchronization slot.
    // This is a clean capability disconnect, not a ban or a consensus fault.
    auto historical_outbound{MakeNode(5)};
    CAddress remembered_historical{historical_outbound->addr,
                                   ServiceFlags(NODE_NETWORK | NODE_WITNESS | NODE_B3_FLOWMESH)};
    BOOST_REQUIRE(m_node.addrman->Add({remembered_historical}, ip(500)));
    connman.Handshake(*historical_outbound,
                      /*successfully_connected=*/true,
                      /*remote_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS | NODE_B3_FLOWMESH),
                      /*local_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*version=*/legacy::P2P_PROTOCOL_VERSION,
                      /*relay_txs=*/true);
    BOOST_CHECK(historical_outbound->fDisconnect);
    BOOST_CHECK(!m_node.banman->IsBanned(historical_outbound->addr));
    BOOST_CHECK(!m_node.banman->IsDiscouraged(historical_outbound->addr));
    const auto remembered{m_node.addrman->GetAddr(
        /*max_addresses=*/0, /*max_pct=*/0, /*network=*/std::nullopt,
        /*filtered=*/false)};
    const auto found{std::find_if(
        remembered.begin(), remembered.end(), [&](const CAddress& candidate) {
            return static_cast<const CService&>(candidate) == historical_outbound->addr;
        })};
    BOOST_REQUIRE(found != remembered.end());
    BOOST_CHECK_EQUAL(found->nServices, NODE_NETWORK);
    peerman.FinalizeNode(*historical_outbound);

    // Block-relay slots are scarce too and obey the same post-H rule.
    auto historical_block_relay{MakeNode(6, ConnectionType::BLOCK_RELAY)};
    connman.Handshake(*historical_block_relay,
                      /*successfully_connected=*/true,
                      /*remote_services=*/ServiceFlags(NODE_NETWORK),
                      /*local_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*version=*/legacy::P2P_PROTOCOL_VERSION,
                      /*relay_txs=*/false);
    BOOST_CHECK(historical_block_relay->fDisconnect);
    peerman.FinalizeNode(*historical_block_relay);

    // Discovery-only automatic sockets must not promote obsolete addresses
    // back into AddrMan's tried set after learning their historical banner.
    for (const ConnectionType type : {ConnectionType::FEELER,
                                      ConnectionType::ADDR_FETCH,
                                      ConnectionType::PRIVATE_BROADCAST}) {
        auto historical_discovery{MakeNode(9 + static_cast<int>(type), type)};
        connman.Handshake(*historical_discovery,
                          /*successfully_connected=*/true,
                          /*remote_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS | NODE_B3_FLOWMESH),
                          /*local_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                          /*version=*/legacy::P2P_PROTOCOL_VERSION,
                          /*relay_txs=*/false);
        BOOST_CHECK(historical_discovery->fDisconnect);
        peerman.FinalizeNode(*historical_discovery);
    }

    // An explicitly configured historical peer is harmless to the automatic
    // slot pool and remains connected under operator control.
    auto historical_manual{MakeNode(7, ConnectionType::MANUAL)};
    connman.Handshake(*historical_manual,
                      /*successfully_connected=*/true,
                      /*remote_services=*/ServiceFlags(NODE_NETWORK),
                      /*local_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*version=*/legacy::P2P_PROTOCOL_VERSION,
                      /*relay_txs=*/true);
    BOOST_REQUIRE(!historical_manual->fDisconnect);
    BOOST_CHECK_EQUAL(historical_manual->GetCommonVersion(),
                      legacy::P2P_COMPATIBILITY_VERSION);
    peerman.FinalizeNode(*historical_manual);

    // The inherited Core capability number 70016 is not a legacy-B3 banner.
    // A pre-80009 upgraded peer therefore remains a valid modern sync peer.
    auto core_version_modern{MakeNode(8)};
    connman.Handshake(*core_version_modern,
                      /*successfully_connected=*/true,
                      /*remote_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*local_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*version=*/PROTOCOL_VERSION,
                      /*relay_txs=*/true);
    BOOST_REQUIRE(!core_version_modern->fDisconnect);
    BOOST_CHECK_EQUAL(core_version_modern->GetCommonVersion(), PROTOCOL_VERSION);
    peerman.FinalizeNode(*core_version_modern);

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

BOOST_FIXTURE_TEST_SUITE(modern_orphan_net_tests, ModernOrphanNetSetup)

BOOST_AUTO_TEST_CASE(modern_headers_reply_across_the_sealed_boundary_is_continuous)
{
    // A node parked at the sealed boundary X is already post-legacy and syncs
    // forward through modern peers with headers-first. Its request must start
    // at X, and a reply that begins with the legacy-codec X header followed by
    // the first corridor header must be accepted as continuous: the corridor
    // header references X by its legacy scrypt identity, not by SHA256d.
    // Before this was fixed the reply was judged non-continuous, every modern
    // peer was discouraged, and the node never learned block H+1.
    LOCK(NetEventsInterface::g_msgproc_mutex);
    ConnmanTestMsg& connman = static_cast<ConnmanTestMsg&>(*m_node.connman);
    PeerManager& peerman = *m_node.peerman;
    Consensus::Params& consensus{
        const_cast<Consensus::Params&>(m_node.chainman->GetConsensus())};
    BOOST_REQUIRE(consensus.transition_pow_bits.has_value());

    // Shape the boundary like mainnet: one real legacy block above genesis
    // becomes the sealed final block X at H = 1, so X's own parent is known
    // and a peer's headers reply can start at X. Until X is pinned the chain
    // is in the X-distribution pause, which accepts the legacy block at H.
    const CBlockIndex* genesis{
        WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip())};
    BOOST_REQUIRE(genesis);
    consensus.legacy_last_pow_block = 1'000;
    consensus.hard_fork_height = 2;
    consensus.legacy_final_hash.reset();
    CBlock legacy1;
    {
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(genesis->GetBlockTime() + 17);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << int64_t{1} << CScriptNum{99};
        coinbase.vout.emplace_back(
            legacy::GetProofOfWorkReward(0, 1, consensus), CScript() << OP_TRUE);
        legacy1.nVersion = 4;
        legacy1.hashPrevBlock = genesis->GetBlockHash();
        legacy1.nTime = coinbase.nTime;
        legacy1.nBits = legacy::GetNextTargetRequired(
            genesis, /*proof_of_stake=*/false, consensus);
        legacy1.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        legacy1.hashMerkleRoot = BlockMerkleRoot(legacy1);
        const arith_uint256 target{arith_uint256().SetCompact(legacy1.nBits)};
        while (UintToArith256(legacy1.GetLegacyB3Hash()) > target) ++legacy1.nNonce;
    }
    {
        DataStream bytes;
        bytes << legacy::TX_LEGACY(legacy1);
        auto decoded{std::make_shared<CBlock>()};
        bytes >> legacy::TX_LEGACY(*decoded);
        bool new_block{false};
        BOOST_REQUIRE(m_node.chainman->ProcessNewBlock(decoded, true, true, &new_block));
    }
    const CBlockIndex* boundary{
        WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip())};
    BOOST_REQUIRE_EQUAL(boundary->nHeight, 1);
    consensus.legacy_final_hash = boundary->GetBlockHash();
    {
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        chainstate.setBlockIndexCandidates.clear();
        chainstate.PopulateBlockIndexCandidates();
    }
    BOOST_REQUIRE_EQUAL(boundary->nHeight,
                        *Consensus::LegacyFinalHeight(consensus));
    // The boundary block is legacy-codec: its identity differs from SHA256d.
    const CBlockHeader boundary_header{boundary->GetBlockHeader()};
    BOOST_REQUIRE(boundary_header.GetMarkerHash(consensus) ==
                  boundary->GetBlockHash());
    BOOST_REQUIRE(boundary_header.GetHash() != boundary->GetBlockHash());

    auto node{MakeNode(0)};
    connman.Handshake(*node,
                      /*successfully_connected=*/true,
                      /*remote_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*local_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*version=*/B3_MODERN_PROTOCOL_VERSION,
                      /*relay_txs=*/true);
    BOOST_REQUIRE(!node->fDisconnect);

    // The initial synchronization request starts at X, not below it.
    bool found_getheaders{false};
    for (const SentMsg& msg : DrainSentMessages(*node)) {
        if (msg.type != NetMsgType::GETHEADERS) continue;
        SpanReader reader{std::as_bytes(std::span{msg.payload})};
        CBlockLocator locator;
        reader >> locator;
        BOOST_REQUIRE(!locator.vHave.empty());
        BOOST_CHECK(locator.vHave.front() == boundary->GetBlockHash());
        found_getheaders = true;
    }
    BOOST_CHECK(found_getheaders);

    // A synced modern peer that was asked from below X (an older node, or a
    // reorganized locator) answers with X itself and the corridor header.
    CBlock corridor{BuildMarkerModernBlock(
        boundary->GetBlockHash(), /*height=*/boundary->nHeight + 1,
        static_cast<uint32_t>(boundary->GetBlockTime() +
                              consensus.transition_pow_min_spacing),
        *consensus.transition_pow_bits)};
    while (!CheckTransitionPowEligibility(corridor)) ++corridor.nNonce;
    BOOST_REQUIRE(corridor.hashPrevBlock == boundary->GetBlockHash());
    std::vector<CBlock> reply;
    reply.emplace_back(boundary_header);
    reply.emplace_back(CBlockHeader{corridor});
    BOOST_REQUIRE(connman.ReceiveMsgFrom(
        *node, NetMsg::Make(NetMsgType::HEADERS, TX_WITH_WITNESS(reply))));
    node->fPauseSend = false;
    connman.ProcessMessagesOnce(*node);
    BOOST_CHECK(peerman.SendMessages(*node));

    // Continuous: the peer is kept, the corridor header becomes the best
    // header, and its block is requested.
    BOOST_CHECK(!node->fDisconnect);
    const uint256 corridor_hash{corridor.GetHash(consensus, boundary->nHeight + 1)};
    BOOST_CHECK(WITH_LOCK(cs_main, return m_node.chainman->m_best_header->GetBlockHash()) ==
                corridor_hash);
    BOOST_CHECK_EQUAL(
        CountType(DrainSentMessages(*node), NetMsgType::GETDATA), 1U);
    peerman.FinalizeNode(*node);
}

BOOST_AUTO_TEST_CASE(connected_wrong_codec_block_keeps_marker_source_identity)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    ConnmanTestMsg& connman = static_cast<ConnmanTestMsg&>(*m_node.connman);
    PeerManager& peerman = *m_node.peerman;

    CNode* node{MakeNode(0).release()};
    connman.Handshake(*node,
                      /*successfully_connected=*/true,
                      /*remote_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*local_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*version=*/B3_MODERN_PROTOCOL_VERSION,
                      /*relay_txs=*/true);
    BOOST_REQUIRE(!node->fDisconnect);
    (void)DrainSentMessages(*node);
    connman.AddTestNode(*node);

    const Consensus::Params& consensus{Params().GetConsensus()};
    const CBlockIndex* genesis{
        WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip())};
    BOOST_REQUIRE(genesis);
    BOOST_REQUIRE_EQUAL(genesis->nHeight, 0);

    // This body is structurally valid under the marker-selected legacy codec,
    // but that codec is forbidden at connected height 1 in this H=0 fixture.
    // Its marker identity (scrypt) therefore differs from the height-selected
    // modern identity (SHA256d). Source bookkeeping must retain the former so
    // BlockChecked can attribute the bad-block-codec failure to this peer.
    CMutableTransaction coinbase;
    coinbase.version = 1;
    coinbase.nTime = static_cast<uint32_t>(genesis->GetBlockTime() + 17);
    coinbase.m_legacy_encoding = true;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << 1 << CScriptNum{99};
    coinbase.vout.emplace_back(0, CScript() << OP_TRUE);

    CBlock wrong_codec;
    wrong_codec.nVersion = 4;
    wrong_codec.hashPrevBlock = genesis->GetBlockHash();
    wrong_codec.nTime = coinbase.nTime;
    wrong_codec.nBits = genesis->nBits;
    wrong_codec.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    wrong_codec.hashMerkleRoot = BlockMerkleRoot(wrong_codec);
    BOOST_REQUIRE(wrong_codec.GetMarkerHash(consensus) !=
                  wrong_codec.GetHash(consensus, 1));

    m_node.validation_signals->RegisterValidationInterface(&peerman);
    ProcessB3BlockMessage(connman, *node, wrong_codec);
    BOOST_CHECK(!node->fDisconnect); // Applied on the next send pass.
    BOOST_CHECK(peerman.SendMessages(*node));
    BOOST_CHECK(node->fDisconnect);
    m_node.validation_signals->UnregisterValidationInterface(&peerman);

    peerman.FinalizeNode(*node);
    connman.ClearTestNodes();
}

BOOST_AUTO_TEST_CASE(unknown_parent_block_requests_headers_with_peer_rate_limit)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    ConnmanTestMsg& connman = static_cast<ConnmanTestMsg&>(*m_node.connman);
    PeerManager& peerman = *m_node.peerman;

    const auto t0{std::chrono::seconds{1'700'000'000}};
    SetMockTime(t0);
    auto node{MakeNode(0)};
    connman.Handshake(*node,
                      /*successfully_connected=*/true,
                      /*remote_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*local_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*version=*/B3_MODERN_PROTOCOL_VERSION,
                      /*relay_txs=*/true);
    BOOST_REQUIRE(!node->fDisconnect);
    (void)DrainSentMessages(*node); // Initial synchronization request.

    const Consensus::Params& consensus{Params().GetConsensus()};
    BOOST_REQUIRE(consensus.modern_pos.has_value());
    BOOST_REQUIRE(consensus.transition_pow_bits.has_value());

    // Advance through the two-block transition corridor so the next valid
    // height is genuinely Modern PoS, not merely modern wire format.
    const CBlockIndex* genesis{
        WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip())};
    BOOST_REQUIRE(genesis);
    CBlock corridor1{BuildMarkerModernBlock(
        genesis->GetBlockHash(), /*height=*/1,
        static_cast<uint32_t>(genesis->GetBlockTime() +
                              consensus.transition_pow_min_spacing),
        *consensus.transition_pow_bits)};
    while (!CheckTransitionPowEligibility(corridor1)) ++corridor1.nNonce;
    ProcessB3BlockMessage(connman, *node, corridor1);
    CBlock corridor2{BuildMarkerModernBlock(
        corridor1.GetHash(consensus, 1), /*height=*/2,
        static_cast<uint32_t>(corridor1.GetBlockTime() +
                              consensus.transition_pow_min_spacing),
        *consensus.transition_pow_bits)};
    while (!CheckTransitionPowEligibility(corridor2)) ++corridor2.nNonce;
    ProcessB3BlockMessage(connman, *node, corridor2);
    BOOST_REQUIRE_EQUAL(
        WITH_LOCK(cs_main,
                  return m_node.chainman->ActiveChain().Tip()->nHeight),
        2);
    BOOST_CHECK(Consensus::GetConsensusPhase(3, consensus) ==
                Consensus::ConsensusPhase::MODERN_POS);
    (void)DrainSentMessages(*node);

    const uint32_t sentinel_bits{consensus.modern_pos->sentinel_bits};

    // A marker-modern full block with a modern-PoS signature shape is safe to
    // check context-free even though its parent (and therefore height) is not
    // known. The sender is not a legacy download owner in this post-boundary
    // fixture, but must still receive a recovery request.
    SetMockTime(t0 + 121s);
    CBlock orphan{BuildMarkerModernBlock(
        BlockHashFromInt(10'000), /*height=*/4,
        static_cast<uint32_t>(t0.count()),
        sentinel_bits)};
    orphan.vchBlockSig.assign(modern::MODERN_POS_SIG_SIZE, 0x01);
    ProcessB3BlockMessage(connman, *node, orphan);
    BOOST_CHECK_EQUAL(
        CountType(DrainSentMessages(*node), NetMsgType::GETHEADERS), 1U);
    BOOST_CHECK(!node->fDisconnect);

    // A second orphan cannot amplify recovery traffic inside the outstanding
    // response window.
    orphan.hashPrevBlock = BlockHashFromInt(10'001);
    ++orphan.nNonce;
    ProcessB3BlockMessage(connman, *node, orphan);
    BOOST_CHECK_EQUAL(
        CountType(DrainSentMessages(*node), NetMsgType::GETHEADERS), 0U);

    // Once the response window passes, recovery can be retried.
    SetMockTime(t0 + 242s);
    orphan.hashPrevBlock = BlockHashFromInt(10'002);
    ++orphan.nNonce;
    ProcessB3BlockMessage(connman, *node, orphan);
    BOOST_CHECK_EQUAL(
        CountType(DrainSentMessages(*node), NetMsgType::GETHEADERS), 1U);

    peerman.FinalizeNode(*node);
    SetMockTime(0s);
}

BOOST_AUTO_TEST_CASE(parent_block_drains_cached_marker_modern_descendants)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    ConnmanTestMsg& connman = static_cast<ConnmanTestMsg&>(*m_node.connman);
    PeerManager& peerman = *m_node.peerman;

    auto node{MakeNode(0)};
    connman.Handshake(*node,
                      /*successfully_connected=*/true,
                      /*remote_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*local_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                      /*version=*/B3_MODERN_PROTOCOL_VERSION,
                      /*relay_txs=*/true);
    BOOST_REQUIRE(!node->fDisconnect);
    (void)DrainSentMessages(*node);

    const Consensus::Params& consensus{Params().GetConsensus()};
    BOOST_REQUIRE(consensus.transition_pow_bits.has_value());
    const CBlockIndex* genesis{
        WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip())};
    BOOST_REQUIRE(genesis);
    BOOST_REQUIRE_EQUAL(genesis->nHeight, 0);

    CBlock parent{BuildMarkerModernBlock(
        genesis->GetBlockHash(), /*height=*/1,
        static_cast<uint32_t>(genesis->GetBlockTime() +
                              consensus.transition_pow_min_spacing),
        *consensus.transition_pow_bits)};
    while (!CheckTransitionPowEligibility(parent)) ++parent.nNonce;

    CBlock child{BuildMarkerModernBlock(
        parent.GetHash(consensus, 1), /*height=*/2,
        static_cast<uint32_t>(parent.GetBlockTime() +
                              consensus.transition_pow_min_spacing),
        *consensus.transition_pow_bits)};
    while (!CheckTransitionPowEligibility(child)) ++child.nNonce;
    const uint256 child_hash{child.GetHash(consensus, 2)};

    // The child is retained but cannot enter the block index before its
    // parent/header ancestry is known.
    ProcessB3BlockMessage(connman, *node, child);
    BOOST_CHECK(WITH_LOCK(
                    cs_main,
                    return m_node.chainman->m_blockman.LookupBlockIndex(
                               child_hash)) == nullptr);

    // A full parent arriving on the P2P path releases the cached child in the
    // same message pass; no reannouncement or second download is needed.
    ProcessB3BlockMessage(connman, *node, parent);
    {
        LOCK(cs_main);
        const CBlockIndex* child_index{
            m_node.chainman->m_blockman.LookupBlockIndex(child_hash)};
        BOOST_REQUIRE(child_index);
        BOOST_CHECK(child_index->nStatus & BLOCK_HAVE_DATA);
        BOOST_CHECK_EQUAL(m_node.chainman->ActiveChain().Tip(), child_index);
    }
    BOOST_CHECK(!node->fDisconnect);

    peerman.FinalizeNode(*node);
}

BOOST_AUTO_TEST_CASE(invalid_parent_does_not_drain_cached_descendant)
{
    ConnmanTestMsg& connman = static_cast<ConnmanTestMsg&>(*m_node.connman);
    PeerManager& peerman = *m_node.peerman;
    auto node{MakeNode(0)};

    const Consensus::Params& consensus{Params().GetConsensus()};
    BOOST_REQUIRE(consensus.transition_pow_bits.has_value());
    const CBlockIndex* genesis{
        WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip())};
    BOOST_REQUIRE(genesis);

    // One unit over the configured corridor reward passes context-free checks
    // and is stored, but fails when connection checks its coinbase amount.
    BOOST_REQUIRE(consensus.transition_pow_reward.has_value());
    CBlock invalid_parent{BuildMarkerModernBlock(
        genesis->GetBlockHash(), /*height=*/1,
        static_cast<uint32_t>(genesis->GetBlockTime() +
                              consensus.transition_pow_min_spacing),
        *consensus.transition_pow_bits,
        /*coinbase_value=*/*consensus.transition_pow_reward + 1)};
    while (!CheckTransitionPowEligibility(invalid_parent)) {
        ++invalid_parent.nNonce;
    }
    const uint256 parent_hash{invalid_parent.GetHash(consensus, 1)};

    CBlock child{BuildMarkerModernBlock(
        parent_hash, /*height=*/2,
        static_cast<uint32_t>(invalid_parent.GetBlockTime() +
                              consensus.transition_pow_min_spacing),
        *consensus.transition_pow_bits)};
    while (!CheckTransitionPowEligibility(child)) ++child.nNonce;
    const uint256 child_hash{child.GetHash(consensus, 2)};
    BlockCheckedRecorder recorder{child_hash};
    m_node.validation_signals->RegisterValidationInterface(&recorder);

    {
        LOCK(NetEventsInterface::g_msgproc_mutex);
        connman.Handshake(*node,
                          /*successfully_connected=*/true,
                          /*remote_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                          /*local_services=*/ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                          /*version=*/B3_MODERN_PROTOCOL_VERSION,
                          /*relay_txs=*/true);
        BOOST_REQUIRE(!node->fDisconnect);
        (void)DrainSentMessages(*node);

        ProcessB3BlockMessage(connman, *node, child);
        ProcessB3BlockMessage(connman, *node, invalid_parent);
    }
    m_node.validation_signals->SyncWithValidationInterfaceQueue();

    // The failed parent may be retained for diagnostics, but it must not
    // release or force-process the cached child.
    BOOST_CHECK_EQUAL(recorder.Seen(), 0);
    {
        LOCK(cs_main);
        const CBlockIndex* parent_index{
            m_node.chainman->m_blockman.LookupBlockIndex(parent_hash)};
        BOOST_REQUIRE(parent_index);
        BOOST_CHECK(parent_index->nStatus & BLOCK_HAVE_DATA);
        BOOST_CHECK(parent_index->nStatus & BLOCK_FAILED_VALID);
        BOOST_CHECK(m_node.chainman->m_blockman.LookupBlockIndex(child_hash) ==
                    nullptr);
    }

    m_node.validation_signals->UnregisterValidationInterface(&recorder);
    {
        LOCK(NetEventsInterface::g_msgproc_mutex);
        peerman.FinalizeNode(*node);
    }
}

BOOST_AUTO_TEST_SUITE_END()
