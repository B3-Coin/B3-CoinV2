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

#endif // B3_FLOWMESH2_TEST_UTIL_REQUIRE_H
