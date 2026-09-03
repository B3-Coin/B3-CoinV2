// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// b3-bridge-ethcheck: relayer dry-run harness for the ETH -> B3 deposit leg.
//
// Consumes files fetched from a beacon node / execution RPC by
// contrib/b3bridge/eth_live_test.py and performs ALL verification in the
// same C++ headers a future consensus wiring would use:
//
//   <dir>/config.json           chain constants + trusted_root (checkpoint)
//   <dir>/store.json            exact B3-finalized LightClientStore (optional)
//   <dir>/bootstrap.json        raw /eth/v1/beacon/light_client/bootstrap/...
//   <dir>/updates.json          raw /eth/v1/beacon/light_client/updates?...
//   <dir>/finality_update.json  raw /eth/v1/beacon/light_client/finality_update (optional)
//   <dir>/receipt_proof.json    {"index","key","value","proof":[...]} (optional)
//
// With --emit-payloads, verification progress goes to stderr and stdout is one
// JSON object containing canonical payload hex produced by
// EncodeBridgeRecordV1. Long execution ancestry is split into dependent
// consensus-sized backfills followed by one mint. Exit code 0 = every step
// verified; nonzero otherwise. NOT part of the node; test/tooling only (option
// B3_BRIDGE_TOOLS).

#include <bridge/admission.h>
#include <bridge/deposit.h>
#include <bridge/exec_chain.h>
#include <bridge/eth_light_client.h>
#include <bridge/lc_json.h>
#include <bridge/mpt.h>
#include <bridge/proof.h>
#include <bridge/relayer_plan.h>
#include <uint256.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace bridge;

namespace {

bool g_emit_payloads{false};

void Info(const char* format, ...)
{
    std::va_list args;
    va_start(args, format);
    std::vfprintf(g_emit_payloads ? stderr : stdout, format, args);
    va_end(args);
}

UniValue LoadJson(const std::string& path)
{
    std::ifstream f{path};
    if (!f) throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    UniValue v;
    if (!v.read(ss.str())) throw std::runtime_error("invalid JSON in " + path);
    return v;
}

const char* Name(LcResult r)
{
    switch (r) {
    case LcResult::OK: return "OK";
    case LcResult::BAD_STRUCTURE: return "BAD_STRUCTURE";
    case LcResult::BOOTSTRAP_PROOF: return "BOOTSTRAP_PROOF";
    case LcResult::MONOTONICITY: return "MONOTONICITY";
    case LcResult::FINALITY_PROOF: return "FINALITY_PROOF";
    case LcResult::EXECUTION_PROOF: return "EXECUTION_PROOF";
    case LcResult::NEXT_PROOF: return "NEXT_PROOF";
    case LcResult::PERIOD: return "PERIOD";
    case LcResult::PARTICIPATION: return "PARTICIPATION";
    case LcResult::SIGNATURE: return "SIGNATURE";
    }
    return "?";
}

void Require(LcResult r, const std::string& what)
{
    if (r != LcResult::OK) {
        throw std::runtime_error(what + " failed: " + Name(r));
    }
    Info("  [ok] %s\n", what.c_str());
}

std::string PayloadHex(const BridgeRecordV1& record)
{
    const auto encoded{EncodeBridgeRecordV1(record)};
    if (!encoded) {
        throw std::runtime_error("record exceeds the canonical type-10 limits");
    }
    return HexStr(*encoded);
}

std::string EthereumHex(const uint256& value)
{
    return "0x" + HexStr(value);
}

UniValue RecordJson(const char* kind, const BridgeRecordV1& record)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("kind", kind);
    out.pushKV("payload_hex", PayloadHex(record));
    return out;
}

