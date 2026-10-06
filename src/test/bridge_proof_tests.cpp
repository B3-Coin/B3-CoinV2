// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bridge/proof.h>
#include <modern/finality_v2_payload.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <tuple>
#include <variant>
#include <vector>

using namespace bridge;

BOOST_AUTO_TEST_SUITE(bridge_proof_tests)

namespace {

uint256 Hash(const unsigned char seed)
{
    std::array<unsigned char, 32> bytes{};
    for (size_t i{0}; i < bytes.size(); ++i) {
        bytes[i] = static_cast<unsigned char>(seed + i);
    }
    return uint256{std::span<const unsigned char>{bytes}};
}

std::vector<uint256> Branch(const size_t count, const unsigned char seed)
{
    std::vector<uint256> out;
    for (size_t i{0}; i < count; ++i) {
        out.push_back(Hash(static_cast<unsigned char>(seed + i)));
    }
    return out;
}

uint256 RootFromBranch(const uint256& leaf, const std::vector<uint256>& branch,
                       const uint64_t gindex)
{
    unsigned depth{0};
    while ((uint64_t{2} << depth) <= gindex) ++depth;
    if (branch.size() != depth) {
        throw std::invalid_argument{"incorrect SSZ branch depth"};
    }
    const uint64_t index{gindex - (uint64_t{1} << depth)};
    uint256 node{leaf};
    for (unsigned k{0}; k < depth; ++k) {
        node = ((index >> k) & 1) ? ssz::HashPair(branch[k], node)
                                  : ssz::HashPair(node, branch[k]);
    }
    return node;
}

void MakeExecutionProofValid(LightClientHeader& header,
                             const unsigned char seed)
{
    header.execution_branch =
        Branch(BRIDGE_EXECUTION_BRANCH_NODES, seed);
    header.beacon.body_root =
        RootFromBranch(header.execution.HashTreeRoot(),
                       header.execution_branch, EXECUTION_PAYLOAD_GINDEX);
}

LightClientHeader Header(const unsigned char seed)
{
    LightClientHeader out;
    out.beacon.slot = 1000 + seed;
    out.beacon.proposer_index = 2000 + seed;
    out.beacon.parent_root = Hash(seed + 1);
    out.beacon.state_root = Hash(seed + 2);
    out.beacon.body_root = Hash(seed + 3);
    out.execution.parent_hash = Hash(seed + 4);
    std::fill(out.execution.fee_recipient.begin(),
              out.execution.fee_recipient.end(), seed + 5);
    out.execution.state_root = Hash(seed + 6);
    out.execution.receipts_root = Hash(seed + 7);
    std::fill(out.execution.logs_bloom.begin(), out.execution.logs_bloom.end(),
              seed + 8);
    out.execution.prev_randao = Hash(seed + 9);
    out.execution.block_number = 10'000 + seed;
    out.execution.gas_limit = 30'000'000;
    out.execution.gas_used = 12'345'678;
    out.execution.timestamp = 1'800'000'000 + seed;
    out.execution.extra_data = {seed, static_cast<unsigned char>(seed + 1)};
    out.execution.base_fee_per_gas = Hash(seed + 10);
    out.execution.block_hash = Hash(seed + 11);
    out.execution.transactions_root = Hash(seed + 12);
    out.execution.withdrawals_root = Hash(seed + 13);
    out.execution.blob_gas_used = 42 + seed;
    out.execution.excess_blob_gas = 84 + seed;
    out.execution_branch = Branch(BRIDGE_EXECUTION_BRANCH_NODES, seed + 14);
    return out;
}

ssz::SyncCommittee Committee(const unsigned char seed)
{
    ssz::SyncCommittee out;
    out.pubkeys.resize(ssz::SYNC_COMMITTEE_SIZE);
    for (size_t i{0}; i < out.pubkeys.size(); ++i) {
        for (size_t j{0}; j < out.pubkeys[i].size(); ++j) {
            out.pubkeys[i][j] =
                static_cast<unsigned char>(seed + i + j);
        }
    }
    for (size_t i{0}; i < out.aggregate_pubkey.size(); ++i) {
        out.aggregate_pubkey[i] = static_cast<unsigned char>(seed + i + 1);
    }
    return out;
}

LightClientUpdate Update(const bool has_next = true)
{
    LightClientUpdate out;
    out.attested = Header(20);
    out.finalized = Header(40);
    out.finality_branch = Branch(7, 60);
    out.has_next = has_next;
    if (has_next) {
        out.next_committee = Committee(70);
        out.next_branch = Branch(6, 80);
    }
    std::fill(out.sync_aggregate.bits.begin(), out.sync_aggregate.bits.end(),
              0xaa);
    std::fill(out.sync_aggregate.signature.begin(),
              out.sync_aggregate.signature.end(), 0xbb);
    out.signature_slot = 12'345;
    return out;
}

BridgeMintV1 Mint()
{
    BridgeMintV1 out;
    out.registry_id = Hash(1);
    out.output_index = 7;
    out.finalized_anchor_hash = Hash(2);
    out.target_block_number = 22'000'000;
    out.tx_index = 123;
    out.receipt_log_index = 4;
    out.ancestry_headers = {{0xf8, 0x01}, {0xf8, 0x02, 0x03}};
    out.mpt_nodes = {{0xe1, 0x01}, {0xf8, 0x02, 0x03, 0x04}};
    return out;
}

void CheckRoundTrip(const BridgeRecordV1& record)
{
    const auto encoded{EncodeBridgeRecordV1(record)};
    BOOST_REQUIRE(encoded.has_value());
    BOOST_CHECK_LE(encoded->size(), MAX_BRIDGE_RECORD_SIZE);
    const auto decoded{DecodeBridgeRecordV1(*encoded)};
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK(*decoded == record);

    const auto mpa{MakeBridgeMpaRecord(record)};
    BOOST_REQUIRE(mpa.has_value());
    BOOST_CHECK_EQUAL(mpa->payload_type, BRIDGE_MPA_TYPE);
    BOOST_CHECK_EQUAL(mpa->payload_version, BRIDGE_MPA_VERSION_V1);
    BOOST_CHECK(DecodeBridgeMpaRecordV1(*mpa) == decoded);
}

//! A digest-V2 statement with every field distinct, as kind 13 carries it.
modern::FinalizedBlockV2 ConflictStatement(const uint8_t bridge_flags = 0)
{
    modern::FinalizedBlockV2 fb;
    fb.lineage_id = Hash(0x10);
    fb.height = 109;
    fb.block_hash = Hash(0x30);
    fb.withdrawal_root = Hash(0x50);
    fb.validator_set_hash = Hash(0x70);
    fb.epoch = 3;
    fb.checkpoint_index = 2;
    fb.checkpoint_history_root = Hash(0x90);
    fb.bridge_flags = bridge_flags;
    return fb;
}

//! A shape-valid certificate payload V2 for n signers: every signer bit
//! set, zero padding and a patterned aggregate signature. The codec checks
//! only the shape, so the signature need not verify.
std::vector<unsigned char> ConflictPayload(const size_t n,
                                           const uint8_t bridge_flags = 0)
{
    modern::FinalityCertificate cert;
    cert.signer_bitmap.assign(modern::SignerBitmapBytes(n), 0);
    for (size_t i{0}; i < n; ++i) {
        cert.signer_bitmap[i / 8] |= static_cast<unsigned char>(1U << (i % 8));
    }
    for (size_t i{0}; i < cert.aggregate_sig.size(); ++i) {
        cert.aggregate_sig[i] = static_cast<unsigned char>(0xa0 + i);
    }
    return modern::EncodeCertificatePayloadV2(ConflictStatement(bridge_flags),
                                              cert);
}

//! The kind 13 wire built by hand: 0x0d || 0x00 || u16 n || certificate.
std::vector<unsigned char> Kind13Wire(const uint16_t n,
                                      const std::vector<unsigned char>& certificate)
{
    std::vector<unsigned char> out{0x0d, 0x00,
                                   static_cast<unsigned char>(n >> 8),
                                   static_cast<unsigned char>(n)};
    out.insert(out.end(), certificate.begin(), certificate.end());
    return out;
}

} // namespace

