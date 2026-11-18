// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license.
// Synthetic checkpoints exercise real native BLS/SSZ and EL-RLP verification.
#include <bridge/gloas_json.h>
#include <test/util/gloas_fixture.h>
#include <boost/test/unit_test.hpp>

namespace gl=bridge::peer::gloas;
namespace fx=gloas_fixture;
using bridge::peer::Result;

BOOST_AUTO_TEST_SUITE(bridge_gloas_peer_light_client_tests)
namespace {
struct Advance {
    gl::Update update;
    gl::Bytes raw;
};
Advance SignedUpdate(const fx::Fixture& f,uint64_t base=0,bool next=true,size_t participants=512) {
    if (!base) base=f.slot;
    Advance out;
    out.raw=fx::ExecutionHeader(fx::Tag(0x41),base+10,1'000'001);
    auto& u{out.update};
    u.finalized=fx::Header(base+10,fx::Keccak(out.raw),fx::Tag(0x51));
    u.has_next=next;
    fx::Tree state; state.leaves[gl::FINALITY_GINDEX]=u.finalized.beacon.HashTreeRoot();
    if (next) { u.next=fx::Committee(); state.leaves[gl::NEXT_GINDEX]=u.next.HashTreeRoot(); u.next_branch=state.Branch(gl::NEXT_GINDEX); }
    u.attested=fx::Header(base+20,fx::Tag(0x61),state.Node(1));
    u.finality_branch=state.Branch(gl::FINALITY_GINDEX);
    for (size_t i{0};i<participants;++i) u.sync.bits[i/8]|=1U<<(i%8);
    const auto domain{bridge::ssz::SyncCommitteeDomain(gl::FORK_VERSION,uint256{std::span{gl::GENESIS_VALIDATORS_ROOT}})};
    u.sync.signature=fx::Sign(bridge::ssz::SigningRoot(u.attested.beacon.HashTreeRoot(),domain),participants);
    u.signature_slot=base+21;
    return out;
}
gl::Bytes Wire(const gl::Update& u) {
    const auto bytes{gl::EncodeRecord(gl::Record{u})}; BOOST_REQUIRE(bytes); return *bytes;
}
UniValue PolicyJson(const fx::Fixture& f) {
    UniValue p{UniValue::VOBJ}; p.pushKV("format","b3-ethereum-sepolia-gloas-policy/1"); p.pushKV("mainnet_enabled",false);
    p.pushKV("chain_id",gl::CHAIN_ID); p.pushKV("genesis_validators_root",bridge::lcjson::EthereumHex(gl::GENESIS_VALIDATORS_ROOT));
    p.pushKV("genesis_time",gl::GENESIS_TIME); p.pushKV("seconds_per_slot",12); p.pushKV("gloas_fork_epoch",gl::FORK_EPOCH);
    p.pushKV("gloas_fork_version","0x90000076"); p.pushKV("trusted_root",bridge::lcjson::EthereumHex(f.policy.trusted_root));
    p.pushKV("trusted_slot",f.policy.trusted_slot); p.pushKV("fork_schedule_valid_through_epoch",f.policy.fork_schedule_valid_through_epoch);
    p.pushKV("max_sync_lag_slots",f.policy.max_sync_lag_slots); p.pushKV("max_recovery_lag_slots",f.policy.max_recovery_lag_slots);
    p.pushKV("min_participants",342); return p;
}
}

BOOST_AUTO_TEST_CASE(bootstrap_hash_is_not_an_execution_state_root_without_rlp_preimage) {
    fx::Fixture f; gl::LightClient client{f.policy};
    BOOST_CHECK(!client.FinalizedHash(f.slot)); BOOST_CHECK(!client.Latest(f.slot,f.execution_header));
    f.Bootstrap(client);
    BOOST_REQUIRE(client.FinalizedHash(f.slot)); BOOST_CHECK(!client.Latest(f.slot,{}));
    const auto cap{client.Latest(f.slot,f.execution_header)}; BOOST_REQUIRE(cap);
    BOOST_CHECK_EQUAL(cap->ChainId(),11'155'111U); BOOST_CHECK(cap->GenesisValidatorsRoot()==uint256{std::span{gl::GENESIS_VALIDATORS_ROOT}});
    BOOST_CHECK(cap->Header().state_root==fx::Tag(0x33)); BOOST_CHECK_EQUAL(cap->Header().block_number,1'000'000U);
    auto altered{f.execution_header}; altered.back()^=1; BOOST_CHECK(!client.Latest(f.slot,altered));
}

