// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license.
// Separate disabled-mainnet Sepolia Gloas proof stream consumer.
#include <bridge/gloas_json.h>
#include <bridge/gloas_journal.h>
#include <chrono>
#include <cstdio>
#include <iostream>
using namespace bridge;
namespace gl=bridge::peer::gloas;
namespace {
uint64_t CurrentSlot() {
    const auto seconds{std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()};
    if (seconds<0 || static_cast<uint64_t>(seconds)<gl::GENESIS_TIME) throw std::runtime_error("LOCAL_CLOCK_BEFORE_GENESIS");
    return (static_cast<uint64_t>(seconds)-gl::GENESIS_TIME)/12;
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

void Emit(const gl::LightClient& client,const char* result,bool accepted,const std::optional<gl::Bytes>& raw) {
    const auto now{CurrentSlot()};
    UniValue out{UniValue::VOBJ};
    out.pushKV("format","b3-ethereum-sepolia-gloas-verification/1"); out.pushKV("result",result); out.pushKV("accepted",accepted);
    out.pushKV("mainnet_enabled",false); out.pushKV("arbitrum_authority",false); out.pushKV("writes_enabled",false);
    out.pushKV("chain_id",gl::CHAIN_ID); out.pushKV("genesis_validators_root",lcjson::EthereumHex(gl::GENESIS_VALIDATORS_ROOT));
    out.pushKV("sequence",client.Sequence()); out.pushKV("policy_sha256",HexStr(client.PolicyDigest()));
    out.pushKV("journal_sha256",HexStr(client.JournalDigest())); out.pushKV("fenced",client.Fenced());
    out.pushKV("trust","explicit Sepolia weak-subjectivity checkpoint; native Gloas BLS/SSZ; native EL RLP hash preimage");
    const auto hash{client.FinalizedHash(now)};
    out.pushKV("fresh_finalized_execution_hash",hash.has_value());
    if (hash) {
        out.pushKV("ethereum_finalized_execution_hash",lcjson::EthereumHex(*hash));
        out.pushKV("beacon_slot",client.State()->finalized.beacon.slot);
    }
    const auto anchor{raw ? client.Latest(now,*raw) : std::nullopt};
    out.pushKV("fresh_finalized_execution",anchor.has_value());
    if (anchor) {
        UniValue h{UniValue::VOBJ}; h.pushKV("beacon_slot",anchor->BeaconSlot()); h.pushKV("block_number",anchor->Header().block_number);
        h.pushKV("block_hash",lcjson::EthereumHex(anchor->Header().block_hash)); h.pushKV("state_root",lcjson::EthereumHex(anchor->Header().state_root));
        h.pushKV("receipts_root",lcjson::EthereumHex(anchor->Header().receipts_root)); out.pushKV("ethereum_finalized_execution",std::move(h));
    }
    std::cout<<out.write()<<'\n'<<std::flush;
}
} // namespace
int main(int argc,char** argv) {
#ifdef WIN32
    std::fprintf(stderr,"UNSUPPORTED_DURABLE_PLATFORM: run under Linux/WSL\n"); return 2;
#else
    if (argc!=5 || std::string_view{argv[1]}!="--policy" || std::string_view{argv[3]}!="--journal") {
        std::fprintf(stderr,"usage: b3-bridge-gloascheck --policy /absolute/sepolia-policy.json --journal /absolute/new-gloas.journal\n"); return 2;
    }
    try {
        gl::journal::File policy_file{argv[2],false}; const auto policy{gl::json::ParsePolicy(gl::json::Read(policy_file.ReadJson()))};
        gl::LightClient client{policy}; gl::journal::File journal{argv[4],true}; gl::journal::Replay(journal,client);
        std::optional<gl::Bytes> raw;
        Emit(client,"READY",false,raw);
        for (;;) {
            std::string line; line.reserve(64000); bool overflow{false}; char c{};
            while (std::cin.get(c) && c!='\n') { if (line.size()<gl::json::MAX_JSON_BYTES) line.push_back(c); else overflow=true; }
            if (line.empty() && !overflow && std::cin.eof()) break;
            if (!std::cin.eof() && std::cin.fail()) throw std::runtime_error("INPUT_READ_FAILED");
            if (overflow) { Emit(client,"JSON_SIZE_LIMIT",false,raw); continue; }
            try {
                const auto v{gl::json::Read(line)}; gl::json::Object(v,{"kind","data"});
                if (v["kind"].isStr() && v["kind"].get_str()=="execution_header") {
                    auto bytes{lcjson::HexField(v["data"],"data")};
                    if (bytes.empty() || bytes.size()>gl::MAX_EL_HEADER_BYTES || !client.Latest(CurrentSlot(),bytes)) Emit(client,"BAD_EXECUTION_PREIMAGE",false,raw);
                    else { raw=std::move(bytes); Emit(client,"EXECUTION_PREIMAGE_VERIFIED",true,raw); }
                } else {
                    const auto record{gl::json::ParseRecord(v)};
                    const auto reply{client.Submit(record,CurrentSlot(),[&](auto bytes) { return journal.Append(bytes); })};
                    Emit(client,Name(reply.result),reply.result==peer::Result::OK,raw);
                    if (client.Fenced()) return 1;
                }
            } catch (const std::exception&) { Emit(client,"MALFORMED_PEER_PROOF",false,raw); }
            if (std::cin.eof()) break;
        }
        Emit(client,"READY",false,raw); return client.Fenced() ? 1 : 0;
    } catch (const std::exception& e) { std::fprintf(stderr,"Sepolia Gloas startup refused: %s\n",e.what()); return 1; }
#endif
}
