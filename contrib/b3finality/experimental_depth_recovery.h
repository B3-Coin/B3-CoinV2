// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_CONTRIB_EXPERIMENTAL_DEPTH_RECOVERY_H
#define B3COIN_CONTRIB_EXPERIMENTAL_DEPTH_RECOVERY_H

#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace b3finality::experimental {

/** OFFLINE PROTOTYPE ONLY -- NOT A SAFE SIGNER RECOVERY RULE.
 *
 * Models the proposed "keep the maximum signed height, find an earlier signed
 * checkpoint on the active branch, wait 20 blocks" selection rule. It does
 * not authorize a signature, change a journal, or connect to production code.
 * Published V1 votes remain usable after a reorg: this selector can propose a
 * conflicting higher vote even when an old quorum certificate already exists.
 * The accompanying real-BLS counterexample deliberately demonstrates that risk.
 */
inline constexpr int PROTOTYPE_DEPTH{20};
inline constexpr size_t MAX_HISTORY_RECORDS{4096};

struct SignedCheckpoint {
    int height{-1};
    uint256 block_hash{};
    uint256 digest{};
    uint64_t epoch{0};
    uint256 signing_set_hash{};
    uint256 successor_set_hash{};

    friend bool operator==(const SignedCheckpoint&, const SignedCheckpoint&) = default;
};

struct FinalizedAnchor {
    int height{-1};
    uint256 block_hash{};
};

struct ProposalRequest {
    bool explicitly_enabled{false};
    // Caller-supplied evidence assertion, NOT a proof of completeness. The
    // current production journal stores only its last vote; it cannot supply
    // this history by guessing previous heights/hashes from the active chain.
    bool complete_signed_history{false};
    int max_ever_signed_height{-1};
    std::vector<SignedCheckpoint> signed_history; // strict ascending order
    int tip_height{-1};
    int checkpoint_start_height{-1};
    int checkpoint_interval{0};
    FinalizedAnchor finalized_anchor;
    SignedCheckpoint candidate;
};

enum class ProposalStatus {
    DISABLED,
    INCOMPLETE_HISTORY,
    MALFORMED_INPUT,
    CHAIN_DATA_UNAVAILABLE,
    FINALIZED_ANCHOR_MISMATCH,
    CANDIDATE_NOT_ABOVE_WATERMARK,
    CANDIDATE_NOT_ABOVE_FINALITY,
    CANDIDATE_TOO_SHALLOW,
    CANDIDATE_NOT_ON_ACTIVE_CHAIN,
    LATEST_VOTE_NOT_ORPHANED,
    NO_COMMON_SIGNED_ANCESTOR,
    EPOCH_OR_SET_MISMATCH,
    PROPOSAL_ONLY_NOT_AUTHORIZATION,
};

struct RecoveryProposal {
    static constexpr bool AUTHORIZES_SIGNING{false};
    SignedCheckpoint common_signed_ancestor;
    SignedCheckpoint candidate;
    int max_ever_signed_height{-1};
    // Preserve the orphaned votes too; none are erased, revoked or rewritten.
    std::vector<SignedCheckpoint> audit_records;
};

struct ProposalResult {
    ProposalStatus status{ProposalStatus::DISABLED};
    std::optional<RecoveryProposal> proposal;
};

/** All records must concern one validator/chain identity, established by the
 * caller. hash_at must read one coherent, immutable active-chain snapshot.
 * This pure selector neither authenticates caller evidence nor performs BLS,
 * block/epoch validation or fork choice. A proposal is deliberately NOT an
 * unlock proof. Missing evidence fails closed rather than inventing history.
 */
