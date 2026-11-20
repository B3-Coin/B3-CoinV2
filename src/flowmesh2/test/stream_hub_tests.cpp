// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/facts.h>
#include <flowmesh2/net/stream_hub.h>
#include <flowmesh2/sim/sim_client.h>
#include <flowmesh2/sim/sim_node.h>
#include <flowmesh2/sim/trace.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/require.h>
#include <flowmesh2/test/util/storage_fixtures.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

/** The stream hub (M5-API-02, O-103) through a simulated k = 1 node: every
 * channel gives a SNAPSHOT then DELTAs with monotone cursors naming the slot,
 * untouched channels stay silent, a queue that overflows reports a gap once
 * and a resubscription from the last cursor replays each later slot exactly
 * once, and subscriptions are checked and bounded.
 */
using namespace flowmesh2;
using namespace flowmesh2::net;
using namespace flowmesh2::sim;
using namespace flowmesh2::test;

namespace {

struct Rig {
    TraceRecorder trace;
    std::unique_ptr<SimNode> node;
    std::optional<SimClient> alice;
    std::optional<SimClient> bob;

    Rig()
    {
        SimNodeConfig config;
        Result<std::unique_ptr<SimNode>> created{SimNode::Create(config, trace)};
        RequireOk(created, "SimNode::Create");
        node = std::move(created).value();
        const std::vector<MarketDescriptor> markets{node->Markets().begin(), node->Markets().end()};
        alice = SimClient::Create("alice", node->Profile(), node->Domain(), node->Seats(), markets).value();
        bob = SimClient::Create("bob", node->Profile(), node->Domain(), node->Seats(), markets).value();
        std::vector<Fact> facts;
        DepositFact a;
        a.cursor = FactCursor{SIM_BASE_HEIGHT + 1, 0};
        a.outpoint = Outpoint{SimLabelHash("fm2/test/stream/T1"), 0};
        a.account = alice->Account();
        a.asset = SimAsset("rUSD");
        a.amount = 1'000'000'000;
        DepositFact b;
        b.cursor = FactCursor{SIM_BASE_HEIGHT + 1, 1};
        b.outpoint = Outpoint{SimLabelHash("fm2/test/stream/T2"), 0};
        b.account = bob->Account();
        b.asset = AssetId::Native();
        b.amount = 10'000'000'000;
        facts.push_back(Fact{a});
        facts.push_back(Fact{b});
        RequireOk(node->Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 1), facts), "AppendBlock");
    }

    //! Submit `frame`, pump, require ADMITTED.
    void Admit(const std::vector<unsigned char>& frame, std::string_view what)
    {
        const Result<RequestId> id{node->Api().Submit(frame)};
        RequireOk(id, what);
        RequireOk(node->Pump(1), "Pump");
        const std::optional<ApiReply> reply{node->Api().TakeReply(*id)};
        BOOST_REQUIRE(reply.has_value());
        const auto* admitted{std::get_if<admission::AdmissionReply>(&reply->body)};
        BOOST_REQUIRE_MESSAGE(admitted != nullptr && admitted->code == AdmissionCode::ADMITTED, std::string{what} + " not admitted");
    }

    std::vector<StreamEvent> Take(StreamId id, size_t max = 64)
    {
        Result<std::vector<StreamEvent>> events{node->Api().TakeEvents(id, max)};
        RequireOk(events, "TakeEvents");
        return std::move(events).value();
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(stream_hub_tests)

BOOST_AUTO_TEST_CASE(slots_accounts_books_and_trades_stream_a_snapshot_then_deltas)
{
    Rig rig;
    const MarketDescriptor m1{SimMarketM1()};
    const MarketId market{m1.Id()};
    StreamHub& hub{rig.node->Seat().Streams()};
    BOOST_CHECK_EQUAL(hub.PublishedHeight(), 0U);
    // Before any publication a subscription waits; its SNAPSHOT comes with slot 1.
    const Result<StreamId> slots{rig.node->Api().Subscribe(StreamRequest{StreamChannel::SLOTS, std::nullopt, std::nullopt, std::nullopt})};
    const Result<StreamId> account{rig.node->Api().Subscribe(StreamRequest{StreamChannel::ACCOUNT, rig.alice->Account(), std::nullopt, std::nullopt})};
    const Result<StreamId> book{rig.node->Api().Subscribe(StreamRequest{StreamChannel::BOOK, std::nullopt, market, std::nullopt})};
    const Result<StreamId> trades{rig.node->Api().Subscribe(StreamRequest{StreamChannel::TRADES, std::nullopt, market, std::nullopt})};
    RequireOk(slots, "Subscribe SLOTS");
    RequireOk(account, "Subscribe ACCOUNT");
    RequireOk(book, "Subscribe BOOK");
    RequireOk(trades, "Subscribe TRADES");
    BOOST_CHECK_EQUAL(hub.Subscriptions(), 4U);
    BOOST_CHECK(rig.Take(*slots).empty());

    // Slot 1: the deposits. Every channel gets its SNAPSHOT; the account's leaf holds the deposit.
    RequireOk(rig.node->SealSlot(), "SealSlot 1");
    {
        const std::vector<StreamEvent> got{rig.Take(*slots)};
        BOOST_REQUIRE_EQUAL(got.size(), 1U);
        BOOST_CHECK(got[0].kind == StreamEventKind::SNAPSHOT);
        BOOST_CHECK(got[0].cursor == (StreamCursor{hub.StreamEpoch(), 1, 1}));
        const auto* slot{std::get_if<SlotEvent>(&got[0].body)};
        BOOST_REQUIRE(slot != nullptr);
        BOOST_CHECK_EQUAL(slot->height, 1U);
        BOOST_CHECK(slot->header_hash == rig.node->Sealed()[0].head->HeaderHash());
        BOOST_CHECK_EQUAL(slot->envelope.size(), rig.node->Sealed()[0].view->Envelope().Encode().size());
        const std::vector<StreamEvent> acc{rig.Take(*account)};
        BOOST_REQUIRE_EQUAL(acc.size(), 1U);
        const auto* leaf{std::get_if<AccountEvent>(&acc[0].body)};
        BOOST_REQUIRE(leaf != nullptr && leaf->leaf.has_value());
        BOOST_CHECK(leaf->deltas.empty()); // a SNAPSHOT carries the leaf, not the slot's deltas
        BOOST_CHECK_EQUAL(leaf->leaf->balances.size(), 1U);
        BOOST_CHECK_EQUAL(leaf->leaf->balances[0].available, 1'000'000'000);
        const std::vector<StreamEvent> bk{rig.Take(*book)};
        BOOST_REQUIRE_EQUAL(bk.size(), 1U);
        BOOST_CHECK(std::get<BookEvent>(bk[0].body).bids.empty() && std::get<BookEvent>(bk[0].body).asks.empty());
        const std::vector<StreamEvent> tr{rig.Take(*trades)};
        BOOST_REQUIRE_EQUAL(tr.size(), 1U);
        BOOST_CHECK(tr[0].kind == StreamEventKind::SNAPSHOT && std::get<TradesEvent>(tr[0].body).fills.empty());
    }

    // Slot 2: alice's bid rests. SLOTS and BOOK and ACCOUNT (reserved quote) move; TRADES is silent.
    rig.Admit(rig.alice->Place("bid", 1, m1, Side::BUY, TimeInForce::GTC, 100, 3, 1).value(), "bid");
    RequireOk(rig.node->SealSlot(), "SealSlot 2");
    {
        const std::vector<StreamEvent> got{rig.Take(*slots)};
        BOOST_REQUIRE_EQUAL(got.size(), 1U);
        BOOST_CHECK(got[0].kind == StreamEventKind::DELTA);
        BOOST_CHECK(got[0].cursor == (StreamCursor{hub.StreamEpoch(), 2, 2}));
        const std::vector<StreamEvent> bk{rig.Take(*book)};
        BOOST_REQUIRE_EQUAL(bk.size(), 1U);
        const BookEvent& ladder{std::get<BookEvent>(bk[0].body)};
        BOOST_REQUIRE_EQUAL(ladder.bids.size(), 1U);
        BOOST_CHECK(ladder.bids[0] == (BookLevel{100, 3, 1}));
        BOOST_CHECK(ladder.asks.empty());
        const std::vector<StreamEvent> acc{rig.Take(*account)};
        BOOST_REQUIRE_EQUAL(acc.size(), 1U);
        const AccountEvent& a{std::get<AccountEvent>(acc[0].body)};
        BOOST_CHECK(!a.deltas.empty());
        BOOST_CHECK(a.leaf.has_value() && a.leaf->open_orders == 1);
        BOOST_CHECK(rig.Take(*trades).empty());
    }

    // Slot 3: bob's ask takes it. TRADES has the fill, BOOK empties, ACCOUNT moves.
    rig.Admit(rig.bob->Place("ask", 1, m1, Side::SELL, TimeInForce::GTC, 100, 3, 2).value(), "ask");
    RequireOk(rig.node->SealSlot(), "SealSlot 3");
    {
        const std::vector<StreamEvent> tr{rig.Take(*trades)};
        BOOST_REQUIRE_EQUAL(tr.size(), 1U);
        BOOST_CHECK(tr[0].kind == StreamEventKind::DELTA);
        BOOST_CHECK_EQUAL(tr[0].cursor.head_slot, 3U);
        const TradesEvent& t{std::get<TradesEvent>(tr[0].body)};
        BOOST_REQUIRE_EQUAL(t.fills.size(), 1U);
        BOOST_CHECK_EQUAL(t.fills[0].lots, 3U);
        BOOST_CHECK_EQUAL(t.fills[0].price_ticks, 100U);
        const std::vector<StreamEvent> bk{rig.Take(*book)};
        BOOST_REQUIRE_EQUAL(bk.size(), 1U);
        BOOST_CHECK(std::get<BookEvent>(bk[0].body).bids.empty());
        const std::vector<StreamEvent> acc{rig.Take(*account)};
        BOOST_REQUIRE_EQUAL(acc.size(), 1U);
        BOOST_CHECK(std::get<AccountEvent>(acc[0].body).leaf->balances.size() == 2); // rUSD and the 3 lots of B3
        const std::vector<StreamEvent> got{rig.Take(*slots)};
        BOOST_REQUIRE_EQUAL(got.size(), 1U);
        BOOST_CHECK(got[0].cursor == (StreamCursor{hub.StreamEpoch(), 3, 3}));
    }
    // Slot 4: a heartbeat. Only SLOTS moves.
    RequireOk(rig.node->SealSlot(), "SealSlot 4");
    BOOST_CHECK_EQUAL(rig.Take(*slots).size(), 1U);
    BOOST_CHECK(rig.Take(*account).empty());
    BOOST_CHECK(rig.Take(*book).empty());
    BOOST_CHECK(rig.Take(*trades).empty());
    RequireOk(rig.node->Api().Unsubscribe(*trades), "Unsubscribe");
    RequireRefusal(rig.node->Api().TakeEvents(*trades, 1), ErrorCode::INVALID_INPUT, STREAM_UNKNOWN, "a gone subscription");
    BOOST_CHECK_EQUAL(hub.Subscriptions(), 3U);
    BOOST_CHECK_EQUAL(hub.PublishedHeight(), 4U);
}

BOOST_AUTO_TEST_CASE(a_gap_is_reported_once_and_a_resubscription_replays_each_slot_once)
{
    Rig rig;
    StreamHub& hub{rig.node->Seat().Streams()};
    // A hub of its own with a tiny queue shows the gap; it is fed by hand from the node's publications.
    Result<std::unique_ptr<StreamHub>> tiny{StreamHub::Create(StreamHubLimits{4, 2, 8}, 77)};
    RequireOk(tiny, "StreamHub::Create");
    const Result<StreamId> id{(*tiny)->Subscribe(StreamRequest{StreamChannel::SLOTS, std::nullopt, std::nullopt, std::nullopt})};
    RequireOk(id, "Subscribe");
    for (int i{0}; i < 3; ++i) {
        RequireOk(rig.node->SealSlot(), "SealSlot");
        const SealedSlot& sealed{rig.node->Sealed().back()};
        (*tiny)->Publish(sealed.head, sealed.view);
    }
    RequireRefusal((*tiny)->TakeEvents(*id, 10), ErrorCode::NEED_DATA, STREAM_GAP, "the overflow");
    RequireRefusal((*tiny)->TakeEvents(*id, 10), ErrorCode::INVALID_INPUT, STREAM_UNKNOWN, "reported once, then gone");
    // Resume from slot 1: slots 2 and 3 are replayed as DELTAs, each once, then slot 4 arrives live.
    StreamRequest resume{StreamChannel::SLOTS, std::nullopt, std::nullopt, StreamCursor{77, 1, 1}};
    const Result<StreamId> again{(*tiny)->Subscribe(resume)};
    RequireOk(again, "resubscribe");
    std::vector<StreamEvent> replayed{(*tiny)->TakeEvents(*again, 10).value()};
    BOOST_REQUIRE_EQUAL(replayed.size(), 2U);
    BOOST_CHECK(replayed[0].kind == StreamEventKind::DELTA && replayed[0].cursor.head_slot == 2 && replayed[0].cursor.seq == 1);
    BOOST_CHECK(replayed[1].kind == StreamEventKind::DELTA && replayed[1].cursor.head_slot == 3 && replayed[1].cursor.seq == 2);
    RequireOk(rig.node->SealSlot(), "SealSlot 4");
    (*tiny)->Publish(rig.node->Sealed().back().head, rig.node->Sealed().back().view);
    replayed = (*tiny)->TakeEvents(*again, 10).value();
    BOOST_REQUIRE_EQUAL(replayed.size(), 1U);
    BOOST_CHECK(replayed[0].cursor.head_slot == 4 && replayed[0].cursor.seq == 3);
    // A cursor of another epoch, or older than the ring, gives a fresh SNAPSHOT of the latest slot.
    const Result<StreamId> other_epoch{(*tiny)->Subscribe(StreamRequest{StreamChannel::SLOTS, std::nullopt, std::nullopt, StreamCursor{78, 5, 3}})};
    RequireOk(other_epoch, "another epoch");
    std::vector<StreamEvent> fresh{(*tiny)->TakeEvents(*other_epoch, 10).value()};
    BOOST_REQUIRE_EQUAL(fresh.size(), 1U);
    BOOST_CHECK(fresh[0].kind == StreamEventKind::SNAPSHOT && fresh[0].cursor.head_slot == 4);
    // The node's own hub has nothing to do with the tiny one.
    BOOST_CHECK_EQUAL(hub.PublishedHeight(), 4U);
}

BOOST_AUTO_TEST_CASE(subscriptions_are_checked_and_bounded)
{
    Rig rig;
    RequireRefusal(rig.node->Api().Subscribe(StreamRequest{StreamChannel::ACCOUNT, std::nullopt, std::nullopt, std::nullopt}), ErrorCode::INVALID_INPUT,
                   STREAM_FILTER, "ACCOUNT without an account");
    RequireRefusal(rig.node->Api().Subscribe(StreamRequest{StreamChannel::BOOK, std::nullopt, std::nullopt, std::nullopt}), ErrorCode::INVALID_INPUT,
                   STREAM_FILTER, "BOOK without a market");
    RequireRefusal(rig.node->Api().Subscribe(StreamRequest{StreamChannel{9}, std::nullopt, std::nullopt, std::nullopt}), ErrorCode::INVALID_INPUT,
                   STREAM_CHANNEL_UNKNOWN, "an unregistered channel");
    RequireOk(rig.node->SealSlot(), "SealSlot 1");
    RequireRefusal(rig.node->Api().Subscribe(StreamRequest{StreamChannel::TRADES, std::nullopt, MarketId{uint256::ONE}, std::nullopt}),
                   ErrorCode::INVALID_INPUT, STREAM_UNKNOWN_MARKET, "an unknown market");
    RequireRefusal(StreamHub::Create(StreamHubLimits{0, 1, 1}, 1), ErrorCode::INVALID_INPUT, STREAM_LIMIT_ZERO, "zero limit");
    Result<std::unique_ptr<StreamHub>> small{StreamHub::Create(StreamHubLimits{2, 8, 8}, 5)};
    RequireOk(small, "a hub of two");
    RequireOk((*small)->Subscribe(StreamRequest{StreamChannel::SLOTS, std::nullopt, std::nullopt, std::nullopt}), "first");
    RequireOk((*small)->Subscribe(StreamRequest{StreamChannel::SLOTS, std::nullopt, std::nullopt, std::nullopt}), "second");
    RequireRefusal((*small)->Subscribe(StreamRequest{StreamChannel::SLOTS, std::nullopt, std::nullopt, std::nullopt}), ErrorCode::BUSY, STREAM_LIMIT, "third");
    RequireRefusal((*small)->Unsubscribe(StreamId{99}), ErrorCode::INVALID_INPUT, STREAM_UNKNOWN, "unknown id");
    BOOST_CHECK_EQUAL((*small)->StreamEpoch(), 5U);
    BOOST_CHECK_EQUAL(std::string{ToString(StreamEventKind::SNAPSHOT)}, "snapshot");
}

BOOST_AUTO_TEST_SUITE_END()
