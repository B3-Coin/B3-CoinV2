// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/codes.h>
#include <flowmesh2/core/merkle.h>
#include <flowmesh2/ledger/merkle_levels.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <uint256.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::ledger;
using namespace flowmesh2::test;

namespace {

uint256 Value(uint64_t n) { return LabelHash("fm2/test/merkle-levels/" + std::to_string(n)); }

std::vector<uint256> Values(uint64_t count, uint64_t salt = 0)
{
    std::vector<uint256> out;
    for (uint64_t i{0}; i < count; ++i) out.push_back(Value(i + salt * 1'000'000));
    return out;
}

//! Root and every proof agree with MerkleRoot and MerkleProve of `values`.
void RequireTree(const MerkleLevels& levels, const std::vector<uint256>& values, bool every_proof)
{
    BOOST_REQUIRE_EQUAL(levels.Size(), values.size());
    const uint256 root{MerkleRoot(values)};
    if (levels.Root() != root) BOOST_FAIL("root differs at size " << values.size());
    for (uint64_t i{0}; i < values.size(); i += every_proof ? 1 : 1 + values.size() / 7) {
        const Result<MerkleProof> mine{levels.Prove(i)};
        const Result<MerkleProof> reference{MerkleProve(values, i)};
        if (!mine.has_value() || *mine != *reference) BOOST_FAIL("proof " << i << " of " << values.size() << " differs");
        if (!MerkleVerify(values[i], *mine, root)) BOOST_FAIL("proof " << i << " does not verify");
    }
    BOOST_REQUIRE(!levels.Prove(values.size()).has_value());
}

//! splitmix64.
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

} // namespace

BOOST_AUTO_TEST_SUITE(merkle_levels_tests)

BOOST_AUTO_TEST_CASE(built_levels_give_merkle_root_and_merkle_prove_for_every_size_to_300)
{
    // Sizes 0..300 cross the chunk boundary (128) twice and cover every
    // promotion of an odd last node on the way. Every proof is checked up to
    // 40 leaves and around the chunk edges, a sample elsewhere.
    for (uint64_t size{0}; size <= 300; ++size) {
        const bool every_proof{size <= 40 || (size >= 126 && size <= 130) || (size >= 254 && size <= 258)};
        RequireTree(MerkleLevels::Build(Values(size)), Values(size), every_proof);
    }
    RequireTree(MerkleLevels{}, {}, true);
}

BOOST_AUTO_TEST_CASE(random_edits_give_the_tree_of_the_edited_sequence)
{
    // Each round applies a sorted batch of updates and inserts and compares
    // the result with a tree built from scratch. The old levels are checked
    // again afterwards: Apply never writes to a shared chunk.
    Rng rng{0x1e7e15};
    std::vector<uint256> values{Values(200)};
    MerkleLevels levels{MerkleLevels::Build(values)};
    uint64_t salt{1};
    for (int round{0}; round < 60; ++round) {
        const std::vector<uint256> before_values{values};
        const MerkleLevels before{levels};
        // Choose the edits on the edited sequence, ascending by position.
        std::vector<MerkleLevels::Edit> edits;
        const uint64_t count{rng.Below(round % 4 == 0 ? 40 : 6) + 1};
        uint64_t position{rng.Below(values.size() / 2 + 1)};
        std::vector<uint256> next;
        next.reserve(values.size() + count);
        uint64_t read{0};
        for (uint64_t e{0}; e < count; ++e) {
            if (e > 0) position += 1 + rng.Below(round % 5 == 0 ? 3 : 60);
            // Copy the old values that come before `position` in the new sequence.
            while (next.size() < position && read < values.size()) next.push_back(values[read++]);
            if (next.size() < position) break; // past the end: stop the batch
            const bool insert{read >= values.size() || rng.Below(2) == 0};
            const uint256 value{Value(salt++ * 7'919)};
            edits.push_back({position, value, insert});
            next.push_back(value);
            if (!insert) ++read;
        }
        while (read < values.size()) next.push_back(values[read++]);
        const Result<MerkleLevels> applied{levels.Apply(edits)};
        BOOST_TEST_CONTEXT("round " << round << " edits " << edits.size() << " size " << next.size())
        {
            BOOST_REQUIRE(applied.has_value());
            RequireTree(*applied, next, false);
            RequireTree(before, before_values, false);
        }
        values = std::move(next);
        levels = *applied;
    }
    BOOST_CHECK_GT(values.size(), 300U);
}

BOOST_AUTO_TEST_CASE(appends_and_front_inserts_rebuild_only_what_they_must)
{
    // An append to every size 0..260, and an insert at the front of every
    // size 0..140: the two ends of the insert cost range.
    std::vector<uint256> values;
    MerkleLevels levels;
    for (uint64_t i{0}; i < 260; ++i) {
        const MerkleLevels::Edit edit{values.size(), Value(i), true};
        levels = levels.Apply({&edit, 1}).value();
        values.push_back(Value(i));
        if (levels.Root() != MerkleRoot(values)) BOOST_FAIL("append " << i);
    }
    values.clear();
    levels = MerkleLevels{};
    for (uint64_t i{0}; i < 140; ++i) {
        const MerkleLevels::Edit edit{0, Value(i), true};
        levels = levels.Apply({&edit, 1}).value();
        values.insert(values.begin(), Value(i));
        if (levels.Root() != MerkleRoot(values)) BOOST_FAIL("front insert " << i);
    }
    RequireTree(levels, values, true);
}

BOOST_AUTO_TEST_CASE(unordered_duplicate_and_out_of_range_edits_are_refused)
{
    const MerkleLevels levels{MerkleLevels::Build(Values(10))};
    const std::vector<MerkleLevels::Edit> descending{{5, Value(1), false}, {3, Value(2), false}};
    const std::vector<MerkleLevels::Edit> duplicate{{4, Value(1), false}, {4, Value(2), true}};
    const std::vector<MerkleLevels::Edit> past_update{{10, Value(1), false}};
    const std::vector<MerkleLevels::Edit> past_insert{{11, Value(1), true}};
    RequireError(levels.Apply(descending), ErrorCode::INVALID_INPUT, "descending positions");
    RequireError(levels.Apply(duplicate), ErrorCode::INVALID_INPUT, "a repeated position");
    RequireError(levels.Apply(past_update), ErrorCode::INVALID_INPUT, "an update past the end");
    RequireError(levels.Apply(past_insert), ErrorCode::INVALID_INPUT, "an insert past the end");
    // The last valid positions: an update of the last value and an append.
    const std::vector<MerkleLevels::Edit> last{{9, Value(1), false}, {10, Value(2), true}};
    std::vector<uint256> values{Values(10)};
    values[9] = Value(1);
    values.push_back(Value(2));
    RequireTree(levels.Apply(last).value(), values, true);
    RequireTree(levels.Apply({}).value(), Values(10), true);
}

BOOST_AUTO_TEST_SUITE_END()
