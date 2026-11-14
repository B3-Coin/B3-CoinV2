// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/cli/https_client.h>

#include <flowmesh2/netio/api_json.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace flowmesh2::cli {

namespace {

using Clock = std::chrono::steady_clock;

std::string Lower(std::string_view text)
{
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

int64_t RemainingMs(Clock::time_point deadline)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
}

void SetSocketTimeouts(int fd, uint32_t ms)
{
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(ms / 1000);
    tv.tv_usec = static_cast<suseconds_t>((ms % 1000) * 1000);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

} // namespace

// ---- ChunkDecoder ----------------------------------------------------------------------

void ChunkDecoder::Feed(std::string_view bytes)
{
    if (m_done || m_malformed) return;
    m_pending.append(bytes);
    for (;;) {
        if (m_in_chunk) {
            // The chunk's bytes, then its CRLF.
            if (m_remaining > 0) {
                const size_t take{std::min(m_remaining, m_pending.size())};
                m_decoded.append(m_pending, 0, take);
                m_pending.erase(0, take);
                m_remaining -= take;
                if (m_remaining > 0) return;
            }
            if (m_pending.size() < 2) return;
            if (m_pending[0] != '\r' || m_pending[1] != '\n') {
                m_malformed = true;
                return;
            }
            m_pending.erase(0, 2);
            m_in_chunk = false;
            continue;
        }
        const size_t line_end{m_pending.find("\r\n")};
        if (line_end == std::string::npos) {
            if (m_pending.size() > 32) m_malformed = true; // a size line is a few hex digits
            return;
        }
        std::string_view line{std::string_view{m_pending}.substr(0, line_end)};
        if (const size_t ext{line.find(';')}; ext != std::string_view::npos) line = line.substr(0, ext); // chunk extensions ignored
        size_t size{0};
        const auto [end, error]{std::from_chars(line.data(), line.data() + line.size(), size, 16)};
        if (line.empty() || error != std::errc{} || end != line.data() + line.size() || size > MAX_CHUNK_BYTES) {
            m_malformed = true;
            return;
        }
        m_pending.erase(0, line_end + 2);
        if (size == 0) {
            m_done = true; // the trailer, if any, is ignored
            m_pending.clear();
            return;
        }
        m_remaining = size;
        m_in_chunk = true;
    }
}

void ChunkDecoder::Take(size_t n) { m_decoded.erase(0, std::min(n, m_decoded.size())); }

// ---- HttpsClient -----------------------------------------------------------------------

HttpsClient::HttpsClient(std::string host, uint16_t port, std::string pin_spki_hex, uint32_t timeout_ms)
    : m_host{std::move(host)}, m_port{port}, m_pin{Lower(pin_spki_hex)}, m_timeout_ms{timeout_ms == 0 ? 1 : timeout_ms}
{
}

HttpsClient::~HttpsClient() { Disconnect(); }

bool HttpsClient::PinWellFormed(std::string_view text)
{
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string HttpsClient::SpkiFingerprint(X509* cert)
{
    if (cert == nullptr) return std::string{};
    EVP_PKEY* key{X509_get0_pubkey(cert)};
    if (key == nullptr) return std::string{};
    unsigned char* der{nullptr};
    const int len{i2d_PUBKEY(key, &der)};
    if (len <= 0 || der == nullptr) return std::string{};
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(der, static_cast<size_t>(len), digest);
    OPENSSL_free(der);
    return netio::ToHex(std::span<const unsigned char>{digest, SHA256_DIGEST_LENGTH});
}

std::optional<std::pair<int, std::string>> HttpsClient::ParseHead(std::string_view raw, size_t& body_start)
{
    const size_t end{raw.find("\r\n\r\n")};
    if (end == std::string_view::npos) return std::nullopt;
    body_start = end + 4;
    const std::string_view head{raw.substr(0, end)};
    // "HTTP/1.1 200 OK": exactly three digits, 100..599, then a space or the end of the line.
    int status{0};
    if (head.size() >= 12 && head.starts_with("HTTP/1.") && head[8] == ' ' && (head.size() == 12 || head[12] == ' ' || head[12] == '\r')) {
        const std::string_view digits{head.substr(9, 3)};
        if (std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            const auto [p, error]{std::from_chars(digits.data(), digits.data() + 3, status)};
            if (error != std::errc{} || status < 100 || status > 599) status = 0;
        }
    }
    return std::pair<int, std::string>{status, std::string{head}};
}

std::optional<size_t> HttpsClient::ContentLength(std::string_view headers)
{
    const std::string lower{Lower(headers)};
    const size_t at{lower.find("\ncontent-length:")};
    if (at == std::string::npos) return std::nullopt;
    size_t pos{at + 16};
    while (pos < lower.size() && lower[pos] == ' ') ++pos;
    size_t value{0};
    const auto [end, error]{std::from_chars(lower.data() + pos, lower.data() + lower.size(), value)};
    if (error != std::errc{} || end == lower.data() + pos) return std::nullopt;
    return value;
}

Result<void> HttpsClient::Connect(Clock::time_point deadline, uint32_t receive_timeout_ms)
{
    Disconnect();
    if (!PinWellFormed(m_pin)) return Error{ErrorCode::INVALID_INPUT, HTTPS_PIN_SHAPE};
    m_ctx = SSL_CTX_new(TLS_client_method());
    if (m_ctx == nullptr) return Error{ErrorCode::BUSY, HTTPS_CONNECT};
    if (SSL_CTX_set_min_proto_version(m_ctx, TLS1_3_VERSION) != 1 || SSL_CTX_set_max_proto_version(m_ctx, TLS1_3_VERSION) != 1) {
        Disconnect();
        return Error{ErrorCode::BUSY, HTTPS_CONNECT};
    }
    SSL_CTX_set_verify(m_ctx, SSL_VERIFY_NONE, nullptr); // the pin below replaces the CA check (O-104 (2))
    // The TCP connect, non-blocking against the deadline.
    const std::string target{m_host + ":" + std::to_string(m_port)};
    BIO* tcp{BIO_new_connect(target.c_str())};
    if (tcp == nullptr) {
        Disconnect();
        return Error{ErrorCode::BUSY, HTTPS_CONNECT};
    }
    BIO_set_nbio(tcp, 1);
    // BIO_do_connect is called again only after poll() says the socket moved: called while the connect is still pending it
    // spins inside OpenSSL until the kernel's SYN timeout (bss_conn.c, BLOCKED_CONNECT), well past any deadline of ours.
    bool ask{true};
    for (;;) {
        if (ask) {
            if (BIO_do_connect(tcp) > 0) break;
            if (!BIO_should_retry(tcp)) {
                BIO_free_all(tcp);
                Disconnect();
                return Error{ErrorCode::BUSY, HTTPS_CONNECT};
            }
        }
        const int64_t left{RemainingMs(deadline)};
        if (left <= 0) {
            BIO_free_all(tcp);
            Disconnect();
            return Error{ErrorCode::BUSY, HTTPS_TIMEOUT};
        }
        int fd{-1};
        if (BIO_get_fd(tcp, &fd) < 0 || fd < 0) {
            BIO_free_all(tcp);
            Disconnect();
            return Error{ErrorCode::BUSY, HTTPS_CONNECT};
        }
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT | POLLIN;
        const int moved{::poll(&pfd, 1, static_cast<int>(std::min<int64_t>(left, 1000)))};
        if (moved < 0 && errno != EINTR) {
            BIO_free_all(tcp);
            Disconnect();
            return Error{ErrorCode::BUSY, HTTPS_CONNECT};
        }
        ask = moved > 0;
    }
    // Connected: back to a blocking socket (BIO_set_nbio only steers the connect) with receive and send timeouts clipped to the
    // deadline for the handshake.
    BIO_set_nbio(tcp, 0);
    m_fd = -1;
    if (BIO_get_fd(tcp, &m_fd) < 0 || m_fd < 0) {
        BIO_free_all(tcp);
        Disconnect();
        return Error{ErrorCode::BUSY, HTTPS_CONNECT};
    }
    const int flags{::fcntl(m_fd, F_GETFL, 0)};
    if (flags >= 0) (void)::fcntl(m_fd, F_SETFL, flags & ~O_NONBLOCK);
    SetSocketTimeouts(m_fd, static_cast<uint32_t>(std::clamp<int64_t>(RemainingMs(deadline), 1, m_timeout_ms)));
    BIO* ssl_bio{BIO_new_ssl(m_ctx, /*client=*/1)};
    if (ssl_bio == nullptr) {
        BIO_free_all(tcp);
        Disconnect();
        return Error{ErrorCode::BUSY, HTTPS_CONNECT};
    }
    m_bio = BIO_push(ssl_bio, tcp);
    BIO_get_ssl(m_bio, &m_ssl);
    if (m_ssl == nullptr) {
        Disconnect();
        return Error{ErrorCode::BUSY, HTTPS_CONNECT};
    }
    for (;;) {
        if (BIO_do_handshake(m_bio) > 0) break;
        if (!BIO_should_retry(m_bio)) {
            Disconnect();
            return Error{ErrorCode::BUSY, HTTPS_CONNECT};
        }
        const int64_t left{RemainingMs(deadline)}; // a receive timeout fired inside the handshake: look at the clock
        if (left <= 0) {
            Disconnect();
            return Error{ErrorCode::BUSY, HTTPS_TIMEOUT};
        }
        SetSocketTimeouts(m_fd, static_cast<uint32_t>(std::clamp<int64_t>(left, 1, m_timeout_ms)));
    }
    SetSocketTimeouts(m_fd, receive_timeout_ms);
    X509* cert{SSL_get1_peer_certificate(m_ssl)};
    const std::string seen{SpkiFingerprint(cert)};
    if (cert != nullptr) X509_free(cert);
    if (seen.size() != m_pin.size() || CRYPTO_memcmp(seen.data(), m_pin.data(), m_pin.size()) != 0) {
        Disconnect();
        return Error{ErrorCode::VERIFY_FAILED, HTTPS_PIN};
    }
    return Ok();
}

void HttpsClient::Disconnect()
{
    if (m_bio != nullptr) BIO_free_all(m_bio);
    if (m_ctx != nullptr) SSL_CTX_free(m_ctx);
    m_bio = nullptr;
    m_ctx = nullptr;
    m_ssl = nullptr;
    m_fd = -1;
}

Result<void> HttpsClient::Write(std::string_view text)
{
    size_t done{0};
    while (done < text.size()) {
        const int n{BIO_write(m_bio, text.data() + done, static_cast<int>(text.size() - done))};
        if (n <= 0) return Error{ErrorCode::BUSY, HTTPS_IO};
        done += static_cast<size_t>(n);
    }
    return Ok();
}

Result<HttpResponse> HttpsClient::Request(std::string_view method, std::string_view path, std::span<const unsigned char> body,
                                          std::string_view content_type)
{
    const Clock::time_point deadline{Clock::now() + std::chrono::milliseconds{m_timeout_ms}};
    if (const Result<void> connected{Connect(deadline, m_timeout_ms)}; !connected.has_value()) return connected.error();
    std::string request{std::string{method} + " " + std::string{path} + " HTTP/1.1\r\nHost: " + m_host + "\r\nConnection: close\r\n"};
    if (!body.empty() || method == "POST") {
        request += "Content-Type: " + std::string{content_type} + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n";
    request.append(reinterpret_cast<const char*>(body.data()), body.size());
    if (const Result<void> written{Write(request)}; !written.has_value()) {
        Disconnect();
        return written.error();
    }
    std::string raw;
    bool closed{false};
    char buf[4096];
    for (;;) {
        const int64_t left{RemainingMs(deadline)};
        if (left <= 0) {
            Disconnect();
            return Error{ErrorCode::BUSY, HTTPS_TIMEOUT};
        }
        SetSocketTimeouts(m_fd, static_cast<uint32_t>(std::clamp<int64_t>(left, 1, m_timeout_ms)));
        const int n{BIO_read(m_bio, buf, sizeof(buf))};
        if (n > 0) {
            raw.append(buf, static_cast<size_t>(n));
            if (raw.size() > MAX_REPLY_BYTES) {
                Disconnect();
                return Error{ErrorCode::OVERSIZE, HTTPS_OVERSIZE};
            }
            continue;
        }
        if (n < 0 && BIO_should_retry(m_bio)) {
            // The receive timeout fired without data: the seat is too slow, not finished.
            Disconnect();
            return Error{ErrorCode::BUSY, HTTPS_TIMEOUT};
        }
        closed = n == 0; // a clean close (Connection: close); anything else dropped the connection
        break;
    }
    Disconnect();
    size_t body_start{0};
    const std::optional<std::pair<int, std::string>> head{ParseHead(raw, body_start)};
    if (!head.has_value()) return Error{raw.empty() || !closed ? ErrorCode::BUSY : ErrorCode::DECODE, raw.empty() || !closed ? HTTPS_IO : HTTPS_RESPONSE};
    HttpResponse response;
    response.status = head->first;
    response.headers = head->second;
    if (response.status == 0) return Error{ErrorCode::DECODE, HTTPS_RESPONSE};
    const std::string_view rest{std::string_view{raw}.substr(body_start)};
    // The reply is complete only when its framing says so: a dropped connection is a transport fault, not an answer.
    if (Lower(response.headers).find("\ntransfer-encoding: chunked") != std::string::npos) {
        ChunkDecoder chunks;
        chunks.Feed(rest);
        if (chunks.Malformed()) return Error{ErrorCode::DECODE, HTTPS_RESPONSE};
        if (!chunks.Done()) return Error{ErrorCode::BUSY, HTTPS_IO};
        response.body = chunks.Decoded();
    } else {
        const std::optional<size_t> length{ContentLength(response.headers)};
        if (length.has_value() ? rest.size() != *length : !closed) return Error{ErrorCode::BUSY, HTTPS_IO};
        response.body = std::string{rest};
    }
    return Ok(std::move(response));
}

Result<HttpResponse> HttpsClient::Stream(std::string_view path, const std::function<bool(const SseEvent&)>& on_event, uint32_t limit_ms)
{
    const Clock::time_point deadline{Clock::now() + std::chrono::milliseconds{limit_ms}};
    // The connect and handshake get the whole timeout; the stream's reads then wake twice a second to look at the clock.
    if (const Result<void> connected{Connect(Clock::now() + std::chrono::milliseconds{m_timeout_ms}, std::min<uint32_t>(m_timeout_ms, 500))};
        !connected.has_value()) {
        return connected.error();
    }
    const std::string request{"GET " + std::string{path} + " HTTP/1.1\r\nHost: " + m_host +
                              "\r\nAccept: text/event-stream\r\nConnection: close\r\n\r\n"};
    if (const Result<void> written{Write(request)}; !written.has_value()) {
        Disconnect();
        return written.error();
    }
    std::string raw; // the head, then (for a non-200 reply) the body
    HttpResponse response;
    bool head_done{false};
    bool chunked{false};
    ChunkDecoder chunks;
    std::string plain; // the body when not chunked
    char buf[4096];
    while (Clock::now() < deadline) {
        const int n{BIO_read(m_bio, buf, sizeof(buf))};
        if (n < 0 && BIO_should_retry(m_bio)) continue; // the receive timeout: look at the clock again
        if (n <= 0) break;
        std::string_view bytes{buf, static_cast<size_t>(n)};
        if (!head_done) {
            raw.append(bytes);
            if (raw.size() > MAX_REPLY_BYTES) {
                Disconnect();
                return Error{ErrorCode::OVERSIZE, HTTPS_OVERSIZE};
            }
            size_t body_start{0};
            const std::optional<std::pair<int, std::string>> head{ParseHead(raw, body_start)};
            if (!head.has_value()) continue;
            head_done = true;
            response.status = head->first;
            response.headers = head->second;
            if (response.status == 0) {
                Disconnect();
                return Error{ErrorCode::DECODE, HTTPS_RESPONSE};
            }
            chunked = Lower(response.headers).find("\ntransfer-encoding: chunked") != std::string::npos;
            bytes = std::string_view{raw}.substr(body_start);
        }
        if (response.status != 200) {
            // The server's answer is an error: read it to its end (Content-Length or the chunk terminator) and hand it back.
            if (chunked) {
                chunks.Feed(bytes);
                if (chunks.Malformed()) {
                    Disconnect();
                    return Error{ErrorCode::DECODE, HTTPS_RESPONSE};
                }
                if (chunks.Done()) break;
            } else {
                plain.append(bytes);
                const std::optional<size_t> length{ContentLength(response.headers)};
                if (length.has_value() && plain.size() >= *length) break;
            }
            if (plain.size() + chunks.Decoded().size() > MAX_REPLY_BYTES) {
                Disconnect();
                return Error{ErrorCode::OVERSIZE, HTTPS_OVERSIZE};
            }
            continue;
        }
        if (!chunked) {
            Disconnect();
            return Error{ErrorCode::DECODE, HTTPS_RESPONSE}; // an event stream is chunked (net/https_sse_api)
        }
        chunks.Feed(bytes);
        if (chunks.Malformed()) {
            Disconnect();
            return Error{ErrorCode::DECODE, HTTPS_RESPONSE};
        }
        if (chunks.Decoded().size() > MAX_REPLY_BYTES) {
            // Decoded bytes waiting for an event's blank line: a seat may not grow the client without bound.
            Disconnect();
            return Error{ErrorCode::OVERSIZE, HTTPS_OVERSIZE};
        }
        // Events end in a blank line.
        for (;;) {
            const std::string& decoded{chunks.Decoded()};
            const size_t end{decoded.find("\n\n")};
            if (end == std::string::npos) break;
            SseEvent event;
            size_t pos{0};
            while (pos < end) {
                size_t line_end{decoded.find('\n', pos)};
                if (line_end == std::string::npos || line_end > end) line_end = end;
                const std::string_view line{std::string_view{decoded}.substr(pos, line_end - pos)};
                if (line.starts_with("id: ")) event.id = std::string{line.substr(4)};
                if (line.starts_with("event: ")) event.event = std::string{line.substr(7)};
                if (line.starts_with("data: ")) event.data = std::string{line.substr(6)};
                pos = line_end + 1;
            }
            chunks.Take(end + 2);
            if (!on_event(event)) {
                Disconnect();
                return Ok(HttpResponse{200, response.headers, std::string{}});
            }
        }
        if (chunks.Done()) break;
    }
    const bool timed_out{Clock::now() >= deadline};
    Disconnect();
    if (!head_done) {
        if (raw.empty()) return Error{ErrorCode::BUSY, timed_out ? HTTPS_TIMEOUT : HTTPS_IO};
        return Error{ErrorCode::DECODE, HTTPS_RESPONSE};
    }
    if (response.status != 200) {
        response.body = chunked ? chunks.Decoded() : plain;
        return Ok(std::move(response));
    }
    return Ok(HttpResponse{200, response.headers, std::string{}});
}

} // namespace flowmesh2::cli
