// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_BRIDGE_PROOF_H
#define B3COIN_BRIDGE_PROOF_H

#include <bridge/deposit.h>
#include <bridge/eth_light_client.h>
#include <consensus/bridge_params.h>
#include <consensus/consensus.h>
#include <modern/finality_v2_payload.h>
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
// V1 only: a V1 mint always fits the record. The V2 receipt limits below can
// sum past it (ancestry 20 KiB + receipt proof 16 KiB), so for V2 the 32 KiB
// record bound itself, enforced by both codec directions, caps that sum.
static_assert(MAX_BRIDGE_MINT_WIRE_SIZE <= MAX_BRIDGE_RECORD_SIZE);

/**
 * Size limits on a kind-3 MINT's receipt proof (`mpt_nodes`). The terminal
 * node is the LAST element: VerifyMptProof walks root-first, must consume
 * every node and ends on the receipt value, so the receipt-bearing leaf is
 * always the final node. Ancestry headers, backfills, the 64-node count and
 * the 32 KiB record cap are the same under every limit set.
 *
 * The limit set is the codec's height selector, so it also carries the
 * record set: `v2_kinds` admits the bridge V2 record kinds (kind 9,
 * ETH_STATE_OBSERVATION, kind 13, FINALITY_CONFLICT, and kind 16,
 * VAULT_SWEEP). Consensus passes the V2 set from V only, so a kind 9, 13 or
 * 16 record is malformed below V and on every network without bridge_v2,
 * exactly as an unknown kind was before.
 */
struct BridgeMptLimits {
    size_t max_node_size;
    size_t max_terminal_node_size;
    size_t max_total_bytes;
    bool v2_kinds{false};
};

//! Today's exact receipt-proof limits and record set, used below the bridge
//! V2 height.
inline constexpr BridgeMptLimits BRIDGE_MPT_LIMITS_V1{
    MAX_BRIDGE_RLP_ITEM_SIZE, MAX_BRIDGE_RLP_ITEM_SIZE, MAX_BRIDGE_MPT_BYTES,
    /*v2_kinds=*/false};
