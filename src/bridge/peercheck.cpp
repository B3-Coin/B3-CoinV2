// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Read-only native consumer for untrusted full proofs emitted by an Ethereum
// peer backend. It opens no RPC/network connection, holds no signing key, and
// cannot activate B3/FlowMesh/Arbitrum custody. A finalized diagnostic JSON
// response is not a native mint-authority object.
#include <bridge/lc_json.h>
#include <bridge/peer_light_client.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace bridge;
namespace {
constexpr size_t MAX_JSON_BYTES{262'144};

void Object(const UniValue& value, std::initializer_list<std::string_view> required,
            std::initializer_list<std::string_view> optional = {})
{
    if (!value.isObject()) throw std::runtime_error("INVALID_JSON_SHAPE");
    std::set<std::string> seen;
    for (const auto& key : value.getKeys()) {
        if (!seen.insert(key).second ||
            (std::find(required.begin(), required.end(), key) == required.end() &&
             std::find(optional.begin(), optional.end(), key) == optional.end())) throw std::runtime_error("INVALID_JSON_FIELDS");
    }
    for (const auto& key : required) if (!seen.contains(std::string{key})) throw std::runtime_error("MISSING_JSON_FIELD");
}

void NoDuplicates(const UniValue& value, size_t depth = 0)
{
    if (depth > 16) throw std::runtime_error("JSON_DEPTH_LIMIT");
    if (value.isObject()) {
        std::set<std::string> keys;
        for (const auto& key : value.getKeys()) if (!keys.insert(key).second) throw std::runtime_error("DUPLICATE_JSON_FIELD");
    }
    if (value.isObject() || value.isArray()) {
        if (value.size() > 2048) throw std::runtime_error("JSON_ARRAY_LIMIT");
        for (size_t i{0}; i < value.size(); ++i) NoDuplicates(value[i], depth + 1);
    }
}

UniValue Json(std::string_view bytes)
{
    if (bytes.empty() || bytes.size() > MAX_JSON_BYTES) throw std::runtime_error("JSON_SIZE_LIMIT");
    // Bound nesting before the general parser allocates its object tree.
    bool quoted{false}, escaped{false};
    int depth{0};
    for (const char c : bytes) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '[' || c == '{') { if (++depth > 16) throw std::runtime_error("JSON_DEPTH_LIMIT"); }
        else if (c == ']' || c == '}') { if (--depth < 0) throw std::runtime_error("INVALID_JSON"); }
    }
    UniValue value;
    if (!value.read(bytes)) throw std::runtime_error("INVALID_JSON");
    NoDuplicates(value);
    return value;
}

void HeaderShape(const UniValue& value)
{
    Object(value, {"beacon", "execution", "execution_branch"});
    Object(lcjson::Field(value, "beacon"), {"slot", "proposer_index", "parent_root", "state_root", "body_root"});
    Object(lcjson::Field(value, "execution"),
           {"parent_hash", "fee_recipient", "state_root", "receipts_root", "logs_bloom", "prev_randao",
            "block_number", "gas_limit", "gas_used", "timestamp", "extra_data", "base_fee_per_gas",
            "block_hash", "transactions_root", "withdrawals_root", "blob_gas_used", "excess_blob_gas"});
}

void CommitteeShape(const UniValue& value) { Object(value, {"pubkeys", "aggregate_pubkey"}); }

peer::Policy ParsePolicy(const UniValue& value)
{
    Object(value, {"format", "mainnet_enabled", "chain_id", "genesis_validators_root", "forks",
                   "electra_epoch", "min_participants", "trusted_root", "trusted_slot",
                   "fork_schedule_valid_through_epoch", "max_sync_lag_slots", "max_recovery_lag_slots", "genesis_time", "seconds_per_slot"});
    if (value["format"].get_str() != "b3-ethereum-peer-policy/1" ||
        !value["mainnet_enabled"].isBool() || value["mainnet_enabled"].get_bool() ||
        !value["chain_id"].isNum() || value["chain_id"].getInt<uint64_t>() != 1) throw std::runtime_error("DISABLED_ETHEREUM_POLICY_REQUIRED");
    if (!value["forks"].isArray() || value["forks"].empty() || value["forks"].size() > 32 ||
        lcjson::Num(value["min_participants"], "min_participants") < 342 ||
        lcjson::Num(value["min_participants"], "min_participants") > 512) throw std::runtime_error("INVALID_POLICY");
    for (size_t i{0}; i < value["forks"].size(); ++i) Object(value["forks"][i], {"epoch", "version"});
    peer::Policy policy;
    policy.config = lcjson::ParseConfig(value);
    policy.trusted_root = lcjson::Root(value["trusted_root"], "trusted_root");
    policy.trusted_slot = lcjson::Num(value["trusted_slot"], "trusted_slot");
    policy.fork_schedule_valid_through_epoch = lcjson::Num(value["fork_schedule_valid_through_epoch"], "fork_schedule_valid_through_epoch");
    policy.max_sync_lag_slots = lcjson::Num(value["max_sync_lag_slots"], "max_sync_lag_slots");
    policy.max_recovery_lag_slots = lcjson::Num(value["max_recovery_lag_slots"], "max_recovery_lag_slots");
    policy.genesis_time = lcjson::Num(value["genesis_time"], "genesis_time");
    policy.seconds_per_slot = lcjson::Num(value["seconds_per_slot"], "seconds_per_slot");
    if (!policy.Valid()) throw std::runtime_error("INVALID_POLICY");
    return policy;
}

