// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bridge/proof.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
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
