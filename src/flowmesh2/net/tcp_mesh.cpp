// TODO(rebirth:M4-NET-01) external cryptographic review and Internet interoperability qualification remain gated.
// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/net/tcp_mesh.h>
#include <flowmesh2/net/tcp_mesh_wire.h>

#include <bip324.h>
#include <hash.h>
#include <key.h>
#include <pubkey.h>
#include <random.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>
#include <set>
#include <utility>

#ifndef _WIN32
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace flowmesh2::net {
namespace {
#ifndef _WIN32
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
#endif
constexpr std::string_view CONFIG_BAD{"invalid authenticated TCP mesh configuration"};
constexpr std::string_view ENDPOINT_BAD{"invalid signed seat endpoint or counter rollback"};
constexpr std::string_view SOCKET_BAD{"cannot bind authenticated seat listener"};
constexpr size_t IO_BUDGET{256 * 1024};

void Put32(std::vector<unsigned char>& b, uint32_t n)
{
    for (int i{0}; i != 4; ++i) b.push_back((n >> (8 * i)) & 255);
}
void Put64(std::vector<unsigned char>& b, uint64_t n)
{
    for (int i{0}; i != 8; ++i) b.push_back((n >> (8 * i)) & 255);
}
void Common(std::vector<unsigned char>& b, const TcpMeshConfig& c)
{
    b.insert(b.end(), c.domain.begin(), c.domain.end());
    const uint256 profile{ProfileHash(c.mesh.profile)};
    b.insert(b.end(), profile.begin(), profile.end());
    b.insert(b.end(), c.seat_set_hash.begin(), c.seat_set_hash.end());
    b.insert(b.end(), c.network_magic.begin(), c.network_magic.end());
    Put64(b, c.epoch);
}
uint256 Digest(std::string_view tag, std::span<const unsigned char> bytes)
{
    // Explicit transport protocol domain; no consensus hash registry changes.
    HashWriter h;
    h.write(std::as_bytes(std::span{tag.data(), tag.size()}));
    h.write(std::as_bytes(bytes));
    return h.GetSHA256();
}

#ifndef _WIN32
using Clock = std::chrono::steady_clock;

struct Address {
    sockaddr_storage storage{};
    socklen_t size{0};
};
std::optional<Address> ParseAddress(const std::string& text, uint16_t port)
{
    Address out;
    auto* v4{reinterpret_cast<sockaddr_in*>(&out.storage)};
    if (inet_pton(AF_INET, text.c_str(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        out.size = sizeof(sockaddr_in);
        return out;
    }
    auto* v6{reinterpret_cast<sockaddr_in6*>(&out.storage)};
    if (inet_pton(AF_INET6, text.c_str(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        out.size = sizeof(sockaddr_in6);
        return out;
    }
    return std::nullopt;
}
bool Nonblocking(int fd)
{
    const int flags{fcntl(fd, F_GETFL, 0)};
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
        fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}
void ConfigureSocket(int fd)
{
    const int one{1};
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
    const int idle{10}, interval{2}, count{3};
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#endif
}

struct Link {
    int fd{-1};
    bool initiator{false};
    bool connecting{false};
    bool dead{false};
    bool hello_sent{false};
    bool hello_received{false};
    bool auth_sent{false};
    bool auth_received{false};
    uint32_t peer{0};
    Lane lane{Lane::BULK};
    Clock::time_point born{Clock::now()};
    std::unique_ptr<BIP324Cipher> cipher;
    std::vector<unsigned char> wire;
    size_t wire_pos{0};
    enum class Sending { NONE, HELLO, AUTH, FRAME } sending{Sending::NONE};
    std::vector<unsigned char> hello;
    std::array<unsigned char, 3> length{};
    size_t length_pos{0};
    std::optional<size_t> packet_length;
    std::vector<unsigned char> packet;
    size_t packet_pos{0};
    bool input_reserved{false};

    Link(int socket, bool dial, uint32_t seat, Lane service) : fd{socket}, initiator{dial}, peer{seat}, lane{service}
    {
        CKey key;
        key.MakeNewKey(true);
        std::array<std::byte, 32> entropy;
        GetStrongRandBytes(std::span{reinterpret_cast<unsigned char*>(entropy.data()), entropy.size()});
        cipher = std::make_unique<BIP324Cipher>(key, entropy);
    }
    ~Link() { if (fd >= 0) (void)close(fd); }
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;
    bool Authenticated() const { return auth_received && auth_sent; }
};

struct Channel {
    std::unique_ptr<Link> link;
    std::deque<std::vector<unsigned char>> outgoing;
    std::deque<std::vector<unsigned char>> incoming;
    Clock::time_point retry{};
    size_t next_address{0};
    size_t incoming_bytes{0}, incoming_messages{0}, outgoing_bytes{0}, outgoing_messages{0};
};

class SocketSeatMesh final : public SeatMesh
{
public:
    SocketSeatMesh(const TcpMeshConfig& config, int listener)
        : SeatMesh{config.mesh}, m_config{config}, m_listener{listener},
          m_channels(config.mesh.seat_count * LANE_COUNT), m_endpoints(config.mesh.seat_count)
    {
        for (const auto& e : config.endpoints) m_endpoints[e.seat] = e;
    }
    ~SocketSeatMesh() override { if (m_listener >= 0) (void)close(m_listener); }

    Result<std::optional<InboundFrame>> Poll() override
    {
        if (m_closed) return Error{ErrorCode::FENCED, "seat mesh listener closed"};
        Accept();
        if (m_closed) return Error{ErrorCode::FENCED, "seat mesh listener failed"};
        // Independent service sockets and bounded I/O per socket prevent a
        // saturated proposal/history stream from blocking critical votes.
        for (const Lane lane : LANES_BY_PRIORITY) {
            for (uint32_t seat{0}; seat < Config().seat_count; ++seat) {
                if (seat == Config().self_seat) continue;
                Channel& c{At(seat, lane)};
                if (!c.link && Config().self_seat < seat && Clock::now() >= c.retry) Dial(seat, lane);
                if (c.link) {
                    Step(*c.link);
                    if (c.link->dead) {
                        ReleaseInFlight(*c.link);
                        c.link.reset();
                        c.retry = Clock::now() + std::chrono::milliseconds{m_config.reconnect_ms};
                    }
                }
            }
        }
        for (size_t i{0}; i < m_pending.size();) {
            Link& link{*m_pending[i]};
            Step(link);
            if (link.dead) {
                ReleaseInFlight(link);
                m_pending.erase(m_pending.begin() + i);
            } else if (link.Authenticated()) {
                Channel& channel{At(link.peer, link.lane)};
                // A returning peer may replace a stale half-open session,
                // but ONLY after proving its pinned identity in the fresh
                // encrypted session. A claimed hello never evicts a peer.
                if (channel.link) ReleaseInFlight(*channel.link);
                channel.link = std::move(m_pending[i]);
                m_pending.erase(m_pending.begin() + i);
            } else {
                ++i;
            }
        }
        // A continuous authenticated critical retry stream cannot starve the
        // proposal it needs to make progress, or source/history recovery.
        static constexpr std::array<Lane, 8> service_cycle{Lane::CRITICAL, Lane::CRITICAL, Lane::CRITICAL, Lane::CRITICAL,
            Lane::PROPOSAL, Lane::PROPOSAL, Lane::ACTION, Lane::BULK};
        for (size_t attempt{0}; attempt < service_cycle.size(); ++attempt) {
            const size_t position{(m_service_cursor + attempt) % service_cycle.size()};
            const Lane lane{service_cycle[position]};
            const size_t index{Index(lane)};
            for (uint32_t n{0}; n < Config().seat_count; ++n) {
                const uint32_t seat{(m_next_peer[index] + n) % Config().seat_count};
                Channel& c{At(seat, lane)};
                if (c.incoming.empty()) continue;
                auto bytes{std::move(c.incoming.front())};
                c.incoming.pop_front();
                c.incoming_bytes -= bytes.size();
                --c.incoming_messages;
                m_in_bytes[index] -= bytes.size();
                --m_in_messages[index];
                m_next_peer[index] = (seat + 1) % Config().seat_count;
                m_service_cursor = (position + 1) % service_cycle.size();
                return Ok(std::optional<InboundFrame>{InboundFrame{seat, lane, std::move(bytes)}});
            }
        }
        return Ok(std::optional<InboundFrame>{});
    }

protected:
    Result<void> Transmit(uint32_t seat, Lane lane, std::span<const unsigned char> frame) override
    {
        if (m_closed) return Error{ErrorCode::FENCED, "seat mesh closed"};
        const size_t index{Index(lane)};
        Channel& channel{At(seat, lane)};
        if (m_out_messages[index] >= m_config.max_messages_per_lane ||
            frame.size() > m_config.max_queued_bytes_per_lane - m_out_bytes[index] ||
            channel.outgoing_messages >= PeerMessages() || frame.size() > PeerBytes() - channel.outgoing_bytes) {
            return Error{ErrorCode::BUSY, "seat mesh lane queue full"};
        }
        channel.outgoing.emplace_back(frame.begin(), frame.end());
        ++channel.outgoing_messages;
        channel.outgoing_bytes += frame.size();
        ++m_out_messages[index];
        m_out_bytes[index] += frame.size();
        return Ok();
    }

private:
    static size_t Index(Lane lane) { return static_cast<size_t>(lane) - 1; }
    Channel& At(uint32_t seat, Lane lane) { return m_channels[seat * LANE_COUNT + Index(lane)]; }
    size_t PeerMessages() const { return m_config.max_messages_per_lane / std::max<uint32_t>(1, Config().seat_count - 1); }
    size_t PeerBytes() const { return m_config.max_queued_bytes_per_lane / std::max<uint32_t>(1, Config().seat_count - 1); }
    void ReleaseInFlight(Link& link)
    {
        if (!link.input_reserved) return;
        const size_t index{Index(link.lane)};
        Channel& channel{At(link.peer, link.lane)};
        channel.incoming_bytes -= *link.packet_length;
        --channel.incoming_messages;
        m_in_bytes[index] -= *link.packet_length;
        --m_in_messages[index];
        link.input_reserved = false;
    }

    uint256 SessionDigest(const Link& link, bool initiator_signature) const
    {
        std::vector<unsigned char> bytes;
        Common(bytes, m_config);
        const auto session{link.cipher->GetSessionID()};
        for (std::byte b : session) bytes.push_back(static_cast<unsigned char>(b));
        Put32(bytes, link.initiator ? Config().self_seat : link.peer);
        Put32(bytes, link.initiator ? link.peer : Config().self_seat);
        bytes.push_back(static_cast<unsigned char>(link.lane));
        bytes.push_back(initiator_signature ? 1 : 0);
        return Digest("B3FlowMeshSession/v1", bytes);
    }

    void Accept()
    {
        for (size_t i{0}; i < m_config.max_pending_connections; ++i) {
            const int fd{accept(m_listener, nullptr, nullptr)};
            if (fd < 0) {
                if (errno == EINTR) continue;
                if (!WouldBlock(errno)) m_closed = true;
                break;
            }
            if (m_pending.size() >= m_config.max_pending_connections || !Nonblocking(fd)) {
                (void)close(fd);
                continue;
            }
            ConfigureSocket(fd);
            m_pending.push_back(std::make_unique<Link>(fd, false, 0, Lane::BULK));
        }
    }

    void Dial(uint32_t seat, Lane lane)
    {
        Channel& c{At(seat, lane)};
        c.retry = Clock::now() + std::chrono::milliseconds{m_config.reconnect_ms};
        const SeatEndpoint& endpoint{m_endpoints[seat]};
        const std::string& text{endpoint.addresses[c.next_address++ % endpoint.addresses.size()]};
        const auto address{ParseAddress(text, endpoint.port)};
        if (!address) return; // Create already validates every pinned address.
        const int fd{socket(address->storage.ss_family, SOCK_STREAM, 0)};
        if (fd < 0) return;
        if (!Nonblocking(fd)) { (void)close(fd); return; }
        ConfigureSocket(fd);
        const int connected{connect(fd, reinterpret_cast<const sockaddr*>(&address->storage), address->size)};
        if (connected != 0 && errno != EINPROGRESS) { (void)close(fd); return; }
        c.link = std::make_unique<Link>(fd, true, seat, lane);
        c.link->connecting = connected != 0;
    }

    void MakeHello(Link& link)
    {
        MeshHello hello{link.lane, Config().self_seat, link.peer, {}};
        const auto key{link.cipher->GetOurPubKey()};
        std::transform(key.begin(), key.end(), hello.ellswift.begin(), [](std::byte b) { return static_cast<unsigned char>(b); });
        const auto bytes{EncodeHello(hello)};
        link.wire.assign(bytes.begin(), bytes.end());
        link.sending = Link::Sending::HELLO;
    }
    void Encrypt(Link& link, std::span<const unsigned char> bytes)
    {
        link.wire.resize(bytes.size() + BIP324Cipher::EXPANSION);
        link.cipher->Encrypt(std::as_bytes(bytes), {}, false,
            std::as_writable_bytes(std::span{link.wire}));
        link.wire_pos = 0;
    }
    void MakeWire(Link& link)
    {
        if (!link.hello_sent) {
            // A listener learns the lane and claimed seat from the dialer's
            // hello. None of that data becomes authenticated until BIP340.
            if (!link.initiator && !link.hello_received) return;
            MakeHello(link);
        } else if (link.hello_received && !link.auth_sent) {
            const auto signature{m_config.sign_session(SessionDigest(link, link.initiator))};
            if (!signature || !XOnlyPubKey{m_config.identity_keys[Config().self_seat]}.VerifySchnorr(
                    SessionDigest(link, link.initiator), *signature)) {
                link.dead = true;
                return;
            }
            Encrypt(link, *signature);
            link.sending = Link::Sending::AUTH;
        } else if (link.Authenticated()) {
            if (!link.initiator && At(link.peer, link.lane).link.get() != &link) return;
            const Channel& c{At(link.peer, link.lane)};
            if (c.outgoing.empty()) return;
            Encrypt(link, c.outgoing.front());
            link.sending = Link::Sending::FRAME;
        }
    }
    void Written(Link& link)
    {
        if (link.sending == Link::Sending::HELLO) link.hello_sent = true;
        if (link.sending == Link::Sending::AUTH) link.auth_sent = true;
        if (link.sending == Link::Sending::FRAME) {
            Channel& c{At(link.peer, link.lane)};
            const size_t index{Index(link.lane)};
            m_out_bytes[index] -= c.outgoing.front().size();
            --m_out_messages[index];
            c.outgoing_bytes -= c.outgoing.front().size();
            --c.outgoing_messages;
            c.outgoing.pop_front();
        }
        link.wire.clear();
        link.wire_pos = 0;
        link.sending = Link::Sending::NONE;
    }
    void Write(Link& link)
    {
        size_t budget{IO_BUDGET};
        while (budget && !link.dead) {
            if (link.wire.empty()) MakeWire(link);
            if (link.wire.empty() || link.dead) return;
            const size_t amount{std::min(budget, link.wire.size() - link.wire_pos)};
            const ssize_t n{send(link.fd, link.wire.data() + link.wire_pos, amount, MSG_NOSIGNAL)};
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && WouldBlock(errno)) return;
                link.dead = true;
                return;
            }
            budget -= n;
            link.wire_pos += n;
            if (link.wire_pos == link.wire.size()) Written(link);
        }
    }
    bool ReadInto(Link& link, unsigned char* target, size_t total, size_t& position, size_t& budget)
    {
        while (position < total && budget) {
            const ssize_t n{recv(link.fd, target + position, std::min(budget, total - position), 0)};
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && WouldBlock(errno)) return false;
                link.dead = true;
                return false;
            }
            position += n;
            budget -= n;
        }
        return position == total;
    }
    void ReadHello(Link& link, size_t& budget)
    {
        link.hello.resize(HELLO_BYTES);
        if (!ReadInto(link, link.hello.data(), HELLO_BYTES, link.packet_pos, budget)) return;
        const auto& h{link.hello};
        const auto hello{CheckHello(std::span<const unsigned char, HELLO_BYTES>{h.data(), HELLO_BYTES},
            HelloExpectation{Config().self_seat, Config().seat_count, link.initiator, link.peer, link.lane})};
        if (!hello) {
            link.dead = true;
            return;
        }
        link.peer = hello->from_seat;
        link.lane = hello->lane;
        // An existing socket remains active until the replacement proves its
        // identity. Merely claiming this seat or lane cannot evict it.
        link.cipher->InitializeForNetwork(EllSwiftPubKey{std::as_bytes(std::span{h}.subspan(10))},
            link.initiator, m_config.network_magic);
        link.hello_received = true;
        link.packet_pos = 0;
        link.hello.clear();
    }
    void Read(Link& link)
    {
        size_t budget{IO_BUDGET};
        if (!link.hello_received) ReadHello(link, budget);
        if (link.dead || !link.hello_received) return;
        while (budget && !link.dead) {
            const size_t index{Index(link.lane)};
            Channel& channel{At(link.peer, link.lane)};
            if (link.Authenticated() && !link.input_reserved && (m_in_messages[index] >= m_config.max_messages_per_lane ||
                channel.incoming_messages >= PeerMessages())) return;
            if (!link.packet_length) {
                if (!ReadInto(link, link.length.data(), link.length.size(), link.length_pos, budget)) return;
                const size_t length{link.cipher->DecryptLength(std::as_bytes(std::span{link.length}))};
                if (length > Config().max_frame_bytes || (!link.auth_received && length != 64)) {
                    link.dead = true;
                    return;
                }
                link.packet_length = length;
            }
            if (link.auth_received && !link.input_reserved) {
                if (m_in_messages[index] >= m_config.max_messages_per_lane ||
                    *link.packet_length > m_config.max_queued_bytes_per_lane - m_in_bytes[index] ||
                    channel.incoming_messages >= PeerMessages() || *link.packet_length > PeerBytes() - channel.incoming_bytes) return;
                // Reserve BEFORE allocating a partial input packet. Otherwise
                // many slow authenticated peers could each hold one max-size
                // body while only complete frames counted against the cap.
                m_in_bytes[index] += *link.packet_length;
                ++m_in_messages[index];
                channel.incoming_bytes += *link.packet_length;
                ++channel.incoming_messages;
                link.input_reserved = true;
            }
            const size_t encrypted{*link.packet_length + BIP324Cipher::EXPANSION - BIP324Cipher::LENGTH_LEN};
            if (link.packet.empty()) link.packet.resize(encrypted);
            if (!ReadInto(link, link.packet.data(), encrypted, link.packet_pos, budget)) return;
            std::vector<unsigned char> plain(*link.packet_length);
            bool ignore{false};
            if (!link.cipher->Decrypt(std::as_bytes(std::span{link.packet}), {}, ignore,
                    std::as_writable_bytes(std::span{plain})) || ignore) {
                link.dead = true;
                return;
            }
            bool activate_after_packet{false};
            if (!link.auth_received) {
                if (!XOnlyPubKey{m_config.identity_keys[link.peer]}.VerifySchnorr(
                        SessionDigest(link, !link.initiator), plain)) {
                    link.dead = true;
                    return;
                }
                link.auth_received = true;
                // An accepted socket is installed in its unique peer/lane
                // channel by Poll before any application packet is consumed.
                activate_after_packet = !link.initiator && At(link.peer, link.lane).link.get() != &link;
            } else {
                // An initiator may send data only after seeing our auth; a
                // receiver likewise waits for its own auth to leave the socket.
                if (!link.Authenticated()) { link.dead = true; return; }
                At(link.peer, link.lane).incoming.push_back(std::move(plain));
                link.input_reserved = false; // reservation now belongs to the queue
            }
            link.packet.clear();
            link.packet_pos = 0;
            link.packet_length.reset();
            link.length_pos = 0;
            if (activate_after_packet) return;
        }
    }
    void Step(Link& link)
    {
        if (!link.Authenticated() && Clock::now() - link.born > std::chrono::milliseconds{m_config.handshake_timeout_ms}) {
            link.dead = true;
            return;
        }
        if (link.connecting) {
            pollfd p{link.fd, POLLOUT, 0};
            if (poll(&p, 1, 0) <= 0) return;
            int error{0};
            socklen_t size{sizeof(error)};
            if (getsockopt(link.fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0) {
                link.dead = true;
                return;
            }
            link.connecting = false;
        }
        Write(link);
        if (!link.dead) Read(link);
        if (!link.dead) Write(link);
    }

    const TcpMeshConfig m_config;
    int m_listener{-1};
    bool m_closed{false};
    std::vector<Channel> m_channels;
    std::vector<SeatEndpoint> m_endpoints;
    std::vector<std::unique_ptr<Link>> m_pending;
    std::array<size_t, LANE_COUNT> m_in_bytes{}, m_out_bytes{}, m_in_messages{}, m_out_messages{};
    std::array<uint32_t, LANE_COUNT> m_next_peer{};
    size_t m_service_cursor{0};
};
#endif // !_WIN32
} // namespace

uint256 TcpSeatMesh::EndpointDigest(const TcpMeshConfig& config, const SeatEndpoint& endpoint)
{
    std::vector<unsigned char> bytes;
    Common(bytes, config);
    Put32(bytes, endpoint.seat);
    Put64(bytes, endpoint.valid_from_epoch);
    Put64(bytes, endpoint.counter);
    Put32(bytes, endpoint.port);
    Put32(bytes, endpoint.addresses.size());
    for (const auto& address : endpoint.addresses) {
        Put32(bytes, address.size());
        bytes.insert(bytes.end(), address.begin(), address.end());
    }
    return Digest("B3FlowMeshEndpoint/v1", bytes);
}

Result<std::unique_ptr<SeatMesh>> TcpSeatMesh::Create(const TcpMeshConfig& config)
{
    const auto checked{SeatMesh::CheckConfig(config.mesh)};
    if (!checked) return checked.error();
    if (config.mesh.profile.chain != ChainType::REGTEST ||
        config.network_magic != std::array<unsigned char, 4>{0xfa, 0xbf, 0xb5, 0xda}) {
        return Error{ErrorCode::UNSUPPORTED_NETWORK, "TCP qualification is regtest only"};
    }
#ifdef _WIN32
    return Error{ErrorCode::FENCED, "authenticated TCP qualification requires the Linux runtime"};
#else
    if (config.mesh.seat_count > 128 || config.mesh.max_frame_bytes < 64 || config.mesh.max_frame_bytes > 0xffffff ||
        config.identity_keys.size() != config.mesh.seat_count || config.endpoints.size() != config.mesh.seat_count ||
        config.minimum_endpoint_counters.size() != config.mesh.seat_count || !config.sign_session ||
        config.domain.IsNull() || config.seat_set_hash.IsNull() || config.network_magic == std::array<unsigned char, 4>{} ||
        config.max_pending_connections == 0 || config.max_pending_connections > 128 ||
        config.max_messages_per_lane == 0 || config.max_messages_per_lane > 4096 ||
        config.max_queued_bytes_per_lane < config.mesh.max_frame_bytes || config.max_queued_bytes_per_lane > 64 * 1024 * 1024 ||
        config.max_messages_per_lane < std::max<uint32_t>(1, config.mesh.seat_count - 1) ||
        config.max_queued_bytes_per_lane / std::max<uint32_t>(1, config.mesh.seat_count - 1) < config.mesh.max_frame_bytes ||
        config.handshake_timeout_ms < 10 || config.handshake_timeout_ms > 60000 || config.reconnect_ms == 0 ||
        config.reconnect_ms > 60000 || config.listen_port == 0) {
        return Error{ErrorCode::INVALID_INPUT, CONFIG_BAD};
    }
    std::set<std::array<unsigned char, 32>> keys;
    for (const auto& key : config.identity_keys) {
        if (!XOnlyPubKey{key}.IsFullyValid() || !keys.insert(key).second) return Error{ErrorCode::INVALID_INPUT, CONFIG_BAD};
    }
    std::set<uint32_t> seats;
    for (const auto& endpoint : config.endpoints) {
        if (endpoint.seat >= config.mesh.seat_count || !seats.insert(endpoint.seat).second || endpoint.port == 0 ||
            endpoint.valid_from_epoch != config.epoch || endpoint.counter < config.minimum_endpoint_counters[endpoint.seat] ||
            endpoint.addresses.empty() || endpoint.addresses.size() > 4 ||
            !XOnlyPubKey{config.identity_keys[endpoint.seat]}.VerifySchnorr(EndpointDigest(config, endpoint), endpoint.identity_signature)) {
            return Error{ErrorCode::VERIFY_FAILED, ENDPOINT_BAD};
        }
        for (const auto& address : endpoint.addresses) {
            if (address.size() > 45 || !ParseAddress(address, endpoint.port)) return Error{ErrorCode::INVALID_INPUT, ENDPOINT_BAD};
        }
        if (endpoint.seat == config.mesh.self_seat && endpoint.port != config.listen_port) return Error{ErrorCode::INVALID_INPUT, CONFIG_BAD};
    }
    const auto address{ParseAddress(config.listen_address, config.listen_port)};
    if (!address) return Error{ErrorCode::INVALID_INPUT, CONFIG_BAD};
    const int fd{socket(address->storage.ss_family, SOCK_STREAM, 0)};
    if (fd < 0) return Error{ErrorCode::BUSY, SOCKET_BAD};
    const int one{1};
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (!Nonblocking(fd) || bind(fd, reinterpret_cast<const sockaddr*>(&address->storage), address->size) != 0 ||
        listen(fd, std::min<size_t>(config.max_pending_connections, 128)) != 0) {
        (void)close(fd);
        return Error{ErrorCode::BUSY, SOCKET_BAD};
    }
    return Ok(std::unique_ptr<SeatMesh>{new SocketSeatMesh{config, fd}});
#endif
}
} // namespace flowmesh2::net