std::vector<unsigned char> ParseProof(const UniValue& value)
{
    Object(value, {"kind", "data"});
    const auto kind{value["kind"].get_str()};
    const auto& data{value["data"]};
    BridgeRecordV1 record;
    if (kind == "bootstrap") {
        Object(data, {"header", "current_sync_committee", "current_sync_committee_branch"});
        HeaderShape(data["header"]);
        CommitteeShape(data["current_sync_committee"]);
        record = {BridgeRecordKindV1::BOOTSTRAP,
                  BridgeBootstrapV1{lcjson::ParseHeader(data["header"]), lcjson::ParseCommittee(data["current_sync_committee"]),
                                    lcjson::Branch(data["current_sync_committee_branch"], "current_sync_committee_branch")}};
    } else if (kind == "update" || kind == "finality_update") {
        Object(data, {"attested_header", "finalized_header", "finality_branch", "sync_aggregate", "signature_slot"},
               {"next_sync_committee", "next_sync_committee_branch"});
        const bool next{!data.find_value("next_sync_committee").isNull()};
        if (next != !data.find_value("next_sync_committee_branch").isNull() || (kind == "finality_update" && next)) throw std::runtime_error("INVALID_NEXT_COMMITTEE");
        HeaderShape(data["attested_header"]);
        HeaderShape(data["finalized_header"]);
        Object(data["sync_aggregate"], {"sync_committee_bits", "sync_committee_signature"});
        if (next) CommitteeShape(data["next_sync_committee"]);
        record = {BridgeRecordKindV1::UPDATE, BridgeUpdateV1{lcjson::ParseUpdate(data)}};
    } else throw std::runtime_error("UNSUPPORTED_PROOF_KIND");
    const auto encoded{EncodeBridgeRecordV1(record)};
    if (!encoded) throw std::runtime_error("CANONICAL_RECORD_LIMIT");
    return *encoded;
}

uint64_t CurrentSlot(const peer::Policy& policy)
{
    const auto now{std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()};
    if (now < 0 || static_cast<uint64_t>(now) < policy.genesis_time) throw std::runtime_error("LOCAL_CLOCK_BEFORE_GENESIS");
    return (static_cast<uint64_t>(now) - policy.genesis_time) / policy.seconds_per_slot;
}

const char* Name(peer::Result result)
{
    switch (result) {
    case peer::Result::OK: return "OK";
    case peer::Result::NO_CHANGE: return "NO_CHANGE";
    case peer::Result::BAD_POLICY: return "BAD_POLICY";
    case peer::Result::BAD_RECORD: return "BAD_RECORD";
    case peer::Result::UNKNOWN_FORK: return "UNKNOWN_FORK";
    case peer::Result::FUTURE_SLOT: return "FUTURE_SLOT";
    case peer::Result::STALE_AUTHORITY: return "STALE_AUTHORITY";
    case peer::Result::BOOTSTRAP_REQUIRED: return "BOOTSTRAP_REQUIRED";
    case peer::Result::BAD_PROOF: return "BAD_PROOF";
    case peer::Result::BAD_JOURNAL: return "BAD_JOURNAL";
    case peer::Result::LIMIT: return "LIMIT";
    case peer::Result::STORAGE_FENCED: return "STORAGE_FENCED";
    }
    return "UNKNOWN";
}

