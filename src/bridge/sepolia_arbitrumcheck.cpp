// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Public TESTNET-only canonical contract settlement. All raw inputs are
// untrusted. Native Gloas journal replay and an exact execution-header preimage
// must mint the same-process authority used for every Arbitrum proof check.
#include <bridge/arbitrum_sepolia_availability.h>
#include <bridge/gloas_json.h>
#include <bridge/gloas_journal.h>
#include <crypto/sha256.h>
#include <chrono>
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
namespace sep = arb::sepolia;
namespace gl = bridge::peer::gloas;
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
UniValue Base(const std::string& result, bool matches=false)
{
    UniValue out{UniValue::VOBJ};out.pushKV("format","b3-arbitrum-sepolia-settlement-check/1");out.pushKV("result",result);
    out.pushKV("commitments_match",matches);out.pushKV("sepolia_contract_settlement_verified",matches);
    out.pushKV("arbitrum_authority",false);out.pushKV("mainnet_enabled",false);out.pushKV("execution_verified",false);
    out.pushKV("writes_enabled",false);out.pushKV("l1_chain_id",gl::CHAIN_ID);out.pushKV("l2_chain_id",sep::CHAIN_ID);
    out.pushKV("trust","explicit Sepolia weak-subjectivity checkpoint; native Gloas BLS/SSZ and EL preimage; reviewed contract settlement with governance and honest-challenge assumptions");
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
{UniValue out;if(!out.read(std::string{bytes.begin(),bytes.end()})||!out.isObject())throw std::runtime_error("JSON_OBJECT_REQUIRED");gl::json::NoDuplicates(out);return out;}
std::string Sha(const arb::Bytes& bytes)
{std::array<unsigned char,32> hash;CSHA256().Write(bytes.data(),bytes.size()).Finalize(hash.data());return Hex(hash);}
uint64_t CurrentSlot()
{
    const auto seconds{std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()};
    if(seconds<0||static_cast<uint64_t>(seconds)<gl::GENESIS_TIME)throw std::runtime_error("LOCAL_CLOCK_BEFORE_GENESIS");
    return (static_cast<uint64_t>(seconds)-gl::GENESIS_TIME)/12;
}
#ifndef WIN32
void ReplayBounded(gl::journal::File& file,gl::LightClient& client)
{
    size_t entries=0,total=0;
    for(;;){std::array<unsigned char,bridge::peer::ENTRY_PREFIX_SIZE> prefix{};if(!file.ReadExact(prefix,true))break;
        const auto n=prefix.size();const uint32_t size=(uint32_t{prefix[n-4]}<<24)|(uint32_t{prefix[n-3]}<<16)|(uint32_t{prefix[n-2]}<<8)|prefix[n-1];
        if(++entries>8192||size<2||size>gl::MAX_RECORD_BYTES||prefix.size()+size+32>64*1024*1024-total)throw std::runtime_error("JOURNAL_SIZE_LIMIT");
        total+=prefix.size()+size+32;arb::Bytes entry{prefix.begin(),prefix.end()};entry.resize(prefix.size()+size+32);
        file.ReadExact(std::span<unsigned char>{entry}.subspan(prefix.size()));
        if(client.ReplayEntry(entry).result!=bridge::peer::Result::OK)throw std::runtime_error("JOURNAL_VERIFICATION_FAILED");}
    if(!entries||!client.EndReplay())throw std::runtime_error("JOURNAL_EMPTY_OR_FENCED");
}
#endif
arb::Proof Proof(const std::filesystem::path& path)
{const auto decoded{codec::DecodeProofV1(Read(path,arb::MAX_PROOF_BYTES))};if(!decoded.proof)throw std::runtime_error("INVALID_PROOF_WIRE");return *decoded.proof;}
bool HeadEnvelope(const arb::Proof& proof,size_t count)
{return proof.l1_accounts.size()==count&&proof.blocks.size()==1&&proof.blocks[0].receipts.empty()&&proof.blocks[0].transactions.empty()&&proof.confirmed_accounts.empty()&&proof.historical_accounts.empty();}
bool SameAssertion(const arb::AssertionPreimage&a,const arb::AssertionPreimage&b)
{return a.parent_assertion==b.parent_assertion&&a.block_hash==b.block_hash&&a.send_root==b.send_root&&a.inbox_position==b.inbox_position&&a.position_in_message==b.position_in_message&&a.machine_status==b.machine_status&&a.end_history_root==b.end_history_root&&a.inbox_accumulator==b.inbox_accumulator;}
bridge::EthAddress Address(const std::string& value)
{const auto bytes{Bytes(value,20)};bridge::EthAddress out;std::copy(bytes.begin(),bytes.end(),out.begin());return out;}
arb::Nodes Nodes(const UniValue& value,size_t& total)
{
    if(!value.isArray()||value.size()>64)throw std::runtime_error("NODE_COUNT_LIMIT");arb::Nodes out;
    for(const auto& item:value.getValues()){
        const auto& text{item.get_str()};if(text.size()<4||text.size()>2+8192*2||text.size()%2)throw std::runtime_error("NODE_BYTE_LIMIT");
        auto bytes{Bytes(text,(text.size()-2)/2)};if(bytes.size()>arb::MAX_PROOF_BYTES-total)throw std::runtime_error("PROOF_BYTE_LIMIT");total+=bytes.size();out.push_back(std::move(bytes));}
    return out;
}
arb::AccountWitness QueryWitness(const std::filesystem::path& path)
{
    const auto doc{gl::json::Read(std::string{[&] {const auto bytes=Read(path,gl::json::MAX_JSON_BYTES);return std::string{bytes.begin(),bytes.end()};}()})};
    gl::json::Object(doc,{"address","nodes","storage"});size_t total=0;
    arb::AccountWitness out;out.address=Address(doc["address"].get_str());out.nodes=Nodes(doc["nodes"],total);
    const auto& slots{doc["storage"]};if(!slots.isArray()||slots.size()>arb::MAX_STORAGE)throw std::runtime_error("STORAGE_COUNT_LIMIT");
    for(const auto& slot:slots.getValues()){gl::json::Object(slot,{"slot","nodes"});const auto bytes{Bytes(slot["slot"].get_str(),32)};bridge::StorageWord word;std::copy(bytes.begin(),bytes.end(),word.begin());out.storage.push_back({word,Nodes(slot["nodes"],total)});}
    return out;
}
void Anchor(UniValue& out,const gl::FinalizedExecution& anchor)
{
    out.pushKV("ethereum_pos_verified",true);out.pushKV("beacon_slot",anchor.BeaconSlot());out.pushKV("sequence",anchor.Sequence());
    out.pushKV("ethereum_finalized_execution_hash",Hex(anchor.Header().block_hash));out.pushKV("ethereum_state_root",Hex(anchor.Header().state_root));
    out.pushKV("ethereum_block_number",std::to_string(anchor.Header().block_number));out.pushKV("policy_digest",Hex(anchor.PolicyDigest()));out.pushKV("journal_digest",Hex(anchor.JournalDigest()));
}
}

