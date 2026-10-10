// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Fresh-lineage vote transport `finsigv2` (bridge V2 stage S3,
// b3-bridge-v2-finality-dependency.md section 6.5 "Votes"): the 148-byte
// wire layout, the separate per-peer / whole-node token buckets, the
// per-connection replay of the verified pool, the pool's verification-budget
// hook and, on one FinalityChainFixture chain with a fresh lineage under V1
// keys, the real PeerManager path (accept, flood once, duplicates, wrong
// sizes, replay to a new peer, V1 budget independence, and a node without
// the fresh lineage ignoring the message).

#include <addrman.h>
#include <crypto/bls.h>
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <modern/finality_schedule.h>
#include <modern/finality_v2.h>
#include <net.h>
#include <net_processing.h>
#include <netgroup.h>
#include <node/finality_fresh_lineage.h>
#include <node/finality_fresh_signature.h>
#include <node/finality_fresh_transport.h>
#include <node/finality_signature.h>
#include <protocol.h>
#include <streams.h>
#include <test/util/finality_fixture.h>
#include <test/util/net.h>
#include <test/util/time.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstring>
#include <set>
#include <span>
#include <vector>

using namespace std::chrono_literals;
using node::FreshLineageSig;
using node::FreshLineageSignaturePool;
using node::FreshVoteBucketLimits;
using node::FreshVoteReplay;
using node::FreshVoteTokenBucket;

namespace {

FreshLineageSig Vote(const uint32_t index, const uint64_t height = 100)
{
    FreshLineageSig sig;
    sig.lineage_id = uint256::ONE;
    sig.epoch = 0;
    sig.height = height;
    sig.index = index;
    sig.signature.fill(0x5c);
    return sig;
}

std::vector<FreshLineageSig> Votes(const uint32_t count, const uint32_t first = 0)
{
    std::vector<FreshLineageSig> out;
    for (uint32_t i{0}; i < count; ++i) out.push_back(Vote(first + i));
    return out;
}

node::FreshVoteExport Export(const std::vector<FreshLineageSig>& votes) { return node::FreshVoteExport{votes}; }

bool Contains(const std::vector<FreshLineageSig>& votes, const FreshLineageSig& sig)
{
    return std::find(votes.begin(), votes.end(), sig) != votes.end();
}

std::chrono::microseconds SteadyNow()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(MockableSteadyClock::now().time_since_epoch());
}

struct DrainedMessages {
    std::vector<FreshLineageSig> fresh;
    size_t finsig{0};
};

/** Drain actual mocked wire messages (as finality_transport_tests does). */
DrainedMessages Drain(CNode& peer)
{
    LOCK(peer.cs_vSend);
    DrainedMessages out;
    const auto decode = [&](const std::string& type, const std::span<const uint8_t> payload) {
        if (type == NetMsgType::FINSIG) ++out.finsig;
        if (type != NetMsgType::FINSIGV2) return;
        BOOST_CHECK_EQUAL(payload.size(), FreshLineageSig::SERIALIZED_SIZE);
        SpanReader reader{std::as_bytes(payload)};
        FreshLineageSig sig;
        reader >> sig;
        BOOST_CHECK(reader.empty());
        out.fresh.push_back(sig);
    };
    std::vector<uint8_t> wire;
    while (true) {
        const auto& [bytes, more, type]{peer.m_transport->GetBytesToSend(false)};
        if (bytes.empty()) break;
        wire.insert(wire.end(), bytes.begin(), bytes.end());
        peer.m_transport->MarkBytesSent(bytes.size());
    }
    size_t offset{0};
    while (offset + 24 <= wire.size()) {
        std::string type(reinterpret_cast<const char*>(wire.data() + offset + 4), 12);
        type.resize(strnlen(type.c_str(), 12));
        const uint32_t size{ReadLE32(wire.data() + offset + 16)};
        BOOST_REQUIRE_LE(offset + 24 + size, wire.size());
        decode(type, std::span<const uint8_t>{wire}.subspan(offset + 24, size));
        offset += 24 + size;
    }
    for (const CSerializedNetMsg& msg : peer.vSendMsg) decode(msg.m_type, std::span<const uint8_t>{msg.data});
    peer.vSendMsg.clear();
    peer.m_send_memusage = 0;
    peer.fPauseSend = false;
    return out;
}

struct FreshTransportNetSetup : b3test::FinalityChainFixture {
    SteadyClockContext transport_clock;
    std::set<NodeId> live;
    int m_G{0};
    int m_Rf{0};

    void PrepareFresh()
    {
        PrepareFinalityChain();
        m_G = m_M + 10;
        m_Rf = m_G + 1;
        MutableConsensus().fresh_lineage = Consensus::FreshLineageParams{
            .activation_height = m_Rf, .max_epoch_extension = 30, .staging_signer = true};
    }

