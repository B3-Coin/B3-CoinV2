// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_FLOWMESH_CHECKPOINT_H
#define B3COIN_MODERN_FLOWMESH_CHECKPOINT_H

#include <consensus/amount.h>
#include <crypto/common.h>
#include <flowmesh/bls_certificate.h>
#include <flowmesh/market.h>
#include <flowmesh/production_commitment.h>
#include <hash.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace modern {

/**
 * FlowMesh checkpoint/effect v1 consensus codec.
 *
 * Every integer is fixed-width big-endian, every hash is its raw 32-byte
 * serialization, and decoders require exact exhaustion. There are no
 * CompactSize fields in these objects. MPA framing supplies the outer payload
 * length; an object's kind selects its one exact inner layout.
 */
inline constexpr uint16_t FLOWMESH_CHECKPOINT_VERSION_V1{1};
inline constexpr size_t FLOWMESH_MAX_CHECKPOINT_EFFECTS{4096};
inline constexpr size_t FLOWMESH_MAX_EFFECT_BRANCH_DEPTH{12};

inline constexpr const char* FLOWMESH_CHECKPOINT_ID_TAG{
    "B3/FLOWMESH/CHECKPOINT/V1"};
inline constexpr const char* FLOWMESH_EFFECT_LEAF_TAG{
    "B3/FLOWMESH/EFFECT/LEAF/V1"};
inline constexpr const char* FLOWMESH_EFFECT_PAD_TAG{
    "B3/FLOWMESH/EFFECT/PAD/V1"};
inline constexpr const char* FLOWMESH_EFFECT_NODE_TAG{
    "B3/FLOWMESH/EFFECT/NODE/V1"};
inline constexpr const char* FLOWMESH_EFFECT_EMPTY_TAG{
    "B3/FLOWMESH/EFFECT/EMPTY/V1"};
inline constexpr const char* FLOWMESH_EFFECT_ROOT_TAG{
    "B3/FLOWMESH/EFFECT/ROOT/V1"};

//! Canonical 32-byte identity returned by FlowMeshCheckpointIdV1.
using FlowMeshCheckpointId = uint256;

enum class FlowMeshEffectKind : uint8_t {
    DEPOSIT_ACCEPTANCE = 1,
    WITHDRAWAL_RECEIPT = 2,
};

/**
 * Certified acceptance of one exact USER_DEPOSIT outpoint. `acceptance_id`
 * is the durable FlowMesh effect identity; the outpoint remains present so a
 * type-9 sweep cannot substitute a different deposit with the same value.
 *
 * Wire (223 bytes):
 *   kind[1] || acceptance_id[32] || market_id[32] || epoch[8] || sequence[8]
 *   || deposit_txid[32] || deposit_vout[4] || account[32] || asset[32]
 *   || amount[8] || vault_id[32] || shard[2]
 */
struct FlowMeshDepositAcceptanceV1 {
    uint256 acceptance_id;
    flowmesh::MarketId market_id;
    uint64_t epoch{0};
    uint64_t sequence{0};
    COutPoint deposit_outpoint;
    uint256 account;
    AssetId asset;
    CAmount amount{0};
    flowmesh::VaultId vault_id;
    uint16_t shard{0};

    friend bool operator==(const FlowMeshDepositAcceptanceV1& a,
                           const FlowMeshDepositAcceptanceV1& b) = default;
};

/**
 * One-time withdrawal authorization. The destination is the OWNER policy
 * commitment fixed by the signed request; it is not a relayer-selected
 * script. Wire (219 bytes):
 *   kind[1] || receipt_id[32] || market_id[32] || epoch[8] || sequence[8]
 *   || account[32] || asset[32] || amount[8]
 *   || destination_owner_commitment[32] || vault_id[32]
 *   || deterministic_change_shard[2]
 */
struct FlowMeshWithdrawalReceiptV1 {
    uint256 receipt_id;
    flowmesh::MarketId market_id;
    uint64_t epoch{0};
    uint64_t sequence{0};
    uint256 account;
    AssetId asset;
    CAmount amount{0};
    uint256 destination_owner_commitment;
    flowmesh::VaultId vault_id;
    uint16_t deterministic_change_shard{0};

    friend bool operator==(const FlowMeshWithdrawalReceiptV1& a,
                           const FlowMeshWithdrawalReceiptV1& b) = default;
};

using FlowMeshEffectV1 =
    std::variant<FlowMeshDepositAcceptanceV1, FlowMeshWithdrawalReceiptV1>;

inline constexpr size_t FLOWMESH_DEPOSIT_ACCEPTANCE_V1_SIZE{
    1 + 32 + 32 + 8 + 8 + 32 + 4 + 32 + 32 + 8 + 32 + 2};
