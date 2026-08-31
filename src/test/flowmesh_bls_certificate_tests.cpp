// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/bls_certificate.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <numeric>
#include <vector>

namespace {

uint256 Filled(const unsigned char value)
{
    uint256 out;
    std::fill(out.begin(), out.end(), value);
    return out;
}

bls::SecretKey Key(const uint32_t index, const unsigned char salt)
{
    std::array<unsigned char, 32> ikm{};
    for (size_t i{0}; i < ikm.size(); ++i) {
        ikm[i] = static_cast<unsigned char>(salt + index * 13 + i * 7);
    }
    const auto key{bls::SecretKey::FromIKM(ikm)};
    BOOST_REQUIRE(key.has_value());
    return *key;
}

struct SeatFixture {
    uint256 domain{Filled(0x11)};
    uint256 market_id{Filled(0x22)};
    uint64_t anchor_height{130};
    uint256 anchor_hash{Filled(0x23)};
    std::vector<bls::SecretKey> secrets;
    std::vector<flowmesh::BlsSeatBinding> bindings;
    flowmesh::ActiveFnBlsSeatSet seats;
};

SeatFixture Seats(const size_t count, const uint64_t epoch = 17,
                  const unsigned char salt = 1)
{
    struct Entry {
        bls::SecretKey secret;
        flowmesh::BlsSeatBinding binding;
        flowmesh::SeatId id;
    };
    SeatFixture out;
    std::vector<Entry> entries;
    for (size_t i{0}; i < count; ++i) {
        const bls::SecretKey secret{Key(i, salt)};
        flowmesh::BlsSeatBinding binding;
        binding.outpoint = COutPoint{
            Txid::FromUint256(Filled(static_cast<unsigned char>(salt + i + 40))),
            static_cast<uint32_t>(i)};
        binding.public_key = secret.GetPublicKey().Compressed();
        binding.proof_of_possession = secret.SignPoP().Compressed();
        entries.push_back(Entry{secret, binding,
                                flowmesh::ComputeFlowMeshSeatId(out.domain, binding.outpoint)});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.id < b.id || (a.id == b.id && a.binding.outpoint < b.binding.outpoint);
    });
    for (const Entry& entry : entries) {
        out.secrets.push_back(entry.secret);
        out.bindings.push_back(entry.binding);
    }
    flowmesh::BlsSeatSetCheck check{flowmesh::BlsSeatSetCheck::BAD_PUBLIC_KEY};
    const auto set{flowmesh::BuildActiveFnBlsSeatSet(
        out.domain, out.market_id, epoch, out.anchor_height, out.anchor_hash,
        out.bindings, check)};
    BOOST_REQUIRE(set.has_value());
    BOOST_REQUIRE(check == flowmesh::BlsSeatSetCheck::OK);
    out.seats = *set;
    return out;
}

flowmesh::BlsCertificateContext Context(const SeatFixture& fixture)
{
    return flowmesh::BlsCertificateContext{fixture.domain, fixture.market_id,
                                           fixture.seats.epoch, fixture.seats.set_hash,
                                           123456, Filled(0x33)};
}

std::vector<flowmesh::IndexedBlsSignature> Sign(
    const SeatFixture& fixture, const flowmesh::BlsCertificateContext& context,
    const std::vector<uint32_t>& indices)
{
    std::vector<flowmesh::IndexedBlsSignature> out;
    for (const uint32_t index : indices) {
        const auto sig{flowmesh::SignBlsMicroblockCertificate(
            fixture.secrets.at(index), context, fixture.seats)};
        BOOST_REQUIRE(sig.has_value());
        out.push_back({index, *sig});
    }
    return out;
}

flowmesh::BlsMicroblockCertificate Certificate(
    const SeatFixture& fixture, const flowmesh::BlsCertificateContext& context,
    const std::vector<uint32_t>& indices)
{
    const auto partials{Sign(fixture, context, indices)};
    flowmesh::BlsCertificateAssemblyCheck check{
        flowmesh::BlsCertificateAssemblyCheck::AGGREGATION_FAILED};
    const auto cert{flowmesh::AssembleBlsMicroblockCertificate(
        context, fixture.seats, partials, check)};
    BOOST_REQUIRE(cert.has_value());
    BOOST_REQUIRE(check == flowmesh::BlsCertificateAssemblyCheck::OK);
    return *cert;
}

} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_bls_certificate_tests)