void AddStoreMetadata(UniValue& out, const LightClientStore& store)
{
    out.pushKV("store_period", store.period);
    out.pushKV("current_sync_committee_root",
               EthereumHex(store.current.HashTreeRoot()));
    if (store.next) {
        out.pushKV("next_sync_committee_root",
                   EthereumHex(store.next->HashTreeRoot()));
    }
    out.pushKV("finalized_beacon_slot",
               store.finalized_header.beacon.slot);
    out.pushKV("finalized_beacon_root",
               EthereumHex(store.finalized_header.beacon.HashTreeRoot()));
    out.pushKV("anchor_block_number",
               store.finalized_header.execution.block_number);
    out.pushKV("anchor_hash",
               EthereumHex(store.finalized_header.execution.block_hash));
    out.pushKV("has_next_committee", store.next.has_value());
}

uint64_t RawAmount64(const std::array<unsigned char, 32>& amount)
{
    for (size_t i{0}; i < 24; ++i) {
        if (amount[i] != 0) {
            throw std::runtime_error("deposit amount does not fit uint64");
        }
    }
    uint64_t out{0};
    for (size_t i{24}; i < amount.size(); ++i) {
        out = (out << 8) | amount[i];
    }
    if (out == 0 || out > uint64_t{std::numeric_limits<int64_t>::max()}) {
        throw std::runtime_error("deposit amount is zero or exceeds B3 RPC range");
    }
    return out;
}

CAmount ClaimAmount(const DepositEvent& deposit, const UniValue& proof)
{
    const uint64_t origin_decimals{lcjson::Num(
        lcjson::Field(proof, "origin_decimals"), "origin_decimals")};
    const uint64_t asset_decimals{lcjson::Num(
        lcjson::Field(proof, "asset_decimals"), "asset_decimals")};
    if (origin_decimals > std::numeric_limits<uint8_t>::max() ||
        asset_decimals > std::numeric_limits<uint8_t>::max()) {
        throw std::runtime_error("bridge decimals exceed uint8");
    }
    const auto converted{ConvertRawUnitsExact(
        deposit.amount, static_cast<uint8_t>(origin_decimals),
        static_cast<uint8_t>(asset_decimals))};
    if (!converted) {
        throw std::runtime_error(
            "deposit amount cannot be converted exactly to B3 asset units");
    }
    return *converted;
}

