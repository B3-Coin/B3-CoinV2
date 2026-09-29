// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/seat_evidence.h>

#include <flowmesh/bls_certificate.h>
#include <flowmesh/seat_id.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
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
    for (size_t i{0}; i < ikm.size(); ++i) ikm[i] = static_cast<unsigned char>(salt + index * 17 + i * 11);
    const auto key{bls::SecretKey::FromIKM(ikm)};
    BOOST_REQUIRE(key.has_value());
    return *key;
}

struct SeatFixture {
    std::vector<bls::SecretKey> secrets;
    flowmesh::ActiveFnBlsSeatSet seats;
};

SeatFixture Seats(const uint256& domain, const flowmesh::MarketId& market, const size_t count,
                  const uint64_t epoch, const unsigned char salt)
{
    struct Entry {
        bls::SecretKey secret;
        flowmesh::BlsSeatBinding binding;
        flowmesh::SeatId seat_id;
    };
    std::vector<Entry> entries;
    for (size_t i{0}; i < count; ++i) {
        const bls::SecretKey secret{Key(i, salt)};
        flowmesh::BlsSeatBinding binding;
        binding.outpoint = COutPoint{Txid::FromUint256(Filled(static_cast<unsigned char>(salt + i + 30))),
                                     static_cast<uint32_t>(salt + i)};
        binding.public_key = secret.GetPublicKey().Compressed();
        binding.proof_of_possession = secret.SignPoP().Compressed();
        entries.push_back({secret, binding, flowmesh::ComputeFlowMeshSeatId(domain, binding.outpoint)});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.seat_id < b.seat_id || (a.seat_id == b.seat_id && a.binding.outpoint < b.binding.outpoint);
    });
    std::vector<flowmesh::BlsSeatBinding> bindings;
    SeatFixture out;
    for (const Entry& entry : entries) {
        out.secrets.push_back(entry.secret);
        bindings.push_back(entry.binding);
    }
    flowmesh::BlsSeatSetCheck check;
    const auto seats{flowmesh::BuildActiveFnBlsSeatSet(domain, market, epoch, 100 + epoch, Filled(static_cast<unsigned char>(0x70 + salt)),
                                                       bindings, check)};
    BOOST_REQUIRE(seats.has_value());
    out.seats = *seats;
    return out;
}

struct EvidenceFixture {
    uint256 domain{Filled(0x11)};
    flowmesh::MarketId market{Filled(0x22)};
    SeatFixture seats{Seats(domain, market, 4, 7, 1)};
    SeatFixture other_seats{Seats(domain, market, 4, 8, 9)};

