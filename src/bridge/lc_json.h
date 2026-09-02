// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_BRIDGE_LC_JSON_H
#define B3COIN_BRIDGE_LC_JSON_H

#include <bridge/eth_light_client.h>
#include <bridge/ssz.h>
#include <uint256.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <array>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

/** Parsers from the Ethereum beacon-API JSON shapes (light-client bootstrap,
 *  updates, finality updates) into the bridge light-client structures.
 *  Used by the b3-bridge-ethcheck harness and, later, the relayer. Strict:
 *  any missing field, wrong width or malformed number throws.
 *  Header-only; not reachable from consensus. */
namespace bridge {
namespace lcjson {

inline std::vector<unsigned char> HexField(const UniValue& v, const std::string& name)
{
    if (!v.isStr()) throw std::runtime_error("field not a string: " + name);
    std::string s{v.get_str()};
    if (s.rfind("0x", 0) == 0) s = s.substr(2);
    auto bytes{TryParseHex<unsigned char>(s)};
    if (!bytes) throw std::runtime_error("bad hex in field: " + name);
    return *bytes;
}

inline uint256 Root(const UniValue& v, const std::string& name)
{
    const auto b{HexField(v, name)};
    if (b.size() != 32) throw std::runtime_error("expected 32 bytes: " + name);
    return uint256{std::span<const unsigned char>{b}};
}

template <size_t N>
inline std::array<unsigned char, N> Bytes(const UniValue& v, const std::string& name)
{
    const auto b{HexField(v, name)};
    if (b.size() != N) throw std::runtime_error("wrong width: " + name);
    std::array<unsigned char, N> out;
    std::copy(b.begin(), b.end(), out.begin());
    return out;
}

inline uint64_t Num(const UniValue& v, const std::string& name)
{
    if (v.isNum()) return v.getInt<uint64_t>();
    if (!v.isStr()) throw std::runtime_error("field not numeric: " + name);
    const auto r{ToIntegral<uint64_t>(v.get_str())};
    if (!r) throw std::runtime_error("bad number: " + name);
    return *r;
}

//! Decimal string (beacon-API base_fee_per_gas) -> SSZ uint256 chunk (LE).
inline uint256 DecimalToUint256LE(const UniValue& v, const std::string& name)
{
    if (!v.isStr()) throw std::runtime_error("field not a string: " + name);
    std::array<unsigned char, 32> le{};
    for (const char c : v.get_str()) {
        if (c < '0' || c > '9') throw std::runtime_error("bad decimal: " + name);
        unsigned carry{static_cast<unsigned>(c - '0')};
        for (auto& byte : le) {
            const unsigned x{static_cast<unsigned>(byte) * 10 + carry};
            byte = static_cast<unsigned char>(x & 0xff);
            carry = x >> 8;
        }
        if (carry) throw std::runtime_error("decimal overflow: " + name);
    }
    return uint256{std::span<const unsigned char>{le}};
}

inline std::vector<uint256> Branch(const UniValue& v, const std::string& name)
{
    if (!v.isArray()) throw std::runtime_error("branch not an array: " + name);
    std::vector<uint256> out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) out.push_back(Root(v[i], name));
    return out;
}

inline const UniValue& Field(const UniValue& v, const std::string& name)
{
    const UniValue& f{v.find_value(name)};
    if (f.isNull()) throw std::runtime_error("missing field: " + name);
    return f;
}

inline ssz::BeaconBlockHeader ParseBeacon(const UniValue& v)
{
    ssz::BeaconBlockHeader h;
    h.slot = Num(Field(v, "slot"), "slot");
    h.proposer_index = Num(Field(v, "proposer_index"), "proposer_index");
    h.parent_root = Root(Field(v, "parent_root"), "parent_root");
    h.state_root = Root(Field(v, "state_root"), "state_root");
    h.body_root = Root(Field(v, "body_root"), "body_root");
    return h;
}

inline ssz::ExecutionPayloadHeader ParseExec(const UniValue& v)
{
    ssz::ExecutionPayloadHeader e;
    e.parent_hash = Root(Field(v, "parent_hash"), "parent_hash");
    e.fee_recipient = Bytes<20>(Field(v, "fee_recipient"), "fee_recipient");
    e.state_root = Root(Field(v, "state_root"), "state_root");
    e.receipts_root = Root(Field(v, "receipts_root"), "receipts_root");
    e.logs_bloom = Bytes<256>(Field(v, "logs_bloom"), "logs_bloom");
    e.prev_randao = Root(Field(v, "prev_randao"), "prev_randao");
    e.block_number = Num(Field(v, "block_number"), "block_number");
    e.gas_limit = Num(Field(v, "gas_limit"), "gas_limit");
    e.gas_used = Num(Field(v, "gas_used"), "gas_used");
    e.timestamp = Num(Field(v, "timestamp"), "timestamp");
    e.extra_data = HexField(Field(v, "extra_data"), "extra_data");
    if (e.extra_data.size() > 32) throw std::runtime_error("extra_data too long");
    e.base_fee_per_gas = DecimalToUint256LE(Field(v, "base_fee_per_gas"), "base_fee_per_gas");
    e.block_hash = Root(Field(v, "block_hash"), "block_hash");
    e.transactions_root = Root(Field(v, "transactions_root"), "transactions_root");
    e.withdrawals_root = Root(Field(v, "withdrawals_root"), "withdrawals_root");
    e.blob_gas_used = Num(Field(v, "blob_gas_used"), "blob_gas_used");
    e.excess_blob_gas = Num(Field(v, "excess_blob_gas"), "excess_blob_gas");
    return e;
}

inline LightClientHeader ParseHeader(const UniValue& v)
{
    LightClientHeader h;
    h.beacon = ParseBeacon(Field(v, "beacon"));
    h.execution = ParseExec(Field(v, "execution"));
    h.execution_branch = Branch(Field(v, "execution_branch"), "execution_branch");
    return h;
}

inline ssz::SyncCommittee ParseCommittee(const UniValue& v)
{
    ssz::SyncCommittee c;
    const UniValue& keys{Field(v, "pubkeys")};
    if (!keys.isArray() || keys.size() != ssz::SYNC_COMMITTEE_SIZE) {
        throw std::runtime_error("committee must have 512 pubkeys");
    }
    c.pubkeys.reserve(ssz::SYNC_COMMITTEE_SIZE);
    for (size_t i = 0; i < keys.size(); ++i) c.pubkeys.push_back(Bytes<48>(keys[i], "pubkey"));
    c.aggregate_pubkey = Bytes<48>(Field(v, "aggregate_pubkey"), "aggregate_pubkey");
    return c;
}

//! Parse the `data` object of a light-client update or finality update.
inline LightClientUpdate ParseUpdate(const UniValue& v)
{
    LightClientUpdate u;
    u.attested = ParseHeader(Field(v, "attested_header"));
    u.finalized = ParseHeader(Field(v, "finalized_header"));
    u.finality_branch = Branch(Field(v, "finality_branch"), "finality_branch");
    if (!v.find_value("next_sync_committee").isNull()) {
        u.has_next = true;
        u.next_committee = ParseCommittee(Field(v, "next_sync_committee"));
        u.next_branch = Branch(Field(v, "next_sync_committee_branch"), "next_sync_committee_branch");
    }
    const UniValue& agg{Field(v, "sync_aggregate")};
    u.sync_aggregate.bits = Bytes<64>(Field(agg, "sync_committee_bits"), "sync_committee_bits");
    u.sync_aggregate.signature = Bytes<96>(Field(agg, "sync_committee_signature"), "sync_committee_signature");
    u.signature_slot = Num(Field(v, "signature_slot"), "signature_slot");
    return u;
}

//! config.json: chain constants + the trust decisions of the run.
inline LightClientConfig ParseConfig(const UniValue& v)
{
    LightClientConfig cfg;
    cfg.genesis_validators_root = Root(Field(v, "genesis_validators_root"), "genesis_validators_root");
    const UniValue& forks{Field(v, "forks")};
    if (!forks.isArray() || forks.empty()) throw std::runtime_error("empty fork schedule");
    for (size_t i = 0; i < forks.size(); ++i) {
        ForkVersion f;
        f.epoch = Num(Field(forks[i], "epoch"), "fork epoch");
        f.version = Bytes<4>(Field(forks[i], "version"), "fork version");
        cfg.forks.push_back(f);
    }
    cfg.electra_epoch = Num(Field(v, "electra_epoch"), "electra_epoch");
    if (!v.find_value("min_participants").isNull()) {
        cfg.min_participants = static_cast<unsigned>(Num(v.find_value("min_participants"), "min_participants"));
    }
    return cfg;
}

inline std::string EthereumHex(const uint256& value)
{
    return "0x" + HexStr(value);
}

template <size_t N>
inline std::string EthereumHex(const std::array<unsigned char, N>& value)
{
    return "0x" + HexStr(value);
}

//! Convert the SSZ uint256 chunk's little-endian bytes to the decimal form
//! used by the beacon API for base_fee_per_gas.
inline std::string Uint256LEToDecimal(const uint256& value)
{
    std::array<unsigned char, 32> number{};
    std::copy(value.begin(), value.end(), number.begin());
    std::string reversed;
    do {
        unsigned remainder{0};
        bool nonzero{false};
        for (size_t i{number.size()}; i-- > 0;) {
            const unsigned dividend{remainder * 256U + number[i]};
            number[i] = static_cast<unsigned char>(dividend / 10U);
            remainder = dividend % 10U;
            nonzero |= number[i] != 0;
        }
        reversed.push_back(static_cast<char>('0' + remainder));
        if (!nonzero) break;
    } while (true);
    return {reversed.rbegin(), reversed.rend()};
}

inline UniValue BeaconJson(const ssz::BeaconBlockHeader& header)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("slot", header.slot);
    out.pushKV("proposer_index", header.proposer_index);
    out.pushKV("parent_root", EthereumHex(header.parent_root));
    out.pushKV("state_root", EthereumHex(header.state_root));
    out.pushKV("body_root", EthereumHex(header.body_root));
    return out;
}

