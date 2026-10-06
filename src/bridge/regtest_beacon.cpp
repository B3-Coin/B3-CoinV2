// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// b3-bridge-regtest-beacon: REGTEST ONLY synthetic sync-committee tool
// (regtest plan section 3.4). It lays the deterministic synthetic beacon
// chain of bridge/regtest_beacon.h over real execution headers from a local
// anvil and emits Beacon-API light-client objects plus their canonical
// type-10 payloads, for the mock beacon, the unchanged relayer and
// b3-bridge-ethcheck. It refuses the Ethereum mainnet genesis validators
// root, fork versions outside the synthetic 0xb3 namespace and any
// electra_epoch other than 0. No network access and no real key material.
// NOT part of the node; test tooling only (option B3_BRIDGE_TOOLS).
//
// Request (one JSON object):
//   {"version": 1, "genesis_validators_root": "0x..", "fork_version": "0xb3..",
//    "electra_epoch": 0, "slot_offset": S0, "base_block": B0,
//    "headers": [eth_getBlockByNumber results for B0, B0+1, ...],
//    "outputs": [{"kind": "bootstrap"} |
//                {"kind": "update", "finalized": N, optional variants:
//                 "participants", "next_committee", "signature_slot",
//                 "slot_shift", "sign_genesis_validators_root",
//                 "sign_fork_version", "finalized_proposer_index"}]}
//
// Stdout: {"outputs": [{"kind", "data": <Beacon-API data object>,
//   "payload_hex": <EncodeBridgeRecordV1 BOOTSTRAP/UPDATE>, ...metadata}]}.
// Exit 1 with "FAIL: ..." on stderr for any malformed or refused request.

#include <bridge/lc_json.h>
#include <bridge/proof.h>
#include <bridge/regtest_beacon.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace bridge;

namespace {

UniValue LoadJson(const std::string& path)
{
    std::ifstream file{path};
    if (!file) throw std::runtime_error("cannot open " + path);
    std::ostringstream text;
    text << file.rdbuf();
    UniValue value;
    if (!value.read(text.str()) || !value.isObject()) {
        throw std::runtime_error("expected one JSON object in " + path);
    }
    return value;
}

const UniValue& Field(const UniValue& object, const std::string& name)
{
    const UniValue& value{object.find_value(name)};
    if (value.isNull()) throw std::runtime_error("missing field: " + name);
    return value;
}

//! Request and output objects are strict, so a misspelled variant can never
//! silently produce a canonical (valid) object.
void CheckKeys(const UniValue& object, const std::string& where,
               std::initializer_list<std::string_view> allowed)
{
    if (!object.isObject()) throw std::runtime_error(where + " must be a JSON object");
    for (const std::string& key : object.getKeys()) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            throw std::runtime_error("unknown key " + where + "." + key);
        }
    }
}

uint64_t Number(const UniValue& object, const std::string& name)
{
    const UniValue& value{Field(object, name)};
    if (!value.isNum()) throw std::runtime_error(name + " must be a number");
    return value.getInt<uint64_t>();
}

std::vector<unsigned char> Data(const UniValue& object, const std::string& name)
{
    const UniValue& value{Field(object, name)};
    if (!value.isStr()) throw std::runtime_error(name + " must be 0x-prefixed hex");
    const std::string_view text{value.get_str()};
    if (!text.starts_with("0x")) throw std::runtime_error(name + " must be 0x-prefixed hex");
    const std::string_view hex{text.substr(2)};
    if (!hex.empty() && !IsHex(hex)) throw std::runtime_error(name + " must be 0x-prefixed hex");
    return ParseHex(hex);
}