inline constexpr size_t FLOWMESH_WITHDRAWAL_RECEIPT_V1_SIZE{
    1 + 32 + 32 + 8 + 8 + 32 + 32 + 8 + 32 + 32 + 2};
static_assert(FLOWMESH_DEPOSIT_ACCEPTANCE_V1_SIZE == 223);
static_assert(FLOWMESH_WITHDRAWAL_RECEIPT_V1_SIZE == 219);

namespace flowmesh_checkpoint_detail {

inline void AppendHash(std::vector<unsigned char>& out, const uint256& hash)
{
    out.insert(out.end(), hash.begin(), hash.end());
}

inline void AppendU16(std::vector<unsigned char>& out, const uint16_t value)
{
    const size_t offset{out.size()};
    out.resize(offset + 2);
    WriteBE16(out.data() + offset, value);
}

inline void AppendU32(std::vector<unsigned char>& out, const uint32_t value)
{
    const size_t offset{out.size()};
    out.resize(offset + 4);
    WriteBE32(out.data() + offset, value);
}

inline void AppendU64(std::vector<unsigned char>& out, const uint64_t value)
{
    const size_t offset{out.size()};
    out.resize(offset + 8);
    WriteBE64(out.data() + offset, value);
}

inline bool ReadHash(const std::span<const unsigned char> in, size_t& cursor,
                     uint256& out)
{
    if (cursor > in.size() || in.size() - cursor < 32) return false;
    std::copy(in.begin() + cursor, in.begin() + cursor + 32, out.begin());
    cursor += 32;
    return true;
}

inline bool ReadU16(const std::span<const unsigned char> in, size_t& cursor,
                    uint16_t& out)
{
    if (cursor > in.size() || in.size() - cursor < 2) return false;
    out = ::ReadBE16(in.data() + cursor);
    cursor += 2;
    return true;
}

inline bool ReadU32(const std::span<const unsigned char> in, size_t& cursor,
                    uint32_t& out)
{
    if (cursor > in.size() || in.size() - cursor < 4) return false;
    out = ::ReadBE32(in.data() + cursor);
    cursor += 4;
    return true;
}

inline bool ReadU64(const std::span<const unsigned char> in, size_t& cursor,
                    uint64_t& out)
{
    if (cursor > in.size() || in.size() - cursor < 8) return false;
    out = ::ReadBE64(in.data() + cursor);
    cursor += 8;
    return true;
}

inline bool ValidAmount(const CAmount amount)
{
    return amount > 0 && amount <= MAX_MONEY;
}

inline uint256 TaggedEmptyHash(const char* tag)
{
    HashWriter writer{TaggedHash(tag)};
    return writer.GetSHA256();
}

inline uint256 EffectNodeHash(const uint256& left, const uint256& right)
{
    HashWriter writer{TaggedHash(FLOWMESH_EFFECT_NODE_TAG)};
    writer << std::span<const unsigned char>{left.begin(), 32};
    writer << std::span<const unsigned char>{right.begin(), 32};
    return writer.GetSHA256();
}

inline uint256 EffectPadHash(const uint64_t absolute_index)
{
    std::array<unsigned char, 8> index{};
    WriteBE64(index.data(), absolute_index);
    HashWriter writer{TaggedHash(FLOWMESH_EFFECT_PAD_TAG)};
    writer << std::span<const unsigned char>{index};
    return writer.GetSHA256();
}

inline uint256 WrapEffectRoot(const uint64_t effect_start, const uint32_t count,
                              const uint256& tree_root)
{
    std::array<unsigned char, 12> numbers{};
    WriteBE64(numbers.data(), effect_start);
    WriteBE32(numbers.data() + 8, count);
    HashWriter writer{TaggedHash(FLOWMESH_EFFECT_ROOT_TAG)};
    writer << std::span<const unsigned char>{numbers};
    writer << std::span<const unsigned char>{tree_root.begin(), 32};
    return writer.GetSHA256();
}

} // namespace flowmesh_checkpoint_detail

