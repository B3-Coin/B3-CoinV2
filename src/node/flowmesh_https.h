// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_NODE_FLOWMESH_HTTPS_H
#define BITCOIN_NODE_FLOWMESH_HTTPS_H

#include <util/fs.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace node {

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
};

/** Syntax and pin validation only: no DNS, filesystem or network access. */
bool ValidateFlowMeshHttpsEndpoint(const HttpsEndpoint& endpoint, std::string& error);
/** Validate and canonicalize an HTTPS origin for duplicate detection. Does not
 * change CA material or certificate pins, or perform network access. */
bool NormalizeFlowMeshHttpsEndpoint(HttpsEndpoint& endpoint, std::string& error);
/** Load configured CA material to catch missing/malformed trust files at
 * startup. Does not contact an endpoint or read a TLS private key. */
bool ValidateFlowMeshHttpsTrust(const HttpsEndpoint& endpoint, std::string& error);

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
 * JSON method allowlist and authenticated action admission. TLS authenticates
 * the server; existing signed actions authenticate trading authority.
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
        std::chrono::milliseconds request_timeout{std::chrono::seconds{10}};
        /** Sequential HTTP/1.1 reuse only. Idle connections remain within
         * max_connections, and do not occupy handler workers. */
        bool keep_alive{true};
        std::chrono::milliseconds idle_timeout{std::chrono::seconds{30}};
        std::chrono::milliseconds connection_lifetime{std::chrono::minutes{5}};
        size_t max_requests_per_connection{128};
    };
    struct Request {
        std::string path;
        std::string body;
        std::string remote_address;
    };
    struct Response {
        int status{200};
        std::string body;
    };
    using Handler = std::function<Response(const Request&)>;

    FlowMeshHttpsServer(Options options, Handler handler);
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
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace node
#endif // BITCOIN_NODE_FLOWMESH_HTTPS_H
