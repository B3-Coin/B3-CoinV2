// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/net/short_id.h>
#include <flowmesh2/test/util/keys.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::net;

namespace {

ActionId Id(const std::string& label) { return ActionId{test::LabelHash("fm2/test/short-id/" + label)}; }

std::string Hex(const agreement::ShortId& id) { return HexStr(id); }

} // namespace

BOOST_AUTO_TEST_SUITE(short_id_tests)

BOOST_AUTO_TEST_CASE(encode_is_keyed_by_header_and_nonce)
{
    // M4-NET-02: Encode is a pure function of (header_hash, nonce, id).
    const ShortIdCodec codec{uint256::ONE, 7};
    const ActionId a{Id("a")};
    const agreement::ShortId sa{codec.Encode(a).value()};
    BOOST_CHECK(sa == codec.Encode(a).value());
    BOOST_CHECK((sa == ShortIdCodec{uint256::ONE, 7}.Encode(a).value()));
    // Another nonce, another header or another id each give another short ID (for these vectors).
    BOOST_CHECK((sa != ShortIdCodec{uint256::ONE, 8}.Encode(a).value()));
    BOOST_CHECK((sa != ShortIdCodec{uint256::ZERO, 7}.Encode(a).value()));
    BOOST_CHECK(sa != codec.Encode(Id("b")).value());
    // 6 bytes, and the 64-bit SipHash is truncated to its low 48 bits in little-endian order.
    BOOST_CHECK_EQUAL(sa.size(), agreement::SHORT_ID_BYTES);
    BOOST_CHECK_EQUAL(Hex(sa).size(), 12U);
    const uint64_t full{ShortIdCodec::SipHashOf(uint256::ONE, 7, a)};
    for (size_t i{0}; i < agreement::SHORT_ID_BYTES; ++i) {
        BOOST_CHECK_EQUAL(sa[i], static_cast<unsigned char>(full >> (8 * i)));
    }
    // Keys are derived as BIP152 derives them: SHA256(header_hash || LE64(nonce)), k0 and k1 its first two words.
    const auto [k0, k1]{ShortIdCodec::Keys(uint256::ONE, 7)};
    BOOST_CHECK(k0 != k1);
    BOOST_CHECK((ShortIdCodec::Keys(uint256::ONE, 7) == std::pair{k0, k1}));
    BOOST_CHECK((ShortIdCodec::Keys(uint256::ONE, 8) != std::pair{k0, k1}));
}

BOOST_AUTO_TEST_CASE(golden_short_ids)
{
    // Self-pinned vectors (the codec has no external reference; a change here
    // is a wire change of the relay encoding, M4-AGR-04 inclusion lists).
    const ShortIdCodec codec{uint256::ONE, 7};
    BOOST_CHECK_EQUAL(Hex(codec.Encode(ActionId{uint256::ZERO}).value()), GOLDEN_SHORT_ID_ZERO);
    BOOST_CHECK_EQUAL(Hex(codec.Encode(ActionId{uint256::ONE}).value()), GOLDEN_SHORT_ID_ONE);
    const ShortIdCodec all_zero{uint256::ZERO, 0};
    BOOST_CHECK_EQUAL(Hex(all_zero.Encode(ActionId{uint256::ZERO}).value()), GOLDEN_SHORT_ID_ALL_ZERO);
}

BOOST_AUTO_TEST_CASE(decode_returns_every_match_none_means_fetch)
{
    const ShortIdCodec codec{uint256::ONE, 7};
    std::vector<ActionId> known;
    for (int i{0}; i < 64; ++i) known.push_back(Id(std::to_string(i)));
    // Every known id decodes to exactly itself (no collision among these 64).
    std::set<agreement::ShortId> seen;
    for (const ActionId& id : known) {
        const agreement::ShortId sid{codec.Encode(id).value()};
        BOOST_CHECK(seen.insert(sid).second);
        const std::vector<ActionId> found{codec.Decode(sid, known).value()};
        BOOST_REQUIRE_EQUAL(found.size(), 1U);
        BOOST_CHECK(found[0] == id);
    }
    // Unknown: none, which means "fetch the body on the BULK lane".
    BOOST_CHECK(codec.Decode(codec.Encode(Id("unknown")).value(), known).value().empty());
    BOOST_CHECK(codec.Decode(agreement::ShortId{}, {}).value().empty());
    // A collision inside `known` (here the same id twice) gives every match, in `known` order.
    std::vector<ActionId> twice{Id("x"), Id("y"), Id("x")};
    const std::vector<ActionId> both{codec.Decode(codec.Encode(Id("x")).value(), twice).value()};
    BOOST_REQUIRE_EQUAL(both.size(), 2U);
    BOOST_CHECK(both[0] == Id("x") && both[1] == Id("x"));
    // Another codec (another proposal) does not decode this one's short IDs.
    const ShortIdCodec other{uint256::ONE, 8};
    BOOST_CHECK(other.Decode(codec.Encode(known[0]).value(), known).value().empty());
}

BOOST_AUTO_TEST_SUITE_END()
