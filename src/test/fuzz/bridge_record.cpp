// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! The type-10 bridge record codec, the first decoder a node runs on a
//! bridge transaction's payload, in the mempool and in every block. Below the
//! bridge V2 height, so always on mainnet, the payload rules decode a record
//! under the V1 limits, while the node's helpers that classify a block's
//! bridge records, and the wallet, decode it under the V2 limits; the wallet
//! also re-encodes a decoded record to rebuild its transaction. So a decoded
//! record must re-encode to its own bytes under the limits it was decoded
//! with, and a record the V1 limits accept must decode the same under the V2
//! limits.

#include <bridge/proof.h>
#include <modern/finality_types.h>
#include <modern/finality_v2_payload.h>
#include <primitives/transaction.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace {

using Bytes = std::vector<unsigned char>;
using Kind = bridge::BridgeRecordKindV1;

template <size_t N>
std::array<unsigned char, N> ConsumeArray(FuzzedDataProvider& fdp)
{
    std::array<unsigned char, N> out{};
    const Bytes bytes{fdp.ConsumeBytes<unsigned char>(N)};
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

//! The record `bytes` decode to re-encodes to them under the limits it was
//! decoded with, the V2 limits only widen the V1 limits, and the MPA frame
//! adds nothing but its type and version.
void CheckRecordBytes(const std::span<const unsigned char> bytes)
{
    const auto v1{bridge::DecodeBridgeRecordV1(bytes, bridge::BRIDGE_MPT_LIMITS_V1)};
    const auto v2{bridge::DecodeBridgeRecordV1(bytes, bridge::BRIDGE_MPT_LIMITS_V2)};
    if (v1) {
        assert(v2 && *v2 == *v1);
        const auto encoded{bridge::EncodeBridgeRecordV1(*v1, bridge::BRIDGE_MPT_LIMITS_V1)};
        assert(encoded && std::ranges::equal(*encoded, bytes));
    }
    if (v2) {
        const auto encoded{bridge::EncodeBridgeRecordV1(*v2, bridge::BRIDGE_MPT_LIMITS_V2)};
        assert(encoded && std::ranges::equal(*encoded, bytes));
    }
    CMpaRecord record{bridge::BRIDGE_MPA_TYPE, bridge::BRIDGE_MPA_VERSION_V1, Bytes(bytes.begin(), bytes.end())};
    assert(bridge::DecodeBridgeMpaRecordV1(record) == v1);
    assert(bridge::DecodeBridgeMpaRecordV1(record, bridge::BRIDGE_MPT_LIMITS_V2) == v2);
    record.payload_version = bridge::BRIDGE_MPA_VERSION_V1 + 1;
    assert(!bridge::DecodeBridgeMpaRecordV1(record, bridge::BRIDGE_MPT_LIMITS_V2));
    record.payload_version = bridge::BRIDGE_MPA_VERSION_V1;
    record.payload_type = bridge::BRIDGE_MPA_TYPE + 1;
    assert(!bridge::DecodeBridgeMpaRecordV1(record, bridge::BRIDGE_MPT_LIMITS_V2));
}

//! A blob of `size` bytes: a few from the input, the rest one fill byte, so
//! that blobs at the size limits cost the input little.
Bytes Blob(FuzzedDataProvider& fdp, const size_t size)
{
    Bytes blob(size, fdp.ConsumeIntegral<uint8_t>());
    const Bytes head{fdp.ConsumeBytes<unsigned char>(std::min<size_t>(size, 8))};
    std::copy(head.begin(), head.end(), blob.begin());
    return blob;
}

//! Proof nodes or headers: mostly small, at times empty or at a size limit,
//! and at times one more than a count limit allows.
std::vector<Bytes> Blobs(FuzzedDataProvider& fdp, const unsigned max_count)
{
    std::vector<Bytes> blobs;
    LIMITED_WHILE(fdp.ConsumeBool(), max_count + 1) {
        const size_t size{fdp.ConsumeBool() ? fdp.ConsumeIntegralInRange<size_t>(1, 64)
                                            : fdp.PickValueInArray<size_t>({0, 1023, 1024, 1025, 8191, 8192, 8193})};
        blobs.push_back(Blob(fdp, size));
    }
    return blobs;
}

//! A Merkle branch: one to eight nodes are valid.
std::vector<uint256> Branch(FuzzedDataProvider& fdp)
{
    std::vector<uint256> branch(fdp.ConsumeIntegralInRange<size_t>(0, bridge::MAX_BRIDGE_MERKLE_BRANCH_NODES + 1));
    for (uint256& node : branch) node = ConsumeUInt256(fdp);
    return branch;
}

bridge::LightClientHeader Header(FuzzedDataProvider& fdp)
{
    bridge::LightClientHeader header;
    header.beacon.slot = fdp.ConsumeIntegral<uint64_t>();
    header.beacon.proposer_index = fdp.ConsumeIntegral<uint64_t>();
    header.beacon.parent_root = ConsumeUInt256(fdp);
    header.beacon.state_root = ConsumeUInt256(fdp);
    header.beacon.body_root = ConsumeUInt256(fdp);
    bridge::ssz::ExecutionPayloadHeader& execution{header.execution};
    execution.parent_hash = ConsumeUInt256(fdp);
    execution.fee_recipient = ConsumeArray<20>(fdp);
    execution.state_root = ConsumeUInt256(fdp);
    execution.receipts_root = ConsumeUInt256(fdp);
    execution.logs_bloom.fill(fdp.ConsumeIntegral<uint8_t>());
    execution.prev_randao = ConsumeUInt256(fdp);
    execution.block_number = fdp.ConsumeIntegral<uint64_t>();
    execution.gas_limit = fdp.ConsumeIntegral<uint64_t>();
    execution.gas_used = fdp.ConsumeIntegral<uint64_t>();
    execution.timestamp = fdp.ConsumeIntegral<uint64_t>();
    execution.extra_data = ConsumeRandomLengthByteVector<unsigned char>(fdp, 34); // 32 at most
    execution.base_fee_per_gas = ConsumeUInt256(fdp);
    execution.block_hash = ConsumeUInt256(fdp);
    execution.transactions_root = ConsumeUInt256(fdp);
    execution.withdrawals_root = ConsumeUInt256(fdp);
    execution.blob_gas_used = fdp.ConsumeIntegral<uint64_t>();
    execution.excess_blob_gas = fdp.ConsumeIntegral<uint64_t>();
    // Exactly four nodes, or now and then one fewer or more.
    header.execution_branch.resize(fdp.ConsumeIntegralInRange<int>(0, 15) == 0
                                       ? fdp.PickValueInArray<size_t>({bridge::BRIDGE_EXECUTION_BRANCH_NODES - 1,
                                                                       bridge::BRIDGE_EXECUTION_BRANCH_NODES + 1})
                                       : bridge::BRIDGE_EXECUTION_BRANCH_NODES);
    for (uint256& node : header.execution_branch) node = ConsumeUInt256(fdp);
    return header;
}

//! 512 keys, or now and then a few fewer or more.
bridge::ssz::SyncCommittee Committee(FuzzedDataProvider& fdp)
{
    using bridge::ssz::SYNC_COMMITTEE_SIZE;
    bridge::ssz::SyncCommittee committee;
    committee.pubkeys.resize(fdp.ConsumeIntegralInRange<int>(0, 15) == 0
                                 ? fdp.PickValueInArray<size_t>({0, SYNC_COMMITTEE_SIZE - 1, SYNC_COMMITTEE_SIZE + 1})
                                 : SYNC_COMMITTEE_SIZE);
    const uint8_t seed{fdp.ConsumeIntegral<uint8_t>()};
    for (size_t i{0}; i < committee.pubkeys.size(); ++i) {
        committee.pubkeys[i].fill(static_cast<unsigned char>(seed + i));
    }
    if (!committee.pubkeys.empty()) committee.pubkeys.front() = ConsumeArray<48>(fdp);
    committee.aggregate_pubkey = ConsumeArray<48>(fdp);
    return committee;
}

//! A record of any kind, built field by field. Most fields are within the
//! codec's rules, some at their limits and a few past them.
bridge::BridgeRecordV1 Record(FuzzedDataProvider& fdp)
{
    bridge::BridgeRecordV1 record;
    record.kind = fdp.PickValueInArray<Kind>({Kind::BOOTSTRAP, Kind::UPDATE, Kind::MINT, Kind::EXECUTION_BACKFILL,
                                              Kind::MANAGED_WITHDRAWAL, Kind::BRIDGE_BURN, Kind::ETH_STATE_OBSERVATION,
                                              Kind::FINALITY_CONFLICT, Kind::VAULT_SWEEP});
    switch (record.kind) {
    case Kind::BOOTSTRAP: {
        bridge::BridgeBootstrapV1 value;
        value.header = Header(fdp);
        value.current_committee = Committee(fdp);
        value.current_committee_branch = Branch(fdp);
        record.payload = std::move(value);
        break;
    }
    case Kind::UPDATE: {
        bridge::BridgeUpdateV1 value;
        bridge::LightClientUpdate& update{value.update};
        update.attested = Header(fdp);
        update.finalized = Header(fdp);
        update.finality_branch = Branch(fdp);
        update.has_next = fdp.ConsumeBool();
        if (update.has_next) {
            update.next_committee = Committee(fdp);
            update.next_branch = Branch(fdp);
        }
        update.sync_aggregate.bits = ConsumeArray<64>(fdp);
        update.sync_aggregate.signature = ConsumeArray<96>(fdp);
        update.signature_slot = fdp.ConsumeIntegral<uint64_t>();
        record.payload = std::move(value);
        break;
    }
    case Kind::MINT: {
        bridge::BridgeMintV1 value;
        value.registry_id = ConsumeUInt256(fdp);
        value.output_index = fdp.ConsumeIntegral<uint32_t>();
        value.finalized_anchor_hash = ConsumeUInt256(fdp);
        value.target_block_number = fdp.ConsumeIntegral<uint64_t>();
        value.tx_index = fdp.ConsumeIntegral<uint64_t>();
        value.receipt_log_index = fdp.ConsumeIntegral<uint32_t>();
        value.ancestry_headers = Blobs(fdp, bridge::MAX_BRIDGE_EXECUTION_ANCESTRY_HEADERS);
        value.mpt_nodes = Blobs(fdp, bridge::MAX_BRIDGE_MPT_NODES);
        record.payload = std::move(value);
        break;
    }
    case Kind::EXECUTION_BACKFILL: {
        bridge::BridgeExecutionBackfillV1 value;
        value.finalized_anchor_hash = ConsumeUInt256(fdp);
        value.target_block_number = fdp.ConsumeIntegral<uint64_t>();
        value.ancestry_headers = Blobs(fdp, bridge::MAX_BRIDGE_EXECUTION_ANCESTRY_HEADERS);
        record.payload = std::move(value);
        break;
    }
    case Kind::MANAGED_WITHDRAWAL: {
        bridge::BridgeManagedWithdrawalV1 value;
        value.registry_id = ConsumeUInt256(fdp);
        value.burn_output_index = fdp.ConsumeIntegral<uint32_t>();
        value.raw_amount = fdp.ConsumeIntegral<uint64_t>();
        value.ethereum_recipient = ConsumeArray<20>(fdp);
        record.payload = std::move(value);
        break;
    }
    case Kind::BRIDGE_BURN: {
        bridge::BridgeBurnV1 value;
        value.registry_id = ConsumeUInt256(fdp);
        value.burn_output_index = fdp.ConsumeIntegral<uint32_t>();
        value.raw_amount = fdp.ConsumeIntegral<uint64_t>();
        value.ethereum_recipient = ConsumeArray<20>(fdp);
        if (fdp.ConsumeBool()) {
            modern::ExitForwarderParamsV2 exit;
            exit.dst_eid = fdp.ConsumeIntegral<uint32_t>();
            exit.to = ConsumeArray<32>(fdp);
            exit.max_bridge_fee_bps = fdp.ConsumeIntegral<uint16_t>();
            exit.fee_cap = fdp.ConsumeIntegral<uint64_t>();
            exit.refund_eoa = ConsumeArray<20>(fdp);
            value.exit = exit;
        }
        record.payload = std::move(value);
        break;
    }
    case Kind::ETH_STATE_OBSERVATION: {
        bridge::BridgeEthStateObservationV2 value;
        value.subject = fdp.PickValueInArray<uint8_t>(
            {bridge::ETH_OBSERVATION_SUBJECT_VERIFIER, bridge::ETH_OBSERVATION_SUBJECT_RESERVE,
             bridge::ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS, bridge::ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR,
             bridge::ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS_SUCCESSOR, fdp.ConsumeIntegral<uint8_t>()});
        value.anchor_hash = ConsumeUInt256(fdp);
        value.verdict = fdp.ConsumeBool() ? 0 : fdp.ConsumeIntegral<uint8_t>();
        // The subject's shape of accounts and storage slots, or now and
        // then one account or slot more.
        const auto shape{bridge::EthObservationShapeV2(value.subject)};
        value.accounts.resize(shape.size() + (fdp.ConsumeIntegralInRange<int>(0, 15) == 0 ? 1 : 0));
        for (size_t a{0}; a < value.accounts.size(); ++a) {
            bridge::BridgeEthAccountProofV2& account{value.accounts[a]};
            account.account_nodes = Blobs(fdp, 4);
            const size_t slots{a < shape.size() ? size_t{shape[a]} : 1};
            account.storage_nodes.resize(slots + (fdp.ConsumeIntegralInRange<int>(0, 15) == 0 ? 1 : 0));
            for (auto& slot_nodes : account.storage_nodes) slot_nodes = Blobs(fdp, 3);
        }
        record.payload = std::move(value);
        break;
    }
    case Kind::FINALITY_CONFLICT: {
        bridge::BridgeFinalityConflictV2 value;
        value.validator_count = fdp.ConsumeIntegralInRange<uint16_t>(0, uint16_t{modern::MAX_FINALITY_SET + 1});
        // The exact length for that many validators, or now and then not.
        const size_t size{modern::FinalizedBlockV2::SIZE + modern::SignerBitmapBytes(value.validator_count) +
                          modern::BLS_SIGNATURE_SIZE};
        value.certificate = Blob(fdp, fdp.ConsumeIntegralInRange<int>(0, 15) == 0 ? size + 1 : size);
        // Mostly a known bridge flag and clear bitmap padding, which the
        // certificate needs to decode; now and then the blob as it is.
        if (value.certificate.size() == size && fdp.ConsumeIntegralInRange<int>(0, 15) != 15) {
            value.certificate[modern::FinalizedBlockV2::SIZE - 1] &= 0x01;
            if (const size_t bits{value.validator_count % 8u}; bits != 0) {
                value.certificate[modern::FinalizedBlockV2::SIZE + modern::SignerBitmapBytes(value.validator_count) - 1] &=
                    static_cast<unsigned char>((1u << bits) - 1);
            }
        }
        record.payload = std::move(value);
        break;
    }
    case Kind::VAULT_SWEEP: {
        bridge::BridgeVaultSweepV2 value;
        value.registry_id = ConsumeUInt256(fdp);
        record.payload = std::move(value);
        break;
    }
    }
    return record;
}

//! `encoded`, the encoding of `record`, with one field just past a limit
//! the encoder keeps, at an offset the codec fixes: a ninth node in a
//! bootstrap's committee branch (its last field), 33 bytes of extra data in
//! its header, or an empty first ancestry header in a mint or backfill.
std::optional<Bytes> PastLimit(const bridge::BridgeRecordV1& record, const Bytes& encoded, FuzzedDataProvider& fdp)
{
    // Kind and reserved byte, the beacon header (112 bytes), and the
    // execution header's fields before its extra data (436 bytes).
    constexpr size_t EXTRA_SIZE_AT{2 + 112 + 436};
    Bytes edited{encoded};
    switch (record.kind) {
    case Kind::BOOTSTRAP: {
        const auto& value{std::get<bridge::BridgeBootstrapV1>(record.payload)};
        if (fdp.ConsumeBool()) {
            if (value.current_committee_branch.size() != bridge::MAX_BRIDGE_MERKLE_BRANCH_NODES) return std::nullopt;
            edited[edited.size() - 1 - 32 * bridge::MAX_BRIDGE_MERKLE_BRANCH_NODES] += 1;
            edited.insert(edited.end(), 32, 0x5a);
        } else {
            if (value.header.execution.extra_data.size() != 32) return std::nullopt;
            edited[EXTRA_SIZE_AT] = 33;
            edited.insert(edited.begin() + EXTRA_SIZE_AT + 1 + 32, 0x5a);
        }
        return edited;
    }
    case Kind::MINT:
    case Kind::EXECUTION_BACKFILL: {
        // The ancestry count follows the fixed fields: 90 bytes into a mint,
        // 42 into a backfill. Its first header's u16 size follows.
        const size_t count_at{record.kind == Kind::MINT ? size_t{90} : size_t{42}};
        const size_t size{(size_t{edited[count_at + 1]} << 8) | edited[count_at + 2]};
        edited[count_at + 1] = 0;
        edited[count_at + 2] = 0;
        edited.erase(edited.begin() + count_at + 3, edited.begin() + count_at + 3 + size);
        return edited;
    }
    default:
        return std::nullopt;
    }
}

} // namespace

