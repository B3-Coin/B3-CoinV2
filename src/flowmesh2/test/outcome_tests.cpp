// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <crypto/hex_base.h>
#include <flowmesh2/core/codes.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/test/util/keys.h>
#include <serialize.h>
#include <streams.h>

#include <functional>
#include <set>
#include <string>
#include <utility>
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

//! The golden outcome of evidence/R4/golden.py: an IOC buy filled 2,000 lots at 250.
OutcomeV2 Sample()
{
    OutcomeV2 o;
    o.source = OutcomeSource::ACTION;
    o.subject = LabelHash("fm2/test/action");
    o.account = AccountId{LabelHash("fm2/test/account")};
    o.nonce = 2;
    o.code = OutcomeCode::FILLED;
    o.remaining_lots = 0;
    o.fills = {Fill{OrderId{LabelHash("fm2/test/maker")}, OrderId{LabelHash("fm2/test/action")},
                    MarketId{LabelHash("fm2/test/market")}, Side::BUY, 250, 2'000, 500'000, 50, 0}};
    o.deltas = {BalanceDelta{AccountId{LabelHash("fm2/test/account")}, AssetId::Native(), 2'000'000'000, 0},
                BalanceDelta{AccountId{LabelHash("fm2/test/account")}, AssetId{LabelHash("fm2/test/rusd")}, -500'000, 0}};
    return o;
}

void Flip(uint256& value) { value.data()[0] ^= 0x01; }

std::vector<std::pair<std::string, std::function<void(OutcomeV2&)>>> FieldMutations()
{
    return {
        {"source", [](OutcomeV2& o) { o.source = OutcomeSource::FACT; }},
        {"subject", [](OutcomeV2& o) { Flip(o.subject); }},
        {"account", [](OutcomeV2& o) { Flip(o.account.id); }},
        {"nonce", [](OutcomeV2& o) { ++o.nonce; }},
        {"code", [](OutcomeV2& o) { o.code = OutcomeCode::IOC_DONE; }},
        {"remaining_lots", [](OutcomeV2& o) { ++o.remaining_lots; }},
        {"fill.maker", [](OutcomeV2& o) { Flip(o.fills[0].maker.id); }},
        {"fill.taker", [](OutcomeV2& o) { Flip(o.fills[0].taker.id); }},
        {"fill.market", [](OutcomeV2& o) { Flip(o.fills[0].market.id); }},
        {"fill.taker_side", [](OutcomeV2& o) { o.fills[0].taker_side = Side::SELL; }},
        {"fill.price_ticks", [](OutcomeV2& o) { ++o.fills[0].price_ticks; }},
        {"fill.lots", [](OutcomeV2& o) { ++o.fills[0].lots; }},
        {"fill.quote_atoms", [](OutcomeV2& o) { ++o.fills[0].quote_atoms; }},
        {"fill.seller_fee", [](OutcomeV2& o) { ++o.fills[0].seller_fee; }},
        {"fill.seller_quote_before", [](OutcomeV2& o) { ++o.fills[0].seller_quote_before; }},
        {"fills removed", [](OutcomeV2& o) { o.fills.clear(); }},
        {"delta.account", [](OutcomeV2& o) { Flip(o.deltas[0].account.id); }},
        {"delta.asset", [](OutcomeV2& o) { Flip(o.deltas[0].asset.id); }},
        {"delta.available", [](OutcomeV2& o) { ++o.deltas[0].available_delta; }},
        {"delta.reserved", [](OutcomeV2& o) { ++o.deltas[0].reserved_delta; }},
        {"deltas swapped", [](OutcomeV2& o) { std::swap(o.deltas[0], o.deltas[1]); }},
    };
}

std::vector<unsigned char> PatchedSample(size_t offset, std::vector<unsigned char> replacement)
{
    auto bytes{Sample().Encode()};
    std::copy(replacement.begin(), replacement.end(), bytes.begin() + offset);
    return bytes;
}

} // namespace

BOOST_AUTO_TEST_SUITE(outcome_tests)

BOOST_AUTO_TEST_CASE(source_registry_pinned)
{
    BOOST_CHECK_EQUAL(static_cast<int>(OutcomeSource::FACT), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(OutcomeSource::ACTION), 2);
    BOOST_CHECK_EQUAL(static_cast<int>(OutcomeSource::SLOT), 3);
    BOOST_CHECK_EQUAL(std::string{ToString(OutcomeSource::SLOT)}, "SLOT");
    BOOST_CHECK_EQUAL(std::string{ToString(static_cast<OutcomeSource>(0))}, "UNKNOWN");
    BOOST_CHECK(!IsKnown(static_cast<OutcomeSource>(4)));
}

BOOST_AUTO_TEST_CASE(golden_bytes_and_leaf)
{
    // Goldens from evidence/R4/golden.py (independent of the C++ code).
    const OutcomeV2 o{Sample()};
    BOOST_CHECK_EQUAL(o.Encode().size(), 382U);
    BOOST_CHECK_EQUAL(HexStr(o.Encode()),
                      "02d287454dc432754e7fba5bf80d207184ddc9f1e4cb5745018a7739331fb5f99713295436770665b2b56e8b1e3ced"
                      "53455c0919bf3a497bd29ddd1ca273c2c3d2020000000000000003000000000000000000011d24633b0e7e994f0d2aa1"
                      "f012228cb91f6c54881108c52ec2386e0d60e5d78ad287454dc432754e7fba5bf80d207184ddc9f1e4cb5745018a7739"
                      "331fb5f997629d31dd15ec7d49cb9eb7ec7244a8dbfa8cfdc9da7973340b8eb780e1f8af9701fa00000000000000d007"
                      "00000000000020a1070000000000320000000000000000000000000000000213295436770665b2b56e8b1e3ced53455c"
                      "0919bf3a497bd29ddd1ca273c2c3d20000000000000000000000000000000000000000000000000000000000000000009"
                      "4357700000000000000000000000013295436770665b2b56e8b1e3ced53455c0919bf3a497bd29ddd1ca273c2c3d2351c"
                      "c7086ec37dd5bebb5775297a065b4f785b3bac0375eed91baf048aa96e85e05ef8ffffffffff0000000000000000");
    BOOST_CHECK_EQUAL(HexStr(o.LeafHash()), "8b723dd598e8cdff10191ab5f8d3be6a31e94dfd33046a244ba889d53d5c5c41");
    BOOST_CHECK(o.LeafHash() == Tagged(HashTag::OUTCOME, o.Encode()));
}

BOOST_AUTO_TEST_CASE(round_trip)
{
    const ProfileParams profile{Profile()};
    for (const OutcomeV2& o : {Sample(), OutcomeV2{}}) {
        const auto decoded{OutcomeV2::Decode(o.Encode(), profile)};
        BOOST_REQUIRE(decoded.has_value());
        BOOST_CHECK(*decoded == o);
        BOOST_CHECK(decoded->LeafHash() == o.LeafHash());
    }
    // Extreme in-range values round-trip.
    OutcomeV2 edge{Sample()};
    edge.fills[0].quote_atoms = MAX_ATOMS;
    edge.deltas[0].available_delta = -MAX_ATOMS;
    edge.deltas[1].reserved_delta = MAX_ATOMS;
    const auto decoded{OutcomeV2::Decode(edge.Encode(), profile)};
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK(*decoded == edge);
}

BOOST_AUTO_TEST_CASE(every_field_changes_the_leaf)
{
    const OutcomeV2 base{Sample()};
    const auto& [f1, f2, f3, f4, f5, f6, f7, f8] = base; // 8 fields
    (void)f1, (void)f2, (void)f3, (void)f4, (void)f5, (void)f6, (void)f7, (void)f8;
    std::set<uint256> leaves{base.LeafHash()};
    for (const auto& [name, mutate] : FieldMutations()) {
        OutcomeV2 changed{base};
        mutate(changed);
        BOOST_CHECK_MESSAGE(leaves.insert(changed.LeafHash()).second, name << ": leaf unchanged or repeated");
    }
}

BOOST_AUTO_TEST_CASE(fill_and_delta_caps)
{
    const ProfileParams profile{Profile()};
    BOOST_REQUIRE_EQUAL(OutcomeV2::MaxFills(profile), 1'024U);
    BOOST_REQUIRE_EQUAL(OutcomeV2::MaxDeltas(profile), 2'050U);

    OutcomeV2 at_cap{Sample()};
    at_cap.fills.assign(1'024, Sample().fills[0]);
    at_cap.deltas.assign(2'050, Sample().deltas[0]);
    BOOST_CHECK_EQUAL(at_cap.Encode().size(), OutcomeV2::MaxEncodedSize(profile));
    BOOST_CHECK(OutcomeV2::Decode(at_cap.Encode(), profile).has_value());

    OutcomeV2 too_many_fills{Sample()};
    too_many_fills.fills.assign(1'025, Sample().fills[0]);
    too_many_fills.deltas.clear();
    CheckError(OutcomeV2::Decode(too_many_fills.Encode(), profile), ErrorCode::OVERSIZE);

    OutcomeV2 too_many_deltas{Sample()};
    too_many_deltas.fills.clear();
    too_many_deltas.deltas.assign(2'051, Sample().deltas[0]);
    CheckError(OutcomeV2::Decode(too_many_deltas.Encode(), profile), ErrorCode::OVERSIZE);

    // A count alone, above the cap, is OVERSIZE before anything is allocated.
    OutcomeV2 empty{Sample()};
    empty.fills.clear();
    empty.deltas.clear();
    auto count_only{empty.Encode()};
    count_only.resize(count_only.size() - 2); // drop both zero counts
    VectorWriter writer{count_only, count_only.size()};
    WriteCompactSize(writer, 1'000'000);
    CheckError(OutcomeV2::Decode(count_only, profile), ErrorCode::OVERSIZE);
}

BOOST_AUTO_TEST_CASE(decode_refusals)
{
    const ProfileParams profile{Profile()};
    // Layout offsets: source 0, subject 1, account 33, nonce 65, code 73,
    // remaining 75, fill count 83, fill 84 (side at 84+96, quote at 84+113).
    for (const unsigned char source : {0x00, 0x04, 0xff}) {
        CheckError(OutcomeV2::Decode(PatchedSample(0, {source}), profile), ErrorCode::DECODE);
    }
    for (const uint16_t code : {uint16_t{0}, uint16_t{11}, uint16_t{63}, uint16_t{77}, uint16_t{97}, uint16_t{0xffff}}) {
        CheckError(OutcomeV2::Decode(PatchedSample(73, {static_cast<unsigned char>(code & 0xff),
                                                         static_cast<unsigned char>(code >> 8)}),
                                     profile),
                   ErrorCode::DECODE);
    }
    CheckError(OutcomeV2::Decode(PatchedSample(84 + 96, {0x00}), profile), ErrorCode::DECODE); // fill side
    // quote_atoms = -1 and MAX_ATOMS + 1.
    CheckError(OutcomeV2::Decode(PatchedSample(84 + 113, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}), profile),
               ErrorCode::DECODE);
    CheckError(OutcomeV2::Decode(PatchedSample(84 + 113, {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40}), profile),
               ErrorCode::DECODE);

    // A delta of MAX_ATOMS + 1 or -(MAX_ATOMS + 1).
    for (const int64_t bad : {MAX_ATOMS + 1, -MAX_ATOMS - 1}) {
        OutcomeV2 o{Sample()};
        o.deltas[1].available_delta = bad;
        CheckError(OutcomeV2::Decode(o.Encode(), profile), ErrorCode::DECODE);
        o = Sample();
        o.deltas[0].reserved_delta = bad;
        CheckError(OutcomeV2::Decode(o.Encode(), profile), ErrorCode::DECODE);
    }

    auto trailing{Sample().Encode()};
    trailing.push_back(0x00);
    CheckError(OutcomeV2::Decode(trailing, profile), ErrorCode::DECODE);
    const auto bytes{Sample().Encode()};
    const std::vector<unsigned char> truncated(bytes.begin(), bytes.end() - 1);
    CheckError(OutcomeV2::Decode(truncated, profile), ErrorCode::DECODE);
    std::vector<unsigned char> huge(OutcomeV2::MaxEncodedSize(profile) + 1, 0x00);
    CheckError(OutcomeV2::Decode(huge, profile), ErrorCode::OVERSIZE);
}

BOOST_AUTO_TEST_SUITE_END()
