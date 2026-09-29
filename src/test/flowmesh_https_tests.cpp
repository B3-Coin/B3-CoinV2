// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_https.h>
#include <node/flowmesh_client.h>
#include <node/flowmesh_client_action_wait.h>
#include <node/flowmesh_client_outcome.h>
#include <node/flowmesh_service.h>
#include <node/flowmesh_timing.h>
#include <chain.h>
#include <consensus/era.h>
#include <consensus/flowmesh_params.h>
#include <crypto/sha256.h>
#include <dbwrapper.h>
#include <flowmesh/bls_certificate.h>
#include <flowmesh/outcome_evidence.h>
#include <flowmesh/production_engine.h>
#include <flowmesh/production_wire.h>
#include <modern/asset_output.h>
#include <modern/asset_validation.h>
#include <modern/chain_domain.h>
#include <modern/flowmesh_seat.h>
#include <modern/fn.h>
#include <modern/mpa.h>
#include <node/flowmesh_anchor.h>
#include <node/flowmesh_checkpoint_index.h>
#include <node/flowmesh_vault_index.h>
#include <node/fn_seat_index.h>
#include <primitives/block.h>
#include <test/util/flowmesh.h>
#include <test/util/setup_common.h>
#include <util/fs.h>
#include <util/sock.h>
#include <util/strencodings.h>
#include <validation.h>

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
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef WIN32
#include <csignal>
#include <sys/resource.h>
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
    //! receive_buffer, when nonzero, sets SO_RCVBUF before connecting, so a
    //! peer that stops reading holds back a large reply early.
    SplitHttpsPeer(uint16_t port, const fs::path& certificate, int max_version = 0, int receive_buffer = 0)
    {
        BOOST_REQUIRE(m_fd != INVALID_SOCKET);
        BOOST_REQUIRE(m_context);
        if (receive_buffer) BOOST_REQUIRE_EQUAL(m_socket.SetSockOpt(SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)), 0);
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

    //! Reads plaintext until the server closes the connection, `limit` bytes
    //! or 5 s. Returns the byte count and the first bytes (the status line).
    std::pair<size_t, std::string> Drain(size_t limit)
    {
        size_t total{0};
        std::string head;
        std::vector<char> bytes(64 * 1024);
        const auto deadline{Clock::now() + std::chrono::seconds{5}};
        while (total < limit) {
            ERR_clear_error();
            const int result{SSL_read(m_ssl.get(), bytes.data(), static_cast<int>(bytes.size()))};
            if (result > 0) {
                if (head.size() < 64) head.append(bytes.data(), std::min<size_t>(result, 64 - head.size()));
                total += result;
            } else if (!Again(result, deadline)) break;
        }
        return {total, head};
    }
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

