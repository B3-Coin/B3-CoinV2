// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/facts.h>
#include <flowmesh2/net/https_sse_api.h>
#include <flowmesh2/netio/api_json.h>
#include <flowmesh2/sim/sim_client.h>
#include <flowmesh2/sim/sim_node.h>
#include <flowmesh2/sim/trace.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/require.h>
#include <flowmesh2/test/util/storage_fixtures.h>
#include <univalue.h>
#include <util/fs.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <boost/test/unit_test.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

/** The HTTPS + SSE API (M5-API-01, O-104) over a simulated k = 1 node: TLS 1.3
 * with a self-signed certificate whose SPKI fingerprint the client pins,
 * actions posted and read back, state and health, the stream as SSE with
 * cursors and heartbeats; TLS 1.2 and plaintext refused; limits refuse the
 * excess and a timed-out request still takes its reply so nothing leaks.
 */
using namespace flowmesh2;
using namespace flowmesh2::net;
using namespace flowmesh2::sim;
using namespace flowmesh2::test;

namespace {

struct Response {
    int status{0};
    std::string body;
    std::string headers;
};

//! A blocking TLS client for the tests: one connection per request (Connection: close).
class TlsClient
{
public:
    TlsClient(uint16_t port, int min_version, int max_version) : m_port{port}, m_min{min_version}, m_max{max_version} {}
    ~TlsClient() { Disconnect(); }
    bool Connected()
    {
        Connect();
        return m_connected;
    }
    std::string PeerSpkiFingerprint()
    {
        Connect();
        if (!m_connected) return {};
        X509* cert{SSL_get1_peer_certificate(m_ssl)};
        if (cert == nullptr) return {};
        EVP_PKEY* key{X509_get0_pubkey(cert)};
        unsigned char* der{nullptr};
        const int len{i2d_PUBKEY(key, &der)};
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(der, static_cast<size_t>(len), digest);
        OPENSSL_free(der);
        X509_free(cert);
        return netio::ToHex(std::span<const unsigned char>{digest, SHA256_DIGEST_LENGTH});
    }
    //! One request on its own connection, the whole reply read.
    Response Request(const std::string& method, const std::string& path, const std::string& body = {},
                     const std::string& content_type = "application/octet-stream")
    {
        Disconnect();
        Connect();
        BOOST_REQUIRE(m_connected);
        std::string request{method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"};
        if (!body.empty() || method == "POST") {
            request += "Content-Type: " + content_type + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
        }
        request += "\r\n" + body;
        BOOST_REQUIRE(BIO_write(m_bio, request.data(), static_cast<int>(request.size())) == static_cast<int>(request.size()));
        std::string raw;
        char buf[4096];
        for (;;) {
            const int n{BIO_read(m_bio, buf, sizeof(buf))};
            if (n <= 0) break;
            raw.append(buf, static_cast<size_t>(n));
        }
        Disconnect();
        return Parse(raw);
    }
    //! Open a stream and read until `until` appears in the decoded body or the limit passes; the connection stays open.
    std::string Stream(const std::string& path, const std::string& until, std::chrono::milliseconds limit)
    {
        Disconnect();
        Connect();
        BOOST_REQUIRE(m_connected);
        const std::string request{"GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nAccept: text/event-stream\r\n\r\n"};
        BOOST_REQUIRE(BIO_write(m_bio, request.data(), static_cast<int>(request.size())) == static_cast<int>(request.size()));
        std::string raw;
        char buf[4096];
        const auto deadline{std::chrono::steady_clock::now() + limit};
        while (std::chrono::steady_clock::now() < deadline) {
            const int n{BIO_read(m_bio, buf, sizeof(buf))};
            if (n <= 0) break;
            raw.append(buf, static_cast<size_t>(n));
            if (DecodeChunked(raw).find(until) != std::string::npos) break;
        }
        return DecodeChunked(raw);
    }

    //! Size the open connection's kernel receive buffer (SO_RCVBUF), so how much a stalled reader's socket holds does not
    //! depend on this kernel's autotuning.
    void SetReceiveBuffer(int bytes)
    {
        int fd{-1};
        BOOST_REQUIRE(m_bio != nullptr && BIO_get_fd(m_bio, &fd) >= 0);
        BOOST_REQUIRE_EQUAL(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)), 0);
    }

