// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <crypto/hex_base.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/engine/engine.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::engine;
using namespace flowmesh2::test;

namespace {

const uint256 PARENT{LabelHash("fm2/test/parent")};
constexpr uint64_t HEIGHT{7};

AccountId Acc(const std::string& name) { return AccountId{LabelHash("fm2/test/account/" + name)}; }
ActionId Act(const std::string& label) { return ActionId{LabelHash("fm2/test/action/" + label)}; }

//! The golden input of evidence/R7/golden.py (action label, account, nonce).
std::vector<OrderingEntry> GoldenEntries()
{
    return {{Act("0"), Acc("alice"), 5}, {Act("1"), Acc("alice"), 3}, {Act("2"), Acc("bob"), 1},
            {Act("3"), Acc("alice"), 9}, {Act("4"), Acc("bob"), 2},   {Act("5"), Acc("carol"), 7},
            {Act("6"), Acc("carol"), 4}, {Act("7"), Acc("bob"), 11}};
}

std::vector<size_t> Order(std::span<const OrderingEntry> entries, const uint256& parent, uint64_t height)
{
    const Result<std::vector<size_t>> order{CanonicalOrder(entries, parent, height)};
    RequireOk(order, "CanonicalOrder");
    return *order;
}

//! The ActionIds in canonical order.
std::vector<ActionId> OrderedIds(std::span<const OrderingEntry> entries, const uint256& parent, uint64_t height)
{
    std::vector<ActionId> ids;
    for (const size_t i : Order(entries, parent, height)) ids.push_back(entries[i].id);
    return ids;
}

//! Every account's nonces in execution order must ascend.
bool NoncesAscendPerAccount(std::span<const OrderingEntry> entries, const std::vector<size_t>& order)
{
    std::map<AccountId, uint64_t> last;
    for (const size_t i : order) {
        const auto it{last.find(entries[i].account)};
        if (it != last.end() && entries[i].nonce <= it->second) return false;
        last[entries[i].account] = entries[i].nonce;
    }
    return true;
}

//! The interleaving of accounts, ignoring which action of an account comes when.
std::vector<AccountId> AccountPattern(std::span<const OrderingEntry> entries, const std::vector<size_t>& order)
{
    std::vector<AccountId> pattern;
    for (const size_t i : order) pattern.push_back(entries[i].account);
    return pattern;
}

uint64_t Lcg(uint64_t& state)
{
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state >> 33;
}

ActionV2 SignedTransfer(const std::string& signer, uint64_t nonce)
{
    ActionV2 a;
    a.domain = LabelHash("fm2/test/domain");
    a.signer = XOnlyFromLabel("fm2/test/" + signer);
    a.nonce = nonce;
    a.ref_slot = 1;
    a.max_delay = 16;
    a.kind = ActionKind::TRANSFER;
    a.payload = EncodePayload(TransferPayload{Acc("x"), AssetId::Native(), 1}).value();
    a.sig = SignDigest(KeyFromLabel("fm2/test/" + signer), a.Id().id, uint256::ONE);
    return a;
}

ActionV2 WithKind(ActionKind kind, std::vector<unsigned char> payload)
{
    ActionV2 a;
    a.kind = kind;
    a.payload = std::move(payload);
    return a;
}

} // namespace

BOOST_AUTO_TEST_SUITE(canonical_order_tests)

BOOST_AUTO_TEST_CASE(order_key_golden)
{
    // Golden from evidence/R7/golden.py: Tagged(ORDER, parent || u64 LE height || ActionId).
    BOOST_CHECK_EQUAL(HexStr(OrderKey(PARENT, HEIGHT, Act("0"))),
                      "f5757c06bcfba650c829ff513bc9098700414fb20b38c8f3693140cd179d44d2");
    // Every input changes the key.
    const uint256 key{OrderKey(PARENT, HEIGHT, Act("0"))};
    BOOST_CHECK(OrderKey(LabelHash("fm2/test/parent2"), HEIGHT, Act("0")) != key);
    BOOST_CHECK(OrderKey(PARENT, HEIGHT + 1, Act("0")) != key);
    BOOST_CHECK(OrderKey(PARENT, HEIGHT, Act("1")) != key);
}

