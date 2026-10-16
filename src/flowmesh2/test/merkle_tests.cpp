// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <consensus/merkle.h>
#include <crypto/hex_base.h>
#include <crypto/sha256.h>
#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/merkle.h>

#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <vector>

using namespace flowmesh2;

namespace {

//! Leaf value i = SHA256(byte i), as in the Python golden script.
std::vector<uint256> Leaves(size_t n)
{
    std::vector<uint256> out(n);
    for (size_t i{0}; i < n; ++i) {
        const unsigned char byte{static_cast<unsigned char>(i)};
        CSHA256{}.Write(&byte, 1).Finalize(out[i].data());
    }
    return out;
}

// ---- Naive reference, written straight from the RFC 6962 recursion. ----

uint256 RefLeaf(const uint256& d) { return Tagged(HashTag::MERKLE_LEAF, std::span{d.data(), d.size()}); }

uint256 RefNode(const uint256& l, const uint256& r)
{
    std::vector<unsigned char> both(l.begin(), l.end());
    both.insert(both.end(), r.begin(), r.end());
    return Tagged(HashTag::MERKLE_NODE, both);
}

size_t SplitPoint(size_t n)
{
    size_t k{1};
    while (k * 2 < n) k *= 2;
    return k;
}

uint256 RefTreeHash(std::vector<uint256> d)
{
    if (d.empty()) return Tagged(HashTag::MERKLE_EMPTY, {});
    if (d.size() == 1) return RefLeaf(d[0]);
    const size_t k{SplitPoint(d.size())};
    return RefNode(RefTreeHash({d.begin(), d.begin() + k}), RefTreeHash({d.begin() + k, d.end()}));
}

uint256 RefRoot(const std::vector<uint256>& d)
{
    std::vector<unsigned char> bytes(8);
    uint64_t n{d.size()};
    for (size_t i{0}; i < 8; ++i) bytes[i] = static_cast<unsigned char>(n >> (8 * i));
    const uint256 mth{RefTreeHash(d)};
    bytes.insert(bytes.end(), mth.begin(), mth.end());
    return Tagged(HashTag::MERKLE_ROOT, bytes);
}

//! RFC 6962 PATH(m, D[n]).
std::vector<uint256> RefPath(size_t m, std::vector<uint256> d)
{
    if (d.size() <= 1) return {};
    const size_t k{SplitPoint(d.size())};
    std::vector<uint256> path;
    if (m < k) {
        path = RefPath(m, {d.begin(), d.begin() + k});
        path.push_back(RefTreeHash({d.begin() + k, d.end()}));
    } else {
        path = RefPath(m - k, {d.begin() + k, d.end()});
        path.push_back(RefTreeHash({d.begin(), d.begin() + k}));
    }
    return path;
}

MerkleProof Prove(const std::vector<uint256>& leaves, uint64_t index)
{
    auto proof{MerkleProve(leaves, index)};
    BOOST_REQUIRE(proof.has_value());
    return *proof;
}

} // namespace

BOOST_AUTO_TEST_SUITE(merkle_tests)

BOOST_AUTO_TEST_CASE(roots_match_reference_sizes_0_to_17)
{
    std::set<uint256> roots;
    for (size_t n{0}; n <= 17; ++n) {
        const auto leaves{Leaves(n)};
        const uint256 root{MerkleRoot(leaves)};
        BOOST_CHECK_MESSAGE(root == RefRoot(leaves), "size " << n);
        BOOST_CHECK(roots.insert(root).second);
    }
}

BOOST_AUTO_TEST_CASE(roots_golden)
{
    // Computed independently with Python hashlib (scratch golden.py).
    const struct {
        size_t size;
        const char* root;
    } goldens[]{
        {0, "c1ca51c946def6db4b8c28b3783d789630a7096fe5d199c1fa76a772b005832d"},
        {1, "5eeef9ba9714c3bace76daf914f513bc3c9989be6b7e6fc99cda05665037ac31"},
        {2, "c2b101b5bcede72f6a12c651abfef28ba1da6b765cb782483d833cd4cd08761a"},
        {3, "97cfb4dd91cdf06e3f527956e16c232b5f79b32a87a673d0d5ab3db123d0a1be"},
        {5, "184e5432dd2fcc10e1bfc7f06eccc0715c61e011a0c2bac4083a832c71dd3b42"},
        {17, "6599987885f5324cc9f52d487c3dd30c629ef588eb09ceb748e422b36a9b7466"},
    };
    for (const auto& g : goldens) BOOST_CHECK_EQUAL(HexStr(MerkleRoot(Leaves(g.size))), g.root);
}

BOOST_AUTO_TEST_CASE(every_proof_verifies_and_equals_the_reference_path)
{
    for (size_t n{1}; n <= 17; ++n) {
        const auto leaves{Leaves(n)};
        const uint256 root{MerkleRoot(leaves)};
        for (size_t i{0}; i < n; ++i) {
            const MerkleProof proof{Prove(leaves, i)};
            BOOST_CHECK_EQUAL(proof.index, i);
            BOOST_CHECK_EQUAL(proof.size, n);
            BOOST_CHECK(proof.path == RefPath(i, leaves));
            BOOST_CHECK_MESSAGE(MerkleVerify(leaves[i], proof, root), "size " << n << " index " << i);
        }
    }
}

