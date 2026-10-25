// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/codes.h>
#include <flowmesh2/core/smt.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;

namespace {

uint256 KeyOf(uint64_t i) { return LabelHash("fm2/test/smt/key/" + std::to_string(i)); }

SmtLeaf LeafOf(uint64_t i)
{
    const uint256 key{KeyOf(i)};
    return SmtLeaf{SmtKeyPath(SmtComponent::ACCOUNTS, std::span<const unsigned char>{key.data(), 32}),
                   LabelHash("fm2/test/smt/value/" + std::to_string(i))};
}

//! Leaves 0..n-1 in tree order.
std::vector<SmtLeaf> Leaves(uint64_t n, uint64_t first = 0)
{
    std::vector<SmtLeaf> out;
    for (uint64_t i{first}; i < first + n; ++i) out.push_back(LeafOf(i));
    std::sort(out.begin(), out.end(), [](const SmtLeaf& a, const SmtLeaf& b) { return SmtPathLess(a.path, b.path); });
    return out;
}

uint256 AbsentPath(uint64_t i) { return LabelHash("fm2/test/smt/absent/" + std::to_string(i)); }

std::string Hex(const uint256& value) { return HexStr(value); }

} // namespace

BOOST_AUTO_TEST_SUITE(smt_tests)

BOOST_AUTO_TEST_CASE(roots_and_proofs_match_the_python_reference)
{
    // Goldens from an independent Python model of the format in core/smt.h
    // (evidence M2/M2-LED-02/smt_ref.py), hex in byte order.
    const uint256 first_key{KeyOf(0)};
    BOOST_CHECK_EQUAL(Hex(SmtKeyPath(SmtComponent::ACCOUNTS, std::span<const unsigned char>{first_key.data(), 32})),
                      "0e7fc42251de553a6a2bed7cd345f989ac9305e9468c9c4bd86beee66a2ba939");
    const std::vector<std::pair<uint64_t, std::string>> roots{
        {0, "0000000000000000000000000000000000000000000000000000000000000000"},
        {1, "787d69ff61fb486f1a6bbe3ed5a68a9b7cceb9296e21e890a3e1496e52d1fbdd"},
        {2, "2f651a871e4936a74c6bf78e5e454436129d550929caf70e186e7fc493562a8e"},
        {5, "580846417942baf48c05d0d19a7fdcde1a4ccc5a230319c43fda45d142a5bf1c"},
        {64, "fecf61a838189d2d05569739da8021ab079fbadf9c8cfcf5b0e189c4532d2e95"},
    };
    for (const auto& [n, hex] : roots) BOOST_CHECK_EQUAL(Hex(SmtRoot(Leaves(n)).value()), hex);

    // The member proof of key 3 and an absence proof in the tree of five.
    std::vector<SmtLeaf> five{Leaves(5)};
    const SmtLeaf three{LeafOf(3)};
    BOOST_CHECK_EQUAL(HexStr(SmtProve(five, three.path).value().Encode()),
                      "0300a01780a91cb75bf46c88b32f7b69ec30c002c620d6ba737c4c77c359f564bc357a1779144f2a1508823b0661dc0e"
                      "471b0d478805e9fb63f1e18ad1fd335087d3e502aa3dffd9321edc7302876aadf551fdc610842c1ac8b51851b2ec6881"
                      "cb758f5aa7f9a0e2e5d8a697a8bae1278280ed4e949cc65a652d1f947ba31f7676e32b05");
    const uint256 absent{SmtKeyPath(SmtComponent::ACCOUNTS, LabelHash("fm2/test/smt/absent"))};
    BOOST_CHECK_EQUAL(HexStr(SmtProve(five, absent).value().Encode()),
                      "0200c01780a91cb75bf46c88b32f7b69ec30c002c620d6ba737c4c77c359f564bc357ac19955eb2f1a5c6c5e2559"
                      "46c63395b67ad9f4a9194476b4fbbf9e101ead9a1201");
}

