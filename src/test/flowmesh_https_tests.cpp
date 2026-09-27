// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_https.h>
#include <node/flowmesh_client.h>
#include <crypto/sha256.h>
#include <dbwrapper.h>
#include <flowmesh/production_wire.h>
#include <test/util/setup_common.h>
#include <util/fs.h>
#include <util/sock.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef WIN32
#include <csignal>
#endif

namespace node {
struct FlowMeshHttpsTestAccess {
    static bool ObserveRejectedCleanup(FlowMeshHttpsServer& server,
                                      std::function<void(size_t, size_t)> observer)
    {
        return server.SetRejectedCleanupObserverForTest(std::move(observer));
    }
    static bool ObserveConnection(FlowMeshHttpsServer& server, std::function<void(bool)> observer)
    {
        return server.SetConnectionObserverForTest(std::move(observer));
    }
};
} // namespace node

namespace {

constexpr size_t REJECTION_BYTE_BUDGET{1024 * 1024 + 8192};
constexpr std::string_view REJECTION_BODY{"{\"error\":\"invalid-http-request\"}"};

struct CleanupObservation {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::pair<size_t, size_t>> samples;

    void Record(size_t ciphertext, size_t plaintext)
    {
        {
            std::lock_guard lock{mutex};
            samples.emplace_back(ciphertext, plaintext);
        }
        condition.notify_all();
    }

    std::optional<std::pair<size_t, size_t>> Await(size_t count)
    {
        std::unique_lock lock{mutex};
        if (!condition.wait_for(lock, std::chrono::seconds{2}, [&] { return samples.size() >= count; })) return {};
        return samples.at(count - 1);
    }

    size_t Count()
    {
        std::lock_guard lock{mutex};
        return samples.size();
    }
};

struct ConnectionObservation {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<bool> samples;

    void Record(bool tcp_nodelay)
    {
        {
            std::lock_guard lock{mutex};
            samples.push_back(tcp_nodelay);
        }
        condition.notify_all();
    }

    std::optional<std::vector<bool>> Await(size_t count)
    {
        std::unique_lock lock{mutex};
        if (!condition.wait_for(lock, std::chrono::seconds{2}, [&] { return samples.size() >= count; })) return {};
        return samples;
    }

    size_t Count()
    {
        std::lock_guard lock{mutex};
        return samples.size();
    }
};

struct ScopedClientSignals {
#ifndef WIN32
    struct sigaction previous{};
    ScopedClientSignals()
    {
        // Node initialization installs this disposition in production. Match
        // it only for this isolated TLS test, restoring the prior test process.
        struct sigaction ignore{};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        BOOST_REQUIRE_EQUAL(sigaction(SIGPIPE, &ignore, &previous), 0);
    }
    ~ScopedClientSignals() { sigaction(SIGPIPE, &previous, nullptr); }
#endif
};

/** All TLS keys are generated solely for this isolated test directory. No
 * external wallet, production certificate, trust store or private key is read. */
struct HttpsFixture : BasicTestingSetup {
    ScopedClientSignals signals;
    fs::path cert;
    fs::path key;
    std::string pin;

    HttpsFixture() : BasicTestingSetup{ChainType::REGTEST}
    {
        cert = m_path_root / "https-test-cert.pem";
        key = m_path_root / "https-test-key.pem";
        CreateCertificate(cert, key, pin, true);
    }

    static void CreateCertificate(const fs::path& cert_path, const fs::path& key_path,
                                  std::string& digest_hex, bool include_ip)
    {
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator{
            EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free};
        BOOST_REQUIRE(generator);
        BOOST_REQUIRE_EQUAL(EVP_PKEY_keygen_init(generator.get()), 1);
        BOOST_REQUIRE_EQUAL(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(generator.get(), NID_X9_62_prime256v1), 1);
        EVP_PKEY* generated{nullptr};
        BOOST_REQUIRE_EQUAL(EVP_PKEY_keygen(generator.get(), &generated), 1);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> secret{generated, EVP_PKEY_free};
        std::unique_ptr<X509, decltype(&X509_free)> certificate{X509_new(), X509_free};
        BOOST_REQUIRE(certificate);
        BOOST_REQUIRE_EQUAL(X509_set_version(certificate.get(), 2), 1);
        BOOST_REQUIRE_EQUAL(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1), 1);
        BOOST_REQUIRE(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60));
        BOOST_REQUIRE(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600));
        BOOST_REQUIRE_EQUAL(X509_set_pubkey(certificate.get(), secret.get()), 1);
        X509_NAME* name{X509_get_subject_name(certificate.get())};
        BOOST_REQUIRE_EQUAL(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("FlowMesh isolated HTTPS test"), -1, -1, 0), 1);
        BOOST_REQUIRE_EQUAL(X509_set_issuer_name(certificate.get(), name), 1);
        X509V3_CTX extension_context;
        X509V3_set_ctx(&extension_context, certificate.get(), certificate.get(), nullptr, nullptr, 0);
        const auto add_extension = [&](int nid, const char* value) {
            std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension{
                X509V3_EXT_conf_nid(nullptr, &extension_context, nid, value), X509_EXTENSION_free};
            BOOST_REQUIRE(extension);
            BOOST_REQUIRE_EQUAL(X509_add_ext(certificate.get(), extension.get(), -1), 1);
        };
        add_extension(NID_basic_constraints, "critical,CA:TRUE");
        add_extension(NID_key_usage, "critical,digitalSignature,keyCertSign");
        add_extension(NID_ext_key_usage, "serverAuth");
        add_extension(NID_subject_alt_name, include_ip ? "DNS:localhost,IP:127.0.0.1" : "DNS:localhost");
        BOOST_REQUIRE(X509_sign(certificate.get(), secret.get(), EVP_sha256()) > 0);
        std::unique_ptr<FILE, decltype(&fclose)> cert_file{fsbridge::fopen(cert_path, "wb"), fclose};
        std::unique_ptr<FILE, decltype(&fclose)> key_file{fsbridge::fopen(key_path, "wb"), fclose};
        BOOST_REQUIRE(cert_file);
        BOOST_REQUIRE(key_file);
        BOOST_REQUIRE_EQUAL(PEM_write_X509(cert_file.get(), certificate.get()), 1);
        BOOST_REQUIRE_EQUAL(PEM_write_PrivateKey(key_file.get(), secret.get(), nullptr, nullptr, 0, nullptr, nullptr), 1);
        std::array<unsigned char, 32> digest{};
        unsigned int length{0};
        BOOST_REQUIRE_EQUAL(X509_digest(certificate.get(), EVP_sha256(), digest.data(), &length), 1);
        BOOST_REQUIRE_EQUAL(length, digest.size());
        digest_hex.clear();
        for (const auto byte : digest) {
            constexpr char HEX[]{"0123456789abcdef"};
            digest_hex += HEX[byte >> 4];
            digest_hex += HEX[byte & 15];
        }
    }

    node::FlowMeshHttpsServer::Options Options() const
    {
        node::FlowMeshHttpsServer::Options options;
        options.cert_file = cert;
        options.key_file = key;
        options.request_timeout = std::chrono::seconds{2};
        return options;
    }
    node::HttpsEndpoint Endpoint(const node::FlowMeshHttpsServer& server) const
    {
        return {"https://127.0.0.1:" + std::to_string(server.Port()), cert, pin};
    }
};

/** A generated-certificate peer which can deliberately separate headers from
 * body and keep the TLS connection open after observing the response. */
class SplitHttpsPeer {
    using Clock = std::chrono::steady_clock;
    const SOCKET m_fd{static_cast<SOCKET>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))};
    Sock m_socket{m_fd};
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> m_context{SSL_CTX_new(TLS_client_method()), SSL_CTX_free};
    std::unique_ptr<SSL, decltype(&SSL_free)> m_ssl{nullptr, SSL_free};
    size_t m_sessions_offered{0};

    bool Again(int result, Clock::time_point deadline)
    {
        const int error{SSL_get_error(m_ssl.get(), result)};
        if (Clock::now() >= deadline ||
            (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)) return false;
        return m_socket.Wait(std::chrono::milliseconds{10},
            error == SSL_ERROR_WANT_READ ? Sock::RECV : Sock::SEND);
    }

