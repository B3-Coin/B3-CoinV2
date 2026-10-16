// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <crypto/hex_base.h>
#include <crypto/sha256.h>
#include <flowmesh2/core/merkle.h>
#include <flowmesh2/core/slot.h>

#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace flowmesh2;

namespace {

//! SHA256(label), as b32(label) in the Python golden script.
uint256 B32(std::string_view label)
{
    uint256 out;
    CSHA256{}.Write(reinterpret_cast<const unsigned char*>(label.data()), label.size()).Finalize(out.data());
    return out;
}

SlotHeaderV2 Sample()
{
    SlotHeaderV2 h;
    h.version = SLOT_HEADER_VERSION;
    h.domain = B32("domain");
    h.profile_hash = B32("profile");
    h.authority_epoch = 7;
    h.height = 42;
    h.parent_hash = B32("parent");
    h.parent_cert_hash = B32("parent_cert");
    h.anchor = Anchor{130, B32("anchor")};
    h.facts_from = FactCursor{130, 0};
    h.facts_to = FactCursor{130, 3};
    h.seat_set_root = B32("seats");
    h.facts_root = B32("facts");
    h.actions_root = B32("actions");
    h.outcomes_root = B32("outcomes");
    h.state_root = B32("state");
    h.action_count = 5;
    h.outcome_count = 9;
    h.commitment_version = COMMITMENT_VERSION_V0;
    return h;
}

void Flip(uint256& value) { value.data()[0] ^= 0x01; }

//! One mutation per SlotHeaderV2 field (structured binding below checks the count).
std::vector<std::pair<std::string, std::function<void(SlotHeaderV2&)>>> FieldMutations()
{
    return {
        {"version", [](SlotHeaderV2& h) { ++h.version; }},
        {"domain", [](SlotHeaderV2& h) { Flip(h.domain); }},
        {"profile_hash", [](SlotHeaderV2& h) { Flip(h.profile_hash); }},
        {"authority_epoch", [](SlotHeaderV2& h) { ++h.authority_epoch; }},
        {"height", [](SlotHeaderV2& h) { ++h.height; }},
        {"parent_hash", [](SlotHeaderV2& h) { Flip(h.parent_hash); }},
        {"parent_cert_hash", [](SlotHeaderV2& h) { Flip(h.parent_cert_hash); }},
        {"anchor.height", [](SlotHeaderV2& h) { ++h.anchor.height; }},
        {"anchor.hash", [](SlotHeaderV2& h) { Flip(h.anchor.hash); }},
        {"facts_from.height", [](SlotHeaderV2& h) { ++h.facts_from.height; }},
        {"facts_from.index", [](SlotHeaderV2& h) { ++h.facts_from.index; }},
        {"facts_to.height", [](SlotHeaderV2& h) { ++h.facts_to.height; }},
        {"facts_to.index", [](SlotHeaderV2& h) { ++h.facts_to.index; }},
        {"seat_set_root", [](SlotHeaderV2& h) { Flip(h.seat_set_root); }},
        {"facts_root", [](SlotHeaderV2& h) { Flip(h.facts_root); }},
        {"actions_root", [](SlotHeaderV2& h) { Flip(h.actions_root); }},
        {"outcomes_root", [](SlotHeaderV2& h) { Flip(h.outcomes_root); }},
        {"state_root", [](SlotHeaderV2& h) { Flip(h.state_root); }},
        {"action_count", [](SlotHeaderV2& h) { ++h.action_count; }},
        {"outcome_count", [](SlotHeaderV2& h) { ++h.outcome_count; }},
        {"commitment_version", [](SlotHeaderV2& h) { ++h.commitment_version; }},
    };
}

template <class T>
void CheckError(const Result<T>& result, ErrorCode expected)
{
    BOOST_REQUIRE_MESSAGE(!result.has_value(), "unexpected success");
    BOOST_CHECK_EQUAL(ToString(result.error().code), ToString(expected));
}

const char* const SAMPLE_BYTES{
    "01f2ff83860a4dc203988ed1a22ba1f21237f04abdbd0c4c951103cfbed121de781900eab6c028483d7126599ee6f50de0d27907b5c65fa9"
    "0524580b4b0f9852b007000000000000002a00000000000000e47125968b3b71049fbc4802d1e40a71ea1359decfabacf70b34588037d4ff"
    "0c2bd3b94abc5263494455a5d26872fcbb549de42f524e3c3545f0ffdea25a4fbd8200000079bfb0e2ba76b9d447606ddbcc494834f05a4c"
    "11deb052e74b49ea307a3c5bcd820000000000000082000000030000002530aa222c31f46dda5c7f589aeb063ad0e7c3be5fb0dd8e23bb38"
    "f620366c900694aacb66d62e742a92e8d5f1e82bd9d2a8ca1be88744201fbe63d0f50075022b0dcdd40016096b852d9d5b79de2ad570249d"
    "c8e4d4ebe4fa4ab81e2aa9af35b8b142fa994e87946b97288475b3e7cef3fbb6b86619a7443589c1e284d5e33e4ba69735ca53765ed6a709"
    "edb56c6ea236b7193a3b29a6b390c346f0f4340e4e050000000900000000"};

} // namespace