BOOST_AUTO_TEST_CASE(real_bls_update_advances_only_after_durable_append) {
    fx::Fixture f; gl::LightClient client{f.policy}; f.Bootstrap(client);
    const auto old{client.Latest(f.slot,f.execution_header)}; BOOST_REQUIRE(old);
    const auto a{SignedUpdate(f)};
    bool called{false};
    const auto reply{client.Submit(Wire(a.update),a.update.signature_slot,[&](auto bytes) {
        called=true; BOOST_CHECK_EQUAL(client.Sequence(),1U); BOOST_CHECK(client.IsCurrent(*old,f.slot));
        f.journal.emplace_back(bytes.begin(),bytes.end()); return true;
    })};
    BOOST_CHECK(called); BOOST_REQUIRE(reply.result==Result::OK); BOOST_CHECK_EQUAL(client.Sequence(),2U);
    BOOST_CHECK(!client.IsCurrent(*old,a.update.signature_slot)); BOOST_CHECK(!client.Latest(a.update.signature_slot,f.execution_header));
    const auto cap{client.Latest(a.update.signature_slot,a.raw)}; BOOST_REQUIRE(cap);
    BOOST_CHECK_EQUAL(cap->Header().block_number,1'000'001U); BOOST_CHECK(client.IsCurrent(*cap,a.update.signature_slot));
}

BOOST_AUTO_TEST_CASE(journal_replays_every_proof_and_binds_the_owning_native_instance) {
    fx::Fixture f; gl::LightClient owner{f.policy}; f.Bootstrap(owner); const auto a{SignedUpdate(f)};
    BOOST_REQUIRE(owner.Submit(Wire(a.update),a.update.signature_slot,f.Sink()).result==Result::OK);
    const auto cap{owner.Latest(a.update.signature_slot,a.raw)}; BOOST_REQUIRE(cap);
    gl::LightClient reopened{f.policy};
    for (const auto& entry:f.journal) { BOOST_REQUIRE(reopened.ReplayEntry(entry).result==Result::OK); BOOST_CHECK(!reopened.Latest(a.update.signature_slot,a.raw)); }
    BOOST_REQUIRE(reopened.EndReplay()); BOOST_CHECK(reopened.State()==owner.State());
    BOOST_CHECK(!reopened.IsCurrent(*cap,a.update.signature_slot)); BOOST_REQUIRE(reopened.Latest(a.update.signature_slot,a.raw));
}

BOOST_AUTO_TEST_CASE(storage_false_or_exception_fences_all_execution_capabilities) {
    for (bool throwing:{false,true}) {
        fx::Fixture f; gl::LightClient c{f.policy}; f.Bootstrap(c); const auto cap{c.Latest(f.slot,f.execution_header)}; BOOST_REQUIRE(cap);
        const auto a{SignedUpdate(f)};
        const auto result{c.Submit(Wire(a.update),a.update.signature_slot,[&](auto bytes)->bool {
            f.journal.emplace_back(bytes.begin(),bytes.end()); if (throwing) throw std::runtime_error("uncertain write"); return false;
        })};
        BOOST_CHECK(result.result==Result::STORAGE_FENCED); BOOST_CHECK(c.Fenced()); BOOST_CHECK_EQUAL(c.Sequence(),1U);
        BOOST_CHECK(!c.Latest(a.update.signature_slot,a.raw)); BOOST_CHECK(!c.IsCurrent(*cap,a.update.signature_slot));
        gl::LightClient replay{f.policy}; for (const auto& entry:f.journal) BOOST_REQUIRE(replay.ReplayEntry(entry).result==Result::OK);
        BOOST_REQUIRE(replay.EndReplay()); BOOST_REQUIRE(replay.Latest(a.update.signature_slot,a.raw));
    }
}