void Emit(const peer::LightClient& client, uint64_t current_slot, const char* result,
          bool accepted, std::optional<LcResult> proof = {})
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("format", "b3-ethereum-peer-verification/1");
    out.pushKV("result", result);
    out.pushKV("accepted", accepted);
    out.pushKV("mainnet_enabled", false);
    out.pushKV("arbitrum_authority", false);
    out.pushKV("writes_enabled", false);
    out.pushKV("sequence", client.Sequence());
    out.pushKV("policy_sha256", HexStr(client.PolicyDigest()));
    out.pushKV("journal_sha256", HexStr(client.JournalDigest()));
    out.pushKV("fenced", client.Fenced());
    out.pushKV("trust", "operator-reviewed-weak-subjectivity-checkpoint; native-BLS-SSZ; local-clock-freshness");
    if (proof) out.pushKV("proof_result", static_cast<unsigned>(*proof));
    const auto anchor{client.Latest(current_slot)};
    out.pushKV("fresh_finalized_execution", anchor.has_value());
    if (anchor) {
        UniValue header{UniValue::VOBJ};
        header.pushKV("beacon_slot", anchor->BeaconSlot());
        header.pushKV("block_number", anchor->Header().block_number);
        header.pushKV("block_hash", lcjson::EthereumHex(anchor->Header().block_hash));
        header.pushKV("state_root", lcjson::EthereumHex(anchor->Header().state_root));
        header.pushKV("receipts_root", lcjson::EthereumHex(anchor->Header().receipts_root));
        out.pushKV("ethereum_finalized_execution", std::move(header));
    }
    std::cout << out.write() << '\n' << std::flush;
}

#ifndef WIN32
/** Linux/macOS owned proof journal. No symlink component, hardlink alias,
 * unsafe writable directory, shared file, or parallel writer is accepted.
 * Root-owned sticky temporary-directory ancestors are permitted; the immediate
 * journal directory must belong to the caller and forbid other writers.
 * The entire proof stream is replayed; corrupt/truncated tails are not pruned.
 */
