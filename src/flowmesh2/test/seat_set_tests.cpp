// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <crypto/bls.h>
#include <crypto/hex_base.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/test/util/keys.h>
#include <serialize.h>

#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;

namespace {

template <class T>
void CheckError(const Result<T>& result, ErrorCode expected)
{
    BOOST_REQUIRE_MESSAGE(!result.has_value(), "unexpected success");
    BOOST_CHECK_EQUAL(ToString(result.error().code), ToString(expected));
}

std::string SeatLabel(size_t i) { return "fm2/test/seat" + std::to_string(i); }

SeatRegistration Registration(size_t i)
{
    const bls::SecretKey key{BlsKeyFromLabel(SeatLabel(i))};
    return SeatRegistration{key.GetPublicKey(), key.SignPoP()};
}

std::vector<SeatRegistration> Registrations(size_t count)
{
    std::vector<SeatRegistration> out;
    for (size_t i{0}; i < count; ++i) out.push_back(Registration(i));
    return out;
}

std::string KeyHex(const bls::PublicKey& key) { return HexStr(key.Compressed()); }

} // namespace

BOOST_AUTO_TEST_SUITE(seat_set_tests)

BOOST_AUTO_TEST_CASE(quorum_table_k_1_to_10)
{
    // floor(2k/3) + 1.
    const std::vector<size_t> expected{1, 2, 3, 3, 4, 5, 5, 6, 7, 7};
    const auto registrations{Registrations(10)};
    for (size_t k{1}; k <= 10; ++k) {
        BOOST_CHECK_EQUAL(QuorumFor(k), expected[k - 1]);
        const auto set{SeatSet::FromPoP(std::span{registrations}.first(k))};
        BOOST_REQUIRE(set.has_value());
        BOOST_CHECK_EQUAL(set->Size(), k);
        BOOST_CHECK_EQUAL(set->Quorum(), expected[k - 1]);
    }
    BOOST_CHECK_EQUAL(QuorumFor(0), 0U);
    BOOST_CHECK_EQUAL(QuorumFor(64), 43U);
    BOOST_CHECK_EQUAL(QuorumFor(MAX_SEATS), 683U);
    // Never below a strict two-thirds majority, never above k.
    for (size_t k{1}; k <= MAX_SEATS; ++k) {
        BOOST_CHECK(3 * QuorumFor(k) > 2 * k);
        BOOST_CHECK(QuorumFor(k) <= k);
    }
}

BOOST_AUTO_TEST_CASE(root_golden_and_index_order)
{
    // The compressed keys come from the repository's BLS KeyGen (bls_tests
    // covers it); the root over them is checked by evidence/R4/golden.py.
    const auto registrations{Registrations(4)};
    BOOST_CHECK_EQUAL(KeyHex(registrations[0].key),
                      "ad7fe2adb8a6af415613f24a8c01d1e8ca6e0e0f9d3815907ee4c4ec53ceaa8ec01537fee972b37bd5ffa300c053c3e0");
    BOOST_CHECK_EQUAL(KeyHex(registrations[1].key),
                      "883aa5e3d75607062efe8a6bbafe390e5ff192930f083cf00009dbac2c6215f51a7d6798d47e0a8fa204caff63971e0e");
    BOOST_CHECK_EQUAL(KeyHex(registrations[2].key),
                      "90ed82888d2654721c06ad41a11f7efbec9cf207cb440d0e5c6ade6202cedae78dfba781833c5f20284dc935437ae85b");
    BOOST_CHECK_EQUAL(KeyHex(registrations[3].key),
                      "80572ad09ad687dea2e4b48eee2f1576abd88280cdc9a4e6f71aec756b9681796f4b1ac10ea7676aed2c5e6d448caf21");

    const auto one{SeatSet::FromPoP(std::span{registrations}.first(1)).value()};
    const auto four{SeatSet::FromPoP(registrations).value()};
    BOOST_CHECK_EQUAL(HexStr(one.Root()), "03bef7792ef0d50e748357f7df2a07c7841fdc23c8c6f479bd7810346efd487a");
    BOOST_CHECK_EQUAL(HexStr(four.Root()), "e7728b1a21d91ea35a41b73fd424d82c504acee61a7ac20890f54267c9ff4867");

    // The root is Tagged(SEATSET, CompactSize(k) || keys) and binds the order.
    HashWriter writer{TaggedWriter(HashTag::SEATSET)};
    WriteCompactSize(writer, 4);
    for (const auto& r : registrations) writer.write(std::as_bytes(std::span{r.key.Compressed()}));
    BOOST_CHECK(four.Root() == writer.GetSHA256());
    const std::vector<SeatRegistration> swapped{registrations[1], registrations[0], registrations[2], registrations[3]};
    BOOST_CHECK(SeatSet::FromPoP(swapped).value().Root() != four.Root());
    for (size_t i{0}; i < 4; ++i) BOOST_CHECK(four.Keys()[i].Key() == registrations[i].key);
}

