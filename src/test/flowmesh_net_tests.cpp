// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <node/flowmesh_net.h>
#include <node/flowmesh_keyfile.h>
#include <node/flowmesh_runtime.h>
#include <crypto/common.h>
#include <hash.h>
#include <logging.h>
#include <netbase.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/sock.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace {
using namespace std::chrono_literals;
using Bytes = std::vector<unsigned char>;
using Kind = flowmesh::WireMessageKind;
bool Admitted(const node::FlowMeshRelayResult& result)
{
    return std::any_of(result.peers.begin(), result.peers.end(), [](const auto& peer) {
        return peer.admission == node::FlowMeshDeliveryAdmission::ADMITTED;
    });
}
struct Feedback {
    mutable std::mutex mutex;
    std::vector<node::FlowMeshDeliveryEvent> events;
    bool Add(const node::FlowMeshDeliveryEvent& event) { std::lock_guard lock{mutex}; events.push_back(event); return true; }
    std::vector<node::FlowMeshDeliveryEvent> Events() const { std::lock_guard lock{mutex}; return events; }
};
// Stop the services before this guard is destroyed. The callback only copies
// public JSON under its own mutex; it never calls back into a service/logger.
class NetTraceCapture {
    const BCLog::CategoryMask m_mask{LogInstance().GetCategoryMask()};
    const std::unordered_map<BCLog::LogFlags, BCLog::Level> m_levels{LogInstance().CategoryLevels()};
    mutable std::mutex m_mutex;
    std::vector<UniValue> m_events;
    std::list<std::function<void(const std::string&)>>::iterator m_callback;

public:
    NetTraceCapture()
    {
        auto levels{m_levels};
        levels[BCLog::BENCH] = BCLog::Level::Debug;
        LogInstance().SetCategoryLogLevel(levels);
        LogInstance().EnableCategory(BCLog::BENCH);
        m_callback = LogInstance().PushBackCallback([this](const std::string& line) {
            constexpr std::string_view marker{"FlowMeshNetTrace "};
            const auto at{line.find(marker)};
            if (at == std::string::npos) return;
            UniValue event;
            if (!event.read(line.substr(at + marker.size())) || !event.isObject()) return;
            std::lock_guard lock{m_mutex};
            if (m_events.size() < 256) m_events.push_back(std::move(event));
        });
    }
    ~NetTraceCapture()
    {
        LogInstance().DeleteCallback(m_callback);
        LogInstance().SetCategoryLogLevel(m_levels);
        LogInstance().DisableCategory(BCLog::ALL);
        LogInstance().EnableCategory(BCLog::LogFlags{m_mask});
    }
    std::vector<UniValue> Events() const { std::lock_guard lock{m_mutex}; return m_events; }
};
struct Sink final : flowmesh::WireMessageSink {
    mutable std::mutex mutex;
    std::vector<flowmesh::QueuedWireMessage> received;
    std::vector<flowmesh::WirePeerId> connected, disconnected;
    std::map<Kind, flowmesh::QueueResult> admission;
    std::map<Kind, size_t> attempts;
    flowmesh::QueueResult EnqueueWireMessage(flowmesh::WirePeerId peer, flowmesh::WireMessage message) override
    {
        std::lock_guard lock{mutex}; ++attempts[message.kind];
        const auto it{admission.find(message.kind)};
        if (it != admission.end() && it->second != flowmesh::QueueResult::ACCEPTED) return it->second;
        received.push_back({peer, std::move(message)}); return flowmesh::QueueResult::ACCEPTED;
    }
    void FlowMeshPeerConnected(flowmesh::WirePeerId peer) override { std::lock_guard lock{mutex}; connected.push_back(peer); }
    void FlowMeshPeerDisconnected(flowmesh::WirePeerId peer) override { std::lock_guard lock{mutex}; disconnected.push_back(peer); }
    std::vector<flowmesh::QueuedWireMessage> Messages() const { std::lock_guard lock{mutex}; return received; }
    void Admit(Kind kind, flowmesh::QueueResult result) { std::lock_guard lock{mutex}; admission[kind] = result; }
    size_t Attempts(Kind kind) const { std::lock_guard lock{mutex}; const auto it{attempts.find(kind)}; return it == attempts.end() ? 0 : it->second; }
};
bool Wait(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 3000ms)
{
    const auto end{std::chrono::steady_clock::now() + timeout};
    do { if (predicate()) return true; std::this_thread::sleep_for(5ms); } while (std::chrono::steady_clock::now() < end);
    return predicate();
}
node::FlowMeshNetConfig Config(const fs::path& path)
{
    node::FlowMeshNetConfig c; c.domain = uint256::ONE; c.datadir = path; c.port = 0; return c;
}
flowmesh::WireMessage Message(Kind kind)
{
    flowmesh::WireMessage message; message.kind = kind; message.header.market_id = uint256::ONE;
    message.header.epoch = 7; message.header.sequence = 19; message.payload = {0x42};
    if (kind == Kind::ATTESTATION) message.payload.assign(flowmesh::FLOWMESH_ATTESTATION_BYTES, 0x42);
    if (kind == Kind::GET) message.payload = *flowmesh::EncodeCatchupRequest(64, flowmesh::FLOWMESH_CATCHUP_MAX_BYTES);
    if (kind == Kind::ENTRIES) message.payload = *flowmesh::EncodeCatchupEntries(std::vector<Bytes>{{0x42}});
    return message;
}
bool Transfer(Sock& socket, std::span<unsigned char> bytes, bool send)
{
    const auto end{std::chrono::steady_clock::now() + 3s}; size_t at{0};
    while (at != bytes.size() && std::chrono::steady_clock::now() < end) {
        Sock::Event events{0};
        if (!socket.Wait(10ms, send ? Sock::SEND : Sock::RECV, &events)) return false;
        if (!(events & (send ? Sock::SEND : Sock::RECV))) continue;
        const auto n{send ? socket.Send(bytes.data() + at, bytes.size() - at, MSG_NOSIGNAL) : socket.Recv(bytes.data() + at, bytes.size() - at, 0)};
        if (n == 0) return false;
        if (n < 0) { if (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINTR) continue; return false; }
        at += n;
    }
    return at == bytes.size();
}
std::unique_ptr<Sock> Connect(const std::string& endpoint)
{
    const auto address{LookupNumeric(endpoint)};
    auto sock{CreateSock(address.GetSAFamily(), SOCK_STREAM, IPPROTO_TCP)};
    if (!sock || !sock->SetNonBlocking()) return {};
    sockaddr_storage raw{}; socklen_t length{sizeof(raw)};
    if (!address.GetSockAddr(reinterpret_cast<sockaddr*>(&raw), &length)) return {};
    const auto result{sock->Connect(reinterpret_cast<sockaddr*>(&raw), length)};
    if (result && WSAGetLastError() != WSAEINPROGRESS && WSAGetLastError() != WSAEWOULDBLOCK) return {};
    Sock::Event events{0};
    if (!sock->Wait(1000ms, Sock::SEND, &events) || !(events & Sock::SEND)) return {};
    int error{0}; length = sizeof(error);
    if (sock->GetSockOpt(SOL_SOCKET, SO_ERROR, &error, &length) || error) return {};
    return sock;
}
bool Closed(Sock& socket)
{
    return Wait([&] { unsigned char byte; const auto n{socket.Recv(&byte, 1, MSG_PEEK)}; return n == 0 || (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK && WSAGetLastError() != WSAEINTR); });
}
struct RawChannel {
    std::unique_ptr<Sock> socket;
    uint256 session;
    CPubKey server;
    uint64_t counter{0};
};
Bytes Hello(const CKey& key, uint8_t channel, const uint256& domain)
{
    Bytes h(105); std::copy_n("FMN2", 4, h.begin()); WriteLE16(h.data() + 4, 2);
    std::copy(domain.begin(), domain.end(), h.begin() + 6); h[38] = channel; h[39] = 0;
    const auto pubkey{key.GetPubKey()}; std::copy(pubkey.begin(), pubkey.end(), h.begin() + 40);
    GetStrongRandBytes(std::span{h}.subspan(73)); return h;
}
bool Handshake(RawChannel& c, const std::string& address, const CKey& key, uint8_t channel)
{
    c.socket = Connect(address); if (!c.socket) return false;
    auto hello{Hello(key, channel, uint256::ONE)};
    if (!Transfer(*c.socket, hello, true)) return false;
    Bytes remote(105); if (!Transfer(*c.socket, remote, false)) return false;
    auto hash{TaggedHash("B3/FlowMeshNet/session/v2")}; hash.write(MakeByteSpan(hello)); hash.write(MakeByteSpan(remote)); c.session = hash.GetHash();
    c.server.Set(remote.begin() + 40, remote.begin() + 73);
    Bytes proof;
    if (!key.SignCompact((TaggedHash("B3/FlowMeshNet/auth/v2") << c.session << key.GetPubKey()).GetHash(), proof) || !Transfer(*c.socket, proof, true)) return false;
    Bytes remote_proof(65); if (!Transfer(*c.socket, remote_proof, false)) return false;
    CPubKey recovered;
    return recovered.RecoverCompact((TaggedHash("B3/FlowMeshNet/auth/v2") << c.session << c.server).GetHash(), remote_proof) && recovered == c.server;
}
Bytes Frame(RawChannel& c, const CKey& key, const flowmesh::WireMessage& message)
{
    flowmesh::WireCheck check; const auto inner{flowmesh::EncodeWireMessage(message, check)};
    BOOST_REQUIRE(inner);
    Bytes bytes(13 + inner->size()); WriteLE32(bytes.data(), 9 + inner->size() + 65);
    bytes[4] = static_cast<unsigned char>(message.kind); WriteLE64(bytes.data() + 5, ++c.counter);
    std::copy(inner->begin(), inner->end(), bytes.begin() + 13);
    auto hash{TaggedHash("B3/FlowMeshNet/frame/v2")}; hash << c.session << key.GetPubKey(); hash.write(MakeByteSpan(std::span{bytes}.subspan(4)));
    Bytes signature; BOOST_REQUIRE(key.SignCompact(hash.GetHash(), signature)); bytes.insert(bytes.end(), signature.begin(), signature.end()); return bytes;
}
std::optional<flowmesh::WireMessage> Receive(RawChannel& c)
{
    Bytes prefix(4); if (!Transfer(*c.socket, prefix, false)) return {};
    const auto length{ReadLE32(prefix.data())}; if (length > 4 * 1024 * 1024 + 1024 || length < 74) return {};
    Bytes body(length); if (!Transfer(*c.socket, body, false)) return {};
    auto hash{TaggedHash("B3/FlowMeshNet/frame/v2")}; hash << c.session << c.server; hash.write(MakeByteSpan(std::span{body}.first(body.size() - 65)));
    CPubKey recovered;
    if (!recovered.RecoverCompact(hash.GetHash(), Bytes{body.end() - 65, body.end()}) || recovered != c.server) return {};
    flowmesh::WireCheck check; return flowmesh::DecodeWireMessage(static_cast<Kind>(body[0]), std::span{body}.subspan(9, body.size() - 74), check);
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_net_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(receive_deadlines_distinguish_progress_from_idle_and_bound_slow_frames)
{
    using Timeout = node::FlowMeshNetReceiveTimeout;
    const auto classify{node::ClassifyFlowMeshNetReceiveTimeout};
    BOOST_CHECK(classify(500ms, 40s, true, false) == Timeout::NONE);
    BOOST_CHECK(classify(29999ms, 40s, true, false) == Timeout::NONE);
    BOOST_CHECK(classify(30s, 40s, true, false) == Timeout::IDLE);
    BOOST_CHECK(classify(1ms, 299999ms, true, false) == Timeout::NONE);
    BOOST_CHECK(classify(1ms, 5min, true, false) == Timeout::FRAME);
    BOOST_CHECK(classify(1ms, 5min, false, false) == Timeout::NONE);
    BOOST_CHECK(classify(30s, 0ms, false, false) == Timeout::IDLE);
    BOOST_CHECK(classify(30s, 0ms, false, true) == Timeout::NONE);
}

BOOST_AUTO_TEST_CASE(pinned_three_channel_round_trip_and_persistent_identity)
{
    Sink a_sink, b_sink;
    const auto a_config{Config(m_path_root / "fmnet-a")};
    node::FlowMeshNetService a{a_config, a_sink}; std::string error;
    BOOST_REQUIRE_MESSAGE(a.Start(error), error);
    const auto identity{a.Snapshot()}; BOOST_REQUIRE(!identity.operator_pubkey.empty());
    auto b_config{Config(m_path_root / "fmnet-b")}; b_config.enable_listen = false;
    b_config.peers = {identity.operator_pubkey + "@" + identity.bind_address};
    node::FlowMeshNetService b{b_config, b_sink}; BOOST_REQUIRE_MESSAGE(b.Start(error), error);
    BOOST_REQUIRE(Wait([&] { return a.Snapshot().peers.size() == 1 && b.Snapshot().peers.size() == 1 && a.Snapshot().peers[0].authenticated && b.Snapshot().peers[0].authenticated; }));
    const auto peer{b.Snapshot().peers[0]}; BOOST_CHECK(peer.live && peer.actions && peer.bulk); BOOST_CHECK(peer.id <= -2); BOOST_CHECK(peer.id != std::numeric_limits<int64_t>::min());
    const std::vector kinds{Kind::HELLO, Kind::ACTION, Kind::PROPOSAL, Kind::ATTESTATION, Kind::CERTIFICATE, Kind::GET, Kind::ENTRIES};
    for (const auto kind : kinds) { node::FlowMeshRuntimeRelay relay; relay.message = Message(kind); relay.peer = peer.id; b.Relay(relay); }
    BOOST_REQUIRE(Wait([&] { return a_sink.Messages().size() == kinds.size(); }));
    const auto received{a_sink.Messages()};
    for (const auto kind : kinds) {
        const auto item{std::find_if(received.begin(), received.end(), [&](const auto& entry) { return entry.message.kind == kind; })};
        BOOST_REQUIRE(item != received.end()); BOOST_CHECK(item->message == Message(kind)); BOOST_CHECK_EQUAL(item->peer, received.front().peer);
    }
    node::FlowMeshRuntimeRelay reply; reply.peer = received.front().peer; reply.message = Message(Kind::ENTRIES); a.Relay(reply);
    BOOST_REQUIRE(Wait([&] { return b_sink.Messages().size() == 1; })); BOOST_CHECK(b_sink.Messages()[0].message == reply.message);
    reply.peer.reset(); reply.exclude_peer = received.front().peer; a.Relay(reply);
    std::this_thread::sleep_for(50ms); BOOST_CHECK_EQUAL(b_sink.Messages().size(), 1U);
    const auto start{std::chrono::steady_clock::now()}; b.Stop(); a.Stop();
    BOOST_CHECK(std::chrono::steady_clock::now() - start < 1s);
    BOOST_REQUIRE_MESSAGE(a.Start(error), error); BOOST_CHECK_EQUAL(a.Snapshot().operator_pubkey, identity.operator_pubkey);
}

BOOST_AUTO_TEST_CASE(runtime_peer_addition_authenticates_three_channels_without_restart)
{
    Sink a_sink, b_sink;
    node::FlowMeshNetService a{Config(m_path_root / "fmnet-live-a"), a_sink};
    auto b_config{Config(m_path_root / "fmnet-live-b")}; b_config.enable_listen = false;
    node::FlowMeshNetService b{b_config, b_sink}; std::string error;
    BOOST_REQUIRE_MESSAGE(a.Start(error), error);
    BOOST_REQUIRE_MESSAGE(b.Start(error), error);
    BOOST_CHECK(b.Snapshot().targets.empty());
    const auto remote{a.Snapshot()}, local{b.Snapshot()};
    const auto endpoint{remote.operator_pubkey + "@" + remote.bind_address};
    const auto added{b.AddPeer(endpoint)};
    BOOST_REQUIRE_MESSAGE(added.accepted, added.error);
    BOOST_CHECK(!added.already_present);
    BOOST_CHECK_EQUAL(added.address, remote.bind_address);
    const auto duplicate{b.AddPeer(endpoint)};
    BOOST_CHECK(duplicate.accepted && duplicate.already_present);
    BOOST_REQUIRE(Wait([&] {
        const auto status{b.Snapshot()};
        return status.targets.size() == 1 && status.targets[0].authenticated;
    }));
    const auto status{b.Snapshot()};
    BOOST_REQUIRE_EQUAL(status.peers.size(), 1U);
    BOOST_CHECK(status.peers[0].authenticated);
    BOOST_CHECK(status.targets[0].runtime_added && status.targets[0].admitted_to_worker);
    for (const auto& channel : status.targets[0].channels) {
        BOOST_CHECK(channel.authenticated);
        BOOST_CHECK_EQUAL(channel.state, "authenticated");
        BOOST_CHECK_GE(channel.attempts, 1U);
        BOOST_CHECK_EQUAL(channel.retry_in_ms, 0);
    }
    node::FlowMeshRuntimeRelay relay; relay.message = Message(Kind::HELLO);
    BOOST_CHECK(Admitted(b.Relay(relay)));
    BOOST_REQUIRE(Wait([&] { return a_sink.Messages().size() == 1; }));
    b.Stop();
    BOOST_CHECK(!b.AddPeer(endpoint).accepted);
    BOOST_REQUIRE_MESSAGE(b.Start(error), error);
    BOOST_CHECK_EQUAL(b.Snapshot().operator_pubkey, local.operator_pubkey);
    BOOST_CHECK(b.Snapshot().targets.empty()); // runtime-only, never persisted to config
}

BOOST_AUTO_TEST_CASE(runtime_peer_addition_validates_pins_ports_duplicates_and_capacity)
{
    Sink sink; auto config{Config(m_path_root / "fmnet-live-bounds")};
    config.enable_listen = false; config.max_peers = 2;
    node::FlowMeshNetService service{config, sink}; std::string error;
    CKey first, second, third; first.MakeNewKey(true); second.MakeNewKey(true); third.MakeNewKey(true);
    const auto key1{HexStr(first.GetPubKey())}, key2{HexStr(second.GetPubKey())}, key3{HexStr(third.GetPubKey())};
    BOOST_CHECK(!service.AddPeer(key1 + "@127.0.0.1:1").accepted);
    BOOST_REQUIRE_MESSAGE(service.Start(error), error);
    for (const auto& value : std::vector<std::string>{"127.0.0.1:1", key1 + "@127.0.0.1", key1 + "@127.0.0.1:0",
             key1 + "@localhost:5649", key1 + "@127.0.0.1:65536", "bad@127.0.0.1:1",
             key1 + "\\@127.0.0.1:1", std::string(257, 'a'), service.Snapshot().operator_pubkey + "@127.0.0.1:1"}) {
        const auto result{service.AddPeer(value)};
        BOOST_CHECK_MESSAGE(!result.accepted && !result.error.empty(), value);
    }
    BOOST_CHECK(service.Snapshot().targets.empty());
    BOOST_REQUIRE(service.AddPeer(key1 + "@127.0.0.1:1").accepted);
    BOOST_CHECK(service.AddPeer(key1 + "@127.0.0.1:1").already_present);
    BOOST_CHECK(!service.AddPeer(key2 + "@127.0.0.1:1").accepted);
    BOOST_CHECK(!service.AddPeer(key1 + "@127.0.0.1:2").accepted);
    BOOST_REQUIRE(service.AddPeer(key2 + "@127.0.0.1:2").accepted);
    const auto full{service.AddPeer(key3 + "@127.0.0.1:3")};
    BOOST_CHECK(!full.accepted);
    BOOST_CHECK(full.error.find("capacity") != std::string::npos);
    BOOST_CHECK_EQUAL(service.Snapshot().targets.size(), 2U);
    std::atomic<bool> start{false};
    std::thread caller{[&] {
        while (!start.load()) std::this_thread::yield();
        for (size_t i{0}; i < 1000; ++i) (void)service.AddPeer(key1 + "@127.0.0.1:1");
    }};
    start = true; service.Stop(); caller.join();
    BOOST_CHECK(!service.Snapshot().running);
    BOOST_CHECK(!service.AddPeer(key1 + "@127.0.0.1:1").accepted);
}

BOOST_AUTO_TEST_CASE(runtime_peer_reservations_do_not_evict_existing_inbound_operator)
{
    Sink a_sink, b_sink;
    auto a_config{Config(m_path_root / "fmnet-live-inbound-a")}; a_config.max_peers = 1;
    node::FlowMeshNetService a{a_config, a_sink}; std::string error;
    BOOST_REQUIRE_MESSAGE(a.Start(error), error);
    const auto remote{a.Snapshot()};
    auto b_config{Config(m_path_root / "fmnet-live-inbound-b")};
    b_config.peers = {remote.operator_pubkey + "@" + remote.bind_address};
    node::FlowMeshNetService b{b_config, b_sink};
    BOOST_REQUIRE_MESSAGE(b.Start(error), error);
    BOOST_REQUIRE(Wait([&] { const auto s{a.Snapshot()}; return s.peers.size() == 1 && s.peers[0].authenticated; }));
    const auto original_id{a.Snapshot().peers[0].id};
    CKey unrelated; unrelated.MakeNewKey(true);
    const auto refused{a.AddPeer(HexStr(unrelated.GetPubKey()) + "@127.0.0.1:1")};
    BOOST_CHECK(!refused.accepted);
    BOOST_CHECK(refused.error.find("capacity") != std::string::npos);
    const auto b_identity{b.Snapshot()};
    BOOST_REQUIRE(a.AddPeer(b_identity.operator_pubkey + "@" + b_identity.bind_address).accepted);
    BOOST_REQUIRE(Wait([&] { const auto s{a.Snapshot()}; return s.targets.size() == 1 && s.targets[0].authenticated; }));
    BOOST_CHECK_EQUAL(a.Snapshot().peers[0].id, original_id);
    BOOST_CHECK(a.Snapshot().peers[0].authenticated);
}

BOOST_AUTO_TEST_CASE(runtime_peer_concurrent_additions_keep_the_target_bound)
{
    Sink sink; auto config{Config(m_path_root / "fmnet-live-concurrent")};
    config.enable_listen = false; config.max_peers = 4;
    node::FlowMeshNetService service{config, sink}; std::string error;
    BOOST_REQUIRE_MESSAGE(service.Start(error), error);
    std::array<std::string, 8> endpoints;
    for (size_t i{0}; i < endpoints.size(); ++i) {
        CKey key; key.MakeNewKey(true);
        endpoints[i] = HexStr(key.GetPubKey()) + "@127.0.0.1:" + std::to_string(i + 1);
    }
    std::atomic<bool> start{false}; std::atomic<size_t> accepted{0};
    std::vector<std::thread> callers;
    for (const auto& endpoint : endpoints) callers.emplace_back([&, endpoint] {
        while (!start.load()) std::this_thread::yield();
        if (service.AddPeer(endpoint).accepted) ++accepted;
    });
    start = true;
    for (auto& caller : callers) caller.join();
    BOOST_CHECK_EQUAL(accepted.load(), config.max_peers);
    BOOST_CHECK_EQUAL(service.Snapshot().targets.size(), config.max_peers);
    BOOST_REQUIRE(Wait([&] {
        const auto status{service.Snapshot()};
        return std::all_of(status.targets.begin(), status.targets.end(), [](const auto& t) { return t.admitted_to_worker; });
    }));
}

BOOST_AUTO_TEST_CASE(runtime_peer_reports_handshake_timeout_separately_from_connect_refusal)
{
    auto listener{CreateSock(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    BOOST_REQUIRE(listener); BOOST_REQUIRE(listener->SetNonBlocking());
    const auto endpoint{LookupNumeric("127.0.0.1", 0)};
    sockaddr_storage raw{}; socklen_t length{sizeof(raw)};
    BOOST_REQUIRE(endpoint.GetSockAddr(reinterpret_cast<sockaddr*>(&raw), &length));
    BOOST_REQUIRE_EQUAL(listener->Bind(reinterpret_cast<sockaddr*>(&raw), length), 0);
    BOOST_REQUIRE_EQUAL(listener->Listen(4), 0);
    length = sizeof(raw); BOOST_REQUIRE_EQUAL(listener->GetSockName(reinterpret_cast<sockaddr*>(&raw), &length), 0);
    CService bound; BOOST_REQUIRE(bound.SetSockAddr(reinterpret_cast<sockaddr*>(&raw), length));
    Sink sink; auto config{Config(m_path_root / "fmnet-live-handshake-timeout")}; config.enable_listen = false;
    node::FlowMeshNetService service{config, sink}; std::string error;
    BOOST_REQUIRE_MESSAGE(service.Start(error), error);
    CKey expected; expected.MakeNewKey(true);
    BOOST_REQUIRE(service.AddPeer(HexStr(expected.GetPubKey()) + "@" + bound.ToStringAddrPort()).accepted);
    std::vector<std::unique_ptr<Sock>> accepted;
    BOOST_REQUIRE(Wait([&] {
        length = sizeof(raw); auto socket{listener->Accept(reinterpret_cast<sockaddr*>(&raw), &length)};
        if (socket) accepted.push_back(std::move(socket));
        return accepted.size() == 3;
    }));
    // Real TCP connections are accepted, but no operator hello is returned.
    BOOST_REQUIRE(Wait([&] {
        const auto status{service.Snapshot()};
        return std::any_of(status.targets[0].channels.begin(), status.targets[0].channels.end(), [](const auto& channel) {
            return channel.last_error == "operator handshake deadline exceeded (five seconds)" && channel.failures != 0;
        });
    }, 7000ms));
    BOOST_CHECK(service.Snapshot().peers.empty());
    BOOST_CHECK(!service.Snapshot().targets[0].authenticated);
}

BOOST_AUTO_TEST_CASE(runtime_peer_wrong_pin_is_reported_and_retried_without_authentication)
{
    Sink a_sink, b_sink;
    node::FlowMeshNetService a{Config(m_path_root / "fmnet-live-wrong-a"), a_sink};
    auto b_config{Config(m_path_root / "fmnet-live-wrong-b")}; b_config.enable_listen = false;
    node::FlowMeshNetService b{b_config, b_sink}; std::string error;
    BOOST_REQUIRE_MESSAGE(a.Start(error), error);
    BOOST_REQUIRE_MESSAGE(b.Start(error), error);
    CKey wrong; wrong.MakeNewKey(true);
    BOOST_REQUIRE(b.AddPeer(HexStr(wrong.GetPubKey()) + "@" + a.Snapshot().bind_address).accepted);
    BOOST_REQUIRE(Wait([&] {
        const auto s{b.Snapshot()};
        return !s.targets.empty() && std::any_of(s.targets[0].channels.begin(), s.targets[0].channels.end(), [](const auto& c) {
            return c.failures != 0 && c.last_error == "remote operator public key does not match configured pin";
        });
    }));
    BOOST_REQUIRE(Wait([&] {
        const auto s{b.Snapshot()};
        return std::any_of(s.targets[0].channels.begin(), s.targets[0].channels.end(), [](const auto& c) { return c.attempts >= 2; });
    }, 5000ms));
    BOOST_CHECK(b.Snapshot().peers.empty());
    BOOST_CHECK(!b.Snapshot().targets[0].authenticated);
}

BOOST_AUTO_TEST_CASE(runtime_peer_reports_refusal_then_connects_when_listener_returns)
{
    Sink a_sink, b_sink; auto a_config{Config(m_path_root / "fmnet-live-return-a")};
    node::FlowMeshNetService a{a_config, a_sink}; std::string error;
    BOOST_REQUIRE_MESSAGE(a.Start(error), error);
    const auto remote{a.Snapshot()}; a.Stop();
    auto b_config{Config(m_path_root / "fmnet-live-return-b")}; b_config.enable_listen = false;
    node::FlowMeshNetService b{b_config, b_sink}; BOOST_REQUIRE_MESSAGE(b.Start(error), error);
    BOOST_REQUIRE(b.AddPeer(remote.operator_pubkey + "@" + remote.bind_address).accepted);
    BOOST_REQUIRE(Wait([&] { const auto s{b.Snapshot()}; return s.targets[0].channels[0].failures != 0; }));
    const auto refused{b.Snapshot().targets[0].channels[0]};
    BOOST_CHECK(!refused.authenticated);
    BOOST_CHECK(!refused.last_error.empty());
    a_config.port = LookupNumeric(remote.bind_address).GetPort();
    node::FlowMeshNetService returning{a_config, a_sink}; BOOST_REQUIRE_MESSAGE(returning.Start(error), error);
    BOOST_REQUIRE(Wait([&] { return b.Snapshot().targets[0].authenticated; }, 6000ms));
    BOOST_CHECK_GE(b.Snapshot().targets[0].channels[0].attempts, 2U);
    BOOST_CHECK_GE(b.Snapshot().targets[0].channels[0].failures, 1U);
}

BOOST_AUTO_TEST_CASE(operator_key_creation_never_overwrites_existing_or_partial_identity)
{
    Sink sink;
    auto config{Config(m_path_root / "fmnet-key-preserve")};
    config.enable_listen = false;
    node::FlowMeshNetService service{config, sink};
    std::string error;
    BOOST_REQUIRE_MESSAGE(service.Start(error), error);
    const auto identity{service.Snapshot().operator_pubkey};
    BOOST_REQUIRE(!identity.empty());
    service.Stop();
    const auto path{config.datadir / "operator.key"};
    FILE* duplicate{node::detail::OpenFlowMeshKeyFile(path.std_path(), true)};
    if (duplicate) std::fclose(duplicate);
    BOOST_REQUIRE(!duplicate);
    BOOST_REQUIRE_MESSAGE(service.Start(error), error);
    BOOST_CHECK_EQUAL(service.Snapshot().operator_pubkey, identity);
    service.Stop();

    auto partial_config{Config(m_path_root / "fmnet-key-partial")};
    partial_config.enable_listen = false;
    fs::create_directories(partial_config.datadir);
    const auto partial_path{partial_config.datadir / "operator.key"};
    FILE* partial{node::detail::OpenFlowMeshKeyFile(partial_path.std_path(), true)};
    BOOST_REQUIRE(partial);
    BOOST_REQUIRE_EQUAL(std::fclose(partial), 0);
    node::FlowMeshNetService partial_service{partial_config, sink};
    BOOST_CHECK(!partial_service.Start(error));
    BOOST_CHECK_EQUAL(error, "Invalid FlowMesh operator key; refusing replacement");
    BOOST_CHECK(fs::exists(partial_path));
    BOOST_CHECK_EQUAL(fs::file_size(partial_path), 0U);
}

BOOST_AUTO_TEST_CASE(bench_trace_round_trip_correlates_exact_inner_wire_without_changing_delivery)
{
    NetTraceCapture capture;
    Sink a_sink, b_sink;
    node::FlowMeshNetService a{Config(m_path_root / "fmnet-trace-a"), a_sink};
    std::string error;
    BOOST_REQUIRE_MESSAGE(a.Start(error), error);
    const auto identity{a.Snapshot()};
    auto b_config{Config(m_path_root / "fmnet-trace-b")};
    b_config.enable_listen = false;
    b_config.peers = {identity.operator_pubkey + "@" + identity.bind_address};
    node::FlowMeshNetService b{b_config, b_sink};
    BOOST_REQUIRE_MESSAGE(b.Start(error), error);
    BOOST_REQUIRE(Wait([&] {
        const auto left{a.Snapshot()}, right{b.Snapshot()};
        return left.peers.size() == 1 && right.peers.size() == 1 &&
               left.peers[0].authenticated && right.peers[0].authenticated;
    }));

    // This tests authenticated transport framing, not application/BLS validity.
    const auto check_trace = [&](const flowmesh::WireMessage& message, uint64_t delivery_id) {
        flowmesh::WireCheck check;
        const auto encoded{flowmesh::EncodeWireMessage(message, check)};
        BOOST_REQUIRE(encoded);
        const auto expected_hash{Hash(*encoded).GetHex()};
        const auto command{std::string{flowmesh::WireCommand(message.kind)}};
        const std::array<std::string, 4> stages{
            "queued", "socket_written", "transport_authenticated_receive", "runtime_ingress_result"};
        BOOST_REQUIRE(Wait([&] {
            const auto events{capture.Events()};
            return std::all_of(stages.begin(), stages.end(), [&](const auto& stage) {
                return std::any_of(events.begin(), events.end(), [&](const auto& event) {
                    return event["wire_hash"].isStr() && event["kind"].isStr() && event["stage"].isStr() &&
                           event["wire_hash"].get_str() == expected_hash &&
                           event["kind"].get_str() == command && event["stage"].get_str() == stage;
                });
            });
        }));
        std::map<std::string, int64_t> times;
        for (const auto& event : capture.Events()) {
            if (!event["wire_hash"].isStr() || !event["kind"].isStr()) continue;
            if (event["wire_hash"].get_str() != expected_hash || event["kind"].get_str() != command) continue;
            const auto stage{event["stage"].get_str()};
            BOOST_CHECK_EQUAL(event["market_id"].get_str(), message.header.market_id.GetHex());
            BOOST_CHECK_EQUAL(event["epoch"].getInt<uint64_t>(), message.header.epoch);
            BOOST_CHECK_EQUAL(event["sequence"].getInt<uint64_t>(), message.header.sequence);
            const auto at{event["monotonic_us"].getInt<int64_t>()};
            BOOST_CHECK_GT(at, 0);
            times.emplace(stage, at);
            const bool outgoing{stage == "queued" || stage == "socket_written"};
            BOOST_CHECK_EQUAL(event["delivery_id"].getInt<uint64_t>(), outgoing ? delivery_id : 0);
            if (stage == "runtime_ingress_result") BOOST_CHECK_EQUAL(event["reason"].get_str(), "accepted");
            // Payloads, signing material and account contents are not logged.
            for (const auto* field : {"payload", "private_key", "secret", "signature"}) BOOST_CHECK(event[field].isNull());
        }
        // The receiver records authentication before returning its local sink
        // admission result. Sender queue logging is deliberately post-unlock,
        // so do not invent queued-before-socket ordering across those threads.
        BOOST_CHECK_LE(times.at("transport_authenticated_receive"), times.at("runtime_ingress_result"));
    };

    node::FlowMeshRuntimeRelay outgoing;
    outgoing.message = Message(Kind::PROPOSAL);
    outgoing.peer = b.Snapshot().peers[0].id;
    outgoing.delivery_id = 41;
    BOOST_REQUIRE(Admitted(b.Relay(outgoing)));
    BOOST_REQUIRE(Wait([&] { return a_sink.Messages().size() == 1; }));
    BOOST_CHECK(a_sink.Messages()[0].message == outgoing.message);
    check_trace(outgoing.message, outgoing.delivery_id);

    node::FlowMeshRuntimeRelay response;
    response.message = Message(Kind::ENTRIES);
    response.peer = a.Snapshot().peers[0].id;
    response.delivery_id = 42;
    BOOST_REQUIRE(Admitted(a.Relay(response)));
    BOOST_REQUIRE(Wait([&] { return b_sink.Messages().size() == 1; }));
    BOOST_CHECK(b_sink.Messages()[0].message == response.message);
    check_trace(response.message, response.delivery_id);
    b.Stop();
    a.Stop();
    BOOST_CHECK_EQUAL(a_sink.Messages().size(), 1U);
    BOOST_CHECK_EQUAL(b_sink.Messages().size(), 1U);
}

BOOST_AUTO_TEST_CASE(unpaired_critical_bytes_wait_for_all_channels_then_replay_disconnects)
{
    Sink sink; node::FlowMeshNetService server{Config(m_path_root / "fmnet-pair"), sink}; std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error); const auto address{server.Snapshot().bind_address};
    CKey key; key.MakeNewKey(true); RawChannel live, actions, bulk;
    BOOST_REQUIRE(Handshake(live, address, key, 0)); BOOST_REQUIRE(Handshake(actions, address, key, 1));
    auto frame{Frame(live, key, Message(Kind::PROPOSAL))}; BOOST_REQUIRE(Transfer(*live.socket, frame, true));
    std::this_thread::sleep_for(30ms); BOOST_CHECK(sink.Messages().empty());
    BOOST_REQUIRE(Handshake(bulk, address, key, 2));
    BOOST_REQUIRE(Wait([&] { return sink.Messages().size() == 1; })); BOOST_CHECK(sink.Messages()[0].message == Message(Kind::PROPOSAL));
    BOOST_REQUIRE(Transfer(*live.socket, frame, true)); BOOST_REQUIRE(Closed(*live.socket));
    BOOST_REQUIRE(Wait([&] { return server.Snapshot().peers.empty(); })); BOOST_CHECK_EQUAL(sink.Messages().size(), 1U);
}

BOOST_AUTO_TEST_CASE(tampered_frame_and_cross_channel_data_never_reach_sink)
{
    Sink sink; node::FlowMeshNetService server{Config(m_path_root / "fmnet-tamper"), sink}; std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error); const auto address{server.Snapshot().bind_address};
    for (const bool cross_channel : {false, true}) {
        CKey key; key.MakeNewKey(true); RawChannel live, actions, bulk;
        BOOST_REQUIRE(Handshake(live, address, key, 0)); BOOST_REQUIRE(Handshake(actions, address, key, 1)); BOOST_REQUIRE(Handshake(bulk, address, key, 2));
        BOOST_REQUIRE(Wait([&] { return !server.Snapshot().peers.empty() && server.Snapshot().peers[0].authenticated; }));
        auto frame{Frame(live, key, Message(cross_channel ? Kind::GET : Kind::PROPOSAL))};
        if (!cross_channel) frame.back() ^= 1;
        BOOST_REQUIRE(Transfer(*live.socket, frame, true)); BOOST_REQUIRE(Closed(*live.socket));
        BOOST_REQUIRE(Wait([&] { return server.Snapshot().peers.empty(); }));
    }
    BOOST_CHECK(sink.Messages().empty());
}

BOOST_AUTO_TEST_CASE(wrong_domain_and_oversized_frame_fail_closed)
{
    Sink sink; node::FlowMeshNetService server{Config(m_path_root / "fmnet-invalid"), sink}; std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error); const auto address{server.Snapshot().bind_address}; CKey key; key.MakeNewKey(true);
    auto sock{Connect(address)}; BOOST_REQUIRE(sock); auto hello{Hello(key, 0, uint256{})};
    BOOST_REQUIRE(Transfer(*sock, hello, true)); BOOST_REQUIRE(Closed(*sock));
    auto old_socket{Connect(address)}; BOOST_REQUIRE(old_socket); auto old_hello{Hello(key, 0, uint256::ONE)};
    std::copy_n("FMN1", 4, old_hello.begin()); WriteLE16(old_hello.data() + 4, 1);
    BOOST_REQUIRE(Transfer(*old_socket, old_hello, true)); BOOST_REQUIRE(Closed(*old_socket));
    RawChannel live, actions, bulk; BOOST_REQUIRE(Handshake(live, address, key, 0)); BOOST_REQUIRE(Handshake(actions, address, key, 1)); BOOST_REQUIRE(Handshake(bulk, address, key, 2));
    Bytes oversized(13); WriteLE32(oversized.data(), UINT32_MAX); oversized[4] = static_cast<uint8_t>(Kind::PROPOSAL); WriteLE64(oversized.data() + 5, 1);
    BOOST_REQUIRE(Transfer(*live.socket, oversized, true)); BOOST_REQUIRE(Closed(*live.socket)); BOOST_CHECK(sink.Messages().empty());
}