uint256 B3DisplayHash(const UniValue& value, const std::string& name)
{
    if (!value.isStr()) throw std::runtime_error(name + " is not a string");
    std::string text{value.get_str()};
    if (text.rfind("0x", 0) == 0) text.erase(0, 2);
    const auto parsed{uint256::FromHex(text)};
    if (!parsed || parsed->IsNull()) {
        throw std::runtime_error("invalid or null B3 display hash: " + name);
    }
    return *parsed;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string{argv[2]} == "--emit-payloads") {
        g_emit_payloads = true;
    } else if (argc != 2) {
        std::fprintf(
            stderr,
            "usage: b3-bridge-ethcheck <dir> [--emit-payloads]\n");
        return 2;
    }
    const std::string dir{std::string{argv[1]} + "/"};
    try {
        UniValue emitted{UniValue::VOBJ};
        UniValue emitted_updates{UniValue::VARR};
        UniValue emitted_backfills{UniValue::VARR};
        std::set<uint256> retained_anchors;
        const UniValue cfg_json{LoadJson(dir + "config.json")};
        const LightClientConfig cfg{lcjson::ParseConfig(cfg_json)};
        Info("b3-bridge-ethcheck: %zu forks, electra_epoch %llu, min_participants %u\n",
             cfg.forks.size(), (unsigned long long)cfg.electra_epoch,
             cfg.min_participants);
        LightClientStore store;
        std::ifstream store_probe{dir + "store.json"};
        if (store_probe.good()) {
            store_probe.close();
            const lcjson::LightClientStoreSnapshot snapshot{
                lcjson::ParseStoreSnapshot(LoadJson(dir + "store.json"))};
            store = snapshot.store;
            Info("B3-finalized store: connection %llu/%s, B3 finality %llu/%s\n",
                 (unsigned long long)snapshot.connection_height,
                 snapshot.connection_block_hash.GetHex().c_str(),
                 (unsigned long long)snapshot.b3_finalized_height,
                 snapshot.b3_finalized_block_hash.GetHex().c_str());
            Info("  store: period %llu, finalized slot %llu, exec block %llu\n",
                 (unsigned long long)store.period,
                 (unsigned long long)store.finalized_header.beacon.slot,
                 (unsigned long long)store.finalized_header.execution.block_number);
            if (g_emit_payloads) {
                UniValue json{UniValue::VOBJ};
                AddStoreMetadata(json, store);
                json.pushKV("source", "b3-finalized-snapshot");
                json.pushKV("connection_height", snapshot.connection_height);
                json.pushKV("connection_block",
                            snapshot.connection_block_hash.GetHex());
                json.pushKV("b3_finalized_height", snapshot.b3_finalized_height);
                json.pushKV("b3_finalized_block",
                            snapshot.b3_finalized_block_hash.GetHex());
                emitted.pushKV("store", std::move(json));
            }
        } else {
            // Original checkpoint bootstrap path remains available before B3
            // has finalized an on-chain light-client connection.
            const uint256 trusted{lcjson::Root(
                lcjson::Field(cfg_json, "trusted_root"), "trusted_root")};
            Info("trusted checkpoint: 0x%s\n", HexStr(trusted).c_str());
            const UniValue boot{LoadJson(dir + "bootstrap.json")};
            const UniValue& bdata{lcjson::Field(boot, "data")};
            BridgeBootstrapV1 bootstrap;
            bootstrap.header = lcjson::ParseHeader(lcjson::Field(bdata, "header"));
            bootstrap.current_committee = lcjson::ParseCommittee(
                lcjson::Field(bdata, "current_sync_committee"));
            bootstrap.current_committee_branch = lcjson::Branch(
                lcjson::Field(bdata, "current_sync_committee_branch"),
                "current_sync_committee_branch");
            Require(InitStore(store, cfg, trusted, bootstrap.header,
                              bootstrap.current_committee,
                              bootstrap.current_committee_branch),
                    "bootstrap (checkpoint root + committee proof + execution proof)");
            Info("  store: period %llu, finalized slot %llu, exec block %llu\n",
                 (unsigned long long)store.period,
                 (unsigned long long)store.finalized_header.beacon.slot,
                 (unsigned long long)store.finalized_header.execution.block_number);
            if (g_emit_payloads) {
                UniValue json{RecordJson(
                    "bootstrap",
                    BridgeRecordV1{BridgeRecordKindV1::BOOTSTRAP, bootstrap})};
                AddStoreMetadata(json, store);
                emitted.pushKV("bootstrap", std::move(json));
            }
        }

        // Committee-period updates.
        const UniValue upds{LoadJson(dir + "updates.json")};
        for (size_t i = 0; i < upds.size(); ++i) {
            const LightClientUpdate u{lcjson::ParseUpdate(lcjson::Field(upds[i], "data"))};
            const unsigned parts{u.sync_aggregate.Participation()};
            const LightClientStore before{store};
            Require(ProcessUpdate(store, cfg, u),
                    "update " + util::ToString(i) + " (sig slot " + util::ToString(u.signature_slot) +
                        ", participation " + util::ToString(parts) + "/512)");
            Info("  store: period %llu, finalized slot %llu, exec block %llu, next %s\n",
                 (unsigned long long)store.period,
                 (unsigned long long)store.finalized_header.beacon.slot,
                 (unsigned long long)store.finalized_header.execution.block_number,
                 store.next ? "yes" : "no");
            if (g_emit_payloads && before != store) {
                UniValue json{RecordJson(
                    "update", BridgeRecordV1{
                                  BridgeRecordKindV1::UPDATE,
                                  BridgeUpdateV1{u}})};
                AddStoreMetadata(json, store);
                json.pushKV("signature_slot", u.signature_slot);
                emitted_updates.push_back(std::move(json));
            }
        }

        // Today's head, if provided.
        std::ifstream fu_probe{dir + "finality_update.json"};
        if (fu_probe.good()) {
            fu_probe.close();
            const UniValue fu{LoadJson(dir + "finality_update.json")};
            const LightClientUpdate u{lcjson::ParseUpdate(lcjson::Field(fu, "data"))};
            const unsigned parts{u.sync_aggregate.Participation()};
            const LightClientStore before{store};
            Require(ProcessUpdate(store, cfg, u),
                    "finality_update (sig slot " + util::ToString(u.signature_slot) +
                        ", participation " + util::ToString(parts) + "/512)");
            if (g_emit_payloads && before != store) {
                UniValue json{RecordJson(
                    "update", BridgeRecordV1{
                                  BridgeRecordKindV1::UPDATE,
                                  BridgeUpdateV1{u}})};
                AddStoreMetadata(json, store);
                json.pushKV("signature_slot", u.signature_slot);
                emitted_updates.push_back(std::move(json));
            }
        }
        const auto& fin{store.finalized_header};
        Info("PROVEN finalized: beacon slot %llu, exec block %llu, receipts_root 0x%s\n",
             (unsigned long long)fin.beacon.slot,
             (unsigned long long)fin.execution.block_number,
             HexStr(fin.execution.receipts_root).c_str());

        // Receipt proof against the PROVEN receipts_root.
        std::ifstream rp_probe{dir + "receipt_proof.json"};
        if (rp_probe.good()) {
            rp_probe.close();
            const UniValue rp{LoadJson(dir + "receipt_proof.json")};
            const uint64_t block{lcjson::Num(lcjson::Field(rp, "block_number"), "block_number")};
            const uint64_t tx_index{
                lcjson::Num(lcjson::Field(rp, "index"), "index")};
            uint256 target_receipts_root{fin.execution.receipts_root};
            uint256 ancestry_source_hash{fin.execution.block_hash};
            std::optional<uint64_t> historical_source_block;
            std::optional<uint256> historical_source_receipts_root;
            std::optional<uint64_t> historical_finalized_execution_block;
            const UniValue& source_anchor{rp.find_value("source_anchor")};
            if (!source_anchor.isNull()) {
                const uint64_t source_block{lcjson::Num(
                    lcjson::Field(source_anchor, "block_number"),
                    "source_anchor.block_number")};
                const uint256 source_hash{lcjson::Root(
                    lcjson::Field(source_anchor, "block_hash"),
                    "source_anchor.block_hash")};
                const uint256 source_receipts_root{lcjson::Root(
                    lcjson::Field(source_anchor, "receipts_root"),
                    "source_anchor.receipts_root")};
                const uint64_t source_beacon_slot{lcjson::Num(
                    lcjson::Field(source_anchor,
                                  "source_finalized_beacon_slot"),
                    "source_anchor.source_finalized_beacon_slot")};
                const uint64_t source_finalized_execution{lcjson::Num(
                    lcjson::Field(source_anchor,
                                  "source_finalized_execution_block"),
                    "source_anchor.source_finalized_execution_block")};
                const uint64_t source_timestamp{lcjson::Num(
                    lcjson::Field(source_anchor, "execution_timestamp"),
                    "source_anchor.execution_timestamp")};
                const uint64_t connected_height{lcjson::Num(
                    lcjson::Field(source_anchor, "connected_height"),
                    "source_anchor.connected_height")};
                const uint256 connected_block{B3DisplayHash(
                    lcjson::Field(source_anchor, "connected_block"),
                    "source_anchor.connected_block")};
                const uint64_t b3_finalized_height{lcjson::Num(
                    lcjson::Field(source_anchor, "b3_finalized_height"),
                    "source_anchor.b3_finalized_height")};
                const uint256 b3_finalized_block{B3DisplayHash(
                    lcjson::Field(source_anchor, "b3_finalized_block"),
                    "source_anchor.b3_finalized_block")};
                if (source_hash.IsNull() || source_receipts_root.IsNull() ||
                    source_block < block ||
                    source_finalized_execution < source_block ||
                    source_finalized_execution - block >
                        MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS ||
                    connected_height > b3_finalized_height ||
                    source_beacon_slot == 0 || source_timestamp == 0) {
                    throw std::runtime_error(
                        "B3 retained source anchor is incomplete or outside the backfill window");
                }
                ancestry_source_hash = source_hash;
                historical_source_block = source_block;
                historical_source_receipts_root = source_receipts_root;
                historical_finalized_execution_block =
                    source_finalized_execution;
                if (g_emit_payloads) {
                    UniValue json{UniValue::VOBJ};
                    json.pushKV("block_number", source_block);
                    json.pushKV("block_hash", EthereumHex(source_hash));
                    json.pushKV("receipts_root",
                                EthereumHex(source_receipts_root));
                    json.pushKV("source_finalized_beacon_slot",
                                source_beacon_slot);
                    json.pushKV("source_finalized_execution_block",
                                source_finalized_execution);
                    json.pushKV("execution_timestamp", source_timestamp);
                    json.pushKV("connected_height", connected_height);
                    json.pushKV("connected_block",
                                connected_block.GetHex());
                    json.pushKV("b3_finalized_height",
                                b3_finalized_height);
                    json.pushKV("b3_finalized_block",
                                b3_finalized_block.GetHex());
                    emitted.pushKV("source_anchor", std::move(json));
                }
            }
            const UniValue& chain_json{rp.find_value("exec_chain")};
            std::vector<std::vector<unsigned char>> chain;
            if (!chain_json.isNull()) {
                if (!chain_json.isArray()) {
                    throw std::runtime_error("exec_chain is not an array");
                }
                for (size_t i = 0; i < chain_json.size(); ++i) {
                    chain.push_back(lcjson::HexField(chain_json[i], "exec_chain"));
                }
            }
            std::optional<ExecAncestor> ancestry_target;
            if (!chain.empty()) {
                ancestry_target = VerifyExecAncestry(
                    ancestry_source_hash, block, chain);
                const auto& anc{ancestry_target};
                if (!anc) throw std::runtime_error("execution ancestry chain failed to verify");
                if (historical_source_block) {
                    const BridgeRetainedExecutionSourceV1 source{
                        *historical_source_block, ancestry_source_hash,
                        *historical_source_receipts_root,
                        *historical_finalized_execution_block};
                    if (!VerifyRetainedExecutionSourceV1(
                            source, block, chain)) {
                        throw std::runtime_error(
                            "execution ancestry does not start at the B3 retained source anchor");
                    }
                }
                target_receipts_root = anc->receipts_root;
                Info("  [ok] ancestry proven: %zu headers from finalized block %llu down to %llu\n",
                     chain.size(),
                     (unsigned long long)historical_source_block.value_or(
                         fin.execution.block_number),
                     (unsigned long long)block);
            } else if (block != fin.execution.block_number ||
                       g_emit_payloads || historical_source_block) {
                throw std::runtime_error(
                    "an exec_chain including the finalized source and deposit target headers is required");
            }
            const auto key{lcjson::HexField(lcjson::Field(rp, "key"), "key")};
            const auto value{lcjson::HexField(lcjson::Field(rp, "value"), "value")};
            if (key != RlpEncodeUint64(tx_index)) {
                throw std::runtime_error(
                    "receipt proof key is not canonical RLP(transaction index)");
            }
            const UniValue& pj{lcjson::Field(rp, "proof")};
            if (!pj.isArray()) throw std::runtime_error("proof is not an array");
            std::vector<std::vector<unsigned char>> proof;
            for (size_t i = 0; i < pj.size(); ++i) proof.push_back(lcjson::HexField(pj[i], "proof"));

            const auto got{VerifyMptProof(target_receipts_root, key, proof)};
            if (!got) throw std::runtime_error("MPT receipt proof failed");
            if (*got != value) throw std::runtime_error("MPT value mismatch");
            Info("  [ok] receipt %llu proven against the receipts_root (%zu proof nodes)\n",
                 (unsigned long long)tx_index, proof.size());

            const auto receipt{DecodeReceipt(*got)};
            if (!receipt) throw std::runtime_error("receipt decode failed");
            size_t transfers{0};
            const uint256 transfer_topic{[] {
                static constexpr char sig[]{"Transfer(address,address,uint256)"};
                uint256 out;
                Keccak256().Write({reinterpret_cast<const unsigned char*>(sig), sizeof(sig) - 1}).Finalize(out);
                return out;
            }()};
            for (const auto& log : receipt->logs) {
                if (!log.topics.empty() && log.topics[0] == transfer_topic) ++transfers;
            }
            Info("  [ok] receipt decoded: type %u, status %u, %zu logs, %zu ERC-20 Transfer(s)\n",
                 receipt->type, unsigned{receipt->status},
                 receipt->logs.size(), transfers);

            // Vault Deposit extraction, when a vault address is given.
            const UniValue& vault_json{rp.find_value("vault")};
            std::optional<EthAddress> vault;
            std::optional<EthAddress> expected_token;
            if (!vault_json.isNull()) {
                vault = lcjson::Bytes<20>(vault_json, "vault");
                expected_token = lcjson::Bytes<20>(
                    lcjson::Field(rp, "token"), "token");
                const auto deposits{ExtractDeposits(*receipt, *vault)};
                for (const auto& d : deposits) {
                    // Amount prints as decimal wei when it fits 64 bits.
                    uint64_t small{0};
                    bool fits{true};
                    for (int i = 0; i < 24; ++i) fits &= (d.amount[i] == 0);
                    for (int i = 24; i < 32; ++i) small = (small << 8) | d.amount[i];
                    Info("  [ok] DEPOSIT PROVEN: id %llu, token 0x%s, amount %s, b3_recipient 0x%s\n",
                         (unsigned long long)d.deposit_id,
                         HexStr(d.token).c_str(),
                         fits ? (util::ToString(small) + " wei").c_str()
                              : ("0x" + HexStr(d.amount)).c_str(),
                         HexStr(d.b3_recipient).c_str());
                }
                const UniValue& expect{rp.find_value("expect_deposits")};
                const size_t want_n{expect.isNull() ? size_t{1} : size_t{lcjson::Num(expect, "expect_deposits")}};
                if (deposits.size() < want_n) {
                    throw std::runtime_error("expected " + util::ToString(want_n) +
                                             " vault deposit(s), extracted " + util::ToString(deposits.size()));
                }
            }

            if (g_emit_payloads) {
                if (!vault) {
                    throw std::runtime_error(
                        "receipt_proof.json must contain vault when emitting a MINT");
                }
                const uint64_t log_index_u64{lcjson::Num(
                    lcjson::Field(rp, "receipt_log_index"),
                    "receipt_log_index")};
                if (log_index_u64 > std::numeric_limits<uint32_t>::max()) {
                    throw std::runtime_error("receipt_log_index exceeds uint32");
                }
                const auto deposit{ExtractDepositAt(
                    *receipt, *vault, static_cast<size_t>(log_index_u64))};
                if (!deposit) {
                    throw std::runtime_error(
                        "receipt_log_index does not name an exact successful vault Deposit");
                }
                if (!expected_token || deposit->token != *expected_token) {
                    throw std::runtime_error(
                        "selected Deposit token does not match the configured bridge token");
                }
                const uint256 registry_id{B3DisplayHash(
                    lcjson::Field(rp, "registry_id"), "registry_id")};
                const UniValue& prior_anchors{
                    rp.find_value("retained_anchor_hashes")};
                if (!prior_anchors.isNull()) {
                    if (!prior_anchors.isArray()) {
                        throw std::runtime_error(
                            "retained_anchor_hashes is not an array");
                    }
                    for (size_t i{0}; i < prior_anchors.size(); ++i) {
                        retained_anchors.insert(lcjson::Root(
                            prior_anchors[i], "retained_anchor_hashes"));
                    }
                }
                const auto execution_plan{PlanBridgeExecutionAncestryV1(
                    ancestry_source_hash, block, chain,
                    retained_anchors)};
                if (!execution_plan || !ancestry_target) {
                    throw std::runtime_error(
                        "execution ancestry cannot be split into canonical bridge records");
                }

                for (const auto& step : execution_plan->backfills) {
                    const BridgeRecordV1 record{
                        BridgeRecordKindV1::EXECUTION_BACKFILL, step.record};
                    UniValue json{RecordJson("execution-backfill", record)};
                    json.pushKV("source_anchor_hash",
                                EthereumHex(step.record.finalized_anchor_hash));
                    json.pushKV("target_block_number",
                                step.record.target_block_number);
                    json.pushKV("target_block_hash",
                                EthereumHex(step.target_block_hash));
                    json.pushKV("header_count",
                                step.record.ancestry_headers.size());
                    emitted_backfills.push_back(std::move(json));
                }

                BridgeMintV1 mint;
                mint.registry_id = registry_id;
                mint.output_index = 0;
                mint.finalized_anchor_hash =
                    execution_plan->mint_anchor_hash;
                mint.target_block_number = block;
                mint.tx_index = tx_index;
                mint.receipt_log_index =
                    static_cast<uint32_t>(log_index_u64);
                mint.ancestry_headers =
                    execution_plan->mint_ancestry_headers;
                mint.mpt_nodes = proof;
                const BridgeRecordV1 mint_record{
                    BridgeRecordKindV1::MINT, mint};
                UniValue mint_json{RecordJson("mint", mint_record)};
                mint_json.pushKV("registry_id", registry_id.GetHex());
                mint_json.pushKV("origin_amount",
                                 RawAmount64(deposit->amount));
                mint_json.pushKV("origin_token",
                                 "0x" + HexStr(deposit->token));
                mint_json.pushKV("amount", ClaimAmount(*deposit, rp));
                mint_json.pushKV(
                    "origin_decimals",
                    lcjson::Num(lcjson::Field(rp, "origin_decimals"),
                                "origin_decimals"));
                mint_json.pushKV(
                    "asset_decimals",
                    lcjson::Num(lcjson::Field(rp, "asset_decimals"),
                                "asset_decimals"));
                mint_json.pushKV("b3_recipient",
                                 "0x" + HexStr(deposit->b3_recipient));
                mint_json.pushKV("deposit_id", deposit->deposit_id);
                mint_json.pushKV("tx_index", tx_index);
                mint_json.pushKV("receipt_log_index", log_index_u64);
                mint_json.pushKV("source_anchor_hash",
                                 EthereumHex(mint.finalized_anchor_hash));
                mint_json.pushKV("target_block_number", block);
                mint_json.pushKV("target_block_hash",
                                 EthereumHex(ancestry_target->block_hash));
                mint_json.pushKV("header_count",
                                 mint.ancestry_headers.size());
                emitted.pushKV("mint", std::move(mint_json));
            }
        }

        if (g_emit_payloads) {
            emitted.pushKV("updates", std::move(emitted_updates));
            emitted.pushKV("backfills", std::move(emitted_backfills));
        }
        Info("ALL VERIFIED\n");
        if (g_emit_payloads) {
            std::printf("%s\n", emitted.write().c_str());
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