BOOST_AUTO_TEST_CASE(golden_canonical_order)
{
    const std::vector<OrderingEntry> entries{GoldenEntries()};
    // Golden from evidence/R7/golden.py: the seeded sort is [4, 6, 2, 5, 3, 7, 1, 0];
    // refilling each account's positions in ascending nonce gives:
    const std::vector<size_t> expected{2, 6, 4, 5, 1, 7, 0, 3};
    const std::vector<size_t> order{Order(entries, PARENT, HEIGHT)};
    BOOST_CHECK_EQUAL_COLLECTIONS(order.begin(), order.end(), expected.begin(), expected.end());
    // That is (bob 1) (carol 4) (bob 2) (carol 7) (alice 3) (bob 11) (alice 5) (alice 9).
    const std::vector<std::pair<std::string, uint64_t>> sequence{{"bob", 1},  {"carol", 4}, {"bob", 2},   {"carol", 7},
                                                                 {"alice", 3}, {"bob", 11}, {"alice", 5}, {"alice", 9}};
    for (size_t r{0}; r < order.size(); ++r) {
        BOOST_CHECK(entries[order[r]].account == Acc(sequence[r].first));
        BOOST_CHECK_EQUAL(entries[order[r]].nonce, sequence[r].second);
    }
    BOOST_CHECK(NoncesAscendPerAccount(entries, order));
}

BOOST_AUTO_TEST_CASE(permuting_originals_gives_the_same_order)
{
    std::vector<OrderingEntry> entries{GoldenEntries()};
    const std::vector<ActionId> expected{OrderedIds(entries, PARENT, HEIGHT)};
    uint64_t seed{1};
    for (int round{0}; round < 64; ++round) {
        for (size_t i{entries.size()}; i > 1; --i) std::swap(entries[i - 1], entries[Lcg(seed) % i]);
        const std::vector<ActionId> ids{OrderedIds(entries, PARENT, HEIGHT)};
        BOOST_CHECK(ids == expected);
    }
}

BOOST_AUTO_TEST_CASE(parent_hash_changes_cross_account_order_only)
{
    const std::vector<OrderingEntry> entries{GoldenEntries()};
    std::set<std::vector<AccountId>> patterns;
    for (uint32_t k{0}; k < 64; ++k) {
        const uint256 parent{LabelHash("fm2/test/parent/" + std::to_string(k))};
        const std::vector<size_t> order{Order(entries, parent, HEIGHT)};
        // One account's actions always run in nonce order...
        BOOST_CHECK(NoncesAscendPerAccount(entries, order));
        patterns.insert(AccountPattern(entries, order));
    }
    // ...while the interleaving of accounts depends on the parent header.
    BOOST_CHECK_GT(patterns.size(), 8U);

    // A single account's order never depends on the seed.
    const std::vector<OrderingEntry> one{{Act("a"), Acc("alice"), 30}, {Act("b"), Acc("alice"), 10}, {Act("c"), Acc("alice"), 20}};
    for (uint32_t k{0}; k < 16; ++k) {
        const std::vector<size_t> order{Order(one, LabelHash("fm2/test/parent/" + std::to_string(k)), k)};
        const std::vector<size_t> expected{1, 2, 0};
        BOOST_CHECK_EQUAL_COLLECTIONS(order.begin(), order.end(), expected.begin(), expected.end());
    }
}

BOOST_AUTO_TEST_CASE(height_changes_the_order)
{
    const std::vector<OrderingEntry> entries{GoldenEntries()};
    std::set<std::vector<AccountId>> patterns;
    for (uint64_t height{1}; height <= 32; ++height) {
        const std::vector<size_t> order{Order(entries, PARENT, height)};
        BOOST_CHECK(NoncesAscendPerAccount(entries, order));
        patterns.insert(AccountPattern(entries, order));
    }
    BOOST_CHECK_GT(patterns.size(), 4U);
}

