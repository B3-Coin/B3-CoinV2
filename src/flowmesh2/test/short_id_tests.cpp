// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/codes.h>
#include <flowmesh2/net/short_id.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/state_fixtures.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::net;
using namespace flowmesh2::test;

namespace {

//! The reference's synthetic ActionIds: a u64 LE counter, then 24 bytes of 0xa5.
ActionId CounterId(uint64_t i)
{
    uint256 id;
    std::fill(id.begin(), id.end(), 0xa5);
    for (int b{0}; b < 8; ++b) id.data()[b] = static_cast<unsigned char>(i >> (8 * b));
    return ActionId{id};
}

ActionId LabelId(const std::string& label) { return ActionId{LabelHash("fm2/test/short-id/" + label)}; }

agreement::ShortId ShortOf(const ShortIdCodec& codec, const ActionId& id)
{
    const Result<agreement::ShortId> encoded{codec.Encode(id)};
    RequireOk(encoded, "Encode");
    return *encoded;
}

std::string HexShort(const ShortIdCodec& codec, const ActionId& id) { return HexStr(ShortOf(codec, id)); }

std::vector<ActionId> DecodeOf(const ShortIdCodec& codec, const agreement::ShortId& s, const std::vector<ActionId>& known)
{
    Result<std::vector<ActionId>> decoded{codec.Decode(s, known)};
    RequireOk(decoded, "Decode");
    return std::move(decoded).value();
}

// A pair the birthday search found (evidence scripts/shortid_collide.cpp over
// 2^26 counters); the Python reference agrees.
constexpr uint64_t COLLIDING_A{12'384'653};
constexpr uint64_t COLLIDING_B{14'495'427};

} // namespace

BOOST_AUTO_TEST_SUITE(short_id_tests)

BOOST_AUTO_TEST_CASE(a_collision_decodes_to_both_actions_and_another_salt_separates_them)
{
    const ActionId a{CounterId(COLLIDING_A)};
    const ActionId b{CounterId(COLLIDING_B)};
    const ShortIdCodec codec{uint256::ONE, 7};
    BOOST_REQUIRE_EQUAL(HexShort(codec, a), "90ef82a91b9f");
    BOOST_REQUIRE_EQUAL(HexShort(codec, b), "90ef82a91b9f");
    // Decode returns both, ascending and once each, and nothing else.
    const std::vector<ActionId> known{b, LabelId("x"), a, b, LabelId("y")};
    const std::vector<ActionId> both{DecodeOf(codec, ShortOf(codec, a), known)};
    BOOST_REQUIRE_EQUAL(both.size(), 2U);
    BOOST_CHECK(both[0] == std::min(a, b) && both[1] == std::max(a, b));
    BOOST_CHECK(DecodeOf(codec, ShortOf(codec, LabelId("x")), known) == std::vector<ActionId>{LabelId("x")});
    BOOST_CHECK(DecodeOf(codec, ShortOf(codec, LabelId("z")), known).empty());
    // Another nonce or another header hash separates the pair.
    const ShortIdCodec other_nonce{uint256::ONE, 8};
    BOOST_CHECK_EQUAL(HexShort(other_nonce, a), "4623334a9576");
    BOOST_CHECK_EQUAL(HexShort(other_nonce, b), "406563119efb");
    const ShortIdCodec other_header{LabelHash("fm2/test/short-id/header"), 7};
    BOOST_CHECK(ShortOf(other_header, a) != ShortOf(other_header, b));
}

