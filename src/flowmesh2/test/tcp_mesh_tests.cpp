// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>
#include <bip324.h>
#include <flowmesh2/net/tcp_mesh.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/mesh_fixtures.h>
#include <hash.h>

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;
using namespace std::chrono_literals;

namespace {
//! True for the "try again" errors of a non-blocking socket. EAGAIN and
//! EWOULDBLOCK are one value on Linux, so compare both only where they differ.
bool WouldBlock(int error)
{
#if EAGAIN == EWOULDBLOCK
    return error == EAGAIN;
#else
    return error == EAGAIN || error == EWOULDBLOCK;
#endif
}
using WireBytes = std::vector<unsigned char>;
using Mesh = std::unique_ptr<net::SeatMesh>;

uint16_t FreePort()
{
    const int fd{socket(AF_INET, SOCK_STREAM, 0)};
    BOOST_REQUIRE(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOST_REQUIRE(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t size{sizeof(address)};
    BOOST_REQUIRE(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    (void)close(fd);
    return ntohs(address.sin_port);
}

struct Pair {
    std::array<CKey, 2> keys{KeyFromLabel("fm2/tcp/TEST/0"), KeyFromLabel("fm2/tcp/TEST/1")};
    std::array<net::TcpMeshConfig, 2> config;
    std::array<Mesh, 2> mesh;

    explicit Pair(bool open = true)
    {
        std::array<uint16_t, 2> ports{FreePort(), FreePort()};
        while (ports[0] == ports[1]) ports[1] = FreePort();
        for (uint32_t self{0}; self < 2; ++self) {
            auto& c{config[self]};
            c.mesh = TestMeshConfig(self, 2);
            c.mesh.max_frame_bytes = 4096;
            c.listen_port = ports[self];
            c.domain = LabelHash("fm2/tcp/TEST/deployment");
            c.seat_set_hash = LabelHash("fm2/tcp/TEST/roster");
            c.network_magic = {0xfa, 0xbf, 0xb5, 0xda};
            c.minimum_endpoint_counters = {3, 4};
            c.handshake_timeout_ms = 100;
            c.reconnect_ms = 5;
            c.max_queued_bytes_per_lane = 8192;
            for (uint32_t seat{0}; seat < 2; ++seat) {
                const XOnlyPubKey pub{keys[seat].GetPubKey()};
                std::array<unsigned char, 32> key{};
                std::copy(pub.begin(), pub.end(), key.begin());
                c.identity_keys.push_back(key);
                net::SeatEndpoint endpoint;
                endpoint.seat = seat;
                endpoint.addresses = {"127.0.0.1"};
                endpoint.port = ports[seat];
                endpoint.counter = seat + 3;
                endpoint.identity_signature = SignDigest(keys[seat], net::TcpSeatMesh::EndpointDigest(c, endpoint), uint256{});
                c.endpoints.push_back(endpoint);
            }
            const CKey key{keys[self]};
            c.sign_session = [key](const uint256& digest) -> Result<std::array<unsigned char, 64>> {
                return Ok(SignDigest(key, digest, uint256{}));
            };
        }
        if (open) { Open(0); Open(1); }
    }
    void Open(uint32_t seat)
    {
        auto made{net::TcpSeatMesh::Create(config[seat])};
        RequireOk(made, "open actual TCP mesh");
        mesh[seat] = std::move(*made);
    }
    std::vector<net::InboundFrame> Pump(size_t wanted, std::chrono::milliseconds timeout = 2000ms)
    {
        std::vector<net::InboundFrame> received;
        const auto end{std::chrono::steady_clock::now() + timeout};
        do {
            for (auto& m : mesh) {
                if (!m) continue;
                auto frame{m->Poll()};
                RequireOk(frame, "socket Poll");
                if (*frame) received.push_back(std::move(**frame));
            }
            if (received.size() >= wanted) return received;
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < end);
        return received;
    }
    void Resign(uint32_t self)
    {
        for (auto& e : config[self].endpoints) {
            e.identity_signature = SignDigest(keys[e.seat], net::TcpSeatMesh::EndpointDigest(config[self], e), uint256{});
        }
    }
};

void Put32(WireBytes& b, uint32_t n) { for (int i{0}; i < 4; ++i) b.push_back((n >> (8 * i)) & 255); }
void Put64(WireBytes& b, uint64_t n) { for (int i{0}; i < 8; ++i) b.push_back((n >> (8 * i)) & 255); }

/** An adversarial real socket speaking the documented cipher protocol. The
 * sender must prove possession of its pinned key; a claimed seat in a hello
 * never grants that identity. */
class RawPeer {
public:
    int fd{-1};
    Pair& pair;
    net::Lane lane;
    uint32_t claimed_seat{0};
    std::unique_ptr<BIP324Cipher> cipher;

    explicit RawPeer(Pair& p, net::Lane service = net::Lane::ACTION, uint32_t claimed = 0)
        : pair{p}, lane{service}, claimed_seat{claimed}
    {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        BOOST_REQUIRE(fd >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(pair.config[1].listen_port);
        BOOST_REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        BOOST_REQUIRE(fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
    }
    ~RawPeer() { if (fd >= 0) (void)close(fd); }

    void Send(const WireBytes& payload)
    {
        size_t position{0};
        const auto end{std::chrono::steady_clock::now() + 2s};
        while (position < payload.size() && std::chrono::steady_clock::now() < end) {
            const ssize_t n{send(fd, payload.data() + position, payload.size() - position, MSG_NOSIGNAL)};
            if (n > 0) position += n;
            else if (n < 0 && (WouldBlock(errno) || errno == EINTR)) {
                auto result{pair.mesh[1]->Poll()}; RequireOk(result, "raw send pump");
                std::this_thread::sleep_for(1ms);
            } else break;
        }
        BOOST_REQUIRE_EQUAL(position, payload.size());
    }
    WireBytes Read(size_t size)
    {
        WireBytes out(size);
        size_t position{0};
        const auto end{std::chrono::steady_clock::now() + 2s};
        while (position < size && std::chrono::steady_clock::now() < end) {
            auto result{pair.mesh[1]->Poll()}; RequireOk(result, "raw receive pump");
            BOOST_REQUIRE(!*result);
            const ssize_t n{recv(fd, out.data() + position, size - position, 0)};
            if (n > 0) position += n;
            else if (n < 0 && (WouldBlock(errno) || errno == EINTR)) std::this_thread::sleep_for(1ms);
            else break;
        }
        BOOST_REQUIRE_EQUAL(position, size);
        return out;
    }
    void Hello()
    {
        CKey fresh;
        fresh.MakeNewKey(true);
        const std::array<std::byte, 32> entropy{};
        cipher = std::make_unique<BIP324Cipher>(fresh, entropy);
        WireBytes hello{1, static_cast<unsigned char>(lane)};
        Put32(hello, claimed_seat); Put32(hello, pair.config[1].mesh.self_seat);
        for (const auto b : cipher->GetOurPubKey()) hello.push_back(static_cast<unsigned char>(b));
        Send(hello);
        const WireBytes response{Read(74)};
        BOOST_CHECK_EQUAL(response[0], 1);
        BOOST_CHECK_EQUAL(response[1], static_cast<unsigned char>(lane));
        cipher->InitializeForNetwork(EllSwiftPubKey{std::as_bytes(std::span{response}.subspan(10))}, true, pair.config[1].network_magic);
    }
    uint256 SessionDigest(bool initiator, const net::TcpMeshConfig* override_config = nullptr, net::Lane override_lane = net::Lane::ACTION)
    {
        const auto& c{override_config ? *override_config : pair.config[1]};
        WireBytes transcript;
        transcript.insert(transcript.end(), c.domain.begin(), c.domain.end());
        const uint256 profile{ProfileHash(c.mesh.profile)};
        transcript.insert(transcript.end(), profile.begin(), profile.end());
        transcript.insert(transcript.end(), c.seat_set_hash.begin(), c.seat_set_hash.end());
        transcript.insert(transcript.end(), c.network_magic.begin(), c.network_magic.end());
        Put64(transcript, c.epoch);
        for (const auto b : cipher->GetSessionID()) transcript.push_back(static_cast<unsigned char>(b));
        Put32(transcript, claimed_seat); Put32(transcript, c.mesh.self_seat);
        transcript.push_back(static_cast<unsigned char>(override_config ? override_lane : lane));
        transcript.push_back(initiator ? 1 : 0);
        HashWriter h;
        constexpr std::string_view tag{"B3FlowMeshSession/v1"};
        h.write(std::as_bytes(std::span{tag.data(), tag.size()}));
        h.write(std::as_bytes(std::span{transcript}));
        return h.GetSHA256();
    }
    WireBytes Encrypted(std::span<const unsigned char> plain)
    {
        WireBytes wire(plain.size() + BIP324Cipher::EXPANSION);
        cipher->Encrypt(std::as_bytes(plain), {}, false, std::as_writable_bytes(std::span{wire}));
        return wire;
    }
    void Authenticate(const std::array<unsigned char, 64>& signature)
    {
        Send(Encrypted(signature));
        const WireBytes length{Read(3)};
        const size_t size{cipher->DecryptLength(std::as_bytes(std::span{length}))};
        BOOST_REQUIRE_EQUAL(size, 64U);
        const WireBytes encrypted{Read(size + BIP324Cipher::EXPANSION - 3)};
        WireBytes plain(size);
        bool ignore{false};
        BOOST_REQUIRE(cipher->Decrypt(std::as_bytes(std::span{encrypted}), {}, ignore,
            std::as_writable_bytes(std::span{plain})));
        BOOST_CHECK(!ignore);
        BOOST_CHECK(XOnlyPubKey{pair.config[1].identity_keys[pair.config[1].mesh.self_seat]}.VerifySchnorr(SessionDigest(false), plain));
    }
    void Authenticate() { Authenticate(SignDigest(pair.keys[claimed_seat], SessionDigest(true), uint256{})); }
};

} // namespace

BOOST_AUTO_TEST_SUITE(tcp_mesh_tests)

BOOST_AUTO_TEST_CASE(signed_endpoint_configuration_refuses_tamper_and_rollback)
{
    Pair p{false};
    auto bad{p.config[0]};
    bad.endpoints[1].port++;
    BOOST_CHECK(!net::TcpSeatMesh::Create(bad));
    bad = p.config[0]; bad.minimum_endpoint_counters[1]++;
    BOOST_CHECK(!net::TcpSeatMesh::Create(bad));
    bad = p.config[0]; bad.identity_keys[1] = bad.identity_keys[0];
    BOOST_CHECK(!net::TcpSeatMesh::Create(bad));
    bad = p.config[0]; bad.epoch++;
    BOOST_CHECK(!net::TcpSeatMesh::Create(bad));
    bad = p.config[0]; bad.domain = LabelHash("another deployment");
    BOOST_CHECK(!net::TcpSeatMesh::Create(bad));
    bad = p.config[0]; bad.network_magic[0] ^= 1;
    BOOST_CHECK(!net::TcpSeatMesh::Create(bad));
    bad = p.config[0]; bad.mesh.profile.chain = ChainType::MAIN;
    const auto mainnet{net::TcpSeatMesh::Create(bad)};
    BOOST_REQUIRE(!mainnet);
    BOOST_CHECK(mainnet.error().code == ErrorCode::UNSUPPORTED_NETWORK);
    bad = p.config[0]; bad.max_queued_bytes_per_lane = bad.mesh.max_frame_bytes - 1;
    BOOST_CHECK(!net::TcpSeatMesh::Create(bad));
}

BOOST_AUTO_TEST_CASE(real_sockets_authenticate_both_seats_and_all_four_lanes)
{
    Pair p;
    DurableVoteFixture durable;
    const auto vote{durable.Vote()};
    RequireOk(p.mesh[0]->SendVote(1, vote, durable.slot.header.Hash()), "durable critical vote");
    const WireBytes proposal{agreement::MESSAGE_VERSION, static_cast<unsigned char>(agreement::MessageType::OFFER), 0x42};
    const WireBytes action{1, 4, 9};
    const WireBytes bulk{5, 7};
    RequireOk(p.mesh[0]->Send(1, net::Lane::PROPOSAL, proposal), "proposal socket");
    RequireOk(p.mesh[0]->Send(1, net::Lane::ACTION, action), "action socket");
    RequireOk(p.mesh[1]->Send(0, net::Lane::BULK, bulk), "reverse bulk socket");
    const auto received{p.Pump(4)};
    BOOST_REQUIRE_EQUAL(received.size(), 4U);
    std::array<bool, 4> lanes{};
    for (const auto& frame : received) {
        lanes[static_cast<size_t>(frame.lane) - 1] = true;
        BOOST_CHECK_EQUAL(frame.from_seat, frame.lane == net::Lane::BULK ? 1U : 0U);
        if (frame.lane == net::Lane::ACTION) BOOST_CHECK(frame.bytes == action);
        if (frame.lane == net::Lane::PROPOSAL) BOOST_CHECK(frame.bytes == proposal);
        if (frame.lane == net::Lane::BULK) BOOST_CHECK(frame.bytes == bulk);
        if (frame.lane == net::Lane::CRITICAL) {
            auto decoded{agreement::DecodeMessage(frame.bytes, p.config[0].mesh.profile)};
            RequireOk(decoded, "wire vote decoded");
            const auto* v{std::get_if<agreement::Vote>(&*decoded)};
            BOOST_REQUIRE(v != nullptr);
            BOOST_CHECK(v->signature == vote.Signature());
        }
    }
    BOOST_CHECK(std::all_of(lanes.begin(), lanes.end(), [](bool b) { return b; }));
}

BOOST_AUTO_TEST_CASE(backpressure_on_bulk_does_not_consume_critical_capacity)
{
    Pair p{false};
    for (auto& c : p.config) { c.max_messages_per_lane = 1; c.max_queued_bytes_per_lane = 4096; }
    p.Open(0); // Peer is offline: nothing can drain these queues.
    RequireOk(p.mesh[0]->Send(1, net::Lane::BULK, WireBytes(4096, 8)), "fill bulk");
    const auto full{p.mesh[0]->Send(1, net::Lane::BULK, WireBytes{9})};
    BOOST_REQUIRE(!full);
    BOOST_CHECK(full.error().code == ErrorCode::BUSY);
    DurableVoteFixture durable;
    RequireOk(p.mesh[0]->SendVote(1, durable.Vote(), durable.slot.header.Hash()), "critical independent from bulk");
    RequireOk(p.mesh[0]->Send(1, net::Lane::ACTION, WireBytes{4}), "action independent from bulk");
    p.Open(1);
    const auto received{p.Pump(3)};
    BOOST_REQUIRE_EQUAL(received.size(), 3U);
    BOOST_CHECK(std::any_of(received.begin(), received.end(), [](const auto& f) { return f.lane == net::Lane::CRITICAL; }));
    RequireOk(p.mesh[0]->Send(1, net::Lane::BULK, WireBytes{10}), "capacity released after actual send");
    BOOST_REQUIRE_EQUAL(p.Pump(1).size(), 1U);
}

BOOST_AUTO_TEST_CASE(offline_peer_cannot_consume_a_live_peers_reserved_capacity)
{
    Pair p{false};
    const CKey offline{KeyFromLabel("fm2/tcp/TEST/2")};
    const XOnlyPubKey pub{offline.GetPubKey()};
    std::array<unsigned char, 32> identity{};
    std::copy(pub.begin(), pub.end(), identity.begin());
    uint16_t offline_port{FreePort()};
    while (offline_port == p.config[0].listen_port || offline_port == p.config[1].listen_port) offline_port = FreePort();
    for (auto& c : p.config) {
        c.mesh.seat_count = 3;
        c.max_messages_per_lane = 6; // Three independently reserved slots per peer.
        c.identity_keys.push_back(identity);
        c.minimum_endpoint_counters.push_back(5);
        net::SeatEndpoint endpoint;
        endpoint.seat = 2; endpoint.addresses = {"127.0.0.1"}; endpoint.port = offline_port; endpoint.counter = 5;
        endpoint.identity_signature = SignDigest(offline, net::TcpSeatMesh::EndpointDigest(c, endpoint), uint256{});
        c.endpoints.push_back(endpoint);
    }
    auto insufficient{p.config[0]};
    insufficient.max_queued_bytes_per_lane = 8191; // floor(8191/2) cannot hold one maximum frame.
    BOOST_CHECK(!net::TcpSeatMesh::Create(insufficient));
    insufficient = p.config[0]; insufficient.max_messages_per_lane = 1;
    BOOST_CHECK(!net::TcpSeatMesh::Create(insufficient));
    p.Open(0);
    DurableVoteFixture durable;
    const auto vote{durable.Vote()};
    for (unsigned i{0}; i < 3; ++i) RequireOk(p.mesh[0]->SendVote(2, vote, durable.slot.header.Hash()), "reserve offline critical queue");
    auto full{p.mesh[0]->SendVote(2, vote, durable.slot.header.Hash())};
    BOOST_REQUIRE(!full); BOOST_CHECK(full.error().code == ErrorCode::BUSY);
    RequireOk(p.mesh[0]->Send(2, net::Lane::BULK, WireBytes(4096, 0x61)), "reserve offline full-size body");
    full = p.mesh[0]->Send(2, net::Lane::BULK, WireBytes{0x62});
    BOOST_REQUIRE(!full); BOOST_CHECK(full.error().code == ErrorCode::BUSY);
    for (unsigned i{0}; i < 3; ++i) RequireOk(p.mesh[0]->SendVote(1, vote, durable.slot.header.Hash()), "healthy critical queue remains available");
    RequireOk(p.mesh[0]->Send(1, net::Lane::BULK, WireBytes(4096, 0x63)), "healthy full-size body remains available");
    p.Open(1);
    const auto received{p.Pump(4)};
    BOOST_REQUIRE_EQUAL(received.size(), 4U);
    BOOST_CHECK_EQUAL(std::count_if(received.begin(), received.end(), [](const auto& frame) { return frame.lane == net::Lane::CRITICAL; }), 3);
    BOOST_CHECK_EQUAL(std::count_if(received.begin(), received.end(), [](const auto& frame) { return frame.lane == net::Lane::BULK && frame.bytes == WireBytes(4096, 0x63); }), 1);
    for (const auto& frame : received) BOOST_CHECK_EQUAL(frame.from_seat, 0U);
    // Offline admission remains bounded after unrelated healthy traffic drains.
    BOOST_CHECK(!p.mesh[0]->SendVote(2, vote, durable.slot.header.Hash()));
    RequireOk(p.mesh[0]->SendVote(1, vote, durable.slot.header.Hash()), "healthy quota released");
    BOOST_REQUIRE_EQUAL(p.Pump(1).size(), 1U);
}

BOOST_AUTO_TEST_CASE(continuous_critical_backlog_gives_proposal_action_and_bulk_bounded_service)
{
    Pair p;
    BOOST_CHECK(p.Pump(1, 50ms).empty()); // All four real sessions authenticate before the backlog.
    DurableVoteFixture durable;
    const auto vote{durable.Vote()};
    for (unsigned i{0}; i < 24; ++i) RequireOk(p.mesh[0]->SendVote(1, vote, durable.slot.header.Hash()), "critical backlog");
    const WireBytes proposal{agreement::MESSAGE_VERSION, static_cast<unsigned char>(agreement::MessageType::OFFER), 0x42};
    RequireOk(p.mesh[0]->Send(1, net::Lane::PROPOSAL, proposal), "queued proposal under critical pressure");
    RequireOk(p.mesh[0]->Send(1, net::Lane::ACTION, WireBytes{0x43}), "queued action under critical pressure");
    RequireOk(p.mesh[0]->Send(1, net::Lane::BULK, WireBytes{0x44}), "queued history under critical pressure");
    // Critical remains continuously nonempty; these eight authenticated receive
    // opportunities must still include every lane, rather than drain 24 votes.
    const auto received{p.Pump(8)};
    BOOST_REQUIRE_EQUAL(received.size(), 8U);
    for (const auto lane : {net::Lane::CRITICAL, net::Lane::PROPOSAL, net::Lane::ACTION, net::Lane::BULK}) {
        BOOST_CHECK(std::any_of(received.begin(), received.end(), [lane](const auto& frame) { return frame.lane == lane; }));
    }
}

BOOST_AUTO_TEST_CASE(peer_restart_reconnects_with_a_fresh_authenticated_session)
{
    Pair p;
    RequireOk(p.mesh[0]->Send(1, net::Lane::ACTION, WireBytes{1}), "before restart");
    BOOST_REQUIRE_EQUAL(p.Pump(1).size(), 1U);
    p.mesh[1].reset();
    BOOST_CHECK(p.Pump(1, 20ms).empty());
    RequireOk(p.mesh[0]->Send(1, net::Lane::ACTION, WireBytes{2}), "queue during outage");
    p.Open(1);
    auto received{p.Pump(1)};
    BOOST_REQUIRE_EQUAL(received.size(), 1U);
    BOOST_CHECK(received[0].bytes == WireBytes{2});
    BOOST_CHECK_EQUAL(received[0].from_seat, 0U);
}

BOOST_AUTO_TEST_CASE(unauthenticated_plaintext_wrong_key_and_wrong_domain_never_deliver)
{
    for (unsigned attack{0}; attack < 5; ++attack) {
        Pair p{false}; p.Open(1);
        RawPeer raw{p}; raw.Hello();
        if (attack == 0) raw.Send(WireBytes(84, 0));
        if (attack == 1) raw.Send(raw.Encrypted(SignDigest(p.keys[1], raw.SessionDigest(true), uint256{})));
        if (attack == 2) {
            auto wrong{p.config[1]}; wrong.domain = LabelHash("wrong deployment");
            raw.Send(raw.Encrypted(SignDigest(p.keys[0], raw.SessionDigest(true, &wrong), uint256{})));
        }
        if (attack == 3) {
            auto same{p.config[1]};
            raw.Send(raw.Encrypted(SignDigest(p.keys[0], raw.SessionDigest(true, &same, net::Lane::BULK), uint256{})));
        }
        if (attack == 4) raw.Send(raw.Encrypted(SignDigest(p.keys[0], raw.SessionDigest(false), uint256{})));
        raw.Send(raw.Encrypted(WireBytes{99}));
        BOOST_CHECK(p.Pump(1, 30ms).empty());
    }
}

BOOST_AUTO_TEST_CASE(session_signature_replay_and_ciphertext_replay_are_refused)
{
    std::array<unsigned char, 64> old_signature{};
    {
        Pair p{false}; p.Open(1);
        RawPeer raw{p}; raw.Hello();
        old_signature = SignDigest(p.keys[0], raw.SessionDigest(true), uint256{});
        raw.Authenticate(old_signature);
        const WireBytes encrypted{raw.Encrypted(WireBytes{7, 9})};
        raw.Send(encrypted);
        auto received{p.Pump(1)};
        BOOST_REQUIRE_EQUAL(received.size(), 1U);
        raw.Send(encrypted); // Identical bytes, wrong packet/rekey sequence.
        BOOST_CHECK(p.Pump(1, 30ms).empty());
    }
    for (const net::Lane lane : {net::Lane::ACTION, net::Lane::BULK}) {
        Pair p{false}; p.Open(1);
        RawPeer raw{p, lane}; raw.Hello();
        raw.Send(raw.Encrypted(old_signature)); // Different fresh EllSwift session.
        raw.Send(raw.Encrypted(WireBytes{8}));
        BOOST_CHECK(p.Pump(1, 30ms).empty());
    }
}

BOOST_AUTO_TEST_CASE(authenticated_reconnect_replaces_half_open_peer_without_trusting_a_claimed_hello)
{
    Pair p{false};
    p.config[1].mesh.max_frame_bytes = 64;
    p.config[1].max_queued_bytes_per_lane = 64;
    p.Open(1);
    RawPeer old{p}; old.Hello(); old.Authenticate();
    old.Send(old.Encrypted(WireBytes{1}));
    BOOST_REQUIRE_EQUAL(p.Pump(1).size(), 1U);
    {
        RawPeer impostor{p}; impostor.Hello();
        impostor.Send(impostor.Encrypted(std::array<unsigned char, 64>{}));
        BOOST_CHECK(p.Pump(1, 20ms).empty());
    }
    old.Send(old.Encrypted(WireBytes{2}));
    const auto still_old{p.Pump(1)};
    BOOST_REQUIRE_EQUAL(still_old.size(), 1U);
    BOOST_CHECK(still_old[0].bytes == WireBytes{2});
    const WireBytes partial{old.Encrypted(WireBytes(64, 0x44))};
    old.Send(WireBytes(partial.begin(), partial.begin() + 3));
    BOOST_CHECK(p.Pump(1, 10ms).empty());
    // Keep the old TCP connection open. Only the valid fresh key proof may
    // replace it, and replacement must release its partial-input reservation.
    RawPeer replacement{p}; replacement.Hello(); replacement.Authenticate();
    replacement.Send(replacement.Encrypted(WireBytes{3}));
    const auto received{p.Pump(1)};
    BOOST_REQUIRE_EQUAL(received.size(), 1U);
    BOOST_CHECK(received[0].bytes == WireBytes{3});
    unsigned char byte{0};
    const ssize_t old_read{recv(old.fd, &byte, 1, 0)};
    BOOST_CHECK(old_read == 0 || (old_read < 0 && errno == ECONNRESET));
}

BOOST_AUTO_TEST_CASE(tampered_ciphertext_and_oversize_packet_are_disconnected)
{
    for (unsigned attack{0}; attack < 2; ++attack) {
        Pair p{false};
        p.config[1].mesh.max_frame_bytes = 64;
        p.Open(1);
        RawPeer raw{p}; raw.Hello(); raw.Authenticate();
        WireBytes encrypted{raw.Encrypted(WireBytes(attack == 0 ? 32 : 65, 1))};
        if (attack == 0) encrypted.back() ^= 1;
        raw.Send(encrypted);
        BOOST_CHECK(p.Pump(1, 30ms).empty());
    }
}

BOOST_AUTO_TEST_CASE(timed_out_unauthenticated_socket_does_not_block_valid_peers)
{
    Pair p{false};
    p.config[1].handshake_timeout_ms = 10;
    p.config[1].max_pending_connections = 1;
    p.Open(1);
    RawPeer silent{p}; // No hello or signature, no application identity.
    BOOST_CHECK(p.Pump(1, 30ms).empty());
    p.Open(0);
    RequireOk(p.mesh[0]->Send(1, net::Lane::ACTION, WireBytes{0x51}), "valid peer after timeout");
    const auto received{p.Pump(1)};
    BOOST_REQUIRE_EQUAL(received.size(), 1U);
    BOOST_CHECK(received[0].bytes == WireBytes{0x51});
}

BOOST_AUTO_TEST_CASE(partial_packets_reserve_global_lane_capacity_and_release_on_disconnect)
{
    for (bool disconnect : {false, true}) {
        Pair p{false};
        auto& c{p.config[1]};
        const CKey receiver{KeyFromLabel("fm2/tcp/TEST/2")};
        const XOnlyPubKey pub{receiver.GetPubKey()};
        std::array<unsigned char, 32> receiver_pub{};
        std::copy(pub.begin(), pub.end(), receiver_pub.begin());
        c.mesh.self_seat = 2;
        c.mesh.seat_count = 3;
        c.mesh.max_frame_bytes = 64;
        c.max_queued_bytes_per_lane = 128; // A full-size independent reservation for each peer.
        c.listen_port = FreePort();
        c.identity_keys.push_back(receiver_pub);
        c.minimum_endpoint_counters.push_back(5);
        net::SeatEndpoint self;
        self.seat = 2; self.addresses = {"127.0.0.1"}; self.port = c.listen_port; self.counter = 5;
        self.identity_signature = SignDigest(receiver, net::TcpSeatMesh::EndpointDigest(c, self), uint256{});
        c.endpoints.push_back(self);
        c.sign_session = [receiver](const uint256& digest) -> Result<std::array<unsigned char, 64>> {
            return Ok(SignDigest(receiver, digest, uint256{}));
        };
        p.Open(1);
        RawPeer slow{p, net::Lane::ACTION, 0}; slow.Hello(); slow.Authenticate();
        RawPeer other{p, net::Lane::ACTION, 1}; other.Hello(); other.Authenticate();
        const WireBytes partial{slow.Encrypted(WireBytes(64, 0x41))};
        slow.Send(WireBytes(partial.begin(), partial.begin() + 3)); // Only the authenticated encrypted length.
        BOOST_CHECK(p.Pump(1, 10ms).empty());
        other.Send(other.Encrypted(WireBytes{0x42}));
        // The incomplete body owns the first peer's entire reservation, but
        // cannot consume the other peer's independent share of the global cap.
        const auto healthy{p.Pump(1)};
        BOOST_REQUIRE_EQUAL(healthy.size(), 1U);
        BOOST_CHECK_EQUAL(healthy[0].from_seat, 1U);
        BOOST_CHECK(healthy[0].bytes == WireBytes{0x42});
        if (disconnect) {
            (void)close(slow.fd); slow.fd = -1;
            BOOST_CHECK(p.Pump(1, 10ms).empty());
            RawPeer replacement{p, net::Lane::ACTION, 0}; replacement.Hello(); replacement.Authenticate();
            replacement.Send(replacement.Encrypted(WireBytes(64, 0x45)));
            const auto received{p.Pump(1)};
            BOOST_REQUIRE_EQUAL(received.size(), 1U);
            BOOST_CHECK_EQUAL(received[0].from_seat, 0U);
            BOOST_CHECK(received[0].bytes == WireBytes(64, 0x45));
        } else {
            slow.Send(WireBytes(partial.begin() + 3, partial.end()));
            const auto received{p.Pump(1)};
            BOOST_REQUIRE_EQUAL(received.size(), 1U);
            BOOST_CHECK_EQUAL(received[0].from_seat, 0U);
            BOOST_CHECK(received[0].bytes == WireBytes(64, 0x41));
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
