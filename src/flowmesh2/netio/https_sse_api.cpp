// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/net/https_sse_api.h>

#include <flowmesh2/core/action.h>
#include <flowmesh2/net/stream_hub.h>
#include <flowmesh2/netio/api_json.h>
#include <sync.h>
#include <threadsafety.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <event2/thread.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace flowmesh2::net {

namespace {

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

constexpr int STATUS_OK{200};
constexpr int STATUS_BAD_REQUEST{400};
constexpr int STATUS_NOT_FOUND{404};
constexpr int STATUS_METHOD_NOT_ALLOWED{405};
constexpr int STATUS_PAYLOAD_TOO_LARGE{413};
constexpr int STATUS_TOO_MANY{429};
constexpr int STATUS_SERVICE_UNAVAILABLE{503};
constexpr int STATUS_GATEWAY_TIMEOUT{504};
constexpr long TEN_YEARS_SECONDS{3650L * 24 * 60 * 60};

std::once_flag g_evthread_once;

int HttpStatusOf(const Error& error)
{
    switch (error.code) {
    case ErrorCode::BUSY: return STATUS_TOO_MANY;
    case ErrorCode::OVERSIZE: return STATUS_PAYLOAD_TOO_LARGE;
    case ErrorCode::FENCED: return STATUS_SERVICE_UNAVAILABLE;
    case ErrorCode::NEED_DATA: return STATUS_SERVICE_UNAVAILABLE;
    default: return STATUS_BAD_REQUEST;
    }
}

std::string NetGroupOf(const std::string& address)
{
    // IPv4: the /16; IPv6: the first three hextets (/48); anything else: itself.
    if (address.find(':') == std::string::npos) {
        size_t dots{0};
        for (size_t i{0}; i < address.size(); ++i) {
            if (address[i] == '.' && ++dots == 2) return address.substr(0, i);
        }
        return address;
    }
    size_t colons{0};
    for (size_t i{0}; i < address.size(); ++i) {
        if (address[i] == ':' && ++colons == 3) return address.substr(0, i);
    }
    return address;
}

std::string SpkiFingerprint(EVP_PKEY* key)
{
    unsigned char* der{nullptr};
    const int len{i2d_PUBKEY(key, &der)};
    if (len <= 0 || der == nullptr) return std::string{};
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(der, static_cast<size_t>(len), digest);
    OPENSSL_free(der);
    return netio::ToHex(std::span<const unsigned char>{digest, SHA256_DIGEST_LENGTH});
}

} // namespace

struct HttpsSseApi::Impl {
    Impl(const HttpsSseConfig& config, ClientApi& backend_in) : cfg{config}, backend{backend_in} {}
    ~Impl();

    const HttpsSseConfig cfg;
    ClientApi& backend;
    SSL_CTX* ctx{nullptr};
    std::string fingerprint;
    event_base* base{nullptr};
    evhttp* http{nullptr};
    evhttp_bound_socket* bound{nullptr};
    uint16_t port{0};
    event* wake{nullptr};
    event* timer{nullptr};
    std::thread thread;
    std::atomic<bool> running{false};

    // The loop thread's own state.
    struct Pending {
        RequestId id;
        RequestKind kind{RequestKind::SUBMIT_ACTION};
        evhttp_request* req{nullptr}; //!< null once the client left or the deadline passed
        evhttp_connection* conn{nullptr};
        std::string ip;
        Clock::time_point deadline;
        std::vector<unsigned char> frame; //!< SUBMIT_ACTION: the bytes, for the action index
    };
    std::map<uint64_t, Pending> pending;
    struct Stream {
        StreamId id;
        evhttp_request* req{nullptr};
        evhttp_connection* conn{nullptr};
        std::string ip;
        Clock::time_point last_send;
    };
    std::map<uint64_t, Stream> streams;
    std::map<std::string, size_t> open_per_ip;
    std::map<std::string, size_t> open_per_group;
    std::map<std::string, std::deque<Clock::time_point>> recent_per_ip;
    size_t open_total{0};
    std::map<ActionId, std::vector<unsigned char>> action_index;
    std::deque<ActionId> action_order;

    // Shared.
    mutable Mutex mutex;
    std::map<std::string, std::pair<std::string, std::string>> statics GUARDED_BY(mutex); //!< path -> (content type, body)
    HttpsSseStatus status GUARDED_BY(mutex);