inline UniValue ExecutionJson(const ssz::ExecutionPayloadHeader& header)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("parent_hash", EthereumHex(header.parent_hash));
    out.pushKV("fee_recipient", EthereumHex(header.fee_recipient));
    out.pushKV("state_root", EthereumHex(header.state_root));
    out.pushKV("receipts_root", EthereumHex(header.receipts_root));
    out.pushKV("logs_bloom", EthereumHex(header.logs_bloom));
    out.pushKV("prev_randao", EthereumHex(header.prev_randao));
    out.pushKV("block_number", header.block_number);
    out.pushKV("gas_limit", header.gas_limit);
    out.pushKV("gas_used", header.gas_used);
    out.pushKV("timestamp", header.timestamp);
    out.pushKV("extra_data", "0x" + HexStr(header.extra_data));
    out.pushKV("base_fee_per_gas", Uint256LEToDecimal(header.base_fee_per_gas));
    out.pushKV("block_hash", EthereumHex(header.block_hash));
    out.pushKV("transactions_root", EthereumHex(header.transactions_root));
    out.pushKV("withdrawals_root", EthereumHex(header.withdrawals_root));
    out.pushKV("blob_gas_used", header.blob_gas_used);
    out.pushKV("excess_blob_gas", header.excess_blob_gas);
    return out;
}

