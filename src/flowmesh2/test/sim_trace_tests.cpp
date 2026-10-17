// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <crypto/sha256.h>
#include <flowmesh2/chain/chain_adapter.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/dev/fake_chain.h>
#include <flowmesh2/sim/trace.h>
#include <flowmesh2/storage/memory_journal.h>
#include <flowmesh2/storage/record.h>
#include <flowmesh2/storage/seat_signer.h>
#include <flowmesh2/storage/signer_log.h>
#include <flowmesh2/test/util/agreement_fixtures.h>
#include <flowmesh2/test/util/storage_fixtures.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::sim;
using namespace flowmesh2::test;

namespace {

std::vector<std::string> Steps(const TraceRecorder& trace)
{
    std::vector<std::string> steps;
    for (const TraceEvent& event : trace.Events()) steps.push_back(event.Step());
    return steps;
}

} // namespace

BOOST_AUTO_TEST_SUITE(sim_trace_tests)

BOOST_AUTO_TEST_CASE(journal_events_are_stamped_with_the_slot)
{
    TraceRecorder trace;
    auto journal{NewMemoryJournal(storage::JournalRole::COMMIT, 11, {}, &trace)};
    const std::vector<unsigned char> payload{1, 2, 3};
    RequireStorageOk(journal->Append(storage::RecordType::APPLIED, payload), "append before the slot");
    trace.BeginSlot(4);
    RequireStorageOk(journal->Append(storage::RecordType::DECISION, payload), "append in the slot");
    RequireStorageOk(journal->Barrier(), "barrier");
    trace.EndSlot(4, "OK");
    trace.Record("client.submit", "alice,c");

    const std::vector<TraceEvent> events{trace.Events()};
    BOOST_REQUIRE_EQUAL(events.size(), 7U);
    BOOST_CHECK(Steps(trace) == (std::vector<std::string>{"commit.append:APPLIED", "slot.begin", "commit.append:DECISION",
                                                           "commit.barrier.start", "commit.barrier.end:ok", "slot.end",
                                                           "client.submit"}));
    const std::vector<uint64_t> slots{events[0].slot, events[1].slot, events[2].slot, events[3].slot,
                                      events[4].slot, events[5].slot, events[6].slot};
    BOOST_CHECK(slots == (std::vector<uint64_t>{0, 4, 4, 4, 4, 4, 0}));
    BOOST_CHECK_EQUAL(events[2].seq, 2U);
    BOOST_CHECK(events[2].record == storage::RecordType::DECISION);
    BOOST_CHECK_EQUAL(events[3].seq, 2U);
    BOOST_CHECK_EQUAL(trace.BarrierCount(storage::JournalRole::COMMIT), 1U);
    BOOST_CHECK_EQUAL(trace.BarrierCount(storage::JournalRole::SIGNER), 0U);
    BOOST_CHECK_EQUAL(events[2].Line(), "4|commit.append|DECISION,instance=11,seq=2");
    BOOST_CHECK_EQUAL(events[4].Line(), "4|commit.barrier.end|instance=11,target=2,ok");
    BOOST_CHECK_EQUAL(events[5].Line(), "4|slot.end|slot=4,OK");
    BOOST_CHECK_EQUAL(events[6].Line(), "0|client.submit|alice,c");

    // A failed barrier is recorded as such.
    TraceRecorder failing;
    auto bad{NewMemoryJournal(storage::JournalRole::SIGNER, 12, storage::MemoryJournalFaults{.fail_barrier = 1}, &failing)};
    RequireStorageOk(bad->Append(storage::RecordType::INTENT, payload), "append");
    RequireCode(bad->Barrier(), ErrorCode::STORAGE_FATAL, "failed barrier");
    BOOST_CHECK(Steps(failing) == (std::vector<std::string>{"signer.append:INTENT", "signer.barrier.start", "signer.barrier.end:fail"}));
}

BOOST_AUTO_TEST_CASE(canonical_trace_and_digest)
{
    TraceRecorder a;
    TraceRecorder b;
    for (TraceRecorder* t : {&a, &b}) {
        t->BeginSlot(1);
        t->Record("chain.poll", "from=130:0");
        t->EndSlot(1, "OK");
    }
    const std::string canonical{"1|slot.begin|slot=1\n1|chain.poll|from=130:0\n1|slot.end|slot=1,OK\n"};
    BOOST_CHECK_EQUAL(a.Canonical(), canonical);
    // Digest = SHA256("fm2/sim/trace/1\n" || canonical), restated here.
    const std::string preimage{"fm2/sim/trace/1\n" + canonical};
    uint256 expected;
    CSHA256{}.Write(reinterpret_cast<const unsigned char*>(preimage.data()), preimage.size()).Finalize(expected.data());
    BOOST_CHECK(a.Digest() == expected);
    BOOST_CHECK(a.Digest() == b.Digest());
    // Any change to the event sequence changes the digest.
    b.Record("client.submit", "bob,a");
    BOOST_CHECK(a.Digest() != b.Digest());
    TraceRecorder c;
    c.BeginSlot(2);
    c.Record("chain.poll", "from=130:0");
    c.EndSlot(2, "OK");
    BOOST_CHECK(a.Digest() != c.Digest());
    BOOST_CHECK_EQUAL(a.Size(), 3U);
}