FUZZ_TARGET(bridge_record)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    if (fdp.ConsumeBool()) {
        // Any payload: never crash, and decode only from the canonical
        // encoding of what it decodes to.
        CheckRecordBytes(fdp.ConsumeRemainingBytes<unsigned char>());
        return;
    }

    // A record built field by field decodes to itself from its encoding
    // under each limit set that encodes it, and so do the same bytes with
    // one byte changed, cut off, added or inserted, when they decode at all.
    // With a field just past a limit, they decode under neither.
    const bridge::BridgeRecordV1 record{Record(fdp)};
    for (const bridge::BridgeMptLimits* limits : {&bridge::BRIDGE_MPT_LIMITS_V1, &bridge::BRIDGE_MPT_LIMITS_V2}) {
        const auto encoded{bridge::EncodeBridgeRecordV1(record, *limits)};
        if (!encoded) continue;
        const auto decoded{bridge::DecodeBridgeRecordV1(*encoded, *limits)};
        assert(decoded && *decoded == record);
        CheckRecordBytes(*encoded);
        Bytes damaged{*encoded};
        const size_t at{fdp.ConsumeIntegralInRange<size_t>(0, damaged.size() - 1)};
        switch (fdp.ConsumeIntegralInRange<int>(0, 3)) {
        case 0: damaged[at] ^= fdp.ConsumeIntegralInRange<uint8_t>(1, 255); break;
        case 1: damaged.resize(at); break;
        case 2: damaged.push_back(fdp.ConsumeIntegral<uint8_t>()); break;
        case 3: damaged.insert(damaged.begin() + at, fdp.ConsumeIntegral<uint8_t>()); break;
        }
        CheckRecordBytes(damaged);
        if (const auto past{PastLimit(record, *encoded, fdp)}) {
            CheckRecordBytes(*past);
            assert(!bridge::DecodeBridgeRecordV1(*past, bridge::BRIDGE_MPT_LIMITS_V2));
        }
    }
}
