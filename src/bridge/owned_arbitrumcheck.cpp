// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Owned-regtest Nitro contract-settlement and complete history checker.
// The explicit owner-trusted L1 fullnode checkpoint authenticates the state
// root. No public Ethereum PoS, public Arbitrum or Nitro execution claim.
#include <bridge/owned_arbitrum_settlement.h>
#include <crypto/sha256.h>
#include <filesystem>
#include <fstream>
#include <univalue.h>
#include <util/strencodings.h>

#include <array>
#include <charconv>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

#ifdef WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace arb = bridge::arbitrum;
namespace hist = arb::history;
namespace codec = arb::proof_codec;
namespace owned = arb::owned;
namespace {
template <typename T> std::string Hex(const T& value) { return "0x" + HexStr(value); }
uint64_t Number(const std::string& text)
{
    uint64_t value{0};
    const auto result{std::from_chars(text.data(), text.data() + text.size(), value)};
    if (text.empty() || (text.size() > 1 && text[0] == '0') || result.ec != std::errc{} ||
        result.ptr != text.data() + text.size()) throw std::runtime_error("INVALID_RANGE_HEIGHT");
    return value;
}
arb::Bytes Bytes(const std::string& text, size_t size)
{
    if (text.size() != 2 + size * 2 || text.substr(0, 2) != "0x" ||
        text.find_first_not_of("0123456789abcdef", 2) != std::string::npos)
        throw std::runtime_error("CANONICAL_LOWERCASE_HASH_REQUIRED");
    const auto bytes{TryParseHex<unsigned char>(text.substr(2))};
    if (!bytes || bytes->size() != size) throw std::runtime_error("INVALID_HASH_LENGTH");
    return *bytes;
}
std::string Decimal(const bridge::StorageWord& word)
{
    std::string out{"0"};
    for (const auto byte : word) {
        unsigned carry{byte};
        for (auto i = out.rbegin(); i != out.rend(); ++i) {
            const auto value{static_cast<unsigned>(*i - '0') * 256 + carry};
            *i = '0' + value % 10;
            carry = value / 10;
        }
        while (carry) { out.insert(out.begin(), '0' + carry % 10); carry /= 10; }
    }
    return out;
}
UniValue Base(const std::string& result, bool matches)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("format","b3-owned-arbitrum-settlement-check/1");
    out.pushKV("result",result); out.pushKV("commitments_match",matches);
    out.pushKV("owned_settlement_verified",matches);out.pushKV("rollup_settlement_verified",matches);
    out.pushKV("public_arbitrum_authority",false);out.pushKV("arbitrum_authority",false);
    out.pushKV("ethereum_pos_verified",false);out.pushKV("execution_verified",false);
    out.pushKV("mainnet_enabled",false);out.pushKV("checkpoint_trust","explicit-owner-trusted-owned-regtest-fullnode-hash");
    out.pushKV("l1_chain_id",1337);out.pushKV("l2_chain_id",412346);
    return out;
}
UniValue Accounts(const std::vector<hist::AccountValue>& values)
{
    UniValue out{UniValue::VARR};
    for (const auto& value : values) {
        UniValue account{UniValue::VOBJ}, slots{UniValue::VARR};
        account.pushKV("address", Hex(value.address));
        account.pushKV("nonce", std::to_string(value.account.nonce));
        account.pushKV("balance", Decimal(value.account.balance));
        account.pushKV("code_hash", Hex(value.account.code_hash));
        account.pushKV("storage_root", Hex(value.account.storage_root));
        for (const auto& storage : value.storage) {
            UniValue slot{UniValue::VOBJ};
            slot.pushKV("slot", Hex(storage.slot)); slot.pushKV("value", Hex(storage.value));
            slots.push_back(std::move(slot));
        }
        account.pushKV("storage", std::move(slots)); out.push_back(std::move(account));
    }
    return out;
}
}