inline UniValue HeaderJson(const LightClientHeader& header)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("beacon", BeaconJson(header.beacon));
    out.pushKV("execution", ExecutionJson(header.execution));
    UniValue branch{UniValue::VARR};
    for (const uint256& node : header.execution_branch) {
        branch.push_back(EthereumHex(node));
    }
    out.pushKV("execution_branch", std::move(branch));
    return out;
}

inline UniValue CommitteeJson(const ssz::SyncCommittee& committee)
{
    UniValue out{UniValue::VOBJ};
    UniValue pubkeys{UniValue::VARR};
    for (const auto& pubkey : committee.pubkeys) {
        pubkeys.push_back(EthereumHex(pubkey));
    }
    out.pushKV("pubkeys", std::move(pubkeys));
    out.pushKV("aggregate_pubkey", EthereumHex(committee.aggregate_pubkey));
    return out;
}

inline UniValue StoreJson(const LightClientStore& store)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("finalized_header", HeaderJson(store.finalized_header));
    out.pushKV("period", store.period);
    out.pushKV("current_sync_committee", CommitteeJson(store.current));
    if (store.next) {
        out.pushKV("next_sync_committee", CommitteeJson(*store.next));
    }
    return out;
}

/** Parse a complete store exported by B3. Unlike a beacon bootstrap, this is
 * trusted because its enclosing B3 connection is finalized. Still reject
 * malformed structure, an unproven execution header, or a forged period so a
 * corrupt/stale operator file cannot silently alter update verification. */