    Result<void> SetupTls() EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    Result<void> MakeSelfSigned();
    Result<void> Start() EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    void Run();
    static void OnRequest(evhttp_request* req, void* arg);
    static void OnWake(evutil_socket_t, short, void* arg);
    static void OnTimer(evutil_socket_t, short, void* arg);
    static void OnClose(evhttp_connection* conn, void* arg);
    static bufferevent* OnNewConnection(event_base* base, void* arg);
    void Handle(evhttp_request* req) EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    void Pump() EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    void Reply(evhttp_request* req, int code, const UniValue& body) EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    void ReplyError(evhttp_request* req, int code, std::string_view error, std::string_view reason) EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    //! A connection unit opened (a request or a stream); false (and a 429 sent) when a limit refuses it.
    bool Admit(evhttp_request* req, const std::string& ip) EXCLUSIVE_LOCKS_REQUIRED(!mutex);
    void Release(const std::string& ip);
    void Remember(const ActionId& id, std::vector<unsigned char> frame);
    void SendEvent(Stream& stream, const std::string& id, std::string_view event, const std::string& data);
    void CloseStream(uint64_t key, bool gap);
    //! Bytes queued to the stream's connection and not yet written to its socket.
    static size_t QueuedOutput(const Stream& stream);
};

HttpsSseApi::Impl::~Impl()
{
    if (http != nullptr) evhttp_free(http);
    if (wake != nullptr) event_free(wake);
    if (timer != nullptr) event_free(timer);
    if (base != nullptr) event_base_free(base);
    if (ctx != nullptr) SSL_CTX_free(ctx);
}

// ---- Lifecycle ----------------------------------------------------------------------

Result<std::unique_ptr<HttpsSseApi>> HttpsSseApi::Create(const HttpsSseConfig& config, ClientApi& backend)
{
    if (config.zero_rtt) return Error{ErrorCode::INVALID_INPUT, HTTPS_ZERO_RTT};
    if (config.max_connections == 0 || config.max_subscriptions == 0 || config.max_body_bytes == 0 || config.max_header_bytes == 0 ||
        config.max_connections_per_ip == 0 || config.max_new_connections_per_minute == 0 || config.max_connections_per_netgroup == 0 ||
        config.max_action_index == 0 || config.reply_deadline_ms == 0 || config.heartbeat_ms == 0) {
        return Error{ErrorCode::INVALID_INPUT, HTTPS_LIMIT_ZERO};
    }
    if ((config.cert_file.empty() || config.key_file.empty()) && !config.dev_self_signed) return Error{ErrorCode::INVALID_INPUT, HTTPS_CERTIFICATE};
    if (config.dev_self_signed && config.cert_file.empty() && config.self_signed_dir.empty()) return Error{ErrorCode::INVALID_INPUT, HTTPS_CERTIFICATE};
    auto impl{std::make_unique<Impl>(config, backend)};
    if (const Result<void> tls{impl->SetupTls()}; !tls.has_value()) return tls.error();
    if (const Result<void> started{impl->Start()}; !started.has_value()) return started.error();
    std::unique_ptr<HttpsSseApi> api{new HttpsSseApi{std::move(impl)}};
    return Ok(std::move(api));
}

HttpsSseApi::HttpsSseApi(std::unique_ptr<Impl> impl) : m_impl{std::move(impl)} {}

HttpsSseApi::~HttpsSseApi() { Stop(); }

