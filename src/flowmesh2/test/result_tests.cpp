// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// <cmath> first on purpose: the macOS <math.h> defines OVERFLOW and DOMAIN as
// macros. Including it before the fm2 headers proves that no fm2 enumerator
// collides with them (ARCHITECTURE.md O-21).
#include <cmath>

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/result.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using namespace flowmesh2;

namespace {

// Compile-time contract of the error model.
static_assert(!std::is_default_constructible_v<Result<int>>, "Result<int>{} must not compile");
static_assert(!std::is_default_constructible_v<Result<void>>, "Result<void>{} must not compile");
static_assert(!std::is_default_constructible_v<Result<std::string>>);
static_assert(!std::is_default_constructible_v<Error>, "an Error always names its code");
static_assert(!std::is_constructible_v<Result<int>, int>, "success is spelled Ok(v), never an implicit T");
static_assert(!std::is_convertible_v<int, Result<int>>);
static_assert(!std::is_constructible_v<Result<void>, int>);
static_assert(std::is_convertible_v<Error, Result<int>>, "an Error converts implicitly");
static_assert(std::is_convertible_v<Error, Result<void>>);
static_assert(std::is_convertible_v<OkValue<int>, Result<int64_t>>, "Ok(v) converts to a wider Result");
static_assert(std::is_convertible_v<OkVoid, Result<void>>);
static_assert(!std::is_convertible_v<OkVoid, Result<int>>, "Ok() cannot make a Result<int>");
static_assert(!std::is_convertible_v<OkValue<int>, Result<void>>);
static_assert(std::is_trivially_copyable_v<Error>, "an Error owns no memory");

// The task ID is the literal at the stub site, also at compile time.
static_assert(NotImplemented("R1").code == ErrorCode::NOT_IMPLEMENTED);
static_assert(NotImplemented("M2-LED-05").detail == "M2-LED-05");
static_assert(Unsupported("M3-ENG-09").code == ErrorCode::UNSUPPORTED);
static_assert(Unsupported("M3-ENG-09").detail == "M3-ENG-09");

// The TaskId format accepted at compile time equals the TODO.md ID pattern.
static_assert(IsWellFormedTaskId("R1"));
static_assert(IsWellFormedTaskId("R14"));
static_assert(IsWellFormedTaskId("M0-AUD-01"));
static_assert(IsWellFormedTaskId("M1-C1-00"));
static_assert(IsWellFormedTaskId("M2-LED-05"));
static_assert(IsWellFormedTaskId("M10-SPEC-01"));
static_assert(IsWellFormedTaskId("M11-REL-06"));
static_assert(IsWellFormedTaskId("MB1-B3D-02"));
static_assert(IsWellFormedTaskId("MB1-ADP-06"));
static_assert(IsWellFormedTaskId("B0-B3D-01"));
static_assert(!IsWellFormedTaskId(""));
static_assert(!IsWellFormedTaskId("R"));
static_assert(!IsWellFormedTaskId("R0"));
static_assert(!IsWellFormedTaskId("R01"));
static_assert(!IsWellFormedTaskId("r1"));
static_assert(!IsWellFormedTaskId(" R1"));
static_assert(!IsWellFormedTaskId("R1 "));
static_assert(!IsWellFormedTaskId("R1-LED-01"));
static_assert(!IsWellFormedTaskId("M2-LED-5"));
static_assert(!IsWellFormedTaskId("M2-LED-005"));
static_assert(!IsWellFormedTaskId("M2-LED-05x"));
static_assert(!IsWellFormedTaskId("M2-led-05"));
static_assert(!IsWellFormedTaskId("M2-1ED-05"));
static_assert(!IsWellFormedTaskId("M02-LED-05"));
static_assert(!IsWellFormedTaskId("M-LED-05"));
static_assert(!IsWellFormedTaskId("M2--05"));
static_assert(!IsWellFormedTaskId("M2-LED05"));
static_assert(!IsWellFormedTaskId("MB2-ADP-06"));
static_assert(!IsWellFormedTaskId("B1-B3D-01"));
static_assert(!IsWellFormedTaskId("X1-ABC-01"));

//! Append-only pin of every ErrorCode integer and name. A new code is added at
//! the END of this vector in the same commit; renumbering fails this test.
const std::vector<std::pair<int, std::string_view>> PINNED_ERROR_CODES{
    {1, "NOT_IMPLEMENTED"},
    {2, "UNSUPPORTED"},
    {3, "UNSUPPORTED_NETWORK"},
    {4, "DECODE"},
    {5, "OVERSIZE"},
    {6, "ARITHMETIC_OVERFLOW"},
    {7, "INVARIANT"},
    {8, "ORDER_VIOLATION"},
    {9, "EQUIVOCATION_REFUSED"},
    {10, "STORAGE_FATAL"},
    {11, "FENCED"},
    {12, "AUTHORITY_CHANGED"},
    {13, "VERIFY_FAILED"},
    {14, "STALE_PARENT"},
    {15, "BUSY"},
    {16, "NEED_DATA"},
    {17, "INVALID_INPUT"},
};

Result<int> StubReturningInt() { return NotImplemented("M3-ENG-02"); }
Result<void> StubReturningVoid() { return Unsupported("M2-LED-05"); }

Result<int> ParseSmall(int raw)
{
    if (raw < 0) return Error{ErrorCode::DECODE, "negative"};
    if (raw > 100) return Error{ErrorCode::OVERSIZE, "above 100"};
    return Ok(raw);
}

Result<int64_t> DoubleIt(int raw)
{
    const auto parsed{ParseSmall(raw)};
    if (!parsed) return parsed.error();
    return Ok(int64_t{*parsed} * 2);
}

} // namespace

