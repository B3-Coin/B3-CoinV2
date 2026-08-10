// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_LEGACY_REPLAY_H
#define B3COIN_LEGACY_REPLAY_H

#include <consensus/params.h>
#include <uint256.h>

#include <cstddef>
#include <map>
#include <span>
#include <string>

class CBlock;
class CCoinsViewCache;

namespace legacy {

/**
 * Trusted mechanical replay of the legacy era (heights <= the finalized
 * legacy boundary H). A distinct engine, not a set of bypass flags inside
 * the modern ConnectBlock path.
 *
 * Once the boundary (H, X) is pinned, the boundary hash attests the entire
 * legacy prefix, so replay only performs mechanical processing:
 *
 *  - safe legacy block/transaction decoding (ApplyRawBlock);
 *  - previous-block hash linkage, anchored on the configured genesis;
 *  - configured checkpoint hashes;
 *  - transaction Merkle roots, including duplicate-transaction mutation;
 *  - referenced UTXO existence, duplicate-spend detection, exact input
 *    removal and exact output creation;
 *  - overflow-safe value accounting;
 *  - preservation of historical txid, vout index, amount, scriptPubKey,
 *    creation height, coinbase/coinstake classification, transaction time
 *    and in-block offset (maturity- and kernel-relevant metadata).
 *
 * It deliberately skips historical script/signature validation, PoW, the
 * PoS kernel, stake modifiers, rewards, difficulty, timestamps/MTP and
 * chainwork fork choice. Malformed or internally inconsistent data is
 * still rejected. There is no UTXO snapshot and no hard-coded UTXO root:
 * the set is reconstructed exclusively by applying blocks.
 *
 * Blocks apply strictly in height order and atomically: a failed block
 * leaves the target view and the replay position untouched, which makes
 * crash-safe forward resumption (ResumeAt) straightforward for a caller
 * that persists the view.
 */
class TrustedReplay
{
public:
    //! `final_height` is H, the last legacy height; blocks above it are
    //! refused. `checkpoints` maps heights to required legacy block hashes.
    TrustedReplay(const Consensus::Params& params, int final_height,
                  std::map<int, uint256> checkpoints);

    //! Resume forward replay: `next_height` is the height to be applied
    //! next and `tip_hash` the hash of its parent.
    void ResumeAt(int next_height, const uint256& tip_hash);

    //! Mechanically apply the next block in height order to `view`.
    //! All-or-nothing: on failure `error` is set and neither the view nor
    //! the replay position changes.
    bool ApplyBlock(const CBlock& block, CCoinsViewCache& view, std::string& error);

    //! Safely decode a raw legacy-encoded block, then apply it.
    bool ApplyRawBlock(std::span<const std::byte> raw, CCoinsViewCache& view, std::string& error);

    int NextHeight() const { return m_next_height; }
    const uint256& TipHash() const { return m_tip_hash; }

private:
    const Consensus::Params& m_params;
    const int m_final_height;
    const std::map<int, uint256> m_checkpoints;
    int m_next_height{0};
    uint256 m_tip_hash{};
};

} // namespace legacy

#endif // B3COIN_LEGACY_REPLAY_H