    //! Read what the open stream connection delivers within `limit` (decoded chunks), up to a gap event or the last chunk;
    //! the connection stays open.
    std::string ReadMore(std::chrono::milliseconds limit)
    {
        if (!m_connected) return {};
        std::string raw;
        char buf[4096];
        const auto deadline{std::chrono::steady_clock::now() + limit};
        // Only what a read added, and the few bytes before it a marker can straddle, is searched. Searching the whole
        // buffer after every read was quadratic in a stalled reader's backlog: 1.5 MB took 7 s under TSan, past the limit.
        constexpr size_t STRADDLE{16};
        while (std::chrono::steady_clock::now() < deadline) {
            const int n{BIO_read(m_bio, buf, sizeof(buf))};
            if (n <= 0) break;
            const size_t from{raw.size() > STRADDLE ? raw.size() - STRADDLE : 0};
            raw.append(buf, static_cast<size_t>(n));
            if (raw.find("event: gap", from) != std::string::npos || raw.find("\r\n0\r\n", from) != std::string::npos) break;
        }
        return DecodeChunks(raw);
    }

    //! The raw bytes the open stream connection delivers within `limit`, read until `until` appears among them or the limit
    //! passes. Not decoded: a read need not end on a chunk boundary, and decoding from inside a chunk loses what follows.
    std::string ReadRawUntil(const std::string& until, std::chrono::milliseconds limit)
    {
        if (!m_connected) return {};
        std::string raw;
        char buf[4096];
        const auto deadline{std::chrono::steady_clock::now() + limit};
        while (std::chrono::steady_clock::now() < deadline) {
            const int n{BIO_read(m_bio, buf, sizeof(buf))};
            if (n <= 0) break;
            const size_t from{raw.size() > until.size() ? raw.size() - until.size() : 0};
            raw.append(buf, static_cast<size_t>(n));
            if (raw.find(until, from) != std::string::npos) break;
        }
        return raw;
    }

    static Response Parse(const std::string& raw)
    {
        Response out;
        const size_t end{raw.find("\r\n\r\n")};
        if (end == std::string::npos) return out;
        out.headers = raw.substr(0, end);
        if (out.headers.size() > 12) out.status = std::atoi(out.headers.substr(9, 3).c_str());
        const std::string rest{raw.substr(end + 4)};
        out.body = out.headers.find("Transfer-Encoding: chunked") != std::string::npos ? DecodeChunks(rest) : rest;
        return out;
    }
    //! The decoded body of a (possibly partial) chunked response.
    static std::string DecodeChunked(const std::string& raw)
    {
        const size_t end{raw.find("\r\n\r\n")};
        if (end == std::string::npos) return {};
        return DecodeChunks(raw.substr(end + 4));
    }
    static std::string DecodeChunks(const std::string& data)
    {
        std::string out;
        size_t pos{0};
        for (;;) {
            const size_t line_end{data.find("\r\n", pos)};
            if (line_end == std::string::npos) break;
            const size_t size{static_cast<size_t>(std::strtoul(data.substr(pos, line_end - pos).c_str(), nullptr, 16))};
            if (size == 0) break;
            if (line_end + 2 + size > data.size()) break;
            out += data.substr(line_end + 2, size);
            pos = line_end + 2 + size + 2;
        }
        return out;
    }

private:
    void Connect()
    {
        if (m_bio != nullptr) return;
        m_ctx = SSL_CTX_new(TLS_client_method());
        BOOST_REQUIRE(m_ctx != nullptr);
        SSL_CTX_set_min_proto_version(m_ctx, m_min);
        SSL_CTX_set_max_proto_version(m_ctx, m_max);
        SSL_CTX_set_verify(m_ctx, SSL_VERIFY_NONE, nullptr); // the pin above replaces the CA check
        m_bio = BIO_new_ssl_connect(m_ctx);
        BOOST_REQUIRE(m_bio != nullptr);
        BIO_get_ssl(m_bio, &m_ssl);
        BOOST_REQUIRE(m_ssl != nullptr);
        const std::string target{"127.0.0.1:" + std::to_string(m_port)};
        BIO_set_conn_hostname(m_bio, target.c_str());
        m_connected = BIO_do_connect(m_bio) > 0 && BIO_do_handshake(m_bio) > 0;
    }
    void Disconnect()
    {
        if (m_bio != nullptr) BIO_free_all(m_bio);
        if (m_ctx != nullptr) SSL_CTX_free(m_ctx);
        m_bio = nullptr;
        m_ctx = nullptr;
        m_ssl = nullptr;
        m_connected = false;
    }

    uint16_t m_port;
    int m_min;
    int m_max;
    SSL_CTX* m_ctx{nullptr};
    BIO* m_bio{nullptr};
    SSL* m_ssl{nullptr};
    bool m_connected{false};
};

