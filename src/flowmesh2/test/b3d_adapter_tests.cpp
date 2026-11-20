// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** The finalized b3d adapter (M7-ADP-01): creation rules, facts usable only
 * when finalized and deep, the monotone cursor, the PLAN-11 epoch rules
 * (a shallow reorg changes nothing, a reorg at or below the exposed anchor
 * bumps the epoch and rewinds, a seat-set block bumps the epoch when it
 * becomes usable, successive disconnects each bump and keep the lowest rewind),
 * anchor verification bounded by the usable anchor, anchor proofs, a block
 * larger than a slot, and link refusals that change nothing.
 */

#include <boost/test/unit_test.hpp>

#include <flowmesh2/chain/b3d_adapter.h>
#include <flowmesh2/chain/chain_adapter.h>
#include <flowmesh2/core/anchor_proof.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/test/util/agreement_fixtures.h>
#include <flowmesh2/test/util/storage_fixtures.h>
#include <util/chaintype.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;
using chain::B3dAdapterConfig;
using chain::B3dFinalizedAdapter;
using chain::ChainBlock;

namespace {

constexpr uint32_t DEPTH{2};
const Anchor BASE{129, BlockHash(129)};

std::unique_ptr<B3dFinalizedAdapter> NewAdapter(const SeatSet& seats, uint32_t depth = DEPTH)
{
    Result<std::unique_ptr<B3dFinalizedAdapter>> adapter{
        B3dFinalizedAdapter::Create(TestNetwork(), B3dAdapterConfig{depth}, SeatAuthority(seats), seats, BASE)};
    RequireOk(adapter, "B3dFinalizedAdapter::Create");
    return std::move(adapter).value();
}

//! The links of fork `fork` at heights first..last, the first hanging off `parent`.
std::vector<HeaderLink> PathOf(uint32_t first, uint32_t last, const uint256& parent, std::string_view fork = "main")
{
    std::vector<HeaderLink> path;
    uint256 previous{parent};
    for (uint32_t height{first}; height <= last; ++height) {
        path.push_back(HeaderLink{height, BlockHash(height, fork), previous});
        previous = BlockHash(height, fork);
    }
    return path;
}

//! Block `height` of fork `fork` whose parent is `parent`.
ChainBlock BlockOf(uint32_t height, const uint256& parent, std::vector<Fact> facts = {}, std::string_view fork = "main",
                   std::optional<SeatSet> seats = std::nullopt)
{
    return ChainBlock{Anchor{height, BlockHash(height, fork)}, parent, std::move(facts), std::move(seats)};
}

void Connect(B3dFinalizedAdapter& adapter, ChainBlock block)
{
    const std::string what{"connect " + std::to_string(block.block.height)};
    RequireOk(adapter.OnConnected(std::move(block)), what);
}

//! Connects empty blocks on top of the tip up to `to`, on fork `fork`.
void Extend(B3dFinalizedAdapter& adapter, uint32_t to, std::string_view fork = "main")
{
    while (adapter.Tip().height < to) {
        const Anchor tip{adapter.Tip()};
        Connect(adapter, BlockOf(tip.height + 1, tip.hash, {}, fork));
    }
}

void Finalize(B3dFinalizedAdapter& adapter, uint32_t height, std::string_view fork = "main")
{
    RequireOk(adapter.OnFinalized(Anchor{height, BlockHash(height, fork)}), "finalize " + std::to_string(height));
}

void Disconnect(B3dFinalizedAdapter& adapter, uint32_t height, std::string_view fork = "main")
{
    RequireOk(adapter.OnDisconnected(Anchor{height, BlockHash(height, fork)}), "disconnect " + std::to_string(height));
}

//! Blocks 130 (three deposits), 131 (none) and 132 (two deposits) on the main fork; returns every fact.
std::vector<Fact> ScriptThreeBlocks(B3dFinalizedAdapter& adapter)
{
    std::vector<Fact> b130{DepositAtCursor({130, 0}, "main/a"), DepositAtCursor({130, 1}, "main/b"), DepositAtCursor({130, 2}, "main/c")};
    std::vector<Fact> b132{DepositAtCursor({132, 0}, "main/d"), DepositAtCursor({132, 1}, "main/e")};
    std::vector<Fact> all{b130};
    all.insert(all.end(), b132.begin(), b132.end());
    Connect(adapter, BlockOf(130, BASE.hash, b130));
    Connect(adapter, BlockOf(131, BlockHash(130)));
    Connect(adapter, BlockOf(132, BlockHash(131), b132));
    return all;
}

chain::PollReply PollOk(B3dFinalizedAdapter& adapter, const FactCursor& after, size_t max = MAX_SLOT_FACTS)
{
    Result<chain::PollReply> reply{adapter.Poll(after, max)};
    RequireOk(reply, "Poll");
    return std::move(reply).value();
}

//! Requires a FACTS reply that passes CheckBatch and returns its batch.
chain::FactBatch RequireBatch(const chain::PollReply& reply, const FactCursor& after, size_t max = MAX_SLOT_FACTS)
{
    BOOST_REQUIRE_MESSAGE(reply.Status() == chain::PollStatus::FACTS, "status " + std::string{chain::ToString(reply.Status())});
    BOOST_REQUIRE(reply.Batch() != nullptr);
    const chain::FactBatch& batch{*reply.Batch()};
    const Result<void> ok{chain::CheckBatch(batch, after, max)};
    BOOST_CHECK_MESSAGE(ok.has_value(), "CheckBatch: " + std::string{ok.has_value() ? "" : ok.error().detail});
    BOOST_CHECK(reply.Authority() == batch.authority);
    return batch;
}

void CheckOk(const Result<void>& result, std::string_view what)
{
    BOOST_CHECK_MESSAGE(result.has_value(), std::string{what} + (result.has_value() ? std::string{} : ": " + std::string{result.error().detail}));
}

struct Snapshot {
    Anchor tip, finalized, exposed;
    AuthorityVersion authority;
    FactCursor end;
    friend bool operator==(const Snapshot&, const Snapshot&) = default;
};
Snapshot Snap(const B3dFinalizedAdapter& adapter)
{
    return Snapshot{adapter.Tip(), adapter.Finalized(), adapter.Exposed(), adapter.CurrentAuthority(), adapter.End()};
}

} // namespace

BOOST_AUTO_TEST_SUITE(b3d_adapter_tests)