Result<void> HttpsSseApi::Impl::MakeSelfSigned()
{
    EVP_PKEY* key{EVP_EC_gen("P-256")};
    if (key == nullptr) return Error{ErrorCode::STORAGE_FATAL, HTTPS_SELF_SIGNED};
    X509* cert{X509_new()};
    if (cert == nullptr) {
        EVP_PKEY_free(key);
        return Error{ErrorCode::STORAGE_FATAL, HTTPS_SELF_SIGNED};
    }
    bool ok{X509_set_version(cert, 2) == 1 && ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) == 1 &&
            X509_gmtime_adj(X509_getm_notBefore(cert), 0) != nullptr && X509_gmtime_adj(X509_getm_notAfter(cert), TEN_YEARS_SECONDS) != nullptr &&
            X509_set_pubkey(cert, key) == 1};
    X509_NAME* name{X509_NAME_new()};
    ok = ok && name != nullptr &&
         X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("fm2d regtest"), -1, -1, 0) == 1 &&
         X509_set_subject_name(cert, name) == 1 && X509_set_issuer_name(cert, name) == 1;
    if (name != nullptr) X509_NAME_free(name);
    if (ok) {
        X509_EXTENSION* san{X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, "IP:127.0.0.1,DNS:localhost")};
        ok = san != nullptr && X509_add_ext(cert, san, -1) == 1;
        if (san != nullptr) X509_EXTENSION_free(san);
    }
    ok = ok && X509_sign(cert, key, EVP_sha256()) > 0;
    const std::string dir{cfg.self_signed_dir};
    const std::string cert_path{dir + "/api.crt"};
    const std::string key_path{dir + "/api.key"};
    const std::string fingerprint_path{dir + "/api.fingerprint"};
    if (ok) {
        ::mkdir(dir.c_str(), 0700);
        FILE* cf{std::fopen(cert_path.c_str(), "wb")};
        ok = cf != nullptr && PEM_write_X509(cf, cert) == 1;
        if (cf != nullptr) std::fclose(cf);
        // The key file is private from its creation (mode 0600 at open), not world-readable until a chmod (review 2026-11-16, R11).
        const int kfd{::open(key_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)};
        FILE* kf{kfd < 0 ? nullptr : ::fdopen(kfd, "wb")};
        if (kf == nullptr && kfd >= 0) ::close(kfd);
        ok = ok && kf != nullptr && PEM_write_PrivateKey(kf, key, nullptr, nullptr, 0, nullptr, nullptr) == 1;
        if (kf != nullptr) std::fclose(kf);
    }
    if (ok) {
        ok = SSL_CTX_use_certificate(ctx, cert) == 1 && SSL_CTX_use_PrivateKey(ctx, key) == 1 && SSL_CTX_check_private_key(ctx) == 1;
        fingerprint = SpkiFingerprint(key);
        FILE* ff{std::fopen(fingerprint_path.c_str(), "wb")};
        if (ff != nullptr) {
            std::fputs((fingerprint + "\n").c_str(), ff);
            std::fclose(ff);
        }
    }
    X509_free(cert);
    EVP_PKEY_free(key);
    if (!ok) return Error{ErrorCode::STORAGE_FATAL, HTTPS_SELF_SIGNED};
    return Ok();
}

Result<void> HttpsSseApi::Impl::SetupTls()
{
    ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == nullptr) return Error{ErrorCode::STORAGE_FATAL, HTTPS_TLS};
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) != 1) return Error{ErrorCode::STORAGE_FATAL, HTTPS_TLS};
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_TICKET);
    SSL_CTX_set_num_tickets(ctx, 0);
    SSL_CTX_set_max_early_data(ctx, 0);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    if (!cfg.cert_file.empty() && !cfg.key_file.empty()) {
        if (SSL_CTX_use_certificate_chain_file(ctx, cfg.cert_file.c_str()) != 1 ||
            SSL_CTX_use_PrivateKey_file(ctx, cfg.key_file.c_str(), SSL_FILETYPE_PEM) != 1 || SSL_CTX_check_private_key(ctx) != 1) {
            return Error{ErrorCode::INVALID_INPUT, HTTPS_CERTIFICATE};
        }
        X509* cert{SSL_CTX_get0_certificate(ctx)};
        EVP_PKEY* key{cert != nullptr ? X509_get0_pubkey(cert) : nullptr};
        if (key == nullptr) return Error{ErrorCode::INVALID_INPUT, HTTPS_CERTIFICATE};
        fingerprint = SpkiFingerprint(key);
        return Ok();
    }
    return MakeSelfSigned();
}

Result<void> HttpsSseApi::Impl::Start()
{
    std::call_once(g_evthread_once, [] { evthread_use_pthreads(); });
    base = event_base_new();
    if (base == nullptr) return Error{ErrorCode::STORAGE_FATAL, HTTPS_BIND};
    http = evhttp_new(base);
    if (http == nullptr) return Error{ErrorCode::STORAGE_FATAL, HTTPS_BIND};
    evhttp_set_allowed_methods(http, EVHTTP_REQ_GET | EVHTTP_REQ_POST);
    evhttp_set_max_body_size(http, static_cast<ev_ssize_t>(cfg.max_body_bytes));
    evhttp_set_max_headers_size(http, static_cast<ev_ssize_t>(cfg.max_header_bytes));
    evhttp_set_timeout(http, static_cast<int>(std::max<uint64_t>(1, cfg.request_timeout_ms / 1000)));
    evhttp_set_bevcb(http, &Impl::OnNewConnection, this);
    evhttp_set_gencb(http, &Impl::OnRequest, this);
    bound = evhttp_bind_socket_with_handle(http, cfg.bind_address.c_str(), cfg.port);
    if (bound == nullptr) return Error{ErrorCode::STORAGE_FATAL, HTTPS_BIND};
    const evutil_socket_t fd{evhttp_bound_socket_get_fd(bound)};
    sockaddr_storage storage{};
    socklen_t len{sizeof(storage)};
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&storage), &len) == 0) {
        if (storage.ss_family == AF_INET) {
            port = ntohs(reinterpret_cast<sockaddr_in*>(&storage)->sin_port);
        } else if (storage.ss_family == AF_INET6) {
            port = ntohs(reinterpret_cast<sockaddr_in6*>(&storage)->sin6_port);
        }
    }
    wake = event_new(base, -1, EV_PERSIST, &Impl::OnWake, this);
    timer = event_new(base, -1, EV_PERSIST, &Impl::OnTimer, this);
    if (wake == nullptr || timer == nullptr) return Error{ErrorCode::STORAGE_FATAL, HTTPS_BIND};
    event_add(wake, nullptr);
    const uint64_t period_ms{std::clamp<uint64_t>(cfg.heartbeat_ms / 4, 20, 250)};
    timeval period{static_cast<time_t>(period_ms / 1000), static_cast<suseconds_t>((period_ms % 1000) * 1000)};
    event_add(timer, &period);
    {
        LOCK(mutex);
        status.port = port;
    }
    running = true;
    thread = std::thread{[this] { Run(); }};
    return Ok();
}

