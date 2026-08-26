// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_MODERN_FINALITY_SCHEDULE_H
#define B3COIN_MODERN_FINALITY_SCHEDULE_H

#include <consensus/modern_pos_params.h>
#include <modern/finality_certificate.h>
#include <modern/finality_types.h>
#include <uint256.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace modern {

/**
 * Checkpoint schedule, depth and epoch-window rules for FINALITY_CERTIFICATE
 * inclusion (plan Commit 11; normative b3-cross-chain-finality-v1.md section 4
 * "Checkpoints", "Signing", "Certificate carrier", "Validation order"; frozen
 * constants CHECKPOINT_INTERVAL = 10, CHECKPOINT_DEPTH = 12, window
 * {current, current - 1}).
 *
 * Everything here is a PURE function of (certificate, the block including it,
 * the epoch state derived from the chain below it, consensus params):
 *
 *   checkpoint   (h_c - M) mod CHECKPOINT_INTERVAL == 0, h_c >= M
 *   depth        h_b - h_c >= CHECKPOINT_DEPTH   (h_b = height of the including block)
 *   ancestry     FinalizedBlock.block_hash == hash of the block at h_c on THIS chain
 *   withdrawal   FinalizedBlock.withdrawal_root == 0 before bridge activation (A3 not in V1)
 *   window       FinalizedBlock.epoch in {current, current - 1}
 *   relation     epoch(h_c) == FinalizedBlock.epoch   (epoch in force at the checkpoint)
 *   monotone     FinalizedBlock.height > highest certified height on this chain
 *   successor    FinalizedBlock.validator_set_hash == hash(Set_{epoch + 1}) as derived here
 *   signing set  Set_{FinalizedBlock.epoch} (never any other), then bitmap -> quorum -> BLS
 *
 * An old certificate can therefore never replace or regress newer finalized
 * state, a certificate can only finalize the exact checkpoint object the
 * schedule permits, and no check depends on anything but chainstate and
 * block data. The epoch state itself (which Set_e is current, epoch starts,
 * the extension/handover machinery) is derived by node::FinalityTracker
 * (Commit 12); this module only consumes a view of it.
 */

//! Epoch of a height given the table of epoch starts (epoch_starts[e] =
//! first height of epoch e, epoch_starts[0] = M); nullopt below M.
inline std::optional<uint64_t> EpochOfHeight(const std::vector<int>& epoch_starts, const int height)
{
    if (epoch_starts.empty() || height < epoch_starts.front()) return std::nullopt;
    uint64_t e{0};
    for (size_t i = 1; i < epoch_starts.size(); ++i) {
        if (height >= epoch_starts[i]) e = i; else break;
    }
    return e;
}

//! Checkpoint heights: (h - M) mod CHECKPOINT_INTERVAL == 0 with h >= M.
inline bool IsCheckpointHeight(const int height, const int modern_start, const int interval)
{
    return interval > 0 && height >= modern_start && (height - modern_start) % interval == 0;
}

//! A certificate for checkpoint h_c may be included at h_b only once h_b - h_c >= depth.
inline bool CheckpointDepthSatisfied(const int checkpoint_height, const int including_height, const int depth)
{
    return including_height >= checkpoint_height && including_height - checkpoint_height >= depth;
}

/**
 * The verifier's view of the epoch state derived from the chain BELOW the
 * including block (after the gated-rotation rule has been applied for the
 * including height). Sets are views (keys PoP-verified at binding time).
 */
struct FinalityEpochView {
    uint64_t current_epoch{0};
    //! epoch_starts[e] for every e <= current_epoch; [0] == M.
    std::vector<int> epoch_starts;
    bool lineage_broken{false};
    //! Highest certified (finalized) height on this chain, if any.
    std::optional<uint64_t> finalized_height;
    //! Set_current and hash(Set_{current+1}) (known for the whole epoch).
    const ValidatorSetView* current_set{nullptr};
    uint256 current_set_hash{};
    uint256 next_set_hash{};
    //! Set_{current-1}, present once current_epoch >= 1.
    const ValidatorSetView* previous_set{nullptr};
};