UniValue Json(const std::string& text)
{
    UniValue value;
    BOOST_REQUIRE_MESSAGE(value.read(text), "not JSON: " + text);
    return value;
}

//! The node's API with a hand on the stream pump: while `hold` is set, TakeEvents answers no events and leaves the hub's
//! queues alone. The HTTPS loop pumps the hub on its own timer too, so stopping the rig's pumper did not stop the draining;
//! a test that needs a queue to overflow holds it here instead of racing that timer.
class HeldStreams final : public ClientApi
{
public:
    explicit HeldStreams(ClientApi& inner) : m_inner{inner} {}
    std::atomic<bool> hold{false};

    Result<RequestId> Submit(std::span<const unsigned char> frame) override { return m_inner.Submit(frame); }
    Result<RequestId> SubmitStatus(const ActionV2& original) override { return m_inner.SubmitStatus(original); }
    Result<RequestId> SubmitHealth() override { return m_inner.SubmitHealth(); }
    std::optional<ApiReply> TakeReply(RequestId id) override { return m_inner.TakeReply(id); }
    Result<ActionStatusReply> GetAction(const ActionV2& original) const override { return m_inner.GetAction(original); }
    Result<read::AccountReply> GetState(const AccountId& account) const override { return m_inner.GetState(account); }
    Result<StreamId> Subscribe(const StreamRequest& request) override { return m_inner.Subscribe(request); }
    Result<std::vector<StreamEvent>> TakeEvents(StreamId id, size_t max) override
    {
        if (hold) return Ok(std::vector<StreamEvent>{});
        return m_inner.TakeEvents(id, max);
    }
    Result<void> Unsubscribe(StreamId id) override { return m_inner.Unsubscribe(id); }

private:
    ClientApi& m_inner;
};

struct Rig {
    TraceRecorder trace;
    std::unique_ptr<SimNode> node;
    std::optional<SimClient> alice;
    std::optional<SimClient> bob;
    std::unique_ptr<HeldStreams> streams; //!< between the server and the node, so it outlives `api`
    std::unique_ptr<HttpsSseApi> api;
    std::atomic<bool> pumping{true};
    std::thread pumper;
    fs::path dir;

    explicit Rig(HttpsSseConfig config = {}, const SimNodeConfig& node_config = {})
    {
        Result<std::unique_ptr<SimNode>> created{SimNode::Create(node_config, trace)};
        RequireOk(created, "SimNode::Create");
        node = std::move(created).value();
        const std::vector<MarketDescriptor> markets{node->Markets().begin(), node->Markets().end()};
        alice = SimClient::Create("alice", node->Profile(), node->Domain(), node->Seats(), markets).value();
        bob = SimClient::Create("bob", node->Profile(), node->Domain(), node->Seats(), markets).value();
        std::vector<Fact> facts;
        DepositFact a;
        a.cursor = FactCursor{SIM_BASE_HEIGHT + 1, 0};
        a.outpoint = Outpoint{SimLabelHash("fm2/test/https/T1"), 0};
        a.account = alice->Account();
        a.asset = SimAsset("rUSD");
        a.amount = 1'000'000'000;
        DepositFact b;
        b.cursor = FactCursor{SIM_BASE_HEIGHT + 1, 1};
        b.outpoint = Outpoint{SimLabelHash("fm2/test/https/T2"), 0};
        b.account = bob->Account();
        b.asset = AssetId::Native();
        b.amount = 10'000'000'000;
        facts.push_back(Fact{a});
        facts.push_back(Fact{b});
        RequireOk(node->Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 1), facts), "AppendBlock");
        RequireOk(node->SealSlot(), "SealSlot 1");
        dir = fs::PathFromString(FM2_TEST_TMP_DIR) / fs::u8path("https_sse_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(dir);
        fs::create_directories(dir);
        config.profile = node->Profile();
        config.dev_self_signed = true;
        config.self_signed_dir = fs::PathToString(dir);
        if (config.heartbeat_ms == 1'000) config.heartbeat_ms = 150;
        streams = std::make_unique<HeldStreams>(node->Api());
        Result<std::unique_ptr<HttpsSseApi>> served{HttpsSseApi::Create(config, *streams)};
        RequireOk(served, "HttpsSseApi::Create");
        api = std::move(served).value();
        StartPumper();
    }
    void StartPumper()
    {
        pumping = true;
        pumper = std::thread{[this] {
            while (pumping) {
                (void)node->Pump(16);
                api->Notify();
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
        }};
    }
    ~Rig()
    {
        pumping = false;
        if (pumper.joinable()) pumper.join();
        api->Stop();
        fs::remove_all(dir);
    }
    std::string Frame(const std::vector<unsigned char>& bytes) const { return std::string{bytes.begin(), bytes.end()}; }
};

} // namespace