inline std::optional<std::vector<unsigned char>> EncodeFlowMeshEffectV1(
    const FlowMeshEffectV1& effect)
{
    using namespace flowmesh_checkpoint_detail;
    std::vector<unsigned char> out;
    if (const auto* deposit{std::get_if<FlowMeshDepositAcceptanceV1>(&effect)}) {
        if (deposit->acceptance_id.IsNull() || deposit->market_id.IsNull() ||
            deposit->deposit_outpoint.hash.IsNull() ||
            deposit->deposit_outpoint.n == COutPoint::NULL_INDEX ||
            deposit->vault_id.IsNull() || deposit->account.IsNull() ||
            !ValidAmount(deposit->amount) || deposit->shard >= 256) {
            return std::nullopt;
        }
        out.reserve(FLOWMESH_DEPOSIT_ACCEPTANCE_V1_SIZE);
        out.push_back(static_cast<uint8_t>(FlowMeshEffectKind::DEPOSIT_ACCEPTANCE));
        AppendHash(out, deposit->acceptance_id);
        AppendHash(out, deposit->market_id);
        AppendU64(out, deposit->epoch);
        AppendU64(out, deposit->sequence);
        AppendHash(out, deposit->deposit_outpoint.hash.ToUint256());
        AppendU32(out, deposit->deposit_outpoint.n);
        AppendHash(out, deposit->account);
        AppendHash(out, deposit->asset);
        AppendU64(out, static_cast<uint64_t>(deposit->amount));
        AppendHash(out, deposit->vault_id);
        AppendU16(out, deposit->shard);
    } else {
        const auto& receipt{std::get<FlowMeshWithdrawalReceiptV1>(effect)};
        if (receipt.receipt_id.IsNull() || receipt.market_id.IsNull() ||
            receipt.account.IsNull() ||
            receipt.destination_owner_commitment.IsNull() ||
            receipt.vault_id.IsNull() || !ValidAmount(receipt.amount) ||
            receipt.deterministic_change_shard >= 256) {
            return std::nullopt;
        }
        out.reserve(FLOWMESH_WITHDRAWAL_RECEIPT_V1_SIZE);
        out.push_back(static_cast<uint8_t>(FlowMeshEffectKind::WITHDRAWAL_RECEIPT));
        AppendHash(out, receipt.receipt_id);
        AppendHash(out, receipt.market_id);
        AppendU64(out, receipt.epoch);
        AppendU64(out, receipt.sequence);
        AppendHash(out, receipt.account);
        AppendHash(out, receipt.asset);
        AppendU64(out, static_cast<uint64_t>(receipt.amount));
        AppendHash(out, receipt.destination_owner_commitment);
        AppendHash(out, receipt.vault_id);
        AppendU16(out, receipt.deterministic_change_shard);
    }
    return out;
}

inline std::optional<FlowMeshEffectV1> DecodeFlowMeshEffectV1(
    const std::span<const unsigned char> in)
{
    using namespace flowmesh_checkpoint_detail;
    if (in.empty()) return std::nullopt;
    size_t cursor{1};
    if (in[0] == static_cast<uint8_t>(FlowMeshEffectKind::DEPOSIT_ACCEPTANCE)) {
        if (in.size() != FLOWMESH_DEPOSIT_ACCEPTANCE_V1_SIZE) return std::nullopt;
        FlowMeshDepositAcceptanceV1 effect;
        uint256 txid;
        uint32_t vout{0};
        uint64_t amount{0};
        if (!ReadHash(in, cursor, effect.acceptance_id) ||
            !ReadHash(in, cursor, effect.market_id) ||
            !ReadU64(in, cursor, effect.epoch) ||
            !ReadU64(in, cursor, effect.sequence) ||
            !ReadHash(in, cursor, txid) || !ReadU32(in, cursor, vout) ||
            !ReadHash(in, cursor, effect.account) ||
            !ReadHash(in, cursor, effect.asset) || !ReadU64(in, cursor, amount) ||
            !ReadHash(in, cursor, effect.vault_id) ||
            !ReadU16(in, cursor, effect.shard) || cursor != in.size() ||
            amount > static_cast<uint64_t>(MAX_MONEY)) {
            return std::nullopt;
        }
        effect.deposit_outpoint = COutPoint{Txid::FromUint256(txid), vout};
        effect.amount = static_cast<CAmount>(amount);
        const FlowMeshEffectV1 wrapped{effect};
        return EncodeFlowMeshEffectV1(wrapped) ? std::optional<FlowMeshEffectV1>{wrapped}
                                                : std::nullopt;
    }
    if (in[0] == static_cast<uint8_t>(FlowMeshEffectKind::WITHDRAWAL_RECEIPT)) {
        if (in.size() != FLOWMESH_WITHDRAWAL_RECEIPT_V1_SIZE) return std::nullopt;
        FlowMeshWithdrawalReceiptV1 effect;
        uint64_t amount{0};
        if (!ReadHash(in, cursor, effect.receipt_id) ||
            !ReadHash(in, cursor, effect.market_id) ||
            !ReadU64(in, cursor, effect.epoch) ||
            !ReadU64(in, cursor, effect.sequence) ||
            !ReadHash(in, cursor, effect.account) ||
            !ReadHash(in, cursor, effect.asset) || !ReadU64(in, cursor, amount) ||
            !ReadHash(in, cursor, effect.destination_owner_commitment) ||
            !ReadHash(in, cursor, effect.vault_id) ||
            !ReadU16(in, cursor, effect.deterministic_change_shard) ||
            cursor != in.size() || amount > static_cast<uint64_t>(MAX_MONEY)) {
            return std::nullopt;
        }
        effect.amount = static_cast<CAmount>(amount);
        const FlowMeshEffectV1 wrapped{effect};
        return EncodeFlowMeshEffectV1(wrapped) ? std::optional<FlowMeshEffectV1>{wrapped}
                                                : std::nullopt;
    }
    return std::nullopt;
}