void HttpsSseApi::Impl::Run() { event_base_loop(base, EVLOOP_NO_EXIT_ON_EMPTY); }

void HttpsSseApi::Stop()
{
    if (!m_impl) return;
    if (m_impl->running.exchange(false)) {
        // The loop may not have entered event_base_loop yet, and libevent clears a loopbreak on entry, so a Stop right after
        // Create used to hang in join. The wake event stays active until the loop processes it, whenever that is, and its
        // callback breaks the loop from inside (review 2026-11-16).
        if (m_impl->wake != nullptr) event_active(m_impl->wake, 0, 0);
        if (m_impl->thread.joinable()) m_impl->thread.join();
        for (auto& [key, stream] : m_impl->streams) (void)m_impl->backend.Unsubscribe(stream.id);
        m_impl->streams.clear();
        m_impl->pending.clear();
    }
}

bufferevent* HttpsSseApi::Impl::OnNewConnection(event_base* base, void* arg)
{
    auto* self{static_cast<Impl*>(arg)};
    SSL* ssl{SSL_new(self->ctx)};
    if (ssl == nullptr) return nullptr;
    return bufferevent_openssl_socket_new(base, -1, ssl, BUFFEREVENT_SSL_ACCEPTING, BEV_OPT_CLOSE_ON_FREE);
}

// ---- Requests ------------------------------------------------------------------------

void HttpsSseApi::Impl::OnRequest(evhttp_request* req, void* arg) NO_THREAD_SAFETY_ANALYSIS { static_cast<Impl*>(arg)->Handle(req); }
void HttpsSseApi::Impl::OnWake(evutil_socket_t, short, void* arg) NO_THREAD_SAFETY_ANALYSIS
{
    auto* self{static_cast<Impl*>(arg)};
    if (!self->running) {
        event_base_loopbreak(self->base); // Stop() asked, from inside the loop so it is never lost
        return;
    }
    self->Pump();
}
void HttpsSseApi::Impl::OnTimer(evutil_socket_t, short, void* arg) NO_THREAD_SAFETY_ANALYSIS
{
    auto* self{static_cast<Impl*>(arg)};
    if (!self->running) {
        event_base_loopbreak(self->base);
        return;
    }
    self->Pump();
}

void HttpsSseApi::Impl::OnClose(evhttp_connection* conn, void* arg) NO_THREAD_SAFETY_ANALYSIS
{
    auto* self{static_cast<Impl*>(arg)};
    // libevent frees the requests still attached to the connection it closes. A request whose reply had not been sent
    // (pending) or had only started (a stream) when the client left is detached first, its connection null, and is this
    // side's to free: it leaked before (review of 2026-11-16, the LSan report the asan CI job was predicted to raise).
    const auto drop{[](evhttp_request*& req) {
        if (req != nullptr && evhttp_request_get_connection(req) == nullptr) evhttp_request_free(req);
        req = nullptr;
    }};
    for (auto& [id, item] : self->pending) {
        if (item.conn == conn && item.req != nullptr) {
            drop(item.req); // the reply is still taken when it comes (no reservation leaks)
            self->Release(item.ip);
            item.conn = nullptr;
        }
    }
    for (auto it{self->streams.begin()}; it != self->streams.end();) {
        if (it->second.conn == conn) {
            (void)self->backend.Unsubscribe(it->second.id);
            self->Release(it->second.ip);
            drop(it->second.req);
            it = self->streams.erase(it);
        } else {
            ++it;
        }
    }
}

void HttpsSseApi::Impl::Reply(evhttp_request* req, int code, const UniValue& body)
{
    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
    evhttp_add_header(evhttp_request_get_output_headers(req), "Cache-Control", "no-store");
    evbuffer* out{evbuffer_new()};
    const std::string text{body.write() + "\n"};
    evbuffer_add(out, text.data(), text.size());
    evhttp_send_reply(req, code, nullptr, out);
    evbuffer_free(out);
    LOCK(mutex);
    ++status.requests_served;
}

