// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NET_HTTPS_SSE_API_H
#define B3_FLOWMESH2_NET_HTTPS_SSE_API_H

#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/net/client_api.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

/** The public HTTPS + Server-Sent Events API (slice R13 interface; landed
 * with M5-API-01 on 2026-11-14, stage 2 step 4b; plan section 3.8; SEC-7
 * PROVISIONAL (the TLS stack choice awaits the owner's ruling, M4-NET-05);
 * UX-10; [CC] boundary 7; ARCHITECTURE.md O-104). The implementation lives
 * in netio/https_sse_api.cpp (library fm2_netio: libevent's evhttp over
 * bufferevent_openssl; OpenSSL >= 3.0 as the build requires).
 *
 * A ClientApi in front of a backend (an InProcessApi, which must outlive it):
 * one event-loop thread serves TLS 1.3 only (0-RTT off, no tickets, no
 * compression, no renegotiation) and hands requests to the backend:
 *   POST /v2/actions         body = exact ActionV2 bytes (octet-stream); the
 *                            queue answer is immediate (429 BUSY, 413
 *                            OVERSIZE, 503 FENCED), then the AdmissionReply
 *                            as JSON once the node's Pump answered (200;
 *                            504 if it did not within reply_deadline_ms; the
 *                            reply is still taken later, so no request ever
 *                            leaks a reservation)
 *   POST /v2/actions/status  body = the original bytes; the ActionStatusReply
 *   GET  /v2/actions/{id}    the status of an action this API admitted (its
 *                            bytes are kept in a bounded index of ADMITTED
 *                            and DUPLICATE frames); 404 unknown otherwise
 *   GET  /v2/state/{account} GetState: the leaf, its proof and the certified
 *                            head, labelled
 *   GET  /v2/health          HealthReply
 *   GET  /v2/stream?channel=slots|account|book|trades&account=&market=&from=epoch:seq:head
 *                            SSE (M5-API-02): events `id: epoch:seq:head`,
 *                            `event: snapshot|delta|heartbeat|gap`, `data:` JSON;
 *                            a heartbeat every heartbeat_ms; `gap` closes the
 *                            stream (resubscribe with from=)
 *   GET  <static>            documents the owner registers (AddStatic: for
 *                            example /v2/genesis), served verbatim
 * JSON: hashes and ids as hex, encoded structures (envelopes, leaves, roots,
 * proofs, outcomes) as hex so clients verify them with b3verify; status and
 * code names as strings. Errors: `{"error": code, "reason": text}`.
 *
 * Limits (PROVISIONAL, research L1): per-IP connections, new connections per
 * minute, per /16 (IPv4) or /48 (IPv6) netgroup, plus a global connection cap,
 * a subscription cap, body and header caps, a request timeout and GET/POST
 * only. Certificate: cert_file and key_file (PEM), or, on REGTEST with
 * dev_self_signed, a P-256 self-signed certificate written to
 * self_signed_dir (api.crt, api.key 0600, api.fingerprint = the SHA-256 of
 * the SubjectPublicKeyInfo, hex) and logged nowhere else. Clients pin that
 * fingerprint.
 *
 * Threading: the event loop owns every connection; Notify() (any thread,
 * the node after a Pump or a publication) wakes it so replies and events go
 * out without polling timers. Stop (or the destructor) joins the thread.
 * Regtest seats serve this API themselves until sentries land (M4-NET-03;
 * N-R11 deferred, O-104).
 */