BOOST_AUTO_TEST_CASE(keys_and_short_ids_match_the_python_reference)
{
    // shortid_ref.py; it also checks SipHash-2-4 against the paper's vector.
    const ShortIdCodec zero{uint256::ONE, 0};
    BOOST_CHECK_EQUAL(strprintf("%016x %016x", zero.K0(), zero.K1()), "9317465f708a9bfb cad130d242b2b766");
    BOOST_CHECK_EQUAL(HexShort(zero, CounterId(0)), "2b199b199964");
    BOOST_CHECK_EQUAL(HexShort(zero, CounterId(1)), "b02825991f08");
    BOOST_CHECK_EQUAL(HexShort(zero, CounterId(2)), "5dfef7c6a5ea");
    const ShortIdCodec seven{uint256::ONE, 7};
    BOOST_CHECK_EQUAL(strprintf("%016x %016x", seven.K0(), seven.K1()), "a25ef911ed4eefdf 3317ec2b187b5f6d");
    BOOST_CHECK_EQUAL(HexShort(seven, CounterId(0)), "5b567e692eb3");
    BOOST_CHECK_EQUAL(HexShort(seven, CounterId(1)), "4801891ef26b");
    BOOST_CHECK_EQUAL(HexShort(seven, CounterId(2)), "f674cc8944ac");
    BOOST_CHECK_EQUAL(HexShort(seven, ActionId{uint256::ONE}), "85fffe28717e");
    BOOST_CHECK(seven.HeaderHash() == uint256::ONE);
    BOOST_CHECK_EQUAL(seven.Nonce(), 7U);
}

BOOST_AUTO_TEST_CASE(resolve_lists_the_fetches_and_fill_completes_the_proposal)
{
    const ShortIdCodec codec{uint256::ONE, 7};
    const ActionId a{CounterId(COLLIDING_A)}, b{CounterId(COLLIDING_B)};
    const ActionId held_1{LabelId("held/1")}, held_2{LabelId("held/2")}, fresh{LabelId("fresh")};
    // Proposal: held_1, a (collides with held b), fresh (not held), held_2, held_1 again.
    const std::vector<ActionId> proposal{held_1, a, fresh, held_2, held_1};
    std::vector<agreement::ShortId> shorts;
    for (const ActionId& id : proposal) shorts.push_back(ShortOf(codec, id));
    const std::vector<ActionId> held{held_2, a, held_1, b, held_2};
    ShortIdResolution r{codec.Resolve(shorts, held)};
    BOOST_CHECK(r.shorts == shorts);
    BOOST_CHECK(r.ids[3] == held_2);
    BOOST_CHECK(r.Open() == (std::vector<uint32_t>{0, 1, 2, 4}));
    BOOST_CHECK(r.open[0] == ShortIdOpen::AMBIGUOUS); // the short ID repeats at 4
    BOOST_CHECK(r.open[1] == ShortIdOpen::AMBIGUOUS); // a and b
    BOOST_CHECK(r.open[2] == ShortIdOpen::MISSING);
    BOOST_CHECK(r.open[4] == ShortIdOpen::AMBIGUOUS);
    BOOST_CHECK(!r.Complete());
    BOOST_CHECK(!r.Ids().has_value());
    // Holding only a: position 1 resolves to it.
    const ShortIdResolution only_a{codec.Resolve(std::vector<agreement::ShortId>{ShortOf(codec, a)}, std::vector<ActionId>{a})};
    BOOST_CHECK(only_a.Complete() && only_a.ids[0] == a);

    // Refusals change nothing.
    const ShortIdResolution before{r};
    RequireFailure(codec.Fill(r, 5, fresh), ErrorCode::INVALID_INPUT, SHORT_ID_FILL_INDEX, "index past the end");
    RequireFailure(codec.Fill(r, 3, held_2), ErrorCode::INVALID_INPUT, SHORT_ID_FILL_RESOLVED, "resolved position");
    RequireFailure(codec.Fill(r, 2, held_1), ErrorCode::INVALID_INPUT, SHORT_ID_FILL_MISMATCH, "another action's ID");
    BOOST_CHECK(r.shorts == before.shorts && r.ids == before.ids && r.open == before.open);

    // b has a's short ID, so Fill takes it: which of the two the proposal
    // meant is decided by the certified full IDs (the header's actions_root),
    // not by this codec.
    ShortIdResolution wrong{r};
    RequireOk(codec.Fill(wrong, 1, b), "Fill with the other colliding action");
    BOOST_CHECK(wrong.ids[1] == b);

    // The fetched IDs close every open position in any order.
    RequireOk(codec.Fill(r, 2, fresh), "Fill the missing body");
    RequireOk(codec.Fill(r, 4, held_1), "Fill the repeat");
    RequireOk(codec.Fill(r, 1, a), "Fill the collision");
    BOOST_CHECK(r.Open() == std::vector<uint32_t>{0});
    RequireFailure(codec.Fill(r, 2, fresh), ErrorCode::INVALID_INPUT, SHORT_ID_FILL_RESOLVED, "a second fill");
    RequireOk(codec.Fill(r, 0, held_1), "Fill the first");
    BOOST_CHECK(r.Complete());
    BOOST_CHECK(r.Open().empty());
    BOOST_REQUIRE(r.Ids().has_value());
    BOOST_CHECK(*r.Ids() == proposal);
    // A malformed resolution is refused, not indexed.
    ShortIdResolution torn{r};
    torn.open.pop_back();
    RequireFailure(codec.Fill(torn, 0, held_1), ErrorCode::INVALID_INPUT, SHORT_ID_FILL_INDEX, "torn resolution");
}