BOOST_AUTO_TEST_SUITE(slot_tests)

BOOST_AUTO_TEST_CASE(golden_bytes_hash_and_commit_digest)
{
    // Goldens computed independently with Python hashlib from the documented layout.
    const SlotHeaderV2 h{Sample()};
    const auto bytes{h.Encode()};
    BOOST_CHECK_EQUAL(bytes.size(), SLOT_HEADER_BYTES);
    BOOST_CHECK_EQUAL(HexStr(bytes), SAMPLE_BYTES);
    BOOST_CHECK_EQUAL(HexStr(h.Hash()), "9269219c0d3133234eb4f96fe74d634494f23965190b62ddec903314d134029c");
    BOOST_CHECK_EQUAL(HexStr(h.CommitDigest()), "9c8e5f77ca5eb60b71df0a1a0a02a7a110b9b10c3785a095626787fc4fb2e99a");
}

BOOST_AUTO_TEST_CASE(round_trip)
{
    const SlotHeaderV2 h{Sample()};
    const auto decoded{SlotHeaderV2::Decode(h.Encode())};
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK(*decoded == h);
    BOOST_CHECK(decoded->Hash() == h.Hash());
    // A default header also round-trips (all-zero hashes are a valid encoding).
    const SlotHeaderV2 zero;
    const auto zero_back{SlotHeaderV2::Decode(zero.Encode())};
    BOOST_REQUIRE(zero_back.has_value());
    BOOST_CHECK(*zero_back == zero);
}

BOOST_AUTO_TEST_CASE(hash_differs_from_commit_digest)
{
    for (const SlotHeaderV2& h : {Sample(), SlotHeaderV2{}}) {
        BOOST_CHECK(h.Hash() != h.CommitDigest());
    }
}

BOOST_AUTO_TEST_CASE(every_field_changes_hash_and_commit_digest)
{
    const SlotHeaderV2 base{Sample()};
    // One name per field: adding a field breaks this line until it has a mutation.
    const auto& [f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18] = base;
    (void)f1, (void)f2, (void)f3, (void)f4, (void)f5, (void)f6, (void)f7, (void)f8, (void)f9;
    (void)f10, (void)f11, (void)f12, (void)f13, (void)f14, (void)f15, (void)f16, (void)f17, (void)f18;
    // 18 fields; anchor and the two cursors have two members each: 21 mutations.
    BOOST_CHECK_EQUAL(FieldMutations().size(), 21U);

    std::set<uint256> hashes{base.Hash()};
    std::set<uint256> digests{base.CommitDigest()};
    for (const auto& [name, mutate] : FieldMutations()) {
        SlotHeaderV2 changed{base};
        mutate(changed);
        BOOST_CHECK_MESSAGE(hashes.insert(changed.Hash()).second, name << ": Hash unchanged or repeated");
        BOOST_CHECK_MESSAGE(digests.insert(changed.CommitDigest()).second, name << ": CommitDigest unchanged or repeated");
    }
}

