// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/params.h>
#include <flowmesh2/dev/dev_budget.h>
#include <flowmesh2/dev/dev_chain_file.h>
#include <flowmesh2/dev/fake_chain.h>
#include <flowmesh2/net/https_sse_api.h>
#include <flowmesh2/net/sync.h>
#include <flowmesh2/net/tcp_mesh.h>
#include <flowmesh2/netio/api_json.h>
#include <flowmesh2/netio/genesis_doc.h>
#include <flowmesh2/seat/seat_node.h>
#include <flowmesh2/storage/file_journal.h>
#include <key.h>
#include <logging.h>
#include <random.h>
#include <univalue.h>
#include <util/fs.h>
#include <util/signalinterrupt.h>
#include <util/threadnames.h>

#include <csignal>
#include <unistd.h>

#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

/** fm2d: one FlowMesh V2 seat as a process (stage 2 step 5, M5-NODE-01;
 * ARCHITECTURE.md O-105). REGTEST only in this stage: the chain source is
 * the shared dev chain file (dev/dev_chain_file.h, DEV ONLY) until the b3d
 * adapter lands (stage 3), the budget is the unlimited dev budget, and the
 * seat serves the HTTPS + SSE API itself (N-R11 deferred, O-104).
 *
 *   fm2d --datadir <dir> --genesis <genesis.json> --seat <index> --keys <dir>
 *        --devchain <file> [--listen <ip>] [--mesh-port <port>]
 *        [--api-bind <ip>] [--api-port <port>] [--heartbeat-ms 1000]
 *        [--log console|file|both]
 *
 * Start: the genesis document gives the seat set, the genesis head, the mesh
 * endpoints and the API ports; <keys>/seat.key (BLS IKM) and
 * <keys>/transport.key (secp256k1) are this seat's; the SIGNER and COMMIT
 * file journals live in <datadir>/signer and <datadir>/commit (created on
 * the first start, recovered afterwards: logs rebuilt, state restored, the
 * agreement resumes its durable view state); the dev chain file is replayed
 * into the fake chain adapter and then tailed; the TCP mesh binds; the HTTPS
 * API binds with a self-signed certificate written to <datadir>; then
 * <datadir>/fm2d.ready names the ports and the fingerprint.
 *
 * The sealing thread loops every millisecond: new dev chain blocks, mesh
 * frames to the node, the agreement's timer, decisions completed, the
 * leader's proposal when there is work (pending actions, new facts) or the
 * heartbeat is due, the API pumped. SIGINT/SIGTERM stop it; the API and the
 * mesh are stopped and the journals closed in that order.
 */
