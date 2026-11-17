// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/net/tcp_mesh.h>

#include <compat/compat.h>
#include <flowmesh2/netio/bip324_session.h>
#include <flowmesh2/netio/mesh_handshake.h>
#include <key.h>
#include <netaddress.h>
#include <netbase.h>
#include <pubkey.h>
#include <sync.h>
#include <util/sock.h>

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <thread>
#include <utility>

namespace flowmesh2::net {

namespace {

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

constexpr size_t READ_CHUNK{64 * 1024};
constexpr int LISTEN_BACKLOG{32};


enum class ConnState : uint8_t {
    CONNECTING,   //!< the dialer waits for the TCP connect to finish
    KEY_EXCHANGE, //!< our ElligatorSwift key is out; theirs is awaited
    HELLO_WAIT,   //!< the channel is keyed; the peer's HELLO is awaited (the dialer sent its own already)
    OPEN,         //!< authenticated: frames flow
};

struct Conn {
    std::shared_ptr<Sock> sock;
    bool initiator{false};
    ConnState state{ConnState::KEY_EXCHANGE};
    std::optional<uint32_t> peer; //!< the seat dialed, or the one the HELLO proved
    Lane lane{Lane::BULK};        //!< the lane dialed, or the HELLO's
    netio::SecureChannel channel;
    std::vector<unsigned char> rbuf;
    std::vector<unsigned char> wbuf;
    size_t wpos{0};
    std::optional<uint32_t> pending_len; //!< the decrypted length of the packet being read
    Clock::time_point opened_at;
    Clock::time_point deadline;         //!< the handshake deadline
    Clock::time_point last_frame_at;    //!< the last authenticated frame RECEIVED (outbound frames prove nothing about the peer)
    Clock::time_point last_progress_at; //!< the last time queued bytes left for the socket (the send-stall clock)
    bool closed{false};
    std::string why;
};

struct Dial {
    uint32_t seat{0};
    Lane lane{Lane::BULK};
    Clock::time_point next_attempt;
    uint64_t backoff_ms{0};
    size_t address_index{0};
    bool active{false}; //!< a connection for this (seat, lane) exists (any state)
};

std::optional<CService> ServiceOf(const std::string& address, uint16_t port)
{
    const CService service{LookupNumeric(address, port)};
    if (!service.IsValid() || service.GetPort() == 0) return std::nullopt;
    return service;
}

} // namespace

struct TcpSeatMesh::Impl {
    explicit Impl(const TcpMeshConfig& config) : cfg{config}, seats{*config.seats} {}

    const TcpMeshConfig cfg;
    const SeatSet seats;
    CKey transport_key;
    std::array<unsigned char, MESH_TRANSPORT_KEY_BYTES> our_transport_pubkey{};
    std::map<uint32_t, SeatEndpointRecord> records; //!< by seat, the highest counter
    std::shared_ptr<Sock> listener;
    uint16_t listen_port{0};
    std::shared_ptr<Sock> wake_read;
    int wake_read_fd{-1}; //!< the pipe's read end, drained with read() (recv fails on a pipe)
    int wake_write{-1};

    // The network thread's own state (no lock: only it touches these).
    std::vector<std::unique_ptr<Conn>> conns;
    std::vector<Dial> dials;

    // Shared with Transmit, Poll and Status.
    mutable Mutex mutex;
    std::map<std::pair<uint32_t, Lane>, std::deque<std::vector<unsigned char>>> outboxes GUARDED_BY(mutex);
    std::array<std::deque<InboundFrame>, LANE_COUNT> inboxes GUARDED_BY(mutex);
    std::vector<size_t> inbox_per_peer GUARDED_BY(mutex);
    std::vector<TcpPeerStatus> peer_status GUARDED_BY(mutex);
    size_t connection_count GUARDED_BY(mutex){0};
    uint64_t steps GUARDED_BY(mutex){0};
    bool drop_all GUARDED_BY(mutex){false};
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::thread thread;