BOOST_AUTO_TEST_CASE(outbound_pin_mismatch_closes_before_sending_authentication)
{
    const auto endpoint{LookupNumeric("127.0.0.1", 0)};
    auto listener{CreateSock(AF_INET, SOCK_STREAM, IPPROTO_TCP)}; BOOST_REQUIRE(listener); BOOST_REQUIRE(listener->SetNonBlocking());
    sockaddr_storage raw{}; socklen_t length{sizeof(raw)}; BOOST_REQUIRE(endpoint.GetSockAddr(reinterpret_cast<sockaddr*>(&raw), &length));
    BOOST_REQUIRE_EQUAL(listener->Bind(reinterpret_cast<sockaddr*>(&raw), length), 0); BOOST_REQUIRE_EQUAL(listener->Listen(4), 0);
    length = sizeof(raw); BOOST_REQUIRE_EQUAL(listener->GetSockName(reinterpret_cast<sockaddr*>(&raw), &length), 0);
    CService bound; BOOST_REQUIRE(bound.SetSockAddr(reinterpret_cast<sockaddr*>(&raw), length));
    CKey expected, impostor; expected.MakeNewKey(true); impostor.MakeNewKey(true);
    Sink sink; auto config{Config(m_path_root / "fmnet-pin")}; config.enable_listen = false;
    config.peers = {HexStr(expected.GetPubKey()) + "@" + bound.ToStringAddrPort()};
    node::FlowMeshNetService client{config, sink}; std::string error; BOOST_REQUIRE_MESSAGE(client.Start(error), error);
    std::unique_ptr<Sock> accepted;
    BOOST_REQUIRE(Wait([&] { length = sizeof(raw); accepted = listener->Accept(reinterpret_cast<sockaddr*>(&raw), &length); return accepted != nullptr; }));
    BOOST_REQUIRE(accepted->SetNonBlocking());
    Bytes client_hello(105); BOOST_REQUIRE(Transfer(*accepted, client_hello, false));
    auto server_hello{Hello(impostor, client_hello[38], uint256::ONE)};
    BOOST_REQUIRE(Transfer(*accepted, server_hello, true));
    // We read the client's actual HELLO. EOF rather than any AUTH bytes proves
    // this attempt processed and rejected the mismatched configured key.
    BOOST_REQUIRE(Closed(*accepted)); BOOST_CHECK(sink.Messages().empty()); BOOST_CHECK(client.Snapshot().peers.empty());
}