public:
    SplitHttpsPeer(uint16_t port, const fs::path& certificate, int max_version = 0)
    {
        BOOST_REQUIRE(m_fd != INVALID_SOCKET);
        BOOST_REQUIRE(m_context);
        if (max_version) BOOST_REQUIRE_EQUAL(SSL_CTX_set_max_proto_version(m_context.get(), max_version), 1);
        // Count resumable sessions the server offers (a ticket or a cacheable
        // session ID). Nothing is stored or offered back to the server.
        SSL_CTX_set_session_cache_mode(m_context.get(), SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
        BOOST_REQUIRE_EQUAL(SSL_CTX_set_app_data(m_context.get(), this), 1);
        SSL_CTX_sess_set_new_cb(m_context.get(), [](SSL* ssl, SSL_SESSION*) {
            ++static_cast<SplitHttpsPeer*>(SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl)))->m_sessions_offered;
            return 0;
        });
        SSL_CTX_set_verify(m_context.get(), SSL_VERIFY_PEER, nullptr);
        BOOST_REQUIRE_EQUAL(SSL_CTX_load_verify_locations(m_context.get(), fs::PathToString(certificate).c_str(), nullptr), 1);
        m_ssl.reset(SSL_new(m_context.get()));
        BOOST_REQUIRE(m_ssl);
        BOOST_REQUIRE_EQUAL(X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(m_ssl.get()), "127.0.0.1"), 1);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        BOOST_REQUIRE_EQUAL(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr), 1);
        BOOST_REQUIRE_EQUAL(m_socket.Connect(reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
        BOOST_REQUIRE(m_socket.SetNonBlocking());
        BOOST_REQUIRE_EQUAL(SSL_set_fd(m_ssl.get(), static_cast<int>(m_fd)), 1);
        const auto deadline{Clock::now() + std::chrono::seconds{2}};
        while (true) {
            ERR_clear_error();
            const int result{SSL_connect(m_ssl.get())};
            if (result == 1) break;
            BOOST_REQUIRE(Again(result, deadline));
        }
    }

    bool Send(std::string_view bytes)
    {
        const auto deadline{Clock::now() + std::chrono::seconds{2}};
        while (!bytes.empty()) {
            ERR_clear_error();
            const int result{SSL_write(m_ssl.get(), bytes.data(), static_cast<int>(bytes.size()))};
            if (result > 0) bytes.remove_prefix(result);
            else if (!Again(result, deadline)) return false;
        }
        return true;
    }

    bool SendHeaders(std::string_view length)
    {
        return Send("POST /flowmesh/v1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
            std::string{length} + "\r\nConnection: close\r\n\r\n");
    }

    bool SendRaw(std::string_view bytes)
    {
        // Deliberately corrupt/truncate *late* TLS input only after receiving
        // the rejection. This is not a second accepted application request.
        const auto deadline{Clock::now() + std::chrono::seconds{2}};
        while (!bytes.empty() && Clock::now() < deadline) {
            const auto sent{m_socket.Send(bytes.data(), bytes.size(), MSG_NOSIGNAL)};
            if (sent > 0) bytes.remove_prefix(sent);
            else {
                const int error{WSAGetLastError()};
                if (error != WSAEWOULDBLOCK && error != WSAEAGAIN && error != WSAEINTR) return false;
                if (!m_socket.Wait(std::chrono::milliseconds{10}, Sock::SEND)) return false;
            }
        }
        return bytes.empty();
    }

    void HalfCloseWithoutTlsNotify()
    {
#ifdef WIN32
        BOOST_REQUIRE_EQUAL(::shutdown(m_fd, SD_SEND), 0);
#else
        BOOST_REQUIRE_EQUAL(::shutdown(m_fd, SHUT_WR), 0);
#endif
    }

    std::string ReadResponse(size_t body_bytes)
    {
        std::string response;
        const auto deadline{Clock::now() + std::chrono::seconds{2}};
        std::array<char, 512> bytes{};
        while (response.size() < 4096) {
            ERR_clear_error();
            const int result{SSL_read(m_ssl.get(), bytes.data(), bytes.size())};
            if (result > 0) response.append(bytes.data(), result);
            else if (!Again(result, deadline)) break;
            const auto end{response.find("\r\n\r\n")};
            if (end != std::string::npos && response.size() >= end + 4 + body_bytes) break;
        }
        return response;
    }

    std::string ReadRejection() { return ReadResponse(REJECTION_BODY.size()); }
    size_t SessionsOffered() const { return m_sessions_offered; }
    int Version() const { return SSL_version(m_ssl.get()); }

    std::string ReadRecord()
    {
        // Without read-ahead or pipelining, one successful SSL_read returns
        // plaintext from exactly one TLS application-data record.
        std::string record(32 * 1024, '\0');
        const auto deadline{Clock::now() + std::chrono::seconds{2}};
        while (true) {
            ERR_clear_error();
            const int result{SSL_read(m_ssl.get(), record.data(), static_cast<int>(record.size()))};
            if (result > 0) {
                record.resize(result);
                return record;
            }
            if (!Again(result, deadline)) return {};
        }
    }

    void NotifyClose()
    {
        const auto deadline{Clock::now() + std::chrono::seconds{2}};
        while (true) {
            ERR_clear_error();
            const int result{SSL_shutdown(m_ssl.get())};
            if (result >= 0 || !Again(result, deadline)) return;
        }
    }

    bool AwaitSocketClose(std::chrono::milliseconds timeout)
    {
        // Called only after consuming the complete HTTP rejection. Discard
        // remaining ciphertext without TLS interpretation; no further request
        // or response is used on this deliberately non-cooperating connection.
        const auto deadline{Clock::now() + timeout};
        std::array<char, 4096> bytes{};
        while (Clock::now() < deadline) {
            const auto received{m_socket.Recv(bytes.data(), bytes.size(), 0)};
            if (received == 0) return true;
            if (received < 0) {
                const int error{WSAGetLastError()};
                if (error != WSAEWOULDBLOCK && error != WSAEAGAIN && error != WSAEINTR) return true;
            }
            if (!m_socket.Wait(std::chrono::milliseconds{5}, Sock::RECV)) return false;
        }
        return false;
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_https_tests, HttpsFixture)

BOOST_AUTO_TEST_CASE(https_two_requests_share_tls_and_idle_connection_releases_worker)
{
    auto options{Options()};
    options.worker_threads = 1;
    std::atomic<unsigned int> handled{0};
    node::FlowMeshHttpsServer server{options, [&](const auto&) {
        ++handled;
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    SplitHttpsPeer peer{server.Port(), cert};
    const std::string request{"POST /flowmesh/v1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\n{}"};
    BOOST_REQUIRE(peer.Send(request));
    const auto first{peer.ReadResponse(2)};
    BOOST_REQUIRE(first.starts_with("HTTP/1.1 200 "));
    BOOST_CHECK_MESSAGE(first.find("Connection: close") == std::string::npos,
                        "A complete valid request should keep its TLS connection reusable");
    // The warm peer is idle. A single server worker must still service a
    // different client rather than wait for this peer's next request.
    const auto other{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}",
        std::chrono::seconds{1}, 1024)};
    BOOST_REQUIRE_MESSAGE(other.response_received, other.error);
    BOOST_CHECK_EQUAL(other.status, 200);
    BOOST_REQUIRE(peer.Send(request));
    const auto second{peer.ReadResponse(2)};
    BOOST_CHECK_MESSAGE(second.starts_with("HTTP/1.1 200 "), second);
    BOOST_CHECK_EQUAL(handled.load(), 3U);
    server.Stop();
}

BOOST_AUTO_TEST_CASE(https_client_reuses_verified_connection_and_exact_request_bodies)
{
    std::atomic<unsigned int> handled{0};
    node::FlowMeshHttpsServer server{Options(), [&](const auto& request) {
        ++handled;
        return node::FlowMeshHttpsServer::Response{200, request.body};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    node::FlowMeshHttpsClient client;
    for (unsigned int i{0}; i < 4; ++i) {
        const auto body{std::string{"{\"request\":"} + std::to_string(i) + "}"};
        const auto result{client.Request(Endpoint(server), "/flowmesh/v1", body, std::chrono::seconds{2}, 1024)};
        BOOST_REQUIRE_MESSAGE(result.response_received, result.error);
        BOOST_CHECK_EQUAL(result.body, body);
        BOOST_CHECK_EQUAL(result.connection_reused, i != 0);
        BOOST_CHECK_EQUAL(result.tls_handshake_performed, i == 0);
        BOOST_CHECK(result.request_may_have_been_sent);
    }
    BOOST_CHECK_EQUAL(handled.load(), 4U);
    client.Reset();
    const auto reset{client.Request(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
    BOOST_REQUIRE_MESSAGE(reset.response_received, reset.error);
    BOOST_CHECK(!reset.connection_reused);
    BOOST_CHECK(reset.tls_handshake_performed);
}

BOOST_AUTO_TEST_CASE(https_warm_response_loss_is_unknown_without_hidden_replay)
{
    std::mutex mutex;
    std::condition_variable changed;
    bool release{false};
    std::atomic<unsigned int> submits{0};
    node::FlowMeshHttpsServer server{Options(), [&](const auto& request) {
        if (request.body == "signed-original") {
            ++submits;
            std::unique_lock lock{mutex};
            changed.wait_for(lock, std::chrono::seconds{2}, [&] { return release; });
        }
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    node::FlowMeshHttpsClient client;
    BOOST_REQUIRE(client.Request(Endpoint(server), "/flowmesh/v1", "warmup", std::chrono::seconds{2}, 1024).response_received);
    const auto lost{client.Request(Endpoint(server), "/flowmesh/v1", "signed-original", std::chrono::milliseconds{150}, 1024)};
    {
        std::lock_guard lock{mutex};
        release = true;
    }
    changed.notify_all();
    BOOST_CHECK(lost.connection_reused);
    BOOST_CHECK(!lost.tls_handshake_performed);
    BOOST_CHECK(!lost.response_received);
    BOOST_CHECK(lost.request_may_have_been_sent);
    BOOST_CHECK_EQUAL(lost.error, "https-deadline");
    BOOST_CHECK_EQUAL(submits.load(), 1U);
    const auto status{client.Request(Endpoint(server), "/flowmesh/v1", "status-only", std::chrono::seconds{2}, 1024)};
    BOOST_REQUIRE_MESSAGE(status.response_received, status.error);
    BOOST_CHECK(!status.connection_reused);
    BOOST_CHECK_EQUAL(submits.load(), 1U);
}

BOOST_AUTO_TEST_CASE(https_reuse_respects_close_legacy_and_request_limit)
{
    for (bool keep_alive : {false, true}) {
        BOOST_TEST_CONTEXT("keep-alive=" << keep_alive) {
            auto options{Options()};
            options.keep_alive = keep_alive;
            options.max_requests_per_connection = 2;
            node::FlowMeshHttpsServer server{options, [](const auto&) {
                return node::FlowMeshHttpsServer::Response{200, "{}"};
            }};
            std::string error;
            BOOST_REQUIRE_MESSAGE(server.Start(error), error);
            node::FlowMeshHttpsClient client;
            for (unsigned int i{0}; i < 3; ++i) {
                const auto reply{client.Request(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
                BOOST_REQUIRE_MESSAGE(reply.response_received, reply.error);
                BOOST_CHECK_EQUAL(reply.connection_reused, keep_alive && i == 1);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(https_warm_second_request_bounds_reject_before_dispatch)
{
    auto options{Options()}; options.max_request_bytes = 32;
    std::atomic<unsigned int> handled{0};
    node::FlowMeshHttpsServer server{options, [&](const auto&) {
        ++handled;
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    node::FlowMeshHttpsClient client;
    BOOST_REQUIRE(client.Request(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024).response_received);
    const auto rejected{client.Request(Endpoint(server), "/flowmesh/v1", std::string(64, 'x'), std::chrono::seconds{2}, 1024)};
    BOOST_REQUIRE_MESSAGE(rejected.response_received, rejected.error);
    BOOST_CHECK_EQUAL(rejected.status, 413);
    BOOST_CHECK(rejected.connection_reused);
    BOOST_CHECK_EQUAL(handled.load(), 1U);
    const auto next{client.Request(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
    BOOST_REQUIRE_MESSAGE(next.response_received, next.error);
    BOOST_CHECK(!next.connection_reused);
    BOOST_CHECK_EQUAL(handled.load(), 2U);
}

BOOST_AUTO_TEST_CASE(https_application_error_reply_keeps_verified_connection)
{
    std::atomic<unsigned int> handled{0};
    node::FlowMeshHttpsServer server{Options(), [&](const auto& request) {
        ++handled;
        if (request.body == "reject") return node::FlowMeshHttpsServer::Response{400, "{\"ok\":false}"};
        if (request.body == "throw") throw std::runtime_error{"handler failure"};
        if (request.body == "redirect") return node::FlowMeshHttpsServer::Response{302, "https://elsewhere.invalid"};
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    node::FlowMeshHttpsClient client;
    const auto rejected{client.Request(Endpoint(server), "/flowmesh/v1", "reject", std::chrono::seconds{2}, 1024)};
    BOOST_REQUIRE_MESSAGE(rejected.response_received, rejected.error);
    BOOST_CHECK_EQUAL(rejected.status, 400);
    BOOST_CHECK_EQUAL(rejected.body, "{\"ok\":false}");
    BOOST_CHECK(rejected.error.empty());
    BOOST_CHECK(!rejected.connection_reused);
    const auto failed{client.Request(Endpoint(server), "/flowmesh/v1", "throw", std::chrono::seconds{2}, 1024)};
    BOOST_REQUIRE_MESSAGE(failed.response_received, failed.error);
    BOOST_CHECK_EQUAL(failed.status, 500);
    BOOST_CHECK(failed.connection_reused);
    const auto next{client.Request(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
    BOOST_REQUIRE_MESSAGE(next.response_received, next.error);
    BOOST_CHECK_EQUAL(next.status, 200);
    BOOST_CHECK(next.connection_reused);
    BOOST_CHECK(!next.tls_handshake_performed);
    // A refused redirect is still an error and never keeps the connection.
    const auto redirect{client.Request(Endpoint(server), "/flowmesh/v1", "redirect", std::chrono::seconds{2}, 1024)};
    BOOST_CHECK_EQUAL(redirect.error, "https-redirect-refused");
    BOOST_CHECK(redirect.connection_reused);
    const auto after_redirect{client.Request(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
    BOOST_REQUIRE_MESSAGE(after_redirect.response_received, after_redirect.error);
    BOOST_CHECK(!after_redirect.connection_reused);
    BOOST_CHECK(after_redirect.tls_handshake_performed);
    BOOST_CHECK_EQUAL(handled.load(), 5U);
    server.Stop();
}

BOOST_AUTO_TEST_CASE(https_warm_pin_and_replaced_ca_revalidate_before_dispatch)
{
    std::atomic<unsigned int> handled{0};
    node::FlowMeshHttpsServer server{Options(), [&](const auto&) {
        ++handled;
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    node::FlowMeshHttpsClient client;
    const auto endpoint{Endpoint(server)};
    BOOST_REQUIRE(client.Request(endpoint, "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024).response_received);
    auto wrong_pin{endpoint}; wrong_pin.certificate_sha256 = std::string(64, '0');
    const auto rejected{client.Request(wrong_pin, "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
    BOOST_CHECK(!rejected.response_received);
    BOOST_CHECK(!rejected.request_may_have_been_sent);
    BOOST_CHECK_EQUAL(handled.load(), 1U);
    BOOST_REQUIRE(client.Request(endpoint, "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024).response_received);
    // Same CA path and restored timestamp, but a different generated CA. A
    // warm connection cannot silently keep authority from its earlier file.
    const auto previous_time{fs::last_write_time(cert)};
    std::string changed_pin;
    CreateCertificate(cert, m_path_root / "replacement-test-key.pem", changed_pin, true);
    fs::last_write_time(cert, previous_time);
    const auto changed_ca{client.Request(endpoint, "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
    BOOST_CHECK(!changed_ca.response_received);
    BOOST_CHECK(!changed_ca.request_may_have_been_sent);
    BOOST_CHECK_EQUAL(handled.load(), 2U);
}

BOOST_AUTO_TEST_CASE(https_warm_malformed_second_message_never_dispatches_surplus)
{
    std::atomic<unsigned int> handled{0};
    node::FlowMeshHttpsServer server{Options(), [&](const auto&) {
        ++handled;
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    SplitHttpsPeer peer{server.Port(), cert};
    const std::string valid{"POST /flowmesh/v1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n\r\n{}"};
    BOOST_REQUIRE(peer.Send(valid));
    BOOST_REQUIRE(peer.ReadResponse(2).starts_with("HTTP/1.1 200 "));
    const std::string invalid{"POST /flowmesh/v1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n{}"};
    BOOST_REQUIRE(peer.Send(invalid + valid));
    BOOST_CHECK(peer.ReadRejection().starts_with("HTTP/1.1 400 "));
    BOOST_CHECK_EQUAL(handled.load(), 1U);
    peer.NotifyClose();
    server.Stop();
}

BOOST_AUTO_TEST_CASE(https_idle_connections_consume_capacity_and_expire_without_workers)
{
    auto options{Options()};
    options.max_connections = 1;
    options.worker_threads = 1;
    options.idle_timeout = std::chrono::milliseconds{200};
    std::atomic<unsigned int> handled{0};
    node::FlowMeshHttpsServer server{options, [&](const auto&) {
        ++handled;
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    SplitHttpsPeer peer{server.Port(), cert};
    BOOST_REQUIRE(peer.Send("POST /flowmesh/v1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n\r\n{}"));
    BOOST_REQUIRE(peer.ReadResponse(2).starts_with("HTTP/1.1 200 "));
    const auto full{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{1}, 1024)};
    BOOST_CHECK(!full.response_received);
    BOOST_CHECK_EQUAL(handled.load(), 1U);
    BOOST_REQUIRE(peer.AwaitSocketClose(std::chrono::seconds{1}));
    const auto next{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{1}, 1024)};
    BOOST_REQUIRE_MESSAGE(next.response_received, next.error);
    BOOST_CHECK_EQUAL(handled.load(), 2U);
}

BOOST_AUTO_TEST_CASE(https_warm_partial_request_retains_absolute_deadline)
{
    auto options{Options()};
    options.request_timeout = std::chrono::milliseconds{200};
    options.idle_timeout = std::chrono::seconds{2};
    std::atomic<unsigned int> handled{0};
    node::FlowMeshHttpsServer server{options, [&](const auto&) {
        ++handled;
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    SplitHttpsPeer peer{server.Port(), cert};
    BOOST_REQUIRE(peer.Send("POST /flowmesh/v1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n\r\n{}"));
    BOOST_REQUIRE(peer.ReadResponse(2).starts_with("HTTP/1.1 200 "));
    BOOST_REQUIRE(peer.Send("P"));
    BOOST_CHECK(peer.AwaitSocketClose(std::chrono::seconds{1}));
    BOOST_CHECK_EQUAL(handled.load(), 1U);
}

BOOST_AUTO_TEST_CASE(https_idle_stop_closes_peers_and_same_server_reopens)
{
    auto options{Options()}; options.worker_threads = 1;
    node::FlowMeshHttpsServer server{options, [](const auto&) {
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    node::FlowMeshHttpsClient client;
    BOOST_REQUIRE(client.Request(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024).response_received);
    const auto start{std::chrono::steady_clock::now()};
    server.Stop();
    BOOST_CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    const auto next{client.Request(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
    BOOST_REQUIRE_MESSAGE(next.response_received, next.error);
    BOOST_CHECK(!next.connection_reused);
    BOOST_CHECK(next.tls_handshake_performed);
}

BOOST_AUTO_TEST_CASE(https_server_disables_nagle_on_accepted_connections)
{
    ConnectionObservation observed;
    node::FlowMeshHttpsServer server{Options(), [](const auto&) {
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    BOOST_REQUIRE(node::FlowMeshHttpsTestAccess::ObserveConnection(server,
        [&](bool tcp_nodelay) { observed.Record(tcp_nodelay); }));
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    BOOST_CHECK(!node::FlowMeshHttpsTestAccess::ObserveConnection(server, {}));
    SplitHttpsPeer peer{server.Port(), cert};
    BOOST_REQUIRE(peer.Send("POST /flowmesh/v1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n\r\n{}"));
    BOOST_REQUIRE(peer.ReadResponse(2).starts_with("HTTP/1.1 200 "));
    node::FlowMeshHttpsClient client;
    for (unsigned int i{0}; i < 3; ++i) {
        const auto reply{client.Request(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
        BOOST_REQUIRE_MESSAGE(reply.response_received, reply.error);
        BOOST_CHECK_EQUAL(reply.connection_reused, i != 0);
    }
    // One read-back per admitted connection; warm requests add none.
    const auto samples{observed.Await(2)};
    BOOST_REQUIRE(samples);
    BOOST_CHECK_EQUAL(samples->size(), 2U);
    for (const bool tcp_nodelay : *samples) BOOST_CHECK(tcp_nodelay);
    peer.NotifyClose();
    server.Stop();
    BOOST_CHECK_EQUAL(observed.Count(), 2U);
}

BOOST_AUTO_TEST_CASE(https_client_sets_nodelay_on_its_tls_sockets)
{
    // libevent creates the client socket; the flag reports that setsockopt
    // succeeded on the socket each request used. Ordering before ClientHello
    // follows from libevent attaching the socket before its handshake starts.
    node::FlowMeshHttpsServer server{Options(), [](const auto& request) {
        return node::FlowMeshHttpsServer::Response{200, request.body};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    node::FlowMeshHttpsClient client;
    for (unsigned int i{0}; i < 3; ++i) {
        const auto reply{client.Request(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
        BOOST_REQUIRE_MESSAGE(reply.response_received, reply.error);
        BOOST_CHECK_EQUAL(reply.connection_reused, i != 0);
        BOOST_CHECK(reply.tcp_nodelay);
    }
    client.Reset();
    const auto cold{client.Request(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
    BOOST_REQUIRE_MESSAGE(cold.response_received, cold.error);
    BOOST_CHECK(!cold.connection_reused);
    BOOST_CHECK(cold.tls_handshake_performed);
    BOOST_CHECK(cold.tcp_nodelay);
    const auto one_shot{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
    BOOST_REQUIRE_MESSAGE(one_shot.response_received, one_shot.error);
    BOOST_CHECK(one_shot.tcp_nodelay);
    server.Stop();
}

BOOST_AUTO_TEST_CASE(https_reply_head_and_small_body_share_one_tls_record)
{
    const std::string small(700, 's');
    std::string large(40000, '\0');
    for (size_t i{0}; i < large.size(); ++i) large[i] = static_cast<char>('a' + i % 26);
    node::FlowMeshHttpsServer server{Options(), [&](const auto& request) {
        return node::FlowMeshHttpsServer::Response{200, request.body == "small" ? small : large};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    const auto post = [](std::string_view body) {
        return "POST /flowmesh/v1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + std::string{body};
    };
    SplitHttpsPeer peer{server.Port(), cert};
    BOOST_REQUIRE(peer.Send(post("small")));
    const auto record{peer.ReadRecord()};
    BOOST_REQUIRE(record.starts_with("HTTP/1.1 200 "));
    const auto end{record.find("\r\n\r\n")};
    BOOST_REQUIRE(end != std::string::npos);
    BOOST_CHECK_EQUAL(record.size(), end + 4 + small.size());
    BOOST_CHECK(record.ends_with(small));
    // A large reply fills its first record, then continues in whole records
    // with the exact remaining body bytes.
    BOOST_REQUIRE(peer.Send(post("large")));
    std::string reply{peer.ReadRecord()};
    BOOST_CHECK_EQUAL(reply.size(), 16384U);
    BOOST_REQUIRE(reply.starts_with("HTTP/1.1 200 "));
    const auto body_start{reply.find("\r\n\r\n")};
    BOOST_REQUIRE(body_start != std::string::npos);
    while (reply.size() < body_start + 4 + large.size()) {
        const auto next{peer.ReadRecord()};
        BOOST_REQUIRE(!next.empty());
        BOOST_CHECK_LE(next.size(), 16384U);
        reply += next;
    }
    BOOST_CHECK_EQUAL(reply.size(), body_start + 4 + large.size());
    BOOST_CHECK(reply.substr(body_start + 4) == large);
    peer.NotifyClose();
    server.Stop();
}

BOOST_AUTO_TEST_CASE(https_reply_bodies_are_exact_across_first_record_boundary)
{
    auto options{Options()};
    options.max_request_bytes = 128 * 1024;
    node::FlowMeshHttpsServer server{options, [](const auto& request) {
        return node::FlowMeshHttpsServer::Response{200, request.body};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    // A keep-alive reply head is about 129 bytes, so the first record holds
    // about 16255 body bytes; cover both sides of that boundary.
    std::vector<size_t> sizes{0, 1};
    for (size_t size{16230}; size <= 16290; ++size) sizes.push_back(size);
    sizes.push_back(40000);
    sizes.push_back(100000);
    node::FlowMeshHttpsClient client;
    for (size_t i{0}; i < sizes.size(); ++i) {
        BOOST_TEST_CONTEXT("body bytes=" << sizes[i]) {
            std::string body(sizes[i], '\0');
            for (size_t j{0}; j < body.size(); ++j) body[j] = static_cast<char>('a' + (j * 7 + sizes[i]) % 26);
            const auto reply{client.Request(Endpoint(server), "/flowmesh/v1", body, std::chrono::seconds{2}, 256 * 1024)};
            BOOST_REQUIRE_MESSAGE(reply.response_received, reply.error);
            BOOST_CHECK_EQUAL(reply.status, 200);
            BOOST_CHECK_EQUAL(reply.body.size(), body.size());
            BOOST_CHECK(reply.body == body);
            BOOST_CHECK_EQUAL(reply.connection_reused, i != 0);
        }
    }
    server.Stop();
}

BOOST_AUTO_TEST_CASE(https_server_issues_no_resumption_tickets)
{
    node::FlowMeshHttpsServer server{Options(), [](const auto&) {
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    // The production client never resumes. The server offers neither TLS 1.3
    // tickets nor a TLS 1.2 ticket or cacheable session ID.
    for (const int version : {TLS1_3_VERSION, TLS1_2_VERSION}) {
        BOOST_TEST_CONTEXT("maximum TLS version=" << version) {
            SplitHttpsPeer peer{server.Port(), cert, version};
            BOOST_REQUIRE(peer.Send("POST /flowmesh/v1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n\r\n{}"));
            BOOST_REQUIRE(peer.ReadResponse(2).starts_with("HTTP/1.1 200 "));
            BOOST_CHECK_EQUAL(peer.Version(), version);
            BOOST_CHECK_EQUAL(peer.SessionsOffered(), 0U);
            peer.NotifyClose();
        }
    }
    server.Stop();
}

BOOST_AUTO_TEST_CASE(https_request_deadline_is_exposed_to_handler)
{
    auto options{Options()};
    options.request_timeout = std::chrono::milliseconds{1500};
    std::mutex mutex;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    node::FlowMeshHttpsServer server{options, [&](const auto& request) {
        std::lock_guard lock{mutex};
        deadline = request.deadline;
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    const auto before{std::chrono::steady_clock::now()};
    const auto reply{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
    const auto after{std::chrono::steady_clock::now()};
    BOOST_REQUIRE_MESSAGE(reply.response_received, reply.error);
    std::lock_guard lock{mutex};
    BOOST_REQUIRE(deadline);
    // Fixed at accept (a cold connection), never at handler entry.
    BOOST_CHECK(*deadline >= before + options.request_timeout);
    BOOST_CHECK(*deadline <= after + options.request_timeout);
    server.Stop();
}

BOOST_AUTO_TEST_CASE(https_start_and_stop_hooks_bracket_workers_and_release_a_waiting_handler)
{
    std::mutex mutex;
    std::condition_variable condition;
    bool released{false};
    std::atomic<unsigned> starts{0}, stops{0}, handled{0}, started_before_handler{0};
    auto options{Options()};
    options.request_timeout = std::chrono::seconds{5};
    options.on_start = [&] {
        {
            std::lock_guard lock{mutex};
            released = false;
        }
        ++starts;
    };
    options.on_stop = [&] {
        ++stops;
        {
            std::lock_guard lock{mutex};
            released = true;
        }
        condition.notify_all();
        throw std::runtime_error{"a throwing hook never escapes Stop"};
    };
    std::atomic<bool> entered{false};
    node::FlowMeshHttpsServer server{options, [&](const auto&) {
        started_before_handler += starts.load() != 0;
        ++handled;
        entered = true;
        std::unique_lock lock{mutex};
        // Only the stop hook releases this bounded wait before its 4 s bound.
        condition.wait_for(lock, std::chrono::seconds{4}, [&] { return released; });
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    server.Stop(); // Never started: no stop hook.
    BOOST_CHECK_EQUAL(stops.load(), 0U);
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    BOOST_CHECK_EQUAL(starts.load(), 1U);
    BOOST_REQUIRE_MESSAGE(server.Start(error), error); // Already running: no second hook.
    BOOST_CHECK_EQUAL(starts.load(), 1U);
    std::thread client{[&] {
        (void)node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{5}, 1024);
    }};
    const auto wait_until{std::chrono::steady_clock::now() + std::chrono::seconds{2}};
    while (!entered.load() && std::chrono::steady_clock::now() < wait_until) std::this_thread::sleep_for(std::chrono::milliseconds{1});
    BOOST_REQUIRE(entered.load());
    const auto stop_started{std::chrono::steady_clock::now()};
    server.Stop();
    BOOST_CHECK(std::chrono::steady_clock::now() - stop_started < std::chrono::seconds{1});
    client.join();
    BOOST_CHECK_EQUAL(stops.load(), 1U);
    server.Stop(); // Already stopped: no second hook.
    BOOST_CHECK_EQUAL(stops.load(), 1U);
    BOOST_CHECK_EQUAL(handled.load(), 1U);
    BOOST_CHECK_EQUAL(started_before_handler.load(), 1U);
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    BOOST_CHECK_EQUAL(starts.load(), 2U);
    server.Stop();
    BOOST_CHECK_EQUAL(stops.load(), 2U);
}

BOOST_AUTO_TEST_CASE(https_active_permits_bound_handshake_read_and_write)
{
    auto options{Options()};
    options.worker_threads = 8;
    options.active_permits = 2;
    options.request_timeout = std::chrono::seconds{3};
    std::atomic<unsigned> handled{0};
    node::FlowMeshHttpsServer server{options, [&](const auto&) {
        ++handled;
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    ConnectionObservation observed;
    BOOST_REQUIRE(node::FlowMeshHttpsTestAccess::ObserveConnection(server, [&](bool nodelay) { observed.Record(nodelay); }));
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    // Each peer completes its handshake, then holds its permit in the read.
    SplitHttpsPeer first{server.Port(), cert};
    SplitHttpsPeer second{server.Port(), cert};
    BOOST_REQUIRE(observed.Await(2));
    // Six workers are idle, but a third connection never begins its
    // handshake (the observer runs first, under the permit) until one frees.
    auto third{std::async(std::launch::async, [&] {
        return node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{3}, 1024);
    })};
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    BOOST_CHECK_EQUAL(observed.Count(), 2U);
    BOOST_CHECK_EQUAL(handled.load(), 0U);
    const std::string request{"POST /flowmesh/v1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n\r\n{}"};
    BOOST_REQUIRE(first.Send(request));
    BOOST_REQUIRE(first.ReadResponse(2).starts_with("HTTP/1.1 200 "));
    const auto reply{third.get()};
    BOOST_REQUIRE_MESSAGE(reply.response_received, reply.error);
    BOOST_CHECK_EQUAL(reply.status, 200);
    BOOST_CHECK_EQUAL(observed.Count(), 3U);
    BOOST_REQUIRE(second.Send(request));
    BOOST_REQUIRE(second.ReadResponse(2).starts_with("HTTP/1.1 200 "));
    BOOST_CHECK_EQUAL(handled.load(), 3U);
    server.Stop();
}

BOOST_AUTO_TEST_CASE(https_suspended_handler_releases_permit_and_resumes_first)
{
    using Clock = std::chrono::steady_clock;
    auto options{Options()};
    options.worker_threads = 3;
    options.active_permits = 1;
    options.request_timeout = std::chrono::seconds{4};
    std::mutex mutex;
    std::condition_variable condition;
    bool suspended{false}, release{false}, slow_entered{false};
    std::vector<std::string> order;
    const auto record = [&](std::string event) {
        std::lock_guard lock{mutex};
        order.push_back(std::move(event));
    };
    node::FlowMeshHttpsServer server{options, [&](const auto& request) {
        if (request.body == "wait") {
            request.suspend();
            request.suspend(); // Idempotent.
            std::unique_lock lock{mutex};
            suspended = true;
            condition.notify_all();
            condition.wait_for(lock, std::chrono::seconds{3}, [&] { return release; });
            lock.unlock();
            request.resume();
            request.resume(); // Idempotent.
            record("wait_resumed");
            return node::FlowMeshHttpsServer::Response{200, "waited"};
        }
        if (request.body == "slow") {
            {
                std::lock_guard lock{mutex};
                slow_entered = true;
            }
            condition.notify_all();
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
            record("slow_returned");
            return node::FlowMeshHttpsServer::Response{200, "slow"};
        }
        record("fast_entered");
        return node::FlowMeshHttpsServer::Response{200, "fast"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    const auto post = [&](std::string body) {
        return std::async(std::launch::async, [&, body] {
            return node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", body, std::chrono::seconds{4}, 1024);
        });
    };
    auto waiting{post("wait")};
    {
        std::unique_lock lock{mutex};
        BOOST_REQUIRE(condition.wait_for(lock, std::chrono::seconds{2}, [&] { return suspended; }));
    }
    // The only permit was released: another connection is served meanwhile.
    const auto fast_started{Clock::now()};
    const auto fast{post("fast").get()};
    BOOST_REQUIRE_MESSAGE(fast.response_received, fast.error);
    BOOST_CHECK_EQUAL(fast.body, "fast");
    BOOST_CHECK(Clock::now() - fast_started < std::chrono::seconds{1});

    // While a slow handler holds the permit, release the waiter and queue a
    // new connection: the resumed handler goes before the queued one.
    auto slow{post("slow")};
    {
        std::unique_lock lock{mutex};
        BOOST_REQUIRE(condition.wait_for(lock, std::chrono::seconds{2}, [&] { return slow_entered; }));
        release = true;
    }
    condition.notify_all();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto queued{post("fast")};
    const auto waited{waiting.get()};
    BOOST_REQUIRE_MESSAGE(waited.response_received, waited.error);
    BOOST_CHECK_EQUAL(waited.body, "waited");
    BOOST_CHECK_EQUAL(slow.get().body, "slow");
    BOOST_CHECK_EQUAL(queued.get().body, "fast");
    {
        std::lock_guard lock{mutex};
        const std::vector<std::string> expected{"fast_entered", "slow_returned", "wait_resumed", "fast_entered"};
        BOOST_CHECK_EQUAL_COLLECTIONS(order.begin(), order.end(), expected.begin(), expected.end());
    }
    server.Stop();
}

BOOST_AUTO_TEST_CASE(https_permits_survive_handlers_that_return_suspended)
{
    auto options{Options()};
    options.worker_threads = 2;
    options.active_permits = 3;
    std::string error;
    {
        node::FlowMeshHttpsServer invalid{options, [](const auto&) { return node::FlowMeshHttpsServer::Response{}; }};
        BOOST_CHECK(!invalid.Start(error));
        BOOST_CHECK_EQUAL(error, "https-server-invalid-options");
    }
    options.active_permits = 1;
    options.request_timeout = std::chrono::seconds{2};
    std::atomic<unsigned> handled{0};
    node::FlowMeshHttpsServer server{options, [&](const auto& request) {
        ++handled;
        request.suspend(); // Returns (or throws) suspended; the server resumes.
        if (request.body == "throw") throw std::runtime_error{"handler failure"};
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    node::FlowMeshHttpsClient client;
    for (const std::string body : {"{}", "throw", "{}", "throw", "{}"}) {
        const auto reply{client.Request(Endpoint(server), "/flowmesh/v1", body, std::chrono::seconds{2}, 1024)};
        BOOST_REQUIRE_MESSAGE(reply.response_received, reply.error);
        BOOST_CHECK_EQUAL(reply.status, body == "throw" ? 500 : 200);
    }
    client.Reset();
    // Still exactly one permit: a peer holding it in its read keeps a second
    // connection from starting, although the other worker is idle.
    SplitHttpsPeer holder{server.Port(), cert};
    const auto blocked{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::milliseconds{300}, 1024)};
    BOOST_CHECK(!blocked.response_received);
    BOOST_REQUIRE(holder.Send("POST /flowmesh/v1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}"));
    BOOST_REQUIRE(holder.ReadResponse(2).starts_with("HTTP/1.1 200 "));
    const auto next{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
    BOOST_REQUIRE_MESSAGE(next.response_received, next.error);
    BOOST_CHECK_EQUAL(handled.load(), 7U);
    server.Stop();
}

BOOST_AUTO_TEST_CASE(https_transport_only_paired_cold_warm_observations)
{
    // This measures generated loopback HTTPS exchanges, NOT matching, BFT,
    // durable certification, Qt display or WAN trading latency. No speed gate.
    node::FlowMeshHttpsServer server{Options(), [](const auto& request) {
        return node::FlowMeshHttpsServer::Response{200, request.body};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    node::FlowMeshHttpsClient warm;
    const auto endpoint{Endpoint(server)};
    const std::string body(128, 'x');
    BOOST_REQUIRE(warm.Request(endpoint, "/flowmesh/v1", body, std::chrono::seconds{2}, 1024).response_received);
    std::vector<double> cold_us, warm_us;
    for (unsigned int i{0}; i < 16; ++i) {
        const auto before{std::chrono::steady_clock::now()};
        const auto cold{node::FlowMeshHttpsRequest(endpoint, "/flowmesh/v1", body, std::chrono::seconds{2}, 1024)};
        const auto middle{std::chrono::steady_clock::now()};
        const auto retained{warm.Request(endpoint, "/flowmesh/v1", body, std::chrono::seconds{2}, 1024)};
        const auto after{std::chrono::steady_clock::now()};
        BOOST_REQUIRE_MESSAGE(cold.response_received, cold.error);
        BOOST_REQUIRE_MESSAGE(retained.response_received, retained.error);
        BOOST_CHECK_EQUAL(cold.body, body);
        BOOST_CHECK_EQUAL(retained.body, body);
        BOOST_CHECK(cold.tls_handshake_performed);
        BOOST_CHECK(retained.connection_reused);
        BOOST_CHECK(!retained.tls_handshake_performed);
        cold_us.push_back(std::chrono::duration<double, std::micro>(middle - before).count());
        warm_us.push_back(std::chrono::duration<double, std::micro>(after - middle).count());
    }
    // Emit only after the observations to avoid per-exchange logging overhead.
    for (unsigned int i{0}; i < cold_us.size(); ++i) {
        BOOST_TEST_MESSAGE("TRANSPORT_ONLY_SAMPLE index=" << i << " cold_us=" << cold_us[i] << " warm_us=" << warm_us[i]);
    }
    std::sort(cold_us.begin(), cold_us.end());
    std::sort(warm_us.begin(), warm_us.end());
    BOOST_TEST_MESSAGE("TRANSPORT_ONLY_MEDIAN samples=16 cold_us=" << (cold_us[7] + cold_us[8]) / 2
        << " warm_us=" << (warm_us[7] + warm_us[8]) / 2 << " cold_handshakes=16 warm_handshakes=0");
}

BOOST_AUTO_TEST_CASE(https_origin_normalization_preserves_trust_and_rejects_credentials)
{
    const std::vector<std::pair<std::string, std::string>> origins{
        {"https://EXAMPLE.org:443/flowmesh/v1", "https://example.org"},
        {"https://example.org/", "https://example.org"},
        {"https://127.0.0.1:5650/flowmesh/v1", "https://127.0.0.1:5650"},
        {"https://[0:0:0:0:0:0:0:1]:443/", "https://[::1]"},
    };
    for (const auto& [url, normalized] : origins) {
        node::HttpsEndpoint endpoint{url, cert, pin};
        std::string error;
        BOOST_REQUIRE_MESSAGE(node::NormalizeFlowMeshHttpsEndpoint(endpoint, error), error);
        BOOST_CHECK_EQUAL(endpoint.url, normalized);
        BOOST_CHECK(endpoint.ca_file == cert);
        BOOST_CHECK_EQUAL(endpoint.certificate_sha256, pin);
    }
    for (const std::string url : {"http://example.org", "https://user:secret@example.org", "https://example.org?token=secret",
                                 "https://example.org/#secret", "https://example.org/admin", "https://example.org:0",
                                 "https://example.org:65536", "https://example.org/\n"}) {
        node::HttpsEndpoint endpoint{url, cert, pin};
        std::string error;
        BOOST_CHECK(!node::NormalizeFlowMeshHttpsEndpoint(endpoint, error));
        BOOST_CHECK_EQUAL(endpoint.url, url);
    }
}

BOOST_AUTO_TEST_CASE(https_roundtrip_verifies_ca_ip_and_pin_and_preserves_exact_body)
{
    std::atomic<unsigned int> handled{0};
    std::atomic<bool> expected_request{false};
    node::FlowMeshHttpsServer server{Options(), [&](const node::FlowMeshHttpsServer::Request& request) {
        ++handled;
        expected_request = request.path == "/flowmesh/v1" && request.remote_address == "127.0.0.1";
        return node::FlowMeshHttpsServer::Response{202, request.body};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    BOOST_REQUIRE(server.Port() != 0);
    const std::string exact{"{\"signed_action\":\"001122aabb\",\"sequence\":17}"};
    const auto response{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", exact,
                                                  std::chrono::seconds{2}, 4096)};
    BOOST_CHECK_MESSAGE(response.response_received, response.error);
    BOOST_CHECK(response.request_may_have_been_sent);
    BOOST_CHECK_EQUAL(response.status, 202);
    BOOST_CHECK_EQUAL(response.body, exact);
    BOOST_CHECK(response.error.empty());
    BOOST_CHECK_EQUAL(handled.load(), 1U);
    BOOST_CHECK(expected_request.load());
    server.Stop();
    BOOST_CHECK_EQUAL(server.Port(), 0U);
}

BOOST_AUTO_TEST_CASE(https_wrong_ca_pin_and_hostname_never_reach_handler)
{
    std::atomic<unsigned int> handled{0};
    node::FlowMeshHttpsServer server{Options(), [&](const auto&) {
        ++handled;
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    const fs::path other_cert{m_path_root / "other-cert.pem"}, other_key{m_path_root / "other-key.pem"};
    std::string other_pin;
    CreateCertificate(other_cert, other_key, other_pin, true);
    auto wrong_ca{Endpoint(server)};
    wrong_ca.ca_file = other_cert;
    const auto ca_result{node::FlowMeshHttpsRequest(wrong_ca, "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
    BOOST_CHECK(!ca_result.response_received);
    BOOST_CHECK(!ca_result.request_may_have_been_sent);
    BOOST_CHECK_EQUAL(ca_result.error, "https-certificate-verification-failed");
    auto wrong_pin{Endpoint(server)};
    wrong_pin.certificate_sha256 = std::string(64, '0');
    const auto pin_result{node::FlowMeshHttpsRequest(wrong_pin, "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
    BOOST_CHECK(!pin_result.response_received);
    BOOST_CHECK(!pin_result.request_may_have_been_sent);
    BOOST_CHECK_EQUAL(pin_result.error, "https-certificate-pin-mismatch");
    BOOST_CHECK_EQUAL(handled.load(), 0U);
    server.Stop();

    CreateCertificate(cert, key, pin, false); // Trusted CA and pin, but no IP SAN for127.0.0.1.
    node::FlowMeshHttpsServer wrong_host{Options(), [&](const auto&) {
        ++handled;
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    BOOST_REQUIRE_MESSAGE(wrong_host.Start(error), error);
    const auto host_result{node::FlowMeshHttpsRequest(Endpoint(wrong_host), "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
    BOOST_CHECK(!host_result.response_received);
    BOOST_CHECK(!host_result.request_may_have_been_sent);
    BOOST_CHECK_EQUAL(host_result.error, "https-certificate-verification-failed");
    BOOST_CHECK_EQUAL(handled.load(), 0U);
}

BOOST_AUTO_TEST_CASE(https_bounds_and_redirect_do_not_retry_or_dispatch_other_paths)
{
    auto options{Options()};
    options.max_request_bytes = 32;
    std::atomic<unsigned int> handled{0};
    node::FlowMeshHttpsServer server{options, [&](const auto& request) {
        ++handled;
        if (request.body == "redirect") return node::FlowMeshHttpsServer::Response{302, "https://elsewhere.invalid"};
        return node::FlowMeshHttpsServer::Response{200, std::string(256, 'x')};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    auto endpoint{Endpoint(server)};
    const auto invalid_path{node::FlowMeshHttpsRequest(endpoint, "/wallet/BRIDGE_RELAYER", "{}", std::chrono::seconds{2}, 1024)};
    BOOST_CHECK(!invalid_path.response_received);
    BOOST_CHECK(!invalid_path.request_may_have_been_sent);
    endpoint.url.replace(0, 5, "http");
    const auto plaintext{node::FlowMeshHttpsRequest(endpoint, "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
    BOOST_CHECK(!plaintext.request_may_have_been_sent);
    BOOST_CHECK_EQUAL(handled.load(), 0U);
    const auto large_request{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", std::string(64, 'x'), std::chrono::seconds{2}, 1024)};
    BOOST_CHECK_MESSAGE(large_request.response_received, large_request.error);
    BOOST_CHECK_EQUAL(large_request.status, 413);
    BOOST_CHECK_EQUAL(handled.load(), 0U);
    const auto large_reply{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{2}, 32)};
    BOOST_CHECK(!large_reply.response_received);
    BOOST_CHECK(large_reply.request_may_have_been_sent);
    BOOST_CHECK_EQUAL(large_reply.error, "https-reply-too-large");
    const auto redirect{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "redirect", std::chrono::seconds{2}, 1024)};
    BOOST_CHECK(redirect.response_received);
    BOOST_CHECK_EQUAL(redirect.status, 302);
    BOOST_CHECK_EQUAL(redirect.error, "https-redirect-refused");
    BOOST_CHECK_EQUAL(handled.load(), 2U);
}

BOOST_AUTO_TEST_CASE(https_deadline_preserves_unknown_outcome_and_stop_joins_worker)
{
    std::mutex mutex;
    std::condition_variable condition;
    bool release{false};
    std::atomic<unsigned int> handled{0};
    node::FlowMeshHttpsServer server{Options(), [&](const auto&) {
        ++handled;
        std::unique_lock lock{mutex};
        condition.wait_for(lock, std::chrono::seconds{2}, [&] { return release; });
        return node::FlowMeshHttpsServer::Response{202, "{}"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    const auto start{std::chrono::steady_clock::now()};
    const auto timeout{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::milliseconds{150}, 1024)};
    const auto elapsed{std::chrono::steady_clock::now() - start};
    {
        std::lock_guard lock{mutex};
        release = true;
    }
    condition.notify_all();
    BOOST_CHECK(!timeout.response_received);
    BOOST_CHECK(timeout.request_may_have_been_sent);
    BOOST_CHECK_EQUAL(timeout.error, "https-deadline");
    BOOST_CHECK_EQUAL(handled.load(), 1U);
    BOOST_CHECK(elapsed < std::chrono::seconds{1});
    server.Stop();
    const auto stopped{node::FlowMeshHttpsRequest({"https://127.0.0.1:1", cert, pin}, "/flowmesh/v1", "{}", std::chrono::milliseconds{100}, 1024)};
    BOOST_CHECK(!stopped.response_received);
    BOOST_CHECK(!stopped.request_may_have_been_sent);
}

BOOST_AUTO_TEST_CASE(https_oversized_eager_requests_deliver_rejection_without_dispatch)
{
    auto options{Options()};
    options.max_request_bytes = 32;
    options.worker_threads = 1;
    std::atomic<unsigned int> handled{0};
    node::FlowMeshHttpsServer server{options, [&](const auto&) {
        ++handled;
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    // The HTTP client writes headers and body independently. Repetition keeps
    // the original close-with-unread-TLS-data race in the regression instead
    // of treating a single lucky 413 response as evidence of a repair.
    for (unsigned int attempt{0}; attempt < 32; ++attempt) {
        BOOST_TEST_CONTEXT("eager oversized request " << attempt) {
            const auto result{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1",
                std::string(64, 'x'), std::chrono::seconds{2}, 1024)};
            BOOST_CHECK_MESSAGE(result.response_received, result.error);
            BOOST_CHECK_EQUAL(result.status, 413);
            BOOST_CHECK_EQUAL(result.body, "{\"error\":\"invalid-http-request\"}");
            BOOST_CHECK_EQUAL(handled.load(), 0U);
        }
    }
    const auto healthy{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}",
        std::chrono::seconds{2}, 1024)};
    BOOST_CHECK_MESSAGE(healthy.response_received, healthy.error);
    BOOST_CHECK_EQUAL(healthy.status, 200);
    BOOST_CHECK_EQUAL(handled.load(), 1U);
    server.Stop();
}

BOOST_AUTO_TEST_CASE(https_split_rejection_is_immediate_and_surplus_is_never_dispatched)
{
    auto options{Options()};
    options.max_request_bytes = 32;
    options.worker_threads = 1;
    std::atomic<unsigned int> handled{0};
    node::FlowMeshHttpsServer server{options, [&](const auto&) {
        ++handled;
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    SplitHttpsPeer peer{server.Port(), cert};
    BOOST_REQUIRE(peer.SendHeaders("64"));
    // Observe complete rejection before releasing any body. Rejection is not
    // delayed to read the oversized body and does not consume handler budget.
    const auto rejection{peer.ReadRejection()};
    BOOST_REQUIRE(rejection.starts_with("HTTP/1.1 413 "));
    BOOST_CHECK(rejection.ends_with("\r\n\r\n{\"error\":\"invalid-http-request\"}"));
    BOOST_CHECK_EQUAL(handled.load(), 0U);
    // A malicious pipelined request in surplus input is discard-only, even if
    // it would otherwise be a legal request on its own connection.
    BOOST_CHECK(peer.Send(std::string(64, 'x') +
        "POST /flowmesh/v1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n\r\n{}"));
    peer.NotifyClose();
    const auto healthy{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}",
        std::chrono::seconds{2}, 1024)};
    BOOST_CHECK_MESSAGE(healthy.response_received, healthy.error);
    BOOST_CHECK_EQUAL(healthy.status, 200);
    BOOST_CHECK_EQUAL(handled.load(), 1U);
    server.Stop();
}

BOOST_AUTO_TEST_CASE(https_rejected_silent_peer_has_bounded_cleanup_and_stop)
{
    auto options{Options()};
    options.max_request_bytes = 32;
    options.worker_threads = 1;
    std::atomic<unsigned int> handled{0};
    node::FlowMeshHttpsServer server{options, [&](const auto&) {
        ++handled;
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    SplitHttpsPeer silent{server.Port(), cert};
    BOOST_REQUIRE(silent.SendHeaders("1073741824"));
    BOOST_REQUIRE(silent.ReadRejection().starts_with("HTTP/1.1 413 "));
    BOOST_CHECK_EQUAL(handled.load(), 0U);
    // Keep the first connection open without body or close-notify. The next
    // accepted connection must regain the sole worker without waiting for the
    // two-second request deadline or the advertised (untrusted) body length.
    const auto start{std::chrono::steady_clock::now()};
    const auto healthy{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}",
        std::chrono::seconds{2}, 1024)};
    const auto cleanup_elapsed{std::chrono::steady_clock::now() - start};
    BOOST_CHECK_MESSAGE(healthy.response_received, healthy.error);
    BOOST_CHECK_EQUAL(healthy.status, 200);
    BOOST_CHECK(cleanup_elapsed < std::chrono::seconds{1});
    BOOST_CHECK_EQUAL(handled.load(), 1U);

    SplitHttpsPeer stopped{server.Port(), cert};
    BOOST_REQUIRE(stopped.SendHeaders("64"));
    BOOST_REQUIRE(stopped.ReadRejection().starts_with("HTTP/1.1 413 "));
    const auto stop_start{std::chrono::steady_clock::now()};
    server.Stop();
    BOOST_CHECK(std::chrono::steady_clock::now() - stop_start < std::chrono::seconds{1});
    BOOST_CHECK_EQUAL(server.Port(), 0U);
    BOOST_CHECK_EQUAL(handled.load(), 1U);
}

BOOST_AUTO_TEST_CASE(https_rejection_cleanup_does_not_extend_original_deadline)
{
    auto options{Options()};
    options.max_request_bytes = 32;
    options.request_timeout = std::chrono::milliseconds{100};
    std::atomic<unsigned int> handled{0};
    node::FlowMeshHttpsServer server{options, [&](const auto&) {
        ++handled;
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    SplitHttpsPeer silent{server.Port(), cert};
    BOOST_REQUIRE(silent.SendHeaders("64"));
    BOOST_REQUIRE(silent.ReadRejection().starts_with("HTTP/1.1 413 "));
    // The 250 ms rejection cleanup allowance must not replace the earlier
    // 100 ms deadline recorded when this socket was accepted. Allow scheduling
    // slack, but less than a fresh cleanup allowance.
    BOOST_CHECK(silent.AwaitSocketClose(std::chrono::milliseconds{200}));
    BOOST_CHECK_EQUAL(handled.load(), 0U);
    const auto healthy{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}",
        std::chrono::seconds{2}, 1024)};
    BOOST_CHECK_MESSAGE(healthy.response_received, healthy.error);
    BOOST_CHECK_EQUAL(healthy.status, 200);
    BOOST_CHECK_EQUAL(handled.load(), 1U);
    server.Stop();
}

BOOST_AUTO_TEST_CASE(https_cleanup_exact_ciphertext_and_plaintext_budget_boundaries)
{
    // Two deliberately different TLS layouts select the independent limits.
    // Header-only first record leaves no buffered application plaintext. A
    // full first record leaves OpenSSL buffered plaintext after ReadRequest's
    // 4 KiB read, allowing the plaintext cap to bind before the ciphertext cap.
    for (const bool buffered_plaintext : {false, true}) {
        BOOST_TEST_CONTEXT("buffered plaintext=" << buffered_plaintext) {
            auto options{Options()};
            options.max_request_bytes = 32;
            options.worker_threads = 1;
            CleanupObservation observed;
            std::atomic<unsigned int> handled{0};
            node::FlowMeshHttpsServer server{options, [&](const auto&) {
                ++handled;
                return node::FlowMeshHttpsServer::Response{200, "{}"};
            }};
            BOOST_REQUIRE(node::FlowMeshHttpsTestAccess::ObserveRejectedCleanup(server,
                [&](size_t cipher, size_t plain) { observed.Record(cipher, plain); }));
            std::string error;
            BOOST_REQUIRE_MESSAGE(server.Start(error), error);
            BOOST_CHECK(!node::FlowMeshHttpsTestAccess::ObserveRejectedCleanup(server, {}));
            SplitHttpsPeer peer{server.Port(), cert};
            std::string first{"POST /flowmesh/v1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4194304\r\n\r\n"};
            if (buffered_plaintext) first.resize(16384, 'x');
            BOOST_REQUIRE(peer.Send(first));
            const auto rejection{peer.ReadRejection()};
            BOOST_REQUIRE(rejection.starts_with("HTTP/1.1 413 "));
            BOOST_REQUIRE(rejection.ends_with(std::string{"\r\n\r\n"} + std::string{REJECTION_BODY}));
            BOOST_CHECK_EQUAL(handled.load(), 0U);
            const std::string pipeline{"POST /flowmesh/v1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n\r\n{}"};
            const std::string surplus{pipeline + std::string(2 * REJECTION_BYTE_BUDGET, 'x') + pipeline};
            // Closure may interrupt this hostile surplus writer. Delivery of
            // surplus is not required; the exact *server* counters are.
            const auto start{std::chrono::steady_clock::now()};
            (void)peer.Send(surplus);
            const auto sample{observed.Await(1)};
            BOOST_REQUIRE(sample);
            const auto [ciphertext, plaintext]{*sample};
            BOOST_TEST_MESSAGE("cleanup boundary buffered=" << buffered_plaintext
                << " ciphertext=" << ciphertext << " plaintext=" << plaintext);
            BOOST_CHECK_LE(ciphertext, REJECTION_BYTE_BUDGET);
            BOOST_CHECK_LE(plaintext, REJECTION_BYTE_BUDGET);
            if (buffered_plaintext) {
                BOOST_CHECK_EQUAL(plaintext, REJECTION_BYTE_BUDGET);
                BOOST_CHECK_LT(ciphertext, REJECTION_BYTE_BUDGET);
            } else {
                BOOST_CHECK_EQUAL(ciphertext, REJECTION_BYTE_BUDGET);
                BOOST_CHECK_LT(plaintext, REJECTION_BYTE_BUDGET);
            }
            BOOST_CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
            BOOST_CHECK(peer.AwaitSocketClose(std::chrono::milliseconds{500}));
            BOOST_CHECK_EQUAL(handled.load(), 0U);
            const auto healthy{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}",
                std::chrono::seconds{2}, 1024)};
            BOOST_REQUIRE_MESSAGE(healthy.response_received, healthy.error);
            BOOST_CHECK_EQUAL(healthy.status, 200);
            BOOST_CHECK_EQUAL(handled.load(), 1U);
            server.Stop();
        }
    }
}

BOOST_AUTO_TEST_CASE(https_cleanup_malformed_and_truncated_late_tls)
{
    for (const bool truncated : {false, true}) {
        BOOST_TEST_CONTEXT("truncated TLS=" << truncated) {
            auto options{Options()};
            options.max_request_bytes = 32;
            options.worker_threads = 1;
            CleanupObservation observed;
            std::atomic<unsigned int> handled{0};
            node::FlowMeshHttpsServer server{options, [&](const auto&) {
                ++handled;
                return node::FlowMeshHttpsServer::Response{200, "{}"};
            }};
            BOOST_REQUIRE(node::FlowMeshHttpsTestAccess::ObserveRejectedCleanup(server,
                [&](size_t cipher, size_t plain) { observed.Record(cipher, plain); }));
            std::string error;
            BOOST_REQUIRE_MESSAGE(server.Start(error), error);
            SplitHttpsPeer peer{server.Port(), cert};
            BOOST_REQUIRE(peer.SendHeaders("4194304"));
            const auto rejection{peer.ReadRejection()};
            BOOST_REQUIRE(rejection.starts_with("HTTP/1.1 413 "));
            BOOST_REQUIRE(rejection.ends_with(std::string{"\r\n\r\n"} + std::string{REJECTION_BODY}));
            const std::string malformed{truncated ? std::string{"\x17\x03\x03\x00\x40short", 10}
                                                 : std::string{"\xff\x03\x03\x00\x01x", 6}};
            const auto start{std::chrono::steady_clock::now()};
            BOOST_REQUIRE(peer.SendRaw(malformed));
            if (truncated) peer.HalfCloseWithoutTlsNotify();
            const auto sample{observed.Await(1)};
            BOOST_REQUIRE(sample);
            BOOST_TEST_MESSAGE("late TLS truncated=" << truncated << " ciphertext=" << sample->first
                << " plaintext=" << sample->second);
            BOOST_CHECK_GT(sample->first, 0U);
            BOOST_CHECK_LE(sample->first, malformed.size());
            BOOST_CHECK_EQUAL(sample->second, 0U);
            BOOST_CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
            BOOST_CHECK(peer.AwaitSocketClose(std::chrono::milliseconds{500}));
            BOOST_CHECK_EQUAL(handled.load(), 0U);
            const auto healthy{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}",
                std::chrono::seconds{2}, 1024)};
            BOOST_REQUIRE_MESSAGE(healthy.response_received, healthy.error);
            BOOST_CHECK_EQUAL(healthy.status, 200);
            BOOST_CHECK_EQUAL(handled.load(), 1U);
            server.Stop();
        }
    }
}

BOOST_AUTO_TEST_CASE(https_cleanup_slow_and_refusing_peers_release_single_worker)
{
    for (const bool slow : {false, true}) {
        BOOST_TEST_CONTEXT("slow peer=" << slow) {
            auto options{Options()};
            options.max_request_bytes = 32;
            options.worker_threads = 1;
            CleanupObservation observed;
            std::atomic<unsigned int> handled{0};
            node::FlowMeshHttpsServer server{options, [&](const auto&) {
                ++handled;
                return node::FlowMeshHttpsServer::Response{200, "{}"};
            }};
            BOOST_REQUIRE(node::FlowMeshHttpsTestAccess::ObserveRejectedCleanup(server,
                [&](size_t cipher, size_t plain) { observed.Record(cipher, plain); }));
            std::string error;
            BOOST_REQUIRE_MESSAGE(server.Start(error), error);
            SplitHttpsPeer peer{server.Port(), cert};
            BOOST_REQUIRE(peer.SendHeaders("4194304"));
            const auto start{std::chrono::steady_clock::now()};
            if (slow) {
                const auto rejection{peer.ReadRejection()};
                BOOST_REQUIRE(rejection.starts_with("HTTP/1.1 413 "));
                BOOST_REQUIRE(rejection.ends_with(std::string{"\r\n\r\n"} + std::string{REJECTION_BODY}));
                // Intentional workload pacing, not a sleep used to obtain a
                // pass: six one-byte TLS writes at an explicit 40 ms cadence.
                // The peer then refuses close-notify or the advertised body.
                for (unsigned int tick{0}; tick < 6; ++tick) {
                    std::this_thread::sleep_until(start + std::chrono::milliseconds{40 * tick});
                    BOOST_REQUIRE(peer.Send("x"));
                }
            }
            // The refusing case never reads the HTTP/TLS response. Neither
            // peer may extend cleanup to the two-second request deadline.
            const auto sample{observed.Await(1)};
            BOOST_REQUIRE(sample);
            BOOST_CHECK_LE(sample->first, REJECTION_BYTE_BUDGET);
            BOOST_CHECK_EQUAL(sample->second, slow ? 6U : 0U);
            BOOST_CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
            BOOST_CHECK(peer.AwaitSocketClose(std::chrono::milliseconds{500}));
            BOOST_CHECK_EQUAL(handled.load(), 0U);
            const auto healthy{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}",
                std::chrono::seconds{2}, 1024)};
            BOOST_REQUIRE_MESSAGE(healthy.response_received, healthy.error);
            BOOST_CHECK_EQUAL(healthy.status, 200);
            BOOST_CHECK_EQUAL(handled.load(), 1U);
            server.Stop();
        }
    }
}

BOOST_AUTO_TEST_CASE(https_cleanup_stop_observes_inflight_rejection_then_restarts)
{
    auto options{Options()};
    options.max_request_bytes = 32;
    options.worker_threads = 1;
    CleanupObservation observed;
    std::atomic<unsigned int> handled{0};
    node::FlowMeshHttpsServer server{options, [&](const auto&) {
        ++handled;
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    BOOST_REQUIRE(node::FlowMeshHttpsTestAccess::ObserveRejectedCleanup(server,
        [&](size_t cipher, size_t plain) { observed.Record(cipher, plain); }));
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    SplitHttpsPeer peer{server.Port(), cert};
    BOOST_REQUIRE(peer.SendHeaders("4194304"));
    const auto rejection{peer.ReadRejection()};
    BOOST_REQUIRE(rejection.starts_with("HTTP/1.1 413 "));
    BOOST_REQUIRE(rejection.ends_with(std::string{"\r\n\r\n"} + std::string{REJECTION_BODY}));
    BOOST_REQUIRE_EQUAL(observed.Count(), 0U);
    const auto start{std::chrono::steady_clock::now()};
    server.Stop();
    BOOST_CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
    BOOST_CHECK_EQUAL(server.Port(), 0U);
    const auto sample{observed.Await(1)};
    BOOST_REQUIRE(sample);
    BOOST_CHECK_EQUAL(sample->first, 0U);
    BOOST_CHECK_EQUAL(sample->second, 0U);
    BOOST_CHECK_EQUAL(handled.load(), 0U);
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    const auto healthy{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}",
        std::chrono::seconds{2}, 1024)};
    BOOST_REQUIRE_MESSAGE(healthy.response_received, healthy.error);
    BOOST_CHECK_EQUAL(healthy.status, 200);
    BOOST_CHECK_EQUAL(handled.load(), 1U);
    server.Stop();
}

BOOST_AUTO_TEST_SUITE_END()

namespace {
struct ClientConnectFixture : TestingSetup {
    ScopedClientSignals signals;
    fs::path cert{m_path_root / "client-cert.pem"};
    fs::path key{m_path_root / "client-key.pem"};
    std::string pin;

    ClientConnectFixture() : TestingSetup{ChainType::REGTEST}
    {
        HttpsFixture::CreateCertificate(cert, key, pin, true);
    }
    node::FlowMeshHttpsServer::Options Options() const
    {
        node::FlowMeshHttpsServer::Options options;
        options.cert_file = cert; options.key_file = key;
        options.request_timeout = std::chrono::seconds{2};
        return options;
    }
    node::HttpsEndpoint Endpoint(const node::FlowMeshHttpsServer& server) const
    {
        return {"https://127.0.0.1:" + std::to_string(server.Port()), cert, pin};
    }
    //! A restart-restored outbox holding one retained public instruction.
    static flowmesh::Action SeedRetainedAction(const fs::path& path, const uint256& market, const uint256& owner)
    {
        flowmesh::Action action;
        action.signer = owner;
        action.sequence = 3;
        action.type = static_cast<uint8_t>(flowmesh::ActionType::CANCEL_BID);
        action.credential = {1, 2, 3};
        const auto bytes{flowmesh::EncodeProductionActionPayload(action)};
        BOOST_REQUIRE(bytes);
        UniValue row{UniValue::VOBJ};
        row.pushKV("market_id", market.GetHex());
        row.pushKV("domain", uint256::ONE.GetHex());
        row.pushKV("config", uint256::ONE.GetHex());
        row.pushKV("action_hex", HexStr(*bytes));
        row.pushKV("action_id", action.Id().GetHex());
        row.pushKV("initial_submission_ms", 1);
        row.pushKV("may_have_been_sent", true);
        row.pushKV("previously_certified", false);
        row.pushKV("owner_account", owner.GetHex());
        UniValue actions{UniValue::VARR};
        actions.push_back(std::move(row));
        UniValue root{UniValue::VOBJ};
        root.pushKV("version", 1);
        root.pushKV("actions", std::move(actions));
        root.pushKV("heads", UniValue{UniValue::VARR});
        CDBWrapper db{DBParams{.path=path, .cache_bytes=1 << 20}};
        db.Write(std::string{"public-client-v1"}, root.write(), true);
        return action;
    }
};

std::string Sha256Hex(const std::vector<unsigned char>& bytes)
{
    unsigned char digest[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(bytes.data(), bytes.size()).Finalize(digest);
    return HexStr(digest);
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_client_connect_tests, ClientConnectFixture)

BOOST_AUTO_TEST_CASE(client_read_failover_distinguishes_https_from_application_readiness)
{
    std::atomic<unsigned int> first_requests{0}, second_requests{0};
    node::FlowMeshHttpsServer first{Options(), [&](const auto&) {
        ++first_requests;
        return node::FlowMeshHttpsServer::Response{200, R"({"ok":false,"error":"Market has no certified head yet","result":null})"};
    }};
    node::FlowMeshHttpsServer second{Options(), [&](const auto&) {
        ++second_requests;
        return node::FlowMeshHttpsServer::Response{200, R"({"ok":true,"result":[],"error":""})"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(first.Start(error), error);
    BOOST_REQUIRE_MESSAGE(second.Start(error), error);
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(first), Endpoint(second)}, m_path_root / "client", error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_CHECK(client->Markets(std::nullopt).empty());
    auto status{client->Status()};
    BOOST_REQUIRE_EQUAL(status.endpoints.size(), 2U);
    BOOST_CHECK(status.endpoints[0].transport_available);
    BOOST_CHECK(!status.endpoints[0].available);
    BOOST_CHECK_EQUAL(status.endpoints[0].retry_after_ms, 0);
    BOOST_CHECK_EQUAL(status.endpoints[0].consecutive_failures, 0U);
    BOOST_CHECK_EQUAL(status.endpoints[0].last_error, "Market has no certified head yet");
    BOOST_CHECK(status.endpoints[1].available);
    BOOST_CHECK_EQUAL(status.active_endpoint, Endpoint(second).url);
    // Another explicit probe must reach the semantically unavailable server
    // immediately; its error must not trigger a transport cooldown.
    BOOST_REQUIRE_MESSAGE(client->Connect(Endpoint(first).url, error), error);
    BOOST_CHECK_EQUAL(first_requests.load(), 2U);
    BOOST_CHECK_EQUAL(second_requests.load(), 2U);
    BOOST_CHECK(!client->Status().engine_enabled);
    BOOST_CHECK_EQUAL(client->Status().pending_actions, 0U);
}

BOOST_AUTO_TEST_CASE(client_connect_persists_public_origins_without_changing_outbox_or_trust)
{
    std::atomic<unsigned int> requests{0};
    std::atomic<bool> read_only{true};
    const auto handler = [&](const node::FlowMeshHttpsServer::Request& request) {
        ++requests;
        UniValue json;
        if (!json.read(request.body) || json["method"].get_str() != "markets") read_only = false;
        return node::FlowMeshHttpsServer::Response{200, R"({"ok":true,"result":[],"error":""})"};
    };
    node::FlowMeshHttpsServer first{Options(), handler}, second{Options(), handler};
    std::string error;
    BOOST_REQUIRE_MESSAGE(first.Start(error), error);
    BOOST_REQUIRE_MESSAGE(second.Start(error), error);
    const fs::path path{m_path_root / "client"};
    const std::string journal{R"({ "version":1, "actions":[], "heads":[] })"};
    {
        CDBWrapper db{DBParams{.path=path, .cache_bytes=1 << 20}};
        db.Write(std::string{"public-client-v1"}, journal, true);
    }
    {
        auto configured{Endpoint(first)};
        configured.url += "/flowmesh/v1";
        auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {configured}, path, error)};
        BOOST_REQUIRE_MESSAGE(client, error);
        BOOST_REQUIRE_MESSAGE(client->Connect(Endpoint(first).url + "/", error), error);
        BOOST_REQUIRE_EQUAL(client->Status().endpoints.size(), 1U);
        // Successful connection proves both custom CA and pin survived URL
        // normalization and selecting the existing endpoint.
        BOOST_CHECK(client->Status().endpoints[0].available);
        BOOST_REQUIRE_MESSAGE(client->Connect(Endpoint(second).url, error), error);
        const auto status{client->Status()};
        BOOST_REQUIRE_EQUAL(status.endpoints.size(), 2U);
        BOOST_CHECK(!status.endpoints[1].transport_available);
        BOOST_CHECK_EQUAL(status.endpoints[1].consecutive_failures, 1U);
        BOOST_CHECK(status.endpoints[1].retry_after_ms > status.endpoints[1].last_attempt_ms);
        BOOST_CHECK_EQUAL(status.endpoints[1].last_error, "https-certificate-verification-failed");
        BOOST_CHECK_EQUAL(status.pending_actions, 0U);
        BOOST_CHECK(!status.engine_enabled);
        BOOST_CHECK(read_only.load());
    }
    {
        CDBWrapper db{DBParams{.path=path, .cache_bytes=1 << 20}};
        std::string retained, saved;
        BOOST_REQUIRE(db.Read(std::string{"public-client-v1"}, retained));
        BOOST_CHECK_EQUAL(retained, journal);
        BOOST_REQUIRE(db.Read(std::string{"public-client-endpoints-v1"}, saved));
        UniValue config;
        BOOST_REQUIRE(config.read(saved));
        BOOST_REQUIRE_EQUAL(config["urls"].size(), 1U);
        BOOST_CHECK_EQUAL(config["urls"][0].get_str(), Endpoint(second).url);
        BOOST_CHECK(saved.find(pin) == std::string::npos);
        BOOST_CHECK(saved.find(fs::PathToString(cert)) == std::string::npos);
    }
    {
        // A runtime origin reappears after restart, while explicit startup
        // CA/pin configuration takes precedence over its default trust.
        auto configured{Endpoint(second)};
        configured.url += "/flowmesh/v1";
        auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {configured}, path, error)};
        BOOST_REQUIRE_MESSAGE(client, error);
        BOOST_REQUIRE_EQUAL(client->Status().endpoints.size(), 1U);
        BOOST_REQUIRE_MESSAGE(client->Connect(Endpoint(second).url, error), error);
        BOOST_CHECK(client->Status().endpoints[0].available);
        BOOST_CHECK(client->Status().endpoints[0].transport_available);
        BOOST_CHECK_EQUAL(client->Status().endpoints[0].retry_after_ms, 0);
        BOOST_CHECK_EQUAL(client->Status().selected_endpoint, Endpoint(second).url);
    }
    {
        auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {}, path, error)};
        BOOST_REQUIRE_MESSAGE(client, error);
        BOOST_REQUIRE_EQUAL(client->Status().endpoints.size(), 1U);
        BOOST_CHECK_EQUAL(client->Status().endpoints[0].url, Endpoint(second).url);
        BOOST_CHECK(!client->Status().endpoints[0].transport_available);
        BOOST_CHECK_EQUAL(client->Status().endpoints[0].last_attempt_ms, 0);
    }
    CDBWrapper db{DBParams{.path=path, .cache_bytes=1 << 20}};
    std::string retained;
    BOOST_REQUIRE(db.Read(std::string{"public-client-v1"}, retained));
    BOOST_CHECK_EQUAL(retained, journal);
    BOOST_CHECK(read_only.load());
    BOOST_CHECK_EQUAL(requests.load(), 3U);
}

BOOST_AUTO_TEST_CASE(client_selected_endpoint_recovers_while_fallback_stays_healthy)
{
    std::atomic<unsigned int> preferred_requests{0}, fallback_requests{0};
    node::FlowMeshHttpsServer reserve{Options(), [&](const auto&) {
        return node::FlowMeshHttpsServer::Response{200, R"({"ok":true,"result":[],"error":""})"};
    }};
    node::FlowMeshHttpsServer fallback{Options(), [&](const auto&) {
        ++fallback_requests;
        return node::FlowMeshHttpsServer::Response{200, R"({"ok":true,"result":[],"error":""})"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(reserve.Start(error), error);
    BOOST_REQUIRE_MESSAGE(fallback.Start(error), error);
    const auto preferred{Endpoint(reserve)};
    auto options{Options()};
    options.port = reserve.Port();
    reserve.Stop();

    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {preferred, Endpoint(fallback)}, m_path_root / "client", error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_MESSAGE(client->Connect(preferred.url, error), error);
    const auto failed_over{client->Status()};
    BOOST_CHECK_EQUAL(failed_over.selected_endpoint, preferred.url);
    BOOST_CHECK_EQUAL(failed_over.active_endpoint, Endpoint(fallback).url);
    BOOST_CHECK_EQUAL(failed_over.endpoints[0].consecutive_failures, 1U);
    BOOST_CHECK(!failed_over.endpoints[0].transport_available);
    BOOST_CHECK_EQUAL(fallback_requests.load(), 1U);

    node::FlowMeshHttpsServer recovered{options, [&](const auto&) {
        ++preferred_requests;
        return node::FlowMeshHttpsServer::Response{200, R"({"ok":true,"result":[],"error":""})"};
    }};
    BOOST_REQUIRE_MESSAGE(recovered.Start(error), error);
    // Expire the first transport cooldown. There is no explicit reconnect,
    // node restart, or failure of the still-healthy fallback endpoint.
    std::this_thread::sleep_for(std::chrono::milliseconds{2100});
    BOOST_CHECK(client->Markets(std::nullopt).empty());
    const auto restored{client->Status()};
    BOOST_CHECK_EQUAL(restored.selected_endpoint, preferred.url);
    BOOST_CHECK_EQUAL(restored.active_endpoint, preferred.url);
    BOOST_CHECK(restored.endpoints[0].available);
    BOOST_CHECK(restored.endpoints[0].transport_available);
    BOOST_CHECK_EQUAL(restored.endpoints[0].consecutive_failures, 0U);
    BOOST_CHECK_EQUAL(restored.endpoints[0].retry_after_ms, 0);
    BOOST_CHECK_EQUAL(preferred_requests.load(), 1U);
    BOOST_CHECK_EQUAL(fallback_requests.load(), 1U);
    BOOST_CHECK_EQUAL(restored.pending_actions, 0U);
}

BOOST_AUTO_TEST_CASE(client_transport_cooldown_does_not_weaken_configured_pin)
{
    std::atomic<unsigned int> requests{0};
    node::FlowMeshHttpsServer server{Options(), [&](const auto&) {
        ++requests;
        return node::FlowMeshHttpsServer::Response{200, R"({"ok":true,"result":[],"error":""})"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    auto configured{Endpoint(server)};
    configured.certificate_sha256 = std::string(64, '0');
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {configured}, m_path_root / "client", error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_MESSAGE(client->Connect(configured.url + "/flowmesh/v1", error), error);
    const auto failed{client->Status().endpoints[0]};
    BOOST_CHECK_EQUAL(failed.last_error, "https-certificate-pin-mismatch");
    BOOST_CHECK_EQUAL(failed.consecutive_failures, 1U);
    BOOST_CHECK(!failed.available);
    BOOST_CHECK(!failed.transport_available);
    BOOST_CHECK_THROW(client->Markets(std::nullopt), std::runtime_error);
    BOOST_CHECK_EQUAL(client->Status().endpoints[0].last_attempt_ms, failed.last_attempt_ms);
    BOOST_CHECK_EQUAL(client->Status().endpoints[0].consecutive_failures, 1U);
    // An explicit user connect may bypass the timer, but never its CA/pin.
    BOOST_REQUIRE_MESSAGE(client->Connect(configured.url, error), error);
    BOOST_CHECK_EQUAL(client->Status().endpoints[0].last_error, "https-certificate-pin-mismatch");
    BOOST_CHECK_EQUAL(client->Status().endpoints[0].consecutive_failures, 2U);
    BOOST_CHECK_EQUAL(requests.load(), 0U);
}

BOOST_AUTO_TEST_CASE(client_invalid_saved_origin_fails_closed_without_reset)
{
    const fs::path path{m_path_root / "client"};
    const std::string invalid{R"({"version":1,"urls":["http://127.0.0.1"],"selected_url":"http://127.0.0.1"})"};
    {
        CDBWrapper db{DBParams{.path=path, .cache_bytes=1 << 20}};
        db.Write(std::string{"public-client-endpoints-v1"}, invalid, true);
    }
    std::string error;
    BOOST_CHECK(!node::MakeRemoteFlowMeshBackend(*m_node.chainman, {}, path, error));
    BOOST_CHECK(error.find("Invalid saved trading endpoint") != std::string::npos);
    CDBWrapper db{DBParams{.path=path, .cache_bytes=1 << 20}};
    std::string preserved;
    BOOST_REQUIRE(db.Read(std::string{"public-client-endpoints-v1"}, preserved));
    BOOST_CHECK_EQUAL(preserved, invalid);
}

BOOST_AUTO_TEST_CASE(client_saved_actions_do_not_wait_for_inflight_network_work)
{
    const fs::path path{m_path_root / "client"};
    const uint256 market{uint256::ONE};
    const uint256 owner{*uint256::FromHex(std::string(64, '4'))};
    const auto action{SeedRetainedAction(path, market, owner)};
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{false}, release{false};
    // Holds the one network call (and so the client's work gate) until the
    // saved-action read below has returned, or at most a bounded time.
    node::FlowMeshHttpsServer server{Options(), [&](const auto&) {
        std::unique_lock lock{mutex};
        entered = true;
        condition.notify_all();
        condition.wait_for(lock, std::chrono::seconds{4}, [&] { return release; });
        return node::FlowMeshHttpsServer::Response{200, R"({"ok":true,"result":[],"error":""})"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server)}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    std::thread discovery{[&] {
        try { (void)client->Markets(std::nullopt); } catch (const std::exception&) {}
    }};
    bool blocked{false};
    {
        std::unique_lock lock{mutex};
        blocked = condition.wait_for(lock, std::chrono::seconds{2}, [&] { return entered; });
    }
    const auto start{std::chrono::steady_clock::now()};
    const auto saved{client->SavedActions(owner, market)};
    const auto elapsed{std::chrono::steady_clock::now() - start};
    {
        std::lock_guard lock{mutex};
        release = true;
    }
    condition.notify_all();
    discovery.join();
    server.Stop();
    BOOST_CHECK(blocked);
    BOOST_CHECK(elapsed < std::chrono::seconds{1});
    BOOST_REQUIRE_EQUAL(saved.size(), 1U);
    BOOST_CHECK(saved[0].receipt.action_id == action.Id());
    BOOST_CHECK(saved[0].account_id == owner);
    BOOST_CHECK(saved[0].market_id == market);
    BOOST_REQUIRE(saved[0].sequence);
    BOOST_CHECK_EQUAL(*saved[0].sequence, 3U);
    BOOST_CHECK_EQUAL(saved[0].canonical_side, "bid");
    const auto bytes{flowmesh::EncodeProductionActionPayload(action)};
    BOOST_REQUIRE(bytes);
    BOOST_CHECK_EQUAL(saved[0].signed_bytes_sha256, Sha256Hex(*bytes));
    BOOST_CHECK_EQUAL(saved[0].signed_bytes_size, bytes->size());
    BOOST_CHECK(saved[0].may_have_been_sent);
    BOOST_CHECK(!saved[0].previously_certified);
}

BOOST_AUTO_TEST_CASE(client_saved_view_reflects_completed_method)
{
    const fs::path path{m_path_root / "client"};
    const uint256 market{uint256::ONE};
    const uint256 owner{*uint256::FromHex(std::string(64, '4'))};
    const auto action{SeedRetainedAction(path, market, owner)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    const auto restored{client->SavedActions(owner, market)};
    BOOST_REQUIRE_EQUAL(restored.size(), 1U);
    BOOST_CHECK_EQUAL(restored[0].receipt.reason, "Retained original action; fresh status evidence required after restart");
    // This market is not established on the test chain, so the authority
    // recheck fails before any network use and records its reason.
    const auto receipt{client->ActionStatus(market, action.Id(), false)};
    BOOST_REQUIRE(!receipt.reason.empty());
    const auto updated{client->SavedActions(owner, market)};
    BOOST_REQUIRE_EQUAL(updated.size(), 1U);
    BOOST_CHECK_EQUAL(updated[0].receipt.reason, receipt.reason);
    BOOST_CHECK(updated[0].receipt.reason != restored[0].receipt.reason);
    BOOST_CHECK_EQUAL(client->SavedActions(owner, std::nullopt).size(), 1U);
    BOOST_CHECK(client->SavedActions(owner, uint256{}).empty());
    BOOST_CHECK(client->SavedActions(uint256{}, market).empty());
    BOOST_CHECK(client->SavedActions(uint256::ONE, market).empty());
}

BOOST_AUTO_TEST_SUITE_END()
