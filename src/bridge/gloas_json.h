// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license.
#ifndef B3COIN_BRIDGE_GLOAS_JSON_H
#define B3COIN_BRIDGE_GLOAS_JSON_H
#include <bridge/gloas_peer_light_client.h>
#include <bridge/lc_json.h>
#include <set>
#include <stdexcept>
#include <string_view>
namespace bridge::peer::gloas::json {
inline constexpr size_t MAX_JSON_BYTES{262'144};
inline void Object(const UniValue& value,std::initializer_list<std::string_view> required,std::initializer_list<std::string_view> optional={}) {
    if (!value.isObject()) throw std::runtime_error("INVALID_JSON_SHAPE");
    std::set<std::string> seen;
    for (const auto& key:value.getKeys()) if (!seen.insert(key).second ||
        (std::find(required.begin(),required.end(),key)==required.end() && std::find(optional.begin(),optional.end(),key)==optional.end())) throw std::runtime_error("INVALID_JSON_FIELDS");
    for (const auto& key:required) if (!seen.contains(std::string{key})) throw std::runtime_error("MISSING_JSON_FIELD");
}
inline void NoDuplicates(const UniValue& value,size_t depth=0) {
    if (depth>16 || ((value.isObject() || value.isArray()) && value.size()>2048)) throw std::runtime_error("JSON_DEPTH_OR_ARRAY_LIMIT");
    if (value.isObject()) { std::set<std::string> keys; for (const auto& k:value.getKeys()) if (!keys.insert(k).second) throw std::runtime_error("DUPLICATE_JSON_FIELD"); }
    if (value.isObject() || value.isArray()) for (size_t i{0};i<value.size();++i) NoDuplicates(value[i],depth+1);
}
inline UniValue Read(std::string_view bytes) {
    if (bytes.empty() || bytes.size()>MAX_JSON_BYTES) throw std::runtime_error("JSON_SIZE_LIMIT");
    bool quoted{false},escaped{false}; int depth{0};
    for (const char c:bytes) {
        if (quoted) { if (escaped) escaped=false; else if (c=='\\') escaped=true; else if (c=='"') quoted=false; }
        else if (c=='"') quoted=true;
        else if (c=='[' || c=='{') { if (++depth>16) throw std::runtime_error("JSON_DEPTH_LIMIT"); }
        else if (c==']' || c=='}') { if (--depth<0) throw std::runtime_error("INVALID_JSON"); }
    }
    UniValue out; if (!out.read(bytes)) throw std::runtime_error("INVALID_JSON"); NoDuplicates(out); return out;
}
inline Policy ParsePolicy(const UniValue& v) {
    Object(v,{"format","mainnet_enabled","chain_id","genesis_validators_root","genesis_time","seconds_per_slot",
              "gloas_fork_epoch","gloas_fork_version","trusted_root","trusted_slot","fork_schedule_valid_through_epoch",
              "max_sync_lag_slots","max_recovery_lag_slots","min_participants"});
    if (v["format"].get_str()!="b3-ethereum-sepolia-gloas-policy/1" || !v["mainnet_enabled"].isBool() || v["mainnet_enabled"].get_bool() ||
        !v["chain_id"].isNum() || v["chain_id"].getInt<uint64_t>()!=CHAIN_ID ||
        lcjson::Root(v["genesis_validators_root"],"genesis_validators_root")!=uint256{std::span{GENESIS_VALIDATORS_ROOT}} ||
        lcjson::Num(v["genesis_time"],"genesis_time")!=GENESIS_TIME || lcjson::Num(v["seconds_per_slot"],"seconds_per_slot")!=12 ||
        lcjson::Num(v["gloas_fork_epoch"],"gloas_fork_epoch")!=FORK_EPOCH ||
        lcjson::Bytes<4>(v["gloas_fork_version"],"gloas_fork_version")!=FORK_VERSION) throw std::runtime_error("DISABLED_PINNED_SEPOLIA_GLOAS_POLICY_REQUIRED");
    const auto participants{lcjson::Num(v["min_participants"],"min_participants")};
    if (participants<342 || participants>512) throw std::runtime_error("INVALID_POLICY_PARTICIPATION");
    Policy p; p.trusted_root=lcjson::Root(v["trusted_root"],"trusted_root"); p.trusted_slot=lcjson::Num(v["trusted_slot"],"trusted_slot");
    p.fork_schedule_valid_through_epoch=lcjson::Num(v["fork_schedule_valid_through_epoch"],"fork_schedule_valid_through_epoch");
    p.max_sync_lag_slots=lcjson::Num(v["max_sync_lag_slots"],"max_sync_lag_slots");
    p.max_recovery_lag_slots=lcjson::Num(v["max_recovery_lag_slots"],"max_recovery_lag_slots"); p.min_participants=participants;
    if (!p.Valid()) throw std::runtime_error("INVALID_POLICY"); return p;
}
inline Header ParseHeader(const UniValue& v) {
    Object(v,{"beacon","execution_block_hash","execution_branch"});
    const auto& b{v["beacon"]}; Object(b,{"slot","proposer_index","parent_root","state_root","body_root"});
    Header h; h.beacon.slot=lcjson::Num(b["slot"],"slot"); h.beacon.proposer_index=lcjson::Num(b["proposer_index"],"proposer_index");
    h.beacon.parent_root=lcjson::Root(b["parent_root"],"parent_root"); h.beacon.state_root=lcjson::Root(b["state_root"],"state_root");
    h.beacon.body_root=lcjson::Root(b["body_root"],"body_root"); h.execution_block_hash=lcjson::Root(v["execution_block_hash"],"execution_block_hash");
    h.execution_branch=lcjson::Branch(v["execution_branch"],"execution_branch");
    if (h.execution_branch.size()!=11) throw std::runtime_error("INVALID_EXECUTION_BRANCH"); return h;
}
inline ssz::SyncCommittee Committee(const UniValue& v) {
    Object(v,{"pubkeys","aggregate_pubkey"}); return lcjson::ParseCommittee(v);
}
inline Bytes ParseRecord(const UniValue& v) {
    Object(v,{"kind","data"}); const auto kind{v["kind"].get_str()}; const auto& d{v["data"]}; Record record;
    if (kind=="bootstrap") {
        Object(d,{"header","current_sync_committee","current_sync_committee_branch"});
        record=Bootstrap{ParseHeader(d["header"]),Committee(d["current_sync_committee"]),lcjson::Branch(d["current_sync_committee_branch"],"current_sync_committee_branch")};
    } else if (kind=="update" || kind=="finality_update") {
        Object(d,{"attested_header","finalized_header","finality_branch","sync_aggregate","signature_slot"},{"next_sync_committee","next_sync_committee_branch"});
        Update u; u.attested=ParseHeader(d["attested_header"]); u.finalized=ParseHeader(d["finalized_header"]);
        u.finality_branch=lcjson::Branch(d["finality_branch"],"finality_branch");
        u.has_next=!d.find_value("next_sync_committee").isNull();
        if (u.has_next!=!d.find_value("next_sync_committee_branch").isNull() || (kind=="finality_update" && u.has_next)) throw std::runtime_error("INVALID_NEXT_COMMITTEE");
        if (u.has_next) { u.next=Committee(d["next_sync_committee"]); u.next_branch=lcjson::Branch(d["next_sync_committee_branch"],"next_sync_committee_branch"); }
        const auto& a{d["sync_aggregate"]}; Object(a,{"sync_committee_bits","sync_committee_signature"});
        u.sync.bits=lcjson::Bytes<64>(a["sync_committee_bits"],"sync_committee_bits");
        u.sync.signature=lcjson::Bytes<96>(a["sync_committee_signature"],"sync_committee_signature");
        u.signature_slot=lcjson::Num(d["signature_slot"],"signature_slot"); record=std::move(u);
    } else throw std::runtime_error("UNSUPPORTED_PROOF_KIND");
    const auto bytes{EncodeRecord(record)}; if (!bytes) throw std::runtime_error("CANONICAL_RECORD_LIMIT"); return *bytes;
}
} // namespace bridge::peer::gloas::json
#endif