BOOST_AUTO_TEST_CASE(old_session_frame_fails_with_fresh_counters_and_same_operator)
{
    Sink sink; node::FlowMeshNetService server{Config(m_path_root / "fmnet-session"), sink}; std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error); const auto address{server.Snapshot().bind_address}; CKey key; key.MakeNewKey(true);
    Bytes old_frame;
    {
        RawChannel live, actions, bulk; BOOST_REQUIRE(Handshake(live, address, key, 0)); BOOST_REQUIRE(Handshake(actions, address, key, 1)); BOOST_REQUIRE(Handshake(bulk, address, key, 2));
        old_frame = Frame(live, key, Message(Kind::PROPOSAL)); BOOST_REQUIRE(Transfer(*live.socket, old_frame, true));
        BOOST_REQUIRE(Wait([&] { return sink.Messages().size() == 1; }));
    }
    BOOST_REQUIRE(Wait([&] { return server.Snapshot().peers.empty(); }));
    RawChannel live, actions, bulk; BOOST_REQUIRE(Handshake(live, address, key, 0)); BOOST_REQUIRE(Handshake(actions, address, key, 1)); BOOST_REQUIRE(Handshake(bulk, address, key, 2));
    // Counter 1 is fresh on this socket, but its signature binds the old nonce transcript.
    BOOST_REQUIRE(Transfer(*live.socket, old_frame, true)); BOOST_REQUIRE(Closed(*live.socket));
    BOOST_CHECK_EQUAL(sink.Messages().size(), 1U);
}

