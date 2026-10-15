// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_NODE_FLOWMESH_HTTPS_H
#define BITCOIN_NODE_FLOWMESH_HTTPS_H

#include <util/fs.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace node {

/** FlowMeshHttpsServer::Options defaults. The client keeps connection reuse
 * strictly inside them, so it never sends into a connection that the server
 * is closing for idleness or age. */
inline constexpr std::chrono::seconds FLOWMESH_HTTPS_DEFAULT_REQUEST_TIMEOUT{10};
inline constexpr std::chrono::seconds FLOWMESH_HTTPS_DEFAULT_IDLE_TIMEOUT{30};
inline constexpr std::chrono::minutes FLOWMESH_HTTPS_DEFAULT_CONNECTION_LIFETIME{5};
//! FlowMeshHttpsServer::Options::worker_threads bound.
inline constexpr size_t FLOWMESH_HTTPS_MAX_WORKER_THREADS{16};
//! Only a configured restricted health handler adds this separate worker.
inline constexpr size_t FLOWMESH_HTTPS_MAX_RESERVED_HEALTH_THREADS{1};
inline constexpr size_t FLOWMESH_HTTPS_MAX_TOTAL_WORKER_THREADS{
    FLOWMESH_HTTPS_MAX_WORKER_THREADS + FLOWMESH_HTTPS_MAX_RESERVED_HEALTH_THREADS};
//! A reserve refusal is before action admission, not proof about earlier requests.
inline constexpr std::string_view FLOWMESH_HTTPS_RESERVED_BUSY_BODY{
    R"({"ok":false,"error":"Public trading handler capacity busy before admission"})"};

struct HttpsEndpoint {
    /** An HTTPS origin, optionally ending in /flowmesh/v1. No userinfo,
     * query, fragment, redirect, or plaintext fallback is supported. */
    std::string url;
    /** Empty selects the TLS library's default CA trust paths. */
    fs::path ca_file;
    /** Optional additional SHA256 pin of the DER leaf certificate, 64 hex
     * characters. A pin NEVER bypasses CA or hostname/IP verification. */
    std::string certificate_sha256;
};

struct HttpsRequestResult {
    bool response_received{false};
    int status{0};
    std::string body;
    std::string error;
    /** Conservative transport observation, not admission. Once a request
     * could have left this process, failures must be treated as unknown. */
    bool request_may_have_been_sent{false};
    /** Transport diagnostics only; none of these fields establishes admission. */
    bool connection_reused{false};
    bool tls_handshake_performed{false};
    /** TCP_NODELAY was set on the socket this request used (latency only). */
    bool tcp_nodelay{false};
    /** The configured CA material validated for this response's TLS session.
     * Invalid on early/transport/TLS failure; null digest with valid=true
     * means default CA paths, whose reload requires explicit Reset. This is
     * transport context, never an admission or consensus proof. */
    struct TrustStamp {
        bool valid{false};
        HttpsEndpoint endpoint;
        std::optional<std::array<unsigned char, 32>> ca_fingerprint;
    } trust;
};

/** Syntax and pin validation only: no DNS, filesystem or network access. */
bool ValidateFlowMeshHttpsEndpoint(const HttpsEndpoint& endpoint, std::string& error);
/** Validate and canonicalize an HTTPS origin for duplicate detection. Does not
 * change CA material or certificate pins, or perform network access. */
bool NormalizeFlowMeshHttpsEndpoint(HttpsEndpoint& endpoint, std::string& error);
/** Load configured CA material to catch missing/malformed trust files at
 * startup. Does not contact an endpoint or read a TLS private key. */
bool ValidateFlowMeshHttpsTrust(const HttpsEndpoint& endpoint, std::string& error);
/** Compare an owned response's validated trust context with its current
 * configured public CA material. Read-only, bounded buffering, no network;
 * call outside client/chain/wallet locks. Like Request's existing pre/post
 * CA-load checks this observes file contents, not arbitrary filesystem ABA
 * or atomic exclusion of administrator changes. Default-path reloads must
 * be fenced by the caller's explicit-reset generation. */
bool FlowMeshHttpsTrustCurrent(const HttpsEndpoint& endpoint, const HttpsRequestResult::TrustStamp& stamp,
                              std::chrono::milliseconds timeout, std::string& error);

/** One externally serialized HTTPS session, retaining at most one connection.
 * Requests are sequential, bounded, and never automatically replayed. Endpoint,
 * pin or explicit CA-file content changes invalidate the connection. Call Reset
 * on default trust-store reload; age, idle and request-count limits also bound
 * reuse. Request/Reset/destruction must not run concurrently. */