BOOST_AUTO_TEST_CASE(frozen_threshold_digest_and_codec_bounds)
{
    static_assert(flowmesh::FlowMeshBlsThreshold(4) == 3);
    static_assert(flowmesh::FlowMeshBlsThreshold(5) == 4);
    static_assert(flowmesh::FlowMeshBlsThreshold(6) == 5);
    static_assert(flowmesh::FlowMeshSignerBitmapBytes(5000) == 625);
    static_assert(flowmesh::FLOWMESH_BLS_CERTIFICATE_MAX_SIZE == 769);

    flowmesh::BlsCertificateContext vector{Filled(0x11), Filled(0x22), 17,
                                           Filled(0x44), 123456, Filled(0x33)};
    BOOST_CHECK_EQUAL(
        HexStr(flowmesh::FlowMeshBlsCertificateDigest(vector)),
        "30484e4acdf917f51ec1cc009df7aa9e7a0683fa002a68c42e1657866825a3f2");

    const SeatFixture fixture{Seats(4)};
    const auto context{Context(fixture)};
    const auto sig{flowmesh::SignBlsMicroblockCertificate(
        fixture.secrets[0], context, fixture.seats)};
    BOOST_REQUIRE(sig.has_value());
    flowmesh::BlsMicroblockCertificate maximum{context.seat_epoch, context.sequence,
                                               context.microblock_hash,
                                               std::vector<unsigned char>(625),
                                               sig->Compressed()};
    maximum.signer_bitmap.back() = 0x80; // canonical seat index 4,999
    const auto bytes{flowmesh::EncodeBlsMicroblockCertificate(maximum, 5000)};
    BOOST_REQUIRE(bytes.has_value());
    BOOST_CHECK_EQUAL(bytes->size(), 769U);
    BOOST_CHECK(flowmesh::DecodeBlsMicroblockCertificate(*bytes, 5000) == maximum);
    BOOST_CHECK(!flowmesh::DecodeBlsMicroblockCertificate(*bytes, 5001));
    BOOST_CHECK(!flowmesh::EncodeBlsMicroblockCertificate(maximum, 4999));
    auto extra{*bytes};
    extra.push_back(0);
    BOOST_CHECK(!flowmesh::DecodeBlsMicroblockCertificate(extra, 5000));

    const auto small{Certificate(fixture, context, {0, 1, 2})};
    const auto encoded{flowmesh::EncodeBlsMicroblockCertificate(small, 4)};
    BOOST_REQUIRE(encoded.has_value());
    BOOST_CHECK_EQUAL(
        HexStr(std::span<const unsigned char>{encoded->data(), 48}),
        "0000000000000011000000000001e240"
        "3333333333333333333333333333333333333333333333333333333333333333");
}

