// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** The REGTEST fake chain adapter (slice R10, fm2_dev): network refusal,
 * the monotone fact cursor, BUSY, NEED_DATA, scripted rewinds that answer
 * AUTHORITY_CHANGED with the rewind cursor, anchor ancestry, seat sets per
 * epoch and the authority hook at named points.
 */

#include <boost/test/unit_test.hpp>

#include <flowmesh2/chain/chain_adapter.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/dev/fake_chain.h>
#include <flowmesh2/storage/memory_journal.h>
#include <flowmesh2/test/util/agreement_fixtures.h>
#include <flowmesh2/test/util/storage_fixtures.h>

#include <memory>
#include <string>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;
using dev::FakeChainAdapter;
using dev::HookPoint;

namespace {

const Anchor BASE{129, BlockHash(129)};

std::unique_ptr<FakeChainAdapter> NewFake(const SeatSet& seats)
{
    Result<std::unique_ptr<FakeChainAdapter>> fake{FakeChainAdapter::Create(TestNetwork(), SeatAuthority(seats), seats, BASE)};
    RequireOk(fake, "FakeChainAdapter::Create");
    return std::move(fake).value();
}

//! Blocks 130 (three deposits), 131 (none) and 132 (two deposits) on fork `fork`.
std::vector<Fact> ScriptThreeBlocks(FakeChainAdapter& fake, std::string_view fork = "main")
{
    const std::string f{fork};
    std::vector<Fact> all;
    std::vector<Fact> b130{DepositAtCursor({130, 0}, f + "/a"), DepositAtCursor({130, 1}, f + "/b"), DepositAtCursor({130, 2}, f + "/c")};
    std::vector<Fact> b132{DepositAtCursor({132, 0}, f + "/d"), DepositAtCursor({132, 1}, f + "/e")};
    all.insert(all.end(), b130.begin(), b130.end());
    all.insert(all.end(), b132.begin(), b132.end());
    RequireOk(fake.AppendBlock(BlockHash(130, fork), b130), "block 130");
    RequireOk(fake.AppendBlock(BlockHash(131, fork), {}), "block 131");
    RequireOk(fake.AppendBlock(BlockHash(132, fork), b132), "block 132");
    return all;
}

chain::PollReply PollOk(FakeChainAdapter& fake, const FactCursor& after, size_t max)
{
    Result<chain::PollReply> reply{fake.Poll(after, max)};
    RequireOk(reply, "Poll");
    return std::move(reply).value();
}

//! Requires a FACTS reply that passes CheckBatch and returns its batch.
chain::FactBatch RequireBatch(const chain::PollReply& reply, const FactCursor& after, size_t max)
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

} // namespace

BOOST_AUTO_TEST_SUITE(fake_chain_tests)