BOOST_AUTO_TEST_CASE(tracing_adapter_forwards_and_reports_authority_reads)
{
    const SeatSet seats{TestSeats({"s0"})};
    const AuthorityVersion authority{SeatAuthority(seats)};
    auto fake{dev::FakeChainAdapter::Create(TestNetwork(), authority, seats, TestAnchor(129)).value()};
    TraceRecorder trace;
    TracingChainAdapter tracing{*fake, trace};

    BOOST_CHECK(tracing.Network().magic == TestNetwork().magic);
    BOOST_CHECK(tracing.CurrentAuthority() == authority);
    RequireOk(tracing.CheckAuthority(authority), "CheckAuthority");
    fake->BumpAuthority();
    RequireError(tracing.CheckAuthority(authority), ErrorCode::AUTHORITY_CHANGED, "CheckAuthority after a bump");
    const std::vector<TraceEvent> events{trace.Events()};
    BOOST_REQUIRE_EQUAL(events.size(), 3U);
    for (const TraceEvent& event : events) BOOST_CHECK_EQUAL(event.kind, "authority.check");
    BOOST_CHECK(events[0].detail.starts_with("epoch=0,"));
    BOOST_CHECK(events[2].detail.starts_with("epoch=1,"));

    // Poll, VerifyAnchor and SeatSetAt are forwarded unchanged and do not read the authority.
    RequireOk(fake->AppendBlock(BlockHash(130), {}), "AppendBlock");
    const Result<chain::PollReply> direct{fake->Poll(fake->Start(), 4)};
    const Result<chain::PollReply> through{tracing.Poll(fake->Start(), 4)};
    RequireOk(through, "Poll");
    BOOST_CHECK(*through == *direct);
    RequireOk(tracing.VerifyAnchor(Anchor{130, BlockHash(130)}, TestAnchor(129)), "VerifyAnchor");
    RequireOk(tracing.SeatSetAt(0), "SeatSetAt");
    RequireError(tracing.SeatSetAt(7), ErrorCode::NEED_DATA, "SeatSetAt unknown epoch");
    BOOST_CHECK_EQUAL(trace.Size(), 3U);
}

BOOST_AUTO_TEST_CASE(sign_events_come_from_the_seat_signer)
{
    TraceRecorder trace;
    auto journal{NewMemoryJournal(storage::JournalRole::SIGNER, 13, {}, &trace)};
    storage::SignerLog log{storage::SignerLog::Create(*journal).value()};
    const uint256 ikm{LabelHash("fm2/test/sim/trace/seat")};
    storage::SeatSigner signer{storage::SeatSigner::FromIKM(std::span<const unsigned char>{ikm.data(), ikm.size()}, log).value()};
    signer.SetSignObserver(&trace);

    const StorageSlot slot{MakeStorageSlot(6, 0, "trace")};
    trace.BeginSlot(6);
    const storage::PendingProposal proposal{
        log.AppendAcceptedProposal(storage::ProposalKey{0, 6, 0}, slot.header, slot.body).value()};
    const storage::PendingIntent pending{log.AppendIntent(CommitVoteKey(slot.header, 0), slot.header.CommitDigest(), proposal).value()};
    RequireStorageOk(log.Barrier(), "B-INTENT");
    const DurableIntent intent{log.Confirm(pending).value()};
    RequireStorageOk(signer.Sign(intent), "Sign");
    trace.EndSlot(6, "OK");

    BOOST_CHECK(Steps(trace) == (std::vector<std::string>{"slot.begin", "signer.append:ACCEPTED_PROPOSAL", "signer.append:INTENT",
                                                           "signer.barrier.start", "signer.barrier.end:ok", "bls.sign",
                                                           "slot.end"}));
    const std::vector<TraceEvent> events{trace.Events()};
    BOOST_CHECK_EQUAL(events[5].Line(), "6|bls.sign|slot=6,view=0,digest=" + slot.header.CommitDigest().GetHex());
    BOOST_CHECK_EQUAL(trace.BarrierCount(storage::JournalRole::SIGNER), 1U);
}

BOOST_AUTO_TEST_SUITE_END()