BOOST_AUTO_TEST_CASE(all_kinds_and_components_round_trip)
{
    const LightClientHeader header{Header(1)};
    const auto encoded_header{EncodeBridgeLightClientHeaderV1(header)};
    BOOST_REQUIRE(encoded_header.has_value());
    BOOST_CHECK(DecodeBridgeLightClientHeaderV1(*encoded_header) == header);

    const ssz::SyncCommittee committee{Committee(2)};
    const auto encoded_committee{EncodeBridgeSyncCommitteeV1(committee)};
    BOOST_REQUIRE(encoded_committee.has_value());
    BOOST_CHECK(DecodeBridgeSyncCommitteeV1(*encoded_committee) == committee);

    CheckRoundTrip(BridgeRecordV1{
        BridgeRecordKindV1::BOOTSTRAP,
        BridgeBootstrapV1{Header(3), Committee(4), Branch(6, 5)}});

    CheckRoundTrip(BridgeRecordV1{BridgeRecordKindV1::UPDATE,
                                  BridgeUpdateV1{Update()}});

    CheckRoundTrip(BridgeRecordV1{BridgeRecordKindV1::MINT, Mint()});

    CheckRoundTrip(BridgeRecordV1{
        BridgeRecordKindV1::EXECUTION_BACKFILL,
        BridgeExecutionBackfillV1{Hash(6), 22'000'001,
                                  {{0xf8, 0x10}, {0xf8, 0x11}}}});

    EthAddress recipient{};
    recipient.back() = 1;
    CheckRoundTrip(BridgeRecordV1{
        BridgeRecordKindV1::MANAGED_WITHDRAWAL,
        BridgeManagedWithdrawalV1{Hash(7), 2, 1'000'000, recipient}});
    CheckRoundTrip(BridgeRecordV1{
        BridgeRecordKindV1::BRIDGE_BURN,
        BridgeBurnV1{Hash(8), 3, 2'000'000, recipient}});
}

