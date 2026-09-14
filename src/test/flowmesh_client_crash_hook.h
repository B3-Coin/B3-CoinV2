// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_TEST_FLOWMESH_CLIENT_CRASH_HOOK_H
#define BITCOIN_TEST_FLOWMESH_CLIENT_CRASH_HOOK_H

// This file is included only by a separately compiled test daemon. No option,
// environment processing or extra file access exists in ordinary builds.
#ifndef FLOWMESH_CLIENT_CRASH_TEST_HOOKS
#error "FlowMesh client crash hooks require an explicitly separate test build"
#endif

#include <chainparams.h>
#include <univalue.h>
#include <util/fs.h>
#include <util/strencodings.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace node::test {

// Markers and captured records contain only public instruction/journal data.
// The fixture must create the marker in its NEW client datadir before launch.
// Both exact path and unique token must agree; a marker alone is insufficient.
inline UniValue ClientCrashGuard(const fs::path& journal)
{
    const char* expected_path{std::getenv("B3_TEST_CLIENT_CRASH_CHAIN_DIR")};
    const char* token{std::getenv("B3_TEST_CLIENT_CRASH_TOKEN")};
    if (!expected_path && !token) return {};
    if (!expected_path || !token || std::string{token}.size() != 64 || !IsHex(token) ||
        Params().GetChainType() != ChainType::REGTEST)
        throw std::runtime_error("Invalid isolated client crash-test guard");
    const fs::path chain_dir{fs::canonical(fs::u8path(expected_path))};
    if (chain_dir.filename() != "regtest" || fs::weakly_canonical(journal) != chain_dir / "flowmesh_client")
        throw std::runtime_error("Crash-test guard does not match exact regtest client datadir");
    const fs::path marker{chain_dir.parent_path() / "flowmesh-client-crash-guard.json"};
    if (fs::file_size(marker) > 4096) throw std::runtime_error("Crash-test marker exceeds bound");
    std::ifstream input{marker.std_path()};
    const std::string contents{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    UniValue value;
    if (!input || !value.read(contents) || !value.isObject() ||
        !value["token"].isStr() || value["token"].get_str() != token ||
        !value["chain_dir"].isStr() || value["chain_dir"].get_str() != chain_dir.utf8string() ||
        !value["fresh_generated_client"].isBool() || !value["fresh_generated_client"].get_bool() ||
        !value["case"].isStr() || !value["pause_before_first_send"].isBool())
        throw std::runtime_error("Crash-test marker identity mismatch");
    return value;
}

inline void ClientCrashRecord(const fs::path& journal, const char* stage, const UniValue& data)
{
    const auto guard{ClientCrashGuard(journal)};
    if (guard.isNull()) return;
    const fs::path output{journal.parent_path().parent_path() / "flowmesh-client-crash-events.jsonl"};
    UniValue event{UniValue::VOBJ};
    event.pushKV("stage", stage); event.pushKV("pid", static_cast<int64_t>(::getpid()));
    event.pushKV("token", guard["token"]); event.pushKV("case", guard["case"]);
    event.pushKV("monotonic_us", std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    event.pushKV("data", data);
    const std::string encoded{event.write() + "\n"};
    constexpr size_t MAX_CAPTURE_BYTES{16 * 1024 * 1024};
    if (encoded.size() > 1024 * 1024 || (fs::exists(output) && fs::file_size(output) + encoded.size() > MAX_CAPTURE_BYTES))
        throw std::runtime_error("Crash-test public capture bound reached");
    std::ofstream stream{output.std_path(), std::ios::app | std::ios::binary};
    stream << encoded; stream.flush();
    if (!stream) throw std::runtime_error("Crash-test public capture failed");
}

inline void ClientCrashBeforeFirstSend(const fs::path& journal, const uint256& action_id, bool deposit)
{
    const auto guard{ClientCrashGuard(journal)};
    if (guard.isNull() || deposit || !guard["pause_before_first_send"].get_bool()) return;
    UniValue data{UniValue::VOBJ}; data.pushKV("action_id", action_id.GetHex());
    ClientCrashRecord(journal, "durable_new_instruction_before_first_send", data);
    // Parent captures the stage and kills exactly this owned client process.
    // A missing parent never allows a delayed accidental first send: timeout
    // throws, retaining the normal already-durable instruction as unknown.
    std::this_thread::sleep_for(std::chrono::seconds{60});
    throw std::runtime_error("Crash-test before-first-send hold expired; instruction remains retained");
}

} // namespace node::test
#endif // BITCOIN_TEST_FLOWMESH_CLIENT_CRASH_HOOK_H