namespace {
arb::Bytes Read(const std::filesystem::path& path,size_t limit)
{
    std::ifstream file{path,std::ios::binary};if(!file)throw std::runtime_error("FILE_REQUIRED");
    arb::Bytes out;std::array<char,8192> buffer;
    while(file){file.read(buffer.data(),buffer.size());const auto n=static_cast<size_t>(file.gcount());if(n>limit-out.size())throw std::runtime_error("FILE_LIMIT");out.insert(out.end(),buffer.begin(),buffer.begin()+n);}
    if(!file.eof())throw std::runtime_error("FILE_READ_FAILED");return out;
}
UniValue Json(const arb::Bytes& bytes)
{UniValue out;if(!out.read(std::string{bytes.begin(),bytes.end()})||!out.isObject())throw std::runtime_error("JSON_OBJECT_REQUIRED");return out;}
uint256 Hash(const UniValue& value)
{return uint256{Bytes(value.get_str(),32)};}
bridge::StorageWord Word(const UniValue& value)
{const auto bytes{Bytes(value.get_str(),32)};bridge::StorageWord out;std::copy(bytes.begin(),bytes.end(),out.begin());return out;}
bridge::EthAddress Address(const UniValue& value)
{const auto bytes{Bytes(value.get_str(),20)};bridge::EthAddress out;std::copy(bytes.begin(),bytes.end(),out.begin());return out;}
std::string Sha(const arb::Bytes& bytes)
{std::array<unsigned char,32> hash;CSHA256().Write(bytes.data(),bytes.size()).Finalize(hash.data());return Hex(hash);}
owned::Policy ParsePolicy(const UniValue& doc,const uint256& l1,const uint256& l2)
{
    if(doc["format"].get_str()!="b3-owned-nitro-deployment-policy/1"||doc["mainnet_enabled"].get_bool()||
       doc["contracts_source_commit"].get_str()!="4341b132cfbdcc980ead03765ca5224ff6cb5d97"||
       Word(doc["latest_confirmed_slot"])!=bridge::SlotWord(116)||Word(doc["assertions_mapping_slot"])!=bridge::SlotWord(117))throw std::runtime_error("UNSUPPORTED_OWNED_POLICY");
    static const std::array<std::string_view,16> roles{"rollup","rollup-admin","rollup-user","governance-proxy","governance-logic","challenge-proxy","challenge-logic","bridge-proxy","bridge-logic","inbox-proxy","inbox-logic","outbox-proxy","outbox-logic","sequencer-proxy","sequencer-logic","core-proxy-admin"};
    const auto& values{doc["code_accounts"].get_array()};if(values.size()!=16)throw std::runtime_error("EXACT_OWNED_GRAPH_REQUIRED");
    owned::Policy policy;policy.l1_genesis=l1;policy.l2_genesis=l2;policy.deployment_block=doc["deployment"]["deployed-at"].getInt<uint64_t>();
    for(size_t i=0;i<16;++i){const auto& value{values[i]};if(value["role"].get_str()!=roles[i])throw std::runtime_error("EXACT_GRAPH_ROLE_ORDER_REQUIRED");
        owned::Pin pin;pin.address=Address(value["address"]);pin.code_hash=Hash(value["code_keccak256"]);
        const auto& slots{value["storage"].get_array()};if(slots.size()>arb::MAX_STORAGE)throw std::runtime_error("STORAGE_LIMIT");
        for(const auto& s:slots.getValues())if(!pin.storage.emplace(Word(s["slot"]),Word(s["value"])).second)throw std::runtime_error("DUPLICATE_POLICY_SLOT");
        policy.accounts.push_back(std::move(pin));}
    const auto& aliases{doc["account_role_aliases"].get_array()};if(aliases.size()!=1||aliases[0]["role"].get_str()!="governance-proxy-admin"||aliases[0]["same_account_as"].get_str()!="core-proxy-admin"||Address(aliases[0]["address"])!=policy.accounts[15].address)throw std::runtime_error("EXACT_GRAPH_ALIAS_REQUIRED");
    const auto latest{policy.accounts[0].storage.find(bridge::SlotWord(116))};if(latest==policy.accounts[0].storage.end())throw std::runtime_error("BASELINE_ASSERTION_REQUIRED");
    const auto node{owned::AssertionSlot(uint256{std::span<const unsigned char>{latest->second}})};
    if(Word(doc["assertion_node_slot"])!=node)throw std::runtime_error("WRONG_BASELINE_ASSERTION_SLOT");
    policy.accounts[0].storage.erase(bridge::SlotWord(116));
    if(policy.accounts[0].storage.erase(node)!=1||policy.accounts[0].storage.erase(owned::Next(node))!=1)throw std::runtime_error("BASELINE_NODE_REQUIRED");
    if(Address(doc["rollup"])!=policy.accounts[0].address||!owned::ValidPolicy(policy))throw std::runtime_error("INVALID_TRUSTED_OWNED_POLICY");
    return policy;
}
}

