// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <crypto/common.h>
#include <flowmesh2/storage/record.h>
#include <flowmesh2/test/util/storage_fixtures.h>
#include <util/strencodings.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::storage;
using namespace flowmesh2::test;

namespace {

//! Records `types[i]` with payload "p<i>", framed one after the other.
std::vector<unsigned char> Image(JournalRole role, uint64_t instance, const std::vector<RecordType>& types)
{
    std::vector<unsigned char> image;
    uint256 prev{JournalGenesisChain(role, instance)};
    for (size_t i{0}; i < types.size(); ++i) {
        const Result<EncodedRecord> rec{EncodeRecord(prev, types[i], Bytes("p" + std::to_string(i)))};
        BOOST_REQUIRE(rec.has_value());
        image.insert(image.end(), rec->bytes.begin(), rec->bytes.end());
        prev = rec->chain;
    }
    return image;
}

//! Byte offset of record `index` (0-based) in an image made by Image() with payloads "p0".."p9".
size_t OffsetOf(size_t index) { return index * (RECORD_OVERHEAD_BYTES + 2); }

} // namespace

BOOST_AUTO_TEST_SUITE(record_tests)

BOOST_AUTO_TEST_CASE(registry_pinned)
{
    // APPEND-ONLY registry (ARCHITECTURE.md 7.1): integers and names pinned.
    const std::vector<std::pair<int, std::string>> expected{
        {1, "INTENT"}, {2, "SIGNED_VOTE"}, {3, "DECISION"}, {4, "APPLIED"}, {5, "SNAPSHOT_MARK"},
        {6, "ACCEPTED_PROPOSAL"}, {7, "VIEW_STATE"}, {8, "NEW_VIEW_ACCEPTED"}, {9, "PREPARED_QC"}, {10, "CONTROL_INTENT"}, {11, "SIGNED_CONTROL"}};
    BOOST_REQUIRE_EQUAL(RecordTypeRegistry().size(), expected.size());
    for (size_t i{0}; i < expected.size(); ++i) {
        BOOST_CHECK_EQUAL(static_cast<int>(RecordTypeRegistry()[i].type), expected[i].first);
        BOOST_CHECK_EQUAL(std::string{RecordTypeRegistry()[i].name}, expected[i].second);
        BOOST_CHECK(IsKnown(RecordTypeRegistry()[i].type));
        BOOST_CHECK_EQUAL(std::string{ToString(RecordTypeRegistry()[i].type)}, expected[i].second);
    }
    BOOST_CHECK(!IsKnown(RecordType{0}));
    BOOST_CHECK(!IsKnown(RecordType{12}));
    BOOST_CHECK_EQUAL(std::string{ToString(RecordType{12})}, "UNKNOWN");
    BOOST_CHECK_EQUAL(static_cast<int>(JournalRole::SIGNER), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(JournalRole::COMMIT), 2);
    BOOST_CHECK(!IsKnown(JournalRole{0}));
    BOOST_CHECK(!IsKnown(JournalRole{3}));
    BOOST_CHECK_EQUAL(std::string{ToString(JournalRole{3})}, "UNKNOWN");
}

BOOST_AUTO_TEST_CASE(role_acceptance_table)
{
    // SIGNER holds {1, 2, 6, 7, 8, 10, 11}; COMMIT holds {3, 4, 5, 9}; nothing else.
    for (int t{0}; t <= 255; ++t) {
        const RecordType type{static_cast<RecordType>(t)};
        const bool signer{t == 1 || t == 2 || t == 6 || t == 7 || t == 8 || t == 10 || t == 11};
        const bool commit{t == 3 || t == 4 || t == 5 || t == 9};
        BOOST_CHECK_EQUAL(RoleAccepts(JournalRole::SIGNER, type), signer);
        BOOST_CHECK_EQUAL(RoleAccepts(JournalRole::COMMIT, type), commit);
        BOOST_CHECK(!RoleAccepts(JournalRole{0}, type));
        BOOST_CHECK(!RoleAccepts(JournalRole{3}, type));
    }
}