    void SetupNetwork()
    {
        m_node.netgroupman = std::make_unique<NetGroupManager>(NetGroupManager::NoAsmap());
        m_node.addrman = std::make_unique<AddrMan>(*m_node.netgroupman, false, 0);
        m_node.connman = std::make_unique<ConnmanTestMsg>(0x1337, 0x1337, *m_node.addrman,
                                                        *m_node.netgroupman, Params());
        PeerManager::Options opts;
        opts.deterministic_rng = true;
        m_node.peerman = PeerManager::make(*m_node.connman, *m_node.addrman, nullptr,
                                           *m_node.chainman, *m_node.mempool, *m_node.warnings, opts);
        CConnman::Options options;
        options.m_msgproc = m_node.peerman.get();
        options.nSendBufferMaxSize = 1024 * 1024;
        m_node.connman->Init(options);
    }

    ConnmanTestMsg& Connman() { return static_cast<ConnmanTestMsg&>(*m_node.connman); }

    CNode& Connect(const NodeId id, const bool finish_handshake = true)
        EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
    {
        struct in_addr address;
        address.s_addr = 0xa0b0c001 + id;
        auto* peer = new CNode(id, nullptr, CAddress{CService{CNetAddr{address}, Params().GetDefaultPort()}, NODE_NONE},
                               0, 0, CAddress{}, "", ConnectionType::OUTBOUND_FULL_RELAY, false, 0);
        Connman().AddTestNode(*peer);
        live.insert(id);
        Connman().Handshake(*peer, finish_handshake, ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                            ServiceFlags(NODE_NETWORK | NODE_WITNESS), B3_MODERN_PROTOCOL_VERSION, true);
        BOOST_REQUIRE(!peer->fDisconnect);
        return *peer;
    }

    void ReceiveRaw(CNode& peer, const std::string& type, std::vector<unsigned char> payload)
        EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
    {
        CSerializedNetMsg msg;
        msg.m_type = type;
        msg.data = std::move(payload);
        BOOST_REQUIRE(Connman().ReceiveMsgFrom(peer, std::move(msg)));
        peer.fPauseSend = false;
        Connman().ProcessMessagesOnce(peer);
    }

    void Receive(CNode& peer, const FreshLineageSig& sig)
        EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
    {
        BOOST_REQUIRE(Connman().ReceiveMsgFrom(peer, NetMsg::Make(NetMsgType::FINSIGV2, sig)));
        peer.fPauseSend = false;
        Connman().ProcessMessagesOnce(peer);
    }

    void Disconnect(CNode& peer)
    {
        peer.fDisconnect = true;
        m_node.peerman->FinalizeNode(peer);
        live.erase(peer.GetId());
    }

    //! The vote of (sk, vk) for fresh checkpoint `checkpoint` on the tip's fresh state.
    FreshLineageSig FreshVote(const bls::SecretKey& sk, const modern::ValidatorKeyBytes& vk, const int checkpoint)
    {
        LOCK(cs_main);
        const auto& state{Finality().Current()};
        BOOST_REQUIRE(state.fresh && state.fresh->bootstrapped);
        const auto epoch{modern::EpochOfHeight(state.fresh->epoch_starts, checkpoint)};
        BOOST_REQUIRE(epoch.has_value());
        const CChain& chain{m_node.chainman->ActiveChain()};
        const modern::FreshHashAtFn hash_at{[&chain](const int h) -> std::optional<uint256> {
            const CBlockIndex* index{chain[h]};
            if (!index) return std::nullopt;
            return index->GetBlockHash();
        }};
        const auto fb{node::ExpectedFinalizedBlockV2(*epoch, checkpoint, *state.fresh, hash_at,
                                                     m_node.chainman->GetConsensus(), nullptr)};
        BOOST_REQUIRE(fb.has_value());
        const auto set{state.fresh->SetForEpoch(*epoch)};
        BOOST_REQUIRE(set);
        const auto index{set->IndexOf(vk)};
        BOOST_REQUIRE(index.has_value());
        const uint256 digest{modern::FinalityDigestV2(m_domain, *fb)};
        FreshLineageSig sig;
        sig.lineage_id = fb->lineage_id;
        sig.epoch = *epoch;
        sig.height = static_cast<uint64_t>(checkpoint);
        sig.index = *index;
        sig.signature = sk.Sign(std::span<const unsigned char>(digest.begin(), 32)).Compressed();
        return sig;
    }

    FreshLineageSignaturePool& FreshPool() EXCLUSIVE_LOCKS_REQUIRED(cs_main)
    {
        return m_node.chainman->ActiveChainstate().FinalitySignatures().Fresh();
    }
    size_t PooledVotes(const FreshLineageSig& sig)
    {
        return WITH_LOCK(cs_main, return FreshPool().SignatureCount(sig.epoch, sig.height));
    }

