// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

// Isolated FlowMesh2 bounded-mailbox foundation tests. No node, chain,
// wallet, authentication, consensus, disk, network or latency qualification.
#include <flowmesh2/mailbox.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Implemented by the independently authored read-slot test translation unit.
// It prints each actual case, returns only when all seven cases complete,
// and throws on failure. This is not a placeholder or success-only stub.
size_t RunReadSlotTests();

namespace {
using flowmesh2::BoundedMailbox;
using flowmesh2::MailboxLimits;
using flowmesh2::MailboxMessage;
using flowmesh2::MailboxPushResult;

std::atomic_size_t g_checks{0};

// Deliberately not assert(): release/NDEBUG builds execute every check.
void Check(bool condition, const char* expression, const char* file, int line)
{
    ++g_checks;
    if (!condition) throw std::runtime_error{std::string{file} + ":" + std::to_string(line) + ": " + expression};
}
#define CHECK(condition) Check(static_cast<bool>(condition), #condition, __FILE__, __LINE__)

uint256 Id(uint32_t value)
{
    uint256 id;
    for (unsigned i{0}; i < 4; ++i) id.begin()[i] = static_cast<unsigned char>(value >> (8 * i));
    return id;
}

uint32_t IdNumber(const uint256& id)
{
    uint32_t value{0};
    for (unsigned i{0}; i < 4; ++i) value |= uint32_t{id.begin()[i]} << (8 * i);
    return value;
}

MailboxMessage Message(uint32_t id, size_t bytes)
{
    MailboxMessage message{Id(id), std::vector<unsigned char>(bytes)};
    for (size_t i{0}; i < bytes; ++i) message.payload[i] = static_cast<unsigned char>(id + 17 * i);
    return message;
}

void StatsEqual(const BoundedMailbox& mailbox, size_t messages, size_t bytes, bool closed = false)
{
    const auto stats{mailbox.Stats()};
    CHECK(stats.messages == messages); CHECK(stats.payload_bytes == bytes); CHECK(stats.closed == closed);
}

void SameMessage(const MailboxMessage& actual, const MailboxMessage& expected)
{
    CHECK(actual.request_id == expected.request_id); CHECK(actual.payload == expected.payload);
}

void Push(BoundedMailbox& mailbox, uint32_t id, size_t bytes)
{
    auto message{Message(id, bytes)};
    CHECK(mailbox.TryPush(std::move(message)) == MailboxPushResult::ACCEPTED);
}

void Pop(BoundedMailbox& mailbox, uint32_t id, size_t bytes)
{
    auto message{mailbox.TryPop()}; CHECK(message.has_value()); SameMessage(*message, Message(id, bytes));
}

// Predetermined start gate, not a sleep or retry-until-success loop. A failed
// thread creation still releases and joins every already-created worker.
class StartGate {
    std::mutex m_mutex;
    std::condition_variable m_ready;
    bool m_open{false};
public:
    void Open() { { std::lock_guard lock{m_mutex}; m_open = true; } m_ready.notify_all(); }
    void Wait()
    {
        std::unique_lock lock{m_mutex};
        if (!m_ready.wait_for(lock, std::chrono::seconds{10}, [&] { return m_open; }))
            throw std::runtime_error{"Bounded worker start gate timed out"};
    }
};

void Workers(size_t count, const std::function<void(size_t)>& run)
{
    StartGate gate;
    std::vector<std::exception_ptr> errors(count);
    std::vector<std::thread> threads; threads.reserve(count);
    struct Drain {
        StartGate& gate; std::vector<std::thread>& threads;
        ~Drain() { gate.Open(); for (auto& thread : threads) if (thread.joinable()) thread.join(); }
    } drain{gate, threads};
    for (size_t i{0}; i < count; ++i) {
        threads.emplace_back([&, i] { try { gate.Wait(); run(i); } catch (...) { errors[i] = std::current_exception(); } });
    }
    gate.Open();
    for (auto& thread : threads) thread.join();
    for (const auto& error : errors) if (error) std::rethrow_exception(error);
}

void ZeroLimitsRejected()
{
    for (const auto limits : {MailboxLimits{0, 1}, MailboxLimits{1, 0}, MailboxLimits{0, 0}}) {
        bool invalid{false};
        try { BoundedMailbox mailbox{limits}; } catch (const std::invalid_argument&) { invalid = true; }
        CHECK(invalid);
    }
}

void CountBoundAndFifo()
{
    BoundedMailbox mailbox{{2, 16}}; StatsEqual(mailbox, 0, 0); CHECK(!mailbox.TryPop());
    Push(mailbox, 1, 4); Push(mailbox, 2, 4); StatsEqual(mailbox, 2, 8);
    auto third{Message(3, 4)}; CHECK(mailbox.TryPush(std::move(third)) == MailboxPushResult::FULL);
    SameMessage(third, Message(3, 4)); StatsEqual(mailbox, 2, 8);
    Pop(mailbox, 1, 4); StatsEqual(mailbox, 1, 4); Push(mailbox, 3, 4);
    Pop(mailbox, 2, 4); Pop(mailbox, 3, 4); CHECK(!mailbox.TryPop()); StatsEqual(mailbox, 0, 0);
}

void PayloadByteExactEdges()
{
    BoundedMailbox mailbox{{4, 5}}; Push(mailbox, 1, 3); Push(mailbox, 2, 2); StatsEqual(mailbox, 2, 5);
    auto one{Message(3, 1)}; CHECK(mailbox.TryPush(std::move(one)) == MailboxPushResult::FULL);
    SameMessage(one, Message(3, 1)); StatsEqual(mailbox, 2, 5);
    Pop(mailbox, 1, 3); Push(mailbox, 3, 3); StatsEqual(mailbox, 2, 5);
    Pop(mailbox, 2, 2); Pop(mailbox, 3, 3); Push(mailbox, 4, 5); StatsEqual(mailbox, 1, 5);
    Pop(mailbox, 4, 5); StatsEqual(mailbox, 0, 0);
}

void RejectionPriorityAndCallerRetention()
{
    BoundedMailbox mailbox{{1, 3}}; Push(mailbox, 1, 3);
    auto full{Message(2, 1)}; const auto* full_bytes{full.payload.data()};
    CHECK(mailbox.TryPush(std::move(full)) == MailboxPushResult::FULL);
    SameMessage(full, Message(2, 1)); CHECK(full.payload.data() == full_bytes);
    auto oversize{Message(3, 4)}; const auto* oversized_bytes{oversize.payload.data()};
    CHECK(mailbox.TryPush(std::move(oversize)) == MailboxPushResult::OVERSIZE);
    SameMessage(oversize, Message(3, 4)); CHECK(oversize.payload.data() == oversized_bytes);
    mailbox.Close();
    CHECK(mailbox.TryPush(std::move(oversize)) == MailboxPushResult::CLOSED);
    SameMessage(oversize, Message(3, 4)); CHECK(oversize.payload.data() == oversized_bytes);
    CHECK(mailbox.TryPush(std::move(full)) == MailboxPushResult::CLOSED);
    SameMessage(full, Message(2, 1)); CHECK(full.payload.data() == full_bytes);
    StatsEqual(mailbox, 1, 3, true); Pop(mailbox, 1, 3); StatsEqual(mailbox, 0, 0, true);
}

void EmptyPayloadStillConsumesCount()
{
    BoundedMailbox mailbox{{2, 1}}; Push(mailbox, 1, 0); Push(mailbox, 2, 0); StatsEqual(mailbox, 2, 0);
    auto empty{Message(3, 0)}; CHECK(mailbox.TryPush(std::move(empty)) == MailboxPushResult::FULL);
    Pop(mailbox, 1, 0); Push(mailbox, 3, 1); StatsEqual(mailbox, 2, 1);
    Pop(mailbox, 2, 0); Pop(mailbox, 3, 1); StatsEqual(mailbox, 0, 0);
}

void SuccessfulPushTransfersPayload()
{
    BoundedMailbox mailbox{{1, 64}}; auto original{Message(42, 64)}; const auto* allocation{original.payload.data()};
    CHECK(mailbox.TryPush(std::move(original)) == MailboxPushResult::ACCEPTED);
    auto popped{mailbox.TryPop()}; CHECK(popped.has_value()); SameMessage(*popped, Message(42, 64));
    // The accepted vector's buffer moved through the queue, rather than an
    // opaque request body being copied or reserialized.
    CHECK(popped->payload.data() == allocation); StatsEqual(mailbox, 0, 0);
}

void CloseRetainsExactFifoAndIsIdempotent()
{
    BoundedMailbox mailbox{{3, 9}}; Push(mailbox, 5, 2); Push(mailbox, 5, 3); Push(mailbox, 9, 4);
    mailbox.Close(); mailbox.Close(); StatsEqual(mailbox, 3, 9, true);
    Pop(mailbox, 5, 2); StatsEqual(mailbox, 2, 7, true); Pop(mailbox, 5, 3); Pop(mailbox, 9, 4);
    CHECK(!mailbox.TryPop()); mailbox.Close(); StatsEqual(mailbox, 0, 0, true);
    auto message{Message(10, 1)}; CHECK(mailbox.TryPush(std::move(message)) == MailboxPushResult::CLOSED);
    SameMessage(message, Message(10, 1));
}

void IndependentMailboxCapacity()
{
    BoundedMailbox blocked{{1, 1}}, available{{2, 4}}; Push(blocked, 1, 1);
    auto rejected{Message(2, 1)}; CHECK(blocked.TryPush(std::move(rejected)) == MailboxPushResult::FULL);
    Push(available, 3, 2); Push(available, 4, 2); StatsEqual(available, 2, 4); StatsEqual(blocked, 1, 1);
    blocked.Close(); Pop(available, 3, 2); Push(available, 5, 2); Pop(available, 4, 2); Pop(available, 5, 2);
    StatsEqual(available, 0, 0); StatsEqual(blocked, 1, 1, true); Pop(blocked, 1, 1);
}

void MaximumConfiguredLimitsDoNotPreallocate()
{
    const auto maximum{std::numeric_limits<size_t>::max()};
    BoundedMailbox mailbox{{maximum, maximum}}; StatsEqual(mailbox, 0, 0);
    Push(mailbox, 1, 4); Push(mailbox, 2, 5); Push(mailbox, 3, 0); StatsEqual(mailbox, 3, 9);
    mailbox.Close(); Pop(mailbox, 1, 4); Pop(mailbox, 2, 5); Pop(mailbox, 3, 0); StatsEqual(mailbox, 0, 0, true);
    // This exercises SIZE_MAX configuration, not physically impossible
    // SIZE_MAX-byte allocations. Addition safety also needs source review.
}

void DeterministicReferenceSequence()
{
    constexpr MailboxLimits limits{7, 31}; BoundedMailbox mailbox{limits};
    std::deque<MailboxMessage> reference; size_t bytes{0}; bool closed{false}; uint32_t seed{24092026};
    for (uint32_t operation{0}; operation < 512; ++operation) {
        seed = seed * 1664525U + 1013904223U;
        if (operation == 384) { mailbox.Close(); closed = true; }
        if ((seed & 3U) != 0) {
            auto message{Message(operation % 63, (seed >> 8) % 36)}; const auto expected_message{message};
            const auto expected{closed ? MailboxPushResult::CLOSED : message.payload.size() > limits.max_payload_bytes ? MailboxPushResult::OVERSIZE :
                reference.size() == limits.max_messages || message.payload.size() > limits.max_payload_bytes - bytes ? MailboxPushResult::FULL : MailboxPushResult::ACCEPTED};
            CHECK(mailbox.TryPush(std::move(message)) == expected);
            if (expected == MailboxPushResult::ACCEPTED) { bytes += expected_message.payload.size(); reference.push_back(expected_message); }
            else SameMessage(message, expected_message);
        } else {
            auto message{mailbox.TryPop()}; CHECK(message.has_value() == !reference.empty());
            if (message) { SameMessage(*message, reference.front()); bytes -= reference.front().payload.size(); reference.pop_front(); }
        }
        StatsEqual(mailbox, reference.size(), bytes, closed);
    }
    mailbox.Close();
    while (!reference.empty()) { auto message{mailbox.TryPop()}; CHECK(message); SameMessage(*message, reference.front()); reference.pop_front(); }
    CHECK(!mailbox.TryPop()); StatsEqual(mailbox, 0, 0, true);
}

void ConcurrentUniqueConsumptionAndCoherentStats()
{
    constexpr size_t producers{4}, each{128}, total{producers * each}, body_bytes{8};
    BoundedMailbox mailbox{{total, total * body_bytes}};
    Workers(producers, [&](size_t producer) {
        for (size_t i{0}; i < each; ++i) {
            Push(mailbox, static_cast<uint32_t>(producer * each + i), body_bytes);
            const auto stats{mailbox.Stats()}; CHECK(stats.messages <= total); CHECK(stats.payload_bytes == stats.messages * body_bytes); CHECK(!stats.closed);
        }
    });
    StatsEqual(mailbox, total, total * body_bytes);
    std::vector<std::vector<uint32_t>> consumed(producers);
    for (auto& values : consumed) values.reserve(each);
    Workers(producers, [&](size_t consumer) {
        for (size_t i{0}; i < each; ++i) {
            auto message{mailbox.TryPop()}; CHECK(message); const auto id{IdNumber(message->request_id)};
            CHECK(id < total); SameMessage(*message, Message(id, body_bytes)); consumed[consumer].push_back(id);
            const auto stats{mailbox.Stats()}; CHECK(stats.messages <= total); CHECK(stats.payload_bytes == stats.messages * body_bytes); CHECK(!stats.closed);
        }
    });
    std::vector<uint32_t> unique; unique.reserve(total);
    for (const auto& values : consumed) unique.insert(unique.end(), values.begin(), values.end());
    std::sort(unique.begin(), unique.end()); CHECK(unique.size() == total);
    for (size_t i{0}; i < total; ++i) CHECK(unique[i] == i);
    CHECK(!mailbox.TryPop()); StatsEqual(mailbox, 0, 0);
}

void ConcurrentClosePreservesEveryAcceptedMessage()
{
    constexpr size_t producers{4}, each{32}, total{producers * each};
    BoundedMailbox mailbox{{total + 1, total + 1}}; Push(mailbox, 1000, 1);
    std::vector<std::vector<uint32_t>> accepted(producers);
    for (auto& values : accepted) values.reserve(each);
    Workers(producers + 1, [&](size_t worker) {
        if (worker == producers) { mailbox.Close(); return; }
        for (size_t i{0}; i < each; ++i) {
            const auto id{static_cast<uint32_t>(worker * each + i)}; auto message{Message(id, 1)};
            const auto result{mailbox.TryPush(std::move(message))};
            CHECK(result == MailboxPushResult::ACCEPTED || result == MailboxPushResult::CLOSED);
            if (result == MailboxPushResult::ACCEPTED) accepted[worker].push_back(id);
            else SameMessage(message, Message(id, 1));
        }
    });
    std::vector<uint32_t> expected{1000};
    for (const auto& values : accepted) expected.insert(expected.end(), values.begin(), values.end());
    StatsEqual(mailbox, expected.size(), expected.size(), true);
    std::vector<uint32_t> actual;
    while (auto message = mailbox.TryPop()) { const auto id{IdNumber(message->request_id)}; SameMessage(*message, Message(id, 1)); actual.push_back(id); }
    CHECK(!actual.empty()); CHECK(actual.front() == 1000); // Preloaded obligation cannot disappear.
    std::sort(actual.begin(), actual.end()); std::sort(expected.begin(), expected.end()); CHECK(actual == expected);
    StatsEqual(mailbox, 0, 0, true);
}

using Test = std::pair<std::string_view, void (*)()>;
const std::vector<Test> TESTS{
    {"zero_limits_rejected", ZeroLimitsRejected},
    {"count_bound_and_fifo", CountBoundAndFifo},
    {"payload_byte_exact_edges", PayloadByteExactEdges},
    {"rejection_priority_and_caller_retention", RejectionPriorityAndCallerRetention},
    {"empty_payload_consumes_count", EmptyPayloadStillConsumesCount},
    {"successful_push_transfers_payload", SuccessfulPushTransfersPayload},
    {"close_retains_exact_fifo", CloseRetainsExactFifoAndIsIdempotent},
    {"independent_mailbox_capacity", IndependentMailboxCapacity},
    {"maximum_configured_limits", MaximumConfiguredLimitsDoNotPreallocate},
    {"deterministic_reference_sequence", DeterministicReferenceSequence},
    {"concurrent_unique_consumption_and_stats", ConcurrentUniqueConsumptionAndCoherentStats},
    {"concurrent_close_retains_accepted", ConcurrentClosePreservesEveryAcceptedMessage},
};
} // namespace

