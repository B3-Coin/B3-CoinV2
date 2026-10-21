// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <crypto/hex_base.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/test/util/keys.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
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

std::string BitmapHex(size_t seat_count, std::vector<uint32_t> signers)
{
    return HexStr(SlotCertificate::MakeBitmap(seat_count, signers).value());
}

//! The golden certificate of evidence/R4/golden.py.
SlotCertificate Sample()
{
    SlotCertificate cert;
    cert.header_hash = LabelHash("fm2/test/header");
    cert.bitmap = SlotCertificate::MakeBitmap(10, std::vector<uint32_t>{0, 2, 9}).value();
    for (size_t i{0}; i < cert.aggregate.size(); ++i) cert.aggregate[i] = static_cast<unsigned char>(i);
    return cert;
}

} // namespace

BOOST_AUTO_TEST_SUITE(certificate_tests)

BOOST_AUTO_TEST_CASE(bitmap_golden_lsb_first)
{
    BOOST_CHECK_EQUAL(BitmapHex(1, {0}), "01");
    BOOST_CHECK_EQUAL(BitmapHex(10, {0, 2, 9}), "0502");
    BOOST_CHECK_EQUAL(BitmapHex(10, {9, 2, 0}), "0502"); // order of the input does not matter
    BOOST_CHECK_EQUAL(BitmapHex(16, {7, 8, 15}), "8081");
    BOOST_CHECK_EQUAL(BitmapHex(17, {16}), "000001");
    BOOST_CHECK_EQUAL(BitmapHex(8, {}), "00");
    BOOST_CHECK_EQUAL(BitmapBytesFor(1), 1U);
    BOOST_CHECK_EQUAL(BitmapBytesFor(8), 1U);
    BOOST_CHECK_EQUAL(BitmapBytesFor(9), 2U);
    BOOST_CHECK_EQUAL(MAX_BITMAP_BYTES, 128U);
}

BOOST_AUTO_TEST_CASE(signer_indices_round_trip_exhaustive_to_k_10)
{
    for (size_t k{1}; k <= 10; ++k) {
        for (uint32_t mask{0}; mask < (1U << k); ++mask) {
            std::vector<uint32_t> signers;
            for (uint32_t i{0}; i < k; ++i) {
                if (mask & (1U << i)) signers.push_back(i);
            }
            SlotCertificate cert;
            cert.bitmap = SlotCertificate::MakeBitmap(k, signers).value();
            const auto back{cert.SignerIndices(k)};
            BOOST_REQUIRE(back.has_value());
            BOOST_CHECK(*back == signers);
        }
    }
}

BOOST_AUTO_TEST_CASE(bits_at_or_above_k_refused)
{
    for (size_t k{1}; k <= 24; ++k) {
        const size_t bytes{BitmapBytesFor(k)};
        for (size_t bit{k}; bit < bytes * 8; ++bit) {
            SlotCertificate cert;
            cert.bitmap.assign(bytes, 0);
            cert.bitmap[bit / 8] = static_cast<unsigned char>(1U << (bit % 8));
            CheckError(cert.SignerIndices(k), ErrorCode::INVALID_INPUT);
        }
        // Wrong length: one byte short (when possible) and one byte long.
        SlotCertificate longer;
        longer.bitmap.assign(bytes + 1, 0);
        CheckError(longer.SignerIndices(k), ErrorCode::INVALID_INPUT);
        if (bytes > 1) {
            SlotCertificate shorter;
            shorter.bitmap.assign(bytes - 1, 0);
            CheckError(shorter.SignerIndices(k), ErrorCode::INVALID_INPUT);
        }
        CheckError(SlotCertificate::MakeBitmap(k, std::vector<uint32_t>{static_cast<uint32_t>(k)}), ErrorCode::INVALID_INPUT);
    }
    SlotCertificate empty;
    CheckError(empty.SignerIndices(0), ErrorCode::INVALID_INPUT);
    CheckError(empty.SignerIndices(MAX_SEATS + 1), ErrorCode::INVALID_INPUT);
    CheckError(SlotCertificate::MakeBitmap(0, std::vector<uint32_t>{}), ErrorCode::INVALID_INPUT);
    CheckError(SlotCertificate::MakeBitmap(MAX_SEATS + 1, std::vector<uint32_t>{}), ErrorCode::INVALID_INPUT);
    CheckError(SlotCertificate::MakeBitmap(4, std::vector<uint32_t>{1, 1}), ErrorCode::INVALID_INPUT);
    // The largest set works at both ends.
    const auto full{SlotCertificate::MakeBitmap(MAX_SEATS, std::vector<uint32_t>{0, MAX_SEATS - 1}).value()};
    BOOST_CHECK_EQUAL(full.size(), MAX_BITMAP_BYTES);
    SlotCertificate max_cert;
    max_cert.bitmap = full;
    BOOST_CHECK(max_cert.SignerIndices(MAX_SEATS).value() == (std::vector<uint32_t>{0, MAX_SEATS - 1}));
}