namespace {

using namespace flowmesh2;

util::SignalInterrupt* g_interrupt{nullptr};

void OnSignal(int) { (void)(*g_interrupt)(); }

int Usage(std::string_view problem)
{
    std::cerr << "fm2d: " << problem << "\n"
              << "usage: fm2d --datadir <dir> --genesis <genesis.json> --seat <i> --keys <dir> --devchain <file>\n"
              << "            [--listen <ip>] [--mesh-port <port>] [--api-bind <ip>] [--api-port <port>] [--heartbeat-ms <ms>] [--log console|file|both]\n";
    return 2;
}

std::optional<uint64_t> ParseU64(std::string_view text)
{
    uint64_t value{0};
    const auto [end, error]{std::from_chars(text.data(), text.data() + text.size(), value)};
    if (error != std::errc{} || end != text.data() + text.size() || text.empty()) return std::nullopt;
    return value;
}

std::optional<std::string> ReadFile(const fs::path& path)
{
    std::ifstream in{fs::PathToString(path)};
    if (!in) return std::nullopt;
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::optional<std::array<unsigned char, 32>> ReadKeyFile(const fs::path& path)
{
    const std::optional<std::string> text{ReadFile(path)};
    if (!text.has_value()) return std::nullopt;
    std::string hex{*text};
    while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r' || hex.back() == ' ')) hex.pop_back();
    const std::optional<std::vector<unsigned char>> bytes{netio::FromHex(hex)};
    if (!bytes.has_value() || bytes->size() != 32) return std::nullopt;
    std::array<unsigned char, 32> out{};
    std::copy(bytes->begin(), bytes->end(), out.begin());
    return out;
}

bool DirHasFiles(const fs::path& dir)
{
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    const fs::directory_iterator it{dir, ec};
    return !ec && it != fs::directory_iterator{};
}

//! The node's events into the log.
class LogHooks final : public seat::NodeHooks
{
public:
    void Record(std::string_view kind, std::string detail) override
    {
        // A message for another slot is routine while a peer catches up; the rest of the refusals matter.
        const bool routine{kind == "pbft.refuse" && detail.find("message for another slot") != std::string::npos};
        if (!routine && (kind == "pbft.refuse" || kind == "node.fence" || kind == "sync.refuse" || kind == "sync.evidence")) {
            LogWarning("fm2d %s %s", std::string{kind}, detail);
        } else if (kind.starts_with("sync.") || kind == "slot.propose") {
            LogInfo("fm2d %s %s", std::string{kind}, detail);
        } else {
            LogDebug(BCLog::NET, "fm2d %s %s\n", std::string{kind}, detail);
        }
    }
    void BeginSlot(uint64_t height) override { LogDebug(BCLog::NET, "fm2d slot.begin %u\n", height); }
    void EndSlot(uint64_t height, std::string_view result) override { LogInfo("fm2d slot %u %s", height, std::string{result}); }
};

struct Options {
    fs::path datadir;
    fs::path genesis;
    uint32_t seat{0};
    bool seat_given{false};
    fs::path keys;
    fs::path devchain;
    std::string listen{"127.0.0.1"};
    std::optional<uint16_t> mesh_port;
    std::string api_bind{"127.0.0.1"};
    std::optional<uint16_t> api_port;
    uint64_t heartbeat_ms{1'000};
    bool log_console{true};
    bool log_file{true};
};

int Fail(std::string_view what, const Error& error)
{
    LogError("fm2d: %s: %s %s", std::string{what}, std::string{ToString(error.code)}, std::string{error.detail});
    std::cerr << "fm2d: " << what << ": " << ToString(error.code) << " " << error.detail << "\n";
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    for (int i{1}; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") return Usage("help");
        if (i + 1 >= argc) return Usage("missing value for " + std::string{arg});
        const std::string value{argv[++i]};
        if (arg == "--datadir") {
            opt.datadir = fs::PathFromString(value);
        } else if (arg == "--genesis") {
            opt.genesis = fs::PathFromString(value);
        } else if (arg == "--seat") {
            const std::optional<uint64_t> seat{ParseU64(value)};
            if (!seat || *seat > 1'023) return Usage("bad seat");
            opt.seat = static_cast<uint32_t>(*seat);
            opt.seat_given = true;
        } else if (arg == "--keys") {
            opt.keys = fs::PathFromString(value);
        } else if (arg == "--devchain") {
            opt.devchain = fs::PathFromString(value);
        } else if (arg == "--listen") {
            opt.listen = value;
        } else if (arg == "--mesh-port") {
            const std::optional<uint64_t> port{ParseU64(value)};
            if (!port || *port == 0 || *port > 65'535) return Usage("bad mesh port");
            opt.mesh_port = static_cast<uint16_t>(*port);
        } else if (arg == "--api-bind") {
            opt.api_bind = value;
        } else if (arg == "--api-port") {
            const std::optional<uint64_t> port{ParseU64(value)};
            if (!port || *port > 65'535) return Usage("bad api port");
            opt.api_port = static_cast<uint16_t>(*port);
        } else if (arg == "--heartbeat-ms") {
            const std::optional<uint64_t> ms{ParseU64(value)};
            if (!ms || *ms == 0 || *ms > 60'000) return Usage("bad heartbeat");
            opt.heartbeat_ms = *ms;
        } else if (arg == "--log") {
            opt.log_console = value == "console" || value == "both";
            opt.log_file = value == "file" || value == "both";
            if (!opt.log_console && !opt.log_file) return Usage("--log takes console, file or both");
        } else {
            return Usage("unknown option " + std::string{arg});
        }
    }
    if (opt.datadir.empty() || opt.genesis.empty() || !opt.seat_given || opt.keys.empty() || opt.devchain.empty()) {
        return Usage("--datadir, --genesis, --seat, --keys and --devchain are required");
    }
    std::error_code ec;
    fs::create_directories(opt.datadir, ec);

    // Logging and signals.
    LogInstance().m_print_to_console = opt.log_console;
    LogInstance().m_print_to_file = opt.log_file;
    LogInstance().m_file_path = opt.datadir / fs::u8path("debug.log");
    LogInstance().m_log_timestamps = true;
    (void)LogInstance().StartLogging();
    util::SignalInterrupt interrupt;
    g_interrupt = &interrupt;
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::signal(SIGPIPE, SIG_IGN);
    util::ThreadRename("fm2d-seal");
    const ECC_Context ecc;

    // The genesis document: seats, head, endpoints.
    const std::optional<std::string> genesis_text{ReadFile(opt.genesis)};
    if (!genesis_text.has_value()) return Usage("cannot read " + fs::PathToString(opt.genesis));
    const Result<netio::GenesisDoc> doc{netio::GenesisDoc::Parse(*genesis_text)};
    if (!doc.has_value()) return Fail("genesis", doc.error());
    if (opt.seat >= doc->seats.size()) return Usage("--seat is outside the genesis seats");
    const Result<ProfileParams> profile{DevRegtestProfile(ChainType::REGTEST)};
    if (!profile.has_value()) return Fail("profile", profile.error());
    const Result<SeatSet> seats{doc->SeatSetOf()};
    if (!seats.has_value()) return Fail("seat set", seats.error());
    const Result<state::HeadView> genesis{doc->GenesisHead(*profile, *seats)};
    if (!genesis.has_value()) return Fail("genesis head", genesis.error());
    const AuthorityVersion authority{doc->Authority(*profile, *seats)};
    const std::optional<std::array<unsigned char, 32>> ikm{ReadKeyFile(opt.keys / fs::u8path("seat.key"))};
    const std::optional<std::array<unsigned char, 32>> transport{ReadKeyFile(opt.keys / fs::u8path("transport.key"))};
    if (!ikm.has_value() || !transport.has_value()) return Usage("cannot read seat.key and transport.key (32 bytes hex each) in " + fs::PathToString(opt.keys));

    // Journals: new on the first start, recovered afterwards.
    const fs::path signer_dir{opt.datadir / fs::u8path("signer")};
    const fs::path commit_dir{opt.datadir / fs::u8path("commit")};
    const bool restart{DirHasFiles(signer_dir) || DirHasFiles(commit_dir)};
    fs::create_directories(signer_dir, ec);
    fs::create_directories(commit_dir, ec);
    Result<std::unique_ptr<storage::FileJournal>> signer_journal{
        restart ? storage::FileJournal::OpenExisting(signer_dir, storage::JournalRole::SIGNER, 1)
                : storage::FileJournal::CreateNew(signer_dir, storage::JournalRole::SIGNER, 1, storage::DEFAULT_SEGMENT_BYTES)};
    if (!signer_journal.has_value()) return Fail("signer journal", signer_journal.error());
    Result<std::unique_ptr<storage::FileJournal>> commit_journal{
        restart ? storage::FileJournal::OpenExisting(commit_dir, storage::JournalRole::COMMIT, 2)
                : storage::FileJournal::CreateNew(commit_dir, storage::JournalRole::COMMIT, 2, storage::DEFAULT_SEGMENT_BYTES)};
    if (!commit_journal.has_value()) return Fail("commit journal", commit_journal.error());

    // The chain: the shared dev chain file replayed into the fake adapter.
    Result<std::unique_ptr<dev::FakeChainAdapter>> chain{dev::FakeChainAdapter::Create(doc->network, authority, *seats, doc->base)};
    if (!chain.has_value()) return Fail("chain adapter", chain.error());
    dev::DevChainTail tail{opt.devchain};
    const auto feed_chain{[&]() -> Result<size_t> {
        Result<std::vector<dev::DevChainBlock>> blocks{tail.Poll()};
        if (!blocks.has_value()) return blocks.error();
        for (const dev::DevChainBlock& block : *blocks) {
            if (const Result<Anchor> appended{(*chain)->AppendBlock(block.hash, block.facts)}; !appended.has_value()) return appended.error();
            LogInfo("fm2d devchain block %u with %u fact(s)", block.height, block.facts.size());
        }
        return Ok(blocks->size());
    }};
    if (const Result<size_t> fed{feed_chain()}; !fed.has_value()) return Fail("dev chain", fed.error());
    Result<std::unique_ptr<dev::UnlimitedDevBudget>> budget{dev::UnlimitedDevBudget::Create(*profile)};
    if (!budget.has_value()) return Fail("budget", budget.error());

    // The mesh.
    net::TcpMeshConfig mesh_config;
    mesh_config.mesh = net::MeshConfig{opt.seat, static_cast<uint32_t>(seats->Size()), net::MaxBulkFrameBytes(*profile), *profile};
    mesh_config.listen_address = opt.listen;
    mesh_config.listen_port = opt.mesh_port.value_or(doc->seats[opt.seat].mesh_port);
    mesh_config.seats = std::make_shared<const SeatSet>(*seats);
    mesh_config.domain = doc->Domain();
    mesh_config.magic = doc->mesh_magic;
    mesh_config.epoch = 0;
    mesh_config.transport_secret = *transport;
    mesh_config.endpoints = doc->Endpoints();
    Result<std::unique_ptr<net::TcpSeatMesh>> mesh{net::TcpSeatMesh::Create(mesh_config)};
    if (!mesh.has_value()) return Fail("mesh", mesh.error());

    // The seat.
    LogHooks hooks;
    seat::SeatNodeConfig config;
    config.profile = *profile;
    config.seat_count = static_cast<uint32_t>(seats->Size());
    config.self_seat = opt.seat;
    config.ikm.assign(ikm->begin(), ikm->end());
    config.max_replies = 256;
    config.pbft.idle_grace_ms = opt.heartbeat_ms;
    uint64_t stream_epoch{0};
    GetRandBytes(std::span<unsigned char>{reinterpret_cast<unsigned char*>(&stream_epoch), sizeof(stream_epoch)});
    config.stream_epoch = stream_epoch == 0 ? 1 : stream_epoch;
    seat::SeatNodeParts parts;
    parts.chain = chain->get();
    parts.signer_journal = signer_journal->get();
    parts.commit_journal = commit_journal->get();
    parts.budget = budget->get();
    parts.mesh = mesh->get();
    parts.hooks = &hooks;
    Result<std::unique_ptr<seat::SeatNode>> node{seat::SeatNode::Create(config, parts, *seats, *genesis, restart)};
    if (!node.has_value()) return Fail(restart ? "seat (restart)" : "seat", node.error());

    // The API.
    net::HttpsSseConfig api_config;
    api_config.profile = *profile;
    api_config.bind_address = opt.api_bind;
    api_config.port = opt.api_port.value_or(doc->seats[opt.seat].api_port);
    api_config.dev_self_signed = true;
    api_config.self_signed_dir = fs::PathToString(opt.datadir);
    // Regtest: one host runs every seat and the test drivers, so the public-API research limits (10 per IP, 30 new per
    // minute) would refuse the operator's own polling. The production defaults stay in HttpsSseConfig.
    api_config.max_connections_per_ip = 64;
    api_config.max_new_connections_per_minute = 6'000;
    api_config.max_connections_per_netgroup = 256;
    Result<std::unique_ptr<net::HttpsSseApi>> api{net::HttpsSseApi::Create(api_config, (*node)->Api())};
    if (!api.has_value()) return Fail("api", api.error());
    (*api)->AddStatic("/v2/genesis", "application/json", *genesis_text);

    {
        UniValue ready{UniValue::VOBJ};
        ready.pushKV("seat", UniValue{static_cast<uint64_t>(opt.seat)});
        ready.pushKV("mesh_port", UniValue{static_cast<uint64_t>((*mesh)->ListenPort())});
        ready.pushKV("api_port", UniValue{static_cast<uint64_t>((*api)->Port())});
        ready.pushKV("fingerprint", UniValue{(*api)->CertificateFingerprint()});
        ready.pushKV("pid", UniValue{static_cast<int64_t>(::getpid())});
        ready.pushKV("restart", UniValue{restart});
        ready.pushKV("head", UniValue{(*node)->Owner().Head()->Header().height});
        std::ofstream out{fs::PathToString(opt.datadir / fs::u8path("fm2d.ready"))};
        out << ready.write() << "\n";
    }
    LogInfo("fm2d seat %u ready: mesh %s:%u api %s:%u head %u%s", opt.seat, opt.listen, (*mesh)->ListenPort(), opt.api_bind, (*api)->Port(),
            (*node)->Owner().Head()->Header().height, restart ? " (restarted)" : "");

    // The sealing loop.
    using Clock = std::chrono::steady_clock;
    const Clock::time_point started{Clock::now()};
    const auto now_ms{[&] { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()) + 1; }};
    uint64_t last_decided_ms{now_ms()};
    constexpr uint64_t PROPOSE_RETRY_MS{50}; // a refused proposal is retried after this pause, not every loop
    uint64_t last_propose_refused_ms{0};
    std::string last_propose_reason;
    uint64_t last_chain_poll_ms{0};
    int exit_code{0};
    while (!interrupt) {
        const uint64_t now{now_ms()};
        if (now - last_chain_poll_ms >= 50) {
            last_chain_poll_ms = now;
            if (const Result<size_t> fed{feed_chain()}; !fed.has_value()) {
                LogError("fm2d dev chain: %s %s", std::string{ToString(fed.error().code)}, std::string{fed.error().detail});
            } else if (*fed > 0) {
                (*node)->OnLocalWork();
            }
        }
        for (size_t i{0}; i < 256; ++i) {
            Result<std::optional<net::InboundFrame>> frame{(*mesh)->Poll()};
            if (!frame.has_value() || !frame->has_value()) break;
            (void)(*node)->OnFrame(**frame);
        }
        (void)(*node)->Tick(now);
        if (const Result<std::optional<seat::SealReport>> completed{(*node)->Complete()}; completed.has_value() && completed->has_value()) {
            last_decided_ms = now;
            (*api)->Notify();
        }
        if ((*node)->IsLeader()) {
            const std::optional<agreement::PbftStatus> status{(*node)->AgreementStatus()};
            if (status.has_value() && !status->proposal_accepted && !status->changing && !status->decided && !status->halted) {
                const bool pending_actions{(*node)->Pool().Size() > 0};
                const bool pending_facts{(*chain)->End() > (*node)->Owner().Head()->Header().facts_to};
                const bool heartbeat_due{now - last_decided_ms >= opt.heartbeat_ms};
                if ((pending_actions || pending_facts || heartbeat_due) && now - last_propose_refused_ms >= PROPOSE_RETRY_MS) {
                    const Result<uint64_t> proposed{(*node)->ProposeSlot()};
                    if (!proposed.has_value()) {
                        // A refusal is retried after a pause, and logged once per distinct reason (BUSY and NEED_DATA are routine waits).
                        last_propose_refused_ms = now;
                        const std::string reason{std::string{ToString(proposed.error().code)} + " " + std::string{proposed.error().detail}};
                        if (reason != last_propose_reason) {
                            last_propose_reason = reason;
                            if (proposed.error().code == ErrorCode::BUSY || proposed.error().code == ErrorCode::NEED_DATA) {
                                LogInfo("fm2d propose refused: %s", reason);
                            } else {
                                LogWarning("fm2d propose refused: %s", reason);
                            }
                        }
                    } else {
                        last_propose_reason.clear();
                    }
                }
            }
        }
        if (const Result<net::PumpReport> pumped{(*node)->Pump(64)}; pumped.has_value() && pumped->answered > 0) {
            (*node)->OnLocalWork();
            (*api)->Notify();
        }
        if ((*node)->Fenced()) {
            LogError("fm2d: the node is fenced; it serves reads only");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    LogInfo("fm2d seat %u stopping at head %u", opt.seat, (*node)->Owner().Head()->Header().height);
    (*api)->Stop();
    (*mesh)->Stop();
    fs::remove(opt.datadir / fs::u8path("fm2d.ready"), ec);
    return exit_code;
}