inline LightClientStore ParseStore(const UniValue& v)
{
    if (!v.isObject()) throw std::runtime_error("light-client store is not an object");
    LightClientStore store;
    store.finalized_header = ParseHeader(Field(v, "finalized_header"));
    store.period = Num(Field(v, "period"), "period");
    store.current = ParseCommittee(Field(v, "current_sync_committee"));
    const UniValue& next{v.find_value("next_sync_committee")};
    if (!next.isNull()) store.next = ParseCommittee(next);
    if (store.period != PeriodAtSlot(store.finalized_header.beacon.slot)) {
        throw std::runtime_error("store period does not match finalized beacon slot");
    }
    if (!store.finalized_header.VerifyExecution()) {
        throw std::runtime_error("store finalized execution proof is invalid");
    }
    return store;
}

inline uint256 B3DisplayHash(const UniValue& v, const std::string& name)
{
    if (!v.isStr()) throw std::runtime_error("field not a string: " + name);
    std::string hex{v.get_str()};
    if (hex.rfind("0x", 0) == 0 || hex.rfind("0X", 0) == 0) hex.erase(0, 2);
    const auto hash{uint256::FromHex(hex)};
    if (!hash || hash->IsNull()) throw std::runtime_error("bad B3 hash: " + name);
    return *hash;
}

struct LightClientStoreSnapshot {
    LightClientStore store{};
    uint64_t connection_height{0};
    uint256 connection_block_hash{};
    uint64_t b3_finalized_height{0};
    uint256 b3_finalized_block_hash{};
};

inline UniValue StoreSnapshotJson(const LightClientStore& store,
                                  uint64_t connection_height,
                                  const uint256& connection_block_hash,
                                  uint64_t b3_finalized_height,
                                  const uint256& b3_finalized_block_hash)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("version", 1);
    UniValue connection{UniValue::VOBJ};
    connection.pushKV("height", connection_height);
    connection.pushKV("block_hash", connection_block_hash.GetHex());
    out.pushKV("connection", std::move(connection));
    UniValue finalized{UniValue::VOBJ};
    finalized.pushKV("height", b3_finalized_height);
    finalized.pushKV("block_hash", b3_finalized_block_hash.GetHex());
    out.pushKV("b3_finalized", std::move(finalized));
    out.pushKV("store", StoreJson(store));
    return out;
}

inline LightClientStoreSnapshot ParseStoreSnapshot(const UniValue& v)
{
    if (!v.isObject()) throw std::runtime_error("store snapshot is not an object");
    if (Num(Field(v, "version"), "version") != 1) {
        throw std::runtime_error("unsupported store snapshot version");
    }
    const UniValue& connection{Field(v, "connection")};
    const UniValue& finalized{Field(v, "b3_finalized")};
    LightClientStoreSnapshot out;
    out.connection_height = Num(Field(connection, "height"), "connection.height");
    out.connection_block_hash = B3DisplayHash(
        Field(connection, "block_hash"), "connection.block_hash");
    out.b3_finalized_height = Num(
        Field(finalized, "height"), "b3_finalized.height");
    out.b3_finalized_block_hash = B3DisplayHash(
        Field(finalized, "block_hash"), "b3_finalized.block_hash");
    if (out.connection_height > out.b3_finalized_height) {
        throw std::runtime_error("light-client connection is not B3-finalized");
    }
    out.store = ParseStore(Field(v, "store"));
    return out;
}

} // namespace lcjson
} // namespace bridge

#endif // B3COIN_BRIDGE_LC_JSON_H
