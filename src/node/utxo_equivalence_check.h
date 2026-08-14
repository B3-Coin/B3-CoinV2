// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_UTXO_EQUIVALENCE_CHECK_H
#define BITCOIN_NODE_UTXO_EQUIVALENCE_CHECK_H

#include <consensus/params.h>
#include <node/utxo_commitment.h>
#include <primitives/block.h>
#include <txdb.h>
#include <uint256.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace node {

/**
 * Operator-facing diagnostic: prove that a trusted replay of the legacy chain
 * from genesis through the final legacy block (H, X) reconstructs exactly the
 * UTXO set of a fully-validated chainstate stopped at that same block. This is
 * the contract's U == U' check. It is entirely outside consensus and outside
 * node startup: nothing in validation or init references it.
 */
struct ReplayEquivalenceOptions {
    int final_height{-1};          //!< H: the height under verification
    uint256 final_hash{};          //!< X: the exact block hash at H
    size_t max_mismatch_sample{20}; //!< bound on retained per-outpoint diagnostics
};

struct ReplayEquivalenceResult {
    //! True iff every verification step passed and the two sets are identical.
    bool ok{false};
    //! Human-readable failures (H/X mismatch, replay failure, missing blocks).
    std::vector<std::string> errors;
    size_t live_count{0};
    size_t replay_count{0};
    uint256 live_commitment{};
    uint256 replay_commitment{};
    //! Total number of differing or one-sided outpoints.
    size_t mismatch_total{0};
    //! At most max_mismatch_sample of them, in canonical order.
    std::vector<UtxoMismatch> mismatch_sample;
    int blocks_replayed{0};
};

//! Reduce a full comparison to counts, commitments and a bounded sample.
ReplayEquivalenceResult SummarizeComparison(const UtxoComparison& cmp, size_t max_sample);

/**
 * Run the whole check. `live_view` is the fully-validated chainstate; its best
 * block must be exactly X (the node was stopped at H). `block_source` must
 * return the block at each height 0..H of the X-anchored chain; the trusted
 * replay engine independently re-verifies linkage, Merkle roots and that the
 * block at H hashes to X, so a wrong or tampered source fails rather than
 * producing a bogus comparison. `replay_scratch` is a fresh, disposable coins
 * database the reconstruction is flushed into (never the node's own).
 */
ReplayEquivalenceResult VerifyReplayEquivalence(
    const Consensus::Params& params,
    const CCoinsView& live_view,
    const std::function<std::optional<CBlock>(int)>& block_source,
    CCoinsViewDB& replay_scratch,
    const ReplayEquivalenceOptions& opts);

} // namespace node

#endif // BITCOIN_NODE_UTXO_EQUIVALENCE_CHECK_H