template <size_t N>
std::array<unsigned char, N> Fixed(const UniValue& object, const std::string& name)
{
    const auto bytes{Data(object, name)};
    if (bytes.size() != N) {
        throw std::runtime_error(name + " must be " + util::ToString(N) + " bytes");
    }
    std::array<unsigned char, N> out;
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

uint256 HashField(const UniValue& object, const std::string& name)
{
    const auto bytes{Fixed<32>(object, name)};
    return uint256{std::span<const unsigned char>{bytes}};
}

//! JSON-RPC quantity ("0x" + 1..2*width hex digits) as big-endian bytes.
std::array<unsigned char, 32> QuantityBE(const UniValue& object, const std::string& name,
                                         const size_t width)
{
    const UniValue& value{Field(object, name)};
    if (!value.isStr()) throw std::runtime_error(name + " must be a hex quantity");
    std::string digits{value.get_str()};
    const bool prefixed{digits.rfind("0x", 0) == 0};
    if (prefixed) digits.erase(0, 2);
    if (digits.size() % 2) digits.insert(digits.begin(), '0');
    if (!prefixed || !IsHex(digits) || digits.size() > 2 * width) {
        throw std::runtime_error(name + " must be a hex quantity of at most " +
                                 util::ToString(width) + " bytes");
    }
    const auto bytes{ParseHex(digits)};
    std::array<unsigned char, 32> out{};
    std::copy(bytes.begin(), bytes.end(), out.end() - bytes.size());
    return out;
}

uint64_t Quantity64(const UniValue& object, const std::string& name)
{
    const auto be{QuantityBE(object, name, 8)};
    uint64_t out{0};
    for (size_t i{24}; i < be.size(); ++i) out = (out << 8) | be[i];
    return out;
}

uint64_t OptionalQuantity64(const UniValue& object, const std::string& name)
{
    return object.find_value(name).isNull() ? 0 : Quantity64(object, name);
}

//! An eth_getBlockByNumber result as the beacon ExecutionPayloadHeader. The
//! transactions and withdrawals roots are carried verbatim; B3 consensus
//! reads only the block hash, number, state root, receipts root and
//! timestamp of E(N) (src/bridge/exec_chain.h).
ssz::ExecutionPayloadHeader ExecutionHeader(const UniValue& rpc)
{
    if (!rpc.isObject()) throw std::runtime_error("execution header must be a JSON object");
    ssz::ExecutionPayloadHeader out;
    out.parent_hash = HashField(rpc, "parentHash");
    out.fee_recipient = Fixed<20>(rpc, "miner");
    out.state_root = HashField(rpc, "stateRoot");
    out.receipts_root = HashField(rpc, "receiptsRoot");
    out.logs_bloom = Fixed<256>(rpc, "logsBloom");
    out.prev_randao = HashField(rpc, "mixHash");
    out.block_number = Quantity64(rpc, "number");
    out.gas_limit = Quantity64(rpc, "gasLimit");
    out.gas_used = Quantity64(rpc, "gasUsed");
    out.timestamp = Quantity64(rpc, "timestamp");
    out.extra_data = Data(rpc, "extraData");
    if (out.extra_data.size() > 32) throw std::runtime_error("extraData is longer than 32 bytes");
    // SSZ uint256 is a little-endian chunk.
    const auto base_fee{QuantityBE(rpc, "baseFeePerGas", 32)};
    std::array<unsigned char, 32> base_fee_le{};
    std::reverse_copy(base_fee.begin(), base_fee.end(), base_fee_le.begin());
    out.base_fee_per_gas = uint256{std::span<const unsigned char>{base_fee_le}};
    out.block_hash = HashField(rpc, "hash");
    out.transactions_root = HashField(rpc, "transactionsRoot");
    if (!rpc.find_value("withdrawalsRoot").isNull()) out.withdrawals_root = HashField(rpc, "withdrawalsRoot");
    out.blob_gas_used = OptionalQuantity64(rpc, "blobGasUsed");
    out.excess_blob_gas = OptionalQuantity64(rpc, "excessBlobGas");
    return out;
}

std::string PayloadHex(const BridgeRecordV1& record)
{
    const auto encoded{EncodeBridgeRecordV1(record)};
    if (!encoded) throw std::runtime_error("record exceeds the canonical type-10 limits");
    return HexStr(*encoded);
}

UniValue BootstrapOutput(const regtest_beacon::SyntheticBeacon& beacon)
{
    const BridgeBootstrapV1 bootstrap{beacon.Bootstrap()};
    const ssz::BeaconBlockHeader& header{bootstrap.header.beacon};
    UniValue out{UniValue::VOBJ};
    out.pushKV("kind", "bootstrap");
    out.pushKV("data", lcjson::BootstrapJson(bootstrap.header, bootstrap.current_committee,
                                             bootstrap.current_committee_branch));
    out.pushKV("payload_hex", PayloadHex(BridgeRecordV1{BridgeRecordKindV1::BOOTSTRAP, bootstrap}));
    out.pushKV("checkpoint_root", lcjson::EthereumHex(header.HashTreeRoot()));
    out.pushKV("checkpoint_slot", header.slot);
    out.pushKV("checkpoint_block", bootstrap.header.execution.block_number);
    out.pushKV("checkpoint_block_hash", lcjson::EthereumHex(bootstrap.header.execution.block_hash));
    out.pushKV("period", PeriodAtSlot(header.slot));
    return out;
}

UniValue UpdateOutput(const regtest_beacon::SyntheticBeacon& beacon, const UniValue& request)
{
    CheckKeys(request, "update", {"kind", "finalized", "participants", "next_committee",
                                  "signature_slot", "slot_shift", "sign_genesis_validators_root",
                                  "sign_fork_version", "finalized_proposer_index"});
    regtest_beacon::UpdateOptions options;
    if (!request.find_value("participants").isNull()) {
        const uint64_t participants{Number(request, "participants")};
        if (participants == 0 || participants > ssz::SYNC_COMMITTEE_SIZE) {
            throw std::runtime_error("participants must be between 1 and 512");
        }
        options.participants = static_cast<unsigned>(participants);
    }
    if (const UniValue& next{request.find_value("next_committee")}; !next.isNull()) {
        if (!next.isBool()) throw std::runtime_error("next_committee must be a boolean");
        options.next_committee = next.get_bool();
    }
    if (!request.find_value("signature_slot").isNull()) {
        options.signature_slot = Number(request, "signature_slot");
    }
    if (!request.find_value("slot_shift").isNull()) {
        options.slot_shift = Number(request, "slot_shift");
    }
    if (!request.find_value("sign_genesis_validators_root").isNull()) {
        options.sign_genesis_validators_root = HashField(request, "sign_genesis_validators_root");
    }
    if (!request.find_value("sign_fork_version").isNull()) {
        options.sign_fork_version = Fixed<4>(request, "sign_fork_version");
    }
    if (!request.find_value("finalized_proposer_index").isNull()) {
        options.finalized_proposer_index = Number(request, "finalized_proposer_index");
    }

    const LightClientUpdate update{beacon.Update(Number(request, "finalized"), options)};
    UniValue out{UniValue::VOBJ};
    out.pushKV("kind", "update");
    out.pushKV("data", lcjson::UpdateJson(update));
    out.pushKV("payload_hex", PayloadHex(BridgeRecordV1{BridgeRecordKindV1::UPDATE, BridgeUpdateV1{update}}));
    out.pushKV("finalized_block", update.finalized.execution.block_number);
    out.pushKV("finalized_block_hash", lcjson::EthereumHex(update.finalized.execution.block_hash));
    out.pushKV("finalized_slot", update.finalized.beacon.slot);
    out.pushKV("finalized_root", lcjson::EthereumHex(update.finalized.beacon.HashTreeRoot()));
    out.pushKV("attested_slot", update.attested.beacon.slot);
    out.pushKV("signature_slot", update.signature_slot);
    // Beacon-API updates are indexed by the attested header's period.
    out.pushKV("period", PeriodAtSlot(update.attested.beacon.slot));
    out.pushKV("participants", update.sync_aggregate.Participation());
    out.pushKV("has_next_committee", update.has_next);
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fputs("usage: b3-bridge-regtest-beacon <request.json>\n", stderr);
        return 2;
    }
    try {
        const UniValue request{LoadJson(argv[1])};
        CheckKeys(request, "request", {"version", "genesis_validators_root", "fork_version",
                                       "electra_epoch", "slot_offset", "base_block", "headers",
                                       "outputs"});
        if (Number(request, "version") != 1) throw std::runtime_error("unsupported request version");
        regtest_beacon::SyntheticDomain domain;
        domain.genesis_validators_root = HashField(request, "genesis_validators_root");
        domain.fork_version = Fixed<4>(request, "fork_version");
        domain.electra_epoch = Number(request, "electra_epoch");
        // Refuse a real-network domain before reading anything else.
        regtest_beacon::CheckDomain(domain);

        const uint64_t base_block{Number(request, "base_block")};
        const UniValue& rpc_headers{Field(request, "headers")};
        if (!rpc_headers.isArray() || rpc_headers.empty()) {
            throw std::runtime_error("headers must be a non-empty array");
        }
        std::vector<ssz::ExecutionPayloadHeader> headers;
        headers.reserve(rpc_headers.size());
        for (size_t i{0}; i < rpc_headers.size(); ++i) headers.push_back(ExecutionHeader(rpc_headers[i]));
        if (headers.front().block_number != base_block) {
            throw std::runtime_error("headers[0] is not base_block");
        }
        const regtest_beacon::SyntheticBeacon beacon{domain, Number(request, "slot_offset"),
                                                     std::move(headers)};

        const UniValue& requested{Field(request, "outputs")};
        if (!requested.isArray()) throw std::runtime_error("outputs must be an array");
        UniValue outputs{UniValue::VARR};
        for (size_t i{0}; i < requested.size(); ++i) {
            const UniValue& kind{Field(requested[i], "kind")};
            if (!kind.isStr()) throw std::runtime_error("output kind must be a string");
            if (kind.get_str() == "bootstrap") {
                CheckKeys(requested[i], "bootstrap", {"kind"});
                outputs.push_back(BootstrapOutput(beacon));
            } else if (kind.get_str() == "update") {
                outputs.push_back(UpdateOutput(beacon, requested[i]));
            } else {
                throw std::runtime_error("unknown output kind " + kind.get_str());
            }
        }
        UniValue result{UniValue::VOBJ};
        result.pushKV("outputs", std::move(outputs));
        std::fputs((result.write() + "\n").c_str(), stdout);
        return 0;
    } catch (const std::exception& e) {
        std::fputs(("FAIL: " + std::string{e.what()} + "\n").c_str(), stderr);
        return 1;
    }
}
