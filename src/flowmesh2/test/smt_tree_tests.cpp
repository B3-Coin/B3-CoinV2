// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/codes.h>
#include <flowmesh2/core/smt.h>
#include <flowmesh2/ledger/smt_tree.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <uint256.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::ledger;
using namespace flowmesh2::test;

namespace {

uint256 PathOf(uint64_t i) { return SmtKeyPath(SmtComponent::ORDERS, LabelHash("fm2/test/smt-tree/" + std::to_string(i))); }
uint256 ValueOf(uint64_t i, uint64_t version) { return LabelHash("fm2/test/smt-tree/value/" + std::to_string(i) + "/" + std::to_string(version)); }

//! The leaves of a path-to-value map, in tree order.
std::vector<SmtLeaf> LeavesOf(const std::map<uint256, uint256>& model)
{
    std::vector<SmtLeaf> out;
    for (const auto& [path, value] : model) out.push_back(SmtLeaf{path, value});
    std::sort(out.begin(), out.end(), [](const SmtLeaf& a, const SmtLeaf& b) { return SmtPathLess(a.path, b.path); });
    return out;
}

//! The tree equals the reference construction of `model`: root, size, shape, lookups and proofs.
void RequireTreeOf(const SmtTree& tree, const std::map<uint256, uint256>& model, uint64_t probes)
{
    const std::vector<SmtLeaf> leaves{LeavesOf(model)};
    BOOST_REQUIRE_EQUAL(tree.Size(), model.size());
    if (tree.Root() != SmtRoot(leaves).value()) BOOST_FAIL("root differs at size " << model.size());
    BOOST_REQUIRE(tree.CheckShape());
    uint64_t probe{0};
    for (const SmtLeaf& leaf : leaves) {
        if (probe++ >= probes) break;
        if (tree.Find(leaf.path) != leaf.value) BOOST_FAIL("lookup differs");
        const SmtProof proof{tree.Prove(leaf.path)};
        if (proof != SmtProve(leaves, leaf.path).value()) BOOST_FAIL("member proof differs");
        if (!SmtVerifyMember(tree.Root(), leaf.path, leaf.value, proof)) BOOST_FAIL("member proof does not verify");
    }
    for (uint64_t i{0}; i < std::min<uint64_t>(probes, 4); ++i) {
        const uint256 absent{LabelHash("fm2/test/smt-tree/absent/" + std::to_string(i))};
        if (tree.Find(absent).has_value()) BOOST_FAIL("an absent path was found");
        const SmtProof proof{tree.Prove(absent)};
        if (proof != SmtProve(leaves, absent).value()) BOOST_FAIL("absence proof differs");
        if (!SmtVerifyAbsent(tree.Root(), absent, proof)) BOOST_FAIL("absence proof does not verify");
    }
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

BOOST_AUTO_TEST_SUITE(smt_tree_tests)

BOOST_AUTO_TEST_CASE(built_trees_are_the_reference_tree_for_every_size_to_100)
{
    std::map<uint256, uint256> model;
    for (uint64_t n{0}; n <= 100; ++n) {
        RequireTreeOf(SmtTree::Build(LeavesOf(model)).value(), model, n <= 16 ? n : 4);
        model.emplace(PathOf(n), ValueOf(n, 0));
    }
    std::vector<SmtLeaf> unordered{LeavesOf(model)};
    std::swap(unordered[3], unordered[4]);
    RequireError(SmtTree::Build(unordered), ErrorCode::INVALID_INPUT, "unordered leaves");
}

BOOST_AUTO_TEST_CASE(random_sets_and_erases_give_the_reference_tree_and_copies_keep_theirs)
{
    // Inserts, value changes, same-value writes and erases (some of absent
    // paths), with a copy kept every 40 operations and checked at the end.
    Rng rng{0x5e7};
    SmtTree tree;
    std::map<uint256, uint256> model;
    std::vector<std::pair<SmtTree, std::map<uint256, uint256>>> copies;
    for (uint64_t op{1}; op <= 1'000; ++op) {
        const uint64_t key{rng.Below(300)};
        const uint64_t kind{rng.Below(10)};
        if (kind < 6) {
            tree.Set(PathOf(key), ValueOf(key, op));
            model[PathOf(key)] = ValueOf(key, op);
        } else if (kind < 7) {
            const auto it{model.find(PathOf(key))};
            if (it != model.end()) {
                const SmtTree before{tree};
                tree.Set(PathOf(key), it->second);
                BOOST_REQUIRE(tree.Root() == before.Root());
            }
        } else {
            const bool had{model.erase(PathOf(key)) == 1};
            BOOST_REQUIRE_EQUAL(tree.Erase(PathOf(key)), had);
        }
        if (op % 40 == 0) {
            RequireTreeOf(tree, model, 3);
            copies.emplace_back(tree, model);
        }
    }
    for (const auto& [copy, copy_model] : copies) RequireTreeOf(copy, copy_model, 2);
    // Erase everything: the empty tree.
    for (const auto& [path, value] : std::map<uint256, uint256>{model}) BOOST_REQUIRE(tree.Erase(path));
    BOOST_CHECK(tree.Root() == SMT_EMPTY);
    BOOST_CHECK_EQUAL(tree.Size(), 0U);
    BOOST_CHECK(!tree.Erase(PathOf(0)));
}

BOOST_AUTO_TEST_CASE(paths_sharing_long_prefixes_build_the_reference_tree)
{
    // Crafted paths that differ only in the last bits force inner nodes with
    // one empty side all the way down, and the collapse on erase.
    std::map<uint256, uint256> model;
    SmtTree tree;
    for (unsigned last : {0x00u, 0x01u, 0x02u, 0x80u, 0xffu}) {
        uint256 path{PathOf(7)};
        path.data()[31] = static_cast<unsigned char>(last);
        tree.Set(path, ValueOf(last, 1));
        model[path] = ValueOf(last, 1);
        RequireTreeOf(tree, model, 8);
    }
    for (const auto& [path, value] : std::map<uint256, uint256>{model}) {
        BOOST_REQUIRE(tree.Erase(path));
        model.erase(path);
        RequireTreeOf(tree, model, 8);
    }
}

BOOST_AUTO_TEST_CASE(a_member_proof_over_n_hashed_paths_is_about_log2_n_siblings)
{
    // The plan's "about 768 B" balance proof is 3 + 32 s + 65 bytes for s
    // siblings, s about log2(n) + 1.33 for hashed paths. Here n = 2^11; the
    // bench in the evidence measures 2^21.
    const uint64_t n{2'048};
    std::vector<SmtLeaf> leaves;
    for (uint64_t i{0}; i < n; ++i) leaves.push_back(SmtLeaf{PathOf(i), ValueOf(i, 0)});
    std::sort(leaves.begin(), leaves.end(), [](const SmtLeaf& a, const SmtLeaf& b) { return SmtPathLess(a.path, b.path); });
    const SmtTree tree{SmtTree::Build(leaves).value()};
    double bytes{0};
    size_t largest{0};
    for (uint64_t i{0}; i < n; ++i) {
        const size_t size{tree.Prove(leaves[i].path).Encode().size()};
        bytes += static_cast<double>(size);
        largest = std::max(largest, size);
    }
    const double mean{bytes / static_cast<double>(n)};
    BOOST_TEST_MESSAGE("member proof over 2^11 leaves: mean " << mean << " B, largest " << largest << " B");
    BOOST_CHECK_LT(mean, 3 + 32 * (11 + 3) + 65);
    BOOST_CHECK_GT(mean, 3 + 32 * (11 - 1) + 65);
}

BOOST_AUTO_TEST_CASE(copies_are_changed_and_read_on_several_threads_at_once)
{
    // Each thread changes its own copy of one shared tree; the shared nodes
    // are only read. TSan reports any write to a shared node.
    std::map<uint256, uint256> model;
    for (uint64_t i{0}; i < 256; ++i) model.emplace(PathOf(i), ValueOf(i, 0));
    const SmtTree base{SmtTree::Build(LeavesOf(model)).value()};
    const uint256 base_root{base.Root()};
    std::vector<uint256> roots(4);
    std::vector<std::thread> threads;
    for (uint64_t t{0}; t < roots.size(); ++t) {
        threads.emplace_back([&base, &roots, t] {
            SmtTree mine{base};
            for (uint64_t i{0}; i < 64; ++i) mine.Set(PathOf(i * 4 + t), ValueOf(i, t + 1));
            for (uint64_t i{0}; i < 16; ++i) mine.Erase(PathOf(i * 4 + (t + 1) % 4));
            roots[t] = mine.Root();
        });
    }
    for (std::thread& thread : threads) thread.join();
    BOOST_CHECK(base.Root() == base_root);
    for (uint64_t t{0}; t < roots.size(); ++t) {
        std::map<uint256, uint256> expected{model};
        for (uint64_t i{0}; i < 64; ++i) expected[PathOf(i * 4 + t)] = ValueOf(i, t + 1);
        for (uint64_t i{0}; i < 16; ++i) expected.erase(PathOf(i * 4 + (t + 1) % 4));
        BOOST_CHECK(roots[t] == SmtRoot(LeavesOf(expected)).value());
    }
}

BOOST_AUTO_TEST_SUITE_END()
