// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <node/flowmesh_net.h>
#include <node/flowmesh_runtime.h>

#include <crypto/common.h>
#include <hash.h>
#include <key.h>
#include <logging.h>
#include <netbase.h>
#include <random.h>
#include <support/cleanse.h>
#include <univalue.h>
#include <util/fs_helpers.h>
#include <util/sock.h>
#include <util/strencodings.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#ifndef WIN32
#include <sys/stat.h>
#endif

namespace node {
namespace {
using Clock = std::chrono::steady_clock;
using Bytes = std::vector<unsigned char>;
using Kind = flowmesh::WireMessageKind;
constexpr size_t MIB{1024 * 1024};
constexpr size_t HELLO_SIZE{4 + 2 + 32 + 1 + 1 + 33 + 32};
constexpr size_t SIGNATURE_SIZE{65};
constexpr size_t FRAME_PREFIX{4 + 1 + 8};
constexpr size_t FRAME_OVERHEAD{FRAME_PREFIX + SIGNATURE_SIZE};
constexpr size_t CHANNELS{3};
// Reservations include queued and currently-writing frames. Neither actions
// nor history can consume the critical class's memory or item allowance.
constexpr std::array<size_t, CHANNELS> GLOBAL_QUEUE_BYTES{32 * MIB, 8 * MIB, 24 * MIB};
constexpr std::array<size_t, CHANNELS> PEER_QUEUE_BYTES{8 * MIB, 2 * MIB, 8 * MIB};
constexpr std::array<size_t, CHANNELS> GLOBAL_QUEUE_ITEMS{8192, 4096, 1024};
constexpr std::array<size_t, CHANNELS> PEER_QUEUE_ITEMS{1024, 512, 128};
constexpr std::array<size_t, CHANNELS> PASS_BYTES{512 * 1024, 64 * 1024, 128 * 1024};
constexpr std::array<size_t, CHANNELS> PASS_FRAMES{64, 8, 2};
constexpr std::array<size_t, CHANNELS> GLOBAL_RX_BYTES{32 * MIB, 8 * MIB, 24 * MIB};
constexpr size_t MAX_HANDSHAKES{8};
constexpr uint8_t PING{255};
constexpr auto IO_WAIT{std::chrono::milliseconds{10}};
constexpr auto HANDSHAKE_TIMEOUT{std::chrono::seconds{5}};
constexpr auto RECONNECT_DELAY{std::chrono::seconds{2}};
constexpr auto INGRESS_RETRY_INITIAL{std::chrono::milliseconds{100}};
constexpr auto INGRESS_RETRY_MAX{std::chrono::milliseconds{1000}};
constexpr auto INGRESS_RETRY_TIMEOUT{std::chrono::seconds{30}};
// Debug traces must not become an unbounded traffic-to-disk amplifier. This
// budget covers this process lifetime, including network-service restarts.
constexpr uint64_t NETWORK_TRACE_BYTES{32 * MIB};
std::atomic<uint64_t> network_trace_bytes{0};
std::atomic<uint64_t> network_trace_dropped{0};

uint8_t Role(const std::string& role)
{
    if (role == "observer") return 0;
    if (role == "sentry") return 1;
    if (role == "validator") return 2;
    throw std::runtime_error{"FlowMesh network role must be observer, sentry, or validator"};
}
std::string RoleName(uint8_t role) { return role == 0 ? "observer" : role == 1 ? "sentry" : "validator"; }
uint8_t Channel(Kind kind)
{
    if (kind == Kind::ACTION) return 1;
    if (kind == Kind::GET || kind == Kind::ENTRIES) return 2;
    return 0;
}
size_t Lane(Kind kind)
{
    if (kind == Kind::ATTESTATION || kind == Kind::CERTIFICATE) return 0;
    if (kind == Kind::PROPOSAL) return 1;
    if (kind == Kind::HELLO) return 2;
    return Channel(kind) == 2 ? 4 : 3;
}
bool RetryError() { const int e{WSAGetLastError()}; return e == WSAEWOULDBLOCK || e == WSAEAGAIN || e == WSAEINTR || e == WSAEINPROGRESS; }
const char* AdmissionReason(flowmesh::QueueResult result)
{
    switch (result) {
    case flowmesh::QueueResult::ACCEPTED: return "accepted";
    case flowmesh::QueueResult::MALFORMED: return "runtime rejected malformed message";
    case flowmesh::QueueResult::RATE_LIMITED: return "runtime admission rate limited";
    case flowmesh::QueueResult::PEER_LIMIT: return "runtime peer admission limit";
    case flowmesh::QueueResult::MARKET_LIMIT: return "runtime market unavailable or admission limited";
    case flowmesh::QueueResult::GLOBAL_LIMIT: return "runtime admission unavailable (reconciliation, shutdown, or queue limit)";
    case flowmesh::QueueResult::RECONCILING: return "b3_reconciling";
    case flowmesh::QueueResult::STOPPED: return "service_stopped";
    }
    return "runtime admission unavailable";
}

// Opt-in local measurements only. The exact inner wire hash correlates these
// transport events with runtime events; it is not a receipt or validity proof.
void NetworkTrace(const char* stage, Kind kind, const flowmesh::WireHeader& header,
                  const uint256& wire_hash, flowmesh::WirePeerId peer,
                  uint64_t delivery_id, const std::string& reason = {})
{
    if (!LogAcceptCategory(BCLog::BENCH, BCLog::Level::Debug)) return;
    UniValue event{UniValue::VOBJ};
    event.pushKV("monotonic_us", std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
    event.pushKV("stage", stage);
    event.pushKV("kind", std::string{flowmesh::WireCommand(kind)});
    event.pushKV("market_id", header.market_id.GetHex());
    event.pushKV("epoch", header.epoch);
    event.pushKV("sequence", header.sequence);
    event.pushKV("wire_hash", wire_hash.GetHex());
    event.pushKV("peer", peer);
    event.pushKV("delivery_id", delivery_id);
    event.pushKV("reason", reason);
    const auto line{event.write()};
    const uint64_t charge{line.size() + 128};
    uint64_t used{network_trace_bytes.load(std::memory_order_relaxed)};
    do {
        if (charge > NETWORK_TRACE_BYTES - used) {
            network_trace_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    } while (!network_trace_bytes.compare_exchange_weak(used, used + charge, std::memory_order_relaxed));
    LogDebug(BCLog::BENCH, "FlowMeshNetTrace %s\n", line);
}

struct Bucket {
    double items, bytes, item_cap, byte_cap, item_rate, byte_rate;
    Clock::time_point at{Clock::now()};
    Bucket(double count, double size, double count_rate, double size_rate)
        : items{count}, bytes{size}, item_cap{count}, byte_cap{size}, item_rate{count_rate}, byte_rate{size_rate} {}
    bool Take(size_t size, Clock::time_point now)
    {
        const auto seconds{std::max(0.0, std::chrono::duration<double>(now - at).count())}; at = now;
        items = std::min(item_cap, items + seconds * item_rate);
        bytes = std::min(byte_cap, bytes + seconds * byte_rate);
        if (items < 1 || bytes < size) return false;
        items -= 1; bytes -= size; return true;
    }
};
std::array<Bucket, 5> Buckets()
{
    // Separate critical credits cannot be consumed by actions or bulk.
    return {Bucket{512, 8 * MIB, 256, 4 * MIB}, Bucket{64, 8 * MIB, 32, 4 * MIB},
            Bucket{32, MIB, 16, MIB / 2}, Bucket{256, 4 * MIB, 128, 2 * MIB},
            Bucket{32, 8 * MIB, 8, 4 * MIB}};
}
struct Packet {
    Kind kind;
    std::shared_ptr<const Bytes> inner;
    uint64_t delivery_id{0};
    struct Cancellation { std::atomic_bool cancelled{false}; size_t references{0}; };
    std::shared_ptr<Cancellation> cancellation;
    flowmesh::WireHeader header;
    uint256 wire_hash;
    size_t Size() const { return inner->size() + FRAME_OVERHEAD; }
};
struct Queues {
    std::array<std::deque<Packet>, 5> lanes;
    size_t bytes{0}, noncritical{0}, count{0};
    void Push(Packet p) { const size_t l{Lane(p.kind)}; bytes += p.Size(); if (l) noncritical += p.Size(); ++count; lanes[l].push_back(std::move(p)); }
    Packet Pop(size_t lane) { Packet p{std::move(lanes[lane].front())}; lanes[lane].pop_front(); bytes -= p.Size(); if (lane) noncritical -= p.Size(); --count; return p; }
};
struct EgressPeer {
    std::array<Queues, CHANNELS> queues;
    std::array<FlowMeshNetTraffic, CHANNELS> traffic;
};

bool Verify(const CPubKey& key, const uint256& digest, std::span<const unsigned char> signature)
{
    if (signature.size() != SIGNATURE_SIZE) return false;
    CPubKey recovered;
    return recovered.RecoverCompact(digest, Bytes{signature.begin(), signature.end()}) && recovered == key;
}
uint256 AuthDigest(const uint256& session, const CPubKey& sender)
{
    return (TaggedHash("B3/FlowMeshNet/auth/v2") << session << sender).GetHash();
}
uint256 FrameDigest(const uint256& session, const CPubKey& sender, std::span<const unsigned char> unsigned_body)
{
    auto writer{TaggedHash("B3/FlowMeshNet/frame/v2")};
    writer << session << sender;
    writer.write(MakeByteSpan(unsigned_body));
    return writer.GetHash();
}
bool Configure(Sock& sock)
{
    const int one{1};
    return sock.SetNonBlocking() && sock.IsSelectable() &&
        sock.SetSockOpt(IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0;
}

struct Target {
    CService address;
    std::optional<CPubKey> pin;
    std::string learned_key;
    std::array<Clock::time_point, CHANNELS> next{};
};
Target ParseTarget(const std::string& value, uint16_t port)
{
    Target out;
    const size_t split{value.find('@')};
    std::string endpoint{value};
    if (split != std::string::npos) {
        const auto text{value.substr(0, split)};
        if (text.size() != 66 || !IsHex(text)) throw std::runtime_error{"FlowMesh peer pin must be a compressed public key"};
        const auto raw{ParseHex(text)}; CPubKey key{raw};
        if (!key.IsFullyValid() || !key.IsCompressed()) throw std::runtime_error{"Invalid FlowMesh peer public key"};
        out.pin = key; endpoint = value.substr(split + 1);
    }
    out.address = LookupNumeric(endpoint, port);
    if (!out.address.IsValid() || (!out.address.IsIPv4() && !out.address.IsIPv6())) throw std::runtime_error{"FlowMesh peers require numeric IPv4 or bracketed IPv6 addresses"};
    return out;
}
} // namespace

FlowMeshNetReceiveTimeout ClassifyFlowMeshNetReceiveTimeout(
    std::chrono::milliseconds idle_age, std::chrono::milliseconds frame_age,
    bool frame_active, bool admission_pending)
{
    if (admission_pending) return FlowMeshNetReceiveTimeout::NONE;
    if (frame_active && frame_age >= std::chrono::minutes{5}) return FlowMeshNetReceiveTimeout::FRAME;
    if (idle_age >= std::chrono::seconds{30}) return FlowMeshNetReceiveTimeout::IDLE;
    return FlowMeshNetReceiveTimeout::NONE;
}

struct FlowMeshNetService::Impl {
    FlowMeshNetConfig config;
    flowmesh::WireMessageSink& sink;
    CKey key;
    CPubKey pubkey;
    bool directory_locked{false};
    std::atomic_bool stopping{false};
    std::thread worker;
    std::shared_ptr<Sock> listener;
    std::vector<Target> targets;
    mutable std::mutex mutex;
    FlowMeshNetSnapshot snapshot;
    // Only queue admission/accounting is shared with callers. Socket state is
    // worker-owned, and no lock is held across I/O, crypto, sink or feedback.
    std::map<flowmesh::WirePeerId, std::shared_ptr<EgressPeer>> admission_peers;
    std::map<uint64_t, std::shared_ptr<Packet::Cancellation>> cancellations;
    std::array<uint64_t, CHANNELS> next_socket{};
    uint64_t next_connection{1};
    flowmesh::WirePeerId next_peer{-2};
    std::array<size_t, CHANNELS> rx_bytes{};
    Bucket accept_budget{16, 16, 4, 4};
    // Bulk cannot exhaust the credits reserved for critical verification.
    std::array<Bucket, 5> global_rx{
        Bucket{1024, 16 * MIB, 512, 8 * MIB}, Bucket{128, 16 * MIB, 64, 8 * MIB},
        Bucket{128, 2 * MIB, 64, MIB}, Bucket{1024, 8 * MIB, 512, 4 * MIB},
        Bucket{64, 16 * MIB, 16, 8 * MIB}};
    // Actual socket bytes, not just future frame preparation, consume these
    // separate class budgets (burst/rate: 32/16, 4/2, 8/4 MiB).
    std::array<Bucket, CHANNELS> global_tx{
        Bucket{1'000'000, 32 * MIB, 1'000'000, 16 * MIB},
        Bucket{1'000'000, 4 * MIB, 1'000'000, 2 * MIB},
        Bucket{1'000'000, 8 * MIB, 1'000'000, 4 * MIB}};

    struct Connection {
        uint64_t id;
        std::shared_ptr<Sock> sock;
        bool outbound{false}, connecting{false}, dead{false}, authenticated{false};
        uint8_t channel{0}, remote_role{0};
        std::optional<size_t> target;
        std::string address, peer_key;
        CPubKey remote_key;
        Bytes local_hello, remote_hello;
        uint256 session;
        enum class Stage { HELLO, AUTH, FRAME } stage{Stage::HELLO};
        Bytes rx;
        size_t rx_pos{0}, rx_charge{0};
        // At most one decoded frame per channel. Its bytes remain charged to
        // GLOBAL_RX_BYTES, and TCP reads stop until admission succeeds.
        std::optional<flowmesh::WireMessage> pending_ingress;
        size_t pending_ingress_charge{0};
        std::string ingress_failure;
        Clock::time_point ingress_pending_since{}, next_ingress_retry{};
        std::chrono::milliseconds ingress_retry_delay{INGRESS_RETRY_INITIAL};
        std::array<unsigned char, FRAME_PREFIX> prefix{};
        size_t prefix_pos{0};
        Bytes tx;
        size_t tx_pos{0}, tx_charge{0};
        bool tx_application{false};
        uint64_t send_counter{0}, receive_counter{0};
        Clock::time_point created{Clock::now()}, last_receive{created}, last_send{created};
        Clock::time_point frame_started{};
        std::shared_ptr<EgressPeer> egress;
        flowmesh::WirePeerId peer_id{-1};
        std::optional<Packet> in_flight;
        std::array<Bucket, 5> receive_budget{Buckets()}, send_budget{Buckets()};
        Connection(uint64_t identity, std::shared_ptr<Sock> socket) : id{identity}, sock{std::move(socket)}, rx(HELLO_SIZE) {}
    };
    struct Peer {
        FlowMeshNetPeer info;
        std::array<uint64_t, CHANNELS> channels{};
        bool notified{false}, configured{false};
        std::shared_ptr<EgressPeer> egress{std::make_shared<EgressPeer>()};
    };
    std::map<uint64_t, std::unique_ptr<Connection>> connections;
    std::map<std::string, Peer> peers;

    Impl(FlowMeshNetConfig c, flowmesh::WireMessageSink& s) : config{std::move(c)}, sink{s} {}

    void LoadKey()
    {
        if (config.datadir.empty()) throw std::runtime_error{"FlowMesh network datadir is required"};
        fs::create_directories(config.datadir);
        if (util::LockDirectory(config.datadir, ".flowmesh-network.lock") != util::LockResult::Success) throw std::runtime_error{"FlowMesh network identity directory is locked"};
        directory_locked = true;
        const fs::path path{config.datadir / "operator.key"};
        if (fs::exists(path)) {
            if (!fs::is_regular_file(fs::symlink_status(path))) throw std::runtime_error{"FlowMesh operator key must be a regular file"};
#ifndef WIN32
            struct stat st{};
            if (stat(path.c_str(), &st) != 0 || (st.st_mode & 077) || st.st_uid != geteuid()) throw std::runtime_error{"FlowMesh operator key must be owner-only (0600)"};
#endif
            FILE* file{fsbridge::fopen(path, "rb")};
            if (!file) throw std::runtime_error{"Cannot read FlowMesh operator key"};
            std::array<unsigned char, 32> secret{};
            const bool valid_size{std::fread(secret.data(), 1, secret.size(), file) == secret.size() && std::fgetc(file) == EOF && !std::ferror(file)};
            std::fclose(file);
            if (valid_size) key.Set(secret.begin(), secret.end(), true);
            memory_cleanse(secret.data(), secret.size());
            if (!valid_size || !key.IsValid()) throw std::runtime_error{"Invalid FlowMesh operator key; refusing replacement"};
        } else {
            key.MakeNewKey(true);
            // Exclusive creation prevents overwriting another network identity.
            FILE* file{fsbridge::fopen(path, "wbx")};
            if (!file) throw std::runtime_error{"Cannot create FlowMesh operator key"};
#ifndef WIN32
            if (fchmod(fileno(file), 0600) != 0) { std::fclose(file); throw std::runtime_error{"Cannot protect FlowMesh operator key"}; }
#endif
            const bool written{std::fwrite(key.begin(), 1, key.size(), file) == key.size() && FileCommit(file)};
            const bool closed{std::fclose(file) == 0};
            if (!written || !closed) throw std::runtime_error{"Cannot persist FlowMesh operator key"};
            DirectoryCommit(config.datadir);
        }
        pubkey = key.GetPubKey();
    }

    Bytes Hello(uint8_t channel)
    {
        Bytes bytes(HELLO_SIZE);
        std::copy_n("FMN2", 4, bytes.begin()); WriteLE16(bytes.data() + 4, 2);
        std::copy(config.domain.begin(), config.domain.end(), bytes.begin() + 6);
        bytes[38] = channel; bytes[39] = Role(config.role);
        std::copy(pubkey.begin(), pubkey.end(), bytes.begin() + 40);
        GetStrongRandBytes(std::span{bytes}.subspan(73, 32));
        return bytes;
    }
    bool ReceiveHello(Connection& c)
    {
        const auto& h{c.rx};
        if (!std::equal(h.begin(), h.begin() + 4, "FMN2") || ReadLE16(h.data() + 4) != 2 ||
            !std::equal(config.domain.begin(), config.domain.end(), h.begin() + 6) || h[38] >= CHANNELS || h[39] > 2 ||
            (c.outbound && h[38] != c.channel)) return false;
        c.channel = h[38]; c.remote_role = h[39];
        c.remote_key.Set(h.begin() + 40, h.begin() + 73);
        if (!c.remote_key.IsFullyValid() || !c.remote_key.IsCompressed() || c.remote_key == pubkey) return false;
        if (c.target && targets[*c.target].pin && c.remote_key != *targets[*c.target].pin) return false;
        c.peer_key = HexStr(c.remote_key);
        c.remote_hello = h;
        if (!c.outbound) { c.local_hello = Hello(c.channel); c.tx = c.local_hello; c.tx_pos = 0; }
        auto hash{TaggedHash("B3/FlowMeshNet/session/v2")};
        hash.write(MakeByteSpan(c.outbound ? c.local_hello : c.remote_hello));
        hash.write(MakeByteSpan(c.outbound ? c.remote_hello : c.local_hello));
        c.session = hash.GetHash();
        Bytes signature;
        if (!key.SignCompact(AuthDigest(c.session, pubkey), signature)) return false;
        c.tx.insert(c.tx.end(), signature.begin(), signature.end());
        c.stage = Connection::Stage::AUTH; c.rx.assign(SIGNATURE_SIZE, 0); c.rx_pos = 0;
        return true;
    }
    bool Authenticate(Connection& c)
    {
        if (!Verify(c.remote_key, AuthDigest(c.session, c.remote_key), c.rx)) return false;
        // Even a redundant authenticated dial learns which already-active
        // peer satisfies this unpinned endpoint; do not redial it forever.
        if (c.target) targets[*c.target].learned_key = c.peer_key;
        const bool configured{c.target.has_value() || std::any_of(targets.begin(), targets.end(), [&](const auto& target) {
            return (target.pin && *target.pin == c.remote_key) || target.learned_key == c.peer_key;
        })};
        auto it{peers.find(c.peer_key)};
        if (it == peers.end()) {
            if (peers.size() >= config.max_peers || next_peer <= std::numeric_limits<flowmesh::WirePeerId>::min() + 1) return false;
            // Public self-identities cannot consume configured-peer slots.
            const size_t unknown{static_cast<size_t>(std::count_if(peers.begin(), peers.end(), [](const auto& peer) { return !peer.second.configured; }))};
            if (!configured && unknown >= config.max_peers - targets.size()) return false;
            Peer p; p.info.id = next_peer--; p.info.address = c.address; p.info.operator_pubkey = c.peer_key;
            p.info.role = RoleName(c.remote_role); p.info.inbound = !c.outbound;
            it = peers.emplace(c.peer_key, std::move(p)).first;
        } else if (it->second.info.role != RoleName(c.remote_role)) return false;
        auto& p{it->second};
        p.configured |= configured;
        if (p.channels[c.channel]) {
            auto& old{*connections.at(p.channels[c.channel])};
            // Simultaneous dial deterministically prefers the lower-key dialer.
            const bool prefer_outbound{HexStr(pubkey) < c.peer_key};
            if (old.outbound == prefer_outbound || c.outbound != prefer_outbound) return false;
            old.dead = true;
        }
        p.channels[c.channel] = c.id;
        c.egress = p.egress; c.peer_id = p.info.id;
        c.authenticated = true; c.stage = Connection::Stage::FRAME; c.rx.clear(); c.rx_pos = 0;
        if (!p.notified && std::all_of(p.channels.begin(), p.channels.end(), [](uint64_t id) { return id != 0; })) {
            p.notified = true;
            { std::lock_guard lock{mutex}; admission_peers.emplace(p.info.id, p.egress); }
            sink.FlowMeshPeerConnected(p.info.id);
        }
        return true;
    }

    void ReleaseReceive(Connection& c)
    {
        rx_bytes[c.channel] -= c.rx_charge; c.rx_charge = 0; Bytes{}.swap(c.rx); c.rx_pos = 0; c.prefix_pos = 0;
    }
    void EgressRejected(const std::string& reason)
    {
        std::lock_guard lock{mutex};
        ++snapshot.egress_rejected_messages; snapshot.last_egress_error = reason;
    }
    void Finish(const Packet& packet, flowmesh::WirePeerId peer,
                const std::shared_ptr<EgressPeer>& egress, FlowMeshDeliveryOutcome outcome)
    {
        const auto channel{Channel(packet.kind)};
        {
            std::lock_guard lock{mutex};
            for (auto* traffic : {&snapshot.traffic[channel], &egress->traffic[channel]}) {
                traffic->queued_bytes -= packet.Size(); --traffic->queued_messages;
                if (outcome == FlowMeshDeliveryOutcome::SOCKET_WRITTEN) ++traffic->socket_written;
                else ++traffic->failed;
            }
            if (--packet.cancellation->references == 0 && packet.delivery_id) cancellations.erase(packet.delivery_id);
        }
        if (LogAcceptCategory(BCLog::BENCH, BCLog::Level::Debug)) {
            // Logging may have been enabled after admission. Do not report an
            // all-zero correlation hash for an already queued signed object.
            const auto wire_hash{packet.wire_hash.IsNull() ? Hash(*packet.inner) : packet.wire_hash};
            NetworkTrace(outcome == FlowMeshDeliveryOutcome::SOCKET_WRITTEN ? "socket_written" : "write_incomplete",
                         packet.kind, packet.header, wire_hash, peer, packet.delivery_id,
                         FlowMeshDeliveryOutcomeName(outcome));
        }
        if (packet.delivery_id && config.delivery_callback && !config.delivery_callback({packet.delivery_id, peer, outcome, FlowMeshDeliveryOutcomeName(outcome)})) {
            std::lock_guard lock{mutex}; ++snapshot.notification_refused;
        }
    }
    void IoBytes(Connection& c, size_t bytes, bool sent)
    {
        if (!c.egress || !bytes) return;
        std::lock_guard lock{mutex};
        for (auto* traffic : {&snapshot.traffic[c.channel], &c.egress->traffic[c.channel]}) {
            (sent ? traffic->sent_bytes : traffic->received_bytes) += bytes;
        }
    }
    bool RetryIngress(Connection& c, Clock::time_point now)
    {
        if (!c.pending_ingress) return true;
        auto peer{peers.find(c.peer_key)};
        if (peer == peers.end() || !peer->second.notified) return false;
        if (now - c.ingress_pending_since >= config.ingress_retry_timeout) {
            c.ingress_failure = "runtime admission retry timed out; disconnecting for explicit reconnect/catch-up";
            return false;
        }
        if (now < c.next_ingress_retry) return true;
        // The sink takes by value. Keep the authenticated original until it
        // ACCEPTS a copy; a rejection must not consume our only retry body.
        const auto result{sink.EnqueueWireMessage(peer->second.info.id, *c.pending_ingress)};
        if (LogAcceptCategory(BCLog::BENCH, BCLog::Level::Debug)) {
            flowmesh::WireCheck check;
            const auto encoded{flowmesh::EncodeWireMessage(*c.pending_ingress, check)};
            if (encoded) NetworkTrace("runtime_ingress_result", c.pending_ingress->kind,
                                      c.pending_ingress->header, Hash(*encoded), peer->second.info.id, 0,
                                      AdmissionReason(result));
        }
        if (result == flowmesh::QueueResult::ACCEPTED) {
            rx_bytes[c.channel] -= c.pending_ingress_charge; c.pending_ingress_charge = 0;
            c.pending_ingress.reset(); return true;
        }
        const std::string reason{AdmissionReason(result)};
        peer->second.info.last_ingress_retry = reason;
        if (result == flowmesh::QueueResult::MALFORMED) {
            c.ingress_failure = reason;
            return false;
        }
        ++peer->second.info.ingress_retries;
        {
            std::lock_guard lock{mutex}; ++snapshot.ingress_retries;
            ++snapshot.traffic[c.channel].ingress_retries; ++c.egress->traffic[c.channel].ingress_retries;
        }
        c.next_ingress_retry = now + c.ingress_retry_delay;
        c.ingress_retry_delay = std::min(INGRESS_RETRY_MAX, c.ingress_retry_delay * 2);
        return true;
    }
    bool ReadFrame(Connection& c, size_t& allowance, Clock::time_point now)
    {
        if (c.prefix_pos < FRAME_PREFIX) {
            const auto n{c.sock->Recv(c.prefix.data() + c.prefix_pos, std::min(FRAME_PREFIX - c.prefix_pos, allowance), 0)};
            if (n < 0) return RetryError();
            if (n == 0) return false;
            if (c.prefix_pos == 0) c.frame_started = now;
            c.last_receive = now;
            c.prefix_pos += n; allowance -= n;
            IoBytes(c, n, false);
            if (c.prefix_pos < FRAME_PREFIX) return true;
            const uint32_t body{ReadLE32(c.prefix.data())};
            const uint8_t kind{c.prefix[4]};
            if (ReadLE64(c.prefix.data() + 5) != c.receive_counter + 1 || c.receive_counter == UINT64_MAX) return false;
            size_t maximum{9 + SIGNATURE_SIZE};
            size_t lane{2};
            if (kind != PING) {
                if (kind > static_cast<uint8_t>(Kind::ENTRIES) || Channel(static_cast<Kind>(kind)) != c.channel) return false;
                maximum += flowmesh::FLOWMESH_WIRE_HEADER_SIZE + flowmesh::PayloadLimitForWireKind(static_cast<Kind>(kind));
                lane = Lane(static_cast<Kind>(kind));
                if (body < 9 + SIGNATURE_SIZE + flowmesh::FLOWMESH_WIRE_HEADER_SIZE) return false;
            }
            if (body < 9 + SIGNATURE_SIZE || body > maximum || body > GLOBAL_RX_BYTES[c.channel] - rx_bytes[c.channel] ||
                !c.receive_budget[lane].Take(body + 4, now) || !global_rx[lane].Take(body + 4, now)) return false;
            c.rx.assign(body, 0); c.rx_charge = body; rx_bytes[c.channel] += body;
            std::copy(c.prefix.begin() + 4, c.prefix.end(), c.rx.begin()); c.rx_pos = 9;
        }
        if (!allowance) return true;
        const auto n{c.sock->Recv(c.rx.data() + c.rx_pos, std::min(c.rx.size() - c.rx_pos, allowance), 0)};
        if (n < 0) return RetryError();
        if (!n) return false;
        c.last_receive = now;
        c.rx_pos += n; allowance -= n;
        IoBytes(c, n, false);
        if (c.rx_pos != c.rx.size()) return true;
        const std::span<const unsigned char> body{c.rx};
        if (!Verify(c.remote_key, FrameDigest(c.session, c.remote_key, body.first(body.size() - SIGNATURE_SIZE)), body.last(SIGNATURE_SIZE))) return false;
        ++c.receive_counter; c.last_receive = now;
        if (c.rx[0] != PING) {
            flowmesh::WireCheck check;
            auto message{flowmesh::DecodeWireMessage(static_cast<Kind>(c.rx[0]), body.subspan(9, body.size() - 9 - SIGNATURE_SIZE), check)};
            if (!message) return false;
            auto p{peers.find(c.peer_key)};
            if (p == peers.end() || !p->second.notified) return false;
            const size_t decoded_charge{message->MemoryUsage()};
            // MemoryUsage is the inner header + payload, while rx_charge also
            // includes the counter, kind and signature. Keep this explicit if
            // the inner accounting changes; never exceed the reserved bytes.
            if (decoded_charge > c.rx_charge) return false;
            if (LogAcceptCategory(BCLog::BENCH, BCLog::Level::Debug)) {
                NetworkTrace("transport_authenticated_receive", message->kind, message->header,
                             Hash(body.subspan(9, body.size() - 9 - SIGNATURE_SIZE)), p->second.info.id, 0);
            }
            ++p->second.info.received_messages;
            { std::lock_guard lock{mutex}; ++snapshot.traffic[c.channel].received; ++c.egress->traffic[c.channel].received; }
            c.pending_ingress = std::move(*message);
            c.ingress_pending_since = now; c.next_ingress_retry = now;
            c.ingress_retry_delay = INGRESS_RETRY_INITIAL;
            // Replace raw-frame accounting with the retained decoded charge.
            ReleaseReceive(c);
            c.pending_ingress_charge = decoded_charge;
            rx_bytes[c.channel] += c.pending_ingress_charge;
            return RetryIngress(c, now);
        }
        ReleaseReceive(c);
        return true;
    }

    bool Frame(Connection& c, uint8_t kind, std::span<const unsigned char> inner)
    {
        if (c.send_counter == UINT64_MAX) return false;
        c.tx.assign(FRAME_PREFIX + inner.size(), 0); c.tx_pos = 0;
        WriteLE32(c.tx.data(), 9 + inner.size() + SIGNATURE_SIZE);
        c.tx[4] = kind; WriteLE64(c.tx.data() + 5, ++c.send_counter);
        std::copy(inner.begin(), inner.end(), c.tx.begin() + FRAME_PREFIX);
        Bytes signature;
        if (!key.SignCompact(FrameDigest(c.session, pubkey, std::span{c.tx}.subspan(4)), signature)) return false;
        c.tx.insert(c.tx.end(), signature.begin(), signature.end()); c.tx_application = kind != PING;
        return true;
    }
    bool PrepareSend(Connection& c, Clock::time_point now, size_t& frames)
    {
        if (c.in_flight && c.in_flight->cancellation->cancelled) {
            // Never omit the tail of a frame that has already entered TCP.
            if (c.tx_pos) return false;
            if (!c.tx.empty()) --c.send_counter;
            c.tx.clear(); c.tx_pos = c.tx_charge = 0; c.tx_application = false;
            auto cancelled{std::move(*c.in_flight)}; c.in_flight.reset();
            Finish(cancelled, c.peer_id, c.egress, FlowMeshDeliveryOutcome::CANCELLED);
        }
        if (!c.tx.empty() || !c.authenticated || !frames) return true;
        auto p{peers.find(c.peer_key)};
        if (p == peers.end() || !p->second.notified) return true;
        // Cancelled items consume bounded worker effort, not a frame sequence.
        for (size_t work{0}; work < 8; ++work) {
            std::optional<Packet> selected;
            {
                std::lock_guard lock{mutex};
                auto& queue{c.egress->queues[c.channel]};
                for (size_t lane{0}; lane < 5; ++lane) {
                    if (queue.lanes[lane].empty()) continue;
                    const auto& packet{queue.lanes[lane].front()};
                    if (!packet.cancellation->cancelled && !c.send_budget[lane].Take(packet.Size(), now)) continue;
                    selected = queue.Pop(lane); break;
                }
            }
            if (!selected) break;
            if (selected->cancellation->cancelled) {
                Finish(*selected, c.peer_id, c.egress, FlowMeshDeliveryOutcome::CANCELLED); continue;
            }
            c.in_flight = std::move(*selected); c.tx_charge = c.in_flight->Size();
            --frames;
            return Frame(c, static_cast<uint8_t>(c.in_flight->kind), *c.in_flight->inner);
        }
        if (now - c.last_send >= std::chrono::seconds{10}) { --frames; return Frame(c, PING, {}); }
        return true;
    }
    bool Pump(Connection& c, Sock::Event events, Clock::time_point now, size_t& bytes, size_t& frames)
    {
        if (c.connecting) {
            if (!(events & Sock::SEND)) return true;
            int error{0}; socklen_t len{sizeof(error)};
            if (c.sock->GetSockOpt(SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error) return false;
            c.connecting = false;
        }
        if ((events & Sock::RECV) && bytes && frames) {
            size_t allowance{std::min<size_t>(64 * 1024, bytes)};
            for (size_t work{0}; allowance && frames && work < 16 && !stopping; ++work) {
                const size_t before{allowance};
                if (c.stage == Connection::Stage::FRAME) {
                    if (c.pending_ingress) break;
                    // A complete authenticated three-channel set is required.
                    const auto peer{peers.find(c.peer_key)};
                    if (peer == peers.end() || !peer->second.notified) break;
                    --frames;
                    const bool valid{ReadFrame(c, allowance, now)};
                    bytes -= before - allowance;
                    if (!valid) return false;
                } else {
                    const auto n{c.sock->Recv(c.rx.data() + c.rx_pos, std::min(c.rx.size() - c.rx_pos, allowance), 0)};
                    if (n < 0) { if (!RetryError()) return false; break; }
                    if (n == 0) return false;
                    c.rx_pos += n; allowance -= n; bytes -= n;
                    if (c.rx_pos == c.rx.size()) {
                        --frames;
                        if (c.stage == Connection::Stage::HELLO ? !ReceiveHello(c) : !Authenticate(c)) return false;
                    }
                }
                if (before == allowance) break;
            }
        }
        if (!PrepareSend(c, now, frames)) return false;
        if ((events & Sock::SEND) && !c.tx.empty() && bytes) {
            const auto allowance{std::min({size_t{64 * 1024}, bytes, c.tx.size() - c.tx_pos})};
            if (!global_tx[c.channel].Take(allowance, now)) return true;
            const auto n{c.sock->Send(c.tx.data() + c.tx_pos, allowance, MSG_NOSIGNAL)};
            if (n < 0) return RetryError();
            if (!n) return false;
            c.tx_pos += n; bytes -= n; c.last_send = now; IoBytes(c, n, true);
            if (c.tx_pos == c.tx.size()) {
                if (c.in_flight) {
                    auto p{peers.find(c.peer_key)}; if (p != peers.end()) ++p->second.info.sent_messages;
                    auto completed{std::move(*c.in_flight)}; c.in_flight.reset();
                    Finish(completed, c.peer_id, c.egress, FlowMeshDeliveryOutcome::SOCKET_WRITTEN);
                }
                c.tx_charge = 0; Bytes{}.swap(c.tx); c.tx_pos = 0; c.tx_application = false;
            }
        }
        return true;
    }

    void AddOutgoing(size_t target, uint8_t channel, Clock::time_point now)
    {
        auto& t{targets[target]}; t.next[channel] = now + RECONNECT_DELAY;
        auto sock{CreateSock(t.address.GetSAFamily(), SOCK_STREAM, IPPROTO_TCP)};
        if (!sock || !Configure(*sock)) return;
        sockaddr_storage address{}; socklen_t length{sizeof(address)};
        if (!t.address.GetSockAddr(reinterpret_cast<sockaddr*>(&address), &length)) return;
        const int result{sock->Connect(reinterpret_cast<sockaddr*>(&address), length)};
        if (result != 0 && !RetryError()) return;
        auto c{std::make_unique<Connection>(next_connection++, std::move(sock))};
        c->target = target; c->outbound = true; c->connecting = result != 0; c->channel = channel;
        c->address = t.address.ToStringAddrPort(); c->local_hello = Hello(channel); c->tx = c->local_hello;
        connections.emplace(c->id, std::move(c));
    }
    void Dial(Clock::time_point now)
    {
        for (size_t i{0}; i < targets.size(); ++i) for (uint8_t channel{0}; channel < CHANNELS; ++channel) {
            if (connections.size() >= config.max_peers * CHANNELS + MAX_HANDSHAKES || stopping) return;
            const auto& t{targets[i]}; if (now < t.next[channel]) continue;
            const std::string identity{t.pin ? HexStr(*t.pin) : t.learned_key};
            auto p{peers.find(identity)};
            if (p != peers.end() && p->second.channels[channel]) continue;
            const bool pending{std::any_of(connections.begin(), connections.end(), [&](const auto& entry) { return entry.second->target == i && entry.second->channel == channel && !entry.second->dead; })};
            if (!pending) AddOutgoing(i, channel, now);
        }
    }
    void Accept(Clock::time_point now)
    {
        if (!listener) return;
        for (size_t n{0}; n < MAX_HANDSHAKES && !stopping; ++n) {
            sockaddr_storage address{}; socklen_t length{sizeof(address)};
            auto sock{listener->Accept(reinterpret_cast<sockaddr*>(&address), &length)};
            if (!sock) return;
            const size_t pending{static_cast<size_t>(std::count_if(connections.begin(), connections.end(), [](const auto& entry) { return !entry.second->outbound && !entry.second->authenticated; }))};
            if (pending >= MAX_HANDSHAKES || connections.size() >= config.max_peers * CHANNELS + MAX_HANDSHAKES || !accept_budget.Take(1, now) || !Configure(*sock)) continue;
            auto c{std::make_unique<Connection>(next_connection++, std::move(sock))};
            CService remote;
            if (remote.SetSockAddr(reinterpret_cast<sockaddr*>(&address), length)) c->address = remote.ToStringAddrPort();
            connections.emplace(c->id, std::move(c));
        }
    }
    void Clean()
    {
        size_t discarded_ingress{0};
        std::string ingress_error;
        for (auto& [id, c] : connections) {
            if (!c->dead || !c->authenticated) continue;
            auto p{peers.find(c->peer_key)};
            if (p == peers.end() || p->second.channels[c->channel] != id) continue;
            std::vector<Packet> abandoned;
            {
                std::lock_guard lock{mutex}; admission_peers.erase(p->second.info.id);
                for (auto& queue : p->second.egress->queues) {
                    for (size_t lane{0}; lane < 5; ++lane) {
                        while (!queue.lanes[lane].empty()) abandoned.push_back(queue.Pop(lane));
                    }
                }
            }
            for (const auto& packet : abandoned) Finish(packet, p->second.info.id, p->second.egress,
                packet.cancellation->cancelled ? FlowMeshDeliveryOutcome::CANCELLED :
                stopping ? FlowMeshDeliveryOutcome::STOPPED : FlowMeshDeliveryOutcome::DISCONNECTED);
            if (p->second.notified) sink.FlowMeshPeerDisconnected(p->second.info.id);
            for (uint64_t channel : p->second.channels) { auto other{connections.find(channel)}; if (other != connections.end()) other->second->dead = true; }
            peers.erase(p);
        }
        for (auto it{connections.begin()}; it != connections.end();) {
            auto& c{*it->second};
            if (!c.dead) { ++it; continue; }
            if (c.in_flight) Finish(*c.in_flight, c.peer_id, c.egress,
                c.in_flight->cancellation->cancelled ? FlowMeshDeliveryOutcome::CANCELLED :
                stopping ? FlowMeshDeliveryOutcome::STOPPED : FlowMeshDeliveryOutcome::DISCONNECTED);
            rx_bytes[c.channel] -= c.rx_charge + c.pending_ingress_charge;
            if (c.pending_ingress) {
                ++discarded_ingress;
                if (!c.ingress_failure.empty() || ingress_error.empty()) {
                    ingress_error = c.ingress_failure.empty() ? "connection closed while runtime admission was pending" : c.ingress_failure;
                }
            }
            it = connections.erase(it);
        }
        if (discarded_ingress) {
            std::lock_guard lock{mutex}; snapshot.ingress_discarded_messages += discarded_ingress;
            snapshot.last_ingress_error = std::move(ingress_error);
        }
    }
    void Publish()
    {
        std::vector<FlowMeshNetPeer> public_peers;
        std::lock_guard lock{mutex};
        for (const auto& [identity, peer] : peers) {
            auto info{peer.info}; info.live = peer.channels[0] != 0;
            info.actions = peer.channels[1] != 0; info.bulk = peer.channels[2] != 0;
            info.authenticated = peer.notified; info.traffic = peer.egress->traffic;
            for (const auto& traffic : info.traffic) info.queued_bytes += traffic.queued_bytes;
            for (uint64_t id : peer.channels) {
                auto c{connections.find(id)};
                if (c != connections.end()) info.pending_ingress_bytes += c->second->pending_ingress_charge;
            }
            public_peers.push_back(std::move(info));
        }
        snapshot.peers = std::move(public_peers);
        snapshot.outbox_queued_bytes = 0; // Admission now reserves the final peer queues synchronously.
        snapshot.pending_ingress_bytes = 0;
        for (const auto& [id, c] : connections) snapshot.pending_ingress_bytes += c->pending_ingress_charge;
    }
    bool ReceiveDeadline(Connection& c, Clock::time_point now)
    {
        if (!c.authenticated) return true;
        const auto timeout{ClassifyFlowMeshNetReceiveTimeout(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - c.last_receive),
            std::chrono::duration_cast<std::chrono::milliseconds>(now - c.frame_started),
            c.prefix_pos != 0, c.pending_ingress.has_value())};
        if (timeout == FlowMeshNetReceiveTimeout::NONE) return true;
        std::lock_guard lock{mutex};
        const std::string traffic{c.channel == 0 ? "critical" : c.channel == 1 ? "action" : "bulk"};
        if (timeout == FlowMeshNetReceiveTimeout::FRAME) {
            ++snapshot.receive_frame_timeouts;
            snapshot.last_disconnect_reason = traffic + " whole-frame receive deadline exceeded (five minutes)";
        } else {
            ++snapshot.receive_idle_timeouts;
            snapshot.last_disconnect_reason = traffic + " socket receive idle timeout (30 seconds without bytes)";
        }
        return false;
    }
    void Run()
    {
        try {
            while (!stopping) {
                auto now{Clock::now()}; Clean(); Dial(now);
                Sock::EventsPerSock wait;
                if (listener) wait.emplace(listener, Sock::Events{Sock::RECV});
                for (auto& [id, c] : connections) {
                    bool queued{false};
                    if (c->egress) { std::lock_guard lock{mutex}; queued = c->egress->queues[c->channel].count != 0; }
                    const auto peer{peers.find(c->peer_key)};
                    const bool receive{!c->pending_ingress && (c->stage != Connection::Stage::FRAME || (peer != peers.end() && peer->second.notified))};
                    const bool send{c->connecting || !c->tx.empty() || queued || (c->authenticated && now - c->last_send >= std::chrono::seconds{10})};
                    wait.emplace(c->sock, Sock::Events{static_cast<Sock::Event>((receive ? Sock::RECV : 0) | (send ? Sock::SEND : 0))});
                }
                if (wait.empty()) { std::this_thread::sleep_for(IO_WAIT); continue; }
                if (!wait.begin()->first->WaitMany(IO_WAIT, wait)) throw std::runtime_error{"FlowMesh socket poll failed"};
                now = Clock::now();
                if (listener && (wait.at(listener).occurred & Sock::RECV)) Accept(now);
                // Critical first, then independently capped action and bulk work.
                // Rotate within each class so a slow/high-volume peer cannot
                // monopolize its class. A single bounded frame crypto/decode
                // remains non-preemptible; report measured pass work explicitly.
                for (uint8_t channel{0}; channel < CHANNELS; ++channel) {
                    const auto work_start{Clock::now()};
                    size_t bytes{PASS_BYTES[channel]}, frames{PASS_FRAMES[channel]};
                    std::vector<uint64_t> ordered;
                    for (const auto& [id, c] : connections) if (!c->dead && c->channel == channel) ordered.push_back(id);
                    const auto pivot{std::lower_bound(ordered.begin(), ordered.end(), next_socket[channel])};
                    std::rotate(ordered.begin(), pivot, ordered.end());
                    for (const auto id : ordered) {
                        if (stopping || !bytes || !frames) break;
                        auto& c{*connections.at(id)}; next_socket[channel] = id + 1;
                        auto events{wait.find(c.sock)}; if (events == wait.end()) continue;
                        const auto peer_start{Clock::now()};
                        const size_t bytes_before{bytes}, frames_before{frames};
                        // Retrying a retained payload also copies bounded work
                        // into the sink; charge it against this class's pass.
                        if (c.pending_ingress && now >= c.next_ingress_retry) --frames;
                        if (!RetryIngress(c, now) || (events->second.occurred & Sock::ERR) ||
                            (!c.authenticated && now - c.created > HANDSHAKE_TIMEOUT) ||
                            !ReceiveDeadline(c, now) ||
                            !Pump(c, events->second.occurred, now, bytes, frames)) c.dead = true;
                        else if (c.authenticated) {
                            auto peer{peers.find(c.peer_key)};
                            if (peer != peers.end() && !peer->second.notified && now - c.created > HANDSHAKE_TIMEOUT) c.dead = true;
                        }
                        if (c.egress) {
                            const auto us{std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - peer_start).count()};
                            std::lock_guard lock{mutex};
                            c.egress->traffic[channel].max_pass_work_us = std::max(c.egress->traffic[channel].max_pass_work_us, static_cast<uint64_t>(us));
                            c.egress->traffic[channel].max_pass_bytes = std::max(c.egress->traffic[channel].max_pass_bytes, uint64_t{bytes_before - bytes});
                            c.egress->traffic[channel].max_pass_operations = std::max(c.egress->traffic[channel].max_pass_operations, uint64_t{frames_before - frames});
                        }
                    }
                    const auto us{std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - work_start).count()};
                    std::lock_guard lock{mutex};
                    snapshot.traffic[channel].max_pass_work_us = std::max(snapshot.traffic[channel].max_pass_work_us, static_cast<uint64_t>(us));
                    snapshot.traffic[channel].max_pass_bytes = std::max(snapshot.traffic[channel].max_pass_bytes, uint64_t{PASS_BYTES[channel] - bytes});
                    snapshot.traffic[channel].max_pass_operations = std::max(snapshot.traffic[channel].max_pass_operations, uint64_t{PASS_FRAMES[channel] - frames});
                }
                Clean(); Publish();
            }
        } catch (const std::exception& e) { std::lock_guard lock{mutex}; snapshot.error = e.what(); }
        catch (...) { std::lock_guard lock{mutex}; snapshot.error = "FlowMesh transport stopped after an internal error"; }
        stopping = true;
        for (auto& [id, c] : connections) c->dead = true;
        try { Clean(); } catch (...) { peers.clear(); connections.clear(); }
        listener.reset();
        std::lock_guard lock{mutex}; admission_peers.clear(); cancellations.clear(); rx_bytes.fill(0);
        snapshot.running = false; snapshot.listening = false; snapshot.peers.clear();
        snapshot.pending_ingress_bytes = snapshot.outbox_queued_bytes = 0;
        for (auto& traffic : snapshot.traffic) traffic.queued_bytes = traffic.queued_messages = 0;
    }
};

FlowMeshNetService::FlowMeshNetService(FlowMeshNetConfig config, flowmesh::WireMessageSink& sink)
    : m_impl{std::make_unique<Impl>(std::move(config), sink)} {}
FlowMeshNetService::~FlowMeshNetService() { Stop(); }
bool FlowMeshNetService::Start(std::string& error)
{
    auto& s{*m_impl};
    if (s.worker.joinable()) { error = "FlowMesh network already started; Stop before restarting"; return false; }
    try {
        if (s.config.domain.IsNull() || s.config.max_peers == 0 || s.config.max_peers > 32 || s.config.peers.size() > s.config.max_peers) throw std::runtime_error{"Invalid FlowMesh domain or peer bounds"};
        if (s.config.ingress_retry_timeout <= std::chrono::milliseconds{0} || s.config.ingress_retry_timeout > INGRESS_RETRY_TIMEOUT) throw std::runtime_error{"FlowMesh ingress retry timeout must be positive and at most 30 seconds"};
        Role(s.config.role);
        s.targets.clear(); for (const auto& peer : s.config.peers) s.targets.push_back(ParseTarget(peer, s.config.port));
        s.LoadKey();
        std::string bound;
        if (s.config.enable_listen) {
            const auto address{LookupNumeric(s.config.bind_host, s.config.port)};
            if ((!address.IsIPv4() && !address.IsIPv6()) || (s.config.port != 0 && !address.IsValid())) throw std::runtime_error{"FlowMesh bind must be a numeric address"};
            auto socket{CreateSock(address.GetSAFamily(), SOCK_STREAM, IPPROTO_TCP)};
            if (!socket || !Configure(*socket)) throw std::runtime_error{"Cannot create FlowMesh listener"};
            const int one{1}; (void)socket->SetSockOpt(SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            sockaddr_storage raw{}; socklen_t length{sizeof(raw)};
            if (!address.GetSockAddr(reinterpret_cast<sockaddr*>(&raw), &length) || socket->Bind(reinterpret_cast<sockaddr*>(&raw), length) != 0 || socket->Listen(16) != 0) throw std::runtime_error{"Cannot bind FlowMesh listener"};
            length = sizeof(raw); CService actual;
            if (socket->GetSockName(reinterpret_cast<sockaddr*>(&raw), &length) != 0 || !actual.SetSockAddr(reinterpret_cast<sockaddr*>(&raw), length)) throw std::runtime_error{"Cannot inspect FlowMesh listener"};
            bound = actual.ToStringAddrPort(); s.listener = std::move(socket);
        }
        { std::lock_guard lock{s.mutex}; s.snapshot = {}; s.snapshot.running = true; s.snapshot.listening = static_cast<bool>(s.listener); s.snapshot.operator_pubkey = HexStr(s.pubkey); s.snapshot.bind_address = bound; }
        s.stopping = false;
        s.worker = std::thread{[&s] { s.Run(); }};
        error.clear(); return true;
    } catch (const std::exception& e) {
        error = e.what(); Stop(); std::lock_guard lock{s.mutex}; s.snapshot.error = error; return false;
    }
}
void FlowMeshNetService::Stop()
{
    auto& s{*m_impl}; s.stopping = true;
    if (s.worker.joinable()) s.worker.join();
    s.listener.reset();
    if (s.directory_locked) { UnlockDirectory(s.config.datadir, ".flowmesh-network.lock"); s.directory_locked = false; }
    std::lock_guard lock{s.mutex};
    s.snapshot.running = false; s.snapshot.listening = false; s.snapshot.peers.clear();
    s.snapshot.pending_ingress_bytes = s.snapshot.outbox_queued_bytes = 0;
}
FlowMeshRelayResult FlowMeshNetService::Relay(const FlowMeshRuntimeRelay& relay)
{
    using Admission = FlowMeshDeliveryAdmission;
    auto& s{*m_impl}; FlowMeshRelayResult result;
    const auto reject_all = [&](Admission admission, const std::string& reason) {
        result.no_peer_reason = admission; result.reason = reason;
        if (relay.peer) result.peers.push_back({*relay.peer, admission, reason});
        s.EgressRejected(reason);
        return result;
    };
    if (s.stopping) return reject_all(Admission::STOPPED, "independent network is stopped");
    if (relay.peer && (*relay.peer >= -1 || *relay.peer == std::numeric_limits<flowmesh::WirePeerId>::min())) {
        return reject_all(Admission::INVALID, "invalid independent destination peer");
    }
    flowmesh::WireCheck check; auto encoded{flowmesh::EncodeWireMessage(relay.message, check)};
    if (!encoded) return reject_all(Admission::INVALID, std::string{"invalid independent outgoing frame: "} + flowmesh::WireCheckName(check));
    const auto channel{Channel(relay.message.kind)};
    const std::string traffic_name{channel == 0 ? "critical" : channel == 1 ? "action" : "bulk"};
    Packet packet{relay.message.kind, std::make_shared<const Bytes>(std::move(*encoded)), relay.delivery_id, {}, relay.message.header, {}};
    if (LogAcceptCategory(BCLog::BENCH, BCLog::Level::Debug)) packet.wire_hash = Hash(*packet.inner);
    const size_t bytes{packet.Size()};
    std::unique_lock lock{s.mutex};
    if (!s.snapshot.running || s.stopping) {
        ++s.snapshot.egress_rejected_messages; s.snapshot.last_egress_error = "independent network is not running";
        result.no_peer_reason = Admission::STOPPED; result.reason = s.snapshot.last_egress_error;
        if (relay.peer) result.peers.push_back({*relay.peer, Admission::STOPPED, result.reason});
        lock.unlock();
        NetworkTrace("queue_refused", packet.kind, packet.header, packet.wire_hash,
                     relay.peer.value_or(0), packet.delivery_id, result.reason);
        return result;
    }
    if (relay.delivery_id) {
        const auto it{s.cancellations.find(relay.delivery_id)};
        if (it != s.cancellations.end()) packet.cancellation = it->second;
    }
    if (!packet.cancellation) packet.cancellation = std::make_shared<Packet::Cancellation>();
    for (const auto& [id, egress] : s.admission_peers) {
        if ((relay.peer && *relay.peer != id) || (relay.exclude_peer && *relay.exclude_peer == id)) continue;
        auto& peer{egress->traffic[channel]}; auto& global{s.snapshot.traffic[channel]};
        Admission admission{Admission::ADMITTED}; std::string reason;
        if (packet.cancellation->cancelled) {
            admission = Admission::INVALID; reason = "delivery id has already been cancelled";
        } else if (bytes > PEER_QUEUE_BYTES[channel] - peer.queued_bytes) {
            admission = Admission::FULL; reason = traffic_name + " per-peer byte limit";
        } else if (peer.queued_messages >= PEER_QUEUE_ITEMS[channel]) {
            admission = Admission::FULL; reason = traffic_name + " per-peer item limit";
        } else if (bytes > GLOBAL_QUEUE_BYTES[channel] - global.queued_bytes) {
            admission = Admission::FULL; reason = traffic_name + " global byte limit";
        } else if (global.queued_messages >= GLOBAL_QUEUE_ITEMS[channel]) {
            admission = Admission::FULL; reason = traffic_name + " global item limit";
        }
        if (admission != Admission::ADMITTED) {
            ++peer.rejected; ++global.rejected; ++s.snapshot.egress_rejected_messages; s.snapshot.last_egress_error = reason;
        } else {
            egress->queues[channel].Push(packet); ++packet.cancellation->references;
            for (auto* traffic : {&peer, &global}) { traffic->queued_bytes += bytes; ++traffic->queued_messages; ++traffic->admitted; }
        }
        result.peers.push_back({id, admission, reason});
    }
    if (packet.cancellation->references && relay.delivery_id) s.cancellations[relay.delivery_id] = packet.cancellation;
    if (!result.peers.empty()) result.no_peer_reason = Admission::ADMITTED; // Individual failures are already explicit per peer.
    if (result.peers.empty()) {
        result.no_peer_reason = Admission::DISCONNECTED; result.reason = "no authenticated independent destination peer";
        if (relay.peer) result.peers.push_back({*relay.peer, Admission::DISCONNECTED, result.reason});
        ++s.snapshot.egress_rejected_messages; ++s.snapshot.traffic[channel].rejected;
        s.snapshot.last_egress_error = result.reason;
    }
    lock.unlock();
    // Optional synchronous debug I/O must not hold the queue-admission lock.
    // These timestamps observe queue outcomes after releasing that lock.
    for (const auto& peer : result.peers) {
        NetworkTrace(peer.admission == Admission::ADMITTED ? "queued" : "queue_refused",
                     packet.kind, packet.header, packet.wire_hash, peer.peer,
                     packet.delivery_id, peer.reason);
    }
    if (result.peers.empty()) NetworkTrace("queue_refused", packet.kind, packet.header, packet.wire_hash,
                                           0, packet.delivery_id, result.reason);
    return result;
}
void FlowMeshNetService::Cancel(uint64_t delivery_id)
{
    if (!delivery_id) return;
    std::lock_guard lock{m_impl->mutex};
    const auto it{m_impl->cancellations.find(delivery_id)};
    if (it != m_impl->cancellations.end()) it->second->cancelled = true;
}
FlowMeshNetSnapshot FlowMeshNetService::Snapshot() const
{
    std::lock_guard lock{m_impl->mutex};
    auto snapshot{m_impl->snapshot};
    snapshot.trace_events_dropped = network_trace_dropped.load(std::memory_order_relaxed);
    return snapshot;
}
} // namespace node
