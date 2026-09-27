// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_https.h>
#include <node/flowmesh_timing.h>

#include <compat/compat.h>
#include <util/sock.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>
#include <event2/dns.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <event2/util.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstdio>
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
// Maximum TLS record plaintext; each SSL_write bounded by it is one record.
constexpr size_t MAX_TLS_RECORD_PLAINTEXT{16384};

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
    FlowMeshTimingSpan* timing{nullptr};
    event_base* base{nullptr};
    HttpsRequestResult result;
    size_t max_reply{0};
    bool complete{false};
    bool response_reusable{false};
    bool callback_failed{false};
};

// SSL callbacks always reference this session-owned object. The active call is
// attached only while its stack frame and absolute-deadline event are alive.
struct ClientSession {
    ClientCall* active{nullptr};
    std::optional<std::array<unsigned char, 32>> pin;
    bool pin_failed{false};
    bool tls_failed{false};
    bool handshake_done{false};
    bool connection_closed{false};
    // Descriptor on which TCP_NODELAY was last set; -1 when none is.
    int nodelay_fd{-1};
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
    auto* session{ssl ? static_cast<ClientSession*>(SSL_get_ex_data(ssl, ClientDataIndex())) : nullptr};
    if (!verified) {
        if (session) session->tls_failed = true;
        return 0;
    }
    if (!session) return 0;
    if (session->pin && X509_STORE_CTX_get_error_depth(certificate_context) == 0) {
        std::array<unsigned char, 32> digest{};
        unsigned int length{0};
        if (X509_digest(X509_STORE_CTX_get_current_cert(certificate_context), EVP_sha256(),
                        digest.data(), &length) != 1 || length != digest.size() ||
            CRYPTO_memcmp(digest.data(), session->pin->data(), digest.size()) != 0) {
            session->pin_failed = true;
            return 0;
        }
    }
    return 1;
}

void HandshakeInfo(const SSL* ssl, int where, int)
{
    auto* session{static_cast<ClientSession*>(SSL_get_ex_data(ssl, ClientDataIndex()))};
    if (!session) return;
    // Latency only, never a trust condition. evhttp/bufferevent create this
    // socket and expose no NODELAY option; libevent attaches the connected
    // socket BIO before it starts the handshake, so the first info event of a
    // connection sees it. A new handshake implies a new socket even when the
    // descriptor number was reused, so it is configured again.
    if (where & SSL_CB_HANDSHAKE_START) session->nodelay_fd = -1;
    if (const int fd{SSL_get_fd(ssl)}; fd >= 0 && fd != session->nodelay_fd) {
        const int one{1};
        const bool set{setsockopt(static_cast<evutil_socket_t>(fd), IPPROTO_TCP, TCP_NODELAY,
                                  reinterpret_cast<const char*>(&one), sizeof(one)) == 0};
        session->nodelay_fd = set ? fd : -1;
        if (session->active) session->active->result.tcp_nodelay = set;
    }
    if ((where & SSL_CB_HANDSHAKE_DONE) == 0) return;
    session->handshake_done = true;
    auto* call{session->active};
    if (!call) return;
    // The HTTP writer may run immediately after this callback. Conservatively
    // report unknown outcome after successful TLS, even if later writes fail.
    call->result.request_may_have_been_sent = true;
    call->result.tls_handshake_performed = true;
    if (call->timing) call->timing->Mark("tls_handshake_done_us");
}