BOOST_AUTO_TEST_CASE(golden_codec_and_hash)
{
    // Goldens from evidence/R4/golden.py (independent of the C++ code).
    const SlotCertificate cert{Sample()};
    BOOST_CHECK_EQUAL(cert.Encode().size(), 131U);
    BOOST_CHECK_EQUAL(HexStr(cert.Encode()),
                      "790f0bba29f4981147021667ddc4d0b7de7661161fd2e8dd03b340903afac46a020502000102030405060708090a0b0c"
                      "0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c"
                      "3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f");
    BOOST_CHECK_EQUAL(HexStr(cert.Hash()), "3558a6216aebbf6501c1766eba167a6d4db337a95e75630b3cfe099a6de7d6cd");
    BOOST_CHECK(cert.Hash() == Tagged(HashTag::CERT, cert.Encode()));

    const auto decoded{SlotCertificate::Decode(cert.Encode())};
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK(*decoded == cert);
}

BOOST_AUTO_TEST_CASE(decode_refusals_and_field_sensitivity)
{
    const SlotCertificate cert{Sample()};
    auto trailing{cert.Encode()};
    trailing.push_back(0x00);
    CheckError(SlotCertificate::Decode(trailing), ErrorCode::DECODE);
    const auto bytes{cert.Encode()};
    const std::vector<unsigned char> truncated(bytes.begin(), bytes.end() - 1);
    CheckError(SlotCertificate::Decode(truncated), ErrorCode::DECODE);

    SlotCertificate at_cap{cert};
    at_cap.bitmap.assign(MAX_BITMAP_BYTES, 0xff);
    BOOST_CHECK_EQUAL(at_cap.Encode().size(), SlotCertificate::MAX_LEGACY_ENCODED_BYTES);
    BOOST_CHECK(SlotCertificate::Decode(at_cap.Encode()).has_value());
    SlotCertificate over{cert};
    over.bitmap.assign(MAX_BITMAP_BYTES + 1, 0xff);
    CheckError(SlotCertificate::Decode(over.Encode()), ErrorCode::OVERSIZE);
    // A bitmap count above the cap with nothing behind it: OVERSIZE before allocation.
    std::vector<unsigned char> count_only(bytes.begin(), bytes.begin() + 32);
    count_only.push_back(0xfd);
    count_only.push_back(0x00);
    count_only.push_back(0x01); // 256
    CheckError(SlotCertificate::Decode(count_only), ErrorCode::OVERSIZE);

    std::set<uint256> hashes{cert.Hash()};
    SlotCertificate changed{cert};
    changed.header_hash.data()[0] ^= 1;
    BOOST_CHECK(hashes.insert(changed.Hash()).second);
    changed = cert;
    changed.bitmap[1] ^= 1;
    BOOST_CHECK(hashes.insert(changed.Hash()).second);
    changed = cert;
    changed.aggregate[95] ^= 1;
    BOOST_CHECK(hashes.insert(changed.Hash()).second);
}


BOOST_AUTO_TEST_CASE(view_extension_is_exact_and_cannot_downgrade_header_policy)
{
    const auto legacy{Sample()};
    const auto old_bytes{legacy.Encode()};
    for (const uint64_t view : {uint64_t{0}, uint64_t{7}, std::numeric_limits<uint64_t>::max()}) {
        auto cert{legacy}; cert.view = view;
        const auto bytes{cert.Encode()};
        BOOST_CHECK_EQUAL(bytes.size(), old_bytes.size() + 8);
        BOOST_CHECK(std::equal(old_bytes.begin(), old_bytes.end(), bytes.begin()));
        const auto decoded{SlotCertificate::Decode(bytes)};
        BOOST_REQUIRE(decoded.has_value());
        BOOST_CHECK(*decoded == cert);
        BOOST_CHECK(cert.Hash() != legacy.Hash());
    }
    for (size_t count = 1; count <= 9; ++count) {
        auto bytes{old_bytes}; bytes.insert(bytes.end(), count, 0);
        if (count == 8) BOOST_CHECK(SlotCertificate::Decode(bytes).value().view == std::optional<uint64_t>{0});
        else CheckError(SlotCertificate::Decode(bytes), ErrorCode::DECODE);
    }
    auto cap{legacy}; cap.bitmap.assign(MAX_BITMAP_BYTES, 0xff); cap.view = 0;
    BOOST_CHECK_EQUAL(cap.Encode().size(), SlotCertificate::MAX_ENCODED_BYTES);
    BOOST_CHECK(SlotCertificate::Decode(cap.Encode()).value() == cap);
    SlotHeaderV2 header; header.commitment_version = COMMITMENT_VERSION_VIEW_BOUND;
    CheckError(CertificateDigest(header, legacy), ErrorCode::VERIFY_FAILED);
    auto cert{legacy}; cert.view = 0;
    BOOST_CHECK(CertificateDigest(header, cert).value() == VersionedCommitDigest(header, 0));
    auto stripped{cert.Encode()}; stripped.resize(stripped.size() - 8);
    CheckError(CertificateDigest(header, SlotCertificate::Decode(stripped).value()), ErrorCode::VERIFY_FAILED);
    for (const uint8_t version : {COMMITMENT_VERSION_V0, COMMITMENT_VERSION_WITHDRAWALS}) {
        header.commitment_version = version;
        CheckError(CertificateDigest(header, cert), ErrorCode::VERIFY_FAILED);
        BOOST_CHECK(CertificateDigest(header, legacy).value() == header.CommitDigest());
    }
    header.commitment_version = 1;
    CheckError(CertificateDigest(header, legacy), ErrorCode::VERIFY_FAILED);
}

BOOST_AUTO_TEST_SUITE_END()