void HttpsSseApi::Impl::ReplyError(evhttp_request* req, int code, std::string_view error, std::string_view reason)
{
    Reply(req, code, netio::ErrorJson(error, reason));
}

bool HttpsSseApi::Impl::Admit(evhttp_request* req, const std::string& ip)
{
    const Clock::time_point now{Clock::now()};
    std::deque<Clock::time_point>& recent{recent_per_ip[ip]};
    while (!recent.empty() && now - recent.front() > std::chrono::seconds{60}) recent.pop_front();
    const std::string group{NetGroupOf(ip)};
    const bool refused{open_total >= cfg.max_connections || open_per_ip[ip] >= cfg.max_connections_per_ip ||
                       recent.size() >= cfg.max_new_connections_per_minute || open_per_group[group] >= cfg.max_connections_per_netgroup};
    if (refused) {
        {
            LOCK(mutex);
            ++status.refused;
        }
        ReplyError(req, STATUS_TOO_MANY, "BUSY", "connection limit");
        return false;
    }
    recent.push_back(now);
    ++open_total;
    ++open_per_ip[ip];
    ++open_per_group[group];
    return true;
}

void HttpsSseApi::Impl::Release(const std::string& ip)
{
    if (open_total > 0) --open_total;
    // Entries at zero leave the maps, so they do not grow with every address ever seen (review 2026-11-16).
    if (auto it{open_per_ip.find(ip)}; it != open_per_ip.end()) {
        if (it->second > 0) --it->second;
        if (it->second == 0) open_per_ip.erase(it);
    }
    if (auto it{open_per_group.find(NetGroupOf(ip))}; it != open_per_group.end()) {
        if (it->second > 0) --it->second;
        if (it->second == 0) open_per_group.erase(it);
    }
}

void HttpsSseApi::Impl::Remember(const ActionId& id, std::vector<unsigned char> frame)
{
    if (action_index.contains(id)) return;
    action_index.emplace(id, std::move(frame));
    action_order.push_back(id);
    while (action_order.size() > cfg.max_action_index) {
        action_index.erase(action_order.front());
        action_order.pop_front();
    }
}