BOOST_AUTO_TEST_CASE(v2_record_set_adds_only_kinds_9_and_13)
{
    // The V2 record set (from the bridge V2 height V) decodes every V1 kind
    // byte-identically and adds only kinds 9, 13 and 16 (node changes
    // sections 3.4, 7.6, 8.2 and 8.5); bridge_state_observation_tests covers
    // kind 9 itself, v2_finality_conflict_record_codec below covers kind 13
    // and vault_sweep_codec kind 16.
    EthAddress recipient{};
    recipient.back() = 1;
    const std::vector<BridgeRecordV1> v1_records{
        {BridgeRecordKindV1::BOOTSTRAP, BridgeBootstrapV1{Header(3), Committee(4), Branch(6, 5)}},
        {BridgeRecordKindV1::UPDATE, BridgeUpdateV1{Update()}},
        {BridgeRecordKindV1::MINT, Mint()},
        {BridgeRecordKindV1::EXECUTION_BACKFILL, BridgeExecutionBackfillV1{Hash(6), 22'000'001, {{0xf8, 0x10}}}},
        {BridgeRecordKindV1::MANAGED_WITHDRAWAL, BridgeManagedWithdrawalV1{Hash(7), 2, 1'000'000, recipient}},
        {BridgeRecordKindV1::BRIDGE_BURN, BridgeBurnV1{Hash(8), 3, 2'000'000, recipient}},
    };
    for (const BridgeRecordV1& record : v1_records) {
        const auto v1{EncodeBridgeRecordV1(record, BRIDGE_MPT_LIMITS_V1)};
        const auto v2{EncodeBridgeRecordV1(record, BRIDGE_MPT_LIMITS_V2)};
        BOOST_REQUIRE(v1 && v2);
        BOOST_CHECK(*v1 == *v2);
        BOOST_CHECK(DecodeBridgeRecordV1(*v1, BRIDGE_MPT_LIMITS_V1) == record);
        BOOST_CHECK(DecodeBridgeRecordV1(*v1, BRIDGE_MPT_LIMITS_V2) == record);
    }

    BridgeEthStateObservationV2 observation;
    observation.subject = ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS;
    observation.anchor_hash = Hash(9);
    BridgeEthAccountProofV2 token;
    token.account_nodes.push_back({0xc1, 0x80});
    token.storage_nodes.resize(3);
    observation.accounts.push_back(token);
    const BridgeRecordV1 kind9{BridgeRecordKindV1::ETH_STATE_OBSERVATION, observation};
    BOOST_CHECK(!EncodeBridgeRecordV1(kind9, BRIDGE_MPT_LIMITS_V1));
    const auto encoded{EncodeBridgeRecordV1(kind9, BRIDGE_MPT_LIMITS_V2)};
    BOOST_REQUIRE(encoded);
    BOOST_CHECK_EQUAL((*encoded)[0], 9);
    BOOST_CHECK(!DecodeBridgeRecordV1(*encoded, BRIDGE_MPT_LIMITS_V1));
    BOOST_CHECK(DecodeBridgeRecordV1(*encoded, BRIDGE_MPT_LIMITS_V2) == kind9);

    // Reserved and unknown kinds (7, 8, 10-12, 14, 15, 17 and beyond) stay
    // malformed in both sets, whatever body follows; kinds 13 and 16 with this kind 9
    // body are malformed too, because neither body has a valid shape.
    for (unsigned kind{0}; kind <= 255; ++kind) {
        if ((kind >= 1 && kind <= 6) || kind == 9) continue;
        auto patched{*encoded};
        patched[0] = static_cast<unsigned char>(kind);
        BOOST_CHECK(!DecodeBridgeRecordV1(patched, BRIDGE_MPT_LIMITS_V1));
        BOOST_CHECK(!DecodeBridgeRecordV1(patched, BRIDGE_MPT_LIMITS_V2));
    }
}