BOOST_AUTO_TEST_CASE(framing_golden)
{
    // Goldens from the independent stdlib script evidence/R8/golden.py.
    BOOST_CHECK_EQUAL(HexStr(JournalGenesisChain(JournalRole::SIGNER, 7)),
                      "5d8f5f3b043c752c5edd175a47530789701f1bd3ef5d562c8c72a500f5e1356f");
    BOOST_CHECK_EQUAL(HexStr(JournalGenesisChain(JournalRole::COMMIT, 7)),
                      "7b0783a7488b434227836030fce6e470c69bf322670e5452dd81abf418623f64");
    BOOST_CHECK_EQUAL(HexStr(JournalGenesisChain(JournalRole::SIGNER, 0x0102030405060708)),
                      "d7c7d682307feb0e5a35dc7568df82ea16b2a4b6d6db2d9d6e882d0300531b70");

    const uint256 genesis{JournalGenesisChain(JournalRole::COMMIT, 7)};
    const Result<EncodedRecord> r1{EncodeRecord(genesis, RecordType::APPLIED, Bytes("fm2/test/payload"))};
    BOOST_REQUIRE(r1.has_value());
    BOOST_CHECK_EQUAL(HexStr(r1->bytes),
                      "100000008a0d76c804666d322f746573742f7061796c6f6164"
                      "f4abb163342389988eac3c87774705927d661f36f714aecca9b6ab51cfbbf300");
    BOOST_CHECK_EQUAL(HexStr(r1->chain), "f4abb163342389988eac3c87774705927d661f36f714aecca9b6ab51cfbbf300");
    const Result<EncodedRecord> r2{EncodeRecord(r1->chain, RecordType::DECISION, {})};
    BOOST_REQUIRE(r2.has_value());
    BOOST_CHECK_EQUAL(HexStr(r2->bytes),
                      "00000000fce2e79903039f3bae73bb1bab1a49ae5e5730e3fe08e8003ebe3a0cedbf96480fc1c86197");
    BOOST_CHECK_EQUAL(r2->bytes.size(), RECORD_OVERHEAD_BYTES);

    // The chain is single SHA-256 over prev || type || payload, the CRC covers type || payload || chain.
    BOOST_CHECK(NextChain(genesis, RecordType::APPLIED, Bytes("fm2/test/payload")) == r1->chain);
    BOOST_CHECK_EQUAL(RecordCrc(RecordType::APPLIED, Bytes("fm2/test/payload"), r1->chain), 0xc8760d8aU);

    std::vector<unsigned char> image{r1->bytes};
    image.insert(image.end(), r2->bytes.begin(), r2->bytes.end());
    const Result<ScanResult> scan{ScanRecords(image, JournalRole::COMMIT, 7)};
    RequireStorageOk(scan, "scan of the golden image");
    BOOST_REQUIRE_EQUAL(scan->records.size(), 2U);
    BOOST_CHECK_EQUAL(scan->records[0].seq, 1U);
    BOOST_CHECK(scan->records[0].type == RecordType::APPLIED);
    BOOST_CHECK(scan->records[0].payload == Bytes("fm2/test/payload"));
    BOOST_CHECK(scan->records[1].seq == 2U && scan->records[1].type == RecordType::DECISION && scan->records[1].payload.empty());
    BOOST_CHECK(scan->last_chain == r2->chain);
    BOOST_CHECK_EQUAL(scan->end_offset, image.size());
}

BOOST_AUTO_TEST_CASE(encode_refusals)
{
    const uint256 genesis{JournalGenesisChain(JournalRole::SIGNER, 1)};
    RequireRefusal(EncodeRecord(genesis, RecordType{0}, {}), ErrorCode::INVALID_INPUT, RECORD_TYPE, "type 0");
    RequireRefusal(EncodeRecord(genesis, RecordType{12}, {}), ErrorCode::INVALID_INPUT, RECORD_TYPE, "type 12");
    const std::vector<unsigned char> at_cap(MAX_RECORD_PAYLOAD_BYTES);
    const Result<EncodedRecord> ok{EncodeRecord(genesis, RecordType::INTENT, at_cap)};
    RequireStorageOk(ok, "payload at the cap");
    BOOST_CHECK_EQUAL(ok->bytes.size(), MAX_RECORD_PAYLOAD_BYTES + RECORD_OVERHEAD_BYTES);
    const std::vector<unsigned char> above_cap(MAX_RECORD_PAYLOAD_BYTES + 1);
    RequireRefusal(EncodeRecord(genesis, RecordType::INTENT, above_cap), ErrorCode::OVERSIZE, RECORD_LENGTH, "payload above the cap");
}

