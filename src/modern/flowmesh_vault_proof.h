// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_FLOWMESH_VAULT_PROOF_H
#define B3COIN_MODERN_FLOWMESH_VAULT_PROOF_H

#include <modern/flowmesh_checkpoint.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace modern {

/**
 * Type-9 authorizes exactly one on-chain vault operation from exactly one
 * checkpointed typed effect. It is evidence only: connected-checkpoint,
 * nullifier, input/output and fee-conservation checks belong to chain
 * validation and are deliberately outside this codec.
 */
enum class FlowMeshVaultProofKind : uint8_t {
    DEPOSIT_SWEEP = 1,
    WITHDRAWAL = 2,
};

struct FlowMeshVaultProofV1 {
    FlowMeshVaultProofKind kind{FlowMeshVaultProofKind::DEPOSIT_SWEEP};
    FlowMeshCheckpointId checkpoint_id;
    FlowMeshEffectV1 effect;
    uint32_t leaf_index{0};
    std::vector<uint256> branch;

    friend bool operator==(const FlowMeshVaultProofV1& a,
                           const FlowMeshVaultProofV1& b) = default;
};

//! Consensus MPA cap. The exact v1 grammar below is tighter (645 bytes max),
//! but the 4-KiB outer cap is frozen so future versions cannot silently grow.
inline constexpr size_t FLOWMESH_VAULT_PROOF_RECORD_MAX_SIZE{4096};
inline constexpr size_t FLOWMESH_VAULT_PROOF_PREFIX_SIZE{1 + 32};
inline constexpr size_t FLOWMESH_VAULT_PROOF_SUFFIX_SIZE{4 + 1};
inline constexpr size_t FLOWMESH_DEPOSIT_SWEEP_PROOF_MIN_SIZE{
    FLOWMESH_VAULT_PROOF_PREFIX_SIZE + FLOWMESH_DEPOSIT_ACCEPTANCE_V1_SIZE +
    FLOWMESH_VAULT_PROOF_SUFFIX_SIZE};
inline constexpr size_t FLOWMESH_WITHDRAWAL_PROOF_MIN_SIZE{
    FLOWMESH_VAULT_PROOF_PREFIX_SIZE + FLOWMESH_WITHDRAWAL_RECEIPT_V1_SIZE +
    FLOWMESH_VAULT_PROOF_SUFFIX_SIZE};
inline constexpr size_t FLOWMESH_VAULT_PROOF_V1_WIRE_MIN_SIZE{
    FLOWMESH_WITHDRAWAL_PROOF_MIN_SIZE};
inline constexpr size_t FLOWMESH_VAULT_PROOF_V1_WIRE_MAX_SIZE{
    FLOWMESH_DEPOSIT_SWEEP_PROOF_MIN_SIZE +
    32 * FLOWMESH_MAX_EFFECT_BRANCH_DEPTH};
static_assert(FLOWMESH_DEPOSIT_SWEEP_PROOF_MIN_SIZE == 261);
static_assert(FLOWMESH_WITHDRAWAL_PROOF_MIN_SIZE == 257);
static_assert(FLOWMESH_VAULT_PROOF_V1_WIRE_MIN_SIZE == 257);
static_assert(FLOWMESH_VAULT_PROOF_V1_WIRE_MAX_SIZE == 645);
static_assert(FLOWMESH_VAULT_PROOF_V1_WIRE_MAX_SIZE <=
              FLOWMESH_VAULT_PROOF_RECORD_MAX_SIZE);

inline bool FlowMeshVaultProofEffectKindMatches(const FlowMeshVaultProofV1& proof)
{
    if (proof.kind == FlowMeshVaultProofKind::DEPOSIT_SWEEP) {
        return std::holds_alternative<FlowMeshDepositAcceptanceV1>(proof.effect);
    }
    if (proof.kind == FlowMeshVaultProofKind::WITHDRAWAL) {
        return std::holds_alternative<FlowMeshWithdrawalReceiptV1>(proof.effect);
    }
    return false;
}

/**
 * Exact wire:
 *   proof_kind[1] || checkpoint_id[32] || canonical_typed_effect[fixed by kind]
 *   || leaf_index[4] || branch_count[1] || branch[branch_count][32]
 *
 * No effect count or vector occurs: one record is one effect proof. The
 * checkpoint supplies effect_start/count and therefore fixes exact branch
 * depth during verification.
 */
inline std::optional<std::vector<unsigned char>> EncodeFlowMeshVaultProofV1(
    const FlowMeshVaultProofV1& proof)
{
    if (!FlowMeshVaultProofEffectKindMatches(proof) || proof.checkpoint_id.IsNull() ||
        proof.leaf_index >= FLOWMESH_MAX_CHECKPOINT_EFFECTS ||
        proof.branch.size() > FLOWMESH_MAX_EFFECT_BRANCH_DEPTH) {
        return std::nullopt;
    }
    const auto effect{EncodeFlowMeshEffectV1(proof.effect)};
    if (!effect) return std::nullopt;
    const size_t total{FLOWMESH_VAULT_PROOF_PREFIX_SIZE + effect->size() +
                       FLOWMESH_VAULT_PROOF_SUFFIX_SIZE + proof.branch.size() * 32};
    if (total > FLOWMESH_VAULT_PROOF_RECORD_MAX_SIZE ||
        total > FLOWMESH_VAULT_PROOF_V1_WIRE_MAX_SIZE) {
        return std::nullopt;
    }
    std::vector<unsigned char> out;
    out.reserve(total);
    out.push_back(static_cast<uint8_t>(proof.kind));
    flowmesh_checkpoint_detail::AppendHash(out, proof.checkpoint_id);
    out.insert(out.end(), effect->begin(), effect->end());
    flowmesh_checkpoint_detail::AppendU32(out, proof.leaf_index);
    out.push_back(static_cast<uint8_t>(proof.branch.size()));
    for (const uint256& sibling : proof.branch) {
        flowmesh_checkpoint_detail::AppendHash(out, sibling);
    }
    return out;
}

