// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/codes.h>
#include <flowmesh2/core/nonce_rule.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/ledger/nonce_window.h>

#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace flowmesh2;
using flowmesh2::ledger::AdmissionCodeFor;
using flowmesh2::ledger::NonceWindow;

namespace {

NonceWindow Empty(uint32_t window) { return NonceWindow::Create(window).value(); }

std::vector<uint64_t> UsedOf(const NonceWindow& w) { return {w.Used().begin(), w.Used().end()}; }

/** Naive reference written from plan section 3.6 with a std::set, independent
 * of core/nonce_rule: accept n iff n is not used and (fewer than W are used or
 * n is above the smallest); on accept insert n and drop the smallest while
 * more than W are used.
 */
struct NaiveWindow {
    uint32_t window;
    std::set<uint64_t> used;

    NonceVerdict Classify(uint64_t n) const
    {
        if (used.contains(n)) return NonceVerdict::USED;
        if (used.size() < window || n > *used.begin()) return NonceVerdict::ACCEPTABLE;
        return NonceVerdict::TOO_LOW;
    }
    bool Insert(uint64_t n)
    {
        if (Classify(n) != NonceVerdict::ACCEPTABLE) return false;
        used.insert(n);
        while (used.size() > window) used.erase(used.begin());
        return true;
    }
};

struct ExhaustiveRun {
    uint64_t sequences{0};
    uint64_t attempts{0};
    uint64_t accepted{0};
    std::string first_problem;
};

void Note(ExhaustiveRun& run, const std::string& problem)
{
    if (run.first_problem.empty()) run.first_problem = problem;
}

//! Depth-first over every attempt sequence: each prefix extends by every value.
void Explore(const NonceWindow& window, const NaiveWindow& naive, int depth, int max_depth, uint64_t alphabet,
             ExhaustiveRun& run)
{
    ++run.sequences;
    if (!run.first_problem.empty() || depth == max_depth) return;
    for (uint64_t n{0}; n < alphabet; ++n) {
        ++run.attempts;
        if (window.Classify(n) != naive.Classify(n)) {
            Note(run, "verdict differs at depth " + std::to_string(depth) + " for nonce " + std::to_string(n));
            return;
        }
        NonceWindow next_window{window};
        NaiveWindow next_naive{naive};
        const Result<void> inserted{next_window.Insert(n)};
        const bool naive_inserted{next_naive.Insert(n)};
        if (inserted.has_value() != naive_inserted) {
            Note(run, "insert outcome differs for nonce " + std::to_string(n));
            return;
        }
        if (!inserted.has_value() && (inserted.error().code != ErrorCode::INVALID_INPUT || !(next_window == window))) {
            Note(run, "a refused insert changed the window or gave the wrong code");
            return;
        }
        if (naive_inserted) ++run.accepted;
        const std::vector<uint64_t> got{UsedOf(next_window)};
        const std::vector<uint64_t> want(next_naive.used.begin(), next_naive.used.end());
        if (got != want || !NonceWindowValid(next_window.Used(), next_window.Size())) {
            Note(run, "window contents differ after nonce " + std::to_string(n));
            return;
        }
        Explore(next_window, next_naive, depth + 1, max_depth, alphabet, run);
    }
}

} // namespace

BOOST_AUTO_TEST_SUITE(nonce_window_tests)

BOOST_AUTO_TEST_CASE(w3_every_sequence_up_to_7_matches_naive_model)
{
    // Every attempt sequence of length 0..7 over the nonces 0..7 with W = 3:
    // sum of 8^l for l = 0..7 sequences. Refused attempts stay in the sequence.
    ExhaustiveRun run;
    Explore(Empty(3), NaiveWindow{3, {}}, 0, 7, 8, run);
    BOOST_REQUIRE_MESSAGE(run.first_problem.empty(), run.first_problem);
    BOOST_CHECK_EQUAL(run.sequences, 2'396'745U);
    BOOST_CHECK_EQUAL(run.attempts, 2'396'744U);
    BOOST_TEST_MESSAGE("accepted inserts: " << run.accepted);
    BOOST_CHECK_GT(run.accepted, 0U);
    BOOST_CHECK_LT(run.accepted, run.attempts);
}

