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
//   <dir>/bootstrap.json        raw /eth/v1/beacon/light_client/bootstrap/...
//   <dir>/updates.json          raw /eth/v1/beacon/light_client/updates?...
//   <dir>/finality_update.json  raw /eth/v1/beacon/light_client/finality_update (optional)
//   <dir>/receipt_proof.json    {"index","key","value","proof":[...]} (optional)
//
// Exit code 0 = every step verified; nonzero otherwise. NOT part of the
// node; test/tooling only (option B3_BRIDGE_TOOLS).

#include <bridge/deposit.h>
#include <bridge/exec_chain.h>
#include <bridge/eth_light_client.h>
#include <bridge/lc_json.h>
#include <bridge/mpt.h>
#include <uint256.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace bridge;

namespace {

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
    std::printf("  [ok] %s\n", what.c_str());
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: b3-bridge-ethcheck <dir>\n");
        return 2;
    }
    const std::string dir{std::string{argv[1]} + "/"};
    try {
        const UniValue cfg_json{LoadJson(dir + "config.json")};
        const LightClientConfig cfg{lcjson::ParseConfig(cfg_json)};
        const uint256 trusted{lcjson::Root(lcjson::Field(cfg_json, "trusted_root"), "trusted_root")};
        std::printf("b3-bridge-ethcheck: %zu forks, electra_epoch %llu, min_participants %u\n",
                    cfg.forks.size(), (unsigned long long)cfg.electra_epoch, cfg.min_participants);
        std::printf("trusted checkpoint: 0x%s\n", HexStr(trusted).c_str());

        // Bootstrap.
        const UniValue boot{LoadJson(dir + "bootstrap.json")};
        const UniValue& bdata{lcjson::Field(boot, "data")};
        LightClientStore store;
        Require(InitStore(store, cfg, trusted,
                          lcjson::ParseHeader(lcjson::Field(bdata, "header")),
                          lcjson::ParseCommittee(lcjson::Field(bdata, "current_sync_committee")),
                          lcjson::Branch(lcjson::Field(bdata, "current_sync_committee_branch"),
                                         "current_sync_committee_branch")),
                "bootstrap (checkpoint root + committee proof + execution proof)");
        std::printf("  store: period %llu, finalized slot %llu, exec block %llu\n",
                    (unsigned long long)store.period,
                    (unsigned long long)store.finalized_header.beacon.slot,
                    (unsigned long long)store.finalized_header.execution.block_number);

        // Committee-period updates.
        const UniValue upds{LoadJson(dir + "updates.json")};
        for (size_t i = 0; i < upds.size(); ++i) {
            const LightClientUpdate u{lcjson::ParseUpdate(lcjson::Field(upds[i], "data"))};
            const unsigned parts{u.sync_aggregate.Participation()};
            Require(ProcessUpdate(store, cfg, u),
                    "update " + util::ToString(i) + " (sig slot " + util::ToString(u.signature_slot) +
                        ", participation " + util::ToString(parts) + "/512)");
            std::printf("  store: period %llu, finalized slot %llu, exec block %llu, next %s\n",
                        (unsigned long long)store.period,
                        (unsigned long long)store.finalized_header.beacon.slot,
                        (unsigned long long)store.finalized_header.execution.block_number,
                        store.next ? "yes" : "no");
        }

        // Today's head, if provided.
        std::ifstream fu_probe{dir + "finality_update.json"};
        if (fu_probe.good()) {
            fu_probe.close();
            const UniValue fu{LoadJson(dir + "finality_update.json")};
            const LightClientUpdate u{lcjson::ParseUpdate(lcjson::Field(fu, "data"))};
            const unsigned parts{u.sync_aggregate.Participation()};
            Require(ProcessUpdate(store, cfg, u),
                    "finality_update (sig slot " + util::ToString(u.signature_slot) +
                        ", participation " + util::ToString(parts) + "/512)");
        }
        const auto& fin{store.finalized_header};
        std::printf("PROVEN finalized: beacon slot %llu, exec block %llu, receipts_root 0x%s\n",
                    (unsigned long long)fin.beacon.slot,
                    (unsigned long long)fin.execution.block_number,
                    HexStr(fin.execution.receipts_root).c_str());

        // Receipt proof against the PROVEN receipts_root.
        std::ifstream rp_probe{dir + "receipt_proof.json"};
        if (rp_probe.good()) {
            rp_probe.close();
            const UniValue rp{LoadJson(dir + "receipt_proof.json")};
            const uint64_t block{lcjson::Num(lcjson::Field(rp, "block_number"), "block_number")};
            uint256 target_receipts_root{fin.execution.receipts_root};
            if (block != fin.execution.block_number) {
                // The deposit block is older than the proven finalized block:
                // close the gap with the keccak parent-hash ancestry chain.
                const UniValue& chain_json{rp.find_value("exec_chain")};
                if (chain_json.isNull()) {
                    throw std::runtime_error("receipt proof is for block " + util::ToString(block) +
                                             ", store finalized " + util::ToString(fin.execution.block_number) +
                                             ", and no exec_chain ancestry was provided");
                }
                std::vector<std::vector<unsigned char>> chain;
                for (size_t i = 0; i < chain_json.size(); ++i) {
                    chain.push_back(lcjson::HexField(chain_json[i], "exec_chain"));
                }
                const auto anc{VerifyExecAncestry(fin.execution.block_hash, block, chain)};
                if (!anc) throw std::runtime_error("execution ancestry chain failed to verify");
                target_receipts_root = anc->receipts_root;
                std::printf("  [ok] ancestry proven: %zu headers from finalized block %llu down to %llu\n",
                            chain.size(), (unsigned long long)fin.execution.block_number,
                            (unsigned long long)block);
            }
            const auto key{lcjson::HexField(lcjson::Field(rp, "key"), "key")};
            const auto value{lcjson::HexField(lcjson::Field(rp, "value"), "value")};
            const UniValue& pj{lcjson::Field(rp, "proof")};
            std::vector<std::vector<unsigned char>> proof;
            for (size_t i = 0; i < pj.size(); ++i) proof.push_back(lcjson::HexField(pj[i], "proof"));

            const auto got{VerifyMptProof(target_receipts_root, key, proof)};
            if (!got) throw std::runtime_error("MPT receipt proof failed");
            if (*got != value) throw std::runtime_error("MPT value mismatch");
            std::printf("  [ok] receipt %llu proven against the receipts_root (%zu proof nodes)\n",
                        (unsigned long long)lcjson::Num(lcjson::Field(rp, "index"), "index"),
                        proof.size());

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
            std::printf("  [ok] receipt decoded: type %u, status %u, %zu logs, %zu ERC-20 Transfer(s)\n",
                        receipt->type, unsigned{receipt->status}, receipt->logs.size(), transfers);

            // Vault Deposit extraction, when a vault address is given.
            const UniValue& vault_json{rp.find_value("vault")};
            if (!vault_json.isNull()) {
                const EthAddress vault{lcjson::Bytes<20>(vault_json, "vault")};
                const auto deposits{ExtractDeposits(*receipt, vault)};
                for (const auto& d : deposits) {
                    // Amount prints as decimal wei when it fits 64 bits.
                    uint64_t small{0};
                    bool fits{true};
                    for (int i = 0; i < 24; ++i) fits &= (d.amount[i] == 0);
                    for (int i = 24; i < 32; ++i) small = (small << 8) | d.amount[i];
                    std::printf("  [ok] DEPOSIT PROVEN: id %llu, token 0x%s, amount %s, b3_recipient 0x%s\n",
                                (unsigned long long)d.deposit_id, HexStr(d.token).c_str(),
                                fits ? (util::ToString(small) + " wei").c_str() : ("0x" + HexStr(d.amount)).c_str(),
                                HexStr(d.b3_recipient).c_str());
                }
                const UniValue& expect{rp.find_value("expect_deposits")};
                const size_t want_n{expect.isNull() ? size_t{1} : size_t{lcjson::Num(expect, "expect_deposits")}};
                if (deposits.size() < want_n) {
                    throw std::runtime_error("expected " + util::ToString(want_n) +
                                             " vault deposit(s), extracted " + util::ToString(deposits.size()));
                }
            }
        }

        std::printf("ALL VERIFIED\n");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