inline constexpr size_t FlowMeshEffectTreeDepth(const size_t count)
{
    if (count <= 1) return 0;
    size_t depth{0};
    size_t width{1};
    while (width < count) {
        width <<= 1;
        ++depth;
    }
    return depth;
}

inline constexpr size_t FlowMeshEffectTreeWidth(const size_t count)
{
    if (count == 0) return 0;
    return size_t{1} << FlowMeshEffectTreeDepth(count);
}

inline std::optional<uint256> FlowMeshEffectLeafHash(
    const uint64_t absolute_index, const FlowMeshEffectV1& effect)
{
    const auto bytes{EncodeFlowMeshEffectV1(effect)};
    if (!bytes) return std::nullopt;
    std::array<unsigned char, 8> index{};
    WriteBE64(index.data(), absolute_index);
    HashWriter writer{TaggedHash(FLOWMESH_EFFECT_LEAF_TAG)};
    writer << std::span<const unsigned char>{index};
    writer << std::span<const unsigned char>{*bytes};
    return writer.GetSHA256();
}

inline uint256 EmptyFlowMeshEffectRoot(const uint64_t effect_start)
{
    const uint256 empty{
        flowmesh_checkpoint_detail::TaggedEmptyHash(FLOWMESH_EFFECT_EMPTY_TAG)};
    return flowmesh_checkpoint_detail::WrapEffectRoot(effect_start, 0, empty);
}

inline std::optional<uint256> ComputeFlowMeshEffectRoot(
    const uint64_t effect_start, const std::span<const FlowMeshEffectV1> effects)
{
    using namespace flowmesh_checkpoint_detail;
    if (effects.size() > FLOWMESH_MAX_CHECKPOINT_EFFECTS) return std::nullopt;
    if (effects.empty()) return EmptyFlowMeshEffectRoot(effect_start);
    const size_t width{FlowMeshEffectTreeWidth(effects.size())};
    if (effect_start > std::numeric_limits<uint64_t>::max() - (width - 1)) {
        return std::nullopt;
    }
    std::vector<uint256> level;
    level.reserve(width);
    for (size_t i{0}; i < effects.size(); ++i) {
        const auto leaf{FlowMeshEffectLeafHash(effect_start + i, effects[i])};
        if (!leaf) return std::nullopt;
        level.push_back(*leaf);
    }
    for (size_t i{effects.size()}; i < width; ++i) {
        level.push_back(EffectPadHash(effect_start + i));
    }
    while (level.size() > 1) {
        for (size_t i{0}; i < level.size(); i += 2) {
            level[i / 2] = EffectNodeHash(level[i], level[i + 1]);
        }
        level.resize(level.size() / 2);
    }
    return WrapEffectRoot(effect_start, static_cast<uint32_t>(effects.size()), level[0]);
}

inline std::optional<std::vector<uint256>> BuildFlowMeshEffectBranch(
    const uint64_t effect_start, const std::span<const FlowMeshEffectV1> effects,
    const uint32_t leaf_index)
{
    using namespace flowmesh_checkpoint_detail;
    if (effects.empty() || effects.size() > FLOWMESH_MAX_CHECKPOINT_EFFECTS ||
        leaf_index >= effects.size()) {
        return std::nullopt;
    }
    const size_t width{FlowMeshEffectTreeWidth(effects.size())};
    if (effect_start > std::numeric_limits<uint64_t>::max() - (width - 1)) {
        return std::nullopt;
    }
    std::vector<uint256> level;
    level.reserve(width);
    for (size_t i{0}; i < effects.size(); ++i) {
        const auto leaf{FlowMeshEffectLeafHash(effect_start + i, effects[i])};
        if (!leaf) return std::nullopt;
        level.push_back(*leaf);
    }
    for (size_t i{effects.size()}; i < width; ++i) {
        level.push_back(EffectPadHash(effect_start + i));
    }

    std::vector<uint256> branch;
    branch.reserve(FlowMeshEffectTreeDepth(effects.size()));
    size_t position{leaf_index};
    while (level.size() > 1) {
        branch.push_back(level[position ^ 1]);
        for (size_t i{0}; i < level.size(); i += 2) {
            level[i / 2] = EffectNodeHash(level[i], level[i + 1]);
        }
        level.resize(level.size() / 2);
        position >>= 1;
    }
    return branch;
}