void HttpsSseApi::Impl::Handle(evhttp_request* req)
{
    evhttp_connection* conn{evhttp_request_get_connection(req)};
    char* address{nullptr};
    ev_uint16_t peer_port{0};
    if (conn != nullptr) evhttp_connection_get_peer(conn, &address, &peer_port);
    const std::string ip{address != nullptr ? address : "?"};
    const evhttp_cmd_type method{evhttp_request_get_command(req)};
    if (method != EVHTTP_REQ_GET && method != EVHTTP_REQ_POST) return ReplyError(req, STATUS_METHOD_NOT_ALLOWED, "METHOD", "GET or POST");
    const evhttp_uri* uri{evhttp_request_get_evhttp_uri(req)};
    const char* path_c{uri != nullptr ? evhttp_uri_get_path(uri) : nullptr};
    const std::string path{path_c != nullptr ? path_c : "/"};
    const char* query_c{uri != nullptr ? evhttp_uri_get_query(uri) : nullptr};
    const std::string query{query_c != nullptr ? query_c : ""};
    if (!Admit(req, ip)) return;
    // Everything below either answers synchronously (and releases) or registers a pending request or a stream.
    const auto sync{[&](int code, const UniValue& body) {
        Reply(req, code, body);
        Release(ip);
    }};
    const auto body_bytes{[&]() -> std::vector<unsigned char> {
        evbuffer* in{evhttp_request_get_input_buffer(req)};
        const size_t len{in != nullptr ? evbuffer_get_length(in) : 0};
        std::vector<unsigned char> bytes(len);
        if (len > 0) evbuffer_remove(in, bytes.data(), len);
        return bytes;
    }};
    const auto queue{[&](Result<RequestId> id, RequestKind kind, std::vector<unsigned char> frame) {
        if (!id.has_value()) return sync(HttpStatusOf(id.error()), netio::ErrorJson(id.error()));
        Pending item;
        item.id = *id;
        item.kind = kind;
        item.req = req;
        item.conn = conn;
        item.ip = ip;
        item.deadline = Clock::now() + Ms{static_cast<int64_t>(cfg.reply_deadline_ms)};
        item.frame = std::move(frame);
        pending.emplace(id->value, std::move(item));
        if (conn != nullptr) evhttp_connection_set_closecb(conn, &Impl::OnClose, this);
        Pump();
    }};

    if (method == EVHTTP_REQ_POST && path == "/v2/actions") {
        std::vector<unsigned char> frame{body_bytes()};
        if (frame.empty()) return sync(STATUS_BAD_REQUEST, netio::ErrorJson("EMPTY", "an action frame is expected"));
        Result<RequestId> id{backend.Submit(frame)};
        return queue(std::move(id), RequestKind::SUBMIT_ACTION, std::move(frame));
    }
    if (method == EVHTTP_REQ_POST && path == "/v2/actions/status") {
        const std::vector<unsigned char> frame{body_bytes()};
        Result<ActionV2> original{ActionV2::Decode(frame, cfg.profile)};
        if (!original.has_value()) return sync(STATUS_BAD_REQUEST, netio::ErrorJson(original.error()));
        return queue(backend.SubmitStatus(*original), RequestKind::ACTION_STATUS, {});
    }
    if (method != EVHTTP_REQ_GET) return sync(STATUS_METHOD_NOT_ALLOWED, netio::ErrorJson("METHOD", "GET expected"));
    if (path == "/v2/health") return queue(backend.SubmitHealth(), RequestKind::HEALTH, {});
    if (path.starts_with("/v2/actions/")) {
        const std::optional<uint256> id_hash{uint256::FromHex(path.substr(12))};
        if (!id_hash.has_value()) return sync(STATUS_BAD_REQUEST, netio::ErrorJson("ID", "a 32-byte hex action id"));
        const ActionId id{*id_hash};
        const auto it{action_index.find(id)};
        if (it == action_index.end()) return sync(STATUS_NOT_FOUND, netio::ErrorJson("UNKNOWN", "this API did not admit that action"));
        const Result<ActionV2> original{ActionV2::Decode(it->second, cfg.profile)};
        if (!original.has_value()) return sync(STATUS_BAD_REQUEST, netio::ErrorJson(original.error()));
        const Result<ActionStatusReply> reply{backend.GetAction(*original)};
        if (!reply.has_value()) return sync(HttpStatusOf(reply.error()), netio::ErrorJson(reply.error()));
        return sync(STATUS_OK, netio::ActionStatusJson(*reply));
    }
    if (path.starts_with("/v2/state/")) {
        const std::optional<uint256> account_hash{uint256::FromHex(path.substr(10))};
        if (!account_hash.has_value()) return sync(STATUS_BAD_REQUEST, netio::ErrorJson("ID", "a 32-byte hex account id"));
        const AccountId account{*account_hash};
        const Result<read::AccountReply> reply{backend.GetState(account)};
        if (!reply.has_value()) return sync(HttpStatusOf(reply.error()), netio::ErrorJson(reply.error()));
        return sync(STATUS_OK, netio::AccountReplyJson(*reply));
    }
    if (path == "/v2/stream") {
        evkeyvalq params{};
        evhttp_parse_query_str(query.c_str(), &params);
        const char* channel_c{evhttp_find_header(&params, "channel")};
        const char* account_c{evhttp_find_header(&params, "account")};
        const char* market_c{evhttp_find_header(&params, "market")};
        const char* from_c{evhttp_find_header(&params, "from")};
        StreamRequest request;
        bool ok{channel_c != nullptr};
        const std::string channel{channel_c != nullptr ? channel_c : ""};
        if (channel == "slots") {
            request.channel = StreamChannel::SLOTS;
        } else if (channel == "account") {
            request.channel = StreamChannel::ACCOUNT;
        } else if (channel == "book") {
            request.channel = StreamChannel::BOOK;
        } else if (channel == "trades") {
            request.channel = StreamChannel::TRADES;
        } else {
            ok = false;
        }
        if (ok && account_c != nullptr) {
            const std::optional<uint256> hash{uint256::FromHex(account_c)};
            ok = hash.has_value();
            if (ok) request.account = AccountId{*hash};
        }
        if (ok && market_c != nullptr) {
            const std::optional<uint256> hash{uint256::FromHex(market_c)};
            ok = hash.has_value();
            if (ok) request.market = MarketId{*hash};
        }
        if (ok && from_c != nullptr) {
            request.from = netio::CursorFromText(from_c);
            ok = request.from.has_value();
        }
        evhttp_clear_headers(&params);
        if (!ok) return sync(STATUS_BAD_REQUEST, netio::ErrorJson("QUERY", "channel=slots|account|book|trades, account=, market=, from=epoch:seq:head"));
        if (streams.size() >= cfg.max_subscriptions) {
            {
                LOCK(mutex);
                ++status.refused;
            }
            return sync(STATUS_TOO_MANY, netio::ErrorJson("BUSY", "too many streams"));
        }
        Result<StreamId> id{backend.Subscribe(request)};
        if (!id.has_value()) return sync(HttpStatusOf(id.error()), netio::ErrorJson(id.error()));
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "text/event-stream");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Cache-Control", "no-store");
        evhttp_send_reply_start(req, STATUS_OK, "OK");
        Stream stream;
        stream.id = *id;
        stream.req = req;
        stream.conn = conn;
        stream.ip = ip;
        stream.last_send = Clock::now();
        streams.emplace(id->value, std::move(stream));
        if (conn != nullptr) evhttp_connection_set_closecb(conn, &Impl::OnClose, this);
        {
            LOCK(mutex);
            ++status.requests_served;
        }
        return Pump();
    }
    {
        LOCK(mutex);
        const auto it{statics.find(path)};
        if (it != statics.end()) {
            evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", it->second.first.c_str());
            evbuffer* out{evbuffer_new()};
            evbuffer_add(out, it->second.second.data(), it->second.second.size());
            evhttp_send_reply(req, STATUS_OK, nullptr, out);
            evbuffer_free(out);
            ++status.requests_served;
            Release(ip);
            return;
        }
    }
    sync(STATUS_NOT_FOUND, netio::ErrorJson("NOT_FOUND", "no such path"));
}