inline std::optional<FlowMeshVaultProofV1> DecodeFlowMeshVaultProofV1(
    const std::span<const unsigned char> in)
{
    // The outer bound and the one-byte kind are checked before selecting any
    // size or allocating the branch.
    if (in.size() < FLOWMESH_VAULT_PROOF_V1_WIRE_MIN_SIZE ||
        in.size() > FLOWMESH_VAULT_PROOF_RECORD_MAX_SIZE || in.empty()) {
        return std::nullopt;
    }
    const auto kind{static_cast<FlowMeshVaultProofKind>(in[0])};
    const size_t effect_size{
        kind == FlowMeshVaultProofKind::DEPOSIT_SWEEP
            ? FLOWMESH_DEPOSIT_ACCEPTANCE_V1_SIZE
            : kind == FlowMeshVaultProofKind::WITHDRAWAL
                  ? FLOWMESH_WITHDRAWAL_RECEIPT_V1_SIZE
                  : 0};
    if (effect_size == 0) return std::nullopt;
    const size_t branch_count_offset{
        FLOWMESH_VAULT_PROOF_PREFIX_SIZE + effect_size + 4};
    if (branch_count_offset >= in.size()) return std::nullopt;
    const size_t branch_count{in[branch_count_offset]};
    if (branch_count > FLOWMESH_MAX_EFFECT_BRANCH_DEPTH) return std::nullopt;
    const size_t expected{branch_count_offset + 1 + branch_count * 32};
    if (expected != in.size() || expected > FLOWMESH_VAULT_PROOF_V1_WIRE_MAX_SIZE) {
        return std::nullopt;
    }

    FlowMeshVaultProofV1 out;
    out.kind = kind;
    size_t cursor{1};
    if (!flowmesh_checkpoint_detail::ReadHash(in, cursor, out.checkpoint_id)) {
        return std::nullopt;
    }
    const auto effect{DecodeFlowMeshEffectV1(in.subspan(cursor, effect_size))};
    if (!effect) return std::nullopt;
    out.effect = *effect;
    cursor += effect_size;
    if (!flowmesh_checkpoint_detail::ReadU32(in, cursor, out.leaf_index) ||
        cursor >= in.size() || in[cursor++] != branch_count ||
        out.checkpoint_id.IsNull() ||
        out.leaf_index >= FLOWMESH_MAX_CHECKPOINT_EFFECTS ||
        !FlowMeshVaultProofEffectKindMatches(out)) {
        return std::nullopt;
    }
    // The count and exact remaining length were bounded above; only now is
    // storage reserved.
    out.branch.reserve(branch_count);
    for (size_t i{0}; i < branch_count; ++i) {
        uint256 sibling;
        if (!flowmesh_checkpoint_detail::ReadHash(in, cursor, sibling)) {
            return std::nullopt;
        }
        out.branch.push_back(sibling);
    }
    if (cursor != in.size()) return std::nullopt;
    return out;
}

inline bool VerifyFlowMeshVaultProofV1(
    const FlowMeshVaultProofV1& proof, const FlowMeshCheckpointCoreV1& checkpoint)
{
    if (!FlowMeshVaultProofEffectKindMatches(proof) ||
        checkpoint.kind != FlowMeshCheckpointKind::EXECUTION ||
        checkpoint.effect_count == 0 ||
        checkpoint.effect_count > FLOWMESH_MAX_CHECKPOINT_EFFECTS ||
        proof.leaf_index >= checkpoint.effect_count ||
        proof.branch.size() != FlowMeshEffectTreeDepth(checkpoint.effect_count) ||
        checkpoint.effect_start >
            std::numeric_limits<uint64_t>::max() - proof.leaf_index ||
        !IsCanonicalFlowMeshCheckpointCoreV1(checkpoint)) {
        return false;
    }
    const auto checkpoint_id{FlowMeshCheckpointIdV1(checkpoint)};
    if (!checkpoint_id || proof.checkpoint_id != *checkpoint_id) return false;
    const auto expected_vault{
        flowmesh::ComputeFlowMeshVaultId(checkpoint.domain, checkpoint.market_id)};
    if (!expected_vault) return false;

    bool context_matches{false};
    if (const auto* deposit{std::get_if<FlowMeshDepositAcceptanceV1>(&proof.effect)}) {
        context_matches = deposit->market_id == checkpoint.market_id &&
                          deposit->epoch == checkpoint.epoch &&
                          deposit->sequence <= checkpoint.sequence &&
                          deposit->vault_id == *expected_vault;
    } else {
        const auto& receipt{std::get<FlowMeshWithdrawalReceiptV1>(proof.effect)};
        context_matches = receipt.market_id == checkpoint.market_id &&
                          receipt.epoch == checkpoint.epoch &&
                          receipt.sequence <= checkpoint.sequence &&
                          receipt.vault_id == *expected_vault;
    }
    return context_matches && VerifyFlowMeshEffectInclusion(
                                  checkpoint.effect_start, checkpoint.effect_count,
                                  checkpoint.effect_root, proof.effect,
                                  proof.leaf_index, proof.branch);
}

} // namespace modern

#endif // B3COIN_MODERN_FLOWMESH_VAULT_PROOF_H