BOOST_AUTO_TEST_CASE(seat_set_is_seatid_ordered_pop_verified_and_unique)
{
    const SeatFixture fixture{Seats(4)};
    BOOST_CHECK(flowmesh::CheckActiveFnBlsSeatSet(fixture.domain, fixture.seats) ==
                flowmesh::BlsSeatSetCheck::OK);
    flowmesh::BlsSeatSetCheck check{flowmesh::BlsSeatSetCheck::OK};

    std::vector<flowmesh::BlsSeatBinding> three{fixture.bindings.begin(),
                                                fixture.bindings.begin() + 3};
    BOOST_CHECK(!flowmesh::BuildActiveFnBlsSeatSet(
        fixture.domain, fixture.market_id, 1, fixture.anchor_height,
        fixture.anchor_hash, three, check));
    BOOST_CHECK(check == flowmesh::BlsSeatSetCheck::TOO_SMALL);

    auto reversed{fixture.bindings};
    std::reverse(reversed.begin(), reversed.end());
    BOOST_CHECK(!flowmesh::BuildActiveFnBlsSeatSet(
        fixture.domain, fixture.market_id, 1, fixture.anchor_height,
        fixture.anchor_hash, reversed, check));
    BOOST_CHECK(check == flowmesh::BlsSeatSetCheck::NON_CANONICAL_MEMBERS);

    auto duplicate_key{fixture.bindings};
    duplicate_key[1].public_key = duplicate_key[0].public_key;
    duplicate_key[1].proof_of_possession = duplicate_key[0].proof_of_possession;
    BOOST_CHECK(!flowmesh::BuildActiveFnBlsSeatSet(
        fixture.domain, fixture.market_id, 1, fixture.anchor_height,
        fixture.anchor_hash, duplicate_key, check));
    BOOST_CHECK(check == flowmesh::BlsSeatSetCheck::DUPLICATE_KEYS);

    auto bad_pop{fixture.bindings};
    bad_pop[2].proof_of_possession.fill(0);
    BOOST_CHECK(!flowmesh::BuildActiveFnBlsSeatSet(
        fixture.domain, fixture.market_id, 1, fixture.anchor_height,
        fixture.anchor_hash, bad_pop, check));
    BOOST_CHECK(check == flowmesh::BlsSeatSetCheck::BAD_PROOF_OF_POSSESSION);

    auto bad_key{fixture.bindings};
    bad_key[2].public_key.fill(0xff);
    BOOST_CHECK(!flowmesh::BuildActiveFnBlsSeatSet(
        fixture.domain, fixture.market_id, 1, fixture.anchor_height,
        fixture.anchor_hash, bad_key, check));
    BOOST_CHECK(check == flowmesh::BlsSeatSetCheck::BAD_PUBLIC_KEY);

    auto bad_id{fixture.seats};
    bad_id.members[0].seat_id = Filled(0x99);
    BOOST_CHECK(flowmesh::CheckActiveFnBlsSeatSet(fixture.domain, bad_id) ==
                flowmesh::BlsSeatSetCheck::BAD_SEAT_ID);

    auto arbitrary_hash{fixture.seats};
    arbitrary_hash.set_hash = Filled(0x98);
    BOOST_CHECK(flowmesh::CheckActiveFnBlsSeatSet(fixture.domain, arbitrary_hash) ==
                flowmesh::BlsSeatSetCheck::BAD_SET_HASH);
    auto changed_anchor{fixture.seats};
    ++changed_anchor.anchor_height;
    BOOST_CHECK(flowmesh::CheckActiveFnBlsSeatSet(fixture.domain, changed_anchor) ==
                flowmesh::BlsSeatSetCheck::BAD_SET_HASH);

    std::vector<flowmesh::BlsSeatBinding> over(5001, fixture.bindings[0]);
    BOOST_CHECK(!flowmesh::BuildActiveFnBlsSeatSet(
        fixture.domain, fixture.market_id, 1, fixture.anchor_height,
        fixture.anchor_hash, over, check));
    BOOST_CHECK(check == flowmesh::BlsSeatSetCheck::TOO_LARGE);
}