inline ProposalResult SelectDepthRecoveryProposal(
    const ProposalRequest& request,
    const std::function<std::optional<uint256>(int)>& hash_at)
{
    const auto refuse = [](const ProposalStatus status) {
        return ProposalResult{status, std::nullopt};
    };
    if (!request.explicitly_enabled) return refuse(ProposalStatus::DISABLED);
    if (!request.complete_signed_history || request.signed_history.size() < 2 ||
        request.signed_history.size() > MAX_HISTORY_RECORDS ||
        request.signed_history.back().height != request.max_ever_signed_height) {
        return refuse(ProposalStatus::INCOMPLETE_HISTORY);
    }
    if (!hash_at || request.tip_height < 0 || request.checkpoint_start_height < 0 ||
        request.checkpoint_interval <= 0 || request.finalized_anchor.height < 0 ||
        request.finalized_anchor.height > request.tip_height ||
        request.finalized_anchor.block_hash.IsNull()) {
        return refuse(ProposalStatus::MALFORMED_INPUT);
    }
    const auto well_formed = [&](const SignedCheckpoint& record) {
        return record.height >= request.checkpoint_start_height &&
               (record.height - request.checkpoint_start_height) % request.checkpoint_interval == 0 &&
               !record.block_hash.IsNull() && !record.digest.IsNull() &&
               !record.signing_set_hash.IsNull() && !record.successor_set_hash.IsNull();
    };
    int previous_height{-1};
    for (const auto& record : request.signed_history) {
        if (!well_formed(record) || record.height <= previous_height) {
            return refuse(ProposalStatus::MALFORMED_INPUT);
        }
        previous_height = record.height;
    }
    if (!well_formed(request.candidate)) return refuse(ProposalStatus::MALFORMED_INPUT);
    if (request.candidate.height <= request.max_ever_signed_height) {
        return refuse(ProposalStatus::CANDIDATE_NOT_ABOVE_WATERMARK);
    }
    if (request.candidate.height <= request.finalized_anchor.height) {
        return refuse(ProposalStatus::CANDIDATE_NOT_ABOVE_FINALITY);
    }
    if (request.candidate.height > request.tip_height ||
        request.tip_height - request.candidate.height < PROTOTYPE_DEPTH) {
        return refuse(ProposalStatus::CANDIDATE_TOO_SHALLOW);
    }
    const auto same_set = [](const SignedCheckpoint& a, const SignedCheckpoint& b) {
        return a.epoch == b.epoch && a.signing_set_hash == b.signing_set_hash &&
               a.successor_set_hash == b.successor_set_hash;
    };
    if (!same_set(request.candidate, request.signed_history.back())) {
        return refuse(ProposalStatus::EPOCH_OR_SET_MISMATCH);
    }
    const auto finalized_hash{hash_at(request.finalized_anchor.height)};
    if (!finalized_hash) return refuse(ProposalStatus::CHAIN_DATA_UNAVAILABLE);
    if (*finalized_hash != request.finalized_anchor.block_hash) {
        return refuse(ProposalStatus::FINALIZED_ANCHOR_MISMATCH);
    }
    const auto candidate_hash{hash_at(request.candidate.height)};
    if (!candidate_hash) return refuse(ProposalStatus::CHAIN_DATA_UNAVAILABLE);
    if (*candidate_hash != request.candidate.block_hash) {
        return refuse(ProposalStatus::CANDIDATE_NOT_ON_ACTIVE_CHAIN);
    }
    for (auto it{request.signed_history.rbegin()}; it != request.signed_history.rend(); ++it) {
        const auto active_hash{hash_at(it->height)};
        if (!active_hash) return refuse(ProposalStatus::CHAIN_DATA_UNAVAILABLE);
        if (*active_hash != it->block_hash) continue;
        if (it == request.signed_history.rbegin()) {
            return refuse(ProposalStatus::LATEST_VOTE_NOT_ORPHANED);
        }
        if (!same_set(*it, request.candidate)) return refuse(ProposalStatus::EPOCH_OR_SET_MISMATCH);
        return {ProposalStatus::PROPOSAL_ONLY_NOT_AUTHORIZATION,
                RecoveryProposal{*it, request.candidate, request.max_ever_signed_height,
                                 request.signed_history}};
    }
    return refuse(ProposalStatus::NO_COMMON_SIGNED_ANCESTOR);
}

} // namespace b3finality::experimental

#endif // B3COIN_CONTRIB_EXPERIMENTAL_DEPTH_RECOVERY_H