//! Bridge V2 (doc/design/b3-bridge-v2-node-changes.md section 4): a terminal
//! receipt node up to 8,192 bytes and a receipt proof up to 16 KiB; every
//! other node keeps the 1,024-byte limit. Kinds 9 (section 8.2), 13
//! (section 8.5) and 14 (section 7.6) are valid.
inline constexpr size_t MAX_BRIDGE_TERMINAL_RECEIPT_NODE_SIZE_V2{8'192};
inline constexpr size_t MAX_BRIDGE_MPT_BYTES_V2{16'384};
inline constexpr BridgeMptLimits BRIDGE_MPT_LIMITS_V2{
    MAX_BRIDGE_RLP_ITEM_SIZE, MAX_BRIDGE_TERMINAL_RECEIPT_NODE_SIZE_V2,
    MAX_BRIDGE_MPT_BYTES_V2, /*v2_kinds=*/true};

// Blob lengths travel as u16, and V2 only widens V1.
static_assert(MAX_BRIDGE_TERMINAL_RECEIPT_NODE_SIZE_V2 <= 0xffff);
static_assert(BRIDGE_MPT_LIMITS_V2.max_node_size ==
              BRIDGE_MPT_LIMITS_V1.max_node_size);
static_assert(BRIDGE_MPT_LIMITS_V2.max_terminal_node_size >=
              BRIDGE_MPT_LIMITS_V1.max_terminal_node_size);
static_assert(BRIDGE_MPT_LIMITS_V2.max_total_bytes >=
              BRIDGE_MPT_LIMITS_V1.max_total_bytes);
static_assert(BRIDGE_MPT_LIMITS_V2.max_terminal_node_size <=
              BRIDGE_MPT_LIMITS_V2.max_total_bytes);

enum class BridgeRecordKindV1 : uint8_t {
    BOOTSTRAP = 1,
    UPDATE = 2,
    MINT = 3,
    EXECUTION_BACKFILL = 4,
    MANAGED_WITHDRAWAL = 5,
    BRIDGE_BURN = 6,
    //! Bridge V2 only (node changes section 8.2): decoded under the V2
    //! record set (BridgeMptLimits::v2_kinds), malformed otherwise.
    ETH_STATE_OBSERVATION = 9,
    //! Bridge V2 only (node changes section 8.5): decoded under the V2
    //! record set (BridgeMptLimits::v2_kinds), malformed otherwise.
    FINALITY_CONFLICT = 13,
    //! Bridge V2 only (node changes section 7.6, B-23 migration): decoded
    //! under the V2 record set (BridgeMptLimits::v2_kinds), malformed
    //! otherwise. Kinds 7, 8 and 10-12 stay reserved (node changes section
    //! 17), and so do 14 and 15, which the staker-signed fast deposit
    //! proposal names FSD_MINT and FSD_FAULT
    //! (doc/design/b3-bridge-v2-staker-signed-deposits.md); the sweep takes
    //! the next free kind.
    VAULT_SWEEP = 16,
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

/**
 * Kind 9 ETH_STATE_OBSERVATION subjects and verdict bits (node changes
 * section 8.2; the gate rows that read them are section 8.4).
 *
 * - VERIFIER_E reads Verifier V2 slots 0, 1, 2, 4 and 6 (E1-E6). Its
 *   verdict byte records the B3-side comparison: bit 0 SETS_MISMATCH (E4)
 *   and bit 1 CERT_CONFLICT (E5). Consensus recomputes and must agree.
 * - RESERVE_E reads Vault V2 slots 0, 1 and 3 and the token's
 *   balances[vault] (E8, wind-down and transit crediting).
 * - TOKEN_FLAGS reads the token's slots 0 and 10 and isBlackListed[vault]
 *   (E7).
 * - RESERVE_E_SUCCESSOR (0x12) and TOKEN_FLAGS_SUCCESSOR (0x13) are RESERVE_E
 *   and TOKEN_FLAGS of the B-23 successor vault v' (node changes section
 *   7.6): the base subject | (vault entry index << 4), with the same shape,
 *   read with v''s pins (bridge::ObservationPinsFromParams). The verifier
 *   is shared, so VERIFIER_E has no successor form.
 *
 * Every other subject value is malformed (0, 0x04-0x11 and 0x14-0xff), and
 * so is a verdict with any other bit or a non-zero verdict on a subject
 * other than VERIFIER_E. Whether a successor subject may appear in a block
 * (from the migration height only) is a consensus rule, not the codec's.
 */
inline constexpr uint8_t ETH_OBSERVATION_SUBJECT_VERIFIER{1};
inline constexpr uint8_t ETH_OBSERVATION_SUBJECT_RESERVE{2};
inline constexpr uint8_t ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS{3};
//! The vault entry index of a subject sits in its high nibble.
inline constexpr unsigned ETH_OBSERVATION_SUBJECT_ENTRY_SHIFT{4};
inline constexpr uint8_t ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR{
    ETH_OBSERVATION_SUBJECT_RESERVE |
    (static_cast<uint8_t>(Consensus::BridgeV2EntryRole::SUCCESSOR)
     << ETH_OBSERVATION_SUBJECT_ENTRY_SHIFT)};
inline constexpr uint8_t ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS_SUCCESSOR{
    ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS |
    (static_cast<uint8_t>(Consensus::BridgeV2EntryRole::SUCCESSOR)
     << ETH_OBSERVATION_SUBJECT_ENTRY_SHIFT)};
static_assert(ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR == 0x12);
static_assert(ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS_SUCCESSOR == 0x13);
inline constexpr uint8_t ETH_OBSERVATION_VERDICT_SETS_MISMATCH{0x01};
inline constexpr uint8_t ETH_OBSERVATION_VERDICT_CERT_CONFLICT{0x02};
inline constexpr uint8_t ETH_OBSERVATION_VERDICT_MASK{0x03};

/**
 * The subject a kind 9 subject reads as: VERIFIER_E, RESERVE_E and
 * TOKEN_FLAGS map to themselves, RESERVE_E_SUCCESSOR to RESERVE_E and
 * TOKEN_FLAGS_SUCCESSOR to TOKEN_FLAGS. Zero for every other value.
 */
inline constexpr uint8_t EthObservationBaseSubject(const uint8_t subject)
{
    switch (subject) {
    case ETH_OBSERVATION_SUBJECT_VERIFIER:
    case ETH_OBSERVATION_SUBJECT_RESERVE:
    case ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS:
        return subject;
    case ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR:
        return ETH_OBSERVATION_SUBJECT_RESERVE;
    case ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS_SUCCESSOR:
        return ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS;
    }
    return 0;
}

/**
 * The vault entry whose pins a subject is read with: SUCCESSOR for the two
 * successor subjects, LAUNCH for every other value (VERIFIER_E reads the
 * shared verifier with the launch pins). Meaningful only for a subject
 * whose EthObservationBaseSubject is non-zero.
 */
inline constexpr Consensus::BridgeV2EntryRole EthObservationEntryRole(
    const uint8_t subject)
{
    return subject == ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR ||
                   subject == ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS_SUCCESSOR
               ? Consensus::BridgeV2EntryRole::SUCCESSOR
               : Consensus::BridgeV2EntryRole::LAUNCH;
}

namespace proof_detail {
inline constexpr std::array<uint8_t, 1> ETH_OBSERVATION_SHAPE_VERIFIER{5};
inline constexpr std::array<uint8_t, 2> ETH_OBSERVATION_SHAPE_RESERVE{3, 1};
inline constexpr std::array<uint8_t, 1> ETH_OBSERVATION_SHAPE_TOKEN_FLAGS{3};
} // namespace proof_detail

/**
 * The proof shape a subject requires: one entry per account in wire order,
 * each the number of storage slots proved for it. VERIFIER_E is
 * [verifier: 5]; RESERVE_E and RESERVE_E_SUCCESSOR are [vault: 3, token: 1];
 * TOKEN_FLAGS and TOKEN_FLAGS_SUCCESSOR are [token: 3]. Empty for an unknown
 * subject.
 */
inline std::span<const uint8_t> EthObservationShapeV2(const uint8_t subject)
{
    switch (EthObservationBaseSubject(subject)) {
    case ETH_OBSERVATION_SUBJECT_VERIFIER:
        return proof_detail::ETH_OBSERVATION_SHAPE_VERIFIER;
    case ETH_OBSERVATION_SUBJECT_RESERVE:
        return proof_detail::ETH_OBSERVATION_SHAPE_RESERVE;
    case ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS:
        return proof_detail::ETH_OBSERVATION_SHAPE_TOKEN_FLAGS;
    }
    return {};
}

/**
 * One account's EIP-1186 proof: the state-trie nodes proving the account
 * (eth_getProof accountProof, 1-64 nodes) and, per slot of the subject's
 * shape, the storage-trie nodes proving that slot (storageProof[i].proof,
 * 0-64 nodes; zero nodes only prove a slot of an empty storage trie). Each
 * node is at most 1,024 bytes and each node list at most 11,264 bytes.
 */
struct BridgeEthAccountProofV2 {
    std::vector<std::vector<unsigned char>> account_nodes{};
    std::vector<std::vector<std::vector<unsigned char>>> storage_nodes{};

    friend bool operator==(const BridgeEthAccountProofV2&,
                           const BridgeEthAccountProofV2&) = default;
};

/**
 * Kind 9 body. Addresses, slot keys and values never travel: the subject
 * and the bridge V2 pins fix the accounts and slots, and the proofs carry
 * the values, all checked against the state root of the retained V2
 * execution anchor `anchor_hash` (raw EVM byte order).
 */
struct BridgeEthStateObservationV2 {
    uint8_t subject{0};
    uint256 anchor_hash{};
    uint8_t verdict{0};
    std::vector<BridgeEthAccountProofV2> accounts{};

    friend bool operator==(const BridgeEthStateObservationV2&,
                           const BridgeEthStateObservationV2&) = default;
};

/**
 * Kind 13 body (node changes section 8.5): one digest-V2 quorum certificate
 * of the fresh finality lineage, offered as evidence that the lineage's
 * committee signed a statement other than B3's canonical one. The wire is
 *
 *   0x0d || 0x00 || u16 n || FinalizedBlockV2(185) || bitmap(ceil(n/8)) || aggSig(96)
 *
 * with n the signing committee's size (1..MAX_FINALITY_SET, big-endian) and
 * the certificate exactly modern::EncodeCertificatePayloadV2's bytes: 286
 * bytes at n = 4, at most 293 within the bridge bound of 64 validators and
 * at most 1,309 at n = 8,192. The codec checks only the shape
 * (DecodeCertificatePayloadV2: n in range, exact length, zero bitmap
 * padding, bridge-flag bits limited to bit 0); which committee signed, the
 * signature and the difference from B3's statement are consensus rules.
 */
struct BridgeFinalityConflictV2 {
    uint16_t validator_count{0};              // n of the signing committee, 1..MAX_FINALITY_SET
    std::vector<unsigned char> certificate{}; // modern certificate payload V2: FinalizedBlockV2(185) || bitmap(ceil(n/8)) || aggSig(96)

    friend bool operator==(const BridgeFinalityConflictV2&,
                           const BridgeFinalityConflictV2&) = default;

    std::optional<std::pair<modern::FinalizedBlockV2, modern::FinalityCertificate>> Decode() const
    {
        return modern::DecodeCertificatePayloadV2(certificate, validator_count);
    }
};
static_assert(modern::MAX_FINALITY_SET <= 0xffff);
static_assert(4 + modern::FINALITY_CERTIFICATE_V2_RECORD_MAX <=
              MAX_BRIDGE_RECORD_SIZE);

/**
 * Kind 16 body (node changes section 7.6, B-23): a request that the block
 * append the sweep leaf moving the retired launch vault v's reserve to the
 * successor v'. The wire is exactly
 *
 *   0x10 || 0x00 || registry_id(32)
 *
 * with `registry_id` the launch vault entry id in raw uint256 order, as
 * kinds 3 and 6 name an entry (the bytes of getbridgev2info
 * `vault_entry_id_evm`). As with kind 6, the amount, the leaf id, the B3
 * height and both vault addresses are deliberately absent: consensus
 * derives every leaf field, so a relayer chooses none of them. The codec
 * refuses only a null id; which entry it must name and when a sweep is
 * valid are consensus rules.
 */
struct BridgeVaultSweepV2 {
    uint256 registry_id{};

    friend bool operator==(const BridgeVaultSweepV2&,
                           const BridgeVaultSweepV2&) = default;
};
inline constexpr size_t BRIDGE_VAULT_SWEEP_WIRE_SIZE{2 + 32};

using BridgeRecordPayloadV1 =
    std::variant<BridgeBootstrapV1, BridgeUpdateV1, BridgeMintV1,
                 BridgeExecutionBackfillV1, BridgeManagedWithdrawalV1,
                 BridgeBurnV1, BridgeEthStateObservationV2,
                 BridgeFinalityConflictV2, BridgeVaultSweepV2>;

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

//! Item size limit for element `index` of `count`: the last element may use
//! the (possibly larger) terminal limit.
inline size_t BlobItemLimit(const size_t index, const size_t count,
                            const size_t max_item, const size_t max_terminal_item)
{
    return index + 1 == count ? max_terminal_item : max_item;
}

inline bool EncodeBlobVector(Writer& writer,
                             const std::vector<std::vector<unsigned char>>& blobs,
                             const size_t max_count, const size_t max_total,
                             const bool require_nonempty,
                             const size_t max_item = MAX_BRIDGE_RLP_ITEM_SIZE,
                             const size_t max_terminal_item = MAX_BRIDGE_RLP_ITEM_SIZE)
{
    if ((require_nonempty && blobs.empty()) || blobs.size() > max_count) {
        return false;
    }
    size_t total{0};
    for (size_t i{0}; i < blobs.size(); ++i) {
        const auto& blob{blobs[i]};
        if (blob.empty() ||
            blob.size() > BlobItemLimit(i, blobs.size(), max_item,
                                        max_terminal_item) ||
            blob.size() > 0xffff || blob.size() > max_total - total) {
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
                             const bool require_nonempty,
                             const size_t max_item = MAX_BRIDGE_RLP_ITEM_SIZE,
                             const size_t max_terminal_item = MAX_BRIDGE_RLP_ITEM_SIZE)
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
        if (!reader.U16(size) || size == 0 ||
            size > BlobItemLimit(i, count, max_item, max_terminal_item) ||
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

//! Subject, anchor and verdict rules shared by both kind 9 directions.
inline bool EthObservationHeaderValid(const uint8_t subject,
                                      const uint256& anchor_hash,
                                      const uint8_t verdict)
{
    return !EthObservationShapeV2(subject).empty() && !anchor_hash.IsNull() &&
           (verdict & ~ETH_OBSERVATION_VERDICT_MASK) == 0 &&
           (subject == ETH_OBSERVATION_SUBJECT_VERIFIER || verdict == 0);
}

// One node list of an account or storage proof: the receipt-proof node
// count, node size and byte limits (64 nodes, 1,024 bytes each, 11,264 in
// total), the same bounds bridge/state_proof.h's verifier enforces.
inline bool EncodeStateProofNodes(
    Writer& writer, const std::vector<std::vector<unsigned char>>& nodes,
    const bool require_nonempty)
{
    return EncodeBlobVector(writer, nodes, MAX_BRIDGE_MPT_NODES,
                            MAX_BRIDGE_MPT_BYTES, require_nonempty);
}

inline bool DecodeStateProofNodes(
    Reader& reader, std::vector<std::vector<unsigned char>>& nodes,
    const bool require_nonempty)
{
    return DecodeBlobVector(reader, nodes, MAX_BRIDGE_MPT_NODES,
                            MAX_BRIDGE_MPT_BYTES, require_nonempty);
}

inline bool EncodeEthStateObservation(Writer& writer,
                                      const BridgeEthStateObservationV2& value)
{
    const auto shape{EthObservationShapeV2(value.subject)};
    if (!EthObservationHeaderValid(value.subject, value.anchor_hash,
                                   value.verdict) ||
        value.accounts.size() != shape.size()) {
        return false;
    }
    writer.U8(value.subject);
    writer.Root(value.anchor_hash);
    writer.U8(value.verdict);
    writer.U8(static_cast<uint8_t>(shape.size()));
    for (size_t i{0}; i < shape.size(); ++i) {
        const BridgeEthAccountProofV2& account{value.accounts[i]};
        if (account.storage_nodes.size() != shape[i] ||
            !EncodeStateProofNodes(writer, account.account_nodes,
                                   /*require_nonempty=*/true)) {
            return false;
        }
        writer.U8(shape[i]);
        for (const auto& slot_nodes : account.storage_nodes) {
            if (!EncodeStateProofNodes(writer, slot_nodes,
                                       /*require_nonempty=*/false)) {
                return false;
            }
        }
    }
    return true;
}

inline bool DecodeEthStateObservation(Reader& reader,
                                      BridgeEthStateObservationV2& value)
{
    uint8_t account_count{0};
    if (!reader.U8(value.subject) || !reader.Root(value.anchor_hash) ||
        !reader.U8(value.verdict) ||
        !EthObservationHeaderValid(value.subject, value.anchor_hash,
                                   value.verdict) ||
        !reader.U8(account_count)) {
        return false;
    }
    const auto shape{EthObservationShapeV2(value.subject)};
    if (account_count != shape.size()) return false;
    value.accounts.assign(shape.size(), {});
    for (size_t i{0}; i < shape.size(); ++i) {
        BridgeEthAccountProofV2& account{value.accounts[i]};
        uint8_t slot_count{0};
        if (!DecodeStateProofNodes(reader, account.account_nodes,
                                   /*require_nonempty=*/true) ||
            !reader.U8(slot_count) || slot_count != shape[i]) {
            return false;
        }
        account.storage_nodes.assign(slot_count, {});
        for (auto& slot_nodes : account.storage_nodes) {
            if (!DecodeStateProofNodes(reader, slot_nodes,
                                       /*require_nonempty=*/false)) {
                return false;
            }
        }
    }
    return true;
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

/**
 * Canonical encode. `mpt_limits` applies only to a MINT's receipt proof and
 * to whether kinds 9, 13 and 16 exist; the V1 default keeps every pre-V2
 * caller byte-identical. Consensus selects the limit set by height (V2 from
 * the bridge V2 activation height), so a record that only fits the V2
 * limits, or any kind 9, 13 or 16 record, is malformed below that height.
 */
inline std::optional<std::vector<unsigned char>> EncodeBridgeRecordV1(
    const BridgeRecordV1& record,
    const BridgeMptLimits& mpt_limits = BRIDGE_MPT_LIMITS_V1)
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
                                     mpt_limits.max_total_bytes,
                                     /*require_nonempty=*/true,
                                     mpt_limits.max_node_size,
                                     mpt_limits.max_terminal_node_size);
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
    case BridgeRecordKindV1::ETH_STATE_OBSERVATION:
        if (!mpt_limits.v2_kinds) break;
        if (const auto* value{
                std::get_if<BridgeEthStateObservationV2>(&record.payload)}) {
            valid = EncodeEthStateObservation(writer, *value);
        }
        break;
    case BridgeRecordKindV1::FINALITY_CONFLICT:
        if (!mpt_limits.v2_kinds) break;
        if (const auto* value{
                std::get_if<BridgeFinalityConflictV2>(&record.payload)}) {
            if (!value->Decode()) break;
            writer.U16(value->validator_count);
            writer.Bytes(value->certificate);
            valid = true;
        }
        break;
    case BridgeRecordKindV1::VAULT_SWEEP:
        if (!mpt_limits.v2_kinds) break;
        if (const auto* value{
                std::get_if<BridgeVaultSweepV2>(&record.payload)}) {
            if (value->registry_id.IsNull()) break;
            writer.Root(value->registry_id);
            valid = true;
        }
        break;
    }

    if (!valid) return std::nullopt;
    auto encoded{writer.Take()};
    if (encoded.size() > MAX_BRIDGE_RECORD_SIZE) return std::nullopt;
    return encoded;
}

//! Strict decode under `mpt_limits` (see EncodeBridgeRecordV1).
inline std::optional<BridgeRecordV1> DecodeBridgeRecordV1(
    const std::span<const unsigned char> encoded,
    const BridgeMptLimits& mpt_limits = BRIDGE_MPT_LIMITS_V1)
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
                              mpt_limits.max_total_bytes,
                              /*require_nonempty=*/true,
                              mpt_limits.max_node_size,
                              mpt_limits.max_terminal_node_size) ||
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
    case BridgeRecordKindV1::ETH_STATE_OBSERVATION: {
        BridgeEthStateObservationV2 value;
        if (!mpt_limits.v2_kinds ||
            !DecodeEthStateObservation(reader, value) || !reader.Empty()) {
            return std::nullopt;
        }
        return BridgeRecordV1{kind, std::move(value)};
    }
    case BridgeRecordKindV1::FINALITY_CONFLICT: {
        // The certificate is everything after n; its exact length is a
        // function of n, so no length prefix travels.
        BridgeFinalityConflictV2 value;
        if (!mpt_limits.v2_kinds || !reader.U16(value.validator_count) ||
            !reader.Vector(reader.Remaining(), value.certificate) ||
            !reader.Empty() || !value.Decode()) {
            return std::nullopt;
        }
        return BridgeRecordV1{kind, std::move(value)};
    }
    case BridgeRecordKindV1::VAULT_SWEEP: {
        BridgeVaultSweepV2 value;
        if (!mpt_limits.v2_kinds || !reader.Root(value.registry_id) ||
            value.registry_id.IsNull() || !reader.Empty()) {
            return std::nullopt;
        }
        return BridgeRecordV1{kind, std::move(value)};
    }
    }
    return std::nullopt;
}

inline std::optional<CMpaRecord> MakeBridgeMpaRecord(
    const BridgeRecordV1& record,
    const BridgeMptLimits& mpt_limits = BRIDGE_MPT_LIMITS_V1)
{
    auto encoded{EncodeBridgeRecordV1(record, mpt_limits)};
    if (!encoded) return std::nullopt;
    return CMpaRecord{BRIDGE_MPA_TYPE, BRIDGE_MPA_VERSION_V1,
                      std::move(*encoded)};
}

inline std::optional<BridgeRecordV1> DecodeBridgeMpaRecordV1(
    const CMpaRecord& record,
    const BridgeMptLimits& mpt_limits = BRIDGE_MPT_LIMITS_V1)
{
    if (record.payload_type != BRIDGE_MPA_TYPE ||
        record.payload_version != BRIDGE_MPA_VERSION_V1) {
        return std::nullopt;
    }
    return DecodeBridgeRecordV1(record.payload, mpt_limits);
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