BOOST_AUTO_TEST_CASE(unread_bulk_does_not_block_critical_live_and_queues_are_bounded)
{
    Sink sink; node::FlowMeshNetService server{Config(m_path_root / "fmnet-bulk"), sink}; std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error); const auto address{server.Snapshot().bind_address}; CKey key; key.MakeNewKey(true); RawChannel live, actions, bulk;
    BOOST_REQUIRE(Handshake(live, address, key, 0)); BOOST_REQUIRE(Handshake(actions, address, key, 1)); BOOST_REQUIRE(Handshake(bulk, address, key, 2));
    BOOST_REQUIRE(Wait([&] { return !server.Snapshot().peers.empty() && server.Snapshot().peers[0].authenticated; }));
    node::FlowMeshRuntimeRelay history; history.message = Message(Kind::ENTRIES);
    history.message.payload = *flowmesh::EncodeCatchupEntries(std::vector<Bytes>{Bytes(1024 * 1024, 0x42)});
    for (size_t i{0}; i < 64; ++i) server.Relay(history);
    node::FlowMeshRuntimeRelay critical; critical.message = Message(Kind::CERTIFICATE); BOOST_CHECK(Admitted(server.Relay(critical)));
    const auto received{Receive(live)}; BOOST_REQUIRE(received); BOOST_CHECK(*received == critical.message);
    BOOST_REQUIRE(Wait([&] { return !server.Snapshot().peers.empty(); }));
    BOOST_CHECK(server.Snapshot().peers[0].queued_bytes <= 32 * 1024 * 1024);
    BOOST_CHECK_GT(server.Snapshot().egress_rejected_messages, 0U);
    BOOST_CHECK(!server.Snapshot().last_egress_error.empty());
}