int main(int argc, char*[])
{
    if (argc != 1) { std::cerr << "Usage: test_flowmesh2_foundations\n"; return 2; }
    if (TESTS.size() != 12) { std::cerr << "Declared mailbox profile requires exactly twelve cases\n"; return 3; }
    size_t cases{0}, failures{0};
    std::cout << "Profile: flowmesh2-clean-core-foundations-test/1; no production qualification\n";
    for (const auto& [name, run] : TESTS) {
        ++cases;
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
        catch (...) { ++failures; std::cerr << "FAIL " << name << ": non-standard exception\n"; }
    }
    size_t read_cases{0};
    try { read_cases = RunReadSlotTests(); if (read_cases != 7) throw std::runtime_error{"Declared read-slot profile requires exactly seven completed cases"}; }
    catch (const std::exception& error) { ++failures; std::cerr << "FAIL read-slot-group: " << error.what() << '\n'; }
    catch (...) { ++failures; std::cerr << "FAIL read-slot-group: non-standard exception\n"; }
    std::cout << "Mailbox cases attempted: " << cases << "; mailbox checks: " << g_checks.load() << "; failures: " << failures << '\n';
    if (!failures) std::cout << "TOTAL: " << cases + read_cases << " passing cases (12 mailbox + 7 read-slot)\n";
    else std::cout << "Qualification FAILED; read-slot partial-case progress is reported above, never counted as a passing group\n";
    return failures ? 1 : 0;
}