BOOST_AUTO_TEST_SUITE(result_tests)

BOOST_AUTO_TEST_CASE(not_implemented_carries_exact_task_id)
{
    const Error direct{NotImplemented("M2-LED-05")};
    BOOST_CHECK(direct.code == ErrorCode::NOT_IMPLEMENTED);
    BOOST_CHECK_EQUAL(std::string{direct.detail}, "M2-LED-05");

    const auto through_result{StubReturningInt()};
    BOOST_REQUIRE(!through_result.has_value());
    BOOST_CHECK(through_result.error().code == ErrorCode::NOT_IMPLEMENTED);
    BOOST_CHECK_EQUAL(std::string{through_result.error().detail}, "M3-ENG-02");

    for (const Error& e : {NotImplemented("R14"), NotImplemented("M10-SPEC-01"), NotImplemented("MB1-ADP-06"),
                           NotImplemented("B0-B3D-01")}) {
        BOOST_CHECK(e.code == ErrorCode::NOT_IMPLEMENTED);
        BOOST_CHECK(IsWellFormedTaskId(e.detail));
    }
    BOOST_CHECK(NotImplemented("M10-SPEC-01").detail == "M10-SPEC-01");
    BOOST_CHECK(NotImplemented("M10-SPEC-01") != NotImplemented("M10-SPEC-02"));
}

BOOST_AUTO_TEST_CASE(unsupported_carries_exact_task_id)
{
    const auto result{StubReturningVoid()};
    BOOST_REQUIRE(!result.has_value());
    BOOST_CHECK(result.error().code == ErrorCode::UNSUPPORTED);
    BOOST_CHECK_EQUAL(std::string{result.error().detail}, "M2-LED-05");
    BOOST_CHECK(result.error() == Unsupported("M2-LED-05"));
    BOOST_CHECK(result.error() != NotImplemented("M2-LED-05"));
}

BOOST_AUTO_TEST_CASE(result_has_no_default_success)
{
    // The static_asserts at the top of this file are the real check; this case
    // pins that an Error-built Result is never a value.
    const Result<int> from_error{Error{ErrorCode::BUSY}};
    BOOST_CHECK(!from_error.has_value());
    BOOST_CHECK(!static_cast<bool>(from_error));
    BOOST_CHECK(from_error.error().detail.empty());
    const Result<void> void_from_error{Error{ErrorCode::FENCED}};
    BOOST_CHECK(!void_from_error.has_value());
}

BOOST_AUTO_TEST_CASE(ok_value_round_trip)
{
    Result<int> number{Ok(42)};
    BOOST_REQUIRE(number.has_value());
    BOOST_CHECK(static_cast<bool>(number));
    BOOST_CHECK_EQUAL(number.value(), 42);
    BOOST_CHECK_EQUAL(*number, 42);
    number.value() = 7;
    BOOST_CHECK_EQUAL(*number, 7);

    const Result<std::string> text{Ok(std::string{"slot"})};
    BOOST_REQUIRE(text.has_value());
    BOOST_CHECK_EQUAL(text->size(), 4U);

    Result<std::unique_ptr<int>> owned{Ok(std::make_unique<int>(5))};
    BOOST_REQUIRE(owned.has_value());
    const std::unique_ptr<int> taken{std::move(owned).value()};
    BOOST_REQUIRE(taken);
    BOOST_CHECK_EQUAL(*taken, 5);

    const Result<int64_t> widened{Ok(int32_t{-3})};
    BOOST_REQUIRE(widened.has_value());
    BOOST_CHECK_EQUAL(*widened, int64_t{-3});

    const auto chained{DoubleIt(21)};
    BOOST_REQUIRE(chained.has_value());
    BOOST_CHECK_EQUAL(*chained, int64_t{42});
}