BOOST_AUTO_TEST_CASE(rejected_ingress_is_retained_and_retried_once_per_admission)
{
    using Result = flowmesh::QueueResult;
    for (const auto rejection : {Result::GLOBAL_LIMIT, Result::RATE_LIMITED, Result::PEER_LIMIT, Result::MARKET_LIMIT, Result::RECONCILING, Result::STOPPED}) {
        Sink sink; sink.Admit(Kind::PROPOSAL, rejection);
        node::FlowMeshNetService server{Config(m_path_root / fs::PathFromString("fmnet-retry-" + std::to_string(static_cast<int>(rejection)))), sink};
        std::string error; BOOST_REQUIRE_MESSAGE(server.Start(error), error);
        CKey key; key.MakeNewKey(true); RawChannel live, actions, bulk;
        BOOST_REQUIRE(Handshake(live, server.Snapshot().bind_address, key, 0)); BOOST_REQUIRE(Handshake(actions, server.Snapshot().bind_address, key, 1));
        BOOST_REQUIRE(Handshake(bulk, server.Snapshot().bind_address, key, 2));
        const auto proposal{Message(Kind::PROPOSAL)}, vote{Message(Kind::ATTESTATION)};
        auto first{Frame(live, key, proposal)}, second{Frame(live, key, vote)};
        BOOST_REQUIRE(Transfer(*live.socket, first, true)); BOOST_REQUIRE(Transfer(*live.socket, second, true));
        BOOST_REQUIRE(Wait([&] {
            const auto status{server.Snapshot()};
            return sink.Attempts(Kind::PROPOSAL) >= 2 && status.pending_ingress_bytes == proposal.MemoryUsage() &&
                status.peers.size() == 1 && status.peers[0].ingress_retries >= 2;
        }));
        BOOST_CHECK(sink.Messages().empty());
        // No second frame is read from this channel while the first is pending.
        BOOST_CHECK_EQUAL(sink.Attempts(Kind::ATTESTATION), 0U);
        auto waiting{server.Snapshot()}; BOOST_REQUIRE_EQUAL(waiting.peers.size(), 1U);
        BOOST_CHECK_GE(waiting.peers[0].ingress_retries, 2U);
        BOOST_CHECK(!waiting.peers[0].last_ingress_retry.empty());
        BOOST_CHECK_EQUAL(waiting.ingress_discarded_messages, 0U);
        sink.Admit(Kind::PROPOSAL, Result::ACCEPTED);
        BOOST_REQUIRE(Wait([&] { return sink.Messages().size() == 2 && server.Snapshot().pending_ingress_bytes == 0; }));
        const auto messages{sink.Messages()};
        BOOST_CHECK(messages[0].message == proposal); BOOST_CHECK(messages[1].message == vote);
        BOOST_CHECK_EQUAL(messages[0].peer, messages[1].peer);
        BOOST_CHECK_EQUAL(server.Snapshot().peers[0].received_messages, 2U);
        BOOST_CHECK_EQUAL(server.Snapshot().ingress_discarded_messages, 0U);
    }
}