BOOST_AUTO_TEST_CASE(proposals_of_held_actions_resolve_without_a_fetch)
{
    // Random proposals drawn from a held pool of 2,000 actions, with 0..3
    // fresh actions mixed in: every held position resolves to its action,
    // every fresh one is MISSING, and filling those gives the proposal back.
    const ShortIdCodec codec{LabelHash("fm2/test/short-id/header/prop"), 0x0123'4567'89ab'cdefULL};
    std::vector<ActionId> pool;
    for (uint64_t i{0}; i < 2'000; ++i) pool.push_back(LabelId("pool/" + std::to_string(i)));
    uint64_t state{0x5107};
    const auto next{[&state] {
        state = state * 6'364'136'223'846'793'005ULL + 1'442'695'040'888'963'407ULL;
        return state >> 33;
    }};
    size_t fetched{0}, resolved{0};
    for (int round{0}; round < 200; ++round) {
        std::vector<ActionId> proposal;
        std::vector<uint32_t> fresh_at;
        const uint64_t size{1 + next() % 64};
        std::vector<uint64_t> picked;
        while (proposal.size() < size) {
            if (next() % 32 == 0 && fresh_at.size() < 3) {
                fresh_at.push_back(static_cast<uint32_t>(proposal.size()));
                proposal.push_back(LabelId("fresh/" + std::to_string(round) + "/" + std::to_string(proposal.size())));
                continue;
            }
            const uint64_t k{next() % pool.size()};
            if (std::find(picked.begin(), picked.end(), k) != picked.end()) continue;
            picked.push_back(k);
            proposal.push_back(pool[k]);
        }
        std::vector<agreement::ShortId> shorts;
        for (const ActionId& id : proposal) shorts.push_back(ShortOf(codec, id));
        ShortIdResolution r{codec.Resolve(shorts, pool)};
        if (r.Open() != fresh_at) BOOST_FAIL("round " << round << ": the fetch list is not the fresh positions");
        for (uint32_t i : fresh_at) {
            if (r.open[i] != ShortIdOpen::MISSING) BOOST_FAIL("a fresh position is not MISSING");
            RequireOk(codec.Fill(r, i, proposal[i]), "Fill a fresh body");
            ++fetched;
        }
        resolved += proposal.size() - fresh_at.size();
        if (!r.Ids().has_value() || *r.Ids() != proposal) BOOST_FAIL("round " << round << ": the proposal did not come back");
    }
    BOOST_TEST_MESSAGE(resolved << " positions resolved from held actions, " << fetched << " fetched");
    BOOST_CHECK_GT(fetched, 100U);
    BOOST_CHECK_GT(resolved, 4'000U);
}

BOOST_AUTO_TEST_SUITE_END()
