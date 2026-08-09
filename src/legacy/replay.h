// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_LEGACY_REPLAY_H
#define B3COIN_LEGACY_REPLAY_H

#include <uint256.h>

#include <optional>

class CBlock;
class CBlockHeader;
class CCoinsViewCache;

namespace legacy {

/**
 * PROTOTYPE-ONLY SKELETON. Trusted mechanical replay of the legacy era
 * (heights <= LEGACY_FINAL_HEIGHT).
 *
 * Once the boundary (H, X) is pinned, historical blocks no longer need rule
 * validation: the boundary hash X attests the entire prefix. Replay then
 * only has to
 *  - check hash linkage, checkpoints, and Merkle/data integrity,
 *  - mechanically reconstruct the UTXO set (txids, outpoints, values and
 *    scriptPubKeys preserved exactly),
 *  - support crash-safe forward resumption,
 * while skipping old PoW, the PoS kernel, stake modifiers, rewards,
 * difficulty, timestamps, chainwork, and script/signature validation.
 *
 * The current tree instead fully validates legacy blocks through the normal
 * ConnectBlock path (see the use_legacy_b3coin branches in validation.cpp);
 * that path migrates here once (H, X) is configured.
 */
class TrustedReplay
{
public:
    virtual ~TrustedReplay() = default;

    /** Check that `header` links to the expected previous block hash. */
    virtual bool VerifyLinkage(const CBlockHeader& header, const uint256& expected_prev) const = 0;

    /** Check Merkle root and structural data integrity of a legacy block. */
    virtual bool VerifyDataIntegrity(const CBlock& block) const = 0;

    /**
     * Apply a linkage- and integrity-checked legacy block to the UTXO set,
     * with no rule validation. Must be idempotent up to the crash-safe
     * resume point.
     */
    virtual bool ApplyBlock(const CBlock& block, int height, CCoinsViewCache& view) = 0;

    /**
     * Highest height whose effects are durably applied; replay resumes at
     * the following height after a crash. std::nullopt before any block is
     * applied.
     */
    virtual std::optional<int> LastReplayedHeight() const = 0;
};

} // namespace legacy

#endif // B3COIN_LEGACY_REPLAY_H
