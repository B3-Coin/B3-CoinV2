// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
// TEST ONLY: healthy view-zero FAST core, not a deployable consensus protocol.
// No view change, timeout unlock, real custody/anchors, HTTPS, Qt or node runtime.
#include <test/flowmesh_fastpath_probe_fixture.h>
#include <dbwrapper.h>
#include <hash.h>
#include <streams.h>
#include <univalue.h>
#include <util/translation.h>
#include <util/strencodings.h>

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

const TranslateFn G_TRANSLATION_FUN{nullptr};
namespace {
using namespace fastprobe;
using Clock = std::chrono::steady_clock;
constexpr uint32_t MAX_FRAME{1 << 20};
constexpr uint16_t STORE_VERSION{1};
enum Kind : uint8_t { REQUEST=1, PROPOSE, VOTE, DECIDE, ACK, RESULT, DONE, INFO, STOP };
using Sig = std::array<unsigned char, bls::SIGNATURE_SIZE>;
using Metrics = std::array<int64_t, 8>;
void Need(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
int64_t Us(Clock::time_point t) { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-t).count(); }
template<class F> void Timed(int64_t& out, F f) { auto t=Clock::now(); f(); out += Us(t); }
void BlobRead(auto& s, Bytes& out) {
    const auto n=ReadCompactSize(s);
    Need(n <= MAX_FRAME, "blob bound"); out.resize(n);
    if(n) s.read(std::as_writable_bytes(std::span{out}));
}
struct Packet {
    uint8_t kind{0}; uint64_t seq{0}; uint32_t seat{0};
    Bytes request, entry, receipt;
    Sig leader{}, vote{}, certificate{}; uint8_t bitmap{0};
    Metrics metrics{};
    template<class S> void Serialize(S& s) const {
        s << kind << seq << seat << request << entry << receipt << leader << vote
          << certificate << bitmap;
        for(auto v:metrics)s<<v;
    }
    template<class S> void Unserialize(S& s) {
        s >> kind >> seq >> seat; BlobRead(s,request); BlobRead(s,entry); BlobRead(s,receipt);
        s >> leader >> vote >> certificate >> bitmap;
        for(auto& v:metrics)s>>v;
    }
};
template<class T> Bytes Encode(const T& value) { Bytes b; VectorWriter w{b,0}; w<<value; return b; }
Packet Decode(std::span<const unsigned char> b) {
    Need(b.size()<=MAX_FRAME,"packet bound"); Packet p; SpanReader r{b}; r>>p;
    Need(r.empty(),"trailing packet bytes"); return p;
}
uint256 Value(const Fixture& f, const Packet& p) {
    HashWriter h;
    h << std::string{"B3/TEST-ONLY/NATIVE-FAST/1/PREPARE"} << f.domain << f.market
      << f.ConfigId() << uint64_t{0} << p.seq << p.request << p.entry << p.receipt;
    // Fixed generated committee is committed by the exact canonical keys.
    for (const auto& k:f.seat_keys) h << k.GetPublicKey().Compressed();
    return h.GetHash();
}
std::span<const unsigned char> Digest(const uint256& v) { return {v.begin(),32}; }
bool Same(const Fixture& f,const Packet& a,const Packet& b) { return a.seq==b.seq && Value(f,a)==Value(f,b); }
struct Crypto {
    const Fixture& f; std::vector<bls::VerifiedPublicKey> verified;
    explicit Crypto(const Fixture& fixture):f(fixture) {
        for(const auto& key:f.seat_keys) {
            auto pk=bls::VerifiedPublicKey::FromPoP(key.GetPublicKey(),key.SignPoP());
            Need(pk.has_value(),"test PoP"); verified.push_back(*pk);
        }
    }
    void VoteOK(const Packet& p, uint32_t seat,const Sig& bytes) const {
        Need(seat<4,"vote seat"); auto sig=bls::Signature::Decode(bytes);
        Need(sig && bls::Verify(verified[seat].Key(),Digest(Value(f,p)),*sig),"invalid vote");
    }
    void CertOK(const Packet& p) const {
        Need((p.bitmap&0xf0)==0 && (p.bitmap&1) && std::popcount(p.bitmap)>=3,"certificate quorum/leader");
        std::vector<bls::VerifiedPublicKey> keys;
        for(unsigned i=0;i<4;++i) if(p.bitmap&(1<<i)) keys.push_back(verified[i]);
        auto sig=bls::Signature::Decode(p.certificate);
        Need(sig && bls::FastAggregateVerify(keys,Digest(Value(f,p)),*sig),"invalid certificate");
    }
};
struct Socket {
    int fd{-1}; Socket()=default; explicit Socket(int f):fd(f) {}
    Socket(Socket&& o) noexcept:fd(std::exchange(o.fd,-1)) {}
    Socket& operator=(Socket&& o) noexcept { if(fd>=0) close(fd); fd=std::exchange(o.fd,-1); return *this; }
    Socket(const Socket&)=delete; ~Socket(){ if(fd>=0) close(fd); }
};
void Configure(int fd) {
    timeval timeout{15,0}; int one=1;
    Need(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0,"receive timeout");
    Need(setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))==0,"send timeout");
    Need(setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one))==0,"nodelay");
}
Socket Connect(int port) {
    Need(port>0 && port<65536,"port"); Socket s{socket(AF_INET,SOCK_STREAM,0)};
    Need(s.fd>=0,"socket"); Configure(s.fd);
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    Need(connect(s.fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0,"loopback connect"); return s;
}
std::pair<Socket,int> Listen() {
    Socket s{socket(AF_INET,SOCK_STREAM,0)}; Need(s.fd>=0,"listen socket");
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    Need(bind(s.fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0,"loopback bind");
    Need(listen(s.fd,4)==0,"listen"); socklen_t n=sizeof(a);
    Need(getsockname(s.fd,reinterpret_cast<sockaddr*>(&a),&n)==0,"getsockname"); return {std::move(s),ntohs(a.sin_port)};
}
void Transfer(int fd, void* buf,size_t n,bool send_it) {
    auto* p=static_cast<unsigned char*>(buf);
    while(n) { auto r=send_it ? send(fd,p,n,0) : recv(fd,p,n,0);
        if(r<0 && errno==EINTR) continue;
        Need(r>0,send_it?"send failed":"receive failed/closed/timeout"); p+=r;n-=r;
    }
}
void Send(int fd,const Packet& p) { auto b=Encode(p); Need(b.size()<=MAX_FRAME,"send bound");
    auto n=htonl(static_cast<uint32_t>(b.size())); Transfer(fd,&n,sizeof(n),true); Transfer(fd,b.data(),b.size(),true); }
Packet Receive(int fd) { uint32_t n;Transfer(fd,&n,sizeof(n),false); n=ntohl(n);
    Need(n>0&&n<=MAX_FRAME,"frame bound");Bytes b(n);Transfer(fd,b.data(),n,false);return Decode(b); }
void Json(const UniValue& v) { std::cout<<v.write()<<std::endl; }
UniValue Event(const char* type) { UniValue v{UniValue::VOBJ};v.pushKV("event",type);v.pushKV("pid",int64_t(getpid()));return v; }

// Every read requires exact decoding. No missing/corrupt record is treated as
// an empty database; a whole coherent old backup still requires external fencing.
template<class T> bool Read(CDBWrapper& db,const std::string& key,T& out) {
    std::unique_ptr<CDBIterator> it{db.NewIterator()};it->Seek(key);std::string found;
    if(!it->Valid()) { Need(it->StatusOK(),"DB iterator status");return false; }
    Need(it->GetKeyExact(found),"malformed DB key"); if(found!=key)return false;
    Need(it->GetValueExact(out),"malformed DB record");Need(it->StatusOK(),"DB read status");return true;
}
std::string Key(char prefix,uint64_t seq) { return std::string{prefix}+std::to_string(seq); }
class Store {
public:
    CDBWrapper db; const Fixture& f; const Crypto& crypto; unsigned seat;
    uint64_t committed{0}; // Count, not entry sequence (maker setup is entry zero).
    std::string cut;
    Store(const std::string& path,const Fixture& fixture,const Crypto& c,unsigned s,std::string fault={}):
        db(DBParams{.path=fs::PathFromString(path),.cache_bytes=1<<20,.obfuscate=false}),f(fixture),crypto(c),seat(s),cut(std::move(fault)) {
        Need(seat<4,"store seat");
        HashWriter id;id<<STORE_VERSION<<f.domain<<f.market<<seat;
        const auto identity=id.GetHash();uint256 saved;
        if(!Read(db,"M",saved)) {
            Need(db.IsEmpty(),"nonempty store without identity"); CDBBatch b{db};b.Write(std::string{"M"},identity);b.Write(std::string{"H"},uint64_t{0});db.WriteBatch(b,true);
        } else Need(saved==identity,"store identity/version mismatch");
        Need(Read(db,"H",committed)&&committed<=MAX_REQUESTS,"missing/invalid committed count");
    }
    void Cut(const char* point) { if(cut==point) _exit(73); }
    Packet Prepare(Packet p, Engine& e, Execution& execution, Metrics& m) {
        Need(p.seq==e.NextEntrySequence(),"unexpected prepare sequence");
        Need(p.receipt==execution.receipt_bytes && p.entry==execution.entry_bytes,"execution mismatch");
        if(seat!=0) crypto.VoteOK(p,0,p.leader);
        Packet intent;
        if(Read(db,Key('I',p.seq),intent)) Need(Same(f,intent,p),"conflicting durable intent");
        else {
            intent=p; intent.kind=PROPOSE;intent.seat=seat;intent.vote={};intent.certificate={};intent.bitmap=0;intent.metrics={};
            Timed(m[1],[&]{ CDBBatch b{db};b.Write(Key('I',p.seq),intent);db.WriteBatch(b,true); });
        }
        Cut("after-intent");
        Packet saved;
        if(Read(db,Key('P',p.seq),saved)) {
            Need(Same(f,saved,p)&&saved.seat==seat,"pending vote mismatch");crypto.VoteOK(saved,seat,saved.vote);p.vote=saved.vote;
        } else {
            Timed(m[2],[&]{p.vote=f.seat_keys[seat].Sign(Digest(Value(f,p))).Compressed();});
            p.seat=seat;p.kind=VOTE;p.metrics={};p.certificate={};p.bitmap=0;
            Timed(m[3],[&]{CDBBatch b{db};b.Write(Key('P',p.seq),p);db.WriteBatch(b,true);});
        }
        Cut("after-signed");p.kind=VOTE;p.seat=seat;return p;
    }
    void Commit(const Packet& p, Engine& e, std::optional<Execution>& execution, Metrics& m) {
        Timed(m[4],[&]{crypto.CertOK(p);});
        Packet old;
        if(Read(db,Key('C',p.seq),old)) { Need(Same(f,old,p),"conflicting decision"); return; }
        Need(p.seq==e.NextEntrySequence() && committed+1==p.seq,"commit sequence");
        if(!execution) Timed(m[0],[&]{execution.emplace(e.Execute(p.request,p.entry));});
        Need(execution->receipt_bytes==p.receipt,"commit receipt mismatch");
        Packet canonical=p;canonical.kind=DECIDE;canonical.metrics={};
        Timed(m[5],[&]{CDBBatch b{db};b.Write(Key('C',p.seq),canonical);b.Write(std::string{"H"},committed+1);
            b.Write(std::string{"S"},Encode(execution->next_state));
            db.WriteBatch(b,true);});
        ++committed;Cut("after-decision");
        Timed(m[6],[&]{e.Apply(std::move(*execution));execution.reset();});
    }
    void Restore(Engine& e) {
        // Reconstruct only from original authenticated entries with valid certs.
        for(uint64_t seq=1;seq<=committed;++seq) {
            Packet p;Need(Read(db,Key('C',seq),p)&&p.seq==seq,"missing decision");crypto.CertOK(p);
            auto x=e.Execute(p.request,p.entry);Need(x.receipt_bytes==p.receipt,"restored receipt mismatch");e.Apply(std::move(x));
        }
        std::unique_ptr<CDBIterator> it{db.NewIterator()};
        for(it->SeekToFirst();it->Valid();it->Next()) {
            std::string key;Need(it->GetKeyExact(key),"bad store key");if(key=="M"||key=="H"||key=="S")continue;
            Need(!key.empty()&&(key[0]=='I'||key[0]=='P'||key[0]=='C'),"unexpected store key");
            Packet p;Need(it->GetValueExact(p),"bad stored packet");Need(key==Key(key[0],p.seq),"key sequence mismatch");
            Need(p.seq>0&&p.seq<=committed+(key[0]!='C'),"orphan record beyond head");
            if(key[0]=='C') {crypto.CertOK(p);continue;}
            if(seat!=0)crypto.VoteOK(p,0,p.leader);
            if(key[0]=='P') {Packet intent;Need(Read(db,Key('I',p.seq),intent)&&Same(f,intent,p),"vote missing intent");
                Need(p.seat==seat,"wrong stored voter");crypto.VoteOK(p,seat,p.vote);}
            if(p.seq<=committed) {Packet decision;Need(Read(db,Key('C',p.seq),decision)&&Same(f,p,decision),"historical obligation mismatch");}
            if(p.seq>committed) {auto x=e.Execute(p.request,p.entry);Need(x.receipt_bytes==p.receipt,"missing pending execution evidence");}
        }
        Need(it->StatusOK(),"store scan status");
        Bytes snapshot;
        if(committed)Need(Read(db,"S",snapshot)&&snapshot==Encode(e.State()),"missing/inconsistent applied snapshot");
        else Need(!Read(db,"S",snapshot),"snapshot without decision");
    }
};

Packet Proposed(Engine& e, const Packet& request, Metrics& m, std::optional<Execution>& x) {
    Timed(m[0],[&]{x.emplace(e.Execute(request.request));}); Packet p;
    p.kind=PROPOSE;p.seq=x->entry_sequence;p.request=x->request_bytes;p.entry=x->entry_bytes;p.receipt=x->receipt_bytes;return p;
}

void Server(unsigned seat,const std::string& path,const std::vector<int>& ports,const std::string& cut) {
    Fixture f; Crypto crypto{f}; Engine engine{f};Store store{path,f,crypto,seat,cut};store.Restore(engine);
    std::vector<Socket> peers;for(int port:ports)peers.push_back(Connect(port));
    Need(seat==0 ? peers.size()==3 : peers.empty(),"role/peer count");
    auto [listener,port]=Listen();auto ready=Event("ready");ready.pushKV("seat",seat);ready.pushKV("port",port);
    ready.pushKV("committed",store.committed);ready.pushKV("state_root",engine.State().Root().GetHex());Json(ready);
    Socket conn{accept(listener.fd,nullptr,nullptr)};Need(conn.fd>=0,"accept");Configure(conn.fd);
    std::optional<Execution> pending;Metrics metrics{};
    while(true) {
        Packet p=Receive(conn.fd);
        if(p.kind==STOP) {for(auto& peer:peers)Send(peer.fd,p);Send(conn.fd,p);break;}
        if(p.kind==INFO) {p.seq=engine.NextEntrySequence();p.entry=Encode(engine.Head());p.receipt=Encode(engine.State().Root());Send(conn.fd,p);continue;}
        if(seat!=0) {
            if(p.kind==PROPOSE) {
                metrics={};Timed(metrics[0],[&]{pending.emplace(engine.Execute(p.request,p.entry));});
                auto vote=store.Prepare(p,engine,*pending,metrics);Send(conn.fd,vote);store.Cut("after-published");
            } else if(p.kind==DECIDE) {
                store.Commit(p,engine,pending,metrics);p.kind=ACK;p.seat=seat;p.metrics=metrics;Send(conn.fd,p);
            } else throw std::runtime_error("unexpected follower message");
            continue;
        }
        Need(p.kind==REQUEST,"unexpected leader message");metrics={};auto start=Clock::now();
        p=Proposed(engine,p,metrics,pending);auto own=store.Prepare(p,engine,*pending,metrics);
        p.leader=own.vote;p.kind=PROPOSE;
        for(auto& peer:peers)Send(peer.fd,p);store.Cut("after-published");
        std::vector<bls::Signature> votes{*bls::Signature::Decode(own.vote)};uint8_t bitmap=1;
        std::array<bool,3> got_vote{};auto quorum_start=Clock::now();
        while(votes.size()<3) {
            std::array<pollfd,3> pf{};for(unsigned i=0;i<3;++i)pf[i]={peers[i].fd,short(got_vote[i]?0:POLLIN),0};
            Need(poll(pf.data(),pf.size(),15000)>0,"quorum unavailable; no unlock/fallback");
            for(const auto& ready:pf)Need(!(ready.revents&(POLLERR|POLLHUP|POLLNVAL)),"peer disconnected; no unlock/fallback");
            for(unsigned i=0;i<3 && votes.size()<3;++i) if(pf[i].revents&POLLIN) {
                auto v=Receive(peers[i].fd);Need(v.kind==VOTE&&v.seat==i+1&&Same(f,p,v),"vote context");
                crypto.VoteOK(v,v.seat,v.vote);Need(!(bitmap&(1<<v.seat)),"duplicate voter");
                votes.push_back(*bls::Signature::Decode(v.vote));bitmap|=1<<v.seat;got_vote[i]=true;
            }
        }
        metrics[7]=Us(quorum_start);auto aggregate=bls::AggregateSignatures(votes);Need(aggregate.has_value(),"aggregate");
        p.bitmap=bitmap;p.certificate=aggregate->Compressed();p.kind=DECIDE;
        store.Commit(p,engine,pending,metrics);
        for(auto& peer:peers)Send(peer.fd,p);
        p.kind=RESULT;p.metrics=metrics;Send(conn.fd,p);
        std::array<bool,3> acked{};std::array<Metrics,4> all{};all[0]=metrics;
        for(unsigned count=0;count<3;) {
            std::array<pollfd,3> pf{};for(unsigned i=0;i<3;++i)pf[i]={peers[i].fd,short(acked[i]?0:POLLIN),0};
            Need(poll(pf.data(),pf.size(),15000)>0,"replication unavailable");
            for(const auto& ready:pf)Need(!(ready.revents&(POLLERR|POLLHUP|POLLNVAL)),"replica disconnected");
            for(unsigned i=0;i<3;++i)if(pf[i].revents&POLLIN) {
                auto a=Receive(peers[i].fd);Need(Same(f,p,a)&&a.seat==i+1,"replica identity");
                if(a.kind==VOTE) {Need(!got_vote[i],"duplicate late vote");crypto.VoteOK(a,a.seat,a.vote);got_vote[i]=true;continue;}
                Need(a.kind==ACK,"expected durable ACK");acked[i]=true;++count;all[i+1]=a.metrics;
            }
        }
        p.kind=DONE;p.metrics[7]=Us(start);Send(conn.fd,p);
        auto result=Event("replicas_applied");result.pushKV("sequence",p.seq);result.pushKV("state_root",engine.State().Root().GetHex());
        UniValue stages{UniValue::VARR};for(auto row:all){UniValue r{UniValue::VARR};for(auto us:row)r.push_back(us);stages.push_back(r);}result.pushKV("node_us",stages);Json(result);
    }
    auto done=Event("clean_shutdown");done.pushKV("seat",seat);done.pushKV("committed",store.committed);Json(done);
}

void Client(int port,const std::string& path,unsigned count) {
    Need(count<=MAX_REQUESTS,"sample bound");Fixture f;Crypto crypto{f}; // Deliberately no Engine.
    CDBWrapper outbox{DBParams{.path=fs::PathFromString(path),.cache_bytes=1<<20,.obfuscate=false}};
    Socket socket=Connect(port);Packet info;info.kind=INFO;Send(socket.fd,info);info=Receive(socket.fd);
    Need(info.kind==INFO&&info.seq==1,"fresh client campaign requires fresh generated replicas");
    uint256 previous_head,previous_root;
    {SpanReader h{info.entry};h>>previous_head;Need(h.empty(),"head encoding");SpanReader r{info.receipt};r>>previous_root;Need(r.empty(),"root encoding");}
    // INFO is a trusted isolated-fixture bootstrap, not a production state proof.
    for(unsigned i=0;i<count;++i) {
        Need(!outbox.Exists(Key('R',i))&&!outbox.Exists(Key('D',i)),"existing client instruction; refusing replacement");
        auto start=Clock::now();Packet p;p.kind=REQUEST;p.request=MakeRequest(f,i);
        auto signed_at=Us(start);{CDBBatch b{outbox};b.Write(Key('R',i),p.request);outbox.WriteBatch(b,true);}
        auto sent_at=Us(start);Send(socket.fd,p);auto result=Receive(socket.fd);auto received_at=Us(start);
        Need(result.kind==RESULT&&result.seq==i+1&&result.request==p.request,"original request binding");
        crypto.CertOK(result);auto receipt=ValidateReceipt(f,result.request,result.entry,result.receipt);
        auto entry=flowmesh::DecodeProductionEntry(result.entry);Need(entry.has_value(),"entry decode");
        Need(entry->parent_hash==previous_head&&receipt.previous_state_root==previous_root,"client continuity");
        previous_head=receipt.entry_hash;previous_root=receipt.state_root;auto verified_at=Us(start);
        {CDBBatch b{outbox};b.Write(Key('D',i),result);outbox.WriteBatch(b,true);}
        auto durable_at=Us(start);auto done=Receive(socket.fd);Need(done.kind==DONE&&Same(f,done,result),"completion binding");
        auto all_at=Us(start);auto sample=Event("sample");sample.pushKV("index",i);sample.pushKV("action_id",receipt.action_id.GetHex());
        sample.pushKV("sequence",result.seq);sample.pushKV("signed_us",signed_at);sample.pushKV("outbox_ready_us",sent_at);
        sample.pushKV("received_us",received_at);sample.pushKV("verified_us",verified_at);sample.pushKV("client_durable_us",durable_at);
        sample.pushKV("all_replicas_observed_us",all_at);sample.pushKV("quantity",receipt.quantity);sample.pushKV("fee",receipt.fee_total);Json(sample);
    }
    Packet stop;stop.kind=STOP;Send(socket.fd,stop);Need(Receive(socket.fd).kind==STOP,"stop ACK");Json(Event("clean_shutdown"));
}

void Inspect(const std::string& path,unsigned seat) {
    Fixture f;Crypto crypto{f};Engine e{f};Store s{path,f,crypto,seat};s.Restore(e);
    auto v=Event("reopened");v.pushKV("seat",seat);v.pushKV("committed",s.committed);v.pushKV("state_root",e.State().Root().GetHex());
    Packet pending;if(Read(s.db,Key('P',e.NextEntrySequence()),pending)) {v.pushKV("pending_value",Value(f,pending).GetHex());v.pushKV("exact_vote",HexStr(pending.vote));}
    Json(v);
}
void ExpectRefusal(auto f,const char* name) {
    bool refused=false;try{f();}catch(const std::exception&){refused=true;}
    Need(refused,name);
}
// Process-interruption tests only. Unlike the TCP campaign, the remaining
// synthetic seats below sign in this process. Never include this in latency.
void Exercise(const std::string& path,const std::string& cut) {
    Fixture f;Crypto crypto{f};Engine e{f};Store s{path,f,crypto,0,cut};s.Restore(e);
    Packet p;Metrics m{};std::optional<Execution> x;
    if(!s.committed) {
        if(Read(s.db,Key('I',1),p))x.emplace(e.Execute(p.request,p.entry));
        else {Packet request;request.request=MakeRequest(f,0);p=Proposed(e,request,m,x);}
        auto vote=s.Prepare(p,e,*x,m);p.leader=vote.vote;
        auto issued=Event("test_vote_published");issued.pushKV("value",Value(f,p).GetHex());issued.pushKV("exact_vote",HexStr(vote.vote));Json(issued);
        s.Cut("after-published");
        std::vector<bls::Signature> votes{*bls::Signature::Decode(vote.vote)};
        for(unsigned i=1;i<3;++i)votes.push_back(f.seat_keys[i].Sign(Digest(Value(f,p))));
        p.bitmap=7;p.certificate=bls::AggregateSignatures(votes)->Compressed();
        s.Commit(p,e,x,m);s.Cut("after-applied");
    } else Need(Read(s.db,Key('C',1),p),"exercise decision missing");
    const auto state=Encode(e.State());s.Commit(p,e,x,m);
    Need(s.committed==1&&state==Encode(e.State()),"duplicate certificate applied twice");
    Packet bad=p;bad.bitmap=3;ExpectRefusal([&]{crypto.CertOK(bad);},"subquorum accepted");
    bad=p;bad.bitmap=0x17;ExpectRefusal([&]{crypto.CertOK(bad);},"unknown seat accepted");
    bad=p;bad.receipt[0]^=1;ExpectRefusal([&]{crypto.CertOK(bad);},"tampered receipt accepted");
    bad=p;bad.seq++;ExpectRefusal([&]{crypto.CertOK(bad);},"wrong slot accepted");
    bad=p;bad.certificate[0]^=1;ExpectRefusal([&]{crypto.CertOK(bad);},"bad signature accepted");
    auto bytes=Encode(p);bytes.push_back(0);ExpectRefusal([&]{Decode(bytes);},"trailing frame accepted");
    auto done=Event("exercise_pass");done.pushKV("committed",s.committed);done.pushKV("state_root",e.State().Root().GetHex());
    done.pushKV("negative_checks",6);Json(done);
}
void CorruptTestStore(const std::string& path,const std::string& mode) {
    Fixture f;Crypto crypto{f};Engine e{f};Store s{path,f,crypto,0};s.Restore(e);
    Need(s.committed==1,"corruption injection requires completed generated fixture");
    CDBBatch b{s.db};
    if(mode=="missing-intent")b.Erase(Key('I',1));
    else if(mode=="missing-snapshot")b.Erase(std::string{"S"});
    else if(mode=="bad-snapshot")b.Write(std::string{"S"},Bytes{1,2,3});
    else if(mode=="bad-record")b.Write(Key('P',1),Bytes{1,2,3});
    else throw std::runtime_error("unknown test corruption");
    s.db.WriteBatch(b,true);Json(Event("injected_test_corruption"));
}
void InspectClient(const std::string& path,unsigned count) {
    Fixture f;Crypto crypto{f};CDBWrapper db{DBParams{.path=fs::PathFromString(path),.cache_bytes=1<<20,.obfuscate=false}};
    for(unsigned i=0;i<count;++i){Bytes original;Packet result;
        Need(Read(db,Key('R',i),original)&&Read(db,Key('D',i),result),"missing original or final client record");
        Need(original==result.request,"changed original request");crypto.CertOK(result);
        auto r=ValidateReceipt(f,original,result.entry,result.receipt);Need(r.action_sequence==i,"changed original sequence");
    }
    auto v=Event("client_reopened");v.pushKV("certified_originals",count);Json(v);
}
} // namespace
int main(int argc,char** argv) {
    try {
        signal(SIGPIPE,SIG_IGN);ECC_Context ecc;
        Need(argc>=2,"mode required");std::string mode=argv[1];
        if(mode=="server") {Need(argc>=5,"server seat path cut [three peer ports]");std::vector<int> ports;
            for(int i=5;i<argc;++i)ports.push_back(std::stoi(argv[i]));Server(std::stoul(argv[2]),argv[3],ports,argv[4]);}
        else if(mode=="client") {Need(argc==5,"client port path count");Client(std::stoi(argv[2]),argv[3],std::stoul(argv[4]));}
        else if(mode=="inspect") {Need(argc==4,"inspect seat path");Inspect(argv[3],std::stoul(argv[2]));}
        else if(mode=="exercise") {Need(argc==4,"exercise path cut");Exercise(argv[2],argv[3]);}
        else if(mode=="corrupt-test-store") {Need(argc==4,"corrupt-test-store path mode");CorruptTestStore(argv[2],argv[3]);}
        else if(mode=="inspect-client") {Need(argc==4,"inspect-client path count");InspectClient(argv[2],std::stoul(argv[3]));}
        else throw std::runtime_error("unknown test mode");
        return 0;
    } catch(const std::exception& e) {std::cerr<<"TEST_PROBE_SAFE_STOP: "<<e.what()<<std::endl;return 1;}
}