BOOST_AUTO_TEST_CASE(empty_single_and_refusals)
{
    BOOST_CHECK(Order(std::vector<OrderingEntry>{}, PARENT, HEIGHT).empty());
    const std::vector<OrderingEntry> single{{Act("0"), Acc("alice"), 5}};
    BOOST_CHECK(Order(single, PARENT, HEIGHT) == std::vector<size_t>{0});

    std::vector<OrderingEntry> repeated_id{GoldenEntries()};
    repeated_id.push_back(OrderingEntry{Act("3"), Acc("dave"), 1});
    RequireError(CanonicalOrder(repeated_id, PARENT, HEIGHT), ErrorCode::INVALID_INPUT, "repeated ActionId");
    std::vector<OrderingEntry> repeated_nonce{GoldenEntries()};
    repeated_nonce.push_back(OrderingEntry{Act("8"), Acc("bob"), 2});
    RequireError(CanonicalOrder(repeated_nonce, PARENT, HEIGHT), ErrorCode::INVALID_INPUT, "repeated (account, nonce)");
}

BOOST_AUTO_TEST_CASE(verified_action_overload_matches_entries)
{
    std::vector<VerifiedAction> actions;
    std::vector<OrderingEntry> entries;
    for (const auto& [signer, nonce] : std::vector<std::pair<std::string, uint64_t>>{
             {"alice", 4}, {"bob", 9}, {"alice", 2}, {"carol", 1}, {"bob", 3}, {"alice", 7}}) {
        const Result<VerifiedAction> verified{VerifiedAction::Verify(SignedTransfer(signer, nonce))};
        RequireOk(verified, "Verify");
        entries.push_back(OrderingEntry{verified->Id(), verified->Account(), nonce});
        actions.push_back(*verified);
    }
    const Result<std::vector<size_t>> from_actions{CanonicalOrder(std::span<const VerifiedAction>{actions}, PARENT, HEIGHT)};
    RequireOk(from_actions, "CanonicalOrder(VerifiedAction)");
    BOOST_CHECK(*from_actions == Order(entries, PARENT, HEIGHT));
    BOOST_CHECK(NoncesAscendPerAccount(entries, *from_actions));
}

BOOST_AUTO_TEST_CASE(phase_of_every_kind)
{
    const MarketId market{LabelHash("fm2/test/market")};
    const auto place = [&](TimeInForce tif) {
        return WithKind(ActionKind::PLACE, EncodePayload(PlacePayload{market, Side::BUY, tif, 10, 1}).value());
    };
    BOOST_CHECK(PhaseOf(place(TimeInForce::GTC)).value() == SlotPhase::TAKERS);
    BOOST_CHECK(PhaseOf(place(TimeInForce::IOC)).value() == SlotPhase::TAKERS);
    BOOST_CHECK(PhaseOf(place(TimeInForce::POST_ONLY)).value() == SlotPhase::POST_ONLY);
    BOOST_CHECK(PhaseOf(WithKind(ActionKind::CANCEL, EncodePayload(CancelPayload{}).value())).value() == SlotPhase::CANCELS);
    BOOST_CHECK(PhaseOf(WithKind(ActionKind::TRANSFER, EncodePayload(TransferPayload{}).value())).value() == SlotPhase::ACCOUNT);
    // A PLACE whose payload does not decode returns the decoder's error.
    RequireError(PhaseOf(WithKind(ActionKind::PLACE, {0x01})), ErrorCode::DECODE, "truncated PLACE payload");
    // The stub branches (the validity predicate refuses these kinds first).
    RequireStub(PhaseOf(WithKind(ActionKind::CANCEL_ALL, {})), "M3-ENG-09");
    RequireStub(PhaseOf(WithKind(ActionKind::WITHDRAW, {})), "M2-LED-05");
    RequireStub(PhaseOf(WithKind(ActionKind::SPOT_TO_FUTURES, {})), "M2-LED-06");
    RequireStub(PhaseOf(WithKind(ActionKind::FUTURES_TO_SPOT, {})), "M2-LED-06");
    RequireError(PhaseOf(WithKind(static_cast<ActionKind>(99), {})), ErrorCode::INVALID_INPUT, "unregistered kind");

    // The phase registry is pinned.
    BOOST_CHECK_EQUAL(static_cast<int>(SlotPhase::DEPOSITS), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(SlotPhase::CONSERVATION), 9);
    for (int p{1}; p <= 9; ++p) BOOST_CHECK(IsKnown(static_cast<SlotPhase>(p)));
    BOOST_CHECK(!IsKnown(static_cast<SlotPhase>(0)));
    BOOST_CHECK(!IsKnown(static_cast<SlotPhase>(10)));
    BOOST_CHECK_EQUAL(ToString(SlotPhase::TAKERS), "TAKERS");
    BOOST_CHECK_EQUAL(ToString(static_cast<SlotPhase>(10)), "UNKNOWN");
}