BOOST_AUTO_TEST_CASE(scan_accepts_empty_and_zero_tail)
{
    const Result<ScanResult> empty{ScanRecords({}, JournalRole::SIGNER, 3)};
    RequireStorageOk(empty, "empty image");
    BOOST_CHECK(empty->records.empty());
    BOOST_CHECK(empty->last_chain == JournalGenesisChain(JournalRole::SIGNER, 3));
    BOOST_CHECK_EQUAL(empty->end_offset, 0U);

    std::vector<unsigned char> image{Image(JournalRole::SIGNER, 3, {RecordType::ACCEPTED_PROPOSAL, RecordType::INTENT})};
    const size_t records_end{image.size()};
    // Zero tails of every length from 1 byte (shorter than a header) to 4 KiB.
    for (size_t tail : {size_t{1}, size_t{8}, size_t{9}, size_t{41}, size_t{4096}}) {
        std::vector<unsigned char> padded{image};
        padded.resize(records_end + tail, 0);
        const Result<ScanResult> scan{ScanRecords(padded, JournalRole::SIGNER, 3)};
        RequireStorageOk(scan, "image with a zero tail");
        BOOST_CHECK_EQUAL(scan->records.size(), 2U);
        BOOST_CHECK_EQUAL(scan->end_offset, records_end);
    }
    // An all-zero image (a fresh segment) holds no record.
    const Result<ScanResult> zeros{ScanRecords(std::vector<unsigned char>(65536, 0), JournalRole::COMMIT, 3)};
    RequireStorageOk(zeros, "fresh zero segment");
    BOOST_CHECK(zeros->records.empty());
    RequireCode(ScanRecords({}, JournalRole{0}, 3), ErrorCode::INVALID_INPUT, "unknown role");
}

