// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <flowmesh2/storage/durable.h>

#include <cstdint>
#include <type_traits>

using namespace flowmesh2;

namespace {

// Compile-time contract of the durability tokens (ARCHITECTURE.md 7.2). The
// constructor is private, so is_constructible is false outside Journal; the
// compile-fail control test/compile_fail/forge_token.cpp proves the same with
// a real construction attempt.
template <class Token>
constexpr bool Unforgeable()
{
    return !std::is_default_constructible_v<Token> &&
           !std::is_copy_constructible_v<Token> &&
           !std::is_copy_assignable_v<Token> &&
           !std::is_move_assignable_v<Token> &&
           std::is_nothrow_move_constructible_v<Token> &&
           !std::is_constructible_v<Token, uint64_t, uint64_t> &&
           !std::is_constructible_v<Token, int, int> &&
           !std::is_convertible_v<uint64_t, Token>;
}

static_assert(Unforgeable<DurableIntent>());
static_assert(Unforgeable<DurableSignedVote>());
static_assert(Unforgeable<DurableDecision>());
static_assert(Unforgeable<DurableApplied>());
static_assert(!std::is_constructible_v<DurableIntent, DurableDecision&&>, "one barrier's token never stands in for another's");
static_assert(!std::is_constructible_v<DurableApplied, DurableDecision&&>);

} // namespace

BOOST_AUTO_TEST_SUITE(durable_tests)

BOOST_AUTO_TEST_CASE(token_kinds_pinned)
{
    // DurableKind values follow RecordType where they match (INTENT=1,
    // SIGNED_VOTE=2, DECISION=3, APPLIED=4); they are pinned here.
    BOOST_CHECK_EQUAL(static_cast<int>(DurableIntent::KIND), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(DurableSignedVote::KIND), 2);
    BOOST_CHECK_EQUAL(static_cast<int>(DurableDecision::KIND), 3);
    BOOST_CHECK_EQUAL(static_cast<int>(DurableApplied::KIND), 4);
}

BOOST_AUTO_TEST_SUITE_END()