BOOST_AUTO_TEST_CASE(signatures_branches_and_domains_are_not_transport_authority) {
    for (unsigned mutation{0};mutation<7;++mutation) {
        fx::Fixture f; gl::LightClient c{f.policy}; f.Bootstrap(c); auto a{SignedUpdate(f)};
        if (mutation==0) a.update.sync.signature[10]^=1;
        if (mutation==1) a.update.finality_branch[2]=fx::Tag(0x81);
        if (mutation==2) a.update.finalized.execution_block_hash=fx::Tag(0x82);
        if (mutation==3) a.update.next_branch[0]=fx::Tag(0x83);
        if (mutation==4) a.update.next.pubkeys[0][10]^=1;
        if (mutation==5) a.update.sync.bits[1]^=1;
        if (mutation==6) {
            const auto domain{bridge::ssz::SyncCommitteeDomain(std::array<unsigned char,4>{6,0,0,0},uint256{std::span{gl::GENESIS_VALIDATORS_ROOT}})};
            a.update.sync.signature=fx::Sign(bridge::ssz::SigningRoot(a.update.attested.beacon.HashTreeRoot(),domain));
        }
        const auto before{c.JournalDigest()}; const auto result{c.Submit(Wire(a.update),a.update.signature_slot,f.Sink())};
        BOOST_CHECK(result.result==Result::BAD_PROOF); BOOST_CHECK_EQUAL(c.Sequence(),1U); BOOST_CHECK(c.JournalDigest()==before); BOOST_CHECK(!c.Fenced());
    }
}

BOOST_AUTO_TEST_CASE(participation_threshold_is_checked_with_actual_aggregate_signatures) {
    for (size_t participants:{341U,342U,512U}) {
        fx::Fixture f; gl::LightClient c{f.policy}; f.Bootstrap(c); const auto a{SignedUpdate(f,0,true,participants)};
        const auto r{c.Submit(Wire(a.update),a.update.signature_slot,f.Sink())};
        BOOST_CHECK(r.result==(participants<342 ? Result::BAD_PROOF : Result::OK));
        BOOST_CHECK_EQUAL(c.Sequence(),participants<342 ? 1U : 2U);
    }
}

BOOST_AUTO_TEST_CASE(committee_rotation_requires_one_proven_period_at_a_time) {
    fx::Fixture f; gl::LightClient c{f.policy}; f.Bootstrap(c); const auto a{SignedUpdate(f)};
    BOOST_REQUIRE(c.Submit(Wire(a.update),a.update.signature_slot,f.Sink()).result==Result::OK);
    const auto period{bridge::PeriodAtSlot(f.slot)}; const auto base{(period+1)*bridge::SLOTS_PER_PERIOD};
    const auto b{SignedUpdate(f,base)};
    BOOST_REQUIRE(c.Submit(Wire(b.update),b.update.signature_slot,f.Sink()).result==Result::OK);
    BOOST_CHECK_EQUAL(c.State()->period,period+1); BOOST_REQUIRE(c.Latest(b.update.signature_slot,b.raw));
    const auto before{c.JournalDigest()}; const auto skipped{SignedUpdate(f,base+2*bridge::SLOTS_PER_PERIOD)};
    BOOST_CHECK(c.Submit(Wire(skipped.update),skipped.update.signature_slot,f.Sink()).result!=Result::OK);
    BOOST_CHECK(c.JournalDigest()==before);
}

BOOST_AUTO_TEST_CASE(stale_recovery_future_and_unknown_fork_never_fast_forward) {
    fx::Fixture f; gl::LightClient stale{f.policy}; BOOST_REQUIRE(stale.EndReplay());
    BOOST_CHECK(stale.Submit(f.Record(),f.slot+bridge::SLOTS_PER_PERIOD+1,f.Sink()).result==Result::STALE_AUTHORITY);
    BOOST_CHECK(!stale.State()); BOOST_CHECK(f.journal.empty());
    gl::LightClient c{f.policy}; f.Bootstrap(c); const auto cap{c.Latest(f.slot,f.execution_header)}; BOOST_REQUIRE(cap);
    const auto a{SignedUpdate(f)};
    BOOST_CHECK(c.Submit(Wire(a.update),f.slot,f.Sink()).result==Result::FUTURE_SLOT);
    BOOST_CHECK(!c.IsCurrent(*cap,f.slot+bridge::SLOTS_PER_PERIOD+1));
    BOOST_CHECK(!c.FinalizedHash((gl::REVIEWED_THROUGH_EPOCH+1)*32));
}