void HttpsSseApi::Impl::SendEvent(Stream& stream, const std::string& id, std::string_view event, const std::string& data)
{
    if (stream.req == nullptr) return;
    std::string text;
    if (!id.empty()) text += "id: " + id + "\n";
    text += "event: " + std::string{event} + "\n";
    text += "data: " + data + "\n\n";
    evbuffer* out{evbuffer_new()};
    evbuffer_add(out, text.data(), text.size());
    evhttp_send_reply_chunk(stream.req, out);
    evbuffer_free(out);
    stream.last_send = Clock::now();
}

size_t HttpsSseApi::Impl::QueuedOutput(const Stream& stream)
{
    if (stream.conn == nullptr) return 0;
    bufferevent* bev{evhttp_connection_get_bufferevent(stream.conn)};
    if (bev == nullptr) return 0;
    return evbuffer_get_length(bufferevent_get_output(bev));
}

void HttpsSseApi::Impl::CloseStream(uint64_t key, bool gap)
{
    const auto it{streams.find(key)};
    if (it == streams.end()) return;
    Stream& stream{it->second};
    if (gap) SendEvent(stream, "", "gap", netio::ErrorJson("STREAM_GAP", STREAM_GAP).write());
    if (stream.req != nullptr) evhttp_send_reply_end(stream.req);
    if (!gap) (void)backend.Unsubscribe(stream.id);
    Release(stream.ip);
    streams.erase(it);
}