BOOST_AUTO_TEST_CASE(https_client_warm_matches_only_a_reusable_connection_to_the_same_origin)
{
    node::FlowMeshHttpsServer server{Options(), [](const auto&) {
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    node::FlowMeshHttpsServer other{Options(), [](const auto&) {
        return node::FlowMeshHttpsServer::Response{200, "{}"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    BOOST_REQUIRE_MESSAGE(other.Start(error), error);
    const auto endpoint{Endpoint(server)};
    node::FlowMeshHttpsClient client;
    BOOST_CHECK(!client.Warm(endpoint)); // Nothing retained yet.
    BOOST_REQUIRE(client.Request(endpoint, "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024).response_received);
    BOOST_CHECK(client.Warm(endpoint));
    // The same origin spelled differently is the same connection.
    auto spelled{endpoint};
    spelled.url += "/flowmesh/v1";
    BOOST_CHECK(client.Warm(spelled));
    // Another origin, another pin, no pin, another CA file or a malformed
    // endpoint never match; the check changes nothing.
    BOOST_CHECK(!client.Warm(Endpoint(other)));
    auto wrong_pin{endpoint};
    wrong_pin.certificate_sha256 = std::string(64, '0');
    BOOST_CHECK(!client.Warm(wrong_pin));
    auto no_pin{endpoint};
    no_pin.certificate_sha256.clear();
    BOOST_CHECK(!client.Warm(no_pin));
    auto other_ca{endpoint};
    other_ca.ca_file = m_path_root / "other-ca.pem";
    BOOST_CHECK(!client.Warm(other_ca));
    auto malformed{endpoint};
    malformed.url = "http://127.0.0.1";
    BOOST_CHECK(!client.Warm(malformed));
    BOOST_CHECK(client.Warm(endpoint));
    const auto reused{client.Request(endpoint, "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
    BOOST_REQUIRE_MESSAGE(reused.response_received, reused.error);
    BOOST_CHECK(reused.connection_reused);
    client.Reset();
    BOOST_CHECK(!client.Warm(endpoint));
    // A connection the server closed is never reported warm.
    BOOST_REQUIRE(client.Request(endpoint, "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024).response_received);
    BOOST_REQUIRE(client.Warm(endpoint));
    server.Stop();
    const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{2}};
    while (client.Warm(endpoint) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    BOOST_CHECK(!client.Warm(endpoint));
    other.Stop();
}

BOOST_AUTO_TEST_CASE(https_client_swap_carries_a_verified_connection_without_a_new_handshake)
{
    ConnectionObservation connections;
    node::FlowMeshHttpsServer server{Options(), [](const auto& request) {
        return node::FlowMeshHttpsServer::Response{200, request.body};
    }};
    BOOST_REQUIRE(node::FlowMeshHttpsTestAccess::ObserveConnection(server,
        [&](bool tcp_nodelay) { connections.Record(tcp_nodelay); }));
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    const auto endpoint{Endpoint(server)};
    node::FlowMeshHttpsClient ordinary, lane;
    const auto first{ordinary.Request(endpoint, "/flowmesh/v1", "{\"first\":1}", std::chrono::seconds{2}, 1024)};
    BOOST_REQUIRE_MESSAGE(first.response_received, first.error);
    BOOST_CHECK(first.tls_handshake_performed);
    BOOST_REQUIRE(ordinary.Warm(endpoint));
    BOOST_REQUIRE(!lane.Warm(endpoint));
    BOOST_CHECK(lane.Swap(ordinary));
    BOOST_CHECK(lane.Warm(endpoint));
    BOOST_CHECK(!ordinary.Warm(endpoint));
    // The carried connection serves the other session without a handshake.
    const auto carried{lane.Request(endpoint, "/flowmesh/v1", "{\"carried\":1}", std::chrono::seconds{2}, 1024)};
    BOOST_REQUIRE_MESSAGE(carried.response_received, carried.error);
    BOOST_CHECK_EQUAL(carried.body, "{\"carried\":1}");
    BOOST_CHECK(carried.connection_reused);
    BOOST_CHECK(!carried.tls_handshake_performed);
    BOOST_CHECK(carried.request_may_have_been_sent);
    // Handed back, it serves the first session again: still one connection.
    BOOST_CHECK(ordinary.Swap(lane));
    const auto back{ordinary.Request(endpoint, "/flowmesh/v1", "{\"back\":1}", std::chrono::seconds{2}, 1024)};
    BOOST_REQUIRE_MESSAGE(back.response_received, back.error);
    BOOST_CHECK(back.connection_reused);
    BOOST_CHECK(!back.tls_handshake_performed);
    const auto samples{connections.Await(1)};
    BOOST_REQUIRE(samples);
    // The session left with the empty state opens its own connection.
    const auto own{lane.Request(endpoint, "/flowmesh/v1", "{}", std::chrono::seconds{2}, 1024)};
    BOOST_REQUIRE_MESSAGE(own.response_received, own.error);
    BOOST_CHECK(!own.connection_reused);
    BOOST_CHECK(own.tls_handshake_performed);
    BOOST_REQUIRE(connections.Await(2));
    // Sessions with different keep-alive policies never exchange state.
    node::FlowMeshHttpsClient one_shot{false};
    BOOST_CHECK(!one_shot.Swap(ordinary));
    BOOST_CHECK(ordinary.Warm(endpoint));
    server.Stop();
    BOOST_CHECK_EQUAL(connections.Count(), 2U);
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
    // Far larger than the socket buffers, so the reply write blocks, yet small
    // enough to drain well inside the deadlines on a loaded host.
    constexpr size_t LARGE_REPLY{2 * 1024 * 1024};
    auto options{Options()};
    options.worker_threads = 8;
    options.active_permits = 2;
    options.request_timeout = std::chrono::seconds{10};
    options.max_reply_bytes = LARGE_REPLY + 1024;
    std::atomic<unsigned> handled{0};
    node::FlowMeshHttpsServer server{options, [&](const auto& request) {
        ++handled;
        if (request.body == "large") return node::FlowMeshHttpsServer::Response{200, std::string(LARGE_REPLY, 'x')};
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

    // Write: a peer that stops reading a reply far larger than the socket
    // buffers keeps its permit while the server's write is blocked, and a
    // peer in its read holds the other; again no further handshake starts.
    SplitHttpsPeer slow{server.Port(), cert, 0, 4096};
    SplitHttpsPeer holder{server.Port(), cert};
    BOOST_REQUIRE(observed.Await(5));
    BOOST_REQUIRE(slow.Send("POST /flowmesh/v1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nConnection: close\r\n\r\nlarge"));
    const auto handler_deadline{std::chrono::steady_clock::now() + std::chrono::seconds{2}};
    while (handled.load() < 4 && std::chrono::steady_clock::now() < handler_deadline) std::this_thread::sleep_for(std::chrono::milliseconds{1});
    BOOST_REQUIRE_EQUAL(handled.load(), 4U);
    auto fourth{std::async(std::launch::async, [&] {
        return node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1", "{}", std::chrono::seconds{10}, 1024);
    })};
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    BOOST_CHECK_EQUAL(observed.Count(), 5U);
    BOOST_CHECK(fourth.wait_for(std::chrono::seconds{0}) == std::future_status::timeout);
    // Reading the whole reply completes the write and frees that permit.
    const auto [drained, head]{slow.Drain(LARGE_REPLY + 4096)};
    BOOST_CHECK(head.starts_with("HTTP/1.1 200 "));
    BOOST_CHECK_GT(drained, LARGE_REPLY);
    const auto fourth_reply{fourth.get()};
    BOOST_REQUIRE_MESSAGE(fourth_reply.response_received, fourth_reply.error);
    BOOST_CHECK_EQUAL(fourth_reply.status, 200);
    BOOST_CHECK_EQUAL(observed.Count(), 6U);
    BOOST_REQUIRE(holder.Send(request));
    BOOST_REQUIRE(holder.ReadResponse(2).starts_with("HTTP/1.1 200 "));
    BOOST_CHECK_EQUAL(handled.load(), 6U);
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
    static flowmesh::Action SeedRetainedAction(const fs::path& path, const uint256& market, const uint256& owner,
                                               const uint256& domain = uint256::ONE, const uint256& config = uint256::ONE)
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
        row.pushKV("domain", domain.GetHex());
        row.pushKV("config", config.GetHex());
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

BOOST_AUTO_TEST_CASE(client_waited_status_without_a_local_market_sends_nothing)
{
    const fs::path path{m_path_root / "client"};
    const uint256 market{uint256::ONE};
    const uint256 owner{*uint256::FromHex(std::string(64, '4'))};
    const auto action{SeedRetainedAction(path, market, owner)};
    std::atomic<unsigned int> requests{0}, waited{0};
    // Answers everything with an empty result, which advertises no wait. The
    // market is not established on this test chain, so no read may reach it.
    node::FlowMeshHttpsServer server{Options(), [&](const node::FlowMeshHttpsServer::Request& request) {
        ++requests;
        if (request.body.find("wait_ms") != std::string::npos) ++waited;
        return node::FlowMeshHttpsServer::Response{200, R"({"ok":true,"result":[],"error":""})"};
    }};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start(error), error);
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server)}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    // A waited call returns promptly with the ordinary call's result: the
    // local authority check fails before any network use on either path.
    // (flowmesh_client_wait_lane_tests shows, on a chain where the market
    // exists, that only a delivery acknowledged in this process enables a
    // waited read.)
    const auto start{std::chrono::steady_clock::now()};
    const auto receipt{client->ActionStatus(market, action.Id(), false, std::chrono::milliseconds{2000})};
    BOOST_CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
    BOOST_CHECK_EQUAL(requests.load(), 0U);
    BOOST_CHECK(!receipt.certificate_verified);
    BOOST_CHECK_EQUAL(receipt.state, "unknown");
    BOOST_CHECK(!receipt.reason.empty());
    BOOST_CHECK(receipt.action_id == action.Id());
    const auto ordinary{client->ActionStatus(market, action.Id(), false)};
    BOOST_CHECK_EQUAL(ordinary.state, receipt.state);
    BOOST_CHECK_EQUAL(ordinary.reason, receipt.reason);
    const auto saved{client->SavedActions(owner, market)};
    BOOST_REQUIRE_EQUAL(saved.size(), 1U);
    BOOST_CHECK(saved[0].may_have_been_sent);
    BOOST_CHECK(!saved[0].previously_certified);
    BOOST_CHECK_EQUAL(saved[0].receipt.reason, receipt.reason);
    BOOST_CHECK_EQUAL(requests.load(), 0U);
    BOOST_CHECK_EQUAL(waited.load(), 0U);
    server.Stop();
}

BOOST_AUTO_TEST_CASE(trading_api_action_wait_contract_and_untracked_actions)
{
    // Not started: no runtime and no recorded action. The contract checks run
    // before any wait, and an unrecorded action never takes a wait slot.
    node::FlowMeshService service{*m_node.chainman, m_path_root / "api-action-wait"};
    const std::string ids{"\"market_id\":\"" + uint256::ONE.GetHex() + "\",\"action_id\":\"" +
                          uint256{uint8_t{2}}.GetHex() + "\""};
    const auto call = [&](node::FlowMeshHttpsServer& server, const std::string& params) {
        const auto reply{node::FlowMeshHttpsRequest(Endpoint(server), "/flowmesh/v1",
            "{\"method\":\"action\",\"params\":{" + params + "}}", std::chrono::seconds{5}, 1 << 20)};
        BOOST_REQUIRE_MESSAGE(reply.response_received, reply.error);
        UniValue value;
        BOOST_REQUIRE(value.read(reply.body));
        return std::pair{reply.status, value};
    };
    std::string error;
    {
        // The default is disabled: exactly the previous request contract.
        auto api{node::MakeFlowMeshTradingApi(service, Options())};
        BOOST_REQUIRE_MESSAGE(api->Start(error), error);
        const auto [status, reply]{call(*api, ids)};
        BOOST_CHECK_EQUAL(status, 200);
        BOOST_CHECK_EQUAL(reply["result"]["receipt_state"].get_str(), "unknown");
        BOOST_CHECK(!reply["result"].exists("wait_status"));
        const auto [refused, rejection]{call(*api, ids + ",\"wait_ms\":500")};
        BOOST_CHECK_EQUAL(refused, 400);
        BOOST_CHECK_EQUAL(rejection["error"].get_str(), "Unknown or duplicate client API field");
        // The error a client matches to withdraw a stale capability.
        BOOST_CHECK_EQUAL(rejection["error"].get_str(), node::FLOWMESH_CLIENT_UNKNOWN_FIELD_ERROR);
        api->Stop();
    }
    {
        auto api{node::MakeFlowMeshTradingApi(service, Options(), {}, {std::chrono::milliseconds{2000}, 4})};
        BOOST_REQUIRE_MESSAGE(api->Start(error), error);
        // A request without wait_ms gets the previous reply shape.
        const auto [status, reply]{call(*api, ids)};
        BOOST_CHECK_EQUAL(status, 200);
        BOOST_CHECK(!reply["result"].exists("wait_status"));
        for (const std::string wait : {"0", "500", "60000", "18446744073709551615"}) {
            BOOST_TEST_CONTEXT("wait_ms=" << wait) {
                const auto start{std::chrono::steady_clock::now()};
                const auto [waited_status, waited]{call(*api, ids + ",\"wait_ms\":" + wait)};
                BOOST_CHECK_EQUAL(waited_status, 200); // Large values are clamped, not refused.
                BOOST_CHECK_EQUAL(waited["result"]["receipt_state"].get_str(), "unknown");
                BOOST_CHECK_EQUAL(waited["result"]["wait_status"].get_str(), wait == "0" ? "none" : "untracked");
                BOOST_CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
            }
        }
        for (const std::string bad : {"-1", "1.5", "\"500\"", "true", "18446744073709551616", "1,\"wait_ms\":2"}) {
            BOOST_TEST_CONTEXT("wait_ms=" << bad) {
                const auto [bad_status, rejection]{call(*api, ids + ",\"wait_ms\":" + bad)};
                BOOST_CHECK_EQUAL(bad_status, 400);
                BOOST_CHECK(!rejection["ok"].get_bool());
            }
        }
        const auto [unknown_status, unknown]{call(*api, ids + ",\"wait\":500")};
        BOOST_CHECK_EQUAL(unknown_status, 400);
        BOOST_CHECK_EQUAL(unknown["error"].get_str(), "Unknown or duplicate client API field");
        api->Stop();
    }
}

BOOST_AUTO_TEST_SUITE_END()

namespace {
bls::SecretKey LaneSeatKey(const unsigned char tag)
{
    std::array<unsigned char, 32> ikm{};
    ikm.fill(tag);
    const auto key{bls::SecretKey::FromIKM(ikm)};
    BOOST_REQUIRE(key);
    return *key;
}

#ifndef WIN32
//! Every write that would extend a regular file in this process fails (EFBIG,
//! as on a full disk) until destruction, so the client outbox's synchronous
//! write throws. SIGXFSZ is ignored meanwhile; both are restored afterwards.
struct ScopedFailedFileWrites {
    struct sigaction previous_action{};
    rlimit previous{};
    ScopedFailedFileWrites()
    {
        struct sigaction ignore{};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        BOOST_REQUIRE_EQUAL(sigaction(SIGXFSZ, &ignore, &previous_action), 0);
        BOOST_REQUIRE_EQUAL(getrlimit(RLIMIT_FSIZE, &previous), 0);
        const rlimit none{0, previous.rlim_max};
        BOOST_REQUIRE_EQUAL(setrlimit(RLIMIT_FSIZE, &none), 0);
    }
    ~ScopedFailedFileWrites()
    {
        setrlimit(RLIMIT_FSIZE, &previous);
        sigaction(SIGXFSZ, &previous_action, nullptr);
    }
};
#endif

//! A scripted public trading endpoint. It answers every method with the
//! reply the test set and counts what it received; 'action' reads with and
//! without wait_ms are counted apart.
struct ScriptedEndpoint {
    std::mutex mutex;
    UniValue markets{UniValue::VARR};
    UniValue plain{UniValue::VOBJ}, waited{UniValue::VOBJ}, submit{UniValue::VOBJ};
    unsigned plain_reads{0}, waited_reads{0}, submits{0}, market_reads{0};
    //! Opt-in outcome requests: 'action' reads carrying "outcome": true, and
    //! 'outcome_evidence' reads (answered with `evidence`, or `evidence_error`).
    unsigned outcome_flags{0}, evidence_reads{0};
    UniValue evidence{UniValue::VOBJ};
    std::string evidence_error;
    //! Answer "outcome" with the unknown-field error (a downgraded endpoint).
    bool reject_outcome_field{false};
    //! Unwaited 'action' replies for specific ActionIds (instead of `plain`),
    //! and how often each was read.
    std::map<std::string, UniValue> by_action;
    std::map<std::string, unsigned> by_action_reads;
    //! Called with the method and params before `mutex` is taken, so a test
    //! can hold one request while others (on other connections) are served.
    //! Set before the endpoint serves.
    std::function<void(const std::string&, const UniValue&)> before;
    //! Submissions carrying wait_ms, counted apart from `submits` and
    //! answered with `waited_submit`, or, with reject_submit_wait, with the
    //! unknown-field error (a downgraded endpoint).
    unsigned waited_submits{0};
    UniValue waited_submit{UniValue::VOBJ};
    bool reject_submit_wait{false};
    //! The params of every submission, in arrival order.
    std::vector<UniValue> submitted;

    node::FlowMeshHttpsServer::Response Handle(const node::FlowMeshHttpsServer::Request& request)
    {
        UniValue json;
        if (!json.read(request.body) || !json.isObject() || !json["method"].isStr()) return {400, R"({"ok":false,"error":"test","result":null})"};
        const auto& method{json["method"].get_str()};
        if (before) before(method, json["params"]);
        std::lock_guard lock{mutex};
        UniValue out{UniValue::VOBJ};
        out.pushKV("ok", true);
        if (method == "action" && json["params"].exists("outcome")) {
            ++outcome_flags;
            if (reject_outcome_field) return {400, R"({"ok":false,"error":"Unknown or duplicate client API field"})"};
        }
        if (method == "outcome_evidence") {
            ++evidence_reads;
            if (!evidence_error.empty()) return {400, R"({"ok":false,"error":")" + evidence_error + R"("})"};
            out.pushKV("result", evidence);
            out.pushKV("error", "");
            return {200, out.write()};
        }
        if (method == "markets") {
            ++market_reads;
            out.pushKV("result", markets);
        } else if (method == "submit") {
            submitted.push_back(json["params"]);
            if (json["params"].exists("wait_ms")) {
                ++waited_submits;
                if (reject_submit_wait) return {400, R"({"ok":false,"error":"Unknown or duplicate client API field"})"};
                out.pushKV("result", waited_submit);
            } else {
                ++submits;
                out.pushKV("result", submit);
            }
        } else if (method == "action" && json["params"].exists("wait_ms")) {
            ++waited_reads;
            out.pushKV("result", waited);
        } else if (method == "action" && json["params"]["action_id"].isStr() &&
                   by_action.contains(json["params"]["action_id"].get_str())) {
            const auto& id{json["params"]["action_id"].get_str()};
            ++by_action_reads[id];
            out.pushKV("result", by_action.at(id));
        } else if (method == "action") {
            ++plain_reads;
            out.pushKV("result", plain);
        } else {
            return {400, R"({"ok":false,"error":"unexpected method","result":null})"};
        }
        out.pushKV("error", "");
        return {200, out.write()};
    }
    std::array<unsigned, 4> Counts()
    {
        std::lock_guard lock{mutex};
        return {plain_reads, waited_reads, submits, market_reads};
    }
};

//! Holds the requests a ScriptedEndpoint::before hook passes to Hold until
//! Release (at most 4 s), and tells the test when they arrived.
struct RequestGate {
    std::mutex mutex;
    std::condition_variable condition;
    unsigned entered{0};
    bool released{false};
    void Hold()
    {
        std::unique_lock lock{mutex};
        ++entered;
        condition.notify_all();
        condition.wait_for(lock, std::chrono::seconds{4}, [&] { return released; });
    }
    bool AwaitEntered(const unsigned count = 1)
    {
        std::unique_lock lock{mutex};
        return condition.wait_for(lock, std::chrono::seconds{3}, [&] { return entered >= count; });
    }
    void Release()
    {
        {
            std::lock_guard lock{mutex};
            released = true;
        }
        condition.notify_all();
    }
};

//! Records this process's FlowMesh timing spans (default off, as in
//! production) for one test; Stop returns the events of one stage. The
//! process-wide capture is left stopped and empty, as other suites expect.
struct TimingCapture {
    bool active{true};
    TimingCapture() { node::FlowMeshTimingControl("start"); }
    ~TimingCapture()
    {
        try {
            if (active) node::FlowMeshTimingControl("stop");
            Clear();
        } catch (...) {
        }
    }
    static void Clear()
    {
        node::FlowMeshTimingControl("start");
        node::FlowMeshTimingControl("stop");
    }
    std::vector<UniValue> Stop(const std::string& stage)
    {
        active = false;
        const auto capture{node::FlowMeshTimingControl("stop")};
        std::vector<UniValue> out;
        for (const auto& row : capture["events"].getValues()) {
            const auto& event{row["event"]};
            if (event["stage"].isStr() && event["stage"].get_str() == stage) out.push_back(event);
        }
        return out;
    }
};

/** A regtest chain on which one FlowMesh market and its four anchored BLS
 * seats are established (the synthetic blocks of flowmesh_service_tests'
 * restart setup), so a remote client's local authority checks pass and
 * certificates made with the seat keys verify. No block data, peers, wallet
 * or network beyond generated loopback HTTPS endpoints. */
struct ClientLaneFixture : TestingSetup {
    ScopedClientSignals signals;
    fs::path cert{m_path_root / "lane-cert.pem"};
    fs::path key{m_path_root / "lane-key.pem"};
    std::string pin;
    const modern::AssetId asset{uint256{uint8_t{0x31}}};
    uint256 domain, config;
    flowmesh::MarketId market;
    flowmesh::VaultId vault;
    flowmesh::ActiveFnBlsSeatSet seats;
    std::vector<bls::SecretKey> secrets;
    //! The bootstrap user deposit established on this chain (account 0x41).
    COutPoint deposit_outpoint;
    CBlockIndex* original_tip{nullptr};

    ClientLaneFixture()
        : TestingSetup(ChainType::REGTEST, {.extra_args = {"-b3modernregtest", "-b3flowmeshtest", "-b3corridorlength=130"}})
    {
        HttpsFixture::CreateCertificate(cert, key, pin, true);
        auto& chainman{*m_node.chainman};
        const auto& params{chainman.GetConsensus()};
        BOOST_REQUIRE(Consensus::FlowMeshSeatBindingScheduleConfigured(params));
        domain = *modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash);
        market = *flowmesh::ComputeFlowMeshMarketId(domain, asset);
        vault = *flowmesh::ComputeFlowMeshVaultId(domain, market);
        config = flowmesh::ComputeExecutionConfigId(vault, asset, modern::NativeAsset(), flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS);
        const auto fn_asset{modern::ConfiguredFnAssetId(params)};
        BOOST_REQUIRE(fn_asset);
        CMutableTransaction bindings;
        bindings.version = 2;
        const CScript owner{CScript() << OP_DUP << OP_HASH160 << std::vector<unsigned char>(20, 0x51) << OP_EQUALVERIFY << OP_CHECKSIG};
        for (uint32_t i{0}; i < 4; ++i) {
            secrets.push_back(LaneSeatKey(20 + i));
            const auto output{modern::MakeFlowMeshSeatOutput(*fn_asset, owner, secrets.back().GetPublicKey())};
            BOOST_REQUIRE(output);
            bindings.vout.push_back(*output);
            bindings.mpa.push_back(modern::MakeFlowMeshSeatBindingRecord(i, secrets.back().SignPoP().Compressed()));
        }
        const uint256 account{uint8_t{0x41}};
        const auto deposit{modern::MakeDexVaultOutput(asset, 1, vault, modern::VAULT_KIND_USER_DEPOSIT,
                                                      modern::FlowMeshUserDepositShard(vault, account), account)};
        BOOST_REQUIRE(deposit);
        CMutableTransaction bootstrap;
        bootstrap.version = 2;
        bootstrap.vout.push_back(*deposit);
        deposit_outpoint = COutPoint{bootstrap.GetHash(), 0};

        LOCK(::cs_main);
        auto& chainstate{chainman.ActiveChainstate()};
        auto& chain{chainstate.m_chain};
        original_tip = chain.Tip();
        BOOST_REQUIRE(original_tip);
        auto& seat_tracker{chainstate.ModernFnSeats()};
        auto& vault_tracker{chainstate.ModernFlowMeshVaults()};
        auto& checkpoint_tracker{chainstate.ModernFlowMeshCheckpoints()};
        BOOST_REQUIRE(seat_tracker.Sync(chain, chainman.m_blockman, params, *original_tip));
        BOOST_REQUIRE(vault_tracker.Sync(chain, chainman.m_blockman, params, *original_tip));
        BOOST_REQUIRE(checkpoint_tracker.Sync(chain, chainman.m_blockman, params, seat_tracker.Index(), vault_tracker.Index(), *original_tip));
        for (int height{1}; height <= *params.flowmesh_activation_height + 1; ++height) {
            CBlock block;
            block.nVersion = 2;
            block.hashPrevBlock = chain.Tip()->GetBlockHash();
            block.nTime = original_tip->nTime + height;
            block.nNonce = height;
            if (height == *params.asset_activation_height) block.vtx = {MakeTransactionRef(bindings), MakeTransactionRef(bootstrap)};
            auto* index{chainman.m_blockman.InsertBlockIndex(block.GetHash())};
            BOOST_REQUIRE(index);
            index->nHeight = height;
            index->pprev = chain.Tip();
            index->nTime = block.nTime;
            index->BuildSkip();
            chain.SetTip(*index);
            seat_tracker.BlockConnected(block, *index, params);
            vault_tracker.BlockConnected(block, *index, params);
            checkpoint_tracker.BlockConnected(block, *index, chain, params, seat_tracker.Index(), vault_tracker.Index());
            BOOST_REQUIRE(seat_tracker.Synced(index->GetBlockHash()));
            BOOST_REQUIRE(vault_tracker.Synced(index->GetBlockHash()));
            BOOST_REQUIRE(checkpoint_tracker.Synced(index->GetBlockHash()));
        }
        const auto* anchor{chain[*params.asset_activation_height]};
        const auto snapshot{seat_tracker.Index().SnapshotAt(*anchor)};
        BOOST_REQUIRE(snapshot);
        std::vector<flowmesh::BlsSeatBinding> seat_bindings;
        for (const auto& member : snapshot->members) seat_bindings.push_back({member.outpoint, member.bls_pubkey, member.proof_of_possession});
        flowmesh::BlsSeatSetCheck check;
        const auto active{flowmesh::BuildActiveFnBlsSeatSet(domain, market, 0, anchor->nHeight, anchor->GetBlockHash(), seat_bindings, check)};
        BOOST_REQUIRE_MESSAGE(active, flowmesh::BlsSeatSetCheckName(check));
        seats = *active;
        std::sort(secrets.begin(), secrets.end(), [&](const auto& a, const auto& b) {
            const auto index_of = [&](const auto& secret) {
                return std::find_if(seats.members.begin(), seats.members.end(),
                    [&](const auto& member) { return member.key.Key() == secret.GetPublicKey(); });
            };
            return index_of(a) < index_of(b);
        });
    }
    ~ClientLaneFixture()
    {
        LOCK(::cs_main);
        m_node.chainman->ActiveChain().SetTip(*original_tip);
    }

    //! Connect one synthetic B3 block carrying `records` and index it.
    void ConnectMpaBlock(const std::vector<CMpaRecord>& records)
    {
        LOCK(::cs_main);
        auto& chainman{*m_node.chainman};
        auto& chainstate{chainman.ActiveChainstate()};
        auto& chain{chainstate.m_chain};
        const auto& params{chainman.GetConsensus()};
        CMutableTransaction transaction;
        transaction.version = 2;
        transaction.mpa = records;
        CBlock block;
        block.nVersion = 2;
        block.hashPrevBlock = chain.Tip()->GetBlockHash();
        block.nTime = chain.Tip()->nTime + 1;
        block.nNonce = chain.Height() + 1;
        block.vtx = {MakeTransactionRef(transaction)};
        auto* index{chainman.m_blockman.InsertBlockIndex(block.GetHash())};
        BOOST_REQUIRE(index);
        index->nHeight = chain.Height() + 1;
        index->pprev = chain.Tip();
        index->nTime = block.nTime;
        index->BuildSkip();
        chain.SetTip(*index);
        auto& seat_tracker{chainstate.ModernFnSeats()};
        auto& vault_tracker{chainstate.ModernFlowMeshVaults()};
        auto& checkpoint_tracker{chainstate.ModernFlowMeshCheckpoints()};
        seat_tracker.BlockConnected(block, *index, params);
        vault_tracker.BlockConnected(block, *index, params);
        checkpoint_tracker.BlockConnected(block, *index, chain, params, seat_tracker.Index(), vault_tracker.Index());
        BOOST_REQUIRE(seat_tracker.Synced(index->GetBlockHash()));
        BOOST_REQUIRE(vault_tracker.Synced(index->GetBlockHash()));
        BOOST_REQUIRE(checkpoint_tracker.Synced(index->GetBlockHash()));
    }

    node::FlowMeshHttpsServer::Options Options(uint16_t port = 0) const
    {
        node::FlowMeshHttpsServer::Options options;
        options.cert_file = cert; options.key_file = key;
        options.request_timeout = std::chrono::seconds{2};
        options.port = port;
        return options;
    }
    node::HttpsEndpoint Endpoint(uint16_t port) const
    {
        return {"https://127.0.0.1:" + std::to_string(port), cert, pin};
    }
    //! `connections`, when set, is told of every connection the endpoint admits.
    std::unique_ptr<node::FlowMeshHttpsServer> Serve(ScriptedEndpoint& endpoint, uint16_t port = 0,
                                                     ConnectionObservation* connections = nullptr) const
    {
        auto server{std::make_unique<node::FlowMeshHttpsServer>(Options(port), [&endpoint](const auto& request) { return endpoint.Handle(request); })};
        if (connections) {
            BOOST_REQUIRE(node::FlowMeshHttpsTestAccess::ObserveConnection(*server,
                [connections](bool tcp_nodelay) { connections->Record(tcp_nodelay); }));
        }
        std::string error;
        BOOST_REQUIRE_MESSAGE(server->Start(error), error);
        return server;
    }
    //! A restored outbox holding one possibly delivered cancel under this market's pins.
    flowmesh::Action Seed(const fs::path& path, const uint256& owner) const
    {
        return ClientConnectFixture::SeedRetainedAction(path, market, owner, domain, config);
    }
    //! This market's discovery row, advertising `wait` (and `submit_wait`
    //! on 'submit') when nonzero.
    UniValue MarketRow(std::chrono::milliseconds wait, std::chrono::milliseconds submit_wait = std::chrono::milliseconds{0}) const
    {
        UniValue row{UniValue::VOBJ};
        row.pushKV("domain", domain.GetHex()); row.pushKV("market_id", market.GetHex());
        row.pushKV("base_asset_id", asset.GetHex()); row.pushKV("vault_id", vault.GetHex());
        row.pushKV("execution_config_id", config.GetHex()); row.pushKV("quote_asset", "B3");
        row.pushKV("running", true); row.pushKV("paused", false); row.pushKV("pending_handoff", false);
        row.pushKV("halt", ""); row.pushKV("error", "");
        row.pushKV("next_microblock_sequence", 1); row.pushKV("last_microblock_hash", uint256{}.GetHex());
        if (wait.count()) row.pushKV("action_wait_ms_max", wait.count());
        if (submit_wait.count()) row.pushKV("submit_wait_ms_max", submit_wait.count());
        return row;
    }
    //! A cancel signed by this fixture's trading key under this market's pins.
    flowmesh::Action SignedCancel(const uint64_t sequence) const
    {
        CKey key;
        std::array<unsigned char, 32> secret{};
        secret.back() = 0x5a;
        key.Set(secret.begin(), secret.end(), true);
        flowmesh::Action action;
        action.signer = flowmesh::AccountForKey(XOnlyPubKey{key.GetPubKey()});
        action.sequence = sequence;
        action.type = static_cast<uint8_t>(flowmesh::ActionType::CANCEL_BID);
        BOOST_REQUIRE(flowmesh::SignAction(key, domain, config, action));
        return action;
    }
    static UniValue Receipt(const flowmesh::Action& action, const std::string& state)
    {
        UniValue receipt{UniValue::VOBJ};
        receipt.pushKV("action_id", action.Id().GetHex()); receipt.pushKV("receipt_state", state);
        receipt.pushKV("reason", "scripted " + state);
        return receipt;
    }
    //! A waited 'submit' reply: the acknowledgement in `state`, the embedded
    //! 'action' reply (omitted when null) and the wait_status hint.
    static UniValue WaitedSubmitReply(const flowmesh::Action& action, const std::string& state, UniValue embedded,
                                      const std::string& wait_status)
    {
        auto reply{Receipt(action, state)};
        if (!embedded.isNull()) {
            embedded.pushKV("wait_status", wait_status);
            reply.pushKV("action", std::move(embedded));
        }
        reply.pushKV("wait_status", wait_status);
        return reply;
    }
    /** An 'action' reply proving the action's inclusion in a microblock that
     * this market's seats certified. The client checks inclusion by that
     * certificate under local B3 authority and never re-executes the entry,
     * so the certified genesis entry with this action added suffices. */
    UniValue CertifiedReceipt(const flowmesh::Action& action) const
    {
        const auto& params{m_node.chainman->GetConsensus()};
        const CScript treasury{params.modern_pos->treasury_script.begin(), params.modern_pos->treasury_script.end()};
        node::ChainAnchorPolicy anchors{*m_node.chainman, Consensus::FLOWMESH_ANCHOR_DEPTH};
        flowmesh::AnchorRef anchor;
        int height;
        {
            LOCK(::cs_main);
            height = m_node.chainman->ActiveChain().Height();
            const auto* index{m_node.chainman->ActiveChain()[static_cast<int>(seats.anchor_height)]};
            anchor = {index->nHeight, index->GetBlockHash()};
        }
        const flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(), flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
        flowmesh::ProductionEpochGate gate{domain, market, seats};
        flowmesh::ProductionEntryCheck check;
        const auto built{flowmesh::BuildProductionExecutionEntry(initial, domain, market, seats, gate, 0, 0, {}, anchor,
            {height, std::nullopt, &anchors}, modern::AssetOwnerCommitment(treasury), {}, nullptr, check)};
        BOOST_REQUIRE_MESSAGE(built, flowmesh::ProductionEntryCheckName(check));
        auto entry{built->entry};
        // Entries carry the semantic action only; its identity excludes the credential.
        auto included{action};
        included.credential.clear();
        entry.actions = {included};
        entry.actions_root = flowmesh::ComputeProductionActionsRoot(entry.actions);
        std::vector<flowmesh::IndexedBlsSignature> signatures;
        const auto context{flowmesh::ProductionCertificateContext(entry)};
        for (uint32_t i{0}; i < flowmesh::FlowMeshBlsThreshold(seats.Size()); ++i) {
            const auto signature{flowmesh::SignBlsMicroblockCertificate(secrets[i], context, seats)};
            BOOST_REQUIRE(signature);
            signatures.push_back({i, *signature});
        }
        flowmesh::BlsMicroblockCertificate certificate;
        BOOST_REQUIRE(flowmesh::AssembleProductionEntryCertificate(entry, seats, signatures, certificate) ==
                      flowmesh::BlsCertificateAssemblyCheck::OK);
        const auto payload{flowmesh::EncodeProductionCertifiedPayload({entry, certificate}, seats.Size())};
        BOOST_REQUIRE(payload);
        auto receipt{Receipt(action, "certified_inclusion")};
        receipt.pushKV("microblock_hash", entry.GetHash().GetHex());
        receipt.pushKV("microblock_sequence", entry.sequence);
        receipt.pushKV("certified_payload", HexStr(*payload));
        return receipt;
    }
    /** An opt-in 'action' reply for a microblock whose body really executes
     * `action`: its certified payload plus the outcome evidence that hashes
     * to the entry's result root. Also returns the evidence hex alone. */
    std::pair<UniValue, std::string> OutcomeReceipt(const flowmesh::Action& action, const uint64_t next_sequence = 0) const
    {
        const auto& params{m_node.chainman->GetConsensus()};
        const CScript treasury{params.modern_pos->treasury_script.begin(), params.modern_pos->treasury_script.end()};
        node::ChainAnchorPolicy anchors{*m_node.chainman, Consensus::FLOWMESH_ANCHOR_DEPTH};
        flowmesh::AnchorRef anchor;
        int height;
        {
            LOCK(::cs_main);
            height = m_node.chainman->ActiveChain().Height();
            const auto* index{m_node.chainman->ActiveChain()[static_cast<int>(seats.anchor_height)]};
            anchor = {index->nHeight, index->GetBlockHash()};
        }
        flowmesh::FlowMeshState initial{vault, asset, modern::NativeAsset(), flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
        // The signer's next account sequence before this entry: an action at
        // exactly that sequence consumes it.
        if (next_sequence > 0) flowmesh::test_only::StateFunding::SetNextSequence(initial, action.signer, next_sequence);
        flowmesh::ProductionEpochGate gate{domain, market, seats};
        flowmesh::ProductionEntryCheck check;
        const std::vector<flowmesh::Action> body{action};
        const auto built{flowmesh::BuildProductionExecutionEntry(initial, domain, market, seats, gate, 0, 0, {}, anchor,
            {height, std::nullopt, &anchors}, modern::AssetOwnerCommitment(treasury), body, nullptr, check)};
        BOOST_REQUIRE_MESSAGE(built, flowmesh::ProductionEntryCheckName(check));
        std::vector<flowmesh::IndexedBlsSignature> signatures;
        const auto context{flowmesh::ProductionCertificateContext(built->entry)};
        for (uint32_t i{0}; i < flowmesh::FlowMeshBlsThreshold(seats.Size()); ++i) {
            const auto signature{flowmesh::SignBlsMicroblockCertificate(secrets[i], context, seats)};
            BOOST_REQUIRE(signature);
            signatures.push_back({i, *signature});
        }
        flowmesh::BlsMicroblockCertificate certificate;
        BOOST_REQUIRE(flowmesh::AssembleProductionEntryCertificate(built->entry, seats, signatures, certificate) ==
                      flowmesh::BlsCertificateAssemblyCheck::OK);
        const auto payload{flowmesh::EncodeProductionCertifiedPayload({built->entry, certificate}, seats.Size())};
        BOOST_REQUIRE(payload);
        const auto evidence{flowmesh::EncodeExecutionOutcomeEvidenceV1(built->entry.sequence, built->entry.GetHash(),
            flowmesh::MakeExecutionOutcomePreimageV1(built->result), built->effects)};
        BOOST_REQUIRE(evidence);
        auto receipt{Receipt(action, "certified_inclusion")};
        receipt.pushKV("microblock_hash", built->entry.GetHash().GetHex());
        receipt.pushKV("microblock_sequence", built->entry.sequence);
        receipt.pushKV("certified_payload", HexStr(*payload));
        receipt.pushKV("outcome_evidence", HexStr(*evidence));
        return {receipt, HexStr(*evidence)};
    }
    //! The outbox row Restore reads back for `action`, from a closed journal.
    static UniValue DurableRow(const fs::path& path, const flowmesh::Action& action)
    {
        CDBWrapper db{DBParams{.path=path, .cache_bytes=1 << 20}};
        std::string json;
        BOOST_REQUIRE(db.Read(std::string{"public-client-v1"}, json));
        UniValue root;
        BOOST_REQUIRE(root.read(json));
        for (const auto& row : root["actions"].getValues()) {
            if (row["action_id"].get_str() == action.Id().GetHex()) return row;
        }
        BOOST_FAIL("retained action missing from the outbox");
        return {};
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_client_wait_lane_tests, ClientLaneFixture)

BOOST_AUTO_TEST_CASE(lane_certification_whose_outbox_save_fails_is_returned_unverified)
{
#ifdef WIN32
    BOOST_TEST_MESSAGE("Needs RLIMIT_FSIZE to fail the outbox write");
#else
    const fs::path path{m_path_root / "client"};
    const uint256 owner{*uint256::FromHex(std::string(64, '4'))};
    const auto action{Seed(path, owner)};
    ScriptedEndpoint validator;
    validator.markets.push_back(MarketRow(std::chrono::milliseconds{1000}));
    validator.plain = Receipt(action, "admitted");
    validator.submit = Receipt(action, "admitted");
    validator.waited = CertifiedReceipt(action);
    validator.waited.pushKV("wait_status", "terminal");
    auto server{Serve(validator)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    // Learn the advertised wait, then redeliver the same signed bytes so the
    // endpoint that acknowledged the delivery is known.
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    const auto delivered{client->ActionStatus(market, action.Id(), true)};
    BOOST_REQUIRE_EQUAL(delivered.state, "admitted");
    BOOST_REQUIRE((validator.Counts() == std::array<unsigned, 4>{1, 0, 1, 1}));

    // The waited read proves certified inclusion, but the outbox write that
    // must precede exposing it fails.
    interfaces::FlowMeshActionReceipt waited;
    {
        ScopedFailedFileWrites full_disk;
        waited = client->ActionStatus(market, action.Id(), false, std::chrono::milliseconds{2000});
    }
    // As c10c952's ordinary refresh: never a verified label that is not
    // durable, the failure as its reason, and no second read in this call.
    BOOST_CHECK(!waited.certificate_verified);
    BOOST_CHECK_EQUAL(waited.state, "unknown");
    BOOST_CHECK(waited.reason.find("LevelDB") != std::string::npos);
    BOOST_CHECK((validator.Counts() == std::array<unsigned, 4>{1, 1, 1, 1}));
    const auto retained{client->SavedActions(owner, market)};
    BOOST_REQUIRE_EQUAL(retained.size(), 1U);
    BOOST_CHECK(!retained[0].receipt.certificate_verified);
    BOOST_CHECK_EQUAL(retained[0].receipt.state, "unknown");
    // Still never resent: the no-replay marker stays set in memory.
    BOOST_CHECK(retained[0].previously_certified);
    BOOST_CHECK(retained[0].may_have_been_sent);

    // With writes working, an ordinary read proves the inclusion again and
    // saves it before returning it. A previously certified action never
    // takes the lane.
    {
        std::lock_guard lock{validator.mutex};
        validator.plain = CertifiedReceipt(action);
    }
    const auto proved{client->ActionStatus(market, action.Id(), false, std::chrono::milliseconds{2000})};
    BOOST_CHECK(proved.certificate_verified);
    BOOST_CHECK_EQUAL(proved.state, "certified_inclusion");
    BOOST_CHECK((validator.Counts() == std::array<unsigned, 4>{2, 1, 1, 1}));
    client.reset();
    server->Stop();
    const auto durable{DurableRow(path, action)};
    BOOST_CHECK(durable["previously_certified"].get_bool());
    BOOST_CHECK(durable["may_have_been_sent"].get_bool());
    BOOST_CHECK_EQUAL(durable["receipt"]["receipt_state"].get_str(), "certified_inclusion");
#endif
}

BOOST_AUTO_TEST_CASE(lane_skips_an_endpoint_in_transport_cooldown)
{
    const fs::path path{m_path_root / "client"};
    const uint256 owner{*uint256::FromHex(std::string(64, '4'))};
    const auto action{Seed(path, owner)};
    ScriptedEndpoint validator;
    validator.markets.push_back(MarketRow(std::chrono::milliseconds{1000}));
    validator.plain = validator.submit = validator.waited = Receipt(action, "admitted");
    validator.waited.pushKV("wait_status", "timeout");
    auto server{Serve(validator)};
    const uint16_t port{server->Port()};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(port)}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    BOOST_REQUIRE_EQUAL(client->ActionStatus(market, action.Id(), true).state, "admitted");
    BOOST_REQUIRE((validator.Counts() == std::array<unsigned, 4>{1, 0, 1, 1}));

    // The delivering endpoint stops: an ordinary read fails at transport and
    // starts its 2 s cooldown.
    server->Stop();
    server.reset();
    (void)client->ActionStatus(market, action.Id(), false);
    BOOST_REQUIRE(!client->Status().endpoints[0].transport_available);
    BOOST_REQUIRE_EQUAL(client->Status().endpoints[0].consecutive_failures, 1U);
    // Back within the cooldown: the waited call sends no waited read, only
    // the ordinary one (which, as in c10c952, ignores read cooldowns).
    server = Serve(validator, port);
    std::this_thread::sleep_for(std::chrono::milliseconds{150}); // Refill the automatic read budget.
    const auto during{client->ActionStatus(market, action.Id(), false, std::chrono::milliseconds{2000})};
    BOOST_CHECK_EQUAL(during.state, "admitted");
    BOOST_CHECK((validator.Counts() == std::array<unsigned, 4>{2, 0, 1, 1}));
    // That response ended the cooldown, so the next waited call uses the lane.
    BOOST_REQUIRE(client->Status().endpoints[0].transport_available);
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    (void)client->ActionStatus(market, action.Id(), false, std::chrono::milliseconds{2000});
    BOOST_CHECK((validator.Counts() == std::array<unsigned, 4>{2, 1, 1, 1}));
    client.reset();
    server->Stop();
}

BOOST_AUTO_TEST_CASE(lane_skips_an_endpoint_that_left_it_unanswered_until_it_answers)
{
    const fs::path path{m_path_root / "client"};
    const uint256 owner{*uint256::FromHex(std::string(64, '4'))};
    const auto action{Seed(path, owner)};
    ScriptedEndpoint reads, deliverer;
    for (auto* endpoint : {&reads, &deliverer}) {
        endpoint->markets.push_back(MarketRow(std::chrono::milliseconds{1000}));
        endpoint->plain = endpoint->submit = endpoint->waited = Receipt(action, "admitted");
        endpoint->waited.pushKV("wait_status", "timeout");
    }
    auto reads_server{Serve(reads)};
    auto deliverer_server{Serve(deliverer)};
    const uint16_t deliverer_port{deliverer_server->Port()};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman,
        {Endpoint(reads_server->Port()), Endpoint(deliverer_port)}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    // Deliver through the second endpoint (learning its advertisement), then
    // select the first for ordinary reads again.
    BOOST_REQUIRE_MESSAGE(client->Connect(Endpoint(deliverer_port).url, error), error);
    BOOST_REQUIRE_EQUAL(client->ActionStatus(market, action.Id(), true).state, "admitted");
    BOOST_REQUIRE_MESSAGE(client->Connect(Endpoint(reads_server->Port()).url, error), error);
    BOOST_REQUIRE((deliverer.Counts() == std::array<unsigned, 4>{1, 0, 1, 1}));
    BOOST_REQUIRE((reads.Counts() == std::array<unsigned, 4>{0, 0, 0, 1}));

    // The delivering endpoint goes silent: the lane gets no response, and
    // the ordinary read in the same call goes to the selected endpoint.
    deliverer_server->Stop();
    deliverer_server.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    BOOST_CHECK_EQUAL(client->ActionStatus(market, action.Id(), false, std::chrono::milliseconds{2000}).state, "admitted");
    BOOST_CHECK((reads.Counts() == std::array<unsigned, 4>{1, 0, 0, 1}));
    // No ordinary request went there, so it is not in transport cooldown.
    BOOST_REQUIRE_EQUAL(client->Status().endpoints[1].retry_after_ms, 0);

    // Back, but nothing has recorded a successful response from it since:
    // the waited call does not try the lane there again.
    deliverer_server = Serve(deliverer, deliverer_port);
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    BOOST_CHECK_EQUAL(client->ActionStatus(market, action.Id(), false, std::chrono::milliseconds{2000}).state, "admitted");
    BOOST_CHECK((deliverer.Counts() == std::array<unsigned, 4>{1, 0, 1, 1}));
    BOOST_CHECK((reads.Counts() == std::array<unsigned, 4>{2, 0, 0, 1}));

    // A successful response from it (here an explicit probe) re-enables it.
    BOOST_REQUIRE_MESSAGE(client->Connect(Endpoint(deliverer_port).url, error), error);
    BOOST_REQUIRE((deliverer.Counts() == std::array<unsigned, 4>{1, 0, 1, 2}));
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    (void)client->ActionStatus(market, action.Id(), false, std::chrono::milliseconds{2000});
    BOOST_CHECK((deliverer.Counts() == std::array<unsigned, 4>{1, 1, 1, 2}));
    client.reset();
    reads_server->Stop();
    deliverer_server->Stop();
}

BOOST_AUTO_TEST_CASE(lane_reply_leaves_the_active_endpoint_to_ordinary_reads)
{
    const fs::path path{m_path_root / "client"};
    const uint256 owner{*uint256::FromHex(std::string(64, '4'))};
    const auto action{Seed(path, owner)};
    ScriptedEndpoint reads, deliverer;
    for (auto* endpoint : {&reads, &deliverer}) {
        endpoint->markets.push_back(MarketRow(std::chrono::milliseconds{1000}));
        endpoint->plain = endpoint->submit = endpoint->waited = Receipt(action, "admitted");
        endpoint->waited.pushKV("wait_status", "timeout");
    }
    auto reads_server{Serve(reads)};
    auto deliverer_server{Serve(deliverer)};
    const auto reads_url{Endpoint(reads_server->Port()).url};
    const auto deliverer_url{Endpoint(deliverer_server->Port()).url};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman,
        {Endpoint(reads_server->Port()), Endpoint(deliverer_server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_MESSAGE(client->Connect(deliverer_url, error), error);
    BOOST_REQUIRE_EQUAL(client->ActionStatus(market, action.Id(), true).state, "admitted");
    BOOST_REQUIRE_MESSAGE(client->Connect(reads_url, error), error);
    BOOST_REQUIRE_EQUAL(client->Status().active_endpoint, reads_url);
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    // The lane's reply from the delivering endpoint is applied, yet ordinary
    // reads and submits still use the selected endpoint, which status keeps
    // naming as active.
    const auto waited{client->ActionStatus(market, action.Id(), false, std::chrono::milliseconds{2000})};
    BOOST_CHECK_EQUAL(waited.endpoint, deliverer_url);
    BOOST_CHECK((deliverer.Counts() == std::array<unsigned, 4>{1, 1, 1, 1}));
    BOOST_CHECK((reads.Counts() == std::array<unsigned, 4>{0, 0, 0, 1}));
    const auto status{client->Status()};
    BOOST_CHECK_EQUAL(status.active_endpoint, reads_url);
    BOOST_CHECK_EQUAL(status.selected_endpoint, reads_url);
    BOOST_CHECK(status.endpoints[1].available);
    BOOST_CHECK(status.endpoints[1].transport_available);
    client.reset();
    reads_server->Stop();
    deliverer_server->Stop();
}

BOOST_AUTO_TEST_CASE(lane_needs_a_delivery_acknowledged_in_this_process)
{
    const fs::path path{m_path_root / "client"};
    const uint256 owner{*uint256::FromHex(std::string(64, '4'))};
    const auto action{Seed(path, owner)};
    ScriptedEndpoint validator;
    validator.markets.push_back(MarketRow(std::chrono::milliseconds{1000}));
    validator.plain = validator.submit = validator.waited = Receipt(action, "admitted");
    validator.waited.pushKV("wait_status", "timeout");
    auto server{Serve(validator)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    // Every other condition holds: the endpoint advertises the wait, the
    // market is established locally, and the restored action may have been
    // sent, has no verified or previous inclusion and no refusal. Only the
    // acknowledging endpoint is unknown after a restart: an ordinary read.
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    const auto restored{client->ActionStatus(market, action.Id(), false, std::chrono::milliseconds{2000})};
    BOOST_CHECK_EQUAL(restored.state, "admitted");
    BOOST_CHECK((validator.Counts() == std::array<unsigned, 4>{1, 0, 0, 1}));
    // The endpoint acknowledges a redelivery of the same signed bytes; the
    // same waited call now takes the lane.
    BOOST_REQUIRE_EQUAL(client->ActionStatus(market, action.Id(), true).state, "admitted");
    BOOST_REQUIRE((validator.Counts() == std::array<unsigned, 4>{2, 0, 1, 1}));
    std::this_thread::sleep_for(std::chrono::milliseconds{150}); // Refill the automatic read budget.
    const auto delivered{client->ActionStatus(market, action.Id(), false, std::chrono::milliseconds{2000})};
    BOOST_CHECK_EQUAL(delivered.state, "admitted");
    BOOST_CHECK((validator.Counts() == std::array<unsigned, 4>{2, 1, 1, 1}));
    client.reset();
    server->Stop();
}

BOOST_AUTO_TEST_CASE(lane_borrows_the_warm_ordinary_connection_for_its_first_waited_read)
{
    const fs::path path{m_path_root / "client"};
    const uint256 owner{*uint256::FromHex(std::string(64, '4'))};
    const auto action{Seed(path, owner)};
    ScriptedEndpoint validator;
    validator.markets.push_back(MarketRow(std::chrono::milliseconds{1000}));
    validator.plain = validator.submit = Receipt(action, "admitted");
    validator.waited = CertifiedReceipt(action);
    validator.waited.pushKV("wait_status", "terminal");
    ConnectionObservation connections;
    auto server{Serve(validator, 0, &connections)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    BOOST_REQUIRE_EQUAL(client->ActionStatus(market, action.Id(), true).state, "admitted");
    BOOST_REQUIRE_EQUAL(connections.Count(), 1U);
    std::this_thread::sleep_for(std::chrono::milliseconds{150}); // Refill the automatic read budget.

    // The lane has never connected; the ordinary connection just delivered
    // the action. The waited read carries it: the endpoint sees no second
    // connection and the read performs no TLS handshake.
    std::vector<UniValue> spans;
    interfaces::FlowMeshActionReceipt waited;
    {
        TimingCapture timing;
        waited = client->ActionStatus(market, action.Id(), false, std::chrono::milliseconds{2000});
        spans = timing.Stop("client_action_wait");
    }
    BOOST_CHECK(waited.certificate_verified);
    BOOST_CHECK_EQUAL(waited.state, "certified_inclusion");
    BOOST_CHECK((validator.Counts() == std::array<unsigned, 4>{1, 1, 1, 1}));
    BOOST_CHECK_EQUAL(connections.Count(), 1U);
    BOOST_REQUIRE_EQUAL(spans.size(), 1U);
    BOOST_CHECK_EQUAL(spans[0]["borrowed"].getInt<uint64_t>(), 1U);
    BOOST_CHECK_EQUAL(spans[0]["connection_reused"].getInt<uint64_t>(), 1U);
    BOOST_CHECK_EQUAL(spans[0]["tls_handshake_performed"].getInt<uint64_t>(), 0U);
    BOOST_CHECK_EQUAL(spans[0]["handed_back"].getInt<uint64_t>(), 1U);
    BOOST_CHECK_EQUAL(spans[0]["result"].get_str(), "certified");
    // Handed back: ordinary work keeps using the same connection.
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    BOOST_CHECK((validator.Counts() == std::array<unsigned, 4>{1, 1, 1, 2}));
    BOOST_CHECK_EQUAL(connections.Count(), 1U);
    client.reset();
    server->Stop();
}

BOOST_AUTO_TEST_CASE(lane_keeps_the_carried_connection_when_ordinary_work_opened_its_own)
{
    const fs::path path{m_path_root / "client"};
    const uint256 owner{*uint256::FromHex(std::string(64, '4'))};
    const auto action{Seed(path, owner)};
    ScriptedEndpoint validator;
    validator.markets.push_back(MarketRow(std::chrono::milliseconds{1000}));
    validator.plain = validator.submit = validator.waited = Receipt(action, "admitted");
    validator.waited.pushKV("wait_status", "timeout");
    RequestGate gate;
    std::atomic<bool> hold{true};
    validator.before = [&](const std::string& method, const UniValue& params) {
        if (method == "action" && params.exists("wait_ms") && hold.exchange(false)) gate.Hold();
    };
    ConnectionObservation connections;
    auto server{Serve(validator, 0, &connections)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    BOOST_REQUIRE_EQUAL(client->ActionStatus(market, action.Id(), true).state, "admitted");
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    TimingCapture timing;
    std::thread waiter{[&] { (void)client->ActionStatus(market, action.Id(), false, std::chrono::milliseconds{2000}); }};
    BOOST_REQUIRE(gate.AwaitEntered());
    // While the lane carries the connection, ordinary work is not blocked
    // by the wait and opens its own connection.
    BOOST_CHECK_EQUAL(client->Markets(std::nullopt).size(), 1U);
    BOOST_CHECK_EQUAL(connections.Count(), 2U);
    gate.Release();
    waiter.join();
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    // Both sessions are warm now: the lane keeps what it carried and the
    // next waited read uses it without borrowing or a handshake.
    (void)client->ActionStatus(market, action.Id(), false, std::chrono::milliseconds{2000});
    BOOST_CHECK_EQUAL(client->Markets(std::nullopt).size(), 1U);
    const auto spans{timing.Stop("client_action_wait")};
    BOOST_CHECK_EQUAL(connections.Count(), 2U);
    BOOST_CHECK((validator.Counts() == std::array<unsigned, 4>{1, 2, 1, 3}));
    BOOST_REQUIRE_EQUAL(spans.size(), 2U);
    BOOST_CHECK_EQUAL(spans[0]["borrowed"].getInt<uint64_t>(), 1U);
    BOOST_CHECK_EQUAL(spans[0]["handed_back"].getInt<uint64_t>(), 0U);
    BOOST_CHECK_EQUAL(spans[1]["borrowed"].getInt<uint64_t>(), 0U);
    BOOST_CHECK_EQUAL(spans[1]["connection_reused"].getInt<uint64_t>(), 1U);
    BOOST_CHECK_EQUAL(spans[1]["tls_handshake_performed"].getInt<uint64_t>(), 0U);
    client.reset();
    server->Stop();
}

BOOST_AUTO_TEST_CASE(lane_hands_back_no_connection_that_an_explicit_reconnect_reset)
{
    const fs::path path{m_path_root / "client"};
    const uint256 owner{*uint256::FromHex(std::string(64, '4'))};
    const auto action{Seed(path, owner)};
    ScriptedEndpoint validator;
    validator.markets.push_back(MarketRow(std::chrono::milliseconds{1000}));
    validator.plain = validator.submit = validator.waited = Receipt(action, "admitted");
    validator.waited.pushKV("wait_status", "timeout");
    RequestGate gate;
    std::atomic<bool> hold{true};
    validator.before = [&](const std::string& method, const UniValue& params) {
        if (method == "action" && params.exists("wait_ms") && hold.exchange(false)) gate.Hold();
    };
    ConnectionObservation connections;
    auto server{Serve(validator, 0, &connections)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    BOOST_REQUIRE_EQUAL(client->ActionStatus(market, action.Id(), true).state, "admitted");
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    TimingCapture timing;
    std::thread waiter{[&] { (void)client->ActionStatus(market, action.Id(), false, std::chrono::milliseconds{2000}); }};
    BOOST_REQUIRE(gate.AwaitEntered());
    // An explicit reconnect revalidates trust while the lane carries the
    // connection verified before it: that one is never handed back.
    BOOST_CHECK_EQUAL(client->Reconnect().status, "reachable");
    BOOST_CHECK_EQUAL(connections.Count(), 2U);
    gate.Release();
    waiter.join();
    // Ordinary work keeps the reconnect's connection.
    BOOST_CHECK_EQUAL(client->Markets(std::nullopt).size(), 1U);
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    // The lane drops what it carried before its next request, then carries
    // the reconnect's verified connection.
    (void)client->ActionStatus(market, action.Id(), false, std::chrono::milliseconds{2000});
    const auto spans{timing.Stop("client_action_wait")};
    BOOST_CHECK_EQUAL(connections.Count(), 2U);
    BOOST_REQUIRE_EQUAL(spans.size(), 2U);
    BOOST_CHECK_EQUAL(spans[0]["borrowed"].getInt<uint64_t>(), 1U);
    BOOST_CHECK_EQUAL(spans[0]["handed_back"].getInt<uint64_t>(), 0U);
    BOOST_CHECK_EQUAL(spans[1]["borrowed"].getInt<uint64_t>(), 1U);
    BOOST_CHECK_EQUAL(spans[1]["connection_reused"].getInt<uint64_t>(), 1U);
    BOOST_CHECK_EQUAL(spans[1]["handed_back"].getInt<uint64_t>(), 1U);
    client.reset();
    server->Stop();
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(flowmesh_client_submit_wait_tests, ClientLaneFixture)

BOOST_AUTO_TEST_CASE(waited_submit_without_a_submit_advert_chains_one_waited_read)
{
    const fs::path path{m_path_root / "client"};
    const auto action{SignedCancel(4)};
    // An f8a3432 endpoint: the action wait only.
    ScriptedEndpoint validator;
    validator.markets.push_back(MarketRow(std::chrono::milliseconds{1000}));
    validator.submit = Receipt(action, "admitted");
    validator.waited = CertifiedReceipt(action);
    validator.waited.pushKV("wait_status", "terminal");
    auto server{Serve(validator)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    // The ordinary submission, then one waited read of the exact action at
    // the endpoint that acknowledged it: the verified inclusion in one call.
    const auto receipt{client->Submit(market, action, std::chrono::milliseconds{2000})};
    BOOST_CHECK(receipt.certificate_verified);
    BOOST_CHECK_EQUAL(receipt.state, "certified_inclusion");
    BOOST_CHECK((validator.Counts() == std::array<unsigned, 4>{0, 1, 1, 1}));
    {
        std::lock_guard lock{validator.mutex};
        BOOST_CHECK_EQUAL(validator.waited_submits, 0U);
        BOOST_REQUIRE_EQUAL(validator.submitted.size(), 1U);
        BOOST_CHECK(!validator.submitted[0].exists("wait_ms"));
        BOOST_CHECK_EQUAL(validator.submitted[0]["action_hex"].get_str(), HexStr(*flowmesh::EncodeProductionActionPayload(action)));
    }
    const auto saved{client->SavedActions(action.signer, market)};
    BOOST_REQUIRE_EQUAL(saved.size(), 1U);
    BOOST_CHECK(saved[0].receipt.certificate_verified);
    BOOST_CHECK(saved[0].may_have_been_sent);
    client.reset();
    server->Stop();
    const auto durable{DurableRow(path, action)};
    BOOST_CHECK(durable["may_have_been_sent"].get_bool());
    BOOST_CHECK_EQUAL(durable["receipt"]["receipt_state"].get_str(), "certified_inclusion");
}

BOOST_AUTO_TEST_CASE(waited_submit_without_any_wait_advert_is_one_plain_submission)
{
    const fs::path path{m_path_root / "client"};
    const auto action{SignedCancel(4)};
    // A c10c952 endpoint: no waits at all.
    ScriptedEndpoint validator;
    validator.markets.push_back(MarketRow(std::chrono::milliseconds{0}));
    validator.submit = Receipt(action, "admitted");
    validator.waited = CertifiedReceipt(action);
    auto server{Serve(validator)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    const auto receipt{client->Submit(market, action, std::chrono::milliseconds{2000})};
    BOOST_CHECK(!receipt.certificate_verified);
    BOOST_CHECK_EQUAL(receipt.state, "admitted");
    BOOST_CHECK((validator.Counts() == std::array<unsigned, 4>{0, 0, 1, 1}));
    // A definite refusal never waits either, even where waits are offered.
    const auto refused_action{SignedCancel(5)};
    {
        std::lock_guard lock{validator.mutex};
        validator.markets = UniValue{UniValue::VARR};
        validator.markets.push_back(MarketRow(std::chrono::milliseconds{1000}));
        validator.submit = Receipt(refused_action, "rejected");
    }
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    const auto refused{client->Submit(market, refused_action, std::chrono::milliseconds{2000})};
    BOOST_CHECK_EQUAL(refused.state, "rejected");
    BOOST_CHECK((validator.Counts() == std::array<unsigned, 4>{0, 0, 2, 2}));
    client.reset();
    server->Stop();
}

BOOST_AUTO_TEST_CASE(waited_submit_is_one_request_that_verifies_certified_inclusion)
{
    const fs::path path{m_path_root / "client"};
    const auto action{SignedCancel(4)};
    ScriptedEndpoint validator;
    validator.markets.push_back(MarketRow(std::chrono::milliseconds{1000}, std::chrono::milliseconds{1000}));
    validator.waited_submit = WaitedSubmitReply(action, "queued", CertifiedReceipt(action), "terminal");
    ConnectionObservation connections;
    auto server{Serve(validator, 0, &connections)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    std::vector<UniValue> spans;
    interfaces::FlowMeshActionReceipt receipt;
    {
        TimingCapture timing;
        receipt = client->Submit(market, action, std::chrono::milliseconds{2000});
        spans = timing.Stop("client_submit_wait");
    }
    // One request: the submission carried the wait, and its reply's
    // certificate verified under local B3 authority.
    BOOST_CHECK(receipt.certificate_verified);
    BOOST_CHECK_EQUAL(receipt.state, "certified_inclusion");
    BOOST_CHECK((validator.Counts() == std::array<unsigned, 4>{0, 0, 0, 1}));
    BOOST_CHECK_EQUAL(connections.Count(), 1U);
    {
        std::lock_guard lock{validator.mutex};
        BOOST_CHECK_EQUAL(validator.waited_submits, 1U);
        BOOST_REQUIRE_EQUAL(validator.submitted.size(), 1U);
        const auto& sent{validator.submitted[0]};
        BOOST_CHECK_EQUAL(sent["action_hex"].get_str(), HexStr(*flowmesh::EncodeProductionActionPayload(action)));
        BOOST_CHECK_EQUAL(sent["action_id"].get_str(), action.Id().GetHex());
        // The smaller of the request and the endpoint's advertised maximum.
        BOOST_CHECK_EQUAL(sent["wait_ms"].getInt<int64_t>(), 1000);
        // No outcome evidence is advertised here, so none is asked for.
        BOOST_CHECK(!sent.exists("outcome"));
    }
    BOOST_REQUIRE_EQUAL(spans.size(), 1U);
    BOOST_CHECK_EQUAL(spans[0]["result"].get_str(), "certified");
    BOOST_CHECK_EQUAL(spans[0]["wait_status"].get_str(), "terminal");
    // The lane carried the ordinary connection the discovery read opened.
    BOOST_CHECK_EQUAL(spans[0]["borrowed"].getInt<uint64_t>(), 1U);
    BOOST_CHECK_EQUAL(spans[0]["tls_handshake_performed"].getInt<uint64_t>(), 0U);
    BOOST_CHECK_EQUAL(spans[0]["handed_back"].getInt<uint64_t>(), 1U);
    const auto saved{client->SavedActions(action.signer, market)};
    BOOST_REQUIRE_EQUAL(saved.size(), 1U);
    BOOST_CHECK(saved[0].receipt.certificate_verified);
    BOOST_CHECK(saved[0].may_have_been_sent);
    BOOST_CHECK(saved[0].previously_certified);
    client.reset();
    server->Stop();
    const auto durable{DurableRow(path, action)};
    BOOST_CHECK(durable["may_have_been_sent"].get_bool());
    BOOST_CHECK(durable["previously_certified"].get_bool());
    BOOST_CHECK_EQUAL(durable["receipt"]["receipt_state"].get_str(), "certified_inclusion");
}

BOOST_AUTO_TEST_CASE(waited_submit_asks_for_outcome_evidence_only_where_offered)
{
    const fs::path path{m_path_root / "client"};
    const auto action{SignedCancel(4)};
    ScriptedEndpoint validator;
    auto row{MarketRow(std::chrono::milliseconds{1000}, std::chrono::milliseconds{1000})};
    row.pushKV("action_outcome_v1", true);
    validator.markets.push_back(row);
    validator.waited_submit = WaitedSubmitReply(action, "queued", OutcomeReceipt(action).first, "terminal");
    auto server{Serve(validator)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    const auto receipt{client->Submit(market, action, std::chrono::milliseconds{2000})};
    BOOST_CHECK(receipt.certificate_verified);
    BOOST_REQUIRE(receipt.outcome_verified);
    BOOST_REQUIRE(receipt.outcome);
    BOOST_CHECK_EQUAL(receipt.outcome->evidence, "certified_result_preimage");
    std::lock_guard lock{validator.mutex};
    BOOST_REQUIRE_EQUAL(validator.submitted.size(), 1U);
    BOOST_CHECK(validator.submitted[0]["outcome"].get_bool());
    client.reset();
    server->Stop();
}

BOOST_AUTO_TEST_CASE(waited_submit_refused_as_an_unknown_field_resends_the_exact_bytes_without_wait)
{
    const fs::path path{m_path_root / "client"};
    const auto action{SignedCancel(4)};
    ScriptedEndpoint validator;
    // A downgraded endpoint that still advertises the submit wait (a stale
    // advertisement), and no action wait, so nothing else follows.
    validator.markets.push_back(MarketRow(std::chrono::milliseconds{0}, std::chrono::milliseconds{1000}));
    validator.reject_submit_wait = true;
    validator.submit = Receipt(action, "rejected");
    auto server{Serve(validator)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    const auto receipt{client->Submit(market, action, std::chrono::milliseconds{2000})};
    // The refused field admitted nothing; the same bytes went again without
    // it. The waited attempt might have been delivered, so the second
    // endpoint answer's refusal is not definite.
    BOOST_CHECK_EQUAL(receipt.state, "unknown");
    BOOST_CHECK(!receipt.certificate_verified);
    {
        std::lock_guard lock{validator.mutex};
        BOOST_CHECK_EQUAL(validator.waited_submits, 1U);
        BOOST_CHECK_EQUAL(validator.submits, 1U);
        BOOST_REQUIRE_EQUAL(validator.submitted.size(), 2U);
        BOOST_CHECK(validator.submitted[0].exists("wait_ms"));
        BOOST_CHECK(!validator.submitted[1].exists("wait_ms"));
        BOOST_CHECK_EQUAL(validator.submitted[0]["action_hex"].get_str(), validator.submitted[1]["action_hex"].get_str());
        validator.submit = Receipt(SignedCancel(5), "admitted");
    }
    // The capability is withdrawn: the next submission carries no wait.
    const auto next{client->Submit(market, SignedCancel(5), std::chrono::milliseconds{2000})};
    BOOST_CHECK_EQUAL(next.state, "admitted");
    std::lock_guard lock{validator.mutex};
    BOOST_CHECK_EQUAL(validator.waited_submits, 1U);
    BOOST_CHECK_EQUAL(validator.submits, 2U);
    client.reset();
    server->Stop();
}

BOOST_AUTO_TEST_CASE(waited_submit_without_a_reply_stays_unknown_and_is_never_resent_to_that_endpoint)
{
    const fs::path path{m_path_root / "client"};
    const auto action{SignedCancel(4)};
    ScriptedEndpoint validator;
    validator.markets.push_back(MarketRow(std::chrono::milliseconds{1000}, std::chrono::milliseconds{1000}));
    validator.plain = Receipt(action, "admitted");
    validator.waited_submit = WaitedSubmitReply(action, "queued", CertifiedReceipt(action), "terminal");
    // The endpoint holds the waited submission past its 2 s request
    // deadline: it may have admitted it, and no reply is ever written.
    validator.before = [](const std::string& method, const UniValue& params) {
        if (method == "submit" && params.exists("wait_ms")) std::this_thread::sleep_for(std::chrono::milliseconds{2300});
    };
    auto server{Serve(validator)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    const auto start{std::chrono::steady_clock::now()};
    const auto receipt{client->Submit(market, action, std::chrono::milliseconds{2000})};
    BOOST_CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{5});
    BOOST_CHECK_EQUAL(receipt.state, "unknown");
    BOOST_CHECK(!receipt.certificate_verified);
    BOOST_CHECK(!receipt.reason.empty());
    {
        std::lock_guard lock{validator.mutex};
        BOOST_CHECK_EQUAL(validator.waited_submits, 1U);
        BOOST_CHECK_EQUAL(validator.submits, 0U);
    }
    // Retained, possibly delivered, visible, and still blocking a different
    // instruction for its account sequence.
    const auto saved{client->SavedActions(action.signer, market)};
    BOOST_REQUIRE_EQUAL(saved.size(), 1U);
    BOOST_CHECK(saved[0].may_have_been_sent);
    BOOST_CHECK_EQUAL(saved[0].receipt.state, "unknown");
    auto other{SignedCancel(4)};
    other.type = static_cast<uint8_t>(flowmesh::ActionType::CANCEL_ASK);
    {
        CKey key;
        std::array<unsigned char, 32> secret{};
        secret.back() = 0x5a;
        key.Set(secret.begin(), secret.end(), true);
        BOOST_REQUIRE(flowmesh::SignAction(key, domain, config, other));
    }
    BOOST_REQUIRE(other.Id() != action.Id());
    const auto blocked{client->Submit(market, other, std::chrono::milliseconds{2000})};
    BOOST_CHECK_EQUAL(blocked.state, "rejected");
    BOOST_CHECK(blocked.reason.find("A different retained action already uses this account sequence") != std::string::npos);
    // The exact action resolves by an ordinary status read, not a resend.
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    BOOST_CHECK_EQUAL(client->ActionStatus(market, action.Id(), false).state, "admitted");
    {
        std::lock_guard lock{validator.mutex};
        BOOST_CHECK_EQUAL(validator.waited_submits, 1U);
        BOOST_CHECK_EQUAL(validator.submits, 0U);
    }
    client.reset();
    server->Stop();
    BOOST_CHECK(DurableRow(path, action)["may_have_been_sent"].get_bool());
}

BOOST_AUTO_TEST_CASE(waited_submit_without_a_reply_resends_the_exact_bytes_only_to_the_other_endpoint)
{
    const fs::path path{m_path_root / "client"};
    const auto action{SignedCancel(4)};
    ScriptedEndpoint silent, other;
    silent.markets.push_back(MarketRow(std::chrono::milliseconds{0}, std::chrono::milliseconds{1000}));
    silent.before = [](const std::string& method, const UniValue& params) {
        if (method == "submit" && params.exists("wait_ms")) std::this_thread::sleep_for(std::chrono::milliseconds{2300});
    };
    other.markets.push_back(MarketRow(std::chrono::milliseconds{0}));
    other.submit = Receipt(action, "admitted");
    auto silent_server{Serve(silent)};
    auto other_server{Serve(other)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman,
        {Endpoint(silent_server->Port()), Endpoint(other_server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    const auto receipt{client->Submit(market, action, std::chrono::milliseconds{2000})};
    // The ordinary failover, minus the endpoint that did not answer.
    BOOST_CHECK_EQUAL(receipt.state, "admitted");
    {
        std::lock_guard lock{silent.mutex};
        BOOST_CHECK_EQUAL(silent.waited_submits, 1U);
        BOOST_CHECK_EQUAL(silent.submits, 0U);
    }
    std::lock_guard lock{other.mutex};
    BOOST_CHECK_EQUAL(other.waited_submits, 0U);
    BOOST_REQUIRE_EQUAL(other.submits, 1U);
    BOOST_CHECK_EQUAL(other.submitted[0]["action_hex"].get_str(), HexStr(*flowmesh::EncodeProductionActionPayload(action)));
    client.reset();
    silent_server->Stop();
    other_server->Stop();
}

BOOST_AUTO_TEST_CASE(waited_submit_row_is_absent_from_the_saved_view_and_never_resent_until_submit_returns)
{
    const fs::path path{m_path_root / "client"};
    const auto action{SignedCancel(4)};
    ScriptedEndpoint validator;
    validator.markets.push_back(MarketRow(std::chrono::milliseconds{1000}, std::chrono::milliseconds{1000}));
    validator.submit = Receipt(action, "admitted");
    validator.waited_submit = WaitedSubmitReply(action, "queued", CertifiedReceipt(action), "terminal");
    RequestGate gate;
    validator.before = [&](const std::string& method, const UniValue& params) {
        if (method == "submit" && params.exists("wait_ms")) gate.Hold();
    };
    auto server{Serve(validator)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    interfaces::FlowMeshActionReceipt receipt;
    std::thread submitter{[&] { receipt = client->Submit(market, action, std::chrono::milliseconds{2000}); }};
    BOOST_REQUIRE(gate.AwaitEntered());
    // In flight: the saved view answers at once, without the row, exactly
    // as while an ordinary submission's network call is in flight.
    const auto start{std::chrono::steady_clock::now()};
    BOOST_CHECK(client->SavedActions(action.signer, market).empty());
    BOOST_CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds{500});
    // The same exact action is not sent again meanwhile.
    const auto again{client->Submit(market, action)};
    BOOST_CHECK_EQUAL(again.state, "unknown");
    BOOST_CHECK(again.reason.find("in flight") != std::string::npos);
    // A different instruction for the same account sequence is refused.
    auto other{action};
    other.type = static_cast<uint8_t>(flowmesh::ActionType::CANCEL_ASK);
    {
        CKey key;
        std::array<unsigned char, 32> secret{};
        secret.back() = 0x5a;
        key.Set(secret.begin(), secret.end(), true);
        BOOST_REQUIRE(flowmesh::SignAction(key, domain, config, other));
    }
    const auto blocked{client->Submit(market, other)};
    BOOST_CHECK_EQUAL(blocked.state, "rejected");
    BOOST_CHECK(blocked.reason.find("A different retained action already uses this account sequence") != std::string::npos);
    gate.Release();
    submitter.join();
    BOOST_CHECK(receipt.certificate_verified);
    const auto saved{client->SavedActions(action.signer, market)};
    BOOST_REQUIRE_EQUAL(saved.size(), 1U);
    BOOST_CHECK(saved[0].receipt.certificate_verified);
    std::lock_guard lock{validator.mutex};
    BOOST_CHECK_EQUAL(validator.waited_submits, 1U);
    BOOST_CHECK_EQUAL(validator.submits, 0U);
    client.reset();
    server->Stop();
}

BOOST_AUTO_TEST_CASE(waited_submit_refusal_after_a_concurrent_observation_is_not_definite)
{
    const fs::path path{m_path_root / "client"};
    const auto action{SignedCancel(4)};
    ScriptedEndpoint validator;
    validator.markets.push_back(MarketRow(std::chrono::milliseconds{0}, std::chrono::milliseconds{1000}));
    validator.plain = Receipt(action, "admitted");
    validator.waited_submit = WaitedSubmitReply(action, "rejected", UniValue{}, "none");
    RequestGate gate;
    validator.before = [&](const std::string& method, const UniValue& params) {
        if (method == "submit" && params.exists("wait_ms")) gate.Hold();
    };
    auto server{Serve(validator)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    interfaces::FlowMeshActionReceipt receipt;
    std::thread submitter{[&] { receipt = client->Submit(market, action, std::chrono::milliseconds{2000}); }};
    BOOST_REQUIRE(gate.AwaitEntered());
    // A status read during the flight observes the action admitted.
    BOOST_CHECK_EQUAL(client->ActionStatus(market, action.Id(), false).state, "admitted");
    gate.Release();
    submitter.join();
    // One endpoint's later refusal never undoes that observation.
    BOOST_CHECK_EQUAL(receipt.state, "admitted");
    BOOST_CHECK(receipt.reason.find("rejected") != std::string::npos);
    auto other{action};
    other.type = static_cast<uint8_t>(flowmesh::ActionType::CANCEL_ASK);
    {
        CKey key;
        std::array<unsigned char, 32> secret{};
        secret.back() = 0x5a;
        key.Set(secret.begin(), secret.end(), true);
        BOOST_REQUIRE(flowmesh::SignAction(key, domain, config, other));
    }
    BOOST_CHECK_EQUAL(client->Submit(market, other).state, "rejected");
    // Without a concurrent observation, a refusal of the first delivery is
    // definite, exactly as for an ordinary submission.
    const auto fresh{SignedCancel(5)};
    {
        std::lock_guard lock{validator.mutex};
        validator.waited_submit = WaitedSubmitReply(fresh, "rejected", UniValue{}, "none");
    }
    // The gate stays released: nothing is held from here on.
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    BOOST_CHECK_EQUAL(client->Submit(market, fresh, std::chrono::milliseconds{2000}).state, "rejected");
    client.reset();
    server->Stop();
}

BOOST_AUTO_TEST_CASE(waited_submit_certification_whose_outbox_save_fails_is_returned_unverified)
{
#ifdef WIN32
    BOOST_TEST_MESSAGE("Needs RLIMIT_FSIZE to fail the outbox write");
#else
    const fs::path path{m_path_root / "client"};
    const auto action{SignedCancel(4)};
    ScriptedEndpoint validator;
    validator.markets.push_back(MarketRow(std::chrono::milliseconds{1000}, std::chrono::milliseconds{1000}));
    validator.waited_submit = WaitedSubmitReply(action, "queued", CertifiedReceipt(action), "terminal");
    // The write-ahead record is durable before the request leaves; writes
    // fail from then on, so saving the verified inclusion fails.
    std::optional<ScopedFailedFileWrites> full_disk;
    validator.before = [&](const std::string& method, const UniValue& params) {
        if (method == "submit" && params.exists("wait_ms")) full_disk.emplace();
    };
    auto server{Serve(validator)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    const auto receipt{client->Submit(market, action, std::chrono::milliseconds{2000})};
    full_disk.reset();
    // Never a verified label that is not durable.
    BOOST_CHECK(!receipt.certificate_verified);
    BOOST_CHECK_EQUAL(receipt.state, "unknown");
    BOOST_CHECK(receipt.reason.find("LevelDB") != std::string::npos);
    const auto saved{client->SavedActions(action.signer, market)};
    BOOST_REQUIRE_EQUAL(saved.size(), 1U);
    BOOST_CHECK(!saved[0].receipt.certificate_verified);
    BOOST_CHECK(saved[0].may_have_been_sent);
    // Still never resent: the no-replay marker stays set in memory.
    BOOST_CHECK(saved[0].previously_certified);
    client.reset();
    server->Stop();
    BOOST_CHECK(DurableRow(path, action)["may_have_been_sent"].get_bool());
#endif
}

BOOST_AUTO_TEST_CASE(waited_submit_the_endpoint_did_not_wait_for_chains_one_waited_read)
{
    const fs::path path{m_path_root / "client"};
    const auto action{SignedCancel(4)};
    ScriptedEndpoint validator;
    validator.markets.push_back(MarketRow(std::chrono::milliseconds{1000}, std::chrono::milliseconds{1000}));
    // Every wait slot was taken: admitted, but not waited for.
    validator.waited_submit = WaitedSubmitReply(action, "queued", Receipt(action, "queued"), "busy");
    validator.waited = CertifiedReceipt(action);
    validator.waited.pushKV("wait_status", "terminal");
    auto server{Serve(validator)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    const auto receipt{client->Submit(market, action, std::chrono::milliseconds{2000})};
    BOOST_CHECK(receipt.certificate_verified);
    BOOST_CHECK((validator.Counts() == std::array<unsigned, 4>{0, 1, 0, 1}));
    std::lock_guard lock{validator.mutex};
    BOOST_CHECK_EQUAL(validator.waited_submits, 1U);
    client.reset();
    server->Stop();
}

BOOST_AUTO_TEST_CASE(waited_submit_never_takes_the_lane_for_a_retained_action)
{
    const fs::path path{m_path_root / "client"};
    const auto action{SignedCancel(4)};
    ScriptedEndpoint validator;
    validator.markets.push_back(MarketRow(std::chrono::milliseconds{1000}, std::chrono::milliseconds{1000}));
    validator.submit = Receipt(action, "admitted");
    validator.waited = Receipt(action, "admitted");
    validator.waited.pushKV("wait_status", "timeout");
    auto server{Serve(validator)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    BOOST_REQUIRE_EQUAL(client->Submit(market, action).state, "admitted");
    // Submitting the retained action again resends its exact bytes by the
    // ordinary path (a waited read may follow), never as a waited submission.
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    BOOST_CHECK_EQUAL(client->Submit(market, action, std::chrono::milliseconds{2000}).state, "admitted");
    BOOST_CHECK((validator.Counts() == std::array<unsigned, 4>{0, 1, 2, 1}));
    std::lock_guard lock{validator.mutex};
    BOOST_CHECK_EQUAL(validator.waited_submits, 0U);
    client.reset();
    server->Stop();
}

BOOST_AUTO_TEST_SUITE_END()

namespace {
//! One public API call; (HTTP status, parsed body).
std::pair<int, UniValue> ApiCall(const node::HttpsEndpoint& endpoint, const std::string& method, const std::string& params)
{
    const auto reply{node::FlowMeshHttpsRequest(endpoint, "/flowmesh/v1",
        "{\"method\":\"" + method + "\",\"params\":" + params + "}", std::chrono::seconds{5}, 1 << 24)};
    BOOST_REQUIRE_MESSAGE(reply.response_received, reply.error);
    UniValue value;
    BOOST_REQUIRE(value.read(reply.body));
    return {reply.status, value};
}

std::set<std::string> KeySet(const UniValue& object)
{
    const auto keys{object.getKeys()};
    return {keys.begin(), keys.end()};
}

//! The status object keys an f8a3432 server sends with health disabled.
std::set<std::string> PreviousStatusKeys(const UniValue& status)
{
    std::set<std::string> out{"market_id", "domain", "base_asset_id", "vault_id", "execution_config_id", "quote_asset",
                              "available", "running", "paused", "pending_handoff", "halt", "error",
                              "next_microblock_sequence", "last_microblock_hash", "epoch", "round", "pending_actions",
                              "checkpoint_pending"};
    if (status["checkpoint_pending"].get_bool()) {
        out.insert({"pending_checkpoint_id", "pending_checkpoint_sequence", "pending_checkpoint_effect_count"});
    }
    return out;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_api_health_tests, ClientLaneFixture)

BOOST_AUTO_TEST_CASE(client_shows_advertised_readiness_without_changing_endpoints)
{
    const auto path{m_path_root / "readiness-client"};
    ScriptedEndpoint behind, older;
    auto row{MarketRow(std::chrono::milliseconds{0})};
    row.pushKV("health_api_version", 1);
    row.pushKV("endpoint_ready", false);
    UniValue codes{UniValue::VARR};
    codes.push_back("catching_up");
    row.pushKV("endpoint_not_ready", codes);
    behind.markets.push_back(row);
    older.markets.push_back(MarketRow(std::chrono::milliseconds{0}));
    auto first{Serve(behind)};
    auto second{Serve(older)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(first->Port()), Endpoint(second->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    auto status{client->Status()};
    BOOST_REQUIRE_EQUAL(status.endpoints.size(), 2U);
    BOOST_CHECK_EQUAL(status.endpoints[0].readiness, "not_ready");
    BOOST_CHECK_EQUAL(status.endpoints[0].readiness_market, market.GetHex());
    BOOST_CHECK((status.endpoints[0].not_ready == std::vector<std::string>{"catching_up"}));
    BOOST_CHECK_GT(status.endpoints[0].readiness_observed_ms, 0);
    BOOST_CHECK_EQUAL(status.endpoints[1].readiness, "unknown");
    // Display only: the endpoint that reports itself not ready still serves
    // the reads, exactly as before.
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    BOOST_CHECK_EQUAL(behind.Counts()[3], 2U);
    BOOST_CHECK_EQUAL(older.Counts()[3], 0U);
    // A malformed hint is unknown and never fails the read.
    {
        std::lock_guard lock{behind.mutex};
        auto malformed{MarketRow(std::chrono::milliseconds{0})};
        malformed.pushKV("health_api_version", 1);
        malformed.pushKV("endpoint_ready", "no");
        malformed.pushKV("endpoint_not_ready", codes);
        behind.markets = UniValue{UniValue::VARR};
        behind.markets.push_back(malformed);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    status = client->Status();
    BOOST_CHECK_EQUAL(status.endpoints[0].readiness, "unknown");
    BOOST_CHECK(status.endpoints[0].readiness_market.empty());
    BOOST_CHECK(status.endpoints[0].not_ready.empty());
    BOOST_CHECK_EQUAL(status.endpoints[0].readiness_observed_ms, 0);
    client.reset();
    first->Stop();
    second->Stop();
}

BOOST_AUTO_TEST_CASE(api_health_disabled_keeps_the_previous_contract)
{
    node::FlowMeshService service{*m_node.chainman, m_path_root / "api-health-disabled"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(service.Start(*m_node.peerman, error), error);
    BOOST_REQUIRE(service.Market(market));
    // The factory default is disabled: exactly the previous contract.
    auto api{node::MakeFlowMeshTradingApi(service, Options())};
    BOOST_REQUIRE_MESSAGE(api->Start(error), error);
    const auto endpoint{Endpoint(api->Port())};
    const auto [status, refused]{ApiCall(endpoint, "health", "{}")};
    BOOST_CHECK_EQUAL(status, 400);
    BOOST_CHECK_EQUAL(refused["error"].get_str(), "This method is not exposed by the public trading API");
    const auto [markets_status, markets]{ApiCall(endpoint, "markets", "{}")};
    BOOST_REQUIRE_EQUAL(markets_status, 200);
    BOOST_REQUIRE_EQUAL(markets["result"].size(), 1U);
    const auto& row{markets["result"][0]};
    BOOST_CHECK(KeySet(row) == PreviousStatusKeys(row));
    const auto [updates_status, updates]{ApiCall(endpoint, "updates", "{\"market_id\":\"" + market.GetHex() + "\"}")};
    BOOST_REQUIRE_MESSAGE(updates_status == 200, updates.write());
    auto keys{PreviousStatusKeys(updates["result"]["status"])};
    keys.insert("chain_reconciling");
    BOOST_CHECK(KeySet(updates["result"]["status"]) == keys);
    api->Stop();
    service.Stop();
}

BOOST_AUTO_TEST_CASE(api_health_contract_capability_and_parameters)
{
    node::FlowMeshService service{*m_node.chainman, m_path_root / "api-health-enabled"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(service.Start(*m_node.peerman, error), error);
    BOOST_REQUIRE(service.Market(market));
    auto api{node::MakeFlowMeshTradingApi(service, Options(), {}, {}, {true})};
    BOOST_REQUIRE_MESSAGE(api->Start(error), error);
    const auto endpoint{Endpoint(api->Port())};
    const std::string one_market{"{\"market_id\":\"" + market.GetHex() + "\"}"};

    const auto [status, reply]{ApiCall(endpoint, "health", "{}")};
    BOOST_REQUIRE_EQUAL(status, 200);
    const auto& health{reply["result"]};
    BOOST_CHECK_EQUAL(health["health_version"].getInt<int>(), 1);
    BOOST_CHECK_EQUAL(health["observation"].get_str(), "local-node");
    BOOST_CHECK_EQUAL(health["service"]["state"].get_str(), "running");
    // No operator detail on the public path.
    BOOST_CHECK(!health["service"].exists("flowmesh_peers"));
    BOOST_REQUIRE_EQUAL(health["markets"].size(), 1U);
    const auto& row{health["markets"][0]};
    BOOST_CHECK_EQUAL(row["market_id"].get_str(), market.GetHex());
    BOOST_CHECK(!row.exists("error"));
    BOOST_CHECK(!row.exists("proposal_wait_reason"));
    BOOST_CHECK(!row["catchup"].exists("peers_hinting_ahead"));

    const auto [single_status, single]{ApiCall(endpoint, "health", one_market)};
    BOOST_CHECK_EQUAL(single_status, 200);
    BOOST_CHECK_EQUAL(single["result"]["markets"].size(), 1U);
    const auto [unknown_status, unknown]{ApiCall(endpoint, "health", "{\"market_id\":\"" + uint256::ONE.GetHex() + "\"}")};
    BOOST_CHECK_EQUAL(unknown_status, 400);
    BOOST_CHECK_EQUAL(unknown["error"].get_str(), "Unknown market");
    const auto [invalid_status, invalid]{ApiCall(endpoint, "health", "{\"market_id\":\"zz\"}")};
    BOOST_CHECK_EQUAL(invalid_status, 400);
    BOOST_CHECK_EQUAL(invalid["error"].get_str(), "Invalid identifier: market_id");
    std::this_thread::sleep_for(std::chrono::milliseconds{1100}); // Refill the per-client health budget.
    const auto [field_status, field]{ApiCall(endpoint, "health", "{\"verbose\":true}")};
    BOOST_CHECK_EQUAL(field_status, 400);
    BOOST_CHECK_EQUAL(field["error"].get_str(), "Unknown or duplicate client API field");

    // Every status object advertises the capability and this endpoint's
    // readiness for the market, as the health row reports it.
    const auto [markets_status, markets]{ApiCall(endpoint, "markets", "{}")};
    BOOST_REQUIRE_EQUAL(markets_status, 200);
    BOOST_REQUIRE_EQUAL(markets["result"].size(), 1U);
    const auto [updates_status, updates]{ApiCall(endpoint, "updates", one_market)};
    BOOST_REQUIRE_EQUAL(updates_status, 200);
    for (const auto* object : {&markets["result"][0], &updates["result"]["status"]}) {
        auto keys{PreviousStatusKeys(*object)};
        keys.insert({"health_api_version", "endpoint_ready", "endpoint_not_ready"});
        if (object == &updates["result"]["status"]) keys.insert("chain_reconciling");
        BOOST_CHECK(KeySet(*object) == keys);
        BOOST_CHECK_EQUAL((*object)["health_api_version"].getInt<int>(), 1);
        BOOST_CHECK_EQUAL((*object)["endpoint_ready"].get_bool(), row["ready"].get_bool());
        BOOST_CHECK_EQUAL((*object)["endpoint_not_ready"].write(), row["not_ready"].write());
    }
    api->Stop();
    service.Stop();
    // A stopped service's endpoint reports itself not ready.
    BOOST_REQUIRE_MESSAGE(api->Start(error), error);
    const auto [stopped_status, stopped]{ApiCall(Endpoint(api->Port()), "health", one_market)};
    BOOST_REQUIRE_EQUAL(stopped_status, 200);
    BOOST_CHECK_EQUAL(stopped["result"]["service"]["state"].get_str(), "stopped");
    BOOST_CHECK_EQUAL(stopped["result"]["markets"][0]["not_ready"].write(), "[\"service_stopped\",\"market_not_installed\"]");
    api->Stop();
}

BOOST_AUTO_TEST_CASE(api_health_is_cached_and_rate_limited_per_client)
{
    node::FlowMeshService service{*m_node.chainman, m_path_root / "api-health-budget"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(service.Start(*m_node.peerman, error), error);
    auto api{node::MakeFlowMeshTradingApi(service, Options(), {}, {}, {true})};
    BOOST_REQUIRE_MESSAGE(api->Start(error), error);
    const auto endpoint{Endpoint(api->Port())};
    // Within the cache interval the all-markets reply is the same object,
    // local ages included.
    const auto start{std::chrono::steady_clock::now()};
    const auto [first_status, first]{ApiCall(endpoint, "health", "{}")};
    const auto [second_status, second]{ApiCall(endpoint, "health", "{}")};
    if (std::chrono::steady_clock::now() - start < node::FLOWMESH_API_HEALTH_CACHE) {
        BOOST_CHECK_EQUAL(first["result"].write(), second["result"].write());
    }
    BOOST_CHECK_EQUAL(first_status, 200);
    BOOST_CHECK_EQUAL(second_status, 200);
    // Four per second per client, then 429; single-market reads count too.
    BOOST_CHECK_EQUAL(ApiCall(endpoint, "health", "{\"market_id\":\"" + market.GetHex() + "\"}").first, 200);
    BOOST_CHECK_EQUAL(ApiCall(endpoint, "health", "{}").first, 200);
    const auto [spent_status, spent]{ApiCall(endpoint, "health", "{}")};
    if (std::chrono::steady_clock::now() - start < std::chrono::seconds{1}) {
        BOOST_CHECK_EQUAL(spent_status, 429);
        BOOST_CHECK_EQUAL(spent["error"].get_str(), node::FLOWMESH_API_HEALTH_BUDGET_ERROR);
    }
    // The ordinary admission budget is separate and unchanged.
    BOOST_CHECK_EQUAL(ApiCall(endpoint, "markets", "{}").first, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds{1100});
    BOOST_CHECK_EQUAL(ApiCall(endpoint, "health", "{}").first, 200);
    api->Stop();
    service.Stop();
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(flowmesh_api_budget_tests, ClientLaneFixture)

BOOST_AUTO_TEST_CASE(order_budget_refuses_orders_not_cancels_before_authentication)
{
    node::FlowMeshService service{*m_node.chainman, m_path_root / "api-order-budget"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(service.Start(*m_node.peerman, error), error);
    BOOST_REQUIRE(service.Market(market));
    auto api{node::MakeFlowMeshTradingApi(service, Options())};
    BOOST_REQUIRE_MESSAGE(api->Start(error), error);
    const auto endpoint{Endpoint(api->Port())};
    // Unsigned actions: each order is charged to the order sub-budget before
    // authentication refuses it, which is what makes junk orders cost budget.
    const auto action = [&](flowmesh::ActionType type, uint64_t sequence) {
        flowmesh::Action out;
        out.signer = uint256{uint8_t{0x77}};
        out.sequence = sequence;
        out.type = static_cast<uint8_t>(type);
        if (type == flowmesh::ActionType::SUBMIT_BID) out.curve = {{5, 1}};
        return out;
    };
    const auto params = [&](const flowmesh::Action& value) {
        return "{\"market_id\":\"" + market.GetHex() + "\",\"action_id\":\"" + value.Id().GetHex() +
               "\",\"action_hex\":\"" + HexStr(*flowmesh::EncodeProductionActionPayload(value)) + "\"}";
    };
    const std::string authentication{"Action authentication failed for this chain/domain/market configuration"};
    const std::string exhausted{"Public trading order budget exhausted; cancels and withdrawals are still accepted"};
    const auto start{std::chrono::steady_clock::now()};
    std::vector<std::pair<int, std::string>> orders;
    for (uint64_t n{0}; n < 12; ++n) {
        const auto [status, reply]{ApiCall(endpoint, "submit", params(action(flowmesh::ActionType::SUBMIT_BID, n)))};
        orders.emplace_back(status, reply["error"].get_str());
    }
    // non_compact_submit_still_order_budgeted: the 13th order, sent with a
    // body the pre-parse classifier counts as a read, still hits the
    // post-parse order budget.
    const auto non_compact{node::FlowMeshHttpsRequest(endpoint, "/flowmesh/v1",
        "{ \"method\":\"submit\",\"params\":" + params(action(flowmesh::ActionType::SUBMIT_BID, 12)) + "}",
        std::chrono::seconds{5}, 1 << 20)};
    const auto [cancel_status, cancel]{ApiCall(endpoint, "submit", params(action(flowmesh::ActionType::CANCEL_BID, 13)))};
    flowmesh::Action deposit;
    deposit.type = static_cast<uint8_t>(flowmesh::ActionType::DEPOSIT);
    deposit.outpoint = COutPoint{Txid::FromUint256(uint256{uint8_t{0x78}}), 0};
    const auto [deposit_status, deposit_reply]{ApiCall(endpoint, "submit", params(deposit))};
    if (std::chrono::steady_clock::now() - start < std::chrono::milliseconds{900}) {
        for (const auto& [status, text] : orders) {
            BOOST_CHECK_EQUAL(status, 400);
            BOOST_CHECK_EQUAL(text, authentication);
        }
        BOOST_REQUIRE(non_compact.response_received);
        BOOST_CHECK_EQUAL(non_compact.status, 400);
        UniValue refused;
        BOOST_REQUIRE(refused.read(non_compact.body));
        BOOST_CHECK_EQUAL(refused["error"].get_str(), exhausted);
        // order_budget_refuses_orders_not_cancels: the cancel is not charged
        // (it reaches authentication), a deposit shares the order budget.
        BOOST_CHECK_EQUAL(cancel_status, 400);
        BOOST_CHECK_EQUAL(cancel["error"].get_str(), authentication);
        BOOST_CHECK_EQUAL(deposit_status, 400);
        BOOST_CHECK_EQUAL(deposit_reply["error"].get_str(), exhausted);
    }
    api->Stop();
    service.Stop();
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(flowmesh_api_outcome_tests, ClientLaneFixture)

BOOST_AUTO_TEST_CASE(api_outcome_contract_advert_and_opt_in_only)
{
    node::FlowMeshService service{*m_node.chainman, m_path_root / "api-outcome"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(service.Start(*m_node.peerman, error), error);
    BOOST_REQUIRE(service.Market(market));
    const std::string ids{"{\"market_id\":\"" + market.GetHex() + "\",\"action_id\":\"" +
                          uint256{uint8_t{2}}.GetHex() + "\""};
    const std::string evidence_params{"{\"market_id\":\"" + market.GetHex() +
        "\",\"microblock_sequence\":7,\"microblock_hash\":\"" + uint256{uint8_t{3}}.GetHex() + "\"}"};
    std::string v1_reply;
    {
        // api_outcome_param_and_method_rejected_when_disabled: the factory
        // default is disabled, exactly the previous contract.
        auto api{node::MakeFlowMeshTradingApi(service, Options())};
        BOOST_REQUIRE_MESSAGE(api->Start(error), error);
        const auto endpoint{Endpoint(api->Port())};
        const auto [status, reply]{ApiCall(endpoint, "action", ids + "}")};
        BOOST_REQUIRE_EQUAL(status, 200);
        v1_reply = reply["result"].write();
        const auto [param_status, param]{ApiCall(endpoint, "action", ids + ",\"outcome\":true}")};
        BOOST_CHECK_EQUAL(param_status, 400);
        BOOST_CHECK_EQUAL(param["error"].get_str(), node::FLOWMESH_CLIENT_UNKNOWN_FIELD_ERROR);
        const auto [method_status, method]{ApiCall(endpoint, "outcome_evidence", evidence_params)};
        BOOST_CHECK_EQUAL(method_status, 400);
        BOOST_CHECK_EQUAL(method["error"].get_str(), "This method is not exposed by the public trading API");
        // api_outcome_advert_only_when_enabled.
        const auto [markets_status, markets]{ApiCall(endpoint, "markets", "{}")};
        BOOST_REQUIRE_EQUAL(markets_status, 200);
        BOOST_CHECK(!markets["result"][0].exists("action_outcome_v1"));
        api->Stop();
    }
    {
        auto api{node::MakeFlowMeshTradingApi(service, Options(), {}, {}, {}, {true})};
        BOOST_REQUIRE_MESSAGE(api->Start(error), error);
        const auto endpoint{Endpoint(api->Port())};
        const auto [markets_status, markets]{ApiCall(endpoint, "markets", "{}")};
        BOOST_REQUIRE_EQUAL(markets_status, 200);
        BOOST_CHECK(markets["result"][0]["action_outcome_v1"].get_bool());
        auto keys{PreviousStatusKeys(markets["result"][0])};
        keys.insert("action_outcome_v1");
        BOOST_CHECK(KeySet(markets["result"][0]) == keys);
        // api_v1_replies_byte_identical_without_opt_in: without "outcome"
        // (or with false) the reply is the v1 reply, byte for byte.
        const auto [status, reply]{ApiCall(endpoint, "action", ids + "}")};
        BOOST_REQUIRE_EQUAL(status, 200);
        BOOST_CHECK_EQUAL(reply["result"].write(), v1_reply);
        const auto [false_status, opted_out]{ApiCall(endpoint, "action", ids + ",\"outcome\":false}")};
        BOOST_REQUIRE_EQUAL(false_status, 200);
        BOOST_CHECK_EQUAL(opted_out["result"].write(), v1_reply);
        const auto [bad_status, bad]{ApiCall(endpoint, "action", ids + ",\"outcome\":1}")};
        BOOST_CHECK_EQUAL(bad_status, 400);
        BOOST_CHECK_EQUAL(bad["error"].get_str(), "Expected boolean: outcome");
        // Opted in, an unknown action is still unknown, with no evidence parts.
        const auto [opt_status, opted]{ApiCall(endpoint, "action", ids + ",\"outcome\":true}")};
        BOOST_REQUIRE_EQUAL(opt_status, 200);
        BOOST_CHECK_EQUAL(opted["result"]["receipt_state"].get_str(), "unknown");
        BOOST_CHECK(!opted["result"].exists("outcome_evidence"));
        BOOST_CHECK(!opted["result"].exists("outcome_evidence_error"));
        BOOST_CHECK(!opted["result"].exists("pool_notice"));
        // The method answers only from memory: nothing retained is EXPIRED.
        const auto [expired_status, expired]{ApiCall(endpoint, "outcome_evidence", evidence_params)};
        BOOST_CHECK_EQUAL(expired_status, 400);
        BOOST_CHECK_EQUAL(expired["error"].get_str(), "FlowMesh outcome evidence is unavailable: EXPIRED");
        const auto [unknown_status, unknown]{ApiCall(endpoint, "outcome_evidence",
            "{\"market_id\":\"" + market.GetHex() + "\",\"microblock_sequence\":7}")};
        BOOST_CHECK_EQUAL(unknown_status, 400);
        BOOST_CHECK_EQUAL(unknown["error"].get_str(), "Invalid text: microblock_hash");
        // api_outcome_extra_parts_charge_tokens: the method costs one more
        // read token; refused, it is the documented budget error.
        std::this_thread::sleep_for(std::chrono::milliseconds{1100});
        const auto start{std::chrono::steady_clock::now()};
        for (int i{0}; i < 27; ++i) ApiCall(endpoint, "markets", "{}");
        const auto [spent_status, spent]{ApiCall(endpoint, "outcome_evidence", evidence_params)};
        if (std::chrono::steady_clock::now() - start < std::chrono::milliseconds{900}) {
            BOOST_CHECK_EQUAL(spent_status, 400);
            BOOST_CHECK_EQUAL(spent["error"].get_str(), node::FLOWMESH_API_OUTCOME_BUDGET_ERROR);
        }
        api->Stop();
    }
    service.Stop();
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(flowmesh_api_submit_wait_tests, ClientLaneFixture)

BOOST_AUTO_TEST_CASE(trading_api_submit_keeps_the_previous_contract_unless_submit_waits_are_offered)
{
    node::FlowMeshService service{*m_node.chainman, m_path_root / "api-submit-wait"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(service.Start(*m_node.peerman, error), error);
    BOOST_REQUIRE(service.Market(market));
    // A correctly signed cancel under this market's pins. This service has
    // no certified genesis yet, so it refuses the submission before queueing.
    CKey key;
    std::array<unsigned char, 32> secret{};
    secret.back() = 9;
    key.Set(secret.begin(), secret.end(), true);
    flowmesh::Action action;
    action.signer = flowmesh::AccountForKey(XOnlyPubKey{key.GetPubKey()});
    action.sequence = 4;
    action.type = static_cast<uint8_t>(flowmesh::ActionType::CANCEL_BID);
    BOOST_REQUIRE(flowmesh::SignAction(key, domain, config, action));
    const std::string submission{"{\"market_id\":\"" + market.GetHex() + "\",\"action_id\":\"" + action.Id().GetHex() +
                                 "\",\"action_hex\":\"" + HexStr(*flowmesh::EncodeProductionActionPayload(action)) + "\""};
    std::string previous_reply;
    for (const auto& [name, policy] : std::vector<std::pair<std::string, node::FlowMeshTradingApiWait>>{
             {"waits disabled", {}},
             {"waits not offered on submit", {std::chrono::milliseconds{2000}, 4}},
             {"submit waits without action waits", {std::chrono::milliseconds{0}, 4, true}}}) {
        BOOST_TEST_CONTEXT(name) {
            auto api{node::MakeFlowMeshTradingApi(service, Options(), {}, policy)};
            BOOST_REQUIRE_MESSAGE(api->Start(error), error);
            const auto endpoint{Endpoint(api->Port())};
            const auto [markets_status, markets]{ApiCall(endpoint, "markets", "{}")};
            BOOST_REQUIRE_EQUAL(markets_status, 200);
            BOOST_CHECK(!markets["result"][0].exists("submit_wait_ms_max"));
            // Exactly the previous contract: wait_ms is an unknown field,
            // refused before anything is decoded or admitted.
            const auto [refused_status, refused]{ApiCall(endpoint, "submit", submission + ",\"wait_ms\":500}")};
            BOOST_CHECK_EQUAL(refused_status, 400);
            BOOST_CHECK_EQUAL(refused["error"].get_str(), node::FLOWMESH_CLIENT_UNKNOWN_FIELD_ERROR);
            const auto [status, reply]{ApiCall(endpoint, "submit", submission + "}")};
            BOOST_REQUIRE_EQUAL(status, 200);
            BOOST_CHECK_EQUAL(reply["result"]["receipt_state"].get_str(), "rejected");
            if (previous_reply.empty()) previous_reply = reply["result"].write();
            BOOST_CHECK_EQUAL(reply["result"].write(), previous_reply);
            api->Stop();
        }
    }
    auto api{node::MakeFlowMeshTradingApi(service, Options(), {}, {std::chrono::milliseconds{2000}, 4, true})};
    BOOST_REQUIRE_MESSAGE(api->Start(error), error);
    const auto endpoint{Endpoint(api->Port())};
    // Offered: advertised next to the action wait, with the same maximum.
    const auto [markets_status, markets]{ApiCall(endpoint, "markets", "{}")};
    BOOST_REQUIRE_EQUAL(markets_status, 200);
    const auto& row{markets["result"][0]};
    BOOST_CHECK_EQUAL(row["submit_wait_ms_max"].getInt<int64_t>(), 2000);
    auto keys{PreviousStatusKeys(row)};
    keys.insert({"action_wait_ms_max", "submit_wait_ms_max"});
    BOOST_CHECK(KeySet(row) == keys);
    // Without wait_ms, the reply is the previous one, byte for byte.
    const auto [plain_status, plain]{ApiCall(endpoint, "submit", submission + "}")};
    BOOST_REQUIRE_EQUAL(plain_status, 200);
    BOOST_CHECK_EQUAL(plain["result"].write(), previous_reply);
    // A refused submission never waits: the same receipt, "none", and no
    // embedded action reply, at once.
    const auto start{std::chrono::steady_clock::now()};
    const auto [waited_status, waited]{ApiCall(endpoint, "submit", submission + ",\"wait_ms\":2000}")};
    BOOST_CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
    BOOST_REQUIRE_EQUAL(waited_status, 200);
    BOOST_CHECK_EQUAL(waited["result"]["receipt_state"].get_str(), "rejected");
    BOOST_CHECK_EQUAL(waited["result"]["wait_status"].get_str(), "none");
    BOOST_CHECK(!waited["result"].exists("action"));
    UniValue without_wait{UniValue::VOBJ};
    for (const auto& key_name : waited["result"].getKeys()) {
        if (key_name != "wait_status") without_wait.pushKV(key_name, waited["result"][key_name]);
    }
    BOOST_CHECK_EQUAL(without_wait.write(), previous_reply);
    // Outcome evidence is not offered by this endpoint: an unknown field.
    const auto [outcome_status, outcome]{ApiCall(endpoint, "submit", submission + ",\"wait_ms\":2000,\"outcome\":true}")};
    BOOST_CHECK_EQUAL(outcome_status, 400);
    BOOST_CHECK_EQUAL(outcome["error"].get_str(), node::FLOWMESH_CLIENT_UNKNOWN_FIELD_ERROR);
    api->Stop();
    service.Stop();
}

BOOST_AUTO_TEST_CASE(trading_api_submit_refuses_a_malformed_wait_before_decoding_the_action)
{
    node::FlowMeshService service{*m_node.chainman, m_path_root / "api-submit-wait-malformed"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(service.Start(*m_node.peerman, error), error);
    auto api{node::MakeFlowMeshTradingApi(service, Options(), {}, {std::chrono::milliseconds{2000}, 4, true}, {}, {true})};
    BOOST_REQUIRE_MESSAGE(api->Start(error), error);
    const auto endpoint{Endpoint(api->Port())};
    // An undecodable action: any error that names wait_ms or outcome was
    // raised before the action was decoded, so nothing was admitted.
    const std::string invalid{"{\"market_id\":\"" + market.GetHex() + "\",\"action_id\":\"" + uint256{uint8_t{2}}.GetHex() +
                              "\",\"action_hex\":\"zz\""};
    for (const auto& [wait, expected] : std::vector<std::pair<std::string, std::string>>{
             {"\"500\"", "Expected integer: wait_ms"}, {"true", "Expected integer: wait_ms"},
             {"1.5", ""}, {"-1", ""}, {"18446744073709551616", ""}}) {
        BOOST_TEST_CONTEXT("wait_ms=" << wait) {
            const auto [status, reply]{ApiCall(endpoint, "submit", invalid + ",\"wait_ms\":" + wait + "}")};
            BOOST_CHECK_EQUAL(status, 400);
            BOOST_CHECK(reply["error"].get_str() != "Invalid bounded bytes: action_hex");
            if (!expected.empty()) BOOST_CHECK_EQUAL(reply["error"].get_str(), expected);
        }
    }
    const auto [bad_outcome_status, bad_outcome]{ApiCall(endpoint, "submit", invalid + ",\"wait_ms\":5,\"outcome\":1}")};
    BOOST_CHECK_EQUAL(bad_outcome_status, 400);
    BOOST_CHECK_EQUAL(bad_outcome["error"].get_str(), "Expected boolean: outcome");
    const auto [lone_status, lone]{ApiCall(endpoint, "submit", invalid + ",\"outcome\":true}")};
    BOOST_CHECK_EQUAL(lone_status, 400);
    BOOST_CHECK_EQUAL(lone["error"].get_str(), "outcome on 'submit' applies only with wait_ms");
    // A valid wait reaches the action's own checks.
    const auto [decoded_status, decoded]{ApiCall(endpoint, "submit", invalid + ",\"wait_ms\":500}")};
    BOOST_CHECK_EQUAL(decoded_status, 400);
    BOOST_CHECK_EQUAL(decoded["error"].get_str(), "Invalid bounded bytes: action_hex");
    api->Stop();
    service.Stop();
}

BOOST_AUTO_TEST_SUITE_END()

namespace {
//! The same reply without the opt-in evidence (as a v1 endpoint sends it).
UniValue WithoutEvidence(const UniValue& reply)
{
    UniValue out{UniValue::VOBJ};
    for (const auto& key : reply.getKeys()) if (key != "outcome_evidence") out.pushKV(key, reply[key]);
    return out;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_client_outcome_evidence_tests, ClientLaneFixture)

BOOST_AUTO_TEST_CASE(advertised_action_outcome_parses_only_true)
{
    UniValue status{UniValue::VOBJ};
    BOOST_CHECK(!node::FlowMeshAdvertisedActionOutcome(status));
    for (const char* value : {"1", "\"true\"", "false", "null"}) {
        UniValue row{UniValue::VOBJ};
        UniValue parsed;
        BOOST_REQUIRE(parsed.read(std::string{"["} + value + "]"));
        row.pushKV("action_outcome_v1", parsed[0]);
        BOOST_CHECK(!node::FlowMeshAdvertisedActionOutcome(row));
    }
    status.pushKV("action_outcome_v1", true);
    BOOST_CHECK(node::FlowMeshAdvertisedActionOutcome(status));
    BOOST_CHECK(!node::FlowMeshAdvertisedActionOutcome(UniValue{}));
}

BOOST_AUTO_TEST_CASE(remote_client_verifies_outcome_evidence_only_on_advertising_endpoints)
{
    const fs::path path{m_path_root / "client"};
    const uint256 owner{*uint256::FromHex(std::string(64, '4'))};
    const auto action{Seed(path, owner)};
    ScriptedEndpoint validator;
    auto row{MarketRow(std::chrono::milliseconds{0})};
    row.pushKV("action_outcome_v1", true);
    validator.markets.push_back(row);
    validator.plain = OutcomeReceipt(action).first;
    auto server{Serve(validator)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    const auto receipt{client->ActionStatus(market, action.Id(), false)};
    BOOST_CHECK_EQUAL(receipt.state, "certified_inclusion");
    BOOST_CHECK(receipt.certificate_verified);
    BOOST_REQUIRE(receipt.outcome_verified);
    BOOST_REQUIRE(receipt.outcome);
    // The seeded cancel is at sequence 3 of a fresh account: included, not
    // executed, and not final (the same bytes could still execute).
    BOOST_CHECK_EQUAL(receipt.outcome->status, "included_not_executed");
    BOOST_CHECK(!receipt.outcome->final);
    BOOST_CHECK_EQUAL(receipt.outcome->reason_code, "INCLUDED_BAD_SEQUENCE");
    BOOST_CHECK_EQUAL(receipt.outcome->evidence, "certified_result_preimage");
    BOOST_CHECK(receipt.outcome->microblock_hash == receipt.microblock_hash);
    BOOST_CHECK_EQUAL(receipt.reason_code, "INCLUDED_BAD_SEQUENCE");
    BOOST_CHECK_EQUAL(validator.outcome_flags, 1U);
    // Verified once, it is returned without another request.
    const auto again{client->ActionStatus(market, action.Id(), false)};
    BOOST_CHECK(again.outcome_verified);
    BOOST_CHECK_EQUAL(validator.Counts()[0], 1U);
    client.reset();
    server->Stop();

    // An endpoint that does not advertise it never receives "outcome", and
    // the verified inclusion says why there is no outcome.
    const fs::path other{m_path_root / "client-old"};
    const auto seeded{Seed(other, owner)};
    ScriptedEndpoint old;
    old.markets.push_back(MarketRow(std::chrono::milliseconds{0}));
    old.plain = WithoutEvidence(OutcomeReceipt(seeded).first); // a v1 reply
    auto old_server{Serve(old)};
    auto old_client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(old_server->Port())}, other, error)};
    BOOST_REQUIRE_MESSAGE(old_client, error);
    BOOST_REQUIRE_EQUAL(old_client->Markets(std::nullopt).size(), 1U);
    const auto unoffered{old_client->ActionStatus(market, seeded.Id(), false)};
    BOOST_CHECK(unoffered.certificate_verified);
    BOOST_CHECK(!unoffered.outcome_verified);
    BOOST_CHECK(!unoffered.outcome);
    BOOST_CHECK_EQUAL(unoffered.reason_code, "OUTCOME_NOT_OFFERED");
    BOOST_CHECK_EQUAL(old.outcome_flags, 0U);
    old_client.reset();
    old_server->Stop();
}

BOOST_AUTO_TEST_CASE(evidence_failure_never_downgrades_inclusion)
{
    const uint256 owner{*uint256::FromHex(std::string(64, '4'))};
    const auto run = [&](const std::string& name, const std::function<void(UniValue&)>& mutate) {
        const fs::path path{m_path_root / fs::PathFromString(name)};
        const auto action{Seed(path, owner)};
        ScriptedEndpoint validator;
        auto row{MarketRow(std::chrono::milliseconds{0})};
        row.pushKV("action_outcome_v1", true);
        validator.markets.push_back(row);
        auto reply{OutcomeReceipt(action).first};
        mutate(reply);
        validator.plain = reply;
        auto server{Serve(validator)};
        std::string error;
        auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
        BOOST_REQUIRE_MESSAGE(client, error);
        BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
        auto receipt{client->ActionStatus(market, action.Id(), false)};
        client.reset();
        server->Stop();
        return receipt;
    };
    // Tampered evidence: the result root no longer matches.
    const auto tampered{run("tampered", [](UniValue& reply) {
        auto hex{reply["outcome_evidence"].get_str()};
        hex[hex.size() - 1] = hex.back() == '0' ? '1' : '0';
        reply.pushKV("outcome_evidence", hex);
    })};
    BOOST_CHECK(tampered.certificate_verified);
    BOOST_CHECK_EQUAL(tampered.state, "certified_inclusion");
    BOOST_CHECK(!tampered.outcome_verified && !tampered.outcome);
    BOOST_CHECK_EQUAL(tampered.reason_code, "OUTCOME_EVIDENCE_INVALID");
    const auto malformed{run("malformed", [](UniValue& reply) { reply.pushKV("outcome_evidence", "zz"); })};
    BOOST_CHECK(malformed.certificate_verified && !malformed.outcome_verified);
    BOOST_CHECK_EQUAL(malformed.reason_code, "OUTCOME_EVIDENCE_INVALID");
    for (const std::string code : {"EXPIRED", "TOO_LARGE", "BUDGET"}) {
        const auto missing{run("missing-" + code, [&](UniValue& reply) {
            UniValue without{UniValue::VOBJ};
            for (const auto& key : reply.getKeys()) if (key != "outcome_evidence") without.pushKV(key, reply[key]);
            without.pushKV("outcome_evidence_error", code);
            reply = without;
        })};
        BOOST_CHECK(missing.certificate_verified && !missing.outcome_verified);
        BOOST_CHECK_EQUAL(missing.reason_code, (code == "EXPIRED" ? "OUTCOME_EVIDENCE_EXPIRED"
                                                : code == "TOO_LARGE" ? "OUTCOME_EVIDENCE_TOO_LARGE"
                                                                      : "OUTCOME_EVIDENCE_BUDGET"));
    }
}

BOOST_AUTO_TEST_CASE(outcome_upgrade_at_most_once_per_endpoint)
{
    const fs::path path{m_path_root / "client"};
    const uint256 owner{*uint256::FromHex(std::string(64, '4'))};
    const auto action{Seed(path, owner)};
    ScriptedEndpoint validator;
    auto row{MarketRow(std::chrono::milliseconds{0})};
    row.pushKV("action_outcome_v1", true);
    validator.markets.push_back(row);
    auto [reply, evidence]{OutcomeReceipt(action)};
    UniValue without{UniValue::VOBJ};
    for (const auto& key : reply.getKeys()) if (key != "outcome_evidence") without.pushKV(key, reply[key]);
    without.pushKV("outcome_evidence_error", "BUDGET");
    validator.plain = without;
    validator.evidence_error = "FlowMesh outcome evidence is unavailable: EXPIRED";
    auto server{Serve(validator)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    const auto first{client->ActionStatus(market, action.Id(), false)};
    BOOST_REQUIRE(first.certificate_verified);
    BOOST_CHECK(!first.outcome_verified);
    BOOST_CHECK_EQUAL(first.reason_code, "OUTCOME_EVIDENCE_BUDGET");
    BOOST_CHECK_EQUAL(validator.evidence_reads, 0U);
    // The verified inclusion asks its endpoint once for the evidence; an
    // unavailable answer is recorded and never retried against it.
    std::this_thread::sleep_for(std::chrono::milliseconds{1100}); // leave the status budget's window
    const auto expired{client->ActionStatus(market, action.Id(), false)};
    BOOST_CHECK(expired.certificate_verified && !expired.outcome_verified);
    BOOST_CHECK_EQUAL(expired.reason_code, "OUTCOME_EVIDENCE_EXPIRED");
    BOOST_CHECK_EQUAL(validator.evidence_reads, 1U);
    {
        std::lock_guard lock{validator.mutex};
        validator.evidence_error.clear();
        validator.evidence.pushKV("outcome_evidence", evidence);
    }
    const auto still{client->ActionStatus(market, action.Id(), false)};
    BOOST_CHECK(!still.outcome_verified);
    BOOST_CHECK_EQUAL(validator.evidence_reads, 1U);
    BOOST_CHECK_EQUAL(validator.Counts()[0], 1U); // inclusion is never re-read
    client.reset();

    // A fresh process asks again and verifies from the evidence method.
    auto restarted{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(restarted, error);
    BOOST_REQUIRE_EQUAL(restarted->Markets(std::nullopt).size(), 1U);
    {
        std::lock_guard lock{validator.mutex};
        validator.plain = reply; // an ordinary proof, with evidence
    }
    const auto proved{restarted->ActionStatus(market, action.Id(), false)};
    BOOST_CHECK(proved.certificate_verified);
    BOOST_CHECK(proved.outcome_verified);
    restarted.reset();
    server->Stop();
}

BOOST_AUTO_TEST_CASE(unknown_field_error_clears_both_capabilities)
{
    const fs::path path{m_path_root / "client"};
    const uint256 owner{*uint256::FromHex(std::string(64, '4'))};
    const auto action{Seed(path, owner)};
    ScriptedEndpoint validator;
    auto row{MarketRow(std::chrono::milliseconds{1000})};
    row.pushKV("action_outcome_v1", true);
    validator.markets.push_back(row);
    validator.plain = WithoutEvidence(OutcomeReceipt(action).first); // its v1 reply
    validator.reject_outcome_field = true;
    auto server{Serve(validator)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    // The downgraded endpoint refuses the field: nothing is applied, and both
    // advertised capabilities are withdrawn until it advertises them again.
    const auto refused{client->ActionStatus(market, action.Id(), false)};
    BOOST_CHECK(!refused.certificate_verified);
    BOOST_CHECK_EQUAL(validator.outcome_flags, 1U);
    std::this_thread::sleep_for(std::chrono::milliseconds{1100});
    const auto plain{client->ActionStatus(market, action.Id(), false)};
    BOOST_CHECK(plain.certificate_verified);
    BOOST_CHECK(!plain.outcome_verified);
    BOOST_CHECK_EQUAL(plain.reason_code, "OUTCOME_NOT_OFFERED");
    BOOST_CHECK_EQUAL(validator.outcome_flags, 1U);
    client.reset();
    server->Stop();
}

BOOST_AUTO_TEST_SUITE_END()

namespace {
//! A different signed action of the same account at the same sequence.
flowmesh::Action SameSequenceConsumer(const flowmesh::Action& action)
{
    auto consumer{action};
    consumer.type = static_cast<uint8_t>(flowmesh::ActionType::CANCEL_ASK);
    consumer.credential = {4, 5, 6};
    return consumer;
}

/** An opt-in 'action' reply for `action` whose pool notice names
 * `consumer_id` in the microblock of `consumer_reply` (an OutcomeReceipt)
 * and carries that entry and its outcome evidence. */
UniValue ConsumerNoticeReply(const flowmesh::Action& action, const UniValue& consumer_reply, const uint256& consumer_id)
{
    UniValue reply{UniValue::VOBJ};
    reply.pushKV("action_id", action.Id().GetHex());
    reply.pushKV("receipt_state", "rejected");
    reply.pushKV("reason", "FlowMesh action account sequence was already used by another certified action");
    UniValue named{UniValue::VOBJ};
    named.pushKV("action_id", consumer_id.GetHex());
    named.pushKV("microblock_sequence", consumer_reply["microblock_sequence"]);
    named.pushKV("microblock_hash", consumer_reply["microblock_hash"]);
    UniValue notice{UniValue::VOBJ};
    notice.pushKV("reason_code", "STALE_SEQUENCE");
    notice.pushKV("account_sequence", action.sequence);
    notice.pushKV("next_account_sequence", action.sequence + 1);
    notice.pushKV("consumed_by", std::move(named));
    reply.pushKV("pool_notice", std::move(notice));
    reply.pushKV("consumer_certified_payload", consumer_reply["certified_payload"]);
    reply.pushKV("consumer_outcome_evidence", consumer_reply["outcome_evidence"]);
    return reply;
}

UniValue AdvertisingRow(UniValue row)
{
    row.pushKV("action_outcome_v1", true);
    return row;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_client_not_executed_tests, ClientLaneFixture)

BOOST_AUTO_TEST_CASE(not_executed_requires_verified_other_consumer)
{
    const fs::path path{m_path_root / "client"};
    const uint256 owner{*uint256::FromHex(std::string(64, '4'))};
    const auto action{Seed(path, owner)};
    const auto consumer{SameSequenceConsumer(action)};
    // The consumer is at the account's next sequence: it uses sequence 3.
    const auto consumed{OutcomeReceipt(consumer, action.sequence).first};
    ScriptedEndpoint validator;
    validator.markets.push_back(AdvertisingRow(MarketRow(std::chrono::milliseconds{0})));
    validator.plain = ConsumerNoticeReply(action, consumed, consumer.Id());
    auto server{Serve(validator)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    const auto receipt{client->ActionStatus(market, action.Id(), false)};
    BOOST_CHECK_EQUAL(receipt.state, "rejected");
    BOOST_CHECK(!receipt.certificate_verified);
    BOOST_REQUIRE(receipt.outcome_verified);
    BOOST_REQUIRE(receipt.outcome);
    BOOST_CHECK_EQUAL(receipt.outcome->status, "not_executed");
    BOOST_CHECK(receipt.outcome->final);
    BOOST_CHECK_EQUAL(receipt.outcome->reason_code, "NOT_EXECUTED_SEQUENCE_CONSUMED");
    BOOST_CHECK_EQUAL(receipt.outcome->evidence, "certified_sequence_consumer");
    BOOST_REQUIRE(receipt.outcome->consumed_by);
    BOOST_CHECK(receipt.outcome->consumed_by->action_id == consumer.Id());
    BOOST_CHECK_EQUAL(receipt.outcome->consumed_by->microblock_hash.GetHex(), consumed["microblock_hash"].get_str());
    BOOST_CHECK_EQUAL(receipt.reason_code, "NOT_EXECUTED_SEQUENCE_CONSUMED");
    BOOST_CHECK(receipt.reason.find("can never execute") != std::string::npos);
    BOOST_CHECK_EQUAL(client->Status().pending_actions, 0U);
    // Final: no further status read, and a retry never sends it again.
    const auto again{client->ActionStatus(market, action.Id(), false)};
    BOOST_CHECK(again.outcome_verified);
    const auto retried{client->ActionStatus(market, action.Id(), true)};
    BOOST_CHECK(retried.outcome_verified);
    BOOST_CHECK_EQUAL(retried.state, "rejected");
    BOOST_CHECK((validator.Counts() == std::array<unsigned, 4>{1, 0, 0, 1}));
    client.reset();
    server->Stop();
    const auto durable{DurableRow(path, action)};
    BOOST_REQUIRE(durable["not_executed_consumer"].isObject());
    BOOST_CHECK_EQUAL(durable["not_executed_consumer"]["action_id"].get_str(), consumer.Id().GetHex());
    BOOST_CHECK_EQUAL(durable["not_executed_consumer"]["microblock_hash"].get_str(), consumed["microblock_hash"].get_str());

    // Anything short of that proof leaves the possibly delivered action
    // unknown and marks nothing.
    const auto run = [&](const std::string& name, const UniValue& reply) {
        const fs::path other{m_path_root / fs::PathFromString(name)};
        const auto seeded{Seed(other, owner)};
        BOOST_REQUIRE(seeded.Id() == action.Id());
        ScriptedEndpoint endpoint;
        endpoint.markets.push_back(AdvertisingRow(MarketRow(std::chrono::milliseconds{0})));
        endpoint.plain = reply;
        auto other_server{Serve(endpoint)};
        auto other_client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(other_server->Port())}, other, error)};
        BOOST_REQUIRE_MESSAGE(other_client, error);
        BOOST_REQUIRE_EQUAL(other_client->Markets(std::nullopt).size(), 1U);
        const auto observed{other_client->ActionStatus(market, action.Id(), false)};
        BOOST_CHECK_MESSAGE(observed.state == "unknown", name);
        BOOST_CHECK_MESSAGE(!observed.outcome_verified && !observed.outcome, name);
        BOOST_CHECK_MESSAGE(observed.reason_code.empty(), name);
        other_client.reset();
        other_server->Stop();
        BOOST_CHECK_MESSAGE(!DurableRow(other, action).exists("not_executed_consumer"), name);
    };
    // The named consumer did not use the sequence (BAD_SEQUENCE there).
    run("not-consumed", ConsumerNoticeReply(action, OutcomeReceipt(consumer).first, consumer.Id()));
    // The "consumer" is this very action: that is execution, not a proof.
    run("self", ConsumerNoticeReply(action, OutcomeReceipt(action, action.sequence).first, action.Id()));
    // Another account's action at the same number proves nothing.
    auto stranger{consumer};
    stranger.signer = *uint256::FromHex(std::string(64, '5'));
    run("other-signer", ConsumerNoticeReply(action, OutcomeReceipt(stranger, action.sequence).first, stranger.Id()));
    // Evidence that does not hash to the certified result root.
    auto tampered{ConsumerNoticeReply(action, consumed, consumer.Id())};
    auto hex{tampered["consumer_outcome_evidence"].get_str()};
    hex[hex.size() - 1] = hex.back() == '0' ? '1' : '0';
    tampered.pushKV("consumer_outcome_evidence", hex);
    run("tampered", tampered);
    // The notice names a different microblock than the certified entry.
    auto misnamed{ConsumerNoticeReply(action, consumed, consumer.Id())};
    UniValue named{misnamed["pool_notice"]["consumed_by"]};
    named.pushKV("microblock_hash", uint256{uint8_t{7}}.GetHex());
    UniValue notice{misnamed["pool_notice"]};
    notice.pushKV("consumed_by", named);
    misnamed.pushKV("pool_notice", notice);
    run("misnamed", misnamed);
    // A certificate that does not verify under local B3 authority.
    auto forged{ConsumerNoticeReply(action, consumed, consumer.Id())};
    auto payload{forged["consumer_certified_payload"].get_str()};
    payload[payload.size() - 3] = payload[payload.size() - 3] == 'a' ? 'b' : 'a';
    forged.pushKV("consumer_certified_payload", payload);
    run("forged", forged);
}

BOOST_AUTO_TEST_CASE(not_executed_marker_saved_before_label_and_withdrawn_on_save_failure)
{
#ifdef WIN32
    BOOST_TEST_MESSAGE("Needs RLIMIT_FSIZE to fail the outbox write");
#else
    const fs::path path{m_path_root / "client"};
    const uint256 owner{*uint256::FromHex(std::string(64, '4'))};
    const auto action{Seed(path, owner)};
    const auto consumer{SameSequenceConsumer(action)};
    const auto consumed{OutcomeReceipt(consumer, action.sequence).first};
    ScriptedEndpoint validator;
    validator.markets.push_back(AdvertisingRow(MarketRow(std::chrono::milliseconds{0})));
    validator.plain = ConsumerNoticeReply(action, consumed, consumer.Id());
    auto server{Serve(validator)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    interfaces::FlowMeshActionReceipt failed;
    {
        ScopedFailedFileWrites full_disk;
        failed = client->ActionStatus(market, action.Id(), false);
    }
    // Never a verified outcome that is not durable.
    BOOST_CHECK_EQUAL(failed.state, "unknown");
    BOOST_CHECK(!failed.outcome_verified && !failed.outcome);
    BOOST_CHECK(failed.reason_code.empty());
    const auto retained{client->SavedActions(owner, market)};
    BOOST_REQUIRE_EQUAL(retained.size(), 1U);
    BOOST_CHECK(!retained[0].receipt.outcome_verified);
    BOOST_CHECK_EQUAL(retained[0].receipt.state, "unknown");
    // With writes working, the same reply proves it again, durably.
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    const auto proved{client->ActionStatus(market, action.Id(), false)};
    BOOST_CHECK_EQUAL(proved.state, "rejected");
    BOOST_CHECK(proved.outcome_verified);
    client.reset();
    server->Stop();
    BOOST_CHECK(DurableRow(path, action)["not_executed_consumer"].isObject());
#endif
}

BOOST_AUTO_TEST_CASE(restart_marker_blocks_resend_and_counts_unresolved)
{
    const fs::path path{m_path_root / "client"};
    const uint256 owner{*uint256::FromHex(std::string(64, '4'))};
    const auto action{Seed(path, owner)};
    const auto consumer{SameSequenceConsumer(action)};
    const auto consumed{OutcomeReceipt(consumer, action.sequence).first};
    std::string error;
    {
        ScriptedEndpoint validator;
        validator.markets.push_back(AdvertisingRow(MarketRow(std::chrono::milliseconds{0})));
        validator.plain = ConsumerNoticeReply(action, consumed, consumer.Id());
        auto server{Serve(validator)};
        auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
        BOOST_REQUIRE_MESSAGE(client, error);
        BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
        BOOST_REQUIRE(client->ActionStatus(market, action.Id(), false).outcome_verified);
        client.reset();
        server->Stop();
    }
    // Restarted against an endpoint that offers no outcomes: the marker is
    // not a verified outcome, the action counts as unresolved, and a retry
    // never sends it.
    {
        ScriptedEndpoint old;
        old.markets.push_back(MarketRow(std::chrono::milliseconds{0}));
        old.plain = ClientLaneFixture::Receipt(action, "rejected");
        old.submit = ClientLaneFixture::Receipt(action, "queued");
        auto server{Serve(old)};
        auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
        BOOST_REQUIRE_MESSAGE(client, error);
        BOOST_CHECK_EQUAL(client->Status().pending_actions, 1U);
        BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
        const auto retried{client->ActionStatus(market, action.Id(), true)};
        BOOST_CHECK_EQUAL(retried.state, "unknown");
        BOOST_CHECK(!retried.outcome_verified && !retried.outcome);
        BOOST_CHECK(retried.reason.find("Previously proven not executed") != std::string::npos);
        BOOST_CHECK(retried.reason.find("will not be resubmitted") != std::string::npos);
        BOOST_CHECK_EQUAL(old.Counts()[2], 0U); // never sent
        BOOST_CHECK_EQUAL(old.outcome_flags, 0U);
        BOOST_CHECK_EQUAL(client->Status().pending_actions, 1U);
        client.reset();
        server->Stop();
        BOOST_CHECK(DurableRow(path, action)["not_executed_consumer"].isObject());
    }
    // An endpoint that offers outcomes but no longer has the consumer's
    // evidence: one re-proof attempt per process, then nothing more.
    {
        ScriptedEndpoint expired;
        expired.markets.push_back(AdvertisingRow(MarketRow(std::chrono::milliseconds{0})));
        expired.plain = ClientLaneFixture::Receipt(action, "unknown");
        expired.by_action.emplace(consumer.Id().GetHex(), WithoutEvidence(consumed));
        auto server{Serve(expired)};
        auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
        BOOST_REQUIRE_MESSAGE(client, error);
        BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
        const auto first{client->ActionStatus(market, action.Id(), false)};
        BOOST_CHECK_EQUAL(first.state, "unknown");
        BOOST_CHECK(!first.outcome_verified);
        std::this_thread::sleep_for(std::chrono::milliseconds{1100});
        (void)client->ActionStatus(market, action.Id(), false);
        {
            std::lock_guard lock{expired.mutex};
            BOOST_CHECK_EQUAL(expired.by_action_reads[consumer.Id().GetHex()], 1U);
        }
        BOOST_CHECK_EQUAL(expired.Counts()[2], 0U);
        client.reset();
        server->Stop();
    }
    // The consumer's certified inclusion and evidence renew the proof.
    {
        ScriptedEndpoint validator;
        validator.markets.push_back(AdvertisingRow(MarketRow(std::chrono::milliseconds{0})));
        validator.plain = ClientLaneFixture::Receipt(action, "unknown");
        validator.by_action.emplace(consumer.Id().GetHex(), consumed);
        auto server{Serve(validator)};
        auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, path, error)};
        BOOST_REQUIRE_MESSAGE(client, error);
        BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
        const auto renewed{client->ActionStatus(market, action.Id(), false)};
        BOOST_CHECK_EQUAL(renewed.state, "rejected");
        BOOST_REQUIRE(renewed.outcome_verified && renewed.outcome);
        BOOST_CHECK_EQUAL(renewed.outcome->status, "not_executed");
        BOOST_CHECK_EQUAL(client->Status().pending_actions, 0U);
        const auto again{client->ActionStatus(market, action.Id(), false)};
        BOOST_CHECK(again.outcome_verified);
        {
            std::lock_guard lock{validator.mutex};
            BOOST_CHECK_EQUAL(validator.by_action_reads[consumer.Id().GetHex()], 1U);
        }
        BOOST_CHECK_EQUAL(validator.Counts()[0], 0U); // A itself was never read
        BOOST_CHECK_EQUAL(validator.Counts()[2], 0U);
        client.reset();
        server->Stop();
    }
}

BOOST_AUTO_TEST_SUITE_END()

namespace {
//! A long request deadline, so a watch is bounded by its own wait.
node::FlowMeshHttpsServer::Options WatchOptions(const ClientLaneFixture& fixture)
{
    auto options{fixture.Options()};
    options.request_timeout = std::chrono::seconds{10};
    return options;
}
std::string UpdatesParams(const uint256& market, const std::string& extra = {})
{
    return "{\"market_id\":\"" + market.GetHex() + "\"" + (extra.empty() ? "" : "," + extra) + "}";
}
std::string CursorParam(const UniValue& cursor)
{
    return "\"cursor\":" + cursor.write();
}
uint64_t Elapsed(const std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}
//! The key set of a watched reply: the page, the status and the hint only.
const std::set<std::string> WATCHED_REPLY_KEYS{"cursor", "gap", "more", "oldest_event_id", "latest_event_id",
                                               "events", "status", "watch_status"};
} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_api_watch_tests, ClientLaneFixture)

BOOST_AUTO_TEST_CASE(api_watch_slots_leave_room_for_action_waits)
{
    std::string error;
    // Without an explicit count, every configuration that started before
    // watches existed still starts: the default shrinks to the room left.
    for (const size_t action_slots : {size_t{0}, size_t{4}, size_t{6}, size_t{7}, size_t{12}}) {
        const auto slots{node::FlowMeshApiWatchSlots(action_slots, true, std::nullopt, error)};
        BOOST_REQUIRE(slots);
        BOOST_CHECK_EQUAL(*slots, std::min<size_t>(8, 14 - action_slots));
    }
    BOOST_CHECK_EQUAL(*node::FlowMeshApiWatchSlots(12, true, std::nullopt, error), 2U);
    // An explicit count is kept when it fits, refused when it does not.
    BOOST_CHECK_EQUAL(*node::FlowMeshApiWatchSlots(4, true, 10, error), 10U);
    BOOST_CHECK_EQUAL(*node::FlowMeshApiWatchSlots(12, true, 2, error), 2U);
    BOOST_CHECK_EQUAL(*node::FlowMeshApiWatchSlots(12, true, 0, error), 0U);
    error.clear();
    BOOST_CHECK(!node::FlowMeshApiWatchSlots(5, true, 10, error));
    BOOST_CHECK(error.find("-flowmeshapiwatchers=10") != std::string::npos);
    // ... unless watches are off, when no watch slot exists at all.
    BOOST_CHECK_EQUAL(*node::FlowMeshApiWatchSlots(12, false, 10, error), 10U);
    for (const int64_t bad : {int64_t{-1}, int64_t{11}, int64_t{1'000'000}}) {
        error.clear();
        BOOST_CHECK(!node::FlowMeshApiWatchSlots(0, true, bad, error));
        BOOST_CHECK(!error.empty());
    }
}

BOOST_AUTO_TEST_CASE(trading_api_updates_watch_disabled_keeps_exact_contract)
{
    node::FlowMeshService service{*m_node.chainman, m_path_root / "api-watch-disabled"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(service.Start(*m_node.peerman, error), error);
    BOOST_REQUIRE(service.Market(market));
    // The factory default is disabled, and so are zero watchers.
    for (const node::FlowMeshTradingApiWatch watch : {node::FlowMeshTradingApiWatch{},
                                                       node::FlowMeshTradingApiWatch{std::chrono::milliseconds{2000}, 0}}) {
        auto api{node::MakeFlowMeshTradingApi(service, WatchOptions(*this), {}, {}, {}, {}, watch)};
        BOOST_REQUIRE_MESSAGE(api->Start(error), error);
        const auto endpoint{Endpoint(api->Port())};
        const auto [markets_status, markets]{ApiCall(endpoint, "markets", "{}")};
        BOOST_REQUIRE_EQUAL(markets_status, 200);
        BOOST_REQUIRE_EQUAL(markets["result"].size(), 1U);
        BOOST_CHECK(KeySet(markets["result"][0]) == PreviousStatusKeys(markets["result"][0]));
        const auto [updates_status, updates]{ApiCall(endpoint, "updates", UpdatesParams(market))};
        BOOST_REQUIRE_EQUAL(updates_status, 200);
        auto keys{PreviousStatusKeys(updates["result"]["status"])};
        keys.insert("chain_reconciling");
        BOOST_CHECK(KeySet(updates["result"]["status"]) == keys);
        BOOST_CHECK(updates["result"].exists("reported_data"));
        BOOST_CHECK(!updates["result"].exists("watch_status"));
        const auto [refused, rejection]{ApiCall(endpoint, "updates", UpdatesParams(market, "\"watch_ms\":500"))};
        BOOST_CHECK_EQUAL(refused, 400);
        BOOST_CHECK_EQUAL(rejection["error"].get_str(), node::FLOWMESH_CLIENT_UNKNOWN_FIELD_ERROR);
        api->Stop();
    }
    service.Stop();
}

BOOST_AUTO_TEST_CASE(trading_api_updates_watch_contract)
{
    node::FlowMeshService service{*m_node.chainman, m_path_root / "api-watch-contract"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(service.Start(*m_node.peerman, error), error);
    BOOST_REQUIRE(service.Market(market));
    auto api{node::MakeFlowMeshTradingApi(service, WatchOptions(*this), {}, {}, {}, {},
                                          {std::chrono::milliseconds{2000}, 8})};
    BOOST_REQUIRE_MESSAGE(api->Start(error), error);
    const auto endpoint{Endpoint(api->Port())};

    // Every status object advertises the capability, and nothing else changes.
    const auto [markets_status, markets]{ApiCall(endpoint, "markets", "{}")};
    BOOST_REQUIRE_EQUAL(markets_status, 200);
    const auto& row{markets["result"][0]};
    BOOST_CHECK_EQUAL(row["updates_watch_ms_max"].getInt<int>(), 2000);
    auto row_keys{PreviousStatusKeys(row)};
    row_keys.insert("updates_watch_ms_max");
    BOOST_CHECK(KeySet(row) == row_keys);
    const auto [plain_status, plain]{ApiCall(endpoint, "updates", UpdatesParams(market))};
    BOOST_REQUIRE_EQUAL(plain_status, 200);
    BOOST_CHECK_EQUAL(plain["result"]["status"]["updates_watch_ms_max"].getInt<int>(), 2000);
    BOOST_CHECK(plain["result"].exists("reported_data"));
    BOOST_CHECK(!plain["result"].exists("watch_status"));
    const auto cursor{plain["result"]["cursor"]};
    const auto head{plain["result"]["status"]["last_microblock_hash"].get_str()};

    const auto watched = [&](const std::string& extra, const char* expected, const uint64_t at_least_ms, const uint64_t below_ms) {
        const auto start{std::chrono::steady_clock::now()};
        const auto [status, reply]{ApiCall(endpoint, "updates", UpdatesParams(market, extra))};
        const auto elapsed{Elapsed(start)};
        BOOST_TEST_CONTEXT(extra) {
            BOOST_REQUIRE_MESSAGE(status == 200, reply.write());
            BOOST_CHECK_EQUAL(reply["result"]["watch_status"].get_str(), expected);
            BOOST_CHECK(KeySet(reply["result"]) == WATCHED_REPLY_KEYS);
            BOOST_CHECK_EQUAL(reply["result"]["status"]["updates_watch_ms_max"].getInt<int>(), 2000);
            BOOST_CHECK_GE(elapsed, at_least_ms);
            BOOST_CHECK_LT(elapsed, below_ms);
        }
        return reply["result"];
    };
    // Answered at once, without a slot: zero, no cursor, a gap, a changed head.
    watched("\"watch_ms\":0," + CursorParam(cursor), "none", 0, 1000);
    watched("\"watch_ms\":2000", "no_cursor", 0, 1000);
    UniValue foreign{UniValue::VOBJ};
    foreign.pushKV("instance_id", uint256::ONE.GetHex());
    foreign.pushKV("event_id", 0);
    const auto gap{watched("\"watch_ms\":2000," + CursorParam(foreign), "gap", 0, 1000)};
    BOOST_CHECK(gap["gap"].get_bool());
    UniValue future{cursor};
    future.pushKV("event_id", cursor["event_id"].getInt<uint64_t>() + 5);
    watched("\"watch_ms\":2000," + CursorParam(future), "gap", 0, 1000);
    watched("\"watch_ms\":2000," + CursorParam(cursor) + ",\"known_head\":\"" + uint256::ONE.GetHex() + "\"", "head_changed", 0, 1000);
    // Nothing new for this cursor and head: it waits, then times out.
    const auto quiet{watched("\"watch_ms\":300," + CursorParam(cursor) + ",\"known_head\":\"" + head + "\"", "timeout", 280, 1500)};
    BOOST_CHECK(quiet["events"].empty());
    BOOST_CHECK_EQUAL(quiet["cursor"].write(), cursor.write());
    // Unknown markets and paging are refused before any slot or wait.
    for (const auto& [params, message] : std::vector<std::pair<std::string, std::string>>{
             {"{\"market_id\":\"" + uint256::ONE.GetHex() + "\",\"watch_ms\":2000," + CursorParam(cursor) + "}", "Unknown FlowMesh market"},
             {UpdatesParams(market, "\"watch_ms\":2000,\"limit\":10," + CursorParam(cursor)), "watch_ms cannot be combined with history paging"},
             {UpdatesParams(market, "\"watch_ms\":2000,\"before_sequence\":1," + CursorParam(cursor)), "watch_ms cannot be combined with history paging"}}) {
        const auto start{std::chrono::steady_clock::now()};
        const auto [status, reply]{ApiCall(endpoint, "updates", params)};
        BOOST_CHECK_EQUAL(status, 400);
        BOOST_CHECK_EQUAL(reply["error"].get_str(), message);
        BOOST_CHECK_LT(Elapsed(start), 1000U);
    }
    for (const std::string bad : {"-1", "1.5", "\"500\"", "true", "18446744073709551616", "1,\"watch_ms\":2"}) {
        BOOST_TEST_CONTEXT("watch_ms=" << bad) {
            const auto [status, reply]{ApiCall(endpoint, "updates", UpdatesParams(market, CursorParam(cursor) + ",\"watch_ms\":" + bad))};
            BOOST_CHECK_EQUAL(status, 400);
            BOOST_CHECK(!reply["ok"].get_bool());
        }
    }
    api->Stop();

    // Any unsigned integer is clamped to the advertised maximum, not refused.
    auto brief{node::MakeFlowMeshTradingApi(service, WatchOptions(*this), {}, {}, {}, {},
                                            {std::chrono::milliseconds{400}, 8})};
    BOOST_REQUIRE_MESSAGE(brief->Start(error), error);
    for (const std::string wait : {"60000", "18446744073709551615"}) {
        const auto start{std::chrono::steady_clock::now()};
        const auto [status, reply]{ApiCall(Endpoint(brief->Port()), "updates", UpdatesParams(market, CursorParam(cursor) + ",\"watch_ms\":" + wait))};
        BOOST_REQUIRE_EQUAL(status, 200);
        BOOST_CHECK_EQUAL(reply["result"]["watch_status"].get_str(), "timeout");
        BOOST_CHECK_GE(Elapsed(start), 380U);
        BOOST_CHECK_LT(Elapsed(start), 1500U);
    }
    brief->Stop();
    service.Stop();
}

BOOST_AUTO_TEST_CASE(trading_api_updates_watchers_are_bounded_and_hold_no_permit)
{
    node::FlowMeshService service{*m_node.chainman, m_path_root / "api-watch-busy"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(service.Start(*m_node.peerman, error), error);
    // No action waits: two permits plus the watch slots are all the workers.
    auto api{node::MakeFlowMeshTradingApi(service, WatchOptions(*this), {}, {}, {}, {},
                                          {std::chrono::milliseconds{1500}, 8})};
    BOOST_REQUIRE_MESSAGE(api->Start(error), error);
    const auto endpoint{Endpoint(api->Port())};
    const auto cursor{ApiCall(endpoint, "updates", UpdatesParams(market)).second["result"]["cursor"]};
    const auto watch = [&] {
        const auto start{std::chrono::steady_clock::now()};
        const auto [status, reply]{ApiCall(endpoint, "updates", UpdatesParams(market, CursorParam(cursor) + ",\"watch_ms\":1500"))};
        BOOST_REQUIRE_EQUAL(status, 200);
        return std::pair{reply["result"]["watch_status"].get_str(), Elapsed(start)};
    };
    auto first{std::async(std::launch::async, watch)};
    auto second{std::async(std::launch::async, watch)};
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    // A third watcher from this address gets the immediate reply.
    const auto [third, third_ms]{watch()};
    BOOST_CHECK_EQUAL(third, "busy");
    BOOST_CHECK_LT(third_ms, 1000U);
    // The sleepers hold no HTTPS permit: ordinary reads keep both permits.
    for (int i{0}; i < 4; ++i) {
        const auto start{std::chrono::steady_clock::now()};
        BOOST_CHECK_EQUAL(ApiCall(endpoint, "markets", "{}").first, 200);
        BOOST_CHECK_LT(Elapsed(start), 1000U);
    }
    for (auto* waiter : {&first, &second}) {
        const auto [label, elapsed]{waiter->get()};
        BOOST_CHECK_EQUAL(label, "timeout");
        BOOST_CHECK_GE(elapsed, 1400U);
    }
    api->Stop();

    // Capacity: one slot means the second watcher is refused at once.
    auto single{node::MakeFlowMeshTradingApi(service, WatchOptions(*this), {}, {}, {}, {},
                                             {std::chrono::milliseconds{1500}, 1})};
    BOOST_REQUIRE_MESSAGE(single->Start(error), error);
    const auto single_endpoint{Endpoint(single->Port())};
    auto sleeping{std::async(std::launch::async, [&] {
        return ApiCall(single_endpoint, "updates", UpdatesParams(market, CursorParam(cursor) + ",\"watch_ms\":1500")).second;
    })};
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    const auto [status, refused]{ApiCall(single_endpoint, "updates", UpdatesParams(market, CursorParam(cursor) + ",\"watch_ms\":1500"))};
    BOOST_CHECK_EQUAL(status, 200);
    BOOST_CHECK_EQUAL(refused["result"]["watch_status"].get_str(), "busy");
    BOOST_CHECK_EQUAL(sleeping.get()["result"]["watch_status"].get_str(), "timeout");
    single->Stop();
    service.Stop();
}

BOOST_AUTO_TEST_CASE(trading_api_updates_watch_wakes_on_commit_and_ends_on_stop)
{
    node::FlowMeshService service{*m_node.chainman, m_path_root / "api-watch-commit"};
    std::string error;
    BOOST_REQUIRE_MESSAGE(service.Start(*m_node.peerman, error), error);
    auto api{node::MakeFlowMeshTradingApi(service, WatchOptions(*this), {}, {}, {}, {},
                                          {std::chrono::milliseconds{2500}, 8})};
    BOOST_REQUIRE_MESSAGE(api->Start(error), error);
    const auto endpoint{Endpoint(api->Port())};
    const auto plain{ApiCall(endpoint, "updates", UpdatesParams(market)).second["result"]};
    BOOST_REQUIRE_EQUAL(plain["status"]["next_microblock_sequence"].getInt<int>(), 0);
    const std::string watch_params{UpdatesParams(market, CursorParam(plain["cursor"]) + ",\"known_head\":\"" +
                                                 plain["status"]["last_microblock_hash"].get_str() + "\",\"watch_ms\":2500")};
    const auto start{std::chrono::steady_clock::now()};
    auto watcher{std::async(std::launch::async, [&] { return ApiCall(endpoint, "updates", watch_params).second["result"]; })};
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    // Arming the seats certifies the market genesis: the commit's head event
    // ends the watch early.
    BOOST_REQUIRE_MESSAGE(service.ArmSeatKeys(secrets, error), error);
    const auto woken{watcher.get()};
    BOOST_CHECK_EQUAL(woken["watch_status"].get_str(), "ready");
    BOOST_CHECK_LT(Elapsed(start), 2400U);
    BOOST_REQUIRE(!woken["events"].empty());
    BOOST_CHECK_EQUAL(woken["events"][0]["kind"].get_str(), "certified_head");
    BOOST_CHECK_EQUAL(woken["status"]["next_microblock_sequence"].getInt<int>(), 1);
    BOOST_CHECK(!woken.exists("reported_data"));

    // Stopping the API ends a sleeping watch at once, not after its wait.
    const std::string next_params{UpdatesParams(market, CursorParam(woken["cursor"]) + ",\"known_head\":\"" +
                                                woken["status"]["last_microblock_hash"].get_str() + "\",\"watch_ms\":2500")};
    auto pending{std::async(std::launch::async, [&] {
        const auto reply{node::FlowMeshHttpsRequest(endpoint, "/flowmesh/v1",
            "{\"method\":\"updates\",\"params\":" + next_params + "}", std::chrono::seconds{5}, 1 << 20)};
        return std::chrono::steady_clock::now();
    })};
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    const auto stop_start{std::chrono::steady_clock::now()};
    api->Stop();
    BOOST_CHECK_LT(std::chrono::duration_cast<std::chrono::milliseconds>(pending.get() - stop_start).count(), 1200);
    service.Stop();
}

BOOST_AUTO_TEST_CASE(updates_watch_page_fits_its_reply_bound)
{
    // The largest page: CLIENT_EVENT_PAGE_MAX events, every optional field
    // present, and reasons at their bound made of characters JSON escapes
    // six times over. The status object is well below the headroom kept.
    flowmesh::ClientEventPage page;
    page.cursor = {uint256::ONE, std::numeric_limits<uint64_t>::max()};
    page.oldest_event_id = page.latest_event_id = std::numeric_limits<uint64_t>::max();
    page.more = true;
    for (size_t i{0}; i < flowmesh::CLIENT_EVENT_PAGE_MAX; ++i) {
        flowmesh::ClientEvent event;
        event.event_id = std::numeric_limits<uint64_t>::max() - i;
        event.market_id = event.action_id = event.account_id = event.signed_action_hash = event.microblock_hash = uint256::ONE;
        event.kind = flowmesh::ClientEventKind::CERTIFIED_INCLUDED;
        event.microblock_sequence = std::numeric_limits<uint64_t>::max();
        event.account_sequence = std::numeric_limits<uint64_t>::max();
        event.reason = std::string(160, '\x01');
        page.events.push_back(event);
    }
    const auto size{node::FlowMeshClientUpdatesPageJson(page).write().size()};
    BOOST_TEST_MESSAGE("worst-case watched page: " << size << " bytes");
    BOOST_CHECK_LT(size + 64 * 1024, node::FLOWMESH_API_WATCH_MAX_REPLY);
}

BOOST_AUTO_TEST_SUITE_END()

namespace {
/** A running service on the lane fixture's chain whose four armed seats
 * certify the market genesis, served by a real restricted API. Restart()
 * replaces both on the same datadir and port: a new event-log instance. */
struct LiveMarket {
    ClientLaneFixture& fixture;
    const fs::path path;
    const node::FlowMeshTradingApiWatch watch;
    const node::FlowMeshTradingApiTicks ticks;
    const node::FlowMeshTradingApiOutcome outcome;
    std::unique_ptr<node::FlowMeshService> service;
    std::unique_ptr<node::FlowMeshHttpsServer> api;
    uint16_t port{0};
    //! Told of every connection the API admits (set before Start).
    std::function<void(bool)> connection_observer;
    LiveMarket(ClientLaneFixture& f, fs::path p, const node::FlowMeshTradingApiWatch w = {},
               const node::FlowMeshTradingApiTicks t = {}, const node::FlowMeshTradingApiOutcome o = {})
        : fixture{f}, path{std::move(p)}, watch{w}, ticks{t}, outcome{o}
    {
        // Certify the market genesis, publish its checkpoint on B3, and start
        // again: the market then admits actions.
        Start();
        std::string error;
        const auto checkpoint{service->NextCheckpointMpa(fixture.market, error)};
        BOOST_REQUIRE_MESSAGE(checkpoint, error);
        Stop();
        fixture.ConnectMpaBlock({checkpoint->record});
        Start();
    }
    ~LiveMarket() { Stop(); }
    void Start()
    {
        service = std::make_unique<node::FlowMeshService>(*fixture.m_node.chainman, path);
        std::string error;
        BOOST_REQUIRE_MESSAGE(service->Start(*fixture.m_node.peerman, error), error);
        BOOST_REQUIRE_MESSAGE(service->ArmSeatKeys(fixture.secrets, error), error);
        auto options{fixture.Options(port)};
        options.request_timeout = std::chrono::seconds{10};
        api = node::MakeFlowMeshTradingApi(*service, options, {}, {}, {}, outcome, watch, ticks);
        if (connection_observer) {
            BOOST_REQUIRE(node::FlowMeshHttpsTestAccess::ObserveConnection(*api, [this](bool nodelay) {
                if (connection_observer) connection_observer(nodelay);
            }));
        }
        BOOST_REQUIRE_MESSAGE(api->Start(error), error);
        port = api->Port();
        BOOST_REQUIRE(WaitSequence(1));
    }
    //! Starts the API again (same port), now telling `observer` of each connection.
    void ObserveConnections(std::function<void(bool)> observer)
    {
        api->Stop();
        connection_observer = std::move(observer);
        BOOST_REQUIRE(node::FlowMeshHttpsTestAccess::ObserveConnection(*api, [this](bool nodelay) {
            if (connection_observer) connection_observer(nodelay);
        }));
        std::string error;
        BOOST_REQUIRE_MESSAGE(api->Start(error), error);
        BOOST_REQUIRE_EQUAL(api->Port(), port);
    }
    void Stop()
    {
        if (api) api->Stop();
        api.reset();
        if (service) service->Stop();
        service.reset();
    }
    void Restart()
    {
        const auto next{service->MarketStatus(fixture.market)->next_sequence};
        Stop();
        Start();
        BOOST_REQUIRE(WaitSequence(next));
    }
    bool WaitSequence(const uint64_t next) const
    {
        const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{10}};
        while (std::chrono::steady_clock::now() < deadline) {
            const auto status{service->MarketStatus(fixture.market)};
            if (status && status->next_sequence >= next && status->halt == node::FlowMeshRuntimeHalt::NONE) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return false;
    }
    //! Certifies one more entry holding `action`.
    void Certify(const flowmesh::Action& action)
    {
        const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{10}};
        while (service->MarketStatus(fixture.market)->paused && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        const auto next{service->MarketStatus(fixture.market)->next_sequence};
        std::string error;
        BOOST_REQUIRE_MESSAGE(service->SubmitLocalAction(fixture.market, action, error), error);
        BOOST_REQUIRE(WaitSequence(next + 1));
    }
    //! Certifies one more entry: the fixture's bootstrap user deposit.
    void Deposit()
    {
        flowmesh::Action deposit;
        deposit.type = static_cast<uint8_t>(flowmesh::ActionType::DEPOSIT);
        deposit.outpoint = fixture.deposit_outpoint;
        Certify(deposit);
    }
    node::HttpsEndpoint Endpoint() const { return fixture.Endpoint(port); }
};

flowmesh::MarketDataQuery Page()
{
    flowmesh::MarketDataQuery query;
    query.limit = 100;
    return query;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_client_watch_lane_tests, ClientLaneFixture)

BOOST_AUTO_TEST_CASE(event_gap_is_raised_only_when_a_snapshot_skips_events)
{
    LiveMarket live{*this, m_path_root / "gap-service"};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {live.Endpoint()}, m_path_root / "gap-client", error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    const auto first{client->Data(market, std::nullopt, Page(), error)};
    BOOST_REQUIRE_MESSAGE(first, error);
    BOOST_CHECK_EQUAL(first->snapshot.next_microblock_sequence, 1U);
    BOOST_CHECK(!first->event_gap);
    // A new head is read through a snapshot taken after the page that
    // reported it. The events in between are still delivered: no gap.
    live.Deposit();
    const auto moved{client->Data(market, std::nullopt, Page(), error)};
    BOOST_REQUIRE_MESSAGE(moved, error);
    BOOST_CHECK_EQUAL(moved->snapshot.next_microblock_sequence, 2U);
    BOOST_CHECK(!moved->event_gap);
    BOOST_CHECK_EQUAL(client->Status().event_gaps, 0U);
    const auto again{client->Data(market, std::nullopt, Page(), error)};
    BOOST_REQUIRE_MESSAGE(again, error);
    BOOST_CHECK(!again->event_gap);
    BOOST_CHECK_EQUAL(client->Status().event_gaps, 0U);
    // A restarted endpoint has a new event log: its events were skipped.
    live.Restart();
    const auto restarted{client->Data(market, std::nullopt, Page(), error)};
    BOOST_REQUIRE_MESSAGE(restarted, error);
    BOOST_CHECK(restarted->event_gap);
    BOOST_CHECK_EQUAL(client->Status().event_gaps, 1U);
    client.reset();
}

namespace {
//! Forwards public API requests to a live endpoint. It can hold watched
//! 'updates' replies, drop them (never written: the reply outlives the
//! request deadline), corrupt 'snapshot' state bytes, and inject a stale
//! updates_watch_ms_max into 'markets' and 'snapshot' status objects.
struct WatchProxy {
    node::HttpsEndpoint upstream;
    std::mutex mutex;
    std::condition_variable changed;
    bool hold{false}, drop{false}, corrupt_snapshot{false}, inject_advert{false};
    unsigned watched{0}, held{0};
    std::vector<std::string> methods;

    void Hold(const bool value)
    {
        {
            std::lock_guard lock{mutex};
            hold = value;
        }
        changed.notify_all();
    }
    bool AwaitHeld(const unsigned count)
    {
        std::unique_lock lock{mutex};
        return changed.wait_for(lock, std::chrono::seconds{5}, [&] { return held >= count; });
    }
    node::FlowMeshHttpsServer::Response Handle(const node::FlowMeshHttpsServer::Request& request)
    {
        UniValue json;
        if (!json.read(request.body) || !json["method"].isStr()) return {400, R"({"ok":false,"error":"proxy"})"};
        const auto method{json["method"].get_str()};
        const bool is_watch{method == "updates" && json["params"].exists("watch_ms")};
        {
            std::lock_guard lock{mutex};
            methods.push_back(method);
            watched += is_watch;
        }
        const auto reply{node::FlowMeshHttpsRequest(upstream, "/flowmesh/v1", request.body, std::chrono::seconds{10}, 32 << 20)};
        if (!reply.response_received) return {502, R"({"ok":false,"error":"proxy upstream failed"})"};
        std::string body{reply.body};
        if (is_watch) {
            std::unique_lock lock{mutex};
            ++held;
            changed.notify_all();
            changed.wait_for(lock, std::chrono::seconds{5}, [&] { return !hold; });
            if (drop) {
                lock.unlock();
                std::this_thread::sleep_until(request.deadline + std::chrono::milliseconds{200});
            }
        }
        UniValue parsed;
        if ((corrupt_snapshot || inject_advert) && parsed.read(body) && parsed["ok"].isTrue()) {
            UniValue result{parsed["result"]};
            if (corrupt_snapshot && method == "snapshot") {
                auto state{result["state_bytes"].get_str()};
                state[state.size() - 2] = state[state.size() - 2] == '0' ? '1' : '0';
                result.pushKV("state_bytes", state);
            }
            if (inject_advert && method == "snapshot") {
                UniValue status{result["status"]};
                status.pushKV("updates_watch_ms_max", 2000);
                result.pushKV("status", status);
            }
            if (inject_advert && method == "markets") {
                UniValue rows{UniValue::VARR};
                for (auto row : result.getValues()) {
                    row.pushKV("updates_watch_ms_max", 2000);
                    rows.push_back(row);
                }
                result = rows;
            }
            parsed.pushKV("result", result);
            body = parsed.write();
        }
        return {reply.status, body};
    }
    unsigned Watched()
    {
        std::lock_guard lock{mutex};
        return watched;
    }
};

std::unique_ptr<node::FlowMeshHttpsServer> Proxy(const ClientLaneFixture& fixture, WatchProxy& proxy,
                                                 std::chrono::milliseconds request_timeout = std::chrono::seconds{10})
{
    auto options{fixture.Options()};
    options.request_timeout = request_timeout;
    auto server{std::make_unique<node::FlowMeshHttpsServer>(options, [&proxy](const auto& request) { return proxy.Handle(request); })};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server->Start(error), error);
    return server;
}

using Watched = std::pair<std::optional<flowmesh::MarketData>, interfaces::FlowMeshMarketWatch>;
Watched WatchRead(node::FlowMeshTradingBackend& client, const uint256& market, std::chrono::milliseconds wait, std::string& error)
{
    interfaces::FlowMeshMarketWatch watch;
    auto data{client.Watch(market, std::nullopt, Page(), wait, watch, error)};
    return {std::move(data), watch};
}
} // namespace

BOOST_AUTO_TEST_CASE(watch_applies_a_commit_through_the_verified_snapshot)
{
    LiveMarket live{*this, m_path_root / "watch-commit-service", {std::chrono::milliseconds{2000}, 8}};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {live.Endpoint()}, m_path_root / "watch-commit-client", error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_MESSAGE(client->Data(market, std::nullopt, Page(), error), error);
    BOOST_CHECK_EQUAL(client->Status().endpoints.at(0).live_updates_ms, 2000U);
    const auto start{std::chrono::steady_clock::now()};
    auto pending{std::async(std::launch::async, [&] {
        std::string watch_error;
        auto result{WatchRead(*client, market, std::chrono::milliseconds{1500}, watch_error)};
        BOOST_CHECK_MESSAGE(result.first, watch_error);
        return result;
    })};
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    live.Deposit();
    // The deposit's admission events wake the (unfiltered) watch first; the
    // caller watches again at once until the commit's head arrives.
    auto [data, watch]{pending.get()};
    for (int follow{0}; data && watch.waited && data->snapshot.next_microblock_sequence < 2 && follow < 5; ++follow) {
        std::tie(data, watch) = WatchRead(*client, market, std::chrono::milliseconds{1500}, error);
    }
    BOOST_REQUIRE(data);
    BOOST_CHECK(watch.status == "ready" || watch.status == "head_changed");
    BOOST_CHECK(watch.supported && watch.waited && !watch.gap);
    BOOST_CHECK_EQUAL(watch.endpoint, live.Endpoint().url);
    BOOST_CHECK_EQUAL(watch.endpoint_live_ms, 2000U);
    BOOST_CHECK_LT(Elapsed(start), 1400U);
    // The new head is the verified snapshot's, never the page's claim.
    BOOST_CHECK_EQUAL(data->snapshot.next_microblock_sequence, 2U);
    BOOST_CHECK(data->certificate_verified && data->account_state_verified && data->remote);
    BOOST_CHECK(!data->event_gap);
    const auto ordinary{client->Data(market, std::nullopt, Page(), error)};
    BOOST_REQUIRE(ordinary);
    BOOST_CHECK(ordinary->snapshot.last_microblock_hash == data->snapshot.last_microblock_hash);
    client.reset();
}

BOOST_AUTO_TEST_CASE(watch_lane_leaves_work_gate_free_and_times_out_quietly)
{
    LiveMarket live{*this, m_path_root / "watch-quiet-service", {std::chrono::milliseconds{2000}, 8}};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {live.Endpoint()}, m_path_root / "watch-quiet-client", error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    const auto before{client->Data(market, std::nullopt, Page(), error)};
    BOOST_REQUIRE_MESSAGE(before, error);
    const auto start{std::chrono::steady_clock::now()};
    auto pending{std::async(std::launch::async, [&] {
        std::string watch_error;
        return WatchRead(*client, market, std::chrono::milliseconds{1200}, watch_error);
    })};
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    // The work gate is free while the watch sleeps on its lane.
    for (int i{0}; i < 3; ++i) {
        const auto read_start{std::chrono::steady_clock::now()};
        BOOST_CHECK_EQUAL(client->Markets(std::nullopt).size(), 1U);
        BOOST_CHECK(client->Data(market, std::nullopt, Page(), error));
        BOOST_CHECK(client->SavedActions(uint256::ONE, market).empty());
        BOOST_CHECK_LT(Elapsed(read_start), 800U);
    }
    // A second watch while the lane is busy reads ordinarily at once.
    const auto second_start{std::chrono::steady_clock::now()};
    std::string second_error;
    const auto [second, second_watch]{WatchRead(*client, market, std::chrono::milliseconds{1200}, second_error)};
    BOOST_CHECK(second);
    BOOST_CHECK_EQUAL(second_watch.status, "lane_busy");
    BOOST_CHECK(!second_watch.supported && !second_watch.waited);
    BOOST_CHECK_LT(Elapsed(second_start), 800U);
    const auto [data, watch]{pending.get()};
    BOOST_REQUIRE(data);
    BOOST_CHECK_EQUAL(watch.status, "timeout");
    BOOST_CHECK(watch.supported && watch.waited && !watch.gap);
    BOOST_CHECK_GE(Elapsed(start), 1150U);
    BOOST_CHECK(data->snapshot.last_microblock_hash == before->snapshot.last_microblock_hash);
    client.reset();
}

BOOST_AUTO_TEST_CASE(watch_never_sends_watch_ms_to_an_endpoint_without_advertisement)
{
    LiveMarket live{*this, m_path_root / "watch-off-service"};
    WatchProxy proxy{.upstream = live.Endpoint()};
    auto front{Proxy(*this, proxy)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(front->Port())}, m_path_root / "watch-off-client", error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    // Before any snapshot, and after one: never a watched request.
    for (int i{0}; i < 3; ++i) {
        const auto start{std::chrono::steady_clock::now()};
        const auto [data, watch]{WatchRead(*client, market, std::chrono::milliseconds{1500}, error)};
        BOOST_REQUIRE_MESSAGE(data, error);
        BOOST_CHECK_EQUAL(watch.status, "not_eligible");
        BOOST_CHECK(!watch.supported && !watch.waited);
        BOOST_CHECK_LT(Elapsed(start), 1000U);
    }
    BOOST_CHECK_EQUAL(client->Status().endpoints.at(0).live_updates_ms, 0U);
    BOOST_CHECK_EQUAL(proxy.Watched(), 0U);
    client.reset();
    front->Stop();
}

BOOST_AUTO_TEST_CASE(watch_unknown_field_withdraws_capability_and_reads_ordinarily)
{
    // A stale advertisement: the endpoint rejects watch_ms.
    LiveMarket live{*this, m_path_root / "watch-stale-service"};
    WatchProxy proxy{.upstream = live.Endpoint()};
    proxy.inject_advert = true;
    auto front{Proxy(*this, proxy)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(front->Port())}, m_path_root / "watch-stale-client", error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_MESSAGE(client->Data(market, std::nullopt, Page(), error), error); // A snapshot learns the advert.
    BOOST_CHECK_EQUAL(client->Status().endpoints.at(0).live_updates_ms, 2000U);
    const auto [data, watch]{WatchRead(*client, market, std::chrono::milliseconds{1500}, error)};
    BOOST_REQUIRE_MESSAGE(data, error); // The same call read ordinarily.
    BOOST_CHECK_EQUAL(watch.status, "unsupported");
    BOOST_CHECK(!watch.supported && !watch.waited);
    BOOST_CHECK_EQUAL(proxy.Watched(), 1U);
    BOOST_CHECK_EQUAL(client->Status().endpoints.at(0).live_updates_ms, 0U);
    // Withdrawn until the endpoint advertises it again.
    const auto [again, again_watch]{WatchRead(*client, market, std::chrono::milliseconds{1500}, error)};
    BOOST_CHECK(again);
    BOOST_CHECK_EQUAL(again_watch.status, "not_eligible");
    BOOST_CHECK_EQUAL(proxy.Watched(), 1U);
    client.reset();
    front->Stop();
}

BOOST_AUTO_TEST_CASE(watch_superseded_cursor_is_not_applied)
{
    LiveMarket live{*this, m_path_root / "watch-superseded-service", {std::chrono::milliseconds{2000}, 8}};
    WatchProxy proxy{.upstream = live.Endpoint()};
    auto front{Proxy(*this, proxy)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(front->Port())}, m_path_root / "watch-superseded-client", error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_MESSAGE(client->Data(market, std::nullopt, Page(), error), error);
    proxy.Hold(true);
    auto pending{std::async(std::launch::async, [&] {
        std::string watch_error;
        return WatchRead(*client, market, std::chrono::milliseconds{1500}, watch_error);
    })};
    // The commit wakes the watch; its reply is held while an ordinary read
    // advances the cache past the watched cursor.
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    live.Deposit();
    BOOST_REQUIRE(proxy.AwaitHeld(1));
    const auto ordinary{client->Data(market, std::nullopt, Page(), error)};
    BOOST_REQUIRE_MESSAGE(ordinary, error);
    BOOST_CHECK_EQUAL(ordinary->snapshot.next_microblock_sequence, 2U);
    proxy.Hold(false);
    const auto [data, watch]{pending.get()};
    BOOST_REQUIRE(data);
    BOOST_CHECK_EQUAL(watch.status, "superseded");
    BOOST_CHECK(!watch.supported && !watch.waited);
    BOOST_CHECK(data->snapshot.last_microblock_hash == ordinary->snapshot.last_microblock_hash);
    BOOST_CHECK(!data->event_gap);
    client.reset();
    front->Stop();
}

BOOST_AUTO_TEST_CASE(watch_transport_loss_marks_endpoint_unanswered_without_cooldown)
{
    LiveMarket live{*this, m_path_root / "watch-loss-service", {std::chrono::milliseconds{2000}, 8}};
    WatchProxy proxy{.upstream = live.Endpoint()};
    proxy.drop = true;
    auto front{Proxy(*this, proxy, std::chrono::milliseconds{1000})};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(front->Port())}, m_path_root / "watch-loss-client", error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_MESSAGE(client->Data(market, std::nullopt, Page(), error), error);
    const auto [lost, lost_watch]{WatchRead(*client, market, std::chrono::milliseconds{300}, error)};
    BOOST_CHECK(!lost);
    BOOST_CHECK(!error.empty());
    BOOST_CHECK_EQUAL(lost_watch.status, "failed");
    const auto status{client->Status()};
    BOOST_CHECK_EQUAL(status.endpoints.at(0).retry_after_ms, 0);
    BOOST_CHECK_EQUAL(status.endpoints.at(0).consecutive_failures, 0U);
    // Unanswered: the next call reads ordinarily (which answers), and only
    // then may the endpoint be watched again.
    proxy.drop = false;
    error.clear();
    const auto [ordinary, ordinary_watch]{WatchRead(*client, market, std::chrono::milliseconds{300}, error)};
    BOOST_REQUIRE_MESSAGE(ordinary, error);
    BOOST_CHECK_EQUAL(ordinary_watch.status, "not_eligible");
    BOOST_CHECK_EQUAL(proxy.Watched(), 1U);
    const auto [again, again_watch]{WatchRead(*client, market, std::chrono::milliseconds{300}, error)};
    BOOST_CHECK(again);
    BOOST_CHECK_EQUAL(again_watch.status, "timeout");
    BOOST_CHECK_EQUAL(proxy.Watched(), 2U);
    // A watch never sends or resends an action.
    for (const auto& method : proxy.methods) BOOST_CHECK(method == "updates" || method == "snapshot" || method == "markets");
    client.reset();
    front->Stop();
}

BOOST_AUTO_TEST_CASE(watch_head_change_verifies_snapshot_before_exposing_state)
{
    LiveMarket live{*this, m_path_root / "watch-forged-service", {std::chrono::milliseconds{2000}, 8}};
    WatchProxy proxy{.upstream = live.Endpoint()};
    auto front{Proxy(*this, proxy)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(front->Port())}, m_path_root / "watch-forged-client", error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    const auto before{client->Data(market, std::nullopt, Page(), error)};
    BOOST_REQUIRE_MESSAGE(before, error);
    live.Deposit();
    // The watched page reports the new head; the snapshot's state is forged.
    proxy.corrupt_snapshot = true;
    const auto [forged, watch]{WatchRead(*client, market, std::chrono::milliseconds{1500}, error)};
    BOOST_CHECK(!forged);
    BOOST_CHECK_EQUAL(watch.status, "invalid");
    BOOST_CHECK(!error.empty());
    // Nothing from the forged reply was exposed: an honest read verifies anew.
    proxy.corrupt_snapshot = false;
    error.clear();
    const auto honest{client->Data(market, std::nullopt, Page(), error)};
    BOOST_REQUIRE_MESSAGE(honest, error);
    BOOST_CHECK_EQUAL(honest->snapshot.next_microblock_sequence, 2U);
    client.reset();
    front->Stop();
}

BOOST_AUTO_TEST_CASE(watch_rechecks_local_authority_after_its_wait)
{
    LiveMarket live{*this, m_path_root / "watch-authority-service", {std::chrono::milliseconds{2000}, 8}};
    WatchProxy proxy{.upstream = live.Endpoint()};
    auto front{Proxy(*this, proxy)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(front->Port())}, m_path_root / "watch-authority-client", error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_MESSAGE(client->Data(market, std::nullopt, Page(), error), error);
    proxy.Hold(true);
    auto pending{std::async(std::launch::async, [&] {
        std::string watch_error;
        return WatchRead(*client, market, std::chrono::milliseconds{1500}, watch_error);
    })};
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    live.Deposit();
    BOOST_REQUIRE(proxy.AwaitHeld(1));
    // While the reply is held, B3 connects a checkpoint past the cached head:
    // the cached state no longer applies under local authority.
    const auto checkpoint{live.service->NextCheckpointMpa(market, error)};
    BOOST_REQUIRE_MESSAGE(checkpoint, error);
    BOOST_REQUIRE_EQUAL(checkpoint->sequence, 1U);
    ConnectMpaBlock({checkpoint->record});
    proxy.Hold(false);
    const auto [data, watch]{pending.get()};
    BOOST_REQUIRE(data);
    BOOST_CHECK_EQUAL(watch.status, "revalidate");
    BOOST_CHECK(!watch.supported);
    // The ordinary read re-verified, reported the gap and moved past the checkpoint.
    BOOST_CHECK_EQUAL(data->snapshot.next_microblock_sequence, 2U);
    BOOST_CHECK(data->event_gap);
    client.reset();
    front->Stop();
}

BOOST_AUTO_TEST_CASE(watch_lane_connection_released_after_busy)
{
    LiveMarket live{*this, m_path_root / "watch-busy-service", {std::chrono::milliseconds{2000}, 8}};
    std::atomic<unsigned> connections{0};
    live.ObserveConnections([&](bool) { ++connections; });
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {live.Endpoint()}, m_path_root / "watch-busy-client", error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    const auto data{client->Data(market, std::nullopt, Page(), error)};
    BOOST_REQUIRE_MESSAGE(data, error);
    // A kept-alive lane: two quiet watches, one connection.
    BOOST_CHECK_EQUAL(WatchRead(*client, market, std::chrono::milliseconds{300}, error).second.status, "timeout");
    const auto lane{connections.load()};
    BOOST_CHECK_EQUAL(WatchRead(*client, market, std::chrono::milliseconds{300}, error).second.status, "timeout");
    BOOST_CHECK_EQUAL(connections.load(), lane);
    // Both watch slots of this address are taken: 'busy', and the lane lets
    // its connection go.
    const auto cursor{ApiCall(live.Endpoint(), "updates", UpdatesParams(market)).second["result"]["cursor"]};
    const std::string held{UpdatesParams(market, CursorParam(cursor) + ",\"known_head\":\"" + data->snapshot.last_microblock_hash.GetHex() + "\",\"watch_ms\":1500")};
    auto first{std::async(std::launch::async, [&] { return ApiCall(live.Endpoint(), "updates", held); })};
    auto second{std::async(std::launch::async, [&] { return ApiCall(live.Endpoint(), "updates", held); })};
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    const auto [busy_data, busy]{WatchRead(*client, market, std::chrono::milliseconds{300}, error)};
    BOOST_CHECK(busy_data);
    BOOST_CHECK_EQUAL(busy.status, "busy");
    BOOST_CHECK(busy.supported && !busy.waited);
    first.get();
    second.get();
    const auto before{connections.load()};
    BOOST_CHECK_EQUAL(WatchRead(*client, market, std::chrono::milliseconds{300}, error).second.status, "timeout");
    BOOST_CHECK_EQUAL(connections.load(), before + 1);
    client.reset();
}

BOOST_AUTO_TEST_CASE(local_backend_watch_wakes_on_commit_and_needs_known_head)
{
    LiveMarket live{*this, m_path_root / "watch-local-service"};
    auto local{node::MakeLocalFlowMeshBackend(*live.service)};
    std::string error;
    const auto current{local->Data(market, std::nullopt, Page(), error)};
    BOOST_REQUIRE_MESSAGE(current, error);
    // Without the head it holds, a caller is answered at once.
    const auto start{std::chrono::steady_clock::now()};
    const auto [plain, plain_watch]{WatchRead(*local, market, std::chrono::milliseconds{1500}, error)};
    BOOST_CHECK(plain);
    BOOST_CHECK_EQUAL(plain_watch.status, "not_eligible");
    BOOST_CHECK(!plain_watch.supported);
    BOOST_CHECK_LT(Elapsed(start), 800U);
    auto query{Page()};
    query.known_head = current->snapshot.last_microblock_hash;
    const auto watch_read = [&] {
        interfaces::FlowMeshMarketWatch watch;
        std::string watch_error;
        auto data{local->Watch(market, std::nullopt, query, std::chrono::milliseconds{1500}, watch, watch_error)};
        return std::pair{std::move(data), watch};
    };
    const auto quiet_start{std::chrono::steady_clock::now()};
    const auto [quiet, quiet_watch]{watch_read()};
    BOOST_REQUIRE(quiet);
    BOOST_CHECK(quiet->unchanged);
    BOOST_CHECK_EQUAL(quiet_watch.status, "timeout");
    BOOST_CHECK(quiet_watch.supported && quiet_watch.waited);
    BOOST_CHECK_GE(Elapsed(quiet_start), 1450U);
    const auto wake_start{std::chrono::steady_clock::now()};
    auto pending{std::async(std::launch::async, watch_read)};
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    live.Deposit();
    // Its admission events wake the watch first; the commit's head follows.
    auto [woken, woken_watch]{pending.get()};
    for (int follow{0}; woken && woken->unchanged && woken_watch.waited && follow < 5; ++follow) {
        std::tie(woken, woken_watch) = watch_read();
    }
    BOOST_REQUIRE(woken);
    BOOST_CHECK(woken_watch.status == "ready" || woken_watch.status == "head_changed");
    BOOST_CHECK(!woken->unchanged);
    BOOST_CHECK_EQUAL(woken->snapshot.next_microblock_sequence, 2U);
    BOOST_CHECK_LT(Elapsed(wake_start), 1400U);
    // A caller behind the head is answered at once.
    const auto [behind, behind_watch]{watch_read()};
    BOOST_CHECK(behind);
    BOOST_CHECK_EQUAL(behind_watch.status, "head_changed");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(flowmesh_client_market_view_tests, ClientLaneFixture)

namespace {
std::pair<int, UniValue> ApiCall(const node::HttpsEndpoint& endpoint, const std::string& method, const std::string& params)
{
    const auto reply{node::FlowMeshHttpsRequest(endpoint, "/flowmesh/v1",
        "{\"method\":\"" + method + "\",\"params\":{" + params + "}}", std::chrono::seconds{10}, 32 << 20)};
    BOOST_REQUIRE_MESSAGE(reply.response_received, reply.error);
    UniValue value;
    BOOST_REQUIRE(value.read(reply.body));
    return {reply.status, value};
}
} // namespace

BOOST_AUTO_TEST_CASE(trading_api_without_clearing_ticks_keeps_the_previous_contract)
{
    const std::string id{"\"market_id\":\"" + market.GetHex() + "\""};
    LiveMarket off{*this, m_path_root / "ticks-off"};
    const auto [status, snapshot]{ApiCall(off.Endpoint(), "snapshot", id)};
    BOOST_CHECK_EQUAL(status, 200);
    BOOST_CHECK(!snapshot["result"]["status"].exists("clearing_ticks_max"));
    BOOST_CHECK(!snapshot["result"]["reported_data"].exists("clearing_ticks"));
    const auto [markets_status, markets]{ApiCall(off.Endpoint(), "markets", "")};
    BOOST_CHECK_EQUAL(markets_status, 200);
    BOOST_CHECK(!markets["result"][0].exists("clearing_ticks_max"));
    for (const char* method : {"snapshot", "updates"}) {
        const auto [refused, rejection]{ApiCall(off.Endpoint(), method, id + ",\"ticks\":{}")};
        BOOST_CHECK_EQUAL(refused, 400);
        BOOST_CHECK_EQUAL(rejection["error"].get_str(), node::FLOWMESH_CLIENT_UNKNOWN_FIELD_ERROR);
    }
}

BOOST_AUTO_TEST_CASE(trading_api_serves_clearing_ticks_when_enabled)
{
    const std::string id{"\"market_id\":\"" + market.GetHex() + "\""};
    LiveMarket on{*this, m_path_root / "ticks-on", {std::chrono::milliseconds{2000}, 2}, {16}};
    on.Deposit();
    const auto [status, plain]{ApiCall(on.Endpoint(), "snapshot", id)};
    BOOST_CHECK_EQUAL(status, 200);
    BOOST_CHECK_EQUAL(plain["result"]["status"]["clearing_ticks_max"].getInt<int>(), 16);
    // Not asked for: reported_data is unchanged.
    BOOST_CHECK(!plain["result"]["reported_data"].exists("clearing_ticks"));
    const auto [ticked_status, ticked]{ApiCall(on.Endpoint(), "snapshot", id + ",\"ticks\":{}")};
    BOOST_CHECK_EQUAL(ticked_status, 200);
    const auto head{ticked["result"]["status"]["last_microblock_hash"].get_str()};
    const auto& ticks{ticked["result"]["reported_data"]["clearing_ticks"]};
    BOOST_CHECK(ticks["available"].get_bool());
    BOOST_REQUIRE_EQUAL(ticks["ticks"].size(), 2U);
    BOOST_CHECK_EQUAL(ticks["ticks"][0]["sequence"].getInt<int>(), 0);
    BOOST_CHECK_EQUAL(ticks["ticks"][1]["microblock_hash"].get_str(), head);
    BOOST_CHECK_EQUAL(ticks["ticks"][1]["kind"].get_str(), "execution");
    BOOST_CHECK_EQUAL(ticks["ticks"][1]["imbalance_side"].get_str(), "none");

    // A light poll: unchanged heavy data, still the ticks after a position.
    const auto [light_status, light]{ApiCall(on.Endpoint(), "updates",
        id + ",\"known_head\":\"" + head + "\",\"ticks\":{\"after_sequence\":0}")};
    BOOST_CHECK_EQUAL(light_status, 200);
    BOOST_CHECK(light["result"]["reported_data"]["unchanged"].get_bool());
    BOOST_REQUIRE_EQUAL(light["result"]["reported_data"]["clearing_ticks"]["ticks"].size(), 1U);
    BOOST_CHECK_EQUAL(light["result"]["reported_data"]["clearing_ticks"]["ticks"][0]["microblock_hash"].get_str(), head);
    // A larger limit is clamped to the advertised maximum, never refused.
    const auto [clamped_status, clamped]{ApiCall(on.Endpoint(), "updates", id + ",\"ticks\":{\"limit\":18446744073709551615}")};
    BOOST_CHECK_EQUAL(clamped_status, 200);
    BOOST_CHECK_EQUAL(clamped["result"]["reported_data"]["clearing_ticks"]["ticks"].size(), 2U);
    // Malformed parameters are refused before any read.
    for (const std::string bad : {"{\"limit\":0}", "{\"limit\":-1}", "{\"after\":1}", "[]", "{\"after_sequence\":\"1\"}"}) {
        BOOST_TEST_CONTEXT("ticks=" << bad) {
            const auto [refused, rejection]{ApiCall(on.Endpoint(), "snapshot", id + ",\"ticks\":" + bad)};
            BOOST_CHECK_EQUAL(refused, 400);
            BOOST_CHECK(!rejection["ok"].get_bool());
        }
    }
    // A watched page carries no reported_data, so no ticks either.
    const auto [watched_status, watched]{ApiCall(on.Endpoint(), "updates", id + ",\"watch_ms\":500,\"ticks\":{}")};
    BOOST_CHECK_EQUAL(watched_status, 400);
    BOOST_CHECK_EQUAL(watched["error"].get_str(), "ticks cannot be combined with watch_ms");
}

namespace {
//! Forwards public API requests to a live endpoint and records each request's
//! method and params. It can add a clearing_ticks_max advertisement to every
//! status object and replace the hash of the last tick of every reply.
struct TickProxy {
    node::HttpsEndpoint upstream;
    std::mutex mutex;
    std::optional<uint64_t> advert;
    bool tamper{false};
    std::vector<std::pair<std::string, UniValue>> requests;

    node::FlowMeshHttpsServer::Response Handle(const node::FlowMeshHttpsServer::Request& request)
    {
        UniValue json;
        if (!json.read(request.body) || !json["method"].isStr()) return {400, R"({"ok":false,"error":"proxy"})"};
        const auto method{json["method"].get_str()};
        std::optional<uint64_t> inject;
        bool corrupt;
        {
            std::lock_guard lock{mutex};
            requests.emplace_back(method, json["params"]);
            inject = advert;
            corrupt = tamper;
        }
        const auto reply{node::FlowMeshHttpsRequest(upstream, "/flowmesh/v1", request.body, std::chrono::seconds{10}, 32 << 20)};
        if (!reply.response_received) return {502, R"({"ok":false,"error":"proxy upstream failed"})"};
        UniValue parsed;
        if ((!inject && !corrupt) || !parsed.read(reply.body) || !parsed["ok"].isTrue()) return {reply.status, reply.body};
        UniValue result{parsed["result"]};
        if (inject && method == "markets") {
            UniValue rows{UniValue::VARR};
            for (auto row : result.getValues()) {
                row.pushKV("clearing_ticks_max", *inject);
                rows.push_back(row);
            }
            result = rows;
        } else if (inject && result["status"].isObject()) {
            UniValue status{result["status"]};
            status.pushKV("clearing_ticks_max", *inject);
            result.pushKV("status", status);
        }
        if (corrupt && result["reported_data"]["clearing_ticks"]["ticks"].isArray() &&
            !result["reported_data"]["clearing_ticks"]["ticks"].empty()) {
            UniValue reported{result["reported_data"]};
            UniValue page{reported["clearing_ticks"]};
            const auto& ticks{page["ticks"].getValues()};
            UniValue rows{UniValue::VARR};
            for (size_t i{0}; i < ticks.size(); ++i) {
                UniValue row{ticks[i]};
                if (i + 1 == ticks.size()) row.pushKV("microblock_hash", uint256::ONE.GetHex());
                rows.push_back(row);
            }
            page.pushKV("ticks", rows);
            reported.pushKV("clearing_ticks", page);
            result.pushKV("reported_data", reported);
        }
        parsed.pushKV("result", result);
        return {reply.status, parsed.write()};
    }
    //! The "ticks" parameter of each recorded request that carried one.
    std::vector<std::pair<std::string, UniValue>> TickParams()
    {
        std::lock_guard lock{mutex};
        std::vector<std::pair<std::string, UniValue>> out;
        for (const auto& [method, params] : requests) {
            if (params.exists("ticks")) out.emplace_back(method, params["ticks"]);
        }
        return out;
    }
    size_t Requests()
    {
        std::lock_guard lock{mutex};
        return requests.size();
    }
};

std::unique_ptr<node::FlowMeshHttpsServer> TickProxyServer(const ClientLaneFixture& fixture, TickProxy& proxy)
{
    auto options{fixture.Options()};
    options.request_timeout = std::chrono::seconds{10};
    auto server{std::make_unique<node::FlowMeshHttpsServer>(options, [&proxy](const auto& request) { return proxy.Handle(request); })};
    std::string error;
    BOOST_REQUIRE_MESSAGE(server->Start(error), error);
    return server;
}

flowmesh::MarketDataQuery TickPage()
{
    auto query{Page()};
    query.ticks = flowmesh::ClearingTickQuery{};
    return query;
}
} // namespace

BOOST_AUTO_TEST_CASE(remote_client_holds_endpoint_ticks_bound_to_its_verified_head)
{
    LiveMarket live{*this, m_path_root / "remote-ticks-service", {}, {256}};
    TickProxy proxy{live.Endpoint()};
    auto server{TickProxyServer(*this, proxy)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, m_path_root / "remote-ticks-client", error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    // The first read learns the advertisement from its snapshot's status.
    const auto first{client->Data(market, std::nullopt, TickPage(), error)};
    BOOST_REQUIRE_MESSAGE(first, error);
    BOOST_REQUIRE(first->ticks);
    BOOST_CHECK(!first->ticks->available);
    BOOST_CHECK_EQUAL(first->ticks->unavailable_reason, "clearing_ticks_not_yet_received");
    BOOST_CHECK(proxy.TickParams().empty());
    // The next one asks for the newest ticks, bound to the verified head.
    const auto held{client->Data(market, std::nullopt, TickPage(), error)};
    BOOST_REQUIRE_MESSAGE(held, error);
    BOOST_REQUIRE(held->ticks && held->ticks->available);
    BOOST_REQUIRE_EQUAL(held->ticks->ticks.size(), 1U);
    BOOST_CHECK(held->ticks->ticks.back().microblock_hash == held->snapshot.last_microblock_hash);
    BOOST_CHECK_EQUAL(node::FlowMeshClientMarketDataJson(*held)["clearing_ticks"]["source"].get_str(), "endpoint_reported");
    auto sent{proxy.TickParams()};
    BOOST_REQUIRE_EQUAL(sent.size(), 1U);
    BOOST_CHECK_EQUAL(sent[0].first, "updates");
    BOOST_CHECK(!sent[0].second.exists("after_sequence"));
    BOOST_CHECK_EQUAL(sent[0].second["limit"].getInt<int>(), 256);
    // A new head: only the ticks after the last one held are asked for,
    // with the updates page and with the snapshot that verifies the head.
    live.Deposit();
    const auto moved{client->Data(market, std::nullopt, TickPage(), error)};
    BOOST_REQUIRE_MESSAGE(moved, error);
    BOOST_CHECK_EQUAL(moved->snapshot.next_microblock_sequence, 2U);
    BOOST_REQUIRE(moved->ticks && moved->ticks->available);
    BOOST_REQUIRE_EQUAL(moved->ticks->ticks.size(), 2U);
    BOOST_CHECK(moved->ticks->ticks.back().microblock_hash == moved->snapshot.last_microblock_hash);
    sent = proxy.TickParams();
    BOOST_REQUIRE_EQUAL(sent.size(), 3U);
    for (size_t i : {1, 2}) {
        BOOST_CHECK_EQUAL(sent[i].first, i == 1 ? "updates" : "snapshot");
        BOOST_CHECK_EQUAL(sent[i].second["after_sequence"].getInt<int>(), 0);
    }
    // The caller's own page of what is held.
    auto after{TickPage()};
    after.ticks->after_sequence = 0;
    const auto newest{client->Data(market, std::nullopt, after, error)};
    BOOST_REQUIRE_MESSAGE(newest, error);
    BOOST_REQUIRE(newest->ticks && newest->ticks->available);
    BOOST_REQUIRE_EQUAL(newest->ticks->ticks.size(), 1U);
    BOOST_CHECK_EQUAL(newest->ticks->ticks[0].sequence, 1U);
    // Reads without ticks, and the status read a signing preflight uses,
    // never carry them.
    const auto carried{proxy.TickParams().size()};
    BOOST_REQUIRE_MESSAGE(client->Data(market, std::nullopt, Page(), error), error);
    const auto status{client->Market(market, std::nullopt)};
    BOOST_REQUIRE(status);
    BOOST_CHECK(status->error.empty());
    BOOST_CHECK_EQUAL(proxy.TickParams().size(), carried);
    client.reset();
}

BOOST_AUTO_TEST_CASE(remote_client_withdraws_a_stale_ticks_advertisement)
{
    // The endpoint serves no ticks; the proxy advertises them anyway.
    LiveMarket live{*this, m_path_root / "stale-ticks-service"};
    TickProxy proxy{live.Endpoint()};
    proxy.advert = 256;
    auto server{TickProxyServer(*this, proxy)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, m_path_root / "stale-ticks-client", error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_CHECK_EQUAL(client->Markets(std::nullopt).size(), 1U);
    // Refused as an unknown field: the same endpoint is read again without
    // them, and the read succeeds.
    const auto requests{proxy.Requests()};
    const auto data{client->Data(market, std::nullopt, TickPage(), error)};
    BOOST_REQUIRE_MESSAGE(data, error);
    BOOST_CHECK_EQUAL(proxy.Requests(), requests + 2);
    BOOST_REQUIRE_EQUAL(proxy.TickParams().size(), 1U);
    BOOST_REQUIRE(data->ticks);
    BOOST_CHECK(!data->ticks->available);
    BOOST_CHECK(client->Status().endpoints.at(0).available);
    BOOST_CHECK(client->Status().endpoints.at(0).last_error.empty());
    // Advertised again, but withdrawn for a while: never sent again.
    for (int read{0}; read < 3; ++read) BOOST_REQUIRE_MESSAGE(client->Data(market, std::nullopt, TickPage(), error), error);
    BOOST_CHECK_EQUAL(proxy.TickParams().size(), 1U);
    client.reset();
}

BOOST_AUTO_TEST_CASE(remote_client_drops_a_contradicting_tick_page_but_not_the_read)
{
    LiveMarket live{*this, m_path_root / "tampered-ticks-service", {}, {256}};
    TickProxy proxy{live.Endpoint()};
    proxy.tamper = true;
    auto server{TickProxyServer(*this, proxy)};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {Endpoint(server->Port())}, m_path_root / "tampered-ticks-client", error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_CHECK_EQUAL(client->Markets(std::nullopt).size(), 1U);
    // The tick at the verified head names another hash: only the ticks go.
    const auto data{client->Data(market, std::nullopt, TickPage(), error)};
    BOOST_REQUIRE_MESSAGE(data, error);
    BOOST_CHECK(data->certificate_verified && data->account_state_verified);
    BOOST_REQUIRE(data->ticks);
    BOOST_CHECK(!data->ticks->available && data->ticks->ticks.empty());
    BOOST_CHECK(data->ticks->unavailable_reason.starts_with("endpoint_clearing_ticks_rejected"));
    {
        std::lock_guard lock{proxy.mutex};
        proxy.tamper = false;
    }
    const auto fixed{client->Data(market, std::nullopt, TickPage(), error)};
    BOOST_REQUIRE_MESSAGE(fixed, error);
    BOOST_REQUIRE(fixed->ticks && fixed->ticks->available);
    BOOST_CHECK(fixed->ticks->ticks.back().microblock_hash == fixed->snapshot.last_microblock_hash);
    client.reset();
}

BOOST_AUTO_TEST_CASE(live_update_reads_fetch_missing_ticks_once_with_an_ordinary_read)
{
    LiveMarket live{*this, m_path_root / "watch-ticks-service", {std::chrono::milliseconds{2000}, 4}, {256}};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {live.Endpoint()}, m_path_root / "watch-ticks-client", error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_CHECK_EQUAL(client->Markets(std::nullopt).size(), 1U);
    const auto held{client->Data(market, std::nullopt, TickPage(), error)};
    BOOST_REQUIRE_MESSAGE(held, error);
    BOOST_REQUIRE(held->ticks && held->ticks->available);
    // A new head verified by a read that asks for no ticks (as a signing
    // preflight's): the held ticks now stop short of it.
    live.Deposit();
    BOOST_REQUIRE(client->Market(market, std::nullopt));
    auto query{TickPage()};
    const auto watch_read = [&] {
        interfaces::FlowMeshMarketWatch watch;
        auto data{client->Watch(market, std::nullopt, query, std::chrono::milliseconds{300}, watch, error)};
        BOOST_REQUIRE_MESSAGE(data, error);
        return std::pair{std::move(*data), watch};
    };
    const auto [caught_up, first]{watch_read()};
    BOOST_CHECK_EQUAL(first.status, "not_eligible");
    BOOST_REQUIRE(caught_up.ticks && caught_up.ticks->available);
    BOOST_CHECK(caught_up.ticks->ticks.back().microblock_hash == caught_up.snapshot.last_microblock_hash);
    // Current again: the next read watches.
    query.known_head = caught_up.snapshot.last_microblock_hash;
    const auto [quiet, second]{watch_read()};
    BOOST_CHECK(second.supported);
    BOOST_REQUIRE(quiet.ticks && quiet.ticks->available);
    client.reset();
}

BOOST_AUTO_TEST_CASE(indicative_clearing_comes_from_verified_state_on_both_backends)
{
    LiveMarket live{*this, m_path_root / "views-service"};
    std::string error;
    auto remote{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {live.Endpoint()}, m_path_root / "views-client", error)};
    BOOST_REQUIRE_MESSAGE(remote, error);
    auto local{node::MakeLocalFlowMeshBackend(*live.service)};
    const uint256 account{uint8_t{0x41}};
    auto query{Page()};
    query.indicative = flowmesh::IndicativeQuery{flowmesh::IndicativeDraft{flowmesh::ClearingEngine::Side::ASK, 5, 1, account}};
    for (auto* backend : {remote.get(), local.get()}) {
        BOOST_TEST_CONTEXT((backend == remote.get() ? "remote" : "local")) {
            const auto data{backend->Data(market, account, query, error)};
            BOOST_REQUIRE_MESSAGE(data, error);
            BOOST_CHECK(!data->book); // The transient copy never leaves the backend.
            BOOST_REQUIRE(data->indicative);
            const auto& view{*data->indicative};
            BOOST_CHECK(view.available);
            BOOST_CHECK(view.head == data->snapshot.last_microblock_hash);
            BOOST_CHECK(view.draft == query.indicative->draft);
            // The verified book is empty: nothing would clear.
            BOOST_CHECK(!view.cleared && !view.draft_replaces_curve && !view.best_bid && !view.best_ask);
            BOOST_CHECK(view.draft_fill_min == CAmount{0} && view.draft_fill_max == CAmount{0});
            const auto json{node::FlowMeshClientMarketDataJson(*data)};
            BOOST_CHECK(!json["indicative"]["certified"].get_bool());
            BOOST_CHECK(json["indicative"]["excludes_pending_actions"].get_bool());
            BOOST_CHECK_EQUAL(json["indicative"]["basis"].get_str(), "verified_standing_curves_with_draft");
            BOOST_CHECK(json["indicative"]["draft"]["assumes_admission"].get_bool());
            BOOST_CHECK(json["indicative"]["draft"]["account_known"].get_bool());
            // A light poll at the same head still answers.
            auto light{query};
            light.known_head = data->snapshot.last_microblock_hash;
            const auto unchanged{backend->Data(market, account, light, error)};
            BOOST_REQUIRE_MESSAGE(unchanged, error);
            BOOST_CHECK(unchanged->unchanged);
            BOOST_REQUIRE(unchanged->indicative);
            BOOST_CHECK(unchanged->indicative->available);
            BOOST_CHECK(unchanged->indicative->head == data->snapshot.last_microblock_hash);
            BOOST_CHECK(node::FlowMeshClientMarketDataJson(*unchanged).exists("indicative"));
            // Not asked for: absent, and the reply is unchanged.
            const auto plain{backend->Data(market, account, Page(), error)};
            BOOST_REQUIRE_MESSAGE(plain, error);
            BOOST_CHECK(!plain->indicative && !plain->ticks);
            BOOST_CHECK(!node::FlowMeshClientMarketDataJson(*plain).exists("indicative"));
        }
    }
    // A new certified head: the remote view follows the newly verified state.
    live.Deposit();
    const auto moved{remote->Data(market, account, query, error)};
    BOOST_REQUIRE_MESSAGE(moved, error);
    BOOST_REQUIRE(moved->indicative);
    BOOST_CHECK_EQUAL(moved->snapshot.next_microblock_sequence, 2U);
    BOOST_CHECK(moved->indicative->head == moved->snapshot.last_microblock_hash);
    remote.reset();
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(flowmesh_api_submit_outcome_tests, ClientLaneFixture)

BOOST_AUTO_TEST_CASE(public_submit_derives_no_outcome_it_would_discard)
{
    LiveMarket live{*this, m_path_root / "submit-outcome-service", {}, {}, {true}};
    const auto action{SignedCancel(0)};
    live.Certify(action);
    BOOST_REQUIRE(live.service->ClientRecentInclusion(market, action.Id()));
    // Anyone may replay a public certified action. The public reply never
    // states an outcome, so none is derived for it: no outcome-part read.
    const std::string params{"{\"market_id\":\"" + market.GetHex() + "\",\"action_id\":\"" + action.Id().GetHex() +
                             "\",\"action_hex\":\"" + HexStr(*flowmesh::EncodeProductionActionPayload(action)) + "\"}"};
    const auto reads{live.service->ClientOutcomeReadsForTest()};
    for (int i{0}; i < 3; ++i) {
        const auto [status, reply]{ApiCall(live.Endpoint(), "submit", params)};
        BOOST_REQUIRE_EQUAL(status, 200);
        BOOST_CHECK_EQUAL(reply["result"]["receipt_state"].get_str(), "certified_inclusion");
        BOOST_CHECK(!reply["result"]["outcome_verified"].get_bool());
        BOOST_CHECK(!reply["result"].exists("outcome"));
    }
    BOOST_CHECK_EQUAL(live.service->ClientOutcomeReadsForTest(), reads);
    // The node's own backend (wallet RPC and Qt) still verifies it.
    const auto local{node::MakeLocalFlowMeshBackend(*live.service)};
    const auto receipt{local->Submit(market, action)};
    BOOST_CHECK_EQUAL(receipt.state, "certified_inclusion");
    BOOST_CHECK(receipt.outcome_verified);
    BOOST_REQUIRE(receipt.outcome);
    BOOST_CHECK_EQUAL(receipt.outcome->status, "rejected"); // nothing to cancel
    BOOST_CHECK_GT(live.service->ClientOutcomeReadsForTest(), reads);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(flowmesh_client_snapshot_outcome_tests, ClientLaneFixture)

BOOST_AUTO_TEST_CASE(snapshot_of_the_inclusion_head_keeps_its_verified_outcome)
{
    LiveMarket live{*this, m_path_root / "snapshot-outcome-service", {}, {}, {true}};
    std::string error;
    auto client{node::MakeRemoteFlowMeshBackend(*m_node.chainman, {live.Endpoint()}, m_path_root / "snapshot-outcome-client", error)};
    BOOST_REQUIRE_MESSAGE(client, error);
    BOOST_REQUIRE_EQUAL(client->Markets(std::nullopt).size(), 1U);
    const auto before{client->Data(market, std::nullopt, Page(), error)};
    BOOST_REQUIRE_MESSAGE(before, error);
    const auto action{SignedCancel(0)};
    const auto submitted{client->Submit(market, action)};
    BOOST_REQUIRE_MESSAGE(submitted.state != "rejected", submitted.reason);
    // The opt-in status read verifies the inclusion and its outcome.
    interfaces::FlowMeshActionReceipt receipt;
    const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{20}};
    while (std::chrono::steady_clock::now() < deadline) {
        receipt = client->ActionStatus(market, action.Id(), false);
        if (receipt.certificate_verified && receipt.outcome_verified) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    BOOST_REQUIRE(receipt.certificate_verified);
    BOOST_REQUIRE(receipt.outcome_verified);
    BOOST_REQUIRE(receipt.outcome);
    BOOST_CHECK_EQUAL(receipt.outcome->status, "rejected"); // nothing to cancel
    // The next read moves the client to the head holding the action through
    // a snapshot, which verifies that inclusion again. The outcome stays
    // verified without any further request.
    const auto moved{client->Data(market, std::nullopt, Page(), error)};
    BOOST_REQUIRE_MESSAGE(moved, error);
    BOOST_REQUIRE(moved->snapshot.last_microblock_hash == receipt.microblock_hash);
    const auto saved{client->SavedActions(action.signer, market)};
    BOOST_REQUIRE_EQUAL(saved.size(), 1U);
    BOOST_CHECK(saved[0].receipt.certificate_verified);
    BOOST_CHECK(saved[0].receipt.microblock_hash == receipt.microblock_hash);
    BOOST_CHECK(saved[0].receipt.outcome_verified);
    BOOST_REQUIRE(saved[0].receipt.outcome);
    BOOST_CHECK_EQUAL(saved[0].receipt.outcome->status, "rejected");
    BOOST_CHECK_EQUAL(saved[0].receipt.reason_code, receipt.reason_code);
    client.reset();
}

BOOST_AUTO_TEST_SUITE_END()
