// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_https.h>

#include <compat/compat.h>
#include <util/sock.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>
#include <event2/dns.h>
#include <event2/event.h>
#include <event2/http.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace node {
namespace {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;
constexpr std::string_view API_PATH{"/flowmesh/v1"};
constexpr size_t MAX_HEADERS{8192};
constexpr size_t MAX_REQUEST{1024 * 1024};
constexpr size_t MAX_REPLY{64 * 1024 * 1024};
constexpr auto MAX_TIMEOUT{std::chrono::seconds{120}};
// Rejection cleanup is not request admission. Bound both decrypted discard and
// underlying socket-read work, including TLS records with no application data.
constexpr size_t MAX_REJECT_DRAIN{MAX_REQUEST + MAX_HEADERS};
constexpr auto REJECT_CLOSE_TIMEOUT{Milliseconds{250}};

template <typename T, auto Free> using Owned = std::unique_ptr<T, decltype(Free)>;
using SslContext = Owned<SSL_CTX, SSL_CTX_free>;
using Ssl = Owned<SSL, SSL_free>;

void FreeDns(evdns_base* dns) { evdns_base_free(dns, 0); }

timeval Timeval(Milliseconds duration)
{
    const auto count{duration.count()};
    return {static_cast<decltype(timeval::tv_sec)>(count / 1000),
            static_cast<decltype(timeval::tv_usec)>((count % 1000) * 1000)};
}

bool NumericAddress(const std::string& host, uint16_t port,
                    sockaddr_storage& address, socklen_t& size)
{
    auto* ipv4{reinterpret_cast<sockaddr_in*>(&address)};
    if (inet_pton(AF_INET, host.c_str(), &ipv4->sin_addr) == 1) {
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = htons(port);
        size = sizeof(sockaddr_in);
        return true;
    }
    address = {};
    auto* ipv6{reinterpret_cast<sockaddr_in6*>(&address)};
    if (inet_pton(AF_INET6, host.c_str(), &ipv6->sin6_addr) == 1) {
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = htons(port);
        size = sizeof(sockaddr_in6);
        return true;
    }
    return false;
}

std::string NumericPeer(const sockaddr_storage& address)
{
    std::array<char, INET6_ADDRSTRLEN> out{};
    const void* source{address.ss_family == AF_INET
        ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(&address)->sin_addr)
        : static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(&address)->sin6_addr)};
    if (!inet_ntop(address.ss_family, source, out.data(), out.size())) return {};
    return out.data();
}

SslContext MakeContext(bool server)
{
    SslContext context{SSL_CTX_new(server ? TLS_server_method() : TLS_client_method()), SSL_CTX_free};
    if (!context || SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) != 1) {
        return {nullptr, SSL_CTX_free};
    }
    SSL_CTX_set_options(context.get(), SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_max_early_data(context.get(), 0); // Never transmit trading actions as replayable TLS 0-RTT.
    SSL_CTX_set_default_passwd_cb(context.get(), [](char*, int, int, void*) { return 0; });
    return context;
}

struct ParsedEndpoint {
    std::string host;
    std::string authority;
    uint16_t port{443};
    bool numeric{false};
};