BOOST_AUTO_TEST_CASE(codec_and_journal_refuse_tamper_truncation_trailing_and_v1_aliases) {
    fx::Fixture f; const auto record{f.Record()}; BOOST_REQUIRE(gl::DecodeRecord(record));
    auto trailing{record}; trailing.push_back(0); BOOST_CHECK(!gl::DecodeRecord(trailing));
    BOOST_CHECK(!gl::DecodeRecord(std::span{record}.first(record.size()-1)));
    auto old{record}; old[0]='E'; BOOST_CHECK(!gl::DecodeRecord(old));
    gl::LightClient c{f.policy}; f.Bootstrap(c);
    for (size_t i:{0U,8U,16U,48U,80U,100U}) {
        auto bad{f.journal[0]}; bad[i]^=1; gl::LightClient replay{f.policy};
        BOOST_CHECK(replay.ReplayEntry(bad).result==Result::BAD_JOURNAL); BOOST_CHECK(replay.Fenced());
    }
    auto bad{f.journal[0]}; std::copy(bridge::peer::JOURNAL_MAGIC.begin(),bridge::peer::JOURNAL_MAGIC.end(),bad.begin());
    gl::LightClient replay{f.policy}; BOOST_CHECK(replay.ReplayEntry(bad).result==Result::BAD_JOURNAL);
}

BOOST_AUTO_TEST_CASE(exact_amsterdam_header_schema_is_required_even_before_hash_match) {
    fx::Fixture f; BOOST_REQUIRE(gl::DecodeExecutionHeader(f.execution_header));
    const auto top{bridge::RlpDecode(f.execution_header)}; BOOST_REQUIRE(top); const auto fields{bridge::RlpChildren(*top)}; BOOST_REQUIRE(fields);
    std::vector<gl::Bytes> out; for (const auto& item:*fields) out.push_back(bridge::RlpEncodeBytes(item.payload));
    auto fewer{out}; fewer.pop_back(); BOOST_CHECK(!gl::DecodeExecutionHeader(bridge::RlpEncodeList(fewer)));
    auto more{out}; more.push_back(bridge::RlpEncodeUint64(0)); BOOST_CHECK(!gl::DecodeExecutionHeader(bridge::RlpEncodeList(more)));
    out[3]=bridge::RlpEncodeBytes(gl::Bytes(31,1)); BOOST_CHECK(!gl::DecodeExecutionHeader(bridge::RlpEncodeList(out)));
    BOOST_CHECK(!gl::DecodeExecutionHeader(gl::Bytes(4097,1)));
}

BOOST_AUTO_TEST_CASE(policy_parser_has_distinct_network_version_and_fixed_genesis_domain) {
    fx::Fixture f; const auto good{PolicyJson(f)}; BOOST_CHECK(gl::json::ParsePolicy(good).Digest()==f.policy.Digest());
    for (const auto& key:std::vector<std::string>{"chain_id","genesis_time","gloas_fork_epoch","gloas_fork_version","genesis_validators_root","format","mainnet_enabled"}) {
        UniValue changed{UniValue::VOBJ};
        for (const auto& field:good.getKeys()) {
            if (field!=key) changed.pushKV(field,good[field]);
            else if (key=="mainnet_enabled") changed.pushKV(key,true);
            else if (key=="format") changed.pushKV(key,"b3-ethereum-peer-policy/1");
            else if (key=="gloas_fork_version") changed.pushKV(key,"0x06000000");
            else if (key=="genesis_validators_root") changed.pushKV(key,bridge::lcjson::EthereumHex(fx::Tag(0x91)));
            else changed.pushKV(key,1);
        }
        BOOST_CHECK_THROW(gl::json::ParsePolicy(changed),std::exception);
    }
    auto p{f.policy}; p.fork_schedule_valid_through_epoch=gl::REVIEWED_THROUGH_EPOCH+1; BOOST_CHECK(!p.Valid());
    p=f.policy; p.max_recovery_lag_slots=bridge::SLOTS_PER_PERIOD+1; BOOST_CHECK(!p.Valid());
    BOOST_CHECK_THROW(gl::json::Read("{\"kind\":\"bootstrap\",\"kind\":\"update\"}"),std::exception);
}
BOOST_AUTO_TEST_SUITE_END()