BOOST_AUTO_TEST_CASE(w100_boundaries)
{
    NonceWindow w{Empty(100)};
    for (uint64_t n{1}; n <= 99; ++n) BOOST_REQUIRE(w.Insert(n).has_value());
    // 99 used: a nonce below the minimum is still acceptable (|used| < W).
    BOOST_CHECK(w.Classify(0) == NonceVerdict::ACCEPTABLE);
    BOOST_REQUIRE(w.Insert(100).has_value());

    // 100 fill the window exactly.
    BOOST_CHECK_EQUAL(w.Used().size(), 100U);
    BOOST_CHECK_EQUAL(w.Used().front(), 1U);
    BOOST_CHECK_EQUAL(w.Used().back(), 100U);
    // Below the minimum: TOO_LOW; a used nonce: USED.
    BOOST_CHECK(w.Classify(0) == NonceVerdict::TOO_LOW);
    BOOST_CHECK(AdmissionCodeFor(w.Classify(0)) == AdmissionCode::NONCE_TOO_LOW);
    for (uint64_t n{1}; n <= 100; ++n) BOOST_CHECK(w.Classify(n) == NonceVerdict::USED);
    BOOST_CHECK(AdmissionCodeFor(w.Classify(50)) == AdmissionCode::NONCE_USED);

    // The 101st evicts the minimum.
    BOOST_CHECK(w.Classify(101) == NonceVerdict::ACCEPTABLE);
    BOOST_REQUIRE(w.Insert(101).has_value());
    BOOST_CHECK_EQUAL(w.Used().size(), 100U);
    BOOST_CHECK_EQUAL(w.Used().front(), 2U);
    BOOST_CHECK_EQUAL(w.Used().back(), 101U);
    // The evicted nonce can never be used again; the new minimum is USED.
    BOOST_CHECK(w.Classify(1) == NonceVerdict::TOO_LOW);
    BOOST_CHECK(w.Classify(2) == NonceVerdict::USED);

    // Refused inserts leave the window unchanged.
    const NonceWindow before{w};
    const Result<void> too_low{w.Insert(1)};
    BOOST_REQUIRE(!too_low.has_value());
    BOOST_CHECK(too_low.error().code == ErrorCode::INVALID_INPUT);
    const Result<void> used{w.Insert(2)};
    BOOST_REQUIRE(!used.has_value());
    BOOST_CHECK(used.error().code == ErrorCode::INVALID_INPUT);
    BOOST_CHECK(w == before);

    // A gap above the minimum is fine; so is the largest nonce, exactly once.
    BOOST_REQUIRE(w.Insert(5'000).has_value());
    BOOST_CHECK_EQUAL(w.Used().front(), 3U);
    const uint64_t max{std::numeric_limits<uint64_t>::max()};
    BOOST_CHECK(w.Classify(max) == NonceVerdict::ACCEPTABLE);
    BOOST_REQUIRE(w.Insert(max).has_value());
    BOOST_CHECK(w.Classify(max) == NonceVerdict::USED);
    BOOST_CHECK_EQUAL(w.Used().size(), 100U);
    BOOST_CHECK(NonceWindowValid(w.Used(), w.Size()));
}

BOOST_AUTO_TEST_CASE(create_and_from_used_refuse_malformed_windows)
{
    const Result<NonceWindow> zero{NonceWindow::Create(0)};
    BOOST_REQUIRE(!zero.has_value());
    BOOST_CHECK(zero.error().code == ErrorCode::INVALID_INPUT);

    for (const std::vector<uint64_t>& bad : {std::vector<uint64_t>{1, 1}, std::vector<uint64_t>{2, 1},
                                            std::vector<uint64_t>{1, 2, 3, 4}}) {
        const Result<NonceWindow> w{NonceWindow::FromUsed(3, bad)};
        BOOST_REQUIRE(!w.has_value());
        BOOST_CHECK(w.error().code == ErrorCode::INVALID_INPUT);
    }
    BOOST_CHECK(!NonceWindow::FromUsed(0, {}).has_value());

    const std::vector<uint64_t> used{1, 5, 9};
    const Result<NonceWindow> w{NonceWindow::FromUsed(3, used)};
    BOOST_REQUIRE(w.has_value());
    BOOST_CHECK(UsedOf(*w) == used);
    BOOST_CHECK_EQUAL(w->Size(), 3U);
    // Full window {1, 5, 9}: below the minimum is TOO_LOW, a gap above it is fine.
    BOOST_CHECK(w->Classify(0) == NonceVerdict::TOO_LOW);
    BOOST_CHECK(w->Classify(4) == NonceVerdict::ACCEPTABLE);
    BOOST_CHECK(w->Classify(5) == NonceVerdict::USED);
    BOOST_CHECK(*w != Empty(3));
    BOOST_CHECK(Empty(3) != Empty(4));
}

BOOST_AUTO_TEST_CASE(admission_codes_for_verdicts)
{
    BOOST_CHECK(AdmissionCodeFor(NonceVerdict::USED) == AdmissionCode::NONCE_USED);
    BOOST_CHECK(AdmissionCodeFor(NonceVerdict::TOO_LOW) == AdmissionCode::NONCE_TOO_LOW);
    BOOST_CHECK(!AdmissionCodeFor(NonceVerdict::ACCEPTABLE).has_value());
    BOOST_CHECK(!AdmissionCodeFor(static_cast<NonceVerdict>(0)).has_value());
    BOOST_CHECK(!AdmissionCodeFor(static_cast<NonceVerdict>(9)).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