std::optional<ParsedEndpoint> ParseEndpoint(const HttpsEndpoint& endpoint, const std::string& path)
{
    if (path != API_PATH || endpoint.url.size() > 2048 ||
        endpoint.url.find_first_of("\r\n\t ") != std::string::npos ||
        endpoint.url.find('\0') != std::string::npos) return std::nullopt;
    Owned<evhttp_uri, evhttp_uri_free> uri{evhttp_uri_parse(endpoint.url.c_str()), evhttp_uri_free};
    if (!uri || !evhttp_uri_get_scheme(uri.get()) ||
        std::string_view{evhttp_uri_get_scheme(uri.get())} != "https" ||
        evhttp_uri_get_userinfo(uri.get()) || evhttp_uri_get_query(uri.get()) ||
        evhttp_uri_get_fragment(uri.get())) return std::nullopt;
    const char* url_path{evhttp_uri_get_path(uri.get())};
    if (url_path && *url_path && std::string_view{url_path} != "/" &&
        std::string_view{url_path} != API_PATH) return std::nullopt;
    const char* host{evhttp_uri_get_host(uri.get())};
    if (!host || !*host) return std::nullopt;
    ParsedEndpoint parsed;
    parsed.host = host;
    if (parsed.host.front() == '[' && parsed.host.back() == ']') {
        parsed.host = parsed.host.substr(1, parsed.host.size() - 2);
    }
    if (parsed.host.empty() || parsed.host.find_first_of("%/\\@") != std::string::npos) return std::nullopt;
    const int port{evhttp_uri_get_port(uri.get())};
    if (port != -1) {
        if (port <= 0 || port > 65535) return std::nullopt;
        parsed.port = static_cast<uint16_t>(port);
    }
    sockaddr_storage address{};
    socklen_t size{};
    parsed.numeric = NumericAddress(parsed.host, parsed.port, address, size);
    parsed.authority = parsed.host.find(':') == std::string::npos ? parsed.host : "[" + parsed.host + "]";
    parsed.authority += ":" + std::to_string(parsed.port);
    return parsed;
}

std::optional<std::array<unsigned char, 32>> ParsePin(const std::string& text)
{
    if (text.size() != 64) return std::nullopt;
    std::array<unsigned char, 32> bytes{};
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i{0}; i < bytes.size(); ++i) {
        const int high{nibble(text[2 * i])}, low{nibble(text[2 * i + 1])};
        if (high < 0 || low < 0) return std::nullopt;
        bytes[i] = static_cast<unsigned char>((high << 4) | low);
    }
    return bytes;
}

struct ClientCall {
    event_base* base{nullptr};
    HttpsRequestResult result;
    size_t max_reply{0};
    std::optional<std::array<unsigned char, 32>> pin;
    bool pin_failed{false};
    bool tls_failed{false};
};

int ClientDataIndex()
{
    static const int index{SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr)};
    return index;
}

int VerifyPeer(int verified, X509_STORE_CTX* certificate_context)
{
    auto* ssl{static_cast<SSL*>(X509_STORE_CTX_get_ex_data(
        certificate_context, SSL_get_ex_data_X509_STORE_CTX_idx()))};
    auto* call{ssl ? static_cast<ClientCall*>(SSL_get_ex_data(ssl, ClientDataIndex())) : nullptr};
    if (!verified) {
        if (call) call->tls_failed = true;
        return 0;
    }
    if (!call) return 0;
    if (call->pin && X509_STORE_CTX_get_error_depth(certificate_context) == 0) {
        std::array<unsigned char, 32> digest{};
        unsigned int length{0};
        if (X509_digest(X509_STORE_CTX_get_current_cert(certificate_context), EVP_sha256(),
                        digest.data(), &length) != 1 || length != digest.size() ||
            CRYPTO_memcmp(digest.data(), call->pin->data(), digest.size()) != 0) {
            call->pin_failed = true;
            return 0;
        }
    }
    return 1;
}

void HandshakeInfo(const SSL* ssl, int where, int)
{
    if ((where & SSL_CB_HANDSHAKE_DONE) == 0) return;
    if (auto* call{static_cast<ClientCall*>(SSL_get_ex_data(ssl, ClientDataIndex()))}) {
        // The HTTP writer may run immediately after this callback. Conservatively
        // report unknown outcome after successful TLS, even if later writes fail.
        call->result.request_may_have_been_sent = true;
    }
}

// Socket BIO keeps the server's bounded Sock owner independent of OpenSSL and
// suppresses SIGPIPE without changing process-wide signal handling.
struct SocketBio {
    Sock& socket;
    size_t read_budget{std::numeric_limits<size_t>::max()};
};