BOOST_AUTO_TEST_CASE(bulk_admission_backpressure_does_not_block_live_ingress)
{
    Sink sink; sink.Admit(Kind::ENTRIES, flowmesh::QueueResult::GLOBAL_LIMIT);
    node::FlowMeshNetService server{Config(m_path_root / "fmnet-ingress-bulk"), sink}; std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    CKey key; key.MakeNewKey(true); RawChannel live, actions, bulk;
    BOOST_REQUIRE(Handshake(live, server.Snapshot().bind_address, key, 0)); BOOST_REQUIRE(Handshake(actions, server.Snapshot().bind_address, key, 1));
    BOOST_REQUIRE(Handshake(bulk, server.Snapshot().bind_address, key, 2));
    const auto entries{Message(Kind::ENTRIES)}, vote{Message(Kind::ATTESTATION)};
    auto history{Frame(bulk, key, entries)}; BOOST_REQUIRE(Transfer(*bulk.socket, history, true));
    BOOST_REQUIRE(Wait([&] { return server.Snapshot().pending_ingress_bytes == entries.MemoryUsage(); }));
    auto critical{Frame(live, key, vote)}; BOOST_REQUIRE(Transfer(*live.socket, critical, true));
    BOOST_REQUIRE(Wait([&] { return sink.Messages().size() == 1; })); BOOST_CHECK(sink.Messages()[0].message == vote);
    sink.Admit(Kind::ENTRIES, flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(Wait([&] { return sink.Messages().size() == 2 && server.Snapshot().pending_ingress_bytes == 0; }));
    BOOST_CHECK(sink.Messages()[1].message == entries);
}

BOOST_AUTO_TEST_CASE(action_admission_backpressure_does_not_block_votes_or_certificates)
{
    Sink sink; sink.Admit(Kind::ACTION, flowmesh::QueueResult::RECONCILING);
    node::FlowMeshNetService server{Config(m_path_root / "fmnet-action-ingress"), sink}; std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    CKey key; key.MakeNewKey(true); RawChannel live, actions, bulk;
    const auto address{server.Snapshot().bind_address};
    BOOST_REQUIRE(Handshake(live, address, key, 0)); BOOST_REQUIRE(Handshake(actions, address, key, 1));
    BOOST_REQUIRE(Handshake(bulk, address, key, 2));
    const auto action{Message(Kind::ACTION)};
    auto action_frame{Frame(actions, key, action)}; BOOST_REQUIRE(Transfer(*actions.socket, action_frame, true));
    BOOST_REQUIRE(Wait([&] { return server.Snapshot().pending_ingress_bytes == action.MemoryUsage(); }));
    for (const auto kind : {Kind::ATTESTATION, Kind::CERTIFICATE}) {
        auto critical{Frame(live, key, Message(kind))}; BOOST_REQUIRE(Transfer(*live.socket, critical, true));
    }
    BOOST_REQUIRE(Wait([&] { return sink.Messages().size() == 2; }));
    BOOST_CHECK(sink.Messages()[0].message.kind == Kind::ATTESTATION);
    BOOST_CHECK(sink.Messages()[1].message.kind == Kind::CERTIFICATE);
    BOOST_CHECK_EQUAL(server.Snapshot().traffic[1].received, 1U);
    BOOST_CHECK_GE(server.Snapshot().traffic[1].ingress_retries, 1U);
    sink.Admit(Kind::ACTION, flowmesh::QueueResult::ACCEPTED);
    BOOST_REQUIRE(Wait([&] { return sink.Messages().size() == 3 && server.Snapshot().pending_ingress_bytes == 0; }));
    BOOST_CHECK(sink.Messages()[2].message == action);
}

BOOST_AUTO_TEST_CASE(slow_action_socket_has_explicit_per_peer_bounds_and_cannot_block_critical)
{
    Sink sink; Feedback feedback; auto config{Config(m_path_root / "fmnet-action-egress")};
    config.delivery_callback = [&](const auto& event) { return feedback.Add(event); };
    node::FlowMeshNetService server{config, sink}; std::string error; BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    CKey key; key.MakeNewKey(true); RawChannel live, actions, bulk;
    const auto address{server.Snapshot().bind_address};
    BOOST_REQUIRE(Handshake(live, address, key, 0)); BOOST_REQUIRE(Handshake(actions, address, key, 1));
    BOOST_REQUIRE(Handshake(bulk, address, key, 2));
    const int receive_buffer{4096};
    BOOST_REQUIRE_EQUAL(actions.socket->SetSockOpt(SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)), 0);
    BOOST_REQUIRE(Wait([&] { const auto status{server.Snapshot()}; return status.peers.size() == 1 && status.peers[0].authenticated; }));
    node::FlowMeshRuntimeRelay action; action.message = Message(Kind::ACTION);
    action.message.payload.assign(flowmesh::FLOWMESH_ACTION_MAX_BYTES, 0x42);
    size_t admitted{0}, full{0};
    for (uint64_t id{1}; id <= 4096; ++id) {
        action.delivery_id = id; const auto result{server.Relay(action)}; BOOST_REQUIRE_EQUAL(result.peers.size(), 1U);
        if (result.peers[0].admission == node::FlowMeshDeliveryAdmission::ADMITTED) ++admitted;
        else { BOOST_CHECK(result.peers[0].admission == node::FlowMeshDeliveryAdmission::FULL); ++full; }
    }
    BOOST_CHECK_GT(admitted, 0U); BOOST_CHECK_GT(full, 0U);
    const auto queued{server.Snapshot()};
    BOOST_CHECK_LE(queued.traffic[1].queued_bytes, 2U * 1024 * 1024);
    BOOST_CHECK_LE(queued.traffic[1].queued_messages, 512U);
    node::FlowMeshRuntimeRelay vote; vote.delivery_id = 5000; vote.message = Message(Kind::ATTESTATION);
    const auto start{std::chrono::steady_clock::now()}; BOOST_REQUIRE(Admitted(server.Relay(vote)));
    const auto received{Receive(live)}; BOOST_REQUIRE(received); BOOST_CHECK(*received == vote.message);
    BOOST_CHECK(std::chrono::steady_clock::now() - start < 1s);
    BOOST_REQUIRE(Wait([&] {
        const auto events{feedback.Events()}; return std::any_of(events.begin(), events.end(), [](const auto& event) {
            return event.delivery_id == 5000 && event.outcome == node::FlowMeshDeliveryOutcome::SOCKET_WRITTEN;
        });
    }));
    server.Stop();
    const auto events{feedback.Events()};
    BOOST_CHECK_EQUAL(events.size(), admitted + 1); // Exactly one terminal local result per admission.
    BOOST_CHECK_EQUAL(server.Snapshot().traffic[1].queued_bytes, 0U);
    BOOST_CHECK_EQUAL(server.Snapshot().traffic[1].admitted, admitted);
    BOOST_CHECK_EQUAL(server.Snapshot().traffic[1].rejected, full);
    BOOST_CHECK_LE(server.Snapshot().traffic[0].max_pass_bytes, 512U * 1024);
    BOOST_CHECK_LE(server.Snapshot().traffic[1].max_pass_bytes, 64U * 1024);
    BOOST_CHECK_LE(server.Snapshot().traffic[2].max_pass_bytes, 128U * 1024);
    BOOST_CHECK_LE(server.Snapshot().traffic[0].max_pass_operations, 64U);
    BOOST_CHECK_LE(server.Snapshot().traffic[1].max_pass_operations, 8U);
    BOOST_CHECK_LE(server.Snapshot().traffic[2].max_pass_operations, 2U);
}