inline bool VerifyFlowMeshEffectInclusion(
    const uint64_t effect_start, const uint32_t effect_count,
    const uint256& expected_root, const FlowMeshEffectV1& effect,
    const uint32_t leaf_index, const std::span<const uint256> branch)
{
    using namespace flowmesh_checkpoint_detail;
    if (effect_count == 0 || effect_count > FLOWMESH_MAX_CHECKPOINT_EFFECTS ||
        leaf_index >= effect_count ||
        branch.size() != FlowMeshEffectTreeDepth(effect_count) ||
        effect_start > std::numeric_limits<uint64_t>::max() - leaf_index) {
        return false;
    }
    const auto leaf{FlowMeshEffectLeafHash(effect_start + leaf_index, effect)};
    if (!leaf) return false;
    uint256 node{*leaf};
    size_t position{leaf_index};
    for (const uint256& sibling : branch) {
        node = (position & 1) ? EffectNodeHash(sibling, node)
                              : EffectNodeHash(node, sibling);
        position >>= 1;
    }
    return WrapEffectRoot(effect_start, effect_count, node) == expected_root;
}

enum class FlowMeshCheckpointKind : uint8_t {
    EXECUTION = 1,
    EPOCH_HANDOFF = 2,
};

struct FlowMeshCheckpointAnchorV1 {
    uint64_t height{0};
    uint256 block_hash;

    friend bool operator==(const FlowMeshCheckpointAnchorV1& a,
                           const FlowMeshCheckpointAnchorV1& b) = default;
};

struct FlowMeshCheckpointHandoffV1 {
    uint64_t next_epoch{0};
    FlowMeshCheckpointAnchorV1 next_anchor;
    uint256 next_seat_set_hash;

    friend bool operator==(const FlowMeshCheckpointHandoffV1& a,
                           const FlowMeshCheckpointHandoffV1& b) = default;
};

/**
 * The exact object committed by CheckpointId. The certificate fields
 * epoch/sequence/microblock_hash are sourced from this core and therefore do
 * not appear a second time in the type-8 payload.
 */
struct FlowMeshCheckpointCoreV1 {
    uint16_t version{FLOWMESH_CHECKPOINT_VERSION_V1};
    FlowMeshCheckpointKind kind{FlowMeshCheckpointKind::EXECUTION};
    uint256 domain;
    flowmesh::MarketId market_id;
    uint64_t epoch{0};
    uint64_t sequence{0};
    uint256 microblock_hash;
    uint256 previous_checkpoint_id;
    //! Anchor of the BLS seat snapshot for this epoch.
    FlowMeshCheckpointAnchorV1 anchor;
    uint256 seat_set_hash;
    //! Independently advancing execution/deposit-history anchor.
    FlowMeshCheckpointAnchorV1 production_anchor;
    uint256 parent_hash;
    uint256 previous_state_root;
    uint256 actions_root;
    uint256 result_root;
    uint256 state_root;
    uint64_t effect_start{0};
    uint32_t effect_count{0};
    uint256 effect_root;
    std::optional<FlowMeshCheckpointHandoffV1> handoff;

    friend bool operator==(const FlowMeshCheckpointCoreV1& a,
                           const FlowMeshCheckpointCoreV1& b) = default;
};

inline constexpr size_t FLOWMESH_EXECUTION_CHECKPOINT_CORE_V1_SIZE{
    2 + 1 + 32 + 32 + 8 + 8 + 32 + 32 + 8 + 32 + 32 + 8 + 32 + 32 + 32 +
    32 + 32 + 32 + 8 + 4 + 32};
inline constexpr size_t FLOWMESH_HANDOFF_CHECKPOINT_CORE_V1_SIZE{
    FLOWMESH_EXECUTION_CHECKPOINT_CORE_V1_SIZE + 8 + 8 + 32 + 32};
static_assert(FLOWMESH_EXECUTION_CHECKPOINT_CORE_V1_SIZE == 463);
static_assert(FLOWMESH_HANDOFF_CHECKPOINT_CORE_V1_SIZE == 543);