const BIO_METHOD* SocketBioMethod()
{
    static const std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> method{[] {
        BIO_METHOD* value{BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "FlowMesh bounded socket")};
        if (!value) return value;
        BIO_meth_set_create(value, [](BIO* bio) { BIO_set_init(bio, 1); return 1; });
        BIO_meth_set_destroy(value, [](BIO*) { return 1; });
        BIO_meth_set_ctrl(value, [](BIO*, int command, long, void*) -> long {
            return command == BIO_CTRL_FLUSH ? 1 : 0;
        });
        BIO_meth_set_read(value, [](BIO* bio, char* data, int length) {
            BIO_clear_retry_flags(bio);
            auto& state{*static_cast<SocketBio*>(BIO_get_data(bio))};
            if (length <= 0 || state.read_budget == 0) return 0;
            const auto result{state.socket.Recv(data,
                std::min(static_cast<size_t>(length), state.read_budget), 0)};
            if (result > 0) state.read_budget -= static_cast<size_t>(result);
            if (result < 0) {
                const int error{WSAGetLastError()};
                if (error == WSAEWOULDBLOCK || error == WSAEAGAIN || error == WSAEINTR) BIO_set_retry_read(bio);
            }
            return static_cast<int>(result);
        });
        BIO_meth_set_write(value, [](BIO* bio, const char* data, int length) {
            BIO_clear_retry_flags(bio);
            const auto result{static_cast<SocketBio*>(BIO_get_data(bio))->socket.Send(data, length, MSG_NOSIGNAL)};
            if (result < 0) {
                const int error{WSAGetLastError()};
                if (error == WSAEWOULDBLOCK || error == WSAEAGAIN || error == WSAEINTR) BIO_set_retry_write(bio);
            }
            return static_cast<int>(result);
        });
        return value;
    }(), BIO_meth_free};
    return method.get();
}

bool ConfigureSocket(Sock& socket)
{
    if (!socket.SetNonBlocking() || !socket.IsSelectable()) return false;
#ifdef SO_NOSIGPIPE
    const int yes{1};
    if (socket.SetSockOpt(SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) != 0) return false;
#endif
    return true;
}

class TlsIo {
    SSL* const m_ssl;
    Sock& m_socket;
    const std::atomic<bool>& m_stopping;
    const Clock::time_point m_deadline;

    bool Again(int result)
    {
        const int error{SSL_get_error(m_ssl, result)};
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) return false;
        if (m_stopping.load() || Clock::now() >= m_deadline) return false;
        const auto remaining{std::chrono::duration_cast<Milliseconds>(m_deadline - Clock::now())};
        return m_socket.Wait(std::clamp(remaining, Milliseconds{1}, Milliseconds{100}),
                             error == SSL_ERROR_WANT_READ ? Sock::RECV : Sock::SEND);
    }

public:
    TlsIo(SSL* ssl, Sock& socket, const std::atomic<bool>& stopping, Clock::time_point deadline)
        : m_ssl{ssl}, m_socket{socket}, m_stopping{stopping}, m_deadline{deadline} {}
    bool Alive() const { return !m_stopping.load() && Clock::now() < m_deadline; }
    bool Handshake()
    {
        while (Alive()) {
            ERR_clear_error();
            const int result{SSL_accept(m_ssl)};
            if (result == 1) return true;
            if (!Again(result)) return false;
        }
        return false;
    }
    int Read(char* data, size_t size)
    {
        while (Alive()) {
            ERR_clear_error();
            const int result{SSL_read(m_ssl, data, static_cast<int>(std::min<size_t>(size, 16384)))};
            if (result > 0) return result;
            if (!Again(result)) break;
        }
        return -1;
    }
    bool Write(std::string_view data)
    {
        while (!data.empty() && Alive()) {
            ERR_clear_error();
            const int result{SSL_write(m_ssl, data.data(), static_cast<int>(std::min<size_t>(data.size(), 16384)))};
            if (result > 0) data.remove_prefix(result);
            else if (!Again(result)) return false;
        }
        return data.empty();
    }
    std::pair<size_t, size_t> FinishRejectedResponse()
    {
        // Closing the socket with unread request ciphertext can reset TCP and
        // erase the already-written 413 at the HTTP client. Send close-notify,
        // then discard (never parse/dispatch) incoming TLS data while the peer
        // receives that response and closes. A hostile/silent peer cannot turn
        // this into unbounded body consumption or extend the request deadline.
        TlsIo closing{m_ssl, m_socket, m_stopping,
            std::min(m_deadline, Clock::now() + REJECT_CLOSE_TIMEOUT)};
        auto& bio{*static_cast<SocketBio*>(BIO_get_data(SSL_get_rbio(m_ssl)))};
        bio.read_budget = MAX_REJECT_DRAIN;
        size_t remaining{MAX_REJECT_DRAIN};
        const auto consumed = [&] {
            return std::pair{MAX_REJECT_DRAIN - bio.read_budget, MAX_REJECT_DRAIN - remaining};
        };
        while (closing.Alive()) {
            ERR_clear_error();
            const int result{SSL_shutdown(m_ssl)};
            if (result == 1) return consumed();
            if (result == 0) break; // close-notify sent; peer has not closed yet.
            if (!closing.Again(result)) return consumed();
        }
        std::array<char, 4096> discarded{};
        while (remaining != 0 && closing.Alive()) {
            const int size{closing.Read(discarded.data(), std::min(discarded.size(), remaining))};
            if (size <= 0) return consumed();
            remaining -= static_cast<size_t>(size);
        }
        return consumed();
    }
};