BOOST_AUTO_TEST_CASE(v2_finality_conflict_record_codec)
{
    // Kind 13 FINALITY_CONFLICT (node changes section 8.5):
    // 0x0d || 0x00 || u16 n || FinalizedBlockV2(185) || bitmap(ceil(n/8)) || aggSig(96),
    // decoded only under the V2 record set.
    const std::vector<unsigned char> payload{ConflictPayload(4)};
    BOOST_REQUIRE_EQUAL(payload.size(), modern::FinalizedBlockV2::SIZE + 1 +
                                            modern::BLS_SIGNATURE_SIZE);
    const BridgeRecordV1 record{BridgeRecordKindV1::FINALITY_CONFLICT,
                                BridgeFinalityConflictV2{4, payload}};

    std::vector<unsigned char> expected{0x0d, 0x00, 0x00, 0x04};
    expected.insert(expected.end(), payload.begin(), payload.end());
    BOOST_REQUIRE_EQUAL(expected.size(), 286U);
    BOOST_CHECK(Kind13Wire(4, payload) == expected);

    const auto encoded{EncodeBridgeRecordV1(record, BRIDGE_MPT_LIMITS_V2)};
    BOOST_REQUIRE(encoded);
    BOOST_CHECK(*encoded == expected);
    // Field offsets inside the record: the raw lineage id right after the
    // 4-byte header, the flag byte at 4 + 184, the bitmap at 4 + 185 and the
    // signature as the 96-byte tail.
    const modern::FinalizedBlockV2 fb{ConflictStatement()};
    BOOST_CHECK(std::equal(fb.lineage_id.begin(), fb.lineage_id.end(),
                           encoded->begin() + 4));
    BOOST_CHECK_EQUAL((*encoded)[4 + 184], 0);
    BOOST_CHECK_EQUAL((*encoded)[4 + 185], 0x0f);
    BOOST_CHECK_EQUAL(encoded->back(), static_cast<unsigned char>(0xa0 + 95));

    const auto decoded{DecodeBridgeRecordV1(*encoded, BRIDGE_MPT_LIMITS_V2)};
    BOOST_REQUIRE(decoded);
    BOOST_CHECK(*decoded == record);
    BOOST_CHECK(decoded->kind == BridgeRecordKindV1::FINALITY_CONFLICT);
    const auto* conflict{
        std::get_if<BridgeFinalityConflictV2>(&decoded->payload)};
    BOOST_REQUIRE(conflict);
    BOOST_CHECK_EQUAL(conflict->validator_count, 4);
    const auto parts{conflict->Decode()};
    BOOST_REQUIRE(parts);
    BOOST_CHECK(parts->first == fb);
    BOOST_CHECK(parts->second.signer_bitmap ==
                std::vector<unsigned char>{0x0f});
    BOOST_CHECK(modern::EncodeCertificatePayloadV2(parts->first,
                                                   parts->second) == payload);

    const auto mpa{MakeBridgeMpaRecord(record, BRIDGE_MPT_LIMITS_V2)};
    BOOST_REQUIRE(mpa);
    BOOST_CHECK_EQUAL(mpa->payload_type, BRIDGE_MPA_TYPE);
    BOOST_CHECK_EQUAL(mpa->payload_version, BRIDGE_MPA_VERSION_V1);
    BOOST_CHECK(mpa->payload == expected);
    BOOST_CHECK(DecodeBridgeMpaRecordV1(*mpa, BRIDGE_MPT_LIMITS_V2) == record);

    // Sizes: 4 + 185 + ceil(n/8) + 96, so at most 293 bytes within the
    // bridge bound of 64 validators and 1,309 at n = 8,192. bridgeFlags
    // bit 0 (deposits closed) is a valid statement.
    for (const auto& [n, flags, size] :
         std::vector<std::tuple<uint16_t, uint8_t, size_t>>{
             {1, 0, 286}, {4, 1, 286}, {9, 0, 287}, {64, 1, 293},
             {8'192, 0, 1'309}, {8'192, 1, 1'309}}) {
        const BridgeRecordV1 sized{
            BridgeRecordKindV1::FINALITY_CONFLICT,
            BridgeFinalityConflictV2{n, ConflictPayload(n, flags)}};
        const auto bytes{EncodeBridgeRecordV1(sized, BRIDGE_MPT_LIMITS_V2)};
        BOOST_REQUIRE(bytes);
        BOOST_CHECK_EQUAL(bytes->size(), size);
        BOOST_CHECK_EQUAL((*bytes)[4 + 184], flags);
        BOOST_CHECK(DecodeBridgeRecordV1(*bytes, BRIDGE_MPT_LIMITS_V2) == sized);
        BOOST_CHECK(!DecodeBridgeRecordV1(*bytes, BRIDGE_MPT_LIMITS_V1));
    }

    // Below V, and on every network without bridge_v2, kind 13 is malformed
    // in both directions (the V1 set is also every default argument).
    BOOST_CHECK(!EncodeBridgeRecordV1(record, BRIDGE_MPT_LIMITS_V1));
    BOOST_CHECK(!EncodeBridgeRecordV1(record));
    BOOST_CHECK(!EncodeBridgeProofV1(record));
    BOOST_CHECK(!MakeBridgeMpaRecord(record, BRIDGE_MPT_LIMITS_V1));
    BOOST_CHECK(!DecodeBridgeRecordV1(expected, BRIDGE_MPT_LIMITS_V1));
    BOOST_CHECK(!DecodeBridgeRecordV1(expected));
    BOOST_CHECK(!DecodeBridgeProofV1(expected));
    BOOST_CHECK(!DecodeBridgeMpaRecordV1(*mpa, BRIDGE_MPT_LIMITS_V1));

    // Shape refusals, checked in both directions: the encoder refuses the
    // body and the decoder refuses the same bytes built by hand.
    const auto refused{[](const uint16_t n,
                          const std::vector<unsigned char>& certificate) {
        const BridgeFinalityConflictV2 body{n, certificate};
        BOOST_CHECK(!body.Decode());
        BOOST_CHECK(!EncodeBridgeRecordV1(
            {BridgeRecordKindV1::FINALITY_CONFLICT, body}, BRIDGE_MPT_LIMITS_V2));
        BOOST_CHECK(!DecodeBridgeRecordV1(Kind13Wire(n, certificate),
                                          BRIDGE_MPT_LIMITS_V2));
    }};
    refused(0, payload);                        // n = 0
    refused(0, {});                             // n = 0, no certificate
    refused(4, {});                             // no certificate
    refused(8'193, ConflictPayload(8'193));     // n above MAX_FINALITY_SET, sized for it
    refused(8'193, payload);
    refused(4, ConflictPayload(9));             // a payload sized for another n
    refused(9, payload);
    refused(8'192, payload);
    refused(4, ConflictPayload(8'192));
    {
        auto padding{payload};                  // a set padding bit (bit 4 of n = 4)
        padding[modern::FinalizedBlockV2::SIZE] |= 0x10;
        refused(4, padding);
        auto last_bit{ConflictPayload(9)};      // bit 9 of the 2-byte bitmap of n = 9
        last_bit[modern::FinalizedBlockV2::SIZE + 1] |= 0x02;
        refused(9, last_bit);
    }
    for (const unsigned char flags : {0x02, 0x03, 0x80, 0xff}) {
        auto bad_flags{payload};                // bridge-flag bits other than bit 0
        bad_flags[modern::FinalizedBlockV2::SIZE - 1] = flags;
        refused(4, bad_flags);
    }
    {
        auto trailing{payload};                 // one trailing byte
        trailing.push_back(0);
        refused(4, trailing);
        auto short_sig{payload};                // one byte short
        short_sig.pop_back();
        refused(4, short_sig);
    }

    // The codec fixes the shape, not the committee: n = 5 shares the
    // one-byte bitmap of n = 4, so it decodes; consensus compares n with the
    // signing committee's size.
    BOOST_CHECK(DecodeBridgeRecordV1(Kind13Wire(5, payload), BRIDGE_MPT_LIMITS_V2));

    auto reserved{expected};
    reserved[1] = 0x01;
    BOOST_CHECK(!DecodeBridgeRecordV1(reserved, BRIDGE_MPT_LIMITS_V2));
    auto trailing{expected};
    trailing.push_back(0);
    BOOST_CHECK(!DecodeBridgeRecordV1(trailing, BRIDGE_MPT_LIMITS_V2));
    for (size_t cut{0}; cut < expected.size(); ++cut) {
        BOOST_CHECK(!DecodeBridgeRecordV1(
            std::span<const unsigned char>{expected}.first(cut),
            BRIDGE_MPT_LIMITS_V2));
    }

    // The kind and the body must agree.
    EthAddress recipient{};
    recipient.back() = 1;
    BOOST_CHECK(!EncodeBridgeRecordV1(
        {BridgeRecordKindV1::FINALITY_CONFLICT,
         BridgeBurnV1{Hash(8), 3, 2'000'000, recipient}},
        BRIDGE_MPT_LIMITS_V2));
    BOOST_CHECK(!EncodeBridgeRecordV1(
        {BridgeRecordKindV1::ETH_STATE_OBSERVATION,
         BridgeFinalityConflictV2{4, payload}},
        BRIDGE_MPT_LIMITS_V2));
    BOOST_CHECK(!EncodeBridgeRecordV1(
        {BridgeRecordKindV1::FINALITY_CONFLICT, BridgeFinalityConflictV2{}},
        BRIDGE_MPT_LIMITS_V2));
}

BOOST_AUTO_TEST_CASE(vault_sweep_codec)
{
    // Kind 16 VAULT_SWEEP (node changes section 7.6, B-23): exactly
    // 0x10 || 0x00 || registry_id(32), the launch vault entry id in raw
    // uint256 order, decoded only under the V2 record set.
    const uint256 entry_id{Hash(0x21)};
    const BridgeRecordV1 record{BridgeRecordKindV1::VAULT_SWEEP,
                                BridgeVaultSweepV2{entry_id}};
    BOOST_CHECK_EQUAL(static_cast<unsigned>(BridgeRecordKindV1::VAULT_SWEEP), 16U);
    BOOST_CHECK_EQUAL(BRIDGE_VAULT_SWEEP_WIRE_SIZE, 34U);

    std::vector<unsigned char> expected{0x10, 0x00};
    expected.insert(expected.end(), entry_id.begin(), entry_id.end());
    BOOST_REQUIRE_EQUAL(expected.size(), BRIDGE_VAULT_SWEEP_WIRE_SIZE);
    // Raw order: the id's first byte (Hash() counts up from the seed) is the
    // record's third byte.
    BOOST_CHECK_EQUAL(HexStr(expected),
                      "1000"
                      "2122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f40");

    const auto encoded{EncodeBridgeRecordV1(record, BRIDGE_MPT_LIMITS_V2)};
    BOOST_REQUIRE(encoded);
    BOOST_CHECK(*encoded == expected);
    const auto decoded{DecodeBridgeRecordV1(expected, BRIDGE_MPT_LIMITS_V2)};
    BOOST_REQUIRE(decoded);
    BOOST_CHECK(*decoded == record);
    BOOST_CHECK(decoded->kind == BridgeRecordKindV1::VAULT_SWEEP);
    const auto* sweep{std::get_if<BridgeVaultSweepV2>(&decoded->payload)};
    BOOST_REQUIRE(sweep);
    BOOST_CHECK(sweep->registry_id == entry_id);

    const auto mpa{MakeBridgeMpaRecord(record, BRIDGE_MPT_LIMITS_V2)};
    BOOST_REQUIRE(mpa);
    BOOST_CHECK_EQUAL(mpa->payload_type, BRIDGE_MPA_TYPE);
    BOOST_CHECK_EQUAL(mpa->payload_version, BRIDGE_MPA_VERSION_V1);
    BOOST_CHECK(mpa->payload == expected);
    BOOST_CHECK(DecodeBridgeMpaRecordV1(*mpa, BRIDGE_MPT_LIMITS_V2) == record);

    // Below V, and on every network without bridge_v2, kind 16 is malformed
    // in both directions (the V1 set is also every default argument).
    BOOST_CHECK(!EncodeBridgeRecordV1(record, BRIDGE_MPT_LIMITS_V1));
    BOOST_CHECK(!EncodeBridgeRecordV1(record));
    BOOST_CHECK(!EncodeBridgeProofV1(record));
    BOOST_CHECK(!MakeBridgeMpaRecord(record, BRIDGE_MPT_LIMITS_V1));
    BOOST_CHECK(!DecodeBridgeRecordV1(expected, BRIDGE_MPT_LIMITS_V1));
    BOOST_CHECK(!DecodeBridgeRecordV1(expected));
    BOOST_CHECK(!DecodeBridgeProofV1(expected));
    BOOST_CHECK(!DecodeBridgeMpaRecordV1(*mpa, BRIDGE_MPT_LIMITS_V1));

    // A null id is refused in both directions.
    BOOST_CHECK(!EncodeBridgeRecordV1(
        {BridgeRecordKindV1::VAULT_SWEEP, BridgeVaultSweepV2{}}, BRIDGE_MPT_LIMITS_V2));
    std::vector<unsigned char> null_id{0x10, 0x00};
    null_id.resize(BRIDGE_VAULT_SWEEP_WIRE_SIZE, 0x00);
    BOOST_CHECK(!DecodeBridgeRecordV1(null_id, BRIDGE_MPT_LIMITS_V2));
    // One non-zero byte anywhere in the id is enough.
    for (const size_t pos : {size_t{2}, size_t{33}}) {
        auto one_byte{null_id};
        one_byte[pos] = 0x01;
        BOOST_CHECK(DecodeBridgeRecordV1(one_byte, BRIDGE_MPT_LIMITS_V2));
    }

    // The body is exactly 32 bytes: every truncation (records of 0-33
    // bytes) and 33- and 35-byte bodies are refused.
    for (size_t cut{0}; cut < expected.size(); ++cut) {
        BOOST_CHECK(!DecodeBridgeRecordV1(
            std::span<const unsigned char>{expected}.first(cut),
            BRIDGE_MPT_LIMITS_V2));
    }
    for (const size_t extra : {size_t{1}, size_t{3}}) {
        auto longer{expected};
        longer.insert(longer.end(), extra, 0x00);
        BOOST_CHECK(!DecodeBridgeRecordV1(longer, BRIDGE_MPT_LIMITS_V2));
        longer.back() = 0x01;
        BOOST_CHECK(!DecodeBridgeRecordV1(longer, BRIDGE_MPT_LIMITS_V2));
    }

    // The reserved byte must be zero.
    for (const unsigned char reserved : {0x01, 0x80, 0xff}) {
        auto patched{expected};
        patched[1] = reserved;
        BOOST_CHECK(!DecodeBridgeRecordV1(patched, BRIDGE_MPT_LIMITS_V2));
    }

    // The same 34 bytes under any other kind byte are malformed in both
    // sets: kinds 7, 8, 10-12, 14 and 15 stay reserved (14 and 15 are the
    // staker-signed fast deposit proposal's FSD_MINT and FSD_FAULT), and no
    // other kind has a 32-byte body.
    for (unsigned kind{0}; kind <= 255; ++kind) {
        if (kind == 16) continue;
        auto patched{expected};
        patched[0] = static_cast<unsigned char>(kind);
        BOOST_CHECK(!DecodeBridgeRecordV1(patched, BRIDGE_MPT_LIMITS_V1));
        BOOST_CHECK(!DecodeBridgeRecordV1(patched, BRIDGE_MPT_LIMITS_V2));
    }

    // The kind and the body must agree.
    EthAddress recipient{};
    recipient.back() = 1;
    BOOST_CHECK(!EncodeBridgeRecordV1(
        {BridgeRecordKindV1::VAULT_SWEEP,
         BridgeBurnV1{entry_id, 1, 2'000'000, recipient}},
        BRIDGE_MPT_LIMITS_V2));
    BOOST_CHECK(!EncodeBridgeRecordV1(
        {BridgeRecordKindV1::BRIDGE_BURN, BridgeVaultSweepV2{entry_id}},
        BRIDGE_MPT_LIMITS_V2));
    BOOST_CHECK(!EncodeBridgeRecordV1(
        {BridgeRecordKindV1::FINALITY_CONFLICT, BridgeVaultSweepV2{entry_id}},
        BRIDGE_MPT_LIMITS_V2));
}

BOOST_AUTO_TEST_CASE(rejects_truncation_trailing_and_noncanonical_envelopes)
{
    const BridgeRecordV1 record{BridgeRecordKindV1::MINT, Mint()};
    const auto encoded{EncodeBridgeRecordV1(record)};
    BOOST_REQUIRE(encoded.has_value());
    for (size_t cut{0}; cut < encoded->size(); ++cut) {
        BOOST_CHECK(!DecodeBridgeRecordV1(
            std::span<const unsigned char>{*encoded}.first(cut)));
    }

    auto trailing{*encoded};
    trailing.push_back(0);
    BOOST_CHECK(!DecodeBridgeRecordV1(trailing));

    auto reserved{*encoded};
    reserved[1] = 1;
    BOOST_CHECK(!DecodeBridgeRecordV1(reserved));

    auto unknown{*encoded};
    unknown[0] = 0;
    BOOST_CHECK(!DecodeBridgeRecordV1(unknown));

    BOOST_CHECK(!EncodeBridgeRecordV1(BridgeRecordV1{
        BridgeRecordKindV1::MINT, BridgeUpdateV1{Update()}}));

    const BridgeRecordV1 no_next{
        BridgeRecordKindV1::UPDATE, BridgeUpdateV1{Update(false)}};
    auto encoded_update{EncodeBridgeRecordV1(no_next)};
    BOOST_REQUIRE(encoded_update.has_value());
    // has_next is immediately before the fixed 64+96+8-byte aggregate tail.
    const size_t has_next_pos{encoded_update->size() - (64 + 96 + 8) - 1};
    (*encoded_update)[has_next_pos] = 2;
    BOOST_CHECK(!DecodeBridgeRecordV1(*encoded_update));

    std::vector<unsigned char> oversized(MAX_BRIDGE_RECORD_SIZE + 1);
    BOOST_CHECK(!DecodeBridgeRecordV1(oversized));
}

BOOST_AUTO_TEST_CASE(rejects_count_element_and_aggregate_limit_violations)
{
    {
        auto value{Mint()};
        value.ancestry_headers.assign(
            MAX_BRIDGE_EXECUTION_ANCESTRY_HEADERS + 1, {0xc0});
        BOOST_CHECK(!EncodeBridgeRecordV1(
            {BridgeRecordKindV1::MINT, std::move(value)}));
    }
    {
        auto value{Mint()};
        value.mpt_nodes.assign(MAX_BRIDGE_MPT_NODES + 1, {0xc0});
        BOOST_CHECK(!EncodeBridgeRecordV1(
            {BridgeRecordKindV1::MINT, std::move(value)}));
    }
    {
        auto value{Mint()};
        value.ancestry_headers = {
            std::vector<unsigned char>(MAX_BRIDGE_RLP_ITEM_SIZE + 1, 1)};
        BOOST_CHECK(!EncodeBridgeRecordV1(
            {BridgeRecordKindV1::MINT, std::move(value)}));
    }
    {
        auto value{Mint()};
        value.ancestry_headers.assign(
            MAX_BRIDGE_EXECUTION_ANCESTRY_BYTES /
                    MAX_BRIDGE_RLP_ITEM_SIZE +
                1,
            std::vector<unsigned char>(MAX_BRIDGE_RLP_ITEM_SIZE, 1));
        BOOST_CHECK(!EncodeBridgeRecordV1(
            {BridgeRecordKindV1::MINT, std::move(value)}));
    }
    {
        auto value{Mint()};
        value.mpt_nodes.assign(
            MAX_BRIDGE_MPT_BYTES / MAX_BRIDGE_RLP_ITEM_SIZE + 1,
            std::vector<unsigned char>(MAX_BRIDGE_RLP_ITEM_SIZE, 1));
        BOOST_CHECK(!EncodeBridgeRecordV1(
            {BridgeRecordKindV1::MINT, std::move(value)}));
    }

    auto minimal{Mint()};
    minimal.ancestry_headers = {{0xc0}};
    minimal.mpt_nodes = {{0xc0}};
    const auto encoded{EncodeBridgeRecordV1(
        {BridgeRecordKindV1::MINT, minimal})};
    BOOST_REQUIRE(encoded.has_value());
    constexpr size_t MINT_FIXED_PREFIX{2 + 32 + 4 + 32 + 8 + 8 + 4};
    auto bad_ancestry_count{*encoded};
    bad_ancestry_count[MINT_FIXED_PREFIX] =
        MAX_BRIDGE_EXECUTION_ANCESTRY_HEADERS + 1;
    BOOST_CHECK(!DecodeBridgeRecordV1(bad_ancestry_count));

    auto bad_mpt_count{*encoded};
    // count + u16 length + the one-byte ancestry node.
    bad_mpt_count[MINT_FIXED_PREFIX + 4] = MAX_BRIDGE_MPT_NODES + 1;
    BOOST_CHECK(!DecodeBridgeRecordV1(bad_mpt_count));

    auto bad_committee{Committee(9)};
    bad_committee.pubkeys.pop_back();
    BOOST_CHECK(!EncodeBridgeSyncCommitteeV1(bad_committee));

    auto bad_header{Header(9)};
    bad_header.execution.extra_data.assign(33, 1);
    BOOST_CHECK(!EncodeBridgeLightClientHeaderV1(bad_header));
    BOOST_CHECK(!bad_header.VerifyExecution());

    const std::vector<unsigned char> too_wide_leaf(33, 1);
    BOOST_CHECK(ssz::LeafBytes(too_wide_leaf).IsNull());
}

BOOST_AUTO_TEST_CASE(exact_receipt_log_index_is_bound)
{
    EthAddress vault{};
    vault[0] = 0x11;
    EthAddress token{};
    token[19] = 0x22;

    EthLog deposit;
    deposit.address = vault;
    deposit.topics.resize(3);
    deposit.topics[0] = DepositTopic();
    for (int i{24}; i < 32; ++i) {
        deposit.topics[1].begin()[i] =
            static_cast<unsigned char>(0x0102030405060708ULL >>
                                       (8 * (31 - i)));
    }
    std::copy(token.begin(), token.end(), deposit.topics[2].begin() + 12);
    deposit.data.resize(64);
    deposit.data[31] = 9;
    deposit.data[63] = 0x44;

    EthReceipt receipt;
    receipt.status = true;
    receipt.logs.push_back(EthLog{});
    receipt.logs.push_back(deposit);

    BOOST_CHECK(!ExtractDepositAt(receipt, vault, 0));
    const auto event{ExtractDepositAt(receipt, vault, 1)};
    BOOST_REQUIRE(event.has_value());
    BOOST_CHECK_EQUAL(event->deposit_id, 0x0102030405060708ULL);
    BOOST_CHECK(event->token == token);
    BOOST_CHECK_EQUAL(event->amount[31], 9);
    BOOST_CHECK_EQUAL(event->b3_recipient[31], 0x44);
    BOOST_CHECK(!ExtractDepositAt(receipt, vault, 2));

    receipt.status = false;
    BOOST_CHECK(!ExtractDepositAt(receipt, vault, 1));
}

BOOST_AUTO_TEST_CASE(light_client_rejects_equal_slot_and_next_committee_conflicts)
{
    LightClientConfig cfg;

    {
        LightClientStore store;
        store.finalized_header.beacon.slot = 64;
        store.finalized_header.beacon.state_root = Hash(1);
        store.period = PeriodAtSlot(store.finalized_header.beacon.slot);

        LightClientUpdate update;
        update.finalized = Header(90);
        update.finalized.beacon.slot = 64;
        MakeExecutionProofValid(update.finalized, 91);
        update.finality_branch = Branch(6, 92);
        update.attested = Header(93);
        update.attested.beacon.slot = 65;
        MakeExecutionProofValid(update.attested, 94);
        update.attested.beacon.state_root = RootFromBranch(
            update.finalized.beacon.HashTreeRoot(), update.finality_branch,
            FINALIZED_ROOT_GINDEX);
        update.signature_slot = 66;
        BOOST_CHECK(VerifyUpdate(store, cfg, update) ==
                    LcResult::MONOTONICITY);
    }

    {
        LightClientStore store;
        store.period = 0;
        store.next = Committee(3);

        LightClientUpdate update;
        update.finalized = store.finalized_header;
        update.attested.beacon.slot = 1;
        update.signature_slot = 2;
        update.has_next = true;
        update.next_committee = Committee(4);
        BOOST_CHECK(VerifyUpdate(store, cfg, update) == LcResult::NEXT_PROOF);
    }
}

BOOST_AUTO_TEST_SUITE_END()