namespace flowmesh2::net {

inline constexpr std::string_view HTTPS_BACKEND_MISSING{"the HTTPS API needs its backend"};               //!< (INVALID_INPUT)
inline constexpr std::string_view HTTPS_ZERO_RTT{"0-RTT must stay off (plan 3.8)"};                       //!< (INVALID_INPUT)
inline constexpr std::string_view HTTPS_CERTIFICATE{"no certificate: give cert_file and key_file, or dev_self_signed"}; //!< (INVALID_INPUT)
inline constexpr std::string_view HTTPS_TLS{"the TLS context could not be set up"};                       //!< (STORAGE_FATAL)
inline constexpr std::string_view HTTPS_BIND{"the HTTPS API could not bind its port"};                    //!< (STORAGE_FATAL)
inline constexpr std::string_view HTTPS_SELF_SIGNED{"the self-signed certificate could not be written"};  //!< (STORAGE_FATAL)
inline constexpr std::string_view HTTPS_LIMIT_ZERO{"zero HTTPS limit"};                                   //!< (INVALID_INPUT)
inline constexpr std::string_view HTTPS_CLOSED{"the HTTPS API is stopped"};                               //!< (FENCED)

struct HttpsSseConfig {
    ProfileParams profile;                     //!< the backend's profile (POST /v2/actions/status and GET /v2/actions/{id} decode frames)
    std::string bind_address{"127.0.0.1"};
    uint16_t port{0};                          //!< 0: an ephemeral port (Port() tells)
    bool zero_rtt{false};                      //!< must stay false (plan 3.8: 0-RTT off)
    std::string cert_file;                     //!< PEM chain
    std::string key_file;                      //!< PEM key
    bool dev_self_signed{false};               //!< REGTEST: generate into self_signed_dir when the files are empty
    std::string self_signed_dir;
    size_t max_connections_per_ip{10};         //!< PROVISIONAL
    size_t max_new_connections_per_minute{30}; //!< per IP, PROVISIONAL
    size_t max_connections_per_netgroup{64};   //!< /16 IPv4, /48 IPv6, PROVISIONAL
    size_t max_connections{256};               //!< every open request and stream, PROVISIONAL
    size_t max_subscriptions{256};             //!< open SSE streams, PROVISIONAL
    size_t max_body_bytes{64 * 1024};
    size_t max_header_bytes{8 * 1024};
    uint64_t request_timeout_ms{10'000};
    uint64_t reply_deadline_ms{2'000};         //!< how long a POST waits for the node's Pump
    uint64_t heartbeat_ms{1'000};              //!< SSE heartbeat
    size_t max_action_index{65'536};           //!< ADMITTED/DUPLICATE frames kept for GET /v2/actions/{id}
};

struct HttpsSseStatus {
    uint16_t port{0};
    size_t open_requests{0}; //!< POSTs waiting for the node
    size_t streams{0};
    uint64_t requests_served{0};
    uint64_t refused{0};     //!< by a limit
};

class HttpsSseApi final : public ClientApi
{
public:
    //! Bind, load or make the certificate, start the event loop (see the file comment).
    static Result<std::unique_ptr<HttpsSseApi>> Create(const HttpsSseConfig& config, ClientApi& backend);
    ~HttpsSseApi() override;

    uint16_t Port() const;
    //! The SHA-256 of the served certificate's SubjectPublicKeyInfo, hex (what clients pin).
    std::string CertificateFingerprint() const;
    //! Wake the loop: replies or events may be ready (the node, after Pump or a publication; any thread).
    void Notify();
    //! Serve `body` verbatim at GET `path` with `content_type` (for example the genesis document).
    void AddStatic(const std::string& path, const std::string& content_type, std::string body);
    HttpsSseStatus Status() const;
    //! Join the loop and close every connection.
    void Stop();

    // ClientApi: the backend's, so the server is itself a ClientApi.
    Result<RequestId> Submit(std::span<const unsigned char> frame) override;
    Result<RequestId> SubmitStatus(const ActionV2& original) override;
    Result<RequestId> SubmitHealth() override;
    std::optional<ApiReply> TakeReply(RequestId id) override;
    Result<ActionStatusReply> GetAction(const ActionV2& original) const override;
    Result<read::AccountReply> GetState(const AccountId& account) const override;
    Result<StreamId> Subscribe(const StreamRequest& request) override;
    Result<std::vector<StreamEvent>> TakeEvents(StreamId id, size_t max) override;
    Result<void> Unsubscribe(StreamId id) override;

private:
    struct Impl;
    explicit HttpsSseApi(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace flowmesh2::net

#endif // B3_FLOWMESH2_NET_HTTPS_SSE_API_H