    Result<void> Start() EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    void Run() EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    void Step() EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    void Wake();
    void AcceptAll() EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    void DialDue() EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    void OnReadable(Conn& conn) EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    void OnWritable(Conn& conn) EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    void Process(Conn& conn) EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    void OnHello(Conn& conn, std::span<const unsigned char> bytes) EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    void SendHello(Conn& conn);
    void Queue(Conn& conn, std::span<const unsigned char> bytes); //!< raw bytes to the socket buffer
    void QueuePacket(Conn& conn, std::span<const unsigned char> contents);
    void PumpOutboxes() EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    void Close(Conn& conn, std::string why) EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    void Reap() EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    Conn* OpenConn(uint32_t seat, Lane lane);
    Dial* DialFor(uint32_t seat, Lane lane);
    void Refused(std::optional<uint32_t> peer) EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    bool InboxFull(uint32_t peer) EXCLUSIVE_LOCKS_REQUIRED(!mutex);
};

// ---- Lifecycle ----------------------------------------------------------------------

Result<std::unique_ptr<TcpSeatMesh>> TcpSeatMesh::Create(const TcpMeshConfig& config)
{
    if (const Result<void> ok{CheckConfig(config.mesh)}; !ok.has_value()) return ok.error();
    if (!config.seats) return Error{ErrorCode::INVALID_INPUT, TCP_MESH_SEATS_MISSING};
    if (config.seats->Size() != config.mesh.seat_count) return Error{ErrorCode::INVALID_INPUT, MESH_CONFIG_INVALID};
    auto impl{std::make_unique<Impl>(config)};
    impl->transport_key.Set(config.transport_secret.begin(), config.transport_secret.end(), true);
    if (!impl->transport_key.IsValid()) return Error{ErrorCode::INVALID_INPUT, TCP_MESH_TRANSPORT_KEY};
    const XOnlyPubKey ours{impl->transport_key.GetPubKey()};
    std::copy(ours.begin(), ours.end(), impl->our_transport_pubkey.begin());
    for (const SeatEndpointRecord& record : config.endpoints) {
        if (const Result<void> ok{netio::VerifyEndpointRecord(record, impl->seats, config.domain)}; !ok.has_value()) return ok.error();
        const auto it{impl->records.find(record.seat)};
        if (it == impl->records.end() || it->second.counter < record.counter) impl->records[record.seat] = record;
    }
    for (uint32_t seat{0}; seat < config.mesh.seat_count; ++seat) {
        if (!impl->records.contains(seat)) return Error{ErrorCode::INVALID_INPUT, TCP_MESH_ENDPOINTS_MISSING};
    }
    if (impl->records.at(config.mesh.self_seat).transport_key != impl->our_transport_pubkey) {
        return Error{ErrorCode::INVALID_INPUT, TCP_MESH_OWN_ENDPOINT};
    }
    {
        LOCK(impl->mutex);
        impl->inbox_per_peer.assign(config.mesh.seat_count, 0);
        impl->peer_status.clear();
        for (uint32_t seat{0}; seat < config.mesh.seat_count; ++seat) {
            if (seat == config.mesh.self_seat) continue;
            TcpPeerStatus status;
            status.seat = seat;
            impl->peer_status.push_back(status);
        }
    }
    if (const Result<void> started{impl->Start()}; !started.has_value()) return started.error();
    std::unique_ptr<TcpSeatMesh> mesh{new TcpSeatMesh{config.mesh, std::move(impl)}};
    return Ok(std::move(mesh));
}

TcpSeatMesh::TcpSeatMesh(const MeshConfig& config, std::unique_ptr<Impl> impl) : SeatMesh{config}, m_impl{std::move(impl)} {}

TcpSeatMesh::~TcpSeatMesh() { Stop(); }

Result<void> TcpSeatMesh::Impl::Start()
{
    // The listening socket.
    const std::optional<CService> bind{ServiceOf(cfg.listen_address, cfg.listen_port == 0 ? 1 : cfg.listen_port)};
    if (!bind.has_value()) return Error{ErrorCode::STORAGE_FATAL, TCP_MESH_BIND};
    CService bind_to{*bind};
    if (cfg.listen_port == 0) bind_to = CService{static_cast<CNetAddr>(*bind), 0};
    std::unique_ptr<Sock> sock{CreateSockOS(bind_to.GetSAFamily(), SOCK_STREAM, IPPROTO_TCP)};
    if (!sock) return Error{ErrorCode::STORAGE_FATAL, TCP_MESH_BIND};
    const int one{1};
    (void)sock->SetSockOpt(SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_storage storage{};
    socklen_t len{sizeof(storage)};
    if (!bind_to.GetSockAddr(reinterpret_cast<struct sockaddr*>(&storage), &len)) return Error{ErrorCode::STORAGE_FATAL, TCP_MESH_BIND};
    if (sock->Bind(reinterpret_cast<struct sockaddr*>(&storage), len) == SOCKET_ERROR) return Error{ErrorCode::STORAGE_FATAL, TCP_MESH_BIND};
    if (sock->Listen(LISTEN_BACKLOG) == SOCKET_ERROR) return Error{ErrorCode::STORAGE_FATAL, TCP_MESH_BIND};
    struct sockaddr_storage bound{};
    socklen_t bound_len{sizeof(bound)};
    if (sock->GetSockName(reinterpret_cast<struct sockaddr*>(&bound), &bound_len) == SOCKET_ERROR) return Error{ErrorCode::STORAGE_FATAL, TCP_MESH_BIND};
    CService bound_service;
    if (!bound_service.SetSockAddr(reinterpret_cast<const struct sockaddr*>(&bound), bound_len)) return Error{ErrorCode::STORAGE_FATAL, TCP_MESH_BIND};
    listen_port = bound_service.GetPort();
    listener = std::shared_ptr<Sock>{std::move(sock)};

    // The wake pipe.
    int fds[2];
    if (::pipe(fds) != 0) return Error{ErrorCode::STORAGE_FATAL, TCP_MESH_WAKE};
    wake_read = std::make_shared<Sock>(static_cast<SOCKET>(fds[0]));
    wake_read_fd = fds[0];
    wake_write = fds[1];
    if (!wake_read->SetNonBlocking()) return Error{ErrorCode::STORAGE_FATAL, TCP_MESH_WAKE};

    // Dial the higher seats on every lane.
    for (uint32_t seat{cfg.mesh.self_seat + 1}; seat < cfg.mesh.seat_count; ++seat) {
        for (const Lane lane : LANES_BY_PRIORITY) {
            Dial dial;
            dial.seat = seat;
            dial.lane = lane;
            dial.next_attempt = Clock::now();
            dials.push_back(dial);
        }
    }
    running = true;
    thread = std::thread{[this] { Run(); }};
    return Ok();
}

void TcpSeatMesh::Stop()
{
    if (!m_impl) return;
    if (m_impl->running.exchange(false)) {
        m_impl->stop = true;
        m_impl->Wake();
        if (m_impl->thread.joinable()) m_impl->thread.join();
        m_impl->conns.clear();
        m_impl->listener.reset();
        m_impl->wake_read.reset();
        if (m_impl->wake_write >= 0) {
            ::close(m_impl->wake_write);
            m_impl->wake_write = -1;
        }
    }
}

void TcpSeatMesh::Impl::Wake()
{
    if (wake_write < 0) return;
    const unsigned char byte{1};
    // Best effort: the pipe is non-blocking, and a full pipe means the loop is awake
    // already. glibc marks write() warn_unused_result and GCC does not accept a (void)
    // cast as a use, so the result is kept (fm2_warnings makes it an error).
    [[maybe_unused]] const ssize_t written{::write(wake_write, &byte, 1)};
}

void TcpSeatMesh::Impl::Run()
{
    while (!stop) Step();
}

// ---- The network thread ------------------------------------------------------------------

void TcpSeatMesh::Impl::Step()
{
    bool drop{false};
    {
        LOCK(mutex);
        drop = drop_all;
        drop_all = false;
    }
    if (drop) {
        for (auto& conn : conns) Close(*conn, "dropped by request");
    }
    Reap();
    PumpOutboxes();

    // At the stranger cap the listener leaves the poll set: pending connections wait in the kernel's backlog instead of
    // waking the thread at once, over and over (review 2026-11-16: the cap used to spin the network thread).
    size_t unauthenticated{0};
    for (const auto& conn : conns) {
        if (!conn->closed && conn->state != ConnState::OPEN && !conn->initiator) ++unauthenticated;
    }
    Sock::EventsPerSock events;
    if (unauthenticated < cfg.limits.max_unauthenticated) events.emplace(listener, Sock::Events{Sock::RECV});
    events.emplace(wake_read, Sock::Events{Sock::RECV});
    const Clock::time_point now{Clock::now()};
    Clock::time_point next_wake{now + Ms{static_cast<int64_t>(cfg.limits.poll_interval_ms)}};
    for (auto& conn : conns) {
        Sock::Event wanted{0};
        if (conn->state == ConnState::CONNECTING) {
            wanted = Sock::SEND;
        } else {
            bool full{false};
            if (conn->state == ConnState::OPEN && conn->peer.has_value()) full = InboxFull(*conn->peer);
            if (!full) wanted |= Sock::RECV;
            if (conn->wpos < conn->wbuf.size()) wanted |= Sock::SEND;
        }
        if (wanted != 0) events.emplace(conn->sock, Sock::Events{wanted});
        if (conn->state != ConnState::OPEN) next_wake = std::min(next_wake, conn->deadline);
    }
    for (const Dial& dial : dials) {
        if (!dial.active) next_wake = std::min(next_wake, dial.next_attempt);
    }
    const Ms timeout{std::max<int64_t>(1, std::chrono::duration_cast<Ms>(next_wake - now).count())};
    (void)listener->WaitMany(timeout, events);

    // The wake pipe: drain it with read(), not recv(): recv fails on a pipe (ENOTSOCK), so the byte Wake() wrote was never
    // taken out, the pipe stayed readable and the thread spun from the first Transmit on (found by the step counter the
    // review fixes added, 2026-11-17).
    if (const auto it{events.find(wake_read)}; it != events.end() && (it->second.occurred & Sock::RECV) != 0) {
        unsigned char buf[64];
        while (::read(wake_read_fd, buf, sizeof(buf)) > 0) {
        }
    }
    if (const auto it{events.find(listener)}; it != events.end() && (it->second.occurred & Sock::RECV) != 0) AcceptAll();
    const Clock::time_point after{Clock::now()};
    for (auto& conn : conns) {
        if (conn->closed) continue;
        const auto it{events.find(conn->sock)};
        const Sock::Event occurred{it == events.end() ? Sock::Event{0} : it->second.occurred};
        if (conn->state == ConnState::CONNECTING) {
            if ((occurred & (Sock::SEND | Sock::ERR)) != 0) {
                int error{0};
                socklen_t len{sizeof(error)};
                if (conn->sock->GetSockOpt(SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error != 0) {
                    Close(*conn, "connect failed");
                    continue;
                }
                conn->state = ConnState::KEY_EXCHANGE;
                const EllSwiftPubKey& ours{conn->channel.OurPubKey()};
                Queue(*conn, std::span<const unsigned char>{reinterpret_cast<const unsigned char*>(ours.data()), ours.size()});
            }
        } else {
            if ((occurred & Sock::ERR) != 0) {
                Close(*conn, "socket error");
                continue;
            }
            if ((occurred & Sock::RECV) != 0) OnReadable(*conn);
            if (conn->closed) continue;
            if ((occurred & Sock::SEND) != 0 || conn->wpos < conn->wbuf.size()) OnWritable(*conn);
        }
        if (!conn->closed && conn->state != ConnState::OPEN && after > conn->deadline) Close(*conn, "handshake timeout");
        // Queued bytes that make no progress for send_stall_ms: the peer does not read (or the connection is dead).
        if (!conn->closed && conn->wpos < conn->wbuf.size() && after > conn->last_progress_at + Ms{static_cast<int64_t>(cfg.limits.send_stall_ms)}) {
            if (conn->peer.has_value()) {
                LOCK(mutex);
                for (TcpPeerStatus& status : peer_status) {
                    if (status.seat == *conn->peer) ++status.stalls;
                }
            }
            Close(*conn, "peer does not read");
        }
    }
    // Bytes read earlier but left unprocessed because the peer's inbox was full: the inbox may have drained since.
    for (auto& conn : conns) {
        if (!conn->closed && conn->state == ConnState::OPEN && !conn->rbuf.empty() && conn->peer.has_value() && !InboxFull(*conn->peer)) Process(*conn);
    }
    Reap();
    DialDue();
    PumpOutboxes();
    for (auto& conn : conns) {
        if (!conn->closed && conn->wpos < conn->wbuf.size()) OnWritable(*conn);
    }
    Reap();
    // Status samples (one lock per step): queued bytes per peer, inbox depth per peer, the step count.
    {
        LOCK(mutex);
        ++steps;
        for (TcpPeerStatus& status : peer_status) {
            status.pending_bytes = 0;
            for (const auto& conn : conns) {
                if (!conn->closed && conn->peer == status.seat) status.pending_bytes += conn->wbuf.size() - conn->wpos;
            }
            status.inbox_frames = inbox_per_peer[status.seat];
        }
    }
}

void TcpSeatMesh::Impl::AcceptAll()
{
    for (;;) {
        size_t unauthenticated{0};
        for (const auto& conn : conns) {
            if (!conn->closed && conn->state != ConnState::OPEN && !conn->initiator) ++unauthenticated;
        }
        if (unauthenticated >= cfg.limits.max_unauthenticated) return;
        struct sockaddr_storage storage{};
        socklen_t len{sizeof(storage)};
        std::unique_ptr<Sock> accepted{listener->Accept(reinterpret_cast<struct sockaddr*>(&storage), &len)};
        if (!accepted) return;
        if (!accepted->SetNonBlocking()) continue;
        auto conn{std::make_unique<Conn>()};
        conn->sock = std::shared_ptr<Sock>{std::move(accepted)};
        conn->initiator = false;
        conn->state = ConnState::KEY_EXCHANGE;
        conn->opened_at = Clock::now();
        conn->deadline = conn->opened_at + Ms{static_cast<int64_t>(cfg.limits.handshake_timeout_ms)};
        const EllSwiftPubKey& ours{conn->channel.OurPubKey()};
        Queue(*conn, std::span<const unsigned char>{reinterpret_cast<const unsigned char*>(ours.data()), ours.size()});
        conns.push_back(std::move(conn));
        LOCK(mutex);
        ++connection_count;
    }
}

Dial* TcpSeatMesh::Impl::DialFor(uint32_t seat, Lane lane)
{
    for (Dial& dial : dials) {
        if (dial.seat == seat && dial.lane == lane) return &dial;
    }
    return nullptr;
}

void TcpSeatMesh::Impl::DialDue()
{
    const Clock::time_point now{Clock::now()};
    for (Dial& dial : dials) {
        if (dial.active || now < dial.next_attempt) continue;
        const SeatEndpointRecord& record{records.at(dial.seat)};
        const std::string& address{record.addresses[dial.address_index % record.addresses.size()]};
        ++dial.address_index;
        dial.backoff_ms = dial.backoff_ms == 0 ? cfg.limits.reconnect_min_ms : std::min(dial.backoff_ms * 2, cfg.limits.reconnect_max_ms);
        dial.next_attempt = now + Ms{static_cast<int64_t>(dial.backoff_ms)};
        const std::optional<CService> service{ServiceOf(address, record.port)};
        if (!service.has_value()) continue;
        std::unique_ptr<Sock> sock{CreateSockOS(service->GetSAFamily(), SOCK_STREAM, IPPROTO_TCP)};
        if (!sock) continue;
        struct sockaddr_storage storage{};
        socklen_t len{sizeof(storage)};
        if (!service->GetSockAddr(reinterpret_cast<struct sockaddr*>(&storage), &len)) continue;
        auto conn{std::make_unique<Conn>()};
        conn->sock = std::shared_ptr<Sock>{std::move(sock)};
        conn->initiator = true;
        conn->peer = dial.seat;
        conn->lane = dial.lane;
        conn->opened_at = now;
        conn->deadline = now + Ms{static_cast<int64_t>(cfg.limits.handshake_timeout_ms)};
        const int rc{conn->sock->Connect(reinterpret_cast<struct sockaddr*>(&storage), len)};
        if (rc == SOCKET_ERROR) {
            const int err{WSAGetLastError()};
            if (err != WSAEINPROGRESS && err != WSAEWOULDBLOCK && err != EINTR) continue;
            conn->state = ConnState::CONNECTING;
        } else {
            conn->state = ConnState::KEY_EXCHANGE;
            const EllSwiftPubKey& ours{conn->channel.OurPubKey()};
            Queue(*conn, std::span<const unsigned char>{reinterpret_cast<const unsigned char*>(ours.data()), ours.size()});
        }
        dial.active = true;
        conns.push_back(std::move(conn));
        LOCK(mutex);
        ++connection_count;
    }
}

void TcpSeatMesh::Impl::OnReadable(Conn& conn)
{
    unsigned char buf[READ_CHUNK];
    // This connection's share of the step: once the budget is spent the socket stays readable and the next step comes
    // back to it, after every other connection, dial and deadline had its turn (review 2026-11-16, defect 4a). A full
    // inbox holds the socket back here too, not only when the poll set is built.
    size_t budget{cfg.limits.read_bytes_per_step};
    for (;;) {
        if (budget == 0) return;
        if (conn.state == ConnState::OPEN && conn.peer.has_value() && InboxFull(*conn.peer)) return;
        const size_t want{std::min(sizeof(buf), budget)};
        const ssize_t n{conn.sock->Recv(buf, want, 0)};
        if (n > 0) {
            budget -= static_cast<size_t>(n);
            conn.rbuf.insert(conn.rbuf.end(), buf, buf + n);
            if (conn.rbuf.size() > cfg.mesh.max_frame_bytes + netio::SecureChannel::EXPANSION + READ_CHUNK) {
                Close(conn, "peer sends faster than we read");
                return;
            }
            Process(conn);
            if (conn.closed) return;
            if (static_cast<size_t>(n) < want) return;
            continue;
        }
        if (n == 0) {
            Close(conn, "peer closed");
            return;
        }
        const int err{WSAGetLastError()};
        if (err == WSAEWOULDBLOCK || err == WSAEINTR || err == WSAEINPROGRESS) return;
        Close(conn, "recv failed");
        return;
    }
}

void TcpSeatMesh::Impl::OnWritable(Conn& conn)
{
    while (conn.wpos < conn.wbuf.size()) {
        const ssize_t n{conn.sock->Send(conn.wbuf.data() + conn.wpos, conn.wbuf.size() - conn.wpos, MSG_NOSIGNAL)};
        if (n > 0) {
            conn.wpos += static_cast<size_t>(n);
            conn.last_progress_at = Clock::now();
            continue;
        }
        const int err{WSAGetLastError()};
        if (n < 0 && (err == WSAEWOULDBLOCK || err == WSAEINTR || err == WSAEINPROGRESS)) break;
        Close(conn, "send failed");
        return;
    }
    if (conn.wpos == conn.wbuf.size()) {
        conn.wbuf.clear();
        conn.wpos = 0;
    } else if (conn.wpos > READ_CHUNK) {
        conn.wbuf.erase(conn.wbuf.begin(), conn.wbuf.begin() + static_cast<std::ptrdiff_t>(conn.wpos));
        conn.wpos = 0;
    }
}

void TcpSeatMesh::Impl::Queue(Conn& conn, std::span<const unsigned char> bytes)
{
    if (conn.wpos == conn.wbuf.size()) conn.last_progress_at = Clock::now(); // nothing was pending: the stall clock starts now
    conn.wbuf.insert(conn.wbuf.end(), bytes.begin(), bytes.end());
}

void TcpSeatMesh::Impl::QueuePacket(Conn& conn, std::span<const unsigned char> contents)
{
    const std::vector<unsigned char> packet{conn.channel.Encrypt(contents)};
    Queue(conn, packet);
}

void TcpSeatMesh::Impl::SendHello(Conn& conn)
{
    netio::MeshHello hello;
    hello.from_seat = cfg.mesh.self_seat;
    hello.to_seat = *conn.peer;
    hello.lane = conn.lane;
    hello.epoch = cfg.epoch;
    hello.seat_set_root = seats.Root();
    hello.transport_key = our_transport_pubkey;
    if (!netio::SignHello(transport_key, conn.channel.SessionId(), cfg.magic, hello)) return;
    QueuePacket(conn, hello.Encode());
}

void TcpSeatMesh::Impl::Process(Conn& conn)
{
    for (;;) {
        if (conn.closed) return;
        if (conn.state == ConnState::KEY_EXCHANGE) {
            if (conn.rbuf.size() < EllSwiftPubKey::size()) return;
            const EllSwiftPubKey theirs{std::span<const std::byte>{reinterpret_cast<const std::byte*>(conn.rbuf.data()), EllSwiftPubKey::size()}};
            conn.rbuf.erase(conn.rbuf.begin(), conn.rbuf.begin() + static_cast<std::ptrdiff_t>(EllSwiftPubKey::size()));
            conn.channel.Initialize(theirs, conn.initiator, cfg.magic);
            conn.state = ConnState::HELLO_WAIT;
            // The dialer speaks first; the acceptor signs only after it verified the dialer's HELLO.
            if (conn.initiator) SendHello(conn);
            continue;
        }
        // A packet: the length prefix, then the body.
        if (!conn.pending_len.has_value()) {
            if (conn.rbuf.size() < netio::SecureChannel::LENGTH_LEN) return;
            const uint32_t len{conn.channel.DecryptLength(std::span<const unsigned char>{conn.rbuf.data(), netio::SecureChannel::LENGTH_LEN})};
            conn.rbuf.erase(conn.rbuf.begin(), conn.rbuf.begin() + static_cast<std::ptrdiff_t>(netio::SecureChannel::LENGTH_LEN));
            if (len > cfg.mesh.max_frame_bytes || len > netio::SecureChannel::MAX_CONTENTS) {
                Close(conn, "packet above the frame cap");
                return;
            }
            conn.pending_len = len;
        }
        const size_t body_len{*conn.pending_len + netio::SecureChannel::HEADER_LEN + netio::SecureChannel::TAG_LEN};
        if (conn.rbuf.size() < body_len) return;
        std::vector<unsigned char> contents;
        const bool ok{conn.channel.Decrypt(std::span<const unsigned char>{conn.rbuf.data(), body_len}, contents)};
        conn.rbuf.erase(conn.rbuf.begin(), conn.rbuf.begin() + static_cast<std::ptrdiff_t>(body_len));
        conn.pending_len.reset();
        if (!ok) {
            Close(conn, "packet does not decrypt");
            return;
        }
        if (conn.state == ConnState::HELLO_WAIT) {
            OnHello(conn, contents);
            continue;
        }
        // OPEN: one frame of the connection's lane from its authenticated peer.
        conn.last_frame_at = Clock::now();
        bool full{false};
        {
            LOCK(mutex);
            inboxes[LaneIndex(conn.lane)].push_back(InboundFrame{*conn.peer, conn.lane, std::move(contents)});
            ++inbox_per_peer[*conn.peer];
            full = inbox_per_peer[*conn.peer] >= cfg.limits.inbox_frames_per_peer;
            for (TcpPeerStatus& status : peer_status) {
                if (status.seat == *conn.peer) ++status.frames_in;
            }
        }
        if (full) return; // the rest of rbuf waits until Poll drains the peer's share (Step comes back to it)
    }
}

void TcpSeatMesh::Impl::OnHello(Conn& conn, std::span<const unsigned char> bytes)
{
    const Result<netio::MeshHello> decoded{netio::MeshHello::Decode(bytes)};
    if (!decoded.has_value()) {
        Refused(conn.peer);
        Close(conn, "HELLO does not decode");
        return;
    }
    const netio::MeshHello& hello{*decoded};
    std::optional<uint32_t> who{conn.peer};
    if (!who.has_value() && hello.from_seat < cfg.mesh.seat_count) who = hello.from_seat;
    const auto refuse{[&](const char* why) {
        Refused(who);
        Close(conn, why);
    }};
    if (hello.to_seat != cfg.mesh.self_seat) return refuse("HELLO for another seat");
    if (hello.from_seat >= cfg.mesh.seat_count || hello.from_seat == cfg.mesh.self_seat) return refuse("HELLO from a seat outside the set, or our own");
    if (hello.epoch != cfg.epoch) return refuse("HELLO for another epoch");
    if (hello.seat_set_root != seats.Root()) return refuse("HELLO names another seat set");
    if (hello.transport_key == our_transport_pubkey) return refuse("HELLO carries our own transport key: another process holds this seat's keys");
    const SeatEndpointRecord& record{records.at(hello.from_seat)};
    if (record.transport_key != hello.transport_key) return refuse("HELLO transport key is not that seat's record's");
    if (conn.initiator && (hello.from_seat != *conn.peer || hello.lane != conn.lane)) return refuse("HELLO is not from the seat dialed, or for another lane");
    if (const Result<void> ok{netio::VerifyHello(hello, conn.channel.SessionId(), cfg.magic)}; !ok.has_value()) return refuse("HELLO signature does not verify");
    // One connection per (seat, lane): an open one that carried a frame recently keeps the pair.
    if (Conn* open{OpenConn(hello.from_seat, hello.lane)}; open != nullptr && open != &conn) {
        const Clock::time_point now{Clock::now()};
        if (open->last_frame_at + Ms{static_cast<int64_t>(cfg.limits.duplicate_grace_ms)} > now) return refuse("a connection for that seat and lane is live");
        Close(*open, "replaced by a newer connection");
    }
    conn.peer = hello.from_seat;
    conn.lane = hello.lane;
    conn.state = ConnState::OPEN;
    conn.last_frame_at = Clock::now();
    {
        LOCK(mutex);
        for (TcpPeerStatus& status : peer_status) {
            if (status.seat == *conn.peer) status.open[LaneIndex(conn.lane)] = true;
        }
    }
    if (!conn.initiator) SendHello(conn);
}

Conn* TcpSeatMesh::Impl::OpenConn(uint32_t seat, Lane lane)
{
    for (auto& conn : conns) {
        if (!conn->closed && conn->state == ConnState::OPEN && conn->peer == seat && conn->lane == lane) return conn.get();
    }
    return nullptr;
}

void TcpSeatMesh::Impl::PumpOutboxes()
{
    std::vector<std::pair<Conn*, std::vector<unsigned char>>> out;
    {
        LOCK(mutex);
        for (auto& [route, queue] : outboxes) {
            if (queue.empty()) continue;
            Conn* conn{OpenConn(route.first, route.second)};
            if (conn == nullptr) continue;
            // A socket with write_buffer_bytes queued takes nothing more: the frames wait in the outbox, whose cap tells the
            // sender BUSY, instead of growing this process's memory for a peer that does not read (review 2026-11-16, 4b).
            size_t pending{conn->wbuf.size() - conn->wpos};
            size_t moved{0};
            while (!queue.empty() && pending < cfg.limits.write_buffer_bytes) {
                pending += queue.front().size() + netio::SecureChannel::EXPANSION;
                out.emplace_back(conn, std::move(queue.front()));
                queue.pop_front();
                ++moved;
            }
            for (TcpPeerStatus& status : peer_status) {
                if (status.seat == route.first) status.frames_out += moved; // this route's frames, not the step's total
            }
        }
    }
    // Outbound frames say nothing about the peer: last_frame_at moves on received frames only, so a dead connection
    // ages out of duplicate_grace_ms and the peer's fresh, authenticated one replaces it (review 2026-11-16, 4c).
    for (auto& [conn, frame] : out) QueuePacket(*conn, frame);
}

void TcpSeatMesh::Impl::Close(Conn& conn, std::string why)
{
    if (conn.closed) return;
    conn.closed = true;
    conn.why = std::move(why);
    if (conn.state == ConnState::OPEN && conn.peer.has_value()) {
        LOCK(mutex);
        for (TcpPeerStatus& status : peer_status) {
            if (status.seat == *conn.peer && OpenConn(*conn.peer, conn.lane) == &conn) status.open[LaneIndex(conn.lane)] = false;
        }
    }
    if (conn.initiator && conn.peer.has_value()) {
        if (Dial* dial{DialFor(*conn.peer, conn.lane)}) dial->active = false;
    }
}

void TcpSeatMesh::Impl::Reap()
{
    size_t removed{0};
    for (auto it{conns.begin()}; it != conns.end();) {
        if ((*it)->closed) {
            it = conns.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed > 0) {
        LOCK(mutex);
        connection_count -= std::min(connection_count, removed);
    }
}

void TcpSeatMesh::Impl::Refused(std::optional<uint32_t> peer)
{
    if (!peer.has_value()) return;
    LOCK(mutex);
    for (TcpPeerStatus& status : peer_status) {
        if (status.seat == *peer) ++status.refused;
    }
}

bool TcpSeatMesh::Impl::InboxFull(uint32_t peer)
{
    LOCK(mutex);
    return inbox_per_peer[peer] >= cfg.limits.inbox_frames_per_peer;
}

// ---- The SeatMesh ports ----------------------------------------------------------------

Result<void> TcpSeatMesh::Transmit(uint32_t seat, Lane lane, std::span<const unsigned char> frame)
{
    if (!m_impl->running) return Error{ErrorCode::FENCED, TCP_MESH_CLOSED};
    {
        LOCK(m_impl->mutex);
        std::deque<std::vector<unsigned char>>& queue{m_impl->outboxes[{seat, lane}]};
        for (const std::vector<unsigned char>& queued : queue) {
            if (queued.size() == frame.size() && std::equal(queued.begin(), queued.end(), frame.begin())) return Ok(); // already queued
        }
        if (queue.size() >= m_impl->cfg.limits.outbox_frames) return Error{ErrorCode::BUSY, LANE_FULL};
        queue.emplace_back(frame.begin(), frame.end());
    }
    m_impl->Wake();
    return Ok();
}

Result<std::optional<InboundFrame>> TcpSeatMesh::Poll()
{
    LOCK(m_impl->mutex);
    for (const Lane lane : LANES_BY_PRIORITY) {
        std::deque<InboundFrame>& inbox{m_impl->inboxes[LaneIndex(lane)]};
        if (inbox.empty()) continue;
        InboundFrame frame{std::move(inbox.front())};
        inbox.pop_front();
        if (m_impl->inbox_per_peer[frame.from_seat] > 0) --m_impl->inbox_per_peer[frame.from_seat];
        return Ok(std::optional<InboundFrame>{std::move(frame)});
    }
    return Ok(std::optional<InboundFrame>{});
}

uint16_t TcpSeatMesh::ListenPort() const { return m_impl->listen_port; }

TcpMeshStatus TcpSeatMesh::Status() const
{
    TcpMeshStatus status;
    status.listen_port = m_impl->listen_port;
    LOCK(m_impl->mutex);
    status.connections = m_impl->connection_count;
    status.steps = m_impl->steps;
    status.peers = m_impl->peer_status;
    return status;
}

void TcpSeatMesh::DropConnections()
{
    {
        LOCK(m_impl->mutex);
        m_impl->drop_all = true;
    }
    m_impl->Wake();
}

} // namespace flowmesh2::net