std::string_view TrimHeader(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

bool ReadRequest(TlsIo& io, size_t max_body, FlowMeshHttpsServer::Request& request, int& status)
{
    std::string received;
    size_t end{std::string::npos};
    std::array<char, 4096> chunk{};
    status = 400;
    while ((end = received.find("\r\n\r\n")) == std::string::npos) {
        if (received.size() >= MAX_HEADERS) { status = 431; return false; }
        const int size{io.Read(chunk.data(), std::min(chunk.size(), MAX_HEADERS - received.size()))};
        if (size <= 0) return false;
        received.append(chunk.data(), size);
    }
    std::string_view headers{received.data(), end};
    const size_t first{headers.find("\r\n")};
    if (first == std::string_view::npos) return false;
    if (headers.substr(0, first) != "POST /flowmesh/v1 HTTP/1.1") {
        status = headers.starts_with("POST ") ? 404 : 405;
        return false;
    }
    headers.remove_prefix(first + 2);
    std::map<std::string, std::string_view> fields;
    while (!headers.empty()) {
        const size_t line_end{headers.find("\r\n")};
        const auto line{headers.substr(0, line_end)};
        const size_t colon{line.find(':')};
        if (colon == 0 || colon == std::string_view::npos) return false;
        std::string name{line.substr(0, colon)};
        for (char& c : name) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '-') return false;
        }
        const auto value{TrimHeader(line.substr(colon + 1))};
        for (const unsigned char c : value) if ((c < 32 && c != '\t') || c == 127) return false;
        if (!fields.emplace(std::move(name), value).second || fields.size() > 32) return false;
        if (line_end == std::string_view::npos) break;
        headers.remove_prefix(line_end + 2);
    }
    if (!fields.contains("host") || fields.at("host").empty() ||
        !fields.contains("content-length") || fields.contains("transfer-encoding") ||
        fields.contains("expect") || fields.contains("content-encoding")) return false;
    size_t length{0};
    const auto value{fields.at("content-length")};
    const auto parsed{std::from_chars(value.data(), value.data() + value.size(), length)};
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || value.empty()) return false;
    if (length > max_body) { status = 413; return false; }
    request.path = API_PATH;
    request.body = received.substr(end + 4);
    if (request.body.size() > length) return false; // No pipelined requests.
    while (request.body.size() < length) {
        const int size{io.Read(chunk.data(), std::min(chunk.size(), length - request.body.size()))};
        if (size <= 0) return false;
        request.body.append(chunk.data(), size);
    }
    return true;
}

} // namespace

bool ValidateFlowMeshHttpsEndpoint(const HttpsEndpoint& endpoint, std::string& error)
{
    if (!ParseEndpoint(endpoint, std::string{API_PATH})) {
        error = "HTTPS endpoint must be an origin or /flowmesh/v1 URL without credentials, query or fragment";
        return false;
    }
    if (!endpoint.certificate_sha256.empty() && !ParsePin(endpoint.certificate_sha256)) {
        error = "HTTPS certificate pin must contain exactly 64 hexadecimal characters";
        return false;
    }
    return true;
}