BOOST_AUTO_TEST_CASE(ok_void_round_trip)
{
    const Result<void> done{Ok()};
    BOOST_CHECK(done.has_value());
    BOOST_CHECK(static_cast<bool>(done));
    BOOST_CHECK_NO_THROW(done.value());
}

BOOST_AUTO_TEST_CASE(error_round_trip)
{
    const Result<int> failed{Error{ErrorCode::DECODE, "trailing bytes"}};
    BOOST_REQUIRE(!failed.has_value());
    BOOST_CHECK(failed.error() == (Error{ErrorCode::DECODE, "trailing bytes"}));
    BOOST_CHECK(failed.error() != (Error{ErrorCode::DECODE, "other"}));
    BOOST_CHECK(failed.error() != (Error{ErrorCode::OVERSIZE, "trailing bytes"}));
    BOOST_CHECK_THROW(failed.value(), util::BadExpectedAccess);

    const Result<void> void_failed{Error{ErrorCode::STORAGE_FATAL, "barrier"}};
    BOOST_REQUIRE(!void_failed.has_value());
    BOOST_CHECK(void_failed.error().code == ErrorCode::STORAGE_FATAL);
    BOOST_CHECK_THROW(void_failed.value(), util::BadExpectedAccess);

    // Errors propagate unchanged through a caller with another value type.
    const auto negative{DoubleIt(-1)};
    BOOST_REQUIRE(!negative.has_value());
    BOOST_CHECK(negative.error() == (Error{ErrorCode::DECODE, "negative"}));
    const auto large{DoubleIt(101)};
    BOOST_REQUIRE(!large.has_value());
    BOOST_CHECK(large.error().code == ErrorCode::OVERSIZE);
}

BOOST_AUTO_TEST_CASE(error_codes_pinned)
{
    const auto registry{ErrorCodeRegistry()};
    BOOST_REQUIRE_EQUAL(registry.size(), PINNED_ERROR_CODES.size());
    for (size_t i{0}; i < registry.size(); ++i) {
        BOOST_CHECK_EQUAL(static_cast<int>(registry[i].code), PINNED_ERROR_CODES[i].first);
        BOOST_CHECK_EQUAL(std::string{registry[i].name}, std::string{PINNED_ERROR_CODES[i].second});
        BOOST_CHECK(IsKnown(registry[i].code));
        BOOST_CHECK_EQUAL(std::string{ToString(registry[i].code)}, std::string{PINNED_ERROR_CODES[i].second});
    }
    BOOST_CHECK_EQUAL(static_cast<int>(ErrorCode::NOT_IMPLEMENTED), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(ErrorCode::ARITHMETIC_OVERFLOW), 6);
    BOOST_CHECK_EQUAL(static_cast<int>(ErrorCode::INVALID_INPUT), 17);
}

BOOST_AUTO_TEST_CASE(error_code_to_string_total)
{
    // Every value of the underlying type maps to its pinned name or "UNKNOWN".
    for (uint32_t raw{0}; raw <= 0xFFFF; ++raw) {
        const auto code{static_cast<ErrorCode>(raw)};
        std::string_view expected{"UNKNOWN"};
        for (const auto& [value, name] : PINNED_ERROR_CODES) {
            if (static_cast<uint32_t>(value) == raw) expected = name;
        }
        if (ToString(code) != expected || IsKnown(code) != (expected != "UNKNOWN")) {
            BOOST_ERROR("ErrorCode " << raw << " maps to " << std::string{ToString(code)});
        }
    }
    BOOST_CHECK_EQUAL(std::string{ToString(static_cast<ErrorCode>(0))}, "UNKNOWN");
    BOOST_CHECK(!IsKnown(static_cast<ErrorCode>(18)));
}

BOOST_AUTO_TEST_SUITE_END()