class FlowMeshHttpsClient {
public:
    explicit FlowMeshHttpsClient(bool keep_alive = true);
    ~FlowMeshHttpsClient();
    FlowMeshHttpsClient(const FlowMeshHttpsClient&) = delete;
    FlowMeshHttpsClient& operator=(const FlowMeshHttpsClient&) = delete;
    HttpsRequestResult Request(const HttpsEndpoint& endpoint,
                               const std::string& path,
                               const std::string& body,
                               std::chrono::milliseconds timeout,
                               size_t max_reply_bytes);
    void Reset();
    /** True when a Request to `endpoint` would reuse the retained connection
     * now: the same normalized origin, CA file and pin, a completed and still
     * valid TLS handshake, inside the age, idle and request-count limits, and
     * no pending EOF or unread bytes. A transport choice only, never trust:
     * Request still re-checks origin, pin and CA-file content before a byte is
     * written. No DNS, connect, file read or write; same external
     * serialization as Request. */
    bool Warm(const HttpsEndpoint& endpoint);
    /** Exchange the retained connections, with their trust state, of two
     * sessions. The caller exclusively owns both, and neither is inside
     * Request. Returns false and changes nothing when their keep-alive
     * policies differ. */
    bool Swap(FlowMeshHttpsClient& other) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/** One bounded POST. Includes DNS/connect/TLS/read in a single deadline.
 * Call only on a worker, never under wallet/chain/runtime locks or on the GUI
 * thread. The caller owns semantic retry and must reuse the exact signed body. */
HttpsRequestResult FlowMeshHttpsRequest(const HttpsEndpoint& endpoint,
                                      const std::string& path,
                                      const std::string& body,
                                      std::chrono::milliseconds timeout,
                                      size_t max_reply_bytes);

/** Restricted TLS listener, independent of administrator/wallet RPC and FMN2.
 * Only POST /flowmesh/v1 is routed to Handler. Handler must be bounded,
 * nonblocking with respect to operator execution, and must implement its own
 * JSON method allowlist and authenticated action admission. It may block only
 * in a bounded wait that ends before Request::deadline, with its permit
 * suspended. TLS authenticates the server; existing signed actions
 * authenticate trading authority.
 * Start/Stop are externally serialized; Stop joins in-progress handlers.
 * No transport code logs request bodies, TLS keys, or credentials. */
class FlowMeshHttpsServer {
public:
    struct Options {
        std::string bind_host{"127.0.0.1"};
        uint16_t port{0};
        fs::path cert_file;
        fs::path key_file;
        size_t max_request_bytes{64 * 1024};
        size_t max_reply_bytes{4 * 1024 * 1024};
        size_t max_connections{64};
        size_t worker_threads{2};
        size_t max_queue{32};
        std::chrono::milliseconds request_timeout{FLOWMESH_HTTPS_DEFAULT_REQUEST_TIMEOUT};
        /** Sequential HTTP/1.1 reuse only. Idle connections remain within
         * max_connections, and do not occupy handler workers. */
        bool keep_alive{true};
        std::chrono::milliseconds idle_timeout{FLOWMESH_HTTPS_DEFAULT_IDLE_TIMEOUT};
        std::chrono::milliseconds connection_lifetime{FLOWMESH_HTTPS_DEFAULT_CONNECTION_LIFETIME};
        size_t max_requests_per_connection{128};
        /** Ordinary connections at once in TLS handshake, request read, handler work,
         * reply write or close; 0 means worker_threads, at most that. Extra
         * workers only serve while a handler has suspended its permit, so
         * handshake CPU, buffered replies and handler concurrency stay within
         * this bound however many handlers sleep in bounded waits. A configured
         * restricted health handler has one separate transport permit/worker;
         * it never increases ordinary handler or wait/watch capacity. */
        size_t active_permits{0};
        /** Optional; must not block and should not throw (exceptions are
         * swallowed). on_start runs in Start just before the workers spawn.
         * on_stop runs exactly once after each on_start, in Stop after new
         * work is refused and before any handler is joined, so a handler in
         * a bounded wait can return promptly. */
        std::function<void()> on_start;
        std::function<void()> on_stop;
    };
    struct Request {
        std::string path;
        std::string body;
        std::string remote_address;
        /** Absolute deadline of this request, fixed when it became readable
         * (including queue wait). A reply finished after it is never written,
         * so a handler that waits must leave time to build and send one. */
        std::chrono::steady_clock::time_point deadline{};
        /** Set by the server for the handler call only (empty otherwise).
         * suspend() releases this request's active permit: call it only right
         * before sleeping in a bounded wait that holds no lock another request
         * needs. resume() waits for a permit, ahead of queued connections, and
         * must precede any further work; the server resumes a handler that
         * returns suspended. Both are idempotent. */
        std::function<void()> suspend;
        std::function<void()> resume;
    };
    struct Response {
        int status{200};
        std::string body;
        //! Explicit pre-admission busy replies on the reserve never persist.
        bool close_connection{false};
    };
    using Handler = std::function<Response(const Request&)>;

    /** Optional restricted handler: exactly one additional worker/transport
     * permit may serve a queued connection while ordinary permits are full.
     * It receives no suspend/resume callbacks. It MUST fully validate and
     * budget health requests itself, never run a general/action/wait handler,
     * and refuse every other request before action admission with an explicit
     * busy response and close_connection=true. Queue/connection/body/reply
     * limits and original deadlines remain shared; this is not availability
     * under full admission, slow TLS/body input or exhausted API budgets.
     * Idle keepalive connections are reclassified on EVERY readable request.
     * Its configured reply bound must fit FLOWMESH_HTTPS_RESERVED_BUSY_BODY.
     * Generic two-argument callers retain their existing worker/permit bound. */
    FlowMeshHttpsServer(Options options, Handler handler, Handler restricted_health_handler = {});
    ~FlowMeshHttpsServer();
    FlowMeshHttpsServer(const FlowMeshHttpsServer&) = delete;
    FlowMeshHttpsServer& operator=(const FlowMeshHttpsServer&) = delete;
    bool Start(std::string& error);
    void Stop();
    uint16_t Port() const;

private:
    friend struct FlowMeshHttpsTestAccess;
    // Test-only observation, configured before Start. Called after cleanup;
    // cannot change its byte budgets, deadline, admission or dispatch rules.
    bool SetRejectedCleanupObserverForTest(std::function<void(size_t, size_t)> observer);
    // Test-only observation, configured before Start. Reports the read-back
    // TCP_NODELAY state once per admitted connection, before its handshake.
    bool SetConnectionObserverForTest(std::function<void(bool tcp_nodelay)> observer);
    //! Leaf observation only: counted owners, queued owners, ordinary active/resuming.
    std::array<size_t, 4> AdmissionCountsForTest();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace node
#endif // BITCOIN_NODE_FLOWMESH_HTTPS_H