inline std::optional<flowmesh::ProductionEntryCommitmentV1>
FlowMeshCheckpointProductionCommitmentV1(
    const FlowMeshCheckpointCoreV1& core)
{
    flowmesh::ProductionEntryCommitmentV1 out;
    out.version = core.version;
    out.kind = static_cast<uint8_t>(core.kind);
    out.domain = core.domain;
    out.market_id = core.market_id;
    out.epoch = core.epoch;
    out.seat_set_hash = core.seat_set_hash;
    out.sequence = core.sequence;
    out.parent_hash = core.parent_hash;
    out.production_anchor = {core.production_anchor.height,
                             core.production_anchor.block_hash};
    out.previous_state_root = core.previous_state_root;
    out.actions_root = core.actions_root;
    out.result_root = core.result_root;
    out.state_root = core.state_root;
    out.effect_start = core.effect_start;
    out.effect_count = core.effect_count;
    out.effect_root = core.effect_root;
    if (core.kind == FlowMeshCheckpointKind::EPOCH_HANDOFF && core.handoff) {
        out.next_epoch = core.handoff->next_epoch;
        out.next_seat_anchor = {core.handoff->next_anchor.height,
                                core.handoff->next_anchor.block_hash};
        out.next_seat_set_hash = core.handoff->next_seat_set_hash;
    }
    if (!flowmesh::ComputeProductionEntryIdentityV1(out)) return std::nullopt;
    return out;
}

inline std::optional<uint256> FlowMeshCheckpointProductionIdentityV1(
    const FlowMeshCheckpointCoreV1& core)
{
    const auto commitment{FlowMeshCheckpointProductionCommitmentV1(core)};
    return commitment ? flowmesh::ComputeProductionEntryIdentityV1(*commitment)
                      : std::nullopt;
}

inline bool IsCanonicalFlowMeshCheckpointCoreV1(
    const FlowMeshCheckpointCoreV1& core)
{
    if (core.version != FLOWMESH_CHECKPOINT_VERSION_V1 || core.domain.IsNull() ||
        core.market_id.IsNull() || core.microblock_hash.IsNull() ||
        core.anchor.block_hash.IsNull() || core.seat_set_hash.IsNull() ||
        core.production_anchor.block_hash.IsNull() ||
        core.previous_state_root.IsNull() || core.actions_root.IsNull() ||
        core.result_root.IsNull() || core.state_root.IsNull() ||
        core.effect_count > FLOWMESH_MAX_CHECKPOINT_EFFECTS ||
        core.effect_start > std::numeric_limits<uint64_t>::max() - core.effect_count) {
        return false;
    }
    if (core.effect_count == 0) {
        if (core.effect_root != EmptyFlowMeshEffectRoot(core.effect_start)) return false;
    } else if (core.effect_root.IsNull()) {
        return false;
    }

    if (core.kind == FlowMeshCheckpointKind::EXECUTION) {
        if (core.handoff) return false;
    } else if (core.kind != FlowMeshCheckpointKind::EPOCH_HANDOFF ||
               !core.handoff || core.effect_count != 0 ||
               core.epoch == std::numeric_limits<uint64_t>::max() ||
               core.handoff->next_epoch != core.epoch + 1 ||
               core.handoff->next_anchor.height <= core.anchor.height ||
               core.handoff->next_anchor.block_hash.IsNull() ||
               core.handoff->next_seat_set_hash.IsNull()) {
        return false;
    }
    const auto identity{FlowMeshCheckpointProductionIdentityV1(core)};
    return identity && *identity == core.microblock_hash;
}

inline std::optional<std::vector<unsigned char>> EncodeFlowMeshCheckpointCoreV1(
    const FlowMeshCheckpointCoreV1& core)
{
    using namespace flowmesh_checkpoint_detail;
    if (!IsCanonicalFlowMeshCheckpointCoreV1(core)) return std::nullopt;
    std::vector<unsigned char> out;
    out.reserve(core.handoff ? FLOWMESH_HANDOFF_CHECKPOINT_CORE_V1_SIZE
                             : FLOWMESH_EXECUTION_CHECKPOINT_CORE_V1_SIZE);
    AppendU16(out, core.version);
    out.push_back(static_cast<uint8_t>(core.kind));
    AppendHash(out, core.domain);
    AppendHash(out, core.market_id);
    AppendU64(out, core.epoch);
    AppendU64(out, core.sequence);
    AppendHash(out, core.microblock_hash);
    AppendHash(out, core.previous_checkpoint_id);
    AppendU64(out, core.anchor.height);
    AppendHash(out, core.anchor.block_hash);
    AppendHash(out, core.seat_set_hash);
    AppendU64(out, core.production_anchor.height);
    AppendHash(out, core.production_anchor.block_hash);
    AppendHash(out, core.parent_hash);
    AppendHash(out, core.previous_state_root);
    AppendHash(out, core.actions_root);
    AppendHash(out, core.result_root);
    AppendHash(out, core.state_root);
    AppendU64(out, core.effect_start);
    AppendU32(out, core.effect_count);
    AppendHash(out, core.effect_root);
    if (core.handoff) {
        AppendU64(out, core.handoff->next_epoch);
        AppendU64(out, core.handoff->next_anchor.height);
        AppendHash(out, core.handoff->next_anchor.block_hash);
        AppendHash(out, core.handoff->next_seat_set_hash);
    }
    return out;
}

