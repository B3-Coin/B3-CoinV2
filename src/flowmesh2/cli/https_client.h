// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CLI_HTTPS_CLIENT_H
#define B3_FLOWMESH2_CLI_HTTPS_CLIENT_H

#include <flowmesh2/core/result.h>

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

/** A blocking TLS 1.3 client for one seat's HTTPS + SSE API (M5-CLI-01,
 * ARCHITECTURE.md O-106; net/https_sse_api.h for the server). One connection
 * per request (Connection: close), as the API expects. The certificate is
 * checked by its SPKI SHA-256 fingerprint, pinned per endpoint (the daemon
 * writes it to `api.fingerprint` and `fm2d.ready`; O-104 (2)): no CA, no
 * hostname. A handshake whose peer key is not the pinned one is dropped
 * before a byte of the request leaves (VERIFY_FAILED HTTPS_PIN).
 *
 * Every step is bounded by `timeout_ms`: the TCP connect (non-blocking,
 * polled against the deadline), the handshake and each read (SO_RCVTIMEO
 * clipped to what is left of the deadline), and a whole Request (a wall-clock
 * deadline, so a trickling peer cannot hold the client). A reply is complete
 * only when its Content-Length or its chunk terminator says so; a connection
 * that drops before that is BUSY HTTPS_IO, never a truncated 200. Reply
 * bodies are capped (MAX_REPLY_BYTES) and chunked bodies are decoded
 * incrementally with the chunk size parsed as bounded hex (a malformed or
 * oversize chunk is DECODE HTTPS_RESPONSE). The host must be an IP literal
 * (the caller checks): name resolution has no deadline.
 */
namespace flowmesh2::cli {

inline constexpr std::string_view HTTPS_CONNECT{"could not connect to the seat's API"};          //!< (BUSY)
inline constexpr std::string_view HTTPS_TIMEOUT{"the seat did not answer within the timeout"};  //!< (BUSY)
inline constexpr std::string_view HTTPS_PIN{"the seat's certificate is not the pinned one"};     //!< (VERIFY_FAILED)
inline constexpr std::string_view HTTPS_PIN_SHAPE{"a certificate pin is not 64 hex characters"}; //!< (INVALID_INPUT)
inline constexpr std::string_view HTTPS_IO{"the connection ended before the reply did"};        //!< (BUSY)
inline constexpr std::string_view HTTPS_RESPONSE{"the reply is not HTTP, or its chunks are malformed"}; //!< (DECODE)
inline constexpr std::string_view HTTPS_OVERSIZE{"the reply is larger than the client accepts"}; //!< (OVERSIZE)

//! Longest reply (headers and body) a Request accepts, and the longest undecoded tail of a stream. PROVISIONAL.
inline constexpr size_t MAX_REPLY_BYTES{4 * 1024 * 1024};
//! Longest chunk of a chunked body. PROVISIONAL.
inline constexpr size_t MAX_CHUNK_BYTES{1024 * 1024};

struct HttpResponse {
    int status{0};
    std::string headers;
    std::string body;
};

struct SseEvent {
    std::string id;
    std::string event;
    std::string data;
};

/** An incremental decoder of a chunked transfer encoding: Feed() bytes as
 * they arrive, read the decoded body from Decoded() and Take() what you
 * consumed. Done() once the terminal chunk arrived; Malformed() once a size
 * line was not hex, exceeded MAX_CHUNK_BYTES or the trailer was wrong.
 */
class ChunkDecoder
{
public:
    void Feed(std::string_view bytes);
    const std::string& Decoded() const { return m_decoded; }
    //! Drop the first `n` decoded bytes (consumed by the caller).
    void Take(size_t n);
    bool Done() const { return m_done; }
    bool Malformed() const { return m_malformed; }
    size_t Pending() const { return m_pending.size(); }

private:
    std::string m_pending; //!< undecoded bytes
    std::string m_decoded;
    size_t m_remaining{0}; //!< bytes of the current chunk still to come (0: at a size line)
    bool m_in_chunk{false};
    bool m_done{false};
    bool m_malformed{false};
};

class HttpsClient
{
public:
    HttpsClient(std::string host, uint16_t port, std::string pin_spki_hex, uint32_t timeout_ms = 5'000);
    ~HttpsClient();
    HttpsClient(const HttpsClient&) = delete;
    HttpsClient& operator=(const HttpsClient&) = delete;

    //! One request on a fresh pinned connection; the whole reply, within the timeout.
    Result<HttpResponse> Request(std::string_view method, std::string_view path, std::span<const unsigned char> body = {},
                                 std::string_view content_type = "application/octet-stream");
    /** GET an event stream; `on_event` returns false to stop. Stops after
     * `limit_ms`, when the server closes, or when on_event says so: the
     * result's status is then 200 with an empty body. A non-200 answer ends
     * the stream at once and is returned with its body (the server's error).
     */
    Result<HttpResponse> Stream(std::string_view path, const std::function<bool(const SseEvent&)>& on_event, uint32_t limit_ms);

    //! The SPKI SHA-256 fingerprint of a certificate, lower-case hex.
    static std::string SpkiFingerprint(X509* cert);
    //! `text` is 64 hex characters (a fingerprint).
    static bool PinWellFormed(std::string_view text);
    //! The status line and headers of `raw` (up to the blank line), if complete: the status (0 unless a three-digit 100..599) and the headers text.
    static std::optional<std::pair<int, std::string>> ParseHead(std::string_view raw, size_t& body_start);
    //! The Content-Length header of `headers`, if present and a number.
    static std::optional<size_t> ContentLength(std::string_view headers);

private:
    Result<void> Connect(std::chrono::steady_clock::time_point deadline, uint32_t receive_timeout_ms);
    void Disconnect();
    Result<void> Write(std::string_view text);

    std::string m_host;
    uint16_t m_port;
    std::string m_pin;
    uint32_t m_timeout_ms;
    int m_fd{-1}; //!< the connected socket, for the per-read receive timeout
    SSL_CTX* m_ctx{nullptr};
    BIO* m_bio{nullptr};
    SSL* m_ssl{nullptr};
};

} // namespace flowmesh2::cli

#endif // B3_FLOWMESH2_CLI_HTTPS_CLIENT_H