int main(int argc,char**argv)
{
    const char* stage="arguments";
    try{
        if(argc==2&&std::string{argv[1]}=="--help"){
            std::cout<<"Sepolia-only native Gloas-to-Arbitrum contract-settlement checker. Mainnet disabled.\n"
                "Common: --policy ABS --journal ABS --execution-header FILE\n"
                "Settlement: --head-proof FILE --head-proof-1 FILE --ancestor-pages JSON --objects DIR --history-proof FILE\n"
                "--previous-height DEC --previous-hash 0xHASH --to-height DEC --to-hash 0xHASH [--vault 0xADDRESS]\n"
                "Discovery instead: --query-latest --rollup-witness JSON (one address/nodes/storage object; not settlement authority).\n"
                "Existing journal is read-only and replayed. Wall-clock freshness applies; no verified flags or fake slot input.\n";return 0;}
#ifdef WIN32
        std::cout<<Base("UNSUPPORTED_DURABLE_PLATFORM").write()<<'\n';return 2;
#else
        bool query=false;std::map<std::string,std::string> args;std::vector<bridge::EthAddress> vaults;
        for(int i=1;i<argc;){const std::string key{argv[i++]};if(key=="--query-latest"){if(query)throw std::runtime_error("DUPLICATE_QUERY");query=true;continue;}
            if(i==argc)throw std::runtime_error("ARGUMENT_VALUE_REQUIRED");const std::string value{argv[i++]};
            if(key=="--vault"){if(vaults.size()>=arb::MAX_ACCOUNTS)throw std::runtime_error("VAULT_LIMIT");vaults.push_back(Address(value));}
            else if(!args.emplace(key,value).second)throw std::runtime_error("DUPLICATE_ARGUMENT");}
        std::set<std::string> required{"--policy","--journal","--execution-header"};
        if(query)required.insert("--rollup-witness");
        else for(const auto* key:{"--head-proof","--head-proof-1","--ancestor-pages","--objects","--history-proof","--previous-height","--previous-hash","--to-height","--to-hash"})required.insert(key);
        if(args.size()!=required.size()||(query&&!vaults.empty()))throw std::runtime_error("EXACT_ARGUMENTS_REQUIRED");for(const auto&[key,value]:args)if(!required.contains(key))throw std::runtime_error("UNKNOWN_ARGUMENT");
        stage="native_policy";gl::journal::File policy_file{args.at("--policy"),false};const auto policy{gl::json::ParsePolicy(gl::json::Read(policy_file.ReadJson()))};gl::LightClient ethereum{policy};
        stage="native_journal";gl::journal::File journal{args.at("--journal"),false};ReplayBounded(journal,ethereum);
        stage="execution_preimage";const auto raw{Read(args.at("--execution-header"),gl::MAX_EL_HEADER_BYTES)};
        const auto anchor{ethereum.Latest(CurrentSlot(),raw)};if(!anchor){std::cout<<Base("STALE_OR_BAD_EXECUTION_PREIMAGE").write()<<'\n';return 2;}
        if(query){stage="query_latest";const auto reply{sep::QueryLatestConfirmed(ethereum,*anchor,CurrentSlot(),QueryWitness(args.at("--rollup-witness")))};
            if(!reply.value){std::cout<<Base(sep::Name(reply.error)).write()<<'\n';return 2;}
            if(!reply.value->IsCurrent(ethereum,CurrentSlot()))throw std::runtime_error("STALE_QUERY");
            auto out{Base("OK")};Anchor(out,*anchor);out.pushKV("latest_confirmed_query_verified",true);out.pushKV("assertion_hash",Hex(reply.value->Hash()));out.pushKV("created_block",std::to_string(reply.value->CreatedBlock()));std::cout<<out.write()<<'\n';return std::cout.good()?0:1;}
        const arb::RangeRequest request{Number(args.at("--previous-height")),Number(args.at("--to-height")),uint256{Bytes(args.at("--previous-hash"),32)},uint256{Bytes(args.at("--to-hash"),32)}};
        stage="head_proofs";const auto first{Proof(args.at("--head-proof"))};const auto second{Proof(args.at("--head-proof-1"))};
        if(!HeadEnvelope(first,16)||!HeadEnvelope(second,1)||!SameAssertion(first.assertion,second.assertion)||first.blocks[0].header!=second.blocks[0].header)throw std::runtime_error("HEAD_GROUP_MISMATCH");
        sep::HeadProof head_proof{first.assertion,{first.l1_accounts,second.l1_accounts},first.blocks[0].header};
        const auto head{sep::Verifier::AuthenticateHead(ethereum,*anchor,CurrentSlot(),head_proof)};if(!head.value){std::cout<<Base(sep::Name(head.error)).write()<<'\n';return 2;}
        auto endpoint{sep::Verifier::BeginAncestor(ethereum,CurrentSlot(),*head.value)};if(!endpoint.value)throw std::runtime_error("STALE_HEAD");
        stage="ancestry";const auto pages{Json(Read(args.at("--ancestor-pages"),8*1024*1024))};gl::json::Object(pages,{"format","pages"});
        if(pages["format"].get_str()!="b3-owned-nitro-ancestor-pages/1"&&pages["format"].get_str()!="b3-arbitrum-sepolia-ancestor-pages/1")throw std::runtime_error("ANCESTRY_FORMAT_REQUIRED");
        if(!pages["pages"].isArray()||pages["pages"].size()>64)throw std::runtime_error("ANCESTRY_PAGE_LIMIT");size_t total=0;
        for(const auto& page:pages["pages"].getValues()){
            if(!page.isArray()||!page.size()||page.size()>arb::MAX_HEADERS)throw std::runtime_error("ANCESTRY_HEADER_LIMIT");std::vector<arb::Bytes> raw_page;
            for(const auto& record:page.getValues()){
                gl::json::Object(record,{"header_sha256"},{"hash","number","parent_hash","receipts","transactions"});const auto name{record["header_sha256"].get_str()};
                if(name.size()!=64||name.find_first_not_of("0123456789abcdef")!=std::string::npos)throw std::runtime_error("INVALID_OBJECT_REFERENCE");
                auto header{Read(std::filesystem::path{args.at("--objects")}/name,arb::MAX_HEADER_BYTES)};
                if(Sha(header)!="0x"+name||header.size()>arb::MAX_PROOF_BYTES-total)throw std::runtime_error("ANCESTRY_OBJECT_OR_LIMIT");total+=header.size();raw_page.push_back(std::move(header));}
            endpoint=sep::Verifier::WalkBack(ethereum,CurrentSlot(),*endpoint.value,raw_page);if(!endpoint.value){std::cout<<Base(sep::Name(endpoint.error)).write()<<'\n';return 2;}}
        stage="complete_history";const auto history{Proof(args.at("--history-proof"))};const auto range{sep::Verifier::VerifyRange(ethereum,CurrentSlot(),*endpoint.value,request,history)};
        if(!range.value){std::cout<<Base(sep::Name(range.error)).write()<<'\n';return 2;}
        const auto& evidence{range.value->History()};const auto events{hist::ExtractEvents(evidence,vaults)};if(!events)throw std::runtime_error("BAD_CANDIDATE_EVENT");
        if(!range.value->IsCurrent(ethereum,CurrentSlot()))throw std::runtime_error("STALE_FINAL_RANGE");
        auto out{Base("OK",true)};Anchor(out,*anchor);out.pushKV("deployment_digest",Hex(range.value->Settlement().DeploymentDigest()));out.pushKV("assertion_hash",Hex(head.value->AssertionHash()));
        out.pushKV("settled_height",std::to_string(head.value->Header().number));out.pushKV("settled_hash",Hex(head.value->Header().hash));
        out.pushKV("previous_height",std::to_string(request.previous_height));out.pushKV("previous_hash",Hex(request.previous_hash));out.pushKV("to_height",std::to_string(request.to_height));out.pushKV("to_hash",Hex(request.to_hash));
        out.pushKV("header_count",evidence.headers.size());out.pushKV("receipt_count",evidence.receipts.size());out.pushKV("newest_accounts",Accounts(evidence.newest_accounts));out.pushKV("oldest_accounts",Accounts(evidence.oldest_accounts));
        UniValue records{UniValue::VARR};for(const auto& event:*events){UniValue record{UniValue::VOBJ};
            record.pushKV("kind",event.kind==hist::EventKind::DEPOSIT?"deposit":"paid");record.pushKV("vault",Hex(event.vault));record.pushKV("block_number",std::to_string(event.block_number));record.pushKV("block_hash",Hex(event.block_hash));
            record.pushKV("transaction_index",std::to_string(event.transaction_index));record.pushKV("transaction_hash",Hex(event.transaction_hash));record.pushKV("log_index",std::to_string(event.log_index));record.pushKV("amount_raw",Decimal(event.amount));
            if(event.kind==hist::EventKind::DEPOSIT){record.pushKV("deposit_id",std::to_string(event.deposit_id));record.pushKV("account",Hex(event.account));record.pushKV("asset",Hex(event.asset));record.pushKV("sender",Hex(event.address));}
            else{record.pushKV("withdrawal_id",Hex(event.withdrawal_id));record.pushKV("recipient",Hex(event.address));}records.push_back(std::move(record));}
        out.pushKV("candidate_events",std::move(records));const auto text{out.write()};if(text.size()>8*1024*1024)throw std::runtime_error("REPORT_LIMIT");std::cout<<text<<'\n';return std::cout.good()?0:1;
#endif
    }catch(const std::exception&){auto out{Base("INVALID_INPUT_OR_LIMIT")};out.pushKV("diagnostic_stage",stage);std::cout<<out.write()<<'\n';return 2;}
}
