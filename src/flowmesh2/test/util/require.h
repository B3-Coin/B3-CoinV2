// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_TEST_UTIL_REQUIRE_H
#define B3_FLOWMESH2_TEST_UTIL_REQUIRE_H

#include <flowmesh2/core/result.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <string_view>

/** Assertion helpers shared by several fixture headers (slice R10).
 *
 * RequireStub used to be defined in both test/util/ledger_fixtures.h and
 * test/util/storage_fixtures.h, so no translation unit could include both
 * (reported in R9). Both now include this one definition.
 */
namespace flowmesh2::test {

//! Requires the NOT_IMPLEMENTED stub error of `task_id` (and never reads error() on success).
template <class T>
void RequireStub(const Result<T>& result, std::string_view task_id)
{
    if (result.has_value()) BOOST_FAIL("stub " + std::string{task_id} + " returned success");
    BOOST_CHECK(result.error().code == ErrorCode::NOT_IMPLEMENTED);
    BOOST_CHECK_EQUAL(result.error().detail, task_id);
}

} // namespace flowmesh2::test

/** BOOST_REQUIRE and BOOST_REQUIRE_EQUAL for the hot loops of exhaustive and
 * differential tests. The condition is the same and is evaluated once; a
 * failure stops the case as BOOST_REQUIRE does, with the condition text, the
 * file and line, and both values for the _EQUAL form. What they leave out is
 * Boost's bookkeeping of every passed assertion, which costs about 20 times
 * as much under ThreadSanitizer and is most of those cases' sanitizer time.
 * Use them only inside loops; a case still needs one ordinary Boost check.
 * FM2_REQUIRE_EQUAL copies both values (a reference could outlive a
 * temporary such as `*book.LevelLots(side, price)`), so use it for small values only.
 */
// clang-format off
#define FM2_REQUIRE(cond) do { if (!(cond)) BOOST_FAIL("FM2_REQUIRE(" #cond ") failed"); } while (false)
#define FM2_REQUIRE_EQUAL(a, b) do { const auto fm2_require_a = (a); const auto fm2_require_b = (b); if (!(fm2_require_a == fm2_require_b)) BOOST_FAIL("FM2_REQUIRE_EQUAL(" #a ", " #b ") failed [" << fm2_require_a << " != " << fm2_require_b << "]"); } while (false)
// clang-format on

#endif // B3_FLOWMESH2_TEST_UTIL_REQUIRE_H
