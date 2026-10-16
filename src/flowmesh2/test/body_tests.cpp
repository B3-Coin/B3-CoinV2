// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <crypto/hex_base.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/merkle.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/test/util/keys.h>
#include <serialize.h>
#include <streams.h>

#include <cstdint>
#include <string_view>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;

namespace {

ProfileParams Profile() { return DevRegtestProfile(ChainType::REGTEST).value(); }

template <class T>
void CheckError(const Result<T>& result, ErrorCode expected)
{
    BOOST_REQUIRE_MESSAGE(!result.has_value(), "unexpected success");
    BOOST_CHECK_EQUAL(ToString(result.error().code), ToString(expected));
}

//! The golden action of action_tests, signed with the auxiliary randomness `aux_label`.
ActionV2 SampleSigned(std::string_view aux_label)
{
    ActionV2 a;
    a.domain = LabelHash("fm2/test/domain");
    a.signer = XOnlyFromLabel("fm2/test/alice");
    a.nonce = 7;
    a.ref_slot = 3;
    a.max_delay = 16;
    a.kind = ActionKind::PLACE;
    a.payload = EncodePayload(PlacePayload{MarketId{LabelHash("fm2/test/market")}, Side::SELL, TimeInForce::GTC, 250, 5'000})
                    .value();
    a.sig = SignDigest(KeyFromLabel("fm2/test/alice"), a.Id().id, LabelHash(aux_label));
    return a;
}

//! The golden body of the Python script.
SlotBody SampleBody()
{
    SlotBody body;
    body.facts = {{0x01, 0x02, 0x03, 0x04, 0x05}, {}};
    body.originals = {SampleSigned("fm2/test/aux").Encode(), SampleSigned("fm2/test/aux2").Encode()};
    return body;
}

std::vector<unsigned char> CountOnly(uint64_t facts, std::vector<unsigned char> tail = {})
{
    std::vector<unsigned char> out;
    VectorWriter writer{out, 0};
    WriteCompactSize(writer, facts);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

} // namespace

BOOST_AUTO_TEST_SUITE(body_tests)

BOOST_AUTO_TEST_CASE(golden_body_hash_and_roots)
{
    // Goldens from evidence/R4/golden.py (independent of the C++ code).
    const SlotBody body{SampleBody()};
    BOOST_CHECK_EQUAL(body.Encode().size(), 409U);
    BOOST_CHECK_EQUAL(HexStr(body.BodyHash()), "26cd3ab5c3511f6cd3b4a6dd898c9b0ab31ea60e1afd9811f52021413cd54cb4");
    BOOST_CHECK_EQUAL(HexStr(body.ActionsRoot()), "66913b86afaefc145574b054b962544c970f99208cc77bfc245fc09c0f70997a");
    BOOST_CHECK_EQUAL(HexStr(body.FactsRoot()), "41ba9fee01292f9b5ed9b2060484848c74e5e76c5579dea153c510a9480acc09");
    BOOST_CHECK_EQUAL(HexStr(SlotBody{}.BodyHash()), "08bb117bb28a8a91d37732e04430070f613143a02efb96f8e5ccd4725b45a7eb");
    BOOST_CHECK_EQUAL(HexStr(ActionLeaf(body.originals[0])),
                      "f8b45b09d71a9bc9d203e56050173eb38dc16db1fea4d41dfab3fc7c37eb5c12");
    BOOST_CHECK_EQUAL(HexStr(ActionLeaf(body.originals[1])),
                      "bcf550f58064e569b11b285496ec6783906c0ecc3920a439a81d2892a1d47a6f");
    // The empty lists commit to the empty tree.
    BOOST_CHECK(SlotBody{}.ActionsRoot() == MerkleRoot({}));
    BOOST_CHECK(SlotBody{}.FactsRoot() == MerkleRoot({}));
}

BOOST_AUTO_TEST_CASE(round_trip_and_exact_length)
{
    const ProfileParams profile{Profile()};
    const SlotBody body{SampleBody()};
    const auto decoded{SlotBody::Decode(body.Encode(), profile)};
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK(*decoded == body);
    BOOST_CHECK(decoded->BodyHash() == body.BodyHash());

    auto trailing{body.Encode()};
    trailing.push_back(0x00);
    CheckError(SlotBody::Decode(trailing, profile), ErrorCode::DECODE);
    const auto bytes{body.Encode()};
    const std::vector<unsigned char> truncated(bytes.begin(), bytes.end() - 1);
    CheckError(SlotBody::Decode(truncated, profile), ErrorCode::DECODE);
    CheckError(SlotBody::Decode({}, profile), ErrorCode::DECODE);
}

BOOST_AUTO_TEST_CASE(originals_cap_1024_accepted_1025_refused)
{
    const ProfileParams profile{Profile()};
    BOOST_REQUIRE_EQUAL(profile.max_actions_per_slot, 1'024U);
    const auto original{SampleSigned("fm2/test/aux").Encode()};

    SlotBody at_cap;
    at_cap.originals.assign(1'024, original);
    const auto ok{SlotBody::Decode(at_cap.Encode(), profile)};
    BOOST_REQUIRE(ok.has_value());
    BOOST_CHECK_EQUAL(ok->originals.size(), 1'024U);

    SlotBody over;
    over.originals.assign(1'025, original);
    CheckError(SlotBody::Decode(over.Encode(), profile), ErrorCode::OVERSIZE);

    // Only the count, no items behind it: still OVERSIZE (checked before allocation).
    CheckError(SlotBody::Decode(CountOnly(0, CountOnly(1'025)), profile), ErrorCode::OVERSIZE);
    CheckError(SlotBody::Decode(CountOnly(0, CountOnly(UINT64_MAX)), profile), ErrorCode::OVERSIZE);
}

BOOST_AUTO_TEST_CASE(fact_and_item_caps)
{
    const ProfileParams profile{Profile()};

    SlotBody facts_at_cap;
    facts_at_cap.facts.assign(MAX_SLOT_FACTS, std::vector<unsigned char>(MAX_FACT_BYTES, 0x11));
    BOOST_CHECK(SlotBody::Decode(facts_at_cap.Encode(), profile).has_value());

    SlotBody too_many_facts;
    too_many_facts.facts.assign(MAX_SLOT_FACTS + 1, {0x01});
    CheckError(SlotBody::Decode(too_many_facts.Encode(), profile), ErrorCode::OVERSIZE);
    CheckError(SlotBody::Decode(CountOnly(MAX_SLOT_FACTS + 1), profile), ErrorCode::OVERSIZE);

    SlotBody long_fact;
    long_fact.facts.push_back(std::vector<unsigned char>(MAX_FACT_BYTES + 1, 0x01));
    CheckError(SlotBody::Decode(long_fact.Encode(), profile), ErrorCode::OVERSIZE);

    const size_t max_action{ActionV2::MaxEncodedSize(profile)};
    SlotBody action_at_cap;
    action_at_cap.originals.push_back(std::vector<unsigned char>(max_action, 0x01));
    BOOST_CHECK(SlotBody::Decode(action_at_cap.Encode(), profile).has_value());
    SlotBody long_action;
    long_action.originals.push_back(std::vector<unsigned char>(max_action + 1, 0x01));
    CheckError(SlotBody::Decode(long_action.Encode(), profile), ErrorCode::OVERSIZE);

    // Whole input above the maximum encoding.
    std::vector<unsigned char> huge(SlotBody::MaxEncodedSize(profile) + 1, 0x00);
    CheckError(SlotBody::Decode(huge, profile), ErrorCode::OVERSIZE);
}

BOOST_AUTO_TEST_CASE(two_signatures_one_action_id_two_leaves)
{
    const ActionV2 first{SampleSigned("fm2/test/aux")};
    const ActionV2 second{SampleSigned("fm2/test/aux2")};
    BOOST_REQUIRE(first.VerifySignature());
    BOOST_REQUIRE(second.VerifySignature());
    BOOST_CHECK(first.sig != second.sig);
    BOOST_CHECK(first.Id() == second.Id());
    BOOST_CHECK(ActionLeaf(first.Encode()) != ActionLeaf(second.Encode()));

    SlotBody a;
    a.originals = {first.Encode()};
    SlotBody b;
    b.originals = {second.Encode()};
    BOOST_CHECK(a.ActionsRoot() != b.ActionsRoot());
    BOOST_CHECK(a.BodyHash() != b.BodyHash());
}

BOOST_AUTO_TEST_CASE(leaf_tags_are_domain_separated)
{
    const std::vector<unsigned char> bytes{0x01, 0x02, 0x03};
    BOOST_CHECK(ActionLeaf(bytes) == Tagged(HashTag::ACTION_LEAF, bytes));
    BOOST_CHECK(FactLeaf(bytes) == Tagged(HashTag::FACT, bytes));
    BOOST_CHECK(ActionLeaf(bytes) != FactLeaf(bytes));

    // Facts and originals are separate lists: moving an item between them
    // changes the body hash and both roots.
    SlotBody as_fact;
    as_fact.facts = {bytes};
    SlotBody as_original;
    as_original.originals = {bytes};
    BOOST_CHECK(as_fact.BodyHash() != as_original.BodyHash());
    BOOST_CHECK(as_fact.FactsRoot() != as_original.FactsRoot());
    BOOST_CHECK(as_fact.ActionsRoot() != as_original.ActionsRoot());
}

BOOST_AUTO_TEST_SUITE_END()
