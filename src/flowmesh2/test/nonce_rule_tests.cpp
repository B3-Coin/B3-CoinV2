// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/nonce_rule.h>
#include <flowmesh2/test/util/require.h>

#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace flowmesh2;

namespace {

template <class T>
void CheckError(const Result<T>& result, ErrorCode expected)
{
    BOOST_REQUIRE_MESSAGE(!result.has_value(), "unexpected success");
    BOOST_CHECK_EQUAL(ToString(result.error().code), ToString(expected));
}

//! Direct restatement of the D7 rule over a std::set (plan section 3.6).
NonceVerdict NaiveClassify(const std::set<uint64_t>& used, uint64_t n, size_t window)
{
    if (used.count(n)) return NonceVerdict::USED;
    if (used.size() < window || n > *used.begin()) return NonceVerdict::ACCEPTABLE;
    return NonceVerdict::TOO_LOW;
}

//! The W largest elements of `all`.
std::set<uint64_t> TopW(const std::set<uint64_t>& all, size_t window)
{
    std::set<uint64_t> top;
    for (auto it{all.rbegin()}; it != all.rend() && top.size() < window; ++it) top.insert(*it);
    return top;
}

/** History model: `consumed` is every nonce ever inserted; the window is the
 * W largest of them. USED iff n is in the window; ACCEPTABLE iff n was never
 * consumed and would be among the W largest after insertion.
 */
NonceVerdict HistoryClassify(const std::set<uint64_t>& consumed, uint64_t n, size_t window)
{
    if (TopW(consumed, window).count(n)) return NonceVerdict::USED;
    std::set<uint64_t> with{consumed};
    with.insert(n);
    if (!consumed.count(n) && TopW(with, window).count(n)) return NonceVerdict::ACCEPTABLE;
    return NonceVerdict::TOO_LOW;
}

std::vector<uint64_t> AsVector(const std::set<uint64_t>& s) { return {s.begin(), s.end()}; }

constexpr uint32_t W{3};
constexpr uint64_t MAX_NONCE{9};

struct Counts {
    size_t states{0};
    size_t classifications{0};
};

void Explore(const std::vector<uint64_t>& window, const std::set<uint64_t>& consumed, size_t depth, Counts& counts)
{
    ++counts.states;
    FM2_REQUIRE(NonceWindowValid(window, W));
    FM2_REQUIRE(AsVector(TopW(consumed, W)) == window);
    for (uint64_t n{0}; n <= MAX_NONCE; ++n) {
        ++counts.classifications;
        const NonceVerdict verdict{ClassifyNonce(window, n, W).value()};
        FM2_REQUIRE(verdict == HistoryClassify(consumed, n, W));
        if (verdict != NonceVerdict::ACCEPTABLE) {
            CheckError(NonceInsert(window, n, W), ErrorCode::INVALID_INPUT);
            continue;
        }
        // The lemma VerifyExpired relies on: acceptable means never consumed.
        FM2_REQUIRE(!consumed.count(n));
        if (depth == 0) continue;
        std::set<uint64_t> next_consumed{consumed};
        next_consumed.insert(n);
        Explore(NonceInsert(window, n, W).value(), next_consumed, depth - 1, counts);
    }
}

} // namespace

BOOST_AUTO_TEST_SUITE(nonce_rule_tests)

BOOST_AUTO_TEST_CASE(verdict_registry_pinned)
{
    BOOST_CHECK_EQUAL(static_cast<int>(NonceVerdict::ACCEPTABLE), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(NonceVerdict::USED), 2);
    BOOST_CHECK_EQUAL(static_cast<int>(NonceVerdict::TOO_LOW), 3);
    BOOST_CHECK_EQUAL(std::string{ToString(NonceVerdict::TOO_LOW)}, "TOO_LOW");
    BOOST_CHECK_EQUAL(std::string{ToString(static_cast<NonceVerdict>(0))}, "UNKNOWN");
}

BOOST_AUTO_TEST_CASE(exhaustive_w3_nonces_0_to_9_against_naive_model)
{
    // Every syntactically valid window: each subset of 0..9 with at most 3 elements.
    size_t windows{0};
    for (uint32_t mask{0}; mask < (1U << (MAX_NONCE + 1)); ++mask) {
        std::set<uint64_t> used;
        for (uint64_t i{0}; i <= MAX_NONCE; ++i) {
            if (mask & (1U << i)) used.insert(i);
        }
        if (used.size() > W) continue;
        ++windows;
        const auto window{AsVector(used)};
        for (uint64_t n{0}; n <= MAX_NONCE; ++n) {
            const NonceVerdict expected{NaiveClassify(used, n, W)};
            BOOST_REQUIRE(ClassifyNonce(window, n, W).value() == expected);
            BOOST_REQUIRE(NonceAcceptable(window, n, W) == (expected == NonceVerdict::ACCEPTABLE));
            if (expected != NonceVerdict::ACCEPTABLE) continue;
            std::set<uint64_t> next{used};
            next.insert(n);
            if (next.size() > W) next.erase(next.begin());
            BOOST_REQUIRE(NonceInsert(window, n, W).value() == AsVector(next));
        }
    }
    BOOST_CHECK_EQUAL(windows, 1U + 10U + 45U + 120U);
}