inline std::optional<FlowMeshCheckpointCoreV1> DecodeFlowMeshCheckpointCoreV1(
    const std::span<const unsigned char> in)
{
    using namespace flowmesh_checkpoint_detail;
    if (in.size() != FLOWMESH_EXECUTION_CHECKPOINT_CORE_V1_SIZE &&
        in.size() != FLOWMESH_HANDOFF_CHECKPOINT_CORE_V1_SIZE) {
        return std::nullopt;
    }
    size_t cursor{0};
    FlowMeshCheckpointCoreV1 core;
    uint8_t kind{0};
    if (!ReadU16(in, cursor, core.version) || cursor >= in.size()) return std::nullopt;
    kind = in[cursor++];
    core.kind = static_cast<FlowMeshCheckpointKind>(kind);
    if (!ReadHash(in, cursor, core.domain) ||
        !ReadHash(in, cursor, core.market_id) ||
        !ReadU64(in, cursor, core.epoch) ||
        !ReadU64(in, cursor, core.sequence) ||
        !ReadHash(in, cursor, core.microblock_hash) ||
        !ReadHash(in, cursor, core.previous_checkpoint_id) ||
        !ReadU64(in, cursor, core.anchor.height) ||
        !ReadHash(in, cursor, core.anchor.block_hash) ||
        !ReadHash(in, cursor, core.seat_set_hash) ||
        !ReadU64(in, cursor, core.production_anchor.height) ||
        !ReadHash(in, cursor, core.production_anchor.block_hash) ||
        !ReadHash(in, cursor, core.parent_hash) ||
        !ReadHash(in, cursor, core.previous_state_root) ||
        !ReadHash(in, cursor, core.actions_root) ||
        !ReadHash(in, cursor, core.result_root) ||
        !ReadHash(in, cursor, core.state_root) ||
        !ReadU64(in, cursor, core.effect_start) ||
        !ReadU32(in, cursor, core.effect_count) ||
        !ReadHash(in, cursor, core.effect_root)) {
        return std::nullopt;
    }
    if (core.kind == FlowMeshCheckpointKind::EPOCH_HANDOFF) {
        FlowMeshCheckpointHandoffV1 handoff;
        if (!ReadU64(in, cursor, handoff.next_epoch) ||
            !ReadU64(in, cursor, handoff.next_anchor.height) ||
            !ReadHash(in, cursor, handoff.next_anchor.block_hash) ||
            !ReadHash(in, cursor, handoff.next_seat_set_hash)) {
            return std::nullopt;
        }
        core.handoff = handoff;
    }
    if (cursor != in.size() || !IsCanonicalFlowMeshCheckpointCoreV1(core)) {
        return std::nullopt;
    }
    return core;
}

inline std::optional<FlowMeshCheckpointId> FlowMeshCheckpointIdV1(
    const FlowMeshCheckpointCoreV1& core)
{
    const auto bytes{EncodeFlowMeshCheckpointCoreV1(core)};
    if (!bytes) return std::nullopt;
    HashWriter writer{TaggedHash(FLOWMESH_CHECKPOINT_ID_TAG)};
    writer << std::span<const unsigned char>{*bytes};
    return writer.GetSHA256();
}

struct FlowMeshCheckpointRecordV1 {
    FlowMeshCheckpointCoreV1 core;
    flowmesh::BlsMicroblockCertificate certificate;

    friend bool operator==(const FlowMeshCheckpointRecordV1& a,
                           const FlowMeshCheckpointRecordV1& b) = default;
};

inline constexpr size_t FLOWMESH_CHECKPOINT_RECORD_MIN_SIZE{
    FLOWMESH_EXECUTION_CHECKPOINT_CORE_V1_SIZE + 1 +
    flowmesh::FLOWMESH_BLS_SIGNATURE_SIZE};
inline constexpr size_t FLOWMESH_CHECKPOINT_RECORD_MAX_SIZE{
    FLOWMESH_HANDOFF_CHECKPOINT_CORE_V1_SIZE +
    flowmesh::FLOWMESH_MAX_SIGNER_BITMAP_BYTES +
    flowmesh::FLOWMESH_BLS_SIGNATURE_SIZE};
static_assert(FLOWMESH_CHECKPOINT_RECORD_MIN_SIZE == 560);
static_assert(FLOWMESH_CHECKPOINT_RECORD_MAX_SIZE == 1264);