BOOST_AUTO_TEST_CASE(create_checks_the_depth_rule_the_seats_and_the_base)
{
    const SeatSet seats{TestSeats({"s0", "s1", "s2", "s3"})};
    const AuthorityVersion authority{SeatAuthority(seats)};
    BOOST_CHECK_EQUAL(chain::FINALIZED_MIN_DEPTH, 30U);

    // TRD-4 holds on every real network; regtest may run shallower.
    for (const ChainType chain_type : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4, ChainType::SIGNET}) {
        NetworkId network{TestNetwork()};
        network.chain = chain_type;
        RequireRefusal(B3dFinalizedAdapter::Create(network, B3dAdapterConfig{29}, authority, seats, BASE), ErrorCode::INVALID_INPUT,
                       chain::B3D_MIN_DEPTH, "depth 29 on a real network");
        Result<std::unique_ptr<B3dFinalizedAdapter>> deep{B3dFinalizedAdapter::Create(network, B3dAdapterConfig{30}, authority, seats, BASE)};
        RequireOk(deep, "depth 30 on a real network");
        BOOST_CHECK(deep.value()->Network().chain == chain_type);
        BOOST_CHECK_EQUAL(deep.value()->MinDepth(), 30U);
    }
    RequireOk(B3dFinalizedAdapter::Create(TestNetwork(), B3dAdapterConfig{0}, authority, seats, BASE), "depth 0 on regtest");

    RequireRefusal(B3dFinalizedAdapter::Create(TestNetwork(), B3dAdapterConfig{DEPTH}, SeatAuthority(TestSeats({"other"})), seats, BASE),
                   ErrorCode::INVALID_INPUT, chain::B3D_SEAT_SET, "seat set of another authority");
    RequireRefusal(B3dFinalizedAdapter::Create(TestNetwork(), B3dAdapterConfig{DEPTH}, authority, seats, Anchor{129, uint256{}}),
                   ErrorCode::INVALID_INPUT, chain::B3D_BLOCK_HASH, "zero base hash");
    RequireRefusal(B3dFinalizedAdapter::Create(TestNetwork(), B3dAdapterConfig{DEPTH}, authority, seats, Anchor{0xffff'fffe, BlockHash(1)}),
                   ErrorCode::INVALID_INPUT, chain::B3D_BASE_HEIGHT, "base at 2^32 - 2");

    const auto adapter{NewAdapter(seats)};
    BOOST_CHECK(adapter->Network().chain == ChainType::REGTEST);
    BOOST_CHECK(adapter->Network().modern_chain_domain == TestNetwork().modern_chain_domain);
    BOOST_CHECK(adapter->CurrentAuthority() == authority);
    BOOST_CHECK(adapter->Base() == BASE);
    BOOST_CHECK(adapter->Tip() == BASE);
    BOOST_CHECK(adapter->Finalized() == BASE);
    BOOST_CHECK(adapter->Exposed() == BASE);
    BOOST_CHECK(adapter->Start() == (FactCursor{130, 0}));
    BOOST_CHECK(adapter->End() == (FactCursor{130, 0}));
    const Result<SeatSet> epoch0{adapter->SeatSetAt(0)};
    RequireOk(epoch0, "SeatSetAt(0)");
    BOOST_CHECK(epoch0.value().Root() == seats.Root());
    RequireRefusal(adapter->SeatSetAt(1), ErrorCode::NEED_DATA, chain::SEAT_SET_UNKNOWN_EPOCH, "SeatSetAt(1)");
    CheckOk(adapter->CheckAuthority(authority), "CheckAuthority");
}

BOOST_AUTO_TEST_CASE(facts_are_served_only_when_finalized_and_deep)
{
    const SeatSet seats{TestSeats({"s0", "s1", "s2", "s3"})};
    const auto adapter{NewAdapter(seats)};
    const std::vector<Fact> all{ScriptThreeBlocks(*adapter)};
    const FactCursor start{adapter->Start()};

    // Connected but not finalized: nothing is usable, the heartbeat is anchored at the base.
    BOOST_CHECK(adapter->Tip() == (Anchor{132, BlockHash(132)}));
    BOOST_CHECK(adapter->Exposed() == BASE);
    {
        const chain::FactBatch batch{RequireBatch(PollOk(*adapter, start), start)};
        BOOST_CHECK(batch.facts.empty());
        BOOST_CHECK(batch.anchor == BASE);
        BOOST_CHECK(batch.to == start);
    }
    // Finalized to the tip, but only block 130 is DEPTH below it.
    Finalize(*adapter, 132);
    BOOST_CHECK(adapter->Finalized() == (Anchor{132, BlockHash(132)}));
    BOOST_CHECK(adapter->Exposed() == (Anchor{130, BlockHash(130)}));
    {
        const chain::FactBatch batch{RequireBatch(PollOk(*adapter, start), start)};
        BOOST_REQUIRE_EQUAL(batch.facts.size(), 3U);
        BOOST_CHECK(std::vector<Fact>(all.begin(), all.begin() + 3) == batch.facts);
        BOOST_CHECK(batch.anchor == (Anchor{130, BlockHash(130)}));
        BOOST_CHECK(batch.to == (FactCursor{131, 0}));
    }
    // The chain grows: depth alone exposes 131 (empty) and then 132.
    Extend(*adapter, 133);
    BOOST_CHECK(adapter->Exposed() == (Anchor{131, BlockHash(131)}));
    {
        const chain::FactBatch batch{RequireBatch(PollOk(*adapter, {131, 0}), {131, 0})};
        BOOST_CHECK(batch.facts.empty());
        BOOST_CHECK(batch.anchor == (Anchor{131, BlockHash(131)}));
        BOOST_CHECK(batch.to == (FactCursor{132, 0}));
    }
    Extend(*adapter, 134);
    {
        const chain::FactBatch batch{RequireBatch(PollOk(*adapter, {131, 0}), {131, 0})};
        BOOST_REQUIRE_EQUAL(batch.facts.size(), 2U);
        BOOST_CHECK(std::vector<Fact>(all.begin() + 3, all.end()) == batch.facts);
        BOOST_CHECK(batch.anchor == (Anchor{132, BlockHash(132)}));
        BOOST_CHECK(batch.to == (FactCursor{133, 0}));
    }
    // Deep but not finalized: 133 stays unusable until the pin moves.
    Extend(*adapter, 136);
    BOOST_CHECK(adapter->Exposed() == (Anchor{132, BlockHash(132)}));
    BOOST_CHECK(adapter->End() == (FactCursor{133, 0}));
    {
        const chain::PollReply reply{PollOk(*adapter, {134, 0})};
        BOOST_CHECK(reply.Status() == chain::PollStatus::NEED_DATA);
        BOOST_CHECK(reply.Authority() == adapter->CurrentAuthority());
    }
    Finalize(*adapter, 135);
    BOOST_CHECK(adapter->Exposed() == (Anchor{134, BlockHash(134)}));
    {
        const chain::FactBatch batch{RequireBatch(PollOk(*adapter, {133, 0}), {133, 0})};
        BOOST_CHECK(batch.facts.empty());
        BOOST_CHECK(batch.anchor == (Anchor{134, BlockHash(134)}));
        BOOST_CHECK(batch.to == (FactCursor{135, 0}));
    }
    // Nothing above changed the authority.
    BOOST_CHECK(adapter->CurrentAuthority() == SeatAuthority(seats));
}

BOOST_AUTO_TEST_CASE(poll_cursor_is_monotone_and_cut_by_max)
{
    const SeatSet seats{TestSeats({"s0"})};
    for (const size_t max : {size_t{1}, size_t{2}, size_t{3}, size_t{4}, MAX_SLOT_FACTS}) {
        const auto adapter{NewAdapter(seats)};
        const std::vector<Fact> all{ScriptThreeBlocks(*adapter)};
        Extend(*adapter, 134);
        Finalize(*adapter, 134);
        BOOST_CHECK(adapter->End() == (FactCursor{133, 0}));

        FactCursor after{adapter->Start()};
        std::vector<Fact> seen;
        for (int round{0}; round < 10 && after < adapter->End(); ++round) {
            const chain::FactBatch batch{RequireBatch(PollOk(*adapter, after, max), after, max)};
            BOOST_CHECK(batch.anchor == adapter->Exposed());
            BOOST_CHECK(after < batch.to); // strictly advances while facts remain
            BOOST_CHECK_LE(batch.facts.size(), max);
            seen.insert(seen.end(), batch.facts.begin(), batch.facts.end());
            after = batch.to;
        }
        BOOST_CHECK(after == adapter->End());
        BOOST_CHECK(seen == all);
        const chain::FactBatch heartbeat{RequireBatch(PollOk(*adapter, after, max), after, max)};
        BOOST_CHECK(heartbeat.facts.empty());
        BOOST_CHECK(heartbeat.to == heartbeat.from);
        BOOST_CHECK(heartbeat.anchor == (Anchor{132, BlockHash(132)}));

        // Misuse is an error, not a reply.
        RequireRefusal(adapter->Poll({130, 0}, 0), ErrorCode::INVALID_INPUT, chain::POLL_MAX_FACTS, "max 0");
        RequireRefusal(adapter->Poll({130, 0}, MAX_SLOT_FACTS + 1), ErrorCode::INVALID_INPUT, chain::POLL_MAX_FACTS, "max above the cap");
        RequireRefusal(adapter->Poll({129, 0}, max), ErrorCode::INVALID_INPUT, chain::B3D_CURSOR_BEFORE_BASE, "cursor before the base");
        RequireRefusal(adapter->Poll({128, 7}, max), ErrorCode::INVALID_INPUT, chain::B3D_CURSOR_BEFORE_BASE, "cursor far before the base");
    }
}

BOOST_AUTO_TEST_CASE(a_reorg_above_the_exposed_anchor_keeps_the_epoch)
{
    const SeatSet seats{TestSeats({"s0", "s1", "s2", "s3"})};
    const AuthorityVersion authority{SeatAuthority(seats)};
    const auto adapter{NewAdapter(seats)};
    ScriptThreeBlocks(*adapter);
    Extend(*adapter, 134);
    Finalize(*adapter, 134);
    BOOST_CHECK(adapter->Exposed() == (Anchor{132, BlockHash(132)}));
    // The consumer took everything usable.
    const chain::FactBatch consumed{RequireBatch(PollOk(*adapter, adapter->Start()), adapter->Start())};
    BOOST_CHECK(consumed.to == (FactCursor{133, 0}));

    // Blocks 134 and 133 leave: above the exposed anchor, so nothing a consumer saw changes.
    Disconnect(*adapter, 134);
    Disconnect(*adapter, 133);
    BOOST_CHECK(adapter->Tip() == (Anchor{132, BlockHash(132)}));
    BOOST_CHECK(adapter->Finalized() == (Anchor{132, BlockHash(132)})); // the pin followed the chain down
    BOOST_CHECK(adapter->Exposed() == (Anchor{132, BlockHash(132)}));   // exposure is sticky: 132 is still on the chain
    BOOST_CHECK(adapter->CurrentAuthority() == authority);
    {
        const chain::FactBatch batch{RequireBatch(PollOk(*adapter, {133, 0}), {133, 0})};
        BOOST_CHECK(batch.facts.empty());
        BOOST_CHECK(batch.anchor == (Anchor{132, BlockHash(132)}));
    }
    // Another fork takes over from 132; its deposit becomes usable once finalized and deep.
    const std::vector<Fact> b133{DepositAtCursor({133, 0}, "b/x")};
    Connect(*adapter, BlockOf(133, BlockHash(132), b133, "b"));
    Extend(*adapter, 135, "b");
    BOOST_CHECK(adapter->Exposed() == (Anchor{132, BlockHash(132)})); // 133' not finalized
    Finalize(*adapter, 135, "b");
    BOOST_CHECK(adapter->Exposed() == (Anchor{133, BlockHash(133, "b")}));
    {
        const chain::FactBatch batch{RequireBatch(PollOk(*adapter, {133, 0}), {133, 0})};
        BOOST_CHECK(batch.facts == b133);
        BOOST_CHECK(batch.anchor == (Anchor{133, BlockHash(133, "b")}));
        BOOST_CHECK(batch.to == (FactCursor{134, 0}));
    }
    BOOST_CHECK(adapter->CurrentAuthority() == authority);
    CheckOk(adapter->VerifyAnchor(Anchor{133, BlockHash(133, "b")}, Anchor{132, BlockHash(132)}), "the fork's 133 descends from 132");
    RequireRefusal(adapter->VerifyAnchor(Anchor{133, BlockHash(133)}, Anchor{132, BlockHash(132)}), ErrorCode::VERIFY_FAILED,
                   chain::ANCHOR_NOT_ON_CHAIN, "the old 133 is gone");
}

BOOST_AUTO_TEST_CASE(a_reorg_at_or_below_the_exposed_anchor_bumps_the_epoch_and_rewinds)
{
    const SeatSet seats{TestSeats({"s0", "s1", "s2", "s3"})};
    const AuthorityVersion authority{SeatAuthority(seats)};
    const auto adapter{NewAdapter(seats)};
    ScriptThreeBlocks(*adapter);
    Extend(*adapter, 134);
    Finalize(*adapter, 134);
    const chain::FactBatch consumed{RequireBatch(PollOk(*adapter, adapter->Start()), adapter->Start())};
    BOOST_CHECK(consumed.to == (FactCursor{133, 0}));
    BOOST_CHECK(consumed.anchor == (Anchor{132, BlockHash(132)}));

    // 134 and 133 go: no change. 132 goes: the consumer's anchor is gone.
    Disconnect(*adapter, 134);
    Disconnect(*adapter, 133);
    BOOST_CHECK(adapter->CurrentAuthority() == authority);
    Disconnect(*adapter, 132);
    const AuthorityVersion bumped{adapter->CurrentAuthority()};
    BOOST_CHECK_EQUAL(bumped.epoch, authority.epoch + 1);
    BOOST_CHECK(bumped.seat_set_root == authority.seat_set_root); // the seat set did not change
    BOOST_CHECK(bumped.profile_hash == authority.profile_hash);
    BOOST_CHECK(adapter->Tip() == (Anchor{131, BlockHash(131)}));
    BOOST_CHECK(adapter->Finalized() == (Anchor{131, BlockHash(131)}));
    BOOST_CHECK(adapter->Exposed() == (Anchor{131, BlockHash(131)})); // the block below the removed one was exposed and is still on the chain
    RequireRefusal(adapter->CheckAuthority(authority), ErrorCode::AUTHORITY_CHANGED, chain::CHAIN_AUTHORITY_CHANGED, "old authority");
    const Result<SeatSet> epoch1{adapter->SeatSetAt(bumped.epoch)};
    RequireOk(epoch1, "SeatSetAt(1)");
    BOOST_CHECK(epoch1.value().Root() == seats.Root());

    // Every poll from after the rewind point says so, with the rewind cursor.
    for (const FactCursor after : {FactCursor{133, 0}, FactCursor{132, 1}, FactCursor{140, 0}}) {
        const chain::PollReply reply{PollOk(*adapter, after)};
        BOOST_CHECK(reply.Status() == chain::PollStatus::AUTHORITY_CHANGED);
        BOOST_REQUIRE(reply.RewindTo().has_value());
        BOOST_CHECK(*reply.RewindTo() == (FactCursor{132, 0}));
        BOOST_CHECK(reply.Authority() == bumped);
    }
    // Resuming at the rewind point clears it: the facts up to 131 are still there, under the new epoch.
    {
        const chain::FactBatch batch{RequireBatch(PollOk(*adapter, {132, 0}), {132, 0})};
        BOOST_CHECK(batch.facts.empty());
        BOOST_CHECK(batch.anchor == (Anchor{131, BlockHash(131)}));
        BOOST_CHECK(batch.authority == bumped);
    }
    {
        const chain::FactBatch batch{RequireBatch(PollOk(*adapter, {130, 0}), {130, 0})};
        BOOST_CHECK_EQUAL(batch.facts.size(), 3U); // block 130's deposits never left
        BOOST_CHECK(batch.anchor == (Anchor{131, BlockHash(131)}));
        BOOST_CHECK(batch.to == (FactCursor{132, 0}));
        BOOST_CHECK(batch.authority == bumped);
    }
    // The replacement chain: its facts arrive under the new epoch once finalized and deep.
    const std::vector<Fact> b132{DepositAtCursor({132, 0}, "b/y")};
    Connect(*adapter, BlockOf(132, BlockHash(131), b132, "b"));
    Extend(*adapter, 134, "b");
    Finalize(*adapter, 134, "b");
    BOOST_CHECK(adapter->Exposed() == (Anchor{132, BlockHash(132, "b")}));
    BOOST_CHECK(adapter->CurrentAuthority() == bumped); // re-exposing 130..132 changes no seat set
    {
        const chain::FactBatch batch{RequireBatch(PollOk(*adapter, {132, 0}), {132, 0})};
        BOOST_CHECK(batch.facts == b132);
        BOOST_CHECK(batch.anchor == (Anchor{132, BlockHash(132, "b")}));
        BOOST_CHECK(batch.authority == bumped);
    }
    // The old 132 is no anchor any more; the new one descends from 131.
    RequireRefusal(adapter->VerifyAnchor(Anchor{132, BlockHash(132)}, Anchor{131, BlockHash(131)}), ErrorCode::VERIFY_FAILED,
                   chain::ANCHOR_NOT_ON_CHAIN, "old 132");
    CheckOk(adapter->VerifyAnchor(Anchor{132, BlockHash(132, "b")}, Anchor{131, BlockHash(131)}), "new 132");
}

BOOST_AUTO_TEST_CASE(a_seat_set_block_bumps_the_epoch_when_it_becomes_usable)
{
    const SeatSet seats{TestSeats({"s0", "s1", "s2", "s3"})};
    const SeatSet seats2{TestSeats({"s0", "s1", "s2", "s3", "s4"})};
    const SeatSet seats3{TestSeats({"s0", "s1", "s2", "s3", "s4", "s5"})};
    const SeatSet seats4{TestSeats({"s0", "s1", "s2", "s3", "s4", "s5", "s6"})};
    const AuthorityVersion authority{SeatAuthority(seats)};
    const auto adapter{NewAdapter(seats)};
    const auto root_at = [&](uint64_t epoch) {
        const Result<SeatSet> set{adapter->SeatSetAt(epoch)};
        RequireOk(set, "SeatSetAt(" + std::to_string(epoch) + ")");
        return set.value().Root();
    };
    Connect(*adapter, BlockOf(130, BASE.hash));
    Connect(*adapter, BlockOf(131, BlockHash(130), {}, "main", seats2));
    Connect(*adapter, BlockOf(132, BlockHash(131), {}, "main", seats3));
    Connect(*adapter, BlockOf(133, BlockHash(132), {}, "main", seats4));
    Extend(*adapter, 134);
    Finalize(*adapter, 130);
    // Finalized to 130 only: 131 is deep but not final, the authority is unchanged.
    BOOST_CHECK(adapter->Exposed() == (Anchor{130, BlockHash(130)}));
    BOOST_CHECK(adapter->CurrentAuthority() == authority);

    // One finality move exposes 131 and 132: two epochs, in block order.
    Finalize(*adapter, 134);
    BOOST_CHECK(adapter->Exposed() == (Anchor{132, BlockHash(132)}));
    const AuthorityVersion epoch2{adapter->CurrentAuthority()};
    BOOST_CHECK_EQUAL(epoch2.epoch, 2U);
    BOOST_CHECK(epoch2.seat_set_root == seats3.Root());
    BOOST_CHECK(epoch2.profile_hash == authority.profile_hash);
    BOOST_CHECK(root_at(0) == seats.Root());
    BOOST_CHECK(root_at(1) == seats2.Root());
    BOOST_CHECK(root_at(2) == seats3.Root());
    RequireRefusal(adapter->SeatSetAt(3), ErrorCode::NEED_DATA, chain::SEAT_SET_UNKNOWN_EPOCH, "SeatSetAt(3)");
    {
        const chain::FactBatch batch{RequireBatch(PollOk(*adapter, adapter->Start()), adapter->Start())};
        BOOST_CHECK(batch.authority == epoch2);
        BOOST_CHECK(batch.anchor == (Anchor{132, BlockHash(132)}));
    }
    // A connect makes the finalized 133 deep enough: the bump comes from OnConnected.
    Extend(*adapter, 135);
    BOOST_CHECK(adapter->Exposed() == (Anchor{133, BlockHash(133)}));
    const AuthorityVersion epoch3{adapter->CurrentAuthority()};
    BOOST_CHECK_EQUAL(epoch3.epoch, 3U);
    BOOST_CHECK(epoch3.seat_set_root == seats4.Root());
    BOOST_CHECK(root_at(3) == seats4.Root());

    // A finality break: each exposed block that leaves bumps once, and the set in
    // force is the highest remaining exposed seat set, else the genesis set.
    Disconnect(*adapter, 135);
    Disconnect(*adapter, 134);
    BOOST_CHECK(adapter->CurrentAuthority() == epoch3); // above the exposed anchor
    BOOST_CHECK(adapter->Finalized() == (Anchor{133, BlockHash(133)}));
    Disconnect(*adapter, 133);
    const AuthorityVersion epoch4{adapter->CurrentAuthority()};
    BOOST_CHECK_EQUAL(epoch4.epoch, 4U);
    BOOST_CHECK(epoch4.seat_set_root == seats3.Root()); // 132 is still exposed and carries seats3
    BOOST_CHECK(adapter->Exposed() == (Anchor{132, BlockHash(132)}));
    Disconnect(*adapter, 132);
    const AuthorityVersion epoch5{adapter->CurrentAuthority()};
    BOOST_CHECK_EQUAL(epoch5.epoch, 5U);
    BOOST_CHECK(epoch5.seat_set_root == seats2.Root());
    BOOST_CHECK(adapter->Exposed() == (Anchor{131, BlockHash(131)}));
    Disconnect(*adapter, 131);
    const AuthorityVersion epoch6{adapter->CurrentAuthority()};
    BOOST_CHECK_EQUAL(epoch6.epoch, 6U);
    BOOST_CHECK(epoch6.seat_set_root == seats.Root()); // no exposed seat-set block left
    BOOST_CHECK(adapter->Exposed() == (Anchor{130, BlockHash(130)}));
    BOOST_CHECK(root_at(4) == seats3.Root());
    BOOST_CHECK(root_at(5) == seats2.Root());
    BOOST_CHECK(root_at(6) == seats.Root());
    {
        const chain::PollReply reply{PollOk(*adapter, {133, 0})};
        BOOST_CHECK(reply.Status() == chain::PollStatus::AUTHORITY_CHANGED);
        BOOST_REQUIRE(reply.RewindTo().has_value());
        BOOST_CHECK(*reply.RewindTo() == (FactCursor{131, 0})); // the lowest removed exposed block
        BOOST_CHECK(reply.Authority() == epoch6);
    }

    // The seat-set block returns on another fork and becomes usable again: epoch 7 with seats2.
    Connect(*adapter, BlockOf(131, BlockHash(130), {}, "b", seats2));
    Extend(*adapter, 134, "b");
    BOOST_CHECK(adapter->CurrentAuthority() == epoch6);
    Finalize(*adapter, 134, "b");
    const AuthorityVersion epoch7{adapter->CurrentAuthority()};
    BOOST_CHECK_EQUAL(epoch7.epoch, 7U);
    BOOST_CHECK(epoch7.seat_set_root == seats2.Root());
    BOOST_CHECK(root_at(7) == seats2.Root());
    BOOST_CHECK(adapter->Exposed() == (Anchor{132, BlockHash(132, "b")}));
    {
        const chain::FactBatch batch{RequireBatch(PollOk(*adapter, {131, 0}), {131, 0})}; // clears the rewind
        BOOST_CHECK(batch.facts.empty());
        BOOST_CHECK(batch.anchor == (Anchor{132, BlockHash(132, "b")}));
        BOOST_CHECK(batch.authority == epoch7);
    }
}

BOOST_AUTO_TEST_CASE(successive_disconnects_below_the_exposed_anchor_each_bump_and_keep_the_lowest_rewind)
{
    // The review scenario: a consumer that applied block 130's deposit and then
    // lagged must be told that 130 left, even though 132 left first.
    const SeatSet seats{TestSeats({"s0", "s1", "s2", "s3"})};
    const AuthorityVersion authority{SeatAuthority(seats)};
    const auto adapter{NewAdapter(seats)};
    const std::vector<Fact> b130{DepositAtCursor({130, 0}, "main/a")};
    Connect(*adapter, BlockOf(130, BASE.hash, b130));
    Extend(*adapter, 132);
    Finalize(*adapter, 132);
    {
        const chain::FactBatch batch{RequireBatch(PollOk(*adapter, adapter->Start()), adapter->Start())};
        BOOST_CHECK(batch.facts == b130);
        BOOST_CHECK(batch.to == (FactCursor{131, 0}));
    }
    Extend(*adapter, 134);
    Finalize(*adapter, 134);
    BOOST_CHECK(adapter->Exposed() == (Anchor{132, BlockHash(132)})); // the consumer is behind, at (131, 0)

    Disconnect(*adapter, 134);
    Disconnect(*adapter, 133);
    BOOST_CHECK(adapter->CurrentAuthority() == authority);
    Disconnect(*adapter, 132);
    BOOST_CHECK_EQUAL(adapter->CurrentAuthority().epoch, 1U);
    BOOST_CHECK(adapter->Exposed() == (Anchor{131, BlockHash(131)}));
    Disconnect(*adapter, 131);
    BOOST_CHECK_EQUAL(adapter->CurrentAuthority().epoch, 2U);
    BOOST_CHECK(adapter->Exposed() == (Anchor{130, BlockHash(130)}));
    Disconnect(*adapter, 130);
    const AuthorityVersion epoch3{adapter->CurrentAuthority()};
    BOOST_CHECK_EQUAL(epoch3.epoch, 3U);
    BOOST_CHECK(epoch3.seat_set_root == seats.Root());
    BOOST_CHECK(adapter->Exposed() == BASE);
    BOOST_CHECK(adapter->Finalized() == BASE);
    BOOST_CHECK(adapter->Tip() == BASE);

    // The consumer's cursor (131, 0) is above the lowest removed block: it must rewind to (130, 0).
    for (const FactCursor after : {FactCursor{131, 0}, FactCursor{130, 1}, FactCursor{133, 0}}) {
        const chain::PollReply reply{PollOk(*adapter, after)};
        BOOST_CHECK_MESSAGE(reply.Status() == chain::PollStatus::AUTHORITY_CHANGED, "status " + std::string{chain::ToString(reply.Status())});
        BOOST_REQUIRE(reply.RewindTo().has_value());
        BOOST_CHECK(*reply.RewindTo() == (FactCursor{130, 0}));
        BOOST_CHECK(reply.Authority() == epoch3);
    }
    {
        const chain::FactBatch batch{RequireBatch(PollOk(*adapter, {130, 0}), {130, 0})};
        BOOST_CHECK(batch.facts.empty());
        BOOST_CHECK(batch.anchor == BASE);
        BOOST_CHECK(batch.authority == epoch3);
    }
    // The replacement 130 carries another deposit; the orphaned one is never served again.
    const std::vector<Fact> b130b{DepositAtCursor({130, 0}, "b/x")};
    Connect(*adapter, BlockOf(130, BASE.hash, b130b, "b"));
    Extend(*adapter, 132, "b");
    Finalize(*adapter, 132, "b");
    BOOST_CHECK(adapter->Exposed() == (Anchor{130, BlockHash(130, "b")}));
    BOOST_CHECK(adapter->CurrentAuthority() == epoch3);
    {
        const chain::FactBatch batch{RequireBatch(PollOk(*adapter, {130, 0}), {130, 0})};
        BOOST_CHECK(batch.facts == b130b);
        BOOST_CHECK(batch.anchor == (Anchor{130, BlockHash(130, "b")}));
        BOOST_CHECK(batch.to == (FactCursor{131, 0}));
    }
}

BOOST_AUTO_TEST_CASE(a_block_with_more_facts_than_a_slot_holds_is_served_over_several_batches)
{
    // The adapter must never wedge on a legal chain block; Poll cuts by max_facts anyway.
    const SeatSet seats{TestSeats({"s0"})};
    const auto adapter{NewAdapter(seats)};
    std::vector<Fact> many;
    for (uint32_t i{0}; i < MAX_SLOT_FACTS + 3; ++i) many.push_back(DepositAtCursor({130, i}, "many/" + std::to_string(i)));
    Connect(*adapter, BlockOf(130, BASE.hash, many));
    // One outpoint twice in a block is still refused, however far apart.
    std::vector<Fact> repeated;
    for (uint32_t i{0}; i < MAX_SLOT_FACTS + 1; ++i) repeated.push_back(DepositAtCursor({131, i}, i == MAX_SLOT_FACTS ? "rep/0" : "rep/" + std::to_string(i)));
    RequireRefusal(adapter->OnConnected(BlockOf(131, BlockHash(130), repeated)), ErrorCode::INVALID_INPUT, chain::BATCH_DUPLICATE_OUTPOINT,
                   "one outpoint twice, a slot apart");
    Extend(*adapter, 132);
    Finalize(*adapter, 132);
    BOOST_CHECK(adapter->Exposed() == (Anchor{130, BlockHash(130)}));
    FactCursor after{adapter->Start()};
    std::vector<Fact> seen;
    for (int round{0}; round < 3 && after < adapter->End(); ++round) {
        const chain::FactBatch batch{RequireBatch(PollOk(*adapter, after), after)};
        BOOST_CHECK_LE(batch.facts.size(), MAX_SLOT_FACTS);
        seen.insert(seen.end(), batch.facts.begin(), batch.facts.end());
        after = batch.to;
    }
    BOOST_CHECK(after == adapter->End());
    BOOST_CHECK(seen == many);
}

BOOST_AUTO_TEST_CASE(anchor_verification_stops_at_the_usable_anchor)
{
    const SeatSet seats{TestSeats({"s0"})};
    const auto adapter{NewAdapter(seats)};
    Extend(*adapter, 136);
    Finalize(*adapter, 134);
    BOOST_CHECK(adapter->Exposed() == (Anchor{134, BlockHash(134)}));

    CheckOk(adapter->VerifyAnchor(Anchor{134, BlockHash(134)}, BASE), "usable anchor over the base");
    CheckOk(adapter->VerifyAnchor(BASE, BASE), "the base over itself");
    CheckOk(adapter->VerifyAnchor(Anchor{133, BlockHash(133)}, Anchor{133, BlockHash(133)}), "an anchor over itself");
    // On the chain, but not finalized and deep: a seat must not sign over it yet.
    RequireRefusal(adapter->VerifyAnchor(Anchor{135, BlockHash(135)}, BASE), ErrorCode::NEED_DATA, chain::ANCHOR_UNKNOWN, "135");
    RequireRefusal(adapter->VerifyAnchor(Anchor{136, BlockHash(136)}, BASE), ErrorCode::NEED_DATA, chain::ANCHOR_UNKNOWN, "the tip");
    RequireRefusal(adapter->VerifyAnchor(Anchor{140, BlockHash(140)}, BASE), ErrorCode::NEED_DATA, chain::ANCHOR_UNKNOWN, "beyond the tip");
    RequireRefusal(adapter->VerifyAnchor(Anchor{132, BlockHash(132)}, Anchor{133, BlockHash(133)}), ErrorCode::INVALID_INPUT,
                   chain::ANCHOR_BELOW_CERTIFIED, "below the certified anchor");
    RequireRefusal(adapter->VerifyAnchor(Anchor{130, BlockHash(130, "b")}, BASE), ErrorCode::VERIFY_FAILED, chain::ANCHOR_NOT_ON_CHAIN,
                   "another fork's 130");
    RequireRefusal(adapter->VerifyAnchor(Anchor{128, BlockHash(128)}, Anchor{128, BlockHash(128)}), ErrorCode::VERIFY_FAILED,
                   chain::ANCHOR_NOT_ON_CHAIN, "below the base");
    RequireRefusal(adapter->VerifyAnchor(Anchor{134, BlockHash(134)}, Anchor{131, BlockHash(131, "b")}), ErrorCode::VERIFY_FAILED,
                   chain::ANCHOR_NOT_DESCENDANT, "certified anchor of another fork");
    // The usable anchor moves with finality and depth.
    Finalize(*adapter, 136);
    CheckOk(adapter->VerifyAnchor(Anchor{134, BlockHash(134)}, BASE), "134 still");
    RequireRefusal(adapter->VerifyAnchor(Anchor{135, BlockHash(135)}, BASE), ErrorCode::NEED_DATA, chain::ANCHOR_UNKNOWN, "135 is 1 deep");
    Extend(*adapter, 137);
    CheckOk(adapter->VerifyAnchor(Anchor{135, BlockHash(135)}, BASE), "135 is 2 deep and final");
}

BOOST_AUTO_TEST_CASE(anchor_proofs_cover_the_header_path_to_the_finality_pin)
{
    const SeatSet seats{TestSeats({"s0"})};
    const auto adapter{NewAdapter(seats)};
    Extend(*adapter, 136);
    Finalize(*adapter, 135);
    BOOST_CHECK(adapter->Exposed() == (Anchor{134, BlockHash(134)}));

    Result<AnchorProof> proved{adapter->ProveAnchor(Anchor{134, BlockHash(134)}, BASE)};
    RequireOk(proved, "ProveAnchor(134, base)");
    const AnchorProof& proof{proved.value()};
    BOOST_CHECK(proof.from == BASE);
    BOOST_CHECK(proof.anchor == (Anchor{134, BlockHash(134)}));
    BOOST_CHECK(proof.finalized == (Anchor{135, BlockHash(135)}));
    BOOST_REQUIRE_EQUAL(proof.path.size(), 6U); // 130..134 to the anchor, then 135, the pin
    for (size_t i{0}; i < proof.path.size(); ++i) {
        const uint32_t height{130 + static_cast<uint32_t>(i)};
        BOOST_CHECK_EQUAL(proof.path[i].height, height);
        BOOST_CHECK(proof.path[i].hash == BlockHash(height));
        BOOST_CHECK(proof.path[i].parent == BlockHash(height - 1));
    }
    CheckOk(CheckAnchorProof(proof), "the adapter's proof checks");

    // A proof of an anchor over itself still carries the path up to the pin.
    Result<AnchorProof> same{adapter->ProveAnchor(Anchor{132, BlockHash(132)}, Anchor{132, BlockHash(132)})};
    RequireOk(same, "ProveAnchor(132, 132)");
    BOOST_CHECK_EQUAL(same.value().path.size(), 3U); // 133..135
    CheckOk(CheckAnchorProof(same.value()), "the anchor is from, the path runs to the pin");

    // The answers of VerifyAnchor come first.
    RequireRefusal(adapter->ProveAnchor(Anchor{135, BlockHash(135)}, BASE), ErrorCode::NEED_DATA, chain::ANCHOR_UNKNOWN, "135 not usable");
    RequireRefusal(adapter->ProveAnchor(Anchor{134, BlockHash(134, "b")}, BASE), ErrorCode::VERIFY_FAILED, chain::ANCHOR_NOT_ON_CHAIN, "forked 134");
    RequireRefusal(adapter->ProveAnchor(Anchor{131, BlockHash(131)}, Anchor{132, BlockHash(132)}), ErrorCode::INVALID_INPUT,
                   chain::ANCHOR_BELOW_CERTIFIED, "131 under 132");

    // Forged proofs fail the structural check.
    const auto forged = [&](auto mutate, std::string_view what) {
        AnchorProof copy{proof};
        mutate(copy);
        RequireRefusal(CheckAnchorProof(copy), ErrorCode::VERIFY_FAILED, ANCHOR_PROOF_PATH, what);
    };
    forged([](AnchorProof& p) { p.path[2].parent = BlockHash(131, "b"); }, "a broken parent link");
    forged([](AnchorProof& p) { p.path[0].parent = BlockHash(128); }, "the first link does not hang off from");
    forged([](AnchorProof& p) { p.path[4].hash = BlockHash(134, "b"); p.path[5].parent = BlockHash(134, "b"); }, "the anchor's link is another block");
    forged([](AnchorProof& p) { p.path.pop_back(); }, "a short path");
    forged([](AnchorProof& p) { p.path.push_back(HeaderLink{136, BlockHash(136), BlockHash(135)}); }, "a long path");
    forged([](AnchorProof& p) { p.path[1].height = 140; }, "a height out of sequence");
    forged([](AnchorProof& p) { p.path[3].hash = uint256{}; p.path[4].parent = uint256{}; }, "a zero hash");
    forged([](AnchorProof& p) { p.from = Anchor{135, BlockHash(135)}; }, "from above the anchor");
    forged([](AnchorProof& p) { p.path.clear(); p.anchor = Anchor{129, BlockHash(129, "b")}; }, "empty path to another block");
    {
        AnchorProof copy{proof};
        copy.finalized = Anchor{133, BlockHash(133)};
        RequireRefusal(CheckAnchorProof(copy), ErrorCode::VERIFY_FAILED, ANCHOR_PROOF_FINALITY, "finality pin below the anchor");
    }
}

BOOST_AUTO_TEST_CASE(anchor_proofs_tie_the_anchor_to_the_finality_pin)
{
    // Review of 2026-11-18 on f21f8d29: the path stopped at the anchor and only the pin's height was checked, so a proof
    // under a pin on another fork, or under another block at the anchor's own height, passed. The path now runs from
    // `from` through the anchor to the pin.
    const SeatSet seats{TestSeats({"s0"})};
    const auto adapter{NewAdapter(seats)};
    Extend(*adapter, 136);
    Finalize(*adapter, 135);
    const Anchor anchor{134, BlockHash(134)};
    const Anchor pin{135, BlockHash(135)};
    Result<AnchorProof> proved{adapter->ProveAnchor(anchor, BASE)};
    RequireOk(proved, "ProveAnchor(134, base)");
    const AnchorProof expected{BASE, anchor, PathOf(130, 135, BASE.hash), pin};
    BOOST_CHECK(proved.value() == expected); // through the anchor to the pin
    CheckOk(CheckAnchorProof(proved.value()), "the adapter's proof");

    const auto refused = [](const AnchorProof& proof, std::string_view detail, std::string_view what) {
        RequireRefusal(CheckAnchorProof(proof), ErrorCode::VERIFY_FAILED, detail, what);
    };
    // The review's forgeries (a path that stops at the anchor) ...
    refused(AnchorProof{BASE, anchor, PathOf(130, 134, BASE.hash), Anchor{135, BlockHash(135, "b")}}, ANCHOR_PROOF_PATH, "a pin on another fork");
    refused(AnchorProof{BASE, anchor, PathOf(130, 134, BASE.hash), Anchor{134, BlockHash(134, "b")}}, ANCHOR_PROOF_FINALITY,
            "another block at the anchor's height");
    refused(AnchorProof{BASE, Anchor{134, BlockHash(134, "b")}, PathOf(130, 134, BASE.hash, "b"), pin}, ANCHOR_PROOF_PATH, "a fork's anchor under the true pin");
    // ... and of the new shape.
    refused(AnchorProof{BASE, anchor, PathOf(130, 135, BASE.hash), Anchor{135, BlockHash(135, "b")}}, ANCHOR_PROOF_FINALITY,
            "the path ends at another block than the pin");
    refused(AnchorProof{BASE, anchor, PathOf(130, 135, BASE.hash, "b"), Anchor{135, BlockHash(135, "b")}}, ANCHOR_PROOF_PATH, "the pin's path misses the anchor");
    CheckOk(CheckAnchorProof(AnchorProof{BASE, BASE, {}, BASE}), "from, anchor and pin one block");
    refused(AnchorProof{BASE, BASE, {}, Anchor{129, BlockHash(129, "b")}}, ANCHOR_PROOF_FINALITY, "an empty path under another block");
}

BOOST_AUTO_TEST_CASE(link_refusals_change_nothing)
{
    const SeatSet seats{TestSeats({"s0", "s1", "s2", "s3"})};
    const auto adapter{NewAdapter(seats)};
    // Before any block: the base cannot leave, and finality cannot name an unknown block.
    RequireRefusal(adapter->OnDisconnected(BASE), ErrorCode::INVALID_INPUT, chain::B3D_BELOW_BASE, "disconnect the base");
    RequireRefusal(adapter->OnFinalized(Anchor{130, BlockHash(130)}), ErrorCode::INVALID_INPUT, chain::B3D_FINALITY_OFF_CHAIN,
                   "finalize an unconnected block");
    CheckOk(adapter->OnFinalized(BASE), "finalize the base again");

    ScriptThreeBlocks(*adapter);
    Extend(*adapter, 134);
    Finalize(*adapter, 133);
    const Snapshot before{Snap(*adapter)};
    BOOST_CHECK(before.exposed == (Anchor{132, BlockHash(132)}));
    const uint256 tip_hash{BlockHash(134)};

    // Connect.
    RequireRefusal(adapter->OnConnected(BlockOf(136, tip_hash)), ErrorCode::INVALID_INPUT, chain::B3D_HEIGHT, "a gap in height");
    RequireRefusal(adapter->OnConnected(BlockOf(134, BlockHash(133))), ErrorCode::INVALID_INPUT, chain::B3D_HEIGHT, "the tip again");
    RequireRefusal(adapter->OnConnected(BlockOf(135, BlockHash(133))), ErrorCode::INVALID_INPUT, chain::B3D_PARENT, "the wrong parent");
    RequireRefusal(adapter->OnConnected(ChainBlock{Anchor{135, uint256{}}, tip_hash, {}, std::nullopt}), ErrorCode::INVALID_INPUT,
                   chain::B3D_BLOCK_HASH, "a zero hash");
    RequireRefusal(adapter->OnConnected(BlockOf(135, tip_hash, {DepositAtCursor({135, 1}, "z")})), ErrorCode::INVALID_INPUT,
                   chain::B3D_FACT_CURSOR, "a fact at index 1 first");
    RequireRefusal(adapter->OnConnected(BlockOf(135, tip_hash, {DepositAtCursor({134, 0}, "z")})), ErrorCode::INVALID_INPUT,
                   chain::B3D_FACT_CURSOR, "a fact of another height");
    RequireRefusal(adapter->OnConnected(BlockOf(135, tip_hash, {DepositAtCursor({135, 0}, "z", 0)})), ErrorCode::INVALID_INPUT,
                   chain::BATCH_DEPOSIT_ZERO, "a zero deposit");
    RequireRefusal(adapter->OnConnected(BlockOf(135, tip_hash, {DepositAtCursor({135, 0}, "z"), DepositAtCursor({135, 1}, "z")})),
                   ErrorCode::INVALID_INPUT, chain::BATCH_DUPLICATE_OUTPOINT, "one outpoint twice in a block");
    RequireRefusal(adapter->OnConnected(BlockOf(135, tip_hash, {DepositAtCursor({135, 0}, "main/a")})), ErrorCode::INVALID_INPUT,
                   chain::B3D_OUTPOINT_REPEATED, "an outpoint deposited in block 130");
    RequireStub(adapter->OnConnected(BlockOf(135, tip_hash, {Fact{SeatChangeFact{FactCursor{135, 0}}}})), "M7-SEAT-01");
    // Disconnect.
    RequireRefusal(adapter->OnDisconnected(Anchor{133, BlockHash(133)}), ErrorCode::INVALID_INPUT, chain::B3D_NOT_TIP, "not the tip");
    RequireRefusal(adapter->OnDisconnected(Anchor{134, BlockHash(134, "b")}), ErrorCode::INVALID_INPUT, chain::B3D_NOT_TIP, "the tip's height, another hash");
    // Finality.
    RequireRefusal(adapter->OnFinalized(Anchor{135, BlockHash(135)}), ErrorCode::INVALID_INPUT, chain::B3D_FINALITY_OFF_CHAIN, "above the tip");
    RequireRefusal(adapter->OnFinalized(Anchor{134, BlockHash(134, "b")}), ErrorCode::INVALID_INPUT, chain::B3D_FINALITY_OFF_CHAIN, "another fork");
    RequireRefusal(adapter->OnFinalized(Anchor{132, BlockHash(132)}), ErrorCode::INVALID_INPUT, chain::B3D_FINALITY_BACKWARDS, "below the pin");
    CheckOk(adapter->OnFinalized(Anchor{133, BlockHash(133)}), "the pin again");

    BOOST_CHECK(Snap(*adapter) == before);
    const chain::FactBatch batch{RequireBatch(PollOk(*adapter, adapter->Start()), adapter->Start())};
    BOOST_CHECK_EQUAL(batch.facts.size(), 5U);
    BOOST_CHECK(batch.anchor == before.exposed);
}

BOOST_AUTO_TEST_SUITE_END()
