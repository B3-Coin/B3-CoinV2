// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Bridge stage 2 (ETH -> B3 mint leg): strict RLP decoding and
// Merkle-Patricia receipts inclusion proofs. The positive vectors are a
// captured Ethereum mainnet fixture whose receipts trie was rebuilt in full
// by an independent generator and matched the block header's receiptsRoot
// (src/test/data/eth_receipts_proof_fixture.h).

#include <bridge/mpt.h>
#include <bridge/rlp.h>
#include <crypto/keccak256.h>
#include <test/data/eth_receipts_proof_fixture.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

using namespace bridge;

BOOST_AUTO_TEST_SUITE(bridge_mpt_tests)

namespace {
std::vector<unsigned char> FromHexStr(const std::string& h)
{
    auto v{TryParseHex<unsigned char>(h)};
    BOOST_REQUIRE(v.has_value());
    return *v;
}
uint256 KeccakOf(std::span<const unsigned char> d)
{
    uint256 out;
    Keccak256().Write(d).Finalize(out);
    return out;
}
} // namespace

BOOST_AUTO_TEST_CASE(rlp_known_answers)
{
    // "dog" -> 0x83 'd' 'o' 'g'
    const auto dog{RlpEncodeBytes(std::span<const unsigned char>{(const unsigned char*)"dog", 3})};
    BOOST_CHECK_EQUAL(HexStr(dog), "83646f67");
    // integers: canonical big-endian, zero = empty string
    BOOST_CHECK_EQUAL(HexStr(RlpEncodeUint64(0)), "80");
    BOOST_CHECK_EQUAL(HexStr(RlpEncodeUint64(15)), "0f");
    BOOST_CHECK_EQUAL(HexStr(RlpEncodeUint64(1024)), "820400");
    // single byte < 0x80 self-encodes
    const unsigned char a{0x61};
    BOOST_CHECK_EQUAL(HexStr(RlpEncodeBytes({&a, 1})), "61");

    // decode round trip
    const auto item{RlpDecode(dog)};
    BOOST_REQUIRE(item);
    BOOST_CHECK(!item->is_list);
    BOOST_CHECK_EQUAL(HexStr(item->payload), "646f67");

    // list ["cat","dog"] = c8 8363617483646f67
    const auto lst{FromHexStr("c88363617483646f67")};
    const auto litem{RlpDecode(lst)};
    BOOST_REQUIRE(litem && litem->is_list);
    const auto kids{RlpChildren(*litem)};
    BOOST_REQUIRE(kids && kids->size() == 2);
    BOOST_CHECK_EQUAL(HexStr((*kids)[0].payload), "636174");
    BOOST_CHECK_EQUAL(HexStr((*kids)[1].payload), "646f67");
}

BOOST_AUTO_TEST_CASE(rlp_strictness)
{
    // Wrapped single byte < 0x80: 0x81 0x61 is non-canonical ("a" must be 0x61).
    BOOST_CHECK(!RlpDecode(FromHexStr("8161")));
    // Long form where short fits: 0xb8 0x03 "dog".
    BOOST_CHECK(!RlpDecode(FromHexStr("b803646f67")));
    // Length-of-length with leading zero.
    BOOST_CHECK(!RlpDecode(FromHexStr("b90003" + std::string(6, '6'))));
    // Trailing garbage after a complete item.
    BOOST_CHECK(!RlpDecode(FromHexStr("83646f6700")));
    // Truncated payload.
    BOOST_CHECK(!RlpDecode(FromHexStr("83646f")));
    // Empty input.
    BOOST_CHECK(!RlpDecode({}));
}

BOOST_AUTO_TEST_CASE(empty_trie_root_constant)
{
    // keccak(rlp("")) -- the canonical empty-trie root.
    const auto empty_rlp{FromHexStr("80")};
    // (HexStr over the raw digest bytes; uint256::GetHex would print reversed.)
    uint256 d{KeccakOf(empty_rlp)};
    BOOST_CHECK_EQUAL(HexStr(d),
                      "56e81f171bcc55a6ff8345e692c0f86e5b48e01b996cadc001622fb5e363b421");
}

BOOST_AUTO_TEST_CASE(mainnet_receipts_inclusion)
{
    const auto root_bytes{FromHexStr(eth_receipts_fixture::RECEIPTS_ROOT_HEX)};
    const uint256 root{std::span<const unsigned char>{root_bytes}};
    BOOST_REQUIRE(!eth_receipts_fixture::CASES.empty());
    for (const auto& c : eth_receipts_fixture::CASES) {
        const auto key{FromHexStr(c.key_hex)};
        BOOST_CHECK(HexStr(RlpEncodeUint64(c.index)) == c.key_hex);
        std::vector<std::vector<unsigned char>> proof;
        proof.reserve(c.proof_hex.size());
        for (const auto& n : c.proof_hex) proof.push_back(FromHexStr(n));
        const auto value{VerifyMptProof(root, key, proof)};
        BOOST_REQUIRE_MESSAGE(value.has_value(), "inclusion failed for index " << c.index);
        BOOST_CHECK_EQUAL(HexStr(*value), c.value_hex);
    }
}

BOOST_AUTO_TEST_CASE(mainnet_receipts_rejections)
{
    const auto root_bytes{FromHexStr(eth_receipts_fixture::RECEIPTS_ROOT_HEX)};
    const uint256 root{std::span<const unsigned char>{root_bytes}};
    const auto& c{eth_receipts_fixture::CASES.front()};
    const auto key{FromHexStr(c.key_hex)};
    std::vector<std::vector<unsigned char>> proof;
    for (const auto& n : c.proof_hex) proof.push_back(FromHexStr(n));

    // Baseline sanity.
    BOOST_REQUIRE(VerifyMptProof(root, key, proof));

    // Wrong root.
    uint256 bad_root{root};
    *bad_root.begin() ^= 0x01;
    BOOST_CHECK(!VerifyMptProof(bad_root, key, proof));

    // A flipped byte anywhere in any node breaks a hash link or the value.
    for (size_t i = 0; i < proof.size(); ++i) {
        auto mutated{proof};
        mutated[i][mutated[i].size() / 2] ^= 0x01;
        BOOST_CHECK(!VerifyMptProof(root, key, mutated));
    }

    // Truncated proof (drop the last node).
    {
        auto shorter{proof};
        shorter.pop_back();
        BOOST_CHECK(!VerifyMptProof(root, key, shorter));
    }
    // Padded proof (extra unconsumed node).
    {
        auto longer{proof};
        longer.push_back(FromHexStr("80"));
        BOOST_CHECK(!VerifyMptProof(root, key, longer));
    }
    // A key that this proof does not cover.
    {
        const auto other_key{RlpEncodeUint64(c.index + 7)};
        BOOST_CHECK(!VerifyMptProof(root, other_key, proof));
    }
    // Empty proof.
    BOOST_CHECK(!VerifyMptProof(root, key, {}));
}

BOOST_AUTO_TEST_SUITE_END()