BOOST_AUTO_TEST_CASE(scan_detects_crc_flip)
{
    const std::vector<unsigned char> image{Image(JournalRole::COMMIT, 5, {RecordType::DECISION, RecordType::APPLIED, RecordType::APPLIED})};
    // Flip every bit of every byte of the middle record in turn: each is caught.
    const size_t begin{OffsetOf(1)};
    const size_t end{OffsetOf(2)};
    for (size_t pos{begin}; pos < end; ++pos) {
        for (int bit{0}; bit < 8; ++bit) {
            std::vector<unsigned char> bad{image};
            bad[pos] ^= static_cast<unsigned char>(1 << bit);
            const Result<ScanResult> scan{ScanRecords(bad, JournalRole::COMMIT, 5)};
            BOOST_REQUIRE_MESSAGE(!scan.has_value(), "flip at byte " << pos << " bit " << bit << " was not detected");
            // Header bytes may also break the length; every other flip is a CRC mismatch.
            if (pos >= begin + 4) {
                BOOST_CHECK_MESSAGE(scan.error().code == ErrorCode::VERIFY_FAILED && scan.error().detail == RECORD_CRC,
                                    "byte " << pos << ": " << scan.error().detail);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(scan_detects_chain_break)
{
    const JournalRole role{JournalRole::COMMIT};
    const std::vector<RecordType> types{RecordType::DECISION, RecordType::APPLIED, RecordType::DECISION};
    const std::vector<unsigned char> image{Image(role, 5, types)};

    // A changed payload with a recomputed (valid) CRC: only the chain catches it.
    {
        std::vector<unsigned char> bad{image};
        const size_t rec{OffsetOf(1)};
        bad[rec + RECORD_HEADER_BYTES] ^= 0x01;
        uint256 chain;
        std::copy_n(bad.begin() + rec + RECORD_HEADER_BYTES + 2, 32, chain.begin());
        const uint32_t crc{RecordCrc(RecordType::APPLIED, std::span{bad}.subspan(rec + RECORD_HEADER_BYTES, 2), chain)};
        WriteLE32(bad.data() + rec + 4, crc);
        RequireRefusal(ScanRecords(bad, role, 5), ErrorCode::VERIFY_FAILED, RECORD_CHAIN, "payload change with a fixed CRC");
    }
    // Two records swapped.
    {
        std::vector<unsigned char> swapped(image.begin(), image.begin() + OffsetOf(0));
        swapped.insert(swapped.end(), image.begin() + OffsetOf(1), image.begin() + OffsetOf(2));
        swapped.insert(swapped.end(), image.begin() + OffsetOf(0), image.begin() + OffsetOf(1));
        swapped.insert(swapped.end(), image.begin() + OffsetOf(2), image.end());
        RequireRefusal(ScanRecords(swapped, role, 5), ErrorCode::VERIFY_FAILED, RECORD_CHAIN, "reordered records");
    }
    // The middle record dropped (a gap).
    {
        std::vector<unsigned char> gap(image.begin(), image.begin() + OffsetOf(1));
        gap.insert(gap.end(), image.begin() + OffsetOf(2), image.end());
        RequireRefusal(ScanRecords(gap, role, 5), ErrorCode::VERIFY_FAILED, RECORD_CHAIN, "dropped record");
    }
    // The right bytes scanned as another instance or another role.
    RequireRefusal(ScanRecords(image, role, 6), ErrorCode::VERIFY_FAILED, RECORD_CHAIN, "scan under another instance");
    const std::vector<unsigned char> signer_image{Image(JournalRole::SIGNER, 5, {RecordType::INTENT})};
    RequireRefusal(ScanRecords(signer_image, JournalRole::COMMIT, 5), ErrorCode::ORDER_VIOLATION, RECORD_ROLE, "signer record in a commit scan");
}

BOOST_AUTO_TEST_CASE(scan_detects_splice_from_another_instance)
{
    // Instance 5 and instance 9 write the same records. Record 2 of instance 9
    // is spliced into instance 5's image: its CRC is intact, its chain is not.
    const std::vector<RecordType> types{RecordType::DECISION, RecordType::APPLIED, RecordType::DECISION};
    const std::vector<unsigned char> a{Image(JournalRole::COMMIT, 5, types)};
    const std::vector<unsigned char> b{Image(JournalRole::COMMIT, 9, types)};
    BOOST_REQUIRE(a.size() == b.size() && a != b);
    std::vector<unsigned char> spliced(a.begin(), a.begin() + OffsetOf(1));
    spliced.insert(spliced.end(), b.begin() + OffsetOf(1), b.begin() + OffsetOf(2));
    spliced.insert(spliced.end(), a.begin() + OffsetOf(2), a.end());
    RequireRefusal(ScanRecords(spliced, JournalRole::COMMIT, 5), ErrorCode::VERIFY_FAILED, RECORD_CHAIN, "spliced record");
    // Each image on its own is fine; so is the tail of b scanned as a whole b.
    RequireStorageOk(ScanRecords(a, JournalRole::COMMIT, 5), "instance 5");
    RequireStorageOk(ScanRecords(b, JournalRole::COMMIT, 9), "instance 9");
    // Splicing b's first record in front of a also fails at once.
    std::vector<unsigned char> front(b.begin(), b.begin() + OffsetOf(1));
    front.insert(front.end(), a.begin() + OffsetOf(1), a.end());
    RequireRefusal(ScanRecords(front, JournalRole::COMMIT, 5), ErrorCode::VERIFY_FAILED, RECORD_CHAIN, "foreign first record");
}

BOOST_AUTO_TEST_CASE(scan_detects_length_type_and_tail_errors)
{
    const JournalRole role{JournalRole::SIGNER};
    const std::vector<unsigned char> image{Image(role, 2, {RecordType::INTENT, RecordType::SIGNED_VOTE})};

    // Truncated anywhere inside the last record.
    for (size_t cut{OffsetOf(1) + 1}; cut < image.size(); ++cut) {
        const std::vector<unsigned char> torn(image.begin(), image.begin() + cut);
        RequireRefusal(ScanRecords(torn, role, 2), ErrorCode::DECODE, RECORD_LENGTH, "torn record");
    }
    // A length beyond the input, and a length above the cap.
    {
        std::vector<unsigned char> bad{image};
        WriteLE32(bad.data() + OffsetOf(1), 3);
        RequireRefusal(ScanRecords(bad, role, 2), ErrorCode::DECODE, RECORD_LENGTH, "length beyond the input");
        WriteLE32(bad.data() + OffsetOf(1), static_cast<uint32_t>(MAX_RECORD_PAYLOAD_BYTES + 1));
        RequireRefusal(ScanRecords(bad, role, 2), ErrorCode::DECODE, RECORD_LENGTH, "length above the cap");
        WriteLE32(bad.data() + OffsetOf(1), 0xffffffffU);
        RequireRefusal(ScanRecords(bad, role, 2), ErrorCode::DECODE, RECORD_LENGTH, "length 2^32-1");
    }
    // An unregistered type with a valid CRC.
    {
        std::vector<unsigned char> bad{image};
        const size_t rec{OffsetOf(1)};
        bad[rec + 8] = 200;
        uint256 chain;
        std::copy_n(bad.begin() + rec + RECORD_HEADER_BYTES + 2, 32, chain.begin());
        WriteLE32(bad.data() + rec + 4, RecordCrc(RecordType{200}, std::span{bad}.subspan(rec + RECORD_HEADER_BYTES, 2), chain));
        RequireRefusal(ScanRecords(bad, role, 2), ErrorCode::DECODE, RECORD_TYPE, "unregistered type");
    }
    // A non-zero byte after the zero tail started.
    {
        std::vector<unsigned char> bad{image};
        bad.resize(image.size() + 100, 0);
        bad.back() = 1;
        RequireRefusal(ScanRecords(bad, role, 2), ErrorCode::DECODE, RECORD_TAIL, "garbage after the zero tail");
        std::vector<unsigned char> short_tail{image};
        short_tail.insert(short_tail.end(), {0, 0, 0, 1});
        RequireRefusal(ScanRecords(short_tail, role, 2), ErrorCode::DECODE, RECORD_LENGTH, "non-zero bytes shorter than a header");
    }
}

BOOST_AUTO_TEST_SUITE_END()