BOOST_AUTO_TEST_CASE(arrival_rank_grows_by_slot_phase_and_rank)
{
    constexpr uint32_t cap{1'024};
    BOOST_CHECK_EQUAL(ArrivalRank(1, SlotPhase::POST_ONLY, 0, cap).value(), 2U * cap);
    BOOST_CHECK_EQUAL(ArrivalRank(1, SlotPhase::POST_ONLY, cap - 1, cap).value(), 3U * cap - 1);
    BOOST_CHECK_EQUAL(ArrivalRank(1, SlotPhase::REST, 0, cap).value(), 3U * cap);
    BOOST_CHECK_EQUAL(ArrivalRank(2, SlotPhase::POST_ONLY, 0, cap).value(), 4U * cap);
    // Strictly increasing along (height, phase, rank) for a fixed cap.
    uint64_t previous{0};
    for (uint64_t height{1}; height <= 4; ++height) {
        for (const SlotPhase phase : {SlotPhase::POST_ONLY, SlotPhase::REST}) {
            for (uint64_t rank : {uint64_t{0}, uint64_t{1}, uint64_t{cap - 1}}) {
                const uint64_t r{ArrivalRank(height, phase, rank, cap).value()};
                BOOST_CHECK_GT(r, previous);
                previous = r;
            }
        }
    }
    // A post-only order of rank 900 queues ahead of a remainder of rank 3 in the same slot.
    BOOST_CHECK_LT(ArrivalRank(5, SlotPhase::POST_ONLY, 900, cap).value(), ArrivalRank(5, SlotPhase::REST, 3, cap).value());

    RequireError(ArrivalRank(1, SlotPhase::TAKERS, 0, cap), ErrorCode::INVALID_INPUT, "phase 5");
    RequireError(ArrivalRank(1, SlotPhase::POST_ONLY, cap, cap), ErrorCode::INVALID_INPUT, "rank at the cap");
    RequireError(ArrivalRank(1, SlotPhase::POST_ONLY, 0, 0), ErrorCode::INVALID_INPUT, "zero cap");
    // Overflow edges: 2 * height overflows, and the block start overflows.
    RequireError(ArrivalRank(std::numeric_limits<uint64_t>::max() / 2 + 1, SlotPhase::POST_ONLY, 0, 1),
                 ErrorCode::ARITHMETIC_OVERFLOW, "2 * height");
    RequireError(ArrivalRank(std::numeric_limits<uint64_t>::max() / 4, SlotPhase::REST, 0, cap), ErrorCode::ARITHMETIC_OVERFLOW,
                 "block start");
    // The largest representable rank with cap 1: (2h+1) * 1 + 0 at h = (2^64-2)/2.
    const uint64_t h{(std::numeric_limits<uint64_t>::max() - 1) / 2};
    BOOST_CHECK_EQUAL(ArrivalRank(h, SlotPhase::REST, 0, 1).value(), std::numeric_limits<uint64_t>::max());
}

BOOST_AUTO_TEST_SUITE_END()
