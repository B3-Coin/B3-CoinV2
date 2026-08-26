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

} // namespace lcjson
} // namespace bridge

#endif // B3COIN_BRIDGE_LC_JSON_H