bool FingerprintCa(const fs::path& path, Clock::time_point deadline,
                   std::optional<std::array<unsigned char, 32>>& digest)
{
    if (path.empty()) { digest.reset(); return true; }
    // Detect replacement even when the filename, size and timestamp are kept.
    // Only regular public trust files are accepted; memory is bounded and the
    // deadline is checked between reads. As with OpenSSL's CA load, a blocked
    // filesystem read itself cannot be interrupted by the network deadline.
    std::error_code file_error;
    if (!fs::is_regular_file(path, file_error) || file_error) return false;
    Owned<FILE, fclose> input{fsbridge::fopen(path, "rb"), fclose};
    Owned<EVP_MD_CTX, EVP_MD_CTX_free> hash{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    if (!input || !hash || EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1) return false;
    std::array<unsigned char, 4096> chunk{};
    while (Clock::now() < deadline) {
        const size_t size{fread(chunk.data(), 1, chunk.size(), input.get())};
        if (size && EVP_DigestUpdate(hash.get(), chunk.data(), size) != 1) return false;
        if (ferror(input.get())) return false;
        if (feof(input.get())) {
            digest.emplace();
            unsigned int length{0};
            return EVP_DigestFinal_ex(hash.get(), digest->data(), &length) == 1 && length == digest->size();
        }
    }
    return false;
}

bool ClientCertificatesValid(SSL* ssl)
{
    if (!ssl || SSL_get_verify_result(ssl) != X509_V_OK) return false;
    const auto* chain{SSL_get0_verified_chain(ssl)};
    if (!chain || sk_X509_num(chain) == 0) return false;
    for (int i{0}; i < sk_X509_num(chain); ++i) {
        const auto* certificate{sk_X509_value(chain, i)};
        if (X509_cmp_current_time(X509_get0_notBefore(certificate)) != -1 ||
            X509_cmp_current_time(X509_get0_notAfter(certificate)) != 1) return false;
    }
    return true;
}

bool ClientReplyReusable(evhttp_request* request, size_t size)
{
    const auto* headers{evhttp_request_get_input_headers(request)};
    bool has_length{false};
    for (auto* field{headers->tqh_first}; field; field = field->next.tqe_next) {
        std::string name{field->key};
        for (char& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (name == "transfer-encoding") return false;
        if (name == "content-length") {
            if (has_length) return false;
            has_length = true;
            const std::string_view value{field->value};
            size_t declared{0};
            const auto parsed{std::from_chars(value.data(), value.data() + value.size(), declared)};
            if (value.empty() || parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size() || declared != size) return false;
        }
        if (name == "connection") {
            std::string value{field->value};
            for (char& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (value.find("close") != std::string::npos) return false;
        }
    }
    return has_length;
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
            const int result{SSL_read(m_ssl, data, static_cast<int>(std::min(size, MAX_TLS_RECORD_PLAINTEXT)))};
            if (result > 0) return result;
            if (!Again(result)) break;
        }
        return -1;
    }
    bool PendingInput() const
    {
        // This restricted endpoint does not admit pipelining. Include bytes in
        // OpenSSL and later TLS records still on the socket, rather than only
        // surplus plaintext returned in the final ReadRequest() read.
        if (SSL_pending(m_ssl) != 0 || SSL_has_pending(m_ssl)) return true;
        char next;
        return m_socket.Recv(&next, 1, MSG_PEEK) > 0;
    }
    bool SurplusRequest(bool& keep_alive)
    {
        if (SSL_pending(m_ssl) != 0) return true;
        if (!PendingInput()) return false;
        // Pending ciphertext can be TLS control data, close-notify, or a
        // partial record. Never reuse it ambiguously; only positive plaintext
        // is sufficient to reject the already framed request as pipelined.
        keep_alive = false;
        auto& bio{*static_cast<SocketBio*>(BIO_get_data(SSL_get_rbio(m_ssl)))};
        const size_t previous{bio.read_budget};
        const size_t budget{std::min(previous, size_t{32 * 1024})};
        bio.read_budget = budget;
        char next;
        ERR_clear_error();
        const int result{SSL_peek(m_ssl, &next, 1)};
        bio.read_budget = previous - (budget - bio.read_budget);
        return result > 0;
    }
    bool Write(std::string_view data, bool* subsequent_input = nullptr)
    {
        while (!data.empty() && Alive()) {
            // Check before writes, never after the final write: a legitimate
            // sequential client may immediately send its next request after
            // receiving the complete response. Input before completion closes
            // this connection after the response and is never redispatched.
            if (subsequent_input && PendingInput()) *subsequent_input = true;
            ERR_clear_error();
            const int result{SSL_write(m_ssl, data.data(), static_cast<int>(std::min(data.size(), MAX_TLS_RECORD_PLAINTEXT)))};
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

bool ReadRequest(TlsIo& io, size_t max_body, FlowMeshHttpsServer::Request& request,
                 int& status, bool& keep_alive)
{
    std::string received;
    size_t end{std::string::npos};
    std::array<char, 4096> chunk{};
    status = 400;
    keep_alive = true; // HTTP/1.1 defaults to persistence unless close is requested.
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
    if (const auto connection{fields.find("connection")}; connection != fields.end()) {
        std::string_view remaining{connection->second};
        do {
            const auto comma{remaining.find(',')};
            std::string token{TrimHeader(remaining.substr(0, comma))};
            if (token.empty()) return false;
            for (char& c : token) {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') &&
                    std::string_view{"!#$%&'*+-.^_`|~"}.find(c) == std::string_view::npos) return false;
            }
            if (token == "close") keep_alive = false;
            if (comma == std::string_view::npos) break;
            remaining.remove_prefix(comma + 1);
            if (remaining.empty()) return false;
        } while (true);
    }
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
    return !io.SurplusRequest(keep_alive);
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

struct FlowMeshHttpsClient::Impl {
    // Reuse ends strictly before the default server limits, so a warm request
    // (reported as possibly sent) is never written into a connection that the
    // server is closing. The server's idle clock starts before the reply
    // reaches the client, and near the end of its lifetime the server clips a
    // request's deadline to that lifetime; the margins cover a round trip.
    static constexpr auto MAX_IDLE{std::chrono::seconds{25}};
    static constexpr auto MAX_AGE{std::chrono::seconds{285}};
    static constexpr size_t MAX_REQUESTS{100};
    static_assert(MAX_IDLE + std::chrono::seconds{5} <= FLOWMESH_HTTPS_DEFAULT_IDLE_TIMEOUT);
    static_assert(MAX_AGE + FLOWMESH_HTTPS_DEFAULT_REQUEST_TIMEOUT + std::chrono::seconds{5} <=
                  FLOWMESH_HTTPS_DEFAULT_CONNECTION_LIFETIME);
    const bool keep_alive;
    ClientSession session;
    HttpsEndpoint endpoint;
    std::optional<std::array<unsigned char, 32>> ca_fingerprint;
    SslContext context{nullptr, SSL_CTX_free};
    Owned<event_base, event_base_free> base{nullptr, event_base_free};
    Owned<evdns_base, FreeDns> dns{nullptr, FreeDns};
    Owned<evhttp_connection, evhttp_connection_free> connection{nullptr, evhttp_connection_free};
    Clock::time_point created{}, last_used{};
    size_t requests{0};

    explicit Impl(bool retain) : keep_alive{retain} {}

    void Reset()
    {
        // Destroy callback owners before their data and event/DNS bases.
        connection.reset();
        dns.reset();
        base.reset();
        context.reset();
        session = {};
        endpoint = {};
        ca_fingerprint.reset();
        requests = 0;
    }

    bool Reusable()
    {
        if (!keep_alive || !connection || session.connection_closed || !session.handshake_done ||
            requests >= MAX_REQUESTS || Clock::now() - created >= MAX_AGE ||
            Clock::now() - last_used >= MAX_IDLE) return false;
        // Process idle EOF and unsolicited bytes before making another request.
        // A close callback permanently disqualifies this evhttp object: calling
        // make_request on a reset object can otherwise reconnect using a fresh,
        // plain bufferevent in libevent. No reset connection is ever dispatched.
        if (event_base_loop(base.get(), EVLOOP_NONBLOCK) < 0 || session.connection_closed) return false;
        auto* bev{evhttp_connection_get_bufferevent(connection.get())};
        if (!bev || bufferevent_getfd(bev) == -1 ||
            evbuffer_get_length(bufferevent_get_input(bev)) != 0 ||
            evbuffer_get_length(bufferevent_get_output(bev)) != 0) return false;
        SSL* ssl{bufferevent_openssl_get_ssl(bev)};
        return ClientCertificatesValid(ssl) && SSL_pending(ssl) == 0 &&
            SSL_has_pending(ssl) == 0 && SSL_get_shutdown(ssl) == 0;
    }

    bool Initialize(const HttpsEndpoint& configured, const ParsedEndpoint& parsed,
                    std::optional<std::array<unsigned char, 32>> pin,
                    std::optional<std::array<unsigned char, 32>> fingerprint,
                    Clock::time_point expires, ClientCall& call)
    {
        session.pin = pin;
        endpoint = configured;
        ca_fingerprint = fingerprint;
        if (call.timing) call.timing->Mark("context_started_us");
        context = MakeContext(false);
        if (!context || ClientDataIndex() < 0) {
            call.result.error = "https-tls-initialization-failed"; return false;
        }
        SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, VerifyPeer);
        if ((endpoint.ca_file.empty() ? SSL_CTX_set_default_verify_paths(context.get())
            : SSL_CTX_load_verify_locations(context.get(), fs::PathToString(endpoint.ca_file).c_str(), nullptr)) != 1) {
            call.result.error = "https-ca-load-failed"; return false;
        }
        std::optional<std::array<unsigned char, 32>> loaded_fingerprint;
        if (!FingerprintCa(endpoint.ca_file, expires, loaded_fingerprint) || loaded_fingerprint != fingerprint) {
            call.result.error = Clock::now() >= expires ? "https-deadline" : "https-ca-changed-during-load";
            return false;
        }
        if (call.timing) call.timing->Mark("ca_ready_us");
        base.reset(event_base_new());
        if (!base) { call.result.error = "https-event-base-failed"; return false; }
        dns.reset(evdns_base_new(base.get(), 1));
        if (!dns) { call.result.error = "https-dns-initialization-failed"; return false; }
        Ssl ssl{SSL_new(context.get()), SSL_free};
        if (!ssl || SSL_set_ex_data(ssl.get(), ClientDataIndex(), &session) != 1) {
            call.result.error = "https-tls-initialization-failed"; return false;
        }
        X509_VERIFY_PARAM* verify{SSL_get0_param(ssl.get())};
        X509_VERIFY_PARAM_set_hostflags(verify, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        const bool identity_ok{parsed.numeric
            ? X509_VERIFY_PARAM_set1_ip_asc(verify, parsed.host.c_str()) == 1
            : SSL_set1_host(ssl.get(), parsed.host.c_str()) == 1 &&
              SSL_set_tlsext_host_name(ssl.get(), parsed.host.c_str()) == 1};
        if (!identity_ok) { call.result.error = "https-invalid-server-identity"; return false; }
        SSL_set_info_callback(ssl.get(), HandshakeInfo);
        Owned<bufferevent, bufferevent_free> bev{bufferevent_openssl_socket_new(
            base.get(), -1, ssl.get(), BUFFEREVENT_SSL_CONNECTING, BEV_OPT_CLOSE_ON_FREE), bufferevent_free};
        if (!bev) { call.result.error = "https-tls-buffer-failed"; return false; }
        ssl.release();
        connection.reset(evhttp_connection_base_bufferevent_new(
            base.get(), dns.get(), bev.get(), parsed.host.c_str(), parsed.port));
        if (!connection) { call.result.error = "https-connection-creation-failed"; return false; }
        bev.release();
        evhttp_connection_set_retries(connection.get(), 0);
        evhttp_connection_set_max_headers_size(connection.get(), MAX_HEADERS);
        evhttp_connection_set_closecb(connection.get(), [](evhttp_connection*, void* argument) {
            static_cast<ClientSession*>(argument)->connection_closed = true;
        }, &session);
        created = Clock::now();
        return true;
    }
};

FlowMeshHttpsClient::FlowMeshHttpsClient(bool keep_alive) : m_impl{std::make_unique<Impl>(keep_alive)} {}
FlowMeshHttpsClient::~FlowMeshHttpsClient() = default;
void FlowMeshHttpsClient::Reset() { m_impl->Reset(); }

HttpsRequestResult FlowMeshHttpsClient::Request(const HttpsEndpoint& endpoint, const std::string& path,
                                               const std::string& body, Milliseconds timeout, size_t max_reply_bytes)
{
    FlowMeshTimingSpan timing{"https_request"};
    ClientCall call;
    call.timing = &timing;
    call.max_reply = max_reply_bytes;
    const auto start{Clock::now()};
    const auto parsed{ParseEndpoint(endpoint, path)};
    if (!parsed || timeout <= Milliseconds{0} || timeout > MAX_TIMEOUT ||
        max_reply_bytes == 0 || max_reply_bytes > MAX_REPLY || body.size() > MAX_REQUEST) {
        Reset();
        call.result.error = "https-invalid-endpoint-or-bounds";
        return call.result;
    }
    const auto expires{start + timeout};
    std::optional<std::array<unsigned char, 32>> pin;
    if (!endpoint.certificate_sha256.empty()) {
        pin = ParsePin(endpoint.certificate_sha256);
        if (!pin) { Reset(); call.result.error = "https-invalid-certificate-pin"; return call.result; }
    }
    HttpsEndpoint normalized{endpoint};
    if (!NormalizeFlowMeshHttpsEndpoint(normalized, call.result.error)) { Reset(); return call.result; }
    std::optional<std::array<unsigned char, 32>> fingerprint;
    if (!FingerprintCa(endpoint.ca_file, expires, fingerprint)) {
        Reset();
        call.result.error = Clock::now() >= expires ? "https-deadline" : "https-ca-load-failed";
        return call.result;
    }
    auto& state{*m_impl};
    if (state.endpoint.url != normalized.url || state.endpoint.ca_file != normalized.ca_file ||
        state.session.pin != pin || state.ca_fingerprint != fingerprint || !state.Reusable()) Reset();
    const bool warm{bool(state.connection)};
    if (!warm && !state.Initialize(normalized, *parsed, pin, fingerprint, expires, call)) {
        Reset(); return call.result;
    }
    call.base = state.base.get();
    evhttp_connection_set_max_body_size(state.connection.get(), max_reply_bytes);
    const auto limit{Timeval(timeout)};
    evhttp_connection_set_timeout_tv(state.connection.get(), &limit);
    auto* request{evhttp_request_new([](evhttp_request* req, void* argument) {
        auto& current{*static_cast<ClientCall*>(argument)};
        current.complete = true;
        if (current.timing) current.timing->Mark("response_callback_us");
        try {
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
                    current.response_reusable = ClientReplyReusable(req, size);
                } else current.result.error = "https-invalid-or-oversized-reply";
            } else if (current.result.error.empty()) current.result.error = "https-transport-failed";
        } catch (...) {
            // Never unwind through libevent or leave its completion cleanup
            // unfinished. Report the failure after callbacks have detached.
            current.callback_failed = true;
            current.result.response_received = false;
            current.result.body.clear();
            current.response_reusable = false;
        }
        event_base_loopbreak(current.base);
    }, &call)};
    if (!request) { Reset(); call.result.error = "https-request-allocation-failed"; return call.result; }
    evhttp_request_set_error_cb(request, [](evhttp_request_error error, void* argument) {
        auto& current{*static_cast<ClientCall*>(argument)};
        try {
            current.result.error = error == EVREQ_HTTP_DATA_TOO_LONG ? "https-reply-too-large"
                : error == EVREQ_HTTP_TIMEOUT ? "https-deadline" : "https-transport-failed";
        } catch (...) { current.callback_failed = true; }
    });
    auto* headers{evhttp_request_get_output_headers(request)};
    evhttp_add_header(headers, "Host", parsed->authority.c_str());
    evhttp_add_header(headers, "Content-Type", "application/json");
    evhttp_add_header(headers, "Connection", state.keep_alive ? "keep-alive" : "close");
    evhttp_add_header(headers, "Content-Length", std::to_string(body.size()).c_str());
    evbuffer_add(evhttp_request_get_output_buffer(request), body.data(), body.size());
    const auto remaining{timeout - std::chrono::duration_cast<Milliseconds>(Clock::now() - start)};
    if (remaining <= Milliseconds{0}) {
        evhttp_request_free(request);
        Reset();
        call.result.error = "https-deadline";
        return call.result;
    }
    Owned<event, event_free> deadline{evtimer_new(state.base.get(), [](evutil_socket_t, short, void* argument) {
        auto& current{*static_cast<ClientCall*>(argument)};
        try { current.result.error = "https-deadline"; }
        catch (...) { current.callback_failed = true; }
        event_base_loopbreak(current.base);
    }, &call), event_free};
    const auto remaining_tv{Timeval(remaining)};
    if (!deadline || evtimer_add(deadline.get(), &remaining_tv) != 0) {
        evhttp_request_free(request);
        deadline.reset();
        Reset();
        call.result.error = "https-deadline-setup-failed";
        return call.result;
    }
    // Dispatch is NOT an observation of socket write or peer receipt.
    timing.Mark("dispatch_started_us");
    timing.Field("connection_reused", uint64_t{warm});
    struct DispatchCleanup {
        Impl& state;
        Owned<event, event_free>& deadline;
        bool detached{false};
        ~DispatchCleanup()
        {
            if (!detached) {
                deadline.reset();
                state.Reset();
            }
        }
    } cleanup{state, deadline};
    state.session.active = &call;
    // Unlike a cold handshake, a warm socket can immediately write. Mark every
    // warm request before make_request makes any of its bytes writer-eligible.
    call.result.connection_reused = warm;
    if (warm) {
        call.result.request_may_have_been_sent = true;
        // The option persists for the socket's lifetime; a cold request is
        // reported by the handshake callback that configures its socket.
        SSL* ssl{bufferevent_openssl_get_ssl(evhttp_connection_get_bufferevent(state.connection.get()))};
        const int fd{ssl ? SSL_get_fd(ssl) : -1};
        call.result.tcp_nodelay = fd >= 0 && fd == state.session.nodelay_fd;
    }
    // libevent owns/frees request whether make_request succeeds or fails.
    if (evhttp_make_request(state.connection.get(), request, EVHTTP_REQ_POST, path.c_str()) != 0) {
        call.result.error = "https-request-setup-failed";
    } else if (event_base_dispatch(state.base.get()) < 0) {
        call.result.error = "https-event-loop-failed";
    }
    timing.Mark("dispatch_completed_us");
    deadline.reset();
    state.session.active = nullptr;
    cleanup.detached = true;
    timing.Field("tcp_nodelay", uint64_t{call.result.tcp_nodelay});
    if (call.callback_failed) {
        Reset();
        call.result.response_received = false;
        call.result.body.clear();
        call.result.error = "https-callback-failed";
        return call.result;
    }
    if (state.session.pin_failed) call.result.error = "https-certificate-pin-mismatch";
    else if (state.session.tls_failed) call.result.error = "https-certificate-verification-failed";
    if (!call.complete && call.result.error.empty()) call.result.error = "https-transport-failed";
    ++state.requests;
    state.last_used = Clock::now();
    // An application-level 4xx/5xx is a complete, exactly framed reply and keeps
    // the verified connection. Redirects and transport errors still reset, and
    // server-side transport rejections always close.
    if (!call.complete || !call.result.response_received || !call.result.error.empty() ||
        !call.response_reusable || !state.Reusable()) Reset();
    return call.result;
}

HttpsRequestResult FlowMeshHttpsRequest(const HttpsEndpoint& endpoint, const std::string& path,
                                      const std::string& body, Milliseconds timeout, size_t max_reply_bytes)
{
    return FlowMeshHttpsClient{false}.Request(endpoint, path, body, timeout, max_reply_bytes);
}

struct FlowMeshHttpsServer::Impl {
    struct Connection {
        std::atomic<size_t>& count;
        std::shared_ptr<Sock> socket;
        SocketBio socket_bio;
        Ssl ssl{nullptr, SSL_free};
        std::string peer;
        Clock::time_point deadline;
        const Clock::time_point expires;
        Clock::time_point idle_deadline;
        uint64_t enqueued_us{0};
        size_t requests{0};
        bool tcp_nodelay{false};

        Connection(std::atomic<size_t>& total, std::shared_ptr<Sock> sock, std::string address,
                   Clock::time_point accepted, const Options& options)
            : count{total}, socket{std::move(sock)}, socket_bio{*socket}, peer{std::move(address)},
              deadline{std::min(accepted + options.request_timeout, accepted + options.connection_lifetime)},
              expires{accepted + options.connection_lifetime}
        {
            ++count;
        }
        ~Connection()
        {
            // Count closing owners until both SSL and the actual socket have
            // been released. Readiness snapshots retain only idle sockets.
            ssl.reset();
            socket.reset();
            --count;
        }
    };
    Options options;
    Handler handler;
    SslContext context{nullptr, SSL_CTX_free};
    std::shared_ptr<Sock> listener;
    std::shared_ptr<Sock> wake_reader;
    std::unique_ptr<Sock> wake_writer;
    SOCKET listener_fd{INVALID_SOCKET};
    std::atomic<uint16_t> port{0};
    std::atomic<bool> stopping{true};
    std::atomic<size_t> connections{0};
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::unique_ptr<Connection>> queue;
    std::vector<std::unique_ptr<Connection>> idle;
    std::thread accept_thread;
    std::vector<std::thread> workers;
    // Empty in production. Test observation occurs only after immutable
    // cleanup has completed; it cannot alter the operation being measured.
    std::function<void(size_t, size_t)> rejected_cleanup_observer;
    // Empty in production. Reads back socket options only, after they are set.
    std::function<void(bool)> connection_observer;

    Impl(Options opts, Handler fn) : options{std::move(opts)}, handler{std::move(fn)} {}

    void Wake()
    {
        if (wake_writer) {
            const char wake{0};
            // A full nonblocking wake socket already has a notification queued.
            while (wake_writer->Send(&wake, 1, MSG_NOSIGNAL) < 0 && WSAGetLastError() == WSAEINTR) {}
        }
    }

    static uint64_t EnqueuedTime()
    {
        return FlowMeshTimingRecording() ? uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count()) : 0;
    }

    bool Serve(Connection& pending)
    {
        FlowMeshTimingSpan timing{"https_server_serve"};
        timing.Field("enqueued_us", pending.enqueued_us);
        timing.Field("connection_request_index", pending.requests + 1);
        if (stopping.load() || Clock::now() >= pending.deadline) return false;
        const bool first{!pending.ssl};
        if (first) {
            // Latency only, never a trust or admission condition: replies are
            // whole TLS records, and with Nagle a record's tail waits for the
            // peer's (delayed) ACK. Admitted API sockets only; ConfigureSocket
            // also configures the listener and the AF_UNIX wake sockets.
            const int one{1};
            pending.tcp_nodelay = pending.socket->SetSockOpt(IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0;
            if (connection_observer) {
                int value{0};
                socklen_t size{sizeof(value)};
                connection_observer(pending.socket->GetSockOpt(IPPROTO_TCP, TCP_NODELAY, &value, &size) == 0 && value != 0);
            }
            pending.ssl.reset(SSL_new(context.get()));
            const BIO_METHOD* method{SocketBioMethod()};
            if (!pending.ssl || !method) return false;
            BIO* bio{BIO_new(method)};
            if (!bio) return false;
            BIO_set_data(bio, &pending.socket_bio);
            SSL_set_bio(pending.ssl.get(), bio, bio);
        }
        timing.Field("tcp_nodelay", uint64_t{pending.tcp_nodelay});
        TlsIo io{pending.ssl.get(), *pending.socket, stopping, pending.deadline};
        if (first) {
            if (!io.Handshake()) return false;
            timing.Mark("handshake_done_us");
        }
        Request request;
        request.remote_address = pending.peer;
        Response response;
        int failure{400};
        bool keep_alive{false};
        const bool rejected{!ReadRequest(io, options.max_request_bytes, request, failure, keep_alive)};
        timing.Mark("request_read_us");
        if (rejected) {
            response = {failure, "{\"error\":\"invalid-http-request\"}"};
        } else if (!io.Alive()) return false;
        else {
            try { response = handler(request); }
            catch (...) { response = {500, "{\"error\":\"handler-failed\"}"}; }
        }
        timing.Mark("handler_completed_us");
        if (!io.Alive()) return false; // Handler may have admitted; caller must treat timeout as unknown.
        if (response.status < 200 || response.status > 599 || response.body.size() > options.max_reply_bytes) {
            response = {500, "{\"error\":\"reply-out-of-bounds\"}"};
        }
        ++pending.requests;
        bool subsequent_input{!rejected && io.PendingInput()};
        keep_alive = keep_alive && options.keep_alive && !rejected && !subsequent_input &&
            pending.requests < options.max_requests_per_connection && Clock::now() < pending.expires;
        std::string head{"HTTP/1.1 " + std::to_string(response.status) +
            " Response\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(response.body.size()) + "\r\nConnection: " + (keep_alive ? "keep-alive" : "close") +
            "\r\nCache-Control: no-store\r\n\r\n"};
        // Same HTTP bytes, fewer records: the status line, headers and first
        // body bytes share one SSL_write (one record), so a small reply is one
        // record and one segment. Larger bodies continue in record-sized writes
        // from the original buffer, never a reply-sized copy.
        const size_t inline_bytes{std::min(response.body.size(),
            MAX_TLS_RECORD_PLAINTEXT - std::min(head.size(), MAX_TLS_RECORD_PLAINTEXT))};
        head.append(response.body, 0, inline_bytes);
        const std::string_view rest{std::string_view{response.body}.substr(inline_bytes)};
        bool* const watch_input{keep_alive ? &subsequent_input : nullptr};
        if (io.Write(head, watch_input) && io.Write(rest, watch_input)) {
            timing.Mark("response_written_us");
            if (rejected) {
                const auto [ciphertext, plaintext]{io.FinishRejectedResponse()};
                if (rejected_cleanup_observer) rejected_cleanup_observer(ciphertext, plaintext);
            }
            else if (keep_alive && !subsequent_input && io.Alive()) return true;
            else SSL_shutdown(pending.ssl.get());
        }
        return false;
    }

    void Worker()
    {
        while (true) {
            std::unique_ptr<Connection> pending;
            {
                std::unique_lock lock{mutex};
                condition.wait(lock, [&] { return stopping.load() || !queue.empty(); });
                if (stopping.load()) return;
                pending = std::move(queue.front());
                queue.pop_front();
            }
            try {
                if (Serve(*pending)) {
                    std::lock_guard lock{mutex};
                    if (!stopping.load() && Clock::now() < pending->expires) {
                        pending->idle_deadline = std::min(pending->expires, Clock::now() + options.idle_timeout);
                        idle.push_back(std::move(pending));
                        Wake();
                    }
                }
            } catch (...) { /* Close, never expose an exception body. */ }
        }
    }

    void Accept()
    {
        while (!stopping.load()) {
            Sock::EventsPerSock ready;
            ready.emplace(listener, Sock::Events{Sock::RECV});
            ready.emplace(wake_reader, Sock::Events{Sock::RECV});
            auto wait_until{Clock::now() + std::chrono::seconds{1}};
            {
                std::lock_guard lock{mutex};
                for (const auto& connection : idle) {
                    ready.emplace(connection->socket, Sock::Events{Sock::RECV});
                    wait_until = std::min(wait_until, connection->idle_deadline);
                }
            }
            // The wakeup makes parking and Stop immediate. This timeout bounds
            // only error recovery and expiry, never the warm-request latency.
            const auto wait{std::max(Milliseconds{0},
                std::chrono::duration_cast<Milliseconds>(wait_until - Clock::now()))};
            if (!listener->WaitMany(wait, ready)) {
                if (WSAGetLastError() == WSAEINTR) continue;
                break;
            }
            if (stopping.load()) break;
            if (ready.at(wake_reader).occurred) {
                std::array<char, 256> discarded{};
                // One bounded read is enough; remaining wake bytes will make
                // the next wait ready, and every iteration scans all owners.
                (void)wake_reader->Recv(discarded.data(), discarded.size(), 0);
            }
            const bool accept_ready{(ready.at(listener).occurred & Sock::RECV) != 0};
            {
                std::lock_guard lock{mutex};
                const auto now{Clock::now()};
                for (auto it{idle.begin()}; it != idle.end();) {
                    auto& connection{*it};
                    const auto event{ready.find(connection->socket)};
                    const Sock::Event occurred{event == ready.end() ? Sock::Event{0} : event->second.occurred};
                    if (now >= connection->idle_deadline || (occurred & Sock::ERR)) {
                        // Drop the descriptor snapshot before destroying its
                        // counted connection, so closing still consumes a slot.
                        if (event != ready.end()) ready.erase(event);
                        it = idle.erase(it);
                    } else if (occurred & Sock::RECV) {
                        ready.erase(event);
                        if (queue.size() < options.max_queue) {
                            // Capture once at readiness, including queue wait.
                            // Further trickle bytes cannot renew this deadline.
                            connection->deadline = std::min(connection->expires, now + options.request_timeout);
                            connection->enqueued_us = EnqueuedTime();
                            queue.push_back(std::move(connection));
                            condition.notify_one();
                        }
                        it = idle.erase(it); // Queue overflow closes this owner.
                    } else ++it;
                }
            }
            if (!accept_ready) continue;
            sockaddr_storage address{};
            socklen_t size{sizeof(address)};
            const SOCKET fd{static_cast<SOCKET>(::accept(listener_fd, reinterpret_cast<sockaddr*>(&address), &size))};
            if (fd == INVALID_SOCKET) continue;
            const auto accepted{Clock::now()};
            auto socket{std::make_shared<Sock>(fd)};
            if (!ConfigureSocket(*socket)) continue;
            std::lock_guard lock{mutex};
            if (stopping.load() || queue.size() >= options.max_queue ||
                connections.load() >= options.max_connections) continue;
            auto pending{std::make_unique<Connection>(connections, std::move(socket), NumericPeer(address), accepted, options)};
            pending->enqueued_us = EnqueuedTime();
            queue.push_back(std::move(pending));
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

bool FlowMeshHttpsServer::SetConnectionObserverForTest(std::function<void(bool)> observer)
{
    // Same externally serialized, stopped-only rule as the cleanup observer.
    if (!m_impl->stopping.load() || !m_impl->workers.empty() || m_impl->accept_thread.joinable()) return false;
    m_impl->connection_observer = std::move(observer);
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
        options.request_timeout <= Milliseconds{0} || options.request_timeout > MAX_TIMEOUT ||
        options.idle_timeout <= Milliseconds{0} || options.idle_timeout > MAX_TIMEOUT ||
        options.connection_lifetime <= Milliseconds{0} || options.connection_lifetime > std::chrono::hours{1} ||
        options.max_requests_per_connection == 0 || options.max_requests_per_connection > 4096) {
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
    state.listener = std::make_shared<Sock>(fd);
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
    evutil_socket_t wake[2];
#ifdef WIN32
    constexpr int wake_family{AF_INET};
#else
    constexpr int wake_family{AF_UNIX};
#endif
    if (evutil_socketpair(wake_family, SOCK_STREAM, 0, wake) != 0) {
        Stop();
        error = "https-server-wakeup-failed";
        return false;
    }
    state.wake_reader = std::make_shared<Sock>(static_cast<SOCKET>(wake[0]));
    state.wake_writer = std::make_unique<Sock>(static_cast<SOCKET>(wake[1]));
    if (!ConfigureSocket(*state.wake_reader) || !ConfigureSocket(*state.wake_writer)) {
        Stop();
        error = "https-server-wakeup-failed";
        return false;
    }
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
    state.Wake();
    state.condition.notify_all();
    if (state.accept_thread.joinable()) state.accept_thread.join();
    {
        std::lock_guard lock{state.mutex};
        state.queue.clear();
        state.idle.clear();
    }
    // Idle/queued peers close before joining admitted handlers, whose existing
    // completion contract is unchanged by persistence.
    for (auto& worker : state.workers) if (worker.joinable()) worker.join();
    state.workers.clear();
    state.wake_reader.reset();
    state.wake_writer.reset();
    state.listener.reset();
    state.listener_fd = INVALID_SOCKET;
    state.context.reset();
    state.port = 0;
}

uint16_t FlowMeshHttpsServer::Port() const { return m_impl->port.load(); }

} // namespace node