void HttpsSseApi::Impl::Pump()
{
    const Clock::time_point now{Clock::now()};
    // Replies: every one the backend produced is taken, whether or not its client is still there.
    for (auto it{pending.begin()}; it != pending.end();) {
        Pending& item{it->second};
        const std::optional<ApiReply> reply{backend.TakeReply(item.id)};
        if (reply.has_value()) {
            if (item.kind == RequestKind::SUBMIT_ACTION) {
                if (const auto* admitted{std::get_if<admission::AdmissionReply>(&reply->body)}) {
                    if ((admitted->code == AdmissionCode::ADMITTED || admitted->code == AdmissionCode::DUPLICATE) && admitted->id.has_value()) {
                        Remember(*admitted->id, std::move(item.frame));
                    }
                }
            }
            if (item.req != nullptr) {
                std::visit(
                    [&](const auto& body) {
                        using T = std::decay_t<decltype(body)>;
                        if constexpr (std::is_same_v<T, admission::AdmissionReply>) {
                            Reply(item.req, STATUS_OK, netio::AdmissionReplyJson(body));
                        } else if constexpr (std::is_same_v<T, ActionStatusReply>) {
                            Reply(item.req, STATUS_OK, netio::ActionStatusJson(body));
                        } else if constexpr (std::is_same_v<T, HealthReply>) {
                            Reply(item.req, STATUS_OK, netio::HealthJson(body));
                        } else {
                            Reply(item.req, HttpStatusOf(body), netio::ErrorJson(body));
                        }
                    },
                    reply->body);
                Release(item.ip);
            }
            it = pending.erase(it);
            continue;
        }
        if (item.req != nullptr && now > item.deadline) {
            ReplyError(item.req, STATUS_GATEWAY_TIMEOUT, "TIMEOUT", "the node did not answer in time; the outcome is unknown, resend the exact bytes");
            Release(item.ip);
            item.req = nullptr; // keep waiting for the reply itself
        }
        ++it;
    }
    // Streams: events, gaps, heartbeats. CloseStream erases from the map, so the keys to close are collected and closed
    // after the loop (review 2026-11-16, defect 5: a gap used to erase the element the loop was on).
    std::vector<uint64_t> gone;
    std::vector<uint64_t> gapped;
    size_t most_queued{0};
    for (auto& [key, stream] : streams) {
        if (stream.req == nullptr) {
            gone.push_back(key);
            continue;
        }
        // Backpressure (R2): a connection with stream_output_bytes queued takes no more events this round; they wait in the
        // hub, whose queue cap ends a stream that stays behind with a gap.
        const size_t queued{QueuedOutput(stream)};
        most_queued = std::max(most_queued, queued);
        const bool backpressured{queued >= cfg.stream_output_bytes};
        // A stalled connection takes nothing (max 0), which still reports the gap the hub declared once the reader fell too far
        // behind: the stream then ends with the gap event instead of lingering until the connection times out.
        Result<std::vector<StreamEvent>> events{backend.TakeEvents(stream.id, backpressured ? 0 : 64)};
        if (!events.has_value()) {
            gapped.push_back(key);
            continue;
        }
        for (const StreamEvent& event : *events) {
            SendEvent(stream, netio::CursorText(event.cursor), ToString(event.kind), netio::StreamEventJson(event).write());
        }
        // No heartbeat to a connection at its cap: the reader is not reading, and on a quiet hub nothing overflows into a gap,
        // so each beat used to grow its output buffer without bound (review workflow of 2026-11-18). Once it drains below
        // the cap, the beats resume.
        if (!backpressured && now - stream.last_send >= Ms{static_cast<int64_t>(cfg.heartbeat_ms)}) {
            SendEvent(stream, "", "heartbeat", "{}");
        }
    }
    for (const uint64_t key : gapped) CloseStream(key, /*gap=*/true);
    for (const uint64_t key : gone) CloseStream(key, false);
    // Addresses whose minute of arrivals has passed leave the rate map.
    for (auto it{recent_per_ip.begin()}; it != recent_per_ip.end();) {
        while (!it->second.empty() && now - it->second.front() > std::chrono::seconds{60}) it->second.pop_front();
        it = it->second.empty() ? recent_per_ip.erase(it) : std::next(it);
    }
    LOCK(mutex);
    status.stream_output_bytes = most_queued;
    status.open_requests = 0;
    for (const auto& [id, item] : pending) {
        if (item.req != nullptr) ++status.open_requests;
    }
    status.streams = streams.size();
}

// ---- The public surface ----------------------------------------------------------------

uint16_t HttpsSseApi::Port() const { return m_impl->port; }
std::string HttpsSseApi::CertificateFingerprint() const { return m_impl->fingerprint; }

void HttpsSseApi::Notify()
{
    if (m_impl->running && m_impl->wake != nullptr) event_active(m_impl->wake, 0, 0);
}

void HttpsSseApi::AddStatic(const std::string& path, const std::string& content_type, std::string body)
{
    LOCK(m_impl->mutex);
    m_impl->statics[path] = {content_type, std::move(body)};
}

HttpsSseStatus HttpsSseApi::Status() const
{
    LOCK(m_impl->mutex);
    return m_impl->status;
}

Result<RequestId> HttpsSseApi::Submit(std::span<const unsigned char> frame)
{
    if (!m_impl->running) return Error{ErrorCode::FENCED, HTTPS_CLOSED};
    return m_impl->backend.Submit(frame);
}
Result<RequestId> HttpsSseApi::SubmitStatus(const ActionV2& original) { return m_impl->backend.SubmitStatus(original); }
Result<RequestId> HttpsSseApi::SubmitHealth() { return m_impl->backend.SubmitHealth(); }
std::optional<ApiReply> HttpsSseApi::TakeReply(RequestId id) { return m_impl->backend.TakeReply(id); }
Result<ActionStatusReply> HttpsSseApi::GetAction(const ActionV2& original) const { return m_impl->backend.GetAction(original); }
Result<read::AccountReply> HttpsSseApi::GetState(const AccountId& account) const { return m_impl->backend.GetState(account); }
Result<StreamId> HttpsSseApi::Subscribe(const StreamRequest& request) { return m_impl->backend.Subscribe(request); }
Result<std::vector<StreamEvent>> HttpsSseApi::TakeEvents(StreamId id, size_t max) { return m_impl->backend.TakeEvents(id, max); }
Result<void> HttpsSseApi::Unsubscribe(StreamId id) { return m_impl->backend.Unsubscribe(id); }

} // namespace flowmesh2::net
