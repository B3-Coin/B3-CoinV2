// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/net/lanes.h>
#include <flowmesh2/net/tcp_mesh.h>
#include <flowmesh2/netio/bip324_session.h>
#include <flowmesh2/netio/mesh_handshake.h>
#include <flowmesh2/test/util/agreement_fixtures.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/mesh_fixtures.h>
#include <flowmesh2/test/util/require.h>
#include <flowmesh2/test/util/storage_fixtures.h>
#include <crypto/bls.h>
#include <key.h>
#include <pubkey.h>
#include <util/sock.h>

#include <boost/test/unit_test.hpp>

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

/** The TCP seat mesh (M4-NET-01, O-102): the encrypted channel round-trips,
 * the HELLO binds the session, two seats over loopback exchange frames on
 * every lane and recover from a dropped connection, a peer with another
 * transport key or a forged record is refused, outboxes are bounded and
 * dedupe, a clone of a seat's keys is refused as a duplicate deployment.
 */
using namespace flowmesh2;
using namespace flowmesh2::net;
using namespace flowmesh2::test;

namespace {

const std::array<uint8_t, 4> MAGIC{0xf2, 0x51, 0x4d, 0x01};

uint16_t FreePort()
{
    const int fd{::socket(AF_INET, SOCK_STREAM, 0)};
    BOOST_REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    BOOST_REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len{sizeof(addr)};
    BOOST_REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    const uint16_t port{ntohs(addr.sin_port)};
    ::close(fd);
    return port;
}

CKey TransportKey(std::string_view label)
{
    const uint256 secret{LabelHash("fm2/test/mesh/transport/" + std::string{label})};
    CKey key;
    key.Set(secret.begin(), secret.end(), true);
    BOOST_REQUIRE(key.IsValid());
    return key;
}

std::array<unsigned char, 32> SecretOf(const CKey& key)
{
    std::array<unsigned char, 32> out{};
    std::copy(key.begin(), key.end(), reinterpret_cast<std::byte*>(out.data()));
    return out;
}

std::array<unsigned char, 32> XOnlyOf(const CKey& key)
{
    const XOnlyPubKey x{key.GetPubKey()};
    std::array<unsigned char, 32> out{};
    std::copy(x.begin(), x.end(), out.begin());
    return out;
}

//! A record for seat `seat` (named s<seat>) at 127.0.0.1:port with `transport`, signed by the seat's TEST BLS key.
SeatEndpointRecord Record(uint32_t seat, uint16_t port, const CKey& transport, const uint256& domain)
{
    SeatEndpointRecord record;
    record.seat = seat;
    record.addresses = {"127.0.0.1"};
    record.port = port;
    record.counter = 1;
    record.transport_key = XOnlyOf(transport);
    const uint256 digest{netio::EndpointDigest(record, domain)};
    const bls::Signature signature{BlsKeyFromLabel(SeatLabel("s" + std::to_string(seat))).Sign(std::span<const unsigned char>{digest.data(), digest.size()})};
    record.signature = signature.Compressed();
    return record;
}

struct Rig {
    std::shared_ptr<const SeatSet> seats{std::make_shared<const SeatSet>(TestSeats({"s0", "s1", "s2", "s3"}))};
    uint256 domain{TestDomain()};
    std::vector<uint16_t> ports;
    std::vector<CKey> transports;
    std::vector<SeatEndpointRecord> records;

    Rig()
    {
        for (uint32_t seat{0}; seat < 4; ++seat) {
            ports.push_back(FreePort());
            transports.push_back(TransportKey("s" + std::to_string(seat)));
            records.push_back(Record(seat, ports.back(), transports.back(), domain));
        }
    }