bool ValidateFlowMeshHttpsTrust(const HttpsEndpoint& endpoint, std::string& error)
{
    auto context{MakeContext(false)};
    if (!context) { error = "Unable to initialize HTTPS trust"; return false; }
    if ((endpoint.ca_file.empty() ? SSL_CTX_set_default_verify_paths(context.get())
        : SSL_CTX_load_verify_locations(context.get(), fs::PathToString(endpoint.ca_file).c_str(), nullptr)) != 1) {
        error = "HTTPS CA file is missing, unreadable or malformed";
        return false;
    }
    return true;
}

bool NormalizeFlowMeshHttpsEndpoint(HttpsEndpoint& endpoint, std::string& error)
{
    if (!ValidateFlowMeshHttpsEndpoint(endpoint, error)) return false;
    const auto parsed{ParseEndpoint(endpoint, std::string{API_PATH})};
    std::string host{parsed->host};
    if (parsed->numeric) {
        sockaddr_storage address{};
        socklen_t size{};
        if (!NumericAddress(host, parsed->port, address, size)) return false;
        host = NumericPeer(address);
    } else {
        for (char& c : host) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    endpoint.url = "https://" + (host.find(':') == std::string::npos ? host : "[" + host + "]");
    if (parsed->port != 443) endpoint.url += ":" + std::to_string(parsed->port);
    return true;
}

HttpsRequestResult FlowMeshHttpsRequest(const HttpsEndpoint& endpoint, const std::string& path,
                                      const std::string& body, Milliseconds timeout, size_t max_reply_bytes)
{
    ClientCall call;
    call.max_reply = max_reply_bytes;
    const auto start{Clock::now()};
    const auto parsed{ParseEndpoint(endpoint, path)};
    if (!parsed || timeout <= Milliseconds{0} || timeout > MAX_TIMEOUT ||
        max_reply_bytes == 0 || max_reply_bytes > MAX_REPLY || body.size() > MAX_REQUEST) {
        call.result.error = "https-invalid-endpoint-or-bounds";
        return call.result;
    }
    if (!endpoint.certificate_sha256.empty()) {
        call.pin = ParsePin(endpoint.certificate_sha256);
        if (!call.pin) { call.result.error = "https-invalid-certificate-pin"; return call.result; }
    }
    auto context{MakeContext(false)};
    if (!context || ClientDataIndex() < 0) { call.result.error = "https-tls-initialization-failed"; return call.result; }
    SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, VerifyPeer);
    if ((endpoint.ca_file.empty() ? SSL_CTX_set_default_verify_paths(context.get())
        : SSL_CTX_load_verify_locations(context.get(), fs::PathToString(endpoint.ca_file).c_str(), nullptr)) != 1) {
        call.result.error = "https-ca-load-failed";
        return call.result;
    }
    Owned<event_base, event_base_free> base{event_base_new(), event_base_free};
    if (!base) { call.result.error = "https-event-base-failed"; return call.result; }
    call.base = base.get();
    Owned<evdns_base, FreeDns> dns{evdns_base_new(base.get(), 1), FreeDns};
    if (!dns) { call.result.error = "https-dns-initialization-failed"; return call.result; }
    Ssl ssl{SSL_new(context.get()), SSL_free};
    if (!ssl || SSL_set_ex_data(ssl.get(), ClientDataIndex(), &call) != 1) {
        call.result.error = "https-tls-initialization-failed"; return call.result;
    }
    X509_VERIFY_PARAM* verify{SSL_get0_param(ssl.get())};
    X509_VERIFY_PARAM_set_hostflags(verify, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    const bool identity_ok{parsed->numeric
        ? X509_VERIFY_PARAM_set1_ip_asc(verify, parsed->host.c_str()) == 1
        : SSL_set1_host(ssl.get(), parsed->host.c_str()) == 1 &&
          SSL_set_tlsext_host_name(ssl.get(), parsed->host.c_str()) == 1};
    if (!identity_ok) { call.result.error = "https-invalid-server-identity"; return call.result; }
    SSL_set_info_callback(ssl.get(), HandshakeInfo);
    Owned<bufferevent, bufferevent_free> bev{bufferevent_openssl_socket_new(
        base.get(), -1, ssl.get(), BUFFEREVENT_SSL_CONNECTING, BEV_OPT_CLOSE_ON_FREE), bufferevent_free};
    if (!bev) { call.result.error = "https-tls-buffer-failed"; return call.result; }
    ssl.release(); // Owned by the SSL bufferevent from now on.
    Owned<evhttp_connection, evhttp_connection_free> connection{evhttp_connection_base_bufferevent_new(
        base.get(), dns.get(), bev.get(), parsed->host.c_str(), parsed->port), evhttp_connection_free};
    if (!connection) { call.result.error = "https-connection-creation-failed"; return call.result; }
    bev.release();
    evhttp_connection_set_retries(connection.get(), 0); // Economic retry belongs to the client backend.
    evhttp_connection_set_max_headers_size(connection.get(), MAX_HEADERS);
    evhttp_connection_set_max_body_size(connection.get(), max_reply_bytes);
    const auto limit{Timeval(timeout)};
    evhttp_connection_set_timeout_tv(connection.get(), &limit);
    auto* request{evhttp_request_new([](evhttp_request* req, void* argument) {
        auto& current{*static_cast<ClientCall*>(argument)};
        if (req) {
            const auto status{evhttp_request_get_response_code(req)};
            auto* input{evhttp_request_get_input_buffer(req)};
            const size_t size{evbuffer_get_length(input)};
            if (size <= current.max_reply && status >= 200 && status <= 599) {
                current.result.response_received = true;
                current.result.status = status;
                current.result.body.resize(size);
                if (size) evbuffer_remove(input, current.result.body.data(), size);
                if (status >= 300 && status < 400) current.result.error = "https-redirect-refused";
            } else current.result.error = "https-invalid-or-oversized-reply";
        } else if (current.result.error.empty()) current.result.error = "https-transport-failed";
        event_base_loopbreak(current.base);
    }, &call)};
    if (!request) { call.result.error = "https-request-allocation-failed"; return call.result; }
    evhttp_request_set_error_cb(request, [](evhttp_request_error error, void* argument) {
        auto& current{*static_cast<ClientCall*>(argument)};
        current.result.error = error == EVREQ_HTTP_DATA_TOO_LONG ? "https-reply-too-large"
            : error == EVREQ_HTTP_TIMEOUT ? "https-deadline" : "https-transport-failed";
    });
    auto* headers{evhttp_request_get_output_headers(request)};
    evhttp_add_header(headers, "Host", parsed->authority.c_str());
    evhttp_add_header(headers, "Content-Type", "application/json");
    evhttp_add_header(headers, "Connection", "close");
    evhttp_add_header(headers, "Content-Length", std::to_string(body.size()).c_str());
    evbuffer_add(evhttp_request_get_output_buffer(request), body.data(), body.size());
    const auto remaining{timeout - std::chrono::duration_cast<Milliseconds>(Clock::now() - start)};
    if (remaining <= Milliseconds{0}) {
        evhttp_request_free(request);
        call.result.error = "https-deadline";
        return call.result;
    }
    Owned<event, event_free> deadline{evtimer_new(base.get(), [](evutil_socket_t, short, void* argument) {
        auto& current{*static_cast<ClientCall*>(argument)};
        current.result.error = "https-deadline";
        event_base_loopbreak(current.base);
    }, &call), event_free};
    const auto remaining_tv{Timeval(remaining)};
    if (!deadline || evtimer_add(deadline.get(), &remaining_tv) != 0) {
        evhttp_request_free(request);
        call.result.error = "https-deadline-setup-failed";
        return call.result;
    }
    // libevent owns/frees request whether make_request succeeds or fails.
    if (evhttp_make_request(connection.get(), request, EVHTTP_REQ_POST, path.c_str()) != 0) {
        call.result.error = "https-request-setup-failed";
    } else if (event_base_dispatch(base.get()) < 0) {
        call.result.error = "https-event-loop-failed";
    }
    if (call.pin_failed) call.result.error = "https-certificate-pin-mismatch";
    else if (call.tls_failed) call.result.error = "https-certificate-verification-failed";
    return call.result;
}

struct FlowMeshHttpsServer::Impl {
    struct Pending {
        std::unique_ptr<Sock> socket;
        std::string peer;
        Clock::time_point deadline;
    };
    Options options;
    Handler handler;
    SslContext context{nullptr, SSL_CTX_free};
    std::unique_ptr<Sock> listener;
    SOCKET listener_fd{INVALID_SOCKET};
    std::atomic<uint16_t> port{0};
    std::atomic<bool> stopping{true};
    std::atomic<size_t> connections{0};
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<Pending> queue;
    std::thread accept_thread;
    std::vector<std::thread> workers;
    // Empty in production. Test observation occurs only after immutable
    // cleanup has completed; it cannot alter the operation being measured.
    std::function<void(size_t, size_t)> rejected_cleanup_observer;

    Impl(Options opts, Handler fn) : options{std::move(opts)}, handler{std::move(fn)} {}

    void Serve(Pending pending)
    {
        if (stopping.load() || Clock::now() >= pending.deadline) return;
        SocketBio socket_bio{*pending.socket};
        Ssl ssl{SSL_new(context.get()), SSL_free};
        const BIO_METHOD* method{SocketBioMethod()};
        if (!ssl || !method) return;
        BIO* bio{BIO_new(method)};
        if (!bio) return;
        BIO_set_data(bio, &socket_bio);
        SSL_set_bio(ssl.get(), bio, bio);
        TlsIo io{ssl.get(), *pending.socket, stopping, pending.deadline};
        if (!io.Handshake()) return;
        Request request;
        request.remote_address = std::move(pending.peer);
        Response response;
        int failure{400};
        const bool rejected{!ReadRequest(io, options.max_request_bytes, request, failure)};
        if (rejected) {
            response = {failure, "{\"error\":\"invalid-http-request\"}"};
        } else if (!io.Alive()) return;
        else {
            try { response = handler(request); }
            catch (...) { response = {500, "{\"error\":\"handler-failed\"}"}; }
        }
        if (!io.Alive()) return; // Handler may have admitted; caller must treat timeout as unknown.
        if (response.status < 200 || response.status > 599 || response.body.size() > options.max_reply_bytes) {
            response = {500, "{\"error\":\"reply-out-of-bounds\"}"};
        }
        const std::string header{"HTTP/1.1 " + std::to_string(response.status) +
            " Response\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(response.body.size()) + "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n"};
        if (io.Write(header) && io.Write(response.body)) {
            if (rejected) {
                const auto [ciphertext, plaintext]{io.FinishRejectedResponse()};
                if (rejected_cleanup_observer) rejected_cleanup_observer(ciphertext, plaintext);
            }
            else SSL_shutdown(ssl.get());
        }
    }

    void Worker()
    {
        while (true) {
            Pending pending;
            {
                std::unique_lock lock{mutex};
                condition.wait(lock, [&] { return stopping.load() || !queue.empty(); });
                if (stopping.load()) return;
                pending = std::move(queue.front());
                queue.pop_front();
            }
            try { Serve(std::move(pending)); } catch (...) { /* Close, never expose an exception body. */ }
            --connections;
        }
    }

    void Accept()
    {
        while (!stopping.load()) {
            Sock::Event occurred{0};
            if (!listener->Wait(Milliseconds{100}, Sock::RECV, &occurred)) break;
            if (!(occurred & Sock::RECV)) continue;
            sockaddr_storage address{};
            socklen_t size{sizeof(address)};
            const SOCKET fd{static_cast<SOCKET>(::accept(listener_fd, reinterpret_cast<sockaddr*>(&address), &size))};
            if (fd == INVALID_SOCKET) continue;
            auto socket{std::make_unique<Sock>(fd)};
            if (!ConfigureSocket(*socket)) continue;
            std::lock_guard lock{mutex};
            if (stopping.load() || queue.size() >= options.max_queue ||
                connections.load() >= options.max_connections) continue;
            ++connections;
            queue.push_back({std::move(socket), NumericPeer(address), Clock::now() + options.request_timeout});
            condition.notify_one();
        }
    }
};

FlowMeshHttpsServer::FlowMeshHttpsServer(Options options, Handler handler)
    : m_impl{std::make_unique<Impl>(std::move(options), std::move(handler))} {}
FlowMeshHttpsServer::~FlowMeshHttpsServer() { Stop(); }

bool FlowMeshHttpsServer::SetRejectedCleanupObserverForTest(std::function<void(size_t, size_t)> observer)
{
    // Start/Stop and this private test setup are externally serialized. Never
    // replace an observer while a worker can be executing it.
    if (!m_impl->stopping.load() || !m_impl->workers.empty() || m_impl->accept_thread.joinable()) return false;
    m_impl->rejected_cleanup_observer = std::move(observer);
    return true;
}

bool FlowMeshHttpsServer::Start(std::string& error)
{
    auto& state{*m_impl};
    if (!state.stopping.load()) return true;
    const auto& options{state.options};
    sockaddr_storage address{};
    socklen_t size{};
    if (!state.handler || !NumericAddress(options.bind_host, options.port, address, size) ||
        options.max_request_bytes == 0 || options.max_request_bytes > MAX_REQUEST ||
        options.max_reply_bytes < 64 || options.max_reply_bytes > MAX_REPLY ||
        options.max_connections == 0 || options.max_connections > 256 ||
        options.worker_threads == 0 || options.worker_threads > 16 ||
        options.max_queue == 0 || options.max_queue > 256 ||
        options.request_timeout <= Milliseconds{0} || options.request_timeout > MAX_TIMEOUT) {
        error = "https-server-invalid-options";
        return false;
    }
    state.context = MakeContext(true);
    if (!state.context ||
        SSL_CTX_use_certificate_chain_file(state.context.get(), fs::PathToString(options.cert_file).c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(state.context.get(), fs::PathToString(options.key_file).c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(state.context.get()) != 1) {
        state.context.reset();
        error = "https-server-certificate-or-key-invalid";
        return false;
    }
    const SOCKET fd{static_cast<SOCKET>(::socket(address.ss_family, SOCK_STREAM, IPPROTO_TCP))};
    if (fd == INVALID_SOCKET) { error = "https-server-socket-failed"; return false; }
    state.listener = std::make_unique<Sock>(fd);
    state.listener_fd = fd;
    const int yes{1};
    (void)state.listener->SetSockOpt(SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (!ConfigureSocket(*state.listener) ||
        state.listener->Bind(reinterpret_cast<const sockaddr*>(&address), size) != 0 ||
        state.listener->Listen(static_cast<int>(options.max_connections)) != 0 ||
        state.listener->GetSockName(reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        state.listener.reset();
        error = "https-server-bind-failed";
        return false;
    }
    state.port = address.ss_family == AF_INET ? ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port)
                                               : ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
    state.stopping = false;
    try {
        for (size_t i{0}; i < options.worker_threads; ++i) state.workers.emplace_back([&state] { state.Worker(); });
        state.accept_thread = std::thread{[&state] { state.Accept(); }};
    } catch (...) {
        Stop();
        error = "https-server-thread-start-failed";
        return false;
    }
    return true;
}

void FlowMeshHttpsServer::Stop()
{
    auto& state{*m_impl};
    state.stopping = true;
    state.condition.notify_all();
    if (state.accept_thread.joinable()) state.accept_thread.join();
    for (auto& worker : state.workers) if (worker.joinable()) worker.join();
    state.workers.clear();
    {
        std::lock_guard lock{state.mutex};
        state.queue.clear();
        state.connections = 0;
    }
    state.listener.reset();
    state.listener_fd = INVALID_SOCKET;
    state.context.reset();
    state.port = 0;
}

uint16_t FlowMeshHttpsServer::Port() const { return m_impl->port.load(); }

} // namespace node