BOOST_AUTO_TEST_CASE(reachable_windows_match_the_history_model)
{
    // Every insert sequence of length <= 5 over 0..9 with W = 3, compared with
    // a model that keeps the full history of consumed nonces.
    Counts counts;
    Explore({}, {}, 5, counts);
    BOOST_TEST_MESSAGE("states " << counts.states << ", classifications " << counts.classifications);
    BOOST_CHECK(counts.states > 10'000);
}

BOOST_AUTO_TEST_CASE(window_100_boundaries)
{
    constexpr uint32_t window{100};
    std::vector<uint64_t> used;
    for (uint64_t n{1}; n <= 100; ++n) used = NonceInsert(used, n, window).value();
    BOOST_CHECK_EQUAL(used.size(), 100U);
    BOOST_CHECK(ClassifyNonce(used, 1, window).value() == NonceVerdict::USED);
    BOOST_CHECK(ClassifyNonce(used, 100, window).value() == NonceVerdict::USED);
    BOOST_CHECK(ClassifyNonce(used, 0, window).value() == NonceVerdict::TOO_LOW);
    BOOST_CHECK(ClassifyNonce(used, 101, window).value() == NonceVerdict::ACCEPTABLE);
    // A gap above the minimum stays usable; inserting it evicts the minimum.
    used.erase(used.begin() + 49); // drop 50
    used = NonceInsert(used, 1'000, window).value();
    BOOST_CHECK_EQUAL(used.size(), 100U);
    BOOST_CHECK(ClassifyNonce(used, 50, window).value() == NonceVerdict::ACCEPTABLE);
    used = NonceInsert(used, 50, window).value();
    BOOST_CHECK_EQUAL(used.front(), 2U); // 1 was evicted
    BOOST_CHECK(ClassifyNonce(used, 1, window).value() == NonceVerdict::TOO_LOW);
    // Extreme values.
    const std::vector<uint64_t> top{std::numeric_limits<uint64_t>::max() - 1};
    BOOST_CHECK(NonceAcceptable(top, std::numeric_limits<uint64_t>::max(), window));
    BOOST_CHECK(NonceAcceptable(top, 0, window)); // window not full
}

BOOST_AUTO_TEST_CASE(malformed_windows_refused)
{
    const std::vector<std::vector<uint64_t>> malformed{{2, 1}, {1, 1}, {1, 2, 3, 4}};
    for (const auto& window : malformed) {
        BOOST_CHECK(!NonceWindowValid(window, W));
        CheckError(ClassifyNonce(window, 5, W), ErrorCode::INVALID_INPUT);
        BOOST_CHECK(!NonceAcceptable(window, 5, W));
        CheckError(NonceInsert(window, 5, W), ErrorCode::INVALID_INPUT);
    }
    BOOST_CHECK(!NonceWindowValid({}, 0));
    CheckError(ClassifyNonce({}, 1, 0), ErrorCode::INVALID_INPUT);
    BOOST_CHECK(NonceWindowValid({}, 1));
}

BOOST_AUTO_TEST_CASE(expiry_window_exhaustive_and_overflow_safe)
{
    for (uint64_t r{0}; r <= 20; ++r) {
        for (uint32_t d{0}; d <= 5; ++d) {
            for (uint64_t s{0}; s <= 30; ++s) {
                const bool in_window{r < s && s <= r + d};
                const bool expired{s > r + d};
                BOOST_REQUIRE(InExpiryWindow(r, static_cast<uint16_t>(d), s) == in_window);
                BOOST_REQUIRE(ExpiredAt(r, static_cast<uint16_t>(d), s) == expired);
            }
        }
    }
    constexpr uint64_t MAX{std::numeric_limits<uint64_t>::max()};
    BOOST_CHECK(InExpiryWindow(MAX - 1, 1'024, MAX));
    BOOST_CHECK(!ExpiredAt(MAX - 1, 1'024, MAX));
    BOOST_CHECK(!InExpiryWindow(MAX, 1'024, MAX));
    BOOST_CHECK(!ExpiredAt(MAX, 1'024, MAX));
    BOOST_CHECK(ExpiredAt(0, 1'024, MAX));
    BOOST_CHECK(!InExpiryWindow(0, 65'535, MAX));
    // Walking skeleton action w: ref_slot 3, max_delay 1 (ARCHITECTURE.md 14.5 N3).
    BOOST_CHECK(InExpiryWindow(3, 1, 4));
    BOOST_CHECK(!InExpiryWindow(3, 1, 5));
    BOOST_CHECK(ExpiredAt(3, 1, 5));
    BOOST_CHECK(!ExpiredAt(3, 1, 4));
    // max_delay 0 is an empty window that is already over at ref_slot + 1.
    BOOST_CHECK(!InExpiryWindow(3, 0, 4));
    BOOST_CHECK(ExpiredAt(3, 0, 4));
}

BOOST_AUTO_TEST_SUITE_END()