BOOST_AUTO_TEST_CASE(every_member_and_absent_path_verifies_and_nothing_else_does)
{
    for (uint64_t n{0}; n <= 40; ++n) {
        const std::vector<SmtLeaf> leaves{Leaves(n)};
        const uint256 root{SmtRoot(leaves).value()};
        BOOST_TEST_CONTEXT("size " << n)
        {
            for (const SmtLeaf& leaf : leaves) {
                const SmtProof proof{SmtProve(leaves, leaf.path).value()};
                BOOST_REQUIRE(SmtVerifyMember(root, leaf.path, leaf.value, proof));
                BOOST_REQUIRE(!SmtVerifyAbsent(root, leaf.path, proof));
                BOOST_REQUIRE(!SmtVerifyMember(root, leaf.path, AbsentPath(0), proof));
                BOOST_REQUIRE(SmtProof::Decode(proof.Encode()).value() == proof);
            }
            for (uint64_t i{0}; i < 8; ++i) {
                const SmtProof proof{SmtProve(leaves, AbsentPath(i)).value()};
                BOOST_REQUIRE(SmtVerifyAbsent(root, AbsentPath(i), proof));
                BOOST_REQUIRE(!SmtVerifyMember(root, AbsentPath(i), proof.leaf.value, proof));
                BOOST_REQUIRE(SmtProof::Decode(proof.Encode()).value() == proof);
                // The proof of one absent path says nothing about a path on another branch.
                if (n > 1 && SmtCommonPrefix(AbsentPath(i), AbsentPath(i + 1)) < proof.depth) {
                    BOOST_REQUIRE(!SmtVerifyAbsent(root, AbsentPath(i + 1), proof));
                }
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(a_member_cannot_be_shown_absent_by_any_one_byte_change)
{
    // Every one-byte change of an encoded proof either fails to decode or
    // fails to verify: membership of the right value only, absence never.
    const std::vector<SmtLeaf> leaves{Leaves(24)};
    const uint256 root{SmtRoot(leaves).value()};
    const SmtLeaf target{leaves[9]};
    const std::vector<unsigned char> bytes{SmtProve(leaves, target.path).value().Encode()};
    size_t decoded{0};
    for (size_t i{0}; i < bytes.size(); ++i) {
        for (unsigned flip : {0x01u, 0x80u, 0xffu}) {
            std::vector<unsigned char> changed{bytes};
            changed[i] ^= static_cast<unsigned char>(flip);
            const Result<SmtProof> proof{SmtProof::Decode(changed)};
            if (!proof.has_value()) continue;
            ++decoded;
            BOOST_REQUIRE(!SmtVerifyAbsent(root, target.path, *proof));
            BOOST_REQUIRE(!SmtVerifyMember(root, target.path, target.value, *proof));
        }
    }
    BOOST_CHECK_GT(decoded, 0U);
    // The same for the absence proof of a path no leaf has.
    const std::vector<unsigned char> absent{SmtProve(leaves, AbsentPath(3)).value().Encode()};
    for (size_t i{0}; i < absent.size(); ++i) {
        std::vector<unsigned char> changed{absent};
        changed[i] ^= 0x01;
        const Result<SmtProof> proof{SmtProof::Decode(changed)};
        if (proof.has_value()) BOOST_REQUIRE(!SmtVerifyAbsent(root, AbsentPath(3), *proof));
    }
}

BOOST_AUTO_TEST_CASE(structural_forgeries_fail)
{
    const std::vector<SmtLeaf> leaves{Leaves(16)};
    const uint256 root{SmtRoot(leaves).value()};
    const SmtLeaf target{leaves[5]};
    const SmtProof good{SmtProve(leaves, target.path).value()};
    BOOST_REQUIRE(SmtVerifyMember(root, target.path, target.value, good));

    // A member's leaf passed off as the terminal of a path it does not share `depth` bits with.
    const SmtProof other{SmtProve(leaves, leaves[12].path).value()};
    uint256 wrong_branch{target.path};
    wrong_branch.data()[0] ^= 0x80;
    BOOST_CHECK(!SmtVerifyAbsent(root, wrong_branch, good));
    BOOST_CHECK(!SmtVerifyAbsent(root, target.path, other));

    // Absence claimed with an EMPTY terminal where the member's leaf sits.
    SmtProof emptied{good};
    emptied.terminal = SmtTerminal::EMPTY;
    emptied.leaf = SmtLeaf{};
    BOOST_CHECK(!SmtVerifyAbsent(root, target.path, emptied));

    // One level shorter or longer, a dropped or added sibling.
    SmtProof shorter{good};
    shorter.depth -= 1;
    shorter.bitmap.resize((shorter.depth + 7) / 8);
    BOOST_CHECK(!SmtVerifyMember(root, target.path, target.value, shorter));
    SmtProof longer{good};
    longer.depth += 1;
    longer.bitmap.resize((longer.depth + 7) / 8, 0);
    BOOST_CHECK(!SmtVerifyMember(root, target.path, target.value, longer));
    SmtProof dropped{good};
    dropped.siblings.pop_back();
    BOOST_CHECK(!SmtVerifyMember(root, target.path, target.value, dropped));
    BOOST_CHECK(!SmtFoldRoot(target.path, dropped).has_value());

    // Another root, another value, another path.
    BOOST_CHECK(!SmtVerifyMember(SmtRoot(Leaves(15)).value(), target.path, target.value, good));
    BOOST_CHECK(!SmtVerifyMember(root, target.path, leaves[6].value, good));
    BOOST_CHECK(!SmtVerifyMember(root, leaves[6].path, target.value, good));
}

BOOST_AUTO_TEST_CASE(the_codec_refuses_every_noncanonical_proof)
{
    const std::vector<SmtLeaf> leaves{Leaves(16)};
    const SmtProof good{SmtProve(leaves, leaves[2].path).value()};
    BOOST_REQUIRE_GT(good.depth, 1U);
    const auto refuses = [](const SmtProof& proof, const char* what) {
        BOOST_TEST_CONTEXT(what) { RequireError(SmtProof::Decode(proof.Encode()), ErrorCode::DECODE, what); }
    };
    SmtProof deep{good};
    deep.depth = SMT_DEPTH + 1;
    deep.bitmap.assign(33, 0);
    deep.bitmap[31] = 0x01;
    deep.siblings.assign(1, LabelHash("x"));
    refuses(deep, "depth above 256");
    SmtProof high_bit{good};
    high_bit.depth = 3;
    high_bit.bitmap = {0x30};
    high_bit.siblings.assign(2, LabelHash("x"));
    refuses(high_bit, "a bitmap bit at or above depth");
    SmtProof no_last{good};
    no_last.depth = 3;
    no_last.bitmap = {0x80};
    no_last.siblings.assign(1, LabelHash("x"));
    refuses(no_last, "no sibling at depth - 1");
    SmtProof zero{good};
    zero.siblings.back() = SMT_EMPTY;
    refuses(zero, "an empty sibling listed");
    SmtProof unknown{good};
    unknown.terminal = static_cast<SmtTerminal>(3);
    refuses(unknown, "an unknown terminal");
    std::vector<unsigned char> trailing{good.Encode()};
    trailing.push_back(0);
    RequireError(SmtProof::Decode(trailing), ErrorCode::DECODE, "trailing bytes");
    std::vector<unsigned char> truncated{good.Encode()};
    truncated.pop_back();
    RequireError(SmtProof::Decode(truncated), ErrorCode::DECODE, "truncated");
    RequireError(SmtProof::Decode(std::vector<unsigned char>(SmtProof::MAX_ENCODED_BYTES + 1, 0)), ErrorCode::OVERSIZE,
                 "oversize");
    // The empty tree's proof: depth 0, EMPTY.
    const SmtProof empty{SmtProve({}, leaves[0].path).value()};
    BOOST_CHECK_EQUAL(HexStr(empty.Encode()), "000001");
    BOOST_CHECK(SmtVerifyAbsent(SMT_EMPTY, leaves[0].path, empty));
}

BOOST_AUTO_TEST_CASE(unordered_leaves_are_refused)
{
    std::vector<SmtLeaf> leaves{Leaves(4)};
    std::swap(leaves[1], leaves[2]);
    RequireError(SmtRoot(leaves), ErrorCode::INVALID_INPUT, "unordered");
    RequireError(SmtProve(leaves, leaves[0].path), ErrorCode::INVALID_INPUT, "unordered");
    std::vector<SmtLeaf> repeated{Leaves(3)};
    repeated[2] = repeated[1];
    RequireError(SmtRoot(repeated), ErrorCode::INVALID_INPUT, "repeated");
}

BOOST_AUTO_TEST_SUITE_END()