class File final {
public:
    File(const std::string& path, bool journal)
    {
        const std::filesystem::path target{path};
        if (!target.is_absolute() || target.lexically_normal() != target ||
            target.filename().empty()) throw std::runtime_error("ABSOLUTE_NORMAL_PATH_REQUIRED");
        int directory{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
        if (directory < 0) throw std::runtime_error("DIRECTORY_OPEN_FAILED");
        const auto safe_ancestor{[](int fd) {
            struct stat info{};
            if (::fstat(fd, &info) || (info.st_uid != ::getuid() && info.st_uid != 0)) return false;
            return !(info.st_mode & 0022) || (info.st_uid == 0 && (info.st_mode & S_ISVTX));
        }};
        if (!safe_ancestor(directory)) { ::close(directory); throw std::runtime_error("UNSAFE_PATH_COMPONENT"); }
        const auto parent{target.parent_path()};
        for (const auto& part : parent.relative_path()) {
            const int next{::openat(directory, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
            ::close(directory);
            if (next < 0) throw std::runtime_error("UNSAFE_PATH_COMPONENT");
            if (!safe_ancestor(next)) { ::close(next); throw std::runtime_error("UNSAFE_PATH_COMPONENT"); }
            directory = next;
        }
        struct stat parent_stat{};
        if (::fstat(directory, &parent_stat) ||
            (parent_stat.st_uid != ::getuid() && parent_stat.st_uid != 0) ||
            (parent_stat.st_mode & 0022) || (journal && parent_stat.st_uid != ::getuid())) {
            ::close(directory);
            throw std::runtime_error("UNSAFE_JOURNAL_DIRECTORY");
        }
        bool created{false};
        if (journal) {
            m_fd = ::openat(directory, target.filename().c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_CREAT | O_EXCL, 0600);
            if (m_fd >= 0) created = true;
            else if (errno == EEXIST) m_fd = ::openat(directory, target.filename().c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
        } else m_fd = ::openat(directory, target.filename().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (m_fd < 0) { ::close(directory); throw std::runtime_error("FILE_OPEN_FAILED"); }
        struct stat info{};
        const bool unsafe{::fstat(m_fd, &info) || !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
                          (info.st_uid != ::getuid() && info.st_uid != 0) || (info.st_mode & 0022) ||
                          (journal && (info.st_uid != ::getuid() || (info.st_mode & 0077)))};
        if (unsafe || (journal && ::flock(m_fd, LOCK_EX | LOCK_NB))) {
            ::close(directory);
            ::close(m_fd); m_fd = -1;
            throw std::runtime_error("UNSAFE_OR_LOCKED_FILE");
        }
        if (journal && ((created && (::fsync(m_fd) || ::fsync(directory))) ||
                        (!created && info.st_size == 0))) {
            ::close(directory);
            ::close(m_fd); m_fd = -1;
            throw std::runtime_error("EMPTY_OR_UNCOMMITTED_JOURNAL");
        }
        ::close(directory);
    }
    ~File() { if (m_fd >= 0) ::close(m_fd); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    bool ReadExact(std::span<unsigned char> output, bool eof_allowed = false)
    {
        size_t done{0};
        while (done < output.size()) {
            const auto got{::read(m_fd, output.data() + done, output.size() - done)};
            if (got < 0 && errno == EINTR) continue;
            if (got == 0 && !done && eof_allowed) return false;
            if (got <= 0) throw std::runtime_error("TRUNCATED_OR_UNREADABLE_FILE");
            done += static_cast<size_t>(got);
        }
        return true;
    }
    std::string ReadJson()
    {
        struct stat info{};
        if (::fstat(m_fd, &info) || info.st_size <= 0 || info.st_size > static_cast<off_t>(MAX_JSON_BYTES)) throw std::runtime_error("JSON_SIZE_LIMIT");
        std::string text(static_cast<size_t>(info.st_size), '\0');
        ReadExact(std::span<unsigned char>{reinterpret_cast<unsigned char*>(text.data()), text.size()});
        std::array<unsigned char, 1> extra{};
        if (ReadExact(extra, true)) throw std::runtime_error("POLICY_CHANGED_DURING_READ");
        return text;
    }
    bool Append(std::span<const unsigned char> entry)
    {
        size_t done{0};
        while (done < entry.size()) {
            const auto wrote{::write(m_fd, entry.data() + done, entry.size() - done)};
            if (wrote < 0 && errno == EINTR) continue;
            if (wrote <= 0) return false;
            done += static_cast<size_t>(wrote);
        }
        return ::fsync(m_fd) == 0;
    }
private:
    int m_fd{-1};
};

void Replay(File& journal, peer::LightClient& client)
{
    for (;;) {
        std::array<unsigned char, peer::ENTRY_PREFIX_SIZE> prefix{};
        if (!journal.ReadExact(prefix, true)) break;
        const size_t n{prefix.size()};
        const uint32_t size{(uint32_t{prefix[n - 4]} << 24) | (uint32_t{prefix[n - 3]} << 16) |
                             (uint32_t{prefix[n - 2]} << 8) | uint32_t{prefix[n - 1]}};
        if (size < 2 || size > MAX_BRIDGE_RECORD_SIZE) throw std::runtime_error("INVALID_JOURNAL_LENGTH");
        std::vector<unsigned char> entry{prefix.begin(), prefix.end()};
        entry.resize(prefix.size() + size + 32);
        journal.ReadExact(std::span<unsigned char>{entry}.subspan(prefix.size()));
        if (client.ReplayEntry(entry).result != peer::Result::OK) throw std::runtime_error("JOURNAL_VERIFICATION_FAILED");
    }
    if (!client.EndReplay()) throw std::runtime_error("JOURNAL_FENCED");
}
#endif
} // namespace

int main(int argc, char** argv)
{
#ifdef WIN32
    std::fprintf(stderr, "UNSUPPORTED_DURABLE_PLATFORM: run the Linux verifier under WSL\n");
    return 2;
#else
    if (argc != 5 || std::string_view{argv[1]} != "--policy" || std::string_view{argv[3]} != "--journal") {
        std::fprintf(stderr, "usage: b3-bridge-peercheck --policy /absolute/policy.json --journal /absolute/checkpoint.journal\n");
        return 2;
    }
    try {
        File policy_file{argv[2], false};
        const auto policy{ParsePolicy(Json(policy_file.ReadJson()))};
        peer::LightClient client{policy};
        File journal{argv[4], true};
        Replay(journal, client);
        Emit(client, CurrentSlot(policy), "READY", false);
        // Bounded NDJSON reader; std::getline would allocate an unbounded line
        // before discovering the size limit.
        for (;;) {
            std::string line;
            line.reserve(64'000);
            bool overflow{false};
            char c{};
            while (std::cin.get(c) && c != '\n') {
                if (line.size() < MAX_JSON_BYTES) line.push_back(c);
                else overflow = true;
            }
            if (line.empty() && !overflow && std::cin.eof()) break;
            if (!std::cin.eof() && std::cin.fail()) throw std::runtime_error("INPUT_READ_FAILED");
            if (overflow) { Emit(client, CurrentSlot(policy), "JSON_SIZE_LIMIT", false); continue; }
            try {
                const auto record{ParseProof(Json(line))};
                const auto reply{client.Submit(record, CurrentSlot(policy), [&](auto bytes) { return journal.Append(bytes); })};
                Emit(client, CurrentSlot(policy), Name(reply.result), reply.result == peer::Result::OK, reply.proof);
                if (client.Fenced()) return 1;
            } catch (const std::exception&) {
                // Do not leak raw peer text, filenames, or parser diagnostics.
                Emit(client, CurrentSlot(policy), "MALFORMED_PEER_PROOF", false);
            }
            if (std::cin.eof()) break;
        }
        // The final diagnostic uses a fresh local clock reading. A launcher
        // must inspect this last status, not an earlier once-fresh response.
        Emit(client, CurrentSlot(policy), "READY", false);
        return client.Fenced() ? 1 : 0;
    } catch (const std::exception& error) {
        // All startup failures are bounded local diagnostics, never raw JSON.
        std::fprintf(stderr, "peer light-client startup refused: %s\n", error.what());
        return 1;
    }
#endif
}