int main(int argc,char**argv)
{
    try{
        if(argc==2&&std::string{argv[1]}=="--help"){
            std::cout<<"Owned REGTEST Nitro settlement checker. Explicit owner-trusted L1 checkpoint; no public PoS authority.\n"
                "--regtest --export DIR --trusted-policy FILE --trusted-policy-sha256 0xHASH --trusted-l1-head 0xHASH\n"
                "--l1-genesis 0xHASH --l2-genesis 0xHASH --previous-height DEC --previous-hash 0xHASH --to-height DEC --to-hash 0xHASH [--vault 0xADDRESS]\n";return 0;}
        std::map<std::string,std::string> args;std::vector<bridge::EthAddress> vaults;bool regtest=false;
        const std::set<std::string> keys{"--export","--trusted-policy","--trusted-policy-sha256","--trusted-l1-head","--l1-genesis","--l2-genesis","--previous-height","--previous-hash","--to-height","--to-hash"};
        for(int i=1;i<argc;){const std::string key{argv[i++]};if(key=="--regtest"){if(regtest)throw std::runtime_error("DUPLICATE_REGTEST");regtest=true;continue;}
            if(i==argc)throw std::runtime_error("ARGUMENT_VALUE_REQUIRED");const std::string value{argv[i++]};
            if(key=="--vault"){if(vaults.size()>=arb::MAX_ACCOUNTS)throw std::runtime_error("VAULT_LIMIT");UniValue v{value};vaults.push_back(Address(v));}
            else if(!keys.contains(key)||!args.emplace(key,value).second)throw std::runtime_error("UNKNOWN_OR_DUPLICATE_ARGUMENT");}
        if(!regtest||args.size()!=keys.size())throw std::runtime_error("EXPLICIT_OWNED_REGTEST_ARGUMENTS_REQUIRED");
        const std::filesystem::path root{args.at("--export")};
        const auto policy_bytes{Read(args.at("--trusted-policy"),2*1024*1024)};
        if(Sha(policy_bytes)!=args.at("--trusted-policy-sha256"))throw std::runtime_error("TRUSTED_POLICY_BYTES_CHANGED");
        owned::Input input;input.regtest=true;input.l1_genesis=uint256{Bytes(args.at("--l1-genesis"),32)};input.l2_genesis=uint256{Bytes(args.at("--l2-genesis"),32)};
        input.trusted_l1_head=uint256{Bytes(args.at("--trusted-l1-head"),32)};
        input.request={Number(args.at("--previous-height")),Number(args.at("--to-height")),uint256{Bytes(args.at("--previous-hash"),32)},uint256{Bytes(args.at("--to-hash"),32)}};
        const auto policy{ParsePolicy(Json(policy_bytes),input.l1_genesis,input.l2_genesis)};
        const auto manifest{Json(Read(root/"manifest.json",8*1024*1024))};
        if(manifest["format"].get_str()!="b3-owned-nitro-settled-export/1"||manifest["mainnet_enabled"].get_bool()||
           manifest["source"]["kind"].get_str()!="owned-regtest"||manifest["source"]["chain_id"].getInt<uint64_t>()!=owned::L2_CHAIN_ID||
           manifest["l1"]["chain_id"].getInt<uint64_t>()!=owned::L1_CHAIN_ID||Hash(manifest["source"]["genesis_hash"])!=input.l2_genesis||
           Hash(manifest["l1"]["genesis_hash"])!=input.l1_genesis||Hash(manifest["l1"]["hash"])!=input.trusted_l1_head)throw std::runtime_error("WRONG_OWNED_EXPORT_CONTEXT");
        const auto& request{manifest["request"]};
        if(request["previous_height"].getInt<uint64_t>()!=input.request.previous_height||request["to_height"].getInt<uint64_t>()!=input.request.to_height||Hash(request["previous_hash"])!=input.request.previous_hash||Hash(request["to_hash"])!=input.request.to_hash)throw std::runtime_error("WRONG_EXPORT_CURSOR");
        input.l1_header=Read(root/"l1-header.rlp",arb::MAX_HEADER_BYTES);
        const auto settlement{codec::DecodeProofV1(Read(root/"settlement-proof.bin",arb::MAX_PROOF_BYTES))};
        const auto history{codec::DecodeProofV1(Read(root/"history-proof.bin",arb::MAX_PROOF_BYTES))};
        if(!settlement.proof||!history.proof)throw std::runtime_error("INVALID_PROOF_WIRE");
        input.settlement=*settlement.proof;input.history=*history.proof;
        const auto ancestors{Json(Read(root/"ancestor-pages.json",8*1024*1024))};
        if(ancestors["format"].get_str()!="b3-owned-nitro-ancestor-pages/1"||ancestors["pages"].size()>64)throw std::runtime_error("ANCESTRY_FORMAT_OR_LIMIT");
        size_t total{0};for(const auto& page:ancestors["pages"].get_array().getValues()){
            if(!page.isArray()||!page.size()||page.size()>arb::MAX_HEADERS)throw std::runtime_error("ANCESTRY_LIMIT");std::vector<arb::Bytes> raw;
            for(const auto& record:page.getValues()){const auto name{record["header_sha256"].get_str()};
                if(name.size()!=64||name.find_first_not_of("0123456789abcdef")!=std::string::npos)throw std::runtime_error("INVALID_OBJECT_REFERENCE");
                auto header{Read(root/"objects"/name,arb::MAX_HEADER_BYTES)};
                if(Sha(header)!="0x"+name||header.size()>arb::MAX_PROOF_BYTES-total)throw std::runtime_error("ANCESTRY_OBJECT_OR_LIMIT");total+=header.size();raw.push_back(std::move(header));}
            input.ancestor_pages.push_back(std::move(raw));}
        const auto checked{owned::Verifier::Verify(policy,input)};
        if(!checked.evidence){std::cout<<Base(checked.error,false).write()<<'\n';return 2;}
        const auto& evidence{checked.evidence->History()};const auto& range{input.request};
        const auto events{hist::ExtractEvents(evidence,vaults)};if(!events){std::cout<<Base("BAD_CANDIDATE_EVENT",false).write()<<'\n';return 2;}
        auto out{Base("OK",true)};out.pushKV("trusted_policy_sha256",Sha(policy_bytes));out.pushKV("policy_digest",Hex(checked.evidence->PolicyDigest()));
        out.pushKV("l1_checkpoint",Hex(input.trusted_l1_head));out.pushKV("l1_genesis",Hex(input.l1_genesis));out.pushKV("l2_genesis",Hex(input.l2_genesis));
        out.pushKV("settled_height",std::to_string(checked.evidence->SettledHead().number));out.pushKV("settled_hash",Hex(checked.evidence->SettledHead().hash));
        out.pushKV("proof_digest",Hex(history.digest));out.pushKV("previous_height",std::to_string(range.previous_height));out.pushKV("previous_hash",Hex(range.previous_hash));
        out.pushKV("to_height",std::to_string(range.to_height));out.pushKV("to_hash",Hex(range.to_hash));out.pushKV("header_count",evidence.headers.size());out.pushKV("receipt_count",evidence.receipts.size());
        out.pushKV("newest_accounts",Accounts(evidence.newest_accounts));out.pushKV("oldest_accounts",Accounts(evidence.oldest_accounts));
        UniValue records{UniValue::VARR};for(const auto& event:*events){UniValue record{UniValue::VOBJ};
            record.pushKV("kind",event.kind==hist::EventKind::DEPOSIT?"deposit":"paid");record.pushKV("vault",Hex(event.vault));record.pushKV("block_number",std::to_string(event.block_number));record.pushKV("block_hash",Hex(event.block_hash));
            record.pushKV("transaction_index",std::to_string(event.transaction_index));record.pushKV("transaction_hash",Hex(event.transaction_hash));record.pushKV("log_index",std::to_string(event.log_index));record.pushKV("amount_raw",Decimal(event.amount));
            if(event.kind==hist::EventKind::DEPOSIT){record.pushKV("deposit_id",std::to_string(event.deposit_id));record.pushKV("account",Hex(event.account));record.pushKV("asset",Hex(event.asset));record.pushKV("sender",Hex(event.address));}
            else{record.pushKV("withdrawal_id",Hex(event.withdrawal_id));record.pushKV("recipient",Hex(event.address));}records.push_back(std::move(record));}
        out.pushKV("candidate_events",std::move(records));const auto text{out.write()};if(text.size()>8*1024*1024)throw std::runtime_error("REPORT_LIMIT");std::cout<<text<<'\n';return std::cout.good()?0:1;
    }catch(const std::exception&){std::cout<<Base("INVALID_INPUT_OR_LIMIT",false).write()<<'\n';return 2;}
}