BOOST_AUTO_TEST_CASE(cancel_and_disconnect_report_each_admitted_object_and_release_reservations)
{
    Sink sink; Feedback feedback; auto config{Config(m_path_root / "fmnet-cancel")};
    config.delivery_callback = [&](const auto& event) { return feedback.Add(event); };
    node::FlowMeshNetService server{config, sink}; std::string error; BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    CKey key; key.MakeNewKey(true); RawChannel live, actions, bulk;
    const auto address{server.Snapshot().bind_address};
    BOOST_REQUIRE(Handshake(live, address, key, 0)); BOOST_REQUIRE(Handshake(actions, address, key, 1));
    BOOST_REQUIRE(Handshake(bulk, address, key, 2));
    const int receive_buffer{4096};
    BOOST_REQUIRE_EQUAL(bulk.socket->SetSockOpt(SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)), 0);
    BOOST_REQUIRE(Wait([&] { const auto status{server.Snapshot()}; return status.peers.size() == 1 && status.peers[0].authenticated; }));
    node::FlowMeshRuntimeRelay history; history.message = Message(Kind::ENTRIES);
    history.message.payload = *flowmesh::EncodeCatchupEntries(std::vector<Bytes>{Bytes(1024 * 1024, 0x42)});
    size_t admitted{0};
    for (uint64_t id{1}; id <= 32; ++id) {
        history.delivery_id = id; if (Admitted(server.Relay(history))) ++admitted;
    }
    BOOST_CHECK_GT(admitted, 0U);
    for (uint64_t id{1}; id <= 32; ++id) server.Cancel(id);
    BOOST_REQUIRE(Wait([&] { return feedback.Events().size() == admitted; }));
    const auto events{feedback.Events()};
    BOOST_CHECK(std::any_of(events.begin(), events.end(), [](const auto& event) { return event.outcome == node::FlowMeshDeliveryOutcome::CANCELLED; }));
    BOOST_CHECK_EQUAL(server.Snapshot().traffic[2].queued_bytes, 0U);
    BOOST_CHECK_EQUAL(server.Snapshot().traffic[2].queued_messages, 0U);
    // Cancellation never manufactures a second terminal result at shutdown.
    server.Stop(); BOOST_CHECK_EQUAL(feedback.Events().size(), admitted);
}