BOOST_AUTO_TEST_CASE(tampered_proofs_fail)
{
    size_t rejected{0};
    for (size_t n{1}; n <= 17; ++n) {
        const auto leaves{Leaves(n)};
        const uint256 root{MerkleRoot(leaves)};
        for (size_t i{0}; i < n; ++i) {
            const MerkleProof good{Prove(leaves, i)};
            // Each path entry with one bit flipped.
            for (size_t j{0}; j < good.path.size(); ++j) {
                MerkleProof bad{good};
                bad.path[j].data()[j % 32] ^= 0x01;
                BOOST_CHECK(!MerkleVerify(leaves[i], bad, root));
                ++rejected;
            }
            // Every other index, including indexes outside the tree.
            for (size_t k{0}; k < n + 3; ++k) {
                if (k == i) continue;
                MerkleProof bad{good};
                bad.index = k;
                BOOST_CHECK_MESSAGE(!MerkleVerify(leaves[i], bad, root), "size " << n << " index " << i << " as " << k);
                ++rejected;
            }
            // Every other size from 0 to 20.
            for (size_t s{0}; s <= 20; ++s) {
                if (s == n) continue;
                MerkleProof bad{good};
                bad.size = s;
                BOOST_CHECK_MESSAGE(!MerkleVerify(leaves[i], bad, root), "size " << n << " index " << i << " as size " << s);
                ++rejected;
            }
            // Wrong leaf, wrong root, an extra or a missing path entry.
            uint256 other_leaf{leaves[i]};
            other_leaf.data()[0] ^= 0x80;
            BOOST_CHECK(!MerkleVerify(other_leaf, good, root));
            uint256 other_root{root};
            other_root.data()[31] ^= 0x01;
            BOOST_CHECK(!MerkleVerify(leaves[i], good, other_root));
            MerkleProof longer{good};
            longer.path.push_back(root);
            BOOST_CHECK(!MerkleVerify(leaves[i], longer, root));
            if (!good.path.empty()) {
                MerkleProof shorter{good};
                shorter.path.pop_back();
                BOOST_CHECK(!MerkleVerify(leaves[i], shorter, root));
            }
            rejected += 4;
        }
    }
    BOOST_TEST_MESSAGE("tampered proofs rejected: " << rejected);
}

BOOST_AUTO_TEST_CASE(duplicated_last_leaf_changes_the_root)
{
    for (size_t n{1}; n <= 16; ++n) {
        auto leaves{Leaves(n)};
        const uint256 root{MerkleRoot(leaves)};
        leaves.push_back(leaves.back());
        BOOST_CHECK_MESSAGE(MerkleRoot(leaves) != root, "size " << n);
    }
    // Contrast: Bitcoin's consensus Merkle root has the CVE-2012-2459 property.
    const auto three{Leaves(3)};
    auto four{three};
    four.push_back(three.back());
    bool mutated{false};
    BOOST_CHECK(ComputeMerkleRoot(three) == ComputeMerkleRoot(four, &mutated));
    BOOST_CHECK(mutated);
    BOOST_CHECK(MerkleRoot(three) != MerkleRoot(four));
}

BOOST_AUTO_TEST_CASE(prove_refuses_an_index_outside_the_tree)
{
    for (size_t n : {size_t{0}, size_t{1}, size_t{5}}) {
        const auto proof{MerkleProve(Leaves(n), n)};
        BOOST_REQUIRE(!proof.has_value());
        BOOST_CHECK_EQUAL(ToString(proof.error().code), "INVALID_INPUT");
    }
    // An empty tree has no leaf to prove, and no proof verifies against it.
    MerkleProof empty;
    BOOST_CHECK(!MerkleVerify(uint256{}, empty, MerkleRoot({})));
}

BOOST_AUTO_TEST_CASE(proof_codec_round_trip_and_cap)
{
    const auto leaves{Leaves(13)};
    const MerkleProof proof{Prove(leaves, 6)};
    const auto bytes{EncodeBytes(proof)};
    BOOST_CHECK_EQUAL(bytes.size(), 8U + 8U + 1U + 32U * proof.path.size());
    const auto decoded{DecodeExact<MerkleProof>(bytes, bytes.size())};
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK(*decoded == proof);

    MerkleProof at_cap;
    at_cap.size = 1;
    at_cap.path.resize(MAX_MERKLE_PATH);
    const auto cap_bytes{EncodeBytes(at_cap)};
    BOOST_CHECK(DecodeExact<MerkleProof>(cap_bytes, cap_bytes.size()).has_value());
    BOOST_CHECK(!MerkleVerify(uint256{}, at_cap, MerkleRoot(Leaves(1))));

    MerkleProof over_cap{at_cap};
    over_cap.path.resize(MAX_MERKLE_PATH + 1);
    BOOST_CHECK_THROW(EncodeBytes(over_cap), CapExceeded);
    BOOST_CHECK(!MerkleVerify(uint256{}, over_cap, MerkleRoot(Leaves(1))));
    // A hand-made encoding with 65 entries is refused as OVERSIZE.
    std::vector<unsigned char> raw(16, 0x00);
    raw.push_back(static_cast<unsigned char>(MAX_MERKLE_PATH + 1));
    raw.resize(raw.size() + 32 * (MAX_MERKLE_PATH + 1), 0x00);
    const auto refused{DecodeExact<MerkleProof>(raw, raw.size())};
    BOOST_REQUIRE(!refused.has_value());
    BOOST_CHECK_EQUAL(ToString(refused.error().code), "OVERSIZE");
}

BOOST_AUTO_TEST_SUITE_END()