enum class CertificatePlacement {
    OK,
    NO_FINALITY_SET,         //!< no Set_0 exists (bootstrap floor not met) -- nothing can be certified
    LINEAGE_BROKEN,          //!< epoch extended beyond MAX_EPOCH_EXTENSION: no certificate is valid
    NOT_CHECKPOINT,          //!< FinalizedBlock.height is not on the checkpoint schedule
    INSUFFICIENT_DEPTH,      //!< including height - checkpoint height < CHECKPOINT_DEPTH
    EPOCH_WINDOW,            //!< FinalizedBlock.epoch not in {current, current-1}
    EPOCH_RELATION,          //!< epoch(checkpoint height) != FinalizedBlock.epoch
    WRONG_BLOCK_HASH,        //!< checkpoint is not the block at that height on this chain
    WITHDRAWAL_ROOT_NONZERO, //!< bridge not active: withdrawal_root must be all-zero
    FINALITY_REGRESSION,     //!< height not strictly above the highest certified height
};

inline const char* CertificatePlacementName(const CertificatePlacement p)
{
    switch (p) {
    case CertificatePlacement::OK: return "ok";
    case CertificatePlacement::NO_FINALITY_SET: return "no-finality-set";
    case CertificatePlacement::LINEAGE_BROKEN: return "lineage-broken";
    case CertificatePlacement::NOT_CHECKPOINT: return "not-checkpoint";
    case CertificatePlacement::INSUFFICIENT_DEPTH: return "insufficient-depth";
    case CertificatePlacement::EPOCH_WINDOW: return "epoch-window";
    case CertificatePlacement::EPOCH_RELATION: return "epoch-relation";
    case CertificatePlacement::WRONG_BLOCK_HASH: return "wrong-block-hash";
    case CertificatePlacement::WITHDRAWAL_ROOT_NONZERO: return "withdrawal-root-nonzero";
    case CertificatePlacement::FINALITY_REGRESSION: return "finality-regression";
    }
    return "unknown";
}

/**
 * Schedule / depth / window / relation / ancestry / monotone checks (the cheap
 * deterministic part, before any cryptography). `hash_at(h)` returns the hash
 * of the block at height h on the chain the including block extends (nullopt
 * when unavailable).
 */
inline CertificatePlacement CheckCertificatePlacement(const FinalizedBlock& fb, const int including_height,
                                                      const FinalityEpochView& view,
                                                      const Consensus::ModernPosParams& pos,
                                                      const std::function<std::optional<uint256>(int)>& hash_at)
{
    if (view.epoch_starts.empty() || view.current_set == nullptr || view.current_set->validator_count == 0) {
        return CertificatePlacement::NO_FINALITY_SET;
    }
    if (view.lineage_broken) return CertificatePlacement::LINEAGE_BROKEN;
    if (fb.height > static_cast<uint64_t>(including_height)) return CertificatePlacement::INSUFFICIENT_DEPTH;
    const int hc{static_cast<int>(fb.height)};
    const int modern_start{view.epoch_starts.front()};
    if (!IsCheckpointHeight(hc, modern_start, pos.checkpoint_interval)) return CertificatePlacement::NOT_CHECKPOINT;
    if (!CheckpointDepthSatisfied(hc, including_height, pos.checkpoint_depth)) return CertificatePlacement::INSUFFICIENT_DEPTH;
    // Window {current, current-1}: an older set may still attest (delayed
    // certificate), nothing newer or older than that.
    if (!(fb.epoch == view.current_epoch || (view.current_epoch >= 1 && fb.epoch == view.current_epoch - 1))) {
        return CertificatePlacement::EPOCH_WINDOW;
    }
    if (fb.epoch == view.current_epoch - 1 && view.current_epoch >= 1 && !view.previous_set) {
        return CertificatePlacement::EPOCH_WINDOW;
    }
    const auto epoch_of_hc{EpochOfHeight(view.epoch_starts, hc)};
    if (!epoch_of_hc || *epoch_of_hc != fb.epoch) return CertificatePlacement::EPOCH_RELATION;
    const auto expected_hash{hash_at(hc)};
    if (!expected_hash || *expected_hash != fb.block_hash) return CertificatePlacement::WRONG_BLOCK_HASH;
    if (!fb.withdrawal_root.IsNull()) return CertificatePlacement::WITHDRAWAL_ROOT_NONZERO;
    if (view.finalized_height && fb.height <= *view.finalized_height) return CertificatePlacement::FINALITY_REGRESSION;
    return CertificatePlacement::OK;
}