BOOST_AUTO_TEST_CASE(refused_delivery_notification_is_explicit)
{
    Sink sink; auto config{Config(m_path_root / "fmnet-refused-feedback")};
    config.delivery_callback = [](const auto&) { return false; };
    node::FlowMeshNetService server{config, sink}; std::string error; BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    CKey key; key.MakeNewKey(true); RawChannel live, actions, bulk;
    const auto address{server.Snapshot().bind_address};
    BOOST_REQUIRE(Handshake(live, address, key, 0)); BOOST_REQUIRE(Handshake(actions, address, key, 1));
    BOOST_REQUIRE(Handshake(bulk, address, key, 2));
    BOOST_REQUIRE(Wait([&] { const auto status{server.Snapshot()}; return status.peers.size() == 1 && status.peers[0].authenticated; }));
    node::FlowMeshRuntimeRelay vote; vote.delivery_id = 1; vote.message = Message(Kind::ATTESTATION);
    BOOST_REQUIRE(Admitted(server.Relay(vote))); BOOST_REQUIRE(Receive(live));
    BOOST_REQUIRE(Wait([&] { return server.Snapshot().notification_refused == 1; }));
    vote.delivery_id = 0; BOOST_REQUIRE(Admitted(server.Relay(vote))); BOOST_REQUIRE(Receive(live));
    BOOST_REQUIRE(Wait([&] { return server.Snapshot().traffic[0].socket_written == 2; }));
    BOOST_CHECK_EQUAL(server.Snapshot().notification_refused, 1U); // Untracked control is not refused feedback.
}

BOOST_AUTO_TEST_CASE(pending_ingress_timeout_or_malformed_retry_is_explicit)
{
    for (const bool malformed : {false, true}) {
        Sink sink; sink.Admit(Kind::PROPOSAL, flowmesh::QueueResult::GLOBAL_LIMIT);
        auto config{Config(m_path_root / (malformed ? "fmnet-retry-malformed" : "fmnet-retry-timeout"))};
        config.ingress_retry_timeout = malformed ? 3000ms : 1000ms;
        node::FlowMeshNetService server{config, sink}; std::string error; BOOST_REQUIRE_MESSAGE(server.Start(error), error);
        CKey key; key.MakeNewKey(true); RawChannel live, actions, bulk;
        BOOST_REQUIRE(Handshake(live, server.Snapshot().bind_address, key, 0)); BOOST_REQUIRE(Handshake(actions, server.Snapshot().bind_address, key, 1));
        BOOST_REQUIRE(Handshake(bulk, server.Snapshot().bind_address, key, 2));
        auto frame{Frame(live, key, Message(Kind::PROPOSAL))}; BOOST_REQUIRE(Transfer(*live.socket, frame, true));
        BOOST_REQUIRE(Wait([&] { return server.Snapshot().pending_ingress_bytes > 0; }));
        if (malformed) sink.Admit(Kind::PROPOSAL, flowmesh::QueueResult::MALFORMED);
        BOOST_REQUIRE(Wait([&] { return server.Snapshot().ingress_discarded_messages == 1 && server.Snapshot().peers.empty(); }));
        BOOST_CHECK(sink.Messages().empty()); BOOST_CHECK_EQUAL(server.Snapshot().pending_ingress_bytes, 0U);
        BOOST_CHECK(server.Snapshot().last_ingress_error.find(malformed ? "malformed" : "timed out") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(outgoing_admission_rejections_are_reported_to_caller)
{
    Sink sink; node::FlowMeshNetService server{Config(m_path_root / "fmnet-admission-status"), sink};
    node::FlowMeshRuntimeRelay relay; relay.message = Message(Kind::CERTIFICATE);
    BOOST_CHECK(!Admitted(server.Relay(relay))); BOOST_CHECK_EQUAL(server.Snapshot().egress_rejected_messages, 1U);
    BOOST_CHECK(!server.Snapshot().last_egress_error.empty());
    std::string error; BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    relay.peer = 0; BOOST_CHECK(!Admitted(server.Relay(relay)));
    BOOST_CHECK_EQUAL(server.Snapshot().egress_rejected_messages, 1U); // Start resets diagnostic session.
    BOOST_CHECK(server.Snapshot().last_egress_error.find("destination") != std::string::npos);
    relay.peer.reset(); relay.message.header.version = 0; BOOST_CHECK(!Admitted(server.Relay(relay)));
    BOOST_CHECK_EQUAL(server.Snapshot().egress_rejected_messages, 2U);
    server.Stop();
    BOOST_CHECK_EQUAL(server.Snapshot().pending_ingress_bytes, 0U);
    BOOST_CHECK_EQUAL(server.Snapshot().outbox_queued_bytes, 0U);
}

BOOST_AUTO_TEST_CASE(invalid_configuration_does_not_start_or_replace_identity)
{
    Sink sink; auto config{Config(m_path_root / "fmnet-config")}; config.peers = {"localhost:5649"};
    node::FlowMeshNetService bad{config, sink}; std::string error; BOOST_CHECK(!bad.Start(error)); BOOST_CHECK(!error.empty()); BOOST_CHECK(!bad.Snapshot().running);
    config.peers.clear(); config.role = "fn-key"; node::FlowMeshNetService invalid_role{config, sink}; BOOST_CHECK(!invalid_role.Start(error));
    config.role = "observer";
    for (const auto timeout : {0ms, 30001ms}) {
        config.ingress_retry_timeout = timeout;
        node::FlowMeshNetService invalid_timeout{config, sink}; BOOST_CHECK(!invalid_timeout.Start(error));
        BOOST_CHECK(error.find("retry timeout") != std::string::npos);
    }
}
BOOST_AUTO_TEST_SUITE_END()
