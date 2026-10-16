// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh2/read_slot.h>
#include <crypto/common.h>

#include <atomic>
#include <cstddef>
#include <iostream>
#include <latch>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

static_assert(!std::is_copy_constructible_v<flowmesh2::ReadLease>);
static_assert(!std::is_copy_assignable_v<flowmesh2::ReadLease>);

namespace {
using namespace flowmesh2;

void Check(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error{message};
}

uint256 Identity(uint64_t value)
{
    uint256 result;
    WriteLE64(result.begin(), value);
    return result;
}

ReadEnvelope Envelope(uint64_t value, size_t size = 8)
{
    return {{uint256{1}, uint256{2}, uint256{3}, uint256{4}, 5, 6},
            {Identity(value), Identity(value - 1), Identity(value + 1000), value},
            std::vector<unsigned char>(size, static_cast<unsigned char>(value % 251))};
}

bool Coherent(const PublishedEnvelope& view)
{
    const auto& envelope{view.envelope};
    const auto number{envelope.commit.sequence};
    return envelope.scope == Envelope(1).scope && envelope.commit == Envelope(number).commit &&
        envelope.payload == Envelope(number).payload;
}

void InvalidAndEmpty()
{
    for (const auto& limits : {std::pair<size_t, size_t>{0, 8}, {1, 0}}) {
        bool refused{false};
        try { ReadSlot slot{limits.first, limits.second}; } catch (const std::invalid_argument&) { refused = true; }
        Check(refused, "zero read-slot limit accepted");
    }
    ReadSlot slot{1, 8};
    Check(!slot.Acquire() && slot.Readers() == 0, "empty slot leaked reader permit");
}

void ExactPayloadLimit()
{
    ReadSlot slot{2, 8};
    Check(slot.Publish(Envelope(1)) == ReadPublishResult::PUBLISHED, "exact payload bound refused");
    const auto rejected{Envelope(2, 9)};
    Check(slot.Publish(rejected) == ReadPublishResult::OVERSIZE, "oversize accepted");
    auto lease{slot.Acquire()};
    Check(lease && lease->View().publication == 1 && Coherent(lease->View()), "rejection changed current head");
    Check(rejected.payload.size() == 9, "publication modified caller");
}

void DeepOwnershipAndOneHead()
{
    ReadSlot slot{2, 8};
    auto input{Envelope(1)};
    input.payload.reserve(1'000'000);
    Check(slot.Publish(input) == ReadPublishResult::PUBLISHED, "first publication failed");
    auto old{slot.Acquire()};
    input.commit = Envelope(90).commit;
    input.payload.assign(8, 90);
    Check(old && Coherent(old->View()), "caller mutation changed published view");
    Check(slot.Publish(Envelope(2)) == ReadPublishResult::PUBLISHED, "second publication failed");
    auto current{slot.Acquire()};
    Check(current && current->View().publication == 2 && Coherent(current->View()), "new coherent head unavailable");
    Check(old->View().publication == 1 && Coherent(old->View()), "old lease changed under publication");
    Check(!slot.Acquire() && slot.Readers() == 2, "reader bound bypassed");
    old.reset();
    auto reused{slot.Acquire()};
    Check(reused && reused->View().publication == 2 && slot.Readers() == 2, "released permit not reusable");
}

void MoveOnlyPermitAccounting()
{
    ReadSlot slot{2, 8};
    Check(slot.Publish(Envelope(1)) == ReadPublishResult::PUBLISHED, "publication failed");
    auto first{slot.Acquire()}, second{slot.Acquire()};
    Check(first && second && slot.Readers() == 2, "two readers unavailable");
    ReadLease held{std::move(*first)};
    first.reset();
    Check(slot.Readers() == 2, "move released a live permit");
    *second = std::move(held);
    Check(slot.Readers() == 1 && Coherent(second->View()), "move assignment lost/doubled permit");
    bool moved_refused{false};
    try { (void)held.View(); } catch (const std::logic_error&) { moved_refused = true; }
    Check(moved_refused, "moved-from lease exposed data");
    second.reset();
    Check(slot.Readers() == 0, "last reader permit leaked");
}

void LeaseOutlivesSlot()
{
    std::optional<ReadLease> lease;
    {
        ReadSlot slot{1, 8};
        Check(slot.Publish(Envelope(1)) == ReadPublishResult::PUBLISHED, "publication failed");
        lease = slot.Acquire();
    }
    Check(lease && Coherent(lease->View()), "slot destruction invalidated retained lease");
    lease.reset(); // Permit control also outlives the slot; no raw owner pointer.
}

void BoundedRetentionUnderPublication()
{
    ReadSlot slot{1, 8};
    Check(slot.Publish(Envelope(1)) == ReadPublishResult::PUBLISHED, "publication failed");
    auto retained{slot.Acquire()};
    for (uint64_t number{2}; number <= 1000; ++number) {
        Check(slot.Publish(Envelope(number)) == ReadPublishResult::PUBLISHED, "bounded publisher refused");
        Check(!slot.Acquire() && slot.Readers() == 1, "retained reader bypassed capacity");
    }
    Check(retained && retained->View().publication == 1 && Coherent(retained->View()), "retained old view changed");
    retained.reset();
    auto latest{slot.Acquire()};
    Check(latest && latest->View().publication == 1000 && Coherent(latest->View()), "latest head not available after release");
}

void ConcurrentCoherentPublication()
{
    ReadSlot slot{4, 8};
    Check(slot.Publish(Envelope(1)) == ReadPublishResult::PUBLISHED, "initial publication failed");
    std::latch start{1};
    std::atomic<bool> failed{false};
    std::vector<std::thread> readers;
    // The supported macOS libc++ has no jthread. Explicit joined ownership
    // gives the same fixture lifetime, including partial launch failure.
    struct Drain {
        std::latch& start;
        std::vector<std::thread>& readers;
        bool opened{false};
        void Open() { if (!opened) { start.count_down(); opened = true; } }
        ~Drain() { Open(); for (auto& reader : readers) if (reader.joinable()) reader.join(); }
    } drain{start, readers};
    for (int reader{0}; reader < 4; ++reader) readers.emplace_back([&] {
        start.wait();
        for (int step{0}; step < 2000; ++step) {
            auto lease{slot.Acquire()};
            if (!lease || !Coherent(lease->View())) failed = true;
        }
    });
    drain.Open();
    for (uint64_t number{2}; number <= 201; ++number) {
        if (slot.Publish(Envelope(number)) != ReadPublishResult::PUBLISHED) failed = true;
    }
    for (auto& reader : readers) reader.join();
    Check(!failed && slot.Readers() == 0, "concurrent view mixed heads or lost permits");
    auto last{slot.Acquire()};
    Check(last && last->View().publication == 201 && Coherent(last->View()), "final publication mismatch");
}
} // namespace

size_t RunReadSlotTests()
{
    size_t count{0};
    const auto run = [&](const char* name, auto test) {
        test();
        std::cout << "PASS " << name << '\n';
        ++count;
    };
    run("read.invalid-and-empty", InvalidAndEmpty);
    run("read.exact-payload-limit", ExactPayloadLimit);
    run("read.deep-ownership-one-head", DeepOwnershipAndOneHead);
    run("read.move-only-permit-accounting", MoveOnlyPermitAccounting);
    run("read.lease-outlives-slot", LeaseOutlivesSlot);
    run("read.bounded-retention", BoundedRetentionUnderPublication);
    run("read.concurrent-coherence", ConcurrentCoherentPublication);
    return count;
}