BOOST_AUTO_TEST_SUITE(https_sse_tests)

BOOST_AUTO_TEST_CASE(posts_reads_and_streams_over_tls)
{
    Rig rig;
    const uint16_t port{rig.api->Port()};
    BOOST_CHECK_NE(port, 0);
    // The pin: the client's view of the certificate equals the server's fingerprint and the file the daemon writes.
    {
        TlsClient client{port, TLS1_3_VERSION, TLS1_3_VERSION};
        BOOST_REQUIRE(client.Connected());
        BOOST_CHECK_EQUAL(client.PeerSpkiFingerprint(), rig.api->CertificateFingerprint());
        std::ifstream fingerprint{fs::PathToString(rig.dir / fs::u8path("api.fingerprint"))};
        std::string line;
        std::getline(fingerprint, line);
        BOOST_CHECK_EQUAL(line, rig.api->CertificateFingerprint());
        const Response health{client.Request("GET", "/v2/health")};
        BOOST_CHECK_EQUAL(health.status, 200);
        const UniValue h{Json(health.body)};
        BOOST_CHECK(!h["fenced"].get_bool());
        BOOST_CHECK_EQUAL(h["published_height"].getInt<uint64_t>(), 1U);
    }
    // POST alice's bid: ADMITTED with its id; the same bytes again: DUPLICATE; GET by id: PENDING, then INCLUDED after the slot.
    const MarketDescriptor m1{SimMarketM1()};
    const std::vector<unsigned char> bid{rig.alice->Place("bid", 1, m1, Side::BUY, TimeInForce::GTC, 100, 3, 1).value()};
    std::string action_id;
    {
        TlsClient client{port, TLS1_3_VERSION, TLS1_3_VERSION};
        const Response posted{client.Request("POST", "/v2/actions", rig.Frame(bid))};
        BOOST_REQUIRE_EQUAL(posted.status, 200);
        const UniValue reply{Json(posted.body)};
        BOOST_CHECK_EQUAL(reply["code"].get_str(), "ADMITTED");
        action_id = reply["action_id"].get_str();
        BOOST_CHECK_EQUAL(action_id.size(), 64U);
    }
    {
        TlsClient client{port, TLS1_3_VERSION, TLS1_3_VERSION};
        const Response again{client.Request("POST", "/v2/actions", rig.Frame(bid))};
        BOOST_REQUIRE_EQUAL(again.status, 200);
        BOOST_CHECK_EQUAL(Json(again.body)["code"].get_str(), "DUPLICATE");
    }
    {
        TlsClient client{port, TLS1_3_VERSION, TLS1_3_VERSION};
        const Response status{client.Request("GET", "/v2/actions/" + action_id)};
        BOOST_REQUIRE_EQUAL(status.status, 200);
        BOOST_CHECK_EQUAL(Json(status.body)["status"].get_str(), "PENDING");
    }
    RequireOk(rig.node->SealSlot(), "SealSlot 2");
    {
        TlsClient client{port, TLS1_3_VERSION, TLS1_3_VERSION};
        const Response status{client.Request("GET", "/v2/actions/" + action_id)};
        BOOST_REQUIRE_EQUAL(status.status, 200);
        const UniValue s{Json(status.body)};
        BOOST_CHECK_EQUAL(s["status"].get_str(), "INCLUDED");
        BOOST_CHECK_EQUAL(s["included"]["ref"]["height"].getInt<uint64_t>(), 2U);
        BOOST_CHECK_EQUAL(s["included"]["outcome"]["code"].get_str(), "RESTING");
        BOOST_CHECK(!s["included"]["envelope"].get_str().empty());
        // The status by the original bytes too.
        const Response by_bytes{client.Request("POST", "/v2/actions/status", rig.Frame(bid))};
        BOOST_REQUIRE_EQUAL(by_bytes.status, 200);
        BOOST_CHECK_EQUAL(Json(by_bytes.body)["status"].get_str(), "INCLUDED");
    }
    {
        TlsClient client{port, TLS1_3_VERSION, TLS1_3_VERSION};
        const Response state{client.Request("GET", "/v2/state/" + rig.alice->Account().id.GetHex())};
        BOOST_REQUIRE_EQUAL(state.status, 200);
        const UniValue s{Json(state.body)};
        BOOST_CHECK_EQUAL(s["status"].get_str(), "FOUND");
        BOOST_CHECK_EQUAL(s["head"]["label"].get_str(), "CERTIFIED");
        BOOST_CHECK_EQUAL(s["head"]["height"].getInt<uint64_t>(), 2U);
        BOOST_CHECK_EQUAL(s["witness"]["leaf"]["open_orders"].getInt<uint64_t>(), 1U);
        BOOST_CHECK(!s["witness"]["proof"]["encoded"].get_str().empty());
        const Response unknown{client.Request("GET", "/v2/state/" + std::string(64, '0'))};
        BOOST_CHECK_EQUAL(unknown.status, 200);
        BOOST_CHECK_EQUAL(Json(unknown.body)["status"].get_str(), "UNKNOWN");
        const Response bad{client.Request("GET", "/v2/state/zz")};
        BOOST_CHECK_EQUAL(bad.status, 400);
        const Response missing{client.Request("GET", "/v2/nothing")};
        BOOST_CHECK_EQUAL(missing.status, 404);
        const Response garbage{client.Request("POST", "/v2/actions", "not an action")};
        BOOST_CHECK_EQUAL(garbage.status, 200); // a definite admission answer
        BOOST_CHECK_NE(Json(garbage.body)["code"].get_str(), "ADMITTED"); // a definite refusal code (WRONG_DOMAIN here: A2 before the shape)
        const Response unknown_id{client.Request("GET", "/v2/actions/" + std::string(64, 'a'))};
        BOOST_CHECK_EQUAL(unknown_id.status, 404);
    }
    // A static document.
    rig.api->AddStatic("/v2/genesis", "application/json", "{\"hello\":1}");
    {
        TlsClient client{port, TLS1_3_VERSION, TLS1_3_VERSION};
        const Response genesis{client.Request("GET", "/v2/genesis")};
        BOOST_CHECK_EQUAL(genesis.status, 200);
        BOOST_CHECK_EQUAL(genesis.body, "{\"hello\":1}");
    }
    // The stream: a snapshot of slot 2, a heartbeat, then slot 3 as a delta.
    {
        TlsClient client{port, TLS1_3_VERSION, TLS1_3_VERSION};
        std::thread sealer{[&] {
            std::this_thread::sleep_for(std::chrono::milliseconds{400});
            RequireOk(rig.node->SealSlot(), "SealSlot 3");
        }};
        const std::string events{client.Stream("/v2/stream?channel=slots", "\"head_slot\":3", std::chrono::milliseconds{5'000})};
        sealer.join();
        BOOST_CHECK(events.find("event: snapshot") != std::string::npos);
        BOOST_CHECK(events.find("event: heartbeat") != std::string::npos);
        BOOST_CHECK(events.find("event: delta") != std::string::npos);
        BOOST_CHECK(events.find("id: " + std::to_string(rig.node->Seat().Streams().StreamEpoch()) + ":1:2") != std::string::npos);
        BOOST_CHECK(events.find("\"channel\":\"slots\"") != std::string::npos);
    }
    {
        TlsClient client{port, TLS1_3_VERSION, TLS1_3_VERSION};
        const Response bad{client.Request("GET", "/v2/stream?channel=nope")};
        BOOST_CHECK_EQUAL(bad.status, 400);
        const Response filter{client.Request("GET", "/v2/stream?channel=account")};
        BOOST_CHECK_EQUAL(filter.status, 400);
    }
    const HttpsSseStatus status{rig.api->Status()};
    BOOST_CHECK_GE(status.requests_served, 12U);
    BOOST_CHECK_EQUAL(status.port, port);
}

BOOST_AUTO_TEST_CASE(older_tls_and_plaintext_are_refused)
{
    Rig rig;
    const uint16_t port{rig.api->Port()};
    TlsClient old{port, TLS1_2_VERSION, TLS1_2_VERSION};
    BOOST_CHECK(!old.Connected());
    TlsClient fresh{port, TLS1_3_VERSION, TLS1_3_VERSION};
    BOOST_CHECK(fresh.Connected());
    // Plaintext: the server speaks TLS only, so an HTTP request gets no HTTP answer.
    const int fd{::socket(AF_INET, SOCK_STREAM, 0)};
    BOOST_REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOST_REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    const std::string request{"GET /v2/health HTTP/1.0\r\n\r\n"};
    BOOST_REQUIRE(::send(fd, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()));
    timeval tv{1, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[256];
    const ssize_t n{::recv(fd, buf, sizeof(buf), 0)};
    ::close(fd);
    const bool no_http{n <= 0 || std::string(buf, static_cast<size_t>(n)).find("HTTP/1.") == std::string::npos};
    BOOST_CHECK(no_http);
    // Refusals at Create.
    HttpsSseConfig zero_rtt;
    zero_rtt.zero_rtt = true;
    RequireRefusal(HttpsSseApi::Create(zero_rtt, rig.node->Api()), ErrorCode::INVALID_INPUT, HTTPS_ZERO_RTT, "0-RTT");
    HttpsSseConfig no_cert;
    RequireRefusal(HttpsSseApi::Create(no_cert, rig.node->Api()), ErrorCode::INVALID_INPUT, HTTPS_CERTIFICATE, "no certificate");
}

BOOST_AUTO_TEST_CASE(limits_and_timeouts_refuse_and_never_leak)
{
    {
        HttpsSseConfig config;
        config.max_connections_per_ip = 2;
        Rig rig{config};
        const uint16_t port{rig.api->Port()};
        // Two streams hold the per-IP budget; a third request is refused with 429 while they are open.
        TlsClient one{port, TLS1_3_VERSION, TLS1_3_VERSION};
        TlsClient two{port, TLS1_3_VERSION, TLS1_3_VERSION};
        (void)one.Stream("/v2/stream?channel=slots", "event: snapshot", std::chrono::milliseconds{3'000});
        (void)two.Stream("/v2/stream?channel=slots", "event: snapshot", std::chrono::milliseconds{3'000});
        TlsClient three{port, TLS1_3_VERSION, TLS1_3_VERSION};
        const Response refused{three.Request("GET", "/v2/health")};
        BOOST_CHECK_EQUAL(refused.status, 429);
        BOOST_CHECK_GE(rig.api->Status().refused, 1U);
        BOOST_CHECK_EQUAL(rig.api->Status().streams, 2U);
    }
    {
        // Without the node's pump a POST times out with 504; once the node pumps, the reply is taken: no open request stays behind.
        HttpsSseConfig config;
        config.reply_deadline_ms = 50;
        Rig rig{config};
        rig.pumping = false;
        if (rig.pumper.joinable()) rig.pumper.join();
        const MarketDescriptor m1{SimMarketM1()};
        const std::vector<unsigned char> bid{rig.alice->Place("late", 1, m1, Side::BUY, TimeInForce::GTC, 100, 1, 1).value()};
        TlsClient client{rig.api->Port(), TLS1_3_VERSION, TLS1_3_VERSION};
        const Response timed_out{client.Request("POST", "/v2/actions", rig.Frame(bid))};
        BOOST_CHECK_EQUAL(timed_out.status, 504);
        BOOST_CHECK_EQUAL(rig.node->Api().OpenRequests(), 1U);
        RequireOk(rig.node->Pump(4), "Pump");
        rig.api->Notify();
        const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{3}};
        while (rig.node->Api().OpenRequests() > 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        BOOST_CHECK_EQUAL(rig.node->Api().OpenRequests(), 0U);
        // The action was admitted meanwhile, so its status is known afterwards (the pump running again).
        rig.StartPumper();
        TlsClient later{rig.api->Port(), TLS1_3_VERSION, TLS1_3_VERSION};
        const Response status{later.Request("POST", "/v2/actions/status", rig.Frame(bid))};
        BOOST_CHECK_EQUAL(status.status, 200);
        BOOST_CHECK_EQUAL(Json(status.body)["status"].get_str(), "PENDING");
    }
}

BOOST_AUTO_TEST_CASE(a_gap_closes_streams_cleanly_and_new_readers_are_served)
{
    // Review of 2026-11-16, defect 5: a gap closed the stream from inside the loop over the streams map, erasing the
    // element the loop was on. Two readers fall behind at once (the node seals more slots than the hub queues
    // between two pumps); both streams end with a gap event, the map survives, and a new reader is served.
    HttpsSseConfig config;
    SimNodeConfig node_config;
    node_config.streams.max_queued_events = 8;
    Rig rig{config, node_config};
    const uint16_t port{rig.api->Port()};
    TlsClient one{port, TLS1_3_VERSION, TLS1_3_VERSION};
    TlsClient two{port, TLS1_3_VERSION, TLS1_3_VERSION};
    BOOST_CHECK(one.Stream("/v2/stream?channel=slots", "event: snapshot", std::chrono::milliseconds{3'000}).find("event: snapshot") != std::string::npos);
    BOOST_CHECK(two.Stream("/v2/stream?channel=slots", "event: snapshot", std::chrono::milliseconds{3'000}).find("event: snapshot") != std::string::npos);
    BOOST_CHECK_EQUAL(rig.api->Status().streams, 2U);
    // Nothing taken from the hub while twelve slots are sealed: both queues overflow the cap of eight. (Stopping the rig's
    // pumper was not enough: the server's own timer pumps the hub every heartbeat_ms / 4 and drained the queues in about
    // four runs of ten.)
    rig.streams->hold = true;
    for (int i{0}; i < 12; ++i) RequireOk(rig.node->SealSlot(), "SealSlot");
    rig.streams->hold = false;
    rig.api->Notify();
    const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{5}};
    while (rig.api->Status().streams > 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds{5});
    BOOST_CHECK_EQUAL(rig.api->Status().streams, 0U);
    BOOST_CHECK(one.ReadMore(std::chrono::milliseconds{2'000}).find("event: gap") != std::string::npos);
    BOOST_CHECK(two.ReadMore(std::chrono::milliseconds{2'000}).find("event: gap") != std::string::npos);
    TlsClient later{port, TLS1_3_VERSION, TLS1_3_VERSION};
    BOOST_CHECK(later.Stream("/v2/stream?channel=slots", "event: snapshot", std::chrono::milliseconds{3'000}).find("event: snapshot") != std::string::npos);
    BOOST_CHECK_EQUAL(rig.api->Status().streams, 1U);
}

BOOST_AUTO_TEST_CASE(a_slow_reader_holds_events_in_the_hub_not_in_memory)
{
    // Review of 2026-11-16, lead R2 (confirmed): events went into libevent's output buffer however slowly the client
    // read, so memory grew with the reader's lag and the hub's gap never came. Once stream_output_bytes are queued to a
    // connection its events wait in the hub; the hub's cap then ends the stream with a gap.
    HttpsSseConfig config;
    config.stream_output_bytes = 4 * 1024;
    SimNodeConfig node_config;
    node_config.streams.max_queued_events = 64;
    Rig rig{config, node_config};
    const uint16_t port{rig.api->Port()};
    TlsClient slow{port, TLS1_3_VERSION, TLS1_3_VERSION};
    BOOST_CHECK(slow.Stream("/v2/stream?channel=slots", "event: snapshot", std::chrono::milliseconds{3'000}).find("event: snapshot") != std::string::npos);
    // A receive buffer of 1 MiB: the socket then holds about 1.5 MB before the server holds events back, as on the CI
    // runners (run 44154355944, tsan), whatever this kernel's autotuning would give.
    slow.SetReceiveBuffer(1024 * 1024);
    // The reader stops reading; the node goes on sealing until the socket buffers are full and the hub's queue overflows.
    size_t most_queued{0};
    const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{30}};
    while (rig.api->Status().streams > 0 && std::chrono::steady_clock::now() < deadline) {
        for (int i{0}; i < 50; ++i) RequireOk(rig.node->SealSlot(), "SealSlot");
        rig.api->Notify();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        most_queued = std::max(most_queued, rig.api->Status().stream_output_bytes);
    }
    BOOST_CHECK_EQUAL(rig.api->Status().streams, 0U);
    // Bounded by the cap plus one round of events; the lag itself stayed in the hub.
    BOOST_CHECK_LE(most_queued, config.stream_output_bytes + 64 * 1024);
    // The backlog the socket holds comes first, then the gap; ReadMore returns at the gap, the limit only bounds a failure.
    BOOST_CHECK(slow.ReadMore(std::chrono::milliseconds{10'000}).find("event: gap") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_reader_that_leaves_with_events_queued_does_not_take_the_process_down)
{
    // A reader that stops reading and then leaves: its connection still holds events the server goes on writing. The
    // kernel answers that write with EPIPE and, unless the process ignores it, SIGPIPE, whose default action ends the
    // process. fm2d ignores SIGPIPE; the test runner has to as well, or one such write ends the whole run. Whether a
    // given write draws EPIPE (signal) or ECONNRESET (none) depends on timing, about one leaver in three on Linux, so
    // six leave in turn.
    HttpsSseConfig config;
    config.stream_output_bytes = 4 * 1024;
    SimNodeConfig node_config;
    node_config.streams.max_queued_events = 4'096; // no gap: the reader leaves first
    Rig rig{config, node_config};
    const uint16_t port{rig.api->Port()};
    for (int round{0}; round < 6; ++round) {
        {
            TlsClient leaver{port, TLS1_3_VERSION, TLS1_3_VERSION};
            BOOST_REQUIRE(leaver.Stream("/v2/stream?channel=slots", "event: snapshot", std::chrono::milliseconds{3'000}).find("event: snapshot") != std::string::npos);
            const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{30}};
            while (rig.api->Status().stream_output_bytes < config.stream_output_bytes && std::chrono::steady_clock::now() < deadline) {
                for (int i{0}; i < 50; ++i) RequireOk(rig.node->SealSlot(), "SealSlot");
                rig.api->Notify();
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
            BOOST_REQUIRE_GE(rig.api->Status().stream_output_bytes, config.stream_output_bytes);
        } // the reader leaves with its socket full: the kernel resets the connection
        for (int i{0}; i < 20; ++i) {
            RequireOk(rig.node->SealSlot(), "SealSlot");
            rig.api->Notify();
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{5}};
        while (rig.api->Status().streams > 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds{5});
        BOOST_REQUIRE_EQUAL(rig.api->Status().streams, 0U);
    }
    TlsClient later{port, TLS1_3_VERSION, TLS1_3_VERSION};
    BOOST_CHECK(later.Stream("/v2/stream?channel=slots", "event: snapshot", std::chrono::milliseconds{3'000}).find("event: snapshot") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_stalled_stream_on_a_quiet_hub_queues_no_heartbeats)
{
    // Review workflow of 2026-11-18: the pump held events back from a backpressured stream (R2) but still queued a
    // heartbeat every heartbeat_ms. With nothing new to send, the hub never overflowed into a gap, so a reader that
    // stopped reading on a quiet hub grew its connection's output buffer without bound (about 230 B/s here, measured
    // over 16 s), and the stream outlived the 10 s request timeout. A stream at its cap now gets no heartbeat either;
    // once its reader drains it, the heartbeats resume.
    HttpsSseConfig config;
    config.stream_output_bytes = 4 * 1024;
    SimNodeConfig node_config;
    node_config.streams.max_queued_events = 4'096; // no gap: the hub stays quiet once the sealing stops
    Rig rig{config, node_config};
    const uint16_t port{rig.api->Port()};
    TlsClient stalled{port, TLS1_3_VERSION, TLS1_3_VERSION};
    BOOST_REQUIRE(stalled.Stream("/v2/stream?channel=slots", "event: snapshot", std::chrono::milliseconds{3'000}).find("event: snapshot") != std::string::npos);
    // A small receive buffer, so the socket's buffers fill and the rest stays queued at the server.
    stalled.SetReceiveBuffer(4 * 1024);
    const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{60}};
    bool full{false};
    while (!full && std::chrono::steady_clock::now() < deadline) {
        for (int i{0}; i < 50; ++i) RequireOk(rig.node->SealSlot(), "SealSlot");
        rig.api->Notify();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        if (rig.api->Status().stream_output_bytes < config.stream_output_bytes) continue;
        // Still at the cap after a pause: the kernel takes no more.
        std::this_thread::sleep_for(std::chrono::milliseconds{500});
        full = rig.api->Status().stream_output_bytes >= config.stream_output_bytes;
    }
    BOOST_REQUIRE(full);
    const size_t queued{rig.api->Status().stream_output_bytes};
    // Two seconds of a quiet hub: about thirteen heartbeat periods, none of them queued.
    std::this_thread::sleep_for(std::chrono::seconds{2});
    BOOST_CHECK_LE(rig.api->Status().stream_output_bytes, queued);
    BOOST_CHECK_EQUAL(rig.api->Status().streams, 1U);
    // The reader reads again: the backlog arrives (two seconds of reading), then heartbeats as before.
    (void)stalled.ReadRawUntil("no such marker", std::chrono::milliseconds{2'000});
    BOOST_CHECK(stalled.ReadRawUntil("event: heartbeat", std::chrono::milliseconds{2'000}).find("event: heartbeat") != std::string::npos);
    BOOST_CHECK_EQUAL(rig.api->Status().streams, 1U);
}

BOOST_AUTO_TEST_CASE(stop_right_after_create_returns)
{
    // Review of 2026-11-16 (a reviewer's lead, confirmed): a Stop before the loop thread entered event_base_loop was lost,
    // because libevent clears a loopbreak on entry, and join never returned. Stop now asks the loop to stop from inside.
    Rig rig;
    for (int i{0}; i < 20; ++i) {
        HttpsSseConfig config;
        config.profile = rig.node->Profile();
        config.dev_self_signed = true;
        config.self_signed_dir = fs::PathToString(rig.dir);
        Result<std::unique_ptr<HttpsSseApi>> api{HttpsSseApi::Create(config, rig.node->Api())};
        RequireOk(api, "Create");
        const auto started{std::chrono::steady_clock::now()};
        (*api)->Stop();
        BOOST_CHECK_LT(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(), 2'000);
    }
}

BOOST_AUTO_TEST_SUITE_END()