    TcpMeshConfig ConfigFor(uint32_t self) const
    {
        TcpMeshConfig config;
        config.mesh = TestMeshConfig(self, 4);
        config.listen_port = ports[self];
        config.seats = seats;
        config.domain = domain;
        config.magic = MAGIC;
        config.transport_secret = SecretOf(transports[self]);
        config.endpoints = records;
        config.limits.handshake_timeout_ms = 2'000;
        config.limits.reconnect_min_ms = 20;
        config.limits.reconnect_max_ms = 200;
        config.limits.poll_interval_ms = 10;
        return config;
    }
};

std::optional<InboundFrame> WaitFrame(SeatMesh& mesh, std::chrono::milliseconds limit = std::chrono::milliseconds{5'000})
{
    const auto deadline{std::chrono::steady_clock::now() + limit};
    while (std::chrono::steady_clock::now() < deadline) {
        Result<std::optional<InboundFrame>> polled{mesh.Poll()};
        if (polled.has_value() && polled->has_value()) return std::move(**polled);
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return std::nullopt;
}

} // namespace

BOOST_AUTO_TEST_SUITE(tcp_mesh_tests)

BOOST_AUTO_TEST_CASE(the_channel_and_the_hello_bind_the_session)
{
    netio::SecureChannel a;
    netio::SecureChannel b;
    a.Initialize(b.OurPubKey(), /*initiator=*/true, MAGIC);
    b.Initialize(a.OurPubKey(), /*initiator=*/false, MAGIC);
    BOOST_CHECK(a.SessionId() == b.SessionId());
    const std::vector<unsigned char> contents{1, 2, 3, 4, 5};
    const std::vector<unsigned char> packet{a.Encrypt(contents)};
    BOOST_CHECK_EQUAL(packet.size(), contents.size() + netio::SecureChannel::EXPANSION);
    BOOST_CHECK_EQUAL(b.DecryptLength(std::span<const unsigned char>{packet.data(), 3}), contents.size());
    std::vector<unsigned char> back;
    BOOST_CHECK(b.Decrypt(std::span<const unsigned char>{packet.data() + 3, packet.size() - 3}, back));
    BOOST_CHECK(back == contents);
    // A tampered packet does not decrypt; another magic gives another session.
    std::vector<unsigned char> tampered{a.Encrypt(contents)};
    tampered.back() ^= 0x01;
    (void)b.DecryptLength(std::span<const unsigned char>{tampered.data(), 3});
    BOOST_CHECK(!b.Decrypt(std::span<const unsigned char>{tampered.data() + 3, tampered.size() - 3}, back));
    netio::SecureChannel c;
    netio::SecureChannel d;
    c.Initialize(d.OurPubKey(), true, MAGIC);
    d.Initialize(c.OurPubKey(), false, {0xf2, 0x51, 0x4d, 0x02});
    BOOST_CHECK(c.SessionId() != d.SessionId());

    // The HELLO: exact codec, a valid signature, and no signature survives another session, lane or seat.
    const CKey transport{TransportKey("hello")};
    netio::MeshHello hello;
    hello.from_seat = 0;
    hello.to_seat = 1;
    hello.lane = Lane::CRITICAL;
    hello.epoch = 0;
    hello.seat_set_root = uint256::ONE;
    hello.transport_key = XOnlyOf(transport);
    BOOST_REQUIRE(netio::SignHello(transport, a.SessionId(), MAGIC, hello));
    const std::vector<unsigned char> bytes{hello.Encode()};
    BOOST_CHECK_EQUAL(bytes.size(), netio::MESH_HELLO_BYTES);
    const Result<netio::MeshHello> decoded{netio::MeshHello::Decode(bytes)};
    RequireOk(decoded, "MeshHello::Decode");
    BOOST_CHECK(*decoded == hello);
    RequireOk(netio::VerifyHello(hello, a.SessionId(), MAGIC), "VerifyHello");
    RequireRefusal(netio::VerifyHello(hello, c.SessionId(), MAGIC), ErrorCode::VERIFY_FAILED, netio::HELLO_SIGNATURE, "another session");
    RequireRefusal(netio::VerifyHello(hello, a.SessionId(), {0xf2, 0x51, 0x4d, 0x02}), ErrorCode::VERIFY_FAILED, netio::HELLO_SIGNATURE, "another magic");
    netio::MeshHello other_lane{hello};
    other_lane.lane = Lane::BULK;
    RequireRefusal(netio::VerifyHello(other_lane, a.SessionId(), MAGIC), ErrorCode::VERIFY_FAILED, netio::HELLO_SIGNATURE, "another lane");
    netio::MeshHello reflected{hello};
    std::swap(reflected.from_seat, reflected.to_seat);
    RequireRefusal(netio::VerifyHello(reflected, a.SessionId(), MAGIC), ErrorCode::VERIFY_FAILED, netio::HELLO_SIGNATURE, "reflected");
    std::vector<unsigned char> shorter{bytes.begin(), bytes.end() - 1};
    RequireRefusal(netio::MeshHello::Decode(shorter), ErrorCode::DECODE, netio::HELLO_SHAPE, "shorter");
    std::vector<unsigned char> other_version{bytes};
    other_version[0] = 2;
    RequireRefusal(netio::MeshHello::Decode(other_version), ErrorCode::DECODE, netio::HELLO_VERSION_UNKNOWN, "version");
}

BOOST_AUTO_TEST_CASE(endpoint_records_are_verified_at_create)
{
    Rig rig;
    TcpMeshConfig config{rig.ConfigFor(0)};
    RequireRefusal(TcpSeatMesh::Create([&] {
                       TcpMeshConfig c{config};
                       c.seats.reset();
                       return c;
                   }()),
                   ErrorCode::INVALID_INPUT, TCP_MESH_SEATS_MISSING, "no seats");
    RequireRefusal(TcpSeatMesh::Create([&] {
                       TcpMeshConfig c{config};
                       c.endpoints[2].signature[3] ^= 0x01;
                       return c;
                   }()),
                   ErrorCode::VERIFY_FAILED, TCP_MESH_ENDPOINT_SIGNATURE, "a forged record");
    RequireRefusal(TcpSeatMesh::Create([&] {
                       TcpMeshConfig c{config};
                       c.endpoints[1].port = 0;
                       return c;
                   }()),
                   ErrorCode::INVALID_INPUT, TCP_MESH_ENDPOINT_SHAPE, "a record without a port");
    RequireRefusal(TcpSeatMesh::Create([&] {
                       TcpMeshConfig c{config};
                       c.endpoints.pop_back();
                       return c;
                   }()),
                   ErrorCode::INVALID_INPUT, TCP_MESH_ENDPOINTS_MISSING, "a seat without a record");
    RequireRefusal(TcpSeatMesh::Create([&] {
                       TcpMeshConfig c{config};
                       c.transport_secret = SecretOf(rig.transports[1]);
                       return c;
                   }()),
                   ErrorCode::INVALID_INPUT, TCP_MESH_OWN_ENDPOINT, "our record names another key");
    RequireRefusal(TcpSeatMesh::Create([&] {
                       TcpMeshConfig c{config};
                       c.transport_secret.fill(0);
                       return c;
                   }()),
                   ErrorCode::INVALID_INPUT, TCP_MESH_TRANSPORT_KEY, "a zero secret");
    SeatEndpointRecord outside{rig.records[0]};
    outside.seat = 9;
    RequireRefusal(netio::VerifyEndpointRecord(outside, *rig.seats, rig.domain), ErrorCode::INVALID_INPUT, TCP_MESH_ENDPOINT_SEAT, "seat outside");
    RequireRefusal(netio::VerifyEndpointRecord(rig.records[0], *rig.seats, uint256::ONE), ErrorCode::VERIFY_FAILED, TCP_MESH_ENDPOINT_SIGNATURE,
                   "another domain");
    Result<std::unique_ptr<TcpSeatMesh>> alone{TcpSeatMesh::Create(config)};
    RequireOk(alone, "a mesh with every record");
    BOOST_CHECK_EQUAL((*alone)->ListenPort(), rig.ports[0]);
    (*alone)->Stop();
}

BOOST_AUTO_TEST_CASE(two_seats_exchange_frames_on_every_lane_and_survive_a_drop)
{
    Rig rig;
    Result<std::unique_ptr<TcpSeatMesh>> m0{TcpSeatMesh::Create(rig.ConfigFor(0))};
    Result<std::unique_ptr<TcpSeatMesh>> m1{TcpSeatMesh::Create(rig.ConfigFor(1))};
    RequireOk(m0, "mesh 0");
    RequireOk(m1, "mesh 1");
    TcpSeatMesh& a{**m0};
    TcpSeatMesh& b{**m1};
    const std::vector<unsigned char> proposal{agreement::MESSAGE_VERSION, static_cast<unsigned char>(agreement::MessageType::PROPOSAL), 7, 7};
    const std::vector<unsigned char> evidence{agreement::MESSAGE_VERSION, static_cast<unsigned char>(agreement::MessageType::EVIDENCE), 5};
    const std::vector<unsigned char> action{9, 9, 9};
    const std::vector<unsigned char> bulk{1, 2, 3, 4};
    // Frames queued before the connections open are delivered once they do.
    RequireOk(a.Send(1, Lane::PROPOSAL, proposal), "Send PROPOSAL");
    RequireOk(a.Send(1, Lane::CRITICAL, evidence), "Send CRITICAL");
    RequireOk(a.Send(1, Lane::ACTION, action), "Send ACTION");
    RequireOk(a.Send(1, Lane::BULK, bulk), "Send BULK");
    std::vector<InboundFrame> got;
    for (int i{0}; i < 4; ++i) {
        std::optional<InboundFrame> frame{WaitFrame(b)};
        BOOST_REQUIRE_MESSAGE(frame.has_value(), "frame " << i << " never arrived");
        BOOST_CHECK_EQUAL(frame->from_seat, 0U);
        got.push_back(std::move(*frame));
    }
    bool saw_proposal{false}, saw_critical{false}, saw_action{false}, saw_bulk{false};
    for (const InboundFrame& frame : got) {
        if (frame.lane == Lane::PROPOSAL) saw_proposal = frame.bytes == proposal;
        if (frame.lane == Lane::CRITICAL) saw_critical = frame.bytes == evidence;
        if (frame.lane == Lane::ACTION) saw_action = frame.bytes == action;
        if (frame.lane == Lane::BULK) saw_bulk = frame.bytes == bulk;
    }
    BOOST_CHECK(saw_proposal && saw_critical && saw_action && saw_bulk);
    // The other direction, and a frame to a seat that is not running stays queued (nothing lost, nothing delivered).
    RequireOk(b.Send(0, Lane::ACTION, action), "Send back");
    std::optional<InboundFrame> back{WaitFrame(a)};
    BOOST_REQUIRE(back.has_value());
    BOOST_CHECK(back->from_seat == 1 && back->lane == Lane::ACTION && back->bytes == action);
    RequireOk(a.Send(2, Lane::ACTION, action), "Send to an absent seat");
    BOOST_CHECK(!WaitFrame(b, std::chrono::milliseconds{200}).has_value());
    // Drop every connection: later frames arrive after the redial.
    a.DropConnections();
    b.DropConnections();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    RequireOk(a.Send(1, Lane::BULK, bulk), "Send after the drop");
    std::optional<InboundFrame> again{WaitFrame(b)};
    BOOST_REQUIRE(again.has_value());
    BOOST_CHECK(again->lane == Lane::BULK && again->bytes == bulk);
    const TcpMeshStatus status{a.Status()};
    BOOST_CHECK_EQUAL(status.listen_port, rig.ports[0]);
    bool found{false};
    for (const TcpPeerStatus& peer : status.peers) {
        if (peer.seat == 1) {
            found = true;
            BOOST_CHECK_GE(peer.frames_out, 5U);
            BOOST_CHECK_GE(peer.frames_in, 1U);
        }
    }
    BOOST_CHECK(found);
    a.Stop();
    RequireRefusal(a.Send(1, Lane::BULK, bulk), ErrorCode::FENCED, TCP_MESH_CLOSED, "a stopped mesh");
    b.Stop();
}

BOOST_AUTO_TEST_CASE(a_peer_with_another_transport_key_or_a_clone_is_refused)
{
    Rig rig;
    Result<std::unique_ptr<TcpSeatMesh>> m0{TcpSeatMesh::Create(rig.ConfigFor(0))};
    RequireOk(m0, "mesh 0");
    // Seat 1 runs with a key that is not its record's (its own record is rewritten to match, so Create accepts it).
    TcpMeshConfig impostor{rig.ConfigFor(1)};
    const CKey other{TransportKey("impostor")};
    impostor.transport_secret = SecretOf(other);
    impostor.endpoints[1] = Record(1, rig.ports[1], other, rig.domain);
    Result<std::unique_ptr<TcpSeatMesh>> m1{TcpSeatMesh::Create(impostor)};
    RequireOk(m1, "the impostor's own mesh");
    const std::vector<unsigned char> bulk{1, 2, 3};
    RequireOk((*m1)->Send(0, Lane::BULK, bulk), "the impostor sends");
    RequireOk((*m0)->Send(1, Lane::BULK, bulk), "seat 0 sends");
    BOOST_CHECK(!WaitFrame(**m0, std::chrono::milliseconds{600}).has_value());
    BOOST_CHECK(!WaitFrame(**m1, std::chrono::milliseconds{200}).has_value());
    bool refused{false};
    for (const TcpPeerStatus& peer : (*m0)->Status().peers) {
        if (peer.seat == 1 && peer.refused > 0) refused = true;
    }
    BOOST_CHECK(refused);
    (*m1)->Stop();

    // The real seat 1 connects; then a clone of seat 1's keys dials seat 0 too and is refused while the real one is live.
    Result<std::unique_ptr<TcpSeatMesh>> real{TcpSeatMesh::Create(rig.ConfigFor(1))};
    RequireOk(real, "the real seat 1");
    RequireOk((*real)->Send(0, Lane::ACTION, bulk), "the real seat sends");
    std::optional<InboundFrame> frame{WaitFrame(**m0)};
    BOOST_REQUIRE(frame.has_value());
    BOOST_CHECK_EQUAL(frame->from_seat, 1U);
    // Keep the real connections live with traffic while the clone tries.
    TcpMeshConfig clone_config{rig.ConfigFor(1)};
    clone_config.listen_port = FreePort();
    Result<std::unique_ptr<TcpSeatMesh>> clone{TcpSeatMesh::Create(clone_config)};
    RequireOk(clone, "the clone");
    const std::vector<unsigned char> clone_bytes{0xc1, 0x0e};
    for (int i{0}; i < 20; ++i) {
        RequireOk((*real)->Send(0, Lane::ACTION, std::vector<unsigned char>{static_cast<unsigned char>(i)}), "keep-alive");
        RequireOk((*clone)->Send(0, Lane::ACTION, clone_bytes), "the clone sends");
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    bool clone_delivered{false};
    for (;;) {
        Result<std::optional<InboundFrame>> polled{(*m0)->Poll()};
        if (!polled.has_value() || !polled->has_value()) break;
        if ((*polled)->bytes == clone_bytes) clone_delivered = true;
    }
    BOOST_CHECK(!clone_delivered);
    (*clone)->Stop();
    (*real)->Stop();
    (*m0)->Stop();
}

BOOST_AUTO_TEST_CASE(outboxes_are_bounded_and_dedupe)
{
    Rig rig;
    TcpMeshConfig config{rig.ConfigFor(0)};
    config.limits.outbox_frames = 4;
    Result<std::unique_ptr<TcpSeatMesh>> m0{TcpSeatMesh::Create(config)};
    RequireOk(m0, "mesh 0");
    for (unsigned char i{0}; i < 4; ++i) RequireOk((*m0)->Send(1, Lane::BULK, std::vector<unsigned char>{i}), "a distinct frame");
    RequireOk((*m0)->Send(1, Lane::BULK, std::vector<unsigned char>{2}), "an identical frame is already queued");
    RequireRefusal((*m0)->Send(1, Lane::BULK, std::vector<unsigned char>{9}), ErrorCode::BUSY, LANE_FULL, "a fifth distinct frame");
    RequireOk((*m0)->Send(1, Lane::ACTION, std::vector<unsigned char>{9}), "another lane has its own outbox");
    RequireOk((*m0)->Send(2, Lane::BULK, std::vector<unsigned char>{9}), "another seat has its own outbox");
    (*m0)->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