    ~FreshTransportNetSetup()
    {
        if (!m_node.peerman) return;
        LOCK(NetEventsInterface::g_msgproc_mutex);
        for (CNode* peer : Connman().TestNodes()) {
            if (live.count(peer->GetId())) m_node.peerman->FinalizeNode(*peer);
        }
        Connman().ClearTestNodes();
        m_node.peerman.reset();
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(finality_fresh_transport_tests)

BOOST_AUTO_TEST_CASE(vote_message_is_148_bytes_with_little_endian_integers)
{
    BOOST_CHECK_EQUAL(std::string{NetMsgType::FINSIGV2}, "finsigv2");
    BOOST_CHECK(std::count(ALL_NET_MESSAGE_TYPES.begin(), ALL_NET_MESSAGE_TYPES.end(), NetMsgType::FINSIGV2) == 1);
    // Appended right after V1 `finsig`, as protocol.h orders the list.
    const auto finsig{std::find(ALL_NET_MESSAGE_TYPES.begin(), ALL_NET_MESSAGE_TYPES.end(), NetMsgType::FINSIG)};
    BOOST_REQUIRE(finsig != ALL_NET_MESSAGE_TYPES.end() && finsig + 1 != ALL_NET_MESSAGE_TYPES.end());
    BOOST_CHECK_EQUAL(*(finsig + 1), NetMsgType::FINSIGV2);

    FreshLineageSig layout;
    for (size_t i{0}; i < 32; ++i) layout.lineage_id.begin()[i] = static_cast<unsigned char>(i + 1);
    layout.epoch = 0x0102030405060708;
    layout.height = 0x1122334455667788;
    layout.index = 0xa1b2c3d4;
    for (size_t i{0}; i < layout.signature.size(); ++i) layout.signature[i] = static_cast<unsigned char>(0x80 + i);

    const CSerializedNetMsg msg{NetMsg::Make(NetMsgType::FINSIGV2, layout)};
    BOOST_CHECK_EQUAL(msg.m_type, "finsigv2");
    const std::vector<unsigned char>& bytes{msg.data};
    BOOST_REQUIRE_EQUAL(bytes.size(), FreshLineageSig::SERIALIZED_SIZE);
    BOOST_REQUIRE_EQUAL(bytes.size(), 148U);
    BOOST_CHECK(std::equal(bytes.begin(), bytes.begin() + 32, layout.lineage_id.begin()));
    BOOST_CHECK_EQUAL(ReadLE64(bytes.data() + 32), layout.epoch);
    BOOST_CHECK_EQUAL(bytes[32], 0x08);
    BOOST_CHECK_EQUAL(ReadLE64(bytes.data() + 40), layout.height);
    BOOST_CHECK_EQUAL(bytes[40], 0x88);
    BOOST_CHECK_EQUAL(ReadLE32(bytes.data() + 48), layout.index);
    BOOST_CHECK_EQUAL(bytes[48], 0xd4);
    BOOST_CHECK(std::equal(bytes.begin() + 52, bytes.end(), layout.signature.begin()));

    SpanReader reader{std::as_bytes(std::span{bytes})};
    FreshLineageSig back;
    reader >> back;
    BOOST_CHECK(reader.empty());
    BOOST_CHECK(back == layout);

    // Complete-message identity: single SHA256 of exactly these bytes, and
    // any changed byte (here one signature bit) is another message.
    uint256 expected;
    CSHA256().Write(bytes.data(), bytes.size()).Finalize(expected.begin());
    BOOST_CHECK(node::FreshVoteId(layout) == expected);
    auto flipped{layout};
    flipped.signature[95] ^= 1;
    BOOST_CHECK(node::FreshVoteId(flipped) != expected);
}

BOOST_AUTO_TEST_CASE(token_buckets_burst_refuse_refill_and_share_the_global_bucket)
{
    // The shipped limits repeat V1 `finsig` (net_processing.cpp) in their
    // own buckets: B-22's bounded committee is out of scope, so one full
    // maximum-size committee still fits in a peer's verification burst.
    BOOST_CHECK_EQUAL(node::FRESH_VOTE_PEER_WIRE.rate_per_second, 128.0);
    BOOST_CHECK_EQUAL(node::FRESH_VOTE_PEER_WIRE.capacity, 2 * modern::MAX_FINALITY_SET);
    BOOST_CHECK_EQUAL(node::FRESH_VOTE_GLOBAL_WIRE.rate_per_second, 512.0);
    BOOST_CHECK_EQUAL(node::FRESH_VOTE_GLOBAL_WIRE.capacity, 4 * modern::MAX_FINALITY_SET);
    BOOST_CHECK_EQUAL(node::FRESH_VOTE_PEER_VERIFY.rate_per_second, 32.0);
    BOOST_CHECK_EQUAL(node::FRESH_VOTE_PEER_VERIFY.capacity, modern::MAX_FINALITY_SET);
    BOOST_CHECK_EQUAL(node::FRESH_VOTE_GLOBAL_VERIFY.rate_per_second, 128.0);
    BOOST_CHECK_EQUAL(node::FRESH_VOTE_GLOBAL_VERIFY.capacity, 2 * modern::MAX_FINALITY_SET);

    SteadyClockContext clock;
    constexpr FreshVoteBucketLimits peer_limits{2.0, 3};
    constexpr FreshVoteBucketLimits global_limits{4.0, 5};
    const auto t0{SteadyNow()};
    FreshVoteTokenBucket a{node::FullFreshVoteBucket(peer_limits, t0)};
    FreshVoteTokenBucket b{node::FullFreshVoteBucket(peer_limits, t0)};
    FreshVoteTokenBucket global{node::FullFreshVoteBucket(global_limits, t0)};
    BOOST_CHECK_EQUAL(a.tokens, 3.0);
    BOOST_CHECK(a.updated == t0);

    // Burst: peer A spends its whole bucket, then is refused; a refusal takes
    // nothing from the shared bucket.
    for (int i{0}; i < 3; ++i) BOOST_CHECK(node::ConsumeFreshVoteToken(a, peer_limits, global, global_limits, SteadyNow()));
    BOOST_CHECK(!node::ConsumeFreshVoteToken(a, peer_limits, global, global_limits, SteadyNow()));
    BOOST_CHECK_EQUAL(global.tokens, 2.0);
    // The global bucket is shared: B has its own three tokens but the node
    // has two left, and B's refused attempt costs B nothing.
    BOOST_CHECK(node::ConsumeFreshVoteToken(b, peer_limits, global, global_limits, SteadyNow()));
    BOOST_CHECK(node::ConsumeFreshVoteToken(b, peer_limits, global, global_limits, SteadyNow()));
    BOOST_CHECK(!node::ConsumeFreshVoteToken(b, peer_limits, global, global_limits, SteadyNow()));
    BOOST_CHECK_EQUAL(b.tokens, 1.0);
    BOOST_CHECK_EQUAL(global.tokens, 0.0);

    // Refill follows the mocked steady clock: 0.25 s gives A 0.5 tokens (not
    // enough) and the node 1; 0.5 s gives A one token.
    clock += 250ms;
    BOOST_CHECK(!node::ConsumeFreshVoteToken(a, peer_limits, global, global_limits, SteadyNow()));
    BOOST_CHECK_EQUAL(global.tokens, 1.0);
    clock += 250ms;
    BOOST_CHECK(node::ConsumeFreshVoteToken(a, peer_limits, global, global_limits, SteadyNow()));
    BOOST_CHECK(!node::ConsumeFreshVoteToken(a, peer_limits, global, global_limits, SteadyNow()));
    // A long pause refills only to capacity.
    clock += 100s;
    for (int i{0}; i < 3; ++i) BOOST_CHECK(node::ConsumeFreshVoteToken(a, peer_limits, global, global_limits, SteadyNow()));
    BOOST_CHECK(!node::ConsumeFreshVoteToken(a, peer_limits, global, global_limits, SteadyNow()));
    BOOST_CHECK_EQUAL(global.tokens, 2.0);
    // A clock that does not advance (or an older timestamp) credits nothing.
    const auto updated{a.updated};
    BOOST_CHECK(!node::ConsumeFreshVoteToken(a, peer_limits, global, global_limits, updated - 1s));
    BOOST_CHECK(a.updated == updated);
    BOOST_CHECK_EQUAL(a.tokens, 0.0);
}

BOOST_AUTO_TEST_CASE(replay_sends_once_per_tick_resends_after_resend_prunes_and_caps)
{
    FreshVoteReplay replay;
    const auto t0{std::chrono::microseconds{10s}};
    std::vector<FreshLineageSig> pool{Votes(3)};

    // First take: everything not yet sent.
    BOOST_CHECK(replay.Due(t0));
    BOOST_CHECK(replay.Take(Export(pool), t0) == pool);
    BOOST_CHECK_EQUAL(replay.Tracked(), 3U);
    // Nothing within one TICK, even with a new vote in the pool.
    pool.push_back(Vote(3));
    BOOST_CHECK(!replay.Due(t0 + 999ms));
    BOOST_CHECK(replay.Take(Export(pool), t0 + 999ms).empty());
    // One TICK later: only the vote not yet sent.
    const auto t1{t0 + FreshVoteReplay::TICK};
    BOOST_CHECK(replay.Take(Export(pool), t1) == std::vector<FreshLineageSig>{Vote(3)});
    BOOST_CHECK(replay.Take(Export(pool), t1 + 1s).empty());
    // RESEND after the first send: the first three again, not the fourth.
    BOOST_CHECK(replay.Take(Export(pool), t0 + FreshVoteReplay::RESEND - 1s).empty());
    BOOST_CHECK(replay.Take(Export(pool), t0 + FreshVoteReplay::RESEND) == Votes(3));
    BOOST_CHECK(replay.Take(Export(pool), t1 + FreshVoteReplay::RESEND) == std::vector<FreshLineageSig>{Vote(3)});

    // Prune: a vote no longer verified (finalized, reorged out, evicted) is
    // remembered until RESEND after its last send, so an export that lags
    // never causes an echo; after that it is forgotten, and when it is
    // verified again it goes out immediately. Votes 0-2 were last sent at
    // t0 + RESEND, vote 3 at t1 + RESEND.
    const auto t2{t1 + FreshVoteReplay::RESEND + 1s};
    const std::vector<FreshLineageSig> without_first{pool.begin() + 1, pool.end()};
    BOOST_CHECK(replay.Take(Export(without_first), t2).empty());
    BOOST_CHECK_EQUAL(replay.Tracked(), 4U);
    BOOST_CHECK(replay.Take(Export(pool), t2 + 1s).empty());
    const auto t3{t0 + 2 * FreshVoteReplay::RESEND};
    BOOST_CHECK(replay.Take(Export(without_first), t3) == (std::vector<FreshLineageSig>{Vote(1), Vote(2)}));
    BOOST_CHECK_EQUAL(replay.Tracked(), 3U);
    // Vote 0 is new again; vote 3 reached RESEND since t1 + RESEND.
    BOOST_CHECK(replay.Take(Export(pool), t3 + 1s) == (std::vector<FreshLineageSig>{Vote(0), Vote(3)}));
    BOOST_CHECK(replay.Take(node::FreshVoteExport{}, t3 + 2s).empty());
    BOOST_CHECK_EQUAL(replay.Tracked(), 4U); // all sent less than RESEND ago
    BOOST_CHECK(replay.Take(node::FreshVoteExport{}, t3 + 1s + FreshVoteReplay::RESEND).empty());
    BOOST_CHECK_EQUAL(replay.Tracked(), 0U);

    // MarkSent: a vote the peer already has (it relayed it, or we flooded
    // it) waits for RESEND; marking twice keeps the later time.
    FreshVoteReplay marked;
    marked.MarkSent(Vote(1), t0);
    marked.MarkSent(Vote(1), t0 + 2s);
    BOOST_CHECK(marked.Take(Export(Votes(2)), t0 + 3s) == std::vector<FreshLineageSig>{Vote(0)});
    BOOST_CHECK(marked.Take(Export(Votes(2)), t0 + 2s + FreshVoteReplay::RESEND - 1s).empty());
    BOOST_CHECK(marked.Take(Export(Votes(2)), t0 + 2s + FreshVoteReplay::RESEND) == std::vector<FreshLineageSig>{Vote(1)});

    // The echo guard holds across a stale export: a vote the peer relayed to
    // us (MarkSent) that the shared export does not contain yet (it is
    // refreshed at most once a second) is still remembered when the next
    // export does contain it, so it is not sent back before RESEND.
    FreshVoteReplay echo;
    echo.MarkSent(Vote(5), t0);
    BOOST_CHECK(echo.Take(Export(Votes(2)), t0 + 100ms) == Votes(2));
    BOOST_CHECK_EQUAL(echo.Tracked(), 3U);
    std::vector<FreshLineageSig> refreshed{Votes(2)};
    refreshed.push_back(Vote(5));
    BOOST_CHECK(echo.Take(Export(refreshed), t0 + 100ms + FreshVoteReplay::TICK).empty());
    BOOST_CHECK(echo.Take(Export(refreshed), t0 + FreshVoteReplay::RESEND - 1s).empty());
    BOOST_CHECK(echo.Take(Export(refreshed), t0 + FreshVoteReplay::RESEND) == std::vector<FreshLineageSig>{Vote(5)});
    // The export carries each vote's id once, in pool order.
    {
        const node::FreshVoteExport exported{refreshed};
        BOOST_REQUIRE_EQUAL(exported.ids.size(), refreshed.size());
        for (size_t i{0}; i < refreshed.size(); ++i) BOOST_CHECK(exported.ids[i] == node::FreshVoteId(refreshed[i]));
        BOOST_CHECK_EQUAL(exported.live.size(), refreshed.size());
    }

    // MAX_BATCH caps one take; the rest follows on the next TICK, in pool
    // order (newest checkpoint first).
    FreshVoteReplay capped;
    const std::vector<FreshLineageSig> many{Votes(static_cast<uint32_t>(FreshVoteReplay::MAX_BATCH) + 36)};
    const auto first{capped.Take(Export(many), t0)};
    BOOST_REQUIRE_EQUAL(first.size(), FreshVoteReplay::MAX_BATCH);
    BOOST_CHECK(std::equal(first.begin(), first.end(), many.begin()));
    const auto second{capped.Take(Export(many), t0 + FreshVoteReplay::TICK)};
    BOOST_REQUIRE_EQUAL(second.size(), 36U);
    BOOST_CHECK(std::equal(second.begin(), second.end(), many.begin() + FreshVoteReplay::MAX_BATCH));
    BOOST_CHECK(capped.Take(Export(many), t0 + 2 * FreshVoteReplay::TICK).empty());
    // A reconnect is a new replay: the whole pool again, capped.
    FreshVoteReplay reconnected;
    BOOST_CHECK_EQUAL(reconnected.Take(Export(many), t0 + 3s).size(), FreshVoteReplay::MAX_BATCH);
}

BOOST_FIXTURE_TEST_CASE(finsigv2_accepts_floods_once_replays_and_is_ignored_without_the_lineage,
                        FreshTransportNetSetup)
{
    PrepareFresh();
    const int G{m_G};
    ProduceTo(G + 8, m_vk_a); // fresh checkpoint G+5 is CHECKPOINT_DEPTH deep
    SetupNetwork();
    LOCK(NetEventsInterface::g_msgproc_mutex);
    CNode& source{Connect(0)};
    CNode& receiver{Connect(1)};
    Drain(source);
    Drain(receiver);

    const FreshLineageSig vote_a{FreshVote(m_bls_a, m_vk_a, G + 5)};
    const FreshLineageSig vote_b{FreshVote(m_bls_b, m_vk_b, G + 5)};
    BOOST_REQUIRE(vote_a.index != vote_b.index);

    // Pool hook: the verification budget is consulted once, only right
    // before BLS; a refusal stores nothing and a retry is permitted. An
    // exact duplicate and a cheap reject never reach it.
    {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        const Consensus::Params& params{m_node.chainman->GetConsensus()};
        FreshLineageSignaturePool pool;
        int calls{0};
        const auto refuse = [&] { ++calls; return false; };
        const auto allow = [&] { ++calls; return true; };
        BOOST_CHECK(pool.Submit(vote_a, Finality(), chain, params, nullptr, refuse) ==
                    FreshLineageSignaturePool::Accept::VERIFICATION_DEFERRED);
        BOOST_CHECK_EQUAL(FreshLineageSignaturePool::AcceptName(FreshLineageSignaturePool::Accept::VERIFICATION_DEFERRED),
                          std::string{"verification-deferred"});
        BOOST_CHECK_EQUAL(calls, 1);
        BOOST_CHECK_EQUAL(pool.SignatureCount(vote_a.epoch, vote_a.height), 0U);
        BOOST_CHECK(pool.Submit(vote_a, Finality(), chain, params, nullptr, allow) ==
                    FreshLineageSignaturePool::Accept::ACCEPTED);
        BOOST_CHECK_EQUAL(calls, 2);
        BOOST_CHECK(pool.Submit(vote_a, Finality(), chain, params, nullptr, allow) ==
                    FreshLineageSignaturePool::Accept::DUPLICATE);
        auto wrong_lineage{vote_b};
        wrong_lineage.lineage_id = uint256::ONE;
        BOOST_CHECK(pool.Submit(wrong_lineage, Finality(), chain, params, nullptr, allow) ==
                    FreshLineageSignaturePool::Accept::LINEAGE_MISMATCH);
        BOOST_CHECK_EQUAL(calls, 2);
    }

    // A valid vote over finsigv2 enters pool.Fresh() and is flooded exactly
    // once to the other peer, never back to its source.
    Receive(source, vote_a);
    BOOST_CHECK_EQUAL(PooledVotes(vote_a), 1U);
    BOOST_CHECK(m_node.peerman->SendMessages(receiver));
    BOOST_CHECK(m_node.peerman->SendMessages(source));
    const auto flooded{Drain(receiver)};
    BOOST_REQUIRE_EQUAL(flooded.fresh.size(), 1U);
    BOOST_CHECK(flooded.fresh[0] == vote_a);
    BOOST_CHECK(Drain(source).fresh.empty());

    // The duplicate is neither re-flooded nor charged to the verification
    // bucket: more exact duplicates than one peer's verification burst
    // (MAX_FINALITY_SET), on a frozen clock, still leave a token for B's
    // vote. They fit in the separate, larger wire bucket.
    for (size_t i{0}; i < node::FRESH_VOTE_PEER_VERIFY.capacity; ++i) Receive(source, vote_a);
    BOOST_CHECK_EQUAL(PooledVotes(vote_a), 1U);
    BOOST_CHECK(Drain(receiver).fresh.empty());
    // V1 `finsig` budgets are separate too: exhaust this peer's V1 wire
    // bucket with cheap-to-reject 116-byte messages first.
    {
        const std::vector<unsigned char> junk(116, 0xff);
        const size_t v1_wire_capacity{2 * modern::MAX_FINALITY_SET};
        for (size_t i{0}; i < v1_wire_capacity + 1; ++i) ReceiveRaw(source, NetMsgType::FINSIG, junk);
    }
    BOOST_CHECK(!source.fDisconnect);

    // 147- and 149-byte messages are ignored without penalty or disconnect,
    // even when their first 148 bytes are B's valid vote.
    {
        DataStream ss;
        ss << vote_b;
        const std::vector<unsigned char> bytes(UCharCast(ss.data()), UCharCast(ss.data()) + ss.size());
        BOOST_REQUIRE_EQUAL(bytes.size(), 148U);
        ReceiveRaw(source, NetMsgType::FINSIGV2, {bytes.begin(), bytes.end() - 1});
        auto long_bytes{bytes};
        long_bytes.push_back(0);
        ReceiveRaw(source, NetMsgType::FINSIGV2, long_bytes);
        ReceiveRaw(source, NetMsgType::FINSIGV2, {});
    }
    BOOST_CHECK(m_node.peerman->SendMessages(source));
    BOOST_CHECK(!source.fDisconnect);
    BOOST_CHECK_EQUAL(PooledVotes(vote_b), 1U);
    BOOST_CHECK(Drain(receiver).fresh.empty());

    Receive(source, vote_b);
    BOOST_CHECK_EQUAL(PooledVotes(vote_b), 2U);
    const auto flooded_b{Drain(receiver)};
    BOOST_REQUIRE_EQUAL(flooded_b.fresh.size(), 1U);
    BOOST_CHECK(flooded_b.fresh[0] == vote_b);

    // Replay: nothing is echoed within RESEND to a peer that has the votes;
    // after RESEND the exact verified votes go out again (lost bytes).
    transport_clock += 2s;
    BOOST_CHECK(m_node.peerman->SendMessages(receiver));
    BOOST_CHECK(m_node.peerman->SendMessages(source));
    BOOST_CHECK(Drain(receiver).fresh.empty());
    BOOST_CHECK(Drain(source).fresh.empty());
    transport_clock += FreshVoteReplay::RESEND;
    receiver.fPauseSend = true;
    BOOST_CHECK(m_node.peerman->SendMessages(receiver));
    BOOST_CHECK(Drain(receiver).fresh.empty()); // respects backpressure, keeps the batch due
    BOOST_CHECK(m_node.peerman->SendMessages(receiver));
    const auto resent{Drain(receiver)};
    BOOST_REQUIRE_EQUAL(resent.fresh.size(), 2U);
    BOOST_CHECK(Contains(resent.fresh, vote_a) && Contains(resent.fresh, vote_b));
    BOOST_CHECK(m_node.peerman->SendMessages(receiver));
    BOOST_CHECK(Drain(receiver).fresh.empty()); // no send-loop flood

    // A pooled vote reaches a newly connected peer within one TICK.
    Disconnect(receiver);
    CNode& fresh_peer{Connect(2, false)};
    BOOST_REQUIRE(Connman().ReceiveMsgFrom(fresh_peer, NetMsg::Make(NetMsgType::VERACK)));
    fresh_peer.fPauseSend = false;
    Connman().ProcessMessagesOnce(fresh_peer);
    BOOST_CHECK(m_node.peerman->SendMessages(fresh_peer));
    const auto restored{Drain(fresh_peer)};
    BOOST_REQUIRE_EQUAL(restored.fresh.size(), 2U);
    BOOST_CHECK(Contains(restored.fresh, vote_a) && Contains(restored.fresh, vote_b));
    // A vote inserted outside P2P (the local signer, the hidden RPC) goes
    // out by replay within one TICK too; B's key signs a later checkpoint.
    Produce(m_vk_a);
    Produce(m_vk_a);
    Produce(m_vk_a);
    Produce(m_vk_a);
    Produce(m_vk_a); // G+13: fresh checkpoint G+10 is deep enough
    const FreshLineageSig local{FreshVote(m_bls_b, m_vk_b, G + 10)};
    {
        LOCK(cs_main);
        BOOST_CHECK(FreshPool().Submit(local, Finality(), m_node.chainman->ActiveChain(),
                                       m_node.chainman->GetConsensus()) ==
                    FreshLineageSignaturePool::Accept::ACCEPTED);
    }
    Drain(fresh_peer);
    transport_clock += FreshVoteReplay::TICK;
    BOOST_CHECK(m_node.peerman->SendMessages(fresh_peer));
    const auto local_relay{Drain(fresh_peer)};
    BOOST_CHECK(Contains(local_relay.fresh, local));

    // Without the fresh lineage (every shipped network) finsigv2 is ignored:
    // nothing is parsed into the pool, nothing is sent even once a replay
    // would be due, and the peer stays connected.
    const FreshLineageSig vote_a10{FreshVote(m_bls_a, m_vk_a, G + 10)};
    const auto pooled_before{PooledVotes(local)};
    BOOST_REQUIRE_EQUAL(pooled_before, 1U);
    MutableConsensus().fresh_lineage.reset();
    Receive(source, vote_a10);
    BOOST_CHECK_EQUAL(PooledVotes(local), pooled_before);
    transport_clock += FreshVoteReplay::RESEND + FreshVoteReplay::TICK;
    BOOST_CHECK(m_node.peerman->SendMessages(source));
    BOOST_CHECK(m_node.peerman->SendMessages(fresh_peer));
    BOOST_CHECK(Drain(source).fresh.empty());
    BOOST_CHECK(Drain(fresh_peer).fresh.empty());
    BOOST_CHECK(!source.fDisconnect);
    BOOST_CHECK(!fresh_peer.fDisconnect);
}

BOOST_FIXTURE_TEST_CASE(finsigv2_gate_wire_and_verify_budgets_bind_inside_net_processing, FreshTransportNetSetup)
{
    PrepareFresh();
    const int G{m_G};
    ProduceTo(G + 13, m_vk_a); // fresh checkpoints G+5 and G+10 are deep enough
    SetupNetwork();
    LOCK(NetEventsInterface::g_msgproc_mutex);
    // The transport clock stays frozen unless advanced: no bucket refills.
    const FreshLineageSig vote_a{FreshVote(m_bls_a, m_vk_a, G + 5)};
    const FreshLineageSig vote_b{FreshVote(m_bls_b, m_vk_b, G + 5)};
    const FreshLineageSig vote_a10{FreshVote(m_bls_a, m_vk_a, G + 10)};
    // Cheap to reject once admitted: another lineage id (LINEAGE_MISMATCH).
    auto cheap{vote_a};
    cheap.lineage_id = uint256::ONE;
    // Reaches the verification budget, then fails to decode (BAD_SIGNATURE).
    auto garbage{vote_a10};
    garbage.signature.fill(0x5c);

    // 1. The gate: without the fresh lineage (every shipped network) more
    //    finsigv2 messages than one peer's wire capacity are dropped before
    //    any budget is charged. With the lineage back, that peer's first
    //    valid vote is admitted and accepted.
    CNode& gated{Connect(0)};
    Drain(gated);
    BOOST_TEST_MESSAGE("gate: flooding with the lineage disabled");
    const auto lineage{m_node.chainman->GetConsensus().fresh_lineage};
    BOOST_REQUIRE(lineage);
    MutableConsensus().fresh_lineage.reset();
    for (size_t i{0}; i < node::FRESH_VOTE_PEER_WIRE.capacity + 1; ++i) Receive(gated, cheap);
    MutableConsensus().fresh_lineage = lineage;
    BOOST_CHECK(!gated.fDisconnect);
    Receive(gated, vote_a);
    BOOST_CHECK_EQUAL(PooledVotes(vote_a), 1U);

    // 2. The per-peer wire bucket: after exactly its capacity of cheap
    //    messages, a valid vote from the same peer is dropped at admission
    //    (not pooled, no penalty), while another peer's copy passes (the
    //    global wire bucket still has room).
    CNode& flooder{Connect(1)};
    Drain(flooder);
    BOOST_TEST_MESSAGE("per-peer wire bucket: cheap flood");
    for (size_t i{0}; i < node::FRESH_VOTE_PEER_WIRE.capacity; ++i) Receive(flooder, cheap);
    Receive(flooder, vote_b);
    BOOST_CHECK_EQUAL(PooledVotes(vote_b), 1U); // vote_a only
    BOOST_CHECK(!flooder.fDisconnect);
    CNode& other{Connect(2)};
    Drain(other);
    Receive(other, vote_b);
    BOOST_CHECK_EQUAL(PooledVotes(vote_b), 2U);

    // 3. The per-peer verification bucket: exactly its capacity of votes that
    //    reach BLS (and fail) exhausts it, so the same peer's valid vote is
    //    deferred (not pooled); one second of refill admits it.
    CNode& verifier{Connect(3)};
    Drain(verifier);
    BOOST_TEST_MESSAGE("per-peer verification bucket: BLS-failure flood");
    BOOST_REQUIRE_LT(node::FRESH_VOTE_PEER_VERIFY.capacity + 2, node::FRESH_VOTE_PEER_WIRE.capacity);
    for (size_t i{0}; i < node::FRESH_VOTE_PEER_VERIFY.capacity; ++i) Receive(verifier, garbage);
    BOOST_CHECK_EQUAL(PooledVotes(vote_a10), 0U);
    Receive(verifier, vote_a10);
    BOOST_CHECK_EQUAL(PooledVotes(vote_a10), 0U);
    BOOST_CHECK(!verifier.fDisconnect);
    transport_clock += 1s;
    Receive(verifier, vote_a10);
    BOOST_CHECK_EQUAL(PooledVotes(vote_a10), 1U);
    BOOST_TEST_MESSAGE("budgets checked");
}

BOOST_AUTO_TEST_SUITE_END()