BOOST_AUTO_TEST_CASE(from_pop_refuses_a_bad_pop)
{
    auto registrations{Registrations(3)};
    // Seat 1 presents seat 2's proof of possession.
    registrations[1].pop = registrations[2].pop;
    CheckError(SeatSet::FromPoP(registrations), ErrorCode::VERIFY_FAILED);

    // A signature under the signing DST over the key bytes is not a PoP.
    const bls::SecretKey key{BlsKeyFromLabel(SeatLabel(0))};
    const auto pk_bytes{key.GetPublicKey().Compressed()};
    const auto wrong_dst{key.Sign(std::span<const unsigned char>{pk_bytes.data(), 32})};
    const std::vector<SeatRegistration> forged{SeatRegistration{key.GetPublicKey(), wrong_dst}};
    CheckError(SeatSet::FromPoP(forged), ErrorCode::VERIFY_FAILED);
}

BOOST_AUTO_TEST_CASE(from_pop_refuses_a_duplicate_key_empty_and_oversize)
{
    const auto registrations{Registrations(2)};
    // Both registrations of seat 0 carry a valid PoP; the key may still appear once only.
    const std::vector<SeatRegistration> duplicate{registrations[0], registrations[1], registrations[0]};
    CheckError(SeatSet::FromPoP(duplicate), ErrorCode::INVALID_INPUT);

    CheckError(SeatSet::FromPoP(std::vector<SeatRegistration>{}), ErrorCode::INVALID_INPUT);
    const std::vector<SeatRegistration> too_many(MAX_SEATS + 1, registrations[0]);
    CheckError(SeatSet::FromPoP(too_many), ErrorCode::OVERSIZE);
}

BOOST_AUTO_TEST_CASE(keys_follow_the_certificate_bitmap)
{
    // Seats 0, 2 and 3 of 4 sign one digest; the bitmap selects exactly their
    // keys and the aggregate verifies over them. Full certificate checking is
    // b3verify (R11); this shows the seat index is the bitmap index.
    const auto registrations{Registrations(4)};
    const SeatSet set{SeatSet::FromPoP(registrations).value()};
    const uint256 digest{LabelHash("fm2/test/commit-digest")};
    const std::span<const unsigned char> message{digest.begin(), digest.end()};
    std::vector<bls::Signature> signatures;
    for (const size_t i : {0U, 2U, 3U}) signatures.push_back(BlsKeyFromLabel(SeatLabel(i)).Sign(message));
    const std::optional<bls::Signature> aggregate{bls::AggregateSignatures(signatures)};
    BOOST_REQUIRE(aggregate.has_value());

    SlotCertificate cert;
    cert.bitmap = SlotCertificate::MakeBitmap(set.Size(), std::vector<uint32_t>{0, 2, 3}).value();
    BOOST_CHECK_EQUAL(HexStr(cert.bitmap), "0d");
    const auto signers{cert.SignerIndices(set.Size()).value()};
    BOOST_REQUIRE(signers.size() >= set.Quorum());
    std::vector<bls::VerifiedPublicKey> keys;
    for (const uint32_t i : signers) keys.push_back(set.Keys()[i]);
    BOOST_CHECK(bls::FastAggregateVerify(keys, message, *aggregate));

    // The wrong subset does not verify.
    std::vector<bls::VerifiedPublicKey> wrong{set.Keys()[0], set.Keys()[1], set.Keys()[3]};
    BOOST_CHECK(!bls::FastAggregateVerify(wrong, message, *aggregate));
}

BOOST_AUTO_TEST_SUITE_END()
