// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NET_HTTPS_SSE_API_H
#define B3_FLOWMESH2_NET_HTTPS_SSE_API_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/net/client_api.h>

#include <cstddef>
#include <cstdint>
#include <memory>

/** The public HTTPS + Server-Sent Events API (slice R13: interface and
 * stub; plan section 3.8; SEC-7, UX-10; [CC] boundary 7).
 *
 * Target (M5-API-01, OWNER-GATED): a ClientApi over the existing OpenSSL
 * stack (OpenSSL >= 3.5, TLS 1.3 only, 0-RTT off, no new WebSocket
 * dependency), served by sentries, never by seats (research N-R11), with
 * one event loop that hands requests to an InProcessApi-like backend:
 *   POST /v2/actions          body = exact ActionV2 bytes; the immediate,
 *                             definite queue answer, then the AdmissionReply
 *   GET  /v2/actions/{id}     GetAction: pending, included (slot, outcome,
 *                             certificate, Merkle paths), expired (with
 *                             proof) or unknown
 *   GET  /v2/state/{account}  GetState: the leaf and its proof, labelled
 *   GET  /v2/stream           SSE (M5-API-02): (stream_epoch, seq,
 *                             head_slot) envelopes, snapshot then deltas,
 *                             1 s heartbeat, resubscribe on any gap
 * Connection limits are the research L1 defaults (PROVISIONAL): at most 10
 * connections per IP, 30 new per minute, 64 per /16 IPv4 or /48 IPv6.
 *
 * Stub: Create returns NotImplemented(M5-API-01).
 */
namespace flowmesh2::net {

//! Target shape of the server configuration (M5-API-01).
struct HttpsSseConfig {
    uint16_t port{0};                     //!< 0: none chosen (the outline binds nothing)
    bool zero_rtt{false};                 //!< must stay false (plan 3.8: 0-RTT off)
    size_t max_connections_per_ip{10};    //!< PROVISIONAL
    size_t max_new_connections_per_minute{30};
    size_t max_connections_per_netgroup{64};
    uint64_t heartbeat_ms{1'000};         //!< SSE heartbeat
};

class HttpsSseApi
{
public:
    HttpsSseApi() = delete;

    /** A ClientApi served over HTTPS + SSE in front of `backend` (which must
     * outlive it). Stub: NotImplemented(M5-API-01); it binds no socket and
     * reads no key.
     */
    static Result<std::unique_ptr<ClientApi>> Create(const HttpsSseConfig& config, ClientApi& backend);
};

} // namespace flowmesh2::net

#endif // B3_FLOWMESH2_NET_HTTPS_SSE_API_H
