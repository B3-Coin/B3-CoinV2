// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_BRIDGE_PROOF_H
#define B3COIN_BRIDGE_PROOF_H

#include <bridge/deposit.h>
#include <bridge/eth_light_client.h>
#include <consensus/consensus.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

/**
 * Canonical type-10 bridge record codec.
 *
 * The outer Modern Payload Area frame supplies type=10 and version=1. Its
 * payload begins with a one-byte kind and one reserved zero byte, followed by
 * the kind-specific body below. Integers are fixed-width big-endian; hashes,
 * Ethereum addresses, BLS keys/signatures and bitvectors are raw fixed arrays.
 * The few variable collections use fixed-width count/length fields and are
 * bounded before any allocation. No generic C++ serialization is used.
 */
namespace bridge {

inline constexpr uint16_t BRIDGE_MPA_TYPE{10};
inline constexpr uint16_t BRIDGE_MPA_VERSION_V1{1};
inline constexpr size_t MAX_BRIDGE_RECORD_SIZE{32'768};
inline constexpr size_t MAX_BRIDGE_LIGHT_CLIENT_HEADER_SIZE{1'024};
inline constexpr size_t BRIDGE_EXECUTION_BRANCH_NODES{4};
inline constexpr size_t MAX_BRIDGE_MERKLE_BRANCH_NODES{8};
inline constexpr size_t MAX_BRIDGE_EXECUTION_ANCESTRY_HEADERS{32};
/** Maximum distance from an anchor's directly finalized execution origin. */
inline constexpr uint64_t MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS{20'000};
inline constexpr size_t MAX_BRIDGE_MPT_NODES{64};
inline constexpr size_t MAX_BRIDGE_RLP_ITEM_SIZE{1'024};
inline constexpr size_t MAX_BRIDGE_EXECUTION_ANCESTRY_BYTES{20 * 1'024};
inline constexpr size_t MAX_BRIDGE_MPT_BYTES{11 * 1'024};

inline constexpr size_t MAX_BRIDGE_HEADER_WIRE_SIZE{853};
inline constexpr size_t BRIDGE_SYNC_COMMITTEE_WIRE_SIZE{
    ssz::SYNC_COMMITTEE_SIZE * 48 + 48};
inline constexpr size_t MAX_BRIDGE_BRANCH_WIRE_SIZE{
    1 + MAX_BRIDGE_MERKLE_BRANCH_NODES * 32};
inline constexpr size_t MAX_BRIDGE_BOOTSTRAP_WIRE_SIZE{
    2 + MAX_BRIDGE_HEADER_WIRE_SIZE + BRIDGE_SYNC_COMMITTEE_WIRE_SIZE +
    MAX_BRIDGE_BRANCH_WIRE_SIZE};
inline constexpr size_t MAX_BRIDGE_UPDATE_WIRE_SIZE{
    2 + 2 * MAX_BRIDGE_HEADER_WIRE_SIZE + MAX_BRIDGE_BRANCH_WIRE_SIZE + 1 +
    BRIDGE_SYNC_COMMITTEE_WIRE_SIZE + MAX_BRIDGE_BRANCH_WIRE_SIZE + 64 + 96 +
    8};
inline constexpr size_t MAX_BRIDGE_MINT_WIRE_SIZE{
    2 + 32 + 4 + 32 + 8 + 8 + 4 + 1 +
    2 * MAX_BRIDGE_EXECUTION_ANCESTRY_HEADERS +
    MAX_BRIDGE_EXECUTION_ANCESTRY_BYTES + 1 + 2 * MAX_BRIDGE_MPT_NODES +
    MAX_BRIDGE_MPT_BYTES};

static_assert(MAX_BRIDGE_RECORD_SIZE <= MAX_PAYLOAD_RECORD_SIZE);
static_assert(MAX_BRIDGE_HEADER_WIRE_SIZE <=
              MAX_BRIDGE_LIGHT_CLIENT_HEADER_SIZE);
static_assert(MAX_BRIDGE_BOOTSTRAP_WIRE_SIZE <= MAX_BRIDGE_RECORD_SIZE);
static_assert(MAX_BRIDGE_UPDATE_WIRE_SIZE <= MAX_BRIDGE_RECORD_SIZE);
static_assert(MAX_BRIDGE_MINT_WIRE_SIZE <= MAX_BRIDGE_RECORD_SIZE);

enum class BridgeRecordKindV1 : uint8_t {
    BOOTSTRAP = 1,
    UPDATE = 2,
    MINT = 3,
    EXECUTION_BACKFILL = 4,
    MANAGED_WITHDRAWAL = 5,
    BRIDGE_BURN = 6,
};

struct BridgeBootstrapV1 {
    LightClientHeader header{};
    ssz::SyncCommittee current_committee{};
    std::vector<uint256> current_committee_branch{};

    friend bool operator==(const BridgeBootstrapV1&,
                           const BridgeBootstrapV1&) = default;
};

struct BridgeUpdateV1 {
    LightClientUpdate update{};

    friend bool operator==(const BridgeUpdateV1&, const BridgeUpdateV1&) = default;
};

struct BridgeMintV1 {
    uint256 registry_id{};
    uint32_t output_index{0};
    uint256 finalized_anchor_hash{};
    uint64_t target_block_number{0};
    uint64_t tx_index{0};
    uint32_t receipt_log_index{0};
    /** Newest first; element zero is the full header hashing to the anchor. */
    std::vector<std::vector<unsigned char>> ancestry_headers{};
    std::vector<std::vector<unsigned char>> mpt_nodes{};

    friend bool operator==(const BridgeMintV1&, const BridgeMintV1&) = default;
};

struct BridgeExecutionBackfillV1 {
    uint256 finalized_anchor_hash{};
    uint64_t target_block_number{0};
    /** Newest first; element zero is the full header hashing to the anchor. */
    std::vector<std::vector<unsigned char>> ancestry_headers{};

    friend bool operator==(const BridgeExecutionBackfillV1&,
                           const BridgeExecutionBackfillV1&) = default;
};

struct BridgeManagedWithdrawalV1 {
    uint256 registry_id{};
    uint32_t burn_output_index{0};
    uint64_t raw_amount{0};
    EthAddress ethereum_recipient{};

    friend bool operator==(const BridgeManagedWithdrawalV1&,
                           const BridgeManagedWithdrawalV1&) = default;
};

/**
 * Decentralized reserve release request. The sequential withdrawal id, B3
 * height, origin chain/token and chain-bound asset id are deliberately absent:
 * consensus derives them while connecting the exact named burn. A relayer
 * therefore cannot choose any field of the Ethereum withdrawal leaf.
 */
struct BridgeBurnV1 {
    uint256 registry_id{};
    uint32_t burn_output_index{0};
    uint64_t raw_amount{0};
    EthAddress ethereum_recipient{};

    friend bool operator==(const BridgeBurnV1&, const BridgeBurnV1&) = default;
};

using BridgeRecordPayloadV1 =
    std::variant<BridgeBootstrapV1, BridgeUpdateV1, BridgeMintV1,
                 BridgeExecutionBackfillV1, BridgeManagedWithdrawalV1,
                 BridgeBurnV1>;

struct BridgeRecordV1 {
    BridgeRecordKindV1 kind{BridgeRecordKindV1::BOOTSTRAP};
    BridgeRecordPayloadV1 payload{BridgeBootstrapV1{}};

    friend bool operator==(const BridgeRecordV1&, const BridgeRecordV1&) = default;
};

// Compatibility vocabulary for callers that describe a type-10 record as a
// proof. Record is the primary name because managed withdrawals are commands,
// not Ethereum inclusion proofs.
using BridgeProofKindV1 = BridgeRecordKindV1;
using BridgeProofPayloadV1 = BridgeRecordPayloadV1;
using BridgeProofV1 = BridgeRecordV1;

namespace proof_detail {

class Writer {
public:
    void U8(const uint8_t value) { m_out.push_back(value); }

    void U16(const uint16_t value)
    {
        m_out.push_back(static_cast<unsigned char>(value >> 8));
        m_out.push_back(static_cast<unsigned char>(value));
    }

    void U32(const uint32_t value)
    {
        for (int shift{24}; shift >= 0; shift -= 8) {
            m_out.push_back(static_cast<unsigned char>(value >> shift));
        }
    }

    void U64(const uint64_t value)
    {
        for (int shift{56}; shift >= 0; shift -= 8) {
            m_out.push_back(static_cast<unsigned char>(value >> shift));
        }
    }

    void Bytes(const std::span<const unsigned char> bytes)
    {
        m_out.insert(m_out.end(), bytes.begin(), bytes.end());
    }

    void Root(const uint256& root)
    {
        Bytes(std::span<const unsigned char>{root.begin(), 32});
    }

    std::vector<unsigned char> Take() { return std::move(m_out); }

private:
    std::vector<unsigned char> m_out{};
};

class Reader {
public:
    explicit Reader(const std::span<const unsigned char> in) : m_in{in} {}

    size_t Remaining() const { return m_in.size() - m_pos; }
    bool Empty() const { return m_pos == m_in.size(); }

    bool U8(uint8_t& out)
    {
        if (Remaining() < 1) return false;
        out = m_in[m_pos++];
        return true;
    }

    bool U16(uint16_t& out)
    {
        if (Remaining() < 2) return false;
        out = (uint16_t{m_in[m_pos]} << 8) | uint16_t{m_in[m_pos + 1]};
        m_pos += 2;
        return true;
    }

    bool U32(uint32_t& out)
    {
        if (Remaining() < 4) return false;
        out = 0;
        for (size_t i{0}; i < 4; ++i) out = (out << 8) | m_in[m_pos + i];
        m_pos += 4;
        return true;
    }

    bool U64(uint64_t& out)
    {
        if (Remaining() < 8) return false;
        out = 0;
        for (size_t i{0}; i < 8; ++i) out = (out << 8) | m_in[m_pos + i];
        m_pos += 8;
        return true;
    }

    bool Bytes(const std::span<unsigned char> out)
    {
        if (out.size() > Remaining()) return false;
        std::copy_n(m_in.begin() + m_pos, out.size(), out.begin());
        m_pos += out.size();
        return true;
    }

    bool Vector(const size_t size, std::vector<unsigned char>& out)
    {
        if (size > Remaining()) return false;
        out.resize(size);
        return Bytes(out);
    }

    bool Root(uint256& out)
    {
        if (Remaining() < 32) return false;
        out = uint256{m_in.subspan(m_pos, 32)};
        m_pos += 32;
        return true;
    }

private:
    std::span<const unsigned char> m_in;
    size_t m_pos{0};
};

inline bool EncodeBranch(Writer& writer, const std::vector<uint256>& branch,
                         const bool require_nonempty = true)
{
    if ((require_nonempty && branch.empty()) ||
        branch.size() > MAX_BRIDGE_MERKLE_BRANCH_NODES) {
        return false;
    }
    writer.U8(static_cast<uint8_t>(branch.size()));
    for (const uint256& node : branch) writer.Root(node);
    return true;
}

inline bool DecodeBranch(Reader& reader, std::vector<uint256>& branch,
                         const bool require_nonempty = true)
{
    uint8_t count{0};
    if (!reader.U8(count) || (require_nonempty && count == 0) ||
        count > MAX_BRIDGE_MERKLE_BRANCH_NODES ||
        size_t{count} * 32 > reader.Remaining()) {
        return false;
    }
    branch.clear();
    branch.reserve(count);
    for (uint8_t i{0}; i < count; ++i) {
        uint256 node;
        if (!reader.Root(node)) return false;
        branch.push_back(node);
    }
    return true;
}

inline bool EncodeExecutionHeader(Writer& writer,
                                  const ssz::ExecutionPayloadHeader& header)
{
    if (!header.ValidForHashTreeRoot()) return false;
    writer.Root(header.parent_hash);
    writer.Bytes(header.fee_recipient);
    writer.Root(header.state_root);
    writer.Root(header.receipts_root);
    writer.Bytes(header.logs_bloom);
    writer.Root(header.prev_randao);
    writer.U64(header.block_number);
    writer.U64(header.gas_limit);
    writer.U64(header.gas_used);
    writer.U64(header.timestamp);
    writer.U8(static_cast<uint8_t>(header.extra_data.size()));
    writer.Bytes(header.extra_data);
    writer.Root(header.base_fee_per_gas);
    writer.Root(header.block_hash);
    writer.Root(header.transactions_root);
    writer.Root(header.withdrawals_root);
    writer.U64(header.blob_gas_used);
    writer.U64(header.excess_blob_gas);
    return true;
}

inline bool DecodeExecutionHeader(Reader& reader,
                                  ssz::ExecutionPayloadHeader& header)
{
    uint8_t extra_size{0};
    if (!reader.Root(header.parent_hash) || !reader.Bytes(header.fee_recipient) ||
        !reader.Root(header.state_root) || !reader.Root(header.receipts_root) ||
        !reader.Bytes(header.logs_bloom) || !reader.Root(header.prev_randao) ||
        !reader.U64(header.block_number) || !reader.U64(header.gas_limit) ||
        !reader.U64(header.gas_used) || !reader.U64(header.timestamp) ||
        !reader.U8(extra_size) || extra_size > 32 ||
        !reader.Vector(extra_size, header.extra_data) ||
        !reader.Root(header.base_fee_per_gas) || !reader.Root(header.block_hash) ||
        !reader.Root(header.transactions_root) ||
        !reader.Root(header.withdrawals_root) ||
        !reader.U64(header.blob_gas_used) ||
        !reader.U64(header.excess_blob_gas)) {
        return false;
    }
    return true;
}

inline bool EncodeHeader(Writer& writer, const LightClientHeader& header)
{
    if (header.execution_branch.size() != BRIDGE_EXECUTION_BRANCH_NODES) {
        return false;
    }
    writer.U64(header.beacon.slot);
    writer.U64(header.beacon.proposer_index);
    writer.Root(header.beacon.parent_root);
    writer.Root(header.beacon.state_root);
    writer.Root(header.beacon.body_root);
    if (!EncodeExecutionHeader(writer, header.execution)) return false;
    for (const uint256& node : header.execution_branch) writer.Root(node);
    return true;
}

inline bool DecodeHeader(Reader& reader, LightClientHeader& header)
{
    if (!reader.U64(header.beacon.slot) ||
        !reader.U64(header.beacon.proposer_index) ||
        !reader.Root(header.beacon.parent_root) ||
        !reader.Root(header.beacon.state_root) ||
        !reader.Root(header.beacon.body_root) ||
        !DecodeExecutionHeader(reader, header.execution) ||
        size_t{BRIDGE_EXECUTION_BRANCH_NODES} * 32 > reader.Remaining()) {
        return false;
    }
    header.execution_branch.resize(BRIDGE_EXECUTION_BRANCH_NODES);
    for (uint256& node : header.execution_branch) {
        if (!reader.Root(node)) return false;
    }
    return true;
}

inline bool EncodeCommittee(Writer& writer, const ssz::SyncCommittee& committee)
{
    if (committee.pubkeys.size() != ssz::SYNC_COMMITTEE_SIZE) return false;
    for (const auto& key : committee.pubkeys) writer.Bytes(key);
    writer.Bytes(committee.aggregate_pubkey);
    return true;
}

inline bool DecodeCommittee(Reader& reader, ssz::SyncCommittee& committee)
{
    if (reader.Remaining() < BRIDGE_SYNC_COMMITTEE_WIRE_SIZE) return false;
    committee.pubkeys.resize(ssz::SYNC_COMMITTEE_SIZE);
    for (auto& key : committee.pubkeys) {
        if (!reader.Bytes(key)) return false;
    }
    return reader.Bytes(committee.aggregate_pubkey);
}

inline bool AbsentCommitteeCanonical(const ssz::SyncCommittee& committee)
{
    return committee.pubkeys.empty() &&
           std::all_of(committee.aggregate_pubkey.begin(),
                       committee.aggregate_pubkey.end(),
                       [](const unsigned char byte) { return byte == 0; });
}

inline bool EncodeUpdate(Writer& writer, const LightClientUpdate& update)
{
    if (!EncodeHeader(writer, update.attested) ||
        !EncodeHeader(writer, update.finalized) ||
        !EncodeBranch(writer, update.finality_branch)) {
        return false;
    }
    writer.U8(update.has_next ? 1 : 0);
    if (update.has_next) {
        if (!EncodeCommittee(writer, update.next_committee) ||
            !EncodeBranch(writer, update.next_branch)) {
            return false;
        }
    } else if (!AbsentCommitteeCanonical(update.next_committee) ||
               !update.next_branch.empty()) {
        return false;
    }
    writer.Bytes(update.sync_aggregate.bits);
    writer.Bytes(update.sync_aggregate.signature);
    writer.U64(update.signature_slot);
    return true;
}

inline bool DecodeUpdate(Reader& reader, LightClientUpdate& update)
{
    uint8_t has_next{0};
    if (!DecodeHeader(reader, update.attested) ||
        !DecodeHeader(reader, update.finalized) ||
        !DecodeBranch(reader, update.finality_branch) ||
        !reader.U8(has_next) || has_next > 1) {
        return false;
    }
    update.has_next = has_next == 1;
    if (update.has_next) {
        if (!DecodeCommittee(reader, update.next_committee) ||
            !DecodeBranch(reader, update.next_branch)) {
            return false;
        }
    } else {
        update.next_committee = {};
        update.next_branch.clear();
    }
    return reader.Bytes(update.sync_aggregate.bits) &&
           reader.Bytes(update.sync_aggregate.signature) &&
           reader.U64(update.signature_slot);
}

inline bool EncodeBlobVector(Writer& writer,
                             const std::vector<std::vector<unsigned char>>& blobs,
                             const size_t max_count, const size_t max_total,
                             const bool require_nonempty)
{
    if ((require_nonempty && blobs.empty()) || blobs.size() > max_count) {
        return false;
    }
    size_t total{0};
    for (const auto& blob : blobs) {
        if (blob.empty() || blob.size() > MAX_BRIDGE_RLP_ITEM_SIZE ||
            blob.size() > max_total - total) {
            return false;
        }
        total += blob.size();
    }
    writer.U8(static_cast<uint8_t>(blobs.size()));
    for (const auto& blob : blobs) {
        writer.U16(static_cast<uint16_t>(blob.size()));
        writer.Bytes(blob);
    }
    return true;
}

inline bool DecodeBlobVector(Reader& reader,
                             std::vector<std::vector<unsigned char>>& blobs,
                             const size_t max_count, const size_t max_total,
                             const bool require_nonempty)
{
    uint8_t count{0};
    if (!reader.U8(count) || (require_nonempty && count == 0) ||
        count > max_count || size_t{count} * 2 > reader.Remaining()) {
        return false;
    }
    blobs.clear();
    blobs.reserve(count);
    size_t total{0};
    for (uint8_t i{0}; i < count; ++i) {
        uint16_t size{0};
        if (!reader.U16(size) || size == 0 || size > MAX_BRIDGE_RLP_ITEM_SIZE ||
            size > max_total - total || size > reader.Remaining()) {
            return false;
        }
        std::vector<unsigned char> blob;
        if (!reader.Vector(size, blob)) return false;
        total += size;
        blobs.push_back(std::move(blob));
    }
    return true;
}

inline bool AddressNonzero(const EthAddress& address)
{
    return std::any_of(address.begin(), address.end(),
                       [](const unsigned char byte) { return byte != 0; });
}

} // namespace proof_detail

/** Standalone canonical component codec used by durable light-client state. */
inline std::optional<std::vector<unsigned char>> EncodeBridgeLightClientHeaderV1(
    const LightClientHeader& header)
{
    proof_detail::Writer writer;
    if (!proof_detail::EncodeHeader(writer, header)) return std::nullopt;
    auto encoded{writer.Take()};
    if (encoded.size() > MAX_BRIDGE_LIGHT_CLIENT_HEADER_SIZE) return std::nullopt;
    return encoded;
}

inline std::optional<LightClientHeader> DecodeBridgeLightClientHeaderV1(
    const std::span<const unsigned char> encoded)
{
    if (encoded.empty() || encoded.size() > MAX_BRIDGE_LIGHT_CLIENT_HEADER_SIZE) {
        return std::nullopt;
    }
    proof_detail::Reader reader{encoded};
    LightClientHeader header;
    if (!proof_detail::DecodeHeader(reader, header) || !reader.Empty()) {
        return std::nullopt;
    }
    return header;
}

/** Exact-size component codec: there is no committee count on the wire. */
inline std::optional<std::vector<unsigned char>> EncodeBridgeSyncCommitteeV1(
    const ssz::SyncCommittee& committee)
{
    proof_detail::Writer writer;
    if (!proof_detail::EncodeCommittee(writer, committee)) return std::nullopt;
    return writer.Take();
}

inline std::optional<ssz::SyncCommittee> DecodeBridgeSyncCommitteeV1(
    const std::span<const unsigned char> encoded)
{
    if (encoded.size() != BRIDGE_SYNC_COMMITTEE_WIRE_SIZE) return std::nullopt;
    proof_detail::Reader reader{encoded};
    ssz::SyncCommittee committee;
    if (!proof_detail::DecodeCommittee(reader, committee) || !reader.Empty()) {
        return std::nullopt;
    }
    return committee;
}

inline std::optional<std::vector<unsigned char>> EncodeBridgeRecordV1(
    const BridgeRecordV1& record)
{
    using namespace proof_detail;
    Writer writer;
    writer.U8(static_cast<uint8_t>(record.kind));
    writer.U8(0); // reserved; every future flag requires a new reviewed version

    bool valid{false};
    switch (record.kind) {
    case BridgeRecordKindV1::BOOTSTRAP:
        if (const auto* value{std::get_if<BridgeBootstrapV1>(&record.payload)}) {
            valid = EncodeHeader(writer, value->header) &&
                    EncodeCommittee(writer, value->current_committee) &&
                    EncodeBranch(writer, value->current_committee_branch);
        }
        break;
    case BridgeRecordKindV1::UPDATE:
        if (const auto* value{std::get_if<BridgeUpdateV1>(&record.payload)}) {
            valid = EncodeUpdate(writer, value->update);
        }
        break;
    case BridgeRecordKindV1::MINT:
        if (const auto* value{std::get_if<BridgeMintV1>(&record.payload)}) {
            if (value->registry_id.IsNull() ||
                value->finalized_anchor_hash.IsNull() ||
                value->target_block_number == 0) {
                break;
            }
            writer.Root(value->registry_id);
            writer.U32(value->output_index);
            writer.Root(value->finalized_anchor_hash);
            writer.U64(value->target_block_number);
            writer.U64(value->tx_index);
            writer.U32(value->receipt_log_index);
            valid = EncodeBlobVector(
                        writer, value->ancestry_headers,
                        MAX_BRIDGE_EXECUTION_ANCESTRY_HEADERS,
                        MAX_BRIDGE_EXECUTION_ANCESTRY_BYTES,
                        /*require_nonempty=*/true) &&
                    EncodeBlobVector(writer, value->mpt_nodes,
                                     MAX_BRIDGE_MPT_NODES,
                                     MAX_BRIDGE_MPT_BYTES,
                                     /*require_nonempty=*/true);
        }
        break;
    case BridgeRecordKindV1::EXECUTION_BACKFILL:
        if (const auto* value{
                std::get_if<BridgeExecutionBackfillV1>(&record.payload)}) {
            if (value->finalized_anchor_hash.IsNull() ||
                value->target_block_number == 0) {
                break;
            }
            writer.Root(value->finalized_anchor_hash);
            writer.U64(value->target_block_number);
            valid = EncodeBlobVector(
                writer, value->ancestry_headers,
                MAX_BRIDGE_EXECUTION_ANCESTRY_HEADERS,
                MAX_BRIDGE_EXECUTION_ANCESTRY_BYTES,
                /*require_nonempty=*/true);
        }
        break;
    case BridgeRecordKindV1::MANAGED_WITHDRAWAL:
        if (const auto* value{
                std::get_if<BridgeManagedWithdrawalV1>(&record.payload)}) {
            if (value->registry_id.IsNull() || value->raw_amount == 0 ||
                !AddressNonzero(value->ethereum_recipient)) {
                break;
            }
            writer.Root(value->registry_id);
            writer.U32(value->burn_output_index);
            writer.U64(value->raw_amount);
            writer.Bytes(value->ethereum_recipient);
            valid = true;
        }
        break;
    case BridgeRecordKindV1::BRIDGE_BURN:
        if (const auto* value{std::get_if<BridgeBurnV1>(&record.payload)}) {
            if (value->registry_id.IsNull() || value->raw_amount == 0 ||
                !AddressNonzero(value->ethereum_recipient)) {
                break;
            }
            writer.Root(value->registry_id);
            writer.U32(value->burn_output_index);
            writer.U64(value->raw_amount);
            writer.Bytes(value->ethereum_recipient);
            valid = true;
        }
        break;
    }

    if (!valid) return std::nullopt;
    auto encoded{writer.Take()};
    if (encoded.size() > MAX_BRIDGE_RECORD_SIZE) return std::nullopt;
    return encoded;
}

inline std::optional<BridgeRecordV1> DecodeBridgeRecordV1(
    const std::span<const unsigned char> encoded)
{
    using namespace proof_detail;
    if (encoded.size() < 2 || encoded.size() > MAX_BRIDGE_RECORD_SIZE) {
        return std::nullopt;
    }
    Reader reader{encoded};
    uint8_t raw_kind{0};
    uint8_t reserved{0};
    if (!reader.U8(raw_kind) || !reader.U8(reserved) || reserved != 0) {
        return std::nullopt;
    }
    const auto kind{static_cast<BridgeRecordKindV1>(raw_kind)};

    switch (kind) {
    case BridgeRecordKindV1::BOOTSTRAP: {
        BridgeBootstrapV1 value;
        if (!DecodeHeader(reader, value.header) ||
            !DecodeCommittee(reader, value.current_committee) ||
            !DecodeBranch(reader, value.current_committee_branch) ||
            !reader.Empty()) {
            return std::nullopt;
        }
        return BridgeRecordV1{kind, std::move(value)};
    }
    case BridgeRecordKindV1::UPDATE: {
        BridgeUpdateV1 value;
        if (!DecodeUpdate(reader, value.update) || !reader.Empty()) {
            return std::nullopt;
        }
        return BridgeRecordV1{kind, std::move(value)};
    }
    case BridgeRecordKindV1::MINT: {
        BridgeMintV1 value;
        if (!reader.Root(value.registry_id) || value.registry_id.IsNull() ||
            !reader.U32(value.output_index) ||
            !reader.Root(value.finalized_anchor_hash) ||
            value.finalized_anchor_hash.IsNull() ||
            !reader.U64(value.target_block_number) ||
            value.target_block_number == 0 || !reader.U64(value.tx_index) ||
            !reader.U32(value.receipt_log_index) ||
            !DecodeBlobVector(reader, value.ancestry_headers,
                              MAX_BRIDGE_EXECUTION_ANCESTRY_HEADERS,
                              MAX_BRIDGE_EXECUTION_ANCESTRY_BYTES,
                              /*require_nonempty=*/true) ||
            !DecodeBlobVector(reader, value.mpt_nodes, MAX_BRIDGE_MPT_NODES,
                              MAX_BRIDGE_MPT_BYTES,
                              /*require_nonempty=*/true) ||
            !reader.Empty()) {
            return std::nullopt;
        }
        return BridgeRecordV1{kind, std::move(value)};
    }
    case BridgeRecordKindV1::EXECUTION_BACKFILL: {
        BridgeExecutionBackfillV1 value;
        if (!reader.Root(value.finalized_anchor_hash) ||
            value.finalized_anchor_hash.IsNull() ||
            !reader.U64(value.target_block_number) ||
            value.target_block_number == 0 ||
            !DecodeBlobVector(reader, value.ancestry_headers,
                              MAX_BRIDGE_EXECUTION_ANCESTRY_HEADERS,
                              MAX_BRIDGE_EXECUTION_ANCESTRY_BYTES,
                              /*require_nonempty=*/true) ||
            !reader.Empty()) {
            return std::nullopt;
        }
        return BridgeRecordV1{kind, std::move(value)};
    }
    case BridgeRecordKindV1::MANAGED_WITHDRAWAL: {
        BridgeManagedWithdrawalV1 value;
        if (!reader.Root(value.registry_id) || value.registry_id.IsNull() ||
            !reader.U32(value.burn_output_index) ||
            !reader.U64(value.raw_amount) || value.raw_amount == 0 ||
            !reader.Bytes(value.ethereum_recipient) ||
            !AddressNonzero(value.ethereum_recipient) || !reader.Empty()) {
            return std::nullopt;
        }
        return BridgeRecordV1{kind, std::move(value)};
    }
    case BridgeRecordKindV1::BRIDGE_BURN: {
        BridgeBurnV1 value;
        if (!reader.Root(value.registry_id) || value.registry_id.IsNull() ||
            !reader.U32(value.burn_output_index) ||
            !reader.U64(value.raw_amount) || value.raw_amount == 0 ||
            !reader.Bytes(value.ethereum_recipient) ||
            !AddressNonzero(value.ethereum_recipient) || !reader.Empty()) {
            return std::nullopt;
        }
        return BridgeRecordV1{kind, std::move(value)};
    }
    }
    return std::nullopt;
}

inline std::optional<CMpaRecord> MakeBridgeMpaRecord(
    const BridgeRecordV1& record)
{
    auto encoded{EncodeBridgeRecordV1(record)};
    if (!encoded) return std::nullopt;
    return CMpaRecord{BRIDGE_MPA_TYPE, BRIDGE_MPA_VERSION_V1,
                      std::move(*encoded)};
}

inline std::optional<BridgeRecordV1> DecodeBridgeMpaRecordV1(
    const CMpaRecord& record)
{
    if (record.payload_type != BRIDGE_MPA_TYPE ||
        record.payload_version != BRIDGE_MPA_VERSION_V1) {
        return std::nullopt;
    }
    return DecodeBridgeRecordV1(record.payload);
}

inline std::optional<std::vector<unsigned char>> EncodeBridgeProofV1(
    const BridgeProofV1& proof)
{
    return EncodeBridgeRecordV1(proof);
}

inline std::optional<BridgeProofV1> DecodeBridgeProofV1(
    const std::span<const unsigned char> encoded)
{
    return DecodeBridgeRecordV1(encoded);
}

} // namespace bridge

#endif // B3COIN_BRIDGE_PROOF_H