BOOST_AUTO_TEST_CASE(end_to_end_and_all_context_rejections)
{
    for (const size_t k : {size_t{4}, size_t{5}, size_t{6}}) {
        const SeatFixture fixture{Seats(k, 44, static_cast<unsigned char>(k + 10))};
        const auto context{Context(fixture)};
        std::vector<uint32_t> indices(flowmesh::FlowMeshBlsThreshold(k));
        std::iota(indices.begin(), indices.end(), 0);
        std::reverse(indices.begin(), indices.end());
        const auto cert{Certificate(fixture, context, indices)};
        BOOST_CHECK(flowmesh::CheckBlsMicroblockCertificate(
                        cert, context, fixture.seats) == flowmesh::BlsCertificateCheck::OK);
        const auto bytes{flowmesh::EncodeBlsMicroblockCertificate(cert, k)};
        BOOST_REQUIRE(bytes.has_value());
        const auto decoded{flowmesh::DecodeBlsMicroblockCertificate(*bytes, k)};
        BOOST_REQUIRE(decoded.has_value());
        BOOST_CHECK(flowmesh::CheckBlsMicroblockCertificate(
                        *decoded, context, fixture.seats) == flowmesh::BlsCertificateCheck::OK);
    }

    const SeatFixture fixture{Seats(4)};
    const auto context{Context(fixture)};
    const auto valid{Certificate(fixture, context, {0, 1, 2})};
    auto cert{valid};
    ++cert.seat_epoch;
    BOOST_CHECK(flowmesh::CheckBlsMicroblockCertificate(cert, context, fixture.seats) ==
                flowmesh::BlsCertificateCheck::WRONG_SEAT_EPOCH);
    cert = valid;
    ++cert.sequence;
    BOOST_CHECK(flowmesh::CheckBlsMicroblockCertificate(cert, context, fixture.seats) ==
                flowmesh::BlsCertificateCheck::WRONG_SEQUENCE);
    cert = valid;
    cert.microblock_hash = Filled(0x55);
    BOOST_CHECK(flowmesh::CheckBlsMicroblockCertificate(cert, context, fixture.seats) ==
                flowmesh::BlsCertificateCheck::WRONG_MICROBLOCK_HASH);

    auto wrong_set_context{context};
    wrong_set_context.seat_set_hash = Filled(0x66);
    BOOST_CHECK(flowmesh::CheckBlsMicroblockCertificate(valid, wrong_set_context,
                                                        fixture.seats) ==
                flowmesh::BlsCertificateCheck::WRONG_SEAT_SET);
    auto wrong_market{context};
    wrong_market.market_id = Filled(0x77);
    BOOST_CHECK(flowmesh::CheckBlsMicroblockCertificate(valid, wrong_market, fixture.seats) ==
                flowmesh::BlsCertificateCheck::WRONG_SEAT_SET);
    auto wrong_domain{context};
    wrong_domain.domain = Filled(0x78);
    BOOST_CHECK(flowmesh::CheckBlsMicroblockCertificate(valid, wrong_domain, fixture.seats) ==
                flowmesh::BlsCertificateCheck::INVALID_SEAT_SET);

    cert = valid;
    cert.signer_bitmap[0] |= 0x80;
    BOOST_CHECK(flowmesh::CheckBlsMicroblockCertificate(cert, context, fixture.seats) ==
                flowmesh::BlsCertificateCheck::MALFORMED_BITMAP);
    cert = valid;
    cert.signer_bitmap[0] = 0;
    BOOST_CHECK(flowmesh::CheckBlsMicroblockCertificate(cert, context, fixture.seats) ==
                flowmesh::BlsCertificateCheck::NO_SIGNERS);
    cert = valid;
    cert.signer_bitmap[0] = 0x03;
    BOOST_CHECK(flowmesh::CheckBlsMicroblockCertificate(cert, context, fixture.seats) ==
                flowmesh::BlsCertificateCheck::BELOW_THRESHOLD);
    cert = valid;
    cert.aggregate_signature.fill(0);
    BOOST_CHECK(flowmesh::CheckBlsMicroblockCertificate(cert, context, fixture.seats) ==
                flowmesh::BlsCertificateCheck::MALFORMED_SIGNATURE);

    const bls::SecretKey outsider{Key(999, 9)};
    BOOST_CHECK(!flowmesh::SignBlsMicroblockCertificate(outsider, context, fixture.seats));
    cert = valid;
    const uint256 digest{flowmesh::FlowMeshBlsCertificateDigest(context)};
    cert.aggregate_signature =
        outsider.Sign(std::span<const unsigned char>{digest.begin(), 32}).Compressed();
    BOOST_CHECK(flowmesh::CheckBlsMicroblockCertificate(cert, context, fixture.seats) ==
                flowmesh::BlsCertificateCheck::BAD_SIGNATURE);
}

BOOST_AUTO_TEST_CASE(aggregation_rejects_bad_partial_duplicate_and_short_quorum)
{
    const SeatFixture fixture{Seats(4, 88, 5)};
    const auto context{Context(fixture)};
    const auto partials{Sign(fixture, context, {0, 1, 2})};
    flowmesh::BlsCertificateAssemblyCheck check{flowmesh::BlsCertificateAssemblyCheck::OK};

    auto short_set{partials};
    short_set.pop_back();
    BOOST_CHECK(!flowmesh::AssembleBlsMicroblockCertificate(
        context, fixture.seats, short_set, check));
    BOOST_CHECK(check == flowmesh::BlsCertificateAssemblyCheck::BELOW_THRESHOLD);

    auto duplicate{partials};
    duplicate[2].seat_index = duplicate[1].seat_index;
    BOOST_CHECK(!flowmesh::AssembleBlsMicroblockCertificate(
        context, fixture.seats, duplicate, check));
    BOOST_CHECK(check == flowmesh::BlsCertificateAssemblyCheck::DUPLICATE_SEAT_INDEX);

    auto out_of_range{partials};
    out_of_range[2].seat_index = 4;
    BOOST_CHECK(!flowmesh::AssembleBlsMicroblockCertificate(
        context, fixture.seats, out_of_range, check));
    BOOST_CHECK(check == flowmesh::BlsCertificateAssemblyCheck::SEAT_INDEX_OUT_OF_RANGE);

    const bls::SecretKey outsider{Key(777, 3)};
    auto bad{partials};
    const uint256 digest{flowmesh::FlowMeshBlsCertificateDigest(context)};
    bad[1].signature = outsider.Sign(std::span<const unsigned char>{digest.begin(), 32});
    BOOST_CHECK(!flowmesh::AssembleBlsMicroblockCertificate(
        context, fixture.seats, bad, check));
    BOOST_CHECK(check == flowmesh::BlsCertificateAssemblyCheck::BAD_PARTIAL_SIGNATURE);
}

BOOST_AUTO_TEST_SUITE_END()
