// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_https.h>
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

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
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

    bool Again(int result, Clock::time_point deadline)
    {
        const int error{SSL_get_error(m_ssl.get(), result)};
        if (Clock::now() >= deadline ||
            (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)) return false;
        return m_socket.Wait(std::chrono::milliseconds{10},
            error == SSL_ERROR_WANT_READ ? Sock::RECV : Sock::SEND);
    }

public:
    SplitHttpsPeer(uint16_t port, const fs::path& certificate)
    {
        BOOST_REQUIRE(m_fd != INVALID_SOCKET);
        BOOST_REQUIRE(m_context);
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

    std::string ReadRejection()
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
            if (end != std::string::npos && response.size() >= end + 4 + 32) break;
        }
        return response;
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