/**
 * Full judgement of a certificate included at `including_height`: placement
 * rules first, then VerifyFinalityCertificate against Set_{fb.epoch} with the
 * successor hash hash(Set_{fb.epoch+1}) as derived on this chain. Returns
 * false with a stable reason string on any failure.
 */
inline bool JudgeFinalityCertificate(const uint256& chain_domain, const FinalizedBlock& fb,
                                     const FinalityCertificate& cert, const int including_height,
                                     const FinalityEpochView& view, const Consensus::ModernPosParams& pos,
                                     const std::function<std::optional<uint256>(int)>& hash_at,
                                     std::string& error)
{
    const auto placement{CheckCertificatePlacement(fb, including_height, view, pos, hash_at)};
    if (placement != CertificatePlacement::OK) {
        error = CertificatePlacementName(placement);
        return false;
    }
    // Set_{fb.epoch} signs; it attests hash(Set_{fb.epoch+1}).
    const bool delayed{fb.epoch != view.current_epoch};
    const ValidatorSetView& signing_set{delayed ? *view.previous_set : *view.current_set};
    const uint256& successor{delayed ? view.current_set_hash : view.next_set_hash};
    const auto check{VerifyFinalityCertificate(chain_domain, fb, cert, signing_set, successor)};
    if (check != CertificateCheck::OK) {
        error = CertificateCheckName(check);
        return false;
    }
    return true;
}

/**
 * Epoch-aware structural matcher: the signer-bitmap width depends on the size
 * of Set_{FinalizedBlock.epoch}, so the FinalizedBlock prefix is decoded first
 * and `set_size_for_epoch(epoch)` resolves n (nullopt => that epoch has no set
 * on this chain => malformed). Same structural rules as MatchFinalityCertificate.
 */
inline bool MatchFinalityCertificateForEpoch(const CTransaction& coinbase,
                                             const std::function<std::optional<uint32_t>(uint64_t)>& set_size_for_epoch,
                                             std::optional<FinalityCertificatePair>& out, std::string& error)
{
    out.reset();
    for (size_t i = 0; i < coinbase.mpa.size(); ++i) {
        const auto& rec{coinbase.mpa[i]};
        if (rec.payload_type != MPA_TYPE_FINALITY_CERTIFICATE || rec.payload_version != MPA_VERSION_V1) continue;
        if (rec.payload.size() < FinalizedBlock::SIZE) { error = "finality-cert-malformed-payload"; return false; }
        const auto fb{FinalizedBlock::Decode(std::span<const unsigned char>(rec.payload).first(FinalizedBlock::SIZE))};
        if (!fb) { error = "finality-cert-malformed-payload"; return false; }
        const auto n{set_size_for_epoch(fb->epoch)};
        if (!n) { error = "finality-cert-unknown-epoch-set"; return false; }
        return MatchFinalityCertificate(coinbase, *n, out, error);
    }
    // No record: MatchFinalityCertificate reports orphan cells / absence.
    return MatchFinalityCertificate(coinbase, /*n=*/1, out, error);
}

} // namespace modern

#endif // B3COIN_MODERN_FINALITY_SCHEDULE_H