inline bool FlowMeshCheckpointCertificateContextMatches(
    const FlowMeshCheckpointRecordV1& record)
{
    return record.certificate.seat_epoch == record.core.epoch &&
           record.certificate.sequence == record.core.sequence &&
           record.certificate.microblock_hash == record.core.microblock_hash;
}

//! Exact BLS signed context derived from a decoded checkpoint core.
inline flowmesh::BlsCertificateContext FlowMeshCheckpointBlsContextV1(
    const FlowMeshCheckpointCoreV1& core)
{
    return flowmesh::BlsCertificateContext{core.domain, core.market_id, core.epoch,
                                            core.seat_set_hash, core.sequence,
                                            core.microblock_hash};
}

inline std::optional<std::vector<unsigned char>> EncodeFlowMeshCheckpointRecordV1(
    const FlowMeshCheckpointRecordV1& record, const size_t seat_count)
{
    if (!FlowMeshCheckpointCertificateContextMatches(record) ||
        !flowmesh::IsWellFormedFlowMeshSignerBitmap(
            record.certificate.signer_bitmap, seat_count)) {
        return std::nullopt;
    }
    const auto core{EncodeFlowMeshCheckpointCoreV1(record.core)};
    if (!core) return std::nullopt;
    std::vector<unsigned char> out;
    out.reserve(core->size() + record.certificate.signer_bitmap.size() +
                flowmesh::FLOWMESH_BLS_SIGNATURE_SIZE);
    out.insert(out.end(), core->begin(), core->end());
    out.insert(out.end(), record.certificate.signer_bitmap.begin(),
               record.certificate.signer_bitmap.end());
    out.insert(out.end(), record.certificate.aggregate_signature.begin(),
               record.certificate.aggregate_signature.end());
    return out;
}

/**
 * Cheap structural decode used before the anchored seat snapshot is loaded.
 * It checks version/kind/core, exact exhaustion, and the global 1..625-byte
 * bitmap bound before allocating. Exact bitmap width/high bits are checked by
 * DecodeFlowMeshCheckpointRecordV1 once the active seat count is known.
 */
inline std::optional<FlowMeshCheckpointRecordV1>
DecodeFlowMeshCheckpointEnvelopeV1(const std::span<const unsigned char> in)
{
    if (in.size() < FLOWMESH_CHECKPOINT_RECORD_MIN_SIZE ||
        in.size() > FLOWMESH_CHECKPOINT_RECORD_MAX_SIZE || in.size() < 3) {
        return std::nullopt;
    }
    const auto kind{static_cast<FlowMeshCheckpointKind>(in[2])};
    const size_t core_size{
        kind == FlowMeshCheckpointKind::EXECUTION
            ? FLOWMESH_EXECUTION_CHECKPOINT_CORE_V1_SIZE
            : kind == FlowMeshCheckpointKind::EPOCH_HANDOFF
                  ? FLOWMESH_HANDOFF_CHECKPOINT_CORE_V1_SIZE
                  : 0};
    if (core_size == 0 || in.size() < core_size + 1 +
                                          flowmesh::FLOWMESH_BLS_SIGNATURE_SIZE) {
        return std::nullopt;
    }
    const size_t bitmap_size{
        in.size() - core_size - flowmesh::FLOWMESH_BLS_SIGNATURE_SIZE};
    if (bitmap_size == 0 ||
        bitmap_size > flowmesh::FLOWMESH_MAX_SIGNER_BITMAP_BYTES) {
        return std::nullopt;
    }
    const auto core{DecodeFlowMeshCheckpointCoreV1(in.first(core_size))};
    if (!core) return std::nullopt;

    FlowMeshCheckpointRecordV1 out;
    out.core = *core;
    out.certificate.seat_epoch = core->epoch;
    out.certificate.sequence = core->sequence;
    out.certificate.microblock_hash = core->microblock_hash;
    out.certificate.signer_bitmap.assign(
        in.begin() + core_size, in.begin() + core_size + bitmap_size);
    std::copy(in.begin() + core_size + bitmap_size, in.end(),
              out.certificate.aggregate_signature.begin());
    return out;
}

inline std::optional<FlowMeshCheckpointRecordV1> DecodeFlowMeshCheckpointRecordV1(
    const std::span<const unsigned char> in, const size_t seat_count)
{
    const auto out{DecodeFlowMeshCheckpointEnvelopeV1(in)};
    if (!out || !flowmesh::IsWellFormedFlowMeshSignerBitmap(
                    out->certificate.signer_bitmap, seat_count)) {
        return std::nullopt;
    }
    return out;
}

} // namespace modern

#endif // B3COIN_MODERN_FLOWMESH_CHECKPOINT_H