BOOST_AUTO_TEST_CASE(create_refuses_real_networks_and_bad_inputs)
{
    const SeatSet seats{TestSeats({"s0"})};
    for (const ChainType chain_type : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4, ChainType::SIGNET}) {
        NetworkId network{TestNetwork()};
        network.chain = chain_type;
        RequireRefusal(FakeChainAdapter::Create(network, SeatAuthority(seats), seats, BASE), ErrorCode::UNSUPPORTED_NETWORK,
                       dev::FAKE_NETWORK, "real network");
    }
    RequireRefusal(FakeChainAdapter::Create(TestNetwork(), SeatAuthority(TestSeats({"s1"})), seats, BASE), ErrorCode::INVALID_INPUT,
                   dev::FAKE_SEAT_SET, "seat set of another authority");
    RequireRefusal(FakeChainAdapter::Create(TestNetwork(), SeatAuthority(seats), seats, Anchor{129, uint256{}}),
                   ErrorCode::INVALID_INPUT, dev::FAKE_BLOCK_HASH, "zero base hash");
    RequireRefusal(FakeChainAdapter::Create(TestNetwork(), SeatAuthority(seats), seats, Anchor{0xffff'fffe, BlockHash(1)}),
                   ErrorCode::INVALID_INPUT, dev::FAKE_BASE_HEIGHT, "base at 2^32 - 2");
    const auto fake{NewFake(seats)};
    BOOST_CHECK(fake->Network().chain == ChainType::REGTEST);
    BOOST_CHECK(fake->CurrentAuthority() == SeatAuthority(seats));
    BOOST_CHECK(fake->Base() == BASE);
    BOOST_CHECK(fake->Tip() == BASE);
    BOOST_CHECK(fake->Start() == (FactCursor{130, 0}));
    BOOST_CHECK(fake->End() == (FactCursor{130, 0}));
    BOOST_CHECK_EQUAL(dev::ToString(HookPoint::POLL), "POLL");
    BOOST_CHECK_EQUAL(dev::ToString(HookPoint::SIGNER_BARRIER_START), "SIGNER_BARRIER_START");
    BOOST_CHECK_EQUAL(dev::ToString(HookPoint::SIGNER_BARRIER_END), "SIGNER_BARRIER_END");
    BOOST_CHECK_EQUAL(dev::ToString(HookPoint::COMMIT_BARRIER_START), "COMMIT_BARRIER_START");
    BOOST_CHECK_EQUAL(dev::ToString(HookPoint::COMMIT_BARRIER_END), "COMMIT_BARRIER_END");
    BOOST_CHECK_EQUAL(dev::ToString(static_cast<HookPoint>(0)), "UNKNOWN");
}

BOOST_AUTO_TEST_CASE(poll_cursor_is_monotone)
{
    const SeatSet seats{TestSeats({"s0"})};
    for (const size_t max : {size_t{1}, size_t{2}, size_t{3}, size_t{4}, MAX_SLOT_FACTS}) {
        const auto fake{NewFake(seats)};
        const std::vector<Fact> all{ScriptThreeBlocks(*fake)};
        BOOST_CHECK(fake->End() == (FactCursor{133, 0}));
        BOOST_CHECK(fake->Tip() == (Anchor{132, BlockHash(132)}));

        FactCursor after{fake->Start()};
        std::vector<Fact> seen;
        for (int round{0}; round < 10 && after < fake->End(); ++round) {
            const chain::FactBatch batch{RequireBatch(PollOk(*fake, after, max), after, max)};
            BOOST_CHECK(batch.anchor == fake->Tip());
            BOOST_CHECK(after < batch.to); // strictly advances while facts remain
            BOOST_CHECK_LE(batch.facts.size(), max);
            seen.insert(seen.end(), batch.facts.begin(), batch.facts.end());
            after = batch.to;
        }
        BOOST_CHECK(after == fake->End());
        BOOST_CHECK(seen == all);
        // At the end: an empty batch anchored at the tip (a heartbeat).
        const chain::FactBatch heartbeat{RequireBatch(PollOk(*fake, after, max), after, max)};
        BOOST_CHECK(heartbeat.facts.empty());
        BOOST_CHECK(heartbeat.to == heartbeat.from);
        BOOST_CHECK(heartbeat.anchor == fake->Tip());
    }
    // A cut batch ends right after its last fact; a poll from inside block 130 resumes there.
    const auto fake{NewFake(seats)};
    ScriptThreeBlocks(*fake);
    const chain::FactBatch first{RequireBatch(PollOk(*fake, fake->Start(), 2), fake->Start(), 2)};
    BOOST_CHECK(first.to == (FactCursor{130, 2}));
    const chain::FactBatch second{RequireBatch(PollOk(*fake, first.to, 2), first.to, 2)};
    BOOST_REQUIRE_EQUAL(second.facts.size(), 2U);
    BOOST_CHECK(CursorOf(second.facts[0]) == (FactCursor{130, 2}));
    BOOST_CHECK(CursorOf(second.facts[1]) == (FactCursor{132, 0}));
    BOOST_CHECK(second.to == (FactCursor{132, 1}));
    // Polling never consumes: the same cursor gives the same batch again.
    BOOST_CHECK(RequireBatch(PollOk(*fake, first.to, 2), first.to, 2) == second);
}

BOOST_AUTO_TEST_CASE(busy_and_need_data)
{
    const SeatSet seats{TestSeats({"s0"})};
    const auto fake{NewFake(seats)};
    ScriptThreeBlocks(*fake);
    fake->QueueBusy(2);
    for (int i{0}; i < 2; ++i) {
        const chain::PollReply busy{PollOk(*fake, fake->Start(), 4)};
        BOOST_CHECK(busy.Status() == chain::PollStatus::BUSY);
        BOOST_CHECK(busy.Batch() == nullptr);
        BOOST_CHECK(busy.Authority() == fake->CurrentAuthority());
    }
    BOOST_CHECK_EQUAL(RequireBatch(PollOk(*fake, fake->Start(), 4), fake->Start(), 4).facts.size(), 4U);

    // Beyond the end the adapter has no data yet; after the next block it has.
    const FactCursor ahead{134, 0};
    const chain::PollReply need{PollOk(*fake, ahead, 4)};
    BOOST_CHECK(need.Status() == chain::PollStatus::NEED_DATA);
    BOOST_CHECK(need.Batch() == nullptr);
    BOOST_CHECK(need.Authority() == fake->CurrentAuthority());
    BOOST_CHECK(PollOk(*fake, FactCursor{133, 5}, 4).Status() == chain::PollStatus::NEED_DATA);
    RequireOk(fake->AppendBlock(BlockHash(133), {DepositAtCursor({133, 0}, "late")}), "block 133");
    BOOST_CHECK(PollOk(*fake, ahead, 4).Status() == chain::PollStatus::FACTS);

    // Misuse is an error, not an answer.
    RequireRefusal(fake->Poll(fake->Start(), 0), ErrorCode::INVALID_INPUT, chain::POLL_MAX_FACTS, "max 0");
    RequireRefusal(fake->Poll(fake->Start(), MAX_SLOT_FACTS + 1), ErrorCode::INVALID_INPUT, chain::POLL_MAX_FACTS, "max 1,025");
    RequireRefusal(fake->Poll(FactCursor{129, 9}, 4), ErrorCode::INVALID_INPUT, dev::FAKE_CURSOR_BEFORE_BASE, "before the base");
}

BOOST_AUTO_TEST_CASE(rewind_answers_authority_changed_with_the_rewind_cursor)
{
    const SeatSet seats{TestSeats({"s0"})};
    const auto fake{NewFake(seats)};
    ScriptThreeBlocks(*fake);
    const AuthorityVersion before{fake->CurrentAuthority()};
    // The consumer read everything, then blocks 131 and 132 are disconnected.
    Result<AuthorityVersion> after_rewind{fake->Rewind(130)};
    RequireOk(after_rewind, "Rewind(130)");
    BOOST_CHECK_EQUAL(after_rewind->epoch, before.epoch + 1);
    BOOST_CHECK(after_rewind->seat_set_root == before.seat_set_root);
    BOOST_CHECK(fake->CurrentAuthority() == *after_rewind);
    BOOST_CHECK(fake->Tip() == (Anchor{130, BlockHash(130)}));
    BOOST_CHECK(fake->End() == (FactCursor{131, 0}));
    RequireRefusal(fake->CheckAuthority(before), ErrorCode::AUTHORITY_CHANGED, chain::CHAIN_AUTHORITY_CHANGED, "old authority");

    for (const FactCursor& consumed : {FactCursor{133, 0}, FactCursor{132, 1}, FactCursor{131, 1}}) {
        const chain::PollReply changed{PollOk(*fake, consumed, 4)};
        BOOST_CHECK(changed.Status() == chain::PollStatus::AUTHORITY_CHANGED);
        BOOST_CHECK(changed.Batch() == nullptr);
        BOOST_REQUIRE(changed.RewindTo().has_value());
        BOOST_CHECK(*changed.RewindTo() == (FactCursor{131, 0}));
        BOOST_CHECK(changed.Authority() == *after_rewind);
    }
    // The other fork: outpoints of the dropped blocks may be deposited again.
    RequireOk(fake->AppendBlock(BlockHash(131, "fork"), {DepositAtCursor({131, 0}, "main/d")}), "block 131 of the fork");
    // Resuming at the rewind cursor clears the rewind and serves the new fork.
    const chain::FactBatch resumed{RequireBatch(PollOk(*fake, FactCursor{131, 0}, 4), FactCursor{131, 0}, 4)};
    BOOST_REQUIRE_EQUAL(resumed.facts.size(), 1U);
    BOOST_CHECK(resumed.anchor == (Anchor{131, BlockHash(131, "fork")}));
    BOOST_CHECK(resumed.authority == *after_rewind);
    BOOST_CHECK(PollOk(*fake, FactCursor{132, 0}, 4).Status() == chain::PollStatus::FACTS);

    // A rewind needs a block to drop and cannot go below the base.
    RequireRefusal(fake->Rewind(131), ErrorCode::INVALID_INPUT, dev::FAKE_REWIND, "rewind to the tip");
    RequireRefusal(fake->Rewind(128), ErrorCode::INVALID_INPUT, dev::FAKE_REWIND, "rewind below the base");
    BOOST_CHECK(fake->CurrentAuthority() == *after_rewind);
    RequireOk(fake->Rewind(129), "rewind to the base");
    BOOST_CHECK(fake->End() == fake->Start());
}

BOOST_AUTO_TEST_CASE(anchor_ancestry)
{
    const SeatSet seats{TestSeats({"s0"})};
    const auto fake{NewFake(seats)};
    ScriptThreeBlocks(*fake);
    const Anchor a130{130, BlockHash(130)};
    const Anchor a131{131, BlockHash(131)};
    const Anchor a132{132, BlockHash(132)};
    CheckOk(fake->VerifyAnchor(a132, BASE), "tip descends from the base");
    CheckOk(fake->VerifyAnchor(a131, a130), "131 descends from 130");
    CheckOk(fake->VerifyAnchor(a131, a131), "an anchor equals itself");
    RequireRefusal(fake->VerifyAnchor(a130, a131), ErrorCode::INVALID_INPUT, chain::ANCHOR_BELOW_CERTIFIED, "below the certified anchor");
    RequireRefusal(fake->VerifyAnchor(Anchor{133, BlockHash(133)}, a130), ErrorCode::NEED_DATA, chain::ANCHOR_UNKNOWN, "above the tip");
    RequireRefusal(fake->VerifyAnchor(Anchor{131, BlockHash(131, "fork")}, a130), ErrorCode::VERIFY_FAILED, chain::ANCHOR_NOT_ON_CHAIN,
                   "block of another fork");
    RequireRefusal(fake->VerifyAnchor(a131, Anchor{128, BlockHash(128)}), ErrorCode::VERIFY_FAILED, chain::ANCHOR_NOT_DESCENDANT,
                   "certified anchor below the fake's history");

    // After a reorg the old fork's blocks are not ancestors of the new tip.
    RequireOk(fake->Rewind(130), "Rewind(130)");
    RequireOk(fake->AppendBlock(BlockHash(131, "fork"), {}), "fork 131");
    RequireOk(fake->AppendBlock(BlockHash(132, "fork"), {}), "fork 132");
    const Anchor f132{132, BlockHash(132, "fork")};
    CheckOk(fake->VerifyAnchor(f132, a130), "new tip descends from the common block");
    RequireRefusal(fake->VerifyAnchor(f132, a131), ErrorCode::VERIFY_FAILED, chain::ANCHOR_NOT_DESCENDANT, "non-ancestor anchor");
    RequireRefusal(fake->VerifyAnchor(a132, a130), ErrorCode::VERIFY_FAILED, chain::ANCHOR_NOT_ON_CHAIN, "old tip");
}

BOOST_AUTO_TEST_CASE(seat_sets_by_epoch)
{
    const SeatSet first{TestSeats({"s0"})};
    const SeatSet second{TestSeats({"s1"})};
    const auto fake{NewFake(first)};
    const AuthorityVersion e0{fake->CurrentAuthority()};
    const AuthorityVersion e1{fake->BumpAuthority()};
    BOOST_CHECK_EQUAL(e1.epoch, e0.epoch + 1);
    BOOST_CHECK(e1.seat_set_root == first.Root());
    const AuthorityVersion e2{fake->BumpAuthority(second)};
    BOOST_CHECK_EQUAL(e2.epoch, e0.epoch + 2);
    BOOST_CHECK(e2.seat_set_root == second.Root());
    BOOST_CHECK(e2.profile_hash == e0.profile_hash);
    for (const auto& [epoch, root] : {std::pair{e0.epoch, first.Root()}, std::pair{e1.epoch, first.Root()}, std::pair{e2.epoch, second.Root()}}) {
        Result<SeatSet> at{fake->SeatSetAt(epoch)};
        RequireOk(at, "SeatSetAt");
        BOOST_CHECK(at->Root() == root);
    }
    RequireRefusal(fake->SeatSetAt(e2.epoch + 1), ErrorCode::NEED_DATA, chain::SEAT_SET_UNKNOWN_EPOCH, "future epoch");
}

BOOST_AUTO_TEST_CASE(append_block_refusals_change_nothing)
{
    const SeatSet seats{TestSeats({"s0"})};
    const auto fake{NewFake(seats)};
    RequireOk(fake->AppendBlock(BlockHash(130), {DepositAtCursor({130, 0}, "x")}), "block 130");
    const auto expect = [&](std::vector<Fact> facts, ErrorCode code, std::string_view detail, std::string_view what) {
        RequireRefusal(fake->AppendBlock(BlockHash(131), std::move(facts)), code, detail, what);
        BOOST_CHECK(fake->End() == (FactCursor{131, 0}));
    };
    RequireRefusal(fake->AppendBlock(uint256{}, {}), ErrorCode::INVALID_INPUT, dev::FAKE_BLOCK_HASH, "zero hash");
    expect({DepositAtCursor({130, 1}, "y")}, ErrorCode::INVALID_INPUT, dev::FAKE_FACT_CURSOR, "cursor of another height");
    expect({DepositAtCursor({131, 1}, "y")}, ErrorCode::INVALID_INPUT, dev::FAKE_FACT_CURSOR, "index gap");
    expect({DepositAtCursor({131, 0}, "y", 0)}, ErrorCode::INVALID_INPUT, chain::BATCH_DEPOSIT_ZERO, "zero deposit");
    {
        DepositFact to_pool{DepositAtCursor({131, 0}, "y")};
        to_pool.account = SeatPoolAccount(RegtestProfile());
        expect({to_pool}, ErrorCode::INVALID_INPUT, chain::BATCH_DEPOSIT_PROTOCOL, "deposit to the seat pool");
    }
    expect({DepositAtCursor({131, 0}, "y"), DepositAtCursor({131, 1}, "y")}, ErrorCode::INVALID_INPUT, chain::BATCH_DUPLICATE_OUTPOINT,
           "one outpoint twice in a block");
    expect({DepositAtCursor({131, 0}, "x")}, ErrorCode::INVALID_INPUT, dev::FAKE_OUTPOINT_REPEATED, "outpoint of block 130");
    {
        RequireStub(fake->AppendBlock(BlockHash(131), {SeatChangeFact{FactCursor{131, 0}}}), "M7-SEAT-01");
        BOOST_CHECK(fake->End() == (FactCursor{131, 0}));
    }
    {
        std::vector<Fact> too_many;
        for (uint32_t i{0}; i <= MAX_SLOT_FACTS; ++i) too_many.push_back(DepositAtCursor({131, i}, "many/" + std::to_string(i)));
        expect(std::move(too_many), ErrorCode::INVALID_INPUT, chain::BATCH_TOO_MANY, "1,025 facts in one block");
    }
    RequireOk(fake->AppendBlock(BlockHash(131), {DepositAtCursor({131, 0}, "y")}), "a valid block 131 after the refusals");
}

BOOST_AUTO_TEST_CASE(bump_hook_at_named_points)
{
    const SeatSet seats{TestSeats({"s0"})};
    const auto fake{NewFake(seats)};
    const uint64_t e0{fake->CurrentAuthority().epoch};
    RequireRefusal(fake->BumpAt(HookPoint::POLL, 0), ErrorCode::INVALID_INPUT, dev::FAKE_HOOK_OCCURRENCE, "occurrence 0");

    // The second signer barrier bumps, between the barrier and the caller's next step.
    auto signer{NewMemoryJournal(storage::JournalRole::SIGNER, 1, {}, &fake->JournalHook())};
    auto commit{NewMemoryJournal(storage::JournalRole::COMMIT, 2, {}, &fake->JournalHook())};
    RequireOk(fake->BumpAt(HookPoint::SIGNER_BARRIER_END, 2), "arm SIGNER_BARRIER_END #2");
    RequireOk(fake->BumpAt(HookPoint::COMMIT_BARRIER_START, 1, TestSeats({"s1"})), "arm COMMIT_BARRIER_START #1");
    RequireStorageOk(signer->Barrier(), "signer barrier 1");
    BOOST_CHECK_EQUAL(fake->CurrentAuthority().epoch, e0);
    RequireStorageOk(signer->Barrier(), "signer barrier 2");
    BOOST_CHECK_EQUAL(fake->CurrentAuthority().epoch, e0 + 1);
    RequireStorageOk(signer->Barrier(), "signer barrier 3 (the hook fired once)");
    BOOST_CHECK_EQUAL(fake->CurrentAuthority().epoch, e0 + 1);
    RequireStorageOk(commit->Barrier(), "commit barrier 1");
    BOOST_CHECK_EQUAL(fake->CurrentAuthority().epoch, e0 + 2);
    BOOST_CHECK(fake->CurrentAuthority().seat_set_root == TestSeats({"s1"}).Root());
    BOOST_CHECK_EQUAL(fake->Reached(HookPoint::SIGNER_BARRIER_START), 3U);
    BOOST_CHECK_EQUAL(fake->Reached(HookPoint::SIGNER_BARRIER_END), 3U);
    BOOST_CHECK_EQUAL(fake->Reached(HookPoint::COMMIT_BARRIER_START), 1U);
    BOOST_CHECK_EQUAL(fake->Reached(HookPoint::COMMIT_BARRIER_END), 1U);

    // A POLL hook fires before the answer, so the batch carries the new authority.
    RequireOk(fake->BumpAt(HookPoint::POLL, 1), "arm POLL #1");
    const chain::PollReply reply{PollOk(*fake, fake->Start(), 4)};
    BOOST_CHECK_EQUAL(reply.Authority().epoch, e0 + 3);
    BOOST_CHECK_EQUAL(RequireBatch(reply, fake->Start(), 4).authority.epoch, e0 + 3);
    BOOST_CHECK_EQUAL(fake->Reached(HookPoint::POLL), 1U);
}

BOOST_AUTO_TEST_SUITE_END()