BOOST_AUTO_TEST_CASE(decode_refusals)
{
    const auto bytes{Sample().Encode()};

    auto bad_version{bytes};
    bad_version[0] = 2;
    CheckError(SlotHeaderV2::Decode(bad_version), ErrorCode::DECODE);
    bad_version[0] = 0;
    CheckError(SlotHeaderV2::Decode(bad_version), ErrorCode::DECODE);

    auto bad_commitment{bytes};
    bad_commitment.back() = 1; // commitment v1 is M2-LED-02, not decodable yet
    CheckError(SlotHeaderV2::Decode(bad_commitment), ErrorCode::DECODE);

    const std::vector<unsigned char> short_input(bytes.begin(), bytes.end() - 1);
    CheckError(SlotHeaderV2::Decode(short_input), ErrorCode::DECODE);

    auto trailing{bytes};
    trailing.push_back(0);
    CheckError(SlotHeaderV2::Decode(trailing), ErrorCode::OVERSIZE); // longer than SLOT_HEADER_BYTES
    CheckError(DecodeExact<SlotHeaderV2>(trailing, trailing.size()), ErrorCode::DECODE);
}

BOOST_AUTO_TEST_CASE(genesis_header)
{
    const AuthorityVersion authority{3, B32("seats"), B32("profile")};
    const Anchor anchor{130, B32("anchor")};
    const FactCursor start{130, 0};
    const auto genesis{GenesisHeader(B32("domain"), authority, anchor, start, B32("state"))};
    BOOST_REQUIRE(genesis.has_value());
    const SlotHeaderV2& g{*genesis};
    const uint256 empty_root{MerkleRoot({})};
    BOOST_CHECK_EQUAL(g.version, SLOT_HEADER_VERSION);
    BOOST_CHECK(g.domain == B32("domain"));
    BOOST_CHECK(g.Authority() == authority);
    BOOST_CHECK_EQUAL(g.height, 0U);
    BOOST_CHECK(g.parent_hash.IsNull());
    BOOST_CHECK(g.parent_cert_hash.IsNull());
    BOOST_CHECK(g.anchor == anchor);
    BOOST_CHECK(g.facts_from == start);
    BOOST_CHECK(g.facts_to == start);
    BOOST_CHECK(g.facts_root == empty_root);
    BOOST_CHECK(g.actions_root == empty_root);
    BOOST_CHECK(g.outcomes_root == empty_root);
    BOOST_CHECK(g.state_root == B32("state"));
    BOOST_CHECK_EQUAL(g.action_count, 0U);
    BOOST_CHECK_EQUAL(g.outcome_count, 0U);
    BOOST_CHECK_EQUAL(g.commitment_version, COMMITMENT_VERSION_V0);
    // Deterministic.
    BOOST_CHECK(GenesisHeader(B32("domain"), authority, anchor, start, B32("state"))->Hash() == g.Hash());

    CheckError(GenesisHeader(uint256{}, authority, anchor, start, B32("state")), ErrorCode::INVALID_INPUT);
    CheckError(GenesisHeader(B32("domain"), AuthorityVersion{3, uint256{}, B32("profile")}, anchor, start, B32("state")),
               ErrorCode::INVALID_INPUT);
    CheckError(GenesisHeader(B32("domain"), AuthorityVersion{3, B32("seats"), uint256{}}, anchor, start, B32("state")),
               ErrorCode::INVALID_INPUT);
    CheckError(GenesisHeader(B32("domain"), authority, anchor, start, uint256{}), ErrorCode::INVALID_INPUT);
}

BOOST_AUTO_TEST_CASE(authority_accessor)
{
    const SlotHeaderV2 h{Sample()};
    const AuthorityVersion expected{7, B32("seats"), B32("profile")};
    BOOST_CHECK(h.Authority() == expected);
}

BOOST_AUTO_TEST_SUITE_END()