    flowmesh::BlsCertificateContext Context(const uint256& hash, const uint64_t sequence = 5) const
    {
        return {domain, market, seats.seats.epoch, seats.seats.set_hash, sequence, hash};
    }
    bls::Signature Sign(const uint32_t seat, const flowmesh::BlsCertificateContext& context) const
    {
        const auto signature{flowmesh::SignBlsMicroblockCertificate(seats.secrets[seat], context, seats.seats)};
        BOOST_REQUIRE(signature);
        return *signature;
    }
    flowmesh::SeatEvidence Equivocation(const uint32_t seat = 2) const
    {
        const auto a{Context(Filled(0xa1))};
        const auto b{Context(Filled(0xb2))};
        return {flowmesh::SeatEvidenceKind::ATTESTATION_EQUIVOCATION, domain, market,
                flowmesh::SeatAttestationStatement(a, seat, Sign(seat, a)),
                flowmesh::SeatAttestationStatement(b, seat, Sign(seat, b))};
    }
    flowmesh::BlsMicroblockCertificate Certify(const flowmesh::BlsCertificateContext& context,
                                               const std::vector<uint32_t>& signers) const
    {
        std::vector<flowmesh::IndexedBlsSignature> partials;
        for (const uint32_t seat : signers) partials.push_back({seat, Sign(seat, context)});
        flowmesh::BlsCertificateAssemblyCheck check;
        const auto certificate{flowmesh::AssembleBlsMicroblockCertificate(context, seats.seats, partials, check)};
        BOOST_REQUIRE(certificate);
        return *certificate;
    }
    flowmesh::SeatEvidence Conflict() const
    {
        return {flowmesh::SeatEvidenceKind::CERTIFICATE_CONFLICT, domain, market,
                flowmesh::SeatCertificateStatement(Certify(Context(Filled(0xa1)), {0, 1, 2}), seats.seats.set_hash),
                flowmesh::SeatCertificateStatement(Certify(Context(Filled(0xb2)), {1, 2, 3}), seats.seats.set_hash)};
    }
    flowmesh::SeatEvidenceCheck Check(const flowmesh::SeatEvidence& evidence) const
    {
        return flowmesh::CheckSeatEvidence(evidence, seats.seats, seats.seats);
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_seat_evidence_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(attestation_equivocation_is_self_verifiable)
{
    EvidenceFixture f;
    const auto evidence{f.Equivocation()};
    BOOST_CHECK(f.Check(evidence) == flowmesh::SeatEvidenceCheck::OK);
    BOOST_CHECK(flowmesh::SeatEvidenceDoubleSigners(evidence).empty());
    // The identity ignores which statement a node happened to hold first.
    auto swapped{evidence};
    std::swap(swapped.first, swapped.second);
    BOOST_CHECK(swapped.Id() == evidence.Id());
    BOOST_CHECK(f.Check(swapped) == flowmesh::SeatEvidenceCheck::OK);

    // The same microblock twice, another sequence, another seat or another
    // epoch is no equivocation.
    auto same{evidence};
    same.second = same.first;
    BOOST_CHECK(f.Check(same) == flowmesh::SeatEvidenceCheck::NOT_CONFLICTING);
    auto later{evidence};
    const auto later_context{f.Context(Filled(0xb2), 6)};
    later.second = flowmesh::SeatAttestationStatement(later_context, 2, f.Sign(2, later_context));
    BOOST_CHECK(f.Check(later) == flowmesh::SeatEvidenceCheck::NOT_CONFLICTING);
    auto two_seats{evidence};
    const auto b{f.Context(Filled(0xb2))};
    two_seats.second = flowmesh::SeatAttestationStatement(b, 1, f.Sign(1, b));
    BOOST_CHECK(f.Check(two_seats) == flowmesh::SeatEvidenceCheck::NOT_CONFLICTING);
    auto other_epoch{evidence};
    other_epoch.second.seat_epoch = f.other_seats.seats.epoch;
    other_epoch.second.seat_set_hash = f.other_seats.seats.set_hash;
    BOOST_CHECK(f.Check(other_epoch) == flowmesh::SeatEvidenceCheck::NOT_CONFLICTING);

    // The checker uses the caller's chain-derived sets, never the record's.
    BOOST_CHECK(flowmesh::CheckSeatEvidence(evidence, f.other_seats.seats, f.seats.seats) ==
                flowmesh::SeatEvidenceCheck::WRONG_SEAT_SET);
    // A signature by another seat, or over another microblock, fails.
    auto forged_first{evidence};
    forged_first.first.signature = f.Sign(1, f.Context(Filled(0xa1))).Compressed();
    BOOST_CHECK(f.Check(forged_first) == flowmesh::SeatEvidenceCheck::BAD_FIRST_SIGNATURE);
    auto forged_second{evidence};
    forged_second.second.microblock_hash = Filled(0xc3);
    BOOST_CHECK(f.Check(forged_second) == flowmesh::SeatEvidenceCheck::BAD_SECOND_SIGNATURE);
    auto out_of_range{evidence};
    out_of_range.first.seat_index = out_of_range.second.seat_index = 4;
    BOOST_CHECK(f.Check(out_of_range) == flowmesh::SeatEvidenceCheck::BAD_FIRST_SIGNATURE);
    // An attestation carries no bitmap.
    auto shaped{evidence};
    shaped.first.signer_bitmap = {0x07};
    BOOST_CHECK(f.Check(shaped) == flowmesh::SeatEvidenceCheck::MALFORMED);
    BOOST_CHECK(!flowmesh::EncodeSeatEvidence(shaped));
}

BOOST_AUTO_TEST_CASE(certificate_conflict_is_self_verifiable_and_names_double_signers)
{
    EvidenceFixture f;
    const auto evidence{f.Conflict()};
    BOOST_CHECK(f.Check(evidence) == flowmesh::SeatEvidenceCheck::OK);
    // Seats 1 and 2 are in both quorums: each signed both microblocks.
    BOOST_CHECK((flowmesh::SeatEvidenceDoubleSigners(evidence) == std::vector<uint32_t>{1, 2}));

    auto tampered{evidence};
    tampered.second.signer_bitmap = {0x0f};
    BOOST_CHECK(f.Check(tampered) == flowmesh::SeatEvidenceCheck::BAD_SECOND_SIGNATURE);
    auto below_quorum{evidence};
    below_quorum.first.signer_bitmap = {0x03};
    BOOST_CHECK(f.Check(below_quorum) == flowmesh::SeatEvidenceCheck::BAD_FIRST_SIGNATURE);
    auto same{evidence};
    same.second = same.first;
    BOOST_CHECK(f.Check(same) == flowmesh::SeatEvidenceCheck::NOT_CONFLICTING);
    // A certificate statement names no single seat.
    auto shaped{evidence};
    shaped.first.seat_index = 1;
    BOOST_CHECK(f.Check(shaped) == flowmesh::SeatEvidenceCheck::MALFORMED);
    auto empty{evidence};
    empty.second.signer_bitmap.clear();
    BOOST_CHECK(f.Check(empty) == flowmesh::SeatEvidenceCheck::MALFORMED);
}

BOOST_AUTO_TEST_CASE(encoding_is_canonical_and_strict)
{
    EvidenceFixture f;
    for (const auto& evidence : {f.Equivocation(), f.Conflict()}) {
        const auto bytes{flowmesh::EncodeSeatEvidence(evidence)};
        BOOST_REQUIRE(bytes);
        BOOST_CHECK_LE(bytes->size(), flowmesh::SEAT_EVIDENCE_MAX_BYTES);
        const auto decoded{flowmesh::DecodeSeatEvidence(*bytes)};
        BOOST_REQUIRE(decoded);
        BOOST_CHECK(*decoded == evidence);
        BOOST_CHECK(decoded->Id() == evidence.Id());
        BOOST_CHECK(f.Check(*decoded) == flowmesh::SeatEvidenceCheck::OK);

        auto truncated{*bytes};
        truncated.pop_back();
        BOOST_CHECK(!flowmesh::DecodeSeatEvidence(truncated));
        auto extended{*bytes};
        extended.push_back(0);
        BOOST_CHECK(!flowmesh::DecodeSeatEvidence(extended));
        auto version{*bytes};
        version[0] = flowmesh::SEAT_EVIDENCE_VERSION + 1;
        BOOST_CHECK(!flowmesh::DecodeSeatEvidence(version));
        auto kind{*bytes};
        kind[1] = 3;
        BOOST_CHECK(!flowmesh::DecodeSeatEvidence(kind));
        // Kinds are not interchangeable: each has its own statement shape.
        auto other_kind{*bytes};
        other_kind[1] = evidence.kind == flowmesh::SeatEvidenceKind::CERTIFICATE_CONFLICT ? 1 : 2;
        BOOST_CHECK(!flowmesh::DecodeSeatEvidence(other_kind));
    }
    BOOST_CHECK(!flowmesh::DecodeSeatEvidence({}));
    // A bitmap larger than the largest seat set is refused before it is read.
    auto oversized{f.Conflict()};
    oversized.first.signer_bitmap.assign(flowmesh::FLOWMESH_MAX_SIGNER_BITMAP_BYTES + 1, 0xff);
    BOOST_CHECK(!flowmesh::EncodeSeatEvidence(oversized));
    auto null_market{f.Equivocation()};
    null_market.market_id.SetNull();
    BOOST_CHECK(!flowmesh::EncodeSeatEvidence(null_market));
    BOOST_CHECK(f.Check(null_market) == flowmesh::SeatEvidenceCheck::MALFORMED);
}

BOOST_AUTO_TEST_SUITE_END()
