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

/** Inject only BEFORE the actual journal write, for one generated fixture
 * action whose new no-resubmit marker is about to become durable. This is a
 * pre-write failure test, not an uncertain-write or power-loss simulation. */
inline void ClientCrashBeforeJournalSave(const fs::path& journal, const UniValue& root)
{
    const auto guard{ClientCrashGuard(journal)};
    if (guard.isNull() || !guard.exists("fail_new_inclusion_save_for")) return;
    if (!guard["fail_new_inclusion_save_for"].isStr() ||
        guard["fail_new_inclusion_save_for"].get_str().size() != 64 ||
        !IsHex(guard["fail_new_inclusion_save_for"].get_str()) || !root["actions"].isArray())
        throw std::runtime_error("Invalid isolated inclusion-save fault target");
    const auto target{guard["fail_new_inclusion_save_for"].get_str()};
    for (const auto& row : root["actions"].getValues()) {
        if (row["action_id"].isStr() && row["action_id"].get_str() == target &&
            row["previously_certified"].isTrue()) {
            ClientCrashRecord(journal, "new_inclusion_save_failed_before_write", root);
            throw std::runtime_error("Injected isolated new-inclusion journal save failure");
        }
    }
}

/** A deliberately COMMITTED write, followed by a fixture throw or hold before
 * Save updates its successful digest or the snapshot publishes its cache.
 * This is not a simulation of an unknown physical write or power loss. */
inline void ClientCrashAfterJournalSyncBeforeDigest(const fs::path& journal, const UniValue& root)
{
    auto guard{ClientCrashGuard(journal)};
    if (guard.isNull() || !guard.exists("after_sync_fault_action_id")) return;
    if (!guard["after_sync_fault_action_id"].isStr() ||
        guard["after_sync_fault_action_id"].get_str().size() != 64 || !IsHex(guard["after_sync_fault_action_id"].get_str()) ||
        !guard["after_sync_fault_market_id"].isStr() ||
        guard["after_sync_fault_market_id"].get_str().size() != 64 || !IsHex(guard["after_sync_fault_market_id"].get_str()) ||
        !guard["after_sync_fault_mode"].isStr() || !root["actions"].isArray())
        throw std::runtime_error("Invalid isolated post-sync fault target");
    const auto action{guard["after_sync_fault_action_id"].get_str()};
    const auto market{guard["after_sync_fault_market_id"].get_str()};
    const auto mode{guard["after_sync_fault_mode"].get_str()};
    if (mode != "throw" && mode != "hold") throw std::runtime_error("Invalid isolated post-sync fault mode");
    bool matched{false};
    for (const auto& row : root["actions"].getValues()) {
        if (row["action_id"].isStr() && row["action_id"].get_str() == action &&
            row["market_id"].isStr() && row["market_id"].get_str() == market && row["previously_certified"].isTrue())
            matched = true;
    }
    if (!matched) return;
    if (mode == "throw") {
        ClientCrashRecord(journal, "committed_journal_test_throw_before_digest", root);
        throw std::runtime_error("Injected test throw AFTER committed synchronous journal write, BEFORE digest/publication");
    }
    ClientCrashRecord(journal, "committed_journal_before_digest_held", root);
    const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{30}};
    while (std::chrono::steady_clock::now() < deadline) {
        guard = ClientCrashGuard(journal);
        if (guard.isNull()) throw std::runtime_error("Isolated post-sync hold lost its fixture guard");
        if (guard.exists("release_after_sync_before_digest")) {
            if (!guard["release_after_sync_before_digest"].isBool())
                throw std::runtime_error("Invalid isolated post-sync release");
            if (guard["release_after_sync_before_digest"].get_bool()) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    throw std::runtime_error("Isolated post-sync hold expired; committed journal preserved without publication");
}

/** Hold a completed lane reply BEFORE it reacquires m_work. The parent may
 * perform an ordinary snapshot meanwhile, then release this exact reply.
 * A missing parent ends by refusal, never by a resend or fresh signature. */
inline void ClientCrashBeforeLaneApply(const fs::path& journal, const uint256& action_id)
{
    auto guard{ClientCrashGuard(journal)};
    if (guard.isNull() || !guard.exists("hold_lane_before_apply")) return;
    if (!guard["hold_lane_before_apply"].isBool())
        throw std::runtime_error("Invalid isolated lane hold");
    if (!guard["hold_lane_before_apply"].get_bool()) return;
    UniValue data{UniValue::VOBJ};
    data.pushKV("action_id", action_id.GetHex());
    ClientCrashRecord(journal, "lane_reply_before_apply_held", data);
    const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{30}};
    while (std::chrono::steady_clock::now() < deadline) {
        guard = ClientCrashGuard(journal);
        if (guard.isNull()) throw std::runtime_error("Isolated lane hold lost its fixture guard");
        if (guard.exists("release_lane_before_apply")) {
            if (!guard["release_lane_before_apply"].isBool())
                throw std::runtime_error("Invalid isolated lane release");
            if (guard["release_lane_before_apply"].get_bool()) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    throw std::runtime_error("Isolated lane hold expired; original instruction remains retained");
}

} // namespace node::test
#endif // BITCOIN_TEST_FLOWMESH_CLIENT_CRASH_HOOK_H
