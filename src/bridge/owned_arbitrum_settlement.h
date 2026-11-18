// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license.
#ifndef B3COIN_BRIDGE_OWNED_ARBITRUM_SETTLEMENT_H
#define B3COIN_BRIDGE_OWNED_ARBITRUM_SETTLEMENT_H
#include <bridge/arbitrum_history.h>
#include <bridge/arbitrum_sepolia.h>

// Explicitly OWNER-TRUSTED owned-regtest L1 checkpoints. This module has no
// public Ethereum PoS or public Arbitrum authority and no production capability.
namespace bridge::arbitrum::owned {
inline constexpr uint64_t L1_CHAIN_ID{1337}, L2_CHAIN_ID{412346};
struct Pin { EthAddress address{}; uint256 code_hash{}; std::map<StorageWord, StorageWord> storage; };
struct Policy {
    uint256 l1_genesis{}, l2_genesis{};
    uint64_t deployment_block{0};
    std::vector<Pin> accounts;
};
inline StorageWord AddressWord(const EthAddress& address)
{ StorageWord out{}; std::copy(address.begin(),address.end(),out.begin()+12); return out; }
inline StorageWord SlotHash(const StorageWord& key, uint64_t base)
{
    Bytes bytes{key.begin(),key.end()}; const auto slot{SlotWord(base)};
    bytes.insert(bytes.end(),slot.begin(),slot.end()); const auto hash{detail::Hash(bytes)};
    StorageWord out; std::copy(hash.begin(),hash.end(),out.begin()); return out;
}
inline StorageWord AssertionSlot(const uint256& hash)
{ StorageWord word; std::copy(hash.begin(),hash.end(),word.begin()); return SlotHash(word,117); }
inline StorageWord Next(StorageWord slot)
{ for (auto i=slot.rbegin();i!=slot.rend();++i) if (++*i) break; return slot; }
inline StorageWord Mask(size_t account, const StorageWord& slot)
{
    StorageWord out; out.fill(0xff);
    if (account==13 && slot==SlotWord(12)) { std::fill_n(out.begin(),8,0); std::fill(out.begin()+24,out.end(),0); }
    if (account==13 && slot==SlotWord(13)) std::fill(out.begin()+16,out.begin()+24,0);
    return out;
}
inline bool EqualMasked(size_t account,const StorageWord& slot,const StorageWord& a,const StorageWord& b)
{ const auto mask{Mask(account,slot)}; for(size_t i=0;i<32;++i)if((a[i]&mask[i])!=(b[i]&mask[i]))return false; return true; }
inline bool ValidPolicy(const Policy& p)
{
    if(p.l1_genesis.IsNull()||p.l2_genesis.IsNull()||!p.deployment_block||p.accounts.size()!=16)return false;
    std::set<EthAddress> addresses;
    for(const auto& pin:p.accounts) if(!detail::Nonzero(pin.address)||pin.code_hash.IsNull()||!addresses.insert(pin.address).second||pin.storage.size()>MAX_STORAGE)return false;
    const auto has=[&](size_t a,const StorageWord& slot,const StorageWord& word){const auto f=p.accounts[a].storage.find(slot);return f!=p.accounts[a].storage.end()&&f->second==word;};
    const auto pointer=[&](size_t a,const StorageWord& slot,size_t target){return has(a,slot,AddressWord(p.accounts[target].address));};
    const auto primary{sepolia::pins::Fixed<32>("360894a13ba1a3210667c828492db98dca3e2076cc3735a920a3ca505d382bbc")};
    const auto secondary{sepolia::pins::Fixed<32>("2b1dbce74324248c222f0ec2d5ed7bd323cfc425b336f0253c5ccfda7265546d")};
    const auto admin{sepolia::pins::Fixed<32>("b53127684a568b3173ae13b9f8a6016e243e63b6e8ee1178d6a717850b5d6103")};
    if(!pointer(0,primary,1)||!pointer(0,secondary,2)||!pointer(0,admin,3)||!pointer(3,primary,4)||!pointer(3,admin,15)||!pointer(15,SlotWord(0),3))return false;
    for(const size_t proxy:{5,7,9,11,13})if(!pointer(proxy,primary,proxy+1)||!pointer(proxy,admin,15))return false;
    if(!has(0,SlotWord(51),{})||!has(0,SlotWord(101),SlotWord(L2_CHAIN_ID))||!has(0,SlotWord(123),{})||
       !has(0,SlotWord(104),sepolia::pins::Fixed<32>("c10cd7ec6acaf1c441a3f6bd0900ad20f15855ba775a96f1939118cbc629dc97")))return false;
    for(const size_t a:{0,3,5,7,9})if(!has(a,SlotWord(0),SlotWord(1)))return false;
    if(!has(9,SlotWord(51),{})||!has(0,SlotWord(122),SlotWord(p.deployment_block))||
       (!has(3,SlotWord(151),SlotWord(0))&&!has(3,SlotWord(151),SlotWord(1))))return false;
    if(!pointer(0,SlotWord(106),9)||!pointer(0,SlotWord(107),7)||!pointer(0,SlotWord(108),11)||
       !pointer(7,SlotWord(8),0)||!pointer(7,SlotWord(9),13)||!pointer(9,SlotWord(101),7)||
       !pointer(11,SlotWord(0),0)||!pointer(11,SlotWord(1),7)||!pointer(13,SlotWord(1),7)||!pointer(13,SlotWord(2),0))return false;
    const auto challenge=p.accounts[0].storage.find(SlotWord(105));
    const auto inbox=p.accounts[9].storage.find(SlotWord(102));
    const auto chain=p.accounts[5].storage.find(SlotWord(8));
    if(challenge==p.accounts[0].storage.end()||!std::equal(challenge->second.begin()+12,challenge->second.end(),p.accounts[5].address.begin())||
       inbox==p.accounts[9].storage.end()||!std::equal(inbox->second.begin()+12,inbox->second.end(),p.accounts[13].address.begin())||
       chain==p.accounts[5].storage.end()||!std::equal(chain->second.begin()+4,chain->second.begin()+24,p.accounts[0].address.begin()))return false;
    // Pin the complete provided config/auth leaves; implementation-only roles
    // have no mutable proxy storage. Reviewed mutable words cannot become pins.
    for(const size_t a:{1,2,4,6,8,10,12,14})if(!p.accounts[a].storage.empty())return false;
    for(const uint64_t slot:{6,7,10})if(p.accounts[7].storage.contains(SlotWord(slot)))return false;
    if(p.accounts[13].storage.contains(SlotWord(0)))return false;
    if(!has(7,Next(SlotHash(AddressWord(p.accounts[9].address),1)),SlotWord(1))||
       !has(7,Next(SlotHash(AddressWord(p.accounts[11].address),2)),SlotWord(1)))return false;
    for(const auto& [account,base]:std::vector<std::pair<size_t,uint64_t>>{{0,101},{5,0},{7,0},{9,101},{11,0},{13,1}}){
        const auto last=account==0?115:account==5?13:account==7?9:account==9?103:account==11?7:14;
        for(uint64_t slot=base;slot<=static_cast<uint64_t>(last);++slot){if(account==7&&(slot==6||slot==7))continue;if(!p.accounts[account].storage.contains(SlotWord(slot)))return false;}}
    const auto validators=p.accounts[0].storage.find(SlotWord(114));
    if(validators==p.accounts[0].storage.end()||detail::Nonzero(std::span{validators->second}.first(24)))return false;
    uint64_t count=0;for(const auto b:std::span{validators->second}.last(8))count=(count<<8)|b;
    if(!count||count>16)return false;
    auto array_hash=detail::Hash(SlotWord(114));StorageWord cell;std::copy(array_hash.begin(),array_hash.end(),cell.begin());
    for(uint64_t i=0;i<count;++i){const auto member=p.accounts[0].storage.find(cell);if(member==p.accounts[0].storage.end()||detail::Nonzero(std::span{member->second}.first(12))||!detail::Nonzero(member->second)||!has(0,SlotHash(member->second,115),SlotWord(i+1)))return false;cell=Next(cell);}
    const auto admin_role_hash=history::Topic("ADMIN_ROLE");StorageWord role_word;std::copy(admin_role_hash.begin(),admin_role_hash.end(),role_word.begin());
    if(!has(3,Next(SlotHash(role_word,101)),role_word))return false;
    const auto executor=history::Topic("EXECUTOR_ROLE");StorageWord executor_word;std::copy(executor.begin(),executor.end(),executor_word.begin());
    if(!has(3,Next(SlotHash(executor_word,101)),role_word))return false;
    return p.accounts[3].storage.size()>=9&&p.accounts[13].storage.size()>=18;
}
inline uint256 Digest(const Policy& policy)
{
    proof_detail::Writer w; w.U64(L1_CHAIN_ID);w.U64(L2_CHAIN_ID);w.Root(policy.l1_genesis);w.Root(policy.l2_genesis);w.U64(policy.deployment_block);
    for(size_t a=0;a<policy.accounts.size();++a){const auto& p=policy.accounts[a];w.Bytes(p.address);w.Root(p.code_hash);w.U32(p.storage.size());for(const auto&[slot,word]:p.storage){w.Bytes(slot);auto masked=word;const auto mask=Mask(a,slot);for(size_t i=0;i<32;++i)masked[i]&=mask[i];w.Bytes(masked);}}
    return peer::TaggedDigest("B3/owned-regtest-nitro-policy/1",w.Take());
}
struct Input {
    bool regtest{false};
    uint256 trusted_l1_head{}, l1_genesis{}, l2_genesis{};
    Bytes l1_header;
    Proof settlement, history;
    std::vector<std::vector<Bytes>> ancestor_pages;
    RangeRequest request;
};
class Verifier;
class Evidence final {
public:
    const history::Evidence& History()const{return m_history;}
    const Header& SettledHead()const{return m_settled;}
    const uint256& PolicyDigest()const{return m_policy;}
    const uint256& L1Checkpoint()const{return m_l1;}
    bool MainnetEnabled()const{return false;}
private:
    friend class Verifier;
    Evidence(history::Evidence history,Header settled,uint256 policy,uint256 l1):m_history{std::move(history)},m_settled{std::move(settled)},m_policy{policy},m_l1{l1}{}
    history::Evidence m_history;Header m_settled;uint256 m_policy,m_l1;
};
struct Reply { std::string error; std::optional<Evidence> evidence; };
class Verifier final {
public:
    static Reply Verify(const Policy& policy,const Input& in)
    {
        if(!in.regtest||!ValidPolicy(policy)||in.l1_genesis!=policy.l1_genesis||in.l2_genesis!=policy.l2_genesis||in.trusted_l1_head.IsNull())return {"OWNED_REGTEST_POLICY_REQUIRED",{}};
        if(in.request.previous_height==0&&in.request.previous_hash!=policy.l2_genesis)return {"WRONG_GENESIS_CURSOR",{}};
        if(in.l1_header.empty()||in.l1_header.size()>MAX_HEADER_BYTES||detail::Hash(in.l1_header)!=in.trusted_l1_head)return {"WRONG_OWNED_L1_CHECKPOINT",{}};
        const auto top{RlpDecode(in.l1_header)};const auto fields{top&&top->is_list?RlpChildren(*top):std::nullopt};
        // Known Ethereum header shapes: London16, Shanghai17, Cancun20,
        // Prague21, Amsterdam23. Their optional tail fields remain strict.
        if(!fields||(fields->size()!=16&&fields->size()!=17&&fields->size()!=20&&fields->size()!=21&&fields->size()!=23))return {"UNSUPPORTED_L1_HEADER",{}};
        const auto fixed=[&](size_t i,size_t width){return !(*fields)[i].is_list&&(*fields)[i].payload.size()==width;};
        if(fields->size()>=17&&!fixed(16,32))return {"BAD_L1_HEADER_EXTENSION",{}};
        if(fields->size()>=20&&(!deposit_detail::RlpUint64((*fields)[17])||!deposit_detail::RlpUint64((*fields)[18])||!fixed(19,32)))return {"BAD_L1_HEADER_EXTENSION",{}};
        if(fields->size()>=21&&!fixed(20,32))return {"BAD_L1_HEADER_EXTENSION",{}};
        if(fields->size()==23&&(!fixed(21,32)||!deposit_detail::RlpUint64((*fields)[22])))return {"BAD_L1_HEADER_EXTENSION",{}};
        std::vector<Bytes> first;for(size_t i=0;i<16;++i){const auto raw{RlpEncodedSpan((*fields)[i],in.l1_header)};first.emplace_back(raw.begin(),raw.end());}
        const auto l1{detail::NitroHeader(RlpEncodeList(first))};if(!l1||l1->number<policy.deployment_block)return {"BAD_L1_HEADER",{}};
        if(proof_codec::EncodeProofV1(in.settlement).result!=proof_codec::Result::OK)return {"SETTLEMENT_LIMIT",{}};
        if(in.settlement.l1_accounts.size()!=16||in.settlement.blocks.size()!=1||!in.settlement.blocks[0].receipts.empty()||!in.settlement.blocks[0].transactions.empty()||!in.settlement.confirmed_accounts.empty()||!in.settlement.historical_accounts.empty())return {"INCOMPLETE_SETTLEMENT_GRAPH",{}};
        const auto& assertion{in.settlement.assertion};if(assertion.machine_status!=1||assertion.parent_assertion.IsNull()||assertion.block_hash.IsNull())return {"BAD_ASSERTION",{}};
        const auto hash{detail::AssertionHash(assertion)};const auto node_slot{AssertionSlot(hash)};
        for(size_t a=0;a<16;++a){
            const auto& pin{policy.accounts[a]};const auto& witness{in.settlement.l1_accounts[a]};
            auto expected{pin.storage};
            if(a==0){StorageWord h;std::copy(hash.begin(),hash.end(),h.begin());expected[SlotWord(116)]=h;expected[node_slot]={};expected[Next(node_slot)]={};}
            if(witness.address!=pin.address||witness.storage.size()!=expected.size())return {"BAD_GRAPH_OR_SLOT_COUNT",{}};
            std::string error;const auto account{VerifyAccountProof(l1->state_root,pin.address,witness.nodes,error)};
            if(!account||account->code_hash!=pin.code_hash)return {"BAD_ACCOUNT_CODE_OR_PROOF",{}};
            auto want=expected.begin();
            for(const auto& item:witness.storage){if(want==expected.end()||item.slot!=want->first)return {"BAD_STORAGE_ORDER",{}};
                const auto value{VerifyStorageValue(account->storage_root,item.slot,item.nodes,error)};if(!value)return {"BAD_STORAGE_PROOF",{}};
                if(a==0&&item.slot==node_slot){if(!sepolia::detail::ConfirmedNodeWord(*value,l1->number))return {"ASSERTION_NOT_CONFIRMED",{}};}
                else if(!(a==0&&item.slot==Next(node_slot))&&!EqualMasked(a,item.slot,*value,want->second))return {"CONFIG_OR_AUTH_CHANGED",{}};
                ++want;
            }
        }
        const auto settled{detail::NitroHeader(in.settlement.blocks[0].header)};
        if(!settled||settled->hash!=assertion.block_hash||settled->timestamp>l1->timestamp)return {"BAD_SETTLED_HEADER",{}};
        if(in.ancestor_pages.size()>64)return {"ANCESTRY_LIMIT",{}};
        auto endpoint{*settled};size_t total{0};
        for(const auto& page:in.ancestor_pages){if(page.empty()||page.size()>MAX_HEADERS)return {"ANCESTRY_LIMIT",{}};
            for(const auto& raw:page){if(raw.size()>MAX_PROOF_BYTES-total)return {"ANCESTRY_LIMIT",{}};total+=raw.size();const auto header{detail::NitroHeader(raw)};
                if(!header||!endpoint.number||header->hash!=endpoint.parent||header->number!=endpoint.number-1||header->timestamp>endpoint.timestamp)return {"BAD_ANCESTRY",{}};endpoint=*header;}}
        if(endpoint.number!=in.request.to_height||endpoint.hash!=in.request.to_hash)return {"WRONG_HISTORY_ENDPOINT",{}};
        const auto checked{history::CheckRange(in.request,in.history)};if(!checked.evidence)return {"BAD_COMPLETE_HISTORY",{}};
        return {"OK",Evidence{*checked.evidence,*settled,Digest(policy),in.trusted_l1_head}};
    }
};
} // namespace bridge::arbitrum::owned
#endif
