// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/ledger/persistent_tree.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace flowmesh2::ledger;

namespace {

using Map = PersistentMap<uint64_t, std::string>;
using Shadow = std::map<uint64_t, std::string>;

//! splitmix64: deterministic test randomness.
struct Rng {
    uint64_t state;
    uint64_t Next()
    {
        uint64_t z{state += 0x9e3779b97f4a7c15ULL};
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    uint64_t Below(uint64_t n) { return n == 0 ? 0 : Next() % n; }
};

//! Every read of `map` agrees with `shadow`. Calls Boost only on a failure.
void RequireSame(const Map& map, const Shadow& shadow, Rng& rng)
{
    if (map.size() != shadow.size()) BOOST_REQUIRE_EQUAL(map.size(), shadow.size());
    if (!std::equal(map.begin(), map.end(), shadow.begin(), shadow.end())) BOOST_FAIL("listing differs");
    if (!map.CheckShape()) BOOST_FAIL("not an AVL tree");
    for (int probe{0}; probe < 8; ++probe) {
        const uint64_t key{rng.Below(4'000)};
        const auto it{shadow.find(key)};
        const std::string* found{map.Find(key)};
        if ((found == nullptr) != (it == shadow.end()) || (found != nullptr && *found != it->second)) BOOST_FAIL("Find differs");
        const auto lower{shadow.lower_bound(key)};
        const auto mine{map.lower_bound(key)};
        if ((mine == map.end()) != (lower == shadow.end()) || (mine != map.end() && *mine != *lower)) BOOST_FAIL("lower_bound differs");
        const size_t rank{static_cast<size_t>(std::distance(shadow.begin(), lower))};
        if (map.Rank(key) != rank) BOOST_REQUIRE_EQUAL(map.Rank(key), rank);
        if (map.contains(key) != (it != shadow.end())) BOOST_FAIL("contains differs");
        if ((map.find(key) == map.end()) != (it == shadow.end())) BOOST_FAIL("find differs");
    }
}

//! The changes Diff must report, from the two listings alone.
std::vector<std::pair<std::optional<std::pair<uint64_t, std::string>>, std::optional<std::pair<uint64_t, std::string>>>>
NaiveDiff(const Shadow& before, const Shadow& after)
{
    std::vector<std::pair<std::optional<std::pair<uint64_t, std::string>>, std::optional<std::pair<uint64_t, std::string>>>> out;
    std::set<uint64_t> keys;
    for (const auto& entry : before) keys.insert(entry.first);
    for (const auto& entry : after) keys.insert(entry.first);
    for (const uint64_t key : keys) {
        const auto b{before.find(key)};
        const auto a{after.find(key)};
        if (b != before.end() && a != after.end() && b->second == a->second) continue;
        out.emplace_back(b == before.end() ? std::nullopt : std::optional{*b}, a == after.end() ? std::nullopt : std::optional{*a});
    }
    return out;
}

auto DiffOf(const Map& before, const Map& after)
{
    std::vector<std::pair<std::optional<std::pair<uint64_t, std::string>>, std::optional<std::pair<uint64_t, std::string>>>> out;
    Map::Diff(before, after, [&out](const Map::value_type* b, const Map::value_type* a) {
        out.emplace_back(b == nullptr ? std::nullopt : std::optional{std::pair<uint64_t, std::string>{*b}},
                         a == nullptr ? std::nullopt : std::optional{std::pair<uint64_t, std::string>{*a}});
        return true;
    });
    return out;
}

//! A value that counts its comparisons.
struct Counted {
    int value{0};
    int* compares{nullptr};
    friend bool operator==(const Counted& a, const Counted& b)
    {
        ++*a.compares;
        return a.value == b.value;
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(persistent_tree_tests)

BOOST_AUTO_TEST_CASE(a_map_matches_std_map_and_every_copy_keeps_its_contents)
{
    // 10,000 random inserts and writes, with a copy taken every 500
    // operations. Each copy is checked against the std::map it was taken
    // from after all later writes, so no write ever reached a shared node.
    Rng rng{0x7ee};
    Map map;
    Shadow shadow;
    std::vector<std::pair<Map, Shadow>> copies;
    for (int op{0}; op < 10'000; ++op) {
        const uint64_t key{rng.Below(4'000)};
        if (rng.Below(3) == 0) {
            const std::string value{"v" + std::to_string(rng.Next() % 1'000)};
            const auto [written, inserted]{map.TryEmplace(key, value)};
            const bool fresh{shadow.try_emplace(key, value).second};
            if (inserted != fresh || *written != shadow.at(key)) BOOST_FAIL("TryEmplace differs");
        } else if (std::string* value{map.FindMutable(key)}) {
            *value += '+';
            shadow.at(key) += '+';
        } else if (shadow.contains(key)) {
            BOOST_FAIL("FindMutable lost a key");
        }
        if (op % 500 == 0) copies.emplace_back(map, shadow);
        if (op % 97 == 0) RequireSame(map, shadow, rng);
    }
    RequireSame(map, shadow, rng);
    for (const auto& [copy, expected] : copies) RequireSame(copy, expected, rng);
    BOOST_CHECK_GT(map.size(), 2'000U);
}

BOOST_AUTO_TEST_CASE(diff_reports_exactly_the_keys_whose_values_differ)
{
    Rng rng{0xd1ff};
    for (int round{0}; round < 30; ++round) {
        Map base;
        Shadow base_shadow;
        const uint64_t size{rng.Below(round < 10 ? 20 : 1'500)};
        for (uint64_t i{0}; i < size; ++i) {
            const uint64_t key{rng.Below(10'000)};
            base.TryEmplace(key, std::to_string(key));
            base_shadow.try_emplace(key, std::to_string(key));
        }
        Map next{base};
        Shadow next_shadow{base_shadow};
        const uint64_t changes{rng.Below(round % 3 == 0 ? 3 : 200)};
        for (uint64_t c{0}; c < changes; ++c) {
            const uint64_t key{rng.Below(10'000)};
            const uint64_t kind{rng.Below(3)};
            if (kind == 0) {
                next.TryEmplace(key, "new");
                next_shadow.try_emplace(key, "new");
            } else if (std::string* value{next.FindMutable(key)}) {
                // kind 1 writes the same value back: a cloned path with no change.
                if (kind == 2) {
                    *value += '!';
                    next_shadow.at(key) += '!';
                }
            }
        }
        BOOST_TEST_CONTEXT("round " << round << " size " << base.size() << " changes " << changes)
        {
            BOOST_REQUIRE(DiffOf(base, next) == NaiveDiff(base_shadow, next_shadow));
            BOOST_REQUIRE(DiffOf(next, base) == NaiveDiff(next_shadow, base_shadow));
            BOOST_REQUIRE(DiffOf(base, base).empty());
            BOOST_REQUIRE_EQUAL(base == next, base_shadow == next_shadow);
        }
        // Trees with no shared node: built separately from the same listing.
        Map rebuilt;
        for (const auto& [key, value] : next_shadow) rebuilt.TryEmplace(key, value);
        BOOST_REQUIRE(DiffOf(next, rebuilt).empty());
        BOOST_REQUIRE(next == rebuilt);
        BOOST_REQUIRE(DiffOf(base, rebuilt) == NaiveDiff(base_shadow, next_shadow));
    }
}

BOOST_AUTO_TEST_CASE(diff_visits_only_the_changed_paths)
{
    // 2^16 keys, 10 changed: Diff compares values only on the changed paths
    // and the nodes next to them, so it calls operator== far fewer times than
    // a walk of the whole tree would.
    int compares{0};
    PersistentMap<uint64_t, Counted> base;
    for (uint64_t key{0}; key < (1u << 16); ++key) base.TryEmplace(key, Counted{0, &compares});
    PersistentMap<uint64_t, Counted> next{base};
    for (uint64_t key{1}; key <= 10; ++key) next.FindMutable(key * 6'007)->value = 1;
    size_t reported{0};
    PersistentMap<uint64_t, Counted>::Diff(base, next, [&reported](const auto*, const auto*) {
        ++reported;
        return true;
    });
    BOOST_CHECK_EQUAL(reported, 10U);
    BOOST_TEST_MESSAGE("value comparisons for 10 changes in 65,536 keys: " << compares);
    BOOST_CHECK_LT(compares, 10 * 2 * 20);
}

BOOST_AUTO_TEST_CASE(writes_stay_in_place_until_the_tree_is_copied)
{
    Map map;
    for (uint64_t key{0}; key < 100; ++key) map.TryEmplace(key, "x");
    std::string* first{map.FindMutable(42)};
    BOOST_CHECK_EQUAL(map.FindMutable(42), first); // this tree made the node: no clone
    *first = "a";
    const Map copy{map};
    std::string* second{map.FindMutable(42)};
    BOOST_CHECK_NE(second, first); // shared since the copy: cloned before the write
    *second = "b";
    BOOST_CHECK_EQUAL(*copy.Find(42), "a");
    BOOST_CHECK_EQUAL(*map.Find(42), "b");
    // A write to the copy's copy reaches neither.
    Map third{copy};
    *third.FindMutable(42) = "c";
    BOOST_CHECK_EQUAL(*copy.Find(42), "a");
    BOOST_CHECK_EQUAL(*map.Find(42), "b");
    // A moved-from tree is empty and usable; the target keeps the values.
    Map moved{std::move(third)};
    BOOST_CHECK_EQUAL(*moved.Find(42), "c");
    BOOST_CHECK(third.empty()); // NOLINT(bugprone-use-after-move): the moved-from state is specified
    third.TryEmplace(1, "one");
    BOOST_CHECK_EQUAL(third.size(), 1U);
    BOOST_CHECK_THROW(map.at(1'000), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(sets_insert_once_and_list_their_keys_ascending)
{
    PersistentSet<uint64_t> set;
    std::set<uint64_t> shadow;
    Rng rng{0x5e7};
    for (int i{0}; i < 5'000; ++i) {
        const uint64_t key{rng.Below(2'000)};
        BOOST_REQUIRE_EQUAL(set.Insert(key), shadow.insert(key).second);
    }
    BOOST_CHECK(std::ranges::equal(set, shadow));
    BOOST_CHECK(set.CheckShape());
    const PersistentSet<uint64_t> before{set};
    BOOST_CHECK(set.Insert(5'000));
    BOOST_CHECK(!before.contains(5'000));
    BOOST_CHECK_EQUAL(before.size() + 1, set.size());
}

BOOST_AUTO_TEST_CASE(sequential_inserts_keep_the_tree_balanced)
{
    PersistentSet<uint64_t> set;
    for (uint64_t key{0}; key < 32'768; ++key) set.Insert(key);
    BOOST_CHECK(set.CheckShape());
    // An AVL tree of n nodes is less than 1.4405 log2(n + 2) high: 21 here.
    BOOST_CHECK_LE(set.Height(), 21U);
    BOOST_CHECK_EQUAL(set.Rank(16'384), 16'384U);
}

BOOST_AUTO_TEST_CASE(copies_are_changed_and_read_on_several_threads_at_once)
{
    // One shared base; four threads each copy it and change their copy while
    // two more read the base. Under TSan this is the data-race check of the
    // token scheme; everywhere it checks that no write reached the base.
    Map base;
    for (uint64_t key{0}; key < 5'000; ++key) base.TryEmplace(key, "base");
    std::vector<Map> results(4);
    std::vector<size_t> seen(2, 0);
    std::vector<std::thread> threads;
    for (size_t t{0}; t < results.size(); ++t) {
        threads.emplace_back([&base, &results, t] {
            Map mine{base};
            for (uint64_t key{t}; key < 5'000; key += 7) *mine.FindMutable(key) = "thread" + std::to_string(t);
            for (uint64_t key{5'000 + t}; key < 6'000; key += 4) mine.TryEmplace(key, "new");
            results[t] = std::move(mine);
        });
    }
    for (size_t r{0}; r < seen.size(); ++r) {
        threads.emplace_back([&base, &seen, r] {
            for (int pass{0}; pass < 5; ++pass) {
                for (const auto& [key, value] : base) seen[r] += value == "base" ? 1 : 0;
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    for (const size_t count : seen) BOOST_CHECK_EQUAL(count, 5U * 5'000);
    for (const auto& [key, value] : base) BOOST_REQUIRE_EQUAL(value, "base");
    for (size_t t{0}; t < results.size(); ++t) {
        BOOST_CHECK(results[t].CheckShape());
        BOOST_CHECK_EQUAL(*results[t].Find(t), "thread" + std::to_string(t));
        BOOST_CHECK_EQUAL(*results[t].Find(t + 1), "base"); // another thread's key: its write never arrives here
        BOOST_CHECK_EQUAL(results[t].size(), 5'000U + 250U);
    }
}

BOOST_AUTO_TEST_SUITE_END()
